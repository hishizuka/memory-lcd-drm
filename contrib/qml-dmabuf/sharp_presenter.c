#define _POSIX_C_SOURCE 200809L

#include "sharp_presenter.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

enum sharp_buffer_state {
    SHARP_BUFFER_FREE,
    SHARP_BUFFER_RENDERING,
    SHARP_BUFFER_READY,
    SHARP_BUFFER_SUBMITTING,
    SHARP_BUFFER_ACTIVE,
};

struct plane_state {
    uint32_t plane_id;
    uint32_t prop_fb_id;
    uint32_t prop_crtc_id;
    uint32_t prop_crtc_x;
    uint32_t prop_crtc_y;
    uint32_t prop_crtc_w;
    uint32_t prop_crtc_h;
    uint32_t prop_src_x;
    uint32_t prop_src_y;
    uint32_t prop_src_w;
    uint32_t prop_src_h;
    uint32_t prop_damage;
    uint64_t fb_id;
    uint64_t crtc_id;
    uint64_t crtc_x;
    uint64_t crtc_y;
    uint64_t crtc_w;
    uint64_t crtc_h;
    uint64_t src_x;
    uint64_t src_y;
    uint64_t src_w;
    uint64_t src_h;
};

struct sharp_buffer {
    struct gbm_bo *bo;
    int dma_fd;
    uint32_t stride;
    uint32_t offset;
    uint64_t modifier;
    EGLImageKHR image;
    GLuint renderbuffer;
    uint32_t sharp_handle;
    uint32_t sharp_fb_id;
    enum sharp_buffer_state state;
    struct drm_mode_rect *damage_rects;
    uint32_t damage_rect_count;
    uint32_t damage_rows;
    bool auto_damage;
};

struct sharp_presenter {
    uint32_t width;
    uint32_t height;
    int render_fd;
    struct gbm_device *gbm;
    EGLDisplay egl_display;
    PFNEGLCREATEIMAGEKHRPROC create_image;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image;
    PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC image_to_renderbuffer;
    int sharp_fd;
    struct plane_state plane;
    struct sharp_buffer buffers[SHARP_PRESENTER_BUFFER_COUNT];
    int active_index;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    pthread_t worker;
    bool mutex_initialized;
    bool condition_initialized;
    bool worker_started;
    bool stopping;
    bool failed;
    bool display_changed;
    int event_fd;
    struct sharp_presenter_stats stats;
    char error[512];
};

static _Thread_local char create_error[512];

static void set_error_locked(
    struct sharp_presenter *presenter, const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    vsnprintf(presenter->error, sizeof(presenter->error), format, arguments);
    va_end(arguments);
    snprintf(create_error, sizeof(create_error), "%s", presenter->error);
}

static void set_error(struct sharp_presenter *presenter, const char *format, ...)
{
    char message[sizeof(create_error)];
    va_list arguments;

    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    snprintf(create_error, sizeof(create_error), "%s", message);
    if (presenter == NULL) {
        return;
    }
    if (presenter->mutex_initialized) {
        pthread_mutex_lock(&presenter->mutex);
    }
    snprintf(presenter->error, sizeof(presenter->error), "%s", message);
    if (presenter->mutex_initialized) {
        pthread_mutex_unlock(&presenter->mutex);
    }
}

const char *sharp_presenter_last_error(const struct sharp_presenter *presenter)
{
    if (presenter != NULL && presenter->error[0] != '\0') {
        return presenter->error;
    }
    return create_error;
}

static int get_property(
    struct sharp_presenter *presenter, int fd, uint32_t object_id,
    uint32_t object_type, const char *name, uint32_t *property_id,
    uint64_t *value)
{
    drmModeObjectProperties *properties;
    uint32_t index;
    int result = -1;

    properties = drmModeObjectGetProperties(fd, object_id, object_type);
    if (properties == NULL) {
        set_error(presenter, "Cannot read DRM object %u properties: %s",
                  object_id, strerror(errno));
        return -1;
    }
    for (index = 0; index < properties->count_props; index++) {
        drmModePropertyRes *property =
            drmModeGetProperty(fd, properties->props[index]);

        if (property == NULL) {
            continue;
        }
        if (strcmp(property->name, name) == 0) {
            *property_id = property->prop_id;
            if (value != NULL) {
                *value = properties->prop_values[index];
            }
            result = 0;
            drmModeFreeProperty(property);
            break;
        }
        drmModeFreeProperty(property);
    }
    drmModeFreeObjectProperties(properties);
    if (result != 0) {
        set_error(presenter, "DRM property %s was not found on object %u", name,
                  object_id);
    }
    return result;
}

static int load_plane_state(
    struct sharp_presenter *presenter, int fd, struct plane_state *state)
{
#define LOAD_PROPERTY(field, name, value_field)                                  \
    do {                                                                          \
        if (get_property(presenter, fd, state->plane_id,                         \
                         DRM_MODE_OBJECT_PLANE, name, &state->field,             \
                         &state->value_field) != 0) {                            \
            return -1;                                                            \
        }                                                                         \
    } while (0)

