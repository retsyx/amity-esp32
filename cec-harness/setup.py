#!/usr/bin/env python3
"""Provision a Pi 5 harness locally, or deploy and provision it over SSH."""

import argparse
import asyncio
import ctypes
import fcntl
import glob
import hashlib
import inspect
import io
import json
import os
from pathlib import Path
import platform
import pwd
import re
import shlex
import subprocess
import sys
import tarfile
import tempfile
import time

from amity_harness.pi_boot import boot_config, overlay_source
from amity_harness.wiring import Wiring


HARNESS = Path(__file__).resolve().parent
BOOT = Path("/boot/firmware")
PACKAGES = (
    "ca-certificates", "device-tree-compiler", "raspi-utils", "python3-dbus-next", "python3-venv", "chromium",
    "bluez", "rfkill", "avahi-daemon", "libnss-mdns", "openssh-server", "sudo",
    "util-linux", "procps", "kmod", "libglib2.0-0t64", "libnl-3-200", "libnl-route-3-200", "libatomic1",
)
REBOOT_REQUIRED = 2
OVERLAY_RECORD = Path("/var/lib/amity-harness/overlay.json")
REBOOT_RECORD = Path("/var/lib/amity-harness/reboot-required")
BOOT_ID = Path("/proc/sys/kernel/random/boot_id")


def run(command, **kwargs):
    if command[0] == "apt-get":
        command = [command[0], "-o", "DPkg::Lock::Timeout=120", *command[1:]]
        kwargs["env"] = {**os.environ, **kwargs.get("env", {}), "DEBIAN_FRONTEND": "noninteractive"}
    return subprocess.run(command, check=kwargs.pop("check", True), text=True, **kwargs)


def validate_platform():
    if (platform.system() != "Linux" or platform.machine() != "aarch64"
            or sys.version_info[:2] != (3, 13)):
        raise ValueError("Use Raspberry Pi OS 64-bit Trixie with Python 3.13 on a Pi 5")
    release = platform.freedesktop_os_release()
    if release.get("VERSION_CODENAME") != "trixie" or release.get("ID") not in {"debian", "raspbian"}:
        raise ValueError("This installer supports Raspberry Pi OS Trixie")
    model = Path("/proc/device-tree/model").read_text().rstrip("\x00")
    if not model.startswith("Raspberry Pi 5") or not (BOOT / "config.txt").is_file():
        raise ValueError("This installer requires a Raspberry Pi 5 and /boot/firmware/config.txt")


def account_for(name=None):
    name = name or os.environ.get("SUDO_USER") or pwd.getpwuid(os.getuid()).pw_name
    account = pwd.getpwnam(name)
    if account.pw_uid == 0 or not re.fullmatch(r"[a-z_][a-z0-9_-]*", name):
        raise ValueError("Run with sudo from the harness account, or supply --user ACCOUNT")
    return account


def as_user(account, command, **kwargs):
    environment = ["env", "HOME=" + account.pw_dir, "USER=" + account.pw_name,
                   "LOGNAME=" + account.pw_name, "SUDO_USER=", "PYTHONDONTWRITEBYTECODE=1",
                   "PYTHONPATH=" + str(HARNESS)]
    prefix = ["runuser", "-u", account.pw_name, "--"] if os.geteuid() == 0 else []
    return run([*prefix, *environment, *command], **kwargs)


def write_file(path, content, *, mode=0o644, backup=False):
    """Replace changed files atomically; retain the original boot configuration."""
    path = Path(path)
    data = content.encode() if isinstance(content, str) else content
    if path.exists() and path.read_bytes() == data:
        if not str(path).startswith(str(BOOT) + "/"):
            os.chmod(path, mode)
        return False
    if backup and path.exists():
        original = path.with_name(path.name + ".before-amity-harness")
        if not original.exists():
            original.write_bytes(path.read_bytes())
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".amity-", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        # The boot partition is FAT and has mount-wide permissions.
        if not str(path).startswith(str(BOOT) + "/"):
            os.chmod(temporary, mode)
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)
    return True


def awaiting_reboot():
    return REBOOT_RECORD.exists() and REBOOT_RECORD.read_text().strip() == BOOT_ID.read_text().strip()


