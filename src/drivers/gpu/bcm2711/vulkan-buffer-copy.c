/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Exact coherent CPU byte transfers are FIFO work, distinct from GPU-rendered graphics and image meta passes. */
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/vulkan-buffer-copy.h"
#include "drivers/gpu/bcm2711/vulkan-resource.h"
#include "drivers/gpu/bcm2711/v3d-memory.h"

static int decode_copy(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct bcm2711_vulkan_buffer_copy *copy);
static int resolve_buffers(const struct bcm2711_vulkan_buffer_copy *copy, struct bcm2711_vulkan_object *device, struct bcm2711_vulkan_resource *resources[2], void *cpu[2]);
static bool intervals_overlap(uintptr_t first, uint64_t first_bytes, uintptr_t second, uint64_t second_bytes);
static int release_copy(struct bcm2711_vulkan_command_node *node);

/*
 * Records one complete reply-free buffer copy with all regions and exact independently retained resources.
 */
int
bcm2711_vulkan_buffer_copy_dispatch(
	struct bcm2711_vulkan_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_command_buffer *command;
	struct bcm2711_vulkan_buffer_copy *copy;
	uint64_t identity;
	uint32_t index;
	int error;
	int released;

	/* The route handles only actual core buffer-copy recording, without a reply-bearing command result. */
	(void)reply;
	*handled = 0;
	if (opcode != GPU_OP_CMD_COPY_BUFFER)
		return 0;
	*handled = 1;
	if (requested != 0)
		return EINVAL;

	/* An exact non-pending primary owns this complete recording vector. */
	identity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_COMMAND_BUFFER, identity);
	if (object == NULL)
		return EINVAL;
	command = object->payload;
	if (command->pending != 0 || command->state != BCM2711_VULKAN_COMMAND_RECORDING)
		return EBUSY;

	/* All region storage stays off the kernel stack and precedes acquisition of either typed input edge. */
	copy = kern_calloc(1, sizeof(*copy));
	if (copy == NULL)
		return ENOMEM;
	copy->record.opcode = opcode;
	copy->record.node.release = release_copy;
	error = decode_copy(session, reader, copy);
	if (error != 0) {
		kern_free(copy);
		return error;
	}

	/* Prior void failures still consume complete actual wire framing without acquiring new inputs. */
	if (command->recording_error != 0) {
		kern_free(copy);
		return 0;
	}

	/* Complete interface, physical interval and recording budget checks precede both independently retained resources. */
	error = bcm2711_vulkan_buffer_copy_validate(&copy->record, command->owner.device);
	if (error == 0 && command->render_open)
		error = EINVAL;
	if (error == 0 && sizeof(*copy) > BCM2711_VULKAN_RECORD_BYTES - command->recorded_bytes)
		error = ENOMEM;
	for (index = 0; index < 2 && error == 0; index++) {
		error = bcm2711_vulkan_object_retain(copy->record.objects[index]);
		if (error == 0)
			copy->retained[index] = true;
	}

	/* Refusing the complete void command preserves its first End outcome and retires every unpublished retained prefix. */
	if (error != 0) {
		command->recording_error = error;
		released = release_copy(&copy->record.node);
		if (released != 0)
			return released;
		return 0;
	}

	/* Publication appends exactly one whole immutable transfer to the primary's finite ordered work. */
	if (command->last == NULL) {
		command->first = &copy->record.node;
	} else {
		command->last->next = &copy->record.node;
	}

	/* Both tail and byte charge describe the same complete retained node. */
	command->last = &copy->record.node;
	command->recorded_bytes += sizeof(*copy);

	/* Succeeded: no application region or public buffer identity can replace the retained copied inputs. */
	return 0;
}

/*
 * Validates the complete buffer-copy vector and all physical source/destination exclusions before any byte changes.
 */
int
bcm2711_vulkan_buffer_copy_validate(
	const struct bcm2711_vulkan_record *record,
	struct bcm2711_vulkan_object *device)
{
	const struct bcm2711_vulkan_buffer_copy *copy;
	const VkBufferCopy *region;
	const VkBufferCopy *other_region;
	struct bcm2711_vulkan_resource *resources[2];
	void *cpu[2];
	uint32_t index;
	uint32_t other;
	bool overlaps;
	int error;

