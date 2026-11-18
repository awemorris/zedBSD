/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv64 address spaces: the kernel's half and the user spaces.
 *
 * Sv39 has one root table per space, so the kernel's half is not a table
 * of its own as on other ports: each user space's root carries a copy of
 * the kernel's root entries 256 to 511.  The kernel's half is therefore
 * built completely at boot, every table below those entries included, and
 * the root entries are never changed again; afterwards only the tables
 * below them change, which every copy shares.
 *
 * The kernel's half holds the direct map of RAM (without the ranges the
 * firmware reserved as no-map), the device window, filled as drivers map
 * their registers, the early console's window of the screen the firmware
 * left on, which the kernel's console takes over and removes, and the
 * kernel image, page by page so that its text alone is executable.  The
 * cache behaviour of memory is the platform's and is not chosen in the
 * tables.
 */

#include <hal/hal.h>
#include "asm.h"
#include "bootinfo.h"
#include "bsp.h"
#include "cons.h"
#include "defs.h"
#include "page.h"
#include "percpu.h"
#include "sbi.h"
#include "space.h"
#include "spin.h"
#include "window.h"

/* The flags of every kernel leaf: valid, global, already accessed and written. */
#define KERNEL_LEAF		(RISCV64_PTE_V | RISCV64_PTE_G | RISCV64_PTE_A | RISCV64_PTE_D)

/* The flags of a pointer to the next table: valid and nothing else. */
#define TABLE_POINTER		RISCV64_PTE_V

/* The permission bits; a valid entry without any of them points at a table. */
#define PERMISSIONS		(RISCV64_PTE_R | RISCV64_PTE_W | RISCV64_PTE_X)

/* The physical page number field of an entry. */
#define PTE_PPN_MASK		0x003ffffffffffc00ULL

/* The bounds of the kernel image's sections, from the linker script. */
extern char __kernel_vma_start[];
extern char __kernel_text_start[];
extern char __kernel_text_end[];
extern char __kernel_rodata_start[];
extern char __kernel_rodata_end[];
extern char __kernel_vma_end[];

/* The boot table locore.S built and runs on until the kernel's own. */
extern uint64_t riscv64_boot_root[];

/*
 * The physical address the kernel image was loaded at.
 *
 * Set by riscv64_kernel_image_init() as the boot hart's first act and
 * constant afterwards; every translation of an image address uses it.
 */
static uint64_t kernel_phys_start;

/*
 * The kernel's root table, which every user space's root copies the upper
 * half of, and which is selected alone when no user space is.
 *
 * Its upper half is written by riscv64_space_init() and never again:
 * kernel_root_sealed says so from then on.
 */
static uint64_t kernel_root[RISCV64_TABLE_ENTRIES] __attribute__((aligned(RISCV64_PAGE_SIZE)));

/*
 * Nonzero once riscv64_space_init() has written kernel_root's upper half,
 * which is never written again from then on.  Written once by the boot hart.
 */
static int kernel_root_sealed;

/*
 * Set once riscv64_space_init() has switched the boot hart to the kernel's
 * root and found the image's sections with the permissions they need; the
 * boot log's closing line reports it.  Written once by the boot hart.
 */
static int kernel_mapping_checked;

/*
 * The second-level tables of the device window, one per root entry.
 *
 * Allocated at boot and linked into the kernel's root; a device mapping
 * writes their entries.  Kept under kernel_table_lock after boot.
 */
static uint64_t *device_tables[RISCV64_DEVICE_ROOT_COUNT];

/* Guards the device window's entries and the screen window's state; zero when free. */
static volatile unsigned kernel_table_lock;

/*
 * The states of the early console's window of the screen: none (no
 * screen, or one the window cannot reach), held by the early console,
 * given to the kernel's console (one mapping request of exactly the
 * screen once the kernel prints), and removed.
 */
enum riscv64_fb_window_state {
	RISCV64_FB_WINDOW_NONE,
	RISCV64_FB_WINDOW_HELD,
	RISCV64_FB_WINDOW_GIVEN,
	RISCV64_FB_WINDOW_REMOVED
};

/*
 * The early console's window of the screen: its state, the screen's
 * physical base and size as the kernel reads them, the kernel address of
 * its first byte, and the second-level table below the window's root
 * entry, whose entries go when the window is removed.
 */
struct riscv64_fb_window {
	enum riscv64_fb_window_state state;
	uint64_t physical;
	uint64_t size;
	uintptr_t address;
	uint64_t *table;
};

/*
 * The early console's window of the screen.  Built by
 * riscv64_space_init(); its state changes under kernel_table_lock.
 */
static struct riscv64_fb_window fb_window;

/*
 * Every user space that exists.  Kept under space_registry_lock; a space is
 * looked up here before its own lock is taken, so a destroyed space is
 * never locked.
 */
static struct riscv64_space *space_registry;

/* The number the next user space gets; kept under space_registry_lock, it starts at one. */
static int next_space_id = 1;

/* How many user spaces space_registry holds; kept under space_registry_lock. */
static uint32_t space_count;

/* Guards space_registry, next_space_id and space_count; zero when free. */
static volatile unsigned space_registry_lock;

/*
 * How many page-table pages the user spaces and the kernel's half hold,
 * for the memory statistics.  Changed with atomic adds, since each space's
 * tables are changed under that space's own lock.
 */
static uint32_t page_table_count;

static int alloc_table(uint64_t **table, hal_physaddr_t *physical);
static uint64_t pointer_entry(hal_physaddr_t physical);
static uint64_t leaf_entry(uint64_t physical, uint64_t flags);
static bool range_is_nomap(uint64_t physical, uint64_t size);
static bool range_is_mappable_ram(uint64_t physical, uint64_t size);
static void build_direct_map(void);
static void build_direct_gigapage(unsigned root_index, uint64_t physical);
static void build_direct_megapage(uint64_t *table, unsigned index, uint64_t physical);
static void build_device_window(void);
static void build_kernel_window(void);
static void build_fb_window(void);
static int fb_window_give(hal_physaddr_t physical, size_t size, uint32_t attr, void **address);
static int fb_window_remove(void *address, size_t size, int *matched);
static uint64_t kernel_page_flags(uintptr_t address);
static void map_hal_devices(void);
static void map_device_blocks(uint64_t physical, uint64_t size);
static void free_table(hal_physaddr_t physical);
static bool leaf_is_uncached(uint64_t entry);
static unsigned long other_ready_harts(void);
static unsigned long harts_selecting(hal_space_t handle, bool *self);
static void flush_space(hal_space_t handle);
static int space_lock_handle(hal_space_t handle, struct riscv64_space **result, bool *irq_enabled);
static void space_unlock(struct riscv64_space *space, bool enabled);
static int valid_user(uintptr_t address, size_t size);
static int valid_attributes(uint32_t attr);
static struct riscv64_table_page *allocate_table(struct riscv64_space *space, uint64_t *parent, unsigned index);
static uint64_t *walk_leaf(struct riscv64_space *space, uintptr_t address, int create);
static int table_empty(const uint64_t *table);
static struct riscv64_table_page *detach_empty_tables(struct riscv64_space *space);
static void free_detached_tables(struct riscv64_table_page *page);
static uint64_t user_leaf_flags(uint32_t attr);
static void sync_executable(uint64_t physical, size_t size, uint32_t attr);

/*
 * Records where the kernel image was loaded.
 */
void
riscv64_kernel_image_init(
	uint64_t phys_start)
{
	/* Every image address translates through this from now on. */
	kernel_phys_start = phys_start;
}

/*
 * Reports the physical address the kernel image was loaded at.
 */
uint64_t
riscv64_kernel_phys_start(
	void)
{
	/* Succeeded: the address recorded at boot. */
	return kernel_phys_start;
}

/*
 * Translates a kernel address, in the image or in the direct map, to its
 * physical address.
 */
uint64_t
riscv64_virt_to_phys(
	const void *address)
{
	uintptr_t value;

	/* An address in the image's window is an offset from the load address. */
	value = (uintptr_t)address;
	if (value >= (uintptr_t)RISCV64_KERNEL_VIRT_BASE)
		return (uint64_t)(value - (uintptr_t)RISCV64_KERNEL_VIRT_BASE) + kernel_phys_start;

	/* Any address outside the direct map is not the kernel's to translate. */
	if (value < (uintptr_t)RISCV64_DIRECT_BASE || value >= (uintptr_t)RISCV64_DIRECT_BASE + RISCV64_DIRECT_LIMIT) {
		HAL_FATAL("riscv64 address outside the image and the direct map");
		return 0;
	}

	/* Succeeded: an address in the direct map is an offset from its base. */
	return (uint64_t)(value - (uintptr_t)RISCV64_DIRECT_BASE);
}

/*
 * Reports the direct-map address of a physical address.
 */
void *
riscv64_phys_to_direct(
	uint64_t physical)
{
	/* The direct map is a fixed offset. */
	return (void *)(uintptr_t)(RISCV64_DIRECT_BASE + physical);
}

/*
 * Reports the satp value of the boot table, which a secondary hart starts
 * on: it maps the image both where it was loaded and where it is linked.
 */
uint64_t
riscv64_boot_satp(
	void)
{
	uint64_t physical;

	/* The boot table is in the image. */
	physical = riscv64_virt_to_phys(riscv64_boot_root);

	/* Succeeded: Sv39 with the table's page number. */
	return ((uint64_t)RISCV64_SATP_MODE_SV39 << 60) | (physical >> RISCV64_PAGE_SHIFT);
}

/*
 * Builds the kernel's half and switches the boot hart to it.
 *
 * Every table below the kernel's root entries is made here, so that the
 * entries never change again and the copies in the user spaces stay right.
 */
