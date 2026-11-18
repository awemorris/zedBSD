/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Buffer and raster colour-image requirements remain immutable through memory, resource and prepared-job retirement. */
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/videocore/vk/vulkan-memory.h"
#include "drivers/gpu/videocore/vk/vulkan-record.h"
#include "drivers/gpu/videocore/vk/vulkan-resource.h"
#include "drivers/gpu/videocore/v3d-memory.h"
#include "drivers/gpu/videocore/native-pass.h"
#include "drivers/gpu/videocore/bcm2711-private.h"
#include "drivers/gpu/videocore/render-device.h"

#if CONFIG_DRIVER_RPI4_GPU || CONFIG_DRIVER_RPI5_GPU
#include "drivers/gpu/videocore/vk/vulkan-video.h"
#endif

/* Existing generated record decoding is device-independent Zlib code, without a Gen12 resource implementation. */
#include "drivers/gpu/i915/vk/vk-codec.inc"

/* The queried native subset has one allocation type, one sample and 4096-square colour images of at most this many bytes. */
#define VULKAN_RESOURCE_BYTES (256ULL << 20)

/*
 * A buffer may declare every core usage (GL's buffers declare them all at
 * once); a use the device lacks (texel and storage descriptors, indirect
 * draws) is refused where it is bound or recorded, not here.
 */
#define VULKAN_BUFFER_USAGE (VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | \
			     VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT)
#define VULKAN_IMAGE_USAGE (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | \
			    VK_IMAGE_USAGE_STORAGE_BIT)

/*
 * A depth/stencil image is used as an attachment, transient or not; the
 * tile buffer loads and stores it.  Its depth may be sampled.  Transfer
 * usage is accepted: such an image is cleared, copied to and from buffers
 * and images of its format, and blitted unscaled.
 */
#define VULKAN_DEPTH_USAGE (VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT | \
			    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)

/* The usage a multisampled image may have: an attachment, copied and resolved, and sampled a sample at a time. */
#define VULKAN_MULTISAMPLE_USAGE (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | \
				  VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | \
				  VK_IMAGE_USAGE_SAMPLED_BIT)

/* The create flags a two-plane picture may carry so its planes can be viewed in their own formats. */
#define VULKAN_IMAGE_PLANE_FLAGS (VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT)

/* The create flags an image may carry: a picture's, and a colour image's cube views. */
#define VULKAN_IMAGE_FLAGS (VULKAN_IMAGE_PLANE_FLAGS | VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)

