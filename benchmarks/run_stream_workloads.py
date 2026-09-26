#!/usr/bin/env python3
"""Run an explicit single-thread decode-ahead/mixing model; no audio device.

Start bursts reset every 32 callbacks. Loop resets follow asset EOF, with
decode-ahead resets counted when queued, not when audible. Steady ends at EOF.
Worker+mix budget exceedances are simulated CPU budgets, not device underruns.
"""
import argparse
import json
from pathlib import Path
import statistics
from run_benchmarks import affinity, cpu_name, native, sha256, write_csv


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("build/stream-results"))
    parser.add_argument("--trials", type=int, default=7)
    parser.add_argument("--blocks", type=int, default=1000)
    parser.add_argument("--cpu", type=int)
    args = parser.parse_args()
    if args.trials < 1 or args.blocks < 1:
        parser.error("trials and blocks must be positive")
    affinity(args.cpu)
    args.output.mkdir(parents=True, exist_ok=True)
    cases = [(voices, callback, scenario) for voices in (1, 16, 64, 128)
             for callback in (128, 256, 512) for scenario in ("steady", "start", "loop")]
    executables = [("baseline", args.baseline)] if args.baseline else []
    executables.append(("candidate", args.candidate))
    rows, expected = [], {}
    with (args.output / "raw.jsonl").open("w", encoding="utf-8") as file:
        for trial in range(args.trials):
            for voices, callback, scenario in cases:
                for revision, executable in (executables if trial % 2 == 0 else list(reversed(executables))):
                    row = native(executable, "--input", args.input, "--voices", voices, "--callback-frames", callback,
                                 "--blocks", args.blocks, "--scenario", scenario)
                    row.update(revision=revision, trial=trial)
                    key = (voices, callback, scenario)
                    signature = (row["checksum"], row["consumed_frames"], row["decoded_frames"])
                    if key in expected and expected[key] != signature:
                        raise ValueError(f"Output mismatch between trials/revisions: {key}")
                    expected[key] = signature
                    rows.append(row)
                    file.write(json.dumps(row) + "\n"); file.flush()
            print(f"Streaming trial {trial + 1}/{args.trials}", flush=True)
    summaries = []
    for voices, callback, scenario in cases:
        for revision, _ in executables:
            group = [row for row in rows if (row["voices"], row["callback_frames"], row["scenario"], row["revision"]) == (voices, callback, scenario, revision)]
            summary = {key: group[0][key] for key in ("voices", "callback_frames", "scenario", "revision", "blocks", "rate", "channels", "ring_pcm_bytes", "voice_state_bytes")}
            for key in ("worker_total_ms", "copy_mix_total_ms", "worker_p99_ms", "copy_mix_p99_ms", "simulated_budget_exceedances"):
                summary[key + "_median"] = statistics.median(row[key] for row in group)
            summaries.append(summary)
    write_csv(args.output / "raw.csv", rows)
    write_csv(args.output / "summary.csv", summaries)
    metadata = {"cpu": cpu_name(), "cpu_affinity": affinity(None), "input": str(args.input), "input_sha256": sha256(args.input),
                "executable_sha256": {revision: sha256(exe) for revision, exe in executables}, "trials": args.trials,
                "model": __doc__, "summary": summaries}
    (args.output / "results.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
