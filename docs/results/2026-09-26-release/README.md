# Release decoder measurements

151 assets, seven alternating trials per case, 7,399 raw trials, and 4941.45 seconds of prepared audio. RGS files use the final encoder; QOA files use the same prepared inputs and unchanged reference encoder.

All timings decode existing files into preallocated PCM. File I/O, allocation, checksums, and warmup are excluded. Checked RGS and QOA include header parsing. Totals sum per-file median batch means.

| Quality | RGS seconds | QOA seconds | RGS / QOA time | Files faster than QOA |
| --- | ---: | ---: | ---: | ---: |
| high | 1.0126 | 1.2594 | 80.4% | 151 / 151 |
| medium | 1.0160 | 1.2594 | 80.7% | 150 / 151 |
| low | 1.0073 | 1.2594 | 80.0% | 151 / 151 |

Below 100% means less decode time. Per-file win counts are strict comparisons of medians, not statistical significance tests. These results describe one compiler and machine. The presets have different quality and size; decode speed does not establish equal perceptual quality.

## Mono and stereo

| Channels | Quality | Assets | RGS ms | QOA ms | RGS / QOA time |
| --- | --- | ---: | ---: | ---: | ---: |
| 1 | high | 59 | 17.6021 | 19.6593 | 89.5% |
| 1 | medium | 59 | 17.5153 | 19.6593 | 89.1% |
| 1 | low | 59 | 17.3209 | 19.6593 | 88.1% |
| 2 | high | 92 | 994.9677 | 1239.7165 | 80.3% |
| 2 | medium | 92 | 998.4867 | 1239.7165 | 80.5% |
| 2 | low | 92 | 989.9324 | 1239.7165 | 79.9% |

## Compatibility

The final decoder matches the archived PCM fingerprints for all 453 older RGS encodings. The frozen decoder from before the quality safeguards also matches all 453 new encodings throughout all seven decode trials. Encoded inputs are verified with SHA-256; decoded FNV-1a fingerprints are consistency checks, not cryptographic PCM comparisons.

The frozen and current decoders use the same decoding algorithm. Their timing differences are build/layout and run variation, not a claimed decoder optimization. Baseline timing rows are retained for transparency and compatibility evidence; the comparison above concerns final RGS versus QOA.

Measured with MSVC 19.44.35219 on an Intel Core i7-12700KF, Windows x64, `/O2`, logical CPU 0. Batch-mean percentiles describe trial variation, not callback latency. No audio device is used by this benchmark.

[Per-file measurements](per_file.csv), [RGS/QOA comparisons](comparisons.csv), [totals](totals.csv), [channel totals](channels.csv), [raw trials](raw.jsonl.gz), [compatibility](compatibility.json), and [metadata](metadata.json).
See [encoder quality measurements](../2026-09-26-quality/README.md) and [benchmark methods](../../benchmarks.md).

The [measured header](../../../benchmarks/baselines/rg_rgs_2026_09_26_measured.h) is preserved exactly. The release header adds a C++ initializer compatibility adjustment; [preprocessing verification](../../../benchmarks/baselines/rg_rgs_2026_09_26_c_equivalence.json) confirms identical C input for both measured harnesses.
