#!/usr/bin/env python3
"""Measure SDL's conversion of prepared PCM to 48 kHz without an audio device."""
import argparse
import json
from pathlib import Path
import platform
import statistics
import sys
import time

from run_benchmarks import ROOT, affinity, cpu_name, native, sha256, write_csv


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, default=ROOT / "build/bench/playback_resample.exe")
    parser.add_argument("--input", type=Path, action="append", required=True)
    parser.add_argument("--output", type=Path, default=ROOT / "build/playback-results")
    parser.add_argument("--sdl-library", type=Path)
    parser.add_argument("--trials", type=int, default=7)
    parser.add_argument("--cpu", type=int)
    args = parser.parse_args()
    if not 1 <= args.trials <= 1000:
        parser.error("trials must be between 1 and 1000")
    if args.sdl_library and sys.platform == "win32":
        staged = args.executable.resolve().parent / "SDL3.dll"
        if not staged.exists() or sha256(staged) != sha256(args.sdl_library):
            parser.error("Stage the selected SDL3.dll beside the executable; PATH can lose to a system DLL")
    affinity(args.cpu)
    args.output.mkdir(parents=True, exist_ok=True)
    results, rows, summaries = [], [], []
    for source in args.input:
        for block in (128, 256, 512):
            result = native(args.executable, source, block, args.trials)
            result.update(input=str(source), input_sha256=sha256(source))
            results.append(result)
            group = result["trials"]
            common = {key: result[key] for key in ("input", "input_sha256", "source_rate", "device_rate",
                                                   "source_frames", "channels", "input_block_frames")}
            rows.extend(common | row for row in group)
            summary = common | {"trials": len(group), "duration_seconds": result["source_frames"] / result["source_rate"]}
            for key in ("init_ms", "put_ms", "get_ms", "flush_ms", "conversion_total_ms", "first_output_input_frames"):
                summary[key + "_median"] = statistics.median(row[key] for row in group)
            summaries.append(summary)
    sources = [args.executable, Path(__file__), ROOT / "benchmarks/playback_resample.c"]
    if args.sdl_library:
        sources.append(args.sdl_library)
    metadata = {"timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                "cpu": cpu_name(), "cpu_affinity": affinity(None), "os": platform.platform(),
                "hashes": {str(path): sha256(path) for path in sources},
                "scope": __doc__, "results": results}
    (args.output / "results.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    write_csv(args.output / "raw.csv", rows)
    write_csv(args.output / "summary.csv", summaries)
    print(f"Saved {len(rows)} trials across {len(summaries)} conversion cases to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