void
riscv64_space_init(
	void)
{
	uint64_t text_page;
	uint64_t rodata_page;
	uint64_t data_page;
	uint64_t root_physical;
	uint64_t satp;
	struct riscv64_percpu *cpu;

	/* Starts from an empty root; the user half stays empty for good. */
	hal_memset(kernel_root, 0, sizeof(kernel_root));
	space_registry = NULL;

	/* Builds the three windows the kernel uses. */
	build_direct_map();
	build_device_window();
	build_kernel_window();

	/* Gives the early console its own window of the screen, which the kernel's console can remove. */
	build_fb_window();

	/* No root entry changes from here on. */
	kernel_root_sealed = 1;

	/* Maps the devices the HAL itself uses before leaving the boot table. */
	map_hal_devices();

	/* Switches to the kernel's own root, with the kernel alone selected. */
	root_physical = riscv64_virt_to_phys(kernel_root);
	satp = ((uint64_t)RISCV64_SATP_MODE_SV39 << 60) | (root_physical >> RISCV64_PAGE_SHIFT);
	riscv64_write_satp(satp);
	cpu = riscv64_percpu_current();
	cpu->current_space = HAL_SPACE_SYS;

	/* The early console draws the screen through its window from now on. */
	if (fb_window.state == RISCV64_FB_WINDOW_HELD)
		riscv64_board_screen_window(fb_window.address);

	/* Checks W^X on the three kinds of section the kernel runs from. */
	text_page = kernel_page_flags((uintptr_t)__kernel_text_start);
	rodata_page = kernel_page_flags((uintptr_t)__kernel_rodata_start);
	data_page = kernel_page_flags((uintptr_t)&kernel_root_sealed);
	if ((text_page & RISCV64_PTE_X) == 0 || (text_page & RISCV64_PTE_W) != 0)
		HAL_FATAL("riscv64 kernel text mapping is not read-execute");
	if ((rodata_page & (RISCV64_PTE_X | RISCV64_PTE_W)) != 0)
		HAL_FATAL("riscv64 kernel rodata mapping is not read-only");
	if ((data_page & RISCV64_PTE_X) != 0)
		HAL_FATAL("riscv64 kernel data mapping is executable");

	/* Records the passed checks for the closing line and reports them now. */
	kernel_mapping_checked = 1;
	hal_puts("RISCV64 PAGING PASS\nRISCV64 W^X PASS\n");
}

/*
 * Reports whether the kernel's direct map holds a whole physical range:
 * RAM throughout and clear of the no-map reservations, which the direct
 * map leaves out.  It holds from the boot table on, and in the kernel's
 * own tables once riscv64_space_init() has built them.
 */
bool
riscv64_space_in_direct_map(
	uint64_t physical,
	uint64_t size)
{
	bool mappable;

	/* Refuses a range past the direct map's reach. */
	if (physical >= RISCV64_DIRECT_LIMIT || size > RISCV64_DIRECT_LIMIT - physical)
		return false;

	/* Refuses a range that is not RAM throughout or that a no-map reservation touches. */
	mappable = range_is_mappable_ram(physical, size);
	if (!mappable)
		return false;

	/* Succeeded: the direct map holds the range. */
	return true;
}

/*
 * Reports whether the kernel's tables were switched to and found right.
 */
bool
riscv64_space_checked(
	void)
{
	/* The tables were not built, or a check stopped the boot first. */
	if (!kernel_mapping_checked)
		return false;

	/* Succeeded: the kernel runs on its own tables with W^X in force. */
	return true;
}

/*
 * Makes a secondary hart select the kernel alone, as the boot hart does
 * after riscv64_space_init(); it arrives on the boot table.
 */
void
riscv64_space_select_system(
	void)
{
	struct riscv64_percpu *cpu;
	uint64_t root_physical;
	uint64_t satp;

	/* Publishes the selection, then installs the kernel's root. */
	cpu = riscv64_percpu_current();
	__atomic_store_n(&cpu->current_space, HAL_SPACE_SYS, __ATOMIC_RELEASE);
	hal_mb();
	root_physical = riscv64_virt_to_phys(kernel_root);
	satp = ((uint64_t)RISCV64_SATP_MODE_SV39 << 60) | (root_physical >> RISCV64_PAGE_SHIFT);
	riscv64_write_satp(satp);
}

/*
 * Drops every translation every hart that is up holds, this one included.
 */
void
riscv64_tlb_flush_everywhere(
	void)
{
	unsigned long others;

	/* This hart's TLB, then the others' through the firmware. */
	riscv64_sfence_vma_all();
	others = other_ready_harts();
	if (others != 0)
		(void)riscv64_sbi_remote_sfence_vma(others, 0);
}

/*
 * Makes the device window reach some pages of a megapage-sized region
 * that must not be reached whole: the uncached alias of RAM a driver owns.
 *
 * Each megapage the range touches gets a table of page leaves the first
 * time, and a leaf for each page of the range; its other pages stay
 * unreachable.  A megapage that is already one leaf (a whole-megapage
 * mapping made earlier) is left as it is.  Leaves are never removed.
 */
int
riscv64_space_map_alias(
	uint64_t physical,
	uint64_t size)
{
	hal_physaddr_t spare_physical;
	uint64_t *spare;
	uint64_t *table;
	uint64_t block;
	uint64_t end;
	uint64_t entry;
	unsigned root;
	unsigned index;
	bool enabled;
	int error;

	/* Requires a non-empty range the window reaches. */
	if (size == 0 || physical >= RISCV64_DEVICE_LIMIT)
		return HAL_ERR_INVALID;
	if (size > RISCV64_DEVICE_LIMIT - physical)
		return HAL_ERR_INVALID;

	/* Gives every megapage the range touches the leaves of its pages. */
	end = physical + size;
	for (block = physical & ~((uint64_t)RISCV64_MEGAPAGE_SIZE - 1U); block < end; block += RISCV64_MEGAPAGE_SIZE) {
		root = (unsigned)(block >> RISCV64_GIGAPAGE_SHIFT);
		index = (unsigned)((block % RISCV64_GIGAPAGE_SIZE) / RISCV64_MEGAPAGE_SIZE);

		/* Makes a table outside the lock when the megapage has none yet; entries are never cleared. */
		spare = NULL;
		spare_physical = 0;
		entry = __atomic_load_n(&device_tables[root][index], __ATOMIC_RELAXED);
		if (entry == 0) {
			error = alloc_table(&spare, &spare_physical);
			if (error != HAL_OK)
				return HAL_ERR_NOMEM;
		}

		/* Links the table if the megapage still has none, then writes the range's leaves. */
		enabled = riscv64_spin_lock(&kernel_table_lock);

		entry = device_tables[root][index];
		if (entry == 0 && spare != NULL) {
			hal_wmb();
			device_tables[root][index] = pointer_entry(spare_physical);
			entry = device_tables[root][index];
			spare = NULL;
		}

		if ((entry & RISCV64_PTE_V) != 0 && (entry & PERMISSIONS) == 0) {
			table = riscv64_phys_to_direct((entry & PTE_PPN_MASK) >> RISCV64_PTE_PPN_SHIFT << RISCV64_PAGE_SHIFT);
			(void)riscv64_window_fill_pages(table, block, physical, size, KERNEL_LEAF | RISCV64_PTE_R | RISCV64_PTE_W);
		}

		riscv64_spin_unlock(&kernel_table_lock, enabled);

		/* Returns a table another caller's link made unnecessary. */
		if (spare != NULL)
			free_table(spare_physical);
	}

	/* Publishes the leaves, then drops stale entries everywhere. */
	hal_wmb();
	riscv64_tlb_flush_everywhere();

	/* Succeeded: every page of the range is reachable in the window. */
	return HAL_OK;
}

/*
 * Creates an empty user space.
 */
hal_space_t
hal_space_create(
	void)
{
	struct riscv64_space *space;
	unsigned index;
	bool enabled;
	int error;

	/* A user space copies the kernel's half, which must be complete. */
	if (!kernel_root_sealed)
		HAL_FATAL("riscv64 user space before the kernel half is built");

	/* Allocates the record. */
	space = kernel_alloc(sizeof(*space));
	if (space == NULL)
		return NULL;

	/* Starts from an empty record. */
	hal_memset(space, 0, sizeof(*space));

	/* Allocates the root table. */
	error = hal_pmem_alloc(RISCV64_PAGE_SIZE, RISCV64_PAGE_SIZE, &space->root_memory);
	if (error != HAL_OK) {
		kernel_free(space);
		return NULL;
	}

	/* Reaches the root table through the direct map. */
	space->root = hal_pmem_to_kernel(space->root_memory);

	/* Empties the user half. */
	for (index = 0; index < RISCV64_USER_ROOT_COUNT; index++)
		space->root[index] = 0;

	/* Copies the kernel's half, which never changes again. */
	for (index = RISCV64_USER_ROOT_COUNT; index < RISCV64_TABLE_ENTRIES; index++)
		space->root[index] = kernel_root[index];

	/* Marks the record as a live space for the handle checks. */
	space->magic = RISCV64_SPACE_MAGIC;

	/* Numbers the space and makes it live. */
	enabled = riscv64_spin_lock(&space_registry_lock);

	space->space_id = next_space_id++;
	space->registry_next = space_registry;
	space_registry = space;
	space_count++;

	riscv64_spin_unlock(&space_registry_lock, enabled);

	/* Succeeded: the space, with nothing of the user's mapped. */
	return space;
}

/*
 * Destroys a user space that no hart has selected and no operation holds.
 */
