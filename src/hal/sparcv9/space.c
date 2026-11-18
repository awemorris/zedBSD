/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The SPARC V9 HAL's address spaces and its software-filled TLB.
 *
 * The kernel half is the same in every space: the direct map of physical
 * memory, which the fast miss handlers fill in 4 MiB pages, and the window
 * of device mappings.  A user space is a list of 8 KiB mappings tagged
 * with the space's context.  Switching spaces drops every TLB entry that
 * belongs to a user space, so a context is never matched after its space
 * is gone, and the next access misses and is resolved from the new space.
 *
 * The port runs on one processor; interrupts are off while a space or the
 * device window is changed, which keeps the lists consistent with the TLB.
 */

#include <hal/hal.h>
#include "asi.h"
#include "defs.h"
#include "page.h"
#include "space.h"
#include "tte.h"

/* The contexts a user space may take; zero is the nucleus, the kernel's. */
#define SPACE_CONTEXT_FIRST	1U
#define SPACE_CONTEXT_LIMIT	8192U

/* The direct-map pages locked into the data TLB at boot: 8 MiB to 32 MiB. */
#define SPACE_LOCKED_FIRST	0x00800000UL
#define SPACE_LOCKED_END	0x02000000UL
#define SPACE_LARGE_PAGE	0x00400000UL

/* The most device mappings the kernel window holds at once. */
#define SPACE_DEVICE_WINDOWS	16U

_Static_assert(SPARCV9_USER_LIMIT - 1U <= (uintptr_t)INTPTR_MAX,
	       "user pointers must not overlap the negative syscall errno window");

/*
 * One device range mapped into the kernel window.
 *
 * Made by hal_space_map_device() and removed by hal_space_unmap_device();
 * a zero size is a free slot.
 */
struct device_window {
	uintptr_t virtual_address;
	uintptr_t physical_address;
	size_t size;
	uint32_t attributes;
};

/*
 * The user space the processor runs in, or NULL when only the kernel half
 * is selected.  Changed by hal_space_switch() with interrupts off.
 */
static struct sparcv9_space *current_space;

/*
 * Every live user space, newest first.  A handle is accepted only when it
 * is on this list, so a destroyed space is refused rather than touched.
 */
static struct sparcv9_space *space_registry;

/*
 * The context the next space takes, and how many spaces and mapping
 * records are alive, for the memory statistics.  Changed with interrupts
 * off.
 */
static uint32_t next_context;
static uint32_t space_count;
static uint32_t mapping_count;

/*
 * The device mappings of the kernel window, and the address the next one
 * starts at.  Addresses are not reused, so a stale TLB entry of a removed
 * mapping can never be matched by a later one.
 */
static struct device_window device_windows[SPACE_DEVICE_WINDOWS];
static uintptr_t device_next_address;

static int space_acquire(hal_space_t handle, struct sparcv9_space **space, bool *enabled);
static void space_release(struct sparcv9_space *space, bool enabled);
static int range_is_user(uintptr_t address, size_t size);
static int attributes_are_valid(uint32_t attributes);
static struct sparcv9_mapping *find_mapping(struct sparcv9_space *space, uintptr_t address);
static struct sparcv9_mapping *detach_mapping(struct sparcv9_space *space, uintptr_t address);
static void free_mappings(struct sparcv9_mapping *list);
static void flush_user_entries(void);
static void flush_device_entries(void);
static void flush_if_current(struct sparcv9_space *space);
static void lock_direct_window(void);
static int write_entry(int instruction, uint64_t tag, uint64_t data);
static int resolve_user_locked(struct sparcv9_space *space, uint64_t tag, int instruction, int write);
static int resolve_device(uint64_t tag, int instruction);

/*
 * Starts the MMU with no user space selected.
 *
 * The kernel runs in the nucleus context; the low part of the direct map
 * that the trap paths use is locked in, so a trap never misses on it.
 */
void
sparcv9_space_init(
	void)
{
	/* Starts with no user space and the first context free. */
	current_space = NULL;
	space_registry = NULL;
	next_context = SPACE_CONTEXT_FIRST;
	space_count = 0;
	mapping_count = 0;
	device_next_address = SPARCV9_DEVICE_VA;
	hal_memset(device_windows, 0, sizeof(device_windows));

	/* Selects the nucleus context, drops the firmware's user entries and locks the window. */
	sparcv9_set_primary_context(0);
	flush_user_entries();
	lock_direct_window();
}

/*
 * Creates a user space containing the shared kernel half.
 */
