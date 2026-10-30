#include <hal/hal.h>
#include "asm.h"
#include "bsp.h"
#include "defs.h"
#include "space.h"
#include "percpu.h"
#include "spin.h"
#include "bsp-rpi-common/framebuffer.h"

#define ARM64_USER_LIMIT 0x0001000000000000ULL
_Static_assert(ARM64_USER_LIMIT - 1U <= (uintptr_t)INTPTR_MAX,
    "user pointers must not overlap the negative syscall errno window");

#define PTE_VALID (1ULL << 0)
#define PTE_TABLE (1ULL << 1)
#define PTE_ATTR(n) ((uint64_t)(n) << 2)
#define PTE_USER (1ULL << 6)
#define PTE_RO (1ULL << 7)
#define PTE_SH_INNER (3ULL << 8)
#define PTE_AF (1ULL << 10)
#define PTE_SW_DIRTY (1ULL << 55)
#define PTE_PXN (1ULL << 53)
#define PTE_UXN (1ULL << 54)
#define PTE_ADDR 0x0000fffffffff000ULL
#define BLOCK_FLAGS (PTE_VALID | PTE_AF | PTE_SH_INNER)
#define PAGE_FLAGS (PTE_VALID | PTE_TABLE | PTE_AF | PTE_SH_INNER)

static uint64_t system_user_l0[512] __attribute__((aligned(ARM64_PAGE_SIZE)));
static uint64_t system_kernel_l0[512] __attribute__((aligned(ARM64_PAGE_SIZE)));
static uint64_t system_kernel_l1[512] __attribute__((aligned(ARM64_PAGE_SIZE)));
static uint64_t system_kernel_l2_low[512] __attribute__((aligned(ARM64_PAGE_SIZE)));
static uint64_t system_kernel_l3_low[512] __attribute__((aligned(ARM64_PAGE_SIZE)));
static uint64_t system_kernel_l2_high[512] __attribute__((aligned(ARM64_PAGE_SIZE)));
static uintptr_t system_ttbr0;

/*
 * Every user space that exists, the number the next one gets, and how many
 * there are.  Kept under space_registry_lock; a space is looked up here
 * before its own lock is taken, so a destroyed space is never locked.
 */
static int next_space_id = 1;
static uint32_t space_count;
static struct arm64_space *space_registry;

/* Guards space_registry, next_space_id and space_count; zero when free. */
static volatile unsigned space_registry_lock;

/*
 * How many page-table pages the user spaces and the kernel's extra tables
 * hold, for the memory statistics.  Changed with atomic adds, since each
 * space's tables are changed under that space's own lock.
 */
static uint32_t page_table_count;

/*
 * Guards the kernel's own tables after boot: the device blocks of the
 * direct map and the uncached window.  Zero when free.
 */
static volatile unsigned kernel_table_lock;

/* The size of one level-2 block of the kernel's direct map. */
#define ARM64_DEVICE_BLOCK_SIZE 0x200000ULL

/* The physical span one level-1 entry of the direct map covers. */
#define ARM64_DIRECT_L1_SPAN 0x40000000ULL

/* The physical span the direct map's level-1 table can reach. */
#define ARM64_DIRECT_LIMIT (512ULL * ARM64_DIRECT_L1_SPAN)

/* The system half's level-0 entry that holds the uncached window. */
#define ARM64_UNCACHED_L0_INDEX 1U

/* Where the uncached window starts and how large it is (one level-0 entry). */
#define ARM64_UNCACHED_BASE (ARM64_DIRECT_BASE + ((uint64_t)ARM64_UNCACHED_L0_INDEX << 39))
#define ARM64_UNCACHED_SIZE (1ULL << 39)

/* The MAIR index locore.S gives Normal non-cacheable memory. */
#define ARM64_ATTR_NORMAL_NC 3U

/*
 * The first unused address of the uncached window.
 *
 * Views are handed out upward and their addresses are not reused: DMA
 * buffers are taken at attach and returned at detach, which never comes near
 * the window's 512 GiB.  Zero means the window is not yet in use.  Changed
 * only under kernel_table_lock.
 */
static uint64_t uncached_next;

/* The system half's level-0 entry that holds the early console's window of the framebuffer. */
#define ARM64_FB_WINDOW_L0_INDEX 2U

/* Where the window starts, and the most it reaches: one level-2 table's gigabyte. */
#define ARM64_FB_WINDOW_BASE (ARM64_DIRECT_BASE + ((uint64_t)ARM64_FB_WINDOW_L0_INDEX << 39))
#define ARM64_FB_WINDOW_SPAN ARM64_DIRECT_L1_SPAN

/*
 * The states of the early console's window of the framebuffer: none (no
 * framebuffer, or one the window cannot reach), held by the early console,
 * given to the kernel's console (one mapping request of exactly the
 * framebuffer once the kernel prints), and removed.
 */
enum arm64_fb_window_state {
	ARM64_FB_WINDOW_NONE,
	ARM64_FB_WINDOW_HELD,
	ARM64_FB_WINDOW_GIVEN,
	ARM64_FB_WINDOW_REMOVED
};

/*
 * The early console's window of the framebuffer: its state, the
 * framebuffer's physical base and size as the kernel reads them, the
 * kernel address of its first byte, and whether the window is cached
 * memory (a framebuffer in RAM) rather than device memory.
 */
struct arm64_fb_window {
	enum arm64_fb_window_state state;
	uint64_t physical;
	uint64_t size;
	uintptr_t address;
	int cached;
};

/*
 * The window's tables: the level-1 table of its level-0 entry, the
 * level-2 table of its gigabyte, and the level-3 tables of the two 2 MiB
 * blocks at the framebuffer's ends that it shares with other memory, so
 * that the window reaches the framebuffer's pages and no others.  Filled
 * by arm64_space_init(), and unlinked from the level-0 entry when the
 * window is removed.
 */
static uint64_t fb_window_l1[512] __attribute__((aligned(ARM64_PAGE_SIZE)));
static uint64_t fb_window_l2[512] __attribute__((aligned(ARM64_PAGE_SIZE)));
static uint64_t fb_window_l3[2][512] __attribute__((aligned(ARM64_PAGE_SIZE)));

/*
 * The early console's window of the framebuffer.  Set while the kernel's
 * tables are built; its state changes under kernel_table_lock.
 */
static struct arm64_fb_window fb_window;

static void fb_window_build(void);
static void fb_window_device_direct(uint64_t physical, uint64_t size);
static void fb_window_fill(uint64_t physical, uint64_t size, uint64_t flags);
static int fb_window_give(hal_physaddr_t physical, size_t size, uint32_t attr, void **address);
static int fb_window_remove(void *address, size_t size, int *matched);
static bool range_touches_ram(uint64_t physical, uint64_t size);
static int uncached_leaf(uint64_t address, bool create, uint64_t **leaf);
static int uncached_table(uint64_t *table, unsigned index, bool create, uint64_t **next);
static bool range_is_ram(uint64_t physical, uint64_t size);
static int device_block_table(unsigned l1_index, uint64_t **table);
static void map_one_device_block(uint64_t *table, unsigned l2_index, uint64_t physical);
static bool space_active_anywhere(hal_space_t handle);

/* Allocates one zero-owner physical page for page-table use. */
static int alloc_page(hal_physaddr_t *memory)
{
	return hal_pmem_alloc(ARM64_PAGE_SIZE, ARM64_PAGE_SIZE, memory);
}
extern char __kernel_text_start[],__kernel_text_end[];
extern char __kernel_rodata_start[],__kernel_rodata_end[];
extern char __kernel_data_start[],__kernel_data_end[];

uintptr_t arm64_direct_to_phys(const void *p) { return (uintptr_t)p-ARM64_DIRECT_BASE; }
void *arm64_phys_to_direct(uintptr_t p) { return (void *)(ARM64_DIRECT_BASE+p); }

static uint64_t table_desc(const void *table)
{ return arm64_direct_to_phys(table)|PTE_VALID|PTE_TABLE; }

/*
 * Maps the 2 MiB block that holds a peripheral as Device-nGnRE memory, in
 * the 3-4 GiB window of the tables arm64_space_init() builds.
 *
 * Only a board whose peripherals lie in that window calls it, from
 * arm64_bsp_map_devices(); an address outside the window is left alone.
 */
