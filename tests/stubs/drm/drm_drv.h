#ifndef TESTS_STUBS_DRM_DRM_DRV_H_
#define TESTS_STUBS_DRM_DRM_DRV_H_

struct device {
	int unused;
};

struct drm_device {
	struct device *dev;
};

static inline void drm_dev_get(struct drm_device *dev)
{
	(void)dev;
}

static inline void drm_dev_put(struct drm_device *dev)
{
	(void)dev;
}

static inline int drm_dev_enter(struct drm_device *dev, int *idx)
{
	(void)dev;
	*idx = 0;
	return 1;
}

static inline void drm_dev_exit(int idx)
{
	(void)idx;
}

#endif
