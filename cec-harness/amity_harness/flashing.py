"""Write one OTA application partition image to every bench DUT over USB.

This is bench maintenance, not a functional-test control surface. It writes
only `otadata` and `ota_0`; NVS, factory provisioning, Matter credentials and
saved configuration are left unchanged. Erasing `otadata` makes the bootloader
start the freshly written `ota_0` image. The boards are flashed in parallel and
failures are reported per DUT without retry.
"""

import argparse
from contextlib import ExitStack
from pathlib import Path
import subprocess
import sys
import tempfile

from .usb import locate_usb
from .wiring import Wiring


# partitions.csv: otadata at 0x21000 (0x2000 bytes), ota_0 at 0x40000 (0x3d0000 bytes).
OTADATA_OFFSET = 0x21000
OTADATA_BYTES = 0x2000
OTA_0_OFFSET = 0x40000
SLOT_BYTES = 0x3d0000
TIMEOUT_SECONDS = 300
_ESP_IMAGE_MAGIC = b"\xe9"


def validate_image(path):
    """Accept only a complete OTA partition image such as build/amity-partition.bin."""
    path = Path(path)
    try:
        size = path.stat().st_size
        with path.open("rb") as image:
            magic = image.read(1)
    except OSError as error:
        raise ValueError(f"Cannot read {path}: {error.strerror or error}") from None
    if size != SLOT_BYTES or magic != _ESP_IMAGE_MAGIC:
        raise ValueError(f"{path} is not a complete OTA partition image; use build/amity-partition.bin")
    return path.resolve()


def esptool_command(python, port, otadata, image):
    return [str(python), "-m", "esptool", "--chip", "esp32s3", "--port", port, "--baud", "460800",
            "--before", "default-reset", "--after", "hard-reset", "write-flash",
            hex(OTADATA_OFFSET), str(otadata), hex(OTA_0_OFFSET), str(image)]


def flash_all(image, *, wiring=None, python=sys.executable, popen=subprocess.Popen,
              timeout=TIMEOUT_SECONDS, out=sys.stdout, err=sys.stderr):
    """Flash every documented DUT; return the names of DUTs that failed.

    A missing or inaccessible board fails on its own; the others are flashed.
    """
    image = validate_image(image)
    wiring = Wiring.from_markdown() if wiring is None else wiring
    targets, failed = [], []
    for target in locate_usb(wiring):
        if "error" in target:
            failed.append(target["dut"])
            print(f"{target['dut']}: FAILED (USB {target['error']})", file=err, flush=True)
        else:
            targets.append(target)
    with tempfile.TemporaryDirectory(prefix="amity-flash-") as directory, ExitStack() as logs:
        otadata = Path(directory) / "otadata.bin"
        otadata.write_bytes(b"\xff" * OTADATA_BYTES)
        workers = []
        try:
            for target in targets:
                log = logs.enter_context(tempfile.TemporaryFile(dir=directory))
                command = esptool_command(python, target["port"], otadata, image)
                workers.append((target, popen(command, stdin=subprocess.DEVNULL, stdout=log,
                                              stderr=subprocess.STDOUT), log))
        except BaseException:
            for _, worker, _ in workers:
                worker.kill()
                worker.wait()
            raise
        for target, worker, log in workers:
            try:
                code = worker.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                worker.kill()
                worker.wait()
                code = "timeout"
            if code == 0:
                print(f"{target['dut']}: flashed {image.name} on {target['port']}", file=out, flush=True)
            else:
                failed.append(target["dut"])
                log.seek(0)
                detail = log.read().decode(errors="replace").strip().splitlines()[-15:]
                print(f"{target['dut']}: FAILED ({code}) on {target['port']}", file=err, flush=True)
                for line in detail:
                    print(f"  {line}", file=err)
    return failed


def main(argv=None):
    parser = argparse.ArgumentParser(
        prog="flash-duts",
        description="USB-flash one application image onto every DUT in TEST-HARNESS.md.")
    parser.add_argument("image", type=Path, help="OTA partition image, e.g. build/amity-partition.bin")
    args = parser.parse_args(argv)
    try:
        failed = flash_all(args.image)
    except (ValueError, OSError, RuntimeError) as error:
        parser.exit(1, f"{parser.prog}: {error}\n")
    if failed:
        print(f"{parser.prog}: failed on {', '.join(failed)}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
