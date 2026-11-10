"""Use HomeKit's SRP shared-secret encoding during the owned Pair Setup."""

from contextlib import contextmanager
from importlib import import_module


@contextmanager
def pair_setup_srp():
    """Temporarily correct aiohomekit's SRP hashing in this private worker."""
    protocol = import_module("aiohomekit.protocol")
    original = protocol.SrpClient

    class PairSetupSrpClient(original):
        # Apple's ADK hashes S without leading zero bytes. Keep aiohomekit's
        # proof generation and strict server-proof verification unchanged.
        # https://github.com/apple/HomeKitADK/blob/master/PAL/Crypto/MbedTLS/HAPMbedTLS.c
        def get_shared_secret_bytes(self):
            return bytes(self.to_byte_array(self.get_shared_secret()))

    protocol.SrpClient = PairSetupSrpClient
    try:
        yield
    finally:
        protocol.SrpClient = original