hal_space_t
hal_space_create(
	void)
{
	struct sparcv9_space *space;
	bool enabled;

	/* Allocates the space's record. */
	space = kernel_alloc(sizeof(*space));
	if (space == NULL)
		return NULL;

	/* Starts the space with no mapping. */
	hal_memset(space, 0, sizeof(*space));
	space->magic = SPARCV9_SPACE_MAGIC;

	/*
	 * Gives the space the next context and publishes it.  Contexts are
	 * reused after they wrap; a reused one is safe because every user
	 * entry is dropped on each switch.
	 */
	enabled = hal_irq_disable();

	space->context = next_context;
	next_context++;
	if (next_context >= SPACE_CONTEXT_LIMIT)
		next_context = SPACE_CONTEXT_FIRST;
	space->registry_next = space_registry;
	space_registry = space;
	space_count++;

	if (enabled)
		hal_irq_enable();

	/* Succeeded: the new space. */
	return space;
}

/*
 * Destroys a user space that no processor selects any more.
 */
void
hal_space_destroy(
	hal_space_t handle)
{
	struct sparcv9_space *space;
	struct sparcv9_space **link;
	struct sparcv9_mapping *mappings;
	bool enabled;

	/* Destroying nothing does nothing. */
	space = handle;
	if (space == NULL)
		return;

	/* Takes the space off the registry, refusing one that is not on it or in use. */
	enabled = hal_irq_disable();

	link = &space_registry;
	while (*link != NULL && *link != space)
		link = &(*link)->registry_next;
	if (*link == NULL || space->destroying)
		HAL_FATAL("invalid SPARC V9 space");
	if (current_space == space)
		HAL_FATAL("destroying an active SPARC V9 space");
	if (space->busy)
		HAL_FATAL("destroying a busy SPARC V9 space");
	space->destroying = 1U;
	*link = space->registry_next;
	mappings = space->mappings;
	space->mappings = NULL;
	space->magic = 0;
	if (space_count != 0)
		space_count--;

	if (enabled)
		hal_irq_enable();

	/* Frees the mappings and the record, which nothing reaches any more. */
	free_mappings(mappings);
	kernel_free(space);
}

/*
 * Selects a user space on the processor, or only the kernel half.
 */
void
hal_space_switch(
	hal_space_t handle)
{
	struct sparcv9_space *space;
	bool enabled;
	int acquired;

	/* Selecting the space already selected changes nothing. */
	if (handle == (hal_space_t)current_space)
		return;

	/* Selects only the kernel half: no user entry may stay behind. */
	if (handle == HAL_SPACE_SYS) {
		enabled = hal_irq_disable();

		flush_user_entries();
		current_space = NULL;
		sparcv9_set_primary_context(0);

		if (enabled)
			hal_irq_enable();
		return;
	}

	/* Refuses a handle that is not a live space. */
	acquired = space_acquire(handle, &space, &enabled);
	if (!acquired)
		HAL_FATAL("invalid SPARC V9 space switch");

	/* Drops the old space's entries and selects the new context. */
	flush_user_entries();
	current_space = space;
	sparcv9_set_primary_context(space->context);

	space_release(space, enabled);
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
	struct sparcv9_space *space;
	struct sparcv9_mapping *mapping;
	struct sparcv9_mapping *added;
	struct sparcv9_mapping *next;
	uintptr_t address;
	uintptr_t offset;
	size_t total;
	bool enabled;
	int user_range;
	int valid;
	int acquired;

	/* Refuses a range that is not whole user pages. */
	address = (uintptr_t)vaddr;
	user_range = range_is_user(address, size);
	if (handle == NULL || !user_range)
		return HAL_ERR_INVALID;

	/* Refuses device pages: this port maps only RAM into user spaces. */
	if ((attr & HAL_SPACE_DEVICE) != 0)
		return HAL_ERR_UNSUPPORTED;

	/* Refuses a physical range that is not whole pages of managed RAM. */
	total = hal_pmem_get_total_size();
	if ((paddr & SPARCV9_PAGE_MASK) != 0)
		return HAL_ERR_INVALID;
	if (paddr >= total || size > total - paddr)
		return HAL_ERR_INVALID;

	/* Refuses attributes that grant nothing, or writable code. */
	valid = attributes_are_valid(attr);
	if (!valid)
		return HAL_ERR_INVALID;

	/*
	 * Builds the new mappings before touching the space, so a failed
	 * allocation leaves it as it was.
	 */
	added = NULL;
	for (offset = 0; offset < size; offset += SPARCV9_PAGE_SIZE) {
		/* Allocates one page's record. */
		mapping = kernel_alloc(sizeof(*mapping));
		if (mapping == NULL) {
			free_mappings(added);
			return HAL_ERR_NOMEM;
		}

		/* Describes the page as present but not yet accessed. */
		mapping->virtual_address = address + offset;
		mapping->physical_address = paddr + offset;
		mapping->attributes = attr;
		mapping->flags = HAL_SPACE_PAGE_PRESENT;
		mapping->next = added;
		added = mapping;
	}

	/* Refuses a stale handle. */
	acquired = space_acquire(handle, &space, &enabled);
	if (!acquired) {
		free_mappings(added);
		return HAL_ERR_STATE;
	}

	/* Refuses a range that overlaps a page already mapped. */
	for (offset = 0; offset < size; offset += SPARCV9_PAGE_SIZE) {
		mapping = find_mapping(space, address + offset);
		if (mapping != NULL) {
			space_release(space, enabled);
			free_mappings(added);
			return HAL_ERR_INVALID;
		}
	}

	/* Links the new pages into the space. */
	while (added != NULL) {
		next = added->next;
		added->next = space->mappings;
		space->mappings = added;
		mapping_count++;
		added = next;
	}
	flush_if_current(space);

	space_release(space, enabled);

	/* Succeeded: the pages are reachable at the next access. */
	return HAL_OK;
}

