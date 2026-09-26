"""Render the article's original figures from archived measurements.

Requires matplotlib and an installed Comic Sans MS font. Run from any directory;
all paths are relative to this file.
The landscape is a schematic, not a measurement of complexity or sound quality.
"""

from __future__ import annotations

import hashlib
import io
import json
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import font_manager
from matplotlib.patches import Polygon

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "docs" / "posts" / "assets"
DATA = json.loads((ASSETS / "rgs-comparison-data.json").read_text(encoding="utf-8"))
BG, INK, MUTED, GRID = "#faf9f5", "#172d38", "#596c75", "#dbe2df"
TEAL, BLUE, GOLD, GRAY = "#007f72", "#3569ac", "#ad6718", "#536374"


def verify_inputs() -> None:
    for source in DATA["source_files"].values():
        raw = (ROOT / source["path"]).read_bytes().replace(b"\r\n", b"\n")
        actual = hashlib.sha256(raw).hexdigest()
        if actual != source["sha256_lf_normalized"]:
            raise ValueError(f"Chart source changed: {source['path']}")
    points = DATA["measured_scatter"]["points"]
    qoa_ms = next(p["decode_sum_per_file_median_ms"] for p in points if p["id"] == "qoa")
    for point in points:
        assert abs(point["encoded_bytes"] / 1048576 - point["encoded_mib"]) < 1e-10
        assert abs(point["decode_sum_per_file_median_ms"] / qoa_ms
                   - point["decode_fraction_of_qoa"]) < 1e-10


def setup() -> None:
    plt.rcParams.update({
        "font.family": "DejaVu Sans", "font.size": 12,
        "text.color": INK, "axes.labelcolor": INK,
        "xtick.color": MUTED, "ytick.color": MUTED,
        "axes.edgecolor": GRID, "axes.facecolor": BG,
        "figure.facecolor": BG, "svg.fonttype": "path",
        "svg.hashsalt": "rgs-v1-2026-09-26",
    })


def save(fig: plt.Figure, name: str, description: str) -> None:
    for extension in ("svg", "png"):
        metadata = {"Description": description}
        if extension == "svg":
            metadata["Date"] = None
        encoded = io.BytesIO()
        fig.savefig(encoded, format=extension, dpi=180,
                    facecolor=BG, metadata=metadata)
        (ASSETS / f"{name}.{extension}").write_bytes(encoded.getvalue())
    plt.close(fig)