void
arm64_space_map_device_block(
	uint64_t physical)
{
	unsigned index;

	/* An address outside the window has no entry here. */
	if (physical < 0xc0000000ULL || physical >= 0x100000000ULL)
		return;

	/* Replaces the window's 2 MiB block with a device block, never executable. */
	index = (unsigned)((physical - 0xc0000000ULL) >> 21);
	system_kernel_l2_high[index] = (physical & ~0x1fffffULL) | BLOCK_FLAGS | PTE_ATTR(1) | PTE_PXN | PTE_UXN;
}

/*
 * Builds the kernel's final tables and switches the boot CPU to them.
 *
 * TTBR1 maps all RAM through the direct map, the first 2 MiB of RAM page
 * by page with W^X for the kernel's sections there, and the firmware's
 * peripheral blocks as device memory; TTBR0 gets an empty user table.  The secondary CPUs load
 * the same TTBR1 when they come up (smp.c).
 */
void
arm64_space_init(
	void)
{
	uint64_t total;
	uint64_t physical;
	uint64_t flags;
	uintptr_t text_start;
	uintptr_t text_end;
	uintptr_t rodata_start;
	uintptr_t rodata_end;
	uintptr_t data_start;
	unsigned index;
	int text_executable;
	int rodata_protected;
	int data_protected;

	/* Locates the kernel image's sections physically. */
	total = hal_pmem_get_total_size();
	text_start = arm64_direct_to_phys(__kernel_text_start);
	text_end = arm64_direct_to_phys(__kernel_text_end);
	rodata_start = arm64_direct_to_phys(__kernel_rodata_start);
	rodata_end = arm64_direct_to_phys(__kernel_rodata_end);
	data_start = arm64_direct_to_phys(__kernel_data_start);

	/* Starts from no user space and empty tables. */
	space_registry = NULL;
	hal_memset(system_user_l0, 0, sizeof(system_user_l0));
	hal_memset(system_kernel_l0, 0, sizeof(system_kernel_l0));
	hal_memset(system_kernel_l1, 0, sizeof(system_kernel_l1));
	hal_memset(system_kernel_l2_low, 0, sizeof(system_kernel_l2_low));
	hal_memset(system_kernel_l3_low, 0, sizeof(system_kernel_l3_low));
	hal_memset(system_kernel_l2_high, 0, sizeof(system_kernel_l2_high));

	/* Maps RAM in 1 GiB blocks, never executable. */
	system_kernel_l0[0] = table_desc(system_kernel_l1);
	for (index = 0; index < 512U; index++) {
		physical = (uint64_t)index << 30;
		if (physical >= total)
			break;
		system_kernel_l1[index] = physical | BLOCK_FLAGS | PTE_PXN | PTE_UXN;
	}

	/*
	 * Does not turn the complete 3--4 GiB aperture into Device memory: a
	 * 4 GiB Pi has ordinary RAM there.  Splits that L1 entry and replaces
	 * only the FDT-discovered peripheral blocks with Device-nGnRE mappings.
	 */
	system_kernel_l1[3] = table_desc(system_kernel_l2_high);
	for (index = 0; index < 512U; index++) {
		physical = 0xc0000000ULL + ((uint64_t)index << 21);
		if (physical < total)
			system_kernel_l2_high[index] = physical | BLOCK_FLAGS | PTE_PXN | PTE_UXN;
	}

	/* Maps the peripherals the HAL itself uses, where the board has them. */
	arm64_bsp_map_devices();

	/*
	 * Splits the first GiB into 2 MiB blocks, leaving out the blocks of
	 * memory the device tree says may not be mapped (the secure
	 * monitor's), so that no speculative access reaches them.
	 */
	system_kernel_l1[0] = table_desc(system_kernel_l2_low);
	for (index = 0; index < 512U; index++) {
		physical = (uint64_t)index << 21;
		if (index != 0 && rpi_boot_is_no_map(physical, (uint64_t)1 << 21)) {
			system_kernel_l2_low[index] = 0;
			continue;
		}
		system_kernel_l2_low[index] = physical | BLOCK_FLAGS | PTE_PXN | PTE_UXN;
	}

	/* Maps the first 2 MiB of RAM page by page: text read-only and executable, rodata read-only. */
	system_kernel_l2_low[0] = table_desc(system_kernel_l3_low);
	for (index = 0; index < 512U; index++) {
		physical = (uint64_t)index * ARM64_PAGE_SIZE;
		flags = PAGE_FLAGS | PTE_PXN | PTE_UXN;
		if (physical >= text_start && physical < text_end) {
			flags = (flags | PTE_RO) & ~PTE_PXN;
		} else if (physical >= rodata_start && physical < rodata_end) {
			flags |= PTE_RO;
		}
		system_kernel_l3_low[index] = physical | flags;
	}

	/* Gives the early console its own window of the framebuffer, which the kernel's console can remove. */
	fb_window_build();

	/* Switches the boot CPU to the final tables, with the kernel alone selected. */
	system_ttbr0 = arm64_direct_to_phys(system_user_l0);
	arm64_write_ttbr1(arm64_direct_to_phys(system_kernel_l0));
	arm64_write_ttbr0(system_ttbr0);
	arm64_flush_tlb();
	arm64_percpu_current()->current_space = HAL_SPACE_SYS;

	/* The early console paints the framebuffer through its window from now on. */
	if (fb_window.state == ARM64_FB_WINDOW_HELD)
		rpi_framebuffer_move((volatile uint8_t *)fb_window.address, fb_window.cached);

	/* Checks W^X on the three sections the kernel runs from. */
	text_executable = (system_kernel_l3_low[(text_start >> 12) & 511U] & PTE_PXN) == 0;
	rodata_protected = (system_kernel_l3_low[(rodata_start >> 12) & 511U] & PTE_PXN) != 0;
	data_protected = (system_kernel_l3_low[(data_start >> 12) & 511U] & PTE_PXN) != 0;
	if (!text_executable || !rodata_protected || !data_protected)
		HAL_FATAL("arm64 kernel W^X table validation failed");
	hal_puts("ARM64 PAGING PASS\nARM64 W^X PASS\n");
}

/*
 * Finds a live user space by its handle and takes its lock.
 *
 * The space is looked up under the registry lock, so a space being
 * destroyed is never locked.  Interrupts stay off until space_unlock();
 * *irq_enabled tells it whether to turn them back on.  Reports 0 for a
 * handle that names no live space.
 */
static int
space_lock_handle(
	hal_space_t handle,
	struct arm64_space **result,
	bool *irq_enabled)
{
	struct arm64_space *space;
	unsigned me;
	bool enabled;
	bool acquired;

	/* Looks the handle up among the live spaces. */
	enabled = arm64_spin_lock(&space_registry_lock);
	for (space = space_registry; space != NULL; space = space->registry_next) {
		if ((hal_space_t)space == handle)
			break;
	}

	/* A handle that names no live space is refused. */
	if (space == NULL || space->destroying) {
		arm64_spin_unlock(&space_registry_lock, enabled);
		return 0;
	}

	/* A second operation on the same space from the same CPU is a fault. */
	me = hal_cpu_current() + 1U;
	if (space->lock_owner == me)
		HAL_FATAL("recursive arm64 space operation");

	/* Waits for another CPU's operation on the space to end. */
	for (;;) {
		acquired = hal_atomic_uint_try_acquire(&space->lock);
		if (acquired)
			break;
		hal_atomic_relax();
	}
	space->lock_owner = me;

	/* Leaves the registry with interrupts still off for the space's operation. */
	__atomic_store_n(&space_registry_lock, 0U, __ATOMIC_RELEASE);

	/* Succeeded: the caller holds the space. */
	*result = space;
	*irq_enabled = enabled;
	return 1;
}

/*
 * Releases a space taken by space_lock_handle() and restores interrupts.
 */
static void
space_unlock(
	struct arm64_space *space,
	bool enabled)
{
	unsigned me;

	/* Only the CPU that holds the space may release it. */
	me = hal_cpu_current() + 1U;
	if (space->lock == 0 || space->lock_owner != me)
		HAL_FATAL("invalid arm64 space unlock");

	/* Releases the space, publishing every change made under it. */
	space->lock_owner = 0;
	arm64_spin_unlock(&space->lock, enabled);
}

