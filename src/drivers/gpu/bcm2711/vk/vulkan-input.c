/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Colour views, normalized samplers and copied SPIR-V modules are immutable inputs to later native draw preparation. */
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/vk/vulkan-input.h"
#include "drivers/gpu/bcm2711/vk/vulkan-resource.h"
#if CONFIG_DRIVER_BCM2711_VIDEO
#include "drivers/gpu/bcm2711/vk/vulkan-video.h"
#endif

/* Only device-independent generated record decoding is reused; native payload construction is separate. */
#include "drivers/gpu/i915/vk/vk-codec.inc"

/* Copied SPIR-V cannot consume the whole per-command arena or create unbounded retained source storage. */
#define VULKAN_MODULE_BYTES (128U << 10)

/* One transient standard record is decoded only for its selected typed input and never stored in a published payload. */
union vulkan_input_record {
	VkImageViewCreateInfo view;
	VkSamplerCreateInfo sampler;
	VkShaderModuleCreateInfo module;
};

static int create_input(struct bcm2711_vulkan_session *session, enum i915_vk_object_kind kind, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int destroy_input(struct bcm2711_vulkan_session *session, enum i915_vk_object_kind kind, struct i915_wire_reader *reader);
static int record_prefix(struct i915_wire_reader *reader, enum i915_vk_object_kind kind);
static int build_view(struct bcm2711_vulkan_session *session, const VkImageViewCreateInfo *info, struct bcm2711_vulkan_object *device, struct bcm2711_vulkan_input_owner **payload);
static int view_plane(const VkImageViewCreateInfo *info, const struct bcm2711_vulkan_resource *image, uint32_t *plane);
static int build_sampler(const VkSamplerCreateInfo *info, struct bcm2711_vulkan_input_owner **payload);
static int build_module(const VkShaderModuleCreateInfo *info, struct bcm2711_vulkan_input_owner **payload);
static int release_input(struct bcm2711_vulkan_session *session, void *payload);
static int identity_swizzle(VkComponentSwizzle swizzle, VkComponentSwizzle component);
static int sampler_wrap(VkSamplerAddressMode mode);
static uint32_t lod_fixed(uint32_t bits);
static void input_reply(struct i915_wire_writer *reply, VkResult status, uint64_t identity);

/*
 * Routes immutable view, sampler and owned SPIR-V module creation and final identity retirement.
 */
int
bcm2711_vulkan_input_dispatch(
	struct bcm2711_vulkan_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	enum i915_vk_object_kind kind;
	bool create;
	int error;

	/* Unimplemented input kinds remain available to later typed routers. */
	*handled = 1;
	create = false;
	switch (opcode) {
	case GPU_OP_CREATE_IMAGE_VIEW:
		create = true;
		kind = I915_VK_OBJ_IMAGE_VIEW;
		break;
	case GPU_OP_DESTROY_IMAGE_VIEW:
		kind = I915_VK_OBJ_IMAGE_VIEW;
		break;
	case GPU_OP_CREATE_SAMPLER:
		create = true;
		kind = I915_VK_OBJ_SAMPLER;
		break;
	case GPU_OP_DESTROY_SAMPLER:
		kind = I915_VK_OBJ_SAMPLER;
		break;
	case GPU_OP_CREATE_SHADER_MODULE:
		create = true;
		kind = I915_VK_OBJ_SHADER_MODULE;
		break;
	case GPU_OP_DESTROY_SHADER_MODULE:
		kind = I915_VK_OBJ_SHADER_MODULE;
		break;
	default:
		*handled = 0;
		return 0;
	}

	/* Ordinary destruction still permits an opcode echo without a parameter reply. */
	if (requested > 1 ||
	    (create &&
	     requested != 1))
		return EINVAL;

	/* Each input's immutable payload and dependencies retire only with its final retained owner. */
	if (create)
		error = create_input(session, kind, reader, reply);
	else
		error = destroy_input(session, kind, reader);
	if (error != 0)
		return error;

	/* Succeeded: one exact typed immutable input operation completed. */
	return 0;
}

/* Decodes a complete selected standard record and publishes its independently owned immutable payload. */
static int
create_input(
	struct bcm2711_vulkan_session *session,
	enum i915_vk_object_kind kind,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	union vulkan_input_record info;
	struct bcm2711_vulkan_object *device;
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_input_owner *payload;
	uint64_t device_id;
	uint64_t present;
	uint64_t allocator;
	uint64_t identity;
	int error;
	int retired;

	/* The copied prefix check supplies semantics for the generated decoder's otherwise ignored pNext/count fields. */
	device_id = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || present != 1)
		return EINVAL;
	error = record_prefix(reader, kind);
	if (error != 0)
		return error;
	kern_memset(&info, 0, sizeof(info));
	if (kind == I915_VK_OBJ_IMAGE_VIEW)
		i915_vkc_dec_VkImageViewCreateInfo(reader, &session->arena, &info.view);
	else if (kind == I915_VK_OBJ_SAMPLER)
		i915_vkc_dec_VkSamplerCreateInfo(reader, &session->arena, &info.sampler);
	else
		i915_vkc_dec_VkShaderModuleCreateInfo(reader, &session->arena, &info.module);

	/* Consume every creation field before any ordinary OOM result can leave the decoder at the next command. */
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

	/* Builders copy generated-arena data and acquire any independent image parent before returning their payload. */
	payload = NULL;
	if (kind == I915_VK_OBJ_IMAGE_VIEW)
		error = build_view(session, &info.view, device, &payload);
	else if (kind == I915_VK_OBJ_SAMPLER)
		error = build_sampler(&info.sampler, &payload);
	else
		error = build_module(&info.module, &payload);
	if (error != 0) {
		if (error == ENOMEM) {
			input_reply(reply, VK_ERROR_OUT_OF_HOST_MEMORY, 0);
			return 0;
		}

		/* Unsupported native semantics remain an explicit refusal before publication. */
		return error;
	}

	/* The root dependency is acquired before the typed input identity enters its namespace. */
	error = bcm2711_vulkan_object_retain(device);
	if (error != 0) {
		retired = release_input(session, payload);
		if (retired != 0)
			return retired;
		return error;
	}

	/* Complete the independently retained input graph before its publication transaction. */
	payload->device = device;
	error = bcm2711_vulkan_object_publish(session, kind, identity, payload, release_input, &object);
	if (error != 0) {
		retired = release_input(session, payload);
		if (retired != 0)
			return retired;
		if (error == ENOMEM || error == ENOSPC) {
			input_reply(reply, VK_ERROR_OUT_OF_HOST_MEMORY, 0);
			return 0;
		}

		/* A malformed identity cannot be hidden behind an ordinary allocation result. */
		return error;
	}

	/* Acknowledge exactly the immutable payload now owned by the typed registry. */
	input_reply(reply, VK_SUCCESS, identity);

	/* Succeeded: generated-arena storage is no longer needed by this independent input. */
	return 0;
}

