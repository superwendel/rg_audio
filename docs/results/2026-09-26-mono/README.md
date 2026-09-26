# Checked-decoder measurements

The September 26, 2026 run covers 151 assets, 7,399 raw trials, seven trials per
case, and 4941.45 seconds of audio. It used MSVC `/O2`, x64, logical CPU 0 on
an Intel Core i7-12700KF.

**Before** means the [frozen decoder baseline](../../../benchmarks/baselines/rg_rgs_2026_09_26.h).
**Current** means the September 26 source with SHA-256
`c9b5543c51524c1b4ef0d4d2e84d16c3a439866e0f0d3fb11e7c40d3986363ae`.
See [performance and quality](../../performance.md) for interpretation and
[benchmark instructions](../../benchmarks.md) for reproduction.

Both codecs decode existing encoded assets into preallocated PCM buffers.
Checked RGS and QOA include header parsing; file I/O, allocation, warmup, and
checksum scans are excluded. Times sum per-file medians. Current / QOA is the
decode-time ratio, so values below 100% mean less time.

| Quality | Before seconds | Current seconds | QOA seconds | Time reduction | Current / QOA |
| --- | ---: | ---: | ---: | ---: | ---: |
| high | 1.0615 | 1.0430 | 1.3068 | 1.7% | 79.8% |
| medium | 1.0748 | 1.0431 | 1.3068 | 2.9% | 79.8% |
| low | 1.0681 | 1.0398 | 1.3068 | 2.7% | 79.6% |

## Channel totals

| Channels | Quality | Assets | Before ms | Current ms | QOA ms | Time reduction | Current / QOA | Faster than QOA |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | high | 59 | 22.8665 | 19.9384 | 21.7696 | 12.8% | 91.6% | 59 |
| 1 | medium | 59 | 23.6938 | 19.9707 | 21.7696 | 15.7% | 91.7% | 58 |
| 1 | low | 59 | 23.6012 | 19.8365 | 21.7696 | 16.0% | 91.1% | 59 |
| 2 | high | 92 | 1038.6002 | 1023.0580 | 1284.9816 | 1.5% | 79.6% | 92 |
| 2 | medium | 92 | 1051.0649 | 1023.0906 | 1284.9816 | 2.7% | 79.6% | 92 |
| 2 | low | 92 | 1044.4882 | 1019.9250 | 1284.9816 | 2.4% | 79.4% | 92 |

Channels come from decoded descriptions, not filenames. Win counts compare
strict per-file medians and do not establish statistical significance.

## Category totals

| Category | Quality | Before seconds | Current seconds | QOA seconds | Time reduction | Current / QOA |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| bandcamp | high | 0.1979 | 0.1951 | 0.2460 | 1.4% | 79.3% |
| bandcamp | medium | 0.2028 | 0.2007 | 0.2460 | 1.1% | 81.6% |
| bandcamp | low | 0.2000 | 0.1973 | 0.2460 | 1.4% | 80.2% |
| oculus_audio_pack | high | 0.0961 | 0.0920 | 0.1126 | 4.3% | 81.7% |
| oculus_audio_pack | medium | 0.0985 | 0.0921 | 0.1126 | 6.6% | 81.7% |
| oculus_audio_pack | low | 0.0966 | 0.0916 | 0.1126 | 5.1% | 81.3% |
| sqam | high | 0.7674 | 0.7559 | 0.9482 | 1.5% | 79.7% |
| sqam | medium | 0.7734 | 0.7503 | 0.9482 | 3.0% | 79.1% |
| sqam | low | 0.7715 | 0.7508 | 0.9482 | 2.7% | 79.2% |

## Per-file counts

| Quality | Faster than before | Equal to before | Faster than QOA | Equal to QOA | Files |
| --- | ---: | ---: | ---: | ---: | ---: |
| high | 143 | 0 | 151 | 0 | 151 |
| medium | 148 | 0 | 150 | 0 | 151 |
| low | 142 | 0 | 151 | 0 | 151 |

All decoded PCM FNV-1a 64-bit fingerprints match the frozen corpus reference
across both revisions and all trials. These are consistency checks, not a
cryptographic byte comparison. Lower RGS settings use different encoded assets
from QOA; timing ratios do not establish equal perceptual quality.

[Per-file measurements](per_file.csv), [revision comparisons](comparisons.csv),
[channel totals](channels.csv), [aggregate totals](totals.csv),
[raw trials](raw.jsonl.gz), and [metadata](metadata.json) provide the data,
compiler/version/flags, and source/executable hashes.
The [nine-file Clang check](clang/README.md) covers a separate compiler build.
