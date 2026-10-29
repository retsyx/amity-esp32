"""Linux address allocation/version rules without touching a CEC adapter."""

import errno
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from amity_harness.cec import CecError, CecSession, LinuxCecBackend


def device(logical, allocation_type, primary=4, physical=0x1100):
    return SimpleNamespace(address=SimpleNamespace(logical=logical, physical=physical),
                           logical_address_type=allocation_type, device_type=primary,
                           all_device_types={0: 0x80, 1: 0x40, 3: 0x20, 4: 0x10,
                                             5: 0x08, 6: 0x04, 7: 0x04}.get(primary, 0))


class Kernel:
    """Model the ioctl's allocation constraints and driver-selected addresses."""

    pools = {0: [0], 1: [1, 2, 9], 2: [3, 6, 7, 10], 3: [4, 8, 11], 4: [5], 5: [14]}

    def __init__(self, occupied=()):
        self.occupied = set(occupied)
        self.requests, self.clears = [], 0
        self.fail = False

    def ioctl(self, fd, direction, number, value):
        if number == 2:
            return value
        if number != 4:
            raise AssertionError(number)
        if not value.num_log_addrs:
            self.clears += 1
            return value
        count = value.num_log_addrs
        types = list(value.log_addr_type[:count])
        primary = list(value.primary_device_type[:count])
        self.requests.append({"version": value.cec_version, "types": types, "primary": primary})
        if self.fail:
            raise OSError(errno.EINVAL, "scripted kernel rejection")
        if (count > 4 or len(set(types)) != count or {1, 3} <= set(types)
                or any(p not in (0, 1, 3, 4, 5, 6, 7) for p in primary)):
            raise OSError(errno.EINVAL, "invalid logical-address request")
        if value.cec_version == 6:
            if count > 2 or (count == 2 and not (set(types) & {0, 4} and set(types) & {1, 3})):
                raise OSError(errno.EINVAL, "CEC 2.0 address combination")
        elif value.cec_version != 5:
            raise OSError(errno.EINVAL, "unsupported CEC version")
        used = set(self.occupied)
        for index, allocation_type in enumerate(types):
            logical = next((address for address in self.pools[allocation_type] if address not in used), 255)
            value.log_addr[index] = logical
            if logical != 255:
                value.log_addr_mask |= 1 << logical
                used.add(logical)
        return value


