#!/usr/bin/env python3
"""Local-only CPU/RSP generation stage for the Windows bootstrap prototype."""
from __future__ import annotations

import argparse
import atexit
from dataclasses import asdict, replace
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import tomllib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from transform_private_n64recomp_inputs import Layout, parse_headers, parse_records, infer_main_code_delta
from probe_n64recomp_cpu import verify_private_work_directory


def derive_layout(rom: bytes, map_text: str, context: dict) -> dict:
    def symbol(name: str) -> int:
        matches = re.findall(r"^\s*(0x[0-9a-fA-F]+)\s+" + re.escape(name) + r"\s*=", map_text, re.M)
        if len(matches) != 1:
            raise ValueError("Missing or ambiguous linker boundary: " + name)
        return int(matches[0], 16)
    layout = Layout(*(symbol(name + "_ROM_START") for name in
        ("mainRelocTable", "overlayRomTable", "overlayTable", "overlay_1")),
        symbol("main_TEXT_SIZE"), symbol("main_DATA_SIZE") + symbol("main_RODATA_SIZE"))
    if not (0 < layout.main_relocation_start < layout.overlay_reference_table_start <
            layout.overlay_table_start < layout.overlay_data_start < len(rom)):
        raise ValueError("Invalid linker boundary ordering")
    main = [section for section in context["section"] if section["name"] == ".main"]
    if len(main) != 1:
        raise ValueError("Expected one main executable section")
    records = parse_records(rom, layout, parse_headers(rom, layout))
    delta = infer_main_code_delta(rom, layout, main[0], records)
    if not 0 <= delta < layout.main_text_size:
        raise ValueError("Invalid inferred entry prefix")
    return asdict(replace(layout, main_text_size=layout.main_text_size - delta))


def audio_configuration(program: bytes, data: bytes) -> str:
    # The ROM's DMA descriptors specify source offset, length-minus-one and
    # IMEM destination. Hardware address masks are independent of game content.
    if len(data) < 18 or len(program) % 4:
        raise ValueError("Incomplete audio program")
    primary_offset, primary_info, secondary_offset, secondary_info = struct.unpack_from(">4I", data)
    primary_size, secondary_transfer_size = (primary_info >> 16) + 1, (secondary_info >> 16) + 1
    # Splat's text container includes alignment NOPs after the DMA payload.
    secondary_size = (secondary_transfer_size + 15) & ~15
    primary_address, secondary_address = primary_info & 0xFFFF, secondary_info & 0xFFFF
    common = secondary_address - primary_address
    if not (primary_offset == 0 and 0 < common < primary_size and
            primary_offset + primary_size == secondary_offset and
            secondary_offset + secondary_size == len(program) and
            primary_address >= 0x1000 and primary_address + primary_size == 0x2000 and
            secondary_address + secondary_size <= 0x2000 and common % 4 == 0):
        raise ValueError("Audio DMA descriptors do not describe the supported two-variant layout")
    if any(program[secondary_offset + secondary_transfer_size:]):
        raise ValueError("Audio container has nonzero alignment padding")
    # Decode the contiguous dispatch table, stopping at non-code data.
    targets = []
    for offset in range(16, len(data) - 1, 2):
        target = struct.unpack_from(">H", data, offset)[0]
        if not primary_address <= target < 0x2000 or target % 4:
            break
        targets.append(target)
    if len(targets) != 16 or len(set(targets)) != 16:
        raise ValueError("Audio command dispatch table did not close")
    return (
        f'text_offset = {primary_offset}\ntext_size = {primary_size}\n'
        f'text_address = {0x04000000 | primary_address}\n'
        'rom_file_path = "program.bin"\noutput_file_path = "./jfg-audio-probe.cpp"\n'
        'output_function_name = "jfg_audio_probe"\n'
        f'extra_indirect_branch_targets = {targets}\n\n[[overlay_slots]]\n'
        f'text_address = {0x04000000 | secondary_address}\n'
        f'overlays = [{{ offset = {common}, size = {primary_size-common} }}, '
        f'{{ offset = {secondary_offset}, size = {secondary_size} }}]\n')


