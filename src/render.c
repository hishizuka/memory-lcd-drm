// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DRM driver for 2.7" Sharp Memory LCD
 *
 * Copyright 2023 Andrew D'Angelo
 */

#include "sharp_drm.h"
#include "render_internal.h"

#include <linux/bitrev.h>
#include <linux/string.h>

#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>

#if defined(CONFIG_ARM64) && defined(CONFIG_KERNEL_MODE_NEON)
#include <asm/neon.h>
#include <asm/simd.h>
#endif

#define LPM027M128B_M0 (1u << 7)
#define LPM027M128B_M4 (1u << 3)
#define LPM027M128B_MODE_1BIT (LPM027M128B_M0 | LPM027M128B_M4)
#define LPM027M128B_MODE_8COLOR LPM027M128B_M0
#define LPM027M128B_64COLOR_LSB_FLAG (1u << 1) // Marks the low-bit plane header
#define LPM027M128B_MODE_64COLOR_MSB LPM027M128B_M0
#define LPM027M128B_MODE_64COLOR_LSB (LPM027M128B_M0 | LPM027M128B_64COLOR_LSB_FLAG)

// 2x2 rank matrix for 5-level (125-color) ordered dithering.
const u8 sharp_dither_matrix_2x2_rank[4] = {
	0, 3,
	2, 1
};

/*
 * The 2197-color quantizer only compares each 4x4 rank against quarter
 * boundaries, so retain the two significant rank bits directly.
 */
const u8 sharp_dither_2197_rank_bucket[4] = {
	0, 2,
	3, 1
};

// sRGB to linear (8-bit) lookup for errdiff2.
static const u8 sharp_srgb_to_linear_lut[256] = {
	  0,   0,   0,   0,   0,   0,   0,   1,   1,   1,   1,   1,   1,   1,   1,   1,
	  1,   1,   2,   2,   2,   2,   2,   2,   2,   2,   3,   3,   3,   3,   3,   3,
	  4,   4,   4,   4,   4,   5,   5,   5,   5,   6,   6,   6,   6,   7,   7,   7,
	  8,   8,   8,   8,   9,   9,   9,  10,  10,  10,  11,  11,  12,  12,  12,  13,
	 13,  13,  14,  14,  15,  15,  16,  16,  17,  17,  17,  18,  18,  19,  19,  20,
	 20,  21,  22,  22,  23,  23,  24,  24,  25,  25,  26,  27,  27,  28,  29,  29,
	 30,  30,  31,  32,  32,  33,  34,  35,  35,  36,  37,  37,  38,  39,  40,  41,
	 41,  42,  43,  44,  45,  45,  46,  47,  48,  49,  50,  51,  51,  52,  53,  54,
	 55,  56,  57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,
	 71,  72,  73,  74,  76,  77,  78,  79,  80,  81,  82,  84,  85,  86,  87,  88,
	 90,  91,  92,  93,  95,  96,  97,  99, 100, 101, 103, 104, 105, 107, 108, 109,
	111, 112, 114, 115, 116, 118, 119, 121, 122, 124, 125, 127, 128, 130, 131, 133,
	134, 136, 138, 139, 141, 142, 144, 146, 147, 149, 151, 152, 154, 156, 157, 159,
	161, 163, 164, 166, 168, 170, 171, 173, 175, 177, 179, 181, 183, 184, 186, 188,
	190, 192, 194, 196, 198, 200, 202, 204, 206, 208, 210, 212, 214, 216, 218, 220,
	222, 224, 226, 229, 231, 233, 235, 237, 239, 242, 244, 246, 248, 250, 253, 255,
};

