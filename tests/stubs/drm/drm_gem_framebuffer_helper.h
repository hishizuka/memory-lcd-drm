#ifndef TESTS_STUBS_DRM_DRM_GEM_FRAMEBUFFER_HELPER_H_
#define TESTS_STUBS_DRM_DRM_GEM_FRAMEBUFFER_HELPER_H_

#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_format_helper.h>

static inline int drm_gem_fb_vmap(struct drm_framebuffer *fb,
	struct iosys_map *map, struct iosys_map *data)
{
	unsigned int i;

	for (i = 0; i < DRM_FORMAT_MAX_PLANES; i++) {
		struct drm_gem_dma_object *obj = drm_fb_dma_get_gem_obj(fb, i);

		if (!obj) {
			continue;
		}
		iosys_map_set_vaddr(&map[i], obj->vaddr);
		iosys_map_set_vaddr(&data[i],
			(u8 *)obj->vaddr + fb->offsets[i]);
	}
	return 0;
}

static inline void drm_gem_fb_vunmap(struct drm_framebuffer *fb,
	struct iosys_map *map)
{
	(void)fb;
	(void)map;
}

#endif
