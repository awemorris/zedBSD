/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MIPS address spaces.
 *
 * The kernel runs in KSEG0 and KSEG1, which the TLB does not translate,
 * so it has no pages of its own (on the 64-bit processor CKSEG0, CKSEG1
 * and xkphys).  The user segment, kuseg (xuseg on the 64-bit processor),
 * goes through the TLB, which software fills: each space keeps a table
 * of pages (two levels of 1024 words on the 32-bit processor, three of
 * 512 doublewords on the 64-bit one), and the refill handler of
 * vectors.S writes the pair of pages a missed address is in from the
 * selected space's table.
 *
 * A pair entry of the TLB holds an even and an odd page, so an entry may
 * hold one page of a pair while the other is still invalid.  Every change
 * of a table entry therefore brings the TLB along at once: the pair is
 * probed under the space's ASID and, when found, written again from the
 * table or replaced by an entry that matches nothing.
 *
 * Accessed and dirty are kept in the table's software bits.  A page is
 * entered invalid until it is first used; the TLB invalid exception of
 * that use records the access and validates the pair.  A writable page
 * is entered clean until it is first written; the TLB modified exception
 * of that write records it and sets the pair's dirty bit.
 *
 * Every space gets an ASID of the TLB's current generation when it is
 * selected.  When the ASIDs of a generation are used up, the next
 * generation starts with the TLB emptied, and every space takes a new
 * ASID when it is next selected; an ASID is never given to a second
 * space in the generation that gave it out.
 */

#include <hal/hal.h>
#include "asm.h"
#include "cache.h"
#include "cpu.h"
#include "io.h"
#include "page.h"
#include "pte.h"
#include "space.h"
#include "spin.h"

/* The ASIDs a generation gives out: 1 to 255; 0 is the kernel's and the invalid entries'. */
#define SPACE_ASID_LIMIT	255U

/* The bytes of one pair of pages, one TLB entry. */
#define SPACE_PAIR_BYTES	0x2000U

/*
 * The kernel's address of the root the TLB refill walks: the selected
 * space's root, or the kernel's empty one.  Read by the refill handler at
 * every TLB miss; changed with interrupts off when a space is selected.
 */
uintptr_t mips_tlb_root;

/*
 * The refills taken since boot, counted by the refill handler itself for
 * the self-test.  It only grows.
 */
uint32_t mips_tlb_refill_count;

/*
 * The root of the kernel alone: every entry zero, so that a refill while
 * no user space is selected writes an invalid pair and the access faults.
 * Never written.
 */
static uintptr_t system_root[MIPS_TABLE_ENTRIES] __attribute__((aligned(MIPS_PAGE_SIZE)));

/* The space selected now, HAL_SPACE_SYS for the kernel alone.  Changed with interrupts off. */
static hal_space_t current_space;

/*
 * Every user space that exists, how many there are and how many leaf
 * tables they hold.  Kept with interrupts off.
 */
static struct mips_space *space_registry;
static uint32_t space_count;
static uint32_t page_table_count;

/*
 * The next ASID to give out and the last one a generation may give; the
 * self-test lowers the limit for a while to make the generations turn.
 * Kept with interrupts off.
 */
static uint32_t asid_next = 1;
static uint32_t asid_limit = SPACE_ASID_LIMIT;

/*
 * The TLB's current generation.  It starts at one, so that a space whose
 * generation is zero never holds a current ASID, and only grows.
 */
static uint64_t asid_generation = 1;

/* The TLB's entries, from Config1.  Set by mips_space_init(). */
static uint32_t tlb_entries;

/*
 * The TLB invalid and TLB modified exceptions the spaces brought up to
 * date since boot, for the self-test.  They only grow.
 */
static uint32_t tlb_invalid_count;
static uint32_t tlb_modified_count;

static int space_lock_handle(hal_space_t handle, struct mips_space **result, bool *irq_enabled);
static int valid_user(uintptr_t address, size_t size);
static int valid_attributes(uint32_t attr, int none_allowed);
static mips_pte_t *walk_leaf(struct mips_space *space, uintptr_t address, int create);
static uintptr_t *table_below(struct mips_space *space, uintptr_t *table, unsigned index, int create);
static void tlb_sync(struct mips_space *space, uintptr_t address, int rewrite);
static void tlb_update(uintptr_t entry_hi, int rewrite, uintptr_t low0, uintptr_t low1);
static void tlb_write_index(uint32_t index, uintptr_t entry_hi, uintptr_t low0, uintptr_t low1);
static uintptr_t invalid_entry_hi(uint32_t index);
static void tlb_flush_all(void);
static uintptr_t read_entry_hi(void);
static void write_entry_hi(uintptr_t value);
static void asid_assign(struct mips_space *space);
static void flush_pair_range(struct mips_space *space, uintptr_t address, size_t size);
static void free_tables(struct mips_space *space);

/*
 * Empties the TLB and selects the kernel alone.
 */
