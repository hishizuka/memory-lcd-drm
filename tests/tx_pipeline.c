// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * User-space state-machine tests for src/tx.c.
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ENODEV
#define ENODEV 19
#endif
#ifndef ENOMEM
#define ENOMEM 12
#endif
#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef EMSGSIZE
#define EMSGSIZE 90
#endif
#ifndef ESHUTDOWN
#define ESHUTDOWN 108
#endif

#include "../src/tx.c"

static int g_spi_result;
static unsigned int g_spi_calls;
static u8 g_spi_values[64];
static size_t g_spi_lengths[64];

bool sharp_render_params_equal(const struct sharp_render_params *a,
	const struct sharp_render_params *b)
{
	return memcmp(a, b, sizeof(*a)) == 0;
}

size_t sharp_render_panel_line_len(const struct sharp_subpanel *panel,
	const struct sharp_render_params *params)
{
	if (params->colors == 2) {
		return panel->line_len_mono;
	}
	if (params->colors == 64) {
		return panel->line_len_color64;
	}
	return panel->line_len_color8;
}

size_t sharp_spi_max_payload(struct sharp_subpanel *panel)
{
	return panel->tx_max_payload;
}

int sharp_spi_write_tagged_batch(struct sharp_subpanel *panel,
	const void *line_data, size_t len, const void *trailer,
	struct spi_transfer xfers[2])
{
	(void)panel;
	assert(g_spi_calls < ARRAY_SIZE(g_spi_values));
	g_spi_values[g_spi_calls] = ((const u8 *)line_data)[2];
	g_spi_lengths[g_spi_calls++] = len;
	(void)trailer;
	(void)xfers;
	return g_spi_result;
}

void sharp_perf_add(atomic64_t *counter, u64 value)
{
	*counter += (atomic64_t)value;
}

bool sharp_perf_enabled(void)
{
	return true;
}

void sharp_perf_update_max(atomic64_t *counter, u64 value)
{
	if ((u64)*counter < value) {
		*counter = (atomic64_t)value;
	}
}

struct tx_fixture {
	struct sharp_memory_device sdev;
	struct device dev;
	struct sharp_render_params params;
	struct drm_rect clip;
};

static void fixture_init_limits(struct tx_fixture *fixture, unsigned int height,
	size_t line_len, size_t slot_capacity)
{
	struct sharp_subpanel *panel;
	unsigned int i;

	memset(fixture, 0, sizeof(*fixture));
	g_spi_result = 0;
	g_spi_calls = 0;
	fixture->sdev.drm.dev = &fixture->dev;
	fixture->sdev.panel_count = 1;
	fixture->sdev.tx_running = true;
	fixture->sdev.tx_wq = alloc_ordered_workqueue("test", 0);
	INIT_DELAYED_WORK(&fixture->sdev.tx_retry_work, sharp_tx_retry_work);
	fixture->params.colors = 2;
	fixture->clip.y1 = 0;
	fixture->clip.y2 = (int)height;

	panel = &fixture->sdev.panels[0];
	panel->width = 8;
	panel->height = height;
	panel->line_len_mono = line_len;
	panel->line_len_color8 = 5;
	panel->line_len_color64 = 8;
	panel->max_line_len = max_t(size_t, line_len, 8);
	panel->row_store_len = (size_t)height * panel->max_line_len;
	panel->tx_max_payload = slot_capacity;
	panel->buf = calloc(height, line_len);
	panel->desired = calloc(1, panel->row_store_len);
	panel->displayed = calloc(1, panel->row_store_len);
	panel->displayed_line_len = calloc(height,
		sizeof(*panel->displayed_line_len));
	panel->row_generation = calloc(height,
		sizeof(*panel->row_generation));
	panel->queued_generation = calloc(height,
		sizeof(*panel->queued_generation));
	panel->dirty_rows = calloc(BITS_TO_LONGS(height),
		sizeof(*panel->dirty_rows));
	panel->displayed_rows = calloc(BITS_TO_LONGS(height),
		sizeof(*panel->displayed_rows));

	for (i = 0; i < SHARP_TX_SLOT_COUNT; i++) {
		struct sharp_tx_slot *slot = &fixture->sdev.tx_slots[i];

		slot->sdev = &fixture->sdev;
		slot->capacity = slot_capacity;
		slot->row_capacity = height;
		slot->buf = calloc(1,
			slot_capacity + LPM027M128B_TRAILER_BYTES);
		slot->rows = calloc(height, sizeof(*slot->rows));
		slot->generations = calloc(height,
			sizeof(*slot->generations));
		slot->state = SHARP_TX_SLOT_FREE;
		INIT_WORK(&slot->work, sharp_tx_slot_work);
	}
}

