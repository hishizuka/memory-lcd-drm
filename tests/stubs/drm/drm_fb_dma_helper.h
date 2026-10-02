#ifndef TESTS_STUBS_DRM_DRM_FB_DMA_HELPER_H_
#define TESTS_STUBS_DRM_DRM_FB_DMA_HELPER_H_

#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>

static inline struct drm_gem_dma_object *drm_fb_dma_get_gem_obj(
	struct drm_framebuffer *fb, unsigned int plane)
{
	return fb->objs[plane] ? fb->objs[plane] : fb->obj;
}

static inline int drm_gem_fb_begin_cpu_access(struct drm_framebuffer *fb,
	int direction)
{
	(void)fb;
	(void)direction;
	return 0;
}

static inline void drm_gem_fb_end_cpu_access(struct drm_framebuffer *fb,
	int direction)
{
	(void)fb;
	(void)direction;
}

#endif
