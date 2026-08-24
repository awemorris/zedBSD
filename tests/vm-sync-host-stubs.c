/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
/* Native-thread synchronization used only by the VM host test. */
#include <kern/lock.h>
#include <kern/waitq.h>

#include <assert.h>
#include <errno.h>
#include <threads.h>

/* ASan quarantines freed backings, so their embedded waitq addresses are not
 * promptly reused during the long combined VM test. */
#define HOST_WAITQ_MAX 512U

struct host_waitq {
	struct wait_queue *queue;
	mtx_t mutex;
	cnd_t condition;
};

static once_flag host_waitq_once = ONCE_FLAG_INIT;
static mtx_t host_waitq_registry_lock;
static cnd_t host_sleep_condition;
static unsigned host_sleep_count;
static struct host_waitq host_waitqs[HOST_WAITQ_MAX];

static void
host_waitq_registry_init(void)
{
	assert(mtx_init(&host_waitq_registry_lock, mtx_plain) == thrd_success);
	assert(cnd_init(&host_sleep_condition) == thrd_success);
}

unsigned
vm_test_waitq_sleep_count(void)
{
	unsigned count;
	call_once(&host_waitq_once, host_waitq_registry_init);
	assert(mtx_lock(&host_waitq_registry_lock) == thrd_success);
	count = host_sleep_count;
	assert(mtx_unlock(&host_waitq_registry_lock) == thrd_success);
	return count;
}

void
vm_test_waitq_wait_for_sleep(unsigned target)
{
	call_once(&host_waitq_once, host_waitq_registry_init);
	assert(mtx_lock(&host_waitq_registry_lock) == thrd_success);
	while (host_sleep_count < target)
		assert(cnd_wait(&host_sleep_condition,
		    &host_waitq_registry_lock) == thrd_success);
	assert(mtx_unlock(&host_waitq_registry_lock) == thrd_success);
}

static struct host_waitq *
host_waitq_find(struct wait_queue *queue, int create)
{
	struct host_waitq *empty = NULL;
	unsigned i;

	call_once(&host_waitq_once, host_waitq_registry_init);
	assert(mtx_lock(&host_waitq_registry_lock) == thrd_success);
	for (i = 0; i < HOST_WAITQ_MAX; i++) {
		if (host_waitqs[i].queue == queue) {
			assert(mtx_unlock(&host_waitq_registry_lock) == thrd_success);
			return &host_waitqs[i];
		}
		if (empty == NULL && host_waitqs[i].queue == NULL)
			empty = &host_waitqs[i];
	}
	if (create && empty != NULL) {
		assert(mtx_init(&empty->mutex, mtx_plain) == thrd_success);
		assert(cnd_init(&empty->condition) == thrd_success);
		empty->queue = queue;
	}
	assert(mtx_unlock(&host_waitq_registry_lock) == thrd_success);
	assert(!create || empty != NULL);
	return empty;
}

void
spin_init(struct spinlock *lock, enum lock_rank rank, const char *name)
{
	lock->held.value = 0;
	lock->rank = rank;
	lock->name = name;
	lock->owner_cpu = 0;
	lock->owner_valid = 0;
}

void
spin_lock(struct spinlock *lock)
{
	while (!atomic_try_acquire_zero(&lock->held))
		thrd_yield();
}

int spin_trylock(struct spinlock *lock)
{ return atomic_try_acquire_zero(&lock->held); }

void spin_unlock(struct spinlock *lock)
{ atomic_store_release(&lock->held, 0); }

unsigned long spin_lock_irqsave(struct spinlock *lock)
{ spin_lock(lock); return 0; }

void spin_unlock_irqrestore(struct spinlock *lock, unsigned long irq)
{ (void)irq; spin_unlock(lock); }

static _Thread_local unsigned host_mutex_identity;

static struct thread *
host_mutex_owner(void)
{
	return (struct thread *)&host_mutex_identity;
}

int
mutex_init(struct mutex *mutex, enum lock_rank rank, const char *name)
{
	if (mutex == NULL)
		return EINVAL;
	spin_init(&mutex->guard, rank, name);
	mutex->owner = NULL;
	mutex->locked = 0;
	mutex->waiters.head = mutex->waiters.tail = NULL;
	mutex->waiters.sequence = 1;
	mutex->waiters.name = name;
	return 0;
}

