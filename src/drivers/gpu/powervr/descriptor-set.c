/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PowerVR descriptor set's memory: the bindings' places in a set and
 * the bytes of each descriptor, laid out as the shader's shared register
 * requests read them.  Nothing here touches the core or memory; the
 * executor's set commands (vk-layout.c) call it, and the host check
 * builds it unchanged.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include <libc/vulkan/vulkan_core.h>

#include "drivers/gpu/powervr/descriptor-set.h"
#include "drivers/gpu/powervr/compiler/shader.h"
#include "drivers/gpu/compiler/spirv.h"

/*
 * The pack information's fields.
 *
 * Values referenced:
 * Mesa 26.2.6 src/imagination/common/pvr_iface.h
 * (PVR_PCK_INFO_FORMAT_OFFSET 0 with _LENGTH 5, PVR_PCK_INFO_SPLIT_OFFSET 5,
 * _SCALE_OFFSET 6, _ROUNDZERO_OFFSET 7, _UNORM_OFFSET 8) and
 * src/imagination/pco/pco_isa.py (F_PCK_FORMAT: u8888 0, s8888 1,
 * u1616 3, s1616 4, u1010102 8, f111110 12, f16f16 14).
 */
#define PACK_FORMAT_U8888		0x00U
#define PACK_FORMAT_S8888		0x01U
#define PACK_FORMAT_U1616		0x03U
#define PACK_FORMAT_S1616		0x04U
#define PACK_FORMAT_U1010102		0x08U
#define PACK_FORMAT_F111110		0x0cU
#define PACK_FORMAT_F16F16		0x0eU
#define PACK_INFO_SPLIT			0x20U
#define PACK_INFO_SCALE			0x40U
#define PACK_INFO_UNORM			POWERVR_SHADER_PACK_INFO_UNORM

/* Where the sampler's words lie in a combined image and sampler descriptor. */
#define COMBINED_SAMPLER_OFFSET		POWERVR_DESCRIPTOR_IMAGE_BYTES

/* Where an image descriptor keeps its layer size (powervr_descriptor_image). */
#define IMAGE_LAYER_BYTES_OFFSET	16U

/* Where an image descriptor keeps its pack information (powervr_descriptor_image). */
#define IMAGE_PACK_INFO_OFFSET		28U

/* Where a storage image descriptor keeps its view's shape (powervr_descriptor_storage_image). */
#define STORAGE_SHAPE_OFFSET		POWERVR_DESCRIPTOR_BUFFER_BYTES

/* Where a sampler descriptor keeps its comparison (powervr_descriptor_sampler). */
#define SAMPLER_COMPARE_OFFSET		16U

/*
 * The filter fields of the sampler's first word.
 *
 * Values referenced: Mesa 26.2.5 src/imagination/csbgen/rogue/
 * texstate.xml (SAMPLER_WORD0: magfilter 36-37, minfilter 38-39,
 * mipfilter 40; enum FILTER: linear 1).
 */
#define SAMPLER_FILTER_MASK		((uint64_t)0x1fU << 36)
#define SAMPLER_GATHER_FILTER		(((uint64_t)1U << 36) | ((uint64_t)1U << 38))

static uint32_t descriptor_stride(uint32_t type);
static void store32(uint8_t *bytes, uint32_t value);
static void store64(uint8_t *bytes, uint64_t value);

/*
 * Lays out a set's bindings in its memory.
 *
 * The bindings are placed in the order of their numbers, each binding's
 * descriptors one after another, with no padding.  A binding of count 0
 * takes no room.  Refuses a binding number past the most, a number named
 * twice, and a type the executor does not take (ENOTSUP, a dynamic
 * storage buffer among them).  A dynamic uniform buffer is laid out as a
 * uniform buffer is: its descriptor holds the range's address as written,
 * and each draw's copy of the set has the bind's dynamic offset added to
 * it (powervr_set_dynamic_place).
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_descriptor_set.c
 * (pvr_CreateDescriptorSetLayout: the bindings sorted by number, each
 * non-dynamic binding at the layout's size so far with the stride of
 * pvr_descriptor_size, the size growing by count times stride).
 */
int
powervr_set_layout_build(
	const struct powervr_set_binding_info *infos,
	uint32_t count,
	struct powervr_set_layout *layout)
{
	struct powervr_set_binding *binding;
	uint32_t index;
	uint32_t number;
	uint32_t stride;
	uint64_t bytes;
	bool named[POWERVR_SET_BINDINGS_MAX];

