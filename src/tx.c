// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Row-oriented transmit pipeline for Sharp Memory LCD panels.
 */

#include "sharp_drm.h"

#include <linux/bitmap.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/overflow.h>
#include <linux/string.h>

#include <drm/drm_managed.h>

#define SHARP_TX_RETRY_LIMIT 3U
#define SHARP_TX_RETRY_DELAY_MS 20U

static u32 sharp_tx_next_generation(u32 generation)
{
	generation++;
	if (!generation) {
		generation++;
	}
	return generation;
}

static bool sharp_tx_panel_has_sendable_locked(
	const struct sharp_subpanel *panel)
{
	unsigned long row;

	if (!panel->desired_valid || !panel->desired_line_len) {
		return false;
	}

	for_each_set_bit(row, panel->dirty_rows, panel->height) {
		if (panel->queued_generation[row]
			!= panel->row_generation[row]) {
			return true;
		}
	}

	return false;
}

static bool sharp_tx_slots_busy_locked(const struct sharp_memory_device *sdev)
{
	unsigned int i;

	for (i = 0; i < SHARP_TX_SLOT_COUNT; i++) {
		if (sdev->tx_slots[i].state != SHARP_TX_SLOT_FREE) {
			return true;
		}
	}

	return false;
}

static unsigned int sharp_tx_slots_used_locked(
	const struct sharp_memory_device *sdev)
{
	unsigned int used = 0;
	unsigned int i;

	for (i = 0; i < SHARP_TX_SLOT_COUNT; i++) {
		used += sdev->tx_slots[i].state != SHARP_TX_SLOT_FREE;
	}

	return used;
}

static void sharp_tx_release_slot_locked(struct sharp_tx_slot *slot)
{
	struct sharp_memory_device *sdev = slot->sdev;
	struct sharp_subpanel *panel;
	unsigned int i;

	if (slot->state == SHARP_TX_SLOT_FREE) {
		return;
	}

	if (slot->panel_index < sdev->panel_count) {
		panel = &sdev->panels[slot->panel_index];
		for (i = 0; i < slot->row_count; i++) {
			unsigned int row = slot->rows[i];

			if (row < panel->height
				&& panel->queued_generation[row]
					== slot->generations[i]) {
				panel->queued_generation[row] = 0;
			}
		}
	}

	slot->len = 0;
	slot->line_len = 0;
	slot->row_count = 0;
	slot->state = SHARP_TX_SLOT_FREE;
	slot->ready_ns = 0;
}

static bool sharp_tx_fill_slot_locked(struct sharp_memory_device *sdev,
	struct sharp_tx_slot *slot, unsigned int panel_index)
{
	struct sharp_subpanel *panel = &sdev->panels[panel_index];
	size_t line_len = panel->desired_line_len;
	size_t payload_limit;
	unsigned long row;
	unsigned int scan_start = panel->tx_next_row;
	unsigned int pass;
	u64 start_ns;
	bool measure;

	if (slot->state != SHARP_TX_SLOT_FREE
		|| !sharp_tx_panel_has_sendable_locked(panel)) {
		return false;
	}

	payload_limit = min(slot->capacity, panel->tx_max_payload);
	if (!line_len || line_len > payload_limit) {
		sdev->tx_last_error = -EMSGSIZE;
		return false;
	}

	measure = sharp_perf_enabled();
	start_ns = measure ? ktime_get_ns() : 0;
	slot->panel_index = panel_index;
	slot->line_len = line_len;
	slot->len = 0;
	slot->row_count = 0;

	/* Resume after the last packed row, including across producer updates. */
	for (pass = 0; pass < 2; pass++) {
		unsigned int limit = pass ? scan_start : panel->height;

		for (row = find_next_bit(panel->dirty_rows, limit,
			pass ? 0 : scan_start); row < limit;
			row = find_next_bit(panel->dirty_rows, limit, row + 1)) {
			u32 generation = panel->row_generation[row];
			u8 *src;
			u8 *dst;

			if (panel->queued_generation[row] == generation) {
				continue;
			}
			if (slot->row_count >= slot->row_capacity
				|| slot->len + line_len > payload_limit) {
				goto packed;
			}

			src = panel->desired + ((size_t)row * panel->max_line_len);
			dst = slot->buf + slot->len;
			memcpy(dst, src, line_len);
			slot->rows[slot->row_count] = (unsigned int)row;
			slot->generations[slot->row_count] = generation;
			panel->queued_generation[row] = generation;
			slot->row_count++;
			slot->len += line_len;
			panel->tx_next_row = (row + 1) % panel->height;
		}
	}

packed:
	if (!slot->row_count) {
		slot->len = 0;
		slot->line_len = 0;
		return false;
	}

