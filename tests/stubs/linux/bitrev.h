#ifndef TESTS_STUBS_LINUX_BITREV_H_
#define TESTS_STUBS_LINUX_BITREV_H_

#include <linux/types.h>

static inline u8 bitrev8(u8 value)
{
	value = (u8)(((value & 0x55u) << 1) | ((value & 0xAAu) >> 1));
	value = (u8)(((value & 0x33u) << 2) | ((value & 0xCCu) >> 2));
	return (u8)((value << 4) | (value >> 4));
}

#endif
