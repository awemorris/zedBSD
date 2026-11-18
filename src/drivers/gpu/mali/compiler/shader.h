/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Mali Valhall (v10, Mali-G310) shader compiler: from a SPIR-V
 * module's compute, vertex or fragment entry point, through the shared
 * scalar IR, to Valhall programs and the description the driver needs to
 * run them.
 *
 * The shader ABI
 * --------------
 *
 * Fast access uniforms (FAU).  A program reads its uniforms from the FAU
 * words the driver points the stage at (the FAU pointer and its count of
 * 64-bit entries).  The program's words are, in order:
 *
 *   - words [0, push_bytes / 4): the push constants, word w holding the
 *     push constant bytes [4w, 4w + 4);
 *   - a vertex program: from the first even word after them, the six
 *     32-bit floats of the viewport transform at viewport_fau_word: the
 *     scale x, y, z, then the offset x, y, z (the hardware takes the
 *     position in screen space: the program divides by w, scales and
 *     offsets x, y and z, and writes 1 / w as the fourth component);
 *   - then, from the first even word on, one 64-bit entry for each buffer
 *     of the program's buffer table (struct mali_shader_buffer): the
 *     buffer's GPU address, low word first, at buffer.fau_word.  A uniform
 *     buffer's address is that of the byte its binding's offset names; a
 *     storage buffer's likewise.
 *
 *   - then the words of the sizes the program queries: for a storage
 *     buffer whose length it reads (OpArrayLength), one word with the
 *     bytes of the buffer's range at buffer.size_fau_word; for a texture
 *     or a storage image whose size it queries, four words at
 *     texture.size_fau_word (image.size_fau_word): the width, the height
 *     and the depth (or the layers of an array, the cubes of a cube
 *     array) of level 0, and the number of levels.
 *
 * fau_words counts them all and is even; the driver gives the stage
 * fau_words / 2 entries.  A size word the program does not read is
 * MALI_SHADER_NO_FAU_WORD.
 *
 * Resource tables.  Textures, samplers and vertex attributes are read
 * through the resource table the stage's resource table pointer names.
 * The compiler numbers what it reads; the driver writes the descriptors:
 *
 *   - table MALI_SHADER_TABLE_TEXTURES, entry k: the texture descriptor of
 *     the program's texture k (struct mali_shader_texture);
 *   - table MALI_SHADER_TABLE_SAMPLERS, entry k: the sampler descriptor of
 *     the same combined image sampler;
 *   - table MALI_SHADER_TABLE_ATTRIBUTES, entry L: a vertex program's
 *     attribute descriptor for input location L (the format, the offset and
 *     the buffer it reads, which the hardware converts to 32-bit floats or
 *     integers as the location's type says);
 *   - table MALI_SHADER_TABLE_VERTEX_BUFFERS, entry B: the buffer
 *     descriptor of Vulkan vertex binding B, which the attribute
 *     descriptors name;
 *   - table MALI_SHADER_TABLE_IMAGES, entry k: the texture descriptor of
 *     the program's storage image k (struct mali_shader_image), written
 *     for storage (one level, no swizzle) as a 2D image of the view's
 *     layers (a 3D image's slices), which the program loads texels from
 *     (LD_TEX_IMM) and finds texel addresses in for its stores and
 *     atomics (LEA_TEX_IMM): the texel's x, y and layer.
 *
 * A texture's descriptor has the dimension of the view the program
 * samples: 2D (a 2D array's layers its array size), 3D, or cube (its
 * array size the cubes, six layers each, face f of cube c being layer
 * 6 c + f in the order +X, -X, +Y, -Y, +Z, -Z).  Its sampler descriptor
 * carries the compare function of a sampler the program compares with
 * (a shadow sampler's depth reference is the program's).
 *
 * Vertex programs.  A vertex shader compiles to the two programs of an
 * index-driven vertex shading draw in one code buffer: the position
 * program at offset 0, which writes the position (and the point size),
 * and the varying program at varying_offset, which writes the generic
 * varyings.  Both find their output through the hardware's vertex output
 * buffer (resource table 61, entry 0) at the vertex's internal index the
 * hardware preloads in r59.  The position is four 32-bit floats at byte 0
 * of the position record; the point size a 16-bit float at byte 0 of the
 * attribute record.  Generic varying location L is four 32-bit words at
 * byte 16 L of the vertex's varying record: varying_bytes is the record's
 * size, which the draw gives the hardware (and the fragment program reads
 * at the same offsets, so the two stages agree by location alone).
 * A vertex program reads its vertex index and instance index from the
 * preloaded r60 and r61.
 *
 * Fragment programs.  A fragment program reads varying location L at
 * byte 16 L of the varying record, interpolated at the pixel's centre (or
 * flat, from the provoking vertex; a NoPerspective varying the vertex
 * program wrote multiplied by w, and the fragment program multiplies by
 * the fragment's 1 / w).  It ends by testing its coverage
 * (ATEST, with the hardware's alpha test datum, using colour 0's alpha)
 * and blending each colour location L it writes, as four 32-bit floats,
 * into render target L with blend descriptor L (the fixed function
 * blend); the last blend ends the program.
 *
 * A fragment program that writes gl_SampleMask ANDs it into the coverage
 * the hardware preloads before the test, and gl_SampleMaskIn reads the
 * samples the rasterizer found covered (bits 15:0 of the preloaded r61).
 * A fragment program that writes gl_FragDepth emits it (ZS_EMIT, a
 * 32-bit float) after the coverage test and before its blends:
 * writes_depth says so, and the draw then takes the depth from the
 * shader (the depth/stencil descriptor's depth source) and tests and
 * updates depth late.  A fragment program that writes its stencil
 * reference (gl_FragStencilRefARB, an integer whose low 8 bits count)
 * emits it with the same ZS_EMIT: writes_stencil says so, and the draw
 * then takes the stencil reference from the shader.  gl_SampleID reads
 * the sample the invocation runs for (bits 20:16 of the preloaded r61):
 * reads_sample_id says so.
 *
 * Compute programs.  A compute program reads its local invocation id from
 * the preloaded r55 (x in bits 15:0, y in 31:16) and r56 (z in 15:0), and
 * its workgroup id from r57, r58 and r59.  gl_NumWorkGroups is a storage
 * buffer of the buffer table with set DRV_GPU_IR_SYSTEM_SET, binding 0,
 * holding the three group counts.  Workgroup shared memory is addressed
 * from the workgroup local storage pointer the driver sets.
 *
 * Uniform buffers.  A program reads a uniform buffer word by word from
 * its FAU address.  Compiled with MALI_SHADER_OPTION_VECTOR_UBO it reads
 * a run of consecutive words with one load of two to four words, whose
 * byte offset is aligned to the load's size rounded up to a power of two
 * (8 for two words, 16 for three or four), so the buffer's address must
 * then be aligned to 16 bytes (a binding's offset is a multiple of the
 * device's uniform buffer offset alignment).
 *
 * Thread local storage.  A program whose values do not fit its registers
 * keeps some of them in memory of its own thread: tls_bytes is the bytes
 * one thread uses (a power of two, at least 16; zero for none), which
 * the driver gives the stage through the thread storage descriptor (its
 * TLS size, the log2 of tls_bytes / 16, and the base of storage enough
 * for every thread the GPU's cores run at once).  The program reaches it
 * through the thread local storage pointer the hardware makes from the
 * descriptor.
 *
 * preload says which of r48..r63 the program reads as the hardware
 * delivers them (bit i for r(48 + i)), and work_registers how many
 * registers it uses (counted from r0, the preloaded ones included): the
 * driver puts both in the shader program descriptor.
 *
 * The same code runs on the build host.
 */

#ifndef KERN_DRIVERS_GPU_MALI_COMPILER_SHADER_H
#define KERN_DRIVERS_GPU_MALI_COMPILER_SHADER_H

#include "drivers/gpu/compiler/ir.h"
#include "drivers/gpu/compiler/spirv.h"

#include <stddef.h>
#include <stdint.h>

/* The stages the compiler takes. */
enum mali_shader_stage {
	MALI_SHADER_COMPUTE = 0,
	MALI_SHADER_VERTEX,
	MALI_SHADER_FRAGMENT
};

/*
 * What the compiler asks the SPIR-V parse to lower beyond the shared
 * profile: storage images, a compute shader's images, a fragment
 * shader's gl_FragDepth, its gl_SampleMask and gl_SampleMaskIn, its
 * gl_SampleID and its stencil reference (gl_FragStencilRefARB) and a
 * module's function calls (inlined).  NoContraction needs nothing more:
 * every product is its own FMA with a zero addend, never fused into a sum.
 */
#define MALI_SHADER_SPIRV_FEATURES	(DRV_GPU_SPIRV_FEATURE_COMPUTE_IMAGES | DRV_GPU_SPIRV_FEATURE_STORAGE_IMAGE | \
					 DRV_GPU_SPIRV_FEATURE_FRAG_DEPTH | DRV_GPU_SPIRV_FEATURE_SAMPLE_MASK | \
					 DRV_GPU_SPIRV_FEATURE_SAMPLE_ID | DRV_GPU_SPIRV_FEATURE_STENCIL_EXPORT | \
					 DRV_GPU_SPIRV_FEATURE_INLINE_CALLS)

/* The resource tables a program reads (see the shader ABI above). */
#define MALI_SHADER_TABLE_TEXTURES		0U
#define MALI_SHADER_TABLE_SAMPLERS		1U
#define MALI_SHADER_TABLE_ATTRIBUTES		2U
#define MALI_SHADER_TABLE_VERTEX_BUFFERS	3U
#define MALI_SHADER_TABLE_IMAGES		4U

/* The hardware's vertex output buffer table of an index-driven draw. */
#define MALI_SHADER_TABLE_VERTEX_OUTPUT		61U

/* The kinds of buffer a program binds. */
#define MALI_SHADER_BUFFER_UNIFORM		1U
#define MALI_SHADER_BUFFER_STORAGE		2U

/* The most entries of each table. */
#define MALI_SHADER_BUFFERS			16U
#define MALI_SHADER_TEXTURES			16U
#define MALI_SHADER_LOCATIONS			16U
#define MALI_SHADER_COLORS			8U
#define MALI_SHADER_IMAGES			8U

/* No FAU word: the program does not read that size. */
#define MALI_SHADER_NO_FAU_WORD			0xFFFFFFFFU

/* The bytes of one generic varying location in the varying record. */
#define MALI_SHADER_VARYING_STRIDE		16U

/* The byte alignment of a program's code, and of the varying program. */
#define MALI_SHADER_CODE_ALIGNMENT		128U

/*
 * One buffer the program binds: its (set, binding), its kind, the FAU word
 * its 64-bit address goes in, for a uniform buffer the bytes the program
 * reads of it (from the binding's offset), and for a storage buffer the
 * FAU word of its range's bytes (or MALI_SHADER_NO_FAU_WORD).
 */
struct mali_shader_buffer {
	uint32_t set;
	uint32_t binding;
	uint32_t kind;
	uint32_t fau_word;
	uint32_t size;
	uint32_t size_fau_word;
};

/*
 * One combined image sampler the program samples: its (set, binding),
 * the entry of the texture and sampler tables its descriptors go in, and
 * the first FAU word of its size (or MALI_SHADER_NO_FAU_WORD).
 */
struct mali_shader_texture {
	uint32_t set;
	uint32_t binding;
	uint32_t index;
	uint32_t size_fau_word;
};

/*
 * One storage image the program loads or stores texels of: its (set,
 * binding), the entry of the image table its descriptor goes in, the
 * VkFormat the shader declares it with, and the first FAU word of its
 * size (or MALI_SHADER_NO_FAU_WORD).
 */
struct mali_shader_image {
	uint32_t set;
	uint32_t binding;
	uint32_t index;
	uint32_t format;
	uint32_t size_fau_word;
};

/*
 * One compiled program.
 *
 * Made by mali_shader_compile() and owned by the caller, who copies the
 * code into GPU memory (aligned to MALI_SHADER_CODE_ALIGNMENT) and frees
 * the program with mali_shader_program_free().  The code ends with the
 * padding the instruction prefetch may read.
 */
struct mali_shader_program {
	/* The stage the program runs in (enum mali_shader_stage). */
	uint32_t stage;

	/* The code, and its bytes (every variant and the padding). */
	uint8_t *code;
	size_t length;

	/*
	 * The registers the program uses, counted from r0 (more than 32 needs
	 * the descriptor's 64-register allocation), and the preloaded
	 * registers it reads (bit i for r(48 + i)).  A vertex program's
	 * varying program has its own.
	 */
	uint32_t work_registers;
	uint32_t preload;

	/*
	 * A vertex program: the varying program's offset in the code (0 when
	 * the shader writes no generic varying, the draw then runs only the
	 * position program), its registers and its preloads.
	 */
	size_t varying_offset;
	uint32_t varying_work_registers;
	uint32_t varying_preload;

	/*
	 * The FAU words the driver writes, an even number, the push constant
	 * bytes among them, and a vertex program's first viewport word.
	 */
	uint32_t fau_words;
	uint32_t push_bytes;
	uint32_t viewport_fau_word;

	/* The buffers the program binds, whose addresses the FAU words carry. */
	struct mali_shader_buffer buffers[MALI_SHADER_BUFFERS];
	uint32_t buffer_count;

	/* The combined image samplers the program samples, by texture table entry. */
	struct mali_shader_texture textures[MALI_SHADER_TEXTURES];
	uint32_t texture_count;

	/* The storage images the program reads and writes, by image table entry. */
	struct mali_shader_image images[MALI_SHADER_IMAGES];
	uint32_t image_count;

	/*
	 * The thread local storage bytes of one thread (a power of two, at
	 * least 16), zero when the program keeps everything in registers.
	 * A vertex program's two programs share it.
	 */
	uint32_t tls_bytes;

	/*
	 * A vertex program: the input locations it reads (bit L), those of
	 * them read as integers rather than floats, the generic varying
	 * locations it writes (bit L), the varying record's bytes, and
	 * whether it writes the point size.
	 */
	uint32_t attribute_mask;
	uint32_t integer_attribute_mask;
	uint32_t varying_mask;
	uint32_t varying_bytes;
	uint32_t writes_point_size;

	/*
	 * A vertex program: whether it reads its vertex index and its instance
	 * index.
	 */
	uint32_t reads_vertex_index;
	uint32_t reads_instance_index;

	/*
	 * A fragment program: the varying locations it reads (bit L) and
	 * those read flat, the colour locations it writes (bit L, blended to
	 * render target L), whether it discards, and whether it reads the
	 * fragment coordinate.
	 */
	uint32_t input_mask;
	uint32_t flat_mask;
	uint32_t color_mask;
	uint32_t discards;
	uint32_t reads_frag_coord;

	/*
	 * A compute program: its workgroup size, the shared memory bytes of a
	 * workgroup, and whether it waits at a workgroup barrier.
	 */
	uint32_t local_size[3];
	uint32_t shared_bytes;
	uint32_t uses_barrier;

	/*
	 * A fragment program: whether it writes the depth (gl_FragDepth),
	 * whether it changes its coverage (gl_SampleMask), and whether it
	 * reads the samples its primitive covers (gl_SampleMaskIn).
	 */
	uint32_t writes_depth;
	uint32_t writes_sample_mask;
	uint32_t reads_sample_mask;

	/*
	 * A fragment program: whether it writes its stencil reference
	 * (gl_FragStencilRefARB), and whether it reads the number of the
	 * sample it runs for (gl_SampleID).
	 */
	uint32_t writes_stencil;
	uint32_t reads_sample_id;

	/*
	 * Whether the program writes memory other threads or later work can
	 * see: a store to a storage buffer or a storage image, or an atomic
	 * operation on either (not shared memory).
	 */
	uint32_t writes_memory;
};

/*
 * What a pipeline asks of a compile beyond the module: flags, a set of
 * MALI_SHADER_OPTION_* (zero, or no record, for none).
 */
struct mali_shader_options {
	uint32_t flags;
};

/*
 * Reads a run of consecutive uniform buffer words with one load where
 * the run's offset is aligned for it (see "Uniform buffers" above);
 * without it every word is a load of its own.
 */
#define MALI_SHADER_OPTION_VECTOR_UBO	0x1U

/*
 * Why a program was refused: the IR instruction (or the SPIR-V word
 * offset, for a refusal of the frontend) and an immutable reason.
 */
struct mali_shader_diagnostic {
	uint32_t instruction;
	const char *reason;
};

/*
 * Compiles a SPIR-V module's stage with the options (NULL for none): 0
 * with the program, EINVAL or ENOTSUP with a reason, ENOSPC when the
 * registers run out, or ENOMEM.
 */
int mali_shader_compile(const uint32_t *words, size_t word_count, uint32_t stage, const struct mali_shader_options *options, struct mali_shader_program **program, struct mali_shader_diagnostic *diagnostic);

/* Compiles a parsed program (the IR is only read), as mali_shader_compile(). */
int mali_shader_compile_ir(const struct drv_gpu_shader_ir *ir, const struct mali_shader_options *options, struct mali_shader_program **program, struct mali_shader_diagnostic *diagnostic);

/* Frees a compiled program. */
void mali_shader_program_free(struct mali_shader_program *program);

#endif /* KERN_DRIVERS_GPU_MALI_COMPILER_SHADER_H */