def retire_followers():
    def listed(command):
        result = run(command, check=False, capture_output=True)
        # systemctl returns 1 when these legacy-unit patterns match nothing.
        # Diagnostics or any other failure still indicate a broken setup.
        if result.returncode and not (result.returncode == 1 and not result.stdout.strip() and not result.stderr.strip()):
            raise subprocess.CalledProcessError(result.returncode, command,
                                                output=result.stdout, stderr=result.stderr)
        return [line.split()[0] for line in result.stdout.splitlines() if line.strip()]

    units = listed(["systemctl", "list-unit-files", "--no-legend", "--no-pager",
                    "amity-cec@*.service", "amity-setup-gpios.service"])
    instances = [unit for unit in units if unit != "amity-cec@.service"]
    # Also catch transient or deleted-but-still-running follower instances.
    instances += listed(["systemctl", "list-units", "--all", "--plain", "--no-legend", "--no-pager",
                         "amity-cec@*.service"])
    for unit in sorted(set(instances)):
        if re.fullmatch(r"amity-cec@[A-Za-z0-9_.-]+\.service|amity-setup-gpios\.service", unit):
            run(["systemctl", "stop", unit])
            if unit in units:
                run(["systemctl", "disable", unit])


def monitor_helper():
    """The runtime Python and CEC pin-monitor helper sudo runs for scenarios."""
    command = (HARNESS.parent / ".harness-python/bin/python", HARNESS / "amity_harness/cec_monitor.py")
    if any(re.fullmatch(r"[A-Za-z0-9/._-]+", str(path)) is None for path in command):
        raise ValueError("Use a harness directory path without spaces or sudoers special characters")
    return tuple(str(path) for path in command)


def configure_access(account):
    for group in ("video", "gpio", "bluetooth", "dialout"):
        run(["groupadd", "--force", group])
    run(["usermod", "-a", "-G", "video,gpio,bluetooth,dialout", account.pw_name])
    write_file("/etc/udev/rules.d/70-amity-harness.rules",
               '# Harness access; scenarios own addresses only while running.\n'
               'SUBSYSTEM=="cec", KERNEL=="cec[0-9]*", GROUP="video", MODE="0660"\n'
               'KERNEL=="gpiomem*", GROUP="gpio", MODE="0660"\n'
               'SUBSYSTEM=="tty", ATTRS{idVendor}=="303a", ATTRS{idProduct}=="1001", '
               'GROUP="dialout", MODE="0660"\n')
    # These are the existing enrollment worker, CEC pin monitor and optional
    # status LED operations.
    python, monitor = monitor_helper()
    sudoers = (f"{account.pw_name} ALL=(root) NOPASSWD: /usr/bin/unshare, /usr/bin/kill, "
               f"{python} -u {monitor} --device /dev/cec* --gpio * --socket *, "
               "/usr/bin/tee /sys/class/leds/ACT/trigger, "
               "/usr/bin/tee /sys/class/leds/ACT/brightness, "
               "/usr/bin/tee /sys/class/leds/ACT/delay_on, "
               "/usr/bin/tee /sys/class/leds/ACT/delay_off, "
               "/usr/bin/tee /sys/class/leds/PWR/trigger, "
               "/usr/bin/tee /sys/class/leds/PWR/brightness, "
               "/usr/bin/tee /sys/class/leds/PWR/delay_on, "
               "/usr/bin/tee /sys/class/leds/PWR/delay_off\n")
    with tempfile.NamedTemporaryFile(mode="w", prefix="amity-sudoers-") as temporary:
        temporary.write(sudoers)
        temporary.flush()
        run(["visudo", "--check", "--file", temporary.name], capture_output=True)
        write_file("/etc/sudoers.d/amity-harness", sudoers, mode=0o440)
    run(["udevadm", "control", "--reload-rules"])
    run(["udevadm", "trigger", "--subsystem-match=cec"])
    run(["udevadm", "trigger", "--subsystem-match=bcm2835-gpiomem"])
    run(["udevadm", "trigger", "--subsystem-match=tty"])
    run(["udevadm", "settle"])


def retire_split_host_sources():
    """Remove obsolete managed sources left by an older overlay deployment."""
    for relative in ("amity_harness/remote.py", "unit_tests/test_remote.py"):
        (HARNESS / relative).unlink(missing_ok=True)


