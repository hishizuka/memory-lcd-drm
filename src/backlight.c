// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DRM driver for 2.7" Sharp Memory LCD
 *
 * Copyright 2023 Andrew D'Angelo
 */

/* Optional PWM backlight registration and brightness transitions. */

#include "sharp_drm.h"

#include <linux/backlight.h>
#include <linux/device.h>
#include <linux/fb.h>
#include <linux/jiffies.h>
#include <linux/math64.h>
#include <linux/property.h>
#include <linux/pwm.h>

#include <drm/drm_managed.h>

#define SHARP_BACKLIGHT_MAX_BRIGHTNESS 255
#define SHARP_BACKLIGHT_DEFAULT_BRIGHTNESS 255
#define SHARP_BACKLIGHT_DEFAULT_PERIOD_NS 15625000ULL
#define SHARP_BACKLIGHT_TRANSITION_MS 300U
#define SHARP_BACKLIGHT_UPDATE_MS 10U
#define SHARP_BACKLIGHT_EASING_SCALE 1024U
static unsigned int sharp_drm_backlight_target_brightness_locked(
	struct sharp_memory_device *sdev)
{
	if (!sdev->backlight || READ_ONCE(sdev->backlight_forced_off)) {
		return 0;
	}

	return backlight_get_brightness(sdev->backlight);
}

static int sharp_drm_apply_backlight_level_locked(
	struct sharp_memory_device *sdev, u32 level)
{
	struct pwm_args args;
	struct pwm_state state;
	u64 duty_cycle;
	u64 max_level = (u64)SHARP_BACKLIGHT_MAX_BRIGHTNESS *
		SHARP_BACKLIGHT_EASING_SCALE;
	int ret;

	if (!sdev->backlight_pwm) {
		return -ENODEV;
	}

	pwm_get_state(sdev->backlight_pwm, &state);
	pwm_get_args(sdev->backlight_pwm, &args);
	if (!state.period) {
		if (args.period) {
			state.period = args.period;
			state.polarity = args.polarity;
		} else {
			state.period = SHARP_BACKLIGHT_DEFAULT_PERIOD_NS;
		}
	}

	duty_cycle = DIV_ROUND_CLOSEST_ULL(state.period * level, max_level);
	if (duty_cycle > state.period) {
		duty_cycle = state.period;
	}

	state.duty_cycle = duty_cycle;
	/* Keep 0% duty active so the PWM pin is driven to the inactive level. */
	state.enabled = true;

	ret = pwm_apply_might_sleep(sdev->backlight_pwm, &state);
	if (!ret) {
		sdev->backlight_current_level = level;
		sdev->backlight_pwm_applied = true;
	}

	return ret;
}

static u32 sharp_drm_backlight_eased_progress(unsigned int elapsed_ms)
{
	u32 progress = (u32)(((u64)elapsed_ms *
		SHARP_BACKLIGHT_EASING_SCALE) / SHARP_BACKLIGHT_TRANSITION_MS);
	u64 progress_squared = (u64)progress * progress;

	/* Smoothstep: 3t^2 - 2t^3. */
	return (u32)((progress_squared *
		(3U * SHARP_BACKLIGHT_EASING_SCALE - 2U * progress)) /
		((u64)SHARP_BACKLIGHT_EASING_SCALE *
		 SHARP_BACKLIGHT_EASING_SCALE));
}

static u32 sharp_drm_backlight_interpolate_level(
	const struct sharp_memory_device *sdev, u32 progress)
{
	if (sdev->backlight_target_level >= sdev->backlight_start_level) {
		return sdev->backlight_start_level +
			(u32)(((u64)(sdev->backlight_target_level -
				sdev->backlight_start_level) * progress) /
				SHARP_BACKLIGHT_EASING_SCALE);
	}

	return sdev->backlight_start_level -
		(u32)(((u64)(sdev->backlight_start_level -
			sdev->backlight_target_level) * progress) /
			SHARP_BACKLIGHT_EASING_SCALE);
}

