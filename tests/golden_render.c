// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * User-space golden byte tests for src/render.c conversion routines.
 *
 * This harness directly includes the driver translation unit and supplies
 * minimal kernel/DRM shims from tests/stubs. SPI and DRM device operations are
 * compiled only to satisfy references; test cases exercise conversion output.
 */

#include <errno.h>
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DMA_FROM_DEVICE 0
#define ENODEV 19
#define ENOMEM 12

int g_param_mono_cutoff = 32;
int g_param_mono_invert;
int g_param_colors = 8;
int g_param_dither_algo = 2;
int g_param_dither_algo_user;
int g_param_blue_noise_2197;

#include "../src/render.c"

#define PANEL_WIDTH 272
#define PANEL_HEIGHT 451
#define DUAL_WIDTH 544
#define SHA256_DIGEST_SIZE 32

struct sha256_ctx {
	u32 state[8];
	u64 bit_len;
	u8 data[64];
	size_t data_len;
};

static const u32 sha256_k[64] = {
	0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
	0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
	0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
	0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
	0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
	0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
	0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
	0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
	0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
	0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
	0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
	0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
	0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
	0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
	0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
	0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static int check_optimized_quantizers(void)
{
	static const u8 reference_rank_4x4[16] = {
		0, 8, 2, 10,
		12, 4, 14, 6,
		3, 11, 1, 9,
		15, 7, 13, 5
	};
	int rank;
	int value;
	int level;
	int y;

	for (y = 0; y < 4; y++) {
		int x;

		for (x = 0; x < 4; x++) {
			u8 expected = (u8)(reference_rank_4x4[x + (y * 4)] >> 2);
			u8 actual = sharp_dither_2197_rank_bucket[
				(x & 1) + ((y & 1) * 2)];

			if (expected != actual) {
				fprintf(stderr,
					"2197 rank bucket mismatch x=%d y=%d\n",
					x, y);
				return 1;
			}
		}
	}

	for (rank = 0; rank < 4; rank++) {
		for (value = 0; value < 256; value++) {
			int reference_level = (value >= 100)
				+ (value >= 165)
				+ (value >= 208)
				+ (value >= 241);
			bool expected = rank < reference_level;
			bool actual = value >= sharp_dither_125_threshold[rank];

			if (expected != actual) {
				fprintf(stderr,
					"125 quantizer mismatch rank=%d value=%d\n",
					rank, value);
				return 1;
			}
		}
	}

	for (level = 0; level <= 12; level++) {
		int remainder = level & 3;
		int base = level >> 2;

		for (rank = 0; rank < 1024; rank++) {
			int expected = base;
			u8 rank_bucket = (u8)(rank >> 8);
			u8 actual;

			if (remainder && rank < remainder * 256) {
				expected++;
			}
			actual = sharp_quantize_2197_channel((u8)level,
				rank_bucket);
			if (expected != actual) {
				fprintf(stderr,
					"2197 quantizer mismatch level=%d rank=%d\n",
					level, rank);
				return 1;
			}
		}
	}

	return 0;
}

static u32 rotr32(u32 value, unsigned int bits)
{
	return (value >> bits) | (value << (32u - bits));
}

static void sha256_transform(struct sha256_ctx *ctx, const u8 data[64])
{
	u32 m[64];
	u32 a, b, c, d, e, f, g, h;
	int i;

	for (i = 0; i < 16; i++) {
		m[i] = ((u32)data[i * 4] << 24)
			| ((u32)data[(i * 4) + 1] << 16)
			| ((u32)data[(i * 4) + 2] << 8)
			| (u32)data[(i * 4) + 3];
	}
	for (i = 16; i < 64; i++) {
		u32 s0 = rotr32(m[i - 15], 7) ^ rotr32(m[i - 15], 18)
			^ (m[i - 15] >> 3);
		u32 s1 = rotr32(m[i - 2], 17) ^ rotr32(m[i - 2], 19)
			^ (m[i - 2] >> 10);

		m[i] = m[i - 16] + s0 + m[i - 7] + s1;
	}

	a = ctx->state[0];
	b = ctx->state[1];
	c = ctx->state[2];
	d = ctx->state[3];
	e = ctx->state[4];
	f = ctx->state[5];
	g = ctx->state[6];
	h = ctx->state[7];

	for (i = 0; i < 64; i++) {
		u32 s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
		u32 ch = (e & f) ^ ((~e) & g);
		u32 temp1 = h + s1 + ch + sha256_k[i] + m[i];
		u32 s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
		u32 maj = (a & b) ^ (a & c) ^ (b & c);
		u32 temp2 = s0 + maj;

		h = g;
		g = f;
		f = e;
		e = d + temp1;
		d = c;
		c = b;
		b = a;
		a = temp1 + temp2;
	}

	ctx->state[0] += a;
	ctx->state[1] += b;
	ctx->state[2] += c;
	ctx->state[3] += d;
	ctx->state[4] += e;
	ctx->state[5] += f;
	ctx->state[6] += g;
	ctx->state[7] += h;
}

static void sha256_init(struct sha256_ctx *ctx)
{
	ctx->data_len = 0;
	ctx->bit_len = 0;
	ctx->state[0] = 0x6a09e667u;
	ctx->state[1] = 0xbb67ae85u;
	ctx->state[2] = 0x3c6ef372u;
	ctx->state[3] = 0xa54ff53au;
	ctx->state[4] = 0x510e527fu;
	ctx->state[5] = 0x9b05688cu;
	ctx->state[6] = 0x1f83d9abu;
	ctx->state[7] = 0x5be0cd19u;
}

static void sha256_update(struct sha256_ctx *ctx, const u8 *data, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		ctx->data[ctx->data_len++] = data[i];
		if (ctx->data_len == sizeof(ctx->data)) {
			sha256_transform(ctx, ctx->data);
			ctx->bit_len += 512;
			ctx->data_len = 0;
		}
	}
}

