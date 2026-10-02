// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DRM driver for 2.7" Sharp Memory LCD
 *
 * Copyright 2023 Andrew D'Angelo
 */

#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/of.h>
#include <linux/string.h>

#include "sharp_drm.h"

static bool sharp_drm_spi_is_aux(const struct spi_device *spi)
{
	const struct spi_device_id *id;

	if (!spi) {
		return false;
	}

	id = spi_get_device_id(spi);
	if (id && !strcmp(id->name, "sharp-drm-aux")) {
		return true;
	}

	if (spi->dev.of_node
		&& of_device_is_compatible(spi->dev.of_node, "sharp-drm-aux")) {
		return true;
	}

	return false;
}

static int sharp_drm_spi_probe(struct spi_device *spi)
{
	int ret;

	dev_dbg(&spi->dev, "probing SPI device\n");

	/* Check if this is the secondary (auxiliary) device. */
	if (sharp_drm_spi_is_aux(spi)) {
		dev_dbg(&spi->dev, "probing secondary SPI device (CS%u)\n",
			spi->chip_select[0]);
		sharp_drm_register_secondary_spi(spi);
		return 0;
	}

	if ((ret = sharp_drm_probe(spi))) {
		return ret;
	}

	dev_dbg(&spi->dev, "probe complete\n");

	return 0;
}

static void sharp_drm_spi_remove(struct spi_device *spi)
{
	if (sharp_drm_spi_is_aux(spi)) {
		sharp_drm_unregister_secondary_spi(spi);
		return;
	}
	sharp_drm_remove(spi);
}

static void sharp_drm_spi_shutdown(struct spi_device *spi)
{
	if (sharp_drm_spi_is_aux(spi)) {
		sharp_drm_unregister_secondary_spi(spi);
		return;
	}
	sharp_drm_shutdown(spi);
}

static const struct of_device_id sharp_drm_of_match[] = {
	{ .compatible = "sharp-drm" },
	{ .compatible = "sharp-drm-aux" },
	{ }
};
MODULE_DEVICE_TABLE(of, sharp_drm_of_match);

static const struct spi_device_id sharp_drm_spi_id[] = {
	{ "sharp-drm", 0 },
	{ "sharp-drm-aux", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, sharp_drm_spi_id);

static struct spi_driver sharp_drm_spi_driver = {
	.driver = {
		.name = "sharp-drm",
		.of_match_table = sharp_drm_of_match,
	},
	.id_table = sharp_drm_spi_id,
	.probe = sharp_drm_spi_probe,
	.remove = sharp_drm_spi_remove,
	.shutdown = sharp_drm_spi_shutdown,
};
module_spi_driver(sharp_drm_spi_driver);

MODULE_VERSION("2.0.0");
MODULE_DESCRIPTION("Sharp Memory LCD DRM driver");
MODULE_AUTHOR("Andrew D'Angelo");
MODULE_LICENSE("GPL");

int sharp_memory_set_invert(int setting)
{
	return params_set_mono_invert(setting);
}
EXPORT_SYMBOL_GPL(sharp_memory_set_invert);
