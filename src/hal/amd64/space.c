/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The amd64 four-level page-table implementation.
 */

#include <hal/hal.h>

#include "acpi-window.h"
#include "asm.h"
#include "bootloader/include/amd64-handoff.h"
#include "bsp.h"
#include "bsp-pcat/lapic.h"
#include "defs.h"
#include "percpu.h"
#include "smp.h"
#include "space.h"

#define AMD64_USER_LIMIT 0x0000800000000000ULL

#define AMD64_ACPI_PDPT_INDEX 509U
#define AMD64_ACPI_WINDOW_BASE 0xffffffff40000000ULL

#define AMD64_ECAM_PD_FIRST 128U
#define AMD64_ECAM_PD_COUNT 128U
#define AMD64_ECAM_VIRTUAL_BASE 0xffffffffd0000000ULL

#define AMD64_FRAMEBUFFER_PD_FIRST 16U
#define AMD64_FRAMEBUFFER_PD_COUNT \
	(AMD64_ECAM_PD_FIRST - AMD64_FRAMEBUFFER_PD_FIRST)

#define AMD64_CURRENT_SPACE (amd64_percpu_current()->current_space)
#define AMD64_SHOOTDOWN_REQUESTS (AMD64_SMP_MAX_CPUS * 4U)

struct amd64_shootdown_request {
	volatile unsigned active;
	hal_space_t space;
	void *vaddr;
	size_t size;
	volatile uint64_t pending;
};

typedef char amd64_user_pointer_window_assert[
	AMD64_USER_LIMIT - 1U <= (uintptr_t)INTPTR_MAX ? 1 : -1];

extern char __kernel_phys_start[];
extern char __kernel_phys_end[];
extern char __kernel_text_phys_start[];
extern char __kernel_text_phys_end[];
extern char __kernel_rodata_phys_start[];
extern char __kernel_rodata_phys_end[];
extern char __kernel_data_phys_start[];
extern char __kernel_data_phys_end[];

