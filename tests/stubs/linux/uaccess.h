#ifndef TESTS_STUBS_LINUX_UACCESS_H_
#define TESTS_STUBS_LINUX_UACCESS_H_

#include <string.h>

#define __user

static inline unsigned long copy_from_user(void *destination,
	const void *source, unsigned long count)
{
	memcpy(destination, source, count);
	return 0;
}

#endif
