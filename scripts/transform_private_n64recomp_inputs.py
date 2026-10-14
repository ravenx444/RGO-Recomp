#!/usr/bin/env python3
"""Build an ignored, private N64Recomp symbol file from JFG linker metadata.

The generated ROM copy, TOML, and diagnostic details are ROM-derived and must
remain under the ignored tools tree.  Standard output contains aggregates only.
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import os
import struct
import subprocess
import tomllib
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable

try:
    from .probe_n64recomp_cpu import EXPECTED_ROM_SHA1, EXPECTED_ROM_SIZE
    from .validate_elf import parse_sections, parse_symbols
except ImportError:  # Direct script execution.
    from probe_n64recomp_cpu import EXPECTED_ROM_SHA1, EXPECTED_ROM_SIZE
    from validate_elf import parse_sections, parse_symbols

HEADER = struct.Struct(">iiiiiHHii")
RELOCATION = struct.Struct(">II")
PATCH_NAMES = {
    4: "R_MIPS_26",
    5: "R_MIPS_HI16",
    6: "R_MIPS_LO16",
}
LINKED_KSEG0_BASE = 0x80000000
RDRAM_END = 0x80800000
EXPECTED_R26_RELOCATION_COUNT = 8980
EXPECTED_SUPPORT_THUNK_COUNT = 30
# Revised input inventory: independently boundary-validated OS helpers are
# a required body, not a support thunk. Older Phase 4 inputs deliberately fail
# these closed counts; their signed evidence is not evidence for this revision.
EXPECTED_DIRECT_CALL_CANDIDATE_COUNT = 14053
EXPECTED_DIRECT_LINKED_CALL_CANDIDATE_COUNT = 13964
EXPECTED_DIRECT_TAIL_CANDIDATE_COUNT = 89
EXPECTED_DIRECT_GENERATED_TARGET_CANDIDATE_COUNT = 13867
EXPECTED_DIRECT_UNRESOLVED_TARGET_CANDIDATE_COUNT = 186
EXPECTED_EXECUTABLE_SYMBOL_COUNT = 3733
EXPECTED_COVERED_ALIAS_COUNT = 824
EXPECTED_COVERED_ALIAS_BODY_START_COUNT = 805
EXPECTED_COVERED_ALIAS_INTERIOR_COUNT = 19
EXPECTED_MANUAL_SIZE_RECOVERY_COUNT = 10
EXPECTED_INDIRECT_TRANSFER_SITE_COUNT = 3624
EXPECTED_NATIVE_RETURN_SITE_COUNT = 3419
EXPECTED_INDIRECT_DECISION_SITE_COUNT = 205


@dataclass(frozen=True)
class Layout:
    main_relocation_start: int
    overlay_reference_table_start: int
    overlay_table_start: int
    overlay_data_start: int
    main_text_size: int
    main_data_size: int


@dataclass(frozen=True)
class Header:
    slot: int
    vram_base: int
    rom_offset: int
    text_size: int
    data_size: int
    bss_size: int
    primary_bytes: int
    secondary_bytes: int
    init_offset: int
    resume_offset: int

    @property
    def populated(self) -> bool:
        return self.text_size + self.data_size != 0

    @property
    def memory_size(self) -> int:
        return self.text_size + self.data_size + self.bss_size

    def rom_start(self, layout: Layout) -> int:
        return layout.overlay_data_start + self.rom_offset

    def primary_start(self, layout: Layout) -> int:
        return self.rom_start(layout) + self.text_size + self.data_size

    def secondary_start(self, layout: Layout) -> int:
        return self.primary_start(layout) + self.primary_bytes


@dataclass(frozen=True)
class Record:
    source_module: int
    table_kind: str
    symbol: int
    site_offset: int
    patch_type: int
    source_type: int


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--rom", type=Path, required=True)
    parser.add_argument("--elf", type=Path)
    parser.add_argument("--readelf", type=Path)
    parser.add_argument("--native-metadata", type=Path)
    parser.add_argument("--metadata-manifest-sha256")
    parser.add_argument("--layout", type=Path, required=True)
    parser.add_argument("--context", type=Path, required=True)
    parser.add_argument("--data-context", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    if args.native_metadata is not None:
        if not args.metadata_manifest_sha256 or args.elf is not None or args.readelf is not None:
            parser.error("Native metadata requires its trusted manifest SHA256 and excludes ELF/readelf")
    elif args.elf is None or args.readelf is None or args.metadata_manifest_sha256:
        parser.error("Legacy metadata requires --elf and --readelf")
    return args


def load_layout(path: Path) -> Layout:
    value = json.loads(path.read_text(encoding="utf-8"))
    return Layout(
        main_relocation_start=value["main_relocation_start"],
        overlay_reference_table_start=value["overlay_reference_table_start"],
        overlay_table_start=value["overlay_table_start"],
        overlay_data_start=value["overlay_data_start"],
        main_text_size=value["main_text_size"],
        main_data_size=value["main_data_size"],
    )


def parse_headers(rom: bytes, layout: Layout) -> list[Header]:
    data = rom[layout.overlay_table_start : layout.overlay_data_start]
    headers = [Header(slot, *values) for slot, values in enumerate(HEADER.iter_unpack(data), 1)]
    if len(headers) != 157 or sum(header.populated for header in headers) != 155:
        raise ValueError("unexpected overlay header population")
    return headers


def parse_entry(entry: tuple[int, int], source_module: int, table_kind: str) -> Record:
    symbol, info = entry
    return Record(source_module, table_kind, symbol, info >> 8, (info >> 4) & 0xF, info & 0xF)


def parse_records(rom: bytes, layout: Layout, headers: list[Header]) -> list[Record]:
    main_count = struct.unpack_from(">I", rom, layout.main_relocation_start)[0]
    main_start = layout.main_relocation_start + 4
    main_end = main_start + main_count * RELOCATION.size
    records = [parse_entry(entry, 0, "main") for entry in RELOCATION.iter_unpack(rom[main_start:main_end])]
    for header in headers:
        if not header.populated:
            continue
        primary_start = header.primary_start(layout)
        secondary_start = header.secondary_start(layout)
        records.extend(
            parse_entry(entry, header.slot, "primary")
            for entry in RELOCATION.iter_unpack(rom[primary_start : primary_start + header.primary_bytes])
        )
        records.extend(
            parse_entry(entry, header.slot, "secondary")
            for entry in RELOCATION.iter_unpack(rom[secondary_start : secondary_start + header.secondary_bytes])
        )
    return records


def read_word(rom: bytes | bytearray, offset: int) -> int:
    if offset < 0 or offset + 4 > len(rom) or offset % 4:
        raise ValueError("private word access is out of bounds or unaligned")
    return struct.unpack_from(">I", rom, offset)[0]


def infer_main_code_delta(rom: bytes, layout: Layout, main_section: dict[str, Any], records: list[Record]) -> int:
    jal_sites = [record.site_offset for record in records if record.source_module == 0 and record.patch_type == 4]
    if not jal_sites:
        raise ValueError("main relocation table contains no direct calls")
    max_site = max(jal_sites)
    max_delta = min(
        int(main_section["size"]) - max_site - 4,
        layout.main_relocation_start - int(main_section["rom"]) - max_site - 4,
    )
    if max_delta < 0:
        raise ValueError("main executable section cannot contain the relocation sites")
    candidates: list[int] = []
    for delta in range(0, max_delta + 1, 4):
        first_word = read_word(rom, int(main_section["rom"]) + delta + jal_sites[0])
        if first_word >> 26 != 3:
            continue
        if all(read_word(rom, int(main_section["rom"]) + delta + site) >> 26 == 3 for site in jal_sites[1:]):
            candidates.append(delta)
    if len(candidates) != 1:
        raise ValueError("main code-base inference is not unique")
    return candidates[0]


def classify_ort(word: int) -> str:
    module = word >> 20
    if module == 0:
        return "main"
    if 1 <= module <= 157:
        return "overlay"
    if module in {0xFFD, 0xFFE}:
        return "main-data"
    if module == 0xFFF:
        return "main-bss"
    if module == 0xFFC:
        return "special-reserved"
    return "anomalous"


def sign_extend_16(value: int) -> int:
    return value - 0x10000 if value & 0x8000 else value


def normalize_runtime_addend(value: int, unresolved_sentinel: int | None = None) -> int:
    """Normalize the custom linker's KSEG0-linked addend to a section offset."""
    value &= 0xFFFFFFFF
    if unresolved_sentinel is not None and value == unresolved_sentinel:
        return 0
    if value & LINKED_KSEG0_BASE:
        return value - LINKED_KSEG0_BASE
    return value


def resolve_relative_target(
    sections: list[dict[str, Any]], target_section: int, target_offset: int, alignment: int = 1
) -> tuple[int, int, int]:
    """Resolve an explicitly section-relative target without address guessing."""
    if not 0 <= target_section < len(sections) or target_offset < 0:
        raise ValueError("invalid relocation target metadata")
    base = int(sections[target_section]["vram"])
    extent = int(sections[target_section]["relocation_size"])
    address = base + target_offset
    if target_offset >= extent or address > 0xFFFFFFFF:
        raise ValueError("relocation target is outside its declared section extent")
    if target_offset % alignment or address % alignment:
        raise ValueError("relocation target does not meet its required alignment")
    return target_section, target_offset, address


