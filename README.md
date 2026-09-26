# rg_audio by Reverse Gravity

`rg_audio` is the public home of RGS (Reverse Gravity Signal), a compact,
lossy PCM16 audio format and single-header C codec for game assets. Add
`rg_audio/src` and `rg_core/src` to the compiler include path, then include
`rg_rgs.h`.

RGS v1 is the supported wire format. Decoders reject unsupported version
markers.

The codec uses internal linkage and has no SDL dependency, implementation
macro, or separately linked library. Its only runtime dependency is
`rg_core`'s `rg_defs.h`. SDL3, `rg_gui`, `rg_text`, SDL_shadercross, libsoxr, libsndfile, and the QOA
reference implementation are development or optional-tool dependencies.

See [performance and quality](docs/performance.md) for codec comparisons,
measurement conditions, and known limitations, and the [changelog](CHANGELOG.md)
for release notes.

## Quick start

```c
#include "rg_rgs.h"

#include <stdlib.h>

int round_trip(const int16_t* pcm, uint32_t frames, uint32_t channels,
               uint32_t rate)
{
    size_t encoded_capacity = rg_rgs_encode_bound(frames, channels, rate);
    uint8_t* encoded = (uint8_t*)malloc(encoded_capacity);
    if (encoded == NULL)
        return 0;

    size_t encoded_size = rg_rgs_encode_s16(
        pcm, frames, channels, rate, encoded, encoded_capacity);

    RgRgsInfo info = {0};
    if (encoded_size == 0 ||
        !rg_rgs_read_header(encoded, encoded_size, &info))
    {
        free(encoded);
        return 0;
    }

    size_t value_count = (size_t)info.samples * info.channels;
    int16_t* decoded = (int16_t*)malloc(value_count * sizeof(*decoded));
    if (decoded == NULL)
    {
        free(encoded);
        return 0;
    }

    size_t written = rg_rgs_decode_s16(
        encoded, encoded_size, decoded, value_count, NULL);
    free(decoded);
    free(encoded);
    return written == value_count;
}
```

MSVC:

```bat
cl /nologo /W4 /O2 /I path\to\rg_audio\src /I path\to\rg_core\src example.c
```

GCC or Clang:

```sh
cc -std=c99 -Wall -Wextra -O2 \
  -Ipath/to/rg_audio/src -Ipath/to/rg_core/src example.c -o example
```

Counts named `samples` or `frames` are per-channel PCM frames unless an API
explicitly says `int16_t` values. Decode destination capacities and decode
return values are total interleaved `int16_t` values across all channels.

## Encoding and allocation

The default encoder uses medium quality. `rg_rgs_encode_s16_ex` accepts
`RG_RGS_QUALITY_HIGH`, `RG_RGS_QUALITY_MEDIUM`, or `RG_RGS_QUALITY_LOW` plus an
optional `target_kbps` hint. The hint is a quality cap, not a rate guarantee:
1-160 selects low; 161-224 demotes high to medium; values above 224 do not
change the chosen quality. Zero leaves quality unchanged.

The encoder checks decoded error per frame and channel and retries difficult
segments with alternative predictor states or slice widths. This costs extra
encoding work and can increase file size; it does not change the v1 decoder.
Presets remain quality/size choices rather than guarantees for every asset.

The codec accepts PCM at 1 Hz through 44.1 kHz and preserves that rate.
Higher-rate calls to either encoder or `rg_rgs_encode_bound` fail. All encoding
and decoding are allocation-free.
The converter and player prepare higher-rate WAVs with libsoxr VHQ before
calling the codec. This keeps resampling and its memory outside the runtime
header and preserves the 44.1 kHz format limit.

Use `rg_rgs_encode_bound` before either encode entry point. The returned bound
covers every public quality choice, including temporary mixed-frame storage.
Quality retries use about 22 KiB of stack storage plus compiler overhead.
Prepare assets outside the audio callback and budget the encoder stack
separately from the decoder's caller-owned PCM buffers.

## Checked, trusted, and streaming decode

Use `rg_rgs_decode_s16` for files or other untrusted input. It validates every
frame, cumulative sample counts, exact end-of-stream, and destination capacity.
Trailing bytes, truncation, impossible sizes, mismatched metadata, and any
non-v1 version marker fail the checked decode.

`rg_rgs_decode_trusted_s16` is an explicit fast path for a complete buffer that
has already passed a checked decode. It is not a substitute for validation and
must never receive attacker-controlled or partially received bytes.

