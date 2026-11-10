"""Pair Setup hashes minimal SRP shared secrets and keeps strict proof checks."""

import asyncio
import hashlib
from importlib import import_module
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness.homekit_srp import pair_setup_srp


class PairSetupBindingTests(unittest.TestCase):
    def test_original_binding_is_restored_after_success_error_and_cancellation(self):
        class OriginalClient:
            pass

        protocol = SimpleNamespace(SrpClient=OriginalClient)
        for error in (None, RuntimeError("pairing failed"), asyncio.CancelledError()):
            with self.subTest(error=type(error).__name__), \
                    patch("amity_harness.homekit_srp.import_module", return_value=protocol) as load:
                try:
                    with pair_setup_srp():
                        self.assertIsNot(protocol.SrpClient, OriginalClient)
                        self.assertTrue(issubclass(protocol.SrpClient, OriginalClient))
                        if error is not None:
                            raise error
                except BaseException as caught:
                    self.assertIs(caught, error)
                self.assertIs(protocol.SrpClient, OriginalClient)
                load.assert_called_once_with("aiohomekit.protocol")

    def test_nested_scope_restores_the_enclosing_binding(self):
        protocol = SimpleNamespace(SrpClient=type("OriginalClient", (), {}))
        original = protocol.SrpClient
        with patch("amity_harness.homekit_srp.import_module", return_value=protocol):
            with pair_setup_srp():
                outer = protocol.SrpClient
                with pair_setup_srp():
                    self.assertIsNot(protocol.SrpClient, outer)
                self.assertIs(protocol.SrpClient, outer)
            self.assertIs(protocol.SrpClient, original)


try:
    protocol = import_module("aiohomekit.protocol")
except ModuleNotFoundError as error:
    if error.name != "aiohomekit":
        raise
    protocol = None


# Deterministic SRP-3072/SHA-512 vectors independently evaluated from Apple's
# ADK formula K=H(minimal S). A/B have full width in both vectors so these
# isolate the shared-secret encoding and leave existing proof behavior intact.
# The fixed test exponents deliberately exercise encodings, not entropy.
# https://github.com/apple/HomeKitADK/blob/master/PAL/Crypto/MbedTLS/HAPMbedTLS.c
_VECTORS = (
    ("normal", 309237455106247847792077114418321698227, 1, (384, 384, 384),
     "5c19ae2c0de664c7948b93f17f30db53c47be24360ca4c5ed0559af5e0a517bc7239f0236fb0bbff5a461b0c17a085322c281bf72b49db60f3353ef9d1d6446b",
     "d186d8eb4549cf8333fd2dad0201e5d152919c5583ff17fca2cfd689ed47cd0636d0b3e14ff1be0e4f142901fcf3ad565a8e858e8b434de92c593b1d5c6f665c",
     "ea17429355c47779a341089fa0dcf09e8be128247081b250571da80c9437d6e1d6bf5a7fcbf8b959dc53f04d733e186fcb1134a6f3abe5298c9dbeb554709893"),
    ("leading_S", 309237455106247847792077114418321698227, 22, (384, 384, 383),
     "da2b07e1d6d2e998757220763b957046e730f55e4135c1033ae68f1086104fbd159092fc36912831ff0f3b3772a3e80ba3ec352f85b1b18be4dab3032e265f7a",
     "c9f228f205426e5cf52b453634b7243cd2959fed7984e65ba8cd6819bb33ab1ed2b6278623b2f1401a9051fb50d4c8dbbdf30a4127979669f5987eff0868bd1c",
     "d16d1f8ed5e239c9d3f2fe09b338049c1527c65b746f4ddb1e6a74fd4e341ea855174dc4eeb064e4bb5f66cf2a1ac4c8fd57210467d7851123facac3a3ece9a3"),
)


@unittest.skipIf(protocol is None, "requires aiohomekit from the Pi harness runtime")
class InstalledControllerSrpTests(unittest.TestCase):
    salt = bytearray.fromhex("000102030405060708090a0b0c0d0e0f")
    pin = "123-45-679"

    def server_public(self, exponent):
        srp = import_module("aiohomekit.crypto.srp")
        digest = hashlib.sha512(b"Pair-Setup:" + self.pin.encode()).digest()
        x = int.from_bytes(hashlib.sha512(self.salt + digest).digest(), "big")
        verifier = pow(srp.GENERATOR_VALUE, x, srp.MODULUS_VALUE)
        public = (srp.CLIENT_K_VALUE * verifier +
                  pow(srp.GENERATOR_VALUE, exponent, srp.MODULUS_VALUE)) % srp.MODULUS_VALUE
        return public.to_bytes(384, "big")

    def test_actual_controller_matches_vectors_and_rejects_wrong_server_proofs(self):
        original = protocol.SrpClient
        for name, a, b, lengths, key, m1, m2 in _VECTORS:
            with self.subTest(vector=name), \
                    patch.object(original, "generate_private_key", return_value=a), pair_setup_srp():
                client = protocol.SrpClient("Pair-Setup", self.pin)
                client.set_salt(self.salt)
                client.set_server_public_key(self.server_public(b))
                self.assertEqual(tuple(len(value.lstrip(b"\0")) for value in
                                       (client.A_b, client.B_b, client.get_shared_secret_bytes())), lengths)
                self.assertEqual(len(client.get_public_key_bytes()), 384)
                self.assertEqual(client.get_session_key_bytes().hex(), key)
                self.assertEqual(client.get_proof_bytes().hex(), m1)
                self.assertTrue(client.verify_servers_proof_bytes(bytes.fromhex(m2)))
                wrong = bytes([bytes.fromhex(m2)[0] ^ 1]) + bytes.fromhex(m2)[1:]
                self.assertFalse(client.verify_servers_proof_bytes(wrong))
            self.assertIs(protocol.SrpClient, original)

    def test_actual_pair_setup_generator_uses_adapter_and_enforces_server_proof(self):
        original = protocol.SrpClient
        tlv = protocol.TLV
        for name, a, b, _, _, m1, m2 in _VECTORS:
            for valid in (True, False):
                with self.subTest(vector=name, valid_proof=valid), \
                        patch.object(original, "generate_private_key", return_value=a), pair_setup_srp():
                    exchange = protocol.perform_pair_setup_part2(self.pin, "owned-controller", self.salt,
                                                                 bytearray(self.server_public(b)))
                    try:
                        request, _ = next(exchange)
                        self.assertEqual(dict(request)[tlv.kTLVType_Proof].hex(), m1)
                        proof = bytes.fromhex(m2)
                        if not valid:
                            proof = bytes([proof[0] ^ 1]) + proof[1:]
                        response = [(tlv.kTLVType_State, tlv.M4), (tlv.kTLVType_Proof, proof)]
                        if valid:
                            request, _ = exchange.send(response)
                            self.assertEqual(dict(request)[tlv.kTLVType_State], tlv.M5)
                        else:
                            with self.assertRaises(protocol.AuthenticationError):
                                exchange.send(response)
                    finally:
                        exchange.close()
                self.assertIs(protocol.SrpClient, original)


if __name__ == "__main__":
    unittest.main()
