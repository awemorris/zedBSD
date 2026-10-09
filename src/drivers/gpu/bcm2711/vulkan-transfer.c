/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Complete copied transfer metadata owns typed inputs; layout-dependent reads wait until FIFO execution. */
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/vulkan-transfer.h"
#include "drivers/gpu/bcm2711/vulkan-resource.h"

static int decode_transfer(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct bcm2711_vulkan_transfer *transfer);
static void decode_subresource(struct i915_wire_reader *reader, struct bcm2711_vulkan_transfer *transfer);
static void decode_copy(struct i915_wire_reader *reader, struct bcm2711_vulkan_transfer *transfer, struct bcm2711_vulkan_transfer_region *region);
static void decode_blit(struct i915_wire_reader *reader, struct bcm2711_vulkan_transfer *transfer, struct bcm2711_vulkan_transfer_region *region);
static int validate_rectangle(const int32_t rectangle[4], const struct bcm2711_vulkan_resource *image, bool destination);
static int validate_overlaps(const struct bcm2711_vulkan_transfer *transfer, struct bcm2711_vulkan_resource *source, struct bcm2711_vulkan_resource *destination);
static bool rectangles_overlap(uintptr_t first_base, uint32_t first_pitch, const int32_t first[4], uintptr_t second_base, uint32_t second_pitch, const int32_t second[4]);
static void sampled_bounds(const struct bcm2711_vulkan_transfer *transfer, uint32_t region, const struct bcm2711_vulkan_resource *image, int32_t bounds[4]);
static int64_t floor_ratio(int64_t numerator, int64_t denominator);
static void rectangle_bounds(const int32_t rectangle[4], uint32_t bounds[4]);
static int release_transfer(struct bcm2711_vulkan_command_node *node);

/*
 * Records one complete reply-free image transfer and independently retains its exact source and destination.
 */
int
bcm2711_vulkan_transfer_dispatch(
	struct bcm2711_vulkan_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_command_buffer *command;
	struct bcm2711_vulkan_transfer *transfer;
	uint64_t identity;
	uint32_t index;
	int error;
	int released;

	/* The private route owns only these two actual reply-free core image operations. */
	(void)reply;
	*handled = 0;
	if (opcode != GPU_OP_CMD_COPY_IMAGE && opcode != GPU_OP_CMD_BLIT_IMAGE)
		return 0;
	*handled = 1;
	if (requested != 0)
		return EINVAL;

	/* Every copied transfer belongs to one exact non-pending recording primary. */
	identity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_COMMAND_BUFFER, identity);
	if (object == NULL)
		return EINVAL;
	command = object->payload;
	if (command->pending != 0 || command->state != BCM2711_VULKAN_COMMAND_RECORDING)
		return EBUSY;

	/* The whole bounded region vector stays on the heap rather than the kernel stack. */
	transfer = kern_calloc(1, sizeof(*transfer));
	if (transfer == NULL)
		return ENOMEM;
	transfer->record.opcode = opcode;
	transfer->record.node.release = release_transfer;
	error = decode_transfer(session, reader, transfer);
	if (error != 0) {
		kern_free(transfer);
		return error;
	}

	/* A prior void failure still consumes the complete actual client command without acquiring new typed edges. */
	if (command->recording_error != 0) {
		kern_free(transfer);
		return 0;
	}

	/* Complete semantics, pass nesting and byte budget precede either independently retained image edge. */
	error = bcm2711_vulkan_transfer_validate(transfer, command->owner.device);
	if (error == 0 && command->render_open)
		error = EINVAL;
	if (error == 0 && sizeof(*transfer) > BCM2711_VULKAN_RECORD_BYTES - command->recorded_bytes)
		error = ENOMEM;
	for (index = 0; index < 2 && error == 0; index++) {
		error = bcm2711_vulkan_object_retain(transfer->record.objects[index]);
		if (error == 0)
			transfer->retained[index] = true;
	}

	/* A refused complete void operation reports its first outcome through End, with no partial recorded owner. */
	if (error != 0) {
		command->recording_error = error;
		released = release_transfer(&transfer->record.node);
		if (released != 0)
			return released;
		return 0;
	}

	/* Publication appends one complete independently owned event to the same finite primary order. */
	if (command->last == NULL)
		command->first = &transfer->record.node;
	else
		command->last->next = &transfer->record.node;
	command->last = &transfer->record.node;
	command->recorded_bytes += sizeof(*transfer);

	/* Succeeded: no caller range, colour format or image identity can replace the retained immutable transfer input graph. */
	return 0;
}

