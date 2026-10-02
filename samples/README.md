# Sample images

English | [日本語](README_ja.md)

Test patterns for checking RGB channel order, panel resolution, color rendition,
and fine pixel detail. Each image is the exact size of its target panel.

| Image | Panel | Resolution | Color mode |
| --- | --- | --- | --- |
| [Landscape, 400x240](images/test-pattern-400x240-8color.png) | JDI LPM027M128B / LPM027M128C | 400x240 | 8 colors |
| [Landscape, 640x480](images/test-pattern-640x480-8color.png) | JDI LPM044M141A | 640x480 | 8 colors |
| [Original portrait, 272x451](images/test-pattern-272x451-64color.png) | AUO U340QBN01 | 272x451 | 64 colors |

![400x240 sample](images/test-pattern-400x240-8color.png)

![640x480 sample](images/test-pattern-640x480-8color.png)

![272x451 sample](images/test-pattern-272x451-64color.png)

The landscape PNGs use exactly eight RGB colors: black, white, red, green, blue,
cyan, magenta, and yellow. Intermediate tones in the mountain scene, 27-tone
swatches, and grayscale ramp use a baked-in 4x4 ordered dither. Text, color bars,
and the 1-pixel / 2-pixel checkerboards use native colors directly.

Display at 1:1 scale without resampling to preserve these checks. Set the driver
to `colors=8`; `dither_algo=0` provides a direct check of the baked-in pixels.
The samples do not require a backlight.

The portrait file is the original RGB PNG displayed and visually confirmed on
an AUO panel connected to `anttest.local`. It retains the original footer. It
contains intermediate RGB tones for the driver's 64-color conversion, so the
PNG itself has more than 64 unique colors. The two landscape files have been
checked for dimensions, pixel colors, and layout, but have not been displayed
on their target panels yet.

## Regenerate the landscape images

Install Pillow and run from the repository root:

```sh
python3 -m pip install Pillow
python3 scripts/generate_sample_images.py
```

The script uses Arial on macOS, or DejaVu Sans / Liberation Sans on Linux.
Font selection can change text glyphs. The original portrait PNG is preserved
and is not overwritten by this script.
