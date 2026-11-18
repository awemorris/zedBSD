/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv32 address spaces: the kernel's half and the user spaces.
 *
 * Sv32 has one root table per space, so the kernel's half is not a table
 * of its own as on other ports: each user space's root carries a copy of
 * the kernel's root entries 512 to 1023.  The kernel's half is therefore
 * built completely at boot, every table below those entries included, and
 * the root entries are never changed again; afterwards only the tables
 * below them change, which every copy shares.
 *
 * The kernel's half holds the direct map of RAM (without the ranges the
 * firmware reserved as no-map), with the kernel image in it page by page
 * so that its text alone is executable, and the device window, mapped
 * whole at boot in megapages, since a root entry made later would not
 * reach the copies in the user spaces.  The cache behaviour of memory is the platform's
 * and is not chosen in the tables.
 */

#include <hal/hal.h>
#include "asm.h"
#include "bootinfo.h"
#include "defs.h"
#include "page.h"
#include "percpu.h"
#include "sbi.h"
#include "space.h"
#include "spin.h"

/* The flags of every kernel leaf: valid, global, already accessed and written. */
#define KERNEL_LEAF		(RISCV32_PTE_V | RISCV32_PTE_G | RISCV32_PTE_A | RISCV32_PTE_D)

/* The flags of a pointer to the next table: valid and nothing else. */
#define TABLE_POINTER		RISCV32_PTE_V

/* The permission bits; a valid entry without any of them points at a table. */
#define PERMISSIONS		(RISCV32_PTE_R | RISCV32_PTE_W | RISCV32_PTE_X)

/* The physical page number field of an entry. */
#define PTE_PPN_MASK		0xfffffc00U

/* The bounds of the kernel image's sections, from the linker script. */
extern char __kernel_vma_start[];
extern char __kernel_text_start[];
extern char __kernel_text_end[];
extern char __kernel_rodata_start[];
extern char __kernel_rodata_end[];
extern char __kernel_vma_end[];

/* The boot table locore.S built and runs on until the kernel's own. */
extern uint32_t riscv32_boot_root[];

/*
 * The physical address the kernel image was loaded at.
 *
 * Set by riscv32_kernel_image_init() as the boot hart's first act and
 * constant afterwards; every translation of an image address uses it.
 */
static uint64_t kernel_phys_start;

/*
 * The kernel's root table, which every user space's root copies the upper
 * half of, and which is selected alone when no user space is.
 *
 * Its upper half is written by riscv32_space_init() and never again:
 * kernel_root_sealed says so from then on.
 */
static uint32_t kernel_root[RISCV32_TABLE_ENTRIES] __attribute__((aligned(RISCV32_PAGE_SIZE)));
static int kernel_root_sealed;

/*
 * Set once riscv32_space_init() has switched the boot hart to the kernel's
 * root and found the image's sections with the permissions they need; the
 * boot log's closing line reports it.  Written once by the boot hart.
 */
static int kernel_mapping_checked;

/*
 * Every user space that exists, the number the next one gets and how many
 * there are.  Kept under space_registry_lock; a space is looked up here
 * before its own lock is taken, so a destroyed space is never locked.
 */
static struct riscv32_space *space_registry;
static int next_space_id = 1;
static uint32_t space_count;

/* Guards space_registry, next_space_id and space_count; zero when free. */
static volatile unsigned space_registry_lock;

/*
 * How many page-table pages the user spaces and the kernel's half hold,
 * for the memory statistics.  Changed with atomic adds, since each space's
 * tables are changed under that space's own lock.
 */
static uint32_t page_table_count;

static int alloc_table(uint32_t **table, hal_physaddr_t *physical);
static uint32_t pointer_entry(hal_physaddr_t physical);
static uint32_t leaf_entry(uint64_t physical, uint32_t flags);
static bool range_is_nomap(uint64_t physical, uint64_t size);
static bool range_is_mappable_ram(uint64_t physical, uint64_t size);
static void build_direct_map(void);
static void build_direct_megapage(unsigned root_index, uint64_t physical);
static void build_device_window(void);
static void build_kernel_window(void);
static uint32_t kernel_page_flags(uintptr_t address);
static unsigned long other_ready_harts(void);
static unsigned long harts_selecting(hal_space_t handle, bool *self);
static void flush_space(hal_space_t handle);
static int space_lock_handle(hal_space_t handle, struct riscv32_space **result, bool *irq_enabled);
static void space_unlock(struct riscv32_space *space, bool enabled);
static int valid_user(uintptr_t address, size_t size);
static int valid_attributes(uint32_t attr);
static struct riscv32_table_page *allocate_table(struct riscv32_space *space, uint32_t *parent, unsigned index);
static uint32_t *walk_leaf(struct riscv32_space *space, uintptr_t address, int create);
static int table_empty(const uint32_t *table);
static struct riscv32_table_page *detach_empty_tables(struct riscv32_space *space);
static void free_detached_tables(struct riscv32_table_page *page);
static uint32_t user_leaf_flags(uint32_t attr);
static void sync_executable(uint64_t physical, size_t size, uint32_t attr);

/*
 * Records where the kernel image was loaded.
 */
void
riscv32_kernel_image_init(
	uint64_t phys_start)
{
	/* Every image address translates through this from now on. */
	kernel_phys_start = phys_start;
}

/*
 * Reports the physical address the kernel image was loaded at.
 */