    LOAD_PROPERTY(prop_fb_id, "FB_ID", fb_id);
    LOAD_PROPERTY(prop_crtc_id, "CRTC_ID", crtc_id);
    LOAD_PROPERTY(prop_crtc_x, "CRTC_X", crtc_x);
    LOAD_PROPERTY(prop_crtc_y, "CRTC_Y", crtc_y);
    LOAD_PROPERTY(prop_crtc_w, "CRTC_W", crtc_w);
    LOAD_PROPERTY(prop_crtc_h, "CRTC_H", crtc_h);
    LOAD_PROPERTY(prop_src_x, "SRC_X", src_x);
    LOAD_PROPERTY(prop_src_y, "SRC_Y", src_y);
    LOAD_PROPERTY(prop_src_w, "SRC_W", src_w);
    LOAD_PROPERTY(prop_src_h, "SRC_H", src_h);
    if (get_property(presenter, fd, state->plane_id, DRM_MODE_OBJECT_PLANE,
                     "FB_DAMAGE_CLIPS", &state->prop_damage, NULL) != 0) {
        return -1;
    }
#undef LOAD_PROPERTY
    return 0;
}

static int find_primary_plane(
    struct sharp_presenter *presenter, int fd, struct plane_state *state)
{
    drmModePlaneRes *resources = drmModeGetPlaneResources(fd);
    uint32_t index;
    int result = -1;

    if (resources == NULL) {
        set_error(presenter, "drmModeGetPlaneResources failed: %s",
                  strerror(errno));
        return -1;
    }
    for (index = 0; index < resources->count_planes; index++) {
        drmModePlane *plane = drmModeGetPlane(fd, resources->planes[index]);
        uint32_t property_id;
        uint64_t type;

        if (plane == NULL) {
            continue;
        }
        if (get_property(presenter, fd, plane->plane_id, DRM_MODE_OBJECT_PLANE,
                         "type", &property_id, &type) == 0 &&
            type == DRM_PLANE_TYPE_PRIMARY && plane->crtc_id != 0) {
            memset(state, 0, sizeof(*state));
            state->plane_id = plane->plane_id;
            result = load_plane_state(presenter, fd, state);
            drmModeFreePlane(plane);
            break;
        }
        drmModeFreePlane(plane);
    }
    drmModeFreePlaneResources(resources);
    if (result != 0 && presenter->error[0] == '\0') {
        set_error(presenter, "Active primary plane was not found");
    }
    return result;
}

static int validate_display(
    struct sharp_presenter *presenter, int fd, uint32_t width,
    uint32_t height)
{
    drmModeRes *resources = drmModeGetResources(fd);
    int index;
    int result = -1;

    if (resources == NULL) {
        set_error(presenter, "drmModeGetResources failed: %s", strerror(errno));
        return -1;
    }
    for (index = 0; index < resources->count_connectors; index++) {
        drmModeConnector *connector =
            drmModeGetConnector(fd, resources->connectors[index]);

        if (connector == NULL) {
            continue;
        }
        if (connector->connection == DRM_MODE_CONNECTED &&
            connector->count_modes > 0 &&
            connector->modes[0].hdisplay == width &&
            connector->modes[0].vdisplay == height) {
            result = 0;
            drmModeFreeConnector(connector);
            break;
        }
        drmModeFreeConnector(connector);
    }
    drmModeFreeResources(resources);
    if (result != 0) {
        set_error(presenter, "Connected %ux%u Sharp connector was not found",
                  width, height);
    }
    return result;
}

static int add_atomic_property(
    struct sharp_presenter *presenter, drmModeAtomicReq *request,
    uint32_t object_id, uint32_t property_id, uint64_t value)
{
    if (drmModeAtomicAddProperty(request, object_id, property_id, value) < 0) {
        set_error(presenter, "drmModeAtomicAddProperty(%u, %u) failed",
                  object_id, property_id);
        return -1;
    }
    return 0;
}

