/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The device-independent SPIR-V frontend shared by the GPU backends.
 *
 * Parsing owns no hardware or GPU allocation. The caller selects a stage,
 * receives a complete scalar IR, and lowers it using its vendor backend.
 */

#ifndef DRIVERS_GPU_COMPILER_SPIRV_H
#define DRIVERS_GPU_COMPILER_SPIRV_H

#include "ir.h"

#include <stddef.h>
#include <stdint.h>

/*
 * The synthetic input locations after all user attributes and varyings.
 * Each backend maps them to its own vertex or fragment payload fields.
 */
#define DRV_GPU_SHADER_LOCATION_VERTEX_INDEX 64U
#define DRV_GPU_SHADER_LOCATION_INSTANCE_INDEX 65U
#define DRV_GPU_SHADER_LOCATION_FRONT_FACING 66U
#define DRV_GPU_SHADER_LOCATION_FRAG_COORD 67U
#define DRV_GPU_SHADER_LOCATION_POINT_COORD 68U
#define DRV_GPU_SHADER_LOCATION_PRIMITIVE_ID 69U

/* The current frontend profile retains its original compute admission limit. */
#define DRV_GPU_SPIRV_MAX_GROUP_INVOCATIONS 128U

/*
 * One module refusal, borrowed from the frontend's immutable reason strings.
 * The caller owns this record; successful parses and allocation failures
 * without a refused instruction leave it cleared.
 */
struct drv_gpu_compile_diagnostic {
	uint32_t opcode;
	uint32_t word_offset;
	const char *reason;
};

/*
 * The fragment shader's SampleMask input builtin (element 0 of the array),
 * read raw like a Flat input.  Only a parse with the GHCI features makes it.
 */
#define DRV_GPU_SHADER_LOCATION_SAMPLE_MASK 70U

/*
 * The fragment shader's SampleId input builtin, the number of the sample
 * the invocation runs for, read raw like a Flat input.  Only a parse with
 * the SAMPLE_ID feature makes it.
 */
#define DRV_GPU_SHADER_LOCATION_SAMPLE_ID 71U

/*
 * What a parse may lower beyond the profile every backend shares, as bits
 * of its `features`: the SampleMask input and output and the FragDepth
 * output builtins, the Centroid decoration recorded on the inputs,
 * OpImageQuerySamples, the storage images, storage texel buffers and
 * input attachments with OpImageRead and OpImageWrite, and images in a
 * compute shader.  A backend asks
 * only for what it lowers; without a bit the parse refuses what it names,
 * as before.
 */
#define DRV_GPU_SPIRV_FEATURE_SAMPLE_MASK 0x1U
#define DRV_GPU_SPIRV_FEATURE_FRAG_DEPTH 0x2U
#define DRV_GPU_SPIRV_FEATURE_CENTROID 0x4U
#define DRV_GPU_SPIRV_FEATURE_QUERY_SAMPLES 0x8U
#define DRV_GPU_SPIRV_FEATURE_STORAGE_IMAGE 0x10U
#define DRV_GPU_SPIRV_FEATURE_INPUT_ATTACHMENT 0x20U

/* A compute shader's sampled and storage images (refused otherwise). */
#define DRV_GPU_SPIRV_FEATURE_COMPUTE_IMAGES 0x40U

/*
 * A fragment shader's SampleId input and FragStencilRefEXT output
 * (SPV_EXT_shader_stencil_export) builtins (refused otherwise).
 */
#define DRV_GPU_SPIRV_FEATURE_SAMPLE_ID 0x80U
#define DRV_GPU_SPIRV_FEATURE_STENCIL_EXPORT 0x100U

/*
 * A module of several functions: the entry function's calls are inlined
 * before the parse (refused otherwise, as more than one function).
 */
#define DRV_GPU_SPIRV_FEATURE_INLINE_CALLS 0x200U

/*
 * Arrays of descriptors: of combined image samplers (SAMPLER_ARRAYS), and
 * of uniform blocks and storage buffers (BUFFER_ARRAYS), indexed by a
 * constant or at run time.  Each element is a uniform of its own
 * (struct drv_gpu_shader_ir_uniform, `element`); an index known only at run
 * time is lowered to an access of every element, each only for the channels
 * whose index names it, and a selection of the results, so a backend sees
 * constant elements alone.  A backend asks for them when the draw binds
 * the element each uniform names (refused otherwise).
 */
#define DRV_GPU_SPIRV_FEATURE_SAMPLER_ARRAYS 0x400U
#define DRV_GPU_SPIRV_FEATURE_BUFFER_ARRAYS 0x800U

/*
 * OpImageQueryLod, as a TEXTURE of the LOD message (refused otherwise).
 */
#define DRV_GPU_SPIRV_FEATURE_QUERY_LOD 0x1000U

/*
 * A geometry shader of more than one invocation for each input primitive
 * (the Invocations execution mode up to DRV_GPU_IR_MAX_GEOMETRY_INVOCATIONS,
 * and gl_InvocationID as a LOAD_SYSTEM), and one that writes gl_PointSize
 * (refused otherwise).
 */
