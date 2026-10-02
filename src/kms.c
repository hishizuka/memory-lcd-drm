// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DRM driver for 2.7" Sharp Memory LCD
 *
 * Copyright 2023 Andrew D'Angelo
 */

#include "sharp_drm.h"

#include <linux/bitmap.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/property.h>
#include <linux/string.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>

#include "ioctl_iface.h"

#define SHARP_VCOM_JDI_EXTCOMIN_HZ 128ULL
#define SHARP_VCOM_SHARP_MONO_TOGGLE_NS NSEC_PER_SEC

static bool g_cacheable_buffers;
module_param_named(cacheable_buffers, g_cacheable_buffers, bool, 0444);
MODULE_PARM_DESC(cacheable_buffers,
	"Cache driver-owned buffers for CPU-only producers; do not enable for DMA writers");

static u64 sharp_drm_vcom_toggle_period_ns(const struct sharp_memory_device *sdev)
{
	if (sdev && sdev->panels[0].panel_type == SHARP_PANEL_TYPE_JDI) {
		return NSEC_PER_SEC / (SHARP_VCOM_JDI_EXTCOMIN_HZ * 2);
	}

	return SHARP_VCOM_SHARP_MONO_TOGGLE_NS;
}

static enum hrtimer_restart vcom_timer_callback(struct hrtimer *timer)
{
	struct sharp_memory_device *sdev = container_of(timer,
		struct sharp_memory_device, vcom_timer);

	if (!READ_ONCE(sdev->vcom_timer_active)) {
		return HRTIMER_NORESTART;
	}

	/* EXTCOMIN changes COM polarity on each rising edge. */
	sdev->vcom_setting = sdev->vcom_setting ? 0 : 1;
	gpiod_set_value(sdev->gpio_vcom, sdev->vcom_setting);

	hrtimer_forward_now(&sdev->vcom_timer,
		ns_to_ktime(sharp_drm_vcom_toggle_period_ns(sdev)));

	return HRTIMER_RESTART;
}

static void sharp_drm_clear_and_power_off(struct sharp_memory_device *sdev,
	bool allow_spi_clear)
{
	int i;

	if (sdev) {
		dev_dbg(sdev->drm.dev, "powering off\n");
	}

	// Always clear display on power-off.
	if (sdev && allow_spi_clear) {
		for (i = 0; i < sdev->panel_count; i++) {
			(void)sharp_spi_clear_screen(&sdev->panels[i]);
		}
	}

	if (sdev) {
		sharp_drm_backlight_force_off(sdev);
	}

	/* Turn off power and all signals */
	if (sdev && sdev->gpio_disp) {
		gpiod_set_value_cansleep(sdev->gpio_disp, 0);
	}
	if (sdev) {
		gpiod_set_value(sdev->gpio_vcom, 0);
	}
}

static void sharp_drm_stop_vcom_timer(struct sharp_memory_device *sdev)
{
	if (!sdev || !sdev->vcom_timer_active) {
		return;
	}

	WRITE_ONCE(sdev->vcom_timer_active, false);
	hrtimer_cancel(&sdev->vcom_timer);
	gpiod_set_value(sdev->gpio_vcom, 0);
	sdev->vcom_setting = 0;
}

static void sharp_drm_shutdown_vcom_timer(struct sharp_memory_device *sdev)
{
	if (!sdev) {
		return;
	}

	WRITE_ONCE(sdev->vcom_timer_active, false);
	hrtimer_cancel(&sdev->vcom_timer);
	sdev->vcom_setting = 0;
}

static void sharp_drm_start_vcom_timer(struct sharp_memory_device *sdev)
{
	if (!sdev) {
		return;
	}

	hrtimer_cancel(&sdev->vcom_timer);
	gpiod_set_value(sdev->gpio_vcom, 0);
	sdev->vcom_setting = 0;
	WRITE_ONCE(sdev->vcom_timer_active, true);
	hrtimer_start(&sdev->vcom_timer,
		ns_to_ktime(sharp_drm_vcom_toggle_period_ns(sdev)),
		HRTIMER_MODE_REL);
}

