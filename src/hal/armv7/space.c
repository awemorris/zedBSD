/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The armv7 address spaces: the kernel's half and the user spaces.
 *
 * TTBCR.N = 1 splits the address space: TTBR1 selects the kernel's
 * first-level table for the upper 2 GiB on every processor, and TTBR0 the
 * selected user space's 8 KiB table for the lower 2 GiB, or an empty one
 * while no user space is selected.  A user space's translations are tagged
 * with its address space identifier, so selecting a space flushes nothing.
 *
 * The kernel's half holds the direct map of the RAM the device tree
 * describes, in sections, except the sections of the kernel image, which
 * are mapped page by page so that its text alone is executable and its
 * read-only data read-only; the device window, which hands out whole
 * sections of device registers as drivers ask for them; and the uncached
 * window, where RAM for a device that does not snoop the caches is mapped
 * a second time, page by page.  The boot runs on
 * the table locore.S built until armv7_space_init() moves to this one.
 *
 * A user page's logical state is its shadow entry (pte.h): the hardware
 * neither sets the access flag nor marks a page dirty here, so the first
 * access and the first store of a page fault, and armv7_space_resolve_fault()
 * records them and lets the access run again without the kernel's help.
 */

#include <hal/hal.h>
#include "asm.h"
#include "bootinfo.h"
#include "defs.h"
#include "page.h"
#include "percpu.h"
#include "pte.h"
#include "space.h"
#include "spin.h"

/* The first-level entry index of the direct map's and the device window's first section. */
#define SPACE_DIRECT_FIRST_INDEX	(ARMV7_DIRECT_BASE >> ARMV7_SECTION_SHIFT)
#define SPACE_DEVICE_FIRST_INDEX	(ARMV7_DEVICE_BASE >> ARMV7_SECTION_SHIFT)

/*
 * The kernel's section of normal memory, read-write and never executed,
 * and the device window's sections of device and of uncached memory.
 */
#define SPACE_SECTION_DATA		(ARMV7_L1_KERNEL_NORMAL | ARMV7_L1_SECTION_XN)
#define SPACE_SECTION_DEVICE		ARMV7_L1_KERNEL_DEVICE
#define SPACE_SECTION_NOCACHE		(ARMV7_L1_TYPE_SECTION | ARMV7_L1_SECTION_TEX0 | ARMV7_L1_SECTION_S | \
					 ARMV7_L1_SECTION_AF | ARMV7_L1_SECTION_XN)

/* The first-level entry index of the uncached window's first section. */
#define SPACE_UNCACHED_FIRST_INDEX	(ARMV7_UNCACHED_BASE >> ARMV7_SECTION_SHIFT)

/*
 * How many sections of the uncached window share one page of tables:
 * each section's table is 1 KiB.
 */
#define SPACE_UNCACHED_TABLES_PER_PAGE	(ARMV7_PAGE_SIZE / ARMV7_L2_TABLE_BYTES)

/* The words of the uncached window's map of used pages. */
#define SPACE_UNCACHED_WORDS		(ARMV7_UNCACHED_PAGES / 32U)

/* The highest address space identifier; 0 is kept for the moments a space changes. */
#define SPACE_ASID_LAST			255U

/* How many pages a range flush takes one at a time before it flushes everything. */
#define SPACE_FLUSH_PAGE_LIMIT		64U

/* The words from a shadow entry to its hardware entry in the same page of tables. */
#define SPACE_HARDWARE_WORDS		(ARMV7_TABLE_PAGE_HARDWARE / 4U)

/*
 * One section of the device window: the physical section it maps and the
 * memory type, kept so that a second request for the same registers gets
 * the same window.
 */
struct space_device_section {
	uint32_t physical;
	unsigned memory_type;
	int used;
};

/* The bounds of the kernel image's sections, from the linker script. */
extern char __kernel_vma_start[];
extern char __kernel_text_start[];
extern char __kernel_text_end[];
extern char __kernel_rodata_start[];
extern char __kernel_rodata_end[];
extern char __kernel_vma_end[];

/*
 * The table locore.S builds and the boot runs on until the kernel's own.
 *
 * It maps the image where it was loaded and the start of RAM in the
 * direct map, and armv7_space_map_device_window() adds the early devices
 * to it.  locore.S zeroes and fills it before the MMU is on, which is why
 * it is not static.
 */
uint32_t armv7_boot_l1[ARMV7_L1_ENTRIES] __attribute__((aligned(ARMV7_KERNEL_L1_ALIGNMENT)));

/*
 * The kernel's first-level table, which TTBR1 selects on every processor
 * from armv7_space_init() on.  Its upper half is the kernel's half; only
 * the device window's entries change after the boot.
 */
static uint32_t kernel_l1[ARMV7_L1_ENTRIES] __attribute__((aligned(ARMV7_KERNEL_L1_ALIGNMENT)));

/*
 * The lower half's table while no user space is selected: every entry
 * zero, so every user address faults.  Never written.
 */
static uint32_t empty_user_l1[ARMV7_L1_USER_ENTRIES] __attribute__((aligned(ARMV7_USER_L1_BYTES)));

/*
 * The table the device window's entries are written to: the boot table
 * until armv7_space_init() moves to the kernel's, which it points at from
 * then on.  Kept under device_lock.
 */
static uint32_t *device_table = armv7_boot_l1;

/* The device window's sections; kept under device_lock. */
static struct space_device_section device_sections[ARMV7_DEVICE_SECTIONS];

/* Guards device_table and device_sections; zero when free. */
static volatile unsigned device_lock;

/*
 * The physical address the kernel image was loaded at.
 *
 * Set by armv7_kernel_image_init() as the boot processor's first act and
 * constant afterwards.
 */
static uint64_t kernel_phys_start;

/*
 * Set once armv7_space_init() has moved to the kernel's table and found
 * the image's sections with the permissions they need; the boot log's
 * closing line reports it.  Written once by the boot processor.
 */
static int kernel_mapping_checked;

/*
 * Every user space that exists, the number the next one gets and how many
 * there are.  Kept under space_registry_lock; a space is looked up here
 * before its own lock is taken, so a destroyed space is never locked.
 */
static struct armv7_space *space_registry;
static int next_space_id = 1;
static uint32_t space_count;

/* Guards space_registry, next_space_id and space_count; zero when free. */
static volatile unsigned space_registry_lock;

/*
 * The current generation of address space identifiers and the next free
 * identifier in it.  When the identifiers run out, the generation moves on
 * (skipping zero, which no space ever has), every translation is dropped
 * and identifiers are handed out from 1 again.  Kept under asid_lock.
 */
static uint32_t asid_generation = 1;
static uint32_t next_asid = 1;

/* Guards asid_generation, next_asid and each space's identifier; zero when free. */
static volatile unsigned asid_lock;

/*
 * The uncached window's pages, one bit each, set while a view uses it.
 * Kept under device_lock, like the window's tables, which once made stay
 * for the kernel's life.
 */
static uint32_t uncached_used[SPACE_UNCACHED_WORDS];

/*
 * How many page-table pages the user spaces and the kernel's half hold,
 * for the memory statistics.  Changed with atomic adds, since each space's
 * tables are changed under that space's own lock.
 */
static uint32_t page_table_count;

static int alloc_table_page(hal_physaddr_t *physical, uint32_t **entries);
static void build_direct_map(void);
static void build_direct_section(uint64_t physical);
static void build_kernel_window(void);
static void move_to_kernel_table(void);
static uint32_t kernel_page_descriptor(uintptr_t address);
static int find_device_run(uint32_t first, uint32_t count, unsigned memory_type, uint32_t *start);
static int find_uncached_run(uint32_t count, uint32_t *start);
static int make_uncached_tables(uint32_t first, uint32_t count);
static uint32_t *uncached_entry(uint32_t page);
static void mark_uncached(uint32_t first, uint32_t count, int used);
static int uncached_page_used(uint32_t page);
static void select_table(uint32_t ttbr0, uint32_t asid);
static void assign_asid(struct armv7_space *space);
static int asid_current(const struct armv7_space *space);
static void flush_space(struct armv7_space *space);
static void flush_space_page(struct armv7_space *space, uintptr_t address);
static int space_lock_handle(hal_space_t handle, struct armv7_space **result, bool *irq_enabled);
static void space_unlock(struct armv7_space *space, bool enabled);
static int valid_user(uintptr_t address, size_t size);
static int user_memory_type(uint32_t attr, uint64_t physical, size_t size, unsigned *memory_type);
static uint32_t *walk_shadow(struct armv7_space *space, uintptr_t address, int create);
static void write_entry(uint32_t *shadow, uint32_t value);
static int table_page_empty(const struct armv7_table_page *page);
static struct armv7_table_page *detach_empty_tables(struct armv7_space *space);
static void free_detached_tables(struct armv7_table_page *page);
static void sync_executable(uint32_t shadow);