/* Withdraws one typed input identity without consuming dependencies held by compiled pipelines or prepared jobs. */
static int
destroy_input(
	struct bcm2711_vulkan_session *session,
	enum i915_vk_object_kind kind,
	struct i915_wire_reader *reader)
{
	struct bcm2711_vulkan_object *device;
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_input_owner *payload;
	uint64_t device_id;
	uint64_t identity;
	uint64_t allocator;
	int error;

	/* Destruction follows the actual ordinary client's device/identity/null-allocator wire. */
	device_id = drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	allocator = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || allocator != 0)
		return EINVAL;
	if (identity == 0)
		return 0;
	device = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DEVICE, device_id);
	object = bcm2711_vulkan_object_find(session, kind, identity);
	if (device == NULL || object == NULL)
		return EINVAL;
	payload = object->payload;
	if (payload->device != device)
		return EINVAL;
	error = bcm2711_vulkan_object_remove(session, kind, identity);
	if (error != 0)
		return error;

	/* Succeeded: only the selected input's public namespace reference retired. */
	return 0;
}

/* Verifies exact supported pNext semantics and shader array extent before the generated record is decoded. */
static int
record_prefix(
	struct i915_wire_reader *reader,
	enum i915_vk_object_kind kind)
{
	struct i915_wire_reader checked;
	uint64_t chain;
	uint64_t bytes;
	uint64_t count;
	uint32_t type;
	uint32_t flags;
	uint32_t expected;

	/* The transient copied cursor checks only the selected standard prefix without changing ordinary record order. */
	checked = *reader;
	type = drv_i915_wire_read_u32(&checked);
	chain = drv_i915_wire_read_u64(&checked);
	expected = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	if (kind == I915_VK_OBJ_IMAGE_VIEW)
		expected = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	else if (kind == I915_VK_OBJ_SAMPLER)
		expected = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	if (checked.error != 0 ||
	    type != expected ||
	    chain != 0)
		return ENOTSUP;
	if (kind != I915_VK_OBJ_SHADER_MODULE)
		return 0;