/*
 * Removes the mappings of a user range.
 */
int
hal_space_unmap(
	hal_space_t handle,
	void *vaddr,
	size_t size)
{
	struct sparcv9_space *space;
	struct sparcv9_mapping *removed;
	struct sparcv9_mapping *mapping;
	uintptr_t address;
	uintptr_t offset;
	bool enabled;
	int user_range;
	int acquired;

	/* Unmapping nothing succeeds. */
	if (size == 0)
		return HAL_OK;

	/* Refuses a range that is not whole user pages. */
	address = (uintptr_t)vaddr;
	user_range = range_is_user(address, size);
	if (handle == NULL || !user_range)
		return HAL_ERR_INVALID;

	/* Refuses a stale handle. */
	acquired = space_acquire(handle, &space, &enabled);
	if (!acquired)
		return HAL_ERR_STATE;

	/* Detaches every mapped page of the range and drops its TLB entries. */
	removed = NULL;
	for (offset = 0; offset < size; offset += SPARCV9_PAGE_SIZE) {
		mapping = detach_mapping(space, address + offset);
		if (mapping == NULL)
			continue;
		mapping->next = removed;
		removed = mapping;
	}
	flush_if_current(space);

	space_release(space, enabled);

	/* Succeeded: frees the records the TLB no longer refers to. */
	free_mappings(removed);
	return HAL_OK;
}

/*
 * Maps a device physical range into the kernel's device window.
 */
int
hal_space_map_device(
	hal_physaddr_t paddr,
	size_t size,
	uint32_t attr,
	void **vaddr)
{
	struct device_window *window;
	uintptr_t first_page;
	uintptr_t offset;
	size_t span;
	bool enabled;
	unsigned index;

	/* Refuses a missing destination, an empty range, or executable device memory. */
	if (vaddr == NULL || size == 0)
		return HAL_ERR_INVALID;
	if ((attr & HAL_SPACE_EXEC) != 0)
		return HAL_ERR_INVALID;

	/* Covers the range with whole pages. */
	first_page = paddr & ~(uintptr_t)SPARCV9_PAGE_MASK;
	offset = paddr - first_page;
	if (size > SIZE_MAX - offset - SPARCV9_PAGE_MASK)
		return HAL_ERR_INVALID;
	span = (offset + size + SPARCV9_PAGE_MASK) & ~(size_t)SPARCV9_PAGE_MASK;

	/* Takes a free slot and the next part of the window. */
	enabled = hal_irq_disable();

	window = NULL;
	for (index = 0; index < SPACE_DEVICE_WINDOWS; index++) {
		if (device_windows[index].size == 0) {
			window = &device_windows[index];
			break;
		}
	}

	/* Refuses when every slot or the whole window is taken. */
	if (window == NULL || span > SPARCV9_DEVICE_VA_END - device_next_address) {
		if (enabled)
			hal_irq_enable();
		return HAL_ERR_NOMEM;
	}

	window->virtual_address = device_next_address;
	window->physical_address = first_page;
	window->size = span;
	window->attributes = attr;
	device_next_address += span;

	if (enabled)
		hal_irq_enable();

	/* Succeeded: the address of the first byte asked for. */
	*vaddr = (void *)(window->virtual_address + offset);
	return HAL_OK;
}

/*
 * Removes a mapping made by hal_space_map_device().
 */