/*
 * Records where the kernel image was loaded.
 */
void
armv7_kernel_image_init(
	uint64_t phys_start)
{
	/* Every image address translates through the direct map; this is for the log and the checks. */
	kernel_phys_start = phys_start;
}

/*
 * Reports the physical address the kernel image was loaded at.
 */
uint64_t
armv7_kernel_phys_start(
	void)
{
	/* Succeeded: the address recorded at boot. */
	return kernel_phys_start;
}

/*
 * Translates a kernel address in the image or the direct map to its
 * physical address.
 */
uint64_t
armv7_virt_to_phys(
	const void *address)
{
	uintptr_t value;

	/* The direct map holds RAM at a fixed offset from its physical address. */
	value = (uintptr_t)address;
	if (value >= (uintptr_t)ARMV7_DIRECT_BASE && value < (uintptr_t)ARMV7_DIRECT_LIMIT)
		return (uint64_t)(value - ARMV7_DIRECT_BASE) + ARMV7_PHYS_BASE;

	/* Any other address is not the kernel's to translate. */
	HAL_FATAL("armv7 address outside the image and the direct map");
	return 0;
}

/*
 * Reports the direct-map address of a physical address of RAM.
 */
void *
armv7_phys_to_direct(
	uint64_t physical)
{
	/* The direct map holds RAM at a fixed offset from its physical address. */
	return (void *)(uintptr_t)(physical - ARMV7_PHYS_BASE + ARMV7_DIRECT_BASE);
}

/*
 * Maps device registers into the device window, a section at a time, and
 * reports where the first byte is.
 *
 * A request for sections the window already maps with the same memory
 * type gets the same window, so two drivers of one block share it, and
 * nothing is ever unmapped.  It works from the first instruction of C on:
 * before the kernel's table exists the boot table takes the sections.
 * Returns HAL_OK, HAL_ERR_INVALID for a range past 4 GiB, or
 * HAL_ERR_NOMEM when the window is full.
 */
int
armv7_space_map_device_window(
	uint64_t physical,
	size_t size,
	unsigned memory_type,
	void **address)
{
	uint64_t first;
	uint64_t end;
	uint32_t first_section;
	uint32_t count;
	uint32_t start;
	uint32_t index;
	uint32_t section;
	uint32_t descriptor;
	bool enabled;
	int found;

	/* Requires a non-empty range the short descriptors can reach. */
	if (address == NULL || size == 0)
		return HAL_ERR_INVALID;
	end = physical + size;
	if (end > 0x100000000ULL || end < physical)
		return HAL_ERR_INVALID;

	/* The whole sections the range touches. */
	first = physical & ~(uint64_t)(ARMV7_SECTION_SIZE - 1);
	first_section = (uint32_t)(first >> ARMV7_SECTION_SHIFT);
	count = (uint32_t)((end - first + ARMV7_SECTION_SIZE - 1) >> ARMV7_SECTION_SHIFT);
	if (count > ARMV7_DEVICE_SECTIONS)
		return HAL_ERR_NOMEM;

	/* The descriptor every section of the run gets. */
	descriptor = SPACE_SECTION_DEVICE;
	if (memory_type == ARMV7_PTE_ATTR_NOCACHE)
		descriptor = SPACE_SECTION_NOCACHE;

	/* Reuses the run that maps the same sections, or takes a free one. */
	enabled = armv7_spin_lock(&device_lock);

	found = find_device_run(first_section, count, memory_type, &start);
	if (!found) {
		armv7_spin_unlock(&device_lock, enabled);
		return HAL_ERR_NOMEM;
	}

	/* Writes the sections a new run needs; a shared run is already written. */
	for (index = 0; index < count; index++) {
		/* A section the run shares is left as it is. */
		if (device_sections[start + index].used)
			continue;
		section = first_section + index;
		device_sections[start + index].physical = section;
		device_sections[start + index].memory_type = memory_type;
		device_sections[start + index].used = 1;
		device_table[SPACE_DEVICE_FIRST_INDEX + start + index] = (section << ARMV7_SECTION_SHIFT) | descriptor;
	}

	armv7_spin_unlock(&device_lock, enabled);

	/* Publishes the entries to the table walk before the caller touches the registers. */
	armv7_dsb();
	armv7_isb();

	/* Succeeded: the registers' first byte, in its section's window. */
	*address = (void *)(uintptr_t)(ARMV7_DEVICE_BASE + ((uintptr_t)start << ARMV7_SECTION_SHIFT) +
				       (uintptr_t)(physical - first));
	return HAL_OK;
}

/*
 * Builds the kernel's table and moves the boot processor to it.
 *
 * The direct map holds exactly the RAM the device tree describes, the
 * image is mapped page by page with its sections' permissions, and the
 * device window keeps what the boot mapped.  The lower half then belongs
 * to the user spaces, and the identity section of the image is gone.
 */
void
armv7_space_init(
	void)
{
	uint32_t text_page;
	uint32_t rodata_page;
	uint32_t data_page;
	unsigned index;
	bool enabled;

	/* Starts from an empty table; the lower half stays empty for good. */
	hal_memset(kernel_l1, 0, sizeof(kernel_l1));
	space_registry = NULL;

	/* The direct map, then the image's pages over it. */
	build_direct_map();
	build_kernel_window();

	/* Copies the device window and makes the kernel's table the one it is written to. */
	enabled = armv7_spin_lock(&device_lock);

	for (index = 0; index < ARMV7_DEVICE_SECTIONS; index++)
		kernel_l1[SPACE_DEVICE_FIRST_INDEX + index] = armv7_boot_l1[SPACE_DEVICE_FIRST_INDEX + index];
	device_table = kernel_l1;

	armv7_spin_unlock(&device_lock, enabled);

	/* Moves to the kernel's table with the address space split. */
	move_to_kernel_table();
	armv7_percpu_current()->current_space = HAL_SPACE_SYS;

	/* Checks W^X on the three kinds of section the kernel runs from. */
	text_page = kernel_page_descriptor((uintptr_t)__kernel_text_start);
	rodata_page = kernel_page_descriptor((uintptr_t)__kernel_rodata_start);
	data_page = kernel_page_descriptor((uintptr_t)&kernel_mapping_checked);
	if ((text_page & ARMV7_L2_XN) != 0 || (text_page & ARMV7_L2_AP2) == 0)
		HAL_FATAL("armv7 kernel text mapping is not read-execute");
	if ((rodata_page & ARMV7_L2_XN) == 0 || (rodata_page & ARMV7_L2_AP2) == 0)
		HAL_FATAL("armv7 kernel rodata mapping is not read-only");
	if ((data_page & ARMV7_L2_XN) == 0 || (data_page & ARMV7_L2_AP2) != 0)
		HAL_FATAL("armv7 kernel data mapping is not read-write");

	/* Records the passed checks for the closing line and reports them now. */
	kernel_mapping_checked = 1;
	hal_puts("ARMV7 W^X PASS\n");
}

/*
 * Reports whether the kernel's table was moved to and found right.
 */
bool
armv7_space_checked(
	void)
{
	/* The table was not built, or a check stopped the boot first. */
	if (!kernel_mapping_checked)
		return false;

	/* Succeeded: the kernel runs on its own table with W^X in force. */
	return true;
}

/*
 * Resolves a fault on a user page of the space this processor has
 * selected, when it is only the first access or the first store of the
 * page, and reports whether it did.
 *
 * The fault may come from user mode or from the kernel reaching user
 * memory; either way the access is run again afterwards.  A fault the HAL
 * cannot resolve is the kernel's, and nothing was changed.
 */
int
armv7_space_resolve_fault(
	uintptr_t address,
	unsigned status,
	int write)
{
	struct armv7_percpu *cpu;
	struct armv7_space *space;
	enum armv7_pte_resolution resolution;
	hal_space_t handle;
	uint32_t *shadow;
	bool enabled;
	int locked;

	/* Only the first access or store of a user page can be resolved here. */
	if (address < ARMV7_USER_MINIMUM || address >= ARMV7_USER_LIMIT)
		return 0;
	if (status != ARMV7_FS_AF_PAGE && status != ARMV7_FS_PERMISSION_PAGE)
		return 0;

	/* The page belongs to the space this processor has selected. */
	cpu = armv7_percpu_current();
	handle = __atomic_load_n(&cpu->current_space, __ATOMIC_ACQUIRE);
	if (handle == HAL_SPACE_SYS)
		return 0;
	locked = space_lock_handle(handle, &space, &enabled);
	if (!locked)
		return 0;

	/* Decides from the page's shadow entry. */
	shadow = walk_shadow(space, address, 0);
	resolution = ARMV7_PTE_TO_KERNEL;
	if (shadow != NULL)
		resolution = armv7_pte_resolve(shadow, status, write);

	/* Remakes the hardware entry from the updated shadow and drops the stale translation. */
	if (resolution == ARMV7_PTE_RESOLVED) {
		write_entry(shadow, *shadow);
		flush_space_page(space, address);
	}

	space_unlock(space, enabled);

	/* The kernel resolves the fault. */
	if (resolution != ARMV7_PTE_RESOLVED)
		return 0;

	/* Succeeded: the access can run again. */
	return 1;
}