void
hal_space_destroy(
	hal_space_t handle)
{
	struct riscv64_space *space;
	struct riscv64_space **link;
	struct riscv64_table_page *page;
	unsigned long selecting;
	bool self;
	bool enabled;

	/* A missing space is nothing to destroy. */
	space = handle;
	if (space == NULL)
		return;

	/* Takes the space out of the registry, so nothing can lock it any more. */
	enabled = riscv64_spin_lock(&space_registry_lock);

	/* Finds the space's link; a space not listed or already going is a fault. */
	link = &space_registry;
	while (*link != NULL && *link != space)
		link = &(*link)->registry_next;
	if (*link == NULL || space->destroying)
		HAL_FATAL("invalid riscv64 space destroy");

	/* A space some hart still has selected cannot go. */
	selecting = harts_selecting(handle, &self);
	if (selecting != 0 || self)
		HAL_FATAL("destroying an active riscv64 space");

	/* A space an operation holds cannot go. */
	if (space->lock != 0)
		HAL_FATAL("destroying a busy riscv64 space");

	/* Marks the space as going, unlinks it and stops counting it. */
	space->destroying = 1U;
	*link = space->registry_next;
	if (space_count != 0)
		space_count--;

	riscv64_spin_unlock(&space_registry_lock, enabled);

	/* Frees every table the space made, and stops counting it. */
	while (space->tables != NULL) {
		page = space->tables;
		space->tables = page->next;
		(void)hal_pmem_free(&page->memory, RISCV64_PAGE_SIZE);
		kernel_free(page);
		(void)__atomic_fetch_sub(&page_table_count, 1U, __ATOMIC_RELAXED);
	}

	/* Marks the record dead for a stale handle, then frees the root and the record. */
	space->magic = 0;
	(void)hal_pmem_free(&space->root_memory, RISCV64_PAGE_SIZE);
	kernel_free(space);
}

/*
 * Selects a user space, or the kernel alone, on the calling hart.
 *
 * The selection is published before satp changes, so a hart that changes
 * the space's tables afterwards sees it and flushes this hart too.  Only
 * this hart's TLB is emptied: without address-space identifiers, what it
 * cached of the space it leaves must go, and no other hart holds entries
 * for this hart's selection.
 */
void
hal_space_switch(
	hal_space_t handle)
{
	struct riscv64_percpu *cpu;
	struct riscv64_space *space;
	uint64_t root_physical;
	uint64_t satp;
	bool enabled;
	bool space_enabled;
	int locked;

	/*
	 * Names the hart with interrupts off, so that a caller outside a task
	 * switch cannot move to another hart between reading the record and
	 * installing the table.
	 */
	enabled = riscv64_irq_save();
	cpu = riscv64_percpu_current();

	/* The space already selected here needs nothing. */
	if (handle == cpu->current_space) {
		riscv64_irq_restore(enabled);
		return;
	}

	/* The kernel alone: its own root. */
	if (handle == HAL_SPACE_SYS) {
		__atomic_store_n(&cpu->current_space, handle, __ATOMIC_RELEASE);
		hal_mb();
		root_physical = riscv64_virt_to_phys(kernel_root);
		satp = ((uint64_t)RISCV64_SATP_MODE_SV39 << 60) | (root_physical >> RISCV64_PAGE_SHIFT);
		riscv64_write_satp(satp);
		riscv64_irq_restore(enabled);
		return;
	}

	/* A user space is held while its root is installed. */
	locked = space_lock_handle(handle, &space, &space_enabled);
	if (!locked)
		HAL_FATAL("invalid riscv64 space switch");

	__atomic_store_n(&cpu->current_space, handle, __ATOMIC_RELEASE);
	hal_mb();
	satp = ((uint64_t)RISCV64_SATP_MODE_SV39 << 60) | ((uint64_t)space->root_memory >> RISCV64_PAGE_SHIFT);
	riscv64_write_satp(satp);

	space_unlock(space, space_enabled);

	/* Restores the caller's interrupt state. */
	riscv64_irq_restore(enabled);
}

/*
 * Maps managed RAM into a user space.
 */
int
hal_space_map(
	hal_space_t handle,
	void *vaddr,
	hal_physaddr_t paddr,
	size_t size,
	uint32_t attr)
{
	struct riscv64_space *space;
	struct riscv64_table_page *detached;
	uint64_t *leaf;
	uintptr_t address;
	uintptr_t offset;
	uintptr_t rollback;
	uint64_t leaf_flags;
	uint64_t target;
	bool enabled;
	bool ram;
	int valid;
	int locked;

	/* Requires a user range and a page-aligned range of RAM. */
	address = (uintptr_t)vaddr;
	if (handle == HAL_SPACE_SYS)
		return HAL_ERR_INVALID;
	valid = valid_user(address, size);
	if (!valid)
		return HAL_ERR_INVALID;
	if (((uint64_t)paddr & (RISCV64_PAGE_SIZE - 1U)) != 0)
		return HAL_ERR_INVALID;
	ram = riscv64_range_is_ram((uint64_t)paddr, (uint64_t)size);
	if (!ram)
		return HAL_ERR_INVALID;

	/* Requires attributes this port can represent. */
	valid = valid_attributes(attr);
	if (valid != HAL_OK)
		return valid;

	/* Holds the space for the whole change. */
	locked = space_lock_handle(handle, &space, &enabled);
	if (!locked)
		return HAL_ERR_STATE;

	/* Refuses a range of which any page is mapped already. */
	for (offset = 0; offset < size; offset += RISCV64_PAGE_SIZE) {
		leaf = walk_leaf(space, address + offset, 0);
		if (leaf != NULL && (*leaf & RISCV64_PTE_V) != 0) {
			space_unlock(space, enabled);
			return HAL_ERR_INVALID;
		}
	}

	/* Code in the pages is fetched correctly once they are executable. */
	sync_executable((uint64_t)paddr, size, attr);

	/*
	 * An uncached mapping points at the board's alias of the pages, after
	 * their cached lines are written back and discarded so none can later
	 * overwrite what is stored through the alias.
	 */
	target = (uint64_t)paddr;
	if ((attr & HAL_SPACE_NOCACHE) != 0) {
		riscv64_board_flush_physical((uint64_t)paddr, (uint64_t)size);
		target += riscv64_board_uncached_offset();
	}

	/* Writes a leaf for each page, unwinding them all if a table cannot be made. */
	leaf_flags = user_leaf_flags(attr);
	for (offset = 0; offset < size; offset += RISCV64_PAGE_SIZE) {
		/* Finds or makes the page's leaf. */
		leaf = walk_leaf(space, address + offset, 1);
		if (leaf == NULL) {
			/* Without a table, clears the leaves written so far. */
			for (rollback = 0; rollback < offset; rollback += RISCV64_PAGE_SIZE) {
				leaf = walk_leaf(space, address + rollback, 0);
				if (leaf != NULL)
					*leaf = 0;
			}

			/* Unlinks the tables left empty, drops their translations and fails. */
			detached = detach_empty_tables(space);
			hal_wmb();
			flush_space(handle);
			free_detached_tables(detached);
			space_unlock(space, enabled);
			return HAL_ERR_NOMEM;
		}

		/* Points the leaf at the page or its alias. */
		*leaf = leaf_entry(target + offset, leaf_flags);
	}

	/* Publishes the leaves to the table walkers of every hart that has the space. */
	hal_wmb();
	flush_space(handle);

	space_unlock(space, enabled);

	/* Succeeded: the range is mapped. */
	return HAL_OK;
}

/*
 * Removes a user range's mappings.
 */
int
hal_space_unmap(
	hal_space_t handle,
	void *vaddr,
	size_t size)
{
	struct riscv64_space *space;
	struct riscv64_table_page *detached;
	uint64_t *leaf;
	uintptr_t address;
	uintptr_t offset;
	bool enabled;
	int valid;
	int locked;

	/* An empty range is nothing to remove. */
	if (size == 0)
		return HAL_OK;

	/* Requires a user range. */
	address = (uintptr_t)vaddr;
	if (handle == HAL_SPACE_SYS)
		return HAL_ERR_INVALID;
	valid = valid_user(address, size);
	if (!valid)
		return HAL_ERR_INVALID;

	/* Holds the space for the whole change. */
	locked = space_lock_handle(handle, &space, &enabled);
	if (!locked)
		return HAL_ERR_STATE;

	/* Clears each leaf. */
	for (offset = 0; offset < size; offset += RISCV64_PAGE_SIZE) {
		leaf = walk_leaf(space, address + offset, 0);
		if (leaf != NULL)
			*leaf = 0;
	}

	/* Unlinks the tables that became empty. */
	detached = detach_empty_tables(space);

	/* Drops the stale translations before the tables are reused. */
	hal_wmb();
	flush_space(handle);
	free_detached_tables(detached);

	space_unlock(space, enabled);

	/* Succeeded: the range is unmapped. */
	return HAL_OK;
}

/*
 * Changes a mapped user range's protection and reports what the old
 * translations recorded.
 *
 * The old leaves are invalidated and flushed before the new ones are
 * written, so a store through a stale translation cannot land after the
 * caller sees the range as clean.  The hardware's accessed and dirty bits
 * are not read: every leaf is made with both set, a valid page counts as
 * accessed, and a page counts as dirty while it is or was writable since
 * the kernel last cleaned it (the software dirty bit).
 */