	memset(slot->buf + slot->capacity, 0, LPM027M128B_TRAILER_BYTES);
	slot->sequence = ++sdev->tx_sequence;
	slot->state = SHARP_TX_SLOT_READY;
	slot->ready_ns = measure ? ktime_get_ns() : 0;
	if (measure) {
		sharp_perf_add(&sdev->perf.pack_ns, slot->ready_ns - start_ns);
		sharp_perf_add(&sdev->perf.packed_bytes, slot->len);
		sharp_perf_update_max(&sdev->perf.max_slots_used,
			sharp_tx_slots_used_locked(sdev));
	}

	return true;
}

static void sharp_tx_queue_ready_locked(struct sharp_memory_device *sdev)
{
	struct sharp_tx_slot *oldest = NULL;
	unsigned int i;

	if (!sdev->tx_running || sdev->tx_paused || sdev->tx_shutdown) {
		return;
	}

	for (i = 0; i < SHARP_TX_SLOT_COUNT; i++) {
		struct sharp_tx_slot *slot = &sdev->tx_slots[i];

		if (slot->state == SHARP_TX_SLOT_READY &&
			(!oldest || slot->sequence < oldest->sequence)) {
			oldest = slot;
		}
	}
	/* Queue one batch at a time so reused slot indices cannot reorder data. */
	if (oldest) {
		queue_work(sdev->tx_wq, &oldest->work);
	}
}

static void sharp_tx_fill_available_locked(struct sharp_memory_device *sdev)
{
	unsigned int i;

	if (!sdev->tx_running || sdev->tx_paused || sdev->tx_shutdown
		|| sdev->tx_retry_blocked) {
		return;
	}

	for (i = 0; i < SHARP_TX_SLOT_COUNT; i++) {
		struct sharp_tx_slot *slot = &sdev->tx_slots[i];
		unsigned int panel_offset;

		if (slot->state != SHARP_TX_SLOT_FREE) {
			continue;
		}

		for (panel_offset = 0; panel_offset < sdev->panel_count;
			panel_offset++) {
			unsigned int panel_index =
				(sdev->tx_next_panel + panel_offset)
				% sdev->panel_count;

			if (sharp_tx_fill_slot_locked(sdev, slot, panel_index)) {
				sdev->tx_next_panel =
					(panel_index + 1) % sdev->panel_count;
				break;
			}
		}
	}
}

static void sharp_tx_reset_retry_locked(struct sharp_memory_device *sdev)
{
	sdev->tx_retry_count = 0;
	sdev->tx_retry_blocked = false;
}

void sharp_tx_kick_locked(struct sharp_memory_device *sdev)
{
	bool sendable = false;
	bool free_slot = false;
	unsigned int i;

	if (!sdev || !sdev->tx_running || sdev->tx_paused
		|| sdev->tx_shutdown) {
		return;
	}
	/* A new producer update starts a fresh, bounded retry budget. */
	cancel_delayed_work(&sdev->tx_retry_work);
	sharp_tx_reset_retry_locked(sdev);

	for (i = 0; i < sdev->panel_count; i++) {
		sendable |= sharp_tx_panel_has_sendable_locked(&sdev->panels[i]);
	}
	for (i = 0; i < SHARP_TX_SLOT_COUNT; i++) {
		free_slot |= sdev->tx_slots[i].state == SHARP_TX_SLOT_FREE;
	}
	if (sendable && !free_slot) {
		sharp_perf_add(&sdev->perf.slot_stalls, 1);
	}

	sharp_tx_fill_available_locked(sdev);
	sharp_tx_queue_ready_locked(sdev);
}

static void sharp_tx_retry_work(struct work_struct *work)
{
	struct sharp_memory_device *sdev = container_of(to_delayed_work(work),
		struct sharp_memory_device, tx_retry_work);

	mutex_lock(&sdev->fb_lock);
	if (sdev->tx_running && !sdev->tx_paused && !sdev->tx_shutdown) {
		sdev->tx_retry_blocked = false;
		sharp_tx_fill_available_locked(sdev);
		sharp_tx_queue_ready_locked(sdev);
	}
	mutex_unlock(&sdev->fb_lock);
}