/*
 * Reports the address space identifier a space has now, 0 for the kernel
 * alone or for a space that has none in the current generation.
 */
uint32_t
armv7_space_asid(
	hal_space_t handle)
{
	struct armv7_space *space;
	uint32_t asid;
	bool enabled;
	int current;

	/* The kernel alone has none. */
	if (handle == HAL_SPACE_SYS)
		return 0;

	/* Reads the identifier under its lock. */
	space = handle;
	enabled = armv7_spin_lock(&asid_lock);

	asid = 0;
	current = asid_current(space);
	if (current)
		asid = space->asid;

	armv7_spin_unlock(&asid_lock, enabled);

	/* Succeeded: the identifier, or 0. */
	return asid;
}

/*
 * Creates an empty user space.
 */
hal_space_t
hal_space_create(
	void)
{
	struct armv7_space *space;
	bool enabled;
	int error;

	/* A user space needs the split the kernel's table brings. */
	if (!kernel_mapping_checked)
		HAL_FATAL("armv7 user space before the kernel table is built");

	/* Allocates the record. */
	space = kernel_alloc(sizeof(*space));
	if (space == NULL)
		return NULL;
	hal_memset(space, 0, sizeof(*space));

	/* Allocates the first-level table, aligned as TTBR0 asks. */
	error = hal_pmem_alloc(ARMV7_USER_L1_BYTES, ARMV7_USER_L1_BYTES, &space->l1_memory);
	if (error != HAL_OK) {
		kernel_free(space);
		return NULL;
	}

	/* Empties the table; the space has no identifier yet. */
	space->l1 = hal_pmem_to_kernel(space->l1_memory);
	hal_memset(space->l1, 0, ARMV7_USER_L1_BYTES);
	space->asid = 0;
	space->asid_generation = 0;
	space->magic = ARMV7_SPACE_MAGIC;

	/* Numbers the space and makes it live. */
	enabled = armv7_spin_lock(&space_registry_lock);

	space->space_id = next_space_id++;
	space->registry_next = space_registry;
	space_registry = space;
	space_count++;

	armv7_spin_unlock(&space_registry_lock, enabled);

	/* Succeeded: the space, with nothing of the user's mapped. */
	return space;
}

/*
 * Destroys a user space that no processor has selected and no operation
 * holds.
 */
void
hal_space_destroy(
	hal_space_t handle)
{
	struct armv7_space *space;
	struct armv7_space **link;
	struct armv7_table_page *page;
	struct armv7_percpu *record;
	hal_space_t selected;
	hal_cpu_id_t cpu;
	bool enabled;

	/* A missing space is nothing to destroy. */
	space = handle;
	if (space == NULL)
		return;

	/* Takes the space out of the registry, so nothing can lock it any more. */
	enabled = armv7_spin_lock(&space_registry_lock);

	link = &space_registry;
	while (*link != NULL && *link != space)
		link = &(*link)->registry_next;
	if (*link == NULL || space->destroying)
		HAL_FATAL("invalid armv7 space destroy");
	if (space->lock != 0)
		HAL_FATAL("destroying a busy armv7 space");
	space->destroying = 1U;
	*link = space->registry_next;
	if (space_count != 0)
		space_count--;

	armv7_spin_unlock(&space_registry_lock, enabled);

	/* Refuses a space a processor still selects. */
	for (cpu = 0; cpu < ARMV7_CPU_MAX; cpu++) {
		record = armv7_percpu_get(cpu);
		selected = __atomic_load_n(&record->current_space, __ATOMIC_ACQUIRE);
		if (selected == handle)
			HAL_FATAL("destroying an active armv7 space");
	}

	/* Drops the space's translations before its tables are reused. */
	flush_space(space);

	/* Frees every page of tables. */
	while (space->tables != NULL) {
		page = space->tables;
		space->tables = page->next;
		(void)hal_pmem_free(&page->memory, ARMV7_PAGE_SIZE);
		kernel_free(page);
		(void)__atomic_fetch_sub(&page_table_count, 1U, __ATOMIC_RELAXED);
	}

	/* Frees the first-level table and the record. */
	space->magic = 0;
	(void)hal_pmem_free(&space->l1_memory, ARMV7_USER_L1_BYTES);
	kernel_free(space);
}

/*
 * Selects a user space, or the kernel alone, on the calling processor.
 *
 * The selection is published before TTBR0 changes, so a processor that
 * changes the space's tables afterwards flushes its translations.  The
 * space keeps its identifier, so nothing is flushed here.
 */
void
hal_space_switch(
	hal_space_t handle)
{
	struct armv7_percpu *cpu;
	struct armv7_space *space;
	uint32_t ttbr0;
	bool enabled;
	bool space_enabled;
	int locked;

	/*
	 * Names the processor with interrupts off, so that a caller outside a
	 * task switch cannot move to another processor between reading the
	 * record and installing the table.
	 */
	enabled = armv7_irq_save();
	cpu = armv7_percpu_current();

	/* The space already selected here needs nothing. */
	if (handle == cpu->current_space) {
		armv7_irq_restore(enabled);
		return;
	}

	/* The kernel alone: the empty lower half, under the reserved identifier. */
	if (handle == HAL_SPACE_SYS) {
		__atomic_store_n(&cpu->current_space, handle, __ATOMIC_RELEASE);
		hal_mb();
		ttbr0 = armv7_pte_ttbr(armv7_virt_to_phys(empty_user_l1));
		select_table(ttbr0, 0);
		armv7_irq_restore(enabled);
		return;
	}

	/* A user space is held while its table and identifier are installed. */
	locked = space_lock_handle(handle, &space, &space_enabled);
	if (!locked)
		HAL_FATAL("invalid armv7 space switch");

	assign_asid(space);
	__atomic_store_n(&cpu->current_space, handle, __ATOMIC_RELEASE);
	hal_mb();
	ttbr0 = armv7_pte_ttbr(space->l1_memory);
	select_table(ttbr0, space->asid);

	space_unlock(space, space_enabled);

	/* Restores the caller's interrupt state. */
	armv7_irq_restore(enabled);
}

/*
 * Maps memory into a user space: managed RAM, or the caller's device
 * pages with HAL_SPACE_DEVICE.
 *
 * A page of RAM starts neither accessed nor dirty, so its first access and
 * its first store are recorded by the faults they take.
 */
int
hal_space_map(
	hal_space_t handle,
	void *vaddr,
	hal_physaddr_t paddr,
	size_t size,
	uint32_t attr)
{
	struct armv7_space *space;
	struct armv7_table_page *detached;
	uint32_t *shadow;
	uint32_t entry;
	uintptr_t address;
	uintptr_t offset;
	uintptr_t rollback;
	unsigned memory_type;
	bool enabled;
	int valid;
	int locked;
	int writable;
	int executable;

	/* Requires a user range and a page-aligned physical range. */
	address = (uintptr_t)vaddr;
	if (handle == HAL_SPACE_SYS)
		return HAL_ERR_INVALID;
	valid = valid_user(address, size);
	if (!valid)
		return HAL_ERR_INVALID;
	if (((uint64_t)paddr & (ARMV7_PAGE_SIZE - 1U)) != 0)
		return HAL_ERR_INVALID;
	if ((uint64_t)paddr + size > 0x100000000ULL)
		return HAL_ERR_INVALID;

	/* Requires attributes this port can represent for this memory. */
	valid = user_memory_type(attr, (uint64_t)paddr, size, &memory_type);
	if (valid != HAL_OK)
		return valid;
	writable = 0;
	if ((attr & HAL_SPACE_WRITE) != 0)
		writable = 1;
	executable = 0;
	if ((attr & HAL_SPACE_EXEC) != 0)
		executable = 1;

	/* Holds the space for the whole change. */
	locked = space_lock_handle(handle, &space, &enabled);
	if (!locked)
		return HAL_ERR_STATE;

	/* Refuses a range of which any page is mapped already. */
	for (offset = 0; offset < size; offset += ARMV7_PAGE_SIZE) {
		shadow = walk_shadow(space, address + offset, 0);
		if (shadow != NULL && (*shadow & ARMV7_PTE_PRESENT) != 0) {
			space_unlock(space, enabled);
			return HAL_ERR_INVALID;
		}
	}

	/* Writes each page's entries, unwinding them all if a table cannot be made. */
	for (offset = 0; offset < size; offset += ARMV7_PAGE_SIZE) {
		shadow = walk_shadow(space, address + offset, 1);
		if (shadow == NULL) {
			for (rollback = 0; rollback < offset; rollback += ARMV7_PAGE_SIZE) {
				shadow = walk_shadow(space, address + rollback, 0);
				if (shadow != NULL)
					write_entry(shadow, 0);
			}

			/* Unlinks the tables left empty, then drops the stale translations and frees them. */
			detached = detach_empty_tables(space);
			flush_space(space);
			free_detached_tables(detached);
			space_unlock(space, enabled);
			return HAL_ERR_NOMEM;
		}

		/* A device page has nothing to record: it starts accessed, and dirty when writable. */
		entry = armv7_pte_shadow((uint64_t)paddr + offset, writable, executable, memory_type);
		if (memory_type == ARMV7_PTE_ATTR_DEVICE)
			entry |= ARMV7_PTE_ACCESSED | ARMV7_PTE_DIRTY;

		/* Code in the page is fetched correctly once it is executable. */
		sync_executable(entry);
		write_entry(shadow, entry);
	}

	/* Drops translations a processor may hold of the range's empty entries. */
	flush_space(space);

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
	struct armv7_space *space;
	struct armv7_table_page *detached;
	uint32_t *shadow;
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

	/* Clears each page's entries. */
	for (offset = 0; offset < size; offset += ARMV7_PAGE_SIZE) {
		shadow = walk_shadow(space, address + offset, 0);
		if (shadow != NULL)
			write_entry(shadow, 0);
	}

	/* Unlinks the tables left empty, drops the stale translations, then frees the tables. */
	detached = detach_empty_tables(space);
	flush_space(space);
	free_detached_tables(detached);

	space_unlock(space, enabled);

	/* Succeeded: the range is unmapped. */
	return HAL_OK;
}

