"""RGO: ROM-free checks for reusing generation and build folders between source builds."""
import ast
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import build_from_rom  # noqa: E402
import rgo_build_cache as cache_tools  # noqa: E402

ROM = "493ced9008dbe932d6e91179b68e8630cf23a023"
# String constants in generator scripts that look like repository paths but are
# not read from this checkout.
NOT_CHECKOUT_PATHS = {
    "cmake": "the cmake tool name",
    "symbols": "a TOML/JSON key",
    "include/recomp.h": "read from the patched N64Recomp checkout (pinned)",
    "src/generated/": "a hygiene pattern",
}


def make_root(directory: Path) -> Path:
    """A minimal copy of the files generation_inputs reads."""
    root = directory / "checkout"
    for relative in cache_tools.generator_scripts(ROOT) + list(cache_tools.COPIED_FILES.values()):
        (root / relative).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / relative, root / relative)
    shutil.copy2(ROOT / "dependencies.lock.json", root / "dependencies.lock.json")
    shutil.copytree(ROOT / "patches/n64recomp", root / "patches/n64recomp")
    return root


def make_workspace(cache: Path, name: str, inputs: dict | None, root: Path = ROOT,
                   complete: bool = True, age: float = 0.0) -> Path:
    workspace = cache / "w" / name
    for relative in cache_tools.EXPECTED_GENERATION_FILES:
        (workspace / relative).parent.mkdir(parents=True, exist_ok=True)
        (workspace / relative).write_bytes(b"generated " + relative.encode())
    for inside, source in cache_tools.COPIED_FILES.items():
        shutil.copy2(root / source, workspace / inside)
    files = {relative: cache_tools.sha256_file(workspace / relative)
             for relative in cache_tools.EXPECTED_GENERATION_FILES}
    (workspace / "generation.json").write_text(json.dumps(
        {"complete": complete, "rom_sha1": ROM, "files": files}))
    if inputs is not None:
        cache_tools.write_key(workspace, inputs)
    stamp = time.time() - age
    for path in (workspace / cache_tools.KEY_FILE, workspace):
        if path.exists():
            os.utime(path, (stamp, stamp))
    return workspace