def generate_audio(workspace: Path, decomp: Path, rsp: Path, run, rom: bytes, cache_ready: bool) -> None:
    audio = workspace / "audio"
    audio.mkdir(exist_ok=True)
    program = (decomp / "assets/ucode_audio.textbin.bin").read_bytes()
    audio_data = (decomp / "assets/data_audio.databin.bin").read_bytes()
    (audio / "program.bin").write_bytes(program)
    (audio / "input.toml").write_text(audio_configuration(program, audio_data))
    run("generate-audio", [str(rsp), "input.toml"], audio)
    generated = audio / "jfg-audio-probe.cpp"
    text = generated.read_text()
    marker = "        slots[slot] = overlay;"
    if text.count(marker) != 1:
        raise ValueError("Unexpected RSP generator overlay dispatcher")
    text = text.replace(marker, "        jfg_audio_probe_overlay_swaps++;\n"
        "        jfg_audio_probe_overlay_variant_one += overlay == 1;\n" + marker)
    text = "#include <cstdint>\nuint32_t jfg_audio_probe_overlay_swaps = 0;\nuint32_t jfg_audio_probe_overlay_variant_one = 0;\n" + text
    generated.write_text(text)
    for name in ("brokered-rsp.hpp", "production-audio-adapter.cpp"):
        shutil.copy2(ROOT / "src/bootstrap" / name, audio / name)
    (audio / "brokered-jfg-audio-probe.cpp").write_text(
        '#include "brokered-rsp.hpp"\n#include "jfg-audio-probe.cpp"\n')
    manifest_files = ("normalized/sources.json", "libultra.json", "audio/production-audio-adapter.cpp",
                      "audio/brokered-rsp.hpp", "audio/brokered-jfg-audio-probe.cpp", "audio/jfg-audio-probe.cpp", "normalized/jfg_original_timing.h", "normalized/original-timing.json", "normalized/jfg_original_timing_fast.h", "normalized/funcs.h")
    (workspace / "generation.json").write_text(json.dumps({"complete": True,
        "rom_sha1": hashlib.sha1(rom).hexdigest(),
        "files": {name: hashlib.sha256((workspace / name).read_bytes()).hexdigest() for name in manifest_files},
        "fresh_matching_elf": True, "fresh_cpu": True, "fresh_audio": True,
        "tool_cache_reused": cache_ready}, indent=2) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom", type=Path, required=True)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--dependency-root", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=8)
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("Run this stage inside WSL/Linux")
    workspace, deps = args.workspace.resolve(), args.dependency_root.resolve()
    verify_private_work_directory(workspace)
    rom = args.rom.read_bytes()
    if len(rom) != 32*1024*1024 or hashlib.sha1(rom).hexdigest() != "493ced9008dbe932d6e91179b68e8630cf23a023":
        raise ValueError("ROM does not match the supported US revision")
    for name in ("git", "make", "gcc", "mips-linux-gnu-as", "mips-linux-gnu-objcopy", "readelf", "cmake", "ninja"):
        if not shutil.which(name):
            raise ValueError("Required WSL tool missing: " + name)
    # Make/IDO cannot reliably build in paths with spaces. A fresh private
    # Linux directory also prevents reuse of any pre-generated game output.
    scratch = Path(tempfile.mkdtemp(prefix="jfg-rom-build-"))
    (workspace / "linux-workspace.txt").write_text(str(scratch) + "\n")
    # RGO: the scratch copy (decomp with the extracted ROM, recompiler build)
    # is several GB inside the WSL disk; remove it when this stage exits.
    # Everything later steps need is written to the workspace.
    if os.environ.get("JFG_KEEP_LINUX_WORKSPACE") != "1":
        atexit.register(shutil.rmtree, scratch, ignore_errors=True)

    def run(name: str, command: list[str], cwd: Path = ROOT) -> None:
        print(name, flush=True)
        with (workspace / (name + ".log")).open("w") as log:
            subprocess.run(command, cwd=cwd, check=True, stdout=log, stderr=subprocess.STDOUT)

    decomp = scratch / "decomp"
    source = deps / "Jet-Force-Gemini"
    run("clone-metadata", ["git", "clone", "--no-hardlinks", str(source), str(decomp)])
    run("decomp-submodules", ["git", "submodule", "update", "--init", "--recursive"], decomp)
    (decomp / "baseroms").mkdir(exist_ok=True)
    (decomp / "baseroms/baserom.us.z64").write_bytes(rom)
    # Only tool caches may be reused. Never copy assets, asm, ELF or maps.
    # A venv from another WSL distribution can exist yet contain packages for
    # a different Python version. Always create this environment locally.
    run("python-environment", [sys.executable, "-m", "venv", str(decomp / ".venv")])
    run("python-dependencies", [str(decomp / ".venv/bin/python3"), "-m", "pip",
        "install", "-r", str(decomp / "requirements.txt")], decomp)
    cache_ready = all((source / item).exists() for item in
        ("tools/ido-recomp/linux/cc", "tools/n64crc", "tools/objdiff/objdiff-cli"))
    if cache_ready:
        for item in ("ido-recomp", "objdiff"):
            shutil.copytree(source / "tools" / item, decomp / "tools" / item)
        shutil.copy2(source / "tools/n64crc", decomp / "tools/n64crc")
    else:
        run("setup-metadata-tools", ["make", "-C", "tools"], decomp)
    run("extract-rom", ["make", "extract"], decomp)
    run("matching-elf", ["make", f"-j{args.jobs}", "COLOR=0"], decomp)
    if hashlib.sha1((decomp / "build/jfg.us.z64").read_bytes()).hexdigest() != hashlib.sha1(rom).hexdigest():
        raise ValueError("Rebuilt ROM does not match the supplied ROM")
    # Prepare the exact patched recompiler in its own checkout.
    recomp = scratch / "N64Recomp"
    run("clone-recompiler", ["git", "clone", "--no-hardlinks", str(deps / "N64Recomp"), str(recomp)])
    run("recompiler-submodules", ["git", "submodule", "update", "--init", "--recursive"], recomp)
    run("patch-recompiler", [sys.executable, str(ROOT / "scripts/apply_n64recomp_patchset.py"),
        "--target", str(recomp), "--project-root", str(ROOT)])
    run("configure-recompiler", ["cmake", "-S", str(recomp), "-B", str(recomp / "build"),
        "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"])
    run("build-recompiler", ["cmake", "--build", str(recomp / "build"), "--target", "N64RecompCLI", "RSPRecomp", "-j", str(args.jobs)])
    cpu, rsp = recomp / "build/N64Recomp", recomp / "build/RSPRecomp"
    elf = decomp / "build/jfg.us.elf"
    # The current CPU root includes four OS routines missing FUNC metadata in
    # the upstream ELF. Existing recovery validates their control flow and
    # callers against this ROM and proves all allocated ELF bytes unchanged.
    for helper in ("task", "pi-init", "vi-main", "cont-read-pack"):
        recovered = workspace / ("helper-" + helper)
        run("recover-" + helper, [sys.executable, "-m", "scripts.phase9_recover_task_helper",
            str(recovered), "--elf", str(elf), "--elf-sha256", hashlib.sha256(elf.read_bytes()).hexdigest(),
            "--rom", str(args.rom), "--helper", helper])
        elf = recovered / ("input-with-" + helper + "-helper.elf")
    context = workspace / "context"
    context.mkdir()
    run("context-config", [sys.executable, str(ROOT / "scripts/prepare_n64recomp_context_dump.py"),
        "--elf", str(elf), "--generated-output", str(context / "unused"),
        "--output-config", str(context / "input.toml")])
    run("context-dump", [str(cpu), str(context / "input.toml"), "--dump-context"], context)
    layout = derive_layout(rom, (decomp / "build/jfg.us.map").read_text(),
                           tomllib.loads((context / "dump.toml").read_text()))
    (workspace / "layout.json").write_text(json.dumps(layout))
    transform = workspace / "transform"
    run("transform", [sys.executable, str(ROOT / "scripts/transform_private_n64recomp_inputs.py"),
        "--rom", str(args.rom), "--elf", str(elf), "--readelf", shutil.which("readelf"),
        "--layout", str(workspace / "layout.json"), "--context", str(context / "dump.toml"),
        "--data-context", str(context / "data_dump.toml"), "--output-dir", str(transform)])
    import original_timing_generation as original_timing
    timing_sites = original_timing.configure(transform)
    run("generate-cpu", [str(cpu), "recompile-private.toml"], transform)
    paired_transfers = original_timing.prepare_raw(transform / "generated")
    run("normalize", [sys.executable, str(ROOT / "scripts/build_private_generated_root.py"),
        "--raw-generated", str(transform / "generated"), "--symbols", str(transform / "symbols-private.toml"),
        "--original-context", str(context / "dump.toml"), "--runtime-manifest", str(transform / "runtime-link-private.json"),
        "--recomp-header", str(recomp / "include/recomp.h"), "--output", str(workspace / "normalized")])
    original_timing.finish(workspace / "normalized", timing_sites, paired_transfers)
    run("identify-sdk", [sys.executable, str(ROOT / "scripts/identify_libultra.py"),
        "--decomp-symbols", str(decomp / "ver/symbols/symbol_addrs.us.txt"),
        "--generated-symbols", str(transform / "symbols-private.toml"), "--private-out", str(workspace / "libultra.json")])
    generate_audio(workspace, decomp, rsp, run, rom, cache_ready)
    print("Fresh CPU and audio generation complete.", flush=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
