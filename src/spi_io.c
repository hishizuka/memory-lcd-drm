// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DRM driver for 2.7" Sharp Memory LCD
 *
 * Copyright 2023 Andrew D'Angelo
 */

#include "sharp_drm.h"

#include <linux/delay.h>
#include <linux/string.h>

static size_t sharp_spi_transfer_limit(struct sharp_subpanel *panel)
{
	size_t limit;

	if (!panel || !panel->spi) {
		return 0;
	}

	limit = spi_max_transfer_size(panel->spi);
	if (!limit || limit == (size_t)-1) {
		return SHARP_SPI_FALLBACK_MAX_XFER;
	}

	return limit;
}

size_t sharp_spi_max_payload(struct sharp_subpanel *panel)
{
	size_t transfer_limit;
	size_t message_limit;
	size_t payload_limit;

	if (!panel || !panel->spi) {
		return 0;
	}

	transfer_limit = sharp_spi_transfer_limit(panel);
	message_limit = spi_max_message_size(panel->spi);
	payload_limit = transfer_limit;

	if (message_limit && message_limit != (size_t)-1) {
		if (message_limit <= LPM027M128B_TRAILER_BYTES) {
			return 0;
		}
		payload_limit = min(payload_limit,
			message_limit - LPM027M128B_TRAILER_BYTES);
	}

	return payload_limit;
}

int sharp_spi_write_tagged_batch(struct sharp_subpanel *panel,
	const void *line_data, size_t len, const void *trailer,
	struct spi_transfer xfers[2])
{
	size_t limit;

	if (!panel || !line_data || !len || !trailer || !xfers) {
		return -EINVAL;
	}

	limit = sharp_spi_max_payload(panel);
	if (!limit || len > limit) {
		return -EMSGSIZE;
	}

	memset(xfers, 0, sizeof(*xfers) * 2);
	xfers[0].tx_buf = line_data;
	xfers[0].len = len;
	xfers[1].tx_buf = trailer;
	xfers[1].len = LPM027M128B_TRAILER_BYTES;

	ndelay(80);

	return spi_sync_transfer(panel->spi, xfers, 2);
}

int sharp_spi_clear_screen(struct sharp_subpanel *panel)
{
	return sharp_spi_set_mode(panel, LPM027M128B_MODE_CLEAR);
}

int sharp_spi_set_mode(struct sharp_subpanel *panel, u8 mode)
{
	int rc;

	if (!panel || !panel->spi || !panel->cmd_buf) {
		return -EINVAL;
	}

	mutex_lock(&panel->cmd_lock);
	panel->cmd_buf[0] = mode;
	panel->cmd_buf[1] = 0;
	panel->cmd_xfers[0].tx_buf = panel->cmd_buf;
	panel->cmd_xfers[0].len = 2;

	ndelay(80);

	rc = spi_sync_transfer(panel->spi, panel->cmd_xfers, 1);
	mutex_unlock(&panel->cmd_lock);

	return rc;
}
