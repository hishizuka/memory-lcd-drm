#ifndef TESTS_STUBS_LINUX_WAIT_H_
#define TESTS_STUBS_LINUX_WAIT_H_

typedef struct {
	int unused;
} wait_queue_head_t;

static inline void init_waitqueue_head(wait_queue_head_t *wait)
{
	wait->unused = 0;
}

static inline void wake_up_all(wait_queue_head_t *wait)
{
	(void)wait;
}

#endif