// Blue-noise rank map for 2197-color dithering (void-and-cluster, fixed sRGB).
const u16 sharp_blue_noise_32[SHARP_BLUE_NOISE_SIZE *
	SHARP_BLUE_NOISE_SIZE] = {
	 512,  917,  726,  965,  432,   29,  916,  513,  987,  402,  236,  804,  664,  302,   72,  514,  144,  660,   58,  743, 1000,   70,  398,  151,  515,  103,  995,  905,  684,   26,  751,  199,
	 464,  253,  140,  601,  193,  678,  357,   93,  844,  650,  327,  919,   95,  437,  697,  993,  904,  396,  488,  203,  653,  279,  761,  889,  233,  350,  755,  577,  255,  415,  956,  846,
	 371,  661,  510,  346,  864,  753,  505,  215,  719,  145,  466,  539,  744,  182,  354,  770,  228,  598,  305,  797,  114,  935,  585, 1006,  695,  841,   50,  472,  800,  132,  597,   65,
	 901,  777,    9,  541,   86,  262,  406,  566, 1005,  893,   53,  267, 1012,  875,  570,  117,  835,   25, 1009,  533,  442,  355,  166,   42,  444,  288,  631,  175,  359, 1019,  703,  299,
	 576,  186,  443,  911,  989,  632,  831,   21,  307,  603,  762,  383,  646,    8,  474,  283,  413,  670,  922,   77,  874,  615,  821,  506,  560,  106,  960,  891,  534,  224,  837,  109,
	1015,  715,  245,  803,  314,  130,  462,  775,  160,  411,  958,  129,  842,  218,  948,  619,  738,  159,  334,  764,  194,  296,  733,  222,  340,  792,  408,  724,   15,  659,  937,  421,
	  40,  339,  640,  100,  572,  712,  217,  976,  655,  866,  227,  558,  711,  329,  786,   81,  871,  486,  559,  423,  952,  683,    7,  897,  992,  673,  150,  261,  468,  324,  156,  787,
	 516,  838,  945,  389,  479,  862,  356,  517,   44,  735,  349,  998,   48,  436,  518, 1003,  209,  303,   49,  649,  113,  495,  393,  519,   78,  297,  599,  830,  983,  756,  616,  258,
	 907,   83,  201,  692,   17,  170,  930,  272,  427,  502,  115,  623,  281,  933,  157,  370,  606,  723,  932,  834,  256,  587,  161,  857,  445,  765,  927,   51,  552,  104,  397, 1002,
	 353,  609,  448,  984,  811,  627,  746,  105,  808,  681,  187,  884,  815,  734,  657,   32,  818,  497,  167,  375,  707,  979,  782,  231,  663,  131,  361,  211,  431,  856,  229,  699,
	 138,  774,  321,  543,  251,  377, 1004,  564,  325,  915,  586,  380,   67,  451,  265,  894,  401,  243,  537,   23,  455,  289,   61,  381,  914,  574,  492,  694,  970,  637,    6,  485,
	 885,  268,   36,  918,  122,  881,   64,  214,  481,    5,  252,  980,  538,  192, 1007,  579,  124,  686,  795,  886,  635,  924,  556,  718, 1016,   22,  270,  773,  120,  309,  794,  569,
	 978,  641,  732,  489,  605,  714,  446,  645,  849,  708,  422,  781,   99,  679,  760,  310,  460,  951,   82,  344,  234,  153,  813,  315,  200,  633,  836,  369,  532,  900,  400,  165,
	  66,  822,  407,  221,  338,  149,  817,  285,  967,  164,  618,  317,  931,  352,  876,   13,  828,  276,  611,  500,  740,  977,  434,  102,  863,  405,  155,  936,   46,  240,  728,  473,
	 367,  520,  107,  855,  963,  565,  409,   35,  521,  454,   71,  827,  139,  467,  625,  171,  522,  731,  414,  133,  578,   10,  682,  523,  997,  737,  582,  457,  685, 1011,  588,  204,
	 688,  275,  994,  654,   84,  785,  198,  929,  742,  216, 1001,  722,  563,  235,  971,  384,  508,   59,  223,  957,  854,  373,  242,  906,  319,   33,  225,  793,   97,  342,  859,  947,
	 778,  449,  173,  747,  308,  484,  612,  333,  840,  387,  602,  269,  920,   45,  676,  812,  322,  892,  667,  788,  284,  490,  754,   85,  476,  651,  382,  953,  278,  629,  163,   18,
	 323,  879,  593,   27,  417,  887,   62,  690,  147,  501,   14,  869,  365,  452,  748,   90,  581,  148, 1017,  542,  108,  656,  176,  567,  810,  143,  860,  535,  480,  745,  429,  553,
	  91,  504,  247,  944,  536,  717,  266,  991,  561,  291,  648,  767,  123,  540,  195,  923,  294,  433,  847,   37,  388,  964,  878,  419,  264,  986,  702,  206,   39,  926,  254,  816,
	 634,  713,  363,  798,  127,  189,  379,  819,   79,  934,  424,  244,  990,  858,  348,  498,  626,  230,  687,  478,  590,  249,  720,   19,  628,  328,  111,  895,  360,  666,  135,  985,
	 426,  207,   54,  604, 1018,  899,  617,  456,  730,  852,  142,  701,  591,   52,  674,  824,    4,  768,  372,  188,  790,  125,  345,  925,  796,  491,  600,  757,  469,  580,  872,  301,
	 524,  954,  839,  461,  290,  752,    3,  239,  351,  525,  316,  482,  205,  412,  287,  137,  526,  950,   87,  908, 1020,  644,  458,  527,  183,   60,  399,  273,   74,  212,  776,    2,
	 739,  136,  652,  385,   92,  551,  969,  668,  128, 1010,   31,  772,  973,  873,  741,  996,  439,  318,  706,  557,  271,  851,   94,  292,  677,  848,  961,  547,  826,  941,  374,  496,
	 336,  902,  274,  780,  928,  220,  418,  801,  890,  608,  391,  647,   89,  562,  168,  643,  238,  870,  141,  425,   30,  693,  975,  390,  509,  210,  727,  101,  428,  622,  700,  190,
	1008,   68,  583,  162,  691,  483,   57,  300,  179,  942,  232,  843,  298,  939,  358,   76,  779,  477,  614,  799,  347,  191,  584,  784,   11,  607,  332,  493,  177,  304,   47,  573,
	 805,  669,  440,  833,  335,  865,  624,  759,  568,   80,  709,  430,  178,  698,  465,  912,  571,   38,  202,  938, 1023,  725,  898,  152,  940,  259,  882,  672,  807,  988,  877,  410,
	 121,  962,  248,   16,  529,  118,  981,  394,  494,  331,  802, 1013,  531,   12,  809,  263,  404,  721,  853,  277,  530,   55,  311,  441,  550,  750,  376,   41,  528,  146,  716,  226,
	 546,  368,  749,  913,  471,  197,  705,  257,   20,  955,  620,   98,  364,  896,  613,  134,  999,  313,  658,  158,  395,  789,  503,  662,   75,  180,  966,  463,  280,  642,  343,  499,
	 868,   73,  621,  282,  675,  362,  903,  825,  545,  169,  880,  241,  689,  507,  219,  758,  544,   88,  453,  982,  610,  116,  260,  861,  392,  820,  596,  110,  888,  949,    1,  763,
	 172,  487,  213,  974,  791,   63,  595,  112,  420,  671,  306,  766,  416,  119,  326,  946,  378,  910,  771,    0,  883,  710,  959,  575,  208, 1021,  696,  366,  736,  237,  447,  592,
	 943,  680,  403,  554,  154,  459,  511,  320,  729,  909,  470,   28,  555,  867,  630,   34,  704,  185,  589,  246,  438,  174,  337,   24,  769,  293,   56,  850,  548,  126,  823,  295,
	  96,  806,   43,  312,  829,  638,  250,  783,  181,   69,  594,  972,  196, 1022,  814,  450,  286,  845,  968,  341,  549,  832,  665,  475,  921,  639,  435,  184,  330, 1014,  636,  386,
};
// Fixed sRGB thresholds derived from equal linear spacing.
#define SHARP_DITHER27_LOW 137
#define SHARP_DITHER27_HIGH 225

const u8 sharp_dither_343_t0[3] = {
	137, 201, 245
};

const u8 sharp_dither_343_t1[3] = {
	82, 173, 225
};

/*
 * Minimum sRGB value that raises a 5-level quantizer above each 2x2 rank.
 * This is equivalent to testing the rank against the former 5-level LUT.
 */
const u8 sharp_dither_125_threshold[4] = {
	100, 165, 208, 241
};

static const u8 sharp_dither_level_13_lut[256] = {
	  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
	  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
	  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
	  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   1,   1,   1,   1,   1,   1,
	  1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,
	  1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,
	  1,   1,   1,   1,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,
	  2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   3,   3,
	  3,   3,   3,   3,   3,   3,   3,   3,   3,   3,   3,   3,   3,   3,   3,   3,
	  3,   3,   3,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,
	  4,   4,   4,   4,   4,   5,   5,   5,   5,   5,   5,   5,   5,   5,   5,   5,
	  5,   5,   5,   5,   5,   6,   6,   6,   6,   6,   6,   6,   6,   6,   6,   6,
	  6,   6,   6,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,
	  8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   9,   9,   9,   9,   9,
	  9,   9,   9,   9,   9,   9,   9,  10,  10,  10,  10,  10,  10,  10,  10,  10,
	 10,  11,  11,  11,  11,  11,  11,  11,  11,  11,  11,  12,  12,  12,  12,  12,
};

// Based on conv_3bit_27colors in ref/pizero_bikecomputer.
static inline u8 sharp_dither_threshold_27colors(bool toggle)
{
	return toggle ? (u8)SHARP_DITHER27_LOW : (u8)SHARP_DITHER27_HIGH;
}

static inline void sharp_dither_thresholds_343colors(u8 t0[3], u8 t1[3])
{
	t0[0] = sharp_dither_343_t0[0];
	t0[1] = sharp_dither_343_t0[1];
	t0[2] = sharp_dither_343_t0[2];
	t1[0] = sharp_dither_343_t1[0];
	t1[1] = sharp_dither_343_t1[1];
	t1[2] = sharp_dither_343_t1[2];
}

static inline u8 sharp_dither_rank_2x2(int x, int y)
{
	int mx = x & 1;
	int my = y & 1;

	return sharp_dither_matrix_2x2_rank[mx + (my * 2)];
}

static inline u8 sharp_srgb_to_linear(u8 v)
{
	return sharp_srgb_to_linear_lut[v];
}

static inline u8 sharp_clamp_u8(int value)
{
	if (value < 0) {
		return 0;
	}
	if (value > 255) {
		return 255;
	}
	return (u8)value;
}

static inline int sharp_clamp_int(int value, int min, int max)
{
	if (value < min) {
		return min;
	}
	if (value > max) {
		return max;
	}
	return value;
}

