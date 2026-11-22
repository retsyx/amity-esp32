"""A paired-remote stand-in for scenario tests; pairing is tested separately."""


class PairedRemote:
    """Forward ``press`` to a scenario's key fake while its pairing is live."""

    def __init__(self, press):
        self._press, self.entered, self.closed, self.presses = press, False, False, []

    def __enter__(self):
        self.entered = True
        return self

    def __exit__(self, exc_type, exc, traceback):
        self.closed = True
        return False

    def press(self, key, *, duration_ms=0):
        if not self.entered or self.closed:
            raise AssertionError("Remote keys need the scenario's live pairing")
        self.presses.append((key, duration_ms))
        return self._press(key, duration_ms)


def pairing(press, remotes=None):
    """Return a ``DUT.pair_ble_keyboard`` patch calling ``press(dut, key, duration_ms)``."""
    def pair(dut, **kwargs):
        remote = PairedRemote(lambda key, duration_ms: press(dut, key, duration_ms))
        if remotes is not None:
            remotes.append((dut, remote))
        return remote
    return pair