int
hal_space_unmap_device(
	void *vaddr,
	size_t size)
{
	struct device_window *window;
	uintptr_t address;
	bool enabled;
	unsigned index;

	/* Finds the window the address lies in. */
	address = (uintptr_t)vaddr;
	enabled = hal_irq_disable();

	window = NULL;
	for (index = 0; index < SPACE_DEVICE_WINDOWS; index++) {
		if (device_windows[index].size == 0)
			continue;
		if (address < device_windows[index].virtual_address)
			continue;
		if (address - device_windows[index].virtual_address >= device_windows[index].size)
			continue;
		window = &device_windows[index];
		break;
	}

	/* Refuses an address no device mapping holds, or a size past its end. */
	if (window == NULL || size > window->size) {
		if (enabled)
			hal_irq_enable();
		return HAL_ERR_INVALID;
	}

	/* Frees the slot and drops the window's TLB entries. */
	hal_memset(window, 0, sizeof(*window));
	flush_device_entries();

	if (enabled)
		hal_irq_enable();

	/* Succeeded: the range is no longer reachable. */
	return HAL_OK;
}

/*
 * Changes the protection of a user range and reports the access and dirty
 * state its pages had.
 */
int
hal_space_prot_query(
	hal_space_t handle,
	void *vaddr,
	size_t size,
	uint32_t attr,
	uint32_t *flags)
{
	struct sparcv9_space *space;
	struct sparcv9_mapping *mapping;
	uintptr_t address;
	uintptr_t offset;
	uint32_t observed;
	bool enabled;
	int user_range;
	int valid;
	int acquired;

	/* Refuses a range that is not whole user pages, or invalid attributes. */
	address = (uintptr_t)vaddr;
	user_range = range_is_user(address, size);
	if (handle == NULL || !user_range)
		return HAL_ERR_INVALID;
	valid = attributes_are_valid(attr);
	if (!valid)
		return HAL_ERR_INVALID;

	/* Refuses a stale handle. */
	acquired = space_acquire(handle, &space, &enabled);
	if (!acquired)
		return HAL_ERR_STATE;

	/* Refuses a range with a page that is not mapped, before changing any. */
	for (offset = 0; offset < size; offset += SPARCV9_PAGE_SIZE) {
		mapping = find_mapping(space, address + offset);
		if (mapping == NULL) {
			space_release(space, enabled);
			return HAL_ERR_INVALID;
		}
	}

	/* Collects the pages' state and gives them the new protection. */
	observed = 0;
	for (offset = 0; offset < size; offset += SPARCV9_PAGE_SIZE) {
		mapping = find_mapping(space, address + offset);
		observed |= mapping->flags;
		mapping->attributes = attr;
	}

	/* Drops the old entries, so the next access takes the new protection. */
	flush_if_current(space);

	space_release(space, enabled);

	/* Succeeded: reports the state the old entries left. */
	if (flags != NULL)
		*flags = observed;
	return HAL_OK;
}

/*
 * Changes the protection of a user range.
 */
int
hal_space_prot(
	hal_space_t handle,
	void *vaddr,
	size_t size,
	uint32_t attr)
{
	int error;

	/* Changes the protection without asking for the pages' state. */
	error = hal_space_prot_query(handle, vaddr, size, attr, NULL);
	if (error != HAL_OK)
		return error;

	/* Succeeded: the new protection is in effect. */
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
	struct sparcv9_space *space;
	struct sparcv9_mapping *mapping;
	bool enabled;
	int user_range;
	int acquired;

	/* Refuses a missing destination or an address that is not a user page. */
	user_range = range_is_user((uintptr_t)vaddr, SPARCV9_PAGE_SIZE);
	if (handle == NULL || flags == NULL || !user_range)
		return HAL_ERR_INVALID;

	/* Refuses a stale handle. */
	acquired = space_acquire(handle, &space, &enabled);
	if (!acquired)
		return HAL_ERR_STATE;

	/* Reports the page's state; an unmapped page has none. */
	mapping = find_mapping(space, (uintptr_t)vaddr);
	*flags = 0;
	if (mapping != NULL)
		*flags = mapping->flags;

	space_release(space, enabled);

	/* Succeeded. */
	return HAL_OK;
}

/*
 * Clears the accessed or dirty state of one user page.
 */
int
hal_space_clear_flags(
	hal_space_t handle,
	void *vaddr,
	uint32_t flags)
{
	struct sparcv9_space *space;
	struct sparcv9_mapping *mapping;
	bool enabled;
	int user_range;
	int acquired;

	/* Refuses an address that is not a user page, or a state that cannot be cleared. */
	user_range = range_is_user((uintptr_t)vaddr, SPARCV9_PAGE_SIZE);
	if (handle == NULL || !user_range)
		return HAL_ERR_INVALID;
	if ((flags & ~(HAL_SPACE_PAGE_ACCESSED | HAL_SPACE_PAGE_DIRTY)) != 0)
		return HAL_ERR_INVALID;

	/* Refuses a stale handle. */
	acquired = space_acquire(handle, &space, &enabled);
	if (!acquired)
		return HAL_ERR_STATE;

	/* Refuses a page that is not mapped. */
	mapping = find_mapping(space, (uintptr_t)vaddr);
	if (mapping == NULL) {
		space_release(space, enabled);
		return HAL_ERR_INVALID;
	}

	/*
	 * Clears the state and drops the page's entry, so the next access
	 * misses and records the state again.
	 */
	mapping->flags &= ~flags;
	flush_if_current(space);

	space_release(space, enabled);

	/* Succeeded. */
	return HAL_OK;
}