static inline int sharp_round_div16(int value)
{
	if (value >= 0) {
		return (value + 8) >> 4;
	}
	return -(((-value) + 8) >> 4);
}

static inline int sharp_quantize_4level_idx_and_value(u8 value, u8 *idx)
{
	int level = ((int)value + 42) / 85;

	if (level > 3) {
		level = 3;
	}

	*idx = (u8)level;
	return level * 85;
}

static inline void sharp_errdiff_spread(s16 *err_curr, s16 *err_next, int width,
	int idx, int dir, int err, int chan)
{
	int right = idx + dir;
	int down_left = idx - dir;
	int down_right = idx + dir;
	int base = (idx * 3) + chan;
	int e7 = sharp_round_div16(err * 7);
	int e3 = sharp_round_div16(err * 3);
	int e5 = sharp_round_div16(err * 5);
	int e1 = sharp_round_div16(err);

	if (right >= 0 && right < width) {
		err_curr[(right * 3) + chan] += e7;
	}
	if (down_left >= 0 && down_left < width) {
		err_next[(down_left * 3) + chan] += e3;
	}
	err_next[base] += e5;
	if (down_right >= 0 && down_right < width) {
		err_next[(down_right * 3) + chan] += e1;
	}
}

static void sharp_dither_errdiff_line_8(const u32 *src_px, int width, int y,
	s16 *err_curr, s16 *err_next, u8 *out_bits)
{
	bool serpentine = (y & 1) != 0;
	int dir = serpentine ? -1 : 1;
	int x = serpentine ? (width - 1) : 0;
	int end = serpentine ? -1 : width;

	for (; x != end; x += dir) {
		u32 px = src_px[x];
		int base = x * 3;
		int r = sharp_clamp_int((int)sharp_srgb_to_linear((u8)((px >> 16) & 0xFF))
			+ err_curr[base + 0], 0, 255);
		int g = sharp_clamp_int((int)sharp_srgb_to_linear((u8)((px >> 8) & 0xFF))
			+ err_curr[base + 1], 0, 255);
		int b = sharp_clamp_int((int)sharp_srgb_to_linear((u8)(px & 0xFF))
			+ err_curr[base + 2], 0, 255);
		int q_r = (r >= 128) ? 255 : 0;
		int q_g = (g >= 128) ? 255 : 0;
		int q_b = (b >= 128) ? 255 : 0;

		out_bits[x] = (u8)(((q_r != 0) << 2) | ((q_g != 0) << 1) | (q_b != 0));

		sharp_errdiff_spread(err_curr, err_next, width, x, dir, r - q_r, 0);
		sharp_errdiff_spread(err_curr, err_next, width, x, dir, g - q_g, 1);
		sharp_errdiff_spread(err_curr, err_next, width, x, dir, b - q_b, 2);
	}
}

static void sharp_dither_errdiff_line_64(const u32 *src_px, int width, int y,
	s16 *err_curr, s16 *err_next, u8 *out_lo, u8 *out_hi)
{
	bool serpentine = (y & 1) != 0;
	int dir = serpentine ? -1 : 1;
	int x = serpentine ? (width - 1) : 0;
	int end = serpentine ? -1 : width;

	for (; x != end; x += dir) {
		u32 px = src_px[x];
		int base = x * 3;
		int r = sharp_clamp_int((int)sharp_srgb_to_linear((u8)((px >> 16) & 0xFF))
			+ err_curr[base + 0], 0, 255);
		int g = sharp_clamp_int((int)sharp_srgb_to_linear((u8)((px >> 8) & 0xFF))
			+ err_curr[base + 1], 0, 255);
		int b = sharp_clamp_int((int)sharp_srgb_to_linear((u8)(px & 0xFF))
			+ err_curr[base + 2], 0, 255);
		u8 r_idx;
		u8 g_idx;
		u8 b_idx;
		int q_r = sharp_quantize_4level_idx_and_value((u8)r, &r_idx);
		int q_g = sharp_quantize_4level_idx_and_value((u8)g, &g_idx);
		int q_b = sharp_quantize_4level_idx_and_value((u8)b, &b_idx);

		out_lo[x] = (u8)(((r_idx & 0x01) << 2) | ((g_idx & 0x01) << 1) | (b_idx & 0x01));
		out_hi[x] = (u8)((((r_idx >> 1) & 0x01) << 2)
			| (((g_idx >> 1) & 0x01) << 1)
			| ((b_idx >> 1) & 0x01));

		sharp_errdiff_spread(err_curr, err_next, width, x, dir, r - q_r, 0);
		sharp_errdiff_spread(err_curr, err_next, width, x, dir, g - q_g, 1);
		sharp_errdiff_spread(err_curr, err_next, width, x, dir, b - q_b, 2);
	}
}

struct sharp_render_params sharp_render_params_snapshot(void)
{
	struct sharp_render_params params = {
		.colors = g_param_colors,
		.mono_cutoff = g_param_mono_cutoff,
		.mono_invert = g_param_mono_invert,
		.dither_algo = g_param_dither_algo,
		.blue_noise = g_param_blue_noise_2197,
	};

	return params;
}

bool sharp_render_params_equal(const struct sharp_render_params *a,
	const struct sharp_render_params *b)
{
	return memcmp(a, b, sizeof(*a)) == 0;
}

size_t sharp_render_panel_line_len(const struct sharp_subpanel *panel,
	const struct sharp_render_params *params)
{
	if (WARN_ON_ONCE(!panel || !params)) {
		return 0;
	}

	if (params->colors == 2) {
		return panel->line_len_mono;
	}
	if (params->colors == 64) {
		return panel->line_len_color64;
	}
	return panel->line_len_color8;
}

static inline void sharp_render_set_color_header(u8 *dst, u8 mode, int y)
{
	dst[0] = mode | ((y >> 8) & 0x03);
	dst[1] = (u8)(y & 0xFF);
}

static inline void sharp_render_pack_8x3(u8 *dst, u32 bits)
{
	dst[0] = (u8)(bits >> 16);
	dst[1] = (u8)(bits >> 8);
	dst[2] = (u8)bits;
}

static inline void sharp_render_set_bit(u8 *dst, int bit_idx)
{
	int byte_idx = bit_idx >> 3;
	int bit_pos = 7 - (bit_idx & 7);

	dst[2 + byte_idx] |= (u8)(1u << bit_pos);
}

static inline void sharp_render_set_mono_header(struct sharp_subpanel *panel,
	u8 *dst, u16 addr)
{
	if (panel && panel->panel_type == SHARP_PANEL_TYPE_SHARP_MONO) {
		// Sharp mono panels use 0x80 + bit-reversed line index.
		dst[0] = 0x80;
		dst[1] = bitrev8((u8)(addr + 1));
		return;
	}

	dst[0] = LPM027M128B_MODE_1BIT | ((addr >> 8) & 0x03);
	dst[1] = (u8)(addr & 0xFF);
}