static void sha256_final(struct sha256_ctx *ctx, u8 hash[SHA256_DIGEST_SIZE])
{
	size_t i = ctx->data_len;
	int j;

	ctx->data[i++] = 0x80;
	if (i > 56) {
		while (i < 64) {
			ctx->data[i++] = 0;
		}
		sha256_transform(ctx, ctx->data);
		i = 0;
	}
	while (i < 56) {
		ctx->data[i++] = 0;
	}

	ctx->bit_len += (u64)ctx->data_len * 8u;
	for (j = 7; j >= 0; j--) {
		ctx->data[56 + (7 - j)] = (u8)(ctx->bit_len >> (j * 8));
	}
	sha256_transform(ctx, ctx->data);

	for (i = 0; i < 4; i++) {
		for (j = 0; j < 8; j++) {
			hash[(j * 4) + i] = (u8)(ctx->state[j] >> (24 - (i * 8)));
		}
	}
}

static void sha256_hex(const u8 *data, size_t len, char out[65])
{
	static const char hex[] = "0123456789abcdef";
	struct sha256_ctx ctx;
	u8 digest[SHA256_DIGEST_SIZE];
	int i;

	sha256_init(&ctx);
	sha256_update(&ctx, data, len);
	sha256_final(&ctx, digest);

	for (i = 0; i < SHA256_DIGEST_SIZE; i++) {
		out[i * 2] = hex[digest[i] >> 4];
		out[(i * 2) + 1] = hex[digest[i] & 0x0f];
	}
	out[64] = '\0';
}

static u32 lcg_next(u32 *state)
{
	*state = (*state * 1664525u) + 1013904223u;
	return *state;
}

static u32 make_pixel(int x, int y, int width, int height, u32 *rng)
{
	u8 grad_x = (u8)((x * 255) / (width - 1));
	u8 grad_y = (u8)((y * 255) / (height - 1));
	u8 noise = (u8)(lcg_next(rng) >> 24);
	u8 r = (u8)((grad_x * 5u + noise * 3u) / 8u);
	u8 g = (u8)((grad_y * 3u + (255u - grad_x) * 2u + noise) / 6u);
	u8 b = (u8)(((x ^ y) & 0xff) ^ (noise >> 1));

	if (x < width / 5 && y < height / 4) {
		r = 0x00;
		g = 0x00;
		b = 0x00;
	} else if (x >= (width * 4) / 5 && y < height / 3) {
		r = 0xff;
		g = 0xff;
		b = 0xff;
	} else if (x >= width / 3 && x < (width * 2) / 3
		&& y >= height / 3 && y < (height * 2) / 3) {
		r = 0xd8;
		g = 0x30;
		b = 0x70;
	}

	return 0xff000000u | ((u32)r << 16) | ((u32)g << 8) | b;
}

static void fill_image(u32 *pixels, int width, int height)
{
	u32 rng = 0x5eed1234u ^ (u32)width;
	int y;

	for (y = 0; y < height; y++) {
		int x;

		for (x = 0; x < width; x++) {
			pixels[(y * width) + x] = make_pixel(x, y, width, height, &rng);
		}
	}
}

static void init_panel(struct sharp_subpanel *panel, int width, int height,
	u8 *buf, s16 *dither_err, u8 *line_lo, u8 *line_hi)
{
	memset(panel, 0, sizeof(*panel));
	panel->width = (unsigned int)width;
	panel->height = (unsigned int)height;
	panel->panel_type = SHARP_PANEL_TYPE_JDI;
	panel->line_len_mono = 2u + ((size_t)width / 8u);
	panel->line_len_color8 = 2u + (((size_t)width * 3u + 7u) / 8u);
	panel->line_len_color64 = 4u + ((((size_t)width * 3u + 7u) / 8u) * 2u);
	panel->max_line_len = panel->line_len_color64;
	panel->buf = buf;
	panel->dither_err = dither_err;
	panel->dither_line_lo = line_lo;
	panel->dither_line_hi = line_hi;
	sharp_render_init_nv12_tables(panel);
}

