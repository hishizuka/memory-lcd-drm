// SPDX-License-Identifier: GPL-2.0-or-later
#include <assert.h>
#include <stdio.h>
#include "../src/fb_update_internal.h"

int main(void)
{
	struct drm_framebuffer fb = { .width = 544, .height = 451 };
	struct sharp_subpanel panels[] = {
		{ .width = 272, .height = 451 },
		{ .width = 272, .height = 451, .x_offset = 272 },
	};
	struct sharp_render_params params = { .colors = 64 };
	struct drm_rect damage = { 270, 100, 272, 101 }, clip, row;

	sharp_drm_prepare_clip(&fb, &damage, false, &params, &clip);
	assert(sharp_drm_panel_clip(&panels[0], &clip, &row));
	assert(row.x1 == 0 && row.x2 == 272 && row.y1 == 100 && row.y2 == 101);
	assert(!sharp_drm_panel_clip(&panels[1], &clip, &row));
	damage.x1 = 272;
	damage.x2 = 273;
	sharp_drm_prepare_clip(&fb, &damage, false, &params, &clip);
	assert(!sharp_drm_panel_clip(&panels[0], &clip, &row));
	assert(sharp_drm_panel_clip(&panels[1], &clip, &row));
	assert(row.x1 == 272 && row.x2 == 544 && row.y1 == 100 && row.y2 == 101);

	params.dither_algo = SHARP_DITHER_ALGO_ERRDIFF2;
	sharp_drm_prepare_clip(&fb, &damage, false, &params, &clip);
	assert(!sharp_drm_panel_clip(&panels[0], &clip, &row));
	assert(sharp_drm_panel_clip(&panels[1], &clip, &row));
	assert(row.x1 == 272 && row.x2 == 544 && row.y1 == 0 && row.y2 == 451);

	/* Invalid shadows or changed parameters must refresh both panels. */
	sharp_drm_prepare_clip(&fb, &damage, true, &params, &clip);
	assert(sharp_drm_panel_clip(&panels[0], &clip, &row));
	assert(row.x1 == 0 && row.x2 == 272 && row.y1 == 0 && row.y2 == 451);
	assert(sharp_drm_panel_clip(&panels[1], &clip, &row));
	assert(row.x1 == 272 && row.x2 == 544 && row.y1 == 0 && row.y2 == 451);

	damage.x1 = 271;
	sharp_drm_prepare_clip(&fb, &damage, false, &params, &clip);
	assert(sharp_drm_panel_clip(&panels[0], &clip, &row));
	assert(sharp_drm_panel_clip(&panels[1], &clip, &row));
	params.colors = 2;
	sharp_drm_prepare_clip(&fb, &damage, false, &params, &clip);
	assert(clip.y1 == 100 && clip.y2 == 101);
	params.dither_algo = SHARP_DITHER_ALGO_MONO_ORDERED;
	sharp_drm_prepare_clip(&fb, &damage, false, &params, &clip);
	assert(clip.y1 == 100 && clip.y2 == 101);
	damage.x2 = damage.x1;
	sharp_drm_prepare_clip(&fb, &damage, false, &params, &clip);
	assert(!sharp_drm_panel_clip(&panels[0], &clip, &row));
	assert(!sharp_drm_panel_clip(&panels[1], &clip, &row));
	puts("all framebuffer damage cases passed");
	return 0;
}