/*
 * Validates complete immutable image transfer interfaces without comparing execution-time layout state.
 */
int
bcm2711_vulkan_transfer_validate(
	const struct bcm2711_vulkan_transfer *transfer,
	struct bcm2711_vulkan_object *device)
{
	struct bcm2711_vulkan_resource *images[2];
	const struct bcm2711_vulkan_transfer_region *region;
	uint32_t index;
	int error;

	/* Complete semantic refusal is distinct from framing failure and cannot produce any native work. */
	if (transfer == NULL || device == NULL)
		return EINVAL;
	if (transfer->record.opcode != GPU_OP_CMD_COPY_IMAGE && transfer->record.opcode != GPU_OP_CMD_BLIT_IMAGE)
		return EINVAL;
	if (transfer->record.semantic_error != 0)
		return transfer->record.semantic_error;
	if (transfer->record.count == 0 || transfer->record.count > BCM2711_VULKAN_TRANSFER_REGIONS)
		return EINVAL;

	/* Both exact typed images have complete same-device bindings and implemented four-byte UNORM texels. */
	for (index = 0; index < 2; index++) {
		if (transfer->record.objects[index] == NULL || transfer->record.objects[index]->kind != I915_VK_OBJ_IMAGE)
			return EINVAL;
		images[index] = transfer->record.objects[index]->payload;
		if (images[index] == NULL ||
		    images[index]->device != device ||
		    images[index]->memory == NULL)
			return EINVAL;
		if (images[index]->format != VK_FORMAT_R8G8B8A8_UNORM && images[index]->format != VK_FORMAT_B8G8R8A8_UNORM)
			return ENOTSUP;
	}