static void fixture_init(struct tx_fixture *fixture, unsigned int height)
{
	fixture_init_limits(fixture, height, 3, 6);
}

static void fixture_destroy(struct tx_fixture *fixture)
{
	struct sharp_subpanel *panel = &fixture->sdev.panels[0];
	unsigned int i;

	sharp_tx_stop(&fixture->sdev);
	for (i = 0; i < SHARP_TX_SLOT_COUNT; i++) {
		free(fixture->sdev.tx_slots[i].buf);
		free(fixture->sdev.tx_slots[i].rows);
		free(fixture->sdev.tx_slots[i].generations);
	}
	free(panel->buf);
	free(panel->desired);
	free(panel->displayed);
	free(panel->displayed_line_len);
	free(panel->row_generation);
	free(panel->queued_generation);
	free(panel->dirty_rows);
	free(panel->displayed_rows);
	destroy_workqueue(fixture->sdev.tx_wq);
}

static void set_wire_row(struct sharp_subpanel *panel, unsigned int line,
	u8 value)
{
	u8 *dst = panel->buf + ((size_t)line * panel->line_len_mono);

	dst[0] = 0x80;
	dst[1] = (u8)(line + 1);
	dst[2] = value;
}

static void complete_slot(struct sharp_tx_slot *slot, int rc)
{
	slot->state = SHARP_TX_SLOT_SENDING;
	sharp_tx_complete_slot_locked(slot, rc);
}

static void test_full_update_batches(void)
{
	struct tx_fixture fixture;
	struct sharp_subpanel *panel;
	struct sharp_tx_slot *slot0;
	struct sharp_tx_slot *slot1;
	unsigned int row;

	fixture_init(&fixture, 4);
	panel = &fixture.sdev.panels[0];
	for (row = 0; row < panel->height; row++) {
		set_wire_row(panel, row, (u8)(0x10 + row));
	}

	assert(sharp_tx_store_converted_locked(&fixture.sdev, panel,
		&fixture.params, &fixture.clip,
		panel->height * panel->line_len_mono) == 0);
	assert(panel->desired_valid);
	for (row = 0; row < panel->height; row++) {
		assert(test_bit(row, panel->dirty_rows));
		assert(panel->row_generation[row] == 1);
	}

	sharp_tx_fill_available_locked(&fixture.sdev);
	slot0 = &fixture.sdev.tx_slots[0];
	slot1 = &fixture.sdev.tx_slots[1];
	assert(slot0->state == SHARP_TX_SLOT_READY);
	assert(slot1->state == SHARP_TX_SLOT_READY);
	assert(slot0->row_count == 2);
	assert(slot1->row_count == 2);
	assert(slot0->rows[0] == 0 && slot0->rows[1] == 1);
	assert(slot1->rows[0] == 2 && slot1->rows[1] == 3);

	complete_slot(slot0, 0);
	assert(!test_bit(0, panel->dirty_rows));
	assert(!test_bit(1, panel->dirty_rows));
	assert(test_bit(2, panel->dirty_rows));
	assert(!panel->displayed_valid);

	complete_slot(slot1, 0);
	assert(bitmap_empty(panel->dirty_rows, panel->height));
	assert(panel->displayed_valid);
	assert(bitmap_full(panel->displayed_rows, panel->height));

	fixture_destroy(&fixture);
}