/*
 * Drops the stale translations of a space after its tables changed.
 *
 * The kernel's tables are used by every CPU, and a user space's by every
 * CPU that has it selected; both are flushed on every CPU at once.  A
 * user space selected nowhere has nothing cached to drop, since a CPU
 * flushes its TLB whenever it selects a space.
 */
static void
flush_locked(
	hal_space_t handle)
{
	bool active;

	/* The kernel's tables are cached on every CPU. */
	if (handle == HAL_SPACE_SYS) {
		arm64_flush_tlb();
		return;
	}

	/* A user space is flushed where it is selected. */
	active = space_active_anywhere(handle);
	if (active)
		arm64_flush_tlb();
}

/*
 * Reports whether any CPU has a space selected.
 *
 * The caller has just changed the space's tables; the full barrier
 * orders those stores before the reads, so a CPU that selects the space
 * after the reads walks the new tables.
 */
static bool
space_active_anywhere(
	hal_space_t handle)
{
	struct arm64_percpu *cpu;
	hal_space_t selected;
	hal_cpu_id_t index;

	/* Orders the table stores before the reads of the selections. */
	hal_mb();

	/* Looks at every CPU's selection. */
	for (index = 0; index < ARM64_CPU_MAX; index++) {
		cpu = arm64_percpu_get(index);
		selected = __atomic_load_n(&cpu->current_space, __ATOMIC_ACQUIRE);
		if (selected == handle)
			return true;
	}

	/* No CPU has the space selected. */
	return false;
}

/* Reports whether a range is page-aligned, non-empty and inside the user half. */
static int
valid_user(
	uintptr_t address,
	size_t size)
{
	/* An empty or unaligned range is refused. */
	if (size == 0)
		return 0;
	if ((address & 4095U) != 0)
		return 0;
	if ((size & 4095U) != 0)
		return 0;

	/* The first page is never mapped, and the range must end inside the user half. */
	if (address < 4096U)
		return 0;
	if (address >= ARM64_USER_LIMIT)
		return 0;
	if (size > ARM64_USER_LIMIT - address)
		return 0;

	/* Succeeded: the range can be mapped. */
	return 1;
}

/* Allocates a zeroed page-table page and records it as the space's. */
static struct arm64_table_page *
allocate_table(
	struct arm64_space *space,
	uint64_t *parent,
	unsigned index)
{
	struct arm64_table_page *page;
	int error;

	/* Allocates the record of the table. */
	page = kernel_alloc(sizeof(*page));
	if (page == NULL)
		return NULL;

	/* Allocates the table itself. */
	error = alloc_page(&page->memory);
	if (error != HAL_OK) {
		kernel_free(page);
		return NULL;
	}

	/* Clears the table and remembers the entry that will point at it. */
	hal_memset(hal_pmem_to_kernel(page->memory), 0, ARM64_PAGE_SIZE);
	page->parent = parent;
	page->parent_index = index;

	/* Lists the table with the space's and counts it. */
	page->next = space->tables;
	space->tables = page;
	(void)__atomic_fetch_add(&page_table_count, 1U, __ATOMIC_RELAXED);

	/* Succeeded: the table, not yet linked into the tree. */
	return page;
}

/*
 * Finds the level-3 entry of a user address, making the tables on the way
 * when asked to.  Reports NULL when a table is missing and not to be made,
 * cannot be made, or a block sits where a table should be.
 */
static uint64_t *
walk_leaf(
	struct arm64_space *space,
	uintptr_t address,
	int create)
{
	static const unsigned shifts[3] = {39, 30, 21};
	struct arm64_table_page *page;
	uint64_t *table;
	uint64_t entry;
	unsigned level;
	unsigned index;

	/* Descends from level 0 to the level-3 table. */
	table = space->l0;
	for (level = 0; level < 3; level++) {
		index = (unsigned)(address >> shifts[level]) & 511U;
		entry = table[index];

		/* A missing table is made only when asked to. */
		if ((entry & PTE_VALID) == 0) {
			if (!create)
				return NULL;
			page = allocate_table(space, table, index);
			if (page == NULL)
				return NULL;
			entry = (uintptr_t)page->memory | PTE_VALID | PTE_TABLE;
			table[index] = entry;
		}

		/* Anything but a table entry ends the walk. */
		if ((entry & (PTE_VALID | PTE_TABLE)) != (PTE_VALID | PTE_TABLE))
			return NULL;
		table = arm64_phys_to_direct((uintptr_t)(entry & PTE_ADDR));
	}

	/* Succeeded: the entry of the page. */
	return &table[(address >> 12) & 511U];
}

/* Reports whether a table has no valid entry. */
static int
table_empty(
	const uint64_t *table)
{
	unsigned index;

	/* Looks for any valid entry. */
	for (index = 0; index < 512U; index++) {
		if ((table[index] & PTE_VALID) != 0)
			return 0;
	}

	/* The table is empty. */
	return 1;
}

/*
 * Unlinks every empty table of a space, repeating until none is left
 * whose children all went, and reports them for freeing after the flush.
 */
static struct arm64_table_page *
detach_empty_tables(
	struct arm64_space *space)
{
	struct arm64_table_page *detached;
	struct arm64_table_page **link;
	struct arm64_table_page *page;
	uint64_t parent_entry;
	int again;
	int empty;

	/* Passes over the tables until a pass detaches nothing. */
	detached = NULL;
	do {
		again = 0;
		link = &space->tables;
		while (*link != NULL) {
			page = *link;

			/* A table that still maps something stays. */
			empty = table_empty(hal_pmem_to_kernel(page->memory));
			if (!empty) {
				link = &page->next;
				continue;
			}

			/* The parent entry must point at this very table. */
			parent_entry = page->parent[page->parent_index];
			if ((parent_entry & PTE_VALID) == 0 ||
			    (parent_entry & PTE_ADDR) != (uintptr_t)page->memory)
				HAL_FATAL("detaching an unlinked arm64 page table");

			/* Unlinks the table from its parent and from the space's list. */
			page->parent[page->parent_index] = 0;
			*link = page->next;
			page->next = detached;
			detached = page;
			again = 1;
		}
	} while (again);

	/* Succeeded: the tables to free once no walker can reach them. */
	return detached;
}

/* Frees tables detached by detach_empty_tables(). */
static void
free_detached_tables(
	struct arm64_table_page *page)
{
	struct arm64_table_page *next;

	/* Frees each table and its record, and stops counting it. */
	while (page != NULL) {
		next = page->next;
		(void)hal_pmem_free(&page->memory, ARM64_PAGE_SIZE);
		kernel_free(page);
		(void)__atomic_fetch_sub(&page_table_count, 1U, __ATOMIC_RELAXED);
		page = next;
	}
}

/*
 * Creates an empty user space.
 */
hal_space_t
hal_space_create(
	void)
{
	struct arm64_space *space;
	bool enabled;
	int error;

	/* Allocates the record. */
	space = kernel_alloc(sizeof(*space));
	if (space == NULL)
		return NULL;
	hal_memset(space, 0, sizeof(*space));

	/* Allocates and clears the level-0 table. */
	error = alloc_page(&space->l0_memory);
	if (error != HAL_OK) {
		kernel_free(space);
		return NULL;
	}
	space->l0 = hal_pmem_to_kernel(space->l0_memory);
	hal_memset(space->l0, 0, ARM64_PAGE_SIZE);
	space->magic = ARM64_SPACE_MAGIC;

	/* Numbers the space and makes it live. */
	enabled = arm64_spin_lock(&space_registry_lock);

	space->space_id = next_space_id++;
	space->registry_next = space_registry;
	space_registry = space;
	space_count++;

	arm64_spin_unlock(&space_registry_lock, enabled);

	/* Succeeded: the space, with nothing mapped. */
	return space;
}

/*
 * Destroys a user space that no CPU has selected and no operation holds.
 */