static void sharp_render_mono_thresholds(const struct sharp_render_params *params,
	int x1, int y, u8 thresholds[4])
{
	static const u8 bayer[16] = {
		0, 8, 2, 10,
		12, 4, 14, 6,
		3, 11, 1, 9,
		15, 7, 13, 5,
	};
	int x;

	for (x = 0; x < 4; x++) {
		int threshold = params->mono_cutoff;

		if (params->dither_algo == SHARP_DITHER_ALGO_MONO_ORDERED) {
			int rank = bayer[((y & 3) * 4) + ((x1 + x) & 3)];

			/*
			 * At cutoff 128, tile thresholds are 8, 24, ..., 248.
			 * Shift their center with cutoff, preserving black/white.
			 * Absolute coordinates keep partial updates in phase.
			 */
			threshold = sharp_clamp_int(threshold + rank * 16 - 120,
				1, 255);
		}
		thresholds[x] = (u8)threshold;
	}
}

static void sharp_render_scalar_mono_row(const u32 *src_px, u8 *dst,
	int width, const u8 thresholds[4], bool invert)
{
	u8 invert_mask = invert ? 0xff : 0;
	int x;

	for (x = 0; x < width; x += 8) {
		u8 packed = 0;
		int bit;

		for (bit = 0; bit < 8; bit++) {
			u32 pixel = src_px[x + bit];
			u32 red = (pixel >> 16) & 0xff;
			u32 green = (pixel >> 8) & 0xff;
			u32 blue = pixel & 0xff;
			u32 luminance = (red * 77) + (green * 150)
				+ (blue * 29);

			if (luminance >= ((u32)thresholds[bit & 3] << 8)) {
				packed |= (u8)(0x80 >> bit);
			}
		}

		dst[x >> 3] = packed ^ invert_mask;
	}
}

// Convert DMA-backed XRGB8888 to tagged mono lines.
static int sharp_render_clip_mono_tagged(struct sharp_subpanel *panel,
	const struct sharp_render_params *params, size_t *result_len, u8 *buf,
	struct drm_framebuffer *fb, const struct drm_rect *clip)
{
	int rc;
	struct iosys_map map[DRM_FORMAT_MAX_PLANES] = { };
	struct iosys_map data[DRM_FORMAT_MAX_PLANES] = { };
	const int width = clip->x2 - clip->x1;
	const int height = clip->y2 - clip->y1;
	const size_t line_len = sharp_render_panel_line_len(panel, params);
	bool use_neon = false;
	int line;

	// Start DMA area
	rc = drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE);
	if (rc) {
		return rc;
	}

	rc = drm_gem_fb_vmap(fb, map, data);
	if (rc) {
		goto out_end_access;
	}
	if (!data[0].vaddr) {
		rc = -ENODEV;
		goto out_vunmap;
	}

#if defined(CONFIG_ARM64) && defined(CONFIG_KERNEL_MODE_NEON)
	if (!(width & 15) && !(clip->x1 & 15) && may_use_simd()) {
		kernel_neon_begin();
		use_neon = true;
		for (line = 0; line < height; line++) {
			int y = clip->y1 + line;
			u8 thresholds[4];
			u8 *line_dst = buf + ((size_t)line * line_len);
			const u8 *src_line = (const u8 *)data[0].vaddr
				+ (y * fb->pitches[0]);
			const u32 *src_px = (const u32 *)(src_line
				+ (clip->x1 * 4));

			sharp_render_set_mono_header(panel, line_dst, (u16)y);
			sharp_render_mono_thresholds(params, clip->x1, y, thresholds);
			sharp_render_neon_mono_row(src_px, line_dst + 2, width,
				thresholds, params->mono_invert);
		}
		kernel_neon_end();
	}
#endif
	if (!use_neon) {
		for (line = 0; line < height; line++) {
			int y = clip->y1 + line;
			u8 thresholds[4];
			u8 *line_dst = buf + ((size_t)line * line_len);
			const u8 *src_line = (const u8 *)data[0].vaddr
				+ ((size_t)y * fb->pitches[0]);
			const u32 *src_px = (const u32 *)(src_line
				+ ((size_t)clip->x1 * 4));

			sharp_render_set_mono_header(panel, line_dst, (u16)y);
			sharp_render_mono_thresholds(params, clip->x1, y, thresholds);
			sharp_render_scalar_mono_row(src_px, line_dst + 2, width,
				thresholds, params->mono_invert);
		}
	}

	rc = 0;
out_vunmap:
	drm_gem_fb_vunmap(fb, map);
out_end_access:
	// End DMA area
	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);

	if (rc) {
		return rc;
	}
	*result_len = (size_t)height * line_len;

	// Success
	return 0;
}

struct sharp_errdiff_ctx {
	s16 *curr;
	s16 *next;
	size_t stride;
	u8 *line_lo;
	u8 *line_hi;
};

struct sharp_color_row_ctx {
	struct sharp_subpanel *panel;
	const struct sharp_render_params *params;
	int width;
	int x1;
	u8 thresh0[3];
	u8 thresh1[3];
	struct sharp_errdiff_ctx err;
};

typedef void (*sharp_color_row_fn)(struct sharp_color_row_ctx *ctx,
	const u32 *src_px, u8 *dst_lo, u8 *dst_hi, int y);

static void sharp_errdiff_ctx_init(struct sharp_errdiff_ctx *ctx,
	struct sharp_subpanel *panel, bool needs_hi)
{
	memset(ctx, 0, sizeof(*ctx));

	if (!panel->dither_err || !panel->dither_line_lo) {
		return;
	}
	if (needs_hi && !panel->dither_line_hi) {
		return;
	}

	ctx->stride = (size_t)panel->width * 3;
	ctx->curr = panel->dither_err;
	ctx->next = panel->dither_err + ctx->stride;
	ctx->line_lo = panel->dither_line_lo;
	ctx->line_hi = panel->dither_line_hi;
	memset(panel->dither_err, 0, ctx->stride * 2 * sizeof(*panel->dither_err));
}

static void sharp_errdiff_ctx_swap(struct sharp_errdiff_ctx *ctx)
{
	s16 *tmp = ctx->curr;

	ctx->curr = ctx->next;
	ctx->next = tmp;
	memset(ctx->next, 0, ctx->stride * sizeof(*ctx->next));
}

static void sharp_render_pack_single_plane(u8 *dst, const u8 *bits, int width)
{
	int full = width / 8;
	int x;

	for (x = 0; x < full; x++) {
		u32 out = 0;
		int px;

		for (px = 0; px < 8; px++) {
			out = (out << 3) | bits[(x * 8) + px];
		}

		sharp_render_pack_8x3(dst, out);
		dst += 3;
	}
}

static void sharp_render_pack_dual_plane(u8 *dst_lo, u8 *dst_hi,
	const u8 *lo, const u8 *hi, int width)
{
	int full = width / 8;
	int x;

	for (x = 0; x < full; x++) {
		u32 out_lo = 0;
		u32 out_hi = 0;
		int px;

		for (px = 0; px < 8; px++) {
			out_lo = (out_lo << 3) | lo[(x * 8) + px];
			out_hi = (out_hi << 3) | hi[(x * 8) + px];
		}

		sharp_render_pack_8x3(dst_lo, out_lo);
		sharp_render_pack_8x3(dst_hi, out_hi);
		dst_lo += 3;
		dst_hi += 3;
	}
}