/*
 * Changes a mapped user range's protection and reports what the old
 * translations recorded.
 *
 * A page is writable in the hardware only once it is dirty, so a store
 * through a translation that is stale when the write permission goes was
 * made to a page already recorded as dirty: the report covers it.
 */
int
hal_space_prot_query(
	hal_space_t handle,
	void *vaddr,
	size_t size,
	uint32_t attr,
	uint32_t *flags)
{
	struct armv7_space *space;
	uint32_t *shadow;
	uint32_t entry;
	uint32_t observed;
	uintptr_t address;
	uintptr_t offset;
	unsigned memory_type;
	bool enabled;
	int valid;
	int locked;
	int writable;
	int executable;

	/* Requires a user range and an access to give it. */
	address = (uintptr_t)vaddr;
	if (handle == HAL_SPACE_SYS)
		return HAL_ERR_INVALID;
	valid = valid_user(address, size);
	if (!valid)
		return HAL_ERR_INVALID;
	if ((attr & (HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC)) == 0)
		return HAL_ERR_INVALID;
	if ((attr & HAL_SPACE_WRITE) != 0 && (attr & HAL_SPACE_EXEC) != 0)
		return HAL_ERR_INVALID;
	writable = 0;
	if ((attr & HAL_SPACE_WRITE) != 0)
		writable = 1;
	executable = 0;
	if ((attr & HAL_SPACE_EXEC) != 0)
		executable = 1;

	/* Holds the space for the whole change. */
	locked = space_lock_handle(handle, &space, &enabled);
	if (!locked)
		return HAL_ERR_STATE;

	/* Refuses a range with any page unmapped, or a device page made executable. */
	for (offset = 0; offset < size; offset += ARMV7_PAGE_SIZE) {
		shadow = walk_shadow(space, address + offset, 0);
		if (shadow == NULL || (*shadow & ARMV7_PTE_PRESENT) == 0) {
			space_unlock(space, enabled);
			return HAL_ERR_INVALID;
		}

		/* Device memory never holds code. */
		memory_type = (*shadow & ARMV7_PTE_ATTR_MASK) >> ARMV7_PTE_ATTR_SHIFT;
		if (memory_type == ARMV7_PTE_ATTR_DEVICE && executable) {
			space_unlock(space, enabled);
			return HAL_ERR_INVALID;
		}
	}

	/* Changes each page's access, keeping its frame, its memory type and what it recorded. */
	observed = 0;
	for (offset = 0; offset < size; offset += ARMV7_PAGE_SIZE) {
		shadow = walk_shadow(space, address + offset, 0);
		entry = *shadow;
		observed |= armv7_pte_change_access(&entry, writable, executable);
		sync_executable(entry);
		write_entry(shadow, entry);
	}

	/* Drops the old translations on every processor before the report. */
	flush_space(space);

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
	struct armv7_space *space;
	uint32_t *shadow;
	uint32_t observed;
	bool enabled;
	int valid;
	int locked;

	/* Requires a user page and somewhere to report. */
	if (handle == HAL_SPACE_SYS || flags == NULL)
		return HAL_ERR_INVALID;
	valid = valid_user((uintptr_t)vaddr, ARMV7_PAGE_SIZE);
	if (!valid)
		return HAL_ERR_INVALID;

	/* Reads the page's shadow entry under the space's lock. */
	locked = space_lock_handle(handle, &space, &enabled);
	if (!locked)
		return HAL_ERR_STATE;

	observed = 0;
	shadow = walk_shadow(space, (uintptr_t)vaddr, 0);
	if (shadow != NULL && (*shadow & ARMV7_PTE_PRESENT) != 0) {
		observed = HAL_SPACE_PAGE_PRESENT;
		if ((*shadow & ARMV7_PTE_ACCESSED) != 0)
			observed |= HAL_SPACE_PAGE_ACCESSED;
		if ((*shadow & ARMV7_PTE_DIRTY) != 0)
			observed |= HAL_SPACE_PAGE_DIRTY;
	}

	space_unlock(space, enabled);

	/* Succeeded: the page's state. */
	*flags = observed;
	return HAL_OK;
}

/*
 * Clears the recorded access and dirty state of one user page.
 *
 * The hardware entry follows the shadow: a page no longer accessed faults
 * on its next access, and one no longer dirty on its next store, so the
 * state is recorded again from then on.
 */
int
hal_space_clear_flags(
	hal_space_t handle,
	void *vaddr,
	uint32_t flags)
{
	struct armv7_space *space;
	uint32_t *shadow;
	uint32_t entry;
	unsigned memory_type;
	bool enabled;
	int valid;
	int locked;

	/* Requires a user page and flags this port records. */
	if (handle == HAL_SPACE_SYS)
		return HAL_ERR_INVALID;
	valid = valid_user((uintptr_t)vaddr, ARMV7_PAGE_SIZE);
	if (!valid)
		return HAL_ERR_INVALID;
	if ((flags & ~(uint32_t)(HAL_SPACE_PAGE_ACCESSED | HAL_SPACE_PAGE_DIRTY)) != 0)
		return HAL_ERR_INVALID;

	/* Changes the page's entries under the space's lock. */
	locked = space_lock_handle(handle, &space, &enabled);
	if (!locked)
		return HAL_ERR_STATE;

	/* Refuses a page that is not mapped. */
	shadow = walk_shadow(space, (uintptr_t)vaddr, 0);
	if (shadow == NULL || (*shadow & ARMV7_PTE_PRESENT) == 0) {
		space_unlock(space, enabled);
		return HAL_ERR_INVALID;
	}

	/* A device page keeps its state: it is never tracked. */
	entry = *shadow;
	memory_type = (entry & ARMV7_PTE_ATTR_MASK) >> ARMV7_PTE_ATTR_SHIFT;
	if (memory_type != ARMV7_PTE_ATTR_DEVICE) {
		if ((flags & HAL_SPACE_PAGE_ACCESSED) != 0)
			entry &= ~(uint32_t)ARMV7_PTE_ACCESSED;
		if ((flags & HAL_SPACE_PAGE_DIRTY) != 0)
			entry &= ~(uint32_t)ARMV7_PTE_DIRTY;
	}

	/* Writes the entries, then drops the stale translation. */
	write_entry(shadow, entry);
	flush_space_page(space, (uintptr_t)vaddr);

	space_unlock(space, enabled);

	/* Succeeded: the page's state is cleared. */
	return HAL_OK;
}

/*
 * Drops the stale translations of a space on every processor.
 */
void
hal_space_flush_tlb(
	hal_space_t handle)
{
	struct armv7_space *space;
	bool enabled;
	int locked;

	/* The kernel's half: every translation, since kernel entries are global. */
	if (handle == HAL_SPACE_SYS) {
		armv7_tlb_flush_all();
		return;
	}

	/* A user space is flushed under its lock. */
	locked = space_lock_handle(handle, &space, &enabled);
	if (!locked)
		HAL_FATAL("invalid armv7 space flush");
	flush_space(space);
	space_unlock(space, enabled);
}

/*
 * Drops the stale translations of a range of a space on every processor.
 */
