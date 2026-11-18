/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Exact coherent CPU byte transfers are FIFO work, distinct from GPU-rendered graphics and image meta passes. */
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/videocore/vk/vulkan-buffer-copy.h"
#include "drivers/gpu/videocore/vk/vulkan-resource.h"
#include "drivers/gpu/videocore/v3d-memory.h"
#if CONFIG_DRIVER_RPI4_GPU || CONFIG_DRIVER_RPI5_GPU
#include <kern/clock.h>
#include "drivers/gpu/videocore/vk/vulkan-video.h"
#endif
#if CONFIG_DRIVER_RPI4_GPU
#include "drivers/platform/rpi4/rpi4-mmal-wire.h"
#endif

/*
 * How a copy's texels change between the buffer and the image: not at all,
 * or a D24 image's depth (bits 8 to 31 of its word, the tile buffer's
 * d24s8 memory format) against the buffer's low 24 bits of a word, or a
 * D24S8 image's stencil (bits 0 to 7) against a buffer byte.
 */
enum copy_convert {
	COPY_RAW = 0,
	COPY_DEPTH24,
	COPY_STENCIL8
};

/* One checked row sequence refers only to bytes inside a retained logical binding; offsets stay numerical until FIFO execution. */
struct copy_span {
	uint64_t offset;
	uint64_t pitch;
	uint64_t bytes;
	uint32_t rows;
	enum copy_convert convert;
};

