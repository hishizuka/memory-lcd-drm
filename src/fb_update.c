// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DRM driver for 2.7" Sharp Memory LCD
 *
 * Copyright 2023 Andrew D'Angelo
 */

/* Framebuffer damage, conversion submission, clear, and redraw. */

#include "sharp_drm.h"
#include "fb_update_internal.h"

#include <linux/dma-mapping.h>
#include <linux/ktime.h>
#include <linux/string.h>

#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_modeset_lock.h>

static bool sharp_drm_panel_needs_full_refresh(struct sharp_subpanel *panel,
	const struct sharp_render_params *params)
{
	return !panel->desired || !panel->desired_valid
		|| !sharp_render_params_equal(&panel->desired_params, params);
}

static bool sharp_drm_framebuffer_imported(struct drm_framebuffer *fb)
{
	struct drm_gem_dma_object *dma_obj;

	if (!fb) {
		return false;
	}
	dma_obj = drm_fb_dma_get_gem_obj(fb, 0);
	return dma_obj && drm_gem_is_imported(&dma_obj->base);
}

static bool sharp_drm_tx_slot_available(
	const struct sharp_memory_device *sdev)
{
	unsigned int i;

	for (i = 0; i < SHARP_TX_SLOT_COUNT; i++) {
		if (READ_ONCE(sdev->tx_slots[i].state) == SHARP_TX_SLOT_FREE) {
			return true;
		}
	}
	return false;
}

int sharp_drm_fb_dirty(struct drm_framebuffer *fb, const struct drm_rect *dirty_rect)
{
	int rc = 0;
	struct sharp_render_params params;
	struct drm_rect clip;
	struct sharp_memory_device *sdev;
	int drm_idx;
	size_t buf_lens[SHARP_SUBPANELS_MAX] = { };
	struct drm_rect panel_clips[SHARP_SUBPANELS_MAX];
	bool convert_panel[SHARP_SUBPANELS_MAX] = { };
	bool force_full = false;
	bool imported;
	bool throttled = false;
	int i;
	u64 update_start_ns;

	// Get device info from DRM struct.
	sdev = drm_to_device(fb->dev);
	// Enter DRM device resource area.
	if (!drm_dev_enter(fb->dev, &drm_idx)) {
		return -ENODEV;
	}

	update_start_ns = sharp_perf_enabled() ? ktime_get_ns() : 0;
	imported = sharp_drm_framebuffer_imported(fb);

retry_lock:
	if (imported && !sharp_drm_tx_slot_available(sdev)) {
		throttled = true;
		/* Atomic state is already committed; signals must not drop its data. */
		wait_event(sdev->tx_slot_wait,
			sharp_drm_tx_slot_available(sdev)
				|| READ_ONCE(sdev->tx_shutdown)
				|| !READ_ONCE(sdev->tx_running));
		if (READ_ONCE(sdev->tx_shutdown)
			|| !READ_ONCE(sdev->tx_running)) {
			rc = -ESHUTDOWN;
			goto out_record;
		}
	}

	mutex_lock(&sdev->tx_control_lock);
	mutex_lock(&sdev->fb_lock);
	if (sdev->tx_shutdown || !sdev->tx_running) {
		rc = -ESHUTDOWN;
		goto out_unlock_fb;
	}
	if (imported && !sharp_drm_tx_slot_available(sdev)) {
		mutex_unlock(&sdev->fb_lock);
		mutex_unlock(&sdev->tx_control_lock);
		goto retry_lock;
	}
	if (throttled) {
		sharp_perf_add(&sdev->perf.throttled_updates, 1);
	}
	params = sharp_render_params_snapshot();

	for (i = 0; i < sdev->panel_count; i++) {
		if (sharp_drm_panel_needs_full_refresh(&sdev->panels[i], &params)) {
			force_full = true;
			break;
		}
	}
	mutex_unlock(&sdev->fb_lock);
	sharp_drm_prepare_clip(fb, dirty_rect, force_full, &params, &clip);

	/* Error diffusion depends on all preceding rows of the same panel. */
	if (force_full || (params.colors != 2 &&
		params.dither_algo == SHARP_DITHER_ALGO_ERRDIFF2)) {
		sharp_perf_add(&sdev->perf.full_refreshes, 1);
	}

	for (i = 0; i < sdev->panel_count; i++) {
		struct sharp_subpanel *panel = &sdev->panels[i];

		convert_panel[i] = sharp_drm_panel_clip(panel, &clip,
			&panel_clips[i]);
		if (!convert_panel[i]) {
			continue;
		}

		{
			u64 convert_start_ns =
				sharp_perf_enabled() ? ktime_get_ns() : 0;

			rc = sharp_render_clip(panel, &params, &buf_lens[i],
				panel->buf, fb, &panel_clips[i]);
			if (convert_start_ns) {
				sharp_perf_add(&sdev->perf.convert_ns,
					ktime_get_ns() - convert_start_ns);
			}
		}
		if (rc) {
			goto out_unlock_control;
		}
	}

	/* The TX worker can finish and send older slots during conversion. */
	mutex_lock(&sdev->fb_lock);
	for (i = 0; i < sdev->panel_count; i++) {
		if (!convert_panel[i]) {
			continue;
		}
		rc = sharp_tx_store_converted_locked(sdev, &sdev->panels[i],
			&params, &panel_clips[i], buf_lens[i]);
		if (rc) {
			goto out_unlock_fb;
		}
	}

	sharp_tx_kick_locked(sdev);

out_unlock_fb:
	mutex_unlock(&sdev->fb_lock);
out_unlock_control:
	mutex_unlock(&sdev->tx_control_lock);

out_record:
	if (update_start_ns) {
		sharp_perf_record_update(sdev,
			ktime_get_ns() - update_start_ns);
	}
	// Exit DRM device resource area.
	drm_dev_exit(drm_idx);

	return rc;
}