static int commit_framebuffer(
    struct sharp_presenter *presenter, uint32_t framebuffer_id,
    uint32_t damage_blob_id)
{
    const struct plane_state *state = &presenter->plane;
    drmModeAtomicReq *request = drmModeAtomicAlloc();
    int result = -1;

    if (request == NULL) {
        set_error(presenter, "drmModeAtomicAlloc failed");
        return -1;
    }
#define ADD_PROPERTY(property, value)                                            \
    do {                                                                          \
        if (add_atomic_property(presenter, request, state->plane_id,             \
                                state->property, value) != 0) {                  \
            goto done;                                                            \
        }                                                                         \
    } while (0)

    ADD_PROPERTY(prop_fb_id, framebuffer_id);
    ADD_PROPERTY(prop_crtc_id, state->crtc_id);
    ADD_PROPERTY(prop_crtc_x, state->crtc_x);
    ADD_PROPERTY(prop_crtc_y, state->crtc_y);
    ADD_PROPERTY(prop_crtc_w, state->crtc_w);
    ADD_PROPERTY(prop_crtc_h, state->crtc_h);
    ADD_PROPERTY(prop_src_x, state->src_x);
    ADD_PROPERTY(prop_src_y, state->src_y);
    ADD_PROPERTY(prop_src_w, state->src_w);
    ADD_PROPERTY(prop_src_h, state->src_h);
    ADD_PROPERTY(prop_damage, damage_blob_id);
#undef ADD_PROPERTY

    if (drmModeAtomicCommit(presenter->sharp_fd, request, 0, NULL) != 0) {
        set_error(presenter, "drmModeAtomicCommit failed: %s", strerror(errno));
        goto done;
    }
    result = 0;

done:
    drmModeAtomicFree(request);
    return result;
}

static int create_damage_blob(
    struct sharp_presenter *presenter, const struct drm_mode_rect *damage,
    uint32_t damage_count, uint32_t *blob_id)
{
    if (damage_count == 0 ||
        drmModeCreatePropertyBlob(presenter->sharp_fd, damage,
                                  damage_count * sizeof(*damage),
                                  blob_id) != 0) {
        set_error(presenter, "drmModeCreatePropertyBlob failed: %s",
                  strerror(errno));
        return -1;
    }
    return 0;
}

static void close_gem_handle(int fd, uint32_t handle)
{
    struct drm_gem_close close_arg = {.handle = handle};

    drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &close_arg);
}

static void notify_event(struct sharp_presenter *presenter)
{
    uint64_t value = 1;
    ssize_t result;

    if (presenter->event_fd == -1) {
        return;
    }
    do {
        result = write(presenter->event_fd, &value, sizeof(value));
    } while (result == -1 && errno == EINTR);
}

static int ready_buffer_locked(const struct sharp_presenter *presenter)
{
    unsigned int index;

    for (index = 0; index < SHARP_PRESENTER_BUFFER_COUNT; index++) {
        if (presenter->buffers[index].state == SHARP_BUFFER_READY) {
            return (int)index;
        }
    }
    return -1;
}

static int detect_damage(
    struct sharp_presenter *presenter, struct sharp_buffer *buffer);

static void *submit_worker(void *argument)
{
    struct sharp_presenter *presenter = argument;

    for (;;) {
        struct sharp_buffer *buffer;
        uint32_t damage_blob = 0;
        int previous_active;
        int buffer_index;
        int result;

        pthread_mutex_lock(&presenter->mutex);
        while (!presenter->stopping &&
               (buffer_index = ready_buffer_locked(presenter)) == -1) {
            pthread_cond_wait(&presenter->condition, &presenter->mutex);
        }
        if (presenter->stopping) {
            pthread_mutex_unlock(&presenter->mutex);
            break;
        }
        buffer = &presenter->buffers[buffer_index];
        buffer->state = SHARP_BUFFER_SUBMITTING;
        pthread_mutex_unlock(&presenter->mutex);

        result = buffer->auto_damage ? detect_damage(presenter, buffer) : 0;
        if (result == 0 && buffer->damage_rect_count == 0) {
            pthread_mutex_lock(&presenter->mutex);
            buffer->state = SHARP_BUFFER_FREE;
            presenter->stats.unchanged_frames++;
            pthread_cond_broadcast(&presenter->condition);
            pthread_mutex_unlock(&presenter->mutex);
            notify_event(presenter);
            continue;
        }
        if (result == 0) {
            result = create_damage_blob(presenter, buffer->damage_rects,
                                        buffer->damage_rect_count,
                                        &damage_blob);
        }
        if (result == 0) {
            result = commit_framebuffer(presenter, buffer->sharp_fb_id,
                                        damage_blob);
            drmModeDestroyPropertyBlob(presenter->sharp_fd, damage_blob);
        }

        pthread_mutex_lock(&presenter->mutex);
        presenter->stats.submitted_frames++;
        presenter->stats.damage_rows += buffer->damage_rows;
        if (result != 0) {
            buffer->state = SHARP_BUFFER_FREE;
            presenter->stats.commit_errors++;
            presenter->failed = true;
            presenter->stopping = true;
            pthread_cond_broadcast(&presenter->condition);
            pthread_mutex_unlock(&presenter->mutex);
            notify_event(presenter);
            break;
        }

        previous_active = presenter->active_index;
        if (previous_active >= 0 && previous_active != buffer_index) {
            presenter->buffers[previous_active].state = SHARP_BUFFER_FREE;
        }
        buffer->state = SHARP_BUFFER_ACTIVE;
        presenter->active_index = buffer_index;
        presenter->display_changed = true;
        presenter->stats.presented_frames++;
        pthread_cond_broadcast(&presenter->condition);
        pthread_mutex_unlock(&presenter->mutex);
        notify_event(presenter);
    }
    return NULL;
}

