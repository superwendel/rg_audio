# RGS player font assets

The RGS player uses the same baked Inter Medium 16 px ASCII font as the
published `rg_gui` examples. The metrics and raw RGBA8 atlas were copied from
`rg_gui` commit `cf79949550c8cbecf7ed2e206e2b95e88c3ab09e`.

| Asset | SHA-256 |
| --- | --- |
| `inter_medium_16.font` | `e31e494cee22d7a7b03355d70862e6631993a55dc75f23b3e0aa896bb1369e83` |
| decoded `inter_medium_16.rgba.b64` | `9d67b0f6edfe34294fecd4888e8058a9c5e21e1fa2589b8ff873496b6230e626` |

The 256 x 128 RGBA8 atlas contains 131072 bytes. The source was the hinted
`Inter-Medium.ttf` from the official Inter 4.1 release (source TTF SHA-256
`97ad806f526e41546d46365bb3a393145f75b7b1568913db74549ad8b8dba872`).
It was baked for U+0020 through U+007E with kerning enabled using the published
`rg_text` baker recipe and vcpkg baseline
`91e8cb4be8195112ea3a9c7e5846bd0b3ff74673`.

The raw atlas is stored as wrapped base64 for text-safe source distribution.
The player decodes it exactly once during asset loading and owns the resulting
byte buffer until shutdown. A deployed build may supply the original
`inter_medium_16.rgba` beside the metrics instead; the raw file takes
precedence when both forms are present.

Inter is licensed under the SIL Open Font License 1.1; see
[`LICENSE-INTER.txt`](LICENSE-INTER.txt).
