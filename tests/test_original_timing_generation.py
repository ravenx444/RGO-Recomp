import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import tomllib
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("original_timing_generation", ROOT / "scripts/original_timing_generation.py")
timing = importlib.util.module_from_spec(spec)
spec.loader.exec_module(timing)

class OriginalTimingGenerationTests(unittest.TestCase):
    def test_operation_classes_and_branch_flags(self):
        # Synthetic VR4300 instructions exercise the hook ABI without game data.
        load = (0x23 << 26) | (2 << 21) | (3 << 16) | 16
        store = (0x2b << 26) | (2 << 21) | (3 << 16) | 16
        self.assertEqual((timing.metadata(2, 0x80001000, load) >> 12) & 3, 1)
        self.assertEqual((timing.metadata(2, 0x80001000, store) >> 12) & 3, 2)
        self.assertTrue(timing.metadata(2, 0x80001000, 4 << 26) & (1 << 10))
        self.assertFalse(timing.metadata(2, 0x80001000, load) & (1 << 10))
        self.assertEqual(timing.metadata(2, 0x80001000, 0) & 255, 2)
        with self.assertRaises(ValueError): timing.metadata(256, 0, 0)

    def test_player_time_observers_cannot_be_skipped_by_fast_clock(self):
        for pc in (0x00f000d4,0x010000d4,0x01100088,0x00f01b30,0x010019f8,0x0110192c):
            self.assertTrue(timing.metadata(pc>>20,pc,0) & 0x800)
            self.assertFalse(timing.metadata(pc>>20,pc+4,0) & 0x800)

    def test_fresh_configuration_is_bounded_and_not_repeatable(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "recompile-private.toml").write_text('[input]\nsymbols_file_path="symbols.toml"\nrom_file_path="image.bin"\n')
            (root / "symbols.toml").write_text('[[section]]\nrom=0\nvram=4096\n[[section.functions]]\nname="fixture"\nvram=4096\nsize=8\n')
            (root / "image.bin").write_bytes(struct.pack(">II", 0, 0x24020001))
            self.assertEqual(timing.configure(root), 2)
            config = tomllib.loads((root / "recompile-private.toml").read_text())
            self.assertTrue(config["input"]["emit_guest_dynamic_returns"])
            self.assertTrue(all("0x00800000U" in row["text"] for row in config["patches"]["hook"]))
            self.assertEqual([x["before_vram"] for x in config["patches"]["hook"]], [4096, 4100])
            with self.assertRaises(ValueError): timing.configure(root)

    def test_odd_double_transfer_selects_pair_only_in_fr_zero(self):
        source = "// 0x80001000: ldc1 $f3, 0($a0)\n    CHECK_FR(ctx, 3);\n    ctx->f3.u64 = value;\n"
        text,count = timing.paired_register_transfers(source)
        self.assertEqual(count, 1)
        self.assertIn("ctx->mips3_float_mode ? &ctx->f3.u64 : &ctx->f2.u64", text)
        self.assertNotIn("CHECK_FR", text)
        even = source.replace("$f3", "$f2").replace("ctx, 3", "ctx, 2").replace("f3.u64", "f2.u64")
        self.assertEqual(timing.paired_register_transfers(even), (even, 0))
        with self.assertRaises(ValueError): timing.paired_register_transfers(source.replace("    CHECK_FR(ctx, 3);\n", ""))

    def test_missing_hooks_or_pair_recovery_cannot_enable_runtime(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            with self.assertRaises(ValueError): timing.finish(root,0,1)
            with self.assertRaises(ValueError): timing.finish(root,1,0)
            self.assertFalse((root / "jfg_original_timing.h").exists())

if __name__ == "__main__": unittest.main()
