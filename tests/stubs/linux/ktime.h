#ifndef TESTS_STUBS_LINUX_KTIME_H_
#define TESTS_STUBS_LINUX_KTIME_H_

#include <linux/types.h>

static inline u64 ktime_get_ns(void)
{
	static u64 now;

	return ++now;
}

#endif