int
hal_space_prot_query(
	hal_space_t handle,
	void *vaddr,
	size_t size,
	uint32_t attr,
	uint32_t *flags)
{
	struct riscv64_space *space;
	uint64_t *leaf;
	uint64_t old;
	uintptr_t address;
	uintptr_t offset;
	uint64_t leaf_flags;
	uint32_t observed;
	bool enabled;
	bool uncached;
	int valid;
	int locked;

	/* Requires a user range and attributes this port can represent. */
	address = (uintptr_t)vaddr;
	if (handle == HAL_SPACE_SYS)
		return HAL_ERR_INVALID;
	valid = valid_user(address, size);
	if (!valid)
		return HAL_ERR_INVALID;
	valid = valid_attributes(attr);
	if (valid != HAL_OK)
		return valid;

	/* Holds the space for the whole change. */
	locked = space_lock_handle(handle, &space, &enabled);
	if (!locked)
		return HAL_ERR_STATE;

	/* Refuses a range with any page unmapped. */
	for (offset = 0; offset < size; offset += RISCV64_PAGE_SIZE) {
		leaf = walk_leaf(space, address + offset, 0);
		if (leaf == NULL || (*leaf & RISCV64_PTE_V) == 0) {
			space_unlock(space, enabled);
			return HAL_ERR_INVALID;
		}

		/*
		 * A page keeps its cache behaviour, which a protection change
		 * does not choose; an uncached page is never made executable.
		 */
		uncached = leaf_is_uncached(*leaf);
		if (uncached && (attr & HAL_SPACE_EXEC) != 0) {
			space_unlock(space, enabled);
			return HAL_ERR_UNSUPPORTED;
		}
	}

	/* Invalidates each old leaf, noting what it recorded. */
	observed = 0;
	for (offset = 0; offset < size; offset += RISCV64_PAGE_SIZE) {
		leaf = walk_leaf(space, address + offset, 0);
		old = *leaf;
		observed |= HAL_SPACE_PAGE_PRESENT | HAL_SPACE_PAGE_ACCESSED;

		/* A writable page may have been written through the old translation. */
		if ((old & RISCV64_PTE_W) != 0)
			old |= RISCV64_PTE_SW_DIRTY;

		/* Reports a page the software dirty bit records as written. */
		if ((old & RISCV64_PTE_SW_DIRTY) != 0)
			observed |= HAL_SPACE_PAGE_DIRTY;

		/* Leaves the leaf invalid, its page and dirty record kept. */
		*leaf = old & ~(uint64_t)RISCV64_PTE_V;
	}

	/* Drops the old translations on every hart that has the space. */
	hal_wmb();
	flush_space(handle);

	/* Pages that become executable get their code synchronized first. */
	for (offset = 0; offset < size; offset += RISCV64_PAGE_SIZE) {
		leaf = walk_leaf(space, address + offset, 0);
		sync_executable((*leaf & PTE_PPN_MASK) >> RISCV64_PTE_PPN_SHIFT << RISCV64_PAGE_SHIFT, RISCV64_PAGE_SIZE, attr);
	}

	/* Writes the new leaves, keeping each page's dirty record. */
	leaf_flags = user_leaf_flags(attr);
	for (offset = 0; offset < size; offset += RISCV64_PAGE_SIZE) {
		leaf = walk_leaf(space, address + offset, 0);
		old = *leaf;
		*leaf = (old & PTE_PPN_MASK) | leaf_flags | (old & RISCV64_PTE_SW_DIRTY);
	}

	/* Publishes the new leaves and drops the old translations. */
	hal_wmb();
	flush_space(handle);

	space_unlock(space, enabled);

	/* Reports what the old translations recorded, when asked. */
	if (flags != NULL)
		*flags = observed;

	/* Succeeded: the range has the new protection. */
	return HAL_OK;
}

/*
 * Changes a mapped user range's protection.
 */
int
hal_space_prot(
	hal_space_t handle,
	void *vaddr,
	size_t size,
	uint32_t attr)
{
	int error;

	/* The same change without the report. */
	error = hal_space_prot_query(handle, vaddr, size, attr, NULL);
	if (error != HAL_OK)
		return error;

	/* Succeeded: the range has the new protection. */
	return HAL_OK;
}

/*
 * Reports the state of one user page: present, accessed and dirty.
 */
int
hal_space_query(
	hal_space_t handle,
	void *vaddr,
	uint32_t *flags)
{
	struct riscv64_space *space;
	uint64_t *leaf;
	uint32_t observed;
	bool enabled;
	int valid;
	int locked;

	/* Requires a user page and somewhere to report. */
	if (handle == HAL_SPACE_SYS || flags == NULL)
		return HAL_ERR_INVALID;
	valid = valid_user((uintptr_t)vaddr, RISCV64_PAGE_SIZE);
	if (!valid)
		return HAL_ERR_INVALID;

	/* Reads the page's leaf under the space's lock. */
	locked = space_lock_handle(handle, &space, &enabled);
	if (!locked)
		return HAL_ERR_STATE;

	observed = 0;
	leaf = walk_leaf(space, (uintptr_t)vaddr, 0);
	if (leaf != NULL && (*leaf & RISCV64_PTE_V) != 0)
		observed = HAL_SPACE_PAGE_PRESENT | HAL_SPACE_PAGE_ACCESSED;
	if (leaf != NULL && (*leaf & RISCV64_PTE_SW_DIRTY) != 0)
		observed |= HAL_SPACE_PAGE_DIRTY;

	space_unlock(space, enabled);

	/* Reports the page's state. */
	*flags = observed;

	/* Succeeded: the state is the caller's. */
	return HAL_OK;
}

/*
 * Clears the recorded state of one user page.
 *
 * Accessed is not tracked, so clearing it does nothing.  Dirty may be
 * cleared only once the page is no longer writable, since a writable page
 * may be written at any moment.
 */
int
hal_space_clear_flags(
	hal_space_t handle,
	void *vaddr,
	uint32_t flags)
{
	struct riscv64_space *space;
	uint64_t *leaf;
	bool enabled;
	int valid;
	int locked;

	/* Requires a user page and flags this port records. */
	if (handle == HAL_SPACE_SYS)
		return HAL_ERR_INVALID;
	valid = valid_user((uintptr_t)vaddr, RISCV64_PAGE_SIZE);
	if (!valid)
		return HAL_ERR_INVALID;
	if ((flags & ~(uint32_t)(HAL_SPACE_PAGE_ACCESSED | HAL_SPACE_PAGE_DIRTY)) != 0)
		return HAL_ERR_INVALID;

	/* Changes the page's leaf under the space's lock. */
	locked = space_lock_handle(handle, &space, &enabled);
	if (!locked)
		return HAL_ERR_STATE;

	/* Refuses a page that is not mapped. */
	leaf = walk_leaf(space, (uintptr_t)vaddr, 0);
	if (leaf == NULL || (*leaf & RISCV64_PTE_V) == 0) {
		space_unlock(space, enabled);
		return HAL_ERR_INVALID;
	}

	/* A read-only page can be called clean. */
	if ((flags & HAL_SPACE_PAGE_DIRTY) != 0 && (*leaf & RISCV64_PTE_W) == 0)
		*leaf &= ~(uint64_t)RISCV64_PTE_SW_DIRTY;

	/* Publishes the leaf and drops the stale entries. */
	hal_wmb();
	flush_space(handle);

	space_unlock(space, enabled);

	/* Succeeded: the page's state is cleared as far as it may be. */
	return HAL_OK;
}

/*
 * Drops the stale translations of a space on every hart that may hold them.
 */
void
hal_space_flush_tlb(
	hal_space_t handle)
{
	struct riscv64_space *space;
	bool enabled;
	int locked;

	/* The kernel's tables need no space lock. */
	if (handle == HAL_SPACE_SYS) {
		enabled = riscv64_irq_save();
		flush_space(handle);
		riscv64_irq_restore(enabled);
		return;
	}

	/* A user space is flushed under its lock. */
	locked = space_lock_handle(handle, &space, &enabled);
	if (!locked)
		HAL_FATAL("invalid riscv64 space flush");

	flush_space(handle);

	space_unlock(space, enabled);
}

/*
 * Drops the stale translations of a range; the whole space is flushed.
 *
 * Some processors of this architecture do not honour a fence for one
 * address, so the HAL never relies on one.
 */
void
hal_space_flush_tlb_range(
	hal_space_t handle,
	void *vaddr,
	size_t size)
{
	UNUSED_PARAMETER(vaddr);

	/* An empty range has nothing stale. */
	if (size == 0)
		return;

	/* Flushes the whole space. */
	hal_space_flush_tlb(handle);
}

/*
 * Reports the size a table level maps: 1 a page, 2 a megapage, 3 a gigapage.
 */
size_t
hal_space_get_page_size(
	int level)
{
	/* Chooses the size of the level. */
	switch (level) {
	case 1:
		return RISCV64_PAGE_SIZE;
	case 2:
		return RISCV64_MEGAPAGE_SIZE;
	case 3:
		return RISCV64_GIGAPAGE_SIZE;
	default:
		break;
	}

	/* No such level. */
	return 0;
}

/*
 * Reports the user half's bounds.
 */
void
hal_space_get_user_range(
	uintptr_t *minimum,
	uintptr_t *limit)
{
	/* The first page is never mapped; the half ends at 256 GiB. */
	if (minimum != NULL)
		*minimum = RISCV64_USER_MINIMUM;
	if (limit != NULL)
		*limit = RISCV64_USER_LIMIT;
}

/*
 * Maps a device range into the kernel's device window.
 *
 * Device P is reached at the window's base plus P.  RAM is already in the
 * direct map and is returned as it is; a range that mixes RAM and devices
 * is refused.  The window's entries are permanent aliases shared by every
 * driver of a block, as the device window's tables are by every space.
 */
