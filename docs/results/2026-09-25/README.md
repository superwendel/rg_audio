# Codec size, quality, and earlier timings

This September 25, 2026 dataset covers 151 original files, seven paired trials,
and 4941.45 seconds of prepared audio. **Its decoder timings precede the
current optimizations.** Use the [performance guide](../../performance.md) for
current results and [benchmark instructions](../../benchmarks.md) to reproduce
measurements. Do not combine timings from these separate runs.

Times sum per-file medians. SNR is the median of defined, finite per-file
values; it is not a perceptual score. File sizes include container overhead.

| Codec | Encoded MiB | QOA size | PCM size | Encode seconds | Decode seconds | Realtime factor | Median SNR dB |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| RGS high | 164.58 | 100.0% | 20.2% | 17.738 | 2.450 | 2016.8 | 35.78 |
| RGS medium | 125.45 | 76.2% | 15.4% | 37.525 | 2.487 | 1986.7 | 28.87 |
| RGS low | 124.11 | 75.4% | 15.2% | 37.627 | 2.482 | 1991.0 | 27.38 |
| QOA | 164.58 | 100.0% | 20.2% | 11.877 | 1.263 | 3912.4 | 35.78 |
| WAV IMA ADPCM | 204.55 | 124.3% | 25.1% | 3.735 | 2.744 | 1800.5 | 33.37 |
| PCM WAV | 814.91 | 495.1% | 100.0% | 0.031 | 0.032 | 156656.4 | exact |

## Revision comparison

These reductions compare the two revisions recorded in this dataset, not the
current decoder. Normalized RGS encodings and decoded-output fingerprints
matched across those revisions.

| Quality | Encode time reduction | Decode time reduction |
| --- | ---: | ---: |
| high | 2.0% | 13.5% |
| medium | 27.7% | 13.5% |
| low | 31.1% | 13.5% |

| Category | Medium encode time reduction | Medium decode time reduction | Medium / QOA size |
| --- | ---: | ---: | ---: |
| bandcamp | 17.4% | 14.9% | 79.5% |
| oculus_audio_pack | 26.9% | 13.4% | 76.4% |
| sqam | 32.0% | 13.1% | 75.3% |

[Per-file quality and timing](per_file.csv), [category results](by_category.csv),
[revision comparisons](baseline_candidate.csv), [raw trials](raw.jsonl.gz), and
[source/compiler metadata](metadata.json) retain the detailed measurements.
The [quality outlier analysis](quality_outlier.json) documents an asset whose
medium preset has substantially higher error than high or low.

## Game workloads

These measurements also use an earlier decoder build. The streaming model
runs sequentially with 1/16/64/128 resident voices, 128/256/512-frame requests,
start bursts, and whole-file loops. It separates worker decode from copy/mix
cost; simulated budget exceedances are not audio-device underruns.

| Workload | Results | Summary | Raw trials |
| --- | --- | --- | --- |
| Footstep streaming | [JSON](stream-footstep/results.json) | [CSV](stream-footstep/summary.csv) | [JSONL](stream-footstep/raw.jsonl.gz) |
| Rain streaming | [JSON](stream-rain/results.json) | [CSV](stream-rain/summary.csv) | [JSONL](stream-rain/raw.jsonl.gz) |
| SDL playback conversion | [JSON](playback/results.json) | [CSV](playback/summary.csv) | [CSV](playback/raw.csv) |

The playback test measures SDL AudioStream conversion to 48 kHz without
opening a device. It separates creation, queueing, draining, and flushing.
Neither workload measures speaker latency or establishes a safe production
voice count.

[Workload build metadata](workload_build.json) records compiler flags, source
and executable hashes, input assets, and trial counts.