void
hal_space_flush_tlb_range(
	hal_space_t handle,
	void *vaddr,
	size_t size)
{
	uintptr_t address;
	uintptr_t end;
	size_t pages;

	/* An empty range has nothing stale. */
	if (size == 0)
		return;

	/* A user space, or a range too long to take a page at a time, is flushed whole. */
	pages = (size + ARMV7_PAGE_SIZE - 1U) / ARMV7_PAGE_SIZE;
	if (handle != HAL_SPACE_SYS || pages > SPACE_FLUSH_PAGE_LIMIT) {
		hal_space_flush_tlb(handle);
		return;
	}

	/* The kernel's pages one at a time, whatever their identifier. */
	address = (uintptr_t)vaddr & ~(uintptr_t)(ARMV7_PAGE_SIZE - 1U);
	end = (uintptr_t)vaddr + size;
	while (address < end) {
		armv7_tlb_flush_kernel_page(address);
		address += ARMV7_PAGE_SIZE;
	}
}

/*
 * Reports the size a table level maps: 1 a small page, 2 a section.
 */
size_t
hal_space_get_page_size(
	int level)
{
	/* Chooses the size of the level. */
	switch (level) {
	case 1:
		return ARMV7_PAGE_SIZE;
	case 2:
		return ARMV7_SECTION_SIZE;
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
		*minimum = ARMV7_USER_MINIMUM;
	if (limit != NULL)
		*limit = ARMV7_USER_LIMIT;
}

/*
 * Maps a device range into the kernel's device window.
 *
 * RAM is already in the direct map and is returned as it is; a range that
 * mixes RAM and devices is refused.  A write-combining request gets
 * normal uncached memory, which is how the architecture combines writes.
 */
int
hal_space_map_device(
	hal_physaddr_t paddr,
	size_t size,
	uint32_t attr,
	void **vaddr)
{
	unsigned memory_type;
	bool ram;
	bool touches;
	int error;

	/* Requires a destination and a non-empty range. */
	if (vaddr == NULL || size == 0)
		return HAL_ERR_INVALID;

	/* Refuses an executable window and the write-through behaviour the window does not offer. */
	if ((attr & HAL_SPACE_EXEC) != 0)
		return HAL_ERR_INVALID;
	if ((attr & HAL_SPACE_WRITETHRU) != 0)
		return HAL_ERR_UNSUPPORTED;

	/* Returns RAM's existing alias. */
	ram = armv7_range_is_ram((uint64_t)paddr, (uint64_t)size);
	if (ram) {
		*vaddr = armv7_phys_to_direct((uint64_t)paddr);
		return HAL_OK;
	}

	/* Refuses a range that mixes RAM and devices. */
	touches = armv7_range_touches_ram((uint64_t)paddr, (uint64_t)size);
	if (touches)
		return HAL_ERR_INVALID;

	/* Device memory, or normal uncached memory for combined writes. */
	memory_type = ARMV7_PTE_ATTR_DEVICE;
	if ((attr & HAL_SPACE_WC) != 0)
		memory_type = ARMV7_PTE_ATTR_NOCACHE;

	/* Takes or shares the window's sections. */
	error = armv7_space_map_device_window((uint64_t)paddr, size, memory_type, vaddr);
	if (error != HAL_OK)
		return error;

	/* Succeeded: the device is reachable in the window. */
	return HAL_OK;
}

/*
 * Releases a caller's use of the shared device window.
 *
 * The window's sections are shared by every driver of a block, so one
 * caller's release leaves them in place.
 */
int
hal_space_unmap_device(
	void *vaddr,
	size_t size)
{
	uintptr_t address;

	/* Requires a non-empty range inside the device window or the direct map. */
	address = (uintptr_t)vaddr;
	if (size == 0)
		return HAL_ERR_INVALID;
	if (address >= ARMV7_DEVICE_BASE && address < ARMV7_DEVICE_LIMIT) {
		if (size > ARMV7_DEVICE_LIMIT - address)
			return HAL_ERR_INVALID;
		return HAL_OK;
	}

	/* An address in the direct map was handed out as RAM. */
	if (address >= ARMV7_DIRECT_BASE && address < ARMV7_DIRECT_LIMIT)
		return HAL_OK;

	/* The address was never handed out by hal_space_map_device(). */
	return HAL_ERR_INVALID;
}

/*
 * Maps managed RAM a second time, uncached, for a device that does not
 * snoop the caches.
 *
 * The view takes free pages of the uncached window, normal non-cacheable
 * memory that is never executed.  The direct map's lines of the range are
 * written back and discarded first.  The kernel's own table must be in
 * use: the window's tables come from the page allocator.
 */
int
hal_pmem_map_uncached(
	hal_physaddr_t paddr,
	size_t size,
	void **vaddr)
{
	uint32_t *entry;
	uint32_t count;
	uint32_t first;
	uint32_t index;
	uint64_t physical;
	bool ram;
	bool enabled;
	int found;
	int error;

	/* Requires a destination and a page-aligned, non-empty range. */
	if (vaddr == NULL || size == 0)
		return HAL_ERR_INVALID;
	if (((uint64_t)paddr & (ARMV7_PAGE_SIZE - 1U)) != 0)
		return HAL_ERR_INVALID;
	if (((uint64_t)size & (ARMV7_PAGE_SIZE - 1U)) != 0)
		return HAL_ERR_INVALID;
	if ((uint64_t)size > (uint64_t)ARMV7_UNCACHED_PAGES * ARMV7_PAGE_SIZE)
		return HAL_ERR_NOMEM;

	/* Refuses anything but RAM, and a boot that has not moved to the kernel's table. */
	ram = armv7_range_is_ram((uint64_t)paddr, (uint64_t)size);
	if (!ram)
		return HAL_ERR_INVALID;
	if (!kernel_mapping_checked)
		return HAL_ERR_UNSUPPORTED;

	/* Writes back and drops the direct map's lines before they could shadow the view. */
	hal_dcache_clean_invalidate_range((uintptr_t)armv7_phys_to_direct((uint64_t)paddr), size);

	/* Takes a run of free pages and the tables under them. */
	count = (uint32_t)(size / ARMV7_PAGE_SIZE);
	enabled = armv7_spin_lock(&device_lock);

	/* Finds the run; a full window refuses the view. */
	found = find_uncached_run(count, &first);
	if (!found) {
		armv7_spin_unlock(&device_lock, enabled);

		return HAL_ERR_NOMEM;
	}

	/* Makes the run's tables; a failed allocation refuses the view. */
	error = make_uncached_tables(first, count);
	if (error != HAL_OK) {
		armv7_spin_unlock(&device_lock, enabled);

		return error;
	}

	/* Points each page at the RAM, normal non-cacheable, the kernel's alone, never executed. */
	for (index = 0; index < count; index++) {
		physical = (uint64_t)paddr + (uint64_t)index * ARMV7_PAGE_SIZE;
		entry = uncached_entry(first + index);
		*entry = ((uint32_t)physical & ARMV7_L2_BASE_MASK) | ARMV7_L2_SMALL | ARMV7_L2_TEX0 |
			 ARMV7_L2_S | ARMV7_L2_AF | ARMV7_L2_XN;
	}

	/* Marks the window's pages as taken by this view. */
	mark_uncached(first, count, 1);

	armv7_spin_unlock(&device_lock, enabled);

	/* Publishes the entries to the table walk; the pages had no translation before. */
	armv7_dsb();
	armv7_isb();

	/* Succeeded: the RAM is reachable uncached at the view. */
	*vaddr = (void *)(uintptr_t)(ARMV7_UNCACHED_BASE + (uintptr_t)first * ARMV7_PAGE_SIZE);
	return HAL_OK;
}

/*
 * Removes a view made by hal_pmem_map_uncached().
 *
 * The pages' translations are dropped and the pages go back to the
 * window; the direct map's lines of the RAM are discarded afterwards, so
 * that a line a speculative read brought in while the view existed is not
 * read later.
 */
int
hal_pmem_unmap_uncached(
	void *vaddr,
	size_t size)
{
	uint32_t *entry;
	uintptr_t address;
	uintptr_t page_address;
	uint32_t count;
	uint32_t first;
	uint32_t index;
	uint64_t physical;
	bool enabled;
	int used;

	/* Requires a page-aligned, non-empty range inside the window. */
	address = (uintptr_t)vaddr;
	if (size == 0)
		return HAL_ERR_INVALID;
	if ((address & (ARMV7_PAGE_SIZE - 1U)) != 0 || ((uint64_t)size & (ARMV7_PAGE_SIZE - 1U)) != 0)
		return HAL_ERR_INVALID;
	if (address < ARMV7_UNCACHED_BASE || address >= ARMV7_UNCACHED_LIMIT)
		return HAL_ERR_INVALID;
	if ((uint64_t)size > (uint64_t)ARMV7_UNCACHED_LIMIT - address)
		return HAL_ERR_INVALID;
	first = (uint32_t)((address - ARMV7_UNCACHED_BASE) / ARMV7_PAGE_SIZE);
	count = (uint32_t)(size / ARMV7_PAGE_SIZE);

	/* Refuses a range with a page no view uses, before anything is changed. */
	enabled = armv7_spin_lock(&device_lock);

	for (index = 0; index < count; index++) {
		used = uncached_page_used(first + index);
		if (!used) {
			armv7_spin_unlock(&device_lock, enabled);

			return HAL_ERR_INVALID;
		}
	}

	/* Unmaps each page and drops its translation and its direct map's lines. */
	for (index = 0; index < count; index++) {
		entry = uncached_entry(first + index);
		physical = (uint64_t)(*entry & ARMV7_L2_BASE_MASK);
		*entry = 0;
		page_address = ARMV7_UNCACHED_BASE + (uintptr_t)(first + index) * ARMV7_PAGE_SIZE;
		armv7_tlb_flush_kernel_page(page_address);
		hal_dcache_invalidate_range((uintptr_t)armv7_phys_to_direct(physical), ARMV7_PAGE_SIZE);
	}

	/* Gives the window's pages back for another view. */
	mark_uncached(first, count, 0);

	armv7_spin_unlock(&device_lock, enabled);

	/* Succeeded: the range is reachable through the direct map alone. */
	return HAL_OK;
}

/*
 * Reports how many user spaces and page-table pages exist.
 */
void
armv7_space_memory_stats(
	uint32_t *spaces,
	uint32_t *tables)
{
	bool enabled;

	/* Samples the space count under the registry lock. */
	enabled = armv7_spin_lock(&space_registry_lock);

	if (spaces != NULL)
		*spaces = space_count;

	armv7_spin_unlock(&space_registry_lock, enabled);

	/* The table count moves atomically. */
	if (tables != NULL)
		*tables = __atomic_load_n(&page_table_count, __ATOMIC_RELAXED);
}

/* Allocates a zeroed page of tables and counts it. */
static int
alloc_table_page(
	hal_physaddr_t *physical,
	uint32_t **entries)
{
	int error;

	/* One page, page-aligned. */
	error = hal_pmem_alloc(ARMV7_PAGE_SIZE, ARMV7_PAGE_SIZE, physical);
	if (error != HAL_OK)
		return error;

	/* Clears it through the direct map. */
	*entries = armv7_phys_to_direct((uint64_t)*physical);
	hal_memset(*entries, 0, ARMV7_PAGE_SIZE);
	(void)__atomic_fetch_add(&page_table_count, 1U, __ATOMIC_RELAXED);

	/* Succeeded: the page is empty and the caller's. */
	return HAL_OK;
}

/*
 * Builds the direct map: each section of the RAM span that holds RAM, as
 * one section where all of it is RAM, otherwise page by page.
 */
static void
build_direct_map(
	void)
{
	uint64_t first;
	uint64_t end;
	uint64_t physical;
	bool touches;

	/* Visits each section of the RAM span. */
	first = armv7_ram_base() & ~(uint64_t)(ARMV7_SECTION_SIZE - 1U);
	end = armv7_ram_end();
	for (physical = first; physical < end; physical += ARMV7_SECTION_SIZE) {
		/* A section with no RAM in it stays unmapped. */
		touches = armv7_range_touches_ram(physical, ARMV7_SECTION_SIZE);
		if (!touches)
			continue;
		build_direct_section(physical);
	}
}

/* Maps one section of the direct map, whole or through a table of pages. */
static void
build_direct_section(
	uint64_t physical)
{
	hal_physaddr_t pages_physical;
	uint32_t *pages;
	uint64_t page;
	unsigned index;
	unsigned page_index;
	bool whole;
	bool ram;
	int error;

	/* A section that is all RAM is one entry. */
	index = (unsigned)((physical - ARMV7_PHYS_BASE + ARMV7_DIRECT_BASE) >> ARMV7_SECTION_SHIFT);
	whole = armv7_range_is_ram(physical, ARMV7_SECTION_SIZE);
	if (whole) {
		kernel_l1[index] = (uint32_t)physical | SPACE_SECTION_DATA;
		return;
	}

	/* Otherwise a table of pages, mapping each page of RAM. */
	error = alloc_table_page(&pages_physical, &pages);
	if (error != HAL_OK)
		HAL_FATAL("armv7 direct map table allocation failed");
	for (page_index = 0; page_index < ARMV7_L2_ENTRIES; page_index++) {
		page = physical + (uint64_t)page_index * ARMV7_PAGE_SIZE;
		ram = armv7_range_is_ram(page, ARMV7_PAGE_SIZE);
		if (ram)
			pages[page_index] = armv7_pte_kernel_page(page, 1, 0);
	}

	/* Links the table of pages into the kernel's table. */
	kernel_l1[index] = armv7_pte_table_pointer(pages_physical);
}

/*
 * Maps the sections of the direct map that hold the kernel image page by
 * page: text read-execute, read-only data read-only, the rest read-write,
 * and the RAM of those sections outside the image as the direct map has
 * it.
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
	uint64_t physical;
	unsigned page;
	bool ram;
	int writable;
	int executable;
	int error;

	/* The image's pages, and the sections that hold them. */
	first = (uintptr_t)__kernel_vma_start & ~(uintptr_t)(ARMV7_SECTION_SIZE - 1U);
	image_end = ((uintptr_t)__kernel_vma_end + ARMV7_PAGE_SIZE - 1U) & ~(uintptr_t)(ARMV7_PAGE_SIZE - 1U);
	end = (image_end + ARMV7_SECTION_SIZE - 1U) & ~(uintptr_t)(ARMV7_SECTION_SIZE - 1U);
	pages = NULL;
	for (address = first; address < end; address += ARMV7_PAGE_SIZE) {
		page = (unsigned)((address % ARMV7_SECTION_SIZE) / ARMV7_PAGE_SIZE);
		physical = armv7_virt_to_phys((const void *)address);

		/* A new section gets its table of pages in place of its entry. */
		if (page == 0) {
			error = alloc_table_page(&pages_physical, &pages);
			if (error != HAL_OK)
				HAL_FATAL("armv7 kernel window table allocation failed");
			kernel_l1[address >> ARMV7_SECTION_SHIFT] = armv7_pte_table_pointer(pages_physical);
		}

		/* Outside the image: RAM as the direct map has it. */
		if (address < (uintptr_t)__kernel_vma_start || address >= image_end) {
			ram = armv7_range_is_ram(physical, ARMV7_PAGE_SIZE);
			if (ram)
				pages[page] = armv7_pte_kernel_page(physical, 1, 0);
			continue;
		}

		/* Chooses the page's permissions by the part of the image it lies in. */
		writable = 1;
		executable = 0;
		if (address >= (uintptr_t)__kernel_text_start && address < (uintptr_t)__kernel_text_end) {
			writable = 0;
			executable = 1;
		} else if (address >= (uintptr_t)__kernel_rodata_start && address < (uintptr_t)__kernel_rodata_end) {
			writable = 0;
		}

		/* Writes the page's entry. */
		pages[page] = armv7_pte_kernel_page(physical, writable, executable);
	}
}

