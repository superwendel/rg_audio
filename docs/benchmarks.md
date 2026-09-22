# Benchmark methodology

The RGS benchmarks are diagnostic programs, not release gates. They report
encoded size, bitrate, and elapsed time so maintainers can spot regressions and
applications can estimate budgets on representative hardware. They never print
a pass/fail verdict based on a fixed percentage and CI does not fail because a
hosted runner is slow or noisy.

## Build and run

On Windows:

```bat
build.bat bench
build.bat bench_stream
```

The codec benchmark can run its deterministic generated corpus or optional PCM
WAV files:

```text
bench_rgs.exe --iters 20
bench_rgs.exe music.wav --iters 20
bench_rgs.exe music.wav ambience.wav --iters 10
bench_rgs.exe --quality high|medium|low --target-kbps N
```

Run `bench_rgs.exe --help` for the exact options supported by the built
revision. Without a file argument, no external or copyrighted audio corpus is
needed: the program generates deterministic tonal, voice-like, and transient
signals in memory. The generator parameters and iteration count are fixed by
the built revision, and the iteration count is printed with the results so runs
can be reproduced against that revision.

`bench_rgs_stream` likewise creates an encoded deterministic input by default
and may accept exact `.rgs` buffers where supported. It uses the public
`RgRgsDecoder` API; it does not duplicate private frame parsing.

On Unix-like systems, build with an optimizing C compiler and link the math
library:

```sh
cc -std=c99 -O2 -Wall -Wextra \
  -Isrc -I../rg_core/src tests/bench_rgs.c -lm -o bench_rgs

cc -std=c99 -O2 -Wall -Wextra \
  -Isrc -I../rg_core/src tests/bench_rgs_stream.c -lm -o bench_rgs_stream
```

## Codec comparison

The benchmark vendors Dominic Szablewski's QOA reference implementation under
`third_party/qoa` and compares it with the public RGS v1 encoder and checked
decoder. QOA is the meaningful lineage baseline because RGS derives its LMS
predictor and 3-bit dequantization from QOA while changing the container and
adding planar mixed-width frames.

For every input, the benchmark should report at least:

- source sample rate, stored RGS rate, channels, and duration;
- selected RGS quality and target-bitrate hint;
- QOA and RGS encoded bytes and duration-normalized bitrate;
- encode and decode elapsed time over the requested iterations; and
- decoded signal-to-noise measurements for the compared outputs.

RGS resamples sources above 44.1 kHz while QOA can retain their source rate.
That is an intentional product-format difference, not a like-for-like residual
coder comparison. Results MUST print both rates and SHOULD be interpreted as
asset outcomes for equal playback duration. If the residual coding alone is of
interest, provide source WAVs already normalized to the same rate and channel
layout.

RGS encoded and decoded outputs use caller-owned memory sized from the public
bound and decoded metadata. If a source above 44.1 kHz invokes the RGS
encoder's documented resampling allocation, that work remains inside its
encode measurement. The vendored QOA reference uses its upstream
allocation-returning encode and decode APIs; those allocations are part of the
corresponding QOA measurements and are freed between operations. Setup, file
I/O, source generation, validation of benchmark arguments, and output
formatting are outside timed sections. Encode and decode results are checked
before their timings are accepted.

## Timing protocol

Elapsed time uses a monotonic, high-resolution platform clock. Each result is
the total for the printed iteration count divided by that count. A benchmark
build uses optimization and should be run without a debugger. For useful local
comparisons:

1. Use the same executable, corpus, quality, target hint, and iteration count.
2. Close high-load applications and allow the machine to reach a stable power
   state.
3. Run each revision several times and compare distributions, not one minimum.
4. Record CPU, operating system, compiler/version, flags, and commit IDs.
5. Treat hosted-CI values as smoke data only; virtualization and contention
   make them unsuitable for performance acceptance.

Compiler dead-code elimination is prevented by consuming decoded output and
checking return values. The benchmarks must not substitute the trusted decoder
for the checked decoder unless the output labels explicitly distinguish it.

## Streaming benchmark

Realtime playback has two independent costs, and `bench_rgs_stream` reports
them separately:

1. **Decoder throughput**: allocation-free calls to
   `rg_rgs_decoder_next_s16` producing complete maximum-bounded PCM frames.
2. **Callback/ring copy**: bounded copies from already decoded PCM slots into
   callback-sized output blocks.

The ring path is a scheduling model; it does not open a real audio device and
does not claim end-to-end latency. The output includes callback size, ring-slot
count or prefill depth, iteration count, total decoded samples, bytes per
second, and per-operation timing statistics supported by the platform. A ring
underrun, decode failure, or sample-count mismatch is a correctness failure,
not a performance result.

Do not benchmark decoding compressed data in the simulated callback and call
that the recommended integration. Production playback should follow the
decode-ahead ownership model in [streaming.md](streaming.md).

## What results mean

- Encoded size and bitrate are deterministic for a fixed input and revision.
- Timing is hardware-, compiler-, build-, and workload-specific.
- The `target_kbps` value is a nonbinding quality cap, so measured bitrate can
  be above or below it.
- Synthetic sine and noise inputs exercise stable extremes but do not replace
  a game's own music, dialogue, ambience, and effects corpus.
- QOA is a comparison point, not a required runtime dependency or a quality
  oracle.

Release notes may cite benchmark results only when they include the full setup,
input provenance, command line, commit IDs, and raw aggregate measurements.