static int run_case(const char *name, int width, int height, int colors,
	int dither, int blue_noise, int mono_cutoff, int mono_invert,
	char hash[65])
{
	u32 *pixels = calloc((size_t)width * (size_t)height, sizeof(*pixels));
	u8 *buf = calloc((size_t)width * (size_t)height, 1);
	s16 *dither_err = calloc((size_t)width * 3u * 2u, sizeof(*dither_err));
	u8 *line_lo = calloc((size_t)width, 1);
	u8 *line_hi = calloc((size_t)width, 1);
	struct sharp_subpanel panel;
	struct drm_gem_dma_object dma_obj;
	struct drm_framebuffer fb;
	struct drm_rect clip;
	struct sharp_render_params params;
	static const struct drm_format_info xrgb8888 = {
		.format = DRM_FORMAT_XRGB8888,
	};
	size_t result_len = 0;
	int rc;

	(void)name;

	if (!pixels || !buf || !dither_err || !line_lo || !line_hi) {
		free(pixels);
		free(buf);
		free(dither_err);
		free(line_lo);
		free(line_hi);
		return ENOMEM;
	}

	fill_image(pixels, width, height);
	init_panel(&panel, width, height, buf, dither_err, line_lo, line_hi);

	dma_obj.vaddr = pixels;
	memset(&fb, 0, sizeof(fb));
	fb.width = (unsigned int)width;
	fb.height = (unsigned int)height;
	fb.pitches[0] = (unsigned int)width * 4u;
	fb.format = &xrgb8888;
	fb.obj = &dma_obj;

	clip.x1 = 0;
	clip.y1 = 0;
	clip.x2 = width;
	clip.y2 = height;

	params.colors = colors;
	params.dither_algo = dither;
	params.blue_noise = blue_noise;
	params.mono_cutoff = mono_cutoff;
	params.mono_invert = mono_invert;

	rc = sharp_render_clip(&panel, &params, &result_len, buf, &fb, &clip);

	if (!rc) {
		sha256_hex(buf, result_len, hash);
	}

	free(pixels);
	free(buf);
	free(dither_err);
	free(line_lo);
	free(line_hi);
	return rc;
}

static int emit_case(FILE *out, const char *name, int width, int height,
	int colors, int dither, int blue_noise, int mono_cutoff, int mono_invert)
{
	char hash[65];
	int rc = run_case(name, width, height, colors, dither, blue_noise,
		mono_cutoff, mono_invert, hash);

	if (rc) {
		fprintf(stderr, "%s: failed with rc=%d\n", name, rc);
		return rc;
	}

	fprintf(out, "%s %s\n", name, hash);
	return 0;
}

struct golden_entry {
	char name[128];
	char hash[65];
};

struct golden_file {
	struct golden_entry *entries;
	size_t count;
};

static int load_golden(const char *path, struct golden_file *golden)
{
	FILE *fp = fopen(path, "r");
	char line[256];
	size_t capacity = 0;

	golden->entries = NULL;
	golden->count = 0;
	if (!fp) {
		fprintf(stderr, "failed to open %s: %s\n", path, strerror(errno));
		return errno ? errno : EINVAL;
	}

	while (fgets(line, sizeof(line), fp)) {
		char name[128];
		char hash[65];

		if (line[0] == '\n' || line[0] == '#') {
			continue;
		}
		if (sscanf(line, "%127s %64s", name, hash) != 2) {
			fprintf(stderr, "invalid golden line: %s", line);
			fclose(fp);
			return EINVAL;
		}
		if (golden->count == capacity) {
			size_t new_capacity = capacity ? capacity * 2u : 32u;
			struct golden_entry *new_entries = realloc(golden->entries,
				new_capacity * sizeof(*new_entries));

			if (!new_entries) {
				fclose(fp);
				free(golden->entries);
				golden->entries = NULL;
				golden->count = 0;
				return ENOMEM;
			}
			golden->entries = new_entries;
			capacity = new_capacity;
		}
		snprintf(golden->entries[golden->count].name,
			sizeof(golden->entries[golden->count].name), "%s", name);
		snprintf(golden->entries[golden->count].hash,
			sizeof(golden->entries[golden->count].hash), "%s", hash);
		golden->count++;
	}

	fclose(fp);
	return 0;
}

static const char *golden_lookup(const struct golden_file *golden,
	const char *name)
{
	size_t i;

	for (i = 0; i < golden->count; i++) {
		if (strcmp(golden->entries[i].name, name) == 0) {
			return golden->entries[i].hash;
		}
	}
	return NULL;
}