static void sharp_tx_complete_slot_locked(struct sharp_tx_slot *slot, int rc)
{
	struct sharp_memory_device *sdev = slot->sdev;
	struct sharp_subpanel *panel = &sdev->panels[slot->panel_index];
	unsigned int i;

	for (i = 0; i < slot->row_count; i++) {
		unsigned int row = slot->rows[i];
		u32 generation = slot->generations[i];

		if (WARN_ON_ONCE(row >= panel->height)) {
			continue;
		}

		if (!rc) {
			u8 *dst = panel->displayed
				+ ((size_t)row * panel->max_line_len);
			const u8 *src = slot->buf
				+ ((size_t)i * slot->line_len);

			memcpy(dst, src, slot->line_len);
			panel->displayed_line_len[row] = (u16)slot->line_len;
			set_bit(row, panel->displayed_rows);
		}

		if (panel->queued_generation[row] == generation) {
			panel->queued_generation[row] = 0;
		}

		if (!rc && panel->desired_valid
			&& panel->row_generation[row] == generation) {
			clear_bit(row, panel->dirty_rows);
		} else {
			set_bit(row, panel->dirty_rows);
		}
	}

	panel->displayed_valid = panel->desired_valid
		&& bitmap_empty(panel->dirty_rows, panel->height)
		&& bitmap_full(panel->displayed_rows, panel->height);

	slot->len = 0;
	slot->line_len = 0;
	slot->row_count = 0;
	slot->state = SHARP_TX_SLOT_FREE;
	slot->ready_ns = 0;
}

static void sharp_tx_slot_work(struct work_struct *work)
{
	struct sharp_tx_slot *slot =
		container_of(work, struct sharp_tx_slot, work);
	struct sharp_memory_device *sdev = slot->sdev;
	struct sharp_subpanel *panel;
	const void *trailer;
	u64 start_ns;
	u64 elapsed_ns;
	size_t len;
	unsigned int rows;
	u64 ready_ns;
	int rc;

	mutex_lock(&sdev->fb_lock);
	if (!sdev->tx_running || sdev->tx_shutdown
		|| slot->state != SHARP_TX_SLOT_READY) {
		sharp_tx_release_slot_locked(slot);
		mutex_unlock(&sdev->fb_lock);
		wake_up_all(&sdev->tx_slot_wait);
		return;
	}

	slot->state = SHARP_TX_SLOT_SENDING;
	panel = &sdev->panels[slot->panel_index];
	trailer = slot->buf + slot->capacity;
	len = slot->len;
	rows = slot->row_count;
	ready_ns = slot->ready_ns;
	mutex_unlock(&sdev->fb_lock);

	start_ns = sharp_perf_enabled() ? ktime_get_ns() : 0;
	if (start_ns && ready_ns) {
		u64 queue_wait_ns = start_ns - ready_ns;

		sharp_perf_add(&sdev->perf.queue_wait_ns, queue_wait_ns);
		sharp_perf_update_max(&sdev->perf.queue_wait_max_ns,
			queue_wait_ns);
	}
	rc = sharp_spi_write_tagged_batch(panel, slot->buf, len, trailer,
		slot->xfers);
	elapsed_ns = start_ns ? ktime_get_ns() - start_ns : 0;

	if (start_ns) {
		sharp_perf_add(&sdev->perf.spi_ns, elapsed_ns);
		sharp_perf_update_max(&sdev->perf.spi_max_ns, elapsed_ns);
	}
	sharp_perf_add(&sdev->perf.sent_rows, rows);
	sharp_perf_add(&sdev->perf.tx_bytes,
		len + LPM027M128B_TRAILER_BYTES);
	sharp_perf_add(&sdev->perf.tx_messages, 1);
	sharp_perf_add(&sdev->perf.tx_transfers, 2);
	if (rc) {
		sharp_perf_add(&sdev->perf.spi_errors, 1);
		dev_warn_ratelimited(sdev->drm.dev,
			"SPI row batch failed: %d\n", rc);
	}

	mutex_lock(&sdev->fb_lock);
	if (rc && !sdev->tx_last_error) {
		sdev->tx_last_error = rc;
	}
	sharp_tx_complete_slot_locked(slot, rc);

	if (sdev->tx_running && !sdev->tx_paused
		&& !sdev->tx_shutdown) {
		if (rc) {
			sdev->tx_retry_blocked = true;
			if (sdev->tx_retry_count < SHARP_TX_RETRY_LIMIT) {
				unsigned int delay_ms = SHARP_TX_RETRY_DELAY_MS
					<< sdev->tx_retry_count++;

				mod_delayed_work(sdev->tx_wq, &sdev->tx_retry_work,
					msecs_to_jiffies(delay_ms));
			}
		} else {
			sharp_tx_fill_available_locked(sdev);
		}
		/* Running work may requeue itself after its PENDING bit is cleared. */
		sharp_tx_queue_ready_locked(sdev);
	}
	mutex_unlock(&sdev->fb_lock);
	wake_up_all(&sdev->tx_slot_wait);
}

