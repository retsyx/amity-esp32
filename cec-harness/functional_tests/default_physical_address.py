"""Live adapter defaults select a scanned back address or the new fallback."""

from contextlib import nullcontext

from amity_harness import DUT, TestBed, TvComponent, AvrComponent, PlaybackComponent
from amity_harness.physical_address import AdapterAddressEvidence


def _exercise_defaults(main, owner, tv, scan, *, detected):
    # Priming makes the next save real even if its value was already configured.
    # In the scanned cases, the detected address then equals the old default.
    with AdapterAddressEvidence(main, tv, scan) as priming:
        with owner.front_physical_address(0, "2.0.0.0") as primed:
            # Complete a real identity exchange before observing the next save;
            # the priming announcement may still be in the owner's CEC FIFO.
            priming.assert_selected(primed.read()["effective_front_physical_address"], changed=False)
            with AdapterAddressEvidence(main, tv, scan) as baseline:
                with owner.front_physical_address(0, "1.0.0.0") as original:
                    original.assert_address(detected=detected, selected=detected or "1.0.0.0")
                    baseline.assert_selected(detected or "1.0.0.0", changed=detected is None)

                    with AdapterAddressEvidence(main, tv, scan) as change:
                        with owner.front_physical_address(0, "2.0.0.0") as edited:
                            edited.assert_address(detected=detected, selected=detected or "2.0.0.0")
                            change.assert_selected(detected or "2.0.0.0", changed=detected is None)

                            with AdapterAddressEvidence(main, tv, scan) as restore:
                                edited.close()
                                original.assert_address(detected=detected, selected=detected or "1.0.0.0")
                                restore.assert_selected(detected or "1.0.0.0", changed=detected is None)


def _default_physical_address(*, remote, scannable):
    main = DUT("amity-dut")
    target = DUT("amity-dut-2") if remote else main
    connection = target.tentacle(0)
    tv = TvComponent(connection.front, "Address TV")
    components = [tv]
    if scannable:
        avr = AvrComponent(connection.back, "Address AVR")
        player = PlaybackComponent(avr, "Address Player", port=1)
        components.extend((avr, player))

    duts = (main, target) if remote else (main,)
    with TestBed(*duts, *components, members=(target,) if remote else ()):
        with (main.add_amity(target) if remote else nullcontext(main)) as owner:
            if remote:
                owner.assert_connected()
            scan = main.scan_hdmi()
            scan.assert_contains_only(*components)
            for component in components:
                scan.assert_on(component, component.endpoint)
            _exercise_defaults(main, owner, tv, scan, detected="1.0.0.0" if scannable else None)
            if remote:
                owner.assert_connected()


def test_default_physical_address_local():
    _default_physical_address(remote=False, scannable=False)


def test_default_physical_address_local_scanned():
    _default_physical_address(remote=False, scannable=True)


def test_default_physical_address_remote():
    _default_physical_address(remote=True, scannable=False)


def test_default_physical_address_remote_scanned():
    _default_physical_address(remote=True, scannable=True)
