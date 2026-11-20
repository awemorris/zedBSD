/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The amdrdna shader compiler: SPIR-V (through the shared scalar IR) to
 * RDNA1 (GFX10.1) programs that run as wave32.
 *
 * A program finds everything it binds through one resource table, whose
 * GPU address the driver puts in s[0:1]: the push constants from offset
 * 0, then the buffer descriptors of the storage buffers and of the
 * uniform buffers, then the image and sampler descriptors of the sampled
 * images, then the descriptors of the storage images and storage texel
 * buffers, then (a vertex program) the vertex buffer descriptors.  The
 * compiler chooses the layout and says it in the program; the driver
 * writes the table for each draw or dispatch.
 */

#ifndef DRIVERS_GPU_AMDRDNA_COMPILER_GFX10_COMPILE_H
#define DRIVERS_GPU_AMDRDNA_COMPILER_GFX10_COMPILE_H

#include "drivers/gpu/compiler/ir.h"
#include "drivers/gpu/compiler/spirv.h"
#include "gfx10-isa.h"

#include <stddef.h>
#include <stdint.h>

/*
 * What the parse lowers beyond the profile every backend shares
 * (drv_gpu_shader_parse_features()): a pixel program's FragDepth output,
 * its FragStencilRefEXT output, its SampleMask input and output,
 * OpImageQueryLod, the storage images and storage texel buffers
 * (OpImageRead, OpImageWrite and the atomics of an R32_UINT or R32_SINT
 * texel), and images in a compute program.
 */
#define AMDRDNA_GFX10_SPIRV_FEATURES	(DRV_GPU_SPIRV_FEATURE_FRAG_DEPTH | DRV_GPU_SPIRV_FEATURE_STENCIL_EXPORT | \
					 DRV_GPU_SPIRV_FEATURE_SAMPLE_MASK | DRV_GPU_SPIRV_FEATURE_QUERY_LOD | \
					 DRV_GPU_SPIRV_FEATURE_STORAGE_IMAGE | DRV_GPU_SPIRV_FEATURE_COMPUTE_IMAGES | \
					 DRV_GPU_SPIRV_FEATURE_VIEW_INDEX)

/*
 * The arrays of descriptors: each element is a binding of its own (struct
 * amdrdna_gfx10_binding, `element`).  A compile asks for them
 * (struct amdrdna_gfx10_compile_options, `descriptor_arrays`) where the
 * draw and the dispatch write the descriptor of the element each binding
 * names.
 */
#define AMDRDNA_GFX10_SPIRV_FEATURES_ARRAYS	(DRV_GPU_SPIRV_FEATURE_SAMPLER_ARRAYS | DRV_GPU_SPIRV_FEATURE_BUFFER_ARRAYS)

/*
 * One buffer or sampled image the program binds: its (set, binding) and
 * the element of an array of descriptors (0 for a binding that is not
 * one), the byte offset of its descriptor in the resource table, and for
 * a uniform buffer the bytes the program reads (from 0).
 *
 * A sampled image read as a texel buffer takes a buffer descriptor (four
 * words, records of the view's format by texel index) in the place of the
 * image descriptor, and no sampler; one read as a multisampled image takes
 * the image descriptor of the multisampled image, whose samples are read
 * as they are stored (an image the driver keeps without its FMASK
 * compression, or with it expanded).
 *
 * A storage image takes the image descriptor of its view (its one level,
 * a cube's faces as an array of six layers) and no sampler; a storage
 * texel buffer (texel_buffer set) takes a buffer descriptor as a texel
 * buffer's in the image descriptor's place.
 */
struct amdrdna_gfx10_binding {
	uint32_t set;
	uint32_t binding;
	uint32_t element;
	uint32_t table_offset;
	uint32_t size;
	uint32_t texel_buffer;
	uint32_t multisampled;
};

/*
 * One compiled program.
 *
 * Made by amdrdna_gfx10_compile() and owned by the caller, who copies the
 * code into GPU memory (256-byte aligned) and frees it with
 * amdrdna_gfx10_program_free().  The code ends with the padding the
 * instruction prefetcher may read.
 */
struct amdrdna_gfx10_program {
	/* The stage the program runs in. */
	enum drv_gpu_shader_stage stage;

	/* The code, in 32-bit words, padded. */
	uint32_t *code;
	uint32_t code_words;

