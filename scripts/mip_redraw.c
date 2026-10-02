// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Periodically issue DRM dirty notifications for mmap-based fbdev writers.
 *
 * Some 32-bit Raspberry Pi OS configurations do not generate repeated dirty
 * notifications after userspace keeps writing through an existing fbdev mmap.
 * This bridge preserves the driver's normal row comparison and skip logic by
 * using DRM_IOCTL_MODE_DIRTYFB rather than the force-redraw private ioctl.
 */

#define _POSIX_C_SOURCE 200809L

#include <drm/drm.h>
#include <drm/drm_mode.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_FPS 30U
#define MAX_FPS 240U
#define SHARP_FB_NAME "sharp_drmdrmfb"

static volatile sig_atomic_t stop_requested;

struct fb_monitor {
	int fd;
	unsigned char *mapping;
	unsigned char *shadow;
	size_t mapping_length;
	size_t stride;
	size_t visible_bytes;
	unsigned int height;
	bool shadow_valid;
};

static void handle_signal(int signal_number)
{
	(void)signal_number;
	stop_requested = 1;
}

static void sleep_milliseconds(long milliseconds)
{
	struct timespec delay = {
		.tv_sec = milliseconds / 1000,
		.tv_nsec = (milliseconds % 1000) * 1000000L,
	};

	while (!stop_requested && nanosleep(&delay, &delay) < 0
		&& errno == EINTR) {
	}
}

static int find_sharp_fb(char *path, size_t path_size,
	unsigned int *framebuffer_index)
{
	char name[128];
	unsigned int index;
	FILE *file;
	int found = -1;

	file = fopen("/proc/fb", "r");
	if (!file) {
		return -1;
	}
	while (fscanf(file, "%u %127[^\n]\n", &index, name) == 2) {
		if (strcmp(name, SHARP_FB_NAME) == 0) {
			snprintf(path, path_size, "/dev/fb%u", index);
			*framebuffer_index = index;
			found = 0;
			break;
		}
	}
	fclose(file);
	if (found < 0) {
		errno = ENODEV;
	}
	return found;
}

static int bind_fbcon(void)
{
	DIR *directory;
	struct dirent *entry;
	int rc = -1;

	directory = opendir("/sys/class/vtconsole");
	if (!directory) {
		return -1;
	}
	while ((entry = readdir(directory)) != NULL) {
		char name_path[256];
		char bind_path[256];
		char name[128] = { 0 };
		FILE *file;
		int length;
		int fd;

		if (strncmp(entry->d_name, "vtcon", 5) != 0) {
			continue;
		}
		length = snprintf(name_path, sizeof(name_path),
			"/sys/class/vtconsole/%s/name", entry->d_name);
		if (length < 0 || (size_t)length >= sizeof(name_path)) {
			continue;
		}
		file = fopen(name_path, "r");
		if (!file) {
			continue;
		}
		if (!fgets(name, sizeof(name), file)) {
			fclose(file);
			continue;
		}
		fclose(file);
		if (!strstr(name, "frame buffer device")) {
			continue;
		}

		length = snprintf(bind_path, sizeof(bind_path),
			"/sys/class/vtconsole/%s/bind", entry->d_name);
		if (length < 0 || (size_t)length >= sizeof(bind_path)) {
			continue;
		}
		fd = open(bind_path, O_WRONLY | O_CLOEXEC);
		if (fd < 0) {
			continue;
		}
		if (write(fd, "1\n", 2) == 2) {
			rc = 0;
		}
		close(fd);
		break;
	}
	closedir(directory);
	return rc;
}

static uint32_t fb_component(unsigned int length, unsigned int offset)
{
	uint32_t value;

	if (!length || offset >= 32) {
		return 0;
	}
	value = length >= 8 ? 0xffU : (1U << length) - 1U;
	return value << offset;
}