int sharp_tx_store_converted_locked(struct sharp_memory_device *sdev,
	struct sharp_subpanel *panel, const struct sharp_render_params *params,
	const struct drm_rect *clip, size_t buf_len)
{
	const size_t line_len = sharp_render_panel_line_len(panel, params);
	unsigned int height;
	u64 start_ns = sharp_perf_enabled() ? ktime_get_ns() : 0;
	unsigned int skipped = 0;
	unsigned int line;

	if (!sdev || !panel || !params || !clip || !line_len
		|| clip->y1 < 0 || clip->y2 < clip->y1
		|| clip->y2 > (int)panel->height) {
		return -EINVAL;
	}
	height = (unsigned int)(clip->y2 - clip->y1);
	if (buf_len != (size_t)height * line_len) {
		return -EINVAL;
	}

	sharp_perf_add(&sdev->perf.candidate_rows, height);

	for (line = 0; line < height; line++) {
		unsigned int row = (unsigned int)clip->y1 + line;
		const u8 *src = panel->buf + ((size_t)line * line_len);
		u8 *desired = panel->desired
			+ ((size_t)row * panel->max_line_len);
		const u8 *displayed = panel->displayed
			+ ((size_t)row * panel->max_line_len);
		bool desired_same = panel->desired_valid
			&& panel->desired_line_len == line_len
			&& memcmp(desired, src, line_len) == 0;
		bool displayed_same = test_bit(row, panel->displayed_rows)
			&& panel->displayed_line_len[row] == line_len
			&& memcmp(displayed, src, line_len) == 0;

		if (!desired_same) {
			if (panel->queued_generation[row]) {
				sharp_perf_add(&sdev->perf.coalesced_rows, 1);
			}
			memcpy(desired, src, line_len);
			panel->row_generation[row] =
				sharp_tx_next_generation(
					panel->row_generation[row]);
		}

		if (panel->queued_generation[row] || !displayed_same) {
			set_bit(row, panel->dirty_rows);
		} else {
			clear_bit(row, panel->dirty_rows);
			skipped++;
		}
	}

	panel->desired_line_len = line_len;
	panel->desired_params = *params;
	panel->desired_valid = true;
	panel->displayed_valid =
		bitmap_empty(panel->dirty_rows, panel->height)
		&& bitmap_full(panel->displayed_rows, panel->height);

	sharp_perf_add(&sdev->perf.skipped_rows, skipped);
	if (start_ns) {
		sharp_perf_add(&sdev->perf.compare_ns,
			ktime_get_ns() - start_ns);
	}

	return 0;
}

void sharp_tx_invalidate_all_locked(struct sharp_memory_device *sdev)
{
	unsigned int panel_index;

	for (panel_index = 0; panel_index < sdev->panel_count; panel_index++) {
		struct sharp_subpanel *panel = &sdev->panels[panel_index];
		unsigned int row;

		for (row = 0; row < panel->height; row++) {
			panel->row_generation[row] =
				sharp_tx_next_generation(
					panel->row_generation[row]);
		}
		panel->desired_valid = false;
		panel->displayed_valid = false;
		panel->desired_line_len = 0;
		panel->tx_next_row = 0;
		bitmap_zero(panel->dirty_rows, panel->height);
		bitmap_zero(panel->displayed_rows, panel->height);
	}
}

static void sharp_tx_cancel_ready_locked(struct sharp_memory_device *sdev)
{
	unsigned int i;

	for (i = 0; i < SHARP_TX_SLOT_COUNT; i++) {
		if (sdev->tx_slots[i].state == SHARP_TX_SLOT_READY) {
			sharp_tx_release_slot_locked(&sdev->tx_slots[i]);
		}
	}
}

