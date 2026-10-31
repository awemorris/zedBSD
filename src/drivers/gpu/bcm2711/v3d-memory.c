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

static int synchronize_range(struct bcm2711_v3d_space *space, uint32_t address, uint64_t bytes);
static bool memory_ready(struct bcm2711_v3d_space *space);
static bool window_open(struct bcm2711_v3d_space *space);
static bool changes_locked(const struct bcm2711_buffer *buffer, uint64_t offset, uint64_t since, uint32_t *box);
static bool changes_boxes_locked(const struct bcm2711_buffer *buffer, uint64_t offset, uint64_t since, uint32_t (*boxes)[4], uint32_t capacity, uint32_t *count);
static void boxes_add(uint32_t (*boxes)[4], uint32_t capacity, uint32_t *count, const uint32_t *box);
static void remove_view(struct bcm2711_v3d_space *space, struct bcm2711_v3d_view *view);

/*
 * Starts one serialized translation batch before DMA launch or after DMA retirement.
 */
int
bcm2711_v3d_memory_begin(
	struct bcm2711_v3d_space *space)
{
	bool ready;

	/* Only the actual native space can own a deferred publication interval. */
	if (space == NULL || space->native == NULL)
		return EINVAL;

	/* Nested batches cannot give two callers the same completion boundary. */
	if (space->deferred)
		return EBUSY;

	/* An active or faulted engine cannot edit translations underneath native work. */
	ready = memory_ready(space);
	if (!ready)
		return EIO;

	/* No earlier interval contributes dirty PTEs to this new caller-owned batch. */
	space->pending_address = 0;
	space->pending_end = 0;
	space->deferred = true;

	/* Succeeded: the caller must end publication before launching DMA or reusing retired RAM. */
	return 0;
}

/*
 * Publishes one complete translation batch before freeing any retired backing.
 * A failed flush preserves every pending view and physical hold in quarantine.
 */
int
bcm2711_v3d_memory_end(
	struct bcm2711_v3d_space *space)
{
	struct bcm2711_v3d_view *view;
	struct bcm2711_v3d_view *next;
	uint32_t address;
	uint64_t end;
	int error;

	/* A matching begin is required even for an interval with no changes. */
	if (space == NULL || space->native == NULL)
		return EINVAL;
	if (!space->deferred)
		return EINVAL;

	/* Consumes this publication interval once, including a later failed hardware flush. */
	address = space->pending_address;
	end = space->pending_end;
	space->deferred = false;
	space->pending_address = 0;
	space->pending_end = 0;

	/* Retained references alone need no PTE publication or physical release. */
	if (end == 0)
		return 0;

	/* One cache/TLB completion covers every changed entry and precedes all VA or RAM reuse. */
	error = bcm2711_v3d_hardware_pages_sync(space->native, address, end - address);
	if (error != 0)
		return error;

	/* Only software-unmapped zero-reference views from this batch may now retire. */
	for (view = space->views; view != NULL; view = next) {
		next = view->next;
		if (!view->pending_release)
			continue;

		/* The completed flush proves these reserved addresses no longer reach the allocation. */
		remove_view(space, view);
		bcm2711_buffer_release(view->buffer);
		kern_free(view);
	}

	/* Succeeded: live maps are publishable and all confirmed batch retirements are consumed. */
	return 0;
}

/*
 * Maps an independent allocation hold before publishing a usable renderer address.
 * The caller excludes native jobs and every other map edit with its controller mutex.
 * Inside a batch, the returned address becomes executable only after checked end.
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
	error = synchronize_range(space, view->address, view->bytes);
	if (error != 0) {
		/* Failed publication retains both the VA and allocation until an explicit global recovery. */
		view->references = 0;
		view->quarantined = true;
		return error;
	}

	/* Succeeded: the caller owns one mapped reference; a batch owner still owes publication before DMA. */
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
 * Batch retirement also keeps that backing until the checked end flush.
 */
int
bcm2711_v3d_memory_release(
	struct bcm2711_v3d_space *space,
	struct bcm2711_v3d_view *view)
{
	bool window;
	bool ready;
	int error;

