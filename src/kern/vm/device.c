/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Retains device storage independently of descriptor and address-space lifetime.
 */

#include <kern/vm-device.h>
#include <kern/device-io.h>
#include <kern/file.h>
#include <kern/kmem.h>
#include <kern/pmem.h>
#include <kern/page.h>
#include <kern/kcrt.h>
#include <uapi/errno.h>
#include "kern/vmspace.h"
#include "kern/lock.h"
#include "kern/sched.h"
#include "kern/swap.h"
#include "kern/vm-commit.h"
#include "kern/vm-lock.h"
#include "kern/vm-object.h"
#include "kern/vm-reclaim.h"
#include <uapi/mman.h>
#include "kern/clock.h"
#include "vmspace-internal.h"

/*
 * Whether this port's user translations write-combine device pages: amd64
 * alone (its HAL maps them through a PAT entry); the other ports would
 * make them Device or cached pages, so such a mapping is refused there.
 */
#if defined(HAL_ARCH_AMD64)
#define VM_DEVICE_WC_SUPPORTED 1
#else
#define VM_DEVICE_WC_SUPPORTED 0
#endif

/*
 * Creates one immutable mapping owner without allocating or freeing its storage.
 */
int
vm_device_create(
	struct file *file,
	uint64_t physical,
	void *address,
	size_t bytes,
	uint32_t attributes,
	uint32_t max_prot,
	void (*release)(void *),
	void *owner,
	struct vm_device_mapping **result)
{
	struct vm_device_mapping *mapping;

	/* Each successful object must retain both its session and terminal callback. */
	if (file == NULL ||
	    result == NULL ||
	    release == NULL)
		return EINVAL;

	/* Only complete pages with a stable kernel alias can back user mappings. */
	if (address == NULL || bytes == 0)
		return EINVAL;

	/* The physical base must match the VM page granularity. */
	if ((physical & (KERN_PAGE_SIZE - 1U)) != 0)
		return EINVAL;

	/* The retained kernel alias must start on the same page boundary. */
	if (((uintptr_t)address & (KERN_PAGE_SIZE - 1U)) != 0)
		return EINVAL;

	/* No partial final page may escape the immutable storage extent. */
	if ((bytes & (KERN_PAGE_SIZE - 1U)) != 0)
		return EINVAL;

	/* Both aliases must describe representable, non-wrapping complete extents. */
	if (physical > UINTPTR_MAX || bytes - 1U > UINTPTR_MAX - physical)
		return EOVERFLOW;

	/* Kernel copy operations require the complete virtual alias to be addressable. */
	if (bytes - 1U > UINTPTR_MAX - (uintptr_t)address)
		return EOVERFLOW;

	/* MMIO and Normal non-cacheable RAM are mutually exclusive storage identities. */
	if ((attributes & ~(VM_DEVICE_MMIO | VM_DEVICE_UNCACHED_RAM | VM_DEVICE_WRITE_COMBINE)) != 0 ||
	    (attributes & (VM_DEVICE_MMIO | VM_DEVICE_UNCACHED_RAM)) == (VM_DEVICE_MMIO | VM_DEVICE_UNCACHED_RAM))
		return EINVAL;

	/* Write-combining is a cache policy of MMIO alone, on a port that has it. */
	if ((attributes & VM_DEVICE_WRITE_COMBINE) != 0 && attributes != (VM_DEVICE_MMIO | VM_DEVICE_WRITE_COMBINE))
		return EINVAL;
	if ((attributes & VM_DEVICE_WRITE_COMBINE) != 0 && VM_DEVICE_WC_SUPPORTED == 0)
		return ENOTSUP;

	/* The original device open grants only a nonempty subset of read and write. */
	if (max_prot == 0 || (max_prot & ~(KERN_PROT_READ | KERN_PROT_WRITE)) != 0)
		return EACCES;

	/* Allocate before taking the file reference or consuming release ownership. */
	mapping = kern_calloc(1U, sizeof(*mapping));
	if (mapping == NULL)
		return ENOMEM;

	/* Every field except the reference count remains immutable until final put. */
	refcount_init(&mapping->references, 1U);
	mapping->file = file;
	mapping->physical = physical;
	mapping->address = address;
	mapping->bytes = bytes;
	mapping->attributes = attributes;
	mapping->max_prot = max_prot;
	mapping->release = release;
	mapping->owner = owner;
	file_ref(file);
	*result = mapping;

	/* Succeeded: the initial reference now owns the callback and file hold. */
	return 0;
}

/*
 * Tells whether this port's user translations can write-combine device
 * pages, so that a driver asks for the policy only where vm_device_create()
 * takes it.
 */
bool
vm_device_write_combine_supported(
	void)
{
	/* Succeeded: the port's answer, fixed when the kernel is built. */
	return VM_DEVICE_WC_SUPPORTED != 0;
}

