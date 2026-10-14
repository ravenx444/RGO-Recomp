from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import scripts.build_private_generated_root as generated_root


ROOT = generated_root.ROOT
# Scratch space stays inside an ignored tree, but outside tools/ so a
# cleanup failure on Windows (a just-run executable still locked) cannot
# leave residue next to pinned tooling.
TEST_TEMP_ROOT = ROOT / "build" / "test-tmp"
TEST_TEMP_ROOT.mkdir(parents=True, exist_ok=True)


class BuildPrivateGeneratedRootTests(unittest.TestCase):
    def test_windows_generated_lines_preserve_continuation_labels(self) -> None:
        definition = (
            "RECOMP" + "_FUNC void fn_003_0090(uint8_t* rdram, recomp_context* ctx) {\n"
            "    // 0x00307E88: nop\n"
            "        // 0x00308518: addiu\n"
            "L_00308518:\n"
            "    // 0x00308518: addiu\n"
            "}\n"
        )
        row = {"name": "fn_003_0090", "section": 3, "offset": 0x7E88,
               "vram": 0x00307E88, "size": 0x17F8}
        continuation = {"name": "fn_003_0090", "section": 3,
                        "offset": 0x8518, "vram": 0x00308518}
        results = []
        for ending in ("\n", "\r\n"):
            with tempfile.TemporaryDirectory(dir=TEST_TEMP_ROOT) as directory:
                path = Path(directory)
                (path / "funcs_0.c").write_bytes(definition.replace("\n", ending).encode())
                body = generated_root.extract_functions(path)[row["name"]]
                results.append(generated_root.add_continuation_dispatch(body, row, [continuation]))
        self.assertEqual(results[0], results[1])
        self.assertIn("goto L_00308518;", results[1])
        self.assertNotIn("JFG_CONT_00308518", results[1])

    def test_jump_table_continuations_reenter_the_owning_body(self) -> None:
        authoritative = [
            {
                "name": "fn_007_0001",
                "section": 7,
                "offset": 0x100,
                "vram": 0x81230100,
                "size": 0x40,
            }
        ]
        manifest = {
            "r_mips_32": [
                {"target_section": 7, "target_section_offset": 0x120},
                {"target_section": 7, "target_section_offset": 0x120},
                {"target_section": 7, "target_section_offset": 0x100},
            ]
        }
        continuations = generated_root.derive_continuation_entries(
            authoritative, manifest
        )
        self.assertEqual(
            continuations,
            [
                {
                    "name": "fn_007_0001",
                    "section": 7,
                    "offset": 0x120,
                    "vram": 0x81230120,
                }
            ],
        )
        definition = (
            "RECOMP" + "_FUNC void fn_007_0001(uint8_t* rdram, recomp_context* ctx) {\n"
            "    uint64_t hi = 0, lo = 0, result = 0;\n"
            "    int c1cs = 0;\n"
            "    // 0x81230100: nop\n"
            "    // 0x81230120: nop\n"
            "}\n"
        )
        rewritten = generated_root.add_continuation_dispatch(
            definition, authoritative[0], continuations
        )
        self.assertIn("ctx->r0 = 0;", rewritten)
        self.assertIn(
            "case UINT32_C(0x00000120): goto JFG_CONT_81230120;",
            rewritten,
        )
        self.assertIn(
            "JFG_CONT_81230120:\n    // 0x81230120:", rewritten
        )
        lookup = generated_root.make_lookup(authoritative, continuations)
        self.assertIn(
            "{7u, UINT32_C(0x00000120), fn_007_0001}", lookup
        )

    def test_continuation_reuses_an_existing_recompiler_label(self) -> None:
        row = {
            "name": "fn_003_0090",
            "section": 3,
            "offset": 0x7E88,
            "vram": 0x00307E88,
            "size": 0x17F8,
        }
        continuation = {
            "name": "fn_003_0090",
            "section": 3,
            "offset": 0x8518,
            "vram": 0x00308518,
        }
        definition = (
            "RECOMP" + "_FUNC void fn_003_0090(uint8_t* rdram, recomp_context* ctx) {\n"
            "    int c1cs = 0;\n"
            "    // 0x00307E88: nop\n"
            "        // 0x00308518: addiu\n"
            "L_00308518:\n"
            "    // 0x00308518: addiu\n"
            "}\n"
        )
        rewritten = generated_root.add_continuation_dispatch(
            definition, row, [continuation]
        )
        self.assertIn(
            "case UINT32_C(0x00008518): goto L_00308518;", rewritten
        )
        self.assertNotIn("JFG_CONT_00308518", rewritten)

    def test_anonymous_recomp_context_typedef_is_tagged_once(self) -> None:
        header = "typedef struct { int value; } recomp_context;\n"
        self.assertEqual(
            generated_root.normalize_recomp_context_typedef(header),
            "typedef struct recomp_context { int value; } recomp_context;\n",
        )

    def test_tagged_recomp_context_typedef_is_idempotent(self) -> None:
        header = "typedef struct recomp_context { int value; } recomp_context;\n"
        self.assertEqual(
            generated_root.normalize_recomp_context_typedef(header), header
        )

    def test_recomp_context_typedef_rejects_zero_or_ambiguous_forms(self) -> None:
        for header in (
            "typedef struct { int value; } other_context;\n",
            "typedef struct { int first; } recomp_context;\n"
            "typedef struct { int second; } recomp_context;\n",
            "typedef struct recomp_context { int first; } recomp_context;\n"
            "typedef struct { int second; } recomp_context;\n",
        ):
            with self.subTest(header=header):
                with self.assertRaisesRegex(
                    generated_root.NormalizationError, "context typedef"
                ):
                    generated_root.normalize_recomp_context_typedef(header)

    def make_inputs(self, root: Path) -> argparse.Namespace:
        raw = root / "raw"
        raw.mkdir()
        (raw / "funcs_0.c").write_text(
            "RECOMP" + "_FUNC void fn_000_0000(uint8_t* rdram, recomp_context* ctx) {\n"
            "    (void)rdram;\n"
            "    (void)ctx;\n"
            "}\n",
            encoding="utf-8",
            newline="\n",
        )
        (raw / "indirect_decisions.json").write_bytes(
            generated_root.canonical_bytes(
                {
                    "schema_version": 1,
                    "kind": "n64recomp-indirect-decision-sidecar",
                    "generated_callable_set": {
                        "kind": "exact-generated-callable-entry-set",
                        "member_count": 1,
                        "members": [
                            {
                                "generated_function": "fn_000_0000",
                                "source_section": 0,
                                "source_offset": 0,
                                "size": 4,
                            }
                        ],
                    },
                    "decisions": [],
                    "direct_calls": [
                        {
                            "classification": "checked-lookup-call",
                            "default_disposition": "fail-closed-lookup-miss",
                            "generated_function": "fn_000_0000",
                            "instruction_class": "jal",
                            "source_offset": 0,
                            "source_section": 0,
                            "target": {
                                "address": 0x80000400,
                                "kind": "runtime-address",
                            },
                            "transfer_role": "linked-call",
                        }
                    ],
                }
            )
        )
        symbols = root / "symbols.toml"
        symbols.write_text(
            "[[section]]\n"
            'name = "section-000"\n'
            "rom = 0x1000\n"
            "vram = 0x80000400\n"
            "size = 4\n"
            "functions = [\n"
            '  { name = "fn_000_0000", vram = 0x80000400, size = 4 },\n'
            "]\n",
            encoding="utf-8",
            newline="\n",
        )
        context = root / "context.toml"
        context.write_text(symbols.read_text(encoding="utf-8"), encoding="utf-8", newline="\n")
        runtime = root / "runtime.json"
        runtime.write_text(
            json.dumps(self.runtime_manifest(), separators=(",", ":")),
            encoding="utf-8",
            newline="\n",
        )
        header = root / "recomp.h"
        header.write_text(
            "#ifndef __RECOMP_H__\n"
            "#define __RECOMP_H__\n"
            "#include <stddef.h>\n"
            "#include <stdint.h>\n"
            "#if defined(_MSC_VER)\n"
            "#define RECOMP_FUNC __declspec(noinline)\n"
            "#else\n"
            "#define RECOMP_FUNC __attribute__((noinline))\n"
            "#endif\n"
            "typedef struct recomp_context { int unused; } recomp_context;\n"
            "typedef void recomp_func_t(uint8_t*, recomp_context*);\n"
            "extern int32_t* section_addresses;\n"
            "#endif\n",
            encoding="utf-8",
            newline="\n",
        )
        return argparse.Namespace(
            raw_generated=raw,
            symbols=symbols,
            original_context=context,
            runtime_manifest=runtime,
            recomp_header=header,
            output=root / "output-a",
        )

    def normalize_fixture(self, arguments: argparse.Namespace) -> dict[str, int]:
        with mock.patch.multiple(
            generated_root,
            EXPECTED_AUTHORITATIVE_BODY_COUNT=1,
            EXPECTED_SUPPORT_THUNK_COUNT=0,
            EXPECTED_SECTION_COUNT=1,
            EXPECTED_R32_RELOCATION_COUNT=1,
            EXPECTED_OVERLAY_SLOT_COUNT=0,
            EXPECTED_EMPTY_OVERLAY_SLOT_COUNT=0,
            EXPECTED_EXECUTABLE_SYMBOL_COUNT=1,
            EXPECTED_COVERED_ALIAS_COUNT=0,
            EXPECTED_COVERED_ALIAS_BODY_START_COUNT=0,
            EXPECTED_COVERED_ALIAS_INTERIOR_COUNT=0,
            EXPECTED_MANUAL_SIZE_RECOVERY_COUNT=0,
            EXPECTED_R26_RELOCATION_COUNT=1,
            EXPECTED_DIRECT_CALL_CANDIDATE_COUNT=1,
            EXPECTED_DIRECT_LINKED_CALL_CANDIDATE_COUNT=1,
            EXPECTED_DIRECT_TAIL_CANDIDATE_COUNT=0,
            EXPECTED_DIRECT_GENERATED_TARGET_CANDIDATE_COUNT=1,
            EXPECTED_DIRECT_UNRESOLVED_TARGET_CANDIDATE_COUNT=0,
            EXPECTED_DIRECT_TRANSFER_ROLE_COUNTS={"linked-call": 1, "direct-tail": 0},
            EXPECTED_DIRECT_INSTRUCTION_CLASS_COUNTS={"jal": 1, "bgezal": 0, "j": 0, "conditional-branch": 0},
            EXPECTED_INSTRUCTION_RELOCATION_COUNT=2,
            EXPECTED_HI_LO_PAIR_COUNT=1,
            EXPECTED_INDIRECT_TRANSFER_SITE_COUNT=0,
            EXPECTED_NATIVE_RETURN_SITE_COUNT=0,
            EXPECTED_INDIRECT_DECISION_SITE_COUNT=0,
        ):
            return generated_root.normalize(arguments)

    @staticmethod
    def tree_digest(root: Path) -> str:
        digest = hashlib.sha256()
        for path in sorted(item for item in root.rglob("*") if item.is_file()):
            digest.update(path.relative_to(root).as_posix().encode("ascii"))
            digest.update(b"\0")
            digest.update(path.read_bytes())
        return digest.hexdigest()

    def test_normalizer_emits_closed_generated_tables(self) -> None:
        with tempfile.TemporaryDirectory(prefix="generated-root-test-", dir=TEST_TEMP_ROOT) as temp:
            arguments = self.make_inputs(Path(temp))
            summary = self.normalize_fixture(arguments)

            self.assertEqual(
                summary,
                {
                    "authoritative_bodies": 1,
                    "callable_wrappers": 1,
                    "alternate_entry_thunks": 0,
                    "table_support_members": 11,
                    "baseline_members": 13,
                    "indirect_decision_sites": 0,
                    "indirect_decision_emissions": 0,
                },
            )
            output = arguments.output
            header = (output / "funcs.h").read_text(encoding="utf-8")
            metadata = (output / "support" / "section_metadata.c").read_text(
                encoding="utf-8"
            )
            lookup = (output / "support" / "lookup_table.c").read_text(encoding="utf-8")
            relocations = (output / "support" / "relocation_checked.c").read_text(
                encoding="utf-8"
            )
            relocation_sites = (output / "support" / "relocation_sites.c").read_text(
                encoding="utf-8"
            )
            relocation_descriptors = (
                output / "support" / "relocation_descriptors.c"
            ).read_text(encoding="utf-8")
            self.assertIn("JfgGeneratedSectionMetadata", header)
            self.assertIn("jfg_generated_section_metadata", metadata)
            self.assertIn("if (base == 0u)", lookup)
            self.assertIn("kLookupSlots", lookup)
            self.assertIn("jfg_generated_lookup_dirty", lookup)
            self.assertIn("jfg_generated_apply_relocations_checked", relocations)
            self.assertNotIn("memcpy", relocations)
            self.assertNotIn("*(uint32_t*)", relocations)
            self.assertIn("rdram[offset + 0u] = (uint8_t)(target >> 24);", relocations)
            self.assertIn("rdram[offset + 1u] = (uint8_t)(target >> 16);", relocations)
            self.assertIn("rdram[offset + 2u] = (uint8_t)(target >> 8);", relocations)
            self.assertIn("rdram[offset + 3u] = (uint8_t)target;", relocations)
            self.assertIn("jfg_generated_relocation_sites", header)
            self.assertIn("jfg_generated_relocation_descriptors", header)
            self.assertIn("kR32SiteRanges", relocation_sites)
            self.assertIn("kR32SiteOffsets", relocation_sites)
            self.assertIn("range.count == 0u ? NULL", relocation_sites)
            self.assertIn("kR32Descriptors", relocation_descriptors)
            self.assertIn("JfgGeneratedR32Descriptor", header)
            self.assertIn("jfg_generated_relocation_sites", (output / "audit" / "link_smoke_registry.cpp").read_text(encoding="utf-8"))
            self.assertIn("jfg_generated_relocation_descriptors", (output / "audit" / "link_smoke_registry.cpp").read_text(encoding="utf-8"))
            registry = (output / "audit" / "link_smoke_registry.cpp").read_text(encoding="utf-8")
            self.assertIn("g_data_sink = &jfg_generated_lookup_dirty;", registry)
            self.assertIn(
                "auto* volatile overlay_slot_reference = &jfg_generated_overlay_slot_section;",
                registry,
            )
            self.assertIn("(void)overlay_slot_reference;", registry)
            manifest_bytes = (output / "sources.json").read_bytes()
            manifest = json.loads(manifest_bytes)
            self.assertEqual(manifest_bytes, generated_root.canonical_bytes(manifest))
            self.assertEqual(manifest["version"], 6)
            self.assertEqual(manifest["alternate_entry_thunk_sources"], [])
            self.assertEqual(len(manifest["table_support_sources"]), 11)
            self.assertEqual(
                manifest["normalizer_revision_sha256"],
                hashlib.sha256(generated_root.NORMALIZER_PATH.read_bytes()).hexdigest(),
            )
            inventory = (output / "cpu_section_inventory.json").read_bytes()
            symbol_inventory_bytes = (output / "symbol_inventory.json").read_bytes()
            self.assertEqual(
                symbol_inventory_bytes,
                generated_root.canonical_bytes(json.loads(symbol_inventory_bytes)),
            )
            for product_path in output.glob("*.json"):
                with self.subTest(canonical_product=product_path.name):
                    payload = product_path.read_bytes()
                    self.assertEqual(
                        payload,
                        generated_root.canonical_bytes(json.loads(payload)),
                    )
            self.assertEqual(
                manifest["cpu_section_inventory"], "cpu_section_inventory.json"
            )
            self.assertEqual(
                manifest["cpu_section_inventory_sha256"],
                hashlib.sha256(inventory).hexdigest(),
            )
            self.assertEqual(
                json.loads(inventory),
                {
                    "schema_version": 2,
                    "kind": "jfg-phase4-cpu-section-inventory",
                    "sections": [
                        {
                            "section_id": "section-000",
                            "kind": "main",
                            "expected": 1,
                            "generated": 1,
                            "excluded": 0,
                            "lookups": 1,
                            "lifecycle": 0,
                            "relocations": 1,
                        }
                    ],
                    "overlay_slots": [],
                },
            )

    def test_authoritative_context_cannot_add_an_unrepresented_function(self) -> None:
        with tempfile.TemporaryDirectory(prefix="generated-root-test-", dir=TEST_TEMP_ROOT) as temp:
            arguments = self.make_inputs(Path(temp))
            arguments.original_context.write_text(
                arguments.original_context.read_text(encoding="utf-8").replace(
                    '  { name = "fn_000_0000", vram = 0x80000400, size = 4 },\n',
                    '  { name = "fn_000_0000", vram = 0x80000400, size = 4 },\n'
                    '  { name = "fn_000_0001", vram = 0x80000404, size = 4 },\n',
                ),
                encoding="utf-8",
                newline="\n",
            )
            with self.assertRaisesRegex(
                generated_root.NormalizationError, "function denominators"
            ):
                self.normalize_fixture(arguments)
            self.assertFalse(arguments.output.exists())

    def test_cpu_section_inventory_is_canonical_and_requires_every_section(self) -> None:
        rows = [
            {"name": "fn_000_0000", "section": 0, "vram": 4, "size": 4},
            {"name": "fn_000_0001", "section": 1, "vram": 12, "size": 4},
        ]
        runtime = {
            "sections": [{"kind": "main"}, {"kind": "overlay"}],
            "r_mips_32": [{"source_section": 1}],
            "semantic_ledgers": {"covered_alias_ledger": []},
            "overlay_slots": [
                {"slot": 1, "section": 1, "disposition": "populated"}
            ],
        }
        authoritative = {(4, 4), (12, 4)}
        with (
            mock.patch.object(generated_root, "EXPECTED_OVERLAY_SLOT_COUNT", 1),
            mock.patch.object(generated_root, "EXPECTED_EMPTY_OVERLAY_SLOT_COUNT", 0),
        ):
            first = generated_root.make_cpu_section_inventory(rows, authoritative, runtime)
            second = generated_root.make_cpu_section_inventory(rows, authoritative, runtime)
            self.assertEqual(first, second)
            document = json.loads(first)
            self.assertEqual([row["section_id"] for row in document["sections"]], ["section-000", "section-001"])
            self.assertEqual([row["relocations"] for row in document["sections"]], [0, 1])
            with self.assertRaisesRegex(generated_root.NormalizationError, "denominator"):
                generated_root.make_cpu_section_inventory(rows[:1], {(4, 4)}, runtime)

    def test_switch_targets_must_remain_in_source_executable_text(self) -> None:
        callable_member = {
            "generated_function": "fn_000_0000",
            "source_section": 0,
            "source_offset": 0,
            "size": 12,
        }
        function_rows = [
            {"name": "fn_000_0000", "section": 0, "offset": 0, "size": 12}
        ]
        discovery = [
            {
                "source_register": register,
                "private_site": {"section": 0, "offset": offset},
            }
            for offset, register in ((0, 2), (4, 3), (8, 4))
        ]
        runtime = {
            "sections": [
                {"text_offset": 0, "text_size": 12},
                {"text_offset": 0, "text_size": 12},
            ],
            "semantic_ledgers": {
                "indirect_transfer_ledger": discovery,
                "direct_call_candidate_ledger": [],
            },
        }
        decisions = [
            {
                "classification": "bounded-switch",
                "default_disposition": "fail-closed-switch-error",
                "generated_function": "fn_000_0000",
                "range": {
                    "kind": "exact-target-member-set",
                    "members": [{"target_section": 0, "target_offset": 0}],
                },
                "source_offset": 0,
                "source_register": 2,
                "source_section": 0,
            },
            {
                "classification": "dynamic-call",
                "default_disposition": "fail-closed-lookup-miss",
                "generated_function": "fn_000_0000",
                "range": {"kind": "generated-callable-set"},
                "source_offset": 4,
                "source_register": 3,
                "source_section": 0,
            },
            {
                "classification": "dynamic-tail",
                "default_disposition": "fail-closed-lookup-miss",
                "generated_function": "fn_000_0000",
                "range": {"kind": "generated-callable-set"},
                "source_offset": 8,
                "source_register": 4,
                "source_section": 0,
            },
        ]
        base = {
            "schema_version": 1,
            "kind": "n64recomp-indirect-decision-sidecar",
            "generated_callable_set": {
                "kind": "exact-generated-callable-entry-set",
                "member_count": 1,
                "members": [callable_member],
            },
            "decisions": decisions,
            "direct_calls": [],
        }
        with tempfile.TemporaryDirectory(
            prefix="generated-switch-target-", dir=TEST_TEMP_ROOT
        ) as temporary:
            raw = Path(temporary)
            path = raw / "indirect_decisions.json"
            with (
                mock.patch.object(generated_root, "EXPECTED_SECTION_COUNT", 2),
                mock.patch.object(
                    generated_root, "EXPECTED_INDIRECT_TRANSFER_SITE_COUNT", 3
                ),
                mock.patch.object(generated_root, "EXPECTED_NATIVE_RETURN_SITE_COUNT", 0),
                mock.patch.object(generated_root, "EXPECTED_INDIRECT_DECISION_SITE_COUNT", 3),
                mock.patch.object(generated_root, "EXPECTED_DIRECT_CALL_CANDIDATE_COUNT", 0),
                mock.patch.object(generated_root, "EXPECTED_DIRECT_GENERATED_TARGET_CANDIDATE_COUNT", 0),
            ):
                path.write_bytes(generated_root.canonical_bytes(base))
                generated_root.load_n64recomp_decision_sidecar(
                    raw, {"fn_000_0000": "definition"}, function_rows, runtime
                )
                guarded = json.loads(generated_root.canonical_bytes(base))
                guarded["schema_version"] = 2
                guarded["decisions"][2]["classification"] = "dynamic-tail-or-return"
                guarded["decisions"][2]["range"] = {
                    "kind": "generated-callable-or-matching-incoming-link"}
                guarded_body = ("const gpr guest_return_link = ctx->r31;\n"
                                "if (guest_call_target == guest_return_link) return;")
                path.write_bytes(generated_root.canonical_bytes(guarded))
                generated_root.load_n64recomp_decision_sidecar(
                    raw, {"fn_000_0000": guarded_body}, function_rows, runtime)
                for mutation in ("old-schema", "unbounded-range", "missing-guard"):
                    malformed = json.loads(generated_root.canonical_bytes(guarded))
                    body = guarded_body
                    if mutation == "old-schema":
                        malformed["schema_version"] = 1
                    elif mutation == "unbounded-range":
                        malformed["decisions"][2]["range"] = {"kind": "any-address"}
                    else:
                        body = "definition"
                    path.write_bytes(generated_root.canonical_bytes(malformed))
                    with self.subTest(guarded=mutation), self.assertRaises(generated_root.NormalizationError):
                        generated_root.load_n64recomp_decision_sidecar(
                            raw, {"fn_000_0000": body}, function_rows, runtime)
                for label, target_section, target_offset in (
                    ("cross-section", 1, 0),
                    ("past-text", 0, 12),
                ):
                    with self.subTest(label=label):
                        malformed = json.loads(generated_root.canonical_bytes(base))
                        target = malformed["decisions"][0]["range"]["members"][0]
                        target["target_section"] = target_section
                        target["target_offset"] = target_offset
                        path.write_bytes(generated_root.canonical_bytes(malformed))
                        with self.assertRaisesRegex(
                            generated_root.NormalizationError,
                            "sidecar|executable|bounded switch",
                        ):
                            generated_root.load_n64recomp_decision_sidecar(
                                raw,
                                {"fn_000_0000": "definition"},
                                function_rows,
                                runtime,
                            )

    def test_two_fresh_outputs_are_byte_deterministic(self) -> None:
        with tempfile.TemporaryDirectory(prefix="generated-root-test-", dir=TEST_TEMP_ROOT) as temp:
            arguments = self.make_inputs(Path(temp))
            self.normalize_fixture(arguments)
            second = argparse.Namespace(**vars(arguments))
            second.output = Path(temp) / "output-b"
            self.normalize_fixture(second)
            self.assertEqual(
                self.tree_digest(arguments.output),
                self.tree_digest(second.output),
            )

    def test_initializer_binds_caller_storage_only_after_success(self) -> None:
        pinned = ROOT / "tools" / "build" / "llvm-22.1.8" / "bin" / "clang.exe"
        compiler = (
            str(pinned)
            if pinned.is_file()
            else shutil.which("clang") or shutil.which("gcc")
        )
        if compiler is None:
            self.skipTest("a C compiler is unavailable")
        with tempfile.TemporaryDirectory(prefix="generated-root-test-", dir=TEST_TEMP_ROOT) as temp:
            arguments = self.make_inputs(Path(temp))
            self.normalize_fixture(arguments)
            driver = Path(temp) / "storage_binding.c"
            driver.write_text(
                '#include "funcs.h"\n'
                '\n'
                'int32_t* section_addresses = NULL;\n'
                'void fn_000_0000(uint8_t* rdram, recomp_context* context) {\n'
                '    (void)rdram;\n'
                '    (void)context;\n'
                '}\n'
                '\n'
                'int main(void) {\n'
                '    int32_t stable[1] = {INT32_C(0x13579BDF)};\n'
                '    int32_t supplied[1] = {0};\n'
                '    section_addresses = stable;\n'
                '    if (jfg_generated_initialize_sections(NULL, 1u) != 0 ||\n'
                '        section_addresses != stable || stable[0] != INT32_C(0x13579BDF)) {\n'
                '        return 1;\n'
                '    }\n'
                '    if (jfg_generated_initialize_sections(supplied, 0u) != 0 ||\n'
                '        section_addresses != stable) {\n'
                '        return 2;\n'
                '    }\n'
                '    if (jfg_generated_initialize_sections(supplied, 1u) == 0 ||\n'
                '        section_addresses != supplied ||\n'
                '        supplied[0] != (int32_t)UINT32_C(0x80000400)) {\n'
                '        return 3;\n'
                '    }\n'
                '    if (jfg_generated_lookup_function((int32_t)UINT32_C(0x80000400)) != fn_000_0000) {\n'
                '        return 4;\n'
                '    }\n'
                '    if (jfg_generated_section_lifecycle(2u, 0u, 0) != 0 ||\n'
                '        supplied[0] != 0 ||\n'
                '        jfg_generated_lookup_function((int32_t)UINT32_C(0x80000400)) != NULL) {\n'
                '        return 5;\n'
                '    }\n'
                '    if (jfg_generated_section_lifecycle(1u, 0u, INT32_C(0x80000400)) != 0 ||\n'
                '        supplied[0] != (int32_t)UINT32_C(0x80000400) ||\n'
                '        jfg_generated_lookup_function((int32_t)UINT32_C(0x80000400)) != fn_000_0000) {\n'
                '        return 6;\n'
                '    }\n'
                '    return 0;\n'
                '}\n',
                encoding="utf-8",
                newline="\n",
            )
            executable = Path(temp) / "storage_binding.exe"
            built = subprocess.run(
                [
                    compiler,
                    "-std=c11",
                    "-O2",
                    "-Wall",
                    "-Wextra",
                    "-Wpedantic",
                    "-Werror",
                    "-Wno-microsoft-include",
                    "-I",
                    str(arguments.output),
                    "-I",
                    str(arguments.output / "include"),
                    str(arguments.output / "support" / "lookup_table.c"),
                    str(arguments.output / "support" / "section_count.c"),
                    str(arguments.output / "support" / "section_initializer.c"),
                    str(arguments.output / "support" / "lifecycle_table.c"),
                    str(driver),
                    "-o",
                    str(executable),
                ],
                cwd=ROOT,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
                timeout=120,
            )
            self.assertEqual(built.returncode, 0, built.stderr.decode(errors="replace"))
            ran = subprocess.run(
                [str(executable)],
                cwd=ROOT,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
                timeout=30,
            )
            self.assertEqual(ran.returncode, 0, ran.stderr.decode(errors="replace"))

    def test_relocation_site_accessor_groups_sections_and_marks_empty_spans(self) -> None:
        manifest = {
            "sections": [{}, {}],
            "r_mips_32": [
                {"source_section": 1, "site_offset": 8},
                {"source_section": 1, "site_offset": 4},
            ],
        }
        rendered = generated_root.make_relocation_sites(manifest)
        count_wrapper = generated_root.make_relocation_count(manifest)
        self.assertIn("UINT32_C(0x00000004)", rendered)
        self.assertIn("UINT32_C(0x00000008)", rendered)
        self.assertLess(
            rendered.index("UINT32_C(0x00000004)"),
            rendered.index("UINT32_C(0x00000008)"),
        )
        self.assertIn("{0u, 0u}", rendered)
        self.assertIn("{0u, 2u}", rendered)
        self.assertIn("*output = range.count == 0u ? NULL", rendered)
        self.assertIn("jfg_generated_relocation_sites", count_wrapper)
        descriptors = generated_root.make_relocation_descriptors(
            {
                "sections": [{}, {}],
                "r_mips_32": [
                    {
                        "source_section": 1,
                        "site_offset": 8,
                        "target_section": 0,
                        "target_section_offset": 12,
                    },
                    {
                        "source_section": 1,
                        "site_offset": 4,
                        "target_section": 1,
                        "target_section_offset": 16,
                    },
                ],
            }
        )
        self.assertIn("JfgGeneratedR32Descriptor", descriptors)
        self.assertLess(
            descriptors.index("UINT32_C(0x00000004)"),
            descriptors.index("UINT32_C(0x00000008)"),
        )
        self.assertIn("UINT32_C(0x00000010)", descriptors)

    @staticmethod
    def runtime_manifest(
        *,
        linked_vram: int = 0x80000400,
        text_offset: int = 0,
        text_size: int = 4,
        data_size: int = 0,
        bss_size: int = 0,
        site_offset: int = 0,
        target_offset: int = 0,
    ) -> dict[str, object]:
        exception_rows = []
        contracts = (
            (
                "exception-000000000000000000000001",
                "anomalous-r-mips-26",
                "direct-call",
                "out-of-domain-overlay-reference",
                "fail-closed-trap",
            ),
            (
                "exception-000000000000000000000002",
                "reserved-atomic-hi-lo",
                "instruction-relocation",
                "reserved-reference-class",
                "unresolved-data-atomic-pair",
            ),
            (
                "exception-000000000000000000000003",
                "reserved-atomic-hi-lo",
                "instruction-relocation",
                "reserved-reference-class",
                "unresolved-data-atomic-pair",
            ),
        )
        for opaque_id, category, evidence_class, reason, disposition in contracts:
            exception_rows.append(
                {
                    "opaque_id": opaque_id,
                    "category": category,
                    "evidence_class": evidence_class,
                    "owner_role": "transformer",
                    "reason": reason,
                    "disposition": disposition,
                    "approval_sha256": generated_root.policy_approval_digest(
                        "phase4-transform-exception-policy-v1",
                        opaque_id,
                        category,
                        disposition,
                    ),
                    "private_record": {},
                }
            )
        empty_digest = generated_root.canonical_digest([])
        direct_candidates = [
            {
                "opaque_id": "direct-call-000000000000000000000001",
                "source_section": 0,
                "source_offset": 0,
                "transfer_role": "linked-call",
                "instruction_class": "jal",
                "decoded_target_vram": linked_vram,
                "target_section": 0,
                "target_offset": 0,
                "target_class": "authoritative-body",
                "disposition": "approved-fail-closed-trap",
            }
        ]
        return {
            "schema_version": 1,
            "sections": [
                {
                    "section": 0,
                    "module": 0,
                    "kind": "main",
                    "rom": 0,
                    "linked_vram": linked_vram,
                    "text_offset": text_offset,
                    "text_size": text_size,
                    "data_size": data_size,
                    "bss_size": bss_size,
                }
            ],
            "overlay_slots": [],
            "audit": {
                "overlay_slot_count": 0,
                "empty_overlay_slot_count": 0,
                "exception_ledger_sha256": generated_root.canonical_digest(exception_rows),
                "exception_approval_set_sha256": generated_root.canonical_digest(
                    sorted(row["approval_sha256"] for row in exception_rows)
                ),
                "exception_category_counts": {
                    "anomalous-r-mips-26": 1,
                    "reserved-atomic-hi-lo": 2,
                },
                "covered_alias_count": 0,
                "covered_alias_body_start_count": 0,
                "covered_alias_interior_count": 0,
                "covered_alias_ledger_sha256": empty_digest,
                "covered_alias_approval_set_sha256": empty_digest,
                "manual_size_recovery_count": 0,
                "manual_size_recovery_ledger_sha256": empty_digest,
                "manual_size_recovery_approval_set_sha256": empty_digest,
                "direct_call_candidate_count": 1,
                "direct_linked_call_candidate_count": 1,
                "direct_tail_candidate_count": 0,
                "direct_instruction_class_counts": {
                    "bgezal": 0,
                    "conditional-branch": 0,
                    "j": 0,
                    "jal": 1,
                },
                "direct_generated_target_candidate_count": 1,
                "direct_unresolved_target_candidate_count": 0,
                "direct_pending_n64recomp_observation_count": 1,
                "direct_call_candidate_ledger_sha256": generated_root.canonical_digest(direct_candidates),
                "direct_call_approval_set_sha256": empty_digest,
                "r26_relocation_count": 1,
                "r26_relocation_resolved_count": 0,
                "r26_relocation_exception_count": 1,
                "instruction_relocation_count": 2,
                "instruction_resolved_count": 0,
                "instruction_fail_closed_count": 2,
                "instruction_relocation_ledger_sha256": empty_digest,
                "instruction_relocation_approval_set_sha256": empty_digest,
                "hi_lo_pair_count": 1,
                "r32_count": 1,
                "r32_ledger_sha256": empty_digest,
                "r32_approval_set_sha256": empty_digest,
                "indirect_transfer_ledger_sha256": empty_digest,
                "indirect_transfer_count": 0,
                "indirect_native_return_candidate_count": 0,
                "indirect_decision_candidate_count": 0,
                "indirect_pending_n64recomp_observation_count": 0,
                "stub_ledger_sha256": empty_digest,
            },
            "semantic_ledgers": {
                "exception_ledger": exception_rows,
                "covered_alias_ledger": [],
                "manual_size_recovery_ledger": [],
                "direct_call_candidate_ledger": direct_candidates,
                "indirect_transfer_ledger": [],
            },
            "r_mips_32": [
                {
                    "source_section": 0,
                    "source_type": 1,
                    "site_offset": site_offset,
                    "target_class": "local-offset",
                    "target_section": 0,
                    "target_section_offset": target_offset,
                }
            ],
        }

    def validate_runtime_fixture(self, document: dict[str, object], base: int) -> None:
        with mock.patch.multiple(
            generated_root,
            EXPECTED_SECTION_COUNT=1,
            EXPECTED_R32_RELOCATION_COUNT=1,
            EXPECTED_OVERLAY_SLOT_COUNT=0,
            EXPECTED_EMPTY_OVERLAY_SLOT_COUNT=0,
            EXPECTED_EXECUTABLE_SYMBOL_COUNT=1,
            EXPECTED_COVERED_ALIAS_COUNT=0,
            EXPECTED_COVERED_ALIAS_BODY_START_COUNT=0,
            EXPECTED_COVERED_ALIAS_INTERIOR_COUNT=0,
            EXPECTED_MANUAL_SIZE_RECOVERY_COUNT=0,
            EXPECTED_R26_RELOCATION_COUNT=1,
            EXPECTED_DIRECT_CALL_CANDIDATE_COUNT=1,
            EXPECTED_DIRECT_LINKED_CALL_CANDIDATE_COUNT=1,
            EXPECTED_DIRECT_TAIL_CANDIDATE_COUNT=0,
            EXPECTED_DIRECT_GENERATED_TARGET_CANDIDATE_COUNT=1,
            EXPECTED_DIRECT_UNRESOLVED_TARGET_CANDIDATE_COUNT=0,
            EXPECTED_DIRECT_TRANSFER_ROLE_COUNTS={"linked-call": 1, "direct-tail": 0},
            EXPECTED_DIRECT_INSTRUCTION_CLASS_COUNTS={"jal": 1, "bgezal": 0, "j": 0, "conditional-branch": 0},
            EXPECTED_INSTRUCTION_RELOCATION_COUNT=2,
            EXPECTED_HI_LO_PAIR_COUNT=1,
            EXPECTED_INDIRECT_TRANSFER_SITE_COUNT=0,
            EXPECTED_NATIVE_RETURN_SITE_COUNT=0,
            EXPECTED_INDIRECT_DECISION_SITE_COUNT=0,
        ):
            generated_root.validate_runtime_manifest(document, [base])

    def test_runtime_manifest_rejects_out_of_extent_and_wrapping_relocation_targets(self) -> None:
        out_of_extent = self.runtime_manifest(target_offset=4)
        with self.assertRaisesRegex(generated_root.NormalizationError, "runtime manifest"):
            self.validate_runtime_fixture(out_of_extent, 0x80000400)

        wrapping_target = self.runtime_manifest(
            linked_vram=0xFFFFFFFC,
            text_size=8,
            target_offset=4,
        )
        with self.assertRaisesRegex(generated_root.NormalizationError, "runtime manifest"):
            self.validate_runtime_fixture(wrapping_target, 0xFFFFFFFC)

        bss_target = self.runtime_manifest(text_size=4, bss_size=4, target_offset=4)
        self.validate_runtime_fixture(bss_target, 0x80000400)

    def test_inventory_ranges_must_fit_their_declared_text_extent_without_wrap(self) -> None:
        document = self.runtime_manifest(text_size=8)
        rows = [
            {
                "name": "fn_000_0000",
                "section": 0,
                "offset": 4,
                "vram": 0x80000404,
                "size": 4,
            }
        ]
        generated_root.validate_function_inventory_extents(rows, document)

        prefixed = self.runtime_manifest(text_offset=4, text_size=4)
        generated_root.validate_function_inventory_extents(rows, prefixed)
        metadata = generated_root.make_section_metadata(prefixed)
        self.assertIn("UINT32_C(0x00000008)", metadata)

        out_of_text = [dict(rows[0], offset=8, vram=0x80000408)]
        with self.assertRaisesRegex(generated_root.NormalizationError, "symbol inventory"):
            generated_root.validate_function_inventory_extents(out_of_text, document)

        overflowing = [
            {
                "name": "fn_000_0000",
                "section": 0,
                "offset": 0,
                "vram": 0xFFFFFFFC,
                "size": 8,
            }
        ]
        overflow_manifest = self.runtime_manifest(linked_vram=0xFFFFFFFC, text_size=8)
        with self.assertRaisesRegex(generated_root.NormalizationError, "symbol inventory"):
            generated_root.validate_function_inventory_extents(overflowing, overflow_manifest)

    def test_existing_or_tracked_output_is_rejected_before_mutation(self) -> None:
        with tempfile.TemporaryDirectory(prefix="generated-root-test-", dir=TEST_TEMP_ROOT) as temp:
            arguments = self.make_inputs(Path(temp))
            arguments.output.mkdir()
            marker = arguments.output / "preserve.txt"
            marker.write_text("preserve", encoding="utf-8")
            with self.assertRaisesRegex(generated_root.NormalizationError, "output boundary"):
                self.normalize_fixture(arguments)
            self.assertEqual(marker.read_text(encoding="utf-8"), "preserve")

        tracked_candidate = ROOT / "tests" / "generated-root-must-not-exist"
        self.assertFalse(tracked_candidate.exists())
        with self.assertRaisesRegex(generated_root.NormalizationError, "output boundary"):
            generated_root._require_new_private_output(tracked_candidate)

    def test_wrong_header_lineage_is_rejected_before_output_creation(self) -> None:
        with tempfile.TemporaryDirectory(prefix="generated-root-test-", dir=TEST_TEMP_ROOT) as temp:
            weak_root = Path(temp) / "weak"
            weak_root.mkdir()
            weak_arguments = self.make_inputs(weak_root)
            weak_arguments.recomp_header.write_text(
                weak_arguments.recomp_header.read_text(encoding="utf-8").replace(
                    "#define RECOMP_FUNC __attribute__((noinline))",
                    "#define RECOMP_FUNC extern inline __attribute__((weak,noinline))",
                ),
                encoding="utf-8",
                newline="\n",
            )
            with self.assertRaisesRegex(generated_root.NormalizationError, "header lineage"):
                self.normalize_fixture(weak_arguments)
            self.assertFalse(weak_arguments.output.exists())

            missing_root = Path(temp) / "missing-div"
            missing_root.mkdir()
            missing_arguments = self.make_inputs(missing_root)
            raw_source = missing_arguments.raw_generated / "funcs_0.c"
            raw_source.write_text(
                raw_source.read_text(encoding="utf-8").replace(
                    "    (void)ctx;",
                    "    DIV32(1, 1);",
                ),
                encoding="utf-8",
                newline="\n",
            )
            with self.assertRaisesRegex(generated_root.NormalizationError, "header lineage"):
                self.normalize_fixture(missing_arguments)
            self.assertFalse(missing_arguments.output.exists())

    def test_duplicate_runtime_manifest_key_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="generated-root-test-", dir=TEST_TEMP_ROOT) as temp:
            duplicate = Path(temp) / "duplicate.json"
            duplicate.write_text(
                '{"schema_version":1,"schema_version":1}',
                encoding="utf-8",
                newline="\n",
            )
            with self.assertRaisesRegex(generated_root.NormalizationError, "runtime manifest"):
                generated_root._load_json(duplicate)


if __name__ == "__main__":
    unittest.main()