void
hal_space_destroy(
	hal_space_t handle)
{
	struct arm64_space *space;
	struct arm64_space **link;
	struct arm64_table_page *page;
	bool enabled;
	bool active;

	/* A missing space is nothing to destroy. */
	space = handle;
	if (space == NULL)
		return;

	/* Takes the space out of the registry, so nothing can lock it any more. */
	enabled = arm64_spin_lock(&space_registry_lock);

	link = &space_registry;
	while (*link != NULL && *link != space)
		link = &(*link)->registry_next;
	if (*link == NULL || space->destroying)
		HAL_FATAL("invalid arm64 space destroy");
	active = space_active_anywhere(space);
	if (active)
		HAL_FATAL("destroying an active arm64 space");
	if (space->lock != 0)
		HAL_FATAL("destroying a busy arm64 space");
	space->destroying = 1U;
	*link = space->registry_next;
	if (space_count != 0)
		space_count--;

	arm64_spin_unlock(&space_registry_lock, enabled);

	/* Frees every table, then the level-0 table and the record. */
	while (space->tables != NULL) {
		page = space->tables;
		space->tables = page->next;
		(void)hal_pmem_free(&page->memory, ARM64_PAGE_SIZE);
		kernel_free(page);
		(void)__atomic_fetch_sub(&page_table_count, 1U, __ATOMIC_RELAXED);
	}
	space->magic = 0;
	(void)hal_pmem_free(&space->l0_memory, ARM64_PAGE_SIZE);
	kernel_free(space);
}

/*
 * Selects a user space, or the kernel alone, on the calling CPU.
 *
 * The selection is published before TTBR0 changes, so a CPU that changes
 * the space's tables afterwards sees it and flushes this CPU too.  Only
 * this CPU's TLB is emptied: without ASIDs, what it cached of the space
 * it leaves must go, and no other CPU holds entries for its selection.
 */
void
hal_space_switch(
	hal_space_t handle)
{
	struct arm64_percpu *cpu;
	struct arm64_space *space;
	uintptr_t ttbr;
	bool enabled;
	bool space_enabled;
	int locked;

	/*
	 * Names the CPU with interrupts off, so that a caller outside a task
	 * switch (an exec) cannot move to another CPU between reading the
	 * record and installing the table, which would publish the selection
	 * in the old CPU's record.
	 */
	enabled = hal_irq_disable();
	cpu = arm64_percpu_current();

	/* The space already selected here needs nothing. */
	if (handle == cpu->current_space) {
		if (enabled)
			hal_irq_enable();
		return;
	}

	/* The kernel alone: the empty user table. */
	if (handle == HAL_SPACE_SYS) {
		__atomic_store_n(&cpu->current_space, handle, __ATOMIC_RELEASE);
		hal_mb();
		arm64_write_ttbr0(system_ttbr0);
		arm64_flush_tlb_local();
		if (enabled)
			hal_irq_enable();
		return;
	}

	/* A user space is held while its table is installed. */
	locked = space_lock_handle(handle, &space, &space_enabled);
	if (!locked)
		HAL_FATAL("invalid arm64 space switch");

	__atomic_store_n(&cpu->current_space, handle, __ATOMIC_RELEASE);
	hal_mb();
	ttbr = (uintptr_t)space->l0_memory;
	arm64_write_ttbr0(ttbr);
	arm64_flush_tlb_local();

	space_unlock(space, space_enabled);

	/* Restores the caller's interrupt state. */
	if (enabled)
		hal_irq_enable();
}

/*
 * Reports the physical address of the kernel's level-0 table, which every
 * CPU loads into TTBR1 once arm64_space_init() has built it.
 */
uintptr_t
arm64_space_kernel_ttbr1(
	void)
{
	uintptr_t physical;

	/* The table lives in the kernel image, inside the direct map. */
	physical = arm64_direct_to_phys(system_kernel_l0);

	/* Succeeded: the table's physical address. */
	return physical;
}

/*
 * Makes a secondary CPU select the kernel alone, as the boot CPU does
 * after arm64_space_init(); it arrives with the boot identity map.
 */
void
arm64_space_select_system(
	void)
{
	struct arm64_percpu *cpu;

	/* Publishes the selection, then installs the empty user table. */
	cpu = arm64_percpu_current();
	__atomic_store_n(&cpu->current_space, HAL_SPACE_SYS, __ATOMIC_RELEASE);
	hal_mb();
	arm64_write_ttbr0(system_ttbr0);
	arm64_flush_tlb_local();
}

/*
 * Makes code written through the data side visible to instruction fetch
 * before a user page becomes executable.  The Cortex-A72's instruction
 * cache does not snoop its data cache, so a page filled by ordinary stores
 * (a file read into memory, or code patched before mprotect) must be
 * cleaned to the point of unification and dropped from the instruction
 * cache, or the CPU fetches stale bytes.
 */
static void
sync_executable(
	hal_physaddr_t physical,
	size_t size,
	uint32_t attr)
{
	/* Only cacheable memory that is to be executed. */
	if ((attr & HAL_SPACE_EXEC) == 0)
		return;
	if ((attr & (HAL_SPACE_DEVICE | HAL_SPACE_NOCACHE)) != 0)
		return;

	/* Its bytes through the kernel's direct map. */
	hal_sync_instruction_stream(arm64_phys_to_direct((uintptr_t)physical), size);
}

