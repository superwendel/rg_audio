# Frozen codec baselines

These headers preserve exact baselines for RGS comparisons. They are benchmark
inputs; applications should include `src/rg_rgs.h`.

| Header | Measurement baseline | SHA-256 |
| --- | --- | --- |
| `rg_rgs_2026_09_26.h` | Decoder before the mono optimization | `90ca4ac05ed3aa3eba8a72ce6b48a9177333486dce63163d895f7cb67fd9d9ac` |
| `rg_rgs_2026_09_26_pre_quality.h` | Codec before encoder quality safeguards; includes the mono decoder optimization | `c9b5543c51524c1b4ef0d4d2e84d16c3a439866e0f0d3fb11e7c40d3986363ae` |
| `rg_rgs_2026_09_26_measured.h` | Exact C header used for release quality and decoder measurements | `ecff374dd626bde76ad2e0b26c257b3b828949295ac6b3c838d53490de19f20e` |

`build.bat bench_decode` selects `rg_rgs_2026_09_26.h` by default. Set
`RG_AUDIO_DECODE_BASELINE` to select another header, and supply the same file
to `benchmarks/run_decode_profile.py --baseline-header` when measuring it.
These headers retain the original license notice and require `rg_core/src`
on the include path.

The measured release snapshot precedes a C++ initializer compatibility
adjustment. [Preprocessing verification](rg_rgs_2026_09_26_c_equivalence.json)
records identical C translation units for the measured and release headers
under the benchmark compiler and flags.