static uint64_t system_pml4[512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t system_pdpt[512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t system_pd[512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t system_kernel_pt[8][512]
	__attribute__((aligned(PAGE_SIZE)));
static uint64_t system_mmio_pd[512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t system_acpi_pd[512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t
    system_acpi_pt[AMD64_ACPI_WINDOW_PT_COUNT][512]
	__attribute__((aligned(PAGE_SIZE)));
static struct amd64_acpi_window acpi_window;
static paddr_t acpi_physical_max;
static unsigned ecam_pd_used;
static uintptr_t system_cr3;
static int next_space_id = 1;
static uint32_t space_count;
static uint32_t page_table_count;
static volatile unsigned space_registry_lock;
static struct amd64_space *space_registry;
static struct amd64_shootdown_request
    shootdowns[AMD64_SHOOTDOWN_REQUESTS];

static paddr_t cpu_physical_max(void);
static void table_count_drop(void);
static int alloc_page(struct hal_pmem *memory);
static bool registry_lock_enter(void);
static void registry_lock_leave(bool enabled);
static int space_op_enter(struct amd64_space *space);
static void space_op_leave(struct amd64_space *space);
static bool space_lock_enter(struct amd64_space *space);
static void space_lock_leave(struct amd64_space *space, bool enabled);
static int valid_user_range(uintptr_t address, size_t size);
static struct amd64_table_page *allocate_table(struct amd64_space *space, uint64_t *parent, unsigned parent_index);
static uint64_t *walk_leaf(struct amd64_space *space, uintptr_t address, int create);
static int table_is_empty(const uint64_t *table);
static struct amd64_table_page *detach_empty_tables(struct amd64_space *space);
static void free_detached_tables(struct amd64_table_page *page);
static uint64_t leaf_flags(uint32_t attr);
static void shootdown(hal_space_t handle, void *vaddr, size_t size);
static void service_shootdowns(hal_cpu_id_t cpu);

/*
 * Converts a direct-map address to its physical address.
 */
uintptr_t
amd64_direct_to_phys(
	const void *address)
{
	/* Reports the offset within the direct physical map. */
	return (uintptr_t)address - (uintptr_t)AMD64_DIRECT_BASE;
}

/*
 * Converts a physical address to its direct-map address.
 */
void *
amd64_phys_to_direct(
	uintptr_t address)
{
	/* Reports the corresponding direct-map address. */
	return (void *)((uintptr_t)AMD64_DIRECT_BASE + address);
}

/*
 * Reports the system page-table root.
 */
uintptr_t
amd64_system_cr3(
	void)
{
	/* Reports the physical address loaded for the system space. */
	return system_cr3;
}

/*
 * Initializes the system address space.
 */
void
amd64_space_init(
	void)
{
	const struct zbl6_framebuffer *framebuffer;
	uintptr_t kernel_start;
	uintptr_t kernel_end;
	uintptr_t base;
	uintptr_t physical;
	unsigned index;
	unsigned first_chunk;
	unsigned chunks;
	unsigned chunk;
	unsigned count;
	uint64_t efer;
	uint64_t end;
	uint64_t flags;
	uintptr_t cr0;
	uintptr_t cr4;

	/* Reads the firmware display and linker-defined kernel extent. */
	framebuffer = hal_get_arch_handoff("pcat.framebuffer");
	kernel_start = (uintptr_t)__kernel_phys_start;
	kernel_end = (uintptr_t)__kernel_phys_end;

	/* Initializes the address-space registry. */
	space_registry_lock = 0;
	space_registry = NULL;

	/* Clears every statically allocated system page table. */
	hal_memset(system_pml4, 0, sizeof(system_pml4));
	hal_memset(system_pdpt, 0, sizeof(system_pdpt));
	hal_memset(system_pd, 0, sizeof(system_pd));
	hal_memset(system_kernel_pt, 0, sizeof(system_kernel_pt));
	hal_memset(system_mmio_pd, 0, sizeof(system_mmio_pd));
	hal_memset(system_acpi_pd, 0, sizeof(system_acpi_pd));
	hal_memset(system_acpi_pt, 0, sizeof(system_acpi_pt));

	/* Initializes the reserved ACPI and ECAM mapping windows. */
	amd64_acpi_window_init(&acpi_window);
	acpi_physical_max = cpu_physical_max();
	ecam_pd_used = 0;

	/* Builds the large-page direct physical map. */
	for (index = 0; index < 512; index++) {
		system_pd[index] = (uint64_t)index * 0x200000ULL |
		    AMD64_PTE_PRESENT | AMD64_PTE_WRITE | AMD64_PTE_LARGE |
		    AMD64_PTE_GLOBAL | AMD64_PTE_NX;
	}

	/* Validates the bounded kernel permission window. */
	first_chunk = (unsigned)(kernel_start / 0x200000U);
	chunks = (unsigned)((kernel_end + 0x1fffffU) / 0x200000U) -
	    first_chunk;
	if (chunks == 0 || chunks > 8)
		HAL_FATAL("amd64 kernel W^X window exceeded");

	/* Replaces kernel large pages with per-page W^X mappings. */
	for (chunk = 0; chunk < chunks; chunk++) {
		base = (uintptr_t)(first_chunk + chunk) * 0x200000U;

		/* Assigns permissions to every kernel page in this chunk. */
		for (index = 0; index < 512; index++) {
			physical = base + (uintptr_t)index * PAGE_SIZE;
			flags = AMD64_PTE_PRESENT | AMD64_PTE_GLOBAL |
			    AMD64_PTE_NX;

			/* Makes text executable and non-rodata pages writable. */
			if (physical >= (uintptr_t)__kernel_text_phys_start &&
			    physical < (uintptr_t)__kernel_text_phys_end) {
				flags &= ~AMD64_PTE_NX;
			} else if (
			    physical < (uintptr_t)__kernel_rodata_phys_start ||
			    physical >= (uintptr_t)__kernel_rodata_phys_end) {
				flags |= AMD64_PTE_WRITE;
			}

			/* Publishes this kernel page with its selected permissions. */
			system_kernel_pt[chunk][index] = physical | flags;
		}

		/* Links the populated kernel leaf table into the direct map. */
		system_pd[first_chunk + chunk] =
		    amd64_direct_to_phys(system_kernel_pt[chunk]) |
		    AMD64_PTE_PRESENT | AMD64_PTE_WRITE;
	}

	/* Maps the fixed legacy MMIO range as uncached large pages. */
	for (index = 0; index < 8; index++) {
		system_mmio_pd[index] = (0xf0000000ULL +
		    (uint64_t)index * 0x200000ULL) | AMD64_PTE_PRESENT |
		    AMD64_PTE_WRITE | AMD64_PTE_NOCACHE | AMD64_PTE_LARGE |
		    AMD64_PTE_GLOBAL | AMD64_PTE_NX;
	}

	/* Maps dedicated uncached windows for the Local APIC and I/O APIC. */
	system_mmio_pd[8] = 0xfee00000ULL | AMD64_PTE_PRESENT |
	    AMD64_PTE_WRITE | AMD64_PTE_NOCACHE | AMD64_PTE_LARGE |
	    AMD64_PTE_GLOBAL | AMD64_PTE_NX;
	system_mmio_pd[9] = 0xfec00000ULL | AMD64_PTE_PRESENT |
	    AMD64_PTE_WRITE | AMD64_PTE_NOCACHE | AMD64_PTE_LARGE |
	    AMD64_PTE_GLOBAL | AMD64_PTE_NX;

	/* Maps the firmware framebuffer when its extent fits the window. */
	if (framebuffer != NULL) {
		base = framebuffer->physical_base & ~0x1fffffULL;
		end = framebuffer->physical_base + framebuffer->size;
		count = (unsigned)((end - base + 0x1fffffULL) /
		    0x200000ULL);

		/* Rejects a framebuffer outside the reserved page-directory range. */
		if (count == 0 || count > AMD64_FRAMEBUFFER_PD_COUNT)
			HAL_FATAL("amd64 framebuffer MMIO window exceeded");

		/* Maps every framebuffer large page uncached. */
		for (index = 0; index < count; index++) {
			system_mmio_pd[AMD64_FRAMEBUFFER_PD_FIRST + index] =
			    (base + (uint64_t)index * 0x200000ULL) |
			    AMD64_PTE_PRESENT | AMD64_PTE_WRITE |
			    AMD64_PTE_NOCACHE | AMD64_PTE_LARGE |
			    AMD64_PTE_GLOBAL | AMD64_PTE_NX;
		}
	}

	/* Permits AP execution through the low trampoline mapping. */
	system_pd[0] &= ~AMD64_PTE_NX;

	/* Connects the direct-map page directory. */
	system_pdpt[0] = amd64_direct_to_phys(system_pd) |
	    AMD64_PTE_PRESENT | AMD64_PTE_WRITE;

	/* Connects every ACPI window page table. */
	for (index = 0; index < AMD64_ACPI_WINDOW_PT_COUNT; index++) {
		system_acpi_pd[index] =
		    amd64_direct_to_phys(system_acpi_pt[index]) |
		    AMD64_PTE_PRESENT | AMD64_PTE_WRITE;
	}

	/* Connects the ACPI, direct-map, and fixed-MMIO directories. */
	system_pdpt[AMD64_ACPI_PDPT_INDEX] =
	    amd64_direct_to_phys(system_acpi_pd) |
	    AMD64_PTE_PRESENT | AMD64_PTE_WRITE;
	system_pdpt[510] = amd64_direct_to_phys(system_pd) |
	    AMD64_PTE_PRESENT | AMD64_PTE_WRITE;
	system_pdpt[511] = amd64_direct_to_phys(system_mmio_pd) |
	    AMD64_PTE_PRESENT | AMD64_PTE_WRITE;

	/* Publishes the high and temporary low PML4 roots. */
	system_pml4[511] = amd64_direct_to_phys(system_pdpt) |
	    AMD64_PTE_PRESENT | AMD64_PTE_WRITE;
	system_pml4[0] = amd64_direct_to_phys(system_pdpt) |
	    AMD64_PTE_PRESENT | AMD64_PTE_WRITE;

	/* Enables execute-disable, global pages, and supervisor write protection. */
	efer = asm_read_msr(0xc0000080U);
	asm_write_msr(0xc0000080U, efer | (1ULL << 11));
	__asm__ volatile("movq %%cr4,%0" : "=r"(cr4));
	cr4 |= 0x80U;
	__asm__ volatile("movq %0,%%cr4" : : "r"(cr4) : "memory");
	__asm__ volatile("movq %%cr0,%0" : "=r"(cr0));
	cr0 |= 0x10000U;
	__asm__ volatile("movq %0,%%cr0" : : "r"(cr0) : "memory");

	/* Activates and records the completed system page-table root. */
	system_cr3 = amd64_direct_to_phys(system_pml4);
	asm_load_cr3(system_cr3);
	__atomic_store_n(
	    &AMD64_CURRENT_SPACE,
	    HAL_SPACE_SYS,
	    __ATOMIC_RELEASE);
}

/*
 * Maps an ACPI physical range into the reserved system window.
 */
const void *
amd64_acpi_map_physical(
	paddr_t physical,
	size_t size)
{
	const void *result;
	unsigned first;
	unsigned new_first;
	unsigned new_count;
	unsigned index;
	unsigned slot;
	paddr_t page_physical;
	paddr_t page;
	size_t offset;
	size_t page_span;
	int mappable;
	int reserved;

	/* Splits the requested address into its page and byte offset. */
	page_physical = physical & ~(paddr_t)(PAGE_SIZE - 1U);
	offset = (size_t)(physical - page_physical);

	/* Rejects an empty mapping. */
	if (size == 0)
		return NULL;

	/* Rejects an offset and size which overflow. */
	if (size > SIZE_MAX - offset)
		return NULL;

	/* Rejects a request larger than the reserved virtual window. */
	if (size + offset >
	    (size_t)AMD64_ACPI_WINDOW_SLOTS * PAGE_SIZE)
		return NULL;

	/* Rounds the mapping extent to whole pages. */
	page_span = size + offset;

	/* Rejects an extent whose page rounding would overflow. */
	if (page_span > SIZE_MAX - (PAGE_SIZE - 1U))
		return NULL;

	/* Applies the validated whole-page rounding. */
	page_span = (page_span + PAGE_SIZE - 1U) &
	    ~(size_t)(PAGE_SIZE - 1U);

	/* Rejects a starting page outside the CPU physical-address range. */
	if (page_physical > acpi_physical_max)
		return NULL;

	/* Rejects an ending page outside the CPU physical-address range. */
	if (page_span - 1U > acpi_physical_max - page_physical)
		return NULL;

	/* Confirms that the BSP permits access to the physical extent. */
	mappable = bsp_physical_range_mappable(page_physical, page_span);
	if (!mappable)
		return NULL;

	/* Reserves slots for all newly required pages. */
	reserved = amd64_acpi_window_reserve(
		&acpi_window,
		physical,
		size,
		&first,
		&offset,
		&new_first,
		&new_count);
	if (!reserved)
		return NULL;

	/* Installs every page newly assigned by the window allocator. */
	for (index = 0; index < new_count; index++) {
		slot = new_first + index;
		page = acpi_window.slot_physical[slot];
		system_acpi_pt[slot / 512U][slot % 512U] =
		    (uint64_t)page | AMD64_PTE_PRESENT | AMD64_PTE_NX;
	}

	/* Makes newly installed translations visible before flushing the TLB. */
	if (new_count != 0) {
		__atomic_thread_fence(__ATOMIC_RELEASE);
		asm_flush_tlb();
	}

	/* Forms the address of the requested byte within its reserved slot. */
	result = (const void *)(uintptr_t)(AMD64_ACPI_WINDOW_BASE +
	    (uint64_t)first * PAGE_SIZE + offset);

	/* Reports the completed ACPI mapping. */
	return result;
}

/*
 * Maps a PCI ECAM range into the fixed MMIO window.
 */
int
amd64_mmio_map_ecam(
	paddr_t physical,
	size_t size,
	void **result)
{
	const uint64_t page_size = 0x200000ULL;
	uint64_t aligned;
	uint64_t end;
	uint64_t offset;
	unsigned count;
	unsigned index;
	unsigned first;

	/* Rejects a missing output location. */
	if (result == NULL)
		return HAL_ERR_INVALID;

	/* Rejects an empty mapping. */
	if (size == 0)
		return HAL_ERR_INVALID;

	/* Rejects a physical extent which overflows. */
	if (physical > UINT64_MAX - size)
		return HAL_ERR_INVALID;

	/* Computes the enclosing large-page extent. */
	aligned = (uint64_t)physical & ~(page_size - 1U);
	offset = (uint64_t)physical - aligned;
	end = offset + size;
	count = (unsigned)((end + page_size - 1U) / page_size);

	/* Rejects a request outside the remaining ECAM window. */
	if (count == 0 || count > AMD64_ECAM_PD_COUNT - ecam_pd_used)
		return HAL_ERR_UNSUPPORTED;

	/* Installs every large page in the requested ECAM extent. */
	first = AMD64_ECAM_PD_FIRST + ecam_pd_used;
	for (index = 0; index < count; index++) {
		system_mmio_pd[first + index] = (aligned +
		    (uint64_t)index * page_size) | AMD64_PTE_PRESENT |
		    AMD64_PTE_WRITE | AMD64_PTE_NOCACHE | AMD64_PTE_LARGE |
		    AMD64_PTE_GLOBAL | AMD64_PTE_NX;
	}

	/* Publishes the consumed window and mapped virtual address. */
	ecam_pd_used += count;
	*result = (void *)(uintptr_t)(AMD64_ECAM_VIRTUAL_BASE +
	    (uint64_t)(first - AMD64_ECAM_PD_FIRST) * page_size + offset);

	/* Reports a successful ECAM mapping. */
	return HAL_OK;
}

/*
 * Creates an empty user address space.
 */
hal_space_t
hal_mem_create_space(
	void)
{
	struct amd64_space *space;
	bool enabled;
	int status;

	/* Allocates the software address-space record. */
	space = hal_malloc(sizeof(*space));
	if (space == NULL)
		return NULL;

	/* Initializes the software record before acquiring its root page. */
	hal_memset(space, 0, sizeof(*space));

	/* Allocates the top-level hardware page table. */
	status = alloc_page(&space->pml4_memory);
	if (status != HAL_OK) {
		hal_free(space);
		return NULL;
	}

	/* Initializes the user root with the shared system mapping. */
	space->pml4 = space->pml4_memory.vaddr;
	hal_memset(space->pml4, 0, PAGE_SIZE);
	space->pml4[511] = system_pml4[511];
	space->magic = AMD64_SPACE_MAGIC;
	space->space_id = __atomic_fetch_add(
	    &next_space_id,
	    1,
	    __ATOMIC_RELAXED);
	(void)__atomic_fetch_add(&space_count, 1U, __ATOMIC_RELAXED);

	/* Publishes the initialized space in the lifetime registry. */
	enabled = registry_lock_enter();
	space->registry_next = space_registry;
	space_registry = space;
	registry_lock_leave(enabled);

	/* Reports the new address-space handle. */
	return space;
}

/*
 * Destroys a retired user address space.
 */
void
hal_page_destroy_space(
	hal_space_t handle)
{
	struct amd64_space *space;
	struct amd64_space **link;
	struct amd64_table_page *page;
	struct hal_cpu_mask ready;
	struct amd64_percpu *target;
	hal_cpu_id_t cpu;
	hal_cpu_id_t current_cpu;
	unsigned active;
	uint32_t old_count;
	bool enabled;

	/* Resolves the opaque handle before validating its lifetime. */
	space = handle;

	/* Ignores a null address-space handle. */
	if (space == NULL)
		return;

	/* Finds and retires the space under the registry lock. */
	enabled = registry_lock_enter();
	link = &space_registry;
	while (*link != NULL && *link != space)
		link = &(*link)->registry_next;

	/* Rejects an unknown or already retiring space. */
	if (*link == NULL || space->destroying)
		HAL_FATAL("invalid amd64 space destroy");

	/* Retires and unlinks the space before releasing the registry. */
	space->destroying = 1U;
	*link = space->registry_next;
	registry_lock_leave(enabled);

	/*
	 * Waits for operations admitted before retirement to release their
	 * ownership of the page-table storage.
	 */
	for (;;) {
		enabled = registry_lock_enter();
		active = space->active_ops;
		registry_lock_leave(enabled);

		/* Leaves the wait once every admitted operation has departed. */
		if (active == 0)
			break;

		/* Services reciprocal requests while waiting for ownership release. */
		current_cpu = hal_cpu_current();
		service_shootdowns(current_cpu);
		__asm__ volatile("pause");
	}

	/* Closes every cached translation before checking active CPUs. */
	shootdown(space, NULL, 0);

	/*
	 * Verifies the generic kernel detached all tasks from this space.
	 * The HAL closes hardware translation windows but never changes task
	 * ownership implicitly.
	 */
	hal_cpu_ready_mask(&ready);
	for (cpu = 0; cpu < hal_cpu_count(); cpu++) {
		/* Skips processors which have not completed startup. */
		if (!hal_cpu_mask_test(&ready, cpu))
			continue;

		/* Resolves the ready processor's current ownership record. */
		target = amd64_percpu_get(cpu);

		/* Rejects destruction while a ready CPU still owns this space. */
		if (__atomic_load_n(
		    &target->current_space,
		    __ATOMIC_ACQUIRE) == space) {
			HAL_FATAL("destroying an active amd64 space");
		}
	}

	/* Releases every subordinate page table. */
	while ((page = space->tables) != NULL) {
		space->tables = page->next;
		(void)hal_pmem_free(&page->memory);
		hal_free(page);
		table_count_drop();
	}

	/* Invalidates and releases the top-level space record. */
	space->magic = 0;
	(void)hal_pmem_free(&space->pml4_memory);
	hal_free(space);

	/* Accounts for the released address space. */
	old_count = __atomic_fetch_sub(&space_count, 1U, __ATOMIC_RELAXED);
	if (old_count == 0)
		HAL_FATAL("amd64 space counter underflow");
}

/*
 * Switches the current CPU to an address space.
 */
void
hal_page_switch_space(
	hal_space_t handle)
{
	struct amd64_space *space;
	uintptr_t cr3;
	bool enabled;
	int entered;

	/* Keeps the current hardware space when no switch is required. */
	if (__atomic_load_n(
	    &AMD64_CURRENT_SPACE,
	    __ATOMIC_ACQUIRE) == handle) {
		return;
	}

	/* Switches directly to the immortal system page tables. */
	if (handle == HAL_SPACE_SYS) {
		enabled = hal_irq_disable();
		asm_load_cr3(system_cr3);
		__atomic_store_n(
		    &AMD64_CURRENT_SPACE,
		    handle,
		    __ATOMIC_RELEASE);

		/* Restores the caller's interrupt state. */
		if (enabled)
			hal_irq_enable();

		/* Completes the system-space switch. */
		return;
	}

	/* Acquires lifetime ownership of the requested user space. */
	space = handle;
	entered = space_op_enter(space);
	if (!entered)
		HAL_FATAL("invalid amd64 space switch");

	/* Serializes the CR3 switch with page-table operations. */
	enabled = space_lock_enter(space);
	cr3 = (uintptr_t)space->pml4_memory.paddr;
	asm_load_cr3(cr3);
	__atomic_store_n(&AMD64_CURRENT_SPACE, handle, __ATOMIC_RELEASE);
	space_lock_leave(space, enabled);
	space_op_leave(space);
}

/*
 * Maps physical pages into a user address space.
 */
int
hal_page_map(
	hal_space_t handle,
	void *pointer,
	hal_physaddr_t physical,
	size_t size,
	uint32_t attr)
{
	struct amd64_space *space;
	struct amd64_table_page *detached;
	uint64_t *leaf;
	uintptr_t address;
	uintptr_t offset;
	uintptr_t rollback;
	bool enabled;
	int entered;

	/* Resolves the mapping destination and its virtual base. */
	space = handle;
	address = (uintptr_t)pointer;

	/* Rejects a missing address space. */
	if (space == NULL)
		return HAL_ERR_INVALID;

	/* Rejects an invalid user virtual range. */
	if (!valid_user_range(address, size))
		return HAL_ERR_INVALID;

	/* Rejects an unaligned physical base. */
	if ((physical & (PAGE_SIZE - 1U)) != 0)
		return HAL_ERR_INVALID;

	/* Rejects a physical base outside the direct-map limit. */
	if (physical >= AMD64_DIRECT_LIMIT)
		return HAL_ERR_INVALID;

	/* Rejects a physical extent outside the direct-map limit. */
	if (size > AMD64_DIRECT_LIMIT - physical)
		return HAL_ERR_INVALID;

	/* Requires at least one useful access permission. */
	if (!(attr & (HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC)))
		return HAL_ERR_INVALID;

	/* Acquires lifetime ownership of the destination space. */
	entered = space_op_enter(space);
	if (!entered)
		return HAL_ERR_STATE;

	/* Serializes validation and publication of the mapping. */
	enabled = space_lock_enter(space);

	/* Verifies that every destination leaf is currently unmapped. */
	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		leaf = walk_leaf(space, address + offset, 0);

		/* Rejects an overlap with an existing mapping. */
		if (leaf != NULL && (*leaf & AMD64_PTE_PRESENT)) {
			space_lock_leave(space, enabled);
			space_op_leave(space);
			return HAL_ERR_INVALID;
		}
	}

	/* Installs each requested leaf mapping. */
	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		leaf = walk_leaf(space, address + offset, 1);

		/* Rolls back all installed leaves when table allocation fails. */
		if (leaf == NULL) {
			/* Clears every leaf installed by this request. */
			for (rollback = 0;
			     rollback < offset;
			     rollback += PAGE_SIZE) {
				leaf = walk_leaf(space, address + rollback, 0);

				/* Clears the leaf when its table remains reachable. */
				if (leaf != NULL)
					*leaf = 0;
			}

			/* Disconnects page tables made empty by rollback. */
			detached = detach_empty_tables(space);

			/* Flushes the broadest range required by the rollback. */
			if (detached != NULL) {
				shootdown(space, NULL, 0);
			} else if (offset != 0) {
				shootdown(space, pointer, offset);
			}

			/* Releases detached tables after remote acknowledgement. */
			free_detached_tables(detached);
			space_lock_leave(space, enabled);
			space_op_leave(space);
			return HAL_ERR_NOMEM;
		}

		/* Publishes this requested leaf mapping. */
		*leaf = (physical + offset) | leaf_flags(attr);
	}

	/* Invalidates translations on CPUs currently using this space. */
	shootdown(space, pointer, size);

	/* Releases mapping and lifetime serialization. */
	space_lock_leave(space, enabled);
	space_op_leave(space);

	/* Reports a successful mapping. */
	return HAL_OK;
}

/*
 * Changes permissions on a mapped user range.
 */
int
hal_page_prot(
	hal_space_t handle,
	void *pointer,
	size_t size,
	uint32_t attr)
{
	int status;

	/* Applies permissions without requesting observed hardware flags. */
	status = hal_page_prot_query(handle, pointer, size, attr, NULL);

	/* Reports the permission operation status. */
	return status;
}

/*
 * Changes permissions and reports observed page flags.
 */
int
hal_page_prot_query(
	hal_space_t handle,
	void *pointer,
	size_t size,
	uint32_t attr,
	uint32_t *flags)
{
	struct amd64_space *space;
	uint64_t *leaf;
	uint64_t old;
	uint64_t desired;
	uint64_t entry;
	uintptr_t address;
	uintptr_t offset;
	uint32_t observed;
	bool enabled;
	int entered;

	/* Resolves the destination and initializes flag observation. */
	space = handle;
	address = (uintptr_t)pointer;
	observed = 0;

	/* Rejects a missing address space. */
	if (space == NULL)
		return HAL_ERR_INVALID;

	/* Rejects an invalid user virtual range. */
	if (!valid_user_range(address, size))
		return HAL_ERR_INVALID;

	/* Requires at least one useful access permission. */
	if (!(attr & (HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC)))
		return HAL_ERR_INVALID;

	/* Acquires lifetime ownership of the destination space. */
	entered = space_op_enter(space);
	if (!entered)
		return HAL_ERR_STATE;

	/* Serializes validation and replacement of the mappings. */
	enabled = space_lock_enter(space);

	/* Validates the complete range before publishing any permission change. */
	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		leaf = walk_leaf(space, address + offset, 0);

		/* Rejects a hole in the requested mapped range. */
		if (leaf == NULL || !(*leaf & AMD64_PTE_PRESENT)) {
			space_lock_leave(space, enabled);
			space_op_leave(space);
			return HAL_ERR_INVALID;
		}
	}

	/* Replaces each leaf while preserving accessed and dirty observations. */
	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		leaf = walk_leaf(space, address + offset, 0);
		old = __atomic_load_n(leaf, __ATOMIC_ACQUIRE);

		/* Retries the replacement if hardware updates the leaf concurrently. */
		do {
			desired = (old & AMD64_PTE_ADDR_MASK) |
			    leaf_flags(attr) |
			    (old & (AMD64_PTE_ACCESSED | AMD64_PTE_DIRTY));
		} while (!__atomic_compare_exchange_n(
		    leaf,
		    &old,
		    desired,
		    false,
		    __ATOMIC_ACQ_REL,
		    __ATOMIC_ACQUIRE));

		/* Records a previously observed access. */
		if (old & AMD64_PTE_ACCESSED)
			observed |= HAL_PAGE_ACCESSED;

		/* Records a previously observed write. */
		if (old & AMD64_PTE_DIRTY)
			observed |= HAL_PAGE_DIRTY;
	}

	/* Invalidates every old translation before the final observation. */
	shootdown(space, pointer, size);

	/*
	 * Collects A/D updates made through old remote translations before
	 * those CPUs acknowledged the shootdown.
	 */
	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		leaf = walk_leaf(space, address + offset, 0);

		/* Detects an unexpected page-table topology change. */
		if (leaf == NULL) {
			space_lock_leave(space, enabled);
			space_op_leave(space);
			return HAL_ERR_STATE;
		}

		/* Reads the replacement leaf after remote acknowledgement. */
		entry = __atomic_load_n(leaf, __ATOMIC_ACQUIRE);

		/* Detects an unexpectedly absent replacement leaf. */
		if (!(entry & AMD64_PTE_PRESENT)) {
			space_lock_leave(space, enabled);
			space_op_leave(space);
			return HAL_ERR_STATE;
		}

		/* Records that the replacement leaf remains present. */
		observed |= HAL_PAGE_PRESENT;

		/* Records an access observed after the shootdown. */
		if (entry & AMD64_PTE_ACCESSED)
			observed |= HAL_PAGE_ACCESSED;

		/* Records a write observed after the shootdown. */
		if (entry & AMD64_PTE_DIRTY)
			observed |= HAL_PAGE_DIRTY;
	}

	/* Releases mapping and lifetime serialization. */
	space_lock_leave(space, enabled);
	space_op_leave(space);

	/* Publishes observed flags when requested by the caller. */
	if (flags != NULL)
		*flags = observed;

	/* Reports a successful permission update. */
	return HAL_OK;
}

