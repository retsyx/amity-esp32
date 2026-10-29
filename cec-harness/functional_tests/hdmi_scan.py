"""One Amity discovers a TV, AVR and playback device."""

from contextlib import contextmanager

from amity_harness import DUT, TestBed, TvComponent, AvrComponent, PlaybackComponent


@contextmanager
def scanned_devices():
    """Share this scenario's topology and full scan assertions with later tests."""
    dut = DUT("amity-dut")
    tentacle = dut.tentacle(0)
    tv = TvComponent(tentacle.front, "Scan TV")
    avr = AvrComponent(tentacle.back, "Scan AVR")
    player = PlaybackComponent(avr, "Scan Player", port=1)

    with TestBed(dut, tv, avr, player):
        scan = dut.scan_hdmi()
        scan.assert_contains(tv, avr, player)
        scan.assert_contains_only(tv, avr, player)
        scan.assert_on(tv, tentacle.front)
        scan.assert_on(avr, tentacle.back)
        scan.assert_on(player, tentacle.back)
        yield dut, tv, avr, player


def test_hdmi_scan():
    with scanned_devices():
        pass
