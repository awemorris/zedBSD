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

/* One checked row sequence refers only to bytes inside a retained logical binding; offsets stay numerical until FIFO execution. */
struct copy_span {
	uint64_t offset;
	uint64_t pitch;
	uint64_t bytes;
	uint32_t rows;
};

static int prepare_spans(const struct bcm2711_vulkan_buffer_copy *copy, struct bcm2711_vulkan_resource *resources[2], uint32_t index, struct copy_span spans[2]);
static int validate_span(const struct copy_span *span, const struct bcm2711_vulkan_resource *resource);
static bool spans_overlap(uintptr_t first_base, const struct copy_span *first, uintptr_t second_base, const struct copy_span *second);
static enum i915_vk_object_kind resource_kind(uint32_t opcode, uint32_t index);
static void decode_image_region(struct i915_wire_reader *reader, VkBufferImageCopy *region);
static int decode_copy(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct bcm2711_vulkan_buffer_copy *copy);
static int resolve_resources(const struct bcm2711_vulkan_buffer_copy *copy, struct bcm2711_vulkan_object *device, struct bcm2711_vulkan_resource *resources[2], void *cpu[2]);
static int release_copy(struct bcm2711_vulkan_command_node *node);

/*
 * Records one complete reply-free buffer transfer with all byte or image regions and independently retained resources.
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

	/* The route handles core byte copies, image uploads and readback with their actual reply-free client framing. */
	(void)reply;
	*handled = 0;
	if (opcode != GPU_OP_CMD_COPY_BUFFER &&
	    opcode != GPU_OP_CMD_COPY_BUFFER_TO_IMAGE &&
	    opcode != GPU_OP_CMD_COPY_IMAGE_TO_BUFFER)
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
 * Validates a complete byte or image transfer and all physical source/destination exclusions before any byte changes.
 */
int
bcm2711_vulkan_buffer_copy_validate(
	const struct bcm2711_vulkan_record *record,
	struct bcm2711_vulkan_object *device)
{
	const struct bcm2711_vulkan_buffer_copy *copy;
	struct bcm2711_vulkan_resource *resources[2];
	struct copy_span spans[2];
	struct copy_span other_spans[2];
	void *cpu[2];
	uint32_t index;
	uint32_t other;
	bool overlaps;
	int error;

	/* A complete bounded copied command is the sole source of each transfer region. */
	if (record == NULL || device == NULL)
		return EINVAL;
	if (record->opcode != GPU_OP_CMD_COPY_BUFFER &&
	    record->opcode != GPU_OP_CMD_COPY_BUFFER_TO_IMAGE &&
	    record->opcode != GPU_OP_CMD_COPY_IMAGE_TO_BUFFER)
		return EINVAL;
	if (record->count == 0 || record->count > BCM2711_VULKAN_BUFFER_COPY_REGIONS)
		return EINVAL;
	copy = (const struct bcm2711_vulkan_buffer_copy *)record;
	error = resolve_resources(copy, device, resources, cpu);
	if (error != 0)
		return error;

	/* Both numerical row sequences of every complete region stay inside the exact logical bindings. */
	for (index = 0; index < record->count; index++) {
		error = prepare_spans(copy, resources, index, spans);
		if (error != 0)
			return error;
	}

	/* Every physical source row excludes every destination row, independently of public object identity or row padding. */
	for (index = 0; index < record->count; index++) {
		error = prepare_spans(copy, resources, index, spans);
		if (error != 0)
			return error;

		/* Compare this source with every independently checked destination region. */
		for (other = 0; other < record->count; other++) {
			error = prepare_spans(copy, resources, other, other_spans);
			if (error != 0)
				return error;
			overlaps = spans_overlap((uintptr_t)cpu[0], &spans[0], (uintptr_t)cpu[1], &other_spans[1]);
			if (overlaps)
				return EINVAL;
		}

		/* Buffer copies forbid multiple writes to one byte; buffer/image copies preserve their recorded region order. */
		if (record->opcode == GPU_OP_CMD_COPY_BUFFER) {
			/* Exclude each earlier destination without counting this region twice. */
			for (other = 0; other < index; other++) {
				error = prepare_spans(copy, resources, other, other_spans);
				if (error != 0)
					return error;
				overlaps = spans_overlap((uintptr_t)cpu[1], &spans[1], (uintptr_t)cpu[1], &other_spans[1]);
				if (overlaps)
					return EINVAL;
			}
		}
	}