static int check_case(const struct golden_file *golden, const char *name,
	int width, int height, int colors, int dither, int blue_noise,
	int mono_cutoff, int mono_invert)
{
	char actual[65];
	const char *expected = golden_lookup(golden, name);
	int rc;

	if (!expected) {
		fprintf(stderr, "%s: missing from golden file\n", name);
		return 1;
	}

	rc = run_case(name, width, height, colors, dither, blue_noise,
		mono_cutoff, mono_invert, actual);
	if (rc) {
		fprintf(stderr, "%s: failed with rc=%d\n", name, rc);
		return 1;
	}
	if (strcmp(expected, actual) != 0) {
		fprintf(stderr, "%s: hash mismatch expected=%s actual=%s\n",
			name, expected, actual);
		return 1;
	}
	return 0;
}

static int check_nv12_render(void)
{
	enum { width = 32, panel_width = 16, height = 7, pitch = 48, offset = 32 };
	static const struct drm_format_info xrgb8888 = {
		.format = DRM_FORMAT_XRGB8888,
	};
	static const struct drm_format_info nv12 = {
		.format = DRM_FORMAT_NV12,
	};
	u32 pixels[offset / 4 + pitch * height];
	u8 y_plane[offset + pitch * height];
	u8 uv_plane[offset + pitch * ((height + 1) / 2)];
	u8 xrgb_buf[width * height];
	u8 nv12_buf[width * height];
	s16 xrgb_err[width * 3 * 2] = { 0 };
	s16 nv12_err[width * 3 * 2] = { 0 };
	u8 xrgb_lo[width] = { 0 };
	u8 xrgb_hi[width] = { 0 };
	u8 nv12_lo[width] = { 0 };
	u8 nv12_hi[width] = { 0 };
	u32 nv12_source_row[width] = { 0 };
	struct drm_gem_dma_object xrgb_obj = { .vaddr = pixels };
	struct drm_gem_dma_object y_obj = { .vaddr = y_plane };
	struct drm_gem_dma_object uv_obj = { .vaddr = uv_plane };
	struct drm_framebuffer xrgb_fb = {
		.width = width,
		.height = height,
		.pitches = { pitch * 4, 0, 0, 0 },
		.offsets = { offset },
		.format = &xrgb8888,
		.obj = &xrgb_obj,
	};
	struct drm_framebuffer nv12_fb = {
		.width = width,
		.height = height,
		.pitches = { pitch, pitch, 0, 0 },
		.offsets = { offset, offset },
		.format = &nv12,
		.objs = { &y_obj, &uv_obj, NULL, NULL },
	};
	struct sharp_subpanel xrgb_panel;
	struct sharp_subpanel nv12_panel;
	struct drm_rect clip = { 0, 0, width, height };
	struct sharp_render_params params = { .colors = 8, .mono_cutoff = 128 };
	size_t xrgb_len = 0;
	size_t nv12_len = 0;
	int y;
	int x;
	int dither;
	int colors, blue, side, partial;

	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			y_plane[offset + (y * pitch) + x] =
				(u8)((x * 17 + y * 41) % 256);
		}
	}
	for (y = 0; y < (height + 1) / 2; y++) {
		for (x = 0; x < width; x += 2) {
			uv_plane[offset + (y * pitch) + x] =
				(u8)((x * 11 + y * 53) % 256);
			uv_plane[offset + (y * pitch) + x + 1] =
				(u8)((x * 7 + y * 67) % 256);
		}
	}

	init_panel(&xrgb_panel, panel_width, height, xrgb_buf, xrgb_err,
		xrgb_lo, xrgb_hi);
	init_panel(&nv12_panel, panel_width, height, nv12_buf, nv12_err,
		nv12_lo, nv12_hi);
	nv12_panel.source_row = nv12_source_row;

	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			int uv_offset = offset + ((y >> 1) * pitch) + (x & ~1);
			u8 source_y = y_plane[offset + (y * pitch) + x];
			u8 u = uv_plane[uv_offset];
			u8 v = uv_plane[uv_offset + 1];
			int luma = nv12_panel.nv12_y_scaled[source_y];
			u8 r = sharp_clamp_u8((luma
				+ nv12_panel.nv12_r_chroma[v] + 128) >> 8);
			u8 g = sharp_clamp_u8((luma
				+ nv12_panel.nv12_g_u_chroma[u]
				+ nv12_panel.nv12_g_v_chroma[v] + 128) >> 8);
			u8 b = sharp_clamp_u8((luma
				+ nv12_panel.nv12_b_chroma[u] + 128) >> 8);

			pixels[offset / 4 + (y * pitch) + x] =
				((u32)r << 16) | ((u32)g << 8) | b;
		}
	}

	for (colors = 8; colors <= 64; colors *= 8)
	for (blue = 0; blue <= 1; blue++)
	for (side = 0; side < 2; side++)
	for (partial = 0; partial < 2; partial++)
	for (dither = SHARP_DITHER_ALGO_NONE;
		dither < SHARP_DITHER_ALGO_COUNT; dither++) {
		int rc;

		memset(xrgb_buf, 0, sizeof(xrgb_buf));
		memset(nv12_buf, 0, sizeof(nv12_buf));
		params.colors = colors;
		params.blue_noise = blue;
		params.dither_algo = dither;
		clip.x1 = side * panel_width;
		clip.x2 = clip.x1 + panel_width;
		clip.y1 = partial && dither != SHARP_DITHER_ALGO_ERRDIFF2 ? 3 : 0;
		xrgb_panel.x_offset = nv12_panel.x_offset = clip.x1;
		rc = sharp_render_clip(&xrgb_panel, &params, &xrgb_len, xrgb_buf,
			&xrgb_fb, &clip);
		if (rc) {
			fprintf(stderr, "XRGB reference render failed with rc=%d\n", rc);
			return 1;
		}
		rc = sharp_render_clip(&nv12_panel, &params, &nv12_len, nv12_buf,
			&nv12_fb, &clip);
		if (rc) {
			fprintf(stderr, "NV12 render failed with rc=%d\n", rc);
			return 1;
		}
		if (xrgb_len != nv12_len
			|| memcmp(xrgb_buf, nv12_buf, xrgb_len) != 0) {
			fprintf(stderr,
				"NV12 differs: colors=%d dither=%d blue=%d side=%d partial=%d\n",
				colors, dither, blue, side, partial);
			return 1;
		}
	}

	return 0;
}

