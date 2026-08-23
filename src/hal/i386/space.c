/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * i386 protected user address spaces.
 */
#include <hal/hal.h>
#include "apic-topology.h"
#include "defs.h"
#include "irq.h"
#include "smp.h"
#include "space.h"

_Static_assert((uintptr_t)(SYS_START - 1U) <= (uintptr_t)INTPTR_MAX,
    "user pointers must not overlap the negative syscall errno window");
_Static_assert(I386_APIC_MAX_CPUS <= 32U,
    "i386 TLB shootdown mask exceeds its storage");

static hal_space_t current_spaces[HAL_CPU_MAX];
#define current_space current_spaces[hal_cpu_current()]
static uint32_t system_cr3;
static int next_space_id;
static uint32_t space_count;
static uint32_t page_table_count;
static volatile unsigned count_lock;
static volatile unsigned shootdown_pool_lock;
static volatile unsigned space_registry_lock;
static struct i386_space *space_registry;

struct i386_shootdown_request {
	volatile unsigned active;
	hal_space_t space;
	void *vaddr;
	size_t size;
	volatile unsigned pending;
	volatile unsigned lock;
};

#define I386_SHOOTDOWN_REQUESTS (I386_APIC_MAX_CPUS * 4U)
static struct i386_shootdown_request shootdowns[I386_SHOOTDOWN_REQUESTS];

static void shootdown(hal_space_t handle, void *vaddr, size_t size);
static void service_shootdowns(hal_cpu_id_t cpu);

static bool
raw_lock_enter(volatile unsigned *lock)
{
	bool enabled = hal_irq_disable();

	for (;;) {
		unsigned previous = 1U;

		__asm__ volatile("xchgl %0, %1"
		    : "+r" (previous), "+m" (*lock) : : "memory");
		if (previous == 0U)
			return enabled;
		__asm__ volatile("pause");
	}
}

static void
raw_lock_leave(volatile unsigned *lock, bool enabled)
{
	unsigned previous = 0U;

	__asm__ volatile("xchgl %0, %1"
	    : "+r" (previous), "+m" (*lock) : : "memory");
	if (previous != 1U)
		HAL_FATAL("invalid i386 raw lock release");
	if (enabled)
		hal_irq_enable();
}

static int
space_op_enter(struct i386_space *space)
{
	struct i386_space *item;
	bool enabled = raw_lock_enter(&space_registry_lock);

	for (item = space_registry; item != NULL; item = item->registry_next)
		if (item == space)
			break;
	if (item == NULL || item->destroying) {
		raw_lock_leave(&space_registry_lock, enabled);
		return 0;
	}
	item->active_ops++;
	raw_lock_leave(&space_registry_lock, enabled);
	return 1;
}

static void
space_op_leave(struct i386_space *space)
{
	bool enabled = raw_lock_enter(&space_registry_lock);

	if (space->active_ops == 0)
		HAL_FATAL("i386 space operation counter underflow");
	space->active_ops--;
	raw_lock_leave(&space_registry_lock, enabled);
}

static void
table_count_drop(void)
{
	bool enabled = raw_lock_enter(&count_lock);

	if (page_table_count == 0)
		HAL_FATAL("i386 page-table counter underflow");
	page_table_count--;
	raw_lock_leave(&count_lock, enabled);
}

static int
alloc_page(struct hal_pmem *memory)
{
	const struct hal_pmem_request request = {
		HAL_PMEM_PADDR_ANY, PAGE_SIZE, PAGE_SIZE,
		HAL_PMEM_TYPE_RAM, 0
	};
	return hal_pmem_alloc(&request, memory);
}

static bool
space_lock_enter(struct i386_space *space)
{
	bool enabled = hal_irq_disable();

	for (;;) {
		unsigned previous = 1U;

		__asm__ volatile("xchgl %0, %1"
		    : "+r" (previous), "+m" (space->lock) : : "memory");
		if (previous == 0U)
			return enabled;
		/* Do not mask an IPI forever while waiting for the CPU which owns
		 * this serializer to complete its shootdown. */
		service_shootdowns(hal_cpu_current());
		__asm__ volatile("pause");
	}
}

static void
space_lock_leave(struct i386_space *space, bool enabled)
{
	raw_lock_leave(&space->lock, enabled);
}

