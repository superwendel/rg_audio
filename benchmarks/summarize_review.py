"""Validate a complete seven-trial corpus run and write local review tables."""
import argparse
from collections import defaultdict
import csv
import gzip
import hashlib
import json
import math
from pathlib import Path
import shutil
import statistics

ROOT = Path(__file__).resolve().parents[1]


def case_key(row):
    return tuple(row[name] for name in ("asset", "lane", "revision", "codec", "quality"))


def validate(report, manifest, raw_rows):
    """Require each expected asset/case/trial and independently verify paired hashes."""
    entries = {entry["relative_path"]: entry for entry in manifest["samples"]}
    if len(entries) != len(manifest["samples"]):
        raise ValueError("Duplicate manifest asset")
    if report["errors"] or report["metadata"]["trials"] != 7:
        raise ValueError("Publication requires zero errors and seven paired trials")
    expected = set()
    legacy = "--legacy-import" in report["metadata"].get("argv", [])
    for asset, entry in entries.items():
        for revision in ("baseline", "candidate"):
            for quality in ("high", "medium", "low"):
                expected.add((asset, "normalized", revision, "rgs", quality))
        for codec in ("qoa", "ima", "pcm"):
            expected.add((asset, "normalized", "reference", codec, "default"))
        if legacy and entry["samplerate"] > 44100:
            for quality in ("high", "medium", "low"):
                expected.add((asset, "legacy_import", "baseline", "rgs", quality))
    summaries = {}
    for row in report["per_file"]:
        key = case_key(row)
        if key in summaries:
            raise ValueError(f"Duplicate summary case: {key}")
        if key not in expected or row["trials"] != 7:
            raise ValueError(f"Unexpected/incomplete summary case: {key}")
        if row["category"] != entries[row["asset"]]["category"]:
            raise ValueError(f"Incorrect category: {key}")
        summaries[key] = row
    if set(summaries) != expected:
        raise ValueError("Missing summary cases, including requested legacy imports")
    trials = defaultdict(dict)
    for row in raw_rows:
        key = case_key(row)
        trial = row["trial"]
        if key not in expected or type(trial) is not int or trial not in range(7):
            raise ValueError(f"Unexpected raw trial: {key}, {trial}")
        if trial in trials[key]:
            raise ValueError(f"Duplicate raw trial: {key}, {trial}")
        if row["source_sha256"] != entries[row["asset"]]["sha256"] or row["category"] != entries[row["asset"]]["category"]:
            raise ValueError(f"Raw corpus provenance mismatch: {key}")
        for field in ("encode_ms", "decode_ms"):
            if not math.isfinite(row[field]) or row[field] <= 0:
                raise ValueError(f"Invalid timing {field}: {key}")
        if row["lane"] == "normalized" and (row["source_frames"] != row["stored_frames"] or row["source_rate"] != row["stored_rate"]):
            raise ValueError(f"Normalized timeline mismatch: {key}")
        trials[key][trial] = row
    if set(trials) != expected or any(set(group) != set(range(7)) for group in trials.values()):
        raise ValueError("Missing raw trials")
    signatures = ("source_frames", "source_rate", "stored_frames", "stored_rate", "channels", "encoded_bytes", "encoded_fnv64", "decoded_fnv64", "prepared_sha256")
    for key, group in trials.items():
        first, summary = group[0], summaries[key]
        for row in group.values():
            if any(row[field] != first[field] for field in signatures):
                raise ValueError(f"Output changed between trials: {key}")
        for field in signatures[:-1] + ("encoded_sha256", "decoded_sha256"):
            if summary[field] != first[field]:
                raise ValueError(f"Summary differs from raw {field}: {key}")
        for field in ("encoded_sha256", "decoded_sha256"):
            digest = summary[field]
            if not isinstance(digest, str) or len(digest) != 64 or any(ch not in "0123456789abcdef" for ch in digest):
                raise ValueError(f"Missing/invalid output hash: {key}")
        if summary["quality_metrics"] != first["quality_metrics"]:
            raise ValueError(f"Quality summary differs from raw: {key}")
        for operation in ("encode", "decode"):
            median = statistics.median(row[operation + "_ms"] for row in group.values())
            if not math.isclose(summary[operation + "_median_ms"], median, rel_tol=1e-12):
                raise ValueError(f"Timing summary differs from raw: {key}")
    pairs = {}
    for row in report["baseline_candidate"]:
        key = (row["asset"], row["quality"])
        if key in pairs:
            raise ValueError(f"Duplicate before/after pair: {key}")
        pairs[key] = row
    expected_pairs = {(asset, quality) for asset in entries for quality in ("high", "medium", "low")}
    if set(pairs) != expected_pairs:
        raise ValueError("Missing/unexpected before/after pairs")
    for asset, quality in expected_pairs:
        before = summaries[(asset, "normalized", "baseline", "rgs", quality)]
        after = summaries[(asset, "normalized", "candidate", "rgs", quality)]
        pair = pairs[(asset, quality)]
        if not pair["encoded_identical"] or not pair["decoded_identical"] or any(
                before[field] != after[field] for field in ("encoded_sha256", "decoded_sha256", "encoded_fnv64", "decoded_fnv64")):
            raise ValueError(f"Optimization changed output: {asset}, {quality}")
    return summaries