int
hal_space_map_device(
	hal_physaddr_t paddr,
	size_t size,
	uint32_t attr,
	void **vaddr)
{
	bool ram;
	bool touches;
	bool enabled;
	int given;

	/* Requires a destination and a non-empty range the window reaches. */
	if (vaddr == NULL || size == 0)
		return HAL_ERR_INVALID;
	if ((uint64_t)paddr >= RISCV64_DEVICE_LIMIT)
		return HAL_ERR_INVALID;
	if ((uint64_t)size > RISCV64_DEVICE_LIMIT - (uint64_t)paddr)
		return HAL_ERR_INVALID;

	/* Refuses an executable window and the cache behaviours the tables cannot choose. */
	if ((attr & HAL_SPACE_EXEC) != 0)
		return HAL_ERR_INVALID;
	if ((attr & (HAL_SPACE_WC | HAL_SPACE_WRITETHRU)) != 0)
		return HAL_ERR_UNSUPPORTED;

	/* The early console's window of exactly the screen goes to the kernel's console once it prints. */
	given = fb_window_give(paddr, size, attr, vaddr);
	if (given)
		return HAL_OK;

	/* Returns RAM's existing alias. */
	ram = riscv64_range_is_ram((uint64_t)paddr, (uint64_t)size);
	if (ram) {
		*vaddr = riscv64_phys_to_direct((uint64_t)paddr);
		return HAL_OK;
	}

	/* Refuses a range that mixes RAM and devices. */
	touches = riscv64_range_touches_ram((uint64_t)paddr, (uint64_t)size);
	if (touches)
		return HAL_ERR_INVALID;

	/* Maps every megapage the range touches, then drops stale entries everywhere. */
	enabled = riscv64_spin_lock(&kernel_table_lock);

	map_device_blocks((uint64_t)paddr, (uint64_t)size);
	riscv64_tlb_flush_everywhere();

	riscv64_spin_unlock(&kernel_table_lock, enabled);

	/* Reports where the window reaches the device. */
	*vaddr = (void *)(uintptr_t)(RISCV64_DEVICE_BASE + (uint64_t)paddr);

	/* Succeeded: the device is reachable in the window. */
	return HAL_OK;
}

/*
 * Releases a caller's use of the shared device window.
 *
 * The window's entries are shared by every driver of a block, so one
 * caller's release leaves them in place.
 */
int
hal_space_unmap_device(
	void *vaddr,
	size_t size)
{
	uint64_t address;
	uint64_t offset;
	int matched;
	int error;

	/* The early console's window, given to the kernel's console, goes. */
	error = fb_window_remove(vaddr, size, &matched);
	if (matched) {
		if (error != HAL_OK)
			return error;

		/* Succeeded: no hart keeps a translation of the screen through the window. */
		return HAL_OK;
	}

	/* Requires a non-empty range inside the device window or the direct map. */
	address = (uint64_t)(uintptr_t)vaddr;
	if (size == 0)
		return HAL_ERR_INVALID;

	/* A range in the device window leaves the shared entries in place. */
	if (address >= RISCV64_DEVICE_BASE && address < RISCV64_DEVICE_BASE + RISCV64_DEVICE_LIMIT) {
		/* Refuses a range that runs past the window's end. */
		offset = address - RISCV64_DEVICE_BASE;
		if ((uint64_t)size > RISCV64_DEVICE_LIMIT - offset)
			return HAL_ERR_INVALID;

		/* Succeeded: nothing to undo in the shared window. */
		return HAL_OK;
	}

	/* A range in the direct map was never mapped for the caller. */
	if (address >= RISCV64_DIRECT_BASE && address < RISCV64_DIRECT_BASE + RISCV64_DIRECT_LIMIT)
		return HAL_OK;

	/* The address was never handed out by hal_space_map_device(). */
	return HAL_ERR_INVALID;
}

/*
 * Makes code written through the data side visible to instruction fetch on
 * every hart.
 */
void
hal_sync_instruction_stream(
	void *addr,
	size_t size)
{
	unsigned long others;

	UNUSED_PARAMETER(addr);

	/* An empty range needs nothing. */
	if (size == 0)
		return;

	/* The calling hart, then every other hart that is up. */
	riscv64_fence_i();
	others = other_ready_harts();
	if (others != 0)
		(void)riscv64_sbi_remote_fence_i(others, 0);
}

/*
 * Makes a range of instructions current; the whole instruction stream is.
 */
void
hal_icache_invalidate_range(
	uintptr_t addr,
	size_t size)
{
	/* The same synchronization, for the caller's range. */
	hal_sync_instruction_stream((void *)addr, size);
}

/*
 * Reports how many user spaces and page-table pages exist.
 */
void
riscv64_space_memory_stats(
	uint32_t *spaces,
	uint32_t *tables)
{
	bool enabled;

	/* Samples the space count under the registry lock. */
	enabled = riscv64_spin_lock(&space_registry_lock);

	if (spaces != NULL)
		*spaces = space_count;

	riscv64_spin_unlock(&space_registry_lock, enabled);

	/* The table count moves atomically. */
	if (tables != NULL)
		*tables = __atomic_load_n(&page_table_count, __ATOMIC_RELAXED);
}

/* Allocates a zeroed page for a kernel table and counts it. */
static int
alloc_table(
	uint64_t **table,
	hal_physaddr_t *physical)
{
	int error;

	/* One page, page-aligned. */
	error = hal_pmem_alloc(RISCV64_PAGE_SIZE, RISCV64_PAGE_SIZE, physical);
	if (error != HAL_OK)
		return error;

	/* Clears it through the direct map, which the boot table also has. */
	*table = riscv64_phys_to_direct((uint64_t)*physical);
	hal_memset(*table, 0, RISCV64_PAGE_SIZE);
	(void)__atomic_fetch_add(&page_table_count, 1U, __ATOMIC_RELAXED);

	/* Succeeded: the table is empty and the caller's. */
	return HAL_OK;
}

/* Builds an entry that points at the next table. */
static uint64_t
pointer_entry(
	hal_physaddr_t physical)
{
	/* The table's page number and the valid bit alone. */
	return ((uint64_t)physical >> RISCV64_PAGE_SHIFT << RISCV64_PTE_PPN_SHIFT) | TABLE_POINTER;
}

/* Builds a leaf entry for a physical address with the given flags. */
static uint64_t
leaf_entry(
	uint64_t physical,
	uint64_t flags)
{
	/* The page number and the flags. */
	return (physical >> RISCV64_PAGE_SHIFT << RISCV64_PTE_PPN_SHIFT) | flags;
}

/* Reports whether a range overlaps a reservation that must not be mapped. */
static bool
range_is_nomap(
	uint64_t physical,
	uint64_t size)
{
	const struct riscv64_fdt_info *info;
	uint64_t reserved_end;
	uint64_t end;
	unsigned index;

	/* Compares the range with every no-map reservation. */
	info = riscv64_boot_fdt();
	end = physical + size;
	for (index = 0; index < info->nomap_count; index++) {
		/* A reservation that ends before the range does not overlap it. */
		reserved_end = info->nomap[index].base + info->nomap[index].size;
		if (reserved_end <= physical)
			continue;

		/* A reservation that starts after the range does not either. */
		if (info->nomap[index].base >= end)
			continue;

		/* Any other reservation overlaps the range. */
		return true;
	}

	/* The range is clear of them. */
	return false;
}

/* Reports whether a whole range is RAM and clear of no-map reservations. */
static bool
range_is_mappable_ram(
	uint64_t physical,
	uint64_t size)
{
	bool ram;
	bool nomap;

	/* Must be RAM throughout. */
	ram = riscv64_range_is_ram(physical, size);
	if (!ram)
		return false;

	/* Must not overlap a no-map reservation. */
	nomap = range_is_nomap(physical, size);
	if (nomap)
		return false;

	/* Succeeded: the range can be one leaf of the direct map. */
	return true;
}

/*
 * Builds the direct map: RAM in the largest leaves its alignment and the
 * reservations allow.
 */
static void
build_direct_map(
	void)
{
	uint64_t first;
	uint64_t end;
	uint64_t physical;
	unsigned root_index;
	bool touches;

	/* Visits each gigapage of the RAM span. */
	first = riscv64_ram_base() & ~((uint64_t)RISCV64_GIGAPAGE_SIZE - 1U);
	end = riscv64_ram_end();
	for (physical = first; physical < end; physical += RISCV64_GIGAPAGE_SIZE) {
		/* A gigapage with no RAM in it stays unmapped. */
		touches = riscv64_range_touches_ram(physical, RISCV64_GIGAPAGE_SIZE);
		if (!touches)
			continue;

		/* Maps the gigapage below its root entry. */
		root_index = RISCV64_DIRECT_ROOT_FIRST + (unsigned)(physical >> RISCV64_GIGAPAGE_SHIFT);
		build_direct_gigapage(root_index, physical);
	}
}

/* Maps one gigapage of the direct map, as a leaf or through a table. */
static void
build_direct_gigapage(
	unsigned root_index,
	uint64_t physical)
{
	hal_physaddr_t table_physical;
	uint64_t *table;
	unsigned index;
	bool whole;
	int error;

	/* A gigapage that is all mappable RAM is one leaf. */
	whole = range_is_mappable_ram(physical, RISCV64_GIGAPAGE_SIZE);
	if (whole) {
		kernel_root[root_index] = leaf_entry(physical, KERNEL_LEAF | RISCV64_PTE_R | RISCV64_PTE_W);
		return;
	}

	/* Otherwise a table of megapages. */
	error = alloc_table(&table, &table_physical);
	if (error != HAL_OK)
		HAL_FATAL("riscv64 direct map table allocation failed");

	/* Maps each megapage of the gigapage as far as it can be. */
	for (index = 0; index < RISCV64_TABLE_ENTRIES; index++)
		build_direct_megapage(table, index, physical + (uint64_t)index * RISCV64_MEGAPAGE_SIZE);

	/* Links the table of megapages into the root's entry. */
	kernel_root[root_index] = pointer_entry(table_physical);
}