void
i386_space_init(void)
{
	system_cr3 = asm_get_cr3();
	hal_memset(current_spaces, 0, sizeof(current_spaces));
	hal_memset(shootdowns, 0, sizeof(shootdowns));
	count_lock = 0;
	shootdown_pool_lock = 0;
	space_registry_lock = 0;
	space_registry = NULL;
	__atomic_store_n(&current_space, HAL_SPACE_SYS, __ATOMIC_RELEASE);
	next_space_id = 1;
	space_count = page_table_count = 0;
}

void
i386_space_init_secondary(void)
{
	asm_load_cr3(system_cr3);
	__atomic_store_n(&current_space, HAL_SPACE_SYS, __ATOMIC_RELEASE);
}

static struct i386_page_table *
find_table(struct i386_space *space, uintptr_t vaddr)
{
	struct i386_page_table *table;
	uintptr_t base = vaddr & 0xffc00000U;

	for (table = space->page_tables; table != NULL; table = table->next)
		if (table->vaddr == base)
			return table;
	return NULL;
}

static struct i386_page_table *
create_table(struct i386_space *space, uintptr_t vaddr)
{
	struct i386_page_table *table;
	unsigned pde = (unsigned)(vaddr >> 22);

	table = hal_malloc(sizeof(*table));
	if (table == NULL)
		return NULL;
	hal_memset(table, 0, sizeof(*table));
	if (alloc_page(&table->memory) != HAL_OK) {
		hal_free(table);
		return NULL;
	}
	table->vaddr = vaddr & 0xffc00000U;
	table->pte = table->memory.vaddr;
	hal_memset(table->pte, 0, PAGE_SIZE);
	table->next = space->page_tables;
	space->page_tables = table;
	{
		bool enabled = raw_lock_enter(&count_lock);
		page_table_count++;
		raw_lock_leave(&count_lock, enabled);
	}
	space->pdt[pde] = ((uint32_t)table->memory.paddr & 0xfffff000U) |
		PTE_PRESENT | PTE_WRITE | PTE_USER;
	return table;
}

/* Disconnect empty page tables while retaining their storage for shootdown. */
static struct i386_page_table *
detach_empty_tables(struct i386_space *space)
{
	struct i386_page_table **link = &space->page_tables;
	struct i386_page_table *detached = NULL;

	while (*link != NULL) {
		struct i386_page_table *table = *link;
		unsigned index;

		for (index = 0; index < 1024U; index++)
			if (table->pte[index] & PTE_PRESENT)
				break;
		if (index != 1024U) {
			link = &table->next;
			continue;
		}
		space->pdt[table->vaddr >> 22] = 0;
		*link = table->next;
		table->next = detached;
		detached = table;
	}
	return detached;
}

static void
free_detached_tables(struct i386_page_table *table)
{
	while (table != NULL) {
		struct i386_page_table *next = table->next;

		(void)hal_pmem_free(&table->memory);
		hal_free(table);
		table_count_drop();
		table = next;
	}
}

hal_space_t
hal_mem_create_space(void)
{
	struct i386_space *space;
	uint32_t *system_pdt;
	unsigned i;

	space = hal_malloc(sizeof(*space));
	if (space == NULL)
		return NULL;
	hal_memset(space, 0, sizeof(*space));
	if (alloc_page(&space->directory_memory) != HAL_OK) {
		hal_free(space);
		return NULL;
	}
	space->pdt = space->directory_memory.vaddr;
	hal_memset(space->pdt, 0, PAGE_SIZE);
	system_pdt = (uint32_t *)(system_cr3 | SYS_START);
	for (i = 512; i < 1024; i++)
		space->pdt[i] = system_pdt[i] & ~PTE_USER;
	space->magic = I386_SPACE_MAGIC;
	{
		bool enabled = raw_lock_enter(&count_lock);
		space->space_id = next_space_id++;
		space_count++;
		raw_lock_leave(&count_lock, enabled);
	}
	{
		bool enabled = raw_lock_enter(&space_registry_lock);

		space->registry_next = space_registry;
		space_registry = space;
		raw_lock_leave(&space_registry_lock, enabled);
	}
	return space;
}

