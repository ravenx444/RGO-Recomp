"""Reproduce the qualified original-OS timing build from the user's own ROM.

Game instruction words remain in private generated output. The public helper
encodes hardware operation classes, architectural register handling and hook ABI.
"""
from pathlib import Path
import json
import re
import tomllib

INTEGER_COST = {24: 4, 25: 4, 26: 36, 27: 36, 28: 7, 29: 7, 30: 68, 31: 68}
FLOAT_COST = {(16, 0): 2, (16, 1): 2, (16, 2): 4, (16, 3): 28, (16, 4): 28, (16, 8): 4, (16, 9): 4, (16, 10): 4, (16, 11): 4, (16, 12): 4, (16, 13): 4, (16, 14): 4, (16, 15): 4, (16, 36): 4, (16, 37): 4, (17, 0): 2, (17, 1): 2, (17, 2): 7, (17, 3): 57, (17, 4): 57, (17, 8): 4, (17, 9): 4, (17, 10): 4, (17, 11): 4, (17, 12): 4, (17, 13): 4, (17, 14): 4, (17, 15): 4, (17, 32): 1, (17, 36): 4, (17, 37): 4, (20, 32): 4, (20, 33): 4, (21, 32): 4, (21, 33): 4}


def metadata(section: int, pc: int, word: int) -> int:
    primary = word >> 26
    fpu = primary in (0x11, 0x31, 0x35, 0x39, 0x3d)
    park = primary == 3 and (((pc+4) & 0xf0000000) | ((word & 0x03ffffff) << 2)) == 0x80075698
    branch = primary in range(1, 8) or primary in range(20, 24) or (primary == 0 and word & 63 in (8,9)) or (primary == 0x11 and (word >> 21) & 31 == 8)
    # Private fractional-player experiments need these boundaries to reach
    # the native observer even inside a fast-clock skip run.
    player_time_sites = (0x00f000d4, 0x010000d4, 0x01100088,
                         0x00f01b30, 0x010019f8, 0x0110192c)
    slow = word == 0x1000ffff or pc in (0x8009ab80, 0x80098b74, *player_time_sites)
    extra = INTEGER_COST.get(word & 63, 0) if primary == 0 else FLOAT_COST.get(((word >> 21) & 31, word & 63), 0) if primary == 17 else 0
    reads = (0x1a,0x1b,*range(0x20,0x28),0x30,0x31,0x34,0x35,0x37)
    writes = (*range(0x28,0x2f),0x38,0x39,0x3c,0x3d,0x3f)
    kind = 1 if primary in reads else 2 if primary in writes else 3 if primary == 0x2f else 0
    if not 0 <= section < 256 or not 0 <= extra < 128:
        raise ValueError("Timing metadata exceeds the hook ABI")
    return section | (fpu<<8) | (park<<9) | (branch<<10) | (slow<<11) | (kind<<12) | (extra<<16)


def configure(transform: Path) -> int:
    config = transform / "recompile-private.toml"
    text = config.read_text()
    if "emit_guest_" in text or "[[patches.hook]]" in text:
        raise ValueError("Timing hooks must be applied once to a fresh generation")
    parsed = tomllib.loads(text)
    options = parsed["input"]
    symbols_path = (transform / options["symbols_file_path"]).resolve()
    rom_path = (transform / options["rom_file_path"]).resolve()
    symbols, rom = tomllib.loads(symbols_path.read_text()), rom_path.read_bytes()
    settings = "emit_guest_link_registers = true\nemit_guest_cpu_state = true\nemit_guest_dynamic_returns = true\nemit_guest_idle_loops = true\n"
    if "[patches]" in text:
        text = text.replace("[patches]", settings+"[patches]",1)
    else:
        text += "\n"+settings+"[patches]\n"
    rows = [text]; count = 0
    for section_id, section in enumerate(symbols["section"]):
        for function in section["functions"]:
            start, size = function["vram"], function["size"]
            offset = section["rom"] + start - section["vram"]
            if size <= 0 or size % 4 or start % 4 or offset < 0 or offset+size > len(rom):
                raise ValueError("Invalid instruction extent")
            # Bound native code expansion to small generated functions. This is
            # a host optimization only; every guest instruction retains its cost.
            for byte in range(0,size,4):
                pc = start+byte
                word = int.from_bytes(rom[offset+byte:offset+byte+4],"big")
                hook = f"jfg_phase9_execution_probe_predecoded(0x{(metadata(section_id,pc,word) | (0x800000 if size <= 512 else 0)):08x}U, 0x{pc:08x}U, 0x{word:08x}U, ctx);"
                rows.append("[[patches.hook]]\nfunc = "+json.dumps(function["name"])+f"\nbefore_vram = 0x{pc:08x}\ntext = "+json.dumps(hook)+"\n")
                count += 1
    config.write_text("\n".join(rows))
    return count


def paired_register_transfers(text: str) -> tuple[str,int]:
    # In FR=0, an odd double-transfer register selects the even register pair.
    lines, op, index, guards, rewrites = [], None, None, 0, 0
    for line in text.splitlines():
        match = re.search(r"// 0x[0-9A-Fa-f]{8}:\s+(\S+)\s+(.*)",line)
        if match:
            op = match[1]
            register = re.match(r"\$f(\d+),",match[2])
            index = int(register[1]) if register else None
        if op in ("ldc1","sdc1") and index is not None and index % 2:
            if re.fullmatch(r"\s*CHECK_FR\(ctx, "+str(index)+r"\);",line):
                guards += 1
                line = "    /* FR=0 double transfers address the preceding even FPR. */"
            old = f"ctx->f{index}.u64"
            if old in line:
                line = line.replace(old,f"*(ctx->mips3_float_mode ? &ctx->f{index}.u64 : &ctx->f{index-1}.u64)")
                rewrites += 1
        lines.append(line)
    if guards != rewrites:
        raise ValueError("Incomplete paired-register transfer rewrite")
    return "\n".join(lines)+"\n", guards


def prepare_raw(directory: Path) -> int:
    transfers = 0
    for path in sorted(directory.glob("*.c")):
        text,count = paired_register_transfers(path.read_text())
        path.write_text(text); transfers += count
    return transfers


def finish(root: Path, sites: int, paired: int) -> None:
    if sites <= 0 or paired <= 0:
        raise ValueError("Original timing requires instruction hooks and paired-register recovery")
    header = root / "funcs.h"
    header.write_text(header.read_text()+"\n#ifdef __cplusplus\nextern \"C\" {\n#endif\nvoid jfg_phase9_execution_probe_predecoded(unsigned,unsigned,unsigned,void*);\n#ifdef __cplusplus\n}\n#endif\n")
    header.write_text(header.read_text()+'\n#include "jfg_original_timing_fast.h"\n')
    (root / "jfg_original_timing_fast.h").write_bytes((Path(__file__).resolve().parents[1] / "include/jfg/boot/original_timing_fast.h").read_bytes())
    (root / "jfg_original_timing.h").write_text("#pragma once\n#define JFG_ORIGINAL_TIMING_GENERATION 1\n")
    (root / "original-timing.json").write_text(json.dumps({"schema":1,"instruction_sites":sites,"paired_transfers":paired,"architecture":"original-os-cache-device-clock","game_data":"generated locally from the supplied ROM"},indent=2)+"\n")