static void test_latest_generation_wins(void)
{
	struct tx_fixture fixture;
	struct sharp_subpanel *panel;
	struct sharp_tx_slot *old_slot;
	struct sharp_tx_slot *new_slot;

	fixture_init(&fixture, 1);
	panel = &fixture.sdev.panels[0];
	set_wire_row(panel, 0, 0x11);
	assert(sharp_tx_store_converted_locked(&fixture.sdev, panel,
		&fixture.params, &fixture.clip, panel->line_len_mono) == 0);
	sharp_tx_fill_available_locked(&fixture.sdev);
	old_slot = &fixture.sdev.tx_slots[0];
	assert(old_slot->generations[0] == 1);

	set_wire_row(panel, 0, 0x22);
	assert(sharp_tx_store_converted_locked(&fixture.sdev, panel,
		&fixture.params, &fixture.clip, panel->line_len_mono) == 0);
	assert(panel->row_generation[0] == 2);
	assert(fixture.sdev.perf.coalesced_rows == 1);
	sharp_tx_fill_available_locked(&fixture.sdev);
	new_slot = &fixture.sdev.tx_slots[1];
	assert(new_slot->state == SHARP_TX_SLOT_READY);
	assert(new_slot->generations[0] == 2);

	complete_slot(old_slot, 0);
	assert(test_bit(0, panel->dirty_rows));
	assert(panel->displayed[2] == 0x11);
	assert(panel->queued_generation[0] == 2);

	complete_slot(new_slot, 0);
	assert(!test_bit(0, panel->dirty_rows));
	assert(panel->displayed[2] == 0x22);
	assert(panel->queued_generation[0] == 0);
	assert(panel->displayed_valid);

	fixture_destroy(&fixture);
}

static void test_more_than_two_batches_progress(void)
{
	struct tx_fixture fixture;
	struct sharp_subpanel *panel;
	struct sharp_tx_slot *slot0;
	struct sharp_tx_slot *slot1;
	unsigned int row;

	/*
	 * Six rows at two rows per slot require three controller-sized batches.
	 * This models a frame larger than the SPI message limit.
	 */
	fixture_init(&fixture, 6);
	panel = &fixture.sdev.panels[0];
	for (row = 0; row < panel->height; row++) {
		set_wire_row(panel, row, (u8)(0x20 + row));
	}

	assert(sharp_tx_store_converted_locked(&fixture.sdev, panel,
		&fixture.params, &fixture.clip,
		panel->height * panel->line_len_mono) == 0);
	sharp_tx_fill_available_locked(&fixture.sdev);
	slot0 = &fixture.sdev.tx_slots[0];
	slot1 = &fixture.sdev.tx_slots[1];
	assert(slot0->rows[0] == 0 && slot0->rows[1] == 1);
	assert(slot1->rows[0] == 2 && slot1->rows[1] == 3);

	complete_slot(slot0, 0);
	sharp_tx_fill_available_locked(&fixture.sdev);
	assert(slot0->state == SHARP_TX_SLOT_READY);
	assert(slot0->rows[0] == 4 && slot0->rows[1] == 5);

	complete_slot(slot1, 0);
	assert(test_bit(4, panel->dirty_rows));
	assert(test_bit(5, panel->dirty_rows));
	complete_slot(slot0, 0);
	assert(bitmap_empty(panel->dirty_rows, panel->height));
	assert(panel->displayed_valid);

	fixture_destroy(&fixture);
}