static void sharp_render_row_8_none(struct sharp_color_row_ctx *ctx,
	const u32 *src_px, u8 *dst_lo, u8 *dst_hi, int y)
{
	int full = ctx->width / 8;
	int x;

	(void)dst_hi;
	(void)y;

	for (x = 0; x < full; x++) {
		u32 out = 0;
		int i;

		for (i = 0; i < 8; i++) {
			u32 px = src_px[i];
			u32 bits = (((px >> 23) & 0x01) << 2)
				| (((px >> 15) & 0x01) << 1)
				| ((px >> 7) & 0x01);

			out = (out << 3) | bits;
		}

		sharp_render_pack_8x3(dst_lo, out);
		dst_lo += 3;
		src_px += 8;
	}
}

static void sharp_render_row_8_27colors(struct sharp_color_row_ctx *ctx,
	const u32 *src_px, u8 *dst_lo, u8 *dst_hi, int y)
{
	int full = ctx->width / 8;
	bool t_index = (ctx->x1 & 1) == 0;
	int x;

	(void)dst_hi;

	if (((ctx->panel->width & 1) == 0) && (y & 1)) {
		t_index = !t_index;
	}

	for (x = 0; x < full; x++) {
		u32 out = 0;
		int i;

		for (i = 0; i < 8; i++) {
			u32 px = src_px[i];
			u8 r = (u8)(px >> 16);
			u8 g = (u8)(px >> 8);
			u8 b = (u8)px;
			u8 t = sharp_dither_threshold_27colors(t_index);
			u32 bits = ((r >= t) << 2);

			t_index = !t_index;
			t = sharp_dither_threshold_27colors(t_index);
			bits |= ((g >= t) << 1);

			t_index = !t_index;
			t = sharp_dither_threshold_27colors(t_index);
			bits |= (b >= t);

			t_index = !t_index;
			out = (out << 3) | bits;
		}

		sharp_render_pack_8x3(dst_lo, out);
		dst_lo += 3;
		src_px += 8;
	}
}

static void sharp_render_row_8_125colors(struct sharp_color_row_ctx *ctx,
	const u32 *src_px, u8 *dst_lo, u8 *dst_hi, int y)
{
	int full = ctx->width / 8;
	u8 threshold_even = sharp_dither_125_threshold[
		sharp_dither_rank_2x2(ctx->x1, y)];
	u8 threshold_odd = sharp_dither_125_threshold[
		sharp_dither_rank_2x2(ctx->x1 + 1, y)];
	int x;

	(void)dst_hi;

	for (x = 0; x < full; x++) {
		u32 out = 0;
		int i;

		for (i = 0; i < 8; i += 2) {
			u32 px_even = src_px[i];
			u32 px_odd = src_px[i + 1];
			u32 bits_even =
				(((u8)(px_even >> 16) >= threshold_even) << 2)
				| (((u8)(px_even >> 8) >= threshold_even) << 1)
				| ((u8)px_even >= threshold_even);
			u32 bits_odd =
				(((u8)(px_odd >> 16) >= threshold_odd) << 2)
				| (((u8)(px_odd >> 8) >= threshold_odd) << 1)
				| ((u8)px_odd >= threshold_odd);

			out = (out << 6) | (bits_even << 3) | bits_odd;
		}

		sharp_render_pack_8x3(dst_lo, out);
		dst_lo += 3;
		src_px += 8;
	}
}

static void sharp_render_row_8_errdiff(struct sharp_color_row_ctx *ctx,
	const u32 *src_px, u8 *dst_lo, u8 *dst_hi, int y)
{
	(void)dst_hi;

	sharp_dither_errdiff_line_8(src_px, ctx->width, y, ctx->err.curr,
		ctx->err.next, ctx->err.line_lo);
	sharp_render_pack_single_plane(dst_lo, ctx->err.line_lo, ctx->width);
	sharp_errdiff_ctx_swap(&ctx->err);
}

static void sharp_render_row_64_none(struct sharp_color_row_ctx *ctx,
	const u32 *src_px, u8 *dst_lo, u8 *dst_hi, int y)
{
	int full = ctx->width / 8;
	int x;

	(void)y;

	for (x = 0; x < full; x++) {
		u32 out_lo = 0;
		u32 out_hi = 0;
		int i;

		for (i = 0; i < 8; i++) {
			u32 px = src_px[i];
			u32 lo_bits = (((px >> 22) & 0x01) << 2)
				| (((px >> 14) & 0x01) << 1)
				| ((px >> 6) & 0x01);
			u32 hi_bits = (((px >> 23) & 0x01) << 2)
				| (((px >> 15) & 0x01) << 1)
				| ((px >> 7) & 0x01);

			out_lo = (out_lo << 3) | lo_bits;
			out_hi = (out_hi << 3) | hi_bits;
		}

		sharp_render_pack_8x3(dst_lo, out_lo);
		sharp_render_pack_8x3(dst_hi, out_hi);
		dst_lo += 3;
		dst_hi += 3;
		src_px += 8;
	}
}

static void sharp_render_row_64_343colors(struct sharp_color_row_ctx *ctx,
	const u32 *src_px, u8 *dst_lo, u8 *dst_hi, int y)
{
	int full = ctx->width / 8;
	int x;

	for (x = 0; x < full; x++) {
		u32 out_lo = 0;
		u32 out_hi = 0;
		int x_base = ctx->x1 + (x * 8);
		int i;

		for (i = 0; i < 8; i++) {
			u32 px = src_px[i];
			u8 r = (u8)(px >> 16);
			u8 g = (u8)(px >> 8);
			u8 b = (u8)px;
			bool toggle = (((x_base + i + y) & 1) == 0);
			const u8 *t = toggle ? ctx->thresh1 : ctx->thresh0;
			u32 lo_bits = 0;
			u32 hi_bits = 0;

			if (r >= t[1]) {
				hi_bits |= 0b100;
			}
			if (r >= t[2] || (r >= t[0] && r < t[1])) {
				lo_bits |= 0b100;
			}

			if (g >= t[1]) {
				hi_bits |= 0b010;
			}
			if (g >= t[2] || (g >= t[0] && g < t[1])) {
				lo_bits |= 0b010;
			}

			if (b >= t[1]) {
				hi_bits |= 0b001;
			}
			if (b >= t[2] || (b >= t[0] && b < t[1])) {
				lo_bits |= 0b001;
			}

			out_lo = (out_lo << 3) | lo_bits;
			out_hi = (out_hi << 3) | hi_bits;
		}

		sharp_render_pack_8x3(dst_lo, out_lo);
		sharp_render_pack_8x3(dst_hi, out_hi);
		dst_lo += 3;
		dst_hi += 3;
		src_px += 8;
	}
}

static inline u8 sharp_quantize_2197_channel(u8 level, u8 rank_bucket)
{
	u8 value = (u8)(level >> 2);
	u8 remainder = (u8)(level & 0x03);

	return (u8)(value + (rank_bucket < remainder));
}

static inline void sharp_quantize_2197_pixel(u8 r, u8 g, u8 b,
	u8 rank_bucket, u32 *lo_bits, u32 *hi_bits)
{
	u8 r_level = sharp_dither_level_13_lut[r];
	u8 g_level = sharp_dither_level_13_lut[g];
	u8 b_level = sharp_dither_level_13_lut[b];
	u8 r_q = sharp_quantize_2197_channel(r_level, rank_bucket);
	u8 g_q = sharp_quantize_2197_channel(g_level, rank_bucket);
	u8 b_q = sharp_quantize_2197_channel(b_level, rank_bucket);

	*lo_bits = ((r_q & 0x01) << 2)
		| ((g_q & 0x01) << 1)
		| (b_q & 0x01);
	*hi_bits = (((r_q >> 1) & 0x01) << 2)
		| (((g_q >> 1) & 0x01) << 1)
		| ((b_q >> 1) & 0x01);
}