/*
 * Moves the boot processor from the boot table to the kernel's: TTBR1
 * takes the kernel's table, TTBCR splits the space, and TTBR0 the empty
 * lower half under the reserved identifier; then every translation of
 * the boot table is dropped.
 *
 * The code runs in the direct map, which both tables map to the same
 * physical addresses, so the instructions are found whichever table
 * translates them during the move.  The split comes before TTBR0 leaves
 * the boot table: until then TTBR0 translates the upper half too, and
 * the boot table (16 KiB, aligned to it) still serves as the lower half's
 * table after the split, so a table walk between two of the writes always
 * finds a valid table.
 */
static void
move_to_kernel_table(
	void)
{
	uint32_t ttbr1;
	uint32_t ttbr0;

	/* Publishes the kernel's table to the table walk. */
	armv7_dsb();

	/* TTBR1, then the split, then the empty lower half. */
	ttbr1 = armv7_pte_ttbr(armv7_virt_to_phys(kernel_l1));
	ttbr0 = armv7_pte_ttbr(armv7_virt_to_phys(empty_user_l1));
	armv7_write_ttbr1(ttbr1);
	armv7_isb();
	armv7_write_ttbcr(ARMV7_TTBCR_SPLIT);
	armv7_isb();
	armv7_write_contextidr(0);
	armv7_isb();
	armv7_write_ttbr0(ttbr0);
	armv7_isb();

	/* Drops every translation the boot table left. */
	armv7_tlb_flush_all();
}