static void sharp_drm_release_secondary_panel(struct sharp_memory_device *sdev)
{
	struct sharp_subpanel *panel;

	if (!sdev) {
		return;
	}

	panel = &sdev->panels[1];

	/*
	 * The auxiliary panel is owned by the SPI core. The primary panel keeps
	 * only a temporary device reference while dual-panel mode is active.
	 */
	if (panel->spi && panel->spi_ref_held) {
		put_device(&panel->spi->dev);
	}
	panel->spi = NULL;
	panel->spi_ref_held = false;
	panel->desired_valid = false;
	panel->displayed_valid = false;
	sdev->panel_count = 1;
	sdev->dual_panel = false;
}

static void sharp_drm_pipe_enable(struct drm_simple_display_pipe *pipe,
	struct drm_crtc_state *crtc_state, struct drm_plane_state *plane_state)
{
	struct sharp_memory_device *sdev;
	int drm_idx;
	int i;

	// Get panel and SPI device structs
	sdev = drm_to_device(pipe->crtc.dev);
	dev_dbg(sdev->drm.dev, "enabling display pipe\n");

	// Enter DRM resource area
	if (!drm_dev_enter(pipe->crtc.dev, &drm_idx)) {
		return;
	}
	if (READ_ONCE(sdev->shutting_down)) {
		goto out_exit;
	}

	// Power up sequence
	if (sdev->gpio_disp) {
		gpiod_set_value_cansleep(sdev->gpio_disp, 1);
	}
	gpiod_set_value(sdev->gpio_vcom, 0);
	sdev->vcom_setting = 0;
	usleep_range(5000, 10000);

	// Clear display
	for (i = 0; i < sdev->panel_count; i++) {
		int rc = sharp_spi_clear_screen(&sdev->panels[i]);

		if (rc) {
			dev_err(sdev->drm.dev, "panel %d clear failed: %d\n", i, rc);
			if (sdev->gpio_disp) {
				// Power down display, VCOM is not running
				gpiod_set_value_cansleep(sdev->gpio_disp, 0);
			}
			goto out_exit;
		}
	}

	sharp_tx_start(sdev);
	sharp_drm_allow_invert_blink(sdev);
	sharp_drm_backlight_restore(sdev);

	sharp_drm_start_vcom_timer(sdev);

out_exit:
	drm_dev_exit(drm_idx);
}

static void sharp_drm_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct sharp_memory_device *sdev;
	bool allow_spi_clear = false;
	int drm_idx;

	// Get panel and SPI device structs
	sdev = drm_to_device(pipe->crtc.dev);
	dev_dbg(sdev->drm.dev, "disabling display pipe\n");

	sharp_drm_disallow_invert_blink(sdev);
	sharp_tx_stop(sdev);

	// Cancel the timer
	sharp_drm_stop_vcom_timer(sdev);

	if (drm_dev_enter(pipe->crtc.dev, &drm_idx)) {
		sharp_drm_clear_and_power_off(sdev, true);
		drm_dev_exit(drm_idx);
		return;
	} else if (sharp_drm_device_is_registered(sdev)) {
		/*
		 * remove() unplugs DRM before atomic shutdown, so drm_dev_enter()
		 * is closed while the SPI devices are still retained by the driver.
		 * The registry check prevents this fallback after device teardown.
		 */
		allow_spi_clear = true;
	}

	sharp_drm_clear_and_power_off(sdev, allow_spi_clear);
}

static void sharp_drm_pipe_update(struct drm_simple_display_pipe *pipe,
				struct drm_plane_state *old_state)
{
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_rect rect;
	int rc;

	if (!pipe->crtc.state->active) {
		return;
	}

	if (drm_atomic_helper_damage_merged(old_state, state, &rect)) {
		rc = sharp_drm_fb_dirty(state->fb, &rect);
		if (rc && rc != -ENODEV && rc != -ESHUTDOWN) {
			dev_warn_ratelimited(pipe->crtc.dev->dev,
				"framebuffer conversion failed: %d\n", rc);
		}
	}
}