static int check_mono_ordered(void)
{
	enum { width = 16, height = 8, pitch = 24, offset = 16,
		line_len = 2 + width / 8, payload_len = height * width / 8 };
	static const struct drm_format_info xrgb8888 = {
		.format = DRM_FORMAT_XRGB8888,
	};
	static const struct drm_format_info nv12 = { .format = DRM_FORMAT_NV12 };
	u32 pixels[offset / 4 + pitch * height];
	u8 y_plane[offset + pitch * height];
	u8 uv_plane[offset + pitch * height / 2];
	u8 buf[line_len * height], inverted[line_len * height];
	u8 partial[line_len * height], previous_cutoff[256][payload_len];
	u32 source_row[width];
	struct drm_gem_dma_object rgb_obj = { .vaddr = pixels };
	struct drm_gem_dma_object y_obj = { .vaddr = y_plane };
	struct drm_gem_dma_object uv_obj = { .vaddr = uv_plane };
	struct drm_framebuffer fb = {
		.width = width, .height = height, .pitches = { pitch * 4 },
		.offsets = { offset }, .format = &xrgb8888, .obj = &rgb_obj,
	};
	struct drm_framebuffer yuv_fb = {
		.width = width, .height = height, .pitches = { pitch, pitch },
		.offsets = { offset, offset }, .format = &nv12,
		.objs = { &y_obj, &uv_obj },
	};
	struct sharp_subpanel panel;
	struct sharp_render_params params = {
		.colors = 2, .dither_algo = SHARP_DITHER_ALGO_MONO_ORDERED,
	};
	struct drm_rect full = { 0, 0, width, height };
	struct drm_rect clipped = { 0, 3, width, 7 };
	size_t length, partial_length;
	int cutoff, value, y, x;

	init_panel(&panel, width, height, buf, NULL, NULL, NULL);
	panel.source_row = source_row;
	memset(pixels, 0xa5, sizeof(pixels));
	memset(y_plane, 0xa5, sizeof(y_plane));
	memset(uv_plane, 0, sizeof(uv_plane));

	/* Exhaust every cutoff/gray pair; avoid accepting newly generated hashes. */
	for (cutoff = 0; cutoff <= 255; cutoff++) {
		u8 previous_gray[payload_len] = { 0 };

		params.mono_cutoff = cutoff;
		params.mono_invert = 0;
		for (value = 0; value <= 255; value++) {
			int white_count = 0;

			for (y = 0; y < height; y++)
			for (x = 0; x < width; x++) {
				pixels[offset / 4 + y * pitch + x] =
					(u32)value * 0x010101;
			}
			assert(sharp_render_clip(&panel, &params, &length,
				buf, &fb, &full) == 0);
			assert(length == sizeof(buf));
			for (y = 0; y < height; y++) {
				u8 thresholds[4], scalar[width / 8];

				assert(buf[y * line_len] == LPM027M128B_MODE_1BIT);
				assert(buf[y * line_len + 1] == y);
				sharp_render_mono_thresholds(&params, 0, y, thresholds);
				sharp_render_scalar_mono_row(
					pixels + offset / 4 + y * pitch,
					scalar, width, thresholds, false);
				/* On ARM64 the public converter above takes the NEON path. */
				assert(memcmp(scalar, buf + y * line_len + 2,
					sizeof(scalar)) == 0);
				for (x = 0; x < width / 8; x++) {
					int index = y * width / 8 + x;
					u8 bits = buf[y * line_len + 2 + x];
					int bit;

					assert((bits & previous_gray[index]) == previous_gray[index]);
					if (cutoff) {
						assert((bits & previous_cutoff[value][index]) == bits);
					}
					previous_gray[index] = bits;
					previous_cutoff[value][index] = bits;
					if (value == 0 || value == 255) {
						assert(bits == (value == 0 ? 0 : 0xff));
					}
					for (bit = 0; bit < 8; bit++) {
						white_count += (bits >> bit) & 1;
					}
					if (cutoff == 128 && value == 128) {
						assert(bits == ((y & 1) ? 0x55 : 0xaa));
					}
				}
			}
			if (cutoff == 128) {
				int expected_per_tile = (value + 8) / 16;

				if (expected_per_tile > 16)
					expected_per_tile = 16;
				assert(white_count == expected_per_tile * (width * height / 16));
			}
		}

		/* The same black/white endpoints hold for limited-range NV12. */
		for (value = 16; value <= 235; value += 219) {
			memset(y_plane + offset, value, pitch * height);
			assert(sharp_render_clip(&panel, &params, &length,
				buf, &yuv_fb, &full) == 0);
			for (y = 0; y < height; y++)
			for (x = 0; x < width / 8; x++) {
				assert(buf[y * line_len + 2 + x] == (value == 16 ? 0 : 0xff));
			}
		}
	}

	/* RGB and NV12 mid-gray agree; row clips, inversion and headers stay stable. */
	params.mono_cutoff = 128;
	memset(y_plane + offset, 126, pitch * height);
	for (y = 0; y < height; y++)
	for (x = 0; x < width; x++) {
		pixels[offset / 4 + y * pitch + x] = 0x808080;
	}
	assert(sharp_render_clip(&panel, &params, &length, buf, &fb, &full) == 0);
	assert(sharp_render_clip(&panel, &params, &partial_length,
		partial, &yuv_fb, &full) == 0);
	assert(length == partial_length && memcmp(buf, partial, length) == 0);
	memset(uv_plane, 255, sizeof(uv_plane));
	assert(sharp_render_clip(&panel, &params, &partial_length,
		partial, &yuv_fb, &full) == 0);
	assert(memcmp(buf, partial, length) == 0);
	for (x = 0; x < 2; x++) {
		struct drm_framebuffer *source = x ? &yuv_fb : &fb;

		assert(sharp_render_clip(&panel, &params, &partial_length,
			partial, source, &clipped) == 0);
		assert(partial_length == line_len * 4);
		assert(memcmp(buf + line_len * 3, partial, partial_length) == 0);
		params.mono_invert = 1;
		assert(sharp_render_clip(&panel, &params, &partial_length,
			inverted, source, &full) == 0);
		for (y = 0; y < height; y++) {
			assert(memcmp(buf + y * line_len, inverted + y * line_len, 2) == 0);
			assert((u8)(buf[y * line_len + 2] ^ inverted[y * line_len + 2]) == 0xff);
			assert((u8)(buf[y * line_len + 3] ^ inverted[y * line_len + 3]) == 0xff);
		}
		params.mono_invert = 0;
	}
	panel.panel_type = SHARP_PANEL_TYPE_SHARP_MONO;
	assert(sharp_render_clip(&panel, &params, &length, partial, &fb, &full) == 0);
	for (y = 0; y < height; y++) {
		assert(partial[y * line_len] == 0x80);
		assert(partial[y * line_len + 1] == bitrev8((u8)(y + 1)));
		assert(memcmp(buf + y * line_len + 2, partial + y * line_len + 2,
			width / 8) == 0);
	}
	return 0;
}