#define DRV_GPU_SPIRV_FEATURE_GEOMETRY_INVOCATIONS 0x2000U
#define DRV_GPU_SPIRV_FEATURE_GEOMETRY_POINT_SIZE 0x4000U

/*
 * Arrays of storage images, with the STORAGE_IMAGE feature: each element
 * is a storage image uniform of its own, as SAMPLER_ARRAYS makes the
 * elements of an array of combined image samplers, and an element chosen
 * at run time is lowered as theirs is (refused otherwise).
 */
#define DRV_GPU_SPIRV_FEATURE_IMAGE_ARRAYS 0x8000U

/*
 * Storage images lowered to buffers before the parse: each storage image
 * becomes a storage buffer of its texel words at its own binding and a
 * uniform block of its view's shape at the binding with
 * DRV_GPU_SPIRV_IMAGE_INFO_BINDING set (drv_gpu_spirv_lower_storage_images),
 * so the IR has storage buffer and uniform block accesses only.  For a
 * backend without image instructions (refused otherwise).
 */
#define DRV_GPU_SPIRV_FEATURE_LOWER_STORAGE_IMAGES 0x10000U

/*
 * The size of a uniform texel buffer (OpImageQuerySize of a Buffer image,
 * desktop GL's textureSize of a samplerBuffer): its element count, as the
 * resinfo message of the image's shape, which STORAGE_IMAGE gives too
 * (refused otherwise).
 */
#define DRV_GPU_SPIRV_FEATURE_TEXEL_BUFFER_SIZE 0x20000U

/*
 * gl_ViewIndex in a graphics stage, as a LOAD_SYSTEM of
 * DRV_GPU_IR_SYSTEM_VIEW_INDEX that the driver binds to the view it
 * compiles the shader for with drv_gpu_shader_ir_bind_view(), or that its
 * backend reads from a value set for each view (refused otherwise).
 */
#define DRV_GPU_SPIRV_FEATURE_VIEW_INDEX 0x40000U

/*
 * vkCmdDispatchBase: gl_WorkGroupID (and gl_GlobalInvocationID) count from
 * the dispatch's first group, which the dispatch places in the system
 * storage buffer (DRV_GPU_IR_SYSTEM_SET) as three words after the three
 * group counts.  For a pipeline created with
 * VK_PIPELINE_CREATE_DISPATCH_BASE_BIT; without it the groups count from
 * zero.
 */
#define DRV_GPU_SPIRV_FEATURE_DISPATCH_BASE 0x80000U

/*
 * The bytes of the system storage buffer: the three group counts, and for
 * a pipeline with the DISPATCH_BASE feature the three words of the first
 * group after them.
 */
#define DRV_GPU_SPIRV_SYSTEM_BYTES 12U
#define DRV_GPU_SPIRV_SYSTEM_BASE_BYTES 24U

/*
 * The bit of the binding of a lowered storage image's shape: the uniform
 * block of eight words -- the view's width, height and slices (a 3D
 * image's depth, an array's or a cube's layers, six to a cube), the 32-bit
 * words from one row to the next and from one slice to the next, then
 * three zeros -- of the storage image at the binding without the bit.  A
 * module that names a binding with the bit is refused by the lowering.
 */
#define DRV_GPU_SPIRV_IMAGE_INFO_BINDING 0x40000000U

/* The words of a lowered storage image's shape, and their places in it. */
#define DRV_GPU_SPIRV_IMAGE_INFO_WORDS 8U
#define DRV_GPU_SPIRV_IMAGE_INFO_WIDTH 0U
#define DRV_GPU_SPIRV_IMAGE_INFO_HEIGHT 1U
#define DRV_GPU_SPIRV_IMAGE_INFO_SLICES 2U
#define DRV_GPU_SPIRV_IMAGE_INFO_ROW_WORDS 3U
#define DRV_GPU_SPIRV_IMAGE_INFO_SLICE_WORDS 4U

/* The most elements one array of descriptors may have. */
#define DRV_GPU_SPIRV_MAX_DESCRIPTOR_ARRAY 32U

/* The features the GHCI backend lowers. */
#define DRV_GPU_SPIRV_FEATURES_GHCI 0x7FU

int drv_gpu_shader_parse(const uint32_t *words, size_t word_count, enum drv_gpu_shader_stage stage, struct drv_gpu_shader_ir **out, struct drv_gpu_compile_diagnostic *diagnostic);
int drv_gpu_shader_parse_features(const uint32_t *words, size_t word_count, enum drv_gpu_shader_stage stage, uint32_t features, struct drv_gpu_shader_ir **out, struct drv_gpu_compile_diagnostic *diagnostic);
void drv_gpu_shader_ir_free(struct drv_gpu_shader_ir *ir);
void drv_gpu_shader_ir_bind_view(struct drv_gpu_shader_ir *ir, uint32_t view);
int drv_gpu_spirv_lower_storage_images(const uint32_t *words, size_t word_count, uint32_t **out, size_t *out_count, struct drv_gpu_compile_diagnostic *diagnostic);

#endif /* DRIVERS_GPU_COMPILER_SPIRV_H */