static uint64_t leaf_flags(uint32_t attr)
{
	uint64_t f=PAGE_FLAGS|PTE_USER|PTE_PXN;
	if(!(attr&HAL_SPACE_WRITE))f|=PTE_RO;else f|=PTE_SW_DIRTY;
	if(!(attr&HAL_SPACE_EXEC))f|=PTE_UXN;
	/*
	 * Uncached user pages are managed RAM a device reads without snooping: they are
	 * Normal non-cacheable, as the kernel's view of the same RAM is, so that user code
	 * may access them at any alignment; Device memory would fault those accesses.
	 */
	if(attr&HAL_SPACE_DEVICE)f|=PTE_ATTR(1);else if(attr&HAL_SPACE_NOCACHE)f|=PTE_ATTR(ARM64_ATTR_NORMAL_NC);
	return f;
}
int hal_space_map(hal_space_t h,void *v,hal_physaddr_t p,size_t n,uint32_t attr)
{
	struct arm64_space *s=h;uintptr_t a=(uintptr_t)v,o;bool enabled;
	if(!s||!valid_user(a,n)||(p&4095)||p>=hal_pmem_get_total_size()||n>hal_pmem_get_total_size()-p||
	   !(attr&(HAL_SPACE_READ|HAL_SPACE_WRITE|HAL_SPACE_EXEC))||((attr&HAL_SPACE_WRITE)&&(attr&HAL_SPACE_EXEC)))return HAL_ERR_INVALID;
	if(!space_lock_handle(h,&s,&enabled))return HAL_ERR_STATE;
	for(o=0;o<n;o+=4096){uint64_t *l=walk_leaf(s,a+o,0);if(l&&(*l&PTE_VALID)){space_unlock(s,enabled);return HAL_ERR_INVALID;}}
	/* Code in the pages is fetched correctly once they are executable. */
	sync_executable(p,n,attr);
	for(o=0;o<n;o+=4096){uint64_t *l=walk_leaf(s,a+o,1);if(!l){struct arm64_table_page *detached;uintptr_t rollback;for(rollback=0;rollback<o;rollback+=4096){l=walk_leaf(s,a+rollback,0);if(l)*l=0;}detached=detach_empty_tables(s);hal_wmb();flush_locked(s);free_detached_tables(detached);space_unlock(s,enabled);return HAL_ERR_NOMEM;}*l=(p+o)|leaf_flags(attr);}
	flush_locked(s);space_unlock(s,enabled);return HAL_OK;
}
int hal_space_prot_query(hal_space_t h,void *v,size_t n,uint32_t attr,uint32_t *flags)
{
	struct arm64_space *s=h;uintptr_t a=(uintptr_t)v,o;uint32_t observed=0;bool enabled;if(!s||!valid_user(a,n)||
	 !(attr&(HAL_SPACE_READ|HAL_SPACE_WRITE|HAL_SPACE_EXEC))||((attr&HAL_SPACE_WRITE)&&(attr&HAL_SPACE_EXEC)))return HAL_ERR_INVALID;
	if(!space_lock_handle(h,&s,&enabled))return HAL_ERR_STATE;
	for(o=0;o<n;o+=4096){uint64_t *l=walk_leaf(s,a+o,0);if(!l||!(*l&PTE_VALID)){space_unlock(s,enabled);return HAL_ERR_INVALID;}}
	/* Use break-before-make for the complete range.  This also covers callers
	 * which change the AttrIndx, not merely the permission bits. */
	for(o=0;o<n;o+=4096){uint64_t *l=walk_leaf(s,a+o,0),old=*l;observed|=HAL_SPACE_PAGE_PRESENT;if(old&PTE_AF)observed|=HAL_SPACE_PAGE_ACCESSED;
		/* This profile has no hardware dirty management.  Any writable
		 * translation is conservatively dirty, including stores made before
		 * the TLB invalidation completes. */
		if((old&PTE_RO)==0)old|=PTE_SW_DIRTY;
		if(old&PTE_SW_DIRTY)observed|=HAL_SPACE_PAGE_DIRTY;
		*l=old&~PTE_VALID;}
	hal_wmb();flush_locked(s);
	/* Pages that become executable get their code synchronized first. */
	for(o=0;o<n;o+=4096){uint64_t *l=walk_leaf(s,a+o,0);sync_executable((hal_physaddr_t)(*l&PTE_ADDR),4096,attr);}
	for(o=0;o<n;o+=4096){uint64_t *l=walk_leaf(s,a+o,0),old=*l;*l=(old&PTE_ADDR)|leaf_flags(attr)|(old&PTE_SW_DIRTY);}
	hal_wmb();flush_locked(s);
	for(o=0;o<n;o+=4096){uint64_t *l=walk_leaf(s,a+o,0),entry=*l;if(entry&PTE_AF)observed|=HAL_SPACE_PAGE_ACCESSED;if(entry&PTE_SW_DIRTY)observed|=HAL_SPACE_PAGE_DIRTY;}
	if(flags)*flags=observed;
	space_unlock(s,enabled);return HAL_OK;
}
int hal_space_prot(hal_space_t h,void *v,size_t n,uint32_t attr){return hal_space_prot_query(h,v,n,attr,NULL);}
int hal_space_unmap(hal_space_t h,void *v,size_t n)
{
	struct arm64_space *s=h;struct arm64_table_page *detached;uintptr_t a=(uintptr_t)v,o;bool enabled;if(!n)return HAL_OK;if(!s||!valid_user(a,n))return HAL_ERR_INVALID;
	if(!space_lock_handle(h,&s,&enabled))return HAL_ERR_STATE;
	for(o=0;o<n;o+=4096){uint64_t *l=walk_leaf(s,a+o,0);if(l)*l=0;}
	detached=detach_empty_tables(s);hal_wmb();flush_locked(s);
	free_detached_tables(detached);space_unlock(s,enabled);return HAL_OK;
}
int hal_space_query(hal_space_t h,void *v,uint32_t *flags)
{
	struct arm64_space *s=h;uint64_t *l;bool enabled;if(!s||!flags||!valid_user((uintptr_t)v,4096))return HAL_ERR_INVALID;
	if(!space_lock_handle(h,&s,&enabled))return HAL_ERR_STATE;
	l=walk_leaf(s,(uintptr_t)v,0);*flags=l&&(*l&PTE_VALID)?HAL_SPACE_PAGE_PRESENT|HAL_SPACE_PAGE_ACCESSED:0;if(l&&(*l&PTE_SW_DIRTY))*flags|=HAL_SPACE_PAGE_DIRTY;space_unlock(s,enabled);return HAL_OK;
}
int hal_space_clear_flags(hal_space_t h,void *v,uint32_t flags)
{
	struct arm64_space *s=h;uint64_t *l;bool enabled;if(!s||!valid_user((uintptr_t)v,4096)||(flags&~(HAL_SPACE_PAGE_ACCESSED|HAL_SPACE_PAGE_DIRTY)))return HAL_ERR_INVALID;
	if(!space_lock_handle(h,&s,&enabled))return HAL_ERR_STATE;
	l=walk_leaf(s,(uintptr_t)v,0);if(!l||!(*l&PTE_VALID)){space_unlock(s,enabled);return HAL_ERR_INVALID;}
	/* AF is deliberately conservative.  DIRTY can become clean only after
	 * write permission has already been revoked. */
	if((flags&HAL_SPACE_PAGE_DIRTY)&&(*l&PTE_RO))*l&=~PTE_SW_DIRTY;
	hal_wmb();flush_locked(s);space_unlock(s,enabled);return HAL_OK;
}
/*
 * Drops the stale translations of a space on every CPU that may hold them.
 */
void
hal_space_flush_tlb(
	hal_space_t handle)
{
	struct arm64_space *space;
	bool enabled;
	int locked;

	/* The kernel's tables need no space lock. */
	if (handle == HAL_SPACE_SYS) {
		enabled = hal_irq_disable();
		flush_locked(handle);
		if (enabled)
			hal_irq_enable();
		return;
	}

	/* A user space is flushed under its lock. */
	locked = space_lock_handle(handle, &space, &enabled);
	if (!locked)
		HAL_FATAL("invalid arm64 space flush");
	flush_locked(space);
	space_unlock(space, enabled);
}
void hal_space_flush_tlb_range(hal_space_t h,void*v,size_t n){(void)v;if(n)hal_space_flush_tlb(h);}
size_t hal_space_get_page_size(int level){if(level==1)return 4096;if(level==2)return 0x200000;if(level==3)return 0x40000000;return 0;}
void hal_space_get_user_range(uintptr_t *minimum,uintptr_t *limit){if(minimum)*minimum=4096;if(limit)*limit=ARM64_USER_LIMIT;}
/*
 * Reports how many user spaces and page-table pages exist.
 */
void
hal_arm64_space_memory_stats(
	uint32_t *spaces,
	uint32_t *tables)
{
	bool enabled;

	/* Samples the space count under the registry lock. */
	enabled = arm64_spin_lock(&space_registry_lock);

	if (spaces != NULL)
		*spaces = space_count;

	arm64_spin_unlock(&space_registry_lock, enabled);

	/* The table count moves atomically. */
	if (tables != NULL)
		*tables = __atomic_load_n(&page_table_count, __ATOMIC_RELAXED);
}

/*
 * Maps one device range into the kernel's direct map.
 *
 * arm64 aliases physical space into the kernel half at a fixed offset, so
 * the address the caller receives is always the direct-map alias.  RAM is
 * already mapped there as cacheable memory and is returned as it is.  A
 * device range gets Device-nGnRE, never-execute entries for every 2 MiB
 * block it touches; the boot mapping covers only the peripherals the HAL
 * itself uses, and on a Pi with 4 GiB or more the peripheral hole below
 * 4 GiB starts out mapped as cacheable memory.
 */
int
hal_space_map_device(
	hal_physaddr_t paddr,
	size_t size,
	uint32_t attr,
	void **vaddr)
{
	uint64_t *table;
	uint64_t first;
	uint64_t end;
	uint64_t block;
	bool ram;
	bool touches_ram;
	bool enabled;
	int given;
	int error;

	/* Requires a destination and a non-empty range the direct map reaches. */
	if (vaddr == NULL || size == 0)
		return HAL_ERR_INVALID;
	if ((uint64_t)paddr >= ARM64_DIRECT_LIMIT)
		return HAL_ERR_INVALID;
	if ((uint64_t)size > ARM64_DIRECT_LIMIT - (uint64_t)paddr)
		return HAL_ERR_INVALID;

	/* Refuses an executable device window and a write-combining one. */
	if ((attr & HAL_SPACE_EXEC) != 0)
		return HAL_ERR_INVALID;
	if ((attr & HAL_SPACE_WC) != 0)
		return HAL_ERR_UNSUPPORTED;

	/* The early console's window of exactly the framebuffer goes to the kernel's console once it prints. */
	given = fb_window_give(paddr, size, attr, vaddr);
	if (given)
		return HAL_OK;

	/* Returns RAM's existing cacheable alias. */
	ram = range_is_ram((uint64_t)paddr, (uint64_t)size);
	if (ram) {
		*vaddr = arm64_phys_to_direct((uintptr_t)paddr);
		return HAL_OK;
	}

	/* Refuses a range that mixes RAM and device space. */
	touches_ram = range_touches_ram((uint64_t)paddr, (uint64_t)size);
	if (touches_ram)
		return HAL_ERR_INVALID;

	/* Maps every 2 MiB block the range touches, with interrupts held off. */
	first = (uint64_t)paddr & ~(ARM64_DEVICE_BLOCK_SIZE - 1U);
	end = (uint64_t)paddr + (uint64_t)size;
	enabled = arm64_spin_lock(&kernel_table_lock);
	for (block = first; block < end; block += ARM64_DEVICE_BLOCK_SIZE) {
		/* Finds or creates the level-2 table that holds the block. */
		error = device_block_table((unsigned)(block / ARM64_DIRECT_L1_SPAN), &table);
		if (error != HAL_OK) {
			arm64_spin_unlock(&kernel_table_lock, enabled);

			/* Reports why the block's table could not be made. */
			return error;
		}

		/* Points the block at the device. */
		map_one_device_block(table, (unsigned)((block % ARM64_DIRECT_L1_SPAN) / ARM64_DEVICE_BLOCK_SIZE), block);
	}

	/* Makes the new entries visible to the table walker and drops stale ones. */
	arm64_flush_tlb();
	arm64_spin_unlock(&kernel_table_lock, enabled);

	/* Succeeded: the device is reachable at its direct-map alias. */
	*vaddr = arm64_phys_to_direct((uintptr_t)paddr);
	return HAL_OK;
}

