"""RGO: the release launcher builds RGO's own source, without batch files."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def player_batch_files(root):
    tracked = subprocess.check_output(
        ["git", "ls-files", "-z"], cwd=root).decode("utf-8").split("\0")
    return [name for name in tracked if name and "/" not in name
            and Path(name).suffix.lower() in (".cmd", ".bat")]


class RgoLauncherReleaseTests(unittest.TestCase):
    def test_setup_downloads_rgo_source(self):
        setup = (ROOT / "launcher/windows/Setup.ps1").read_text(encoding="utf-8")
        self.assertIn("$script:SourceRepository = 'https://github.com/ravenx444/RGO-Recomp.git'", setup)
        self.assertNotIn("TK22-26/JetForceGemini-Recomp.git", setup)

    def test_setup_build_does_not_rebuild_the_open_launcher(self):
        # build_from_rom.py refuses to build the frontend while it is running,
        # and during setup the launcher (the frontend) is always running.
        setup = (ROOT / "launcher/windows/Setup.ps1").read_text(encoding="utf-8")
        build = re.search(r"\$build = @\([^\n]*build_from_rom\.py[^\n]*\)", setup)
        self.assertIsNotNone(build)
        self.assertIn("'--no-frontend'", build.group(0))

    def test_launcher_links_point_at_rgo(self):
        for path in ("src/app/frontend_win32.cpp", "launcher/windows/Launcher.cs",
                     "launcher/windows/Support.cs", "launcher/ui/assets/about.rml"):
            text = (ROOT / path).read_text(encoding="utf-8")
            self.assertNotIn("github.com/TK22-26/JetForceGemini-Recomp", text, path)

    def test_first_run_message_is_shown(self):
        message = "First-time setup can take an hour or more."
        frontend = (ROOT / "src/app/frontend_win32.cpp").read_text(encoding="utf-8")
        ui = (ROOT / "launcher/ui/native_ui.cpp").read_text(encoding="utf-8")
        self.assertIn('L"First-time setup can take an hour or more. "', frontend)
        self.assertIn("First-time setup can take an hour or more.", ui)
        self.assertIn(message, (ROOT / "docs/rgo/PLAYER_GUIDE.md").read_text(encoding="utf-8").replace("**", ""))

    def test_portable_install_keeps_files_next_to_the_launcher(self):
        frontend = (ROOT / "src/app/frontend_win32.cpp").read_text(encoding="utf-8")
        self.assertIn('fs::path(module).parent_path() / L"data"', frontend)
        self.assertIn('SetEnvironmentVariableW(L"RGO_DATA_ROOT"', frontend)
        self.assertNotIn("FOLDERID_LocalAppData", frontend)
        first_run = (ROOT / "launcher/windows/FirstRun.cs").read_text(encoding="utf-8")
        self.assertIn('GetEnvironmentVariable("RGO_DATA_ROOT")', first_run)
        self.assertIn('" -DataRoot "', first_run)
        setup = (ROOT / "launcher/windows/Setup.ps1").read_text(encoding="utf-8")
        self.assertIn("[string]$DataRoot", setup)
        self.assertIn("$build += @('--build-root', $script:BuildRoot)", setup)
        images = (ROOT / "launcher/windows/InventoryImages.cs").read_text(encoding="utf-8")
        self.assertNotIn("LocalApplicationData", images)

    def test_portable_build_root_fits_build_paths_limit(self):
        import sys
        sys.path.insert(0, str(ROOT / "scripts"))
        import build_paths
        # The launcher allows data\b up to 50 characters; build_paths must accept it.
        base = Path("C:/" + "x" * (50 - len("C:/")))
        self.assertEqual(build_paths.MAX_BINARY_ROOT, 80)
        self.assertLessEqual(len(str(base)) + len("/0123456789ab/w/000000000000/n"), build_paths.MAX_BINARY_ROOT)

    def test_open_reports_folder(self):
        frontend = (ROOT / "src/app/frontend_win32.cpp").read_text(encoding="utf-8")
        ui = (ROOT / "launcher/ui/native_ui.cpp").read_text(encoding="utf-8")
        self.assertIn('else if (a == "reports")', frontend)
        self.assertIn('app.dataRoot / L"reports"', frontend)
        self.assertIn('button("reports", "Open reports folder")', ui)
        self.assertIn('a == "reports"', ui)

    def test_wsl_scratch_is_removed(self):
        generate = (ROOT / "scripts/generate_from_rom.py").read_text(encoding="utf-8")
        self.assertIn("atexit.register(shutil.rmtree, scratch, ignore_errors=True)", generate)

    def test_batch_files_use_crlf(self):
        attributes = (ROOT / ".gitattributes").read_text(encoding="utf-8")
        self.assertIn("*.cmd text eol=crlf", attributes)

    def test_no_batch_files_for_players(self):
        self.assertEqual(player_batch_files(ROOT), [])

    def test_player_batch_check_preserves_untracked_helpers(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subprocess.run(["git", "init", "-q"], cwd=root, check=True)
            (root / "Play RGO.bat").write_text("@echo off\n")
            developer = root / "scripts" / "dev"
            developer.mkdir(parents=True)
            (developer / "build.cmd").write_text("@echo off\n")
            subprocess.run(["git", "add", "scripts/dev/build.cmd"], cwd=root, check=True)
            self.assertEqual(player_batch_files(root), [])
            subprocess.run(["git", "add", "Play RGO.bat"], cwd=root, check=True)
            self.assertEqual(player_batch_files(root), ["Play RGO.bat"])


if __name__ == "__main__":
    unittest.main()