static const struct drm_simple_display_pipe_funcs sharp_drm_pipe_funcs = {
	.enable = sharp_drm_pipe_enable,
	.disable = sharp_drm_pipe_disable,
	.update = sharp_drm_pipe_update
	// .prepare_fb and .cleanup_fb are handled automatically when not set
};

static int sharp_drm_connector_get_modes(struct drm_connector *connector)
{
	struct sharp_memory_device *sdev = drm_to_device(connector->dev);

	return drm_connector_helper_get_modes_fixed(connector, sdev->mode);
}

static const struct drm_connector_helper_funcs sharp_drm_connector_hfuncs = {
	.get_modes = sharp_drm_connector_get_modes,
};

static const struct drm_connector_funcs sharp_drm_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_mode_config_funcs sharp_drm_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static const struct drm_mode_config_helper_funcs sharp_drm_mode_config_hfuncs = {
	/* Panel RAM is accessible only after the pipe has been enabled. */
	.atomic_commit_tail = drm_atomic_helper_commit_tail_rpm,
};

static const uint32_t sharp_drm_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_NV12,
};

/* Sharp LS027B7DH01 / JDI LPM027M128B / JDI LPM027M128C */
static const struct drm_display_mode sharp_drm_ls027b7dh01_mode = {
	DRM_MODE_INIT(60, 400, 240, 59, 35),
};

/* U340QBN01 */
static const struct drm_display_mode sharp_drm_u340qbn01_mode = {
	DRM_MODE_INIT(60, 272, 451, 45, 74),
};

/* U340QBN01 x2 (dual panel) */
static const struct drm_display_mode sharp_drm_u340qbn01_dual_mode = {
	DRM_MODE_INIT(60, 544, 451, 90, 74),
};

/* JDI LPM044M141A */
static const struct drm_display_mode sharp_drm_lpm044m141a_mode = {
	DRM_MODE_INIT(60, 640, 480, 90, 67),
};

/* Sharp LS044Q7DH01 */
static const struct drm_display_mode sharp_drm_ls044q7dh01_mode = {
	DRM_MODE_INIT(60, 320, 240, 90, 67),
};

struct sharp_drm_mode_desc {
	u32 width;
	u32 height;
	const struct drm_display_mode *mode;
};

static const struct sharp_drm_mode_desc sharp_drm_modes[] = {
	{ 400, 240, &sharp_drm_ls027b7dh01_mode },
	{ 272, 451, &sharp_drm_u340qbn01_mode },
	{ 640, 480, &sharp_drm_lpm044m141a_mode },
	{ 320, 240, &sharp_drm_ls044q7dh01_mode },
};

static enum sharp_panel_type sharp_drm_read_panel_type(struct device *dev)
{
	const char *panel_type = NULL;

	if (device_property_read_string(dev, "panel-type", &panel_type)) {
		return SHARP_PANEL_TYPE_JDI;
	}

	if (!strcmp(panel_type, "sharp_mono")) {
		return SHARP_PANEL_TYPE_SHARP_MONO;
	}

	if (!strcmp(panel_type, "jdi")) {
		return SHARP_PANEL_TYPE_JDI;
	}

	dev_warn(dev, "Unsupported panel-type=\"%s\", using jdi\n", panel_type);
	return SHARP_PANEL_TYPE_JDI;
}

static const struct sharp_drm_mode_desc *sharp_drm_find_mode(u32 width, u32 height)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(sharp_drm_modes); i++) {
		if (sharp_drm_modes[i].width == width
			&& sharp_drm_modes[i].height == height) {
			return &sharp_drm_modes[i];
		}
	}

	return NULL;
}