/*
 * Maps managed RAM a second time, uncached, for a device that does not snoop.
 *
 * The view lives in its own level-0 entry of the system half, so making it
 * never touches a live translation.  The direct map's cached lines of the
 * range are written back and discarded first.
 */
int
hal_pmem_map_uncached(
	hal_physaddr_t paddr,
	size_t size,
	void **vaddr)
{
	uint64_t *leaf;
	uint64_t start;
	uint64_t offset;
	uint64_t undo;
	bool ram;
	bool enabled;
	int error;

	/* Requires a destination and a page-aligned, non-empty range. */
	if (vaddr == NULL || size == 0)
		return HAL_ERR_INVALID;
	if (((uint64_t)paddr & (ARM64_PAGE_SIZE - 1U)) != 0)
		return HAL_ERR_INVALID;
	if (((uint64_t)size & (ARM64_PAGE_SIZE - 1U)) != 0)
		return HAL_ERR_INVALID;

	/* Refuses anything but RAM. */
	ram = range_is_ram((uint64_t)paddr, (uint64_t)size);
	if (!ram)
		return HAL_ERR_INVALID;

	/* Writes back and drops the direct map's lines before they could shadow the view. */
	hal_dcache_invalidate_range((uintptr_t)arm64_phys_to_direct((uintptr_t)paddr), size);

	/* Takes addresses for the view from the window. */
	enabled = arm64_spin_lock(&kernel_table_lock);
	if (uncached_next == 0)
		uncached_next = ARM64_UNCACHED_BASE;
	if ((uint64_t)size > ARM64_UNCACHED_BASE + ARM64_UNCACHED_SIZE - uncached_next) {
		arm64_spin_unlock(&kernel_table_lock, enabled);

		/* Reports a window that is used up. */
		return HAL_ERR_NOMEM;
	}

	/* Maps each page as Normal non-cacheable, never executable. */
	start = uncached_next;
	for (offset = 0; offset < (uint64_t)size; offset += ARM64_PAGE_SIZE) {
		/* Finds or makes the page's entry; a failure unmaps what was mapped. */
		error = uncached_leaf(start + offset, true, &leaf);
		if (error != HAL_OK) {
			for (undo = 0; undo < offset; undo += ARM64_PAGE_SIZE) {
				(void)uncached_leaf(start + undo, false, &leaf);
				*leaf = 0;
			}

			/* Drops the translations of the pages already mapped. */
			arm64_flush_tlb();
			arm64_spin_unlock(&kernel_table_lock, enabled);

			/* Reports why the view could not be made. */
			return error;
		}

		/* Points the page at the RAM. */
		*leaf = ((uint64_t)paddr + offset) | PAGE_FLAGS | PTE_ATTR(ARM64_ATTR_NORMAL_NC) | PTE_PXN | PTE_UXN;
	}

	/* Publishes the entries to the walker and takes the addresses. */
	arm64_flush_tlb();
	uncached_next = start + (uint64_t)size;
	arm64_spin_unlock(&kernel_table_lock, enabled);

	/* Succeeded: the RAM is reachable uncached at the view. */
	*vaddr = (void *)(uintptr_t)start;
	return HAL_OK;
}

/*
 * Removes an uncached view made by hal_pmem_map_uncached().
 *
 * The direct map's lines of the range are discarded afterwards, so a line a
 * speculative read brought in while the view existed is not read later.
 */
int
hal_pmem_unmap_uncached(
	void *vaddr,
	size_t size)
{
	uint64_t *leaf;
	uint64_t start;
	uint64_t offset;
	uint64_t physical;
	uint64_t run_start;
	uint64_t run_size;
	bool enabled;
	int error;

	/* Requires a page-aligned range inside the used part of the window. */
	start = (uint64_t)(uintptr_t)vaddr;
	if (size == 0)
		return HAL_ERR_INVALID;
	if ((start & (ARM64_PAGE_SIZE - 1U)) != 0 || ((uint64_t)size & (ARM64_PAGE_SIZE - 1U)) != 0)
		return HAL_ERR_INVALID;
	if (start < ARM64_UNCACHED_BASE || uncached_next == 0)
		return HAL_ERR_INVALID;
	if (start >= uncached_next || (uint64_t)size > uncached_next - start)
		return HAL_ERR_INVALID;

	/* Refuses a hole in the view before any page is unmapped. */
	enabled = arm64_spin_lock(&kernel_table_lock);
	for (offset = 0; offset < (uint64_t)size; offset += ARM64_PAGE_SIZE) {
		error = uncached_leaf(start + offset, false, &leaf);
		if (error != HAL_OK || (*leaf & PTE_VALID) == 0) {
			arm64_spin_unlock(&kernel_table_lock, enabled);

			/* Reports the hole in the view. */
			return HAL_ERR_INVALID;
		}
	}

	/*
	 * Unmaps every page and drops the direct map's lines of it, one
	 * physically contiguous run at a time, then drops the translations
	 * once: a whole TLB flush per page made freeing a large GPU buffer
	 * take tens of milliseconds (ws141-p013, 2026-10-12).  The view is
	 * Normal non-cacheable, so its stale translations until the flush
	 * allocate no line that the invalidation could miss.
	 */
	run_start = 0;
	run_size = 0;
	for (offset = 0; offset < (uint64_t)size; offset += ARM64_PAGE_SIZE) {
		(void)uncached_leaf(start + offset, false, &leaf);
		physical = *leaf & PTE_ADDR;
		*leaf = 0;
		if (run_size != 0 && physical == run_start + run_size) {
			run_size += ARM64_PAGE_SIZE;
			continue;
		}
		if (run_size != 0)
			hal_dcache_invalidate_range((uintptr_t)arm64_phys_to_direct((uintptr_t)run_start), (size_t)run_size);
		run_start = physical;
		run_size = ARM64_PAGE_SIZE;
	}
	hal_dcache_invalidate_range((uintptr_t)arm64_phys_to_direct((uintptr_t)run_start), (size_t)run_size);
	arm64_flush_tlb();

	/* Lets interrupts in again. */
	arm64_spin_unlock(&kernel_table_lock, enabled);

	/* Succeeded: the RAM is reachable through the direct map only. */
	return HAL_OK;
}

/*
 * Releases a caller's use of the shared direct device mapping.
 *
 * Device blocks are permanent kernel aliases shared by boot code and drivers.
 * Removing a whole block for one caller would revoke other live peripherals.
 */
int
hal_space_unmap_device(
	void *vaddr,
	size_t size)
{
	uint64_t address;
	uint64_t physical;
	int matched;
	int error;

	/* The early console's window, given to the kernel's console, goes. */
	error = fb_window_remove(vaddr, size, &matched);
	if (matched) {
		if (error != HAL_OK)
			return error;

		/* Succeeded: no CPU keeps a translation of the framebuffer through the window. */
		return HAL_OK;
	}

	/* Requires a nonempty range in the device mapping's direct window. */
	address = (uint64_t)(uintptr_t)vaddr;
	if (size == 0 || address < ARM64_DIRECT_BASE)
		return HAL_ERR_INVALID;

	/* Excludes uncached RAM views and ranges extending beyond the direct map. */
	physical = address - ARM64_DIRECT_BASE;
	if (physical >= ARM64_DIRECT_LIMIT)
		return HAL_ERR_INVALID;

	/* Checks the length without wrapping the address at the window's end. */
	if ((uint64_t)size > ARM64_DIRECT_LIMIT - physical)
		return HAL_ERR_INVALID;

	/* Succeeded: this caller has released its handle; shared aliases remain. */
	return HAL_OK;
}

