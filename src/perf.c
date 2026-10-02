// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Runtime performance counters for the Sharp Memory LCD DRM driver.
 */

#include "sharp_drm.h"

#include <linux/debugfs.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/uaccess.h>

static bool g_perf_stats_enabled;
module_param_named(perf_stats, g_perf_stats_enabled, bool, 0644);
MODULE_PARM_DESC(perf_stats,
	"collect Sharp DRM update pipeline performance counters");

bool sharp_perf_enabled(void)
{
	return READ_ONCE(g_perf_stats_enabled);
}

void sharp_perf_add(atomic64_t *counter, u64 value)
{
	if (!sharp_perf_enabled()) {
		return;
	}
	atomic64_add((s64)value, counter);
}

void sharp_perf_update_max(atomic64_t *counter, u64 value)
{
	s64 observed;
	s64 next = (s64)value;

	if (!sharp_perf_enabled()) {
		return;
	}
	observed = atomic64_read(counter);

	while (next > observed) {
		s64 previous = atomic64_cmpxchg(counter, observed, next);

		if (previous == observed) {
			break;
		}
		observed = previous;
	}
}

void sharp_perf_record_update(struct sharp_memory_device *sdev, u64 value)
{
	unsigned int bucket = 0;
	u64 upper = value ? value - 1 : 0;

	if (!sdev || !sharp_perf_enabled()) {
		return;
	}

	while (upper && bucket < SHARP_PERF_LATENCY_BUCKETS - 1) {
		upper >>= 1;
		bucket++;
	}

	atomic64_add(1, &sdev->perf.update_calls);
	atomic64_add((s64)value, &sdev->perf.update_ns);
	sharp_perf_update_max(&sdev->perf.update_max_ns, value);
	atomic64_add(1, &sdev->perf.update_latency[bucket]);
}

void sharp_perf_reset(struct sharp_memory_device *sdev)
{
	struct sharp_perf_stats *stats;
	unsigned int i;

	if (!sdev) {
		return;
	}

	stats = &sdev->perf;
	atomic64_set(&stats->update_calls, 0);
	atomic64_set(&stats->update_ns, 0);
	atomic64_set(&stats->update_max_ns, 0);
	atomic64_set(&stats->convert_ns, 0);
	atomic64_set(&stats->compare_ns, 0);
	atomic64_set(&stats->pack_ns, 0);
	atomic64_set(&stats->packed_bytes, 0);
	atomic64_set(&stats->spi_ns, 0);
	atomic64_set(&stats->spi_max_ns, 0);
	atomic64_set(&stats->queue_wait_ns, 0);
	atomic64_set(&stats->queue_wait_max_ns, 0);
	atomic64_set(&stats->candidate_rows, 0);
	atomic64_set(&stats->sent_rows, 0);
	atomic64_set(&stats->skipped_rows, 0);
	atomic64_set(&stats->tx_bytes, 0);
	atomic64_set(&stats->tx_messages, 0);
	atomic64_set(&stats->tx_transfers, 0);
	atomic64_set(&stats->slot_stalls, 0);
	atomic64_set(&stats->max_slots_used, 0);
	atomic64_set(&stats->coalesced_rows, 0);
	atomic64_set(&stats->throttled_updates, 0);
	atomic64_set(&stats->spi_errors, 0);
	atomic64_set(&stats->full_refreshes, 0);
	for (i = 0; i < SHARP_PERF_LATENCY_BUCKETS; i++) {
		atomic64_set(&stats->update_latency[i], 0);
	}
}

static u64 sharp_perf_read(const atomic64_t *counter)
{
	return (u64)atomic64_read(counter);
}

static u64 sharp_perf_update_percentile(const struct sharp_perf_stats *stats,
	u64 total, unsigned int percentile)
{
	u64 target;
	u64 seen = 0;
	unsigned int i;

	if (!total) {
		return 0;
	}

	target = (total / 100) * percentile;
	target += DIV_ROUND_UP((total % 100) * percentile, 100);
	for (i = 0; i < SHARP_PERF_LATENCY_BUCKETS; i++) {
		seen += sharp_perf_read(&stats->update_latency[i]);
		if (seen >= target) {
			return i == SHARP_PERF_LATENCY_BUCKETS - 1
				? U64_MAX : (u64)1 << i;
		}
	}

	return U64_MAX;
}