static const struct drm_display_mode *sharp_drm_read_single_mode(struct device *dev,
	bool has_width, bool has_height, u32 width, u32 height)
{
	const struct sharp_drm_mode_desc *desc;

	if (!has_width && !has_height) {
		return &sharp_drm_ls027b7dh01_mode;
	}

	if (!has_width || !has_height) {
		dev_warn(dev, "Only one of width/height provided, using 400x240\n");
		return &sharp_drm_ls027b7dh01_mode;
	}

	if (!width || !height) {
		dev_warn(dev, "Invalid width/height %ux%u, using 400x240\n",
			width, height);
		return &sharp_drm_ls027b7dh01_mode;
	}

	desc = sharp_drm_find_mode(width, height);
	if (desc) {
		return desc->mode;
	}

	dev_warn(dev, "Unsupported mode %ux%u, using 400x240\n", width, height);
	return &sharp_drm_ls027b7dh01_mode;
}

static int sharp_drm_read_dimensions(struct device *dev, bool dual_panel,
	u32 *panel_width, u32 *panel_height, const struct drm_display_mode **out_mode)
{
	u32 width;
	u32 height;
	bool has_width;
	bool has_height;
	const struct drm_display_mode *mode;

	if (!panel_width || !panel_height || !out_mode) {
		return -EINVAL;
	}

	has_width = !device_property_read_u32(dev, "width", &width);
	has_height = !device_property_read_u32(dev, "height", &height);

	if (dual_panel) {
		if (!has_width && !has_height) {
			width = 272;
			height = 451;
		} else if (!has_width || !has_height) {
			dev_err(dev, "dual-panel requires both width and height\n");
			return -EINVAL;
		}

		if (width != 272 || height != 451) {
			dev_err(dev, "dual-panel only supports 272x451 (got %ux%u)\n",
				width, height);
			return -EINVAL;
		}

		*panel_width = width;
		*panel_height = height;
		*out_mode = &sharp_drm_u340qbn01_dual_mode;
		return 0;
	}

	mode = sharp_drm_read_single_mode(dev, has_width, has_height, width, height);
	*out_mode = mode;
	*panel_width = mode->hdisplay;
	*panel_height = mode->vdisplay;

	return 0;
}

DEFINE_DRM_GEM_DMA_FOPS(sharp_drm_fops);

static int sharp_drm_ioctl_redraw(struct drm_device *dev, void *data,
	struct drm_file *file)
{
	(void)data;
	(void)file;

	return sharp_drm_redraw_fb(dev, -1);
}

static const struct drm_ioctl_desc sharp_drm_ioctls[] = {
	DRM_IOCTL_DEF_DRV_REDRAW,
};

static struct drm_gem_object *sharp_drm_gem_create_object(
	struct drm_device *drm, size_t size)
{
	struct drm_gem_dma_object *dma_obj;

	(void)drm;
	(void)size;
	dma_obj = kzalloc(sizeof(*dma_obj), GFP_KERNEL);
	if (!dma_obj) {
		return ERR_PTR(-ENOMEM);
	}
	/* Imported buffers retain their exporter's mapping and cache policy. */
	dma_obj->map_noncoherent = g_cacheable_buffers;
	return &dma_obj->base;
}

static const struct drm_driver sharp_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops = &sharp_drm_fops,
	DRM_GEM_DMA_DRIVER_OPS_VMAP,
	DRM_FBDEV_DMA_DRIVER_OPS,
	.gem_create_object = sharp_drm_gem_create_object,
	.name = "sharp_drm",
	.desc = "Sharp Memory LCD panel",
	.major = 1,
	.minor = 1,

	.ioctls = sharp_drm_ioctls,
	.num_ioctls = ARRAY_SIZE(sharp_drm_ioctls)
};

static void sharp_drm_init_line_lens(struct sharp_subpanel *panel)
{
	size_t plane_len = (size_t)((panel->width * 3 + 7) / 8);

	panel->line_len_mono = 2 + (panel->width / 8);
	panel->line_len_color8 = 2 + plane_len;
	panel->line_len_color64 = 4 + (plane_len * 2);
	panel->max_line_len = panel->line_len_color64;
}

