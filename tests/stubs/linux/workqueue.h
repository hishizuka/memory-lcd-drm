#ifndef TESTS_STUBS_LINUX_WORKQUEUE_H_
#define TESTS_STUBS_LINUX_WORKQUEUE_H_

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

struct workqueue_struct;
struct work_struct {
	void (*func)(struct work_struct *work);
	struct work_struct *next;
	struct workqueue_struct *queue;
	bool pending;
};
struct delayed_work {
	struct work_struct work;
	struct delayed_work *next;
	struct workqueue_struct *queue;
	unsigned long delay;
	bool scheduled;
};
struct workqueue_struct {
	struct work_struct *head;
	struct work_struct *tail;
	struct delayed_work *delayed;
};

#define WQ_MEM_RECLAIM 0u
#define WQ_FREEZABLE 0u
#define INIT_WORK(item, callback) \
	(*(item) = (struct work_struct){ .func = (callback) })
#define INIT_DELAYED_WORK(item, callback) \
	(*(item) = (struct delayed_work){ .work.func = (callback) })
#define to_delayed_work(item) \
	((struct delayed_work *)((char *)(item) - offsetof(struct delayed_work, work)))

static inline struct workqueue_struct *alloc_ordered_workqueue(
	const char *name, unsigned int flags)
{
	(void)name;
	(void)flags;
	return calloc(1, sizeof(struct workqueue_struct));
}

static inline bool queue_work(struct workqueue_struct *wq,
	struct work_struct *work)
{
	if (work->pending) {
		return false;
	}
	work->pending = true;
	work->queue = wq;
	work->next = NULL;
	if (wq->tail) {
		wq->tail->next = work;
	} else {
		wq->head = work;
	}
	wq->tail = work;
	return true;
}

static inline bool test_workqueue_run_one(struct workqueue_struct *wq)
{
	struct work_struct *work = wq->head;

	if (!work) {
		return false;
	}
	wq->head = work->next;
	if (!wq->head) {
		wq->tail = NULL;
	}
	/* Match the kernel: clear PENDING before calling the worker. */
	work->pending = false;
	work->queue = NULL;
	work->next = NULL;
	work->func(work);
	return true;
}

static inline void flush_workqueue(struct workqueue_struct *wq)
{
	unsigned int budget = 10000;

	while (test_workqueue_run_one(wq)) {
		assert(--budget);
	}
}

static inline bool cancel_work_sync(struct work_struct *work)
{
	struct workqueue_struct *wq = work->queue;
	struct work_struct **link;
	struct work_struct *previous = NULL;

	if (!work->pending) {
		return false;
	}
	for (link = &wq->head; *link; link = &(*link)->next) {
		if (*link == work) {
			*link = work->next;
			if (wq->tail == work) {
				wq->tail = previous;
			}
			work->pending = false;
			work->queue = NULL;
			work->next = NULL;
			return true;
		}
		previous = *link;
	}
	assert(false);
	return false;
}

static inline bool cancel_delayed_work(struct delayed_work *work)
{
	bool canceled = cancel_work_sync(&work->work);

	if (work->scheduled) {
		struct delayed_work **link = &work->queue->delayed;

		while (*link != work) {
			assert(*link);
			link = &(*link)->next;
		}
		*link = work->next;
		work->scheduled = false;
		work->queue = NULL;
		work->next = NULL;
		canceled = true;
	}
	return canceled;
}

static inline bool cancel_delayed_work_sync(struct delayed_work *work)
{
	return cancel_delayed_work(work);
}

static inline bool mod_delayed_work(struct workqueue_struct *wq,
	struct delayed_work *work, unsigned long delay)
{
	bool pending = cancel_delayed_work(work);

	work->queue = wq;
	work->delay = delay;
	work->scheduled = true;
	work->next = wq->delayed;
	wq->delayed = work;
	return pending;
}

static inline void test_workqueue_advance(struct workqueue_struct *wq,
	unsigned long elapsed)
{
	struct delayed_work **link = &wq->delayed;

	while (*link) {
		struct delayed_work *work = *link;

		if (work->delay > elapsed) {
			work->delay -= elapsed;
			link = &work->next;
			continue;
		}
		*link = work->next;
		work->scheduled = false;
		work->queue = NULL;
		work->next = NULL;
		queue_work(wq, &work->work);
	}
}

static inline void destroy_workqueue(struct workqueue_struct *wq)
{
	assert(!wq->head && !wq->delayed);
	free(wq);
}

#endif