/* Maps one megapage of the direct map, as a leaf, through a table, or not at all. */
static void
build_direct_megapage(
	uint64_t *table,
	unsigned index,
	uint64_t physical)
{
	hal_physaddr_t pages_physical;
	uint64_t *pages;
	uint64_t page;
	unsigned page_index;
	bool whole;
	bool touches;
	bool mappable;
	int error;

	/* A megapage without RAM stays unmapped. */
	touches = riscv64_range_touches_ram(physical, RISCV64_MEGAPAGE_SIZE);
	if (!touches)
		return;

	/* A megapage that is all mappable RAM is one leaf. */
	whole = range_is_mappable_ram(physical, RISCV64_MEGAPAGE_SIZE);
	if (whole) {
		table[index] = leaf_entry(physical, KERNEL_LEAF | RISCV64_PTE_R | RISCV64_PTE_W);
		return;
	}

	/* Otherwise a table of pages. */
	error = alloc_table(&pages, &pages_physical);
	if (error != HAL_OK)
		HAL_FATAL("riscv64 direct map table allocation failed");

	/* Maps each page of the megapage that is mappable RAM. */
	for (page_index = 0; page_index < RISCV64_TABLE_ENTRIES; page_index++) {
		/* Maps the page only when it is mappable RAM. */
		page = physical + (uint64_t)page_index * RISCV64_PAGE_SIZE;
		mappable = range_is_mappable_ram(page, RISCV64_PAGE_SIZE);
		if (mappable)
			pages[page_index] = leaf_entry(page, KERNEL_LEAF | RISCV64_PTE_R | RISCV64_PTE_W);
	}

	/* Links the page table into the root's entry. */
	table[index] = pointer_entry(pages_physical);
}

/*
 * Builds the device window: an empty table below each of its root
 * entries, which device mappings fill.
 */
static void
build_device_window(
	void)
{
	hal_physaddr_t physical;
	unsigned index;
	int error;

	/* One table per root entry of the window. */
	for (index = 0; index < RISCV64_DEVICE_ROOT_COUNT; index++) {
		/* Allocates the empty table. */
		error = alloc_table(&device_tables[index], &physical);
		if (error != HAL_OK)
			HAL_FATAL("riscv64 device window table allocation failed");

		/* Links it into the window's root entry. */
		kernel_root[RISCV64_DEVICE_ROOT_FIRST + index] = pointer_entry(physical);
	}
}

/*
 * Builds the kernel image's window, page by page: text read-execute,
 * read-only data read-only, the rest read-write.
 */
static void
build_kernel_window(
	void)
{
	hal_physaddr_t table_physical;
	hal_physaddr_t pages_physical;
	uint64_t *table;
	uint64_t *pages;
	uintptr_t address;
	uintptr_t end;
	uint64_t flags;
	uint64_t page_physical;
	unsigned megapage;
	unsigned page;
	int error;

	/* The second-level table below the window's root entry. */
	error = alloc_table(&table, &table_physical);
	if (error != HAL_OK)
		HAL_FATAL("riscv64 kernel window table allocation failed");

	/* Links it into the window's root entry. */
	kernel_root[RISCV64_KERNEL_ROOT_INDEX] = pointer_entry(table_physical);

	/* Maps each page of the image through a table per megapage. */
	end = ((uintptr_t)__kernel_vma_end + RISCV64_PAGE_SIZE - 1U) & ~(uintptr_t)(RISCV64_PAGE_SIZE - 1U);
	pages = NULL;
	for (address = (uintptr_t)RISCV64_KERNEL_VIRT_BASE; address < end; address += RISCV64_PAGE_SIZE) {
		megapage = (unsigned)((address - (uintptr_t)RISCV64_KERNEL_VIRT_BASE) / RISCV64_MEGAPAGE_SIZE);
		page = (unsigned)(((address - (uintptr_t)RISCV64_KERNEL_VIRT_BASE) % RISCV64_MEGAPAGE_SIZE) / RISCV64_PAGE_SIZE);

		/* A new megapage gets its table of pages. */
		if (page == 0) {
			error = alloc_table(&pages, &pages_physical);
			if (error != HAL_OK)
				HAL_FATAL("riscv64 kernel window table allocation failed");

			/* Links the table into the megapage's entry. */
			table[megapage] = pointer_entry(pages_physical);
		}

		/* Chooses the page's permissions by the section it lies in. */
		flags = KERNEL_LEAF | RISCV64_PTE_R | RISCV64_PTE_W;
		if (address >= (uintptr_t)__kernel_text_start && address < (uintptr_t)__kernel_text_end) {
			flags = KERNEL_LEAF | RISCV64_PTE_R | RISCV64_PTE_X;
		} else if (address >= (uintptr_t)__kernel_rodata_start && address < (uintptr_t)__kernel_rodata_end) {
			flags = KERNEL_LEAF | RISCV64_PTE_R;
		}

		/* Maps the page, where the image was loaded, with those permissions. */
		page_physical = riscv64_virt_to_phys((const void *)address);
		pages[page] = leaf_entry(page_physical, flags);
	}
}

/* Reports the flags of the kernel window's leaf for an image address. */
static uint64_t
kernel_page_flags(
	uintptr_t address)
{
	const uint64_t *table;
	const uint64_t *pages;
	uint64_t entry;
	unsigned megapage;
	unsigned page;

	/* Walks the window's two tables below the root entry. */
	entry = kernel_root[RISCV64_KERNEL_ROOT_INDEX];
	table = riscv64_phys_to_direct((entry & PTE_PPN_MASK) >> RISCV64_PTE_PPN_SHIFT << RISCV64_PAGE_SHIFT);
	megapage = (unsigned)((address - (uintptr_t)RISCV64_KERNEL_VIRT_BASE) / RISCV64_MEGAPAGE_SIZE);
	page = (unsigned)(((address - (uintptr_t)RISCV64_KERNEL_VIRT_BASE) % RISCV64_MEGAPAGE_SIZE) / RISCV64_PAGE_SIZE);
	entry = table[megapage];
	pages = riscv64_phys_to_direct((entry & PTE_PPN_MASK) >> RISCV64_PTE_PPN_SHIFT << RISCV64_PAGE_SHIFT);

	/* Succeeded: the leaf's flags. */
	return pages[page] & ~PTE_PPN_MASK;
}

/*
 * Builds the early console's window of the screen below its root entry:
 * a megapage leaf where the screen's pages fill the megapage, and a table
 * of only its pages where they do not, so that the window reaches the
 * screen and nothing beside it.  A screen the window cannot reach stays
 * drawn where the board drew it, and is never given.
 */
static void
build_fb_window(
	void)
{
	hal_physaddr_t table_physical;
	hal_physaddr_t pages_physical;
	uint64_t *table;
	uint64_t *pages;
	uint64_t physical;
	uint64_t size;
	uint64_t aligned;
	uint64_t end;
	uint64_t block;
	unsigned index;
	int present;
	int error;

	/* Nothing to hold without a screen the early console draws on. */
	fb_window.state = RISCV64_FB_WINDOW_NONE;
	present = riscv64_board_screen(&physical, &size);
	if (!present || size == 0)
		return;

	/* Refuses a screen that wraps or that one gigabyte of window does not hold. */
	if (physical > ~0ULL - size - RISCV64_PAGE_SIZE)
		return;
	aligned = physical & ~((uint64_t)RISCV64_MEGAPAGE_SIZE - 1U);
	end = (physical + size + RISCV64_PAGE_SIZE - 1U) & ~((uint64_t)RISCV64_PAGE_SIZE - 1U);
	if (end - aligned > RISCV64_GIGAPAGE_SIZE)
		return;

	/* The second-level table below the window's root entry. */
	error = alloc_table(&table, &table_physical);
	if (error != HAL_OK)
		HAL_FATAL("riscv64 screen window table allocation failed");

	/* Links it into the window's root entry. */
	kernel_root[RISCV64_FB_WINDOW_ROOT_INDEX] = pointer_entry(table_physical);

	/* Each megapage the screen touches, read-write and never executable. */
	for (block = aligned; block < end; block += RISCV64_MEGAPAGE_SIZE) {
		index = (unsigned)((block - aligned) / RISCV64_MEGAPAGE_SIZE);

		/* A megapage the screen fills is one leaf. */
		if (physical <= block && end - block >= RISCV64_MEGAPAGE_SIZE) {
			table[index] = leaf_entry(block, KERNEL_LEAF | RISCV64_PTE_R | RISCV64_PTE_W);
			continue;
		}

		/* A megapage it fills partly gets a table of its pages alone. */
		error = alloc_table(&pages, &pages_physical);
		if (error != HAL_OK)
			HAL_FATAL("riscv64 screen window table allocation failed");
		(void)riscv64_window_fill_pages(pages, block, physical, size, KERNEL_LEAF | RISCV64_PTE_R | RISCV64_PTE_W);
		table[index] = pointer_entry(pages_physical);
	}

	/* The early console holds it, and draws through it until the kernel prints. */
	fb_window.physical = physical;
	fb_window.size = size;
	fb_window.address = (uintptr_t)(RISCV64_FB_WINDOW_BASE + (physical - aligned));
	fb_window.table = table;
	fb_window.state = RISCV64_FB_WINDOW_HELD;
}