def write_csv(path, rows):
    fields = list(dict.fromkeys(key for row in rows for key, value in row.items() if not isinstance(value, dict)))
    with path.open("w", newline="", encoding="utf-8") as file:
        writer = csv.DictWriter(file, fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--results", type=Path, default=ROOT / "build/benchmark-results")
    parser.add_argument("--destination", type=Path, default=ROOT / "build/published-results/corpus")
    args = parser.parse_args()
    report = json.loads((args.results / "results.json").read_text(encoding="utf-8"))
    manifest_path = ROOT / "benchmarks/corpus_manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if report["metadata"]["hashes"]["manifest"]["sha256"] != hashlib.sha256(manifest_path.read_bytes()).hexdigest():
        raise ValueError("Manifest changed since measurement")
    with (args.results / "raw.jsonl").open(encoding="utf-8") as raw:
        validate(report, manifest, (json.loads(line) for line in raw if line.strip()))
    files = report["per_file"]
    expected_assets = {entry["relative_path"] for entry in manifest["samples"]}
    groups = defaultdict(list)
    for row in files:
        groups[(row["lane"], row["revision"], row["codec"], row["quality"])].append(row)
    normalized_cases = [("normalized", rev, "rgs", quality)
                        for rev in ("baseline", "candidate") for quality in ("high", "medium", "low")]
    normalized_cases += [("normalized", "reference", codec, "default") for codec in ("qoa", "ima", "pcm")]
    for key in normalized_cases:
        if {row["asset"] for row in groups[key]} != expected_assets:
            raise ValueError(f"Unequal/missing corpus coverage: {key}")
    args.destination.mkdir(parents=True, exist_ok=True)
    (args.destination / "metadata.json").write_text(json.dumps(report["metadata"], indent=2), encoding="utf-8")
    write_csv(args.destination / "per_file.csv", [row | row["quality_metrics"] for row in files])
    write_csv(args.destination / "by_category.csv", report["by_category"])
    write_csv(args.destination / "baseline_candidate.csv", report["baseline_candidate"])
    with (args.results / "raw.jsonl").open("rb") as source:
        with gzip.GzipFile(filename=str(args.destination / "raw.jsonl.gz"), mode="wb", mtime=0) as output:
            shutil.copyfileobj(source, output)
    def total(key, field):
        return sum(row[field] for row in groups[key])
    qoa = ("normalized", "reference", "qoa", "default")
    pcm = ("normalized", "reference", "pcm", "default")
    duration = total(qoa, "duration_seconds")
    lines = ["# Corpus measurements", "", f"{len(expected_assets)} original files; seven paired trials; {duration:.2f} seconds of prepared audio.", "",
             "Times below sum per-file medians. Quality is the median of defined, finite per-file SNR values, not a perceptual score.", "",
             "| Codec | Encoded MiB | QOA size | PCM size | Encode seconds | Decode seconds | Decode × realtime | Median SNR dB |",
             "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
    cases = [("RGS high", ("normalized", "candidate", "rgs", "high")),
             ("RGS medium", ("normalized", "candidate", "rgs", "medium")),
             ("RGS low", ("normalized", "candidate", "rgs", "low")),
             ("QOA", qoa), ("WAV IMA ADPCM", ("normalized", "reference", "ima", "default")), ("PCM WAV", pcm)]
    for name, key in cases:
        size = total(key, "encoded_bytes")
        enc, dec = total(key, "encode_median_ms"), total(key, "decode_median_ms")
        snrs = [r["quality_metrics"]["snr_db"] for r in groups[key] if r["quality_metrics"]["snr_db"] is not None]
        quality = f"{statistics.median(snrs):.2f}" if snrs else ("exact" if all(r["quality_metrics"]["exact_pcm"] for r in groups[key]) else "undefined")
        lines.append(f"| {name} | {size / 2**20:.2f} | {size / total(qoa, 'encoded_bytes'):.1%} | {size / total(pcm, 'encoded_bytes'):.1%} | {enc / 1000:.3f} | {dec / 1000:.3f} | {duration * 1000 / dec:.1f} | {quality} |")
    lines += ["", "## Before and after", "", "All normalized RGS files and decoded outputs are byte-identical across revisions.", "",
              "| Quality | Encode time reduction | Decode time reduction |", "| --- | ---: | ---: |"]
    for quality in ("high", "medium", "low"):
        before, after = ("normalized", "baseline", "rgs", quality), ("normalized", "candidate", "rgs", quality)
        enc = 1 - total(after, "encode_median_ms") / total(before, "encode_median_ms")
        dec = 1 - total(after, "decode_median_ms") / total(before, "decode_median_ms")
        lines.append(f"| {quality} | {enc:.1%} | {dec:.1%} |")
    lines += ["", "## Category results", "", "| Category | Medium encode time reduction | Medium decode time reduction | Medium / QOA size |",
              "| --- | ---: | ---: | ---: |"]
    for category in ("bandcamp", "oculus_audio_pack", "sqam"):
        def subtotal(revision, codec, field):
            return sum(row[field] for row in files if row["category"] == category and row["lane"] == "normalized"
                       and row["revision"] == revision and row["codec"] == codec and row["quality"] == ("medium" if codec == "rgs" else "default"))
        enc = 1 - subtotal("candidate", "rgs", "encode_median_ms") / subtotal("baseline", "rgs", "encode_median_ms")
        dec = 1 - subtotal("candidate", "rgs", "decode_median_ms") / subtotal("baseline", "rgs", "decode_median_ms")
        size = subtotal("candidate", "rgs", "encoded_bytes") / subtotal("reference", "qoa", "encoded_bytes")
        lines.append(f"| {category} | {enc:.1%} | {dec:.1%} | {size:.1%} |")
    lines += ["", "Full per-file quality/error and timing details: [per_file.csv](per_file.csv).",
              "Paired changes: [baseline_candidate.csv](baseline_candidate.csv).",
              "Raw trials: [raw.jsonl.gz](raw.jsonl.gz). Build and source provenance: [metadata.json](metadata.json).", ""]
    (args.destination / "README.md").write_text("\n".join(lines), encoding="utf-8")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