/*
 * Removes mappings from a user address space.
 */
int
hal_page_unmap(
	hal_space_t handle,
	void *pointer,
	size_t size)
{
	struct amd64_space *space;
	struct amd64_table_page *detached;
	uint64_t *leaf;
	uintptr_t address;
	uintptr_t offset;
	bool enabled;
	int entered;

	/* Resolves the unmap destination and its virtual base. */
	space = handle;
	address = (uintptr_t)pointer;

	/* Accepts an empty unmap operation without inspecting the handle. */
	if (size == 0)
		return HAL_OK;

	/* Rejects a missing address space. */
	if (space == NULL)
		return HAL_ERR_INVALID;

	/* Rejects an invalid user virtual range. */
	if (!valid_user_range(address, size))
		return HAL_ERR_INVALID;

	/* Acquires lifetime ownership of the destination space. */
	entered = space_op_enter(space);
	if (!entered)
		return HAL_ERR_STATE;

	/* Serializes removal of the requested leaves. */
	enabled = space_lock_enter(space);

	/* Clears every reachable leaf in the requested range. */
	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		leaf = walk_leaf(space, address + offset, 0);

		/* Clears the mapping when its leaf table exists. */
		if (leaf != NULL)
			*leaf = 0;
	}

	/*
	 * Disconnects empty tables before acknowledgement while retaining their
	 * storage until every stale translation and page walk has ended.
	 */
	detached = detach_empty_tables(space);

	/* Flushes the full space when parent entries were disconnected. */
	if (detached != NULL) {
		shootdown(space, NULL, 0);
	} else {
		shootdown(space, pointer, size);
	}

	/* Releases detached tables after remote acknowledgement. */
	free_detached_tables(detached);
	space_lock_leave(space, enabled);
	space_op_leave(space);

	/* Reports a successful unmap operation. */
	return HAL_OK;
}

