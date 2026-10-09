/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* VA reuse follows actual translation flush; failed retirement preserves native RAM. */
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"
#include "drivers/gpu/bcm2711/v3d-memory.h"

static bool memory_ready(struct bcm2711_v3d_space *space);
static void remove_view(struct bcm2711_v3d_space *space, struct bcm2711_v3d_view *view);

/*
 * Maps an independent allocation hold before publishing a usable renderer address.
 * The caller excludes native jobs and every other map edit with its controller mutex.
 */
int
bcm2711_v3d_memory_map(
	struct bcm2711_v3d_space *space,
	struct bcm2711_buffer *buffer,
	struct bcm2711_v3d_view **result)
{
	struct bcm2711_v3d_view **position;
	struct bcm2711_v3d_view *view;
	uint64_t address;
	uint64_t bytes;
	uint64_t end;
	uint64_t physical_end;
	bool ready;
	int error;

	/* Only the checked ready engine can publish new native mappings. */
	*result = NULL;
	ready = memory_ready(space);
	if (!ready)
		return EIO;
	bytes = buffer->memory.size;
	if (bytes == 0 ||
	    bytes >= 0x100000000ULL ||
	    (bytes & 4095U) != 0)
		return EINVAL;
	physical_end = 1ULL << space->native->hardware.physical_bits;
	if (buffer->memory.paddr >= physical_end || bytes > physical_end - buffer->memory.paddr)
		return ENOTSUP;

	/* Finds the first complete hole while retaining failed-flush reservations as occupied. */
	address = 4096;
	position = &space->views;
	while (*position != NULL) {
		end = address + bytes;
		if (end <= (*position)->address)
			break;
		address = (uint64_t)(*position)->address + (*position)->bytes;
		position = &(*position)->next;
	}

	/* A whole page-rounded allocation must fit without consuming the reserved zero page. */
	if (address + bytes > 0x100000000ULL)
		return ENOSPC;
	view = kern_calloc(1, sizeof(*view));
	if (view == NULL)
		return ENOMEM;
	view->buffer = buffer;
	view->address = (uint32_t)address;
	view->bytes = bytes;
	view->references = 1;

	/* Software installation validates every slot before changing any page entry. */
	error = bcm2711_v3d_pages_map(space->native->hardware.pages->address, view->address, buffer->memory.paddr, bytes);
	if (error != 0) {
		kern_free(view);
		return error;
	}

	/* A native hold and VA reservation exist before either translation cache can observe them. */
	bcm2711_buffer_retain(buffer);
	view->next = *position;
	*position = view;
	error = bcm2711_v3d_hardware_pages_sync(space->native);
	if (error != 0) {
		/* Failed publication retains both the VA and allocation until an explicit global recovery. */
		view->references = 0;
		view->quarantined = true;
		return error;
	}

	/* Succeeded: the caller owns one mapped reference and can publish this renderer address. */
	*result = view;
	return 0;
}

/*
 * Retains a live native view for a prepared or executing trusted job.
 * The caller serializes references with resource destruction and worker retirement.
 */
void
bcm2711_v3d_memory_retain(
	struct bcm2711_v3d_view *view)
{
	/* A dead or quarantined view cannot be resurrected by another descriptor. */
	if (view->references == 0 ||
	    view->references == UINT32_MAX ||
	    view->quarantined)
		__builtin_trap();
	view->references++;

	/* Succeeded: the native allocation has another independent owner. */
	return;
}

/*
 * Retires a view only after its last resource or job owner has stopped native access.
 * Failed unmap or flush keeps its descriptor, VA reservation and allocation hold.
 */
int
bcm2711_v3d_memory_release(
	struct bcm2711_v3d_space *space,
	struct bcm2711_v3d_view *view)
{
	bool ready;
	int error;

	/* Each retained job or resource consumes exactly one live mapped reference. */
	if (view->references == 0)
		__builtin_trap();
	view->references--;
	if (view->references != 0)
		return 0;

	/* A faulted engine supplies no proof that a former translation or job is retired. */
	view->quarantined = true;
	ready = memory_ready(space);
	if (!ready)
		return EIO;

	/* Removes all software entries before flushing both hardware translation domains. */
	error = bcm2711_v3d_pages_unmap(space->native->hardware.pages->address, view->address, view->bytes);
	if (error != 0)
		return error;
	error = bcm2711_v3d_hardware_pages_sync(space->native);
	if (error != 0)
		return error;

	/* No device can still use this mapping, so VA and physical storage can be reused. */
	remove_view(space, view);
	bcm2711_buffer_release(view->buffer);
	kern_free(view);

	/* Succeeded: the final mapped owner and its native allocation hold are retired. */
	return 0;
}

/*
 * Releases quarantined ranges after checked native reset and external owner retirement.
 * The caller closes all admission and joins every worker before entering this routine.
 */
int
bcm2711_v3d_memory_recover(
	struct bcm2711_v3d_space *space)
{
	struct bcm2711_v3d_view *view;
	struct bcm2711_v3d_view *next;
	uint32_t *pages;
	bool ready;
	int error;

	/* A failed provider or engine reset never supplies a translation retirement boundary. */
	ready = memory_ready(space);
	if (!ready)
		return EIO;

	/* No surviving resource or job may lose its VA underneath an automatic recovery. */
	for (view = space->views; view != NULL; view = view->next) {
		if (view->references != 0 || !view->quarantined)
			return EBUSY;
	}

	/* Resets retired reservations, including entries already cleared by a failed earlier flush. */
	pages = space->native->hardware.pages->address;
	for (view = space->views; view != NULL; view = view->next) {
		/* Every owned range remains page-rounded and disjoint in the sorted reservation list. */
		kern_memset(pages + view->address / 4096U, 0, (size_t)(view->bytes / 4096U) * sizeof(*pages));
	}

	/* Hardware must observe the whole retirement before the first physical reference is dropped. */
	error = bcm2711_v3d_hardware_pages_sync(space->native);
	if (error != 0)
		return error;
	view = space->views;
	space->views = NULL;

	/* Consumes only fully retired quarantined views after both translation domains are flushed. */
	while (view != NULL) {
		next = view->next;
		bcm2711_buffer_release(view->buffer);
		kern_free(view);
		view = next;
	}

	/* Succeeded: all quarantined storage and VA reservations are safe to reuse. */
	return 0;
}

/* Removes one verified-retired mapping from the caller-serialized reservation list. */
static void
remove_view(
	struct bcm2711_v3d_space *space,
	struct bcm2711_v3d_view *view)
{
	struct bcm2711_v3d_view **position;

	/* Finds the exact lifetime owner rather than accepting a matching virtual address. */
	position = &space->views;
	while (*position != NULL && *position != view)
		position = &(*position)->next;

	/* Native allocation release may follow only a reservation this space actually owns. */
	if (*position == NULL)
		__builtin_trap();
	*position = view->next;

	/* Succeeded: the released view no longer occupies the native allocation list. */
	return;
}

/* Samples IRQ-owned fault admission without holding the guard across allocation or waits. */
static bool
memory_ready(
	struct bcm2711_v3d_space *space)
{
	unsigned long enabled;
	bool ready;

	/* Every asynchronous MMU fault closes mapping admission through the native IRQ guard. */
	enabled = spin_lock_irqsave(&space->native->hardware.guard);

	ready = false;
	if (space->native->hardware.ready &&
	    !space->native->hardware.faulted &&
	    !space->native->hardware.job_busy)
		ready = true;

	spin_unlock_irqrestore(&space->native->hardware.guard, enabled);

	/* Succeeded: the caller knows whether one serialized mapping operation can be admitted. */
	return ready;
}
