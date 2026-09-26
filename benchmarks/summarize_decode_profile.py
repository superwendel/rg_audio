#!/usr/bin/env python3
"""Validate and summarize the 151-asset, seven-trial MSVC checked-decoder review.

Publication consumes existing results only. It never reruns a benchmark or
copies audio assets. Raw trial data is compressed; no large results.json is
published. Output defaults to ignored build storage; published documentation is
protected from overwrite because local metadata may contain machine paths.
"""
from __future__ import annotations

import argparse
from collections import defaultdict
import gzip
import hashlib
import json
import math
from pathlib import Path
import shutil

from run_decode_profile import ROOT, csv_write, summarize

ASSET_COUNT = 151
TRIAL_COUNT = 7
EXPECTED_ROWS = 7399
QUALITIES = ("high", "medium", "low")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for block in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def case_key(row: dict) -> tuple:
    return tuple(row[field] for field in ("asset", "revision", "quality", "mode"))


def digest_valid(value: object, width: int) -> bool:
    return isinstance(value, str) and len(value) == width and all(character in "0123456789abcdef" for character in value)


def compare_tables(actual: list[dict], expected: list[dict], keys: tuple[str, ...], name: str) -> None:
    indexed = {}
    for row in actual:
        key = tuple(row[field] for field in keys)
        if key in indexed:
            raise ValueError(f"Duplicate {name} row: {key}")
        indexed[key] = row
    expected_keys = {tuple(row[field] for field in keys) for row in expected}
    if set(indexed) != expected_keys:
        raise ValueError(f"Incomplete/unexpected {name} coverage")
    for reference in expected:
        key = tuple(reference[field] for field in keys)
        row = indexed[key]
        for field, value in reference.items():
            observed = row.get(field)
            if isinstance(value, float):
                if not isinstance(observed, (int, float)) or not math.isfinite(observed) or not math.isclose(observed, value, rel_tol=1e-12, abs_tol=1e-12):
                    raise ValueError(f"{name} differs from raw trials: {key}, {field}")
            elif observed != value:
                raise ValueError(f"{name} differs from raw trials: {key}, {field}")


