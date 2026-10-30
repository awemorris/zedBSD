/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PowerVR shader compiler: from a SPIR-V module's compute, vertex or
 * fragment entry point to a shading cluster program and the description
 * the driver needs to start it.
 *
 * The driver sees only this description.  The program reads every
 * resource it uses from shared registers the driver's data sequencer
 * program fills before the task starts, in the order of the request
 * table; vertex inputs arrive in vertex input registers, fragment inputs
 * in coefficient registers, and a fragment's colours are left as floats
 * in temporaries the driver's own ending reads.  The same code runs on
 * the build host.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_SHADER_H
#define KERN_DRIVERS_GPU_POWERVR_SHADER_H

#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/powervr/compiler/shader-ir.h"

/* The stages the compiler takes. */
enum powervr_shader_stage {
	POWERVR_SHADER_COMPUTE = 0,
	POWERVR_SHADER_VERTEX,
	POWERVR_SHADER_FRAGMENT
};

/*
 * What one shared register request asks the driver to load.
 *
 * A descriptor request names a part of the binding's descriptor: the
 * buffer's address words, the image state words or the sampler state
 * words, so the driver keeps the descriptor's layout to itself.
 */
enum powervr_shader_request_kind {
	POWERVR_SHADER_REQUEST_SET_BASE = 0,
	POWERVR_SHADER_REQUEST_DESCRIPTOR,
	POWERVR_SHADER_REQUEST_PUSH_CONSTANTS,
	POWERVR_SHADER_REQUEST_LITERAL,
	POWERVR_SHADER_REQUEST_WORKGROUP_ID,
	POWERVR_SHADER_REQUEST_NUM_WORKGROUPS,
	POWERVR_SHADER_REQUEST_BLEND_CONSTANTS,

	/* Words the program's shared program computes: the data sequencer loads nothing for them. */
	POWERVR_SHADER_REQUEST_COMPUTED
};

/*
 * Whether a compute program reads its workgroup id from coefficients 0
 * to 2, which the one-kernel executor's data sequencer program writes
 * for each workgroup (powervr_compute_workgroup_program), instead of the
 * request's shared registers or inlined words.  Build with
 * -DPOWERVR_SHADER_WORKGROUP_COEFFICIENTS=0 for the executor of one
 * kernel per workgroup.
 */
#ifndef POWERVR_SHADER_WORKGROUP_COEFFICIENTS
#define POWERVR_SHADER_WORKGROUP_COEFFICIENTS	1
#endif

/* The parts of a descriptor a request may name. */
enum powervr_shader_descriptor_part {
	POWERVR_SHADER_PART_BUFFER_ADDRESS = 0,
	POWERVR_SHADER_PART_IMAGE_STATE,
	POWERVR_SHADER_PART_SAMPLER_STATE
};

/* The most entries of each table. */
#define POWERVR_SHADER_REQUESTS		32U
#define POWERVR_SHADER_INPUTS		16U
#define POWERVR_SHADER_VARYINGS		16U
#define POWERVR_SHADER_OUTPUTS		8U

/* A register a program does not use. */
#define POWERVR_SHADER_NO_REG		0xffffffffU

/* The words of a texture's image state and sampler state. */
#define POWERVR_SHADER_STATE_WORDS	4U

/*
 * One shared register request.
 *
 * Shared is the first shared register the words land in, and words how
 * many; the requests are packed from shared register 0 in table order.
 * Offset is the first byte of a push constant range.
 */
struct powervr_shader_request {
	uint8_t kind;
	uint8_t part;
	uint8_t words;
	uint16_t shared;
	uint32_t set;
	uint32_t binding;
	uint32_t array_index;
	uint32_t offset;
	uint32_t literal;
};

/* One vertex input: its location, its words, and its first vertex input register. */
struct powervr_shader_input {
	uint32_t location;
	uint32_t components;
	uint32_t reg;
};

/*
 * One varying.
 *
 * For a vertex shader, reg is the first vertex output word; for a
 * fragment shader, the first coefficient set (four coefficient
 * registers per word, A, B, C and a pad).
 */
struct powervr_shader_varying {
	uint32_t location;
	uint32_t components;
	uint32_t flat;
	uint32_t reg;
};

/*
 * One fragment colour output: its location, its attachment's format,
 * and where the program leaves it: with a key, the pixel output
 * register holding the packed colour; without one, the first of four
 * float temporaries.
 */
struct powervr_shader_output {
	uint32_t location;
	uint32_t format;
	uint32_t reg;
};

/*
 * One colour attachment's blend: the Vulkan factors and operations of
 * the colour and the alpha, and the channels written (bit 0 red to bit 3
 * alpha).  Enable 0 writes the shader's colour as it is.
 */
struct powervr_shader_blend {
	uint8_t enable;
	uint8_t source_color;
	uint8_t destination_color;
	uint8_t color_operation;
	uint8_t source_alpha;
	uint8_t destination_alpha;
	uint8_t alpha_operation;
	uint8_t write_mask;
};

