// SPDX-License-Identifier: GPL-2.0-or-later
/* Hardware smoke tests for modeset ordering, framebuffer clear, and PRIME. */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

struct buffer {
	int owner;
	uint32_t source_handle, handle, pitch;
	uint64_t size;
	uint8_t *map;
};

static volatile sig_atomic_t signals_received;

static void fail(const char *operation)
{
	fprintf(stderr, "%s: %s\n", operation, strerror(errno));
	exit(1);
}

static void require(int condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "%s\n", message);
		exit(1);
	}
}

static int parameter(const char *name, const char *value)
{
	char path[256];
	int fd, saved;
	ssize_t result;

	snprintf(path, sizeof(path), "/sys/module/sharp_drm/parameters/%s", name);
	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;
	result = write(fd, value, strlen(value));
	saved = errno;
	close(fd);
	errno = saved;
	return result == (ssize_t)strlen(value) ? 0 : -1;
}

static void reset_stats(void)
{
	int fd = open("/sys/kernel/debug/sharp_drm/stats", O_WRONLY);
	if (fd < 0 || write(fd, "reset\n", 6) != 6)
		fail("reset stats");
	close(fd);
}

static uint64_t counter(const char *name)
{
	FILE *file = fopen("/sys/kernel/debug/sharp_drm/stats", "r");
	char key[80];
	unsigned long long value;
	uint64_t result = 0;

	if (!file)
		fail("read stats");
	while (fscanf(file, "%79s %llu", key, &value) == 2) {
		if (!strcmp(key, name)) {
			result = value;
			break;
		}
	}
	fclose(file);
	return result;
}

static void wait_full_frame(unsigned int height)
{
	unsigned int attempt;
	for (attempt = 0; attempt < 200; attempt++) {
		if (counter("sent_rows") >= height)
			break;
		usleep(10000);
	}
	require(counter("sent_rows") >= height, "full frame was not transmitted");
	require(counter("spi_errors") == 0, "SPI transfer failed");
}

static struct buffer create_buffer(int destination, int owner,
	unsigned int width, unsigned int height, unsigned int bpp)
{
	struct drm_mode_create_dumb create = { .width = width,
		.height = height + 1, .bpp = bpp };
	struct drm_mode_map_dumb mapping;
	struct buffer buffer = { .owner = owner };
	int prime;

	if (ioctl(owner, DRM_IOCTL_MODE_CREATE_DUMB, &create))
		fail("create dumb buffer");
	buffer.source_handle = create.handle;
	buffer.handle = create.handle;
	buffer.pitch = create.pitch;
	buffer.size = create.size;
	mapping = (struct drm_mode_map_dumb) { .handle = create.handle };
	if (ioctl(owner, DRM_IOCTL_MODE_MAP_DUMB, &mapping))
		fail("map dumb buffer");
	buffer.map = mmap(NULL, create.size, PROT_READ | PROT_WRITE,
		MAP_SHARED, owner, mapping.offset);
	if (buffer.map == MAP_FAILED)
		fail("mmap dumb buffer");
	memset(buffer.map, 0xa5, create.size);
	if (destination != owner) {
		if (drmPrimeHandleToFD(owner, create.handle, DRM_CLOEXEC | DRM_RDWR, &prime))
			fail("export PRIME buffer");
		if (drmPrimeFDToHandle(destination, prime, &buffer.handle))
			fail("import PRIME buffer");
		close(prime);
	}
	return buffer;
}

static void destroy_buffer(int destination, struct buffer *buffer)
{
	struct drm_mode_destroy_dumb destroy = { .handle = buffer->source_handle };
	munmap(buffer->map, buffer->size);
	if (destination != buffer->owner)
		drmCloseBufferHandle(destination, buffer->handle);
	ioctl(buffer->owner, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
}

static uint32_t add_framebuffer(int fd, unsigned int width, unsigned int height,
	uint32_t format, struct buffer *buffers, unsigned int planes)
{
	uint32_t handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 }, id;
	unsigned int plane;
	for (plane = 0; plane < planes; plane++) {
		handles[plane] = buffers[plane].handle;
		pitches[plane] = buffers[plane].pitch;
		offsets[plane] = 32;
	}
	if (drmModeAddFB2(fd, width, height, format, handles, pitches, offsets, &id, 0))
		fail("add framebuffer with offsets");
	return id;
}

static void set_framebuffer(int fd, uint32_t crtc, uint32_t fb,
	uint32_t connector, drmModeModeInfo *mode)
{
	int rc;
	do {
		rc = drmModeSetCrtc(fd, crtc, fb, 0, 0, fb ? &connector : NULL,
			fb ? 1 : 0, fb ? mode : NULL);
	} while (rc && errno == EINTR);
	if (rc)
		fail("set CRTC");
}