/*
 * Retains storage for another region or pin without consulting a driver.
 */
void
vm_device_ref(
	struct vm_device_mapping *mapping)
{
	/* A new region or pin retains the immutable extent and its backend session. */
	refcount_get(&mapping->references);

	/* Succeeded: final storage release must now wait for this additional owner. */
	return;
}

/*
 * Final release runs outside VM locks and before the device's final file close.
 */
void
vm_device_put(
	struct vm_device_mapping *mapping)
{
	struct file *file;
	int final;

	/* Optional region cleanup can release an absent mapping. */
	if (mapping == NULL)
		return;

	/* A live region or user-access pin keeps the complete storage extent stable. */
	final = refcount_put(&mapping->references);
	if (!final)
		return;

	/* Release the resource hold while its open backend session still exists. */
	file = mapping->file;
	mapping->release(mapping->owner);
	kern_free(mapping);

	/* Descriptor close may now destroy the session and its remaining resources. */
	(void)file_close(file);

	/* Succeeded: no mapping owner can access the released storage again. */
	return;
}

/*
 * Returns only storage attributes, keeping region access rights independent.
 */
uint32_t
vm_device_page_attributes(
	const struct vm_device_mapping *mapping)
{
	/* Write-combined MMIO keeps device identity; a port without the policy refuses the mapping. */
	if ((mapping->attributes & VM_DEVICE_WRITE_COMBINE) != 0)
		return HAL_SPACE_DEVICE | HAL_SPACE_WC;

	/* MMIO pages keep explicit device identity and uncached translations. */
	if ((mapping->attributes & VM_DEVICE_MMIO) != 0)
		return HAL_SPACE_DEVICE | HAL_SPACE_NOCACHE;

	/* Managed uncached RAM uses Normal memory attributes, preserving unaligned byte access. */
	if ((mapping->attributes & VM_DEVICE_UNCACHED_RAM) != 0)
		return HAL_SPACE_NOCACHE;

	/* Succeeded: ordinary DMA RAM requires no device-only page attributes. */
	return 0U;
}

/*
 * Reads through the retained kernel alias, never through the RAM direct map.
 */
int
vm_device_read(
	struct vm_device_mapping *mapping,
	size_t offset,
	void *destination,
	size_t bytes)
{
	const volatile uint8_t *source;
	uint8_t *output;
	size_t index;

	/* Every copy must stay inside the immutable backing and its original rights. */
	if (mapping == NULL || destination == NULL)
		return EINVAL;

	/* A retained mapping does not grant read rights absent from its original open. */
	if ((mapping->max_prot & KERN_PROT_READ) == 0)
		return EACCES;

	/* Copying may reach the final byte but must never wrap or cross the backing extent. */
	if (offset > mapping->bytes || bytes > mapping->bytes - offset)
		return EFAULT;

	/* Managed RAM uses its retained cached or Normal non-cacheable kernel alias. */
	if ((mapping->attributes & VM_DEVICE_MMIO) == 0) {
		kern_memcpy(destination, (const uint8_t *)mapping->address + offset, bytes);

		/* Succeeded: the retained RAM alias supplied the entire requested range. */
		return 0;
	}

	/* Device-backed bytes use ordered device accessors with no cached RAM alias. */
	source = (const volatile uint8_t *)mapping->address + offset;
	output = destination;
	kern_io_read_barrier();

	/* Each byte crosses the device accessor boundary while the pin retains storage. */
	for (index = 0U; index < bytes; index++)
		output[index] = kern_mmio_read8(source + index);

	/* Complete device reads before ordinary kernel code consumes their output. */
	kern_io_read_barrier();

	/* Succeeded: the entire requested range has been copied. */
	return 0;
}

/*
 * Writes through the same retained alias used by the resource's kernel owner.
 */
int
vm_device_write(
	struct vm_device_mapping *mapping,
	size_t offset,
	const void *source,
	size_t bytes)
{
	volatile uint8_t *destination;
	const uint8_t *input;
	size_t index;

	/* A later VM operation cannot exceed this mapping's original write authority. */
	if (mapping == NULL || source == NULL)
		return EINVAL;

	/* A retained mapping does not grant write rights absent from its original open. */
	if ((mapping->max_prot & KERN_PROT_WRITE) == 0)
		return EACCES;

	/* Copying may reach the final byte but must never wrap or cross the backing extent. */
	if (offset > mapping->bytes || bytes > mapping->bytes - offset)
		return EFAULT;

	/* Managed RAM uses the same cached or Normal non-cacheable alias as its owner. */
	if ((mapping->attributes & VM_DEVICE_MMIO) == 0) {
		kern_memcpy((uint8_t *)mapping->address + offset, source, bytes);

		/* Succeeded: the retained RAM alias received the entire requested range. */
		return 0;
	}

	/* Ordered device access preserves the MMIO alias's cache and visibility rules. */
	destination = (volatile uint8_t *)mapping->address + offset;
	input = source;
	kern_io_write_barrier();

	/* Each byte crosses the device accessor boundary while the pin retains storage. */
	for (index = 0U; index < bytes; index++)
		kern_mmio_write8(destination + index, input[index]);

	/* Complete device writes before the caller can release its storage pin. */
	kern_io_write_barrier();

	/* Succeeded: every requested byte is written before releasing the pin. */
	return 0;
}

