#!/usr/bin/env python3
"""Compare decode-only revisions on frozen encoded corpus files, without encoding.

Checked/trusted timings decode a complete asset into the same preallocated PCM
buffer. Parse-only validates every RGS frame and exact EOF without PCM work.
QOA uses the reference frame decoder with a caller-owned PCM buffer. Each native
trial performs one untimed warmup, then a batch lasting at least --min-ms.
"""
from __future__ import annotations

import argparse
from collections import defaultdict
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for block in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def cpu_affinity(cpu: int | None) -> list[int]:
    if sys.platform == "win32":
        import ctypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.GetCurrentProcess.restype = ctypes.c_void_p
        kernel.GetProcessAffinityMask.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_size_t)]
        kernel.SetProcessAffinityMask.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        process = kernel.GetCurrentProcess()
        allowed, system = ctypes.c_size_t(), ctypes.c_size_t()
        if not kernel.GetProcessAffinityMask(process, ctypes.byref(allowed), ctypes.byref(system)):
            raise OSError(ctypes.get_last_error(), "GetProcessAffinityMask failed")
        if cpu is not None:
            if cpu < 0 or cpu >= ctypes.sizeof(allowed) * 8 or not (system.value & (1 << cpu)):
                raise ValueError(f"CPU {cpu} unavailable")
            if not kernel.SetProcessAffinityMask(process, 1 << cpu):
                raise OSError(ctypes.get_last_error(), "SetProcessAffinityMask failed")
            allowed.value = 1 << cpu
        return [index for index in range(ctypes.sizeof(allowed) * 8) if allowed.value & (1 << index)]
    if hasattr(os, "sched_getaffinity"):
        if cpu is not None:
            os.sched_setaffinity(0, {cpu})
        return sorted(os.sched_getaffinity(0))
    if cpu is not None:
        raise ValueError("CPU affinity unsupported on this platform")
    return []


def cpu_name() -> str:
    if sys.platform == "win32":
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DESCRIPTION\System\CentralProcessor\0") as key:
            return winreg.QueryValueEx(key, "ProcessorNameString")[0].strip()
    return platform.processor() or platform.machine()


def invoke(executable: Path, path: Path, mode: str, minimum_ms: float) -> dict:
    command = [str(executable.resolve()), "--input", str(path.resolve()), "--mode", mode, "--min-ms", str(minimum_ms)]
    completed = subprocess.run(command, capture_output=True, text=True)
    if completed.returncode:
        raise RuntimeError(f"{executable.name} exited {completed.returncode}: {completed.stderr.strip() or completed.stdout.strip()}")
    return json.loads(completed.stdout)