static void check_clear(struct buffer *buffer, unsigned int width_bytes,
	unsigned int height, uint8_t value)
{
	unsigned int y, x;
	for (x = 0; x < 32; x++)
		require(buffer->map[x] == 0xa5, "clear overwrote framebuffer prefix");
	for (y = 0; y < height; y++) {
		for (x = 0; x < buffer->pitch; x++) {
			uint8_t expected = x < width_bytes ? value : 0xa5;
			require(buffer->map[32 + y * buffer->pitch + x] == expected,
				"framebuffer clear produced wrong bytes");
		}
	}
}

static void signal_handler(int signal_number)
{
	(void)signal_number;
	signals_received++;
}

int main(int argc, char **argv)
{
	int fd, gpu = -1;
	drmModeRes *resources;
	drmModeConnector *connector;
	drmModeCrtc *original;
	drmModeModeInfo mode;
	struct buffer rgb, nv12[2];
	uint32_t crtc, connector_id, rgb_id, nv12_id;
	unsigned int width, height, iteration;

	if (argc < 2 || argc > 3) {
		fprintf(stderr, "Usage: %s SHARP_CARD [GPU_CARD]\n", argv[0]);
		return 2;
	}
	fd = open(argv[1], O_RDWR | O_CLOEXEC);
	if (fd < 0 || drmSetMaster(fd))
		fail("open Sharp DRM master");
	resources = drmModeGetResources(fd);
	require(resources && resources->count_crtcs && resources->count_connectors,
		"no Sharp CRTC or connector");
	crtc = resources->crtcs[0];
	connector_id = resources->connectors[0];
	connector = drmModeGetConnector(fd, connector_id);
	require(connector && connector->count_modes, "no Sharp display mode");
	mode = connector->modes[0];
	width = mode.hdisplay;
	height = mode.vdisplay;
	original = drmModeGetCrtc(fd, crtc);
	require(original != NULL, "could not save original CRTC");

	rgb = create_buffer(fd, fd, width + 4, height, 32);
	rgb_id = add_framebuffer(fd, width, height, DRM_FORMAT_XRGB8888, &rgb, 1);
	set_framebuffer(fd, crtc, 0, connector_id, &mode);
	reset_stats();
	set_framebuffer(fd, crtc, rgb_id, connector_id, &mode);
	wait_full_frame(height);
	puts("PASS: first enable sends a full frame without additional damage");
	set_framebuffer(fd, crtc, 0, connector_id, &mode);
	require(parameter("display_invert", "1") < 0 && errno == ESHUTDOWN,
		"disabled pipe accepted display command");
	require(parameter("display_invert", "0.05,0.001") < 0 && errno == ESHUTDOWN,
		"disabled pipe accepted inversion blink");
	require(parameter("display_blink", "1") < 0 && errno == ESHUTDOWN,
		"disabled pipe accepted blink command");
	require(parameter("display_clear", "1") < 0 && errno == ESHUTDOWN,
		"disabled pipe accepted clear command");
	reset_stats();
	set_framebuffer(fd, crtc, rgb_id, connector_id, &mode);
	wait_full_frame(height);
	puts("PASS: re-enable sends a full frame; disabled pipe rejects commands");
	reset_stats();
	/* The first driver ioctl is SHARP_REDRAW and takes no arguments. */
	if (ioctl(fd, DRM_IO(DRM_COMMAND_BASE), NULL))
		fail("force redraw ioctl");
	require(counter("candidate_rows") == height && counter("sent_rows") >= height,
		"force redraw did not convert and synchronously transmit the full frame");
	require(counter("spi_errors") == 0, "force redraw SPI transfer failed");
	puts("PASS: force redraw ioctl converts and synchronously transmits the full frame");
	if (parameter("display_blink", "1") || parameter("display_blink", "2") ||
		parameter("display_blink", "0") || parameter("display_invert", "1") ||
		parameter("display_invert", "0"))
		fail("panel blink/invert commands");
	puts("PASS: black/white blink and inversion commands return to normal mode");
	if (parameter("display_clear", "1"))
		fail("clear XRGB framebuffer");
	check_clear(&rgb, width * 4, height, 0);
	puts("PASS: XRGB clear respects framebuffer offset and row padding");
	if (parameter("dither_algo", "6"))
		fail("enable error diffusion");
	{
		drmModeClip row = { .x1 = 0, .x2 = width, .y1 = height / 2,
			.y2 = height / 2 + 1 };
		reset_stats();
		if (drmModeDirtyFB(fd, rgb_id, &row, 1))
			fail("damage one row with error diffusion");
		require(counter("candidate_rows") == height && counter("full_refreshes") == 1,
			"partial error-diffusion damage did not convert all preceding rows");
	}
	if (parameter("dither_algo", "4"))
		fail("restore ordered dithering");
	puts("PASS: one-row error-diffusion damage converts the full panel height");

	nv12[0] = create_buffer(fd, fd, width + 16, height, 8);
	nv12[1] = create_buffer(fd, fd, ((width + 1) & ~1U) + 16, (height + 1) / 2, 8);
	nv12_id = add_framebuffer(fd, width, height, DRM_FORMAT_NV12, nv12, 2);
	set_framebuffer(fd, crtc, nv12_id, connector_id, &mode);
	if (parameter("display_clear", "1"))
		fail("clear NV12 framebuffer");
	check_clear(&nv12[0], width, height, 16);
	check_clear(&nv12[1], (width + 1) & ~1U, (height + 1) / 2, 128);
	puts("PASS: NV12 clear writes black Y/UV values and respects plane offsets");
	if (getenv("SHARP_BENCH_NV12")) {
		drmModeClip full = { .x1 = 0, .x2 = width, .y1 = 0, .y2 = height };
		unsigned int y, x;
		uint64_t calls, convert_ns;

		/* A fixed varied image measures conversion without repeated SPI traffic. */
		for (y = 0; y < height; y++)
			for (x = 0; x < width; x++)
				nv12[0].map[32 + y * nv12[0].pitch + x] = (x * 17 + y * 41) & 255;
		for (y = 0; y < (height + 1) / 2; y++)
			for (x = 0; x < ((width + 1) & ~1U); x++)
				nv12[1].map[32 + y * nv12[1].pitch + x] = (x * 11 + y * 53) & 255;
		if (drmModeDirtyFB(fd, nv12_id, &full, 1))
			fail("warm up NV12 benchmark");
		usleep(300000);
		reset_stats();
		for (iteration = 0; iteration < 200; iteration++)
			if (drmModeDirtyFB(fd, nv12_id, &full, 1))
				fail("benchmark NV12 damage");
		calls = counter("update_calls");
		convert_ns = counter("convert_ns");
		require(calls == 200 && counter("spi_errors") == 0,
			"NV12 benchmark lost an update or failed SPI");
		printf("NV12 benchmark: colors=64 dither=4 updates=%llu convert_ns=%llu avg_us=%.3f\n",
			(unsigned long long)calls, (unsigned long long)convert_ns,
			(double)convert_ns / calls / 1000);
	}

	if (argc == 3) {
		struct buffer imported;
		uint32_t imported_id;
		struct sigaction action = { .sa_handler = signal_handler };
		struct itimerval timer = { .it_interval = { 0, 1000 },
			.it_value = { 0, 1000 } };
		gpu = open(argv[2], O_RDWR | O_CLOEXEC);
		if (gpu < 0)
			fail("open GPU DRM device");
		imported = create_buffer(fd, gpu, width + 4, height, 32);
		imported_id = add_framebuffer(fd, width, height,
			DRM_FORMAT_XRGB8888, &imported, 1);
		reset_stats();
		sigaction(SIGALRM, &action, NULL);
		setitimer(ITIMER_REAL, &timer, NULL);
		for (iteration = 0; iteration < 40; iteration++) {
			unsigned int y;
			for (y = 0; y < height; y++)
				memset(imported.map + 32 + y * imported.pitch,
					iteration & 1 ? 0xff : 0, width * 4);
			set_framebuffer(fd, crtc, imported_id, connector_id, &mode);
		}
		timer = (struct itimerval) { 0 };
		setitimer(ITIMER_REAL, &timer, NULL);
		/* A synchronous panel command drains the latest queued frame. */
		if (parameter("display_invert", "0"))
			fail("drain PRIME transmit slots");
		wait_full_frame(height);
		require(signals_received > 0, "signal pressure was not exercised");
		require(counter("throttled_updates") > 0, "imported-buffer wait was not exercised");
		require(counter("update_calls") == 40, "a PRIME commit dropped its update callback");
		require(counter("candidate_rows") == (uint64_t)height * 40,
			"a PRIME commit skipped framebuffer conversion under signal pressure");
		printf("PASS: 40 PRIME commits under %d signals; throttled updates=%llu, SPI errors=0\n",
			(int)signals_received, (unsigned long long)counter("throttled_updates"));
		set_framebuffer(fd, crtc, rgb_id, connector_id, &mode);
		drmModeRmFB(fd, imported_id);
		destroy_buffer(fd, &imported);
		close(gpu);
	}
	if (original->mode_valid)
		set_framebuffer(fd, crtc, original->buffer_id, connector_id, &original->mode);
	else
		set_framebuffer(fd, crtc, 0, connector_id, &mode);
	drmModeRmFB(fd, nv12_id);
	drmModeRmFB(fd, rgb_id);
	destroy_buffer(fd, &nv12[1]);
	destroy_buffer(fd, &nv12[0]);
	destroy_buffer(fd, &rgb);
	drmModeFreeCrtc(original);
	drmModeFreeConnector(connector);
	drmModeFreeResources(resources);
	drmDropMaster(fd);
	close(fd);
	return 0;
}
