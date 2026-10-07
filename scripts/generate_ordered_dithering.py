#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Illustrate yellow/red ordered dithering with one shared English image.

Requires Pillow and a C compiler.
The 27/125-color patterns come from the actual driver converter.
"""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from PIL import Image, ImageDraw, PngImagePlugin

from generate_rendering_comparison import ROOT, OUTPUT, convert, verify_unpacking
from generate_sample_images import font


YELLOW = (255, 255, 0)
RED = (255, 0, 0)
INK = "#192b3b"
MUTED = "#4a5c6b"
COPY = {
    "title": "Yellow + red pixels create intermediate shades",
    "guide": "One square = one pixel. Bars illustrate the apparent color at a distance.",
    "modes": (("3 shades", "27 apparent colors"), ("5 shades", "125 apparent colors")),
    "shades": ("Yellow", "Yellow-orange", "Orange", "Red-orange", "Red"),
}


def srgb_channel(linear):
    value = 12.92 * linear if linear <= 0.0031308 else 1.055 * linear ** (1 / 2.4) - 0.055
    return round(255 * value)


def apparent_color(red_fraction):
    """Average ideal yellow/red pixels in linear light, then encode as sRGB."""
    return (255, srgb_channel(1 - red_fraction), 0)


def driver_patterns(executable, algorithm, counts):
    patterns = {}
    for count in counts:
        source = Image.new("RGB", (16, 16), apparent_color(count / 4))
        output = convert(executable, source, 8, algorithm)
        tile = output.crop((0, 0, 2, 2))
        if {color for _, color in output.getcolors()} - {YELLOW, RED}:
            raise RuntimeError("Yellow/red demonstration contains another native color")
        if sum(n for n, color in tile.getcolors() if color == RED) != count:
            raise RuntimeError("Driver pattern does not match the illustrated red fraction")
        for y in range(output.height):
            for x in range(output.width):
                if output.getpixel((x, y)) != tile.getpixel((x % 2, y % 2)):
                    raise RuntimeError("Driver pattern does not repeat on a 2x2 tile")
        patterns[count] = tile
    return patterns


def generate(modes):
    copy = COPY
    canvas = Image.new("RGB", (1200, 550), "#ffffff")
    draw = ImageDraw.Draw(canvas)

    def text(x, y, value, size=24, bold=False, fill=INK, center=False):
        face = font(size, bold)
        if center:
            x -= draw.textlength(value, font=face) / 2
        box = draw.textbbox((x, y), value, font=face, anchor="lt")
        if box[0] < 0 or box[2] > canvas.width or box[3] > canvas.height:
            raise RuntimeError(f"Label outside canvas: {value}")
        draw.text((x, y), value, font=face, fill=fill, anchor="lt")

    def grid(tile, x, y, size):
        # Neutral separators are enlargement guides, not additional pixel colors.
        canvas.paste(tile.resize((size, size), Image.Resampling.NEAREST), (x, y))
        step = size // tile.width
        for i in range(1, tile.width):
            draw.line((x + i * step, y, x + i * step, y + size - 1), fill=INK, width=2)
            draw.line((x, y + i * step, x + size - 1, y + i * step), fill=INK, width=2)
        draw.rectangle((x, y, x + size - 1, y + size - 1), outline=INK, width=2)

    text(36, 30, copy["title"], 36, True)
    text(36, 86, copy["guide"], 23, fill=MUTED)

    centers = (350, 535, 720, 905, 1090)
    for row, (patterns, labels) in enumerate(zip(modes, copy["modes"])):
        top = 140 + row * 210
        title, color_count = labels
        text(36, top + 20, title, 32, True)
        text(36, top + 70, color_count, 24, fill=MUTED)
        for count, tile in patterns.items():
            center = centers[count]
            grid(tile, center - 50, top, 100)
            draw.rectangle((center - 50, top + 116, center + 49, top + 141),
                           fill=apparent_color(count / 4))
            text(center, top + 154, copy["shades"][count], 23, center=True)

    metadata = PngImagePlugin.PngInfo()
    metadata.add_text("Description", copy["title"])
    metadata.add_text("PatternSource", "27/125: src/render.c")
    metadata.add_text("ApparentColors", "Ideal linear-light averages encoded as sRGB, not measured panel colors")
    path = OUTPUT / "ordered-dithering-en.png"
    OUTPUT.mkdir(parents=True, exist_ok=True)
    canvas.save(path, pnginfo=metadata, optimize=True)
    print(path)


def main():
    with tempfile.TemporaryDirectory(prefix="sharp-ordered-dithering-") as temp:
        executable = Path(temp) / "render-preview"
        subprocess.run([
            *shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-O2",
            "-Wall", "-Wextra", "-Wno-sign-compare",
            "-I", str(ROOT / "tests/stubs"), "-I", str(ROOT / "src"),
            str(ROOT / "scripts/render_preview.c"), "-o", str(executable),
        ], check=True)
        verify_unpacking(executable)
        modes = (driver_patterns(executable, 2, (0, 2, 4)),
                 driver_patterns(executable, 3, range(5)))
    generate(modes)
    print("Verified native colors, driver tile proportions, and 2x2 repetition.")


if __name__ == "__main__":
    main()
