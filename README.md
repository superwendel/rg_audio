# rg_audio by Reverse Gravity

`rg_audio` is the public home of RGS (Reverse Gravity Signal), a compact,
lossy PCM16 audio format and single-header C codec for game assets. Add
`rg_audio/src` and `rg_core/src` to the compiler include path, then include
`rg_rgs.h`.

RGS v1 is the only public wire format. Decoders deliberately reject the older
laboratory version markers and the experimental RGSX format so a successful
header read has one unambiguous layout and validation contract.

The codec uses internal linkage and has no SDL dependency, implementation
macro, or separately linked library. Its only runtime dependency is
`rg_core`'s `rg_defs.h`. SDL3, `rg_gui`, `rg_text`, SDL_shadercross, and the QOA
reference implementation are development or optional-tool dependencies.

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

Input from 1 Hz through 44.1 kHz is encoded at its original rate. Higher-rate
input is low-pass filtered and resampled to 44.1 kHz. This downsampling path is
the encoder's only allocation. Define both `RG_RGS_MALLOC` and `RG_RGS_FREE`
before inclusion to route it through a custom allocator; defining only one is
an error. Encoding at 44.1 kHz or below and all decoding are allocation-free.

Use `rg_rgs_encode_bound` before either encode entry point. The returned bound
covers every public quality choice and the possible resampled timeline.

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
build.bat rgs_convert
build.bat bench
```

`rgs_convert input.wav output.rgs` converts PCM WAV input. Add
`--quality high|medium|low` and/or `--target-kbps N` to select encoder options.
The converter stages output beside its destination and then replaces it, so a
failed conversion does not leave a partial `.rgs` file.

The optional `rgs_player` is an A/B WAV/RGS player built with `rg_gui`,
`rg_text`, and SDL3. It encodes WAV input in memory and writes a sidecar only
after an explicit Save action or `--write-sidecar`. Set `RG_GUI_DIR`,
`RG_TEXT_DIR`, and the SDL/vcpkg variables described by `build.bat` when the
repositories are not siblings.

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
and compares RGS with the vendored QOA reference. It prints measurements but
does not impose a machine-dependent performance threshold. See
[docs/benchmarks.md](docs/benchmarks.md).

## Dependency baseline

| Dependency | Tested revision |
| --- | --- |
| `rg_core` | `27d5475a4af221813977f4b7d62e4e3f88cffab2` |
| `rg_gui` (optional player) | `cf79949550c8cbecf7ed2e206e2b95e88c3ab09e` |
| `rg_text` (optional player) | `0161dfd1790e9469a39c734fa61cb5640a36fd51` |
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
