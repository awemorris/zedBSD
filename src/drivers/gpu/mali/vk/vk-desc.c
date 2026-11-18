/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Valhall descriptor packers.  See vk-desc.h.
 *
 * Values referenced (every field place below unless a block names
 * another): Mesa 26.2.6 src/panfrost/genxml/v10.xml (the structs named
 * in each packer's comment, and the enums "Descriptor Type", "Buffer
 * Type", "Shader Register Allocation", "Texture Dimension", "Plane
 * Type", "Clump Ordering", "Clump Format", "Attribute Type", "Blend
 * Mode", "Blend Operand A/B/C", "Block Format", "Func", "Depth Clamp
 * Mode", "Depth Source", "Address Mode").
 */

#include <stdbool.h>
#include <stdint.h>

#include "drivers/gpu/mali/vk/vk-desc.h"

/* The descriptor types. */
#define TYPE_TEXTURE		2U
#define TYPE_ATTRIBUTE		5U
#define TYPE_SHADER		8U
#define TYPE_BUFFER		9U
#define TYPE_PLANE		10U

/* The compares whose operands' sides matter (VkCompareOp's numbers, the hardware's too). */
#define VK_COMPARE_LESS		1U
#define VK_COMPARE_LESS_EQUAL	3U
#define VK_COMPARE_GREATER	4U
#define VK_COMPARE_GREATER_EQUAL	6U

/* A blend's sRGB and round to the target's precision bits, and the memory format's sRGB bit. */
#define BLEND_SRGB		(1U << 10)
#define BLEND_ROUND		(1U << 11)
#define BLEND_FORMAT_SRGB	(1U << 20)

/* The framebuffer's tie-break rule of one sample: -180 in, 0 out. */
#define TIE_BREAK_MINUS_180_IN	2U

/* A program descriptor's bit 8: the GL coverage bitmask of a fragment program, half warps of a position program. */
#define SHADER_BIT8		(1U << 8)

/* The buffer types, the register allocations, and the other enum values used. */
#define BUFFER_SIMPLE		1U
#define BUFFER_TILER_HEAP	2U
#define ALLOCATION_64		0U
#define ALLOCATION_32		2U
#define DIMENSION_2D		2U
#define PLANE_GENERIC		1U
#define CLUMP_LINEAR		2U
#define ATTRIBUTE_1D		1U
#define BLEND_FIXED		2U
#define BLOCK_LINEAR		2U
/*
 * The Local Storage word 1 of no workgroup memory: the WLS Instances
 * field (1:0, five bits) holds the log2 of the instances, and the
 * enumerated "No Workgroup Mem" value 0x80000000 goes in as its log2.
 *
 * Values referenced: Mesa 26.2.6 src/panfrost/genxml/v10.xml (struct
 * "Local Storage", WLS Instances with the log2 modifier and its value
 * No Workgroup Mem) and genxml/gen_pack.py (a log2 field packs
 * util_logbase2 of its value).
 */
#define NO_WORKGROUP_MEMORY	0x1fU
#define TLS_PACKED		1U

/* The blend operands. */
#define OPERAND_A_ZERO		1U
#define OPERAND_A_SRC		2U
#define OPERAND_A_DEST		3U
#define OPERAND_B_SRC_MINUS_DEST	0U
#define OPERAND_B_SRC_PLUS_DEST	1U
#define OPERAND_B_SRC		2U
#define OPERAND_B_DEST		3U
#define OPERAND_C_ZERO		1U
#define OPERAND_C_SRC		2U
#define OPERAND_C_DEST		3U
#define OPERAND_C_SRC_ALPHA	5U
#define OPERAND_C_DEST_ALPHA	6U
#define OPERAND_C_CONSTANT	7U

/* The Vulkan blend factors and operations the fixed function takes. */
#define FACTOR_ZERO		0U
#define FACTOR_ONE		1U
#define FACTOR_SRC_COLOR	2U
#define FACTOR_DST_COLOR	4U
#define FACTOR_SRC_ALPHA	6U
#define FACTOR_DST_ALPHA	8U
#define FACTOR_CONSTANT_COLOR	10U
#define FACTOR_CONSTANT_ALPHA	12U
#define OP_ADD			0U
#define OP_SUBTRACT		1U
#define OP_REVERSE_SUBTRACT	2U

static void put64(uint32_t *words, unsigned at, uint64_t value);
static void clear(uint32_t *words, unsigned count);
static uint32_t factor_base(uint32_t factor);
static bool factor_inverted(uint32_t factor);
static uint32_t operand_c(uint32_t factor);
static uint32_t blend_function(uint32_t source, uint32_t destination, uint32_t operation, bool *supported);

/*
 * Packs a shader program descriptor (struct "Shader Program"): the
 * type, the stage, bit 8 (a fragment program's coverage bitmask of the
 * GL type; a position program's warp limit of half; clear for a varying
 * program, MALI_DESC_STAGE_VARYING with the vertex stage, and a compute
 * program), 32 or 64 registers a thread, the preloaded registers (bit i
 * for r(48 + i)) and the code's address.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (struct "Shader Program":
 * fragment coverage bitmask type and vertex warp limit at 0:8; enums
 * "Coverage bitmask type" GL 1, "Warp limit" full 0 and half 1) and
 * src/panfrost/vulkan/panvk_vX_shader.c (the program descriptors before
 * PAN_ARCH 12: fragment GL, position half, varying full).
 */
void
mali_desc_shader(
	uint32_t *words,
	unsigned stage,
	uint32_t work_registers,
	uint32_t preload,
	uint64_t code)
{
	uint32_t allocation;

	/* More than 32 registers takes the 64-register allocation. */
	clear(words, 8);
	allocation = ALLOCATION_32;
	if (work_registers > 32U)
		allocation = ALLOCATION_64;

	/* Word 0: type 0..3, stage 4..7, allocation 30..31; word 1: the preloads; words 2..3: the code. */
	words[0] = TYPE_SHADER | (((uint32_t)stage & 0xfU) << 4) | (allocation << 30);

	/* Bit 8: a fragment program's GL coverage bitmask, a position program's half warps. */
	if (stage == MALI_DESC_STAGE_FRAGMENT || stage == MALI_DESC_STAGE_VERTEX)
		words[0] |= SHADER_BIT8;
	words[1] = preload & 0xffffU;
	put64(words, 2, code);
}

/*
 * Packs a thread storage descriptor (struct "Local Storage") of no
 * thread-local and no workgroup memory.
 */
void
mali_desc_local_storage(
	uint32_t *words)
{
	clear(words, 8);
	words[1] = NO_WORKGROUP_MEMORY;
}

/*
 * Packs one resource table entry (struct "Resource"): a table of
 * descriptors at an address, of a number of bytes.
 */
void
mali_desc_resource(
	uint32_t *words,
	uint64_t address,
	uint64_t bytes)
{
	/* The address (56 bits), whether it holds descriptors (word 1 bit 24), the bytes. */
	clear(words, 4);
	put64(words, 0, address & 0x00ffffffffffffffULL);
	if (bytes != 0U)
		words[1] |= 1U << 24;
	put64(words, 2, bytes);
}

/* Packs a plain buffer descriptor (struct "Buffer"): type, simple, size, address. */
void
mali_desc_buffer(
	uint32_t *words,
	uint64_t address,
	uint32_t bytes)
{
	clear(words, 8);
	words[0] = TYPE_BUFFER | (BUFFER_SIMPLE << 4);
	words[1] = bytes;
	put64(words, 2, address);
}

/*
 * Packs a 2D texture descriptor of one level and layer (struct
 * "Texture"): its pixel format, its extent, its swizzle and the address
 * of its plane descriptor.
 */
void
mali_desc_texture(
	uint32_t *words,
	uint32_t pixel,
	uint32_t width,
	uint32_t height,
	uint32_t swizzle,
	uint64_t planes)
{
	/* Word 0: type, dimension 4..6, format 10..31; word 1: the extent less one; word 2: the swizzle. */
	clear(words, 8);
	words[0] = TYPE_TEXTURE | (DIMENSION_2D << 4) | ((pixel & 0x3fffffU) << 10);
	words[1] = ((width - 1U) & 0xffffU) | (((height - 1U) & 0xffffU) << 16);
	words[2] = swizzle & 0xfffU;

	/* Words 4..5: the planes; word 6: one layer; word 7: one deep (both less one, zero). */
	put64(words, 4, planes);
}

/*
 * Packs a generic linear plane (struct "Generic Plane"): the clump
 * format of the texel size, the bytes, the address, the row stride and
 * the extent.
 */
void
mali_desc_plane(
	uint32_t *words,
	uint32_t texel_bytes,
	uint64_t address,
	uint32_t row_stride,
	uint64_t bytes,
	uint32_t width,
	uint32_t height)
{
	/* Word 0: type, plane type 4..7, clump ordering 8..11, clump format 24..31. */
	clear(words, 8);
	words[0] = TYPE_PLANE | (PLANE_GENERIC << 4) | (CLUMP_LINEAR << 8) | (mali_desc_clump_format(texel_bytes) << 24);

	/* The bytes, the address, the row and slice strides and the extent less one. */
	words[1] = (uint32_t)bytes;
	put64(words, 2, address);
	words[4] = row_stride;
	words[6] = (uint32_t)bytes;
	words[7] = ((width - 1U) & 0xffffU) | (((height - 1U) & 0xffffU) << 16);
}

/*
 * Packs a vertex attribute descriptor (struct "Attribute"): its format,
 * the table of the buffer descriptors it reads, per vertex or instance,
 * its offset, its buffer's index there and the stride.
 */
void
mali_desc_attribute(
	uint32_t *words,
	uint32_t pixel,
	uint32_t table,
	bool instance,
	int32_t offset,
	uint32_t buffer,
	uint32_t stride)
{
	/* Word 0: type, 1D 4..7, offset enable 8, format 10..31; word 1: table 0..5, frequency 6..7. */
	clear(words, 8);
	words[0] = TYPE_ATTRIBUTE | (ATTRIBUTE_1D << 4) | (1U << 8) | ((pixel & 0x3fffffU) << 10);
	words[1] = table & 0x3fU;
	if (instance)
		words[1] |= 1U << 6;

	/* The offset, the buffer index and the stride. */
	words[2] = (uint32_t)offset;
	words[3] = buffer;
	words[4] = stride;
}

/* Packs the workgroup size word (struct "Compute size workgroup"): each size less one, ten bits apart. */
uint32_t
mali_desc_workgroup(
	uint32_t x,
	uint32_t y,
	uint32_t z)
{
	uint32_t word;

	word = (x - 1U) & 0x3ffU;
	word |= ((y - 1U) & 0x3ffU) << 10;
	word |= ((z - 1U) & 0x3ffU) << 20;
	return word;
}

/*
 * Packs a fixed-function blend descriptor (struct "Blend", "Blend
 * Fixed-Function"): whether it reads the destination, the result
 * rounded to the target's precision, sRGB when the memory format is
 * (its bit 20), the equation word, four components into render target
 * `target`, converted from the memory format.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (struct "Blend": sRGB 0:10,
 * round to FB precision 0:11) and src/panfrost/vulkan/panvk_vX_blend.c
 * (emit_blend_desc: sRGB of an sRGB format, round to FB precision on
 * every enabled blend).
 */
void
mali_desc_blend(
	uint32_t *words,
	bool load_destination,
	uint32_t equation,
	uint32_t color_mask,
	unsigned target,
	uint32_t memory_format)
{
	/* Word 0: load destination 0, enable 9; word 1: the equation; words 2..3: fixed function. */
	clear(words, 4);
	words[0] = (1U << 9) | BLEND_ROUND;
	if (load_destination || color_mask != 0xfU)
		words[0] |= 1U;
	if ((memory_format & BLEND_FORMAT_SRGB) != 0U)
		words[0] |= BLEND_SRGB;
	words[1] = equation;
	words[2] = BLEND_FIXED | (3U << 3) | (((uint32_t)target & 0xfU) << 16);
	words[3] = memory_format & 0x3fffffU;
}

/*
 * Builds the blend equation word (struct "Blend Equation") of a Vulkan
 * attachment's state packed as the pipeline keeps it: the colour source
 * factor 0..4, destination factor 5..9, operation 10..12, the alpha
 * source 13..17, destination 18..22, operation 23..25.  supported
 * says false when the fixed function cannot do it (the caller then
 * blends as a replace).
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/panfrost/lib/pan_blend.c (to_mali_function,
 * to_c_factor, pan_blend_to_fixed_function_equation: the equation is
 * A + B * C, with the cases of a zero or one factor and of equal
 * factors).
 */
uint32_t
mali_desc_blend_equation(
	uint32_t vk_equation,
	uint32_t color_mask,
	bool *supported)
{
	uint32_t rgb;
	uint32_t alpha;

	/* Each half's function. */
	*supported = true;
	rgb = blend_function(vk_equation & 0x1fU, (vk_equation >> 5) & 0x1fU, (vk_equation >> 10) & 7U, supported);
	alpha = blend_function((vk_equation >> 13) & 0x1fU, (vk_equation >> 18) & 0x1fU, (vk_equation >> 23) & 7U, supported);

	/* A replace when the fixed function cannot blend it. */
	if (!*supported) {
		rgb = OPERAND_A_SRC | (OPERAND_B_SRC << 4) | (OPERAND_C_ZERO << 8);
		alpha = rgb;
	}

	/* RGB 0..11, alpha 12..23, the colour mask 28..31. */
	return rgb | (alpha << 12) | ((color_mask & 0xfU) << 28);
}

/*
 * Packs a linear render target (struct "RGB Render Target"): the
 * internal buffer's offset and format, the writeback format, block
 * format and swizzle, the memory and the clear colour.
 */
void
mali_desc_target(
	uint32_t *words,
	const struct mali_desc_target *target)
{
	/* Word 0: internal buffer offset 4..15 (in 16 bytes), internal format 26..31. */
	clear(words, 16);
	words[0] = ((target->internal_offset >> 4) & 0xfffU) << 4;
	words[0] |= (target->internal & 0x3fU) << 26;

	/* Word 1: write enable 0, writeback format 3..7, linear 8..11, sRGB 14, swizzle 16..27. */
	if (target->write)
		words[1] = 1U;
	words[1] |= (target->writeback & 0x1fU) << 3;
	words[1] |= BLOCK_LINEAR << 8;
	if (target->srgb)
		words[1] |= 1U << 14;
	words[1] |= (target->swizzle & 0xfffU) << 16;

	/* Words 8..11: the writeback buffer; 12..15: the clear. */
	put64(words, 8, target->base);
	words[10] = target->row_stride;
	words[11] = 0;
	words[12] = target->clear[0];
	words[13] = target->clear[1];
	words[14] = target->clear[2];
	words[15] = target->clear[3];
}

/*
 * Packs the framebuffer parameters (struct "Framebuffer Parameters") of
 * one sample: the extent, the bound, the tile size (log2 of its pixels),
 * the targets, their colour buffer's bytes a tile and the tiler context.
 * The sample positions table is the caller's (words 4..5).
 */
void
mali_desc_framebuffer(
	uint32_t *words,
	uint32_t width,
	uint32_t height,
	uint32_t targets,
	uint32_t tile_size,
	uint32_t color_buffer_bytes,
	uint64_t tiler)
{
	uint32_t log2;

	/* The tile size's base-two logarithm. */
	log2 = 0;
	while ((1U << log2) < tile_size)
		log2++;

	/* Words 8..10: the extent and the bound, less one. */
	clear(words, 16);
	words[8] = ((width - 1U) & 0xffffU) | (((height - 1U) & 0xffffU) << 16);
	words[9] = 0;
	words[10] = words[8];

	/*
	 * Word 11: the tie-break rule 6..8 (-180 in, 0 out, of one sample),
	 * the tile size 9..12, the first vertex provoking 14, the point
	 * sprite's origin at the top (bit 15 clear), the targets 19..22, the
	 * colour buffer 24..31.
	 *
	 * Values referenced:
	 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (struct "Framebuffer
	 * Parameters": tie-break rule 11:6; enum "Tie-Break Rule":
	 * MINUS_180_IN_0_OUT 2), src/panfrost/lib/pan_desc.c (pan_emit_fbd:
	 * that rule below 16 samples) and the Vulkan specification 1.0
	 * (PointCoord's origin is the upper left).
	 */
	words[11] = TIE_BREAK_MINUS_180_IN << 6;
	words[11] |= (log2 & 0xfU) << 9;
	words[11] |= 1U << 14;
	words[11] |= ((targets - 1U) & 0xfU) << 19;
	words[11] |= ((color_buffer_bytes >> 10) & 0xffU) << 24;

	/* Words 14..15: the tiler context. */
	put64(words, 14, tiler);
}

/*
 * Packs a tiler heap descriptor (struct "Tiler Heap"): one chunk at
 * base, used from after its 64-byte header to its end.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c (the heap
 * descriptor's bottom 64 bytes past the base, its top at base + size).
 */
void
mali_desc_tiler_heap(
	uint32_t *words,
	uint64_t base,
	uint32_t bytes)
{
	clear(words, 8);
	words[0] = TYPE_BUFFER | (BUFFER_TILER_HEAP << 4);
	words[1] = bytes;
	put64(words, 2, base);
	put64(words, 4, base + 64U);
	put64(words, 6, base + bytes);
}

/*
 * Packs a tiler context (struct "Tiler Context") of one layer: the
 * hierarchy, the first vertex provoking, the framebuffer's extent, the
 * heap descriptor and the geometry buffer.  The completed pointers and
 * the private state start zero.
 */
void
mali_desc_tiler(
	uint32_t *words,
	uint32_t width,
	uint32_t height,
	uint64_t heap,
	uint64_t geometry,
	uint32_t geometry_bytes,
	uint32_t hierarchy_mask)
{
	/* Word 2: the hierarchy 0..12, first provoking vertex 18; word 3: the extent less one. */
	clear(words, 32);
	words[2] = (hierarchy_mask & 0x1fffU) | (1U << 18);
	words[3] = ((width - 1U) & 0xffffU) | (((height - 1U) & 0xffffU) << 16);

	/* Words 6..7: the heap; 8..9: the geometry buffer's size and address in 4 KiB units. */
	put64(words, 6, heap);
	put64(words, 8, ((uint64_t)(geometry_bytes >> 12) & 0xfffU) | ((geometry >> 12) << 12));
}

/*
 * Builds the low word of a framebuffer pointer (struct "Framebuffer
 * pointer"): the type bit, the targets less one 2..5, the address in
 * 64-byte units from bit 6.
 */
uint32_t
mali_desc_framebuffer_pointer(
	uint64_t fbd,
	uint32_t targets)
{
	return (uint32_t)(fbd & ~0x3fULL) | 1U | (((targets - 1U) & 0xfU) << 2);
}

/*
 * Packs a draw descriptor (struct "Draw"): the flags, the render target
 * mask, the blend descriptors, and the shader environment.
 */
void
mali_desc_draw(
	uint32_t *words,
	uint32_t flags0,
	uint32_t render_target_mask,
	uint64_t blend,
	uint32_t blend_count,
	uint64_t resources,
	uint64_t shader,
	uint64_t storage,
	uint64_t fau,
	uint32_t fau_count)
{
	/* Flags 0; flags 1: every sample, the targets. */
	clear(words, 32);
	words[0] = flags0;
	words[1] = 0xffffU | ((render_target_mask & 0xffU) << 16);

	/* The depth range 0 to 1, the blends. */
	words[6] = 0;
	words[7] = 0x3f800000U;
	put64(words, 12, (blend & ~0xfULL) | (blend_count & 0xfU));

	/* The shader environment from word 16: the FAU count, the tables, the program, the storage, the FAU. */
	words[17] = fau_count & 0xffU;
	put64(words, 24, resources);
	put64(words, 26, shader);
	put64(words, 28, storage);
	put64(words, 30, fau);
}

/*
 * Builds the primitive flags word (struct "Primitive Flags"): the draw
 * mode, the index type, both depth culls, a secondary (varying) shader,
 * the basic position FIFO.
 */
uint32_t
mali_desc_primitive(
	unsigned draw_mode,
	unsigned index_type)
{
	uint32_t word;

	word = (uint32_t)draw_mode & 0xfU;
	word |= ((uint32_t)index_type & 7U) << 8;
	word |= 1U << 16;
	word |= 1U << 17;
	word |= 1U << 22;
	return word;
}

/* Builds a scissor box's low word (struct "Scissor"): its minimum x and y. */
uint32_t
mali_desc_scissor_low(
	uint32_t x,
	uint32_t y)
{
	return (x & 0xffffU) | ((y & 0xffffU) << 16);
}

/* Builds a scissor box's high word: its maximum x and y (inclusive). */
uint32_t
mali_desc_scissor_high(
	uint32_t x,
	uint32_t y)
{
	return (x & 0xffffU) | ((y & 0xffffU) << 16);
}

/*
 * Gives the raw clump format of a texel size.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (enum "Clump Format": RAW8 0,
 * RAW16 1, RAW32 2, RAW64 3, RAW128 4, RAW96 10);
 * src/panfrost/lib/pan_texture.c (pan_clump_format: the raw format of
 * the block size).
 */
uint32_t
mali_desc_clump_format(
	uint32_t texel_bytes)
{
	/* One case a size. */
	switch (texel_bytes) {
	case 1:
		return 0U;
	case 2:
		return 1U;
	case 8:
		return 3U;
	case 12:
		return 10U;
	case 16:
		return 4U;
	default:
		return 2U;
	}
}

/*
 * Packs a texture descriptor (struct "Texture") of a view: the
 * dimension, the format, the extent less one, the swizzle (the identity
 * for storage), the levels less one, the maximum level of detail (the
 * last level, 8.8 fixed; zero for storage), the planes, the array size
 * and the depth less one.
 *
 * Procedure referenced: Mesa 26.2.6 src/panfrost/lib/pan_texture.c
 * (pan_sampled_texture_emit and pan_storage_texture_emit: a cube's array
 * size in cubes, the storage view's level of detail range and swizzle).
 */
void
mali_desc_texture_view(
	uint32_t *words,
	const struct mali_desc_view *view)
{
	uint32_t levels;
	uint32_t array_size;
	uint32_t depth;
	uint32_t swizzle;

	/* At least one level, layer and slice. */
	levels = view->levels;
	if (levels == 0U || view->storage)
		levels = 1U;
	array_size = view->array_size;
	if (array_size == 0U)
		array_size = 1U;
	depth = view->depth;
	if (depth == 0U)
		depth = 1U;
	swizzle = view->swizzle;
	if (view->storage)
		swizzle = 0x688U;

	/* Word 0: type, dimension 4..6, format 10..31; word 1: the extent; word 2: swizzle, levels 16..20. */
	clear(words, 8);
	words[0] = TYPE_TEXTURE | ((view->dimension & 7U) << 4) | ((view->pixel & 0x3fffffU) << 10);
	words[1] = ((view->width - 1U) & 0xffffU) | (((view->height - 1U) & 0xffffU) << 16);
	words[2] = (swizzle & 0xfffU) | (((levels - 1U) & 0x1fU) << 16);

	/* Word 3: the maximum level of detail 16..28 (one sample, no minimum). */
	words[3] = (((levels - 1U) << 8) & 0x1fffU) << 16;

	/* Words 4..5: the planes; word 6: the array size less one; word 7: the depth less one. */
	put64(words, 4, view->planes);
	words[6] = (array_size - 1U) & 0xffffU;
	words[7] = (depth - 1U) & 0xffffU;
}

/*
 * Packs a thread storage descriptor (struct "Local Storage") with a
 * thread's local storage of tls_bytes (none for zero) at tls_base, and
 * no workgroup memory.  The base, 4096-byte aligned, goes in shifted
 * right by 8 with the packed address mode, the size as the log2 of its
 * 16-byte units.
 *
 * Procedure referenced: Mesa 26.2.6 src/panfrost/lib/pan_desc.c
 * (pan_emit_tls for PAN_ARCH 9 and on) and pan_scratch.c
 * (pan_get_stack_shift).
 */
void
mali_desc_thread_storage(
	uint32_t *words,
	uint32_t tls_bytes,
	uint64_t tls_base)
{
	uint64_t pointer;

	/* No workgroup memory. */
	clear(words, 8);
	words[1] = NO_WORKGROUP_MEMORY;

	/* No local storage either. */
	if (tls_bytes == 0U)
		return;

	/* Word 0: the size; words 2..3: the base >> 8 (48 bits) and the packed mode at 28..31. */
	words[0] = mali_desc_tls_size(tls_bytes) & 0x1fU;
	pointer = (tls_base >> 8) & 0x0000ffffffffffffULL;
	words[2] = (uint32_t)pointer;
	words[3] = (uint32_t)(pointer >> 32) | (TLS_PACKED << 28);
}

/*
 * Gives the bytes one workgroup's instance of workgroup memory takes: a
 * workgroup's shared bytes rounded up to a power of two of at least the
 * least (zero for none).
 *
 * Procedure referenced: Mesa 26.2.6 src/panfrost/lib/pan_desc.h
 * (pan_wls_adjust_size: the next power of two of at least 128).
 */
uint32_t
mali_desc_wls_bytes(
	uint32_t shared_bytes)
{
	uint32_t bytes;

	/* None for none. */
	if (shared_bytes == 0U)
		return 0;

	/* The least power of two of at least the least holding them. */
	bytes = MALI_DESC_WLS_MIN_BYTES;
	while (bytes < shared_bytes && bytes < MALI_DESC_WLS_MAX_BYTES)
		bytes *= 2U;

	/* Succeeded: the bytes. */
	return bytes;
}

/*
 * Adds workgroup memory to a thread storage descriptor packed by
 * mali_desc_thread_storage(): instances of it a core (a power of two),
 * each of instance_bytes (as mali_desc_wls_bytes() gives them), from
 * wls_base (4096-byte aligned).  The instance count goes in as its
 * log2, the size as the log2 of the instance's bytes plus one; the size
 * base stays zero.
 *
 * Values referenced: Mesa 26.2.6 src/panfrost/genxml/v10.xml (struct
 * "Local Storage": WLS Instances 1:0 of 5 bits, a log2; WLS Size Base
 * 1:5; WLS Size Scale 1:8 of 5 bits; WLS Base Pointer 4:0 of 64 bits).
 *
 * Procedure referenced: Mesa 26.2.6 src/panfrost/lib/pan_desc.c
 * (pan_emit_tls: the size scale from the adjusted size).
 */
void
mali_desc_workgroup_storage(
	uint32_t *words,
	uint32_t instances,
	uint32_t instance_bytes,
	uint64_t wls_base)
{
	uint32_t instance_shift;
	uint32_t size_shift;

	/* The log2 of the instances and of an instance's bytes. */
	instance_shift = 0U;
	while ((1U << instance_shift) < instances && instance_shift < 31U)
		instance_shift++;
	size_shift = 0U;
	while ((1U << size_shift) < instance_bytes && size_shift < 31U)
		size_shift++;

	/* Word 1: the instances 0..4, the size scale 8..12; words 4..5: the base. */
	words[1] = (instance_shift & 0x1fU) | (((size_shift + 1U) & 0x1fU) << 8);
	put64(words, 4, wls_base);
}

/* Gives the TLS size field of a thread's bytes: log2 of its 16-byte units, rounded up. */
uint32_t
mali_desc_tls_size(
	uint32_t tls_bytes)
{
	uint32_t units;
	uint32_t shift;

	/* The units, and the least power of two holding them. */
	units = (tls_bytes + 15U) / 16U;
	shift = 0U;
	while ((1U << shift) < units && shift < 31U)
		shift++;

	/* Succeeded: the shift. */
	return shift;
}

/*
 * Gives a sampler's compare function field (word 1 bits 13..15) of a
 * VkCompareOp, whose numbers are the hardware's (never, less, equal,
 * less or equal, greater, not equal, greater or equal, always), with
 * the operands' sides exchanged: the hardware compares the texel with
 * the reference, Vulkan the reference with the texel, so less and
 * greater, and less or equal and greater or equal, trade places.
 *
 * Values referenced: Mesa 26.2.6 src/panfrost/genxml/v10.xml (enum
 * "Func"; struct "Sampler": compare function at 1:13),
 * src/panfrost/lib/pan_encoder.h (pan_flip_compare_func) and
 * src/panfrost/vulkan/panvk_vX_sampler.c (the sampler's compare
 * function flipped); the Vulkan specification (VkCompareOp).
 */
uint32_t
mali_desc_sampler_compare(
	uint32_t compare_op)
{
	uint32_t function;

	/* One case a function whose sides matter; the others are their own flip. */
	function = compare_op & 7U;
	switch (function) {
	case VK_COMPARE_LESS:
		function = VK_COMPARE_GREATER;
		break;
	case VK_COMPARE_GREATER:
		function = VK_COMPARE_LESS;
		break;
	case VK_COMPARE_LESS_EQUAL:
		function = VK_COMPARE_GREATER_EQUAL;
		break;
	case VK_COMPARE_GREATER_EQUAL:
		function = VK_COMPARE_LESS_EQUAL;
		break;
	default:
		break;
	}

	/* Succeeded: the field in place. */
	return function << 13;
}

/* Stores a 64-bit value as two words, low first. */
static void
put64(
	uint32_t *words,
	unsigned at,
	uint64_t value)
{
	words[at] = (uint32_t)value;
	words[at + 1U] = (uint32_t)(value >> 32);
}

/* Clears a number of words. */
static void
clear(
	uint32_t *words,
	unsigned count)
{
	unsigned index;

	for (index = 0; index < count; index++)
		words[index] = 0;
}

/* Gives a factor without its inversion: ONE is an inverted ZERO. */
static uint32_t
factor_base(
	uint32_t factor)
{
	/* ZERO and ONE, then the inverted ones (odd numbers past ONE) fold onto their base. */
	if (factor == FACTOR_ONE)
		return FACTOR_ZERO;
	if (factor >= FACTOR_SRC_COLOR && (factor & 1U) != 0U)
		return factor - 1U;
	return factor;
}

/* Says whether a factor is the inverse of its base. */
static bool
factor_inverted(
	uint32_t factor)
{
	/* ONE inverts ZERO; ONE_MINUS_* are the odd numbers past ONE. */
	if (factor == FACTOR_ONE)
		return true;
	if (factor >= FACTOR_SRC_COLOR && (factor & 1U) != 0U)
		return true;
	return false;
}

/* Gives the C operand of a factor's base. */
static uint32_t
operand_c(
	uint32_t factor)
{
	/* One case a base. */
	switch (factor_base(factor)) {
	case FACTOR_SRC_COLOR:
		return OPERAND_C_SRC;
	case FACTOR_DST_COLOR:
		return OPERAND_C_DEST;
	case FACTOR_SRC_ALPHA:
		return OPERAND_C_SRC_ALPHA;
	case FACTOR_DST_ALPHA:
		return OPERAND_C_DEST_ALPHA;
	case FACTOR_CONSTANT_COLOR:
	case FACTOR_CONSTANT_ALPHA:
		return OPERAND_C_CONSTANT;
	default:
		return OPERAND_C_ZERO;
	}
}

/* Builds one blend function (struct "Blend Function"): A + B * C. */
static uint32_t
blend_function(
	uint32_t source,
	uint32_t destination,
	uint32_t operation,
	bool *supported)
{
	uint32_t a;
	uint32_t b;
	uint32_t c;
	bool negate_a;
	bool negate_b;
	bool invert_c;
	uint32_t word;

	/* Only add and the two subtractions. */
	negate_a = false;
	negate_b = false;
	if (operation > OP_REVERSE_SUBTRACT) {
		*supported = false;
		return 0;
	}

	/* The case of a zero or one factor, or of factors of one base. */
	if (source == FACTOR_ZERO) {
		a = OPERAND_A_ZERO;
		b = OPERAND_B_DEST;
		negate_b = operation == OP_SUBTRACT;
		invert_c = factor_inverted(destination);
		c = operand_c(destination);
	} else if (source == FACTOR_ONE) {
		a = OPERAND_A_SRC;
		b = OPERAND_B_DEST;
		negate_b = operation == OP_SUBTRACT;
		negate_a = operation == OP_REVERSE_SUBTRACT;
		invert_c = factor_inverted(destination);
		c = operand_c(destination);
	} else if (destination == FACTOR_ZERO) {
		a = OPERAND_A_ZERO;
		b = OPERAND_B_SRC;
		negate_b = operation == OP_REVERSE_SUBTRACT;
		invert_c = factor_inverted(source);
		c = operand_c(source);
	} else if (destination == FACTOR_ONE) {
		a = OPERAND_A_DEST;
		b = OPERAND_B_SRC;
		negate_a = operation == OP_SUBTRACT;
		negate_b = operation == OP_REVERSE_SUBTRACT;
		invert_c = factor_inverted(source);
		c = operand_c(source);
	} else if (factor_base(source) == factor_base(destination) && factor_inverted(source) != factor_inverted(destination)) {
		a = OPERAND_A_DEST;
		invert_c = factor_inverted(source);
		c = operand_c(source);
		b = OPERAND_B_SRC_MINUS_DEST;
		if (operation == OP_REVERSE_SUBTRACT) {
			b = OPERAND_B_SRC_PLUS_DEST;
			negate_b = true;
		} else if (operation == OP_SUBTRACT) {
			b = OPERAND_B_SRC_PLUS_DEST;
			negate_a = true;
		}
	} else {
		*supported = false;
		return 0;
	}

	/* A 0..1, negate A 3, B 4..5, negate B 7, C 8..10, invert C 11. */
	word = a | (b << 4) | (c << 8);
	if (negate_a)
		word |= 1U << 3;
	if (negate_b)
		word |= 1U << 7;
	if (invert_c)
		word |= 1U << 11;
	return word;
}
