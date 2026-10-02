// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/device.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/ctype.h>
#include <linux/kernel.h>
#include <linux/overflow.h>
#include <linux/property.h>
#include <linux/string.h>

#include "sharp_drm.h"
#include "params_internal.h"

int g_param_mono_cutoff = 32;
int g_param_mono_invert = 0;
int g_param_colors = 8;
int g_param_display_clear = 0;
int g_param_display_blink = 0;
int g_param_display_invert = 0;
int g_param_dither_algo = SHARP_DITHER_ALGO_27COLORS;
int g_param_dither_algo_user = 0;
int g_param_blue_noise_2197 = 0;

static void params_set_default_dither_for_colors(u32 colors)
{
	if (g_param_dither_algo_user) {
		return;
	}

	if (colors == 2) {
		g_param_dither_algo = SHARP_DITHER_ALGO_NONE;
	} else if (colors == 8) {
		g_param_dither_algo = SHARP_DITHER_ALGO_27COLORS;
	} else if (colors == 64) {
		g_param_dither_algo = SHARP_DITHER_ALGO_343COLORS;
	}
}

void params_read_colors(struct device *dev, bool sharp_mono)
{
	u32 colors;

	if (sharp_mono) {
		if (!device_property_read_u32(dev, "colors", &colors) && colors != 2) {
			dev_warn(dev, "panel-type=sharp_mono forces colors=2 (got %u)\n",
				colors);
		}
		g_param_colors = 2;
		params_set_default_dither_for_colors(2);
		return;
	}

	if (!device_property_read_u32(dev, "colors", &colors)) {
		if (colors == 2 || colors == 8 || colors == 64) {
			g_param_colors = (int)colors;
			params_set_default_dither_for_colors(colors);
		} else {
			dev_warn(dev, "Unsupported colors=%u, using %d\n",
				colors, g_param_colors);
		}
	}
}

void params_force_colors(struct device *dev, int colors, const char *reason)
{
	if (g_param_colors != colors) {
		dev_warn(dev, "%s forces colors=%d (got %d)\n",
			reason, colors, g_param_colors);
		g_param_colors = colors;
		params_set_default_dither_for_colors((u32)colors);
	}
}

int params_current_colors(void)
{
	return g_param_colors;
}

static int set_param_u8(const char *val, const struct kernel_param *kp)
{
	int rc, result;
	struct sharp_memory_device *lock;

	// Parse string value
	if ((rc = kstrtoint(val, 10, &result)) || (result < 0) || (result > 0xff)) {
		return -EINVAL;
	}

	lock = sharp_drm_render_params_lock();
	rc = param_set_int(val, kp);
	sharp_drm_render_params_unlock(lock);

	return rc;
}

static const struct kernel_param_ops u8_param_ops = {
	.set = set_param_u8,
	.get = param_get_int,
};

static int set_param_panel_type(const char *val, const struct kernel_param *kp)
{
	(void)val;
	(void)kp;
	return -EINVAL;
}

static int get_param_panel_type(char *buffer, const struct kernel_param *kp)
{
	(void)kp;
	return scnprintf(buffer, PAGE_SIZE, "%s\n", sharp_drm_panel_type_name());
}

static const struct kernel_param_ops panel_type_param_ops = {
	.set = set_param_panel_type,
	.get = get_param_panel_type,
};

static int set_param_colors(const char *val, const struct kernel_param *kp)
{
	int rc;
	int result;
	struct sharp_memory_device *lock;

	rc = kstrtoint(val, 10, &result);
	if (rc) {
		return rc;
	}

	if (result != 2 && result != 8 && result != 64) {
		return -EINVAL;
	}

	lock = sharp_drm_render_params_lock();
	rc = sharp_drm_validate_colors(result);
	if (rc) {
		sharp_drm_render_params_unlock(lock);
		return rc;
	}

	rc = param_set_int(val, kp);
	if (rc) {
		sharp_drm_render_params_unlock(lock);
		return rc;
	}

	g_param_dither_algo_user = 0;
	if (result == 2) {
		g_param_dither_algo = SHARP_DITHER_ALGO_NONE;
	} else if (result == 8) {
		g_param_dither_algo = SHARP_DITHER_ALGO_27COLORS;
	} else if (result == 64) {
		g_param_dither_algo = SHARP_DITHER_ALGO_343COLORS;
	}

	sharp_drm_render_params_unlock(lock);

	return 0;
}

static const struct kernel_param_ops colors_param_ops = {
	.set = set_param_colors,
	.get = param_get_int,
};

static int set_param_dither_algo(const char *val, const struct kernel_param *kp)
{
	int rc;
	int result;
	struct sharp_memory_device *lock;

	rc = kstrtoint(val, 10, &result);
	if (rc) {
		return rc;
	}
	if (result < 0 || result >= SHARP_DITHER_ALGO_COUNT) {
		return -EINVAL;
	}

	lock = sharp_drm_render_params_lock();
	rc = param_set_int(val, kp);
	if (!rc) {
		g_param_dither_algo_user = 1;
	}
	sharp_drm_render_params_unlock(lock);
	return rc;
}

static const struct kernel_param_ops dither_algo_param_ops = {
	.set = set_param_dither_algo,
	.get = param_get_int,
};

