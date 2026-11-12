"""Matter SDK installation replaces a changed pin without patching stale files."""

import contextlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
import zipfile

_SPEC = importlib.util.spec_from_file_location("harness_setup_matter",
                                               Path(__file__).resolve().parents[1] / "setup_matter.py")
setup_matter = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(setup_matter)


def native(tag):
    return tag + b"".join(b"/data/chip_" + suffix + b"\0" for suffix in (b"factory.ini", b"config.ini",
                                                                        b"counters.ini"))


class MatterInstallTests(unittest.TestCase):
    def wheel(self, directory, name, files):
        with zipfile.ZipFile(directory / name, "w") as archive:
            for member, data in files.items():
                archive.writestr(member, data)
        return setup_matter.Wheel(name, "chip", "1", "0" * 64)

    def test_changed_pin_replaces_the_whole_installed_sdk(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
            root = Path(directory)
            destination = root / ".matter-python"
            old = self.wheel(root, "old.whl", {setup_matter._NATIVE: native(b"old"), "chip/stale.py": b"stale"})
            setup_matter.install([old], root, destination)
            self.assertTrue((destination / "chip/stale.py").is_file())
            setup_matter.install([old], root, destination)  # Unchanged pins only verify.
            new = self.wheel(root, "new.whl", {setup_matter._NATIVE: native(b"new"), "chip/core.py": b"core"})
            setup_matter.install([new], root, destination)
            setup_matter.verify_installed([new], root, destination)
            self.assertFalse((destination / "chip/stale.py").exists())
            self.assertEqual(sorted(path.name for path in root.iterdir()),
                             [".matter-python", "new.whl", "old.whl"])


if __name__ == "__main__":
    unittest.main()