	/* Source byte extent and encoded word count must describe the same complete bounded SPIR-V module. */
	flags = drv_i915_wire_read_u32(&checked);
	bytes = drv_i915_wire_read_u64(&checked);
	count = drv_i915_wire_read_u64(&checked);
	if (checked.error != 0 ||
	    flags != 0 ||
	    bytes < 20 ||
	    bytes > VULKAN_MODULE_BYTES ||
	    (bytes & 3U) != 0 ||
	    count != bytes / 4U)
		return ENOTSUP;

	/* Succeeded: the generated decoder cannot leave retained source words outside its represented code extent. */
	return 0;
}

/* Builds one full-colour image view while retaining the exact underlying typed image. */
static int
build_view(
	struct bcm2711_vulkan_session *session,
	const VkImageViewCreateInfo *info,
	struct bcm2711_vulkan_object *device,
	struct bcm2711_vulkan_input_owner **payload)
{
	struct bcm2711_vulkan_object *image;
	struct bcm2711_vulkan_resource *description;
	struct bcm2711_vulkan_image_view *view;
	uint32_t plane;
	uint32_t levels;
	int error;

	/* Views address one colour subresource, or one plane of an NV12 picture, from level 0 at its only layer. */
	if (info->flags != 0 ||
	    info->viewType != VK_IMAGE_VIEW_TYPE_2D ||
	    info->subresourceRange.baseMipLevel != 0 ||
	    info->subresourceRange.baseArrayLayer != 0)
		return ENOTSUP;
	if (info->subresourceRange.layerCount != 1 && info->subresourceRange.layerCount != VK_REMAINING_ARRAY_LAYERS)
		return ENOTSUP;
	image = bcm2711_vulkan_object_find(session, I915_VK_OBJ_IMAGE, (uint64_t)(uintptr_t)info->image);
	if (image == NULL)
		return EINVAL;
	description = image->payload;
	if (description->device != device)
		return EINVAL;

	/* A view starts at level 0 and may span the image's levels; it is sampled at level 0. */
	levels = description->levels;
	if (levels == 0)
		levels = 1;
	if (info->subresourceRange.levelCount == 0)
		return EINVAL;
	if (info->subresourceRange.levelCount != VK_REMAINING_MIP_LEVELS && info->subresourceRange.levelCount > levels)
		return EINVAL;

	/* A plane view reads its plane in the plane's own format; any other view the image's whole colour. */
	error = view_plane(info, description, &plane);
	if (error != 0)
		return error;

	/* Identity or explicit same-channel selectors have exact equivalent native TMU/TLB semantics. */
	error = identity_swizzle(info->components.r, VK_COMPONENT_SWIZZLE_R);
	if (error != 0)
		return error;
	error = identity_swizzle(info->components.g, VK_COMPONENT_SWIZZLE_G);
	if (error != 0)
		return error;
	error = identity_swizzle(info->components.b, VK_COMPONENT_SWIZZLE_B);
	if (error != 0)
		return error;
	error = identity_swizzle(info->components.a, VK_COMPONENT_SWIZZLE_A);
	if (error != 0)
		return error;
	view = kern_calloc(1, sizeof(*view));
	if (view == NULL)
		return ENOMEM;
	error = bcm2711_vulkan_object_retain(image);
	if (error != 0) {
		kern_free(view);
		return error;
	}

	/* The image dependency is independent of both image identity and later memory binding. */
	view->owner.parent = image;
	view->format = info->format;
	view->components = info->components;
	view->plane = plane;
	view->levels = levels;
	if (info->subresourceRange.levelCount != VK_REMAINING_MIP_LEVELS)
		view->levels = info->subresourceRange.levelCount;
	*payload = &view->owner;

	/* Succeeded: one immutable view owns its exact colour image independently. */
	return 0;
}

/*
 * Finds the plane a view reads.
 *
 * A colour view of the image's own format reads the whole image (plane 0),
 * as does the view of a depth/stencil image.
 * A view of an NV12 picture that the GPU samples may read its luma plane
 * as R8 (plane 1) or its interleaved chroma plane as RG8 (plane 2).
 */
