// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * AArch64 NEON color conversion for Sharp Memory LCD panels.
 *
 * This translation unit must only be called inside a kernel_neon_begin() /
 * kernel_neon_end() critical section.
 */

#include <linux/types.h>

#include "render_internal.h"

#include <asm/neon-intrinsics.h>

static const u8 sharp_rgb_indices[3][16] = {
	{ 2, 1, 0, 6, 5, 4, 10, 9, 8, 14, 13, 12, 18, 17, 16, 22 },
	{ 21, 20, 26, 25, 24, 30, 29, 28, 34, 33, 32, 38, 37, 36, 42, 41 },
	{ 40, 46, 45, 44, 50, 49, 48, 54, 53, 52, 58, 57, 56, 62, 61, 60 },
};

static const u8 sharp_pixel_first_mask[3][16] = {
	{
		0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0xff, 0xff,
		0xff, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0x00,
	},
	{
		0x00, 0x00, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00,
		0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0xff, 0xff,
	},
	{
		0xff, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0x00,
		0x00, 0x00, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00,
	},
};

static const u8 sharp_rank_expand_indices[3][16] = {
	{ 0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5 },
	{ 5, 5, 6, 6, 6, 7, 7, 7, 8, 8, 8, 9, 9, 9, 10, 10 },
	{ 10, 11, 11, 11, 12, 12, 12, 13, 13, 13, 14, 14, 14, 15, 15, 15 },
};

static const s8 sharp_pack_shifts[8] = {
	7, 6, 5, 4, 3, 2, 1, 0
};

static const u8 sharp_component_first_mask[16] = {
	0xff, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0x00,
	0xff, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0x00,
};

/*
 * The 13-level LUT is monotonic. The high nibble selects the level at the
 * start of that 16-value range and the first transition inside the range.
 * Only the final range has a second transition, at 251.
 */
static const u8 sharp_level13_nibble_base[16] = {
	0, 0, 0, 0, 1, 1, 1, 2,
	3, 3, 4, 5, 6, 8, 9, 10,
};

static const u8 sharp_level13_nibble_edge[16] = {
	16, 16, 16, 10, 16, 16, 4, 14,
	16, 3, 5, 5, 3, 11, 7, 1,
};

static inline uint8x16x3_t sharp_load_rgb(const u8 *src,
	uint8x16_t idx0, uint8x16_t idx1, uint8x16_t idx2)
{
	uint8x16x4_t table;
	uint8x16x3_t rgb;

	/*
	 * DRM XRGB8888 is B,G,R,X in memory on the little-endian Raspberry Pi.
	 * TBL removes X and emits the exact R,G,B bit-stream order used on wire.
	 */
	table.val[0] = vld1q_u8(src);
	table.val[1] = vld1q_u8(src + 16);
	table.val[2] = vld1q_u8(src + 32);
	table.val[3] = vld1q_u8(src + 48);
	rgb.val[0] = vqtbl4q_u8(table, idx0);
	rgb.val[1] = vqtbl4q_u8(table, idx1);
	rgb.val[2] = vqtbl4q_u8(table, idx2);

	return rgb;
}

static inline u8 sharp_pack_8bits(uint8x8_t bits, int8x8_t shifts)
{
	return vaddv_u8(vshl_u8(bits, shifts));
}

static inline void sharp_store_plane(u8 *dst, uint8x16x3_t bits,
	int8x8_t shifts)
{
	dst[0] = sharp_pack_8bits(vget_low_u8(bits.val[0]), shifts);
	dst[1] = sharp_pack_8bits(vget_high_u8(bits.val[0]), shifts);
	dst[2] = sharp_pack_8bits(vget_low_u8(bits.val[1]), shifts);
	dst[3] = sharp_pack_8bits(vget_high_u8(bits.val[1]), shifts);
	dst[4] = sharp_pack_8bits(vget_low_u8(bits.val[2]), shifts);
	dst[5] = sharp_pack_8bits(vget_high_u8(bits.val[2]), shifts);
}

