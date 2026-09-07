/* Shared host-only policy mutex and IRQ services. */
#include <kern/cache-memory.h>
#include <kern/lock.h>
#include <sched.h>

bool hal_irq_disable(void) { return false; }
void hal_irq_enable(void) { }
int mutex_init(struct mutex *lock, enum lock_rank rank, const char *name)
{
	memset(lock, 0, sizeof(*lock));
	lock->guard.rank = rank;
	lock->guard.name = name;
	return 0;
}
void mutex_lock(struct mutex *lock)
{
	unsigned expected = 0;
	while (!atomic_raw_compare_exchange(&lock->locked, &expected, 1)) {
		expected = 0;
		sched_yield();
	}
}
void mutex_unlock(struct mutex *lock) { atomic_raw_store_release(&lock->locked, 0); }