static void test_failed_batch_remains_dirty(void)
{
	struct tx_fixture fixture;
	struct sharp_subpanel *panel;
	struct sharp_tx_slot *slot;

	fixture_init(&fixture, 1);
	panel = &fixture.sdev.panels[0];
	set_wire_row(panel, 0, 0x33);
	assert(sharp_tx_store_converted_locked(&fixture.sdev, panel,
		&fixture.params, &fixture.clip, panel->line_len_mono) == 0);
	sharp_tx_fill_available_locked(&fixture.sdev);
	slot = &fixture.sdev.tx_slots[0];

	complete_slot(slot, -EIO);
	assert(test_bit(0, panel->dirty_rows));
	assert(panel->queued_generation[0] == 0);
	assert(!test_bit(0, panel->displayed_rows));
	assert(!panel->displayed_valid);

	sharp_tx_fill_available_locked(&fixture.sdev);
	assert(fixture.sdev.tx_slots[0].state == SHARP_TX_SLOT_READY);
	assert(fixture.sdev.tx_slots[0].generations[0]
		== panel->row_generation[0]);

	fixture_destroy(&fixture);
}

static void test_invalidate_requires_new_full_conversion(void)
{
	struct tx_fixture fixture;
	struct sharp_subpanel *panel;
	u32 generation;

	fixture_init(&fixture, 1);
	panel = &fixture.sdev.panels[0];
	set_wire_row(panel, 0, 0x44);
	assert(sharp_tx_store_converted_locked(&fixture.sdev, panel,
		&fixture.params, &fixture.clip, panel->line_len_mono) == 0);
	generation = panel->row_generation[0];
	set_bit(0, panel->displayed_rows);

	sharp_tx_invalidate_all_locked(&fixture.sdev);
	assert(!panel->desired_valid);
	assert(!panel->displayed_valid);
	assert(panel->desired_line_len == 0);
	assert(panel->row_generation[0] != generation);
	assert(bitmap_empty(panel->dirty_rows, panel->height));
	assert(bitmap_empty(panel->displayed_rows, panel->height));

	fixture_destroy(&fixture);
}

static void test_dual_panel_round_robin(void)
{
	struct tx_fixture fixture;
	struct sharp_subpanel *left;
	struct sharp_subpanel *right;
	struct sharp_tx_slot *slot0;
	struct sharp_tx_slot *slot1;

	fixture_init(&fixture, 1);
	fixture.sdev.panel_count = 2;
	left = &fixture.sdev.panels[0];
	right = &fixture.sdev.panels[1];
	*right = *left;
	right->buf = calloc(1, right->line_len_mono);
	right->desired = calloc(1, right->row_store_len);
	right->displayed = calloc(1, right->row_store_len);
	right->displayed_line_len = calloc(1,
		sizeof(*right->displayed_line_len));
	right->row_generation = calloc(1,
		sizeof(*right->row_generation));
	right->queued_generation = calloc(1,
		sizeof(*right->queued_generation));
	right->dirty_rows = calloc(BITS_TO_LONGS(right->height),
		sizeof(*right->dirty_rows));
	right->displayed_rows = calloc(BITS_TO_LONGS(right->height),
		sizeof(*right->displayed_rows));

	set_wire_row(left, 0, 0x55);
	set_wire_row(right, 0, 0xaa);
	assert(sharp_tx_store_converted_locked(&fixture.sdev, left,
		&fixture.params, &fixture.clip, left->line_len_mono) == 0);
	assert(sharp_tx_store_converted_locked(&fixture.sdev, right,
		&fixture.params, &fixture.clip, right->line_len_mono) == 0);

	sharp_tx_fill_available_locked(&fixture.sdev);
	slot0 = &fixture.sdev.tx_slots[0];
	slot1 = &fixture.sdev.tx_slots[1];
	assert(slot0->state == SHARP_TX_SLOT_READY);
	assert(slot1->state == SHARP_TX_SLOT_READY);
	assert(slot0->panel_index == 0);
	assert(slot1->panel_index == 1);
	assert(slot0->sequence < slot1->sequence);

	complete_slot(slot0, 0);
	complete_slot(slot1, 0);
	assert(left->displayed[2] == 0x55);
	assert(right->displayed[2] == 0xaa);

	free(right->buf);
	free(right->desired);
	free(right->displayed);
	free(right->displayed_line_len);
	free(right->row_generation);
	free(right->queued_generation);
	free(right->dirty_rows);
	free(right->displayed_rows);
	fixture.sdev.panel_count = 1;
	fixture_destroy(&fixture);
}

