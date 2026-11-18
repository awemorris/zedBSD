/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 32-bit PowerPC address spaces.
 *
 * The supervisor reaches RAM and the device windows through block
 * address translation, so the kernel has no pages of its own.  RAM is
 * mapped from 0 in power-of-two blocks of at most 256 MiB, up to where
 * the user half starts; each device window is one 256 MiB block above
 * the user half.  The processor's four data pairs (eight on the 7455 and
 * later) go first to RAM's first two blocks, then to the device windows
 * in the board's order, then to the rest of RAM; RAM ends at the first
 * block that has no pair.  The user half,
 * segments 4 to 7, goes through the hashed page table, which is only a
 * cache: each space keeps its pages in a two-level table of its own, and
 * a storage fault on a user address puts the page into the hashed table
 * from there.  An entry may be dropped from the hashed table at any time.
 *
 * A space's segments get virtual segment ids of their own, so switching
 * spaces is loading four segment registers.  The kernel alone has ids no
 * entry ever uses.
 *
 * Accessed and modified are kept in the space's table: a page enters the
 * hashed table read-only until it is written, and the first write's
 * protection fault records the change and makes the entry writable.
 *
 * Values referenced: PowerPC Microprocessor Family: The Programming
 * Environments for 32-Bit Microprocessors (MPCFPE32B), the BAT registers,
 * the segment registers, the page table entry and the hash functions.
 */

#include <hal/hal.h>
#include "asm.h"
#include "defs.h"
#include "page.h"
#include "space.h"

/* The hashed table: 64 KiB, 1024 groups of eight entries. */
#define HTAB_BYTES		0x10000U
#define HTAB_GROUPS		1024U
#define HTAB_GROUP_ENTRIES	8U

/* A hashed table entry's first word: valid, the segment id, the hash function, the page's top bits. */
#define PTE_VALID		0x80000000U
#define PTE_VSID_SHIFT		7
#define PTE_SECONDARY		0x40U

/* A hashed table entry's second word: memory coherence, read-write or read-only. */
#define PTE_COHERENT		0x10U
#define PTE_READ_WRITE		0x2U
#define PTE_READ_ONLY		0x3U

/* A segment register: the user's key is 1, the supervisor's 0. */
#define SEGMENT_USER_KEY	0x20000000U

/* The space's own leaf entry: the page, and the software's bits. */
#define LEAF_VALID		0x1U
#define LEAF_WRITE		0x2U
#define LEAF_EXEC		0x4U
#define LEAF_ACCESSED		0x8U
#define LEAF_MODIFIED		0x10U
#define LEAF_PAGE_MASK		0xfffff000U

/*
 * A block translation: its length field (BL, the length in 128 KiB units
 * less one, at bit 2), supervisor only; RAM coherent, the device windows
 * uncached and guarded.
 */
#define BAT_LENGTH_SHIFT	17U
#define BAT_LENGTH_FIELD_SHIFT	2U
#define BAT_SMALLEST		0x20000U
#define BAT_SUPERVISOR		0x2U
#define BAT_COHERENT		0x10U
#define BAT_UNCACHED_GUARDED	0x28U
#define BAT_READ_WRITE		0x2U
#define BAT_ADDRESS_MASK	0xfffe0000U

/* The size of a device window, and the largest RAM block: 256 MiB. */
#define DEVICE_WINDOW_BYTES	0x10000000U

/* The most RAM blocks: 1 GiB in 256 MiB blocks, or fewer and smaller. */
#define RAM_BLOCKS_MAX		PPC32_BAT_PAIRS_HIGH

/*
 * The device windows the block translations map, reached at their own
 * addresses, and how many; the end of RAM they map.  Set once by
 * ppc32_space_init() and constant afterwards.
 */
static uint32_t device_window[PPC32_BAT_PAIRS_HIGH];
static unsigned device_window_count;
static uint64_t mapped_ram_end;

/* The hashed table, aligned to its size as SDR1 asks. */
static uint32_t htab[HTAB_BYTES / 4U] __attribute__((aligned(HTAB_BYTES)));