def validate(report: dict, manifest: dict, corpus: dict, raw: list[dict]) -> tuple[list[dict], list[dict], list[dict]]:
    entries = {row["relative_path"]: row for row in manifest["samples"]}
    if len(entries) != ASSET_COUNT or len(manifest["samples"]) != ASSET_COUNT:
        raise ValueError("This publication requires the complete 151-file manifest")
    metadata = report["metadata"]
    if report["complete"] is not True or report["errors"] or report["actual_rows"] != EXPECTED_ROWS or report["expected_rows"] != EXPECTED_ROWS:
        raise ValueError("Publication requires a complete run with zero errors and 7399 trials")
    if metadata["trials"] != TRIAL_COUNT or metadata["modes"] != ["checked"] or metadata["cpu_affinity"] != [0]:
        raise ValueError("Expected seven checked-decoder trials pinned to logical CPU 0")
    if len(metadata["assets"]) != ASSET_COUNT or set(metadata["assets"]) != set(entries):
        raise ValueError("Metadata asset selection differs from the full manifest")
    for revision in ("baseline", "candidate", "reference"):
        if not metadata["compiler"].get(revision, "").startswith("MSVC "):
            raise ValueError("This publication requires the MSVC run; Clang results belong in a separate report")
    build = metadata.get("build", {})
    if "/O2" not in build.get("flags", "").split() or "x64" not in build.get("compiler", "").lower():
        raise ValueError("Missing MSVC /O2 x64 build provenance")
    for revision in ("baseline", "candidate"):
        if build["executables"][revision]["sha256"] != metadata["hashes"][revision + "_executable"]["sha256"]:
            raise ValueError(f"Build and measured executable hashes differ: {revision}")
    if build["baseline_source"]["sha256"] != metadata["hashes"]["baseline_header"]["sha256"]:
        raise ValueError("Build and measured baseline header hashes differ")
    if build["input_hashes"]["candidate_header"]["sha256"] != metadata["hashes"]["candidate_header"]["sha256"]:
        raise ValueError("Build and measured candidate header hashes differ")
    if corpus.get("errors"):
        raise ValueError("Frozen corpus reference contains errors")
    references = {}
    for row in corpus["per_file"]:
        if row["lane"] == "normalized" and ((row["codec"] == "rgs" and row["revision"] == "candidate") or
                                             (row["codec"] == "qoa" and row["revision"] == "reference")):
            key = (row["asset"], row["codec"], row["quality"])
            if key in references:
                raise ValueError(f"Duplicate frozen corpus reference: {key}")
            references[key] = row
    expected = {(asset, revision, quality, "checked") for asset in entries
                for revision in ("baseline", "candidate") for quality in QUALITIES}
    expected.update((asset, "reference", "default", "qoa") for asset in entries)
    if len(raw) != EXPECTED_ROWS:
        raise ValueError("Raw trial count is not 7399")
    seen = defaultdict(set)
    fingerprints = {}
    for row in raw:
        key = case_key(row)
        trial = row["trial"]
        if key not in expected or type(trial) is not int or trial not in range(TRIAL_COUNT):
            raise ValueError(f"Unexpected raw case/trial: {key}, {trial}")
        if trial in seen[key]:
            raise ValueError(f"Duplicate raw trial: {key}, {trial}")
        seen[key].add(trial)
        if row["category"] != entries[row["asset"]]["category"]:
            raise ValueError(f"Incorrect raw asset category: {key}")
        if row["compiler"] != metadata["compiler"][row["revision"]]:
            raise ValueError(f"Compiler changed during measurement: {key}")
        for field in ("mean_ms", "elapsed_ms"):
            if not isinstance(row[field], (int, float)) or not math.isfinite(row[field]) or row[field] <= 0:
                raise ValueError(f"Invalid raw timing: {key}, {field}")
        if type(row["batch"]) is not int or row["batch"] < 1 or row["warmups"] != 1:
            raise ValueError(f"Invalid native timing batch: {key}")
        if not math.isclose(row["mean_ms"] * row["batch"], row["elapsed_ms"], rel_tol=1e-6, abs_tol=1e-6):
            raise ValueError(f"Native batch timing does not reconcile: {key}")
        codec = "qoa" if row["mode"] == "qoa" else "rgs"
        reference = references[(row["asset"], codec, row["quality"])]
        if (row["frames"], row["channels"], row["rate"], row["encoded_bytes"]) != (
                reference["stored_frames"], reference["channels"], reference["stored_rate"], reference["encoded_bytes"]):
            raise ValueError(f"Raw asset description differs from frozen corpus: {key}")
        if row["codec_frames"] != (row["frames"] + 5119) // 5120 or row["output_buffer_bytes"] != row["frames"] * row["channels"] * 2:
            raise ValueError(f"Incorrect frame/output accounting: {key}")
        if row["checked_decode_preflight"] is not False:
            raise ValueError(f"Unexpected trusted preflight in checked/QOA comparison: {key}")
        for field in ("encoded_fnv64", "decoded_fnv64"):
            if not digest_valid(row[field], 16) or row[field] != reference[field]:
                raise ValueError(f"Fingerprint differs from frozen corpus: {key}, {field}")
        if row["expected_decoded_fnv64"] != reference["decoded_fnv64"]:
            raise ValueError(f"Expected fingerprint differs from frozen corpus: {key}")
        if not digest_valid(row["encoded_sha256"], 64) or row["encoded_sha256"] != reference["encoded_sha256"]:
            raise ValueError(f"Encoded input SHA256 differs from frozen corpus: {key}")
        signature = tuple(row[field] for field in ("frames", "channels", "rate", "encoded_bytes", "encoded_fnv64", "decoded_fnv64", "encoded_sha256"))
        if key in fingerprints and fingerprints[key] != signature:
            raise ValueError(f"Trial output changed: {key}")
        fingerprints[key] = signature
    if set(seen) != expected or any(trials != set(range(TRIAL_COUNT)) for trials in seen.values()):
        raise ValueError("Missing cases or trial IDs")
    files, comparisons, totals = summarize(raw, TRIAL_COUNT)
    if len(comparisons) != ASSET_COUNT * len(QUALITIES) or any(row["decoded_identical"] is not True for row in comparisons):
        raise ValueError("Expected 453 before/after comparisons with matching PCM fingerprints")
    compare_tables(report["per_file"], files, ("asset", "revision", "quality", "mode"), "per-file summary")
    compare_tables(report["comparisons"], comparisons, ("asset", "quality", "mode"), "before/after comparison")
    compare_tables(report["totals"], totals, ("category", "revision", "quality", "mode"), "aggregate totals")
    return files, comparisons, totals


