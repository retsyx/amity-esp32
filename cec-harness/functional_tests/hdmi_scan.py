"""One Amity discovers a TV, AVR and playback device."""

from amity_harness import DUT, TestBed, TvComponent, AvrComponent, PlaybackComponent


def test_hdmi_scan():
    dut = DUT("amity-dut")
    tv = TvComponent(dut, "Scan TV")
    avr = AvrComponent(dut, "Scan AVR", parent=tv, port=1)
    player = PlaybackComponent(dut, "Scan Player", parent=avr, port=1)

    with TestBed(dut, tv, avr, player):
        dut.scan_hdmi().assert_contains(tv, avr, player)
