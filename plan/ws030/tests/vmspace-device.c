/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Exercises actual VM device ownership through a stateful translation peer.
 * Non-device backing paths deliberately abort instead of silently succeeding.
 */

#define _POSIX_C_SOURCE 200809L
#include <kern/vmspace.h>
#include <kern/vm-device.h>
#include "libc/include/fcntl.h"
#include <kern/file.h>
#include <kern/kmem.h>
#include <kern/page.h>
#include <kern/vm-lock.h>
#include <kern/vm-object.h>
#include <kern/vm-reclaim.h>
#include <kern/vm-commit.h>
#include <kern/device-io.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_SIZE KERN_PAGE_SIZE
#define TEST_ADDRESS 0x400000U
#define TEST_PHYSICAL 0x100000000ULL
#define TEST_PAGES 4U
#define TEST_TRANSLATIONS 32U

/* Each active peer entry stands for a completed user page-table mapping. */
struct test_translation {
	hal_space_t space;
	uintptr_t address;
	hal_physaddr_t physical;
	uint32_t attributes;
};

/* A file owner survives descriptor close until every mapping and pin retires. */
struct test_owner {
	struct file file;
	unsigned references;
	unsigned releases;
	unsigned closes;
	void *storage;
};

/* The kernel sentinel must remain distinct from every fixture-created space. */
struct vmspace kernel_vmspace;

/* Fixed host geometry replaces architecture discovery for every test space. */
struct vm_layout vm_layout;

/* Translation state belongs to the single-threaded peer and empties at teardown. */
static struct test_translation translations[TEST_TRANSLATIONS];

/* Lock depths expose forbidden finalizers and fault operations under VM locks. */
static unsigned metadata_depth;
static unsigned mutex_depth;

/* Allocation accounting and one-shot failures expose incomplete rollbacks. */
static unsigned allocations;
static unsigned fail_allocation;

/* Each HAL operation counts separately so failures can target a partial update. */
static unsigned map_calls;
static unsigned protect_calls;
static unsigned unmap_calls;
static unsigned fail_map;
static unsigned fail_protect;
static unsigned fail_unmap;

/* MMIO counters distinguish device accessors from direct RAM memcpy. */
static unsigned mmio_reads;
static unsigned mmio_writes;
static unsigned barriers;

/* Production optional checkpoints remain absent; finalizers check lock ordering. */
extern void vmspace_unmap_retire_checkpoint(struct vmspace *vm, uintptr_t start, size_t size) __attribute__((weak));
extern void vmspace_pin_page_checkpoint(struct vmspace *vm, size_t index, size_t page_count) __attribute__((weak));

#include "vmspace-device-prototypes.h"

static void vmspace_wait_fault_event(struct vmspace *vm, uint64_t sequence);
static struct test_translation *translation_find(hal_space_t space, uintptr_t address);
static void owner_release(void *argument);
static struct vm_device_mapping *owner_create(struct test_owner *owner, uint32_t attributes, uint32_t permissions);
static void destroy_space(struct vmspace *vm);
static void test_mapping_lifetime(uint32_t attributes);
static void test_failures(void);
static void assert_translation(struct vmspace *vm, uintptr_t address, size_t offset, uint32_t rights, uint32_t attributes);
static void assert_no_translations(struct vmspace *vm);
static void reject_non_device(void);

/*
 * Runs independent MMIO and RAM lifetime scenarios and partial HAL failures.
 */
int
main(void)
{
	/* Exercises both storage kinds against the same VM state machine. */
	test_mapping_lifetime(VM_DEVICE_MMIO);
	test_mapping_lifetime(0U);
	test_failures();

	/* Every descriptor, mapping, page record and pin must have retired. */
	assert(allocations == 0U);
	assert(metadata_depth == 0U);
	assert(mutex_depth == 0U);
	puts("VM device map/fault/protect/split/fork/pin/close: PASS");

	/* Succeeded: every observed lifetime and rollback invariant held. */
	return 0;
}

/*
 * Allocates tracked metadata with an optional one-shot failure.
 */
void *
kern_calloc(
	size_t count,
	size_t size)
{
	void *allocation;

	/* A countdown targets a particular allocation inside a transaction. */
	if (fail_allocation != 0U) {
		fail_allocation--;

		/* The selected allocation fails without acquiring an owner. */
		if (fail_allocation == 0U)
			return NULL;
	}

	/* The host allocator supplies zeroed metadata just as the kernel does. */
	allocation = calloc(count, size);
	if (allocation == NULL)
		return NULL;

	/* Each allocation remains counted until its actual cleanup calls free. */
	allocations++;

	/* Succeeded: the caller owns this metadata. */
	return allocation;
}

/*
 * Releases exactly one tracked metadata allocation.
 */
void
kern_free(
	void *allocation)
{
	/* Null cleanup does not own a metadata record. */
	if (allocation == NULL)
		return;

	/* An underflow exposes a duplicate release. */
	assert(allocations != 0U);
	allocations--;
	free(allocation);

	/* Succeeded: the metadata allocation is gone. */
	return;
}

/*
 * Retains a fixture file through its enclosing owner record.
 */
void
file_ref(
	struct file *file)
{
	struct test_owner *owner;