def landscape() -> None:
    fig = plt.figure(figsize=(14.4, 10))
    magenta, good, meh, lossless = "#c82f9b", "#77b978", "#eb7d79", "#ad85be"
    fig.text(.073, .943, "WHERE RGS FITS", color=TEAL, fontsize=11, weight="bold")
    fig.text(.073, .897, "Audio codecs and the Triangle of Neglect", fontsize=26, weight="bold")
    fig.text(.073, .863, "A subjective landscape, after the QOA article by Dominic Szablewski.",
             color=MUTED, fontsize=12)
    ax = fig.add_axes((.092, .272, .862, .547))
    ax.set(xlim=(0, 1510), ylim=(0, 10))
    ax.set_xticks(range(0, 1500, 200))
    ax.set_yticks([])
    ax.set_xlabel("bitrate (kb/s) · 44.1 kHz stereo reference", fontsize=13, labelpad=13)
    ax.set_ylabel("format / decoder complexity (subjective)", fontsize=13, labelpad=23)
    ax.tick_params(axis="x", length=5, color=INK, pad=8)
    ax.spines[["top", "right", "left", "bottom"]].set_visible(False)
    ax.annotate("", xy=(1510, 0), xytext=(0, 0),
                arrowprops={"arrowstyle": "-|>", "color": INK, "lw": 2.1}, annotation_clip=False)
    ax.annotate("", xy=(0, 10), xytext=(0, 0),
                arrowprops={"arrowstyle": "-|>", "color": INK, "lw": 2.1}, annotation_clip=False)

    # This region and the five approximate historical points below are an
    # attributed redraw of the subjective February 2023 PhobosLab sketch.
    triangle = [(130, .16), (130, 3.42), (770, .16)]
    ax.add_patch(Polygon(triangle, closed=True, facecolor=magenta, alpha=.055,
                         edgecolor="none", zorder=0))
    ax.add_patch(Polygon(triangle, closed=True, fill=False, edgecolor=magenta,
                         linewidth=2.7, linestyle=(0, (4, 2.8)), zorder=1))
    ax.text(455, 2.88, "The Triangle of Neglect", color=magenta,
            fontsize=17, weight="bold", rotation=-13, va="center")

    points = {p["id"]: p for p in DATA["measured_scatter"]["points"]}
    storage = {p["id"]: p for p in DATA["storage_only_points"]}
    qoa_rate = points["qoa"]["equivalent_stereo_44100_kbps"]
    ima_rate = storage["ima"]["equivalent_stereo_44100_kbps"]
    pcm_rate = 16 * 44100 * 2 / 1000
    # Y values are drawing coordinates only, never decoder timings or scores.
    codecs = [
        ("Opus", 64, 9.15, good, (24, 0)),
        ("Vorbis", 96, 6.55, good, (24, 0)),
        ("MP3", 128, 5.42, good, (24, 0)),
        ("WavPack", 740, 3.76, lossless, (24, 0)),
        ("FLAC", 770, 2.98, lossless, (24, 0)),
        ("QOA", qoa_rate, 1.88, good, (22, 4)),
        ("ADPCM", ima_rate, .61, meh, (22, -3)),
        ("WAV / PCM16", pcm_rate, .37, lossless, (-110, 20)),
    ]
    for label, x, y, color, offset in codecs:
        ax.scatter([x], [y], s=240, facecolor=color, edgecolor=INK,
                   linewidth=1.5, zorder=4)
        ax.annotate(label, (x, y), xytext=offset, textcoords="offset points",
                    fontsize=15, weight="bold", va="center", zorder=5)

    low_rate = points["rgs_low"]["equivalent_stereo_44100_kbps"]
    medium_rate = points["rgs_medium"]["equivalent_stereo_44100_kbps"]
    high_rate = points["rgs_high"]["equivalent_stereo_44100_kbps"]
    # RGS adds format cases to the same LMS core. Place it above QOA for
    # conceptual decoder complexity; its faster measured runtime is unrelated
    # to this illustrative coordinate. Encoder search effort is not plotted.
    rgs_y = 2.45
    ax.plot([low_rate, high_rate], [rgs_y, rgs_y], color=good,
            linewidth=6, solid_capstyle="round", zorder=6)
    ax.scatter([low_rate, high_rate], [rgs_y, rgs_y], marker="o", s=240,
               facecolor=good, edgecolor=INK, linewidth=1.5, zorder=7)
    ax.annotate("RGS", xy=((low_rate + high_rate) / 2, rgs_y),
                xytext=((low_rate + high_rate) / 2, 3.2), fontsize=18,
                color=TEAL, weight="bold", ha="center", va="center",
                arrowprops={"arrowstyle": "-", "color": TEAL,
                            "linewidth": 1.0, "shrinkA": 4, "shrinkB": 7}, zorder=8)

    # RGS shares the subjective "good" category and the other codecs' circles.
    legend = fig.add_axes((.092, .147, .862, .045))
    legend.set(xlim=(0, 1), ylim=(0, 1))
    legend.axis("off")
    legend.text(0, .5, "Subjective quality labels:", fontsize=10.5, va="center", color=MUTED)
    for x, color, label in [(.265, lossless, "lossless"), (.402, good, '"good"'), (.527, meh, '"meh"')]:
        legend.scatter([x], [.5], s=90, facecolor=color, edgecolor=INK, linewidth=1)
        legend.text(x + .02, .5, label, va="center", fontsize=10.5)
    legend.scatter([.666], [.5], s=90, marker="o", facecolor=good, edgecolor=INK, linewidth=1)
    legend.text(.688, .5, f'RGS: "{DATA["rendered_landscape"]["rgs_quality_label"]}"',
                va="center", fontsize=10.5, color=TEAL)
    fig.text(.092, .130,
             f"RGS corpus-derived stereo rates: low {low_rate:.0f} · medium {medium_rate:.0f} · high {high_rate:.0f} kb/s.",
             color=TEAL, fontsize=11.2, weight="bold")
    fig.text(.092, .088,
             "RGS adds format rules to QOA's predictor, so it sits above QOA. Vertical distances are illustrative; encoder effort is not shown.\n"
             "Other codec positions and quality labels follow the original subjective sketch. This is not a decode-time comparison.",
             color=MUTED, fontsize=9.4, linespacing=1.5)
    fig.text(.092, .043,
             "RGS, QOA and IMA ADPCM rates use corpus bits per channel sample × 88,200 / 1,000; PCM uses 16-bit payload.\n"
             "Redrawn after phoboslab.org/log/2023/02/qoa-time-domain-audio-compression, with RGS added.",
             color=MUTED, fontsize=9.4, linespacing=1.5)
    save(fig, "rgs-codec-landscape", "Attributed redraw of the subjective QOA codec landscape and Triangle of Neglect, with individual Opus, Vorbis, MP3, WavPack, FLAC, QOA, ADPCM and WAV points. RGS extends from approximately 216 to 285 kb/s and is placed above QOA for its additional format and decoder rules. Vertical distances are illustrative, not decode timings; encoder search effort is not plotted. Quality labels are subjective; RGS is rated good as the project's own assessment.")