int sharp_tx_pause_locked(struct sharp_memory_device *sdev)
{
	int rc = 0;

	if (!sdev) {
		return -ENODEV;
	}
	lockdep_assert_held(&sdev->tx_control_lock);

	if (!sdev->tx_wq || sdev->tx_shutdown) {
		return -ESHUTDOWN;
	}

	mutex_lock(&sdev->fb_lock);
	sdev->tx_last_error = 0;
	sharp_tx_reset_retry_locked(sdev);
	cancel_delayed_work(&sdev->tx_retry_work);
	if (!sdev->tx_running) {
		sdev->tx_paused = true;
		mutex_unlock(&sdev->fb_lock);
		return 0;
	}
	sdev->tx_paused = false;
	sharp_tx_fill_available_locked(sdev);
	sharp_tx_queue_ready_locked(sdev);
	mutex_unlock(&sdev->fb_lock);

	for (;;) {
		bool busy;
		bool sendable = false;
		unsigned int i;

		flush_workqueue(sdev->tx_wq);

		mutex_lock(&sdev->fb_lock);
		if (sdev->tx_last_error) {
			rc = sdev->tx_last_error;
			sharp_tx_cancel_ready_locked(sdev);
			sdev->tx_paused = true;
			cancel_delayed_work(&sdev->tx_retry_work);
			mutex_unlock(&sdev->fb_lock);
			break;
		}

		for (i = 0; i < sdev->panel_count; i++) {
			sendable |= sharp_tx_panel_has_sendable_locked(
				&sdev->panels[i]);
		}
		busy = sharp_tx_slots_busy_locked(sdev);
		if (!sendable && !busy) {
			sdev->tx_paused = true;
			mutex_unlock(&sdev->fb_lock);
			break;
		}

		sharp_tx_fill_available_locked(sdev);
		sharp_tx_queue_ready_locked(sdev);
		mutex_unlock(&sdev->fb_lock);
	}

	return rc;
}

void sharp_tx_resume_locked(struct sharp_memory_device *sdev)
{
	if (!sdev) {
		return;
	}
	lockdep_assert_held(&sdev->tx_control_lock);

	mutex_lock(&sdev->fb_lock);
	if (sdev->tx_wq && sdev->tx_running && !sdev->tx_shutdown) {
		sdev->tx_paused = false;
		sdev->tx_last_error = 0;
		sharp_tx_reset_retry_locked(sdev);
		sharp_tx_fill_available_locked(sdev);
		sharp_tx_queue_ready_locked(sdev);
	}
	mutex_unlock(&sdev->fb_lock);
	wake_up_all(&sdev->tx_slot_wait);
}

int sharp_tx_wait_idle(struct sharp_memory_device *sdev)
{
	int rc;

	if (!sdev) {
		return -ENODEV;
	}
	mutex_lock(&sdev->tx_control_lock);
	rc = sharp_tx_pause_locked(sdev);
	sharp_tx_resume_locked(sdev);
	mutex_unlock(&sdev->tx_control_lock);

	return rc;
}

void sharp_tx_start(struct sharp_memory_device *sdev)
{
	if (!sdev || !sdev->tx_wq) {
		return;
	}

	mutex_lock(&sdev->tx_control_lock);
	mutex_lock(&sdev->fb_lock);
	if (!sdev->tx_shutdown && !READ_ONCE(sdev->shutting_down)) {
		sharp_tx_cancel_ready_locked(sdev);
		sharp_tx_invalidate_all_locked(sdev);
		sdev->tx_last_error = 0;
		sharp_tx_reset_retry_locked(sdev);
		sdev->tx_running = true;
		sdev->tx_paused = false;
	}
	mutex_unlock(&sdev->fb_lock);
	mutex_unlock(&sdev->tx_control_lock);
	wake_up_all(&sdev->tx_slot_wait);
}

static void sharp_tx_stop_locked(struct sharp_memory_device *sdev)
{
	unsigned int i;

	mutex_lock(&sdev->fb_lock);
	sdev->tx_running = false;
	sdev->tx_paused = true;
	sharp_tx_cancel_ready_locked(sdev);
	mutex_unlock(&sdev->fb_lock);

	cancel_delayed_work_sync(&sdev->tx_retry_work);
	for (i = 0; i < SHARP_TX_SLOT_COUNT; i++) {
		cancel_work_sync(&sdev->tx_slots[i].work);
	}

	mutex_lock(&sdev->fb_lock);
	sharp_tx_cancel_ready_locked(sdev);
	sharp_tx_invalidate_all_locked(sdev);
	mutex_unlock(&sdev->fb_lock);
	wake_up_all(&sdev->tx_slot_wait);
}

