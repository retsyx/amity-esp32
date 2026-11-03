"""Supply missing Pi GPIO CEC support without replacing the stock CEC core.

Linux's CEC_PIN option changes struct cec_adapter, so enabling it only for an
external module would corrupt the core ABI. Our standalone build keeps its pin
state in adapter-private data instead. Hashes pin the GPL-2.0-only upstream
sources; their copyright/license notices remain in the installed DKMS tree.
"""

import hashlib
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tempfile
import urllib.request


_NAME = "amity-cec-gpio"
_VERSION = "1"
_SOURCE_ROOT = Path("/usr/src")
_MODULE_ROOT = Path("/lib/modules")
_UPSTREAM = "https://raw.githubusercontent.com/torvalds/linux"
_FILES = {
    "cec-pin.c": "drivers/media/cec/core/cec-pin.c",
    "cec-pin-priv.h": "drivers/media/cec/core/cec-pin-priv.h",
    "cec-gpio.c": "drivers/media/cec/platform/cec-gpio/cec-gpio.c",
}
_HASHES = {
    "6.12": {
        "cec-pin.c": "003725c91bea161637badf7a3ac316239e11c730352dbd3f19f7a0c1fab55974",
        "cec-pin-priv.h": "dbcfef84c4b13d76f6b64390ed7791864cdb48e56ea47ff0da08782c3f0faada",
        "cec-gpio.c": "0df97e39cceaf3f729bb103e033d3ad8327262a2861314e35ad5883acc946d68",
    },
    "6.18": {
        "cec-pin.c": "dc568958f9f3e11131a95185902cd2b3379eb85ccda70e5af4b00e32eff3a4b8",
        "cec-pin-priv.h": "644f82d55e12dc417b87dbf45003d567ecb6419722ab0e7544eaa76e884094cc",
        "cec-gpio.c": "d819119683427573fa8ac6a18f73bc183f779cf7b3ebc382ec8f0487fb3789e0",
    },
}

_MAKEFILE = """obj-m += cec-gpio.o
ifeq ($(CONFIG_CEC_PIN),y)
cec-gpio-y := gpio.o
else
cec-gpio-y := gpio.o pin.o
endif
"""

_COMPAT = """/* SPDX-License-Identifier: GPL-2.0-only */
#include "cec-pin-priv.h"
static inline void *amity_cec_get_drvdata(struct cec_adapter *adap)
{
#ifdef CONFIG_CEC_PIN
    return cec_get_drvdata(adap);
#else
    struct cec_pin *pin = cec_get_drvdata(adap);
    return pin->amity_drvdata;
#endif
}
"""

# DKMS invokes this for each target kernel, including later package upgrades.
# Keep a copy of the pinned inputs and regenerate every build, never patch a
# previously patched source or silently accept a different upstream revision.
_PREPARE = '''#!/usr/bin/env python3
import hashlib
from pathlib import Path
import re
import sys

HASHES = __HASHES__
release = sys.argv[1]
match = re.match(r"^([0-9]+\\.[0-9]+)(?:[.-]|$)", release)
series = match[1] if match else None
if series not in HASHES:
    raise SystemExit("Amity GPIO CEC supports kernel series 6.12 and 6.18; found " + release)
root = Path(__file__).resolve().parent
sources = {}
for name, digest in HASHES[series].items():
    data = (root / series / name).read_bytes()
    if hashlib.sha256(data).hexdigest() != digest:
        raise SystemExit("Pinned Linux CEC source hash mismatch: " + name)
    sources[name] = data.decode()
pin = sources["cec-pin.c"]
if pin.count("adap->pin") != 11 or pin.count("&cec_pin_adap_ops, priv, name,") != 1:
    raise SystemExit("Unexpected upstream CEC pin storage layout")
pin = pin.replace("adap->pin", "adap->priv")
pin = pin.replace("&cec_pin_adap_ops, priv, name,", "&cec_pin_adap_ops, pin, name,")
pin = pin.replace("pin->ops = pin_ops;", "pin->ops = pin_ops;\\n\\tpin->amity_drvdata = priv;")
header = sources["cec-pin-priv.h"]
if header.count("struct cec_pin {") != 1:
    raise SystemExit("Unexpected upstream private CEC pin structure")
header = header.replace("struct cec_pin {", "struct cec_pin {\\n\\tvoid *amity_drvdata;")
gpio = sources["cec-gpio.c"]
if "#include <media/cec-pin.h>" not in gpio or not gpio.count("cec_get_drvdata(adap)"):
    raise SystemExit("Unexpected upstream CEC GPIO storage layout")
gpio = gpio.replace("#include <media/cec-pin.h>", '#include "amity-compat.h"')
gpio = gpio.replace("cec_get_drvdata(adap)", "amity_cec_get_drvdata(adap)")
for name, data in (("pin.c", pin), ("gpio.c", gpio), ("cec-pin-priv.h", header)):
    (root / name).write_text(data)
'''.replace("__HASHES__", repr(_HASHES))