static void sharp_drm_backlight_work(struct work_struct *work)
{
	struct sharp_memory_device *sdev = container_of(to_delayed_work(work),
		struct sharp_memory_device, backlight_work);
	unsigned int elapsed_ms;
	u32 level;
	int ret;

	mutex_lock(&sdev->backlight_lock);
	if (!sdev->backlight_transition_active) {
		mutex_unlock(&sdev->backlight_lock);
		return;
	}

	elapsed_ms = jiffies_to_msecs(jiffies -
		sdev->backlight_transition_start_jiffies);
	if (elapsed_ms >= SHARP_BACKLIGHT_TRANSITION_MS) {
		level = sdev->backlight_target_level;
		sdev->backlight_transition_active = false;
	} else {
		level = sharp_drm_backlight_interpolate_level(sdev,
			sharp_drm_backlight_eased_progress(elapsed_ms));
	}

	ret = sharp_drm_apply_backlight_level_locked(sdev, level);
	if (ret) {
		sdev->backlight_transition_active = false;
		dev_warn_ratelimited(sdev->drm.dev,
			"failed to update backlight transition: %d\n", ret);
	} else if (sdev->backlight_transition_active) {
		schedule_delayed_work(&sdev->backlight_work,
			msecs_to_jiffies(SHARP_BACKLIGHT_UPDATE_MS));
	}
	mutex_unlock(&sdev->backlight_lock);
}

static int sharp_drm_set_backlight_target_locked(
	struct sharp_memory_device *sdev, u32 target_level, bool immediate)
{
	sdev->backlight_target_level = target_level;
	if (immediate || !sdev->backlight_pwm_applied) {
		sdev->backlight_transition_active = false;
		cancel_delayed_work(&sdev->backlight_work);
		return sharp_drm_apply_backlight_level_locked(sdev, target_level);
	}

	if (target_level == sdev->backlight_current_level) {
		sdev->backlight_transition_active = false;
		cancel_delayed_work(&sdev->backlight_work);
		return 0;
	}

	sdev->backlight_start_level = sdev->backlight_current_level;
	sdev->backlight_transition_start_jiffies = jiffies;
	sdev->backlight_transition_active = true;
	mod_delayed_work(system_wq, &sdev->backlight_work, 0);

	return 0;
}

static int sharp_drm_backlight_update_status(struct backlight_device *backlight)
{
	struct sharp_memory_device *sdev = bl_get_data(backlight);
	unsigned int brightness;
	u32 target_level;
	bool immediate;
	int ret;

	mutex_lock(&sdev->backlight_lock);
	brightness = sharp_drm_backlight_target_brightness_locked(sdev);
	target_level = brightness * SHARP_BACKLIGHT_EASING_SCALE;
	immediate = READ_ONCE(sdev->backlight_forced_off) ||
		(backlight->props.state & BL_CORE_SUSPENDED);
	if (sdev->backlight_startup_pending &&
		!READ_ONCE(sdev->backlight_forced_off) &&
		!(backlight->props.state & BL_CORE_SUSPENDED)) {
		immediate = brightness == SHARP_BACKLIGHT_MAX_BRIGHTNESS;
		sdev->backlight_startup_pending = false;
	}
	ret = sharp_drm_set_backlight_target_locked(sdev, target_level, immediate);
	mutex_unlock(&sdev->backlight_lock);

	return ret;
}

static int sharp_drm_backlight_get_brightness(struct backlight_device *backlight)
{
	struct sharp_memory_device *sdev = bl_get_data(backlight);
	unsigned int brightness;

	mutex_lock(&sdev->backlight_lock);
	brightness = DIV_ROUND_CLOSEST(sdev->backlight_current_level,
		SHARP_BACKLIGHT_EASING_SCALE);
	mutex_unlock(&sdev->backlight_lock);

	return brightness;
}

static const struct backlight_ops sharp_drm_backlight_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = sharp_drm_backlight_update_status,
	.get_brightness = sharp_drm_backlight_get_brightness,
};