static int sharp_perf_stats_show(struct seq_file *seq, void *unused)
{
	struct sharp_memory_device *sdev = seq->private;
	struct sharp_perf_stats *stats = &sdev->perf;
	u64 updates = sharp_perf_read(&stats->update_calls);
	u64 messages = sharp_perf_read(&stats->tx_messages);
	u64 update_ns = sharp_perf_read(&stats->update_ns);
	u64 spi_ns = sharp_perf_read(&stats->spi_ns);

	(void)unused;

	seq_printf(seq, "enabled %u\n", sharp_perf_enabled() ? 1 : 0);
	seq_printf(seq, "update_calls %llu\n", updates);
	seq_printf(seq, "update_ns %llu\n", update_ns);
	seq_printf(seq, "update_avg_ns %llu\n",
		updates ? div64_u64(update_ns, updates) : 0);
	seq_printf(seq, "update_max_ns %llu\n",
		sharp_perf_read(&stats->update_max_ns));
	seq_printf(seq, "update_p50_ns %llu\n",
		sharp_perf_update_percentile(stats, updates, 50));
	seq_printf(seq, "update_p95_ns %llu\n",
		sharp_perf_update_percentile(stats, updates, 95));
	seq_printf(seq, "update_p99_ns %llu\n",
		sharp_perf_update_percentile(stats, updates, 99));
	seq_printf(seq, "convert_ns %llu\n",
		sharp_perf_read(&stats->convert_ns));
	seq_printf(seq, "compare_ns %llu\n",
		sharp_perf_read(&stats->compare_ns));
	seq_printf(seq, "pack_ns %llu\n",
		sharp_perf_read(&stats->pack_ns));
	seq_printf(seq, "packed_bytes %llu\n",
		sharp_perf_read(&stats->packed_bytes));
	seq_printf(seq, "spi_ns %llu\n", spi_ns);
	seq_printf(seq, "spi_avg_ns %llu\n",
		messages ? div64_u64(spi_ns, messages) : 0);
	seq_printf(seq, "spi_max_ns %llu\n",
		sharp_perf_read(&stats->spi_max_ns));
	seq_printf(seq, "queue_wait_ns %llu\n",
		sharp_perf_read(&stats->queue_wait_ns));
	seq_printf(seq, "queue_wait_avg_ns %llu\n",
		messages ? div64_u64(
			sharp_perf_read(&stats->queue_wait_ns), messages) : 0);
	seq_printf(seq, "queue_wait_max_ns %llu\n",
		sharp_perf_read(&stats->queue_wait_max_ns));
	seq_printf(seq, "candidate_rows %llu\n",
		sharp_perf_read(&stats->candidate_rows));
	seq_printf(seq, "sent_rows %llu\n",
		sharp_perf_read(&stats->sent_rows));
	seq_printf(seq, "skipped_rows %llu\n",
		sharp_perf_read(&stats->skipped_rows));
	seq_printf(seq, "tx_bytes %llu\n",
		sharp_perf_read(&stats->tx_bytes));
	seq_printf(seq, "tx_messages %llu\n", messages);
	seq_printf(seq, "tx_transfers %llu\n",
		sharp_perf_read(&stats->tx_transfers));
	seq_printf(seq, "slot_stalls %llu\n",
		sharp_perf_read(&stats->slot_stalls));
	seq_printf(seq, "max_slots_used %llu\n",
		sharp_perf_read(&stats->max_slots_used));
	seq_printf(seq, "coalesced_rows %llu\n",
		sharp_perf_read(&stats->coalesced_rows));
	seq_printf(seq, "throttled_updates %llu\n",
		sharp_perf_read(&stats->throttled_updates));
	seq_printf(seq, "spi_errors %llu\n",
		sharp_perf_read(&stats->spi_errors));
	seq_printf(seq, "full_refreshes %llu\n",
		sharp_perf_read(&stats->full_refreshes));

	return 0;
}

static int sharp_perf_stats_open(struct inode *inode, struct file *file)
{
	struct sharp_memory_device *sdev = inode->i_private;
	int ret;

	drm_dev_get(&sdev->drm);
	ret = single_open(file, sharp_perf_stats_show, sdev);
	if (ret) {
		drm_dev_put(&sdev->drm);
	}

	return ret;
}

static ssize_t sharp_perf_stats_write(struct file *file,
	const char __user *user_buf, size_t count, loff_t *ppos)
{
	struct seq_file *seq = file->private_data;
	struct sharp_memory_device *sdev = seq->private;
	char buf[16];
	size_t len;

	(void)ppos;

	len = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, user_buf, len)) {
		return -EFAULT;
	}
	buf[len] = '\0';

	if (sysfs_streq(buf, "enable") || sysfs_streq(buf, "1")) {
		WRITE_ONCE(g_perf_stats_enabled, true);
		sharp_perf_reset(sdev);
	} else if (sysfs_streq(buf, "disable")) {
		WRITE_ONCE(g_perf_stats_enabled, false);
	} else if (sysfs_streq(buf, "reset") || sysfs_streq(buf, "0")) {
		sharp_perf_reset(sdev);
	} else {
		return -EINVAL;
	}

	return count;
}

static int sharp_perf_stats_release(struct inode *inode, struct file *file)
{
	struct seq_file *seq = file->private_data;
	struct sharp_memory_device *sdev = seq->private;
	int ret;

	ret = single_release(inode, file);
	drm_dev_put(&sdev->drm);

	return ret;
}

static const struct file_operations sharp_perf_stats_fops = {
	.owner = THIS_MODULE,
	.open = sharp_perf_stats_open,
	.read = seq_read,
	.write = sharp_perf_stats_write,
	.llseek = seq_lseek,
	.release = sharp_perf_stats_release,
};

void sharp_perf_debugfs_init(struct sharp_memory_device *sdev)
{
	struct dentry *dir;

	if (!sdev) {
		return;
	}

	sharp_perf_reset(sdev);
	dir = debugfs_create_dir("sharp_drm", NULL);
	if (IS_ERR_OR_NULL(dir)) {
		sdev->debugfs_dir = NULL;
		return;
	}

	sdev->debugfs_dir = dir;
	debugfs_create_file("stats", 0644, dir, sdev, &sharp_perf_stats_fops);
}

void sharp_perf_debugfs_cleanup(struct sharp_memory_device *sdev)
{
	if (!sdev) {
		return;
	}

	debugfs_remove_recursive(sdev->debugfs_dir);
	sdev->debugfs_dir = NULL;
}