static inline uint8x16x3_t sharp_load_pixel_first_mask(void)
{
	uint8x16x3_t mask;

	mask.val[0] = vld1q_u8(sharp_pixel_first_mask[0]);
	mask.val[1] = vld1q_u8(sharp_pixel_first_mask[1]);
	mask.val[2] = vld1q_u8(sharp_pixel_first_mask[2]);
	return mask;
}

static inline uint8x16x3_t sharp_blend_pixel_pattern(uint8x16x3_t mask,
	u8 first, u8 second)
{
	uint8x16_t first_vec = vdupq_n_u8(first);
	uint8x16_t second_vec = vdupq_n_u8(second);
	uint8x16x3_t result;

	result.val[0] = vbslq_u8(mask.val[0], first_vec, second_vec);
	result.val[1] = vbslq_u8(mask.val[1], first_vec, second_vec);
	result.val[2] = vbslq_u8(mask.val[2], first_vec, second_vec);
	return result;
}

static inline uint8x16x3_t sharp_compare_pattern(uint8x16x3_t rgb,
	uint8x16x3_t threshold)
{
	uint8x16x3_t bits;

	bits.val[0] = vshrq_n_u8(vcgeq_u8(rgb.val[0], threshold.val[0]), 7);
	bits.val[1] = vshrq_n_u8(vcgeq_u8(rgb.val[1], threshold.val[1]), 7);
	bits.val[2] = vshrq_n_u8(vcgeq_u8(rgb.val[2], threshold.val[2]), 7);
	return bits;
}

void sharp_render_neon_mono_row(const u32 *src_px, u8 *dst, int width,
	const u8 thresholds[4], bool invert)
{
	const u8 *src = (const u8 *)src_px;
	const u16 threshold_values[8] = {
		(u16)thresholds[0] << 8, (u16)thresholds[1] << 8,
		(u16)thresholds[2] << 8, (u16)thresholds[3] << 8,
		(u16)thresholds[0] << 8, (u16)thresholds[1] << 8,
		(u16)thresholds[2] << 8, (u16)thresholds[3] << 8,
	};
	uint16x8_t threshold = vld1q_u16(threshold_values);
	uint8x8_t weight_r = vdup_n_u8(77);
	uint8x8_t weight_g = vdup_n_u8(150);
	uint8x8_t weight_b = vdup_n_u8(29);
	int8x8_t shifts = vld1_s8(sharp_pack_shifts);
	u8 invert_mask = invert ? 0xff : 0x00;
	int x;

	for (x = 0; x < width; x += 16) {
		uint8x16x4_t bgra = vld4q_u8(src);
		uint16x8_t sum_lo =
			vmull_u8(vget_low_u8(bgra.val[2]), weight_r);
		uint16x8_t sum_hi =
			vmull_u8(vget_high_u8(bgra.val[2]), weight_r);
		uint8x8_t bits_lo;
		uint8x8_t bits_hi;

		sum_lo = vmlal_u8(sum_lo, vget_low_u8(bgra.val[1]), weight_g);
		sum_hi = vmlal_u8(sum_hi, vget_high_u8(bgra.val[1]), weight_g);
		sum_lo = vmlal_u8(sum_lo, vget_low_u8(bgra.val[0]), weight_b);
		sum_hi = vmlal_u8(sum_hi, vget_high_u8(bgra.val[0]), weight_b);
		bits_lo = vshr_n_u8(vmovn_u16(vcgeq_u16(sum_lo, threshold)), 7);
		bits_hi = vshr_n_u8(vmovn_u16(vcgeq_u16(sum_hi, threshold)), 7);
		dst[0] = sharp_pack_8bits(bits_lo, shifts) ^ invert_mask;
		dst[1] = sharp_pack_8bits(bits_hi, shifts) ^ invert_mask;
		src += 64;
		dst += 2;
	}
}