void
hal_page_destroy_space(hal_space_t handle)
{
	struct i386_space *space = handle;
	struct i386_space **link;
	struct i386_page_table *table;
	struct hal_cpu_mask ready;
	hal_cpu_id_t cpu;
	bool enabled;

	if (space == NULL)
		return;
	enabled = raw_lock_enter(&space_registry_lock);
	for (link = &space_registry; *link != NULL && *link != space;
	    link = &(*link)->registry_next)
		;
	if (*link == NULL || space->destroying)
		HAL_FATAL("invalid i386 space destroy");
	space->destroying = 1U;
	*link = space->registry_next;
	raw_lock_leave(&space_registry_lock, enabled);
	for (;;) {
		unsigned active;

		enabled = raw_lock_enter(&space_registry_lock);
		active = space->active_ops;
		raw_lock_leave(&space_registry_lock, enabled);
		if (active == 0)
			break;
		service_shootdowns(hal_cpu_current());
		__asm__ volatile("pause");
	}
	/*
	 * Address-space lifetime remains a kernel responsibility.  Prevent new
	 * switches, then close remote hardware page-walk windows; no task-detach
	 * operation is needed in the HAL.
	 */
	shootdown(space, NULL, 0);
	hal_cpu_ready_mask(&ready);
	for (cpu = 0; cpu < hal_cpu_count(); cpu++)
		if (hal_cpu_mask_test(&ready, cpu) &&
		    __atomic_load_n(&current_spaces[cpu], __ATOMIC_ACQUIRE) == space)
			HAL_FATAL("destroying an active i386 space");
	while ((table = space->page_tables) != NULL) {
		space->page_tables = table->next;
		(void)hal_pmem_free(&table->memory);
		hal_free(table);
		table_count_drop();
	}
	space->magic = 0;
	(void)hal_pmem_free(&space->directory_memory);
	hal_free(space);
	enabled = raw_lock_enter(&count_lock);
	if (space_count == 0)
		HAL_FATAL("i386 space counter underflow");
	space_count--;
	raw_lock_leave(&count_lock, enabled);
}

void
hal_i386_space_memory_stats(uint32_t *spaces, uint32_t *tables)
{
	bool enabled = raw_lock_enter(&count_lock);

	if (spaces != NULL)
		*spaces = space_count;
	if (tables != NULL)
		*tables = page_table_count;
	raw_lock_leave(&count_lock, enabled);
}

void
hal_page_switch_space(hal_space_t handle)
{
	struct i386_space *space;
	hal_space_t old;
	uint32_t cr3;
	bool enabled;

	old = __atomic_load_n(&current_space, __ATOMIC_ACQUIRE);
	if (handle == old)
		return;
	if (handle == HAL_SPACE_SYS) {
		enabled = hal_irq_disable();
		asm_load_cr3(system_cr3);
		__atomic_store_n(&current_space, handle, __ATOMIC_RELEASE);
		if (enabled)
			hal_irq_enable();
		return;
	}
	space = handle;
	if (!space_op_enter(space))
		HAL_FATAL("invalid i386 space switch");
	enabled = space_lock_enter(space);
	cr3 = (uint32_t)space->directory_memory.paddr;
	asm_load_cr3(cr3);
	__atomic_store_n(&current_space, handle, __ATOMIC_RELEASE);
	space_lock_leave(space, enabled);
	space_op_leave(space);
}

static int
valid_user_range(uintptr_t vaddr, size_t size)
{
	return size != 0 && (vaddr & (PAGE_SIZE - 1U)) == 0 &&
		(size & (PAGE_SIZE - 1U)) == 0 && vaddr >= PAGE_SIZE &&
		vaddr < SYS_START && size <= SYS_START - vaddr;
}

static uint32_t
pte_flags(uint32_t attr)
{
	uint32_t flags = PTE_PRESENT | PTE_USER;

	if (attr & HAL_SPACE_WRITE)
		flags |= PTE_WRITE;
	if (attr & HAL_SPACE_NOCACHE)
		flags |= PTE_NOCACHE;
	if (attr & HAL_SPACE_WRITETHRU)
		flags |= PTE_WRITEBACK;
	return flags;
}

/*
 * An 80386 has atomic memory xchg/locked logical operations but no CMPXCHG.
 * Software PTE writers are serialized by space->lock; only the page walker can
 * change A/D here.  xchg returns the exact pre-publication value, and the
 * locked OR carries any newly observed A/D bits into the replacement PTE.
 */
