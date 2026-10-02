#ifndef TESTS_STUBS_DRM_DRM_FOURCC_H_
#define TESTS_STUBS_DRM_DRM_FOURCC_H_

#include <linux/types.h>

#define fourcc_code(a, b, c, d) \
	((u32)(a) | ((u32)(b) << 8) | ((u32)(c) << 16) | ((u32)(d) << 24))

#define DRM_FORMAT_XRGB8888 fourcc_code('X', 'R', '2', '4')
#define DRM_FORMAT_NV12 fourcc_code('N', 'V', '1', '2')
#define DRM_FORMAT_MAX_PLANES 4

struct drm_format_info {
	u32 format;
};

#endif
