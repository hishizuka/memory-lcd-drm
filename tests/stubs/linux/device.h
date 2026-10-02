#ifndef TESTS_STUBS_LINUX_DEVICE_H_
#define TESTS_STUBS_LINUX_DEVICE_H_
#include <drm/drm_drv.h>
static inline void get_device(struct device *dev) { (void)dev; }
static inline void put_device(struct device *dev) { (void)dev; }
#endif