void
mips_space_init(
	void)
{
	uint32_t config1;
	uint32_t company;

	/* The TLB's size, from Config1. */
	config1 = mips_read_config1();
	tlb_entries = ((config1 >> MIPS_CONFIG1_TLB_SHIFT) & MIPS_CONFIG1_TLB_MASK) + 1U;

	/*
	 * An Ingenic core leaves its own page mode for the architecture's
	 * TLB before the TLB is first written.  Its loader may have done so
	 * already; writing it again does no harm.
	 */
	company = (mips_read_processor_id() >> MIPS_PRID_COMPANY_SHIFT) & MIPS_PRID_COMPANY_MASK;
	if (company == MIPS_PRID_COMPANY_INGENIC_D1)
		__asm__ volatile("mtc0 %0, $5, 4\n\tehb" : : "r"((uint32_t)MIPS_INGENIC_PAGE_CTRL_VTLB) : "memory");

	/* Pages of 4 KiB, no wired entry, and every entry one that matches nothing. */
	__asm__ volatile("mtc0 $0, $5\n\tmtc0 $0, $6\n\tehb" : : : "memory");
	tlb_flush_all();

	/* The kernel alone: the empty root and ASID 0. */
	mips_tlb_root = (uintptr_t)system_root;
	write_entry_hi(0);
	current_space = HAL_SPACE_SYS;
	hal_printf(MIPS_HAL_NAME " HAL TLB entries=%u asid=%u\n", tlb_entries, asid_limit);
}

/*
 * Brings the TLB up to date after a TLB invalid or modified exception on
 * a user address of the selected space.
 *
 * A mapped page that allows the access records it (accessed, and dirty
 * for a store), and its pair is written again; the access is then
 * retried.  Called with interrupts off.
 */
int
mips_space_fault(
	uintptr_t address,
	int access)
{
	struct mips_space *space;
	mips_pte_t *leaf;
	mips_pte_t entry;

	/* Only the user segment of a user space has pages. */
	if (address < MIPS_USER_MINIMUM || address >= MIPS_USER_LIMIT)
		return MIPS_SPACE_FAULT_MISSING;
	if (current_space == HAL_SPACE_SYS)
		return MIPS_SPACE_FAULT_MISSING;

	/* The fault is in the running space. */
	space = current_space;

	/* The page must be mapped. */
	leaf = walk_leaf(space, address, 0);
	if (leaf == NULL || (*leaf & MIPS_PTE_PRESENT) == 0)
		return MIPS_SPACE_FAULT_MISSING;
	entry = *leaf;

	/* A store needs a writable page, and records the write. */
	if (access == MIPS_SPACE_ACCESS_STORE || access == MIPS_SPACE_ACCESS_MODIFY) {
		if ((entry & MIPS_PTE_WRITABLE) == 0)
			return MIPS_SPACE_FAULT_PROTECTION;
		entry |= MIPS_PTE_ACCESSED | MIPS_PTE_DIRTY;
	} else {
		if ((entry & (MIPS_PTE_READABLE | MIPS_PTE_WRITABLE | MIPS_PTE_EXEC)) == 0)
			return MIPS_SPACE_FAULT_PROTECTION;
		entry |= MIPS_PTE_ACCESSED;
	}

	/* Counts the exception for the self-test. */
	if (access == MIPS_SPACE_ACCESS_MODIFY) {
		tlb_modified_count++;
	} else {
		tlb_invalid_count++;
	}

	/* Records the use and writes the pair again from the table. */
	*leaf = mips_pte_compose(entry);
	tlb_sync(space, address, 1);

	/* Succeeded: the access is retried with the pair up to date. */
	return MIPS_SPACE_FAULT_FIXED;
}

/*
 * Creates an empty user space.
 */
hal_space_t
hal_space_create(
	void)
{
	struct mips_space *space;
	bool enabled;
	int error;

	/* The record. */
	space = kernel_alloc(sizeof(*space));
	if (space == NULL)
		return NULL;

	/* Starts the record empty. */
	hal_memset(space, 0, sizeof(*space));

	/* The root, cleared through the kernel's view of it. */
	error = hal_pmem_alloc(MIPS_PAGE_SIZE, MIPS_PAGE_SIZE, &space->root_memory);
	if (error != HAL_OK) {
		kernel_free(space);
		return NULL;
	}

	/* Clears the root and marks the record as a live space. */
	space->root = hal_pmem_to_kernel(space->root_memory);
	hal_memset(space->root, 0, MIPS_PAGE_SIZE);
	space->magic = MIPS_SPACE_MAGIC;

	/*
	 * Makes the space live.  Its generation is zero, so it has no ASID
	 * until it is first selected.
	 */
	enabled = mips_irq_save();

	space->registry_next = space_registry;
	space_registry = space;
	space_count++;

	mips_irq_restore(enabled);

	/* Succeeded: the space, with nothing mapped. */
	return space;
}

/*
 * Destroys a user space that is not selected.
 *
 * Its ASID is not given out again in its generation, and the next
 * generation empties the TLB, so its entries never match another space.
 */
