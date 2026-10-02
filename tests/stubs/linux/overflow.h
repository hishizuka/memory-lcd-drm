#ifndef TESTS_STUBS_LINUX_OVERFLOW_H_
#define TESTS_STUBS_LINUX_OVERFLOW_H_

#define check_add_overflow(a, b, result) \
	__builtin_add_overflow((a), (b), (result))
#define check_mul_overflow(a, b, result) \
	__builtin_mul_overflow((a), (b), (result))

#endif
