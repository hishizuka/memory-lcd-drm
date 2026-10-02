#ifndef TESTS_STUBS_LINUX_MATH64_H_
#define TESTS_STUBS_LINUX_MATH64_H_

#include <linux/types.h>

static inline u64 div64_u64(u64 dividend, u64 divisor)
{
	return dividend / divisor;
}

#endif