	/* Each retained job or resource consumes exactly one live mapped reference. */
	if (view->references == 0)
		__builtin_trap();
	view->references--;
	if (view->references != 0)
		return 0;

	/* Inside the executor's window the GPU may be running: the view stays mapped until the window closes. */
	window = window_open(space);
	if (window) {
		view->delayed = true;
		return 0;
	}

	/* A faulted engine supplies no proof that a former translation or job is retired. */
	view->quarantined = true;
	ready = memory_ready(space);
	if (!ready)
		return EIO;

	/* Removes all software entries before flushing both hardware translation domains. */
	error = bcm2711_v3d_pages_unmap(space->native->hardware.pages->address, view->address, view->bytes);
	if (error != 0)
		return error;
	error = synchronize_range(space, view->address, view->bytes);
	if (error != 0)
		return error;

	/* The final batch flush must complete before this zero-reference backing can be reused. */
	if (space->deferred) {
		view->pending_release = true;
		return 0;
	}

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

	/* A caller still owns an unfinished translation-publication interval. */
	if (space->deferred)
		return EBUSY;

	/* No surviving resource or job may lose its VA underneath an automatic recovery; a delayed view counts as quarantined. */
	for (view = space->views; view != NULL; view = view->next) {
		if (view->references != 0 || (!view->quarantined && !view->delayed))
			return EBUSY;
	}

	/* Resets retired reservations, including entries already cleared by a failed earlier flush. */
	pages = space->native->hardware.pages->address;
	for (view = space->views; view != NULL; view = view->next) {
		/* Every owned range remains page-rounded and disjoint in the sorted reservation list. */
		kern_memset(pages + view->address / 4096U, 0, (size_t)(view->bytes / 4096U) * sizeof(*pages));
	}

