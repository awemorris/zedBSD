/* amd64 four-level page-table implementation. */
#include <hal/hal.h>
#include "defs.h"
#include "asm.h"
#include "percpu.h"
#include "space.h"
#include "smp.h"
#include "bsp-pcat/lapic.h"
#include "bootloader/include/amd64-handoff.h"

static uint64 system_pml4[512] __attribute__((aligned(PAGE_SIZE)));
static uint64 system_pdpt[512] __attribute__((aligned(PAGE_SIZE)));
static uint64 system_pd[512] __attribute__((aligned(PAGE_SIZE)));
static uint64 system_kernel_pt[8][512]
	__attribute__((aligned(PAGE_SIZE)));
static uint64 system_mmio_pd[512] __attribute__((aligned(PAGE_SIZE)));
static uintptr_t system_cr3;
#define AMD64_CURRENT_SPACE (amd64_percpu_current()->current_space)
static int next_space_id = 1;
static uint32 space_count;
static uint32 page_table_count;

struct amd64_shootdown_request {
	volatile unsigned active;
	hal_space_t space;
	void *vaddr;
	size_t size;
	volatile uint64 pending;
};
#define AMD64_SHOOTDOWN_REQUESTS (AMD64_SMP_MAX_CPUS * 4U)
static struct amd64_shootdown_request shootdowns[AMD64_SHOOTDOWN_REQUESTS];

static void shootdown(hal_space_t handle, void *vaddr, size_t size);
static void service_shootdowns(hal_cpu_id_t cpu);

