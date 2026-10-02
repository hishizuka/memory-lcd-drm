// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DRM driver for 2.7" Sharp Memory LCD
 *
 * Copyright 2023 Andrew D'Angelo
 */

/* Display commands and timed inversion, with DRM lifetime guards. */

#include "sharp_drm.h"

#include <linux/jiffies.h>

#include <drm/drm_managed.h>

static void sharp_drm_cancel_invert_blink(struct sharp_memory_device *sdev)
{
	if (!sdev) {
		return;
	}

	mutex_lock(&sdev->invert_lock);
	sdev->invert_blink_active = false;
	sdev->invert_blink_state = false;
	mutex_unlock(&sdev->invert_lock);
	cancel_delayed_work_sync(&sdev->invert_work);
}

void sharp_drm_disallow_invert_blink(struct sharp_memory_device *sdev)
{
	mutex_lock(&sdev->invert_lock);
	sdev->invert_blink_allowed = false;
	mutex_unlock(&sdev->invert_lock);
	sharp_drm_cancel_invert_blink(sdev);
}

static int sharp_drm_send_display_mode_device(struct sharp_memory_device *sdev,
	u8 mode)
{
	int drm_idx;
	int rc = 0;
	int i;

	if (!sdev) {
		return -ENODEV;
	}

	// Enter DRM resource area
	if (!drm_dev_enter(&sdev->drm, &drm_idx)) {
		return -ENODEV;
	}

	mutex_lock(&sdev->tx_control_lock);
	rc = sharp_tx_pause_locked(sdev);
	if (rc) {
		goto out_resume;
	}
	if (!sdev->tx_running || READ_ONCE(sdev->shutting_down)) {
		rc = -ESHUTDOWN;
		goto out_resume;
	}

	for (i = 0; i < sdev->panel_count; i++) {
		int panel_rc = sharp_spi_set_mode(&sdev->panels[i], mode);

		if (panel_rc && !rc) {
			rc = panel_rc;
		}
	}

out_resume:
	sharp_tx_resume_locked(sdev);
	mutex_unlock(&sdev->tx_control_lock);

	drm_dev_exit(drm_idx);

	return rc;
}

static void sharp_drm_invert_work(struct work_struct *work)
{
	struct sharp_memory_device *sdev =
		container_of(work, struct sharp_memory_device, invert_work.work);
	unsigned long now = jiffies;
	unsigned long remaining;
	unsigned long delay;
	u8 cmd;
	int rc;

	mutex_lock(&sdev->invert_lock);
	if (!sdev->invert_blink_active || !sdev->invert_blink_allowed) {
		mutex_unlock(&sdev->invert_lock);
		return;
	}

	if (time_after_eq(now, sdev->invert_end_jiffies)) {
		sdev->invert_blink_active = false;
		sdev->invert_blink_state = false;
		mutex_unlock(&sdev->invert_lock);
		/*
		 * Worker context must not take modeset locks. INVERT is a panel
		 * display mode and panel RAM remains valid, so ending blink only
		 * needs a NO_UPDATE mode command.
		 */
		sharp_drm_send_display_mode_device(sdev,
			LPM027M128B_MODE_NO_UPDATE);
		return;
	}

	sdev->invert_blink_state = !sdev->invert_blink_state;
	cmd = sdev->invert_blink_state
		? LPM027M128B_MODE_INVERT
		: LPM027M128B_MODE_NO_UPDATE;

	remaining = sdev->invert_end_jiffies - now;
	delay = sdev->invert_interval_jiffies;
	mutex_unlock(&sdev->invert_lock);

	rc = sharp_drm_send_display_mode_device(sdev, cmd);

	if (delay > remaining) {
		delay = remaining;
	}
	if (!delay) {
		delay = 1;
	}
	mutex_lock(&sdev->invert_lock);
	if (rc == -ENODEV || rc == -ESHUTDOWN) {
		sdev->invert_blink_active = false;
		sdev->invert_blink_state = false;
	}
	if (sdev->invert_blink_active && sdev->invert_blink_allowed) {
		schedule_delayed_work(&sdev->invert_work, delay);
	}
	mutex_unlock(&sdev->invert_lock);
}

