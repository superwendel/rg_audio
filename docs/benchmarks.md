# Running the benchmarks

See [performance results](performance.md) for measured codec and game-workload
comparisons. Benchmarks are diagnostics, not CI speed gates: results depend on
hardware, compiler, and system load. Correctness checks must pass before timing
results are used.

## Setup and codec comparison

Run these commands from the repository root in a Visual Studio developer
shell. Python, CMake, and the sibling `rg_core` checkout are required.

```bat
python -m venv .venv
call .venv\Scripts\activate.bat
python -m pip install numpy
python tools/build_audio_deps.py
python benchmarks/corpus.py
build.bat bench_compare
python benchmarks/run_benchmarks.py --baseline build/bench/codec_baseline.exe --trials 7 --cpu 0
```

The dependency bootstrap verifies pinned libsoxr and libsndfile archives and
installs shared libraries under `build/deps/install`. Set `RG_AUDIO_DEPS_DIR`
or `RG_CORE_DIR` to use other installations. These libraries serve the tools
and benchmarks; the codec itself depends only on `rg_core/rg_defs.h`.

The [corpus manifest](../benchmarks/corpus_manifest.json) identifies 151 PCM16
WAVs from the [QOA samples](https://qoaformat.org/samples/) collection. Downloads
are SHA-256 checked and stored under ignored `build/corpus/original`. The
samples have mixed provenance; availability does not imply permission to
redistribute the collection.

The runner compares RGS high/medium/low, QOA, PCM WAV, and WAV IMA ADPCM. Results
in `build/benchmark-results` include metadata, raw trials, summaries, prepared
PCM, and listening exports. Use `--output` for another directory,
`--category` or `--max-files` for a subset, and `--min-ms` to change the
minimum timing batch. State exclusions when reporting results.

For another compiler, build `benchmarks/codec_adapter.c` with the core and
audio dependency include directories, linking libsoxr, libsndfile, and the
platform math library. Build both revisions with the same optimization flags;
select the baseline header with `RG_RGS_HEADER`. Supply executable paths
through `--candidate` and `--baseline`. Shared-library directories must be
available to the loader: `PATH` on Windows or `LD_LIBRARY_PATH` on Linux,
including `lib64` when applicable.

## Decode-only comparison

After producing encoded inputs above, compare the current decoder with the
frozen baseline preceding the mono optimization:

```bat
build.bat bench_decode
python benchmarks/run_decode_profile.py --baseline build/decode-profile/baseline.exe --candidate build/decode-profile/candidate.exe --build-metadata build/decode-profile/build_metadata.json --mode checked --trials 7 --cpu 0 --output build/decode-results
```

The harness needs no audio preprocessing libraries. It decodes existing files
into preallocated PCM buffers, checking encoded SHA-256 and decoded
fingerprints against the corpus results. `--corpus-results` selects another
input run; repeat `--asset relative/path.wav` for individual assets.

The default [frozen baseline](../benchmarks/baselines/rg_rgs_2026_09_26.h) is
shared by the build and runner. `RG_AUDIO_DECODE_BASELINE` selects another
header; `RG_AUDIO_DECODE_BASELINE_SHA256` verifies its bytes before compilation.
`RG_AUDIO_DECODE_BUILD_DIR` changes the executable/metadata directory and is
also honored by `benchmarks/test_decode_profile.py`. When overriding defaults,
pass matching executable, metadata, and `--baseline-header` paths to the runner.

`--mode checked` measures the checked public decoder. Omitting it adds
trusted and parse-only diagnostics. Trusted mode performs an untimed full
checked decode before measuring trusted decoding. Parse-only validates frames
without producing PCM and must not be presented as decode throughput.

## Game workloads

The synthetic programs provide quick local diagnostics:

```bat
build.bat bench
bench_rgs.exe --iters 20 --quality medium
bench_rgs_stream.exe --iters 20 --callback-frames 256
```

WAVs passed directly to `bench_rgs` must already be at or below 44.1 kHz. The
ring benchmark separates decoder and copy costs and requires complete callback
requests; it does not measure device latency or scheduling resilience.

Use an exported RGS asset for the many-voice model:

```bat
build.bat bench_workloads
python benchmarks/run_stream_workloads.py --candidate build/bench/stream_candidate.exe --baseline build/bench/stream_baseline.exe --input path/to/asset.rgs --trials 7 --cpu 0
```

The model covers 1/16/64/128 resident voices, 128/256/512-frame requests, steady
playback, start bursts, and whole-file loops. It records worker decode time,
copy/mix time, memory, and simulated budget exceedances. It runs sequentially;
those exceedances are not measured device underruns. Production callback and
ownership guidance is in [streaming.md](streaming.md).

Measure SDL3 conversion to a 48 kHz output rate separately:

```bat
build.bat bench_playback
python benchmarks/run_playback_resample.py --input path/to/prepared.wav --trials 7 --cpu 0
```

Repeat `--input` for more files. The runner measures 128/256/512-frame blocks
without opening a device, separating stream creation, queueing, draining, and
flushing. SDL queue allocation is included; file loading, checksums, and
destruction are excluded. First-output counts describe queued input, not
speaker latency. Use `--sdl-library` to record the SDL binary. On Windows it
must match the DLL staged beside the executable by the playback build target;
audio dependency DLLs must also be on `PATH`.

### Real-device playback check

Build the optional host test with the [player dependencies](../README.md)
installed, then run it explicitly from CMD with representative mono and stereo
WAVs. `SDL3_DIR` selects the SDL installation; this target needs no shaders.

```bat
build.bat playback_check
set "PATH=%CD%\build\deps\install\bin;%PATH%"
test_rgs_playback.exe --input path\to\mono.wav --seconds 10 --quality medium
test_rgs_playback.exe --input path\to\mono.wav --seconds 10 --voices 16 --load-threads 4
test_rgs_playback.exe --input path\to\stereo.wav --seconds 10 --quality medium
test_rgs_playback.exe --input path\to\stereo.wav --seconds 10 --voices 16 --load-threads 4
```

Use `%RG_AUDIO_DEPS_DIR%\bin` on `PATH` for a custom dependency installation.
The test opens the default SDL output device and rejects dummy/disk backends.
It reuses the player's decoder workers and rings, verifies consumed PCM against
a full decode, and mixes the voices before replacing output with silence.
Use non-silent assets. `--quality` accepts high/medium/low, `--voices` accepts
1–64, and `--load-threads` adds 0–64 CPU load threads for a 1–60 second run.

JSON reports distinguish the first 250 ms from running playback, including
callback progress, ring underruns, PCM mismatches, and worker errors. Callback
deadline misses are reported separately from the PCM/progress pass. Timings
include verification work; they are not codec throughput or audio-device
latency measurements. A pass verifies this host's callback path under the chosen
load, not subjective sound quality or speaker output. Hosted CI does not run
this device test.

For a separate hidden GPU startup check after `build.bat test_ci`, use
`rgs_player.exe --smoke-test --hidden`. That command normally plays a short tone;
set `SDL_AUDIO_DRIVER=dummy` for this GPU-only check and clear it before running
the real-device test. Listening to the exported WAVs remains a separate quality
review.

## Encoder quality regression checks

`benchmarks/evaluate_encoder_quality.py` compares a candidate encoder with an
existing corpus run. Preserve that run's `results.json`, prepared WAVs, and
listening exports under `build/benchmark-results` (or select it with `--archive`).
Build `benchmarks/codec_adapter.c` against the candidate header, then run:

```bat
python benchmarks/evaluate_encoder_quality.py --candidate path/to/candidate-adapter.exe --cpu 0 --output build/quality-review/results
```

Repeat `--asset` to select a smaller set. All three presets are checked by
default; repeat `--quality` to narrow the selection. Input and archived output
SHA-256 hashes must match. The adapter checks checked, trusted, and streaming
decoder PCM agreement outside its timed regions.

Whole-file SNR, segmental SNR, and spectral error are accompanied by separate
measurements for every 5,120-sample codec frame and channel, including partial
tails. The report records squared error, peak error, unexpected full-scale
output, and explicit regression thresholds. These are numerical diagnostics;
an improved whole-file score can still contain worse local errors. Listen to
flagged excerpts before accepting an encoder change.

The default is one measured trial after an untimed warmup, suitable for
screening size and encode cost. It does not establish a precise speed change.
For paired encode timing, supply `--baseline` and `--baseline-header` for an
adapter built with the same flags against a frozen earlier header. Both
adapters must use the same current adapter source. `--build-metadata` verifies
the selected header, executable, and adapter SHA-256 records. Archived timings
are context only; paired ratios require fresh baseline measurements.

Outputs remain in ignored build storage, including full frame tables and
listening WAVs. Public result sets contain curated measurements and metadata.

## Interpreting results

All formats receive identical prepared PCM. Rates at or below 44.1 kHz remain
unchanged; higher rates use single-threaded libsoxr VHQ with no dither. Channel
count and the actual flushed frame count are preserved. IMA padding contributes
to file size but is excluded from decoded error calculations.

Codec timings exclude file I/O, preparation, quality metrics, and exports. RGS
and QOA reuse caller-owned output buffers. PCM/IMA use libsndfile virtual I/O,
including open/close and internal allocations, so their timing scope differs.
Reported buffer sizes do not establish third-party peak memory usage.

Warmup precedes each timed batch. Multiple trials alternate revision order;
`--cpu` pins the runner and its children to one logical CPU. Keep builds, tests,
and other workloads separate from performance runs. Batch-mean p95/p99 describe
run variation, not callback tail latency. Preserve compiler flags, metadata,
raw trials, input hashes, and output fingerprints with results.

Compare size, bitrate, encode/decode cost, streaming cost, and memory alongside
SNR, segmental SNR, and log-spectral error. Metrics use aligned prepared PCM
without an alignment search that could conceal delays. PCM must reconstruct
exactly, repeated fingerprints must agree, and shortened timelines fail the
run. Numerical scores do not replace listening or establish equal perceptual
quality across codecs.

The optional `--legacy-import` lane exercises the older encoder's high-rate
input conversion separately from the matched-input comparison. Its one-frame
duration-rounding allowance does not apply to normal codec comparisons.
