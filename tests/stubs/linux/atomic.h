#ifndef TESTS_STUBS_LINUX_ATOMIC_H_
#define TESTS_STUBS_LINUX_ATOMIC_H_

typedef int atomic_t;
typedef long long atomic64_t;

static inline void atomic64_add(long long value, atomic64_t *counter)
{
	*counter += value;
}

static inline long long atomic64_read(const atomic64_t *counter)
{
	return *counter;
}

static inline long long atomic64_cmpxchg(atomic64_t *counter,
	long long old, long long new_value)
{
	long long previous = *counter;

	if (previous == old) {
		*counter = new_value;
	}
	return previous;
}

static inline void atomic64_set(atomic64_t *counter, long long value)
{
	*counter = value;
}

#endif
