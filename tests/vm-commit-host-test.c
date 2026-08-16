#include "kern/swap.h"
#include "kern/lock.h"
#include "kern/vm-commit.h"

#include <assert.h>
#include <errno.h>
#include <hal/hal.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static struct swap_backend backend;
static int use_swap;

struct reserve_worker { uint64_t pages; };

static void *
reserve_all(void *argument)
{
	struct reserve_worker *worker=argument;
	while(vm_commit_reserve(VM_COMMIT_PAGE_SIZE)==0)
		worker->pages++;
	return NULL;
}

bool hal_irq_disable(void) { return false; }
void hal_irq_enable(void) { }
void spin_init(struct spinlock *lock,enum lock_rank rank,const char *name)
{ lock->held.value=0;lock->rank=rank;lock->name=name;lock->owner_cpu=0;lock->owner_valid=0; }
void spin_lock(struct spinlock *lock)
{ while(!atomic_try_acquire_zero(&lock->held)); }
int spin_trylock(struct spinlock *lock)
{ return atomic_try_acquire_zero(&lock->held); }
void spin_unlock(struct spinlock *lock)
{ atomic_store_release(&lock->held,0); }
unsigned long spin_lock_irqsave(struct spinlock *lock)
{ spin_lock(lock);return 0; }
void spin_unlock_irqrestore(struct spinlock *lock,unsigned long irq)
{ (void)irq;spin_unlock(lock); }

void
hal_memory_get_stats(struct hal_memory_stats *stats)
{
	memset(stats, 0, sizeof(*stats));
	stats->physical_free = 320U * VM_COMMIT_PAGE_SIZE;
}

struct swap_backend *swap_system_backend(void)
{
	return use_swap ? &backend : NULL;
}

int swap_get_stats(struct swap_backend *swap, uint32_t *total,
		   uint32_t *free_slots)
{
	if (swap == NULL || total == NULL || free_slots == NULL)
		return EINVAL;
	*total = swap->slot_count;
	*free_slots = swap->free_slots;
	return 0;
}

void
hal_fatal(const char *file, int line, const char *message)
{
	fprintf(stderr, "%s:%d: %s\n", file, line, message);
	abort();
}

static void
run_case(int swap_enabled)
{
	struct vm_commit_stats stats, after;
	struct reserve_worker workers[8];
	pthread_t threads[8];
	uint64_t expected_pages = 320U - VM_COMMIT_SYSTEM_RESERVE_PAGES;
	uint64_t reserved=0;
	size_t exact;
	unsigned index;

	memset(&backend, 0, sizeof(backend));
	backend.slot_count = 128;
	use_swap = swap_enabled;
	if (swap_enabled)
		expected_pages += backend.slot_count;
	assert(vm_commit_init() == 0);
	assert(vm_commit_init() == EBUSY);
	vm_commit_get_stats(&stats);
	assert(stats.physical_pages == 256);
	assert(stats.swap_pages == (swap_enabled ? 128U : 0U));
	assert(stats.limit_pages == expected_pages && stats.used_pages == 0);
	assert(vm_commit_reserve(0) == EINVAL);
	assert(vm_commit_reserve(1) == EINVAL);
	exact = (size_t)expected_pages * VM_COMMIT_PAGE_SIZE;
	assert(vm_commit_reserve(exact) == 0);
	vm_commit_get_stats(&stats);
	assert(stats.used_pages == expected_pages);
	assert(vm_commit_reserve(VM_COMMIT_PAGE_SIZE) == ENOMEM);
	vm_commit_get_stats(&after);
	assert(!memcmp(&stats, &after, sizeof(stats)));
	assert(vm_commit_can_shutdown_swap() == (!swap_enabled));
	vm_commit_release(exact);
	assert(vm_commit_reserve(VM_COMMIT_PAGE_SIZE) == 0);
	vm_commit_release(VM_COMMIT_PAGE_SIZE);
	assert(vm_commit_can_shutdown_swap());
	memset(workers,0,sizeof(workers));
	for(index=0;index<8;index++)
		assert(pthread_create(&threads[index],NULL,reserve_all,
		    &workers[index])==0);
	for(index=0;index<8;index++) {
		assert(pthread_join(threads[index],NULL)==0);
		reserved+=workers[index].pages;
	}
	assert(reserved==expected_pages);
	vm_commit_get_stats(&stats);
	assert(stats.used_pages==expected_pages);
	for(index=0;index<8;index++)
		if(workers[index].pages!=0)
			vm_commit_release((size_t)workers[index].pages*
			    VM_COMMIT_PAGE_SIZE);
	vm_commit_get_stats(&stats);
	assert(stats.used_pages==0);
}

static void
wait_success(pid_t child)
{
	int status;
	assert(waitpid(child, &status, 0) == child);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int
main(void)
{
	pid_t child = fork();
	assert(child >= 0);
	if (child == 0) {
		run_case(0);
		_exit(0);
	}
	wait_success(child);
	child = fork();
	assert(child >= 0);
	if (child == 0) {
		run_case(1);
		_exit(0);
	}
	wait_success(child);
	puts("zedBSD strict VM commit host tests: PASS");
	return 0;
}
