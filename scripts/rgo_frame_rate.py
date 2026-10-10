#!/usr/bin/env python3
"""Summarize an RGO frame trace (JFG_RGO_FRAME_TRACE) into game logic rates.

Each trace row is one game frame, written when viFrameSync starts it. The
summary reports, per segment of the run (front-end mode and level):

* game frames per emulated second (rows per 60 VIs): the real logic rate;
* the ticks (VIs) each frame was simulated with, as a histogram;
* game time covered (sum of ticks / 60) against emulated time, which must
  match or the game runs slow or fast;
* emulated seconds per wall second: below 1.0 the host could not keep up
  and the whole game ran in slow motion;
* wall-clock frame times (min / median / max) between frame starts.

See docs/rgo/framerate.md ("Headless measurement").
"""
from __future__ import annotations

import argparse
import csv
import json
import statistics
from pathlib import Path


def load(path: Path) -> list[dict[str, int]]:
    with path.open(newline="") as stream:
        return [{key: int(value) for key, value in row.items()}
                for row in csv.DictReader(stream, delimiter="\t")]


def summarize(rows: list[dict[str, int]]) -> dict:
    if len(rows) < 2:
        return {"frames": len(rows)}
    first, last = rows[0], rows[-1]
    vis = last["vi"] - first["vi"]
    wall = (last["wall_us"] - first["wall_us"]) / 1e6
    # Row i's ticks belong to the frame that ended at row i.
    ticks = [row["ticks"] for row in rows[1:]]
    histogram: dict[int, int] = {}
    for tick in ticks:
        histogram[tick] = histogram.get(tick, 0) + 1
    frame_wall_ms = [(b["wall_us"] - a["wall_us"]) / 1000 for a, b in zip(rows, rows[1:])]
    frame_vis = [b["vi"] - a["vi"] for a, b in zip(rows, rows[1:])]
    emulated = vis / 60
    return {
        "frames": len(rows) - 1,
        "vis": vis,
        "emulated_seconds": round(emulated, 3),
        "wall_seconds": round(wall, 3),
        "game_frames_per_emulated_second": round((len(rows) - 1) / emulated, 2) if vis else None,
        "game_frames_per_wall_second": round((len(rows) - 1) / wall, 2) if wall else None,
        "emulated_per_wall": round(emulated / wall, 3) if wall else None,
        "game_time_over_emulated_time": round(sum(ticks) / vis, 3) if vis else None,
        "ticks_histogram": dict(sorted(histogram.items())),
        "vis_per_frame_histogram": {k: frame_vis.count(k) for k in sorted(set(frame_vis))},
        "frame_wall_ms": {
            "min": round(min(frame_wall_ms), 2),
            "median": round(statistics.median(frame_wall_ms), 2),
            "p95": round(sorted(frame_wall_ms)[int(len(frame_wall_ms) * 0.95)], 2),
            "max": round(max(frame_wall_ms), 2),
        },
        "guest_instructions_per_emulated_second": round(
            (last["instructions"] - first["instructions"]) / emulated) if vis else None,
    }


def segments(rows: list[dict[str, int]]) -> list[tuple[str, list[dict[str, int]]]]:
    out: list[tuple[str, list[dict[str, int]]]] = []
    for row in rows:
        key = f"mode {row['front_mode']} level {row['level']}"
        if out and out[-1][0] == key:
            out[-1][1].append(row)
        else:
            out.append((key, [row]))
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--min-frames", type=int, default=30,
                        help="hide segments shorter than this many frames")
    parser.add_argument("--from-vi", type=int, default=0,
                        help="ignore frames before this VI (boot and loading)")
    args = parser.parse_args()
    rows = [row for row in load(args.trace) if row["vi"] >= args.from_vi]
    report = {"whole_run": summarize(rows), "segments": []}
    for key, segment in segments(rows):
        if len(segment) >= args.min_frames:
            report["segments"].append({"segment": key, "first_vi": segment[0]["vi"],
                                       **summarize(segment)})
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