static int
view_plane(
	const VkImageViewCreateInfo *info,
	const struct bcm2711_vulkan_resource *image,
	uint32_t *plane)
{
	VkImageAspectFlags aspects;

	/*
	 * A depth/stencil image (ws141-p017) is viewed whole, with every aspect
	 * its format has, as the framebuffer attachment it only can be.
	 */
	*plane = 0;
	if (image->format == VK_FORMAT_D24_UNORM_S8_UINT || image->format == VK_FORMAT_X8_D24_UNORM_PACK32) {
		if (info->format != image->format)
			return EINVAL;

		/* A combined image is viewed with depth and stencil, a depth-only one with depth alone. */
		aspects = VK_IMAGE_ASPECT_DEPTH_BIT;
		if (image->format == VK_FORMAT_D24_UNORM_S8_UINT)
			aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
		if (info->subresourceRange.aspectMask != aspects)
			return ENOTSUP;

		/* Succeeded: the attachment's whole image. */
		return 0;
	}

	/* The whole image's colour in its own format. */
	if (info->subresourceRange.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT) {
		if (info->format != image->format)
			return EINVAL;
		return 0;
	}

#if CONFIG_DRIVER_BCM2711_VIDEO
	/* Only a sampled NV12 picture has planes to view. */
	if (image->format != (VkFormat)BCM2711_VIDEO_FORMAT_NV12 || (image->usage & VK_IMAGE_USAGE_SAMPLED_BIT) == 0)
		return ENOTSUP;

	/* The luma plane as R8. */
	if (info->subresourceRange.aspectMask == VK_IMAGE_ASPECT_PLANE_0_BIT) {
		if (info->format != VK_FORMAT_R8_UNORM)
			return EINVAL;
		*plane = 1;
		return 0;
	}

	/* The chroma plane as RG8, Cb in red and Cr in green. */
	if (info->subresourceRange.aspectMask == VK_IMAGE_ASPECT_PLANE_1_BIT) {
		if (info->format != VK_FORMAT_R8G8_UNORM)
			return EINVAL;
		*plane = 2;
		return 0;
	}
#endif

	/* Any other aspect has no view. */
	return ENOTSUP;
}

/* Builds the single-level normalized sampler subset used by actual Keiland nearest and glass-linear sampling. */
static int
build_sampler(
	const VkSamplerCreateInfo *info,
	struct bcm2711_vulkan_input_owner **payload)
{
	struct bcm2711_vulkan_sampler *sampler;
	uint32_t bias;
	uint32_t minimum;
	uint32_t maximum;
	int error;

	/* Every published sampler has native filter and normalized 2D semantics; optional comparison/anisotropy remain absent. */
	if (info->flags != 0 ||
	    info->anisotropyEnable != VK_FALSE ||
	    info->compareEnable != VK_FALSE ||
	    info->unnormalizedCoordinates != VK_FALSE)
		return ENOTSUP;
	if (info->magFilter != VK_FILTER_NEAREST && info->magFilter != VK_FILTER_LINEAR)
		return ENOTSUP;
	if (info->minFilter != VK_FILTER_NEAREST && info->minFilter != VK_FILTER_LINEAR)
		return ENOTSUP;
	if (info->mipmapMode != VK_SAMPLER_MIPMAP_MODE_NEAREST && info->mipmapMode != VK_SAMPLER_MIPMAP_MODE_LINEAR)
		return ENOTSUP;
	error = sampler_wrap(info->addressModeU);
	if (error != 0)
		return error;
	error = sampler_wrap(info->addressModeV);
	if (error != 0)
		return error;
	error = sampler_wrap(info->addressModeW);
	if (error != 0)
		return error;

	/* Kernel floating-point registers remain untouched while exact float bits establish the supported one-level LOD range. */
	kern_memcpy(&bias, &info->mipLodBias, sizeof(bias));
	kern_memcpy(&minimum, &info->minLod, sizeof(minimum));
	kern_memcpy(&maximum, &info->maxLod, sizeof(maximum));
	if ((bias & 0x7fffffffU) != 0 ||
	    (minimum & 0x7fffffffU) != 0 ||
	    ((maximum & 0x80000000U) != 0 &&
	     (maximum & 0x7fffffffU) != 0) ||
	    (maximum & 0x7f800000U) == 0x7f800000U)
		return ENOTSUP;
	sampler = kern_calloc(1, sizeof(*sampler));
	if (sampler == NULL)
		return ENOMEM;

	/* Immutable draw-time fields contain no pointers into the command arena. */
	sampler->mag = info->magFilter;
	sampler->min = info->minFilter;
	sampler->u = info->addressModeU;
	sampler->v = info->addressModeV;
	if (info->mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR)
		sampler->mip_linear = 1;
	sampler->max_lod = lod_fixed(maximum);
	*payload = &sampler->owner;