static uint32_t
pte_replace_preserve_ad(uint32_t *entry, uint32_t replacement)
{
	uint32_t snapshot = *entry;
	uint32_t previous = replacement |
	    (snapshot & (PTE_ACCESS | PTE_DIRTY));
	uint32_t ad;

	__asm__ volatile("xchgl %0, %1"
	    : "+r" (previous), "+m" (*entry) : : "memory");
	ad = previous & (PTE_ACCESS | PTE_DIRTY);
	if (ad != 0)
		__asm__ volatile("lock; orl %1, %0"
		    : "+m" (*entry) : "r" (ad) : "memory", "cc");
	return previous;
}

int
hal_page_map(hal_space_t handle, void *address, hal_physaddr_t paddr, size_t size,
	     uint32_t attr)
{
	struct i386_space *space = handle;
	uintptr_t vaddr = (uintptr_t)address;
	uintptr_t offset;
	bool enabled;

	if (space == NULL ||
	    !valid_user_range(vaddr, size) ||
	    (paddr & (PAGE_SIZE - 1U)) != 0 ||
	    paddr > UINT32_MAX - (size - PAGE_SIZE) ||
	    (attr & (HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC)) == 0)
		return HAL_ERR_INVALID;
	if (!space_op_enter(space))
		return HAL_ERR_STATE;
	enabled = space_lock_enter(space);
	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		struct i386_page_table *table = find_table(space, vaddr + offset);
		unsigned index = (unsigned)((vaddr + offset) >> 12) & 1023U;

		if (table != NULL && (table->pte[index] & PTE_PRESENT)) {
			space_lock_leave(space, enabled);
			space_op_leave(space);
			return HAL_ERR_INVALID;
		}
	}
	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		struct i386_page_table *table = find_table(space, vaddr + offset);
		unsigned index = (unsigned)((vaddr + offset) >> 12) & 1023U;

		if (table == NULL)
			table = create_table(space, vaddr + offset);
		if (table == NULL) {
			struct i386_page_table *detached;
			uintptr_t rollback;

			for (rollback = 0; rollback < offset;
			    rollback += PAGE_SIZE) {
				table = find_table(space, vaddr + rollback);
				if (table != NULL)
					table->pte[((vaddr + rollback) >> 12) & 1023U] = 0;
			}
			detached = detach_empty_tables(space);
			/* Parent PDEs are part of the invalidation boundary too. */
			if (detached != NULL)
				shootdown(space, NULL, 0);
			else if (offset != 0)
				shootdown(space, address, offset);
			free_detached_tables(detached);
			space_lock_leave(space, enabled);
			space_op_leave(space);
			return HAL_ERR_NOMEM;
		}
		table->pte[index] = (uint32_t)(paddr + offset) | pte_flags(attr);
	}
	shootdown(space, address, size);
	space_lock_leave(space, enabled);
	space_op_leave(space);
	return HAL_OK;
}

int
hal_page_prot(hal_space_t handle, void *address, size_t size, uint32_t attr)
{
	return hal_page_prot_query(handle, address, size, attr, NULL);
}

int
hal_page_prot_query(hal_space_t handle, void *address, size_t size,
	uint32_t attr, uint32_t *flags)
{
	struct i386_space *space = handle;
	uintptr_t vaddr = (uintptr_t)address;
	uintptr_t offset;
	uint32_t observed = 0;
	bool enabled;

	if (space == NULL ||
	    !valid_user_range(vaddr, size) ||
	    (attr & (HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC)) == 0)
		return HAL_ERR_INVALID;
	if (!space_op_enter(space))
		return HAL_ERR_STATE;
	enabled = space_lock_enter(space);
	/* Validate the full range before changing the first PTE. */
	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		struct i386_page_table *table = find_table(space, vaddr + offset);
		unsigned index = (unsigned)((vaddr + offset) >> 12) & 1023U;

		if (table == NULL || !(table->pte[index] & PTE_PRESENT)) {
			space_lock_leave(space, enabled);
			space_op_leave(space);
			return HAL_ERR_INVALID;
		}
	}
	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		struct i386_page_table *table = find_table(space, vaddr + offset);
		unsigned index = (unsigned)((vaddr + offset) >> 12) & 1023U;
		uint32_t snapshot = table->pte[index];
		uint32_t old = pte_replace_preserve_ad(&table->pte[index],
		    (snapshot & 0xfffff000U) | pte_flags(attr));
		if (old & PTE_ACCESS)
			observed |= HAL_PAGE_ACCESSED;
		if (old & PTE_DIRTY)
			observed |= HAL_PAGE_DIRTY;
	}
	shootdown(space, address, size);

	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		struct i386_page_table *table = find_table(space, vaddr + offset);
		unsigned index = (unsigned)((vaddr + offset) >> 12) & 1023U;
		uint32_t entry;

		if (table == NULL ||
		    !((entry = table->pte[index]) & PTE_PRESENT)) {
			space_lock_leave(space, enabled);
			space_op_leave(space);
			return HAL_ERR_STATE;
		}
		observed |= HAL_PAGE_PRESENT;
		if (entry & PTE_ACCESS)
			observed |= HAL_PAGE_ACCESSED;
		if (entry & PTE_DIRTY)
			observed |= HAL_PAGE_DIRTY;
	}
	space_lock_leave(space, enabled);
	space_op_leave(space);
	if (flags != NULL)
		*flags = observed;
	return HAL_OK;
}

