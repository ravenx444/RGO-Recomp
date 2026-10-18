"""Exercise actual Git bundle delivery through the Windows setup helper."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(os.name == "nt" and shutil.which("git"), "Windows Git integration")
class TesterSourceBundleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="rgo-bundle-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "reviewed-source"
        self.source.mkdir()
        self.git("init", str(self.source))
        (self.source / "scripts").mkdir()
        (self.source / "scripts/build_from_rom.py").write_text("# synthetic fixture only\n")
        self.git("-C", str(self.source), "add", ".")
        self.commit()
        self.destination = self.root / "portable"
        self.destination.mkdir()
        self.bundle()

    def git(self, *args):
        return subprocess.check_output(["git", *args], text=True, stderr=subprocess.STDOUT).strip()

    def commit(self):
        self.git("-C", str(self.source), "-c", "user.name=Fixture", "-c",
                 "user.email=test@example.invalid", "commit", "-m", "Synthetic source fixture")
        self.revision = self.git("-C", str(self.source), "rev-parse", "HEAD")

    def bundle(self):
        path = self.destination / "source.bundle"
        if path.exists():
            path.unlink()
        self.git("-C", str(self.source), "bundle", "create", str(path), "HEAD")

    def setup(self, revision=None):
        def q(path):
            return "'" + str(path).replace("'", "''") + "'"
        script = self.root / "invoke.ps1"
        script.write_text("$ErrorActionPreference='Stop'\n. " + q(ROOT / "launcher/windows/Setup.ps1") +
                          "\n$script:Portable=$true\n$script:SetupRoot=" + q(self.destination / "data") +
                          "\n$script:SourceRepository='file:///nonexistent-rgo-test-remote'\ntry {\n" +
                          "Get-PinnedSource '" + (revision or self.revision) +
                          "' | ForEach-Object { Write-Output $_ }\n} catch { Write-Output $_.Exception.Message; exit 1 }\n")
        return subprocess.run(["powershell.exe", "-NoProfile", "-File", str(script)],
                              capture_output=True, text=True, timeout=45)

    def test_fresh_bundle_setup_and_clean_update_without_repository_access(self):
        result = self.setup()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        cache = self.destination / "data/source/current"
        self.assertEqual(self.git("-C", str(cache), "rev-parse", "HEAD"), self.revision)
        (self.source / "change.txt").write_text("second synthetic revision\n")
        self.git("-C", str(self.source), "add", ".")
        self.commit()
        self.bundle()
        result = self.setup()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.git("-C", str(cache), "rev-parse", "HEAD"), self.revision)

    def test_wrong_revision_is_rejected(self):
        result = self.setup("a" * 40)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.destination / "data/source/current").exists())

    def test_corrupt_bundle_is_rejected(self):
        (self.destination / "source.bundle").write_bytes(b"not a git bundle")
        result = self.setup()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.destination / "data/source/current").exists())

    def test_dirty_cache_is_preserved_and_rejected(self):
        self.assertEqual(self.setup().returncode, 0)
        keep = self.destination / "data/source/current/scripts/build_from_rom.py"
        keep.write_text("owner changes preserved\n")
        result = self.setup()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(keep.read_text(), "owner changes preserved\n")