/*
 * Builds the early console's window of the framebuffer into the kernel's
 * tables before they are switched to.
 *
 * The window is the same memory type the direct map gives the framebuffer,
 * so that no page is mapped with two types: cached for a framebuffer in
 * RAM, as the direct map holds all RAM, and Device-nGnRE for one outside
 * RAM, as hal_space_map_device() gives device memory, with the direct
 * map's blocks of it made device blocks too.  A framebuffer the window
 * cannot reach is left out: the early console keeps painting it through
 * the direct map, and it is never given.
 */
static void
fb_window_build(
	void)
{
	uint64_t physical;
	uint64_t size;
	uint64_t aligned;
	uint64_t end;
	uint64_t flags;
	bool ram;
	int present;

	/* Nothing to hold without a framebuffer. */
	fb_window.state = ARM64_FB_WINDOW_NONE;
	present = rpi_framebuffer_extent(&physical, &size);
	if (!present)
		return;

	/* Refuses a framebuffer that wraps or that one gigabyte of window does not hold. */
	if (physical + size < physical)
		return;
	aligned = physical & ~(ARM64_DEVICE_BLOCK_SIZE - 1U);
	end = (physical + size + ARM64_PAGE_SIZE - 1U) & ~((uint64_t)ARM64_PAGE_SIZE - 1U);
	if (end - aligned > ARM64_FB_WINDOW_SPAN)
		return;

	/* The direct map's memory type for the framebuffer. */
	ram = range_is_ram(physical, size);
	if (ram) {
		/* RAM: cached, inner shareable like the direct map. */
		flags = PTE_ATTR(0) | PTE_PXN | PTE_UXN;
	} else {
		/*
		 * Device memory: the boot tables' cached lines of it go first, then
		 * the direct map's blocks of it become device blocks.
		 */
		hal_dcache_invalidate_range((uintptr_t)arm64_phys_to_direct((uintptr_t)physical), (size_t)size);
		fb_window_device_direct(physical, size);
		flags = PTE_ATTR(1) | PTE_PXN | PTE_UXN;
	}

	/* Maps the framebuffer's pages into the window. */
	fb_window_fill(physical, size, flags);

	/* The early console holds it, and paints through it until the kernel prints. */
	fb_window.physical = physical;
	fb_window.size = size;
	fb_window.address = (uintptr_t)(ARM64_FB_WINDOW_BASE + (physical - aligned));
	fb_window.cached = 0;
	if (ram)
		fb_window.cached = 1;
	fb_window.state = ARM64_FB_WINDOW_HELD;
}

/*
 * Makes the direct map's 2 MiB blocks of a framebuffer outside RAM device
 * blocks, in the tables being built.  Only blocks of the two split
 * gigabytes are made so, and only those that hold no RAM; another block
 * keeps its mapping (a gigabyte block of RAM is not split).
 */
static void
fb_window_device_direct(
	uint64_t physical,
	uint64_t size)
{
	uint64_t *table;
	uint64_t block;
	uint64_t end;
	unsigned l1_index;
	unsigned l2_index;
	bool touches;

	/* Each 2 MiB block the framebuffer touches. */
	end = physical + size;
	for (block = physical & ~(ARM64_DEVICE_BLOCK_SIZE - 1U); block < end; block += ARM64_DEVICE_BLOCK_SIZE) {
		/* Only the first gigabyte and the 3--4 GiB one are split into blocks. */
		l1_index = (unsigned)(block / ARM64_DIRECT_L1_SPAN);
		l2_index = (unsigned)((block % ARM64_DIRECT_L1_SPAN) / ARM64_DEVICE_BLOCK_SIZE);
		if (l1_index == 0U) {
			table = system_kernel_l2_low;
		} else if (l1_index == 3U) {
			table = system_kernel_l2_high;
		} else {
			continue;
		}

		/* A block left unmapped stays so, and one that holds RAM stays cached. */
		if ((table[l2_index] & PTE_VALID) == 0)
			continue;
		touches = range_touches_ram(block, ARM64_DEVICE_BLOCK_SIZE);
		if (touches)
			continue;

		/* The tables are not in use yet: the entry changes without a break. */
		table[l2_index] = block | BLOCK_FLAGS | PTE_ATTR(1) | PTE_PXN | PTE_UXN;
	}
}

/*
 * Writes the window's entries for a framebuffer: a 2 MiB block entry where
 * the framebuffer's pages fill the block, and a level-3 table of only its
 * pages at either end where they do not.
 */
static void
fb_window_fill(
	uint64_t physical,
	uint64_t size,
	uint64_t flags)
{
	uint64_t *table;
	uint64_t aligned;
	uint64_t first;
	uint64_t end;
	uint64_t block;
	uint64_t page;
	uint64_t stop;
	unsigned edge;
	unsigned index;

	/* Links the window's level-1 and level-2 tables under its level-0 entry. */
	system_kernel_l0[ARM64_FB_WINDOW_L0_INDEX] = table_desc(fb_window_l1);
	fb_window_l1[0] = table_desc(fb_window_l2);

	/* The framebuffer's first and past-the-end pages, and the block the window starts at. */
	first = physical & ~((uint64_t)ARM64_PAGE_SIZE - 1U);
	end = (physical + size + ARM64_PAGE_SIZE - 1U) & ~((uint64_t)ARM64_PAGE_SIZE - 1U);
	aligned = physical & ~(ARM64_DEVICE_BLOCK_SIZE - 1U);

	/* Each block the framebuffer touches, at most two of them partly. */
	edge = 0;
	for (block = aligned; block < end; block += ARM64_DEVICE_BLOCK_SIZE) {
		index = (unsigned)((block - aligned) / ARM64_DEVICE_BLOCK_SIZE);

		/* A block the framebuffer fills is one entry. */
		if (first <= block && end - block >= ARM64_DEVICE_BLOCK_SIZE) {
			fb_window_l2[index] = block | BLOCK_FLAGS | flags;
			continue;
		}

		/* A block it fills partly gets an edge table with its pages alone. */
		table = fb_window_l3[edge];
		edge++;
		page = block;
		if (page < first)
			page = first;
		stop = block + ARM64_DEVICE_BLOCK_SIZE;
		if (stop > end)
			stop = end;
		for (; page < stop; page += ARM64_PAGE_SIZE)
			table[(page - block) / ARM64_PAGE_SIZE] = page | PAGE_FLAGS | flags;
		fb_window_l2[index] = table_desc(table);
	}
}

/*
 * Gives the early console's window to the kernel's console: the first
 * mapping request of exactly the framebuffer (its physical base and size,
 * uncached and writable) once the kernel prints through kernel_putc, when
 * the early console paints no more.  Returns 1 with address set, or 0 for
 * every other request.
 */
static int
fb_window_give(
	hal_physaddr_t physical,
	size_t size,
	uint32_t attr,
	void **address)
{
	void (*printer)(int);
	bool enabled;
	int given;

	/* Only the uncached writable mapping a console asks for. */
	if (attr != (HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_NOCACHE))
		return 0;

	/* Only once the kernel prints, so that the early console no longer paints. */
	printer = __atomic_load_n(&kernel_putc, __ATOMIC_ACQUIRE);
	if (printer == NULL)
		return 0;

	/* Gives only the framebuffer itself, exactly, while the early console holds it. */
	enabled = arm64_spin_lock(&kernel_table_lock);

	given = 0;
	if (fb_window.state == ARM64_FB_WINDOW_HELD &&
	    fb_window.physical == (uint64_t)physical &&
	    fb_window.size == (uint64_t)size) {
		/* GIVEN: the one holder's release removes it; no other request gets it. */
		fb_window.state = ARM64_FB_WINDOW_GIVEN;
		*address = (void *)fb_window.address;
		given = 1;
	}

	arm64_spin_unlock(&kernel_table_lock, enabled);

	/* Reports whether the request took the window. */
	return given;
}

/*
 * Removes the early console's window once it was given and its holder
 * releases it (address and size as given): the early console gives the
 * framebuffer up, the window's level-0 entry goes, and every CPU drops
 * every translation of it.  matched says whether the release was of the
 * window.
 */
