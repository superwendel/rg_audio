#!/usr/bin/env python3
"""Reproducible native codec trials; imports, metric work and file I/O are untimed.

Requires NumPy. Build codec_adapter.c for the candidate and frozen baseline.
The manifest owns corpus provenance; this runner verifies every source SHA256.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import shutil
import statistics
import subprocess
import sys
import time
import wave

import numpy as np

ROOT = Path(__file__).resolve().parents[1]


def audio_dependencies() -> Path:
    return Path(os.environ.get("RG_AUDIO_DEPS_DIR", str(ROOT / "build/deps/install"))).resolve()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for block in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def native(executable: Path, *arguments: object) -> dict:
    environment = os.environ.copy()
    environment["PATH"] = str(audio_dependencies() / "bin") + os.pathsep + environment.get("PATH", "")
    result = subprocess.run([str(executable.resolve()), *map(str, arguments)],
                            capture_output=True, text=True, check=False, env=environment)
    if result.returncode:
        raise RuntimeError(f"{executable.name} exited {result.returncode}: {result.stderr.strip() or result.stdout.strip()}")
    return json.loads(result.stdout.strip())


def read_pcm(path: Path) -> tuple[np.ndarray, int]:
    with wave.open(str(path), "rb") as file:
        if file.getsampwidth() != 2 or file.getcomptype() != "NONE":
            raise ValueError(f"Expected PCM16 WAV: {path}")
        samples = np.frombuffer(file.readframes(file.getnframes()), dtype="<i2")
        return samples.reshape(-1, file.getnchannels()).astype(np.float64), file.getframerate()


def quality_metrics(reference: np.ndarray, decoded: np.ndarray, rate: int) -> dict:
    """Diagnostic errors, not perceptual quality scores. No signal alignment search."""
    if reference.ndim != 2 or decoded.ndim != 2 or reference.shape[1] != decoded.shape[1]:
        raise ValueError("Channel mismatch")
    count = min(len(reference), len(decoded))
    if not count:
        raise ValueError("Empty decoded audio")
    a, b = reference[:count], decoded[:count]
    delta = a - b
    error = float(np.sum(delta * delta))
    signal = float(np.sum(a * a))
    exact = error == 0 and len(reference) == len(decoded)
    snr = 10 * np.log10(signal / error) if signal > 0 and error > 0 else None
    window = max(1, int(rate * 0.02))
    segments = count // window
    segmental = None
    if segments:
        reference_blocks = a[:segments * window].reshape(segments, -1)
        error_blocks = delta[:segments * window].reshape(segments, -1)
        powers = np.sum(reference_blocks * reference_blocks, axis=1)
        errors = np.sum(error_blocks * error_blocks, axis=1)
        audible = powers > window * a.shape[1]  # Exclude digital silence / sub-LSB energy.
        if np.any(audible):
            scores = 10 * np.log10(powers[audible] / np.maximum(errors[audible], 1e-30))
            segmental = float(np.mean(np.clip(scores, -10, 60)))
    # RMS log-magnitude spectral error, normalized to full scale; fixed floor avoids log(0).
    fft_size = min(2048, count)
    hann = np.hanning(fft_size)
    spectral_sum = 0.0
    spectral_count = 0
    if fft_size >= 4:
        for start in range(0, count - fft_size + 1, max(1, fft_size // 2)):
            x = np.abs(np.fft.rfft(a[start:start + fft_size] * hann[:, None], axis=0)) / (32768 * fft_size)
            y = np.abs(np.fft.rfft(b[start:start + fft_size] * hann[:, None], axis=0)) / (32768 * fft_size)
            difference = 20 * np.log10(np.maximum(x, 1e-7)) - 20 * np.log10(np.maximum(y, 1e-7))
            spectral_sum += float(np.sum(difference * difference))
            spectral_count += difference.size
    return {"snr_db": float(snr) if snr is not None else None,
            "segmental_snr_db": segmental,
            "log_spectral_rms_db": (spectral_sum / spectral_count) ** 0.5 if spectral_count else None,
            "exact_pcm": exact, "compared_frames": count,
            "reference_frames": len(reference), "decoded_frames": len(decoded),
            "max_abs_error": float(np.max(np.abs(delta)))}


def percentile(values: list[float], fraction: float) -> float:
    return float(np.quantile(values, fraction))


def aggregate(rows: list[dict]) -> list[dict]:
    groups: dict[tuple, list[dict]] = {}
    for row in rows:
        key = (row["asset"], row["lane"], row["revision"], row["codec"], row["quality"])
        groups.setdefault(key, []).append(row)
    output = []
    for group in groups.values():
        first = group[0]
        summary = {key: first[key] for key in ["asset", "category", "lane", "revision", "codec", "quality",
                  "source_frames", "source_rate", "stored_frames", "stored_rate", "channels", "encoded_bytes"]}
        summary["trials"] = len(group)
        summary["duration_seconds"] = first["source_frames"] / first["source_rate"]
        summary["kbps"] = first["encoded_bytes"] * 8 / summary["duration_seconds"] / 1000
        for operation in ("encode", "decode"):
            values = [row[f"{operation}_ms"] for row in group]
            summary[f"{operation}_median_ms"] = statistics.median(values)
            summary[f"{operation}_p95_ms"] = percentile(values, 0.95)
            summary[f"{operation}_p99_ms"] = percentile(values, 0.99)
        summary["decode_realtime"] = summary["duration_seconds"] * 1000 / summary["decode_median_ms"]
        summary["quality_metrics"] = next((row["quality_metrics"] for row in group if "quality_metrics" in row), {})
        for key in ("encoded_sha256", "decoded_sha256", "encoded_fnv64", "decoded_fnv64", "decoder_state_bytes", "four_slot_pcm_bytes"):
            summary[key] = first.get(key)
        for key in ("init_first_frame_ms", "stream_decode_ms"):
            values = [row[key] for row in group if row.get(key) is not None]
            summary[key.replace("_ms", "_median_ms")] = statistics.median(values) if values else None
        output.append(summary)
    return output


def category_summary(rows: list[dict]) -> list[dict]:
    groups: dict[tuple, list[dict]] = {}
    for row in rows:
        key = (row["category"], row["lane"], row["revision"], row["codec"], row["quality"])
        groups.setdefault(key, []).append(row)
    output = []
    for key, group in groups.items():
        duration = sum(row["duration_seconds"] for row in group)
        encoded = sum(row["encoded_bytes"] for row in group)
        encode_ms = sum(row["encode_median_ms"] for row in group)
        decode_ms = sum(row["decode_median_ms"] for row in group)
        output.append(dict(zip(("category", "lane", "revision", "codec", "quality"), key)) |
                      {"assets": len(group), "duration_seconds": duration, "encoded_bytes": encoded,
                       "kbps": encoded * 8 / duration / 1000,
                       "encode_sum_median_ms": encode_ms, "decode_sum_median_ms": decode_ms,
                       "decode_realtime": duration * 1000 / decode_ms})
    return output


def write_csv(path: Path, rows: list[dict]) -> None:
    if not rows:
        return
    keys = list(dict.fromkeys(key for row in rows for key in row if not isinstance(row[key], dict)))
    with path.open("w", newline="", encoding="utf-8") as file:
        writer = csv.DictWriter(file, keys, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def revision_comparisons(rows: list[dict]) -> list[dict]:
    pairs = {}
    for row in rows:
        if row["codec"] == "rgs" and row["lane"] == "normalized":
            pairs.setdefault((row["asset"], row["quality"]), {})[row["revision"]] = row
    output = []
    for (asset, quality), pair in pairs.items():
        if "baseline" not in pair or "candidate" not in pair:
            continue
        before, after = pair["baseline"], pair["candidate"]
        output.append({"asset": asset, "category": after["category"], "quality": quality,
                       "encode_speedup": before["encode_median_ms"] / after["encode_median_ms"],
                       "decode_speedup": before["decode_median_ms"] / after["decode_median_ms"],
                       "encoded_size_ratio": after["encoded_bytes"] / before["encoded_bytes"],
                       "encoded_identical": before["encoded_sha256"] == after["encoded_sha256"],
                       "decoded_identical": before["decoded_sha256"] == after["decoded_sha256"]})
    return output


def cpu_name() -> str:
    if sys.platform == "win32":
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DESCRIPTION\System\CentralProcessor\0") as key:
            return winreg.QueryValueEx(key, "ProcessorNameString")[0].strip()
    return platform.processor() or platform.machine()


def affinity(cpu: int | None) -> list[int]:
    if sys.platform == "win32":
        import ctypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.GetCurrentProcess.restype = ctypes.c_void_p
        process = kernel.GetCurrentProcess()
        process_mask, system_mask = ctypes.c_size_t(), ctypes.c_size_t()
        kernel.GetProcessAffinityMask.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_size_t)]
        kernel.SetProcessAffinityMask.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        if not kernel.GetProcessAffinityMask(process, ctypes.byref(process_mask), ctypes.byref(system_mask)):
            raise OSError(ctypes.get_last_error(), "GetProcessAffinityMask failed")
        if cpu is not None:
            if cpu < 0 or not (system_mask.value & (1 << cpu)):
                raise ValueError(f"CPU {cpu} unavailable")
            if not kernel.SetProcessAffinityMask(process, 1 << cpu):
                raise OSError(ctypes.get_last_error(), "SetProcessAffinityMask failed")
            process_mask.value = 1 << cpu
        return [index for index in range(ctypes.sizeof(process_mask) * 8) if process_mask.value & (1 << index)]
    if hasattr(os, "sched_getaffinity"):
        if cpu is not None:
            os.sched_setaffinity(0, {cpu})
        return sorted(os.sched_getaffinity(0))
    if cpu is not None:
        raise ValueError("CPU affinity is unsupported on this platform")
    return []


def metadata(args: argparse.Namespace) -> dict:
    git = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True, text=True)
    paths = {"candidate_executable": args.candidate, "manifest": args.manifest,
             "candidate_header": ROOT / "src/rg_rgs.h", "qoa_header": ROOT / "third_party/qoa/qoa.h",
             "adapter_source": ROOT / "benchmarks/codec_adapter.c", "runner_source": Path(__file__),
             "prepare_header": ROOT / "tools/rgs_audio_prepare.h"}
    if args.baseline:
        paths["baseline_executable"] = args.baseline
        paths["baseline_header"] = ROOT / "build/baseline/src/rg_rgs.h"
    libraries = {str(path.relative_to(ROOT) if path.is_relative_to(ROOT) else path): sha256(path) for path in sorted(audio_dependencies().rglob("*"))
                 if path.is_file() and path.suffix.lower() in (".dll", ".lib", ".a", ".so", ".dylib")}
    result = {"schema_version": 1, "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
              "cpu": cpu_name(), "logical_cpus": os.cpu_count(), "cpu_affinity": affinity(None), "os": platform.platform(),
              "python": sys.version, "numpy": np.__version__, "git_head": git.stdout.strip(),
              "argv": sys.argv, "trials": args.trials, "minimum_batch_ms": args.min_ms,
              "hashes": {name: {"path": str(path.resolve()), "sha256": sha256(path)} for name, path in paths.items() if path.exists()},
              "library_hashes": libraries,
              "timing_scope": "Native in-memory whole-asset operations; input loading, shared preparation and output I/O excluded. RGS/QOA reuse output buffers. libsndfile PCM/IMA includes library open/close and its internal allocations.",
              "statistics": "Per-file p95/p99 are quantiles across trial batch means, not callback tail latency. Category times sum per-file medians.",
              "quality_note": "SNR/segmental SNR/spectral error are diagnostics, not perceptual rankings. Exported PCM16 WAV files support listening comparisons."}
    if args.build_metadata and args.build_metadata.exists():
        result["build"] = json.loads(args.build_metadata.read_text(encoding="utf-8-sig"))
    return result


def entries_from_manifest(path: Path) -> list[dict]:
    content = json.loads(path.read_text(encoding="utf-8-sig"))
    if isinstance(content, list):
        return content
    for key in ("samples", "files", "assets", "entries"):
        if key in content:
            return content[key]
    raise ValueError("Manifest must contain samples/files/assets/entries")


def run(args: argparse.Namespace) -> int:
    if args.trials < 1 or not math.isfinite(args.min_ms) or args.min_ms < 0:
        raise ValueError("trials must be positive and min-ms nonnegative")
    args.output.mkdir(parents=True, exist_ok=True)
    affinity(args.cpu)
    prepared_dir = args.output / "prepared"
    listening_dir = args.output / "listening"
    prepared_dir.mkdir(exist_ok=True)
    listening_dir.mkdir(exist_ok=True)
    meta = metadata(args)
    (args.output / "metadata.json").write_text(json.dumps(meta, indent=2), encoding="utf-8")
    entries = entries_from_manifest(args.manifest)
    if args.category:
        entries = [entry for entry in entries if entry["category"] in args.category]
    if args.max_files:
        entries = entries[:args.max_files]
    rows, errors, preparations = [], [], []
    raw_path = args.output / "raw.jsonl"
    with raw_path.open("w", encoding="utf-8") as raw:
        for asset_index, entry in enumerate(entries):
            relative = entry["relative_path"]
            source = (args.corpus_root / relative).resolve()
            if not source.is_relative_to(args.corpus_root.resolve()):
                raise ValueError(f"Corpus path escapes root: {relative}")
            asset_id = hashlib.sha256(relative.encode()).hexdigest()[:20]
            prepared = prepared_dir / (asset_id + ".wav")
            try:
                if sha256(source).lower() != entry["sha256"].lower():
                    raise ValueError(f"Corpus SHA256 mismatch: {relative}")
                # sf_open uses the Windows narrow-character API. Keep original
                # provenance but provide a byte-identical ASCII alias to C.
                if sys.platform == "win32" and not str(source).isascii():
                    alias = prepared_dir / (entry["sha256"] + ".original.wav")
                    shutil.copyfile(source, alias)
                    source = alias.resolve()
                started = time.perf_counter()
                preparation = native(args.candidate, "--input", source, "--prepare-output", prepared)
                preparation.update(asset=relative, sha256=sha256(prepared),
                                   preparation_process_ms=(time.perf_counter() - started) * 1000)
                preparations.append(preparation)
                reference, reference_rate = read_pcm(prepared)
                cases = []
                for quality in ("high", "medium", "low"):
                    if args.baseline:
                        cases.append(("normalized", "baseline", args.baseline, "rgs", quality, prepared))
                    cases.append(("normalized", "candidate", args.candidate, "rgs", quality, prepared))
                cases.extend(("normalized", "reference", args.candidate, codec, "default", prepared) for codec in ("pcm", "qoa", "ima"))
                if args.legacy_import and args.baseline and preparation["source_rate"] > preparation["rate"]:
                    cases.extend(("legacy_import", "baseline", args.baseline, "rgs", quality, source) for quality in ("high", "medium", "low"))
                metrics, expected = {}, {}
                for trial in range(args.trials):
                    ordered = cases if trial % 2 == 0 else list(reversed(cases))
                    for lane, revision, executable, codec, quality, input_path in ordered:
                        case_id = f"{asset_id}_{lane}_{revision}_{codec}_{quality}"
                        decoded = listening_dir / (case_id + ".wav")
                        encoded = listening_dir / (case_id + (".rgs" if codec == "rgs" else ".qoa" if codec == "qoa" else ".encoded.wav"))
                        command = ["--input", input_path, "--codec", codec, "--quality", quality if codec == "rgs" else "medium", "--min-ms", args.min_ms]
                        if trial == 0:
                            command.extend(["--encoded", encoded, "--decoded", decoded])
                        row = native(executable, *command)
                        row.update(asset=relative, category=entry["category"], lane=lane, revision=revision,
                                   codec=codec, quality=quality, trial=trial, source_sha256=entry["sha256"], prepared_sha256=preparation["sha256"])
                        signature = tuple(row[key] for key in ("encoded_bytes", "stored_frames", "stored_rate", "channels", "encoded_fnv64", "decoded_fnv64"))
                        if case_id in expected and signature != expected[case_id]:
                            raise ValueError(f"Nondeterministic output in {case_id}, trial {trial}")
                        expected[case_id] = signature
                        if trial == 0:
                            samples, rate = read_pcm(decoded)
                            if rate != reference_rate:
                                raise ValueError(f"Unexpected decoded rate for {case_id}: {rate} != {reference_rate}")
                            permitted_length_error = 1 if lane == "legacy_import" else 0
                            if abs(len(samples) - len(reference)) > permitted_length_error:
                                raise ValueError(f"Timeline length mismatch for {case_id}: {len(samples)} != {len(reference)}")
                            metrics[case_id] = quality_metrics(reference, samples, rate)
                            row["quality_metrics"] = metrics[case_id]
                            row["encoded_sha256"] = sha256(encoded)
                            row["decoded_sha256"] = sha256(decoded)
                            row["listening_wav"] = str(decoded.relative_to(args.output))
                        rows.append(row)
                        raw.write(json.dumps(row, allow_nan=False) + "\n")
                        raw.flush()
                print(f"[{asset_index + 1}/{len(entries)}] {entry['category']}: {relative}", flush=True)
            except (OSError, RuntimeError, ValueError) as error:
                errors.append({"asset": relative, "error": str(error)})
                print(f"ERROR {relative}: {error}", file=sys.stderr, flush=True)
    summary = aggregate(rows)
    categories = category_summary(summary)
    comparisons = revision_comparisons(summary)
    report = {"metadata": meta, "preparation": preparations, "per_file": summary, "by_category": categories, "baseline_candidate": comparisons, "errors": errors}
    (args.output / "results.json").write_text(json.dumps(report, indent=2, allow_nan=False), encoding="utf-8")
    write_csv(args.output / "raw.csv", rows)
    write_csv(args.output / "per_file.csv", [row | row["quality_metrics"] for row in summary])
    write_csv(args.output / "by_category.csv", categories)
    write_csv(args.output / "baseline_candidate.csv", comparisons)
    print(f"Saved {len(rows)} trials to {args.output}; {len(errors)} errors", flush=True)
    return 1 if errors or not rows else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=ROOT / "benchmarks/corpus_manifest.json")
    parser.add_argument("--corpus-root", type=Path, default=ROOT / "build/corpus/original")
    parser.add_argument("--candidate", type=Path, default=ROOT / "build/bench/codec_candidate.exe")
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--output", type=Path, default=ROOT / "build/benchmark-results")
    parser.add_argument("--build-metadata", type=Path, default=ROOT / "build/bench/build_metadata.json")
    parser.add_argument("--trials", type=int, default=7)
    parser.add_argument("--min-ms", type=float, default=10)
    parser.add_argument("--legacy-import", action="store_true")
    parser.add_argument("--max-files", type=int)
    parser.add_argument("--category", action="append")
    parser.add_argument("--cpu", type=int, help="Pin this process and its native children to one logical CPU")
    return run(parser.parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
