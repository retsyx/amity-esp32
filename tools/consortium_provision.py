#!/usr/bin/env python3
"""Physically provision the reset-retained consortium SRP verifier over USB.

The generated setup secret remains only in a mode-0600 local code file. USB
carries salt/verifier, never that password. Existing Wi-Fi, fabrics, bonds and
operational identity are preserved. Existing verifiers require the explicit
--rotate --confirm-id physical USB workflow; the previous setup code is revoked.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import secrets
import time

N = int("FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7EDEE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8A163BF0598DA48361C55D39A69163FA8FD24CF5F83655D23DCA3AD961C62F356208552BB9ED529077096966D670C354E4ABC9804F1746C08CA18217C32905E462E36CE3BE39E77\
2C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF6955817183995497CEA956AE515D2261898FA051015728E5A8AAAC42DAD33170D04507A33A85521ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7ABF5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6BF12FFA06D98A0864D87602733EC86A64521F2B18177B200CBBE117577A615D6C770988C0BAD946E208E24FA074E5AB3143DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF", 16)


def verifier(username: str, password: str, salt: bytes) -> bytes:
    if not re.fullmatch(r"[0-9a-f]{32}", username) or username == "0" * 32:
        raise ValueError("invalid OctopusId")
    if not re.fullmatch(r"[0-9a-f]{64}", password) or len(salt) != 32:
        raise ValueError("invalid bootstrap profile")
    inner = hashlib.sha512((username + ":" + password).encode("ascii")).digest()
    x = int.from_bytes(hashlib.sha512(salt + inner).digest(), "big")
    return pow(5, x, N).to_bytes(384, "big")


def wait_for(port, prefix: bytes, timeout: float) -> bytes:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        line = port.readline().strip()
        if line.startswith(prefix):
            return line
    raise RuntimeError("The device did not acknowledge the USB command")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--rotate", action="store_true", help="Rotate a lost or disclosed verifier through physically attached USB")
    parser.add_argument("--confirm-id", help="Exact local OctopusId required for rotation")
    args = parser.parse_args()
    if args.rotate and (not args.confirm_id or not re.fullmatch(r"[0-9a-f]{32}", args.confirm_id) or args.confirm_id == "0" * 32):
        parser.error("Rotation requires --confirm-id with the exact nonzero lowercase OctopusId")
    if args.confirm_id and not args.rotate:
        parser.error("--confirm-id is used only with --rotate")
    if args.output.exists():
        parser.error("The output file already exists")
    import serial
    device = serial.Serial(port=None, baudrate=115200, timeout=0.2, exclusive=True)
    device.dtr = False
    device.rts = False
    device.port = args.port
    device.open()
    with device as port:
        port.write(b"\nAMITY CONSORTIUM ID\n")
        line = wait_for(port, b"AMITY CONSORTIUM ID ", 10)
        username = line.split()[-1].decode("ascii")
        if args.rotate and username != args.confirm_id:
            raise RuntimeError("The attached OctopusId differs from --confirm-id; no verifier or code artifact was changed")
        password = secrets.token_hex(32)
        salt = secrets.token_bytes(32)
        record = salt.hex() + ":" + verifier(username, password, salt).hex()
        # Write the recoverable secret artifact first. A USB interruption cannot
        # leave a committed verifier whose only corresponding code was lost.
        fd = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w") as output:
            output.write("amityc1:" + username + ":" + password + "\n")
            output.flush()
            os.fsync(output.fileno())
        command = "ROTATE " + username + " " if args.rotate else "PROVISION "
        prefix = b"AMITY CONSORTIUM ROTATE " if args.rotate else b"AMITY CONSORTIUM PROVISION "
        port.write(("AMITY CONSORTIUM " + command + record + "\n").encode("ascii"))
        result = wait_for(port, prefix, 10)
        if result != prefix + b"OK":
            raise RuntimeError("The device did not confirm the verifier change. The private code artifact is retained; do not assume which code is active after an interrupted write.")
    print("Consortium verifier " + ("rotated. Origin identity retained." if args.rotate else "installed.") + " Private setup code saved to " + str(args.output))


if __name__ == "__main__":
    main()
