/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Mapping of anonymous, file, shared and stack regions, the lookup of
 * free ranges for them, and msync.
 */

#include "kern/vmspace.h"
#include "kern/vm-device.h"
#include "kern/file.h"
#include "kern/kmem.h"
#include "kern/lock.h"
#include "kern/page.h"
#include "kern/sched.h"
#include "kern/swap.h"
#include "kern/vm-commit.h"
#include "kern/vm-lock.h"
#include "kern/vm-object.h"
#include "kern/vm-reclaim.h"
#include <uapi/errno.h>
#include <uapi/mman.h>
#include <kern/kcrt.h>
#include "kern/clock.h"
#include "vmspace-internal.h"

static int prepare_region(uintptr_t start, size_t size, uint32_t prot, enum vm_region_backing backing, struct file *file, off_t file_offset, uintptr_t data_start, size_t data_size, unsigned flags, size_t commit_size, struct vm_region **result);
static void discard_prepared_region(struct vm_region *region);
static int vmspace_map_anon_fixed_noreplace_locked(struct vmspace *vm, uintptr_t start, size_t size, uint32_t prot, struct vm_region **result);
static int vmspace_map_stack_locked(struct vmspace *vm, uintptr_t top, size_t size, size_t guard_size);
static struct vm_object *private_file_object(struct file *file);

/*
 * Writes back the shared object pages of a user range.
 *
 * Exactly one of MS_ASYNC and MS_SYNC must be set; MS_INVALIDATE
 * additionally advances the generation so cached translations are
 * revalidated.
 */
int
vmspace_sync(
	struct vmspace *vm,
	uintptr_t start,
	size_t size,
	int flags)
{
	uintptr_t current;
	uintptr_t end;
	struct vm_object *object;
	struct vm_region *region;
	uintptr_t overlap_end;
	off_t offset;
	int error;

	/* Rejects a malformed range or an inconsistent flag combination. */
	if (vm == NULL ||
	    vm == &kernel_vmspace ||
	    !vmspace_range_valid(start, size) ||
	    (flags & ~(MS_ASYNC | MS_INVALIDATE | MS_SYNC)) != 0 ||
	    (flags & (MS_ASYNC | MS_SYNC)) == 0 ||
	    (flags & (MS_ASYNC | MS_SYNC)) == (MS_ASYNC | MS_SYNC))
		return EINVAL;

	/* Syncs each region's object with no VM lock held. */
	current = start;
	end = start + size;
	while (current < end) {
		object = NULL;
		offset = 0;

		vm_metadata_enter();
		mutex_lock(&vm->lock);

		/* Refuses a range that reaches a hole in the address space. */
		region = vmspace_find_region_locked(vm, current, 1);
		if (region == NULL) {
			mutex_unlock(&vm->lock);
			vm_metadata_leave();
			return ENOMEM;
		}

		/* Clamps the run to the region and takes the object it maps. */
		overlap_end = region->start + region->size;
		if (overlap_end > end)
			overlap_end = end;

		/* Takes a shared region's object so it survives the unlock. */
		if (region->object != NULL && (region->flags & VM_REGION_SHARED) != 0) {
			object = region->object;
			vm_object_ref(object);
			offset = region->file_offset +
			    (off_t)(current - region->start);
		}

		/* Syncs the object outside the locks, then releases it. */
		mutex_unlock(&vm->lock);
		vm_metadata_leave();

		if (object != NULL)
			error = vm_object_sync_range(object, offset,
			    overlap_end - current, flags);
		else
			error = 0;

		if (object != NULL)
			vm_object_put(object);

		if (error != 0)
			return error;

		current = overlap_end;
	}

	/* An invalidating sync forces every fault to look again. */
	if ((flags & MS_INVALIDATE) != 0) {
		vm_metadata_enter();
		mutex_lock(&vm->lock);
		vmspace_generation_advance_locked(vm);
		mutex_unlock(&vm->lock);
		vm_metadata_leave();
	}

	return 0;
}

