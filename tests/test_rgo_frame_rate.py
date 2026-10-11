from __future__ import annotations

import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "rgo_frame_rate.py"
SPEC = importlib.util.spec_from_file_location("rgo_frame_rate", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
FRAME_RATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FRAME_RATE)

HEADER = "vi\tticks\tdelta_time\tcount\tinstructions\twall_us\tfront_mode\tlevel\n"


def trace(rows: list[tuple[int, int, int, int]]) -> list[dict[str, int]]:
    """Rows of (vi, ticks, wall_us, level); other columns are constant."""
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "frames.tsv"
        path.write_text(HEADER + "".join(
            f"{vi}\t{ticks}\t{ticks}\t{vi * 781250}\t{vi * 1000}\t{wall}\t16\t{level}\n"
            for vi, ticks, wall, level in rows))
        return FRAME_RATE.load(path)


class FrameRateSummaryTests(unittest.TestCase):
    def test_busy_governor_and_underrun_columns(self) -> None:
        header = HEADER.rstrip("\n") + "\tbusy_us\ttrue60\taudio_underruns\n"
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "frames.tsv"
            path.write_text(header + "".join(
                f"{vi}\t1\t1\t{vi * 781250}\t{vi * 1000}\t{vi * 16667}\t16\t93\t"
                f"{10000 + vi}\t{2 if vi >= 90 else 1}\t{vi // 50}\n"
                for vi in range(0, 101)))
            summary = FRAME_RATE.summarize(FRAME_RATE.load(path))
        self.assertEqual(summary["busy_ms"]["max"], 10.1)
        self.assertEqual(summary["holding_30_frames"], 11)
        self.assertEqual(summary["audio_underruns"], 2)

    def test_sixty_logic_frames_per_emulated_second(self) -> None:
        rows = trace([(vi, 1, vi * 16667, 93) for vi in range(0, 601)])
        summary = FRAME_RATE.summarize(rows)
        self.assertEqual(summary["frames"], 600)
        self.assertEqual(summary["game_frames_per_emulated_second"], 60.0)
        self.assertEqual(summary["ticks_histogram"], {1: 600})
        self.assertEqual(summary["game_time_over_emulated_time"], 1.0)
        self.assertAlmostEqual(summary["emulated_per_wall"], 1.0, places=2)

    def test_thirty_and_slow_motion(self) -> None:
        # Two VIs per frame, but the host took twice the real time.
        rows = trace([(vi, 2, vi * 33334, 93) for vi in range(0, 601, 2)])
        summary = FRAME_RATE.summarize(rows)
        self.assertEqual(summary["game_frames_per_emulated_second"], 30.0)
        self.assertAlmostEqual(summary["emulated_per_wall"], 0.5, places=2)
        self.assertAlmostEqual(summary["game_frames_per_wall_second"], 15.0, places=1)

    def test_segments_follow_level_changes(self) -> None:
        rows = trace([(vi, 2, vi * 16667, 323 if vi < 100 else 93) for vi in range(0, 200, 2)])
        names = [name for name, _ in FRAME_RATE.segments(rows)]
        self.assertEqual(names, ["mode 16 level 323", "mode 16 level 93"])

    def test_short_trace(self) -> None:
        self.assertEqual(FRAME_RATE.summarize(trace([(0, 2, 0, 93)])), {"frames": 1})


class HeadlessRunArgumentTests(unittest.TestCase):
    def test_unpaced_needs_the_deterministic_clock(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run(
                [sys.executable, str(ROOT / "scripts" / "rgo_headless_run.py"),
                 "--executable", str(Path(directory) / "missing"),
                 "--rom", str(Path(directory) / "missing.z64"),
                 "--mode", "true60", "--unpaced", "--out", directory],
                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 2)
        self.assertIn("--unpaced needs --deterministic", result.stderr)


if __name__ == "__main__":
    unittest.main()