/*
 * Drops the TLB entries of a space, or of the kernel's device window.
 */
void
hal_space_flush_tlb(
	hal_space_t handle)
{
	struct sparcv9_space *space;
	bool enabled;
	int acquired;

	/* The kernel half: only the device window's entries can change. */
	if (handle == HAL_SPACE_SYS) {
		enabled = hal_irq_disable();

		flush_device_entries();

		if (enabled)
			hal_irq_enable();
		return;
	}

	/* Refuses a handle that is not a live space. */
	acquired = space_acquire(handle, &space, &enabled);
	if (!acquired)
		HAL_FATAL("invalid SPARC V9 space flush");

	/* Drops the user entries when the space is the one selected. */
	flush_if_current(space);

	space_release(space, enabled);
}

/*
 * Drops the TLB entries of a range; the whole space is dropped.
 */
void
hal_space_flush_tlb_range(
	hal_space_t handle,
	void *vaddr,
	size_t size)
{
	UNUSED_PARAMETER(vaddr);

	/* An empty range drops nothing. */
	if (size == 0)
		return;

	/* The TLB holds few entries, so dropping them all costs little. */
	hal_space_flush_tlb(handle);
}

/*
 * Reports the page size of a level; only the base page is used.
 */
size_t
hal_space_get_page_size(
	int level)
{
	/* The base page is the only size the HAL maps for the kernel's use. */
	if (level == 1)
		return SPARCV9_PAGE_SIZE;

	/* No larger page is offered. */
	return 0;
}

/*
 * Reports the range of user addresses.
 */
void
hal_space_get_user_range(
	uintptr_t *minimum,
	uintptr_t *limit)
{
	/* The first page stays unmapped so a null pointer faults. */
	if (minimum != NULL)
		*minimum = SPARCV9_PAGE_SIZE;

	/* The user half ends where the hole of the 44-bit address space begins. */
	if (limit != NULL)
		*limit = SPARCV9_USER_LIMIT;
}

/*
 * Resolves a TLB miss from the current user space or the device window.
 *
 * tag is the miss's tag access value: the page's address and the context
 * the access was made in.
 */
int
sparcv9_resolve_miss(
	uintptr_t tag,
	int instruction,
	int write)
{
	struct sparcv9_space *space;
	bool enabled;
	int acquired;
	int resolved;

	/* A kernel address is a device mapping or nothing. */
	if (tag >= SPARCV9_DEVICE_VA) {
		resolved = resolve_device(tag, instruction);
		return resolved;
	}

	/* A user address needs a selected space. */
	space = current_space;
	if (space == NULL)
		return 0;
	acquired = space_acquire(space, &space, &enabled);
	if (!acquired)
		return 0;

	/* Writes the entry from the space's mapping, if it allows the access. */
	resolved = 0;
	if (space == current_space)
		resolved = resolve_user_locked(space, tag, instruction, write);

	space_release(space, enabled);

	/* Reports whether the entry was written. */
	return resolved;
}

/*
 * Writes the TLB entry of a user address before the first user access,
 * as a miss would.
 */
int
sparcv9_prime_mapping(
	uintptr_t address,
	int instruction,
	int write)
{
	struct sparcv9_space *space;
	uint64_t tag;
	bool enabled;
	int acquired;
	int resolved;

	/* Needs a selected space. */
	space = current_space;
	if (space == NULL)
		return 0;
	acquired = space_acquire(space, &space, &enabled);
	if (!acquired)
		return 0;

	/* Tags the page with the space's context, as a user access would. */
	resolved = 0;
	tag = (address & ~(uintptr_t)SPARCV9_PAGE_MASK) | (uint64_t)space->context;
	if (space == current_space)
		resolved = resolve_user_locked(space, tag, instruction, write);

	space_release(space, enabled);

	/* Reports whether the entry was written. */
	return resolved;
}

/*
 * Reports the number of address spaces and of mapping records.
 */
void
sparcv9_space_memory_stats(
	uint32_t *spaces,
	uint32_t *mappings)
{
	bool enabled;

	/* Samples both counters together. */
	enabled = hal_irq_disable();

	if (spaces != NULL)
		*spaces = space_count;
	if (mappings != NULL)
		*mappings = mapping_count;

	if (enabled)
		hal_irq_enable();
}