def install(account, wiring):
    # Render/validate all wiring and the existing boot file before any mutation.
    source = overlay_source(wiring)
    config_path = BOOT / "config.txt"
    configuration = boot_config(config_path.read_text(), wiring)
    print("Installing Pi harness packages", flush=True)
    run(["apt-get", "update"])
    run(["apt-get", "install", "--yes", "--no-install-recommends", *PACKAGES],
        env={**os.environ, "DEBIAN_FRONTEND": "noninteractive"})
    from amity_harness.pi_cec_driver import ensure_driver
    if ensure_driver(run):
        print("Kernel headers require a reboot; rerun setup afterward.", flush=True)
        return REBOOT_REQUIRED
    retire_followers()
    retire_split_host_sources()
    configure_access(account)
    with tempfile.TemporaryDirectory(prefix="amity-overlay-") as directory:
        dts, dtbo = Path(directory) / "harness.dts", Path(directory) / "harness.dtbo"
        dts.write_text(source)
        run(["dtc", "-@", "-I", "dts", "-O", "dtb", "-o", str(dtbo), str(dts)])
        binary = dtbo.read_bytes()
        changed = write_file(BOOT / "overlays/amity-harness.dtbo", binary)
        write_file(OVERLAY_RECORD, json.dumps({"source": hashlib.sha256(source.encode()).hexdigest(),
                                             "binary": hashlib.sha256(binary).hexdigest()}) + "\n")
    changed = write_file(config_path, configuration, backup=True) or changed
    if changed:
        write_file(REBOOT_RECORD, BOOT_ID.read_text())
    write_file("/etc/sysctl.d/70-amity-harness.conf",
               "# Matter commissioning requires IPv6.\n"
               "net.ipv6.conf.all.disable_ipv6=0\nnet.ipv6.conf.default.disable_ipv6=0\n")
    run(["sysctl", "-w", "net.ipv6.conf.all.disable_ipv6=0", "net.ipv6.conf.default.disable_ipv6=0"])
    run(["systemctl", "enable", "--now", "bluetooth.service", "avahi-daemon.service", "ssh.service"])
    run(["rfkill", "unblock", "bluetooth"])
    run(["bluetoothctl", "--timeout", "15", "power", "on"], timeout=20)
    # Refresh supplementary groups through runuser instead of requiring a new login.
    for gpio in sorted(wiring.reset.values()):
        as_user(account, ["pinctrl", "set", str(gpio), "ip", "pn"])
    print("Installing the standalone functional-test runtime", flush=True)
    as_user(account, [sys.executable, "-B", str(HARNESS / "setup_runtime.py")])
    print("Installing and verifying the pinned Matter controller", flush=True)
    as_user(account, [sys.executable, "-B", str(HARNESS / "setup_matter.py")])
    if awaiting_reboot():
        print("Installation complete; a reboot must apply the CEC wiring.", flush=True)
        return REBOOT_REQUIRED
    as_user(account, [sys.executable, "-B", str(HARNESS / "setup.py"), "--check"])
    REBOOT_RECORD.unlink(missing_ok=True)
    return 0


def check_cec(wiring):
    from amity_harness.cec import _Caps, _ioctl
    expected = {f"cec@{gpio}" for gpio in wiring.cec.values()}
    found = set()
    for path in sorted(glob.glob("/dev/cec*")):
        fd = os.open(path, os.O_RDWR | os.O_CLOEXEC)
        try:
            caps = _ioctl(fd, 3, 0, _Caps())
            if caps.driver.decode() != "cec-gpio":
                continue
            name = caps.name.decode()
            if name not in expected or name in found:
                raise ValueError("Unexpected or duplicate GPIO CEC adapter: " + name)
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            # The kernel exclusive mode also catches non-framework followers.
            # Closing this descriptor relinquishes it without claiming addresses.
            _ioctl(fd, 1, 9, ctypes.c_uint32(0x32))
            if caps.capabilities & 0x0f != 0x0f or caps.available_log_addrs < 4:
                raise ValueError("CEC adapter lacks required capabilities: " + name)
            found.add(name)
        finally:
            os.close(fd)
    if found != expected:
        raise ValueError("CEC adapters missing; reboot after setup: " + ", ".join(sorted(expected - found)))


async def check_bluetooth():
    from dbus_next import BusType, Message, MessageType
    from dbus_next.aio import MessageBus
    bus = await MessageBus(bus_type=BusType.SYSTEM).connect()
    try:
        reply = await bus.call(Message(destination="org.bluez", path="/",
                                       interface="org.freedesktop.DBus.ObjectManager",
                                       member="GetManagedObjects"))
        if reply.message_type == MessageType.ERROR:
            raise ValueError("Cannot inspect BlueZ: " + str(reply.error_name))
        adapter = reply.body[0].get("/org/bluez/hci0", {})
        if (not adapter.get("org.bluez.Adapter1", {}).get("Powered")
                or not adapter["org.bluez.Adapter1"]["Powered"].value
                or "org.bluez.GattManager1" not in adapter
                or "org.bluez.LEAdvertisingManager1" not in adapter):
            raise ValueError("hci0 must be powered and support BLE GATT and advertising")
    finally:
        bus.disconnect()