static int create_buffer(
    struct sharp_presenter *presenter, struct sharp_buffer *buffer)
{
    EGLint image_attributes[13];

    buffer->dma_fd = -1;
    buffer->image = EGL_NO_IMAGE_KHR;
    buffer->modifier = DRM_FORMAT_MOD_INVALID;
    buffer->state = SHARP_BUFFER_FREE;
    buffer->damage_rects =
        calloc(presenter->height, sizeof(*buffer->damage_rects));
    if (buffer->damage_rects == NULL) {
        set_error(presenter, "Damage rectangle allocation failed");
        return -1;
    }
    buffer->bo = gbm_bo_create(
        presenter->gbm, presenter->width, presenter->height,
        GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    if (buffer->bo == NULL) {
        set_error(presenter, "gbm_bo_create(linear XRGB8888) failed");
        return -1;
    }
    buffer->stride = gbm_bo_get_stride_for_plane(buffer->bo, 0);
    buffer->offset = gbm_bo_get_offset(buffer->bo, 0);
    buffer->modifier = gbm_bo_get_modifier(buffer->bo);
    if (buffer->modifier != DRM_FORMAT_MOD_LINEAR &&
        buffer->modifier != DRM_FORMAT_MOD_INVALID) {
        set_error(presenter, "GBM returned non-linear modifier 0x%016llx",
                  (unsigned long long)buffer->modifier);
        return -1;
    }
    buffer->dma_fd = gbm_bo_get_fd(buffer->bo);
    if (buffer->dma_fd == -1) {
        set_error(presenter, "gbm_bo_get_fd failed: %s", strerror(errno));
        return -1;
    }

    image_attributes[0] = EGL_WIDTH;
    image_attributes[1] = (EGLint)presenter->width;
    image_attributes[2] = EGL_HEIGHT;
    image_attributes[3] = (EGLint)presenter->height;
    image_attributes[4] = EGL_LINUX_DRM_FOURCC_EXT;
    image_attributes[5] = DRM_FORMAT_XRGB8888;
    image_attributes[6] = EGL_DMA_BUF_PLANE0_FD_EXT;
    image_attributes[7] = buffer->dma_fd;
    image_attributes[8] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
    image_attributes[9] = (EGLint)buffer->offset;
    image_attributes[10] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
    image_attributes[11] = (EGLint)buffer->stride;
    image_attributes[12] = EGL_NONE;
    buffer->image = presenter->create_image(
        presenter->egl_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL,
        image_attributes);
    if (buffer->image == EGL_NO_IMAGE_KHR) {
        set_error(presenter, "eglCreateImageKHR failed: EGL error 0x%04x",
                  eglGetError());
        return -1;
    }

    glGenRenderbuffers(1, &buffer->renderbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, buffer->renderbuffer);
    presenter->image_to_renderbuffer(GL_RENDERBUFFER, buffer->image);
    if (glGetError() != GL_NO_ERROR) {
        set_error(presenter,
                  "glEGLImageTargetRenderbufferStorageOES failed");
        return -1;
    }
    return 0;
}

static int import_buffer(
    struct sharp_presenter *presenter, struct sharp_buffer *buffer)
{
    uint32_t handles[4] = {0};
    uint32_t pitches[4] = {0};
    uint32_t offsets[4] = {0};

