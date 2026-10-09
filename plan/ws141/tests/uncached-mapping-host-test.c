/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Actual native buffer/VM source preserves Normal NC attributes and retires its alias only after the final owner. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <hal/hal.h>
#include <kern/pmem.h>
#include <kern/vm-device.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/buffer.h"

/* The two fixture aliases distinguish direct-map misuse; actual CPU cache/physical aliases are outside this model. */
static void *cached;

/* This fixture alias stays live until the actual native final-owner unmap. */
static void *uncached;

/* Only an ordinary mapping refusal is injected; no production-only test control exists. */
static int mapping_error;

/* Observed retirement must unmap before returning the physical run. */
static unsigned unmapped;

/* Actual callback and file holds distinguish VM retirement from an initial descriptor release. */
static unsigned file_holds;

/* Ordinary descriptor accounting includes actual buffer and retained VM objects. */
static unsigned allocations;

static void release_buffer(void *owner);

/*
 * Supplies ordinary descriptor allocation for actual native and VM owners.
 */
void *
kern_calloc(
	size_t count,
	size_t bytes)
{
	void *pointer;

	/* Actual storage ownership is counted only after a successful ordinary allocation. */
	pointer = calloc(count, bytes);
	if (pointer != NULL)
		allocations++;

	/* Succeeded: the caller receives the exact host allocation outcome. */
	return pointer;
}

/*
 * Retires actual native logical storage through its ordinary allocator.
 */
void
kern_free(
	void *pointer)
{
	/* An absent partial-construction pointer owns no storage. */
	if (pointer == NULL)
		return;
	assert(allocations != 0);
	allocations--;
	free(pointer);
}

/*
 * Supplies one checked ordinary physical run with a distinguishable cached fixture alias.
 */
int
kern_pmem_alloc_limited(
	size_t bytes,
	size_t alignment,
	uint64_t limit,
	size_t boundary,
	struct kern_pmem *memory)
{
	int error;

	/* The actual buffer allocator must request precisely the native page-rounded reachability contract. */
	assert(cached == NULL && uncached == NULL && bytes == 8192 && alignment == 4096 && limit == 0x3fffffffU && boundary == 0);
	error = posix_memalign(&cached, alignment, bytes);
	if (error != 0)
		return ENOMEM;
	memory->paddr = 0x100000;
	memory->size = bytes;

	/* Succeeded: the fixture models one owned physical extent independently of its later CPU alias. */
	return 0;
}

/*
 * Exposes the initial direct map before the actual native buffer switches to Normal NC.
 */
void *
kern_pmem_to_kernel(
	hal_physaddr_t physical)
{
	/* Native buffer construction addresses only the exact retained physical run. */
	assert(physical == 0x100000 && cached != NULL);

	/* Succeeded: this is the fixture's initial cached alias, not the later coherent view. */
	return cached;
}

/*
 * Maps one Normal NC fixture alias without changing any production buffer behavior.
 */
int
kern_pmem_map_uncached(
	const struct kern_pmem *memory,
	void **mapped)
{
	int error;

	/* A mapping refusal leaves the actual unpublished cached allocation eligible for release. */
	if (mapping_error != 0)
		return mapping_error;
	assert(cached != NULL && uncached == NULL && memory->size == 8192);
	error = posix_memalign(&uncached, 4096, memory->size);
	if (error != 0)
		return ENOMEM;
	memcpy(uncached, cached, memory->size);
	memset(cached, 0xa5, memory->size);
	*mapped = uncached;

	/* Succeeded: the distinct host storage detects use of the retired cached address in actual driver/VM copies. */
	return 0;
}

/*
 * Removes the fixture view only after the actual final native reference retires.
 */
int
kern_pmem_unmap_uncached(
	void *mapped,
	size_t bytes)
{
	/* The actual native unmap must precede physical retirement and use the exact mapped extent. */
	assert(mapped == uncached && cached != NULL && bytes == 8192);
	free(uncached);
	uncached = NULL;
	unmapped++;

	/* Succeeded: no fixture CPU alias remains before physical storage retirement. */
	return 0;
}

/*
 * Returns physical storage after proving the uncached alias was already retired.
 */
int
kern_pmem_free(
	struct kern_pmem *memory)
{
	/* Freeing physical RAM behind an uncached alias would violate the actual native owner contract. */
	assert(cached != NULL && uncached == NULL && memory->size == 8192);
	free(cached);
	cached = NULL;
	memory->size = 0;

	/* Succeeded: the fixture retains no physical or CPU storage. */
	return 0;
}

/*
 * Observes the VM helper's independent retained file lifetime.
 */
void
file_ref(
	struct file *file)
{
	/* The fixture never exposes a real host file or a production kernel file implementation. */
	assert(file == (struct file *)1);
	file_holds++;
}

/*
 * Releases the VM helper's final retained file hold.
 */
