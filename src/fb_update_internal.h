/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SHARP_FB_UPDATE_INTERNAL_H_
#define SHARP_FB_UPDATE_INTERNAL_H_

#include "sharp_drm.h"

static inline void sharp_drm_prepare_clip(struct drm_framebuffer *fb,
	const struct drm_rect *dirty_rect, bool force_full,
	const struct sharp_render_params *params, struct drm_rect *clip)
{
	*clip = *dirty_rect;
	if (force_full) {
		clip->x1 = 0;
		clip->x2 = fb->width;
	}
	/* Error diffusion depends on preceding rows within the selected panel. */
	if (force_full || (params->colors != 2 &&
		params->dither_algo == SHARP_DITHER_ALGO_ERRDIFF2)) {
		clip->y1 = 0;
		clip->y2 = fb->height;
	}
}

static inline bool sharp_drm_panel_clip(const struct sharp_subpanel *panel,
	const struct drm_rect *clip, struct drm_rect *panel_clip)
{
	int panel_x1 = (int)panel->x_offset;
	int panel_x2 = panel_x1 + (int)panel->width;

	if (clip->x1 >= panel_x2 || clip->x2 <= panel_x1 ||
		clip->x1 >= clip->x2 || clip->y1 >= clip->y2) {
		return false;
	}
	/* SPI updates carry complete rows even when only a few pixels changed. */
	*panel_clip = *clip;
	panel_clip->x1 = panel_x1;
	panel_clip->x2 = panel_x2;
	return true;
}

#endif
