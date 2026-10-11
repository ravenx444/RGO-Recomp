from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
import shutil
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "assemble_g2_private_evidence.py"
SPEC = importlib.util.spec_from_file_location("assemble_g2_private_evidence", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
ASSEMBLER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ASSEMBLER)


def digest(label: str) -> str:
    return hashlib.sha256(label.encode("utf-8")).hexdigest()


class AssembleG2PrivateEvidenceTests(unittest.TestCase):
    def setUp(self) -> None:
        (ROOT / "tools").mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=ROOT / "tools", prefix="g2-assemble-")
        self.bundle = Path(self.temporary.name)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def pins(self, decision: bytes) -> dict[str, str]:
        return {
            "jfg_decomp_commit": hashlib.sha1(b"decomp").hexdigest(),
            "n64recomp_commit": hashlib.sha1(b"recomp").hexdigest(),
            "supported_input_id": "jfg-us-retail",
            "input_rom_sha256": digest("rom"),
            "dependency_lock_sha256": digest("lock"),
            "architecture_decision_sha256": hashlib.sha256(decision).hexdigest(),
        }

    def plan(self, directory: Path) -> tuple[dict[str, object], dict[str, str]]:
        entries: list[dict[str, object]] = []
        decision = b"accepted decision\n"
        pins = self.pins(decision)
        for index, (requirement_id, evidence_class) in enumerate(ASSEMBLER.EXPECTED):
            prefix = f"e{index:02d}"
            policy_id = ASSEMBLER.H.MAPPING_IDS[(requirement_id, evidence_class)]
            execution_id = f"exec-{index:02d}"
            case_id = "cpu-case" if requirement_id == "cpu-sections" else f"case-{index:02d}"
            relative = Path("production") / policy_id / execution_id
            (directory / relative).mkdir(parents=True, exist_ok=True)
            config = directory / relative / "configuration.json"
            source = pins["dependency_lock_sha256"] if evidence_class == "human-approved-decision" else pins["input_rom_sha256"]
            subject = hashlib.sha256(decision).hexdigest() if evidence_class == "human-approved-decision" else hashlib.sha256(("cpu case" if requirement_id == "cpu-sections" else f"case {index}").encode()).hexdigest()
            configuration = {"schema_version": 1, "kind": "test-config", "requirement_id": requirement_id, "evidence_class": evidence_class, "case_id": case_id, "subject_sha256": subject, "source_input_sha256": source, "observation_sha256": digest(f"observation-{index}"), "policy_id": policy_id}
            if evidence_class != "human-approved-decision":
                configuration["source_derivation"] = {}
            if requirement_id == "cpu-sections":
                family = ("clang", "gcc", "msvc")[index]
                configuration["g3_compiler_id"] = f"{family}-test"
                configuration["g3_compiler_executable_sha256"] = digest(family)
            config.write_bytes(ASSEMBLER.G2._canonical_bytes(configuration))
            artifacts: list[dict[str, str]] = [{"path": (relative / config.name).as_posix(), "role": "configuration"}]
            if evidence_class == "human-approved-decision":
                lock_path = directory / relative / "dependency-lock.json"
                lock_path.write_bytes(b"{}")
                artifacts.append({"path": (relative / lock_path.name).as_posix(), "role": "input"})
                input_path = directory / relative / "decision.md"
                input_path.write_bytes(decision)
                artifacts.append({"path": (relative / input_path.name).as_posix(), "role": "decision"})
            else:
                input_path = directory / relative / "case-input.bin"
                input_path.write_bytes(("cpu case" if requirement_id == "cpu-sections" else f"case {index}").encode())
                probe = directory / relative / "subject-probe"
                probe.write_bytes(b"probe")
                output = directory / relative / "observation.json"
                output.write_bytes(f"output {index}".encode())
                artifacts.extend(({"path": (relative / input_path.name).as_posix(), "role": "input"}, {"path": (relative / probe.name).as_posix(), "role": "input"}, {"path": (relative / output.name).as_posix(), "role": "output"}))
            artifacts.append({"path": (relative / "result.json").as_posix(), "role": "result"})
            entries.append({
                "requirement_id": requirement_id, "evidence_class": evidence_class,
                "id": execution_id, "case_id": case_id,
                "environment": {
                    "environment_id": "cpu-producer-env" if requirement_id == "cpu-sections" else f"env-{index:02d}",
                    "platform_id": "test-platform",
                    "architecture_id": "test-architecture" if requirement_id == "cpu-sections" else f"arch-{index:02d}",
                    "toolchain_sha256": digest("cpu-producer-toolchain") if requirement_id == "cpu-sections" else digest(f"toolchain-{index}"),
                },
                "artifacts": artifacts,
            })
        return {"$schema": ASSEMBLER.PLAN_SCHEMA, "schema_version": 1, "kind": ASSEMBLER.PLAN_KIND, "executions": entries}, pins

    def policy(self) -> dict[tuple[str, str], object]:
        return {
            mapping: ASSEMBLER.G2.PinnedHarness(f"h-{index}", SCRIPT, digest(f"h-{index}"))
            for index, mapping in enumerate(set(ASSEMBLER.EXPECTED))
        }

    def write_plan(self, plan: dict[str, object], directory: Path) -> Path:
        path = directory / "plan.json"
        path.write_bytes(ASSEMBLER.G2._canonical_bytes(plan))
        return path

    def test_two_independent_outputs_are_deterministic(self) -> None:
        plan, pins = self.plan(self.bundle)
        plan_path = self.write_plan(plan, self.bundle)
        first = self.bundle / "first"
        second = self.bundle / "second"
        first.mkdir()
        second.mkdir()
        # The plan's artifacts are relative to its own bundle, so use copies.
        outputs = []
        for target in (first, second):
            work = target / "bundle"
            shutil.copytree(self.bundle, work, ignore=shutil.ignore_patterns("first", "second"))
            local_plan = work / "plan.json"
            private, public = ASSEMBLER._assemble_for_tests(local_plan, work / "private.json", work / "public.json", pins=pins, harness_pins=self.policy())
            self.assertNotIn("input_rom_sha256", public["pins"])
            self.assertEqual(public["pins"]["supported_input_id"], "jfg-us-retail")
            outputs.append((ASSEMBLER.G2._canonical_bytes(private), ASSEMBLER.G2._canonical_bytes(public)))
        self.assertEqual(outputs[0], outputs[1])

    def test_omission_duplicate_and_order_swap_are_rejected(self) -> None:
        plan, pins = self.plan(self.bundle)
        variants = []
        omitted = copy.deepcopy(plan)
        omitted["executions"].pop()  # type: ignore[index]
        variants.append(omitted)
        duplicate = copy.deepcopy(plan)
        duplicate["executions"][1]["artifacts"][0]["path"] = duplicate["executions"][0]["artifacts"][0]["path"]  # type: ignore[index]
        variants.append(duplicate)
        swapped = copy.deepcopy(plan)
        swapped["executions"][5], swapped["executions"][6] = swapped["executions"][6], swapped["executions"][5]  # type: ignore[index]
        variants.append(swapped)
        for index, variant in enumerate(variants):
            with self.subTest(index=index):
                directory = self.bundle / f"bad-{index}"
                directory.mkdir()
                plan_path = self.write_plan(variant, directory)
                with self.assertRaises(ASSEMBLER.AssemblyError):
                    ASSEMBLER._assemble_for_tests(plan_path, directory / "private.json", directory / "public.json", pins=pins, harness_pins=self.policy())

    def test_tracked_output_is_rejected(self) -> None:
        plan, pins = self.plan(self.bundle)
        plan_path = self.write_plan(plan, self.bundle)
        with self.assertRaises(ASSEMBLER.AssemblyError):
                    ASSEMBLER._assemble_for_tests(plan_path, ROOT / "LICENSE", self.bundle / "public.json", pins=pins, harness_pins=self.policy())

    def test_legacy_shared_and_cross_execution_paths_are_rejected(self) -> None:
        plan, pins = self.plan(self.bundle)
        legacy = copy.deepcopy(plan)
        first = legacy["executions"][0]  # type: ignore[index]
        first["artifacts"][1]["path"] = first["artifacts"][1]["path"].replace("/exec-00/", "/")  # type: ignore[index]
        swapped = copy.deepcopy(plan)
        swapped["executions"][0]["artifacts"][1]["path"] = swapped["executions"][1]["artifacts"][1]["path"]  # type: ignore[index]
        for index, variant in enumerate((legacy, swapped)):
            directory = self.bundle / f"namespace-{index}"
            directory.mkdir()
            plan_path = self.write_plan(variant, directory)
            with self.assertRaises(ASSEMBLER.AssemblyError):
                ASSEMBLER._assemble_for_tests(plan_path, directory / "private.json", directory / "public.json", pins=pins, harness_pins=self.policy())

    def test_cpu_case_binds_three_g3_products_without_claiming_three_execution_environments(self) -> None:
        plan, pins = self.plan(self.bundle)
        cpu = plan["executions"][:3]  # type: ignore[index]
        self.assertEqual({item["case_id"] for item in cpu}, {"cpu-case"})
        paths = [item["artifacts"][1]["path"] for item in cpu]
        self.assertEqual(len(set(paths)), 3)
        plan_path = self.write_plan(plan, self.bundle)
        private, _ = ASSEMBLER._assemble_for_tests(plan_path, self.bundle / "cpu-private.json", self.bundle / "cpu-public.json", pins=pins, harness_pins=self.policy())
        executions = private["requirements"]["cpu-sections"]["executions"]  # type: ignore[index]
        self.assertEqual(len({item["subject_sha256"] for item in executions}), 1)
        self.assertEqual(len({item["environment_sha256"] for item in executions}), 1)
        compiler_ids = set()
        for item in cpu:
            config_path = self.bundle / item["artifacts"][0]["path"]
            config = json.loads(config_path.read_text(encoding="utf-8"))
            compiler_ids.add(config["g3_compiler_id"])
        self.assertEqual(compiler_ids, {"clang-test", "gcc-test", "msvc-test"})

    def test_late_staged_validation_rejection_leaves_no_final_outputs(self) -> None:
        plan, pins = self.plan(self.bundle)
        plan_path = self.write_plan(plan, self.bundle)
        private = self.bundle / "late-private.json"
        public = self.bundle / "late-public.json"
        with self.assertRaises(ASSEMBLER.AssemblyError):
            ASSEMBLER._assemble_for_tests(plan_path, private, public, pins=pins, harness_pins=self.policy(), validator=lambda *_args: ["reject"])
        self.assertFalse(private.exists())
        self.assertFalse(public.exists())
        self.assertFalse(any(self.bundle.glob("production/**/result.json")))

    def test_cli_fails_closed_without_trusted_pins(self) -> None:
        plan, _ = self.plan(self.bundle)
        plan_path = self.write_plan(plan, self.bundle)
        self.assertEqual(ASSEMBLER.main(["--plan", str(plan_path), "--private-evidence", str(self.bundle / "private.json"), "--public-evidence", str(self.bundle / "public.json")]), 1)