static void test_worker_drains_more_than_two_batches(void)
{
	struct tx_fixture fixture;
	struct sharp_subpanel *panel;
	unsigned int row;

	fixture_init(&fixture, 6);
	panel = &fixture.sdev.panels[0];
	for (row = 0; row < panel->height; row++) {
		set_wire_row(panel, row, (u8)(0x20 + row));
	}
	assert(sharp_tx_store_converted_locked(&fixture.sdev, panel,
		&fixture.params, &fixture.clip, 6 * panel->line_len_mono) == 0);
	sharp_tx_kick_locked(&fixture.sdev);
	flush_workqueue(fixture.sdev.tx_wq);
	assert(g_spi_calls == 3);
	assert(g_spi_values[0] == 0x20 && g_spi_values[1] == 0x22
		&& g_spi_values[2] == 0x24);
	assert(panel->displayed_valid);
	fixture_destroy(&fixture);
}

static void test_worker_retries_without_new_damage(void)
{
	struct tx_fixture fixture;
	struct sharp_subpanel *panel;

	fixture_init(&fixture, 4);
	panel = &fixture.sdev.panels[0];
	for (unsigned int row = 0; row < panel->height; row++) {
		set_wire_row(panel, row, (u8)(0x40 + row));
	}
	assert(sharp_tx_store_converted_locked(&fixture.sdev, panel,
		&fixture.params, &fixture.clip, 4 * panel->line_len_mono) == 0);
	sharp_tx_kick_locked(&fixture.sdev);
	g_spi_result = -EIO;
	assert(test_workqueue_run_one(fixture.sdev.tx_wq));
	assert(fixture.sdev.tx_retry_work.scheduled);
	g_spi_result = 0;
	flush_workqueue(fixture.sdev.tx_wq);
	assert(g_spi_calls == 2); /* The other READY batch still runs. */
	assert(test_bit(0, panel->dirty_rows));
	test_workqueue_advance(fixture.sdev.tx_wq, SHARP_TX_RETRY_DELAY_MS);
	flush_workqueue(fixture.sdev.tx_wq);
	assert(g_spi_calls == 3 && panel->displayed_valid);
	fixture_destroy(&fixture);
}

static void test_worker_retry_budget_and_shutdown(void)
{
	struct tx_fixture fixture;
	struct sharp_subpanel *panel;

	fixture_init(&fixture, 1);
	panel = &fixture.sdev.panels[0];
	set_wire_row(panel, 0, 0x70);
	assert(sharp_tx_store_converted_locked(&fixture.sdev, panel,
		&fixture.params, &fixture.clip, panel->line_len_mono) == 0);
	sharp_tx_kick_locked(&fixture.sdev);
	g_spi_result = -EIO;
	flush_workqueue(fixture.sdev.tx_wq);
	for (unsigned int retry = 0; retry < SHARP_TX_RETRY_LIMIT; retry++) {
		test_workqueue_advance(fixture.sdev.tx_wq, 1000);
		flush_workqueue(fixture.sdev.tx_wq);
	}
	assert(g_spi_calls == 1 + SHARP_TX_RETRY_LIMIT);
	assert(!fixture.sdev.tx_retry_work.scheduled);
	assert(fixture.sdev.tx_retry_blocked);
	assert(test_bit(0, panel->dirty_rows));
	sharp_tx_kick_locked(&fixture.sdev);
	flush_workqueue(fixture.sdev.tx_wq);
	assert(fixture.sdev.tx_retry_work.scheduled);
	sharp_tx_shutdown(&fixture.sdev);
	assert(!fixture.sdev.tx_retry_work.scheduled);
	test_workqueue_advance(fixture.sdev.tx_wq, 1000);
	flush_workqueue(fixture.sdev.tx_wq);
	assert(g_spi_calls == 2 + SHARP_TX_RETRY_LIMIT);
	sharp_tx_start(&fixture.sdev);
	assert(!fixture.sdev.tx_running);
	fixture_destroy(&fixture);
}