static int fill_fb_red(int fd)
{
	struct fb_fix_screeninfo fixed;
	struct fb_var_screeninfo variable;
	uint32_t pixel;
	unsigned char *line;
	size_t pixel_bytes;
	size_t visible_bytes;
	unsigned int x;
	unsigned int y;
	int rc = -1;

	if (ioctl(fd, FBIOGET_FSCREENINFO, &fixed) < 0
		|| ioctl(fd, FBIOGET_VSCREENINFO, &variable) < 0
		|| variable.bits_per_pixel % 8 != 0) {
		return -1;
	}
	pixel_bytes = variable.bits_per_pixel / 8;
	visible_bytes = (size_t)variable.xres * pixel_bytes;
	if (pixel_bytes != sizeof(pixel)
		|| visible_bytes > fixed.line_length) {
		errno = ENOTSUP;
		return -1;
	}
	line = calloc(1, fixed.line_length);
	if (!line) {
		return -1;
	}
	pixel = fb_component(variable.red.length, variable.red.offset);
	for (x = 0; x < variable.xres; x++) {
		memcpy(line + (size_t)x * pixel_bytes, &pixel, sizeof(pixel));
	}
	for (y = 0; y < variable.yres; y++) {
		off_t offset = (off_t)y * fixed.line_length;

		if (pwrite(fd, line, fixed.line_length, offset)
			!= (ssize_t)fixed.line_length) {
			goto out;
		}
	}
	rc = 0;
out:
	free(line);
	return rc;
}

static int initialize_display_red(char *path, size_t path_size)
{
	struct fb_con2fbmap map = { .console = 1 };
	unsigned int framebuffer_index;
	int fd;
	int rc = -1;

	if (find_sharp_fb(path, path_size, &framebuffer_index) < 0) {
		return -1;
	}
	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		return -1;
	}
	map.framebuffer = framebuffer_index;
	if (ioctl(fd, FBIOPUT_CON2FBMAP, &map) < 0) {
		goto out;
	}
	if (bind_fbcon() < 0) {
		goto out;
	}
	sleep_milliseconds(500);
	if (fill_fb_red(fd) < 0) {
		goto out;
	}
	fprintf(stderr, "Initialized %s with red\n", path);
	rc = 0;
out:
	close(fd);
	return rc;
}

static int fb_monitor_open(struct fb_monitor *monitor, const char *path)
{
	struct fb_fix_screeninfo fixed;
	struct fb_var_screeninfo variable;
	size_t mapping_length;

	monitor->fd = open(path, O_RDONLY | O_CLOEXEC);
	if (monitor->fd < 0) {
		return -1;
	}
	if (ioctl(monitor->fd, FBIOGET_FSCREENINFO, &fixed) < 0
		|| ioctl(monitor->fd, FBIOGET_VSCREENINFO, &variable) < 0
		|| variable.bits_per_pixel % 8 != 0) {
		goto fail;
	}
	monitor->stride = fixed.line_length;
	monitor->visible_bytes =
		(size_t)variable.xres * (variable.bits_per_pixel / 8);
	monitor->height = variable.yres;
	if (!monitor->stride || monitor->visible_bytes > monitor->stride
		|| !monitor->height
		|| monitor->height > SIZE_MAX / monitor->stride) {
		errno = EINVAL;
		goto fail;
	}
	mapping_length = monitor->stride * monitor->height;
	if (fixed.smem_len && mapping_length > fixed.smem_len) {
		errno = EINVAL;
		goto fail;
	}
	monitor->mapping = mmap(NULL, mapping_length, PROT_READ,
		MAP_SHARED, monitor->fd, 0);
	if (monitor->mapping == MAP_FAILED) {
		monitor->mapping = NULL;
		goto fail;
	}
	monitor->shadow = malloc(mapping_length);
	if (!monitor->shadow) {
		munmap(monitor->mapping, mapping_length);
		monitor->mapping = NULL;
		goto fail;
	}
	monitor->mapping_length = mapping_length;
	monitor->shadow_valid = false;
	return 0;

fail:
	close(monitor->fd);
	monitor->fd = -1;
	return -1;
}

static void fb_monitor_close(struct fb_monitor *monitor)
{
	if (monitor->mapping) {
		munmap(monitor->mapping, monitor->mapping_length);
	}
	free(monitor->shadow);
	if (monitor->fd >= 0) {
		close(monitor->fd);
	}
}

static bool fb_monitor_damage(struct fb_monitor *monitor,
	unsigned int *first_row, unsigned int *last_row)
{
	unsigned int first = monitor->height;
	unsigned int last = 0;
	unsigned int y;

	for (y = 0; y < monitor->height; y++) {
		size_t offset = (size_t)y * monitor->stride;
		unsigned char *current = monitor->mapping + offset;
		unsigned char *previous = monitor->shadow + offset;

		if (!monitor->shadow_valid
			|| memcmp(current, previous,
				monitor->visible_bytes) != 0) {
			memcpy(previous, current, monitor->visible_bytes);
			if (first == monitor->height) {
				first = y;
			}
			last = y + 1;
		}
	}
	monitor->shadow_valid = true;
	if (first == monitor->height) {
		return false;
	}
	*first_row = first;
	*last_row = last;
	return true;
}

