/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The nvrtx shader compiler: SPIR-V to Turing (SM75) code.
 *
 * The shared SPIR-V frontend makes the scalar IR
 * (drivers/gpu/compiler/); this backend chooses SM75 instructions for
 * it, assigns registers, marks the dependences, and encodes 128-bit
 * instructions.  The result says what the dispatch needs: the general
 * registers to reserve, the shared memory, the workgroup's size, and
 * which buffer goes where in the constant buffers (NVRTX_SM75_CB_*
 * below).
 *
 * Compute shaders compile with arithmetic, conversions, the special
 * functions, storage and shared memory with their atomics, workgroup
 * barriers and fences, integer division, and loops; vertex and fragment
 * programs compile with their shader program header:
 * attributes, interpolated inputs, colour, depth and sample mask outputs,
 * storage buffers, the texture operations (samples of 2D, 3D and cube
 * images and arrays of them, with a compare, a bias, a level or 2D
 * derivatives; fetches, gathers, size and level of detail queries),
 * derivatives, 16-bit float packing and discards; geometry programs
 * compile with their per-vertex inputs, their invocations, and the
 * vertices and strips they emit.  Derivatives of a 3D or cube sample or
 * with a compare, texel buffers, multisampled images, output streams other
 * than the first and spilling are refused with a reason.
 *
 * The same program is compiled for Turing (SM75, RTX 20 and GTX 16),
 * Ampere (SM86, RTX 30) or Ada (SM89, RTX 40): the instructions
 * chosen are the same, a global access's memory order field and the
 * latencies the dependence marking waits differ.  The functions
 * without a shader model compile for Turing.
 *
 * The rest of the driver includes only this header.  The compiler's
 * pieces share the others beside it: isa.h (the instruction set's facts),
 * encode.h (the encoder), schedule.h (the reordering and the dependence
 * marking) and lower.h (the lowering's state).
 */

#ifndef DRIVERS_GPU_NVRTX_COMPILER_SM75_SM75_H
#define DRIVERS_GPU_NVRTX_COMPILER_SM75_SM75_H

#include "drivers/gpu/compiler/ir.h"
#include "drivers/gpu/nvrtx/compiler/sm75/isa.h"

#include <stddef.h>
#include <stdint.h>

/*
 * ==== The constant buffer layout the compiler and the dispatch agree on (zedBSD's) ====
 *
 * Constant buffer 0 holds the push constants from byte 0, and from
 * NVRTX_SM75_CB_STORAGE one 16-byte entry per storage buffer the shader
 * uses: the buffer's GPU address (low word, high word) and its size in
 * bytes.  Constant buffers 1 to 7 are the first seven uniform buffers, in
 * the order the shader's uniform list names them; a uniform buffer past
 * them is read from memory through an entry of the storage table as a
 * storage buffer is (the compute launch has eight constant buffers).  Each
 * stage of a graphics pipeline has its own constant buffer 0.
 */
#define NVRTX_SM75_CB_ROOT		0U
#define NVRTX_SM75_CB_PUSH		0x000U
#define NVRTX_SM75_CB_PUSH_BYTES	0x100U
#define NVRTX_SM75_CB_STORAGE		0x200U
#define NVRTX_SM75_CB_STORAGE_ENTRY	16U
#define NVRTX_SM75_CB_STORAGE_SIZE	8U
#define NVRTX_SM75_CB_STORAGE_MAX	32U
#define NVRTX_SM75_CB_UNIFORM_FIRST	1U
#define NVRTX_SM75_CB_UNIFORM_MAX	7U

/*
 * The texture handles: from NVRTX_SM75_CB_TEXTURES of constant buffer 0, one
 * word per sampled image the shader uses, in the order of its uniform list
 * (the dispatch writes the texture header index in bits 19:0 and the
 * sampler's in 31:20).  The placement in constant buffer 0 is zedBSD's own.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak_nir_lower_tex.c,
 * lower_tex (lines 156-160), and
 * src/nouveau/vulkan/nvk_descriptor_types.h,
 * NVK_IMAGE_DESCRIPTOR_IMAGE_INDEX_MASK,
 * NVK_IMAGE_DESCRIPTOR_SAMPLER_INDEX_MASK and struct
 * nvk_sampled_image_descriptor (lines 14-22): the handle's bits, header
 * 19:0 and sampler 31:20.
 */
#define NVRTX_SM75_CB_TEXTURES		0x400U
#define NVRTX_SM75_CB_TEXTURES_MAX	32U

/*
 * A draw's first instance, which a vertex program adds to the instance
 * number the hardware gives (it counts from 0) for gl_InstanceIndex: the
 * word at NVRTX_SM75_CB_BASE_INSTANCE of constant buffer 0, after the
 * texture handles.  The placement is zedBSD's own.
 *
 * Procedure referenced: Mesa 25.3.6
 * src/nouveau/vulkan/nvk_nir_lower_descriptors.c (load_base_instance from
 * the root table's draw.base_instance) and src/nouveau/compiler/nak_nir.c
 * (load_instance_id, the attribute's word alone).
 */
#define NVRTX_SM75_CB_BASE_INSTANCE	0x480U

/*
 * The (set, binding) of one buffer or image the shader uses, the element of
 * the binding's array of descriptors (0 for a binding that is not one), and
 * for a uniform buffer the bytes it reads (from 0) and whether it is one.
 */
struct nvrtx_sm75_binding {
	uint32_t set;
	uint32_t binding;
	uint32_t element;
	uint32_t size;
	uint32_t uniform;
};

/*
 * One compiled compute program.
 *
 * Made by nvrtx_sm75_compile() and owned by the caller, who copies the code
 * into GPU memory and frees it with nvrtx_sm75_binary_free().
 *
 * storage[k] is the buffer whose address and size the dispatch writes at
 * NVRTX_SM75_CB_STORAGE + 16 * k of constant buffer 0 (a uniform buffer past
 * the seventh among them, its uniform set); uniforms[j] is the buffer the
 * dispatch binds as constant buffer 1 + j.
 */
struct nvrtx_sm75_binary {
	/* The code: 128-bit instructions, four words each, padded to a whole 128-byte line. */
	uint32_t *code;
	uint32_t code_words;

	/* The shader model the code is for (NVRTX_SM_*). */
	uint32_t shader_model;

	/* The general registers the program needs, the hardware's two included (the QMD's register count). */
	uint32_t gpr_count;

	/* The workgroup's size, its shared memory in bytes, and the workgroup barriers it uses (the QMD's barrier count). */
	uint32_t local_size[3];
	uint32_t shared_bytes;
	uint32_t barrier_count;

	/* The push constant bytes the program reads (at offset 0 of constant buffer 0). */
	uint32_t push_bytes;

	/* The storage buffers and the uniform buffers, as above. */
	struct nvrtx_sm75_binding storage[NVRTX_SM75_CB_STORAGE_MAX];
	uint32_t storage_count;
	struct nvrtx_sm75_binding uniforms[NVRTX_SM75_CB_UNIFORM_MAX];
	uint32_t uniform_count;

	/* The sampled images: the k-th one's texture handle is the word at NVRTX_SM75_CB_TEXTURES + 4 * k of constant buffer 0. */
	struct nvrtx_sm75_binding textures[NVRTX_SM75_CB_TEXTURES_MAX];
	uint32_t texture_count;

	/*
	 * A vertex or fragment program's shader program header (version 4, 128
	 * bytes), which the program's address points at, the code following
	 * it; nonzero graphics says it is one (a compute program has none).
	 */
	uint32_t graphics;
	uint32_t sph[NVRTX_SM75_SPH_WORDS];

	/* A geometry program's input primitive's vertices (1, 2, 3, 4 or 6; 0 for another program). */
	uint32_t vertices_in;
};

/*
 * Why a program was refused: the IR instruction (or SPIR-V word offset for
 * a refusal of the frontend) and an immutable reason.
 */
struct nvrtx_sm75_diagnostic {
	uint32_t instruction;
	const char *reason;
};

/*
 * What the compiler takes beyond the frontend's common profile: a pixel
 * program's depth and sample mask outputs and its covered samples' input,
 * arrays of combined image samplers and of buffers (each element a binding
 * of the binary's lists), the level of detail query, a geometry
 * program's invocations and point size output, and the size of a uniform
 * texel buffer.
 */
#define NVRTX_SM75_SPIRV_FEATURES	(DRV_GPU_SPIRV_FEATURE_SAMPLE_MASK | DRV_GPU_SPIRV_FEATURE_FRAG_DEPTH | \
					 DRV_GPU_SPIRV_FEATURE_SAMPLER_ARRAYS | DRV_GPU_SPIRV_FEATURE_BUFFER_ARRAYS | \
					 DRV_GPU_SPIRV_FEATURE_QUERY_LOD | DRV_GPU_SPIRV_FEATURE_GEOMETRY_INVOCATIONS | \
					 DRV_GPU_SPIRV_FEATURE_GEOMETRY_POINT_SIZE | DRV_GPU_SPIRV_FEATURE_TEXEL_BUFFER_SIZE)

/* Compiles a SPIR-V compute module: 0 with the program, EINVAL or ENOTSUP with a reason, or ENOMEM. */
int nvrtx_sm75_compile(const uint32_t *words, size_t word_count, struct nvrtx_sm75_binary **binary, struct nvrtx_sm75_diagnostic *diagnostic);

/* Compiles a SPIR-V module's stage (compute, vertex or fragment), as nvrtx_sm75_compile(). */
int nvrtx_sm75_compile_stage(const uint32_t *words, size_t word_count, enum drv_gpu_shader_stage stage, struct nvrtx_sm75_binary **binary, struct nvrtx_sm75_diagnostic *diagnostic);

/* Compiles a parsed compute program (the IR is only read). */
int nvrtx_sm75_compile_ir(const struct drv_gpu_shader_ir *ir, struct nvrtx_sm75_binary **binary, struct nvrtx_sm75_diagnostic *diagnostic);

/* Compiles a SPIR-V module's stage for a shader model (NVRTX_SM_TURING, _AMPERE or _ADA); ENOTSUP for another. */
int nvrtx_sm75_compile_stage_sm(const uint32_t *words, size_t word_count, enum drv_gpu_shader_stage stage, uint32_t sm, struct nvrtx_sm75_binary **binary, struct nvrtx_sm75_diagnostic *diagnostic);

/* Compiles a parsed program for a shader model, as nvrtx_sm75_compile_stage_sm(). */
int nvrtx_sm75_compile_ir_sm(const struct drv_gpu_shader_ir *ir, uint32_t sm, struct nvrtx_sm75_binary **binary, struct nvrtx_sm75_diagnostic *diagnostic);

/* Frees a compiled program. */
void nvrtx_sm75_binary_free(struct nvrtx_sm75_binary *binary);

#endif
