// SPDX-License-Identifier: GPL-2.0-or-later
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "../src/params_internal.h"

static void check_value(const char *text, unsigned int expected)
{
	const char *end;
	unsigned int value;

	assert(parse_seconds_ms(text, &end, &value) == 0);
	assert(*end == '\0');
	assert(value == expected);
}

int main(void)
{
	unsigned int value;
	const char *end;

	check_value("0", 0);
	check_value(".001", 1);
	check_value(" 1.25 ", 1250);
	check_value("1.2349", 1234);
	check_value("4294967.295", U32_MAX);
	assert(parse_seconds_ms("4294967.296", NULL, &value) == -ERANGE);
	assert(parse_seconds_ms("18446744073709552", NULL, &value) == -ERANGE);
	assert(parse_seconds_ms("18446744073709551616.001", NULL, &value)
		== -ERANGE);
	assert(parse_seconds_ms("999999999999999999999999999", NULL, &value)
		== -ERANGE);
	assert(parse_seconds_ms("", NULL, &value) == -EINVAL);
	assert(parse_seconds_ms(".", NULL, &value) == -EINVAL);
	assert(parse_seconds_ms("-1", NULL, &value) == -EINVAL);
	assert(parse_seconds_ms("2.5,0.1", &end, &value) == 0);
	assert(value == 2500 && strcmp(end, ",0.1") == 0);
	puts("all parameter parser cases passed");
	return 0;
}