/*
 * Reports hardware flags for one user page.
 */
int
hal_page_query(
	hal_space_t handle,
	void *pointer,
	uint32_t *flags)
{
	struct amd64_space *space;
	uint64_t *leaf;
	bool enabled;
	int entered;

	/* Resolves the queried address-space handle. */
	space = handle;

	/* Rejects a missing address space or output location. */
	if (space == NULL || flags == NULL)
		return HAL_ERR_INVALID;

	/* Rejects an invalid user page address. */
	if (!valid_user_range((uintptr_t)pointer, PAGE_SIZE))
		return HAL_ERR_INVALID;

	/* Acquires lifetime ownership of the queried space. */
	entered = space_op_enter(space);
	if (!entered)
		return HAL_ERR_STATE;

	/* Reads the leaf under the page-table serializer. */
	enabled = space_lock_enter(space);
	leaf = walk_leaf(space, (uintptr_t)pointer, 0);
	*flags = leaf != NULL && (*leaf & AMD64_PTE_PRESENT) ?
	    HAL_PAGE_PRESENT : 0;

	/* Reports an observed access bit. */
	if (leaf != NULL && (*leaf & AMD64_PTE_ACCESSED))
		*flags |= HAL_PAGE_ACCESSED;

	/* Reports an observed dirty bit. */
	if (leaf != NULL && (*leaf & AMD64_PTE_DIRTY))
		*flags |= HAL_PAGE_DIRTY;

	/* Releases page-table and lifetime ownership. */
	space_lock_leave(space, enabled);
	space_op_leave(space);

	/* Reports a successful query. */
	return HAL_OK;
}