	/* Refuses a missing table or result. */
	if ((count != 0 && infos == NULL) || layout == NULL)
		return EINVAL;

	/* No binding yet. */
	for (number = 0; number < POWERVR_SET_BINDINGS_MAX; number++) {
		layout->bindings[number].type = 0;
		layout->bindings[number].count = 0;
		layout->bindings[number].offset = 0;
		layout->bindings[number].stride = 0;
		named[number] = false;
	}
	layout->bytes = 0;

	/* Each named binding's type and count, checked. */
	for (index = 0; index < count; index++) {
		number = infos[index].binding;
		if (number >= POWERVR_SET_BINDINGS_MAX)
			return ENOTSUP;
		if (named[number])
			return EINVAL;
		named[number] = true;
		if (infos[index].count == 0)
			continue;

		stride = descriptor_stride(infos[index].type);
		if (stride == 0)
			return ENOTSUP;

		binding = &layout->bindings[number];
		binding->type = infos[index].type;
		binding->count = infos[index].count;
		binding->stride = stride;
	}

	/* The places, in the order of the numbers. */
	bytes = 0;
	for (number = 0; number < POWERVR_SET_BINDINGS_MAX; number++) {
		binding = &layout->bindings[number];
		if (binding->count == 0)
			continue;

		binding->offset = (uint32_t)bytes;
		bytes += (uint64_t)binding->count * binding->stride;
		if (bytes > UINT32_MAX)
			return E2BIG;
	}

	/* Succeeded: the set's size. */
	layout->bytes = (uint32_t)bytes;
	return 0;
}

/*
 * Reports where a part of one descriptor lies in a set's memory: a
 * buffer's address, an image's state words or layer size, a sampler's
 * state words or comparison, or a storage image's texel buffer or shape
 * (enum powervr_shader_descriptor_part).
 *
 * A combined image and sampler holds the image first and the sampler
 * after it.  A storage image holds the buffer descriptor of its texels,
 * which its binding names as a buffer's address, then its shape, which
 * the binding with DRV_GPU_SPIRV_IMAGE_INFO_BINDING set names.  Refuses a
 * binding the layout does not name, an element past its count, and a
 * part its type does not have.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_descriptor_set.c
 * (write_buffer, write_sampler, write_image_sampler, write_sampled_image:
 * the descriptor at the binding's offset plus the element times the
 * stride) and src/imagination/vulkan/pvr_common.h
 * (struct pvr_combined_image_sampler_descriptor: image, then sampler).
 */