	/* Succeeded: all complete byte and raster regions have live exact non-overlapping source and destination rows. */
	return 0;
}

/*
 * Copies selected coherent byte or raster rows after native retirement while preserving all unselected image and buffer padding.
 */
int
bcm2711_vulkan_buffer_copy_run(
	const struct bcm2711_vulkan_record *record)
{
	const struct bcm2711_vulkan_buffer_copy *copy;
	struct bcm2711_vulkan_resource *resources[2];
	struct bcm2711_vulkan_resource *source;
	struct copy_span *spans;
	void *cpu[2];
	uint32_t image_slot;
	uint32_t index;
	uint32_t row;
	uint64_t source_offset;
	uint64_t destination_offset;
	int error;

	/* Typed pending resources remain alive across the entire synchronous controller-protected transfer. */
	if (record == NULL ||
	    record->objects[0] == NULL ||
	    record->objects[0]->payload == NULL)
		return EINVAL;
	if (record->objects[0]->kind != I915_VK_OBJ_BUFFER && record->objects[0]->kind != I915_VK_OBJ_IMAGE)
		return EINVAL;
	copy = (const struct bcm2711_vulkan_buffer_copy *)record;
	source = record->objects[0]->payload;
	error = bcm2711_vulkan_buffer_copy_validate(record, source->device);
	if (error != 0)
		return error;

	/* Complete actual backing resolution precedes every FIFO-visible layout check and byte change. */
	error = resolve_resources(copy, source->device, resources, cpu);
	if (error != 0)
		return error;
	if (record->opcode != GPU_OP_CMD_COPY_BUFFER) {
		image_slot = 0;
		if (record->opcode == GPU_OP_CMD_COPY_BUFFER_TO_IMAGE)
			image_slot = 1;
		if (resources[image_slot]->layout != record->layout)
			return EINVAL;
	}

	/* A finite heap vector completes every fallible row calculation before the first output byte changes. */
	spans = kern_calloc(record->count * 2U, sizeof(*spans));
	if (spans == NULL)
		return ENOMEM;
	for (index = 0; index < record->count; index++) {
		error = prepare_spans(copy, resources, index, &spans[index * 2U]);
		if (error != 0) {
			kern_free(spans);
			return error;
		}
	}

	/* Preceding native completion and coherent host publication become visible before any selected source row is read. */
	kern_io_read_barrier();
	for (index = 0; index < record->count; index++) {
		/* Copy each equal selected row using the source and destination's independent exact numerical pitches. */
		source_offset = spans[index * 2U].offset;
		destination_offset = spans[index * 2U + 1U].offset;
		for (row = 0; row < spans[index * 2U].rows; row++) {
			kern_memcpy((uint8_t *)cpu[1] + destination_offset, (const uint8_t *)cpu[0] + source_offset, (size_t)spans[index * 2U].bytes);
			source_offset += spans[index * 2U].pitch;
			destination_offset += spans[index * 2U + 1U].pitch;
		}
	}

	/* Normal uncached output publication precedes the next FIFO CPU read or native launch; transient CPU metadata then retires. */
	kern_io_write_barrier();
	kern_free(spans);

	/* Succeeded: selected raw bytes changed with no channel conversion, native launch or false DMA completion. */
	return 0;
}

