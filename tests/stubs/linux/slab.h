#ifndef TESTS_STUBS_LINUX_SLAB_H_
#define TESTS_STUBS_LINUX_SLAB_H_

#include <stdlib.h>

#define GFP_KERNEL 0

static inline void *kcalloc(size_t n, size_t size, int flags)
{
	(void)flags;
	return calloc(n, size);
}

static inline void kfree(void *ptr)
{
	free(ptr);
}

#endif