uint64_t
riscv32_kernel_phys_start(
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
riscv32_virt_to_phys(
	const void *address)
{
	uintptr_t value;

	/* The image and the direct map hold RAM at its own addresses. */
	value = (uintptr_t)address;
	if (value >= (uintptr_t)RISCV32_DIRECT_BASE && value < (uintptr_t)RISCV32_DIRECT_LIMIT)
		return (uint64_t)value;

	/* Any other address is not the kernel's to translate. */
	HAL_FATAL("riscv32 address outside the image and the direct map");
	return 0;
}

/*
 * Reports the direct-map address of a physical address.
 */
void *
riscv32_phys_to_direct(
	uint64_t physical)
{
	/* The direct map holds RAM at its own addresses. */
	return (void *)(uintptr_t)physical;
}

/*
 * Reports the satp value of the boot table, which a secondary hart starts
 * on: it maps the image both where it was loaded and where it is linked.
 */
uint64_t
riscv32_boot_satp(
	void)
{
	uint64_t physical;

	/* The boot table is in the image. */
	physical = riscv32_virt_to_phys(riscv32_boot_root);

	/* Succeeded: Sv32 with the table's page number. */
	return RISCV32_SATP_MODE_SV32 | (physical >> RISCV32_PAGE_SHIFT);
}

/*
 * Builds the kernel's half and switches the boot hart to it.
 *
 * Every table below the kernel's root entries is made here, so that the
 * entries never change again and the copies in the user spaces stay right.
 */
void
riscv32_space_init(
	void)
{
	uint32_t text_page;
	uint32_t rodata_page;
	uint32_t data_page;
	unsigned long satp;

	/* Starts from an empty root; the user half stays empty for good. */
	hal_memset(kernel_root, 0, sizeof(kernel_root));
	space_registry = NULL;

	/* Builds the two windows the kernel uses, then the image's pages in the first. */
	build_direct_map();
	build_device_window();
	build_kernel_window();

	/* No root entry changes from here on. */
	kernel_root_sealed = 1;

	/* Switches to the kernel's own root, with the kernel alone selected. */
	satp = RISCV32_SATP_MODE_SV32 | (unsigned long)(riscv32_virt_to_phys(kernel_root) >> RISCV32_PAGE_SHIFT);
	riscv32_write_satp(satp);
	riscv32_percpu_current()->current_space = HAL_SPACE_SYS;

	/* Checks W^X on the three kinds of section the kernel runs from. */
	text_page = kernel_page_flags((uintptr_t)__kernel_text_start);
	rodata_page = kernel_page_flags((uintptr_t)__kernel_rodata_start);
	data_page = kernel_page_flags((uintptr_t)&kernel_root_sealed);
	if ((text_page & RISCV32_PTE_X) == 0 || (text_page & RISCV32_PTE_W) != 0)
		HAL_FATAL("riscv32 kernel text mapping is not read-execute");
	if ((rodata_page & (RISCV32_PTE_X | RISCV32_PTE_W)) != 0)
		HAL_FATAL("riscv32 kernel rodata mapping is not read-only");
	if ((data_page & RISCV32_PTE_X) != 0)
		HAL_FATAL("riscv32 kernel data mapping is executable");

	/* Records the passed checks for the closing line and reports them now. */
	kernel_mapping_checked = 1;
	hal_puts("RISCV32 PAGING PASS\nRISCV32 W^X PASS\n");
}

/*
 * Reports whether the kernel's tables were switched to and found right.
 */
bool
riscv32_space_checked(
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
 * after riscv32_space_init(); it arrives on the boot table.
 */
void
riscv32_space_select_system(
	void)
{
	struct riscv32_percpu *cpu;
	unsigned long satp;

	/* Publishes the selection, then installs the kernel's root. */
	cpu = riscv32_percpu_current();
	__atomic_store_n(&cpu->current_space, HAL_SPACE_SYS, __ATOMIC_RELEASE);
	hal_mb();
	satp = RISCV32_SATP_MODE_SV32 | (unsigned long)(riscv32_virt_to_phys(kernel_root) >> RISCV32_PAGE_SHIFT);
	riscv32_write_satp(satp);
}

/*
 * Drops every translation every hart that is up holds, this one included.
 */
void
riscv32_tlb_flush_everywhere(
	void)
{
	unsigned long others;

	/* This hart's TLB, then the others' through the firmware. */
	riscv32_sfence_vma_all();
	others = other_ready_harts();
	if (others != 0)
		(void)riscv32_sbi_remote_sfence_vma(others, 0);
}

/*
 * Creates an empty user space.
 */
hal_space_t
hal_space_create(
	void)
{
	struct riscv32_space *space;
	unsigned index;
	bool enabled;
	int error;

	/* A user space copies the kernel's half, which must be complete. */
	if (!kernel_root_sealed)
		HAL_FATAL("riscv32 user space before the kernel half is built");

	/* Allocates the record. */
	space = kernel_alloc(sizeof(*space));
	if (space == NULL)
		return NULL;
	hal_memset(space, 0, sizeof(*space));

	/* Allocates the root table. */
	error = hal_pmem_alloc(RISCV32_PAGE_SIZE, RISCV32_PAGE_SIZE, &space->root_memory);
	if (error != HAL_OK) {
		kernel_free(space);
		return NULL;
	}

	/* Empties the user half and copies the kernel's. */
	space->root = hal_pmem_to_kernel(space->root_memory);
	for (index = 0; index < RISCV32_USER_ROOT_COUNT; index++)
		space->root[index] = 0;
	for (index = RISCV32_USER_ROOT_COUNT; index < RISCV32_TABLE_ENTRIES; index++)
		space->root[index] = kernel_root[index];
	space->magic = RISCV32_SPACE_MAGIC;

	/* Numbers the space and makes it live. */
	enabled = riscv32_spin_lock(&space_registry_lock);

	space->space_id = next_space_id++;
	space->registry_next = space_registry;
	space_registry = space;
	space_count++;

	riscv32_spin_unlock(&space_registry_lock, enabled);

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
	struct riscv32_space *space;
	struct riscv32_space **link;
	struct riscv32_table_page *page;
	unsigned long selecting;
	bool self;
	bool enabled;

	/* A missing space is nothing to destroy. */
	space = handle;
	if (space == NULL)
		return;

	/* Takes the space out of the registry, so nothing can lock it any more. */
	enabled = riscv32_spin_lock(&space_registry_lock);

	link = &space_registry;
	while (*link != NULL && *link != space)
		link = &(*link)->registry_next;
	if (*link == NULL || space->destroying)
		HAL_FATAL("invalid riscv32 space destroy");
	selecting = harts_selecting(handle, &self);
	if (selecting != 0 || self)
		HAL_FATAL("destroying an active riscv32 space");
	if (space->lock != 0)
		HAL_FATAL("destroying a busy riscv32 space");
	space->destroying = 1U;
	*link = space->registry_next;
	if (space_count != 0)
		space_count--;

	riscv32_spin_unlock(&space_registry_lock, enabled);

	/* Frees every table, then the root and the record. */
	while (space->tables != NULL) {
		page = space->tables;
		space->tables = page->next;
		(void)hal_pmem_free(&page->memory, RISCV32_PAGE_SIZE);
		kernel_free(page);
		(void)__atomic_fetch_sub(&page_table_count, 1U, __ATOMIC_RELAXED);
	}

	/* Frees the root and the record. */
	space->magic = 0;
	(void)hal_pmem_free(&space->root_memory, RISCV32_PAGE_SIZE);
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
	struct riscv32_percpu *cpu;
	struct riscv32_space *space;
	unsigned long satp;
	bool enabled;
	bool space_enabled;
	int locked;

	/*
	 * Names the hart with interrupts off, so that a caller outside a task
	 * switch cannot move to another hart between reading the record and
	 * installing the table.
	 */
	enabled = riscv32_irq_save();
	cpu = riscv32_percpu_current();

	/* The space already selected here needs nothing. */
	if (handle == cpu->current_space) {
		riscv32_irq_restore(enabled);
		return;
	}

	/* The kernel alone: its own root. */
	if (handle == HAL_SPACE_SYS) {
		__atomic_store_n(&cpu->current_space, handle, __ATOMIC_RELEASE);
		hal_mb();
		satp = RISCV32_SATP_MODE_SV32 | (unsigned long)(riscv32_virt_to_phys(kernel_root) >> RISCV32_PAGE_SHIFT);
		riscv32_write_satp(satp);
		riscv32_irq_restore(enabled);
		return;
	}

	/* A user space is held while its root is installed. */
	locked = space_lock_handle(handle, &space, &space_enabled);
	if (!locked)
		HAL_FATAL("invalid riscv32 space switch");

	__atomic_store_n(&cpu->current_space, handle, __ATOMIC_RELEASE);
	hal_mb();
	satp = RISCV32_SATP_MODE_SV32 | ((unsigned long)space->root_memory >> RISCV32_PAGE_SHIFT);
	riscv32_write_satp(satp);

	space_unlock(space, space_enabled);

	/* Restores the caller's interrupt state. */
	riscv32_irq_restore(enabled);
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
	struct riscv32_space *space;
	struct riscv32_table_page *detached;
	uint32_t *leaf;
	uintptr_t address;
	uintptr_t offset;
	uintptr_t rollback;
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
	if (((uint64_t)paddr & (RISCV32_PAGE_SIZE - 1U)) != 0)
		return HAL_ERR_INVALID;
	ram = riscv32_range_is_ram((uint64_t)paddr, (uint64_t)size);
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
	for (offset = 0; offset < size; offset += RISCV32_PAGE_SIZE) {
		leaf = walk_leaf(space, address + offset, 0);
		if (leaf != NULL && (*leaf & RISCV32_PTE_V) != 0) {
			space_unlock(space, enabled);
			return HAL_ERR_INVALID;
		}
	}

	/* Code in the pages is fetched correctly once they are executable. */
	sync_executable((uint64_t)paddr, size, attr);

	/* Writes a leaf for each page, unwinding them all if a table cannot be made. */
	for (offset = 0; offset < size; offset += RISCV32_PAGE_SIZE) {
		leaf = walk_leaf(space, address + offset, 1);
		if (leaf == NULL) {
			for (rollback = 0; rollback < offset; rollback += RISCV32_PAGE_SIZE) {
				leaf = walk_leaf(space, address + rollback, 0);
				if (leaf != NULL)
					*leaf = 0;
			}

			/* Unlinks the tables left empty, then drops the stale translations and frees them. */
			detached = detach_empty_tables(space);
			hal_wmb();
			flush_space(handle);
			free_detached_tables(detached);
			space_unlock(space, enabled);
			return HAL_ERR_NOMEM;
		}

		/* Writes the page's leaf. */
		*leaf = leaf_entry((uint64_t)paddr + offset, user_leaf_flags(attr));
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
	struct riscv32_space *space;
	struct riscv32_table_page *detached;
	uint32_t *leaf;
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

	/* Clears each leaf, then unlinks the tables that became empty. */
	for (offset = 0; offset < size; offset += RISCV32_PAGE_SIZE) {
		leaf = walk_leaf(space, address + offset, 0);
		if (leaf != NULL)
			*leaf = 0;
	}

	/* Unlinks the tables left empty, then drops the stale translations and frees them. */
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
	struct riscv32_space *space;
	uint32_t *leaf;
	uint32_t old;
	uintptr_t address;
	uintptr_t offset;
	uint32_t observed;
	bool enabled;
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
	for (offset = 0; offset < size; offset += RISCV32_PAGE_SIZE) {
		leaf = walk_leaf(space, address + offset, 0);
		if (leaf == NULL || (*leaf & RISCV32_PTE_V) == 0) {
			space_unlock(space, enabled);
			return HAL_ERR_INVALID;
		}
	}

	/* Invalidates each old leaf, noting what it recorded. */
	observed = 0;
	for (offset = 0; offset < size; offset += RISCV32_PAGE_SIZE) {
		leaf = walk_leaf(space, address + offset, 0);
		old = *leaf;
		observed |= HAL_SPACE_PAGE_PRESENT | HAL_SPACE_PAGE_ACCESSED;

		/* A writable page may have been written through the old translation. */
		if ((old & RISCV32_PTE_W) != 0)
			old |= RISCV32_PTE_SW_DIRTY;
		if ((old & RISCV32_PTE_SW_DIRTY) != 0)
			observed |= HAL_SPACE_PAGE_DIRTY;
		*leaf = old & ~(uint32_t)RISCV32_PTE_V;
	}

	/* Publishes the entries before the translations are dropped. */
	hal_wmb();
	flush_space(handle);

	/* Pages that become executable get their code synchronized first. */
	for (offset = 0; offset < size; offset += RISCV32_PAGE_SIZE) {
		leaf = walk_leaf(space, address + offset, 0);
		sync_executable((*leaf & PTE_PPN_MASK) >> RISCV32_PTE_PPN_SHIFT << RISCV32_PAGE_SHIFT, RISCV32_PAGE_SIZE, attr);
	}

	/* Writes the new leaves, keeping each page's dirty record. */
	for (offset = 0; offset < size; offset += RISCV32_PAGE_SIZE) {
		leaf = walk_leaf(space, address + offset, 0);
		old = *leaf;
		*leaf = (old & PTE_PPN_MASK) | user_leaf_flags(attr) | (old & RISCV32_PTE_SW_DIRTY);
	}

	/* Publishes the entries before the translations are dropped. */
	hal_wmb();
	flush_space(handle);

	space_unlock(space, enabled);

	/* Succeeded: reports what the old translations recorded, when asked. */
	if (flags != NULL)
		*flags = observed;
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
	struct riscv32_space *space;
	uint32_t *leaf;
	uint32_t observed;
	bool enabled;
	int valid;
	int locked;

	/* Requires a user page and somewhere to report. */
	if (handle == HAL_SPACE_SYS || flags == NULL)
		return HAL_ERR_INVALID;
	valid = valid_user((uintptr_t)vaddr, RISCV32_PAGE_SIZE);
	if (!valid)
		return HAL_ERR_INVALID;

	/* Reads the page's leaf under the space's lock. */
	locked = space_lock_handle(handle, &space, &enabled);
	if (!locked)
		return HAL_ERR_STATE;

	observed = 0;
	leaf = walk_leaf(space, (uintptr_t)vaddr, 0);
	if (leaf != NULL && (*leaf & RISCV32_PTE_V) != 0)
		observed = HAL_SPACE_PAGE_PRESENT | HAL_SPACE_PAGE_ACCESSED;
	if (leaf != NULL && (*leaf & RISCV32_PTE_SW_DIRTY) != 0)
		observed |= HAL_SPACE_PAGE_DIRTY;

	space_unlock(space, enabled);

	/* Succeeded: the page's state. */
	*flags = observed;
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
	struct riscv32_space *space;
	uint32_t *leaf;
	bool enabled;
	int valid;
	int locked;

	/* Requires a user page and flags this port records. */
	if (handle == HAL_SPACE_SYS)
		return HAL_ERR_INVALID;
	valid = valid_user((uintptr_t)vaddr, RISCV32_PAGE_SIZE);
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
	if (leaf == NULL || (*leaf & RISCV32_PTE_V) == 0) {
		space_unlock(space, enabled);
		return HAL_ERR_INVALID;
	}

	/* A read-only page can be called clean; then the stale entries go. */
	if ((flags & HAL_SPACE_PAGE_DIRTY) != 0 && (*leaf & RISCV32_PTE_W) == 0)
		*leaf &= ~(uint32_t)RISCV32_PTE_SW_DIRTY;
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
	struct riscv32_space *space;
	bool enabled;
	int locked;

	/* The kernel's tables need no space lock. */
	if (handle == HAL_SPACE_SYS) {
		enabled = riscv32_irq_save();
		flush_space(handle);
		riscv32_irq_restore(enabled);
		return;
	}

	/* A user space is flushed under its lock. */
	locked = space_lock_handle(handle, &space, &enabled);
	if (!locked)
		HAL_FATAL("invalid riscv32 space flush");
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
 * Reports the size a table level maps: 1 a page, 2 a megapage.
 */
size_t
hal_space_get_page_size(
	int level)
{
	/* Chooses the size of the level. */
	switch (level) {
	case 1:
		return RISCV32_PAGE_SIZE;
	case 2:
		return RISCV32_MEGAPAGE_SIZE;
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
	/* The first page is never mapped; the half ends at 2 GiB. */
	if (minimum != NULL)
		*minimum = RISCV32_USER_MINIMUM;
	if (limit != NULL)
		*limit = RISCV32_USER_LIMIT;
}

/*
 * Maps a device range into the kernel's device window.
 *
 * Device P is reached at the window's base plus P.  RAM is already in the
 * direct map and is returned as it is; a range that mixes RAM and devices
 * is refused.  The window is mapped whole at boot and shared by every
 * space.
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

	/* Requires a destination and a non-empty range the window reaches. */
	if (vaddr == NULL || size == 0)
		return HAL_ERR_INVALID;
	if ((uint64_t)paddr >= RISCV32_DEVICE_LIMIT)
		return HAL_ERR_INVALID;
	if ((uint64_t)size > RISCV32_DEVICE_LIMIT - (uint64_t)paddr)
		return HAL_ERR_INVALID;

	/* Refuses an executable window and the cache behaviours the tables cannot choose. */
	if ((attr & HAL_SPACE_EXEC) != 0)
		return HAL_ERR_INVALID;
	if ((attr & (HAL_SPACE_WC | HAL_SPACE_WRITETHRU)) != 0)
		return HAL_ERR_UNSUPPORTED;

	/* Returns RAM's existing alias. */
	ram = riscv32_range_is_ram((uint64_t)paddr, (uint64_t)size);
	if (ram) {
		*vaddr = riscv32_phys_to_direct((uint64_t)paddr);
		return HAL_OK;
	}

	/* Refuses a range that mixes RAM and devices. */
	touches = riscv32_range_touches_ram((uint64_t)paddr, (uint64_t)size);
	if (touches)
		return HAL_ERR_INVALID;

	/* The whole window was mapped at boot. */
	/* Succeeded: the device is reachable in the window. */
	*vaddr = (void *)(uintptr_t)(RISCV32_DEVICE_BASE + (uint64_t)paddr);
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

	/* Requires a non-empty range inside the device window or the direct map. */
	address = (uint64_t)(uintptr_t)vaddr;
	if (size == 0)
		return HAL_ERR_INVALID;
	if (address >= RISCV32_DEVICE_BASE && address < (uint64_t)RISCV32_DEVICE_BASE + RISCV32_DEVICE_LIMIT) {
		offset = address - RISCV32_DEVICE_BASE;
		if ((uint64_t)size > RISCV32_DEVICE_LIMIT - offset)
			return HAL_ERR_INVALID;
		return HAL_OK;
	}

	/* An address in the direct map was handed out as RAM. */
	if (address >= RISCV32_DIRECT_BASE && address < RISCV32_DIRECT_LIMIT)
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
	riscv32_fence_i();
	others = other_ready_harts();
	if (others != 0)
		(void)riscv32_sbi_remote_fence_i(others, 0);
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
riscv32_space_memory_stats(
	uint32_t *spaces,
	uint32_t *tables)
{
	bool enabled;

	/* Samples the space count under the registry lock. */
	enabled = riscv32_spin_lock(&space_registry_lock);

	if (spaces != NULL)
		*spaces = space_count;

	riscv32_spin_unlock(&space_registry_lock, enabled);

	/* The table count moves atomically. */
	if (tables != NULL)
		*tables = __atomic_load_n(&page_table_count, __ATOMIC_RELAXED);
}

/* Allocates a zeroed page for a kernel table and counts it. */
static int
alloc_table(
	uint32_t **table,
	hal_physaddr_t *physical)
{
	int error;

	/* One page, page-aligned. */
	error = hal_pmem_alloc(RISCV32_PAGE_SIZE, RISCV32_PAGE_SIZE, physical);
	if (error != HAL_OK)
		return error;

	/* Clears it through the direct map, which the boot table also has. */
	*table = riscv32_phys_to_direct((uint64_t)*physical);
	hal_memset(*table, 0, RISCV32_PAGE_SIZE);
	(void)__atomic_fetch_add(&page_table_count, 1U, __ATOMIC_RELAXED);

	/* Succeeded: the table is empty and the caller's. */
	return HAL_OK;
}

/* Builds an entry that points at the next table. */
static uint32_t
pointer_entry(
	hal_physaddr_t physical)
{
	/* The table's page number and the valid bit alone. */
	return (uint32_t)(physical >> RISCV32_PAGE_SHIFT << RISCV32_PTE_PPN_SHIFT) | TABLE_POINTER;
}

/* Builds a leaf entry for a physical address with the given flags. */
static uint32_t
leaf_entry(
	uint64_t physical,
	uint32_t flags)
{
	/* The page number and the flags. */
	return (uint32_t)(physical >> RISCV32_PAGE_SHIFT << RISCV32_PTE_PPN_SHIFT) | flags;
}

/* Reports whether a range overlaps a reservation that must not be mapped. */
static bool
range_is_nomap(
	uint64_t physical,
	uint64_t size)
{
	const struct riscv32_fdt_info *info;
	uint64_t reserved_end;
	uint64_t end;
	unsigned index;

	/* Compares the range with every no-map reservation. */
	info = riscv32_boot_fdt();
	end = physical + size;
	for (index = 0; index < info->nomap_count; index++) {
		reserved_end = info->nomap[index].base + info->nomap[index].size;
		if (reserved_end <= physical)
			continue;
		if (info->nomap[index].base >= end)
			continue;
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
	ram = riscv32_range_is_ram(physical, size);
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
 * Builds the direct map: RAM in megapages where a whole megapage is
 * mappable, otherwise page by page.
 */
static void
build_direct_map(
	void)
{
	uint64_t first;
	uint64_t end;
	uint64_t physical;
	bool touches;

	/* Visits each megapage of the RAM span that the window reaches. */
	first = riscv32_ram_base() & ~((uint64_t)RISCV32_MEGAPAGE_SIZE - 1U);
	end = riscv32_ram_end();
	if (first < RISCV32_DIRECT_BASE)
		first = RISCV32_DIRECT_BASE;
	if (end > RISCV32_DIRECT_LIMIT)
		end = RISCV32_DIRECT_LIMIT;
	for (physical = first; physical < end; physical += RISCV32_MEGAPAGE_SIZE) {
		/* A megapage with no RAM in it stays unmapped. */
		touches = riscv32_range_touches_ram(physical, RISCV32_MEGAPAGE_SIZE);
		if (!touches)
			continue;
		build_direct_megapage((unsigned)(physical >> RISCV32_MEGAPAGE_SHIFT), physical);
	}
}

/* Maps one megapage of the direct map, as a leaf or through a table of pages. */
static void
build_direct_megapage(
	unsigned root_index,
	uint64_t physical)
{
	hal_physaddr_t pages_physical;
	uint32_t *pages;
	uint64_t page;
	unsigned page_index;
	bool whole;
	bool mappable;
	int error;

	/* A megapage that is all mappable RAM is one leaf. */
	whole = range_is_mappable_ram(physical, RISCV32_MEGAPAGE_SIZE);
	if (whole) {
		kernel_root[root_index] = leaf_entry(physical, KERNEL_LEAF | RISCV32_PTE_R | RISCV32_PTE_W);
		return;
	}

	/* Otherwise a table of pages, mapping each mappable page. */
	error = alloc_table(&pages, &pages_physical);
	if (error != HAL_OK)
		HAL_FATAL("riscv32 direct map table allocation failed");
	for (page_index = 0; page_index < RISCV32_TABLE_ENTRIES; page_index++) {
		page = physical + (uint64_t)page_index * RISCV32_PAGE_SIZE;
		mappable = range_is_mappable_ram(page, RISCV32_PAGE_SIZE);
		if (mappable)
			pages[page_index] = leaf_entry(page, KERNEL_LEAF | RISCV32_PTE_R | RISCV32_PTE_W);
	}

	/* Links the table of pages into the root. */
	kernel_root[root_index] = pointer_entry(pages_physical);
}

/*
 * Builds the device window: the low 1 GiB of physical space in read-write,
 * never-executable megapages.
 */
static void
build_device_window(
	void)
{
	uint64_t physical;
	unsigned index;

	/* One leaf per root entry of the window. */
	for (index = 0; index < RISCV32_DEVICE_ROOT_COUNT; index++) {
		physical = (uint64_t)index * RISCV32_MEGAPAGE_SIZE;
		kernel_root[RISCV32_DEVICE_ROOT_FIRST + index] = leaf_entry(physical, KERNEL_LEAF | RISCV32_PTE_R | RISCV32_PTE_W);
	}
}

/*
 * Maps the megapages of the direct map that hold the kernel image page by
 * page: text read-execute, read-only data read-only, the rest read-write,
 * and the RAM of those megapages after the image as the direct map has it.
 */
static void
build_kernel_window(
	void)
{
	hal_physaddr_t pages_physical;
	uint32_t *pages;
	uintptr_t address;
	uintptr_t first;
	uintptr_t image_end;
	uintptr_t end;
	uint32_t flags;
	unsigned page;
	bool mappable;
	int error;

	/* The image's pages, and the megapages that hold them. */
	first = (uintptr_t)__kernel_vma_start & ~(uintptr_t)(RISCV32_MEGAPAGE_SIZE - 1U);
	image_end = ((uintptr_t)__kernel_vma_end + RISCV32_PAGE_SIZE - 1U) & ~(uintptr_t)(RISCV32_PAGE_SIZE - 1U);
	end = (image_end + RISCV32_MEGAPAGE_SIZE - 1U) & ~(uintptr_t)(RISCV32_MEGAPAGE_SIZE - 1U);
	pages = NULL;
	for (address = first; address < end; address += RISCV32_PAGE_SIZE) {
		page = (unsigned)((address % RISCV32_MEGAPAGE_SIZE) / RISCV32_PAGE_SIZE);

		/* A new megapage gets its table of pages in place of its leaf. */
		if (page == 0) {
			error = alloc_table(&pages, &pages_physical);
			if (error != HAL_OK)
				HAL_FATAL("riscv32 kernel window table allocation failed");
			kernel_root[address >> RISCV32_MEGAPAGE_SHIFT] = pointer_entry(pages_physical);
		}

		/* Outside the image: RAM as the direct map has it. */
		if (address < (uintptr_t)__kernel_vma_start || address >= image_end) {
			mappable = range_is_mappable_ram(address, RISCV32_PAGE_SIZE);
			if (mappable)
				pages[page] = leaf_entry(address, KERNEL_LEAF | RISCV32_PTE_R | RISCV32_PTE_W);
			continue;
		}

		/* Chooses the page's permissions by the section it lies in. */
		flags = KERNEL_LEAF | RISCV32_PTE_R | RISCV32_PTE_W;
		if (address >= (uintptr_t)__kernel_text_start && address < (uintptr_t)__kernel_text_end) {
			flags = KERNEL_LEAF | RISCV32_PTE_R | RISCV32_PTE_X;
		} else if (address >= (uintptr_t)__kernel_rodata_start && address < (uintptr_t)__kernel_rodata_end) {
			flags = KERNEL_LEAF | RISCV32_PTE_R;
		}

		/* Writes the page's leaf. */
		pages[page] = leaf_entry(address, flags);
	}
}

/* Reports the flags of the leaf for an image address. */
static uint32_t
kernel_page_flags(
	uintptr_t address)
{
	const uint32_t *pages;
	uint32_t entry;

	/* Walks the root entry to the table of pages below it. */
	entry = kernel_root[address >> RISCV32_MEGAPAGE_SHIFT];
	pages = riscv32_phys_to_direct((entry & PTE_PPN_MASK) >> RISCV32_PTE_PPN_SHIFT << RISCV32_PAGE_SHIFT);
	entry = pages[(address >> RISCV32_PAGE_SHIFT) & (RISCV32_TABLE_ENTRIES - 1U)];

	/* Succeeded: the leaf's flags. */
	return entry & ~PTE_PPN_MASK;
}

/* Reports the hart mask of every hart that is up, except the caller's. */
static unsigned long
other_ready_harts(
	void)
{
	struct riscv32_percpu *self;
	struct riscv32_percpu *record;
	unsigned long mask;
	unsigned ready;
	hal_cpu_id_t cpu;

	/* Collects each ready hart's bit by its hart id. */
	self = riscv32_percpu_current();
	mask = 0;
	for (cpu = 0; cpu < RISCV32_CPU_MAX; cpu++) {
		record = riscv32_percpu_get(cpu);
		if (record == self)
			continue;
		ready = __atomic_load_n(&record->ready, __ATOMIC_ACQUIRE);
		if (ready == 0)
			continue;
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
	struct riscv32_percpu *current;
	struct riscv32_percpu *record;
	hal_space_t selected;
	unsigned long mask;
	unsigned ready;
	hal_cpu_id_t cpu;

	/* Orders the table stores before the reads of the selections. */
	hal_mb();

	/* Looks at every hart's selection. */
	current = riscv32_percpu_current();
	*self = false;
	mask = 0;
	for (cpu = 0; cpu < RISCV32_CPU_MAX; cpu++) {
		record = riscv32_percpu_get(cpu);
		selected = __atomic_load_n(&record->current_space, __ATOMIC_ACQUIRE);
		if (selected != handle)
			continue;
		if (record == current) {
			*self = true;
			continue;
		}

		/* Reads whether the hart is up. */
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
		riscv32_tlb_flush_everywhere();
		return;
	}

	/* A user space where it is selected. */
	others = harts_selecting(handle, &self);
	if (self)
		riscv32_sfence_vma_all();
	if (others != 0)
		(void)riscv32_sbi_remote_sfence_vma(others, 0);
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
	struct riscv32_space **result,
	bool *irq_enabled)
{
	struct riscv32_space *space;
	unsigned me;
	bool enabled;
	bool acquired;

	/* Looks the handle up among the live spaces. */
	enabled = riscv32_spin_lock(&space_registry_lock);
	for (space = space_registry; space != NULL; space = space->registry_next) {
		if ((hal_space_t)space == handle)
			break;
	}

	/* A handle that names no live space is refused. */
	if (space == NULL || space->destroying) {
		riscv32_spin_unlock(&space_registry_lock, enabled);
		return 0;
	}

	/* A second operation on the same space from the same hart is a fault. */
	me = hal_cpu_current() + 1U;
	if (space->lock_owner == me)
		HAL_FATAL("recursive riscv32 space operation");

	/* Waits for another hart's operation on the space to end. */
	for (;;) {
		acquired = hal_atomic_uint_try_acquire(&space->lock);
		if (acquired)
			break;
		hal_atomic_relax();
	}

	/* Records the holder, so that a nested operation is caught. */
	space->lock_owner = me;

	/* Leaves the registry with interrupts still off for the space's operation. */
	__atomic_store_n(&space_registry_lock, 0U, __ATOMIC_RELEASE);

	/* Succeeded: the caller holds the space. */
	*result = space;
	*irq_enabled = enabled;
	return 1;
}

/* Releases a space taken by space_lock_handle() and restores interrupts. */
static void
space_unlock(
	struct riscv32_space *space,
	bool enabled)
{
	unsigned me;

	/* Only the hart that holds the space may release it. */
	me = hal_cpu_current() + 1U;
	if (space->lock == 0 || space->lock_owner != me)
		HAL_FATAL("invalid riscv32 space unlock");

	/* Releases the space, publishing every change made under it. */
	space->lock_owner = 0;
	riscv32_spin_unlock(&space->lock, enabled);
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
	if ((address & (RISCV32_PAGE_SIZE - 1U)) != 0)
		return 0;
	if ((size & (RISCV32_PAGE_SIZE - 1U)) != 0)
		return 0;

	/* The first page is never mapped, and the range must end inside the user half. */
	if (address < RISCV32_USER_MINIMUM)
		return 0;
	if (address >= RISCV32_USER_LIMIT)
		return 0;
	if (size > RISCV32_USER_LIMIT - address)
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
	/* A mapping must allow some access. */
	if ((attr & (HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC)) == 0)
		return HAL_ERR_INVALID;

	/* A page is never both writable and executable. */
	if ((attr & HAL_SPACE_WRITE) != 0 && (attr & HAL_SPACE_EXEC) != 0)
		return HAL_ERR_INVALID;

	/* The cache behaviour of memory is the platform's, and device pages are not offered to users. */
	if ((attr & (HAL_SPACE_NOCACHE | HAL_SPACE_WRITETHRU | HAL_SPACE_WC | HAL_SPACE_DEVICE)) != 0)
		return HAL_ERR_UNSUPPORTED;

	/* Succeeded: the attributes can be represented. */
	return HAL_OK;
}

/* Allocates a zeroed page-table page and records it as the space's. */
static struct riscv32_table_page *
allocate_table(
	struct riscv32_space *space,
	uint32_t *parent,
	unsigned index)
{
	struct riscv32_table_page *page;
	int error;

	/* Allocates the record of the table. */
	page = kernel_alloc(sizeof(*page));
	if (page == NULL)
		return NULL;

	/* Allocates the table itself. */
	error = hal_pmem_alloc(RISCV32_PAGE_SIZE, RISCV32_PAGE_SIZE, &page->memory);
	if (error != HAL_OK) {
		kernel_free(page);
		return NULL;
	}

	/* Clears the table and remembers the entry that will point at it. */
	hal_memset(hal_pmem_to_kernel(page->memory), 0, RISCV32_PAGE_SIZE);
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
static uint32_t *
walk_leaf(
	struct riscv32_space *space,
	uintptr_t address,
	int create)
{
	struct riscv32_table_page *page;
	uint32_t *table;
	uint32_t entry;
	unsigned level;
	unsigned index;

	/* Descends from the root to the last-level table. */
	table = space->root;
	for (level = 0; level < 1U; level++) {
		index = (unsigned)(address >> RISCV32_MEGAPAGE_SHIFT) & (RISCV32_TABLE_ENTRIES - 1U);
		entry = table[index];

		/* A missing table is made only when asked to. */
		if ((entry & RISCV32_PTE_V) == 0) {
			if (!create)
				return NULL;
			page = allocate_table(space, table, index);
			if (page == NULL)
				return NULL;
			entry = pointer_entry(page->memory);
			table[index] = entry;
		}

		/* A leaf where a table should be ends the walk. */
		if ((entry & PERMISSIONS) != 0)
			return NULL;
		table = riscv32_phys_to_direct((entry & PTE_PPN_MASK) >> RISCV32_PTE_PPN_SHIFT << RISCV32_PAGE_SHIFT);
	}

	/* Succeeded: the entry of the page. */
	return &table[(address >> RISCV32_PAGE_SHIFT) & (RISCV32_TABLE_ENTRIES - 1U)];
}

/* Reports whether a table has no valid entry. */
static int
table_empty(
	const uint32_t *table)
{
	unsigned index;

	/* Looks for any valid entry. */
	for (index = 0; index < RISCV32_TABLE_ENTRIES; index++) {
		if ((table[index] & RISCV32_PTE_V) != 0)
			return 0;
	}

	/* The table is empty. */
	return 1;
}

/*
 * Unlinks every empty table of a space, repeating until none is left
 * whose children all went, and reports them for freeing after the flush.
 */
static struct riscv32_table_page *
detach_empty_tables(
	struct riscv32_space *space)
{
	struct riscv32_table_page *detached;
	struct riscv32_table_page **link;
	struct riscv32_table_page *page;
	uint32_t parent_entry;
	int again;
	int empty;
	uint32_t expected_entry;

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
			expected_entry = pointer_entry(page->memory);
			if ((parent_entry & RISCV32_PTE_V) == 0 ||
			    expected_entry != parent_entry)
				HAL_FATAL("detaching an unlinked riscv32 page table");

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
	struct riscv32_table_page *page)
{
	struct riscv32_table_page *next;

	/* Frees each table and its record, and stops counting it. */
	while (page != NULL) {
		next = page->next;
		(void)hal_pmem_free(&page->memory, RISCV32_PAGE_SIZE);
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
static uint32_t
user_leaf_flags(
	uint32_t attr)
{
	uint32_t flags;

	/* Every user leaf is valid, the user's and already accessed. */
	flags = RISCV32_PTE_V | RISCV32_PTE_U | RISCV32_PTE_A;

	/* A readable page; a writable or executable one is readable too. */
	flags |= RISCV32_PTE_R;
	if ((attr & HAL_SPACE_WRITE) != 0)
		flags |= RISCV32_PTE_W | RISCV32_PTE_D | RISCV32_PTE_SW_DIRTY;
	if ((attr & HAL_SPACE_EXEC) != 0)
		flags |= RISCV32_PTE_X;

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
	/* Only memory that is to be executed. */
	if ((attr & HAL_SPACE_EXEC) == 0)
		return;

	/* Its bytes through the kernel's direct map. */
	hal_sync_instruction_stream(riscv32_phys_to_direct(physical), size);
}
