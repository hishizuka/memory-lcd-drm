#ifndef TESTS_STUBS_LINUX_DELAY_H_
#define TESTS_STUBS_LINUX_DELAY_H_

static inline void ndelay(unsigned long nsecs)
{
	(void)nsecs;
}

#endif