int
file_close(
	struct file *file)
{
	/* The actual terminal VM callback ends before this open-file hold can retire. */
	assert(file == (struct file *)1 && file_holds != 0 && allocations == 0);
	file_holds--;

	/* Succeeded: final VM retirement no longer retains its fixture open file. */
	return 0;
}

/*
 * Refuses accidental MMIO access for the actual Normal NC RAM copy path.
 */
uint8_t
kern_mmio_read8(
	const volatile void *address)
{
	/* Managed Normal NC RAM must not enter a Device-memory accessor. */
	(void)address;
	assert(0);

	/* Succeeded: this unreachable return only supplies the fixture's declared ABI. */
	return 0;
}

/*
 * Refuses accidental MMIO writes for the actual Normal NC RAM copy path.
 */
void
kern_mmio_write8(
	volatile void *address,
	uint8_t byte)
{
	/* A retained Normal NC RAM alias supports ordinary byte copies without MMIO accessors. */
	(void)address;
	(void)byte;
	assert(0);
}

/*
 * Supplies a fixture barrier without claiming physical cache or SMP ordering.
 */
void
kern_io_read_barrier(
	void)
{
	/* No physical MMIO ordering is modeled by this single-thread host fixture. */
	return;
}

/*
 * Supplies a fixture write barrier for the unused MMIO copy branch.
 */
void
kern_io_write_barrier(
	void)
{
	/* Normal NC RAM takes ordinary copy operations instead of this device-only branch. */
	return;
}

/*
 * Rejects unexpected native retention diagnostics on ordinary successful retirement.
 */
void
kern_logf(
	const char *format,
	...)
{
	/* Every modeled physical/unmap release succeeds; a log would identify an unexpected ownership failure. */
	(void)format;
	assert(0);
}

/*
 * Checks alias selection, managed Normal NC attributes, independent VM holds and failure unwind.
 */
int
main(
	void)
{
	struct bcm2711_buffer *buffer;
	struct vm_device_mapping *mapping;
	uint32_t attributes;
	uint8_t source[3];
	uint8_t output[3];
	int error;

	/* A successful Normal NC owner keeps checked physical placement but replaces its former cached alias. */
	error = bcm2711_buffer_create_uncached(5000, 0x3fffffffU, 4096, &buffer);
	assert(error == 0 && buffer->uncached && buffer->address == uncached && buffer->address != cached);
	assert(buffer->bytes == 5000 && buffer->memory.size == 8192 && buffer->references == 1);
	bcm2711_buffer_retain(buffer);
	error = vm_device_create((struct file *)1, buffer->memory.paddr, buffer->address, buffer->memory.size,
				 VM_DEVICE_UNCACHED_RAM, KERN_PROT_READ | KERN_PROT_WRITE, release_buffer, buffer, &mapping);
	assert(error == 0 && file_holds == 1 && buffer->references == 2);
	attributes = vm_device_page_attributes(mapping);
	assert(attributes == HAL_SPACE_NOCACHE && (attributes & HAL_SPACE_DEVICE) == 0);

	/* Unaligned byte copies use the retained Normal alias, never poisoned cached storage or MMIO accessors. */
	source[0] = 3;
	source[1] = 7;
	source[2] = 11;
	error = vm_device_write(mapping, 4095, source, sizeof(source));
	assert(error == 0);
	error = vm_device_read(mapping, 4095, output, sizeof(output));
	assert(error == 0 && memcmp(source, output, sizeof(source)) == 0);
	assert(((uint8_t *)cached)[4095] == 0xa5 && ((uint8_t *)uncached)[4095] == 3);

	/* Resource retirement cannot unmap or free memory still held by a forked/pinned VM reference. */
	bcm2711_buffer_release(buffer);
	assert(uncached != NULL && unmapped == 0 && buffer->references == 1);
	vm_device_ref(mapping);
	vm_device_put(mapping);
	assert(uncached != NULL && unmapped == 0 && file_holds == 1);
	vm_device_put(mapping);
	assert(cached == NULL && uncached == NULL && unmapped == 1 && file_holds == 0 && allocations == 0);

	/* Ordinary uncached mapping failure retires its unpublished original allocation and descriptor. */
	mapping_error = ENOMEM;
	error = bcm2711_buffer_create_uncached(5000, 0x3fffffffU, 4096, &buffer);
	assert(error == ENOMEM && buffer == NULL && cached == NULL && uncached == NULL && allocations == 0);
	puts("WS141 actual native Normal NC buffer/VM attributes/alias lifetime: PASS");

	/* Succeeded: native software ownership/translation flags are verified without physical cache or SMP claims. */
	return 0;
}

/* Releases exactly the buffer reference transferred to the retained VM callback. */
static void
release_buffer(
	void *owner)
{
	/* No native data alias is accessed after its final reference is released. */
	bcm2711_buffer_release(owner);
}