def csv_write(path: Path, rows: list[dict]) -> None:
    if not rows:
        return
    fields = list(dict.fromkeys(key for row in rows for key, value in row.items() if not isinstance(value, (dict, list))))
    with path.open("w", encoding="utf-8", newline="") as file:
        writer = csv.DictWriter(file, fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def quantile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    at = (len(ordered) - 1) * fraction
    low = int(at)
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (at - low)


def summarize(rows: list[dict], required_trials: int) -> tuple[list[dict], list[dict], list[dict]]:
    groups = defaultdict(list)
    for row in rows:
        groups[(row["asset"], row["revision"], row["quality"], row["mode"])].append(row)
    files = []
    for group in groups.values():
        first = group[0]
        values = [row["mean_ms"] for row in group]
        item = {key: first[key] for key in ("asset", "category", "revision", "quality", "mode", "frames", "channels", "rate", "encoded_bytes", "encoded_sha256", "decoded_fnv64")}
        item.update(trials=len(group), complete=len(group) == required_trials,
                    median_ms=statistics.median(values), p95_ms=quantile(values, .95), p99_ms=quantile(values, .99),
                    duration_seconds=first["frames"] / first["rate"])
        item["ns_per_sample"] = item["median_ms"] * 1e6 / (item["frames"] * item["channels"])
        item["realtime_factor"] = item["duration_seconds"] * 1000 / item["median_ms"]
        files.append(item)
    by_asset = {(row["asset"], row["revision"], row["quality"], row["mode"]): row for row in files if row["complete"]}
    comparisons = []
    for row in files:
        if row["revision"] != "candidate" or not row["complete"]:
            continue
        before = by_asset.get((row["asset"], "baseline", row["quality"], row["mode"]))
        qoa = by_asset.get((row["asset"], "reference", "default", "qoa"))
        if before and qoa:
            comparisons.append({"asset": row["asset"], "category": row["category"], "quality": row["quality"], "mode": row["mode"],
                                "baseline_ms": before["median_ms"], "candidate_ms": row["median_ms"], "qoa_ms": qoa["median_ms"],
                                "speedup": before["median_ms"] / row["median_ms"],
                                "time_reduction": 1 - row["median_ms"] / before["median_ms"],
                                "candidate_over_qoa_time": row["median_ms"] / qoa["median_ms"],
                                "decoded_identical": None if row["mode"] == "parse" else before["decoded_fnv64"] == row["decoded_fnv64"]})
    totals = []
    for category in ["all"] + sorted({row["category"] for row in files}):
        for revision, quality, mode in sorted({(row["revision"], row["quality"], row["mode"]) for row in files}):
            selected = [row for row in files if row["complete"] and (category == "all" or row["category"] == category)
                        and (row["revision"], row["quality"], row["mode"]) == (revision, quality, mode)]
            if not selected:
                continue
            # QOA and before/after totals cover exactly the selected asset set.
            qoa_rows = [by_asset.get((row["asset"], "reference", "default", "qoa")) for row in selected]
            before_rows = [by_asset.get((row["asset"], "baseline", quality, mode)) for row in selected]
            duration = sum(row["duration_seconds"] for row in selected)
            milliseconds = sum(row["median_ms"] for row in selected)
            qoa_ms = sum(row["median_ms"] for row in qoa_rows if row) if all(qoa_rows) else None
            baseline_ms = sum(row["median_ms"] for row in before_rows if row) if all(before_rows) else None
            totals.append({"category": category, "revision": revision, "quality": quality, "mode": mode,
                           "assets": len(selected), "duration_seconds": duration, "sum_median_ms": milliseconds,
                           "realtime_factor": duration * 1000 / milliseconds,
                           "candidate_over_qoa_time": milliseconds / qoa_ms if qoa_ms is not None and revision == "candidate" else None,
                           "speedup": baseline_ms / milliseconds if baseline_ms is not None and revision == "candidate" else None})
    return files, comparisons, totals


def run(args: argparse.Namespace) -> int:
    if args.trials < 1 or not math.isfinite(args.min_ms) or not 0 <= args.min_ms <= 60000:
        raise ValueError("trials must be positive; min-ms must be finite in [0,60000]")
    affinity = cpu_affinity(args.cpu)
    source_results = args.corpus_results / "results.json"
    corpus = json.loads(source_results.read_text(encoding="utf-8"))
    if corpus.get("errors"):
        raise ValueError("Source corpus results contain errors")
    inputs = {}
    for row in corpus["per_file"]:
        if row["lane"] != "normalized" or not ((row["codec"] == "rgs" and row["revision"] == "candidate") or row["codec"] == "qoa"):
            continue
        key = (row["asset"], row["codec"], row["quality"])
        if key in inputs:
            raise ValueError(f"Duplicate source input: {key}")
        inputs[key] = row
    assets = sorted({key[0] for key in inputs})
    if args.asset:
        unknown = set(args.asset) - set(assets)
        if unknown:
            raise ValueError(f"Unknown corpus assets: {sorted(unknown)}")
        assets = list(dict.fromkeys(args.asset))
    if not assets:
        raise ValueError("No selected assets")
    modes = list(dict.fromkeys(args.mode or ["checked", "trusted", "parse"]))
    args.output.mkdir(parents=True, exist_ok=True)
    source_paths = {"candidate_executable": args.candidate, "baseline_executable": args.baseline,
                    "candidate_header": args.candidate_header, "baseline_header": args.baseline_header,
                    "qoa_header": ROOT / "third_party/qoa/qoa.h", "native_source": ROOT / "benchmarks/decode_profile.c",
                    "runner_source": Path(__file__), "corpus_results": source_results}
    provenance = {name: {"path": str(path.resolve()), "sha256": sha256(path)} for name, path in source_paths.items()}
    metadata = {"schema_version": 1, "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                "cpu": cpu_name(), "logical_cpus": os.cpu_count(), "cpu_affinity": affinity, "os": platform.platform(),
                "python": sys.version, "argv": sys.argv, "trials": args.trials, "minimum_batch_ms": args.min_ms,
                "assets": assets, "modes": modes, "hashes": provenance,
                "timing_scope": __doc__,
                "statistics": "Per-file percentiles describe trial batch means, not callback latency. Totals sum per-file medians. QOA runs once per asset/trial with the candidate executable; revision order alternates each trial.",
                "parse_note": "Parse-only is validation work, not a PCM decode competitor; its QOA time ratio is a cost attribution diagnostic.",
                "compiler": {}}
    if args.build_metadata:
        metadata["build"] = json.loads(args.build_metadata.read_text(encoding="utf-8"))
    metadata_path = args.output / "metadata.json"
    metadata_path.write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    rows, errors = [], []
    with (args.output / "raw.jsonl").open("w", encoding="utf-8") as raw:
        for index, asset in enumerate(assets):
            try:
                cases = []
                for quality in ("high", "medium", "low"):
                    reference = inputs[(asset, "rgs", quality)]
                    for mode in modes:
                        for revision, executable in (("baseline", args.baseline), ("candidate", args.candidate)):
                            cases.append((revision, executable, mode, quality, reference))
                cases.append(("reference", args.candidate, "qoa", "default", inputs[(asset, "qoa", "default")]))
                verified = {}
                asset_id = hashlib.sha256(asset.encode()).hexdigest()[:20]
                for _, _, mode, quality, reference in cases:
                    key = (reference["codec"], quality)
                    if key in verified:
                        continue
                    extension = ".qoa" if mode == "qoa" else ".rgs"
                    label = "reference_qoa" if mode == "qoa" else "candidate_rgs"
                    path = args.corpus_results / "listening" / f"{asset_id}_normalized_{label}_{quality}{extension}"
                    digest = sha256(path)
                    if digest != reference["encoded_sha256"]:
                        raise ValueError(f"Encoded file changed: {path}")
                    verified[key] = (path, digest)
                for trial in range(args.trials):
                    for revision, executable, mode, quality, reference in (cases if trial % 2 == 0 else list(reversed(cases))):
                        path, digest = verified[(reference["codec"], quality)]
                        row = invoke(executable, path, mode, args.min_ms)
                        if not math.isfinite(row["mean_ms"]) or row["mean_ms"] <= 0:
                            raise ValueError("Non-positive/nonfinite timing; increase --min-ms")
                        if row["mode"] != mode or (row["frames"], row["channels"], row["rate"], row["encoded_bytes"]) != (
                                reference["stored_frames"], reference["channels"], reference["stored_rate"], reference["encoded_bytes"]):
                            raise ValueError(f"Decoded description mismatch: {asset}, {revision}, {quality}, {mode}")
                        if row["encoded_fnv64"] != reference["encoded_fnv64"]:
                            raise ValueError(f"Encoded checksum mismatch: {asset}")
                        if mode != "parse" and row["decoded_fnv64"] != reference["decoded_fnv64"]:
                            raise ValueError(f"Decoded checksum mismatch: {asset}, {revision}, {quality}, {mode}")
                        if mode == "parse" and (row["decoded_fnv64"] is not None or row["output_buffer_bytes"] != 0):
                            raise ValueError("Parse-only mode unexpectedly produced PCM")
                        row.update(asset=asset, category=reference["category"], revision=revision, quality=quality,
                                   trial=trial, encoded_sha256=digest, expected_decoded_fnv64=reference["decoded_fnv64"])
                        metadata["compiler"][revision] = row["compiler"]
                        rows.append(row)
                        raw.write(json.dumps(row, allow_nan=False) + "\n"); raw.flush()
                print(f"[{index + 1}/{len(assets)}] {asset}", flush=True)
            except (KeyError, OSError, RuntimeError, ValueError) as error:
                errors.append({"asset": asset, "error": str(error)})
                print(f"ERROR {asset}: {error}", file=sys.stderr, flush=True)
    files, comparisons, totals = summarize(rows, args.trials)
    expected_rows = len(assets) * (6 * len(modes) + 1) * args.trials
    report = {"metadata": metadata, "expected_rows": expected_rows, "actual_rows": len(rows),
              "complete": not errors and len(rows) == expected_rows,
              "per_file": files, "comparisons": comparisons, "totals": totals, "errors": errors}
    metadata_path.write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    (args.output / "results.json").write_text(json.dumps(report, indent=2, allow_nan=False), encoding="utf-8")
    csv_write(args.output / "raw.csv", rows)
    csv_write(args.output / "per_file.csv", files)
    csv_write(args.output / "comparisons.csv", comparisons)
    csv_write(args.output / "totals.csv", totals)
    print(f"Saved {len(rows)}/{expected_rows} trials to {args.output}; {len(errors)} errors", flush=True)
    return 0 if report["complete"] else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--baseline-header", type=Path, default=ROOT / "benchmarks/baselines/rg_rgs_2026_09_26.h")
    parser.add_argument("--candidate-header", type=Path, default=ROOT / "src/rg_rgs.h")
    parser.add_argument("--corpus-results", type=Path, default=ROOT / "build/benchmark-results")
    parser.add_argument("--output", type=Path, default=ROOT / "build/decode-profile/results")
    parser.add_argument("--build-metadata", type=Path)
    parser.add_argument("--asset", action="append", help="Exact corpus relative path; repeat for a subset")
    parser.add_argument("--mode", choices=("checked", "trusted", "parse"), action="append", help="Repeat to choose RGS modes; QOA is always included")
    parser.add_argument("--trials", type=int, default=7)
    parser.add_argument("--min-ms", type=float, default=10)
    parser.add_argument("--cpu", type=int)
    return run(parser.parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