/* The segment ids the kernel alone loads into the user segments. */
static const uint32_t system_segments[4] = {
	0x00fffff4U | SEGMENT_USER_KEY, 0x00fffff5U | SEGMENT_USER_KEY,
	0x00fffff6U | SEGMENT_USER_KEY, 0x00fffff7U | SEGMENT_USER_KEY
};

/* The space selected now, and the round-robin victim of a full group. */
static hal_space_t current_space;
static unsigned evict_next;

/*
 * Every user space that exists, the number the next one gets, how many
 * there are and how many table pages they hold.  Kept with interrupts off.
 */
static struct ppc32_space *space_registry;
static uint32_t next_space_id = 1;
static uint32_t space_count;
static uint32_t page_table_count;

static int space_lock_handle(hal_space_t handle, struct ppc32_space **result, bool *irq_enabled);
static int valid_user(uintptr_t address, size_t size);
static int valid_attributes(uint32_t attr);
static uint32_t *walk_leaf(struct ppc32_space *space, uintptr_t address, int create);
static void drop_entry(struct ppc32_space *space, uintptr_t address);
static void put_entry(struct ppc32_space *space, uintptr_t address, uint32_t leaf, int writable);
static uint32_t segment_vsid(const struct ppc32_space *space, uintptr_t address);
static uint32_t *group_of(uint32_t hash);
static uintptr_t victim_address(uint32_t word0, uint32_t group_index);
static void free_tables(struct ppc32_space *space);
static int in_device_window(hal_physaddr_t paddr, size_t size);
static void set_bat(uint32_t *bats, unsigned pair, uint32_t address, uint32_t size, uint32_t lower, bool instructions);

/*
 * Takes the processor from the firmware's translation to the kernel's:
 * RAM to ram_end (at most up to the user half) and the device windows,
 * the board's 256 MiB blocks in the order it needs them, in blocks; the
 * user segments empty, the hashed table cleared.
 */