static inline uint8x16_t sharp_level13(uint8x16_t value,
	uint8x16_t base_table, uint8x16_t edge_table)
{
	uint8x16_t nibble = vshrq_n_u8(value, 4);
	uint8x16_t low = vandq_u8(value, vdupq_n_u8(0x0f));
	uint8x16_t base = vqtbl1q_u8(base_table, nibble);
	uint8x16_t edge = vqtbl1q_u8(edge_table, nibble);
	uint8x16_t first_inc =
		vshrq_n_u8(vcgeq_u8(low, edge), 7);
	uint8x16_t second_inc =
		vshrq_n_u8(vcgeq_u8(value, vdupq_n_u8(251)), 7);

	return vaddq_u8(base, vaddq_u8(first_inc, second_inc));
}

static inline void sharp_quantize_2197_vector(uint8x16_t level,
	uint8x16_t rank_bucket, uint8x16_t *lo, uint8x16_t *hi)
{
	uint8x16_t remainder = vandq_u8(level, vdupq_n_u8(0x03));
	uint8x16_t value = vshrq_n_u8(level, 2);
	uint8x16_t increment =
		vshrq_n_u8(vcltq_u8(rank_bucket, remainder), 7);
	uint8x16_t quantized = vaddq_u8(value, increment);

	*lo = vandq_u8(quantized, vdupq_n_u8(0x01));
	*hi = vandq_u8(vshrq_n_u8(quantized, 1), vdupq_n_u8(0x01));
}

static void sharp_render_neon_8_none(const u8 *src, u8 *dst, int width)
{
	uint8x16_t idx0 = vld1q_u8(sharp_rgb_indices[0]);
	uint8x16_t idx1 = vld1q_u8(sharp_rgb_indices[1]);
	uint8x16_t idx2 = vld1q_u8(sharp_rgb_indices[2]);
	int8x8_t shifts = vld1_s8(sharp_pack_shifts);
	int x;

	for (x = 0; x < width; x += 16) {
		uint8x16x3_t rgb = sharp_load_rgb(src, idx0, idx1, idx2);
		uint8x16x3_t bits;

		bits.val[0] = vshrq_n_u8(rgb.val[0], 7);
		bits.val[1] = vshrq_n_u8(rgb.val[1], 7);
		bits.val[2] = vshrq_n_u8(rgb.val[2], 7);
		sharp_store_plane(dst, bits, shifts);
		src += 64;
		dst += 6;
	}
}

static void sharp_render_neon_8_27colors(const u8 *src, u8 *dst,
	int width, int x1, int y)
{
	uint8x16_t idx0 = vld1q_u8(sharp_rgb_indices[0]);
	uint8x16_t idx1 = vld1q_u8(sharp_rgb_indices[1]);
	uint8x16_t idx2 = vld1q_u8(sharp_rgb_indices[2]);
	uint8x16_t mask = vld1q_u8(sharp_component_first_mask);
	bool first_low = ((x1 + y) & 1) == 0;
	uint8x16_t threshold = vbslq_u8(mask,
		vdupq_n_u8(first_low ? 137 : 225),
		vdupq_n_u8(first_low ? 225 : 137));
	int8x8_t shifts = vld1_s8(sharp_pack_shifts);
	int x;

	for (x = 0; x < width; x += 16) {
		uint8x16x3_t rgb = sharp_load_rgb(src, idx0, idx1, idx2);
		uint8x16x3_t bits;

		bits.val[0] = vshrq_n_u8(vcgeq_u8(rgb.val[0], threshold), 7);
		bits.val[1] = vshrq_n_u8(vcgeq_u8(rgb.val[1], threshold), 7);
		bits.val[2] = vshrq_n_u8(vcgeq_u8(rgb.val[2], threshold), 7);
		sharp_store_plane(dst, bits, shifts);
		src += 64;
		dst += 6;
	}
}