/*
 * Gives the early console's window to the kernel's console: the first
 * mapping request of exactly the screen (its physical base and size,
 * uncached and writable) once the kernel prints through kernel_putc, when
 * the early console draws no more.  Returns 1 with address set, or 0 for
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

	/* Only once the kernel prints, so that the early console no longer draws. */
	printer = __atomic_load_n(&kernel_putc, __ATOMIC_ACQUIRE);
	if (printer == NULL)
		return 0;

	/* Gives only the screen itself, exactly, while the early console holds it. */
	enabled = riscv64_spin_lock(&kernel_table_lock);

	given = 0;
	if (fb_window.state == RISCV64_FB_WINDOW_HELD &&
	    fb_window.physical == (uint64_t)physical &&
	    fb_window.size == (uint64_t)size) {
		/* GIVEN: the one holder's release removes it; no other request gets it. */
		fb_window.state = RISCV64_FB_WINDOW_GIVEN;
		*address = (void *)fb_window.address;
		given = 1;
	}

	riscv64_spin_unlock(&kernel_table_lock, enabled);

	/* Reports whether the request took the window. */
	return given;
}

/*
 * Removes the early console's window once it was given and its holder
 * releases it (address and size as given): the early console forgets the
 * screen, the window's entries go, and every hart drops every translation
 * of it.  The root entry stays, as every root entry of the kernel's half
 * does, pointing at an empty table.  matched says whether the release was
 * of the window.
 */
static int
fb_window_remove(
	void *address,
	size_t size,
	int *matched)
{
	unsigned index;
	bool enabled;

	/* Takes only a release of the window as it was given. */
	enabled = riscv64_spin_lock(&kernel_table_lock);

	*matched = 0;
	if (fb_window.state != RISCV64_FB_WINDOW_GIVEN ||
	    fb_window.address != (uintptr_t)address ||
	    fb_window.size != (uint64_t)size) {
		riscv64_spin_unlock(&kernel_table_lock, enabled);
		return HAL_OK;
	}

	/* REMOVED: the window is the remover's alone from here. */
	*matched = 1;
	fb_window.state = RISCV64_FB_WINDOW_REMOVED;

	riscv64_spin_unlock(&kernel_table_lock, enabled);

	/* The early console forgets the screen first. */
	riscv64_cons_detach_screen();

	/* Clears every entry below the root entry, then every hart drops its translations. */
	enabled = riscv64_spin_lock(&kernel_table_lock);

	for (index = 0; index < RISCV64_TABLE_ENTRIES; index++)
		__atomic_store_n(&fb_window.table[index], 0U, __ATOMIC_RELEASE);
	riscv64_tlb_flush_everywhere();

	riscv64_spin_unlock(&kernel_table_lock, enabled);

	/* Succeeded: the window is gone. */
	return HAL_OK;
}

/*
 * Maps the devices the HAL itself reaches: the interrupt controller, the
 * firmware configuration interface, the UART, the cache controller and a
 * screen the firmware left on that the direct map does not hold, which the
 * early console draws on through the device window.
 */
static void
map_hal_devices(
	void)
{
	const struct riscv64_fdt_info *info;
	const struct riscv64_framebuffer *framebuffer;
	bool direct;

	/* Each device the tree names, in the window the kernel's root now holds. */
	info = riscv64_boot_fdt();

	/* The interrupt controller. */
	if (info->plic_size != 0)
		map_device_blocks(info->plic_base, info->plic_size);

	/* The firmware configuration interface. */
	if (info->fwcfg_size != 0)
		map_device_blocks(info->fwcfg_base, info->fwcfg_size);

	/* The UART. */
	if (info->uart_size != 0)
		map_device_blocks(info->uart_base, info->uart_size);

	/* The cache controller. */
	if (info->ccache_size != 0)
		map_device_blocks(info->ccache_base, info->ccache_size);

	/* Without a screen there is nothing more to map. */
	framebuffer = riscv64_boot_framebuffer();
	if (framebuffer == NULL)
		return;

	/* A screen the direct map holds is drawn through it, and one the early console's window holds through that. */
	direct = riscv64_space_in_direct_map(framebuffer->phys, framebuffer->size);
	if (direct)
		return;
	if (fb_window.state == RISCV64_FB_WINDOW_HELD)
		return;

	/* Reaches any other screen, device memory or no-map RAM, through the device window. */
	map_device_blocks(framebuffer->phys, framebuffer->size);
}

/* Writes the device window's leaves for every megapage a range touches. */
static void
map_device_blocks(
	uint64_t physical,
	uint64_t size)
{
	uint64_t *table;
	uint64_t block;
	uint64_t end;
	uint64_t entry;
	unsigned root;
	unsigned index;

	/* Ignores a range outside the window, which the caller refused already. */
	if (physical >= RISCV64_DEVICE_LIMIT || size > RISCV64_DEVICE_LIMIT - physical)
		return;

	/* A read-write, never-executable leaf for each megapage. */
	end = physical + size;
	for (block = physical & ~((uint64_t)RISCV64_MEGAPAGE_SIZE - 1U); block < end; block += RISCV64_MEGAPAGE_SIZE) {
		root = (unsigned)(block >> RISCV64_GIGAPAGE_SHIFT);
		index = (unsigned)((block % RISCV64_GIGAPAGE_SIZE) / RISCV64_MEGAPAGE_SIZE);

		/* A megapage that holds page leaves gets all of them, which is the whole megapage. */
		entry = device_tables[root][index];
		if ((entry & RISCV64_PTE_V) != 0 && (entry & PERMISSIONS) == 0) {
			table = riscv64_phys_to_direct((entry & PTE_PPN_MASK) >> RISCV64_PTE_PPN_SHIFT << RISCV64_PAGE_SHIFT);
			(void)riscv64_window_fill_pages(table, block, block, RISCV64_MEGAPAGE_SIZE, KERNEL_LEAF | RISCV64_PTE_R | RISCV64_PTE_W);
			continue;
		}

		/* Any other megapage becomes one leaf. */
		device_tables[root][index] = leaf_entry(block, KERNEL_LEAF | RISCV64_PTE_R | RISCV64_PTE_W);
	}
}

/* Reports the hart mask of every hart that is up, except the caller's. */
static unsigned long
other_ready_harts(
	void)
{
	struct riscv64_percpu *self;
	struct riscv64_percpu *record;
	unsigned long mask;
	unsigned ready;
	hal_cpu_id_t cpu;

	/* Collects each ready hart's bit by its hart id. */
	self = riscv64_percpu_current();
	mask = 0;
	for (cpu = 0; cpu < RISCV64_CPU_MAX; cpu++) {
		/* The caller's own hart is left out. */
		record = riscv64_percpu_get(cpu);
		if (record == self)
			continue;

		/* A hart that is not up is left out. */
		ready = __atomic_load_n(&record->ready, __ATOMIC_ACQUIRE);
		if (ready == 0)
			continue;

		/* Adds the hart by its hart id. */
		mask |= 1UL << record->hart_id;
	}

	/* Succeeded: the harts to reach through the firmware. */
	return mask;
}

/*
 * Reports the hart mask of the other harts that have a space selected,
 * and whether the caller's hart has.
 *
 * The full barrier orders the caller's table stores before the reads, so a
 * hart that selects the space after the reads walks the new tables.
 */
static unsigned long
harts_selecting(
	hal_space_t handle,
	bool *self)
{
	struct riscv64_percpu *current;
	struct riscv64_percpu *record;
	hal_space_t selected;
	unsigned long mask;
	unsigned ready;
	hal_cpu_id_t cpu;

	/* Orders the table stores before the reads of the selections. */
	hal_mb();

	/* Looks at every hart's selection. */
	current = riscv64_percpu_current();
	*self = false;
	mask = 0;
	for (cpu = 0; cpu < RISCV64_CPU_MAX; cpu++) {
		/* Skips a hart that has another space selected. */
		record = riscv64_percpu_get(cpu);
		selected = __atomic_load_n(&record->current_space, __ATOMIC_ACQUIRE);
		if (selected != handle)
			continue;

		/* The calling hart drops its own translations without an interrupt. */
		if (record == current) {
			*self = true;
			continue;
		}

		/* Adds a hart that is up to the ones to interrupt. */
		ready = __atomic_load_n(&record->ready, __ATOMIC_ACQUIRE);
		if (ready != 0)
			mask |= 1UL << record->hart_id;
	}

	/* Succeeded: the other harts that hold the space's translations. */
	return mask;
}

/*
 * Drops the stale translations of a space after its tables changed.
 *
 * The kernel's half is cached by every hart; a user space by the harts
 * that have it selected.  A space selected nowhere has nothing cached,
 * since a hart flushes its TLB whenever it selects a space.
 */
static void
flush_space(
	hal_space_t handle)
{
	unsigned long others;
	bool self;

	/* The kernel's half is everywhere. */
	if (handle == HAL_SPACE_SYS) {
		riscv64_tlb_flush_everywhere();
		return;
	}

	/* A user space where it is selected. */
	others = harts_selecting(handle, &self);
	if (self)
		riscv64_sfence_vma_all();

	/* The other harts that have it selected, through the firmware. */
	if (others != 0)
		(void)riscv64_sbi_remote_sfence_vma(others, 0);
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
	struct riscv64_space **result,
	bool *irq_enabled)
{
	struct riscv64_space *space;
	unsigned me;
	bool enabled;
	bool acquired;

	/* Looks the handle up among the live spaces. */
	enabled = riscv64_spin_lock(&space_registry_lock);

	/* Walks the registry to the space the handle names, or to its end. */
	for (space = space_registry; space != NULL; space = space->registry_next) {
		if ((hal_space_t)space == handle)
			break;
	}

	/* A handle that names no live space is refused. */
	if (space == NULL || space->destroying) {
		riscv64_spin_unlock(&space_registry_lock, enabled);
		return 0;
	}

	/* A second operation on the same space from the same hart is a fault. */
	me = hal_cpu_current() + 1U;
	if (space->lock_owner == me)
		HAL_FATAL("recursive riscv64 space operation");

	/* Waits for another hart's operation on the space to end. */
	for (;;) {
		/* Leaves the wait once the lock is taken. */
		acquired = hal_atomic_uint_try_acquire(&space->lock);
		if (acquired)
			break;

		/* Waits a moment before trying again. */
		hal_atomic_relax();
	}

	/* Records the hart that holds the space's lock. */
	space->lock_owner = me;

	/* Leaves the registry with interrupts still off for the space's operation. */
	__atomic_store_n(&space_registry_lock, 0U, __ATOMIC_RELEASE);

	/* Hands the space and the interrupt state to the caller. */
	*result = space;
	*irq_enabled = enabled;

	/* Succeeded: the caller holds the space. */
	return 1;
}

