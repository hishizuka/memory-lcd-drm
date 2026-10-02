#ifndef TESTS_STUBS_LINUX_JIFFIES_H_
#define TESTS_STUBS_LINUX_JIFFIES_H_

#include <linux/types.h>

static inline unsigned long msecs_to_jiffies(unsigned int milliseconds)
{
	return milliseconds;
}

#endif
