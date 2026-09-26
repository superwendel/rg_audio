#!/usr/bin/env python3
"""Collect local tables for two streaming runs and SDL conversion trials."""
import argparse
from collections import defaultdict
import gzip
import json
from pathlib import Path
import shutil

from run_benchmarks import ROOT, sha256


def copy_stream(source, destination):
    report = json.loads((source / "results.json").read_text())
    rows = [json.loads(line) for line in (source / "raw.jsonl").read_text().splitlines()]
    groups = defaultdict(list)
    for row in rows:
        groups[(row["voices"], row["callback_frames"], row["scenario"], row["revision"])].append(row)
    keys = {(v, c, s, r) for v in (1, 16, 64, 128) for c in (128, 256, 512)
            for s in ("steady", "start", "loop") for r in ("baseline", "candidate")}
    if set(groups) != keys or report["trials"] != 7:
        raise ValueError(f"Incomplete streaming cases in {source}")
    for (voices, callback, scenario, revision), group in groups.items():
        if len(group) != 7 or {row["trial"] for row in group} != set(range(7)):
            raise ValueError("Incomplete or duplicate streaming trials")
        other = groups[(voices, callback, scenario, "candidate" if revision == "baseline" else "baseline")]
        signatures = {(row["checksum"], row["consumed_frames"], row["decoded_frames"]) for row in group + other}
        if len(signatures) != 1 or any(row["steady_state_allocations"] for row in group):
            raise ValueError("Streaming output mismatch or measured allocation")
    destination.mkdir(parents=True, exist_ok=True)
    for name in ("results.json", "summary.csv"):
        shutil.copyfile(source / name, destination / name)
    with (source / "raw.jsonl").open("rb") as data:
        with gzip.GzipFile(filename=str(destination / "raw.jsonl.gz"), mode="wb", mtime=0) as output:
            shutil.copyfileobj(data, output)
    return len(rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", type=Path, default=ROOT / "build/published-results/corpus")
    parser.add_argument("--build-metadata", type=Path, default=ROOT / "build/bench/build_metadata.json")
    args = parser.parse_args()
    total = 0
    for name in ("footstep", "rain"):
        total += copy_stream(ROOT / ("build/stream-results-" + name), args.destination / ("stream-" + name))
    source = ROOT / "build/playback-results"
    playback = json.loads((source / "results.json").read_text())
    if len(playback["results"]) != 6:
        raise ValueError("Expected two playback inputs and three block sizes")
    for case in playback["results"]:
        group = case["trials"]
        if len(group) != 7 or {row["trial"] for row in group} != set(range(7)):
            raise ValueError("Incomplete playback trials")
        if len({(row["output_frames"], row["checksum"]) for row in group}) != 1:
            raise ValueError("Nondeterministic SDL conversion")
    destination = args.destination / "playback"
    destination.mkdir(parents=True, exist_ok=True)
    for name in ("results.json", "raw.csv", "summary.csv"):
        shutil.copyfile(source / name, destination / name)
    paths = ["benchmarks/stream_workload.c", "benchmarks/run_stream_workloads.py",
             "benchmarks/playback_resample.c", "benchmarks/run_playback_resample.py",
             "src/rg_rgs.h", "build/baseline/src/rg_rgs.h",
             "build/bench/stream_candidate.exe", "build/bench/stream_baseline.exe",
             "build/bench/playback_resample.exe"]
    build = json.loads(args.build_metadata.read_text(encoding="utf-8-sig"))
    provenance = {"compiler": build["compiler"], "flags": build["flags"],
                  "source_hash_note": "Source hashes captured during archival; preserve them with the measured executable hashes.",
                  "sha256": {path: sha256(ROOT / path) for path in paths},
                  "stream_inputs": {"footstep": "oculus_audio_pack/footsteps_shoe_concrete_run_04.wav",
                                    "rain": "oculus_audio_pack/ambient_city_rain_lp.wav"},
                  "quality": "medium", "trials_per_case": 7, "stream_trials": total, "playback_trials": 42}
    (args.destination / "workload_build.json").write_text(json.dumps(provenance, indent=2), encoding="utf-8")
    print(f"Published {total} stream trials and 42 playback conversion trials")


if __name__ == "__main__":
    main()