/*
 * Finds a live space by its handle and marks it busy, with interrupts off;
 * returns 1, or 0 for a handle that is not a live space.
 */
static int
space_acquire(
	hal_space_t handle,
	struct sparcv9_space **space,
	bool *enabled)
{
	struct sparcv9_space *candidate;
	bool was_enabled;

	/* Looks the handle up in the registry with interrupts off. */
	was_enabled = hal_irq_disable();
	candidate = space_registry;
	while (candidate != NULL && (hal_space_t)candidate != handle)
		candidate = candidate->registry_next;

	/* Refuses a handle that is gone or going. */
	if (candidate == NULL || candidate->destroying) {
		if (was_enabled)
			hal_irq_enable();
		return 0;
	}

	/* An operation never nests inside another on the same space. */
	if (candidate->busy)
		HAL_FATAL("recursive SPARC V9 space operation");

	/* Succeeded: the space is the caller's until space_release(). */
	candidate->busy = 1U;
	*space = candidate;
	*enabled = was_enabled;
	return 1;
}

/* Ends an operation on a space and restores the interrupts. */
static void
space_release(
	struct sparcv9_space *space,
	bool enabled)
{
	/* Only a busy space can be released. */
	if (space->busy != 1U)
		HAL_FATAL("invalid SPARC V9 space release");

	/* The space is free for the next operation. */
	space->busy = 0;
	if (enabled)
		hal_irq_enable();
}

/* Reports whether a range is whole pages of the user half, past page zero. */
static int
range_is_user(
	uintptr_t address,
	size_t size)
{
	/* Refuses an empty or unaligned range. */
	if (size == 0)
		return 0;
	if ((address & SPARCV9_PAGE_MASK) != 0 || (size & SPARCV9_PAGE_MASK) != 0)
		return 0;

	/* Refuses page zero and anything outside the user half. */
	if (address < SPARCV9_PAGE_SIZE || address >= SPARCV9_USER_LIMIT)
		return 0;
	if (size > SPARCV9_USER_LIMIT - address)
		return 0;

	/* The range is user pages. */
	return 1;
}

/* Reports whether attributes grant some access and keep code unwritable. */
static int
attributes_are_valid(
	uint32_t attributes)
{
	/* Refuses attributes that grant no access at all. */
	if ((attributes & (HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC)) == 0)
		return 0;

	/* Refuses a page that is both writable and executable. */
	if ((attributes & HAL_SPACE_WRITE) != 0 && (attributes & HAL_SPACE_EXEC) != 0)
		return 0;

	/* The attributes are usable. */
	return 1;
}

/* Finds the mapping of one page, or NULL. */
static struct sparcv9_mapping *
find_mapping(
	struct sparcv9_space *space,
	uintptr_t address)
{
	struct sparcv9_mapping *mapping;

	/* Walks the space's list. */
	for (mapping = space->mappings; mapping != NULL; mapping = mapping->next) {
		if (mapping->virtual_address == address)
			return mapping;
	}

	/* The page is not mapped. */
	return NULL;
}

/* Takes the mapping of one page off its space's list, or returns NULL. */
static struct sparcv9_mapping *
detach_mapping(
	struct sparcv9_space *space,
	uintptr_t address)
{
	struct sparcv9_mapping **link;
	struct sparcv9_mapping *mapping;

	/* Finds the link that points at the page's mapping. */
	link = &space->mappings;
	while (*link != NULL && (*link)->virtual_address != address)
		link = &(*link)->next;

	/* The page is not mapped. */
	if (*link == NULL)
		return NULL;

	/* Succeeded: unlinks the mapping; the caller frees it. */
	mapping = *link;
	*link = mapping->next;
	mapping->next = NULL;
	if (mapping_count != 0)
		mapping_count--;
	return mapping;
}

/* Frees a list of mappings that no space holds any more. */
static void
free_mappings(
	struct sparcv9_mapping *list)
{
	struct sparcv9_mapping *next;

	/* Frees the records one after another. */
	while (list != NULL) {
		next = list->next;
		kernel_free(list);
		list = next;
	}
}

/*
 * Drops every TLB entry that is not locked and not part of the kernel's
 * direct map: the user entries, and the firmware's left from boot.
 */
