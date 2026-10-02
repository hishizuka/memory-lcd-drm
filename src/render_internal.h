/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SHARP_RENDER_INTERNAL_H_
#define SHARP_RENDER_INTERNAL_H_

#include <linux/types.h>

#define SHARP_BLUE_NOISE_SIZE 32
#define SHARP_BLUE_NOISE_MASK (SHARP_BLUE_NOISE_SIZE - 1)

extern const u8 sharp_dither_2197_rank_bucket[4];
extern const u8 sharp_dither_matrix_2x2_rank[4];
extern const u8 sharp_dither_343_t0[3];
extern const u8 sharp_dither_343_t1[3];
extern const u8 sharp_dither_125_threshold[4];
extern const u16 sharp_blue_noise_32[SHARP_BLUE_NOISE_SIZE *
	SHARP_BLUE_NOISE_SIZE];

enum sharp_neon_mode {
	SHARP_NEON_8_NONE,
	SHARP_NEON_8_27COLORS,
	SHARP_NEON_8_125COLORS,
	SHARP_NEON_64_NONE,
	SHARP_NEON_64_343COLORS,
	SHARP_NEON_64_2197COLORS,
	SHARP_NEON_64_2197COLORS_BLUE,
};

#if defined(CONFIG_ARM64) && defined(CONFIG_KERNEL_MODE_NEON)
void sharp_render_neon_mono_row(const u32 *src_px, u8 *dst, int width,
	const u8 thresholds[4], bool invert);
void sharp_render_neon_rows(enum sharp_neon_mode mode, const u8 *src,
	size_t src_pitch, u8 *dst, size_t dst_pitch, size_t plane_len,
	int width, int x1, int y0, int rows);
#endif

#endif