static void sharp_render_neon_8_125colors(const u8 *src, u8 *dst,
	int width, int x1, int y)
{
	uint8x16_t idx0 = vld1q_u8(sharp_rgb_indices[0]);
	uint8x16_t idx1 = vld1q_u8(sharp_rgb_indices[1]);
	uint8x16_t idx2 = vld1q_u8(sharp_rgb_indices[2]);
	uint8x16x3_t mask = sharp_load_pixel_first_mask();
	int first_rank = sharp_dither_matrix_2x2_rank[
		(x1 & 1) + ((y & 1) * 2)];
	int second_rank = sharp_dither_matrix_2x2_rank[
		((x1 + 1) & 1) + ((y & 1) * 2)];
	uint8x16x3_t threshold = sharp_blend_pixel_pattern(mask,
		sharp_dither_125_threshold[first_rank],
		sharp_dither_125_threshold[second_rank]);
	int8x8_t shifts = vld1_s8(sharp_pack_shifts);
	int x;

	for (x = 0; x < width; x += 16) {
		uint8x16x3_t rgb = sharp_load_rgb(src, idx0, idx1, idx2);
		uint8x16x3_t bits = sharp_compare_pattern(rgb, threshold);

		sharp_store_plane(dst, bits, shifts);
		src += 64;
		dst += 6;
	}
}

static void sharp_render_neon_64_none(const u8 *src, u8 *dst_lo,
	u8 *dst_hi, int width)
{
	uint8x16_t idx0 = vld1q_u8(sharp_rgb_indices[0]);
	uint8x16_t idx1 = vld1q_u8(sharp_rgb_indices[1]);
	uint8x16_t idx2 = vld1q_u8(sharp_rgb_indices[2]);
	uint8x16_t one = vdupq_n_u8(0x01);
	int8x8_t shifts = vld1_s8(sharp_pack_shifts);
	int x;

	for (x = 0; x < width; x += 16) {
		uint8x16x3_t rgb = sharp_load_rgb(src, idx0, idx1, idx2);
		uint8x16x3_t lo;
		uint8x16x3_t hi;

		hi.val[0] = vshrq_n_u8(rgb.val[0], 7);
		hi.val[1] = vshrq_n_u8(rgb.val[1], 7);
		hi.val[2] = vshrq_n_u8(rgb.val[2], 7);
		lo.val[0] = vandq_u8(vshrq_n_u8(rgb.val[0], 6), one);
		lo.val[1] = vandq_u8(vshrq_n_u8(rgb.val[1], 6), one);
		lo.val[2] = vandq_u8(vshrq_n_u8(rgb.val[2], 6), one);
		sharp_store_plane(dst_lo, lo, shifts);
		sharp_store_plane(dst_hi, hi, shifts);
		src += 64;
		dst_lo += 6;
		dst_hi += 6;
	}
}

static void sharp_render_neon_64_343colors(const u8 *src, u8 *dst_lo,
	u8 *dst_hi, int width, int x1, int y)
{
	uint8x16_t idx0 = vld1q_u8(sharp_rgb_indices[0]);
	uint8x16_t idx1 = vld1q_u8(sharp_rgb_indices[1]);
	uint8x16_t idx2 = vld1q_u8(sharp_rgb_indices[2]);
	uint8x16x3_t mask = sharp_load_pixel_first_mask();
	bool first_toggle = ((x1 + y) & 1) == 0;
	const u8 *first = first_toggle
		? sharp_dither_343_t1 : sharp_dither_343_t0;
	const u8 *second = first_toggle
		? sharp_dither_343_t0 : sharp_dither_343_t1;
	uint8x16x3_t threshold0 =
		sharp_blend_pixel_pattern(mask, first[0], second[0]);
	uint8x16x3_t threshold1 =
		sharp_blend_pixel_pattern(mask, first[1], second[1]);
	uint8x16x3_t threshold2 =
		sharp_blend_pixel_pattern(mask, first[2], second[2]);
	int8x8_t shifts = vld1_s8(sharp_pack_shifts);
	int x;

	for (x = 0; x < width; x += 16) {
		uint8x16x3_t rgb = sharp_load_rgb(src, idx0, idx1, idx2);
		uint8x16x3_t lo;
		uint8x16x3_t hi;
		int i;

		for (i = 0; i < 3; i++) {
			uint8x16_t c0 = vcgeq_u8(rgb.val[i], threshold0.val[i]);
			uint8x16_t c1 = vcgeq_u8(rgb.val[i], threshold1.val[i]);
			uint8x16_t c2 = vcgeq_u8(rgb.val[i], threshold2.val[i]);

			hi.val[i] = vshrq_n_u8(c1, 7);
			lo.val[i] = vshrq_n_u8(
				veorq_u8(veorq_u8(c0, c1), c2), 7);
		}
		sharp_store_plane(dst_lo, lo, shifts);
		sharp_store_plane(dst_hi, hi, shifts);
		src += 64;
		dst_lo += 6;
		dst_hi += 6;
	}
}

