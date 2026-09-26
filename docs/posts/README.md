# RGS articles

- [RGS: small audio, simple decoding, built for games](rgs-game-audio.md)
  introduces the design, codec landscape, measured results, and listening workflow.
- [RGS v1, byte by byte](rgs-specification.md) explains the wire format and
  exact decoding rules, with a complete example.
- [RGS v1 specification PDF](../../output/pdf/rgs-v1-specification.pdf) is a
  printable decoding reference. The [normative specification](../rgs_format.md)
  remains the format authority.

## Figures and data

| Figure | PNG | SVG |
| --- | --- | --- |
| Codec landscape and Triangle of Neglect | [PNG](assets/rgs-codec-landscape.png) | [SVG](assets/rgs-codec-landscape.svg) |
| Measured size versus decode time | [PNG](assets/rgs-size-vs-decode.png) | [SVG](assets/rgs-size-vs-decode.svg) |

The landscape redraws the PhobosLab chart's individual codecs and Triangle of
Neglect, then adds RGS above QOA for its additional format/decoder rules.
Vertical distances and quality labels are subjective; encoder search effort
is not plotted. The chart rates RGS as "good" in this overview.
RGS, QOA, and IMA bitrates
use corpus-derived equivalent 44.1 kHz stereo rates; PCM uses its raw payload
rate, and the other codec positions remain approximate historical examples.
The measured figure uses the same September 26, 2026 release run for RGS and QOA. Exact
values, formulas, and source hashes are in
[rgs-comparison-data.json](assets/rgs-comparison-data.json). PCM and IMA values
are included in that data as storage-only references, without decoder timing
coordinates.

The landscape uses Comic Sans MS to emphasize its illustrative, subjective
placement. The SVG stores lettering as outlines, so readers do not need the font.

To regenerate the figures from the repository root, install Matplotlib in
your Python environment and Comic Sans MS on your system, then run:

```bat
python tools/plot_rgs_comparison.py
```

The script verifies the archived source hashes and writes both PNG and SVG.
To regenerate the PDF, install ReportLab and run:

```bat
python tools/export_rgs_spec.py
```

These articles credit Dominic Szablewski's QOA work and link the original
posts and codec landscape. The articles and chart code use the repository's
[MIT License](../../LICENSE); the original PhobosLab post and figure are
credited to their author.
