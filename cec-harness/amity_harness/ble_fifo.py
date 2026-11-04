"""Saturate the real CEC FIFO with paired-keyboard input and verify dropped holds."""

import re
import time

from .ble_input import BLEInputEvidence
from .cec import Frame
from .keys import Key
from .tx_policy import fresh_log_delta


_USAGES = (0x28, 0x29, 0x4A, 0x4F, 0x50, 0x51)
_KEYS = (0x00, 0x0D, 0x09, 0x04, 0x03, 0x02)
_DROP = re.compile(r"TX dropped: FIFO full \(GPIO (\d+), (\d+) bytes: ([0-9a-fA-F ]+)\)")


def _fifo_drops(text):
    rows = []
    for match in _DROP.finditer(text):
        raw = bytes.fromhex(match[3])
        if len(raw) != int(match[2]) or not 1 <= len(raw) <= 16:
            raise AssertionError("A FIFO drop log has an invalid frame length")
        rows.append((int(match[1]), raw))
    return rows


class BLEFIFOEvidence(BLEInputEvidence):
    """Eight admitted frames and exact dropped presses, using shipping UI logs."""

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

    def assert_saturated(self, keyboard, *, source, volume):
        if (source not in self.components or volume not in self.components or source is volume
                or source.dut is not self.dut or volume.dut is not self.dut
                or source.gpio != volume.gpio):
            raise ValueError("Logged FIFO saturation needs distinct local recipients on one CEC wire")
        self._fifo_gpio = source.gpio
        record = self._begin(keyboard, "fifo_saturation")
        before = self.dut.capture_logs(expected_boot=self.scan._boot).assert_same_boot()
        initiator = self._initiators[source.gpio]
        expected = [(source, Frame(initiator, source.address.logical, 0x44, bytes([key]))) for key in _KEYS]
        expected.extend(((volume, Frame(initiator, volume.address.logical, 0x44, bytes([Key.VOLUME_UP]))),
                         (volume, Frame(initiator, volume.address.logical, 0x45))))
        primary = None
        try:
            with keyboard.reports() as reports:
                # Six keyboard holds plus one consumer hold submit seven
                # presses. Switching the consumer submits its release eighth;
                # the next two distinct presses must encounter the full FIFO.
                started, _ = self._change(lambda: reports.report(keyboard=_USAGES, consumer=0xE9), record, "seven_down")
                self._change(lambda: reports.report(keyboard=_USAGES, consumer=0xEA), record, "volume_down_drop")
                last, deadline = self._change(lambda: reports.report(keyboard=_USAGES, consumer=0xE2), record, "mute_drop")

                def prefix(_):
                    frames = [(gpio, frame) for gpio, frame in self._wire_snapshot()
                              if gpio == source.gpio and frame.timestamp_ns > started]
                    if len(frames) < 8:
                        return None
                    observed = frames[:8]
                    if [(gpio, frame.raw) for gpio, frame in observed] != [(component.gpio, frame.raw) for component, frame in expected]:
                        raise AssertionError("The saturated CEC FIFO did not transmit its exact eight admitted frames in order")
                    if not all(self._handled(component, frame) for (component, _), (_, frame) in zip(expected, observed)):
                        return None
                    return tuple(frame for _, frame in observed)

                admitted = self._wait(prefix, deadline_ns=deadline, message="No exact eight-frame physical FIFO admission prefix")
                # Replace one source hold with a new key after the prefix has
                # drained. Keep dropped Mute physically down as capacity frees.
                barrier_after, barrier_deadline = self._change(
                    lambda: reports.report(keyboard=(*_USAGES[:-1], 0x52), consumer=0xE2), record, "source_barrier")
                barrier = self._press(source, Key.UP, after=barrier_after, deadline_ns=barrier_deadline)
                held_through = barrier.timestamp_ns + self._budget()
                self._drain(held_through)
                up, up_deadline = self._change(reports.all_up, record, "saturated_all_up")
                released = self._release(source, after=up, deadline_ns=up_deadline)
                quiet_through = released.timestamp_ns + self._budget()
                self._drain(quiet_through)

                def no_replay(frames, *, before_fresh):
                    for gpio, frame in frames:
                        if frame.timestamp_ns <= started:
                            continue
                        if frame.opcode == 0x44 and frame.operands in (b"\x42", b"\x43"):
                            raise AssertionError("A FIFO-dropped BLE press was transmitted or replayed")
                        if (gpio == volume.gpio and frame.destination == volume.address.logical
                                and frame.timestamp_ns > admitted[-1].timestamp_ns and frame.opcode in (0x44, 0x45)):
                            raise AssertionError("A failed consumer hold or matching up produced delayed CEC work")
                        if (gpio == source.gpio and frame.destination == source.address.logical
                                and frame.opcode == 0x44 and released.timestamp_ns < frame.timestamp_ns <= before_fresh):
                            raise AssertionError("Source presses continued after their final admitted release")

                fresh = time.monotonic_ns()
                no_replay(self._snapshot(), before_fresh=fresh)
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
            dut_gpio = volume.dut.wiring.dut_cec_gpio(volume.dut.name, volume.tentacle, volume.direction)
            key_drops = [(gpio, raw) for gpio, raw in drops if len(raw) > 1 and raw[1] in (0x44, 0x45)]
            expected_drops = [(dut_gpio, Frame(initiator, volume.address.logical, 0x44, bytes([key])).raw)
                              for key in (Key.VOLUME_DOWN, Key.MUTE)]
            if key_drops != expected_drops:
                raise AssertionError("The real FIFO did not log exactly the two expected dropped key presses")
            status_query = Frame(initiator, volume.address.logical, 0x71).raw
            if any(gpio != dut_gpio or raw != status_query
                   for gpio, raw in drops if (gpio, raw) not in key_drops):
                raise AssertionError("FIFO saturation dropped unexpected non-key work")
            def final_policy(frames):
                # Include both drains and the slow final owner read. Exactly
                # two releases belong to each recipient across these streams.
                frames = [(gpio, frame) for gpio, frame in frames if frame.timestamp_ns > started]
                releases = [(gpio, frame.raw, frame.timestamp_ns) for gpio, frame in frames if frame.opcode == 0x45]
                expected_releases = [(volume.gpio, admitted[-1].raw, admitted[-1].timestamp_ns),
                                     (source.gpio, released.raw, released.timestamp_ns),
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
                        initial_up = (frame.raw == admitted[-2].raw
                                      and frame.timestamp_ns == admitted[-2].timestamp_ns)
                        fresh_mute = (frame.operands == b"\x43"
                                      and mute < frame.timestamp_ns < volume_release.timestamp_ns)
                        if not initial_up and not fresh_mute:
                            raise AssertionError("An old consumer press replayed outside its admitted stream")
                    if (gpio == source.gpio and frame.destination == source.address.logical
                            and frame.timestamp_ns > released.timestamp_ns
                            and not (fresh < frame.timestamp_ns < source_release.timestamp_ns
                                     and frame.operands == b"\x00")):
                        raise AssertionError("An old source hold replayed after its final release")
            record.update(component=source.name, volume_component=volume.name,
                          fifo_capacity=8, admitted_prefix=[frame.raw.hex(":") for frame in admitted],
                          prefix_includes_all_dut_frames=True,
                          dropped_key_frames=[raw.hex(":") for _, raw in key_drops],
                          non_key_drops=[raw.hex(":") for gpio, raw in drops if (gpio, raw) not in key_drops],
                          saturated_burst_end_monotonic_ns=last,
                          source_barrier_wire_timestamp_ns=barrier.timestamp_ns,
                          held_after_drop_through_monotonic_ns=held_through,
                          all_up_monotonic_ns=up, initial_release_wire_timestamp_ns=released.timestamp_ns,
                          no_replay_through_monotonic_ns=quiet_through,
                          fresh_press_wire_timestamp_ns=fresh_press.timestamp_ns,
                          fresh_mute_wire_timestamp_ns=mute_press.timestamp_ns,
                          drop_log_same_boot=True)
            routes = {key: source for key in (*_KEYS, Key.UP)}
            routes.update({key: volume for key in (Key.VOLUME_UP, Key.VOLUME_DOWN, Key.MUTE)})
            return self._finish(record, routes=routes, released=((source, source_release), (volume, volume_release)),
                                validate_frames=final_policy)
        except BaseException as error:
            primary = error
            raise
        finally:
            self._all_up(keyboard, primary)