void
hal_space_destroy(
	hal_space_t handle)
{
	struct mips_space *space;
	struct mips_space **link;
	bool enabled;

	/* A missing space is nothing to destroy. */
	space = handle;
	if (space == NULL)
		return;

	/* Takes the space out of the registry and its ASID out of use. */
	enabled = mips_irq_save();

	/* Finds the link that names the space. */
	link = &space_registry;
	while (*link != NULL && *link != space)
		link = &(*link)->registry_next;

	/* A space that is not registered, or is already going, is a kernel bug. */
	if (*link == NULL || space->destroying)
		HAL_FATAL("invalid mips space destroy");

	/* The selected space cannot go. */
	if (current_space == handle)
		HAL_FATAL("destroying an active mips space");

	/*
	 * Marks the space as going and unlinks it.  The count of spaces
	 * drops with it, and a zero generation means it holds no ASID.
	 */
	space->destroying = 1U;
	*link = space->registry_next;
	if (space_count != 0)
		space_count--;
	space->generation = 0;

	mips_irq_restore(enabled);

	/* Frees the tables, then the root and the record. */
	free_tables(space);
	space->magic = 0;
	(void)hal_pmem_free(&space->root_memory, MIPS_PAGE_SIZE);
	kernel_free(space);
}

/*
 * Selects a user space, or the kernel alone: the root the refill walks
 * and the ASID of EntryHi.
 */
void
hal_space_switch(
	hal_space_t handle)
{
	struct mips_space *space;
	bool enabled;
	int locked;

	/* The space already selected needs nothing. */
	if (handle == current_space)
		return;

	/* The kernel alone: the empty root and ASID 0. */
	if (handle == HAL_SPACE_SYS) {
		enabled = mips_irq_save();
		mips_tlb_root = (uintptr_t)system_root;
		write_entry_hi(0);
		current_space = handle;
		mips_irq_restore(enabled);
		return;
	}

	/* A user space: its root and an ASID of the current generation. */
	locked = space_lock_handle(handle, &space, &enabled);
	if (!locked)
		HAL_FATAL("invalid mips space switch");
	asid_assign(space);
	mips_tlb_root = (uintptr_t)space->root;
	write_entry_hi(space->asid);
	current_space = handle;
	mips_irq_restore(enabled);
}

/*
 * Maps managed RAM into a user space.  The pages are entered unused and
 * clean; their first use and first write record themselves.
 */
int
hal_space_map(
	hal_space_t handle,
	void *vaddr,
	hal_physaddr_t paddr,
	size_t size,
	uint32_t attr)
{
	struct mips_space *space;
	mips_pte_t *leaf;
	mips_pte_t software;
	uintptr_t address;
	uintptr_t offset;
	uintptr_t rollback;
	bool enabled;
	int valid;
	int locked;
	bool ram;
	bool aliases;
	bool executable;

	/* Requires a user range, a page-aligned range of RAM and attributes the tables can hold. */
	address = (uintptr_t)vaddr;
	if (handle == HAL_SPACE_SYS)
		return HAL_ERR_INVALID;
	valid = valid_user(address, size);
	if (!valid)
		return HAL_ERR_INVALID;
	if ((paddr & (MIPS_PAGE_SIZE - 1U)) != 0)
		return HAL_ERR_INVALID;
	ram = mips_range_is_ram(paddr, size);
	if (!ram)
		return HAL_ERR_INVALID;
	valid = valid_attributes(attr, 0);
	if (valid != HAL_OK)
		return valid;
	locked = space_lock_handle(handle, &space, &enabled);
	if (!locked)
		return HAL_ERR_STATE;

	/* Refuses a range of which any page is mapped already. */
	for (offset = 0; offset < size; offset += MIPS_PAGE_SIZE) {
		leaf = walk_leaf(space, address + offset, 0);
		if (leaf != NULL && (*leaf & MIPS_PTE_PRESENT) != 0) {
			mips_irq_restore(enabled);
			return HAL_ERR_INVALID;
		}
	}

	/* Writes an entry for each page, unwinding them if a table cannot be made. */
	software = mips_pte_software_bits(attr);
	for (offset = 0; offset < size; offset += MIPS_PAGE_SIZE) {
		leaf = walk_leaf(space, address + offset, 1);
		if (leaf == NULL) {
			/* Takes back the entries already written. */
			for (rollback = 0; rollback < offset; rollback += MIPS_PAGE_SIZE) {
				leaf = walk_leaf(space, address + rollback, 0);
				if (leaf != NULL)
					*leaf = 0;
			}

			/* Drops the TLB's copies of the entries taken back. */
			flush_pair_range(space, address, offset);

			/* Turns interrupts back on as the caller had them. */
			mips_irq_restore(enabled);
			return HAL_ERR_NOMEM;
		}

		/* Writes the page's entry, unused and clean. */
		*leaf = mips_pte_compose(((mips_pte_t)(paddr + offset) & MIPS_PTE_FRAME_MASK) | software);
	}

	/* A pair the TLB holds with one page invalid takes the new page at once. */
	for (offset = 0; offset < size; offset += MIPS_PAGE_SIZE)
		tlb_sync(space, address + offset, 1);
	mips_irq_restore(enabled);

	/* Pages that sit in several colors leave none of the kernel's lines behind. */
	aliases = mips_cache_aliases();
	executable = false;
	if ((attr & HAL_SPACE_EXEC) != 0)
		executable = true;
	if (aliases) {
		for (offset = 0; offset < size; offset += MIPS_PAGE_SIZE)
			mips_cache_flush_page_colors(paddr + offset, executable);
	}

	/* Code in the pages is fetched from memory, not from stale cache lines. */
	if (executable)
		hal_sync_instruction_stream(hal_pmem_to_kernel(paddr), size);

	/* Succeeded: the range is mapped. */
	return HAL_OK;
}