/* Copies all actual typed IDs, explicit layouts and opcode-selected standard region fields before semantic refusal. */
static int
decode_copy(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct bcm2711_vulkan_buffer_copy *copy)
{
	uint64_t identity;
	uint64_t array;
	uint32_t index;

	/* Source and destination kinds follow the opcode; the readback layout precedes its destination buffer identity. */
	identity = drv_i915_wire_read_u64(reader);
	copy->record.objects[0] = bcm2711_vulkan_object_find(session, resource_kind(copy->record.opcode, 0), identity);
	if (copy->record.opcode == GPU_OP_CMD_COPY_IMAGE_TO_BUFFER)
		copy->record.layout = drv_i915_wire_read_u32(reader);
	identity = drv_i915_wire_read_u64(reader);
	copy->record.objects[1] = bcm2711_vulkan_object_find(session, resource_kind(copy->record.opcode, 1), identity);
	if (copy->record.opcode == GPU_OP_CMD_COPY_BUFFER_TO_IMAGE)
		copy->record.layout = drv_i915_wire_read_u32(reader);
	copy->record.count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (array != copy->record.count || array > BCM2711_VULKAN_BUFFER_COPY_REGIONS)
		return EINVAL;

	/* Complete standard region metadata is copied independently of native padding or caller storage. */
	for (index = 0; index < copy->record.count; index++) {
		if (copy->record.opcode == GPU_OP_CMD_COPY_BUFFER) {
			copy->regions.buffers[index].srcOffset = drv_i915_wire_read_u64(reader);
			copy->regions.buffers[index].dstOffset = drv_i915_wire_read_u64(reader);
			copy->regions.buffers[index].size = drv_i915_wire_read_u64(reader);
		} else {
			decode_image_region(reader, &copy->regions.images[index]);
		}
	}

	/* Incomplete wire framing cannot leave a copied semantic command behind. */
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: the complete region vector may now be validated before acquiring either resource owner. */
	return 0;
}