    if (drmPrimeFDToHandle(presenter->sharp_fd, buffer->dma_fd,
                           &buffer->sharp_handle) != 0) {
        set_error(presenter, "drmPrimeFDToHandle failed: %s", strerror(errno));
        return -1;
    }
    handles[0] = buffer->sharp_handle;
    pitches[0] = buffer->stride;
    offsets[0] = buffer->offset;
    if (drmModeAddFB2(presenter->sharp_fd, presenter->width,
                      presenter->height, DRM_FORMAT_XRGB8888, handles, pitches,
                      offsets, &buffer->sharp_fb_id, 0) != 0) {
        set_error(presenter, "drmModeAddFB2 failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static void destroy_buffer(
    struct sharp_presenter *presenter, struct sharp_buffer *buffer)
{
    if (buffer->sharp_fb_id != 0 && presenter->sharp_fd != -1) {
        drmModeRmFB(presenter->sharp_fd, buffer->sharp_fb_id);
    }
    if (buffer->sharp_handle != 0 && presenter->sharp_fd != -1) {
        close_gem_handle(presenter->sharp_fd, buffer->sharp_handle);
    }
    if (buffer->renderbuffer != 0) {
        glDeleteRenderbuffers(1, &buffer->renderbuffer);
    }
    if (buffer->image != EGL_NO_IMAGE_KHR &&
        presenter->destroy_image != NULL) {
        presenter->destroy_image(presenter->egl_display, buffer->image);
    }
    if (buffer->dma_fd != -1) {
        close(buffer->dma_fd);
    }
    if (buffer->bo != NULL) {
        gbm_bo_destroy(buffer->bo);
    }
    free(buffer->damage_rects);
    memset(buffer, 0, sizeof(*buffer));
    buffer->dma_fd = -1;
    buffer->image = EGL_NO_IMAGE_KHR;
}

struct sharp_presenter *sharp_presenter_create(
    const char *render_node, const char *sharp_card, uint32_t width,
    uint32_t height)
{
    struct sharp_presenter *presenter;
    unsigned int index;

    create_error[0] = '\0';
    presenter = calloc(1, sizeof(*presenter));
    if (presenter == NULL) {
        set_error(NULL, "Presenter allocation failed");
        return NULL;
    }
    if (width == 0 || height == 0 || width > INT16_MAX ||
        height > INT16_MAX) {
        set_error(NULL, "Invalid presenter size %ux%u", width, height);
        free(presenter);
        return NULL;
    }
    presenter->width = width;
    presenter->height = height;
    presenter->render_fd = -1;
    presenter->sharp_fd = -1;
    presenter->event_fd = -1;
    presenter->active_index = -1;
    presenter->egl_display = EGL_NO_DISPLAY;
    for (index = 0; index < SHARP_PRESENTER_BUFFER_COUNT; index++) {
        presenter->buffers[index].dma_fd = -1;
        presenter->buffers[index].image = EGL_NO_IMAGE_KHR;
    }
    if (pthread_mutex_init(&presenter->mutex, NULL) != 0) {
        set_error(NULL, "pthread_mutex_init failed");
        goto fail;
    }
    presenter->mutex_initialized = true;
    if (pthread_cond_init(&presenter->condition, NULL) != 0) {
        set_error(presenter, "pthread_cond_init failed");
        goto fail;
    }
    presenter->condition_initialized = true;
    presenter->event_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (presenter->event_fd == -1) {
        set_error(presenter, "eventfd failed: %s", strerror(errno));
        goto fail;
    }

    presenter->egl_display = eglGetCurrentDisplay();
    if (presenter->egl_display == EGL_NO_DISPLAY ||
        eglGetCurrentContext() == EGL_NO_CONTEXT) {
        set_error(presenter, "A current EGL context is required");
        goto fail;
    }
    presenter->create_image =
        (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    presenter->destroy_image =
        (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    presenter->image_to_renderbuffer =
        (PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC)eglGetProcAddress(
            "glEGLImageTargetRenderbufferStorageOES");
    if (presenter->create_image == NULL || presenter->destroy_image == NULL ||
        presenter->image_to_renderbuffer == NULL) {
        set_error(presenter, "Required EGLImage functions are unavailable");
        goto fail;
    }

    presenter->render_fd = open(render_node, O_RDWR | O_CLOEXEC);
    if (presenter->render_fd == -1) {
        set_error(presenter, "open %s failed: %s", render_node,
                  strerror(errno));
        goto fail;
    }
    presenter->gbm = gbm_create_device(presenter->render_fd);
    if (presenter->gbm == NULL) {
        set_error(presenter, "gbm_create_device failed");
        goto fail;
    }
    for (index = 0; index < SHARP_PRESENTER_BUFFER_COUNT; index++) {
        if (create_buffer(presenter, &presenter->buffers[index]) != 0) {
            goto fail;
        }
    }

    presenter->sharp_fd = open(sharp_card, O_RDWR | O_CLOEXEC);
    if (presenter->sharp_fd == -1) {
        set_error(presenter, "open %s failed: %s", sharp_card,
                  strerror(errno));
        goto fail;
    }
    if (drmSetClientCap(presenter->sharp_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES,
                        1) != 0 ||
        drmSetClientCap(presenter->sharp_fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
        set_error(presenter, "Enabling atomic DRM capabilities failed: %s",
                  strerror(errno));
        goto fail;
    }
    if (validate_display(presenter, presenter->sharp_fd, width, height) != 0 ||
        find_primary_plane(presenter, presenter->sharp_fd,
                           &presenter->plane) != 0) {
        goto fail;
    }
    if (presenter->plane.crtc_w != width ||
        presenter->plane.crtc_h != height || presenter->plane.fb_id == 0) {
        set_error(presenter, "Unexpected active Sharp plane state");
        goto fail;
    }
    for (index = 0; index < SHARP_PRESENTER_BUFFER_COUNT; index++) {
        if (import_buffer(presenter, &presenter->buffers[index]) != 0) {
            goto fail;
        }
    }
    if (pthread_create(&presenter->worker, NULL, submit_worker, presenter) !=
        0) {
        set_error(presenter, "pthread_create failed");
        goto fail;
    }
    presenter->worker_started = true;
    return presenter;

fail:
    sharp_presenter_destroy(presenter);
    return NULL;
}

int sharp_presenter_acquire(
    struct sharp_presenter *presenter, uint32_t *buffer_index,
    uint32_t *renderbuffer)
{
    int index;

    if (presenter == NULL || buffer_index == NULL || renderbuffer == NULL) {
        set_error(presenter, "Invalid acquire request");
        return SHARP_PRESENTER_ERROR;
    }
    pthread_mutex_lock(&presenter->mutex);
    if (presenter->failed || presenter->stopping) {
        pthread_mutex_unlock(&presenter->mutex);
        return SHARP_PRESENTER_ERROR;
    }

    index = ready_buffer_locked(presenter);
    if (index != -1) {
        presenter->buffers[index].state = SHARP_BUFFER_RENDERING;
        presenter->stats.replaced_pending_frames++;
    } else {
        unsigned int candidate;

        for (candidate = 0; candidate < SHARP_PRESENTER_BUFFER_COUNT;
             candidate++) {
            if (presenter->buffers[candidate].state == SHARP_BUFFER_FREE) {
                index = (int)candidate;
                presenter->buffers[index].state = SHARP_BUFFER_RENDERING;
                break;
            }
        }
    }
    if (index == -1) {
        presenter->stats.busy_acquires++;
        pthread_mutex_unlock(&presenter->mutex);
        return SHARP_PRESENTER_BUSY;
    }
    *buffer_index = (uint32_t)index;
    *renderbuffer = presenter->buffers[index].renderbuffer;
    pthread_mutex_unlock(&presenter->mutex);
    return SHARP_PRESENTER_ACQUIRED;
}

static int finish_render(struct sharp_presenter *presenter)
{
    glFinish();
    if (glGetError() != GL_NO_ERROR) {
        set_error(presenter, "glFinish reported an OpenGL error");
        return -1;
    }
    return 0;
}

static int validate_submit_locked(
    struct sharp_presenter *presenter, uint32_t buffer_index,
    struct sharp_buffer **buffer)
{
    *buffer = &presenter->buffers[buffer_index];
    if (presenter->failed || presenter->stopping ||
        (*buffer)->state != SHARP_BUFFER_RENDERING) {
        set_error_locked(presenter,
                         "Submitted buffer is not in RENDERING state");
        return -1;
    }
    return 0;
}

static void queue_buffer_locked(
    struct sharp_presenter *presenter, struct sharp_buffer *buffer)
{
    buffer->state = SHARP_BUFFER_READY;
    pthread_cond_signal(&presenter->condition);
}

int sharp_presenter_submit(
    struct sharp_presenter *presenter, uint32_t buffer_index, uint32_t y,
    uint32_t height)
{
    struct sharp_buffer *buffer;

    if (presenter == NULL || buffer_index >= SHARP_PRESENTER_BUFFER_COUNT ||
        height == 0 || y >= presenter->height ||
        height > presenter->height - y) {
        set_error(presenter, "Invalid submit request");
        return -1;
    }
    if (finish_render(presenter) != 0) {
        return -1;
    }

    pthread_mutex_lock(&presenter->mutex);
    if (validate_submit_locked(presenter, buffer_index, &buffer) != 0) {
        pthread_mutex_unlock(&presenter->mutex);
        return -1;
    }
    buffer->damage_rects[0] = (struct drm_mode_rect){
        .x1 = 0,
        .y1 = (int16_t)y,
        .x2 = (int16_t)presenter->width,
        .y2 = (int16_t)(y + height),
    };
    buffer->damage_rect_count = 1;
    buffer->damage_rows = height;
    buffer->auto_damage = false;
    queue_buffer_locked(presenter, buffer);
    pthread_mutex_unlock(&presenter->mutex);
    return 0;
}

static int detect_damage(
    struct sharp_presenter *presenter, struct sharp_buffer *buffer)
{
    struct sharp_buffer *active;
    uint32_t active_stride;
    uint32_t buffer_stride;
    void *active_map_data = NULL;
    void *buffer_map_data = NULL;
    uint8_t *active_mapping;
    uint8_t *buffer_mapping;
    uint32_t run_start = 0;
    uint32_t y;
    bool in_run = false;

    buffer->damage_rect_count = 0;
    buffer->damage_rows = 0;
    if (presenter->active_index < 0) {
        buffer->damage_rects[0] = (struct drm_mode_rect){
            .x1 = 0,
            .y1 = 0,
            .x2 = (int16_t)presenter->width,
            .y2 = (int16_t)presenter->height,
        };
        buffer->damage_rect_count = 1;
        buffer->damage_rows = presenter->height;
        return 0;
    }

    active = &presenter->buffers[presenter->active_index];
    active_mapping = gbm_bo_map(
        active->bo, 0, 0, presenter->width, presenter->height,
        GBM_BO_TRANSFER_READ, &active_stride, &active_map_data);
    if (active_mapping == NULL) {
        set_error(presenter, "gbm_bo_map(active read) failed");
        return -1;
    }
    buffer_mapping = gbm_bo_map(
        buffer->bo, 0, 0, presenter->width, presenter->height,
        GBM_BO_TRANSFER_READ, &buffer_stride, &buffer_map_data);
    if (buffer_mapping == NULL) {
        set_error(presenter, "gbm_bo_map(rendered read) failed");
        gbm_bo_unmap(active->bo, active_map_data);
        return -1;
    }

    for (y = 0; y < presenter->height; y++) {
        bool changed = memcmp(buffer_mapping + y * buffer_stride,
                              active_mapping + y * active_stride,
                              presenter->width * 4) != 0;

        if (changed) {
            buffer->damage_rows++;
            if (!in_run) {
                run_start = y;
                in_run = true;
            }
        } else if (in_run) {
            buffer->damage_rects[buffer->damage_rect_count++] =
                (struct drm_mode_rect){
                    .x1 = 0,
                    .y1 = (int16_t)run_start,
                    .x2 = (int16_t)presenter->width,
                    .y2 = (int16_t)y,
                };
            in_run = false;
        }
    }
    if (in_run) {
        buffer->damage_rects[buffer->damage_rect_count++] =
            (struct drm_mode_rect){
                .x1 = 0,
                .y1 = (int16_t)run_start,
                .x2 = (int16_t)presenter->width,
                .y2 = (int16_t)presenter->height,
            };
    }
    gbm_bo_unmap(buffer->bo, buffer_map_data);
    gbm_bo_unmap(active->bo, active_map_data);
    return 0;
}

int sharp_presenter_submit_auto_damage(
    struct sharp_presenter *presenter, uint32_t buffer_index)
{
    struct sharp_buffer *buffer;

    if (presenter == NULL || buffer_index >= SHARP_PRESENTER_BUFFER_COUNT) {
        set_error(presenter, "Invalid automatic damage submit request");
        return -1;
    }
    if (finish_render(presenter) != 0) {
        return -1;
    }

    pthread_mutex_lock(&presenter->mutex);
    if (validate_submit_locked(presenter, buffer_index, &buffer) != 0) {
        pthread_mutex_unlock(&presenter->mutex);
        return -1;
    }
    buffer->auto_damage = true;
    queue_buffer_locked(presenter, buffer);
    pthread_mutex_unlock(&presenter->mutex);
    return 0;
}

int sharp_presenter_event_fd(const struct sharp_presenter *presenter)
{
    return presenter != NULL ? presenter->event_fd : -1;
}

int sharp_presenter_dispatch(struct sharp_presenter *presenter)
{
    uint64_t value;
    ssize_t result;
    bool failed;

    if (presenter == NULL || presenter->event_fd == -1) {
        return -1;
    }
    do {
        result = read(presenter->event_fd, &value, sizeof(value));
    } while (result == (ssize_t)sizeof(value) ||
             (result == -1 && errno == EINTR));
    if (result == -1 && errno != EAGAIN) {
        set_error(presenter, "eventfd read failed: %s", strerror(errno));
        return -1;
    }
    pthread_mutex_lock(&presenter->mutex);
    failed = presenter->failed;
    pthread_mutex_unlock(&presenter->mutex);
    return failed ? -1 : 0;
}

uint32_t sharp_presenter_stride(
    const struct sharp_presenter *presenter, uint32_t buffer_index)
{
    if (presenter == NULL || buffer_index >= SHARP_PRESENTER_BUFFER_COUNT) {
        return 0;
    }
    return presenter->buffers[buffer_index].stride;
}

uint64_t sharp_presenter_modifier(
    const struct sharp_presenter *presenter, uint32_t buffer_index)
{
    if (presenter == NULL || buffer_index >= SHARP_PRESENTER_BUFFER_COUNT) {
        return DRM_FORMAT_MOD_INVALID;
    }
    return presenter->buffers[buffer_index].modifier;
}

static int dump_buffer_ppm(
    struct sharp_presenter *presenter, struct sharp_buffer *buffer,
    const char *path)
{
    uint32_t map_stride;
    void *map_data = NULL;
    uint8_t *mapping;
    FILE *output;
    uint32_t x;
    uint32_t y;

    mapping = gbm_bo_map(buffer->bo, 0, 0, presenter->width,
                         presenter->height, GBM_BO_TRANSFER_READ, &map_stride,
                         &map_data);
    if (mapping == NULL) {
        set_error_locked(presenter, "gbm_bo_map(read) failed");
        return -1;
    }
    output = fopen(path, "wb");
    if (output == NULL) {
        set_error_locked(presenter, "open %s failed: %s", path,
                         strerror(errno));
        gbm_bo_unmap(buffer->bo, map_data);
        return -1;
    }
    fprintf(output, "P6\n%u %u\n255\n", presenter->width,
            presenter->height);
    for (y = 0; y < presenter->height; y++) {
        const uint8_t *row = mapping + y * map_stride;

        for (x = 0; x < presenter->width; x++) {
            const uint8_t *pixel = row + x * 4;

            fputc(pixel[2], output);
            fputc(pixel[1], output);
            fputc(pixel[0], output);
        }
    }
    fclose(output);
    gbm_bo_unmap(buffer->bo, map_data);
    return 0;
}

int sharp_presenter_dump_active_ppm(
    struct sharp_presenter *presenter, const char *path)
{
    struct sharp_buffer *buffer = NULL;
    int ready_index;
    int result;

    if (presenter == NULL || path == NULL) {
        set_error(presenter, "Invalid PPM output request");
        return -1;
    }
    glFinish();
    pthread_mutex_lock(&presenter->mutex);
    ready_index = ready_buffer_locked(presenter);
    if (ready_index >= 0) {
        buffer = &presenter->buffers[ready_index];
    } else if (presenter->active_index >= 0) {
        buffer = &presenter->buffers[presenter->active_index];
    }
    if (buffer == NULL) {
        pthread_mutex_unlock(&presenter->mutex);
        set_error(presenter, "No completed presenter buffer is available");
        return -1;
    }
    result = dump_buffer_ppm(presenter, buffer, path);
    pthread_mutex_unlock(&presenter->mutex);
    return result;
}

int sharp_presenter_get_stats(
    const struct sharp_presenter *presenter,
    struct sharp_presenter_stats *stats)
{
    if (presenter == NULL || stats == NULL) {
        return -1;
    }
    pthread_mutex_lock((pthread_mutex_t *)&presenter->mutex);
    *stats = presenter->stats;
    pthread_mutex_unlock((pthread_mutex_t *)&presenter->mutex);
    return 0;
}

int sharp_presenter_restore(struct sharp_presenter *presenter)
{
    struct drm_mode_rect full_damage;
    uint32_t damage_blob = 0;
    bool changed;
    int result = 0;
    unsigned int index;

    if (presenter == NULL) {
        return 0;
    }
    if (presenter->mutex_initialized) {
        pthread_mutex_lock(&presenter->mutex);
        presenter->stopping = true;
        for (index = 0; index < SHARP_PRESENTER_BUFFER_COUNT; index++) {
            if (presenter->buffers[index].state == SHARP_BUFFER_READY ||
                presenter->buffers[index].state == SHARP_BUFFER_RENDERING) {
                presenter->buffers[index].state = SHARP_BUFFER_FREE;
            }
        }
        if (presenter->condition_initialized) {
            pthread_cond_broadcast(&presenter->condition);
        }
        pthread_mutex_unlock(&presenter->mutex);
    }
    if (presenter->worker_started) {
        pthread_join(presenter->worker, NULL);
        presenter->worker_started = false;
    }
    if (!presenter->mutex_initialized) {
        return 0;
    }
    pthread_mutex_lock(&presenter->mutex);
    changed = presenter->display_changed;
    pthread_mutex_unlock(&presenter->mutex);
    if (!changed || presenter->sharp_fd == -1) {
        return 0;
    }
    full_damage = (struct drm_mode_rect){
        .x1 = 0,
        .y1 = 0,
        .x2 = (int16_t)presenter->width,
        .y2 = (int16_t)presenter->height,
    };
    if (create_damage_blob(presenter, &full_damage, 1, &damage_blob) != 0) {
        return -1;
    }
    result = commit_framebuffer(presenter, (uint32_t)presenter->plane.fb_id,
                                damage_blob);
    drmModeDestroyPropertyBlob(presenter->sharp_fd, damage_blob);
    if (result == 0) {
        pthread_mutex_lock(&presenter->mutex);
        presenter->display_changed = false;
        presenter->active_index = -1;
        for (index = 0; index < SHARP_PRESENTER_BUFFER_COUNT; index++) {
            presenter->buffers[index].state = SHARP_BUFFER_FREE;
        }
        pthread_mutex_unlock(&presenter->mutex);
    }
    return result;
}

void sharp_presenter_destroy(struct sharp_presenter *presenter)
{
    unsigned int index;

    if (presenter == NULL) {
        return;
    }
    sharp_presenter_restore(presenter);
    for (index = 0; index < SHARP_PRESENTER_BUFFER_COUNT; index++) {
        destroy_buffer(presenter, &presenter->buffers[index]);
    }
    if (presenter->gbm != NULL) {
        gbm_device_destroy(presenter->gbm);
    }
    if (presenter->render_fd != -1) {
        close(presenter->render_fd);
    }
    if (presenter->sharp_fd != -1) {
        close(presenter->sharp_fd);
    }
    if (presenter->event_fd != -1) {
        close(presenter->event_fd);
    }
    if (presenter->condition_initialized) {
        pthread_cond_destroy(&presenter->condition);
    }
    if (presenter->mutex_initialized) {
        pthread_mutex_destroy(&presenter->mutex);
    }
    free(presenter);
}