	/* The file is the first field, and its owner outlives all retained references. */
	owner = (struct test_owner *)file;
	assert(owner->references != 0U);
	owner->references++;

	/* Succeeded: the open session has another lifetime owner. */
	return;
}

/*
 * Observes final descriptor closure after the resource release callback.
 */
int
file_close(
	struct file *file)
{
	struct test_owner *owner;

	/* Final session teardown must run outside either VM lock. */
	owner = (struct test_owner *)file;
	assert(metadata_depth == 0U);
	assert(mutex_depth == 0U);
	assert(owner->references != 0U);
	owner->references--;

	/* The resource callback must precede the session's final close. */
	if (owner->references == 0U) {
		assert(owner->releases == 1U);
		owner->closes++;
	}

	/* Succeeded: this file reference has retired. */
	return 0;
}

/*
 * Records the outer VM metadata serializer in this single-threaded peer.
 */
void
vm_metadata_enter(void)
{
	/* Metadata acquisition precedes the per-space mutex. */
	assert(mutex_depth == 0U);
	metadata_depth++;

	/* Succeeded: metadata is protected until the matching leave. */
	return;
}

/*
 * Closes the outer VM metadata critical section.
 */
void
vm_metadata_leave(void)
{
	/* Releasing metadata while a space mutex remains held violates ordering. */
	assert(mutex_depth == 0U);
	assert(metadata_depth != 0U);
	metadata_depth--;

	/* Succeeded: this metadata hold has retired. */
	return;
}

/*
 * Records each space mutex acquisition under the outer serializer.
 */
void
mutex_lock(
	struct mutex *mutex)
{
	/* The fixture does not model recursive or concurrent space locks. */
	assert(metadata_depth != 0U);
	assert(mutex->locked == 0U);
	mutex->locked = 1U;
	mutex_depth++;

	/* Succeeded: the space is exclusively owned. */
	return;
}

/*
 * Retires one per-space mutex hold.
 */
void
mutex_unlock(
	struct mutex *mutex)
{
	/* Both counters must still describe the acquired space lock. */
	assert(mutex->locked == 1U);
	assert(mutex_depth != 0U);
	mutex->locked = 0U;
	mutex_depth--;

	/* Succeeded: the space lock is released. */
	return;
}

/*
 * Models the short guard used by actual fault wakeup functions.
 */
unsigned long
spin_lock_irqsave(
	struct spinlock *lock)
{
	/* No second guard owner may overlap this single-threaded critical section. */
	assert(lock->owner_valid == 0U);
	lock->owner_valid = 1U;

	/* Succeeded: the fixture's prior interrupt state is zero. */
	return 0UL;
}

/*
 * Ends the short fault-notification guard.
 */
void
spin_unlock_irqrestore(
	struct spinlock *lock,
	unsigned long enabled)
{
	/* The fixture has no independent interrupt state to restore. */
	(void)enabled;
	assert(lock->owner_valid == 1U);
	lock->owner_valid = 0U;

	/* Succeeded: another guard owner may proceed. */
	return;
}

/*
 * Advances the fault notification generation without a scheduler.
 */
void
waitq_wake_all(
	struct wait_queue *queue)
{
	/* Completion observers see a different generation after each wake. */
	queue->sequence++;

	/* Succeeded: the peer records this wakeup. */
	return;
}

/*
 * Returns the generation used to avoid missing a fault completion.
 */
uint64_t
waitq_sequence(
	const struct wait_queue *queue)
{
	/* Succeeded: reports the peer's latest completed notification. */
	return queue->sequence;
}

/*
 * Supplies stable host test user-address geometry.
 */
void
vmspace_layout_init(void)
{
	/* Every test maps far from both ends of this intentionally finite range. */
	vm_layout.user_minimum = PAGE_SIZE;
	vm_layout.user_limit = 0x80000000U;
	vm_layout.mmap_base = TEST_ADDRESS;

	/* Succeeded: all production range checks now share one geometry. */
	return;
}

/*
 * Constructs only the address-space ownership shell for actual fork logic.
 */
struct vmspace *
vmspace_create(void)
{
	struct vmspace *vm;

	/* Region construction remains the actual production map_region function. */
	vm = kern_calloc(1U, sizeof(*vm));
	if (vm == NULL)
		return NULL;

	/* A distinct identity separates parent and child hardware translations. */
	vm->space = vm;
	vm->address_limit = UINT64_MAX;
	vm->data_limit = UINT64_MAX;
	vm->stack_limit = UINT64_MAX;

	/* Succeeded: the new shell owns no regions or pages yet. */
	return vm;
}

/*
 * Allocates page metadata while leaving all device-fault behavior production-owned.
 */
struct vm_page *
vm_page_alloc_metadata(void)
{
	struct vm_page *page;

	/* Host heap allocation replaces only the kernel metadata slab mechanism. */
	page = kern_calloc(1U, sizeof(*page));
	if (page == NULL)
		return NULL;

	/* Succeeded: the caller owns a zeroed page record. */
	return page;
}

/*
 * Releases page metadata after actual unmap and retire logic detaches it.
 */