static void sharp_render_neon_64_2197colors(const u8 *src, u8 *dst_lo,
	u8 *dst_hi, int width, int x1, int y)
{
	uint8x16_t idx0 = vld1q_u8(sharp_rgb_indices[0]);
	uint8x16_t idx1 = vld1q_u8(sharp_rgb_indices[1]);
	uint8x16_t idx2 = vld1q_u8(sharp_rgb_indices[2]);
	uint8x16x3_t mask = sharp_load_pixel_first_mask();
	int first_rank = (x1 & 1) + ((y & 1) * 2);
	int second_rank = ((x1 + 1) & 1) + ((y & 1) * 2);
	uint8x16x3_t rank_bucket = sharp_blend_pixel_pattern(mask,
		sharp_dither_2197_rank_bucket[first_rank],
		sharp_dither_2197_rank_bucket[second_rank]);
	uint8x16_t base_table = vld1q_u8(sharp_level13_nibble_base);
	uint8x16_t edge_table = vld1q_u8(sharp_level13_nibble_edge);
	int8x8_t shifts = vld1_s8(sharp_pack_shifts);
	int x;

	for (x = 0; x < width; x += 16) {
		uint8x16x3_t rgb = sharp_load_rgb(src, idx0, idx1, idx2);
		uint8x16x3_t lo;
		uint8x16x3_t hi;
		int i;

		for (i = 0; i < 3; i++) {
			uint8x16_t level =
				sharp_level13(rgb.val[i], base_table, edge_table);

			sharp_quantize_2197_vector(level, rank_bucket.val[i],
				&lo.val[i], &hi.val[i]);
		}
		sharp_store_plane(dst_lo, lo, shifts);
		sharp_store_plane(dst_hi, hi, shifts);
		src += 64;
		dst_lo += 6;
		dst_hi += 6;
	}
}

static inline uint8x16_t sharp_load_blue_rank_buckets(const u16 *rank_row,
	int start)
{
	uint16x8_t lo = vld1q_u16(rank_row + start);
	uint16x8_t hi = vld1q_u16(rank_row + start + 8);

	return vcombine_u8(vmovn_u16(vshrq_n_u16(lo, 8)),
		vmovn_u16(vshrq_n_u16(hi, 8)));
}