static int is_sharp_drm(int fd)
{
	char name[32] = { 0 };
	struct drm_version version = {
		.name_len = sizeof(name) - 1,
		.name = name,
	};

	if (ioctl(fd, DRM_IOCTL_VERSION, &version) < 0) {
		return 0;
	}
	name[sizeof(name) - 1] = '\0';
	return strcmp(name, "sharp_drm") == 0;
}

static int open_card(const char *requested, char *selected,
	size_t selected_size)
{
	DIR *directory;
	struct dirent *entry;
	int fd;

	if (strcmp(requested, "auto") != 0) {
		fd = open(requested, O_RDWR | O_CLOEXEC);
		if (fd < 0) {
			return -1;
		}
		snprintf(selected, selected_size, "%s", requested);
		return fd;
	}

	directory = opendir("/dev/dri");
	if (!directory) {
		return -1;
	}
	while ((entry = readdir(directory)) != NULL) {
		char path[256];
		int path_length;

		if (strncmp(entry->d_name, "card", 4) != 0) {
			continue;
		}
		path_length = snprintf(path, sizeof(path), "/dev/dri/%s",
			entry->d_name);
		if (path_length < 0
			|| (size_t)path_length >= sizeof(path)) {
			continue;
		}
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0) {
			continue;
		}
		if (is_sharp_drm(fd)) {
			snprintf(selected, selected_size, "%s", path);
			closedir(directory);
			return fd;
		}
		close(fd);
	}
	closedir(directory);
	errno = ENODEV;
	return -1;
}

static int find_framebuffer(int fd, uint32_t *framebuffer_id,
	uint32_t *width, uint32_t *height)
{
	struct drm_mode_card_res resources = { 0 };
	uint32_t *framebuffers = NULL;
	uint32_t *crtcs = NULL;
	uint32_t framebuffer_capacity;
	uint32_t crtc_capacity;
	uint32_t i;
	int rc = -1;

	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &resources) < 0) {
		return -1;
	}
	framebuffer_capacity = resources.count_fbs;
	crtc_capacity = resources.count_crtcs;
	if (framebuffer_capacity) {
		framebuffers = calloc(framebuffer_capacity, sizeof(*framebuffers));
		if (!framebuffers) {
			goto out;
		}
		resources.fb_id_ptr = (uintptr_t)framebuffers;
	}
	if (crtc_capacity) {
		crtcs = calloc(crtc_capacity, sizeof(*crtcs));
		if (!crtcs) {
			goto out;
		}
		resources.crtc_id_ptr = (uintptr_t)crtcs;
	}
	resources.count_connectors = 0;
	resources.count_encoders = 0;
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &resources) < 0) {
		goto out;
	}

	*framebuffer_id = 0;
	for (i = 0; i < resources.count_crtcs && i < crtc_capacity; i++) {
		struct drm_mode_crtc crtc = { .crtc_id = crtcs[i] };

		if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) == 0
			&& crtc.fb_id) {
			*framebuffer_id = crtc.fb_id;
			break;
		}
	}
	if (!*framebuffer_id && resources.count_fbs
		&& framebuffer_capacity) {
		*framebuffer_id = framebuffers[0];
	}
	if (*framebuffer_id) {
		struct drm_mode_fb_cmd2 framebuffer = {
			.fb_id = *framebuffer_id,
		};

		if (ioctl(fd, DRM_IOCTL_MODE_GETFB2, &framebuffer) == 0
			&& framebuffer.width && framebuffer.height) {
			*width = framebuffer.width;
			*height = framebuffer.height;
			rc = 0;
		}
	}

out:
	free(crtcs);
	free(framebuffers);
	return rc;
}

static int mark_dirty(int fd, uint32_t framebuffer_id,
	uint32_t width, uint32_t first_row, uint32_t last_row)
{
	struct drm_clip_rect clip = {
		.x1 = 0,
		.y1 = first_row,
		.x2 = width,
		.y2 = last_row,
	};
	struct drm_mode_fb_dirty_cmd dirty = {
		.fb_id = framebuffer_id,
		.num_clips = 1,
		.clips_ptr = (uintptr_t)&clip,
	};

	return ioctl(fd, DRM_IOCTL_MODE_DIRTYFB, &dirty);
}

