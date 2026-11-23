/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GHCI shader compiler: SPIR-V (through the shared scalar IR) to GSI1
 * programs, the standard shader ISA every GHCI GPU with a shader unit
 * runs.
 *
 * A program is GSI1 code alone; the pipeline state carries its register
 * count, shared memory, workgroup size, varying count and fragment flags.
 * It reaches its resources the way gsi1-abi.h says, and the compile tells
 * the executor what to put where: the texture and sampler slots of sets 0
 * and 1, the entries of the resource table (set 2), the dynamic offsets
 * and push constants of the constant bank, and the scratch memory.  The
 * vertex and fragment programs of a pipeline share those tables: the
 * second one is compiled with the first one's tables and extends them.
 */

#ifndef DRIVERS_GPU_GHCI_COMPILER_GSI1_COMPILE_H
#define DRIVERS_GPU_GHCI_COMPILER_GSI1_COMPILE_H

#include "drivers/gpu/compiler/ir.h"

#include <stddef.h>
#include <stdint.h>

/* The user locations a vertex program reads and writes and a fragment program reads. */
#define GHCI_GSI1_LOCATIONS		16U

/* The input attachments a fragment program may read (maxPerStageDescriptorInputAttachments). */
#define GHCI_GSI1_INPUT_ATTACHMENTS	8U

/*
 * The texture and sampler slots of a pipeline (the entries of set 0's
 * texture and sampler tables a texture instruction's immediate index
 * reaches), the resource table's entries, and the dynamic offsets.
 */
#define GHCI_GSI1_TEXTURE_SLOTS		64U
#define GHCI_GSI1_SAMPLER_SLOTS		64U
#define GHCI_GSI1_RESOURCES		128U
#define GHCI_GSI1_DYNAMIC_OFFSETS	32U

/* The primitive a fragment program is compiled for: its inputs differ (a point's are vertex 0's, a line has no vertex 2). */
#define GHCI_GSI1_PRIMITIVE_TRIANGLE	0U
#define GHCI_GSI1_PRIMITIVE_LINE	1U
#define GHCI_GSI1_PRIMITIVE_POINT	2U

/* What the executor writes into a texture slot: an image view, or a uniform texel buffer's view as gsi1-abi.h lays it out. */
#define GHCI_GSI1_TEXTURE_IMAGE		1U
#define GHCI_GSI1_TEXTURE_TEXEL_BUFFER	2U

/* What the executor writes into a sampler slot: the sampler of a combined image sampler. */
#define GHCI_GSI1_SAMPLER_COMBINED	1U

/*
 * What the executor writes at a resource table entry: a buffer
 * descriptor (one 16-byte unit) of a uniform buffer, a storage buffer or
 * the view of a uniform or storage texel buffer (stride = the texel's
 * bytes, format = the view's), or a texture descriptor (two units) of a
 * storage image's or an input attachment's view.
 */
#define GHCI_GSI1_RESOURCE_UNIFORM_BUFFER	1U
#define GHCI_GSI1_RESOURCE_STORAGE_BUFFER	2U
#define GHCI_GSI1_RESOURCE_UNIFORM_TEXEL_BUFFER	3U
#define GHCI_GSI1_RESOURCE_STORAGE_TEXEL_BUFFER	4U
#define GHCI_GSI1_RESOURCE_STORAGE_IMAGE	5U
#define GHCI_GSI1_RESOURCE_INPUT_ATTACHMENT	6U

/* The fragment program's flags (PSO dw15): it reads the centroid barycentrics, exports the depth, may discard. */
#define GHCI_GSI1_FLAG_CENTROID		(1U << 0)
#define GHCI_GSI1_FLAG_DEPTH_EXPORT	(1U << 1)
#define GHCI_GSI1_FLAG_DISCARD		(1U << 2)

/* The program asks an image its sample count (texsz.s), which needs multisampling; not a PSO flag. */
#define GHCI_GSI1_FLAG_SAMPLE_QUERY	(1U << 3)

/* A fragment program's colour outputs: bit k for attachment k, and the second colour of attachment 0. */
#define GHCI_GSI1_OUTPUT_SECOND_COLOR	(1U << 8)

/* One texture or sampler slot: what fills it (0: free), and the Vulkan binding and array element it comes from. */
struct ghci_gsi1_slot {
	uint32_t kind;
	uint32_t set;
	uint32_t binding;
	uint32_t element;
};

/*
 * One entry of the resource table: what fills it, the Vulkan binding and
 * array element it comes from, and its byte offset from the table's
 * address.
 */
struct ghci_gsi1_resource {
	uint32_t kind;
	uint32_t set;
	uint32_t binding;
	uint32_t element;
	uint32_t offset;
};

/*
 * The tables a pipeline's programs share: the texture and sampler slots
 * in use and the resource table's entries, with the bytes the table
 * takes.  A zeroed record is an empty pipeline.
 */
struct ghci_gsi1_bindings {
	struct ghci_gsi1_slot texture[GHCI_GSI1_TEXTURE_SLOTS];
	uint32_t texture_count;
	struct ghci_gsi1_slot sampler[GHCI_GSI1_SAMPLER_SLOTS];
	uint32_t sampler_count;
	struct ghci_gsi1_resource resource[GHCI_GSI1_RESOURCES];
	uint32_t resource_count;
	uint32_t resource_bytes;
};

/* A dynamic uniform or storage buffer of the pipeline layout. */
struct ghci_gsi1_dynamic {
	uint32_t set;
	uint32_t binding;
};

