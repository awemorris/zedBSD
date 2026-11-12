/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The descriptors of the Vulkan executor: samplers, image and buffer
 * views, descriptor set layouts, pipeline layouts, descriptor pools and
 * sets, and the hardware tables of a draw or a dispatch.
 *
 * A sampler and a view keep the descriptors the hardware reads of them
 * (section 8.3 of the specification), made once at their create.  A set
 * keeps, for each of its descriptors, the bytes a table takes of what was
 * written there, so that the tables of a draw are copies: set 0 the
 * texture descriptors and set 1 the sampler descriptors of the slots the
 * pipeline's programs use, set 2 the pipeline's resource table (a buffer
 * descriptor, or a copy of a texture descriptor, at each entry's offset),
 * and the constant bank the push constants and the dynamic offsets
 * (compiler/gsi1-abi.h).
 */

#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "vk.h"

#include "drivers/gpu/i915/vk/vk-codec.inc"

/* What a check answers for a request no valid client makes (the command fails). */
#define LAYOUT_INVALID			((uint32_t)VK_RESULT_MAX_ENUM)

/* The rows of a uniform texel buffer read as a 2D image (maxTexelBufferElements = 256 x 16384). */
#define LAYOUT_TEXEL_ROWS_MAX		16384U

/* The descriptor types that carry a dynamic offset. */
#define LAYOUT_DYNAMIC_UNIFORM		((uint32_t)VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
#define LAYOUT_DYNAMIC_STORAGE		((uint32_t)VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)

/* The most bindings of a set layout, the most descriptors of a set, and the most push-constant ranges of a pipeline layout. */
#define LAYOUT_BINDINGS_MAX		1024U
#define LAYOUT_DESCRIPTORS_MAX		4096U
#define LAYOUT_PUSH_RANGES_MAX		64U

/* The payload a write carries for a descriptor type: image infos, buffer infos or texel buffer views. */
#define LAYOUT_FAMILY_IMAGE		1U
#define LAYOUT_FAMILY_BUFFER		2U
#define LAYOUT_FAMILY_TEXEL		3U

/* The hardware tables a descriptor of a set goes into: set 0's textures, set 1's samplers, set 2's resource table. */
#define LAYOUT_TABLE_TEXTURE		0U
#define LAYOUT_TABLE_SAMPLER		1U
#define LAYOUT_TABLE_RESOURCE		2U

/* A buffer descriptor's flags: read only. */
#define LAYOUT_BUFFER_READ_ONLY		(1U << 0)

/* The hardware's swizzle of each component: the texture's own R, G, B and A. */
#define LAYOUT_SWIZZLE_R		2U
#define LAYOUT_SWIZZLE_SHIFT		3U

/* The dimensions of a texture descriptor. */
#define LAYOUT_DIMENSION_1D		0U
#define LAYOUT_DIMENSION_2D		1U
#define LAYOUT_DIMENSION_3D		2U
#define LAYOUT_DIMENSION_CUBE		3U

/* The faces of a cube. */
#define LAYOUT_CUBE_FACES		6U

/* An IEEE 754 single: the bits of its exponent and fraction, the bias of the exponent, and its largest exponent. */
#define FLOAT_FRACTION_BITS		23U
#define FLOAT_FRACTION_MASK		0x007fffffU
#define FLOAT_EXPONENT_MASK		0xffU
#define FLOAT_EXPONENT_BIAS		150
#define FLOAT_EXPONENT_SPECIAL		0xffU

/* The fixed-point fields of a sampler: the 8.8 LOD bias (within maxSamplerLodBias, 15) and the 4.4 LOD clamps. */
#define SAMPLER_BIAS_FRACTION		8U
#define SAMPLER_BIAS_MAX		(15 << 8)
#define SAMPLER_LOD_FRACTION		4U
#define SAMPLER_LOD_MAX			0xff

static int create_sampler(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int create_image_view(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static uint32_t image_view_admit(struct ghci_vk_session *session, const VkImageViewCreateInfo *info, struct ghci_vk_image_view *view);
static uint32_t image_view_descriptors(const struct ghci_vk_image *image, const VkImageViewCreateInfo *info, struct ghci_vk_image_view *view);
static int create_buffer_view(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static uint32_t buffer_view_admit(struct ghci_vk_session *session, const VkBufferViewCreateInfo *info, struct ghci_vk_buffer_view *view);
static int create_set_layout(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int set_layout_fill(struct ghci_vk_session *session, const VkDescriptorSetLayoutCreateInfo *info, struct ghci_vk_set_layout *layout);
static int create_pipeline_layout(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int create_pool(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int reset_pool(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static void pool_sets_destroy(struct ghci_vk_session *session, uint64_t pool);
static int allocate_sets(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static uint32_t set_make(struct ghci_vk_session *session, struct ghci_vk_object *pool, uint64_t layout_id, uint64_t identity);
static int free_sets(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int update_sets(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int update_write(struct ghci_vk_session *session, struct i915_wire_reader *reader);
static int write_one(struct ghci_vk_session *session, uint32_t type, uint32_t immutable, uint64_t sampler, uint64_t view, const VkDescriptorBufferInfo *buffer, struct ghci_vk_descriptor *descriptor);
static int update_copy(struct ghci_vk_session *session, const VkCopyDescriptorSet *copy);
static int destroy_object(struct ghci_vk_session *session, enum ghci_vk_kind kind, struct i915_wire_reader *reader);
static struct ghci_vk_descriptor *descriptor_at(struct ghci_vk_set *set, uint32_t binding, uint32_t element, const struct ghci_vk_layout_binding **found);
static int descriptor_fits(uint32_t table, uint32_t kind, const struct ghci_vk_layout_binding *binding, const struct ghci_vk_descriptor *descriptor);
static int kind_types(uint32_t table, uint32_t kind, uint32_t *types);
static int descriptor_walk(const struct ghci_vk_set_layout *layout, uint32_t *index, uint32_t *element);
static int layout_binding_of(const struct ghci_vk_set_layout *layout, uint32_t binding, uint32_t *index);
static uint32_t layout_family(uint32_t type);
static struct ghci_vk_device *device_of(struct ghci_vk_session *session, uint64_t handle);
static uint32_t swizzle_of(uint32_t swizzle, uint32_t own);
static int32_t float_fixed(uint32_t bits, uint32_t fraction);
static int32_t clamp_int(int32_t value, int32_t low, int32_t high);
static uint32_t float_bits(const float *value);

/*
 * Runs a sampler, view, set layout, pipeline layout, descriptor pool or
 * descriptor set command; *handled stays clear for another group's
 * number.
 */
int
ghci_vk_layout_route(
	struct ghci_vk_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	/* Picks the command, or leaves it to another group. */
	*handled = 1;
	switch (opcode) {
	case GPU_OP_CREATE_SAMPLER:
		error = create_sampler(session, requested, reader, reply);
		break;
	case GPU_OP_DESTROY_SAMPLER:
		error = destroy_object(session, GHCI_VK_SAMPLER, reader);
		break;
	case GPU_OP_CREATE_IMAGE_VIEW:
		error = create_image_view(session, requested, reader, reply);
		break;
	case GPU_OP_DESTROY_IMAGE_VIEW:
		error = destroy_object(session, GHCI_VK_IMAGE_VIEW, reader);
		break;
	case GPU_OP_CREATE_BUFFER_VIEW:
		error = create_buffer_view(session, requested, reader, reply);
		break;
	case GPU_OP_DESTROY_BUFFER_VIEW:
		error = destroy_object(session, GHCI_VK_BUFFER_VIEW, reader);
		break;
	case GPU_OP_CREATE_DESCRIPTOR_SET_LAYOUT:
		error = create_set_layout(session, requested, reader, reply);
		break;
	case GPU_OP_DESTROY_DESCRIPTOR_SET_LAYOUT:
		error = destroy_object(session, GHCI_VK_SET_LAYOUT, reader);
		break;
	case GPU_OP_CREATE_PIPELINE_LAYOUT:
		error = create_pipeline_layout(session, requested, reader, reply);
		break;
	case GPU_OP_DESTROY_PIPELINE_LAYOUT:
		error = destroy_object(session, GHCI_VK_PIPELINE_LAYOUT, reader);
		break;
	case GPU_OP_CREATE_DESCRIPTOR_POOL:
		error = create_pool(session, requested, reader, reply);
		break;
	case GPU_OP_DESTROY_DESCRIPTOR_POOL:
		error = destroy_object(session, GHCI_VK_DESCRIPTOR_POOL, reader);
		break;
	case GPU_OP_RESET_DESCRIPTOR_POOL:
		error = reset_pool(session, requested, reader, reply);
		break;
	case GPU_OP_ALLOCATE_DESCRIPTOR_SETS:
		error = allocate_sets(session, requested, reader, reply);
		break;
	case GPU_OP_FREE_DESCRIPTOR_SETS:
		error = free_sets(session, requested, reader, reply);
		break;
	case GPU_OP_UPDATE_DESCRIPTOR_SETS:
		error = update_sets(session, requested, reader, reply);
		break;
	default:
		*handled = 0;
		return 0;
	}

	/* A command that could not be decoded or run. */
	if (error != 0)
		return error;

	/* Succeeded: the command ran. */
	return 0;
}

/*
 * Reports the GPU address of an offset into a VkDeviceMemory of the
 * session.  Returns 0, or ENOENT for memory the session does not have.
 */
int
ghci_vk_bound_va(
	struct ghci_vk_session *session,
	uint64_t memory,
	uint64_t offset,
	uint64_t *va)
{
	struct ghci_vk_object *object;

	/* The memory, and its mapping in the device's session. */
	object = ghci_vk_object_find(session, GHCI_VK_MEMORY, memory);
	if (object == NULL || object->as.memory.mapping == NULL)
		return ENOENT;

	/* Succeeded: the mapping's address plus the offset. */
	*va = object->as.memory.mapping->va + offset;
	return 0;
}

/* Takes one more hold of a set layout. */
void
ghci_vk_set_layout_hold(
	struct ghci_vk_set_layout *layout)
{
	/* One more holder. */
	layout->holds++;
}

/* Lets one hold of a set layout go; the last frees it. */
void
ghci_vk_set_layout_unhold(
	struct ghci_vk_set_layout *layout)
{
	/* Nothing held. */
	if (layout == NULL)
		return;

	/* Others still hold it. */
	layout->holds--;
	if (layout->holds != 0U)
		return;

	/* The last holder: its budget, the bindings, the immutable samplers and the record. */
	if (layout->session != NULL)
		ghci_vk_budget_give(layout->session, layout->charged);
	kern_free(layout->bindings);
	kern_free(layout->samplers);
	kern_free(layout);
}

/*
 * Lets a set layout, a pipeline layout or a descriptor set go: a set
 * gives its descriptors back to its pool.
 */
void
ghci_vk_layout_release(
	struct ghci_vk_session *session,
	struct ghci_vk_object *object)
{
	struct ghci_vk_pipeline_layout *pipeline_layout;
	struct ghci_vk_object *pool;
	struct ghci_vk_set *set;
	uint32_t index;
	uint32_t type;

	/* What each kind holds. */
	switch (object->kind) {
	case GHCI_VK_SET_LAYOUT:
		ghci_vk_set_layout_unhold(object->as.set_layout);
		object->as.set_layout = NULL;
		break;
	case GHCI_VK_PIPELINE_LAYOUT:
		pipeline_layout = object->as.pipeline_layout;
		if (pipeline_layout == NULL)
			break;

		/* Each set layout it holds, then the record. */
		for (index = 0U; index < pipeline_layout->set_count; index++)
			ghci_vk_set_layout_unhold(pipeline_layout->sets[index]);
		kern_free(pipeline_layout);
		object->as.pipeline_layout = NULL;
		break;
	case GHCI_VK_DESCRIPTOR_SET:
		set = object->as.set;
		if (set == NULL)
			break;

		/* The pool counts the set and its descriptors no more (a pool destroyed first counts nothing). */
		pool = ghci_vk_object_find(session, GHCI_VK_DESCRIPTOR_POOL, set->pool);
		if (pool != NULL) {
			pool->as.pool_of_sets.sets--;
			for (index = 0U; index < set->layout->binding_count; index++) {
				type = set->layout->bindings[index].type;
				pool->as.pool_of_sets.used[type] -= set->layout->bindings[index].count;
			}
		}

		/* The descriptors, the layout's hold and the record. */
		ghci_vk_set_layout_unhold(set->layout);
		kern_free(set->descriptors);
		kern_free(set);
		object->as.set = NULL;
		break;
	default:
		break;
	}
}

/*
 * Makes the hardware tables and the constant-bank words of a draw or a
 * dispatch with a pipeline: the texture and sampler descriptors of its
 * slots and its resource table from the Vulkan sets bound at 0 to 3
 * (sets[i] the identity, 0 for none), its push constants, and its
 * dynamic offsets in the pipeline layout's order.  Returns 0, ENOENT
 * when a slot or an entry names a set or a binding that is not bound, a
 * binding of a type the slot or entry does not take, or a descriptor
 * not written, or EINVAL for dynamic offsets that are not the pipeline
 * layout's.
 */
int
ghci_vk_layout_tables(
	struct ghci_vk_session *session,
	const struct ghci_vk_pipeline *pipeline,
	const uint64_t *sets,
	const uint32_t *dynamic_offsets,
	uint32_t dynamic_count,
	const uint8_t *push,
	uint32_t push_bytes,
	struct ghci_vk_tables *tables)
{
	const struct ghci_gsi1_bindings *bindings;
	const struct ghci_gsi1_resource *entry;
	const struct ghci_vk_layout_binding *binding;
	struct ghci_vk_descriptor *descriptor;
	struct ghci_vk_object *object;
	struct ghci_vk_set *bound[GHCI_VK_SETS];
	uint32_t index;
	uint32_t bytes;
	uint32_t word;
	int error;

	/* The dynamic offsets are the pipeline layout's, and the push constants fit the bank. */
	if (dynamic_count != pipeline->dynamic_count || push_bytes > GSI1_ABI_PUSH_BYTES)
		return EINVAL;

	/* The bound sets of the session. */
	for (index = 0U; index < GHCI_VK_SETS; index++) {
		bound[index] = NULL;
		object = ghci_vk_object_find(session, GHCI_VK_DESCRIPTOR_SET, sets[index]);
		if (object != NULL)
			bound[index] = object->as.set;
	}

	/* Empty tables. */
	kern_memset(tables, 0, sizeof(*tables));
	bindings = &pipeline->bindings;

	/* Set 0: each texture slot's texture descriptor (element 0 of its binding). */
	for (index = 0U; index < bindings->texture_count; index++) {
		if (bindings->texture[index].kind == 0U)
			continue;
		if (bindings->texture[index].set >= GHCI_VK_SETS || bound[bindings->texture[index].set] == NULL)
			return ENOENT;
		descriptor = descriptor_at(bound[bindings->texture[index].set], bindings->texture[index].binding, 0U, &binding);
		if (descriptor == NULL)
			return ENOENT;
		error = descriptor_fits(LAYOUT_TABLE_TEXTURE, bindings->texture[index].kind, binding, descriptor);
		if (error != 0)
			return error;
		kern_memcpy(&tables->textures[index * GHCI_TEXTURE_DESC_BYTES], descriptor->texture, GHCI_TEXTURE_DESC_BYTES);
	}
	tables->texture_count = bindings->texture_count;

	/* Set 1: each sampler slot's sampler descriptor. */
	for (index = 0U; index < bindings->sampler_count; index++) {
		if (bindings->sampler[index].kind == 0U)
			continue;
		if (bindings->sampler[index].set >= GHCI_VK_SETS || bound[bindings->sampler[index].set] == NULL)
			return ENOENT;
		descriptor = descriptor_at(bound[bindings->sampler[index].set], bindings->sampler[index].binding, 0U, &binding);
		if (descriptor == NULL)
			return ENOENT;
		error = descriptor_fits(LAYOUT_TABLE_SAMPLER, bindings->sampler[index].kind, binding, descriptor);
		if (error != 0)
			return error;
		kern_memcpy(&tables->samplers[index * GHCI_SAMPLER_DESC_BYTES], descriptor->sampler, GHCI_SAMPLER_DESC_BYTES);
	}
	tables->sampler_count = bindings->sampler_count;

	/* Set 2: each entry, a buffer descriptor or a copy of a texture descriptor at its offset. */
	for (index = 0U; index < bindings->resource_count; index++) {
		entry = &bindings->resource[index];
		if (entry->set >= GHCI_VK_SETS || bound[entry->set] == NULL)
			return ENOENT;
		descriptor = descriptor_at(bound[entry->set], entry->binding, entry->element, &binding);
		if (descriptor == NULL)
			return ENOENT;
		error = descriptor_fits(LAYOUT_TABLE_RESOURCE, entry->kind, binding, descriptor);
		if (error != 0)
			return error;

		/* An image's texture descriptor takes two units, a buffer's descriptor one. */
		bytes = GHCI_BUFFER_DESC_BYTES;
		if (entry->kind == GHCI_GSI1_RESOURCE_STORAGE_IMAGE || entry->kind == GHCI_GSI1_RESOURCE_INPUT_ATTACHMENT)
			bytes = GHCI_TEXTURE_DESC_BYTES;
		if (entry->offset > GSI1_ABI_RESOURCE_TABLE_BYTES - bytes)
			return EINVAL;
		if (bytes == GHCI_TEXTURE_DESC_BYTES)
			kern_memcpy(&tables->resources[entry->offset], descriptor->texture, bytes);
		else
			kern_memcpy(&tables->resources[entry->offset], descriptor->buffer, bytes);
	}
	tables->resource_bytes = bindings->resource_bytes;

	/* The bank: push-constant bytes 0-95 in c0-c23 and 96-127 in c32-c39 (a last partial word zero-filled). */
	for (index = 0U; index * 4U < push_bytes; index++) {
		word = 0U;
		bytes = push_bytes - index * 4U;
		if (bytes > 4U)
			bytes = 4U;
		kern_memcpy(&word, &push[index * 4U], bytes);
		if (index < GSI1_ABI_PUSH_LOW_WORDS)
			tables->bank[GSI1_ABI_BANK_PUSH_LOW + index] = word;
		else
			tables->bank[GSI1_ABI_BANK_PUSH_HIGH + index - GSI1_ABI_PUSH_LOW_WORDS] = word;
	}

	/* The dynamic offsets from c40, one word each. */
	for (index = 0U; index < dynamic_count; index++)
		tables->bank[GSI1_ABI_BANK_DYNAMIC + index] = dynamic_offsets[index];
	tables->bank_count = GSI1_ABI_BANK_DYNAMIC + dynamic_count;

	/* Succeeded: the tables and the bank of the draw. */
	return 0;
}

/* vkCreateSampler: the create form with VkSamplerCreateInfo; the sampler descriptor of section 8.3. */
static int
create_sampler(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkSamplerCreateInfo info;
	struct ghci_sampler_desc desc;
	struct ghci_vk_sampler sampler;
	struct ghci_vk_device *device;
	struct ghci_vk_object *object;
	uint64_t handle;
	uint64_t present;
	uint64_t identity;
	uint32_t result;
	int bilinear;
	int error;

	/* The device, the create info and the tail. */
	kern_memset(&info, 0, sizeof(info));
	handle = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present == 1U)
		i915_vkc_dec_VkSamplerCreateInfo(reader, &session->arena, &info);
	identity = ghci_vk_create_tail(reader);
	if (reader->error != 0 || present != 1U)
		return EINVAL;
	device = device_of(session, handle);
	if (device == NULL)
		return EINVAL;

	/*
	 * The fields as the hardware takes them: the filters, the mip mode,
	 * the address modes and the compare operator are Vulkan's numbers;
	 * the border is transparent black, opaque black or opaque white
	 * (Vulkan's float and integer kinds of each); no anisotropy
	 * (samplerAnisotropy is false).
	 *
	 * Values referenced: amaterrace-gpu d9252e4 docs/ghci.md (section
	 * 8.3, GhciSamplerDescriptor).
	 */
	kern_memset(&desc, 0, sizeof(desc));
	desc.min_filter = info.minFilter;
	desc.mag_filter = info.magFilter;
	desc.mipmap_mode = info.mipmapMode;
	desc.address[0] = info.addressModeU;
	desc.address[1] = info.addressModeV;
	desc.address[2] = info.addressModeW;
	desc.compare_enable = info.compareEnable != VK_FALSE ? 1U : 0U;
	desc.compare_op = info.compareOp;
	desc.lod_bias_fx8 = clamp_int(float_fixed(float_bits(&info.mipLodBias), SAMPLER_BIAS_FRACTION), -SAMPLER_BIAS_MAX, SAMPLER_BIAS_MAX);
	desc.min_lod_fx4 = (uint32_t)clamp_int(float_fixed(float_bits(&info.minLod), SAMPLER_LOD_FRACTION), 0, SAMPLER_LOD_MAX);
	desc.max_lod_fx4 = (uint32_t)clamp_int(float_fixed(float_bits(&info.maxLod), SAMPLER_LOD_FRACTION), 0, SAMPLER_LOD_MAX);
	desc.max_anisotropy = 1U;
	desc.border_type = (uint32_t)info.borderColor / 2U;
	desc.unnormalized = info.unnormalizedCoordinates != VK_FALSE ? 1U : 0U;

	/*
	 * Without the texture unit's bilinear filter no format has
	 * FILTER_LINEAR, which a linear filter or mip mode needs, so the
	 * sampler takes the nearest texel of the nearest level.
	 *
	 * Values referenced: amaterrace-gpu ad23fe0 docs/ghci.md (section
	 * 3.2.2, EXT_CAP_FMT0 [16] CAP_TU_BILINEAR).
	 */
	bilinear = ghci_caps_has(&session->device->caps, GHCI_HAS_TU_BILINEAR);
	if (!bilinear) {
		if (desc.min_filter == (uint32_t)VK_FILTER_LINEAR)
			desc.min_filter = (uint32_t)VK_FILTER_NEAREST;
		if (desc.mag_filter == (uint32_t)VK_FILTER_LINEAR)
			desc.mag_filter = (uint32_t)VK_FILTER_NEAREST;
		if (desc.mipmap_mode == (uint32_t)VK_SAMPLER_MIPMAP_MODE_LINEAR)
			desc.mipmap_mode = (uint32_t)VK_SAMPLER_MIPMAP_MODE_NEAREST;
	}

	/* Anisotropy is a feature the device lacks; a field the descriptor cannot hold is refused. */
	result = (uint32_t)VK_SUCCESS;
	if (info.anisotropyEnable != VK_FALSE) {
		kern_logf("ghci: %s: vk: a sampler with anisotropy refused\n", session->device->name);
		result = (uint32_t)VK_ERROR_FEATURE_NOT_PRESENT;
	} else {
		error = ghci_sampler_desc_encode(sampler.descriptor, &desc);
		if (error != 0) {
			kern_logf("ghci: %s: vk: a sampler the descriptor does not hold refused\n", session->device->name);
			result = (uint32_t)VK_ERROR_FEATURE_NOT_PRESENT;
		}
	}

	/* The object. */
	if (result == (uint32_t)VK_SUCCESS) {
		error = ghci_vk_object_add(session, GHCI_VK_SAMPLER, identity, device, &object);
		if (error != 0)
			return error;
		object->as.sampler = sampler;
	}

	/* The result, and the identity when it succeeded. */
	ghci_vk_reply_created(reply, requested, result, identity);

	/* Succeeded: the sampler exists, or the reply says why not. */
	return 0;
}

/* vkCreateImageView: the create form with VkImageViewCreateInfo. */
static int
create_image_view(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkImageViewCreateInfo info;
	struct ghci_vk_image_view view;
	struct ghci_vk_device *device;
	struct ghci_vk_object *object;
	uint64_t handle;
	uint64_t present;
	uint64_t identity;
	uint32_t result;
	int error;

	/* The device, the create info and the tail. */
	kern_memset(&info, 0, sizeof(info));
	handle = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present == 1U)
		i915_vkc_dec_VkImageViewCreateInfo(reader, &session->arena, &info);
	identity = ghci_vk_create_tail(reader);
	if (reader->error != 0 || present != 1U)
		return EINVAL;
	device = device_of(session, handle);
	if (device == NULL)
		return EINVAL;

	/* The view and its descriptors; an image that is not the session's or not bound fails the command. */
	kern_memset(&view, 0, sizeof(view));
	result = image_view_admit(session, &info, &view);
	if (result == LAYOUT_INVALID)
		return EINVAL;

	/* The object. */
	if (result == (uint32_t)VK_SUCCESS) {
		error = ghci_vk_object_add(session, GHCI_VK_IMAGE_VIEW, identity, device, &object);
		if (error != 0)
			return error;
		object->as.image_view = view;
	}

	/* The result, and the identity when it succeeded. */
	ghci_vk_reply_created(reply, requested, result, identity);

	/* Succeeded: the view exists, or the reply says why not. */
	return 0;
}

/*
 * Checks an image view (its image of the session, bound, the subresource
 * range within it, a format the hardware takes, no cube array, no
 * stencil aspect alone of a combined format) and fills its record.
 * Returns the VkResult, or LAYOUT_INVALID for a view no valid client
 * makes.
 */
static uint32_t
image_view_admit(
	struct ghci_vk_session *session,
	const VkImageViewCreateInfo *info,
	struct ghci_vk_image_view *view)
{
	const struct ghci_vk_image *image;
	struct ghci_vk_object *object;
	uint64_t va;
	uint32_t levels;
	uint32_t layers;
	uint32_t traits;
	uint32_t bytes;
	uint32_t block;
	int error;

	/* The image, of the session and bound to memory. */
	object = ghci_vk_object_find(session, GHCI_VK_IMAGE, (uint64_t)(uintptr_t)info->image);
	if (object == NULL || object->as.image.memory == 0U)
		return LAYOUT_INVALID;
	image = &object->as.image;
	error = ghci_vk_bound_va(session, image->memory, image->offset, &va);
	if (error != 0)
		return LAYOUT_INVALID;

	/* The levels and layers, the remaining ones for VK_REMAINING_*. */
	levels = info->subresourceRange.levelCount;
	if (levels == VK_REMAINING_MIP_LEVELS)
		levels = image->layout.levels - info->subresourceRange.baseMipLevel;
	layers = info->subresourceRange.layerCount;
	if (layers == VK_REMAINING_ARRAY_LAYERS)
		layers = image->layout.layers - info->subresourceRange.baseArrayLayer;
	if (info->subresourceRange.baseMipLevel >= image->layout.levels || levels == 0U || levels > image->layout.levels - info->subresourceRange.baseMipLevel)
		return LAYOUT_INVALID;
	if (info->subresourceRange.baseArrayLayer >= image->layout.layers || layers == 0U || layers > image->layout.layers - info->subresourceRange.baseArrayLayer)
		return LAYOUT_INVALID;

	/* A format the hardware takes, of the image's texel size. */
	error = ghci_vk_format_size(info->format, &bytes, &block);
	if (error != 0 || bytes != image->layout.texel_bytes || block != image->layout.block) {
		kern_logf("ghci: %s: vk: an image view of format %u refused\n", session->device->name, (unsigned)info->format);
		return (uint32_t)VK_ERROR_FORMAT_NOT_SUPPORTED;
	}

	/* No cube array (imageCubeArray is false). */
	if (info->viewType == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) {
		kern_logf("ghci: %s: vk: a cube array view refused\n", session->device->name);
		return (uint32_t)VK_ERROR_FEATURE_NOT_PRESENT;
	}

	/* A texture descriptor reads depth and stencil interleaved: the stencil alone of a combined format is not taken. */
	(void)ghci_vk_format_traits(info->format, &traits);
	if ((traits & GHCI_VK_FORMAT_DEPTH) != 0U && info->subresourceRange.aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT) {
		kern_logf("ghci: %s: vk: a view of the stencil aspect alone refused\n", session->device->name);
		return (uint32_t)VK_ERROR_FORMAT_NOT_SUPPORTED;
	}

	/* The record: the image, the range, and the address of its first level and layer. */
	view->image = (uint64_t)(uintptr_t)info->image;
	view->format = info->format;
	view->view_type = info->viewType;
	view->aspect = info->subresourceRange.aspectMask;
	view->base_level = info->subresourceRange.baseMipLevel;
	view->levels = levels;
	view->base_layer = info->subresourceRange.baseArrayLayer;
	view->layers = layers;
	view->samples = image->layout.samples;
	view->width = ghci_layout_extent(image->layout.width, view->base_level);
	view->height = ghci_layout_extent(image->layout.height, view->base_level);
	view->row_pitch = ghci_layout_row(&image->layout, view->base_level);
	view->layer_stride = ghci_layout_layer_stride(&image->layout);
	view->va = va + (uint64_t)view->base_layer * ghci_layout_layer_stride(&image->layout);
	view->va += ghci_layout_level_offset(&image->layout, view->base_level);

	/* Succeeded when the descriptors hold the view. */
	return image_view_descriptors(image, info, view);
}

/*
 * Fills the two texture descriptors of an image view: sampled, its levels
 * from the first with every layer of the view; single, the first level
 * alone (a storage image and an input attachment).  Both start at the
 * view's first level and layer; the layers (or a 3D level's slices) are
 * a slice pitch apart.  Returns the VkResult.
 */
static uint32_t
image_view_descriptors(
	const struct ghci_vk_image *image,
	const VkImageViewCreateInfo *info,
	struct ghci_vk_image_view *view)
{
	struct ghci_texture_desc desc;
	uint32_t traits;
	uint32_t depth;
	uint32_t slice;
	uint32_t dimension;
	int error;

	/*
	 * The depth (a 3D level's slices, or the view's layers) and the
	 * slice pitch (a plane of a 3D level, or the layer stride), and the
	 * dimension: 1D and 1D arrays, 2D and 2D arrays, 3D, and the cube
	 * of six layers.
	 *
	 * Values referenced: amaterrace-gpu d9252e4 docs/ghci.md (section
	 * 8.5.2, the dimensions and the subresource placement).
	 */
	depth = view->layers;
	slice = ghci_layout_layer_stride(&image->layout);
	dimension = LAYOUT_DIMENSION_2D;
	switch (info->viewType) {
	case VK_IMAGE_VIEW_TYPE_1D:
	case VK_IMAGE_VIEW_TYPE_1D_ARRAY:
		dimension = LAYOUT_DIMENSION_1D;
		break;
	case VK_IMAGE_VIEW_TYPE_3D:
		dimension = LAYOUT_DIMENSION_3D;
		depth = ghci_layout_extent(image->layout.depth, view->base_level);
		slice = ghci_layout_plane(&image->layout, view->base_level);
		break;
	case VK_IMAGE_VIEW_TYPE_CUBE:
		dimension = LAYOUT_DIMENSION_CUBE;
		depth = LAYOUT_CUBE_FACES;
		break;
	default:
		break;
	}

	/* The sampled descriptor: the view's levels, its format and swizzle. */
	kern_memset(&desc, 0, sizeof(desc));
	desc.base = view->va;
	desc.width = view->width;
	desc.height = view->height;
	desc.depth = depth;
	desc.format = info->format;
	desc.mip_levels = view->levels;
	desc.row_pitch = view->row_pitch;
	desc.slice_pitch = slice;
	desc.samples = view->samples;
	desc.dimension = dimension;
	(void)ghci_vk_format_traits(info->format, &traits);
	if ((traits & GHCI_VK_FORMAT_SRGB) != 0U)
		desc.flags = GHCI_TEX_FLAG_SRGB;
	desc.swizzle = swizzle_of(info->components.r, 0U);
	desc.swizzle |= swizzle_of(info->components.g, 1U) << LAYOUT_SWIZZLE_SHIFT;
	desc.swizzle |= swizzle_of(info->components.b, 2U) << (2U * LAYOUT_SWIZZLE_SHIFT);
	desc.swizzle |= swizzle_of(info->components.a, 3U) << (3U * LAYOUT_SWIZZLE_SHIFT);
	error = ghci_texture_desc_encode(view->sampled, &desc);
	if (error != 0)
		return (uint32_t)VK_ERROR_FORMAT_NOT_SUPPORTED;

	/* The single descriptor: one level, a cube as its six layers, the components as they are. */
	desc.mip_levels = 1U;
	if (dimension == LAYOUT_DIMENSION_CUBE)
		desc.dimension = LAYOUT_DIMENSION_2D;
	desc.swizzle = swizzle_of(VK_COMPONENT_SWIZZLE_IDENTITY, 0U);
	desc.swizzle |= swizzle_of(VK_COMPONENT_SWIZZLE_IDENTITY, 1U) << LAYOUT_SWIZZLE_SHIFT;
	desc.swizzle |= swizzle_of(VK_COMPONENT_SWIZZLE_IDENTITY, 2U) << (2U * LAYOUT_SWIZZLE_SHIFT);
	desc.swizzle |= swizzle_of(VK_COMPONENT_SWIZZLE_IDENTITY, 3U) << (3U * LAYOUT_SWIZZLE_SHIFT);
	error = ghci_texture_desc_encode(view->single, &desc);
	if (error != 0)
		return (uint32_t)VK_ERROR_FORMAT_NOT_SUPPORTED;

	/* Succeeded: both descriptors. */
	return (uint32_t)VK_SUCCESS;
}

/* vkCreateBufferView: the create form with VkBufferViewCreateInfo. */
static int
create_buffer_view(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkBufferViewCreateInfo info;
	struct ghci_vk_buffer_view view;
	struct ghci_vk_device *device;
	struct ghci_vk_object *object;
	uint64_t handle;
	uint64_t present;
	uint64_t identity;
	uint32_t result;
	int error;

	/* The device, the create info and the tail. */
	kern_memset(&info, 0, sizeof(info));
	handle = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present == 1U)
		i915_vkc_dec_VkBufferViewCreateInfo(reader, &session->arena, &info);
	identity = ghci_vk_create_tail(reader);
	if (reader->error != 0 || present != 1U)
		return EINVAL;
	device = device_of(session, handle);
	if (device == NULL)
		return EINVAL;

	/* The view and its descriptors; a buffer that is not the session's or not bound fails the command. */
	kern_memset(&view, 0, sizeof(view));
	result = buffer_view_admit(session, &info, &view);
	if (result == LAYOUT_INVALID)
		return EINVAL;

	/* The object. */
	if (result == (uint32_t)VK_SUCCESS) {
		error = ghci_vk_object_add(session, GHCI_VK_BUFFER_VIEW, identity, device, &object);
		if (error != 0)
			return error;
		object->as.buffer_view = view;
	}

	/* The result, and the identity when it succeeded. */
	ghci_vk_reply_created(reply, requested, result, identity);

	/* Succeeded: the view exists, or the reply says why not. */
	return 0;
}

/*
 * Checks a buffer view (its buffer of the session, bound, the range
 * within it, a texel format the hardware takes, at most
 * maxTexelBufferElements) and fills its buffer descriptor and the texture
 * descriptor of a uniform texel buffer: the view's texels as a linear 2D
 * image 256 texels wide, element i at (i mod 256, i / 256).  Returns the
 * VkResult, or LAYOUT_INVALID for a view no valid client makes.
 */
static uint32_t
buffer_view_admit(
	struct ghci_vk_session *session,
	const VkBufferViewCreateInfo *info,
	struct ghci_vk_buffer_view *view)
{
	const struct ghci_vk_buffer *buffer;
	struct ghci_vk_object *object;
	struct ghci_buffer_desc buffer_desc;
	struct ghci_texture_desc texture;
	uint64_t range;
	uint64_t va;
	uint64_t elements;
	uint32_t bytes;
	uint32_t block;
	int error;

	/* The buffer, of the session and bound to memory. */
	object = ghci_vk_object_find(session, GHCI_VK_BUFFER, (uint64_t)(uintptr_t)info->buffer);
	if (object == NULL || object->as.buffer.memory == 0U)
		return LAYOUT_INVALID;
	buffer = &object->as.buffer;
	error = ghci_vk_bound_va(session, buffer->memory, buffer->offset + info->offset, &va);
	if (error != 0)
		return LAYOUT_INVALID;

	/* The range, the rest of the buffer for VK_WHOLE_SIZE, within the buffer. */
	if (info->offset >= buffer->size)
		return LAYOUT_INVALID;
	range = info->range;
	if (range == VK_WHOLE_SIZE)
		range = buffer->size - info->offset;
	if (range > buffer->size - info->offset)
		return LAYOUT_INVALID;

	/* A texel format the hardware takes, and at least one element. */
	error = ghci_vk_format_size(info->format, &bytes, &block);
	if (error != 0 || block != 0U) {
		kern_logf("ghci: %s: vk: a buffer view of format %u refused\n", session->device->name, (unsigned)info->format);
		return (uint32_t)VK_ERROR_FORMAT_NOT_SUPPORTED;
	}
	elements = range / bytes;
	if (elements == 0U)
		return LAYOUT_INVALID;
	if (elements > (uint64_t)GSI1_ABI_TEXEL_BUFFER_WIDTH * LAYOUT_TEXEL_ROWS_MAX) {
		kern_logf("ghci: %s: vk: a buffer view past maxTexelBufferElements refused\n", session->device->name);
		return (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY;
	}

	/*
	 * The buffer descriptor: the view's address and bytes, the texel's
	 * stride and the format.
	 *
	 * Values referenced: amaterrace-gpu d9252e4 docs/ghci.md (section
	 * 8.3, GhciBufferDescriptor).
	 */
	kern_memset(&buffer_desc, 0, sizeof(buffer_desc));
	buffer_desc.base = va;
	buffer_desc.bytes = (uint32_t)(elements * bytes);
	buffer_desc.stride = bytes;
	buffer_desc.format = info->format;
	error = ghci_buffer_desc_encode(view->buffer_descriptor, &buffer_desc);
	if (error != 0)
		return (uint32_t)VK_ERROR_FORMAT_NOT_SUPPORTED;

	/* The texture descriptor: 256 texels a row (fewer for a short view), as many rows as it takes. */
	kern_memset(&texture, 0, sizeof(texture));
	texture.base = va;
	texture.width = GSI1_ABI_TEXEL_BUFFER_WIDTH;
	if (elements < GSI1_ABI_TEXEL_BUFFER_WIDTH)
		texture.width = (uint32_t)elements;
	texture.height = (uint32_t)((elements + GSI1_ABI_TEXEL_BUFFER_WIDTH - 1U) >> GSI1_ABI_TEXEL_BUFFER_SHIFT);
	texture.depth = 1U;
	texture.format = info->format;
	texture.mip_levels = 1U;
	texture.row_pitch = GSI1_ABI_TEXEL_BUFFER_WIDTH * bytes;
	texture.slice_pitch = texture.row_pitch * texture.height;
	texture.samples = 1U;
	texture.dimension = LAYOUT_DIMENSION_2D;
	texture.swizzle = swizzle_of(VK_COMPONENT_SWIZZLE_IDENTITY, 0U);
	texture.swizzle |= swizzle_of(VK_COMPONENT_SWIZZLE_IDENTITY, 1U) << LAYOUT_SWIZZLE_SHIFT;
	texture.swizzle |= swizzle_of(VK_COMPONENT_SWIZZLE_IDENTITY, 2U) << (2U * LAYOUT_SWIZZLE_SHIFT);
	texture.swizzle |= swizzle_of(VK_COMPONENT_SWIZZLE_IDENTITY, 3U) << (3U * LAYOUT_SWIZZLE_SHIFT);
	error = ghci_texture_desc_encode(view->texture, &texture);
	if (error != 0)
		return (uint32_t)VK_ERROR_FORMAT_NOT_SUPPORTED;

	/* Succeeded: the record. */
	view->buffer = (uint64_t)(uintptr_t)info->buffer;
	view->format = info->format;
	view->elements = (uint32_t)elements;
	return (uint32_t)VK_SUCCESS;
}

/* vkCreateDescriptorSetLayout: the create form with VkDescriptorSetLayoutCreateInfo. */
static int
create_set_layout(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkDescriptorSetLayoutCreateInfo info;
	struct ghci_vk_set_layout *layout;
	struct ghci_vk_device *device;
	struct ghci_vk_object *object;
	uint64_t handle;
	uint64_t present;
	uint64_t identity;
	uint64_t bytes;
	int error;

	/* The device, the create info and the tail. */
	kern_memset(&info, 0, sizeof(info));
	handle = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present == 1U)
		i915_vkc_dec_VkDescriptorSetLayoutCreateInfo(reader, &session->arena, &info);
	identity = ghci_vk_create_tail(reader);
	if (reader->error != 0 || present != 1U)
		return EINVAL;
	device = device_of(session, handle);
	if (device == NULL)
		return EINVAL;

	/* The layout, held by its object. */
	layout = kern_calloc(1U, sizeof(*layout));
	if (layout == NULL)
		return ENOMEM;
	layout->holds = 1U;
	error = set_layout_fill(session, &info, layout);
	if (error != 0) {
		ghci_vk_set_layout_unhold(layout);
		return error;
	}

	/* Its bytes, within the session's budget (given back when the last hold goes). */
	bytes = sizeof(*layout) + (uint64_t)layout->binding_count * sizeof(*layout->bindings);
	if (layout->samplers != NULL)
		bytes += (uint64_t)layout->descriptor_count * GHCI_SAMPLER_DESC_BYTES;
	error = ghci_vk_budget_take(session, bytes);
	if (error != 0) {
		ghci_vk_set_layout_unhold(layout);
		ghci_vk_reply_created(reply, requested, (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY, identity);
		return 0;
	}
	layout->session = session;
	layout->charged = bytes;

	/* The object. */
	error = ghci_vk_object_add(session, GHCI_VK_SET_LAYOUT, identity, device, &object);
	if (error != 0) {
		ghci_vk_set_layout_unhold(layout);
		return error;
	}
	object->as.set_layout = layout;

	/* The result and the identity. */
	ghci_vk_reply_created(reply, requested, (uint32_t)VK_SUCCESS, identity);

	/* Succeeded: the layout exists. */
	return 0;
}

/*
 * Fills a set layout: its bindings in increasing binding order, each
 * one's first descriptor, the dynamic buffers, and the immutable
 * samplers' descriptors.  Returns 0, EINVAL for a layout no valid client
 * makes, or ENOMEM.
 */
static int
set_layout_fill(
	struct ghci_vk_session *session,
	const VkDescriptorSetLayoutCreateInfo *info,
	struct ghci_vk_set_layout *layout)
{
	const VkDescriptorSetLayoutBinding *given;
	struct ghci_vk_layout_binding moved;
	struct ghci_vk_object *sampler;
	uint64_t decoded;
	uint32_t index;
	uint32_t place;
	uint32_t element;
	uint32_t first;

	/* A bounded number of bindings of the Vulkan 1.0 types, each one decoded. */
	decoded = ghci_vk_decoded_count(session, info->pBindings);
	if (info->bindingCount > LAYOUT_BINDINGS_MAX || decoded != info->bindingCount)
		return EINVAL;
	if (info->bindingCount != 0U) {
		layout->bindings = kern_calloc(info->bindingCount, sizeof(*layout->bindings));
		if (layout->bindings == NULL)
			return ENOMEM;
	}

	/* Each binding, kept in increasing binding order (an insertion). */
	for (index = 0U; index < info->bindingCount; index++) {
		given = &info->pBindings[index];
		if ((uint32_t)given->descriptorType >= GHCI_VK_DESCRIPTOR_TYPES)
			return EINVAL;
		decoded = ghci_vk_decoded_count(session, given->pImmutableSamplers);
		if (given->pImmutableSamplers != NULL && decoded != given->descriptorCount)
			return EINVAL;
		place = index;
		while (place > 0U && layout->bindings[place - 1U].binding > given->binding) {
			layout->bindings[place] = layout->bindings[place - 1U];
			place--;
		}
		kern_memset(&moved, 0, sizeof(moved));
		moved.binding = given->binding;
		moved.type = given->descriptorType;
		moved.count = given->descriptorCount;
		moved.stages = given->stageFlags;
		if ((moved.type == (uint32_t)VK_DESCRIPTOR_TYPE_SAMPLER || moved.type == (uint32_t)VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) &&
		    given->pImmutableSamplers != NULL)
			moved.immutable = 1U;
		layout->bindings[place] = moved;
	}
	layout->binding_count = info->bindingCount;

	/* The first descriptor of each binding (in the order of the bindings), the dynamic ones, and no binding twice. */
	first = 0U;
	for (index = 0U; index < layout->binding_count; index++) {
		if (index > 0U && layout->bindings[index - 1U].binding == layout->bindings[index].binding)
			return EINVAL;
		layout->bindings[index].first = first;
		if (layout->bindings[index].count > LAYOUT_DESCRIPTORS_MAX - first)
			return EINVAL;
		first += layout->bindings[index].count;
		if (layout->bindings[index].type == LAYOUT_DYNAMIC_UNIFORM || layout->bindings[index].type == LAYOUT_DYNAMIC_STORAGE)
			layout->dynamic_count += layout->bindings[index].count;
	}
	layout->descriptor_count = first;

	/* The immutable samplers' descriptors, by descriptor index. */
	for (index = 0U; index < info->bindingCount; index++) {
		given = &info->pBindings[index];
		if ((given->descriptorType != VK_DESCRIPTOR_TYPE_SAMPLER && given->descriptorType != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) ||
		    given->pImmutableSamplers == NULL)
			continue;
		if (layout->samplers == NULL) {
			layout->samplers = kern_calloc(layout->descriptor_count, GHCI_SAMPLER_DESC_BYTES);
			if (layout->samplers == NULL)
				return ENOMEM;
		}
		(void)layout_binding_of(layout, given->binding, &place);
		for (element = 0U; element < given->descriptorCount; element++) {
			sampler = ghci_vk_object_find(session, GHCI_VK_SAMPLER, (uint64_t)(uintptr_t)given->pImmutableSamplers[element]);
			if (sampler == NULL)
				return EINVAL;
			kern_memcpy(&layout->samplers[(layout->bindings[place].first + element) * GHCI_SAMPLER_DESC_BYTES],
				    sampler->as.sampler.descriptor,
				    GHCI_SAMPLER_DESC_BYTES);
		}
	}

	/* Succeeded: the layout is filled. */
	return 0;
}

/*
 * vkCreatePipelineLayout: the create form with VkPipelineLayoutCreateInfo.
 * The layout holds its set layouts and lists the dynamic buffers of
 * every set, set by set and binding by binding, one per descriptor, which
 * is the order of the dynamic offsets (bank words 40 and on).
 */
static int
create_pipeline_layout(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkPipelineLayoutCreateInfo info;
	struct ghci_vk_pipeline_layout *layout;
	struct ghci_vk_set_layout *set_layout;
	struct ghci_vk_device *device;
	struct ghci_vk_object *object;
	uint64_t handle;
	uint64_t present;
	uint64_t identity;
	uint64_t decoded;
	uint32_t index;
	uint32_t binding;
	uint32_t element;
	uint32_t end;
	int error;

	/* The device, the create info and the tail. */
	kern_memset(&info, 0, sizeof(info));
	handle = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present == 1U)
		i915_vkc_dec_VkPipelineLayoutCreateInfo(reader, &session->arena, &info);
	identity = ghci_vk_create_tail(reader);
	if (reader->error != 0 || present != 1U)
		return EINVAL;
	device = device_of(session, handle);
	if (device == NULL)
		return EINVAL;

	/* At most the bound sets' number, each a layout of the session, and a bounded number of decoded push-constant ranges. */
	decoded = ghci_vk_decoded_count(session, info.pSetLayouts);
	if (info.setLayoutCount > GHCI_VK_SETS || decoded != info.setLayoutCount)
		return EINVAL;
	decoded = ghci_vk_decoded_count(session, info.pPushConstantRanges);
	if (info.pushConstantRangeCount > LAYOUT_PUSH_RANGES_MAX || decoded != info.pushConstantRangeCount)
		return EINVAL;
	layout = kern_calloc(1U, sizeof(*layout));
	if (layout == NULL)
		return ENOMEM;
	error = 0;
	for (index = 0U; index < info.setLayoutCount; index++) {
		object = ghci_vk_object_find(session, GHCI_VK_SET_LAYOUT, (uint64_t)(uintptr_t)info.pSetLayouts[index]);
		if (object == NULL) {
			error = EINVAL;
			break;
		}
		set_layout = object->as.set_layout;
		ghci_vk_set_layout_hold(set_layout);
		layout->sets[index] = set_layout;
		layout->set_count = index + 1U;

		/* The set's dynamic buffers, one per descriptor, in the order of their offsets, from its first. */
		layout->dynamic_base[index] = layout->dynamic_count;
		for (binding = 0U; binding < set_layout->binding_count && error == 0; binding++) {
			if (set_layout->bindings[binding].type != LAYOUT_DYNAMIC_UNIFORM && set_layout->bindings[binding].type != LAYOUT_DYNAMIC_STORAGE)
				continue;
			for (element = 0U; element < set_layout->bindings[binding].count; element++) {
				if (layout->dynamic_count >= GHCI_GSI1_DYNAMIC_OFFSETS) {
					error = EINVAL;
					break;
				}
				layout->dynamic[layout->dynamic_count].set = index;
				layout->dynamic[layout->dynamic_count].binding = set_layout->bindings[binding].binding;
				layout->dynamic_count++;
			}
		}
		if (error != 0)
			break;
		layout->dynamic_sets[index] = layout->dynamic_count - layout->dynamic_base[index];
	}

	/* The sets past the layout's have no dynamic offset, after the last. */
	for (index = layout->set_count; index < GHCI_VK_SETS; index++)
		layout->dynamic_base[index] = layout->dynamic_count;

	/* The push constants' bytes: the end of the furthest range, at most 128. */
	for (index = 0U; index < info.pushConstantRangeCount && error == 0; index++) {
		end = info.pPushConstantRanges[index].offset + info.pPushConstantRanges[index].size;
		if (end > GSI1_ABI_PUSH_BYTES || end < info.pPushConstantRanges[index].offset) {
			error = EINVAL;
			break;
		}
		if (end > layout->push_bytes)
			layout->push_bytes = end;
	}

	/* A layout no valid client makes lets its holds go. */
	if (error != 0) {
		for (index = 0U; index < layout->set_count; index++)
			ghci_vk_set_layout_unhold(layout->sets[index]);
		kern_free(layout);
		return error;
	}

	/* Its bytes, within the session's budget. */
	error = ghci_vk_budget_take(session, sizeof(*layout));
	if (error != 0) {
		for (index = 0U; index < layout->set_count; index++)
			ghci_vk_set_layout_unhold(layout->sets[index]);
		kern_free(layout);
		ghci_vk_reply_created(reply, requested, (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY, identity);
		return 0;
	}

	/* The object. */
	error = ghci_vk_object_add(session, GHCI_VK_PIPELINE_LAYOUT, identity, device, &object);
	if (error != 0) {
		ghci_vk_budget_give(session, sizeof(*layout));
		for (index = 0U; index < layout->set_count; index++)
			ghci_vk_set_layout_unhold(layout->sets[index]);
		kern_free(layout);
		return error;
	}
	object->as.pipeline_layout = layout;
	object->charged = sizeof(*layout);

	/* The result and the identity. */
	ghci_vk_reply_created(reply, requested, (uint32_t)VK_SUCCESS, identity);

	/* Succeeded: the layout exists. */
	return 0;
}

/* vkCreateDescriptorPool: the create form with VkDescriptorPoolCreateInfo; the pool counts its sets and descriptors. */
static int
create_pool(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkDescriptorPoolCreateInfo info;
	struct ghci_vk_descriptor_pool pool;
	struct ghci_vk_device *device;
	struct ghci_vk_object *object;
	uint64_t handle;
	uint64_t present;
	uint64_t identity;
	uint64_t decoded;
	uint32_t index;
	uint32_t type;
	int error;

	/* The device, the create info and the tail. */
	kern_memset(&info, 0, sizeof(info));
	handle = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present == 1U)
		i915_vkc_dec_VkDescriptorPoolCreateInfo(reader, &session->arena, &info);
	identity = ghci_vk_create_tail(reader);
	if (reader->error != 0 || present != 1U)
		return EINVAL;
	device = device_of(session, handle);
	if (device == NULL)
		return EINVAL;

	/* The sets and the descriptors of each type it has. */
	kern_memset(&pool, 0, sizeof(pool));
	pool.flags = info.flags;
	pool.max_sets = info.maxSets;
	decoded = ghci_vk_decoded_count(session, info.pPoolSizes);
	if (decoded != info.poolSizeCount)
		return EINVAL;
	for (index = 0U; index < info.poolSizeCount; index++) {
		type = info.pPoolSizes[index].type;
		if (type >= GHCI_VK_DESCRIPTOR_TYPES)
			return EINVAL;

		/* The sizes of one type add up, saturated at the largest count. */
		if (info.pPoolSizes[index].descriptorCount > UINT32_MAX - pool.capacity[type])
			pool.capacity[type] = UINT32_MAX;
		else
			pool.capacity[type] += info.pPoolSizes[index].descriptorCount;
	}

	/* The object. */
	error = ghci_vk_object_add(session, GHCI_VK_DESCRIPTOR_POOL, identity, device, &object);
	if (error != 0)
		return error;
	object->as.pool_of_sets = pool;

	/* The result and the identity. */
	ghci_vk_reply_created(reply, requested, (uint32_t)VK_SUCCESS, identity);

	/* Succeeded: the pool exists. */
	return 0;
}

/* vkResetDescriptorPool: [device][pool][u32 flags] -> [result]; every set of the pool goes. */
static int
reset_pool(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct ghci_vk_object *pool;
	uint64_t identity;

	/* The device, the pool and the flags. */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;
	pool = ghci_vk_object_find(session, GHCI_VK_DESCRIPTOR_POOL, identity);
	if (pool == NULL)
		return EINVAL;

	/* Its sets. */
	pool_sets_destroy(session, identity);

	/* The result. */
	ghci_vk_reply_result(reply, requested, (uint32_t)VK_SUCCESS);

	/* Succeeded: the pool is empty. */
	return 0;
}

/* Destroys every set of a pool. */
static void
pool_sets_destroy(
	struct ghci_vk_session *session,
	uint64_t pool)
{
	struct ghci_vk_object **position;
	struct ghci_vk_object *object;
	uint32_t bucket;

	/* Each bucket, each object of it; a destroyed one leaves its place to the next. */
	for (bucket = 0U; bucket < GHCI_VK_BUCKETS; bucket++) {
		position = &session->buckets[bucket];
		while (*position != NULL) {
			object = *position;
			if (object->kind == GHCI_VK_DESCRIPTOR_SET && object->as.set != NULL && object->as.set->pool == pool) {
				ghci_vk_object_destroy(session, object);
				continue;
			}
			position = &object->next;
		}
	}
}

/*
 * vkAllocateDescriptorSets: [device][u64 1][VkDescriptorSetAllocateInfo]
 * [u64 count][identities] -> [result][u64 count][identities].  All the
 * sets are made, or none.
 */
static int
allocate_sets(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkDescriptorSetAllocateInfo info;
	struct ghci_vk_object *pool;
	struct ghci_vk_object *object;
	const uint8_t *identities;
	uint64_t identity;
	uint64_t present;
	uint64_t count;
	uint64_t decoded;
	uint64_t index;
	uint64_t made;
	uint32_t result;

	/* The device, the allocate info, and the identities. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present == 1U)
		i915_vkc_dec_VkDescriptorSetAllocateInfo(reader, &session->arena, &info);
	count = ghci_vk_read_count(reader);
	identities = drv_i915_wire_read_array(reader, (size_t)count, sizeof(uint64_t));
	if (reader->error != 0 || present != 1U || count != info.descriptorSetCount || (count != 0U && identities == NULL))
		return EINVAL;
	decoded = ghci_vk_decoded_count(session, info.pSetLayouts);
	if (decoded != count)
		return EINVAL;
	pool = ghci_vk_object_find(session, GHCI_VK_DESCRIPTOR_POOL, (uint64_t)(uintptr_t)info.descriptorPool);
	if (pool == NULL)
		return EINVAL;

	/* Each set, up to the first that cannot be made. */
	result = (uint32_t)VK_SUCCESS;
	made = 0U;
	for (index = 0U; index < count; index++) {
		kern_memcpy(&identity, identities + index * sizeof(identity), sizeof(identity));
		result = set_make(session, pool, (uint64_t)(uintptr_t)info.pSetLayouts[index], identity);
		if (result != (uint32_t)VK_SUCCESS)
			break;
		made++;
	}

	/* A set that could not be made takes the ones made before it with it. */
	if (result != (uint32_t)VK_SUCCESS) {
		for (index = 0U; index < made; index++) {
			kern_memcpy(&identity, identities + index * sizeof(identity), sizeof(identity));
			object = ghci_vk_object_find(session, GHCI_VK_DESCRIPTOR_SET, identity);
			if (object != NULL)
				ghci_vk_object_destroy(session, object);
		}
	}

	/* A set no valid client asks for fails the command. */
	if (result == LAYOUT_INVALID)
		return EINVAL;

	/* The result, and the identities when they were made. */
	if (requested != 0U) {
		drv_i915_wire_reply_u32(reply, result);
		if (result == (uint32_t)VK_SUCCESS) {
			drv_i915_wire_reply_u64(reply, count);
			for (index = 0U; index < count; index++) {
				kern_memcpy(&identity, identities + index * sizeof(identity), sizeof(identity));
				drv_i915_wire_reply_u64(reply, identity);
			}
		}
	}

	/* Succeeded: the sets exist, or the reply says why not. */
	return 0;
}

/*
 * Makes one set of a pool with a layout: the pool's room for it, the
 * descriptors (the immutable samplers in place), and the object.
 * Returns the VkResult; LAYOUT_INVALID for a layout or an identity no
 * valid client gives.
 */
static uint32_t
set_make(
	struct ghci_vk_session *session,
	struct ghci_vk_object *pool,
	uint64_t layout_id,
	uint64_t identity)
{
	struct ghci_vk_descriptor_pool *counts;
	struct ghci_vk_set_layout *layout;
	struct ghci_vk_object *object;
	struct ghci_vk_set *set;
	uint64_t need[GHCI_VK_DESCRIPTOR_TYPES];
	uint64_t bytes;
	uint32_t index;
	uint32_t type;
	int error;

	/* The layout, of the session. */
	object = ghci_vk_object_find(session, GHCI_VK_SET_LAYOUT, layout_id);
	if (object == NULL)
		return LAYOUT_INVALID;
	layout = object->as.set_layout;

	/* Room in the pool: a set more, and the descriptors of each type (the bindings of a type together). */
	counts = &pool->as.pool_of_sets;
	if (counts->sets >= counts->max_sets)
		return (uint32_t)VK_ERROR_OUT_OF_POOL_MEMORY;
	kern_memset(need, 0, sizeof(need));
	for (index = 0U; index < layout->binding_count; index++)
		need[layout->bindings[index].type] += layout->bindings[index].count;
	for (type = 0U; type < GHCI_VK_DESCRIPTOR_TYPES; type++) {
		if ((uint64_t)counts->used[type] + need[type] > counts->capacity[type])
			return (uint32_t)VK_ERROR_OUT_OF_POOL_MEMORY;
	}

	/* Its bytes, within the session's budget. */
	bytes = sizeof(*set) + (uint64_t)layout->descriptor_count * sizeof(*set->descriptors);
	error = ghci_vk_budget_take(session, bytes);
	if (error != 0)
		return (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY;

	/* The set and its descriptors, the immutable samplers written. */
	set = kern_calloc(1U, sizeof(*set));
	if (set == NULL) {
		ghci_vk_budget_give(session, bytes);
		return (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	if (layout->descriptor_count != 0U) {
		set->descriptors = kern_calloc(layout->descriptor_count, sizeof(*set->descriptors));
		if (set->descriptors == NULL) {
			kern_free(set);
			ghci_vk_budget_give(session, bytes);
			return (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY;
		}
	}
	for (index = 0U; layout->samplers != NULL && index < layout->descriptor_count; index++)
		kern_memcpy(set->descriptors[index].sampler, &layout->samplers[index * GHCI_SAMPLER_DESC_BYTES], GHCI_SAMPLER_DESC_BYTES);
	set->pool = pool->identity;
	set->layout = layout;

	/* The object; a set of that identity already is a client's error. */
	object = ghci_vk_object_find(session, GHCI_VK_DESCRIPTOR_SET, identity);
	if (object != NULL)
		error = EEXIST;
	else
		error = ghci_vk_object_add(session, GHCI_VK_DESCRIPTOR_SET, identity, pool->device, &object);
	if (error != 0) {
		kern_free(set->descriptors);
		kern_free(set);
		ghci_vk_budget_give(session, bytes);
		if (error == ENOMEM)
			return (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY;
		return LAYOUT_INVALID;
	}

	/* The set holds its layout, and the pool counts it. */
	ghci_vk_set_layout_hold(layout);
	object->as.set = set;
	object->charged = bytes;
	counts->sets++;
	for (index = 0U; index < layout->binding_count; index++)
		counts->used[layout->bindings[index].type] += layout->bindings[index].count;

	/* Succeeded: the set exists. */
	return (uint32_t)VK_SUCCESS;
}

/* vkFreeDescriptorSets: [device][pool][u32 count][u64 count][identities] -> [result]. */
static int
free_sets(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct ghci_vk_object *object;
	const uint8_t *identities;
	uint64_t identity;
	uint64_t pool;
	uint64_t count;
	uint64_t index;

	/* The device, the pool and the sets. */
	(void)drv_i915_wire_read_u64(reader);
	pool = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = ghci_vk_read_count(reader);
	identities = drv_i915_wire_read_array(reader, (size_t)count, sizeof(uint64_t));
	if (reader->error != 0 || (count != 0U && identities == NULL))
		return EINVAL;

	/* Each set of the pool (the null handle frees nothing). */
	for (index = 0U; index < count; index++) {
		kern_memcpy(&identity, identities + index * sizeof(identity), sizeof(identity));
		object = ghci_vk_object_find(session, GHCI_VK_DESCRIPTOR_SET, identity);
		if (object != NULL && object->as.set != NULL && object->as.set->pool == pool)
			ghci_vk_object_destroy(session, object);
	}

	/* The result. */
	ghci_vk_reply_result(reply, requested, (uint32_t)VK_SUCCESS);

	/* Succeeded: the sets are gone. */
	return 0;
}

/*
 * vkUpdateDescriptorSets: [device][u32 n][u64 n]{write}[u32 m][u64 m]
 * {VkCopyDescriptorSet}: the writes, then the copies, in order.
 */
static int
update_sets(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkCopyDescriptorSet copy;
	uint64_t count;
	uint64_t index;
	int error;

	/* The device and the writes. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = ghci_vk_read_count(reader);
	if (reader->error != 0)
		return EINVAL;
	for (index = 0U; index < count; index++) {
		error = update_write(session, reader);
		if (error != 0)
			return error;
	}

	/* The copies. */
	(void)drv_i915_wire_read_u32(reader);
	count = ghci_vk_read_count(reader);
	if (reader->error != 0)
		return EINVAL;
	for (index = 0U; index < count; index++) {
		kern_memset(&copy, 0, sizeof(copy));
		i915_vkc_dec_VkCopyDescriptorSet(reader, &session->arena, &copy);
		if (reader->error != 0)
			return EINVAL;
		error = update_copy(session, &copy);
		if (error != 0)
			return error;
	}

	/* The result when it was asked for. */
	ghci_vk_reply_result(reply, requested, (uint32_t)VK_SUCCESS);

	/* Succeeded: the sets hold what was written and copied. */
	return 0;
}

/*
 * Applies one write: [u32 sType][u64 0][u64 set][u32 binding][u32 element]
 * [u32 count][u32 type][u64 n]{[u64 sampler][u64 view][u32 layout]}
 * [u64 n]{VkDescriptorBufferInfo}[u64 n]{[u64 view]}.  A write past a
 * binding goes on into the next ones, as Vulkan's do, which must be of
 * its type.  A write that fails part of the way leaves the descriptors
 * before it written (the frame ends).
 */
static int
update_write(
	struct ghci_vk_session *session,
	struct i915_wire_reader *reader)
{
	VkDescriptorBufferInfo buffers[1];
	struct ghci_vk_object *object;
	struct ghci_vk_set *set;
	const struct ghci_vk_layout_binding *binding;
	uint64_t images[2];
	uint64_t texel;
	uint64_t identity;
	uint64_t image_count;
	uint64_t buffer_count;
	uint64_t texel_count;
	uint64_t index;
	uint32_t binding_number;
	uint32_t element;
	uint32_t count;
	uint32_t type;
	uint32_t place;
	int error;

	/* The head of the write. */
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	binding_number = drv_i915_wire_read_u32(reader);
	element = drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u32(reader);
	type = drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The set, of the session, and the binding of its layout, of the type written. */
	object = ghci_vk_object_find(session, GHCI_VK_DESCRIPTOR_SET, identity);
	if (object == NULL || object->as.set == NULL)
		return EINVAL;
	set = object->as.set;
	error = layout_binding_of(set->layout, binding_number, &place);
	if (error != 0 || set->layout->bindings[place].type != type)
		return EINVAL;

	/* The image infos, one per descriptor of an image type (none otherwise). */
	image_count = ghci_vk_read_count(reader);
	if (image_count != 0U && (image_count != count || layout_family(type) != LAYOUT_FAMILY_IMAGE))
		return EINVAL;
	for (index = 0U; index < image_count; index++) {
		images[0] = drv_i915_wire_read_u64(reader);
		images[1] = drv_i915_wire_read_u64(reader);
		(void)drv_i915_wire_read_u32(reader);
		if (reader->error != 0)
			return EINVAL;
		error = descriptor_walk(set->layout, &place, &element);
		if (error != 0)
			return EINVAL;
		binding = &set->layout->bindings[place];
		if (binding->type != type)
			return EINVAL;
		error = write_one(session, type, binding->immutable, images[0], images[1], NULL, &set->descriptors[binding->first + element]);
		if (error != 0)
			return error;
		element++;
	}

	/* The buffer infos, one per descriptor of a buffer type. */
	buffer_count = ghci_vk_read_count(reader);
	if (buffer_count != 0U && (buffer_count != count || layout_family(type) != LAYOUT_FAMILY_BUFFER))
		return EINVAL;
	for (index = 0U; index < buffer_count; index++) {
		i915_vkc_dec_VkDescriptorBufferInfo(reader, &session->arena, &buffers[0]);
		if (reader->error != 0)
			return EINVAL;
		error = descriptor_walk(set->layout, &place, &element);
		if (error != 0)
			return EINVAL;
		binding = &set->layout->bindings[place];
		if (binding->type != type)
			return EINVAL;
		error = write_one(session, type, 0U, 0U, 0U, &buffers[0], &set->descriptors[binding->first + element]);
		if (error != 0)
			return error;
		element++;
	}

	/* The texel buffer views, one per descriptor of a texel buffer type. */
	texel_count = ghci_vk_read_count(reader);
	if (texel_count != 0U && (texel_count != count || layout_family(type) != LAYOUT_FAMILY_TEXEL))
		return EINVAL;
	for (index = 0U; index < texel_count; index++) {
		texel = drv_i915_wire_read_u64(reader);
		if (reader->error != 0)
			return EINVAL;
		error = descriptor_walk(set->layout, &place, &element);
		if (error != 0)
			return EINVAL;
		binding = &set->layout->bindings[place];
		if (binding->type != type)
			return EINVAL;
		error = write_one(session, type, 0U, 0U, texel, NULL, &set->descriptors[binding->first + element]);
		if (error != 0)
			return error;
		element++;
	}

	/* A write must carry the payload of its type. */
	if (image_count + buffer_count + texel_count != count)
		return EINVAL;

	/* Succeeded: the descriptors are written. */
	return 0;
}

/*
 * Writes one descriptor of a type: the sampler (not an immutable one),
 * the image view's texture descriptor (the sampled one, or the single one
 * of a storage image or an input attachment), a buffer view's
 * descriptors, or a buffer descriptor of a buffer range (read only for a
 * uniform buffer).  Returns 0, or EINVAL for an object the session does
 * not have.
 */
static int
write_one(
	struct ghci_vk_session *session,
	uint32_t type,
	uint32_t immutable,
	uint64_t sampler,
	uint64_t view,
	const VkDescriptorBufferInfo *buffer,
	struct ghci_vk_descriptor *descriptor)
{
	struct ghci_buffer_desc desc;
	struct ghci_vk_object *object;
	uint64_t range;
	uint64_t va;
	int error;

	/* The sampler of a sampler or a combined image sampler, unless the layout's is immutable. */
	if ((type == (uint32_t)VK_DESCRIPTOR_TYPE_SAMPLER || type == (uint32_t)VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) && immutable == 0U) {
		object = ghci_vk_object_find(session, GHCI_VK_SAMPLER, sampler);
		if (object == NULL)
			return EINVAL;
		kern_memcpy(descriptor->sampler, object->as.sampler.descriptor, GHCI_SAMPLER_DESC_BYTES);
	}

	/* What each type takes. */
	switch (type) {
	case VK_DESCRIPTOR_TYPE_SAMPLER:
		break;
	case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
	case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
		object = ghci_vk_object_find(session, GHCI_VK_IMAGE_VIEW, view);
		if (object == NULL)
			return EINVAL;
		kern_memcpy(descriptor->texture, object->as.image_view.sampled, GHCI_TEXTURE_DESC_BYTES);
		break;
	case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
	case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
		object = ghci_vk_object_find(session, GHCI_VK_IMAGE_VIEW, view);
		if (object == NULL)
			return EINVAL;
		kern_memcpy(descriptor->texture, object->as.image_view.single, GHCI_TEXTURE_DESC_BYTES);
		break;
	case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
	case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
		object = ghci_vk_object_find(session, GHCI_VK_BUFFER_VIEW, view);
		if (object == NULL)
			return EINVAL;
		kern_memcpy(descriptor->texture, object->as.buffer_view.texture, GHCI_TEXTURE_DESC_BYTES);
		kern_memcpy(descriptor->buffer, object->as.buffer_view.buffer_descriptor, GHCI_BUFFER_DESC_BYTES);
		if (type == (uint32_t)VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER)
			descriptor->buffer[GHCI_BUFFER_FLAGS] = LAYOUT_BUFFER_READ_ONLY;
		break;
	default:
		/* A buffer: its range at its address, the rest of the buffer for VK_WHOLE_SIZE. */
		object = ghci_vk_object_find(session, GHCI_VK_BUFFER, (uint64_t)(uintptr_t)buffer->buffer);
		if (object == NULL || object->as.buffer.memory == 0U || buffer->offset > object->as.buffer.size)
			return EINVAL;
		error = ghci_vk_bound_va(session, object->as.buffer.memory, object->as.buffer.offset + buffer->offset, &va);
		if (error != 0)
			return EINVAL;
		range = buffer->range;
		if (range == VK_WHOLE_SIZE || range > object->as.buffer.size - buffer->offset)
			range = object->as.buffer.size - buffer->offset;

		/*
		 * Values referenced: amaterrace-gpu d9252e4 docs/ghci.md
		 * (section 8.3, GhciBufferDescriptor: stride and format 0 for
		 * a uniform or storage buffer, flags [0] ReadOnly).
		 */
		kern_memset(&desc, 0, sizeof(desc));
		desc.base = va;
		desc.bytes = (uint32_t)range;
		if (type == (uint32_t)VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || type == LAYOUT_DYNAMIC_UNIFORM)
			desc.flags = LAYOUT_BUFFER_READ_ONLY;
		(void)ghci_buffer_desc_encode(descriptor->buffer, &desc);
		break;
	}

	/* Succeeded: the descriptor is written. */
	descriptor->written = 1U;
	return 0;
}

/*
 * Applies one copy of descriptors between two sets (or within one),
 * going on into the next bindings; each source descriptor's binding is of
 * the type of the destination's.
 */
static int
update_copy(
	struct ghci_vk_session *session,
	const VkCopyDescriptorSet *copy)
{
	struct ghci_vk_object *source_object;
	struct ghci_vk_object *target_object;
	struct ghci_vk_set *source;
	struct ghci_vk_set *target;
	struct ghci_vk_descriptor moved;
	uint32_t source_place;
	uint32_t target_place;
	uint32_t source_element;
	uint32_t target_element;
	uint32_t index;
	int error;

	/* Both sets, of the session, and both bindings. */
	source_object = ghci_vk_object_find(session, GHCI_VK_DESCRIPTOR_SET, (uint64_t)(uintptr_t)copy->srcSet);
	target_object = ghci_vk_object_find(session, GHCI_VK_DESCRIPTOR_SET, (uint64_t)(uintptr_t)copy->dstSet);
	if (source_object == NULL || target_object == NULL || source_object->as.set == NULL || target_object->as.set == NULL)
		return EINVAL;
	source = source_object->as.set;
	target = target_object->as.set;
	error = layout_binding_of(source->layout, copy->srcBinding, &source_place);
	if (error != 0)
		return EINVAL;
	error = layout_binding_of(target->layout, copy->dstBinding, &target_place);
	if (error != 0)
		return EINVAL;

	/* Each descriptor, through a copy so that an overlap within a set reads what was there. */
	source_element = copy->srcArrayElement;
	target_element = copy->dstArrayElement;
	for (index = 0U; index < copy->descriptorCount; index++) {
		error = descriptor_walk(source->layout, &source_place, &source_element);
		if (error != 0)
			return EINVAL;
		error = descriptor_walk(target->layout, &target_place, &target_element);
		if (error != 0)
			return EINVAL;
		if (source->layout->bindings[source_place].type != target->layout->bindings[target_place].type)
			return EINVAL;
		moved = source->descriptors[source->layout->bindings[source_place].first + source_element];
		if (target->layout->bindings[target_place].immutable != 0U)
			kern_memcpy(moved.sampler, target->descriptors[target->layout->bindings[target_place].first + target_element].sampler, GHCI_SAMPLER_DESC_BYTES);
		target->descriptors[target->layout->bindings[target_place].first + target_element] = moved;
		source_element++;
		target_element++;
	}

	/* Succeeded: the descriptors are copied. */
	return 0;
}

/* The destroy of a sampler, a view, a layout or a pool (a pool takes its sets with it): no reply body. */
static int
destroy_object(
	struct ghci_vk_session *session,
	enum ghci_vk_kind kind,
	struct i915_wire_reader *reader)
{
	struct ghci_vk_object *object;
	uint64_t identity;
	uint64_t device;

	/* The destroy form. */
	identity = ghci_vk_destroy_form(reader, &device);
	if (reader->error != 0)
		return EINVAL;

	/* A pool's sets first. */
	if (kind == GHCI_VK_DESCRIPTOR_POOL && identity != 0U)
		pool_sets_destroy(session, identity);

	/* The object, when the session has it (the null handle destroys nothing). */
	object = ghci_vk_object_find(session, kind, identity);
	if (object != NULL)
		ghci_vk_object_destroy(session, object);

	/* Succeeded: the object is gone. */
	return 0;
}

/*
 * Finds the descriptor of a set at a binding and an element, with the
 * binding of the layout; NULL when the layout has no such descriptor.
 */
static struct ghci_vk_descriptor *
descriptor_at(
	struct ghci_vk_set *set,
	uint32_t binding,
	uint32_t element,
	const struct ghci_vk_layout_binding **found)
{
	uint32_t place;
	int error;

	/* The binding, and the element within it. */
	error = layout_binding_of(set->layout, binding, &place);
	if (error != 0 || element >= set->layout->bindings[place].count)
		return NULL;

	/* Succeeded: the descriptor. */
	*found = &set->layout->bindings[place];
	return &set->descriptors[set->layout->bindings[place].first + element];
}

/*
 * Checks that a descriptor fills a texture slot, a sampler slot or a
 * resource table entry of a kind: a binding of a type the kind takes,
 * and a descriptor written (or an immutable sampler).  Returns 0, or
 * ENOENT.
 */
static int
descriptor_fits(
	uint32_t table,
	uint32_t kind,
	const struct ghci_vk_layout_binding *binding,
	const struct ghci_vk_descriptor *descriptor)
{
	uint32_t types[2];
	int error;

	/* The types the kind takes. */
	error = kind_types(table, kind, types);
	if (error != 0)
		return error;
	if (binding->type != types[0] && binding->type != types[1])
		return ENOENT;

	/* A sampler slot of an immutable sampler needs no write. */
	if (table == LAYOUT_TABLE_SAMPLER && binding->immutable != 0U)
		return 0;

	/* Anything else must have been written. */
	if (descriptor->written == 0U)
		return ENOENT;

	/* Succeeded: the descriptor fills it. */
	return 0;
}

/*
 * Reports the two descriptor types (the same one twice for one) that
 * fill a texture slot, a sampler slot or a resource table entry of a
 * kind.  Returns 0, or ENOENT for a kind the driver does not fill.
 */
static int
kind_types(
	uint32_t table,
	uint32_t kind,
	uint32_t *types)
{
	/* Set 0's textures: an image of a combined image sampler or a sampled image, or a uniform texel buffer. */
	if (table == LAYOUT_TABLE_TEXTURE && kind == GHCI_GSI1_TEXTURE_IMAGE) {
		types[0] = (uint32_t)VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		types[1] = (uint32_t)VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		return 0;
	}
	if (table == LAYOUT_TABLE_TEXTURE && kind == GHCI_GSI1_TEXTURE_TEXEL_BUFFER) {
		types[0] = (uint32_t)VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
		types[1] = types[0];
		return 0;
	}

	/* Set 1's samplers: of a combined image sampler or a sampler. */
	if (table == LAYOUT_TABLE_SAMPLER && kind == GHCI_GSI1_SAMPLER_COMBINED) {
		types[0] = (uint32_t)VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		types[1] = (uint32_t)VK_DESCRIPTOR_TYPE_SAMPLER;
		return 0;
	}
	if (table != LAYOUT_TABLE_RESOURCE)
		return ENOENT;

	/* Set 2's entries: each kind's buffer (dynamic or not), texel buffer or image. */
	switch (kind) {
	case GHCI_GSI1_RESOURCE_UNIFORM_BUFFER:
		types[0] = (uint32_t)VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		types[1] = LAYOUT_DYNAMIC_UNIFORM;
		break;
	case GHCI_GSI1_RESOURCE_STORAGE_BUFFER:
		types[0] = (uint32_t)VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		types[1] = LAYOUT_DYNAMIC_STORAGE;
		break;
	case GHCI_GSI1_RESOURCE_UNIFORM_TEXEL_BUFFER:
		types[0] = (uint32_t)VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
		types[1] = types[0];
		break;
	case GHCI_GSI1_RESOURCE_STORAGE_TEXEL_BUFFER:
		types[0] = (uint32_t)VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
		types[1] = types[0];
		break;
	case GHCI_GSI1_RESOURCE_STORAGE_IMAGE:
		types[0] = (uint32_t)VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		types[1] = types[0];
		break;
	case GHCI_GSI1_RESOURCE_INPUT_ATTACHMENT:
		types[0] = (uint32_t)VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
		types[1] = types[0];
		break;
	default:
		return ENOENT;
	}

	/* Succeeded: the types. */
	return 0;
}

/*
 * Moves a binding place and an element on to the next binding with
 * descriptors while the element is past the binding's count.  Returns 0,
 * or ENOENT past the last binding.
 */
static int
descriptor_walk(
	const struct ghci_vk_set_layout *layout,
	uint32_t *index,
	uint32_t *element)
{
	/* The bindings after this one, as long as the element is past the binding. */
	while (*index < layout->binding_count && *element >= layout->bindings[*index].count) {
		*element -= layout->bindings[*index].count;
		(*index)++;
	}

	/* Past the last binding. */
	if (*index >= layout->binding_count)
		return ENOENT;

	/* Succeeded: the place of the descriptor. */
	return 0;
}

/* Finds the place of a binding number in a layout.  Returns 0, or ENOENT. */
static int
layout_binding_of(
	const struct ghci_vk_set_layout *layout,
	uint32_t binding,
	uint32_t *index)
{
	uint32_t place;

	/* The bindings in order. */
	for (place = 0U; place < layout->binding_count; place++) {
		if (layout->bindings[place].binding == binding) {
			*index = place;
			return 0;
		}
	}

	/* Succeeded: none. */
	return ENOENT;
}

/* Reports the payload a write carries for a descriptor type (LAYOUT_FAMILY_*). */
static uint32_t
layout_family(
	uint32_t type)
{
	/* The texel buffers, the plain buffers, and the samplers and images. */
	if (type == (uint32_t)VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || type == (uint32_t)VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)
		return LAYOUT_FAMILY_TEXEL;
	if (type >= (uint32_t)VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && type <= LAYOUT_DYNAMIC_STORAGE)
		return LAYOUT_FAMILY_BUFFER;

	/* Succeeded: an image or a sampler. */
	return LAYOUT_FAMILY_IMAGE;
}

/* Finds a device of the session by its identity; NULL when it has none. */
static struct ghci_vk_device *
device_of(
	struct ghci_vk_session *session,
	uint64_t handle)
{
	struct ghci_vk_object *object;

	/* The device's object, and its record. */
	object = ghci_vk_object_find(session, GHCI_VK_DEVICE, handle);
	if (object == NULL)
		return NULL;

	/* Succeeded: the record. */
	return object->device;
}

/*
 * Reports the hardware's swizzle of a component (own: 0 R to 3 A) from
 * its VkComponentSwizzle: identity is the component's own; ZERO, ONE, R,
 * G, B and A are the hardware's 0 to 5.
 *
 * Values referenced: amaterrace-gpu d9252e4 docs/ghci.md (section 8.3,
 * component_swizzle: 0 ZERO, 1 ONE, 2 R, 3 G, 4 B, 5 A).
 */
static uint32_t
swizzle_of(
	uint32_t swizzle,
	uint32_t own)
{
	/* The component's own channel. */
	if (swizzle == (uint32_t)VK_COMPONENT_SWIZZLE_IDENTITY || swizzle > (uint32_t)VK_COMPONENT_SWIZZLE_A)
		return LAYOUT_SWIZZLE_R + own;

	/* Succeeded: Vulkan's ZERO (1) to A (6) one below. */
	return swizzle - 1U;
}

/*
 * Reports the nearest integer to an IEEE 754 single times 2^fraction (a
 * half rounds away from zero), saturated to +-2^30; a NaN is 0.  The
 * kernel touches no floating-point register, so the value is built from
 * the bits.
 */
static int32_t
float_fixed(
	uint32_t bits,
	uint32_t fraction)
{
	uint64_t magnitude;
	uint32_t exponent;
	int32_t value;
	int shift;

	/* A NaN, an infinity, a zero or a subnormal. */
	exponent = (bits >> FLOAT_FRACTION_BITS) & FLOAT_EXPONENT_MASK;
	if (exponent == FLOAT_EXPONENT_SPECIAL && (bits & FLOAT_FRACTION_MASK) != 0U)
		return 0;
	if (exponent == 0U)
		return 0;

	/* The fraction with its leading one, moved by the exponent and the fixed point. */
	magnitude = (uint64_t)((bits & FLOAT_FRACTION_MASK) | (1U << FLOAT_FRACTION_BITS));
	shift = (int)exponent - FLOAT_EXPONENT_BIAS + (int)fraction;
	if (exponent == FLOAT_EXPONENT_SPECIAL || shift > 6) {
		magnitude = 1ULL << 30;
	} else if (shift >= 0) {
		magnitude <<= shift;
	} else if (shift > -40) {
		magnitude = (magnitude + (1ULL << (-shift - 1))) >> -shift;
	} else {
		magnitude = 0U;
	}
	if (magnitude > (1ULL << 30))
		magnitude = 1ULL << 30;

	/* The sign. */
	value = (int32_t)magnitude;
	if ((bits >> 31) != 0U)
		value = -value;

	/* Succeeded: the fixed-point value. */
	return value;
}

/* Reports a value within low to high. */
static int32_t
clamp_int(
	int32_t value,
	int32_t low,
	int32_t high)
{
	/* Below, above, or within. */
	if (value < low)
		return low;
	if (value > high)
		return high;

	/* Succeeded: the value itself. */
	return value;
}

/* Reports the bits of a float field the codec stored. */
static uint32_t
float_bits(
	const float *value)
{
	uint32_t bits;

	/* The bits unchanged. */
	kern_memcpy(&bits, value, sizeof(bits));
	return bits;
}