static unsigned int parse_fps(const char *text)
{
	char *end;
	unsigned long value;

	errno = 0;
	value = strtoul(text, &end, 10);
	if (errno || *text == '\0' || *end != '\0'
		|| value == 0 || value > MAX_FPS) {
		fprintf(stderr, "Invalid FPS: %s\n", text);
		exit(2);
	}
	return (unsigned int)value;
}

int main(int argc, char **argv)
{
	const char *requested_card = "auto";
	unsigned int fps = DEFAULT_FPS;
	unsigned int refresh_count = 0;
	bool initialize_red = false;
	bool exit_after_initialize = false;
	struct fb_monitor monitor = { .fd = -1 };
	uint32_t framebuffer_id = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	char framebuffer_path[64] = { 0 };
	char selected_card[256] = { 0 };
	struct sigaction action = {
		.sa_handler = handle_signal,
	};
	int fd = -1;

	if (argc > 4) {
		fprintf(stderr,
			"Usage: %s [CARD|auto [FPS [--initialize-red"
			"|--initialize-red-once]]]\n",
			argv[0]);
		return 2;
	}
	if (argc >= 2) {
		requested_card = argv[1];
	}
	if (argc == 3) {
		fps = parse_fps(argv[2]);
	}
	if (argc == 4) {
		fps = parse_fps(argv[2]);
		if (strcmp(argv[3], "--initialize-red") == 0) {
			initialize_red = true;
		} else if (strcmp(argv[3], "--initialize-red-once") == 0) {
			initialize_red = true;
			exit_after_initialize = true;
		} else {
			fprintf(stderr, "Unknown option: %s\n", argv[3]);
			return 2;
		}
	}
	sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);

	while (initialize_red && !stop_requested) {
		if (initialize_display_red(framebuffer_path,
			sizeof(framebuffer_path)) == 0) {
			break;
		}
		sleep_milliseconds(1000);
	}
	while (!stop_requested && framebuffer_path[0] == '\0') {
		unsigned int framebuffer_index;

		if (find_sharp_fb(framebuffer_path, sizeof(framebuffer_path),
			&framebuffer_index) == 0) {
			break;
		}
		sleep_milliseconds(1000);
	}
	while (!stop_requested && monitor.fd < 0) {
		if (fb_monitor_open(&monitor, framebuffer_path) == 0) {
			break;
		}
		sleep_milliseconds(1000);
	}

	while (!stop_requested) {
		if (fd < 0) {
			fd = open_card(requested_card, selected_card,
				sizeof(selected_card));
			if (fd < 0) {
				sleep_milliseconds(1000);
				continue;
			}
			fprintf(stderr, "Using %s at %u FPS\n",
				selected_card, fps);
			framebuffer_id = 0;
		}

		if (!framebuffer_id || refresh_count++ >= fps) {
			uint32_t next_id;
			uint32_t next_width;
			uint32_t next_height;

			refresh_count = 0;
			if (find_framebuffer(fd, &next_id, &next_width,
				&next_height) < 0) {
				close(fd);
				fd = -1;
				framebuffer_id = 0;
				sleep_milliseconds(1000);
				continue;
			}
			if (next_id != framebuffer_id) {
				fprintf(stderr, "Framebuffer %u: %ux%u\n",
					next_id, next_width, next_height);
			}
			framebuffer_id = next_id;
			width = next_width;
			height = next_height;
		}

		{
			unsigned int first_row;
			unsigned int last_row;

			if (!fb_monitor_damage(&monitor, &first_row,
				&last_row)) {
				sleep_milliseconds(1000L / fps);
				continue;
			}
			if (last_row > height) {
				last_row = height;
			}
			if (first_row >= last_row) {
				sleep_milliseconds(1000L / fps);
				continue;
			}
			if (mark_dirty(fd, framebuffer_id, width, first_row,
				last_row) == 0) {
				if (exit_after_initialize) {
					break;
				}
				sleep_milliseconds(1000L / fps);
				continue;
			}
		}
		{
			monitor.shadow_valid = false;
			if (errno == ENODEV || errno == EBADF) {
				close(fd);
				fd = -1;
			}
			framebuffer_id = 0;
		}
		sleep_milliseconds(1000L / fps);
	}

	if (fd >= 0) {
		close(fd);
	}
	fb_monitor_close(&monitor);
	return 0;
}
