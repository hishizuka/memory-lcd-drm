#ifndef TESTS_STUBS_DRM_DRM_FRAMEBUFFER_H_
#define TESTS_STUBS_DRM_DRM_FRAMEBUFFER_H_

#include <linux/types.h>
#include <drm/drm_fourcc.h>

struct drm_device;
struct drm_gem_dma_object;

struct drm_framebuffer {
	struct drm_device *dev;
	unsigned int width;
	unsigned int height;
	unsigned int pitches[4];
	unsigned int offsets[4];
	const struct drm_format_info *format;
	struct drm_gem_dma_object *obj;
	struct drm_gem_dma_object *objs[4];
};

#endif