/* Releases a space taken by space_lock_handle() and restores interrupts. */
static void
space_unlock(
	struct riscv64_space *space,
	bool enabled)
{
	unsigned me;

	/* Only the hart that holds the space may release it. */
	me = hal_cpu_current() + 1U;
	if (space->lock == 0 || space->lock_owner != me)
		HAL_FATAL("invalid riscv64 space unlock");

	/* Releases the space, publishing every change made under it. */
	space->lock_owner = 0;
	riscv64_spin_unlock(&space->lock, enabled);
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
	if ((address & (RISCV64_PAGE_SIZE - 1U)) != 0)
		return 0;
	if ((size & (RISCV64_PAGE_SIZE - 1U)) != 0)
		return 0;

	/* The first page is never mapped, and the range must end inside the user half. */
	if (address < RISCV64_USER_MINIMUM)
		return 0;
	if (address >= RISCV64_USER_LIMIT)
		return 0;
	if (size > RISCV64_USER_LIMIT - address)
		return 0;

	/* Succeeded: the range can be mapped. */
	return 1;
}

/*
 * Checks a user mapping's attributes: some access, never written and
 * executed at once, and no cache behaviour or device class the tables
 * cannot express on this port.
 */
static int
valid_attributes(
	uint32_t attr)
{
	uint64_t offset;

	/* A mapping must allow some access. */
	if ((attr & (HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC)) == 0)
		return HAL_ERR_INVALID;

	/* A page is never both writable and executable. */
	if ((attr & HAL_SPACE_WRITE) != 0 && (attr & HAL_SPACE_EXEC) != 0)
		return HAL_ERR_INVALID;

	/* The cache behaviour of memory is the platform's, and device pages are not offered to users. */
	if ((attr & (HAL_SPACE_WRITETHRU | HAL_SPACE_WC | HAL_SPACE_DEVICE)) != 0)
		return HAL_ERR_UNSUPPORTED;

	/* Uncached memory is the board's alias, which only some boards have and nothing runs from. */
	if ((attr & HAL_SPACE_NOCACHE) != 0) {
		offset = riscv64_board_uncached_offset();
		if (offset == 0)
			return HAL_ERR_UNSUPPORTED;
		if ((attr & HAL_SPACE_EXEC) != 0)
			return HAL_ERR_UNSUPPORTED;
	}

	/* Succeeded: the attributes can be represented. */
	return HAL_OK;
}

/* Allocates a zeroed page-table page and records it as the space's. */
static struct riscv64_table_page *
allocate_table(
	struct riscv64_space *space,
	uint64_t *parent,
	unsigned index)
{
	struct riscv64_table_page *page;
	uint64_t *table;
	int error;

	/* Allocates the record of the table. */
	page = kernel_alloc(sizeof(*page));
	if (page == NULL)
		return NULL;

	/* Allocates the table itself. */
	error = hal_pmem_alloc(RISCV64_PAGE_SIZE, RISCV64_PAGE_SIZE, &page->memory);
	if (error != HAL_OK) {
		kernel_free(page);
		return NULL;
	}

	/* Clears the table through the direct map. */
	table = hal_pmem_to_kernel(page->memory);
	hal_memset(table, 0, RISCV64_PAGE_SIZE);

	/* Remembers the entry that will point at the table. */
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
 * Finds the last-level entry of a user address, making the tables on the
 * way when asked to.  Reports NULL when a table is missing and not to be
 * made, cannot be made, or a leaf sits where a table should be.
 */
static uint64_t *
walk_leaf(
	struct riscv64_space *space,
	uintptr_t address,
	int create)
{
	static const unsigned shifts[2] = {30, 21};
	struct riscv64_table_page *page;
	uint64_t *table;
	uint64_t entry;
	unsigned level;
	unsigned index;

	/* Descends from the root to the last-level table. */
	table = space->root;
	for (level = 0; level < 2U; level++) {
		index = (unsigned)(address >> shifts[level]) & (RISCV64_TABLE_ENTRIES - 1U);
		entry = table[index];

		/* A missing table is made only when asked to. */
		if ((entry & RISCV64_PTE_V) == 0) {
			/* A walk that only looks makes nothing. */
			if (!create)
				return NULL;

			/* Allocates the table; without memory the walk fails. */
			page = allocate_table(space, table, index);
			if (page == NULL)
				return NULL;

			/* Links the new table into its parent's entry. */
			entry = pointer_entry(page->memory);
			table[index] = entry;
		}

		/* A leaf where a table should be ends the walk. */
		if ((entry & PERMISSIONS) != 0)
			return NULL;

		/* Steps down into the table the entry points at. */
		table = riscv64_phys_to_direct((entry & PTE_PPN_MASK) >> RISCV64_PTE_PPN_SHIFT << RISCV64_PAGE_SHIFT);
	}

	/* Succeeded: the entry of the page. */
	return &table[(address >> RISCV64_PAGE_SHIFT) & (RISCV64_TABLE_ENTRIES - 1U)];
}

/* Reports whether a table has no valid entry. */
static int
table_empty(
	const uint64_t *table)
{
	unsigned index;

	/* Looks for any valid entry. */
	for (index = 0; index < RISCV64_TABLE_ENTRIES; index++) {
		if ((table[index] & RISCV64_PTE_V) != 0)
			return 0;
	}

	/* The table is empty. */
	return 1;
}

/*
 * Unlinks every empty table of a space, repeating until none is left
 * whose children all went, and reports them for freeing after the flush.
 */
static struct riscv64_table_page *
detach_empty_tables(
	struct riscv64_space *space)
{
	struct riscv64_table_page *detached;
	struct riscv64_table_page **link;
	struct riscv64_table_page *page;
	const uint64_t *table;
	uint64_t parent_entry;
	uint64_t expected_entry;
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
			table = hal_pmem_to_kernel(page->memory);
			empty = table_empty(table);
			if (!empty) {
				link = &page->next;
				continue;
			}

			/* The parent entry must point at this very table. */
			parent_entry = page->parent[page->parent_index];
			expected_entry = pointer_entry(page->memory);
			if ((parent_entry & RISCV64_PTE_V) == 0 ||
			    expected_entry != parent_entry)
				HAL_FATAL("detaching an unlinked riscv64 page table");

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
	struct riscv64_table_page *page)
{
	struct riscv64_table_page *next;

	/* Frees each table and its record, and stops counting it. */
	while (page != NULL) {
		next = page->next;
		(void)hal_pmem_free(&page->memory, RISCV64_PAGE_SIZE);
		kernel_free(page);
		(void)__atomic_fetch_sub(&page_table_count, 1U, __ATOMIC_RELAXED);
		page = next;
	}
}

/*
 * Chooses a user leaf's flags: valid, user, accessed, and the access the
 * attributes allow; a writable page is born dirty, in both the hardware's
 * bit and the kernel's record.
 */
static uint64_t
user_leaf_flags(
	uint32_t attr)
{
	uint64_t flags;

	/* Every user leaf is valid, the user's and already accessed. */
	flags = RISCV64_PTE_V | RISCV64_PTE_U | RISCV64_PTE_A;

	/* A readable page; a writable or executable one is readable too. */
	flags |= RISCV64_PTE_R;

	/* A writable page is born dirty, in the hardware's bit and the kernel's record. */
	if ((attr & HAL_SPACE_WRITE) != 0)
		flags |= RISCV64_PTE_W | RISCV64_PTE_D | RISCV64_PTE_SW_DIRTY;

	/* An executable page. */
	if ((attr & HAL_SPACE_EXEC) != 0)
		flags |= RISCV64_PTE_X;

	/* Succeeded: the leaf's flags. */
	return flags;
}

/*
 * Makes code written through the data side visible to instruction fetch
 * before a user page becomes executable.
 */
static void
sync_executable(
	uint64_t physical,
	size_t size,
	uint32_t attr)
{
	void *direct;

	/* Only memory that is to be executed. */
	if ((attr & HAL_SPACE_EXEC) == 0)
		return;

	/* Its bytes through the kernel's direct map. */
	direct = riscv64_phys_to_direct(physical);
	hal_sync_instruction_stream(direct, size);
}

/* Returns a kernel table page that was never linked. */
static void
free_table(
	hal_physaddr_t physical)
{
	hal_physaddr_t block;

	/* Gives the page back and stops counting it. */
	block = physical;
	(void)hal_pmem_free(&block, RISCV64_PAGE_SIZE);
	(void)__atomic_fetch_sub(&page_table_count, 1U, __ATOMIC_RELAXED);
}

/* Reports whether a user leaf points at the board's uncached alias rather than RAM. */
static bool
leaf_is_uncached(
	uint64_t entry)
{
	uint64_t physical;
	bool ram;

	/* The page the leaf points at; RAM is reached cached, the alias is not RAM. */
	physical = (entry & PTE_PPN_MASK) >> RISCV64_PTE_PPN_SHIFT << RISCV64_PAGE_SHIFT;
	ram = riscv64_range_is_ram(physical, RISCV64_PAGE_SIZE);
	if (ram)
		return false;

	/* Succeeded: the leaf reaches the alias. */
	return true;
}
