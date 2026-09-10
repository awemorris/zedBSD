/* Link-only output operation counts; includes concurrent external callers. */
#include <hal/hal.h>
#include <kern/vmspace.h>

static unsigned active;
static uint64_t releases;
static uint64_t calls[5];
static uint64_t pages_seen[5];

static void
record(unsigned kind, size_t pages)
{
	if (!__atomic_load_n(&active, __ATOMIC_ACQUIRE))
		return;
	__atomic_add_fetch(&calls[kind], 1, __ATOMIC_RELAXED);
	__atomic_add_fetch(&pages_seen[kind], pages, __ATOMIC_RELAXED);
}

int __real_vmspace_user_lease_acquire(const struct vmspace_pinned_page *, size_t,
    struct vmspace_user_lease **);
int
__wrap_vmspace_user_lease_acquire(const struct vmspace_pinned_page *pages,
    size_t count, struct vmspace_user_lease **result)
{
	__atomic_store_n(&active, 1, __ATOMIC_RELEASE);
	return __real_vmspace_user_lease_acquire(pages, count, result);
}

void __real_vmspace_user_lease_release(struct vmspace_user_lease *);
void
__wrap_vmspace_user_lease_release(struct vmspace_user_lease *lease)
{
	uint64_t count;
	unsigned index;

	__real_vmspace_user_lease_release(lease);
	if (!__atomic_load_n(&active, __ATOMIC_ACQUIRE))
		return;
	count = __atomic_add_fetch(&releases, 1, __ATOMIC_RELAXED);
	if (count % 256 != 0)
		return;
	for (index = 0; index < 5; index++) {
		hal_printf("OUTPUT-OPS leases=%llu kind=%u calls=%llu pages=%llu\n",
		    (unsigned long long)count, index,
		    (unsigned long long)__atomic_load_n(&calls[index], __ATOMIC_RELAXED),
		    (unsigned long long)__atomic_load_n(&pages_seen[index], __ATOMIC_RELAXED));
	}
}

int __real_hal_space_map(hal_space_t, void *, hal_physaddr_t, size_t, uint32_t);
int
__wrap_hal_space_map(hal_space_t space, void *address, hal_physaddr_t pa,
    size_t size, uint32_t attr)
{
	record(space == HAL_SPACE_SYS ? 0 : 1, size / 4096);
	return __real_hal_space_map(space, address, pa, size, attr);
}

int __real_hal_space_unmap(hal_space_t, void *, size_t);
int
__wrap_hal_space_unmap(hal_space_t space, void *address, size_t size)
{
	record(space == HAL_SPACE_SYS ? 2 : 3, size / 4096);
	return __real_hal_space_unmap(space, address, size);
}

int __real_vmspace_pin_user_pages(struct vmspace *, uintptr_t, size_t, uint32_t,
    struct vmspace_pinned_page *, size_t);
int
__wrap_vmspace_pin_user_pages(struct vmspace *vm, uintptr_t address, size_t size,
    uint32_t required, struct vmspace_pinned_page *pages, size_t count)
{
	record(4, count);
	return __real_vmspace_pin_user_pages(vm, address, size, required, pages, count);
}
