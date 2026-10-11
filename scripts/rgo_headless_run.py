#!/usr/bin/env python3
"""Run the headless Linux build through a new game and measure the logic rate.

The headless build (CMake option JFG_HEADLESS_LIVE, docs/rgo/framerate.md,
"Headless measurement") runs the live runtime without a window or GPU. This
script starts it with a frame-rate setting, lets the autopilot start a new
game (title, name keyboard, the opening cutscenes, the first level), stops
after a number of VIs and summarizes the frame trace with rgo_frame_rate.py.

The ROM and everything this writes stay outside the repository.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rgo_frame_rate  # noqa: E402

MODES = {
    # name: (framerate, n64_slowdown, interpolate_frames)
    "original-slowdown": ("original", "on", "on"),
    "original": ("original", "off", "on"),
    "true60": ("60", "off", "on"),
    "display": ("display", "off", "on"),
    "unlocked": ("unlocked", "off", "on"),
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--executable", type=Path, required=True,
                        help="headless jfg-native-boot (JFG_HEADLESS_LIVE build)")
    parser.add_argument("--rom", type=Path, required=True)
    parser.add_argument("--mode", choices=sorted(MODES), required=True)
    parser.add_argument("--vis", type=int, default=15000, help="stop after this many VIs")
    parser.add_argument("--out", type=Path, required=True, help="private output directory")
    parser.add_argument("--timeout", type=int, default=3600)
    parser.add_argument("--switch", action="append", default=[], metavar="VI:MODE",
                        help="rewrite video.ini to MODE once the game reaches VI "
                             "(repeatable), to check that settings apply live")
    parser.add_argument("--deterministic", action="store_true",
                        help="turn off the fast clock's real-time floor, so the "
                             "logic rate is measured in emulated time on any host")
    parser.add_argument("--unpaced", action="store_true",
                        help="with --deterministic: do not wait for the wall clock at "
                             "each VI, so emulated_per_wall measures host speed "
                             "(above 1.0: headroom for real time)")
    args = parser.parse_args()
    if args.unpaced and not args.deterministic:
        parser.error("--unpaced needs --deterministic")

    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    video = out / "video.ini"

    def write_video(mode: str) -> None:
        framerate, slowdown, interpolate = MODES[mode]
        # Replace atomically: the game rereads the file while it runs.
        temporary = video.with_suffix(".tmp")
        temporary.write_text(f"version=2\naspect=original\nframerate={framerate}\n"
                             f"interpolate_frames={interpolate}\nn64_slowdown={slowdown}\n")
        temporary.replace(video)

    write_video(args.mode)
    switches = sorted((int(vi), mode) for vi, mode in (item.split(":") for item in args.switch))
    for _, mode in switches:
        if mode not in MODES:
            parser.error(f"unknown mode {mode}")
    environment = {**os.environ,
                   "SDL_AUDIODRIVER": os.environ.get("SDL_AUDIODRIVER", "dummy"),
                   "JFG_VIDEO_CONFIG": str(video),
                   "JFG_RGO_AUTOPILOT": "newgame",
                   "JFG_RGO_EXIT_AFTER_VI": str(args.vis),
                   "JFG_RGO_FRAME_TRACE": str(out / "frames.tsv"),
                   "JFG_RGO_COUNTER_LOG": "1",
                   # Exact real-time audio sink: SDL's dummy driver plays a few
                   # percent too fast or slow, which shows as false underruns.
                   "JFG_RGO_WALL_AUDIO": os.environ.get("JFG_RGO_WALL_AUDIO", "1")}
    if args.deterministic:
        environment["JFG_RGO_DETERMINISTIC_CLOCK"] = "1"
    if args.unpaced:
        environment["JFG_RGO_UNPACED"] = "1"
    started = time.monotonic()
    trace = out / "frames.tsv"
    trace.unlink(missing_ok=True)
    with (out / "stdout.txt").open("w") as stdout, (out / "stderr.txt").open("w") as stderr:
        process = subprocess.Popen(
            [str(args.executable), "--rom", str(args.rom), "--save", str(out / "jfg.flash"),
             "--controller-pak", str(out / "controller-1.pak"), "--play"],
            env=environment, stdout=stdout, stderr=stderr)
        applied = []
        while process.poll() is None:
            if time.monotonic() - started > args.timeout:
                process.kill()
                break
            if switches:
                lines = trace.read_text().splitlines() if trace.exists() else []
                current = int(lines[-1].split("\t")[0]) if len(lines) > 1 else 0
                if current >= switches[0][0]:
                    vi, mode = switches.pop(0)
                    write_video(mode)
                    applied.append({"requested_vi": vi, "applied_after_vi": current, "mode": mode})
            time.sleep(0.5)
    wall = time.monotonic() - started
    rows = rgo_frame_rate.load(out / "frames.tsv")
    output = (out / "stdout.txt").read_text()
    report = {
        "mode": args.mode,
        "deterministic": args.deterministic,
        "unpaced": args.unpaced,
        "switches": applied,
        "exit_code": process.returncode,
        # A completed run ends at the VI target; anything else is a failure
        # (a fail-closed trap prints a jfg-phase6-native-first-trap ledger).
        "reached_vi_target": '"status":"retrace-target"' in output,
        "trap": next((line for line in output.splitlines() if "first-trap" in line), None),
        "wall_seconds": round(wall, 1),
        "whole_run": rgo_frame_rate.summarize(rows),
        "segments": [{"segment": key, "first_vi": segment[0]["vi"],
                      **rgo_frame_rate.summarize(segment)}
                     for key, segment in rgo_frame_rate.segments(rows) if len(segment) >= 30],
    }
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return 0 if report["reached_vi_target"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