def check_ready(wiring):
    """Read hardware/software state without transmitting, pairing or pressing GPIOs."""
    if os.geteuid() == 0:
        raise ValueError("Readiness must run as the normal harness account")
    if awaiting_reboot():
        raise ValueError("Reboot required to apply the installed CEC wiring")
    source = overlay_source(wiring)
    configuration = (BOOT / "config.txt").read_text()
    if boot_config(configuration, wiring) != configuration:
        raise ValueError("Saved boot wiring differs from TEST-HARNESS.md; rerun setup")
    expected = {"source": hashlib.sha256(source.encode()).hexdigest(),
                "binary": hashlib.sha256((BOOT / "overlays/amity-harness.dtbo").read_bytes()).hexdigest()}
    if json.loads(OVERLAY_RECORD.read_text()) != expected:
        raise ValueError("Installed CEC overlay differs from the wiring; rerun setup")
    check_cec(wiring)
    run([sys.executable, "-B", str(HARNESS / "setup_runtime.py"), "--verify-installed"])
    runtime_python = HARNESS.parent / ".harness-python/bin/python"
    run([str(runtime_python), "-B", "-m", "amity_harness.usb", "--check", "--wiring", str(wiring.source)],
        env={**os.environ, "PYTHONPATH": str(HARNESS)})
    for gpio in sorted(wiring.reset.values()):
        state = run(["pinctrl", "get", str(gpio)], capture_output=True).stdout
        if not re.search(r"\bip\b", state) or not re.search(r"\bpn\b", state):
            raise ValueError(f"Reset GPIO {gpio} must be input with no pulls; rerun setup")
    memories = glob.glob("/dev/gpiomem*")
    if not memories or not all(os.access(path, os.R_OK | os.W_OK) for path in memories):
        raise ValueError("Harness account cannot access GPIO memory; rerun setup or reconnect SSH")
    asyncio.run(asyncio.wait_for(check_bluetooth(), timeout=15))
    run([sys.executable, "-B", str(HARNESS / "setup_matter.py"), "--verify-installed"])
    ctypes.CDLL(str(HARNESS.parent / ".matter-python/chip/_ChipDeviceCtrl.so"))
    run(["sudo", "-n", "unshare", "--mount", "--propagation", "private", "true"], capture_output=True)
    python, monitor = monitor_helper()
    run(["sudo", "-n", "-l", python, "-u", monitor, "--device", "/dev/cec0", "--gpio", "0",
         "--socket", "/tmp/amity-cec-monitor.sock"], capture_output=True)
    for service in ("bluetooth", "avahi-daemon", "ssh"):
        run(["systemctl", "is-active", "--quiet", service])
    if not any(line.split()[-1] != "lo" for line in Path("/proc/net/if_inet6").read_text().splitlines() if line.strip()):
        raise ValueError("A network interface needs IPv6 for Matter commissioning")
    print(f"Harness ready: {len(wiring.cec)} CEC wires, {len(wiring.reset)} reset inputs, "
          f"{len(wiring.identities)} USB DUTs, BLE, Matter and browser verified.")


def ssh_command(host, port=None, *, bootstrap=False):
    if not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.@:\[\]-]*", host):
        raise ValueError("Invalid SSH destination")
    return ["ssh", "-T", "-o", "BatchMode=yes", "-o",
            "StrictHostKeyChecking=" + ("accept-new" if bootstrap else "yes"),
            "-o", "ConnectTimeout=10", "-o", "ServerAliveInterval=5",
            "-o", "ServerAliveCountMax=3", *(["-p", str(port)] if port else []), host]


def source_archive():
    """Deploy harness sources only; never ship the checkout's credentials or captures."""
    archive = io.BytesIO()
    with tarfile.open(fileobj=archive, mode="w:gz") as output:
        top_level = {"setup.py", "setup_matter.py", "matter-requirements.txt", "setup_runtime.py",
                     "runtime-requirements.txt", "runtime-constraints.txt", "run", "run.py",
                     "fw-reset", "flash-duts", "README.md", "TEST-HARNESS.md", "DIAGNOSTICS.md"}
        packages = {"amity_harness", "functional_tests", "unit_tests"}
        for path in sorted(HARNESS.rglob("*")):
            relative = path.relative_to(HARNESS)
            source = ((len(relative.parts) == 1 and path.name in top_level)
                      or (len(relative.parts) == 2 and relative.parts[0] in packages
                          and path.suffix == ".py"))
            if source and path.is_file() and not path.is_symlink():
                output.add(path, arcname=str(Path("cec-harness") / path.relative_to(HARNESS)))
    return archive.getvalue()


