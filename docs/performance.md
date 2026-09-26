# Performance and quality

RGS targets compact game audio with allocation-free decoding. Choose a preset
using both size and listening tests: numerical quality and decode cost vary
by asset. These measurements cover music, effects, ambience, and speech on one
machine; they do not establish performance on every platform.

## Decode performance

The September 26, 2026 release comparison uses 151 files containing 4,941.45
seconds of audio: 59 mono and 92 stereo. It ran on Windows 11, an Intel Core
i7-12700KF, with MSVC 19.44 `/O2`, pinned to one logical CPU. Each file and
codec had seven trials. Times sum the median for each file. RGS inputs were
produced by the current encoder after its quality fixes.

| Codec | Decode seconds |
| --- | ---: |
| RGS high | 1.0126 |
| RGS medium | 1.0160 |
| RGS low | 1.0073 |
| QOA reference | 1.2594 |

RGS takes 19.3–20.0% less decode time
than QOA overall in this build. Mono totals are 10.5–11.9%
lower, and stereo totals are 19.5–20.1% lower. Per-file
medians and variation are available in the [release decode results](results/2026-09-26-release/README.md).

Both codecs decode existing encoded files into preallocated PCM buffers.
Checked RGS timing includes header and frame validation. File I/O,
allocation, preparation, warmup, and output checksums are outside the timer.
The RGS presets use different encoded representations and quality tradeoffs
from QOA. Results describe these compiled implementations, not an inherent
speed ranking of the formats or equal perceptual quality.

The release data checks both the prior and current decoders against the new
RGS files. Their decoder logic is unchanged by the encoder fix; differences
between those decoder timings should not be attributed to a new algorithm.
The earlier [mono optimization comparison](results/2026-09-26-mono/README.md)
includes a separate nine-file Clang measurement. No ARM performance results
are included. Decoded FNV fingerprints are consistency checks, not
cryptographic proof that two outputs are byte-for-byte equal.

## Size and quality

All formats receive the same prepared PCM from the 151-file
[QOA sample corpus](https://qoaformat.org/samples/). Rates above 44.1 kHz use
libsoxr VHQ. Current RGS quality and size measurements are compared with the
unchanged QOA, IMA, and PCM references from the September 25 run.

| Codec | Encoded MiB | Median SNR dB |
| --- | ---: | ---: |
| RGS high | 164.58 | 35.78 |
| RGS medium | 125.53 | 28.87 |
| RGS low | 124.60 | 27.38 |
| QOA | 164.58 | 35.78 |
| WAV IMA ADPCM | 204.55 | 33.37 |
| PCM WAV | 814.91 | exact |

RGS medium is 23.7% smaller than QOA and
38.6% smaller than IMA in this corpus. Low saves another
0.93 MiB. SNR is the median of finite per-file values;
it does not replace listening. IMA block padding counts toward file size but
is excluded from error calculations beyond the source timeline. High and QOA
have the same total size rounded to MiB here; RGS adds four header bytes per
file, or 604 bytes across this corpus. The current RGS encoder can reconstruct
different PCM.

The encoder now measures actual decoded error and retries difficult frames.
For `oculus_audio_pack/ambience_forest_birds_03.wav`, medium improves from
2.87 to 19.15 dB SNR. Current high scores
20.47 dB and low 21.74 dB. This addresses the earlier
medium outlier without changing the v1 decoder or wire format.

Across all 453 file/preset comparisons, none loses 0.25 dB or more of whole-file
SNR. That threshold does not rule out smaller changes or local regressions.
Retries minimize the current frame/channel's squared error, and changed
predictor state can affect subsequent frames. Preset names do not guarantee
monotonic quality, and some individual frame errors still increase. Audition
difficult assets and choose a different preset or PCM when needed. The
[quality results](results/2026-09-26-quality/README.md) report exact minima and
flagged frame/channel observations, including remaining tradeoffs.

Compared with the prior encoder, total size changes by
0.000% for high,
0.064% for medium, and
0.395% for low. A paired single-trial
screen measured 13.1–17.2%
more encoding time. These encode timings are screening results, separate
from the seven-trial decode comparison. Prepare assets outside the audio
callback.

The [September 25 codec comparison](results/2026-09-25/README.md) preserves
the earlier encoder's measurements. Its RGS quality and decode timings are
historical; use the release data above for the current implementation.

## Game integration

Decoding runs on a worker; the audio callback consumes ready PCM. Whole-file
throughput does not establish a safe voice count, callback latency, or device
underrun rate. Applications also need to budget for mixing, device-rate
conversion, scheduling, and memory.

Local Windows WASAPI checks exercised representative mono and stereo assets
for 10 seconds each, both with one voice and with 16 voices plus four CPU
load threads. They verified decoded PCM and mixing before silencing output,
with zero ring underruns, PCM mismatches, worker errors, or measured callback
budget misses. This is a bounded host check, not a game-wide performance
guarantee, speaker-latency measurement, or subjective listening result. See
[device-check instructions](benchmarks.md#real-device-playback-check) to run it
on your target hardware.

The earlier [game-workload data](results/2026-09-25/README.md#game-workloads)
includes 1/16/64/128 resident voices, 128/256/512-frame requests, synchronized
starts, whole-file loops, and separate SDL AudioStream conversion. Those
measurements use an earlier decoder and a sequential scheduling model; they
do not measure a running game's audio device.

RGS provides sequential decoding from a resident compressed buffer and reset
to the beginning. Seeking, sample-accurate loop regions, incremental file
input, and speaker-layout metadata belong to the application or asset
container. Small effects can share predecoded PCM to avoid per-voice frame
buffers. See [streaming integration](streaming.md) for ownership and callback
guidance and [benchmark instructions](benchmarks.md) to reproduce measurements.
