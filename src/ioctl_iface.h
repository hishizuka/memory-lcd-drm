/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IOCTL_IFACE_H_
#define IOCTL_IFACE_H_

#include <drm/drm_ioctl.h>

// No parameters, callable from kernel space
#define DRM_SHARP_REDRAW 0x00

#define DRM_IOCTL_SHARP_REDRAW \
	DRM_IO(DRM_COMMAND_BASE + DRM_SHARP_REDRAW)

#define DRM_IOCTL_DEF_DRV_REDRAW \
	DRM_IOCTL_DEF_DRV(SHARP_REDRAW, sharp_drm_ioctl_redraw, DRM_RENDER_ALLOW)

#endif