/*
 * Removes a user range's mappings and their TLB entries.
 */
int
hal_space_unmap(
	hal_space_t handle,
	void *vaddr,
	size_t size)
{
	struct mips_space *space;
	mips_pte_t *leaf;
	hal_physaddr_t frame;
	uintptr_t address;
	uintptr_t offset;
	bool enabled;
	bool aliases;
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
	aliases = mips_cache_aliases();
	locked = space_lock_handle(handle, &space, &enabled);
	if (!locked)
		return HAL_ERR_STATE;

	/*
	 * Clears each entry and drops its pair from the TLB.  Pages that
	 * sit in several colors are emptied before they can be given out.
	 */
	for (offset = 0; offset < size; offset += MIPS_PAGE_SIZE) {
		leaf = walk_leaf(space, address + offset, 0);
		if (leaf == NULL || (*leaf & MIPS_PTE_PRESENT) == 0)
			continue;
		frame = *leaf & MIPS_PTE_FRAME_MASK;
		*leaf = 0;
		tlb_sync(space, address + offset, 0);
		if (aliases)
			mips_cache_flush_page_colors(frame, false);
	}

	/* Turns interrupts back on as the caller had them. */
	mips_irq_restore(enabled);

	/* Succeeded: the range is unmapped; empty leaf tables stay until the space goes. */
	return HAL_OK;
}

/*
 * Changes a mapped user range's protection and reports what the space's
 * table recorded.
 *
 * HAL_SPACE_NONE keeps the pages mapped without any access: a use is a
 * protection fault, not a missing page.
 */
int
hal_space_prot_query(
	hal_space_t handle,
	void *vaddr,
	size_t size,
	uint32_t attr,
	uint32_t *flags)
{
	struct mips_space *space;
	mips_pte_t *leaf;
	uint32_t observed;
	mips_pte_t kept;
	uintptr_t address;
	uintptr_t offset;
	bool enabled;
	bool refetch;
	int valid;
	int locked;

	/* Requires a user range and attributes the tables can hold. */
	address = (uintptr_t)vaddr;
	if (handle == HAL_SPACE_SYS)
		return HAL_ERR_INVALID;
	valid = valid_user(address, size);
	if (!valid)
		return HAL_ERR_INVALID;
	valid = valid_attributes(attr, 1);
	if (valid != HAL_OK)
		return valid;
	locked = space_lock_handle(handle, &space, &enabled);
	if (!locked)
		return HAL_ERR_STATE;

	/* Refuses a range with any page unmapped. */
	for (offset = 0; offset < size; offset += MIPS_PAGE_SIZE) {
		leaf = walk_leaf(space, address + offset, 0);
		if (leaf == NULL || (*leaf & MIPS_PTE_PRESENT) == 0) {
			mips_irq_restore(enabled);
			return HAL_ERR_INVALID;
		}
	}

	/*
	 * A range made executable under virtually tagged instruction lines
	 * is fetched afresh: its pages are written back through the
	 * kernel's address, and the instruction cache is dropped whole.
	 */
	refetch = false;
	if ((attr & HAL_SPACE_EXEC) != 0 && mips_cache_virtual_tags())
		refetch = true;

	/* Rewrites each entry, keeping its page and its records, and brings its pair along. */
	observed = 0;
	for (offset = 0; offset < size; offset += MIPS_PAGE_SIZE) {
		leaf = walk_leaf(space, address + offset, 0);
		observed |= HAL_SPACE_PAGE_PRESENT;
		if ((*leaf & MIPS_PTE_ACCESSED) != 0)
			observed |= HAL_SPACE_PAGE_ACCESSED;
		if ((*leaf & MIPS_PTE_DIRTY) != 0)
			observed |= HAL_SPACE_PAGE_DIRTY;
		kept = *leaf & (MIPS_PTE_FRAME_MASK | MIPS_PTE_ACCESSED | MIPS_PTE_DIRTY);
		kept |= mips_pte_software_bits(attr);
		*leaf = mips_pte_compose(kept);
		tlb_sync(space, address + offset, 1);
		if (refetch)
			hal_dcache_clean_range((uintptr_t)hal_pmem_to_kernel((hal_physaddr_t)(kept & MIPS_PTE_FRAME_MASK)), MIPS_PAGE_SIZE);
	}
	if (refetch)
		mips_cache_invalidate_instructions();

	/* Turns interrupts back on as the caller had them. */
	mips_irq_restore(enabled);

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
	struct mips_space *space;
	mips_pte_t *leaf;
	uint32_t observed;
	bool enabled;
	int locked;
	int valid;

	/* Requires a user page and somewhere to report. */
	if (handle == HAL_SPACE_SYS || flags == NULL)
		return HAL_ERR_INVALID;
	valid = valid_user((uintptr_t)vaddr, MIPS_PAGE_SIZE);
	if (!valid)
		return HAL_ERR_INVALID;
	locked = space_lock_handle(handle, &space, &enabled);
	if (!locked)
		return HAL_ERR_STATE;

	/* Reads the page's entry. */
	observed = 0;
	leaf = walk_leaf(space, (uintptr_t)vaddr, 0);
	if (leaf != NULL && (*leaf & MIPS_PTE_PRESENT) != 0) {
		observed = HAL_SPACE_PAGE_PRESENT;
		if ((*leaf & MIPS_PTE_ACCESSED) != 0)
			observed |= HAL_SPACE_PAGE_ACCESSED;
		if ((*leaf & MIPS_PTE_DIRTY) != 0)
			observed |= HAL_SPACE_PAGE_DIRTY;
	}

	/* Turns interrupts back on as the caller had them. */
	mips_irq_restore(enabled);

	/* Succeeded: the page's state. */
	*flags = observed;
	return HAL_OK;
}

