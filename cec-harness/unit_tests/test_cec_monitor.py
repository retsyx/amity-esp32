"""The privileged helper hands off only a validated read-only monitor FD."""

import array
import ctypes
import os
from pathlib import Path
import socket
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

from amity_harness import cec_monitor
from unit_tests.harness_pins import cec_gpio


class CecMonitorTests(unittest.TestCase):
    def test_helper_opens_exact_read_only_gpio_and_only_changes_its_monitor_mode(self):
        def ioctl(fd, command, value, mutate):
            self.assertEqual(fd, 42)
            if command == 0xc04c6100:
                caps = cec_monitor._Caps();caps.driver = b"cec-gpio"
                caps.name = ("cec@" + str(cec_gpio())).encode();caps.capabilities = 0x2bf
                value[:] = bytes(caps)
            else:
                self.assertEqual(command, 0x40046109)
                self.assertEqual(ctypes.c_uint32.from_buffer_copy(value).value, 0xd0)
        with patch("amity_harness.cec_monitor.os.open", return_value=42) as opened, \
             patch("amity_harness.cec_monitor.fcntl.ioctl", side_effect=ioctl), \
             patch("amity_harness.cec_monitor.os.close") as closed:
            self.assertEqual(cec_monitor._open("/dev/cec12", cec_gpio()), 42)
            opened.assert_called_once_with("/dev/cec12", os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK)
            closed.assert_not_called()

    def test_wrong_device_or_capability_closes_descriptor_without_setting_mode(self):
        for driver, name, capabilities in ((b"vc4_hdmi", b"hdmi0", 0x2bf),
                                            (b"cec-gpio", b"cec@255", 0x2bf),
                                            (b"cec-gpio", ("cec@" + str(cec_gpio())).encode(), 0x0f)):
            with self.subTest(driver=driver, name=name, capabilities=capabilities):
                def ioctl(fd, command, value, mutate):
                    self.assertEqual(command, 0xc04c6100)
                    caps = cec_monitor._Caps();caps.driver = driver;caps.name = name;caps.capabilities = capabilities
                    value[:] = bytes(caps)
                with patch("amity_harness.cec_monitor.os.open", return_value=42), \
                     patch("amity_harness.cec_monitor.fcntl.ioctl", side_effect=ioctl), \
                     patch("amity_harness.cec_monitor.os.close") as closed, self.assertRaises(ValueError):
                    cec_monitor._open("/dev/cec12", cec_gpio())
                closed.assert_called_once_with(42)
        for device in ("/etc/passwd", "/dev/cec12/../secret", "cec12", "/dev/cec12\n"):
            with patch("amity_harness.cec_monitor.os.open") as opened, self.assertRaises(ValueError):
                cec_monitor._open(device, cec_gpio())
            opened.assert_not_called()

    def test_real_unix_fd_handoff_is_validated_noninheritable_and_helper_exits(self):
        threads, commands, errors = [], [], []
        parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
        server = Mock();server.__enter__ = Mock(return_value=server);server.__exit__ = Mock(return_value=False)
        server.accept.return_value = (parent, None)
        def launch(command, **kwargs):
            commands.append(command)
            read_fd, write_fd = os.pipe();os.close(write_fd)
            def send():
                try:
                    with child:
                        child.sendmsg([b"M"], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array("i", [read_fd]))])
                except BaseException as error:
                    errors.append(error)
                finally:
                    os.close(read_fd)
            thread = threading.Thread(target=send);threads.append(thread);thread.start()
            def wait(timeout):
                thread.join(timeout)
                self.assertFalse(thread.is_alive())
                if errors:
                    raise errors[0]
                return 0
            return SimpleNamespace(wait=wait, poll=lambda: None if thread.is_alive() else 0,
                                   terminate=lambda: None, kill=lambda: None)
        with patch("amity_harness.cec_monitor.subprocess.Popen", side_effect=launch), \
             patch("amity_harness.cec_monitor._check") as checked, \
             patch("amity_harness.cec_monitor.socket.socket", return_value=server):
            fd = cec_monitor.privileged_monitor_fd("/dev/cec12", cec_gpio())
            try:
                self.assertFalse(os.get_inheritable(fd))
                checked.assert_called_once_with(fd, cec_gpio())
                self.assertEqual(commands[0][:2], ["sudo", "-n"])
                self.assertEqual(Path(commands[0][4]).name, "cec_monitor.py")
                self.assertEqual(commands[0][commands[0].index("--device") + 1], "/dev/cec12")
            finally:
                os.close(fd)

    def test_privileged_process_drops_groups_gid_uid_before_transferring_fd(self):
        order = []
        client = Mock()
        client.__enter__ = Mock(return_value=client);client.__exit__ = Mock(return_value=False)
        client.sendmsg.side_effect = lambda *args: order.append("transfer")
        with patch("sys.argv", ["cec_monitor.py", "--device", "/dev/cec12", "--gpio", str(cec_gpio()), "--socket", "/tmp/private/fd.sock"]), \
             patch.dict(os.environ, {"SUDO_UID": "1000", "SUDO_GID": "1001"}), \
             patch("amity_harness.cec_monitor.os.geteuid", return_value=0), \
             patch("amity_harness.cec_monitor._open", return_value=42), \
             patch("amity_harness.cec_monitor.os.setgroups", side_effect=lambda value: order.append(("groups", value))), \
             patch("amity_harness.cec_monitor.os.setgid", side_effect=lambda value: order.append(("gid", value))), \
             patch("amity_harness.cec_monitor.os.setuid", side_effect=lambda value: order.append(("uid", value))), \
             patch("amity_harness.cec_monitor.socket.socket", return_value=client), \
             patch("amity_harness.cec_monitor.os.close") as closed:
            cec_monitor._main()
        self.assertEqual(order, [("groups", []), ("gid", 1001), ("uid", 1000), "transfer"])
        closed.assert_called_once_with(42)


if __name__ == "__main__":
    unittest.main()