int
hal_page_unmap(hal_space_t handle, void *address, size_t size)
{
	struct i386_space *space = handle;
	struct i386_page_table *detached;
	uintptr_t vaddr = (uintptr_t)address;
	uintptr_t offset;
	bool enabled;

	if (size == 0)
		return HAL_OK;
	if (space == NULL ||
	    !valid_user_range(vaddr, size))
		return HAL_ERR_INVALID;
	if (!space_op_enter(space))
		return HAL_ERR_STATE;
	enabled = space_lock_enter(space);
	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		struct i386_page_table *table = find_table(space, vaddr + offset);
		unsigned index = (unsigned)((vaddr + offset) >> 12) & 1023U;

		if (table != NULL)
			table->pte[index] = 0;
	}
	/* Parent PDEs are cleared before acknowledgement; storage survives it. */
	detached = detach_empty_tables(space);
	if (detached != NULL)
		shootdown(space, NULL, 0);
	else
		shootdown(space, address, size);
	free_detached_tables(detached);
	space_lock_leave(space, enabled);
	space_op_leave(space);
	return HAL_OK;
}

int
hal_page_query(hal_space_t handle, void *address, uint32_t *flags)
{
	struct i386_space *space = handle;
	uintptr_t vaddr = (uintptr_t)address;
	struct i386_page_table *table;
	uint32_t pte;
	unsigned index;
	bool enabled;

	if (space == NULL || flags == NULL ||
	    !valid_user_range(vaddr, PAGE_SIZE))
		return HAL_ERR_INVALID;
	if (!space_op_enter(space))
		return HAL_ERR_STATE;
	enabled = space_lock_enter(space);
	table = find_table(space, vaddr);
	index = (unsigned)(vaddr >> 12) & 1023U;
	pte = table != NULL ? table->pte[index] : 0;
	*flags = (pte & PTE_PRESENT ? HAL_PAGE_PRESENT : 0) |
		(pte & PTE_ACCESS ? HAL_PAGE_ACCESSED : 0) |
		(pte & PTE_DIRTY ? HAL_PAGE_DIRTY : 0);
	space_lock_leave(space, enabled);
	space_op_leave(space);
	return HAL_OK;
}

int
hal_page_clear_flags(hal_space_t handle, void *address, uint32_t flags)
{
	struct i386_space *space = handle;
	uintptr_t vaddr = (uintptr_t)address;
	struct i386_page_table *table;
	unsigned index;
	uint32_t mask = 0;
	uint32_t keep;
	bool enabled;

	if (space == NULL ||
	    !valid_user_range(vaddr, PAGE_SIZE) ||
	    (flags & ~(HAL_PAGE_ACCESSED | HAL_PAGE_DIRTY)) != 0)
		return HAL_ERR_INVALID;
	if (!space_op_enter(space))
		return HAL_ERR_STATE;
	enabled = space_lock_enter(space);
	table = find_table(space, vaddr);
	index = (unsigned)(vaddr >> 12) & 1023U;
	if (table == NULL || !(table->pte[index] & PTE_PRESENT)) {
		space_lock_leave(space, enabled);
		space_op_leave(space);
		return HAL_ERR_INVALID;
	}
	if (flags & HAL_PAGE_ACCESSED)
		mask |= PTE_ACCESS;
	if (flags & HAL_PAGE_DIRTY)
		mask |= PTE_DIRTY;
	keep = ~mask;
	__asm__ volatile("lock; andl %1, %0"
	    : "+m" (table->pte[index]) : "r" (keep) : "memory", "cc");
	shootdown(space, address, PAGE_SIZE);
	space_lock_leave(space, enabled);
	space_op_leave(space);
	return HAL_OK;
}