static int check_color_growth_in_existing_buffer(void)
{
	static const int sizes[][2] = { {272, 451}, {320, 240}, {400, 240}, {640, 480} };
	static const int colors[] = {2, 8, 64, 2, 64};
	static const struct drm_format_info format = { .format = DRM_FORMAT_XRGB8888 };
	unsigned int size, mode;

	for (size = 0; size < ARRAY_SIZE(sizes); size++) {
		int width = sizes[size][0], height = sizes[size][1];
		size_t capacity = (size_t)width * height, length;
		u8 *buffer = calloc(capacity, 1);
		u32 *pixels = calloc(capacity, sizeof(*pixels));
		struct sharp_subpanel panel;
		struct drm_gem_dma_object obj = { .vaddr = pixels };
		struct drm_framebuffer fb = {
			.width = width, .height = height, .pitches = { width * 4 },
			.format = &format, .obj = &obj,
		};
		struct drm_rect clip = {0, 0, width, height};
		struct sharp_render_params params = { .mono_cutoff = 128 };

		assert(buffer && pixels);
		fill_image(pixels, width, height);
		/* Match the driver's allocation, independent of the initial color mode. */
		init_panel(&panel, width, height, buffer, NULL, NULL, NULL);
		assert((size_t)height * panel.line_len_color64 <= capacity);
		for (mode = 0; mode < ARRAY_SIZE(colors); mode++) {
			size_t i;

			params.colors = colors[mode];
			memset(buffer, 0xa5, capacity);
			assert(sharp_render_clip(&panel, &params, &length,
				buffer, &fb, &clip) == 0);
			assert(length == (size_t)height * sharp_render_panel_line_len(&panel, &params));
			assert(length <= capacity);
			for (i = length; i < capacity; i++)
				assert(buffer[i] == 0xa5);
		}
		free(buffer);
		free(pixels);
	}
	return 0;
}

