# Encoder quality measurements

151 assets, three presets, and identical SHA-256-verified prepared PCM for both revisions.
Every native trial checks sample-for-sample agreement between checked, trusted, and streaming decoders outside timing.

## Quality and size

SNR is a numerical error measure, not a perceptual score. The minimum is taken across all 151 files, including small negative changes.

| Quality | Baseline MiB | Current MiB | Size change | Whole-corpus squared-error change | Minimum file SNR change | Improved / equal / worse files |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| high | 164.58 | 164.58 | +0.000% | -2.70% | -0.000704 dB | 16 / 134 / 1 |
| medium | 125.45 | 125.53 | +0.064% | -8.92% | +0.000000 dB | 25 / 126 / 0 |
| low | 124.11 | 124.60 | +0.395% | -15.44% | +0.000000 dB | 49 / 102 / 0 |

## Local errors

Counts below refer to codec-frame/channel observations: stereo contributes two observations per temporal frame. Severe flags use the published numerical thresholds; they do not establish audibility or literal predictor clipping.

| Quality | Baseline severe | Current severe | Newly flagged | Resolved flags | Error-doubling regressions |
| --- | ---: | ---: | ---: | ---: | ---: |
| high | 4 | 5 | 2 | 1 | 2 |
| medium | 8 | 5 | 2 | 5 | 0 |
| low | 93 | 3 | 0 | 90 | 3 |

The flagged-frame archive includes every baseline/current severe observation, every error-doubling regression, and the ten largest squared-error increases per quality. It records both revisions' peak error, RMS error, SNR, full-scale counts, and flags. Improved file averages do not imply that every local error improved.

Largest squared-error increase at high: `bandcamp/allegaeon-beasts-and-worms.wav`, frame 1248, channel 1 (zero-based), 144.893 s. Frame SNR changes by -0.583 dB and peak absolute error changes from 19307 to 20699 PCM16 units.
Largest squared-error increase at medium: `bandcamp/allegaeon-beasts-and-worms.wav`, frame 1163, channel 1 (zero-based), 135.024 s. Frame SNR changes by -2.011 dB and peak absolute error changes from 9414 to 13982 PCM16 units.
Largest squared-error increase at low: `bandcamp/allegaeon-beasts-and-worms.wav`, frame 1116, channel 0 (zero-based), 129.567 s. Frame SNR changes by -0.916 dB and peak absolute error changes from 9349 to 9865 PCM16 units.

## Original bird-ambience outlier

| Quality | Baseline SNR | Current SNR | Baseline / current unexpected full-scale samples |
| --- | ---: | ---: | ---: |
| high | 14.367 dB | 20.472 dB | 23 / 1 |
| medium | 2.870 dB | 19.153 dB | 392 / 6 |
| low | 19.119 dB | 21.740 dB | 6 / 0 |

High and medium still have one flagged bird-ambience frame/channel each. These
gains do not eliminate every transient error.

## Encode cost

This run used one trial per revision and preset, with one untimed native warmup per invocation. Times below sum per-file trial medians. They screen encode overhead; they are not a seven-trial performance result, callback deadline measurement, or process CPU-time measurement. Baseline timings were measured again with the same adapter and compiler flags.

| Quality | Baseline seconds | Current seconds | Screening time change |
| --- | ---: | ---: | ---: |
| high | 12.964 | 14.719 | +13.5% |
| medium | 28.727 | 32.497 | +13.1% |
| low | 28.786 | 33.742 | +17.2% |

Measured on Windows 11, Intel Core i7-12700KF, logical CPU 0, MSVC 19.44.35219 `/O2`, x64. Encode timing excludes preparation, file I/O, error calculations, and decoder API agreement checks.

[Per-file measurements](per_file.csv), [totals](totals.csv), [flagged-frame comparisons](flagged_frames.csv.gz), [native trials](raw.jsonl.gz), and [metadata](metadata.json).
Full frame tables and listening exports remain local; their hashes and complete row counts are recorded in metadata. The method and rerun instructions are in [benchmarks.md](../../benchmarks.md).

The [measured header](../../../benchmarks/baselines/rg_rgs_2026_09_26_measured.h) is preserved exactly. The release header adds a C++ initializer compatibility adjustment; [preprocessing verification](../../../benchmarks/baselines/rg_rgs_2026_09_26_c_equivalence.json) confirms identical C input for both measured harnesses.