/* Reports the small-page descriptor the kernel's table has for an image address. */
static uint32_t
kernel_page_descriptor(
	uintptr_t address)
{
	const uint32_t *pages;
	uint32_t entry;

	/* Walks the first-level entry to the table of pages below it. */
	entry = kernel_l1[address >> ARMV7_SECTION_SHIFT];
	if ((entry & ARMV7_L1_TYPE_MASK) != ARMV7_L1_TYPE_TABLE)
		return 0;
	pages = armv7_phys_to_direct(entry & ARMV7_L1_TABLE_BASE_MASK);

	/* Succeeded: the page's descriptor. */
	return pages[(address >> ARMV7_PAGE_SHIFT) & (ARMV7_L2_ENTRIES - 1U)];
}

/*
 * Finds the device window's run for a request: the one that maps the same
 * sections with the same memory type, else the lowest free one.  The
 * caller holds device_lock.  Reports 0 when there is neither.
 */
static int
find_device_run(
	uint32_t first,
	uint32_t count,
	unsigned memory_type,
	uint32_t *start)
{
	const struct space_device_section *section;
	uint32_t candidate;
	uint32_t index;
	int matches;
	int free_run;

	/* Looks for a run that maps the sections already. */
	for (candidate = 0; candidate + count <= ARMV7_DEVICE_SECTIONS; candidate++) {
		matches = 1;
		for (index = 0; index < count; index++) {
			section = &device_sections[candidate + index];
			if (!section->used || section->physical != first + index || section->memory_type != memory_type) {
				matches = 0;
				break;
			}
		}

		/* A run that maps all of them is shared. */
		if (matches) {
			*start = candidate;
			return 1;
		}
	}

	/* Otherwise the lowest run of free sections. */
	for (candidate = 0; candidate + count <= ARMV7_DEVICE_SECTIONS; candidate++) {
		free_run = 1;
		for (index = 0; index < count; index++) {
			if (device_sections[candidate + index].used) {
				free_run = 0;
				break;
			}
		}

		/* A free run is taken. */
		if (free_run) {
			*start = candidate;
			return 1;
		}
	}

	/* The window is full. */
	return 0;
}

/*
 * Installs a lower-half table and an identifier on the calling processor.
 *
 * The identifier is set to the reserved 0 while TTBR0 changes, so a table
 * walk made in between cannot tag the new table's entries with the old
 * identifier or the old table's with the new one; the barriers before the
 * changes of the context id are those of Cortex-A9 erratum 754322
 * (Procedure referenced: ARM DDI 0406C, synchronization of changes of ASID
 * and TTBR; Linux v6.12 arch/arm/mm/proc-v7-2level.S, the dsb of
 * CONFIG_ARM_ERRATA_754322).
 */
static void
select_table(
	uint32_t ttbr0,
	uint32_t asid)
{
	/* Moves to the reserved identifier, then to the new table. */
	armv7_dsb();
	armv7_write_contextidr(0);
	armv7_isb();
	armv7_write_ttbr0(ttbr0);
	armv7_isb();

	/* Moves to the new identifier. */
	armv7_dsb();
	armv7_write_contextidr(asid & 0xffU);
	armv7_isb();
}

/*
 * Gives a space an identifier of the current generation, if it has none.
 *
 * When the identifiers run out, the generation moves on and every
 * translation is dropped, so an identifier handed out again tags nothing
 * stale.  With one processor nothing else runs on an old identifier
 * meanwhile.
 */
static void
assign_asid(
	struct armv7_space *space)
{
	bool enabled;
	int current;

	/* Changes the identifiers under their lock. */
	enabled = armv7_spin_lock(&asid_lock);

	current = asid_current(space);
	if (!current) {
		/* Starts a new generation once every identifier is taken. */
		if (next_asid > SPACE_ASID_LAST) {
			asid_generation++;
			if (asid_generation == 0)
				asid_generation = 1;
			next_asid = 1;
			armv7_tlb_flush_all();
		}

		/* Hands out the next identifier. */
		space->asid = next_asid;
		space->asid_generation = asid_generation;
		next_asid++;
	}

	armv7_spin_unlock(&asid_lock, enabled);
}

/* Reports whether a space's identifier belongs to the current generation. */
static int
asid_current(
	const struct armv7_space *space)
{
	/* An identifier of an old generation may name another space now. */
	if (space->asid_generation != asid_generation)
		return 0;

	/* Succeeded: the identifier is the space's. */
	return 1;
}

/*
 * Drops every translation of a user space on every processor.  A space
 * with no identifier of the current generation has none.
 */
static void
flush_space(
	struct armv7_space *space)
{
	uint32_t asid;
	bool enabled;
	int current;

	/* Reads the identifier under its lock. */
	enabled = armv7_spin_lock(&asid_lock);

	asid = space->asid;
	current = asid_current(space);

	armv7_spin_unlock(&asid_lock, enabled);

	/* The space's translations, broadcast to the inner shareable domain. */
	if (current)
		armv7_tlb_flush_asid(asid);
}

/* Drops one page's translations of a user space on every processor. */
static void
flush_space_page(
	struct armv7_space *space,
	uintptr_t address)
{
	uint32_t asid;
	bool enabled;
	int current;

	/* Reads the identifier under its lock. */
	enabled = armv7_spin_lock(&asid_lock);

	asid = space->asid;
	current = asid_current(space);

	armv7_spin_unlock(&asid_lock, enabled);

	/* The page's translation, broadcast to the inner shareable domain. */
	if (current)
		armv7_tlb_flush_page(address, asid);
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
	struct armv7_space **result,
	bool *irq_enabled)
{
	struct armv7_space *space;
	unsigned me;
	bool enabled;
	bool acquired;

	/* Looks the handle up among the live spaces. */
	enabled = armv7_spin_lock(&space_registry_lock);
	for (space = space_registry; space != NULL; space = space->registry_next) {
		if ((hal_space_t)space == handle)
			break;
	}

	/* A handle that names no live space is refused. */
	if (space == NULL || space->destroying) {
		armv7_spin_unlock(&space_registry_lock, enabled);
		return 0;
	}

	/* A second operation on the same space from the same processor is a fault. */
	me = hal_cpu_current() + 1U;
	if (space->lock_owner == me)
		HAL_FATAL("recursive armv7 space operation");

	/* Waits for another processor's operation on the space to end. */
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
	struct armv7_space *space,
	bool enabled)
{
	unsigned me;

	/* Only the processor that holds the space may release it. */
	me = hal_cpu_current() + 1U;
	if (space->lock == 0 || space->lock_owner != me)
		HAL_FATAL("invalid armv7 space unlock");

	/* Releases the space, publishing every change made under it. */
	space->lock_owner = 0;
	armv7_spin_unlock(&space->lock, enabled);
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
	if ((address & (ARMV7_PAGE_SIZE - 1U)) != 0)
		return 0;
	if ((size & (ARMV7_PAGE_SIZE - 1U)) != 0)
		return 0;

	/* The first page is never mapped, and the range must end inside the user half. */
	if (address < ARMV7_USER_MINIMUM)
		return 0;
	if (address >= ARMV7_USER_LIMIT)
		return 0;
	if (size > ARMV7_USER_LIMIT - address)
		return 0;

	/* Succeeded: the range can be mapped. */
	return 1;
}

/*
 * Checks a user mapping's attributes and the memory it maps, and chooses
 * its memory type: some access, never written and executed at once;
 * managed RAM as normal cached memory, or the caller's device pages, which
 * must not be RAM and are never executed.  An uncached, write-through or
 * write-combining view of RAM is not offered.
 */
static int
user_memory_type(
	uint32_t attr,
	uint64_t physical,
	size_t size,
	unsigned *memory_type)
{
	bool ram;
	bool touches;

	/* A mapping must allow some access. */
	if ((attr & (HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC)) == 0)
		return HAL_ERR_INVALID;

	/* A page is never both writable and executable. */
	if ((attr & HAL_SPACE_WRITE) != 0 && (attr & HAL_SPACE_EXEC) != 0)
		return HAL_ERR_INVALID;

	/* The caller's device pages: never RAM, never executed. */
	if ((attr & HAL_SPACE_DEVICE) != 0) {
		if ((attr & HAL_SPACE_EXEC) != 0)
			return HAL_ERR_INVALID;
		touches = armv7_range_touches_ram(physical, (uint64_t)size);
		if (touches)
			return HAL_ERR_INVALID;
		*memory_type = ARMV7_PTE_ATTR_DEVICE;
		return HAL_OK;
	}

	/* RAM keeps the direct map's cache behaviour. */
	if ((attr & (HAL_SPACE_NOCACHE | HAL_SPACE_WRITETHRU | HAL_SPACE_WC)) != 0)
		return HAL_ERR_UNSUPPORTED;
	ram = armv7_range_is_ram(physical, (uint64_t)size);
	if (!ram)
		return HAL_ERR_INVALID;

	/* Succeeded: normal cached memory. */
	*memory_type = ARMV7_PTE_ATTR_NORMAL;
	return HAL_OK;
}