/*
 * Publishes a driver-owned shared extent without charging or allocating RAM pages.
 */
int
vmspace_map_device(
	struct vmspace *vm,
	uintptr_t hint,
	size_t size,
	uint32_t prot,
	struct vm_device_mapping *mapping,
	int exact,
	uintptr_t *mapped)
{
	struct vm_region *region;
	uintptr_t start;
	size_t rounded;
	int error;
	int valid;
	int occupied;

	/* Requires a retained backing and a user address space with an output owner. */
	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;

	/* Both the backing and the final mapped address need an explicit owner. */
	if (mapping == NULL || mapped == NULL)
		return EINVAL;

	/* Refuses empty or unroundable extents before any VM allocation. */
	if (size == 0 || size > SIZE_MAX - (PAGE_SIZE - 1U))
		return EINVAL;

	/* Neither initial nor later access rights may exceed the driver's original open authority. */
	if ((prot & ~mapping->max_prot) != 0)
		return EACCES;

	/* Partial final pages are already included in the driver's retained extent. */
	rounded = (size + PAGE_SIZE - 1U) & ~(size_t)(PAGE_SIZE - 1U);
	if (rounded > mapping->bytes)
		return EINVAL;

	/* Address selection and region publication share the usual VM serializer. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	/* Resolves an exact unoccupied placement or searches the ordinary user address range. */
	start = hint;
	error = 0;
	if (exact) {
		/* Exact placement must satisfy user range bounds before overlap can be examined. */
		valid = vmspace_range_valid(start, rounded);
		if (!valid) {
			error = EINVAL;
		} else {
			/* Existing regions remain untouched by MAP_FIXED_NOREPLACE semantics. */
			occupied = vmspace_overlaps(vm, start, rounded);
			if (occupied)
				error = EEXIST;
		}
	} else {
		error = vmspace_find_free_range_locked(vm, hint, rounded, PAGE_SIZE, &start);
	}

	/* Device ownership supplies storage; no anonymous commitment is reserved. */
	if (error == 0) {
		error = vmspace_map_region(
			vm,
			start,
			rounded,
			prot,
			VM_BACKING_DEVICE,
			NULL,
			0,
			start,
			rounded,
			VM_REGION_SHARED,
			0,
			&region);
	}

	/* A published region retains the immutable backing until final region or pin retirement. */
	if (error == 0) {
		vm_device_ref(mapping);
		region->device = mapping;
		region->max_prot = mapping->max_prot;
		*mapped = start;
		vmspace_generation_advance_locked(vm);
	}

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* The caller keeps its initial backing reference on both success and failure. */
	if (error != 0)
		return error;

	/* Succeeded: faults can now publish the retained device pages lazily. */
	return 0;
}

/*
 * Maps retained device storage while unmap and protection wait on its region
 * hold.
 */
int
vmspace_device_fault(
	struct vmspace *vm,
	struct vm_region *region,
	struct vm_page *page)
{
	hal_physaddr_t physical;
	uint32_t attributes;
	int status;
	int error;

	/* Immutable mapping geometry stays valid through the entire HAL operation. */
	physical = (hal_physaddr_t)(region->device->physical +
	    region->device_offset + page->address - region->start);
	attributes = region->prot | vm_device_page_attributes(region->device);
	status = hal_space_map(
		vm->space,
		(void *)page->address,
		physical,
		PAGE_SIZE,
		attributes);
	error = 0;
	if (status == HAL_ERR_NOMEM)
		error = ENOMEM;
	else if (status != HAL_OK)
		error = EFAULT;

	/* Publish completion before releasing the hold which excluded VM mutation. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	/* Only a successful HAL operation makes the retained device page visible to user access. */
	if (error == 0)
		page->flags |= VM_PAGE_RESIDENT | VM_MAPPING_MAPPED;

	/* Clearing BUSY and the hold lets unmap, protect and waiting faults observe one completed attempt. */
	page->flags &= ~VM_MAPPING_BUSY;
	region->hold_count--;
	vmspace_generation_advance_locked(vm);
	vmspace_fault_wake_locked(vm);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* An allocation failure retains only harmless page metadata for a later retry. */
	if (error != 0)
		return error;

	/* Succeeded: the user page shares the driver's original device storage. */
	return 0;
}
