#ifndef TESTS_STUBS_LINUX_MUTEX_H_
#define TESTS_STUBS_LINUX_MUTEX_H_

#include <assert.h>
#include <stdbool.h>

#ifdef TEST_REAL_MUTEX
#include <pthread.h>
struct mutex {
	pthread_mutex_t native;
};
#define __MUTEX_INITIALIZER(name) { .native = PTHREAD_MUTEX_INITIALIZER }
static inline void mutex_lock(struct mutex *lock)
{
	int rc = pthread_mutex_lock(&lock->native);
	assert(rc == 0);
}
static inline void mutex_unlock(struct mutex *lock)
{
	int rc = pthread_mutex_unlock(&lock->native);
	assert(rc == 0);
}
#else
struct mutex {
	bool locked;
};

#define lockdep_assert_held(lock) assert((lock)->locked)

static inline void mutex_lock(struct mutex *lock)
{
	assert(!lock->locked);
	lock->locked = true;
}

static inline void mutex_unlock(struct mutex *lock)
{
	assert(lock->locked);
	lock->locked = false;
}
#endif

#endif