/*
 * Clears the recorded state of one user page.  Clearing accessed makes
 * the pair's page invalid and clearing dirty makes it clean, so that the
 * next use or write records itself again.
 */
int
hal_space_clear_flags(
	hal_space_t handle,
	void *vaddr,
	uint32_t flags)
{
	struct mips_space *space;
	mips_pte_t *leaf;
	mips_pte_t entry;
	bool enabled;
	int locked;
	int valid;

	/* Requires a user page and flags the table records. */
	if (handle == HAL_SPACE_SYS)
		return HAL_ERR_INVALID;
	valid = valid_user((uintptr_t)vaddr, MIPS_PAGE_SIZE);
	if (!valid)
		return HAL_ERR_INVALID;
	if ((flags & ~(uint32_t)(HAL_SPACE_PAGE_ACCESSED | HAL_SPACE_PAGE_DIRTY)) != 0)
		return HAL_ERR_INVALID;
	locked = space_lock_handle(handle, &space, &enabled);
	if (!locked)
		return HAL_ERR_STATE;

	/* Refuses a page that is not mapped. */
	leaf = walk_leaf(space, (uintptr_t)vaddr, 0);
	if (leaf == NULL || (*leaf & MIPS_PTE_PRESENT) == 0) {
		mips_irq_restore(enabled);
		return HAL_ERR_INVALID;
	}

	/* Clears the records, and the TLB's bits that would not set them again. */
	entry = *leaf;
	if ((flags & HAL_SPACE_PAGE_ACCESSED) != 0)
		entry &= ~(mips_pte_t)MIPS_PTE_ACCESSED;
	if ((flags & HAL_SPACE_PAGE_DIRTY) != 0)
		entry &= ~(mips_pte_t)MIPS_PTE_DIRTY;
	*leaf = mips_pte_compose(entry);
	tlb_sync(space, (uintptr_t)vaddr, 1);
	mips_irq_restore(enabled);

	/* Succeeded: the page's state is cleared. */
	return HAL_OK;
}

/*
 * Drops every TLB entry of a space by giving up its ASID; the selected
 * space takes a new one at once.
 */
void
hal_space_flush_tlb(
	hal_space_t handle)
{
	struct mips_space *space;
	bool enabled;
	int locked;

	/* The kernel alone has no entry to drop. */
	if (handle == HAL_SPACE_SYS)
		return;
	locked = space_lock_handle(handle, &space, &enabled);
	if (!locked)
		return;

	/*
	 * Generation zero is never current: the old ASID's entries match
	 * nothing the space selects again, and the ASID is not given to
	 * another space before the TLB is emptied.
	 */
	space->generation = 0;
	if (current_space == handle) {
		asid_assign(space);
		write_entry_hi(space->asid);
	}

	/* Turns interrupts back on as the caller had them. */
	mips_irq_restore(enabled);
}

/*
 * Drops the TLB entries of a range of a space.
 */
void
hal_space_flush_tlb_range(
	hal_space_t handle,
	void *vaddr,
	size_t size)
{
	struct mips_space *space;
	uintptr_t first;
	uintptr_t end;
	uint32_t pairs;
	bool enabled;
	int locked;

	/* The kernel alone and an empty range have no entry to drop. */
	if (handle == HAL_SPACE_SYS || size == 0)
		return;

	/* A range of more pairs than the TLB holds is the whole space. */
	first = (uintptr_t)vaddr & ~(uintptr_t)(SPACE_PAIR_BYTES - 1U);
	end = (uintptr_t)vaddr + size;
	pairs = (uint32_t)((end - first + SPACE_PAIR_BYTES - 1U) / SPACE_PAIR_BYTES);
	if (pairs > tlb_entries) {
		hal_space_flush_tlb(handle);
		return;
	}

	/* Drops each pair of the range under the space's ASID. */
	locked = space_lock_handle(handle, &space, &enabled);
	if (!locked)
		return;
	flush_pair_range(space, first, end - first);
	mips_irq_restore(enabled);
}

/*
 * Reports the size a table level maps: only pages.
 */
size_t
hal_space_get_page_size(
	int level)
{
	/* The tables have no larger page. */
	if (level == 1)
		return MIPS_PAGE_SIZE;
	return 0;
}

/*
 * Reports the user segment's bounds.
 */
void
hal_space_get_user_range(
	uintptr_t *minimum,
	uintptr_t *limit)
{
	/* kuseg (the reach of the tables in xuseg), without its first page. */
	if (minimum != NULL)
		*minimum = MIPS_USER_MINIMUM;
	if (limit != NULL)
		*limit = MIPS_USER_LIMIT;
}

/*
 * Reports where the kernel reaches a device range, uncached: in KSEG1 for
 * anything in the low 512 MiB of physical space on the 32-bit processor,
 * in xkphys for any physical address on the 64-bit one.
 */