static void
flush_user_entries(
	void)
{
	uint64_t data;
	uint64_t tag;
	unsigned entry;

	/* Walks every data and instruction TLB entry. */
	for (entry = 0; entry < SPARCV9_TLB_ENTRIES; entry++) {
		/* Drops a data entry unless it is locked or maps the direct map for the kernel. */
		data = sparcv9_dtlb_read_data(entry);
		if ((data & SPARCV9_TTE_VALID) != 0 && (data & SPARCV9_TTE_LOCKED) == 0) {
			tag = sparcv9_dtlb_read_tag(entry);
			if (tag < SPARCV9_DIRECT_BASE ||
			    tag >= SPARCV9_DIRECT_BASE + SPARCV9_DIRECT_LIMIT ||
			    (data & SPARCV9_TTE_GLOBAL) == 0)
				sparcv9_dtlb_write_entry(entry, 0, 0);
		}

		/* Drops an instruction entry unless it is locked; kernel text misses back in. */
		data = sparcv9_itlb_read_data(entry);
		if ((data & SPARCV9_TTE_VALID) != 0 && (data & SPARCV9_TTE_LOCKED) == 0)
			sparcv9_itlb_write_entry(entry, 0, 0);
	}
}

/* Drops every data TLB entry of the device window. */
static void
flush_device_entries(
	void)
{
	uint64_t data;
	uint64_t tag;
	unsigned entry;

	/* Walks every data TLB entry. */
	for (entry = 0; entry < SPARCV9_TLB_ENTRIES; entry++) {
		/* Skips an entry that is not in use. */
		data = sparcv9_dtlb_read_data(entry);
		if ((data & SPARCV9_TTE_VALID) == 0)
			continue;

		/* Drops an entry whose page lies in the device window. */
		tag = sparcv9_dtlb_read_tag(entry);
		if (tag >= SPARCV9_DEVICE_VA && tag < SPARCV9_DEVICE_VA_END)
			sparcv9_dtlb_write_entry(entry, 0, 0);
	}
}

/* Drops the user entries when a space is the one selected. */
static void
flush_if_current(
	struct sparcv9_space *space)
{
	/* A space not selected has no entry in the TLB. */
	if (space != current_space)
		return;

	/* Drops the user entries, so the next access misses into the new mappings. */
	flush_user_entries();
}

/*
 * Locks the direct map from 8 MiB to 32 MiB into the data TLB in 4 MiB
 * pages, in the entries the firmware left free.
 */
static void
lock_direct_window(
	void)
{
	uintptr_t physical;
	uint64_t tag;
	uint64_t data;
	uint64_t old;
	unsigned entry;

	/* Fills one free entry per 4 MiB page. */
	entry = 0;
	for (physical = SPACE_LOCKED_FIRST; physical < SPACE_LOCKED_END; physical += SPACE_LARGE_PAGE) {
		/* Skips the entries the firmware has locked. */
		while (entry < SPARCV9_TLB_ENTRIES) {
			old = sparcv9_dtlb_read_data(entry);
			if ((old & SPARCV9_TTE_LOCKED) == 0)
				break;
			entry++;
		}

		/* Stops when the TLB has no entry left to lock. */
		if (entry >= SPARCV9_TLB_ENTRIES)
			return;

		/* Writes a locked, cacheable, privileged and global entry for the page. */
		tag = SPARCV9_DIRECT_BASE + physical;
		data = sparcv9_tte(physical,
				   SPARCV9_TTE_SIZE_4M |
				   SPARCV9_TTE_CP |
				   SPARCV9_TTE_CV |
				   SPARCV9_TTE_LOCKED |
				   SPARCV9_TTE_PRIVILEGED |
				   SPARCV9_TTE_WRITE |
				   SPARCV9_TTE_GLOBAL);
		sparcv9_dtlb_write_entry(entry, tag, data);
		entry++;
	}
}

/*
 * Writes one TLB entry: over the entry already holding the same page and
 * context, so the TLB never holds two that match, or else in the first
 * unlocked slot from one picked by the page's address.  Returns 1, or 0
 * when every slot is locked.
 */