	/* Only the complete copied bounded core command can supply valid region metadata. */
	if (record == NULL ||
	    device == NULL ||
	    record->opcode != GPU_OP_CMD_COPY_BUFFER)
		return EINVAL;
	if (record->count == 0 || record->count > BCM2711_VULKAN_BUFFER_COPY_REGIONS)
		return EINVAL;
	copy = (const struct bcm2711_vulkan_buffer_copy *)record;
	error = resolve_buffers(copy, device, resources, cpu);
	if (error != 0)
		return error;

	/* All logical intervals are checked before absolute alias arithmetic or any selected source read. */
	for (index = 0; index < record->count; index++) {
		region = &copy->regions[index];
		if (region->size == 0)
			return EINVAL;
		if (region->srcOffset >= resources[0]->bytes || region->dstOffset >= resources[1]->bytes)
			return EINVAL;
		if (region->size > resources[0]->bytes - region->srcOffset || region->size > resources[1]->bytes - region->dstOffset)
			return EINVAL;
	}

	/* Every source region excludes every destination even when distinct public buffers share the same physical backing. */
	for (index = 0; index < record->count; index++) {
		region = &copy->regions[index];
		for (other = 0; other < record->count; other++) {
			other_region = &copy->regions[other];
			overlaps = intervals_overlap((uintptr_t)cpu[0] + region->srcOffset, region->size, (uintptr_t)cpu[1] + other_region->dstOffset, other_region->size);
			if (overlaps)
				return EINVAL;
		}

		/* Destination regions also exclude one another, so a transfer has no order-dependent output byte. */
		for (other = 0; other < index; other++) {
			other_region = &copy->regions[other];
			overlaps = intervals_overlap((uintptr_t)cpu[1] + region->dstOffset, region->size, (uintptr_t)cpu[1] + other_region->dstOffset, other_region->size);
			if (overlaps)
				return EINVAL;
		}
	}

	/* Succeeded: the complete immutable vector has exact live coherent non-overlapping inputs and outputs. */
	return 0;
}

/*
 * Copies exact coherent buffer bytes after preceding native completion while the controller mutex protects both bindings.
 */
int
bcm2711_vulkan_buffer_copy_run(
	const struct bcm2711_vulkan_record *record)
{
	const struct bcm2711_vulkan_buffer_copy *copy;
	const VkBufferCopy *region;
	struct bcm2711_vulkan_resource *resources[2];
	struct bcm2711_vulkan_resource *source;
	void *cpu[2];
	uint32_t index;
	int error;

	/* The pending primary retains both actual typed resource payloads across this synchronous FIFO action. */
	if (record == NULL ||
	    record->opcode != GPU_OP_CMD_COPY_BUFFER ||
	    record->objects[0] == NULL ||
	    record->objects[0]->kind != I915_VK_OBJ_BUFFER ||
	    record->objects[0]->payload == NULL)
		return EINVAL;
	copy = (const struct bcm2711_vulkan_buffer_copy *)record;
	source = record->objects[0]->payload;
	error = bcm2711_vulkan_buffer_copy_validate(record, source->device);
	if (error != 0)
		return error;

	/* Complete binding resolution is performed before any output; controller exclusion makes every checked address stable. */
	error = resolve_buffers(copy, source->device, resources, cpu);
	if (error != 0)
		return error;
	kern_io_read_barrier();

	/* The validated vector has no fallible operation after its first exact copied byte. */
	for (index = 0; index < record->count; index++) {
		region = &copy->regions[index];
		kern_memcpy((uint8_t *)cpu[1] + region->dstOffset, (const uint8_t *)cpu[0] + region->srcOffset, (size_t)region->size);
	}

	/* Normal uncached writes precede the following FIFO CPU read or supervised native GPU launch. */
	kern_io_write_barrier();

	/* Succeeded: only selected destination intervals changed, with no GPU launch or false DMA completion. */
	return 0;
}