	/* Hardware must observe the whole retirement before the first physical reference is dropped. */
	error = bcm2711_v3d_hardware_pages_sync(space->native, 0, 0x100000000ULL);
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

/*
 * Records one write of the box of the image at offset and advances the generation.
 *
 * The run's guard orders writes from the render and display devices.  A NULL box
 * stands for a write whose extent is unknown, which forces whole copies.
 */
void
bcm2711_v3d_memory_written(
	struct bcm2711_buffer *buffer,
	uint64_t offset,
	const uint32_t *box)
{
	struct bcm2711_buffer_write *write;
	unsigned long enabled;

	/* The new generation names this write in the ring; the run's guard orders it with the display's writes. */
	enabled = spin_lock_irqsave(&buffer->guard);

	buffer->generation++;
	write = &buffer->writes[buffer->write_next % BCM2711_BUFFER_WRITES];
	buffer->write_next++;
	write->generation = buffer->generation;
	write->offset = offset;

	/* An unknown extent is recorded as an impossible box. */
	if (box == NULL) {
		write->offset = UINT64_MAX;
		kern_memset(write->box, 0, sizeof(write->box));
	} else {
		kern_memcpy(write->box, box, sizeof(write->box));
	}

	spin_unlock_irqrestore(&buffer->guard, enabled);

	/* Succeeded: the write is recorded. */
	return;
}

/*
 * Reports the union of the writes of the image at offset after generation since.
 *
 * Every generation after since must still be in the ring and belong to the
 * same image; otherwise the caller copies the whole image.
 */
bool
bcm2711_v3d_memory_changes(
	struct bcm2711_buffer *buffer,
	uint64_t offset,
	uint64_t since,
	uint32_t *box)
{
	unsigned long enabled;
	bool known;

	/* The generation and the ring are read together under the run's guard. */
	enabled = spin_lock_irqsave(&buffer->guard);

	known = changes_locked(buffer, offset, since, box);

	spin_unlock_irqrestore(&buffer->guard, enabled);

	/* Reports whether box covers every change. */
	return known;
}

/*
 * Reports the writes of the image at offset after generation since as a
 * list of at most capacity boxes: a box another holds is
 * left out, and writes needing more boxes are given as their union.
 *
 * Every generation after since must still be in the ring and belong to the
 * same image; otherwise the caller copies the whole image.  A count of 0
 * with true means nothing changed.
 */
bool
bcm2711_v3d_memory_changes_boxes(
	struct bcm2711_buffer *buffer,
	uint64_t offset,
	uint64_t since,
	uint32_t (*boxes)[4],
	uint32_t capacity,
	uint32_t *count)
{
	unsigned long enabled;
	bool known;

	/* The generation and the ring are read together under the run's guard. */
	enabled = spin_lock_irqsave(&buffer->guard);

	known = changes_boxes_locked(buffer, offset, since, boxes, capacity, count);

	spin_unlock_irqrestore(&buffer->guard, enabled);

	/* Reports whether the boxes cover every change. */
	return known;
}

/* Computes the union of the writes after since; the caller holds the run's guard. */
static bool
changes_locked(
	const struct bcm2711_buffer *buffer,
	uint64_t offset,
	uint64_t since,
	uint32_t *box)
{
	const struct bcm2711_buffer_write *write;
	uint64_t generation;
	uint32_t index;

	/* No change leaves an empty box. */
	box[0] = UINT32_MAX;
	box[1] = UINT32_MAX;
	box[2] = 0;
	box[3] = 0;
	if (since > buffer->generation || buffer->generation - since > BCM2711_BUFFER_WRITES)
		return false;

	/* Each later generation's write, found by its place in the ring. */
	for (generation = since + 1U; generation <= buffer->generation; generation++) {
		index = (uint32_t)((buffer->write_next - 1U - (buffer->generation - generation)) % BCM2711_BUFFER_WRITES);
		write = &buffer->writes[index];

		/* A forgotten, unknown or other image's write leaves only a whole copy. */
		if (write->generation != generation || write->offset != offset)
			return false;
		if (write->box[0] < box[0])
			box[0] = write->box[0];
		if (write->box[1] < box[1])
			box[1] = write->box[1];
		if (write->box[2] > box[2])
			box[2] = write->box[2];
		if (write->box[3] > box[3])
			box[3] = write->box[3];
	}

	/* Succeeded: box covers every pixel the image changed after since. */
	return true;
}

/* Publishes a range immediately or accumulates it in the caller's no-DMA batch. */
static int
synchronize_range(
	struct bcm2711_v3d_space *space,
	uint32_t address,
	uint64_t bytes)
{
	uint64_t end;
	int error;

	/* Ordinary resource operations preserve their existing immediate visibility contract. */
	if (!space->deferred) {
		error = bcm2711_v3d_hardware_pages_sync(space->native, address, bytes);
		if (error != 0)
			return error;
		return 0;
	}

	/* One bounded interval covers all edited PTEs, including holes that stayed unchanged. */
	end = (uint64_t)address + bytes;
	if (space->pending_end == 0 || address < space->pending_address)
		space->pending_address = address;
	if (end > space->pending_end)
		space->pending_end = end;

	/* Succeeded: actual hardware publication remains the batch owner's checked end operation. */
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
	    !space->native->hardware.job_busy &&
	    !space->native->hardware.window)
		ready = true;

	spin_unlock_irqrestore(&space->native->hardware.guard, enabled);

	/* Succeeded: the caller knows whether one serialized mapping operation can be admitted. */
	return ready;
}

/* Tells whether the executor's window is open. */
static bool
window_open(
	struct bcm2711_v3d_space *space)
{
	unsigned long enabled;
	bool window;

	/* The executor opens and closes it under the native IRQ guard. */
	enabled = spin_lock_irqsave(&space->native->hardware.guard);

	window = space->native->hardware.window;

	spin_unlock_irqrestore(&space->native->hardware.guard, enabled);

	/* Succeeded: the caller knows whether translations must stay unchanged. */
	return window;
}

/*
 * Unmaps and frees the views whose last reference ended inside the window.
 *
 * One batch publishes all their PTE withdrawals before any backing is freed.
 * A faulted engine proves nothing: the views stay for checked recovery.
 */
int
bcm2711_v3d_memory_release_delayed(
	struct bcm2711_v3d_space *space)
{
	struct bcm2711_v3d_view *view;
	bool found;
	bool ready;
	int error;
	int ended;

