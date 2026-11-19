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

	/*
	 * The address of the dispatch's shared memory: a slice of
	 * shared_bytes for each workgroup, in the order of the workgroups'
	 * flat ids (x fastest), zero before the dispatch's first workgroup.
	 */
	POWERVR_SHADER_REQUEST_SHARED_MEMORY,

	/* Words the program's shared program computes: the data sequencer loads nothing for them. */
	POWERVR_SHADER_REQUEST_COMPUTED,

	/*
	 * The address of the render's tile buffer binding (0 up), low word
	 * first: the memory holding a tile's pixels of the colour
	 * attachments the pixel output registers have no room for
	 * (powervr_shader_pixel_places).
	 */
	POWERVR_SHADER_REQUEST_TILE_BUFFER
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

/*
 * The instances of one task.  A compute workgroup runs as whole tasks:
 * its local index (0 up) is in vertex input register 0, written by the
 * data sequencer, and a workgroup that is not whole tasks has its
 * program check the index against its size.
 */
#define POWERVR_SHADER_TASK_INSTANCES	32U

/* The parts of a descriptor a request may name. */
enum powervr_shader_descriptor_part {
	POWERVR_SHADER_PART_BUFFER_ADDRESS = 0,
	POWERVR_SHADER_PART_IMAGE_STATE,
	POWERVR_SHADER_PART_SAMPLER_STATE,

	/* The comparison of a sampler that compares (a VkCompareOp, one word; never for one that does not). */
	POWERVR_SHADER_PART_SAMPLER_COMPARE,

	/* The bytes from one array layer of an image's view to the next (one word). */
	POWERVR_SHADER_PART_IMAGE_LAYER_BYTES,

	/* The pack information of an image's view (one word; POWERVR_SHADER_PACK_INFO_UNORM for an unsigned normalized format). */
	POWERVR_SHADER_PART_IMAGE_PACK_INFO,

	/*
	 * The shape of a storage image's view as its lowering reads it
	 * (POWERVR_SHADER_SHAPE_WORDS words: width, height, slices, and the
	 * words from row to row and from slice to slice), asked for by the
	 * binding with DRV_GPU_SPIRV_IMAGE_INFO_BINDING set.
	 */
	POWERVR_SHADER_PART_IMAGE_SHAPE
};

/*
 * The words of a storage image's shape the program reads: the first five
 * of the lowering's eight (the last three are zero).
 */
#define POWERVR_SHADER_SHAPE_WORDS	5U

/*
 * The pack information's bit of an unsigned normalized format.
 *
 * Values referenced:
 * Mesa 26.2.6 src/imagination/common/pvr_iface.h
 * (PVR_PCK_INFO_UNORM_OFFSET 8).
 */
#define POWERVR_SHADER_PACK_INFO_UNORM	0x100U

/* The most entries of each table. */
#define POWERVR_SHADER_REQUESTS		32U
#define POWERVR_SHADER_INPUTS		16U
#define POWERVR_SHADER_VARYINGS		16U
#define POWERVR_SHADER_OUTPUTS		8U

/* A register a program does not use. */
#define POWERVR_SHADER_NO_REG		0xffffffffU

/*
 * The pixel output registers of a pixel, and the most tile buffers a
 * render's colour attachments may take beyond them.
 *
 * Values referenced:
 * Mesa 26.2.6 src/imagination/include/hwdef/rogue_hw_utils.h
 * (rogue_get_max_output_regs_per_pixel: 4 without
 * eight_output_registers) and src/imagination/vulkan/pvr_device.h
 * (PVR_MAX_TILE_BUFFER_COUNT 7);
 * src/imagination/common/device_info/bxe-4-32.h (no
 * eight_output_registers).
 */
#define POWERVR_SHADER_PIXEL_REGISTERS	4U
#define POWERVR_SHADER_TILE_BUFFERS	7U

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
 * One varying (linear: a fragment input iterated without perspective).
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
	uint32_t linear;
};

/*
 * One fragment colour output: its location, its attachment's format,
 * and where the program leaves it: with a key, the first pixel output
 * register holding the packed colour (powervr_shader_pixel_places'
 * register for the attachment), or, when the attachment is in a tile
 * buffer (tile_buffer not POWERVR_SHADER_NO_REG), its first word among
 * the pixel's words of that buffer; without a key, the first of four
 * float temporaries.
 */
