#!/usr/bin/env python3
"""Generate landscape test patterns using only the eight native RGB colors."""

from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


OUTPUT_DIR = Path(__file__).resolve().parents[1] / "samples" / "images"
BAYER_4X4 = ((0, 8, 2, 10), (12, 4, 14, 6), (3, 11, 1, 9), (15, 7, 13, 5))
NATIVE_COLORS = [
    (255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0),
    (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0),
]


def font(size, bold=False):
    """Find a readable font on macOS or Linux without bundling font files."""
    candidates = (
        "/System/Library/Fonts/Supplemental/Arial Bold.ttf" if bold
        else "/System/Library/Fonts/Supplemental/Arial.ttf",
        "DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Bold.ttf" if bold
        else "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
    )
    for candidate in candidates:
        try:
            return ImageFont.truetype(candidate, size)
        except OSError:
            pass
    raise RuntimeError("Install Arial, DejaVu Sans, or Liberation Sans to generate samples")


def native_eight_colors(source):
    """Bake intermediate tones into a 4x4 ordered pattern at native resolution."""
    width, height = source.size
    indices = bytearray(width * height)
    pixels = source.load()
    for y in range(height):
        for x in range(width):
            r, g, b = pixels[x, y]
            threshold = BAYER_4X4[y % 4][x % 4] * 16 + 8
            indices[y * width + x] = ((r > threshold) << 2) | ((g > threshold) << 1) | (b > threshold)
    result = Image.frombytes("P", source.size, bytes(indices))
    result.putpalette([
        component
        for i in range(8)
        for component in ((255 if i & 4 else 0), (255 if i & 2 else 0), (255 if i & 1 else 0))
    ])
    return result


def landscape_pattern(width, height, panel_name):
    image = Image.new("RGB", (width, height), "white")
    draw = ImageDraw.Draw(image)
    draw.fontmode = "1"
    sx, sy = width / 400, height / 240
    scale = min(sx, sy)

    def xy(x, y):
        return round(x * sx), round(y * sy)

    def label(x, y, value, size=11, bold=False):
        draw.text(xy(x, y), value, fill="black", font=font(round(size * scale), bold))

    draw.rectangle((0, 0, width - 1, height - 1), outline="black", width=2)
    label(9, 7, panel_name, 27, True)
    label(236, 12, f"{width} x {height}", 14, True)
    label(236, 30, "8 COLORS / SHARP DRM", 11)

    for i, color in enumerate(NATIVE_COLORS):
        left, top = xy(8 + 48 * i, 55)
        right, bottom = xy(8 + 48 * (i + 1), 85)
        draw.rectangle((left, top, right - 1, bottom - 1), fill=color)
        label(28 + i * 48, 88, "WYCGMRBK"[i], 11, True)

    # Reflow the original mountain scene to the left side of the panel.
    left, top = xy(8, 108)
    right, bottom = xy(248, 219)
    scene_width, scene_height = right - left, bottom - top

    def scene_point(x, y):
        return left + round(x * scene_width / 256), top + round(y * scene_height / 138)

    def scene_polygon(points, color):
        draw.polygon([scene_point(*point) for point in points], fill=color)

    draw.rectangle((left, top, right - 1, bottom - 1), fill=(85, 170, 255))
    sun_x, sun_y = scene_point(197, 34)
    radius = round(min(scene_width / 256, scene_height / 138) * 22)
    draw.ellipse((sun_x - radius, sun_y - radius, sun_x + radius, sun_y + radius), fill="yellow")
    draw.rectangle((*scene_point(0, 107), right - 1, bottom - 1), fill=(0, 85, 170))
    scene_polygon([(0, 107), (74, 18), (149, 107)], (85, 85, 85))
    scene_polygon([(44, 54), (74, 18), (104, 54), (89, 47), (75, 60), (60, 44)], "white")
    scene_polygon([(82, 107), (175, 50), (255, 107)], (0, 170, 85))
    scene_polygon([(0, 121), (80, 98), (147, 118), (197, 101), (255, 114), (255, 137), (0, 137)], (0, 85, 0))

    label(256, 107, "DITHER / 27 TONES", 11, True)
    for i in range(27):
        levels = (0, 128, 255)
        color = (levels[i // 9], levels[(i // 3) % 3], levels[i % 3])
        x, y = 256 + (i % 9) * 15, 125 + (i // 9) * 13
        left, top = xy(x, y)
        right, bottom = xy(x + 15, y + 13)
        draw.rectangle((left, top, right - 1, bottom - 1), fill=color)

    label(256, 170, "GRAYSCALE", 10, True)
    left, top = xy(256, 185)
    right, bottom = xy(391, 198)
    for x in range(left, right):
        value = round((x - left) * 255 / (right - left - 1))
        draw.line((x, top, x, bottom - 1), fill=(value, value, value))

    label(256, 205, "1px / 2px", 10)
    # Keep these checks at one and two physical pixels in both resolutions.
    for start, cell in ((313, 1), (355, 2)):
        left, top = xy(start, 204)
        right, bottom = xy(start + 36, 219)
        for y in range(top, bottom):
            for x in range(left, right):
                value = 255 if ((x - left) // cell + (y - top) // cell) % 2 else 0
                draw.point((x, y), fill=(value, value, value))

    label(9, 225, "MEMORY LCD / NATIVE 8-COLOR TEST", 10)
    return native_eight_colors(image)


def main():
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    for width, height, name in ((400, 240, "JDI 2.7"), (640, 480, "JDI 4.4")):
        image = landscape_pattern(width, height, name)
        path = OUTPUT_DIR / f"test-pattern-{width}x{height}-8color.png"
        image.save(path, bits=4, optimize=True)
        print(path)


if __name__ == "__main__":
    main()