/*
 * Maps anonymous memory at a fixed address, failing with EEXIST when the
 * range is in use.
 */
int
vmspace_map_anon_fixed_noreplace(
	struct vmspace *vm,
	uintptr_t start,
	size_t size,
	uint32_t prot,
	struct vm_region **result)
{
	int error;

	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;

	/* Runs the mapping under the metadata and space locks. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	error = vmspace_map_anon_fixed_noreplace_locked(vm, start, size, prot, result);
	if (error == 0)
		vmspace_generation_advance_locked(vm);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Maps anonymous memory at a fixed address, replacing whatever was there.
 */
int
vmspace_map_anon_fixed(
	struct vmspace *vm,
	uintptr_t start,
	size_t size,
	uint32_t prot,
	int shared,
	struct vm_region **result)
{
	struct vm_object *object;
	struct vm_region *prepared;
	unsigned flags;
	size_t commit_size;
	size_t data_size;
	int error;

	object = NULL;
	prepared = NULL;

	if (vm == NULL ||
	    vm == &kernel_vmspace ||
	    (shared != 0 && shared != 1))
		return EINVAL;

	/* A shared mapping is backed by an anonymous object; private memory is committed. */
	if (shared) {
		flags = VM_REGION_SHARED;
		commit_size = 0;
		data_size = size;
	} else {
		flags = 0;
		if (prot != 0)
			commit_size = size;
		else
			commit_size = 0;
		data_size = 0;
	}

	if (shared) {
		error = vm_object_create_anonymous(size, &object);
		if (error != 0)
			return error;
	}

	/* Prepares the region fully before replacing the old mappings. */
	error = prepare_region(start,
			       size,
			       prot,
			       VM_BACKING_ANON,
			       NULL,
			       0,
			       start,
			       data_size,
			       flags,
			       commit_size,
			       &prepared);
	if (error != 0) {
		if (object != NULL)
			vm_object_put(object);
		return error;
	}

	prepared->object = object;