static void
shootdown(hal_space_t handle, void *vaddr, size_t size)
{
	hal_cpu_id_t sender = hal_cpu_current(), cpu;
	struct i386_shootdown_request *request = NULL;
	struct hal_cpu_mask ready;
	unsigned slot;
	unsigned pending = 0;
	bool pool_enabled;

	pool_enabled = raw_lock_enter(&shootdown_pool_lock);
	for (slot = 0; slot < I386_SHOOTDOWN_REQUESTS; slot++) {
		if (shootdowns[slot].active == 0U) {
			request = &shootdowns[slot];
			request->active = 2U;
			break;
		}
	}
	raw_lock_leave(&shootdown_pool_lock, pool_enabled);
	if (request == NULL)
		HAL_FATAL("i386 TLB shootdown request pool exhausted");
	hal_cpu_ready_mask(&ready);
	for (cpu = 0; cpu < hal_cpu_count(); cpu++) {
		if (cpu == sender || cpu >= I386_APIC_MAX_CPUS ||
		    !hal_cpu_mask_test(&ready, cpu))
			continue;
		if (handle == HAL_SPACE_SYS ||
		    __atomic_load_n(&current_spaces[cpu], __ATOMIC_ACQUIRE) == handle)
			pending |= 1U << cpu;
	}
	request->space = handle;
	request->vaddr = vaddr;
	request->size = size;
	request->pending = pending;
	hal_wmb();
	request->active = 1U;
	hal_wmb();
	for (cpu = 0; cpu < hal_cpu_count(); cpu++)
		if ((pending & (1U << cpu)) != 0 &&
		    i386_smp_send_tlb(cpu) != HAL_OK)
			HAL_FATAL("i386 TLB shootdown delivery failed");
	if (handle == HAL_SPACE_SYS ||
	    __atomic_load_n(&current_space, __ATOMIC_ACQUIRE) == handle)
		asm_flash_tlb();
	while (request->pending != 0) {
		/* Break reciprocal-shootdown deadlocks without opening all IRQs. */
		service_shootdowns(sender);
		__asm__ volatile("pause");
	}
	hal_mb();
	request->active = 0U;
}

static void
service_shootdowns(hal_cpu_id_t cpu)
{
	unsigned bit = 1U << cpu;
	unsigned slot;

	for (slot = 0; slot < I386_SHOOTDOWN_REQUESTS; slot++) {
		struct i386_shootdown_request *request = &shootdowns[slot];
		bool enabled;

		if (request->active != 1U)
			continue;
		hal_rmb();
		enabled = raw_lock_enter(&request->lock);
		if (request->active == 1U && (request->pending & bit) != 0) {
			asm_flash_tlb();
			request->pending &= ~bit;
		}
		raw_lock_leave(&request->lock, enabled);
	}
}

void
i386_tlb_interrupt(void)
{
	service_shootdowns(hal_cpu_current());
	hal_irq_send_eoi(IRQ_MAX + 2U);
}

void
hal_page_flush_tlb(hal_space_t handle)
{
	struct i386_space *space = handle;
	bool enabled;

	if (handle == HAL_SPACE_SYS) {
		shootdown(handle, NULL, 0);
		return;
	}
	if (!space_op_enter(space))
		HAL_FATAL("invalid i386 space flush");
	enabled = space_lock_enter(space);
	shootdown(space, NULL, 0);
	space_lock_leave(space, enabled);
	space_op_leave(space);
}

void
hal_page_flush_tlb_range(hal_space_t handle, void *vaddr, size_t size)
{
	struct i386_space *space = handle;
	bool enabled;

	if (size == 0)
		return;
	if (handle == HAL_SPACE_SYS) {
		shootdown(handle, vaddr, size);
		return;
	}
	if (!space_op_enter(space))
		HAL_FATAL("invalid i386 space range flush");
	enabled = space_lock_enter(space);
	shootdown(space, vaddr, size);
	space_lock_leave(space, enabled);
	space_op_leave(space);
}

size_t hal_page_get_page_size(int level)
{
	return level == 1 ? PAGE_SIZE : 0;
}

void hal_page_get_user_range(uintptr_t *minimum, uintptr_t *limit)
{
	if (minimum != NULL) *minimum = PAGE_SIZE;
	if (limit != NULL) *limit = SYS_START;
}