void
ppc32_space_init(
	uint64_t ram_end,
	const uint32_t *windows,
	unsigned window_count)
{
	uint32_t bats[PPC32_BAT_PAIRS_HIGH * 4U];
	uint32_t segments[16];
	uint32_t ram_base[RAM_BLOCKS_MAX];
	uint32_t ram_size[RAM_BLOCKS_MAX];
	uint64_t address;
	uint64_t left;
	uint32_t size;
	unsigned ram_count;
	unsigned pairs;
	unsigned pair;
	unsigned index;
	unsigned ram_mapped;
	bool high;

	/* RAM in power-of-two blocks from 0, largest first, so each is aligned to its size. */
	if (ram_end > PPC32_RAM_WINDOW_LIMIT)
		ram_end = PPC32_RAM_WINDOW_LIMIT;
	ram_count = 0;
	address = 0;
	while (address < ram_end && ram_count < RAM_BLOCKS_MAX) {
		left = ram_end - address;
		if (left < BAT_SMALLEST)
			break;
		size = DEVICE_WINDOW_BYTES;
		while (size > left)
			size >>= 1;
		ram_base[ram_count] = (uint32_t)address;
		ram_size[ram_count] = size;
		ram_count++;
		address += size;
	}
	if (ram_count == 0)
		HAL_FATAL("ppc32 has no RAM to map");

	/* The pairs: the first two RAM blocks, the windows, then the rest of RAM. */
	high = ppc32_cpu_has_high_bats();
	pairs = high ? PPC32_BAT_PAIRS_HIGH : PPC32_BAT_PAIRS;
	hal_memset(bats, 0, sizeof(bats));
	pair = 0;
	ram_mapped = 0;
	while (ram_mapped < ram_count && ram_mapped < 2U && pair < pairs) {
		set_bat(bats, pair, ram_base[ram_mapped], ram_size[ram_mapped], BAT_COHERENT | BAT_READ_WRITE, true);
		pair++;
		ram_mapped++;
	}
	device_window_count = 0;
	for (index = 0; index < window_count; index++) {
		/* A window over RAM or the user half would hide them. */
		if (windows[index] < PPC32_USER_LIMIT) {
			hal_printf("PPC32 WINDOW %08x REFUSED\n", windows[index]);
			continue;
		}
		if (pair == pairs) {
			hal_printf("PPC32 WINDOW %08x UNMAPPED\n", windows[index]);
			continue;
		}
		set_bat(bats, pair, windows[index] & ~(DEVICE_WINDOW_BYTES - 1U), DEVICE_WINDOW_BYTES,
			BAT_UNCACHED_GUARDED | BAT_READ_WRITE, false);
		device_window[device_window_count] = windows[index] & ~(DEVICE_WINDOW_BYTES - 1U);
		device_window_count++;
		hal_printf("PPC32 WINDOW %08x\n", windows[index]);
		pair++;
	}
	while (ram_mapped < ram_count && pair < pairs) {
		set_bat(bats, pair, ram_base[ram_mapped], ram_size[ram_mapped], BAT_COHERENT | BAT_READ_WRITE, true);
		pair++;
		ram_mapped++;
	}
	mapped_ram_end = (uint64_t)ram_base[ram_mapped - 1U] + ram_size[ram_mapped - 1U];
	hal_printf("PPC32 RAM WINDOW %llu MiB IN %u BLOCKS, %u OF %u PAIRS\n",
		   mapped_ram_end / (1024U * 1024U), ram_mapped, pair, pairs);

	/* Every segment the kernel's; the user ones with ids that map nothing. */
	for (index = 0; index < 16U; index++)
		segments[index] = 0x00fff000U | index;
	for (index = 0; index < 4U; index++)
		segments[PPC32_USER_SEGMENT_FIRST + index] = system_segments[index];

	/* An empty hashed table, then the switch. */
	hal_memset(htab, 0, sizeof(htab));
	space_registry = NULL;
	current_space = HAL_SPACE_SYS;
	ppc32_mmu_load(bats, segments, (uint32_t)(uintptr_t)htab, high ? 1U : 0U);
	hal_puts("PPC32 PAGING PASS\n");
}

/*
 * Reports the end of the RAM the block translations map: the most RAM
 * the kernel can reach.
 */
uint64_t
ppc32_space_ram_end(
	void)
{
	/* Succeeded: fixed at boot. */
	return mapped_ram_end;
}

/*
 * Puts a user page back into the hashed table after a storage fault, and
 * records its use.  Reports whether the fault was such a page.
 */
int
ppc32_space_reload(
	uintptr_t address,
	int store,
	int execute)
{
	struct ppc32_space *space;
	uint32_t *leaf;
	int writable;

	/* Only the user half of a user space has pages. */
	if (address < PPC32_USER_MINIMUM || address >= PPC32_USER_LIMIT)
		return 0;
	if (current_space == HAL_SPACE_SYS)
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
	writable = (*leaf & LEAF_WRITE) != 0 && (*leaf & LEAF_MODIFIED) != 0;
	drop_entry(space, address);
	put_entry(space, address, *leaf, writable);

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
	struct ppc32_space *space;
	unsigned index;
	bool enabled;
	int error;

	/* The record and its root. */
	space = kernel_alloc(sizeof(*space));
	if (space == NULL)
		return NULL;
	hal_memset(space, 0, sizeof(*space));
	error = hal_pmem_alloc(PPC32_PAGE_SIZE, PPC32_PAGE_SIZE, &space->root_memory);
	if (error != HAL_OK) {
		kernel_free(space);
		return NULL;
	}

	/* Clears the root through the kernel's view of it. */
	space->root = hal_pmem_to_kernel(space->root_memory);
	hal_memset(space->root, 0, PPC32_PAGE_SIZE);
	space->magic = PPC32_SPACE_MAGIC;

	/* Numbers the space, gives its segments their ids and makes it live. */
	enabled = ppc32_irq_save();
	space->space_id = next_space_id++;
	for (index = 0; index < 4U; index++)
		space->segments[index] = ((space->space_id << 4) | (PPC32_USER_SEGMENT_FIRST + index)) | SEGMENT_USER_KEY;
	space->registry_next = space_registry;
	space_registry = space;
	space_count++;
	ppc32_irq_restore(enabled);

	/* Succeeded: the space, with nothing mapped. */
	return space;
}