/*
 * Finds the shadow entry of a user address, making its page of tables
 * when asked to.  Reports NULL when the page of tables is missing and not
 * to be made, or cannot be made.
 */
static uint32_t *
walk_shadow(
	struct armv7_space *space,
	uintptr_t address,
	int create)
{
	struct armv7_table_page *page;
	uint32_t *entries;
	uint32_t entry;
	unsigned first_index;
	int error;

	/* The pair of first-level entries of the address's 2 MiB. */
	first_index = (unsigned)(address >> ARMV7_SECTION_SHIFT) & ~1U;
	entry = space->l1[first_index];

	/* An existing page of tables lies 2 KiB below the first hardware table. */
	if (entry != 0) {
		entries = armv7_phys_to_direct((entry & ARMV7_L1_TABLE_BASE_MASK) - ARMV7_TABLE_PAGE_HARDWARE);
		return &entries[armv7_pte_index(address)];
	}

	/* A missing page is made only when asked to. */
	if (!create)
		return NULL;

	/* Allocates the record of the page. */
	page = kernel_alloc(sizeof(*page));
	if (page == NULL)
		return NULL;

	/* Allocates the page itself, empty. */
	error = alloc_table_page(&page->memory, &page->entries);
	if (error != HAL_OK) {
		kernel_free(page);
		return NULL;
	}

	/* Lists the page with the space's. */
	page->first_index = first_index;
	page->next = space->tables;
	space->tables = page;

	/* Points the pair of first-level entries at the two hardware tables. */
	space->l1[first_index] = armv7_pte_table_pointer(page->memory + ARMV7_TABLE_PAGE_HARDWARE);
	space->l1[first_index + 1U] = armv7_pte_table_pointer(page->memory + ARMV7_TABLE_PAGE_HARDWARE + ARMV7_L2_TABLE_BYTES);
	armv7_dsb();

	/* Succeeded: the entry of the address in the new page. */
	return &page->entries[armv7_pte_index(address)];
}

/*
 * Writes a shadow entry and the hardware entry made from it, and
 * publishes the hardware entry to the table walk.
 */
static void
write_entry(
	uint32_t *shadow,
	uint32_t value)
{
	/* The shadow, then the descriptor the walk reads. */
	shadow[0] = value;
	shadow[SPACE_HARDWARE_WORDS] = armv7_pte_hardware(value);
	armv7_dsb();
}

/* Reports whether a page of tables maps nothing. */
static int
table_page_empty(
	const struct armv7_table_page *page)
{
	unsigned index;

	/* Looks for a present shadow entry. */
	for (index = 0; index < 2U * ARMV7_L2_ENTRIES; index++) {
		if ((page->entries[index] & ARMV7_PTE_PRESENT) != 0)
			return 0;
	}

	/* The page is empty. */
	return 1;
}

/*
 * Unlinks every empty page of tables of a space and reports them for
 * freeing after the flush.
 */
static struct armv7_table_page *
detach_empty_tables(
	struct armv7_space *space)
{
	struct armv7_table_page *detached;
	struct armv7_table_page **link;
	struct armv7_table_page *page;
	int empty;

	/* Passes over the pages once; a page has no child to wait for. */
	detached = NULL;
	link = &space->tables;
	while (*link != NULL) {
		page = *link;

		/* A page that still maps something stays. */
		empty = table_page_empty(page);
		if (!empty) {
			link = &page->next;
			continue;
		}

		/* Unlinks the page from its pair of entries and from the space's list. */
		space->l1[page->first_index] = 0;
		space->l1[page->first_index + 1U] = 0;
		*link = page->next;
		page->next = detached;
		detached = page;
	}

	/* Publishes the cleared entries before the flush. */
	armv7_dsb();

	/* Succeeded: the pages to free once no walk can reach them. */
	return detached;
}

/* Frees pages of tables detached by detach_empty_tables(). */
static void
free_detached_tables(
	struct armv7_table_page *page)
{
	struct armv7_table_page *next;

	/* Frees each page and its record, and stops counting it. */
	while (page != NULL) {
		next = page->next;
		(void)hal_pmem_free(&page->memory, ARMV7_PAGE_SIZE);
		kernel_free(page);
		(void)__atomic_fetch_sub(&page_table_count, 1U, __ATOMIC_RELAXED);
		page = next;
	}
}

/*
 * Makes code written through the data side visible to instruction fetch
 * before a user page becomes executable.
 */
static void
sync_executable(
	uint32_t shadow)
{
	/* Only a present page that is to be executed. */
	if ((shadow & ARMV7_PTE_PRESENT) == 0)
		return;
	if ((shadow & ARMV7_PTE_EXEC) == 0)
		return;

	/* Its bytes through the kernel's direct map. */
	hal_sync_instruction_stream(armv7_phys_to_direct(shadow & ARMV7_PTE_FRAME_MASK), ARMV7_PAGE_SIZE);
}

/*
 * Finds the lowest run of free pages of the uncached window.  The caller
 * holds device_lock.  Reports 0 when there is none.
 */
static int
find_uncached_run(
	uint32_t count,
	uint32_t *start)
{
	uint32_t candidate;
	uint32_t length;
	uint32_t page;
	int used;

	/* Walks the window, counting the free pages since the last used one. */
	candidate = 0;
	length = 0;
	for (page = 0; page < ARMV7_UNCACHED_PAGES; page++) {
		used = uncached_page_used(page);
		if (used) {
			candidate = page + 1U;
			length = 0;
			continue;
		}

		/* A run long enough is taken. */
		length++;
		if (length == count) {
			*start = candidate;
			return 1;
		}
	}

	/* The window has no such run. */
	return 0;
}

/*
 * Makes the tables under a run of the uncached window's pages, a page of
 * four sections' tables at a time.  The caller holds device_lock.
 */
static int
make_uncached_tables(
	uint32_t first,
	uint32_t count)
{
	hal_physaddr_t pages_physical;
	uint32_t *pages;
	uint32_t section;
	uint32_t last_section;
	uint32_t group;
	uint32_t index;
	int error;

	/* Visits the sections the run touches. */
	section = first / ARMV7_L2_ENTRIES;
	last_section = (first + count - 1U) / ARMV7_L2_ENTRIES;
	for (; section <= last_section; section++) {
		/* A section that has its table already is left as it is. */
		if (kernel_l1[SPACE_UNCACHED_FIRST_INDEX + section] != 0)
			continue;

		/* A new page holds the tables of the section's group of four. */
		error = alloc_table_page(&pages_physical, &pages);
		if (error != HAL_OK)
			return error;

		/* Links each table of the page to its section. */
		group = section - section % SPACE_UNCACHED_TABLES_PER_PAGE;
		for (index = 0; index < SPACE_UNCACHED_TABLES_PER_PAGE; index++) {
			kernel_l1[SPACE_UNCACHED_FIRST_INDEX + group + index] =
				armv7_pte_table_pointer(pages_physical + (hal_physaddr_t)index * ARMV7_L2_TABLE_BYTES);
		}
	}

	/* Publishes the new first-level entries to the table walk. */
	armv7_dsb();

	/* Succeeded: every page of the run has its table. */
	return HAL_OK;
}

/*
 * Reports the second-level entry of one page of the uncached window,
 * through the direct map; its section's table must exist.
 */
static uint32_t *
uncached_entry(
	uint32_t page)
{
	uint32_t *table;
	uint32_t pointer;

	/* The section's table, from its first-level entry. */
	pointer = kernel_l1[SPACE_UNCACHED_FIRST_INDEX + page / ARMV7_L2_ENTRIES];
	table = armv7_phys_to_direct((uint64_t)(pointer & ARMV7_L1_TABLE_BASE_MASK));

	/* Succeeded: the page's entry. */
	return &table[page % ARMV7_L2_ENTRIES];
}

/* Marks a run of the uncached window's pages used or free; the caller holds device_lock. */
static void
mark_uncached(
	uint32_t first,
	uint32_t count,
	int used)
{
	uint32_t page;

	/* Sets or clears each page's bit. */
	for (page = first; page < first + count; page++) {
		if (used) {
			uncached_used[page / 32U] |= 1U << (page % 32U);
		} else {
			uncached_used[page / 32U] &= ~(1U << (page % 32U));
		}
	}
}

/* Reports whether a page of the uncached window is used; the caller holds device_lock. */
static int
uncached_page_used(
	uint32_t page)
{
	/* A page past the window is never free. */
	if (page >= ARMV7_UNCACHED_PAGES)
		return 1;

	/* A used page has its bit set. */
	if ((uncached_used[page / 32U] & (1U << (page % 32U))) != 0)
		return 1;

	/* Succeeded: the page is free. */
	return 0;
}
