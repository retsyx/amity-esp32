"""Admit one ten-key keyboard report in the real CEC FIFO and verify its held streams."""

import re
import time

from .ble_input import BLEInputEvidence
from .cec import Frame
from .keys import Key
from .tx_policy import fresh_log_delta


_USAGES = (0x28, 0x29, 0x4A, 0x4F, 0x50, 0x51, 0x52, 0x7F, 0x80, 0x81)
_HELD_USAGES = tuple(usage for usage in _USAGES if usage not in (0x51, 0x52))
_KEYS = (0x00, 0x0D, 0x09, 0x04, 0x03, 0x02, 0x01)
_CONSUMER_KEYS = (Key.MUTE, Key.VOLUME_UP, Key.VOLUME_DOWN)
_DROP = re.compile(r"TX dropped: FIFO full \(GPIO (\d+), (\d+) bytes: ([0-9a-fA-F ]+)\)")


def _fifo_drops(text):
    rows = []
    for match in _DROP.finditer(text):
        raw = bytes.fromhex(match[3])
        if len(raw) != int(match[2]) or not 1 <= len(raw) <= 16:
            raise AssertionError("A FIFO drop log has an invalid frame length")
        rows.append((int(match[1]), raw))
    if len(rows) != text.count("cec: TX dropped"):
        raise AssertionError("A CEC drop log has an unrecognized format")
    return rows


