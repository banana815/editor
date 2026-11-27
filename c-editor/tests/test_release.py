"""Release assembly must fail closed and preserve package bytes and notices."""

import hashlib
import importlib.util
import tempfile
import unittest
from pathlib import Path
from zipfile import ZipFile

spec = importlib.util.spec_from_file_location(
    "prepare_release", Path(__file__).resolve().parents[1] / "tools/prepare_release.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.inputs = self.root / "artifacts"
        self.output = self.root / "release"
        self.cmake = self.root / "CMakeLists.txt"
        self.cmake.write_text("project(NoteLab VERSION 0.3.0 LANGUAGES C CXX)")
        self.paths = (
            "notelab-Linux-X64/NoteLab-0.3.0-Linux-x86_64.tar.gz",
            "notelab-macOS-ARM64/NoteLab-0.3.0-Darwin-arm64.zip",
            "notelab-Windows-X64/NoteLab-0.3.0-Windows-AMD64.zip",
            "notelab-android-unsigned/app-release-unsigned.apk",
            "notelab-ios-device-unsigned/notelab-ios-device-unsigned.tar.gz",
            "notelab-ios-simulator-unsigned/notelab-ios-simulator-unsigned.tar.gz",
            "notelab-browser/notelab.html", "notelab-browser/notelab.js",
            "notelab-browser/notelab.wasm", "notelab-browser/notelab.data",
            "notelab-browser/THIRD_PARTY.md", "notelab-browser/licenses/example/LICENSE.txt",
            "notelab-browser/browser-library-fixture.json",
            "notelab-android-startup/screenshot.png",
        )
        for name in self.paths:
            path = self.inputs / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(name.encode())

    def prepare(self, tag="v0.3.0"):
        release.prepare(self.inputs, self.output, tag, self.cmake)

    def test_complete_release_has_seven_packages_and_verified_checksums(self):
        self.prepare()
        checksums = (self.output / "SHA256SUMS.txt").read_text().splitlines()
        self.assertEqual(7, len(checksums))
        self.assertEqual(8, len(list(self.output.iterdir())))
        for line in checksums:
            digest, name = line.split("  ")
            self.assertEqual(digest, hashlib.sha256((self.output / name).read_bytes()).hexdigest())
        windows = Path(self.paths[2]).name
        self.assertEqual((self.inputs / self.paths[2]).read_bytes(), (self.output / windows).read_bytes())
        with ZipFile(self.output / "NoteLab-0.3.0-Web.zip") as archive:
            self.assertEqual({"notelab.html", "notelab.js", "notelab.wasm", "notelab.data",
                              "THIRD_PARTY.md", "licenses/example/LICENSE.txt"}, set(archive.namelist()))

    def test_missing_platform_or_license_cannot_create_partial_output(self):
        for name in self.paths[:12]:
            with self.subTest(name=name):
                path = self.inputs / name
                data = path.read_bytes()
                path.unlink()
                with self.assertRaises(ValueError):
                    self.prepare()
                self.assertFalse(self.output.exists())
                path.write_bytes(data)

    def test_version_mismatch_and_ambiguous_platform_fail(self):
        with self.assertRaisesRegex(ValueError, "exactly match"):
            self.prepare("v0.2.0")
        duplicate = self.inputs / "notelab-Windows-ARM64/NoteLab-0.3.0-Windows-ARM64.zip"
        duplicate.parent.mkdir()
        duplicate.write_bytes(b"duplicate")
        with self.assertRaisesRegex(ValueError, "Expected one Windows"):
            self.prepare()
        self.assertFalse(self.output.exists())

    def test_does_not_overwrite_or_include_stale_output(self):
        self.output.mkdir()
        existing = self.output / "stale.txt"
        existing.write_bytes(b"keep")
        with self.assertRaisesRegex(ValueError, "must be empty"):
            self.prepare()
        self.assertEqual(b"keep", existing.read_bytes())


if __name__ == "__main__":
    unittest.main()