struct powervr_shader_output {
	uint32_t location;
	uint32_t format;
	uint32_t reg;
	uint32_t tile_buffer;
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

/*
 * Not a form of the fragment program: a vertex program whose draws are
 * points writes the point size (the shader's gl_PointSize, 1.0 when it
 * writes none) to the vertex output word point_size_reg, after the
 * varyings; without it the point size is dropped.
 */
#define POWERVR_SHADER_FORM_POINT_SIZE		0x40U

/*
 * Which primitives' fragments gl_FrontFacing calls front: those the
 * core sees clockwise, counter-clockwise, or all (points and lines).
 */
#define POWERVR_SHADER_FRONT_CLOCKWISE		0U
#define POWERVR_SHADER_FRONT_COUNTER_CLOCKWISE	1U
#define POWERVR_SHADER_FRONT_ALWAYS		2U

struct powervr_shader_key {
	uint32_t attribute_formats[POWERVR_SHADER_INPUTS];
	uint32_t output_formats[POWERVR_SHADER_OUTPUTS];
	struct powervr_shader_blend blend[POWERVR_SHADER_OUTPUTS];
	uint32_t samples;
	uint32_t forms;
	uint32_t front_face;
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
 * the attributes' registers; POWERVR_SHADER_NO_REG when not read.  A
 * vertex program keyed for points (POWERVR_SHADER_FORM_POINT_SIZE)
 * writes the point size to vertex output word point_size_reg, after its
 * varyings (POWERVR_SHADER_NO_REG otherwise).  A fragment program that
 * reads gl_PointCoord iterates it linearly from the two coefficient sets
 * from point_coord_coefficient, which the driver fills with the point
 * sprite's coordinates (POWERVR_SHADER_NO_REG when not read).  The
 * vertex index includes the draw's first vertex or vertex offset, the
 * instance index the first instance.  Discards is 1 for a fragment
 * program that may discard its fragment (it gives the depth and
 * visibility unit its feedback, so its draws pass punch-through).
 * Depth_feedback is 1 for a fragment program that writes gl_FragDepth,
 * or writes memory without early fragment tests (it gives the depth with
 * its feedback, so its draws pass depth feedback, before punch-through).
 * Uses_z is 1 for a fragment program that iterates the fragment's depth
 * linearly from the coefficient set at z_coefficient (the depth feedback
 * of a program that writes memory); the driver then has the TA keep each
 * vertex's z and fills that set from it.
 * Stores is 1 for a program that writes buffer memory (a uniform or
 * storage buffer's words).
 *
 * A fragment program with a shared program (shared_length not 0) has
 * it in the same code at shared_offset (16-byte aligned, after the
 * program; length counts both): the driver issues it from the shared
 * register program, with shared_temps temporaries, after the loads.
 * It fills the words of the program's computed request.
 *
 * A compute program with workgroup memory (shared_bytes not 0) keeps
 * each workgroup's in its slice of the dispatch's shared memory
 * (POWERVR_SHADER_REQUEST_SHARED_MEMORY).
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
	uint32_t z_coefficient;
	uint32_t uses_z;
	uint32_t vertex_index_reg;
	uint32_t instance_index_reg;
	uint32_t point_size_reg;
	uint32_t point_coord_coefficient;
	uint32_t discards;
	uint32_t depth_feedback;
	uint32_t stores;
	uint32_t workgroup[3];
	uint32_t shared_bytes;
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

/*
 * The pixel output registers of a fragment key's colour attachments:
 * each attachment's first register (POWERVR_SHADER_NO_REG for none or a
 * format the compiler does not pack), and the registers a pixel uses.
 * Refuses (ENOTSUP) attachments that need a tile buffer.
 */
int powervr_shader_pixel_layout(const uint32_t formats[POWERVR_SHADER_OUTPUTS], uint32_t regs[POWERVR_SHADER_OUTPUTS], uint32_t *registers);

/*
 * Where a colour attachment's pixel lies: its first pixel output
 * register, or, when tile_buffer is a tile buffer's binding (not
 * POWERVR_SHADER_NO_REG), its first word among the pixel's words of
 * that buffer, which hold the same words as the pixel output registers
 * would.
 */
struct powervr_shader_pixel_place {
	uint32_t reg;
	uint32_t tile_buffer;
};

/*
 * The places of a fragment key's colour attachments: in the pixel
 * output registers while they have room, else in tile buffers (reg
 * POWERVR_SHADER_NO_REG for none or a format the compiler does not
 * pack), the registers a pixel uses (up to the furthest word placed,
 * in the registers or in a tile buffer, whose words the end of the tile
 * loads into the registers) and the tile buffers taken.  Refuses
 * (ENOTSUP) attachments past the tile buffers there are.
 */
int powervr_shader_pixel_places(const uint32_t formats[POWERVR_SHADER_OUTPUTS], struct powervr_shader_pixel_place places[POWERVR_SHADER_OUTPUTS], uint32_t *registers, uint32_t *tile_buffers);

#endif /* KERN_DRIVERS_GPU_POWERVR_SHADER_H */