static int sharp_drm_setup_dma(struct device *dev)
{
	int ret;

	// The SPI device is used to allocate DMA memory
	if (!dev->coherent_dma_mask) {
		ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
		if (ret) {
			dev_warn(dev, "Failed to set dma mask %d\n", ret);
			return ret;
		}
	}

	return 0;
}

static struct sharp_memory_device *sharp_drm_alloc_device(struct device *dev)
{
	struct sharp_memory_device *sdev;

	sdev = devm_drm_dev_alloc(dev, &sharp_drm_driver,
		struct sharp_memory_device, drm);
	if (IS_ERR(sdev)) {
		dev_err(dev, "failed to allocate DRM device\n");
	}

	return sdev;
}

static int sharp_drm_init_gpios(struct device *dev, struct sharp_memory_device *sdev)
{
	sdev->gpio_disp = devm_gpiod_get_optional(dev, "disp", GPIOD_OUT_HIGH);
	if (IS_ERR(sdev->gpio_disp)) {
		return dev_err_probe(dev, PTR_ERR(sdev->gpio_disp),
			"Failed to get GPIO 'disp'\n");
	}

	sdev->gpio_vcom = devm_gpiod_get(dev, "vcom", GPIOD_OUT_LOW);
	if (IS_ERR(sdev->gpio_vcom)) {
		return dev_err_probe(dev, PTR_ERR(sdev->gpio_vcom),
			"Failed to get GPIO 'vcom'\n");
	}
	if (gpiod_cansleep(sdev->gpio_vcom)) {
		return dev_err_probe(dev, -EINVAL,
			"GPIO 'vcom' must support non-sleeping timer access\n");
	}

	return 0;
}

static int sharp_drm_init_mode_config(struct sharp_memory_device *sdev)
{
	struct drm_device *drm = &sdev->drm;
	int ret;

	ret = drmm_mode_config_init(drm);
	if (ret) {
		return ret;
	}
	drm->mode_config.funcs = &sharp_drm_mode_config_funcs;
	drm->mode_config.helper_private = &sharp_drm_mode_config_hfuncs;

	return 0;
}

static void sharp_drm_init_subpanel(struct sharp_subpanel *panel,
	struct spi_device *spi, u32 width, u32 height, unsigned int x_offset,
	enum sharp_panel_type panel_type)
{
	panel->spi = spi;
	panel->width = width;
	panel->height = height;
	panel->x_offset = x_offset;
	panel->panel_type = panel_type;
	sharp_drm_init_line_lens(panel);
	sharp_render_init_nv12_tables(panel);

	if (panel->panel_type == SHARP_PANEL_TYPE_SHARP_MONO && panel->height > 255) {
		dev_warn(&spi->dev,
			"panel-type=sharp_mono height %u exceeds 255; line addressing will wrap\n",
			panel->height);
	}
}

