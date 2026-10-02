// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * User-space boundary tests for src/spi_io.c.
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef EMSGSIZE
#define EMSGSIZE 90
#endif

#include "../src/spi_io.c"

static void panel_init(struct sharp_subpanel *panel, struct spi_device *spi,
	u8 command[2])
{
	memset(panel, 0, sizeof(*panel));
	memset(spi, 0, sizeof(*spi));
	panel->spi = spi;
	panel->cmd_buf = command;
}

static void test_controller_limits(void)
{
	struct sharp_subpanel panel;
	struct spi_device spi;
	u8 command[2];

	panel_init(&panel, &spi, command);
	spi.max_transfer_size = (size_t)-1;
	spi.max_message_size = (size_t)-1;
	assert(sharp_spi_max_payload(&panel)
		== SHARP_SPI_FALLBACK_MAX_XFER);

	spi.max_transfer_size = 100;
	spi.max_message_size = 80;
	assert(sharp_spi_max_payload(&panel) == 78);

	spi.max_transfer_size = 64;
	spi.max_message_size = (size_t)-1;
	assert(sharp_spi_max_payload(&panel) == 64);
}

static void test_tagged_batch_transfers(void)
{
	struct sharp_subpanel panel;
	struct spi_device spi;
	struct spi_transfer xfers[2];
	u8 command[2];
	u8 data[70] = { 0 };
	u8 trailer[2] = { 0 };

	panel_init(&panel, &spi, command);
	spi.max_transfer_size = 100;
	spi.max_message_size = 80;

	assert(sharp_spi_write_tagged_batch(&panel, data, sizeof(data),
		trailer, xfers) == 0);
	assert(spi.sync_calls == 1);
	assert(spi.sync_first_len[0] == 70);
	assert(spi.sync_num_xfers[0] == 2);
	assert(xfers[0].tx_buf == data && xfers[0].len == sizeof(data));
	assert(xfers[1].tx_buf == trailer && xfers[1].len == sizeof(trailer));
}

static void test_invalid_batch(void)
{
	struct sharp_subpanel panel;
	struct spi_device spi;
	struct spi_transfer xfers[2];
	u8 command[2];
	u8 trailer[2] = { 0 };
	u8 data[81] = { 0 };

	panel_init(&panel, &spi, command);
	spi.max_transfer_size = 100;
	spi.max_message_size = 80;

	assert(sharp_spi_write_tagged_batch(&panel, data, 79, trailer, xfers)
		== -EMSGSIZE);
	assert(sharp_spi_write_tagged_batch(&panel, data, 0, trailer, xfers)
		== -EINVAL);
	assert(spi.sync_calls == 0);
}

static void test_panel_commands(void)
{
	struct sharp_subpanel panel;
	struct spi_device spi;
	u8 command[2];

	panel_init(&panel, &spi, command);
	assert(sharp_spi_set_mode(&panel, LPM027M128B_MODE_INVERT) == 0);
	assert(command[0] == LPM027M128B_MODE_INVERT && command[1] == 0);
	assert(sharp_spi_clear_screen(&panel) == 0);
	assert(command[0] == LPM027M128B_MODE_CLEAR && command[1] == 0);
	assert(spi.sync_calls == 2);
	assert(spi.sync_first_len[0] == 2 && spi.sync_num_xfers[0] == 1);
	assert(spi.sync_first_len[1] == 2 && spi.sync_num_xfers[1] == 1);
	assert(!panel.cmd_lock.locked);
}

int main(void)
{
	test_controller_limits();
	test_tagged_batch_transfers();
	test_invalid_batch();
	test_panel_commands();
	puts("all SPI I/O cases passed");
	return 0;
}
