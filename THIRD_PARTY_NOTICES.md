# Third-party notices

RGS is derived from the Quite OK Audio (QOA) codec. The runtime and development
dependencies are separated below so applications can audit what they ship.

## Quite OK Audio runtime derivation

`src/rg_rgs.h` contains a QOA-derived least-mean-squares predictor, dequantizer
tables, and associated arithmetic originally by Dominic Szablewski. QOA is
distributed under the MIT license. The complete upstream copyright and license
notice is reproduced in `src/rg_rgs.h` next to the derived implementation.

The RGS container, frame ordering, mixed 2-bit/3-bit representation, quality
selection, resampling, checked decoder, and streaming API are Reverse Gravity
work. The QOA notice still applies to the portions derived from QOA.

## QOA reference implementation

`third_party/qoa/qoa.h` is the reference QOA implementation by Dominic
Szablewski and is distributed under the MIT license in
`third_party/qoa/LICENSE`. It is compiled only by the deterministic comparison
benchmark; applications using the RGS header do not compile this vendored
file.

## Inter player assets

The optional `rg_gui` player bundles Inter Medium font data under
`tools/assets/rgs_player/`. Inter is copyright The Inter Project Authors and is
distributed under the SIL Open Font License 1.1, reproduced in
`tools/assets/rgs_player/LICENSE-INTER.txt`. These assets and `rg_gui`,
`rg_text`, SDL3, and SDL_shadercross are player/development dependencies, not
RGS codec runtime dependencies.