static void sharp_render_neon_64_2197colors_blue(const u8 *src,
	u8 *dst_lo, u8 *dst_hi, int width, int x1, int y)
{
	uint8x16_t idx0 = vld1q_u8(sharp_rgb_indices[0]);
	uint8x16_t idx1 = vld1q_u8(sharp_rgb_indices[1]);
	uint8x16_t idx2 = vld1q_u8(sharp_rgb_indices[2]);
	uint8x16_t rank_idx0 = vld1q_u8(sharp_rank_expand_indices[0]);
	uint8x16_t rank_idx1 = vld1q_u8(sharp_rank_expand_indices[1]);
	uint8x16_t rank_idx2 = vld1q_u8(sharp_rank_expand_indices[2]);
	uint8x16_t base_table = vld1q_u8(sharp_level13_nibble_base);
	uint8x16_t edge_table = vld1q_u8(sharp_level13_nibble_edge);
	const u16 *rank_row = sharp_blue_noise_32
		+ ((y & SHARP_BLUE_NOISE_MASK) * SHARP_BLUE_NOISE_SIZE);
	int8x8_t shifts = vld1_s8(sharp_pack_shifts);
	int x;

	for (x = 0; x < width; x += 16) {
		int rank_start = (x1 + x) & SHARP_BLUE_NOISE_MASK;
		uint8x16_t rank =
			sharp_load_blue_rank_buckets(rank_row, rank_start);
		uint8x16x3_t rank_bucket;
		uint8x16x3_t rgb = sharp_load_rgb(src, idx0, idx1, idx2);
		uint8x16x3_t lo;
		uint8x16x3_t hi;
		int i;

		rank_bucket.val[0] = vqtbl1q_u8(rank, rank_idx0);
		rank_bucket.val[1] = vqtbl1q_u8(rank, rank_idx1);
		rank_bucket.val[2] = vqtbl1q_u8(rank, rank_idx2);
		for (i = 0; i < 3; i++) {
			uint8x16_t level =
				sharp_level13(rgb.val[i], base_table, edge_table);

			sharp_quantize_2197_vector(level, rank_bucket.val[i],
				&lo.val[i], &hi.val[i]);
		}
		sharp_store_plane(dst_lo, lo, shifts);
		sharp_store_plane(dst_hi, hi, shifts);
		src += 64;
		dst_lo += 6;
		dst_hi += 6;
	}
}

void sharp_render_neon_rows(enum sharp_neon_mode mode, const u8 *src,
	size_t src_pitch, u8 *dst, size_t dst_pitch, size_t plane_len,
	int width, int x1, int y0, int rows)
{
	int row;

#define SHARP_NEON_FOR_EACH_ROW(call) \
	do { \
		for (row = 0; row < rows; row++) { \
			const u8 *row_src = src + ((size_t)row * src_pitch); \
			u8 *row_dst = dst + ((size_t)row * dst_pitch) + 2; \
			u8 *row_hi = plane_len ? row_dst + plane_len + 2 : NULL; \
			(void)row_hi; \
			call; \
		} \
	} while (0)

	switch (mode) {
	case SHARP_NEON_8_NONE:
		SHARP_NEON_FOR_EACH_ROW(
			sharp_render_neon_8_none(row_src, row_dst, width));
		break;
	case SHARP_NEON_8_27COLORS:
		SHARP_NEON_FOR_EACH_ROW(
			sharp_render_neon_8_27colors(row_src, row_dst, width,
				x1, y0 + row));
		break;
	case SHARP_NEON_8_125COLORS:
		SHARP_NEON_FOR_EACH_ROW(
			sharp_render_neon_8_125colors(row_src, row_dst, width,
				x1, y0 + row));
		break;
	case SHARP_NEON_64_NONE:
		SHARP_NEON_FOR_EACH_ROW(
			sharp_render_neon_64_none(row_src, row_dst, row_hi,
				width));
		break;
	case SHARP_NEON_64_343COLORS:
		SHARP_NEON_FOR_EACH_ROW(
			sharp_render_neon_64_343colors(row_src, row_dst, row_hi,
				width, x1, y0 + row));
		break;
	case SHARP_NEON_64_2197COLORS:
		SHARP_NEON_FOR_EACH_ROW(
			sharp_render_neon_64_2197colors(row_src, row_dst, row_hi,
				width, x1, y0 + row));
		break;
	case SHARP_NEON_64_2197COLORS_BLUE:
		SHARP_NEON_FOR_EACH_ROW(
			sharp_render_neon_64_2197colors_blue(row_src, row_dst,
				row_hi, width, x1, y0 + row));
		break;
	}

#undef SHARP_NEON_FOR_EACH_ROW
}