static int
write_entry(
	int instruction,
	uint64_t tag,
	uint64_t data)
{
	uint64_t old;
	uint64_t old_tag;
	unsigned entry;
	unsigned count;
	int found;

	/* Looks for a valid entry of the same page and context. */
	found = 0;
	for (entry = 0; entry < SPARCV9_TLB_ENTRIES; entry++) {
		/* Reads the slot and its tag from the TLB the access goes through. */
		if (instruction) {
			old = sparcv9_itlb_read_data(entry);
			old_tag = sparcv9_itlb_read_tag(entry);
		} else {
			old = sparcv9_dtlb_read_data(entry);
			old_tag = sparcv9_dtlb_read_tag(entry);
		}

		/* Takes a valid, unlocked slot of the same page and context. */
		if ((old & SPARCV9_TTE_VALID) != 0 &&
		    (old & SPARCV9_TTE_LOCKED) == 0 &&
		    old_tag == tag) {
			found = 1;
			break;
		}
	}

	/* Otherwise spreads entries over the TLB by the page's address. */
	if (!found) {
		entry = (unsigned)((tag >> 13) & (SPARCV9_TLB_ENTRIES - 1U));

		/* Looks for the first slot that is not locked. */
		for (count = 0; count < SPARCV9_TLB_ENTRIES; count++) {
			/* Reads the slot from the TLB the access goes through. */
			if (instruction) {
				old = sparcv9_itlb_read_data(entry);
			} else {
				old = sparcv9_dtlb_read_data(entry);
			}

			/* Stops at a slot that can be replaced. */
			if ((old & SPARCV9_TTE_LOCKED) == 0)
				break;
			entry = (entry + 1U) & (SPARCV9_TLB_ENTRIES - 1U);
		}

		/* Every slot is locked. */
		if (count == SPARCV9_TLB_ENTRIES)
			return 0;
	}

	/* Succeeded: writes the entry with its tag. */
	if (instruction) {
		sparcv9_itlb_write_entry(entry, tag, data);
	} else {
		sparcv9_dtlb_write_entry(entry, tag, data);
	}
	return 1;
}

/*
 * Writes the TLB entry of a user page from its mapping; the caller holds
 * the space.  Returns 1, or 0 when no mapping allows the access.
 */
static int
resolve_user_locked(
	struct sparcv9_space *space,
	uint64_t tag,
	int instruction,
	int write)
{
	struct sparcv9_mapping *mapping;
	uintptr_t page;
	uint64_t flags;
	uint64_t data;
	int written;

	/* Finds the page's mapping. */
	page = (uintptr_t)tag & ~(uintptr_t)SPARCV9_PAGE_MASK;
	mapping = find_mapping(space, page);
	if (mapping == NULL)
		return 0;

	/* Refuses an access the mapping does not grant. */
	if (instruction && (mapping->attributes & HAL_SPACE_EXEC) == 0)
		return 0;
	if (write && (mapping->attributes & HAL_SPACE_WRITE) == 0)
		return 0;
	if (!instruction && !write && (mapping->attributes & HAL_SPACE_READ) == 0)
		return 0;

	/* Builds a cacheable entry, writable when the mapping is. */
	flags = SPARCV9_TTE_CP | SPARCV9_TTE_CV;
	if ((mapping->attributes & HAL_SPACE_WRITE) != 0)
		flags |= SPARCV9_TTE_WRITE;
	data = sparcv9_tte(mapping->physical_address, flags);

	/*
	 * The TLB does not report stores back, so a writable page is taken as
	 * dirty once it is in the TLB.  Clearing the state drops the entry, so
	 * a later access brings it back and records it again.
	 */
	mapping->flags |= HAL_SPACE_PAGE_ACCESSED;
	if ((mapping->attributes & HAL_SPACE_WRITE) != 0)
		mapping->flags |= HAL_SPACE_PAGE_DIRTY;

	/* Writes the entry under the context of the access. */
	written = write_entry(instruction, tag, data);

	/* Reports whether the entry was written. */
	return written;
}

/*
 * Writes the data TLB entry of a device-window page; returns 1, or 0 when
 * no device mapping holds the address.
 */
static int
resolve_device(
	uint64_t tag,
	int instruction)
{
	struct device_window *window;
	uintptr_t page;
	uintptr_t physical;
	uint64_t flags;
	uint64_t data;
	unsigned index;
	int written;

	/* Device memory is never executed. */
	if (instruction)
		return 0;

	/* Finds the window the page lies in. */
	page = (uintptr_t)tag & ~(uintptr_t)SPARCV9_PAGE_MASK;
	window = NULL;
	for (index = 0; index < SPACE_DEVICE_WINDOWS; index++) {
		if (device_windows[index].size == 0)
			continue;
		if (page < device_windows[index].virtual_address)
			continue;
		if (page - device_windows[index].virtual_address >= device_windows[index].size)
			continue;
		window = &device_windows[index];
		break;
	}

	/* No device mapping holds the page. */
	if (window == NULL)
		return 0;

	/*
	 * Builds an uncached entry with side effects that inverts the byte
	 * order, so the device's little-endian registers read as numbers.
	 */
	physical = window->physical_address + (page - window->virtual_address);
	flags = SPARCV9_TTE_SIDE_EFFECT | SPARCV9_TTE_IE | SPARCV9_TTE_PRIVILEGED | SPARCV9_TTE_GLOBAL;
	if ((window->attributes & HAL_SPACE_WRITE) != 0)
		flags |= SPARCV9_TTE_WRITE;
	data = sparcv9_tte(physical, flags);

	/* Writes the entry; it matches in every context. */
	written = write_entry(0, page, data);

	/* Reports whether the entry was written. */
	return written;
}
