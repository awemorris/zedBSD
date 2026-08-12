#include "kern/swap.h"
#include "kern/vm-commit.h"

#include <assert.h>
#include <errno.h>
#include <hal/hal.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static struct swap_backend backend;
static int use_swap;

bool hal_irq_disable(void) { return false; }
void hal_irq_enable(void) { }

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
	uint64_t expected_pages = 320U - VM_COMMIT_SYSTEM_RESERVE_PAGES;
	size_t exact;

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