static void test_ready_slots_follow_sequence(void)
{
	struct tx_fixture fixture;
	struct sharp_subpanel *panel;

	fixture_init(&fixture, 1);
	panel = &fixture.sdev.panels[0];
	set_wire_row(panel, 0, 0x11);
	assert(sharp_tx_store_converted_locked(&fixture.sdev, panel,
		&fixture.params, &fixture.clip, panel->line_len_mono) == 0);
	assert(sharp_tx_fill_slot_locked(&fixture.sdev,
		&fixture.sdev.tx_slots[1], 0));
	set_wire_row(panel, 0, 0x22);
	assert(sharp_tx_store_converted_locked(&fixture.sdev, panel,
		&fixture.params, &fixture.clip, panel->line_len_mono) == 0);
	sharp_tx_fill_available_locked(&fixture.sdev);
	assert(fixture.sdev.tx_slots[1].sequence <
		fixture.sdev.tx_slots[0].sequence);
	sharp_tx_queue_ready_locked(&fixture.sdev);
	flush_workqueue(fixture.sdev.tx_wq);
	assert(g_spi_calls == 2);
	assert(g_spi_values[0] == 0x11 && g_spi_values[1] == 0x22);
	assert(panel->displayed[2] == 0x22 && panel->displayed_valid);
	fixture_destroy(&fixture);
}

static void test_controller_row_boundaries(void)
{
	static const struct {
		unsigned int height;
		size_t line_len, capacity;
		unsigned int colors, batches;
		size_t first, last;
	} cases[] = {
		{ 25, 10, 78, 2, 4, 70, 40 },
		{ 480, 484, SHARP_SPI_FALLBACK_MAX_XFER, 64, 4, 484 * 135, 484 * 75 },
		{ 480, 242, SHARP_SPI_FALLBACK_MAX_XFER, 8, 2, 242 * 270, 242 * 210 },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		struct tx_fixture fixture;
		struct sharp_subpanel *panel;
		size_t total = 0;
		unsigned int batch;

		fixture_init_limits(&fixture, cases[i].height,
			cases[i].line_len, cases[i].capacity);
		panel = &fixture.sdev.panels[0];
		fixture.params.colors = cases[i].colors;
		panel->line_len_color8 = panel->line_len_color64 = cases[i].line_len;
		assert(sharp_tx_store_converted_locked(&fixture.sdev, panel,
			&fixture.params, &fixture.clip, cases[i].height * cases[i].line_len) == 0);
		sharp_tx_kick_locked(&fixture.sdev);
		flush_workqueue(fixture.sdev.tx_wq);
		assert(g_spi_calls == cases[i].batches);
		assert(g_spi_lengths[0] == cases[i].first);
		assert(g_spi_lengths[g_spi_calls - 1] == cases[i].last);
		for (batch = 0; batch < g_spi_calls; batch++) {
			assert(g_spi_lengths[batch] <= cases[i].capacity);
			assert(g_spi_lengths[batch] % cases[i].line_len == 0);
			total += g_spi_lengths[batch];
		}
		assert(total == cases[i].height * cases[i].line_len);
		assert(panel->displayed_valid);
		fixture_destroy(&fixture);
	}
}

