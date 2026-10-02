#ifndef SHARP_PRESENTER_H
#define SHARP_PRESENTER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHARP_PRESENTER_BUFFER_COUNT 2
#define SHARP_PRESENTER_ACQUIRED 0
#define SHARP_PRESENTER_BUSY 1
#define SHARP_PRESENTER_ERROR (-1)
#define SHARP_PRESENTER_NO_BUFFER UINT32_MAX

struct sharp_presenter;

struct sharp_presenter_stats {
    uint64_t submitted_frames;
    uint64_t presented_frames;
    uint64_t replaced_pending_frames;
    uint64_t busy_acquires;
    uint64_t commit_errors;
    uint64_t unchanged_frames;
    uint64_t damage_rows;
};

struct sharp_presenter *sharp_presenter_create(
    const char *render_node, const char *sharp_card, uint32_t width,
    uint32_t height);
const char *sharp_presenter_last_error(
    const struct sharp_presenter *presenter);
int sharp_presenter_acquire(
    struct sharp_presenter *presenter, uint32_t *buffer_index,
    uint32_t *renderbuffer);
int sharp_presenter_submit(
    struct sharp_presenter *presenter, uint32_t buffer_index, uint32_t y,
    uint32_t height);
int sharp_presenter_submit_auto_damage(
    struct sharp_presenter *presenter, uint32_t buffer_index);
int sharp_presenter_event_fd(const struct sharp_presenter *presenter);
int sharp_presenter_dispatch(struct sharp_presenter *presenter);
uint32_t sharp_presenter_stride(
    const struct sharp_presenter *presenter, uint32_t buffer_index);
uint64_t sharp_presenter_modifier(
    const struct sharp_presenter *presenter, uint32_t buffer_index);
int sharp_presenter_dump_active_ppm(
    struct sharp_presenter *presenter, const char *path);
int sharp_presenter_get_stats(
    const struct sharp_presenter *presenter,
    struct sharp_presenter_stats *stats);
int sharp_presenter_restore(struct sharp_presenter *presenter);
void sharp_presenter_destroy(struct sharp_presenter *presenter);

#ifdef __cplusplus
}
#endif

#endif
