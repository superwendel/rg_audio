#!/usr/bin/env python3
"""Evaluate an encoder change against the prepared, archived 151-file corpus.

Only RGS is encoded. Existing prepared PCM and old encoded/decoded exports are
verified and reused. One native trial (plus the adapter's untimed warmup) is the
default: timings are screening measurements, not a seven-trial performance
claim. Optional freshly built baseline trials allow paired encode comparisons.
Whole-file metrics and every 5120-sample frame/channel are examined separately.
All outputs, including listening WAVs and machine metadata, remain under build.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import platform
import statistics
import sys
import time

import numpy as np

from run_benchmarks import (ROOT, affinity, audio_dependencies, cpu_name, native,
                            quality_metrics, read_pcm, sha256, write_csv)

QUALITIES = ("high", "medium", "low")
FRAME_SAMPLES = 5120
THRESHOLDS = {
    "error_exceeds_signal_min_rms": 128,
    "clip_burst_min_unexpected_full_scale": 4,
    "clip_burst_max_snr_db": 10,
    "large_error_min_peak": 16384,
    "large_error_min_rms": 1024,
    "frame_regression_min_sse_ratio": 2,
    "frame_regression_min_rms": 128,
    "file_regression_min_snr_drop_db": 0.25,
}


def frame_diagnostics(reference: np.ndarray, decoded: np.ndarray) -> list[dict]:
    """Numerical screens, not listening scores or proof of predictor clipping.

    Channels are kept separate so a bad mono channel cannot hide behind a
    louder channel. Full-scale output alone does not prove arithmetic clipping.
    The final partial codec frame is included with its actual sample count.
    """
    if reference.ndim != 2 or reference.shape != decoded.shape or not len(reference):
        raise ValueError("Source and decoded timelines/channels must match exactly")
    starts = np.arange(0, len(reference), FRAME_SAMPLES)
    counts = np.minimum(FRAME_SAMPLES, len(reference) - starts)
    difference = reference.astype(np.float64, copy=False) - decoded
    signal = np.add.reduceat(reference.astype(np.float64, copy=False) ** 2, starts, axis=0)
    errors = np.add.reduceat(difference ** 2, starts, axis=0)
    peaks = np.maximum.reduceat(np.abs(difference), starts, axis=0)
    source_fs = (reference == -32768) | (reference == 32767)
    output_fs = (decoded == -32768) | (decoded == 32767)
    source_counts = np.add.reduceat(source_fs.astype(np.int64), starts, axis=0)
    output_counts = np.add.reduceat(output_fs.astype(np.int64), starts, axis=0)
    unexpected = np.add.reduceat((output_fs & ~source_fs).astype(np.int64), starts, axis=0)
    rows = []
    for index, (start, count) in enumerate(zip(starts, counts)):
        for channel in range(reference.shape[1]):
            power, error = float(signal[index, channel]), float(errors[index, channel])
            rms = math.sqrt(error / int(count))
            snr = 10 * math.log10(power / error) if power > 0 and error > 0 else None
            error_exceeds_signal = error > power and rms >= THRESHOLDS["error_exceeds_signal_min_rms"]
            clip_burst = (int(unexpected[index, channel]) >= THRESHOLDS["clip_burst_min_unexpected_full_scale"]
                          and source_counts[index, channel] == 0
                          and (power == 0 or snr is not None and snr < THRESHOLDS["clip_burst_max_snr_db"]))
            large_error = (peaks[index, channel] >= THRESHOLDS["large_error_min_peak"]
                           and rms >= THRESHOLDS["large_error_min_rms"])
            rows.append({"frame_index": index, "channel": channel, "start_sample": int(start),
                         "frames": int(count), "signal_squared_sum": power, "error_squared_sum": error,
                         "snr_db": snr, "error_rms": rms, "max_abs_error": int(peaks[index, channel]),
                         "source_full_scale": int(source_counts[index, channel]),
                         "decoded_full_scale": int(output_counts[index, channel]),
                         "unexpected_full_scale": int(unexpected[index, channel]),
                         "error_exceeds_signal": bool(error_exceeds_signal), "clip_burst": bool(clip_burst),
                         "large_error": bool(large_error),
                         "severe": bool(error_exceeds_signal or clip_burst or large_error)})
    return rows


def frame_pairs(before: list[dict], after: list[dict]) -> list[dict]:
    if len(before) != len(after):
        raise ValueError("Frame diagnostic coverage differs")
    pairs = []
    for old, new in zip(before, after):
        if any(old[key] != new[key] for key in ("frame_index", "channel", "start_sample", "frames")):
            raise ValueError("Frame diagnostic timelines differ")
        old_error, new_error = old["error_squared_sum"], new["error_squared_sum"]
        ratio = new_error / old_error if old_error else None
        regression = (new_error > old_error and new_error >= old_error * THRESHOLDS["frame_regression_min_sse_ratio"]
                      and new["error_rms"] >= THRESHOLDS["frame_regression_min_rms"])
        pairs.append({key: new[key] for key in ("frame_index", "channel", "start_sample", "frames")} |
                     {"baseline_sse": old_error, "candidate_sse": new_error, "sse_ratio": ratio,
                      "baseline_snr_db": old["snr_db"], "candidate_snr_db": new["snr_db"],
                      "baseline_error_rms": old["error_rms"], "candidate_error_rms": new["error_rms"],
                      "baseline_unexpected_full_scale": old["unexpected_full_scale"],
                      "candidate_unexpected_full_scale": new["unexpected_full_scale"],
                      "baseline_severe": old["severe"], "candidate_severe": new["severe"],
                      "introduced_severe": new["severe"] and not old["severe"],
                      "resolved_severe": old["severe"] and not new["severe"],
                      "error_regression": bool(regression)})
    return pairs


def check_native(row: dict, reference: np.ndarray, rate: int, quality: str) -> None:
    expected = (len(reference), rate, reference.shape[1])
    if row["codec"] != "rgs" or row["quality"] != quality or (
            row["stored_frames"], row["stored_rate"], row["channels"]) != expected or (
            row["source_frames"], row["source_rate"], row["channels"]) != expected:
        raise ValueError("Native codec/timeline description differs from prepared PCM")
    if row.get("decode_apis_agree") is not True:
        raise ValueError("Rebuild the adapter: checked/trusted/stream PCM agreement is required")
    if any(not math.isfinite(row[key]) or row[key] <= 0 for key in ("encode_ms", "decode_ms")):
        raise ValueError("Invalid native timing")


def check_signature(row: dict, expected: dict) -> None:
    fields = ("stored_frames", "stored_rate", "channels", "encoded_bytes", "encoded_fnv64", "decoded_fnv64")
    if any(row[key] != expected[key] for key in fields):
        raise ValueError("Encoded/decoded fingerprint or description changed")


def record(path: Path) -> dict:
    return {"path": str(path.resolve()), "sha256": sha256(path)}


def validate_build_metadata(build: dict, hashes: dict) -> None:
    """Tie selected source/executable bytes to the recorded native build."""
    recorded = build.get("sha256", {})
    if not isinstance(recorded, dict) or not recorded:
        raise ValueError("Build metadata must contain source/executable SHA256 records")
    paths = {str(Path(path).resolve() if Path(path).is_absolute() else (ROOT / path).resolve()): digest
             for path, digest in recorded.items()}
    required = ["candidate_executable", "candidate_header", "adapter_source"]
    if "baseline_executable" in hashes:
        required += ["baseline_executable", "baseline_header"]
    for name in required:
        source = hashes[name]
        observed = paths.get(str(Path(source["path"]).resolve()))
        # A byte-identical frozen header may have been relocated for publication.
        if observed is None and name.endswith("_header") and source["sha256"] in recorded.values():
            observed = source["sha256"]
        if observed != source["sha256"]:
            raise ValueError(f"Build metadata differs from selected {name}")


def compare_file(asset: str, quality: str, old: dict, metrics: dict,
                 diagnostics: list[dict], pairs: list[dict], trials: list[dict]) -> dict:
    candidate = [row for row in trials if row["revision"] == "candidate"]
    baseline = [row for row in trials if row["revision"] == "baseline"]
    after_snr, before_snr = metrics["snr_db"], old["quality_metrics"]["snr_db"]
    before_error, after_error = sum(row["baseline_sse"] for row in pairs), sum(row["candidate_sse"] for row in pairs)
    snr_change = after_snr - before_snr if after_snr is not None and before_snr is not None else None
    candidate_ms = statistics.median(row["encode_ms"] for row in candidate)
    baseline_ms = statistics.median(row["encode_ms"] for row in baseline) if baseline else None
    return {"asset": asset, "category": old["category"], "quality": quality, "trials": len(candidate),
            "frames": old["stored_frames"], "channels": old["channels"], "rate": old["stored_rate"],
            "baseline_encoded_bytes": old["encoded_bytes"], "candidate_encoded_bytes": candidate[0]["encoded_bytes"],
            "encoded_size_ratio": candidate[0]["encoded_bytes"] / old["encoded_bytes"],
            "baseline_snr_db": before_snr, "candidate_snr_db": after_snr, "snr_change_db": snr_change,
            "file_quality_regression": (snr_change is not None and snr_change <= -THRESHOLDS["file_regression_min_snr_drop_db"])
                                       or (before_error == 0 and after_error > 0),
            "baseline_sse": before_error, "candidate_sse": after_error,
            "candidate_segmental_snr_db": metrics["segmental_snr_db"],
            "candidate_log_spectral_rms_db": metrics["log_spectral_rms_db"],
            "candidate_max_abs_error": metrics["max_abs_error"],
            "baseline_unexpected_full_scale": sum(row["baseline_unexpected_full_scale"] for row in pairs),
            "candidate_unexpected_full_scale": sum(row["unexpected_full_scale"] for row in diagnostics),
            "baseline_severe_frames": sum(row["baseline_severe"] for row in pairs),
            "candidate_severe_frames": sum(row["candidate_severe"] for row in pairs),
            "introduced_severe_frames": sum(row["introduced_severe"] for row in pairs),
            "resolved_severe_frames": sum(row["resolved_severe"] for row in pairs),
            "frame_error_regressions": sum(row["error_regression"] for row in pairs),
            "archived_encode_median_ms": old["encode_median_ms"],
            "fresh_baseline_encode_median_ms": baseline_ms,
            "candidate_encode_median_ms": candidate_ms,
            "paired_encode_time_ratio": candidate_ms / baseline_ms if baseline_ms else None,
            "candidate_decode_median_ms": statistics.median(row["decode_ms"] for row in candidate),
            "decode_apis_agree": all(row["decode_apis_agree"] for row in trials)}


def summarize(files: list[dict]) -> list[dict]:
    rows = []
    for quality in QUALITIES:
        selected = [row for row in files if row["quality"] == quality]
        if not selected:
            continue
        row = {"quality": quality, "assets": len(selected)}
        for field in ("baseline_encoded_bytes", "candidate_encoded_bytes", "baseline_sse", "candidate_sse",
                      "baseline_severe_frames", "candidate_severe_frames", "introduced_severe_frames",
                      "resolved_severe_frames", "frame_error_regressions", "candidate_encode_median_ms"):
            row[field] = sum(item[field] for item in selected)
        row["file_quality_regressions"] = sum(item["file_quality_regression"] for item in selected)
        row["encoded_size_ratio"] = row["candidate_encoded_bytes"] / row["baseline_encoded_bytes"]
        row["fresh_baseline_encode_median_ms"] = (sum(item["fresh_baseline_encode_median_ms"] for item in selected)
            if all(item["fresh_baseline_encode_median_ms"] is not None for item in selected) else None)
        row["paired_encode_time_ratio"] = (row["candidate_encode_median_ms"] / row["fresh_baseline_encode_median_ms"]
            if row["fresh_baseline_encode_median_ms"] else None)
        row["median_snr_change_db"] = statistics.median(item["snr_change_db"] for item in selected if item["snr_change_db"] is not None) if any(item["snr_change_db"] is not None for item in selected) else None
        rows.append(row)
    return rows


def run(args: argparse.Namespace) -> int:
    if args.trials < 1 or not math.isfinite(args.min_ms) or not 0 <= args.min_ms <= 60000:
        raise ValueError("trials must be positive and min-ms finite in [0,60000]")
    if bool(args.baseline) != bool(args.baseline_header):
        raise ValueError("Supply both --baseline and --baseline-header, or neither")
    if (args.output / "raw.jsonl").exists() or (args.output / "results.json").exists():
        raise ValueError("Output already contains a run; choose a new output directory")
    if (ROOT / "docs").resolve() == args.output.resolve() or (ROOT / "docs").resolve() in args.output.resolve().parents:
        raise ValueError("Quality review outputs belong in local build storage, not docs")
    cpu_affinity = affinity(args.cpu)
    report_path = args.archive / "results.json"
    archived = json.loads(report_path.read_text(encoding="utf-8"))
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    if archived["errors"] or sha256(args.manifest) != archived["metadata"]["hashes"]["manifest"]["sha256"]:
        raise ValueError("Archived run has errors or its corpus manifest changed")
    old = {(row["asset"], row["quality"]): row for row in archived["per_file"]
           if row["codec"] == "rgs" and row["revision"] == "candidate" and row["lane"] == "normalized"}
    preparations = {row["asset"]: row for row in archived["preparation"]}
    all_assets = [row["relative_path"] for row in manifest["samples"]]
    assets = list(dict.fromkeys(args.asset or all_assets))
    if not assets or set(assets) - set(all_assets):
        raise ValueError("Unknown or empty asset selection")
    qualities = list(dict.fromkeys(args.quality or QUALITIES))
    sources = {"candidate_executable": args.candidate, "candidate_header": args.candidate_header,
               "adapter_source": ROOT / "benchmarks/codec_adapter.c", "runner_source": Path(__file__),
               "metric_source": ROOT / "benchmarks/run_benchmarks.py", "archive": report_path,
               "manifest": args.manifest}
    if args.baseline:
        sources |= {"baseline_executable": args.baseline, "baseline_header": args.baseline_header}
    meta = {"schema_version": 1, "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "cpu": cpu_name(), "cpu_affinity": cpu_affinity, "os": platform.platform(), "argv": sys.argv,
            "trials": args.trials, "minimum_batch_ms": args.min_ms, "assets": assets, "qualities": qualities,
            "hashes": {key: record(path) for key, path in sources.items()},
            "libraries": {str(path): sha256(path) for path in (audio_dependencies() / "bin").glob("*.dll")},
            "thresholds": THRESHOLDS,
            "method": __doc__,
            "timing_note": "Native wall-clock encode CPU work excludes preparation, I/O, metrics, and checked/trusted/stream agreement. One warmup per invocation; trials alternate revisions. These are screening timings, not a seven-trial performance claim. Archived timings are context only; paired ratios require fresh baseline trials.",
            "quality_note": "SNR and frame thresholds are numerical screens, not perceptual guarantees. Full-scale counts are not a count of internal predictor saturation. Every codec frame/channel including partial tails is reported; regressions require review, not automatic acceptance/rejection."}
    if args.build_metadata:
        meta["build"] = json.loads(args.build_metadata.read_text(encoding="utf-8-sig"))
        validate_build_metadata(meta["build"], meta["hashes"])
    args.output.mkdir(parents=True, exist_ok=True)
    listening = args.output / "listening"
    listening.mkdir(exist_ok=True)
    (args.output / "metadata.json").write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")
    files, errors, raw_rows = [], [], []
    with (args.output / "raw.jsonl").open("w", encoding="utf-8") as raw, \
            (args.output / "per_frame.csv").open("w", newline="", encoding="utf-8") as frame_file, \
            (args.output / "frame_comparisons.csv").open("w", newline="", encoding="utf-8") as pair_file:
        frame_writer = pair_writer = None
        for asset_index, asset in enumerate(assets):
            try:
                asset_id = hashlib.sha256(asset.encode()).hexdigest()[:20]
                prepared = args.archive / "prepared" / (asset_id + ".wav")
                if sha256(prepared) != preparations[asset]["sha256"]:
                    raise ValueError("Prepared PCM differs from archived input")
                reference, rate = read_pcm(prepared)
                if (len(reference), rate, reference.shape[1]) != tuple(preparations[asset][key] for key in ("frames", "rate", "channels")):
                    raise ValueError("Prepared PCM description differs from archive")
                for quality in qualities:
                    previous = old[asset, quality]
                    archived_name = f"{asset_id}_normalized_candidate_rgs_{quality}"
                    previous_encoded = args.archive / "listening" / (archived_name + ".rgs")
                    previous_wav = previous_encoded.with_suffix(".wav")
                    if sha256(previous_encoded) != previous["encoded_sha256"] or sha256(previous_wav) != previous["decoded_sha256"]:
                        raise ValueError("Archived encoded/decoded bytes changed")
                    before, before_rate = read_pcm(previous_wav)
                    if before_rate != rate or before.shape != reference.shape:
                        raise ValueError("Archived decoded timeline differs from prepared PCM")
                    baseline_frames = frame_diagnostics(reference, before)
                    del before
                    case_rows = []
                    signatures = {}
                    encoded = listening / (asset_id + "_" + quality + ".rgs")
                    decoded = encoded.with_suffix(".wav")
                    for trial in range(args.trials):
                        cases = [("candidate", args.candidate)]
                        if args.baseline:
                            cases.insert(0, ("baseline", args.baseline))
                        if (asset_index + trial) % 2:
                            cases.reverse()
                        for revision, executable in cases:
                            command = ["--input", prepared, "--codec", "rgs", "--quality", quality, "--min-ms", args.min_ms]
                            if revision == "candidate" and trial == 0:
                                command += ["--encoded", encoded, "--decoded", decoded]
                            row = native(executable, *command)
                            check_native(row, reference, rate, quality)
                            if revision == "baseline":
                                check_signature(row, previous)
                            if revision in signatures:
                                check_signature(row, signatures[revision])
                            signatures[revision] = row.copy()
                            row.update(asset=asset, category=previous["category"], revision=revision, trial=trial,
                                       prepared_sha256=preparations[asset]["sha256"])
                            raw_rows.append(row); case_rows.append(row)
                            raw.write(json.dumps(row, allow_nan=False) + "\n"); raw.flush()
                    after, after_rate = read_pcm(decoded)
                    if after_rate != rate or after.shape != reference.shape:
                        raise ValueError("Candidate decoded timeline differs from prepared PCM")
                    metrics = quality_metrics(reference, after, rate)
                    candidate_frames = frame_diagnostics(reference, after)
                    del after
                    pairs = frame_pairs(baseline_frames, candidate_frames)
                    identifiers = {"asset": asset, "category": previous["category"], "quality": quality}
                    for revision, rows in (("baseline", baseline_frames), ("candidate", candidate_frames)):
                        rows = [identifiers | {"revision": revision} | row for row in rows]
                        if frame_writer is None:
                            frame_writer = csv.DictWriter(frame_file, list(rows[0])); frame_writer.writeheader()
                        frame_writer.writerows(rows)
                    paired_rows = [identifiers | row for row in pairs]
                    if pair_writer is None:
                        pair_writer = csv.DictWriter(pair_file, list(paired_rows[0])); pair_writer.writeheader()
                    pair_writer.writerows(paired_rows)
                    frame_file.flush(); pair_file.flush()
                    result = compare_file(asset, quality, previous, metrics, candidate_frames, pairs, case_rows)
                    result.update(encoded_sha256=sha256(encoded), decoded_sha256=sha256(decoded),
                                  encoded_fnv64=signatures["candidate"]["encoded_fnv64"],
                                  decoded_fnv64=signatures["candidate"]["decoded_fnv64"],
                                  listening_wav=str(decoded.relative_to(args.output)))
                    files.append(result)
                    write_csv(args.output / "per_file.csv", files)
                print(f"[{asset_index + 1}/{len(assets)}] {asset}", flush=True)
            except (OSError, RuntimeError, ValueError, KeyError) as error:
                errors.append({"asset": asset, "error": str(error)})
                print(f"ERROR {asset}: {error}", file=sys.stderr, flush=True)
    for key, path in sources.items():
        if sha256(path) != meta["hashes"][key]["sha256"]:
            errors.append({"asset": None, "error": f"Measured source/executable changed during run: {key}"})
    totals = summarize(files)
    expected_rows = len(assets) * len(qualities) * args.trials * (2 if args.baseline else 1)
    complete = not errors and len(files) == len(assets) * len(qualities) and len(raw_rows) == expected_rows
    report = {"metadata": meta, "complete": complete, "expected_trials": expected_rows,
              "actual_trials": len(raw_rows), "per_file": files, "totals": totals, "errors": errors}
    (args.output / "results.json").write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    write_csv(args.output / "totals.csv", totals)
    write_csv(args.output / "regressions.csv", [row for row in files if row["file_quality_regression"] or row["introduced_severe_frames"] or row["frame_error_regressions"]])
    print(json.dumps({"complete": complete, "assets": len(assets), "cases": len(files), "errors": errors, "totals": totals}, indent=2), flush=True)
    return 0 if complete else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, default=ROOT / "build/benchmark-results")
    parser.add_argument("--manifest", type=Path, default=ROOT / "benchmarks/corpus_manifest.json")
    parser.add_argument("--candidate", type=Path, default=ROOT / "build/quality-review/codec_candidate.exe")
    parser.add_argument("--candidate-header", type=Path, default=ROOT / "src/rg_rgs.h")
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--baseline-header", type=Path)
    parser.add_argument("--build-metadata", type=Path)
    parser.add_argument("--output", type=Path, default=ROOT / "build/quality-review/full-results")
    parser.add_argument("--asset", action="append", help="Exact corpus relative path; repeat to select a subset")
    parser.add_argument("--quality", action="append", choices=QUALITIES)
    parser.add_argument("--trials", type=int, default=1)
    parser.add_argument("--min-ms", type=float, default=0)
    parser.add_argument("--cpu", type=int)
    return run(parser.parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