/*
 * Destroys a user space that is not selected, and drops its entries from
 * the hashed table so that its ids could be met again.
 */
void
hal_space_destroy(
	hal_space_t handle)
{
	struct ppc32_space *space;
	struct ppc32_space **link;
	uint32_t vsid;
	unsigned index;
	bool enabled;

	/* A missing space is nothing to destroy. */
	space = handle;
	if (space == NULL)
		return;

	/* Takes the space out of the registry and its entries out of the table. */
	enabled = ppc32_irq_save();
	link = &space_registry;
	while (*link != NULL && *link != space)
		link = &(*link)->registry_next;
	if (*link == NULL || space->destroying)
		HAL_FATAL("invalid ppc32 space destroy");
	if (current_space == handle)
		HAL_FATAL("destroying an active ppc32 space");
	space->destroying = 1U;
	*link = space->registry_next;
	if (space_count != 0)
		space_count--;
	for (index = 0; index < HTAB_BYTES / 4U; index += 2U) {
		vsid = (htab[index] >> PTE_VSID_SHIFT) & 0x00ffffffU;
		if ((htab[index] & PTE_VALID) != 0 && (vsid >> 4) == space->space_id)
			htab[index] = 0;
	}

	/* Orders the removals before the table is used again. */
	__asm__ volatile("sync" : : : "memory");
	ppc32_irq_restore(enabled);

	/* Frees the tables, then the root and the record. */
	free_tables(space);
	space->magic = 0;
	(void)hal_pmem_free(&space->root_memory, PPC32_PAGE_SIZE);
	kernel_free(space);
}

/*
 * Selects a user space, or the kernel alone: four segment registers.
 */