static void sharp_render_row_64_2197colors(struct sharp_color_row_ctx *ctx,
	const u32 *src_px, u8 *dst_lo, u8 *dst_hi, int y)
{
	int full = ctx->width / 8;
	const u8 *rank_buckets =
		sharp_dither_2197_rank_bucket + ((y & 1) * 2);
	u8 rank_even = rank_buckets[ctx->x1 & 1];
	u8 rank_odd = rank_buckets[(ctx->x1 + 1) & 1];
	int x;

	for (x = 0; x < full; x++) {
		u32 out_lo = 0;
		u32 out_hi = 0;
		int i;

		for (i = 0; i < 8; i += 2) {
			u32 px_even = src_px[i];
			u32 px_odd = src_px[i + 1];
			u32 lo_even;
			u32 hi_even;
			u32 lo_odd;
			u32 hi_odd;

			sharp_quantize_2197_pixel((u8)(px_even >> 16),
				(u8)(px_even >> 8), (u8)px_even, rank_even,
				&lo_even, &hi_even);
			sharp_quantize_2197_pixel((u8)(px_odd >> 16),
				(u8)(px_odd >> 8), (u8)px_odd, rank_odd,
				&lo_odd, &hi_odd);
			out_lo = (out_lo << 6) | (lo_even << 3) | lo_odd;
			out_hi = (out_hi << 6) | (hi_even << 3) | hi_odd;
		}

		sharp_render_pack_8x3(dst_lo, out_lo);
		sharp_render_pack_8x3(dst_hi, out_hi);
		dst_lo += 3;
		dst_hi += 3;
		src_px += 8;
	}
}

static void sharp_render_row_64_2197colors_blue(struct sharp_color_row_ctx *ctx,
	const u32 *src_px, u8 *dst_lo, u8 *dst_hi, int y)
{
	int full = ctx->width / 8;
	const u16 *rank_row = sharp_blue_noise_32
		+ ((y & SHARP_BLUE_NOISE_MASK) * SHARP_BLUE_NOISE_SIZE);
	int x;

	for (x = 0; x < full; x++) {
		u32 out_lo = 0;
		u32 out_hi = 0;
		int x_base = ctx->x1 + (x * 8);
		int i;

		for (i = 0; i < 8; i++) {
			u32 px = src_px[i];
			u8 r = (u8)(px >> 16);
			u8 g = (u8)(px >> 8);
			u8 b = (u8)px;
			u8 rank_bucket = (u8)(rank_row[
				(x_base + i) & SHARP_BLUE_NOISE_MASK] >> 8);
			u32 lo_bits;
			u32 hi_bits;

			sharp_quantize_2197_pixel(r, g, b, rank_bucket,
				&lo_bits, &hi_bits);
			out_lo = (out_lo << 3) | lo_bits;
			out_hi = (out_hi << 3) | hi_bits;
		}

		sharp_render_pack_8x3(dst_lo, out_lo);
		sharp_render_pack_8x3(dst_hi, out_hi);
		dst_lo += 3;
		dst_hi += 3;
		src_px += 8;
	}
}

static void sharp_render_row_64_errdiff(struct sharp_color_row_ctx *ctx,
	const u32 *src_px, u8 *dst_lo, u8 *dst_hi, int y)
{
	sharp_dither_errdiff_line_64(src_px, ctx->width, y, ctx->err.curr,
		ctx->err.next, ctx->err.line_lo, ctx->err.line_hi);
	sharp_render_pack_dual_plane(dst_lo, dst_hi, ctx->err.line_lo,
		ctx->err.line_hi, ctx->width);
	sharp_errdiff_ctx_swap(&ctx->err);
}

static sharp_color_row_fn sharp_render_select_color8_row(
	struct sharp_color_row_ctx *ctx)
{
	switch (ctx->params->dither_algo) {
	case SHARP_DITHER_ALGO_27COLORS:
		return sharp_render_row_8_27colors;
	case SHARP_DITHER_ALGO_125COLORS:
		return sharp_render_row_8_125colors;
	case SHARP_DITHER_ALGO_ERRDIFF2:
		sharp_errdiff_ctx_init(&ctx->err, ctx->panel, false);
		if (ctx->err.curr && ctx->err.next && ctx->err.line_lo) {
			return sharp_render_row_8_errdiff;
		}
		return sharp_render_row_8_none;
	default:
		return sharp_render_row_8_none;
	}
}

static sharp_color_row_fn sharp_render_select_color64_row(
	struct sharp_color_row_ctx *ctx)
{
	switch (ctx->params->dither_algo) {
	case SHARP_DITHER_ALGO_343COLORS:
		sharp_dither_thresholds_343colors(ctx->thresh0, ctx->thresh1);
		return sharp_render_row_64_343colors;
	case SHARP_DITHER_ALGO_2197COLORS:
		if (ctx->params->blue_noise) {
			return sharp_render_row_64_2197colors_blue;
		}
		return sharp_render_row_64_2197colors;
	case SHARP_DITHER_ALGO_ERRDIFF2:
		sharp_errdiff_ctx_init(&ctx->err, ctx->panel, true);
		if (ctx->err.curr && ctx->err.next && ctx->err.line_lo && ctx->err.line_hi) {
			return sharp_render_row_64_errdiff;
		}
		return sharp_render_row_64_none;
	default:
		return sharp_render_row_64_none;
	}
}

#if defined(CONFIG_ARM64) && defined(CONFIG_KERNEL_MODE_NEON)
static bool sharp_render_select_neon_mode(
	const struct sharp_render_params *params, enum sharp_neon_mode *mode)
{
	if (params->colors == 64) {
		switch (params->dither_algo) {
		case SHARP_DITHER_ALGO_343COLORS:
			*mode = SHARP_NEON_64_343COLORS;
			break;
		case SHARP_DITHER_ALGO_2197COLORS:
			*mode = params->blue_noise
				? SHARP_NEON_64_2197COLORS_BLUE
				: SHARP_NEON_64_2197COLORS;
			break;
		case SHARP_DITHER_ALGO_ERRDIFF2:
			return false;
		default:
			*mode = SHARP_NEON_64_NONE;
			break;
		}
		return true;
	}

	switch (params->dither_algo) {
	case SHARP_DITHER_ALGO_27COLORS:
		*mode = SHARP_NEON_8_27COLORS;
		break;
	case SHARP_DITHER_ALGO_125COLORS:
		*mode = SHARP_NEON_8_125COLORS;
		break;
	case SHARP_DITHER_ALGO_ERRDIFF2:
		return false;
	default:
		*mode = SHARP_NEON_8_NONE;
		break;
	}
	return true;
}
#endif

