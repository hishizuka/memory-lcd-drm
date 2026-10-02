#ifndef TESTS_STUBS_LINUX_STRING_H_
#define TESTS_STUBS_LINUX_STRING_H_

#include <string.h>

static inline int sysfs_streq(const char *left, const char *right)
{
	size_t left_len = strlen(left);
	size_t right_len = strlen(right);

	if (left_len && left[left_len - 1] == '\n') {
		left_len--;
	}
	return left_len == right_len
		&& memcmp(left, right, right_len) == 0;
}

#endif
