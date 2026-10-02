#ifndef TESTS_STUBS_DRM_DRM_MANAGED_H_
#define TESTS_STUBS_DRM_DRM_MANAGED_H_

#include <stddef.h>
#include <stdlib.h>

#include <drm/drm_drv.h>

#ifndef GFP_KERNEL
#define GFP_KERNEL 0
#endif

static inline void *drmm_kzalloc(struct drm_device *drm, size_t size, int flags)
{
	(void)drm;
	(void)flags;
	return calloc(1, size);
}

static inline void *drmm_kcalloc(struct drm_device *drm, size_t count,
	size_t size, int flags)
{
	(void)drm;
	(void)flags;
	return calloc(count, size);
}

static inline int drmm_add_action_or_reset(struct drm_device *drm,
	void (*action)(void *), void *data)
{
	(void)drm;
	(void)action;
	(void)data;
	return 0;
}

#define drmm_alloc_ordered_workqueue(drm, name, flags) \
	alloc_ordered_workqueue((name), (flags))

#endif
