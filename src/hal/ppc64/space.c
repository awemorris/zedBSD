/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 64-bit PowerPC address spaces.
 *
 * The hashed page table takes one of two forms, which the board chooses.
 * In a logical partition the hypervisor owns the table, and the HAL
 * enters and removes its entries with hypervisor calls.  On a processor
 * the kernel runs directly, such as the PowerPC 970, the table is in RAM
 * the board set aside, and the HAL writes its entries and invalidates the
 * translation buffer itself.  RAM, the device windows and the
 * framebuffer are mapped at their
 * own addresses in the large page the processor offers (64 KiB in a
 * partition, 16 MiB on the 970), entered once and never removed.  The
 * user range, segments 0x10 to 0x27, uses 4 KiB pages: each space keeps
 * its pages in a two-level table of its own, and a storage fault on a
 * user address enters the page from there, so the hashed table is only a
 * cache of them.
 *
 * Each segment has a virtual segment id: the kernel's windows and the
 * kernel alone have fixed ones, a user space its own, which the segment
 * lookaside buffer's user entries are reloaded with at each switch.
 *
 * Accessed and modified are kept in the space's table: a page enters
 * read-only until it is written, and the first write's protection fault
 * records the change and enters it writable.
 *
 * Values referenced: Power ISA Version 3.0B, Book III (the segment
 * lookaside buffer entry, the hashed page table entry, the 256 MiB segment
 * hash); QEMU v10.0.0 hw/ppc/spapr_vhyp_mmu.c and target/ppc/mmu-hash64.c
 * (the H_ENTER and H_REMOVE arguments, the storage attributes they accept
 * and the entry's compare value).
 */

#include <hal/hal.h>
#include "asm.h"
#include "board.h"
#include "defs.h"
#include "page.h"
#include "space.h"

/*
 * The segment lookaside buffer: valid, the user's key, the large page
 * (64 KiB with its size field, or the 970's one large size, 16 MiB), the
 * entries the HAL uses.
 */
#define SLB_VALID		0x08000000UL
#define SLB_KEY_USER		0x400UL
#define SLB_LARGE_64K		0x110UL
#define SLB_LARGE_16M		0x100UL
#define SLB_RAM_FIRST		0U

/* The segment ids of the kernel's windows, of the kernel alone's user segments, and of user spaces. */
#define VSID_RAM		0x100000UL
#define VSID_KERNEL_WINDOW	0x200000UL
#define VSID_SYSTEM_USER	0x300000UL
#define VSID_USER		0x1000000UL

/*
 * A hashed table entry's first word: valid, secondary hash, large page,
 * and one of the bits the architecture leaves to software, which marks
 * the kernel's windows in a table the HAL writes itself so that no user
 * page takes their place; the compare's mask.
 */
#define HPTE_VALID		0x1UL
#define HPTE_SECONDARY		0x2UL
#define HPTE_LARGE		0x4UL
#define HPTE_BOLTED		0x10UL
#define HPTE_FLAGS_MASK		0x7fUL
#define HPTE_AVPN_MASK		0x3fffffffffffff80UL

/* A hashed table entry's second word: a 64 KiB page, referenced, changed, uncached, coherent, guarded, the access. */
#define HPTE_LARGE_64K		0x1000UL
#define HPTE_REFERENCED		0x100UL
#define HPTE_CHANGED		0x80UL
#define HPTE_UNCACHED		0x20UL
#define HPTE_COHERENT		0x10UL
#define HPTE_GUARDED		0x8UL
#define HPTE_SUPERVISOR		0x0UL
#define HPTE_READ_WRITE		0x2UL
#define HPTE_READ_ONLY		0x3UL
#define HPTE_RPN_MASK		0x0ffffffffffff000UL

/* The hypervisor's flags and answers. */
#define H_AVPN			(1UL << (63 - 32))
#define H_SUCCESS		0

/*
 * A table the HAL writes itself: its smallest size, the slots of a group,
 * the bits of a 4 KiB page's number the group index carries, the
 * translation buffer's congruence classes on the 970, and the address
 * bits an invalidation names.
 *
 * Values referenced: PowerPC Architecture Book III Version 2.02 (the
 * SDR1 register, the page table update and invalidation sequence); IBM
 * PowerPC 970FX RISC Microprocessor User's Manual Version 1.7 (the 256
 * congruence classes of the translation buffer and tlbiel's index).
 */
#define TABLE_SHIFT_MINIMUM	18U
#define TABLE_GROUP_SLOTS	8U
#define TABLE_HASH_PAGE_MASK	0x7ffUL
#define TLB_CONGRUENCE_CLASSES	256U
#define TLBIE_ADDRESS_MASK	0x0000fffffffff000UL

/* The space's own leaf entry: the page, and the software's bits. */
#define LEAF_VALID		0x1UL
#define LEAF_WRITE		0x2UL
#define LEAF_EXEC		0x4UL
#define LEAF_ACCESSED		0x8UL
#define LEAF_MODIFIED		0x10UL
#define LEAF_PAGE_MASK		0xfffffffffffff000UL

/* A leaf table covers 2 MiB; the root, of six pages, the user range. */
#define LEAF_ENTRIES		512U
#define LEAF_SHIFT		21
#define ROOT_ENTRIES		((unsigned)((PPC64_USER_LIMIT - PPC64_USER_MINIMUM) >> LEAF_SHIFT))
#define ROOT_BYTES		(ROOT_ENTRIES * 8U)

/* The groups of the hashed table, and the victim of a full group. */
static uint64_t pteg_mask;
static unsigned evict_next;

/*
 * The form of the hashed table, set once at boot: whether the hypervisor
 * keeps it, and otherwise its entries, two doublewords each, which the
 * HAL writes with interrupts off.
 */
static int table_by_hypervisor;
static volatile uint64_t *table_entries;

/*
 * The large page of the kernel's windows, set once at boot, and how the
 * segment entry and the table entry's second word say it.
 */
static unsigned large_page_shift;
static uint64_t slb_large;
static uint64_t hpte_large_size;

/*
 * The kernel's segments beyond RAM, set once at boot: the device windows
 * and the framebuffer's, by their segment number, and the first segment
 * lookaside buffer entry the user segments take after them.  A device
 * range in one of the window segments is reached at its own address.
 */
static uint64_t kernel_window_segments[PPC64_BOARD_WINDOWS_MAX + 1U];
static unsigned kernel_window_count;
static unsigned device_window_count;
static unsigned slb_user_first;

/*
 * The RAM the kernel maps from address 0, set once at boot: its 256 MiB
 * segments, which take the first segment lookaside buffer entries, and
 * its end, which the page allocator manages up to.
 */
static unsigned ram_segment_count;
static uint64_t ram_limit;

/* The space selected now. */
static hal_space_t current_space;

/*
 * Every user space that exists, the number the next one gets, how many
 * there are and how many table pages they hold.  Kept with interrupts off.
 */
static struct ppc64_space *space_registry;
static uint32_t next_space_id = 1;
static uint32_t space_count;
static uint32_t page_table_count;

static int space_lock_handle(hal_space_t handle, struct ppc64_space **result, bool *irq_enabled);
static int valid_user(uintptr_t address, size_t size);
static int valid_attributes(uint32_t attr);
static uint64_t *walk_leaf(struct ppc64_space *space, uintptr_t address, int create);
static void load_user_segments(uint64_t first_vsid);
static void choose_large_page(unsigned shift);
static unsigned add_kernel_window(uint64_t esid);
static void map_kernel_window(uint64_t address, uint64_t size, uint64_t storage);
static int in_device_window(hal_physaddr_t paddr, size_t size);
static void table_init(uint64_t base, uint32_t shift);
static int enter(uint64_t vsid, uintptr_t address, unsigned page_shift, uint64_t second, int bolted, uint64_t *first_out);
static int enter_by_hypervisor(uint64_t hash, uint64_t first, uint64_t second, uint64_t *first_out);
static int enter_direct(uint64_t hash, uint64_t first, uint64_t second, uint64_t *first_out);
static void write_slot(volatile uint64_t *slot, uint64_t first, uint64_t second);
static void evict_slot(volatile uint64_t *slot, uint64_t group);
static void flush_translation(uint64_t vsid, uint64_t epn);
static void drop_entry(uint64_t vsid, uintptr_t address);
static void drop_by_hypervisor(uint64_t hash, uint64_t avpn);
static void drop_direct(uint64_t vsid, uint64_t epn, uint64_t hash, uint64_t avpn);
static uint64_t user_vsid(const struct ppc64_space *space, uintptr_t address);
static void free_tables(struct ppc64_space *space);

/*
 * Maps RAM, the device windows and the framebuffer in large pages, loads
 * the segment lookaside buffer and turns translation on.  The board's firmware is not
 * called after this; translation is off when it starts.
 */
void
ppc64_space_init(
	const struct ppc64_board_info *board)
{
	uint64_t memory_size;
	uint64_t framebuffer_base;
	uint64_t framebuffer_size;
	uint64_t large_page_size;
	uint64_t address;
	uint64_t first;
	uint64_t esid;
	unsigned index;
	unsigned window_budget;
	int framebuffer_in_window;
	int error;

	/* The table's groups: 128 bytes each. */
	if (board->htab_shift < TABLE_SHIFT_MINIMUM)
		HAL_FATAL("ppc64 firmware reports no hashed table size");
	pteg_mask = (1UL << (board->htab_shift - 7U)) - 1U;

	/* The kernel's large page, which the board names. */
	choose_large_page(board->large_page_shift);
	large_page_size = 1UL << large_page_shift;

	/* The table's form: the hypervisor's, or the HAL's own, emptied and named to the processor. */
	table_by_hypervisor = 0;
	if (board->under_hypervisor) {
		table_by_hypervisor = 1;
	} else {
		table_init(board->htab_base, board->htab_shift);
	}

	/*
	 * The RAM the kernel maps: what the board has up to the limit, and
	 * no more segments than the buffer holds beside the user's and the
	 * windows'.
	 */
	memory_size = board->memory_size;
	if (memory_size > PPC64_RAM_LIMIT_MAX)
		memory_size = PPC64_RAM_LIMIT_MAX;
	ram_segment_count = (unsigned)((memory_size + (1UL << PPC64_SEGMENT_SHIFT) - 1U) >> PPC64_SEGMENT_SHIFT);
	if (ram_segment_count == 0)
		ram_segment_count = 1;
	window_budget = PPC64_USER_SEGMENT_COUNT + board->window_count + 1U;
	if (board->window_count > PPC64_BOARD_WINDOWS_MAX || board->slb_entries <= window_budget)
		HAL_FATAL("ppc64 board names too small a segment lookaside buffer");
	if (ram_segment_count > board->slb_entries - window_budget)
		ram_segment_count = board->slb_entries - window_budget;
	if (memory_size > (uint64_t)ram_segment_count << PPC64_SEGMENT_SHIFT)
		memory_size = (uint64_t)ram_segment_count << PPC64_SEGMENT_SHIFT;
	ram_limit = memory_size;

	/* A clean buffer, then the RAM segments. */
	__asm__ volatile("slbia" : : : "memory");
	for (esid = 0; esid < ram_segment_count; esid++) {
		__asm__ volatile("slbmte %0,%1" : : "r"(((VSID_RAM + esid) << 12) | SLB_KEY_USER | slb_large),
				 "r"((esid << PPC64_SEGMENT_SHIFT) | SLB_VALID | (SLB_RAM_FIRST + esid)) : "memory");
	}

	/* The device windows' segments, then the framebuffer's when it is in none of them. */
	kernel_window_count = 0;
	for (index = 0; index < board->window_count && index < PPC64_BOARD_WINDOWS_MAX; index++) {
		esid = board->windows[index] >> PPC64_SEGMENT_SHIFT;

		/* A window over RAM's segments or the user range would hide them. */
		if (esid < ram_segment_count || esid >= PPC64_USER_SEGMENT_FIRST)
			continue;
		(void)add_kernel_window(esid);
	}
	device_window_count = kernel_window_count;
	framebuffer_base = board->framebuffer_address;
	framebuffer_size = (uint64_t)board->framebuffer_linebytes * board->framebuffer_height;
	framebuffer_in_window = 0;
	if (framebuffer_base != 0) {
		framebuffer_in_window = in_device_window(framebuffer_base, (size_t)framebuffer_size);
		if (!framebuffer_in_window)
			(void)add_kernel_window(framebuffer_base >> PPC64_SEGMENT_SHIFT);
	}

	/* The user segments with ids that map nothing, after the kernel's. */
	slb_user_first = ram_segment_count + kernel_window_count;
	load_user_segments(VSID_SYSTEM_USER);

	/* RAM, supervisor only. */
	for (address = 0; address < memory_size; address += large_page_size) {
		error = enter(VSID_RAM + (address >> PPC64_SEGMENT_SHIFT), address, large_page_shift,
			      address | HPTE_REFERENCED | HPTE_CHANGED | HPTE_COHERENT | HPTE_SUPERVISOR, 1, &first);
		if (error != HAL_OK)
			HAL_FATAL("ppc64 RAM window does not fit the hashed table");
	}

	/* Each device window whole, uncached and guarded, supervisor only. */
	for (index = 0; index < device_window_count; index++) {
		map_kernel_window(kernel_window_segments[index] << PPC64_SEGMENT_SHIFT, 1UL << PPC64_SEGMENT_SHIFT,
				  HPTE_UNCACHED | HPTE_GUARDED);
	}

	/* Then the framebuffer by itself, uncached, when no window holds it. */
	if (framebuffer_base != 0 && !framebuffer_in_window)
		map_kernel_window(framebuffer_base, framebuffer_size, HPTE_UNCACHED);

	/* Translation on, in the kernel's state. */
	space_registry = NULL;
	current_space = HAL_SPACE_SYS;
	ppc64_write_msr(ppc64_read_msr() | PPC64_MSR_IR | PPC64_MSR_DR);
	hal_puts("PPC64 PAGING PASS\n");
}

/*
 * Reports the end of the RAM the kernel maps, which the page allocator
 * manages up to.
 */
uint64_t
ppc64_space_ram_limit(
	void)
{
	/* Succeeded: the limit set at boot. */
	return ram_limit;
}

/*
 * Enters a user page into the hashed table after a storage fault, and
 * records its use.  Reports whether the fault was such a page.
 */
int
ppc64_space_reload(
	uintptr_t address,
	int store,
	int execute)
{
	struct ppc64_space *space;
	uint64_t *leaf;
	uint64_t second;
	uint64_t first;
	uint64_t vsid;
	int error;

	/* Only the user range of a user space has pages. */
	if (address < PPC64_USER_MINIMUM ||
	    address >= PPC64_USER_LIMIT ||
	    current_space == HAL_SPACE_SYS)
		return 0;
	space = current_space;

	/* The page must be mapped and allow the access. */
	leaf = walk_leaf(space, address, 0);
	if (leaf == NULL || (*leaf & LEAF_VALID) == 0)
		return 0;
	if (store && (*leaf & LEAF_WRITE) == 0)
		return 0;
	if (execute && (*leaf & LEAF_EXEC) == 0)
		return 0;

	/* Records the use; a page enters writable only once it was written. */
	*leaf |= LEAF_ACCESSED;
	if (store)
		*leaf |= LEAF_MODIFIED;
	second = (*leaf & LEAF_PAGE_MASK) | HPTE_REFERENCED | HPTE_CHANGED | HPTE_COHERENT | HPTE_READ_ONLY;
	if ((*leaf & LEAF_WRITE) != 0 && (*leaf & LEAF_MODIFIED) != 0)
		second = (*leaf & LEAF_PAGE_MASK) | HPTE_REFERENCED | HPTE_CHANGED | HPTE_COHERENT | HPTE_READ_WRITE;

	/* Replaces whatever entry the page had with the new one. */
	vsid = user_vsid(space, address);
	drop_entry(vsid, address);
	error = enter(vsid, address & ~(uintptr_t)(PPC64_PAGE_SIZE - 1U), PPC64_PAGE_SHIFT, second, 0, &first);
	if (error != HAL_OK)
		return 0;

	/* Succeeded: the access is retried. */
	return 1;
}

/*
 * Creates an empty user space.
 */
hal_space_t
hal_space_create(
	void)
{
	struct ppc64_space *space;
	bool enabled;
	int error;

	/* The record and its root. */
	space = kernel_alloc(sizeof(*space));
	if (space == NULL)
		return NULL;
	hal_memset(space, 0, sizeof(*space));
	error = hal_pmem_alloc(ROOT_BYTES, PPC64_PAGE_SIZE, &space->root_memory);
	if (error != HAL_OK) {
		kernel_free(space);
		return NULL;
	}

	/* Clears the root through the kernel's view of it. */
	space->root = hal_pmem_to_kernel(space->root_memory);
	hal_memset(space->root, 0, ROOT_BYTES);
	space->magic = PPC64_SPACE_MAGIC;

	/* Numbers the space and makes it live. */
	enabled = ppc64_irq_save();
	space->space_id = next_space_id++;
	space->registry_next = space_registry;
	space_registry = space;
	space_count++;
	ppc64_irq_restore(enabled);

	/* Succeeded: the space, with nothing mapped. */
	return space;
}

/*
 * Destroys a user space that is not selected, removing its entries from
 * the hashed table.
 */
void
hal_space_destroy(
	hal_space_t handle)
{
	struct ppc64_space *space;
	struct ppc64_space **link;
	uint64_t *leaves;
	uintptr_t address;
	unsigned root_index;
	unsigned index;
	bool enabled;

	/* A missing space is nothing to destroy. */
	space = handle;
	if (space == NULL)
		return;

	/* Takes the space out of the registry and its entries out of the table. */
	enabled = ppc64_irq_save();
	link = &space_registry;
	while (*link != NULL && *link != space)
		link = &(*link)->registry_next;
	if (*link == NULL || space->destroying)
		HAL_FATAL("invalid ppc64 space destroy");
	if (current_space == handle)
		HAL_FATAL("destroying an active ppc64 space");
	space->destroying = 1U;
	*link = space->registry_next;
	if (space_count != 0)
		space_count--;
	for (root_index = 0; root_index < ROOT_ENTRIES; root_index++) {
		if (space->root[root_index] == 0)
			continue;
		leaves = hal_pmem_to_kernel(space->root[root_index]);
		for (index = 0; index < LEAF_ENTRIES; index++) {
			if ((leaves[index] & LEAF_VALID) == 0)
				continue;
			address = PPC64_USER_MINIMUM + ((uintptr_t)root_index << LEAF_SHIFT) + ((uintptr_t)index << PPC64_PAGE_SHIFT);
			drop_entry(user_vsid(space, address), address);
		}
	}

	/* Turns interrupts back on as the caller had them. */
	ppc64_irq_restore(enabled);

	/* Frees the tables, then the root and the record. */
	free_tables(space);
	space->magic = 0;
	(void)hal_pmem_free(&space->root_memory, ROOT_BYTES);
	kernel_free(space);
}

/*
 * Selects a user space, or the kernel alone: four segment entries.
 */
void
hal_space_switch(
	hal_space_t handle)
{
	struct ppc64_space *space;
	bool enabled;
	int locked;

	/* The space already selected needs nothing. */
	if (handle == current_space)
		return;

	/* The kernel alone: ids that map nothing. */
	if (handle == HAL_SPACE_SYS) {
		enabled = ppc64_irq_save();
		load_user_segments(VSID_SYSTEM_USER);
		current_space = handle;
		ppc64_irq_restore(enabled);
		return;
	}

	/* A user space: its own ids. */
	locked = space_lock_handle(handle, &space, &enabled);
	if (!locked)
		HAL_FATAL("invalid ppc64 space switch");
	load_user_segments(VSID_USER + ((uint64_t)space->space_id << 8));
	current_space = handle;
	ppc64_irq_restore(enabled);
}

/*
 * Maps managed RAM into a user space; the hashed table is filled on use.
 */
int
hal_space_map(
	hal_space_t handle,
	void *vaddr,
	hal_physaddr_t paddr,
	size_t size,
	uint32_t attr)
{
	struct ppc64_space *space;
	uint64_t *leaf;
	uint64_t flags;
	uintptr_t address;
	uintptr_t offset;
	uintptr_t rollback;
	bool enabled;
	int valid;
	int locked;
	bool ram;

	/* Requires a user range, a page-aligned range of RAM and attributes the tables can hold. */
	address = (uintptr_t)vaddr;
	if (handle == HAL_SPACE_SYS)
		return HAL_ERR_INVALID;
	valid = valid_user(address, size);
	if (!valid)
		return HAL_ERR_INVALID;
	if ((paddr & (PPC64_PAGE_SIZE - 1U)) != 0)
		return HAL_ERR_INVALID;
	ram = ppc64_range_is_ram(paddr, size);
	if (!ram)
		return HAL_ERR_INVALID;
	valid = valid_attributes(attr);
	if (valid != HAL_OK)
		return valid;
	locked = space_lock_handle(handle, &space, &enabled);
	if (!locked)
		return HAL_ERR_STATE;

	/* Refuses a range of which any page is mapped already. */
	for (offset = 0; offset < size; offset += PPC64_PAGE_SIZE) {
		leaf = walk_leaf(space, address + offset, 0);
		if (leaf != NULL && (*leaf & LEAF_VALID) != 0) {
			ppc64_irq_restore(enabled);
			return HAL_ERR_INVALID;
		}
	}

	/* Writes a leaf for each page, unwinding them if a table cannot be made. */
	flags = LEAF_VALID;
	if ((attr & HAL_SPACE_WRITE) != 0)
		flags |= LEAF_WRITE;
	if ((attr & HAL_SPACE_EXEC) != 0)
		flags |= LEAF_EXEC;
	for (offset = 0; offset < size; offset += PPC64_PAGE_SIZE) {
		leaf = walk_leaf(space, address + offset, 1);
		if (leaf == NULL) {
			for (rollback = 0; rollback < offset; rollback += PPC64_PAGE_SIZE) {
				leaf = walk_leaf(space, address + rollback, 0);
				if (leaf != NULL)
					*leaf = 0;
			}

			/* Turns interrupts back on as the caller had them. */
			ppc64_irq_restore(enabled);
			return HAL_ERR_NOMEM;
		}

		/* Writes the page's leaf. */
		*leaf = ((uint64_t)(paddr + offset) & LEAF_PAGE_MASK) | flags;
	}

	/* Code in the pages is fetched from memory, not from stale cache lines. */
	if ((attr & HAL_SPACE_EXEC) != 0)
		hal_sync_instruction_stream(hal_pmem_to_kernel(paddr), size);
	ppc64_irq_restore(enabled);

	/* Succeeded: the range is mapped. */
	return HAL_OK;
}

/*
 * Removes a user range's mappings and their hashed entries.
 */
int
hal_space_unmap(
	hal_space_t handle,
	void *vaddr,
	size_t size)
{
	struct ppc64_space *space;
	uint64_t *leaf;
	uintptr_t address;
	uintptr_t offset;
	bool enabled;
	int locked;
	int valid;

	/* An empty range is nothing to remove. */
	if (size == 0)
		return HAL_OK;
	address = (uintptr_t)vaddr;
	if (handle == HAL_SPACE_SYS)
		return HAL_ERR_INVALID;
	valid = valid_user(address, size);
	if (!valid)
		return HAL_ERR_INVALID;
	locked = space_lock_handle(handle, &space, &enabled);
	if (!locked)
		return HAL_ERR_STATE;

	/* Clears each leaf and removes its entry. */
	for (offset = 0; offset < size; offset += PPC64_PAGE_SIZE) {
		leaf = walk_leaf(space, address + offset, 0);
		if (leaf == NULL || (*leaf & LEAF_VALID) == 0)
			continue;
		*leaf = 0;
		drop_entry(user_vsid(space, address + offset), address + offset);
	}

	/* Turns interrupts back on as the caller had them. */
	ppc64_irq_restore(enabled);

	/* Succeeded: the range is unmapped; empty leaf tables stay until the space goes. */
	return HAL_OK;
}

/*
 * Changes a mapped user range's protection and reports what the space's
 * table recorded.
 */
int
hal_space_prot_query(
	hal_space_t handle,
	void *vaddr,
	size_t size,
	uint32_t attr,
	uint32_t *flags)
{
	struct ppc64_space *space;
	uint64_t *leaf;
	uint64_t kept;
	uint32_t observed;
	uintptr_t address;
	uintptr_t offset;
	bool enabled;
	int valid;
	int locked;

	/* Requires a user range and attributes the tables can hold. */
	address = (uintptr_t)vaddr;
	if (handle == HAL_SPACE_SYS)
		return HAL_ERR_INVALID;
	valid = valid_user(address, size);
	if (!valid)
		return HAL_ERR_INVALID;
	valid = valid_attributes(attr);
	if (valid != HAL_OK)
		return valid;
	locked = space_lock_handle(handle, &space, &enabled);
	if (!locked)
		return HAL_ERR_STATE;

	/* Refuses a range with any page unmapped. */
	for (offset = 0; offset < size; offset += PPC64_PAGE_SIZE) {
		leaf = walk_leaf(space, address + offset, 0);
		if (leaf == NULL || (*leaf & LEAF_VALID) == 0) {
			ppc64_irq_restore(enabled);
			return HAL_ERR_INVALID;
		}
	}

	/* Rewrites each leaf, keeping its page and its records, and removes its entry. */
	observed = 0;
	for (offset = 0; offset < size; offset += PPC64_PAGE_SIZE) {
		leaf = walk_leaf(space, address + offset, 0);
		observed |= HAL_SPACE_PAGE_PRESENT;
		if ((*leaf & LEAF_ACCESSED) != 0)
			observed |= HAL_SPACE_PAGE_ACCESSED;
		if ((*leaf & LEAF_MODIFIED) != 0)
			observed |= HAL_SPACE_PAGE_DIRTY;
		kept = (*leaf & (LEAF_PAGE_MASK | LEAF_ACCESSED | LEAF_MODIFIED)) | LEAF_VALID;
		if ((attr & HAL_SPACE_WRITE) != 0)
			kept |= LEAF_WRITE;
		if ((attr & HAL_SPACE_EXEC) != 0)
			kept |= LEAF_EXEC;
		*leaf = kept;
		drop_entry(user_vsid(space, address + offset), address + offset);
	}

	/* Turns interrupts back on as the caller had them. */
	ppc64_irq_restore(enabled);

	/* Succeeded: reports what was recorded, when asked. */
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
	struct ppc64_space *space;
	uint64_t *leaf;
	uint32_t observed;
	bool enabled;
	int locked;
	int valid;

	/* Requires a user page and somewhere to report. */
	if (handle == HAL_SPACE_SYS || flags == NULL)
		return HAL_ERR_INVALID;
	valid = valid_user((uintptr_t)vaddr, PPC64_PAGE_SIZE);
	if (!valid)
		return HAL_ERR_INVALID;
	locked = space_lock_handle(handle, &space, &enabled);
	if (!locked)
		return HAL_ERR_STATE;

	/* Reads the page's leaf. */
	observed = 0;
	leaf = walk_leaf(space, (uintptr_t)vaddr, 0);
	if (leaf != NULL && (*leaf & LEAF_VALID) != 0) {
		observed = HAL_SPACE_PAGE_PRESENT;
		if ((*leaf & LEAF_ACCESSED) != 0)
			observed |= HAL_SPACE_PAGE_ACCESSED;
		if ((*leaf & LEAF_MODIFIED) != 0)
			observed |= HAL_SPACE_PAGE_DIRTY;
	}

	/* Turns interrupts back on as the caller had them. */
	ppc64_irq_restore(enabled);

	/* Succeeded: the page's state. */
	*flags = observed;
	return HAL_OK;
}

/*
 * Clears the recorded state of one user page; its entry is removed, so
 * the next use records it again.
 */
int
hal_space_clear_flags(
	hal_space_t handle,
	void *vaddr,
	uint32_t flags)
{
	struct ppc64_space *space;
	uint64_t *leaf;
	bool enabled;
	int locked;
	int valid;

	/* Requires a user page and flags the table records. */
	if (handle == HAL_SPACE_SYS)
		return HAL_ERR_INVALID;
	valid = valid_user((uintptr_t)vaddr, PPC64_PAGE_SIZE);
	if (!valid)
		return HAL_ERR_INVALID;
	if ((flags & ~(uint32_t)(HAL_SPACE_PAGE_ACCESSED | HAL_SPACE_PAGE_DIRTY)) != 0)
		return HAL_ERR_INVALID;
	locked = space_lock_handle(handle, &space, &enabled);
	if (!locked)
		return HAL_ERR_STATE;

	/* Refuses a page that is not mapped. */
	leaf = walk_leaf(space, (uintptr_t)vaddr, 0);
	if (leaf == NULL || (*leaf & LEAF_VALID) == 0) {
		ppc64_irq_restore(enabled);
		return HAL_ERR_INVALID;
	}

	/* Clears the records and the entry that would not set them again. */
	if ((flags & HAL_SPACE_PAGE_ACCESSED) != 0)
		*leaf &= ~(uint64_t)LEAF_ACCESSED;
	if ((flags & HAL_SPACE_PAGE_DIRTY) != 0)
		*leaf &= ~(uint64_t)LEAF_MODIFIED;
	drop_entry(user_vsid(space, (uintptr_t)vaddr), (uintptr_t)vaddr);
	ppc64_irq_restore(enabled);

	/* Succeeded: the page's state is cleared. */
	return HAL_OK;
}

/*
 * Drops the stale translations of a space: its entries are always
 * removed when its table changes, so there is nothing left to do.
 */
void
hal_space_flush_tlb(
	hal_space_t handle)
{
	UNUSED_PARAMETER(handle);
}

/*
 * Drops the stale translations of a range; nothing is left to do.
 */
void
hal_space_flush_tlb_range(
	hal_space_t handle,
	void *vaddr,
	size_t size)
{
	UNUSED_PARAMETER(handle);
	UNUSED_PARAMETER(vaddr);
	UNUSED_PARAMETER(size);
}

/*
 * Reports the size a table level maps: only pages.
 */
size_t
hal_space_get_page_size(
	int level)
{
	/* User pages are small pages only. */
	if (level == 1)
		return PPC64_PAGE_SIZE;
	return 0;
}

/*
 * Reports the user range's bounds.
 */
void
hal_space_get_user_range(
	uintptr_t *minimum,
	uintptr_t *limit)
{
	/* Segments 0x10 to 0x1f. */
	if (minimum != NULL)
		*minimum = PPC64_USER_MINIMUM;
	if (limit != NULL)
		*limit = PPC64_USER_LIMIT;
}

/*
 * Reports where the kernel reaches a device range: RAM at its own
 * addresses; no device window is offered yet.
 */
int
hal_space_map_device(
	hal_physaddr_t paddr,
	size_t size,
	uint32_t attr,
	void **vaddr)
{
	bool ram;
	int window;

	/* Requires a destination, a non-empty range and no execution. */
	if (vaddr == NULL || size == 0)
		return HAL_ERR_INVALID;
	if ((attr & HAL_SPACE_EXEC) != 0)
		return HAL_ERR_INVALID;

	/* RAM, as it is. */
	ram = ppc64_range_is_ram(paddr, size);
	if (ram) {
		*vaddr = (void *)(uintptr_t)paddr;
		return HAL_OK;
	}

	/* A range inside a device window, as it is. */
	window = in_device_window(paddr, size);
	if (window) {
		*vaddr = (void *)(uintptr_t)paddr;
		return HAL_OK;
	}

	/* Anything else has no translation. */
	return HAL_ERR_UNSUPPORTED;
}

/*
 * Releases a device range; nothing was mapped for it.
 */
int
hal_space_unmap_device(
	void *vaddr,
	size_t size)
{
	UNUSED_PARAMETER(vaddr);

	/* An empty range was never handed out. */
	if (size == 0)
		return HAL_ERR_INVALID;
	return HAL_OK;
}

/*
 * Makes code written through the data side visible to instruction fetch.
 */
void
hal_sync_instruction_stream(
	void *addr,
	size_t size)
{
	uintptr_t line;
	uintptr_t end;

	/* Each cache line of the range: pushed, then dropped from the instruction side. */
	end = (uintptr_t)addr + size;
	for (line = (uintptr_t)addr & ~(uintptr_t)127U; line < end; line += 128U)
		__asm__ volatile("dcbst 0,%0" : : "r"(line) : "memory");
	__asm__ volatile("sync" : : : "memory");
	for (line = (uintptr_t)addr & ~(uintptr_t)127U; line < end; line += 128U)
		__asm__ volatile("icbi 0,%0" : : "r"(line) : "memory");
	__asm__ volatile("sync\n\tisync" : : : "memory");
}

/*
 * Makes a range of instructions current.
 */
void
hal_icache_invalidate_range(
	uintptr_t addr,
	size_t size)
{
	/* The same synchronization. */
	hal_sync_instruction_stream((void *)addr, size);
}

/*
 * Reports how many user spaces and table pages exist.
 */
void
ppc64_space_memory_stats(
	uint32_t *spaces,
	uint32_t *tables)
{
	bool enabled;

	/* Samples both counts together. */
	enabled = ppc64_irq_save();
	if (spaces != NULL)
		*spaces = space_count;
	if (tables != NULL)
		*tables = page_table_count;
	ppc64_irq_restore(enabled);
}

/*
 * Finds a live user space by its handle with interrupts off for its
 * operation.  Reports 0 for a handle that names no live space.
 */
static int
space_lock_handle(
	hal_space_t handle,
	struct ppc64_space **result,
	bool *irq_enabled)
{
	struct ppc64_space *space;
	bool enabled;

	/* Looks the handle up among the live spaces. */
	enabled = ppc64_irq_save();
	for (space = space_registry; space != NULL; space = space->registry_next) {
		if ((hal_space_t)space == handle)
			break;
	}

	/* A handle that names no live space is refused. */
	if (space == NULL || space->destroying) {
		ppc64_irq_restore(enabled);
		return 0;
	}

	/* Succeeded: the caller holds the space until it turns interrupts back on. */
	*result = space;
	*irq_enabled = enabled;
	return 1;
}

/* Reports whether a range is page-aligned, non-empty and inside the user range. */
static int
valid_user(
	uintptr_t address,
	size_t size)
{
	/* An empty or unaligned range is refused. */
	if (size == 0)
		return 0;
	if ((address & (PPC64_PAGE_SIZE - 1U)) != 0 || (size & (PPC64_PAGE_SIZE - 1U)) != 0)
		return 0;

	/* The range must lie inside the user range. */
	if (address < PPC64_USER_MINIMUM || address >= PPC64_USER_LIMIT)
		return 0;
	if (size > PPC64_USER_LIMIT - address)
		return 0;
	return 1;
}

/*
 * Checks a user mapping's attributes: some access, never written and
 * executed at once, and no cache behaviour or device class.
 */
static int
valid_attributes(
	uint32_t attr)
{
	/* A mapping must allow some access, and not both writing and executing. */
	if ((attr & (HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC)) == 0)
		return HAL_ERR_INVALID;
	if ((attr & HAL_SPACE_WRITE) != 0 && (attr & HAL_SPACE_EXEC) != 0)
		return HAL_ERR_INVALID;

	/* Device pages and other cache behaviours are not offered to users. */
	if ((attr & (HAL_SPACE_NOCACHE | HAL_SPACE_WRITETHRU | HAL_SPACE_WC | HAL_SPACE_DEVICE)) != 0)
		return HAL_ERR_UNSUPPORTED;
	return HAL_OK;
}

/*
 * Finds the space's own leaf of a user address, making its leaf table
 * when asked to.  Reports NULL when the table is missing and not to be
 * made, or cannot be made.
 */
static uint64_t *
walk_leaf(
	struct ppc64_space *space,
	uintptr_t address,
	int create)
{
	struct ppc64_table_page *page;
	uint64_t *table;
	unsigned index;
	int error;

	/* The root entry: the leaf table's physical address, or zero. */
	index = (unsigned)((address - PPC64_USER_MINIMUM) >> LEAF_SHIFT);
	if (space->root[index] == 0) {
		if (!create)
			return NULL;
		page = kernel_alloc(sizeof(*page));
		if (page == NULL)
			return NULL;
		error = hal_pmem_alloc(PPC64_PAGE_SIZE, PPC64_PAGE_SIZE, &page->memory);
		if (error != HAL_OK) {
			kernel_free(page);
			return NULL;
		}

		/* Clears the table and links it into the space. */
		hal_memset(hal_pmem_to_kernel(page->memory), 0, PPC64_PAGE_SIZE);
		page->next = space->tables;
		space->tables = page;
		space->root[index] = page->memory;
		page_table_count++;
	}

	/* Succeeded: the page's leaf. */
	table = hal_pmem_to_kernel(space->root[index]);
	return &table[(address >> PPC64_PAGE_SHIFT) & (LEAF_ENTRIES - 1U)];
}

/*
 * Loads the user segment entries with ids from a base plus the segment,
 * dropping what the old ones translated.
 */
static void
load_user_segments(
	uint64_t first_vsid)
{
	uint64_t esid;
	uint64_t index;

	/* Each user segment: the old entry invalidated, the new one written. */
	for (index = 0; index < PPC64_USER_SEGMENT_COUNT; index++) {
		esid = PPC64_USER_SEGMENT_FIRST + index;
		__asm__ volatile("slbie %0" : : "r"(esid << PPC64_SEGMENT_SHIFT) : "memory");
		__asm__ volatile("slbmte %0,%1" : : "r"(((first_vsid + esid) << 12) | SLB_KEY_USER),
				 "r"((esid << PPC64_SEGMENT_SHIFT) | SLB_VALID | (slb_user_first + index)) : "memory");
	}

	/* Makes the new segments take effect. */
	__asm__ volatile("isync" : : : "memory");
}

/*
 * Gives a kernel window segment its segment lookaside buffer entry, once;
 * reports its index among the kernel's windows.
 */
static unsigned
add_kernel_window(
	uint64_t esid)
{
	unsigned index;

	/* A segment already given keeps its entry. */
	for (index = 0; index < kernel_window_count; index++) {
		if (kernel_window_segments[index] == esid)
			return index;
	}

	/* The segment's entry, its id from the segment number, supervisor only. */
	if (kernel_window_count > PPC64_BOARD_WINDOWS_MAX)
		HAL_FATAL("ppc64 board names too many windows");
	index = kernel_window_count;
	kernel_window_segments[index] = esid;
	__asm__ volatile("slbmte %0,%1" : : "r"(((VSID_KERNEL_WINDOW + esid) << 12) | SLB_KEY_USER | slb_large),
			 "r"((esid << PPC64_SEGMENT_SHIFT) | SLB_VALID | (ram_segment_count + index)) : "memory");
	kernel_window_count++;

	/* Succeeded: the window's index. */
	return index;
}

/* Maps a range of a kernel window segment at its own address in large pages. */
static void
map_kernel_window(
	uint64_t address,
	uint64_t size,
	uint64_t storage)
{
	uint64_t large_page_size;
	uint64_t page;
	uint64_t first;
	int error;

	/* Each large page the range touches, supervisor only. */
	large_page_size = 1UL << large_page_shift;
	for (page = address & ~(large_page_size - 1U); page < address + size; page += large_page_size) {
		error = enter(VSID_KERNEL_WINDOW + (page >> PPC64_SEGMENT_SHIFT), page, large_page_shift,
			      page | HPTE_REFERENCED | HPTE_CHANGED | storage | HPTE_SUPERVISOR, 1, &first);
		if (error != HAL_OK)
			HAL_FATAL("ppc64 device window does not fit the hashed table");
	}
}

/* Tells whether a range lies in one device window segment. */
static int
in_device_window(
	hal_physaddr_t paddr,
	size_t size)
{
	uint64_t esid;
	unsigned index;

	/* An empty range, or one across segments, is in no window. */
	if (size == 0)
		return 0;
	esid = paddr >> PPC64_SEGMENT_SHIFT;
	if (((paddr + size - 1U) >> PPC64_SEGMENT_SHIFT) != esid)
		return 0;

	/* One of the device windows' segments. */
	for (index = 0; index < device_window_count; index++) {
		if (kernel_window_segments[index] == esid)
			return 1;
	}

	/* None holds it. */
	return 0;
}

/* Sets the kernel's large page and its encodings from the board's choice. */
static void
choose_large_page(
	unsigned shift)
{
	/* A partition's 64 KiB page names its size in both entries. */
	if (shift == 16U) {
		large_page_shift = shift;
		slb_large = SLB_LARGE_64K;
		hpte_large_size = HPTE_LARGE_64K;
		return;
	}

	/* The 970's one large page, 16 MiB, needs only the large bit. */
	if (shift == 24U) {
		large_page_shift = shift;
		slb_large = SLB_LARGE_16M;
		hpte_large_size = 0;
		return;
	}

	/* No other large page is known. */
	HAL_FATAL("ppc64 board names an unknown large page");
}

/*
 * Empties the table the HAL keeps itself, names it to the processor and
 * drops every translation the firmware left in the buffer.
 */
static void
table_init(
	uint64_t base,
	uint32_t shift)
{
	uint64_t table_register;
	unsigned congruence_class;

	/* The table must be in RAM, aligned to its size. */
	if (base == 0 || (base & ((1UL << shift) - 1U)) != 0)
		HAL_FATAL("ppc64 board gives no aligned hashed table");
	table_entries = (volatile uint64_t *)(uintptr_t)base;
	hal_memset((void *)(uintptr_t)base, 0, 1UL << shift);
	__asm__ volatile("sync" : : : "memory");

	/* The table register holds the origin and the size above the smallest. */
	table_register = base | (shift - TABLE_SHIFT_MINIMUM);
	__asm__ volatile("mtspr 25,%0\n\tisync" : : "r"(table_register) : "memory");

	/* Every congruence class of the translation buffer, on this processor only. */
	for (congruence_class = 0; congruence_class < TLB_CONGRUENCE_CLASSES; congruence_class++)
		__asm__ volatile("tlbiel %0" : : "r"((uint64_t)congruence_class << PPC64_PAGE_SHIFT) : "memory");
	__asm__ volatile("ptesync" : : : "memory");
}

/*
 * Enters one page: its first word from the segment id and the address,
 * into the primary group, else the secondary, else over a victim of the
 * primary group.  A bolted page is one of the kernel's windows, which no
 * victim choice removes from a table the HAL writes itself.
 */
static int
enter(
	uint64_t vsid,
	uintptr_t address,
	unsigned page_shift,
	uint64_t second,
	int bolted,
	uint64_t *first_out)
{
	uint64_t first;
	uint64_t epn;
	uint64_t hash;
	int error;

	/* The hash and the compare value of a 256 MiB segment's page. */
	epn = (address & ((1UL << PPC64_SEGMENT_SHIFT) - 1U)) & ~((1UL << page_shift) - 1U);
	hash = vsid ^ (epn >> page_shift);
	first = (vsid << 12) | ((epn >> 16) & HPTE_AVPN_MASK) | HPTE_VALID;
	if (page_shift != PPC64_PAGE_SHIFT) {
		first |= HPTE_LARGE;
		second |= hpte_large_size;
	}

	/* The hypervisor places the entry in its own table. */
	if (table_by_hypervisor) {
		error = enter_by_hypervisor(hash, first, second, first_out);
		if (error != HAL_OK)
			return error;

		/* Succeeded: the hypervisor holds the entry. */
		return HAL_OK;
	}

	/* The HAL places it in its own, marking the kernel's windows. */
	if (bolted)
		first |= HPTE_BOLTED;
	error = enter_direct(hash, first, second, first_out);
	if (error != HAL_OK)
		return error;

	/* Succeeded: the table holds the entry. */
	return HAL_OK;
}

/* Enters a page through the hypervisor's calls. */
static int
enter_by_hypervisor(
	uint64_t hash,
	uint64_t first,
	uint64_t second,
	uint64_t *first_out)
{
	uint64_t arguments[8];
	unsigned function;
	long status;

	/* A free slot under either hash function. */
	for (function = 0; function < 2U; function++) {
		hal_memset(arguments, 0, sizeof(arguments));
		arguments[1] = ((function == 0 ? hash : ~hash) & pteg_mask) * 8U;
		arguments[2] = first | (function == 0 ? 0 : HPTE_SECONDARY);
		arguments[3] = second;
		status = ppc64_hcall(PPC64_H_ENTER, arguments);
		if (status == H_SUCCESS) {
			*first_out = first | (function == 0 ? 0 : HPTE_SECONDARY);
			return HAL_OK;
		}
	}

	/* Otherwise a victim of the primary group goes. */
	hal_memset(arguments, 0, sizeof(arguments));
	arguments[1] = (hash & pteg_mask) * 8U + (evict_next++ % 8U);
	(void)ppc64_hcall(PPC64_H_REMOVE, arguments);
	hal_memset(arguments, 0, sizeof(arguments));
	arguments[1] = (hash & pteg_mask) * 8U;
	arguments[2] = first;
	arguments[3] = second;
	status = ppc64_hcall(PPC64_H_ENTER, arguments);
	if (status != H_SUCCESS)
		return HAL_ERR_NOMEM;

	/* Succeeded: the entry took the victim's group. */
	*first_out = first;
	return HAL_OK;
}

/* Enters a page into the table the HAL writes itself. */
static int
enter_direct(
	uint64_t hash,
	uint64_t first,
	uint64_t second,
	uint64_t *first_out)
{
	volatile uint64_t *slot;
	uint64_t group;
	unsigned function;
	unsigned index;
	unsigned tries;

	/* A free slot under either hash function: the primary group, then the secondary. */
	for (function = 0; function < 2U; function++) {
		group = hash & pteg_mask;
		if (function != 0)
			group = ~hash & pteg_mask;

		/* Each slot of the group; an occupied one is passed over. */
		for (index = 0; index < TABLE_GROUP_SLOTS; index++) {
			slot = &table_entries[(group * TABLE_GROUP_SLOTS + index) * 2U];
			if ((slot[0] & HPTE_VALID) != 0)
				continue;

			/* The slot is free: the entry goes there, its hash function named. */
			if (function != 0)
				first |= HPTE_SECONDARY;
			write_slot(slot, first, second);
			*first_out = first;
			return HAL_OK;
		}
	}

	/* Otherwise a victim of the primary group goes, taken in turn. */
	group = hash & pteg_mask;
	for (tries = 0; tries < TABLE_GROUP_SLOTS; tries++) {
		index = evict_next++ % TABLE_GROUP_SLOTS;
		slot = &table_entries[(group * TABLE_GROUP_SLOTS + index) * 2U];

		/* A kernel window is never the victim. */
		if ((slot[0] & HPTE_BOLTED) != 0)
			continue;

		/* The victim leaves the table and the buffer, and the entry takes its slot. */
		evict_slot(slot, group);
		write_slot(slot, first, second);
		*first_out = first;
		return HAL_OK;
	}

	/* Every slot of the group holds a kernel window. */
	return HAL_ERR_NOMEM;
}

/*
 * Writes an entry into a free slot: the second word first, and the first
 * word, which makes it valid, only once the second is in place.
 */
static void
write_slot(
	volatile uint64_t *slot,
	uint64_t first,
	uint64_t second)
{
	/* The translation the entry gives, then the entry made valid. */
	slot[1] = second;
	__asm__ volatile("eieio" : : : "memory");
	slot[0] = first;
	__asm__ volatile("ptesync" : : : "memory");
}

/*
 * Removes a user page's entry from its slot and its translation from the
 * buffer.  The page's address comes from the entry: the compare value
 * holds its segment and its high bits, and the group, under the hash
 * function the entry names, its low bits.
 */
static void
evict_slot(
	volatile uint64_t *slot,
	uint64_t group)
{
	uint64_t first;
	uint64_t vsid;
	uint64_t hash;
	uint64_t epn;

	/* The entry leaves the table before its translation leaves the buffer. */
	first = slot[0];
	slot[0] = 0;
	__asm__ volatile("ptesync" : : : "memory");

	/* An empty slot has no translation to drop. */
	if ((first & HPTE_VALID) == 0)
		return;

	/* The page's segment id, and the hash the group was chosen by. */
	vsid = first >> 12;
	hash = group;
	if ((first & HPTE_SECONDARY) != 0)
		hash = ~group;

	/* The page's number in its segment: the high bits from the compare value, the low ones from the hash. */
	epn = ((first & HPTE_AVPN_MASK) << 16) & ((1UL << PPC64_SEGMENT_SHIFT) - 1U);
	epn |= ((hash ^ vsid) & TABLE_HASH_PAGE_MASK) << PPC64_PAGE_SHIFT;
	flush_translation(vsid, epn);
}

/* Drops a 4 KiB page's translation from every processor's buffer. */
static void
flush_translation(
	uint64_t vsid,
	uint64_t epn)
{
	uint64_t virtual_address;

	/* The page's virtual address, in the bits the invalidation compares. */
	virtual_address = ((vsid << PPC64_SEGMENT_SHIFT) | epn) & TLBIE_ADDRESS_MASK;
	__asm__ volatile("ptesync\n\ttlbie %0,0\n\teieio\n\ttlbsync\n\tptesync"
			 : : "r"(virtual_address) : "memory");
}

/* Removes a user page's entry, under whichever hash function it went in. */
static void
drop_entry(
	uint64_t vsid,
	uintptr_t address)
{
	uint64_t epn;
	uint64_t hash;
	uint64_t avpn;

	/* The compare value, the same under both functions. */
	epn = (address & ((1UL << PPC64_SEGMENT_SHIFT) - 1U)) & ~(uint64_t)(PPC64_PAGE_SIZE - 1U);
	hash = vsid ^ (epn >> PPC64_PAGE_SHIFT);
	avpn = ((vsid << 12) | ((epn >> 16) & HPTE_AVPN_MASK)) & ~(uint64_t)HPTE_FLAGS_MASK;

	/* The hypervisor removes it from its own table. */
	if (table_by_hypervisor) {
		drop_by_hypervisor(hash, avpn);
		return;
	}

	/* The HAL removes it from its own. */
	drop_direct(vsid, epn, hash, avpn);
}

/* Removes a page's entry through the hypervisor's calls. */
static void
drop_by_hypervisor(
	uint64_t hash,
	uint64_t avpn)
{
	uint64_t arguments[8];
	unsigned function;
	unsigned slot;

	/* Every slot of both groups; the hypervisor removes only a matching entry. */
	for (function = 0; function < 2U; function++) {
		for (slot = 0; slot < 8U; slot++) {
			hal_memset(arguments, 0, sizeof(arguments));
			arguments[0] = H_AVPN;
			arguments[1] = ((function == 0 ? hash : ~hash) & pteg_mask) * 8U + slot;
			arguments[2] = avpn;
			(void)ppc64_hcall(PPC64_H_REMOVE, arguments);
		}
	}
}

/* Removes a 4 KiB page's entry from the table the HAL writes itself. */
static void
drop_direct(
	uint64_t vsid,
	uint64_t epn,
	uint64_t hash,
	uint64_t avpn)
{
	volatile uint64_t *slot;
	uint64_t secondary;
	uint64_t group;
	uint64_t first;
	unsigned function;
	unsigned index;

	/* Every slot of both groups: the primary, then the secondary. */
	for (function = 0; function < 2U; function++) {
		group = hash & pteg_mask;
		secondary = 0;
		if (function != 0) {
			group = ~hash & pteg_mask;
			secondary = HPTE_SECONDARY;
		}

		/* Each slot of the group. */
		for (index = 0; index < TABLE_GROUP_SLOTS; index++) {
			slot = &table_entries[(group * TABLE_GROUP_SLOTS + index) * 2U];
			first = slot[0];

			/* Only a valid small page of this address under this function matches. */
			if ((first & HPTE_VALID) == 0)
				continue;
			if ((first & (HPTE_SECONDARY | HPTE_LARGE)) != secondary)
				continue;
			if ((first & ~(uint64_t)HPTE_FLAGS_MASK) != avpn)
				continue;

			/* The entry leaves the table, then its translation the buffer. */
			slot[0] = 0;
			flush_translation(vsid, epn);
		}
	}
}

/* Reports the segment id of a user address in a space. */
static uint64_t
user_vsid(
	const struct ppc64_space *space,
	uintptr_t address)
{
	/* The space's base and the segment. */
	return VSID_USER + ((uint64_t)space->space_id << 8) + (address >> PPC64_SEGMENT_SHIFT);
}

/* Frees a space's leaf tables. */
static void
free_tables(
	struct ppc64_space *space)
{
	struct ppc64_table_page *page;

	/* Each page and its record. */
	while (space->tables != NULL) {
		page = space->tables;
		space->tables = page->next;
		(void)hal_pmem_free(&page->memory, PPC64_PAGE_SIZE);
		kernel_free(page);
		page_table_count--;
	}
}