static void
table_count_drop(void)
{
	uint32 old = __atomic_fetch_sub(&page_table_count, 1U,
	    __ATOMIC_RELAXED);
	if (old == 0)
		HAL_FATAL("amd64 page-table counter underflow");
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

extern char __kernel_phys_start[], __kernel_phys_end[];
extern char __kernel_text_phys_start[], __kernel_text_phys_end[];
extern char __kernel_rodata_phys_start[], __kernel_rodata_phys_end[];
extern char __kernel_data_phys_start[], __kernel_data_phys_end[];

uintptr_t amd64_direct_to_phys(const void *address)
{
	return (uintptr_t)address - (uintptr_t)AMD64_DIRECT_BASE;
}

void *amd64_phys_to_direct(uintptr_t address)
{
	return (void *)((uintptr_t)AMD64_DIRECT_BASE + address);
}

uintptr_t amd64_system_cr3(void) { return system_cr3; }

void
amd64_space_init(void)
{
	const struct zbl6_framebuffer *framebuffer =
	    hal_get_arch_handoff("pcat.framebuffer");
	uintptr_t kernel_start = (uintptr_t)__kernel_phys_start;
	uintptr_t kernel_end = (uintptr_t)__kernel_phys_end;
	unsigned index, first_chunk, chunks, chunk;
	uint64 efer;
	uintptr_t cr0, cr4;

	hal_memset(system_pml4, 0, sizeof(system_pml4));
	hal_memset(system_pdpt, 0, sizeof(system_pdpt));
	hal_memset(system_pd, 0, sizeof(system_pd));
	hal_memset(system_kernel_pt, 0, sizeof(system_kernel_pt));
	hal_memset(system_mmio_pd, 0, sizeof(system_mmio_pd));
	for (index = 0; index < 512; index++)
		system_pd[index] = (uint64)index * 0x200000ULL |
		    AMD64_PTE_PRESENT | AMD64_PTE_WRITE | AMD64_PTE_LARGE |
		    AMD64_PTE_GLOBAL | AMD64_PTE_NX;
	first_chunk = (unsigned)(kernel_start / 0x200000U);
	chunks = (unsigned)((kernel_end + 0x1fffffU) / 0x200000U) -
	    first_chunk;
	if (chunks == 0 || chunks > 8)
		HAL_FATAL("amd64 kernel W^X window exceeded");
	for (chunk = 0; chunk < chunks; chunk++) {
		uintptr_t base = (uintptr_t)(first_chunk + chunk) * 0x200000U;
		for (index = 0; index < 512; index++) {
			uintptr_t physical = base + (uintptr_t)index * PAGE_SIZE;
			uint64 flags = AMD64_PTE_PRESENT | AMD64_PTE_GLOBAL |
			    AMD64_PTE_NX;
			if (physical >= (uintptr_t)__kernel_text_phys_start &&
			    physical < (uintptr_t)__kernel_text_phys_end)
				flags &= ~AMD64_PTE_NX;
			else if (physical < (uintptr_t)__kernel_rodata_phys_start ||
			    physical >= (uintptr_t)__kernel_rodata_phys_end)
				flags |= AMD64_PTE_WRITE;
			system_kernel_pt[chunk][index] = physical | flags;
		}
		system_pd[first_chunk + chunk] =
		    amd64_direct_to_phys(system_kernel_pt[chunk]) |
		    AMD64_PTE_PRESENT | AMD64_PTE_WRITE;
	}
	for (index = 0; index < 8; index++)
		system_mmio_pd[index] = (0xf0000000ULL +
		    (uint64)index * 0x200000ULL) | AMD64_PTE_PRESENT |
		    AMD64_PTE_WRITE | AMD64_PTE_NOCACHE | AMD64_PTE_LARGE |
		    AMD64_PTE_GLOBAL | AMD64_PTE_NX;
	/* Dedicated uncached windows for the Local APIC and I/O APIC. */
	system_mmio_pd[8] = 0xfee00000ULL | AMD64_PTE_PRESENT |
	    AMD64_PTE_WRITE | AMD64_PTE_NOCACHE | AMD64_PTE_LARGE |
	    AMD64_PTE_GLOBAL | AMD64_PTE_NX;
	system_mmio_pd[9] = 0xfec00000ULL | AMD64_PTE_PRESENT |
	    AMD64_PTE_WRITE | AMD64_PTE_NOCACHE | AMD64_PTE_LARGE |
	    AMD64_PTE_GLOBAL | AMD64_PTE_NX;
	if (framebuffer != NULL) {
		uint64 base = framebuffer->physical_base & ~0x1fffffULL;
		uint64 end = framebuffer->physical_base + framebuffer->size;
		unsigned count = (unsigned)((end - base + 0x1fffffULL) /
		    0x200000ULL);
		if (count == 0 || count > 496U)
			HAL_FATAL("amd64 framebuffer MMIO window exceeded");
		for (index = 0; index < count; index++)
			system_mmio_pd[16U + index] = (base +
			    (uint64)index * 0x200000ULL) |
			    AMD64_PTE_PRESENT | AMD64_PTE_WRITE |
			    AMD64_PTE_NOCACHE | AMD64_PTE_LARGE |
			    AMD64_PTE_GLOBAL | AMD64_PTE_NX;
	}
	/* APs enter long mode at the low trampoline before jumping high. */
	system_pd[0] &= ~AMD64_PTE_NX;
	system_pdpt[0] = amd64_direct_to_phys(system_pd) |
	    AMD64_PTE_PRESENT | AMD64_PTE_WRITE;
	system_pdpt[510] = amd64_direct_to_phys(system_pd) |
	    AMD64_PTE_PRESENT | AMD64_PTE_WRITE;
	system_pdpt[511] = amd64_direct_to_phys(system_mmio_pd) |
	    AMD64_PTE_PRESENT | AMD64_PTE_WRITE;
	system_pml4[511] = amd64_direct_to_phys(system_pdpt) |
	    AMD64_PTE_PRESENT | AMD64_PTE_WRITE;
	system_pml4[0] = amd64_direct_to_phys(system_pdpt) |
	    AMD64_PTE_PRESENT | AMD64_PTE_WRITE;
	efer = asm_read_msr(0xc0000080U);
	asm_write_msr(0xc0000080U, efer | (1ULL << 11));
	__asm__ volatile("movq %%cr4,%0" : "=r"(cr4));
	cr4 |= 0x80U;
	__asm__ volatile("movq %0,%%cr4" : : "r"(cr4) : "memory");
	__asm__ volatile("movq %%cr0,%0" : "=r"(cr0));
	cr0 |= 0x10000U;
	__asm__ volatile("movq %0,%%cr0" : : "r"(cr0) : "memory");
	system_cr3 = amd64_direct_to_phys(system_pml4);
	asm_load_cr3(system_cr3);
	__atomic_store_n(&AMD64_CURRENT_SPACE, HAL_SPACE_SYS, __ATOMIC_RELEASE);
}

static int valid_space(hal_space_t handle)
{
	return handle == HAL_SPACE_SYS ||
	    (handle != NULL && ((struct amd64_space *)handle)->magic ==
	    AMD64_SPACE_MAGIC);
}

static bool
space_lock_enter(struct amd64_space *space)
{
	bool enabled = hal_irq_disable();
	while (__atomic_exchange_n(&space->lock, 1U, __ATOMIC_ACQUIRE) != 0)
		__asm__ volatile("pause");
	return enabled;
}

static void
space_lock_leave(struct amd64_space *space, bool enabled)
{
	__atomic_store_n(&space->lock, 0U, __ATOMIC_RELEASE);
	if (enabled)
		hal_irq_enable();
}

static int valid_user_range(uintptr_t address, size_t size)
{
	return size != 0 && (address & (PAGE_SIZE - 1U)) == 0 &&
	    (size & (PAGE_SIZE - 1U)) == 0 && address >= PAGE_SIZE &&
	    address < 0x0000800000000000ULL &&
	    size <= 0x0000800000000000ULL - address;
}

static struct amd64_table_page *
allocate_table(struct amd64_space *space, uint64 *parent,
	unsigned parent_index)
{
	struct amd64_table_page *page = hal_malloc(sizeof(*page));
	if (page == NULL) return NULL;
	if (alloc_page(&page->memory) != HAL_OK) {
		hal_free(page);
		return NULL;
	}
	hal_memset(page->memory.vaddr, 0, PAGE_SIZE);
	page->parent = parent;
	page->parent_index = parent_index;
	page->next = space->tables;
	space->tables = page;
	(void)__atomic_fetch_add(&page_table_count, 1U, __ATOMIC_RELAXED);
	return page;
}

static uint64 *
walk_leaf(struct amd64_space *space, uintptr_t address, int create)
{
	unsigned shifts[3] = { 39, 30, 21 };
	uint64 *table = space->pml4;
	unsigned level;

	for (level = 0; level < 3; level++) {
		unsigned index = (unsigned)(address >> shifts[level]) & 511U;
		uint64 entry = table[index];
		if (!(entry & AMD64_PTE_PRESENT)) {
			struct amd64_table_page *page;
			if (!create) return NULL;
			page = allocate_table(space, table, index);
			if (page == NULL) return NULL;
			entry = (uintptr_t)page->memory.paddr |
			    AMD64_PTE_PRESENT | AMD64_PTE_WRITE | AMD64_PTE_USER;
			table[index] = entry;
		}
		if (entry & AMD64_PTE_LARGE) return NULL;
		table = amd64_phys_to_direct((uintptr_t)(entry &
		    AMD64_PTE_ADDR_MASK));
	}
	return &table[(address >> 12) & 511U];
}

static int
table_is_empty(const uint64 *table)
{
	unsigned index;
	for (index = 0; index < 512; index++)
		if (table[index] & AMD64_PTE_PRESENT)
			return 0;
	return 1;
}

static void
reclaim_empty_tables(struct amd64_space *space)
{
	int reclaimed;
	do {
		struct amd64_table_page **link = &space->tables;
		reclaimed = 0;
		while (*link != NULL) {
			struct amd64_table_page *page = *link;
			uint64 expected = (uintptr_t)page->memory.paddr;
			if (!table_is_empty(page->memory.vaddr)) {
				link = &page->next;
				continue;
			}
			if ((page->parent[page->parent_index] &
			    AMD64_PTE_ADDR_MASK) == expected)
				page->parent[page->parent_index] = 0;
			*link = page->next;
			(void)hal_pmem_free(&page->memory);
			hal_free(page);
			table_count_drop();
			reclaimed = 1;
		}
	} while (reclaimed);
}

hal_space_t
hal_mem_create_space(void)
{
	struct amd64_space *space = hal_malloc(sizeof(*space));
	if (space == NULL) return NULL;
	hal_memset(space, 0, sizeof(*space));
	if (alloc_page(&space->pml4_memory) != HAL_OK) {
		hal_free(space);
		return NULL;
	}
	space->pml4 = space->pml4_memory.vaddr;
	hal_memset(space->pml4, 0, PAGE_SIZE);
	space->pml4[511] = system_pml4[511];
	space->magic = AMD64_SPACE_MAGIC;
	space->space_id = __atomic_fetch_add(&next_space_id, 1,
	    __ATOMIC_RELAXED);
	(void)__atomic_fetch_add(&space_count, 1U, __ATOMIC_RELAXED);
	return space;
}

void
hal_page_destroy_space(hal_space_t handle)
{
	struct amd64_space *space = handle;
	struct amd64_table_page *page;
	bool enabled;
	if (space == NULL) return;
	if (!valid_space(space)) HAL_FATAL("invalid amd64 space destroy");
	if (AMD64_CURRENT_SPACE == space) hal_page_switch_space(HAL_SPACE_SYS);
	enabled = space_lock_enter(space);
	if (__atomic_exchange_n(&space->destroying, 1U,
	    __ATOMIC_ACQ_REL) != 0)
		HAL_FATAL("amd64 space destroyed twice");
	space_lock_leave(space, enabled);
	/*
	 * Never wait for another CPU while holding a space lock with local
	 * interrupts disabled: the target may be waiting for this lock and
	 * therefore unable to acknowledge the TLB interrupt.
	 */
	shootdown(space, NULL, 0);
	enabled = space_lock_enter(space);
	while ((page = space->tables) != NULL) {
		space->tables = page->next;
		(void)hal_pmem_free(&page->memory);
		hal_free(page);
		table_count_drop();
	}
	space->magic = 0;
	space_lock_leave(space, enabled);
	(void)hal_pmem_free(&space->pml4_memory);
	hal_free(space);
	if (__atomic_fetch_sub(&space_count, 1U, __ATOMIC_RELAXED) == 0)
		HAL_FATAL("amd64 space counter underflow");
}

void
hal_page_switch_space(hal_space_t handle)
{
	uintptr_t cr3;
	if (!valid_space(handle)) HAL_FATAL("invalid amd64 space switch");
	if (handle != HAL_SPACE_SYS && __atomic_load_n(
	    &((struct amd64_space *)handle)->destroying,
	    __ATOMIC_ACQUIRE) != 0)
		HAL_FATAL("switch to destroying amd64 space");
	if (handle == AMD64_CURRENT_SPACE) return;
	cr3 = handle == HAL_SPACE_SYS ? system_cr3 :
	    (uintptr_t)((struct amd64_space *)handle)->pml4_memory.paddr;
	asm_load_cr3(cr3);
	__atomic_store_n(&AMD64_CURRENT_SPACE, handle, __ATOMIC_RELEASE);
}

static uint64 leaf_flags(uint32 attr)
{
	uint64 flags = AMD64_PTE_PRESENT | AMD64_PTE_USER;
	if (attr & HAL_SPACE_WRITE) flags |= AMD64_PTE_WRITE;
	if (attr & HAL_SPACE_NOCACHE) flags |= AMD64_PTE_NOCACHE;
	if (attr & HAL_SPACE_WRITETHRU) flags |= AMD64_PTE_WRITETHRU;
	if (attr & HAL_SPACE_DEVICE) flags |= AMD64_PTE_NOCACHE;
	if (!(attr & HAL_SPACE_EXEC)) flags |= AMD64_PTE_NX;
	return flags;
}

int
hal_page_map(hal_space_t handle, void *pointer, hal_physaddr_t physical,
	size_t size, uint32 attr)
{
	struct amd64_space *space = handle;
	uintptr_t address = (uintptr_t)pointer, offset;
	bool enabled;
	if (space == NULL || !valid_space(space) ||
	    !valid_user_range(address, size) ||
	    (physical & (PAGE_SIZE - 1U)) != 0 ||
	    physical >= AMD64_DIRECT_LIMIT || size > AMD64_DIRECT_LIMIT - physical ||
	    !(attr & (HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC)))
		return HAL_ERR_INVALID;
	enabled = space_lock_enter(space);
	if (space->destroying) {
		space_lock_leave(space, enabled);
		return HAL_ERR_STATE;
	}
	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		uint64 *leaf = walk_leaf(space, address + offset, 0);
		if (leaf != NULL && (*leaf & AMD64_PTE_PRESENT)) {
			space_lock_leave(space, enabled);
			return HAL_ERR_INVALID;
		}
	}
	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		uint64 *leaf = walk_leaf(space, address + offset, 1);
		if (leaf == NULL) {
			uintptr_t rollback;
			for (rollback = 0; rollback < offset; rollback += PAGE_SIZE) {
				leaf = walk_leaf(space, address + rollback, 0);
				if (leaf != NULL) *leaf = 0;
			}
			reclaim_empty_tables(space);
			space_lock_leave(space, enabled);
			return HAL_ERR_NOMEM;
		}
		*leaf = (physical + offset) | leaf_flags(attr);
	}
	space_lock_leave(space, enabled);
	/* A different CPU may currently execute in this address space. */
	hal_page_flush_tlb_range(space, pointer, size);
	return HAL_OK;
}