static int sharp_drm_init_buffers(struct drm_device *drm, struct sharp_subpanel *panel)
{
	size_t buf_len;
	size_t row_store_len;
	size_t err_len;
	int ret;

	ret = drmm_mutex_init(drm, &panel->cmd_lock);
	if (ret) {
		return ret;
	}

	if (check_mul_overflow((size_t)panel->width, (size_t)panel->height, &buf_len)) {
		return -EINVAL;
	}
	if (check_mul_overflow((size_t)panel->height, panel->max_line_len,
		&row_store_len)) {
		return -EINVAL;
	}
	if (check_mul_overflow((size_t)panel->width, (size_t)3, &err_len)
		|| check_mul_overflow(err_len, (size_t)2, &err_len)) {
		return -EINVAL;
	}

	panel->row_store_len = row_store_len;
	panel->cmd_buf = drmm_kzalloc(drm, 2, GFP_KERNEL);
	panel->buf = drmm_kzalloc(drm, buf_len, GFP_KERNEL);
	panel->desired = drmm_kzalloc(drm, panel->row_store_len, GFP_KERNEL);
	panel->displayed = drmm_kzalloc(drm, panel->row_store_len, GFP_KERNEL);
	panel->displayed_line_len = drmm_kcalloc(drm, panel->height,
		sizeof(*panel->displayed_line_len), GFP_KERNEL);
	panel->row_generation = drmm_kcalloc(drm, panel->height,
		sizeof(*panel->row_generation), GFP_KERNEL);
	panel->queued_generation = drmm_kcalloc(drm, panel->height,
		sizeof(*panel->queued_generation), GFP_KERNEL);
	panel->dirty_rows = drmm_kcalloc(drm, BITS_TO_LONGS(panel->height),
		sizeof(*panel->dirty_rows), GFP_KERNEL);
	panel->displayed_rows = drmm_kcalloc(drm, BITS_TO_LONGS(panel->height),
		sizeof(*panel->displayed_rows), GFP_KERNEL);
	panel->dither_err = drmm_kcalloc(drm, err_len, sizeof(*panel->dither_err),
		GFP_KERNEL);
	panel->dither_line_lo = drmm_kcalloc(drm, panel->width,
		sizeof(*panel->dither_line_lo), GFP_KERNEL);
	panel->dither_line_hi = drmm_kcalloc(drm, panel->width,
		sizeof(*panel->dither_line_hi), GFP_KERNEL);
	panel->source_row = drmm_kcalloc(drm, panel->width,
		sizeof(*panel->source_row), GFP_KERNEL);
	if (!panel->cmd_buf || !panel->buf
		|| !panel->desired || !panel->displayed
		|| !panel->displayed_line_len || !panel->row_generation
		|| !panel->queued_generation || !panel->dirty_rows
		|| !panel->displayed_rows
		|| !panel->dither_err || !panel->dither_line_lo
		|| !panel->dither_line_hi || !panel->source_row) {
		return -ENOMEM;
	}

	return 0;
}

static void sharp_drm_init_subpanel_state(struct sharp_subpanel *panel)
{
	panel->desired_valid = false;
	panel->displayed_valid = false;
	panel->desired_line_len = 0;
	panel->desired_params = sharp_render_params_snapshot();
}

static int sharp_drm_init_device_state(struct sharp_memory_device *sdev)
{
	int ret;

	sdev->vcom_timer_active = false;
	sdev->vcom_setting = 0;
	hrtimer_setup(&sdev->vcom_timer, vcom_timer_callback, CLOCK_MONOTONIC,
		HRTIMER_MODE_REL);
	ret = sharp_drm_control_init(sdev);
	if (ret) {
		return ret;
	}
	ret = drmm_mutex_init(&sdev->drm, &sdev->fb_lock);
	if (ret) {
		return ret;
	}
	ret = drmm_mutex_init(&sdev->drm, &sdev->tx_control_lock);
	if (ret) {
		return ret;
	}
	ret = sharp_drm_backlight_state_init(sdev);
	if (ret) {
		return ret;
	}

	return 0;
}

static void sharp_drm_set_mode_limits(struct drm_device *drm,
	const struct drm_display_mode *mode)
{
	drm->mode_config.min_width = mode->hdisplay;
	drm->mode_config.max_width = mode->hdisplay;
	drm->mode_config.min_height = mode->vdisplay;
	drm->mode_config.max_height = mode->vdisplay;
}

static int sharp_drm_setup_connector(struct drm_device *drm,
	struct sharp_memory_device *sdev)
{
	int ret;

	ret = drmm_connector_init(drm, &sdev->connector, &sharp_drm_connector_funcs,
		DRM_MODE_CONNECTOR_SPI, NULL);
	if (ret) {
		return ret;
	}
	drm_connector_helper_add(&sdev->connector, &sharp_drm_connector_hfuncs);

	return 0;
}

static int sharp_drm_setup_pipe(struct drm_device *drm,
	struct sharp_memory_device *sdev)
{
	int ret;

	ret = drm_simple_display_pipe_init(drm, &sdev->pipe, &sharp_drm_pipe_funcs,
		sharp_drm_formats, ARRAY_SIZE(sharp_drm_formats),
		NULL, &sdev->connector);
	if (ret) {
		return ret;
	}

