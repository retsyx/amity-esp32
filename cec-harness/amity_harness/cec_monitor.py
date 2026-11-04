"""Open one read-only CEC monitor, passing no elevated responder authority."""

import argparse
import array
import ctypes
import fcntl
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import tempfile


class _Caps(ctypes.Structure):
    # Stable Linux include/uapi/linux/cec.h CEC_ADAP_G_CAPS layout.
    _fields_ = [("driver", ctypes.c_char * 32), ("name", ctypes.c_char * 32),
                ("available_log_addrs", ctypes.c_uint32), ("capabilities", ctypes.c_uint32),
                ("version", ctypes.c_uint32)]


def _arguments(device, gpio):
    if (not isinstance(device, str) or re.fullmatch(r"/dev/cec[0-9]+", device) is None
            or type(gpio) is not int or not 0 <= gpio <= 255):
        raise ValueError("Use one exact CEC GPIO device")


def _check(fd, gpio):
    data = bytearray(ctypes.sizeof(_Caps))
    fcntl.ioctl(fd, 0xc04c6100, data, True)
    caps = _Caps.from_buffer_copy(data)
    if caps.driver != b"cec-gpio" or caps.name != ("cec@" + str(gpio)).encode() or not caps.capabilities & (1 << 7):
        raise ValueError("The descriptor is not the selected monitor-capable CEC GPIO")


def _open(device, gpio):
    _arguments(device, gpio)
    fd = os.open(device, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        _check(fd, gpio)
        mode = bytearray(bytes(ctypes.c_uint32(0xd0)))
        fcntl.ioctl(fd, 0x40046109, mode, True)  # CEC_S_MODE / MONITOR_PIN only.
        return fd
    except BaseException:
        os.close(fd)
        raise


def privileged_monitor_fd(device, gpio):
    """A short sudo helper passes its monitor FD, then immediately exits."""
    _arguments(device, gpio)
    with tempfile.TemporaryDirectory(prefix="amity-cec-monitor-") as directory:
        address = str(Path(directory) / "fd.sock")
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
            server.bind(address);server.listen(1);server.settimeout(5)
            process = subprocess.Popen(["sudo", "-n", sys.executable, "-u", str(Path(__file__).resolve()),
                                        "--device", device, "--gpio", str(gpio), "--socket", address],
                                       stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            fd = None
            try:
                with server.accept()[0] as connection:
                    connection.settimeout(5)
                    marker, ancillary, flags, _ = connection.recvmsg(1, socket.CMSG_SPACE(array.array("i").itemsize),
                                                                      getattr(socket, "MSG_CMSG_CLOEXEC", 0))
                    descriptors = []
                    for level, kind, data in ancillary:
                        if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                            values = array.array("i");values.frombytes(data)
                            descriptors.extend(values)
                    if marker != b"M" or flags & socket.MSG_CTRUNC or len(descriptors) != 1:
                        for extra in descriptors:
                            os.close(extra)
                        raise RuntimeError("The CEC monitor helper returned no unique descriptor")
                    fd = descriptors[0]
                    os.set_inheritable(fd, False)
                    _check(fd, gpio)
                if process.wait(timeout=5) != 0:
                    raise RuntimeError("The CEC monitor helper could not open its selected wire")
                result, fd = fd, None
                return result
            finally:
                if fd is not None:
                    os.close(fd)
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        process.kill();process.wait(timeout=2)


def _main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True)
    parser.add_argument("--gpio", type=int, required=True)
    parser.add_argument("--socket", required=True)
    arguments = parser.parse_args()
    _arguments(arguments.device, arguments.gpio)
    if not os.path.isabs(arguments.socket) or len(arguments.socket.encode()) > 107:
        raise ValueError("Use the caller's bounded private Unix socket")
    uid, gid = os.environ.get("SUDO_UID", ""), os.environ.get("SUDO_GID", "")
    if os.geteuid() != 0 or not uid.isdecimal() or not gid.isdecimal() or int(uid) == 0:
        raise ValueError("Run only as the invoking unprivileged user's sudo helper")
    fd = _open(arguments.device, arguments.gpio)
    try:
        os.setgroups([]);os.setgid(int(gid));os.setuid(int(uid))
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
            connection.settimeout(5);connection.connect(arguments.socket)
            connection.sendmsg([b"M"], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array("i", [fd]))])
    finally:
        os.close(fd)


if __name__ == "__main__":
    _main()