def measured_comparison() -> None:
    fig = plt.figure(figsize=(12.8, 8.2))
    fig.text(.065, .927, "MEASURED ON THE SAME 151 ASSETS", color=TEAL, fontsize=11, weight="bold")
    fig.text(.065, .876, "RGS uses less decode time than QOA", fontsize=25, weight="bold")
    fig.text(.065, .833, "RGS medium: 23.7% smaller files · 19.3% less decode time", fontsize=14, color=TEAL)
    ax = fig.add_axes((.105, .25, .84, .52))
    ax.set(xlim=(114, 174), ylim=(.95, 1.32), xlabel="Total encoded size (MiB)  ·  smaller to the left",
           ylabel="Total decode time (seconds)  ·  faster below")
    ax.set_xticks([120, 130, 140, 150, 160, 170])
    ax.set_yticks([1.0, 1.1, 1.2, 1.3])
    ax.grid(color=GRID, linewidth=.8, zorder=0)
    ax.spines[["top", "right"]].set_visible(False)
    ax.xaxis.labelpad = 12
    ax.yaxis.labelpad = 12
    points = {p["id"]: p for p in DATA["measured_scatter"]["points"]}
    specs = [
        ("rgs_medium", TEAL, "o", (119.6, 1.112)),
        ("rgs_low", GOLD, "s", (133.2, .99)),
        ("rgs_high", BLUE, "D", (155.5, 1.105)),
        ("qoa", GRAY, "o", (153.5, 1.268)),
    ]
    for ident, color, marker, label_at in specs:
        point = points[ident]
        x, y = point["encoded_mib"], point["decode_sum_per_file_median_ms"] / 1000
        ax.scatter([x], [y], color=color, marker=marker, s=115,
                   edgecolor=BG, linewidth=1.5, zorder=4)
        ax.annotate(f"{point['label']}\n{x:.2f} MiB · {y:.4f} s",
                    xy=(x, y), xytext=label_at, color=color, fontsize=11,
                    weight="bold", linespacing=1.6, va="center",
                    arrowprops={"arrowstyle": "-", "color": color,
                                "shrinkA": 6, "shrinkB": 7}, zorder=3)
    fig.text(.105, .145, "Windows 11 · Intel i7-12700KF · MSVC 19.44 /O2 · one logical CPU · 7 trials per case", fontsize=10, color=MUTED)
    fig.text(.105, .087, "4,941 seconds of audio. Totals sum per-file medians. Existing encoded inputs; preallocated PCM;\n"
             "checked RGS decoding. File I/O, allocation, warmup and checksum scans are outside the timer.",
             fontsize=9.5, color=MUTED, linespacing=1.5)
    fig.text(.105, .034, "Presets are not quality-matched. Neither axis measures sound quality. Source: September 26, 2026 release data.",
             fontsize=9.4, color=MUTED)
    save(fig, "rgs-size-vs-decode", "Measured size versus decode time for RGS high, medium, low and QOA on 151 identical prepared sources. Presets are not quality-matched. Timings are sums of per-file medians from the September 26, 2026 Windows/MSVC release run.")


if __name__ == "__main__":
    verify_inputs()
    setup()
    landscape_font = DATA["rendered_landscape"]["font_family"]
    try:
        font_manager.findfont(landscape_font, fallback_to_default=False)
    except ValueError as error:
        raise SystemExit(f"Install {landscape_font} to regenerate the subjective landscape chart.") from error
    # The informal typeface reinforces that these positions are illustrative.
    with plt.rc_context({"font.family": landscape_font}):
        landscape()
    measured_comparison()
    print(f"Wrote two figures in SVG and PNG to {ASSETS}")
