#!/usr/bin/env python3
"""Normalize local N64Recomp output into the Phase 4 private link model.

All inputs and outputs handled by this helper are ROM-derived and must remain
under the ignored tools tree.  Standard output reports aggregate counts only.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import subprocess
import sys
import tomllib
from pathlib import Path


FUNCTION_RE = re.compile(
    r"^RECOMP_FUNC void (fn_[0-9]{3}_[0-9]{4})\(uint8_t\* rdram, recomp_context\* ctx\) \{",
    re.MULTILINE,
)
RECOMP_CONTEXT_ANONYMOUS_RE = re.compile(
    r"(?P<prefix>\btypedef[ \t\n]+struct)(?P<gap>[ \t\n]*)"
    r"(?P<open>\{)(?P<body>.*?)\}(?P<suffix>[ \t\n]+recomp_context[ \t\n]*;)",
    re.DOTALL,
)
RECOMP_CONTEXT_TAGGED_RE = re.compile(
    r"\btypedef[ \t\n]+struct[ \t\n]+recomp_context[ \t\n]*\{"
    r".*?\}[ \t\n]+recomp_context[ \t\n]*;",
    re.DOTALL,
)
ROOT = Path(__file__).resolve().parents[1]
NORMALIZER_PATH = Path(__file__).resolve()
# Required-body inventory revision includes the original task conversion
# helper and the original PI-handle initialization helper. These exact
# denominators intentionally reject older inputs. Existing
# Phase 4 acceptance manifests remain tied to their older generation pins.
EXPECTED_AUTHORITATIVE_BODY_COUNT = 2909
EXPECTED_SUPPORT_THUNK_COUNT = 30
EXPECTED_OVERLAY_SLOT_COUNT = 157
EXPECTED_EMPTY_OVERLAY_SLOT_COUNT = 2
EXPECTED_SECTION_COUNT = 156
EXPECTED_R32_RELOCATION_COUNT = 2028
EXPECTED_R26_RELOCATION_COUNT = 8980
EXPECTED_DIRECT_CALL_CANDIDATE_COUNT = 14053
EXPECTED_DIRECT_LINKED_CALL_CANDIDATE_COUNT = 13964
EXPECTED_DIRECT_TAIL_CANDIDATE_COUNT = 89
EXPECTED_DIRECT_GENERATED_TARGET_CANDIDATE_COUNT = 13867
EXPECTED_DIRECT_UNRESOLVED_TARGET_CANDIDATE_COUNT = 186
EXPECTED_DIRECT_TRANSFER_ROLE_COUNTS = {"linked-call": 13964, "direct-tail": 89}
EXPECTED_DIRECT_INSTRUCTION_CLASS_COUNTS = {
    "jal": 13963,
    "bgezal": 1,
    "j": 76,
    "conditional-branch": 13,
}
EXPECTED_EXECUTABLE_SYMBOL_COUNT = 3733
EXPECTED_COVERED_ALIAS_COUNT = 824
EXPECTED_COVERED_ALIAS_BODY_START_COUNT = 805
EXPECTED_COVERED_ALIAS_INTERIOR_COUNT = 19
EXPECTED_MANUAL_SIZE_RECOVERY_COUNT = 10
EXPECTED_INSTRUCTION_RELOCATION_COUNT = 26007
EXPECTED_HI_LO_PAIR_COUNT = 8478
EXPECTED_INDIRECT_TRANSFER_SITE_COUNT = 3624
EXPECTED_NATIVE_RETURN_SITE_COUNT = 3419
EXPECTED_INDIRECT_DECISION_SITE_COUNT = 205
MAX_TEXT_INPUT_BYTES = 512 * 1024 * 1024
MAX_RAW_SOURCE_FILE_BYTES = 64 * 1024 * 1024
PRIVATE_PATH_TIMEOUT_SECONDS = 10
UINT32_LIMIT = 1 << 32
RUNTIME_BRIDGE_SYMBOLS = [
    "cache_op",
    "cop0_eret",
    "cop0_read",
    "cop0_status_read",
    "cop0_status_write",
    "cop0_tlb_op",
    "cop0_write",
    "do_break",
    "get_function",
    "jfg_minimal_runtime_bind_cpu",
    "jfg_minimal_runtime_bind_dispatch",
    "jfg_minimal_runtime_initialize",
    "jfg_minimal_runtime_section_capacity",
    "jfg_minimal_runtime_unbind_cpu",
    "jfg_minimal_runtime_unbind_dispatch",
    "pause_self",
    "recomp_syscall_handler",
    "reserved_instruction",
    "switch_error",
]
RAW_HEADER_CALL_SYMBOLS = ("DIV32", "DIVU32", *RUNTIME_BRIDGE_SYMBOLS)
C_NON_CODE_RE = re.compile(
    r"//[^\r\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'",
    re.DOTALL,
)
RECOMP_FUNC_DEFINITION_RE = re.compile(
    r"^[ \t]*#[ \t]*define[ \t]+RECOMP_FUNC(?:[ \t]+(?P<body>[^\r\n]*))?$",
    re.MULTILINE,
)


class NormalizationError(RuntimeError):
    """Raised with a public-safe diagnostic for malformed private inputs."""


def _is_reparse_point(path: Path) -> bool:
    try:
        metadata = path.lstat()
    except OSError:
        return False
    attributes = getattr(metadata, "st_file_attributes", 0)
    reparse = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0)
    return stat.S_ISLNK(metadata.st_mode) or bool(attributes & reparse)


def _repository_relative(path: Path) -> Path | None:
    try:
        return path.resolve(strict=False).relative_to(ROOT.resolve(strict=True))
    except ValueError:
        return None


def _is_private_path(path: Path) -> bool:
    relative = _repository_relative(path)
    if relative is None:
        return True
    try:
        result = subprocess.run(
            ["git", "-C", str(ROOT), "check-ignore", "-q", "--", relative.as_posix()],
            check=False,
            capture_output=True,
            timeout=PRIVATE_PATH_TIMEOUT_SECONDS,
        )
    except (OSError, subprocess.TimeoutExpired):
        return False
    return result.returncode == 0


def _require_private_regular_file(path: Path) -> Path:
    absolute = Path(os.path.abspath(path))
    if _is_reparse_point(absolute) or not _is_private_path(absolute):
        raise NormalizationError("input boundary is invalid")
    try:
        metadata = absolute.stat(follow_symlinks=False)
    except OSError as error:
        raise NormalizationError("input boundary is invalid") from error
    if not stat.S_ISREG(metadata.st_mode) or metadata.st_size > MAX_TEXT_INPUT_BYTES:
        raise NormalizationError("input boundary is invalid")
    return absolute


def _require_private_directory(path: Path) -> Path:
    absolute = Path(os.path.abspath(path))
    if _is_reparse_point(absolute) or not _is_private_path(absolute):
        raise NormalizationError("input boundary is invalid")
    try:
        metadata = absolute.stat(follow_symlinks=False)
    except OSError as error:
        raise NormalizationError("input boundary is invalid") from error
    if not stat.S_ISDIR(metadata.st_mode):
        raise NormalizationError("input boundary is invalid")
    return absolute


def _require_new_private_output(path: Path) -> Path:
    absolute = Path(os.path.abspath(path))
    if absolute == ROOT.resolve(strict=True) or not _is_private_path(absolute):
        raise NormalizationError("output boundary is invalid")
    if absolute.exists() or _is_reparse_point(absolute):
        raise NormalizationError("output boundary is invalid")
    parent = absolute.parent
    try:
        metadata = parent.stat(follow_symlinks=False)
    except OSError as error:
        raise NormalizationError("output boundary is invalid") from error
    if not stat.S_ISDIR(metadata.st_mode) or _is_reparse_point(parent):
        raise NormalizationError("output boundary is invalid")
    return absolute


def _read_text(path: Path, *, maximum: int = MAX_TEXT_INPUT_BYTES) -> str:
    source = _require_private_regular_file(path)
    try:
        size = source.stat(follow_symlinks=False).st_size
        if size > maximum:
            raise NormalizationError("input size is invalid")
        with source.open("r", encoding="utf-8", newline="") as stream:
            value = stream.read(maximum + 1)
    except (OSError, UnicodeError) as error:
        raise NormalizationError("input could not be read") from error
    if len(value.encode("utf-8")) > maximum:
        raise NormalizationError("input size is invalid")
    return value


def _reject_duplicate_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise NormalizationError("runtime manifest is invalid")
        result[key] = value
    return result


def _load_json(path: Path) -> object:
    try:
        return json.loads(
            _read_text(path),
            object_pairs_hook=_reject_duplicate_keys,
            parse_constant=lambda _: (_ for _ in ()).throw(
                NormalizationError("runtime manifest is invalid")
            ),
        )
    except (json.JSONDecodeError, TypeError, ValueError) as error:
        raise NormalizationError("runtime manifest is invalid") from error


def canonical_bytes(value: object) -> bytes:
    try:
        return json.dumps(
            value,
            sort_keys=True,
            separators=(",", ":"),
            ensure_ascii=True,
            allow_nan=False,
        ).encode("utf-8")
    except (TypeError, ValueError, OverflowError, RecursionError) as error:
        raise NormalizationError("semantic product is invalid") from error


def canonical_digest(value: object) -> str:
    return hashlib.sha256(canonical_bytes(value)).hexdigest()


def decision_by_unique_site(rows: list[dict[str, object]]) -> list[dict[str, object]]:
    unique: dict[tuple[int, int], dict[str, object]] = {}
    for row in rows:
        site = (int(row["source_section"]), int(row["source_offset"]))
        unique.setdefault(site, row)
    return [unique[site] for site in sorted(unique)]


def policy_approval_digest(
    policy_id: str,
    opaque_id: str,
    category: str,
    disposition: str,
) -> str:
    return canonical_digest(
        {
            "policy_id": policy_id,
            "opaque_id": opaque_id,
            "category": category,
            "disposition": disposition,
        }
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--raw-generated", type=Path, required=True)
    parser.add_argument("--symbols", type=Path, required=True)
    parser.add_argument("--original-context", type=Path, required=True)
    parser.add_argument("--runtime-manifest", type=Path, required=True)
    parser.add_argument("--recomp-header", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def write_text(path: Path, value: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(value, encoding="utf-8", newline="\n")


def extract_functions(raw_root: Path) -> dict[str, str]:
    raw_root = _require_private_directory(raw_root)
    result: dict[str, str] = {}
    total_bytes = 0
    for path in sorted(raw_root.glob("funcs_*.c"), key=lambda item: item.name):
        text = _read_text(path, maximum=MAX_RAW_SOURCE_FILE_BYTES)
        total_bytes += len(text.encode("utf-8"))
        if total_bytes > MAX_TEXT_INPUT_BYTES:
            raise NormalizationError("raw generated source set exceeds limit")
        # MSVC generators write CRLF; normalize only line endings after the
        # original byte limits have been checked. Label ownership stays strict.
        text = text.replace("\r\n", "\n")
        matches = list(FUNCTION_RE.finditer(text))
        if not matches:
            raise ValueError("raw generated source has no functions")
        for index, match in enumerate(matches):
            end = matches[index + 1].start() if index + 1 < len(matches) else len(text)
            body = text[match.start() : end].rstrip() + "\n"
            if (
                "__func__" in body
                or body.count("switch_error(")
                != body.count("switch_error((const char*)0")
            ):
                raise ValueError("raw generated source contains a non-opaque diagnostic name")
            name = match.group(1)
            if name in result:
                raise ValueError("duplicate generated function")
            result[name] = body
    return result


def load_function_inventory(symbols_path: Path) -> tuple[list[dict[str, int | str]], list[int]]:
    try:
        symbols = tomllib.loads(_read_text(symbols_path))
    except (tomllib.TOMLDecodeError, TypeError, ValueError) as error:
        raise NormalizationError("symbol inventory is invalid") from error
    sections = symbols.get("section")
    if not isinstance(sections, list):
        raise NormalizationError("symbol inventory is invalid")
    rows: list[dict[str, int | str]] = []
    section_bases: list[int] = []
    names: set[str] = set()
    for section_index, section in enumerate(sections):
        if not isinstance(section, dict):
            raise NormalizationError("symbol inventory is invalid")
        base_value = section.get("vram")
        functions = section.get("functions")
        if type(base_value) is not int or not isinstance(functions, list):
            raise NormalizationError("symbol inventory is invalid")
        base = base_value
        if not 0 <= base <= 0xFFFFFFFF:
            raise NormalizationError("symbol inventory is invalid")
        section_bases.append(base)
        for function in functions:
            if not isinstance(function, dict):
                raise NormalizationError("symbol inventory is invalid")
            name = function.get("name")
            vram = function.get("vram")
            size = function.get("size")
            if (
                not isinstance(name, str)
                or FUNCTION_RE.fullmatch(
                    f"RECOMP_FUNC void {name}(uint8_t* rdram, recomp_context* ctx) {{"
                )
                is None
                or name in names
                or type(vram) is not int
                or type(size) is not int
                or not base <= vram <= 0xFFFFFFFF
                or not 0 < size <= 0xFFFFFFFF
                or vram % 4 != 0
                or size % 4 != 0
                # Keep every emitted 32-bit range representable.  The
                # exclusive end may equal 2**32, but it must never wrap.
                or vram + size > UINT32_LIMIT
            ):
                raise NormalizationError("symbol inventory is invalid")
            names.add(name)
            rows.append(
                {
                    "name": name,
                    "section": section_index,
                    "offset": vram - base,
                    "vram": vram,
                    "size": size,
                }
            )
    return rows, section_bases


def validate_function_inventory_extents(
    rows: list[dict[str, int | str]], runtime_manifest: dict[str, object]
) -> None:
    """Require each generated function to fit wholly within its text section.

    The normalizer subsequently emits ``base + offset`` lookup expressions, so
    accepting a range that crosses a section boundary or a 32-bit boundary
    would make otherwise deterministic generated C unsafe.
    """
    sections = runtime_manifest.get("sections")
    if not isinstance(sections, list):
        raise NormalizationError("symbol inventory is invalid")
    for row in rows:
        section_index = row.get("section")
        vram = row.get("vram")
        size = row.get("size")
        offset = row.get("offset")
        if (
            type(section_index) is not int
            or type(vram) is not int
            or type(size) is not int
            or type(offset) is not int
            or not 0 <= section_index < len(sections)
            or not 0 <= vram <= 0xFFFFFFFF
            or not 0 < size <= 0xFFFFFFFF
            or not 0 <= offset <= 0xFFFFFFFF
        ):
            raise NormalizationError("symbol inventory is invalid")
        section = sections[section_index]
        if not isinstance(section, dict):
            raise NormalizationError("symbol inventory is invalid")
        base = section.get("linked_vram")
        text_offset = section.get("text_offset")
        text_size = section.get("text_size")
        if (
            type(base) is not int
            or type(text_offset) is not int
            or type(text_size) is not int
            or base + offset > 0xFFFFFFFF
            or vram != base + offset
            or vram + size > UINT32_LIMIT
            or offset + size > text_offset + text_size
        ):
            raise NormalizationError("symbol inventory is invalid")


def authoritative_coordinates(context_path: Path) -> set[tuple[int, int]]:
    try:
        context = tomllib.loads(_read_text(context_path))
    except (tomllib.TOMLDecodeError, TypeError, ValueError) as error:
        raise NormalizationError("authoritative context is invalid") from error
    sections = context.get("section")
    if not isinstance(sections, list):
        raise NormalizationError("authoritative context is invalid")
    result: set[tuple[int, int]] = set()
    for section in sections:
        if not isinstance(section, dict) or not isinstance(section.get("functions"), list):
            raise NormalizationError("authoritative context is invalid")
        for function in section["functions"]:
            if not isinstance(function, dict):
                raise NormalizationError("authoritative context is invalid")
            vram = function.get("vram")
            size = function.get("size")
            if (
                type(vram) is not int
                or type(size) is not int
                or not 0 <= vram <= 0xFFFFFFFF
                or not 0 < size <= 0xFFFFFFFF
                or vram % 4 != 0
                or size % 4 != 0
            ):
                raise NormalizationError("authoritative context is invalid")
            coordinate = (vram, size)
            if coordinate in result:
                raise NormalizationError("authoritative context is invalid")
            result.add(coordinate)
    return result


def make_cpu_section_inventory(
    rows: list[dict[str, int | str]],
    authoritative_coordinates_set: set[tuple[int, int]],
    runtime_manifest: dict[str, object],
) -> bytes:
    """Emit the private, coordinate-free per-section G3 denominator.

    The normalizer is the only component that simultaneously holds the
    authoritative pre-transform function set, the normalized generated rows,
    and the validated relocation manifest.  Deriving this inventory here keeps
    the later G2 CPU probe from accepting caller-authored section outcomes.
    """
    sections = runtime_manifest.get("sections")
    relocations = runtime_manifest.get("r_mips_32")
    overlay_slots = runtime_manifest.get("overlay_slots")
    if (
        not isinstance(sections, list)
        or not isinstance(relocations, list)
        or not isinstance(overlay_slots, list)
    ):
        raise NormalizationError("CPU section inventory inputs are invalid")

    expected = [0] * len(sections)
    generated = [0] * len(sections)
    for row in rows:
        section_index = row.get("section")
        vram = row.get("vram")
        size = row.get("size")
        if (
            type(section_index) is not int
            or not 0 <= section_index < len(sections)
            or type(vram) is not int
            or type(size) is not int
        ):
            raise NormalizationError("CPU section inventory inputs are invalid")
        if (vram, size) in authoritative_coordinates_set:
            expected[section_index] += 1
            generated[section_index] += 1

    semantic_ledgers = runtime_manifest.get("semantic_ledgers")
    covered_aliases = (
        semantic_ledgers.get("covered_alias_ledger")
        if isinstance(semantic_ledgers, dict)
        else None
    )
    if not isinstance(covered_aliases, list):
        raise NormalizationError("CPU covered-alias inventory is invalid")
    excluded = [0] * len(sections)
    for alias in covered_aliases:
        source = alias.get("private_source") if isinstance(alias, dict) else None
        section_index = source.get("section") if isinstance(source, dict) else None
        if type(section_index) is not int or not 0 <= section_index < len(sections):
            raise NormalizationError("CPU covered-alias inventory is invalid")
        expected[section_index] += 1
        excluded[section_index] += 1

    relocation_counts = [0] * len(sections)
    for relocation in relocations:
        if not isinstance(relocation, dict):
            raise NormalizationError("CPU section inventory inputs are invalid")
        source_section = relocation.get("source_section")
        if type(source_section) is not int or not 0 <= source_section < len(sections):
            raise NormalizationError("CPU section inventory inputs are invalid")
        relocation_counts[source_section] += 1

    inventory_rows: list[dict[str, int | str]] = []
    for index, section in enumerate(sections):
        if not isinstance(section, dict) or section.get("kind") not in {"main", "overlay"}:
            raise NormalizationError("CPU section inventory inputs are invalid")
        if (
            expected[index] == 0
            or generated[index] + excluded[index] != expected[index]
        ):
            raise NormalizationError("CPU section inventory denominator is incomplete")
        inventory_rows.append(
            {
                "section_id": f"section-{index:03d}",
                "kind": str(section["kind"]),
                "expected": expected[index],
                "generated": generated[index],
                "excluded": excluded[index],
                "lookups": generated[index],
                "lifecycle": 0 if section["kind"] == "main" else 1,
                "relocations": relocation_counts[index],
            }
        )

    slot_rows: list[dict[str, object]] = []
    for index, slot in enumerate(overlay_slots, start=1):
        if (
            not isinstance(slot, dict)
            or slot.get("slot") != index
            or slot.get("disposition") not in {"populated", "empty-fail-closed"}
        ):
            raise NormalizationError("CPU overlay slot inventory is invalid")
        section = slot.get("section")
        disposition = str(slot["disposition"])
        if disposition == "populated":
            if type(section) is not int or not 1 <= section < len(sections):
                raise NormalizationError("CPU overlay slot inventory is invalid")
            section_id: str | None = f"section-{section:03d}"
        elif section is None:
            section_id = None
        else:
            raise NormalizationError("CPU overlay slot inventory is invalid")
        slot_rows.append(
            {
                "slot_id": f"slot-{index:03d}",
                "disposition": disposition,
                "section_id": section_id,
            }
        )
    if (
        len(slot_rows) != EXPECTED_OVERLAY_SLOT_COUNT
        or sum(row["disposition"] == "populated" for row in slot_rows)
        != EXPECTED_OVERLAY_SLOT_COUNT - EXPECTED_EMPTY_OVERLAY_SLOT_COUNT
        or sum(row["disposition"] == "empty-fail-closed" for row in slot_rows)
        != EXPECTED_EMPTY_OVERLAY_SLOT_COUNT
    ):
        raise NormalizationError("CPU overlay slot denominator is incomplete")

    document = {
        "schema_version": 2,
        "kind": "jfg-phase4-cpu-section-inventory",
        "sections": inventory_rows,
        "overlay_slots": slot_rows,
    }
    return json.dumps(
        document,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
        allow_nan=False,
    ).encode("utf-8")


def validate_runtime_manifest(
    document: object,
    section_bases: list[int],
) -> tuple[dict[str, object], list[int]]:
    if not isinstance(document, dict) or set(document) != {
        "schema_version",
        "sections",
        "r_mips_32",
        "overlay_slots",
        "audit",
        "semantic_ledgers",
    }:
        raise NormalizationError("runtime manifest is invalid")
    if document.get("schema_version") != 1:
        raise NormalizationError("runtime manifest is invalid")
    sections = document.get("sections")
    relocations = document.get("r_mips_32")
    overlay_slots = document.get("overlay_slots")
    audit = document.get("audit")
    semantic_ledgers = document.get("semantic_ledgers")
    if (
        not isinstance(sections, list)
        or len(sections) != EXPECTED_SECTION_COUNT
        or len(sections) != len(section_bases)
        or not isinstance(relocations, list)
        or not isinstance(overlay_slots, list)
        or len(overlay_slots) != EXPECTED_OVERLAY_SLOT_COUNT
        or not isinstance(audit, dict)
        or not isinstance(semantic_ledgers, dict)
        or len(relocations) != EXPECTED_R32_RELOCATION_COUNT
    ):
        raise NormalizationError("runtime manifest denominators changed")

    initial_bases: list[int] = []
    for index, section in enumerate(sections):
        if not isinstance(section, dict):
            raise NormalizationError("runtime manifest is invalid")
        required = {
            "section",
            "module",
            "kind",
            "rom",
            "linked_vram",
            "text_offset",
            "text_size",
            "data_size",
            "bss_size",
        }
        if set(section) != required:
            raise NormalizationError("runtime manifest is invalid")
        kind = section.get("kind")
        numeric = [
            section.get("section"),
            section.get("module"),
            section.get("rom"),
            section.get("linked_vram"),
            section.get("text_offset"),
            section.get("text_size"),
            section.get("data_size"),
            section.get("bss_size"),
        ]
        if (
            kind not in {"main", "overlay"}
            or any(type(value) is not int or not 0 <= value <= 0xFFFFFFFF for value in numeric)
            or section.get("section") != index
            or section.get("linked_vram") != section_bases[index]
            or section.get("text_size") == 0
            or section.get("linked_vram") % 4 != 0
            or section.get("text_offset") % 4 != 0
            or section.get("text_size") % 4 != 0
            or section.get("data_size") % 4 != 0
            or section.get("bss_size") % 4 != 0
            # These extents feed generated ``base + offset`` expressions.
            # Reject them before generation rather than relying on C unsigned
            # wraparound at relocation or lookup time.
            or section.get("text_offset") + section.get("text_size") + section.get("data_size") + section.get("bss_size") > UINT32_LIMIT
            or section.get("linked_vram") + section.get("text_offset") + section.get("text_size") + section.get("data_size") + section.get("bss_size") > UINT32_LIMIT
        ):
            raise NormalizationError("runtime manifest is invalid")
        initial_bases.append(section_bases[index] if kind == "main" else 0)

    populated_modules = {
        int(section["module"]): index
        for index, section in enumerate(sections)
        if section.get("kind") == "overlay"
    }
    empty_slots = 0
    for expected_slot, row in enumerate(overlay_slots, 1):
        if not isinstance(row, dict) or set(row) != {"slot", "section", "disposition", "source_binding"} or row.get("slot") != expected_slot or not isinstance(row.get("source_binding"), str) or len(row["source_binding"]) != 64:
            raise NormalizationError("overlay slot ledger is invalid")
        section = row.get("section")
        if row.get("disposition") == "populated":
            if type(section) is not int or populated_modules.get(expected_slot) != section:
                raise NormalizationError("overlay slot ledger is invalid")
        elif row.get("disposition") == "empty-fail-closed" and section is None:
            empty_slots += 1
        else:
            raise NormalizationError("overlay slot ledger is invalid")
    if empty_slots != EXPECTED_EMPTY_OVERLAY_SLOT_COUNT:
        raise NormalizationError("overlay slot ledger is invalid")
    expected_audit_keys = {
        "overlay_slot_count",
        "empty_overlay_slot_count",
        "exception_ledger_sha256",
        "exception_approval_set_sha256",
        "exception_category_counts",
        "covered_alias_count",
        "covered_alias_body_start_count",
        "covered_alias_interior_count",
        "covered_alias_ledger_sha256",
        "covered_alias_approval_set_sha256",
        "manual_size_recovery_count",
        "manual_size_recovery_ledger_sha256",
        "manual_size_recovery_approval_set_sha256",
        "direct_call_candidate_count",
        "direct_linked_call_candidate_count",
        "direct_tail_candidate_count",
        "direct_instruction_class_counts",
        "direct_generated_target_candidate_count",
        "direct_unresolved_target_candidate_count",
        "direct_pending_n64recomp_observation_count",
        "direct_call_candidate_ledger_sha256",
        "direct_call_approval_set_sha256",
        "r26_relocation_count",
        "r26_relocation_resolved_count",
        "r26_relocation_exception_count",
        "instruction_relocation_count",
        "instruction_resolved_count",
        "instruction_fail_closed_count",
        "instruction_relocation_ledger_sha256",
        "instruction_relocation_approval_set_sha256",
        "hi_lo_pair_count",
        "r32_count",
        "r32_ledger_sha256",
        "r32_approval_set_sha256",
        "indirect_transfer_ledger_sha256",
        "indirect_transfer_count",
        "indirect_native_return_candidate_count",
        "indirect_decision_candidate_count",
        "indirect_pending_n64recomp_observation_count",
        "stub_ledger_sha256",
    }
    digest_fields = {
        key
        for key in expected_audit_keys
        if key.endswith("_sha256")
    }
    if (
        set(audit) != expected_audit_keys
        or audit.get("overlay_slot_count") != EXPECTED_OVERLAY_SLOT_COUNT
        or audit.get("empty_overlay_slot_count") != EXPECTED_EMPTY_OVERLAY_SLOT_COUNT
        or audit.get("covered_alias_count") != EXPECTED_COVERED_ALIAS_COUNT
        or audit.get("covered_alias_body_start_count")
        != EXPECTED_COVERED_ALIAS_BODY_START_COUNT
        or audit.get("covered_alias_interior_count")
        != EXPECTED_COVERED_ALIAS_INTERIOR_COUNT
        or audit.get("manual_size_recovery_count")
        != EXPECTED_MANUAL_SIZE_RECOVERY_COUNT
        or audit.get("direct_call_candidate_count")
        != EXPECTED_DIRECT_CALL_CANDIDATE_COUNT
        or audit.get("direct_linked_call_candidate_count")
        != EXPECTED_DIRECT_LINKED_CALL_CANDIDATE_COUNT
        or audit.get("direct_tail_candidate_count")
        != EXPECTED_DIRECT_TAIL_CANDIDATE_COUNT
        or audit.get("direct_instruction_class_counts")
        != dict(sorted(EXPECTED_DIRECT_INSTRUCTION_CLASS_COUNTS.items()))
        or audit.get("direct_generated_target_candidate_count")
        != EXPECTED_DIRECT_GENERATED_TARGET_CANDIDATE_COUNT
        or audit.get("direct_unresolved_target_candidate_count")
        != EXPECTED_DIRECT_UNRESOLVED_TARGET_CANDIDATE_COUNT
        or audit.get("direct_pending_n64recomp_observation_count")
        != EXPECTED_DIRECT_CALL_CANDIDATE_COUNT
        or audit.get("r26_relocation_count") != EXPECTED_R26_RELOCATION_COUNT
        or audit.get("r26_relocation_resolved_count")
        != EXPECTED_R26_RELOCATION_COUNT - 1
        or audit.get("r26_relocation_exception_count") != 1
        or audit.get("instruction_relocation_count")
        != EXPECTED_INSTRUCTION_RELOCATION_COUNT
        or audit.get("instruction_resolved_count")
        != EXPECTED_INSTRUCTION_RELOCATION_COUNT - 2
        or audit.get("instruction_fail_closed_count") != 2
        or audit.get("hi_lo_pair_count") != EXPECTED_HI_LO_PAIR_COUNT
        or audit.get("r32_count") != EXPECTED_R32_RELOCATION_COUNT
        or audit.get("indirect_transfer_count")
        != EXPECTED_INDIRECT_TRANSFER_SITE_COUNT
        or audit.get("indirect_native_return_candidate_count")
        != EXPECTED_NATIVE_RETURN_SITE_COUNT
        or audit.get("indirect_decision_candidate_count")
        != EXPECTED_INDIRECT_DECISION_SITE_COUNT
        or audit.get("indirect_pending_n64recomp_observation_count")
        != EXPECTED_INDIRECT_TRANSFER_SITE_COUNT
        or any(
            not isinstance(audit.get(key), str)
            or re.fullmatch(r"[0-9a-f]{64}", str(audit.get(key))) is None
            for key in digest_fields
        )
        or audit.get("exception_category_counts")
        != {"anomalous-r-mips-26": 1, "reserved-atomic-hi-lo": 2}
    ):
        raise NormalizationError("runtime audit ledger is invalid")

    if set(semantic_ledgers) != {
        "exception_ledger",
        "covered_alias_ledger",
        "manual_size_recovery_ledger",
        "direct_call_candidate_ledger",
        "indirect_transfer_ledger",
    }:
        raise NormalizationError("runtime semantic ledgers are invalid")
    exception_ledger = semantic_ledgers.get("exception_ledger")
    covered_alias_ledger = semantic_ledgers.get("covered_alias_ledger")
    manual_size_recovery_ledger = semantic_ledgers.get(
        "manual_size_recovery_ledger"
    )
    direct_candidate_ledger = semantic_ledgers.get("direct_call_candidate_ledger")
    indirect_ledger = semantic_ledgers.get("indirect_transfer_ledger")
    if (
        not isinstance(exception_ledger, list)
        or len(exception_ledger) != 3
        or not isinstance(covered_alias_ledger, list)
        or len(covered_alias_ledger) != EXPECTED_COVERED_ALIAS_COUNT
        or not isinstance(manual_size_recovery_ledger, list)
        or len(manual_size_recovery_ledger) != EXPECTED_MANUAL_SIZE_RECOVERY_COUNT
        or not isinstance(direct_candidate_ledger, list)
        or len(direct_candidate_ledger) != EXPECTED_DIRECT_CALL_CANDIDATE_COUNT
        or not isinstance(indirect_ledger, list)
        or len(indirect_ledger) != EXPECTED_INDIRECT_TRANSFER_SITE_COUNT
        or canonical_digest(exception_ledger) != audit.get("exception_ledger_sha256")
        or canonical_digest(indirect_ledger)
        != audit.get("indirect_transfer_ledger_sha256")
        or canonical_digest(direct_candidate_ledger)
        != audit.get("direct_call_candidate_ledger_sha256")
        or canonical_digest(covered_alias_ledger)
        != audit.get("covered_alias_ledger_sha256")
        or canonical_digest(manual_size_recovery_ledger)
        != audit.get("manual_size_recovery_ledger_sha256")
    ):
        raise NormalizationError("runtime semantic ledgers are invalid")
    exception_ids: set[str] = set()
    exception_approvals: list[str] = []
    exception_counts: dict[str, int] = {
        "anomalous-r-mips-26": 0,
        "reserved-atomic-hi-lo": 0,
    }
    exception_contracts = {
        "anomalous-r-mips-26": (
            "direct-call",
            "out-of-domain-overlay-reference",
            "fail-closed-trap",
        ),
        "reserved-atomic-hi-lo": (
            "instruction-relocation",
            "reserved-reference-class",
            "unresolved-data-atomic-pair",
        ),
    }
    for row in exception_ledger:
        if not isinstance(row, dict) or set(row) != {
            "opaque_id",
            "category",
            "evidence_class",
            "owner_role",
            "reason",
            "disposition",
            "approval_sha256",
            "private_record",
        }:
            raise NormalizationError("runtime exception ledger is invalid")
        opaque_id = row.get("opaque_id")
        category = row.get("category")
        contract = exception_contracts.get(category) if isinstance(category, str) else None
        if (
            not isinstance(opaque_id, str)
            or re.fullmatch(r"exception-[0-9a-f]{24}", opaque_id) is None
            or opaque_id in exception_ids
            or contract is None
            or row.get("owner_role") != "transformer"
            or (row.get("evidence_class"), row.get("reason"), row.get("disposition"))
            != contract
            or row.get("approval_sha256")
            != policy_approval_digest(
                "phase4-transform-exception-policy-v1",
                opaque_id,
                category,
                str(row.get("disposition")),
            )
            or not isinstance(row.get("private_record"), dict)
        ):
            raise NormalizationError("runtime exception ledger is invalid")
        exception_ids.add(opaque_id)
        exception_approvals.append(str(row["approval_sha256"]))
        exception_counts[category] += 1
    if (
        exception_counts != audit.get("exception_category_counts")
        or canonical_digest(sorted(exception_approvals))
        != audit.get("exception_approval_set_sha256")
    ):
        raise NormalizationError("runtime exception ledger is invalid")

    covered_alias_ids: set[str] = set()
    covered_alias_approvals: list[str] = []
    covered_alias_coordinates: set[tuple[str, int, int]] = set()
    covered_alias_role_counts = {
        "authoritative-body": 0,
        "alternate-entry-thunk": 0,
    }
    for row in covered_alias_ledger:
        if not isinstance(row, dict) or set(row) != {
            "opaque_id",
            "mapping_role",
            "evidence_class",
            "reason",
            "disposition",
            "approval_sha256",
            "generated_function",
            "private_source",
        }:
            raise NormalizationError("runtime covered-alias ledger is invalid")
        opaque_id = row.get("opaque_id")
        mapping_role = row.get("mapping_role")
        disposition = row.get("disposition")
        source = row.get("private_source")
        if (
            not isinstance(opaque_id, str)
            or re.fullmatch(r"alias-[0-9a-f]{24}", opaque_id) is None
            or opaque_id in covered_alias_ids
            or mapping_role not in covered_alias_role_counts
            or row.get("evidence_class") != "zero-size-covered-function"
            or row.get("reason") != "covered-address-in-authoritative-body"
            or disposition
            != (
                "deterministic-body-alias"
                if mapping_role == "authoritative-body"
                else "deterministic-thunk-alias"
            )
            or row.get("approval_sha256")
            != policy_approval_digest(
                "phase4-covered-alias-policy-v1",
                opaque_id,
                "zero-size-covered-alias",
                str(disposition),
            )
            or not isinstance(row.get("generated_function"), str)
            or not isinstance(source, dict)
            or set(source)
            != {
                "private_name",
                "private_owner_name",
                "section",
                "offset",
                "owner_offset",
                "owner_size",
            }
            or not isinstance(source.get("private_name"), str)
            or not source["private_name"]
            or not isinstance(source.get("private_owner_name"), str)
            or not source["private_owner_name"]
            or type(source.get("section")) is not int
            or not 0 <= source["section"] < EXPECTED_SECTION_COUNT
            or type(source.get("offset")) is not int
            or type(source.get("owner_offset")) is not int
            or type(source.get("owner_size")) is not int
            or source["offset"] < source["owner_offset"]
            or source["offset"] >= source["owner_offset"] + source["owner_size"]
            or source["offset"] % 4 != 0
            or source["owner_offset"] % 4 != 0
            or source["owner_size"] <= 0
            or source["owner_size"] % 4 != 0
            or (
                str(source["private_name"]),
                int(source["section"]),
                int(source["offset"]),
            )
            in covered_alias_coordinates
        ):
            raise NormalizationError("runtime covered-alias ledger is invalid")
        covered_alias_ids.add(opaque_id)
        covered_alias_approvals.append(str(row["approval_sha256"]))
        covered_alias_coordinates.add(
            (
                str(source["private_name"]),
                int(source["section"]),
                int(source["offset"]),
            )
        )
        covered_alias_role_counts[str(mapping_role)] += 1
    if (
        covered_alias_role_counts
        != {
            "authoritative-body": EXPECTED_COVERED_ALIAS_BODY_START_COUNT,
            "alternate-entry-thunk": EXPECTED_COVERED_ALIAS_INTERIOR_COUNT,
        }
        or canonical_digest(sorted(covered_alias_approvals))
        != audit.get("covered_alias_approval_set_sha256")
    ):
        raise NormalizationError("runtime covered-alias ledger is incomplete")

    manual_size_ids: set[str] = set()
    manual_size_approvals: list[str] = []
    for row in manual_size_recovery_ledger:
        if not isinstance(row, dict) or set(row) != {
            "opaque_id",
            "mapping_role",
            "evidence_class",
            "reason",
            "disposition",
            "approval_sha256",
            "generated_function",
            "private_source",
        }:
            raise NormalizationError("runtime manual-size ledger is invalid")
        opaque_id = row.get("opaque_id")
        source = row.get("private_source")
        if (
            not isinstance(opaque_id, str)
            or re.fullmatch(r"manual-size-[0-9a-f]{24}", opaque_id) is None
            or opaque_id in manual_size_ids
            or row.get("mapping_role") != "authoritative-body"
            or row.get("evidence_class") != "uncovered-zero-size-function"
            or row.get("reason") != "next-executable-symbol-boundary"
            or row.get("disposition") != "recovered-authoritative-body"
            or row.get("approval_sha256")
            != policy_approval_digest(
                "phase4-manual-size-recovery-policy-v1",
                opaque_id,
                "uncovered-zero-size-function",
                "recovered-authoritative-body",
            )
            or not isinstance(row.get("generated_function"), str)
            or not isinstance(source, dict)
            or set(source)
            != {
                "private_name",
                "generated_function",
                "section",
                "offset",
                "recovered_size",
            }
            or source.get("generated_function") != row.get("generated_function")
            or not isinstance(source.get("private_name"), str)
            or type(source.get("section")) is not int
            or not 0 <= source["section"] < EXPECTED_SECTION_COUNT
            or type(source.get("offset")) is not int
            or source["offset"] < 0
            or source["offset"] % 4 != 0
            or type(source.get("recovered_size")) is not int
            or source["recovered_size"] <= 0
            or source["recovered_size"] % 4 != 0
        ):
            raise NormalizationError("runtime manual-size ledger is invalid")
        manual_size_ids.add(opaque_id)
        manual_size_approvals.append(str(row["approval_sha256"]))
    if canonical_digest(sorted(manual_size_approvals)) != audit.get(
        "manual_size_recovery_approval_set_sha256"
    ):
        raise NormalizationError("runtime manual-size ledger is incomplete")

    direct_candidate_ids: set[str] = set()
    direct_candidate_sites: set[tuple[int, int]] = set()
    generated_target_candidates = 0
    unresolved_target_candidates = 0
    approved_direct_candidates = 0
    direct_role_counts: dict[str, int] = {
        "linked-call": 0,
        "direct-tail": 0,
    }
    direct_instruction_class_counts: dict[str, int] = {
        "jal": 0,
        "bgezal": 0,
        "j": 0,
        "conditional-branch": 0,
    }
    for row in direct_candidate_ledger:
        if not isinstance(row, dict) or set(row) != {
            "opaque_id",
            "source_section",
            "source_offset",
            "transfer_role",
            "instruction_class",
            "decoded_target_vram",
            "target_section",
            "target_offset",
            "target_class",
            "disposition",
        }:
            raise NormalizationError("runtime direct-call candidate ledger is invalid")
        opaque_id = row.get("opaque_id")
        source_section = row.get("source_section")
        source_offset = row.get("source_offset")
        target_section = row.get("target_section")
        target_offset = row.get("target_offset")
        target_class = row.get("target_class")
        disposition = row.get("disposition")
        transfer_role = row.get("transfer_role")
        instruction_class = row.get("instruction_class")
        if (
            not isinstance(opaque_id, str)
            or re.fullmatch(r"direct-call-[0-9a-f]{24}", opaque_id) is None
            or opaque_id in direct_candidate_ids
            or type(source_section) is not int
            or not 0 <= source_section < EXPECTED_SECTION_COUNT
            or type(source_offset) is not int
            or source_offset < 0
            or source_offset % 4 != 0
            or (source_section, source_offset) in direct_candidate_sites
            or type(row.get("decoded_target_vram")) is not int
            or not 0 <= row["decoded_target_vram"] <= 0xFFFFFFFF
            or disposition
            not in {
                "requires-n64recomp-direct-observation",
                "approved-fail-closed-trap",
            }
            or transfer_role not in direct_role_counts
            or instruction_class not in direct_instruction_class_counts
            or (
                transfer_role == "linked-call"
                and instruction_class not in {"jal", "bgezal"}
            )
            or (
                transfer_role == "direct-tail"
                and instruction_class not in {"j", "conditional-branch"}
            )
        ):
            raise NormalizationError("runtime direct-call candidate ledger is invalid")
        source = sections[source_section]
        if (
            not isinstance(source, dict)
            or source_offset < source["text_offset"]
            or source_offset + 4 > source["text_offset"] + source["text_size"]
        ):
            raise NormalizationError("runtime direct-call candidate source is invalid")
        if target_class in {"authoritative-body", "alternate-entry"}:
            if (
                type(target_section) is not int
                or not 0 <= target_section < EXPECTED_SECTION_COUNT
                or type(target_offset) is not int
                or target_offset < 0
                or target_offset % 4 != 0
            ):
                raise NormalizationError("runtime direct-call candidate target is invalid")
            target = sections[target_section]
            if (
                not isinstance(target, dict)
                or target_offset < target["text_offset"]
                or target_offset + 4 > target["text_offset"] + target["text_size"]
                or target["linked_vram"] + target_offset
                != row["decoded_target_vram"]
            ):
                raise NormalizationError("runtime direct-call candidate target is invalid")
            generated_target_candidates += 1
        elif (
            target_class == "unresolved-candidate"
            and target_section is None
            and target_offset is None
            and disposition != "approved-fail-closed-trap"
        ):
            unresolved_target_candidates += 1
        else:
            raise NormalizationError("runtime direct-call candidate target is invalid")
        approved_direct_candidates += int(disposition == "approved-fail-closed-trap")
        direct_role_counts[str(transfer_role)] += 1
        direct_instruction_class_counts[str(instruction_class)] += 1
        direct_candidate_ids.add(opaque_id)
        direct_candidate_sites.add((source_section, source_offset))
    if (
        generated_target_candidates
        != audit.get("direct_generated_target_candidate_count")
        or unresolved_target_candidates
        != audit.get("direct_unresolved_target_candidate_count")
        or approved_direct_candidates != 1
        or direct_role_counts
        != EXPECTED_DIRECT_TRANSFER_ROLE_COUNTS
        or direct_instruction_class_counts
        != EXPECTED_DIRECT_INSTRUCTION_CLASS_COUNTS
        or audit.get("direct_instruction_class_counts")
        != dict(sorted(direct_instruction_class_counts.items()))
    ):
        raise NormalizationError("runtime direct-call candidate ledger is incomplete")

    indirect_ids: set[str] = set()
    indirect_sites: set[tuple[int, int]] = set()
    indirect_discovery_counts = {
        "native-return-candidate": 0,
        "indirect-decision-candidate": 0,
    }
    for row in indirect_ledger:
        if not isinstance(row, dict) or set(row) != {
            "opaque_id",
            "transfer_kind",
            "source_register",
            "discovery_class",
            "disposition",
            "private_site",
        }:
            raise NormalizationError("runtime indirect ledger is invalid")
        opaque_id = row.get("opaque_id")
        site = row.get("private_site")
        if (
            not isinstance(opaque_id, str)
            or re.fullmatch(r"indirect-[0-9a-f]{24}", opaque_id) is None
            or opaque_id in indirect_ids
            or row.get("transfer_kind") not in {"jr", "jalr"}
            or type(row.get("source_register")) is not int
            or not 0 <= row["source_register"] < 32
            or row.get("discovery_class") not in indirect_discovery_counts
            or row.get("discovery_class")
            != (
                "native-return-candidate"
                if row["source_register"] == 31
                else "indirect-decision-candidate"
            )
            or row.get("disposition") != "requires-n64recomp-observation"
            or not isinstance(site, dict)
            or set(site) != {"section", "offset"}
            or type(site.get("section")) is not int
            or not 0 <= site["section"] < EXPECTED_SECTION_COUNT
            or type(site.get("offset")) is not int
            or site["offset"] < 0
            or site["offset"] % 4 != 0
            or (site["section"], site["offset"]) in indirect_sites
        ):
            raise NormalizationError("runtime indirect ledger is invalid")
        indirect_ids.add(opaque_id)
        indirect_sites.add((site["section"], site["offset"]))
        indirect_discovery_counts[str(row["discovery_class"])] += 1
    if indirect_discovery_counts != {
        "native-return-candidate": EXPECTED_NATIVE_RETURN_SITE_COUNT,
        "indirect-decision-candidate": EXPECTED_INDIRECT_DECISION_SITE_COUNT,
    }:
        raise NormalizationError("runtime indirect ledger is incomplete")

    seen_sites: set[tuple[int, int]] = set()
    for relocation in relocations:
        if not isinstance(relocation, dict) or set(relocation) != {
            "source_section",
            "source_type",
            "site_offset",
            "target_class",
            "target_section",
            "target_section_offset",
        }:
            raise NormalizationError("runtime manifest is invalid")
        values = [
            relocation.get("source_section"),
            relocation.get("source_type"),
            relocation.get("site_offset"),
            relocation.get("target_section"),
            relocation.get("target_section_offset"),
        ]
        if any(type(value) is not int or not 0 <= value <= 0xFFFFFFFF for value in values):
            raise NormalizationError("runtime manifest is invalid")
        source_section = relocation["source_section"]
        site_offset = relocation["site_offset"]
        target_section = relocation["target_section"]
        target_offset = relocation["target_section_offset"]
        if source_section >= len(sections) or target_section >= len(sections):
            raise NormalizationError("runtime manifest is invalid")
        source_extent = (
            sections[source_section]["text_offset"]
            + sections[source_section]["text_size"]
            + sections[source_section]["data_size"]
        )
        target_extent = (
            sections[target_section]["text_offset"]
            + sections[target_section]["text_size"]
            + sections[target_section]["data_size"]
            + sections[target_section]["bss_size"]
        )
        source_base = sections[source_section]["linked_vram"]
        target_base = sections[target_section]["linked_vram"]
        if (
            relocation.get("source_type") not in {0, 1, 3}
            or relocation.get("target_class") not in {"local-offset", "overlay"}
            or site_offset % 4 != 0
            or target_offset % 4 != 0
            or site_offset + 4 > source_extent
            or target_offset >= target_extent
            or source_base + site_offset > 0xFFFFFFFF
            or target_base + target_offset > 0xFFFFFFFF
        ):
            raise NormalizationError("runtime manifest is invalid")
        site = (source_section, site_offset)
        if site in seen_sites:
            raise NormalizationError("runtime manifest is invalid")
        seen_sites.add(site)
    return document, initial_bases


def load_n64recomp_decision_sidecar(
    raw_root: Path,
    raw_functions: dict[str, str],
    function_rows: list[dict[str, int | str]],
    runtime_manifest: dict[str, object],
) -> tuple[dict[str, object], bytes, dict[str, int]]:
    """Validate A5 observations against both private discovery ledgers."""
    raw_root = _require_private_directory(raw_root)
    sidecar_path = raw_root / "indirect_decisions.json"
    document = _load_json(sidecar_path)
    if (
        not isinstance(document, dict)
        or set(document)
        != {"schema_version", "kind", "generated_callable_set", "decisions", "direct_calls"}
        or type(document.get("schema_version")) is not int
        or document.get("schema_version") not in (1, 2)
        or document.get("kind") != "n64recomp-indirect-decision-sidecar"
    ):
        raise NormalizationError("N64Recomp decision sidecar is invalid")
    expected_callable_members = [
        {
            "generated_function": str(row["name"]),
            "source_section": int(row["section"]),
            "source_offset": int(row["offset"]),
            "size": int(row["size"]),
        }
        for row in sorted(
            function_rows,
            key=lambda item: (
                int(item["section"]), int(item["offset"]), -int(item["size"]), str(item["name"])
            ),
        )
    ]
    if document.get("generated_callable_set") != {
        "kind": "exact-generated-callable-entry-set",
        "member_count": len(expected_callable_members),
        "members": expected_callable_members,
    }:
        raise NormalizationError("N64Recomp callable set is invalid")
    decisions = document.get("decisions")
    direct_calls = document.get("direct_calls")
    semantic_ledgers = runtime_manifest.get("semantic_ledgers")
    runtime_sections = runtime_manifest.get("sections")
    if (
        not isinstance(decisions, list)
        or not isinstance(direct_calls, list)
        or not isinstance(semantic_ledgers, dict)
        or not isinstance(runtime_sections, list)
    ):
        raise NormalizationError("N64Recomp decision sidecar is invalid")
    discovery_rows = semantic_ledgers.get("indirect_transfer_ledger")
    direct_candidates = semantic_ledgers.get("direct_call_candidate_ledger")
    if not isinstance(discovery_rows, list) or not isinstance(direct_candidates, list):
        raise NormalizationError("N64Recomp decision sidecar is invalid")
    function_by_name = {str(row["name"]): row for row in function_rows}
    callable_coordinates = {
        (int(row["source_section"]), int(row["source_offset"]))
        for row in expected_callable_members
    }

    discovery_by_site = {
        (int(row["private_site"]["section"]), int(row["private_site"]["offset"])): row
        for row in discovery_rows
    }
    indirect_emissions: dict[tuple[int, int], set[str]] = {}
    indirect_projection: dict[tuple[int, int], bytes] = {}
    classification_counts = {
        "native-return": 0,
        "bounded-switch": 0,
        "dynamic-call": 0,
        "dynamic-tail": 0,
    }
    if document["schema_version"] == 2:
        classification_counts["dynamic-tail-or-return"] = 0
    unique_classifications: dict[tuple[int, int], str] = {}
    for row in decisions:
        if not isinstance(row, dict) or set(row) != {
            "classification", "default_disposition", "generated_function", "range",
            "source_offset", "source_register", "source_section",
        }:
            raise NormalizationError("N64Recomp decision sidecar is invalid")
        classification = row.get("classification")
        source_section = row.get("source_section")
        source_offset = row.get("source_offset")
        source_register = row.get("source_register")
        generated_function = row.get("generated_function")
        range_value = row.get("range")
        site = (source_section, source_offset)
        discovery = discovery_by_site.get(site)
        if (
            classification not in classification_counts
            or type(source_section) is not int
            or type(source_offset) is not int
            or type(source_register) is not int
            or not isinstance(generated_function, str)
            or generated_function not in raw_functions
            or generated_function not in function_by_name
            or not isinstance(range_value, dict)
            or not isinstance(discovery, dict)
            or discovery.get("source_register") != source_register
        ):
            raise NormalizationError("N64Recomp decision sidecar is invalid")
        if classification == "native-return":
            if (
                source_register != 31
                or row.get("default_disposition") != "fail-closed-return-context"
                or range_value != {"kind": "active-native-return-continuation"}
            ):
                raise NormalizationError("N64Recomp native return decision is invalid")
        elif classification == "bounded-switch":
            members = range_value.get("members")
            if (
                source_register == 31
                or row.get("default_disposition") != "fail-closed-switch-error"
                or set(range_value) != {"kind", "members"}
                or range_value.get("kind") != "exact-target-member-set"
                or not isinstance(members, list)
                or not members
                or members != sorted(members, key=lambda item: (item.get("target_section", -1), item.get("target_offset", -1)) if isinstance(item, dict) else (-1, -1))
            ):
                raise NormalizationError("N64Recomp bounded switch decision is invalid")
            seen_members: set[tuple[int, int]] = set()
            for member in members:
                if not isinstance(member, dict) or set(member) != {"target_section", "target_offset"}:
                    raise NormalizationError("N64Recomp bounded switch decision is invalid")
                target = (member.get("target_section"), member.get("target_offset"))
                if (
                    type(target[0]) is not int
                    or type(target[1]) is not int
                    or target[0] != source_section
                    or target in seen_members
                    or not 0 <= target[0] < len(runtime_sections)
                    or target[1] % 4 != 0
                ):
                    raise NormalizationError("N64Recomp bounded switch decision is invalid")
                section = runtime_sections[target[0]]
                if (
                    not isinstance(section, dict)
                    or target[1] < section["text_offset"]
                    or target[1] + 4 > section["text_offset"] + section["text_size"]
                ):
                    raise NormalizationError("N64Recomp switch target is not executable")
                seen_members.add(target)
        elif classification == "dynamic-tail-or-return":
            if (
                document["schema_version"] != 2
                or source_register == 31
                or row.get("default_disposition") != "fail-closed-lookup-miss"
                or range_value != {"kind": "generated-callable-or-matching-incoming-link"}
                or "const gpr guest_return_link = ctx->r31;" not in raw_functions[generated_function]
                or "if (guest_call_target == guest_return_link) return;" not in raw_functions[generated_function]
            ):
                raise NormalizationError("N64Recomp guarded return decision is invalid")
        elif (
            source_register == 31
            or row.get("default_disposition") != "fail-closed-lookup-miss"
            or range_value != {"kind": "generated-callable-set"}
        ):
            raise NormalizationError("N64Recomp dynamic decision is invalid")
        owner = function_by_name[generated_function]
        if (
            int(owner["section"]) != source_section
            or not int(owner["offset"]) <= source_offset < int(owner["offset"]) + int(owner["size"])
        ):
            raise NormalizationError("N64Recomp decision owner is invalid")
        projection = canonical_bytes({
            "classification": classification,
            "default_disposition": row["default_disposition"],
            "range": range_value,
            "source_register": source_register,
        })
        if site in indirect_projection and indirect_projection[site] != projection:
            raise NormalizationError("N64Recomp duplicate-site decisions differ")
        indirect_projection[site] = projection
        indirect_emissions.setdefault(site, set()).add(generated_function)
        unique_classifications[site] = str(classification)
        classification_counts[str(classification)] += 1
    expected_indirect_owners = {
        site: {
            str(row["name"])
            for row in function_rows
            if int(row["section"]) == site[0]
            and int(row["offset"]) <= site[1] < int(row["offset"]) + int(row["size"])
        }
        for site in discovery_by_site
    }
    if (
        set(indirect_projection) != set(discovery_by_site)
        or indirect_emissions != expected_indirect_owners
        or len(indirect_projection) != EXPECTED_INDIRECT_TRANSFER_SITE_COUNT
        or sum(value == "native-return" for value in unique_classifications.values())
        != EXPECTED_NATIVE_RETURN_SITE_COUNT
        or sum(value != "native-return" for value in unique_classifications.values())
        != EXPECTED_INDIRECT_DECISION_SITE_COUNT
        or len(decisions) != sum(len(value) for value in expected_indirect_owners.values())
    ):
        raise NormalizationError("N64Recomp decision sidecar does not close discovery")

    candidate_by_site = {
        (int(row["source_section"]), int(row["source_offset"])): row
        for row in direct_candidates
    }
    direct_emissions: dict[tuple[int, int], set[str]] = {}
    direct_projection: dict[tuple[int, int], bytes] = {}
    for row in direct_calls:
        if not isinstance(row, dict) or set(row) != {
            "classification", "default_disposition", "generated_function",
            "instruction_class", "source_offset", "source_section", "target", "transfer_role",
        }:
            raise NormalizationError("N64Recomp direct-call sidecar is invalid")
        site = (row.get("source_section"), row.get("source_offset"))
        candidate = candidate_by_site.get(site)
        owner_name = row.get("generated_function")
        target = row.get("target")
        if (
            not isinstance(candidate, dict)
            or not isinstance(owner_name, str)
            or owner_name not in function_by_name
            or row.get("classification") != "checked-lookup-call"
            or row.get("default_disposition") != "fail-closed-lookup-miss"
            or row.get("transfer_role") != candidate.get("transfer_role")
            or row.get("instruction_class") != candidate.get("instruction_class")
            or target != {"address": candidate.get("decoded_target_vram"), "kind": "runtime-address"}
        ):
            raise NormalizationError("N64Recomp direct-call sidecar is invalid")
        owner = function_by_name[owner_name]
        if (
            int(owner["section"]) != site[0]
            or not int(owner["offset"]) <= site[1] < int(owner["offset"]) + int(owner["size"])
        ):
            raise NormalizationError("N64Recomp direct-call owner is invalid")
        projection = canonical_bytes({
            "classification": row["classification"],
            "default_disposition": row["default_disposition"],
            "instruction_class": row["instruction_class"],
            "target": target,
            "transfer_role": row["transfer_role"],
        })
        if site in direct_projection and direct_projection[site] != projection:
            raise NormalizationError("N64Recomp duplicate-site direct calls differ")
        direct_projection[site] = projection
        direct_emissions.setdefault(site, set()).add(owner_name)
    expected_direct_owners: dict[tuple[int, int], set[str]] = {}
    for site, candidate in candidate_by_site.items():
        owners: set[str] = set()
        for row in function_rows:
            if (
                int(row["section"]) != site[0]
                or not int(row["offset"]) <= site[1] < int(row["offset"]) + int(row["size"])
            ):
                continue
            if candidate["transfer_role"] == "direct-tail":
                section = runtime_sections[int(row["section"])]
                owner_start = int(section["linked_vram"]) + int(row["offset"])
                if owner_start <= int(candidate["decoded_target_vram"]) < owner_start + int(row["size"]):
                    continue
            owners.add(str(row["name"]))
        expected_direct_owners[site] = owners
    generated_target_count = sum(
        row.get("target_class") in {"authoritative-body", "alternate-entry"}
        and (row.get("target_section"), row.get("target_offset")) in callable_coordinates
        for row in direct_candidates
    )
    if (
        set(direct_projection) != set(candidate_by_site)
        or direct_emissions != expected_direct_owners
        or len(direct_projection) != EXPECTED_DIRECT_CALL_CANDIDATE_COUNT
        or len(direct_calls) != sum(len(value) for value in expected_direct_owners.values())
        or generated_target_count != EXPECTED_DIRECT_GENERATED_TARGET_CANDIDATE_COUNT
    ):
        raise NormalizationError("N64Recomp direct-call sidecar does not close discovery")
    payload = canonical_bytes(document)
    try:
        original = _read_text(sidecar_path).encode("utf-8")
    except UnicodeError as error:
        raise NormalizationError("N64Recomp decision sidecar is invalid") from error
    if original not in {payload, payload + b"\n"}:
        raise NormalizationError("N64Recomp decision sidecar is not canonical")
    return document, payload, classification_counts


def _section_id(index: int) -> str:
    return f"section-{index:03d}"


def _slot_id(index: int) -> str:
    return f"slot-{index:03d}"


def make_alternate_entry_ledger(
    support: list[dict[str, int | str]],
    runtime_manifest: dict[str, object],
) -> bytes:
    semantic_ledgers = runtime_manifest.get("semantic_ledgers")
    covered_aliases = (
        semantic_ledgers.get("covered_alias_ledger")
        if isinstance(semantic_ledgers, dict)
        else None
    )
    if not isinstance(covered_aliases, list):
        raise NormalizationError("alternate-entry alias evidence is invalid")
    alias_coordinates = {
        (int(source["section"]), int(source["offset"]))
        for item in covered_aliases
        if isinstance(item, dict)
        and item.get("mapping_role") == "alternate-entry-thunk"
        and isinstance((source := item.get("private_source")), dict)
        and type(source.get("section")) is int
        and type(source.get("offset")) is int
    }
    entries: list[dict[str, object]] = []
    approvals: list[str] = []
    for row in sorted(
        support,
        key=lambda item: (
            int(item["section"]),
            int(item["offset"]),
            str(item["name"]),
        ),
    ):
        opaque_id = "alternate-" + canonical_digest(
            {
                "name": row["name"],
                "section": row["section"],
                "offset": row["offset"],
                "size": row["size"],
            }
        )[:24]
        is_alias = (int(row["section"]), int(row["offset"])) in alias_coordinates
        evidence_class = (
            "covered-function-alias" if is_alias else "control-transfer-entry"
        )
        reason_code = (
            "zero-size-covered-address"
            if is_alias
            else "validated-control-transfer-entry"
        )
        approval = policy_approval_digest(
            "phase4-alternate-entry-policy-v1",
            opaque_id,
            reason_code,
            "deterministic-generated-thunk",
        )
        approvals.append(approval)
        entries.append(
            {
                "opaque_id": opaque_id,
                "section_id": _section_id(int(row["section"])),
                "evidence_class": evidence_class,
                "owner_role": "alternate-entry-thunk",
                "reason_code": reason_code,
                "disposition": "deterministic-generated-thunk",
                "approval_sha256": approval,
            }
        )
    reason_counts = {
        reason: sum(row["reason_code"] == reason for row in entries)
        for reason in (
            "validated-control-transfer-entry",
            "zero-size-covered-address",
        )
    }
    if (
        reason_counts
        != {
            "validated-control-transfer-entry": (
                EXPECTED_SUPPORT_THUNK_COUNT - EXPECTED_COVERED_ALIAS_INTERIOR_COUNT
            ),
            "zero-size-covered-address": EXPECTED_COVERED_ALIAS_INTERIOR_COUNT,
        }
        or len(entries) != EXPECTED_SUPPORT_THUNK_COUNT
        or len(
        {str(row["opaque_id"]) for row in entries}
        )
        != len(entries)
    ):
        raise NormalizationError("alternate-entry ledger is invalid")
    return canonical_bytes(
        {
            "schema_version": 1,
            "kind": "jfg-phase4-alternate-entry-ledger",
            "policy_id": "phase4-alternate-entry-policy-v1",
            "entry_count": len(entries),
            "reason_counts": reason_counts,
            "approval_set_sha256": canonical_digest(sorted(approvals)),
            "entries": entries,
        }
    )


def make_exception_ledger(runtime_manifest: dict[str, object]) -> bytes:
    semantic_ledgers = runtime_manifest["semantic_ledgers"]
    audit = runtime_manifest["audit"]
    assert isinstance(semantic_ledgers, dict) and isinstance(audit, dict)
    source = semantic_ledgers["exception_ledger"]
    assert isinstance(source, list)
    entries = [
        {
            key: row[key]
            for key in (
                "opaque_id",
                "category",
                "evidence_class",
                "owner_role",
                "reason",
                "disposition",
                "approval_sha256",
            )
        }
        for row in source
        if isinstance(row, dict)
    ]
    if len(entries) != len(source):
        raise NormalizationError("exception ledger is invalid")
    return canonical_bytes(
        {
            "schema_version": 1,
            "kind": "jfg-phase4-approved-exception-ledger",
            "policy_id": "phase4-transform-exception-policy-v1",
            "entry_count": len(entries),
            "category_counts": audit["exception_category_counts"],
            "source_ledger_sha256": audit["exception_ledger_sha256"],
            "approval_set_sha256": audit["exception_approval_set_sha256"],
            "entries": entries,
        }
    )


def make_covered_alias_ledger(runtime_manifest: dict[str, object]) -> bytes:
    semantic_ledgers = runtime_manifest["semantic_ledgers"]
    audit = runtime_manifest["audit"]
    assert isinstance(semantic_ledgers, dict) and isinstance(audit, dict)
    source = semantic_ledgers["covered_alias_ledger"]
    assert isinstance(source, list)
    entries = [
        {
            "opaque_id": row["opaque_id"],
            "section_id": _section_id(int(row["private_source"]["section"])),
            "mapping_role": row["mapping_role"],
            "evidence_class": row["evidence_class"],
            "reason_code": "zero-size-covered-address",
            "disposition": row["disposition"],
            "approval_sha256": row["approval_sha256"],
        }
        for row in source
    ]
    return canonical_bytes(
        {
            "schema_version": 1,
            "kind": "jfg-phase4-covered-alias-ledger",
            "policy_id": "phase4-covered-alias-policy-v1",
            "entry_count": len(entries),
            "mapping_role_counts": {
                role: sum(row["mapping_role"] == role for row in entries)
                for role in ("authoritative-body", "alternate-entry-thunk")
            },
            "source_ledger_sha256": audit["covered_alias_ledger_sha256"],
            "approval_set_sha256": audit["covered_alias_approval_set_sha256"],
            "entries": entries,
        }
    )


def make_manual_size_recovery_ledger(runtime_manifest: dict[str, object]) -> bytes:
    semantic_ledgers = runtime_manifest["semantic_ledgers"]
    audit = runtime_manifest["audit"]
    assert isinstance(semantic_ledgers, dict) and isinstance(audit, dict)
    source = semantic_ledgers["manual_size_recovery_ledger"]
    assert isinstance(source, list)
    entries = [
        {
            "opaque_id": row["opaque_id"],
            "section_id": _section_id(int(row["private_source"]["section"])),
            "owner_role": "authoritative-body",
            "evidence_class": row["evidence_class"],
            "reason_code": "next-executable-symbol-boundary",
            "disposition": row["disposition"],
            "approval_sha256": row["approval_sha256"],
        }
        for row in source
    ]
    return canonical_bytes(
        {
            "schema_version": 1,
            "kind": "jfg-phase4-manual-size-recovery-ledger",
            "policy_id": "phase4-manual-size-recovery-policy-v1",
            "entry_count": len(entries),
            "source_ledger_sha256": audit["manual_size_recovery_ledger_sha256"],
            "approval_set_sha256": audit[
                "manual_size_recovery_approval_set_sha256"
            ],
            "entries": entries,
        }
    )


def make_overlay_slot_inventory(runtime_manifest: dict[str, object]) -> bytes:
    sections = runtime_manifest["sections"]
    slots = runtime_manifest["overlay_slots"]
    assert isinstance(sections, list) and isinstance(slots, list)
    rows = [
        {
            "slot_id": _slot_id(int(row["slot"])),
            "disposition": row["disposition"],
            "section_id": (
                _section_id(int(row["section"]))
                if row["section"] is not None
                else None
            ),
            "source_binding_sha256": row["source_binding"],
        }
        for row in slots
        if isinstance(row, dict)
    ]
    populated = sum(row["disposition"] == "populated" for row in rows)
    empty = sum(row["disposition"] == "empty-fail-closed" for row in rows)
    if (
        len(sections) != EXPECTED_SECTION_COUNT
        or len(rows) != EXPECTED_OVERLAY_SLOT_COUNT
        or populated != EXPECTED_OVERLAY_SLOT_COUNT - EXPECTED_EMPTY_OVERLAY_SLOT_COUNT
        or empty != EXPECTED_EMPTY_OVERLAY_SLOT_COUNT
    ):
        raise NormalizationError("overlay slot inventory is invalid")
    return canonical_bytes(
        {
            "schema_version": 1,
            "kind": "jfg-phase4-overlay-slot-inventory",
            "executable_section_count": len(sections),
            "slot_count": len(rows),
            "populated_slot_count": populated,
            "empty_slot_count": empty,
            "slots": rows,
        }
    )


def make_lookup_inventory(
    rows: list[dict[str, int | str]],
    authoritative_coordinates_set: set[tuple[int, int]],
    runtime_manifest: dict[str, object],
) -> bytes:
    section_counts = [
        {"authoritative_entry_count": 0, "alternate_entry_count": 0}
        for _ in range(EXPECTED_SECTION_COUNT)
    ]
    entries: list[dict[str, str]] = []
    for row in sorted(
        rows,
        key=lambda item: (
            int(item["section"]),
            int(item["offset"]),
            -int(item["size"]),
        ),
    ):
        entry_class = (
            "authoritative-body"
            if (int(row["vram"]), int(row["size"]))
            in authoritative_coordinates_set
            else "alternate-entry"
        )
        section_index = int(row["section"])
        count_key = (
            "authoritative_entry_count"
            if entry_class == "authoritative-body"
            else "alternate_entry_count"
        )
        section_counts[section_index][count_key] += 1
        entries.append(
            {
                "entry_id": "lookup-"
                + canonical_digest(
                    {
                        "name": row["name"],
                        "section": section_index,
                        "offset": row["offset"],
                        "size": row["size"],
                    }
                )[:24],
                "section_id": _section_id(section_index),
                "entry_class": entry_class,
            }
        )
    sections = [
        {"section_id": _section_id(index), **counts}
        for index, counts in enumerate(section_counts)
    ]
    authoritative_count = sum(
        row["authoritative_entry_count"] for row in section_counts
    )
    alternate_count = sum(row["alternate_entry_count"] for row in section_counts)
    slots = runtime_manifest.get("overlay_slots")
    if (
        authoritative_count != EXPECTED_AUTHORITATIVE_BODY_COUNT
        or alternate_count != EXPECTED_SUPPORT_THUNK_COUNT
        or len(entries) != authoritative_count + alternate_count
        or not isinstance(slots, list)
        or len(slots) != EXPECTED_OVERLAY_SLOT_COUNT
    ):
        raise NormalizationError("lookup semantic inventory is invalid")
    return canonical_bytes(
        {
            "schema_version": 1,
            "kind": "jfg-phase4-generated-lookup-inventory",
            "executable_section_count": EXPECTED_SECTION_COUNT,
            "overlay_slot_count": len(slots),
            "entry_count": len(entries),
            "authoritative_entry_count": authoritative_count,
            "alternate_entry_count": alternate_count,
            "sections": sections,
            "entries": entries,
        }
    )


def make_lifecycle_inventory(runtime_manifest: dict[str, object]) -> bytes:
    slots = runtime_manifest["overlay_slots"]
    sections = runtime_manifest["sections"]
    assert isinstance(slots, list) and isinstance(sections, list)
    rows = []
    for row in slots:
        assert isinstance(row, dict)
        populated = row["disposition"] == "populated"
        rows.append(
            {
                "slot_id": _slot_id(int(row["slot"])),
                "section_id": (
                    _section_id(int(row["section"])) if populated else None
                ),
                "disposition": (
                    "load-unload-reload" if populated else "empty-fail-closed"
                ),
            }
        )
    return canonical_bytes(
        {
            "schema_version": 1,
            "kind": "jfg-phase4-overlay-lifecycle-inventory",
            "executable_section_count": len(sections),
            "slot_count": len(rows),
            "populated_slot_count": sum(
                row["disposition"] == "load-unload-reload" for row in rows
            ),
            "empty_slot_count": sum(
                row["disposition"] == "empty-fail-closed" for row in rows
            ),
            "slots": rows,
        }
    )


def make_relocation_inventory(runtime_manifest: dict[str, object]) -> bytes:
    sections = runtime_manifest["sections"]
    relocations = runtime_manifest["r_mips_32"]
    audit = runtime_manifest["audit"]
    assert isinstance(sections, list) and isinstance(relocations, list)
    assert isinstance(audit, dict)
    counts = [0] * len(sections)
    for row in relocations:
        assert isinstance(row, dict)
        counts[int(row["source_section"])] += 1
    return canonical_bytes(
        {
            "schema_version": 1,
            "kind": "jfg-phase4-relocation-inventory",
            "executable_section_count": len(sections),
            "entry_count": len(relocations),
            "resolved_entry_count": len(relocations),
            "unresolved_entry_count": 0,
            "ledger_sha256": audit["r32_ledger_sha256"],
            "approval_set_sha256": audit["r32_approval_set_sha256"],
            "sections": [
                {"section_id": _section_id(index), "entry_count": count}
                for index, count in enumerate(counts)
            ],
        }
    )


def make_generated_inventory(
    authoritative: list[dict[str, int | str]],
    support: list[dict[str, int | str]],
    symbol_inventory_bytes: bytes,
    alternate_entry_ledger_bytes: bytes,
    covered_alias_ledger_bytes: bytes,
    manual_size_recovery_ledger_bytes: bytes,
) -> bytes:
    alternate = json.loads(alternate_entry_ledger_bytes)
    aliases = json.loads(covered_alias_ledger_bytes)
    recoveries = json.loads(manual_size_recovery_ledger_bytes)
    return canonical_bytes(
        {
            "schema_version": 1,
            "kind": "jfg-phase4-generated-inventory",
            "executable_section_count": EXPECTED_SECTION_COUNT,
            "expected_symbol_count": EXPECTED_EXECUTABLE_SYMBOL_COUNT,
            "authoritative_body_count": len(authoritative),
            "callable_wrapper_count": len(authoritative),
            "alternate_entry_thunk_count": len(support),
            "table_support_member_count": 11,
            "generated_game_function_stub_count": 0,
            "covered_alias_count": EXPECTED_COVERED_ALIAS_COUNT,
            "covered_alias_ledger_sha256": hashlib.sha256(
                covered_alias_ledger_bytes
            ).hexdigest(),
            "covered_alias_approval_set_sha256": aliases["approval_set_sha256"],
            "manual_size_recovery_count": EXPECTED_MANUAL_SIZE_RECOVERY_COUNT,
            "manual_size_recovery_ledger_sha256": hashlib.sha256(
                manual_size_recovery_ledger_bytes
            ).hexdigest(),
            "manual_size_recovery_approval_set_sha256": recoveries[
                "approval_set_sha256"
            ],
            "symbol_inventory_sha256": hashlib.sha256(
                symbol_inventory_bytes
            ).hexdigest(),
            "alternate_entry_ledger_sha256": hashlib.sha256(
                alternate_entry_ledger_bytes
            ).hexdigest(),
            "alternate_entry_approval_set_sha256": alternate[
                "approval_set_sha256"
            ],
        }
    )


def make_generation_result(
    runtime_manifest: dict[str, object],
    generated_inventory_bytes: bytes,
    alternate_entry_ledger_bytes: bytes,
    exception_ledger_bytes: bytes,
    covered_alias_ledger_bytes: bytes,
    manual_size_recovery_ledger_bytes: bytes,
    slot_inventory_bytes: bytes,
    lookup_inventory_bytes: bytes,
    lifecycle_inventory_bytes: bytes,
    relocation_inventory_bytes: bytes,
    decision_sidecar_bytes: bytes,
) -> bytes:
    audit = runtime_manifest["audit"]
    slots = runtime_manifest["overlay_slots"]
    sections = runtime_manifest["sections"]
    relocations = runtime_manifest["r_mips_32"]
    assert isinstance(audit, dict) and isinstance(slots, list)
    assert isinstance(sections, list) and isinstance(relocations, list)
    alternate = json.loads(alternate_entry_ledger_bytes)
    aliases = json.loads(covered_alias_ledger_bytes)
    recoveries = json.loads(manual_size_recovery_ledger_bytes)
    decisions = json.loads(decision_sidecar_bytes)
    decision_rows = decisions["decisions"]
    direct_rows = decisions["direct_calls"]
    unique_sites = {
        (row["source_section"], row["source_offset"]) for row in decision_rows
    }
    unique_direct_sites = {
        (row["source_section"], row["source_offset"]) for row in direct_rows
    }
    semantic_ledgers = runtime_manifest["semantic_ledgers"]
    assert isinstance(semantic_ledgers, dict)
    direct_candidates = semantic_ledgers["direct_call_candidate_ledger"]
    assert isinstance(direct_candidates, list)
    direct_approvals = []
    for row in direct_candidates:
        if row["target_class"] == "unresolved-candidate":
            category = "unresolved-runtime-address"
            disposition = "fail-closed-lookup-miss"
        elif row["disposition"] == "approved-fail-closed-trap":
            category = "anomalous-transform-target"
            disposition = "fail-closed-trap"
        else:
            continue
        direct_approvals.append(
            policy_approval_digest(
                "phase4-direct-call-disposition-policy-v1",
                str(row["opaque_id"]),
                category,
                disposition,
            )
        )
    populated = sum(
        isinstance(row, dict) and row.get("disposition") == "populated"
        for row in slots
    )
    empty = len(slots) - populated
    generation = {
        "schema_version": 1,
        "kind": "jfg-phase4-generation-semantic-result",
        "symbols": {
            "expected_count": EXPECTED_EXECUTABLE_SYMBOL_COUNT,
            "generated_count": EXPECTED_AUTHORITATIVE_BODY_COUNT,
            "replaceable_function_count": EXPECTED_AUTHORITATIVE_BODY_COUNT,
            "alternate_entry_thunk_count": EXPECTED_SUPPORT_THUNK_COUNT,
            "excluded_count": EXPECTED_COVERED_ALIAS_COUNT,
            "unclassified_count": 0,
            "duplicate_native_symbol_count": 0,
            "exclusion_category_counts": {
                "covered-alias": EXPECTED_COVERED_ALIAS_COUNT,
                "validated-non-code": 0,
                "runtime-abi": 0,
            },
            "inventory_sha256": hashlib.sha256(
                generated_inventory_bytes
            ).hexdigest(),
            "covered_alias_ledger_sha256": hashlib.sha256(
                covered_alias_ledger_bytes
            ).hexdigest(),
            "approval_set_sha256": aliases["approval_set_sha256"],
            "manual_size_recovery_count": EXPECTED_MANUAL_SIZE_RECOVERY_COUNT,
            "manual_size_recovery_ledger_sha256": hashlib.sha256(
                manual_size_recovery_ledger_bytes
            ).hexdigest(),
            "manual_size_recovery_approval_set_sha256": recoveries[
                "approval_set_sha256"
            ],
        },
        "calls": {
            "direct": {
                "candidate_count": audit["direct_call_candidate_count"],
                "expected_count": len(unique_direct_sites),
                "transfer_role_counts": {
                    "linked-call": audit["direct_linked_call_candidate_count"],
                    "direct-tail": audit["direct_tail_candidate_count"],
                },
                "instruction_class_counts": audit[
                    "direct_instruction_class_counts"
                ],
                "resolved_count": audit[
                    "direct_generated_target_candidate_count"
                ]
                - audit["r26_relocation_exception_count"],
                "approved_exception_count": len(direct_approvals),
                "unexplained_count": 0,
                "approved_exception_category_counts": {
                    "runtime-abi": 0,
                    "fail-closed-disposition": len(direct_approvals),
                },
                "ledger_sha256": canonical_digest(
                    {
                        "candidate_ledger_sha256": audit[
                            "direct_call_candidate_ledger_sha256"
                        ],
                        "decision_sidecar_sha256": hashlib.sha256(
                            decision_sidecar_bytes
                        ).hexdigest(),
                    }
                ),
                "approval_set_sha256": canonical_digest(
                    sorted(direct_approvals)
                ),
            },
            "indirect_ranges": {
                "expected_count": len(unique_sites),
                "resolved_count": len(unique_sites),
                "native_return_count": sum(
                    row["classification"] == "native-return"
                    for row in decision_by_unique_site(decision_rows)
                ),
                "decision_range_count": sum(
                    row["classification"] != "native-return"
                    for row in decision_by_unique_site(decision_rows)
                ),
                "approved_exception_count": 0,
                "unexplained_count": 0,
                "approved_exception_category_counts": {
                    "fail-closed-disposition": 0
                },
                "ledger_sha256": canonical_digest(
                    {
                        "discovery_ledger_sha256": audit[
                            "indirect_transfer_ledger_sha256"
                        ],
                        "decision_sidecar_sha256": hashlib.sha256(
                            decision_sidecar_bytes
                        ).hexdigest(),
                    }
                ),
                "approval_set_sha256": canonical_digest([]),
            },
        },
        "relocations": {
            "instructions": {
                "expected_count": audit["instruction_relocation_count"],
                "resolved_count": audit["instruction_resolved_count"],
                "approved_fail_closed_count": audit[
                    "instruction_fail_closed_count"
                ],
                "unexplained_count": 0,
                "hi_lo_pair_count": audit["hi_lo_pair_count"],
                "atomic_hi_lo_pair_count": audit["hi_lo_pair_count"],
                "partial_hi_lo_pair_count": 0,
                "ledger_sha256": audit[
                    "instruction_relocation_ledger_sha256"
                ],
                "approval_set_sha256": audit[
                    "instruction_relocation_approval_set_sha256"
                ],
            },
            "data_r32": {
                "expected_count": len(relocations),
                "resolved_count": len(relocations),
                "approved_fail_closed_count": 0,
                "unexplained_count": 0,
                "ledger_sha256": audit["r32_ledger_sha256"],
                "approval_set_sha256": audit["r32_approval_set_sha256"],
            },
        },
        "overlays": {
            "executable_section_count": len(sections),
            "expected_slot_count": len(slots),
            "populated_slot_count": populated,
            "empty_slot_count": empty,
            "listed_slot_count": len(slots),
            "lookup_table_slot_count": len(slots),
            "lifecycle_table_slot_count": len(slots),
            "generated_populated_module_count": populated,
            "relocation_table_entry_count": len(relocations),
            "resolved_relocation_entry_count": len(relocations),
            "unresolved_relocation_entry_count": 0,
            "missing_slot_count": 0,
            "duplicate_slot_count": 0,
            "manifest_sha256": canonical_digest(runtime_manifest),
            "slot_inventory_sha256": hashlib.sha256(slot_inventory_bytes).hexdigest(),
            "lookup_table_sha256": hashlib.sha256(
                lookup_inventory_bytes
            ).hexdigest(),
            "lifecycle_table_sha256": hashlib.sha256(
                lifecycle_inventory_bytes
            ).hexdigest(),
            "relocation_table_sha256": hashlib.sha256(
                relocation_inventory_bytes
            ).hexdigest(),
        },
        "stubs": {
            "policy_id": "zero-generated-game-stubs-v1",
            "generated_game_function_stub_count": 0,
            "unexplained_count": 0,
            "ledger_sha256": audit["stub_ledger_sha256"],
        },
        "semantic_bindings": {
            "alternate_entry_ledger_sha256": hashlib.sha256(
                alternate_entry_ledger_bytes
            ).hexdigest(),
            "covered_alias_ledger_sha256": hashlib.sha256(
                covered_alias_ledger_bytes
            ).hexdigest(),
            "manual_size_recovery_ledger_sha256": hashlib.sha256(
                manual_size_recovery_ledger_bytes
            ).hexdigest(),
            "exception_ledger_sha256": hashlib.sha256(
                exception_ledger_bytes
            ).hexdigest(),
            "n64recomp_decision_sidecar_sha256": hashlib.sha256(
                decision_sidecar_bytes
            ).hexdigest(),
        },
    }
    return canonical_bytes(generation)


def make_report_set(
    generation_result_bytes: bytes,
    generated_inventory_bytes: bytes,
    alternate_entry_ledger_bytes: bytes,
    exception_ledger_bytes: bytes,
    covered_alias_ledger_bytes: bytes,
    manual_size_recovery_ledger_bytes: bytes,
    slot_inventory_bytes: bytes,
    lookup_inventory_bytes: bytes,
    lifecycle_inventory_bytes: bytes,
    relocation_inventory_bytes: bytes,
    decision_sidecar_bytes: bytes,
) -> bytes:
    products = {
        "generation-result": generation_result_bytes,
        "generated-inventory": generated_inventory_bytes,
        "alternate-entry-ledger": alternate_entry_ledger_bytes,
        "exception-ledger": exception_ledger_bytes,
        "covered-alias-ledger": covered_alias_ledger_bytes,
        "manual-size-recovery-ledger": manual_size_recovery_ledger_bytes,
        "overlay-slot-inventory": slot_inventory_bytes,
        "overlay-lookup-table": lookup_inventory_bytes,
        "overlay-lifecycle-table": lifecycle_inventory_bytes,
        "relocation-table": relocation_inventory_bytes,
        "n64recomp-decision-sidecar": decision_sidecar_bytes,
    }
    return canonical_bytes(
        {
            "schema_version": 1,
            "kind": "jfg-phase4-generation-report-set",
            "reports": [
                {
                    "product_kind": kind,
                    "sha256": hashlib.sha256(payload).hexdigest(),
                }
                for kind, payload in sorted(products.items())
            ],
        }
    )


def make_header(authoritative: list[dict[str, int | str]], support: list[dict[str, int | str]]) -> str:
    lines = [
        "#ifndef JFG_PRIVATE_GENERATED_FUNCS_H",
        "#define JFG_PRIVATE_GENERATED_FUNCS_H",
        "",
        '#include "recomp.h"',
        "",
        "#ifdef __cplusplus",
        'extern "C" {',
        "#endif",
        "",
    ]
    for row in authoritative:
        name = str(row["name"])
        lines.append(f"void {name}(uint8_t* rdram, recomp_context* ctx);")
        lines.append(f"void {name}_recomp(uint8_t* rdram, recomp_context* ctx);")
    for row in support:
        name = str(row["name"])
        lines.append(f"void {name}(uint8_t* rdram, recomp_context* ctx);")
    lines.extend(
        [
            "",
            "typedef struct {",
            "    uint32_t rom_start;",
            "    uint32_t linked_vram;",
            "    uint32_t text_rom_offset;",
            "    uint32_t text_size;",
            "    uint32_t data_size;",
            "    uint32_t bss_size;",
            "    uint32_t is_overlay;",
            "} JfgGeneratedSectionMetadata;",
            "",
            "recomp_func_t* jfg_generated_lookup_function(int32_t vram);",
            "extern int jfg_generated_lookup_dirty;",
            "size_t jfg_generated_section_count(void);",
            "int jfg_generated_initialize_sections(int32_t* addresses, size_t capacity);",
            "int jfg_generated_section_metadata(uint32_t section, JfgGeneratedSectionMetadata* output);",
            "int jfg_generated_section_lifecycle(uint32_t operation, uint32_t section, int32_t base);",
            "int jfg_generated_overlay_slot_section(uint32_t slot, uint32_t* section);",
            "size_t jfg_generated_relocation_count(uint32_t source_section);",
            "int jfg_generated_relocation_sites(uint32_t source_section, const uint32_t** output, size_t* count);",
            "typedef struct { uint32_t site_offset; uint32_t target_section; uint32_t target_offset; } JfgGeneratedR32Descriptor;",
            "int jfg_generated_relocation_descriptors(uint32_t source_section, const JfgGeneratedR32Descriptor** output, size_t* count);",
            "int jfg_generated_apply_relocations_checked(uint8_t* rdram, size_t rdram_size, uint32_t source_section);",
            "void jfg_generated_apply_relocations(uint8_t* rdram, uint32_t source_section);",
            "typedef int (*jfg_generated_dispatch_call_callback_t)(void* opaque, int32_t vram, uint8_t* rdram, recomp_context* ctx);",
            "int jfg_minimal_runtime_bind_dispatch(jfg_generated_dispatch_call_callback_t callback, void* opaque);",
            "size_t jfg_minimal_runtime_section_capacity(void);",
            "int jfg_minimal_runtime_initialize(void);",
            "int jfg_minimal_runtime_unbind_dispatch(jfg_generated_dispatch_call_callback_t callback, void* opaque);",
            "int jfg_minimal_runtime_bind_cpu(int (*callback)(void*, void*, uint32_t, uint32_t, uint64_t*), void* opaque);",
            "int jfg_minimal_runtime_unbind_cpu(int (*callback)(void*, void*, uint32_t, uint32_t, uint64_t*), void* opaque);",
            "",
            "#ifdef __cplusplus",
            "}",
            "#endif",
            "",
            "#endif",
            "",
        ]
    )
    return "\n".join(lines)


def derive_continuation_entries(
    authoritative: list[dict[str, int | str]],
    runtime_manifest: dict[str, object],
) -> list[dict[str, int | str]]:
    """Return unique R_MIPS_32 targets that enter an authoritative body.

    N64 jump tables contain relocated code pointers which can name an
    instruction inside a recompiled function rather than its public entry.
    Keep these entries private to the normalized generated root: the public
    lookup still returns the owning wrapper, while the body consumes the
    section-relative continuation offset from the architectural zero-register
    slot and immediately restores that slot to zero.
    """
    relocations = runtime_manifest.get("r_mips_32")
    if not isinstance(relocations, list):
        raise NormalizationError("continuation inventory is invalid")
    by_section: dict[int, list[dict[str, int | str]]] = {}
    for row in authoritative:
        section = row.get("section")
        if type(section) is not int:
            raise NormalizationError("continuation inventory is invalid")
        by_section.setdefault(section, []).append(row)

    entries: dict[tuple[int, int], dict[str, int | str]] = {}
    for relocation in relocations:
        if not isinstance(relocation, dict):
            raise NormalizationError("continuation inventory is invalid")
        section = relocation.get("target_section")
        offset = relocation.get("target_section_offset")
        if type(section) is not int or type(offset) is not int:
            raise NormalizationError("continuation inventory is invalid")
        owners = [
            row
            for row in by_section.get(section, [])
            if int(row["offset"]) < offset < int(row["offset"]) + int(row["size"])
        ]
        if len(owners) > 1:
            raise NormalizationError("continuation target ownership is ambiguous")
        if not owners:
            continue
        owner = owners[0]
        entries[(section, offset)] = {
            "section": section,
            "offset": offset,
            "vram": int(owner["vram"]) + offset - int(owner["offset"]),
            "name": str(owner["name"]),
        }
    return [entries[key] for key in sorted(entries)]


def add_continuation_dispatch(
    definition: str,
    row: dict[str, int | str],
    continuations: list[dict[str, int | str]],
) -> str:
    owned = [entry for entry in continuations if entry["name"] == row["name"]]
    if not owned:
        return definition
    first_instruction = re.search(r"^[ \t]*// 0x[0-9A-F]{8}:", definition, re.MULTILINE)
    if first_instruction is None:
        raise NormalizationError("continuation body has no instruction boundary")

    cases: list[str] = []
    rewritten = definition
    for entry in owned:
        address = int(entry["vram"])
        offset = int(entry["offset"])
        existing_label = f"L_{address:08X}"
        if re.search(rf"^{re.escape(existing_label)}:$", rewritten, re.MULTILINE):
            label = existing_label
        else:
            marker = f"    // 0x{address:08X}:"
            if rewritten.count(marker) != 1:
                raise NormalizationError("continuation instruction boundary is ambiguous")
            label = f"JFG_CONT_{address:08X}"
            rewritten = rewritten.replace(marker, f"{label}:\n{marker}", 1)
        cases.append(f"    case UINT32_C(0x{offset:08X}): goto {label};")

    dispatch = (
        "    const uint32_t jfg_continuation_entry = (uint32_t)ctx->r0;\n"
        "    ctx->r0 = 0;\n"
        "    switch (jfg_continuation_entry) {\n"
        + "\n".join(cases)
        + "\n    default: break;\n"
        "    }\n"
    )
    insertion = first_instruction.start()
    return rewritten[:insertion] + dispatch + rewritten[insertion:]


def make_lookup(
    rows: list[dict[str, int | str]],
    continuations: list[dict[str, int | str]] | None = None,
) -> str:
    continuations = [] if continuations is None else continuations
    declarations = "\n".join(
        f"extern void {row['name']}(uint8_t*, recomp_context*);" for row in rows
    )
    exact_entries = [
        f"    {{{row['section']}u, UINT32_C(0x{int(row['offset']):08X}), {row['name']}}},"
        for row in rows
    ]
    continuation_entries = [
        f"    {{{row['section']}u, UINT32_C(0x{int(row['offset']):08X}), {row['name']}}},"
        for row in continuations
    ]
    entries = "\n".join(exact_entries + continuation_entries)
    entry_count = len(exact_entries) + len(continuation_entries)
    lookup_capacity = 1
    while lookup_capacity < max(2, entry_count * 2):
        lookup_capacity *= 2
    return f'''#include "funcs.h"

#include <stddef.h>

{declarations}

typedef struct {{ uint32_t section; uint32_t offset; recomp_func_t* function; }} JfgLookupEntry;
typedef struct {{ uint32_t address; recomp_func_t* function; }} JfgLookupSlot;

static const JfgLookupEntry kLookupEntries[] = {{
{entries}
}};

static JfgLookupSlot kLookupSlots[{lookup_capacity}u];
int jfg_generated_lookup_dirty = 1;

static void jfg_generated_lookup_rebuild(void) {{
    const size_t capacity = sizeof(kLookupSlots) / sizeof(kLookupSlots[0]);
    for (size_t index = 0; index < capacity; ++index) {{
        kLookupSlots[index].function = NULL;
    }}
    for (size_t index = 0;
         index < sizeof(kLookupEntries) / sizeof(kLookupEntries[0]);
         ++index) {{
        const JfgLookupEntry* entry = &kLookupEntries[index];
        const uint32_t base = (uint32_t)section_addresses[entry->section];
        if (base == 0u) {{
            continue;
        }}
        const uint32_t address = base + entry->offset;
        size_t slot = ((size_t)(address * UINT32_C(2654435761))) &
                      (capacity - 1u);
        while (kLookupSlots[slot].function != NULL &&
               kLookupSlots[slot].address != address) {{
            slot = (slot + 1u) & (capacity - 1u);
        }}
        if (kLookupSlots[slot].function == NULL) {{
            kLookupSlots[slot].address = address;
            kLookupSlots[slot].function = entry->function;
        }}
    }}
    jfg_generated_lookup_dirty = 0;
}}

recomp_func_t* jfg_generated_lookup_function(int32_t vram) {{
    const size_t capacity = sizeof(kLookupSlots) / sizeof(kLookupSlots[0]);
    const uint32_t address = (uint32_t)vram;
    if (jfg_generated_lookup_dirty) {{
        jfg_generated_lookup_rebuild();
    }}
    size_t slot = ((size_t)(address * UINT32_C(2654435761))) &
                  (capacity - 1u);
    while (kLookupSlots[slot].function != NULL) {{
        if (kLookupSlots[slot].address == address) {{
            return kLookupSlots[slot].function;
        }}
        slot = (slot + 1u) & (capacity - 1u);
    }}
    return NULL;
}}
'''


def make_section_count(section_bases: list[int]) -> str:
    return f'''#include "funcs.h"

size_t jfg_generated_section_count(void) {{
    return {len(section_bases)}u;
}}
'''


def make_section_initializer(initial_bases: list[int]) -> str:
    bases = "\n".join(f"    INT32_C(0x{base:08X})," for base in initial_bases)
    return f'''#include "funcs.h"

#include <stddef.h>

static const int32_t kLinkedSectionBases[] = {{
{bases}
}};

int jfg_generated_initialize_sections(int32_t* addresses, size_t capacity) {{
    const size_t required = sizeof(kLinkedSectionBases) / sizeof(kLinkedSectionBases[0]);
    if (addresses == NULL || capacity < required) {{
        return 0;
    }}
    for (size_t index = 0; index < required; ++index) {{
        addresses[index] = kLinkedSectionBases[index];
    }}
    /* Publish the supplied storage only after its complete initialization. */
    section_addresses = addresses;
    jfg_generated_lookup_dirty = 1;
    return 1;
}}
'''


def make_section_metadata(runtime_manifest: dict[str, object]) -> str:
    sections = runtime_manifest["sections"]
    if not isinstance(sections, list):
        raise NormalizationError("runtime manifest is invalid")
    metadata = "\n".join(
        "    {UINT32_C(0x%08X), UINT32_C(0x%08X), UINT32_C(0x%08X), "
        "UINT32_C(0x%08X), UINT32_C(0x%08X), UINT32_C(0x%08X), %du},"
        % (
            int(section["rom"]),
            int(section["linked_vram"]),
            int(section["text_offset"]),
            int(section["text_offset"]) + int(section["text_size"]),
            int(section["data_size"]),
            int(section["bss_size"]),
            1 if section["kind"] == "overlay" else 0,
        )
        for section in sections
    )
    return f'''#include "funcs.h"

#include <stddef.h>

static const JfgGeneratedSectionMetadata kSectionMetadata[] = {{
{metadata}
}};

int jfg_generated_section_metadata(
    uint32_t section,
    JfgGeneratedSectionMetadata* output
) {{
    if (output == NULL ||
        section >= sizeof(kSectionMetadata) / sizeof(kSectionMetadata[0])) {{
        return 0;
    }}
    *output = kSectionMetadata[section];
    return 1;
}}
'''


def make_lifecycle() -> str:
    return '''#include "funcs.h"

int jfg_generated_section_lifecycle(uint32_t operation, uint32_t section, int32_t base) {{
    const uint32_t section_count = (uint32_t)jfg_generated_section_count();
    if (operation == 0u) {{
        return jfg_generated_initialize_sections(section_addresses, section_count) != 0 ? 0 : -1;
    }}
    if (section >= section_count) {{
        return -1;
    }}
    if (operation == 1u) {{
        section_addresses[section] = base;
        jfg_generated_lookup_dirty = 1;
        return 0;
    }}
    if (operation == 2u) {{
        section_addresses[section] = 0;
        jfg_generated_lookup_dirty = 1;
        return 0;
    }}
    return -1;
}}
'''.replace("{{", "{").replace("}}", "}")


def make_overlay_slot_table(runtime_manifest: dict[str, object]) -> str:
    slots = runtime_manifest.get("overlay_slots")
    if not isinstance(slots, list):
        raise NormalizationError("runtime manifest is invalid")
    rows = "\n".join(
        "    {%du, %du}," % (
            1 if row.get("disposition") == "populated" else 0,
            int(row["section"]) if row.get("section") is not None else 0,
        )
        for row in slots if isinstance(row, dict)
    )
    return f'''#include "funcs.h"

typedef struct {{ uint32_t populated; uint32_t section; }} JfgOverlaySlot;
static const JfgOverlaySlot kOverlaySlots[] = {{
{rows}
}};
int jfg_generated_overlay_slot_section(uint32_t slot, uint32_t* section) {{
    if (section == NULL || slot == 0u || slot > sizeof(kOverlaySlots) / sizeof(kOverlaySlots[0])) return 0;
    const JfgOverlaySlot value = kOverlaySlots[slot - 1u];
    if (value.populated == 0u) return 0;
    *section = value.section;
    return 1;
}}
'''


def make_relocation_entries(runtime_manifest: dict[str, object]) -> str:
    rows = runtime_manifest["r_mips_32"]
    if not isinstance(rows, list):
        raise NormalizationError("runtime manifest is invalid")
    return "\n".join(
        "    {%du, UINT32_C(0x%08X), %du, UINT32_C(0x%08X)},"
        % (
            int(row["source_section"]),
            int(row["site_offset"]),
            int(row["target_section"]),
            int(row["target_section_offset"]),
        )
        for row in rows
    )


def make_relocation_count(runtime_manifest: dict[str, object]) -> str:
    if not isinstance(runtime_manifest.get("r_mips_32"), list):
        raise NormalizationError("runtime manifest is invalid")
    return '''#include "funcs.h"

#include <stddef.h>

size_t jfg_generated_relocation_count(uint32_t source_section) {
    const uint32_t* sites = NULL;
    size_t count = 0u;
    if (!jfg_generated_relocation_sites(source_section, &sites, &count)) {
        return 0u;
    }
    (void)sites;
    return count;
}
'''


def make_relocation_sites(runtime_manifest: dict[str, object]) -> str:
    rows = runtime_manifest["r_mips_32"]
    if not isinstance(rows, list):
        raise NormalizationError("runtime manifest is invalid")
    sections = runtime_manifest["sections"]
    if not isinstance(sections, list):
        raise NormalizationError("runtime manifest is invalid")
    sites_by_section: list[list[int]] = [[] for _ in sections]
    for row in rows:
        if not isinstance(row, dict):
            raise NormalizationError("runtime manifest is invalid")
        sites_by_section[int(row["source_section"])].append(int(row["site_offset"]))
    # The manifest is validated before this generator is called.  Sorting keeps
    # the private ABI deterministic without exposing its generated contents in
    # any tracked source.
    for sites in sites_by_section:
        sites.sort()
    flattened_sites = [site for sites in sites_by_section for site in sites]
    site_values = "\n".join(f"    UINT32_C(0x{site:08X})," for site in flattened_sites)
    ranges: list[str] = []
    begin = 0
    for sites in sites_by_section:
        ranges.append(f"    {{{begin}u, {len(sites)}u}},")
        begin += len(sites)
    return f'''#include "funcs.h"

#include <stddef.h>

typedef struct {{ size_t begin; size_t count; }} JfgR32SiteRange;

static const uint32_t kR32SiteOffsets[] = {{
{site_values}
}};

static const JfgR32SiteRange kR32SiteRanges[] = {{
{chr(10).join(ranges)}
}};

int jfg_generated_relocation_sites(
    uint32_t source_section,
    const uint32_t** output,
    size_t* count
) {{
    if (output == NULL || count == NULL ||
        source_section >= sizeof(kR32SiteRanges) / sizeof(kR32SiteRanges[0])) {{
        return 0;
    }}
    const JfgR32SiteRange range = kR32SiteRanges[source_section];
    *count = range.count;
    *output = range.count == 0u ? NULL : kR32SiteOffsets + range.begin;
    return 1;
}}
'''


def make_relocation_descriptors(runtime_manifest: dict[str, object]) -> str:
    """Emit the private, complete R_MIPS_32 target formula inventory.

    This is deliberately a separate support object from the site accessor so
    the generated-object audit has one exported support symbol per object.
    The data stays in the ignored generated root; tracked code only knows the
    ABI shape and validates it against the relocation writer at run time.
    """
    rows = runtime_manifest["r_mips_32"]
    sections = runtime_manifest["sections"]
    if not isinstance(rows, list) or not isinstance(sections, list):
        raise NormalizationError("runtime manifest is invalid")
    descriptors_by_section: list[list[tuple[int, int, int]]] = [
        [] for _ in sections
    ]
    for row in rows:
        if not isinstance(row, dict):
            raise NormalizationError("runtime manifest is invalid")
        descriptors_by_section[int(row["source_section"])].append(
            (
                int(row["site_offset"]),
                int(row["target_section"]),
                int(row["target_section_offset"]),
            )
        )
    for descriptors in descriptors_by_section:
        descriptors.sort()
    flattened = [
        descriptor
        for descriptors in descriptors_by_section
        for descriptor in descriptors
    ]
    values = "\n".join(
        "    {UINT32_C(0x%08X), %du, UINT32_C(0x%08X)}," % descriptor
        for descriptor in flattened
    )
    ranges: list[str] = []
    begin = 0
    for descriptors in descriptors_by_section:
        ranges.append(f"    {{{begin}u, {len(descriptors)}u}},")
        begin += len(descriptors)
    return f'''#include "funcs.h"

#include <stddef.h>

typedef struct {{ size_t begin; size_t count; }} JfgR32DescriptorRange;

static const JfgGeneratedR32Descriptor kR32Descriptors[] = {{
{values}
}};

static const JfgR32DescriptorRange kR32DescriptorRanges[] = {{
{chr(10).join(ranges)}
}};

int jfg_generated_relocation_descriptors(
    uint32_t source_section,
    const JfgGeneratedR32Descriptor** output,
    size_t* count
) {{
    if (output == NULL || count == NULL ||
        source_section >= sizeof(kR32DescriptorRanges) / sizeof(kR32DescriptorRanges[0])) {{
        return 0;
    }}
    const JfgR32DescriptorRange range = kR32DescriptorRanges[source_section];
    *count = range.count;
    *output = range.count == 0u ? NULL : kR32Descriptors + range.begin;
    return 1;
}}
'''


def make_relocations_checked(runtime_manifest: dict[str, object]) -> str:
    entries = make_relocation_entries(runtime_manifest)
    return f'''#include "funcs.h"

#include <stddef.h>
typedef struct {{ uint32_t source_section; uint32_t site_offset; uint32_t target_section; uint32_t target_offset; }} JfgR32Entry;

static const JfgR32Entry kR32Entries[] = {{
{entries}
}};

int jfg_generated_apply_relocations_checked(
    uint8_t* rdram,
    size_t rdram_size,
    uint32_t source_section
) {{
    const uint32_t section_count = (uint32_t)jfg_generated_section_count();
    if (rdram == NULL || source_section >= section_count) {{
        return 0;
    }}
    for (size_t index = 0; index < sizeof(kR32Entries) / sizeof(kR32Entries[0]); ++index) {{
        const JfgR32Entry* entry = &kR32Entries[index];
        if (entry->source_section == source_section) {{
            if (entry->target_section >= section_count) {{
                return 0;
            }}
            const uint32_t source_base = (uint32_t)section_addresses[entry->source_section];
            const uint32_t target_base = (uint32_t)section_addresses[entry->target_section];
            const uint64_t site = (uint64_t)source_base + entry->site_offset;
            const uint64_t target = (uint64_t)target_base + entry->target_offset;
            if (source_base < UINT32_C(0x80000000) ||
                target_base < UINT32_C(0x80000000) ||
                site > UINT32_MAX || target > UINT32_MAX ||
                site - UINT32_C(0x80000000) > rdram_size ||
                rdram_size - (size_t)(site - UINT32_C(0x80000000)) < sizeof(uint32_t)) {{
                return 0;
            }}
        }}
    }}
    for (size_t index = 0; index < sizeof(kR32Entries) / sizeof(kR32Entries[0]); ++index) {{
        const JfgR32Entry* entry = &kR32Entries[index];
        if (entry->source_section == source_section) {{
            const uint32_t site = (uint32_t)section_addresses[entry->source_section] + entry->site_offset;
            const uint32_t target = (uint32_t)section_addresses[entry->target_section] + entry->target_offset;
            const size_t offset = (size_t)(site - UINT32_C(0x80000000));
            /* RDRAM is guest big-endian regardless of the host byte order. */
            rdram[offset + 0u] = (uint8_t)(target >> 24);
            rdram[offset + 1u] = (uint8_t)(target >> 16);
            rdram[offset + 2u] = (uint8_t)(target >> 8);
            rdram[offset + 3u] = (uint8_t)target;
        }}
    }}
    return 1;
}}
'''


def make_relocations() -> str:
    return '''#include "funcs.h"

#include <stddef.h>
#include <stdlib.h>

void jfg_generated_apply_relocations(uint8_t* rdram, uint32_t source_section) {
    if (jfg_generated_apply_relocations_checked(
            rdram,
            (size_t)UINT32_C(0x00800000),
            source_section) == 0) {
        abort();
    }
}
'''


def make_registry(
    authoritative: list[dict[str, int | str]],
    support: list[dict[str, int | str]],
    section_count_value: int,
) -> str:
    declarations: list[str] = []
    references: list[str] = []
    for row in authoritative:
        name = str(row["name"])
        declarations.extend(
            [
                f'extern "C" void {name}(uint8_t*, recomp_context*);',
                f'extern "C" void {name}_recomp(uint8_t*, recomp_context*);',
            ]
        )
        references.extend([f"    g_function_sink = {name};", f"    g_function_sink = {name}_recomp;"])
    for row in support:
        name = str(row["name"])
        declarations.append(f'extern "C" void {name}(uint8_t*, recomp_context*);')
        references.append(f"    g_function_sink = {name};")
    first = authoritative[0]
    return '''#include "funcs.h"

#include <cstdint>

''' + "\n".join(declarations) + '''

namespace {
recomp_func_t* volatile g_function_sink = nullptr;
int* volatile g_data_sink = nullptr;
volatile bool g_exercise_fail_closed = false;

int checked_dispatch(
    void*, const int32_t vram, uint8_t* rdram, recomp_context* context) {
    return rdram != nullptr && context != nullptr &&
        jfg_generated_lookup_function(vram) != nullptr ? 1 : 0;
}
}

extern "C" int jfg_generated_link_smoke(void) {
''' + "\n".join(references) + f'''
    g_data_sink = &jfg_generated_lookup_dirty;
    const size_t section_count = jfg_generated_section_count();
    int32_t boundary_probe[{section_count_value}] = {{INT32_C(0x13579BDF)}};
    if (section_count != {section_count_value}u ||
        jfg_generated_initialize_sections(boundary_probe, section_count - 1u) != 0 ||
        boundary_probe[0] != INT32_C(0x13579BDF) ||
        jfg_generated_initialize_sections(section_addresses, section_count) == 0 ||
        jfg_generated_section_lifecycle(0u, 0u, 0) != 0) {{
        return 1;
    }}
    if (jfg_generated_lookup_function(INT32_C(0x{int(first['vram']):08X})) != {first['name']}) {{
        return 2;
    }}
    auto* volatile relocation_reference = &jfg_generated_apply_relocations;
    auto* volatile checked_relocation_reference = &jfg_generated_apply_relocations_checked;
    auto* volatile relocation_count_reference = &jfg_generated_relocation_count;
    auto* volatile relocation_sites_reference = &jfg_generated_relocation_sites;
    auto* volatile relocation_descriptors_reference = &jfg_generated_relocation_descriptors;
    auto* volatile lifecycle_reference = &jfg_generated_section_lifecycle;
    auto* volatile overlay_slot_reference = &jfg_generated_overlay_slot_section;
    auto* volatile metadata_reference = &jfg_generated_section_metadata;
    auto* volatile count_reference = &jfg_generated_section_count;
    auto* volatile initialize_reference = &jfg_generated_initialize_sections;
    (void)relocation_reference;
    (void)checked_relocation_reference;
    (void)relocation_count_reference;
    (void)relocation_sites_reference;
    (void)relocation_descriptors_reference;
    (void)lifecycle_reference;
    (void)overlay_slot_reference;
    (void)metadata_reference;
    (void)count_reference;
    (void)initialize_reference;
    recomp_context context{{}};
    cop0_status_write(&context, UINT64_C(5));
    uint8_t dispatch_memory[1] = {{0}};
    if (jfg_minimal_runtime_section_capacity() < section_count ||
        jfg_minimal_runtime_initialize() == 0 ||
        cop0_status_read(&context) != UINT64_C(5) ||
        jfg_minimal_runtime_bind_dispatch(checked_dispatch, nullptr) == 0) {{
        return 3;
    }}
    get_function(INT32_C(0x{int(first['vram']):08X}))(dispatch_memory, &context);
    if (jfg_minimal_runtime_unbind_dispatch(checked_dispatch, nullptr) == 0) {{
        return 3;
    }}
    if (jfg_minimal_runtime_bind_cpu(nullptr, nullptr) != 0 ||
        jfg_minimal_runtime_unbind_cpu(nullptr, nullptr) != 0) {{
        return 3;
    }}
    if (g_exercise_fail_closed) {{
        uint8_t memory[1] = {{0}};
        cop0_write(&context, 0u, 0u);
        (void)cop0_read(&context, 0u);
        cop0_eret(memory, &context);
        cache_op(memory, &context, 0u, 0u);
        cop0_tlb_op(&context, 0u);
        reserved_instruction(memory, &context, 0u, 0u);
        switch_error("generated", 0u, 0u);
        do_break(0u);
        recomp_syscall_handler(memory, &context, 0);
        pause_self(memory);
    }}
    return g_function_sink == nullptr || g_data_sink == nullptr ? 4 : 0;
}}
'''


def symbol_digest(symbols: list[str]) -> str:
    normalized = "".join(f"{symbol}\n" for symbol in sorted(symbols))
    return hashlib.sha256(normalized.encode("utf-8")).hexdigest()


def normalize_recomp_context_typedef(header: str) -> str:
    """Give the one generated context typedef a C/C++-compatible tag.

    The ignored upstream header is otherwise retained verbatim.  Accepting a
    broader C declaration grammar here could rewrite an unrelated type, so the
    two exact supported forms are intentionally the entire contract.
    """
    anonymous = list(RECOMP_CONTEXT_ANONYMOUS_RE.finditer(header))
    tagged = list(RECOMP_CONTEXT_TAGGED_RE.finditer(header))
    if len(anonymous) == 1 and not tagged:
        match = anonymous[0]
        return (
            header[: match.start()]
            + match.group("prefix")
            + " recomp_context"
            + match.group("gap")
            + match.group("open")
            + match.group("body")
            + "}"
            + match.group("suffix")
            + header[match.end() :]
        )
    if not anonymous and len(tagged) == 1:
        return header
    raise NormalizationError("recompiler context typedef is invalid")


def validate_recomp_header_lineage(
    header: str,
    raw_functions: dict[str, str],
) -> None:
    """Reject incompatible generator/header pairs before emitting output."""
    header_code = C_NON_CODE_RE.sub(" ", header)
    definitions = [
        (match.group("body") or "").strip()
        for match in RECOMP_FUNC_DEFINITION_RE.finditer(header_code)
    ]
    if not definitions:
        raise NormalizationError("recompiler header lineage is invalid")
    for definition in definitions:
        if (
            re.search(r"\b(?:extern|inline|weak)\b", definition) is not None
            or re.search(r"\b(?:noinline|noipa)\b", definition) is None
        ):
            raise NormalizationError("recompiler header lineage is invalid")

    raw_code = C_NON_CODE_RE.sub(" ", "\n".join(raw_functions.values()))
    for symbol in RAW_HEADER_CALL_SYMBOLS:
        call = re.compile(rf"\b{re.escape(symbol)}[ \t\r\n]*\(")
        if call.search(raw_code) is not None and call.search(header_code) is None:
            raise NormalizationError("recompiler header lineage is invalid")


def normalize(args: argparse.Namespace) -> dict[str, int]:
    raw_functions = extract_functions(args.raw_generated)
    rows, section_bases = load_function_inventory(args.symbols)
    original = authoritative_coordinates(args.original_context)
    authoritative = [row for row in rows if (int(row["vram"]), int(row["size"])) in original]
    support = [row for row in rows if (int(row["vram"]), int(row["size"])) not in original]
    generated_authoritative_coordinates = {
        (int(row["vram"]), int(row["size"])) for row in authoritative
    }
    if (
        len(authoritative) != EXPECTED_AUTHORITATIVE_BODY_COUNT
        or len(support) != EXPECTED_SUPPORT_THUNK_COUNT
        or original != generated_authoritative_coordinates
        or len(rows) != len(raw_functions)
        or len(section_bases) != EXPECTED_SECTION_COUNT
    ):
        raise NormalizationError("private generated function denominators changed")
    if set(raw_functions) != {str(row["name"]) for row in rows}:
        raise NormalizationError("symbol and generated-source inventories differ")

    runtime_manifest, initial_bases = validate_runtime_manifest(
        _load_json(args.runtime_manifest),
        section_bases,
    )
    validate_function_inventory_extents(rows, runtime_manifest)
    continuations = derive_continuation_entries(authoritative, runtime_manifest)
    (
        _decision_sidecar,
        decision_sidecar_bytes,
        decision_classification_counts,
    ) = load_n64recomp_decision_sidecar(
        args.raw_generated,
        raw_functions,
        rows,
        runtime_manifest,
    )
    cpu_section_inventory_bytes = make_cpu_section_inventory(
        rows, original, runtime_manifest
    )
    recomp_header = normalize_recomp_context_typedef(
        _read_text(args.recomp_header).replace("\r\n", "\n")
    )
    if "\r" in recomp_header or "#define __RECOMP_H__" not in recomp_header:
        raise NormalizationError("recompiler header is invalid")
    validate_recomp_header_lineage(recomp_header, raw_functions)

    output = _require_new_private_output(args.output)
    output.mkdir()
    for directory in ("baseline", "support", "runtime", "audit", "patch", "include"):
        destination = output / directory
        destination.mkdir(parents=True)

    write_text(output / "include" / "recomp.h", recomp_header)
    write_text(output / "funcs.h", make_header(authoritative, support))

    baseline_sources: list[str] = []
    for row in authoritative:
        name = str(row["name"])
        definition = raw_functions[name].replace(
            f"RECOMP_FUNC void {name}(", f"RECOMP_FUNC void {name}_recomp(", 1
        )
        definition = add_continuation_dispatch(
            definition, row, continuations
        )
        body_path = output / "baseline" / f"{name}_recomp.c"
        wrapper_path = output / "baseline" / f"{name}.c"
        write_text(body_path, '#include "recomp.h"\n#include "funcs.h"\n\n' + definition)
        write_text(
            wrapper_path,
            '#include "recomp.h"\n#include "funcs.h"\n\n'
            f"RECOMP_FUNC void {name}(uint8_t* rdram, recomp_context* ctx) {{\n"
            f"    {name}_recomp(rdram, ctx);\n"
            "}\n",
        )
        baseline_sources.extend(
            [body_path.relative_to(output).as_posix(), wrapper_path.relative_to(output).as_posix()]
        )

    for row in support:
        name = str(row["name"])
        support_path = output / "support" / f"{name}.c"
        write_text(
            support_path,
            '#include "recomp.h"\n#include "funcs.h"\n\n' + raw_functions[name],
        )
        baseline_sources.append(support_path.relative_to(output).as_posix())

    support_sources = [
        ("support/lookup_table.c", make_lookup(rows, continuations)),
        ("support/section_count.c", make_section_count(section_bases)),
        ("support/section_initializer.c", make_section_initializer(initial_bases)),
        ("support/section_metadata.c", make_section_metadata(runtime_manifest)),
        ("support/lifecycle_table.c", make_lifecycle()),
        ("support/overlay_slot_table.c", make_overlay_slot_table(runtime_manifest)),
        ("support/relocation_table.c", make_relocations()),
        (
            "support/relocation_checked.c",
            make_relocations_checked(runtime_manifest),
        ),
        ("support/relocation_count.c", make_relocation_count(runtime_manifest)),
        ("support/relocation_sites.c", make_relocation_sites(runtime_manifest)),
        (
            "support/relocation_descriptors.c",
            make_relocation_descriptors(runtime_manifest),
        ),
    ]
    for relative, content in support_sources:
        write_text(output / relative, content)
        baseline_sources.append(relative)

    write_text(
        output / "audit" / "link_smoke_registry.cpp",
        make_registry(authoritative, support, len(section_bases)),
    )

    body_sources = sorted(
        f"baseline/{row['name']}_recomp.c" for row in authoritative
    )
    wrapper_sources = sorted(f"baseline/{row['name']}.c" for row in authoritative)
    alternate_entry_thunk_sources = sorted(f"support/{row['name']}.c" for row in support)
    table_support_source_paths = sorted(relative for relative, _ in support_sources)
    support_source_paths = sorted(alternate_entry_thunk_sources + table_support_source_paths)
    body_symbols = sorted(f"{row['name']}_recomp" for row in authoritative)
    normal_symbols = sorted(str(row["name"]) for row in authoritative)
    section_address_support_symbols = sorted(
        [
            "jfg_generated_initialize_sections",
            "jfg_generated_section_count",
            "jfg_generated_section_metadata",
        ]
    )
    lookup_support_symbols = sorted(
        [
            "jfg_generated_lookup_function",
            "jfg_generated_overlay_slot_section",
        ]
    )
    lifecycle_support_symbols = ["jfg_generated_section_lifecycle"]
    relocation_support_symbols = sorted(
        [
            "jfg_generated_apply_relocations",
            "jfg_generated_apply_relocations_checked",
            "jfg_generated_relocation_count",
            "jfg_generated_relocation_sites",
            "jfg_generated_relocation_descriptors",
        ]
    )
    support_symbols = sorted(
        [str(row["name"]) for row in support]
        + section_address_support_symbols
        + lookup_support_symbols
        + lifecycle_support_symbols
        + relocation_support_symbols
    )
    alternate_entry_thunk_symbols = sorted(str(row["name"]) for row in support)
    table_support_symbols = sorted(set(support_symbols) - set(alternate_entry_thunk_symbols))
    support_data_symbols = ["jfg_generated_lookup_dirty"]
    coverage = sorted(
        set(
            body_symbols
            + normal_symbols
            + support_symbols
            + support_data_symbols
            + RUNTIME_BRIDGE_SYMBOLS
            + ["section_addresses"]
        )
    )
    symbol_inventory = {
        "version": 2,
        "section_count": len(section_bases),
        "baseline_body_symbols": body_symbols,
        "baseline_body_data_symbols": [],
        "normal_callable_symbols": normal_symbols,
        "normal_wrapper_data_symbols": [],
        "patch_symbols": [],
        "patch_data_symbols": [],
        "support_lifecycle_symbols": support_symbols,
        "alternate_entry_thunk_symbols": alternate_entry_thunk_symbols,
        "table_support_symbols": table_support_symbols,
        "section_address_support_symbols": section_address_support_symbols,
        "lookup_support_symbols": lookup_support_symbols,
        "lifecycle_support_symbols": lifecycle_support_symbols,
        "relocation_support_symbols": relocation_support_symbols,
        "support_data_symbols": support_data_symbols,
        "runtime_bridge_symbols": RUNTIME_BRIDGE_SYMBOLS,
        "runtime_data_symbols": ["section_addresses"],
        "link_smoke_entry_symbols": ["jfg_generated_link_smoke"],
        "coverage_symbol_count": len(coverage),
        "coverage_sha256": symbol_digest(coverage),
    }
    inventory_bytes = canonical_bytes(symbol_inventory)
    (output / "symbol_inventory.json").write_bytes(inventory_bytes)
    (output / "cpu_section_inventory.json").write_bytes(cpu_section_inventory_bytes)

    alternate_entry_ledger_bytes = make_alternate_entry_ledger(
        support, runtime_manifest
    )
    exception_ledger_bytes = make_exception_ledger(runtime_manifest)
    covered_alias_ledger_bytes = make_covered_alias_ledger(runtime_manifest)
    manual_size_recovery_ledger_bytes = make_manual_size_recovery_ledger(
        runtime_manifest
    )
    slot_inventory_bytes = make_overlay_slot_inventory(runtime_manifest)
    lookup_inventory_bytes = make_lookup_inventory(rows, original, runtime_manifest)
    lifecycle_inventory_bytes = make_lifecycle_inventory(runtime_manifest)
    relocation_inventory_bytes = make_relocation_inventory(runtime_manifest)
    generated_inventory_bytes = make_generated_inventory(
        authoritative,
        support,
        inventory_bytes,
        alternate_entry_ledger_bytes,
        covered_alias_ledger_bytes,
        manual_size_recovery_ledger_bytes,
    )
    generation_result_bytes = make_generation_result(
        runtime_manifest,
        generated_inventory_bytes,
        alternate_entry_ledger_bytes,
        exception_ledger_bytes,
        covered_alias_ledger_bytes,
        manual_size_recovery_ledger_bytes,
        slot_inventory_bytes,
        lookup_inventory_bytes,
        lifecycle_inventory_bytes,
        relocation_inventory_bytes,
        decision_sidecar_bytes,
    )
    report_set_bytes = make_report_set(
        generation_result_bytes,
        generated_inventory_bytes,
        alternate_entry_ledger_bytes,
        exception_ledger_bytes,
        covered_alias_ledger_bytes,
        manual_size_recovery_ledger_bytes,
        slot_inventory_bytes,
        lookup_inventory_bytes,
        lifecycle_inventory_bytes,
        relocation_inventory_bytes,
        decision_sidecar_bytes,
    )
    semantic_products = {
        "alternate_entry_ledger.json": alternate_entry_ledger_bytes,
        "covered_alias_ledger.json": covered_alias_ledger_bytes,
        "exception_ledger.json": exception_ledger_bytes,
        "generated_inventory.json": generated_inventory_bytes,
        "generation_result.json": generation_result_bytes,
        "manual_size_recovery_ledger.json": manual_size_recovery_ledger_bytes,
        "n64recomp_decision_sidecar.json": decision_sidecar_bytes,
        "overlay_slot_inventory.json": slot_inventory_bytes,
        "overlay_lookup_table.json": lookup_inventory_bytes,
        "overlay_lifecycle_table.json": lifecycle_inventory_bytes,
        "relocation_table.json": relocation_inventory_bytes,
        "report_set.json": report_set_bytes,
    }
    for relative, payload in semantic_products.items():
        (output / relative).write_bytes(payload)

    try:
        normalizer_bytes = NORMALIZER_PATH.read_bytes()
    except OSError as error:
        raise NormalizationError("normalizer revision is unavailable") from error
    # This digest is a stale-root guard: it binds the output to the exact
    # normalizer revision that emitted it. It does not authenticate the output
    # or attest that the generated sources are trustworthy.
    manifest = {
        "version": 6,
        "n64recomp_include": "include",
        "baseline_body_sources": body_sources,
        "normal_wrapper_sources": wrapper_sources,
        "support_sources": support_source_paths,
        "alternate_entry_thunk_sources": alternate_entry_thunk_sources,
        "table_support_sources": table_support_source_paths,
        "patch_sources": [],
        "game_patch_function_count": 0,
        "link_smoke_sources": ["audit/link_smoke_registry.cpp"],
        "symbol_inventory": "symbol_inventory.json",
        "symbol_inventory_sha256": hashlib.sha256(inventory_bytes).hexdigest(),
        "cpu_section_inventory": "cpu_section_inventory.json",
        "cpu_section_inventory_sha256": hashlib.sha256(
            cpu_section_inventory_bytes
        ).hexdigest(),
        "generated_inventory": "generated_inventory.json",
        "generated_inventory_sha256": hashlib.sha256(
            generated_inventory_bytes
        ).hexdigest(),
        "generation_result": "generation_result.json",
        "generation_result_sha256": hashlib.sha256(
            generation_result_bytes
        ).hexdigest(),
        "alternate_entry_ledger": "alternate_entry_ledger.json",
        "alternate_entry_ledger_sha256": hashlib.sha256(
            alternate_entry_ledger_bytes
        ).hexdigest(),
        "covered_alias_ledger": "covered_alias_ledger.json",
        "covered_alias_ledger_sha256": hashlib.sha256(
            covered_alias_ledger_bytes
        ).hexdigest(),
        "exception_ledger": "exception_ledger.json",
        "exception_ledger_sha256": hashlib.sha256(
            exception_ledger_bytes
        ).hexdigest(),
        "manual_size_recovery_ledger": "manual_size_recovery_ledger.json",
        "manual_size_recovery_ledger_sha256": hashlib.sha256(
            manual_size_recovery_ledger_bytes
        ).hexdigest(),
        "n64recomp_decision_sidecar": "n64recomp_decision_sidecar.json",
        "n64recomp_decision_sidecar_sha256": hashlib.sha256(
            decision_sidecar_bytes
        ).hexdigest(),
        "overlay_slot_inventory": "overlay_slot_inventory.json",
        "overlay_slot_inventory_sha256": hashlib.sha256(
            slot_inventory_bytes
        ).hexdigest(),
        "overlay_lookup_table": "overlay_lookup_table.json",
        "overlay_lookup_table_sha256": hashlib.sha256(
            lookup_inventory_bytes
        ).hexdigest(),
        "overlay_lifecycle_table": "overlay_lifecycle_table.json",
        "overlay_lifecycle_table_sha256": hashlib.sha256(
            lifecycle_inventory_bytes
        ).hexdigest(),
        "relocation_table": "relocation_table.json",
        "relocation_table_sha256": hashlib.sha256(
            relocation_inventory_bytes
        ).hexdigest(),
        "report_set": "report_set.json",
        "report_set_sha256": hashlib.sha256(report_set_bytes).hexdigest(),
        "normalizer_revision_sha256": hashlib.sha256(normalizer_bytes).hexdigest(),
    }
    (output / "sources.json").write_bytes(canonical_bytes(manifest))
    runtime_manifest_binding = hashlib.sha256(
        json.dumps(
            runtime_manifest,
            sort_keys=True,
            separators=(",", ":"),
            ensure_ascii=True,
        ).encode("utf-8")
    ).hexdigest()
    write_text(
        output / "runtime-manifest.canonical.sha256",
        runtime_manifest_binding + "\n",
    )
    return {
        "authoritative_bodies": len(authoritative),
        "callable_wrappers": len(authoritative),
        "alternate_entry_thunks": len(support),
        "table_support_members": len(support_sources),
        "baseline_members": len(baseline_sources),
        "indirect_decision_sites": EXPECTED_INDIRECT_TRANSFER_SITE_COUNT,
        "indirect_decision_emissions": sum(decision_classification_counts.values()),
    }


def main() -> int:
    try:
        summary = normalize(parse_args())
    except NormalizationError as error:
        print(f"Phase 4 generation normalization failed: {error}", file=sys.stderr)
        return 1
    except (OSError, TypeError, ValueError):
        print("Phase 4 generation normalization failed: input or output unavailable", file=sys.stderr)
        return 1
    print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