static int sharp_render_clip_color_tagged(struct sharp_subpanel *panel,
	const struct sharp_render_params *params, size_t *result_len, u8 *buf,
	struct drm_framebuffer *fb, const struct drm_rect *clip)
{
	int rc;
	struct iosys_map map[DRM_FORMAT_MAX_PLANES] = { };
	struct iosys_map data[DRM_FORMAT_MAX_PLANES] = { };
	const int width = clip->x2 - clip->x1;
	const int height = clip->y2 - clip->y1;
	const size_t plane_len = (size_t)((width * 3 + 7) / 8);
	const size_t line_len = sharp_render_panel_line_len(panel, params);
	struct sharp_color_row_ctx ctx;
	sharp_color_row_fn row_fn;
	int line;
#if defined(CONFIG_ARM64) && defined(CONFIG_KERNEL_MODE_NEON)
	enum sharp_neon_mode neon_mode = SHARP_NEON_8_NONE;
	bool neon_candidate;
	bool use_neon = false;
#endif

	WARN_ON_ONCE(width != panel->width);

	memset(&ctx, 0, sizeof(ctx));
	ctx.panel = panel;
	ctx.params = params;
	ctx.width = width;
	ctx.x1 = clip->x1;

	if (params->colors == 64) {
		row_fn = sharp_render_select_color64_row(&ctx);
	} else {
		row_fn = sharp_render_select_color8_row(&ctx);
	}

#if defined(CONFIG_ARM64) && defined(CONFIG_KERNEL_MODE_NEON)
	neon_candidate = !(width & 15) && !(clip->x1 & 15)
		&& sharp_render_select_neon_mode(params, &neon_mode);
#endif

	rc = drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE);
	if (rc) {
		return rc;
	}

	rc = drm_gem_fb_vmap(fb, map, data);
	if (rc) {
		goto out_end_access;
	}
	if (!data[0].vaddr) {
		rc = -ENODEV;
		goto out_vunmap;
	}

#if defined(CONFIG_ARM64) && defined(CONFIG_KERNEL_MODE_NEON)
	if (neon_candidate && may_use_simd()) {
		kernel_neon_begin();
		use_neon = true;
	}
#endif

#if defined(CONFIG_ARM64) && defined(CONFIG_KERNEL_MODE_NEON)
	if (use_neon) {
		for (line = 0; line < height; line++) {
			int y = clip->y1 + line;
			u8 *dst = buf + ((size_t)line * line_len);

			if (params->colors == 64) {
				u8 *dst_hi = dst + 2 + plane_len;

				sharp_render_set_color_header(dst,
					LPM027M128B_MODE_64COLOR_LSB, y);
				sharp_render_set_color_header(dst_hi,
					LPM027M128B_MODE_64COLOR_MSB, y);
			} else {
				sharp_render_set_color_header(dst,
					LPM027M128B_MODE_8COLOR, y);
			}
		}
		sharp_render_neon_rows(neon_mode,
			(const u8 *)data[0].vaddr
				+ ((size_t)clip->y1 * fb->pitches[0])
				+ ((size_t)clip->x1 * 4),
			fb->pitches[0], buf, line_len,
			params->colors == 64 ? plane_len : 0,
			width, clip->x1, clip->y1, height);
	} else
#endif
	{
		for (line = 0; line < height; line++) {
			int y = clip->y1 + line;
			u8 *dst = buf + ((size_t)line * line_len);
			u8 *dst_hi = NULL;
			const u8 *src_line = (const u8 *)data[0].vaddr
				+ ((size_t)y * fb->pitches[0]);
			const u32 *src_px = (const u32 *)(src_line
				+ ((size_t)clip->x1 * 4));

			if (params->colors == 64) {
				dst_hi = dst + 2 + plane_len;
				sharp_render_set_color_header(dst,
					LPM027M128B_MODE_64COLOR_LSB, y);
				sharp_render_set_color_header(dst_hi,
					LPM027M128B_MODE_64COLOR_MSB, y);
			} else {
				sharp_render_set_color_header(dst,
					LPM027M128B_MODE_8COLOR, y);
			}
			row_fn(&ctx, src_px, dst + 2,
				dst_hi ? dst_hi + 2 : NULL, y);
		}
	}

#if defined(CONFIG_ARM64) && defined(CONFIG_KERNEL_MODE_NEON)
	if (use_neon) {
		kernel_neon_end();
	}
#endif

	rc = 0;
out_vunmap:
	drm_gem_fb_vunmap(fb, map);
out_end_access:
	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);

	if (rc) {
		return rc;
	}
	*result_len = (size_t)height * line_len;

	return 0;
}

static void sharp_render_nv12_source_row(struct sharp_subpanel *panel,
	const u8 *y_plane, const u8 *uv_plane, int source_x, int width)
{
	int cached_uv_x = -1;
	int r_chroma = 0;
	int g_chroma = 0;
	int b_chroma = 0;
	int x;

	for (x = 0; x < width; x++) {
		int uv_x = (source_x + x) & ~1;
		int c;
		u8 r;
		u8 g;
		u8 b;

		if (uv_x != cached_uv_x) {
			u8 u = uv_plane[uv_x];
			u8 v = uv_plane[uv_x + 1];

			r_chroma = panel->nv12_r_chroma[v];
			g_chroma = panel->nv12_g_u_chroma[u]
				+ panel->nv12_g_v_chroma[v];
			b_chroma = panel->nv12_b_chroma[u];
			cached_uv_x = uv_x;
		}

		c = panel->nv12_y_scaled[y_plane[source_x + x]];
		r = sharp_clamp_u8((c + r_chroma + 128) >> 8);
		g = sharp_clamp_u8((c + g_chroma + 128) >> 8);
		b = sharp_clamp_u8((c + b_chroma + 128) >> 8);
		panel->source_row[x] = ((u32)r << 16) | ((u32)g << 8) | b;
	}
}

void sharp_render_init_nv12_tables(struct sharp_subpanel *panel)
{
	int value;

	if (!panel) {
		return;
	}

	for (value = 0; value < 256; value++) {
		int chroma = value - 128;

		panel->nv12_y_scaled[value] =
			298 * max_t(int, value - 16, 0);
		panel->nv12_r_chroma[value] = 459 * chroma;
		panel->nv12_g_u_chroma[value] = -55 * chroma;
		panel->nv12_g_v_chroma[value] = -136 * chroma;
		panel->nv12_b_chroma[value] = 541 * chroma;
	}
}

static void sharp_render_nv12_row_8_threshold(
	struct sharp_color_row_ctx *ctx, const u8 *src_y, const u8 *src_uv,
	u8 *dst, int y)
{
	struct sharp_subpanel *panel = ctx->panel;
	bool dither27_toggle = (ctx->x1 & 1) == 0;
	int cached_uv_x = -1;
	int r_chroma = 0;
	int g_chroma = 0;
	int b_chroma = 0;
	int full = ctx->width / 8;
	int group;

	if (((panel->width & 1) == 0) && (y & 1)) {
		dither27_toggle = !dither27_toggle;
	}

	for (group = 0; group < full; group++) {
		u32 out = 0;
		int i;

		for (i = 0; i < 8; i++) {
			int local_x = (group * 8) + i;
			int source_x = ctx->x1 + local_x;
			int uv_x = source_x & ~1;
			int threshold_r;
			int threshold_g;
			int threshold_b;
			int luma;
			u32 bits;

			if (uv_x != cached_uv_x) {
				u8 u = src_uv[uv_x];
				u8 v = src_uv[uv_x + 1];

				r_chroma = panel->nv12_r_chroma[v];
				g_chroma = panel->nv12_g_u_chroma[u]
					+ panel->nv12_g_v_chroma[v];
				b_chroma = panel->nv12_b_chroma[u];
				cached_uv_x = uv_x;
			}

			switch (ctx->params->dither_algo) {
			case SHARP_DITHER_ALGO_27COLORS:
				threshold_r = sharp_dither_threshold_27colors(
					dither27_toggle);
				threshold_g = sharp_dither_threshold_27colors(
					!dither27_toggle);
				threshold_b = threshold_r;
				dither27_toggle = !dither27_toggle;
				break;
			case SHARP_DITHER_ALGO_125COLORS:
				threshold_r = sharp_dither_125_threshold[
					sharp_dither_rank_2x2(source_x, y)];
				threshold_g = threshold_r;
				threshold_b = threshold_r;
				break;
			default:
				threshold_r = 128;
				threshold_g = 128;
				threshold_b = 128;
				break;
			}

			luma = panel->nv12_y_scaled[src_y[source_x]] + 128;
			bits = ((luma + r_chroma >= (threshold_r << 8)) << 2)
				| ((luma + g_chroma >= (threshold_g << 8)) << 1)
				| (luma + b_chroma >= (threshold_b << 8));
			out = (out << 3) | bits;
		}

		sharp_render_pack_8x3(dst, out);
		dst += 3;
	}
}