	/* Nothing to do without a delayed view. */
	found = false;
	for (view = space->views; view != NULL; view = view->next) {
		if (view->delayed)
			found = true;
	}
	if (!found)
		return 0;

	/* Recovery reclaims them after a fault. */
	ready = memory_ready(space);
	if (!ready)
		return 0;

	/* Every withdrawal joins one batch; the end flushes once and frees them. */
	error = bcm2711_v3d_memory_begin(space);
	if (error != 0)
		return error;
	for (view = space->views; view != NULL; view = view->next) {
		if (!view->delayed)
			continue;
		view->delayed = false;
		view->quarantined = true;
		error = bcm2711_v3d_pages_unmap(space->native->hardware.pages->address, view->address, view->bytes);
		if (error != 0)
			break;
		error = synchronize_range(space, view->address, view->bytes);
		if (error != 0)
			break;
		view->pending_release = true;
	}
	ended = bcm2711_v3d_memory_end(space);
	if (error == 0)
		error = ended;
	if (error != 0)
		return error;

	/* Succeeded: the delayed views and their backing are retired. */
	return 0;
}

/* Collects the writes after since as boxes; the caller holds the run's guard. */
static bool
changes_boxes_locked(
	const struct bcm2711_buffer *buffer,
	uint64_t offset,
	uint64_t since,
	uint32_t (*boxes)[4],
	uint32_t capacity,
	uint32_t *count)
{
	const struct bcm2711_buffer_write *write;
	uint64_t generation;
	uint32_t index;

	/* No change leaves no box. */
	*count = 0;
	if (capacity == 0)
		return false;
	if (since > buffer->generation || buffer->generation - since > BCM2711_BUFFER_WRITES)
		return false;

	/* Each later generation's write, found by its place in the ring. */
	for (generation = since + 1U; generation <= buffer->generation; generation++) {
		index = (uint32_t)((buffer->write_next - 1U - (buffer->generation - generation)) % BCM2711_BUFFER_WRITES);
		write = &buffer->writes[index];

		/* A forgotten, unknown or other image's write leaves only a whole copy. */
		if (write->generation != generation || write->offset != offset)
			return false;

		/* An empty box changed nothing. */
		if (write->box[2] <= write->box[0] || write->box[3] <= write->box[1])
			continue;
		boxes_add(boxes, capacity, count, write->box);
	}

	/* Succeeded: the boxes cover every pixel the image changed after since. */
	return true;
}

/* Adds a box to a list: left out when one holds it, as the union of all when no room is left. */
static void
boxes_add(
	uint32_t (*boxes)[4],
	uint32_t capacity,
	uint32_t *count,
	const uint32_t *box)
{
	uint32_t index;

	/* A box that holds it already. */
	for (index = 0; index < *count; index++) {
		if (boxes[index][0] <= box[0] &&
		    boxes[index][1] <= box[1] &&
		    boxes[index][2] >= box[2] &&
		    boxes[index][3] >= box[3])
			return;
	}

	/* Room for it. */
	if (*count < capacity) {
		kern_memcpy(boxes[*count], box, sizeof(boxes[0]));
		*count += 1U;
		return;
	}

	/* No room: the first box becomes the union of all of them and the new one. */
	for (index = 1; index < *count; index++) {
		if (boxes[index][0] < boxes[0][0])
			boxes[0][0] = boxes[index][0];
		if (boxes[index][1] < boxes[0][1])
			boxes[0][1] = boxes[index][1];
		if (boxes[index][2] > boxes[0][2])
			boxes[0][2] = boxes[index][2];
		if (boxes[index][3] > boxes[0][3])
			boxes[0][3] = boxes[index][3];
	}
	if (box[0] < boxes[0][0])
		boxes[0][0] = box[0];
	if (box[1] < boxes[0][1])
		boxes[0][1] = box[1];
	if (box[2] > boxes[0][2])
		boxes[0][2] = box[2];
	if (box[3] > boxes[0][3])
		boxes[0][3] = box[3];
	*count = 1;
}