	/*
	 * The VGPRs and SGPRs the program uses (counted from 0), and the user
	 * SGPRs the driver loads (s0 .. s(user_sgprs - 1)).
	 */
	uint32_t vgpr_count;
	uint32_t sgpr_count;
	uint32_t user_sgprs;

	/* Whether the program reads gl_ViewIndex (compiled for the view of its options). */
	uint32_t reads_view;

	/* A compute program's workgroup size, shared memory bytes and whether it waits at a workgroup barrier. */
	uint32_t local_size[3];
	uint32_t shared_bytes;
	uint32_t uses_barrier;

	/* The push constant bytes the program reads, at offset 0 of the table. */
	uint32_t push_bytes;

	/* The bytes of the resource table the driver writes. */
	uint32_t table_bytes;

	/*
	 * A vertex program: the locations it fetches from vertex buffers (bit
	 * L), whose descriptors start at vertex_table_offset (location L's at
	 * vertex_table_offset + 16 L); the parameter locations it exports (bit
	 * L, exported as parameter L); and whether it writes the point size.
	 */
	uint32_t fetched_locations;
	uint32_t vertex_table_offset;
	uint32_t parameter_outputs;
	uint32_t writes_point_size;

	/*
	 * A pixel program: the input VGPRs it has the hardware deliver (the
	 * bits of the pixel input enable), the attribute locations it reads
	 * (bit L, read as attribute L) and those read flat, the colour
	 * locations it writes (bit L: the n-th one set is exported as colour
	 * export n, which the colour unit gives to target L), and whether it
	 * discards.
	 */
	uint32_t pixel_inputs;
	uint32_t attribute_inputs;
	uint32_t flat_inputs;
	uint32_t color_outputs;
	uint32_t discards;

	/*
	 * A pixel program: how it exports each colour location
	 * (AMDRDNA_GFX10_COLOUR_*, 4 bits a location), as the options said.
	 */
	uint32_t color_exports;

	/*
	 * A pixel program: whether it writes the depth, the stencil reference
	 * and the sample mask, which it exports to the depth target.
	 */
	uint32_t writes_depth;
	uint32_t writes_stencil;
	uint32_t writes_sample_mask;

	/*
	 * A vertex program: whether it exports the primitive number (the
	 * driver then has the hardware deliver it).  A pixel program: the
	 * attribute it reads the point sprite coordinate from and the one it
	 * reads the primitive number from (AMDRDNA_GFX10_NO_ATTRIBUTE for
	 * none), and whether its second colour (of a dual-source blend) is
	 * exported as colour 1.
	 */
	uint32_t exports_primitive_id;
	uint32_t point_coord_attribute;
	uint32_t primitive_id_attribute;
	uint32_t dual_source;

	/*
	 * A vertex program: whether it runs as a primitive shader (exporting
	 * its primitives too) rather than as a legacy vertex program, and
	 * whether that primitive shader culls its primitives (it then takes
	 * the culling's user SGPRs and shared_bytes of LDS).
	 */
	uint32_t primitive_shader;
	uint32_t primitive_culling;

	/* The storage buffers, the uniform buffers and the sampled images, and where their descriptors go. */
	struct amdrdna_gfx10_binding storage[AMDRDNA_GFX10_STORAGE_MAX];
	uint32_t storage_used;
	struct amdrdna_gfx10_binding uniforms[AMDRDNA_GFX10_UNIFORM_MAX];
	uint32_t uniform_used;
	struct amdrdna_gfx10_binding textures[AMDRDNA_GFX10_TEXTURE_MAX];
	uint32_t texture_used;

	/* The storage images and storage texel buffers, and where their descriptors go. */
	struct amdrdna_gfx10_binding images[AMDRDNA_GFX10_IMAGE_MAX];
	uint32_t image_used;
};