int
hal_page_prot(hal_space_t handle, void *pointer, size_t size, uint32 attr)
{
	struct amd64_space *space = handle;
	uintptr_t address = (uintptr_t)pointer, offset;
	bool enabled;
	if (space == NULL || !valid_space(space) ||
	    !valid_user_range(address, size) ||
	    !(attr & (HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC)))
		return HAL_ERR_INVALID;
	enabled = space_lock_enter(space);
	if (space->destroying) {
		space_lock_leave(space, enabled);
		return HAL_ERR_STATE;
	}
	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		uint64 *leaf = walk_leaf(space, address + offset, 0);
		uint64 physical;
		if (leaf == NULL || !(*leaf & AMD64_PTE_PRESENT)) {
			space_lock_leave(space, enabled);
			return HAL_ERR_INVALID;
		}
		physical = *leaf & AMD64_PTE_ADDR_MASK;
		*leaf = physical | leaf_flags(attr);
	}
	space_lock_leave(space, enabled);
	hal_page_flush_tlb_range(space, pointer, size);
	return HAL_OK;
}

int
hal_page_unmap(hal_space_t handle, void *pointer, size_t size)
{
	struct amd64_space *space = handle;
	uintptr_t address = (uintptr_t)pointer, offset;
	bool enabled;
	if (size == 0) return HAL_OK;
	if (space == NULL || !valid_space(space) ||
	    !valid_user_range(address, size)) return HAL_ERR_INVALID;
	enabled = space_lock_enter(space);
	if (space->destroying) {
		space_lock_leave(space, enabled);
		return HAL_ERR_STATE;
	}
	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		uint64 *leaf = walk_leaf(space, address + offset, 0);
		if (leaf != NULL) *leaf = 0;
	}
	space_lock_leave(space, enabled);
	/*
	 * Keep the page-table pages alive until every CPU has discarded the
	 * leaf translations.  Reclaiming them before shootdown can turn a
	 * remote page walk into a use-after-free.
	 */
	hal_page_flush_tlb_range(space, pointer, size);
	enabled = space_lock_enter(space);
	if (!space->destroying)
		reclaim_empty_tables(space);
	space_lock_leave(space, enabled);
	return HAL_OK;
}