/*
 * Clears selected hardware flags on one user page.
 */
int
hal_page_clear_flags(
	hal_space_t handle,
	void *pointer,
	uint32_t flags)
{
	struct amd64_space *space;
	uint64_t *leaf;
	uint64_t mask;
	uint64_t old;
	uint64_t desired;
	bool enabled;
	int entered;

	/* Resolves the destination and initializes its hardware clear mask. */
	space = handle;
	mask = 0;

	/* Rejects a missing address space. */
	if (space == NULL)
		return HAL_ERR_INVALID;

	/* Rejects an invalid user page address. */
	if (!valid_user_range((uintptr_t)pointer, PAGE_SIZE))
		return HAL_ERR_INVALID;

	/* Rejects flags outside the clearable hardware set. */
	if ((flags & ~(HAL_PAGE_ACCESSED | HAL_PAGE_DIRTY)) != 0)
		return HAL_ERR_INVALID;

	/* Acquires lifetime ownership of the destination space. */
	entered = space_op_enter(space);
	if (!entered)
		return HAL_ERR_STATE;

	/* Locates the leaf under the page-table serializer. */
	enabled = space_lock_enter(space);
	leaf = walk_leaf(space, (uintptr_t)pointer, 0);

	/* Rejects an absent mapping. */
	if (leaf == NULL || !(*leaf & AMD64_PTE_PRESENT)) {
		space_lock_leave(space, enabled);
		space_op_leave(space);
		return HAL_ERR_INVALID;
	}

	/* Builds the hardware bit mask requested by the caller. */
	if (flags & HAL_PAGE_ACCESSED)
		mask |= AMD64_PTE_ACCESSED;

	/* Includes the dirty bit when requested by the caller. */
	if (flags & HAL_PAGE_DIRTY)
		mask |= AMD64_PTE_DIRTY;

	/* Clears the selected bits despite concurrent hardware updates. */
	old = __atomic_load_n(leaf, __ATOMIC_ACQUIRE);
	do {
		desired = old & ~mask;
	} while (!__atomic_compare_exchange_n(
	    leaf,
	    &old,
	    desired,
	    false,
	    __ATOMIC_ACQ_REL,
	    __ATOMIC_ACQUIRE));

	/* Invalidates cached translations of the changed leaf. */
	shootdown(space, pointer, PAGE_SIZE);

	/* Releases page-table and lifetime ownership. */
	space_lock_leave(space, enabled);
	space_op_leave(space);

	/* Reports a successful flag clear. */
	return HAL_OK;
}