class GenerationInputTests(unittest.TestCase):
    def test_closure_contains_every_script_the_generator_runs_or_imports(self):
        scripts = set(cache_tools.generator_scripts(ROOT))
        for name in ("generate_from_rom", "apply_n64recomp_patchset", "prepare_n64recomp_context_dump",
                     "transform_private_n64recomp_inputs", "build_private_generated_root",
                     "identify_libultra", "phase9_recover_task_helper", "original_timing_generation",
                     "probe_n64recomp_cpu", "validate_elf", "public_safe"):
            self.assertIn("scripts/" + name + ".py", scripts)
        # Not a generation input.
        self.assertNotIn("scripts/build_from_rom.py", scripts)

    def test_every_checkout_path_the_generator_mentions_is_keyed_or_refreshed(self):
        # Guards against a generator change that starts reading a new
        # repository file: add it to GENERATION_DATA or COPIED_FILES.
        tracked = subprocess.run(["git", "-C", str(ROOT), "ls-files"], capture_output=True, text=True)
        if tracked.returncode:
            self.skipTest("needs a git checkout")
        top_level = {line.split("/")[0] for line in tracked.stdout.splitlines()}
        scripts = cache_tools.generator_scripts(ROOT)
        covered = set(scripts) | set(cache_tools.GENERATION_DATA) | set(cache_tools.COPIED_FILES.values())
        for script in scripts:
            for node in ast.walk(ast.parse((ROOT / script).read_text(encoding="utf-8"))):
                if not (isinstance(node, ast.Constant) and isinstance(node.value, str)):
                    continue
                value = node.value
                if value.split("/")[0] not in top_level or value in NOT_CHECKOUT_PATHS:
                    continue
                clean = value.rstrip("/")
                self.assertTrue(any(path == clean or path.startswith(clean + "/") or clean.startswith(path + "/")
                                    for path in covered), f"{script} refers to {value!r}")

    def test_copied_files_and_manifest_match_the_generator(self):
        source = (ROOT / cache_tools.GENERATOR).read_text(encoding="utf-8")
        tree = ast.parse(source)
        manifest = next(node.value for node in ast.walk(tree) if isinstance(node, ast.Assign)
                        and any(isinstance(t, ast.Name) and t.id == "manifest_files" for t in node.targets))
        self.assertEqual(set(ast.literal_eval(manifest)), cache_tools.EXPECTED_GENERATION_FILES)
        self.assertIn('for name in ("brokered-rsp.hpp", "production-audio-adapter.cpp"):', source)
        self.assertIn('shutil.copy2(ROOT / "src/bootstrap" / name, audio / name)', source)
        timing = (ROOT / "scripts/original_timing_generation.py").read_text(encoding="utf-8")
        self.assertIn('(root / "jfg_original_timing_fast.h").write_bytes((Path(__file__).resolve().parents[1] / '
                      '"include/jfg/boot/original_timing_fast.h").read_bytes())', timing)
        self.assertTrue(set(cache_tools.COPIED_FILES) <= cache_tools.EXPECTED_GENERATION_FILES)

    def test_key_changes_with_each_generation_input_only(self):
        with tempfile.TemporaryDirectory() as directory:
            root = make_root(Path(directory))
            base = cache_tools.generation_inputs(root, ROM, "Ubuntu-24.04")
            key = cache_tools.generation_key(base)
            self.assertEqual(key, cache_tools.generation_key(
                cache_tools.generation_inputs(root, ROM, "Ubuntu-24.04")))
            self.assertNotEqual(key, cache_tools.generation_key(
                cache_tools.generation_inputs(root, "0" * 40, "Ubuntu-24.04")))
            self.assertNotEqual(key, cache_tools.generation_key(
                cache_tools.generation_inputs(root, ROM, "Debian")))

            def changed_after(relative: str, edit) -> bool:
                path = root / relative
                original = path.read_bytes()
                edit(path)
                try:
                    return cache_tools.generation_key(
                        cache_tools.generation_inputs(root, ROM, "Ubuntu-24.04")) != key
                finally:
                    path.write_bytes(original)

            append = lambda path: path.write_bytes(path.read_bytes() + b"\n# changed\n")
            for relative in ("scripts/generate_from_rom.py", "scripts/build_private_generated_root.py",
                             "scripts/validate_elf.py", "patches/n64recomp/series.json"):
                self.assertTrue(changed_after(relative, append), relative)
            # Copied files are refreshed instead of regenerating.
            for relative in cache_tools.COPIED_FILES.values():
                self.assertFalse(changed_after(relative, append), relative)
            # A CRLF checkout of identical text is not a change.
            crlf = lambda path: path.write_bytes(path.read_bytes().replace(b"\n", b"\r\n"))
            self.assertFalse(changed_after("scripts/generate_from_rom.py", crlf))

            def lock_edit(dependency: str):
                def edit(path: Path) -> None:
                    lock = json.loads(path.read_text())
                    for entry in lock["repositories"]:
                        if entry["id"] == dependency:
                            entry["commit"] = "f" * 40
                    path.write_text(json.dumps(lock))
                return edit
            self.assertTrue(changed_after("dependencies.lock.json", lock_edit("n64recomp")))
            self.assertTrue(changed_after("dependencies.lock.json", lock_edit("jfg-decomp")))
            # RT64 and the runtime are compiled by MSBuild, which tracks them itself.
            rt64 = cache_tools.generation_inputs(root, ROM, "Ubuntu-24.04")
            lock_edit("rt64")(root / "dependencies.lock.json")
            changed = cache_tools.generation_inputs(root, ROM, "Ubuntu-24.04")
            self.assertEqual({k: v for k, v in rt64.items() if k != "dependencies.lock.json"},
                             {k: v for k, v in changed.items() if k != "dependencies.lock.json"})

    def test_new_generator_script_joins_the_key(self):
        with tempfile.TemporaryDirectory() as directory:
            root = make_root(Path(directory))
            (root / "scripts/rgo_new_helper.py").write_text("VALUE = 1\n")
            generator = root / cache_tools.GENERATOR
            generator.write_text(generator.read_text().replace(
                "import tomllib\n", "import tomllib\nimport rgo_new_helper\n", 1))
            self.assertIn("scripts/rgo_new_helper.py", cache_tools.generator_scripts(root))

    def test_change_descriptions_name_what_changed(self):
        old = {"rom-sha1": "a", "scripts/generate_from_rom.py": "1", "dependency:n64recomp": "x"}
        new = {"rom-sha1": "a", "scripts/generate_from_rom.py": "2", "dependency:n64recomp": "y",
               "scripts/new.py": "3"}
        self.assertEqual(cache_tools.describe_changes(old, new),
                         "pinned n64recomp changed, scripts/generate_from_rom.py changed, scripts/new.py added")
        many = {f"scripts/{index}.py": "1" for index in range(9)}
        self.assertTrue(cache_tools.describe_changes({}, many).endswith("and 5 more"))


class ReuseDecisionTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.cache = Path(self.directory.name) / "cache with spaces"
        self.inputs = cache_tools.generation_inputs(ROOT, ROM, "Ubuntu-24.04")

    def tearDown(self):
        self.directory.cleanup()

    def test_first_build_generates(self):
        decision = cache_tools.choose_workspace(self.cache, self.inputs)
        self.assertIsNone(decision.workspace)
        self.assertIn("no cached generation yet", decision.message)

    def test_matching_complete_generation_is_reused(self):
        workspace = make_workspace(self.cache, "aaaaaaaaaaaa", self.inputs)
        decision = cache_tools.choose_workspace(self.cache, self.inputs)
        self.assertEqual(decision.workspace, workspace)
        self.assertTrue(decision.message.startswith("Reusing ROM generation from "))
        self.assertTrue(decision.message.endswith("(inputs unchanged)"))

    def test_newest_matching_generation_wins(self):
        make_workspace(self.cache, "aaaaaaaaaaaa", self.inputs, age=100)
        newer = make_workspace(self.cache, "bbbbbbbbbbbb", self.inputs, age=10)
        self.assertEqual(cache_tools.choose_workspace(self.cache, self.inputs).workspace, newer)

    def test_older_matching_generation_is_found_after_a_branch_switch(self):
        older = make_workspace(self.cache, "aaaaaaaaaaaa", self.inputs, age=100)
        other = dict(self.inputs, **{"scripts/generate_from_rom.py": "other branch"})
        make_workspace(self.cache, "bbbbbbbbbbbb", other, age=10)
        self.assertEqual(cache_tools.choose_workspace(self.cache, self.inputs).workspace, older)

    def test_reuse_moves_a_workspace_to_the_front(self):
        older = make_workspace(self.cache, "aaaaaaaaaaaa", self.inputs, age=100)
        newer = make_workspace(self.cache, "bbbbbbbbbbbb", None, age=10)
        self.assertEqual(cache_tools.list_workspaces(self.cache), [newer, older])
        cache_tools.mark_used(older)
        self.assertEqual(cache_tools.list_workspaces(self.cache), [older, newer])

    def test_changed_input_regenerates_and_says_why(self):
        old = dict(self.inputs, **{"scripts/generate_from_rom.py": "old"})
        make_workspace(self.cache, "aaaaaaaaaaaa", old)
        decision = cache_tools.choose_workspace(self.cache, self.inputs)
        self.assertIsNone(decision.workspace)
        self.assertEqual(decision.message, "Regenerating: scripts/generate_from_rom.py changed")

    def test_incomplete_or_tampered_generation_is_not_reused(self):
        make_workspace(self.cache, "aaaaaaaaaaaa", self.inputs, complete=False)
        tampered = make_workspace(self.cache, "bbbbbbbbbbbb", self.inputs)
        (tampered / "normalized/funcs.h").write_text("edited")
        missing = make_workspace(self.cache, "cccccccccccc", self.inputs)
        (missing / "libultra.json").unlink()
        self.assertIsNone(cache_tools.choose_workspace(self.cache, self.inputs).workspace)
        self.assertEqual(cache_tools.verify_generation(tampered), "generated file changed: normalized/funcs.h")

    def test_forged_or_corrupt_key_is_ignored(self):
        workspace = make_workspace(self.cache, "aaaaaaaaaaaa", self.inputs)
        record = json.loads((workspace / cache_tools.KEY_FILE).read_text())
        record["inputs"]["rom-sha1"] = "edited"
        (workspace / cache_tools.KEY_FILE).write_text(json.dumps(record))
        self.assertIsNone(cache_tools.choose_workspace(self.cache, self.inputs).workspace)
        (workspace / cache_tools.KEY_FILE).write_text("{")
        self.assertIsNone(cache_tools.read_key(workspace))

    def test_builds_from_before_the_cache_are_not_reused(self):
        make_workspace(self.cache, "aaaaaaaaaaaa", None)
        decision = cache_tools.choose_workspace(self.cache, self.inputs)
        self.assertIsNone(decision.workspace)
        self.assertIn("before the build cache existed", decision.message)

    def test_other_folders_are_not_workspaces(self):
        make_workspace(self.cache, "not-a-run-id", self.inputs)
        make_workspace(self.cache, "AAAAAAAAAAAA", self.inputs)
        self.assertEqual(cache_tools.list_workspaces(self.cache), [])
        self.assertIsNone(cache_tools.choose_workspace(self.cache, self.inputs).workspace)

    def test_refresh_copies_changed_checkout_files_only(self):
        with tempfile.TemporaryDirectory() as directory:
            root = make_root(Path(directory))
            workspace = make_workspace(self.cache, "aaaaaaaaaaaa", self.inputs, root=root)
            old = time.time() - 1000
            for inside in cache_tools.COPIED_FILES:
                os.utime(workspace / inside, (old, old))
            self.assertEqual(cache_tools.refresh_copied_files(root, workspace), [])
            for inside in cache_tools.COPIED_FILES:
                self.assertAlmostEqual((workspace / inside).stat().st_mtime, old, delta=1)
            changed = root / "include/jfg/boot/original_timing_fast.h"
            changed.write_bytes(changed.read_bytes() + b"\n// other branch\n")
            self.assertEqual(cache_tools.refresh_copied_files(root, workspace),
                             ["include/jfg/boot/original_timing_fast.h"])
            self.assertEqual((workspace / "normalized/jfg_original_timing_fast.h").read_bytes(),
                             changed.read_bytes())
            self.assertGreater((workspace / "normalized/jfg_original_timing_fast.h").stat().st_mtime, old + 10)
            self.assertAlmostEqual((workspace / "audio/brokered-rsp.hpp").stat().st_mtime, old, delta=1)
            manifest = json.loads((workspace / "generation.json").read_text())
            self.assertEqual(manifest["files"]["normalized/jfg_original_timing_fast.h"],
                             hashlib.sha256(changed.read_bytes()).hexdigest())
            self.assertIsNone(cache_tools.verify_generation(workspace))
            # A copied file edited in the workspace does not block reuse; it is refreshed.
            (workspace / "audio/brokered-rsp.hpp").write_text("stale")
            self.assertIsNone(cache_tools.verify_generation(workspace))
            self.assertEqual(cache_tools.refresh_copied_files(root, workspace), ["src/bootstrap/brokered-rsp.hpp"])


class CleanupTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.cache = Path(self.directory.name) / "cache"
        self.inputs = cache_tools.generation_inputs(ROOT, ROM, "Ubuntu-24.04")

    def tearDown(self):
        self.directory.cleanup()

    def test_selection_keeps_current_and_newest_reusable_other(self):
        paths = [Path(name) for name in ("e", "d", "c", "b", "a")]  # newest first
        reusable = {Path("c"), Path("a")}
        self.assertEqual(cache_tools.select_for_cleanup(paths, Path("e"), reusable.__contains__),
                         [Path("d"), Path("b"), Path("a")])
        # Without a reusable other, keep the newest other.
        self.assertEqual(cache_tools.select_for_cleanup(paths, Path("a"), lambda _: False),
                         [Path("d"), Path("c"), Path("b")])
        self.assertEqual(cache_tools.select_for_cleanup([Path("a")], Path("a"), lambda _: True), [])
        self.assertEqual(cache_tools.select_for_cleanup([], Path("a"), lambda _: True), [])

    def test_cleanup_deletes_old_builds_and_reports_space(self):
        failed = make_workspace(self.cache, "111111111111", None, age=50)
        older_good = make_workspace(self.cache, "222222222222", self.inputs, age=40)
        old_good = make_workspace(self.cache, "333333333333", self.inputs, age=30)
        failed_new = make_workspace(self.cache, "444444444444", self.inputs, complete=False, age=20)
        current = make_workspace(self.cache, "555555555555", self.inputs, age=0)
        (older_good / "n").mkdir()
        (older_good / "n" / "big.obj").write_bytes(b"x" * 4096)
        read_only = older_good / "n" / "read-only.pack"
        read_only.write_bytes(b"y")
        read_only.chmod(0o444)
        messages = []
        freed = cache_tools.clean_old_workspaces(self.cache, current, log=messages.append)
        self.assertGreater(freed, 4096)
        self.assertEqual(sorted(p.name for p in (self.cache / "w").iterdir()),
                         [old_good.name, current.name])
        for gone in (failed, older_good, failed_new):
            self.assertFalse(gone.exists())
        self.assertTrue(messages[-1].startswith("Deleted old build folders, freed "))

    def test_cleanup_skips_a_build_in_use(self):
        make_workspace(self.cache, "111111111111", self.inputs, age=50)
        busy = make_workspace(self.cache, "222222222222", None, age=40)
        make_workspace(self.cache, "333333333333", self.inputs, age=30)
        current = make_workspace(self.cache, "444444444444", self.inputs, age=0)
        with cache_tools.WorkspaceLock(busy) as lock:
            self.assertTrue(lock.acquire())
            self.assertTrue(cache_tools.workspace_in_use(busy))
            messages = []
            cache_tools.clean_old_workspaces(self.cache, current, log=messages.append)
            self.assertTrue(busy.exists())
            self.assertIn(f"Kept old build {busy.name}: another build is using it", messages)
        self.assertFalse(cache_tools.workspace_in_use(busy))

    def test_lock_is_exclusive(self):
        workspace = make_workspace(self.cache, "111111111111", None)
        first, second = cache_tools.WorkspaceLock(workspace), cache_tools.WorkspaceLock(workspace)
        self.assertTrue(first.acquire())
        self.assertFalse(second.acquire())
        first.release()
        self.assertTrue(second.acquire())
        second.release()

    def test_never_deletes_outside_this_checkouts_workspaces(self):
        outside = Path(self.directory.name) / "precious"
        outside.mkdir()
        (outside / "file").write_text("keep")
        named = self.cache / "w" / "abcdefabcdef"
        named.parent.mkdir(parents=True)
        for target in (outside, self.cache, self.cache / "w", self.cache / "d"):
            with self.assertRaises(ValueError):
                cache_tools.remove_workspace(self.cache, target)
        # A link named like a workspace is neither listed nor followed.
        named.symlink_to(outside, target_is_directory=True)
        self.assertEqual(cache_tools.list_workspaces(self.cache), [])
        with self.assertRaises(ValueError):
            cache_tools.remove_workspace(self.cache, named)
        current = make_workspace(self.cache, "222222222222", self.inputs)
        cache_tools.clean_old_workspaces(self.cache, current, log=lambda _: None)
        self.assertEqual((outside / "file").read_text(), "keep")