static int check_dither_fallbacks(void)
{
	static const int color_modes[] = { 2, 8, 64 };
	int mode;

	for (mode = 0; mode < (int)ARRAY_SIZE(color_modes); mode++) {
		char reference[65], actual[65];
		int dither;

		assert(run_case("fallback reference", 32, 8, color_modes[mode],
			SHARP_DITHER_ALGO_NONE, 0, 32, 1, reference) == 0);
		for (dither = 1; dither < SHARP_DITHER_ALGO_COUNT; dither++) {
			if (color_modes[mode] == 2 &&
				dither == SHARP_DITHER_ALGO_MONO_ORDERED)
				continue;
			if (color_modes[mode] != 2 &&
				dither != SHARP_DITHER_ALGO_MONO_ORDERED)
				continue;
			assert(run_case("fallback", 32, 8, color_modes[mode],
				dither, 1, 32, 1, actual) == 0);
			assert(strcmp(reference, actual) == 0);
		}
	}
	return 0;
}

static int check_right_panel_partial_clip(void)
{
	enum { height = 8, pitch_pixels = DUAL_WIDTH + 8, offset_pixels = 4 };
	static const struct drm_format_info xrgb8888 = {
		.format = DRM_FORMAT_XRGB8888,
	};
	u32 pixels[offset_pixels + pitch_pixels * height];
	u8 full_buf[PANEL_WIDTH * height];
	u8 partial_buf[PANEL_WIDTH * height];
	s16 errors[PANEL_WIDTH * 3 * 2] = { 0 };
	u8 line_lo[PANEL_WIDTH] = { 0 };
	u8 line_hi[PANEL_WIDTH] = { 0 };
	struct drm_gem_dma_object obj = { .vaddr = pixels };
	struct drm_framebuffer fb = {
		.width = DUAL_WIDTH,
		.height = height,
		.pitches = { pitch_pixels * 4 },
		.offsets = { offset_pixels * 4 },
		.format = &xrgb8888,
		.obj = &obj,
	};
	struct sharp_subpanel panel;
	struct drm_rect full = { PANEL_WIDTH, 0, DUAL_WIDTH, height };
	struct drm_rect partial = { PANEL_WIDTH, 3, DUAL_WIDTH, 7 };
	struct sharp_render_params params = { .mono_cutoff = 128 };
	static const int color_modes[] = { 2, 8, 64 };
	u32 rng = 0x12345678;
	int mode, dither, y, x;

	memset(pixels, 0xa5, sizeof(pixels));
	for (y = 0; y < height; y++) {
		for (x = 0; x < DUAL_WIDTH; x++) {
			pixels[offset_pixels + y * pitch_pixels + x] =
				make_pixel(x, y, DUAL_WIDTH, height, &rng);
		}
	}
	init_panel(&panel, PANEL_WIDTH, height, full_buf, errors,
		line_lo, line_hi);
	panel.x_offset = PANEL_WIDTH;

	for (mode = 0; mode < (int)ARRAY_SIZE(color_modes); mode++) {
		params.colors = color_modes[mode];
		for (dither = SHARP_DITHER_ALGO_NONE;
			dither < SHARP_DITHER_ALGO_COUNT; dither++) {
			int blue;

			if (dither == SHARP_DITHER_ALGO_ERRDIFF2)
				continue;
			params.dither_algo = dither;
			for (blue = 0; blue <= (dither == SHARP_DITHER_ALGO_2197COLORS);
				blue++) {
				size_t full_len, partial_len, line_len;
				int rc;

				params.blue_noise = blue;
				rc = sharp_render_clip(&panel, &params, &full_len,
					full_buf, &fb, &full);
				if (!rc) {
					rc = sharp_render_clip(&panel, &params, &partial_len,
						partial_buf, &fb, &partial);
				}
				if (rc) {
					fprintf(stderr, "right panel render failed: %d\n", rc);
					return 1;
				}
				line_len = full_len / height;
				if (partial_len != line_len * 4 ||
					memcmp(full_buf + line_len * 3, partial_buf, partial_len)) {
					fprintf(stderr,
						"right panel partial clip mismatch colors=%d dither=%d blue=%d\n",
						params.colors, dither, blue);
					return 1;
				}
			}
		}
	}
	return 0;
}

