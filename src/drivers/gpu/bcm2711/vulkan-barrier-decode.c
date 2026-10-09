/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Barrier decoding copies each count-selected dependency without borrowing wire pointers or generated arena contents. */
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/vulkan-barrier.h"

static int array_count(struct i915_wire_reader *reader, struct bcm2711_vulkan_barrier *barrier, uint32_t *count);
static void decode_header(struct i915_wire_reader *reader, struct bcm2711_vulkan_barrier *barrier, VkStructureType expected, struct bcm2711_vulkan_barrier_entry *entry);
static void decode_buffer(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct bcm2711_vulkan_barrier *barrier, struct bcm2711_vulkan_barrier_entry *entry);
static void decode_image(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct bcm2711_vulkan_barrier *barrier, struct bcm2711_vulkan_barrier_entry *entry);

/*
 * Decodes the complete existing client representation of one bounded explicit native dependency command.
 */
int
bcm2711_vulkan_barrier_decode(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct bcm2711_vulkan_barrier *barrier)
{
	struct bcm2711_vulkan_barrier_entry *entry;
	uint32_t count;
	uint32_t index;
	int error;

	/* Stage and dependency selections precede three independently count-selected arrays in the library wire format. */
	barrier->source = drv_i915_wire_read_u32(reader);
	barrier->destination = drv_i915_wire_read_u32(reader);
	barrier->flags = drv_i915_wire_read_u32(reader);
	error = array_count(reader, barrier, &count);
	if (error != 0)
		return error;

	/* Global memory dependencies own no typed resource but preserve their complete declared access scopes. */
	for (index = 0; index < count; index++) {
		entry = &barrier->entries[barrier->count++];
		decode_header(reader, barrier, VK_STRUCTURE_TYPE_MEMORY_BARRIER, entry);
	}

	/* Buffer dependencies retain their exact logical ranges independently of later public resource withdrawal. */
	error = array_count(reader, barrier, &count);
	if (error != 0)
		return error;
	for (index = 0; index < count; index++) {
		entry = &barrier->entries[barrier->count++];
		decode_buffer(session, reader, barrier, entry);
	}

	/* Images have one implemented full-colour subresource and carry explicit old/new layouts. */
	error = array_count(reader, barrier, &count);
	if (error != 0)
		return error;
	for (index = 0; index < count; index++) {
		entry = &barrier->entries[barrier->count++];
		decode_image(session, reader, barrier, entry);
	}

	/* An incomplete array can never be acknowledged as an ordinary void semantic refusal. */
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: complete copied dependencies contain no caller pointer or acquired partial owner. */
	return 0;
}

/* Bounds every count-selected array against the command's remaining whole dependency capacity before indexing it. */
static int
array_count(
	struct i915_wire_reader *reader,
	struct bcm2711_vulkan_barrier *barrier,
	uint32_t *count)
{
	uint32_t declared;
	uint64_t present;

	/* Array presence must select exactly the declared count; one finite limit covers all three arrays. */
	declared = drv_i915_wire_read_u32(reader);
	present = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || present != declared)
		return EINVAL;
	if (declared > BCM2711_VULKAN_BARRIERS - barrier->count)
		return E2BIG;
	*count = declared;

	/* Succeeded: the following array has one complete bounded dependency slot for every encoded element. */
	return 0;
}

/* Records unsupported structure/extension semantics while still consuming the complete selected native record. */
static void
decode_header(
	struct i915_wire_reader *reader,
	struct bcm2711_vulkan_barrier *barrier,
	VkStructureType expected,
	struct bcm2711_vulkan_barrier_entry *entry)
{
	uint32_t type;
	uint64_t next;

	/* Every dependency shares an exact type, absent extension chain and two explicit access masks. */
	type = drv_i915_wire_read_u32(reader);
	next = drv_i915_wire_read_u64(reader);
	entry->source = drv_i915_wire_read_u32(reader);
	entry->destination = drv_i915_wire_read_u32(reader);
	if (barrier->semantic_error == 0 && (type != (uint32_t)expected || next != 0))
		barrier->semantic_error = ENOTSUP;
}

/* Copies one exact buffer selection and its immutable logical range without taking a reference yet. */
static void
decode_buffer(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct bcm2711_vulkan_barrier *barrier,
	struct bcm2711_vulkan_barrier_entry *entry)
{
	uint64_t identity;

	/* Buffer ownership and its range follow the common access scope header. */
	decode_header(reader, barrier, VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, entry);
	entry->source_family = drv_i915_wire_read_u32(reader);
	entry->destination_family = drv_i915_wire_read_u32(reader);
	identity = drv_i915_wire_read_u64(reader);
	entry->offset = drv_i915_wire_read_u64(reader);
	entry->bytes = drv_i915_wire_read_u64(reader);
	entry->object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_BUFFER, identity);
	if (barrier->semantic_error == 0 && entry->object == NULL)
		barrier->semantic_error = EINVAL;
}

/* Copies one full-colour image layout transition and its exact typed resource without retaining a wire handle. */
static void
decode_image(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct bcm2711_vulkan_barrier *barrier,
	struct bcm2711_vulkan_barrier_entry *entry)
{
	uint64_t identity;

	/* Image transition semantics remain independent of native record padding and caller structure lifetime. */
	decode_header(reader, barrier, VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, entry);
	entry->before = drv_i915_wire_read_u32(reader);
	entry->after = drv_i915_wire_read_u32(reader);
	entry->source_family = drv_i915_wire_read_u32(reader);
	entry->destination_family = drv_i915_wire_read_u32(reader);
	identity = drv_i915_wire_read_u64(reader);
	entry->range.aspectMask = drv_i915_wire_read_u32(reader);
	entry->range.baseMipLevel = drv_i915_wire_read_u32(reader);
	entry->range.levelCount = drv_i915_wire_read_u32(reader);
	entry->range.baseArrayLayer = drv_i915_wire_read_u32(reader);
	entry->range.layerCount = drv_i915_wire_read_u32(reader);
	entry->object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_IMAGE, identity);
	if (barrier->semantic_error == 0 && entry->object == NULL)
		barrier->semantic_error = EINVAL;
}
