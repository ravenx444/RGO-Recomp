"""RGO: ROM-free checks that source builds start the game through the frontend."""
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import build_from_rom  # noqa: E402


class SourceBuildFrontendTests(unittest.TestCase):
    def test_frontend_build_uses_the_launcher_script_and_given_output(self):
        output = Path("C:/JFG/b/0123456789ab/f")
        command = build_from_rom.frontend_build_command(ROOT, output)
        self.assertTrue(command[0].lower().endswith("powershell.exe"))
        self.assertIn("-NonInteractive", command)
        script = command[command.index("-File") + 1]
        self.assertEqual(Path(script), ROOT / "scripts/build_launcher.ps1")
        self.assertTrue((ROOT / "scripts/build_launcher.ps1").is_file())
        self.assertEqual(command[command.index("-OutputDirectory") + 1], str(output))
        self.assertEqual(command[command.index("-NativeBuildDirectory") + 1], str(output / "n"))

    def test_play_starts_the_frontend_with_quick_play_and_profile(self):
        launcher = Path("C:/JFG/b/0123456789ab/f/RGO-Launcher.exe")
        profile = Path("D:/JFGRecomp/profiles/default")
        self.assertEqual(build_from_rom.frontend_play_command(launcher, profile),
                         [str(launcher), "--play", "--profile", str(profile)])

    def test_profile_points_at_the_fresh_build_in_frontend_format(self):
        with tempfile.TemporaryDirectory() as directory:
            profile = Path(directory) / "profile with spaces"
            rom = Path(directory) / "my rom é.z64"
            game = Path(directory) / "w" / "n" / "Release" / "jfg-native-boot.exe"
            (profile).mkdir()
            (profile / "frontend-runtime.txt").write_text("C:/old/jfg-native-boot.exe\r\n")
            build_from_rom.select_frontend_build(profile, rom, game)
            # The frontend reads UTF-8 and trims line breaks; write none.
            self.assertEqual((profile / "frontend-rom.txt").read_bytes(), str(rom).encode("utf-8"))
            self.assertEqual((profile / "frontend-runtime.txt").read_bytes(), str(game).encode("utf-8"))
            self.assertEqual(sorted(p.name for p in profile.iterdir()),
                             ["frontend-rom.txt", "frontend-runtime.txt"])

    def test_environment_drops_inherited_jfg_variables_and_names_the_checkout(self):
        environment = build_from_rom.frontend_environment(
            {"PATH": "x", "JFG_PHASE8_INPUT_REPLAY": "replay", "jfg_video_config": "y"}, ROOT)
        self.assertEqual(environment, {"PATH": "x", "JFG_SOURCE_ROOT": str(ROOT)})

    def test_frontend_mutex_matches_the_frontend(self):
        source = (ROOT / "src/app/frontend_win32.cpp").read_text(encoding="utf-8")
        self.assertIn('L"Local\\\\JFGUnifiedFrontend"', source)
        self.assertEqual(build_from_rom.FRONTEND_MUTEX, "Local\\JFGUnifiedFrontend")
        if sys.platform != "win32":
            self.assertFalse(build_from_rom.frontend_is_open())


if __name__ == "__main__":
    unittest.main()