int
hal_page_query(hal_space_t handle, void *pointer, uint32 *flags)
{
	struct amd64_space *space = handle;
	uint64 *leaf;
	bool enabled;
	if (space == NULL || !valid_space(space) || flags == NULL ||
	    !valid_user_range((uintptr_t)pointer, PAGE_SIZE))
		return HAL_ERR_INVALID;
	enabled = space_lock_enter(space);
	if (space->destroying) {
		space_lock_leave(space, enabled);
		return HAL_ERR_STATE;
	}
	leaf = walk_leaf(space, (uintptr_t)pointer, 0);
	*flags = leaf != NULL && (*leaf & AMD64_PTE_PRESENT) ? HAL_PAGE_PRESENT : 0;
	if (leaf != NULL && (*leaf & AMD64_PTE_ACCESSED)) *flags |= HAL_PAGE_ACCESSED;
	if (leaf != NULL && (*leaf & AMD64_PTE_DIRTY)) *flags |= HAL_PAGE_DIRTY;
	space_lock_leave(space, enabled);
	return HAL_OK;
}

int
hal_page_clear_flags(hal_space_t handle, void *pointer, uint32 flags)
{
	struct amd64_space *space = handle;
	uint64 *leaf, mask = 0;
	bool enabled;
	if (space == NULL || !valid_space(space) ||
	    !valid_user_range((uintptr_t)pointer, PAGE_SIZE) ||
	    (flags & ~(HAL_PAGE_ACCESSED | HAL_PAGE_DIRTY)) != 0)
		return HAL_ERR_INVALID;
	enabled = space_lock_enter(space);
	if (space->destroying) {
		space_lock_leave(space, enabled);
		return HAL_ERR_STATE;
	}
	leaf = walk_leaf(space, (uintptr_t)pointer, 0);
	if (leaf == NULL || !(*leaf & AMD64_PTE_PRESENT)) {
		space_lock_leave(space, enabled);
		return HAL_ERR_INVALID;
	}
	if (flags & HAL_PAGE_ACCESSED) mask |= AMD64_PTE_ACCESSED;
	if (flags & HAL_PAGE_DIRTY) mask |= AMD64_PTE_DIRTY;
	*leaf &= ~mask;
	space_lock_leave(space, enabled);
	hal_page_flush_tlb_range(space, pointer, PAGE_SIZE);
	return HAL_OK;
}