void
vm_page_free_metadata(
	struct vm_page *page)
{
	/* Device metadata must never retain an unrelated RAM backing. */
	assert(page->private_page == NULL);
	assert(page->object_page == NULL);
	kern_free(page);

	/* Succeeded: this page record has retired. */
	return;
}

/*
 * Prepares only the generic fault reservation before calling the actual device path.
 */
int
vmspace_fault(
	struct vmspace *vm,
	uintptr_t address,
	uint32_t required)
{
	struct vm_region *region;
	struct vm_page *page;
	int error;

	/* Faulting from pin acquisition must occur outside the VM serializers. */
	assert(metadata_depth == 0U);
	assert(mutex_depth == 0U);
	region = find_region_locked(vm, address, 1U);
	if (region == NULL)
		return EFAULT;

	/* The omitted general dispatcher must reject absent access before reserving. */
	if ((region->prot & required) != required)
		return EFAULT;

	/* Existing failed-fault metadata can be retried without another record. */
	page = find_page(region, address);
	if (page == NULL) {
		page = vm_page_alloc_metadata();
		if (page == NULL)
			return ENOMEM;

		/* The region owns the metadata before the HAL transaction starts. */
		page->vm = vm;
		page->region = region;
		page->address = address & ~(uintptr_t)(PAGE_SIZE - 1U);
		page->next = region->pages;
		region->pages = page;
	}

	/* A previously completed device fault needs no second HAL mapping. */
	if ((page->flags & VM_MAPPING_MAPPED) != 0U)
		return 0;

	/* BUSY and the region hold exclude unmap until production completion clears both. */
	page->flags |= VM_MAPPING_BUSY;
	region->hold_count++;
	error = vmspace_device_fault(vm, region, page);
	if (error != 0)
		return error;

	/* Succeeded: the actual device-fault path published its translation. */
	return 0;
}

/*
 * Maps one peer page and optionally fails after earlier pages already committed.
 */
int
hal_space_map(
	hal_space_t space,
	void *address,
	hal_physaddr_t physical,
	size_t bytes,
	uint32_t attributes)
{
	struct test_translation *entry;
	struct vmspace *vm;
	struct vm_region *region;
	struct vm_page *page;
	unsigned index;

	/* Fault completion calls HAL without VM locks while its reservation excludes mutation. */
	vm = space;
	region = find_region_locked(vm, (uintptr_t)address, 1U);
	assert(region != NULL);
	page = find_page(region, (uintptr_t)address);
	assert(page != NULL);

	/* A reserved fault must remain protected by BUSY until its HAL mapping completes. */
	if (region->hold_count != 0U) {
		assert(metadata_depth == 0U);
		assert(mutex_depth == 0U);
		assert((page->flags & VM_MAPPING_BUSY) != 0U);
	} else {
		assert(metadata_depth != 0U);
		assert(mutex_depth != 0U);
	}

	/* Page-sized updates must retain an explicit accessible user permission. */
	assert(bytes == PAGE_SIZE);
	assert((attributes & (HAL_SPACE_READ | HAL_SPACE_WRITE)) != 0U);
	map_calls++;

	/* Failure injection must not mutate this page's prior hardware state. */
	if (map_calls == fail_map)
		return HAL_ERR_NOMEM;

	/* A duplicate mapping would hide incorrect MAP_NONE restoration. */
	entry = translation_find(space, (uintptr_t)address);
	assert(entry == NULL);

	/* Reserve one empty peer entry before publishing the completed mapping. */
	for (index = 0U; index < TEST_TRANSLATIONS; index++) {
		if (translations[index].space == NULL) {
			entry = &translations[index];
			break;
		}
	}

	/* The fixture bounds exceed every scenario's live page count. */
	assert(entry != NULL);
	entry->space = space;
	entry->address = (uintptr_t)address;
	entry->physical = physical;
	entry->attributes = attributes;

	/* Succeeded: this page now has the requested physical and cache identity. */
	return HAL_OK;
}

/*
 * Changes only the attributes of an existing peer translation.
 */
int
hal_space_prot(
	hal_space_t space,
	void *address,
	size_t bytes,
	uint32_t attributes)
{
	struct test_translation *entry;

	/* Protection changes require an existing translation and both VM serializers. */
	assert(metadata_depth != 0U);
	assert(mutex_depth != 0U);
	assert(bytes == PAGE_SIZE);
	protect_calls++;
	if (protect_calls == fail_protect)
		return HAL_ERR_NOMEM;

	/* The peer preserves physical ownership while publishing changed attributes. */
	entry = translation_find(space, (uintptr_t)address);
	assert(entry != NULL);
	entry->attributes = attributes;

	/* Succeeded: the peer now exposes the new access and cache flags. */
	return HAL_OK;
}

/*
 * Removes one complete peer translation with optional partial-transaction failure.
 */
int
hal_space_unmap(
	hal_space_t space,
	void *address,
	size_t bytes)
{
	struct test_translation *entry;

	/* A failed shootdown leaves the previous translation intact under both VM locks. */
	assert(metadata_depth != 0U);
	assert(mutex_depth != 0U);
	assert(bytes == PAGE_SIZE);
	unmap_calls++;
	if (unmap_calls == fail_unmap)
		return HAL_ERR_NOMEM;

	/* Empty entries cannot be unmapped twice. */
	entry = translation_find(space, (uintptr_t)address);
	assert(entry != NULL);
	memset(entry, 0, sizeof(*entry));

	/* Succeeded: this user address no longer reaches its former backing. */
	return HAL_OK;
}