void sharp_tx_stop(struct sharp_memory_device *sdev)
{
	if (!sdev || !sdev->tx_wq) {
		return;
	}
	mutex_lock(&sdev->tx_control_lock);
	sharp_tx_stop_locked(sdev);
	mutex_unlock(&sdev->tx_control_lock);
}

void sharp_tx_shutdown(struct sharp_memory_device *sdev)
{
	if (!sdev || !sdev->tx_wq) {
		return;
	}

	mutex_lock(&sdev->tx_control_lock);
	mutex_lock(&sdev->fb_lock);
	sdev->tx_shutdown = true;
	mutex_unlock(&sdev->fb_lock);
	sharp_tx_stop_locked(sdev);
	mutex_unlock(&sdev->tx_control_lock);
	wake_up_all(&sdev->tx_slot_wait);
}

int sharp_tx_init(struct sharp_memory_device *sdev)
{
	struct drm_device *drm;
	size_t max_capacity = 0;
	size_t checked_capacity;
	unsigned int max_rows = 0;
	unsigned int panel_index;
	unsigned int slot_index;
	int ret;

	if (!sdev) {
		return -EINVAL;
	}

	drm = &sdev->drm;
	for (panel_index = 0; panel_index < sdev->panel_count; panel_index++) {
		struct sharp_subpanel *panel = &sdev->panels[panel_index];
		size_t payload = sharp_spi_max_payload(panel);

		if (payload < panel->max_line_len) {
			dev_err(drm->dev,
				"SPI payload limit %zu is smaller than panel line %zu\n",
				payload, panel->max_line_len);
			return -EMSGSIZE;
		}

		panel->tx_max_payload = min(payload, panel->row_store_len);
		max_capacity = max(max_capacity, panel->tx_max_payload);
		max_rows = max(max_rows, panel->height);
	}

	if (!max_capacity || !max_rows
		|| check_add_overflow(max_capacity,
			(size_t)LPM027M128B_TRAILER_BYTES, &checked_capacity)) {
		return -EINVAL;
	}
	max_capacity = checked_capacity - LPM027M128B_TRAILER_BYTES;

	for (slot_index = 0; slot_index < SHARP_TX_SLOT_COUNT; slot_index++) {
		struct sharp_tx_slot *slot = &sdev->tx_slots[slot_index];
		size_t alloc_len;

		if (check_add_overflow(max_capacity,
			(size_t)LPM027M128B_TRAILER_BYTES, &alloc_len)) {
			return -EINVAL;
		}

		slot->buf = drmm_kzalloc(drm, alloc_len, GFP_KERNEL);
		slot->rows = drmm_kcalloc(drm, max_rows,
			sizeof(*slot->rows), GFP_KERNEL);
		slot->generations = drmm_kcalloc(drm, max_rows,
			sizeof(*slot->generations), GFP_KERNEL);
		if (!slot->buf || !slot->rows || !slot->generations) {
			return -ENOMEM;
		}

		slot->sdev = sdev;
		slot->capacity = max_capacity;
		slot->row_capacity = max_rows;
		slot->state = SHARP_TX_SLOT_FREE;
		INIT_WORK(&slot->work, sharp_tx_slot_work);
	}

	/*
	 * Register the managed workqueue after its buffers so final cleanup
	 * destroys the queue before releasing memory referenced by queued work.
	 */
	sdev->tx_wq = drmm_alloc_ordered_workqueue(drm, "sharp_drm_tx",
		WQ_MEM_RECLAIM | WQ_FREEZABLE);
	if (IS_ERR(sdev->tx_wq)) {
		ret = PTR_ERR(sdev->tx_wq);
		sdev->tx_wq = NULL;
		return ret;
	}

	sdev->tx_sequence = 0;
	INIT_DELAYED_WORK(&sdev->tx_retry_work, sharp_tx_retry_work);
	init_waitqueue_head(&sdev->tx_slot_wait);
	sdev->tx_next_panel = 0;
	sdev->tx_running = false;
	sdev->tx_paused = true;
	sdev->tx_shutdown = false;
	sdev->tx_last_error = 0;

	return 0;
}