static void
shootdown(hal_space_t handle, void *vaddr, size_t size)
{
	hal_cpu_id_t sender = hal_cpu_current(), cpu;
	struct amd64_shootdown_request *request = NULL;
	struct hal_cpu_mask ready;
	unsigned slot;
	uint64 pending = 0;

	/*
	 * A timer interrupt may preempt a thread that is waiting here and run a
	 * second thread on the same CPU.  Requests therefore cannot be indexed
	 * by sender CPU.  Reserve a pool entry before publishing its fields.
	 */
	for (slot = 0; slot < AMD64_SHOOTDOWN_REQUESTS; slot++) {
		unsigned expected = 0;
		if (__atomic_compare_exchange_n(&shootdowns[slot].active,
		    &expected, 2U, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
			request = &shootdowns[slot];
			break;
		}
	}
	if (request == NULL)
		HAL_FATAL("amd64 TLB shootdown request pool exhausted");
	hal_cpu_ready_mask(&ready);
	for (cpu = 0; cpu < hal_cpu_count(); cpu++) {
		struct amd64_percpu *target;
		if (cpu == sender || cpu >= AMD64_SMP_MAX_CPUS ||
		    !hal_cpu_mask_test(&ready, cpu))
			continue;
		target = amd64_percpu_get(cpu);
		if (handle == HAL_SPACE_SYS ||
		    __atomic_load_n(&target->current_space, __ATOMIC_ACQUIRE) ==
		    handle)
			pending |= (uint64)1U << cpu;
	}
	request->space = handle;
	request->vaddr = vaddr;
	request->size = size;
	__atomic_store_n(&request->pending, pending, __ATOMIC_RELEASE);
	__atomic_store_n(&request->active, 1U, __ATOMIC_RELEASE);
	for (cpu = 0; cpu < hal_cpu_count(); cpu++)
		if ((pending & ((uint64)1U << cpu)) != 0 &&
		    amd64_lapic_send_vector(amd64_smp_apic_id(cpu),
		    AMD64_VECTOR_TLB) != HAL_OK)
			HAL_FATAL("amd64 TLB shootdown delivery failed");
	if (handle == HAL_SPACE_SYS || AMD64_CURRENT_SPACE == handle)
		asm_flush_tlb();
	while (__atomic_load_n(&request->pending, __ATOMIC_ACQUIRE) != 0) {
		/*
		 * Page operations are legal with local interrupts disabled.  Polling
		 * incoming requests here breaks the reciprocal-shootdown deadlock
		 * without enabling arbitrary interrupt handlers inside a caller's
		 * critical section.
		 */
		service_shootdowns(sender);
		__asm__ volatile("pause");
	}
	__atomic_store_n(&request->active, 0U, __ATOMIC_RELEASE);
}

static void
service_shootdowns(hal_cpu_id_t cpu)
{
	uint64 bit = (uint64)1U << cpu;
	unsigned slot;

	for (slot = 0; slot < AMD64_SHOOTDOWN_REQUESTS; slot++) {
		struct amd64_shootdown_request *request = &shootdowns[slot];
		if (__atomic_load_n(&request->active, __ATOMIC_ACQUIRE) == 1U &&
		    (__atomic_load_n(&request->pending, __ATOMIC_ACQUIRE) & bit) != 0) {
			asm_flush_tlb();
			(void)__atomic_fetch_and(&request->pending, ~bit,
			    __ATOMIC_RELEASE);
		}
	}
}

void
amd64_tlb_interrupt(void)
{
	hal_irq_ack_t acknowledge = amd64_irq_ack_begin(AMD64_VECTOR_TLB, -1);
	service_shootdowns(hal_cpu_current());
	hal_irq_send_eoi(acknowledge);
}

void hal_page_flush_tlb(hal_space_t handle)
{
	shootdown(handle, NULL, 0);
}

void hal_page_flush_tlb_range(hal_space_t handle, void *vaddr, size_t size)
{
	if (size != 0)
		shootdown(handle, vaddr, size);
}

size_t hal_page_get_page_size(int level)
{
	if (level == 1) return PAGE_SIZE;
	if (level == 2) return 0x200000U;
	return 0;
}

void hal_page_get_user_range(uintptr_t *minimum, uintptr_t *limit)
{
	if (minimum != NULL) *minimum = PAGE_SIZE;
	if (limit != NULL) *limit = 0x0000800000000000ULL;
}

void hal_amd64_space_memory_stats(uint32 *spaces, uint32 *tables)
{
	if (spaces != NULL)
		*spaces = __atomic_load_n(&space_count, __ATOMIC_RELAXED);
	if (tables != NULL)
		*tables = __atomic_load_n(&page_table_count, __ATOMIC_RELAXED);
}