/*
 * Services a TLB shootdown interrupt.
 */
void
amd64_tlb_interrupt(
	void)
{
	hal_irq_ack_t acknowledge;
	hal_cpu_id_t cpu;

	/* Acknowledges and services all requests pending for this CPU. */
	acknowledge = amd64_irq_ack_begin(AMD64_VECTOR_TLB, -1);
	cpu = hal_cpu_current();
	service_shootdowns(cpu);
	hal_irq_send_eoi(acknowledge);
}

/*
 * Flushes all translations for an address space.
 */
void
hal_page_flush_tlb(
	hal_space_t handle)
{
	struct amd64_space *space;
	bool enabled;
	int entered;

	/* Flushes the immortal system space without registry ownership. */
	if (handle == HAL_SPACE_SYS) {
		shootdown(handle, NULL, 0);

		/* Completes the system-space flush. */
		return;
	}

	/* Acquires lifetime ownership of the requested user space. */
	space = handle;
	entered = space_op_enter(space);
	if (!entered)
		HAL_FATAL("invalid amd64 space flush");

	/* Serializes and broadcasts the complete invalidation. */
	enabled = space_lock_enter(space);
	shootdown(space, NULL, 0);
	space_lock_leave(space, enabled);
	space_op_leave(space);
}

/*
 * Flushes a translation range for an address space.
 */
void
hal_page_flush_tlb_range(
	hal_space_t handle,
	void *vaddr,
	size_t size)
{
	struct amd64_space *space;
	bool enabled;
	int entered;

	/* Ignores an empty flush range. */
	if (size == 0)
		return;

	/* Flushes the immortal system space without registry ownership. */
	if (handle == HAL_SPACE_SYS) {
		shootdown(handle, vaddr, size);

		/* Completes the system-space range flush. */
		return;
	}

	/* Acquires lifetime ownership of the requested user space. */
	space = handle;
	entered = space_op_enter(space);
	if (!entered)
		HAL_FATAL("invalid amd64 space range flush");

	/* Serializes and broadcasts the range invalidation. */
	enabled = space_lock_enter(space);
	shootdown(space, vaddr, size);
	space_lock_leave(space, enabled);
	space_op_leave(space);
}

/*
 * Reports the page size for a translation level.
 */
size_t
hal_page_get_page_size(
	int level)
{
	/* Reports the leaf-page size. */
	if (level == 1)
		return PAGE_SIZE;

	/* Reports the supported large-page size. */
	if (level == 2)
		return 0x200000U;

	/* Reports an unsupported translation level. */
	return 0;
}

/*
 * Reports the valid user virtual-address range.
 */
void
hal_page_get_user_range(
	uintptr_t *minimum,
	uintptr_t *limit)
{
	/* Publishes the lowest valid user page when requested. */
	if (minimum != NULL)
		*minimum = PAGE_SIZE;

	/* Publishes the exclusive upper user address when requested. */
	if (limit != NULL)
		*limit = AMD64_USER_LIMIT;
}

/*
 * Reports live amd64 address-space memory statistics.
 */
void
hal_amd64_space_memory_stats(
	uint32_t *spaces,
	uint32_t *tables)
{
	/* Publishes the live address-space count when requested. */
	if (spaces != NULL)
		*spaces = __atomic_load_n(&space_count, __ATOMIC_RELAXED);

	/* Publishes the subordinate page-table count when requested. */
	if (tables != NULL) {
		*tables = __atomic_load_n(
		    &page_table_count,
		    __ATOMIC_RELAXED);
	}
}

/* Reports the highest physical address supported by the CPU. */
static paddr_t
cpu_physical_max(
	void)
{
	uint32_t eax;
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;
	unsigned bits;
	paddr_t maximum;

	/* Queries the highest supported extended CPUID leaf. */
	eax = 0x80000000U;
	bits = 36U;
	__asm__ volatile("cpuid"
	    : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));

	/* Reads the architectural physical-address width when available. */
	if (eax >= 0x80000008U) {
		eax = 0x80000008U;
		__asm__ volatile("cpuid"
		    : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
		bits = eax & 0xffU;
	}

	/* Falls back when firmware exposes an invalid architectural width. */
	if (bits < 32U || bits > 52U)
		bits = 36U;

	/* Forms the inclusive maximum physical address. */
	maximum = ((paddr_t)1U << bits) - 1U;

	/* Reports the physical-address ceiling. */
	return maximum;
}

/* Decrements the subordinate page-table allocation count. */
static void
table_count_drop(
	void)
{
	uint32_t old;

	/* Accounts for one released page table. */
	old = __atomic_fetch_sub(&page_table_count, 1U, __ATOMIC_RELAXED);

	/* Detects page-table accounting underflow. */
	if (old == 0)
		HAL_FATAL("amd64 page-table counter underflow");
}

/* Allocates one page-table page. */
static int
alloc_page(
	struct hal_pmem *memory)
{
	const struct hal_pmem_request request = {
		HAL_PMEM_PADDR_ANY,
		PAGE_SIZE,
		PAGE_SIZE,
		HAL_PMEM_TYPE_RAM,
		0
	};
	int status;

	/* Allocates a page matching the hardware table constraints. */
	status = hal_pmem_alloc(&request, memory);

	/* Reports the allocator result. */
	return status;
}

/* Acquires the address-space registry lock. */
static bool
registry_lock_enter(
	void)
{
	bool enabled;

	/* Disables local interrupts before taking the registry serializer. */
	enabled = hal_irq_disable();

	/* Waits until this CPU owns the registry serializer. */
	while (__atomic_exchange_n(
	    &space_registry_lock,
	    1U,
	    __ATOMIC_ACQUIRE) != 0) {
		__asm__ volatile("pause");
	}

	/* Reports the caller's prior interrupt state. */
	return enabled;
}