	/* Source and destination permissions remain the application's actual transfer usages, independent of internal texture sampling. */
	if ((images[0]->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0 || (images[1]->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0)
		return EINVAL;
	if (transfer->source_layout != VK_IMAGE_LAYOUT_GENERAL && transfer->source_layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
		return EINVAL;
	if (transfer->destination_layout != VK_IMAGE_LAYOUT_GENERAL && transfer->destination_layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
		return EINVAL;
	if (images[0] == images[1] &&
	    (transfer->source_layout != VK_IMAGE_LAYOUT_GENERAL ||
	    transfer->destination_layout != VK_IMAGE_LAYOUT_GENERAL))
		return EINVAL;

	/* Equal destination endpoints are invalid; equal source endpoints remain legitimate constant-coordinate blits. */
	if (transfer->filter != VK_FILTER_NEAREST && transfer->filter != VK_FILTER_LINEAR)
		return ENOTSUP;
	for (index = 0; index < transfer->record.count; index++) {
		/* The actual complete image bounds validate both signed boxes before any alias calculation. */
		error = validate_rectangle(transfer->regions[index].source, images[0], false);
		if (error != 0)
			return error;
		error = validate_rectangle(transfer->regions[index].destination, images[1], true);
		if (error != 0)
			return error;

		/* Copies preserve equal ascending source/destination extents and use no filtered channel conversion. */
		region = &transfer->regions[index];
		if (transfer->record.opcode == GPU_OP_CMD_COPY_IMAGE) {
			if (transfer->filter != VK_FILTER_NEAREST)
				return EINVAL;
			if (region->source[2] <= region->source[0] || region->source[3] <= region->source[1])
				return EINVAL;
			if (region->source[2] - region->source[0] != region->destination[2] - region->destination[0] ||
			    region->source[3] - region->source[1] != region->destination[3] - region->destination[1])
				return EINVAL;
		}
	}

	/* Aliased physical image bindings obey all source/destination exclusions and copy destination/destination exclusion. */
	error = validate_overlaps(transfer, images[0], images[1]);
	if (error != 0)
		return error;

	/* Succeeded: each copied rectangle can lower independently after preceding queue writes and transitions retire. */
	return 0;
}

/* Copies every actual client field before a late ordinary semantic refusal can be retained for End. */
static int
decode_transfer(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct bcm2711_vulkan_transfer *transfer)
{
	uint64_t identity;
	uint64_t array;
	uint32_t index;

	/* Typed IDs and declared layouts precede the complete finite actual region array. */
	identity = drv_i915_wire_read_u64(reader);
	transfer->record.objects[0] = bcm2711_vulkan_object_find(session, I915_VK_OBJ_IMAGE, identity);
	transfer->source_layout = drv_i915_wire_read_u32(reader);
	identity = drv_i915_wire_read_u64(reader);
	transfer->record.objects[1] = bcm2711_vulkan_object_find(session, I915_VK_OBJ_IMAGE, identity);
	transfer->destination_layout = drv_i915_wire_read_u32(reader);
	transfer->record.count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (array != transfer->record.count || array > BCM2711_VULKAN_TRANSFER_REGIONS)
		return EINVAL;

	/* Copy and blit have independent exact standard element framing, with no native padding or application pointer. */
	for (index = 0; index < transfer->record.count; index++) {
		if (transfer->record.opcode == GPU_OP_CMD_COPY_IMAGE)
			decode_copy(reader, transfer, &transfer->regions[index]);
		else
			decode_blit(reader, transfer, &transfer->regions[index]);
	}

	/* Copy always samples nearest raw channels; blit retains the filter following its entire region array. */
	transfer->filter = VK_FILTER_NEAREST;
	if (transfer->record.opcode == GPU_OP_CMD_BLIT_IMAGE)
		transfer->filter = drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: all complete semantic errors stay on this unpublished copied node. */
	return 0;
}

/* Consumes one complete standard single-colour, single-mip, single-layer subresource selection. */
static void
decode_subresource(
	struct i915_wire_reader *reader,
	struct bcm2711_vulkan_transfer *transfer)
{
	uint32_t aspect;
	uint32_t mip;
	uint32_t layer;
	uint32_t count;

	/* Invalid selections still consume all four standard words before the next field. */
	aspect = drv_i915_wire_read_u32(reader);
	mip = drv_i915_wire_read_u32(reader);
	layer = drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u32(reader);
	if (aspect != VK_IMAGE_ASPECT_COLOR_BIT ||
	    mip != 0 ||
	    layer != 0 ||
	    count != 1)
		transfer->record.semantic_error = EINVAL;

	/* Succeeded: the entire selection was consumed even when its semantics were refused. */
	return;
}

/* Converts one complete image copy's origin and extent to owned directed endpoint rectangles. */
static void
decode_copy(
	struct i915_wire_reader *reader,
	struct bcm2711_vulkan_transfer *transfer,
	struct bcm2711_vulkan_transfer_region *region)
{
	int32_t source_z;
	int32_t destination_z;
	uint32_t width;
	uint32_t height;
	uint32_t depth;
	int64_t source_x;
	int64_t source_y;
	int64_t destination_x;
	int64_t destination_y;

	/* Actual source subresource and signed offsets precede the distinct destination selection. */
	decode_subresource(reader, transfer);
	region->source[0] = (int32_t)drv_i915_wire_read_u32(reader);
	region->source[1] = (int32_t)drv_i915_wire_read_u32(reader);
	source_z = (int32_t)drv_i915_wire_read_u32(reader);
	decode_subresource(reader, transfer);
	region->destination[0] = (int32_t)drv_i915_wire_read_u32(reader);
	region->destination[1] = (int32_t)drv_i915_wire_read_u32(reader);
	destination_z = (int32_t)drv_i915_wire_read_u32(reader);
	width = drv_i915_wire_read_u32(reader);
	height = drv_i915_wire_read_u32(reader);
	depth = drv_i915_wire_read_u32(reader);

	/* Arithmetic stays wide until every standard nonempty two-dimensional extent is representable. */
	source_x = (int64_t)region->source[0] + width;
	source_y = (int64_t)region->source[1] + height;
	destination_x = (int64_t)region->destination[0] + width;
	destination_y = (int64_t)region->destination[1] + height;
	if (source_z != 0 ||
	    destination_z != 0 ||
	    depth != 1 ||
	    width == 0 ||
	    height == 0 ||
	    source_x > 0x7fffffffLL ||
	    source_y > 0x7fffffffLL ||
	    destination_x > 0x7fffffffLL ||
	    destination_y > 0x7fffffffLL) {
		transfer->record.semantic_error = EINVAL;
		return;
	}

	/* All eventual copied endpoints retain exact texel coordinates rather than narrowing a wrapped sum. */
	region->source[2] = (int32_t)source_x;
	region->source[3] = (int32_t)source_y;
	region->destination[2] = (int32_t)destination_x;
	region->destination[3] = (int32_t)destination_y;

	/* Succeeded: both copy boxes have equal exact nonempty extents. */
	return;
}

/* Copies one complete directed blit box while keeping both axis reversals independent. */
static void
decode_blit(
	struct i915_wire_reader *reader,
	struct bcm2711_vulkan_transfer *transfer,
	struct bcm2711_vulkan_transfer_region *region)
{
	int32_t source_z[2];
	int32_t destination_z[2];
	uint32_t index;
	uint64_t array;

	/* Each actual two-dimensional colour subresource has two signed three-coordinate corners. */
	decode_subresource(reader, transfer);
	array = drv_i915_wire_read_u64(reader);
	if (array != 2) {
		reader->error = EINVAL;
		return;
	}

	/* Copy only the two completely framed signed coordinate triplets. */
	for (index = 0; index < 2; index++) {
		region->source[index * 2U] = (int32_t)drv_i915_wire_read_u32(reader);
		region->source[index * 2U + 1U] = (int32_t)drv_i915_wire_read_u32(reader);
		source_z[index] = (int32_t)drv_i915_wire_read_u32(reader);
	}

	/* The destination retains its own directed endpoints, rather than inheriting source orientation. */
	decode_subresource(reader, transfer);
	array = drv_i915_wire_read_u64(reader);
	if (array != 2) {
		reader->error = EINVAL;
		return;
	}

	/* Copy only the two completely framed signed coordinate triplets. */
	for (index = 0; index < 2; index++) {
		region->destination[index * 2U] = (int32_t)drv_i915_wire_read_u32(reader);
		region->destination[index * 2U + 1U] = (int32_t)drv_i915_wire_read_u32(reader);
		destination_z[index] = (int32_t)drv_i915_wire_read_u32(reader);
	}

	/* Single-layer two-dimensional images require the standard zero-to-one Z interval. */
	if (source_z[0] != 0 ||
	    source_z[1] != 1 ||
	    destination_z[0] != 0 ||
	    destination_z[1] != 1)
		transfer->record.semantic_error = EINVAL;

	/* Succeeded: both independently framed signed boxes and layer intervals were consumed. */
	return;
}

/* Checks both endpoints against actual image dimensions without rejecting valid source-axis reversals or constant sampling. */
static int
validate_rectangle(
	const int32_t rectangle[4],
	const struct bcm2711_vulkan_resource *image,
	bool destination)
{
	uint32_t index;

	/* Every signed endpoint lies in the image's complete legal texel box. */
	for (index = 0; index < 4; index++) {
		if (rectangle[index] < 0)
			return EINVAL;
		if ((index & 1U) == 0 && (uint32_t)rectangle[index] > image->width)
			return EINVAL;
		if ((index & 1U) != 0 && (uint32_t)rectangle[index] > image->height)
			return EINVAL;
	}

	/* A destination always contains actual texels; a source may have constant coordinates along either axis. */
	if (destination &&
	    (rectangle[0] == rectangle[2] ||
	    rectangle[1] == rectangle[3]))
		return EINVAL;

	/* Succeeded: copied endpoints have no negative, wrapped or out-of-image coordinate. */
	return 0;
}

/* Checks exact raster row intervals so legal disjoint aliases do not require a speculative whole-image exclusion. */
static int
validate_overlaps(
	const struct bcm2711_vulkan_transfer *transfer,
	struct bcm2711_vulkan_resource *source,
	struct bcm2711_vulkan_resource *destination)
{
	struct bcm2711_v3d_view *view;
	void *source_cpu;
	void *destination_cpu;
	uint32_t address;
	uint32_t index;
	uint32_t other;
	int32_t sampled[4];
	bool overlaps;
	int error;

	/* Actual coherent aliases identify physical placement independently of public memory IDs or GPU virtual aliases. */
	error = bcm2711_vulkan_resource_backing(source, 0, source->bytes, &view, &address, &source_cpu);
	if (error != 0)
		return error;
	error = bcm2711_vulkan_resource_backing(destination, 0, destination->bytes, &view, &address, &destination_cpu);
	if (error != 0)
		return error;

	/* Copy destination regions cannot overlap; blit destinations retain their recorded execution order. */
	for (index = 0; index < transfer->record.count; index++) {
		for (other = 0; other < index && transfer->record.opcode == GPU_OP_CMD_COPY_IMAGE; other++) {
			overlaps = rectangles_overlap((uintptr_t)destination_cpu, destination->pitch, transfer->regions[index].destination, (uintptr_t)destination_cpu, destination->pitch, transfer->regions[other].destination);
			if (overlaps)
				return EINVAL;
		}

		/* Filter footprints include constant-coordinate source boxes and edge-clamped neighbours beyond a linear source box. */
		sampled_bounds(transfer, index, source, sampled);

		/* Every sampled source region is excluded from every destination, rather than checking only matched pairs. */
		for (other = 0; other < transfer->record.count; other++) {
			overlaps = rectangles_overlap((uintptr_t)source_cpu, source->pitch, sampled, (uintptr_t)destination_cpu, destination->pitch, transfer->regions[other].destination);
			if (overlaps)
				return EINVAL;
		}
	}

	/* Succeeded: the whole transfer cannot overwrite a later region's copied source bytes. */
	return 0;
}

/* Derives the actual affine sample footprint of destination pixel centres, including linear neighbours and image-edge clamping. */
static void
sampled_bounds(
	const struct bcm2711_vulkan_transfer *transfer,
	uint32_t region_index,
	const struct bcm2711_vulkan_resource *image,
	int32_t bounds[4])
{
	const struct bcm2711_vulkan_transfer_region *region;
	uint32_t source[4];
	uint32_t destination[4];
	uint32_t extent;
	uint32_t axis;
	int64_t length;
	int64_t delta;
	int64_t denominator;
	int64_t first;
	int64_t last;

	/* Copy uses exact texel rectangles, with no interpolated or filtered neighbouring read. */
	region = &transfer->regions[region_index];
	if (transfer->record.opcode == GPU_OP_CMD_COPY_IMAGE) {
		kern_memcpy(bounds, region->source, sizeof(region->source));
		return;
	}

	/* Reversals change sample order but do not change the union of possible texels along either axis. */
	rectangle_bounds(region->source, source);
	rectangle_bounds(region->destination, destination);
	for (axis = 0; axis < 2; axis++) {
		extent = image->width;
		if (axis != 0)
			extent = image->height;
		length = destination[axis + 2U] - destination[axis];
		delta = source[axis + 2U] - source[axis];
		denominator = length * 2;
		first = (int64_t)source[axis] * denominator + delta;
		last = (int64_t)source[axis + 2U] * denominator - delta;

		/* Linear sampling selects the neighbours surrounding a texel centre; nearest selects the texel containing the coordinate. */
		if (transfer->filter == VK_FILTER_LINEAR) {
			/* Exact boundary ties conservatively include the lower neighbour selected by permitted coordinate rounding. */
			last = floor_ratio(last - length, denominator) + 1;
			if ((first - length) % denominator == 0)
				first -= denominator;
			first = floor_ratio(first - length, denominator);
		} else {
			last = floor_ratio(last, denominator);
			if (first % denominator == 0)
				first -= denominator;
			first = floor_ratio(first, denominator);
		}

		/* Internal CLAMP_TO_EDGE agrees with Vulkan image-edge behaviour, including constant coordinates at either edge. */
		if (first < 0)
			first = 0;
		if (first >= extent)
			first = extent - 1U;
		if (last < 0)
			last = 0;
		if (last >= extent)
			last = extent - 1U;
		bounds[axis] = (int32_t)first;
		bounds[axis + 2U] = (int32_t)last + 1;
	}

	/* Succeeded: half-open sampled bounds include every possible input texel of the private texture quad. */
	return;
}

/* Rounds a bounded signed rational down rather than using C's truncation toward zero. */
static int64_t
floor_ratio(
	int64_t numerator,
	int64_t denominator)
{
	int64_t result;

	/* Only linear sampling at the lower image edge can have a negative numerator. */
	result = numerator / denominator;
	if (numerator < 0 && numerator % denominator != 0)
		result--;

	/* Succeeded: the index selects the lower texel neighbour on either side of zero. */
	return result;
}

/* Compares two ordered raster row interval lists with linear progress and no per-texel loop. */
static bool
rectangles_overlap(
	uintptr_t first_base,
	uint32_t first_pitch,
	const int32_t first[4],
	uintptr_t second_base,
	uint32_t second_pitch,
	const int32_t second[4])
{
	uint32_t a[4];
	uint32_t b[4];
	uintptr_t first_start;
	uintptr_t first_end;
	uintptr_t second_start;
	uintptr_t second_end;
	uint32_t first_row;
	uint32_t second_row;

	/* Directed corners become row-major half-open raster boxes only for alias exclusion. */
	rectangle_bounds(first, a);
	rectangle_bounds(second, b);
	first_row = a[1];
	second_row = b[1];
	while (first_row < a[3] && second_row < b[3]) {
		first_start = first_base + (uint64_t)first_row * first_pitch + a[0] * 4U;
		first_end = first_base + (uint64_t)first_row * first_pitch + a[2] * 4U;
		second_start = second_base + (uint64_t)second_row * second_pitch + b[0] * 4U;
		second_end = second_base + (uint64_t)second_row * second_pitch + b[2] * 4U;
		if (first_start < second_end && second_start < first_end)
			return true;

		/* At least one complete row retires per comparison, including disjoint physical allocations. */
		if (first_end <= second_start)
			first_row++;
		else
			second_row++;
	}

	/* Succeeded: no byte interval belongs to both raster region lists. */
	return false;
}

/* Copies normalized endpoint bounds without changing the immutable direction used for sampling. */
static void
rectangle_bounds(
	const int32_t rectangle[4],
	uint32_t bounds[4])
{
	uint32_t axis;

	/* Each independent axis selects its own ascending half-open pair. */
	for (axis = 0; axis < 2; axis++) {
		bounds[axis] = (uint32_t)rectangle[axis];
		bounds[axis + 2U] = (uint32_t)rectangle[axis + 2U];
		if (bounds[axis] > bounds[axis + 2U]) {
			bounds[axis] = (uint32_t)rectangle[axis + 2U];
			bounds[axis + 2U] = (uint32_t)rectangle[axis];
		}
	}

	/* Succeeded: each normalized axis retains the same complete unordered endpoint pair. */
	return;
}

/* Releases only successfully acquired typed edges before retiring the copied whole transfer node. */
static int
release_transfer(
	struct bcm2711_vulkan_command_node *node)
{
	struct bcm2711_vulkan_transfer *transfer;
	uint32_t index;
	int first;
	int error;

	/* The common prefix owns the entire allocation and its exact acquired input edge flags. */
	transfer = (struct bcm2711_vulkan_transfer *)node;
	first = 0;
	for (index = 0; index < 2; index++) {
		if (!transfer->retained[index])
			continue;
		error = bcm2711_vulkan_object_release(transfer->record.objects[index]);
		if (first == 0 && error != 0)
			first = error;
	}

	/* An ordinary native mapping teardown refusal never strands the CPU metadata owner. */
	kern_free(transfer);
	if (first != 0)
		return first;

	/* Succeeded: the whole immutable transfer owns no input or recording storage. */
	return 0;
}