static int set_param_bool(const char *val, const struct kernel_param *kp)
{
	int rc;
	int result;
	struct sharp_memory_device *lock;

	rc = kstrtoint(val, 10, &result);
	if (rc) {
		return rc;
	}
	if (result < 0 || result > 1) {
		return -EINVAL;
	}

	lock = sharp_drm_render_params_lock();
	rc = param_set_int(val, kp);
	sharp_drm_render_params_unlock(lock);

	return rc;
}

static const struct kernel_param_ops bool_param_ops = {
	.set = set_param_bool,
	.get = param_get_int,
};

static int set_param_display_clear(const char *val, const struct kernel_param *kp)
{
	int rc, result;

	rc = kstrtoint(val, 10, &result);
	if (rc) {
		return rc;
	}
	if (result < 0 || result > 1) {
		return -EINVAL;
	}

	if (result) {
		rc = sharp_drm_display_clear();
		if (rc) {
			return rc;
		}
	}

	*((int *)kp->arg) = 0;

	return 0;
}

static const struct kernel_param_ops display_clear_param_ops = {
	.set = set_param_display_clear,
	.get = param_get_int,
};

static int set_param_display_blink(const char *val, const struct kernel_param *kp)
{
	int rc, result;

	rc = kstrtoint(val, 10, &result);
	if (rc) {
		return rc;
	}
	if (result < 0 || result > 2) {
		return -EINVAL;
	}

	rc = sharp_drm_display_blink(result);
	if (rc) {
		return rc;
	}

	*((int *)kp->arg) = result;

	return 0;
}

static const struct kernel_param_ops display_blink_param_ops = {
	.set = set_param_display_blink,
	.get = param_get_int,
};


static int set_param_display_invert(const char *val, const struct kernel_param *kp)
{
	int rc, result;
	const char *p;
	unsigned int duration_ms;
	unsigned int interval_ms;

	if (strchr(val, ',')) {
		rc = parse_seconds_ms(val, &p, &duration_ms);
		if (rc) {
			return rc;
		}
		if (*p != ',') {
			return -EINVAL;
		}
		p++;
		rc = parse_seconds_ms(p, &p, &interval_ms);
		if (rc) {
			return rc;
		}
		if (*p != '\0') {
			return -EINVAL;
		}

		rc = sharp_drm_display_invert_blink(duration_ms, interval_ms);
		if (rc) {
			return rc;
		}

		*((int *)kp->arg) = 0;
		return 0;
	}

	rc = kstrtoint(val, 10, &result);
	if (rc) {
		return rc;
	}
	if (result < 0 || result > 1) {
		return -EINVAL;
	}

	rc = sharp_drm_display_invert(result);
	if (rc) {
		return rc;
	}

	*((int *)kp->arg) = result;

	return 0;
}

static const struct kernel_param_ops display_invert_param_ops = {
	.set = set_param_display_invert,
	.get = param_get_int,
};

module_param_cb(mono_cutoff, &u8_param_ops, &g_param_mono_cutoff, 0664);
MODULE_PARM_DESC(mono_cutoff,
	"Mono threshold from 0-255; ordered dither center (128 is neutral, default 32)");

module_param_cb(mono_invert, &bool_param_ops, &g_param_mono_invert, 0664);
MODULE_PARM_DESC(mono_invert, "0 for no inversion, 1 for inversion");

module_param_cb(colors, &colors_param_ops, &g_param_colors, 0664);
MODULE_PARM_DESC(colors, "2 for mono, 8 for 3-bit color, 64 for 6-bit color (default 8)");

module_param_cb(panel_type, &panel_type_param_ops, NULL, 0444);
MODULE_PARM_DESC(panel_type, "Panel type (read-only): jdi or sharp_mono");

module_param_cb(dither_algo, &dither_algo_param_ops, &g_param_dither_algo, 0664);
MODULE_PARM_DESC(dither_algo,
	"0: off, 1: mono ordered 4x4 (2-color only), "
	"2: 27colors (8-color only, default for 8-color), "
	"3: 125colors (8-color only), 4: 343colors (64-color only, default for 64-color), "
	"5: 2197colors (64-color only), 6: errdiff2 (8/64-color only)");

module_param_cb(blue_noise_2197, &bool_param_ops, &g_param_blue_noise_2197, 0664);
MODULE_PARM_DESC(blue_noise_2197, "0: off (default), 1: on. Applies to 2197colors");

module_param_cb(display_clear, &display_clear_param_ops, &g_param_display_clear, 0664);
MODULE_PARM_DESC(display_clear,
	"Write 1 to fill the active framebuffer with black and redraw it (one-shot)");

module_param_cb(display_blink, &display_blink_param_ops, &g_param_display_blink, 0664);
MODULE_PARM_DESC(display_blink, "0=off, 1=blink black, 2=blink white");

module_param_cb(display_invert, &display_invert_param_ops, &g_param_display_invert, 0664);
MODULE_PARM_DESC(display_invert, "0=normal, 1=invert display colors");

int params_set_mono_invert(int setting)
{
	struct sharp_memory_device *lock;

	if (setting < 0 || setting > 1) {
		return -EINVAL;
	}

	lock = sharp_drm_render_params_lock();
	g_param_mono_invert = setting;
	sharp_drm_render_params_unlock(lock);

	return 0;
}