/* Releases the address-space registry lock. */
static void
registry_lock_leave(
	bool enabled)
{
	/* Publishes every registry update before releasing ownership. */
	__atomic_store_n(&space_registry_lock, 0U, __ATOMIC_RELEASE);

	/* Restores enabled interrupts when the caller had them enabled. */
	if (enabled)
		hal_irq_enable();
}

/* Registers an operation against a space which may be retired. */
static int
space_op_enter(
	struct amd64_space *space)
{
	struct amd64_space *item;
	bool enabled;

	/* Searches the live registry while retirement is excluded. */
	enabled = registry_lock_enter();
	item = space_registry;
	while (item != NULL && item != space)
		item = item->registry_next;

	/* Rejects an unknown or retiring address space. */
	if (item == NULL || item->destroying) {
		registry_lock_leave(enabled);
		return 0;
	}

	/* Records lifetime ownership before releasing the registry. */
	item->active_ops++;
	registry_lock_leave(enabled);

	/* Reports successful admission. */
	return 1;
}

/* Releases an admitted address-space operation. */
static void
space_op_leave(
	struct amd64_space *space)
{
	bool enabled;

	/* Serializes the lifetime ownership update. */
	enabled = registry_lock_enter();

	/* Detects an unbalanced operation release. */
	if (space->active_ops == 0)
		HAL_FATAL("amd64 space operation counter underflow");

	/* Releases lifetime ownership before leaving the registry lock. */
	space->active_ops--;
	registry_lock_leave(enabled);
}

/* Acquires one address space's page-table lock. */
static bool
space_lock_enter(
	struct amd64_space *space)
{
	bool enabled;

	/* Disables interrupts before acquiring the page-table serializer. */
	enabled = hal_irq_disable();

	/*
	 * Waits for ownership while servicing incoming invalidations.  A target
	 * spinning with IRQs masked must acknowledge a request issued by the CPU
	 * which owns this serializer.
	 */
	while (__atomic_exchange_n(
	    &space->lock,
	    1U,
	    __ATOMIC_ACQUIRE) != 0) {
		service_shootdowns(hal_cpu_current());
		__asm__ volatile("pause");
	}

	/* Reports the caller's prior interrupt state. */
	return enabled;
}

/* Releases one address space's page-table lock. */
static void
space_lock_leave(
	struct amd64_space *space,
	bool enabled)
{
	/* Publishes page-table updates before releasing ownership. */
	__atomic_store_n(&space->lock, 0U, __ATOMIC_RELEASE);

	/* Restores enabled interrupts when the caller had them enabled. */
	if (enabled)
		hal_irq_enable();
}

/* Validates one page-aligned user virtual range. */
static int
valid_user_range(
	uintptr_t address,
	size_t size)
{
	/* Rejects an empty range. */
	if (size == 0)
		return 0;

	/* Rejects an unaligned starting address. */
	if ((address & (PAGE_SIZE - 1U)) != 0)
		return 0;

	/* Rejects a non-page-sized extent. */
	if ((size & (PAGE_SIZE - 1U)) != 0)
		return 0;

	/* Keeps the null guard page outside user mappings. */
	if (address < PAGE_SIZE)
		return 0;

	/* Rejects a starting address outside the lower canonical half. */
	if (address >= AMD64_USER_LIMIT)
		return 0;

	/* Rejects an extent which crosses the user limit. */
	if (size > AMD64_USER_LIMIT - address)
		return 0;

	/* Reports a valid user range. */
	return 1;
}

/* Allocates and links one subordinate page table. */
static struct amd64_table_page *
allocate_table(
	struct amd64_space *space,
	uint64_t *parent,
	unsigned parent_index)
{
	struct amd64_table_page *page;
	int status;

	/* Allocates the software ownership record. */
	page = hal_malloc(sizeof(*page));
	if (page == NULL)
		return NULL;

	/* Allocates the physical page-table storage. */
	status = alloc_page(&page->memory);
	if (status != HAL_OK) {
		hal_free(page);
		return NULL;
	}

	/* Initializes and links the new subordinate table. */
	hal_memset(page->memory.vaddr, 0, PAGE_SIZE);
	page->parent = parent;
	page->parent_index = parent_index;
	page->next = space->tables;
	space->tables = page;
	(void)__atomic_fetch_add(&page_table_count, 1U, __ATOMIC_RELAXED);

	/* Reports the linked table owner. */
	return page;
}

/* Finds or creates the leaf entry for a virtual address. */
static uint64_t *
walk_leaf(
	struct amd64_space *space,
	uintptr_t address,
	int create)
{
	unsigned shifts[3] = { 39, 30, 21 };
	struct amd64_table_page *page;
	uint64_t *table;
	uint64_t entry;
	unsigned level;
	unsigned index;

	/* Descends the three page-table levels above the leaf. */
	table = space->pml4;
	for (level = 0; level < 3; level++) {
		index = (unsigned)(address >> shifts[level]) & 511U;
		entry = table[index];

		/* Creates a missing subordinate table when requested. */
		if (!(entry & AMD64_PTE_PRESENT)) {
			/* Reports a missing path to a lookup-only caller. */
			if (!create)
				return NULL;

			/* Allocates the subordinate table required by this level. */
			page = allocate_table(space, table, index);

			/* Reports allocation failure to the mapping caller. */
			if (page == NULL)
				return NULL;

			/* Links the new subordinate table into its parent. */
			entry = (uintptr_t)page->memory.paddr |
			    AMD64_PTE_PRESENT | AMD64_PTE_WRITE |
			    AMD64_PTE_USER;
			table[index] = entry;
		}

		/* Rejects an unexpected large page in a user-table path. */
		if (entry & AMD64_PTE_LARGE)
			return NULL;

		/* Descends through the selected subordinate table. */
		table = amd64_phys_to_direct(
			(uintptr_t)(entry & AMD64_PTE_ADDR_MASK));
	}

	/* Reports the leaf entry selected by the address. */
	return &table[(address >> 12) & 511U];
}

/* Tests whether a subordinate page table has no present entries. */
static int
table_is_empty(
	const uint64_t *table)
{
	unsigned index;

	/* Searches for any present entry. */
	for (index = 0; index < 512; index++) {
		/* Reports a table which still owns a child mapping. */
		if (table[index] & AMD64_PTE_PRESENT)
			return 0;
	}

	/* Reports an empty table. */
	return 1;
}

/* Detaches every empty table without releasing its storage. */
static struct amd64_table_page *
detach_empty_tables(
	struct amd64_space *space)
{
	struct amd64_table_page *detached;
	struct amd64_table_page **link;
	struct amd64_table_page *page;
	uint64_t expected;
	uint64_t parent_entry;
	int reclaimed;

	/* Starts with an empty detached-table result list. */
	detached = NULL;

	/*
	 * Repeats until removing a child no longer makes another table empty.
	 * Storage remains valid until the caller completes a full shootdown.
	 */
	do {
		link = &space->tables;
		reclaimed = 0;

		/* Examines every table still linked to this address space. */
		while (*link != NULL) {
			page = *link;
			expected = (uintptr_t)page->memory.paddr;

			/* Keeps nonempty tables linked in the hardware tree. */
			if (!table_is_empty(page->memory.vaddr)) {
				link = &page->next;
				continue;
			}

			/* Reads the software-owned table's parent link. */
			parent_entry = page->parent[page->parent_index];

			/* Detects a broken software-to-hardware ownership link. */
			if (!(parent_entry & AMD64_PTE_PRESENT) ||
			    (parent_entry & AMD64_PTE_ADDR_MASK) != expected) {
				HAL_FATAL("detaching an unlinked amd64 page table");
			}

			/* Disconnects and transfers the empty table to the result list. */
			page->parent[page->parent_index] = 0;
			*link = page->next;
			page->next = detached;
			detached = page;
			reclaimed = 1;
		}
	} while (reclaimed);

