#!/usr/bin/env python3
"""Inspect an exported normalized RGS asset without encoding or timing it.

Replay one frame with Python integer arithmetic to distinguish predictor
saturation from source full-scale samples and serialized-state disagreement.
"""
from __future__ import annotations

import argparse
import ast
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

import numpy as np

from run_benchmarks import quality_metrics, read_pcm, sha256


def frames(data: bytes) -> list[dict]:
    if data[:4] != b"rgs!" or data[8:12] != b"\x01\x07\0\0":
        raise ValueError("Expected an RGS v1 file")
    output = []
    offset, start = 12, 0
    while offset < len(data):
        channels = data[offset] & 15
        count, size = struct.unpack_from("<HH", data, offset + 4)
        if not channels or channels > 8 or not count or size < 8 + 16 * channels or offset + size > len(data):
            raise ValueError("Invalid frame extent")
        states = [struct.unpack_from("<8h", data, offset + 8 + channel * 16)
                  for channel in range(channels)]
        output.append(dict(offset=offset, start=start, count=count, size=size,
                           channels=channels, flags=data[offset] >> 4, states=states))
        offset += size
        start += count
    if start != struct.unpack_from("<I", data, 4)[0]:
        raise ValueError("File sample count mismatch")
    return output


def dequant_tables(header: Path) -> dict:
    text = header.read_text(encoding="utf-8")
    tables = {}
    for width, name in ((2, "rg_rgs_dequant2_tab"), (3, "rg_rgs_dequant_tab")):
        match = re.search(r"static const int " + name + r"\[16\]\[\d\] = \{(.*?)\};", text, re.S)
        if match is None:
            raise ValueError(f"Dequantizer table missing: {name}")
        body = match.group(1).replace("{", "[").replace("}", "]")
        tables[width] = ast.literal_eval("[" + body + "]")
    return tables


