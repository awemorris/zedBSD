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

#include <errno.h>
#include <string.h>

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

	/* Device storage cannot become executable through later mprotect calls. */
	if ((attributes & ~VM_DEVICE_MMIO) != 0)
		return EINVAL;

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
	/* MMIO pages keep explicit device identity and uncached translations. */
	if ((mapping->attributes & VM_DEVICE_MMIO) != 0)
		return HAL_SPACE_DEVICE | HAL_SPACE_NOCACHE;

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

	/* Ordinary DMA RAM keeps its established cached alias. */
	if ((mapping->attributes & VM_DEVICE_MMIO) == 0) {
		memcpy(destination, (const uint8_t *)mapping->address + offset, bytes);

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

	/* Ordinary DMA RAM uses its existing cached kernel alias. */
	if ((mapping->attributes & VM_DEVICE_MMIO) == 0) {
		memcpy((uint8_t *)mapping->address + offset, source, bytes);

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