static int visit_cases(FILE *out, const struct golden_file *golden)
{
	static const int mono_cutoffs[] = { 0, 64, 128, 192 };
	int failures = 0;
	int i;

	for (i = 0; i < (int)ARRAY_SIZE(mono_cutoffs); i++) {
		int invert;

		for (invert = 0; invert <= 1; invert++) {
			char name[128];

			snprintf(name, sizeof(name), "w272_h451_c2_cutoff%d_invert%d",
				mono_cutoffs[i], invert);
			if (golden) {
				failures += check_case(golden, name, PANEL_WIDTH, PANEL_HEIGHT,
					2, 0, 0, mono_cutoffs[i], invert);
			} else if (emit_case(out, name, PANEL_WIDTH, PANEL_HEIGHT,
				2, 0, 0, mono_cutoffs[i], invert)) {
				failures++;
			}
		}
	}

	for (i = 0; i < SHARP_DITHER_ALGO_COUNT; i++) {
		/* Mono ordered fallback is covered separately, without new golden hashes. */
		if (i == SHARP_DITHER_ALGO_MONO_ORDERED)
			continue;
		int blue_max = (i == SHARP_DITHER_ALGO_2197COLORS) ? 1 : 0;
		int blue;

		for (blue = 0; blue <= blue_max; blue++) {
			char name[128];

			snprintf(name, sizeof(name), "w272_h451_c8_dither%d_blue%d", i, blue);
			if (golden) {
				failures += check_case(golden, name, PANEL_WIDTH, PANEL_HEIGHT,
					8, i, blue, 128, 0);
			} else if (emit_case(out, name, PANEL_WIDTH, PANEL_HEIGHT,
				8, i, blue, 128, 0)) {
				failures++;
			}
		}
	}

	for (i = 0; i < SHARP_DITHER_ALGO_COUNT; i++) {
		/* Mono ordered fallback is covered separately, without new golden hashes. */
		if (i == SHARP_DITHER_ALGO_MONO_ORDERED)
			continue;
		int blue_max = (i == SHARP_DITHER_ALGO_2197COLORS) ? 1 : 0;
		int blue;

		for (blue = 0; blue <= blue_max; blue++) {
			char name[128];

			snprintf(name, sizeof(name), "w272_h451_c64_dither%d_blue%d", i, blue);
			if (golden) {
				failures += check_case(golden, name, PANEL_WIDTH, PANEL_HEIGHT,
					64, i, blue, 128, 0);
			} else if (emit_case(out, name, PANEL_WIDTH, PANEL_HEIGHT,
				64, i, blue, 128, 0)) {
				failures++;
			}
		}
	}

	if (golden) {
		failures += check_case(golden,
			"w544_h451_c64_dither5_blue1_dualpanel", DUAL_WIDTH,
			PANEL_HEIGHT, 64, SHARP_DITHER_ALGO_2197COLORS, 1, 128, 0);
	} else if (emit_case(out, "w544_h451_c64_dither5_blue1_dualpanel",
		DUAL_WIDTH, PANEL_HEIGHT, 64, SHARP_DITHER_ALGO_2197COLORS, 1,
		128, 0)) {
		failures++;
	}

	return failures;
}

int main(int argc, char **argv)
{
	if (check_optimized_quantizers()) {
		return 1;
	}
	if (check_nv12_render()) {
		return 1;
	}
	if (check_mono_ordered()) {
		return 1;
	}
	if (check_color_growth_in_existing_buffer()) {
		return 1;
	}
	if (check_dither_fallbacks()) {
		return 1;
	}
	if (check_right_panel_partial_clip()) {
		return 1;
	}

	if (argc == 3 && strcmp(argv[1], "--check") == 0) {
		struct golden_file golden;
		int rc = load_golden(argv[2], &golden);
		int failures;

		if (rc) {
			return 1;
		}
		failures = visit_cases(NULL, &golden);
		free(golden.entries);
		if (failures) {
			return 1;
		}
		printf("all golden render cases passed\n");
		return 0;
	}

	if (argc != 1) {
		fprintf(stderr, "usage: %s [--check golden.txt]\n", argv[0]);
		return 2;
	}

	return visit_cases(stdout, NULL) ? 1 : 0;
}
