from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
import tempfile
import unittest
import zipfile
from io import BytesIO
from pathlib import Path
from unittest import mock

import scripts.phase4_evidence_harness as harness


class Phase4ProductionHarnessTests(unittest.TestCase):
    @staticmethod
    def canonical(value: object) -> bytes:
        return harness._canonical_bytes(value)

    @staticmethod
    def digest(payload: bytes) -> str:
        return hashlib.sha256(payload).hexdigest()

    def inventory(self, files: dict[str, bytes]) -> bytes:
        return self.canonical(
            {
                "schema_version": 1,
                "kind": "jfg-phase4-file-inventory",
                "files": [
                    {
                        "path": path,
                        "sha256": self.digest(files[path]),
                        "size": len(files[path]),
                    }
                    for path in sorted(files)
                ],
            }
        )

    def member_inventory(self, members: list[dict[str, object]]) -> bytes:
        return self.canonical(
            {
                "schema_version": 1,
                "kind": "jfg-phase4-archive-member-inventory",
                "members": members,
            }
        )

    def archive(self, name: str = "unit.o", body: bytes = b"\x7fELFobject") -> tuple[bytes, bytes]:
        encoded_name = f"{name}/".encode("ascii").ljust(16, b" ")
        header = b"".join(
            (
                encoded_name,
                b"0".ljust(12, b" "),
                b"0".ljust(6, b" "),
                b"0".ljust(6, b" "),
                b"100644".ljust(8, b" "),
                str(len(body)).encode("ascii").ljust(10, b" "),
                b"`\n",
            )
        )
        archive = b"!<arch>\n" + header + body + (b"\n" if len(body) & 1 else b"")
        members = [{"name": name, "sha256": self.digest(body), "size": len(body)}]
        return archive, self.member_inventory(members)

    def zip_product(self, files: dict[str, bytes]) -> bytes:
        buffer = BytesIO()
        with zipfile.ZipFile(buffer, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for path in sorted(files):
                info = zipfile.ZipInfo(path)
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o100644 << 16
                archive.writestr(info, files[path])
        return buffer.getvalue()

    def cpu_inventory(self, rows: list[dict[str, object]] | None = None) -> bytes:
        if rows is None:
            rows = [
                {
                    "section_id": "section-000",
                    "kind": "main",
                    "expected": 1,
                    "generated": 1,
                    "excluded": 0,
                    "lookups": 1,
                    "lifecycle": 0,
                    "relocations": 0,
                },
                {
                    "section_id": "section-001",
                    "kind": "overlay",
                    "expected": 1,
                    "generated": 1,
                    "excluded": 0,
                    "lookups": 1,
                    "lifecycle": 1,
                    "relocations": 1,
                },
            ]
        return self.canonical(
            {
                "schema_version": 2,
                "kind": "jfg-phase4-cpu-section-inventory",
                "sections": rows,
                "overlay_slots": [
                    {
                        "slot_id": "slot-001",
                        "disposition": "populated",
                        "section_id": "section-001",
                    },
                    {
                        "slot_id": "slot-002",
                        "disposition": "empty-fail-closed",
                        "section_id": None,
                    },
                ],
            }
        )

    def generation_fixture(
        self, cpu_inventory: bytes | None = None
    ) -> tuple[dict[str, bytes], dict[str, object]]:
        inventory = cpu_inventory or self.cpu_inventory()
        generation_inputs = {
            "patched.z64": b"private patched image",
            "symbols.toml": b"[section]\n",
            "original_context.toml": b"[section]\n",
            "runtime_manifest.json": b"{}\n",
            "recomp.h": b"typedef int recomp_context;\n",
            "input.elf": b"private authoritative ELF",
            "original.z64": b"private original ROM",
            "overlay-layout.json": b"{}\n",
            "data_context.toml": b"[data]\n",
        }
        opaque_symbols = (b"fn_000_0000", b"fn_000_0001")
        symbol_inventory = self.canonical({"version": 2, "fixture": True})
        alternate_opaque_id = "alternate-000000000000000000000001"
        alternate_approval = harness._policy_approval_digest(
            "phase4-alternate-entry-policy-v1",
            alternate_opaque_id,
            "validated-control-transfer-entry",
            "deterministic-generated-thunk",
        )
        alternate = self.canonical(
            {
                "schema_version": 1,
                "kind": "jfg-phase4-alternate-entry-ledger",
                "policy_id": "phase4-alternate-entry-policy-v1",
                "entry_count": 1,
                "reason_counts": {
                    "validated-control-transfer-entry": 1,
                    "zero-size-covered-address": 0,
                },
                "approval_set_sha256": self.digest(
                    self.canonical([alternate_approval])
                ),
                "entries": [
                    {
                        "opaque_id": alternate_opaque_id,
                        "section_id": "section-001",
                        "evidence_class": "control-transfer-entry",
                        "owner_role": "alternate-entry-thunk",
                        "reason_code": "validated-control-transfer-entry",
                        "disposition": "deterministic-generated-thunk",
                        "approval_sha256": alternate_approval,
                    }
                ],
            }
        )
        aliases = self.canonical(
            {
                "schema_version": 1,
                "kind": "jfg-phase4-covered-alias-ledger",
                "policy_id": "phase4-covered-alias-policy-v1",
                "entry_count": 0,
                "mapping_role_counts": {
                    "authoritative-body": 0,
                    "alternate-entry-thunk": 0,
                },
                "source_ledger_sha256": self.digest(self.canonical([])),
                "approval_set_sha256": self.digest(self.canonical([])),
                "entries": [],
            }
        )
        recoveries = self.canonical(
            {
                "schema_version": 1,
                "kind": "jfg-phase4-manual-size-recovery-ledger",
                "policy_id": "phase4-manual-size-recovery-policy-v1",
                "entry_count": 0,
                "source_ledger_sha256": self.digest(self.canonical([])),
                "approval_set_sha256": self.digest(self.canonical([])),
                "entries": [],
            }
        )
        exceptions = self.canonical(
            {
                "schema_version": 1,
                "kind": "jfg-phase4-approved-exception-ledger",
                "policy_id": "phase4-transform-exception-policy-v1",
                "entry_count": 0,
                "category_counts": {
                    "anomalous-r-mips-26": 0,
                    "reserved-atomic-hi-lo": 0,
                },
                "source_ledger_sha256": self.digest(self.canonical([])),
                "approval_set_sha256": self.digest(self.canonical([])),
                "entries": [],
            }
        )
        slots = self.canonical(
            {
                "schema_version": 1,
                "kind": "jfg-phase4-overlay-slot-inventory",
                "executable_section_count": 2,
                "slot_count": 2,
                "populated_slot_count": 1,
                "empty_slot_count": 1,
                "slots": [
                    {
                        "slot_id": "slot-001",
                        "disposition": "populated",
                        "section_id": "section-001",
                        "source_binding_sha256": self.digest(b"slot-1"),
                    },
                    {
                        "slot_id": "slot-002",
                        "disposition": "empty-fail-closed",
                        "section_id": None,
                        "source_binding_sha256": self.digest(b"slot-2"),
                    },
                ],
            }
        )
        lifecycle = self.canonical(
            {
                "schema_version": 1,
                "kind": "jfg-phase4-overlay-lifecycle-inventory",
                "executable_section_count": 2,
                "slot_count": 2,
                "populated_slot_count": 1,
                "empty_slot_count": 1,
                "slots": [
                    {
                        "slot_id": "slot-001",
                        "section_id": "section-001",
                        "disposition": "load-unload-reload",
                    },
                    {
                        "slot_id": "slot-002",
                        "section_id": None,
                        "disposition": "empty-fail-closed",
                    },
                ],
            }
        )
        lookup = self.canonical(
            {
                "schema_version": 1,
                "kind": "jfg-phase4-generated-lookup-inventory",
                "executable_section_count": 2,
                "overlay_slot_count": 2,
                "entry_count": 3,
                "authoritative_entry_count": 2,
                "alternate_entry_count": 1,
                "sections": [
                    {
                        "section_id": "section-000",
                        "authoritative_entry_count": 1,
                        "alternate_entry_count": 0,
                    },
                    {
                        "section_id": "section-001",
                        "authoritative_entry_count": 1,
                        "alternate_entry_count": 1,
                    },
                ],
                "entries": [
                    {
                        "entry_id": "lookup-000000000000000000000001",
                        "section_id": "section-000",
                        "entry_class": "authoritative-body",
                    },
                    {
                        "entry_id": "lookup-000000000000000000000002",
                        "section_id": "section-001",
                        "entry_class": "authoritative-body",
                    },
                    {
                        "entry_id": "lookup-000000000000000000000003",
                        "section_id": "section-001",
                        "entry_class": "alternate-entry",
                    },
                ],
            }
        )
        relocations = self.canonical(
            {
                "schema_version": 1,
                "kind": "jfg-phase4-relocation-inventory",
                "executable_section_count": 2,
                "entry_count": 1,
                "resolved_entry_count": 1,
                "unresolved_entry_count": 0,
                "ledger_sha256": self.digest(b"relocation-ledger"),
                "approval_set_sha256": self.digest(self.canonical([])),
                "sections": [
                    {"section_id": "section-000", "entry_count": 0},
                    {"section_id": "section-001", "entry_count": 1},
                ],
            }
        )
        decisions = self.canonical(
            {
                "schema_version": 1,
                "kind": "n64recomp-indirect-decision-sidecar",
                "generated_callable_set": {
                    "kind": "exact-generated-callable-entry-set",
                    "member_count": 3,
                    "members": [
                        {"generated_function": "fn_000_0000", "source_section": 0, "source_offset": 0, "size": 4},
                        {"generated_function": "fn_000_0001", "source_section": 1, "source_offset": 0, "size": 4},
                        {"generated_function": "fn_999_9999", "source_section": 1, "source_offset": 4, "size": 4},
                    ],
                },
                "decisions": [
                    {
                        "classification": "bounded-switch",
                        "default_disposition": "fail-closed-switch-error",
                        "generated_function": "fn_000_0000",
                        "range": {"kind": "exact-target-member-set", "members": [{"target_section": 0, "target_offset": 0}]},
                        "source_offset": 0,
                        "source_register": 2,
                        "source_section": 0,
                    },
                    {
                        "classification": "dynamic-call",
                        "default_disposition": "fail-closed-lookup-miss",
                        "generated_function": "fn_000_0001",
                        "range": {"kind": "generated-callable-set"},
                        "source_offset": 0,
                        "source_register": 3,
                        "source_section": 1,
                    },
                    {
                        "classification": "dynamic-tail",
                        "default_disposition": "fail-closed-lookup-miss",
                        "generated_function": "fn_999_9999",
                        "range": {"kind": "generated-callable-set"},
                        "source_offset": 4,
                        "source_register": 4,
                        "source_section": 1,
                    },
                ],
                "direct_calls": [],
            }
        )
        generated_inventory_document = {
            "schema_version": 1,
            "kind": "jfg-phase4-generated-inventory",
            "executable_section_count": 2,
            "expected_symbol_count": 2,
            "authoritative_body_count": 2,
            "callable_wrapper_count": 2,
            "alternate_entry_thunk_count": 1,
            "table_support_member_count": 11,
            "generated_game_function_stub_count": 0,
            "covered_alias_count": 0,
            "covered_alias_ledger_sha256": self.digest(aliases),
            "covered_alias_approval_set_sha256": self.digest(self.canonical([])),
            "manual_size_recovery_count": 0,
            "manual_size_recovery_ledger_sha256": self.digest(recoveries),
            "manual_size_recovery_approval_set_sha256": self.digest(self.canonical([])),
            "symbol_inventory_sha256": self.digest(symbol_inventory),
            "alternate_entry_ledger_sha256": self.digest(alternate),
            "alternate_entry_approval_set_sha256": self.digest(
                self.canonical([alternate_approval])
            ),
        }
        generated_inventory = self.canonical(generated_inventory_document)
        public_record = {
            "symbols": {
                "expected_count": 2,
                "generated_count": 2,
                "replaceable_function_count": 2,
                "alternate_entry_thunk_count": 1,
                "excluded_count": 0,
                "unclassified_count": 0,
                "duplicate_native_symbol_count": 0,
                "exclusion_category_counts": {
                    "covered-alias": 0,
                    "validated-non-code": 0,
                    "runtime-abi": 0,
                },
                "inventory_sha256": self.digest(generated_inventory),
                "approval_set_sha256": self.digest(self.canonical([alternate_approval])),
                "covered_alias_ledger_sha256": self.digest(aliases),
                "manual_size_recovery_count": 0,
                "manual_size_recovery_ledger_sha256": self.digest(recoveries),
                "manual_size_recovery_approval_set_sha256": self.digest(self.canonical([])),
            },
            "calls": {
                "direct": {
                    "candidate_count": 0,
                    "expected_count": 0,
                    "transfer_role_counts": {"linked-call": 0, "direct-tail": 0},
                    "instruction_class_counts": {
                        "jal": 0,
                        "bgezal": 0,
                        "j": 0,
                        "conditional-branch": 0,
                    },
                    "resolved_count": 0,
                    "approved_exception_count": 0,
                    "unexplained_count": 0,
                    "approved_exception_category_counts": {
                        "runtime-abi": 0,
                        "fail-closed-disposition": 0,
                    },
                    "ledger_sha256": self.digest(b"direct"),
                    "approval_set_sha256": self.digest(self.canonical([])),
                },
                "indirect_ranges": {
                    "expected_count": 3,
                    "resolved_count": 3,
                    "native_return_count": 0,
                    "decision_range_count": 3,
                    "approved_exception_count": 0,
                    "unexplained_count": 0,
                    "ledger_sha256": self.digest(b"indirect"),
                    "approval_set_sha256": self.digest(self.canonical([])),
                },
            },
            "relocations": {
                "instruction": {
                    "expected_count": 0,
                    "resolved_count": 0,
                    "approved_exception_count": 0,
                    "unexplained_count": 0,
                    "hi_lo_pair_count": 0,
                    "ledger_sha256": self.digest(b"instruction"),
                    "approval_set_sha256": self.digest(self.canonical([])),
                },
                "data_r32": {
                    "expected_count": 1,
                    "resolved_count": 1,
                    "approved_exception_count": 0,
                    "unexplained_count": 0,
                    "ledger_sha256": self.digest(b"relocation-ledger"),
                    "approval_set_sha256": self.digest(self.canonical([])),
                },
            },
            "overlays": {
                "executable_section_count": 2,
                "expected_slot_count": 2,
                "populated_slot_count": 1,
                "empty_slot_count": 1,
                "listed_slot_count": 2,
                "lookup_table_slot_count": 2,
                "lifecycle_table_slot_count": 2,
                "generated_populated_module_count": 1,
                "relocation_table_entry_count": 1,
                "resolved_relocation_entry_count": 1,
                "unresolved_relocation_entry_count": 0,
                "missing_slot_count": 0,
                "duplicate_slot_count": 0,
                "slot_inventory_sha256": self.digest(slots),
                "lookup_table_sha256": self.digest(lookup),
                "lifecycle_table_sha256": self.digest(lifecycle),
                "relocation_table_sha256": self.digest(relocations),
            },
            "stubs": {
                "generated_game_function_stub_count": 0,
                "stub_ledger_sha256": self.digest(self.canonical([])),
            },
        }
        generation_result = self.canonical(
            {
                "schema_version": 1,
                "kind": "jfg-phase4-generation-semantic-result",
                **public_record,
                "semantic_bindings": {
                    "alternate_entry_ledger_sha256": self.digest(alternate),
                    "covered_alias_ledger_sha256": self.digest(aliases),
                    "manual_size_recovery_ledger_sha256": self.digest(recoveries),
                    "exception_ledger_sha256": self.digest(exceptions),
                    "n64recomp_decision_sidecar_sha256": self.digest(decisions),
                },
            }
        )
        semantic_products = {
            "generated-inventory": generated_inventory,
            "generation-result": generation_result,
            "alternate-entry-ledger": alternate,
            "covered-alias-ledger": aliases,
            "exception-ledger": exceptions,
            "manual-size-recovery-ledger": recoveries,
            "n64recomp-decision-sidecar": decisions,
            "overlay-slot-inventory": slots,
            "overlay-lookup-table": lookup,
            "overlay-lifecycle-table": lifecycle,
            "relocation-table": relocations,
        }
        reports = self.canonical(
            {
                "schema_version": 1,
                "kind": "jfg-phase4-generation-report-set",
                "reports": [
                    {"product_kind": kind, "sha256": self.digest(payload)}
                    for kind, payload in sorted(semantic_products.items())
                ],
            }
        )
        semantic_products["report-set"] = reports
        files = {
            "baseline/body0_recomp.c": b"RECOMP" b"_FUNC void " + opaque_symbols[0] + b"_recomp(uint8_t* rdram, recomp_context* ctx) {}\n",
            "baseline/body1_recomp.c": b"RECOMP" b"_FUNC void " + opaque_symbols[1] + b"_recomp(uint8_t* rdram, recomp_context* ctx) {}\n",
            "baseline/wrapper0.c": b"RECOMP" b"_FUNC void " + opaque_symbols[0] + b"(uint8_t* rdram, recomp_context* ctx) {}\n",
            "baseline/wrapper1.c": b"RECOMP" b"_FUNC void " + opaque_symbols[1] + b"(uint8_t* rdram, recomp_context* ctx) {}\n",
            "support/tables.c": b"jfg_generated_lookup_function jfg_generated_section_lifecycle jfg_generated_apply_relocations_checked\n",
            "support/thunk.c": b"RECOMP" b"_FUNC void fn_999_9999(uint8_t* rdram, recomp_context* ctx) {}\n",
            "cpu_section_inventory.json": inventory,
            "symbol_inventory.json": symbol_inventory,
        }
        semantic_paths = {
            kind: kind.replace("-", "_") + ".json"
            for kind in semantic_products
        }
        files.update(
            {semantic_paths[kind]: payload for kind, payload in semantic_products.items()}
        )
        files["sources.json"] = self.canonical(
            {
                "version": 6,
                "baseline_body_sources": ["baseline/body0_recomp.c", "baseline/body1_recomp.c"],
                "normal_wrapper_sources": ["baseline/wrapper0.c", "baseline/wrapper1.c"],
                "support_sources": ["support/tables.c", "support/thunk.c"],
                "alternate_entry_thunk_sources": ["support/thunk.c"],
                "table_support_sources": ["support/tables.c"],
                "patch_sources": [],
                "link_smoke_sources": [],
                "cpu_section_inventory": "cpu_section_inventory.json",
                "cpu_section_inventory_sha256": self.digest(inventory),
                **{
                    member: semantic_paths[product_kind]
                    for member, product_kind in {
                        "generated_inventory": "generated-inventory",
                        "generation_result": "generation-result",
                        "alternate_entry_ledger": "alternate-entry-ledger",
                        "covered_alias_ledger": "covered-alias-ledger",
                        "exception_ledger": "exception-ledger",
                        "manual_size_recovery_ledger": "manual-size-recovery-ledger",
                        "n64recomp_decision_sidecar": "n64recomp-decision-sidecar",
                        "overlay_slot_inventory": "overlay-slot-inventory",
                        "overlay_lookup_table": "overlay-lookup-table",
                        "overlay_lifecycle_table": "overlay-lifecycle-table",
                        "relocation_table": "relocation-table",
                        "report_set": "report-set",
                    }.items()
                },
                **{
                    member + "_sha256": self.digest(semantic_products[product_kind])
                    for member, product_kind in {
                        "generated_inventory": "generated-inventory",
                        "generation_result": "generation-result",
                        "alternate_entry_ledger": "alternate-entry-ledger",
                        "covered_alias_ledger": "covered-alias-ledger",
                        "exception_ledger": "exception-ledger",
                        "manual_size_recovery_ledger": "manual-size-recovery-ledger",
                        "n64recomp_decision_sidecar": "n64recomp-decision-sidecar",
                        "overlay_slot_inventory": "overlay-slot-inventory",
                        "overlay_lookup_table": "overlay-lookup-table",
                        "overlay_lifecycle_table": "overlay-lifecycle-table",
                        "relocation_table": "relocation-table",
                        "report_set": "report-set",
                    }.items()
                },
            }
        )
        products = {
            "generation-input-archive": self.zip_product(generation_inputs),
            "generation-input-inventory": self.inventory(generation_inputs),
            "generated-source-archive": self.zip_product(files),
            "source-file-inventory": self.inventory(files),
            "cpu-section-inventory": inventory,
            **semantic_products,
        }
        return products, public_record

    def analysis_sources(self) -> dict[str, bytes]:
        root = Path(harness.__file__).resolve().parents[1]
        return {
            "analysis_minimal.cpp": b'#include "minimal_runtime.cpp"\n',
            "analysis_overlay.cpp": b'#include "generated_overlay_runtime.cpp"\n',
            "analysis_relocator.cpp": b'#include "custom_overlay_relocator.cpp"\n',
            "minimal_runtime.cpp": (
                root / "src" / "runtime" / "recomp_support" / "minimal_runtime.cpp"
            ).read_bytes(),
            "recomp.h": (
                root
                / "tests"
                / "fixtures"
                / "generated-code-synthetic"
                / "include"
                / "recomp.h"
            ).read_bytes(),
            "generated_overlay_runtime.cpp": (
                root / "src" / "runtime" / "generated_overlay_runtime.cpp"
            ).read_bytes(),
            "custom_overlay_relocator.cpp": (
                root / "src" / "runtime" / "custom_overlay_relocator.cpp"
            ).read_bytes(),
            "jfg/runtime/generated_overlay_runtime.hpp": (
                root / "include" / "jfg" / "runtime" / "generated_overlay_runtime.hpp"
            ).read_bytes(),
            "jfg/runtime/custom_overlay_relocator.hpp": (
                root / "include" / "jfg" / "runtime" / "custom_overlay_relocator.hpp"
            ).read_bytes(),
        }

    def result_product(self, product_kind: str, public_record: dict[str, object]) -> bytes:
        observed = dict(public_record)
        observed.pop("result_sha256", None)
        return self.canonical(
            {
                "schema_version": 1,
                "kind": "jfg-phase4-public-result",
                "product_kind": product_kind,
                "observed": observed,
            }
        )

    def write_request(
        self,
        root: Path,
        evidence_kind: str,
        public_record: dict[str, object],
        product_payloads: dict[str, bytes],
        expected_product_kinds: set[str],
        *,
        extra_log: bool = False,
        plan_extra: dict[str, object] | None = None,
        toolchain_sha256: str | None = None,
    ) -> dict[str, object]:
        prefix = root / "production" / evidence_kind
        prefix.mkdir(parents=True)
        artifacts: list[dict[str, object]] = []
        products: list[dict[str, str]] = []
        expected_products: dict[str, str] = {}
        fixed_names = {
            "smoke-executable": "smoke-probe.exe" if os.name == "nt" else "smoke-probe",
            "forced-link-executable": "forced-link-probe.exe" if os.name == "nt" else "forced-link-probe",
            "instrumented-executable": "instrumented-probe.exe" if os.name == "nt" else "instrumented-probe",
            "baseline-archive": "baseline.a",
            "patch-archive": "patch.a",
            "generated-source-archive": "generated.zip",
            "generation-input-archive": "generation-input.zip",
            "analysis-source-archive": "analysis.zip",
        }
        for product_kind, payload in product_payloads.items():
            filename = fixed_names.get(product_kind, f"{product_kind}.json")
            path = prefix / filename
            path.write_bytes(payload)
            relative = path.relative_to(root).as_posix()
            artifacts.append(
                {"path": relative, "role": "output", "sha256": self.digest(payload)}
            )
            products.append({"product_kind": product_kind, "artifact_path": relative})
            if product_kind in expected_product_kinds:
                expected_products[product_kind] = self.digest(payload)
        plan: dict[str, object] = {
            "schema_version": 1,
            "kind": "jfg-phase4-production-audit",
            "evidence_kind": evidence_kind,
            "products": products,
        }
        if plan_extra:
            plan.update(plan_extra)
        plan_payload = self.canonical(plan)
        plan_path = prefix / "audit-plan.json"
        plan_path.write_bytes(plan_payload)
        artifacts.insert(
            0,
            {
                "path": plan_path.relative_to(root).as_posix(),
                "role": "configuration",
                "sha256": self.digest(plan_payload),
            },
        )
        if extra_log:
            log_path = prefix / "fabricated.log"
            log_path.write_text("all checks passed\n", encoding="utf-8")
            artifacts.append(
                {
                    "path": log_path.relative_to(root).as_posix(),
                    "role": "log",
                    "sha256": self.digest(log_path.read_bytes()),
                }
            )
        record_sha = self.digest(self.canonical(public_record))
        digest_fields: list[list[str]] = []

        def collect(value: object, location: str = "$") -> None:
            if isinstance(value, dict):
                for key in sorted(value):
                    child = value[key]
                    child_path = f"{location}.{key}"
                    if (
                        isinstance(child, str)
                        and key.endswith("_sha256")
                        and harness.SHA256_RE.fullmatch(child)
                    ):
                        digest_fields.append([child_path, child])
                    collect(child, child_path)
            elif isinstance(value, list):
                for index, child in enumerate(value):
                    collect(child, f"{location}[{index}]")

        collect(public_record)
        placeholder = self.digest(b"placeholder")
        execution = {
            "id": "production-execution",
            "evidence_kind": evidence_kind,
            "harness_id": "phase4-production-audit-v1",
            "harness_sha256": placeholder,
            "case_id": "production-case",
            "public_record_sha256": record_sha,
            "public_result_set_sha256": self.digest(self.canonical(digest_fields)),
            "subject_sha256": placeholder,
            "source_input_sha256": placeholder,
            "declaration_sha256": placeholder,
            "pins_sha256": placeholder,
            "environment_sha256": placeholder,
            "input_set_sha256": placeholder,
            "output_set_sha256": placeholder,
            "artifact_set_sha256": placeholder,
            "result_sha256": placeholder,
            "observed_exit_code": 0,
            "passed": True,
            "artifacts": artifacts,
        }
        return {
            "schema_version": 1,
            "kind": "jfg-phase4-harness-request",
            "public_claim_sha256": placeholder,
            "pins": {"input_rom_sha256": placeholder},
            "execution": execution,
            "public_binding": {
                "public_record": public_record,
                "expected_products": expected_products,
                "toolchain_sha256": toolchain_sha256 or placeholder,
            },
        }

    def run_harness(self, root: Path, request: object) -> subprocess.CompletedProcess[bytes]:
        environment = {
            key: os.environ[key]
            for key in (
                "PATH",
                "SYSTEMROOT",
                "WINDIR",
                "SystemDrive",
                "ProgramData",
                "ProgramFiles",
                "ProgramFiles(x86)",
                "ProgramW6432",
                "TMP",
                "TEMP",
            )
            if key in os.environ
        }
        environment["JFG_PHASE4_REPOSITORY_ROOT"] = str(
            Path(harness.__file__).resolve().parents[1]
        )
        return subprocess.run(
            [sys.executable, "-I", str(Path(harness.__file__).resolve())],
            cwd=root,
            env=environment,
            input=self.canonical(request),
            capture_output=True,
            check=False,
            timeout=35,
        )

    def repro_products(self) -> tuple[dict[str, bytes], dict[str, object]]:
        generation_products, _ = self.generation_fixture()
        source_inventory = generation_products["source-file-inventory"]
        member_inventory = self.member_inventory(
            [{"name": "unit.o", "sha256": self.digest(b"object"), "size": 6}]
        )
        products = {
            "generation-input-archive": generation_products["generation-input-archive"],
            "generation-input-inventory": generation_products["generation-input-inventory"],
            "generated-source-archive": generation_products["generated-source-archive"],
            "normalized-run-payload": self.canonical(
                {
                    "normalization_policy_id": "phase4-run-payload-v1",
                    "records": [],
                }
            ),
            "generated-inventory": self.canonical({"kind": "generated", "records": []}),
            "cpu-section-inventory": generation_products["cpu-section-inventory"],
            "overlay-lookup-table": self.canonical({"kind": "lookup", "records": []}),
            "overlay-lifecycle-table": self.canonical(
                {"kind": "lifecycle", "records": []}
            ),
            "relocation-table": self.canonical({"kind": "relocations", "records": []}),
            "report-set": self.canonical({"kind": "reports", "records": []}),
            "source-file-inventory": source_inventory,
            "baseline-member-inventory": member_inventory,
            "patch-member-inventory": member_inventory,
        }
        for product_kind in (
            "generated-inventory",
            "generation-result",
            "alternate-entry-ledger",
            "covered-alias-ledger",
            "exception-ledger",
            "manual-size-recovery-ledger",
            "n64recomp-decision-sidecar",
            "overlay-slot-inventory",
            "overlay-lookup-table",
            "overlay-lifecycle-table",
            "relocation-table",
            "report-set",
        ):
            products[product_kind] = generation_products[product_kind]
        public_record = {
            "normalization_policy_id": "phase4-run-payload-v1",
            "normalized_run_payload_sha256": self.digest(products["normalized-run-payload"]),
            "generated_inventory_sha256": self.digest(products["generated-inventory"]),
            "cpu_section_inventory_sha256": self.digest(products["cpu-section-inventory"]),
            "overlay_table_sha256": self.digest(products["overlay-lookup-table"]),
            "lifecycle_table_sha256": self.digest(products["overlay-lifecycle-table"]),
            "relocation_table_sha256": self.digest(products["relocation-table"]),
            "report_set_sha256": self.digest(products["report-set"]),
            "source_file_inventory_sha256": self.digest(products["source-file-inventory"]),
            "baseline_member_inventory_sha256": self.digest(
                products["baseline-member-inventory"]
            ),
            "patch_member_inventory_sha256": self.digest(
                products["patch-member-inventory"]
            ),
        }
        return products, public_record

    def config_products(
        self, *, observe_addition: bool = True
    ) -> tuple[dict[str, bytes], dict[str, object]]:
        digest = self.digest
        base_symbols = {
            "symbol-a": {
                "name_sha256": digest(b"name-a"),
                "section_sha256": digest(b"section-a"),
            }
        }
        mutated_symbols = dict(base_symbols)
        if observe_addition:
            mutated_symbols["symbol-b"] = {
                "name_sha256": digest(b"name-b"),
                "section_sha256": digest(b"section-b"),
            }
        base_payload = self.canonical(
            {
                "schema_version": 1,
                "kind": "jfg-phase4-config-observation",
                "symbols": base_symbols,
                "lookup": {},
                "patches": {},
                "compiler_options": {},
            }
        )
        mutated_payload = self.canonical(
            {
                "schema_version": 1,
                "kind": "jfg-phase4-config-observation",
                "symbols": mutated_symbols,
                "lookup": {},
                "patches": {},
                "compiler_options": {},
            }
        )
        categories = {category: 0 for category in harness.CONFIG_CATEGORIES}
        categories["symbol-addition"] = 1
        expectation = self.canonical(categories)
        base_config = b"base configuration\n"
        mutated_config_set = self.canonical({"mutation_ids": ["mutation-a"]})
        public_record: dict[str, object] = {
            "base_config_sha256": digest(base_config),
            "mutated_config_set_sha256": digest(mutated_config_set),
            "predeclared_expectation_sha256": digest(expectation),
            "expected_category_counts": categories,
            "observed_category_counts": categories,
            "result_sha256": "0" * 64,
        }
        result_product = self.result_product("config-diff-result", public_record)
        public_record["result_sha256"] = digest(result_product)
        products = {
            "base-config": base_config,
            "mutated-config-set": mutated_config_set,
            "predeclared-expectation": expectation,
            "config-diff-result": result_product,
            "base-run-payload": base_payload,
            "mutated-run-payload": mutated_payload,
        }
        return products, public_record

    def test_repro_product_set_is_independently_parsed_before_generation_replay(self) -> None:
        products, public_record = self.repro_products()
        request = {"public_binding": {"toolchain_sha256": "0" * 64}}
        with mock.patch.object(harness, "_replay_generation") as replay:
            harness._verify_reproducibility(products, public_record, request)
        replay.assert_called_once()

    def test_generation_archive_is_recounted_against_public_denominators(self) -> None:
        products, public_record = self.generation_fixture()
        request = {"public_binding": {"toolchain_sha256": "0" * 64}}
        with mock.patch.object(harness, "_replay_generation") as replay:
            harness._verify_generation(products, public_record, request)
        replay.assert_called_once()

        public_record["symbols"]["generated_count"] = 3  # type: ignore[index]
        with mock.patch.object(harness, "_replay_generation"):
            with self.assertRaisesRegex(harness.AuditError, "public denominators"):
                harness._verify_generation(products, public_record, request)

    def test_generation_rejects_tampered_cpu_inventory_rows_and_archive_binding(self) -> None:
        valid, public_record = self.generation_fixture()
        valid_inventory = valid["cpu-section-inventory"]
        document = json.loads(valid_inventory)
        rows = document["sections"]
        assert isinstance(rows, list)
        cases: list[tuple[str, bytes, bytes]] = []

        altered = json.loads(valid_inventory)
        altered["sections"][1]["relocations"] = 0
        altered_bytes = self.canonical(altered)
        cases.append(("altered-product", altered_bytes, valid_inventory))

        dropped = json.loads(valid_inventory)
        dropped["sections"] = dropped["sections"][:1]
        dropped_bytes = self.cpu_inventory(dropped["sections"])
        cases.append(("dropped-row", dropped_bytes, dropped_bytes))

        reordered = json.loads(valid_inventory)
        reordered["sections"] = list(reversed(reordered["sections"]))
        reordered_bytes = self.cpu_inventory(reordered["sections"])
        cases.append(("reordered-row", reordered_bytes, reordered_bytes))

        for label, product_inventory, archive_inventory in cases:
            with self.subTest(label=label):
                products, record = self.generation_fixture(archive_inventory)
                products["cpu-section-inventory"] = product_inventory
                with mock.patch.object(harness, "_replay_generation"):
                    with self.assertRaises(harness.AuditError):
                        harness._verify_generation(
                            products,
                            record,
                            {"public_binding": {"toolchain_sha256": "0" * 64}},
                        )

    def test_generation_input_archive_is_closed_over_exact_private_inputs(self) -> None:
        products, _ = self.generation_fixture()
        contents = harness._generation_input_contents(products)
        self.assertEqual(set(contents), harness.GENERATION_INPUT_NAMES)

        changed = dict(contents)
        changed["extra.bin"] = b"plausible but unbound"
        products["generation-input-archive"] = self.zip_product(changed)
        products["generation-input-inventory"] = self.inventory(changed)
        with self.assertRaisesRegex(harness.AuditError, "fixed inventory"):
            harness._generation_input_contents(products)

    def test_generation_claim_cannot_pass_when_private_replay_differs(self) -> None:
        products, public_record = self.generation_fixture()
        with mock.patch.object(
            harness,
            "_replay_generation",
            side_effect=harness.AuditError(
                "generation replay output differs from the staged source archive"
            ),
        ):
            with self.assertRaisesRegex(harness.AuditError, "replay output differs"):
                harness._verify_generation(
                    products,
                    public_record,
                    {"public_binding": {"toolchain_sha256": "0" * 64}},
                )

    def test_generation_replay_rejects_when_context_replay_fails(self) -> None:
        products, _ = self.generation_fixture()
        _, contents = harness._zip_files(products["generated-source-archive"])
        root = Path(harness.__file__).resolve().parents[1]
        # Isolate the context-process failure from historical source drift.
        # Separate tests above/below exercise the unmodified production pins.
        fixture_pins = {path: self.digest((root / path).read_bytes())
                        for path in harness.GENERATION_REPLAY_SOURCE_SHA256}
        with mock.patch.object(harness, "GENERATION_REPLAY_SOURCE_SHA256", fixture_pins), mock.patch.dict(
            os.environ, {"JFG_PHASE4_REPOSITORY_ROOT": str(root)}
        ), mock.patch.object(
            harness,
            "_fixed_generation_recompiler",
            return_value=(root / "tools" / "results" / "phase4" / "n64recomp-bounded" / "build" / "N64Recomp", b"\x7fELF"),
        ), mock.patch.object(
            harness, "_verify_fixed_wsl_file"
        ), mock.patch.object(
            harness, "_run_fixed_wsl", return_value=(1, b"", b"")
        ):
            with self.assertRaisesRegex(harness.AuditError, "context replay failed"):
                harness._replay_generation(
                    products,
                    contents,
                    {
                        "pins": {
                            "input_elf_sha256": self.digest(
                                harness._generation_input_contents(products)["input.elf"]
                            )
                        },
                        "public_binding": {"toolchain_sha256": "0" * 64},
                    },
                )

    def test_generation_replay_binds_private_input_elf_to_public_pin(self) -> None:
        products, _ = self.generation_fixture()
        _, contents = harness._zip_files(products["generated-source-archive"])
        with self.assertRaisesRegex(harness.AuditError, "ELF differs from the public pin"):
            harness._replay_generation(
                products,
                contents,
                {
                    "pins": {"input_elf_sha256": "0" * 64},
                    "public_binding": {"toolchain_sha256": "0" * 64},
                },
            )

    def test_artifact_over_fixed_128_mib_bound_is_rejected(self) -> None:
        self.assertEqual(harness.MAX_ARTIFACT_BYTES, 128 * 1024 * 1024)
        with tempfile.TemporaryDirectory(prefix="phase4-oversized-") as temp:
            root = Path(temp)
            artifact = root / "oversized.bin"
            with artifact.open("wb") as stream:
                stream.seek(harness.MAX_ARTIFACT_BYTES)
                stream.write(b"x")
            previous = Path.cwd()
            try:
                os.chdir(root)
                with self.assertRaisesRegex(harness.AuditError, "artifact is oversized"):
                    harness._read_regular("oversized.bin")
            finally:
                os.chdir(previous)

    def test_generation_input_staging_uses_the_declared_private_file_bound(self) -> None:
        with tempfile.TemporaryDirectory(prefix="phase4-generation-staging-") as temp:
            payload = b"\0" * (4 * 1024 * 1024 + 1)
            harness._stage_generation_inputs(Path(temp), {"input.elf": payload})
            self.assertEqual((Path(temp) / "input.elf").stat().st_size, len(payload))

    def test_predeclared_config_diff_is_derived_from_both_payloads(self) -> None:
        expected = {
            "base-config",
            "mutated-config-set",
            "predeclared-expectation",
            "config-diff-result",
        }
        with tempfile.TemporaryDirectory(prefix="phase4-production-") as temp:
            root = Path(temp)
            products, public_record = self.config_products(observe_addition=True)
            request = self.write_request(
                root,
                "configuration-mutation",
                public_record,
                products,
                expected,
            )
            result = self.run_harness(root, request)
            self.assertEqual(result.returncode, 0, result.stderr)

        with tempfile.TemporaryDirectory(prefix="phase4-production-") as temp:
            root = Path(temp)
            products, public_record = self.config_products(observe_addition=False)
            request = self.write_request(
                root,
                "configuration-mutation",
                public_record,
                products,
                expected,
            )
            result = self.run_harness(root, request)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, b"")

    def test_missing_product_and_unplanned_fabricated_log_fail(self) -> None:
        with tempfile.TemporaryDirectory(prefix="phase4-production-") as temp:
            root = Path(temp)
            products, public_record = self.repro_products()
            products.pop("patch-member-inventory")
            request = self.write_request(
                root,
                "reproducibility-run-a",
                public_record,
                products,
                set(products),
            )
            result = self.run_harness(root, request)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, b"")
            self.assertEqual(result.stderr, b"")

        with tempfile.TemporaryDirectory(prefix="phase4-production-") as temp:
            root = Path(temp)
            products, public_record = self.repro_products()
            request = self.write_request(
                root,
                "reproducibility-run-a",
                public_record,
                products,
                set(products),
                extra_log=True,
            )
            result = self.run_harness(root, request)
            self.assertNotEqual(result.returncode, 0)

    def test_fabricated_compiler_result_fails_even_with_matching_claimed_digest(self) -> None:
        with tempfile.TemporaryDirectory(prefix="phase4-production-") as temp:
            root = Path(temp)
            generated_sources = {
                "sources.json": self.canonical(
                    {
                        "baseline_body_sources": ["unit.c"],
                        "normal_wrapper_sources": [],
                        "support_sources": [],
                        "patch_sources": [],
                        "link_smoke_sources": [],
                    }
                ),
                "unit.c": b"void unit(void) {}\n",
            }
            source_inventory = self.inventory(generated_sources)
            fabricated_result = self.canonical({"passed": True})
            public_record = {
                "compiler_id": "clang-22",
                "family": "clang",
                "target_id": "windows-x64",
                "configuration_id": "debug",
                "result_sha256": self.digest(fabricated_result),
            }
            products = {
                "compiler-result": fabricated_result,
                "generated-source-archive": self.zip_product(generated_sources),
                "source-file-inventory": source_inventory,
                "smoke-executable": b"MZjfg_generated_link_smoke\x00jfg_generated_lookup_function\x00jfg_generated_section_count",
            }
            request = self.write_request(
                root,
                "compiler-clang",
                public_record,
                products,
                set(products) - {"smoke-executable"},
            )
            result = self.run_harness(root, request)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, b"")

    def test_marker_only_sanitizer_executable_cannot_replace_bound_source(self) -> None:
        with tempfile.TemporaryDirectory(prefix="phase4-production-") as temp:
            root = Path(temp)
            files = self.analysis_sources()
            source_inventory = self.inventory(files)
            sanitizer_record: dict[str, object] = {
                "sanitizer_id": "address",
                "compiler_family": "gcc",
                "target_id": "linux-x64",
                "run_count": 1,
                "issue_count": 0,
                "passed": True,
                "source_inventory_sha256": self.digest(source_inventory),
                "result_sha256": "0" * 64,
            }
            result_product = self.result_product("sanitizer-result", sanitizer_record)
            sanitizer_record["result_sha256"] = self.digest(result_product)
            result_product = self.result_product("sanitizer-result", sanitizer_record)
            public_record = {
                "result": sanitizer_record,
                "source_policy_id": harness.ANALYSIS_SOURCE_POLICY_ID,
                "handwritten_bridge_unit_count": harness.ANALYSIS_BRIDGE_UNIT_COUNT,
            }
            products = {
                "analysis-source-inventory": source_inventory,
                "sanitizer-result": result_product,
                "instrumented-executable": b"MZjfg_phase4_sanitizer_probe without instrumentation",
            }
            request = self.write_request(
                root,
                "address-sanitizer",
                public_record,
                products,
                {"analysis-source-inventory", "sanitizer-result"},
            )
            result = self.run_harness(root, request)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, b"")

    def test_sanitizer_rejects_source_archive_not_equal_to_tracked_policy(self) -> None:
        with tempfile.TemporaryDirectory(prefix="phase4-production-") as temp:
            root = Path(temp)
            files = self.analysis_sources()
            files["minimal_runtime.cpp"] += b"// fabricated bridge\n"
            source_inventory = self.inventory(files)
            sanitizer_record: dict[str, object] = {
                "sanitizer_id": "address",
                "compiler_family": "gcc",
                "target_id": "linux-x64",
                "run_count": 1,
                "issue_count": 0,
                "passed": True,
                "source_inventory_sha256": self.digest(source_inventory),
                "result_sha256": "0" * 64,
            }
            result_product = self.result_product("sanitizer-result", sanitizer_record)
            sanitizer_record["result_sha256"] = self.digest(result_product)
            result_product = self.result_product("sanitizer-result", sanitizer_record)
            public_record = {
                "result": sanitizer_record,
                "source_policy_id": harness.ANALYSIS_SOURCE_POLICY_ID,
                "handwritten_bridge_unit_count": harness.ANALYSIS_BRIDGE_UNIT_COUNT,
            }
            products = {
                "analysis-source-archive": self.zip_product(files),
                "analysis-source-inventory": source_inventory,
                "sanitizer-result": result_product,
            }
            request = self.write_request(
                root,
                "address-sanitizer",
                public_record,
                products,
                {"analysis-source-inventory", "sanitizer-result"},
            )
            result = self.run_harness(root, request)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, b"")

    def test_current_analysis_sources_cannot_reuse_stale_historical_pins(self) -> None:
        files = self.analysis_sources()
        self.assertEqual(set(files), set(harness.ANALYSIS_SOURCE_SHA256))
        products = {"analysis-source-archive": self.zip_product(files),
                    "analysis-source-inventory": self.inventory(files)}
        record = {"result": {}, "source_policy_id": harness.ANALYSIS_SOURCE_POLICY_ID,
                  "handwritten_bridge_unit_count": harness.ANALYSIS_BRIDGE_UNIT_COUNT}
        actual = {path: self.digest(payload) for path, payload in files.items()}
        if actual == harness.ANALYSIS_SOURCE_SHA256:
            self.assertEqual(harness._analysis_sources(products, record), files)
        else:
            with self.assertRaisesRegex(harness.AuditError, "differs from the tracked source policy"):
                harness._analysis_sources(products, record)

    def test_current_replay_sources_cannot_reuse_stale_historical_pins(self) -> None:
        root = Path(harness.__file__).resolve().parents[1]
        for pins, verify in (
                (harness.BUILD_REPLAY_SOURCE_SHA256, harness._verify_build_replay_source_closure),
                (harness.GENERATION_REPLAY_SOURCE_SHA256, harness._verify_generation_replay_source_closure)):
            with self.subTest(verifier=verify.__name__):
                actual = {path: self.digest((root / path).read_bytes()) for path in pins}
                if actual == pins:
                    verify(root)
                else:
                    with self.assertRaisesRegex(harness.AuditError, "differs from its pin"):
                        verify(root)

    def test_compiler_replay_policy_has_the_exact_three_target_closure(self) -> None:
        # jfg_generated_{link_smoke,baseline_audit,patch_audit} never links
        # jfg_runtime: all generated inputs are private archive members, and
        # these are the only repository-owned files CMake reads or compiles.
        expected = {
            "CMakeLists.txt",
            "cmake/GeneratedCode.cmake",
            "cmake/Warnings.cmake",
            "scripts/audit_generated_objects.py",
            "scripts/build_private_generated_root.py",
            "scripts/prepare_generated_sources.py",
            "src/runtime/recomp_support/minimal_runtime.cpp",
            "src/runtime/recomp_support/patch_archive_anchor.c",
            "tests/CMakeLists.txt",
            "tests/generated_baseline_audit.cpp",
            "tests/generated_link_smoke.cpp",
            "tests/generated_patch_audit.cpp",
        }
        self.assertEqual(set(harness.BUILD_REPLAY_SOURCE_SHA256), expected)
        self.assertEqual(harness.BUILD_REPLAY_TRACKED_CLOSURE, frozenset(expected))
        self.assertFalse(
            any(path.startswith(("src/runtime/", "include/jfg/"))
                and "recomp_support" not in path
                for path in expected)
        )

    def test_compiler_replay_rejects_tampered_closed_tracked_input(self) -> None:
        with tempfile.TemporaryDirectory(prefix="phase4-source-closure-") as temp:
            root = Path(temp)
            tracked = root / "tracked-input.txt"
            tracked.write_bytes(b"reviewed")
            with mock.patch.object(
                harness,
                "BUILD_REPLAY_SOURCE_SHA256",
                {"tracked-input.txt": self.digest(b"reviewed")},
            ):
                harness._verify_build_replay_source_closure(root)
                tracked.write_bytes(b"tampered")
                with self.assertRaisesRegex(harness.AuditError, "differs from its pin"):
                    harness._verify_build_replay_source_closure(root)

    def test_generation_replay_rejects_tampered_closed_tracked_input(self) -> None:
        with tempfile.TemporaryDirectory(prefix="phase4-generation-source-") as temp:
            root = Path(temp)
            tracked = root / "tracked-input.py"
            tracked.write_bytes(b"reviewed")
            with mock.patch.object(
                harness,
                "GENERATION_REPLAY_SOURCE_SHA256",
                {"tracked-input.py": self.digest(b"reviewed")},
            ):
                harness._verify_generation_replay_source_closure(root)
                tracked.write_bytes(b"tampered")
                with self.assertRaisesRegex(harness.AuditError, "differs from its pin"):
                    harness._verify_generation_replay_source_closure(root)

    def test_pinned_local_build_tool_rejects_a_hash_mismatch(self) -> None:
        with tempfile.TemporaryDirectory(prefix="phase4-tool-identity-") as temp:
            tool = Path(temp) / "tool.exe"
            tool.write_bytes(b"trusted tool")
            with self.assertRaisesRegex(harness.AuditError, "differs from its repository pin"):
                harness._verify_pinned_local_tool(tool, "0" * 64)

    def test_wsl_build_tool_policy_is_closed_and_rejects_unknown_paths(self) -> None:
        self.assertTrue(
            {
                "/usr/bin/cmake",
                "/usr/bin/ninja",
                "/usr/bin/gcc-13",
                "/usr/bin/g++-13",
                "/usr/bin/ar",
                "/usr/bin/ranlib",
                "/usr/bin/ld",
                "/usr/bin/nm",
                "/usr/bin/python3",
                "/usr/bin/readelf",
                "/usr/libexec/gcc/x86_64-linux-gnu/13/cc1",
                "/usr/libexec/gcc/x86_64-linux-gnu/13/cc1plus",
                "/usr/libexec/gcc/x86_64-linux-gnu/13/collect2",
            }.issubset(harness.WSL_BUILD_TOOL_SHA256)
        )
        self.assertTrue(
            {
                "cmake.exe",
                "ninja.exe",
                "vcvars64.bat",
                "cmd.exe",
                "link.exe",
                "lib.exe",
                "dumpbin.exe",
                "python.exe",
                "wsl.exe",
            }.issubset(harness.WINDOWS_BUILD_TOOL_SHA256)
        )
        observed: list[tuple[str, str]] = []
        with mock.patch.object(
            harness,
            "_verify_fixed_wsl_file",
            side_effect=lambda path, digest: observed.append((path, digest)),
        ):
            harness._verify_wsl_replay_toolchain()
        self.assertEqual(observed, list(harness.WSL_BUILD_TOOL_SHA256.items()))
        with self.assertRaisesRegex(harness.AuditError, "policy is invalid"):
            harness._verify_fixed_wsl_file("/usr/bin/unbound-tool", "0" * 64)

    def test_cmake_replay_arguments_force_all_material_build_tools(self) -> None:
        arguments = harness._cmake_replay_arguments(
            "repo", "generated", "build", "cc", "cxx", "Release", "ninja",
            "ar", "ranlib", "ld", "nm", "python",
        )
        self.assertTrue(
            {
                "-DCMAKE_MAKE_PROGRAM=ninja",
                "-DCMAKE_AR=ar",
                "-DCMAKE_RANLIB=ranlib",
                "-DCMAKE_LINKER=ld",
                "-DCMAKE_NM=nm",
                "-DPython3_EXECUTABLE=python",
            }.issubset(arguments)
        )

    def test_forced_link_rejects_standalone_probe_when_source_replay_fails(self) -> None:
        products, _ = self.generation_fixture()
        baseline, baseline_members = self.archive("baseline.o")
        patch, patch_members = self.archive("patch.o")
        products.update(
            {
                "baseline-archive": baseline,
                "baseline-member-inventory": baseline_members,
                "patch-archive": patch,
                "patch-member-inventory": patch_members,
                # The marker-bearing bytes represent a nonce-capable standalone
                # probe.  It must not be accepted when source replay fails.
                "forced-link-executable": b"MZjfg_generated_link_smoke "
                b"jfg_generated_lookup_function jfg_generated_apply_relocations "
                b"jfg_generated_section_lifecycle",
            }
        )
        clang_record: dict[str, object] = {
            "family": "clang",
            "target_id": "windows-x64",
            "configuration_id": "release",
            "compiler_id": "clang-22.1.8",
            "source_inventory_sha256": self.digest(products["source-file-inventory"]),
            "compiler_executable_sha256": "0" * 64,
        }
        clang_record["option_set_sha256"] = self.digest(
            self.canonical(harness._compiler_option_set(clang_record))
        )
        public_record = {
            "compilers": [clang_record],
            "overlays": {},
            "libraries": {
                "baseline": {},
                "patch": {},
                "minimal_runtime": {},
            },
        }
        request = {"execution": {"evidence_kind": "forced-object-link-audit"}}
        with mock.patch.object(
            harness,
            "_replay_compiler_build_record",
            side_effect=harness.AuditError("fixed compiler configure replay failed"),
        ), mock.patch.object(
            harness, "_verify_forced_object_semantics"
        ), mock.patch.object(harness, "_run_product_probe") as probe:
            with self.assertRaisesRegex(harness.AuditError, "configure replay failed"):
                harness._verify_forced_link(products, {}, {"public_binding": {"public_record": public_record}, **request})
        probe.assert_not_called()

    @unittest.skipUnless(os.name == "nt", "local compiler replay is Windows-only")
    def test_fixed_clang_replay_compiles_and_whole_object_links_source_archive(self) -> None:
        root = Path(harness.__file__).resolve().parents[1]
        fixture = root / "tests" / "fixtures" / "generated-code-synthetic"
        clang = root / "tools" / "build" / "llvm-22.1.8" / "bin" / "clang-cl.exe"
        try:
            harness._fixed_windows_cmake()
            harness._fixed_windows_ninja()
        except harness.AuditError:
            self.skipTest("fixed Visual Studio build tools are unavailable")
        if not clang.is_file():
            self.skipTest("fixed local clang-cl is unavailable")
        contents = {
            path.relative_to(fixture).as_posix(): path.read_bytes()
            for path in fixture.rglob("*")
            if path.is_file()
        }
        manifest = json.loads(contents["sources.json"])
        manifest["version"] = 2
        manifest["normalizer_revision_sha256"] = self.digest(
            (root / "scripts" / "build_private_generated_root.py").read_bytes()
        )
        contents["sources.json"] = json.dumps(manifest, indent=2).encode("utf-8") + b"\n"
        environment = harness._windows_build_environment()
        public_record: dict[str, object] = {
            "family": "clang",
            "target_id": "windows-x64",
            "configuration_id": "release",
            "compiler_id": harness._local_compiler_id(
                "clang", clang.resolve(), environment
            ),
        }
        public_record["option_set_sha256"] = self.digest(
            self.canonical(harness._compiler_option_set(public_record))
        )
        with mock.patch.dict(
            os.environ, {"JFG_PHASE4_REPOSITORY_ROOT": str(root)}
        ):
            harness._windows_replay_compiler_build(
                contents,
                public_record,
                self.digest(clang.read_bytes()),
            )

    @unittest.skipUnless(os.name == "nt", "local fixed Clang replay is Windows-only")
    def test_harness_runs_fixed_local_clang_analyzer_on_every_bridge(self) -> None:
        clang = (
            Path(harness.__file__).resolve().parents[1]
            / "tools"
            / "build"
            / "llvm-22.1.8"
            / "bin"
            / "clang++.exe"
        )
        if not clang.is_file():
            self.skipTest("the fixed local Clang toolchain is unavailable")
        toolchain_sha256 = self.digest(clang.read_bytes())
        self.assertEqual(harness._fixed_clang(toolchain_sha256), clang.resolve())

        files = self.analysis_sources()
        source_inventory = self.inventory(files)
        analyzer_record: dict[str, object] = {
            "tool_id": "clang-analyzer",
            "undefined_behavior_issue_count": 0,
            "passed": True,
            "result_sha256": "0" * 64,
        }
        result_product = self.result_product("analyzer-result", analyzer_record)
        analyzer_record["result_sha256"] = self.digest(result_product)
        result_product = self.result_product("analyzer-result", analyzer_record)
        public_record = {
            "result": analyzer_record,
            "source_policy_id": harness.ANALYSIS_SOURCE_POLICY_ID,
            "handwritten_bridge_unit_count": harness.ANALYSIS_BRIDGE_UNIT_COUNT,
        }
        products = {
            "analysis-source-archive": self.zip_product(files),
            "analysis-source-inventory": source_inventory,
            "analyzer-result": result_product,
        }
        with tempfile.TemporaryDirectory(prefix="phase4-production-") as temp:
            root = Path(temp)
            request = self.write_request(
                root,
                "clang-static-analysis",
                public_record,
                products,
                {"analysis-source-inventory", "analyzer-result"},
                toolchain_sha256=toolchain_sha256,
            )
            result = self.run_harness(root, request)
        self.assertEqual(result.returncode, 0, result.stderr.decode("utf-8"))
        self.assertTrue(json.loads(result.stdout)["validated"])

    def test_fixed_analyzer_replay_preserves_host_cpp_include_roots(self) -> None:
        with tempfile.TemporaryDirectory(prefix="phase4-analyzer-env-") as temp, mock.patch.dict(
            os.environ, {"INCLUDE": "fixed-cpp-include-root"}
        ):
            return_code, stdout, stderr = harness._run_bounded(
                [
                    sys.executable,
                    "-c",
                    "import os; print(os.environ.get('INCLUDE', ''), end='')",
                ],
                cwd=Path(temp),
            )
        self.assertEqual((return_code, stdout, stderr), (0, b"fixed-cpp-include-root", b""))

    @unittest.skipUnless(os.name == "nt", "WSL trust-boundary test is Windows-only")
    def test_path_shadow_and_swap_cannot_replace_wsl_trust_anchor(self) -> None:
        with tempfile.TemporaryDirectory(prefix="phase4-shadow-wsl-") as temp:
            fake = Path(temp) / "wsl.exe"
            fake.write_bytes(b"fake WSL")
            with mock.patch.dict(os.environ, {"PATH": temp}):
                selected = harness._fixed_wsl()
            self.assertNotEqual(selected.resolve(), fake.resolve())

            def mutate(command: list[str], **_kwargs: object) -> tuple[int, bytes, bytes]:
                Path(command[0]).write_bytes(b"swapped WSL")
                return 0, b"", b""

            with mock.patch.object(harness, "_run_bounded", side_effect=mutate):
                with self.assertRaisesRegex(harness.AuditError, "changed during execution"):
                    harness._run_fixed_wsl(["--status"], cwd=Path(temp))

    @unittest.skipUnless(os.name == "nt", "fixed sanitizer replay uses Ubuntu-24.04 WSL")
    def test_harness_compiles_and_executes_bound_bridge_with_gcc_sanitizers(self) -> None:
        wsl = Path("C:/Windows/System32/wsl.exe")
        identity = subprocess.run(
            [
                str(wsl),
                "-d",
                "Ubuntu-24.04",
                "--exec",
                "/usr/bin/sha256sum",
                "/usr/bin/g++-13",
            ],
            capture_output=True,
            check=False,
            timeout=10,
        )
        if identity.returncode != 0:
            self.skipTest("pinned WSL GCC is unavailable")
        toolchain_sha256 = identity.stdout.decode("ascii").split()[0]
        files = self.analysis_sources()
        # This replay certifies the frozen historical bridge policy, not any
        # later fork edits. The stale-policy rejection test remains mandatory.
        actual = {name: self.digest(payload) for name, payload in files.items()}
        if actual != harness.ANALYSIS_SOURCE_SHA256:
            self.skipTest("current bridge revisions are outside the frozen sanitizer source policy")
        source_inventory = self.inventory(files)
        for evidence_kind, sanitizer_id in (
            ("address-sanitizer", "address"),
            ("undefined-behavior-sanitizer", "undefined-behavior"),
        ):
            with self.subTest(evidence_kind=evidence_kind), tempfile.TemporaryDirectory(
                prefix="phase4-production-"
            ) as temp:
                sanitizer_record: dict[str, object] = {
                    "sanitizer_id": sanitizer_id,
                    "compiler_family": "gcc",
                    "target_id": "linux-x64",
                    "run_count": 1,
                    "issue_count": 0,
                    "passed": True,
                    "source_inventory_sha256": self.digest(source_inventory),
                    "result_sha256": "0" * 64,
                }
                result_product = self.result_product("sanitizer-result", sanitizer_record)
                sanitizer_record["result_sha256"] = self.digest(result_product)
                result_product = self.result_product("sanitizer-result", sanitizer_record)
                public_record = {
                    "result": sanitizer_record,
                    "source_policy_id": harness.ANALYSIS_SOURCE_POLICY_ID,
                    "handwritten_bridge_unit_count": harness.ANALYSIS_BRIDGE_UNIT_COUNT,
                }
                products = {
                    "analysis-source-archive": self.zip_product(files),
                    "analysis-source-inventory": source_inventory,
                    "sanitizer-result": result_product,
                }
                root = Path(temp)
                request = self.write_request(
                    root,
                    evidence_kind,
                    public_record,
                    products,
                    {"analysis-source-inventory", "sanitizer-result"},
                    toolchain_sha256=toolchain_sha256,
                )
                result = self.run_harness(root, request)
                self.assertEqual(result.returncode, 0, result.stderr.decode("utf-8"))
                self.assertTrue(json.loads(result.stdout)["validated"])

    def test_request_cannot_add_a_command_or_argument_vector(self) -> None:
        with tempfile.TemporaryDirectory(prefix="phase4-production-") as temp:
            root = Path(temp)
            products, public_record = self.repro_products()
            request = self.write_request(
                root,
                "reproducibility-run-a",
                public_record,
                products,
                set(products),
                plan_extra={"command": "ignored", "arguments": ["private"]},
            )
            result = self.run_harness(root, request)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, b"")


if __name__ == "__main__":
    unittest.main()
