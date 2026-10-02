#ifndef TESTS_STUBS_DRM_DRM_FORMAT_HELPER_H_
#define TESTS_STUBS_DRM_DRM_FORMAT_HELPER_H_

#include <linux/types.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_rect.h>

struct iosys_map {
	void *vaddr;
};

struct drm_format_conv_state {
	int unused;
};

static inline void iosys_map_set_vaddr(struct iosys_map *map, void *vaddr)
{
	map->vaddr = vaddr;
}

static inline void drm_format_conv_state_init(struct drm_format_conv_state *state)
{
	(void)state;
}

static inline void drm_format_conv_state_release(struct drm_format_conv_state *state)
{
	(void)state;
}

static inline void drm_fb_xrgb8888_to_gray8(struct iosys_map *dst,
	const unsigned int *dst_pitch, const struct iosys_map *src,
	struct drm_framebuffer *fb, const struct drm_rect *clip,
	struct drm_format_conv_state *state)
{
	u8 *out = (u8 *)dst->vaddr;
	const u8 *base = (const u8 *)src->vaddr;
	int width = clip->x2 - clip->x1;
	int height = clip->y2 - clip->y1;
	int y;

	(void)dst_pitch;
	(void)state;

	for (y = 0; y < height; y++) {
		const u32 *line = (const u32 *)(base + ((clip->y1 + y) * fb->pitches[0])
			+ (clip->x1 * 4));
		int x;

		for (x = 0; x < width; x++) {
			u32 px = line[x];
			u8 r = (u8)(px >> 16);
			u8 g = (u8)(px >> 8);
			u8 b = (u8)px;

			out[(y * width) + x] = (u8)(((u32)r * 77u
				+ (u32)g * 150u + (u32)b * 29u) >> 8);
		}
	}
}

#endif