def replay(data: bytes, frame: dict, tables: dict) -> tuple[np.ndarray, dict, list[tuple]]:
    offset, count, channels = frame["offset"], frame["count"], frame["channels"]
    slices = (count + 19) // 20
    mode_bytes = (slices + 7) // 8
    map_start = offset + 8 + channels * 16
    position = map_start + (mode_bytes * channels if frame["flags"] == 5 else 0)
    output = np.empty((count, channels), dtype=np.int16)
    saturation_events = two_bit_slices = max_weight = 0
    final_states = []
    for channel, state in enumerate(frame["states"]):
        history, weights = list(state[:4]), list(state[4:])
        max_weight = max(max_weight, *map(abs, weights))
        for slice_index in range(slices):
            two_bit = frame["flags"] == 6 or (frame["flags"] == 5 and
                (data[map_start + channel * mode_bytes + slice_index // 8] >> (slice_index % 8)) & 1)
            width, size = (2, 6) if two_bit else (3, 8)
            two_bit_slices += bool(two_bit)
            word = int.from_bytes(data[position:position + size], "little")
            position += size
            scale = word & 15
            for sample in range(min(20, count - slice_index * 20)):
                code = (word >> (4 + sample * width)) & ((1 << width) - 1)
                residual = tables[width][scale][code]
                predicted = sum(w * h for w, h in zip(weights, history)) // 8192
                reconstructed = predicted + residual
                saturation_events += reconstructed < -32768 or reconstructed > 32767
                value = max(-32768, min(32767, reconstructed))
                output[slice_index * 20 + sample, channel] = value
                delta = residual // 16
                weights = [w + (-delta if h < 0 else delta) for w, h in zip(weights, history)]
                history = history[1:] + [value]
                max_weight = max(max_weight, *map(abs, weights))
        final_states.append(tuple(history + weights))
    if position != offset + frame["size"]:
        raise ValueError("Frame replay did not consume its exact payload")
    return output, dict(saturation_events=saturation_events, two_bit_slices=two_bit_slices,
                        max_absolute_predictor_weight=max_weight), final_states


def full_scale(values: np.ndarray) -> np.ndarray:
    return (values == -32768) | (values == 32767)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--results-root", type=Path, default=Path("build/benchmark-results"))
    parser.add_argument("--asset", required=True)
    parser.add_argument("--frame-index", type=int, default=18, help="Zero-based RGS frame index")
    parser.add_argument("--revision", default="candidate", choices=("candidate", "baseline"))
    parser.add_argument("--codec-header", type=Path, default=Path("src/rg_rgs.h"))
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    asset_id = hashlib.sha256(args.asset.encode()).hexdigest()[:20]
    prepared = args.results_root / "prepared" / (asset_id + ".wav")
    source, rate = read_pcm(prepared)
    tables = dequant_tables(args.codec_header)
    report = dict(schema_version=1, asset=args.asset, lane="normalized", revision=args.revision,
                  command=["python", "benchmarks/analyze_quality_outlier.py", *sys.argv[1:]],
                  method="Read-only analysis of existing PCM16 WAV and RGS exports; no re-encoding or performance measurement. Python frame replay uses mathematical floor and unbounded integer intermediates.",
                  analyzer_sha256=sha256(Path(__file__)), codec_header_sha256=sha256(args.codec_header),
                  source=dict(prepared_path=str(prepared), sha256=sha256(prepared), rate=rate,
                              frames=len(source), channels=source.shape[1],
                              full_scale_samples=int(full_scale(source).sum())), presets=[])
    for quality in ("high", "medium", "low"):
        name = f"{asset_id}_normalized_{args.revision}_rgs_{quality}"
        encoded = args.results_root / "listening" / (name + ".rgs")
        decoded_path = args.results_root / "listening" / (name + ".wav")
        decoded, decoded_rate = read_pcm(decoded_path)
        if source.shape != decoded.shape or rate != decoded_rate:
            raise ValueError("Normalized source/decoded timelines differ")
        data = encoded.read_bytes()
        descriptions = frames(data)
        if args.frame_index < 0 or args.frame_index >= len(descriptions):
            raise ValueError("Frame index outside encoded asset")
        frame = descriptions[args.frame_index]
        start, end = frame["start"], frame["start"] + frame["count"]
        reconstructed, diagnostics, final_states = replay(data, frame, tables)
        if not np.array_equal(reconstructed, decoded[start:end]):
            raise ValueError("Independent frame replay differs from exported decoded PCM")
        diagnostics["replay_matches_exported_pcm"] = True
        diagnostics["end_state_matches_next_serialized_state"] = (
            final_states == descriptions[args.frame_index + 1]["states"]
            if args.frame_index + 1 < len(descriptions) else None)
        error = source - decoded
        total_sse = float(np.sum(error * error))
        frame_sse = float(np.sum(error[start:end] * error[start:end]))
        worst = np.unravel_index(int(np.argmax(np.abs(error))), error.shape)
        metrics = quality_metrics(source, decoded, rate)
        baseline_encoded = args.results_root / "listening" / (
            f"{asset_id}_normalized_baseline_rgs_{quality}.rgs")
        baseline_decoded = baseline_encoded.with_suffix(".wav")
        baseline_comparison = None
        if args.revision == "candidate" and baseline_encoded.exists() and baseline_decoded.exists():
            baseline_pcm, baseline_rate = read_pcm(baseline_decoded)
            baseline_comparison = dict(
                encoded_sha256=sha256(baseline_encoded), decoded_sha256=sha256(baseline_decoded),
                encoded_identical=baseline_encoded.read_bytes() == data,
                decoded_pcm_identical=baseline_rate == rate and np.array_equal(baseline_pcm, decoded))
        report["presets"].append(dict(
            quality=quality, encoded_path=str(encoded), decoded_path=str(decoded_path),
            encoded_bytes=len(data), encoded_sha256=sha256(encoded), decoded_sha256=sha256(decoded_path),
            baseline_comparison=baseline_comparison,
            snr_db=metrics["snr_db"], segmental_snr_db=metrics["segmental_snr_db"],
            total_squared_error=total_sse, decoded_full_scale_samples=int(full_scale(decoded).sum()),
            worst_error=dict(sample_frame=int(worst[0]), channel=int(worst[1]),
                             source_sample=int(source[worst]), decoded_sample=int(decoded[worst]),
                             absolute_error=int(abs(error[worst]))),
            selected_frame=dict(zero_based_index=args.frame_index, start_sample_frame=start,
                                end_sample_frame_exclusive=end, start_seconds=start / rate, end_seconds=end / rate,
                                squared_error=frame_sse,
                                percent_of_total_squared_error=100 * frame_sse / total_sse if total_sse else 0,
                                source_full_scale_samples=int(full_scale(source[start:end]).sum()),
                                decoded_full_scale_samples=int(full_scale(decoded[start:end]).sum()),
                                decoded_full_scale_without_source_full_scale_samples=int((
                                    full_scale(decoded[start:end]) & ~full_scale(source[start:end])).sum()),
                                **diagnostics)))
    report["observations"] = dict(
        all_frame_replays_match_exported_pcm=True,
        all_frame_end_states_match_next_serialized_state=all(
            row["selected_frame"]["end_state_matches_next_serialized_state"] is True
            for row in report["presets"]),
        all_presets_identical_to_baseline=all(
            row["baseline_comparison"] is not None and row["baseline_comparison"]["encoded_identical"]
            and row["baseline_comparison"]["decoded_pcm_identical"] for row in report["presets"]))
    report["interpretation"] = (
        "Preset names do not guarantee monotonic whole-asset error: local width decisions also change future predictor state. "
        "Full-scale output counts are distinguished from source full-scale values and actual saturation events in the independent replay. "
        "Matching replay and next-frame state indicate consistent decoding/serialization for this frame; severe error can still be an encoder-quality defect. "
        "Inspect per-asset outliers and listen before choosing a preset; retain PCM or another codec as a fallback.")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    print(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