static int sharp_drm_apply_display_command(u8 cmd, bool clear_framebuffer)
{
	struct sharp_memory_device *sdev;
	int drm_idx;
	int rc;

	sdev = sharp_drm_device_get();
	if (!sdev) {
		return -ENODEV;
	}
	if (!drm_dev_enter(&sdev->drm, &drm_idx)) {
		rc = -ENODEV;
		goto out_put;
	}

	sharp_drm_cancel_invert_blink(sdev);
	rc = sharp_drm_send_display_mode_device(sdev, cmd);
	if (!rc && clear_framebuffer) {
		rc = sharp_drm_clear_primary(sdev);
	}
	drm_dev_exit(drm_idx);

out_put:
	sharp_drm_device_put(sdev);
	return rc;
}

int sharp_drm_display_clear(void)
{
	return sharp_drm_apply_display_command(LPM027M128B_MODE_NO_UPDATE, true);
}

int sharp_drm_display_blink(int mode)
{
	u8 cmd;

	switch (mode) {
	case 0:
		cmd = LPM027M128B_MODE_NO_UPDATE;
		break;
	case 1:
		cmd = LPM027M128B_MODE_BLINK_BLACK;
		break;
	case 2:
		cmd = LPM027M128B_MODE_BLINK_WHITE;
		break;
	default:
		return -EINVAL;
	}

	return sharp_drm_apply_display_command(cmd, false);
}

int sharp_drm_display_invert(int enable)
{
	u8 cmd = enable ? LPM027M128B_MODE_INVERT : LPM027M128B_MODE_NO_UPDATE;

	return sharp_drm_apply_display_command(cmd, false);
}

int sharp_drm_display_invert_blink(unsigned int duration_ms, unsigned int interval_ms)
{
	struct sharp_memory_device *sdev;
	unsigned long duration_jiffies;
	unsigned long interval_jiffies;
	unsigned long delay;
	int drm_idx;
	int rc;

	if (!duration_ms || !interval_ms) {
		return -EINVAL;
	}

	interval_jiffies = msecs_to_jiffies(interval_ms);
	if (!interval_jiffies) {
		interval_jiffies = 1;
	}
	duration_jiffies = msecs_to_jiffies(duration_ms);
	if (!duration_jiffies) {
		duration_jiffies = 1;
	}

	sdev = sharp_drm_device_get();
	if (!sdev) {
		return -ENODEV;
	}
	if (!drm_dev_enter(&sdev->drm, &drm_idx)) {
		rc = -ENODEV;
		goto out_put;
	}

	sharp_drm_cancel_invert_blink(sdev);

	mutex_lock(&sdev->invert_lock);
	if (!sdev->invert_blink_allowed || READ_ONCE(sdev->shutting_down)) {
		mutex_unlock(&sdev->invert_lock);
		rc = -ESHUTDOWN;
		goto out_exit;
	}
	sdev->invert_blink_active = true;
	sdev->invert_blink_state = true;
	sdev->invert_interval_jiffies = interval_jiffies;
	sdev->invert_end_jiffies = jiffies + duration_jiffies;
	mutex_unlock(&sdev->invert_lock);

	rc = sharp_drm_send_display_mode_device(sdev, LPM027M128B_MODE_INVERT);
	if (rc) {
		mutex_lock(&sdev->invert_lock);
		sdev->invert_blink_active = false;
		sdev->invert_blink_state = false;
		mutex_unlock(&sdev->invert_lock);
		goto out_exit;
	}

	delay = interval_jiffies;
	if (delay > duration_jiffies) {
		delay = duration_jiffies;
	}
	if (!delay) {
		delay = 1;
	}
	/* Schedule under the same lock that closes admission during disable. */
	mutex_lock(&sdev->invert_lock);
	if (sdev->invert_blink_active && sdev->invert_blink_allowed) {
		schedule_delayed_work(&sdev->invert_work, delay);
		rc = 0;
	} else {
		rc = -ESHUTDOWN;
	}
	mutex_unlock(&sdev->invert_lock);

out_exit:
	drm_dev_exit(drm_idx);
out_put:
	sharp_drm_device_put(sdev);
	return rc;
}

int sharp_drm_control_init(struct sharp_memory_device *sdev)
{
	sdev->invert_blink_active = false;
	sdev->invert_blink_state = false;
	sdev->invert_end_jiffies = 0;
	sdev->invert_interval_jiffies = 0;
	INIT_DELAYED_WORK(&sdev->invert_work, sharp_drm_invert_work);

	return drmm_mutex_init(&sdev->drm, &sdev->invert_lock);
}

void sharp_drm_allow_invert_blink(struct sharp_memory_device *sdev)
{
	mutex_lock(&sdev->invert_lock);
	sdev->invert_blink_allowed = !READ_ONCE(sdev->shutting_down);
	mutex_unlock(&sdev->invert_lock);
}