void
hal_space_switch(
	hal_space_t handle)
{
	struct ppc32_space *space;
	bool enabled;
	int locked;

	/* The space already selected needs nothing. */
	if (handle == current_space)
		return;

	/* The kernel alone: ids that map nothing. */
	if (handle == HAL_SPACE_SYS) {
		enabled = ppc32_irq_save();
		ppc32_load_user_segments(system_segments);
		current_space = handle;
		ppc32_irq_restore(enabled);
		return;
	}

	/* A user space: its own ids. */
	locked = space_lock_handle(handle, &space, &enabled);
	if (!locked)
		HAL_FATAL("invalid ppc32 space switch");
	ppc32_load_user_segments(space->segments);
	current_space = handle;
	ppc32_irq_restore(enabled);
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
	struct ppc32_space *space;
	uint32_t *leaf;
	uint32_t flags;
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
	if ((paddr & (PPC32_PAGE_SIZE - 1U)) != 0)
		return HAL_ERR_INVALID;
	ram = ppc32_range_is_ram(paddr, size);
	if (!ram)
		return HAL_ERR_INVALID;
	valid = valid_attributes(attr);
	if (valid != HAL_OK)
		return valid;
	locked = space_lock_handle(handle, &space, &enabled);
	if (!locked)
		return HAL_ERR_STATE;

	/* Refuses a range of which any page is mapped already. */
	for (offset = 0; offset < size; offset += PPC32_PAGE_SIZE) {
		leaf = walk_leaf(space, address + offset, 0);
		if (leaf != NULL && (*leaf & LEAF_VALID) != 0) {
			ppc32_irq_restore(enabled);
			return HAL_ERR_INVALID;
		}
	}

	/* Writes a leaf for each page, unwinding them if a table cannot be made. */
	flags = LEAF_VALID;
	if ((attr & HAL_SPACE_WRITE) != 0)
		flags |= LEAF_WRITE;
	if ((attr & HAL_SPACE_EXEC) != 0)
		flags |= LEAF_EXEC;
	for (offset = 0; offset < size; offset += PPC32_PAGE_SIZE) {
		leaf = walk_leaf(space, address + offset, 1);
		if (leaf == NULL) {
			for (rollback = 0; rollback < offset; rollback += PPC32_PAGE_SIZE) {
				leaf = walk_leaf(space, address + rollback, 0);
				if (leaf != NULL)
					*leaf = 0;
			}

			/* Turns interrupts back on as the caller had them. */
			ppc32_irq_restore(enabled);
			return HAL_ERR_NOMEM;
		}

		/* Writes the page's leaf. */
		*leaf = ((uint32_t)(paddr + offset) & LEAF_PAGE_MASK) | flags;
	}

	/* Code in the pages is fetched from memory, not from stale cache lines. */
	if ((attr & HAL_SPACE_EXEC) != 0)
		hal_sync_instruction_stream(hal_pmem_to_kernel(paddr), size);
	ppc32_irq_restore(enabled);

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
	struct ppc32_space *space;
	uint32_t *leaf;
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

	/* Clears each leaf and drops its entry. */
	for (offset = 0; offset < size; offset += PPC32_PAGE_SIZE) {
		leaf = walk_leaf(space, address + offset, 0);
		if (leaf == NULL || (*leaf & LEAF_VALID) == 0)
			continue;
		*leaf = 0;
		drop_entry(space, address + offset);
	}

	/* Turns interrupts back on as the caller had them. */
	ppc32_irq_restore(enabled);

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
	struct ppc32_space *space;
	uint32_t *leaf;
	uint32_t observed;
	uint32_t kept;
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
	for (offset = 0; offset < size; offset += PPC32_PAGE_SIZE) {
		leaf = walk_leaf(space, address + offset, 0);
		if (leaf == NULL || (*leaf & LEAF_VALID) == 0) {
			ppc32_irq_restore(enabled);
			return HAL_ERR_INVALID;
		}
	}

	/* Rewrites each leaf, keeping its page and its records, and drops its entry. */
	observed = 0;
	for (offset = 0; offset < size; offset += PPC32_PAGE_SIZE) {
		leaf = walk_leaf(space, address + offset, 0);
		observed |= HAL_SPACE_PAGE_PRESENT;
		if ((*leaf & LEAF_ACCESSED) != 0)
			observed |= HAL_SPACE_PAGE_ACCESSED;
		if ((*leaf & LEAF_MODIFIED) != 0)
			observed |= HAL_SPACE_PAGE_DIRTY;
		kept = *leaf & (LEAF_PAGE_MASK | LEAF_ACCESSED | LEAF_MODIFIED);
		kept |= LEAF_VALID;
		if ((attr & HAL_SPACE_WRITE) != 0)
			kept |= LEAF_WRITE;
		if ((attr & HAL_SPACE_EXEC) != 0)
			kept |= LEAF_EXEC;
		*leaf = kept;
		drop_entry(space, address + offset);

		/* A page made executable has its code fetched from memory, not from stale cache lines. */
		if ((attr & HAL_SPACE_EXEC) != 0)
			hal_sync_instruction_stream(hal_pmem_to_kernel(kept & LEAF_PAGE_MASK), PPC32_PAGE_SIZE);
	}

	/* Turns interrupts back on as the caller had them. */
	ppc32_irq_restore(enabled);

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
	struct ppc32_space *space;
	uint32_t *leaf;
	uint32_t observed;
	bool enabled;
	int locked;
	int valid;

	/* Requires a user page and somewhere to report. */
	if (handle == HAL_SPACE_SYS || flags == NULL)
		return HAL_ERR_INVALID;
	valid = valid_user((uintptr_t)vaddr, PPC32_PAGE_SIZE);
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
	ppc32_irq_restore(enabled);

	/* Succeeded: the page's state. */
	*flags = observed;
	return HAL_OK;
}

/*
 * Clears the recorded state of one user page; its entry is dropped, so
 * the next use records it again.
 */