static int prepare_spans(const struct bcm2711_vulkan_buffer_copy *copy, struct bcm2711_vulkan_resource *resources[2], uint32_t index, uint32_t layer, struct copy_span spans[2]);
static uint32_t region_layers(const struct bcm2711_vulkan_buffer_copy *copy, uint32_t index);
static const struct bcm2711_vulkan_resource *copy_image(const struct bcm2711_vulkan_buffer_copy *copy);
static bool raw_depth(VkFormat format);
static bool d24_depth(VkFormat format);
static void convert_row(const struct copy_span *source, const struct copy_span *destination, const uint8_t *from, uint8_t *to, bool upload);
static int validate_span(const struct copy_span *span, const struct bcm2711_vulkan_resource *resource);
static bool spans_overlap(uintptr_t first_base, const struct copy_span *first, uintptr_t second_base, const struct copy_span *second);
static enum i915_vk_object_kind resource_kind(uint32_t opcode, uint32_t index);
static void decode_image_region(struct i915_wire_reader *reader, VkBufferImageCopy *region);
static int decode_copy(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct bcm2711_vulkan_buffer_copy *copy);
static int resolve_resources(const struct bcm2711_vulkan_buffer_copy *copy, struct bcm2711_vulkan_object *device, struct bcm2711_vulkan_resource *resources[2], void *cpu[2]);
static int release_copy(struct bcm2711_vulkan_command_node *node);
static bool written_box(const struct bcm2711_vulkan_buffer_copy *copy, uint32_t box[4]);
#if CONFIG_DRIVER_RPI4_GPU || CONFIG_DRIVER_RPI5_GPU
static int prepare_plane(const VkBufferImageCopy *region, const struct bcm2711_vulkan_resource *image, uint32_t buffer_slot, uint32_t image_slot, struct copy_span spans[2]);
#endif

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
		kern_logf("v3d: vk: command %u refused while recording (%d)\n", (unsigned)copy->record.opcode, error);
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
	uint32_t layer;
	uint32_t other_layer;
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

	/* Both numerical row sequences of every layer of every complete region stay inside the exact logical bindings. */
	for (index = 0; index < record->count; index++) {
		for (layer = 0; layer < region_layers(copy, index); layer++) {
			error = prepare_spans(copy, resources, index, layer, spans);
			if (error != 0)
				return error;
		}
	}

	/* Every physical source row excludes every destination row, independently of public object identity or row padding. */
	for (index = 0; index < record->count; index++) {
		for (layer = 0; layer < region_layers(copy, index); layer++) {
			error = prepare_spans(copy, resources, index, layer, spans);
			if (error != 0)
				return error;

			/* Compare this source with every independently checked destination region's layers. */
			for (other = 0; other < record->count; other++) {
				for (other_layer = 0; other_layer < region_layers(copy, other); other_layer++) {
					error = prepare_spans(copy, resources, other, other_layer, other_spans);
					if (error != 0)
						return error;
					overlaps = spans_overlap((uintptr_t)cpu[0], &spans[0], (uintptr_t)cpu[1], &other_spans[1]);
					if (overlaps)
						return EINVAL;
				}
			}

			/* Buffer copies forbid multiple writes to one byte; buffer/image copies preserve their recorded region order. */
			if (record->opcode == GPU_OP_CMD_COPY_BUFFER) {
				/* Exclude each earlier destination without counting this region twice. */
				for (other = 0; other < index; other++) {
					error = prepare_spans(copy, resources, other, 0, other_spans);
					if (error != 0)
						return error;
					overlaps = spans_overlap((uintptr_t)cpu[1], &spans[1], (uintptr_t)cpu[1], &other_spans[1]);
					if (overlaps)
						return EINVAL;
				}
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
	uint32_t level;
	uint32_t layer;
	uint32_t first_layer;
	uint32_t pairs;
	uint32_t pair;
	VkImageLayout current;
	uint32_t row;
	uint64_t source_offset;
	uint64_t destination_offset;
	struct bcm2711_v3d_view *view;
	uint32_t address;
	uint32_t box[4];
	void *destination;
	bool known;
#if CONFIG_DRIVER_RPI4_GPU || CONFIG_DRIVER_RPI5_GPU
	uint64_t started;
	bool cached;
	bool raw;
#endif
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

		/* Every layer of every region's mip level is in the layout the command names. */
		for (index = 0; index < record->count; index++) {
			level = copy->regions.images[index].imageSubresource.mipLevel;
			first_layer = copy->regions.images[index].imageSubresource.baseArrayLayer;
			for (layer = 0; layer < region_layers(copy, index); layer++) {
				current = bcm2711_vulkan_image_level_layout(resources[image_slot], level, first_layer + layer);
				if (current != record->layout)
					return EINVAL;
			}
		}
	}

	/* Each layer of each region is one pair of row sequences. */
	pairs = 0;
	for (index = 0; index < record->count; index++)
		pairs += region_layers(copy, index);

	/* A finite heap vector completes every fallible row calculation before the first output byte changes. */
	spans = kern_calloc(pairs * 2U, sizeof(*spans));
	if (spans == NULL)
		return ENOMEM;
	pair = 0;
	for (index = 0; index < record->count; index++) {
		for (layer = 0; layer < region_layers(copy, index); layer++) {
			error = prepare_spans(copy, resources, index, layer, &spans[pair * 2U]);
			if (error != 0) {
				kern_free(spans);
				return error;
			}
			pair++;
		}
	}

	/* Preceding native completion and coherent host publication become visible before any selected source row is read. */
	kern_io_read_barrier();
	for (index = 0; index < pairs; index++) {
		/* Copy each equal selected row using the source and destination's independent exact numerical pitches. */
		source_offset = spans[index * 2U].offset;
		destination_offset = spans[index * 2U + 1U].offset;
#if CONFIG_DRIVER_RPI4_GPU || CONFIG_DRIVER_RPI5_GPU
		/*
		 * A decoded NV12 picture's rows come from the decoder's cached
		 * copy of the frame when it still has one; the image's memory is
		 * uncached and slow to read.
		 */
		if (record->opcode == GPU_OP_CMD_COPY_IMAGE_TO_BUFFER &&
		    resources[0]->format == (VkFormat)BCM2711_VIDEO_FORMAT_NV12) {
			/* The rows from the cached copy, when it covers them. */
			started = clock_ticks();
			cached = bcm2711_vulkan_video_readback(
				resources[0],
				source_offset,
				spans[index * 2U].bytes,
				spans[index * 2U].rows,
				(uint8_t *)cpu[1] + destination_offset,
				spans[index * 2U + 1U].pitch);

			/*
			 * Else every row comes from the image; an image holding the
			 * firmware's samples as they are (a sampled one) is corrected
			 * on the way, as the cached copy is.
			 */
			raw = bcm2711_vulkan_video_image_raw(resources[0]);
			for (row = 0; !cached && row < spans[index * 2U].rows; row++) {
#if CONFIG_DRIVER_RPI4_GPU
				if (raw) {
					drv_rpi4_mmal_copy_samples((uint8_t *)cpu[1] + destination_offset, (const uint8_t *)cpu[0] + source_offset, (size_t)spans[index * 2U].bytes);
				} else {
					kern_memcpy((uint8_t *)cpu[1] + destination_offset, (const uint8_t *)cpu[0] + source_offset, (size_t)spans[index * 2U].bytes);
				}
#else
				/* The HEVC decoder's samples need no correction. */
				(void)raw;
				kern_memcpy((uint8_t *)cpu[1] + destination_offset, (const uint8_t *)cpu[0] + source_offset, (size_t)spans[index * 2U].bytes);
#endif

				/* The next row of each side. */
				source_offset += spans[index * 2U].pitch;
				destination_offset += spans[index * 2U + 1U].pitch;
			}

			/* The readback's time, for the video cost line. */
			bcm2711_vulkan_video_readback_done(cached, clock_ticks() - started);
			continue;
		}
#endif
		for (row = 0; row < spans[index * 2U].rows; row++) {
			if (spans[index * 2U].convert == COPY_RAW) {
				kern_memcpy((uint8_t *)cpu[1] + destination_offset, (const uint8_t *)cpu[0] + source_offset, (size_t)spans[index * 2U].bytes);
			} else {
				convert_row(&spans[index * 2U], &spans[index * 2U + 1U], (const uint8_t *)cpu[0] + source_offset, (uint8_t *)cpu[1] + destination_offset,
					    record->opcode == GPU_OP_CMD_COPY_BUFFER_TO_IMAGE);
			}
			source_offset += spans[index * 2U].pitch;
			destination_offset += spans[index * 2U + 1U].pitch;
		}
	}

	/* Normal uncached output publication precedes the next FIFO CPU read or native launch; transient CPU metadata then retires. */
	kern_io_write_barrier();
	kern_free(spans);

	/*
	 * The destination's snapshots, incremental copies and cursor images learn
	 * the written box of an image, or an unknown extent.  The backing resolved
	 * above; if it no longer does, the copy fails rather than leave changed
	 * bytes under an unchanged generation.
	 */
	error = bcm2711_vulkan_resource_backing(resources[1], 0, resources[1]->bytes, &view, &address, &destination);
	if (error != 0)
		return error;
	known = written_box(copy, box);
	if (known) {
		bcm2711_v3d_memory_written(view->buffer, address - view->address, box);
	} else {
		bcm2711_v3d_memory_written(view->buffer, address - view->address, NULL);
	}

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

		/*
		 * An image uses its actual transfer flag and a colour format, or a
		 * depth format whose depth is its texels' bytes (D16, D32_SFLOAT),
		 * whose texels are copied as their bytes, or a D24 one, whose depth
		 * and stencil are moved to and from their places in its words.
		 */
		if (object->kind == I915_VK_OBJ_IMAGE) {
			usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
			if (index != 0)
				usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;

			/* A multisampled image is not copied to or from a buffer. */
			if (resources[index]->samples != 1U)
				return ENOTSUP;
#if CONFIG_DRIVER_RPI4_GPU || CONFIG_DRIVER_RPI5_GPU
			/* A decoded NV12 picture may be read back, plane by plane. */
			if (resources[index]->format == (VkFormat)BCM2711_VIDEO_FORMAT_NV12 &&
			    copy->record.opcode == GPU_OP_CMD_COPY_IMAGE_TO_BUFFER) {
				/* Accepted. */
			} else if (bcm2711_vulkan_colour_format(resources[index]->format) == NULL &&
				   !raw_depth(resources[index]->format) &&
				   !d24_depth(resources[index]->format)) {
				return ENOTSUP;
			}
#else
			if (bcm2711_vulkan_colour_format(resources[index]->format) == NULL &&
			    !raw_depth(resources[index]->format) &&
			    !d24_depth(resources[index]->format))
				return ENOTSUP;
#endif
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
	uint32_t layer,
	struct copy_span spans[2])
{
	const VkBufferImageCopy *image_region;
	const VkBufferCopy *buffer_region;
	struct bcm2711_vulkan_resource *image;
	struct bcm2711_vulkan_image_level level;
	uint32_t image_slot;
	uint32_t buffer_slot;
	uint32_t width;
	uint32_t height;
	uint32_t row_length;
	uint32_t image_height;
	uint32_t layers;
	uint32_t slice;
	uint64_t layer_bytes;
	uint32_t texel;
	uint32_t buffer_texel;
	uint32_t aspect;
	enum copy_convert convert;
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
#if CONFIG_DRIVER_RPI4_GPU || CONFIG_DRIVER_RPI5_GPU
		/* An NV12 plane has its own size, texel and place. */
		if (image->format == (VkFormat)BCM2711_VIDEO_FORMAT_NV12) {
			error = prepare_plane(image_region, image, buffer_slot, image_slot, spans);
			if (error != 0)
				return error;
			error = validate_span(&spans[0], resources[0]);
			if (error != 0)
				return error;
			error = validate_span(&spans[1], resources[1]);
			if (error != 0)
				return error;
			return 0;
		}
#endif
		/*
		 * Colour, or the depth of a depth image (or a D24S8 image's
		 * stencil), in a run of the image's array layers (a 3D image's
		 * one, whose slices are checked below).
		 */
		layers = bcm2711_vulkan_image_layers(image);
		aspect = VK_IMAGE_ASPECT_COLOR_BIT;
		convert = COPY_RAW;
		if (raw_depth(image->format) || d24_depth(image->format))
			aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
		if (d24_depth(image->format))
			convert = COPY_DEPTH24;
		if (image->format == VK_FORMAT_D24_UNORM_S8_UINT && image_region->imageSubresource.aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT) {
			aspect = VK_IMAGE_ASPECT_STENCIL_BIT;
			convert = COPY_STENCIL8;
		}
		if (image_region->imageSubresource.aspectMask != aspect ||
		    image_region->imageSubresource.layerCount == 0 ||
		    image_region->imageSubresource.baseArrayLayer >= layers ||
		    image_region->imageSubresource.layerCount > layers - image_region->imageSubresource.baseArrayLayer)
			return EINVAL;
		if (layer >= region_layers(copy, index))
			return EINVAL;

		/* The selected mip level's own raster rows. */
		error = bcm2711_vulkan_image_level(image, image_region->imageSubresource.mipLevel, &level);
		if (error != 0)
			return error;

		/* Two-dimensional origins and extents must contain actual nonempty rows wholly inside the level. */
		width = image_region->imageExtent.width;
		height = image_region->imageExtent.height;
		if (image_region->imageOffset.x < 0 ||
		    image_region->imageOffset.y < 0 ||
		    image_region->imageOffset.z < 0 ||
		    image_region->imageExtent.depth == 0 ||
		    width == 0 || height == 0)
			return EINVAL;
		if ((uint64_t)image_region->imageOffset.x + width > level.width ||
		    (uint64_t)image_region->imageOffset.y + height > level.height)
			return EINVAL;

		/*
		 * A 3D image's slices from z, each a layer of the copy, lie in
		 * the level; any other image has the one slice.
		 */
		slice = 0;
		if (image->depth != 0) {
			if ((uint64_t)image_region->imageOffset.z + image_region->imageExtent.depth > bcm2711_vulkan_image_slices(image, image_region->imageSubresource.mipLevel))
				return EINVAL;
			slice = (uint32_t)image_region->imageOffset.z;
		} else if (image_region->imageOffset.z != 0 || image_region->imageExtent.depth != 1) {
			return EINVAL;
		}

		/* Zero strides mean packed extent; explicit row and layer strides cannot be smaller than the selected image region. */
		row_length = image_region->bufferRowLength;
		if (row_length == 0)
			row_length = width;
		image_height = image_region->bufferImageHeight;
		if (image_height == 0)
			image_height = height;
		if (row_length < width || image_height < height)
			return EINVAL;
		texel = image->texel_bytes;
		buffer_texel = texel;
		if (convert == COPY_STENCIL8)
			buffer_texel = 1U;
		if ((uint64_t)row_length * texel > 0x7fffffffULL || (image_region->bufferOffset % buffer_texel) != 0)
			return EINVAL;

		/*
		 * Each layer touches only its selected rows; in the buffer the
		 * layers follow one another, image height rows of row length apart
		 * (no trailing unused-row requirement after the last), in the image
		 * one layer stride apart.
		 */
		layer_bytes = (uint64_t)image_height * row_length * buffer_texel;
		spans[buffer_slot].offset = image_region->bufferOffset + layer_bytes * layer;
		spans[buffer_slot].pitch = (uint64_t)row_length * buffer_texel;
		spans[image_slot].offset = (uint64_t)(image_region->imageSubresource.baseArrayLayer + slice + layer) * image->layer_stride;
		spans[image_slot].offset += level.offset + (uint64_t)image_region->imageOffset.y * level.pitch + (uint64_t)image_region->imageOffset.x * texel;
		spans[image_slot].pitch = level.pitch;
		spans[image_slot].bytes = (uint64_t)width * texel;
		spans[buffer_slot].bytes = (uint64_t)width * buffer_texel;
		spans[0].rows = height;
		spans[1].rows = height;
		spans[0].convert = convert;
		spans[1].convert = convert;
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
	uint64_t limit;

	/* Nonempty first-row placement is bounded by the exact logical resource (with a mip-mapped image's levels) rather than rounded storage. */
	limit = resource->bytes;
	if (resource->storage_bytes > limit)
		limit = resource->storage_bytes;
	if (span->rows == 0 ||
	    span->bytes == 0 ||
	    span->offset >= limit)
		return EINVAL;
	remaining = limit - span->offset;
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

/* Reports the union of an upload's image regions; a buffer destination has no image box. */
static bool
written_box(
	const struct bcm2711_vulkan_buffer_copy *copy,
	uint32_t box[4])
{
	const VkBufferImageCopy *region;
	uint32_t index;

	/* Only uploads write an image. */
	if (copy->record.opcode != GPU_OP_CMD_COPY_BUFFER_TO_IMAGE)
		return false;

	/* A smaller mip level's or a later layer's rows lie outside level 0's box; such a write is of unknown extent. */
	for (index = 0; index < copy->record.count; index++) {
		if (copy->regions.images[index].imageSubresource.mipLevel != 0)
			return false;
		if (copy->regions.images[index].imageSubresource.baseArrayLayer != 0 ||
		    copy->regions.images[index].imageSubresource.layerCount != 1)
			return false;
		if (copy->regions.images[index].imageOffset.z != 0 ||
		    copy->regions.images[index].imageExtent.depth != 1)
			return false;
	}

	/* The union of every region's box. */
	box[0] = UINT32_MAX;
	box[1] = UINT32_MAX;
	box[2] = 0;
	box[3] = 0;
	for (index = 0; index < copy->record.count; index++) {
		region = &copy->regions.images[index];
		if ((uint32_t)region->imageOffset.x < box[0])
			box[0] = (uint32_t)region->imageOffset.x;
		if ((uint32_t)region->imageOffset.y < box[1])
			box[1] = (uint32_t)region->imageOffset.y;
		if ((uint32_t)region->imageOffset.x + region->imageExtent.width > box[2])
			box[2] = (uint32_t)region->imageOffset.x + region->imageExtent.width;
		if ((uint32_t)region->imageOffset.y + region->imageExtent.height > box[3])
			box[3] = (uint32_t)region->imageOffset.y + region->imageExtent.height;
	}

	/* Succeeded: the box covers every written pixel of the image. */
	return true;
}

/*
 * Reports how many array layers a region copies: one for a buffer copy, and
 * for an image region its layer count, or a 3D image's slices (its depth;
 * zero, refused later, counts as one).
 */
static uint32_t
region_layers(
	const struct bcm2711_vulkan_buffer_copy *copy,
	uint32_t index)
{
	const struct bcm2711_vulkan_resource *image;
	uint32_t layers;

	/* A buffer copy is one span pair. */
	if (copy->record.opcode == GPU_OP_CMD_COPY_BUFFER)
		return 1;

	/* An image region's layers, or a 3D image's slices, bounded by the most an image has. */
	layers = copy->regions.images[index].imageSubresource.layerCount;
	image = copy_image(copy);
	if (image != NULL && image->depth != 0)
		layers = copy->regions.images[index].imageExtent.depth;
	if (layers == 0 || layers > BCM2711_VULKAN_IMAGE_LAYERS)
		return 1;

	/* Succeeded: the region's layers. */
	return layers;
}

/* Finds the image of a buffer/image copy; NULL for a buffer copy or an image not yet resolved. */
static const struct bcm2711_vulkan_resource *
copy_image(
	const struct bcm2711_vulkan_buffer_copy *copy)
{
	const struct bcm2711_vulkan_object *object;

	/* The image is the source of a readback and the destination of an upload. */
	object = NULL;
	if (copy->record.opcode == GPU_OP_CMD_COPY_IMAGE_TO_BUFFER)
		object = copy->record.objects[0];
	else if (copy->record.opcode == GPU_OP_CMD_COPY_BUFFER_TO_IMAGE)
		object = copy->record.objects[1];
	if (object == NULL || object->kind != I915_VK_OBJ_IMAGE)
		return NULL;

	/* Succeeded: the image's description. */
	return object->payload;
}

#if CONFIG_DRIVER_RPI4_GPU || CONFIG_DRIVER_RPI5_GPU
/*
 * Prepares the rows of one NV12 plane region read back to a buffer: plane
 * 0 is one byte a texel over the whole picture, plane 1 two bytes (a Cb
 * and a Cr) over half the width and height, at the chroma plane's offset.
 */
static int
prepare_plane(
	const VkBufferImageCopy *region,
	const struct bcm2711_vulkan_resource *image,
	uint32_t buffer_slot,
	uint32_t image_slot,
	struct copy_span spans[2])
{
	uint64_t chroma;
	uint64_t bytes;
	uint64_t base;
	uint32_t pitch;
	uint32_t texel;
	uint32_t plane_width;
	uint32_t plane_height;
	uint32_t width;
	uint32_t height;
	uint32_t row_length;

	/* The plane: its texel, its size and where it starts. */
	bcm2711_vulkan_video_nv12_layout(image->width, image->height, &pitch, &chroma, &bytes);
	if (region->imageSubresource.aspectMask == VK_IMAGE_ASPECT_PLANE_0_BIT) {
		texel = 1U;
		plane_width = image->width;
		plane_height = image->height;
		base = 0;
	} else if (region->imageSubresource.aspectMask == VK_IMAGE_ASPECT_PLANE_1_BIT) {
		texel = 2U;
		plane_width = (image->width + 1U) / 2U;
		plane_height = (image->height + 1U) / 2U;
		base = chroma;
	} else {
		return EINVAL;
	}

	/* One level and layer, a nonempty region inside the plane. */
	width = region->imageExtent.width;
	height = region->imageExtent.height;
	if (region->imageSubresource.mipLevel != 0 ||
	    region->imageSubresource.baseArrayLayer != 0 ||
	    region->imageSubresource.layerCount != 1 ||
	    region->imageOffset.x < 0 ||
	    region->imageOffset.y < 0 ||
	    region->imageOffset.z != 0 ||
	    region->imageExtent.depth != 1 ||
	    width == 0 || height == 0)
		return EINVAL;
	if ((uint64_t)region->imageOffset.x + width > plane_width ||
	    (uint64_t)region->imageOffset.y + height > plane_height)
		return EINVAL;

	/* The buffer's rows: packed unless a longer row is given, aligned to the texel. */
	row_length = region->bufferRowLength;
	if (row_length == 0)
		row_length = width;
	if (row_length < width || (region->bufferOffset % texel) != 0)
		return EINVAL;

	/* The two row sequences. */
	spans[buffer_slot].offset = region->bufferOffset;
	spans[buffer_slot].pitch = (uint64_t)row_length * texel;
	spans[image_slot].offset = base + (uint64_t)region->imageOffset.y * pitch + (uint64_t)region->imageOffset.x * texel;
	spans[image_slot].pitch = pitch;
	spans[0].bytes = (uint64_t)width * texel;
	spans[1].bytes = spans[0].bytes;
	spans[0].rows = height;
	spans[1].rows = height;

	/* Succeeded: the plane's rows are bounded by the caller. */
	return 0;
}
#endif

/*
 * Tells whether a depth format's depth aspect is its texels' bytes as a
 * buffer has them (D16 and D32_SFLOAT; D24's depth is in the high bits of
 * the tile buffer's word and in the low bits of a buffer's).
 */
static bool
raw_depth(
	VkFormat format)
{
	/* The two formats. */
	if (format == VK_FORMAT_D16_UNORM || format == VK_FORMAT_D32_SFLOAT)
		return true;

	/* Any other. */
	return false;
}

/* Tells whether a format is a D24 one, whose depth is the high 24 bits of the tile buffer's word (X8_D24 and D24S8). */
static bool
d24_depth(
	VkFormat format)
{
	/* The two formats. */
	if (format == VK_FORMAT_X8_D24_UNORM_PACK32 || format == VK_FORMAT_D24_UNORM_S8_UINT)
		return true;

	/* Any other. */
	return false;
}

/*
 * Moves one row of a D24 image's depth or a D24S8 image's stencil between
 * the image's words and the buffer's texels, explicit bytes at a time.
 *
 * Upload writes the image (its other aspect's bits kept); readback writes
 * the buffer: the depth as the low 24 bits of a word (the high byte zero),
 * the stencil as a byte.
 */
static void
convert_row(
	const struct copy_span *source,
	const struct copy_span *destination,
	const uint8_t *from,
	uint8_t *to,
	bool upload)
{
	const uint8_t *texel;
	uint32_t pixels;
	uint32_t pixel;
	uint32_t word;
	uint32_t value;

	/* Readback: each image word's depth bits, or its stencil byte, into the buffer. */
	if (!upload) {
		pixels = (uint32_t)(source->bytes / 4U);
		for (pixel = 0; pixel < pixels; pixel++) {
			texel = from + pixel * 4U;
			word = (uint32_t)texel[0] | (uint32_t)texel[1] << 8 | (uint32_t)texel[2] << 16 | (uint32_t)texel[3] << 24;
			if (source->convert == COPY_STENCIL8) {
				to[pixel] = (uint8_t)word;
				continue;
			}
			value = word >> 8;
			to[pixel * 4U] = (uint8_t)value;
			to[pixel * 4U + 1U] = (uint8_t)(value >> 8);
			to[pixel * 4U + 2U] = (uint8_t)(value >> 16);
			to[pixel * 4U + 3U] = 0;
		}
		return;
	}

	/* Upload: the buffer's depth into bits 8 to 31, or its stencil into bits 0 to 7, of each image word, the other bits kept. */
	pixels = (uint32_t)(destination->bytes / 4U);
	for (pixel = 0; pixel < pixels; pixel++) {
		word = (uint32_t)to[pixel * 4U] | (uint32_t)to[pixel * 4U + 1U] << 8 | (uint32_t)to[pixel * 4U + 2U] << 16 | (uint32_t)to[pixel * 4U + 3U] << 24;
		if (source->convert == COPY_STENCIL8) {
			word = (word & 0xffffff00U) | from[pixel];
		} else {
			texel = from + pixel * 4U;
			value = (uint32_t)texel[0] | (uint32_t)texel[1] << 8 | (uint32_t)texel[2] << 16;
			word = (word & 0xffU) | (value << 8);
		}
		to[pixel * 4U] = (uint8_t)word;
		to[pixel * 4U + 1U] = (uint8_t)(word >> 8);
		to[pixel * 4U + 2U] = (uint8_t)(word >> 16);
		to[pixel * 4U + 3U] = (uint8_t)(word >> 24);
	}
}