/*
 * Stops on an invariant violation with the actual production call site.
 */
void
hal_fatal(
	const char *file,
	int line,
	const char *message)
{
	/* Fatal kernel conditions fail this host process immediately. */
	fprintf(stderr, "%s:%d: %s\n", file, line, message);
	abort();

	/* Succeeded: unreachable after the fatal process termination. */
	return;
}

/*
 * Preserves the device byte-read boundary while recording its use.
 */
uint8_t
kern_mmio_read8(
	const volatile void *address)
{
	/* Every byte must pass through this accessor for MMIO storage. */
	mmio_reads++;

	/* Succeeded: reads the retained device alias. */
	return *(const volatile uint8_t *)address;
}

/*
 * Preserves the device byte-write boundary while recording its use.
 */
void
kern_mmio_write8(
	volatile void *address,
	uint8_t byte)
{
	/* Every device store remains distinct from an ordinary RAM memcpy. */
	mmio_writes++;
	*(volatile uint8_t *)address = byte;

	/* Succeeded: the device alias contains this byte. */
	return;
}

/*
 * Records the read visibility boundary used by device copies.
 */
void
kern_io_read_barrier(void)
{
	/* The test observes ordering points around every MMIO copy. */
	barriers++;

	/* Succeeded: the peer observed a read barrier. */
	return;
}

/*
 * Records the write visibility boundary used by device copies.
 */
void
kern_io_write_barrier(void)
{
	/* The test observes ordering points around every MMIO copy. */
	barriers++;

	/* Succeeded: the peer observed a write barrier. */
	return;
}

/* Public non-device collaborators fail if a device path accidentally enters them. */
void
file_exec_snapshot_ref(
	struct file_exec_snapshot *snapshot)
{
	/* Device mappings have no executable file snapshot. */
	(void)snapshot;
	reject_non_device();

	/* Succeeded: unreachable for this device-only fixture. */
	return;
}

/*
 * Rejects unexpected executable snapshot retirement.
 */
void
file_exec_snapshot_put(
	struct file_exec_snapshot *snapshot)
{
	/* No tested mapping owns an executable snapshot. */
	(void)snapshot;
	reject_non_device();

	/* Succeeded: unreachable for this device-only fixture. */
	return;
}

/*
 * Rejects unexpected shared-file backing retention.
 */
void
vm_object_ref(
	struct vm_object *object)
{
	/* All tested regions own device storage instead of a file cache object. */
	(void)object;
	reject_non_device();

	/* Succeeded: unreachable for this device-only fixture. */
	return;
}

/*
 * Rejects unexpected shared-file backing retirement.
 */
void
vm_object_put(
	struct vm_object *object)
{
	/* Device retirement never invokes the shared-file cache. */
	(void)object;
	reject_non_device();

	/* Succeeded: unreachable for this device-only fixture. */
	return;
}

/*
 * Rejects anonymous commitment charges for device-owned storage.
 */
int
vm_commit_reserve(
	size_t bytes)
{
	/* The driver already owns every physical byte in this fixture. */
	(void)bytes;
	reject_non_device();

	/* Succeeded: unreachable for this device-only fixture. */
	return 0;
}

/*
 * Rejects anonymous commitment retirement for device-owned storage.
 */
void
vm_commit_release(
	size_t bytes)
{
	/* Device mappings must never acquire a corresponding commit charge. */
	(void)bytes;
	reject_non_device();

	/* Succeeded: unreachable for this device-only fixture. */
	return;
}

/*
 * Rejects private RAM retention on a device mapping.
 */
void
vm_private_page_ref(
	struct vm_private_page *backing)
{
	/* Device aliases never share a private COW page. */
	(void)backing;
	reject_non_device();

	/* Succeeded: unreachable for this device-only fixture. */
	return;
}

/*
 * Rejects private RAM retirement on a device mapping.
 */
void
vm_private_page_put(
	struct vm_private_page *backing)
{
	/* Device aliases retire their own vm_device_mapping reference. */
	(void)backing;
	reject_non_device();

	/* Succeeded: unreachable for this device-only fixture. */
	return;
}

/*
 * Rejects private COW serialization on a device mapping.
 */
void
vm_private_page_operation_end(
	struct vm_private_page *backing)
{
	/* Device fork aliases remain shared without private-page operations. */
	(void)backing;
	reject_non_device();

	/* Succeeded: unreachable for this device-only fixture. */
	return;
}

/*
 * Rejects private backing waits on a device mapping.
 */
int
vm_private_page_wait_idle(
	struct vm_private_page *backing)
{
	/* No private-page operation can block this device-only fixture. */
	(void)backing;
	reject_non_device();

	/* Succeeded: unreachable for this device-only fixture. */
	return 0;
}

/*
 * Rejects private-page pinning on a device mapping.
 */
