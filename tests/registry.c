// SPDX-License-Identifier: GPL-2.0-or-later
/* Concurrent publication and module parameter writes using real mutexes. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define TEST_REAL_MUTEX
#include "../src/registry.c"

static int colors;
static unsigned int property_reads;
static atomic_bool stop_writer;
static atomic_uint writes;

void params_read_colors(struct device *dev, bool mono)
{
	(void)dev;
	/* Reading properties must occur inside the same lock as publication. */
	assert(pthread_mutex_trylock(&g_registry.lock.native) == EBUSY);
	property_reads++;
	colors = mono ? 2 : 8;
}

void params_force_colors(struct device *dev, int forced, const char *reason)
{
	(void)dev;
	(void)reason;
	assert(pthread_mutex_trylock(&g_registry.lock.native) == EBUSY);
	colors = forced;
}

int sharp_drm_redraw_primary(struct sharp_memory_device *sdev, int height)
{
	(void)sdev;
	return height;
}

static void *write_colors(void *unused)
{
	static const int values[] = { 2, 8, 64 };
	unsigned int i = 0;
	(void)unused;
	while (!atomic_load(&stop_writer)) {
		struct sharp_memory_device *lock = sharp_drm_render_params_lock();
		int value = values[i++ % ARRAY_SIZE(values)];

		if (!sharp_drm_validate_colors(value)) {
			colors = value;
		}
		if (lock && (lock->dual_panel ||
			lock->panels[0].panel_type == SHARP_PANEL_TYPE_SHARP_MONO)) {
			assert(colors == sharp_drm_forced_colors(lock));
		}
		sharp_drm_render_params_unlock(lock);
		atomic_fetch_add(&writes, 1);
	}
	return NULL;
}

static void init_device(struct sharp_memory_device *sdev)
{
	memset(sdev, 0, sizeof(*sdev));
	assert(pthread_mutex_init(&sdev->tx_control_lock.native, NULL) == 0);
	assert(pthread_mutex_init(&sdev->fb_lock.native, NULL) == 0);
}

int main(void)
{
	struct sharp_memory_device mono, dual, other;
	pthread_t writer;
	unsigned int i;

	init_device(&mono);
	init_device(&dual);
	init_device(&other);
	mono.panels[0].panel_type = SHARP_PANEL_TYPE_SHARP_MONO;
	dual.dual_panel = true;
	assert(pthread_create(&writer, NULL, write_colors, NULL) == 0);
	while (atomic_load(&writes) < 100) {
		sched_yield();
	}
	for (i = 0; i < 1000; i++) {
		struct sharp_memory_device *sdev = i & 1 ? &mono : &dual;
		struct sharp_memory_device *lock;
		unsigned int reads;

		assert(sharp_drm_register_device(sdev) == 0);
		lock = sharp_drm_render_params_lock();
		assert(lock == sdev && colors == sharp_drm_forced_colors(sdev));
		sharp_drm_render_params_unlock(lock);
		reads = property_reads;
		assert(sharp_drm_register_device(sdev) == 0);
		assert(property_reads == reads);
		assert(sharp_drm_register_device(&other) == -EBUSY);
		assert(property_reads == reads);
		assert(sharp_drm_device_is_registered(sdev));
		assert(sharp_drm_redraw_fb(&sdev->drm, 7) == 7);
		sharp_drm_unregister_device(&other);
		assert(sharp_drm_device_is_registered(sdev));
		sharp_drm_unregister_device(sdev);
		assert(sharp_drm_device_get() == NULL);
		assert(sharp_drm_redraw_fb(&sdev->drm, 7) == -ENODEV);
	}
	atomic_store(&stop_writer, true);
	assert(pthread_join(writer, NULL) == 0);
	assert(property_reads == 1000);
	puts("all concurrent registry cases passed");
	return 0;
}
