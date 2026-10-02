// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Issue a standard DRM_IOCTL_MODE_DIRTYFB request for one rectangle.
 */

#define _POSIX_C_SOURCE 200809L

#include <drm/drm.h>
#include <drm/drm_mode.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int parse_coordinate(const char *text, uint16_t *value)
{
	char *end;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno || *text == '\0' || *end != '\0' || parsed > UINT16_MAX) {
		return -1;
	}
	*value = (uint16_t)parsed;
	return 0;
}

int main(int argc, char **argv)
{
	struct drm_mode_fb_dirty_cmd dirty = { 0 };
	struct drm_mode_card_res resources = { 0 };
	struct drm_clip_rect clip;
	uint32_t *framebuffers = NULL;
	uint32_t *crtcs = NULL;
	uint32_t framebuffer_id = 0;
	uint32_t framebuffer_capacity;
	uint32_t crtc_capacity;
	uint32_t crtc_count;
	uint32_t i;
	int fd;
	int rc = 1;

	if (argc != 6) {
		fprintf(stderr, "Usage: %s CARD X1 Y1 X2 Y2\n", argv[0]);
		return 2;
	}
	if (parse_coordinate(argv[2], &clip.x1)
		|| parse_coordinate(argv[3], &clip.y1)
		|| parse_coordinate(argv[4], &clip.x2)
		|| parse_coordinate(argv[5], &clip.y2)
		|| clip.x1 >= clip.x2 || clip.y1 >= clip.y2) {
		fprintf(stderr, "Invalid dirty rectangle\n");
		return 2;
	}

	fd = open(argv[1], O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", argv[1], strerror(errno));
		return 1;
	}

	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &resources) < 0) {
		fprintf(stderr, "DRM_IOCTL_MODE_GETRESOURCES: %s\n",
			strerror(errno));
		goto out_close;
	}
	framebuffer_capacity = resources.count_fbs;
	crtc_capacity = resources.count_crtcs;
	if (framebuffer_capacity) {
		framebuffers = calloc(framebuffer_capacity, sizeof(*framebuffers));
		if (!framebuffers) {
			fprintf(stderr, "calloc: %s\n", strerror(errno));
			goto out_close;
		}
		resources.fb_id_ptr = (uintptr_t)framebuffers;
	}
	if (crtc_capacity) {
		crtcs = calloc(crtc_capacity, sizeof(*crtcs));
		if (!crtcs) {
			fprintf(stderr, "calloc: %s\n", strerror(errno));
			goto out_free;
		}
		resources.crtc_id_ptr = (uintptr_t)crtcs;
	}
	resources.count_connectors = 0;
	resources.count_encoders = 0;
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &resources) < 0) {
		fprintf(stderr, "DRM_IOCTL_MODE_GETRESOURCES: %s\n",
			strerror(errno));
		goto out_free;
	}

	crtc_count = resources.count_crtcs < crtc_capacity
		? resources.count_crtcs : crtc_capacity;
	for (i = 0; !framebuffer_id && i < crtc_count; i++) {
		struct drm_mode_crtc crtc = { .crtc_id = crtcs[i] };

		if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) == 0
			&& crtc.fb_id) {
			framebuffer_id = crtc.fb_id;
		}
	}
	if (!framebuffer_id && resources.count_fbs && framebuffer_capacity) {
		framebuffer_id = framebuffers[0];
	}
	if (!framebuffer_id) {
		fprintf(stderr, "No DRM framebuffer is active\n");
		goto out_free;
	}

	dirty.fb_id = framebuffer_id;
	dirty.num_clips = 1;
	dirty.clips_ptr = (uintptr_t)&clip;
	if (ioctl(fd, DRM_IOCTL_MODE_DIRTYFB, &dirty) < 0) {
		fprintf(stderr, "DRM_IOCTL_MODE_DIRTYFB: %s\n",
			strerror(errno));
		goto out_free;
	}

	printf("fb_id=%u dirty=%u,%u-%u,%u\n", dirty.fb_id,
		clip.x1, clip.y1, clip.x2, clip.y2);
	rc = 0;

out_free:
	free(crtcs);
	free(framebuffers);
out_close:
	close(fd);
	return rc;
}