static void test_color_mode_changes_update_transfer_lengths(void)
{
	static const unsigned int modes[] = { 2, 8, 64, 2, 64, 8 };
	static const size_t lengths[] = { 36, 104, 208, 36, 208, 104 };
	struct tx_fixture fixture;
	struct sharp_subpanel *panel;
	unsigned int i, row;

	fixture_init_limits(&fixture, 2, 208, 416);
	panel = &fixture.sdev.panels[0];
	panel->width = 272;
	panel->line_len_mono = 36;
	panel->line_len_color8 = 104;
	panel->line_len_color64 = 208;
	/* Identical bytes still require transmission when the wire length changes. */
	memset(panel->buf, 0x3c, 416);
	for (i = 0; i < ARRAY_SIZE(modes); i++) {
		unsigned int before = g_spi_calls;

		fixture.params.colors = modes[i];
		assert(sharp_tx_store_converted_locked(&fixture.sdev, panel,
			&fixture.params, &fixture.clip, 2 * lengths[i]) == 0);
		assert(bitmap_full(panel->dirty_rows, panel->height));
		sharp_tx_kick_locked(&fixture.sdev);
		flush_workqueue(fixture.sdev.tx_wq);
		assert(g_spi_calls == before + 1);
		assert(g_spi_lengths[before] == 2 * lengths[i]);
		assert(panel->displayed_valid);
		for (row = 0; row < panel->height; row++)
			assert(panel->displayed_line_len[row] == lengths[i]);
	}

	/* Queue old formats, then switch again while both TX slots are occupied. */
	for (i = 0; i < 3; i++) {
		fixture.params.colors = modes[2 - i];
		assert(sharp_tx_store_converted_locked(&fixture.sdev, panel,
			&fixture.params, &fixture.clip, 2 * lengths[2 - i]) == 0);
		sharp_tx_kick_locked(&fixture.sdev);
	}
	flush_workqueue(fixture.sdev.tx_wq);
	assert(g_spi_lengths[g_spi_calls - 1] == 72);
	assert(panel->desired_line_len == 36 && panel->displayed_valid);
	assert(bitmap_empty(panel->dirty_rows, panel->height));
	for (row = 0; row < panel->height; row++)
		assert(panel->displayed_line_len[row] == 36);
	fixture_destroy(&fixture);
}

static void test_row_larger_than_controller_limit(void)
{
	struct tx_fixture fixture;
	struct sharp_subpanel *panel;

	fixture_init_limits(&fixture, 1, 484, 483);
	panel = &fixture.sdev.panels[0];
	assert(sharp_tx_store_converted_locked(&fixture.sdev, panel,
		&fixture.params, &fixture.clip, 484) == 0);
	sharp_tx_kick_locked(&fixture.sdev);
	flush_workqueue(fixture.sdev.tx_wq);
	assert(g_spi_calls == 0 && fixture.sdev.tx_last_error == -EMSGSIZE);
	assert(test_bit(0, panel->dirty_rows));
	fixture_destroy(&fixture);
}

static void test_pause_resume_lock_ownership(void)
{
	struct tx_fixture fixture;
	struct sharp_subpanel *panel;

	fixture_init(&fixture, 1);
	panel = &fixture.sdev.panels[0];
	set_wire_row(panel, 0, 0x11);
	assert(sharp_tx_store_converted_locked(&fixture.sdev, panel,
		&fixture.params, &fixture.clip, panel->line_len_mono) == 0);
	sharp_tx_kick_locked(&fixture.sdev);
	mutex_lock(&fixture.sdev.tx_control_lock);
	assert(sharp_tx_pause_locked(&fixture.sdev) == 0);
	assert(fixture.sdev.tx_control_lock.locked && fixture.sdev.tx_paused);
	assert(g_spi_calls == 1 && panel->displayed_valid);
	set_wire_row(panel, 0, 0x22);
	assert(sharp_tx_store_converted_locked(&fixture.sdev, panel,
		&fixture.params, &fixture.clip, panel->line_len_mono) == 0);
	sharp_tx_resume_locked(&fixture.sdev);
	assert(fixture.sdev.tx_control_lock.locked && !fixture.sdev.tx_paused);
	mutex_unlock(&fixture.sdev.tx_control_lock);
	flush_workqueue(fixture.sdev.tx_wq);
	assert(g_spi_calls == 2 && g_spi_values[1] == 0x22);
	fixture_destroy(&fixture);
}