static int create_buffer(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int create_image(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int volume_shape(const struct bcm2711_vulkan_session *session, const VkImageCreateInfo *info, const struct bcm2711_vulkan_colour_format *colour);
static int publish_resource(struct bcm2711_vulkan_session *session, enum i915_vk_object_kind kind, struct bcm2711_vulkan_resource *description, uint32_t layouts, uint64_t device_id, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int release_resource(struct bcm2711_vulkan_session *session, void *payload);
static int destroy_resource(struct bcm2711_vulkan_session *session, enum i915_vk_object_kind kind, struct i915_wire_reader *reader);
static int memory_requirements(struct bcm2711_vulkan_session *session, enum i915_vk_object_kind kind, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int bind_memory(struct bcm2711_vulkan_session *session, enum i915_vk_object_kind kind, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int image_layout(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static struct bcm2711_vulkan_resource *find_resource(struct bcm2711_vulkan_session *session, enum i915_vk_object_kind kind, uint64_t device_id, uint64_t identity);
static int external_declaration(struct i915_wire_reader *reader, uint32_t expected);
static int sharing_mode(VkSharingMode mode, uint32_t count, const uint32_t *families);
static void creation_reply(struct i915_wire_writer *reply, VkResult status, uint64_t identity);

/*
 * Routes native storage description, requirements, exact memory binding and colour-image layout commands.
 */
int
bcm2711_vulkan_resource_dispatch(
	struct bcm2711_vulkan_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	/* Only implemented typed operations consume a command; other resources remain another router's responsibility. */
	*handled = 1;
	switch (opcode) {
	case GPU_OP_CREATE_BUFFER:
	case GPU_OP_CREATE_IMAGE:
	case GPU_OP_DESTROY_BUFFER:
	case GPU_OP_DESTROY_IMAGE:
	case GPU_OP_GET_BUFFER_MEMORY_REQUIREMENTS:
	case GPU_OP_GET_IMAGE_MEMORY_REQUIREMENTS:
	case GPU_OP_BIND_BUFFER_MEMORY:
	case GPU_OP_BIND_IMAGE_MEMORY:
	case GPU_OP_GET_IMAGE_SUBRESOURCE_LAYOUT:
		break;
	default:
		*handled = 0;
		return 0;
	}

	/* Ordinary void destruction still permits the client's echoed-opcode flag. */
	if (requested > 1)
		return EINVAL;
	if (opcode != GPU_OP_DESTROY_BUFFER &&
	    opcode != GPU_OP_DESTROY_IMAGE &&
	    requested != 1)
		return EINVAL;

	/* Resource kind always follows the opcode, never an untyped identity lookup. */
	switch (opcode) {
	case GPU_OP_CREATE_BUFFER:
		error = create_buffer(session, reader, reply);
		break;
	case GPU_OP_CREATE_IMAGE:
		error = create_image(session, reader, reply);
		break;
	case GPU_OP_DESTROY_BUFFER:
		error = destroy_resource(session, I915_VK_OBJ_BUFFER, reader);
		break;
	case GPU_OP_DESTROY_IMAGE:
		error = destroy_resource(session, I915_VK_OBJ_IMAGE, reader);
		break;
	case GPU_OP_GET_BUFFER_MEMORY_REQUIREMENTS:
		error = memory_requirements(session, I915_VK_OBJ_BUFFER, reader, reply);
		break;
	case GPU_OP_GET_IMAGE_MEMORY_REQUIREMENTS:
		error = memory_requirements(session, I915_VK_OBJ_IMAGE, reader, reply);
		break;
	case GPU_OP_BIND_BUFFER_MEMORY:
		error = bind_memory(session, I915_VK_OBJ_BUFFER, reader, reply);
		break;
	case GPU_OP_BIND_IMAGE_MEMORY:
		error = bind_memory(session, I915_VK_OBJ_IMAGE, reader, reply);
		break;
	default:
		error = image_layout(session, reader, reply);
		break;
	}

	/* Native framing or ownership failure stops this immutable submission. */
	if (error != 0)
		return error;

	/* Succeeded: one exact typed resource operation completed. */
	return 0;
}

/*
 * Resolves one bounded resource span to its retained native VA view and coherent CPU alias.
 */
int
bcm2711_vulkan_resource_backing(
	struct bcm2711_vulkan_resource *resource,
	uint64_t offset,
	uint64_t bytes,
	struct bcm2711_v3d_view **view,
	uint32_t *address,
	void **cpu)
{
	struct bcm2711_vulkan_memory *memory;
	struct bcm2711_buffer *buffer;
	uint64_t limit;
	uint64_t begin;
	uint64_t native;

	/* An unbound description grants neither a native address nor a CPU alias. */
	*view = NULL;
	*address = 0;
	*cpu = NULL;
	if (resource == NULL ||
	    resource->memory == NULL ||
	    bytes == 0)
		return EINVAL;

	/* A mip-mapped image's levels lie after level 0, inside its whole storage. */
	limit = resource->bytes;
	if (resource->storage_bytes > limit)
		limit = resource->storage_bytes;
	if (offset > limit || bytes > limit - offset)
		return EINVAL;
	memory = resource->memory->payload;
	if (memory->view == NULL || memory->view->quarantined)
		return EIO;

	/* The binding was bounded at publication; recheck the exact backing before exposing a draw/transfer span. */
	begin = resource->offset + offset;
	if (begin < resource->offset ||
	    begin > memory->bytes ||
	    bytes > memory->bytes - begin)
		return EINVAL;
	buffer = memory->view->buffer;
	if (!buffer->uncached ||
	    buffer->address == NULL ||
	    begin > buffer->bytes ||
	    bytes > buffer->bytes - begin)
		return EIO;
	native = (uint64_t)memory->view->address + begin;
	if (native >= 0x100000000ULL || bytes > 0x100000000ULL - native)
		return EOVERFLOW;

	/* Returned pointers are borrowed; prepared jobs must retain the resource or view before leaving the controller mutex. */
	*view = memory->view;
	*address = (uint32_t)native;
	*cpu = (uint8_t *)buffer->address + begin;

	/* Succeeded: the entire requested span belongs to the retained immutable binding. */
	return 0;
}

/*
 * The colour formats an image may have, sampled through the native texture
 * format of each, and the Output Image Format of the ones that are colour
 * attachments (the tile buffer loads and stores them as it is; SNORM, the
 * 16-bit UNORM and SNORM ones and RGB9E5 are not).  The ones of 32-bit
 * float or integer channels, and the integer ones, are not filtered
 * linearly.
 * Values referenced: Mesa 26.2.6 src/broadcom/vulkan/v3dvx_formats.c (each
 * format's texture type, render target type, swizzle and filtering; the
 * UNORM16 and SNORM16 render targets Mesa emulates in the shader are left
 * out).
 */
static const struct bcm2711_vulkan_colour_format colour_formats[] = {
	{ VK_FORMAT_R8G8B8A8_UNORM, BCM2711_NATIVE_TEXEL_RGBA8, 4, 0, 1, 27U },
	{ VK_FORMAT_B8G8R8A8_UNORM, BCM2711_NATIVE_TEXEL_RGBA8, 4, 1, 1, 27U },
	{ VK_FORMAT_R8G8B8A8_SRGB, BCM2711_NATIVE_TEXEL_RGBA8_SRGB, 4, 0, 1, 0U },
	{ VK_FORMAT_B8G8R8A8_SRGB, BCM2711_NATIVE_TEXEL_RGBA8_SRGB, 4, 1, 1, 0U },
	{ VK_FORMAT_R8G8B8A8_SNORM, BCM2711_NATIVE_TEXEL_RGBA8_SNORM, 4, 0, 1, BCM2711_VULKAN_NO_OUTPUT },
	{ VK_FORMAT_R8G8B8A8_UINT, BCM2711_NATIVE_TEXEL_RGBA8UI, 4, 0, 0, 34U },
	{ VK_FORMAT_R8G8B8A8_SINT, BCM2711_NATIVE_TEXEL_RGBA8I, 4, 0, 0, 31U },
	{ VK_FORMAT_R8_UNORM, BCM2711_NATIVE_TEXEL_R8, 1, 0, 1, 30U },
	{ VK_FORMAT_R8_SNORM, BCM2711_NATIVE_TEXEL_R8_SNORM, 1, 0, 1, BCM2711_VULKAN_NO_OUTPUT },
	{ VK_FORMAT_R8_UINT, BCM2711_NATIVE_TEXEL_R8UI, 1, 0, 0, 36U },
	{ VK_FORMAT_R8_SINT, BCM2711_NATIVE_TEXEL_R8I, 1, 0, 0, 33U },
	{ VK_FORMAT_R8G8_UNORM, BCM2711_NATIVE_TEXEL_RG8, 2, 0, 1, 29U },
	{ VK_FORMAT_R8G8_SNORM, BCM2711_NATIVE_TEXEL_RG8_SNORM, 2, 0, 1, BCM2711_VULKAN_NO_OUTPUT },
	{ VK_FORMAT_R8G8_UINT, BCM2711_NATIVE_TEXEL_RG8UI, 2, 0, 0, 35U },
	{ VK_FORMAT_R8G8_SINT, BCM2711_NATIVE_TEXEL_RG8I, 2, 0, 0, 32U },
	{ VK_FORMAT_R5G6B5_UNORM_PACK16, BCM2711_NATIVE_TEXEL_RGB565, 2, 0, 1, 7U },
	{ VK_FORMAT_R4G4B4A4_UNORM_PACK16, BCM2711_NATIVE_TEXEL_RGBA4, 2, 0, 1, 6U },
	{ VK_FORMAT_R5G5B5A1_UNORM_PACK16, BCM2711_NATIVE_TEXEL_RGB5_A1, 2, 0, 1, 4U },
	{ VK_FORMAT_A2B10G10R10_UNORM_PACK32, BCM2711_NATIVE_TEXEL_RGB10_A2, 4, 0, 1, 3U },
	{ VK_FORMAT_A2B10G10R10_UINT_PACK32, BCM2711_NATIVE_TEXEL_RGB10_A2UI, 4, 0, 0, 2U },
	{ VK_FORMAT_R16_UNORM, BCM2711_NATIVE_TEXEL_R16, 2, 0, 1, BCM2711_VULKAN_NO_OUTPUT },
	{ VK_FORMAT_R16_SNORM, BCM2711_NATIVE_TEXEL_R16_SNORM, 2, 0, 1, BCM2711_VULKAN_NO_OUTPUT },
	{ VK_FORMAT_R16_SFLOAT, BCM2711_NATIVE_TEXEL_R16F, 2, 0, 1, 20U },
	{ VK_FORMAT_R16_UINT, BCM2711_NATIVE_TEXEL_R16UI, 2, 0, 0, 26U },
	{ VK_FORMAT_R16_SINT, BCM2711_NATIVE_TEXEL_R16I, 2, 0, 0, 23U },
	{ VK_FORMAT_R16G16_UNORM, BCM2711_NATIVE_TEXEL_RG16, 4, 0, 1, BCM2711_VULKAN_NO_OUTPUT },
	{ VK_FORMAT_R16G16_SNORM, BCM2711_NATIVE_TEXEL_RG16_SNORM, 4, 0, 1, BCM2711_VULKAN_NO_OUTPUT },
	{ VK_FORMAT_R16G16_SFLOAT, BCM2711_NATIVE_TEXEL_RG16F, 4, 0, 1, 19U },
	{ VK_FORMAT_R16G16_UINT, BCM2711_NATIVE_TEXEL_RG16UI, 4, 0, 0, 25U },
	{ VK_FORMAT_R16G16_SINT, BCM2711_NATIVE_TEXEL_RG16I, 4, 0, 0, 22U },
	{ VK_FORMAT_R16G16B16A16_UNORM, BCM2711_NATIVE_TEXEL_RGBA16, 8, 0, 1, BCM2711_VULKAN_NO_OUTPUT },
	{ VK_FORMAT_R16G16B16A16_SNORM, BCM2711_NATIVE_TEXEL_RGBA16_SNORM, 8, 0, 1, BCM2711_VULKAN_NO_OUTPUT },
	{ VK_FORMAT_R16G16B16A16_SFLOAT, BCM2711_NATIVE_TEXEL_RGBA16F, 8, 0, 1, 18U },
	{ VK_FORMAT_R16G16B16A16_UINT, BCM2711_NATIVE_TEXEL_RGBA16UI, 8, 0, 0, 24U },
	{ VK_FORMAT_R16G16B16A16_SINT, BCM2711_NATIVE_TEXEL_RGBA16I, 8, 0, 0, 21U },
	{ VK_FORMAT_B10G11R11_UFLOAT_PACK32, BCM2711_NATIVE_TEXEL_R11G11B10F, 4, 0, 1, 8U },
	{ VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, BCM2711_NATIVE_TEXEL_RGB9E5, 4, 0, 1, BCM2711_VULKAN_NO_OUTPUT },
	{ VK_FORMAT_R32_SFLOAT, BCM2711_NATIVE_TEXEL_R32F, 4, 0, 0, 11U },
	{ VK_FORMAT_R32_UINT, BCM2711_NATIVE_TEXEL_R32UI, 4, 0, 0, 17U },
	{ VK_FORMAT_R32_SINT, BCM2711_NATIVE_TEXEL_R32I, 4, 0, 0, 14U },
	{ VK_FORMAT_R32G32_SFLOAT, BCM2711_NATIVE_TEXEL_RG32F, 8, 0, 0, 10U },
	{ VK_FORMAT_R32G32_UINT, BCM2711_NATIVE_TEXEL_RG32UI, 8, 0, 0, 16U },
	{ VK_FORMAT_R32G32_SINT, BCM2711_NATIVE_TEXEL_RG32I, 8, 0, 0, 13U },
	{ VK_FORMAT_R32G32B32A32_SFLOAT, BCM2711_NATIVE_TEXEL_RGBA32F, 16, 0, 0, 9U },
	{ VK_FORMAT_R32G32B32A32_UINT, BCM2711_NATIVE_TEXEL_RGBA32UI, 16, 0, 0, 15U },
	{ VK_FORMAT_R32G32B32A32_SINT, BCM2711_NATIVE_TEXEL_RGBA32I, 16, 0, 0, 12U }
};

/* Finds one of the colour formats an image may have. */
const struct bcm2711_vulkan_colour_format *
bcm2711_vulkan_colour_format(
	VkFormat format)
{
	uint32_t index;

	/* The table, in no particular order. */
	for (index = 0; index < sizeof(colour_formats) / sizeof(colour_formats[0]); index++) {
		if (colour_formats[index].format == format)
			return &colour_formats[index];
	}

	/* Any other format has no colour image. */
	return NULL;
}

/*
 * Describes how the tile buffer holds a colour attachment of format.
 *
 * The internal type and bpp follow the output format, and the clamp the
 * type, as Mesa's v3dX(get_internal_type_bpp_for_output_format) and the
 * 7.1 v3dX(clamp_for_format_and_type) choose them: sRGB targets are held
 * as clamped 16-bit floats, converted at load and store.  Only 8-bit UNORM
 * and 16-bit float targets blend (v3dX(format_supports_blending)); the
 * fragment program writes 32-bit float and integer ones a word a channel,
 * as Mesa's v3dv sets f32_color_rb for them.
 * Values referenced: Mesa 26.2.6 src/broadcom/vulkan/v3dvx_formats.c,
 * v3dvx_cmd_buffer.c and v3dv_pipeline.c (pipeline_populate_v3d_fs_key's
 * f32_color_rb).
 */
int
bcm2711_vulkan_colour_target(
	VkFormat format,
	struct bcm2711_vulkan_colour_target *target)
{
	const struct bcm2711_vulkan_colour_format *colour;
	uint32_t output;

	/* A colour format with an output format. */
	colour = bcm2711_vulkan_colour_format(format);
	if (colour == NULL || colour->output == BCM2711_VULKAN_NO_OUTPUT)
		return ENOTSUP;
	output = colour->output;
	kern_memset(target, 0, sizeof(*target));
	target->output = output;
	target->texel = colour->bytes;
	target->swap = colour->swap != 0;

	/* The internal type and bpp of the output format (Output Image Format values, channels named from the low bits). */
	switch (output) {
	case 27U:
	case 29U:
	case 30U:
	case 6U:
	case 7U:
	case 4U:
		/* rgba8, rg8, r8, abgr4444, bgr565, abgr1555. */
		target->type = BCM2711_NATIVE_TYPE_8;
		target->bpp = 0;
		break;
	case 31U:
	case 32U:
	case 33U:
		/* rgba8i, rg8i, r8i. */
		target->type = BCM2711_NATIVE_TYPE_8I;
		target->bpp = 0;
		break;
	case 34U:
	case 35U:
	case 36U:
		/* rgba8ui, rg8ui, r8ui. */
		target->type = BCM2711_NATIVE_TYPE_8UI;
		target->bpp = 0;
		break;
	case 0U:
	case 3U:
	case 8U:
	case 18U:
	case 19U:
	case 20U:
		/* srgb8_alpha8, rgb10_a2, r11f_g11f_b10f and the 16-bit floats, 64 bits a pixel (the alpha kept for the test). */
		target->type = BCM2711_NATIVE_TYPE_16F;
		target->bpp = 1U;
		break;
	case 21U:
		/* rgba16i. */
		target->type = BCM2711_NATIVE_TYPE_16I;
		target->bpp = 1U;
		break;
	case 22U:
	case 23U:
		/* rg16i, r16i. */
		target->type = BCM2711_NATIVE_TYPE_16I;
		target->bpp = 0;
		break;
	case 2U:
	case 24U:
		/* rgb10_a2ui, rgba16ui. */
		target->type = BCM2711_NATIVE_TYPE_16UI;
		target->bpp = 1U;
		break;
	case 25U:
	case 26U:
		/* rg16ui, r16ui. */
		target->type = BCM2711_NATIVE_TYPE_16UI;
		target->bpp = 0;
		break;
	case 12U:
	case 13U:
	case 14U:
		/* rgba32i, rg32i, r32i. */
		target->type = BCM2711_NATIVE_TYPE_32I;
		target->bpp = 14U - output;
		break;
	case 15U:
	case 16U:
	case 17U:
		/* rgba32ui, rg32ui, r32ui. */
		target->type = BCM2711_NATIVE_TYPE_32UI;
		target->bpp = 17U - output;
		break;
	case 9U:
	case 10U:
	case 11U:
		/* rgba32f, rg32f, r32f. */
		target->type = BCM2711_NATIVE_TYPE_32F;
		target->bpp = 11U - output;
		break;
	default:
		return ENOTSUP;
	}

	/*
	 * The 7.1 clamp of the type (Render Target Type Clamp): integers
	 * clamped to the target's range, sRGB's 16-bit floats to [0, 1], the
	 * others unclamped.
	 */
	switch (target->type) {
	case BCM2711_NATIVE_TYPE_8I:
		target->clamp = 16U;
		break;
	case BCM2711_NATIVE_TYPE_8UI:
		target->clamp = 20U;
		break;
	case BCM2711_NATIVE_TYPE_8:
		target->clamp = 8U;
		break;
	case BCM2711_NATIVE_TYPE_16I:
		target->clamp = 17U;
		break;
	case BCM2711_NATIVE_TYPE_16UI:
		target->clamp = 21U;
		break;
	case BCM2711_NATIVE_TYPE_16F:
		target->clamp = 9U;
		if (output == 0U)
			target->clamp = 24U;
		break;
	case BCM2711_NATIVE_TYPE_32I:
		target->clamp = 18U;
		break;
	case BCM2711_NATIVE_TYPE_32UI:
		target->clamp = 22U;
		break;
	default:
		target->clamp = 10U;
		break;
	}

	/* The channels: two of the RG formats, one of the R formats, three of R11G11B10, four of the others. */
	target->components = 4U;
	if (output == 10U || output == 13U || output == 16U || output == 19U || output == 22U || output == 25U || output == 29U || output == 32U || output == 35U)
		target->components = 2U;
	if (output == 11U || output == 14U || output == 17U || output == 20U || output == 23U || output == 26U || output == 30U || output == 33U || output == 36U)
		target->components = 1U;
	if (output == 7U || output == 8U)
		target->components = 3U;

	/* Integer channels, 32-bit writes, blending and exact load/store follow the type. */
	if (target->type == BCM2711_NATIVE_TYPE_8I ||
	    target->type == BCM2711_NATIVE_TYPE_8UI ||
	    target->type == BCM2711_NATIVE_TYPE_16I ||
	    target->type == BCM2711_NATIVE_TYPE_16UI ||
	    target->type == BCM2711_NATIVE_TYPE_32I ||
	    target->type == BCM2711_NATIVE_TYPE_32UI)
		target->integer = true;
	if (target->integer || target->type == BCM2711_NATIVE_TYPE_32F)
		target->wide = true;
	if (target->type == BCM2711_NATIVE_TYPE_8 || target->type == BCM2711_NATIVE_TYPE_16F)
		target->blend = true;
	target->exact = true;
	if (output == 0U || output == 3U || output == 8U)
		target->exact = false;

	/* Succeeded: the target's place in the tile buffer. */
	return 0;
}

/*
 * Gives the bytes of a texel of a storage image format: the formats Vulkan
 * 1.0 requires of storage images without shaderStorageImageExtendedFormats,
 * which a 7.1 engine's compute programs read and write through the general
 * memory TMU and convert themselves; zero for any other format.
 *
 * Values referenced: the Vulkan specification, "Required Format Support"
 * (the formats with VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT).
 */
uint32_t
bcm2711_vulkan_storage_texel(
	VkFormat format)
{
	/* Each required format by its texel's size. */
	switch (format) {
	case VK_FORMAT_R8G8B8A8_UNORM:
	case VK_FORMAT_R8G8B8A8_SNORM:
	case VK_FORMAT_R8G8B8A8_UINT:
	case VK_FORMAT_R8G8B8A8_SINT:
	case VK_FORMAT_R32_UINT:
	case VK_FORMAT_R32_SINT:
	case VK_FORMAT_R32_SFLOAT:
		return 4;
	case VK_FORMAT_R16G16B16A16_UINT:
	case VK_FORMAT_R16G16B16A16_SINT:
	case VK_FORMAT_R16G16B16A16_SFLOAT:
	case VK_FORMAT_R32G32_UINT:
	case VK_FORMAT_R32G32_SINT:
	case VK_FORMAT_R32G32_SFLOAT:
		return 8;
	case VK_FORMAT_R32G32B32A32_UINT:
	case VK_FORMAT_R32G32B32A32_SINT:
	case VK_FORMAT_R32G32B32A32_SFLOAT:
		return 16;
	default:
		break;
	}

	/* Succeeded: no other format is a storage image's. */
	return 0;
}

/*
 * Tells whether a format is one of the tile buffer's depth/stencil formats.
 *
 * D24S8 has depth and stencil, X8_D24, D16 and D32_SFLOAT depth alone.
 */
bool
bcm2711_vulkan_depth_format(
	VkFormat format)
{
	/* The two 32-bit formats whose depth is the tile buffer's 24-bit depth, and the 16-bit one. */
	if (format == VK_FORMAT_D24_UNORM_S8_UINT)
		return true;
	if (format == VK_FORMAT_X8_D24_UNORM_PACK32)
		return true;
	if (format == VK_FORMAT_D16_UNORM)
		return true;

	/* And the 32-bit float depth. */
	if (format == VK_FORMAT_D32_SFLOAT)
		return true;

	/* Any other format is colour or unsupported. */
	return false;
}

/*
 * Describes one mip level of an image: level 0 is the image's own raster;
 * each smaller level halves the size above it (to no less than one texel)
 * and starts where the one above ends, at a 64-byte multiple pitch.
 */
int
bcm2711_vulkan_image_level(
	const struct bcm2711_vulkan_resource *image,
	uint32_t level,
	struct bcm2711_vulkan_image_level *result)
{
	uint64_t offset;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t index;

	/* Only the image's own levels exist; a one-level image counts as one. */
	if (image == NULL || result == NULL)
		return EINVAL;
	if (level != 0 && level >= image->levels)
		return EINVAL;

	/* Level 0 is the image's raster. */
	width = image->width;
	height = image->height;
	pitch = image->pitch;
	offset = 0;

	/* Each smaller level starts after the one above it. */
	for (index = 1; index <= level; index++) {
		offset += (uint64_t)pitch * height;
		width >>= 1;
		if (width == 0)
			width = 1;
		height >>= 1;
		if (height == 0)
			height = 1;
		pitch = (width * image->texel_bytes + 63U) & ~63U;
	}

	/* The level's rows (twice as many of a multisampled image's one level, whose pixels are 2x2 blocks of samples). */
	result->offset = offset;
	result->bytes = (uint64_t)pitch * height;
	if (image->samples == 4U)
		result->bytes *= 2U;
	result->width = width;
	result->height = height;
	result->pitch = pitch;

	/* Succeeded: the caller has the level's place and size. */
	return 0;
}

/*
 * Fills view with one mip level of one array layer (a 3D image's slice) of
 * an image as a one-level, one-layer 2D image of its own: the level's
 * raster in the layer at the binding offset moved to it, borrowing the
 * image's memory and device
 * (the caller keeps the image retained while it uses the view, and never
 * publishes or releases it).
 */
int
bcm2711_vulkan_image_level_view(
	const struct bcm2711_vulkan_resource *image,
	uint32_t level,
	uint32_t layer,
	struct bcm2711_vulkan_resource *view)
{
	struct bcm2711_vulkan_image_level geometry;
	uint32_t layers;
	int error;

	/* The level's place and size in a layer. */
	error = bcm2711_vulkan_image_level(image, level, &geometry);
	if (error != 0)
		return error;

	/* Only the level's own layers (or slices) exist. */
	layers = bcm2711_vulkan_image_slices(image, level);
	if (layer >= layers)
		return EINVAL;

	/* The view is the image moved to the level's rows in the layer, with only that level and layer. */
	*view = *image;
	view->offset = image->offset + (uint64_t)layer * image->layer_stride + geometry.offset;
	view->bytes = geometry.bytes;
	view->required_bytes = geometry.bytes;
	view->width = geometry.width;
	view->height = geometry.height;
	view->pitch = geometry.pitch;
	view->levels = 1;
	view->storage_bytes = 0;
	view->layers = 1;
	view->layer_stride = 0;
	view->cube = false;
	view->layer_layouts = NULL;
	view->depth = 0;
	view->layout = bcm2711_vulkan_image_level_layout(image, level, layer);

	/* Succeeded: the view addresses the level's raster in the layer alone. */
	return 0;
}

/*
 * Reports how many array layers an image has (one at least).
 */
uint32_t
bcm2711_vulkan_image_layers(
	const struct bcm2711_vulkan_resource *image)
{
	/* An image made without layers has the one. */
	if (image->layers == 0)
		return 1;

	/* Succeeded: the image's layers. */
	return image->layers;
}

/*
 * Reports the rasters a mip level of an image has: a 3D image's slices at
 * the level (halved with each level, one at least), any other image's
 * array layers.
 */
uint32_t
bcm2711_vulkan_image_slices(
	const struct bcm2711_vulkan_resource *image,
	uint32_t level)
{
	uint32_t slices;

	/* An image of layers has them at every level. */
	if (image->depth == 0)
		return bcm2711_vulkan_image_layers(image);

	/* A 3D image's slices halve with each level. */
	slices = 1;
	if (level < 32U)
		slices = image->depth >> level;
	if (slices == 0)
		slices = 1;

	/* Succeeded: the level's slices. */
	return slices;
}

/*
 * Reports the current layout of one mip level of one array layer of an image.
 */
VkImageLayout
bcm2711_vulkan_image_level_layout(
	const struct bcm2711_vulkan_resource *image,
	uint32_t level,
	uint32_t layer)
{
	uint32_t levels;

	/* A level past the image's is answered as its level 0; a 3D image's slices share their level's layout. */
	if (level >= BCM2711_VULKAN_IMAGE_LEVELS)
		level = 0;
	if (image->depth != 0)
		layer = 0;

	/* The layers after the first keep their levels' layouts in their own table. */
	if (layer != 0 && image->layer_layouts != NULL && layer < bcm2711_vulkan_image_layers(image)) {
		levels = image->levels;
		if (levels == 0)
			levels = 1;
		if (level >= levels)
			level = 0;
		return image->layer_layouts[(layer - 1U) * levels + level];
	}

	/* Level 0's layout of the first layer is the image's own. */
	if (level == 0)
		return image->layout;

	/* Succeeded: a smaller level's own layout. */
	return image->level_layouts[level];
}

/*
 * Records the layout one mip level of one array layer of an image moved to.
 */
void
bcm2711_vulkan_image_set_level_layout(
	struct bcm2711_vulkan_resource *image,
	uint32_t level,
	uint32_t layer,
	VkImageLayout layout)
{
	uint32_t levels;

	/* A level past the table records nothing; a 3D image's slices share their level's layout. */
	if (level >= BCM2711_VULKAN_IMAGE_LEVELS)
		return;
	if (image->depth != 0)
		layer = 0;

	/* The layers after the first keep their levels' layouts in their own table. */
	if (layer != 0) {
		levels = image->levels;
		if (levels == 0)
			levels = 1;
		if (image->layer_layouts != NULL && layer < bcm2711_vulkan_image_layers(image) && level < levels)
			image->layer_layouts[(layer - 1U) * levels + level] = layout;
		return;
	}

	/* Level 0's layout of the first layer is the image's own. */
	if (level == 0) {
		image->layout = layout;
	} else {
		image->level_layouts[level] = layout;
	}

	/* Succeeded: following operations see the level's layout. */
	return;
}

/*
 * Resolves one range of a colour image clear against the image.
 *
 * range is the record's word (vulkan-record.h): its counts of zero become
 * every level or layer from the first one.  A 3D image's range names its
 * one layer, and layers then reports one; the caller clears each level's
 * slices.  EINVAL for a range past the image's levels or layers.
 */
int
bcm2711_vulkan_image_clear_range(
	const struct bcm2711_vulkan_resource *image,
	uint32_t range,
	uint32_t *level,
	uint32_t *levels,
	uint32_t *layer,
	uint32_t *layers)
{
	uint32_t image_levels;
	uint32_t image_layers;

	/* The range's four fields. */
	*level = (range >> BCM2711_VULKAN_CLEAR_LEVEL_SHIFT) & 15U;
	*levels = (range >> BCM2711_VULKAN_CLEAR_LEVELS_SHIFT) & 15U;
	*layer = (range >> BCM2711_VULKAN_CLEAR_LAYER_SHIFT) & 255U;
	*layers = (range >> BCM2711_VULKAN_CLEAR_LAYERS_SHIFT) & 511U;

	/* The image's own levels and layers (a 3D image has one layer). */
	image_levels = image->levels;
	if (image_levels == 0)
		image_levels = 1;
	image_layers = bcm2711_vulkan_image_layers(image);

	/* The first level and layer are the image's. */
	if (*level >= image_levels || *layer >= image_layers)
		return EINVAL;

	/* A zero count stands for every level or layer from the first one. */
	if (*levels == 0)
		*levels = image_levels - *level;
	if (*layers == 0)
		*layers = image_layers - *layer;

	/* The counts end inside the image. */
	if (*levels > image_levels - *level)
		return EINVAL;
	if (*layers > image_layers - *layer)
		return EINVAL;

	/* Succeeded: the range names levels and layers the image has. */
	return 0;
}

/*
 * Tells whether the session's engine is a V3D 7.1.
 *
 * The 7.1 alone has the formats, attachments and image kinds its tile
 * buffer and compiler add over the 4.2.
 */
bool
bcm2711_vulkan_engine_71(
	const struct bcm2711_vulkan_session *session)
{
	const struct bcm2711_v3d *engine;
	uint32_t layout;

	/* The hub's identification names the technology version in its low four bits. */
	engine = session->render->device->space.native;
	layout = engine->hardware.hub_ident[1];
	if ((layout & 15U) != 7U)
		return false;

	/* Succeeded: the engine is a 7.1. */
	return true;
}

/* Creates one native buffer description from the client's full standard record. */
static int
create_buffer(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkBufferCreateInfo info;
	struct bcm2711_vulkan_resource description;
	uint64_t device_id;
	uint64_t present;
	uint32_t allowed;
	int error;

	/* Decode the required input using the existing bounded command arena. */
	device_id = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present != 1 || reader->error != 0)
		return EINVAL;
	error = external_declaration(reader, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO);
	if (error != 0)
		return error;
	kern_memset(&info, 0, sizeof(info));
	i915_vkc_dec_VkBufferCreateInfo(reader, &session->arena, &info);
	if (reader->error != 0)
		return reader->error;
	if (info.sType != VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO ||
	    info.flags != 0 ||
	    info.size == 0 ||
	    info.size > VULKAN_RESOURCE_BYTES ||
	    info.usage == 0)
		return ENOTSUP;
	allowed = VULKAN_BUFFER_USAGE;
#if CONFIG_DRIVER_RPI4_GPU || CONFIG_DRIVER_RPI5_GPU
	/* A bitstream buffer is read by the CPU into the video decoder's staging. */
	allowed |= BCM2711_VIDEO_BUFFER_USAGE_DECODE_SRC;
#endif
	if ((info.usage & ~allowed) != 0)
		return ENOTSUP;
	error = sharing_mode(info.sharingMode, info.queueFamilyIndexCount, info.pQueueFamilyIndices);
	if (error != 0)
		return error;

	/* Buffer allocation requirements use a complete 64-byte-aligned extent, without committing storage yet. */
	kern_memset(&description, 0, sizeof(description));
	description.bytes = info.size;
	description.required_bytes = (info.size + 63U) & ~63ULL;
	description.alignment = 64;
	description.usage = info.usage;
	error = publish_resource(session, I915_VK_OBJ_BUFFER, &description, 0, device_id, reader, reply);
	if (error != 0)
		return error;

	/* Succeeded: the typed buffer owns an immutable unbound storage description. */
	return 0;
}

/* Creates one raster colour image, with its mip levels and array layers, with exact native row pitch. */
static int
create_image(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkImageCreateInfo info;
	struct bcm2711_vulkan_resource description;
	struct bcm2711_vulkan_image_level geometry;
	uint64_t device_id;
	uint64_t present;
	uint32_t largest;
	uint32_t level;
	uint32_t texel_bytes;
	uint32_t layouts;
#if CONFIG_DRIVER_RPI4_GPU || CONFIG_DRIVER_RPI5_GPU
	uint64_t chroma;
#endif
	const struct bcm2711_vulkan_colour_format *colour;
	bool nv12;
	bool depth;
	bool cube;
	int error;

	/* The codec accepts the client's stripped external declaration; native storage capabilities remain separate. */
	device_id = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present != 1 || reader->error != 0)
		return EINVAL;
	error = external_declaration(reader, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
	if (error != 0)
		return error;
	kern_memset(&info, 0, sizeof(info));
	i915_vkc_dec_VkImageCreateInfo(reader, &session->arena, &info);
	if (reader->error != 0)
		return reader->error;
	if (info.sType != VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO ||
	    (info.flags & ~VULKAN_IMAGE_FLAGS) != 0 ||
	    (info.imageType != VK_IMAGE_TYPE_2D && info.imageType != VK_IMAGE_TYPE_3D) ||
	    info.extent.width == 0 ||
	    info.extent.width > 4096 ||
	    info.extent.height == 0 ||
	    info.extent.height > 4096 ||
	    (info.imageType == VK_IMAGE_TYPE_2D && info.extent.depth != 1) ||
	    info.mipLevels == 0 ||
	    info.mipLevels > BCM2711_VULKAN_IMAGE_LEVELS ||
	    info.arrayLayers == 0 ||
	    info.arrayLayers > BCM2711_VULKAN_IMAGE_LAYERS ||
	    (info.samples != VK_SAMPLE_COUNT_1_BIT && info.samples != VK_SAMPLE_COUNT_4_BIT))
		return ENOTSUP;

	/*
	 * A multisampled image (on a 7.1 engine) is a one-level, one-layer
	 * optimally tiled 2D attachment of a colour format the tile buffer
	 * holds or of a depth/stencil format, copied and resolved, and
	 * sampled only by a fetch of one sample.
	 */
	if (info.samples == VK_SAMPLE_COUNT_4_BIT) {
		if (!bcm2711_vulkan_engine_71(session) || info.imageType != VK_IMAGE_TYPE_2D)
			return ENOTSUP;
		if (info.flags != 0 ||
		    info.mipLevels != 1 ||
		    info.arrayLayers != 1 ||
		    info.tiling != VK_IMAGE_TILING_OPTIMAL ||
		    (info.usage & ~VULKAN_MULTISAMPLE_USAGE) != 0 ||
		    (info.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) == 0)
			return ENOTSUP;
		colour = bcm2711_vulkan_colour_format(info.format);
		if ((colour == NULL || colour->output == BCM2711_VULKAN_NO_OUTPUT) && !bcm2711_vulkan_depth_format(info.format))
			return ENOTSUP;
	}
	nv12 = false;
#if CONFIG_DRIVER_RPI4_GPU || CONFIG_DRIVER_RPI5_GPU
	/*
	 * An NV12 picture decodes, serves as a reference, is read back, and is
	 * sampled through views of its planes.
	 */
	if (info.format == (VkFormat)BCM2711_VIDEO_FORMAT_NV12) {
		nv12 = true;
		if (info.usage == 0 ||
		    (info.usage & ~(BCM2711_VIDEO_USAGE_DECODE_DST | BCM2711_VIDEO_USAGE_DECODE_DPB | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) != 0)
			return ENOTSUP;
	}
#endif

	/*
	 * A depth/stencil image is an optimally tiled attachment with the tile
	 * buffer's 32-bit D24S8 or X8_D24 pixels or 16-bit D16 ones, kept in
	 * the same raster form as a colour image.
	 */
	depth = false;
	texel_bytes = 4U;
	if (info.format == VK_FORMAT_D16_UNORM)
		texel_bytes = 2U;
	colour = bcm2711_vulkan_colour_format(info.format);
	if (colour != NULL) {
		texel_bytes = colour->bytes;
		if (colour->output == BCM2711_VULKAN_NO_OUTPUT && (info.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0)
			return ENOTSUP;
	}
	if (bcm2711_vulkan_depth_format(info.format)) {
		depth = true;
		if (info.flags != 0 ||
		    info.tiling != VK_IMAGE_TILING_OPTIMAL ||
		    (info.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0 ||
		    (info.usage & ~VULKAN_DEPTH_USAGE) != 0)
			return ENOTSUP;
	}

	/*
	 * Only an NV12 picture's plane views and a mutable storage image's
	 * storage views (checked below with its usage) reinterpret the image's
	 * format; a colour image may be viewed as a cube when its layers are
	 * square and come in sixes (Vulkan's rules for CUBE_COMPATIBLE).
	 */
	cube = false;
	if (!nv12 && (info.flags & VULKAN_IMAGE_PLANE_FLAGS) != 0 && (info.flags & VULKAN_IMAGE_PLANE_FLAGS) != VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT)
		return ENOTSUP;
	if (!nv12 && (info.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) != 0 && (info.usage & VK_IMAGE_USAGE_STORAGE_BIT) == 0)
		return ENOTSUP;
	if ((info.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) != 0) {
		if (nv12 ||
		    depth ||
		    info.extent.width != info.extent.height ||
		    info.arrayLayers < 6U ||
		    (info.arrayLayers % 6U) != 0)
			return ENOTSUP;
		cube = true;
	}

	/* Only an optimally tiled colour or depth/stencil image has more than one layer (a depth one is a 2D array, never a cube). */
	if (info.arrayLayers != 1 &&
	    (nv12 || info.tiling != VK_IMAGE_TILING_OPTIMAL))
		return ENOTSUP;
	if (!nv12 && !depth && colour == NULL)
		return ENOTSUP;
	if (info.tiling != VK_IMAGE_TILING_LINEAR && info.tiling != VK_IMAGE_TILING_OPTIMAL)
		return ENOTSUP;
	if (!nv12 && !depth && (info.usage == 0 || (info.usage & ~VULKAN_IMAGE_USAGE) != 0))
		return ENOTSUP;

	/* A storage image is an optimally tiled one-sample image of a storage format, which only a 7.1 engine's compute programs write. */
	if ((info.usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0) {
		if (!bcm2711_vulkan_engine_71(session) ||
		    bcm2711_vulkan_storage_texel(info.format) == 0 ||
		    info.tiling != VK_IMAGE_TILING_OPTIMAL ||
		    info.samples != VK_SAMPLE_COUNT_1_BIT)
			return ENOTSUP;
	}
	if (info.initialLayout != VK_IMAGE_LAYOUT_UNDEFINED &&
	    (info.initialLayout != VK_IMAGE_LAYOUT_PREINITIALIZED || info.tiling != VK_IMAGE_TILING_LINEAR))
		return ENOTSUP;
	error = sharing_mode(info.sharingMode, info.queueFamilyIndexCount, info.pQueueFamilyIndices);
	if (error != 0)
		return error;

	/* A 3D image is a sampled and copied colour image of slices. */
	if (info.imageType == VK_IMAGE_TYPE_3D) {
		error = volume_shape(session, &info, colour);
		if (error != 0)
			return error;
	}

	/* Only an optimally tiled colour image has mip levels, no more than its size (a 3D image's depth too) halves to one texel. */
	if (info.mipLevels != 1) {
		if (nv12 || depth || info.tiling != VK_IMAGE_TILING_OPTIMAL)
			return ENOTSUP;
		largest = info.extent.width;
		if (info.extent.height > largest)
			largest = info.extent.height;
		if (info.extent.depth > largest)
			largest = info.extent.depth;
		if ((largest >> (info.mipLevels - 1U)) == 0)
			return EINVAL;
	}

	/* Optimal images use the same native raster backing; Vulkan does not require exposing their CPU subresource layout. */
	kern_memset(&description, 0, sizeof(description));
	description.width = info.extent.width;
	description.height = info.extent.height;
	description.texel_bytes = texel_bytes;
	description.samples = 1;
	description.pitch = (info.extent.width * texel_bytes + 63U) & ~63U;
	description.bytes = (uint64_t)description.pitch * info.extent.height;

	/* A multisampled image's raster holds each pixel's samples as a 2x2 block. */
	if (info.samples == VK_SAMPLE_COUNT_4_BIT) {
		description.samples = 4;
		description.pitch = (info.extent.width * 2U * texel_bytes + 63U) & ~63U;
		description.bytes = (uint64_t)description.pitch * info.extent.height * 2U;
	}
#if CONFIG_DRIVER_RPI4_GPU || CONFIG_DRIVER_RPI5_GPU
	/* NV12 rows are the decoder's: 32-byte pitch, luma then interleaved chroma (vulkan-video.c). */
	if (nv12)
		bcm2711_vulkan_video_nv12_layout(info.extent.width, info.extent.height, &description.pitch, &chroma, &description.bytes);
#endif
	description.required_bytes = description.bytes;
	description.alignment = 64;
	description.usage = info.usage;
	description.format = info.format;
	description.tiling = info.tiling;
	description.layout = info.initialLayout;

	/* The smaller levels follow level 0, each starting where the one above ends; all start in the initial layout. */
	description.levels = info.mipLevels;
	description.storage_bytes = description.bytes;
	for (level = 1; level < info.mipLevels; level++) {
		error = bcm2711_vulkan_image_level(&description, level, &geometry);
		if (error != 0)
			return error;
		description.storage_bytes = geometry.offset + geometry.bytes;
		description.level_layouts[level] = info.initialLayout;
	}
	/*
	 * Each array layer holds every level and starts where the one before
	 * ends (a 64-byte multiple, as every level's rows are); the layers'
	 * layouts after the first are kept with the published image.
	 */
	description.layers = info.arrayLayers;
	description.cube = cube;
	if (!nv12 && (info.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) != 0)
		description.mutable_format = true;
	layouts = 0;
	if (info.arrayLayers > 1) {
		description.layer_stride = description.storage_bytes;
		if (description.layer_stride > VULKAN_RESOURCE_BYTES / info.arrayLayers)
			return ENOTSUP;
		description.storage_bytes = description.layer_stride * info.arrayLayers;
		layouts = (info.arrayLayers - 1U) * info.mipLevels;
	}

	/* A 3D image's slices are kept as layers are, each with every level; the first slice keeps the levels' layouts. */
	if (info.imageType == VK_IMAGE_TYPE_3D) {
		description.depth = info.extent.depth;
		description.layer_stride = description.storage_bytes;
		if (description.layer_stride > VULKAN_RESOURCE_BYTES / info.extent.depth)
			return ENOTSUP;
		description.storage_bytes = description.layer_stride * info.extent.depth;
	}
	description.required_bytes = description.storage_bytes;
	error = publish_resource(session, I915_VK_OBJ_IMAGE, &description, layouts, device_id, reader, reply);
	if (error != 0)
		return error;

	/* Succeeded: the colour image has one immutable native layout and no physical ownership before binding. */
	return 0;
}

/*
 * Checks a 3D image's shape: an optimally tiled colour image of one layer
 * and one sample, sampled and copied but not drawn into (no view of a slice
 * as a 2D image), on a 7.1 engine, whose compiler reads 3D coordinates.
 */
static int
volume_shape(
	const struct bcm2711_vulkan_session *session,
	const VkImageCreateInfo *info,
	const struct bcm2711_vulkan_colour_format *colour)
{
	bool engine;

	/* The 4.2 compiler samples 2D images alone. */
	engine = bcm2711_vulkan_engine_71(session);
	if (!engine)
		return ENOTSUP;

	/*
	 * A colour image of slices, with no layers or samples of its own and
	 * no flag but a storage image's mutable format (which the image's
	 * checks take as a 2D image's).
	 */
	if (colour == NULL ||
	    (info->flags & ~(uint32_t)VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) != 0 ||
	    info->tiling != VK_IMAGE_TILING_OPTIMAL ||
	    info->arrayLayers != 1 ||
	    info->samples != VK_SAMPLE_COUNT_1_BIT ||
	    (info->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0)
		return ENOTSUP;

	/* No side longer than the most slices. */
	if (info->extent.depth == 0 ||
	    info->extent.width > BCM2711_VULKAN_IMAGE_DEPTH ||
	    info->extent.height > BCM2711_VULKAN_IMAGE_DEPTH ||
	    info->extent.depth > BCM2711_VULKAN_IMAGE_DEPTH)
		return ENOTSUP;

	/* Succeeded: the image's slices are laid out as layers. */
	return 0;
}

/*
 * Publishes a complete copied resource description after acquiring its
 * logical-device dependency.  layouts is the number of layouts an image's
 * layers after the first keep: they are allocated with the resource, each
 * starting in the description's layout.
 */
static int
publish_resource(
	struct bcm2711_vulkan_session *session,
	enum i915_vk_object_kind kind,
	struct bcm2711_vulkan_resource *description,
	uint32_t layouts,
	uint64_t device_id,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_object *device;
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_resource *resource;
	uint64_t allocator;
	uint64_t present;
	uint64_t identity;
	uint32_t index;
	int error;
	int retired;

	/* Creation completes the standard null allocator and exact guest-chosen typed output identity. */
	allocator = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 ||
	    allocator != 0 ||
	    present != 1 ||
	    identity == 0)
		return EINVAL;
	device = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DEVICE, device_id);
	if (device == NULL)
		return EINVAL;
	object = bcm2711_vulkan_object_find(session, kind, identity);
	if (object != NULL)
		return EEXIST;
	resource = kern_calloc(1, sizeof(*resource) + (size_t)layouts * sizeof(VkImageLayout));
	if (resource == NULL) {
		creation_reply(reply, VK_ERROR_OUT_OF_HOST_MEMORY, 0);
		return 0;
	}

	/* The copied description contains no generated-arena pointers or guest application addresses. */
	*resource = *description;
	resource->layer_layouts = NULL;
	if (layouts != 0) {
		resource->layer_layouts = (VkImageLayout *)(resource + 1);
		for (index = 0; index < layouts; index++)
			resource->layer_layouts[index] = description->layout;
	}
	error = bcm2711_vulkan_object_retain(device);
	if (error != 0) {
		kern_free(resource);
		return error;
	}

	/* An independent device edge exists before the resource can enter its typed registry. */
	resource->device = device;
	error = bcm2711_vulkan_object_publish(session, kind, identity, resource, release_resource, &object);
	if (error != 0) {
		retired = release_resource(session, resource);
		if (retired != 0)
			return retired;
		/* The registry's storage is the kernel's (host memory); a full device pool is the device's. */
		if (error == ENOMEM || error == ENOBUFS) {
			creation_reply(reply, VK_ERROR_OUT_OF_HOST_MEMORY, 0);
			return 0;
		}
		if (error == ENOSPC) {
			creation_reply(reply, VK_ERROR_OUT_OF_DEVICE_MEMORY, 0);
			return 0;
		}

		/* A framing refusal is distinct from a structured ordinary allocation failure. */
		return error;
	}

	/* Acknowledge only the fully published typed description. */
	creation_reply(reply, VK_SUCCESS, identity);

	/* Succeeded: the namespace owns one unbound native resource description. */
	return 0;
}

/* Releases memory and device dependency edges after the final resource or prepared-job owner retires. */
static int
release_resource(
	struct bcm2711_vulkan_session *session,
	void *payload)
{
	struct bcm2711_vulkan_resource *resource;
	int error;
	int parent_error;

	/* Bound memory remains alive until this resource's final dependent owner releases it. */
	(void)session;
	resource = payload;
	error = bcm2711_vulkan_object_release(resource->memory);
	parent_error = bcm2711_vulkan_object_release(resource->device);
	kern_free(resource);

	/* Native memory retirement errors remain visible after logical metadata retirement. */
	if (error != 0)
		return error;
	if (parent_error != 0)
		return parent_error;

	/* Succeeded: no native allocation or root is owned by this retired resource description. */
	return 0;
}

/* Withdraws only the selected resource identity while retained dependent owners keep its binding alive. */
static int
destroy_resource(
	struct bcm2711_vulkan_session *session,
	enum i915_vk_object_kind kind,
	struct i915_wire_reader *reader)
{
	struct bcm2711_vulkan_resource *resource;
	uint64_t device_id;
	uint64_t identity;
	uint64_t allocator;
	int error;

	/* Ordinary destruction uses the standard null allocator and same-device typed identity. */
	device_id = drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	allocator = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || allocator != 0)
		return EINVAL;
	if (identity == 0)
		return 0;
	resource = find_resource(session, kind, device_id, identity);
	if (resource == NULL)
		return EINVAL;
	error = bcm2711_vulkan_object_remove(session, kind, identity);
	if (error != 0)
		return error;

	/* Succeeded: only the registry's resource ownership edge retired. */
	return 0;
}

/* Returns the exact immutable size/alignment and the one native coherent memory-type bit. */
static int
memory_requirements(
	struct bcm2711_vulkan_session *session,
	enum i915_vk_object_kind kind,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_resource *resource;
	VkMemoryRequirements requirements;
	uint64_t device_id;
	uint64_t identity;
	uint64_t present;

	/* The device and typed resource must resolve before any native requirements can be reported. */
	device_id = drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || present != 1)
		return EINVAL;
	resource = find_resource(session, kind, device_id, identity);
	if (resource == NULL)
		return EINVAL;

	/* The public record matches the exact native extent required by memory binding. */
	kern_memset(&requirements, 0, sizeof(requirements));
	requirements.size = resource->required_bytes;
	requirements.alignment = resource->alignment;
	requirements.memoryTypeBits = 1;
	drv_i915_wire_reply_u64(reply, 1);
	i915_vkc_enc_VkMemoryRequirements(reply, &requirements);

	/* Succeeded: the actual client decoder can select native coherent type zero without guessing. */
	return 0;
}

/* Binds one complete native memory interval while acquiring an independent allocation dependency. */
static int
bind_memory(
	struct bcm2711_vulkan_session *session,
	enum i915_vk_object_kind kind,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_resource *resource;
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_memory *memory;
	uint64_t device_id;
	uint64_t identity;
	uint64_t allocation;
	uint64_t offset;
	int error;

	/* The client's exact memory offset follows two typed identities on the selected device. */
	device_id = drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	allocation = drv_i915_wire_read_u64(reader);
	offset = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	resource = find_resource(session, kind, device_id, identity);
	object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_MEMORY, allocation);
	if (resource == NULL ||
	    object == NULL ||
	    resource->memory != NULL)
		return EINVAL;
	memory = object->payload;
	if (memory->device != resource->device)
		return EINVAL;

	/* Unsupported, misaligned or partial intervals never mutate the existing unbound resource. */
	if (memory->view == NULL ||
	    memory->view->quarantined ||
	    (offset & (resource->alignment - 1U)) != 0 ||
	    offset > memory->bytes ||
	    resource->required_bytes > memory->bytes - offset) {
		drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY);
		return 0;
	}

	/* The binding owns the actual memory object independently of its published allocation identity. */
	error = bcm2711_vulkan_object_retain(object);
	if (error != 0)
		return error;
	resource->memory = object;
	resource->offset = offset;
	drv_i915_wire_reply_u32(reply, VK_SUCCESS);

	/* Succeeded: the resource's entire storage interval is retained until its final native owner retires. */
	return 0;
}

/* Returns the single colour subresource's raster layout only for an actual linear image. */
static int
image_layout(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_resource *resource;
	VkImageSubresource subresource;
	VkSubresourceLayout layout;
	uint64_t device_id;
	uint64_t identity;
	uint64_t input;
	uint64_t output;
#if CONFIG_DRIVER_RPI4_GPU || CONFIG_DRIVER_RPI5_GPU
	uint64_t chroma;
	uint64_t bytes;
	uint32_t pitch;
#endif

	/* Both generated input and output pointers must describe the one supported complete colour subresource. */
	device_id = drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	input = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || input != 1)
		return EINVAL;
	kern_memset(&subresource, 0, sizeof(subresource));
	i915_vkc_dec_VkImageSubresource(reader, &session->arena, &subresource);
	output = drv_i915_wire_read_u64(reader);
#if CONFIG_DRIVER_RPI4_GPU || CONFIG_DRIVER_RPI5_GPU
	/* The plane aspects of an NV12 picture are checked with its format below. */
	if (reader->error != 0 ||
	    output != 1 ||
	    subresource.mipLevel != 0 ||
	    subresource.arrayLayer != 0)
		return EINVAL;
	resource = find_resource(session, I915_VK_OBJ_IMAGE, device_id, identity);
	if (resource == NULL)
		return ENOTSUP;
#else
	if (reader->error != 0 ||
	    output != 1 ||
	    subresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
	    subresource.mipLevel != 0 ||
	    subresource.arrayLayer != 0)
		return EINVAL;
	resource = find_resource(session, I915_VK_OBJ_IMAGE, device_id, identity);
	if (resource == NULL || resource->tiling != VK_IMAGE_TILING_LINEAR)
		return ENOTSUP;
#endif

	/* The first subresource starts at zero relative to the binding, never at a host pointer or GPU VA. */
	kern_memset(&layout, 0, sizeof(layout));
	layout.size = resource->bytes;
	layout.rowPitch = resource->pitch;
	layout.arrayPitch = resource->bytes;
	layout.depthPitch = resource->bytes;
#if CONFIG_DRIVER_RPI4_GPU || CONFIG_DRIVER_RPI5_GPU
	/*
	 * An NV12 picture's planes are answered for either tiling (the private
	 * promise libvulkan's README calls N3): its rows are linear.
	 */
	if (resource->format == (VkFormat)BCM2711_VIDEO_FORMAT_NV12) {
		bcm2711_vulkan_video_nv12_layout(resource->width, resource->height, &pitch, &chroma, &bytes);
		if (subresource.aspectMask == VK_IMAGE_ASPECT_PLANE_0_BIT) {
			layout.offset = 0;
			layout.size = chroma;
		} else if (subresource.aspectMask == VK_IMAGE_ASPECT_PLANE_1_BIT) {
			layout.offset = chroma;
			layout.size = bytes - chroma;
		} else {
			return EINVAL;
		}
		layout.rowPitch = pitch;
		drv_i915_wire_reply_u64(reply, 1);
		i915_vkc_enc_VkSubresourceLayout(reply, &layout);
		return 0;
	}
	if (subresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT)
		return EINVAL;
	if (resource->tiling != VK_IMAGE_TILING_LINEAR)
		return ENOTSUP;
#endif
	drv_i915_wire_reply_u64(reply, 1);
	i915_vkc_enc_VkSubresourceLayout(reply, &layout);

	/* Succeeded: the client receives exact immutable raster pitch and extent. */
	return 0;
}

/* Finds a borrowed typed resource whose retained parent is the exact live selected logical device. */
static struct bcm2711_vulkan_resource *
find_resource(
	struct bcm2711_vulkan_session *session,
	enum i915_vk_object_kind kind,
	uint64_t device_id,
	uint64_t identity)
{
	struct bcm2711_vulkan_object *device;
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_resource *resource;

	/* Both lookups stay inside the same open's typed registry under the controller mutex. */
	device = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DEVICE, device_id);
	object = bcm2711_vulkan_object_find(session, kind, identity);
	if (device == NULL || object == NULL)
		return NULL;
	resource = object->payload;
	if (resource->device != device)
		return NULL;

	/* Succeeded: the caller borrows one immutable same-device resource description. */
	return resource;
}

/* Checks the codec's optional external-memory declaration without consuming the complete standard record. */
static int
external_declaration(
	struct i915_wire_reader *reader,
	uint32_t expected)
{
	struct i915_wire_reader checked;
	uint64_t present;
	uint64_t next;
	uint32_t type;
	uint32_t handles;

	/* A copied immutable cursor lets the ordinary generated decoder consume the same record afterward. */
	checked = *reader;
	(void)drv_i915_wire_read_u32(&checked);
	present = drv_i915_wire_read_u64(&checked);
	if (checked.error != 0 || present > 1)
		return EINVAL;
	if (present == 0)
		return 0;

	/* The complete native declaration names only opaque or private WSI sharing, with no unsupported chained extension. */
	type = drv_i915_wire_read_u32(&checked);
	next = drv_i915_wire_read_u64(&checked);
	handles = drv_i915_wire_read_u32(&checked);
	if (checked.error != 0 ||
	    type != expected ||
	    next != 0)
		return ENOTSUP;
	if (handles == 0 || (handles & ~0x201U) != 0)
		return ENOTSUP;

	/* Succeeded: the generated record's skipped extension has no unimplemented native semantics. */
	return 0;
}

/* Accepts only the native graphics queue family, including exact single-family concurrent declarations. */
static int
sharing_mode(
	VkSharingMode mode,
	uint32_t count,
	const uint32_t *families)
{
	/* Exclusive mode does not consume the standard's ignored family-index fields. */
	if (mode == VK_SHARING_MODE_EXCLUSIVE)
		return 0;
	if (mode != VK_SHARING_MODE_CONCURRENT ||
	    count != 1 ||
	    families == NULL ||
	    families[0] != 0)
		return ENOTSUP;

	/* Succeeded: one queue family owns every supported access to this native description. */
	return 0;
}

/* Writes the ordinary creation result and exact typed output identity. */
static void
creation_reply(
	struct i915_wire_writer *reply,
	VkResult status,
	uint64_t identity)
{
	/* A failed ordinary creation retains no local output identity. */
	drv_i915_wire_reply_u32(reply, (uint32_t)status);
	drv_i915_wire_reply_u64(reply, 1);
	drv_i915_wire_reply_u64(reply, identity);

	/* Succeeded: the reply carries the selected resource status and identity. */
	return;
}