int
vm_private_page_pin(
	struct vm_private_page *backing,
	struct kern_pmem *memory)
{
	/* Device pins must retain vm_device_mapping instead of RAM metadata. */
	(void)backing;
	(void)memory;
	reject_non_device();

	/* Succeeded: unreachable for this device-only fixture. */
	return 0;
}

/*
 * Rejects private-page unpinning on a device mapping.
 */
void
vm_private_page_unpin(
	struct vm_private_page *backing)
{
	/* Device pin release must use its matching mapping owner. */
	(void)backing;
	reject_non_device();

	/* Succeeded: unreachable for this device-only fixture. */
	return;
}

/*
 * Rejects an accidental private resident-page query.
 */
int
vm_private_page_is_resident(
	const struct vm_page *page)
{
	/* A device region's explicit branch must bypass this RAM-only predicate. */
	(void)page;
	reject_non_device();

	/* Succeeded: unreachable for this device-only fixture. */
	return 0;
}

/*
 * Rejects COW sharing of a device mapping during fork.
 */
int
vm_page_share_private(
	struct vm_page *source,
	struct vm_page *copy)
{
	/* Fork must retain the original device backing and leave the child lazy. */
	(void)source;
	(void)copy;
	reject_non_device();

	/* Succeeded: unreachable for this device-only fixture. */
	return 0;
}

/*
 * Rejects private reverse-map detachment for device storage.
 */
void
vm_page_untrack(
	struct vm_page *page)
{
	/* Device mappings own neither reclaim tracking nor COW reverse maps. */
	(void)page;
	reject_non_device();

	/* Succeeded: unreachable for this device-only fixture. */
	return;
}

/*
 * Rejects shared-file reverse-map detachment for device storage.
 */
void
vm_object_mapping_remove(
	struct vm_object_page *page,
	struct vm_page *mapping)
{
	/* Device pages never join a file-cache reverse mapping list. */
	(void)page;
	(void)mapping;
	reject_non_device();

	/* Succeeded: unreachable for this device-only fixture. */
	return;
}

/*
 * Rejects shared-file pinning for device storage.
 */
int
vm_object_page_pin(
	struct vm_object_page *page)
{
	/* A device pin must bypass file-cache lifetime accounting. */
	(void)page;
	reject_non_device();

	/* Succeeded: unreachable for this device-only fixture. */
	return 0;
}

/*
 * Rejects shared-file unpinning for device storage.
 */
void
vm_object_page_unpin(
	struct vm_object_page *page)
{
	/* A device pin retires only its own immutable backing record. */
	(void)page;
	reject_non_device();

	/* Succeeded: unreachable for this device-only fixture. */
	return;
}

/* Actual functions are copied unchanged by the test-only runner. */
#include "vmspace-device-functions.h"

/* Rejects a scheduler wait which this single-threaded scenario cannot satisfy. */
static void
vmspace_wait_fault_event(
	struct vmspace *vm,
	uint64_t sequence)
{
	/* The fixture completes every reserved fault synchronously before mutation. */
	(void)vm;
	(void)sequence;
	abort();

	/* Succeeded: unreachable without an incorrectly retained BUSY reservation. */
	return;
}

/* Finds a peer translation by both address-space identity and virtual page. */
static struct test_translation *
translation_find(
	hal_space_t space,
	uintptr_t address)
{
	unsigned index;

	/* Parent and forked child may map the same address independently. */
	for (index = 0U; index < TEST_TRANSLATIONS; index++) {
		if (translations[index].space != space)
			continue;

		/* Only the exact virtual page can describe this request. */
		if (translations[index].address == address)
			return &translations[index];
	}

	/* The peer has no completed mapping at this address. */
	return NULL;
}

/* Frees storage only after the final region or transient pin relinquishes it. */
static void
owner_release(
	void *argument)
{
	struct test_owner *owner;

	/* The session is still alive and both VM locks are absent during teardown. */
	owner = argument;
	assert(metadata_depth == 0U);
	assert(mutex_depth == 0U);
	assert(owner->references != 0U);
	assert(owner->releases == 0U);
	owner->releases++;
	free(owner->storage);
	owner->storage = NULL;

	/* Succeeded: the driver's storage hold has retired exactly once. */
	return;
}

/* Creates a real device mapping over an aligned peer-owned storage extent. */
static struct vm_device_mapping *
owner_create(
	struct test_owner *owner,
	uint32_t attributes,
	uint32_t permissions)
{
	struct vm_device_mapping *mapping;
	int error;

	/* One initial file reference models the process descriptor. */
	memset(owner, 0, sizeof(*owner));
	owner->references = 1U;
	error = posix_memalign(&owner->storage, PAGE_SIZE, TEST_PAGES * PAGE_SIZE);
	assert(error == 0);
	memset(owner->storage, 0, TEST_PAGES * PAGE_SIZE);

	/* The real mapping constructor acquires its callback and file lifetime. */
	mapping = NULL;
	error = vm_device_create(
		&owner->file,
		TEST_PHYSICAL,
		owner->storage,
		TEST_PAGES * PAGE_SIZE,
		attributes,
		permissions,
		owner_release,
		owner,
		&mapping);
	assert(error == 0);
	assert(mapping != NULL);
	assert(owner->references == 2U);

	/* Succeeded: the caller owns the mapping's initial reference. */
	return mapping;
}

