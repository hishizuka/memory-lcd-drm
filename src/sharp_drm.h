/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SHARP_DRM_H_
#define SHARP_DRM_H_

#include <linux/atomic.h>
#include <linux/gpio/consumer.h>
#include <linux/hrtimer.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/spi/spi.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_modes.h>
#include <drm/drm_rect.h>
#include <drm/drm_simple_kms_helper.h>

struct device;
struct backlight_device;
struct dentry;
struct pwm_device;

#define SHARP_SPI_FALLBACK_MAX_XFER 65535u
#define SHARP_TX_SLOT_COUNT 2
#define SHARP_PERF_LATENCY_BUCKETS 64
#define LPM027M128B_TRAILER_BYTES 2
#define LPM027M128B_MODE_NO_UPDATE 0x00
#define LPM027M128B_MODE_BLINK_BLACK 0x10
#define LPM027M128B_MODE_INVERT 0x14
#define LPM027M128B_MODE_BLINK_WHITE 0x18
#define LPM027M128B_MODE_CLEAR 0x20

enum sharp_panel_type {
	SHARP_PANEL_TYPE_JDI = 0,
	SHARP_PANEL_TYPE_SHARP_MONO = 1,
};

enum sharp_dither_algo {
	SHARP_DITHER_ALGO_NONE = 0,
	SHARP_DITHER_ALGO_MONO_ORDERED = 1,
	SHARP_DITHER_ALGO_27COLORS = 2,
	SHARP_DITHER_ALGO_125COLORS = 3,
	SHARP_DITHER_ALGO_343COLORS = 4,
	SHARP_DITHER_ALGO_2197COLORS = 5,
	SHARP_DITHER_ALGO_ERRDIFF2 = 6,
	SHARP_DITHER_ALGO_COUNT,
};

#define SHARP_SUBPANELS_MAX 2

struct sharp_render_params {
	int colors;
	int mono_cutoff;
	int mono_invert;
	int dither_algo;
	int blue_noise;
};

enum sharp_tx_slot_state {
	SHARP_TX_SLOT_FREE = 0,
	SHARP_TX_SLOT_READY,
	SHARP_TX_SLOT_SENDING,
};

struct sharp_memory_device;

struct sharp_tx_slot {
	struct work_struct work;
	struct sharp_memory_device *sdev;
	unsigned char *buf;
	unsigned int *rows;
	u32 *generations;
	size_t capacity;
	size_t len;
	size_t line_len;
	unsigned int row_capacity;
	unsigned int row_count;
	unsigned int panel_index;
	u64 sequence;
	u64 ready_ns;
	enum sharp_tx_slot_state state;
	struct spi_transfer xfers[2];
};

struct sharp_perf_stats {
	atomic64_t update_calls;
	atomic64_t update_ns;
	atomic64_t update_max_ns;
	atomic64_t convert_ns;
	atomic64_t compare_ns;
	atomic64_t pack_ns;
	atomic64_t packed_bytes;
	atomic64_t spi_ns;
	atomic64_t spi_max_ns;
	atomic64_t queue_wait_ns;
	atomic64_t queue_wait_max_ns;
	atomic64_t candidate_rows;
	atomic64_t sent_rows;
	atomic64_t skipped_rows;
	atomic64_t tx_bytes;
	atomic64_t tx_messages;
	atomic64_t tx_transfers;
	atomic64_t slot_stalls;
	atomic64_t max_slots_used;
	atomic64_t coalesced_rows;
	atomic64_t throttled_updates;
	atomic64_t spi_errors;
	atomic64_t full_refreshes;
	atomic64_t update_latency[SHARP_PERF_LATENCY_BUCKETS];
};

struct sharp_subpanel {
	struct spi_device *spi;
	struct mutex cmd_lock;
	bool spi_ref_held;
	unsigned int height;
	unsigned int width;
	unsigned int x_offset;
	enum sharp_panel_type panel_type;
	size_t line_len_mono;
	size_t line_len_color8;
	size_t line_len_color64;
	size_t max_line_len;