int
powervr_set_descriptor_offset(
	const struct powervr_set_layout *layout,
	uint32_t binding,
	uint32_t element,
	uint32_t part,
	uint32_t *offset)
{
	const struct powervr_set_binding *entry;
	uint32_t within;
	uint32_t shape;

	/* The binding of a storage image's shape is the image's own with the shape's bit. */
	shape = binding & DRV_GPU_SPIRV_IMAGE_INFO_BINDING;
	binding &= ~DRV_GPU_SPIRV_IMAGE_INFO_BINDING;

	/* Refuses a missing layout or result, and a binding or element it does not have. */
	if (layout == NULL || offset == NULL || binding >= POWERVR_SET_BINDINGS_MAX)
		return EINVAL;
	entry = &layout->bindings[binding];
	if (element >= entry->count)
		return EINVAL;

	/* Only a storage image has a shape. */
	if (shape != 0U && entry->type != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
		return EINVAL;

	/* The part's place in the descriptor, by the binding's type. */
	switch (entry->type) {
	case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
	case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
	case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
		if (part != POWERVR_SHADER_PART_BUFFER_ADDRESS)
			return EINVAL;
		within = 0;
		break;
	case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
		if (part == POWERVR_SHADER_PART_IMAGE_STATE) {
			within = 0;
		} else if (part == POWERVR_SHADER_PART_IMAGE_LAYER_BYTES) {
			within = IMAGE_LAYER_BYTES_OFFSET;
		} else if (part == POWERVR_SHADER_PART_IMAGE_PACK_INFO) {
			within = IMAGE_PACK_INFO_OFFSET;
		} else {
			return EINVAL;
		}
		break;
	case VK_DESCRIPTOR_TYPE_SAMPLER:
		if (part == POWERVR_SHADER_PART_SAMPLER_STATE) {
			within = 0;
		} else if (part == POWERVR_SHADER_PART_SAMPLER_COMPARE) {
			within = SAMPLER_COMPARE_OFFSET;
		} else {
			return EINVAL;
		}
		break;
	case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
		if (shape == 0U && part == POWERVR_SHADER_PART_BUFFER_ADDRESS) {
			within = 0;
		} else if (shape != 0U && part == POWERVR_SHADER_PART_IMAGE_SHAPE) {
			within = STORAGE_SHAPE_OFFSET;
		} else {
			return EINVAL;
		}
		break;
	case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
		if (part == POWERVR_SHADER_PART_IMAGE_STATE) {
			within = 0;
		} else if (part == POWERVR_SHADER_PART_IMAGE_LAYER_BYTES) {
			within = IMAGE_LAYER_BYTES_OFFSET;
		} else if (part == POWERVR_SHADER_PART_IMAGE_PACK_INFO) {
			within = IMAGE_PACK_INFO_OFFSET;
		} else if (part == POWERVR_SHADER_PART_SAMPLER_STATE) {
			within = COMBINED_SAMPLER_OFFSET;
		} else if (part == POWERVR_SHADER_PART_SAMPLER_COMPARE) {
			within = COMBINED_SAMPLER_OFFSET + SAMPLER_COMPARE_OFFSET;
		} else {
			return EINVAL;
		}
		break;
	default:
		return EINVAL;
	}

	/* Succeeded: the binding's place, the element's, then the part's. */
	*offset = entry->offset + element * entry->stride + within;
	return 0;
}

/*
 * Reports how many dynamic descriptors a set layout has: the descriptor
 * count of each binding of a dynamic uniform buffer, summed.  A bind of
 * a set of this layout takes this many dynamic offsets.
 */
uint32_t
powervr_set_dynamic_count(
	const struct powervr_set_layout *layout)
{
	uint32_t number;
	uint32_t count;

	/* Each binding of a dynamic type adds its descriptors. */
	count = 0;
	for (number = 0; number < POWERVR_SET_BINDINGS_MAX; number++) {
		if (layout->bindings[number].type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
			continue;

		count += layout->bindings[number].count;
	}

	/* Succeeded: the count. */
	return count;
}

/*
 * Reports where the address of a set's dynamic descriptor of an index
 * lies in its memory.
 *
 * The dynamic descriptors are numbered as the dynamic offsets of a bind
 * name them: by binding number, then by element.  Refuses an index past
 * the layout's dynamic descriptors.
 *
 * Procedure referenced: Khronos Vulkan 1.3 specification, "Descriptor
 * Set Binding" (vkCmdBindDescriptorSets: the dynamic offsets are taken
 * in the order of the sets, and within a set in the order of the
 * binding numbers and the array elements).
 */
int
powervr_set_dynamic_place(
	const struct powervr_set_layout *layout,
	uint32_t index,
	uint32_t *offset)
{
	const struct powervr_set_binding *entry;
	uint32_t number;
	uint32_t skipped;

	/* Refuses a missing layout or result. */
	if (layout == NULL || offset == NULL)
		return EINVAL;

	/* The binding whose descriptors take the index, counting the ones before it. */
	skipped = 0;
	for (number = 0; number < POWERVR_SET_BINDINGS_MAX; number++) {
		entry = &layout->bindings[number];
		if (entry->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
			continue;

		/* The element of this binding, when the index falls in it. */
		if (index - skipped < entry->count) {
			*offset = entry->offset + (index - skipped) * entry->stride;
			return 0;
		}

		skipped += entry->count;
	}

	/* No dynamic descriptor of that index. */
	return EINVAL;
}

/*
 * Reports the pack information an image descriptor carries for the
 * shader's pack instruction: the pack format, and whether the channels
 * are split, scaled, rounded to zero and unsigned normalized.
 *
 * The 8-bit normalized formats in red-first order pack as four 8-bit
 * channels, the 16-bit ones (normalized or half float) as two 16-bit
 * channels a word split over two words, A2B10G10R10 unsigned
 * normalized as 10, 10, 10 and 2 bits and B10G11R11 as its two 11-bit
 * and one 10-bit floats; the rest carry the invalid format.  The
 * unsigned normalized bit is set for every format of unsigned
 * normalized or sRGB channels, the packed depth and stencil one
 * included.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/imagination/vulkan/pvr_arch_tex_state.c
 * (setup_pck_info), src/imagination/pco/pco.c (pco_pipe_to_pck_format:
 * R8 to R8G8B8A8 UNORM U8888 and SNORM S8888 with scale, R16 to
 * R16G16B16A16 FLOAT F16F16 split, UNORM U1616 and SNORM S1616 with scale
 * and split, R10G10B10A2_UNORM U1010102 with scale, R11G11B10_FLOAT
 * F111110), src/vulkan/util/vk_format.c (A2B10G10R10_UNORM_PACK32 is
 * pipe's R10G10B10A2_UNORM, B10G11R11_UFLOAT_PACK32 its R11G11B10_FLOAT,
 * D24_UNORM_S8_UINT its Z24_UNORM_S8_UINT) and
 * src/util/format/u_format_parse.py (is_unorm: a name with _UNORM or
 * _SRGB).
 */
uint32_t
powervr_pack_info(
	uint32_t vk_format)
{
	/* The pack format and the bits, by format. */
	switch (vk_format) {
	case VK_FORMAT_R8_UNORM:
	case VK_FORMAT_R8G8_UNORM:
	case VK_FORMAT_R8G8B8A8_UNORM:
		return PACK_FORMAT_U8888 | PACK_INFO_SCALE | PACK_INFO_UNORM;
	case VK_FORMAT_R8_SNORM:
	case VK_FORMAT_R8G8_SNORM:
	case VK_FORMAT_R8G8B8A8_SNORM:
		return PACK_FORMAT_S8888 | PACK_INFO_SCALE;
	case VK_FORMAT_R16_UNORM:
	case VK_FORMAT_R16G16_UNORM:
	case VK_FORMAT_R16G16B16A16_UNORM:
		return PACK_FORMAT_U1616 | PACK_INFO_SPLIT | PACK_INFO_SCALE | PACK_INFO_UNORM;
	case VK_FORMAT_R16_SNORM:
	case VK_FORMAT_R16G16_SNORM:
	case VK_FORMAT_R16G16B16A16_SNORM:
		return PACK_FORMAT_S1616 | PACK_INFO_SPLIT | PACK_INFO_SCALE;
	case VK_FORMAT_R16_SFLOAT:
	case VK_FORMAT_R16G16_SFLOAT:
	case VK_FORMAT_R16G16B16A16_SFLOAT:
		return PACK_FORMAT_F16F16 | PACK_INFO_SPLIT;
	case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
		return PACK_FORMAT_U1010102 | PACK_INFO_SCALE | PACK_INFO_UNORM;
	case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
		return PACK_FORMAT_F111110;
	case VK_FORMAT_R8G8B8A8_SRGB:
	case VK_FORMAT_B8G8R8A8_UNORM:
	case VK_FORMAT_B8G8R8A8_SRGB:
	case VK_FORMAT_D16_UNORM:
	case VK_FORMAT_D24_UNORM_S8_UINT:
		return POWERVR_PACK_FORMAT_INVALID | PACK_INFO_UNORM;
	default:
		break;
	}

	/* No pack format, and not unsigned normalized. */
	return POWERVR_PACK_FORMAT_INVALID;
}

/*
 * Writes a buffer descriptor: the address of the range's first byte, the
 * range's size and a zero offset.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_descriptor_set.c
 * (write_buffer: addr, size, the offset left zero) and
 * src/imagination/vulkan/pvr_common.h (struct pvr_buffer_descriptor:
 * addr 64 bits, size 32, offset 32).
 */
void
powervr_descriptor_buffer(
	uint8_t bytes[POWERVR_DESCRIPTOR_BUFFER_BYTES],
	uint64_t address,
	uint32_t range)
{
	/* The address, the size, then the offset. */
	store64(&bytes[0], address);
	store32(&bytes[8], range);
	store32(&bytes[12], 0);
}

/*
 * Writes an image descriptor: the view's two state words, then the four
 * words the shader reads (the layer size, the buffer elements, the depth
 * slice and the pack information).
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_tex_state.c
 * (pvr_arch_pack_tex_state: meta[LAYER_SIZE], [BUFFER_ELEMS], [Z_SLICE],
 * [PCK_INFO]), src/imagination/vulkan/pvr_arch_image.c (a 2D view: the
 * plane's layer size, no buffer elements, slice 0) and
 * src/imagination/pco/pco_data.h (enum pco_image_meta).
 */
void
powervr_descriptor_image(
	uint8_t bytes[POWERVR_DESCRIPTOR_IMAGE_BYTES],
	const uint64_t words[2],
	uint32_t layer_bytes,
	uint32_t pack_info)
{
	/* The state words. */
	store64(&bytes[0], words[0]);
	store64(&bytes[8], words[1]);

	/* The layer size, no buffer elements, slice 0, the pack information. */
	store32(&bytes[IMAGE_LAYER_BYTES_OFFSET], layer_bytes);
	store32(&bytes[20], 0);
	store32(&bytes[24], 0);
	store32(&bytes[IMAGE_PACK_INFO_OFFSET], pack_info);
}

/*
 * Writes a sampler descriptor: its two state words, the four words the
 * shader reads (the comparison, then three reserved), and the two words
 * of its gather form, which filters linearly with no mip filter.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_sampler.c
 * (pvr_CreateSampler: meta[COMPARE_OP] VK_COMPARE_OP_NEVER without
 * compare; the gather sampler: word 0 with mipfilter false, minfilter
 * and magfilter LINEAR, word 1 copied) and
 * src/imagination/vulkan/pvr_common.h (struct pvr_sampler_descriptor).
 */
void
powervr_descriptor_sampler(
	uint8_t bytes[POWERVR_DESCRIPTOR_SAMPLER_BYTES],
	const uint64_t words[2],
	uint32_t compare_op)
{
	uint64_t gather;

	/* The state words. */
	store64(&bytes[0], words[0]);
	store64(&bytes[8], words[1]);

	/* The comparison and the three reserved words. */
	store32(&bytes[SAMPLER_COMPARE_OFFSET], compare_op);
	store32(&bytes[20], 0);
	store32(&bytes[24], 0);
	store32(&bytes[28], 0);

	/* The gather form: linear filters, no mip filter, the second word as it is. */
	gather = (words[0] & ~SAMPLER_FILTER_MASK) | SAMPLER_GATHER_FILTER;
	store64(&bytes[32], gather);
	store64(&bytes[40], words[1]);
}

/*
 * Writes a storage image descriptor: the buffer descriptor of its texels
 * (the address of the view's first texel and the bytes to the image's
 * end), then its view's shape -- width, height, slices, and the 32-bit
 * words from row to row and from slice to slice -- and zeros to its end.
 * The lowering of the shader's storage images reads them
 * (spirv-lower-image.inc).
 */
void
powervr_descriptor_storage_image(
	uint8_t bytes[POWERVR_DESCRIPTOR_STORAGE_IMAGE_BYTES],
	uint64_t address,
	uint32_t range,
	const uint32_t shape[POWERVR_SHADER_SHAPE_WORDS])
{
	uint32_t index;

	/* The texels' buffer descriptor. */
	powervr_descriptor_buffer(&bytes[0], address, range);

	/* The shape's words, then zeros. */
	for (index = 0; index < (POWERVR_DESCRIPTOR_STORAGE_IMAGE_BYTES - STORAGE_SHAPE_OFFSET) / 4U; index++) {
		if (index < POWERVR_SHADER_SHAPE_WORDS)
			store32(&bytes[STORAGE_SHAPE_OFFSET + index * 4U], shape[index]);
		else
			store32(&bytes[STORAGE_SHAPE_OFFSET + index * 4U], 0);
	}
}

/* Reports the bytes between descriptors of a type, 0 for a type the executor does not take. */
static uint32_t
descriptor_stride(
	uint32_t type)
{
	/* The size of each kind. */
	switch (type) {
	case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
	case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
	case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
		return POWERVR_DESCRIPTOR_BUFFER_BYTES;
	case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
		return POWERVR_DESCRIPTOR_IMAGE_BYTES;
	case VK_DESCRIPTOR_TYPE_SAMPLER:
		return POWERVR_DESCRIPTOR_SAMPLER_BYTES;
	case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
		return POWERVR_DESCRIPTOR_COMBINED_BYTES;
	case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
		return POWERVR_DESCRIPTOR_STORAGE_IMAGE_BYTES;
	default:
		break;
	}

	/* Not a type the executor takes. */
	return 0;
}

/* Stores a 32-bit word in little-endian order. */
static void
store32(
	uint8_t *bytes,
	uint32_t value)
{
	/* Each byte, lowest first. */
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

/* Stores a 64-bit word in little-endian order. */
static void
store64(
	uint8_t *bytes,
	uint64_t value)
{
	/* The low word, then the high one. */
	store32(&bytes[0], (uint32_t)value);
	store32(&bytes[4], (uint32_t)(value >> 32));
}