`RgRgsDecoder` exposes allocation-free frame-at-a-time decode. Initialize it
with `rg_rgs_decoder_init`, repeatedly call `rg_rgs_decoder_next_s16`, and use
`rg_rgs_decoder_reset` to rewind. The decoder borrows the encoded buffer for
its entire lifetime. `RG_RGS_DECODE_OUTPUT_TOO_SMALL` reports the required
per-channel frame count without advancing; `RG_RGS_DECODE_INVALID` poisons the
decoder until reset. The largest output frame is
`RG_RGS_MAX_FRAME_SAMPLES` per channel.

The complete wire contract is in [docs/rgs_format.md](docs/rgs_format.md).
Realtime ownership and ring-buffer guidance is in
[docs/streaming.md](docs/streaming.md).

## Tools

From a Visual Studio Developer Command Prompt, with sibling `rg_core` or an
explicit `RG_CORE_DIR`:

```bat
build.bat
python tools/build_audio_deps.py
build.bat rgs_convert
build.bat bench
```

`rgs_convert input.wav output.rgs` converts PCM WAV input. Add
`--quality high|medium|low` and/or `--target-kbps N` to select encoder options.
The converter stages output beside its destination and then replaces it, so a
failed conversion does not leave a partial `.rgs` file.

Tool dependencies are installed under `build/deps/install` by the pinned
bootstrap above; set `RG_AUDIO_DEPS_DIR` to use another compatible installation.
The bootstrap needs Python 3.12 or later, CMake, and a C/C++ compiler.

The optional `rgs_player` is an A/B WAV/RGS player built with `rg_gui`,
`rg_text`, and SDL3. It encodes WAV input in memory and writes a sidecar only
after an explicit Save action or `--write-sidecar`. Set `RG_GUI_DIR`,
`RG_TEXT_DIR`, and the SDL/vcpkg variables described by `build.bat` when the
repositories are not siblings.

Windows builds stage the selected `SDL3.dll` beside each tool to keep an
older system installation from overriding it. Set `SDL3_DIR` to the SDL
development package, or provide `SDL3_INCLUDE_DIR`, `SDL3_LIB_DIR`, and
`SDL3_BIN_DIR` explicitly.

To listen from Command Prompt, pass a WAV file or a folder containing WAVs:

```bat
set "PATH=%CD%\build\deps\install\bin;%PATH%"
rgs_player.exe path\to\audio-folder
```

**Tab** switches between the WAV reference and its in-memory RGS medium
encoding. **Space** pauses or resumes, and **Left/Right** changes tracks.
The player scans the selected folder without descending into subfolders.
The `PATH` command affects only that Command Prompt session.

## Build, test, and benchmark

```bat
build.bat test
build.bat test_ci
build.bat test_release
build.bat bench
build.bat clean
```

No argument aliases `test`. The hosted CI gate runs the SDL-free runtime suite,
streaming and allocator tests, C++ double-inclusion check, example, converter
round trip, sanitizers, and bounded fuzzing. It compiles the optional player
and all shader formats but does not claim hosted GPU or audio-device execution.
`test_release` adds local player execution checks when its dependencies and
hardware are available.

The benchmark generates a deterministic corpus when no WAV paths are supplied
and compares RGS with the vendored QOA reference. `build.bat bench_compare`
builds the reproducible real-corpus comparison against PCM WAV, QOA, and IMA
ADPCM, including a frozen RGS baseline. Measurements do not impose a
machine-dependent performance threshold. See
[docs/benchmarks.md](docs/benchmarks.md).

## Dependency baseline

| Dependency | Tested revision |
| --- | --- |
| `rg_core` | `d5d3f4413da22568572a37f5c6bf4e0506c68a2a` |
| `rg_gui` (optional player) | `f7a65957787159d8f25c6ee9f2ca41dbec6c76e7` |
| `rg_text` (optional player) | `5331db7dee83338dacbf8f2e7b90d69acb60bac1` |
| libsoxr (asset tools) | 0.1.3, archive hash pinned in `tools/build_audio_deps.py` |
| libsndfile (comparison tools) | 1.2.2, archive hash pinned in `tools/build_audio_deps.py` |
| vcpkg ports | baseline `91e8cb4be8195112ea3a9c7e5846bd0b3ff74673` |
| SDL3 | release 3.4.14, commit `147a8ee32dbf9ac02f3794964490687b6bbda1bc` |

## Third-party code, license, and trademark

The RGS predictor and tables are QOA-derived and retain Dominic Szablewski's
MIT notice in the runtime header. The reference QOA implementation is vendored
for benchmarking, and the optional player ships Inter font assets. See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for the complete separation
and notices.

The software and documentation are available under the [MIT License](LICENSE).
Reverse Gravity is a registered trademark of Steven Wendel in the United
States. The license grants rights to the software and documentation, but not to
the Reverse Gravity name or trademark except to identify its origin; see
[TRADEMARKS.md](TRADEMARKS.md).