/*
 * What a pipeline tells a compile beyond the module.
 *
 * A zeroed record is a valid request: 63 registers, one sample per pixel,
 * triangles, each attribute location the vertex program declares bound
 * with its variable's components, no dynamic buffers and an empty
 * pipeline.
 */
struct ghci_gsi1_options {
	/* The registers per lane the program may use: min(63, RESIDENT_WARPS[23:16]); 0 reads as 63. */
	uint32_t registers;

	/* Nonzero when the GPU writes the sample mask in r6 (DEVICE_CAPS[31]). */
	uint32_t sample_mask_input;

	/* A fragment program: the pipeline's samples per pixel (1, 2 or 4; 0 reads as 1). */
	uint32_t samples;

	/* A fragment program: the primitive it is compiled for (GHCI_GSI1_PRIMITIVE_*). */
	uint32_t primitive;

	/* Nonzero when the device was created with robustBufferAccess. */
	uint32_t robust_buffer_access;

	/*
	 * A vertex program: nonzero has_attribute_map gives each location's
	 * bound attribute's components (its format's, 1 to 4; 0 for a
	 * location without an attribute), in place of the variables'; bit L
	 * of attribute_integer says that location L's format is an integer
	 * one (a component it lacks reads 0, and the fourth integer 1, not
	 * 1.0).
	 */
	uint32_t has_attribute_map;
	uint32_t attribute_components[GHCI_GSI1_LOCATIONS];
	uint32_t attribute_integer;

	/*
	 * A fragment program's input attachments, by InputAttachmentIndex:
	 * the VkFormat of the render pass's attachment (0 for none) and its
	 * samples.
	 */
	uint32_t input_attachment_format[GHCI_GSI1_INPUT_ATTACHMENTS];
	uint32_t input_attachment_samples[GHCI_GSI1_INPUT_ATTACHMENTS];

	/*
	 * A compute program of a pipeline made for vkCmdDispatchBase: its
	 * workgroups count from the first one in bank words 72 to 74.
	 */
	uint32_t dispatch_base;

	/*
	 * A graphics program of a multiview subpass: gl_ViewIndex is bank word
	 * 75, which the executor loads before each view.  Otherwise it is 0.
	 */
	uint32_t multiview;

	/* The pipeline layout's dynamic buffers, in the order of its dynamic offsets (bank word 40 + index). */
	uint32_t dynamic_count;
	struct ghci_gsi1_dynamic dynamic[GHCI_GSI1_DYNAMIC_OFFSETS];

	/* The tables the pipeline's other program already took (zero for none). */
	struct ghci_gsi1_bindings bindings;
};

/*
 * One compiled program.
 *
 * Made by ghci_gsi1_compile() and owned by the caller, who copies the
 * code into GPU memory (8-byte aligned), puts the record's numbers into
 * the pipeline state and the executor's tables, and frees the record with
 * ghci_gsi1_program_free().
 */
struct ghci_gsi1_program {
	/* The stage the program runs in. */
	enum drv_gpu_shader_stage stage;

	/* The code, two 32-bit words to an instruction (word0, then word1). */
	uint32_t *words;
	uint32_t word_count;
	uint32_t instruction_count;

	/* used_regs of the PSO (1 to 63). */
	uint32_t used_regs;

	/* A compute program: the shared memory's bytes and the workgroup's size. */
	uint32_t lds_bytes;
	uint32_t local_size[3];

	/* A fragment program's GHCI_GSI1_FLAG_*. */
	uint32_t flags;

	/* The vec4 varyings the program writes (vertex) or reads (fragment): PSO varying_count. */
	uint32_t varying_count;

	/* A vertex program: the attribute registers the fetcher writes (at most 60). */
	uint32_t attribute_registers;

	/* A fragment program: the colour attachments it exports (bit k) and GHCI_GSI1_OUTPUT_SECOND_COLOR. */
	uint32_t color_outputs;

	/* The push-constant bytes it reads, and the dynamic offsets (bit i: bank word 40 + i). */
	uint32_t push_bytes;
	uint32_t dynamic_used;

	/* The scratch memory's bytes per warp (0: none); its address is in the driver's table. */
	uint32_t scratch_bytes;

	/* The pipeline's tables with this program's slots and entries. */
	struct ghci_gsi1_bindings bindings;
};

/*
 * Why a program was refused: the IR instruction (or the SPIR-V word
 * offset, for a refusal of the frontend) and an immutable reason.
 */
struct ghci_gsi1_diagnostic {
	uint32_t instruction;
	const char *reason;
};

/*
 * Compiles a SPIR-V module's stage with the options (NULL for none): 0
 * with the program, EINVAL or ENOTSUP with a reason, or ENOMEM.
 */
int ghci_gsi1_compile(const uint32_t *words, size_t word_count, enum drv_gpu_shader_stage stage, const struct ghci_gsi1_options *options, struct ghci_gsi1_program **program, struct ghci_gsi1_diagnostic *diagnostic);

/* Compiles a parsed program (the IR is only read), as ghci_gsi1_compile(). */
int ghci_gsi1_compile_ir(const struct drv_gpu_shader_ir *ir, const struct ghci_gsi1_options *options, struct ghci_gsi1_program **program, struct ghci_gsi1_diagnostic *diagnostic);

/*
 * Makes the fragment program of a pipeline without a fragment stage: all
 * ones to the sample mask, and the end.  Returns 0 or ENOMEM.
 */
int ghci_gsi1_empty_fragment(struct ghci_gsi1_program **program);

/* Frees a compiled program. */
void ghci_gsi1_program_free(struct ghci_gsi1_program *program);

#endif /* DRIVERS_GPU_GHCI_COMPILER_GSI1_COMPILE_H */
