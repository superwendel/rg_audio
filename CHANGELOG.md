# Changelog

## 0.1.0

- RGS v1 supports compact PCM16 game assets with checked, trusted, and
  frame-at-a-time decoding. Encoding and decoding require no heap allocation.
- Optimized mono and multichannel decoding while preserving the v1 wire format
  and decoder output for existing files.
- Added encoder checks and bounded retries for difficult frames. New encodings
  can differ from earlier encoder revisions without requiring a new decoder.
- WAV tools prepare rates above 44.1 kHz with libsoxr VHQ. Resampling stays
  outside the runtime header.
- Added reproducible comparisons with QOA, IMA ADPCM, and PCM, plus game-style
  streaming workloads and an optional audio-device playback check.
- Expanded malformed-input, arithmetic, allocator, streaming, C/C++, sanitizer,
  and encoder regression coverage. Documented buffer ownership and integration
  with `rg_core`, `rg_gui`, and `rg_text`.

See [performance and quality](docs/performance.md) for measurements and limits,
and the [format specification](docs/rgs_format.md) for the v1 compatibility
contract. Preset quality is asset-dependent; audition representative content
and validate scheduling on the hardware used by your game.