/* Copies all actual typed IDs and standard 64-bit region fields before any complete semantic refusal is reported. */
static int
decode_copy(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct bcm2711_vulkan_buffer_copy *copy)
{
	uint64_t identity;
	uint64_t array;
	uint32_t index;

	/* Actual buffer IDs precede the declared count and its independently encoded array extent. */
	identity = drv_i915_wire_read_u64(reader);
	copy->record.objects[0] = bcm2711_vulkan_object_find(session, I915_VK_OBJ_BUFFER, identity);
	identity = drv_i915_wire_read_u64(reader);
	copy->record.objects[1] = bcm2711_vulkan_object_find(session, I915_VK_OBJ_BUFFER, identity);
	copy->record.count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (array != copy->record.count || array > BCM2711_VULKAN_BUFFER_COPY_REGIONS)
		return EINVAL;

	/* Each owned region retains exact byte offsets and size without native structure padding or a caller pointer. */
	for (index = 0; index < copy->record.count; index++) {
		copy->regions[index].srcOffset = drv_i915_wire_read_u64(reader);
		copy->regions[index].dstOffset = drv_i915_wire_read_u64(reader);
		copy->regions[index].size = drv_i915_wire_read_u64(reader);
	}

	/* Incomplete wire framing cannot leave a copied semantic command behind. */
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: the complete region vector may now be validated before acquiring either resource owner. */
	return 0;
}

/* Resolves two exact same-device transfer buffers to live Normal uncached backing without retaining application addresses. */
static int
resolve_buffers(
	const struct bcm2711_vulkan_buffer_copy *copy,
	struct bcm2711_vulkan_object *device,
	struct bcm2711_vulkan_resource *resources[2],
	void *cpu[2])
{
	struct bcm2711_vulkan_object *object;
	struct bcm2711_v3d_view *view;
	uint32_t address;
	uint32_t usage;
	uint32_t index;
	int error;

	/* A source needs transfer-source usage and a destination transfer-destination usage on its actual typed resource. */
	for (index = 0; index < 2; index++) {
		object = copy->record.objects[index];
		if (object == NULL ||
		    object->kind != I915_VK_OBJ_BUFFER ||
		    object->payload == NULL)
			return EINVAL;
		resources[index] = object->payload;
		usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		if (index != 0)
			usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		if (resources[index]->device != device ||
		    resources[index]->memory == NULL ||
		    (resources[index]->usage & usage) == 0)
			return EINVAL;

		/* Every byte belongs to a retained complete coherent logical binding rather than rounded allocation padding. */
		error = bcm2711_vulkan_resource_backing(resources[index], 0, resources[index]->bytes, &view, &address, &cpu[index]);
		if (error != 0)
			return error;
		if (!view->buffer->uncached)
			return ENOTSUP;
	}

	/* Succeeded: both independent typed edges supply the same stable coherent addresses throughout controller exclusion. */
	return 0;
}

/* Compares bounded absolute physical byte intervals without treating two public identities as disjoint storage. */
static bool
intervals_overlap(
	uintptr_t first,
	uint64_t first_bytes,
	uintptr_t second,
	uint64_t second_bytes)
{
	/* All absolute endpoints come from already validated complete live logical backing intervals. */
	if (first < second + second_bytes && second < first + first_bytes)
		return true;

	/* Succeeded: these two exact byte intervals share no physical destination/source location. */
	return false;
}

/* Releases only independently acquired resource edges and the complete copied CPU command after permitted retirement. */
static int
release_copy(
	struct bcm2711_vulkan_command_node *node)
{
	struct bcm2711_vulkan_buffer_copy *copy;
	uint32_t index;
	int first;
	int error;

	/* Acquired flags make both same-resource references and partial retention refusal unwind exactly once. */
	copy = (struct bcm2711_vulkan_buffer_copy *)node;
	first = 0;
	for (index = 0; index < 2; index++) {
		if (!copy->retained[index])
			continue;
		error = bcm2711_vulkan_object_release(copy->record.objects[index]);
		if (first == 0 && error != 0)
			first = error;
	}

	/* The CPU command retires even if native mapping teardown separately enters persistent quarantine. */
	kern_free(copy);
	if (first != 0)
		return first;

	/* Succeeded: neither complete nor unpublished copy metadata owns a resource edge. */
	return 0;
}