/* Unmaps every remaining region through actual detach and lockless retirement. */
static void
destroy_space(
	struct vmspace *vm)
{
	int error;

	/* The public wrapper unpublishes regions before retiring them outside both locks. */
	while (vm->regions != NULL) {
		error = vmspace_unmap(vm, vm->regions->start, vm->regions->size);
		assert(error == 0);
	}

	/* An empty address space must not leave any peer translations or charge. */
	assert_no_translations(vm);
	assert(vm->mapped_virtual_bytes == 0U);
	kern_free(vm);

	/* Succeeded: the complete address-space shell has retired. */
	return;
}

/* Checks page identity and cache policy independently of VM metadata flags. */
static void
assert_translation(
	struct vmspace *vm,
	uintptr_t address,
	size_t offset,
	uint32_t rights,
	uint32_t attributes)
{
	struct test_translation *entry;
	uint32_t expected;

	/* The physical offset must remain stable across split, protection and fork. */
	entry = translation_find(vm->space, address);
	assert(entry != NULL);
	assert(entry->physical == TEST_PHYSICAL + offset);
	expected = rights;

	/* Device aliases retain uncached MMIO semantics in every reconstructed PTE. */
	if (attributes == VM_DEVICE_MMIO)
		expected |= HAL_SPACE_DEVICE | HAL_SPACE_NOCACHE;

	/* The peer must contain no permission or cache bits beyond those promised. */
	assert(entry->attributes == expected);

	/* Succeeded: the hardware peer matches the independent expected identity. */
	return;
}

/* Confirms that a space no longer has any hardware peer mapping. */
static void
assert_no_translations(
	struct vmspace *vm)
{
	unsigned index;

	/* Freed virtual ranges cannot remain accessible through stale PTEs. */
	for (index = 0U; index < TEST_TRANSLATIONS; index++)
		assert(translations[index].space != vm->space);

	/* Succeeded: every peer mapping of this space has been removed. */
	return;
}