	/* Succeeded: native texture setup can represent every retained sampler field. */
	return 0;
}

/* Copies complete SPIR-V source into an independently owned native module; compilation waits for its pipeline key. */
static int
build_module(
	const VkShaderModuleCreateInfo *info,
	struct bcm2711_vulkan_input_owner **payload)
{
	struct bcm2711_vulkan_module *module;
	uint32_t count;

	/* Header validity precedes copying; actual stage/entry/interface/instruction support is checked by the compiler at pipeline creation. */
	if (info->flags != 0 ||
	    info->pCode == NULL ||
	    info->pCode[0] != 0x07230203U ||
	    info->pCode[3] == 0 ||
	    info->pCode[4] != 0)
		return ENOTSUP;
	count = (uint32_t)(info->codeSize / 4U);
	module = kern_calloc(1, sizeof(*module) + (size_t)(count - 1U) * sizeof(uint32_t));
	if (module == NULL)
		return ENOMEM;

	/* The retained source is independent of the mutable per-command arena and original client submission bytes. */
	module->word_count = count;
	kern_memcpy(module->words, info->pCode, info->codeSize);
	*payload = &module->owner;

	/* Succeeded: later pipeline compilation can consume immutable independently owned SPIR-V words. */
	return 0;
}

/* Retires immutable host input storage after its independent image and device dependency edges are released. */
static int
release_input(
	struct bcm2711_vulkan_session *session,
	void *payload)
{
	struct bcm2711_vulkan_input_owner *owner;
	int error;
	int parent_error;

	/* The common prefix is the first member of every allocated immutable input payload. */
	(void)session;
	owner = payload;
	error = bcm2711_vulkan_object_release(owner->parent);
	parent_error = bcm2711_vulkan_object_release(owner->device);
	kern_free(owner);

	/* Preserve native image/memory retirement failure ahead of ordinary root retirement failure. */
	if (error != 0)
		return error;
	if (parent_error != 0)
		return parent_error;

	/* Succeeded: no retained immutable input or logical dependency remains owned by this payload. */
	return 0;
}

/* Accepts identity and explicit same-channel selectors without claiming arbitrary unlowered view swizzles. */
static int
identity_swizzle(
	VkComponentSwizzle swizzle,
	VkComponentSwizzle component)
{
	/* Other selectors require a native view-specific texture lowering key and remain unsupported. */
	if (swizzle != VK_COMPONENT_SWIZZLE_IDENTITY && swizzle != component)
		return ENOTSUP;

	/* Succeeded: this selector preserves the canonical colour component. */
	return 0;
}

/* Accepts finite native wrap modes for normalized single-level texture coordinates. */
static int
sampler_wrap(
	VkSamplerAddressMode mode)
{
	/* Border colours and mirror-clamp semantics are not exposed by the native texture subset. */
	if (mode != VK_SAMPLER_ADDRESS_MODE_REPEAT &&
	    mode != VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT &&
	    mode != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE)
		return ENOTSUP;

	/* Succeeded: native sampler-state lowering can encode this wrap mode directly. */
	return 0;
}

/* Writes one ordinary Vulkan creation result and its exact typed output identity. */
static void
input_reply(
	struct i915_wire_writer *reply,
	VkResult status,
	uint64_t identity)
{
	/* Failure keeps the requested output pointer present with a null identity. */
	drv_i915_wire_reply_u32(reply, (uint32_t)status);
	drv_i915_wire_reply_u64(reply, 1);
	drv_i915_wire_reply_u64(reply, identity);

	/* Succeeded: the reply carries the selected input status and identity. */
	return;
}

/*
 * Converts a level of detail's float bits (zero or more, finite) to the
 * sampler's 4.8 fixed point, no more than 15 and 255/256, without the
 * kernel's floating-point registers.
 */
static uint32_t
lod_fixed(
	uint32_t bits)
{
	uint32_t mantissa;
	int32_t exponent;

	/* Zero, a negative value and anything below 1/256 are level 0. */
	if ((bits & 0x80000000U) != 0)
		return 0;
	exponent = (int32_t)((bits >> 23) & 0xffU) - 127;
	if (exponent < -8)
		return 0;

	/* Sixteen and more are the largest. */
	if (exponent >= 4)
		return 0xfffU;

	/* value * 256 = mantissa * 2^(exponent - 15), the mantissa with its hidden bit. */
	mantissa = (bits & 0x7fffffU) | 0x800000U;

	/* Succeeded: the fixed-point value, truncated. */
	return mantissa >> (15 - exponent);
}