	unsigned char *buf;
	struct spi_transfer cmd_xfers[1];
	unsigned char *cmd_buf;
	unsigned char *desired;
	unsigned char *displayed;
	u16 *displayed_line_len;
	u32 *row_generation;
	u32 *queued_generation;
	unsigned long *dirty_rows;
	unsigned long *displayed_rows;
	s16 *dither_err;
	u8 *dither_line_lo;
	u8 *dither_line_hi;
	u32 *source_row;
	s32 nv12_y_scaled[256];
	s32 nv12_r_chroma[256];
	s32 nv12_g_u_chroma[256];
	s32 nv12_g_v_chroma[256];
	s32 nv12_b_chroma[256];
	size_t row_store_len;
	size_t desired_line_len;
	size_t tx_max_payload;
	unsigned int tx_next_row;
	bool desired_valid;
	bool displayed_valid;
	struct sharp_render_params desired_params;
};

struct sharp_memory_device {
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;
	const struct drm_display_mode *mode;
	struct drm_connector connector;

	struct hrtimer vcom_timer;
	bool vcom_timer_active;
	u8 vcom_setting;

	struct mutex fb_lock;
	struct mutex tx_control_lock;
	struct workqueue_struct *tx_wq;
	struct delayed_work tx_retry_work;
	unsigned int tx_retry_count;
	bool tx_retry_blocked;
	wait_queue_head_t tx_slot_wait;
	struct sharp_tx_slot tx_slots[SHARP_TX_SLOT_COUNT];
	u64 tx_sequence;
	u8 tx_next_panel;
	bool tx_running;
	bool tx_paused;
	bool tx_shutdown;
	bool shutting_down;
	int tx_last_error;

	struct gpio_desc *gpio_disp;
	struct gpio_desc *gpio_vcom;
	struct backlight_device *backlight;
	struct pwm_device *backlight_pwm;
	struct delayed_work backlight_work;
	struct mutex backlight_lock;
	bool backlight_forced_off;
	bool backlight_pwm_applied;
	bool backlight_startup_pending;
	bool backlight_transition_active;
	u32 backlight_current_level;
	u32 backlight_start_level;
	u32 backlight_target_level;
	unsigned long backlight_transition_start_jiffies;

	/*
	 * Locking rules:
	 * - The global DRM registry mutex in registry.c may only be held while
	 *   taking tx_control_lock, fb_lock, or invert_lock. It must not be held
	 *   across modeset locks or SPI I/O.
	 * - Taking the registry mutex briefly while modeset locks are held is
	 *   allowed because no path holds the registry mutex and then takes a
	 *   modeset lock.
	 * - tx_control_lock may be followed by fb_lock. The TX workers only take
	 *   fb_lock and never take tx_control_lock.
	 * - fb_lock, invert_lock, and backlight_lock are otherwise leaf locks.
	 *   invert_lock protects only display inversion blink state and must
	 *   not be held while taking fb_lock.
	 * - cmd_lock serializes each panel's reusable command buffer and SPI
	 *   transfers. No path takes another lock while holding cmd_lock.
	 * - fb_lock, invert_lock, and backlight_lock are not held across SPI I/O.
	 *
	 * One-shot control paths resolve the device under the registry mutex,
	 * take a DRM device reference, drop the registry mutex, and only then run
	 * modeset or SPI operations.
	 */
	bool dual_panel;
	u8 panel_count;
	unsigned int logical_width;
	unsigned int logical_height;

	struct sharp_subpanel panels[SHARP_SUBPANELS_MAX];

	struct delayed_work invert_work;
	struct mutex invert_lock;
	bool invert_blink_active;
	bool invert_blink_allowed;
	bool invert_blink_state;
	unsigned long invert_end_jiffies;
	unsigned long invert_interval_jiffies;

	struct sharp_perf_stats perf;
	struct dentry *debugfs_dir;
};

static inline struct sharp_memory_device *drm_to_device(struct drm_device *drm)
{
	return container_of(drm, struct sharp_memory_device, drm);
}

extern int g_param_mono_cutoff;
extern int g_param_mono_invert;
extern int g_param_colors;
extern int g_param_dither_algo;
extern int g_param_dither_algo_user;
extern int g_param_blue_noise_2197;

void params_read_colors(struct device *dev, bool sharp_mono);
void params_force_colors(struct device *dev, int colors, const char *reason);
int params_current_colors(void);
int params_set_mono_invert(int setting);

int sharp_drm_probe(struct spi_device *spi);
void sharp_drm_remove(struct spi_device *spi);
void sharp_drm_shutdown(struct spi_device *spi);
void sharp_drm_register_secondary_spi(struct spi_device *spi);
void sharp_drm_unregister_secondary_spi(struct spi_device *spi);