int
hal_space_map_device(
	hal_physaddr_t paddr,
	size_t size,
	uint32_t attr,
	void **vaddr)
{
	/* Requires a destination, a non-empty range and no execution. */
	if (vaddr == NULL || size == 0)
		return HAL_ERR_INVALID;
	if ((attr & HAL_SPACE_EXEC) != 0)
		return HAL_ERR_INVALID;

#if !defined(HAL_ARCH_MIPS64)
	/* Only the low 512 MiB has a fixed uncached window. */
	if ((uint64_t)paddr >= MIPS_KSEG_SIZE || (uint64_t)size > MIPS_KSEG_SIZE - (uint64_t)paddr)
		return HAL_ERR_UNSUPPORTED;
#endif

	/* Succeeded: the range in the uncached segment. */
	*vaddr = (void *)mips_uncached_address(paddr);
	return HAL_OK;
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
 * Reports how many user spaces and leaf tables exist.
 */
void
mips_space_memory_stats(
	uint32_t *spaces,
	uint32_t *tables)
{
	bool enabled;

	/* Samples both counts together. */
	enabled = mips_irq_save();

	if (spaces != NULL)
		*spaces = space_count;
	if (tables != NULL)
		*tables = page_table_count;

	mips_irq_restore(enabled);
}

/*
 * Reports the TLB exceptions counted since boot.
 */
void
mips_space_counters(
	struct mips_space_counters *counters)
{
	bool enabled;

	/* Samples the three counts together. */
	enabled = mips_irq_save();

	counters->refills = __atomic_load_n(&mips_tlb_refill_count, __ATOMIC_RELAXED);
	counters->invalids = tlb_invalid_count;
	counters->modifies = tlb_modified_count;

	mips_irq_restore(enabled);
}

/*
 * Sets the last ASID a generation gives out and reports the one before,
 * so that the self-test can make the generations turn and put the limit
 * back.  A limit of zero or above the hardware's is refused and changes
 * nothing.
 */
uint32_t
mips_space_set_asid_limit(
	uint32_t limit)
{
	uint32_t previous;
	bool enabled;

	/* Swaps the limit in. */
	enabled = mips_irq_save();

	/* Keeps the limit before and takes a limit the hardware allows. */
	previous = asid_limit;
	if (limit != 0 && limit <= SPACE_ASID_LIMIT)
		asid_limit = limit;

	mips_irq_restore(enabled);

	/* Succeeded: the limit before. */
	return previous;
}

/*
 * Reports the TLB's current generation.
 */
uint64_t
mips_space_generation(
	void)
{
	/* Succeeded: the generation. */
	return asid_generation;
}

/*
 * Reports a space's ASID, or zero when it holds none of the current
 * generation.
 */
uint32_t
mips_space_asid(
	hal_space_t handle)
{
	struct mips_space *space;
	uint32_t asid;
	bool enabled;
	int locked;

	/* The kernel alone has ASID 0. */
	if (handle == HAL_SPACE_SYS)
		return 0;
	locked = space_lock_handle(handle, &space, &enabled);
	if (!locked)
		return 0;

	/* An ASID of an older generation is no ASID. */
	asid = 0;
	if (space->generation == asid_generation)
		asid = space->asid;
	mips_irq_restore(enabled);

	/* Succeeded: the space's ASID. */
	return asid;
}

/*
 * Finds a live user space by its handle with interrupts off for its
 * operation.  Reports 0 for a handle that names no live space.
 */
static int
space_lock_handle(
	hal_space_t handle,
	struct mips_space **result,
	bool *irq_enabled)
{
	struct mips_space *space;
	bool enabled;

	/* Looks the handle up among the live spaces. */
	enabled = mips_irq_save();
	for (space = space_registry; space != NULL; space = space->registry_next) {
		if ((hal_space_t)space == handle)
			break;
	}

	/* A handle that names no live space is refused. */
	if (space == NULL || space->destroying) {
		mips_irq_restore(enabled);
		return 0;
	}

	/* Succeeded: the caller holds the space until it turns interrupts back on. */
	*result = space;
	*irq_enabled = enabled;
	return 1;
}

/* Reports whether a range is page-aligned, non-empty and inside the user segment. */
static int
valid_user(
	uintptr_t address,
	size_t size)
{
	/* An empty or unaligned range is refused. */
	if (size == 0)
		return 0;
	if ((address & (MIPS_PAGE_SIZE - 1U)) != 0 || (size & (MIPS_PAGE_SIZE - 1U)) != 0)
		return 0;

	/* The range must lie inside the user segment. */
	if (address < MIPS_USER_MINIMUM || address >= MIPS_USER_LIMIT)
		return 0;
	if (size > MIPS_USER_LIMIT - address)
		return 0;
	return 1;
}

/*
 * Checks a user mapping's attributes: some access (unless a protection
 * change may take none), never written and executed at once, and no
 * cache behaviour or device class.
 */
static int
valid_attributes(
	uint32_t attr,
	int none_allowed)
{
	/* A mapping must allow some access, and not both writing and executing. */
	if (!none_allowed && (attr & (HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC)) == 0)
		return HAL_ERR_INVALID;
	if ((attr & HAL_SPACE_WRITE) != 0 && (attr & HAL_SPACE_EXEC) != 0)
		return HAL_ERR_INVALID;

	/* Device pages and other cache behaviours are not offered to users. */
	if ((attr & (HAL_SPACE_NOCACHE | HAL_SPACE_WRITETHRU | HAL_SPACE_WC | HAL_SPACE_DEVICE)) != 0)
		return HAL_ERR_UNSUPPORTED;
	return HAL_OK;
}

/*
 * Finds the space's entry of a user address, making the tables on the
 * way when asked to.  Reports NULL when a table is missing and not to be
 * made, or cannot be made.
 */
static mips_pte_t *
walk_leaf(
	struct mips_space *space,
	uintptr_t address,
	int create)
{
	uintptr_t *table;
	unsigned level;
	unsigned shift;
	unsigned index;

	/* Each level above the leaves: the root, and on the 64-bit processor a middle table. */
	table = space->root;
	for (level = 0; level + 1U < MIPS_TABLE_LEVELS; level++) {
		shift = MIPS_PAGE_SHIFT + MIPS_TABLE_INDEX_BITS * (MIPS_TABLE_LEVELS - 1U - level);
		index = (unsigned)((address >> shift) & (MIPS_TABLE_ENTRIES - 1U));
		table = table_below(space, table, index, create);
		if (table == NULL)
			return NULL;
	}

	/* Succeeded: the page's entry. */
	return (mips_pte_t *)&table[(address >> MIPS_PAGE_SHIFT) & (MIPS_TABLE_ENTRIES - 1U)];
}

/*
 * Reports the table an entry of a table above the leaves points to,
 * making it when the entry is empty and asked to.  Reports NULL when it
 * is missing and not to be made, or cannot be made.
 */
static uintptr_t *
table_below(
	struct mips_space *space,
	uintptr_t *table,
	unsigned index,
	int create)
{
	struct mips_table_page *page;
	uintptr_t *below;
	int error;

	/* An entry already made is the kernel's address of the table. */
	if (table[index] != 0)
		return (uintptr_t *)table[index];
	if (!create)
		return NULL;

	/* A page for the table, with its record. */
	page = kernel_alloc(sizeof(*page));
	if (page == NULL)
		return NULL;
	error = hal_pmem_alloc(MIPS_PAGE_SIZE, MIPS_PAGE_SIZE, &page->memory);
	if (error != HAL_OK) {
		kernel_free(page);
		return NULL;
	}

	/* Clears the table and links it into the space, where the refill finds it. */
	below = hal_pmem_to_kernel(page->memory);
	hal_memset(below, 0, MIPS_PAGE_SIZE);
	page->root_index = index;
	page->next = space->tables;
	space->tables = page;
	table[index] = (uintptr_t)below;
	page_table_count++;

	/* Succeeded: the new, empty table. */
	return below;
}

/*
 * Brings the TLB's entry of an address's pair in line with the table:
 * written again from the table's two entries, or, when asked to drop
 * it, replaced by an entry that matches nothing.  A space without an
 * ASID of the current generation has no entry.  Called with interrupts
 * off.
 */
static void
tlb_sync(
	struct mips_space *space,
	uintptr_t address,
	int rewrite)
{
	mips_pte_t *even;
	uintptr_t low0;
	uintptr_t low1;
	uintptr_t entry_hi;

	/* Without a current ASID the TLB holds nothing of the space. */
	if (space->generation != asid_generation)
		return;

	/* The pair's two entries, shifted as the refill shifts them. */
	low0 = 0;
	low1 = 0;
	if (rewrite) {
		even = walk_leaf(space, address & ~(uintptr_t)(SPACE_PAIR_BYTES - 1U), 0);
		if (even == NULL) {
			rewrite = 0;
		} else {
			low0 = even[0] >> MIPS_PTE_HARDWARE_SHIFT;
			low1 = even[1] >> MIPS_PTE_HARDWARE_SHIFT;
		}
	}

	/* Probes for the pair under the space's ASID and updates what is found. */
	entry_hi = (address & (uintptr_t)MIPS_ENTRY_HI_VPN2_MASK) | space->asid;
	tlb_update(entry_hi, rewrite, low0, low1);
}

/*
 * Probes the TLB for an EntryHi and, when an entry matches, writes it
 * with the two EntryLo values or replaces it by an entry that matches
 * nothing: a KSEG0 pair, which the TLB never translates, unique to its
 * index, with both pages invalid.  The current EntryHi is put back.
 *
 * tlbwi, never tlbwr: a random write would leave the matching entry in
 * place and make two entries match, which a real processor takes as a
 * machine check.  Called with interrupts off.
 *
 * Values referenced: MIPS32 and MIPS64 Architecture for Programmers
 * Volume III (the TLB instructions tlbp, tlbwi and tlbwr, the EntryHi,
 * EntryLo, Index, PageMask and Wired registers, the TLB refill, invalid
 * and modified exceptions, and the execution hazards of their writes).
 */
static void
tlb_update(
	uintptr_t entry_hi,
	int rewrite,
	uintptr_t low0,
	uintptr_t low1)
{
	uintptr_t saved;
	uint32_t index;

	/* Keeps the EntryHi of the space selected now. */
	saved = read_entry_hi();

	/* Probes; Index has its top bit set when nothing matches. */
	__asm__ volatile(MIPS_WIDE_MTC0_STRING " %1, $10\n\t"
			 "ehb\n\t"
			 "tlbp\n\t"
			 "ehb\n\t"
			 "mfc0 %0, $0"
			 : "=r"(index)
			 : "r"(entry_hi)
			 : "memory");

	/* Writes the matching entry, again or as one that matches nothing. */
	if ((index & MIPS_INDEX_PROBE_FAILED) == 0) {
		if (rewrite) {
			tlb_write_index(index, entry_hi, low0, low1);
		} else {
			tlb_write_index(index, invalid_entry_hi(index), 0, 0);
		}
	}

	/* Puts the selected space's EntryHi back. */
	write_entry_hi(saved);
}

/* Writes one TLB entry at an index. */
static void
tlb_write_index(
	uint32_t index,
	uintptr_t entry_hi,
	uintptr_t low0,
	uintptr_t low1)
{
	/* The index, the pair's tag and pages, then the write once they are in place. */
	__asm__ volatile("mtc0 %0, $0\n\t"
			 MIPS_WIDE_MTC0_STRING " %1, $10\n\t"
			 MIPS_WIDE_MTC0_STRING " %2, $2\n\t"
			 MIPS_WIDE_MTC0_STRING " %3, $3\n\t"
			 "ehb\n\t"
			 "tlbwi\n\t"
			 "ehb"
			 :
			 : "r"(index), "r"(entry_hi), "r"(low0), "r"(low1)
			 : "memory");
}

/*
 * Reports the EntryHi of an entry that matches nothing, unique to its
 * index: a pair of KSEG0 (on the 64-bit processor CKSEG0), which the TLB
 * never translates, under ASID 0.
 */
static uintptr_t
invalid_entry_hi(
	uint32_t index)
{
	/* Succeeded: the index's own pair of the unmapped segment. */
	return mips_segment_pointer(MIPS_KSEG0_BASE + index * SPACE_PAIR_BYTES);
}

/*
 * Replaces every TLB entry by one that matches nothing, each with a
 * KSEG0 tag of its own so that no two entries are alike.  Called with
 * interrupts off or before they are on.
 */
static void
tlb_flush_all(
	void)
{
	uintptr_t saved;
	uint32_t index;

	/* Keeps the EntryHi of the space selected now. */
	saved = read_entry_hi();

	/* Writes each entry. */
	for (index = 0; index < tlb_entries; index++)
		tlb_write_index(index, invalid_entry_hi(index), 0, 0);

	/* Puts the selected space's EntryHi back. */
	write_entry_hi(saved);
}

/* Reads EntryHi. */
static uintptr_t
read_entry_hi(
	void)
{
	uintptr_t value;

	/* The register. */
	__asm__ volatile(MIPS_WIDE_MFC0_STRING " %0, $10" : "=r"(value));

	/* Succeeded: its value. */
	return value;
}

/* Writes EntryHi and clears the hazard before the TLB is used with it. */
static void
write_entry_hi(
	uintptr_t value)
{
	/* The write, then the hazard barrier. */
	__asm__ volatile(MIPS_WIDE_MTC0_STRING " %0, $10\n\tehb" : : "r"(value) : "memory");
}

/*
 * Gives a space an ASID of the current generation when it has none.
 *
 * When the generation has given out its last ASID, the next generation
 * starts with the TLB emptied: every space's ASID is then old, and each
 * takes a new one when it is next selected.  Called with interrupts off.
 */
static void
asid_assign(
	struct mips_space *space)
{
	/* A space with a current ASID keeps it. */
	if (space->generation == asid_generation)
		return;

	/*
	 * A used-up generation ends: the TLB is emptied, so no entry of an
	 * old ASID can match a space that takes the same number next.
	 */
	if (asid_next > asid_limit) {
		asid_generation++;
		tlb_flush_all();
		asid_next = 1;

		/*
		 * Virtually tagged instruction lines carry the old ASIDs too,
		 * which the new generation gives out again.
		 */
		if (mips_cache_virtual_tags())
			mips_cache_invalidate_instructions();
	}

	/* Hands out the next ASID of the generation. */
	space->asid = asid_next;
	space->generation = asid_generation;
	asid_next++;
}

/* Drops the TLB's entries of every pair a range touches.  Called with interrupts off. */
static void
flush_pair_range(
	struct mips_space *space,
	uintptr_t address,
	size_t size)
{
	uintptr_t pair;
	uintptr_t end;

	/* Each pair from the first to the one holding the last byte. */
	end = address + size;
	for (pair = address & ~(uintptr_t)(SPACE_PAIR_BYTES - 1U); pair < end; pair += SPACE_PAIR_BYTES)
		tlb_sync(space, pair, 0);
}

/* Frees a space's leaf tables. */
static void
free_tables(
	struct mips_space *space)
{
	struct mips_table_page *page;
	bool enabled;

	/* Each page and its record. */
	while (space->tables != NULL) {
		page = space->tables;
		space->tables = page->next;
		(void)hal_pmem_free(&page->memory, MIPS_PAGE_SIZE);
		kernel_free(page);
		enabled = mips_irq_save();
		page_table_count--;
		mips_irq_restore(enabled);
	}
}
