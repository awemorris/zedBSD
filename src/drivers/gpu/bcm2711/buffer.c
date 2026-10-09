/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Every allocation has one contiguous run, a stable CPU view and checked owners. */
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <kern/klog.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/buffer.h"

/* Bounds storage descriptions independently of available physical memory. */
#define BUFFER_MAX_BYTES (256ULL * 1024U * 1024U)

/*
 * Allocates a page-rounded contiguous run satisfying a device's address limit.
 * Failures leave no allocation owned by the caller.
 */
int
bcm2711_buffer_create(
	uint64_t bytes,
	uint64_t limit,
	size_t alignment,
	struct bcm2711_buffer **result)
{
	struct bcm2711_buffer *buffer;
	size_t rounded;
	int error;
	int released;

	/* Keeps byte rounding and physical placement representable. */
	*result = NULL;
	if (bytes == 0 || bytes > BUFFER_MAX_BYTES)
		return EINVAL;
	if (alignment < 4096U || (alignment & (alignment - 1U)) != 0)
		return EINVAL;
	rounded = (size_t)((bytes + 4095U) & ~4095ULL);

	/* Creates the owner before acquiring its physical allocation. */
	buffer = kern_calloc(1, sizeof(*buffer));
	if (buffer == NULL)
		return ENOMEM;

	/* Allocates under the native placement contract, without a bus-address alias. */
	error = kern_pmem_alloc_limited(rounded, alignment, limit, 0, &buffer->memory);
	if (error != 0) {
		kern_free(buffer);
		return error;
	}

	/* A successful allocation must supply a usable CPU view of the whole run. */
	buffer->address = kern_pmem_to_kernel(buffer->memory.paddr);
	if (buffer->address == NULL) {
		released = kern_pmem_free(&buffer->memory);
		if (released != 0) {
			/* Preserves the descriptor of a run the allocator refused to retire. */
			kern_logf("bcm2711: unmapped allocation release failed %d\n", released);
			return EFAULT;
		}

		/* The allocator retired the failed CPU view without any DMA publication. */
		kern_free(buffer);
		return EFAULT;
	}

	/* Initializes the bytes before sharing this immutable placement with another owner. */
	kern_memset(buffer->address, 0, rounded);
	spin_init(&buffer->guard, LOCK_RANK_DEVICE, "bcm2711-buffer");
	buffer->bytes = bytes;
	buffer->references = 1;
	*result = buffer;

	/* Succeeded: the caller holds the initial reference to zeroed contiguous RAM. */
	return 0;
}

/*
 * Retains storage protected by an existing reference or a publisher's mutex.
 */
void
bcm2711_buffer_retain(
	struct bcm2711_buffer *buffer)
{
	unsigned long enabled;

	/* Detects reference misuse before creating an unbounded lifetime or resurrection. */
	enabled = spin_lock_irqsave(&buffer->guard);

	if (buffer->references == 0 || buffer->references == UINT64_MAX)
		__builtin_trap();
	buffer->references++;

	spin_unlock_irqrestore(&buffer->guard, enabled);
}

/*
 * Releases one reference after its owner has stopped every DMA use.
 * A failed physical release keeps the run and descriptor allocated.
 */
void
bcm2711_buffer_release(
	struct bcm2711_buffer *buffer)
{
	unsigned long enabled;
	uint64_t remaining;
	int error;

	/* Drops exactly one live reference while another device may retain the allocation. */
	enabled = spin_lock_irqsave(&buffer->guard);

	if (buffer->references == 0)
		__builtin_trap();
	buffer->references--;
	remaining = buffer->references;

	spin_unlock_irqrestore(&buffer->guard, enabled);

	/* Other resource or DMA owners still need the same CPU and physical addresses. */
	if (remaining != 0)
		return;

	/* Only the final stopped owner may return the physical run to the allocator. */
	error = kern_pmem_free(&buffer->memory);
	if (error != 0) {
		kern_logf("bcm2711: retained failed physical release %d\n", error);
		return;
	}

	/* No shared reference or native access remains. */
	kern_free(buffer);
}