@unittest.skipUnless(shutil.which("cmake"), "needs cmake")
class WriteIfChangedTests(unittest.TestCase):
    def test_unchanged_content_keeps_the_timestamp(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            output = directory / "patched.cpp"
            script = directory / "write.cmake"
            # Carriage returns, list separators and brackets must survive exactly.
            script.write_text(
                f'include("{(ROOT / "cmake/WriteIfChanged.cmake").as_posix()}")\n'
                'set(content "int a; // \\\\ [[x]]\\r\\n${EXTRA}")\n'
                f'jfg_write_if_changed("{output.as_posix()}" "${{content}}")\n')
            def write(extra: str) -> None:
                subprocess.run(["cmake", f"-DEXTRA={extra}", "-P", str(script)], check=True)
            write("one")
            self.assertEqual(output.read_bytes(), b"int a; // \\ [[x]]\r\none")
            os.utime(output, (1000, 1000))
            write("one")
            self.assertEqual(output.stat().st_mtime, 1000)
            write("two")
            self.assertEqual(output.read_bytes(), b"int a; // \\ [[x]]\r\ntwo")
            self.assertGreater(output.stat().st_mtime, 1000)


class BuildOptionTests(unittest.TestCase):
    def test_default_jobs_use_all_cores_unless_memory_is_short(self):
        gib = 2**30
        self.assertEqual(cache_tools.default_jobs(16, 64 * gib), 16)
        self.assertEqual(cache_tools.default_jobs(32, 128 * gib), 32)
        self.assertEqual(cache_tools.default_jobs(16, 16 * gib), 10)
        self.assertEqual(cache_tools.default_jobs(8, None), 8)
        self.assertEqual(cache_tools.default_jobs(None, None), 4)
        self.assertEqual(cache_tools.default_jobs(4, gib), 1)
        self.assertEqual(cache_tools.default_jobs(256, 1024 * gib), cache_tools.MAX_JOBS)

    def test_step_plan_counts_only_steps_that_run(self):
        lock = json.loads((ROOT / "dependencies.lock.json").read_text())
        with tempfile.TemporaryDirectory() as directory:
            deps = Path(directory)
            for name in build_from_rom.NAMES.values():
                (deps / name).mkdir()
            reused = build_from_rom.plan_steps(deps, lock, True, False, True)
            self.assertEqual(reused, [f"submodules-{d}" for d in
                                      [e["id"] for e in lock["repositories"] if e["id"] in build_from_rom.NAMES]]
                             + ["configure", "compile", "frontend"])
            (deps / "rt64").rmdir()
            fresh = build_from_rom.plan_steps(deps, lock, True, True, False)
            self.assertEqual(fresh[:2], ["clone-rt64", "pin-rt64"])
            self.assertEqual(fresh[-3:], ["generate", "configure", "compile"])
            self.assertEqual(build_from_rom.plan_steps(deps, lock, False, True, True),
                             ["generate", "configure", "compile", "frontend"])


if __name__ == "__main__":
    unittest.main()
