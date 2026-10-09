/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Explicit dependencies stay immutable through queue wait and publish layout changes only after prior native operations retire. */
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"
#include "drivers/gpu/bcm2711/v3d-memory.h"
#include "drivers/gpu/bcm2711/vulkan-barrier.h"

static int release_barrier(struct bcm2711_vulkan_command_node *node);
static int retain_barrier(struct bcm2711_vulkan_barrier *barrier);

/*
 * Records one complete reply-free barrier with independently retained typed dependencies.
 */
int
bcm2711_vulkan_barrier_dispatch(
	struct bcm2711_vulkan_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_command_buffer *command;
	struct bcm2711_vulkan_barrier *barrier;
	uint64_t identity;
	int error;
	int released;

	/* Only this exact reply-free synchronization opcode belongs to the private barrier route. */
	(void)reply;
	*handled = 0;
	if (opcode != GPU_OP_CMD_PIPELINE_BARRIER)
		return 0;
	*handled = 1;
	if (requested != 0)
		return EINVAL;

	/* One session-local recording primary owns the entire copied dependency command. */
	identity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_COMMAND_BUFFER, identity);
	if (object == NULL)
		return EINVAL;
	command = object->payload;
	if (command->pending != 0 || command->state != BCM2711_VULKAN_COMMAND_RECORDING)
		return EBUSY;

	/* Heap storage bounds the kernel stack even when Keiland imports a complete batch of image dependencies. */
	barrier = kern_calloc(1, sizeof(*barrier));
	if (barrier == NULL)
		return ENOMEM;

	/* The common node prefix permits ordered graphics walks without losing this dependency array's independent destructor. */
	barrier->record.opcode = opcode;
	barrier->record.node.release = release_barrier;
	error = bcm2711_vulkan_barrier_decode(session, reader, barrier);
	if (error != 0) {
		kern_free(barrier);
		return error;
	}

	/* A prior void refusal still consumes complete framing but acquires no new recording owners. */
	if (command->recording_error != 0) {
		kern_free(barrier);
		return 0;
	}

	/* Barriers outside a pass cover the admitted serialized graphics, host and transfer operations. */
	error = bcm2711_vulkan_barrier_validate(barrier, command->owner.device);
	if (command->render_open)
		error = ENOTSUP;
	if (sizeof(*barrier) > BCM2711_VULKAN_RECORD_BYTES - command->recorded_bytes)
		error = ENOMEM;
	if (error == 0)
		error = retain_barrier(barrier);
	if (error != 0) {
		command->recording_error = error;
		released = release_barrier(&barrier->record.node);
		if (released != 0)
			return released;
		return 0;
	}

	/* A complete dependency node joins the same finite ordered primary list as all graphics state and draw events. */
	if (command->last == NULL) {
		command->first = &barrier->record.node;
	} else {
		command->last->next = &barrier->record.node;
	}

	/* The primary tail and retained-byte charge publish the same complete dependency node. */
	command->last = &barrier->record.node;
	command->recorded_bytes += sizeof(*barrier);

	/* Succeeded: the primary owns every dependency until permitted reset or final prepared graph retirement. */
	return 0;
}

/*
 * Makes serialized coherent host/native writes visible and commits the complete admitted image layout transition set.
 */
int
bcm2711_vulkan_barrier_run(
	const struct bcm2711_vulkan_record *record)
{
	const struct bcm2711_vulkan_barrier *barrier;
	const struct bcm2711_vulkan_barrier_entry *entry;
	struct bcm2711_vulkan_resource *resource;
	struct bcm2711_v3d_view *view;
	void *cpu;
	uint64_t offset;
	uint64_t bytes;
	uint32_t address;
	uint32_t index;
	int error;

	/* Prepared graph ownership supplies the same immutable derived node until native retirement. */
	if (record == NULL || record->opcode != GPU_OP_CMD_PIPELINE_BARRIER)
		return EINVAL;
	barrier = (const struct bcm2711_vulkan_barrier *)record;

	/* A complete preflight checks the FIFO-visible layout state before changing any image in this command. */
	for (index = 0; index < barrier->count; index++) {
		entry = &barrier->entries[index];
		if (entry->object == NULL)
			continue;
		resource = entry->object->payload;

		/* Each retained resource still resolves to coherent live native backing before any layout state changes. */
		offset = 0;
		bytes = resource->bytes;
		if (entry->object->kind == I915_VK_OBJ_BUFFER) {
			offset = entry->offset;
			bytes = entry->bytes;
			if (bytes == VK_WHOLE_SIZE)
				bytes = resource->bytes - offset;
		}

		/* Native output visibility and host publication both require the exact Normal uncached mapping contract. */
		error = bcm2711_vulkan_resource_backing(resource, offset, bytes, &view, &address, &cpu);
		if (error != 0)
			return error;
		if (!view->buffer->uncached)
			return EIO;

		/* Image source layout is FIFO state, not a recording-time guess before preceding passes or transitions execute. */
		if (entry->object->kind == I915_VK_OBJ_IMAGE &&
		    entry->before != VK_IMAGE_LAYOUT_UNDEFINED && entry->before != resource->layout)
			return EINVAL;
	}

	/* Normal uncached RAM needs no data-cache maintenance; the worker already waits for whole native completion and GPU cache retirement. */
	kern_io_read_barrier();
	kern_io_write_barrier();

	/* Every image transition publishes only after all earlier operations and the whole dependency set passed preflight. */
	for (index = 0; index < barrier->count; index++) {
		entry = &barrier->entries[index];
		if (entry->object == NULL || entry->object->kind != I915_VK_OBJ_IMAGE)
			continue;
		resource = entry->object->payload;
		resource->layout = entry->after;
	}

	/* Succeeded: full serial dependency ordering and each exact image layout are visible to following FIFO operations. */
	return 0;
}

/* Acquires one independent reference per exact resource selection while tracking only successfully retained edges. */
static int
retain_barrier(
	struct bcm2711_vulkan_barrier *barrier)
{
	struct bcm2711_vulkan_barrier_entry *entry;
	uint32_t index;
	int error;

	/* Global dependencies own no resource; buffer/image dependencies cannot lose their logical backing while the primary is pending. */
	for (index = 0; index < barrier->count; index++) {
		entry = &barrier->entries[index];
		if (entry->object == NULL)
			continue;
		error = bcm2711_vulkan_object_retain(entry->object);
		if (error != 0)
			return error;
		entry->retained = true;
	}

	/* Succeeded: every resource-dependent entry has its own independently tracked recording hold. */
	return 0;
}

/* Consumes every successfully acquired typed dependency even after an earlier native mapping teardown refusal. */
static int
release_barrier(
	struct bcm2711_vulkan_command_node *node)
{
	struct bcm2711_vulkan_barrier *barrier;
	struct bcm2711_vulkan_barrier_entry *entry;
	uint32_t index;
	int first;
	int error;

	/* Partial construction owns only entries whose successful retain was explicitly recorded. */
	barrier = (struct bcm2711_vulkan_barrier *)node;
	first = 0;
	for (index = 0; index < barrier->count; index++) {
		entry = &barrier->entries[index];
		if (!entry->retained)
			continue;
		error = bcm2711_vulkan_object_release(entry->object);
		if (first == 0 && error != 0)
			first = error;
	}

	/* The copied node retires even when independently rooted native storage needs later checked recovery. */
	kern_free(barrier);
	if (first != 0)
		return first;

	/* Succeeded: no typed dependency or immutable dependency-array storage remains owned by this node. */
	return 0;
}
