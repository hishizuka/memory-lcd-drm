#ifndef TESTS_STUBS_LINUX_BITMAP_H_
#define TESTS_STUBS_LINUX_BITMAP_H_

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define BITS_PER_LONG (sizeof(unsigned long) * 8u)
#define BITS_TO_LONGS(nbits) (((nbits) + BITS_PER_LONG - 1u) / BITS_PER_LONG)

static inline void set_bit(unsigned int bit, unsigned long *bitmap)
{
	bitmap[bit / BITS_PER_LONG] |= 1ul << (bit % BITS_PER_LONG);
}

static inline void clear_bit(unsigned int bit, unsigned long *bitmap)
{
	bitmap[bit / BITS_PER_LONG] &= ~(1ul << (bit % BITS_PER_LONG));
}

static inline bool test_bit(unsigned int bit, const unsigned long *bitmap)
{
	return (bitmap[bit / BITS_PER_LONG]
		& (1ul << (bit % BITS_PER_LONG))) != 0;
}

static inline void bitmap_zero(unsigned long *bitmap, unsigned int nbits)
{
	memset(bitmap, 0, BITS_TO_LONGS(nbits) * sizeof(*bitmap));
}

static inline bool bitmap_empty(const unsigned long *bitmap, unsigned int nbits)
{
	unsigned int bit;

	for (bit = 0; bit < nbits; bit++) {
		if (test_bit(bit, bitmap)) {
			return false;
		}
	}
	return true;
}

static inline bool bitmap_full(const unsigned long *bitmap, unsigned int nbits)
{
	unsigned int bit;

	for (bit = 0; bit < nbits; bit++) {
		if (!test_bit(bit, bitmap)) {
			return false;
		}
	}
	return true;
}

#define for_each_set_bit(bit, bitmap, nbits) \
	for ((bit) = 0; (bit) < (nbits); (bit)++) \
		if (test_bit((unsigned int)(bit), (bitmap)))

static inline unsigned long find_next_bit(const unsigned long *bitmap,
	unsigned long nbits, unsigned long offset)
{
	while (offset < nbits && !test_bit((unsigned int)offset, bitmap)) {
		offset++;
	}
	return offset;
}

#endif
