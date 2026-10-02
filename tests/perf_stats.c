// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * User-space tests for src/perf.c counter and histogram behavior.
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "../src/perf.c"

static void test_disabled_and_reset(void)
{
	struct sharp_memory_device sdev;

	memset(&sdev, 0xff, sizeof(sdev));
	g_perf_stats_enabled = false;
	sharp_perf_reset(&sdev);
	sharp_perf_add(&sdev.perf.sent_rows, 3);
	assert(sdev.perf.sent_rows == 0);
}

static void test_update_percentiles(void)
{
	struct sharp_memory_device sdev;

	memset(&sdev, 0, sizeof(sdev));
	g_perf_stats_enabled = true;
	sharp_perf_record_update(&sdev, 100);
	sharp_perf_record_update(&sdev, 200);
	sharp_perf_record_update(&sdev, 300);
	sharp_perf_record_update(&sdev, 400);

	assert(sdev.perf.update_calls == 4);
	assert(sdev.perf.update_ns == 1000);
	assert(sdev.perf.update_max_ns == 400);
	assert(sharp_perf_update_percentile(&sdev.perf, 4, 50) == 256);
	assert(sharp_perf_update_percentile(&sdev.perf, 4, 95) == 512);

	sharp_perf_reset(&sdev);
	assert(sdev.perf.update_calls == 0);
	assert(sharp_perf_update_percentile(&sdev.perf, 0, 50) == 0);
}

static void test_debugfs_control(void)
{
	struct sharp_memory_device sdev;
	struct seq_file seq = { .private = &sdev };
	struct file file = { .private_data = &seq };
	loff_t position = 0;
	const char enable[] = "enable\n";
	const char disable[] = "disable\n";
	const char invalid[] = "invalid\n";

	memset(&sdev, 0, sizeof(sdev));
	g_perf_stats_enabled = false;
	assert(sharp_perf_stats_write(&file, enable, sizeof(enable) - 1,
		&position) == (ssize_t)(sizeof(enable) - 1));
	assert(g_perf_stats_enabled);
	assert(sharp_perf_stats_write(&file, disable, sizeof(disable) - 1,
		&position) == (ssize_t)(sizeof(disable) - 1));
	assert(!g_perf_stats_enabled);
	assert(sharp_perf_stats_write(&file, invalid, sizeof(invalid) - 1,
		&position) == -EINVAL);
}

int main(void)
{
	test_disabled_and_reset();
	test_update_percentiles();
	test_debugfs_control();
	puts("all performance counter cases passed");
	return 0;
}