int
mutex_trylock(struct mutex *mutex)
{
	int acquired = 0;
	spin_lock(&mutex->guard);
	assert(mutex->owner != host_mutex_owner());
	if (!mutex->locked) {
		mutex->locked = 1;
		mutex->owner = host_mutex_owner();
		acquired = 1;
	}
	spin_unlock(&mutex->guard);
	return acquired;
}

int
mutex_owned(struct mutex *mutex)
{
	int owned;
	spin_lock(&mutex->guard);
	owned = mutex->locked && mutex->owner == host_mutex_owner();
	spin_unlock(&mutex->guard);
	return owned;
}

int
mutex_lock_interruptible(struct mutex *mutex)
{
	while (!mutex_trylock(mutex))
		thrd_yield();
	return 0;
}

void
mutex_lock(struct mutex *mutex)
{
	(void)mutex_lock_interruptible(mutex);
}

void
mutex_unlock(struct mutex *mutex)
{
	spin_lock(&mutex->guard);
	assert(mutex->locked && mutex->owner == host_mutex_owner());
	mutex->owner = NULL;
	mutex->locked = 0;
	spin_unlock(&mutex->guard);
}

int
mutex_wait(struct mutex *mutex, struct wait_queue *condition,
	uint64_t observed, uint64_t deadline, unsigned flags)
{
	int error;
	assert(mutex->locked && mutex->owner == host_mutex_owner());
	mutex_unlock(mutex);
	spin_lock(&mutex->guard);
	error = waitq_sleep(condition, &mutex->guard, observed, deadline, flags);
	spin_unlock(&mutex->guard);
	mutex_lock(mutex);
	return error;
}

void
waitq_init(struct wait_queue *queue, const char *name)
{
	queue->head = queue->tail = NULL;
	__atomic_store_n(&queue->sequence, 1, __ATOMIC_RELEASE);
	queue->name = name;
	(void)host_waitq_find(queue, 1);
}

uint64_t waitq_sequence(const struct wait_queue *queue)
{ return __atomic_load_n(&queue->sequence, __ATOMIC_ACQUIRE); }

int
waitq_sleep(struct wait_queue *queue, struct spinlock *lock,
	uint64_t observed, uint64_t deadline, unsigned flags)
{
	struct host_waitq *host;
	(void)deadline;
	if (queue == NULL || lock == NULL || flags != 0)
		return EINVAL;
	host = host_waitq_find(queue, 0);
	assert(host != NULL);
	assert(mtx_lock(&host->mutex) == thrd_success);
	if (waitq_sequence(queue) != observed) {
		assert(mtx_unlock(&host->mutex) == thrd_success);
		return EAGAIN;
	}
	assert(mtx_lock(&host_waitq_registry_lock) == thrd_success);
	host_sleep_count++;
	assert(cnd_broadcast(&host_sleep_condition) == thrd_success);
	assert(mtx_unlock(&host_waitq_registry_lock) == thrd_success);
	spin_unlock(lock);
	while (waitq_sequence(queue) == observed)
		assert(cnd_wait(&host->condition, &host->mutex) == thrd_success);
	assert(mtx_unlock(&host->mutex) == thrd_success);
	spin_lock(lock);
	return 0;
}

void
waitq_wake_one(struct wait_queue *queue)
{
	struct host_waitq *host = host_waitq_find(queue, 0);
	assert(host != NULL && mtx_lock(&host->mutex) == thrd_success);
	(void)__atomic_add_fetch(&queue->sequence, 1, __ATOMIC_RELEASE);
	assert(cnd_signal(&host->condition) == thrd_success);
	assert(mtx_unlock(&host->mutex) == thrd_success);
}

void
waitq_wake_all(struct wait_queue *queue)
{
	struct host_waitq *host = host_waitq_find(queue, 0);
	assert(host != NULL && mtx_lock(&host->mutex) == thrd_success);
	(void)__atomic_add_fetch(&queue->sequence, 1, __ATOMIC_RELEASE);
	assert(cnd_broadcast(&host->condition) == thrd_success);
	assert(mtx_unlock(&host->mutex) == thrd_success);
}

void __attribute__((weak))
vm_commit_release(size_t bytes)
{
	(void)bytes;
}

struct file;

void __attribute__((weak))
record_lock_release_file(struct file *file)
{
	(void)file;
}