def test_target_normalization() -> None:
    synthetic = [
        {"vram": 0x1000, "relocation_size": 0x5000},
        {"vram": 0x2000, "relocation_size": 0x1000},
    ]
    # 0x2000 is both a valid offset in section 0 and a linked address in
    # section 1. Explicit offset provenance must keep it in section 0.
    if resolve_relative_target(synthetic, 0, 0x2000) != (0, 0x2000, 0x3000):
        raise AssertionError("relative relocation provenance was not preserved")
    if normalize_runtime_addend(0x80000004) != 4:
        raise AssertionError("KSEG0 relocation addend normalization failed")


def toml_quote(value: str) -> str:
    return json.dumps(value, ensure_ascii=True)


def function_intervals(sections: list[dict[str, Any]]) -> dict[int, list[tuple[int, int]]]:
    result: dict[int, list[tuple[int, int]]] = {}
    for index, section in enumerate(sections):
        base = int(section["vram"])
        result[index] = sorted(
            (int(function["vram"]) - base, int(function["vram"]) - base + int(function["size"]))
            for function in section["functions"]
        )
    return result


def find_cross_body_branch_entries(
    rom: bytes,
    sections: list[dict[str, Any]],
) -> dict[int, set[int]]:
    entries: dict[int, set[int]] = collections.defaultdict(set)
    intervals = function_intervals(sections)
    for section_index, section in enumerate(sections):
        base = int(section["vram"])
        starts = {start for start, _ in intervals[section_index]}
        for function in section["functions"]:
            function_vram = int(function["vram"])
            function_size = int(function["size"])
            function_start = function_vram - base
            function_end = function_start + function_size
            function_rom = int(section["rom"]) + function_start
            for instruction_offset in range(0, function_size, 4):
                word = read_word(rom, function_rom + instruction_offset)
                opcode = word >> 26
                pc = function_vram + instruction_offset
                target_vram: int | None = None
                if opcode == 2:
                    target_vram = ((pc + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
                elif opcode in {4, 5, 6, 7, 20, 21, 22, 23}:
                    target_vram = (pc + 4 + (sign_extend_16(word & 0xFFFF) << 2)) & 0xFFFFFFFF
                elif opcode == 1 and ((word >> 16) & 0x1F) in {0, 1, 2, 3, 16, 17, 18, 19}:
                    target_vram = (pc + 4 + (sign_extend_16(word & 0xFFFF) << 2)) & 0xFFFFFFFF
                elif opcode == 17 and ((word >> 21) & 0x1F) == 8:
                    target_vram = (pc + 4 + (sign_extend_16(word & 0xFFFF) << 2)) & 0xFFFFFFFF
                if target_vram is None:
                    continue
                target_offset = target_vram - base
                if function_start <= target_offset < function_end or target_offset in starts:
                    continue
                if any(start < target_offset < end for start, end in intervals[section_index]):
                    entries[section_index].add(target_offset)
    return entries


def find_indirect_transfer_sites(
    rom: bytes, sections: list[dict[str, Any]]
) -> list[dict[str, int | str]]:
    """Enumerate every JR/JALR in authoritative executable bodies.

    Register targets cannot be reconstructed statically here.  Each site is
    therefore carried as an ignored ledger row with an explicit fail-closed
    disposition instead of being silently omitted from the call denominator.
    """
    rows: list[dict[str, int | str]] = []
    for section_index, section in enumerate(sections):
        base = int(section["vram"])
        for function in section["functions"]:
            start = int(function["vram"]) - base
            size = int(function["size"])
            for offset in range(0, size, 4):
                word = read_word(rom, int(section["rom"]) + start + offset)
                if word >> 26 != 0:
                    continue
                function_code = word & 0x3F
                if function_code in {8, 9}:
                    rows.append({
                        "section": section_index,
                        "offset": start + offset,
                        "transfer_kind": "jr" if function_code == 8 else "jalr",
                        "source_register": (word >> 21) & 0x1F,
                    })
    return rows


def run_readelf(readelf_tool: Path, elf: Path, option: str) -> str:
    if (
        not readelf_tool.is_absolute()
        or not readelf_tool.is_file()
        or not elf.is_file()
        or option not in {"-SW", "-sW"}
    ):
        raise ValueError("fixed readelf boundary is invalid")
    result = subprocess.run(
        [str(readelf_tool), option, str(elf)],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=60,
        check=False,
    )
    if result.returncode != 0 or result.stderr or len(result.stdout) > 128 * 1024 * 1024:
        raise ValueError("fixed readelf execution failed")
    try:
        return result.stdout.decode("utf-8")
    except UnicodeError as error:
        raise ValueError("fixed readelf output is invalid") from error


def derive_executable_symbol_ledgers(
    elf: Path, readelf_tool: Path, context_sections: list[dict[str, Any]]
) -> tuple[list[dict[str, int | str]], list[dict[str, int | str]]]:
    """Derive covered zero-size aliases and recovered zero-size bodies from ELF."""
    elf_sections = parse_sections(run_readelf(readelf_tool, elf, "-SW"))
    symbols = parse_symbols(run_readelf(readelf_tool, elf, "-sW"))
    section_by_index = {str(section.index): section for section in elf_sections}
    executable = [
        symbol
        for symbol in symbols
        if symbol.symbol_type == "FUNC"
        and symbol.defined
        and symbol.section in section_by_index
        and "X" in section_by_index[symbol.section].flags
    ]
    positive = [symbol for symbol in executable if symbol.size > 0]
    zero = [symbol for symbol in executable if symbol.size == 0]
    if len(executable) != EXPECTED_EXECUTABLE_SYMBOL_COUNT:
        raise ValueError("authoritative executable symbol denominator changed")

    context_functions = [
        (section_index, function)
        for section_index, section in enumerate(context_sections)
        for function in section["functions"]
    ]

    covered_rows: list[dict[str, int | str]] = []
    uncovered = []
    for symbol in zero:
        owners = [
            candidate
            for candidate in positive
            if candidate.section == symbol.section
            and candidate.value <= symbol.value < candidate.value + candidate.size
        ]
        if len(owners) == 1:
            owner = owners[0]
            context_matches = [
                (section_index, function)
                for section_index, function in context_functions
                if int(function["vram"]) == owner.value
                and int(function["size"]) == owner.size
            ]
            if len(context_matches) != 1:
                raise ValueError("covered alias owner is absent from context")
            section_index, _ = context_matches[0]
            covered_rows.append(
                {
                    "private_name": symbol.name,
                    "private_owner_name": owner.name,
                    "section": section_index,
                    "offset": symbol.value - int(context_sections[section_index]["vram"]),
                    "owner_offset": owner.value
                    - int(context_sections[section_index]["vram"]),
                    "owner_size": owner.size,
                }
            )
        elif not owners:
            uncovered.append(symbol)
        else:
            raise ValueError("covered alias has ambiguous sized owner")

    starts_by_section: dict[str, list[int]] = collections.defaultdict(list)
    for symbol in executable:
        starts_by_section[symbol.section].append(symbol.value)
    for section_index, values in starts_by_section.items():
        section = section_by_index[section_index]
        values.append(section.address + section.size)
        starts_by_section[section_index] = sorted(set(values))
    recovery_rows: list[dict[str, int | str]] = []
    for symbol in uncovered:
        following = next(
            value
            for value in starts_by_section[symbol.section]
            if value > symbol.value
        )
        recovered_size = following - symbol.value
        context_matches = [
            (section_index, function)
            for section_index, function in context_functions
            if int(function["vram"]) == symbol.value
            and int(function["size"]) == recovered_size
        ]
        if len(context_matches) != 1 or recovered_size <= 0 or recovered_size % 4:
            raise ValueError("manual size recovery does not match context")
        section_index, function = context_matches[0]
        recovery_rows.append(
            {
                "private_name": symbol.name,
                "generated_function": str(function["name"]),
                "section": section_index,
                "offset": symbol.value - int(context_sections[section_index]["vram"]),
                "recovered_size": recovered_size,
            }
        )

    covered_rows.sort(
        key=lambda row: (int(row["section"]), int(row["offset"]), str(row["private_name"]))
    )
    recovery_rows.sort(
        key=lambda row: (int(row["section"]), int(row["offset"]), str(row["private_name"]))
    )
    body_start_count = sum(row["offset"] == row["owner_offset"] for row in covered_rows)
    interior_coordinates = {
        (int(row["section"]), int(row["offset"]))
        for row in covered_rows
        if row["offset"] != row["owner_offset"]
    }
    if (
        len(covered_rows) != EXPECTED_COVERED_ALIAS_COUNT
        or body_start_count != EXPECTED_COVERED_ALIAS_BODY_START_COUNT
        or len(interior_coordinates) != EXPECTED_COVERED_ALIAS_INTERIOR_COUNT
        or len(recovery_rows) != EXPECTED_MANUAL_SIZE_RECOVERY_COUNT
    ):
        raise ValueError("authoritative zero-size function partition changed")
    return covered_rows, recovery_rows


def find_direct_call_candidates(
    rom: bytes,
    sections: list[dict[str, Any]],
    generated_functions: list[list[dict[str, int | str]]],
    approved_fail_closed_sites: set[tuple[int, int]],
    r26_targets_by_site: dict[tuple[int, int], tuple[int, int]],
) -> list[dict[str, int | str | None]]:
    """Decode every direct call/tail emission candidate in authoritative code."""
    generated_targets: dict[int, tuple[int, int, str]] = {}
    generated_targets_by_coordinate: dict[tuple[int, int], tuple[int, int, str]] = {}
    authoritative_starts = {
        int(function["vram"])
        for section in sections
        for function in section["functions"]
    }
    for section_index, functions in enumerate(generated_functions):
        base = int(sections[section_index]["vram"])
        for function in functions:
            target_vram = int(function["vram"])
            if target_vram in generated_targets:
                raise ValueError("generated direct-call target is ambiguous")
            generated_targets[target_vram] = (
                section_index,
                target_vram - base,
                (
                    "authoritative-body"
                    if target_vram in authoritative_starts
                    else "alternate-entry"
                ),
            )
            generated_targets_by_coordinate[
                (section_index, target_vram - base)
            ] = generated_targets[target_vram]

    rows_by_site: dict[tuple[int, int], dict[str, int | str | None]] = {}
    for section_index, section in enumerate(sections):
        base = int(section["vram"])
        rom_base = int(section["rom"])
        for function in generated_functions[section_index]:
            start = int(function["vram"]) - base
            size = int(function["size"])
            for function_offset in range(0, size, 4):
                site_offset = start + function_offset
                word = read_word(rom, rom_base + site_offset)
                opcode = word >> 26
                register_immediate = (word >> 16) & 0x1F
                transfer_role: str
                if opcode == 3:
                    instruction_class = "jal"
                    transfer_role = "linked-call"
                elif opcode == 1 and register_immediate == 17:
                    instruction_class = "bgezal"
                    transfer_role = "linked-call"
                elif opcode == 2:
                    jump_target = (
                        ((base + site_offset + 4) & 0xF0000000)
                        | ((word & 0x03FFFFFF) << 2)
                    )
                    function_start = int(function["vram"])
                    function_end = function_start + size
                    if function_start <= jump_target < function_end:
                        continue
                    instruction_class = "j"
                    transfer_role = "direct-tail"
                elif opcode in {4, 5, 6, 7}:
                    branch_target = (
                        base
                        + site_offset
                        + 4
                        + (sign_extend_16(word & 0xFFFF) << 2)
                    ) & 0xFFFFFFFF
                    function_start = int(function["vram"])
                    function_end = function_start + size
                    if function_start <= branch_target < function_end:
                        continue
                    instruction_class = "conditional-branch"
                    transfer_role = "direct-tail"
                else:
                    continue
                pc = base + site_offset
                target_vram = (
                    ((pc + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
                    if instruction_class in {"jal", "j"}
                    else (
                        pc + 4 + (sign_extend_16(word & 0xFFFF) << 2)
                    )
                    & 0xFFFFFFFF
                )
                site = (section_index, site_offset)
                relocation_target = r26_targets_by_site.get(site)
                if relocation_target is not None:
                    target = generated_targets_by_coordinate.get(relocation_target)
                    target_vram = (
                        int(sections[relocation_target[0]]["vram"])
                        + relocation_target[1]
                    )
                else:
                    target = generated_targets.get(target_vram)
                target_section, target_offset, target_class = (
                    target
                    if target is not None
                    else (None, None, "unresolved-candidate")
                )
                candidate = {
                    "source_section": section_index,
                    "source_offset": site_offset,
                    "transfer_role": transfer_role,
                    "instruction_class": instruction_class,
                    "decoded_target_vram": target_vram,
                    "target_section": target_section,
                    "target_offset": target_offset,
                    "target_class": target_class,
                    "disposition": (
                        "approved-fail-closed-trap"
                        if site in approved_fail_closed_sites
                        else "requires-n64recomp-direct-observation"
                    ),
                }
                previous = rows_by_site.get(site)
                if previous is not None and previous != candidate:
                    raise ValueError("direct-call owner projections disagree")
                rows_by_site[site] = candidate
    rows = [rows_by_site[site] for site in sorted(rows_by_site)]
    candidate_sites = {
        (int(row["source_section"]), int(row["source_offset"])) for row in rows
    }
    transfer_role_counts = collections.Counter(row["transfer_role"] for row in rows)
    instruction_class_counts = collections.Counter(
        row["instruction_class"] for row in rows
    )
    if (
        len(r26_targets_by_site) != EXPECTED_R26_RELOCATION_COUNT
        or not set(r26_targets_by_site).issubset(candidate_sites)
        or transfer_role_counts
        != {"linked-call": 13964, "direct-tail": 89}
        or instruction_class_counts
        != {"jal": 13963, "bgezal": 1, "j": 76, "conditional-branch": 13}
    ):
        raise ValueError(
            "R_MIPS_26 sites do not reconcile with call candidates: "
            f"site_count={len(rows)}, role_counts={dict(transfer_role_counts)}, "
            f"instruction_class_counts={dict(instruction_class_counts)}"
        )
    return rows


def opaque_ledger_id(prefix: str, value: object) -> str:
    payload = json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return prefix + "-" + hashlib.sha256(payload).hexdigest()[:24]


def canonical_digest(value: object) -> str:
    return hashlib.sha256(
        json.dumps(
            value,
            sort_keys=True,
            separators=(",", ":"),
            ensure_ascii=True,
            allow_nan=False,
        ).encode("utf-8")
    ).hexdigest()


def approval_digest(
    policy_id: str,
    opaque_id: str,
    category: str,
    disposition: str,
) -> str:
    """Bind an approved exception to the closed tracked policy vocabulary.

    This is deliberately a policy-approval commitment, not a claim that a
    person signed an individual ROM coordinate.  The private row remains in
    the ignored transform output; the digest can safely cross into aggregate
    evidence without disclosing that row.
    """
    return canonical_digest(
        {
            "policy_id": policy_id,
            "opaque_id": opaque_id,
            "category": category,
            "disposition": disposition,
        }
    )


def split_functions(sections: list[dict[str, Any]], split_offsets: dict[int, set[int]]) -> tuple[list[list[dict[str, int | str]]], int, int]:
    rewritten: list[list[dict[str, int | str]]] = []
    split_count = 0
    uncovered_count = 0
    for section_index, section in enumerate(sections):
        base = int(section["vram"])
        functions = sorted(section["functions"], key=lambda item: int(item["vram"]))
        output: list[dict[str, int | str]] = []
        pending = set(split_offsets.get(section_index, set()))
        for function in functions:
            start = int(function["vram"]) - base
            end = start + int(function["size"])
            cuts = sorted(offset for offset in pending if start < offset < end)
            split_count += len(cuts)
            pending.difference_update(cuts)
            pending.discard(start)
            if end <= start or start % 4 or (end - start) % 4:
                raise ValueError("invalid function boundary")
            # Keep the authoritative body intact. Interior direct-call targets are
            # alternate entry points, so add deterministic overlapping bodies from
            # each entry to the original end instead of severing intra-body branches.
            output.append({"vram": base + start, "size": end - start})
            for entry in cuts:
                if entry % 4 or end <= entry:
                    raise ValueError("invalid synthesized entry boundary")
                output.append({"vram": base + entry, "size": end - entry})
        uncovered_count += len(pending)
        rewritten.append(sorted(output, key=lambda item: (int(item["vram"]), -int(item["size"]))))
    for section_index, functions in enumerate(rewritten):
        for ordinal, function in enumerate(functions):
            function["name"] = f"fn_{section_index:03d}_{ordinal:04d}"
    return rewritten, split_count, uncovered_count


def write_symbol_file(
    path: Path,
    sections: list[dict[str, Any]],
    functions: list[list[dict[str, int | str]]],
    relocs: dict[int, list[dict[str, int | str]]],
) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as stream:
        for section_index, section in enumerate(sections):
            stream.write("[[section]]\n")
            stream.write(f'name = "section_{section_index:03d}"\n')
            stream.write(f'rom = 0x{int(section["rom"]):08X}\n')
            stream.write(f'vram = 0x{int(section["vram"]):08X}\n')
            stream.write(f'size = 0x{int(section["size"]):08X}\n')
            stream.write(f'relocation_size = 0x{int(section["relocation_size"]):08X}\n')
            stream.write("functions = [\n")
            for function in functions[section_index]:
                stream.write(
                    "    { name = %s, vram = 0x%08X, size = 0x%08X },\n"
                    % (toml_quote(str(function["name"])), int(function["vram"]), int(function["size"]))
                )
            stream.write("]\n")
            stream.write("relocs = [\n")
            for reloc in sorted(relocs.get(section_index, []), key=lambda item: int(item["vram"])):
                stream.write(
                    "    { type = %s, vram = 0x%08X, target_section = %d, target_section_offset = 0x%08X },\n"
                    % (
                        toml_quote(str(reloc["type"])),
                        int(reloc["vram"]),
                        int(reloc["target_section"]),
                        int(reloc["target_section_offset"]),
                    )
                )
            stream.write("]\n\n")


def write_config(path: Path) -> None:
    path.write_text(
        "[input]\n"
        'symbols_file_path = "symbols-private.toml"\n'
        'rom_file_path = "patched-private.z64"\n'
        'output_func_path = "generated"\n'
        'indirect_decision_sidecar_path = "generated/indirect_decisions.json"\n'
        "functions_per_output_file = 50\n"
        "unpaired_lo16_warnings = false\n"
        "use_lookup_for_all_function_calls = true\n",
        encoding="utf-8",
        newline="\n",
    )


def find_named_function(sections: list[dict[str, Any]], name: str) -> tuple[int, int]:
    matches = [
        (section_index, int(function["vram"]) - int(section["vram"]))
        for section_index, section in enumerate(sections)
        for function in section["functions"]
        if function.get("name") == name
    ]
    if len(matches) != 1:
        raise ValueError("required fail-closed function target is not unique")
    return matches[0]


def find_named_data_symbol(data_context: dict[str, Any], name: str) -> int:
    matches = [
        int(symbol["vram"])
        for section in data_context.get("section", [])
        for symbol in section.get("symbols", [])
        if symbol.get("name") == name
    ]
    if len(set(matches)) != 1:
        raise ValueError("required fail-closed data target is not unique")
    return matches[0]


def derive_main_bss_size(
    data_context: dict[str, Any], main_base: int, layout: Layout, main_code_delta: int
) -> int:
    """Derive the contiguous, non-ROM-backed main BSS from the data context."""
    initialized_extent = main_code_delta + layout.main_text_size + layout.main_data_size
    values = (main_base, main_code_delta, layout.main_text_size, layout.main_data_size)
    if (
        any(value < 0 or value > 0xFFFFFFFF or value % 4 for value in values)
        or initialized_extent > 0xFFFFFFFF
        or main_base < LINKED_KSEG0_BASE
        or main_base + initialized_extent >= RDRAM_END
    ):
        raise ValueError("main runtime extent is invalid")
    sections = data_context.get("section")
    if not isinstance(sections, list):
        raise ValueError("main BSS context is unavailable")
    bss_start = main_base + initialized_extent
    candidates: list[dict[str, Any]] = []
    for section in sections:
        if not isinstance(section, dict) or "rom" in section:
            continue
        try:
            vram = section["vram"]
            size = section["size"]
        except (KeyError, TypeError, ValueError) as error:
            raise ValueError("main BSS context is invalid") from error
        if type(vram) is not int or type(size) is not int:
            raise ValueError("main BSS context is invalid")
        if vram == bss_start:
            candidates.append(section)
    if len(candidates) != 1:
        raise ValueError("main BSS context is not unique")
    candidate = candidates[0]
    name = candidate.get("name")
    if not isinstance(name, str) or not name.lower().endswith("bss"):
        raise ValueError("main BSS context is invalid")
    bss_size = candidate["size"]
    if (
        bss_size <= 0
        or bss_size % 4
        or bss_start % 4
        or bss_start + bss_size > RDRAM_END
    ):
        raise ValueError("main BSS extent is invalid")
    return bss_size


def write_runtime_tables(
    output_dir: Path,
    sections: list[dict[str, Any]],
    headers: list[Header],
    overlay_section_by_slot: dict[int, int],
    main_section_index: int,
    layout: Layout,
    main_code_delta: int,
    main_bss_size: int,
    runtime_relocs: list[dict[str, int | str]],
    audit: dict[str, object],
) -> str:
    section_rows: list[dict[str, int | str]] = [
        {
            "module": 0,
            "section": main_section_index,
            "kind": "main",
            "rom": int(sections[main_section_index]["rom"]),
            "linked_vram": int(sections[main_section_index]["vram"]),
            "text_offset": main_code_delta,
            "text_size": layout.main_text_size,
            "data_size": layout.main_data_size,
            "bss_size": main_bss_size,
        }
    ]
    for header in headers:
        if not header.populated:
            continue
        section_index = overlay_section_by_slot[header.slot]
        section_rows.append(
            {
                "module": header.slot,
                "section": section_index,
                "kind": "overlay",
                "rom": int(sections[section_index]["rom"]),
                "linked_vram": int(sections[section_index]["vram"]),
                "text_offset": 0,
                "text_size": header.text_size,
                "data_size": header.data_size,
                "bss_size": header.bss_size,
            }
        )
    # Overlay slots are a loader-facing denominator, not executable sections:
    # two slots deliberately contain no module and must never be fabricated as
    # generated code or assigned a section-address entry.
    overlay_slots = [
        {
            "slot": header.slot,
            "section": overlay_section_by_slot[header.slot] if header.populated else None,
            "disposition": "populated" if header.populated else "empty-fail-closed",
            # The ignored source binding includes the complete header, even
            # though empty slot callback/ROM metadata is never exposed to the
            # generated runtime table or used as executable behavior.
            "source_binding": hashlib.sha256(json.dumps(header.__dict__, sort_keys=True, separators=(",", ":")).encode("utf-8")).hexdigest(),
        }
        for header in headers
    ]
    if len(overlay_slots) != 157 or sum(row["section"] is None for row in overlay_slots) != 2:
        raise ValueError("overlay slot ledger did not reconcile")
    semantic_ledgers = audit.pop("semantic_ledgers", None)
    if not isinstance(semantic_ledgers, dict):
        raise ValueError("runtime semantic ledgers are unavailable")
    manifest = {
        "schema_version": 1,
        "sections": sorted(section_rows, key=lambda item: int(item["section"])),
        "r_mips_32": runtime_relocs,
        "overlay_slots": overlay_slots,
        "audit": audit,
        "semantic_ledgers": semantic_ledgers,
    }
    canonical = json.dumps(manifest, sort_keys=True, separators=(",", ":")).encode("utf-8")
    digest = hashlib.sha256(canonical).hexdigest()
    (output_dir / "runtime-link-private.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )

    class_code = {
        "main": 0,
        "overlay": 1,
        "main-data": 2,
        "main-bss": 3,
        "local-offset": 4,
        "local-jump": 5,
    }
    table_path = output_dir / "runtime-link-private.cpp"
    with table_path.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write("#include <cstddef>\n#include <cstdint>\n\n")
        stream.write("struct RuntimeSection { std::uint16_t module, section; std::uint32_t rom, linked_vram, text_offset, text_size, data_size, bss_size; };\n")
        stream.write("struct RuntimeReloc { std::uint16_t source_section, target_section; std::uint32_t site_offset, target_offset; std::uint8_t source_type, target_class; };\n\n")
        stream.write("extern const RuntimeSection kRuntimeSections[] = {\n")
        for row in manifest["sections"]:
            stream.write(
                "    {%d, %d, 0x%08X, 0x%08X, 0x%08X, 0x%08X, 0x%08X, 0x%08X},\n"
                % (
                    int(row["module"]), int(row["section"]), int(row["rom"]), int(row["linked_vram"]),
                    int(row["text_offset"]), int(row["text_size"]), int(row["data_size"]), int(row["bss_size"]),
                )
            )
        stream.write("};\nextern const std::size_t kRuntimeSectionCount = sizeof(kRuntimeSections) / sizeof(kRuntimeSections[0]);\n\n")
        stream.write("extern const RuntimeReloc kRuntimeRelocs[] = {\n")
        for row in runtime_relocs:
            stream.write(
                "    {%d, %d, 0x%08X, 0x%08X, %d, %d},\n"
                % (
                    int(row["source_section"]), int(row["target_section"]), int(row["site_offset"]),
                    int(row["target_section_offset"]), int(row["source_type"]), class_code[str(row["target_class"])],
                )
            )
        stream.write("};\nextern const std::size_t kRuntimeRelocCount = sizeof(kRuntimeRelocs) / sizeof(kRuntimeRelocs[0]);\n")

    (output_dir / "runtime-link-probe.cpp").write_text(
        "#include <cstddef>\n#include <cstdint>\n\n"
        "struct RuntimeSection { std::uint16_t module, section; std::uint32_t rom, linked_vram, text_offset, text_size, data_size, bss_size; };\n"
        "struct RuntimeReloc { std::uint16_t source_section, target_section; std::uint32_t site_offset, target_offset; std::uint8_t source_type, target_class; };\n"
        "extern const RuntimeSection kRuntimeSections[];\nextern const std::size_t kRuntimeSectionCount;\n"
        "extern const RuntimeReloc kRuntimeRelocs[];\nextern const std::size_t kRuntimeRelocCount;\n"
        "volatile std::uint64_t probe_sink;\n"
        "int main() {\n"
        "    if (kRuntimeSectionCount != 156 || kRuntimeRelocCount != 2028) return 2;\n"
        "    std::uint64_t fold = 0;\n"
        "    for (std::size_t i = 0; i < kRuntimeSectionCount; ++i) fold ^= kRuntimeSections[i].rom + kRuntimeSections[i].text_size;\n"
        "    for (std::size_t i = 0; i < kRuntimeRelocCount; ++i) fold ^= kRuntimeRelocs[i].site_offset + kRuntimeRelocs[i].target_offset;\n"
        "    probe_sink = fold;\n"
        "    return 0;\n"
        "}\n",
        encoding="utf-8",
        newline="\n",
    )
    return digest


def main() -> int:
    args = parse_args()
    test_target_normalization()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    rom = args.rom.read_bytes()
    if len(rom) != EXPECTED_ROM_SIZE or hashlib.sha1(rom).hexdigest() != EXPECTED_ROM_SHA1:
        raise ValueError("unsupported ROM input")
    layout = load_layout(args.layout)
    headers = parse_headers(rom, layout)
    records = parse_records(rom, layout, headers)
    context = tomllib.loads(args.context.read_text(encoding="utf-8"))
    data_context = tomllib.loads(args.data_context.read_text(encoding="utf-8"))
    sections = context.get("section")
    if not isinstance(sections, list) or len(sections) != 156:
        raise ValueError("unexpected executable section inventory")
    if args.native_metadata is not None:
        try:
            from .native_generation_metadata import load_bundle
        except ImportError:  # Direct script execution.
            from native_generation_metadata import load_bundle
        factual_context, factual_data, bindings = load_bundle(
            args.native_metadata, args.metadata_manifest_sha256
        )
        if (context != factual_context or data_context != factual_data
                or json.loads(args.layout.read_text(encoding="utf-8")) != bindings["layout"]):
            raise ValueError("Native metadata inputs do not match the qualified bundle")
        covered_alias_rows = bindings["covered_aliases"]
        manual_size_recovery_rows = bindings["manual_sizes"]
    else:
        covered_alias_rows, manual_size_recovery_rows = derive_executable_symbol_ledgers(
            args.elf, args.readelf, sections
        )

    overlay_section_by_slot: dict[int, int] = {}
    section_by_rom = {int(section["rom"]): index for index, section in enumerate(sections)}
    overlay_vram_matches = 0
    overlay_initial_unloaded_bases = sum(header.populated and header.vram_base == 0 for header in headers)
    overlay_text_contained = 0
    for header in headers:
        if not header.populated:
            continue
        section_index = section_by_rom.get(header.rom_start(layout))
        if section_index is None or section_index in overlay_section_by_slot.values():
            raise ValueError("overlay sections do not reconcile with executable sections")
        section = sections[section_index]
        overlay_vram_matches += int(int(section["vram"]) == header.vram_base)
        overlay_text_contained += int(int(section["size"]) >= header.text_size)
        # The loader retains the primary relocation table immediately after
        # text/data/BSS in the same allocation, and local relocations may
        # address that storage.
        section["relocation_size"] = header.memory_size + header.primary_bytes
        overlay_section_by_slot[header.slot] = section_index
    if overlay_text_contained != 155:
        raise ValueError(
            "overlay section reconciliation failed "
            f"(vram matches={overlay_vram_matches}, text-contained={overlay_text_contained})"
        )
    main_sections = set(range(len(sections))) - set(overlay_section_by_slot.values())
    if len(main_sections) != 1:
        raise ValueError("main executable section is not unique")
    main_section_index = next(iter(main_sections))
    main_section = sections[main_section_index]
    main_base = int(main_section["vram"])
    permanent_main_ends = [
        int(section.get("vram", 0)) + int(section.get("size", 0))
        for section in data_context.get("section", [])
        if int(section.get("vram", 0)) >= main_base
        and int(section.get("vram", 0)) + int(section.get("size", 0)) <= 0x100000000
    ]
    if not permanent_main_ends:
        raise ValueError("main relocation extent cannot be established")
    # This broad extent validates relocation targets; it is deliberately not
    # the main loader BSS allocation, which is derived below from its exact
    # adjacent non-ROM data-context section.
    main_section["relocation_size"] = max(permanent_main_ends) - main_base
    main_code_delta = infer_main_code_delta(rom, layout, main_section, records)
    main_bss_size = derive_main_bss_size(data_context, main_base, layout, main_code_delta)
    for section in sections:
        base = int(section["vram"])
        extent = int(section["relocation_size"])
        if base + extent > 0x100000000:
            raise ValueError("invalid relocation target extent")

    fail_closed_trap_section, fail_closed_trap_offset = find_named_function(sections, "TrapDanglingJump")
    unresolved_data_vram = find_named_data_symbol(data_context, "gUnresolvedSymbolAddr")
    unresolved_data_offset = (unresolved_data_vram - int(main_section["vram"])) & 0xFFFFFFFF

    ort_bytes = rom[layout.overlay_reference_table_start : layout.overlay_table_start]
    ort = [word for (word,) in struct.iter_unpack(">I", ort_bytes)]
    header_by_slot = {header.slot: header for header in headers}
    if any(
        record.source_module != 0
        and (record.source_module not in header_by_slot or not header_by_slot[record.source_module].populated)
        for record in records
    ):
        raise ValueError("relocation references an empty overlay slot")
    patched_rom = bytearray(rom)
    relocs: dict[int, list[dict[str, int | str]]] = collections.defaultdict(list)
    runtime_relocs: list[dict[str, int | str]] = []
    r26_targets: dict[int, set[int]] = collections.defaultdict(set)
    aggregates: collections.Counter[str] = collections.Counter()
    by_patch: collections.Counter[int] = collections.Counter()
    by_source: collections.Counter[int] = collections.Counter()
    skipped_classes: collections.Counter[str] = collections.Counter()

    def source_section_and_site(record: Record) -> tuple[int, int, int]:
        if record.source_module == 0:
            section_index = main_section_index
            site_offset = main_code_delta + record.site_offset
            if record.source_type == 3:
                site_offset = main_code_delta + layout.main_text_size + record.site_offset
        else:
            section_index = overlay_section_by_slot[record.source_module]
            site_offset = record.site_offset
        section = sections[section_index]
        return section_index, site_offset, int(section["rom"]) + site_offset

    def base_target(record: Record, site_word: int) -> tuple[int, int, str] | None:
        if record.source_type in {0, 3}:
            if record.symbol >= len(ort):
                return None
            target_word = ort[record.symbol]
            target_class = classify_ort(target_word)
            target_module = target_word >> 20
            target_offset = target_word & 0xFFFFF
            if target_class == "main":
                return main_section_index, main_code_delta + target_offset, target_class
            if target_class == "overlay":
                target_section = overlay_section_by_slot.get(target_module)
                if target_section is None:
                    return None
                return target_section, target_offset, target_class
            if target_class == "main-data":
                return main_section_index, main_code_delta + layout.main_text_size + target_offset, target_class
            if target_class == "main-bss":
                return main_section_index, main_code_delta + layout.main_text_size + layout.main_data_size + target_offset, target_class
            return None
        if record.source_type == 1:
            source_section = main_section_index if record.source_module == 0 else overlay_section_by_slot[record.source_module]
            return source_section, record.symbol, "local-offset"
        if record.source_type == 2:
            source_section = main_section_index if record.source_module == 0 else overlay_section_by_slot[record.source_module]
            return source_section, (site_word & 0x03FFFFFF) << 2, "local-jump"
        return None

    def external_target_class(record: Record) -> str | None:
        if record.source_type not in {0, 3} or record.symbol >= len(ort):
            return None
        return classify_ort(ort[record.symbol])

    def patch_hi_lo_words(hi_rom: int, lo_rom: int, value: int) -> None:
        hi_word = read_word(patched_rom, hi_rom)
        lo_word = read_word(patched_rom, lo_rom)
        patched_hi = (hi_word & 0xFFFF0000) | (((value + 0x8000) >> 16) & 0xFFFF)
        patched_lo = (lo_word & 0xFFFF0000) | (value & 0xFFFF)
        struct.pack_into(">I", patched_rom, hi_rom, patched_hi)
        struct.pack_into(">I", patched_rom, lo_rom, patched_lo)

    def require_lo_pair(record_index: int, record: Record) -> tuple[Record, int, int, int]:
        if record_index + 1 >= len(records):
            raise ValueError("unpaired HI16 relocation")
        next_record = records[record_index + 1]
        if (
            next_record.source_module != record.source_module
            or next_record.table_kind != record.table_kind
            or next_record.patch_type != 6
            or next_record.source_type != record.source_type
            or next_record.symbol != record.symbol
        ):
            raise ValueError("invalid HI16/LO16 pair")
        next_source_section, next_site_offset, next_site_rom = source_section_and_site(next_record)
        if next_site_offset == source_section_and_site(record)[1]:
            raise ValueError("HI16/LO16 pair uses the same patch site")
        return next_record, next_source_section, next_site_offset, next_site_rom

    index = 0
    while index < len(records):
        record = records[index]
        by_patch[record.patch_type] += 1
        by_source[record.source_type] += 1
        source_section, site_offset, site_rom = source_section_and_site(record)
        site_word = read_word(patched_rom, site_rom)
        if record.patch_type == 2:
            target = base_target(record, site_word)
            if target is None:
                aggregates["unresolved_runtime_r_mips_32"] += 1
                index += 1
                continue
            target_section, target_offset, target_class = target
            if record.source_type == 1:
                target_offset += normalize_runtime_addend(site_word)
            target_section, target_offset, _ = resolve_relative_target(
                sections, target_section, target_offset
            )
            runtime_relocs.append(
                {
                    "source_section": source_section,
                    "site_offset": site_offset,
                    "target_section": target_section,
                    "target_section_offset": target_offset,
                    "source_type": record.source_type,
                    "target_class": target_class,
                }
            )
            aggregates["runtime_r_mips_32"] += 1
            index += 1
            continue
        if record.patch_type not in PATCH_NAMES:
            aggregates["unsupported_patch_type"] += 1
            index += 1
            continue
        target = base_target(record, site_word)
        if target is None:
            target_class = external_target_class(record)
            if target_class == "anomalous" and record.patch_type == 4:
                skipped_classes[target_class] += 1
                target = (fail_closed_trap_section, fail_closed_trap_offset, "fail-closed-trap")
                aggregates["fail_closed_anomalous_direct"] += 1
            elif target_class == "special-reserved" and record.patch_type == 5:
                next_record, next_source_section, next_site_offset, next_site_rom = require_lo_pair(index, record)
                if next_source_section != source_section:
                    raise ValueError("reserved HI16/LO16 pair crosses source sections")
                skipped_classes[target_class] += 2
                lo_word = read_word(patched_rom, next_site_rom)
                original_addend = ((site_word & 0xFFFF) << 16) + sign_extend_16(lo_word & 0xFFFF)
                aggregates["reserved_pair_addend_matches_unresolved_data"] += int(
                    (original_addend & 0xFFFFFFFF) == unresolved_data_vram
                )
                reserved_section, reserved_offset, _ = resolve_relative_target(
                    sections, main_section_index, unresolved_data_offset
                )
                patch_hi_lo_words(site_rom, next_site_rom, unresolved_data_vram)
                for pair_record, pair_site_offset in ((record, site_offset), (next_record, next_site_offset)):
                    relocs[source_section].append(
                        {
                            "type": PATCH_NAMES[pair_record.patch_type],
                            "vram": int(sections[source_section]["vram"]) + pair_site_offset,
                            "target_section": reserved_section,
                            "target_section_offset": reserved_offset,
                        }
                    )
                aggregates["translated_code_reloc"] += 2
                aggregates["fail_closed_reserved_hi_lo_pairs"] += 1
                aggregates["fail_closed_reserved_hi_lo_sites"] += 2
                by_patch[next_record.patch_type] += 1
                by_source[next_record.source_type] += 1
                index += 2
                continue
            else:
                aggregates["unresolved_code_reloc"] += 1
                index += 1
                continue
        target_section, target_offset, target_class = target
        aggregates[f"target_{target_class}"] += 1

        if record.patch_type == 5:
            next_record, next_source_section, next_site_offset, next_site_rom = require_lo_pair(index, record)
            if next_source_section != source_section:
                raise ValueError("HI16/LO16 pair crosses source sections")
            lo_word = read_word(patched_rom, next_site_rom)
            addend = ((site_word & 0xFFFF) << 16) + sign_extend_16(lo_word & 0xFFFF)
            final_offset = target_offset + normalize_runtime_addend(addend, unresolved_data_vram)
            target_section, final_offset, final_vram = resolve_relative_target(
                sections, target_section, final_offset
            )
            patch_hi_lo_words(site_rom, next_site_rom, final_vram)
            for pair_record, pair_site_offset in ((record, site_offset), (next_record, next_site_offset)):
                relocs[source_section].append(
                    {
                        "type": PATCH_NAMES[pair_record.patch_type],
                        "vram": int(sections[source_section]["vram"]) + pair_site_offset,
                        "target_section": target_section,
                        "target_section_offset": final_offset,
                    }
                )
            aggregates["translated_code_reloc"] += 2
            aggregates["translated_hi_lo_pairs"] += 1
            by_patch[next_record.patch_type] += 1
            by_source[next_record.source_type] += 1
            index += 2
            continue

        if record.patch_type == 6:
            final_offset = target_offset + (site_word & 0xFFFF)
            target_section, final_offset, final_vram = resolve_relative_target(
                sections, target_section, final_offset
            )
            patched_word = (site_word & 0xFFFF0000) | (final_vram & 0xFFFF)
            struct.pack_into(">I", patched_rom, site_rom, patched_word)
            aggregates["translated_standalone_lo16"] += 1
        else:
            final_offset = target_offset
            if site_word >> 26 != 3:
                raise ValueError("R_MIPS_26 patch site is not a JAL instruction")
            if site_offset % 4:
                raise ValueError("R_MIPS_26 patch site is not word aligned")
            target_section, final_offset, target_vram = resolve_relative_target(
                sections, target_section, final_offset, alignment=4
            )
            patched_word = (site_word & 0xFC000000) | ((target_vram >> 2) & 0x03FFFFFF)
            struct.pack_into(">I", patched_rom, site_rom, patched_word)
            r26_targets[target_section].add(final_offset)
            aggregates["patched_r_mips_26"] += 1

        relocs[source_section].append(
            {
                "type": PATCH_NAMES[record.patch_type],
                "vram": int(sections[source_section]["vram"]) + site_offset,
                "target_section": target_section,
                "target_section_offset": final_offset,
            }
        )
        aggregates["translated_code_reloc"] += 1
        index += 1

    if sum(by_patch.values()) != len(records) or sum(by_source.values()) != len(records):
        raise ValueError("relocation accounting mismatch")

    for source_index, source_relocs in relocs.items():
        ordered = sorted(source_relocs, key=lambda item: int(item["vram"]))
        if any(int(current["vram"]) >= int(following["vram"]) for current, following in zip(ordered, ordered[1:])):
            raise ValueError("code relocation sites are duplicated or unordered")
        for reloc in ordered:
            source = sections[source_index]
            source_address = int(reloc["vram"])
            source_offset = source_address - int(source["vram"])
            if source_offset < 0 or source_offset % 4 or source_offset + 4 > int(source["size"]):
                raise ValueError("code relocation site is outside its source section")
            target = sections[int(reloc["target_section"])]
            offset = int(reloc["target_section_offset"])
            if offset >= int(target["relocation_size"]) or int(target["vram"]) + offset > 0xFFFFFFFF:
                raise ValueError("code relocation target is outside its declared extent")
    for reloc in runtime_relocs:
        target = sections[int(reloc["target_section"])]
        offset = int(reloc["target_section_offset"])
        if offset >= int(target["relocation_size"]) or int(target["vram"]) + offset > 0xFFFFFFFF:
            raise ValueError("runtime relocation target is outside its declared extent")

    intervals = function_intervals(sections)
    exact_targets = 0
    interior_targets = 0
    gap_targets = 0
    split_offsets: dict[int, set[int]] = collections.defaultdict(set)
    for section_index, targets in r26_targets.items():
        cur_intervals = intervals[section_index]
        starts = {start for start, _ in cur_intervals}
        for target in targets:
            if target in starts:
                exact_targets += 1
            elif any(start < target < end for start, end in cur_intervals):
                interior_targets += 1
                split_offsets[section_index].add(target)
            else:
                gap_targets += 1
    branch_entry_offsets = find_cross_body_branch_entries(patched_rom, sections)
    branch_entry_count = 0
    for section_index, entries in branch_entry_offsets.items():
        new_entries = entries - split_offsets[section_index]
        branch_entry_count += len(new_entries)
        split_offsets[section_index].update(entries)
    same_body_link_entry_offsets: dict[int, set[int]] = collections.defaultdict(set)
    for section_index, section in enumerate(sections):
        base = int(section["vram"])
        rom_base = int(section["rom"])
        for function in section["functions"]:
            start = int(function["vram"]) - base
            end = start + int(function["size"])
            for source_offset in range(start, end, 4):
                word = read_word(patched_rom, rom_base + source_offset)
                if word >> 26 != 1 or ((word >> 16) & 0x1F) != 17:
                    continue
                target = source_offset + 4 + (sign_extend_16(word & 0xFFFF) << 2)
                if start < target < end:
                    same_body_link_entry_offsets[section_index].add(target)
    same_body_link_entry_count = 0
    for section_index, entries in same_body_link_entry_offsets.items():
        new_entries = entries - split_offsets[section_index]
        same_body_link_entry_count += len(new_entries)
        split_offsets[section_index].update(entries)
    alias_entry_offsets: dict[int, set[int]] = collections.defaultdict(set)
    for row in covered_alias_rows:
        if row["offset"] != row["owner_offset"]:
            alias_entry_offsets[int(row["section"])].add(int(row["offset"]))
    alias_entry_count = 0
    for section_index, entries in alias_entry_offsets.items():
        new_entries = entries - split_offsets[section_index]
        alias_entry_count += len(new_entries)
        split_offsets[section_index].update(entries)
    rewritten_functions, split_count, uncovered_split_count = split_functions(sections, split_offsets)
    if (
        uncovered_split_count != 0
        or split_count
        != (
            interior_targets
            + branch_entry_count
            + same_body_link_entry_count
            + alias_entry_count
        )
        or alias_entry_count != 16
        or same_body_link_entry_count != 1
        or split_count != EXPECTED_SUPPORT_THUNK_COUNT
    ):
        raise ValueError("direct-call synthesized entries did not reconcile")

    code_site_outside_function = 0
    for section_index, section_relocs in relocs.items():
        base = int(sections[section_index]["vram"])
        ranges = [(int(function["vram"]), int(function["vram"]) + int(function["size"])) for function in rewritten_functions[section_index]]
        for reloc in section_relocs:
            address = int(reloc["vram"])
            if not any(start <= address < end for start, end in ranges):
                code_site_outside_function += 1

    (output_dir / "patched-private.z64").write_bytes(patched_rom)
    write_symbol_file(output_dir / "symbols-private.toml", sections, rewritten_functions, relocs)
    write_config(output_dir / "recompile-private.toml")
    if len(runtime_relocs) != 2028 or aggregates["unresolved_runtime_r_mips_32"] != 0:
        raise ValueError("R_MIPS_32 runtime table did not reconcile")
    exception_records = [
        record
        for record in records
        if record.patch_type in PATCH_NAMES
        and record.source_type in {0, 3}
        and record.symbol < len(ort)
        and classify_ort(ort[record.symbol]) in {"special-reserved", "anomalous"}
    ]
    anomalous_records = [record for record in exception_records if classify_ort(ort[record.symbol]) == "anomalous"]
    reserved_records = [record for record in exception_records if classify_ort(ort[record.symbol]) == "special-reserved"]
    if len(anomalous_records) != 1 or len(reserved_records) != 2:
        raise ValueError("fail-closed exception inventory changed")
    exception_ledger = []
    for record in anomalous_records:
        opaque_id = opaque_ledger_id("exception", record.__dict__)
        category = "anomalous-r-mips-26"
        disposition = "fail-closed-trap"
        exception_ledger.append({
            "opaque_id": opaque_id,
            "category": "anomalous-r-mips-26",
            "evidence_class": "direct-call",
            "owner_role": "transformer",
            "reason": "out-of-domain-overlay-reference",
            "disposition": disposition,
            "approval_sha256": approval_digest(
                "phase4-transform-exception-policy-v1",
                opaque_id,
                category,
                disposition,
            ),
            "private_record": record.__dict__,
        })
    for record in reserved_records:
        opaque_id = opaque_ledger_id("exception", record.__dict__)
        category = "reserved-atomic-hi-lo"
        disposition = "unresolved-data-atomic-pair"
        exception_ledger.append({
            "opaque_id": opaque_id,
            "category": "reserved-atomic-hi-lo",
            "evidence_class": "instruction-relocation",
            "owner_role": "transformer",
            "reason": "reserved-reference-class",
            "disposition": disposition,
            "approval_sha256": approval_digest(
                "phase4-transform-exception-policy-v1",
                opaque_id,
                category,
                disposition,
            ),
            "private_record": record.__dict__,
        })
    if len({row["opaque_id"] for row in exception_ledger}) != len(exception_ledger):
        raise ValueError("exception ledger IDs are not unique")
    generated_function_by_coordinate = {
        (
            section_index,
            int(function["vram"]) - int(sections[section_index]["vram"]),
        ): str(function["name"])
        for section_index, functions in enumerate(rewritten_functions)
        for function in functions
    }
    covered_alias_ledger = []
    for source in covered_alias_rows:
        coordinate = (int(source["section"]), int(source["offset"]))
        generated_function = generated_function_by_coordinate.get(coordinate)
        if generated_function is None:
            raise ValueError("covered alias lacks deterministic generated mapping")
        mapping_role = (
            "authoritative-body"
            if source["offset"] == source["owner_offset"]
            else "alternate-entry-thunk"
        )
        disposition = (
            "deterministic-body-alias"
            if mapping_role == "authoritative-body"
            else "deterministic-thunk-alias"
        )
        opaque_source = {
            "private_name": source["private_name"],
            "section": source["section"],
            "offset": source["offset"],
            "generated_function": generated_function,
        }
        opaque_id = opaque_ledger_id("alias", opaque_source)
        covered_alias_ledger.append(
            {
                "opaque_id": opaque_id,
                "mapping_role": mapping_role,
                "evidence_class": "zero-size-covered-function",
                "reason": "covered-address-in-authoritative-body",
                "disposition": disposition,
                "approval_sha256": approval_digest(
                    "phase4-covered-alias-policy-v1",
                    opaque_id,
                    "zero-size-covered-alias",
                    disposition,
                ),
                "generated_function": generated_function,
                "private_source": source,
            }
        )
    manual_size_recovery_ledger = []
    for source in manual_size_recovery_rows:
        opaque_id = opaque_ledger_id("manual-size", source)
        disposition = "recovered-authoritative-body"
        manual_size_recovery_ledger.append(
            {
                "opaque_id": opaque_id,
                "mapping_role": "authoritative-body",
                "evidence_class": "uncovered-zero-size-function",
                "reason": "next-executable-symbol-boundary",
                "disposition": disposition,
                "approval_sha256": approval_digest(
                    "phase4-manual-size-recovery-policy-v1",
                    opaque_id,
                    "uncovered-zero-size-function",
                    disposition,
                ),
                "generated_function": source["generated_function"],
                "private_source": source,
            }
        )
    if (
        len(covered_alias_ledger) != EXPECTED_COVERED_ALIAS_COUNT
        or len(manual_size_recovery_ledger) != EXPECTED_MANUAL_SIZE_RECOVERY_COUNT
        or len({row["opaque_id"] for row in covered_alias_ledger})
        != len(covered_alias_ledger)
        or len({row["opaque_id"] for row in manual_size_recovery_ledger})
        != len(manual_size_recovery_ledger)
    ):
        raise ValueError("function alias and manual-size ledgers are incomplete")
    indirect_ledger = [
        {
            "opaque_id": opaque_ledger_id("indirect", row),
            "transfer_kind": row["transfer_kind"],
            "source_register": row["source_register"],
            "discovery_class": (
                "native-return-candidate"
                if row["source_register"] == 31
                else "indirect-decision-candidate"
            ),
            # Register 31 strongly identifies the native-return route but does
            # not prove how N64Recomp emitted it.  Every site remains pending
            # until the pinned sidecar independently records the disposition.
            "disposition": "requires-n64recomp-observation",
            "private_site": {"section": row["section"], "offset": row["offset"]},
        }
        for row in find_indirect_transfer_sites(patched_rom, sections)
    ]
    if len(indirect_ledger) != EXPECTED_INDIRECT_TRANSFER_SITE_COUNT:
        raise ValueError("authoritative indirect transfer denominator changed")
    native_return_candidates = sum(
        row["discovery_class"] == "native-return-candidate"
        for row in indirect_ledger
    )
    decision_candidates = len(indirect_ledger) - native_return_candidates
    if (
        native_return_candidates != EXPECTED_NATIVE_RETURN_SITE_COUNT
        or decision_candidates != EXPECTED_INDIRECT_DECISION_SITE_COUNT
    ):
        raise ValueError("authoritative indirect transfer partition changed")
    if len({row["opaque_id"] for row in indirect_ledger}) != len(indirect_ledger):
        raise ValueError("indirect transfer ledger IDs are not unique")
    exception_ledger_sha256 = canonical_digest(exception_ledger)
    exception_approval_set_sha256 = canonical_digest(
        sorted(row["approval_sha256"] for row in exception_ledger)
    )
    indirect_ledger_sha256 = canonical_digest(indirect_ledger)
    covered_alias_ledger_sha256 = canonical_digest(covered_alias_ledger)
    covered_alias_approval_set_sha256 = canonical_digest(
        sorted(row["approval_sha256"] for row in covered_alias_ledger)
    )
    manual_size_recovery_ledger_sha256 = canonical_digest(
        manual_size_recovery_ledger
    )
    manual_size_recovery_approval_set_sha256 = canonical_digest(
        sorted(row["approval_sha256"] for row in manual_size_recovery_ledger)
    )
    anomalous_sites = {
        (source_section_and_site(record)[0], source_section_and_site(record)[1])
        for record in anomalous_records
    }
    r26_targets_by_site: dict[tuple[int, int], tuple[int, int]] = {}
    for source_section, source_relocs in relocs.items():
        source_base = int(sections[source_section]["vram"])
        for relocation in source_relocs:
            if relocation["type"] != "R_MIPS_26":
                continue
            site = (
                source_section,
                int(relocation["vram"]) - source_base,
            )
            if site in r26_targets_by_site:
                raise ValueError("R_MIPS_26 target mapping is duplicated")
            r26_targets_by_site[site] = (
                int(relocation["target_section"]),
                int(relocation["target_section_offset"]),
            )
    direct_call_candidate_ledger = []
    for row in find_direct_call_candidates(
        patched_rom,
        sections,
        rewritten_functions,
        anomalous_sites,
        r26_targets_by_site,
    ):
        direct_call_candidate_ledger.append(
            {"opaque_id": opaque_ledger_id("direct-call", row), **row}
        )
    generated_target_candidate_count = sum(
        row["target_class"] in {"authoritative-body", "alternate-entry"}
        for row in direct_call_candidate_ledger
    )
    unresolved_target_candidate_count = (
        len(direct_call_candidate_ledger) - generated_target_candidate_count
    )
    if (
        len(direct_call_candidate_ledger) != EXPECTED_DIRECT_CALL_CANDIDATE_COUNT
        or generated_target_candidate_count
        != EXPECTED_DIRECT_GENERATED_TARGET_CANDIDATE_COUNT
        or unresolved_target_candidate_count
        != EXPECTED_DIRECT_UNRESOLVED_TARGET_CANDIDATE_COUNT
        or sum(
            row["disposition"] == "approved-fail-closed-trap"
            for row in direct_call_candidate_ledger
        )
        != 1
        or len({row["opaque_id"] for row in direct_call_candidate_ledger})
        != len(direct_call_candidate_ledger)
    ):
        raise ValueError("authoritative direct-call candidate denominator changed")
    r26_records = [record for record in records if record.patch_type == 4]
    if len(r26_records) != EXPECTED_R26_RELOCATION_COUNT:
        raise ValueError("R_MIPS_26 relocation denominator changed")
    instruction_records = [
        record for record in records if record.patch_type in PATCH_NAMES
    ]
    r32_records = [record for record in records if record.patch_type == 2]
    direct_ledger_sha256 = canonical_digest(direct_call_candidate_ledger)
    instruction_ledger_sha256 = canonical_digest(
        [
            {
                "opaque_id": opaque_ledger_id("instruction", record.__dict__),
                "disposition": (
                    "approved-fail-closed-atomic-pair"
                    if record in reserved_records
                    else "resolved"
                ),
            }
            for record in instruction_records
        ]
    )
    r32_ledger_sha256 = canonical_digest(
        [opaque_ledger_id("r32", record.__dict__) for record in r32_records]
    )
    direct_approval_set_sha256 = canonical_digest(
        sorted(
            row["approval_sha256"]
            for row in exception_ledger
            if row["evidence_class"] == "direct-call"
        )
    )
    instruction_approval_set_sha256 = canonical_digest(
        sorted(
            row["approval_sha256"]
            for row in exception_ledger
            if row["evidence_class"] == "instruction-relocation"
        )
    )
    empty_approval_set_sha256 = canonical_digest([])
    audit = {
        "overlay_slot_count": len(headers),
        "empty_overlay_slot_count": sum(not header.populated for header in headers),
        "exception_ledger_sha256": exception_ledger_sha256,
        "exception_approval_set_sha256": exception_approval_set_sha256,
        "exception_category_counts": {
            "anomalous-r-mips-26": len(anomalous_records),
            "reserved-atomic-hi-lo": len(reserved_records),
        },
        "covered_alias_count": len(covered_alias_ledger),
        "covered_alias_body_start_count": sum(
            row["mapping_role"] == "authoritative-body"
            for row in covered_alias_ledger
        ),
        "covered_alias_interior_count": sum(
            row["mapping_role"] == "alternate-entry-thunk"
            for row in covered_alias_ledger
        ),
        "covered_alias_ledger_sha256": covered_alias_ledger_sha256,
        "covered_alias_approval_set_sha256": covered_alias_approval_set_sha256,
        "manual_size_recovery_count": len(manual_size_recovery_ledger),
        "manual_size_recovery_ledger_sha256": manual_size_recovery_ledger_sha256,
        "manual_size_recovery_approval_set_sha256": manual_size_recovery_approval_set_sha256,
        "direct_call_candidate_count": len(direct_call_candidate_ledger),
        "direct_linked_call_candidate_count": sum(
            row["transfer_role"] == "linked-call"
            for row in direct_call_candidate_ledger
        ),
        "direct_tail_candidate_count": sum(
            row["transfer_role"] == "direct-tail"
            for row in direct_call_candidate_ledger
        ),
        "direct_instruction_class_counts": dict(
            sorted(
                collections.Counter(
                    row["instruction_class"]
                    for row in direct_call_candidate_ledger
                ).items()
            )
        ),
        "direct_generated_target_candidate_count": generated_target_candidate_count,
        "direct_unresolved_target_candidate_count": unresolved_target_candidate_count,
        "direct_pending_n64recomp_observation_count": len(
            direct_call_candidate_ledger
        ),
        "direct_call_candidate_ledger_sha256": direct_ledger_sha256,
        "direct_call_approval_set_sha256": direct_approval_set_sha256,
        "r26_relocation_count": len(r26_records),
        "r26_relocation_resolved_count": len(r26_records) - len(anomalous_records),
        "r26_relocation_exception_count": len(anomalous_records),
        "instruction_relocation_count": len(instruction_records),
        "instruction_resolved_count": len(instruction_records) - len(reserved_records),
        "instruction_fail_closed_count": len(reserved_records),
        "instruction_relocation_ledger_sha256": instruction_ledger_sha256,
        "instruction_relocation_approval_set_sha256": instruction_approval_set_sha256,
        "hi_lo_pair_count": aggregates["translated_hi_lo_pairs"]
        + aggregates["fail_closed_reserved_hi_lo_pairs"],
        "r32_count": len(r32_records),
        "r32_ledger_sha256": r32_ledger_sha256,
        "r32_approval_set_sha256": empty_approval_set_sha256,
        "indirect_transfer_ledger_sha256": indirect_ledger_sha256,
        "indirect_transfer_count": len(indirect_ledger),
        "indirect_native_return_candidate_count": native_return_candidates,
        "indirect_decision_candidate_count": decision_candidates,
        "indirect_pending_n64recomp_observation_count": len(indirect_ledger),
        "stub_ledger_sha256": canonical_digest([]),
        "semantic_ledgers": {
            "exception_ledger": exception_ledger,
            "covered_alias_ledger": covered_alias_ledger,
            "manual_size_recovery_ledger": manual_size_recovery_ledger,
            "direct_call_candidate_ledger": direct_call_candidate_ledger,
            "indirect_transfer_ledger": indirect_ledger,
        },
    }
    runtime_digest = write_runtime_tables(
        output_dir, sections, headers, overlay_section_by_slot,
        main_section_index, layout, main_code_delta, main_bss_size,
        runtime_relocs, audit,
    )
    private_details = {
        "main_section_index": main_section_index,
        "main_code_delta": main_code_delta,
        "exception_ledger": exception_ledger,
        "covered_alias_ledger": covered_alias_ledger,
        "manual_size_recovery_ledger": manual_size_recovery_ledger,
        "direct_call_candidate_ledger": direct_call_candidate_ledger,
        "indirect_transfer_ledger": indirect_ledger,
        "game_function_stubs": [],
    }
    (output_dir / "transform-private.json").write_text(
        json.dumps(private_details, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    summary = {
        "schema_version": 1,
        "section_reconciliation": {
            "executable_sections": len(sections),
            "populated_overlays": len(overlay_section_by_slot),
            "main_sections": 1,
            "header_runtime_bases_initially_unloaded": overlay_initial_unloaded_bases,
            "linked_vram_surrogate_matches": overlay_vram_matches,
            "text_extents_contained": overlay_text_contained,
        },
        "functions": {
            "before": sum(len(section["functions"]) for section in sections),
            "after": sum(len(functions) for functions in rewritten_functions),
            "authoritative_bodies_retained": sum(len(section["functions"]) for section in sections),
            "direct_target_exact_starts": exact_targets,
            "direct_target_synthesized_entry_starts": interior_targets,
            "cross_body_branch_synthesized_entry_starts": branch_entry_count,
            "covered_alias_synthesized_entry_starts": alias_entry_count,
            "covered_alias_count": len(covered_alias_ledger),
            "manual_size_recovery_count": len(manual_size_recovery_ledger),
            "direct_target_gaps": gap_targets,
        },
        "relocations": {
            "total": len(records),
            "translated_code": aggregates["translated_code_reloc"],
            "runtime_r_mips_32": aggregates["runtime_r_mips_32"],
            "runtime_r_mips_32_table_entries": len(runtime_relocs),
            "runtime_link_manifest_sha256": runtime_digest,
            "unresolved_code": aggregates["unresolved_code_reloc"],
            "r_mips_26_patched": aggregates["patched_r_mips_26"],
            "normal_hi_lo_pairs": aggregates["translated_hi_lo_pairs"],
            "total_hi_lo_pairs": aggregates["translated_hi_lo_pairs"] + aggregates["fail_closed_reserved_hi_lo_pairs"],
            "standalone_lo16": aggregates["translated_standalone_lo16"],
            "targets_normalized_with_explicit_provenance": aggregates["translated_code_reloc"] + aggregates["runtime_r_mips_32"],
            "code_sites_outside_functions": code_site_outside_function,
            "source_type_counts": {str(key): by_source[key] for key in sorted(by_source)},
            "patch_type_counts": {str(key): by_patch[key] for key in sorted(by_patch)},
            "unresolved_target_classes": dict(sorted(skipped_classes.items())),
        },
        "fail_closed_exceptions": {
            "anomalous_direct_sites": aggregates["fail_closed_anomalous_direct"],
            "reserved_hi_lo_pairs": aggregates["fail_closed_reserved_hi_lo_pairs"],
            "reserved_hi_lo_sites": aggregates["fail_closed_reserved_hi_lo_sites"],
            "reserved_pair_consumed_atomically": aggregates["fail_closed_reserved_hi_lo_pairs"] == 1,
            "game_function_stubs": 0,
        },
        "direct_call_candidates": {
            "discovered_candidate_count": len(direct_call_candidate_ledger),
            "generated_target_candidate_count": generated_target_candidate_count,
            "unresolved_target_candidate_count": unresolved_target_candidate_count,
            "pending_n64recomp_observation_count": len(
                direct_call_candidate_ledger
            ),
            "r_mips_26_relocation_count": len(r26_records),
            "ledger_sha256": audit["direct_call_candidate_ledger_sha256"],
        },
        "indirect_transfers": {
            "discovered_site_count": len(indirect_ledger),
            "native_return_candidate_count": native_return_candidates,
            "decision_candidate_count": decision_candidates,
            "pending_n64recomp_observation_count": len(indirect_ledger),
            "ledger_sha256": audit["indirect_transfer_ledger_sha256"],
        },
        "privacy": "aggregate-only; private coordinates are stored only in ignored artifacts",
    }
    (output_dir / "transform-safe-summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