static void sharp_drm_set_backlight_forced_off(struct sharp_memory_device *sdev,
	bool force_off)
{
	int ret;

	if (!sdev || !sdev->backlight_pwm || !sdev->backlight) {
		return;
	}

	WRITE_ONCE(sdev->backlight_forced_off, force_off);
	ret = backlight_update_status(sdev->backlight);

	if (ret) {
		dev_warn(sdev->drm.dev, "failed to update backlight PWM: %d\n", ret);
	}
}

void sharp_drm_backlight_force_off(struct sharp_memory_device *sdev)
{
	sharp_drm_set_backlight_forced_off(sdev, true);
}

void sharp_drm_backlight_restore(struct sharp_memory_device *sdev)
{
	sharp_drm_set_backlight_forced_off(sdev, false);
}

void sharp_drm_cancel_backlight_transition(
	struct sharp_memory_device *sdev)
{
	if (!sdev || !sdev->backlight_pwm) {
		return;
	}

	mutex_lock(&sdev->backlight_lock);
	sdev->backlight_transition_active = false;
	mutex_unlock(&sdev->backlight_lock);
	cancel_delayed_work_sync(&sdev->backlight_work);
}

int sharp_drm_backlight_state_init(struct sharp_memory_device *sdev)
{
	INIT_DELAYED_WORK(&sdev->backlight_work, sharp_drm_backlight_work);

	return drmm_mutex_init(&sdev->drm, &sdev->backlight_lock);
}

int sharp_drm_init_backlight(struct device *dev, struct sharp_memory_device *sdev)
{
	struct backlight_properties props = { 0 };
	u32 default_brightness = SHARP_BACKLIGHT_DEFAULT_BRIGHTNESS;
	int ret;

	/*
	 * Backlight PWM is optional.  Some PWM firmware-node implementations
	 * return -EINVAL, rather than -ENOENT, when no "pwms" property exists.
	 * Check the property explicitly so panels without a backlight do not fail
	 * probe, while still reporting malformed PWM configurations.
	 */
	if (!device_property_present(dev, "pwms")) {
		dev_info(dev, "No backlight PWM configured\n");
		return 0;
	}

	sdev->backlight_pwm = devm_pwm_get(dev, "backlight");
	if (IS_ERR(sdev->backlight_pwm)) {
		ret = PTR_ERR(sdev->backlight_pwm);
		sdev->backlight_pwm = NULL;
		if (ret == -ENOENT || ret == -ENODEV) {
			dev_info(dev, "No backlight PWM configured\n");
			return 0;
		}
		return dev_err_probe(dev, ret, "Failed to get backlight PWM\n");
	}

	if (!device_property_read_u32(dev, "backlight-default-brightness",
		&default_brightness) &&
		default_brightness > SHARP_BACKLIGHT_MAX_BRIGHTNESS) {
		dev_warn(dev, "backlight-default-brightness=%u exceeds %u; clamping\n",
			default_brightness, SHARP_BACKLIGHT_MAX_BRIGHTNESS);
		default_brightness = SHARP_BACKLIGHT_MAX_BRIGHTNESS;
	}

	props.type = BACKLIGHT_RAW;
	props.scale = BACKLIGHT_SCALE_LINEAR;
	props.max_brightness = SHARP_BACKLIGHT_MAX_BRIGHTNESS;
	props.brightness = default_brightness;
	props.power = FB_BLANK_UNBLANK;

	WRITE_ONCE(sdev->backlight_forced_off, true);
	sdev->backlight_startup_pending = true;
	sdev->backlight = devm_backlight_device_register(dev, "backlight", dev,
		sdev, &sharp_drm_backlight_ops, &props);
	if (IS_ERR(sdev->backlight)) {
		ret = PTR_ERR(sdev->backlight);
		sdev->backlight = NULL;
		return dev_err_probe(dev, ret, "Failed to register backlight device\n");
	}

	ret = backlight_update_status(sdev->backlight);
	if (ret) {
		return dev_err_probe(dev, ret, "Failed to initialize backlight device\n");
	}

	dev_info(dev,
		"backlight registered as /sys/class/backlight/backlight (default=%u)\n",
		default_brightness);

	return 0;
}