	/* Reports tables safe to release after a complete shootdown. */
	return detached;
}

/* Releases a list of detached subordinate page tables. */
static void
free_detached_tables(
	struct amd64_table_page *page)
{
	struct amd64_table_page *next;

	/* Releases every detached table and its ownership record. */
	while (page != NULL) {
		next = page->next;
		(void)hal_pmem_free(&page->memory);
		hal_free(page);
		table_count_drop();
		page = next;
	}
}

/* Builds a hardware leaf permission mask. */
static uint64_t
leaf_flags(
	uint32_t attr)
{
	uint64_t flags;

	/* Starts with the permissions common to every user mapping. */
	flags = AMD64_PTE_PRESENT | AMD64_PTE_USER;

	/* Enables writes when requested. */
	if (attr & HAL_SPACE_WRITE)
		flags |= AMD64_PTE_WRITE;

	/* Disables caching when requested. */
	if (attr & HAL_SPACE_NOCACHE)
		flags |= AMD64_PTE_NOCACHE;

	/* Enables write-through caching when requested. */
	if (attr & HAL_SPACE_WRITETHRU)
		flags |= AMD64_PTE_WRITETHRU;

	/* Treats device mappings as uncached. */
	if (attr & HAL_SPACE_DEVICE)
		flags |= AMD64_PTE_NOCACHE;

	/* Disables execution unless explicitly requested. */
	if (!(attr & HAL_SPACE_EXEC))
		flags |= AMD64_PTE_NX;

	/* Reports the completed hardware flags. */
	return flags;
}

/* Invalidates matching translations on every ready CPU. */
static void
shootdown(
	hal_space_t handle,
	void *vaddr,
	size_t size)
{
	struct amd64_shootdown_request *request;
	struct amd64_percpu *target;
	struct hal_cpu_mask ready;
	hal_cpu_id_t sender;
	hal_cpu_id_t cpu;
	hal_space_t current_space;
	uint32_t apic_id;
	unsigned slot;
	unsigned expected;
	uint64_t pending;
	int reserved;
	int status;

	/* Captures the sending CPU and initializes request construction. */
	sender = hal_cpu_current();
	request = NULL;
	pending = 0;

	/*
	 * Reserves a pool entry rather than indexing by sender CPU.  A timer
	 * interrupt can preempt a waiting thread and run another thread on the
	 * same CPU.
	 */
	for (slot = 0; slot < AMD64_SHOOTDOWN_REQUESTS; slot++) {
		expected = 0;
		reserved = __atomic_compare_exchange_n(
			&shootdowns[slot].active,
			&expected,
			2U,
			0,
			__ATOMIC_ACQUIRE,
			__ATOMIC_RELAXED);

		/* Selects the first request slot reserved by this CPU. */
		if (reserved) {
			request = &shootdowns[slot];
			break;
		}
	}

	/* Rejects exhaustion rather than losing an invalidation. */
	if (request == NULL)
		HAL_FATAL("amd64 TLB shootdown request pool exhausted");

	/* Selects every ready remote CPU using the affected address space. */
	hal_cpu_ready_mask(&ready);
	for (cpu = 0; cpu < hal_cpu_count(); cpu++) {
		/* Skips the sender, out-of-range CPUs, and CPUs not yet ready. */
		if (cpu == sender || cpu >= AMD64_SMP_MAX_CPUS ||
		    !hal_cpu_mask_test(&ready, cpu)) {
			continue;
		}

		/* Resolves the ready remote processor's current-space record. */
		target = amd64_percpu_get(cpu);

		/* Selects every remote CPU for a system-space flush. */
		if (handle == HAL_SPACE_SYS) {
			pending |= (uint64_t)1U << cpu;
			continue;
		}

		/* Observes the remote processor's current address space. */
		current_space = __atomic_load_n(
		    &target->current_space,
		    __ATOMIC_ACQUIRE);

		/* Selects a remote CPU currently using this user space. */
		if (current_space == handle)
			pending |= (uint64_t)1U << cpu;
	}

	/* Publishes request contents before making the slot active. */
	request->space = handle;
	request->vaddr = vaddr;
	request->size = size;
	__atomic_store_n(&request->pending, pending, __ATOMIC_RELEASE);
	__atomic_store_n(&request->active, 1U, __ATOMIC_RELEASE);

	/* Delivers the shootdown interrupt to every selected remote CPU. */
	for (cpu = 0; cpu < hal_cpu_count(); cpu++) {
		/* Skips CPUs absent from this request. */
		if ((pending & ((uint64_t)1U << cpu)) == 0)
			continue;

		/* Resolves and signals the target processor. */
		apic_id = amd64_smp_apic_id(cpu);
		status = amd64_lapic_send_vector(
			apic_id,
			AMD64_VECTOR_TLB);

		/* Rejects a delivery failure which could leave stale translations. */
		if (status != HAL_OK)
			HAL_FATAL("amd64 TLB shootdown delivery failed");
	}

	/* Invalidates the sender when it currently uses the affected space. */
	if (handle == HAL_SPACE_SYS || AMD64_CURRENT_SPACE == handle)
		asm_flush_tlb();

	/*
	 * Waits for acknowledgements while servicing reciprocal requests.  Page
	 * operations may enter with local interrupts disabled, so polling avoids
	 * a reciprocal-shootdown deadlock without enabling arbitrary handlers.
	 */
	while (__atomic_load_n(&request->pending, __ATOMIC_ACQUIRE) != 0) {
		service_shootdowns(sender);
		__asm__ volatile("pause");
	}

	/* Releases the acknowledged request slot. */
	__atomic_store_n(&request->active, 0U, __ATOMIC_RELEASE);
}

/* Services all active shootdown requests targeting one CPU. */
static void
service_shootdowns(
	hal_cpu_id_t cpu)
{
	struct amd64_shootdown_request *request;
	uint64_t bit;
	uint64_t pending;
	unsigned active;
	unsigned slot;

	/* Selects this CPU's bit in every pending request mask. */
	bit = (uint64_t)1U << cpu;

	/* Examines every request which could target this CPU. */
	for (slot = 0; slot < AMD64_SHOOTDOWN_REQUESTS; slot++) {
		request = &shootdowns[slot];
		active = __atomic_load_n(&request->active, __ATOMIC_ACQUIRE);

		/* Skips slots not yet published as active. */
		if (active != 1U)
			continue;

		/* Observes the processors still awaiting acknowledgement. */
		pending = __atomic_load_n(&request->pending, __ATOMIC_ACQUIRE);

		/* Skips requests which do not target this CPU. */
		if ((pending & bit) == 0)
			continue;

		/* Flushes locally before acknowledging the request. */
		asm_flush_tlb();
		(void)__atomic_fetch_and(
		    &request->pending,
		    ~bit,
		    __ATOMIC_RELEASE);
	}
}