static void sharp_render_nv12_mono_row(const u8 *src_y, u8 *dst,
	int width, const u8 thresholds[4], bool invert)
{
	u8 limited[4];
	u8 invert_mask = invert ? 0xff : 0;
	int x;

	for (x = 0; x < 4; x++) {
		limited[x] = (u8)(16 + (((int)thresholds[x] * 219 + 127) / 255));
	}

	for (x = 0; x < width; x += 8) {
		u8 packed = 0;
		int bit;

		for (bit = 0; bit < 8; bit++) {
			/*
			 * NV12 luma uses limited range. Compare in that range so
			 * monochrome output does not need a YUV-to-RGB conversion.
			 */
			if (src_y[x + bit] >= limited[bit & 3]) {
				packed |= (u8)(0x80 >> bit);
			}
		}

		dst[x >> 3] = packed ^ invert_mask;
	}
}

static int sharp_render_clip_nv12_tagged(struct sharp_subpanel *panel,
	const struct sharp_render_params *params, size_t *result_len, u8 *buf,
	struct drm_framebuffer *fb, const struct drm_rect *clip)
{
	struct iosys_map map[DRM_FORMAT_MAX_PLANES] = { };
	struct iosys_map data[DRM_FORMAT_MAX_PLANES] = { };
	const int width = clip->x2 - clip->x1;
	const int height = clip->y2 - clip->y1;
	const size_t plane_len = (size_t)((width * 3 + 7) / 8);
	const size_t line_len = sharp_render_panel_line_len(panel, params);
	struct sharp_color_row_ctx ctx;
	sharp_color_row_fn row_fn = NULL;
	int line;
	int rc;
#if defined(CONFIG_ARM64) && defined(CONFIG_KERNEL_MODE_NEON)
	enum sharp_neon_mode neon_mode = SHARP_NEON_64_NONE;
	bool use_neon = false;
#endif

	if (!panel->source_row || width != panel->width) {
		return -EINVAL;
	}

	rc = drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE);
	if (rc) {
		return rc;
	}

	rc = drm_gem_fb_vmap(fb, map, data);
	if (rc) {
		goto out_end_access;
	}

	if (!data[0].vaddr || !data[1].vaddr) {
		rc = -ENODEV;
		goto out_vunmap;
	}

	memset(&ctx, 0, sizeof(ctx));
	ctx.panel = panel;
	ctx.params = params;
	ctx.width = width;
	ctx.x1 = clip->x1;
	if (params->colors == 64) {
		row_fn = sharp_render_select_color64_row(&ctx);
	} else if (params->colors != 2) {
		row_fn = sharp_render_select_color8_row(&ctx);
	}

#if defined(CONFIG_ARM64) && defined(CONFIG_KERNEL_MODE_NEON)
	if (params->colors == 64 && !(width & 15) && !(clip->x1 & 15)
		&& sharp_render_select_neon_mode(params, &neon_mode)
		&& may_use_simd()) {
		kernel_neon_begin();
		use_neon = true;
	}
#endif

	for (line = 0; line < height; line++) {
		int y = clip->y1 + line;
		u8 *dst = buf + ((size_t)line * line_len);
		const u8 *src_y = (const u8 *)data[0].vaddr
			+ ((size_t)y * fb->pitches[0]);

		if (params->colors == 2) {
			u8 thresholds[4];

			sharp_render_set_mono_header(panel, dst, (u16)y);
			sharp_render_mono_thresholds(params, clip->x1, y, thresholds);
			sharp_render_nv12_mono_row(src_y + clip->x1, dst + 2,
				width, thresholds, params->mono_invert);
		} else {
			u8 *dst_hi = NULL;
			const u8 *src_uv = (const u8 *)data[1].vaddr
				+ ((size_t)(y >> 1) * fb->pitches[1]);

			if (params->colors == 8
				&& params->dither_algo != SHARP_DITHER_ALGO_ERRDIFF2) {
				sharp_render_set_color_header(dst,
					LPM027M128B_MODE_8COLOR, y);
				sharp_render_nv12_row_8_threshold(&ctx, src_y,
					src_uv, dst + 2, y);
				continue;
			}

			sharp_render_nv12_source_row(panel, src_y, src_uv,
				clip->x1, width);
			if (params->colors == 64) {
				dst_hi = dst + 2 + plane_len;
				sharp_render_set_color_header(dst,
					LPM027M128B_MODE_64COLOR_LSB, y);
				sharp_render_set_color_header(dst_hi,
					LPM027M128B_MODE_64COLOR_MSB, y);
			} else {
				sharp_render_set_color_header(dst,
					LPM027M128B_MODE_8COLOR, y);
			}
#if defined(CONFIG_ARM64) && defined(CONFIG_KERNEL_MODE_NEON)
			if (use_neon) {
				sharp_render_neon_rows(neon_mode,
					(const u8 *)panel->source_row, (size_t)width * 4,
					dst, line_len, plane_len, width, clip->x1, y, 1);
			} else
#endif
			{
				row_fn(&ctx, panel->source_row, dst + 2,
					dst_hi ? dst_hi + 2 : NULL, y);
			}
		}
	}

#if defined(CONFIG_ARM64) && defined(CONFIG_KERNEL_MODE_NEON)
	if (use_neon) {
		kernel_neon_end();
	}
#endif

	*result_len = (size_t)height * line_len;
	rc = 0;

out_vunmap:
	drm_gem_fb_vunmap(fb, map);
out_end_access:
	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);
	return rc;
}

int sharp_render_clip(struct sharp_subpanel *panel,
	const struct sharp_render_params *params, size_t *buf_len, u8 *buf,
	struct drm_framebuffer *fb, const struct drm_rect *clip)
{
	if (fb->format && fb->format->format == DRM_FORMAT_NV12) {
		return sharp_render_clip_nv12_tagged(panel, params, buf_len, buf,
			fb, clip);
	}
	if (params->colors == 2) {
		return sharp_render_clip_mono_tagged(panel, params, buf_len, buf, fb, clip);
	}
	return sharp_render_clip_color_tagged(panel, params, buf_len, buf, fb, clip);
}