	// Enable damaged screen area clips
	drm_plane_enable_fb_damage_clips(&sdev->pipe.plane);
	drm_mode_config_reset(drm);

	return 0;
}

static int sharp_drm_register_drm(struct spi_device *spi, struct drm_device *drm)
{
	int ret;

	ret = drm_dev_register(drm, 0);
	if (ret) {
		return ret;
	}

	spi_set_drvdata(spi, drm);
	drm_client_setup_with_fourcc(drm, DRM_FORMAT_XRGB8888);

	return 0;
}

static u32 sharp_drm_spi_chip_select(const struct spi_device *spi)
{
	return spi->chip_select[0];
}

int sharp_drm_probe(struct spi_device *spi)
{
	struct device *dev;
	struct sharp_memory_device *sdev;
	struct drm_device *drm;
	struct spi_device *secondary_spi = NULL;
	struct device_link *secondary_link = NULL;
	const struct drm_display_mode *mode;
	enum sharp_panel_type panel_type;
	bool dual_panel;
	u32 panel_width;
	u32 panel_height;
	int ret;

	// Get DRM device from SPI struct
	dev = &spi->dev;
	panel_type = sharp_drm_read_panel_type(dev);
	dual_panel = device_property_read_bool(dev, "dual-panel");

	if (dual_panel && panel_type == SHARP_PANEL_TYPE_SHARP_MONO) {
		dev_err(dev, "dual-panel does not support panel-type=sharp_mono\n");
		return -EINVAL;
	}
	ret = sharp_drm_setup_dma(dev);
	if (ret) {
		return ret;
	}

	// Allocate panel storage
	sdev = sharp_drm_alloc_device(dev);
	if (IS_ERR(sdev)) {
		return PTR_ERR(sdev);
	}
	ret = sharp_drm_init_device_state(sdev);
	if (ret) {
		return ret;
	}
	sdev->dual_panel = dual_panel;
	sdev->panel_count = dual_panel ? 2 : 1;
	sdev->panels[0].panel_type = panel_type;

	if (panel_type == SHARP_PANEL_TYPE_JDI) {
		dev_info(dev, "EXTCOMIN software frequency=%llu Hz\n",
			SHARP_VCOM_JDI_EXTCOMIN_HZ);
	}

	// Initialize GPIO
	ret = sharp_drm_init_gpios(dev, sdev);
	if (ret) {
		return ret;
	}
	ret = sharp_drm_init_backlight(dev, sdev);
	if (ret) {
		goto out_unregister_secondary;
	}

	// Initalize DRM mode
	ret = sharp_drm_init_mode_config(sdev);
	if (ret) {
		goto out_unregister_secondary;
	}
	drm = &sdev->drm;

	ret = sharp_drm_read_dimensions(dev, dual_panel, &panel_width, &panel_height, &mode);
	if (ret) {
		goto out_unregister_secondary;
	}
	sdev->mode = mode;
	sdev->logical_width = mode->hdisplay;
	sdev->logical_height = mode->vdisplay;

	// Initialize primary panel contents
	sharp_drm_init_subpanel(&sdev->panels[0], spi, panel_width, panel_height,
		0, panel_type);
	// Allocate reused heap buffers suitable for SPI source
	ret = sharp_drm_init_buffers(drm, &sdev->panels[0]);
	if (ret) {
		goto out_unregister_secondary;
	}
	sharp_drm_init_subpanel_state(&sdev->panels[0]);

	if (dual_panel) {
		secondary_spi = sharp_drm_secondary_spi_get();
		if (!secondary_spi) {
			dev_info(dev, "dual_panel: secondary SPI device not ready, deferring\n");
			ret = -EPROBE_DEFER;
			goto out_unregister_secondary;
		}
		dev_info(dev, "dual_panel: using secondary SPI (CS%u)\n",
			sharp_drm_spi_chip_select(secondary_spi));
		sdev->panels[1].spi = secondary_spi;
		sdev->panels[1].spi_ref_held = true;
		secondary_link = device_link_add(dev, &secondary_spi->dev,
			DL_FLAG_AUTOREMOVE_CONSUMER);
		if (!secondary_link) {
			dev_err(dev, "dual_panel: failed to link secondary SPI device\n");
			ret = -ENOMEM;
			goto out_unregister_secondary;
		}
		ret = sharp_drm_setup_dma(&secondary_spi->dev);
		if (ret) {
			dev_err(dev, "dual_panel: failed to setup DMA: %d\n", ret);
			goto out_unregister_secondary;
		}
		sharp_drm_init_subpanel(&sdev->panels[1], secondary_spi, panel_width,
			panel_height, panel_width, panel_type);
		ret = sharp_drm_init_buffers(drm, &sdev->panels[1]);
		if (ret) {
			dev_err(dev, "dual_panel: failed to init buffers: %d\n", ret);
			goto out_unregister_secondary;
		}
		sharp_drm_init_subpanel_state(&sdev->panels[1]);
	}

	ret = sharp_tx_init(sdev);
	if (ret) {
		dev_err(dev, "failed to initialize TX pipeline: %d\n", ret);
		goto out_unregister_secondary;
	}

	// DRM mode settings
	sharp_drm_set_mode_limits(drm, mode);

	// Configure DRM connector
	ret = sharp_drm_setup_connector(drm, sdev);
	if (ret) {
		goto out_unregister_secondary;
	}

	// Initialize DRM pipe
	ret = sharp_drm_setup_pipe(drm, sdev);
	if (ret) {
		goto out_unregister_secondary;
	}

	/* All control-visible state is initialized before publishing the device. */
	ret = sharp_drm_register_device(sdev);
	if (ret) {
		goto out_unregister_secondary;
	}
	ret = sharp_drm_register_drm(spi, drm);
	if (ret) {
		sharp_drm_unregister_device(sdev);
		goto out_unregister_secondary;
	}
	dev_info(dev, "dual_panel=%d, panel_type=%d, colors=%d\n",
		dual_panel, panel_type, params_current_colors());
	sharp_perf_debugfs_init(sdev);

	return 0;

out_unregister_secondary:
	sharp_drm_backlight_force_off(sdev);
	sharp_drm_cancel_backlight_transition(sdev);
	if (secondary_link) {
		device_link_del(secondary_link);
	}
	if (secondary_spi) {
		sharp_drm_release_secondary_panel(sdev);
	}
	return ret;
}