	error = vmspace_replace_prepared(vm, prepared, result);
	if (error != 0)
		discard_prepared_region(prepared);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Maps a new shared anonymous object at a free address near a hint.
 */
int
vmspace_map_anon_shared_find(
	struct vmspace *vm,
	uintptr_t hint,
	size_t size,
	uint32_t prot,
	uintptr_t *mapped)
{
	struct vm_object *object;
	struct vm_region *region;
	uintptr_t start;
	size_t rounded;
	int error;

	region = NULL;

	/* Rejects a kernel space, a missing result, or an unusable size. */
	if (vm == NULL ||
	    vm == &kernel_vmspace ||
	    mapped == NULL ||
	    size == 0 ||
	    size > SIZE_MAX - (PAGE_SIZE - 1U))
		return EINVAL;

	/* Builds the anonymous object the mapping will share. */
	rounded = (size + PAGE_SIZE - 1U) & ~(uintptr_t)(PAGE_SIZE - 1U);
	error = vm_object_create_anonymous(rounded, &object);
	if (error != 0)
		return error;

	/* Finds a free range and maps the object there. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	/* Places the mapping in the first free range that fits. */
	error = vmspace_find_free_range_locked(vm, hint, rounded, PAGE_SIZE, &start);
	if (error == 0) {
		error = vmspace_map_region(vm,
				   start,
				   rounded,
				   prot,
				   VM_BACKING_ANON,
				   NULL,
				   0,
				   start,
				   rounded,
				   VM_REGION_SHARED,
				   0,
				   &region);
	}

	if (error == 0) {
		region->object = object;
		*mapped = start;
		vmspace_generation_advance_locked(vm);
	}

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	if (error != 0)
		vm_object_put(object);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Maps a private file region at a fixed address that must be free.
 */
int
vmspace_map_file(
	struct vmspace *vm,
	uintptr_t start,
	size_t size,
	uint32_t prot,
	struct file *file,
	off_t offset,
	uintptr_t data_start,
	size_t data_size,
	struct vm_region **result)
{
	struct vm_object *object;
	int error;

	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;

	/* Takes the file's object before the VM locks. */
	object = private_file_object(file);

	/* Runs the mapping under the metadata and space locks. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	error = vmspace_map_file_locked(vm, start, size, prot, file, offset,
	    data_start, data_size, object, result);
	if (error == 0)
		vmspace_generation_advance_locked(vm);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* A mapping that failed gives the object back. */
	if (error != 0 && object != NULL)
		vm_object_put(object);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Maps a shared file region at a fixed address that must be free.
 */
int
vmspace_map_file_shared(
	struct vmspace *vm,
	uintptr_t start,
	size_t size,
	uint32_t prot,
	struct file *file,
	off_t offset,
	size_t data_size,
	struct vm_region **result)
{
	struct vm_object *object;
	int error;

	/* Rejects unsupported address spaces before acquiring a shared object. */
	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;

	/* Resolves FAT identity before taking higher-ranked VM metadata locks. */
	error = vm_object_get_shared(file, &object);
	if (error != 0)
		return error;

	/* Publishes the region while the object makes formatting admission busy. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	error = vmspace_map_file_shared_locked(
		vm, start, size, prot, file, offset, data_size, object, result);
	if (error == 0)
		vmspace_generation_advance_locked(vm);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* Releases failed preparation after leaving address-space locks. */
	if (error != 0)
		vm_object_put(object);

	/* Reports publication or its exact failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Maps a file region at a fixed address, replacing whatever was there.
 */
int
vmspace_map_file_fixed(
	struct vmspace *vm,
	uintptr_t start,
	size_t size,
	uint32_t prot,
	struct file *file,
	off_t offset,
	size_t data_size,
	int shared,
	struct vm_region **result)
{
	struct vm_object *object;
	struct vm_region *prepared;
	unsigned flags;
	size_t commit_size;
	int error;

	object = NULL;
	prepared = NULL;

	if (vm == NULL ||
	    vm == &kernel_vmspace ||
	    file == NULL ||
	    (shared != 0 && shared != 1))
		return EINVAL;

	/* A shared mapping uses the file's object; a writable private one is committed. */
	if (shared) {
		flags = VM_REGION_SHARED;
		commit_size = 0;
	} else {
		flags = 0;
		if ((prot & HAL_SPACE_WRITE) != 0)
			commit_size = size;
		else
			commit_size = 0;
	}

	if (shared) {
		error = vm_object_get_shared(file, &object);
		if (error != 0)
			return error;
	} else {
		/* A private mapping maps the file's cached pages when it can. */
		object = private_file_object(file);
		if (object != NULL)
			flags |= VM_REGION_PRIVATE_OBJECT;
	}

	/* Prepares the region fully before replacing the old mappings. */
	error = prepare_region(start, size, prot, VM_BACKING_FILE, file, offset,
	    start, data_size, flags, commit_size, &prepared);
	if (error != 0) {
		if (object != NULL)
			vm_object_put(object);
		return error;
	}

	prepared->object = object;
	error = vmspace_replace_prepared(vm, prepared, result);
	if (error != 0)
		discard_prepared_region(prepared);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Maps the initial stack and its guard region below a top address.
 */
int
vmspace_map_stack(
	struct vmspace *vm,
	uintptr_t top,
	size_t size,
	size_t guard_size)
{
	int error;

	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;
	/* Runs the mapping under the metadata and space locks. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	error = vmspace_map_stack_locked(vm, top, size, guard_size);
	if (error == 0)
		vmspace_generation_advance_locked(vm);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Finds the region that contains a range, for inspection only.
 */
struct vm_region *
vmspace_find_region(
	struct vmspace *vm,
	uintptr_t address,
	size_t size)
{
	struct vm_region *region;

	/* Looks the region up under the metadata and space locks. */
	if (vm == NULL || vm == &kernel_vmspace)
		return NULL;
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	region = vmspace_find_region_locked(vm, address, size);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* Compatibility inspection only; callers must not retain the pointer. */
	return region;
}

/*
 * Reports the shared object and offset that back a user range.
 */
int
vmspace_shared_mapping_key(
	struct vmspace *vm,
	uintptr_t address,
	size_t size,
	struct vm_object **object,
	uintptr_t *offset)
{
	struct vm_region *region;
	int error;

	error = EINVAL;

	if (vm == NULL ||
	    vm == &kernel_vmspace ||
	    object == NULL ||
	    offset == NULL)
		return EINVAL;
	/* Reports the object and offset of a shared file-backed region. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	region = vmspace_find_region_locked(vm, address, size);
	if (region != NULL &&
	    (region->flags & VM_REGION_SHARED) != 0 &&
	    region->object != NULL &&
	    region->file_offset >= 0 &&
	    (uintmax_t)region->file_offset <= UINTPTR_MAX -
	    (address - region->start)) {
		vm_object_ref(region->object);
		*object = region->object;
		*offset = (uintptr_t)region->file_offset + address - region->start;
		error = 0;
	}

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Finds a free aligned range between two bounds.
 */
int
vmspace_find_free_range_bounded(
	struct vmspace *vm,
	uintptr_t minimum,
	uintptr_t maximum,
	size_t size,
	size_t alignment,
	uintptr_t *mapped)
{
	int error;

	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;

	/* Runs the mapping under the metadata and space locks. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	error = vmspace_find_free_range_bounded_locked(vm, minimum, maximum, size, alignment, mapped);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Finds a free aligned range at or above a hint in the mmap area.
 */
int
vmspace_find_free_range(
	struct vmspace *vm,
	uintptr_t hint,
	size_t size,
	size_t alignment,
	uintptr_t *mapped)
{
	int error;

	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;

	/* Runs the mapping under the metadata and space locks. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	error = vmspace_find_free_range_locked(vm, hint, size, alignment, mapped);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Maps anonymous memory at a free address near a hint.
 */
int
vmspace_map_find(
	struct vmspace *vm,
	uintptr_t hint,
	size_t size,
	uint32_t prot,
	uintptr_t *mapped)
{
	int error;

	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;

	/* Runs the mapping under the metadata and space locks. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	error = vmspace_map_find_locked(vm, hint, size, prot, mapped);
	if (error == 0)
		vmspace_generation_advance_locked(vm);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Maps a private file region at a free address near a hint.
 */
int
vmspace_map_file_find(
	struct vmspace *vm,
	uintptr_t hint,
	size_t size,
	uint32_t prot,
	struct file *file,
	off_t offset,
	size_t data_size,
	uintptr_t *mapped)
{
	struct vm_object *object;
	int error;

	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;

	/* Takes the file's object before the VM locks. */
	object = private_file_object(file);

	/* Runs the mapping under the metadata and space locks. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	/* Maps the file at a free range, handing the region the object. */
	error = vmspace_map_file_find_locked(vm, hint, size, prot, file, offset, data_size, object, mapped);
	if (error == 0)
		vmspace_generation_advance_locked(vm);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* A mapping that failed gives the object back. */
	if (error != 0 && object != NULL)
		vm_object_put(object);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Maps a shared file region at a free address near a hint.
 */
int
vmspace_map_file_shared_find(
	struct vmspace *vm,
	uintptr_t hint,
	size_t size,
	uint32_t prot,
	struct file *file,
	off_t offset,
	size_t data_size,
	uintptr_t *mapped)
{
	struct vm_object *object;
	int error;

	/* Rejects incomplete requests before taking a shared-object reference. */
	if (vm == NULL || vm == &kernel_vmspace || mapped == NULL)
		return EINVAL;

	/* Resolves FAT identity before taking higher-ranked VM metadata locks. */
	error = vm_object_get_shared(file, &object);
	if (error != 0)
		return error;

	/* Selects and publishes the region while the object excludes formatting. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	error = vmspace_map_file_shared_find_locked(vm, hint, size, prot, file, offset, data_size, object, mapped);
	if (error == 0)
		vmspace_generation_advance_locked(vm);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* Releases failed preparation outside address-space locks. */
	if (error != 0)
		vm_object_put(object);

	/* Reports publication or its exact failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Publishes captured cache pages as a private mapping, never MAP_SHARED.
 */
int
vmspace_map_exec_snapshot(
	struct vmspace *vm,
	uintptr_t start,
	uint32_t prot,
	struct file_exec_snapshot *snapshot)
{
	struct vm_region *region;
	int error;

	/* Requires a prepared read-only snapshot and a read-only mapping. */
	if (vm == NULL || vm == &kernel_vmspace || snapshot == NULL ||
	    !snapshot->input.active || !snapshot->input.shared_read ||
	    snapshot->input.read_object == NULL || snapshot->length == 0 ||
	    snapshot->page_count != snapshot->length / PAGE_SIZE ||
	    (prot & HAL_SPACE_WRITE) != 0)
		return EINVAL;

	/* Maps the snapshot's range, keeping the snapshot alive with the region. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	error = vmspace_map_region(vm, start, snapshot->length, prot, VM_BACKING_FILE,
	    snapshot->input.file, snapshot->offset, start, snapshot->length,
	    0, 0, &region);
	if (error == 0) {
		file_exec_snapshot_ref(snapshot);
		region->snapshot = snapshot;
		vmspace_generation_advance_locked(vm);
	}

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* Reports why the mapping failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Creates a region in a vmspace at a range that must be free; the caller
 * holds the VM locks.
 */
int
vmspace_map_region(
	struct vmspace *vm,
	uintptr_t start,
	size_t size,
	uint32_t prot,
	enum vm_region_backing backing,
	struct file *file,
	off_t file_offset,
	uintptr_t data_start,
	size_t data_size,
	unsigned flags,
	size_t commit_size,
	struct vm_region **result)
{
	struct vm_region *region;
	uint32_t max_prot;
	uint64_t mapped_bytes;
	int error;

	max_prot = HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC;

	/* Validates the range, the address limit, and the file description. */
	if (vm == NULL ||
	    vm == &kernel_vmspace ||
	    !vmspace_range_valid(start, size) ||
	    vmspace_overlaps(vm, start, size) ||
	    (prot & ~(HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC)) != 0)
		return EINVAL;

	mapped_bytes = vm->mapped_virtual_bytes;
	if (mapped_bytes > vm->address_limit)
		mapped_bytes = vm->address_limit;

	if ((uint64_t)size > vm->address_limit - mapped_bytes)
		return ENOMEM;

	if (backing == VM_BACKING_FILE &&
	    (file == NULL || file_offset < 0 || data_start < start ||
	     data_start >= start + size || data_size > start + size - data_start))
		return EINVAL;

	/* A shared mapping of an unwritable file can never become writable. */
	if ((flags & VM_REGION_SHARED) != 0 && file != NULL &&
	    ((file_status_flags_get(file) & O_ACCMODE) == O_RDONLY ||
	     file->f_ops == NULL || file->f_ops->pwrite == NULL))
		max_prot &= ~HAL_SPACE_WRITE;
	if ((prot & ~max_prot) != 0)
		return EACCES;

	/* Reserves the commit charge and links the region. */
	region = kern_calloc(1, sizeof(*region));
	if (region == NULL)
		return ENOMEM;
	if (commit_size != 0) {
		error = vm_commit_reserve(commit_size);
		if (error != 0) {
			kern_free(region);
			return error;
		}
	}

	/* Describes the mapping and publishes it in the address space. */
	region->start = start;
	region->size = size;
	region->prot = prot;
	region->max_prot = max_prot;
	region->flags = flags;
	region->commit_size = commit_size;
	region->backing = backing;
	region->file = file;
	region->file_offset = file_offset;
	region->data_start = data_start;
	region->data_size = data_size;

	if (file != NULL)
		file_ref(file);

	vmspace_insert_region(vm, region);
	vm->mapped_virtual_bytes += size;

	if (result != NULL)
		*result = region;

	return 0;
}

/*
 * Maps retained device storage at an exact range, retiring whatever the
 * range held (MAP_FIXED).
 *
 * The region takes its own reference to the mapping before it can be
 * faulted on; the caller keeps the reference it holds.
 */
int
vmspace_map_device_replace(
	struct vmspace *vm,
	uintptr_t start,
	size_t size,
	uint32_t prot,
	struct vm_device_mapping *mapping)
{
	struct vm_region *prepared;
	int error;

	/* Prepares the shared device region fully before the old mappings go. */
	error = prepare_region(start,
			       size,
			       prot,
			       VM_BACKING_DEVICE,
			       NULL,
			       0,
			       start,
			       size,
			       VM_REGION_SHARED,
			       0,
			       &prepared);
	if (error != 0)
		return error;

	/* The region holds the backing, and its rights never exceed the open's. */
	vm_device_ref(mapping);
	prepared->device = mapping;
	prepared->max_prot = mapping->max_prot;

	/* Publishes the region in place of what the range held. */
	error = vmspace_replace_prepared(vm, prepared, NULL);
	if (error != 0) {
		/* Discarding the region drops its backing reference too. */
		discard_prepared_region(prepared);
		return error;
	}

	/* Succeeded: faults now publish the device pages at the fixed address. */
	return 0;
}

/*
 * Maps anonymous memory at a free range; the caller holds the VM locks.
 */
int
vmspace_map_anon_locked(
	struct vmspace *vm,
	uintptr_t start,
	size_t size,
	uint32_t prot,
	struct vm_region **result)
{
	size_t commit_size;
	int error;

	/* Inaccessible memory is not committed until it becomes accessible. */
	if (prot != 0)
		commit_size = size;
	else
		commit_size = 0;

	/* Reports the failure. */
	error = vmspace_map_region(vm, start, size, prot, VM_BACKING_ANON, NULL, 0, start, 0, 0, commit_size, result);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Maps a private file region at a free range; the caller holds the VM locks.
 */
int
vmspace_map_file_locked(
	struct vmspace *vm,
	uintptr_t start,
	size_t size,
	uint32_t prot,
	struct file *file,
	off_t file_offset,
	uintptr_t data_start,
	size_t data_size,
	struct vm_object *object,
	struct vm_region **result)
{
	struct vm_region *region;
	size_t commit_size;
	unsigned flags;
	int error;

	/* Only a writable private file mapping needs commit. */
	if ((prot & HAL_SPACE_WRITE) != 0)
		commit_size = size;
	else
		commit_size = 0;

	/* A region given the file's object maps its cached pages, copying on write. */
	flags = 0;
	if (object != NULL)
		flags = VM_REGION_PRIVATE_OBJECT;

	/* Maps the region. */
	error = vmspace_map_region(vm, start, size, prot, VM_BACKING_FILE, file, file_offset, data_start, data_size, flags, commit_size, &region);
	if (error != 0)
		return error;

	/* The region takes the caller's object reference. */
	region->object = object;
	if (result != NULL)
		*result = region;

	/* Succeeded. */
	return 0;
}

/*
 * Maps a shared file region at a free range; the caller holds the VM locks.
 */
int
vmspace_map_file_shared_locked(
	struct vmspace *vm,
	uintptr_t start,
	size_t size,
	uint32_t prot,
	struct file *file,
	off_t file_offset,
	size_t data_size,
	struct vm_object *object,
	struct vm_region **result)
{
	struct vm_region *region;
	int error;

	/* Requires a prepared object and an aligned file offset. */
	if (object == NULL)
		return EINVAL;

	/* Rejects offsets that cannot describe shared pages. */
	if (file_offset < 0 || (file_offset & (PAGE_SIZE - 1U)) != 0)
		return EINVAL;

	/* Creates the region while its address-space metadata is locked. */
	error = vmspace_map_region(vm,
			   start,
			   size,
			   prot,
			   VM_BACKING_FILE,
			   file,
			   file_offset,
			   start,
			   data_size,
			   VM_REGION_SHARED,
			   0,
			   &region);
	if (error != 0)
		return error;

	/* Transfers the prepared shared-object reference to the region. */
	region->object = object;

	/* Returns the published region when requested. */
	if (result != NULL)
		*result = region;

	/* Reports successful publication. */
	return 0;
}

/* Builds a region that is not yet visible in any vmspace. */
static int
prepare_region(
	uintptr_t start,
	size_t size,
	uint32_t prot,
	enum vm_region_backing backing,
	struct file *file,
	off_t file_offset,
	uintptr_t data_start,
	size_t data_size,
	unsigned flags,
	size_t commit_size,
	struct vm_region **result)
{
	struct vm_region *region;
	uint32_t max_prot;
	int error;

	max_prot = HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC;

	/*
	 * MAP_FIXED uses this to reserve every fallible resource before it
	 * detaches an old mapping.
	 */
	if (result == NULL ||
	    !vmspace_range_valid(start, size) ||
	    (prot & ~(HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC)) != 0)
		return EINVAL;
	if (backing == VM_BACKING_FILE &&
	    (file == NULL || file_offset < 0 || data_start < start ||
	    data_start >= start + size || data_size > start + size - data_start))
		return EINVAL;

	/* A shared mapping of an unwritable file can never become writable. */
	if ((flags & VM_REGION_SHARED) != 0 && file != NULL &&
	    ((file_status_flags_get(file) & O_ACCMODE) == O_RDONLY ||
	    file->f_ops == NULL || file->f_ops->pwrite == NULL))
		max_prot &= ~HAL_SPACE_WRITE;
	if ((prot & ~max_prot) != 0)
		return EACCES;

	/* Reserves the commit charge and fills the region in. */
	region = kern_calloc(1, sizeof(*region));
	if (region == NULL)
		return ENOMEM;
	if (commit_size != 0) {
		error = vm_commit_reserve(commit_size);
		if (error != 0) {
			kern_free(region);
			return error;
		}
	}

	/* Describes the mapping without publishing it yet. */
	region->start = start;
	region->size = size;
	region->prot = prot;
	region->max_prot = max_prot;
	region->flags = flags;
	region->commit_size = commit_size;
	region->backing = backing;
	region->file = file;
	region->file_offset = file_offset;
	region->data_start = data_start;
	region->data_size = data_size;
	if (file != NULL)
		file_ref(file);
	*result = region;
	return 0;
}

/* Releases a prepared region that was never published. */
static void
discard_prepared_region(
	struct vm_region *region)
{
	if (region == NULL)
		return;
	if (region->file != NULL)
		(void)file_close(region->file);
	if (region->object != NULL)
		vm_object_put(region->object);
	if (region->snapshot != NULL)
		file_exec_snapshot_put(region->snapshot);
	if (region->device != NULL)
		vm_device_put(region->device);
	if (region->commit_size != 0)
		vm_commit_release(region->commit_size);
	vmspace_region_page_index_free(region);
	kern_free(region);
}

/* Maps anonymous memory at a range that must not be in use; the caller holds the VM locks. */
static int
vmspace_map_anon_fixed_noreplace_locked(
	struct vmspace *vm,
	uintptr_t start,
	size_t size,
	uint32_t prot,
	struct vm_region **result)
{
	int error;

	/* Refuses a malformed range, or one an existing region covers. */
	if (vm == NULL || !vmspace_range_valid(start, size))
		return EINVAL;
	if (vmspace_overlaps(vm, start, size))
		return EEXIST;

	/* Maps the range as ordinary anonymous memory. */

	/* Reports why the mapping failed. */
	error = vmspace_map_anon_locked(vm, start, size, prot, result);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Takes the shared VM object of a file a private mapping may map its cached
 * pages from, or NULL when the file has none (the mapping then copies each
 * page from the file).  The caller holds no VM lock: finding the object may
 * take the file system's locks first.
 */
static struct vm_object *
private_file_object(
	struct file *file)
{
	struct vm_object *object;
	int error;

	/* Asks for the object; a file without one maps page by page. */
	object = NULL;
	error = vm_object_get_shared(file, &object);
	if (error != 0)
		return NULL;

	/* Reports the object with the reference the region will take. */
	return object;
}

/* Maps the stack and its guard below a top address; the caller holds the VM locks. */
static int
vmspace_map_stack_locked(
	struct vmspace *vm,
	uintptr_t top,
	size_t size,
	size_t guard_size)
{
	struct vm_region *guard;
	struct vm_region *stack;
	uintptr_t bottom;
	uintptr_t guard_bottom;
	uint64_t mapped_bytes;
	int error;

	/* Validates the geometry against the layout and the limits. */
	if (vm == NULL ||
	    vm == &kernel_vmspace ||
	    size == 0 ||
	    guard_size == 0 ||
	    (top & (PAGE_SIZE - 1U)) != 0 ||
	    (size & (PAGE_SIZE - 1U)) != 0 ||
	    (guard_size & (PAGE_SIZE - 1U)) != 0 ||
	    top > vm_layout.user_limit ||
	    size > top ||
	    guard_size > top - size)
		return EINVAL;

	mapped_bytes = vm->mapped_virtual_bytes;
	if (mapped_bytes > vm->address_limit)
		mapped_bytes = vm->address_limit;

	if ((uint64_t)size > vm->stack_limit ||
	    (uint64_t)size + guard_size > vm->address_limit - mapped_bytes)
		return ENOMEM;

	bottom = top - size;
	guard_bottom = bottom - guard_size;
	if (!vmspace_range_valid(guard_bottom, size + guard_size) ||
	    vmspace_overlaps(vm, guard_bottom, size + guard_size))
		return EINVAL;

	/* Allocates both regions and the stack's commit charge. */
	guard = kern_calloc(1, sizeof(*guard));
	if (guard == NULL)
		return ENOMEM;

	stack = kern_calloc(1, sizeof(*stack));
	if (stack == NULL) {
		kern_free(guard);
		return ENOMEM;
	}

	/* Reserves the commitment the stack will use. */
	error = vm_commit_reserve(size);
	if (error != 0) {
		kern_free(stack);
		kern_free(guard);
		return error;
	}

	/* The guard is inaccessible and immutable; the stack is read-write. */
	guard->start = guard_bottom;
	guard->size = guard_size;
	guard->backing = VM_BACKING_ANON;
	guard->flags = VM_REGION_GUARD | VM_REGION_IMMUTABLE;
	guard->max_prot = 0;
	guard->data_start = guard_bottom;
	stack->start = bottom;
	stack->size = size;
	stack->prot = HAL_SPACE_READ | HAL_SPACE_WRITE;
	stack->max_prot = stack->prot;
	stack->flags = VM_REGION_STACK | VM_REGION_IMMUTABLE;
	stack->commit_size = size;
	stack->backing = VM_BACKING_ANON;
	stack->data_start = bottom;

	vmspace_insert_region(vm, guard);
	vmspace_insert_region(vm, stack);

	vm->mapped_virtual_bytes += size + guard_size;
	vm->stack_guard_bottom = guard_bottom;
	vm->stack_bottom = bottom;
	vm->stack_top = top;

	return 0;
}