/* Exercises retained storage through lazy mapping, split aliases, fork and pins. */
static void
test_mapping_lifetime(
	uint32_t attributes)
{
	struct test_owner owner;
	struct vm_device_mapping *mapping;
	struct vmspace *vm;
	struct vmspace *child;
	struct vmspace *failed;
	struct vm_private_page *wait_backing;
	struct vmspace_pinned_page pins[2];
	struct vm_region *region;
	struct test_translation *entry;
	uintptr_t address;
	uint32_t permissions;
	uint8_t input[3];
	uint8_t output[3];
	unsigned reads;
	unsigned writes;
	unsigned old_barriers;
	unsigned index;
	int error;

	/* A descriptor and the constructor initially retain the driver's storage. */
	permissions = HAL_SPACE_READ | HAL_SPACE_WRITE;
	mapping = owner_create(&owner, attributes, permissions);
	vm = vmspace_create();
	assert(vm != NULL);

	/* Publication begins inaccessible and without anonymous commitment or PTEs. */
	error = vmspace_map_device(vm, TEST_ADDRESS, TEST_PAGES * PAGE_SIZE, 0U, mapping, 1, &address);
	assert(error == 0);
	assert(address == TEST_ADDRESS);
	assert(vm->regions->commit_size == 0U);
	assert_no_translations(vm);
	vm_device_put(mapping);
	error = file_close(&owner.file);
	assert(error == 0);
	assert(owner.references == 1U);
	assert(owner.releases == 0U);

	/* NONE-to-RW changes rights while absent pages remain lazy. */
	error = vmspace_protect(vm, address, TEST_PAGES * PAGE_SIZE, permissions);
	assert(error == 0);
	assert_no_translations(vm);

	/* Actual pin acquisition faults pages and retains independent mapping references. */
	error = vmspace_pin_user_pages(
		vm,
		address + PAGE_SIZE + 7U,
		PAGE_SIZE,
		permissions,
		pins,
		2U);
	assert(error == 0);
	assert(pins[0].kind == VMSPACE_PINNED_DEVICE);
	assert(pins[0].device_offset == PAGE_SIZE);
	assert(pins[1].device_offset == PAGE_SIZE * 2U);
	assert_translation(vm, address + PAGE_SIZE, PAGE_SIZE, permissions, attributes);
	assert_translation(vm, address + PAGE_SIZE * 2U, PAGE_SIZE * 2U, permissions, attributes);

	/* A protected middle page splits the same backing into independently offset regions. */
	error = vmspace_protect(vm, address + PAGE_SIZE, PAGE_SIZE, HAL_SPACE_READ);
	assert(error == 0);
	region = vm->regions->next;
	assert(region->start == address + PAGE_SIZE);
	assert(region->device_offset == PAGE_SIZE);
	assert(region->next->device_offset == PAGE_SIZE * 2U);
	assert_translation(vm, address + PAGE_SIZE, PAGE_SIZE, HAL_SPACE_READ, attributes);

	/* Restoring access after NONE must reconstruct the split page at its original offset. */
	error = vmspace_protect(vm, address + PAGE_SIZE, PAGE_SIZE, 0U);
	assert(error == 0);
	entry = translation_find(vm->space, address + PAGE_SIZE);
	assert(entry == NULL);
	error = vmspace_protect(vm, address + PAGE_SIZE, PAGE_SIZE, permissions);
	assert(error == 0);
	assert_translation(vm, address + PAGE_SIZE, PAGE_SIZE, permissions, attributes);

	/* Fork retains the same split backing and leaves every child PTE lazy. */
	child = NULL;
	failed = NULL;
	wait_backing = NULL;
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	error = vmspace_fork_locked(vm, &child, &wait_backing, &failed);
	assert(error == 0);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	assert(child != NULL);
	assert(failed == NULL);
	assert(wait_backing == NULL);
	assert_no_translations(child);
	assert(child->regions->next->device_offset == PAGE_SIZE);
	assert(child->regions->next->pages == NULL);
	error = vmspace_fault(child, address + PAGE_SIZE, HAL_SPACE_WRITE);
	assert(error == 0);
	assert_translation(child, address + PAGE_SIZE, PAGE_SIZE, permissions, attributes);

	/* Unmapping a pinned parent range removes access without invalidating the saved pin. */
	error = vmspace_unmap(vm, address + PAGE_SIZE, PAGE_SIZE);
	assert(error == 0);
	entry = translation_find(vm->space, address + PAGE_SIZE);
	assert(entry == NULL);
	assert_translation(child, address + PAGE_SIZE, PAGE_SIZE, permissions, attributes);
	destroy_space(vm);
	destroy_space(child);
	assert(owner.releases == 0U);
	assert(owner.closes == 0U);

	/* A pin continues to access the original alias after all descriptors and regions vanish. */
	input[0] = 0x73U;
	input[1] = 0x19U;
	input[2] = 0xc4U;
	reads = mmio_reads;
	writes = mmio_writes;
	old_barriers = barriers;
	error = vm_device_write(pins[0].owner.device, pins[0].device_offset + 9U, input, sizeof(input));
	assert(error == 0);
	error = vm_device_read(pins[0].owner.device, pins[0].device_offset + 9U, output, sizeof(output));
	assert(error == 0);

	/* Byte identity must survive independently of the access mechanism. */
	for (index = 0U; index < sizeof(input); index++)
		assert(output[index] == input[index]);

	/* MMIO copies must use accessors and barriers, while RAM copies use their cached alias. */
	if (attributes == VM_DEVICE_MMIO) {
		assert(mmio_reads == reads + sizeof(input));
		assert(mmio_writes == writes + sizeof(input));
		assert(barriers == old_barriers + 4U);
	} else {
		assert(mmio_reads == reads);
		assert(mmio_writes == writes);
		assert(barriers == old_barriers);
	}

	/* The final pin retires the resource before closing its retained session exactly once. */
	vmspace_unpin_user_pages(pins, 2U);
	assert(pins[0].kind == VMSPACE_PINNED_NONE);
	assert(pins[1].kind == VMSPACE_PINNED_NONE);
	assert(owner.releases == 1U);
	assert(owner.closes == 1U);
	assert(owner.references == 0U);

	/* Succeeded: no page, region, descriptor or transient pin leaked its ownership. */
	return;
}

