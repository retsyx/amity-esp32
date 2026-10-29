"""One Amity discovers a TV, AVR and playback device."""

from contextlib import contextmanager

from amity_harness import DUT, TestBed, TvComponent, AvrComponent, PlaybackComponent


@contextmanager
def scanned_devices(*, extra_player=False):
    """Share this scenario's topology and full scan assertions with later tests."""
    dut = DUT("amity-dut")
    tentacle = dut.tentacle(0)
    tv = TvComponent(tentacle.front, "Scan TV")
    avr = AvrComponent(tentacle.back, "Scan AVR")
    player = PlaybackComponent(avr, "Scan Player", port=1)
    others = (PlaybackComponent(avr, "Other Player", port=2, address_type="tuner"),) if extra_player else ()
    components = (tv, avr, player, *others)

    with TestBed(dut, *components):
        scan = dut.scan_hdmi()
        scan.assert_contains(*components)
        scan.assert_contains_only(*components)
        scan.assert_on(tv, tentacle.front)
        scan.assert_on(avr, tentacle.back)
        scan.assert_on(player, tentacle.back)
        for other in others:
            scan.assert_on(other, tentacle.back)
        yield dut, *components


def test_hdmi_scan():
    with scanned_devices():
        pass