def remote(args):
    ssh = ssh_command(args.host, args.ssh_port, bootstrap=not args.check)
    preflight = ("import json,platform,sys\nfrom pathlib import Path\n"
                 "BOOT=Path('/boot/firmware')\n" + inspect.getsource(validate_platform)
                 + "\nvalidate_platform()\nprint(json.dumps({'home':str(Path.home()), "
                 "'boot':Path('/proc/sys/kernel/random/boot_id').read_text().strip()}))")
    info = run([*ssh, shlex.join(["python3", "-c", preflight])], capture_output=True)
    state = json.loads(info.stdout)
    root = args.root or state["home"] + "/amity/esp32"
    if not root.startswith("/") or any(character in root for character in "\x00\r\n"):
        raise ValueError("--root must be an absolute Pi repository directory")
    command = ["python3", "-B", root + "/cec-harness/setup.py"]
    if args.check:
        return run([*ssh, shlex.join([*command, "--check"])], check=False).returncode
    # Establish admin authority before copying any sources.
    run([*ssh, "sudo -n true"])
    print(f"Deploying harness sources to {args.host}:{root}", flush=True)
    extract = "mkdir -p " + shlex.quote(root) + " && tar -xzf - --no-same-owner -C " + shlex.quote(root)
    subprocess.run([*ssh, extract], input=source_archive(), check=True)
    # An old image can need one reboot for matching kernel headers and another
    # for the generated CEC overlay. Resume the entire idempotent installation.
    for stage in range(3):
        result = run([*ssh, shlex.join(["sudo", "-n", *command])], check=False)
        if result.returncode != REBOOT_REQUIRED:
            return result.returncode
        print("Rebooting the Pi; setup will resume when SSH returns", flush=True)
        run([*ssh, "sudo -n reboot"], check=False, capture_output=True)
        deadline = time.monotonic() + args.reboot_timeout
        while time.monotonic() < deadline:
            time.sleep(3)
            probe = run([*ssh, "cat /proc/sys/kernel/random/boot_id"], check=False, capture_output=True)
            if probe.returncode == 0 and probe.stdout.strip() != state["boot"]:
                state["boot"] = probe.stdout.strip()
                break
        else:
            raise ValueError("Pi did not return after reboot; rerun the same setup command once reachable")
    raise ValueError("Setup still requires a reboot after three passes; inspect the reported kernel/boot errors")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", help="SSH destination; copy harness, install, reboot if needed and verify")
    parser.add_argument("--ssh-port", type=int)
    parser.add_argument("--root", help="Pi repository directory (default: SSH user's ~/amity/esp32)")
    parser.add_argument("--user", help="normal harness account for a local root invocation")
    parser.add_argument("--check", action="store_true", help="verify an existing installation without provisioning")
    parser.add_argument("--reboot", action="store_true", help="reboot after a local installation if required")
    parser.add_argument("--reboot-timeout", type=int, default=240)
    args = parser.parse_args(argv)
    try:
        if args.ssh_port is not None and not 1 <= args.ssh_port <= 65535:
            raise ValueError("SSH port must be between 1 and 65535")
        if args.reboot_timeout <= 0:
            raise ValueError("Reboot timeout must be positive")
        if args.host:
            if args.user or args.reboot:
                raise ValueError("--user and --reboot are local options; remote setup handles its own reboot")
            return remote(args)
        if args.root or args.ssh_port:
            raise ValueError("--root and --ssh-port require --host")
        validate_platform()
        wiring = Wiring.from_markdown()
        overlay_source(wiring)
        account = account_for(args.user)
        if args.check:
            if os.geteuid() == 0:
                return as_user(account, [sys.executable, "-B", str(HARNESS / "setup.py"), "--check"]).returncode
            check_ready(wiring)
            return 0
        if os.geteuid() != 0:
            raise ValueError("Run local installation with sudo python3 cec-harness/setup.py")
        result = install(account, wiring)
        if result == REBOOT_REQUIRED:
            if args.reboot:
                run(["systemctl", "reboot"])
            else:
                print("Run sudo reboot, then rerun sudo python3 cec-harness/setup.py; remote setup handles this automatically.")
        return result
    except (OSError, ValueError, RuntimeError, ImportError, subprocess.CalledProcessError, subprocess.TimeoutExpired,
            asyncio.TimeoutError) as error:
        print("Harness setup failed: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