/* Resolves exact same-device transfer resources to live Normal uncached backing without retaining application addresses. */
static int
resolve_resources(
	const struct bcm2711_vulkan_buffer_copy *copy,
	struct bcm2711_vulkan_object *device,
	struct bcm2711_vulkan_resource *resources[2],
	void *cpu[2])
{
	struct bcm2711_vulkan_object *object;
	struct bcm2711_v3d_view *view;
	uint32_t address;
	enum i915_vk_object_kind kind;
	uint32_t usage;
	uint32_t index;
	VkImageLayout layout;
	int error;

	/* A source needs transfer-source usage and a destination transfer-destination usage on its actual typed resource. */
	for (index = 0; index < 2; index++) {
		object = copy->record.objects[index];
		kind = resource_kind(copy->record.opcode, index);
		if (object == NULL ||
		    object->kind != kind ||
		    object->payload == NULL)
			return EINVAL;
		resources[index] = object->payload;
		usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		if (index != 0)
			usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

		/* An image uses its actual transfer flag and implemented raw four-byte colour format. */
		if (object->kind == I915_VK_OBJ_IMAGE) {
			usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
			if (index != 0)
				usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			if (resources[index]->format != VK_FORMAT_R8G8B8A8_UNORM && resources[index]->format != VK_FORMAT_B8G8R8A8_UNORM)
				return ENOTSUP;
		}

		/* Complete same-device binding and actual transfer permission precede any coherent pointer resolution. */
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

	/* Explicit layouts are validated independently of their FIFO execution-time current values. */
	if (copy->record.opcode != GPU_OP_CMD_COPY_BUFFER) {
		layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		if (copy->record.opcode == GPU_OP_CMD_COPY_BUFFER_TO_IMAGE) {
			layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		}

		/* One actual image side determines the required transfer layout; GENERAL is also valid for either direction. */
		if (copy->record.layout != VK_IMAGE_LAYOUT_GENERAL && copy->record.layout != layout)
			return EINVAL;
	}

	/* Succeeded: both independent typed edges supply the same stable coherent addresses throughout controller exclusion. */
	return 0;
}

/* Copies a complete standard buffer-image region without a native-struct or caller-pointer dependency. */
static void
decode_image_region(
	struct i915_wire_reader *reader,
	VkBufferImageCopy *region)
{
	/* Buffer placement precedes the complete colour subresource and signed image origin. */
	region->bufferOffset = drv_i915_wire_read_u64(reader);
	region->bufferRowLength = drv_i915_wire_read_u32(reader);
	region->bufferImageHeight = drv_i915_wire_read_u32(reader);

	/* The complete colour subresource selection is consumed even when its eventual semantics are refused. */
	region->imageSubresource.aspectMask = drv_i915_wire_read_u32(reader);
	region->imageSubresource.mipLevel = drv_i915_wire_read_u32(reader);
	region->imageSubresource.baseArrayLayer = drv_i915_wire_read_u32(reader);
	region->imageSubresource.layerCount = drv_i915_wire_read_u32(reader);

	/* Signed image origin is independent of the buffer's byte placement and row stride. */
	region->imageOffset.x = (int32_t)drv_i915_wire_read_u32(reader);
	region->imageOffset.y = (int32_t)drv_i915_wire_read_u32(reader);
	region->imageOffset.z = (int32_t)drv_i915_wire_read_u32(reader);

	/* The selected extent controls copied rows; it never includes trailing image or buffer padding. */
	region->imageExtent.width = drv_i915_wire_read_u32(reader);
	region->imageExtent.height = drv_i915_wire_read_u32(reader);
	region->imageExtent.depth = drv_i915_wire_read_u32(reader);

	/* Succeeded: complete semantic validation may occur after the entire immutable command was consumed. */
	return;
}

/* Selects the sole image side of a buffer-image transfer and leaves ordinary buffer copies fully typed as buffers. */
static enum i915_vk_object_kind
resource_kind(
	uint32_t opcode,
	uint32_t index)
{
	/* Readback's source and upload's destination are actual images; their other sides are actual buffers. */
	if (opcode == GPU_OP_CMD_COPY_IMAGE_TO_BUFFER && index == 0)
		return I915_VK_OBJ_IMAGE;
	if (opcode == GPU_OP_CMD_COPY_BUFFER_TO_IMAGE && index == 1)
		return I915_VK_OBJ_IMAGE;

	/* Succeeded: buffer kind covers both byte-copy sides and the selected buffer-image staging side. */
	return I915_VK_OBJ_BUFFER;
}

/* Builds exact numerical row intervals from complete copied metadata and actual logical image pitch. */
static int
prepare_spans(
	const struct bcm2711_vulkan_buffer_copy *copy,
	struct bcm2711_vulkan_resource *resources[2],
	uint32_t index,
	struct copy_span spans[2])
{
	const VkBufferImageCopy *image_region;
	const VkBufferCopy *buffer_region;
	struct bcm2711_vulkan_resource *image;
	uint32_t image_slot;
	uint32_t buffer_slot;
	uint32_t width;
	uint32_t height;
	uint32_t row_length;
	uint32_t image_height;
	int error;

	/* Exact byte copies have one row and no implicit padding on either side. */
	kern_memset(spans, 0, sizeof(*spans) * 2U);
	if (copy->record.opcode == GPU_OP_CMD_COPY_BUFFER) {
		buffer_region = &copy->regions.buffers[index];
		spans[0].offset = buffer_region->srcOffset;
		spans[1].offset = buffer_region->dstOffset;
		spans[0].bytes = buffer_region->size;
		spans[1].bytes = buffer_region->size;
		spans[0].rows = 1;
		spans[1].rows = 1;
	} else {
		/* The sole actual image side supplies the logical pitch and implemented single-subresource colour dimensions. */
		image_slot = 0;
		if (copy->record.opcode == GPU_OP_CMD_COPY_BUFFER_TO_IMAGE)
			image_slot = 1;
		buffer_slot = 1U - image_slot;
		image = resources[image_slot];
		image_region = &copy->regions.images[index];
		if (image_region->imageSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
		    image_region->imageSubresource.mipLevel != 0 ||
		    image_region->imageSubresource.baseArrayLayer != 0 ||
		    image_region->imageSubresource.layerCount != 1)
			return EINVAL;

		/* Two-dimensional origins and extents must contain actual nonempty rows wholly inside the image. */
		width = image_region->imageExtent.width;
		height = image_region->imageExtent.height;
		if (image_region->imageOffset.x < 0 ||
		    image_region->imageOffset.y < 0 ||
		    image_region->imageOffset.z != 0 ||
		    image_region->imageExtent.depth != 1 ||
		    width == 0 || height == 0)
			return EINVAL;
		if ((uint64_t)image_region->imageOffset.x + width > image->width ||
		    (uint64_t)image_region->imageOffset.y + height > image->height)
			return EINVAL;

		/* Zero strides mean packed extent; explicit row and layer strides cannot be smaller than the selected image region. */
		row_length = image_region->bufferRowLength;
		if (row_length == 0)
			row_length = width;
		image_height = image_region->bufferImageHeight;
		if (image_height == 0)
			image_height = height;
		if (row_length < width || image_height < height)
			return EINVAL;
		if ((uint64_t)row_length * 4U > 0x7fffffffULL || (image_region->bufferOffset & 3U) != 0)
			return EINVAL;

		/* The single layer touches only its selected rows; explicit image height introduces no trailing unused-row requirement. */
		spans[buffer_slot].offset = image_region->bufferOffset;
		spans[buffer_slot].pitch = (uint64_t)row_length * 4U;
		spans[image_slot].offset = (uint64_t)image_region->imageOffset.y * image->pitch + (uint64_t)image_region->imageOffset.x * 4U;
		spans[image_slot].pitch = image->pitch;
		spans[0].bytes = (uint64_t)width * 4U;
		spans[1].bytes = spans[0].bytes;
		spans[0].rows = height;
		spans[1].rows = height;
	}

	/* Both entire logical row sequences are bounded before either absolute CPU pointer or overlap comparison can use them. */
	error = validate_span(&spans[0], resources[0]);
	if (error != 0)
		return error;
	error = validate_span(&spans[1], resources[1]);
	if (error != 0)
		return error;

	/* Succeeded: both numerical sequences have equal exact selected byte widths and row counts. */
	return 0;
}

/* Bounds the final selected row with subtraction and division before computing any possibly overflowing pitch product. */
static int
validate_span(
	const struct copy_span *span,
	const struct bcm2711_vulkan_resource *resource)
{
	uint64_t remaining;

	/* Nonempty first-row placement is bounded by the exact logical resource rather than rounded storage. */
	if (span->rows == 0 ||
	    span->bytes == 0 ||
	    span->offset >= resource->bytes)
		return EINVAL;
	remaining = resource->bytes - span->offset;
	if (span->bytes > remaining)
		return EINVAL;
	remaining -= span->bytes;

	/* Multiple rows use a complete ascending pitch and leave the final selected row inside the same exact binding. */
	if (span->rows > 1) {
		if (span->pitch < span->bytes)
			return EINVAL;
		if ((uint64_t)(span->rows - 1U) > remaining / span->pitch)
			return EINVAL;
	}

	/* Succeeded: no row product or absolute byte addition can escape the retained complete logical resource. */
	return 0;
}

/* Compares ascending physical row lists in linear time while excluding only actual copied bytes, not unused padding. */
static bool
spans_overlap(
	uintptr_t first_base,
	const struct copy_span *first,
	uintptr_t second_base,
	const struct copy_span *second)
{
	uint64_t first_offset;
	uint64_t second_offset;
	uint32_t first_row;
	uint32_t second_row;
	uintptr_t first_start;
	uintptr_t second_start;

	/* Checked numerical placement supplies stable complete row lists under controller exclusion. */
	first_offset = first->offset;
	second_offset = second->offset;
	first_row = 0;
	second_row = 0;
	while (first_row < first->rows && second_row < second->rows) {
		first_start = first_base + first_offset;
		second_start = second_base + second_offset;
		if (first_start < second_start + second->bytes && second_start < first_start + first->bytes)
			return true;

		/* At least one disjoint complete row retires per comparison, even when the two bindings have different pitches. */
		if (first_start + first->bytes <= second_start) {
			first_row++;
			first_offset += first->pitch;
		} else {
			second_row++;
			second_offset += second->pitch;
		}
	}

	/* Succeeded: the selected row unions share no actual copied physical byte. */
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