int
hal_space_clear_flags(
	hal_space_t handle,
	void *vaddr,
	uint32_t flags)
{
	struct ppc32_space *space;
	uint32_t *leaf;
	bool enabled;
	int locked;
	int valid;

	/* Requires a user page and flags the table records. */
	if (handle == HAL_SPACE_SYS)
		return HAL_ERR_INVALID;
	valid = valid_user((uintptr_t)vaddr, PPC32_PAGE_SIZE);
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
		ppc32_irq_restore(enabled);
		return HAL_ERR_INVALID;
	}

	/* Clears the records and the entry that would not set them again. */
	if ((flags & HAL_SPACE_PAGE_ACCESSED) != 0)
		*leaf &= ~(uint32_t)LEAF_ACCESSED;
	if ((flags & HAL_SPACE_PAGE_DIRTY) != 0)
		*leaf &= ~(uint32_t)LEAF_MODIFIED;
	drop_entry(space, (uintptr_t)vaddr);
	ppc32_irq_restore(enabled);

	/* Succeeded: the page's state is cleared. */
	return HAL_OK;
}

/*
 * Drops the stale translations of a space: its entries are always
 * dropped when its table changes, so there is nothing left to do.
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
	/* The hashed table has no larger page. */
	if (level == 1)
		return PPC32_PAGE_SIZE;
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
	/* Segments 4 to 7. */
	if (minimum != NULL)
		*minimum = PPC32_USER_MINIMUM;
	if (limit != NULL)
		*limit = PPC32_USER_LIMIT;
}

/*
 * Reports where the kernel reaches a device range: RAM and the device
 * windows are reached at their own addresses; nothing else is mapped.
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
	ram = ppc32_range_is_ram(paddr, size);
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
	for (line = (uintptr_t)addr & ~(uintptr_t)31U; line < end; line += 32U)
		__asm__ volatile("dcbst 0,%0" : : "r"(line) : "memory");
	__asm__ volatile("sync" : : : "memory");
	for (line = (uintptr_t)addr & ~(uintptr_t)31U; line < end; line += 32U)
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
ppc32_space_memory_stats(
	uint32_t *spaces,
	uint32_t *tables)
{
	bool enabled;

	/* Samples both counts together. */
	enabled = ppc32_irq_save();
	if (spaces != NULL)
		*spaces = space_count;
	if (tables != NULL)
		*tables = page_table_count;
	ppc32_irq_restore(enabled);
}

/*
 * Finds a live user space by its handle with interrupts off for its
 * operation.  Reports 0 for a handle that names no live space.
 */