def summarize_channels(files: list[dict], comparisons: list[dict]) -> list[dict]:
    channels = {(row["asset"], row["quality"]): row["channels"] for row in files if row["revision"] == "candidate"}
    summary = []
    for count in sorted(set(channels.values())):
        for quality in QUALITIES:
            selected = [row for row in comparisons if row["quality"] == quality and channels[row["asset"], quality] == count]
            before = sum(row["baseline_ms"] for row in selected)
            after = sum(row["candidate_ms"] for row in selected)
            qoa = sum(row["qoa_ms"] for row in selected)
            summary.append({"channels": count, "quality": quality, "assets": len(selected),
                            "baseline_ms": before, "candidate_ms": after, "qoa_ms": qoa,
                            "time_reduction": 1 - after / before, "candidate_over_qoa_time": after / qoa,
                            "faster_than_baseline": sum(row["candidate_ms"] < row["baseline_ms"] for row in selected),
                            "faster_than_qoa": sum(row["candidate_ms"] < row["qoa_ms"] for row in selected)})
    return summary


def render_readme(metadata: dict, comparisons: list[dict], totals: list[dict], channels: list[dict]) -> str:
    indexed = {(row["category"], row["revision"], row["quality"], row["mode"]): row for row in totals}
    qoa_ms = indexed[("all", "reference", "default", "qoa")]["sum_median_ms"]
    duration = indexed[("all", "reference", "default", "qoa")]["duration_seconds"]
    lines = ["# Checked-decoder measurements", "",
             f"151 assets, 7,399 raw trials, seven alternating trials per case, and {duration:.2f} seconds of audio. "
             f"MSVC `/O2`, x64, logical CPU 0 on {metadata['cpu']}.", "",
             "All timings decode existing encoded assets into preallocated PCM buffers. File I/O, allocation, "
             "checksum scans, and warmup are excluded. Each timed RGS checked pass and QOA pass includes its header parsing. "
             "QOA runs once per asset/trial in the candidate executable.", "",
             "Times below sum per-file medians; these are measurements on one machine. "
             "The QOA percentage is current RGS decode time divided by QOA decode time: below 100% means less time.", "",
             "| Quality | Before seconds | Current seconds | QOA seconds | Time reduction | Current / QOA |",
             "| --- | ---: | ---: | ---: | ---: | ---: |"]
    for quality in QUALITIES:
        before = indexed[("all", "baseline", quality, "checked")]["sum_median_ms"]
        after = indexed[("all", "candidate", quality, "checked")]["sum_median_ms"]
        lines.append(f"| {quality} | {before / 1000:.4f} | {after / 1000:.4f} | {qoa_ms / 1000:.4f} | {1 - after / before:.1%} | {after / qoa_ms:.1%} |")
    lines += ["", "## Channel totals", "",
              "| Channels | Quality | Assets | Before ms | Current ms | QOA ms | Time reduction | Current / QOA | Faster than QOA |",
              "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for row in channels:
        lines.append(f"| {row['channels']} | {row['quality']} | {row['assets']} | {row['baseline_ms']:.4f} | {row['candidate_ms']:.4f} | {row['qoa_ms']:.4f} | {row['time_reduction']:.1%} | {row['candidate_over_qoa_time']:.1%} | {row['faster_than_qoa']} |")
    lines += ["", "Counts use strict per-file medians, not statistical significance tests. "
              "The channel grouping uses decoded descriptions, not filenames.", "", "## Category totals", "",
              "| Category | Quality | Before seconds | Current seconds | QOA seconds | Time reduction | Current / QOA |",
              "| --- | --- | ---: | ---: | ---: | ---: | ---: |"]
    for category in sorted({row["category"] for row in comparisons}):
        reference = indexed[(category, "reference", "default", "qoa")]["sum_median_ms"]
        for quality in QUALITIES:
            before = indexed[(category, "baseline", quality, "checked")]["sum_median_ms"]
            after = indexed[(category, "candidate", quality, "checked")]["sum_median_ms"]
            lines.append(f"| {category} | {quality} | {before / 1000:.4f} | {after / 1000:.4f} | {reference / 1000:.4f} | {1 - after / before:.1%} | {after / reference:.1%} |")
    lines += ["", "## Per-file comparisons", "",
              "Counts use strict per-file median comparisons. They do not establish statistical significance; "
              "small differences may be timing noise. Ties are reported separately.", "",
              "| Quality | Faster than before | Equal to before | Faster than QOA | Equal to QOA | Files |",
              "| --- | ---: | ---: | ---: | ---: | ---: |"]
    for quality in QUALITIES:
        selected = [row for row in comparisons if row["quality"] == quality]
        before_wins = sum(row["candidate_ms"] < row["baseline_ms"] for row in selected)
        before_ties = sum(row["candidate_ms"] == row["baseline_ms"] for row in selected)
        qoa_wins = sum(row["candidate_ms"] < row["qoa_ms"] for row in selected)
        qoa_ties = sum(row["candidate_ms"] == row["qoa_ms"] for row in selected)
        lines.append(f"| {quality} | {before_wins} | {before_ties} | {qoa_wins} | {qoa_ties} | {len(selected)} |")
    lines += ["", "## Validation and provenance", "",
              "Every decoded PCM FNV-1a 64-bit fingerprint matches its frozen corpus reference across all seven trials "
              "and both RGS revisions. These are consistency fingerprints, not a cryptographic byte comparison. "
              "Encoded inputs are identified by SHA-256; executable, source, compiler, and build provenance are recorded in metadata.", "",
              "Only the checked RGS public decoder is represented here. "
              "Lower RGS quality settings use different encoded assets from QOA; this table compares decode cost, not equal perceptual quality.", "",
              "[Per-file measurements](per_file.csv), [before/after comparisons](comparisons.csv), "
              "[aggregate totals](totals.csv), [raw trials](raw.jsonl.gz), and [build/source metadata](metadata.json).", "",
              "These are local measurement outputs. The curated public comparison is in docs/performance.md.", ""]
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--results", type=Path, default=ROOT / "build/decode-profile/full-results")
    parser.add_argument("--destination", type=Path, default=ROOT / "build/published-results/decode")
    args = parser.parse_args()
    destination = args.destination.resolve()
    published_docs = (ROOT / "docs").resolve()
    if destination == published_docs or published_docs in destination.parents or destination in published_docs.parents:
        raise ValueError("Write local summaries outside docs, then curate metadata before publication")
    report = json.loads((args.results / "results.json").read_text(encoding="utf-8"))
    manifest_path = ROOT / "benchmarks/corpus_manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    corpus_path = Path(report["metadata"]["hashes"]["corpus_results"]["path"])
    if sha256(corpus_path) != report["metadata"]["hashes"]["corpus_results"]["sha256"]:
        raise ValueError("Frozen corpus results changed since profiling")
    corpus = json.loads(corpus_path.read_text(encoding="utf-8"))
    if sha256(manifest_path) != corpus["metadata"]["hashes"]["manifest"]["sha256"]:
        raise ValueError("Corpus manifest differs from the frozen reference")
    with (args.results / "raw.jsonl").open(encoding="utf-8") as file:
        raw = [json.loads(line) for line in file if line.strip()]
    files, comparisons, totals = validate(report, manifest, corpus, raw)
    existing_metadata = destination / "metadata.json"
    if existing_metadata.exists() and json.loads(existing_metadata.read_text(encoding="utf-8")) != report["metadata"]:
        raise ValueError("Destination already contains a different measured run; choose a new destination")
    destination.mkdir(parents=True, exist_ok=True)
    (destination / "metadata.json").write_text(json.dumps(report["metadata"], indent=2), encoding="utf-8")
    csv_write(destination / "per_file.csv", files)
    csv_write(destination / "comparisons.csv", comparisons)
    csv_write(destination / "totals.csv", totals)
    channels = summarize_channels(files, comparisons)
    csv_write(destination / "channels.csv", channels)
    with (args.results / "raw.jsonl").open("rb") as source:
        with gzip.GzipFile(filename=str(destination / "raw.jsonl.gz"), mode="wb", mtime=0) as output:
            shutil.copyfileobj(source, output)
    readme = render_readme(report["metadata"], comparisons, totals, channels)
    (destination / "README.md").write_text(readme, encoding="utf-8")
    print(readme)


if __name__ == "__main__":
    main()