class BLEFIFOEvidence(BLEInputEvidence):
    """All ten presses of one report admitted in order, with no drop in shipping UI logs."""

    def __init__(self, dut, scan, *components):
        super().__init__(dut, scan, *components)
        self._wire_frames, self._wire_seen = [], set()
        self._fifo_gpio = None

    def _observe(self, component, frame, transport):
        super()._observe(component, frame, transport)
        if (transport != "wire" or component.gpio != self._fifo_gpio
                or frame.source != self._initiators.get(component.gpio)):
            return
        with self._condition:
            if type(frame.timestamp_ns) is not int or frame.timestamp_ns <= 0:
                self._invalid_clock = True
            elif frame.timestamp_ns >= self._started:
                identity = component.gpio, frame.timestamp_ns, frame.raw
                if identity not in self._wire_seen:
                    if len(self._wire_frames) >= 4096:
                        raise AssertionError("FIFO physical observation exceeded its frame bound")
                    self._wire_seen.add(identity)
                    self._wire_frames.append((component.gpio, frame))
            self._condition.notify_all()

    def _wire_snapshot(self):
        self._snapshot()  # Preserve the shared session and physical-clock checks.
        with self._condition:
            return tuple(self._wire_frames)

    def assert_ten_key_admission(self, keyboard, *, source, volume):
        if (source not in self.components or volume not in self.components or source is volume
                or source.dut is not self.dut or volume.dut is not self.dut
                or source.gpio != volume.gpio):
            raise ValueError("Logged FIFO admission needs distinct local recipients on one CEC wire")
        self._fifo_gpio = source.gpio
        record = self._begin(keyboard, "fifo_admission")
        before = self.dut.capture_logs(expected_boot=self.scan._boot).assert_same_boot()
        initiator = self._initiators[source.gpio]
        expected = [(source, Frame(initiator, source.address.logical, 0x44, bytes([key]))) for key in _KEYS]
        expected += [(volume, Frame(initiator, volume.address.logical, 0x44, bytes([key]))) for key in _CONSUMER_KEYS]
        primary = None
        try:
            with keyboard.reports() as reports:
                # One ten-key keyboard notification submits seven source
                # presses, then Mute and Volume Up/Down. Separate BLE reports
                # can cross a completed CEC frame and cannot prove that the
                # FIFO admitted all ten together.
                started, deadline = self._change(
                    lambda: reports.report(keyboard=_USAGES, consumer=0), record, "ten_down")

                def prefix(count, deadline_ns):
                    def find(_):
                        frames = [(gpio, frame) for gpio, frame in self._wire_snapshot()
                                  if gpio == source.gpio and frame.timestamp_ns > started]
                        if len(frames) < count:
                            return None
                        observed = frames[:count]
                        if ([(gpio, frame.raw) for gpio, frame in observed]
                                != [(component.gpio, frame.raw) for component, frame in expected[:count]]):
                            raise AssertionError("The CEC FIFO did not transmit all ten admitted presses in order")
                        if (observed[-1][1].timestamp_ns > deadline_ns
                                or not all(self._handled(component, frame)
                                           for (component, _), (_, frame) in zip(expected, observed))):
                            return None
                        return tuple(frame for _, frame in observed)
                    return find

                # The first eight presses keep the deadline they had when they
                # filled an eight-slot FIFO. Admitted now, Volume Up and Down
                # add two presses to the prefix and two repeating streams ahead
                # of each later wait behind the holds, so those waits get two
                # press periods more, measured here on the wire.
                missing = "No exact ten-frame physical FIFO admission prefix"
                head = self._wait(prefix(8, deadline), deadline_ns=deadline, message=missing)
                press_period = (head[-1].timestamp_ns - head[0].timestamp_ns) // 7
                volume_ns = 2 * press_period
                admitted = self._wait(prefix(len(expected), deadline + volume_ns),
                                      deadline_ns=deadline + volume_ns, message=missing)
                # End two admitted navigation holds while eight others,
                # including both volume keys, stay down. Drain their already
                # admitted frames.
                capacity_up, _ = self._change(
                    lambda: reports.report(keyboard=_HELD_USAGES, consumer=0), record, "source_capacity_up")
                capacity_through = capacity_up + self._budget()
                self._drain(capacity_through)
                # Pressing released Down again proves fresh admission while
                # the other holds, including both volume keys, continue.
                # Its recipient still has held source keys, so its own up does
                # not require a separate CEC release.
                barrier_after, barrier_deadline = self._change(
                    lambda: reports.report(keyboard=(*_HELD_USAGES, 0x51), consumer=0), record, "source_barrier_down")
                barrier = self._press(source, Key.DOWN, after=barrier_after,
                                      deadline_ns=barrier_deadline + volume_ns, require_pressed=False)
                barrier_up, _ = self._change(
                    lambda: reports.report(keyboard=_HELD_USAGES, consumer=0), record, "source_barrier_up")
                # A Down repeat admitted before that up can still finish.
                barrier_through = barrier_up + self._budget() + volume_ns
                held_through = max(barrier.timestamp_ns + self._budget(), barrier_through)
                self._drain(held_through)
                up, up_deadline = self._change(reports.all_up, record, "held_all_up")
                released = self._release(source, after=up, deadline_ns=up_deadline + volume_ns)
                initial_volume_release = self._release(volume, after=up, deadline_ns=up_deadline + volume_ns)
                quiet_through = max(released.timestamp_ns, initial_volume_release.timestamp_ns) + self._budget()
                self._drain(quiet_through)

                def no_replay(frames, *, before_fresh):
                    for gpio, frame in frames:
                        if frame.timestamp_ns <= started:
                            continue
                        if (gpio == source.gpio and frame.destination == source.address.logical
                                and frame.opcode == 0x44 and frame.timestamp_ns > barrier.timestamp_ns
                                and (frame.operands == b"\x01" or frame.operands == b"\x02"
                                     and frame.timestamp_ns > barrier_through)):
                            raise AssertionError("A released navigation hold replayed after the fresh FIFO barrier")
                        if (gpio == volume.gpio and frame.destination == volume.address.logical
                                and frame.timestamp_ns > initial_volume_release.timestamp_ns
                                and frame.opcode in (0x44, 0x45)):
                            raise AssertionError("The consumer stream continued after its final release")
                        if (gpio == source.gpio and frame.destination == source.address.logical
                                and frame.opcode == 0x44 and released.timestamp_ns < frame.timestamp_ns <= before_fresh):
                            raise AssertionError("Source presses continued after their final admitted release")

                fresh = time.monotonic_ns()
                held = self._snapshot()
                no_replay(held, before_fresh=fresh)
                # Both volume keys stayed down until all-up. A running stream
                # always has one press outstanding, so each must still put a
                # press on the wire after held_through, ahead of its release.
                for key in (Key.VOLUME_UP, Key.VOLUME_DOWN):
                    if not any(gpio == volume.gpio and frame.destination == volume.address.logical
                               and frame.opcode == 0x44 and frame.operands == bytes([key])
                               and held_through < frame.timestamp_ns < initial_volume_release.timestamp_ns
                               for gpio, frame in held):
                        raise AssertionError("A held volume key stopped repeating before its final release")
                fresh, fresh_deadline = self._change(lambda: reports.report(keyboard=(0x28,), consumer=0), record, "fresh_select_down")
                fresh_press = self._press(source, Key.SELECT, after=fresh, deadline_ns=fresh_deadline)
                fresh_up, fresh_up_deadline = self._change(reports.all_up, record, "fresh_select_up")
                source_release = self._release(source, after=fresh_up, deadline_ns=fresh_up_deadline)
                mute, mute_deadline = self._change(lambda: reports.report(consumer=0xE2), record, "fresh_mute_down")
                mute_press = self._press(volume, Key.MUTE, after=mute, deadline_ns=mute_deadline)
                mute_up, mute_up_deadline = self._change(reports.all_up, record, "fresh_mute_up")
                volume_release = self._release(volume, after=mute_up, deadline_ns=mute_up_deadline)

            after = self.dut.capture_logs(expected_boot=self.scan._boot).assert_same_boot()
            drops = _fifo_drops(fresh_log_delta(before, after))
            record["fifo_drops"] = [{"gpio": gpio, "frame": raw.hex(":")} for gpio, raw in drops]
            if drops:
                raise AssertionError("The CEC FIFO dropped a frame during the ten-key check")
            def final_policy(frames):
                # Include both drains and the slow final owner read. Exactly
                # two releases belong to each recipient across these streams.
                frames = [(gpio, frame) for gpio, frame in frames if frame.timestamp_ns > started]
                releases = [(gpio, frame.raw, frame.timestamp_ns) for gpio, frame in frames if frame.opcode == 0x45]
                expected_releases = [(source.gpio, released.raw, released.timestamp_ns),
                                     (volume.gpio, initial_volume_release.raw, initial_volume_release.timestamp_ns),
                                     (source.gpio, source_release.raw, source_release.timestamp_ns),
                                     (volume.gpio, volume_release.raw, volume_release.timestamp_ns)]
                if releases != expected_releases:
                    raise AssertionError("FIFO streams need exactly two source and two consumer releases")
                if volume_release.timestamp_ns <= mute_up:
                    raise AssertionError("Fresh Mute was released before its matching physical up")
                for gpio, frame in frames:
                    if frame.opcode != 0x44:
                        continue
                    if gpio == volume.gpio and frame.destination == volume.address.logical:
                        initial = (frame.operands in (b"\x43", b"\x41", b"\x42")
                                   and started < frame.timestamp_ns < initial_volume_release.timestamp_ns)
                        fresh_mute = (frame.operands == b"\x43"
                                      and mute < frame.timestamp_ns < volume_release.timestamp_ns)
                        if not initial and not fresh_mute:
                            raise AssertionError("An old consumer press replayed outside its admitted stream")
                    if (gpio == source.gpio and frame.destination == source.address.logical
                            and frame.timestamp_ns > released.timestamp_ns
                            and not (fresh < frame.timestamp_ns < source_release.timestamp_ns
                                     and frame.operands == b"\x00")):
                        raise AssertionError("An old source hold replayed after its final release")
            record.update(component=source.name, volume_component=volume.name,
                          admitted_prefix=[frame.raw.hex(":") for frame in admitted],
                          prefix_includes_all_dut_frames=True, press_period_ns=press_period,
                          ten_down_monotonic_ns=started,
                          ten_key_report_count=1, ten_key_usages=list(_USAGES),
                          source_barrier_key=int(Key.DOWN),
                          source_barrier_up_monotonic_ns=barrier_up,
                          source_capacity_up_monotonic_ns=capacity_up,
                          source_capacity_drained_through_monotonic_ns=capacity_through,
                          held_keyboard_usages=list(_HELD_USAGES),
                          source_barrier_wire_timestamp_ns=barrier.timestamp_ns,
                          held_after_barrier_through_monotonic_ns=held_through,
                          all_up_monotonic_ns=up, initial_release_wire_timestamp_ns=released.timestamp_ns,
                          initial_volume_release_wire_timestamp_ns=initial_volume_release.timestamp_ns,
                          no_replay_through_monotonic_ns=quiet_through,
                          fresh_press_wire_timestamp_ns=fresh_press.timestamp_ns,
                          fresh_mute_wire_timestamp_ns=mute_press.timestamp_ns,
                          drop_log_same_boot=True)
            routes = {key: source for key in _KEYS}
            routes.update({key: volume for key in (Key.VOLUME_UP, Key.VOLUME_DOWN, Key.MUTE)})
            return self._finish(record, routes=routes, released=((source, source_release), (volume, volume_release)),
                                validate_frames=final_policy)
        except BaseException as error:
            primary = error
            raise
        finally:
            self._all_up(keyboard, primary)