void sharp_drm_remove(struct spi_device *spi)
{
	struct drm_device *drm;
	struct sharp_memory_device *sdev;

	// Get DRM and panel device from SPI
	drm = spi_get_drvdata(spi);
	if (!drm) {
		return;
	}
	sdev = drm_to_device(drm);
	dev_dbg(drm->dev, "removing DRM device\n");
	sharp_perf_debugfs_cleanup(sdev);
	WRITE_ONCE(sdev->shutting_down, true);
	drm_dev_unplug(drm);
	sharp_drm_disallow_invert_blink(sdev);
	drm_atomic_helper_shutdown(drm);
	sharp_tx_shutdown(sdev);
	sharp_drm_backlight_force_off(sdev);
	sharp_drm_cancel_backlight_transition(sdev);
	sharp_drm_shutdown_vcom_timer(sdev);
	sharp_drm_release_secondary_panel(sdev);
	sharp_drm_unregister_device(sdev);
}

void sharp_drm_shutdown(struct spi_device *spi)
{
	struct drm_device *drm;
	struct sharp_memory_device *sdev;

	if (!spi) {
		return;
	}

	drm = spi_get_drvdata(spi);
	if (!drm) {
		return;
	}

	sdev = drm_to_device(drm);
	WRITE_ONCE(sdev->shutting_down, true);
	sharp_drm_disallow_invert_blink(sdev);
	drm_atomic_helper_shutdown(drm);
	sharp_tx_shutdown(sdev);
	sharp_drm_backlight_force_off(sdev);
	sharp_drm_cancel_backlight_transition(sdev);
	sharp_drm_shutdown_vcom_timer(sdev);
}