def kernel_series(release):
    match = re.fullmatch(r"(\d+\.\d+)(?:[.-][A-Za-z0-9.+_-]+)?", release)
    if not match or match[1] not in _HASHES:
        raise RuntimeError("Missing GPIO CEC driver: supported kernel series are 6.12 and 6.18; found " + release)
    return match[1]


def _download(series, name):
    url = f"{_UPSTREAM}/v{series}/{_FILES[name]}"
    with urllib.request.urlopen(url, timeout=60) as response:
        data = response.read(256 * 1024)
    if hashlib.sha256(data).hexdigest() != _HASHES[series][name]:
        raise RuntimeError("Pinned Linux CEC source hash mismatch: " + name)
    return data


def _install_sources():
    destination = _SOURCE_ROOT / f"{_NAME}-{_VERSION}"
    files = {"Makefile": _MAKEFILE.encode(), "amity-compat.h": _COMPAT.encode(),
             "prepare.py": _PREPARE.encode(), "dkms.conf": f'''PACKAGE_NAME="{_NAME}"
PACKAGE_VERSION="{_VERSION}"
BUILT_MODULE_NAME[0]="cec-gpio"
DEST_MODULE_LOCATION[0]="/kernel/drivers/media/cec/platform/cec-gpio"
AUTOINSTALL="yes"
PRE_BUILD="prepare.py ${{kernelver}}"
MAKE[0]="make -C /lib/modules/${{kernelver}}/build M=${{dkms_tree}}/${{PACKAGE_NAME}}/${{PACKAGE_VERSION}}/build modules"
CLEAN="rm -f *.o *.ko *.mod *.mod.c Module.symvers modules.order"
'''.encode()}
    for series, hashes in _HASHES.items():
        for name in hashes:
            files[f"{series}/{name}"] = _download(series, name)
    # A failed download cannot leave DKMS with a partial source tree.
    _SOURCE_ROOT.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=".amity-cec-", dir=_SOURCE_ROOT))
    try:
        for name, data in files.items():
            path = staging / name
            path.parent.mkdir(exist_ok=True)
            path.write_bytes(data)
            if name == "prepare.py":
                path.chmod(0o755)
        if destination.exists():
            for name, data in files.items():
                path = destination / name
                if path.is_symlink() or not path.is_file() or path.read_bytes() != data:
                    raise RuntimeError("Installed Amity CEC DKMS sources differ: " + str(path))
            (destination / "prepare.py").chmod(0o755)
        else:
            os.rename(staging, destination)
    finally:
        if staging.exists():
            shutil.rmtree(staging)


def ensure_driver(run):
    """Install/load GPIO CEC; return True when a kernel reboot must happen first.

    ``run`` is subprocess.run-compatible and defaults to check=True/text=True.
    A working existing driver is retained. Otherwise exact running headers are
    used; stale fresh-image kernels are advanced to the Pi OS packaged kernel
    and must be rebooted before calling this function again.
    """
    if run(["modinfo", "cec-gpio"], check=False, stdout=subprocess.PIPE,
           stderr=subprocess.PIPE).returncode == 0:
        run(["modprobe", "cec-gpio"])
        return False
    release = platform.release()
    kernel_series(release)
    build = _MODULE_ROOT / release / "build"
    run(["apt-get", "install", "--yes", "--no-install-recommends", "dkms", "build-essential"])
    if not (build / "Makefile").is_file():
        package = "linux-headers-" + release
        available = run(["apt-cache", "show", package], check=False,
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if available.returncode:
            run(["apt-get", "install", "--yes", "--no-install-recommends",
                 "linux-image-rpi-2712", "linux-headers-rpi-2712"])
            return True
        run(["apt-get", "install", "--yes", "--no-install-recommends", package])
    configuration = (build / ".config").read_text()
    if not re.search(r"^CONFIG_CEC_CORE=[my]$", configuration, re.MULTILINE):
        raise RuntimeError("The running kernel lacks the stock CEC core; install Raspberry Pi OS 64-bit Trixie")
    symbols = (build / "Module.symvers").read_text()
    if "\tcec_allocate_adapter\t" not in symbols:
        raise RuntimeError("Running kernel headers do not export the stock CEC core ABI")
    _install_sources()
    status = run(["dkms", "status", "-m", _NAME, "-v", _VERSION], stdout=subprocess.PIPE)
    if not status.stdout.strip():
        run(["dkms", "add", "-m", _NAME, "-v", _VERSION])
    run(["dkms", "build", "-m", _NAME, "-v", _VERSION, "-k", release])
    run(["dkms", "install", "-m", _NAME, "-v", _VERSION, "-k", release])
    run(["depmod", "-a", release])
    run(["modprobe", "cec-gpio"])
    return False