/* Exercises failed publication and protection changes after a partial HAL commit. */
static void
test_failures(void)
{
	struct test_owner owner;
	struct vm_device_mapping *mapping;
	struct vmspace *vm;
	struct vmspace *child;
	struct vmspace *failed;
	struct vm_private_page *wait_backing;
	struct vm_page *page;
	struct vm_region *region;
	uintptr_t address;
	uint32_t permissions;
	unsigned references;
	unsigned index;
	int error;

	/* The initial backing remains caller-owned through every rejected publication. */
	permissions = HAL_SPACE_READ | HAL_SPACE_WRITE;
	mapping = owner_create(&owner, VM_DEVICE_MMIO, permissions);
	vm = vmspace_create();
	assert(vm != NULL);
	error = vmspace_map_device(vm, TEST_ADDRESS, PAGE_SIZE, HAL_SPACE_EXEC, mapping, 1, &address);
	assert(error == EACCES);
	assert(vm->regions == NULL);
	error = vmspace_map_device(vm, TEST_ADDRESS, SIZE_MAX, permissions, mapping, 1, &address);
	assert(error == EINVAL);
	fail_allocation = 1U;
	error = vmspace_map_device(vm, TEST_ADDRESS, TEST_PAGES * PAGE_SIZE, permissions, mapping, 1, &address);
	assert(error == ENOMEM);
	assert(mapping->references.value == 1U);
	assert(vm->mapped_virtual_bytes == 0U);

	/* A successful retry publishes one region and rejects an overlapping exact request. */
	error = vmspace_map_device(vm, TEST_ADDRESS, TEST_PAGES * PAGE_SIZE, permissions, mapping, 1, &address);
	assert(error == 0);
	error = vmspace_map_device(vm, TEST_ADDRESS, PAGE_SIZE, permissions, mapping, 1, &address);
	assert(error == EEXIST);
	assert(mapping->references.value == 2U);

	/* Later protection changes cannot exceed the driver's original authority. */
	error = vmspace_protect(vm, TEST_ADDRESS, TEST_PAGES * PAGE_SIZE, HAL_SPACE_EXEC);
	assert(error == EACCES);
	assert(vm->regions->prot == permissions);

	/* Failed HAL faults must release BUSY and region holds without inventing a PTE. */
	fail_map = map_calls + 1U;
	error = vmspace_fault(vm, TEST_ADDRESS, HAL_SPACE_WRITE);
	assert(error == ENOMEM);
	page = vm->regions->pages;
	assert(page->flags == 0U);
	assert(vm->regions->hold_count == 0U);
	assert_no_translations(vm);

	/* The same harmless page record can be faulted successfully on retry. */
	for (index = 0U; index < TEST_PAGES; index++) {
		error = vmspace_fault(vm, TEST_ADDRESS + index * PAGE_SIZE, HAL_SPACE_WRITE);
		assert(error == 0);
	}

	/* A partial protection downgrade must restore every earlier page's rights and MMIO attributes. */
	fail_protect = protect_calls + 2U;
	error = vmspace_protect(vm, TEST_ADDRESS, TEST_PAGES * PAGE_SIZE, HAL_SPACE_READ);
	assert(error == EINVAL);
	assert(vm->regions->prot == permissions);

	/* Each expected translation is independent of the VM traversal order. */
	for (index = 0U; index < TEST_PAGES; index++) {
		assert_translation(
			vm,
			TEST_ADDRESS + index * PAGE_SIZE,
			index * PAGE_SIZE,
			permissions,
			VM_DEVICE_MMIO);
	}

	/* A partial NONE transition must remap earlier pages with the exact old MMIO policy. */
	fail_unmap = unmap_calls + 2U;
	error = vmspace_protect(vm, TEST_ADDRESS, TEST_PAGES * PAGE_SIZE, 0U);
	assert(error == EINVAL);

	/* Rollback preserves both physical ownership and access for every page. */
	for (index = 0U; index < TEST_PAGES; index++) {
		assert_translation(
			vm,
			TEST_ADDRESS + index * PAGE_SIZE,
			index * PAGE_SIZE,
			permissions,
			VM_DEVICE_MMIO);
	}

	/* A successful NONE transition leaves metadata which a later RW failure must not partly expose. */
	error = vmspace_protect(vm, TEST_ADDRESS, TEST_PAGES * PAGE_SIZE, 0U);
	assert(error == 0);
	assert_no_translations(vm);
	fail_map = map_calls + 2U;
	error = vmspace_protect(vm, TEST_ADDRESS, TEST_PAGES * PAGE_SIZE, permissions);
	assert(error == EINVAL);
	assert(vm->regions->prot == 0U);
	assert_no_translations(vm);

	/* Successful recovery must also clear every transaction-only metadata flag. */
	error = vmspace_protect(vm, TEST_ADDRESS, TEST_PAGES * PAGE_SIZE, permissions);
	assert(error == 0);
	for (page = vm->regions->pages; page != NULL; page = page->next) {
		assert((page->flags & (VM_MAPPING_PROTECT_ADDED | VM_MAPPING_PROTECT_REMOVED)) == 0U);
	}

	/* A split allocation failure consumes neither a backing reference nor a region boundary. */
	references = mapping->references.value;
	fail_allocation = 1U;
	error = vmspace_protect(vm, TEST_ADDRESS + PAGE_SIZE, PAGE_SIZE, HAL_SPACE_READ);
	assert(error == ENOMEM);
	assert(mapping->references.value == references);
	assert(vm->regions->size == TEST_PAGES * PAGE_SIZE);
	assert(vm->regions->next == NULL);

	/* Create multiple aliases, then fail fork after the first child alias has retained storage. */
	error = vmspace_protect(vm, TEST_ADDRESS + PAGE_SIZE, PAGE_SIZE, HAL_SPACE_READ);
	assert(error == 0);
	references = mapping->references.value;
	child = NULL;
	failed = NULL;
	wait_backing = NULL;
	fail_allocation = 3U;
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	error = vmspace_fork_locked(vm, &child, &wait_backing, &failed);
	assert(error == ENOMEM);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	assert(child == NULL);
	assert(failed != NULL);
	assert(mapping->references.value == references + 1U);
	destroy_space(failed);
	assert(mapping->references.value == references);

	/* The failed child must not alter source region permissions or retained offsets. */
	region = vm->regions->next;
	assert(region->prot == HAL_SPACE_READ);
	assert(region->device_offset == PAGE_SIZE);
	assert(region->next->device_offset == PAGE_SIZE * 2U);
	destroy_space(vm);
	vm_device_put(mapping);
	error = file_close(&owner.file);
	assert(error == 0);
	assert(owner.releases == 1U);
	assert(owner.closes == 1U);

	/* Succeeded: all injected failures preserved the caller's remaining owners. */
	return;
}

/* Terminates any accidental dependency on private RAM or shared-file machinery. */
static void
reject_non_device(void)
{
	/* A device path must not acquire, free or charge ordinary page ownership. */
	fputs("unexpected non-device VM collaborator\n", stderr);
	abort();

	/* Succeeded: unreachable after the failed invariant. */
	return;
}
