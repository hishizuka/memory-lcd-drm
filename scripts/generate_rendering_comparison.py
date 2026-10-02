#!/usr/bin/env python3
"""Create a reproducible comparison from src/render.c, not a reimplementation.

Requires Pillow and a C compiler. Run from any directory:
    python3 scripts/generate_rendering_comparison.py
    python3 scripts/generate_rendering_comparison.py --source path/to/image.png

The default source is docs/images/rendering-source.png. No resizing is applied.
Source width must be divisible by 16, matching the converter harness.

Default source: Pi Zero Bikecomputer tmp/20260818-ome-3d2b-color-comparison/
ome-3d2b-source.png (272 x 451). Map data: OpenStreetMap contributors;
processed elevation data: Geospatial Information Authority of Japan.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from PIL import Image, ImageDraw, PngImagePlugin

from generate_sample_images import font

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "docs" / "images"
MONO_CUTOFF = 188
MONO_DITHER_CUTOFF = 128
MODES = [
    ("Original", "Full-color RGB source", None, 0, 0, 128),
    ("2-color mono", f"colors=2 / dither_algo=0\nmono_cutoff={MONO_CUTOFF}", 2, 0, 0, MONO_CUTOFF),
    ("Mono ordered", f"colors=2 / dither_algo=1\nmono_cutoff={MONO_DITHER_CUTOFF}", 2, 1, 0, MONO_DITHER_CUTOFF),
    ("8-color direct", "colors=8 / dither_algo=0", 8, 0, 0, 128),
    ("27-color dithered", "colors=8 / dither_algo=2", 8, 2, 0, 128),
    ("125-color dithered", "colors=8 / dither_algo=3", 8, 3, 0, 128),
    ("8-color errdiff2", "colors=8 / dither_algo=6", 8, 6, 0, 128),
    ("64-color direct", "colors=64 / dither_algo=0", 64, 0, 0, 128),
    ("343-color dithered", "colors=64 / dither_algo=4", 64, 4, 0, 128),
    ("2197-color ordered", "colors=64 / dither_algo=5\nblue_noise_2197=0", 64, 5, 0, 128),
    ("64-color errdiff2", "colors=64 / dither_algo=6", 64, 6, 0, 128),
    ("64-color blue noise", "colors=64 / dither_algo=5\nblue_noise_2197=1", 64, 5, 1, 128),
]


GROUPS = [
    ("SOURCE & MONOCHROME", range(0, 3)),
    ("8-COLOR OUTPUT", range(3, 7)),
    ("64-COLOR OUTPUT", range(7, 12)),
]


def convert(executable, source, colors, dither, blue=0, cutoff=128):
    result = subprocess.run(
        [str(executable), *map(str, source.size), str(colors), str(dither), str(cutoff), str(blue)],
        input=source.tobytes(), stdout=subprocess.PIPE, check=True,
    )
    expected = source.width * source.height * 3
    if len(result.stdout) != expected:
        raise RuntimeError("Unexpected converter output length")
    image = Image.frombytes("RGB", source.size, result.stdout)
    levels = {0, 255} if colors in (2, 8) else {0, 85, 170, 255}
    if not set(result.stdout) <= levels:
        raise RuntimeError("Decoded output contains non-native channel levels")
    if image.getcolors(maxcolors=colors) is None:
        raise RuntimeError("Decoded output exceeds the native palette")
    return image


def verify_unpacking(executable):
    """Check all 64 wire codes against their exact ideal RGB values."""
    chart = Image.new("RGB", (64, 1))
    chart.putdata([(r, g, b) for r in (0, 85, 170, 255)
                   for g in (0, 85, 170, 255) for b in (0, 85, 170, 255)])
    if convert(executable, chart, 64, 0).tobytes() != chart.tobytes():
        raise RuntimeError("64-color wire decoding failed the native palette check")
    primary = Image.new("RGB", (16, 1))
    primary_pixels = [(r, g, b) for r in (0, 255) for g in (0, 255)
                      for b in (0, 255)] * 2
    primary.putdata(primary_pixels)
    for _, _, colors, dither, blue, _ in MODES:
        if colors in (8, 64):
            if convert(executable, primary, colors, dither, blue).tobytes() != primary.tobytes():
                raise RuntimeError("Native RGB primaries changed during conversion")
    mono = convert(executable, primary, 2, 0)
    expected = [(255,) * 3 if (77*r + 150*g + 29*b) >= 128*256 else (0,) * 3
                for r, g, b in primary_pixels]
    if mono.tobytes() != bytes(v for pixel in expected for v in pixel):
        raise RuntimeError("Monochrome wire decoding failed the threshold check")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=OUTPUT / "rendering-source.png")
    args = parser.parse_args()
    source = Image.open(args.source).convert("RGB")
    if source.width % 16 or not (16 <= source.width <= 2048 and 1 <= source.height <= 2048):
        parser.error("Source width must be divisible by 16; dimensions must be at most 2048")
    with tempfile.TemporaryDirectory(prefix="sharp-render-preview-") as temp:
        executable = Path(temp) / "render-preview"
        subprocess.run([
            *shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-O2",
            "-Wall", "-Wextra", "-Wno-sign-compare",
            "-I", str(ROOT / "tests/stubs"), "-I", str(ROOT / "src"),
            str(ROOT / "scripts/render_preview.c"), "-o", str(executable),
        ], check=True)
        verify_unpacking(executable)
        images = [source if colors is None else convert(executable, source, colors, dither, blue, cutoff)
                  for _, _, colors, dither, blue, cutoff in MODES]
    margin, gap = 32, 20
    card_w, card_h = max(source.width + 24, 296), source.height + 100
    width = margin * 2 + card_w * 5 + gap * 4
    group_h = 48 + card_h + 32
    footer_y = 110 + len(GROUPS) * group_h
    canvas = Image.new("RGB", (width, footer_y + 94), "#eef2f6")
    draw = ImageDraw.Draw(canvas)
    draw.text((margin, 22), "MEMORY LCD  /  RENDERING MODES", font=font(32, True), fill="#142638")
    draw.text((margin, 66), "One source image + eleven modes. Actual driver conversion output.",
              font=font(20), fill="#465b6e")
    for row, (heading, indices) in enumerate(GROUPS):
        group_y = 110 + row * group_h
        draw.text((margin, group_y), heading, font=font(24, True), fill="#142638")
        for col, i in enumerate(indices):
            title, detail, colors, _, _, _ = MODES[i]
            x = margin + col * (card_w + gap)
            y = group_y + 48
            draw.rounded_rectangle((x, y, x + card_w, y + card_h), radius=12, fill="white")
            accent = "#126e82" if colors == 8 else "#7756a4" if colors == 64 else "#66798c"
            draw.rectangle((x + 12, y + 14, x + 16, y + 38), fill=accent)
            draw.text((x + 24, y + 12), title, font=font(23, True), fill="#142638")
            draw.text((x + 12, y + 45), detail, font=font(16), fill="#526476")
            canvas.paste(images[i], (x + 12, y + 92))
    draw.text((margin, footer_y), f"{source.width} x {source.height} pixels per image. Dithered counts describe apparent shades; output stays at 2, 8 or 64 colors.",
              font=font(16), fill="#354f65")
    draw.text((margin, footer_y + 25), "Ideal RGB preview, not a panel photograph. Open at full size to inspect the dither patterns.",
              font=font(16), fill="#526476")
    draw.text((margin, footer_y + 51), "Map: Pi Zero Bikecomputer / (c) OpenStreetMap contributors. Elevation: GSI Japan (processed).",
              font=font(15), fill="#526476")
    metadata = PngImagePlugin.PngInfo()
    metadata.add_text("Description", "Driver output decoded to ideal RGB; not measured LCD colorimetry")
    metadata.add_text("Modes", json.dumps(MODES))
    metadata.add_text("SourceSHA256", hashlib.sha256(source.tobytes()).hexdigest())
    metadata.add_text("RendererSHA256", hashlib.sha256((ROOT / "src/render.c").read_bytes()).hexdigest())
    OUTPUT.mkdir(parents=True, exist_ok=True)
    source.save(OUTPUT / "rendering-source.png", optimize=True)
    canvas.save(OUTPUT / "rendering-modes.png", pnginfo=metadata, optimize=True)
    print(OUTPUT / "rendering-modes.png")
    print("Verified native RGB palettes, monochrome threshold, output lengths and color counts.")


if __name__ == "__main__":
    main()
