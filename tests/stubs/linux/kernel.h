#ifndef TESTS_STUBS_LINUX_KERNEL_H_
#define TESTS_STUBS_LINUX_KERNEL_H_

#include <stddef.h>
#include <stdint.h>

#ifndef U64_MAX
#define U64_MAX UINT64_MAX
#endif

#ifndef U32_MAX
#define U32_MAX UINT32_MAX
#endif

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#ifndef container_of
#define container_of(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))
#endif

#ifndef min
#define min(x, y) ((x) < (y) ? (x) : (y))
#endif

#ifndef max
#define max(x, y) ((x) > (y) ? (x) : (y))
#endif

#define min_t(type, x, y) ({ \
	type __min1 = (x); \
	type __min2 = (y); \
	__min1 < __min2 ? __min1 : __min2; \
})

#define max_t(type, x, y) ({ \
	type __max1 = (x); \
	type __max2 = (y); \
	__max1 > __max2 ? __max1 : __max2; \
})

#ifndef DIV_ROUND_UP
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#endif

#define READ_ONCE(value) (value)
#define WRITE_ONCE(value, new_value) ((value) = (new_value))

#ifndef WARN_ON_ONCE
#define WARN_ON_ONCE(condition) (!!(condition))
#endif

#define dev_err(dev, fmt, ...) ((void)(dev))
#define dev_warn(dev, fmt, ...) ((void)(dev))
#define pr_warn(...) ((void)0)
#define dev_warn_ratelimited(dev, fmt, ...) ((void)(dev))

#define IS_ERR(ptr) ((uintptr_t)(ptr) >= (uintptr_t)-4095)
#define IS_ERR_OR_NULL(ptr) (!(ptr) || IS_ERR(ptr))
#define PTR_ERR(ptr) ((long)(intptr_t)(ptr))

#endif