class LinuxBackendTests(unittest.TestCase):
    def setUp(self):
        self.backend = LinuxCecBackend(24)
        self.backend.fd = 123
        self.backend.caps = SimpleNamespace(available_log_addrs=4)
        self.kernel = Kernel()

    def configure(self, components):
        with patch("amity_harness.cec._ioctl", side_effect=self.kernel.ioctl):
            self.backend.configure(components)

    def test_existing_one_address_and_audio_playback_pair_keep_cec_20(self):
        for components in ([device(4, 3)], [device(5, 4, 5), device(4, 3)],
                           [device(0, 0, 0, 0), device(4, 3)]):
            with self.subTest(count=len(components)):
                self.configure(components)
                self.assertEqual(self.backend.cec_version, 6)
                self.assertEqual(self.kernel.requests[-1]["version"], 6)

    def test_audio_and_two_sources_use_14_without_rewriting_primary_types(self):
        self.configure([device(5, 4, 5), device(4, 3), device(3, 2)])
        self.assertEqual(self.backend.cec_version, 5)
        self.assertEqual(self.kernel.requests, [{"version": 5, "types": [4, 3, 2],
                                               "primary": [5, 4, 4]}])

    def test_non_20_pair_uses_14_and_four_distinct_types_fit_ioctl(self):
        self.configure([device(4, 3), device(3, 2)])
        self.assertEqual(self.backend.cec_version, 5)
        self.configure([device(0, 0, 0, 0), device(5, 4, 5), device(4, 3), device(3, 2)])
        self.assertEqual(self.kernel.requests[-1]["version"], 5)
        self.assertEqual(len(self.kernel.requests[-1]["types"]), 4)

    def test_duplicate_record_playback_and_invalid_primary_reject_before_ioctl(self):
        for components in ([device(4, 3), device(8, 3)], [device(1, 1), device(4, 3)],
                           [device(3, 2, 2)]):
            with self.subTest(components=components), patch("amity_harness.cec._ioctl") as ioctl:
                with self.assertRaises(CecError):
                    self.backend.configure(components)
                ioctl.assert_not_called()

    def test_driver_capacity_and_four_address_abi_limit_are_both_enforced(self):
        components = [device(5, 4, 5), device(4, 3), device(3, 2)]
        self.backend.caps.available_log_addrs = 2
        with patch("amity_harness.cec._ioctl") as ioctl:
            with self.assertRaises(CecError):
                self.backend.configure(components)
            ioctl.assert_not_called()
        self.backend.caps.available_log_addrs = 8
        with patch("amity_harness.cec._ioctl") as ioctl:
            with self.assertRaises(CecError):
                self.backend.configure([*components, device(0, 0, 0, 0), device(14, 5, 7)])
            ioctl.assert_not_called()

    def test_wrong_allocated_address_is_cleared_and_never_reported_configured(self):
        self.kernel.occupied.add(4)
        with self.assertRaisesRegex(CecError, "claimed"):
            self.configure([device(4, 3)])
        self.assertEqual(self.kernel.clears, 2)
        self.assertIsNone(self.backend.cec_version)

    def test_kernel_error_does_not_trigger_a_version_or_type_retry(self):
        self.kernel.fail = True
        with self.assertRaises(OSError):
            self.configure([device(4, 3)])
        self.assertEqual(self.kernel.requests, [{"version": 6, "types": [3], "primary": [4]}])
        self.assertIsNone(self.backend.cec_version)

    def test_clear_close_and_closed_adapter_have_no_configured_version(self):
        self.configure([device(4, 3)])
        self.configure([])
        self.assertIsNone(self.backend.cec_version)
        self.configure([device(4, 3)])
        with patch("amity_harness.cec._ioctl", side_effect=self.kernel.ioctl), patch("os.close") as close:
            self.backend.close()
            close.assert_called_once_with(123)
        self.assertIsNone(self.backend.cec_version)
        with self.assertRaises(CecError):
            self.backend.configure([])

    def test_close_attempts_both_adapter_resets_and_fd_close_after_failures(self):
        for failing in ((4,), (2,), ("close",), (4, 2, "close")):
            with self.subTest(failing=failing):
                backend = LinuxCecBackend(24)
                backend.fd, backend.cec_version = 123, 5
                calls = []
                errors = {step: OSError(f"failed {step}") for step in failing}

                def ioctl(fd, direction, number, value):
                    self.assertEqual(fd, 123)
                    calls.append(number)
                    if number == 4:
                        self.assertEqual(value.num_log_addrs, 0)
                    else:
                        self.assertEqual(value.value, 0xffff)
                    if number in errors:
                        raise errors[number]
                    return value

                def close(fd):
                    self.assertEqual(fd, 123)
                    calls.append("close")
                    if "close" in errors:
                        raise errors["close"]

                with patch("amity_harness.cec._ioctl", side_effect=ioctl), \
                     patch("amity_harness.cec.os.close", side_effect=close):
                    with self.assertRaises(OSError) as raised:
                        backend.close()
                    self.assertIs(raised.exception, errors[failing[0]])
                    self.assertEqual(calls, [4, 2, "close"])
                    self.assertIsNone(backend.fd)
                    self.assertIsNone(backend.cec_version)
                    backend.close()
                    self.assertEqual(calls, [4, 2, "close"])

    def test_session_sets_version_before_announcing_and_updates_after_power_changes(self):
        announced = []
        first = SimpleNamespace(gpio=24, power=SimpleNamespace(value="standby"), cec_version=6)
        second = SimpleNamespace(gpio=24, power=SimpleNamespace(value="standby"), cec_version=6)
        first.announce = lambda: announced.append(("first", first.cec_version))
        second.announce = lambda: announced.append(("second", second.cec_version))
        session = CecSession([first, second])
        backend = SimpleNamespace(cec_version=5, configure=lambda active: None)
        session._backends[24] = backend
        session._configure(24)
        self.assertEqual(announced, [("first", 5), ("second", 5)])
        first.power.value = "off"
        backend.cec_version = 6
        session._configure(24)
        self.assertEqual(announced[-1], ("second", 6))
        # Lightweight injected backends need no version metadata contract.
        del backend.cec_version
        second.cec_version = 5
        session._configure(24)
        self.assertEqual(announced[-1], ("second", 6))


if __name__ == "__main__":
    unittest.main()