/*
 * How a pixel program exports the colour of a location, by the format of
 * the target the location's colour goes to: four 16-bit floats (an 8-bit
 * normalized target or a 16-bit float one: the default, 0); the 32-bit
 * channels red, red and green, red and alpha, or all four, as they are
 * (a target of 32-bit channels, and a 32-bit red one whose alpha covers
 * samples); four 16-bit integers, unsigned or signed, saturated (a target
 * of 16-bit integers), or first clamped to the 8 bits of an 8-bit integer
 * target.  4 bits a location in
 * amdrdna_gfx10_compile_options.color_exports.
 *
 * Values referenced: Mesa 25.0.7 src/amd/common/ac_shader_util.c
 * (ac_choose_spi_color_formats: FP16_ABGR for 8_8_8_8 UNORM, SNORM and
 * SRGB and 16_16_16_16 FLOAT; UINT16_ABGR and SINT16_ABGR for the 8-bit
 * and 16-bit integers; 32_R, or 32_AR with alpha, for COLOR_32; 32_GR for
 * COLOR_32_32; 32_ABGR for COLOR_32_32_32_32) and
 * src/amd/compiler/aco_instruction_selection.cpp (export_fs_mrt_color: the
 * 8-bit integers clamped to 255, or to -128 .. 127, before the packing).
 */
#define AMDRDNA_GFX10_COLOUR_HALVES	0U
#define AMDRDNA_GFX10_COLOUR_R		1U
#define AMDRDNA_GFX10_COLOUR_GR		2U
#define AMDRDNA_GFX10_COLOUR_AR		3U
#define AMDRDNA_GFX10_COLOUR_ABGR	4U
#define AMDRDNA_GFX10_COLOUR_UINT16	5U
#define AMDRDNA_GFX10_COLOUR_SINT16	6U
#define AMDRDNA_GFX10_COLOUR_UINT8	7U
#define AMDRDNA_GFX10_COLOUR_SINT8	8U
#define AMDRDNA_GFX10_COLOUR_KINDS	9U
#define AMDRDNA_GFX10_COLOUR_BITS	4U
#define AMDRDNA_GFX10_COLOUR_MASK	0xfU

/*
 * What a pipeline asks of a compile beyond the module: a vertex program
 * that also exports the primitive number as parameter
 * AMDRDNA_GFX10_PRIMITIVE_ID_PARAMETER, because the pixel program reads it;
 * a vertex program compiled as a primitive shader (refused together
 * with the primitive number); a primitive shader that culls its
 * primitives; a module parsed with the arrays of descriptors
 * (AMDRDNA_GFX10_SPIRV_FEATURES_ARRAYS, refused otherwise); and how a
 * pixel program exports each colour location (AMDRDNA_GFX10_COLOUR_*, 4
 * bits a location from location 0; 0, four 16-bit floats, for every one
 * unless said); the view of a multiview pass a graphics program is
 * compiled for (its gl_ViewIndex; 0 outside multiview); and a compute
 * program for vkCmdDispatchBase (DRV_GPU_SPIRV_FEATURE_DISPATCH_BASE: its
 * workgroups count from the base after the counts in the system buffer).
 */
struct amdrdna_gfx10_compile_options {
	uint32_t pass_primitive_number;
	uint32_t primitive_shader;
	uint32_t primitive_culling;
	uint32_t descriptor_arrays;
	uint32_t color_exports;
	uint32_t view;
	uint32_t dispatch_base;
};

/*
 * Why a program was refused: the IR instruction (or the SPIR-V word
 * offset, for a refusal of the frontend) and an immutable reason.
 */
struct amdrdna_gfx10_diagnostic {
	uint32_t instruction;
	const char *reason;
};

/*
 * Compiles a SPIR-V module's stage with the options (NULL for none): 0
 * with the program, EINVAL or ENOTSUP with a reason, or ENOMEM.
 */
int amdrdna_gfx10_compile(const uint32_t *words, size_t word_count, enum drv_gpu_shader_stage stage, const struct amdrdna_gfx10_compile_options *options, struct amdrdna_gfx10_program **program, struct amdrdna_gfx10_diagnostic *diagnostic);

/* Compiles a parsed program (the IR is only read), as amdrdna_gfx10_compile(). */
int amdrdna_gfx10_compile_ir(const struct drv_gpu_shader_ir *ir, const struct amdrdna_gfx10_compile_options *options, struct amdrdna_gfx10_program **program, struct amdrdna_gfx10_diagnostic *diagnostic);

/* Frees a compiled program. */
void amdrdna_gfx10_program_free(struct amdrdna_gfx10_program *program);

/*
 * Reports how a pixel program exports a colour location
 * (AMDRDNA_GFX10_COLOUR_*): as its options said, the second colour of a
 * dual-source blend as the first.
 */
uint32_t amdrdna_gfx10_colour_export(const struct amdrdna_gfx10_program *program, uint32_t location);

#endif /* DRIVERS_GPU_AMDRDNA_COMPILER_GFX10_COMPILE_H */
