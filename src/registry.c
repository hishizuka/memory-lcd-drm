// SPDX-License-Identifier: GPL-2.0-or-later
/* Device publication and serialization with module parameter writers. */

#include "sharp_drm.h"
#include <linux/device.h>

struct sharp_drm_registry {
	struct mutex lock;
	struct sharp_memory_device *device;
	struct spi_device *secondary_spi;
};

static struct sharp_drm_registry g_registry = {
	.lock = __MUTEX_INITIALIZER(g_registry.lock),
};

static struct sharp_memory_device *sharp_drm_registry_device_locked(void)
{
	return g_registry.device;
}

struct sharp_memory_device *sharp_drm_device_get(void)
{
	struct sharp_memory_device *sdev;

	mutex_lock(&g_registry.lock);
	sdev = sharp_drm_registry_device_locked();
	if (sdev) {
		drm_dev_get(&sdev->drm);
	}
	mutex_unlock(&g_registry.lock);

	return sdev;
}

void sharp_drm_device_put(struct sharp_memory_device *sdev)
{
	if (sdev) {
		drm_dev_put(&sdev->drm);
	}
}

static void sharp_drm_enforce_colors(struct device *dev,
	struct sharp_memory_device *sdev);

int sharp_drm_register_device(struct sharp_memory_device *sdev)
{
	int ret = 0;

	mutex_lock(&g_registry.lock);
	if (g_registry.device) {
		ret = g_registry.device == sdev ? 0 : -EBUSY;
	} else {
		/* Apply panel constraints and publish under the parameter writer lock. */
		params_read_colors(sdev->drm.dev,
			sdev->panels[0].panel_type == SHARP_PANEL_TYPE_SHARP_MONO);
		sharp_drm_enforce_colors(sdev->drm.dev, sdev);
		g_registry.device = sdev;
	}
	mutex_unlock(&g_registry.lock);

	return ret;
}

void sharp_drm_unregister_device(struct sharp_memory_device *sdev)
{
	mutex_lock(&g_registry.lock);
	if (g_registry.device == sdev) {
		g_registry.device = NULL;
	}
	mutex_unlock(&g_registry.lock);
}

bool sharp_drm_device_is_registered(struct sharp_memory_device *sdev)
{
	bool registered;

	mutex_lock(&g_registry.lock);
	registered = sharp_drm_registry_device_locked() == sdev;
	mutex_unlock(&g_registry.lock);

	return registered;
}

static const char *sharp_drm_panel_type_to_name(enum sharp_panel_type panel_type)
{
	switch (panel_type) {
	case SHARP_PANEL_TYPE_SHARP_MONO:
		return "sharp_mono";
	case SHARP_PANEL_TYPE_JDI:
		return "jdi";
	default:
		return "unknown";
	}
}

struct sharp_memory_device *sharp_drm_render_params_lock(void)
{
	struct sharp_memory_device *sdev;

	mutex_lock(&g_registry.lock);
	sdev = sharp_drm_registry_device_locked();

	if (sdev) {
		mutex_lock(&sdev->tx_control_lock);
		mutex_lock(&sdev->fb_lock);
	}
	return sdev;
}

void sharp_drm_render_params_unlock(struct sharp_memory_device *sdev)
{
	if (sdev) {
		mutex_unlock(&sdev->fb_lock);
		mutex_unlock(&sdev->tx_control_lock);
	}
	mutex_unlock(&g_registry.lock);
}

const char *sharp_drm_panel_type_name(void)
{
	struct sharp_memory_device *sdev;
	const char *name = "unknown";

	mutex_lock(&g_registry.lock);
	sdev = sharp_drm_registry_device_locked();
	if (sdev) {
		name = sharp_drm_panel_type_to_name(sdev->panels[0].panel_type);
	}
	mutex_unlock(&g_registry.lock);

	return name;
}

void sharp_drm_register_secondary_spi(struct spi_device *spi)
{
	if (!spi) {
		return;
	}

	mutex_lock(&g_registry.lock);
	if (g_registry.secondary_spi && g_registry.secondary_spi != spi) {
		dev_warn(&spi->dev, "replacing secondary SPI device (CS%u)\n",
			spi->chip_select[0]);
	}
	g_registry.secondary_spi = spi;
	mutex_unlock(&g_registry.lock);
}

void sharp_drm_unregister_secondary_spi(struct spi_device *spi)
{
	mutex_lock(&g_registry.lock);
	if (g_registry.secondary_spi == spi) {
		g_registry.secondary_spi = NULL;
	}
	mutex_unlock(&g_registry.lock);
}

struct spi_device *sharp_drm_secondary_spi_get(void)
{
	struct spi_device *spi;

	mutex_lock(&g_registry.lock);
	spi = g_registry.secondary_spi;
	if (spi) {
		get_device(&spi->dev);
	}
	mutex_unlock(&g_registry.lock);

	return spi;
}

static int sharp_drm_forced_colors(const struct sharp_memory_device *sdev)
{
	if (!sdev) {
		return 0;
	}

	if (sdev->panels[0].panel_type == SHARP_PANEL_TYPE_SHARP_MONO) {
		return 2;
	}
	if (sdev->dual_panel) {
		return 64;
	}

	return 0;
}

static const char *sharp_drm_forced_colors_reason(
	const struct sharp_memory_device *sdev)
{
	if (sdev && sdev->panels[0].panel_type == SHARP_PANEL_TYPE_SHARP_MONO) {
		return "panel-type=sharp_mono";
	}
	if (sdev && sdev->dual_panel) {
		return "dual-panel";
	}
	return "device";
}

static int sharp_drm_validate_colors_for_device(
	const struct sharp_memory_device *sdev, int colors)
{
	int forced = sharp_drm_forced_colors(sdev);

	if (forced && colors != forced) {
		pr_warn("sharp_memory: colors=%d rejected: %s supports only colors=%d\n",
			colors, sharp_drm_forced_colors_reason(sdev), forced);
		return -EINVAL;
	}

	return 0;
}

int sharp_drm_validate_colors(int colors)
{
	/* Caller holds sharp_drm_render_params_lock(). */
	return sharp_drm_validate_colors_for_device(
		sharp_drm_registry_device_locked(), colors);
}

static void sharp_drm_enforce_colors(struct device *dev,
	struct sharp_memory_device *sdev)
{
	int forced = sharp_drm_forced_colors(sdev);

	if (forced) {
		params_force_colors(dev, forced, sharp_drm_forced_colors_reason(sdev));
	}
}

int sharp_drm_redraw_fb(struct drm_device *drm, int height)
{
	struct sharp_memory_device *sdev;
	int drm_idx;
	int ret;

	if (!drm) {
		return -ENODEV;
	}

	sdev = drm_to_device(drm);
	mutex_lock(&g_registry.lock);
	if (sharp_drm_registry_device_locked() != sdev) {
		mutex_unlock(&g_registry.lock);
		return -ENODEV;
	}
	drm_dev_get(drm);
	mutex_unlock(&g_registry.lock);

	if (!drm_dev_enter(drm, &drm_idx)) {
		ret = -ENODEV;
		goto out_put;
	}

	ret = sharp_drm_redraw_primary(sdev, height);
	drm_dev_exit(drm_idx);

out_put:
	drm_dev_put(drm);
	return ret;
}