static int
fb_window_remove(
	void *address,
	size_t size,
	int *matched)
{
	bool enabled;

	/* Takes only a release of the window as it was given. */
	enabled = arm64_spin_lock(&kernel_table_lock);

	*matched = 0;
	if (fb_window.state != ARM64_FB_WINDOW_GIVEN ||
	    fb_window.address != (uintptr_t)address ||
	    fb_window.size != (uint64_t)size) {
		arm64_spin_unlock(&kernel_table_lock, enabled);
		return HAL_OK;
	}

	/* REMOVED: the window is the remover's alone from here. */
	*matched = 1;
	fb_window.state = ARM64_FB_WINDOW_REMOVED;

	arm64_spin_unlock(&kernel_table_lock, enabled);

	/* The early console gives the framebuffer up first. */
	rpi_cons_framebuffer_release();

	/* Unlinks the window, then every CPU drops its translations. */
	enabled = arm64_spin_lock(&kernel_table_lock);

	__atomic_store_n(&system_kernel_l0[ARM64_FB_WINDOW_L0_INDEX], 0U, __ATOMIC_RELEASE);
	arm64_flush_tlb();

	arm64_spin_unlock(&kernel_table_lock, enabled);

	/* Succeeded: the window is gone. */
	return HAL_OK;
}

/* Reports whether a physical range overlaps any RAM the firmware described. */
static bool
range_touches_ram(
	uint64_t physical,
	uint64_t size)
{
	const struct rpi_fdt_info *info;
	uint64_t ram_end;
	uint64_t end;
	unsigned i;

	/* Compares the range with every memory range of the firmware's tree. */
	info = rpi_boot_info();
	end = physical + size;
	for (i = 0; i < info->memory_count; i++) {
		/* Skips a memory range that ends before the range starts. */
		ram_end = info->memory[i].base + info->memory[i].size;
		if (ram_end <= physical)
			continue;

		/* Skips a memory range that starts after the range ends. */
		if (info->memory[i].base >= end)
			continue;

		/* Reports the overlap. */
		return true;
	}

	/* Reports a range entirely outside RAM. */
	return false;
}

/* Reports whether a physical range lies entirely inside one RAM range. */
static bool
range_is_ram(
	uint64_t physical,
	uint64_t size)
{
	const struct rpi_fdt_info *info;
	uint64_t ram_end;
	unsigned i;

	/* Looks for a memory range of the firmware's tree that holds it all. */
	info = rpi_boot_info();
	for (i = 0; i < info->memory_count; i++) {
		/* Skips a memory range that starts after the range. */
		if (info->memory[i].base > physical)
			continue;

		/* Skips a bank below this address before subtracting its end. */
		ram_end = info->memory[i].base + info->memory[i].size;
		if (physical >= ram_end)
			continue;

		/* Skips a bank that contains the start but not the entire range. */
		if (ram_end - physical < size)
			continue;

		/* Reports the memory range that holds it. */
		return true;
	}

	/* Reports a range that is not wholly RAM. */
	return false;
}

/*
 * Finds the level-2 table of the direct map for one level-1 entry.
 *
 * An empty entry lies beyond RAM and gets a fresh table.  An entry that
 * maps a whole gigabyte of RAM as one block cannot hold a device.
 */
static int
device_block_table(
	unsigned l1_index,
	uint64_t **table)
{
	hal_physaddr_t memory;
	uint64_t entry;
	uint64_t *fresh;
	int error;

	/* Uses the table the entry already points at. */
	entry = system_kernel_l1[l1_index];
	if ((entry & (PTE_VALID | PTE_TABLE)) == (PTE_VALID | PTE_TABLE)) {
		*table = arm64_phys_to_direct((uintptr_t)(entry & PTE_ADDR));
		return HAL_OK;
	}

	/* Refuses to split a gigabyte block of RAM. */
	if ((entry & PTE_VALID) != 0)
		return HAL_ERR_INVALID;

	/* Allocates an empty table for the unmapped gigabyte. */
	error = alloc_page(&memory);
	if (error != HAL_OK)
		return error;

	/* Clears the table so every block of the gigabyte starts unmapped. */
	fresh = arm64_phys_to_direct((uintptr_t)memory);
	hal_memset(fresh, 0, ARM64_PAGE_SIZE);
	(void)__atomic_fetch_add(&page_table_count, 1U, __ATOMIC_RELAXED);

	/*
	 * Links the table.  The entry was invalid, so no stale translation of it
	 * can exist and no break-before-make is needed; the store is ordered
	 * before the walker's next use by the barrier in arm64_flush_tlb().
	 */
	system_kernel_l1[l1_index] = (uint64_t)memory | PTE_VALID | PTE_TABLE;

	/* Succeeded: the caller fills blocks of the new table. */
	*table = fresh;
	return HAL_OK;
}

/*
 * Makes one level-2 entry a Device-nGnRE block for the given physical block.
 *
 * An entry that already maps the block that way is left alone.  An entry
 * that maps anything else is broken before it is remade, as the
 * architecture requires when attributes change.
 */
static void
map_one_device_block(
	uint64_t *table,
	unsigned l2_index,
	uint64_t physical)
{
	uint64_t wanted;

	/* Keeps a block that is already the wanted device mapping. */
	wanted = physical | BLOCK_FLAGS | PTE_ATTR(1) | PTE_PXN | PTE_UXN;
	if (table[l2_index] == wanted)
		return;

	/* Breaks a valid entry and drops its translation before remaking it. */
	if ((table[l2_index] & PTE_VALID) != 0) {
		table[l2_index] = 0;
		arm64_flush_tlb();
	}

	/* Makes the block a device mapping. */
	table[l2_index] = wanted;
}

/*
 * Finds the level-3 entry of one page of the uncached window.
 *
 * With create, missing tables are allocated on the way down.
 */
static int
uncached_leaf(
	uint64_t address,
	bool create,
	uint64_t **leaf)
{
	uint64_t *level1;
	uint64_t *level2;
	uint64_t *level3;
	int error;

	/* Walks from the window's level-0 entry down to the page's table. */
	error = uncached_table(system_kernel_l0, ARM64_UNCACHED_L0_INDEX, create, &level1);
	if (error != HAL_OK)
		return error;

	/* Descends to the gigabyte's table. */
	error = uncached_table(level1, (unsigned)((address >> 30) & 511U), create, &level2);
	if (error != HAL_OK)
		return error;

	/* Descends to the 2 MiB block's table. */
	error = uncached_table(level2, (unsigned)((address >> 21) & 511U), create, &level3);
	if (error != HAL_OK)
		return error;

	/* Succeeded: the page's entry in its level-3 table. */
	*leaf = &level3[(address >> 12) & 511U];
	return HAL_OK;
}

/*
 * Finds the table one entry points at, creating it when asked.
 *
 * A new table is cleared before it is linked, so no stale entry is ever
 * visible to the walker.
 */
static int
uncached_table(
	uint64_t *table,
	unsigned index,
	bool create,
	uint64_t **next)
{
	hal_physaddr_t memory;
	uint64_t *fresh;
	int error;

	/* Uses the table the entry already points at. */
	if ((table[index] & (PTE_VALID | PTE_TABLE)) == (PTE_VALID | PTE_TABLE)) {
		*next = arm64_phys_to_direct((uintptr_t)(table[index] & PTE_ADDR));
		return HAL_OK;
	}

	/* Refuses an entry that is a block, or a missing one without create. */
	if ((table[index] & PTE_VALID) != 0 || !create)
		return HAL_ERR_INVALID;

	/* Allocates and clears the table. */
	error = alloc_page(&memory);
	if (error != HAL_OK)
		return HAL_ERR_NOMEM;

	/* Clears it so every entry starts invalid. */
	fresh = arm64_phys_to_direct((uintptr_t)memory);
	hal_memset(fresh, 0, ARM64_PAGE_SIZE);
	(void)__atomic_fetch_add(&page_table_count, 1U, __ATOMIC_RELAXED);

	/* Links it; the entry was invalid, so nothing stale can be cached. */
	table[index] = (uint64_t)memory | PTE_VALID | PTE_TABLE;

	/* Succeeded: the caller continues into the new table. */
	*next = fresh;
	return HAL_OK;
}