/*
 * What the pipeline tells the compiler beyond the SPIR-V: each vertex
 * input location's format (0 for words as the shader reads them), each
 * colour location's attachment format (0 for none, the output dropped),
 * its blend, and the sample count.  A compile without a key leaves
 * fragment colours as floats in temporaries.
 *
 * Forms (POWERVR_SHADER_FORM_*) turns on fragment program forms the
 * driver takes: the shared program, which moves the work only
 * push constants, descriptors and constants decide into one task a draw
 * that the data sequencer's shared register program issues after its
 * loads, writing the results to shared registers the pixels read (the
 * driver starts it: powervr_shader_program.shared_length); a sample
 * whose coordinates are a varying iterated by the sample itself; and
 * the colour written to an 8-bit attachment, opaque or blended
 * over, added, subtracted or multiplied, by one sum-of-products
 * operation in place of the unpack, the blend's arithmetic and the pack;
 * with the raw form besides, the blended colour handed to that operation
 * unclamped, which it clamps itself (seen on the board); and the
 * iterate and sample that waits for its own data (its scheduling
 * control) in place of a wait after it.  Bit 0x2 (the pack written into
 * the pixel output, which lost the device on the board) is no form.
 */
#define POWERVR_SHADER_FORM_SHARED_PROGRAM	0x1U
#define POWERVR_SHADER_FORM_ITERATE_SAMPLE	0x4U
#define POWERVR_SHADER_FORM_SUM_OF_PRODUCTS	0x8U
#define POWERVR_SHADER_FORM_SUM_RAW		0x10U
#define POWERVR_SHADER_FORM_SAMPLE_WAITS	0x20U

struct powervr_shader_key {
	uint32_t attribute_formats[POWERVR_SHADER_INPUTS];
	uint32_t output_formats[POWERVR_SHADER_OUTPUTS];
	struct powervr_shader_blend blend[POWERVR_SHADER_OUTPUTS];
	uint32_t samples;
	uint32_t forms;
};

/*
 * What the driver needs to start a compiled program.
 *
 * Temps, shareds, vertex_inputs and coefficients count registers.  A
 * vertex shader writes its position to vertex output words 0 to 3 and
 * its varyings after them; vertex_outputs counts them all.  A fragment
 * shader with a perspective-correct varying reads the w coefficients at
 * w_coefficient.  A vertex shader reading its vertex index or instance
 * index wants the driver's data sequencer program to write it to the
 * vertex input register vertex_index_reg or instance_index_reg, after
 * the attributes' registers; POWERVR_SHADER_NO_REG when not read.  The
 * vertex index includes the draw's first vertex or vertex offset, the
 * instance index the first instance.  Discards is 1 for a fragment
 * program that may discard its fragment (it gives the depth and
 * visibility unit its feedback, so its draws pass punch-through).
 *
 * A fragment program with a shared program (shared_length not 0) has
 * it in the same code at shared_offset (16-byte aligned, after the
 * program; length counts both): the driver issues it from the shared
 * register program, with shared_temps temporaries, after the loads.
 * It fills the words of the program's computed request.
 */
struct powervr_shader_program {
	uint32_t stage;
	size_t length;
	size_t shared_offset;
	size_t shared_length;
	uint32_t shared_temps;
	uint32_t temps;
	uint32_t shareds;
	uint32_t vertex_inputs;
	uint32_t vertex_outputs;
	uint32_t coefficients;
	uint32_t w_coefficient;
	uint32_t uses_w;
	uint32_t vertex_index_reg;
	uint32_t instance_index_reg;
	uint32_t discards;
	uint32_t workgroup[3];
	struct powervr_shader_request requests[POWERVR_SHADER_REQUESTS];
	uint32_t request_count;
	struct powervr_shader_input inputs[POWERVR_SHADER_INPUTS];
	uint32_t input_count;
	struct powervr_shader_varying varyings[POWERVR_SHADER_VARYINGS];
	uint32_t varying_count;
	struct powervr_shader_output outputs[POWERVR_SHADER_OUTPUTS];
	uint32_t output_count;
};

/*
 * The compiler's working memory, given by the caller so the compiler
 * itself never allocates.  Its contents mean nothing between calls.
 */
struct powervr_shader_workspace;

size_t powervr_shader_workspace_size(void);
int powervr_shader_read(const uint32_t *words, size_t count, uint32_t stage, const struct powervr_shader_key *key, struct powervr_shader_workspace *workspace, struct powervr_shader_program *program, struct powervr_ir_shader **shader);
int powervr_shader_compile(const uint32_t *words, size_t count, uint32_t stage, const struct powervr_shader_key *key, struct powervr_shader_workspace *workspace, struct powervr_shader_program *program, uint8_t *code, size_t room);
int powervr_shader_compile_inline(const uint32_t *words, size_t count, uint32_t stage, const struct powervr_shader_key *key, const uint32_t *shared_values, uint32_t value_count, struct powervr_shader_workspace *workspace, struct powervr_shader_program *program, uint8_t *code, size_t room);

#endif /* KERN_DRIVERS_GPU_POWERVR_SHADER_H */
