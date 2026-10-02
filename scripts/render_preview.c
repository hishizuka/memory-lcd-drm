// SPDX-License-Identifier: GPL-2.0-or-later
/* Run the actual driver converter with the existing userspace DRM stubs.
 * Input: packed RGB bytes on stdin. Output: packed RGB bytes on stdout.
 * Panel codes are visualized with ideal RGB levels, not measured LCD colors.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DMA_FROM_DEVICE 0
int g_param_mono_cutoff = 128;
int g_param_mono_invert;
int g_param_colors = 8;
int g_param_dither_algo;
int g_param_blue_noise_2197;
#include "../src/render.c"

static int argument(const char *text, int min, int max)
{
	char *end;
	long value;
	errno = 0;
	value = strtol(text, &end, 10);
	if (errno || !*text || *end || value < min || value > max) {
		fprintf(stderr, "Invalid argument: %s\n", text);
		exit(2);
	}
	return (int)value;
}

static unsigned int bit_at(const u8 *data, unsigned int bit)
{
	return (data[bit / 8] >> (7 - bit % 8)) & 1;
}

int main(int argc, char **argv)
{
	if (argc != 6 && argc != 7) {
		fprintf(stderr, "Usage: %s width height colors dither mono_cutoff [blue_noise]\n", argv[0]);
		return 2;
	}
	int width = argument(argv[1], 16, 2048);
	int height = argument(argv[2], 1, 2048);
	int colors = argument(argv[3], 2, 64);
	int dither = argument(argv[4], 0, SHARP_DITHER_ALGO_COUNT - 1);
	int cutoff = argument(argv[5], 0, 255);
	int blue_noise = argc == 7 ? argument(argv[6], 0, 1) : 0;
	if (width % 16 || (colors != 2 && colors != 8 && colors != 64)) {
		fprintf(stderr, "Width must be divisible by 16; colors must be 2, 8 or 64\n");
		return 2;
	}
	size_t count = (size_t)width * height;
	size_t plane_len = (size_t)width * 3 / 8;
	struct sharp_subpanel panel = {
		.width = width, .height = height, .panel_type = SHARP_PANEL_TYPE_JDI,
		.line_len_mono = 2 + (size_t)width / 8,
		.line_len_color8 = 2 + plane_len,
		.line_len_color64 = 4 + plane_len * 2,
	};
	u8 *rgb = malloc(count * 3);
	u32 *pixels = calloc(count, sizeof(*pixels));
	u8 *wire = calloc(height, panel.line_len_color64);
	panel.dither_err = calloc((size_t)width * 6, sizeof(*panel.dither_err));
	panel.dither_line_lo = calloc(width, 1);
	panel.dither_line_hi = calloc(width, 1);
	int result = 1;
	if (!rgb || !pixels || !wire || !panel.dither_err ||
		!panel.dither_line_lo || !panel.dither_line_hi) {
		fprintf(stderr, "Allocation failed\n");
		goto out;
	}
	if (fread(rgb, 3, count, stdin) != count || fgetc(stdin) != EOF) {
		fprintf(stderr, "Input size does not match dimensions\n");
		goto out;
	}
	for (size_t i = 0; i < count; i++)
		pixels[i] = ((u32)rgb[i * 3] << 16) |
			((u32)rgb[i * 3 + 1] << 8) | rgb[i * 3 + 2];
	struct drm_gem_dma_object dma = { .vaddr = pixels };
	static const struct drm_format_info format = { .format = DRM_FORMAT_XRGB8888 };
	struct drm_framebuffer fb = {
		.width = width, .height = height, .pitches = { width * 4 },
		.format = &format, .obj = &dma,
	};
	struct drm_rect clip = { .x1 = 0, .y1 = 0, .x2 = width, .y2 = height };
	struct sharp_render_params params = {
		.colors = colors, .dither_algo = dither, .mono_cutoff = cutoff,
		.blue_noise = blue_noise,
	};
	size_t length = 0;
	int rc = sharp_render_clip(&panel, &params, &length, wire, &fb, &clip);
	if (rc || length != sharp_render_panel_line_len(&panel, &params) * height) {
		fprintf(stderr, "Driver conversion failed: %d\n", rc);
		goto out;
	}
	size_t stride = length / height;
	for (int y = 0; y < height; y++) {
		const u8 *lo = wire + y * stride + 2;
		const u8 *hi = lo + plane_len + 2;
		for (int x = 0; x < width; x++) {
			for (int c = 0; c < 3; c++) {
				unsigned int value;
				if (colors == 2)
					value = bit_at(lo, x) * 255;
				else if (colors == 8)
					value = bit_at(lo, x * 3 + c) * 255;
				else
					value = (bit_at(lo, x * 3 + c) +
						2 * bit_at(hi, x * 3 + c)) * 85;
				rgb[((size_t)y * width + x) * 3 + c] = (u8)value;
			}
		}
	}
	if (fwrite(rgb, 3, count, stdout) != count || fflush(stdout)) {
		fprintf(stderr, "Output write failed\n");
		goto out;
	}
	result = 0;
out:
	free(rgb);
	free(pixels);
	free(wire);
	free(panel.dither_err);
	free(panel.dither_line_lo);
	free(panel.dither_line_hi);
	return result;
}