static void sharp_drm_invalidate_shadows(struct sharp_memory_device *sdev)
{
	mutex_lock(&sdev->tx_control_lock);
	mutex_lock(&sdev->fb_lock);
	sharp_tx_invalidate_all_locked(sdev);
	mutex_unlock(&sdev->fb_lock);
	mutex_unlock(&sdev->tx_control_lock);
}

int sharp_drm_redraw_primary(struct sharp_memory_device *sdev,
	int height)
{
	struct drm_plane *plane;
	struct drm_framebuffer *fb;
	struct drm_rect dirty_rect;
	int ret;

	if (!sdev) {
		return -ENODEV;
	}

	plane = &sdev->pipe.plane;
	ret = drm_modeset_lock(&plane->mutex, NULL);
	if (ret) {
		return ret;
	}

	fb = plane->state ? plane->state->fb : NULL;
	if (!fb) {
		drm_modeset_unlock(&plane->mutex);
		return 0;
	}
	drm_framebuffer_get(fb);

	dirty_rect.x1 = 0;
	dirty_rect.x2 = fb->width;
	dirty_rect.y1 = 0;
	dirty_rect.y2 = (height > 0) ? min_t(int, height, fb->height) : fb->height;

	sharp_drm_invalidate_shadows(sdev);
	ret = sharp_drm_fb_dirty(fb, &dirty_rect);
	if (!ret) {
		ret = sharp_tx_wait_idle(sdev);
	}

	drm_framebuffer_put(fb);
	drm_modeset_unlock(&plane->mutex);

	return ret;
}

static int sharp_drm_fill_framebuffer_black(struct drm_framebuffer *fb)
{
	struct iosys_map map[DRM_FORMAT_MAX_PLANES] = { };
	struct iosys_map data[DRM_FORMAT_MAX_PLANES] = { };
	size_t row_lens[2];
	unsigned int heights[2];
	u8 fill[2];
	unsigned int planes;
	unsigned int plane;
	unsigned int y;
	int ret;

	if (!fb) {
		return -ENODEV;
	}
	switch (fb->format->format) {
	case DRM_FORMAT_XRGB8888:
		planes = 1;
		row_lens[0] = (size_t)fb->width * 4;
		heights[0] = fb->height;
		fill[0] = 0;
		break;
	case DRM_FORMAT_NV12:
		planes = 2;
		row_lens[0] = fb->width;
		row_lens[1] = DIV_ROUND_UP(fb->width, 2) * 2;
		heights[0] = fb->height;
		heights[1] = DIV_ROUND_UP(fb->height, 2);
		/* The converter uses limited-range NV12. */
		fill[0] = 16;
		fill[1] = 128;
		break;
	default:
		return -EINVAL;
	}
	for (plane = 0; plane < planes; plane++) {
		if (row_lens[plane] > fb->pitches[plane]) {
			return -EINVAL;
		}
	}

	ret = drm_gem_fb_begin_cpu_access(fb, DMA_TO_DEVICE);
	if (ret) {
		return ret;
	}

	ret = drm_gem_fb_vmap(fb, map, data);
	if (ret) {
		goto out_end_access;
	}
	for (plane = 0; plane < planes; plane++) {
		if (!data[plane].vaddr || data[plane].is_iomem) {
			ret = -ENODEV;
			goto out_vunmap;
		}
	}
	for (plane = 0; plane < planes; plane++) {
		u8 *dst = data[plane].vaddr;

		for (y = 0; y < heights[plane]; y++) {
			memset(dst + (size_t)y * fb->pitches[plane],
				fill[plane], row_lens[plane]);
		}
	}

out_vunmap:
	drm_gem_fb_vunmap(fb, map);
out_end_access:
	drm_gem_fb_end_cpu_access(fb, DMA_TO_DEVICE);

	return ret;
}

int sharp_drm_clear_primary(struct sharp_memory_device *sdev)
{
	struct drm_plane *plane;
	struct drm_framebuffer *fb;
	struct drm_rect dirty_rect;
	int ret;

	if (!sdev) {
		return -ENODEV;
	}

	plane = &sdev->pipe.plane;
	ret = drm_modeset_lock(&plane->mutex, NULL);
	if (ret) {
		return ret;
	}

	fb = plane->state ? plane->state->fb : NULL;
	if (!fb) {
		drm_modeset_unlock(&plane->mutex);
		return 0;
	}
	drm_framebuffer_get(fb);

	mutex_lock(&sdev->tx_control_lock);
	mutex_lock(&sdev->fb_lock);
	ret = sharp_drm_fill_framebuffer_black(fb);
	if (ret) {
		mutex_unlock(&sdev->fb_lock);
		mutex_unlock(&sdev->tx_control_lock);
		goto out_put;
	}
	sharp_tx_invalidate_all_locked(sdev);
	mutex_unlock(&sdev->fb_lock);
	mutex_unlock(&sdev->tx_control_lock);

	dirty_rect.x1 = 0;
	dirty_rect.x2 = fb->width;
	dirty_rect.y1 = 0;
	dirty_rect.y2 = fb->height;

	ret = sharp_drm_fb_dirty(fb, &dirty_rect);
	if (!ret) {
		ret = sharp_tx_wait_idle(sdev);
	}

out_put:
	drm_framebuffer_put(fb);
	drm_modeset_unlock(&plane->mutex);

	return ret;
}