/* Returns a referenced device; release it with sharp_drm_device_put(). */
struct sharp_memory_device *sharp_drm_device_get(void);
void sharp_drm_device_put(struct sharp_memory_device *sdev);
int sharp_drm_register_device(struct sharp_memory_device *sdev);
void sharp_drm_unregister_device(struct sharp_memory_device *sdev);
bool sharp_drm_device_is_registered(struct sharp_memory_device *sdev);
struct spi_device *sharp_drm_secondary_spi_get(void);

/* Caller holds a device reference and has entered the DRM resource area. */
int sharp_drm_redraw_primary(struct sharp_memory_device *sdev, int height);
int sharp_drm_clear_primary(struct sharp_memory_device *sdev);
int sharp_drm_redraw_fb(struct drm_device *drm, int height);

int sharp_drm_control_init(struct sharp_memory_device *sdev);
void sharp_drm_allow_invert_blink(struct sharp_memory_device *sdev);
void sharp_drm_disallow_invert_blink(struct sharp_memory_device *sdev);
int sharp_drm_display_clear(void);
int sharp_drm_display_blink(int mode);
int sharp_drm_display_invert(int enable);
int sharp_drm_display_invert_blink(unsigned int duration_ms,
	unsigned int interval_ms);
const char *sharp_drm_panel_type_name(void);
/* Caller must hold sharp_drm_render_params_lock(). */
int sharp_drm_validate_colors(int colors);
struct sharp_memory_device *sharp_drm_render_params_lock(void);
void sharp_drm_render_params_unlock(struct sharp_memory_device *sdev);

int sharp_drm_backlight_state_init(struct sharp_memory_device *sdev);
int sharp_drm_init_backlight(struct device *dev, struct sharp_memory_device *sdev);
void sharp_drm_backlight_force_off(struct sharp_memory_device *sdev);
void sharp_drm_backlight_restore(struct sharp_memory_device *sdev);
void sharp_drm_cancel_backlight_transition(struct sharp_memory_device *sdev);

int sharp_memory_set_invert(int setting);

struct sharp_render_params sharp_render_params_snapshot(void);
bool sharp_render_params_equal(const struct sharp_render_params *a,
	const struct sharp_render_params *b);
size_t sharp_render_panel_line_len(const struct sharp_subpanel *panel,
	const struct sharp_render_params *params);
void sharp_render_init_nv12_tables(struct sharp_subpanel *panel);
int sharp_render_clip(struct sharp_subpanel *panel,
	const struct sharp_render_params *params, size_t *result_len, u8 *buf,
	struct drm_framebuffer *fb, const struct drm_rect *clip);

int sharp_spi_clear_screen(struct sharp_subpanel *panel);
int sharp_spi_set_mode(struct sharp_subpanel *panel, u8 mode);
size_t sharp_spi_max_payload(struct sharp_subpanel *panel);
int sharp_spi_write_tagged_batch(struct sharp_subpanel *panel,
	const void *line_data, size_t len, const void *trailer,
	struct spi_transfer xfers[2]);

int sharp_tx_init(struct sharp_memory_device *sdev);
void sharp_tx_start(struct sharp_memory_device *sdev);
void sharp_tx_stop(struct sharp_memory_device *sdev);
void sharp_tx_shutdown(struct sharp_memory_device *sdev);
/* Caller holds tx_control_lock across pause, control I/O, and resume. */
int sharp_tx_pause_locked(struct sharp_memory_device *sdev);
void sharp_tx_resume_locked(struct sharp_memory_device *sdev);
int sharp_tx_wait_idle(struct sharp_memory_device *sdev);
void sharp_tx_invalidate_all_locked(struct sharp_memory_device *sdev);
int sharp_tx_store_converted_locked(struct sharp_memory_device *sdev,
	struct sharp_subpanel *panel, const struct sharp_render_params *params,
	const struct drm_rect *clip, size_t buf_len);
void sharp_tx_kick_locked(struct sharp_memory_device *sdev);

void sharp_perf_reset(struct sharp_memory_device *sdev);
bool sharp_perf_enabled(void);
void sharp_perf_add(atomic64_t *counter, u64 value);
void sharp_perf_update_max(atomic64_t *counter, u64 value);
void sharp_perf_record_update(struct sharp_memory_device *sdev, u64 value);
void sharp_perf_debugfs_init(struct sharp_memory_device *sdev);
void sharp_perf_debugfs_cleanup(struct sharp_memory_device *sdev);

int sharp_drm_fb_dirty(struct drm_framebuffer *fb,
	const struct drm_rect *dirty_rect);

#endif