static void test_wait_idle_releases_lock_after_error(void)
{
	struct tx_fixture fixture;
	struct sharp_subpanel *panel;

	fixture_init(&fixture, 1);
	panel = &fixture.sdev.panels[0];
	set_wire_row(panel, 0, 0x33);
	assert(sharp_tx_store_converted_locked(&fixture.sdev, panel,
		&fixture.params, &fixture.clip, panel->line_len_mono) == 0);
	sharp_tx_kick_locked(&fixture.sdev);
	g_spi_result = -EIO;
	assert(sharp_tx_wait_idle(&fixture.sdev) == -EIO);
	assert(!fixture.sdev.tx_control_lock.locked && !fixture.sdev.fb_lock.locked);
	assert(!fixture.sdev.tx_paused);
	g_spi_result = 0;
	flush_workqueue(fixture.sdev.tx_wq);
	assert(panel->displayed_valid);
	sharp_tx_shutdown(&fixture.sdev);
	assert(sharp_tx_wait_idle(&fixture.sdev) == -ESHUTDOWN);
	assert(!fixture.sdev.tx_control_lock.locked);
	assert(sharp_tx_wait_idle(NULL) == -ENODEV);
	fixture_destroy(&fixture);
}

static void test_continuous_updates_reach_every_row(void)
{
	static const unsigned int sizes[][2] = { { 272, 451 }, { 640, 480 } };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		struct tx_fixture fixture;
		struct sharp_subpanel *panel;
		size_t line_len = 4 + sizes[i][0] * 3 / 4;
		unsigned int frame, row;

		fixture_init_limits(&fixture, sizes[i][1], line_len, 65535);
		panel = &fixture.sdev.panels[0];
		panel->width = sizes[i][0];
		panel->line_len_color64 = line_len;
		fixture.params.colors = 64;
		for (frame = 1; frame <= 40; frame++) {
			for (row = 0; row < panel->height; row++) {
				set_wire_row(panel, row, (u8)frame);
			}
			assert(sharp_tx_store_converted_locked(&fixture.sdev, panel,
				&fixture.params, &fixture.clip, panel->height * line_len) == 0);
			sharp_tx_kick_locked(&fixture.sdev);
			if (frame > 1) {
				assert(test_workqueue_run_one(fixture.sdev.tx_wq));
			}
		}
		/* Every row progresses while a faster local producer is still active. */
		for (row = 0; row < panel->height; row++) {
			assert(test_bit(row, panel->displayed_rows));
			assert(panel->displayed[row * panel->max_line_len + 2] >= 30);
		}
		flush_workqueue(fixture.sdev.tx_wq);
		assert(bitmap_empty(panel->dirty_rows, panel->height));
		for (row = 0; row < panel->height; row++) {
			assert(panel->displayed[row * panel->max_line_len + 2] == 40);
		}
		fixture_destroy(&fixture);
	}
}

int main(void)
{
	test_continuous_updates_reach_every_row();
	test_full_update_batches();
	test_latest_generation_wins();
	test_more_than_two_batches_progress();
	test_failed_batch_remains_dirty();
	test_invalidate_requires_new_full_conversion();
	test_dual_panel_round_robin();
	test_worker_drains_more_than_two_batches();
	test_worker_retries_without_new_damage();
	test_worker_retry_budget_and_shutdown();
	test_ready_slots_follow_sequence();
	test_controller_row_boundaries();
	test_color_mode_changes_update_transfer_lengths();
	test_row_larger_than_controller_limit();
	test_pause_resume_lock_ownership();
	test_wait_idle_releases_lock_after_error();
	puts("all TX pipeline cases passed");
	return 0;
}
