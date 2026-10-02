/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SHARP_PARAMS_INTERNAL_H_
#define SHARP_PARAMS_INTERNAL_H_

#include <linux/ctype.h>
#include <linux/kernel.h>
#include <linux/overflow.h>
#include <linux/types.h>

static inline int parse_seconds_ms(const char *val, const char **endp, unsigned int *out_ms)
{
	const char *p = val;
	u64 whole = 0;
	u64 frac = 0;
	u64 ms = 0;
	int frac_digits = 0;
	bool saw_digit = false;

	while (*p && isspace(*p)) {
		p++;
	}

	while (*p && isdigit(*p)) {
		saw_digit = true;
		if (check_mul_overflow(whole, 10u, &whole) ||
		    check_add_overflow(whole, (u64)(*p - '0'), &whole)) {
			return -ERANGE;
		}
		p++;
	}

	if (*p == '.') {
		p++;
		while (*p && isdigit(*p)) {
			if (frac_digits < 3) {
				frac = (frac * 10u) + (u64)(*p - '0');
				frac_digits++;
			}
			p++;
		}
	}

	if (!saw_digit && frac_digits == 0) {
		return -EINVAL;
	}

	while (frac_digits < 3) {
		frac *= 10u;
		frac_digits++;
	}

	while (*p && isspace(*p)) {
		p++;
	}

	if (check_mul_overflow(whole, 1000u, &ms) ||
	    check_add_overflow(ms, frac, &ms) || ms > U32_MAX) {
		return -ERANGE;
	}

	*out_ms = (unsigned int)ms;
	if (*out_ms == 0 && (whole || frac)) {
		*out_ms = 1;
	}

	if (endp) {
		*endp = p;
	}

	return 0;
}


#endif