static int
space_lock_handle(
	hal_space_t handle,
	struct ppc32_space **result,
	bool *irq_enabled)
{
	struct ppc32_space *space;
	bool enabled;

	/* Looks the handle up among the live spaces. */
	enabled = ppc32_irq_save();
	for (space = space_registry; space != NULL; space = space->registry_next) {
		if ((hal_space_t)space == handle)
			break;
	}

	/* A handle that names no live space is refused. */
	if (space == NULL || space->destroying) {
		ppc32_irq_restore(enabled);
		return 0;
	}

	/* Succeeded: the caller holds the space until it turns interrupts back on. */
	*result = space;
	*irq_enabled = enabled;
	return 1;
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
	if ((address & (PPC32_PAGE_SIZE - 1U)) != 0 || (size & (PPC32_PAGE_SIZE - 1U)) != 0)
		return 0;

	/* The range must lie inside the user half. */
	if (address < PPC32_USER_MINIMUM || address >= PPC32_USER_LIMIT)
		return 0;
	if (size > PPC32_USER_LIMIT - address)
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
static uint32_t *
walk_leaf(
	struct ppc32_space *space,
	uintptr_t address,
	int create)
{
	struct ppc32_table_page *page;
	uint32_t *table;
	unsigned index;
	int error;

	/* The root entry: the leaf table's physical address, or zero. */
	index = (unsigned)(address >> 22);
	if (space->root[index] == 0) {
		if (!create)
			return NULL;
		page = kernel_alloc(sizeof(*page));
		if (page == NULL)
			return NULL;
		error = hal_pmem_alloc(PPC32_PAGE_SIZE, PPC32_PAGE_SIZE, &page->memory);
		if (error != HAL_OK) {
			kernel_free(page);
			return NULL;
		}

		/* Clears the table and links it into the space. */
		hal_memset(hal_pmem_to_kernel(page->memory), 0, PPC32_PAGE_SIZE);
		page->root_index = index;
		page->next = space->tables;
		space->tables = page;
		space->root[index] = (uint32_t)page->memory;
		page_table_count++;
	}

	/* Succeeded: the page's leaf. */
	table = hal_pmem_to_kernel(space->root[index]);
	return &table[(address >> PPC32_PAGE_SHIFT) & 0x3ffU];
}

/* Reports the segment id of a user address in a space. */
static uint32_t
segment_vsid(
	const struct ppc32_space *space,
	uintptr_t address)
{
	/* The segment register's low 24 bits. */
	return space->segments[(address >> PPC32_SEGMENT_SHIFT) - PPC32_USER_SEGMENT_FIRST] & 0x00ffffffU;
}

/* Reports the group of eight entries a hash names. */
static uint32_t *
group_of(
	uint32_t hash)
{
	/* 64 bytes a group. */
	return &htab[(hash & (HTAB_GROUPS - 1U)) * HTAB_GROUP_ENTRIES * 2U];
}

/* Drops a user address's entry from the hashed table, wherever it is. */
static void
drop_entry(
	struct ppc32_space *space,
	uintptr_t address)
{
	uint32_t *group;
	uint32_t vsid;
	uint32_t hash;
	uint32_t word;
	unsigned function;
	unsigned slot;

	/* The entry's first word under each hash function. */
	vsid = segment_vsid(space, address);
	hash = (vsid & 0x7ffffU) ^ ((uint32_t)(address >> PPC32_PAGE_SHIFT) & 0xffffU);
	for (function = 0; function < 2U; function++) {
		word = PTE_VALID | (vsid << PTE_VSID_SHIFT) | (uint32_t)((address >> 22) & 0x3fU);
		if (function != 0) {
			word |= PTE_SECONDARY;
			hash = ~hash;
		}

		/* Clears the matching entry of the group the function reaches. */
		group = group_of(hash);
		for (slot = 0; slot < HTAB_GROUP_ENTRIES; slot++) {
			if (group[slot * 2U] == word)
				group[slot * 2U] = 0;
		}
	}

	/* The processor's cached translation goes too. */
	ppc32_tlbie(address);
}

/*
 * Puts a user address's entry into the hashed table: a free slot of
 * either group, or a victim of the first.
 */
static void
put_entry(
	struct ppc32_space *space,
	uintptr_t address,
	uint32_t leaf,
	int writable)
{
	uint32_t *group;
	uint32_t *chosen;
	uint32_t vsid;
	uint32_t hash;
	uint32_t word0;
	uint32_t word1;
	uintptr_t victim;
	unsigned function;
	unsigned slot;

	/* The entry's words. */
	vsid = segment_vsid(space, address);
	hash = (vsid & 0x7ffffU) ^ ((uint32_t)(address >> PPC32_PAGE_SHIFT) & 0xffffU);
	word1 = (leaf & LEAF_PAGE_MASK) | PTE_COHERENT | PTE_READ_ONLY;
	if (writable)
		word1 = (leaf & LEAF_PAGE_MASK) | PTE_COHERENT | PTE_READ_WRITE;

	/* A free slot under either hash function. */
	chosen = NULL;
	word0 = 0;
	for (function = 0; function < 2U && chosen == NULL; function++) {
		group = group_of(function == 0 ? hash : ~hash);
		for (slot = 0; slot < HTAB_GROUP_ENTRIES; slot++) {
			if ((group[slot * 2U] & PTE_VALID) == 0) {
				chosen = &group[slot * 2U];
				word0 = PTE_VALID | (vsid << PTE_VSID_SHIFT) | (uint32_t)((address >> 22) & 0x3fU);
				if (function != 0)
					word0 |= PTE_SECONDARY;
				break;
			}
		}
	}

	/* Otherwise a victim of the first group, whose translation is dropped. */
	if (chosen == NULL) {
		group = group_of(hash);
		chosen = &group[(evict_next++ % HTAB_GROUP_ENTRIES) * 2U];
		victim = victim_address(chosen[0], hash & (HTAB_GROUPS - 1U));
		chosen[0] = 0;
		ppc32_tlbie(victim);
		word0 = PTE_VALID | (vsid << PTE_VSID_SHIFT) | (uint32_t)((address >> 22) & 0x3fU);
	}

	/* The second word first, then the valid first word. */
	chosen[1] = word1;
	__asm__ volatile("eieio" : : : "memory");
	chosen[0] = word0;
	__asm__ volatile("sync" : : : "memory");
}

/*
 * Reports the user address an entry of a group translates, from its
 * segment id (whose low bits are the segment), its hash function and the
 * top bits of its page.
 */
static uintptr_t
victim_address(
	uint32_t word0,
	uint32_t group_index)
{
	uint32_t vsid;
	uint32_t hash;
	uint32_t page;

	/* The hash the group was reached by, then the page's low ten bits from it. */
	vsid = (word0 >> PTE_VSID_SHIFT) & 0x00ffffffU;
	hash = group_index;
	if ((word0 & PTE_SECONDARY) != 0)
		hash = ~group_index & (HTAB_GROUPS - 1U);
	page = ((word0 & 0x3fU) << 10) | ((hash ^ vsid) & 0x3ffU);

	/* Succeeded: the segment, then the page. */
	return ((uintptr_t)(vsid & 0xfU) << PPC32_SEGMENT_SHIFT) | ((uintptr_t)page << PPC32_PAGE_SHIFT);
}

/* Frees a space's leaf tables. */
static void
free_tables(
	struct ppc32_space *space)
{
	struct ppc32_table_page *page;

	/* Each page and its record. */
	while (space->tables != NULL) {
		page = space->tables;
		space->tables = page->next;
		(void)hal_pmem_free(&page->memory, PPC32_PAGE_SIZE);
		kernel_free(page);
		page_table_count--;
	}
}

/* Tells whether a physical range lies inside one device window. */
static int
in_device_window(
	hal_physaddr_t paddr,
	size_t size)
{
	unsigned index;
	uint64_t end;

	/* The range's end, which must not wrap. */
	end = (uint64_t)paddr + size;
	if (end < paddr)
		return 0;

	/* Looks for the window that holds both ends. */
	for (index = 0; index < device_window_count; index++) {
		if (paddr < device_window[index])
			continue;
		if (end > (uint64_t)device_window[index] + DEVICE_WINDOW_BYTES)
			continue;
		return 1;
	}

	/* No window holds the range. */
	return 0;
}

/*
 * Writes one block translation pair: the upper word (the effective
 * address, the length, supervisor only) and the lower (the physical
 * address, the storage attributes, the access), in the data pair and,
 * for RAM, in the instruction pair too.
 */
static void
set_bat(
	uint32_t *bats,
	unsigned pair,
	uint32_t address,
	uint32_t size,
	uint32_t lower,
	bool instructions)
{
	uint32_t upper;
	unsigned base;

	/* The words: the low four pairs first, then the high four, instruction pairs before data pairs. */
	upper = (address & BAT_ADDRESS_MASK) |
		(((size >> BAT_LENGTH_SHIFT) - 1U) << BAT_LENGTH_FIELD_SHIFT) |
		BAT_SUPERVISOR;
	base = (pair / PPC32_BAT_PAIRS) * PPC32_BAT_PAIRS * 4U + (pair % PPC32_BAT_PAIRS) * 2U;
	bats[base + PPC32_BAT_PAIRS * 2U] = upper;
	bats[base + PPC32_BAT_PAIRS * 2U + 1U] = (address & BAT_ADDRESS_MASK) | lower;
	if (instructions) {
		bats[base] = upper;
		bats[base + 1U] = (address & BAT_ADDRESS_MASK) | lower;
	}
}
