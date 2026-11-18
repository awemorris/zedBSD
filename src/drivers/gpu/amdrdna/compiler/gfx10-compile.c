/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The amdrdna shader compiler's lowering (gfx10-compile.h): from the
 * shared scalar IR to a list of GFX10 instructions, which gfx10-wait.c
 * then gives its waits and writes as code.
 *
 * Every IR value lives in a VGPR, one 32-bit lane per invocation of the
 * wave.  A value gets the lowest free VGPR when it is defined and gives it
 * back after the last instruction that reads it; a value read inside a
 * loop that was made before the loop lives to the loop's end, and a loop
 * variable keeps its VGPR through every MOVE into it.  A Boolean is a VGPR
 * of all ones or zero: a comparison writes the lane mask VCC_LO and a
 * selection turns it into the Boolean at once; a selection, a loop's end
 * and a guarded access turn the Boolean back into VCC_LO.
 *
 * What is the same for every lane (push constants, uniform buffer words,
 * descriptors, the workgroup id) is read into SGPRs.  The descriptors are
 * loaded once at the start; a uniform word is loaded into one of a few
 * scalar temporaries and copied into the value's VGPR.
 *
 * A loop narrows the lane mask: each pass ends by dropping the lanes whose
 * condition is false and branching back while any lane is left; the mask
 * saved at the loop's start comes back after it.  Lanes that left early
 * keep the values they had, which is what the IR's loop variables say.
 */

#include "gfx10-compile.h"

#include "gfx10-encode.h"
#include "gfx10-wait.h"

#include "drivers/gpu/compiler/optimize.h"
#include "drivers/gpu/compiler/spirv.h"

#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include <stddef.h>
#include <stdint.h>

/* A value without a VGPR. */
#define GFX10_NO_REG			UINT16_MAX

/* A value not yet defined, and an IR instruction that reads no value. */
#define GFX10_NOT_DEFINED		UINT32_MAX

/* The most values one IR instruction reads. */
#define GFX10_MAX_SOURCES		DRV_GPU_IR_TEXTURE_MAX_PARAMS

/* The deepest loop nest. */
#define GFX10_LOOP_DEPTH		16U

/* The scalar temporaries uniform words are loaded into, in turn. */
#define GFX10_SCALAR_TEMPS		8U

/* The instructions the list grows by when it is full. */
#define GFX10_LIST_GROWTH		256U

/* What a uniform of the IR maps to: no slot. */
#define GFX10_NO_SLOT			UINT32_MAX

/*
 * The bits of a float's sign, and of everything but the sign.
 *
 * Values referenced: IEEE 754-2019 (the binary32 format: the sign in bit
 * 31, the exponent and the significand in bits 30:0).
 */
#define GFX10_SIGN_BIT			0x80000000U
#define GFX10_MAGNITUDE_BITS		0x7fffffffU

/*
 * The range of an 8-bit integer colour target: 255 unsigned, -128 .. 127
 * signed (the two's complement bits of -128).
 */
#define GFX10_UINT8_MAX			255U
#define GFX10_SINT8_MAX			127U
#define GFX10_SINT8_MIN			0xffffff80U

/* All ones, the Boolean true of the IR. */
#define GFX10_TRUE			UINT32_MAX

/*
 * 2^32 less 512 as a float (4294966784.0): the reciprocal of a divisor
 * scaled by it and truncated is a 32-bit fixed-point reciprocal a little
 * below the true one.
 *
 * Value referenced: Mesa 25.0.7 src/compiler/nir/nir_lower_idiv.c
 * (emit_udiv, the scale 4294966784.0).
 */
#define GFX10_RECIPROCAL_SCALE		0x4f7ffffeU

/*
 * The first SGPR the compiler hands out for itself (after the user and the
 * system SGPRs), a multiple of four: of every program but a primitive
 * shader, and of a primitive shader (whose user SGPRs start at s8).
 */
#define GFX10_SGPR_FIRST_FREE		8U
#define GFX10_SGPR_FIRST_FREE_NGG	12U
#define GFX10_SGPR_FIRST_FREE_CULL	20U

/*
 * A culling primitive shader's SGPRs: the lane masks of its primitives'
 * tests (the culled ones in the first, which become the kept ones, a
 * temporary in the fifth); after the tests the others hold the kept
 * vertices, the kept vertices of the waves before, of the subgroup, an
 * address, a count and the waves' mask.
 */
#define GFX10_CULL_SGPRS		8U
#define GFX10_CULL_CULLED		0U
#define GFX10_CULL_KEPT_PRIMITIVES	0U
#define GFX10_CULL_KEPT_VERTICES	1U
#define GFX10_CULL_BEFORE		2U
#define GFX10_CULL_LIVE			3U
#define GFX10_CULL_TEMP			4U
#define GFX10_CULL_SCRATCH		5U
#define GFX10_CULL_COUNT		6U
#define GFX10_CULL_WAVE_MASK		7U

/*
 * A culling primitive shader's vertex place in LDS (bytes): x / w, y / w,
 * w, the kept mark and the new number; after the culling the place of a
 * new number holds the vertex and instance numbers its vertex arrived
 * with (in the words of x / w and y / w, read no more by then).  The
 * places of the subgroup's most vertices, then a word for each of its
 * most waves.
 *
 * Values referenced: Mesa 25.0.7 src/amd/common/nir/ac_nir_lower_ngg.c
 * (the lds_es_* layout and compact_vertices_after_culling: the position
 * and the repacked arguments stored at the exporter's place, lines
 * 86-105 and 609-680).
 */
#define GFX10_CULL_X			0U
#define GFX10_CULL_Y			4U
#define GFX10_CULL_W			8U
#define GFX10_CULL_KEPT			12U
#define GFX10_CULL_NUMBER		16U
#define GFX10_CULL_VERTEX_ID		0U
#define GFX10_CULL_INSTANCE_ID		4U
#define GFX10_CULL_PLACE_BYTES		20U
#define GFX10_CULL_PLACES		128U
#define GFX10_CULL_MOST_WAVES		4U

/*
 * The parts a culling primitive shader is lowered in: the whole program
 * of any other; the culling part (the program's position alone, the
 * culling and the compaction); the deferred part (the whole vertex
 * program, run again by the lanes of the vertices kept, then the
 * primitives' export).
 */
#define GFX10_PART_WHOLE		0U
#define GFX10_PART_CULL			1U
#define GFX10_PART_DEFERRED		2U

/* The culling flags' bit numbers (AMDRDNA_GFX10_CULL_*) the program tests. */
#define GFX10_CULL_FLAG_BIT_ENABLED	4U
#define GFX10_CULL_FLAG_BIT_VERTEX_1	5U

/* The VGPRs a compute program's local invocation id x, y and z arrive in. */
#define GFX10_CS_LOCAL_ID_X		0U
#define GFX10_CS_LOCAL_ID_Y		1U
#define GFX10_CS_LOCAL_ID_Z		2U
#define GFX10_CS_INPUT_VGPRS		3U

/* The SGPR a compute program's workgroup id x arrives in; y and z follow. */
#define GFX10_CS_GROUP_ID_X		2U

/*
 * The VGPRs a vertex program's vertex number, primitive number (when the
 * driver has it delivered) and instance number arrive in (each without its
 * base), with VGPR_COMP_CNT 3 so that v0 .. v3 are delivered; and the SGPR
 * a pixel program's primitive mask arrives in, after its two user SGPRs.
 *
 * Values referenced: Mesa 25.0.7 src/amd/vulkan/radv_shader_args.c
 * (declare_vs_input_vgprs, the legacy VS on GFX10: vertex_id, an unused
 * one, vs_prim_id, then instance_id, lines 152-183;
 * declare_ps_input_vgprs and the prim_mask SGPR, line 855).
 */
#define GFX10_VS_VERTEX_ID		0U
#define GFX10_VS_PRIMITIVE_ID		2U
#define GFX10_VS_INSTANCE_ID		3U
#define GFX10_VS_INPUT_VGPRS		4U
#define GFX10_PS_PRIM_MASK		2U

/*
 * The bits of the pixel input enable this compiler uses, and the VGPRs
 * each delivers in this order: the perspective barycentrics at the pixel
 * centre (2), the linear ones (2), the position's x, y, z, w (1 each),
 * the face (1) and the samples the pixel covers (1).  The hardware packs
 * the enabled inputs from v0 in the order of the bits.
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10.json
 * (SPI_PS_INPUT_ENA: PERSP_CENTER_ENA bit 1, LINEAR_CENTER_ENA bit 5,
 * POS_X_FLOAT_ENA .. POS_W_FLOAT_ENA bits 8 .. 11, FRONT_FACE_ENA bit 12,
 * SAMPLE_COVERAGE_ENA bit 14) and src/amd/vulkan/radv_shader_args.c
 * (declare_ps_input_vgprs: the VGPRs in that order, lines 265-276).
 */
#define GFX10_PS_PERSP_CENTER		(1U << 1)
#define GFX10_PS_LINEAR_CENTER		(1U << 5)
#define GFX10_PS_POSITION		(0xfU << 8)
#define GFX10_PS_FRONT_FACE		(1U << 12)
#define GFX10_PS_SAMPLE_COVERAGE	(1U << 14)

/* The colour targets a pixel program writes. */
#define GFX10_COLOR_TARGETS		8U

/*
 * The output channels a program stores, by number: a vertex program's
 * position (4) and point size, or a pixel program's depth, stencil
 * reference and sample mask in the depth target's x, y and z; then for each location four channels (a
 * vertex program's parameter, a pixel program's colour).
 */
#define GFX10_OUTPUT_POSITION		0U
#define GFX10_OUTPUT_POINT_SIZE		4U
#define GFX10_OUTPUT_DEPTH		0U
#define GFX10_OUTPUT_STENCIL		1U
#define GFX10_OUTPUT_SAMPLE_MASK	2U
#define GFX10_OUTPUT_LOCATIONS		8U
#define GFX10_OUTPUT_CHANNELS		(GFX10_OUTPUT_LOCATIONS + 4U * AMDRDNA_GFX10_LOCATIONS)

/* A value whose operand is its VGPR (it has no SGPR or inline constant of its own). */
#define GFX10_NO_OPERAND		UINT16_MAX

/*
 * The most chunks of 16 uniform bytes (push constants or uniform buffer
 * words) loaded into SGPRs at the start, and the scratch VGPRs one IR
 * instruction may take for operands that must be VGPRs.
 */
#define GFX10_UNIFORM_CHUNKS		32U
#define GFX10_SCRATCH_VGPRS		8U

/*
 * The chunks of 16 uniform bytes kept, in turn, for the words that got no
 * SGPR of their own; the cache is forgotten at each loop's start and end,
 * where the code is reached from two places.
 */
#define GFX10_CHUNK_CACHE		4U

/*
 * The most distinct scalar values (SGPRs, the literal) one vector ALU
 * instruction may read on GFX10.
 *
 * Values referenced: Mesa 25.0.7 src/amd/compiler/aco_validate.cpp
 * (validate_ir, the VALU check: const_bus_limit 2 from GFX10 but for the
 * 64-bit shifts, lines 472-480).
 */
#define GFX10_CONSTANT_BUS_LIMIT	2U

/* The SGPRs of an image descriptor and of a sampler, and the byte offset of the sampler after the image descriptor in the table. */
#define GFX10_IMAGE_SGPRS		8U
#define GFX10_SAMPLER_SGPRS		4U

/*
 * A descriptor that is not loaded at the start, for want of SGPRs, but
 * into the window just before each instruction that uses it; the window
 * holds the largest, an image's and its sampler's.
 */
#define GFX10_DESCRIPTOR_AT_USE		(UINT32_MAX - 1U)
#define GFX10_WINDOW_SGPRS		(GFX10_IMAGE_SGPRS + GFX10_SAMPLER_SGPRS)

/*
 * The most address VGPRs a sample takes (an offset, a depth reference,
 * six gradients and three coordinates of a 3D image's; a bias, a layer
 * and a level take the place of some), and the bits of one channel of a
 * packed texel offset: 6 bits, the next channel 8 bits up.
 *
 * Values referenced: Mesa 25.0.7 src/amd/compiler/aco_instruction_selection.cpp
 * (visit_tex: each constant offset & 0x3F shifted by 8 * i, lines
 * 9150-9160).
 */
#define GFX10_SAMPLE_ADDRESSES		12U
#define GFX10_OFFSET_CHANNEL_MASK	0x3fU
#define GFX10_OFFSET_CHANNEL_SHIFT	8U

/*
 * The marks place_addresses() puts for the address VGPRs fill_addresses()
 * computes: the packed offset, the rounded layer of an array, a cube's
 * face coordinates and face, and a cube's gradients on its face.
 */
#define GFX10_ADDRESS_OFFSET		UINT32_MAX
#define GFX10_ADDRESS_LAYER		(UINT32_MAX - 1U)
#define GFX10_ADDRESS_CUBE		(UINT32_MAX - 2U)
#define GFX10_ADDRESS_CUBE_GRADIENT	(UINT32_MAX - 3U)

/*
 * The floats 1.5, which moves a cube's face coordinates into [1, 2], and
 * 8.0, the step of the face number from one cube of an array to the next.
 *
 * Values referenced: Mesa 25.0.7 src/amd/common/nir/ac_nir_lower_tex.c
 * (prepare_cube_coords: nir_ffma_imm2 with 1.5 and nir_ffma_imm1 with
 * 8.0, lines 138-145).
 */
#define GFX10_CUBE_CENTRE		0x3fc00000U
#define GFX10_CUBE_LAYER_STRIDE		0x41000000U

/*
 * The temporaries of a cube sample with gradients: 2 ma, 1 / |2 ma|, the
 * sign of ma, the signs of sc's and tc's gradients, 1.0, a gradient's three
 * components and the gradient along the face's axis.
 */
#define GFX10_CUBE_MA			0U
#define GFX10_CUBE_INVERSE		1U
#define GFX10_CUBE_SIGN			2U
#define GFX10_CUBE_SC_SIGN		3U
#define GFX10_CUBE_TC_SIGN		4U
#define GFX10_CUBE_ONE			5U
#define GFX10_CUBE_DX			6U
#define GFX10_CUBE_DY			7U
#define GFX10_CUBE_DZ			8U
#define GFX10_CUBE_AXIS			9U
#define GFX10_CUBE_TEMPS		10U

/*
 * The state of one compilation.
 *
 * Lives on the stack of amdrdna_gfx10_compile_ir(); its arrays are
 * allocated there and freed before it returns.
 */
struct gfx10_lowering {
	/* The program, the options it is compiled with, the result being built, and where a refusal is described. */
	const struct drv_gpu_shader_ir *ir;
	struct amdrdna_gfx10_compile_options options;
	struct amdrdna_gfx10_program *program;
	struct amdrdna_gfx10_diagnostic *diagnostic;

	/* Each value's VGPR (GFX10_NO_REG while it has none), and the IR instruction that last reads it, plus one (0: none). */
	uint16_t *vgpr;
	uint32_t *last_use;

	/*
	 * Each value's operand when it is not a VGPR: an inline constant, or
	 * the SGPR a uniform word was loaded into at the start
	 * (GFX10_NO_OPERAND otherwise); and whether a MOVE writes the value,
	 * which then always lives in a VGPR.
	 */
	uint16_t *operand_code;
	uint8_t *moved;

	/*
	 * Each output channel: the value its export reads where it lives
	 * (GFX10_NOT_DEFINED for none), when the channel is stored once with
	 * a value that has a VGPR of its own, which then lives to the end;
	 * otherwise the VGPR the stores copy into (GFX10_NO_REG until the
	 * first store takes one, kept to the end).
	 */
	uint32_t output_value[GFX10_OUTPUT_CHANNELS];
	uint16_t output_reg[GFX10_OUTPUT_CHANNELS];

	/* Each output channel the program writes, which a parameter's export enables. */
	uint8_t output_written[GFX10_OUTPUT_CHANNELS];

	/*
	 * The chunks of 16 uniform bytes loaded at the start: the uniform
	 * buffer's slot (or GFX10_NO_SLOT for the push constants), the byte
	 * offset, and the first of the four SGPRs.
	 */
	uint32_t chunk_slot[GFX10_UNIFORM_CHUNKS];
	uint32_t chunk_offset[GFX10_UNIFORM_CHUNKS];
	uint32_t chunk_sgpr[GFX10_UNIFORM_CHUNKS];
	uint32_t chunk_count;

	/*
	 * The cache of uniform chunks: its first SGPR, each entry's buffer and
	 * offset and whether it holds one, the IR instruction (plus one) up
	 * to which a value read straight from its SGPRs keeps it, and the
	 * next to replace.  One more chunk after the entries is loaded when
	 * every entry is kept; its words are copied at once.
	 */
	uint32_t cache_sgpr;
	uint32_t cache_slot[GFX10_CHUNK_CACHE];
	uint32_t cache_offset[GFX10_CHUNK_CACHE];
	uint8_t cache_valid[GFX10_CHUNK_CACHE];
	uint32_t cache_until[GFX10_CHUNK_CACHE];
	uint32_t cache_next;

	/* The scratch VGPRs taken for the IR instruction being lowered, given back after it. */
	uint32_t scratch[GFX10_SCRATCH_VGPRS];
	uint32_t scratch_count;

	/* Which VGPRs are taken, the first one the allocator hands out, and one past the highest ever taken. */
	uint8_t taken[AMDRDNA_GFX10_VGPR_COUNT];
	uint32_t vgpr_first;
	uint32_t vgpr_highest;

	/*
	 * Each uniform's slot: its storage, uniform buffer, texture or storage
	 * image number; GFX10_NO_SLOT for none.
	 */
	uint32_t *slot;

	/*
	 * The first SGPR of each storage buffer's and each uniform buffer's
	 * descriptor, loaded at the start; GFX10_DESCRIPTOR_AT_USE for one
	 * loaded into the window just before each instruction that uses it.
	 */
	uint32_t storage_sgpr[AMDRDNA_GFX10_STORAGE_MAX];
	uint32_t uniform_sgpr[AMDRDNA_GFX10_UNIFORM_MAX];

	/* The SGPRs a descriptor loaded at its use goes to (an image's and its sampler's at most), GFX10_NOT_DEFINED for none. */
	uint32_t descriptor_window;

	/* The SGPR each loop depth saves the lane mask in, the one a guarded access saves it in, and the scalar temporaries. */
	uint32_t loop_save[GFX10_LOOP_DEPTH];
	uint32_t guard_save;
	uint32_t scalar_temp[GFX10_SCALAR_TEMPS];
	uint32_t scalar_temp_next;

	/*
	 * The part of the program lowered (GFX10_PART_*); a culling part's
	 * SGPRs and the VGPRs its position is exported from.
	 */
	uint32_t part;
	uint32_t cull_sgpr[GFX10_CULL_SGPRS];
	uint16_t cull_position[4];

	/* One past the highest SGPR the program uses. */
	uint32_t sgpr_highest;

	/*
	 * A vertex program: the VGPRs of the vertex and instance numbers (with
	 * their bases) and of each fetched location's four channels; the
	 * first SGPR of each fetched location's descriptor.
	 */
	uint32_t vertex_index_vgpr;
	uint32_t instance_index_vgpr;
	uint32_t input_vgpr[AMDRDNA_GFX10_LOCATIONS];
	uint32_t vertex_sgpr[AMDRDNA_GFX10_LOCATIONS];

	/*
	 * A vertex program: the SGPRs its base vertex, first instance and
	 * instance mask arrive in, the VGPRs of its vertex and instance numbers
	 * as the hardware delivers them, and the VGPRs the hardware fills,
	 * which differ between a legacy vertex program and a primitive shader.
	 */
	uint32_t base_vertex_sgpr;
	uint32_t first_instance_sgpr;
	uint32_t instance_mask_sgpr;
	uint32_t vertex_id_input;
	uint32_t instance_id_input;
	uint32_t hardware_vgprs;

	/* The first SGPR after the user and the system SGPRs, a multiple of four. */
	uint32_t sgpr_first_free;

	/*
	 * A vertex program: the IR instruction (plus one) after which each
	 * fetched location's VGPRs, and the vertex and instance numbers', are
	 * read no more (0: only by the start); their VGPRs go back then.  The
	 * bit of each location whose VGPRs went back, and whether the
	 * numbers' and the hardware's input VGPRs did.
	 */
	uint32_t input_until[AMDRDNA_GFX10_LOCATIONS];
	uint32_t vertex_index_until;
	uint32_t instance_index_until;
	uint32_t inputs_returned;
	int vertex_index_returned;
	int instance_index_returned;

	/*
	 * A pixel program: the first VGPR of the perspective and the linear
	 * barycentrics, of the position, of the face and of the samples
	 * covered; the SGPR of the lanes
	 * still alive, and
	 * the label of the early end a discard of every lane takes.
	 */
	uint32_t perspective_vgpr;
	uint32_t screen_linear_vgpr;
	uint32_t position_input_vgpr;
	uint32_t face_vgpr;
	uint32_t coverage_vgpr;
	uint32_t live_sgpr;
	uint32_t early_end_label;

	/* A primitive shader: the label of its end, which a wave without vertices goes to at once. */
	uint32_t vertices_end_label;

	/*
	 * A pixel program that samples with derivatives runs whole quads:
	 * the helper lanes run with the alive ones, and only the alive lanes
	 * are exported.
	 */
	int whole_quads;

	/* The first SGPR of each sampled image's descriptor and of its sampler (GFX10_DESCRIPTOR_AT_USE as above). */
	uint32_t image_sgpr[AMDRDNA_GFX10_TEXTURE_MAX];
	uint32_t sampler_sgpr[AMDRDNA_GFX10_TEXTURE_MAX];

	/* The first SGPR of each storage image's or storage texel buffer's descriptor (GFX10_DESCRIPTOR_AT_USE as above). */
	uint32_t storage_image_sgpr[AMDRDNA_GFX10_IMAGE_MAX];

	/* The instruction list: entries, room, and the next label number. */
	struct amdrdna_gfx10_inst *list;
	uint32_t count;
	uint32_t capacity;
	uint32_t next_label;

	/* Nonzero when the list could not grow. */
	int out_of_memory;

	/* The loops being lowered: each one's label at its start, innermost last. */
	uint32_t loop_label[GFX10_LOOP_DEPTH];
	uint32_t loop_depth;

	/* The IR instruction being lowered. */
	uint32_t index;

	/*
	 * The Boolean whose lanes VCC_LO holds now (GFX10_NOT_DEFINED for
	 * none): a comparison leaves its own there, a condition taken from a
	 * Boolean leaves that one.  Any other write of VCC_LO or of the lane
	 * mask, and any label, forgets it.
	 */
	uint32_t vcc_value;

	/*
	 * Each value that is a Boolean only ever used as a condition, with VCC_LO
	 * kept from its comparison to its last use: it gets no VGPR.
	 */
	uint8_t *condition_only;

	/* Nonzero when a condition the lowering counted on was not in VCC_LO. */
	int lost_condition;

	/* The Boolean a guard narrowed the lanes by, which VCC_LO still holds when the guard ends. */
	uint32_t guard_condition;

	/* Each IR instruction whose results nothing reads and that has no other effect: it is not lowered. */
	uint8_t *dead;
};

/*
 * What one texture instruction asks of the image: the values of its
 * coordinate (u, v and a 3D image's r, or a cube's direction), the layer
 * of an array (GFX10_NOT_DEFINED for none), the depth reference, the bias,
 * the level and the gradients (each coordinate's along x, then each one's
 * along y), and which of them it has; the texel offset of u, v and r (4
 * bits each, -8 .. 7); a gather and its channel, a fetch of an integer
 * texel, or a size query of a level; and the image's shape.
 *
 * Lives on the stack of lower_sample().
 */
struct gfx10_sample {
	uint32_t coordinate[3];
	uint32_t coordinate_count;
	uint32_t layer;
	uint32_t reference;
	uint32_t bias;
	uint32_t level;
	uint32_t gradient[6];
	uint32_t offset[3];
	uint32_t channel;
	int compared;
	int biased;
	int leveled;
	int level_is_zero;
	int gradients;
	int gathered;
	int fetch;
	int size_query;
	int volume;
	int cube;
	int arrayed;
	int buffer;
	int multisampled;
	uint32_t sample_index;
	int lod_query;
};

static int refuse(struct gfx10_lowering *lowering, int error, const char *reason);
static int lower_part(const struct drv_gpu_shader_ir *ir, const struct amdrdna_gfx10_compile_options *options, uint32_t part, uint32_t first_label, struct amdrdna_gfx10_diagnostic *diagnostic, struct gfx10_lowering *lowering);
static void release_lowering(struct gfx10_lowering *lowering);
static int writes_memory(const struct drv_gpu_shader_ir *ir);
static int compile_culling(const struct drv_gpu_shader_ir *ir, const struct amdrdna_gfx10_compile_options *options, struct amdrdna_gfx10_program **program, struct amdrdna_gfx10_diagnostic *diagnostic);
static int ir_sources(const struct drv_gpu_shader_ir_inst *instruction, uint32_t *sources);
static int ir_defines(const struct drv_gpu_shader_ir_inst *instruction);
static int prepare(struct gfx10_lowering *lowering);
static int find_dead(struct gfx10_lowering *lowering);
static int extend_loops(struct gfx10_lowering *lowering);
static int map_uniforms(struct gfx10_lowering *lowering);
static void texture_shape(const struct gfx10_lowering *lowering, const struct drv_gpu_shader_ir_uniform *uniform, struct amdrdna_gfx10_binding *binding);
static int prepare_graphics(struct gfx10_lowering *lowering);
static const struct drv_gpu_shader_ir_io *find_input(const struct drv_gpu_shader_ir *ir, uint32_t location);
static int plan_registers(struct gfx10_lowering *lowering);
static int plan_descriptors(struct gfx10_lowering *lowering, uint32_t deepest, uint32_t *sgpr);
static uint32_t storage_descriptor(struct gfx10_lowering *lowering, uint32_t slot);
static uint32_t uniform_descriptor(struct gfx10_lowering *lowering, uint32_t slot);
static void texture_descriptors(struct gfx10_lowering *lowering, uint32_t slot, uint32_t *image, uint32_t *sampler);
static uint32_t storage_image_descriptor(struct gfx10_lowering *lowering, uint32_t slot);
static int storage_image_slot(struct gfx10_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t *slot, int *texel_buffer);
static unsigned storage_dimension(uint32_t shape, uint32_t *count);
static int storage_address(struct gfx10_lowering *lowering, uint32_t first, uint32_t count, uint32_t *block);
static int lower_image(struct gfx10_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);
static uint32_t reserve_vgprs(struct gfx10_lowering *lowering, uint32_t count);
static void prologue_vertex(struct gfx10_lowering *lowering);
static void prologue_primitive(struct gfx10_lowering *lowering);
static int place_for_culling(struct gfx10_lowering *lowering);
static int cull_and_compact(struct gfx10_lowering *lowering);
static int finish_exports(struct gfx10_lowering *lowering);
static void emit_lanes_below(struct gfx10_lowering *lowering, unsigned count);
static void emit_field(struct gfx10_lowering *lowering, unsigned dst, unsigned source, uint32_t selector);
static void emit_scalar_literal(struct gfx10_lowering *lowering, unsigned opcode, unsigned dst, unsigned source, uint32_t literal);
static void plan_input_lifetimes(struct gfx10_lowering *lowering);
static void return_inputs(struct gfx10_lowering *lowering, uint32_t upto);
static uint32_t cache_chunk(struct gfx10_lowering *lowering, uint32_t slot, uint32_t offset, int spare);
static int alias_uniform(struct gfx10_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction);
static int lower_input(struct gfx10_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);
static int lower_output(struct gfx10_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction);
static void lower_kill(struct gfx10_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction);
static int lower_sample(struct gfx10_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);
static int read_sample(struct gfx10_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, struct gfx10_sample *sample);
static uint32_t place_addresses(const struct gfx10_sample *sample, uint32_t *addresses);
static int fill_addresses(struct gfx10_lowering *lowering, const struct gfx10_sample *sample, const uint32_t *addresses, uint32_t count, uint32_t block);
static int fill_cube(struct gfx10_lowering *lowering, const struct gfx10_sample *sample, uint32_t first);
static int fill_cube_gradients(struct gfx10_lowering *lowering, const struct gfx10_sample *sample, uint32_t first, uint32_t gradients);
static void cube_gradient(struct gfx10_lowering *lowering, const uint32_t *direction, const uint32_t *temp, uint32_t first, uint32_t out);
static void emit_compare_face(struct gfx10_lowering *lowering, unsigned opcode, uint32_t constant, uint32_t face);
static int fetch_texel_buffer(struct gfx10_lowering *lowering, const struct gfx10_sample *sample, uint32_t slot, uint32_t dst);
static void emit_modified(struct gfx10_lowering *lowering, unsigned opcode, unsigned dst, unsigned src, unsigned modifiers);
static void emit_fused(struct gfx10_lowering *lowering, unsigned dst, unsigned a, unsigned b, uint32_t constant);
static void emit_layer_face(struct gfx10_lowering *lowering, unsigned face, unsigned layer);
static uint32_t sample_offset(const struct gfx10_sample *sample);
static unsigned sample_dimension(const struct gfx10_sample *sample);
static unsigned sample_opcode(const struct gfx10_sample *sample, int derivatives);
static int lower_derivative(struct gfx10_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);
static void restrict_to_alive(struct gfx10_lowering *lowering);
static int finish_exports(struct gfx10_lowering *lowering);
static int export_location(struct gfx10_lowering *lowering, uint32_t target, uint32_t first_channel, uint32_t mask, uint32_t flags);
static int export_colour(struct gfx10_lowering *lowering, uint32_t target, uint32_t first_channel, uint32_t flags);
static int export_wide_colour(struct gfx10_lowering *lowering, uint32_t target, uint32_t first_channel, uint32_t kind, uint32_t flags);
static int export_source(struct gfx10_lowering *lowering, uint32_t channel, unsigned *code);
static void emit_export(struct gfx10_lowering *lowering, uint32_t target, const unsigned *sources, uint32_t mask, uint32_t flags);
static int prologue(struct gfx10_lowering *lowering);
static int lower(struct gfx10_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);
static int lower_float(struct gfx10_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);
static int lower_integer(struct gfx10_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);
static int lower_compare(struct gfx10_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);
static int lower_uniform(struct gfx10_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);
static int lower_system(struct gfx10_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);
static int lower_storage(struct gfx10_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);
static int lower_loop(struct gfx10_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction);
static int lower_divide(struct gfx10_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);
static void divide_unsigned(struct gfx10_lowering *lowering, unsigned numerator, unsigned denominator, uint32_t quotient, uint32_t remainder, uint32_t temp);
static int lower_atomic(struct gfx10_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);
static int lower_shared(struct gfx10_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);
static void lower_sync(struct gfx10_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction);
static void emit_waits(struct gfx10_lowering *lowering, uint32_t fences);
static int finish(struct gfx10_lowering *lowering);
static int finish_code(struct gfx10_lowering *lowering);
static void guard_begin(struct gfx10_lowering *lowering, uint32_t predicate);
static int guard_writes(struct gfx10_lowering *lowering, uint32_t predicate);
static void guard_end(struct gfx10_lowering *lowering);
static void condition_from_boolean(struct gfx10_lowering *lowering, uint32_t value);
static void boolean_from_condition(struct gfx10_lowering *lowering, uint32_t dst);
static void emit(struct gfx10_lowering *lowering, const struct amdrdna_gfx10_inst *inst);
static void emit_vector(struct gfx10_lowering *lowering, enum amdrdna_gfx10_kind kind, unsigned opcode, unsigned dst, unsigned src0, unsigned src1, unsigned src2);
static void emit_vector_literal(struct gfx10_lowering *lowering, enum amdrdna_gfx10_kind kind, unsigned opcode, unsigned dst, unsigned src0, unsigned src1, uint32_t literal);
static void emit_multiply_add(struct gfx10_lowering *lowering, unsigned dst, unsigned factor, uint32_t constant, unsigned addend);
static void emit_scalar(struct gfx10_lowering *lowering, enum amdrdna_gfx10_kind kind, unsigned opcode, unsigned dst, unsigned src0, unsigned src1);
static void emit_scalar_load(struct gfx10_lowering *lowering, unsigned opcode, unsigned dst, unsigned base, uint32_t offset, unsigned count);
static void emit_label(struct gfx10_lowering *lowering, uint32_t label);
static void emit_branch(struct gfx10_lowering *lowering, unsigned opcode, uint32_t label);
static unsigned constant_operand(uint32_t bits);
static unsigned value_operand(const struct gfx10_lowering *lowering, uint32_t value);
static unsigned vgpr_operand(struct gfx10_lowering *lowering, uint32_t value);
static unsigned scratch_copy(struct gfx10_lowering *lowering, unsigned code);
static void give_scratch(struct gfx10_lowering *lowering);
static void legalize_scalars(struct gfx10_lowering *lowering, struct amdrdna_gfx10_inst *inst);
static unsigned vector_reads(const struct amdrdna_gfx10_inst *inst);
static void plan_uniforms(struct gfx10_lowering *lowering, uint32_t *sgpr);
static int has_operand(struct gfx10_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction);
static int plan_outputs(struct gfx10_lowering *lowering);
static void plan_conditions(struct gfx10_lowering *lowering);
static int defines_condition(const struct gfx10_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction);
static uint32_t condition_source(const struct drv_gpu_shader_ir_inst *instruction);
static int touches_condition(const struct gfx10_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction);
static void forget_condition(struct gfx10_lowering *lowering, const struct amdrdna_gfx10_inst *inst);
static uint32_t output_channel(const struct gfx10_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction);
static unsigned next_scalar_temp(struct gfx10_lowering *lowering);
static int take_vgpr(struct gfx10_lowering *lowering, uint32_t *reg);
static int take_vgpr_pair(struct gfx10_lowering *lowering, uint32_t *reg);
static int take_vgpr_block(struct gfx10_lowering *lowering, uint32_t count, uint32_t *reg);
static void give_vgpr(struct gfx10_lowering *lowering, uint32_t reg);
static void retire(struct gfx10_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction);

/*
 * Compiles a SPIR-V module's stage.
 *
 * The module is parsed by the shared frontend, optimized, and lowered; a
 * program the lowering refuses after the optimizer rewrote it is lowered
 * again from the parsed one, which keeps the parsed program's outcome.
 * Returns 0 with the program, or the frontend's or the lowering's error
 * with the diagnostic filled.
 */
int
amdrdna_gfx10_compile(
	const uint32_t *words,
	size_t word_count,
	enum drv_gpu_shader_stage stage,
	const struct amdrdna_gfx10_compile_options *options,
	struct amdrdna_gfx10_program **program,
	struct amdrdna_gfx10_diagnostic *diagnostic)
{
	struct drv_gpu_shader_ir *ir;
	struct drv_gpu_shader_ir optimized;
	struct drv_gpu_shader_optimize_options optimize_options;
	struct drv_gpu_compile_diagnostic parse_diagnostic;
	uint32_t features;
	int error;

	/* A refused compile never leaves an old output pointer live. */
	if (program == NULL)
		return EINVAL;
	*program = NULL;

	/* Starts with no refusal. */
	if (diagnostic != NULL)
		kern_memset(diagnostic, 0, sizeof(*diagnostic));

	/* A module without words is refused. */
	if (words == NULL)
		return EINVAL;

	/* Parses the stage. */
	ir = NULL;
	parse_diagnostic.word_offset = 0U;
	parse_diagnostic.reason = NULL;
	features = AMDRDNA_GFX10_SPIRV_FEATURES;
	if (options != NULL && options->descriptor_arrays != 0U)
		features |= AMDRDNA_GFX10_SPIRV_FEATURES_ARRAYS;
	error = drv_gpu_shader_parse_features(words, word_count, stage, features, &ir, &parse_diagnostic);
	if (error != 0) {
		if (diagnostic != NULL) {
			diagnostic->instruction = parse_diagnostic.word_offset;
			diagnostic->reason = parse_diagnostic.reason;
		}

		/* Reports why the stage could not be parsed. */
		return error;
	}

	/* Optimizes a private copy: every block runs (nothing is skipped), and the known bits fold. */
	kern_memset(&optimize_options, 0, sizeof(optimize_options));
	optimize_options.runs_every_block = 1U;
	optimize_options.simplify = 1U;
	kern_memset(optimize_options.needed, 1, sizeof(optimize_options.needed));
	error = drv_gpu_shader_optimize(ir, &optimize_options, &optimized);
	if (error != 0) {
		drv_gpu_shader_ir_free(ir);
		return error;
	}

	/* Lowers the rewritten program, or the parsed one when the optimizer left it alone. */
	if (optimized.instructions != NULL) {
		error = amdrdna_gfx10_compile_ir(&optimized, options, program, diagnostic);
	} else {
		error = amdrdna_gfx10_compile_ir(ir, options, program, diagnostic);
	}

	/* A rewritten program refused for any reason but memory is lowered from the parsed one. */
	if (optimized.instructions != NULL && error != 0 && error != ENOMEM) {
		if (diagnostic != NULL)
			kern_memset(diagnostic, 0, sizeof(*diagnostic));
		error = amdrdna_gfx10_compile_ir(ir, options, program, diagnostic);
	}

	/* The IR retires before the program is handed over. */
	kern_free(optimized.instructions);
	drv_gpu_shader_ir_free(ir);

	/* Reports the lowering's refusal. */
	if (error != 0)
		return error;

	/* Succeeded: the caller owns the program. */
	return 0;
}

/*
 * Compiles a parsed program.
 *
 * A vertex program asked to cull as a primitive shader is lowered in two
 * parts, which run one after the other in the one program: its position
 * alone, the culling and the compaction, then the whole program again on
 * the vertices kept (a program that writes memory, which must not run
 * twice, does not cull).
 *
 * Returns 0 with the program in *program, EINVAL for a malformed program,
 * ENOTSUP for one the compiler does not cover (both with the diagnostic),
 * or ENOMEM.
 */
int
amdrdna_gfx10_compile_ir(
	const struct drv_gpu_shader_ir *ir,
	const struct amdrdna_gfx10_compile_options *options,
	struct amdrdna_gfx10_program **program,
	struct amdrdna_gfx10_diagnostic *diagnostic)
{
	struct gfx10_lowering *lowering;
	struct amdrdna_gfx10_compile_options whole_options;
	int error;

	/* A refused compile never leaves an old output pointer live. */
	if (program == NULL || ir == NULL)
		return EINVAL;
	*program = NULL;

	/* A vertex program that culls is lowered in its two parts. */
	kern_memset(&whole_options, 0, sizeof(whole_options));
	if (options != NULL)
		whole_options = *options;
	if (ir->stage == DRV_GPU_STAGE_VERTEX && whole_options.primitive_shader != 0U && whole_options.primitive_culling != 0U) {
		if (writes_memory(ir) == 0)
			return compile_culling(ir, &whole_options, program, diagnostic);
		whole_options.primitive_culling = 0U;
	}

	/* The state, too large for a kernel stack. */
	lowering = kern_calloc(1U, sizeof(*lowering));
	if (lowering == NULL)
		return ENOMEM;

	/* The program in one part, its waits and its code. */
	error = lower_part(ir, &whole_options, GFX10_PART_WHOLE, 0U, diagnostic, lowering);
	if (error == 0)
		error = finish_code(lowering);
	release_lowering(lowering);

	/* A refused program is never handed over. */
	if (error != 0) {
		amdrdna_gfx10_program_free(lowering->program);
		kern_free(lowering);
		return error;
	}

	/* Succeeded: the caller owns the program. */
	*program = lowering->program;
	kern_free(lowering);
	return 0;
}

/*
 * Lowers a program, or one part of it, into lowering: its instruction
 * list (labels numbered from first_label) and its program, which the
 * caller frees, with release_lowering() for the rest.
 */
static int
lower_part(
	const struct drv_gpu_shader_ir *ir,
	const struct amdrdna_gfx10_compile_options *options,
	uint32_t part,
	uint32_t first_label,
	struct amdrdna_gfx10_diagnostic *diagnostic,
	struct gfx10_lowering *lowering)
{
	const struct drv_gpu_shader_ir_inst *instruction;
	uint32_t dst;
	uint32_t index;
	uint32_t block;
	int defines;
	int error;

	/* The state, empty. */
	kern_memset(lowering, 0, sizeof(*lowering));
	lowering->ir = ir;
	lowering->diagnostic = diagnostic;
	lowering->options = *options;
	lowering->part = part;
	lowering->next_label = first_label;

	/* The result. */
	lowering->program = kern_calloc(1U, sizeof(*lowering->program));
	if (lowering->program == NULL)
		return ENOMEM;
	lowering->program->stage = ir->stage;
	if (ir->stage == DRV_GPU_STAGE_FRAGMENT)
		lowering->program->color_exports = options->color_exports;
	lowering->program->point_coord_attribute = AMDRDNA_GFX10_NO_ATTRIBUTE;
	lowering->program->primitive_id_attribute = AMDRDNA_GFX10_NO_ATTRIBUTE;

	/* Checks the program, finds the last reads, maps the uniforms and places the registers. */
	error = prepare(lowering);

	/* Loads what every lane shares. */
	if (error == 0)
		error = prologue(lowering);

	/* Lowers every instruction in order. */
	for (index = 0U; error == 0 && index < ir->instruction_count; index++) {
		instruction = &ir->instructions[index];
		lowering->index = index;

		/* An instruction whose results nothing reads is not lowered. */
		if (lowering->dead[index] != 0U)
			continue;

		/* A constant or a uniform word with an operand of its own needs no instruction. */
		defines = has_operand(lowering, instruction);
		if (defines != 0)
			continue;

		/* The destination's VGPR, a value redefined by a MOVE keeping its own; a sample's four in a row. */
		dst = GFX10_NO_REG;
		defines = ir_defines(instruction);
		if (defines == 4) {
			/* A sample's channels in a block of their own. */
			error = take_vgpr_block(lowering, 4U, &dst);
			if (error != 0)
				break;
			for (block = 0U; block < 4U; block++)
				lowering->vgpr[instruction->dst + block] = (uint16_t)(dst + block);
		} else if (defines != 0 && lowering->condition_only[instruction->dst] != 0U) {
			/* A Boolean used only as a condition stays in VCC_LO. */
			dst = GFX10_NO_REG;
		} else if (defines != 0) {
			if (lowering->vgpr[instruction->dst] == GFX10_NO_REG) {
				error = take_vgpr(lowering, &dst);
				if (error != 0)
					break;
				lowering->vgpr[instruction->dst] = (uint16_t)dst;
			}

			/* Takes the register the value now lives in. */
			dst = lowering->vgpr[instruction->dst];
		}

		/* The instruction's GFX10 instructions. */
		error = lower(lowering, instruction, dst);
		if (error != 0)
			break;

		/* A list that could not grow fails the compile. */
		if (lowering->out_of_memory != 0) {
			error = ENOMEM;
			break;
		}

		/* A condition expected in VCC_LO that was not there is the compiler's own fault, refused. */
		if (lowering->lost_condition != 0) {
			error = refuse(lowering, ENOTSUP, "a condition was not kept where the compiler expected it");
			break;
		}

		/* The scratch VGPRs, and the VGPRs of values no later instruction reads, go back. */
		give_scratch(lowering);
		retire(lowering, instruction);
		return_inputs(lowering, index + 1U);
	}

	/* The end of the program or of the part. */
	if (error == 0)
		error = finish(lowering);

	/* Reports the outcome; the caller releases the state. */
	return error;
}

/* Frees what a lowering allocated but its program: the per-value and per-uniform arrays and the list. */
static void
release_lowering(
	struct gfx10_lowering *lowering)
{
	/* Each array, NULL when it was never allocated. */
	kern_free(lowering->vgpr);
	kern_free(lowering->last_use);
	kern_free(lowering->operand_code);
	kern_free(lowering->moved);
	kern_free(lowering->condition_only);
	kern_free(lowering->dead);
	kern_free(lowering->slot);
	kern_free(lowering->list);
	lowering->vgpr = NULL;
	lowering->last_use = NULL;
	lowering->operand_code = NULL;
	lowering->moved = NULL;
	lowering->condition_only = NULL;
	lowering->dead = NULL;
	lowering->slot = NULL;
	lowering->list = NULL;
}

/* Reports nonzero when a program writes memory (a storage, image or shared store, an atomic): it must run once a vertex. */
static int
writes_memory(
	const struct drv_gpu_shader_ir *ir)
{
	uint32_t index;
	uint32_t op;

	/* Each instruction's operation. */
	for (index = 0U; index < ir->instruction_count; index++) {
		op = ir->instructions[index].op;
		if (op == DRV_GPU_IR_STORE_STORAGE || op == DRV_GPU_IR_STORE_SHARED || op == DRV_GPU_IR_ATOMIC ||
		    op == DRV_GPU_IR_IMAGE_WRITE)
			return 1;
	}

	/* Nothing is written. */
	return 0;
}

/*
 * Compiles a vertex program that culls as a primitive shader in its two
 * parts: the culling part lowers a copy of the program whose outputs but
 * the position are dropped (their computation goes with them), the
 * deferred part the whole program; the deferred part's code follows the
 * culling part's, its labels numbered after them, and the program takes
 * the deferred part's interface with the culling part's user SGPRs and
 * LDS, and the registers of the larger of the two.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/common/nir/ac_nir_lower_ngg.c
 * (add_deferred_attribute_culling: the top part a clone of the shader
 * with every output but the position removed, lines 1170-1210;
 * remove_culling_shader_outputs).
 */
static int
compile_culling(
	const struct drv_gpu_shader_ir *ir,
	const struct amdrdna_gfx10_compile_options *options,
	struct amdrdna_gfx10_program **program,
	struct amdrdna_gfx10_diagnostic *diagnostic)
{
	struct gfx10_lowering *cull;
	struct gfx10_lowering *deferred;
	struct drv_gpu_shader_ir position_ir;
	struct drv_gpu_shader_ir_inst *instructions;
	struct amdrdna_gfx10_compile_options deferred_options;
	uint32_t index;
	int error;

	/* The two states, too large for a kernel stack, nothing lowered yet; a copy of the instructions. */
	cull = kern_calloc(1U, sizeof(*cull));
	deferred = kern_calloc(1U, sizeof(*deferred));
	instructions = kern_calloc(ir->instruction_count + 1U, sizeof(*instructions));
	if (cull == NULL || deferred == NULL || instructions == NULL) {
		kern_free(cull);
		kern_free(deferred);
		kern_free(instructions);
		return ENOMEM;
	}

	/* The position part: the copy with every other output's store dropped. */
	for (index = 0U; index < ir->instruction_count; index++) {
		instructions[index] = ir->instructions[index];
		if (instructions[index].op == DRV_GPU_IR_STORE_OUTPUT && instructions[index].location != DRV_GPU_IR_LOCATION_POSITION)
			instructions[index].op = DRV_GPU_IR_NOP;
	}
	position_ir = *ir;
	position_ir.instructions = instructions;

	/* The two parts, the deferred one's labels after the culling one's. */
	error = lower_part(&position_ir, options, GFX10_PART_CULL, 0U, diagnostic, cull);
	if (error == 0) {
		deferred_options = *options;
		deferred_options.primitive_culling = 0U;
		error = lower_part(ir, &deferred_options, GFX10_PART_DEFERRED, cull->next_label, diagnostic, deferred);
	}

	/* The deferred part's code after the culling part's, in the deferred part's lowering. */
	for (index = 0U; error == 0 && index < deferred->count; index++)
		emit(cull, &deferred->list[index]);
	if (error == 0 && cull->out_of_memory != 0)
		error = ENOMEM;
	if (error == 0) {
		kern_free(deferred->list);
		deferred->list = cull->list;
		deferred->count = cull->count;
		deferred->capacity = cull->capacity;
		cull->list = NULL;
		if (cull->vgpr_highest > deferred->vgpr_highest)
			deferred->vgpr_highest = cull->vgpr_highest;
		if (cull->sgpr_highest > deferred->sgpr_highest)
			deferred->sgpr_highest = cull->sgpr_highest;
		deferred->program->user_sgprs = cull->program->user_sgprs;
		deferred->program->shared_bytes = cull->program->shared_bytes;
		deferred->program->primitive_culling = 1U;
		error = finish_code(deferred);
	}

	/* The culling part's program and both lowerings' state go; the position part's copy too. */
	release_lowering(cull);
	release_lowering(deferred);
	amdrdna_gfx10_program_free(cull->program);
	kern_free(instructions);

	/* A refused program is never handed over. */
	if (error != 0) {
		amdrdna_gfx10_program_free(deferred->program);
		kern_free(cull);
		kern_free(deferred);
		return error;
	}

	/* Succeeded: the caller owns the program. */
	*program = deferred->program;
	kern_free(cull);
	kern_free(deferred);
	return 0;
}


/*
 * Reports how a pixel program exports a colour location: its options' 4
 * bits of the location; the second colour of a dual-source blend goes to
 * target 0 as the first does, and is exported as the first is.
 */
uint32_t
amdrdna_gfx10_colour_export(
	const struct amdrdna_gfx10_program *program,
	uint32_t location)
{
	uint32_t kind;

	/* The second colour of a dual-source blend as the first. */
	if (program->dual_source != 0U && location == 1U)
		location = 0U;

	/* The location's bits; a location past the targets, or a kind not known, as four 16-bit floats. */
	if (location >= GFX10_COLOR_TARGETS)
		return AMDRDNA_GFX10_COLOUR_HALVES;
	kind = (program->color_exports >> (AMDRDNA_GFX10_COLOUR_BITS * location)) & AMDRDNA_GFX10_COLOUR_MASK;
	if (kind >= AMDRDNA_GFX10_COLOUR_KINDS)
		return AMDRDNA_GFX10_COLOUR_HALVES;

	/* Succeeded: the location's export. */
	return kind;
}
/*
 * Frees a compiled program.
 */
void
amdrdna_gfx10_program_free(
	struct amdrdna_gfx10_program *program)
{
	/* Nothing to free. */
	if (program == NULL)
		return;

	/* The code, then the record. */
	kern_free(program->code);
	kern_free(program);
}

/* Describes a refusal at the instruction being lowered and returns its error. */
static int
refuse(
	struct gfx10_lowering *lowering,
	int error,
	const char *reason)
{
	/* The diagnostic is optional. */
	if (lowering->diagnostic != NULL) {
		lowering->diagnostic->instruction = lowering->index;
		lowering->diagnostic->reason = reason;
	}

	/* Reports the error. */
	return error;
}

/*
 * Lists the values an IR instruction reads.
 *
 * Returns how many it put in sources[], or -1 for an operation the
 * compiler does not lower.
 */
static int
ir_sources(
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t *sources)
{
	uint32_t count;
	uint32_t index;

	/* The number of values read, by the operation. */
	count = 0U;
	switch (instruction->op) {
	case DRV_GPU_IR_NOP:
	case DRV_GPU_IR_SKIP_BEGIN:
	case DRV_GPU_IR_SKIP_END:
	case DRV_GPU_IR_CONST:
	case DRV_GPU_IR_ICONST:
	case DRV_GPU_IR_BOOL:
	case DRV_GPU_IR_LOAD_PUSH:
	case DRV_GPU_IR_LOAD_UBO:
	case DRV_GPU_IR_LOAD_SYSTEM:
	case DRV_GPU_IR_STORAGE_SIZE:
	case DRV_GPU_IR_LOOP_BEGIN:
	case DRV_GPU_IR_LOAD_INPUT:
		count = 0U;
		break;
	case DRV_GPU_IR_FNEG:
	case DRV_GPU_IR_FABS:
	case DRV_GPU_IR_RSQ:
	case DRV_GPU_IR_RCP:
	case DRV_GPU_IR_SQRT:
	case DRV_GPU_IR_EXP2:
	case DRV_GPU_IR_LOG2:
	case DRV_GPU_IR_SIN:
	case DRV_GPU_IR_COS:
	case DRV_GPU_IR_FLOOR:
	case DRV_GPU_IR_FRACT:
	case DRV_GPU_IR_FTRUNC:
	case DRV_GPU_IR_FROUND_EVEN:
	case DRV_GPU_IR_NOT:
	case DRV_GPU_IR_INEG:
	case DRV_GPU_IR_INOT:
	case DRV_GPU_IR_I2F:
	case DRV_GPU_IR_U2F:
	case DRV_GPU_IR_F2I:
	case DRV_GPU_IR_F2U:
	case DRV_GPU_IR_MOVE:
	case DRV_GPU_IR_UNPACK_HALF:
	case DRV_GPU_IR_LOOP_END:
	case DRV_GPU_IR_STORE_OUTPUT:
	case DRV_GPU_IR_KILL:
	case DRV_GPU_IR_DDX:
	case DRV_GPU_IR_DDX_FINE:
	case DRV_GPU_IR_DDY:
	case DRV_GPU_IR_DDY_FINE:
		count = 1U;
		break;
	case DRV_GPU_IR_FADD:
	case DRV_GPU_IR_FSUB:
	case DRV_GPU_IR_FMUL:
	case DRV_GPU_IR_FMIN:
	case DRV_GPU_IR_FMAX:
	case DRV_GPU_IR_FLT:
	case DRV_GPU_IR_FGE:
	case DRV_GPU_IR_FEQ:
	case DRV_GPU_IR_FNEU:
	case DRV_GPU_IR_AND:
	case DRV_GPU_IR_OR:
	case DRV_GPU_IR_IADD:
	case DRV_GPU_IR_ISUB:
	case DRV_GPU_IR_IMUL:
	case DRV_GPU_IR_IAND:
	case DRV_GPU_IR_IOR:
	case DRV_GPU_IR_IXOR:
	case DRV_GPU_IR_SHL:
	case DRV_GPU_IR_SHR:
	case DRV_GPU_IR_ASR:
	case DRV_GPU_IR_ILT:
	case DRV_GPU_IR_IGE:
	case DRV_GPU_IR_ULT:
	case DRV_GPU_IR_UGE:
	case DRV_GPU_IR_IEQ:
	case DRV_GPU_IR_INE:
	case DRV_GPU_IR_PACK_HALF:
	case DRV_GPU_IR_UDIV:
	case DRV_GPU_IR_UMOD:
	case DRV_GPU_IR_IDIV:
	case DRV_GPU_IR_IREM:
		count = 2U;
		break;
	case DRV_GPU_IR_BARRIER:
	case DRV_GPU_IR_FENCE:
		count = 0U;
		break;
	case DRV_GPU_IR_ATOMIC:
		/* The offset and the value, the compare of a compare-and-exchange, then the predicate. */
		count = 2U;
		if (instruction->immediate == DRV_GPU_IR_ATOMIC_CMPXCHG)
			count++;
		if (instruction->component == DRV_GPU_IR_ATOMIC_PREDICATED)
			count++;

		/* An image's texel also reads its y and its layer, after the others. */
		if ((instruction->image_shape & DRV_GPU_IR_IMAGE_TEXEL) != 0U) {
			for (index = 0U; index < count; index++)
				sources[index] = instruction->src[index];
			sources[count] = instruction->src[0] + 1U;
			sources[count + 1U] = instruction->src[0] + 2U;
			return (int)count + 2;
		}
		break;
	case DRV_GPU_IR_IMAGE_READ:
		/* The src[1] parameters from src[0], then the predicate of a guarded read. */
		if (instruction->src[1] == 0U || instruction->src[1] > 3U)
			return -1;
		for (index = 0U; index < instruction->src[1]; index++)
			sources[index] = instruction->src[0] + index;
		count = instruction->src[1];
		if (instruction->component != 0U)
			sources[count++] = instruction->src[2];
		return (int)count;
	case DRV_GPU_IR_IMAGE_WRITE:
		/* The src[1] parameters from src[0], the four texel values from src[2], then the predicate. */
		if (instruction->src[1] == 0U || instruction->src[1] > 3U)
			return -1;
		for (index = 0U; index < instruction->src[1]; index++)
			sources[index] = instruction->src[0] + index;
		count = instruction->src[1];
		for (index = 0U; index < 4U; index++)
			sources[count++] = instruction->src[2] + index;
		if (instruction->component != 0U)
			sources[count++] = instruction->src[3];
		return (int)count;
	case DRV_GPU_IR_SELECT:
		count = 3U;
		break;
	case DRV_GPU_IR_SAMPLE:
		count = 2U;
		break;
	case DRV_GPU_IR_SAMPLE_BIAS:
	case DRV_GPU_IR_SAMPLE_LOD:
		count = 3U;
		break;
	case DRV_GPU_IR_TEXTURE:
		/* The message's parameters are src[1] consecutive values from src[0]. */
		if (instruction->src[1] == 0U || instruction->src[1] > DRV_GPU_IR_TEXTURE_MAX_PARAMS)
			return -1;
		for (index = 0U; index < instruction->src[1]; index++)
			sources[index] = instruction->src[0] + index;
		return (int)instruction->src[1];
	case DRV_GPU_IR_LOAD_STORAGE:
	case DRV_GPU_IR_LOAD_SHARED:
		/* The offset, and the predicate of a guarded load. */
		count = 1U;
		if (instruction->component != 0U)
			count = 2U;
		break;
	case DRV_GPU_IR_STORE_STORAGE:
	case DRV_GPU_IR_STORE_SHARED:
		/* The offset and the data, and the predicate of a guarded store. */
		count = 2U;
		if (instruction->component != 0U)
			count = 3U;
		break;
	default:
		/* An operation the compiler does not lower yet. */
		return -1;
	}

	/* The sources are the first ones of the instruction. */
	for (index = 0U; index < count; index++)
		sources[index] = instruction->src[index];

	/* Reports the count. */
	return (int)count;
}

/* Reports how many values from its destination an IR instruction defines: 0, 1, or a sample's (or an image read's) 4. */
static int
ir_defines(
	const struct drv_gpu_shader_ir_inst *instruction)
{
	/* The operations that define nothing, and the samples. */
	switch (instruction->op) {
	case DRV_GPU_IR_SAMPLE:
	case DRV_GPU_IR_SAMPLE_BIAS:
	case DRV_GPU_IR_SAMPLE_LOD:
	case DRV_GPU_IR_TEXTURE:
	case DRV_GPU_IR_IMAGE_READ:
		return 4;
	case DRV_GPU_IR_IMAGE_WRITE:
	case DRV_GPU_IR_NOP:
	case DRV_GPU_IR_SKIP_BEGIN:
	case DRV_GPU_IR_SKIP_END:
	case DRV_GPU_IR_STORE_STORAGE:
	case DRV_GPU_IR_STORE_SHARED:
	case DRV_GPU_IR_BARRIER:
	case DRV_GPU_IR_FENCE:
	case DRV_GPU_IR_LOOP_BEGIN:
	case DRV_GPU_IR_LOOP_END:
	case DRV_GPU_IR_STORE_OUTPUT:
	case DRV_GPU_IR_KILL:
		return 0;
	default:
		return 1;
	}
}

/*
 * Checks the program and prepares its lowering: the last read of every
 * value, the loops' lifetimes, the uniforms' slots and the registers.
 */
static int
prepare(
	struct gfx10_lowering *lowering)
{
	const struct drv_gpu_shader_ir *ir;
	const struct drv_gpu_shader_ir_inst *instruction;
	uint32_t sources[GFX10_MAX_SOURCES];
	uint32_t index;
	uint32_t source;
	int count;
	int defines;
	int error;

	/* Names the module being lowered. */
	ir = lowering->ir;

	/* Compute, vertex and pixel programs are compiled. */
	if (ir->stage != DRV_GPU_STAGE_COMPUTE &&
	    ir->stage != DRV_GPU_STAGE_VERTEX &&
	    ir->stage != DRV_GPU_STAGE_FRAGMENT) {
		error = refuse(lowering, ENOTSUP, "only compute, vertex and fragment shaders are compiled");
		return error;
	}

	/* The per-value arrays. */
	lowering->vgpr = kern_calloc(ir->value_count + 1U, sizeof(*lowering->vgpr));
	if (lowering->vgpr == NULL)
		return ENOMEM;

	/* Allocates the table of each value's last read. */
	lowering->last_use = kern_calloc(ir->value_count + 1U, sizeof(*lowering->last_use));
	if (lowering->last_use == NULL)
		return ENOMEM;

	/* Allocates the table of each value's operand code. */
	lowering->operand_code = kern_calloc(ir->value_count + 1U, sizeof(*lowering->operand_code));
	if (lowering->operand_code == NULL)
		return ENOMEM;

	/* Allocates the table of the values that were moved. */
	lowering->moved = kern_calloc(ir->value_count + 1U, sizeof(*lowering->moved));
	if (lowering->moved == NULL)
		return ENOMEM;

	/* Allocates the table of the values only a condition reads. */
	lowering->condition_only = kern_calloc(ir->value_count + 1U, sizeof(*lowering->condition_only));
	if (lowering->condition_only == NULL)
		return ENOMEM;

	/* Allocates the table of the instructions that are dropped. */
	lowering->dead = kern_calloc(ir->instruction_count + 1U, sizeof(*lowering->dead));
	if (lowering->dead == NULL)
		return ENOMEM;

	/* VCC_LO holds no Boolean at the start. */
	lowering->vcc_value = GFX10_NOT_DEFINED;

	/* No value has a VGPR or an operand of its own yet. */
	for (index = 0U; index <= ir->value_count; index++) {
		lowering->vgpr[index] = GFX10_NO_REG;
		lowering->operand_code[index] = GFX10_NO_OPERAND;
	}

	/* No output channel has its value or its VGPR yet. */
	for (index = 0U; index < GFX10_OUTPUT_CHANNELS; index++) {
		lowering->output_value[index] = GFX10_NOT_DEFINED;
		lowering->output_reg[index] = GFX10_NO_REG;
	}

	/* Each instruction: covered, its values in range. */
	for (index = 0U; index < ir->instruction_count; index++) {
		instruction = &ir->instructions[index];
		lowering->index = index;

		/* The sources of an operation the compiler covers. */
		count = ir_sources(instruction, sources);
		if (count < 0) {
			error = refuse(lowering, ENOTSUP, "the operation is not compiled yet");
			return error;
		}

		/* The destination is a value of the program. */
		defines = ir_defines(instruction);
		if (defines != 0 && (instruction->dst >= ir->value_count || ir->value_count - instruction->dst < (uint32_t)defines)) {
			error = refuse(lowering, EINVAL, "a destination is not a value of the program");
			return error;
		}

		/* A MOVE's destination always lives in a VGPR. */
		if (instruction->op == DRV_GPU_IR_MOVE)
			lowering->moved[instruction->dst] = 1U;

		/* Each source is a value of the program. */
		for (source = 0U; source < (uint32_t)count; source++) {
			if (sources[source] >= ir->value_count) {
				error = refuse(lowering, EINVAL, "a source is not a value of the program");
				return error;
			}
		}
	}

	/* The instructions whose results nothing reads. */
	error = find_dead(lowering);
	if (error != 0)
		return error;

	/* Each value's last read by an instruction that is lowered. */
	for (index = 0U; index < ir->instruction_count; index++) {
		if (lowering->dead[index] != 0U)
			continue;
		count = ir_sources(&ir->instructions[index], sources);
		for (source = 0U; count > 0 && source < (uint32_t)count; source++)
			lowering->last_use[sources[source]] = index + 1U;
	}

	/* Values read inside a loop that were made before it live to the loop's end. */
	error = extend_loops(lowering);
	if (error != 0)
		return error;

	/* A graphics program's inputs and outputs. */
	error = prepare_graphics(lowering);
	if (error != 0)
		return error;

	/* The uniforms' slots and the resource table. */
	error = map_uniforms(lowering);
	if (error != 0)
		return error;

	/* The outputs exported where their values live. */
	error = plan_outputs(lowering);
	if (error != 0)
		return error;

	/* The registers the inputs, the descriptors and the lowering keep. */
	error = plan_registers(lowering);
	if (error != 0)
		return error;

	/* The Booleans that live in VCC_LO alone. */
	plan_conditions(lowering);

	/* How long a vertex program's inputs are read. */
	plan_input_lifetimes(lowering);

	/* The program's interface. */
	lowering->program->local_size[0] = ir->local_size[0];
	lowering->program->local_size[1] = ir->local_size[1];
	lowering->program->local_size[2] = ir->local_size[2];
	if (lowering->program->primitive_culling == 0U)
		lowering->program->shared_bytes = ir->shared_bytes;
	lowering->program->uses_barrier = ir->uses_barrier;

	/* Succeeded: the program can be lowered. */
	lowering->index = 0U;
	return 0;
}

/*
 * Finds the instructions not to lower: those that only define values,
 * none of which any instruction still lowered reads.  Walking backwards,
 * a dropped instruction's own reads no longer count, so a chain that only
 * fed it goes too.  An atomic always runs.
 */
static int
find_dead(
	struct gfx10_lowering *lowering)
{
	const struct drv_gpu_shader_ir *ir;
	const struct drv_gpu_shader_ir_inst *instruction;
	uint32_t sources[GFX10_MAX_SOURCES];
	uint32_t *reads;
	uint32_t index;
	uint32_t source;
	uint32_t channel;
	int count;
	int defines;
	int unread;

	/* Names the module being scanned. */
	ir = lowering->ir;

	/* How many instructions read each value. */
	reads = kern_calloc(ir->value_count + 1U, sizeof(*reads));
	if (reads == NULL)
		return ENOMEM;
	for (index = 0U; index < ir->instruction_count; index++) {
		count = ir_sources(&ir->instructions[index], sources);
		for (source = 0U; count > 0 && source < (uint32_t)count; source++)
			reads[sources[source]]++;
	}

	/* From the last instruction back: one whose values nobody reads is dropped, with its reads. */
	for (index = ir->instruction_count; index > 0U; index--) {
		instruction = &ir->instructions[index - 1U];
		defines = ir_defines(instruction);
		if (defines == 0 || instruction->op == DRV_GPU_IR_ATOMIC)
			continue;

		/* Every value it defines is unread. */
		unread = 1;
		for (channel = 0U; channel < (uint32_t)defines; channel++) {
			if (reads[instruction->dst + channel] != 0U)
				unread = 0;
		}

		/* An instruction with a value somebody reads stays. */
		if (unread == 0)
			continue;

		/* The instruction goes, and its sources lose a reader. */
		lowering->dead[index - 1U] = 1U;
		count = ir_sources(instruction, sources);
		for (source = 0U; count > 0 && source < (uint32_t)count; source++)
			reads[sources[source]]--;
	}

	/* Releases the read counts. */
	kern_free(reads);

	/* Succeeded: the dropped instructions are marked. */
	return 0;
}

/*
 * Makes every value a loop's body reads that was defined before the loop
 * live to the loop's end, so that the next pass still finds it.
 */
static int
extend_loops(
	struct gfx10_lowering *lowering)
{
	const struct drv_gpu_shader_ir *ir;
	const struct drv_gpu_shader_ir_inst *instruction;
	uint32_t *first_definition;
	uint32_t starts[GFX10_LOOP_DEPTH];
	uint32_t sources[GFX10_MAX_SOURCES];
	uint32_t depth;
	uint32_t start;
	uint32_t index;
	uint32_t inner;
	uint32_t source;
	int defines;
	int count;
	int error;

	/* Names the module being scanned. */
	ir = lowering->ir;

	/* Each value's first definition. */
	first_definition = kern_calloc(ir->value_count + 1U, sizeof(*first_definition));
	if (first_definition == NULL)
		return ENOMEM;

	/* No value is defined before the instructions are read. */
	for (index = 0U; index <= ir->value_count; index++)
		first_definition[index] = GFX10_NOT_DEFINED;

	/* The first instruction that defines each value. */
	for (index = 0U; index < ir->instruction_count; index++) {
		instruction = &ir->instructions[index];
		defines = ir_defines(instruction);
		for (inner = 0U; inner < (uint32_t)defines; inner++) {
			if (first_definition[instruction->dst + inner] == GFX10_NOT_DEFINED)
				first_definition[instruction->dst + inner] = index;
		}
	}

	/* Each loop, at its end: the values its body reads that came before it live to the end. */
	depth = 0U;
	for (index = 0U; index < ir->instruction_count; index++) {
		instruction = &ir->instructions[index];
		lowering->index = index;

		/* A loop's start is remembered. */
		if (instruction->op == DRV_GPU_IR_LOOP_BEGIN) {
			if (depth == GFX10_LOOP_DEPTH) {
				kern_free(first_definition);
				error = refuse(lowering, ENOTSUP, "loops nested deeper than the compiler follows");
				return error;
			}

			/* Remembers where the loop starts. */
			starts[depth] = index;
			depth++;
			continue;
		}

		/* Anything but a loop's end is passed. */
		if (instruction->op != DRV_GPU_IR_LOOP_END)
			continue;

		/* An end without a start is malformed. */
		if (depth == 0U) {
			kern_free(first_definition);
			error = refuse(lowering, EINVAL, "a loop's end without its beginning");
			return error;
		}

		/* The loop is closed: its start is taken back. */
		depth--;
		start = starts[depth];

		/* The body's reads of earlier values, by the instructions that are lowered. */
		for (inner = start + 1U; inner <= index; inner++) {
			if (lowering->dead[inner] != 0U)
				continue;
			count = ir_sources(&ir->instructions[inner], sources);
			for (source = 0U; count > 0 && source < (uint32_t)count; source++) {
				if (first_definition[sources[source]] >= start)
					continue;
				if (lowering->last_use[sources[source]] < index + 1U)
					lowering->last_use[sources[source]] = index + 1U;
			}
		}
	}

	/* Releases the first-definition table. */
	kern_free(first_definition);

	/* Every loop must end. */
	if (depth != 0U) {
		error = refuse(lowering, EINVAL, "a loop without its end");
		return error;
	}

	/* Succeeded: the values live through their loops. */
	return 0;
}

/*
 * Gives each uniform its slot and lays out the resource table: the push
 * constants, then the storage buffers' descriptors, then the uniform
 * buffers'.
 */
static int
map_uniforms(
	struct gfx10_lowering *lowering)
{
	const struct drv_gpu_shader_ir *ir;
	const struct drv_gpu_shader_ir_uniform *uniform;
	struct amdrdna_gfx10_program *program;
	uint32_t table;
	uint32_t index;
	int error;

	/* Names the module and the program being filled. */
	ir = lowering->ir;
	program = lowering->program;

	/* One slot per uniform. */
	lowering->slot = kern_calloc(ir->uniform_count + 1U, sizeof(*lowering->slot));
	if (lowering->slot == NULL)
		return ENOMEM;

	/* The push constants come first, rounded to a descriptor's 16 bytes. */
	program->push_bytes = ir->push_bytes;
	table = (ir->push_bytes + 15U) & ~15U;

	/* The storage buffers in order. */
	for (index = 0U; index < ir->uniform_count; index++) {
		uniform = &ir->uniforms[index];
		lowering->slot[index] = GFX10_NO_SLOT;
		if (uniform->kind != DRV_GPU_IR_UNIFORM_STORAGE)
			continue;

		/* The table holds a fixed number of storage buffers. */
		if (program->storage_used == AMDRDNA_GFX10_STORAGE_MAX) {
			error = refuse(lowering, ENOTSUP, "more storage buffers than the table holds");
			return error;
		}

		/* The buffer's descriptor takes the next 16 bytes. */
		lowering->slot[index] = program->storage_used;
		program->storage[program->storage_used].set = uniform->set;
		program->storage[program->storage_used].binding = uniform->binding;
		program->storage[program->storage_used].element = uniform->element;
		program->storage[program->storage_used].table_offset = table;
		program->storage[program->storage_used].size = 0U;
		program->storage_used++;
		table += AMDRDNA_GFX10_BUFFER_DESCRIPTOR_BYTES;
	}

	/* The uniform buffers in order. */
	for (index = 0U; index < ir->uniform_count; index++) {
		uniform = &ir->uniforms[index];
		if (uniform->kind != DRV_GPU_IR_UNIFORM_BLOCK)
			continue;

		/* The table holds a fixed number of uniform buffers. */
		if (program->uniform_used == AMDRDNA_GFX10_UNIFORM_MAX) {
			error = refuse(lowering, ENOTSUP, "more uniform buffers than the table holds");
			return error;
		}

		/* The buffer's descriptor takes the next 16 bytes. */
		lowering->slot[index] = program->uniform_used;
		program->uniforms[program->uniform_used].set = uniform->set;
		program->uniforms[program->uniform_used].binding = uniform->binding;
		program->uniforms[program->uniform_used].element = uniform->element;
		program->uniforms[program->uniform_used].table_offset = table;
		program->uniforms[program->uniform_used].size = uniform->offset + uniform->size;
		program->uniform_used++;
		table += AMDRDNA_GFX10_BUFFER_DESCRIPTOR_BYTES;
	}

	/* The sampled images in order: the image descriptor, then the sampler. */
	for (index = 0U; index < ir->uniform_count; index++) {
		uniform = &ir->uniforms[index];
		if (uniform->kind != DRV_GPU_IR_UNIFORM_SAMPLED_IMAGE)
			continue;

		/* The table holds a fixed number of sampled images. */
		if (program->texture_used == AMDRDNA_GFX10_TEXTURE_MAX) {
			error = refuse(lowering, ENOTSUP, "more sampled images than the table holds");
			return error;
		}

		/* The two descriptors take the next 48 bytes. */
		lowering->slot[index] = program->texture_used;
		program->textures[program->texture_used].set = uniform->set;
		program->textures[program->texture_used].binding = uniform->binding;
		program->textures[program->texture_used].element = uniform->element;
		program->textures[program->texture_used].table_offset = table;
		program->textures[program->texture_used].size = 0U;
		texture_shape(lowering, uniform, &program->textures[program->texture_used]);
		program->texture_used++;
		table += AMDRDNA_GFX10_IMAGE_DESCRIPTOR_BYTES + AMDRDNA_GFX10_SAMPLER_DESCRIPTOR_BYTES;
	}

	/* The storage images and storage texel buffers in order: an image descriptor's place each. */
	for (index = 0U; index < ir->uniform_count; index++) {
		uniform = &ir->uniforms[index];
		if (uniform->kind != DRV_GPU_IR_UNIFORM_STORAGE_IMAGE && uniform->kind != DRV_GPU_IR_UNIFORM_TEXEL_BUFFER)
			continue;

		/* The table holds a fixed number of storage images. */
		if (program->image_used == AMDRDNA_GFX10_IMAGE_MAX) {
			error = refuse(lowering, ENOTSUP, "more storage images than the table holds");
			return error;
		}

		/* The descriptor takes the next 32 bytes (a texel buffer's the first 16 of them). */
		lowering->slot[index] = program->image_used;
		program->images[program->image_used].set = uniform->set;
		program->images[program->image_used].binding = uniform->binding;
		program->images[program->image_used].element = uniform->element;
		program->images[program->image_used].table_offset = table;
		program->images[program->image_used].size = 0U;
		program->images[program->image_used].texel_buffer = uniform->kind == DRV_GPU_IR_UNIFORM_TEXEL_BUFFER ? 1U : 0U;
		program->images[program->image_used].multisampled = 0U;
		program->image_used++;
		table += AMDRDNA_GFX10_IMAGE_DESCRIPTOR_BYTES;
	}

	/* A vertex program's vertex buffer descriptors, one per location up to the highest fetched. */
	if (lowering->ir->stage == DRV_GPU_STAGE_VERTEX && program->fetched_locations != 0U) {
		program->vertex_table_offset = table;
		for (index = 0U; index < AMDRDNA_GFX10_LOCATIONS; index++) {
			if ((program->fetched_locations >> index) != 0U)
				table += AMDRDNA_GFX10_BUFFER_DESCRIPTOR_BYTES;
		}
	}

	/* Succeeded: the table's size is known. */
	program->table_bytes = table;
	return 0;
}

/*
 * Records how the program reads a sampled image, from the shape of the
 * texture instructions that name it: as a texel buffer, or as a
 * multisampled image.  An image no instruction reads is neither.
 */
static void
texture_shape(
	const struct gfx10_lowering *lowering,
	const struct drv_gpu_shader_ir_uniform *uniform,
	struct amdrdna_gfx10_binding *binding)
{
	const struct drv_gpu_shader_ir_inst *instruction;
	uint32_t index;

	/* Each texture instruction that names the image. */
	binding->texel_buffer = 0U;
	binding->multisampled = 0U;
	for (index = 0U; index < lowering->ir->instruction_count; index++) {
		instruction = &lowering->ir->instructions[index];
		if (instruction->op != DRV_GPU_IR_TEXTURE ||
		    instruction->location != uniform->set ||
		    instruction->immediate != uniform->binding ||
		    instruction->element != uniform->element)
			continue;

		/* The shape says how the image is read. */
		if ((instruction->image_shape & DRV_GPU_IR_IMAGE_KIND_MASK) == DRV_GPU_IR_IMAGE_BUFFER)
			binding->texel_buffer = 1U;
		if ((instruction->image_shape & DRV_GPU_IR_IMAGE_MULTISAMPLED) != 0U)
			binding->multisampled = 1U;
	}
}

/*
 * Places the descriptors of the program's resources in SGPRs from *sgpr,
 * all of them loaded at the start when they fit with what the program
 * keeps besides (the alive lanes, deepest loop mask saves, the guard's
 * save, the scalar temporaries, a culling program's masks and the uniform
 * words' cache).  When they do not, a window takes the largest descriptor
 * and the descriptors that do not fit any more are loaded into it before
 * each instruction that uses them: the vertex buffers' first (a vertex
 * program fetches them all at the start, so they always are loaded
 * there), then the storage buffers', the uniform buffers', the sampled
 * images' and the storage images' in order.  Moves *sgpr past them.  Returns 0, or ENOTSUP when
 * not even the vertex buffers' descriptors fit.
 */
static int
plan_descriptors(
	struct gfx10_lowering *lowering,
	uint32_t deepest,
	uint32_t *sgpr)
{
	struct amdrdna_gfx10_program *program;
	uint32_t index;
	uint32_t vertex_count;
	uint32_t needed;
	uint32_t kept;
	uint32_t room;
	uint32_t next;
	int error;

	/* Names the program being laid out. */
	program = lowering->program;
	next = *sgpr;

	/* What the program keeps besides the descriptors, and the uniform words' cache (aligned, with its spare chunk). */
	kept = deepest + 1U + GFX10_SCALAR_TEMPS + 3U + 4U * (GFX10_CHUNK_CACHE + 1U);
	if (program->discards != 0U || lowering->whole_quads != 0)
		kept++;
	if (program->primitive_culling != 0U)
		kept += GFX10_CULL_SGPRS;

	/* What every descriptor loaded at the start takes. */
	vertex_count = 0U;
	for (index = 0U; index < AMDRDNA_GFX10_LOCATIONS; index++) {
		if ((program->fetched_locations & (1U << index)) != 0U)
			vertex_count++;
	}
	needed = 4U * (vertex_count + program->storage_used + program->uniform_used);
	needed += (GFX10_IMAGE_SGPRS + GFX10_SAMPLER_SGPRS) * program->texture_used;
	needed += GFX10_IMAGE_SGPRS * program->image_used;

	/* All of them at the start when they fit, else the window first. */
	lowering->descriptor_window = GFX10_NOT_DEFINED;
	room = AMDRDNA_GFX10_SGPR_COUNT;
	if (kept < room)
		room -= kept;
	else
		room = 0U;
	if (next + needed > room) {
		lowering->descriptor_window = next;
		next += GFX10_WINDOW_SGPRS;
	}

	/* Each fetched vertex location's descriptor, always at the start. */
	for (index = 0U; index < AMDRDNA_GFX10_LOCATIONS; index++) {
		if ((program->fetched_locations & (1U << index)) == 0U)
			continue;
		if (next + 4U > room) {
			error = refuse(lowering, ENOTSUP, "the vertex buffers' descriptors need more SGPRs than there are");
			return error;
		}
		lowering->vertex_sgpr[index] = next;
		next += 4U;
	}

	/* Each storage buffer's descriptor, at the start while there is room. */
	for (index = 0U; index < program->storage_used; index++) {
		lowering->storage_sgpr[index] = GFX10_DESCRIPTOR_AT_USE;
		if (next + 4U > room)
			continue;
		lowering->storage_sgpr[index] = next;
		next += 4U;
	}

	/* Each uniform buffer's descriptor, likewise. */
	for (index = 0U; index < program->uniform_used; index++) {
		lowering->uniform_sgpr[index] = GFX10_DESCRIPTOR_AT_USE;
		if (next + 4U > room)
			continue;
		lowering->uniform_sgpr[index] = next;
		next += 4U;
	}

	/* Each texture's image descriptor and sampler, likewise. */
	for (index = 0U; index < program->texture_used; index++) {
		lowering->image_sgpr[index] = GFX10_DESCRIPTOR_AT_USE;
		lowering->sampler_sgpr[index] = GFX10_DESCRIPTOR_AT_USE;
		if (next + GFX10_IMAGE_SGPRS + GFX10_SAMPLER_SGPRS > room)
			continue;
		lowering->image_sgpr[index] = next;
		next += GFX10_IMAGE_SGPRS;
		lowering->sampler_sgpr[index] = next;
		next += GFX10_SAMPLER_SGPRS;
	}

	/* Each storage image's descriptor, likewise. */
	for (index = 0U; index < program->image_used; index++) {
		lowering->storage_image_sgpr[index] = GFX10_DESCRIPTOR_AT_USE;
		if (next + GFX10_IMAGE_SGPRS > room)
			continue;
		lowering->storage_image_sgpr[index] = next;
		next += GFX10_IMAGE_SGPRS;
	}

	/* Succeeded: the descriptors are placed. */
	*sgpr = next;
	return 0;
}

/*
 * Reports the first SGPR of a storage buffer's descriptor: where the
 * start loaded it, or the window, into which it is loaded now (the
 * instruction that uses it must come next).
 */
static uint32_t
storage_descriptor(
	struct gfx10_lowering *lowering,
	uint32_t slot)
{
	/* Loaded at the start. */
	if (lowering->storage_sgpr[slot] != GFX10_DESCRIPTOR_AT_USE)
		return lowering->storage_sgpr[slot];

	/* Loaded into the window from the resource table. */
	emit_scalar_load(lowering,
			 AMDRDNA_GFX10_S_LOAD_DWORDX4,
			 lowering->descriptor_window,
			 AMDRDNA_GFX10_ABI_TABLE,
			 lowering->program->storage[slot].table_offset,
			 4U);
	return lowering->descriptor_window;
}

/* Reports the first SGPR of a uniform buffer's descriptor, as storage_descriptor() does a storage buffer's. */
static uint32_t
uniform_descriptor(
	struct gfx10_lowering *lowering,
	uint32_t slot)
{
	/* Loaded at the start. */
	if (lowering->uniform_sgpr[slot] != GFX10_DESCRIPTOR_AT_USE)
		return lowering->uniform_sgpr[slot];

	/* Loaded into the window from the resource table. */
	emit_scalar_load(lowering,
			 AMDRDNA_GFX10_S_LOAD_DWORDX4,
			 lowering->descriptor_window,
			 AMDRDNA_GFX10_ABI_TABLE,
			 lowering->program->uniforms[slot].table_offset,
			 4U);
	return lowering->descriptor_window;
}

/* Reports the first SGPRs of a sampled image's descriptor and of its sampler, as storage_descriptor() does a storage buffer's. */
static void
texture_descriptors(
	struct gfx10_lowering *lowering,
	uint32_t slot,
	uint32_t *image,
	uint32_t *sampler)
{
	/* Loaded at the start. */
	if (lowering->image_sgpr[slot] != GFX10_DESCRIPTOR_AT_USE) {
		*image = lowering->image_sgpr[slot];
		*sampler = lowering->sampler_sgpr[slot];
		return;
	}

	/* Both loaded into the window from the resource table, the sampler after the image. */
	emit_scalar_load(lowering,
			 AMDRDNA_GFX10_S_LOAD_DWORDX8,
			 lowering->descriptor_window,
			 AMDRDNA_GFX10_ABI_TABLE,
			 lowering->program->textures[slot].table_offset,
			 GFX10_IMAGE_SGPRS);
	emit_scalar_load(lowering,
			 AMDRDNA_GFX10_S_LOAD_DWORDX4,
			 lowering->descriptor_window + GFX10_IMAGE_SGPRS,
			 AMDRDNA_GFX10_ABI_TABLE,
			 lowering->program->textures[slot].table_offset + AMDRDNA_GFX10_IMAGE_DESCRIPTOR_BYTES,
			 GFX10_SAMPLER_SGPRS);
	*image = lowering->descriptor_window;
	*sampler = lowering->descriptor_window + GFX10_IMAGE_SGPRS;
}

/*
 * Reports the first SGPR of a storage image's (or storage texel buffer's)
 * descriptor, as storage_descriptor() does a storage buffer's.
 */
static uint32_t
storage_image_descriptor(
	struct gfx10_lowering *lowering,
	uint32_t slot)
{
	/* Loaded at the start. */
	if (lowering->storage_image_sgpr[slot] != GFX10_DESCRIPTOR_AT_USE)
		return lowering->storage_image_sgpr[slot];

	/* Loaded into the window from the resource table. */
	emit_scalar_load(lowering,
			 AMDRDNA_GFX10_S_LOAD_DWORDX8,
			 lowering->descriptor_window,
			 AMDRDNA_GFX10_ABI_TABLE,
			 lowering->program->images[slot].table_offset,
			 GFX10_IMAGE_SGPRS);
	return lowering->descriptor_window;
}

/*
 * Places the registers the program keeps for its whole run: the inputs the
 * hardware delivers, the descriptors, the loop and guard mask saves and the
 * scalar temporaries.
 */
static int
plan_registers(
	struct gfx10_lowering *lowering)
{
	struct amdrdna_gfx10_program *program;
	const struct drv_gpu_shader_ir_inst *instruction;
	uint32_t sgpr;
	uint32_t index;
	uint32_t depth;
	uint32_t deepest;
	uint32_t reg;
	int error;

	/* Names the program being laid out. */
	program = lowering->program;
	error = 0;

	/* The inputs the hardware delivers, by the stage; the compiler's own SGPRs start after them. */
	lowering->sgpr_first_free = GFX10_SGPR_FIRST_FREE;
	if (lowering->ir->stage == DRV_GPU_STAGE_COMPUTE) {
		/* The table address and the workgroup id in s0 .. s4, the local ids in v0 .. v2. */
		program->user_sgprs = AMDRDNA_GFX10_ABI_USER_SGPRS;
		lowering->vgpr_first = GFX10_CS_INPUT_VGPRS;
	} else if (lowering->ir->stage == DRV_GPU_STAGE_VERTEX && program->primitive_shader != 0U) {
		/*
		 * The table in s0 .. s1, the counts in s2 and s3, the base vertex,
		 * the first instance and the instance mask in s8 .. s10; v0 .. v8,
		 * taken until the start has read them.
		 */
		program->user_sgprs = AMDRDNA_GFX10_NGG_USER_SGPRS;
		if (program->primitive_culling != 0U)
			program->user_sgprs = AMDRDNA_GFX10_NGG_CULL_USER_SGPRS;
		lowering->base_vertex_sgpr = AMDRDNA_GFX10_NGG_BASE_VERTEX;
		lowering->first_instance_sgpr = AMDRDNA_GFX10_NGG_FIRST_INSTANCE;
		lowering->instance_mask_sgpr = AMDRDNA_GFX10_NGG_INSTANCE_MASK;
		lowering->vertex_id_input = AMDRDNA_GFX10_NGG_VERTEX_ID_VGPR;
		lowering->instance_id_input = AMDRDNA_GFX10_NGG_INSTANCE_ID_VGPR;
		lowering->hardware_vgprs = AMDRDNA_GFX10_NGG_INPUT_VGPRS;
		lowering->sgpr_first_free = GFX10_SGPR_FIRST_FREE_NGG;
		if (program->primitive_culling != 0U)
			lowering->sgpr_first_free = GFX10_SGPR_FIRST_FREE_CULL;
		lowering->vgpr_first = 0U;
		for (index = 0U; index < lowering->hardware_vgprs; index++)
			lowering->taken[index] = 1U;
	} else if (lowering->ir->stage == DRV_GPU_STAGE_VERTEX) {
		/*
		 * The table, the base vertex, the first instance and the instance
		 * mask in s0 .. s4; v0 .. v3, taken until the start has read them.
		 */
		program->user_sgprs = AMDRDNA_GFX10_ABI_USER_SGPRS_VS;
		lowering->base_vertex_sgpr = AMDRDNA_GFX10_ABI_BASE_VERTEX;
		lowering->first_instance_sgpr = AMDRDNA_GFX10_ABI_FIRST_INSTANCE;
		lowering->instance_mask_sgpr = AMDRDNA_GFX10_ABI_INSTANCE_MASK;
		lowering->vertex_id_input = GFX10_VS_VERTEX_ID;
		lowering->instance_id_input = GFX10_VS_INSTANCE_ID;
		lowering->hardware_vgprs = GFX10_VS_INPUT_VGPRS;
		lowering->vgpr_first = 0U;
		for (index = 0U; index < lowering->hardware_vgprs; index++)
			lowering->taken[index] = 1U;
	} else {
		/* The table in s0 .. s1, the primitive mask in s2; the enabled inputs packed from v0. */
		program->user_sgprs = AMDRDNA_GFX10_ABI_USER_SGPRS;
		lowering->vgpr_first = 0U;
		lowering->perspective_vgpr = reserve_vgprs(lowering, 2U);
		if ((program->pixel_inputs & GFX10_PS_LINEAR_CENTER) != 0U)
			lowering->screen_linear_vgpr = reserve_vgprs(lowering, 2U);
		if ((program->pixel_inputs & GFX10_PS_POSITION) != 0U)
			lowering->position_input_vgpr = reserve_vgprs(lowering, 4U);
		if ((program->pixel_inputs & GFX10_PS_FRONT_FACE) != 0U)
			lowering->face_vgpr = reserve_vgprs(lowering, 1U);
		if ((program->pixel_inputs & GFX10_PS_SAMPLE_COVERAGE) != 0U)
			lowering->coverage_vgpr = reserve_vgprs(lowering, 1U);
	}

	/* The registers reserved so far are the highest in use. */
	lowering->vgpr_highest = lowering->vgpr_first;

	/* A vertex program takes its vertex and instance numbers and fetched locations until their last read. */
	if (lowering->ir->stage == DRV_GPU_STAGE_VERTEX) {
		lowering->vgpr_highest = lowering->hardware_vgprs;

		/* The primitive number, copied at the start into the register its export reads. */
		if (program->exports_primitive_id != 0U) {
			error = take_vgpr(lowering, &reg);
			if (error != 0)
				return error;
			lowering->output_reg[GFX10_OUTPUT_LOCATIONS + 4U * AMDRDNA_GFX10_PRIMITIVE_ID_PARAMETER] = (uint16_t)reg;
		}

		/* Takes the register the vertex number arrives in. */
		error = take_vgpr(lowering, &lowering->vertex_index_vgpr);
		if (error != 0)
			return error;
		error = take_vgpr(lowering, &lowering->instance_index_vgpr);
		if (error != 0)
			return error;
		for (index = 0U; index < AMDRDNA_GFX10_LOCATIONS; index++) {
			if ((program->fetched_locations & (1U << index)) == 0U)
				continue;
			error = take_vgpr_block(lowering, 4U, &lowering->input_vgpr[index]);
			if (error != 0)
				return error;
		}
	}

	/* The deepest loop nest: one lane mask save per depth. */
	depth = 0U;
	deepest = 0U;
	for (index = 0U; index < lowering->ir->instruction_count; index++) {
		instruction = &lowering->ir->instructions[index];
		if (instruction->op == DRV_GPU_IR_LOOP_BEGIN) {
			depth++;
			if (depth > deepest)
				deepest = depth;
		} else if (instruction->op == DRV_GPU_IR_LOOP_END && depth != 0U) {
			depth--;
		}
	}

	/* The descriptors, four SGPRs each from a multiple of four, all loaded at the start when they fit. */
	sgpr = lowering->sgpr_first_free;
	error = plan_descriptors(lowering, deepest, &sgpr);
	if (error != 0)
		return error;

	/* A pixel program that discards or runs whole quads keeps the lanes still alive. */
	if (program->discards != 0U || lowering->whole_quads != 0) {
		lowering->live_sgpr = sgpr;
		sgpr++;
	}

	/* Each nesting level of loops saves the lane mask in a register. */
	for (index = 0U; index < deepest; index++) {
		lowering->loop_save[index] = sgpr;
		sgpr++;
	}

	/* The guard's lane mask save and the scalar temporaries. */
	lowering->guard_save = sgpr;
	sgpr++;
	for (index = 0U; index < GFX10_SCALAR_TEMPS; index++) {
		lowering->scalar_temp[index] = sgpr;
		sgpr++;
	}

	/* A culling primitive shader's lane masks. */
	if (program->primitive_culling != 0U) {
		for (index = 0U; index < GFX10_CULL_SGPRS; index++) {
			lowering->cull_sgpr[index] = sgpr;
			sgpr++;
		}
	}

	/* Everything fits below VCC. */
	if (sgpr > AMDRDNA_GFX10_SGPR_COUNT) {
		error = refuse(lowering, ENOTSUP, "the descriptors and the loops need more SGPRs than there are");
		return error;
	}

	/* The uniform words loaded once at the start, as many as the SGPRs left hold, and the cache for the others. */
	plan_uniforms(lowering, &sgpr);
	if (sgpr > AMDRDNA_GFX10_SGPR_COUNT) {
		error = refuse(lowering, ENOTSUP, "the descriptors and the loops leave no SGPRs for the uniform words");
		return error;
	}

	/* Succeeded: the registers are placed. */
	lowering->sgpr_highest = sgpr;
	return 0;
}

/*
 * Loads what every lane shares: the descriptors first, then a vertex
 * program's fetches (which wait only for the descriptors), then the
 * uniform words loaded once; a pixel program points M0 at its primitive's
 * attributes and keeps its alive lanes.
 */
static int
prologue(
	struct gfx10_lowering *lowering)
{
	struct amdrdna_gfx10_program *program;
	uint32_t index;
	uint32_t location;
	uint32_t descriptor;

	/* Names the program being set up. */
	program = lowering->program;

	/* A primitive shader first has its space given and its primitives exported. */
	if (program->primitive_shader != 0U)
		prologue_primitive(lowering);

	/* Each storage buffer's descriptor loaded at the start. */
	for (index = 0U; index < program->storage_used; index++) {
		if (lowering->storage_sgpr[index] == GFX10_DESCRIPTOR_AT_USE)
			continue;
		emit_scalar_load(lowering,
				 AMDRDNA_GFX10_S_LOAD_DWORDX4,
				 lowering->storage_sgpr[index],
				 AMDRDNA_GFX10_ABI_TABLE,
				 program->storage[index].table_offset,
				 4U);
	}

	/* Each uniform buffer's descriptor loaded at the start. */
	for (index = 0U; index < program->uniform_used; index++) {
		if (lowering->uniform_sgpr[index] == GFX10_DESCRIPTOR_AT_USE)
			continue;
		emit_scalar_load(lowering,
				 AMDRDNA_GFX10_S_LOAD_DWORDX4,
				 lowering->uniform_sgpr[index],
				 AMDRDNA_GFX10_ABI_TABLE,
				 program->uniforms[index].table_offset,
				 4U);
	}

	/* Each sampled image's descriptor and sampler loaded at the start. */
	for (index = 0U; index < program->texture_used; index++) {
		if (lowering->image_sgpr[index] == GFX10_DESCRIPTOR_AT_USE)
			continue;
		emit_scalar_load(lowering,
				 AMDRDNA_GFX10_S_LOAD_DWORDX8,
				 lowering->image_sgpr[index],
				 AMDRDNA_GFX10_ABI_TABLE,
				 program->textures[index].table_offset,
				 GFX10_IMAGE_SGPRS);
		emit_scalar_load(lowering,
				 AMDRDNA_GFX10_S_LOAD_DWORDX4,
				 lowering->sampler_sgpr[index],
				 AMDRDNA_GFX10_ABI_TABLE,
				 program->textures[index].table_offset + AMDRDNA_GFX10_IMAGE_DESCRIPTOR_BYTES,
				 GFX10_SAMPLER_SGPRS);
	}

	/* Each storage image's descriptor loaded at the start. */
	for (index = 0U; index < program->image_used; index++) {
		if (lowering->storage_image_sgpr[index] == GFX10_DESCRIPTOR_AT_USE)
			continue;
		emit_scalar_load(lowering,
				 AMDRDNA_GFX10_S_LOAD_DWORDX8,
				 lowering->storage_image_sgpr[index],
				 AMDRDNA_GFX10_ABI_TABLE,
				 program->images[index].table_offset,
				 GFX10_IMAGE_SGPRS);
	}

	/* Each fetched location's vertex buffer descriptor. */
	for (location = 0U; location < AMDRDNA_GFX10_LOCATIONS; location++) {
		if ((program->fetched_locations & (1U << location)) == 0U)
			continue;
		emit_scalar_load(lowering,
				 AMDRDNA_GFX10_S_LOAD_DWORDX4,
				 lowering->vertex_sgpr[location],
				 AMDRDNA_GFX10_ABI_TABLE,
				 program->vertex_table_offset + AMDRDNA_GFX10_BUFFER_DESCRIPTOR_BYTES * location,
				 4U);
	}

	/* A vertex program fetches its inputs. */
	if (lowering->ir->stage == DRV_GPU_STAGE_VERTEX)
		prologue_vertex(lowering);

	/* The uniform words used, 16 bytes at a time: the push constants from the table, the others through their buffer. */
	for (index = 0U; index < lowering->chunk_count; index++) {
		if (lowering->chunk_slot[index] == GFX10_NO_SLOT) {
			emit_scalar_load(lowering,
					 AMDRDNA_GFX10_S_LOAD_DWORDX4,
					 lowering->chunk_sgpr[index],
					 AMDRDNA_GFX10_ABI_TABLE,
					 lowering->chunk_offset[index],
					 4U);
		} else {
			descriptor = uniform_descriptor(lowering, lowering->chunk_slot[index]);
			emit_scalar_load(lowering,
					 AMDRDNA_GFX10_S_BUFFER_LOAD_DWORDX4,
					 lowering->chunk_sgpr[index],
					 descriptor,
					 lowering->chunk_offset[index],
					 4U);
		}
	}

	/* A pixel program points M0 at its primitive's attributes. */
	if (lowering->ir->stage == DRV_GPU_STAGE_FRAGMENT) {
		emit_scalar(lowering,
			    AMDRDNA_GFX10_KIND_SCALAR_ONE,
			    AMDRDNA_GFX10_S_MOV_B32,
			    AMDRDNA_GFX10_M0,
			    AMDRDNA_GFX10_S(GFX10_PS_PRIM_MASK),
			    0U);

		/* The lanes alive at the start are those the wave runs. */
		if (program->discards != 0U || lowering->whole_quads != 0) {
			emit_scalar(lowering,
				    AMDRDNA_GFX10_KIND_SCALAR_ONE,
				    AMDRDNA_GFX10_S_MOV_B32,
				    lowering->live_sgpr,
				    AMDRDNA_GFX10_EXEC_LO,
				    0U);
		}

		/* A discard may end the wave early. */
		if (program->discards != 0U) {
			lowering->early_end_label = lowering->next_label;
			lowering->next_label++;
		}

		/* Whole quads: every quad with a lane alive runs all four, so the derivatives have their neighbours. */
		if (lowering->whole_quads != 0) {
			emit_scalar(lowering,
				    AMDRDNA_GFX10_KIND_SCALAR_ONE,
				    AMDRDNA_GFX10_S_WQM_B32,
				    AMDRDNA_GFX10_EXEC_LO,
				    AMDRDNA_GFX10_EXEC_LO,
				    0U);
		}
	}

	/*
	 * A vertex program's hardware inputs, and the inputs only the start
	 * read, go back; a culling part keeps its primitive's vertex numbers
	 * (v0 and v1) and the vertex and instance numbers for its end, a
	 * deferred part the primitive's word in v0.
	 */
	if (lowering->ir->stage == DRV_GPU_STAGE_VERTEX) {
		for (index = 0U; index < lowering->hardware_vgprs; index++) {
			if (lowering->part == GFX10_PART_CULL &&
			    (index == AMDRDNA_GFX10_NGG_PRIMITIVE_VGPR || index == AMDRDNA_GFX10_NGG_VERTEX_PAIR_VGPR ||
			     index == AMDRDNA_GFX10_NGG_VERTEX_ID_VGPR || index == AMDRDNA_GFX10_NGG_INSTANCE_ID_VGPR))
				continue;
			if (lowering->part == GFX10_PART_DEFERRED && index == AMDRDNA_GFX10_NGG_PRIMITIVE_VGPR)
				continue;
			give_vgpr(lowering, index);
		}
		return_inputs(lowering, 0U);
	}

	/* A list that could not grow fails the compile. */
	if (lowering->out_of_memory != 0)
		return ENOMEM;

	/* Succeeded: the shared values are on their way. */
	return 0;
}

/* Lowers one IR instruction into dst's VGPR (GFX10_NO_REG when it defines nothing). */
static int
lower(
	struct gfx10_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	unsigned operand;
	int error;

	/* Picks the lowering by the operation. */
	switch (instruction->op) {
	case DRV_GPU_IR_NOP:
	case DRV_GPU_IR_SKIP_BEGIN:
	case DRV_GPU_IR_SKIP_END:
		/* Every block runs, so a skippable block's markers do nothing. */
		error = 0;
		break;
	case DRV_GPU_IR_CONST:
	case DRV_GPU_IR_ICONST:
	case DRV_GPU_IR_BOOL:
		/* The inline operand that stands for the constant, or the literal when none does. */
		operand = constant_operand(instruction->immediate);
		emit_vector_literal(lowering,
				    AMDRDNA_GFX10_KIND_VECTOR_ONE,
				    AMDRDNA_GFX10_V_MOV_B32,
				    AMDRDNA_GFX10_V(dst),
				    operand,
				    0U,
				    instruction->immediate);
		error = 0;
		break;
	case DRV_GPU_IR_MOVE:
		/* The moved value, wherever it lives. */
		operand = value_operand(lowering, instruction->src[0]);
		emit_vector(lowering,
			    AMDRDNA_GFX10_KIND_VECTOR_ONE,
			    AMDRDNA_GFX10_V_MOV_B32,
			    AMDRDNA_GFX10_V(dst),
			    operand,
			    0U,
			    0U);
		error = 0;
		break;
	case DRV_GPU_IR_FADD:
	case DRV_GPU_IR_FSUB:
	case DRV_GPU_IR_FMUL:
	case DRV_GPU_IR_FNEG:
	case DRV_GPU_IR_FABS:
	case DRV_GPU_IR_FMIN:
	case DRV_GPU_IR_FMAX:
	case DRV_GPU_IR_RSQ:
	case DRV_GPU_IR_RCP:
	case DRV_GPU_IR_SQRT:
	case DRV_GPU_IR_EXP2:
	case DRV_GPU_IR_LOG2:
	case DRV_GPU_IR_SIN:
	case DRV_GPU_IR_COS:
	case DRV_GPU_IR_FLOOR:
	case DRV_GPU_IR_FRACT:
	case DRV_GPU_IR_FTRUNC:
	case DRV_GPU_IR_FROUND_EVEN:
	case DRV_GPU_IR_I2F:
	case DRV_GPU_IR_U2F:
	case DRV_GPU_IR_F2I:
	case DRV_GPU_IR_F2U:
	case DRV_GPU_IR_PACK_HALF:
	case DRV_GPU_IR_UNPACK_HALF:
		error = lower_float(lowering, instruction, dst);
		break;
	case DRV_GPU_IR_AND:
	case DRV_GPU_IR_OR:
	case DRV_GPU_IR_NOT:
	case DRV_GPU_IR_IADD:
	case DRV_GPU_IR_ISUB:
	case DRV_GPU_IR_IMUL:
	case DRV_GPU_IR_INEG:
	case DRV_GPU_IR_IAND:
	case DRV_GPU_IR_IOR:
	case DRV_GPU_IR_IXOR:
	case DRV_GPU_IR_INOT:
	case DRV_GPU_IR_SHL:
	case DRV_GPU_IR_SHR:
	case DRV_GPU_IR_ASR:
		error = lower_integer(lowering, instruction, dst);
		break;
	case DRV_GPU_IR_FLT:
	case DRV_GPU_IR_FGE:
	case DRV_GPU_IR_FEQ:
	case DRV_GPU_IR_FNEU:
	case DRV_GPU_IR_ILT:
	case DRV_GPU_IR_IGE:
	case DRV_GPU_IR_ULT:
	case DRV_GPU_IR_UGE:
	case DRV_GPU_IR_IEQ:
	case DRV_GPU_IR_INE:
	case DRV_GPU_IR_SELECT:
		error = lower_compare(lowering, instruction, dst);
		break;
	case DRV_GPU_IR_LOAD_PUSH:
	case DRV_GPU_IR_LOAD_UBO:
		error = lower_uniform(lowering, instruction, dst);
		break;
	case DRV_GPU_IR_LOAD_SYSTEM:
		error = lower_system(lowering, instruction, dst);
		break;
	case DRV_GPU_IR_LOAD_STORAGE:
	case DRV_GPU_IR_STORE_STORAGE:
	case DRV_GPU_IR_STORAGE_SIZE:
		error = lower_storage(lowering, instruction, dst);
		break;
	case DRV_GPU_IR_LOOP_BEGIN:
	case DRV_GPU_IR_LOOP_END:
		error = lower_loop(lowering, instruction);
		break;
	case DRV_GPU_IR_UDIV:
	case DRV_GPU_IR_UMOD:
	case DRV_GPU_IR_IDIV:
	case DRV_GPU_IR_IREM:
		error = lower_divide(lowering, instruction, dst);
		break;
	case DRV_GPU_IR_ATOMIC:
		/* An atomic on shared memory, or on a storage buffer. */
		if (instruction->location == DRV_GPU_IR_LOCATION_SHARED) {
			error = lower_shared(lowering, instruction, dst);
		} else {
			error = lower_atomic(lowering, instruction, dst);
		}

		break;
	case DRV_GPU_IR_LOAD_SHARED:
	case DRV_GPU_IR_STORE_SHARED:
		error = lower_shared(lowering, instruction, dst);
		break;
	case DRV_GPU_IR_BARRIER:
	case DRV_GPU_IR_FENCE:
		lower_sync(lowering, instruction);
		error = 0;
		break;
	case DRV_GPU_IR_LOAD_INPUT:
		error = lower_input(lowering, instruction, dst);
		break;
	case DRV_GPU_IR_STORE_OUTPUT:
		error = lower_output(lowering, instruction);
		break;
	case DRV_GPU_IR_KILL:
		lower_kill(lowering, instruction);
		error = 0;
		break;
	case DRV_GPU_IR_SAMPLE:
	case DRV_GPU_IR_SAMPLE_BIAS:
	case DRV_GPU_IR_SAMPLE_LOD:
	case DRV_GPU_IR_TEXTURE:
		error = lower_sample(lowering, instruction, dst);
		break;
	case DRV_GPU_IR_DDX:
	case DRV_GPU_IR_DDX_FINE:
	case DRV_GPU_IR_DDY:
	case DRV_GPU_IR_DDY_FINE:
		error = lower_derivative(lowering, instruction, dst);
		break;
	case DRV_GPU_IR_IMAGE_READ:
	case DRV_GPU_IR_IMAGE_WRITE:
		error = lower_image(lowering, instruction, dst);
		break;
	default:
		error = refuse(lowering, ENOTSUP, "the operation is not compiled yet");
		break;
	}

	/* Reports the lowering's refusal. */
	if (error != 0)
		return error;

	/* Succeeded: the instruction is lowered. */
	return 0;
}

/* Lowers a float operation, a conversion, or a half-float packing. */
static int
lower_float(
	struct gfx10_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	unsigned a;
	unsigned b;
	unsigned d;
	unsigned opcode;
	unsigned shift;
	uint32_t temp;
	int error;

	/* Decodes the operands the arithmetic reads. */
	a = value_operand(lowering, instruction->src[0]);
	b = 0U;
	if (instruction->op == DRV_GPU_IR_FADD || instruction->op == DRV_GPU_IR_FSUB ||
	    instruction->op == DRV_GPU_IR_FMUL || instruction->op == DRV_GPU_IR_FMIN ||
	    instruction->op == DRV_GPU_IR_FMAX || instruction->op == DRV_GPU_IR_PACK_HALF)
		b = value_operand(lowering, instruction->src[1]);
	d = AMDRDNA_GFX10_V(dst);

	/* Picks the instructions by the operation. */
	switch (instruction->op) {
	case DRV_GPU_IR_FADD:
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_ADD_F32, d, a, b, 0U);
		break;
	case DRV_GPU_IR_FSUB:
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_SUB_F32, d, a, b, 0U);
		break;
	case DRV_GPU_IR_FMUL:
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_MUL_F32, d, a, b, 0U);
		break;
	case DRV_GPU_IR_FMIN:
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_MIN_F32, d, a, b, 0U);
		break;
	case DRV_GPU_IR_FMAX:
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_MAX_F32, d, a, b, 0U);
		break;
	case DRV_GPU_IR_FNEG:
		/* The negation flips the sign bit and nothing else, a NaN included. */
		emit_vector_literal(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_XOR_B32, d, AMDRDNA_GFX10_LITERAL, a, GFX10_SIGN_BIT);
		break;
	case DRV_GPU_IR_FABS:
		/* The absolute value clears the sign bit and nothing else. */
		emit_vector_literal(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_AND_B32, d, AMDRDNA_GFX10_LITERAL, a, GFX10_MAGNITUDE_BITS);
		break;
	case DRV_GPU_IR_SIN:
	case DRV_GPU_IR_COS:
		/* The sine and cosine units take the angle in turns: the radians scaled by 1 / (2 pi) first. */
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_MUL_F32, d, AMDRDNA_GFX10_INV_TWO_PI, a, 0U);
		opcode = AMDRDNA_GFX10_V_SIN_F32;
		if (instruction->op == DRV_GPU_IR_COS)
			opcode = AMDRDNA_GFX10_V_COS_F32;
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_ONE, opcode, d, d, 0U, 0U);
		break;
	case DRV_GPU_IR_FRACT:
		/* The fraction is x - floor(x), the floor taken first into the destination. */
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_ONE, AMDRDNA_GFX10_V_FLOOR_F32, d, a, 0U, 0U);
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_SUB_F32, d, a, d, 0U);
		break;
	case DRV_GPU_IR_PACK_HALF:
		/* Each half is converted on its own, the high one into a temporary, then the two are packed. */
		error = take_vgpr(lowering, &temp);
		if (error != 0)
			return error;
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_ONE, AMDRDNA_GFX10_V_CVT_F16_F32, d, a, 0U, 0U);
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_ONE, AMDRDNA_GFX10_V_CVT_F16_F32, AMDRDNA_GFX10_V(temp), b, 0U, 0U);
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_WIDE, AMDRDNA_GFX10_V_PACK_B32_F16, d, d, AMDRDNA_GFX10_V(temp), 0U);
		give_vgpr(lowering, temp);
		break;
	case DRV_GPU_IR_UNPACK_HALF:
		/* The high half is shifted down first; the conversion reads the low 16 bits. */
		if (instruction->component != 0U) {
			shift = constant_operand(16U);
			emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_LSHRREV_B32, d, shift, a, 0U);
			a = d;
		}

		/* Converts the half to a float. */
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_ONE, AMDRDNA_GFX10_V_CVT_F32_F16, d, a, 0U, 0U);
		break;
	default:
		/* The one-source operations each have one instruction. */
		switch (instruction->op) {
		case DRV_GPU_IR_RSQ:
			opcode = AMDRDNA_GFX10_V_RSQ_F32;
			break;
		case DRV_GPU_IR_RCP:
			opcode = AMDRDNA_GFX10_V_RCP_F32;
			break;
		case DRV_GPU_IR_SQRT:
			opcode = AMDRDNA_GFX10_V_SQRT_F32;
			break;
		case DRV_GPU_IR_EXP2:
			opcode = AMDRDNA_GFX10_V_EXP_F32;
			break;
		case DRV_GPU_IR_LOG2:
			opcode = AMDRDNA_GFX10_V_LOG_F32;
			break;
		case DRV_GPU_IR_FLOOR:
			opcode = AMDRDNA_GFX10_V_FLOOR_F32;
			break;
		case DRV_GPU_IR_FTRUNC:
			opcode = AMDRDNA_GFX10_V_TRUNC_F32;
			break;
		case DRV_GPU_IR_FROUND_EVEN:
			opcode = AMDRDNA_GFX10_V_RNDNE_F32;
			break;
		case DRV_GPU_IR_I2F:
			opcode = AMDRDNA_GFX10_V_CVT_F32_I32;
			break;
		case DRV_GPU_IR_U2F:
			opcode = AMDRDNA_GFX10_V_CVT_F32_U32;
			break;
		case DRV_GPU_IR_F2I:
			opcode = AMDRDNA_GFX10_V_CVT_I32_F32;
			break;
		default:
			opcode = AMDRDNA_GFX10_V_CVT_U32_F32;
			break;
		}

		/* Converts the float to the integer. */
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_ONE, opcode, d, a, 0U, 0U);
		break;
	}

	/* Succeeded: the operation is lowered. */
	return 0;
}

/* Lowers an integer or a bitwise (Boolean included) operation. */
static int
lower_integer(
	struct gfx10_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	unsigned a;
	unsigned b;
	unsigned d;

	/* Decodes the operands the logic operation reads. */
	a = value_operand(lowering, instruction->src[0]);
	b = 0U;
	if (instruction->op != DRV_GPU_IR_NOT && instruction->op != DRV_GPU_IR_INOT &&
	    instruction->op != DRV_GPU_IR_INEG)
		b = value_operand(lowering, instruction->src[1]);
	d = AMDRDNA_GFX10_V(dst);

	/* Picks the instruction by the operation. */
	switch (instruction->op) {
	case DRV_GPU_IR_AND:
	case DRV_GPU_IR_IAND:
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_AND_B32, d, a, b, 0U);
		break;
	case DRV_GPU_IR_OR:
	case DRV_GPU_IR_IOR:
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_OR_B32, d, a, b, 0U);
		break;
	case DRV_GPU_IR_IXOR:
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_XOR_B32, d, a, b, 0U);
		break;
	case DRV_GPU_IR_NOT:
	case DRV_GPU_IR_INOT:
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_ONE, AMDRDNA_GFX10_V_NOT_B32, d, a, 0U, 0U);
		break;
	case DRV_GPU_IR_IADD:
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_ADD_NC_U32, d, a, b, 0U);
		break;
	case DRV_GPU_IR_ISUB:
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_SUB_NC_U32, d, a, b, 0U);
		break;
	case DRV_GPU_IR_INEG:
		/* The negation is zero minus the value. */
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_SUB_NC_U32, d, AMDRDNA_GFX10_ZERO, a, 0U);
		break;
	case DRV_GPU_IR_IMUL:
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_WIDE, AMDRDNA_GFX10_V_MUL_LO_U32, d, a, b, 0U);
		break;
	case DRV_GPU_IR_SHL:
		/* The shifts take the count first and use its low five bits, which is the IR's modulo 32. */
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_LSHLREV_B32, d, b, a, 0U);
		break;
	case DRV_GPU_IR_SHR:
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_LSHRREV_B32, d, b, a, 0U);
		break;
	default:
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_ASHRREV_I32, d, b, a, 0U);
		break;
	}

	/* Succeeded: the operation is lowered. */
	return 0;
}

/* Lowers a comparison into a Boolean, or a selection by a Boolean. */
static int
lower_compare(
	struct gfx10_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	unsigned opcode;
	unsigned taken_clear;
	unsigned taken_set;
	unsigned left;
	unsigned right;

	/* A selection: the condition into VCC_LO, then each lane takes the second source where it is set. */
	if (instruction->op == DRV_GPU_IR_SELECT) {
		condition_from_boolean(lowering, instruction->src[0]);
		taken_clear = value_operand(lowering, instruction->src[2]);
		taken_set = value_operand(lowering, instruction->src[1]);
		emit_vector(lowering,
			    AMDRDNA_GFX10_KIND_VECTOR_TWO,
			    AMDRDNA_GFX10_V_CNDMASK_B32,
			    AMDRDNA_GFX10_V(dst),
			    taken_clear,
			    taken_set,
			    AMDRDNA_GFX10_VCC_LO);
		return 0;
	}

	/* Picks the comparison by the operation. */
	switch (instruction->op) {
	case DRV_GPU_IR_FLT:
		opcode = AMDRDNA_GFX10_V_CMP_LT_F32;
		break;
	case DRV_GPU_IR_FGE:
		opcode = AMDRDNA_GFX10_V_CMP_GE_F32;
		break;
	case DRV_GPU_IR_FEQ:
		opcode = AMDRDNA_GFX10_V_CMP_EQ_F32;
		break;
	case DRV_GPU_IR_FNEU:
		/* "Not equal" in the unordered sense: true when either is NaN. */
		opcode = AMDRDNA_GFX10_V_CMP_NEQ_F32;
		break;
	case DRV_GPU_IR_ILT:
		opcode = AMDRDNA_GFX10_V_CMP_LT_I32;
		break;
	case DRV_GPU_IR_IGE:
		opcode = AMDRDNA_GFX10_V_CMP_GE_I32;
		break;
	case DRV_GPU_IR_ULT:
		opcode = AMDRDNA_GFX10_V_CMP_LT_U32;
		break;
	case DRV_GPU_IR_UGE:
		opcode = AMDRDNA_GFX10_V_CMP_GE_U32;
		break;
	case DRV_GPU_IR_IEQ:
		opcode = AMDRDNA_GFX10_V_CMP_EQ_U32;
		break;
	default:
		opcode = AMDRDNA_GFX10_V_CMP_NE_U32;
		break;
	}

	/* Reads the two values being compared. */
	left = value_operand(lowering, instruction->src[0]);
	right = value_operand(lowering, instruction->src[1]);

	/* The comparison writes VCC_LO, which holds the Boolean from now on. */
	emit_vector(lowering,
		    AMDRDNA_GFX10_KIND_VECTOR_COMPARE,
		    opcode,
		    AMDRDNA_GFX10_VCC_LO,
		    left,
		    right,
		    0U);
	lowering->vcc_value = instruction->dst;

	/* A Boolean read as a value too is made in its VGPR. */
	if (lowering->condition_only[instruction->dst] == 0U)
		boolean_from_condition(lowering, dst);

	/* Succeeded: the comparison is lowered. */
	return 0;
}

/* Lowers the read of a push constant or uniform buffer word: a scalar load, then a copy to every lane. */
static int
lower_uniform(
	struct gfx10_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	const struct drv_gpu_shader_ir_uniform *uniform;
	uint32_t slot;
	uint32_t offset;
	uint32_t found;
	int error;

	/* The buffer: the push constants, or a uniform buffer of the program. */
	slot = GFX10_NO_SLOT;
	if (instruction->op == DRV_GPU_IR_LOAD_PUSH) {
		/* The word lies inside the push constants. */
		if (instruction->immediate + 4U > lowering->ir->push_bytes) {
			error = refuse(lowering, EINVAL, "a push constant read past the push constants");
			return error;
		}
	} else {
		if (instruction->location >= lowering->ir->uniform_count) {
			error = refuse(lowering, EINVAL, "a uniform buffer read names no uniform");
			return error;
		}

		/* The uniform the read names. */
		uniform = &lowering->ir->uniforms[instruction->location];
		if (uniform->kind != DRV_GPU_IR_UNIFORM_BLOCK) {
			error = refuse(lowering, EINVAL, "a uniform buffer read names another kind of uniform");
			return error;
		}

		/* The slot the uniform is bound to. */
		slot = lowering->slot[instruction->location];
	}

	/* The 16-byte chunk the word lies in starts at this offset. */
	offset = instruction->immediate & ~15U;

	/* The 16 bytes around the word, from the cache of chunks or loaded where there is room. */
	found = cache_chunk(lowering, slot, offset, 1);

	/* Every lane gets the word. */
	emit_vector(lowering,
		    AMDRDNA_GFX10_KIND_VECTOR_ONE,
		    AMDRDNA_GFX10_V_MOV_B32,
		    AMDRDNA_GFX10_V(dst),
		    AMDRDNA_GFX10_S(lowering->cache_sgpr + 4U * found + (instruction->immediate & 15U) / 4U),
		    0U,
		    0U);

	/* Succeeded: the word is the value. */
	return 0;
}

/* Lowers the read of a compute invocation's built-in value. */
static int
lower_system(
	struct gfx10_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	const struct drv_gpu_shader_ir *ir;
	unsigned d;
	unsigned source;
	int error;

	/* Names the module and the destination register. */
	ir = lowering->ir;
	d = AMDRDNA_GFX10_V(dst);

	/* Picks the input register by the value asked for. */
	switch (instruction->component) {
	case DRV_GPU_IR_SYSTEM_LOCAL_ID_X:
		source = AMDRDNA_GFX10_V(GFX10_CS_LOCAL_ID_X);
		break;
	case DRV_GPU_IR_SYSTEM_LOCAL_ID_Y:
		source = AMDRDNA_GFX10_V(GFX10_CS_LOCAL_ID_Y);
		break;
	case DRV_GPU_IR_SYSTEM_LOCAL_ID_Z:
		source = AMDRDNA_GFX10_V(GFX10_CS_LOCAL_ID_Z);
		break;
	case DRV_GPU_IR_SYSTEM_GROUP_ID_X:
		source = AMDRDNA_GFX10_S(GFX10_CS_GROUP_ID_X);
		break;
	case DRV_GPU_IR_SYSTEM_GROUP_ID_Y:
		source = AMDRDNA_GFX10_S(GFX10_CS_GROUP_ID_X + 1U);
		break;
	case DRV_GPU_IR_SYSTEM_GROUP_ID_Z:
		source = AMDRDNA_GFX10_S(GFX10_CS_GROUP_ID_X + 2U);
		break;
	case DRV_GPU_IR_SYSTEM_LOCAL_INDEX:
		/*
		 * The linear index is x + size_x * (y + size_y * z); the
		 * sizes are below 2^24, so the 24-bit multiply-adds are exact.
		 */
		emit_multiply_add(lowering,
				  d,
				  AMDRDNA_GFX10_V(GFX10_CS_LOCAL_ID_Z),
				  ir->local_size[1],
				  AMDRDNA_GFX10_V(GFX10_CS_LOCAL_ID_Y));
		emit_multiply_add(lowering,
				  d,
				  d,
				  ir->local_size[0],
				  AMDRDNA_GFX10_V(GFX10_CS_LOCAL_ID_X));
		return 0;
	default:
		error = refuse(lowering, ENOTSUP, "the built-in value is not one a compute program has");
		return error;
	}

	/* Copies the input to the value's VGPR. */
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_ONE, AMDRDNA_GFX10_V_MOV_B32, d, source, 0U, 0U);

	/* Succeeded: the built-in value is read. */
	return 0;
}

/* Lowers a storage buffer read, write or size query. */
static int
lower_storage(
	struct gfx10_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	const struct drv_gpu_shader_ir_uniform *uniform;
	struct amdrdna_gfx10_inst inst;
	uint32_t slot;
	uint32_t predicate;
	int guarded;
	int error;

	/* The uniform is a storage buffer of the program. */
	if (instruction->location >= lowering->ir->uniform_count) {
		error = refuse(lowering, EINVAL, "a storage access names no uniform");
		return error;
	}

	/* The uniform the access names. */
	uniform = &lowering->ir->uniforms[instruction->location];
	if (uniform->kind != DRV_GPU_IR_UNIFORM_STORAGE) {
		error = refuse(lowering, EINVAL, "a storage access names another kind of uniform");
		return error;
	}

	/* The slot the storage buffer is bound to. */
	slot = lowering->slot[instruction->location];

	/* The size is the descriptor's record count, the buffer's bytes. */
	if (instruction->op == DRV_GPU_IR_STORAGE_SIZE) {
		emit_vector(lowering,
			    AMDRDNA_GFX10_KIND_VECTOR_ONE,
			    AMDRDNA_GFX10_V_MOV_B32,
			    AMDRDNA_GFX10_V(dst),
			    AMDRDNA_GFX10_S(storage_descriptor(lowering, slot) + 2U),
			    0U,
			    0U);
		return 0;
	}

	/* A guarded access runs only for the lanes whose predicate holds. */
	guarded = 0;
	if (instruction->op == DRV_GPU_IR_LOAD_STORAGE && instruction->component != 0U) {
		guard_begin(lowering, instruction->src[1]);
		guarded = 1;
	} else if (instruction->op == DRV_GPU_IR_STORE_STORAGE) {
		predicate = GFX10_NOT_DEFINED;
		if (instruction->component != 0U)
			predicate = instruction->src[2];
		guarded = guard_writes(lowering, predicate);
	}

	/* The access: the byte offset in a VGPR, the descriptor, no scalar offset. */
	kern_memset(&inst, 0, sizeof(inst));
	inst.kind = AMDRDNA_GFX10_KIND_BUFFER;
	inst.src[0] = (uint16_t)vgpr_operand(lowering, instruction->src[0]);
	inst.src[1] = (uint16_t)storage_descriptor(lowering, slot);
	inst.src[2] = AMDRDNA_GFX10_ZERO;
	inst.flags = AMDRDNA_GFX10_FLAG_OFFSET_ADDRESS;
	if (instruction->op == DRV_GPU_IR_LOAD_STORAGE) {
		/*
		 * A load writes the value's VGPR.  It goes past both caches (the
		 * coherent bits are set together), so a store of another
		 * workgroup is seen once its fence has run.
		 *
		 * Procedure referenced: Mesa 25.0.7 src/amd/compiler/README-ISA.md
		 * ("RDNA L0, L1 cache and DLC, GLC bits").
		 */
		inst.opcode = AMDRDNA_GFX10_BUFFER_LOAD_DWORD;
		inst.dst = (uint16_t)AMDRDNA_GFX10_V(dst);
		inst.count = 1U;
		inst.flags |= AMDRDNA_GFX10_FLAG_GLOBALLY_COHERENT;
		inst.flags |= AMDRDNA_GFX10_FLAG_DEVICE_COHERENT;
	} else {
		/* A store reads the data's VGPR. */
		inst.opcode = AMDRDNA_GFX10_BUFFER_STORE_DWORD;
		inst.src[3] = (uint16_t)vgpr_operand(lowering, instruction->src[1]);
	}

	/* Emits the access. */
	emit(lowering, &inst);

	/* The lanes come back after a guarded access. */
	if (guarded != 0)
		guard_end(lowering);

	/* Succeeded: the access is lowered. */
	return 0;
}

/*
 * Lowers a loop's start or end.
 *
 * At the start the lane mask is saved and the label of the next pass is
 * placed.  At the end the lanes whose condition is false are dropped, the
 * loop branches back while any lane is left, and the saved mask comes back.
 */
static int
lower_loop(
	struct gfx10_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction)
{
	uint32_t depth;
	uint32_t label;
	int error;

	/* The code after a loop's start or end is reached from two places: the cached chunks are forgotten. */
	kern_memset(lowering->cache_valid, 0, sizeof(lowering->cache_valid));

	/* A loop's start. */
	if (instruction->op == DRV_GPU_IR_LOOP_BEGIN) {
		depth = lowering->loop_depth;
		label = lowering->next_label;
		lowering->next_label++;

		/* Saves the lanes that enter, and marks the start of a pass. */
		emit_scalar(lowering,
			    AMDRDNA_GFX10_KIND_SCALAR_ONE,
			    AMDRDNA_GFX10_S_MOV_B32,
			    lowering->loop_save[depth],
			    AMDRDNA_GFX10_EXEC_LO,
			    0U);
		emit_label(lowering, label);
		lowering->loop_label[depth] = label;
		lowering->loop_depth++;
		return 0;
	}

	/* A loop's end matches the innermost start. */
	if (lowering->loop_depth == 0U) {
		error = refuse(lowering, EINVAL, "a loop's end without its beginning");
		return error;
	}

	/* The innermost loop ends. */
	lowering->loop_depth--;
	depth = lowering->loop_depth;

	/* Drops the lanes whose condition is false. */
	condition_from_boolean(lowering, instruction->src[0]);
	emit_scalar(lowering,
		    AMDRDNA_GFX10_KIND_SCALAR_TWO,
		    AMDRDNA_GFX10_S_AND_B32,
		    AMDRDNA_GFX10_EXEC_LO,
		    AMDRDNA_GFX10_EXEC_LO,
		    AMDRDNA_GFX10_VCC_LO);

	/* Runs another pass while any lane is left. */
	emit_branch(lowering, AMDRDNA_GFX10_S_CBRANCH_EXECNZ, lowering->loop_label[depth]);

	/* The lanes that entered come back. */
	emit_scalar(lowering,
		    AMDRDNA_GFX10_KIND_SCALAR_ONE,
		    AMDRDNA_GFX10_S_MOV_B32,
		    AMDRDNA_GFX10_EXEC_LO,
		    lowering->loop_save[depth],
		    0U);

	/* Except those a discard inside the loop killed. */
	if (lowering->program->discards != 0U)
		restrict_to_alive(lowering);

	/* Succeeded: the loop is closed. */
	return 0;
}

/*
 * Lowers an integer division or remainder, unsigned or signed.
 *
 * The signed forms divide the magnitudes and give the quotient the sign
 * of the operands' exclusive or, the remainder the sign of the numerator.
 */
static int
lower_divide(
	struct gfx10_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	unsigned numerator;
	unsigned denominator;
	uint32_t quotient;
	uint32_t remainder;
	uint32_t temp;
	uint32_t sign;
	uint32_t magnitude;
	unsigned shift;
	int signed_form;
	int error;

	/* Decodes the operands of the division. */
	numerator = value_operand(lowering, instruction->src[0]);
	denominator = value_operand(lowering, instruction->src[1]);
	signed_form = 0;
	if (instruction->op == DRV_GPU_IR_IDIV || instruction->op == DRV_GPU_IR_IREM)
		signed_form = 1;

	/* The quotient, the remainder and a scratch register. */
	error = take_vgpr(lowering, &quotient);
	if (error != 0)
		return error;
	error = take_vgpr(lowering, &remainder);
	if (error != 0)
		return error;
	error = take_vgpr(lowering, &temp);
	if (error != 0)
		return error;

	/*
	 * The signed forms divide the magnitudes: |x| is the larger of x and
	 * -x (the most negative integer stays itself, which read unsigned is
	 * its magnitude), |n| held in dst and |d| in a fourth register.
	 */
	if (signed_form != 0) {
		error = take_vgpr(lowering, &magnitude);
		if (error != 0)
			return error;
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_SUB_NC_U32, AMDRDNA_GFX10_V(temp), AMDRDNA_GFX10_ZERO, numerator, 0U);
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_MAX_I32, AMDRDNA_GFX10_V(dst), numerator, AMDRDNA_GFX10_V(temp), 0U);
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_SUB_NC_U32, AMDRDNA_GFX10_V(temp), AMDRDNA_GFX10_ZERO, denominator, 0U);
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_MAX_I32, AMDRDNA_GFX10_V(magnitude), denominator, AMDRDNA_GFX10_V(temp), 0U);
		divide_unsigned(lowering, AMDRDNA_GFX10_V(dst), AMDRDNA_GFX10_V(magnitude), quotient, remainder, temp);
		give_vgpr(lowering, magnitude);
	} else {
		divide_unsigned(lowering, numerator, denominator, quotient, remainder, temp);
	}

	/* Picks the result, giving a signed one its sign: (r ^ s) - s with s all ones for a negative result. */
	if (instruction->op == DRV_GPU_IR_UDIV) {
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_ONE, AMDRDNA_GFX10_V_MOV_B32, AMDRDNA_GFX10_V(dst), AMDRDNA_GFX10_V(quotient), 0U, 0U);
	} else if (instruction->op == DRV_GPU_IR_UMOD) {
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_ONE, AMDRDNA_GFX10_V_MOV_B32, AMDRDNA_GFX10_V(dst), AMDRDNA_GFX10_V(remainder), 0U, 0U);
	} else {
		/* The sign: of numerator ^ denominator for the quotient, of the numerator for the remainder. */
		sign = temp;
		shift = constant_operand(31U);
		if (instruction->op == DRV_GPU_IR_IDIV) {
			emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_XOR_B32, AMDRDNA_GFX10_V(sign), numerator, denominator, 0U);
			emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_ASHRREV_I32, AMDRDNA_GFX10_V(sign), shift, AMDRDNA_GFX10_V(sign), 0U);
			emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_XOR_B32, AMDRDNA_GFX10_V(dst), AMDRDNA_GFX10_V(quotient), AMDRDNA_GFX10_V(sign), 0U);
		} else {
			emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_ASHRREV_I32, AMDRDNA_GFX10_V(sign), shift, numerator, 0U);
			emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_XOR_B32, AMDRDNA_GFX10_V(dst), AMDRDNA_GFX10_V(remainder), AMDRDNA_GFX10_V(sign), 0U);
		}

		/* Completes the two's complement by subtracting the sign. */
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_SUB_NC_U32, AMDRDNA_GFX10_V(dst), AMDRDNA_GFX10_V(dst), AMDRDNA_GFX10_V(sign), 0U);
	}

	/* Releases the temporary registers. */
	give_vgpr(lowering, temp);
	give_vgpr(lowering, remainder);
	give_vgpr(lowering, quotient);

	/* Succeeded: the division is lowered. */
	return 0;
}

/*
 * Divides two unsigned integers: the quotient and the remainder into their
 * VGPRs, temp used as scratch; none of the three is the numerator's or the
 * denominator's.
 *
 * A fixed-point reciprocal of the denominator from the float unit is
 * refined once by one Newton step; the quotient it gives is at most two
 * too small, and two corrections add the rest.
 *
 * Procedure referenced: Mesa 25.0.7 src/compiler/nir/nir_lower_idiv.c
 * (emit_udiv).
 */
static void
divide_unsigned(
	struct gfx10_lowering *lowering,
	unsigned numerator,
	unsigned denominator,
	uint32_t quotient,
	uint32_t remainder,
	uint32_t temp)
{
	unsigned q;
	unsigned r;
	unsigned t;
	unsigned one;
	uint32_t pass;

	/* Names the registers of the division step. */
	q = AMDRDNA_GFX10_V(quotient);
	r = AMDRDNA_GFX10_V(remainder);
	t = AMDRDNA_GFX10_V(temp);

	/* The estimate e of 2^32 / d, in the quotient's VGPR: the float reciprocal scaled and truncated. */
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_ONE, AMDRDNA_GFX10_V_CVT_F32_U32, t, denominator, 0U, 0U);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_ONE, AMDRDNA_GFX10_V_RCP_IFLAG_F32, t, t, 0U, 0U);
	emit_vector_literal(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_MUL_F32, t, AMDRDNA_GFX10_LITERAL, t, GFX10_RECIPROCAL_SCALE);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_ONE, AMDRDNA_GFX10_V_CVT_U32_F32, t, t, 0U, 0U);

	/* One Newton step: e += high(e * (-d * e)), the error term kept in the remainder's VGPR. */
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_SUB_NC_U32, q, AMDRDNA_GFX10_ZERO, denominator, 0U);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_WIDE, AMDRDNA_GFX10_V_MUL_LO_U32, q, q, t, 0U);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_WIDE, AMDRDNA_GFX10_V_MUL_HI_U32, q, t, q, 0U);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_ADD_NC_U32, t, t, q, 0U);

	/* The first quotient high(n * e), and its remainder n - q * d. */
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_WIDE, AMDRDNA_GFX10_V_MUL_HI_U32, q, numerator, t, 0U);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_WIDE, AMDRDNA_GFX10_V_MUL_LO_U32, t, q, denominator, 0U);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_SUB_NC_U32, r, numerator, t, 0U);

	/* The constant 1, by which a quotient grows. */
	one = constant_operand(1U);

	/* Twice: where the remainder is still at least d, the quotient grows by one and the remainder drops by d. */
	for (pass = 0U; pass < 2U; pass++) {
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_COMPARE, AMDRDNA_GFX10_V_CMP_GE_U32, AMDRDNA_GFX10_VCC_LO, r, denominator, 0U);
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_ADD_NC_U32, t, one, q, 0U);
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_CNDMASK_B32, q, q, t, AMDRDNA_GFX10_VCC_LO);
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_SUB_NC_U32, t, r, denominator, 0U);
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_CNDMASK_B32, r, r, t, AMDRDNA_GFX10_VCC_LO);
	}
}

/*
 * Lowers an atomic on a storage buffer word, or on a texel of a storage
 * image or storage texel buffer of one 32-bit integer channel.
 *
 * The buffer atomic writes the old value over its data register, so the
 * data is first copied into dst (or, for a compare-and-swap, into a pair
 * with the compare after it).  The old value is asked for only when
 * something reads it.
 */
static int
lower_atomic(
	struct gfx10_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	const struct drv_gpu_shader_ir_uniform *uniform;
	struct amdrdna_gfx10_inst inst;
	uint32_t slot;
	uint32_t pair;
	uint32_t predicate;
	uint32_t block;
	uint32_t count;
	uint32_t index;
	unsigned opcode;
	unsigned data;
	unsigned compare;
	unsigned dimension;
	int returns;
	int compare_swap;
	int guarded;
	int texel;
	int texel_buffer;
	int error;

	/* The uniform is a storage buffer, or a storage image or texel buffer of a texel atomic. */
	texel = (instruction->image_shape & DRV_GPU_IR_IMAGE_TEXEL) != 0U;
	texel_buffer = 0;
	if (texel) {
		error = storage_image_slot(lowering, instruction, &slot, &texel_buffer);
		if (error != 0)
			return error;
	} else {
		if (instruction->location >= lowering->ir->uniform_count) {
			error = refuse(lowering, EINVAL, "an atomic names no uniform");
			return error;
		}

		/* The uniform the atomic names. */
		uniform = &lowering->ir->uniforms[instruction->location];
		if (uniform->kind != DRV_GPU_IR_UNIFORM_STORAGE) {
			error = refuse(lowering, EINVAL, "an atomic names another kind of uniform");
			return error;
		}

		/* The slot the storage buffer is bound to. */
		slot = lowering->slot[instruction->location];
	}

	/* Picks the buffer atomic by the operation. */
	compare_swap = 0;
	switch (instruction->immediate) {
	case DRV_GPU_IR_ATOMIC_AND:
		opcode = AMDRDNA_GFX10_BUFFER_ATOMIC_AND;
		break;
	case DRV_GPU_IR_ATOMIC_OR:
		opcode = AMDRDNA_GFX10_BUFFER_ATOMIC_OR;
		break;
	case DRV_GPU_IR_ATOMIC_XOR:
		opcode = AMDRDNA_GFX10_BUFFER_ATOMIC_XOR;
		break;
	case DRV_GPU_IR_ATOMIC_XCHG:
		opcode = AMDRDNA_GFX10_BUFFER_ATOMIC_SWAP;
		break;
	case DRV_GPU_IR_ATOMIC_ADD:
		opcode = AMDRDNA_GFX10_BUFFER_ATOMIC_ADD;
		break;
	case DRV_GPU_IR_ATOMIC_SUB:
		opcode = AMDRDNA_GFX10_BUFFER_ATOMIC_SUB;
		break;
	case DRV_GPU_IR_ATOMIC_SMAX:
		opcode = AMDRDNA_GFX10_BUFFER_ATOMIC_SMAX;
		break;
	case DRV_GPU_IR_ATOMIC_SMIN:
		opcode = AMDRDNA_GFX10_BUFFER_ATOMIC_SMIN;
		break;
	case DRV_GPU_IR_ATOMIC_UMAX:
		opcode = AMDRDNA_GFX10_BUFFER_ATOMIC_UMAX;
		break;
	case DRV_GPU_IR_ATOMIC_UMIN:
		opcode = AMDRDNA_GFX10_BUFFER_ATOMIC_UMIN;
		break;
	case DRV_GPU_IR_ATOMIC_CMPXCHG:
		opcode = AMDRDNA_GFX10_BUFFER_ATOMIC_CMPSWAP;
		compare_swap = 1;
		break;
	default:
		error = refuse(lowering, EINVAL, "an atomic operation the IR does not have");
		return error;
	}

	/* The old value is returned only when a later instruction reads it. */
	returns = 0;
	if (lowering->last_use[instruction->dst] > lowering->index + 1U)
		returns = 1;

	/* A compare-and-swap takes its data and compare in a pair of VGPRs. */
	pair = dst;
	if (compare_swap != 0) {
		error = take_vgpr_pair(lowering, &pair);
		if (error != 0)
			return error;
	}

	/* The data (and the compare) copied where the atomic writes the old value. */
	data = value_operand(lowering, instruction->src[1]);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_ONE, AMDRDNA_GFX10_V_MOV_B32, AMDRDNA_GFX10_V(pair), data, 0U, 0U);
	if (compare_swap != 0) {
		compare = value_operand(lowering, instruction->src[2]);
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_ONE, AMDRDNA_GFX10_V_MOV_B32, AMDRDNA_GFX10_V(pair + 1U), compare, 0U, 0U);
	}

	/* A predicated atomic runs only for the lanes whose predicate holds (and never for a pixel program's helpers). */
	predicate = GFX10_NOT_DEFINED;
	if (instruction->component == DRV_GPU_IR_ATOMIC_PREDICATED) {
		predicate = instruction->src[2];
		if (compare_swap != 0)
			predicate = instruction->src[3];
	}

	/* A texel's address: the texel buffer's index, or the image's coordinates the dimension takes. */
	block = GFX10_NO_SLOT;
	count = 1U;
	dimension = 0U;
	if (texel) {
		if (texel_buffer == 0)
			dimension = storage_dimension(instruction->image_shape, &count);
		error = storage_address(lowering, instruction->src[0], count, &block);
		if (error != 0)
			return error;
	}

	/* Guards the writes of the lanes the predicate disables. */
	guarded = guard_writes(lowering, predicate);

	/* The atomic: the byte offset in a VGPR, the descriptor, no scalar offset. */
	kern_memset(&inst, 0, sizeof(inst));
	inst.kind = AMDRDNA_GFX10_KIND_BUFFER;
	inst.opcode = (uint16_t)opcode;
	inst.src[2] = AMDRDNA_GFX10_ZERO;
	inst.src[3] = (uint16_t)AMDRDNA_GFX10_V(pair);
	inst.flags = AMDRDNA_GFX10_FLAG_OFFSET_ADDRESS;
	if (returns != 0) {
		inst.dst = (uint16_t)AMDRDNA_GFX10_V(pair);
		inst.flags |= AMDRDNA_GFX10_FLAG_GLOBALLY_COHERENT;
	}
	if (!texel) {
		inst.src[0] = (uint16_t)vgpr_operand(lowering, instruction->src[0]);
		inst.src[1] = (uint16_t)storage_descriptor(lowering, slot);
	}

	/*
	 * A texel buffer's word by its index (the descriptor's stride the
	 * word's four bytes); an image's texel by the image atomic of the
	 * operation at unnormalized coordinates, its data and compare in the
	 * channels its mask selects.
	 *
	 * Procedure referenced: Mesa 25.0.7
	 * src/amd/compiler/aco_instruction_selection.cpp (visit_image_atomic:
	 * the MUBUF atomic with idxen for GLSL_SAMPLER_DIM_BUF; otherwise the
	 * MIMG atomic with unrm, the dimension ac_get_image_dim gives and the
	 * mask of the data's dwords).
	 */
	if (texel) {
		inst.src[0] = (uint16_t)AMDRDNA_GFX10_V(block);
		inst.src[1] = (uint16_t)storage_image_descriptor(lowering, slot);
		inst.flags = AMDRDNA_GFX10_FLAG_INDEX_ADDRESS | (inst.flags & AMDRDNA_GFX10_FLAG_GLOBALLY_COHERENT);
	}
	if (texel && texel_buffer == 0) {
		inst.kind = AMDRDNA_GFX10_KIND_IMAGE;
		inst.opcode = (uint16_t)(opcode - AMDRDNA_GFX10_IMAGE_ATOMIC_FROM_BUFFER);
		inst.dst = (uint16_t)AMDRDNA_GFX10_V(pair);
		inst.src[2] = AMDRDNA_GFX10_NULL;
		inst.src[3] = 0U;
		inst.mask = compare_swap != 0 ? 0x3U : 0x1U;
		inst.count = (uint8_t)count;
		inst.dimension = (uint8_t)dimension;
		inst.flags = AMDRDNA_GFX10_FLAG_UNNORMALIZED;
		if (returns != 0)
			inst.flags |= AMDRDNA_GFX10_FLAG_GLOBALLY_COHERENT;
	}

	/* Emits the atomic. */
	emit(lowering, &inst);

	/* The lanes come back after a guarded atomic. */
	if (guarded != 0)
		guard_end(lowering);

	/* Gives the texel's address back. */
	if (texel) {
		for (index = 0U; index < count; index++)
			give_vgpr(lowering, block + index);
	}

	/* A compare-and-swap's old value moves from the pair to dst. */
	if (compare_swap != 0) {
		if (returns != 0)
			emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_ONE, AMDRDNA_GFX10_V_MOV_B32, AMDRDNA_GFX10_V(dst), AMDRDNA_GFX10_V(pair), 0U, 0U);
		give_vgpr(lowering, pair + 1U);
		give_vgpr(lowering, pair);
	}

	/* Succeeded: the atomic is lowered. */
	return 0;
}

/*
 * Finds the storage image or storage texel buffer an image read, write or
 * texel atomic names (by its index in the uniform list): its slot, and
 * whether it is a texel buffer.  Refuses any other uniform.
 */
static int
storage_image_slot(
	struct gfx10_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t *slot,
	int *texel_buffer)
{
	const struct drv_gpu_shader_ir_uniform *uniform;
	int error;

	/* A uniform of the program. */
	if (instruction->location >= lowering->ir->uniform_count) {
		error = refuse(lowering, EINVAL, "an image access names no uniform");
		return error;
	}

	/* A storage image or a storage texel buffer (an input attachment is not compiled). */
	uniform = &lowering->ir->uniforms[instruction->location];
	if (uniform->kind != DRV_GPU_IR_UNIFORM_STORAGE_IMAGE && uniform->kind != DRV_GPU_IR_UNIFORM_TEXEL_BUFFER) {
		error = refuse(lowering, ENOTSUP, "an image access of something that is not a storage image");
		return error;
	}

	/* Succeeded: its slot and kind. */
	*slot = lowering->slot[instruction->location];
	*texel_buffer = uniform->kind == DRV_GPU_IR_UNIFORM_TEXEL_BUFFER;
	return 0;
}

/*
 * Reports the dimension field of an access of a storage image of an IR
 * image shape, and the address VGPRs it takes (*count): a 3D image's x, y
 * and z; an array's or a cube's x, y and layer, a cube read as the array
 * of its six faces its storage descriptor is; a 2D image's x and y.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/common/ac_shader_util.c
 * (ac_get_image_dim: a cube as ac_image_2darray to match the descriptor)
 * and src/amd/vulkan/radv_image_view.c (radv_tex_dim: a cube's storage
 * view as V_008F1C_SQ_RSRC_IMG_2D_ARRAY).
 */
static unsigned
storage_dimension(
	uint32_t shape,
	uint32_t *count)
{
	uint32_t kind;

	/* A 3D image. */
	kind = shape & DRV_GPU_IR_IMAGE_KIND_MASK;
	*count = 3U;
	if (kind == DRV_GPU_IR_IMAGE_3D)
		return AMDRDNA_GFX10_IMAGE_DIM_3D;

	/* An array of 2D images, or a cube's faces. */
	if ((shape & DRV_GPU_IR_IMAGE_ARRAYED) != 0U || kind == DRV_GPU_IR_IMAGE_CUBE)
		return AMDRDNA_GFX10_IMAGE_DIM_2D_ARRAY;

	/* A 2D image. */
	*count = 2U;
	return AMDRDNA_GFX10_IMAGE_DIM_2D;
}

/* Copies count consecutive IR values from first into a block of VGPRs of its own (*block), an image access's address. */
static int
storage_address(
	struct gfx10_lowering *lowering,
	uint32_t first,
	uint32_t count,
	uint32_t *block)
{
	uint32_t index;
	unsigned operand;
	int error;

	/* The block. */
	error = take_vgpr_block(lowering, count, block);
	if (error != 0)
		return error;

	/* Each value, wherever it lives, into its VGPR. */
	for (index = 0U; index < count; index++) {
		operand = value_operand(lowering, first + index);
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_ONE, AMDRDNA_GFX10_V_MOV_B32, AMDRDNA_GFX10_V(*block + index), operand, 0U, 0U);
	}

	/* Succeeded: the address is in the block. */
	return 0;
}

/*
 * Lowers a read or a write of a texel of a storage image or of a storage
 * texel buffer.
 *
 * A storage image's texel is read by image_load and written by image_store
 * of the four channels at unnormalized coordinates, the image descriptor
 * converting from and to the view's format (the channels it lacks read as
 * its channel selects say); a texel buffer's by the typed buffer load and
 * store by index, the buffer descriptor converting (past its records a
 * read is zero and a write is dropped).  A read goes past the per-unit and
 * the shader array's caches, as a storage buffer's load does, so that a
 * write of another workgroup is seen after its fence.  A guarded read runs
 * for the lanes of its predicate; a write for those and, in a pixel
 * program, never for a helper lane.
 *
 * Procedure referenced: Mesa 25.0.7
 * src/amd/compiler/aco_instruction_selection.cpp (visit_image_load and
 * visit_image_store: buffer_load_format_xyzw and buffer_store_format_xyzw
 * with idxen for GLSL_SAMPLER_DIM_BUF; otherwise image_load and image_store
 * with unrm, the dimension of ac_get_image_dim and the mask of the
 * channels) and src/amd/compiler/README-ISA.md ("RDNA L0, L1 cache and
 * DLC, GLC bits").
 */
static int
lower_image(
	struct gfx10_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	struct amdrdna_gfx10_inst inst;
	uint32_t slot;
	uint32_t block;
	uint32_t data;
	uint32_t count;
	uint32_t index;
	uint32_t predicate;
	unsigned dimension;
	unsigned operand;
	int texel_buffer;
	int guarded;
	int error;

	/* The image the access names. */
	error = storage_image_slot(lowering, instruction, &slot, &texel_buffer);
	if (error != 0)
		return error;

	/* The address: a texel buffer's index, or the coordinates the image's dimension takes. */
	count = 1U;
	dimension = 0U;
	if (texel_buffer == 0)
		dimension = storage_dimension(instruction->image_shape, &count);
	error = storage_address(lowering, instruction->src[0], count, &block);
	if (error != 0)
		return error;

	/* A write's four values in a block of their own, then the lanes it runs for. */
	data = GFX10_NO_SLOT;
	guarded = 0;
	if (instruction->op == DRV_GPU_IR_IMAGE_WRITE) {
		error = take_vgpr_block(lowering, 4U, &data);
		if (error != 0)
			return error;
		for (index = 0U; index < 4U; index++) {
			operand = value_operand(lowering, instruction->src[2] + index);
			emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_ONE, AMDRDNA_GFX10_V_MOV_B32, AMDRDNA_GFX10_V(data + index), operand, 0U, 0U);
		}
		predicate = GFX10_NOT_DEFINED;
		if (instruction->component != 0U)
			predicate = instruction->src[3];
		guarded = guard_writes(lowering, predicate);
	} else if (instruction->component != 0U) {
		guard_begin(lowering, instruction->src[2]);
		guarded = 1;
	}

	/* The access: through the image descriptor, or the texel buffer's buffer descriptor in its place. */
	kern_memset(&inst, 0, sizeof(inst));
	inst.src[0] = (uint16_t)AMDRDNA_GFX10_V(block);
	inst.src[1] = (uint16_t)storage_image_descriptor(lowering, slot);
	if (texel_buffer != 0) {
		inst.kind = AMDRDNA_GFX10_KIND_BUFFER;
		inst.src[2] = AMDRDNA_GFX10_ZERO;
		inst.flags = AMDRDNA_GFX10_FLAG_INDEX_ADDRESS;
		if (instruction->op == DRV_GPU_IR_IMAGE_READ) {
			inst.opcode = AMDRDNA_GFX10_BUFFER_LOAD_FORMAT_XYZW;
			inst.dst = (uint16_t)AMDRDNA_GFX10_V(dst);
			inst.count = 4U;
			inst.flags |= AMDRDNA_GFX10_FLAG_GLOBALLY_COHERENT | AMDRDNA_GFX10_FLAG_DEVICE_COHERENT;
		} else {
			inst.opcode = AMDRDNA_GFX10_BUFFER_STORE_FORMAT_XYZW;
			inst.src[3] = (uint16_t)AMDRDNA_GFX10_V(data);
		}
	} else {
		inst.kind = AMDRDNA_GFX10_KIND_IMAGE;
		inst.src[2] = AMDRDNA_GFX10_NULL;
		inst.mask = 0xfU;
		inst.count = (uint8_t)count;
		inst.dimension = (uint8_t)dimension;
		inst.flags = AMDRDNA_GFX10_FLAG_UNNORMALIZED;
		if (instruction->op == DRV_GPU_IR_IMAGE_READ) {
			inst.opcode = AMDRDNA_GFX10_IMAGE_LOAD;
			inst.dst = (uint16_t)AMDRDNA_GFX10_V(dst);
			inst.flags |= AMDRDNA_GFX10_FLAG_GLOBALLY_COHERENT | AMDRDNA_GFX10_FLAG_DEVICE_COHERENT;
		} else {
			inst.opcode = AMDRDNA_GFX10_IMAGE_STORE;
			inst.dst = (uint16_t)AMDRDNA_GFX10_V(data);
		}
	}
	emit(lowering, &inst);

	/* The lanes come back after a guarded access. */
	if (guarded != 0)
		guard_end(lowering);

	/* The address and the data go back. */
	for (index = 0U; index < count; index++)
		give_vgpr(lowering, block + index);
	if (data != GFX10_NO_SLOT) {
		for (index = 0U; index < 4U; index++)
			give_vgpr(lowering, data + index);
	}

	/* Succeeded: the access is lowered. */
	return 0;
}

/*
 * Lowers a read, a write or an atomic of the workgroup's shared memory.
 *
 * The shared memory accesses on GFX10 need no M0 (only the per-lane
 * address variants read it).
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/compiler/README-ISA.md
 * ("m0 with LDS instructions on Vega and newer").
 */
static int
lower_shared(
	struct gfx10_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	struct amdrdna_gfx10_inst inst;
	uint32_t predicate;
	int guarded;
	int error;

	/* Starts the shared-memory instruction. */
	kern_memset(&inst, 0, sizeof(inst));
	inst.kind = AMDRDNA_GFX10_KIND_SHARED;
	inst.src[0] = (uint16_t)vgpr_operand(lowering, instruction->src[0]);
	guarded = 0;
	predicate = 0U;

	/* Picks the access by the operation. */
	if (instruction->op == DRV_GPU_IR_LOAD_SHARED) {
		inst.opcode = AMDRDNA_GFX10_DS_READ_B32;
		inst.dst = (uint16_t)AMDRDNA_GFX10_V(dst);
		if (instruction->component != 0U) {
			guarded = 1;
			predicate = instruction->src[1];
		}
	} else if (instruction->op == DRV_GPU_IR_STORE_SHARED) {
		inst.opcode = AMDRDNA_GFX10_DS_WRITE_B32;
		inst.src[1] = (uint16_t)vgpr_operand(lowering, instruction->src[1]);
		if (instruction->component != 0U) {
			guarded = 1;
			predicate = instruction->src[2];
		}
	} else {
		/* An atomic returns the old value; a compare-and-store takes the compare first. */
		inst.dst = (uint16_t)AMDRDNA_GFX10_V(dst);
		inst.src[1] = (uint16_t)vgpr_operand(lowering, instruction->src[1]);
		switch (instruction->immediate) {
		case DRV_GPU_IR_ATOMIC_AND:
			inst.opcode = AMDRDNA_GFX10_DS_AND_RTN_B32;
			break;
		case DRV_GPU_IR_ATOMIC_OR:
			inst.opcode = AMDRDNA_GFX10_DS_OR_RTN_B32;
			break;
		case DRV_GPU_IR_ATOMIC_XOR:
			inst.opcode = AMDRDNA_GFX10_DS_XOR_RTN_B32;
			break;
		case DRV_GPU_IR_ATOMIC_XCHG:
			inst.opcode = AMDRDNA_GFX10_DS_WRXCHG_RTN_B32;
			break;
		case DRV_GPU_IR_ATOMIC_ADD:
			inst.opcode = AMDRDNA_GFX10_DS_ADD_RTN_U32;
			break;
		case DRV_GPU_IR_ATOMIC_SUB:
			inst.opcode = AMDRDNA_GFX10_DS_SUB_RTN_U32;
			break;
		case DRV_GPU_IR_ATOMIC_SMAX:
			inst.opcode = AMDRDNA_GFX10_DS_MAX_RTN_I32;
			break;
		case DRV_GPU_IR_ATOMIC_SMIN:
			inst.opcode = AMDRDNA_GFX10_DS_MIN_RTN_I32;
			break;
		case DRV_GPU_IR_ATOMIC_UMAX:
			inst.opcode = AMDRDNA_GFX10_DS_MAX_RTN_U32;
			break;
		case DRV_GPU_IR_ATOMIC_UMIN:
			inst.opcode = AMDRDNA_GFX10_DS_MIN_RTN_U32;
			break;
		case DRV_GPU_IR_ATOMIC_CMPXCHG:
			inst.opcode = AMDRDNA_GFX10_DS_CMPST_RTN_B32;
			inst.src[1] = (uint16_t)vgpr_operand(lowering, instruction->src[2]);
			inst.src[2] = (uint16_t)vgpr_operand(lowering, instruction->src[1]);
			break;
		default:
			error = refuse(lowering, EINVAL, "an atomic operation the IR does not have");
			return error;
		}

		/* A predicated atomic runs only where its predicate holds. */
		if (instruction->component == DRV_GPU_IR_ATOMIC_PREDICATED) {
			guarded = 1;
			predicate = instruction->src[2];
			if (instruction->immediate == DRV_GPU_IR_ATOMIC_CMPXCHG)
				predicate = instruction->src[3];
		}
	}

	/* The access, under its guard when it has one. */
	if (guarded != 0)
		guard_begin(lowering, predicate);
	emit(lowering, &inst);
	if (guarded != 0)
		guard_end(lowering);

	/* Succeeded: the access is lowered. */
	return 0;
}

/*
 * Lowers a fence or a workgroup barrier.
 *
 * A fence waits until the accesses of the memory it names are done (the
 * loads of storage go past the caches, so nothing is to be invalidated).
 * A barrier runs its fences, then waits for the other waves.
 */
static void
lower_sync(
	struct gfx10_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction)
{
	struct amdrdna_gfx10_inst inst;

	/* The fence's waits. */
	emit_waits(lowering, instruction->immediate);

	/* A barrier waits for every wave of the workgroup. */
	if (instruction->op == DRV_GPU_IR_BARRIER) {
		kern_memset(&inst, 0, sizeof(inst));
		inst.kind = AMDRDNA_GFX10_KIND_CONTROL;
		inst.opcode = AMDRDNA_GFX10_S_BARRIER;
		emit(lowering, &inst);
	}
}

/* Waits for the memory accesses a set of fences (DRV_GPU_IR_FENCE_*) names. */
static void
emit_waits(
	struct gfx10_lowering *lowering,
	uint32_t fences)
{
	struct amdrdna_gfx10_inst inst;
	uint32_t immediate;

	/* The loads and returning atomics of storage, and the shared memory accesses, by the counter wait. */
	immediate = AMDRDNA_GFX10_WAIT_NONE;
	if ((fences & DRV_GPU_IR_FENCE_GLOBAL) != 0U)
		immediate &= ~AMDRDNA_GFX10_WAIT_VECTOR_MASK;
	if ((fences & DRV_GPU_IR_FENCE_SHARED) != 0U)
		immediate &= ~AMDRDNA_GFX10_WAIT_SCALAR_MASK;
	if (immediate != AMDRDNA_GFX10_WAIT_NONE) {
		kern_memset(&inst, 0, sizeof(inst));
		inst.kind = AMDRDNA_GFX10_KIND_CONTROL;
		inst.opcode = AMDRDNA_GFX10_S_WAITCNT;
		inst.immediate = immediate;
		emit(lowering, &inst);
	}

	/* The stores and the other atomics of storage, by the store counter wait. */
	if ((fences & DRV_GPU_IR_FENCE_GLOBAL) != 0U) {
		kern_memset(&inst, 0, sizeof(inst));
		inst.kind = AMDRDNA_GFX10_KIND_SCALAR_CONSTANT;
		inst.opcode = AMDRDNA_GFX10_S_WAITCNT_VSCNT;
		inst.dst = AMDRDNA_GFX10_NULL;
		emit(lowering, &inst);
	}
}

/*
 * Finds what a graphics program reads and writes: a vertex program's
 * fetched locations, parameters and point size; a pixel program's
 * attributes (flat or not), position, face, colours and discards.  It
 * decides the pixel inputs the hardware delivers.
 */
static int
prepare_graphics(
	struct gfx10_lowering *lowering)
{
	const struct drv_gpu_shader_ir *ir;
	const struct drv_gpu_shader_ir_inst *instruction;
	const struct drv_gpu_shader_ir_io *input;
	struct amdrdna_gfx10_program *program;
	uint32_t index;
	uint32_t location;
	uint32_t next;
	int uses_linear;
	int reads_point_coord;
	int reads_primitive_id;
	int writes_color_1;
	int error;

	/* Names the module and the program being scanned. */
	ir = lowering->ir;
	program = lowering->program;
	uses_linear = 0;
	reads_point_coord = 0;
	reads_primitive_id = 0;
	writes_color_1 = 0;

	/* A compute program has none of these. */
	if (ir->stage == DRV_GPU_STAGE_COMPUTE)
		return 0;

	/* Each input read and output written by an instruction that is lowered. */
	for (index = 0U; index < ir->instruction_count; index++) {
		instruction = &ir->instructions[index];
		lowering->index = index;
		location = instruction->location;
		if (lowering->dead[index] != 0U)
			continue;

		/* A discard, in a pixel program only. */
		if (instruction->op == DRV_GPU_IR_KILL) {
			if (ir->stage != DRV_GPU_STAGE_FRAGMENT) {
				error = refuse(lowering, EINVAL, "a discard outside a fragment shader");
				return error;
			}

			/* Marks the program as one that discards. */
			program->discards = 1U;
			continue;
		}

		/* A difference across the group of four is a pixel program's only, and runs whole quads. */
		if (instruction->op == DRV_GPU_IR_DDX ||
		    instruction->op == DRV_GPU_IR_DDX_FINE ||
		    instruction->op == DRV_GPU_IR_DDY ||
		    instruction->op == DRV_GPU_IR_DDY_FINE) {
			if (ir->stage != DRV_GPU_STAGE_FRAGMENT) {
				error = refuse(lowering, EINVAL, "a derivative outside a fragment shader");
				return error;
			}

			/* Marks the program as one that runs whole quads. */
			lowering->whole_quads = 1;
			continue;
		}

		/* A pixel program's sample with derivatives runs whole quads. */
		if (ir->stage == DRV_GPU_STAGE_FRAGMENT) {
			if (instruction->op == DRV_GPU_IR_SAMPLE || instruction->op == DRV_GPU_IR_SAMPLE_BIAS) {
				lowering->whole_quads = 1;
			} else if (instruction->op == DRV_GPU_IR_TEXTURE &&
				   ((instruction->component & 0x1fU) == DRV_GPU_IR_TEXTURE_SAMPLE ||
				    (instruction->component & 0x1fU) == DRV_GPU_IR_TEXTURE_SAMPLE_BIAS ||
				    (instruction->component & 0x1fU) == DRV_GPU_IR_TEXTURE_SAMPLE_COMPARE ||
				    (instruction->component & 0x1fU) == DRV_GPU_IR_TEXTURE_SAMPLE_BIAS_COMPARE ||
				    (instruction->component & 0x1fU) == DRV_GPU_IR_TEXTURE_LOD)) {
				lowering->whole_quads = 1;
			}
		}

		/* Only the inputs and the outputs are looked at further. */
		if (instruction->op != DRV_GPU_IR_LOAD_INPUT && instruction->op != DRV_GPU_IR_STORE_OUTPUT)
			continue;

		/* Four channels to a location. */
		if (instruction->component > 3U) {
			error = refuse(lowering, EINVAL, "an input or output channel past the fourth");
			return error;
		}

		/* A vertex program's input: a fetched location, or the vertex or instance number. */
		if (ir->stage == DRV_GPU_STAGE_VERTEX && instruction->op == DRV_GPU_IR_LOAD_INPUT) {
			if (location < AMDRDNA_GFX10_LOCATIONS) {
				program->fetched_locations |= 1U << location;
			} else if (location != DRV_GPU_SHADER_LOCATION_VERTEX_INDEX &&
				   location != DRV_GPU_SHADER_LOCATION_INSTANCE_INDEX) {
				error = refuse(lowering, ENOTSUP, "a vertex input that is not compiled yet");
				return error;
			}

			continue;
		}

		/* A vertex program's output: the position, the point size, or a parameter. */
		if (ir->stage == DRV_GPU_STAGE_VERTEX) {
			if (location == DRV_GPU_IR_LOCATION_POINT_SIZE) {
				program->writes_point_size = 1U;
			} else if (location < AMDRDNA_GFX10_LOCATIONS) {
				program->parameter_outputs |= 1U << location;
			} else if (location != DRV_GPU_IR_LOCATION_POSITION) {
				error = refuse(lowering, ENOTSUP, "a vertex output that is not compiled yet");
				return error;
			}

			continue;
		}

		/*
		 * A pixel program's output: a colour, the second colour of a
		 * dual-source blend (exported as colour 1), the depth or the
		 * sample mask.
		 */
		if (instruction->op == DRV_GPU_IR_STORE_OUTPUT) {
			if (location == DRV_GPU_IR_LOCATION_FRAG_DEPTH) {
				program->writes_depth = 1U;
				continue;
			}

			/* The stencil reference goes to the depth target too. */
			if (location == DRV_GPU_IR_LOCATION_FRAG_STENCIL) {
				program->writes_stencil = 1U;
				continue;
			}

			/* The sample mask goes to the depth target too. */
			if (location == DRV_GPU_IR_LOCATION_SAMPLE_MASK) {
				program->writes_sample_mask = 1U;
				continue;
			}

			/* The second colour. */
			if (location == DRV_GPU_IR_LOCATION_SECOND_COLOR) {
				program->dual_source = 1U;
				program->color_outputs |= 1U << 1;
				continue;
			}

			/* A colour past the targets is refused. */
			if (location >= GFX10_COLOR_TARGETS) {
				error = refuse(lowering, ENOTSUP, "a fragment output that is not compiled yet");
				return error;
			}

			/* A colour at location 1 is remembered for the dual-source check. */
			if (location == 1U)
				writes_color_1 = 1;
			program->color_outputs |= 1U << location;
			continue;
		}

		/*
		 * A pixel program's input: the position, the face, the samples
		 * covered, the point sprite coordinate, the primitive number, or an
		 * attribute.
		 */
		if (location == DRV_GPU_SHADER_LOCATION_FRAG_COORD) {
			program->pixel_inputs |= GFX10_PS_POSITION;
		} else if (location == DRV_GPU_SHADER_LOCATION_FRONT_FACING) {
			program->pixel_inputs |= GFX10_PS_FRONT_FACE;
		} else if (location == DRV_GPU_SHADER_LOCATION_SAMPLE_MASK) {
			program->pixel_inputs |= GFX10_PS_SAMPLE_COVERAGE;
		} else if (location == DRV_GPU_SHADER_LOCATION_POINT_COORD) {
			reads_point_coord = 1;
		} else if (location == DRV_GPU_SHADER_LOCATION_PRIMITIVE_ID) {
			reads_primitive_id = 1;
		} else if (location < AMDRDNA_GFX10_LOCATIONS) {
			program->attribute_inputs |= 1U << location;
			input = find_input(ir, location);
			if (input != NULL && input->flat != 0U) {
				program->flat_inputs |= 1U << location;
			} else if (input != NULL && input->noperspective != 0U) {
				uses_linear = 1;
			}
		} else {
			error = refuse(lowering, ENOTSUP, "a fragment input that is not compiled yet");
			return error;
		}
	}

	/* A pixel program always has the perspective barycentrics; the linear ones when an input needs them. */
	if (ir->stage == DRV_GPU_STAGE_FRAGMENT) {
		program->pixel_inputs |= GFX10_PS_PERSP_CENTER;
		if (uses_linear != 0)
			program->pixel_inputs |= GFX10_PS_LINEAR_CENTER;
	}

	/* The second colour takes colour 1's export. */
	if (program->dual_source != 0U && writes_color_1 != 0) {
		error = refuse(lowering, ENOTSUP, "a second colour with a colour at location 1");
		return error;
	}

	/*
	 * The point sprite coordinate and the primitive number take the
	 * attributes after the highest the program reads: the hardware makes
	 * the coordinate, the primitive number is read flat from the parameter
	 * the vertex program exports it in.
	 */
	next = 0U;
	for (index = 0U; index < AMDRDNA_GFX10_LOCATIONS; index++) {
		if ((program->attribute_inputs & (1U << index)) != 0U)
			next = index + 1U;
	}

	/* A pixel program that reads the point coordinate takes the next attribute. */
	if (reads_point_coord != 0) {
		if (next == AMDRDNA_GFX10_LOCATIONS) {
			error = refuse(lowering, ENOTSUP, "no attribute is left for the point sprite coordinate");
			return error;
		}

		/* Gives the point coordinate its attribute. */
		program->point_coord_attribute = next;
		program->attribute_inputs |= 1U << next;
		next++;
	}

	/* A pixel program that reads the primitive number takes the next attribute. */
	if (reads_primitive_id != 0) {
		if (next == AMDRDNA_GFX10_LOCATIONS) {
			error = refuse(lowering, ENOTSUP, "no attribute is left for the primitive number");
			return error;
		}

		/* Gives the primitive number its attribute. */
		program->primitive_id_attribute = next;
		program->attribute_inputs |= 1U << next;
		program->flat_inputs |= 1U << next;
		next++;
	}

	/* A vertex program asked to export the primitive number keeps its parameter free for it. */
	if (ir->stage == DRV_GPU_STAGE_VERTEX && lowering->options.pass_primitive_number != 0U) {
		if ((program->parameter_outputs & (1U << AMDRDNA_GFX10_PRIMITIVE_ID_PARAMETER)) != 0U) {
			error = refuse(lowering, ENOTSUP, "the primitive number's parameter is an output of the program");
			return error;
		}

		/* Reserves the primitive number's parameter. */
		program->parameter_outputs |= 1U << AMDRDNA_GFX10_PRIMITIVE_ID_PARAMETER;
		program->exports_primitive_id = 1U;
	}

	/* A vertex program asked to run as a primitive shader, which has no primitive number of a vertex to export. */
	if (ir->stage == DRV_GPU_STAGE_VERTEX && lowering->options.primitive_shader != 0U) {
		if (program->exports_primitive_id != 0U) {
			error = refuse(lowering, ENOTSUP, "a primitive shader does not export the primitive number");
			return error;
		}

		/* Marks the program as a primitive shader; its culling part culls, in the vertices' places in LDS. */
		program->primitive_shader = 1U;
		if (lowering->part == GFX10_PART_CULL) {
			program->primitive_culling = 1U;
			program->shared_bytes = GFX10_CULL_PLACES * GFX10_CULL_PLACE_BYTES + 4U * GFX10_CULL_MOST_WAVES;
		}
	}

	/* Succeeded: the interface is known. */
	lowering->index = 0U;
	return 0;
}

/* Finds the interface record of an input location; NULL when the program has none. */
static const struct drv_gpu_shader_ir_io *
find_input(
	const struct drv_gpu_shader_ir *ir,
	uint32_t location)
{
	uint32_t index;

	/* Each input of the interface. */
	for (index = 0U; index < ir->input_count; index++) {
		if (ir->inputs[index].location == location)
			return &ir->inputs[index];
	}

	/* No record. */
	return NULL;
}

/* Reserves count VGPRs for the whole program after those already kept; returns the first. */
static uint32_t
reserve_vgprs(
	struct gfx10_lowering *lowering,
	uint32_t count)
{
	uint32_t first;

	/* The block grows past the registers the allocator hands out. */
	first = lowering->vgpr_first;
	lowering->vgpr_first += count;
	if (lowering->vgpr_first > lowering->vgpr_highest)
		lowering->vgpr_highest = lowering->vgpr_first;

	/* The first register of the block. */
	return first;
}

/*
 * Fetches a vertex program's inputs: the vertex and instance numbers with
 * their bases, and the four channels of each fetched location, by the
 * vertex number or, where the instance mask has the location's bit, by
 * the instance number.  The descriptors were loaded before.
 */
static void
prologue_vertex(
	struct gfx10_lowering *lowering)
{
	struct amdrdna_gfx10_program *program;
	struct amdrdna_gfx10_inst inst;
	uint32_t location;
	unsigned temp;
	unsigned index_vgpr;
	unsigned bit_number;
	unsigned all_lanes;

	/* Names the program being filled. */
	program = lowering->program;

	/* The primitive number into the register its export reads. */
	if (program->exports_primitive_id != 0U) {
		emit_vector(lowering,
			    AMDRDNA_GFX10_KIND_VECTOR_ONE,
			    AMDRDNA_GFX10_V_MOV_B32,
			    AMDRDNA_GFX10_V(lowering->output_reg[GFX10_OUTPUT_LOCATIONS + 4U * AMDRDNA_GFX10_PRIMITIVE_ID_PARAMETER]),
			    AMDRDNA_GFX10_V(GFX10_VS_PRIMITIVE_ID),
			    0U,
			    0U);
	}

	/* The numbers: the delivered vertex number plus the base vertex, the instance number plus the first instance. */
	emit_vector(lowering,
		    AMDRDNA_GFX10_KIND_VECTOR_TWO,
		    AMDRDNA_GFX10_V_ADD_NC_U32,
		    AMDRDNA_GFX10_V(lowering->vertex_index_vgpr),
		    AMDRDNA_GFX10_S(lowering->base_vertex_sgpr),
		    AMDRDNA_GFX10_V(lowering->vertex_id_input),
		    0U);
	emit_vector(lowering,
		    AMDRDNA_GFX10_KIND_VECTOR_TWO,
		    AMDRDNA_GFX10_V_ADD_NC_U32,
		    AMDRDNA_GFX10_V(lowering->instance_index_vgpr),
		    AMDRDNA_GFX10_S(lowering->first_instance_sgpr),
		    AMDRDNA_GFX10_V(lowering->instance_id_input),
		    0U);

	/* Each fetched location. */
	for (location = 0U; location < AMDRDNA_GFX10_LOCATIONS; location++) {
		if ((program->fetched_locations & (1U << location)) == 0U)
			continue;

		/*
		 * Its record number, chosen for every lane: the instance mask's
		 * bit into SCC, SCC into a mask of all lanes or none, the mask
		 * selecting the instance number over the vertex number.  The
		 * selection is made into the location's first channel register,
		 * which the fetch then overwrites.
		 */
		temp = next_scalar_temp(lowering);
		index_vgpr = AMDRDNA_GFX10_V(lowering->input_vgpr[location]);
		bit_number = constant_operand(location);
		all_lanes = constant_operand(GFX10_TRUE);
		emit_scalar(lowering,
			    AMDRDNA_GFX10_KIND_SCALAR_COMPARE,
			    AMDRDNA_GFX10_S_BITCMP1_B32,
			    0U,
			    AMDRDNA_GFX10_S(lowering->instance_mask_sgpr),
			    bit_number);
		emit_scalar(lowering,
			    AMDRDNA_GFX10_KIND_SCALAR_TWO,
			    AMDRDNA_GFX10_S_CSELECT_B32,
			    temp,
			    all_lanes,
			    AMDRDNA_GFX10_ZERO);
		emit_vector(lowering,
			    AMDRDNA_GFX10_KIND_VECTOR_TWO,
			    AMDRDNA_GFX10_V_CNDMASK_B32,
			    index_vgpr,
			    AMDRDNA_GFX10_V(lowering->vertex_index_vgpr),
			    AMDRDNA_GFX10_V(lowering->instance_index_vgpr),
			    temp);

		/* The fetch of the four channels, the format and the missing channels from the descriptor. */
		kern_memset(&inst, 0, sizeof(inst));
		inst.kind = AMDRDNA_GFX10_KIND_BUFFER;
		inst.opcode = AMDRDNA_GFX10_BUFFER_LOAD_FORMAT_XYZW;
		inst.dst = (uint16_t)index_vgpr;
		inst.src[0] = (uint16_t)index_vgpr;
		inst.src[1] = (uint16_t)lowering->vertex_sgpr[location];
		inst.src[2] = AMDRDNA_GFX10_ZERO;
		inst.flags = AMDRDNA_GFX10_FLAG_INDEX_ADDRESS;
		inst.count = 4U;
		emit(lowering, &inst);
	}
}

/*
 * Starts a primitive shader: asks for the space of the subgroup's vertices
 * and primitives (the subgroup's first wave alone), exports the wave's
 * primitives as the hardware packed them, and leaves the lanes of the
 * wave's vertices running for the vertex program.  A wave without
 * primitives or without vertices exports none of them: an export with no
 * lane running is not skipped on GFX10.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/common/nir/ac_nir_lower_ngg.c
 * (ac_nir_lower_ngg_nogs, the passthrough path: the allocation from the
 * subgroup's counts by the first wave, the early export of the packed
 * primitive by each primitive lane, then the vertex program on the vertex
 * lanes), src/amd/common/nir/ac_nir_prerast_utils.c
 * (ac_nir_export_primitive: the primitive's export is marked done), RADV
 * 25.0.7's NGG vertex program as ACO writes it on Navi10 (the primitive
 * export and the vertex part each behind s_cbranch_execz) and
 * src/amd/compiler/aco_instruction_selection.cpp (select_shader: on GFX10
 * an NGG program without a geometry stage starts with s_barrier, so that
 * every wave has launched before the allocation request, a Navi1x
 * hardware bug, lines 11380-11386; a vertex program first raises its wave's
 * priority to 3, line 11374).
 */
static void
prologue_primitive(
	struct gfx10_lowering *lowering)
{
	struct amdrdna_gfx10_inst inst;
	unsigned vertices;
	unsigned primitives;
	unsigned wave_count;
	unsigned sources[4];
	uint32_t skip_label;

	/*
	 * A deferred part starts on the lanes the culling part left running
	 * (none: to the primitives' export at once).
	 */
	if (lowering->part == GFX10_PART_DEFERRED) {
		lowering->vertices_end_label = lowering->next_label;
		lowering->next_label++;
		emit_branch(lowering, AMDRDNA_GFX10_S_CBRANCH_EXECZ, lowering->vertices_end_label);
		return;
	}

	/* The wave's priority raised, as a vertex program's is. */
	kern_memset(&inst, 0, sizeof(inst));
	inst.kind = AMDRDNA_GFX10_KIND_CONTROL;
	inst.opcode = AMDRDNA_GFX10_S_SETPRIO;
	inst.immediate = 3U;
	emit(lowering, &inst);

	/* Every wave of the subgroup launched before the request. */
	kern_memset(&inst, 0, sizeof(inst));
	inst.kind = AMDRDNA_GFX10_KIND_CONTROL;
	inst.opcode = AMDRDNA_GFX10_S_BARRIER;
	emit(lowering, &inst);

	/*
	 * Only the subgroup's first wave asks: the others branch past the
	 * request.  A culling program asks after its culling, for the vertices
	 * it keeps.
	 */
	if (lowering->program->primitive_culling == 0U) {
		skip_label = lowering->next_label;
		lowering->next_label++;
		wave_count = next_scalar_temp(lowering);
		emit_scalar_literal(lowering,
				    AMDRDNA_GFX10_S_AND_B32,
				    wave_count,
				    AMDRDNA_GFX10_S(AMDRDNA_GFX10_NGG_WAVE_INFO),
				    AMDRDNA_GFX10_NGG_WAVE_NUMBER);
		emit_branch(lowering, AMDRDNA_GFX10_S_CBRANCH_SCC1, skip_label);

		/* The subgroup's vertex and primitive counts, the primitives moved to their place in the request. */
		vertices = next_scalar_temp(lowering);
		primitives = next_scalar_temp(lowering);
		emit_field(lowering, vertices, AMDRDNA_GFX10_S(AMDRDNA_GFX10_NGG_SUBGROUP_INFO), AMDRDNA_GFX10_NGG_SUBGROUP_VERTICES);
		emit_field(lowering, primitives, AMDRDNA_GFX10_S(AMDRDNA_GFX10_NGG_SUBGROUP_INFO), AMDRDNA_GFX10_NGG_SUBGROUP_PRIMITIVES);
		emit_scalar(lowering,
			    AMDRDNA_GFX10_KIND_SCALAR_TWO,
			    AMDRDNA_GFX10_S_LSHL_B32,
			    primitives,
			    primitives,
			    constant_operand(AMDRDNA_GFX10_ALLOC_PRIMITIVE_SHIFT));
		emit_scalar(lowering,
			    AMDRDNA_GFX10_KIND_SCALAR_TWO,
			    AMDRDNA_GFX10_S_OR_B32,
			    AMDRDNA_GFX10_M0,
			    vertices,
			    primitives);

		/* The request, which the hardware must have before any export. */
		kern_memset(&inst, 0, sizeof(inst));
		inst.kind = AMDRDNA_GFX10_KIND_CONTROL;
		inst.opcode = AMDRDNA_GFX10_S_SENDMSG;
		inst.immediate = AMDRDNA_GFX10_MESSAGE_GS_ALLOC;
		emit(lowering, &inst);
		emit_label(lowering, skip_label);
	}

	/*
	 * Each primitive lane exports the word the hardware packed in v0,
	 * which ends the primitive's exports; a culling program exports it at
	 * its end.
	 */
	if (lowering->program->primitive_culling == 0U) {
		wave_count = next_scalar_temp(lowering);
		emit_field(lowering, wave_count, AMDRDNA_GFX10_S(AMDRDNA_GFX10_NGG_WAVE_INFO), AMDRDNA_GFX10_NGG_WAVE_PRIMITIVES);
		emit_lanes_below(lowering, wave_count);
		skip_label = lowering->next_label;
		lowering->next_label++;
		emit_branch(lowering, AMDRDNA_GFX10_S_CBRANCH_EXECZ, skip_label);
		sources[0] = AMDRDNA_GFX10_V(AMDRDNA_GFX10_NGG_PRIMITIVE_VGPR);
		sources[1] = 0U;
		sources[2] = 0U;
		sources[3] = 0U;
		emit_export(lowering, AMDRDNA_GFX10_EXPORT_PRIMITIVE, sources, 0x1U, AMDRDNA_GFX10_FLAG_LAST);
		emit_label(lowering, skip_label);
	}

	/*
	 * The vertex lanes run the rest of the program; a wave without
	 * vertices goes to its end at once (a culling one to its primitives).
	 */
	wave_count = next_scalar_temp(lowering);
	emit_field(lowering, wave_count, AMDRDNA_GFX10_S(AMDRDNA_GFX10_NGG_WAVE_INFO), AMDRDNA_GFX10_NGG_WAVE_VERTICES);
	emit_lanes_below(lowering, wave_count);
	lowering->vertices_end_label = lowering->next_label;
	lowering->next_label++;
	emit_branch(lowering, AMDRDNA_GFX10_S_CBRANCH_EXECZ, lowering->vertices_end_label);
}

/*
 * Finds the VGPRs a culling part's position is exported from (in
 * lowering->cull_position).  They stay taken to the end, so the culling's
 * own VGPRs do not overwrite them.
 */
static int
cull_channels(
	struct gfx10_lowering *lowering)
{
	unsigned code;
	uint32_t channel;
	uint32_t reg;
	int error;

	/* The position's four channels, each in a VGPR kept to the end. */
	for (channel = 0U; channel < 4U; channel++) {
		error = export_source(lowering, GFX10_OUTPUT_POSITION + channel, &code);
		if (error != 0)
			return error;
		lowering->cull_position[channel] = (uint16_t)code;
		reg = code - AMDRDNA_GFX10_VGPR_FIRST;
		lowering->taken[reg] = 1U;
		if (reg + 1U > lowering->vgpr_highest)
			lowering->vgpr_highest = reg + 1U;
	}

	/* Succeeded: the position's VGPRs are known. */
	return 0;
}

/* Appends a shared-memory read into dst or a write of data, at a byte offset from the address in a VGPR. */
static void
emit_shared_access(
	struct gfx10_lowering *lowering,
	unsigned opcode,
	unsigned dst,
	uint32_t address,
	unsigned data,
	uint32_t offset)
{
	struct amdrdna_gfx10_inst inst;

	/* The access: a read names its result, a write its data. */
	kern_memset(&inst, 0, sizeof(inst));
	inst.kind = AMDRDNA_GFX10_KIND_SHARED;
	inst.opcode = (uint16_t)opcode;
	inst.dst = (uint16_t)dst;
	inst.src[0] = (uint16_t)AMDRDNA_GFX10_V(address);
	inst.src[1] = (uint16_t)data;
	inst.immediate = offset;
	emit(lowering, &inst);
}

/*
 * Appends the subgroup's meeting point: every lane, once its shared-memory
 * writes are done, waits for every wave of the subgroup.
 */
static void
emit_subgroup_barrier(
	struct gfx10_lowering *lowering)
{
	struct amdrdna_gfx10_inst inst;

	/* Every lane, the writes done, the barrier. */
	emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_ONE, AMDRDNA_GFX10_S_MOV_B32, AMDRDNA_GFX10_EXEC_LO, constant_operand(GFX10_TRUE), 0U);
	kern_memset(&inst, 0, sizeof(inst));
	inst.kind = AMDRDNA_GFX10_KIND_CONTROL;
	inst.opcode = AMDRDNA_GFX10_S_WAITCNT;
	inst.immediate = AMDRDNA_GFX10_WAIT_NONE & ~AMDRDNA_GFX10_WAIT_SCALAR_MASK;
	emit(lowering, &inst);
	kern_memset(&inst, 0, sizeof(inst));
	inst.kind = AMDRDNA_GFX10_KIND_CONTROL;
	inst.opcode = AMDRDNA_GFX10_S_BARRIER;
	emit(lowering, &inst);
}

/* Appends the address of the place of the lane's vertex in the subgroup (the wave's number times 32 and the lane). */
static void
emit_own_place(
	struct gfx10_lowering *lowering,
	uint32_t address)
{
	unsigned first;

	/* The wave's first number, the lane's number, its byte address. */
	first = next_scalar_temp(lowering);
	emit_field(lowering, first, AMDRDNA_GFX10_S(AMDRDNA_GFX10_NGG_WAVE_INFO), AMDRDNA_GFX10_NGG_WAVE_NUMBER_FIELD);
	emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_TWO, AMDRDNA_GFX10_S_LSHL_B32, first, first, constant_operand(5U));
	emit_vector(lowering,
		    AMDRDNA_GFX10_KIND_VECTOR_WIDE,
		    AMDRDNA_GFX10_V_MBCNT_LO_U32_B32,
		    AMDRDNA_GFX10_V(address),
		    constant_operand(GFX10_TRUE),
		    first,
		    0U);
	emit_multiply_add(lowering, AMDRDNA_GFX10_V(address), AMDRDNA_GFX10_V(address), GFX10_CULL_PLACE_BYTES, AMDRDNA_GFX10_ZERO);
}

/*
 * Leaves a culling primitive shader's vertex places for its primitives:
 * each vertex lane writes x / w, y / w and w of its position to LDS at its
 * number in the subgroup, and clears its kept mark.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/common/nir/ac_nir_lower_ngg.c
 * (add_deferred_attribute_culling: the ES threads store the position's
 * w and x / w, y / w at their invocation index and clear their accepted
 * flag, lines 1222-1235).
 */
static int
place_for_culling(
	struct gfx10_lowering *lowering)
{
	unsigned position[4];
	uint32_t address;
	uint32_t reciprocal;
	uint32_t x;
	uint32_t y;
	uint32_t channel;
	int error;

	/* The position's VGPRs. */
	error = cull_channels(lowering);
	if (error != 0)
		return error;
	for (channel = 0U; channel < 4U; channel++)
		position[channel] = lowering->cull_position[channel];

	/* The registers of the place: its address, 1 / w, x / w and y / w (y / w holds the cleared mark after). */
	error = take_vgpr(lowering, &address);
	if (error == 0)
		error = take_vgpr(lowering, &reciprocal);
	if (error == 0)
		error = take_vgpr(lowering, &x);
	if (error == 0)
		error = take_vgpr(lowering, &y);
	if (error != 0)
		return error;

	/* The address; x / w and y / w by the reciprocal of w. */
	emit_own_place(lowering, address);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_ONE, AMDRDNA_GFX10_V_RCP_F32, AMDRDNA_GFX10_V(reciprocal), position[3], 0U, 0U);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_MUL_F32, AMDRDNA_GFX10_V(x), position[0], AMDRDNA_GFX10_V(reciprocal), 0U);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_MUL_F32, AMDRDNA_GFX10_V(y), position[1], AMDRDNA_GFX10_V(reciprocal), 0U);

	/* The three words of the place and the cleared mark. */
	emit_shared_access(lowering, AMDRDNA_GFX10_DS_WRITE_B32, 0U, address, AMDRDNA_GFX10_V(x), GFX10_CULL_X);
	emit_shared_access(lowering, AMDRDNA_GFX10_DS_WRITE_B32, 0U, address, AMDRDNA_GFX10_V(y), GFX10_CULL_Y);
	emit_shared_access(lowering, AMDRDNA_GFX10_DS_WRITE_B32, 0U, address, position[3], GFX10_CULL_W);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_ONE, AMDRDNA_GFX10_V_MOV_B32, AMDRDNA_GFX10_V(reciprocal), AMDRDNA_GFX10_ZERO, 0U, 0U);
	emit_shared_access(lowering, AMDRDNA_GFX10_DS_WRITE_B32, 0U, address, AMDRDNA_GFX10_V(reciprocal), GFX10_CULL_KEPT);

	/* Succeeded: the place is on its way to LDS. */
	return 0;
}

/* Appends dst = a OR b, or dst = b when first is set, of two lane masks. */
static void
emit_mask_or(
	struct gfx10_lowering *lowering,
	unsigned dst,
	unsigned mask,
	int first)
{
	/* The first mask is taken as it is. */
	if (first != 0) {
		emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_ONE, AMDRDNA_GFX10_S_MOV_B32, dst, mask, 0U);
		return;
	}

	/* A later one is added. */
	emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_TWO, AMDRDNA_GFX10_S_OR_B32, dst, dst, mask);
}

/* Appends dst = all lanes when the culling flag `bit` is set, else none. */
static void
emit_flag_mask(
	struct gfx10_lowering *lowering,
	unsigned dst,
	uint32_t bit)
{
	/* The flag into SCC, SCC into the mask. */
	emit_scalar(lowering,
		    AMDRDNA_GFX10_KIND_SCALAR_COMPARE,
		    AMDRDNA_GFX10_S_BITCMP1_B32,
		    0U,
		    AMDRDNA_GFX10_S(AMDRDNA_GFX10_NGG_CULL_FLAGS),
		    constant_operand(bit));
	emit_scalar(lowering,
		    AMDRDNA_GFX10_KIND_SCALAR_TWO,
		    AMDRDNA_GFX10_S_CSELECT_B32,
		    dst,
		    constant_operand(GFX10_TRUE),
		    AMDRDNA_GFX10_ZERO);
}

/* Appends a branch to label when the culling flag `bit` is clear. */
static void
emit_flag_branch(
	struct gfx10_lowering *lowering,
	uint32_t bit,
	uint32_t label)
{
	/* The flag into SCC, the branch on its clear. */
	emit_scalar(lowering,
		    AMDRDNA_GFX10_KIND_SCALAR_COMPARE,
		    AMDRDNA_GFX10_S_BITCMP1_B32,
		    0U,
		    AMDRDNA_GFX10_S(AMDRDNA_GFX10_NGG_CULL_FLAGS),
		    constant_operand(bit));
	emit_branch(lowering, AMDRDNA_GFX10_S_CBRANCH_SCC0, label);
}

/*
 * Tests a culling primitive shader's primitives (the primitive lanes run,
 * their three places read into place[][]): culled gets the lanes whose
 * primitive is culled: when every w is zero, negative or not a number;
 * by its face, or its zero area, when its area is finite; when its box is
 * outside the view, or (with the flag) holds no sample's centre, unless a
 * w is negative.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/common/nir/ac_nir_cull.c
 * (analyze_position_w, cull_face_triangle, calc_bbox_triangle,
 * cull_frustrum, cull_small_primitive_triangle and ac_nir_cull_triangle,
 * lines 21-160).
 */
static void
emit_cull_tests(
	struct gfx10_lowering *lowering,
	uint32_t place[3][3],
	const uint32_t *temp)
{
	unsigned culled;
	unsigned negative;
	unsigned reflected;
	unsigned positive;
	unsigned test;
	unsigned other;
	unsigned front;
	unsigned outside;
	uint32_t vertex;
	uint32_t channel;
	uint32_t index;
	uint32_t small_label;

	/* The lane masks of the tests. */
	culled = AMDRDNA_GFX10_S(lowering->cull_sgpr[GFX10_CULL_CULLED]);
	negative = AMDRDNA_GFX10_S(lowering->cull_sgpr[1]);
	reflected = AMDRDNA_GFX10_S(lowering->cull_sgpr[2]);
	positive = AMDRDNA_GFX10_S(lowering->cull_sgpr[3]);
	test = AMDRDNA_GFX10_S(lowering->cull_sgpr[GFX10_CULL_TEMP]);
	other = AMDRDNA_GFX10_S(lowering->cull_sgpr[5]);
	front = AMDRDNA_GFX10_S(lowering->cull_sgpr[6]);
	outside = AMDRDNA_GFX10_S(lowering->cull_sgpr[7]);
	small_label = lowering->next_label;
	lowering->next_label++;

	/* The w tests: any w above zero, any below, and the reflection of an odd count below. */
	for (vertex = 0U; vertex < 3U; vertex++) {
		emit_vector(lowering,
			    AMDRDNA_GFX10_KIND_VECTOR_COMPARE,
			    AMDRDNA_GFX10_V_CMP_LT_F32,
			    test,
			    AMDRDNA_GFX10_ZERO,
			    AMDRDNA_GFX10_V(place[vertex][2]),
			    0U);
		emit_mask_or(lowering, positive, test, vertex == 0U);
		emit_vector(lowering,
			    AMDRDNA_GFX10_KIND_VECTOR_COMPARE,
			    AMDRDNA_GFX10_V_CMP_LT_F32,
			    test,
			    AMDRDNA_GFX10_V(place[vertex][2]),
			    AMDRDNA_GFX10_ZERO,
			    0U);
		emit_mask_or(lowering, negative, test, vertex == 0U);
		if (vertex == 0U) {
			emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_ONE, AMDRDNA_GFX10_S_MOV_B32, reflected, test, 0U);
		} else {
			emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_TWO, AMDRDNA_GFX10_S_XOR_B32, reflected, reflected, test);
		}
	}

	/* Culled so far: no w above zero. */
	emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_ONE, AMDRDNA_GFX10_S_NOT_B32, culled, positive, 0U);

	/* The determinant (x2 - x0)(y1 - y0) - (x0 - x1)(y0 - y2) in temp[0]. */
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_SUB_F32, AMDRDNA_GFX10_V(temp[0]), AMDRDNA_GFX10_V(place[2][0]), AMDRDNA_GFX10_V(place[0][0]), 0U);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_SUB_F32, AMDRDNA_GFX10_V(temp[1]), AMDRDNA_GFX10_V(place[1][1]), AMDRDNA_GFX10_V(place[0][1]), 0U);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_MUL_F32, AMDRDNA_GFX10_V(temp[0]), AMDRDNA_GFX10_V(temp[0]), AMDRDNA_GFX10_V(temp[1]), 0U);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_SUB_F32, AMDRDNA_GFX10_V(temp[1]), AMDRDNA_GFX10_V(place[0][0]), AMDRDNA_GFX10_V(place[1][0]), 0U);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_SUB_F32, AMDRDNA_GFX10_V(temp[2]), AMDRDNA_GFX10_V(place[0][1]), AMDRDNA_GFX10_V(place[2][1]), 0U);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_MUL_F32, AMDRDNA_GFX10_V(temp[1]), AMDRDNA_GFX10_V(temp[1]), AMDRDNA_GFX10_V(temp[2]), 0U);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_SUB_F32, AMDRDNA_GFX10_V(temp[0]), AMDRDNA_GFX10_V(temp[0]), AMDRDNA_GFX10_V(temp[1]), 0U);

	/* Counter-clockwise: the determinant above zero, or below it under a reflection. */
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_COMPARE, AMDRDNA_GFX10_V_CMP_LT_F32, test, AMDRDNA_GFX10_ZERO, AMDRDNA_GFX10_V(temp[0]), 0U);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_COMPARE, AMDRDNA_GFX10_V_CMP_LT_F32, other, AMDRDNA_GFX10_V(temp[0]), AMDRDNA_GFX10_ZERO, 0U);
	emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_TWO, AMDRDNA_GFX10_S_AND_B32, other, other, reflected);
	emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_TWO, AMDRDNA_GFX10_S_ANDN2_B32, test, test, reflected);
	emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_TWO, AMDRDNA_GFX10_S_OR_B32, front, test, other);

	/* Front facing: counter-clockwise when the front is, else clockwise. */
	emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_ONE, AMDRDNA_GFX10_S_NOT_B32, test, front, 0U);
	emit_scalar(lowering,
		    AMDRDNA_GFX10_KIND_SCALAR_COMPARE,
		    AMDRDNA_GFX10_S_BITCMP1_B32,
		    0U,
		    AMDRDNA_GFX10_S(AMDRDNA_GFX10_NGG_CULL_FLAGS),
		    constant_operand(2U));
	emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_TWO, AMDRDNA_GFX10_S_CSELECT_B32, front, front, test);

	/* The face culled: the front when it is culled, the back when it is, or a zero area. */
	emit_flag_mask(lowering, test, 0U);
	emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_TWO, AMDRDNA_GFX10_S_AND_B32, test, test, front);
	emit_flag_mask(lowering, other, 1U);
	emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_TWO, AMDRDNA_GFX10_S_ANDN2_B32, other, other, front);
	emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_TWO, AMDRDNA_GFX10_S_OR_B32, test, test, other);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_COMPARE, AMDRDNA_GFX10_V_CMP_EQ_F32, other, AMDRDNA_GFX10_V(temp[0]), AMDRDNA_GFX10_ZERO, 0U);
	emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_TWO, AMDRDNA_GFX10_S_OR_B32, test, test, other);

	/* Only for a finite determinant (d - d is zero), then added to the culled. */
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_SUB_F32, AMDRDNA_GFX10_V(temp[1]), AMDRDNA_GFX10_V(temp[0]), AMDRDNA_GFX10_V(temp[0]), 0U);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_COMPARE, AMDRDNA_GFX10_V_CMP_EQ_F32, other, AMDRDNA_GFX10_V(temp[1]), AMDRDNA_GFX10_ZERO, 0U);
	emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_TWO, AMDRDNA_GFX10_S_AND_B32, test, test, other);
	emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_TWO, AMDRDNA_GFX10_S_OR_B32, culled, culled, test);

	/* The box: x from temp[0] to temp[1], y from temp[2] to temp[3]. */
	for (channel = 0U; channel < 2U; channel++) {
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_MIN_F32, AMDRDNA_GFX10_V(temp[2U * channel]), AMDRDNA_GFX10_V(place[0][channel]), AMDRDNA_GFX10_V(place[1][channel]), 0U);
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_MIN_F32, AMDRDNA_GFX10_V(temp[2U * channel]), AMDRDNA_GFX10_V(temp[2U * channel]), AMDRDNA_GFX10_V(place[2][channel]), 0U);
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_MAX_F32, AMDRDNA_GFX10_V(temp[2U * channel + 1U]), AMDRDNA_GFX10_V(place[0][channel]), AMDRDNA_GFX10_V(place[1][channel]), 0U);
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_MAX_F32, AMDRDNA_GFX10_V(temp[2U * channel + 1U]), AMDRDNA_GFX10_V(temp[2U * channel + 1U]), AMDRDNA_GFX10_V(place[2][channel]), 0U);
	}

	/* Outside the view: a box wholly below -1 or above 1 in x or y. */
	for (channel = 0U; channel < 2U; channel++) {
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_COMPARE, AMDRDNA_GFX10_V_CMP_LT_F32, test, AMDRDNA_GFX10_V(temp[2U * channel + 1U]), AMDRDNA_GFX10_MINUS_ONE, 0U);
		emit_mask_or(lowering, outside, test, channel == 0U);
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_COMPARE, AMDRDNA_GFX10_V_CMP_LT_F32, test, AMDRDNA_GFX10_ONE, AMDRDNA_GFX10_V(temp[2U * channel]), 0U);
		emit_mask_or(lowering, outside, test, 0);
	}

	/*
	 * With the flag, a box that holds no sample's centre: its ends on the
	 * screen, widened by the precision, round to the same number.
	 */
	emit_scalar(lowering,
		    AMDRDNA_GFX10_KIND_SCALAR_COMPARE,
		    AMDRDNA_GFX10_S_BITCMP1_B32,
		    0U,
		    AMDRDNA_GFX10_S(AMDRDNA_GFX10_NGG_CULL_FLAGS),
		    constant_operand(3U));
	emit_branch(lowering, AMDRDNA_GFX10_S_CBRANCH_SCC0, small_label);
	for (channel = 0U; channel < 2U; channel++) {
		for (index = 0U; index < 2U; index++) {
			emit_vector(lowering,
				    AMDRDNA_GFX10_KIND_VECTOR_WIDE,
				    AMDRDNA_GFX10_V_FMA_F32,
				    AMDRDNA_GFX10_V(temp[2U * channel + index]),
				    AMDRDNA_GFX10_V(temp[2U * channel + index]),
				    AMDRDNA_GFX10_S(AMDRDNA_GFX10_NGG_CULL_VIEWPORT + channel),
				    AMDRDNA_GFX10_S(AMDRDNA_GFX10_NGG_CULL_VIEWPORT + 2U + channel));
			emit_vector(lowering,
				    AMDRDNA_GFX10_KIND_VECTOR_TWO,
				    index == 0U ? AMDRDNA_GFX10_V_SUB_F32 : AMDRDNA_GFX10_V_ADD_F32,
				    AMDRDNA_GFX10_V(temp[2U * channel + index]),
				    AMDRDNA_GFX10_V(temp[2U * channel + index]),
				    AMDRDNA_GFX10_S(AMDRDNA_GFX10_NGG_CULL_PRECISION),
				    0U);
			emit_vector(lowering,
				    AMDRDNA_GFX10_KIND_VECTOR_ONE,
				    AMDRDNA_GFX10_V_RNDNE_F32,
				    AMDRDNA_GFX10_V(temp[2U * channel + index]),
				    AMDRDNA_GFX10_V(temp[2U * channel + index]),
				    0U,
				    0U);
		}
		emit_vector(lowering,
			    AMDRDNA_GFX10_KIND_VECTOR_COMPARE,
			    AMDRDNA_GFX10_V_CMP_EQ_F32,
			    test,
			    AMDRDNA_GFX10_V(temp[2U * channel]),
			    AMDRDNA_GFX10_V(temp[2U * channel + 1U]),
			    0U);
		emit_mask_or(lowering, outside, test, 0);
	}
	emit_label(lowering, small_label);

	/* The box's test counts only when no w is negative. */
	emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_TWO, AMDRDNA_GFX10_S_ANDN2_B32, outside, outside, negative);
	emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_TWO, AMDRDNA_GFX10_S_OR_B32, culled, culled, outside);

}

/*
 * Ends a culling primitive shader's culling part, once its vertex lanes
 * have left their places: its primitive lanes test their primitives and
 * mark the vertices of those kept; the vertices kept are numbered afresh
 * through the subgroup (each wave's count in LDS, the waves before it
 * summed), the subgroup's first wave asks for the space of the kept
 * vertices and every primitive (none when no vertex is kept: then one
 * degenerate triangle of NaN positions, the GFX10 hang's workaround, and
 * the program ends), each kept vertex leaves the vertex and instance
 * numbers it arrived with at the place of its new number, each primitive
 * lane packs its primitive with the new vertex numbers into v0 (null when
 * it is culled), and the first lanes, one for each new number of the
 * wave, take the numbers left at their places in the VGPRs the hardware
 * delivers them in: the deferred part runs the whole vertex program on
 * them alone.  Every step that reads what another wave wrote waits at the
 * subgroup's barrier first.  Without the culling flag every primitive is
 * kept; the vertex numbers a topology of points or lines does not have
 * are left out.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/common/nir/ac_nir_lower_ngg.c
 * (add_deferred_attribute_culling, lines 1119-1420: the position part
 * first, the kept mark set by each kept primitive, the vertex count of the
 * request, the primitive count all or none, the vertex and instance
 * numbers repacked and the vertex program's arguments overwritten with
 * them; compact_vertices_after_culling, lines 600-690: the new number
 * stored at the old place and the repacked arguments at the new; without
 * the passthrough mode the vertex numbers in the 16-bit halves of v0 and
 * v1, lines 115-128), src/amd/common/nir/ac_nir.c
 * (ac_nir_repack_invocations_in_workgroup: each wave's count in LDS, the
 * new number as the waves before it and mbcnt of its kept lanes, lines
 * 746-860), src/amd/common/nir/ac_nir_prerast_utils.c
 * (ac_nir_ngg_alloc_vertices_fully_culled_workaround, lines 851-889;
 * ac_nir_pack_ngg_prim_exp_arg: 9 bits each, 10 apart, the null bit 31),
 * src/amd/common/nir/ac_nir_lower_intrinsics_to_args.c (the subgroup's
 * wave count in bits 31:28 of merged_wave_info, line 97) and
 * src/amd/compiler/aco_insert_waitcnt.cpp (a workgroup barrier waits for
 * the shared-memory stores first).
 */
static int
cull_and_compact(
	struct gfx10_lowering *lowering)
{
	struct amdrdna_gfx10_inst inst;
	unsigned sources[4];
	unsigned kept;
	unsigned vertex_kept;
	unsigned before;
	unsigned live;
	unsigned scratch;
	unsigned count;
	unsigned waves;
	unsigned wave_count;
	uint32_t place[3][3];
	uint32_t temp[4];
	uint32_t vertex_place[3];
	uint32_t own;
	uint32_t number;
	uint32_t word;
	uint32_t data;
	uint32_t vertex;
	uint32_t channel;
	uint32_t index;
	uint32_t wave;
	uint32_t end_label;
	uint32_t skip_label;
	uint32_t tested_label;
	uint32_t empty_label;
	int error;

	/* The scalar registers: the kept primitives, the kept vertices, the waves' counts. */
	kept = AMDRDNA_GFX10_S(lowering->cull_sgpr[GFX10_CULL_KEPT_PRIMITIVES]);
	vertex_kept = AMDRDNA_GFX10_S(lowering->cull_sgpr[GFX10_CULL_KEPT_VERTICES]);
	before = AMDRDNA_GFX10_S(lowering->cull_sgpr[GFX10_CULL_BEFORE]);
	live = AMDRDNA_GFX10_S(lowering->cull_sgpr[GFX10_CULL_LIVE]);
	scratch = AMDRDNA_GFX10_S(lowering->cull_sgpr[GFX10_CULL_SCRATCH]);
	count = AMDRDNA_GFX10_S(lowering->cull_sgpr[GFX10_CULL_COUNT]);
	waves = AMDRDNA_GFX10_S(lowering->cull_sgpr[GFX10_CULL_WAVE_MASK]);

	/* The VGPRs: the places, four temporaries, the three vertex places, the own place, a number, a value; the word is v0. */
	error = 0;
	for (vertex = 0U; vertex < 3U && error == 0; vertex++) {
		for (channel = 0U; channel < 3U && error == 0; channel++)
			error = take_vgpr(lowering, &place[vertex][channel]);
	}
	for (index = 0U; index < 4U && error == 0; index++)
		error = take_vgpr(lowering, &temp[index]);
	for (vertex = 0U; vertex < 3U && error == 0; vertex++)
		error = take_vgpr(lowering, &vertex_place[vertex]);
	if (error == 0)
		error = take_vgpr(lowering, &own);
	if (error == 0)
		error = take_vgpr(lowering, &number);
	if (error == 0)
		error = take_vgpr(lowering, &data);
	if (error != 0)
		return error;
	word = AMDRDNA_GFX10_NGG_PRIMITIVE_VGPR;
	end_label = lowering->next_label;
	tested_label = lowering->next_label + 1U;
	empty_label = lowering->next_label + 2U;
	lowering->next_label += 3U;

	/* The places are written; every lane's own place. */
	emit_subgroup_barrier(lowering);
	emit_own_place(lowering, own);
	emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_ONE, AMDRDNA_GFX10_S_MOV_B32, kept, AMDRDNA_GFX10_ZERO, 0U);

	/*
	 * The primitive lanes: their vertex numbers (the halves of v0, the low
	 * half of v1) and places; with the culling flag their tests, else
	 * every primitive is kept; then the kept ones mark their vertices.
	 */
	skip_label = lowering->next_label;
	lowering->next_label++;
	wave_count = next_scalar_temp(lowering);
	emit_field(lowering, wave_count, AMDRDNA_GFX10_S(AMDRDNA_GFX10_NGG_WAVE_INFO), AMDRDNA_GFX10_NGG_WAVE_PRIMITIVES);
	emit_lanes_below(lowering, wave_count);
	emit_branch(lowering, AMDRDNA_GFX10_S_CBRANCH_EXECZ, skip_label);
	for (vertex = 0U; vertex < 3U; vertex++) {
		emit_vector(lowering,
			    AMDRDNA_GFX10_KIND_VECTOR_TWO,
			    AMDRDNA_GFX10_V_LSHRREV_B32,
			    AMDRDNA_GFX10_V(vertex_place[vertex]),
			    constant_operand(vertex == 1U ? 16U : 0U),
			    AMDRDNA_GFX10_V(vertex == 2U ? AMDRDNA_GFX10_NGG_VERTEX_PAIR_VGPR : AMDRDNA_GFX10_NGG_PRIMITIVE_VGPR),
			    0U);
		emit_vector_literal(lowering,
				    AMDRDNA_GFX10_KIND_VECTOR_TWO,
				    AMDRDNA_GFX10_V_AND_B32,
				    AMDRDNA_GFX10_V(vertex_place[vertex]),
				    AMDRDNA_GFX10_LITERAL,
				    AMDRDNA_GFX10_V(vertex_place[vertex]),
				    AMDRDNA_GFX10_NGG_VERTEX_NUMBER_MASK);
		emit_multiply_add(lowering,
				  AMDRDNA_GFX10_V(vertex_place[vertex]),
				  AMDRDNA_GFX10_V(vertex_place[vertex]),
				  GFX10_CULL_PLACE_BYTES,
				  AMDRDNA_GFX10_ZERO);
	}
	emit_flag_branch(lowering, GFX10_CULL_FLAG_BIT_ENABLED, tested_label);
	for (vertex = 0U; vertex < 3U; vertex++) {
		for (channel = 0U; channel < 3U; channel++)
			emit_shared_access(lowering, AMDRDNA_GFX10_DS_READ_B32, AMDRDNA_GFX10_V(place[vertex][channel]), vertex_place[vertex], 0U,
					   GFX10_CULL_X + 4U * channel);
	}
	emit_cull_tests(lowering, place, temp);
	emit_label(lowering, tested_label);
	emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_TWO, AMDRDNA_GFX10_S_ANDN2_B32, kept, AMDRDNA_GFX10_EXEC_LO, kept);

	/* The kept primitives mark the vertices they have (one for points, two for lines). */
	emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_ONE, AMDRDNA_GFX10_S_MOV_B32, AMDRDNA_GFX10_EXEC_LO, kept, 0U);
	emit_branch(lowering, AMDRDNA_GFX10_S_CBRANCH_EXECZ, skip_label);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_ONE, AMDRDNA_GFX10_V_MOV_B32, AMDRDNA_GFX10_V(data), constant_operand(1U), 0U, 0U);
	for (vertex = 0U; vertex < 3U; vertex++) {
		if (vertex != 0U)
			emit_flag_branch(lowering, GFX10_CULL_FLAG_BIT_VERTEX_1 + vertex - 1U, skip_label);
		emit_shared_access(lowering, AMDRDNA_GFX10_DS_WRITE_B32, 0U, vertex_place[vertex], AMDRDNA_GFX10_V(data), GFX10_CULL_KEPT);
	}
	emit_label(lowering, skip_label);
	emit_subgroup_barrier(lowering);

	/* The vertex lanes read their marks: the kept vertices of the wave. */
	emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_ONE, AMDRDNA_GFX10_S_MOV_B32, vertex_kept, AMDRDNA_GFX10_ZERO, 0U);
	skip_label = lowering->next_label;
	lowering->next_label++;
	wave_count = next_scalar_temp(lowering);
	emit_field(lowering, wave_count, AMDRDNA_GFX10_S(AMDRDNA_GFX10_NGG_WAVE_INFO), AMDRDNA_GFX10_NGG_WAVE_VERTICES);
	emit_lanes_below(lowering, wave_count);
	emit_branch(lowering, AMDRDNA_GFX10_S_CBRANCH_EXECZ, skip_label);
	emit_shared_access(lowering, AMDRDNA_GFX10_DS_READ_B32, AMDRDNA_GFX10_V(data), own, 0U, GFX10_CULL_KEPT);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_COMPARE, AMDRDNA_GFX10_V_CMP_NE_U32, vertex_kept, AMDRDNA_GFX10_ZERO, AMDRDNA_GFX10_V(data), 0U);
	emit_label(lowering, skip_label);

	/* The wave's count of kept vertices, written by its first lane after the vertices' places. */
	emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_ONE, AMDRDNA_GFX10_S_BCNT1_I32_B32, count, vertex_kept, 0U);
	emit_field(lowering, scratch, AMDRDNA_GFX10_S(AMDRDNA_GFX10_NGG_WAVE_INFO), AMDRDNA_GFX10_NGG_WAVE_NUMBER_FIELD);
	emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_TWO, AMDRDNA_GFX10_S_LSHL_B32, scratch, scratch, constant_operand(2U));
	emit_scalar_literal(lowering, AMDRDNA_GFX10_S_ADD_U32, scratch, scratch, GFX10_CULL_PLACES * GFX10_CULL_PLACE_BYTES);
	emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_ONE, AMDRDNA_GFX10_S_MOV_B32, AMDRDNA_GFX10_EXEC_LO, constant_operand(1U), 0U);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_ONE, AMDRDNA_GFX10_V_MOV_B32, AMDRDNA_GFX10_V(number), scratch, 0U, 0U);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_ONE, AMDRDNA_GFX10_V_MOV_B32, AMDRDNA_GFX10_V(data), count, 0U, 0U);
	emit_shared_access(lowering, AMDRDNA_GFX10_DS_WRITE_B32, 0U, number, AMDRDNA_GFX10_V(data), 0U);
	emit_subgroup_barrier(lowering);

	/*
	 * Every wave's count: the kept vertices of the waves before this one
	 * (the new numbers' start) and of every wave of the subgroup.
	 */
	emit_field(lowering, waves, AMDRDNA_GFX10_S(AMDRDNA_GFX10_NGG_WAVE_INFO), AMDRDNA_GFX10_NGG_WAVE_NUMBER_FIELD);
	emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_TWO, AMDRDNA_GFX10_S_BFM_B32, waves, waves, AMDRDNA_GFX10_ZERO);
	emit_field(lowering, scratch, AMDRDNA_GFX10_S(AMDRDNA_GFX10_NGG_WAVE_INFO), AMDRDNA_GFX10_NGG_WAVE_COUNT_FIELD);
	emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_TWO, AMDRDNA_GFX10_S_BFM_B32, scratch, scratch, AMDRDNA_GFX10_ZERO);
	emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_ONE, AMDRDNA_GFX10_S_MOV_B32, before, AMDRDNA_GFX10_ZERO, 0U);
	emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_ONE, AMDRDNA_GFX10_S_MOV_B32, live, AMDRDNA_GFX10_ZERO, 0U);
	emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_ONE, AMDRDNA_GFX10_S_MOV_B32, AMDRDNA_GFX10_EXEC_LO, constant_operand(1U), 0U);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_ONE, AMDRDNA_GFX10_V_MOV_B32, AMDRDNA_GFX10_V(number), AMDRDNA_GFX10_ZERO, 0U, 0U);
	for (wave = 0U; wave < GFX10_CULL_MOST_WAVES; wave++) {
		emit_shared_access(lowering,
				   AMDRDNA_GFX10_DS_READ_B32,
				   AMDRDNA_GFX10_V(place[wave / 3U][wave % 3U]),
				   number,
				   0U,
				   GFX10_CULL_PLACES * GFX10_CULL_PLACE_BYTES + 4U * wave);
	}
	for (wave = 0U; wave < GFX10_CULL_MOST_WAVES; wave++) {
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_ONE, AMDRDNA_GFX10_V_READFIRSTLANE_B32, count, AMDRDNA_GFX10_V(place[wave / 3U][wave % 3U]), 0U, 0U);
		emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_COMPARE, AMDRDNA_GFX10_S_BITCMP1_B32, 0U, waves, constant_operand(wave));
		emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_TWO, AMDRDNA_GFX10_S_CSELECT_B32, AMDRDNA_GFX10_S(lowering->cull_sgpr[GFX10_CULL_TEMP]), count, AMDRDNA_GFX10_ZERO);
		emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_TWO, AMDRDNA_GFX10_S_ADD_U32, before, before, AMDRDNA_GFX10_S(lowering->cull_sgpr[GFX10_CULL_TEMP]));
		emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_COMPARE, AMDRDNA_GFX10_S_BITCMP1_B32, 0U, scratch, constant_operand(wave));
		emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_TWO, AMDRDNA_GFX10_S_CSELECT_B32, AMDRDNA_GFX10_S(lowering->cull_sgpr[GFX10_CULL_TEMP]), count, AMDRDNA_GFX10_ZERO);
		emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_TWO, AMDRDNA_GFX10_S_ADD_U32, live, live, AMDRDNA_GFX10_S(lowering->cull_sgpr[GFX10_CULL_TEMP]));
	}

	/*
	 * The subgroup's first wave asks for the space: the kept vertices and
	 * every primitive, or, with no vertex kept, one vertex and one primitive
	 * that its first lane exports as a triangle of vertex 0 at a NaN place.
	 */
	skip_label = lowering->next_label;
	lowering->next_label++;
	emit_scalar_literal(lowering, AMDRDNA_GFX10_S_AND_B32, AMDRDNA_GFX10_S(lowering->cull_sgpr[GFX10_CULL_TEMP]), AMDRDNA_GFX10_S(AMDRDNA_GFX10_NGG_WAVE_INFO),
			    AMDRDNA_GFX10_NGG_WAVE_NUMBER);
	emit_branch(lowering, AMDRDNA_GFX10_S_CBRANCH_SCC1, skip_label);
	emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_COMPARE, AMDRDNA_GFX10_S_CMP_EQ_U32, 0U, live, AMDRDNA_GFX10_ZERO);
	emit_branch(lowering, AMDRDNA_GFX10_S_CBRANCH_SCC1, empty_label);
	emit_field(lowering, AMDRDNA_GFX10_S(lowering->cull_sgpr[GFX10_CULL_TEMP]), AMDRDNA_GFX10_S(AMDRDNA_GFX10_NGG_SUBGROUP_INFO), AMDRDNA_GFX10_NGG_SUBGROUP_PRIMITIVES);
	emit_scalar(lowering,
		    AMDRDNA_GFX10_KIND_SCALAR_TWO,
		    AMDRDNA_GFX10_S_LSHL_B32,
		    AMDRDNA_GFX10_S(lowering->cull_sgpr[GFX10_CULL_TEMP]),
		    AMDRDNA_GFX10_S(lowering->cull_sgpr[GFX10_CULL_TEMP]),
		    constant_operand(AMDRDNA_GFX10_ALLOC_PRIMITIVE_SHIFT));
	emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_TWO, AMDRDNA_GFX10_S_OR_B32, AMDRDNA_GFX10_M0, live, AMDRDNA_GFX10_S(lowering->cull_sgpr[GFX10_CULL_TEMP]));
	kern_memset(&inst, 0, sizeof(inst));
	inst.kind = AMDRDNA_GFX10_KIND_CONTROL;
	inst.opcode = AMDRDNA_GFX10_S_SENDMSG;
	inst.immediate = AMDRDNA_GFX10_MESSAGE_GS_ALLOC;
	emit(lowering, &inst);
	emit_branch(lowering, AMDRDNA_GFX10_S_BRANCH, skip_label);

	/* Nothing kept: one vertex, one primitive, exported by the first lane; the program ends. */
	emit_label(lowering, empty_label);
	emit_scalar_literal(lowering, AMDRDNA_GFX10_S_OR_B32, AMDRDNA_GFX10_M0, AMDRDNA_GFX10_ZERO, 1U | (1U << AMDRDNA_GFX10_ALLOC_PRIMITIVE_SHIFT));
	kern_memset(&inst, 0, sizeof(inst));
	inst.kind = AMDRDNA_GFX10_KIND_CONTROL;
	inst.opcode = AMDRDNA_GFX10_S_SENDMSG;
	inst.immediate = AMDRDNA_GFX10_MESSAGE_GS_ALLOC;
	emit(lowering, &inst);
	emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_ONE, AMDRDNA_GFX10_S_MOV_B32, AMDRDNA_GFX10_EXEC_LO, constant_operand(1U), 0U);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_ONE, AMDRDNA_GFX10_V_MOV_B32, AMDRDNA_GFX10_V(word), AMDRDNA_GFX10_ZERO, 0U, 0U);
	sources[0] = AMDRDNA_GFX10_V(word);
	sources[1] = 0U;
	sources[2] = 0U;
	sources[3] = 0U;
	emit_export(lowering, AMDRDNA_GFX10_EXPORT_PRIMITIVE, sources, 0x1U, AMDRDNA_GFX10_FLAG_LAST);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_ONE, AMDRDNA_GFX10_V_MOV_B32, AMDRDNA_GFX10_V(data), constant_operand(GFX10_TRUE), 0U, 0U);
	sources[0] = AMDRDNA_GFX10_V(data);
	sources[1] = AMDRDNA_GFX10_V(data);
	sources[2] = AMDRDNA_GFX10_V(data);
	sources[3] = AMDRDNA_GFX10_V(data);
	emit_export(lowering, AMDRDNA_GFX10_EXPORT_POSITION, sources, 0xfU, AMDRDNA_GFX10_FLAG_LAST | AMDRDNA_GFX10_FLAG_VALID_MASK);
	emit_label(lowering, end_label);
	kern_memset(&inst, 0, sizeof(inst));
	inst.kind = AMDRDNA_GFX10_KIND_CONTROL;
	inst.opcode = AMDRDNA_GFX10_S_ENDPGM;
	emit(lowering, &inst);
	emit_label(lowering, skip_label);

	/* The other waves end too when nothing is kept. */
	emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_COMPARE, AMDRDNA_GFX10_S_CMP_EQ_U32, 0U, live, AMDRDNA_GFX10_ZERO);
	emit_branch(lowering, AMDRDNA_GFX10_S_CBRANCH_SCC1, end_label);

	/*
	 * The kept vertices: the new number (the waves before and the kept
	 * lanes below) at their own place, the vertex and instance numbers
	 * they arrived with at the new number's.
	 */
	skip_label = lowering->next_label;
	lowering->next_label++;
	emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_ONE, AMDRDNA_GFX10_S_MOV_B32, AMDRDNA_GFX10_EXEC_LO, vertex_kept, 0U);
	emit_branch(lowering, AMDRDNA_GFX10_S_CBRANCH_EXECZ, skip_label);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_WIDE, AMDRDNA_GFX10_V_MBCNT_LO_U32_B32, AMDRDNA_GFX10_V(number), vertex_kept, before, 0U);
	emit_shared_access(lowering, AMDRDNA_GFX10_DS_WRITE_B32, 0U, own, AMDRDNA_GFX10_V(number), GFX10_CULL_NUMBER);
	emit_multiply_add(lowering, AMDRDNA_GFX10_V(number), AMDRDNA_GFX10_V(number), GFX10_CULL_PLACE_BYTES, AMDRDNA_GFX10_ZERO);
	emit_shared_access(lowering, AMDRDNA_GFX10_DS_WRITE_B32, 0U, number, AMDRDNA_GFX10_V(AMDRDNA_GFX10_NGG_VERTEX_ID_VGPR), GFX10_CULL_VERTEX_ID);
	emit_shared_access(lowering, AMDRDNA_GFX10_DS_WRITE_B32, 0U, number, AMDRDNA_GFX10_V(AMDRDNA_GFX10_NGG_INSTANCE_ID_VGPR), GFX10_CULL_INSTANCE_ID);
	emit_label(lowering, skip_label);
	emit_subgroup_barrier(lowering);

	/*
	 * The primitive lanes: a kept primitive's new vertex numbers from its
	 * vertices' places, packed into v0; a culled one is null.
	 */
	skip_label = lowering->next_label;
	lowering->next_label++;
	wave_count = next_scalar_temp(lowering);
	emit_field(lowering, wave_count, AMDRDNA_GFX10_S(AMDRDNA_GFX10_NGG_WAVE_INFO), AMDRDNA_GFX10_NGG_WAVE_PRIMITIVES);
	emit_lanes_below(lowering, wave_count);
	emit_branch(lowering, AMDRDNA_GFX10_S_CBRANCH_EXECZ, skip_label);
	emit_vector_literal(lowering,
			    AMDRDNA_GFX10_KIND_VECTOR_ONE,
			    AMDRDNA_GFX10_V_MOV_B32,
			    AMDRDNA_GFX10_V(word),
			    AMDRDNA_GFX10_LITERAL,
			    0U,
			    AMDRDNA_GFX10_NGG_NULL_PRIMITIVE);
	emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_TWO, AMDRDNA_GFX10_S_AND_B32, AMDRDNA_GFX10_EXEC_LO, AMDRDNA_GFX10_EXEC_LO, kept);
	emit_branch(lowering, AMDRDNA_GFX10_S_CBRANCH_EXECZ, skip_label);
	for (vertex = 0U; vertex < 3U; vertex++)
		emit_shared_access(lowering, AMDRDNA_GFX10_DS_READ_B32, AMDRDNA_GFX10_V(place[0][vertex]), vertex_place[vertex], 0U, GFX10_CULL_NUMBER);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_ONE, AMDRDNA_GFX10_V_MOV_B32, AMDRDNA_GFX10_V(word), AMDRDNA_GFX10_V(place[0][0]), 0U, 0U);
	for (vertex = 1U; vertex < 3U; vertex++) {
		emit_vector(lowering,
			    AMDRDNA_GFX10_KIND_VECTOR_TWO,
			    AMDRDNA_GFX10_V_LSHLREV_B32,
			    AMDRDNA_GFX10_V(data),
			    constant_operand(10U * vertex),
			    AMDRDNA_GFX10_V(place[0][vertex]),
			    0U);
		emit_flag_mask(lowering, scratch, GFX10_CULL_FLAG_BIT_VERTEX_1 + vertex - 1U);
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_CNDMASK_B32, AMDRDNA_GFX10_V(data), AMDRDNA_GFX10_ZERO, AMDRDNA_GFX10_V(data), scratch);
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_OR_B32, AMDRDNA_GFX10_V(word), AMDRDNA_GFX10_V(data), AMDRDNA_GFX10_V(word), 0U);
	}
	emit_label(lowering, skip_label);

	/*
	 * The first lanes, one for each new number from the wave's first on,
	 * take the vertex and instance numbers left at their places; the
	 * deferred part runs on these lanes.
	 */
	skip_label = lowering->next_label;
	lowering->next_label++;
	emit_field(lowering, AMDRDNA_GFX10_S(lowering->cull_sgpr[GFX10_CULL_TEMP]), AMDRDNA_GFX10_S(AMDRDNA_GFX10_NGG_WAVE_INFO), AMDRDNA_GFX10_NGG_WAVE_NUMBER_FIELD);
	emit_scalar(lowering,
		    AMDRDNA_GFX10_KIND_SCALAR_TWO,
		    AMDRDNA_GFX10_S_LSHL_B32,
		    AMDRDNA_GFX10_S(lowering->cull_sgpr[GFX10_CULL_TEMP]),
		    AMDRDNA_GFX10_S(lowering->cull_sgpr[GFX10_CULL_TEMP]),
		    constant_operand(5U));
	emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_TWO, AMDRDNA_GFX10_S_SUB_U32, count, live, AMDRDNA_GFX10_S(lowering->cull_sgpr[GFX10_CULL_TEMP]));
	emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_TWO, AMDRDNA_GFX10_S_CSELECT_B32, count, AMDRDNA_GFX10_ZERO, count);
	emit_scalar(lowering, AMDRDNA_GFX10_KIND_SCALAR_TWO, AMDRDNA_GFX10_S_MIN_U32, count, count, constant_operand(32U));
	emit_lanes_below(lowering, count);
	emit_branch(lowering, AMDRDNA_GFX10_S_CBRANCH_EXECZ, skip_label);
	emit_shared_access(lowering, AMDRDNA_GFX10_DS_READ_B32, AMDRDNA_GFX10_V(AMDRDNA_GFX10_NGG_VERTEX_ID_VGPR), own, 0U, GFX10_CULL_VERTEX_ID);
	emit_shared_access(lowering, AMDRDNA_GFX10_DS_READ_B32, AMDRDNA_GFX10_V(AMDRDNA_GFX10_NGG_INSTANCE_ID_VGPR), own, 0U, GFX10_CULL_INSTANCE_ID);
	emit_label(lowering, skip_label);

	/* Succeeded: the deferred part follows on the lanes of the kept vertices. */
	return 0;
}

/*
 * Narrows the lanes run to the first `count` (an SGPR holding 0 .. 32):
 * the mask of that many low bits, or all lanes for 32, which the bit-mask
 * operation cannot make.
 */
static void
emit_lanes_below(
	struct gfx10_lowering *lowering,
	unsigned count)
{
	unsigned mask;

	/* The low bits, and SCC set when every lane is meant. */
	mask = next_scalar_temp(lowering);
	emit_scalar(lowering,
		    AMDRDNA_GFX10_KIND_SCALAR_TWO,
		    AMDRDNA_GFX10_S_BFM_B32,
		    mask,
		    count,
		    AMDRDNA_GFX10_ZERO);
	emit_scalar(lowering,
		    AMDRDNA_GFX10_KIND_SCALAR_COMPARE,
		    AMDRDNA_GFX10_S_CMP_EQ_U32,
		    0U,
		    count,
		    constant_operand(32U));

	/* The lane mask: every lane, or the low bits. */
	emit_scalar(lowering,
		    AMDRDNA_GFX10_KIND_SCALAR_TWO,
		    AMDRDNA_GFX10_S_CSELECT_B32,
		    AMDRDNA_GFX10_EXEC_LO,
		    constant_operand(GFX10_TRUE),
		    mask);
}

/* Appends dst = the unsigned bit field of source that selector (its offset, and its width from bit 16) names. */
static void
emit_field(
	struct gfx10_lowering *lowering,
	unsigned dst,
	unsigned source,
	uint32_t selector)
{
	/* The extraction, the selector as the literal. */
	emit_scalar_literal(lowering, AMDRDNA_GFX10_S_BFE_U32, dst, source, selector);
}

/* Appends dst = source op literal, a two-source scalar operation on a 32-bit constant. */
static void
emit_scalar_literal(
	struct gfx10_lowering *lowering,
	unsigned opcode,
	unsigned dst,
	unsigned source,
	uint32_t literal)
{
	struct amdrdna_gfx10_inst inst;

	/* Builds the operation, the constant as the literal. */
	kern_memset(&inst, 0, sizeof(inst));
	inst.kind = AMDRDNA_GFX10_KIND_SCALAR_TWO;
	inst.opcode = (uint16_t)opcode;
	inst.dst = (uint16_t)dst;
	inst.src[0] = (uint16_t)source;
	inst.src[1] = AMDRDNA_GFX10_LITERAL;
	inst.literal = literal;

	/* Places it. */
	emit(lowering, &inst);
}

/* Lowers the read of a vertex or pixel program's input. */
static int
lower_input(
	struct gfx10_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	const struct drv_gpu_shader_ir_io *input;
	struct amdrdna_gfx10_inst inst;
	uint32_t location;
	uint32_t attribute;
	uint32_t barycentric;
	unsigned source;
	int error;

	/* Names the location the input is read from. */
	location = instruction->location;

	/* A vertex program copies the fetched channel or the number. */
	if (lowering->ir->stage == DRV_GPU_STAGE_VERTEX) {
		if (location == DRV_GPU_SHADER_LOCATION_VERTEX_INDEX) {
			source = AMDRDNA_GFX10_V(lowering->vertex_index_vgpr);
		} else if (location == DRV_GPU_SHADER_LOCATION_INSTANCE_INDEX) {
			source = AMDRDNA_GFX10_V(lowering->instance_index_vgpr);
		} else {
			source = AMDRDNA_GFX10_V(lowering->input_vgpr[location] + instruction->component);
		}

		/* Copies the vertex input to the destination. */
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_ONE, AMDRDNA_GFX10_V_MOV_B32, AMDRDNA_GFX10_V(dst), source, 0U, 0U);
		return 0;
	}

	/* A pixel program's input. */
	if (lowering->ir->stage != DRV_GPU_STAGE_FRAGMENT) {
		error = refuse(lowering, ENOTSUP, "inputs of this stage are not compiled");
		return error;
	}

	/*
	 * The position: x, y and z as delivered, w as 1 / w (the hardware
	 * delivers w).
	 *
	 * Procedure referenced: Mesa 25.0.7
	 * src/amd/vulkan/radv_pipeline_graphics.c (nir_lower_fragcoord_wtrans,
	 * line 2710).
	 */
	if (location == DRV_GPU_SHADER_LOCATION_FRAG_COORD) {
		source = AMDRDNA_GFX10_V(lowering->position_input_vgpr + instruction->component);
		if (instruction->component == 3U) {
			emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_ONE, AMDRDNA_GFX10_V_RCP_F32, AMDRDNA_GFX10_V(dst), source, 0U, 0U);
		} else {
			emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_ONE, AMDRDNA_GFX10_V_MOV_B32, AMDRDNA_GFX10_V(dst), source, 0U, 0U);
		}

		/* Succeeded: the fragment coordinate is copied. */
		return 0;
	}

	/*
	 * The face: the front face where the delivered float is above zero.
	 *
	 * Procedure referenced: Mesa 25.0.7
	 * src/amd/common/nir/ac_nir_lower_intrinsics_to_args.c
	 * (load_front_face: the argument greater than 0, line 282).
	 */
	if (location == DRV_GPU_SHADER_LOCATION_FRONT_FACING) {
		emit_vector(lowering,
			    AMDRDNA_GFX10_KIND_VECTOR_COMPARE,
			    AMDRDNA_GFX10_V_CMP_LT_F32,
			    AMDRDNA_GFX10_VCC_LO,
			    AMDRDNA_GFX10_ZERO,
			    AMDRDNA_GFX10_V(lowering->face_vgpr),
			    0U);
		lowering->vcc_value = instruction->dst;
		if (lowering->condition_only[instruction->dst] == 0U)
			boolean_from_condition(lowering, dst);
		return 0;
	}

	/*
	 * The samples the pixel covers, bit for bit (one sample a pixel: its
	 * bit 0).
	 *
	 * Procedure referenced: Mesa 25.0.7
	 * src/amd/common/nir/ac_nir_lower_intrinsics_to_args.c
	 * (load_sample_mask_in: the sample coverage argument).
	 */
	if (location == DRV_GPU_SHADER_LOCATION_SAMPLE_MASK) {
		source = AMDRDNA_GFX10_V(lowering->coverage_vgpr);
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_ONE, AMDRDNA_GFX10_V_MOV_B32, AMDRDNA_GFX10_V(dst), source, 0U, 0U);
		return 0;
	}

	/* The attribute: the point sprite coordinate's, the primitive number's, or the location's own. */
	attribute = location;
	if (location == DRV_GPU_SHADER_LOCATION_POINT_COORD) {
		attribute = lowering->program->point_coord_attribute;
	} else if (location == DRV_GPU_SHADER_LOCATION_PRIMITIVE_ID) {
		attribute = lowering->program->primitive_id_attribute;
	} else if (location >= AMDRDNA_GFX10_LOCATIONS) {
		error = refuse(lowering, ENOTSUP, "a fragment input that is not compiled yet");
		return error;
	}

	/* An attribute: flat from the provoking vertex, or interpolated in two steps. */
	kern_memset(&inst, 0, sizeof(inst));
	inst.kind = AMDRDNA_GFX10_KIND_INTERPOLATE;
	inst.dst = (uint16_t)AMDRDNA_GFX10_V(dst);
	inst.target = (uint8_t)attribute;
	inst.channel = (uint8_t)instruction->component;
	if ((lowering->program->flat_inputs & (1U << attribute)) != 0U) {
		inst.opcode = AMDRDNA_GFX10_V_INTERP_MOV_F32;
		inst.src[0] = AMDRDNA_GFX10_INTERP_P0;
		emit(lowering, &inst);
		return 0;
	}

	/* The barycentrics: linear for a noperspective input, perspective otherwise (the point sprite coordinate too). */
	barycentric = lowering->perspective_vgpr;
	input = NULL;
	if (location < AMDRDNA_GFX10_LOCATIONS)
		input = find_input(lowering->ir, location);
	if (input != NULL && input->noperspective != 0U)
		barycentric = lowering->screen_linear_vgpr;

	/* The first step with i, the second adding j. */
	inst.opcode = AMDRDNA_GFX10_V_INTERP_P1_F32;
	inst.src[0] = (uint16_t)AMDRDNA_GFX10_V(barycentric);
	emit(lowering, &inst);
	inst.opcode = AMDRDNA_GFX10_V_INTERP_P2_F32;
	inst.src[0] = (uint16_t)AMDRDNA_GFX10_V(barycentric + 1U);
	emit(lowering, &inst);

	/* Succeeded: the input is in dst. */
	return 0;
}

/*
 * Lowers the write of an output: nothing when the export reads the value
 * where it lives, otherwise a copy into the channel's VGPR, taken at the
 * first store and kept to the end.
 */
static int
lower_output(
	struct gfx10_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction)
{
	uint32_t channel;
	uint32_t reg;
	unsigned source;
	int error;

	/* The output channel stored. */
	channel = output_channel(lowering, instruction);
	if (channel == GFX10_OUTPUT_CHANNELS) {
		error = refuse(lowering, ENOTSUP, "outputs of this stage are not compiled");
		return error;
	}

	/* The export reads the value where it lives. */
	if (lowering->output_value[channel] == instruction->src[0])
		return 0;

	/* The channel's own VGPR, taken at its first store. */
	if (lowering->output_reg[channel] == GFX10_NO_REG) {
		error = take_vgpr(lowering, &reg);
		if (error != 0)
			return error;
		lowering->output_reg[channel] = (uint16_t)reg;
	}

	/* The value into it. */
	source = value_operand(lowering, instruction->src[0]);
	emit_vector(lowering,
		    AMDRDNA_GFX10_KIND_VECTOR_ONE,
		    AMDRDNA_GFX10_V_MOV_B32,
		    AMDRDNA_GFX10_V(lowering->output_reg[channel]),
		    source,
		    0U,
		    0U);

	/* Succeeded: the output waits for the export. */
	return 0;
}

/*
 * Lowers a discard: the lanes whose Boolean is true leave the alive mask
 * and the lanes the wave runs; when none is alive the wave takes the early
 * end.
 */
static void
lower_kill(
	struct gfx10_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction)
{
	/* The lanes to discard into VCC_LO, then out of the alive mask (SCC says whether any is left). */
	condition_from_boolean(lowering, instruction->src[0]);
	emit_scalar(lowering,
		    AMDRDNA_GFX10_KIND_SCALAR_TWO,
		    AMDRDNA_GFX10_S_ANDN2_B32,
		    lowering->live_sgpr,
		    lowering->live_sgpr,
		    AMDRDNA_GFX10_VCC_LO);

	/* No lane alive: the early end. */
	emit_branch(lowering, AMDRDNA_GFX10_S_CBRANCH_SCC0, lowering->early_end_label);

	/* The lanes run are only those alive (and their quads' helpers). */
	restrict_to_alive(lowering);
}

/*
 * Narrows the lanes run to those alive, or in whole quads to the quads
 * with a lane alive.
 */
static void
restrict_to_alive(
	struct gfx10_lowering *lowering)
{
	unsigned mask;

	/* The alive lanes, widened to whole quads when the program runs them. */
	mask = lowering->live_sgpr;
	if (lowering->whole_quads != 0) {
		mask = next_scalar_temp(lowering);
		emit_scalar(lowering,
			    AMDRDNA_GFX10_KIND_SCALAR_ONE,
			    AMDRDNA_GFX10_S_WQM_B32,
			    mask,
			    lowering->live_sgpr,
			    0U);
	}

	/* The lanes run keep only those. */
	emit_scalar(lowering,
		    AMDRDNA_GFX10_KIND_SCALAR_TWO,
		    AMDRDNA_GFX10_S_AND_B32,
		    AMDRDNA_GFX10_EXEC_LO,
		    AMDRDNA_GFX10_EXEC_LO,
		    mask);
}

/*
 * Lowers a sample of a sampled image, a gather of four of its texels, a
 * fetch of one texel, or a query of its size.
 *
 * The message's parameters are read by the image's shape (read_sample()),
 * placed in the order the image access takes its address VGPRs
 * (place_addresses()), copied into a block of VGPRs in a row
 * (fill_addresses(), and fill_cube() for the face coordinates of a cube),
 * and the image access is appended.  A pixel program samples with the
 * derivatives its whole quads give; another stage samples level 0 (or the
 * level given).  The channels go to the values from dst, which have
 * consecutive VGPRs: four, or for a depth compare the one result.
 */
static int
lower_sample(
	struct gfx10_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	const struct drv_gpu_shader_ir_uniform *uniform;
	struct amdrdna_gfx10_inst inst;
	struct gfx10_sample sample;
	uint32_t addresses[GFX10_SAMPLE_ADDRESSES];
	uint32_t count;
	uint32_t index;
	uint32_t slot;
	uint32_t block;
	uint32_t inst_count;
	uint32_t zero_level;
	uint32_t image;
	uint32_t sampler;
	uint32_t storage_count;
	int derivatives;
	int storage;
	int error;

	/* Finds the sampled image named by (set, binding) and the element, or the storage image of a size query. */
	slot = GFX10_NO_SLOT;
	storage = 0;
	for (index = 0U; index < lowering->ir->uniform_count; index++) {
		uniform = &lowering->ir->uniforms[index];
		if (uniform->set != instruction->location ||
		    uniform->binding != instruction->immediate ||
		    uniform->element != instruction->element)
			continue;
		if (uniform->kind == DRV_GPU_IR_UNIFORM_SAMPLED_IMAGE) {
			slot = lowering->slot[index];
		} else if (uniform->kind == DRV_GPU_IR_UNIFORM_STORAGE_IMAGE) {
			slot = lowering->slot[index];
			storage = 1;
		}
	}

	/* Refuses a sample of an image the program does not bind. */
	if (slot == GFX10_NO_SLOT) {
		error = refuse(lowering, EINVAL, "a sample names no sampled image");
		return error;
	}

	/* Reads what the sample asks for. */
	error = read_sample(lowering, instruction, &sample);
	if (error != 0)
		return error;

	/* A storage image is only asked its size, through its own descriptor (a cube's as the array of its faces). */
	if (storage != 0 && sample.size_query == 0) {
		error = refuse(lowering, EINVAL, "a storage image read by a sample");
		return error;
	}

	/* A texel buffer is fetched by its index through the buffer descriptor in the image's place. */
	if (sample.buffer != 0) {
		error = fetch_texel_buffer(lowering, &sample, slot, dst);
		return error;
	}

	/* Names the operand code of the constant 0. */
	zero_level = constant_operand(0U);

	/* A sample at the constant level 0 takes the access without a level. */
	if (sample.leveled != 0 &&
	    sample.fetch == 0 &&
	    sample.size_query == 0 &&
	    lowering->operand_code[sample.level] == zero_level) {
		sample.leveled = 0;
		sample.level_is_zero = 1;
	}

	/* Decides whether the sample takes the implicit derivatives a pixel program's whole quads give. */
	derivatives = 0;
	if (lowering->ir->stage == DRV_GPU_STAGE_FRAGMENT &&
	    sample.leveled == 0 &&
	    sample.level_is_zero == 0 &&
	    sample.gradients == 0 &&
	    sample.size_query == 0 &&
	    sample.gathered == 0)
		derivatives = 1;

	/* Refuses a bias where there are no derivatives to move. */
	if (sample.biased != 0 && derivatives == 0) {
		error = refuse(lowering, ENOTSUP, "a biased sample outside a fragment shader");
		return error;
	}

	/* A level of detail is asked of a pixel program's derivatives only. */
	if (sample.lod_query != 0 && lowering->ir->stage != DRV_GPU_STAGE_FRAGMENT) {
		error = refuse(lowering, ENOTSUP, "a level of detail's query outside a fragment shader");
		return error;
	}

	/* Places the address VGPRs in the order the access takes them. */
	count = place_addresses(&sample, addresses);

	/* Takes the coordinates as they are when they are the whole address and already in two VGPRs in a row. */
	if (count == 2U &&
	    addresses[0] == sample.coordinate[0] &&
	    addresses[1] == sample.coordinate[1] &&
	    lowering->operand_code[sample.coordinate[0]] == GFX10_NO_OPERAND &&
	    lowering->operand_code[sample.coordinate[1]] == GFX10_NO_OPERAND &&
	    lowering->vgpr[sample.coordinate[1]] == lowering->vgpr[sample.coordinate[0]] + 1U) {
		block = lowering->vgpr[sample.coordinate[0]];
		inst_count = 2U;
		count = 0U;
	} else {
		/* Takes a block of address VGPRs. */
		error = take_vgpr_block(lowering, count, &block);
		if (error != 0)
			return error;
		inst_count = count;
	}

	/* Fills the address VGPRs in order. */
	error = fill_addresses(lowering, &sample, addresses, count, block);
	if (error != 0)
		return error;

	/* Builds the image access: the opcode by what the sample has; a fetch and a size query take no sampler. */
	kern_memset(&inst, 0, sizeof(inst));
	inst.kind = AMDRDNA_GFX10_KIND_IMAGE;
	inst.opcode = (uint16_t)sample_opcode(&sample, derivatives);
	inst.dst = (uint16_t)AMDRDNA_GFX10_V(dst);
	inst.src[0] = (uint16_t)AMDRDNA_GFX10_V(block);
	if (storage != 0) {
		image = storage_image_descriptor(lowering, slot);
		sampler = AMDRDNA_GFX10_NULL;
	} else {
		texture_descriptors(lowering, slot, &image, &sampler);
	}
	inst.src[1] = (uint16_t)image;
	inst.src[2] = (uint16_t)sampler;
	inst.mask = 0xfU;
	inst.count = (uint8_t)inst_count;
	inst.dimension = (uint8_t)sample_dimension(&sample);
	if (storage != 0)
		inst.dimension = (uint8_t)storage_dimension(instruction->image_shape, &storage_count);

	/* A fetch reads at texel coordinates and, like a size query, without a sampler. */
	if (sample.fetch != 0) {
		inst.src[2] = AMDRDNA_GFX10_NULL;
		inst.flags = AMDRDNA_GFX10_FLAG_UNNORMALIZED;
	}

	/* A size query reads without a sampler. */
	if (sample.size_query != 0)
		inst.src[2] = AMDRDNA_GFX10_NULL;

	/*
	 * A compare returns its one result; a gather reads the one channel
	 * its mask names from each of the four texels; a level of detail's
	 * query returns the level clamped and the one computed.
	 */
	if (sample.compared != 0) {
		inst.mask = 0x1U;
	} else if (sample.gathered != 0) {
		inst.mask = (uint8_t)(1U << sample.channel);
	} else if (sample.lod_query != 0) {
		inst.mask = 0x3U;
	}

	/* Appends the access. */
	emit(lowering, &inst);

	/* Gives the address block back. */
	for (index = 0U; index < count; index++)
		give_vgpr(lowering, block + index);

	/* Succeeded: the channels are in the values from dst. */
	return 0;
}

/*
 * Fetches the texel of a texel buffer at an integer index: the typed
 * buffer load by index of the four channels, which the buffer descriptor
 * the driver puts in the image descriptor's place converts from the
 * view's format (the missing channels as its channel selects say; past
 * its records zero).
 *
 * Procedure referenced: Mesa 25.0.7
 * src/amd/compiler/aco_instruction_selection.cpp (visit_tex, the
 * GLSL_SAMPLER_DIM_BUF case: buffer_load_format_xyzw with idxen, the
 * texel index the address and the offset 0, lines 9372-9407).
 */
static int
fetch_texel_buffer(
	struct gfx10_lowering *lowering,
	const struct gfx10_sample *sample,
	uint32_t slot,
	uint32_t dst)
{
	struct amdrdna_gfx10_inst inst;
	uint32_t image;
	uint32_t sampler;
	uint32_t index_vgpr;
	unsigned operand;
	int error;

	/* The index in a VGPR of its own. */
	error = take_vgpr(lowering, &index_vgpr);
	if (error != 0)
		return error;
	operand = value_operand(lowering, sample->coordinate[0]);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_ONE, AMDRDNA_GFX10_V_MOV_B32, AMDRDNA_GFX10_V(index_vgpr), operand, 0U, 0U);

	/* The load of the four channels by index, through the first four words of the image's place. */
	texture_descriptors(lowering, slot, &image, &sampler);
	kern_memset(&inst, 0, sizeof(inst));
	inst.kind = AMDRDNA_GFX10_KIND_BUFFER;
	inst.opcode = AMDRDNA_GFX10_BUFFER_LOAD_FORMAT_XYZW;
	inst.dst = (uint16_t)AMDRDNA_GFX10_V(dst);
	inst.src[0] = (uint16_t)AMDRDNA_GFX10_V(index_vgpr);
	inst.src[1] = (uint16_t)image;
	inst.src[2] = AMDRDNA_GFX10_ZERO;
	inst.flags = AMDRDNA_GFX10_FLAG_INDEX_ADDRESS;
	inst.count = 4U;
	emit(lowering, &inst);

	/* Gives the index back. */
	give_vgpr(lowering, index_vgpr);

	/* Succeeded: the channels are in the values from dst. */
	return 0;
}

/*
 * Reads what one sample instruction asks of its image: the coordinate
 * (u, v and for a 3D image r, or a cube's direction, or a texel buffer's
 * index), the layer of an array, the sample of a multisampled image, the
 * depth reference, the bias, the level, the gradients, the gather's
 * channel and the constant texel offset.
 *
 * A TEXTURE message takes its parameters in the order of its type
 * (ir.h); the two-coordinate operations name theirs as sources.  A
 * message whose parameters do not match its type and the image's shape,
 * a texel buffer or a multisampled image read other than by a fetch (a
 * multisampled image's size query aside) and a cube with a texel offset
 * are refused.
 */
static int
read_sample(
	struct gfx10_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	struct gfx10_sample *sample)
{
	uint32_t message;
	uint32_t first;
	uint32_t next;
	uint32_t kind;
	uint32_t index;
	uint32_t offset_bits;
	int error;

	/* Starts with a 2D sample of nothing. */
	kern_memset(sample, 0, sizeof(*sample));
	sample->layer = GFX10_NOT_DEFINED;
	sample->coordinate_count = 2U;

	/* Reads a two-coordinate operation: u, v, then the bias or the level; the offset in bits 7:0 of its component. */
	if (instruction->op != DRV_GPU_IR_TEXTURE) {
		sample->coordinate[0] = instruction->src[0];
		sample->coordinate[1] = instruction->src[1];
		if (instruction->op == DRV_GPU_IR_SAMPLE_BIAS) {
			sample->biased = 1;
			sample->bias = instruction->src[2];
		} else if (instruction->op == DRV_GPU_IR_SAMPLE_LOD) {
			sample->leveled = 1;
			sample->level = instruction->src[2];
		}

		/* Unpacks the texel offsets. */
		offset_bits = (instruction->component >> 4) & 0xffU;
		sample->offset[0] = (offset_bits >> 4) & 0xfU;
		sample->offset[1] = offset_bits & 0xfU;

		/* Succeeded: the operation is read. */
		return 0;
	}

	/* A texel buffer and a multisampled image. */
	kind = instruction->image_shape & DRV_GPU_IR_IMAGE_KIND_MASK;
	if (kind == DRV_GPU_IR_IMAGE_BUFFER)
		sample->buffer = 1;
	if ((instruction->image_shape & DRV_GPU_IR_IMAGE_MULTISAMPLED) != 0U)
		sample->multisampled = 1;

	/* Records the shape: a 3D image and a cube have three coordinates. */
	sample->cube = 0;
	if (kind == DRV_GPU_IR_IMAGE_CUBE)
		sample->cube = 1;
	if (kind == DRV_GPU_IR_IMAGE_3D || kind == DRV_GPU_IR_IMAGE_CUBE)
		sample->coordinate_count = 3U;
	sample->volume = 0;
	if (kind == DRV_GPU_IR_IMAGE_3D)
		sample->volume = 1;

	/* The texel offset of u, v and r: bits 19:16, 15:12 and 11:8 of the component. */
	sample->offset[0] = (instruction->component >> 16) & 0xfU;
	sample->offset[1] = (instruction->component >> 12) & 0xfU;
	sample->offset[2] = (instruction->component >> 8) & 0xfU;

	/* Reads the parameters before the coordinate, by the message. */
	message = instruction->component & DRV_GPU_IR_TEXTURE_MESSAGE_MASK;
	first = instruction->src[0];
	next = first;
	switch (message) {
	case DRV_GPU_IR_TEXTURE_SAMPLE:
		break;
	case DRV_GPU_IR_TEXTURE_SAMPLE_BIAS:
		sample->biased = 1;
		sample->bias = next;
		next++;
		break;
	case DRV_GPU_IR_TEXTURE_SAMPLE_LOD:
		sample->leveled = 1;
		sample->level = next;
		next++;
		break;
	case DRV_GPU_IR_TEXTURE_SAMPLE_COMPARE:
		sample->compared = 1;
		sample->reference = next;
		next++;
		break;
	case DRV_GPU_IR_TEXTURE_SAMPLE_BIAS_COMPARE:
		sample->compared = 1;
		sample->reference = next;
		sample->biased = 1;
		sample->bias = next + 1U;
		next += 2U;
		break;
	case DRV_GPU_IR_TEXTURE_SAMPLE_LOD_COMPARE:
		sample->compared = 1;
		sample->reference = next;
		sample->leveled = 1;
		sample->level = next + 1U;
		next += 2U;
		break;
	case DRV_GPU_IR_TEXTURE_SAMPLE_DERIVS:
		sample->gradients = 1;
		break;
	case DRV_GPU_IR_TEXTURE_SAMPLE_DERIV_COMPARE:
		sample->compared = 1;
		sample->reference = next;
		sample->gradients = 1;
		next++;
		break;
	case DRV_GPU_IR_TEXTURE_GATHER4:
		sample->gathered = 1;
		sample->channel = DRV_GPU_IR_TEXTURE_CHANNEL(instruction->component);
		break;
	case DRV_GPU_IR_TEXTURE_GATHER4_COMPARE:
		sample->gathered = 1;
		sample->compared = 1;
		sample->reference = next;
		next++;
		break;
	case DRV_GPU_IR_TEXTURE_LD:
		sample->fetch = 1;
		break;
	case DRV_GPU_IR_TEXTURE_RESINFO:
		sample->size_query = 1;
		sample->leveled = 1;
		sample->level = next;
		next++;
		break;
	case DRV_GPU_IR_TEXTURE_LOD:
		sample->lod_query = 1;
		break;
	default:
		error = refuse(lowering, ENOTSUP, "the texture message is not compiled yet");
		return error;
	}

	/* A texel buffer is only fetched; a multisampled image only fetched or asked its size. */
	if ((sample->buffer != 0 && sample->fetch == 0) ||
	    (sample->multisampled != 0 && sample->fetch == 0 && sample->size_query == 0)) {
		error = refuse(lowering, ENOTSUP, "a texel buffer or a multisampled image read other than by a fetch");
		return error;
	}

	/* A cube sampled with a texel offset is refused. */
	if (sample->cube != 0 && (instruction->component >> 8) != 0U) {
		error = refuse(lowering, ENOTSUP, "a cube sampled with a texel offset");
		return error;
	}

	/*
	 * Reads the coordinate: a texel buffer's index (then the 0 of v and
	 * the level, which the fetch does not take); a fetch's u, v and level
	 * (none of a multisampled image), then r (a 3D image's) or the layer;
	 * a sample's components each with its two gradients when it has them;
	 * a size query's none.
	 */
	if (sample->buffer != 0) {
		sample->coordinate[0] = next;
		sample->coordinate_count = 1U;
		next += 3U;
	} else if (sample->fetch != 0) {
		sample->coordinate[0] = next;
		sample->coordinate[1] = next + 1U;
		sample->leveled = 1;
		sample->level = next + 2U;
		next += 3U;
		if (sample->volume != 0) {
			sample->coordinate[2] = next;
			next++;
		}
	} else if (sample->gradients != 0) {
		for (index = 0U; index < sample->coordinate_count; index++) {
			sample->coordinate[index] = next;
			sample->gradient[index] = next + 1U;
			sample->gradient[sample->coordinate_count + index] = next + 2U;
			next += 3U;
		}
	} else if (sample->size_query == 0) {
		for (index = 0U; index < sample->coordinate_count; index++) {
			sample->coordinate[index] = next;
			next++;
		}
	}

	/* The layer of an array follows the coordinate (a level of detail's query takes none). */
	if ((instruction->image_shape & DRV_GPU_IR_IMAGE_ARRAYED) != 0U && sample->size_query == 0 && sample->lod_query == 0) {
		sample->layer = next;
		next++;
	}

	/* A multisampled image's fetch takes the sample last, and no level. */
	if (sample->multisampled != 0 && sample->fetch != 0) {
		sample->leveled = 0;
		sample->sample_index = next;
		next++;
	}

	/* Records an array whose size is asked: the access then names the array. */
	if ((instruction->image_shape & DRV_GPU_IR_IMAGE_ARRAYED) != 0U)
		sample->arrayed = 1;

	/* Refuses a message whose parameters are not the ones its type and the image's shape take. */
	if (instruction->src[1] != next - first) {
		error = refuse(lowering, ENOTSUP, "a texture message whose parameters do not match the image's shape");
		return error;
	}

	/* Succeeded: the message is read. */
	return 0;
}

/*
 * Places the address VGPRs of a sample in the order the image access
 * takes them, and reports how many there are: the packed offset, the
 * bias, the depth reference, the gradients along x then along y, the
 * coordinate (a cube's face coordinates and face), the layer, the level;
 * a size query takes the level alone.  The offset, the layer and a cube's
 * three are marks that fill_addresses() computes.
 *
 * Procedure referenced: Mesa 25.0.7
 * src/amd/compiler/aco_instruction_selection.cpp (visit_tex: the address
 * order offset, bias, compare, derivatives (all of d/dx, then all of
 * d/dy), coordinates, then the level, lines 9216-9234 and 9411-9427; the
 * size query with the level alone and no sampler, line 9279).
 */
static uint32_t
place_addresses(
	const struct gfx10_sample *sample,
	uint32_t *addresses)
{
	uint32_t count;
	uint32_t index;
	uint32_t packed_offset;

	/* A size query's level. */
	count = 0U;
	if (sample->size_query != 0) {
		addresses[count] = sample->level;
		count++;

		/* Succeeded: the one address. */
		return count;
	}

	/* Reads the packed offset of the sample. */
	packed_offset = sample_offset(sample);

	/* The packed offset, the bias and the reference. */
	if (packed_offset != 0U && sample->fetch == 0) {
		addresses[count] = GFX10_ADDRESS_OFFSET;
		count++;
	}

	/* The bias follows. */
	if (sample->biased != 0) {
		addresses[count] = sample->bias;
		count++;
	}

	/* The reference of a comparison follows. */
	if (sample->compared != 0) {
		addresses[count] = sample->reference;
		count++;
	}

	/* A cube's gradients: the two face coordinates' along x, then along y. */
	if (sample->gradients != 0 && sample->cube != 0) {
		for (index = 0U; index < 4U; index++) {
			addresses[count] = GFX10_ADDRESS_CUBE_GRADIENT;
			count++;
		}
	}

	/* The gradients: each coordinate's along x, then each one's along y. */
	if (sample->gradients != 0 && sample->cube == 0) {
		for (index = 0U; index < 2U * sample->coordinate_count; index++) {
			addresses[count] = sample->gradient[index];
			count++;
		}
	}

	/* The coordinate: a cube's face coordinates and face, or the coordinate's components, then the layer. */
	if (sample->cube != 0) {
		addresses[count] = GFX10_ADDRESS_CUBE;
		addresses[count + 1U] = GFX10_ADDRESS_CUBE;
		addresses[count + 2U] = GFX10_ADDRESS_CUBE;
		count += 3U;
	} else {
		for (index = 0U; index < sample->coordinate_count; index++) {
			addresses[count] = sample->coordinate[index];
			count++;
		}

		/* An array layer follows the coordinates (layer 0 for a level of detail's query). */
		if (sample->layer != GFX10_NOT_DEFINED || (sample->lod_query != 0 && sample->arrayed != 0)) {
			addresses[count] = GFX10_ADDRESS_LAYER;
			count++;
		}

		/* The sample of a multisampled image's fetch. */
		if (sample->multisampled != 0 && sample->fetch != 0) {
			addresses[count] = sample->sample_index;
			count++;
		}
	}

	/* The level last. */
	if (sample->leveled != 0) {
		addresses[count] = sample->level;
		count++;
	}

	/* Succeeded: the addresses are placed. */
	return count;
}

/*
 * Fills a block of address VGPRs: each value copied, the packed offset
 * written, the layer of an array rounded to the nearest integer (a fetch's
 * is an integer already), and a cube's face coordinates computed.
 */
static int
fill_addresses(
	struct gfx10_lowering *lowering,
	const struct gfx10_sample *sample,
	const uint32_t *addresses,
	uint32_t count,
	uint32_t block)
{
	uint32_t packed;
	uint32_t index;
	uint32_t cube;
	uint32_t gradients;
	unsigned layer_opcode;
	unsigned packed_operand;
	unsigned operand;
	int error;

	/* Copies each address into its VGPR. */
	cube = GFX10_NOT_DEFINED;
	gradients = GFX10_NOT_DEFINED;
	packed = sample_offset(sample);
	packed_operand = constant_operand(packed);
	layer_opcode = AMDRDNA_GFX10_V_RNDNE_F32;
	if (sample->fetch != 0)
		layer_opcode = AMDRDNA_GFX10_V_MOV_B32;
	for (index = 0U; index < count; index++) {
		if (addresses[index] == GFX10_ADDRESS_OFFSET) {
			/* The packed offset is a constant. */
			emit_vector_literal(lowering,
					    AMDRDNA_GFX10_KIND_VECTOR_ONE,
					    AMDRDNA_GFX10_V_MOV_B32,
					    AMDRDNA_GFX10_V(block + index),
					    packed_operand,
					    0U,
					    packed);
		} else if (addresses[index] == GFX10_ADDRESS_LAYER && sample->lod_query != 0) {
			/* A level of detail's query of an array names layer 0. */
			emit_vector(lowering,
				    AMDRDNA_GFX10_KIND_VECTOR_ONE,
				    AMDRDNA_GFX10_V_MOV_B32,
				    AMDRDNA_GFX10_V(block + index),
				    AMDRDNA_GFX10_ZERO,
				    0U,
				    0U);
		} else if (addresses[index] == GFX10_ADDRESS_LAYER) {
			/* A sample's layer is the nearest integer to the one given. */
			operand = value_operand(lowering, sample->layer);
			emit_vector(lowering,
				    AMDRDNA_GFX10_KIND_VECTOR_ONE,
				    layer_opcode,
				    AMDRDNA_GFX10_V(block + index),
				    operand,
				    0U,
				    0U);
		} else if (addresses[index] == GFX10_ADDRESS_CUBE) {
			/* The face coordinates and the face are computed together after the others. */
			if (cube == GFX10_NOT_DEFINED)
				cube = block + index;
		} else if (addresses[index] == GFX10_ADDRESS_CUBE_GRADIENT) {
			/* So are a cube's gradients on its face. */
			if (gradients == GFX10_NOT_DEFINED)
				gradients = block + index;
		} else {
			/* Any other address is copied from the value it names. */
			operand = value_operand(lowering, addresses[index]);
			emit_vector(lowering,
				    AMDRDNA_GFX10_KIND_VECTOR_ONE,
				    AMDRDNA_GFX10_V_MOV_B32,
				    AMDRDNA_GFX10_V(block + index),
				    operand,
				    0U,
				    0U);
		}
	}

	/* Computes a cube's face coordinates and face into their three VGPRs, and its gradients. */
	if (cube != GFX10_NOT_DEFINED && gradients != GFX10_NOT_DEFINED) {
		error = fill_cube_gradients(lowering, sample, cube, gradients);
		if (error != 0)
			return error;
	} else if (cube != GFX10_NOT_DEFINED) {
		error = fill_cube(lowering, sample, cube);
		if (error != 0)
			return error;
	}

	/* Succeeded: the address block is filled. */
	return 0;
}

/*
 * Computes the address of a cube sample from its direction into three
 * VGPRs from first: the two coordinates on the face the direction points
 * at, each divided by the length along the face's axis and moved into
 * [1, 2] (the face's coordinate plus 1, as the access takes it), and the
 * face's number, plus 8 times the layer of an array of cubes.
 *
 * Procedure referenced: Mesa 25.0.7
 * src/amd/common/nir/ac_nir_lower_tex.c (prepare_cube_coords: the face
 * coordinates times 1 / |ma| plus 1.5, the face plus the layer times 8,
 * lines 72-145) and src/amd/compiler/aco_instruction_selection.cpp
 * (visit_alu, nir_op_cube_amd: v_cubema_f32, v_cubesc_f32, v_cubetc_f32
 * and v_cubeid_f32, lines 2403-2412).
 */
static int
fill_cube(
	struct gfx10_lowering *lowering,
	const struct gfx10_sample *sample,
	uint32_t first)
{
	unsigned x;
	unsigned y;
	unsigned z;
	unsigned s;
	unsigned t;
	unsigned face;
	unsigned layer_value;
	uint32_t layer;
	int error;

	/* The direction's components, and the three VGPRs the address takes. */
	x = value_operand(lowering, sample->coordinate[0]);
	y = value_operand(lowering, sample->coordinate[1]);
	z = value_operand(lowering, sample->coordinate[2]);
	s = AMDRDNA_GFX10_V(first);
	t = AMDRDNA_GFX10_V(first + 1U);
	face = AMDRDNA_GFX10_V(first + 2U);

	/* The reciprocal of the length along the face's axis (the face VGPR holds it until the face is computed). */
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_WIDE, AMDRDNA_GFX10_V_CUBEMA_F32, face, x, y, z);
	emit_modified(lowering, AMDRDNA_GFX10_V_RCP_F32, face, face, AMDRDNA_GFX10_WIDE_ABS(0));

	/* The face coordinates, scaled and moved into [1, 2]. */
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_WIDE, AMDRDNA_GFX10_V_CUBESC_F32, s, x, y, z);
	emit_fused(lowering, s, s, face, GFX10_CUBE_CENTRE);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_WIDE, AMDRDNA_GFX10_V_CUBETC_F32, t, x, y, z);
	emit_fused(lowering, t, t, face, GFX10_CUBE_CENTRE);

	/* The face's number. */
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_WIDE, AMDRDNA_GFX10_V_CUBEID_F32, face, x, y, z);

	/* An array of cubes adds 8 times the layer, rounded to the nearest integer, to the face. */
	if (sample->layer != GFX10_NOT_DEFINED) {
		error = take_vgpr(lowering, &layer);
		if (error != 0)
			return error;

		/* The layer, rounded to the nearest integer, into the register taken. */
		layer_value = value_operand(lowering, sample->layer);
		emit_vector(lowering,
			    AMDRDNA_GFX10_KIND_VECTOR_ONE,
			    AMDRDNA_GFX10_V_RNDNE_F32,
			    AMDRDNA_GFX10_V(layer),
			    layer_value,
			    0U,
			    0U);
		emit_layer_face(lowering, face, AMDRDNA_GFX10_V(layer));
		give_vgpr(lowering, layer);
	}

	/* Succeeded: the cube's address is computed. */
	return 0;
}

/*
 * Computes the address of a cube sample with explicit gradients into
 * three VGPRs from first, and the gradients on the face into four from
 * gradients: the face coordinates divided by the length along the face's
 * axis (then moved into [1, 2]) and the face's number, as fill_cube()
 * does; and the derivative of each face coordinate along x and along y by
 * the rule of the quotient, from the direction's gradient of the
 * coordinate on the face (its sign by the face) and of the length along
 * the face's axis.  An array of cubes adds 8 times the layer to the face.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/common/nir/ac_nir_lower_tex.c
 * (build_cube_select: the gradient of sc, tc and ma by the face, ma's as
 * twice its absolute value, lines 28-73; prepare_cube_coords: the face
 * coordinates times 1 / |ma|, each gradient deriv_sc / |ma| less
 * deriv_ma / |ma| times the face coordinate, then the face coordinates
 * plus 1.5, lines 75-151).
 */
static int
fill_cube_gradients(
	struct gfx10_lowering *lowering,
	const struct gfx10_sample *sample,
	uint32_t first,
	uint32_t gradients)
{
	uint32_t temp[GFX10_CUBE_TEMPS];
	uint32_t direction[3];
	unsigned x;
	unsigned y;
	unsigned z;
	unsigned s;
	unsigned t;
	unsigned face;
	unsigned layer_value;
	uint32_t index;
	uint32_t layer;
	int error;

	/* The temporaries: 2 ma, 1 / |2 ma|, the face's three signs, and three for each gradient. */
	for (index = 0U; index < GFX10_CUBE_TEMPS; index++) {
		error = take_vgpr(lowering, &temp[index]);
		if (error != 0)
			return error;
	}

	/* The direction's components, and the three VGPRs the address takes. */
	x = value_operand(lowering, sample->coordinate[0]);
	y = value_operand(lowering, sample->coordinate[1]);
	z = value_operand(lowering, sample->coordinate[2]);
	s = AMDRDNA_GFX10_V(first);
	t = AMDRDNA_GFX10_V(first + 1U);
	face = AMDRDNA_GFX10_V(first + 2U);

	/* 2 ma, its reciprocal's absolute value, the face, and the face coordinates over |2 ma|. */
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_WIDE, AMDRDNA_GFX10_V_CUBEMA_F32,
		    AMDRDNA_GFX10_V(temp[GFX10_CUBE_MA]), x, y, z);
	emit_modified(lowering, AMDRDNA_GFX10_V_RCP_F32, AMDRDNA_GFX10_V(temp[GFX10_CUBE_INVERSE]),
		      AMDRDNA_GFX10_V(temp[GFX10_CUBE_MA]), AMDRDNA_GFX10_WIDE_ABS(0));
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_WIDE, AMDRDNA_GFX10_V_CUBEID_F32, face, x, y, z);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_WIDE, AMDRDNA_GFX10_V_CUBESC_F32, s, x, y, z);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_MUL_F32, s, s,
		    AMDRDNA_GFX10_V(temp[GFX10_CUBE_INVERSE]), 0U);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_WIDE, AMDRDNA_GFX10_V_CUBETC_F32, t, x, y, z);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_MUL_F32, t, t,
		    AMDRDNA_GFX10_V(temp[GFX10_CUBE_INVERSE]), 0U);

	/* The sign of ma: 1 where 2 ma >= 0, else -1. */
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_ONE, AMDRDNA_GFX10_V_MOV_B32,
		    AMDRDNA_GFX10_V(temp[GFX10_CUBE_ONE]), AMDRDNA_GFX10_ONE, 0U, 0U);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_ONE, AMDRDNA_GFX10_V_MOV_B32,
		    AMDRDNA_GFX10_V(temp[GFX10_CUBE_SIGN]), AMDRDNA_GFX10_ONE, 0U, 0U);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_COMPARE, AMDRDNA_GFX10_V_CMP_LE_F32, AMDRDNA_GFX10_VCC_LO,
		    AMDRDNA_GFX10_ZERO, AMDRDNA_GFX10_V(temp[GFX10_CUBE_MA]), 0U);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_CNDMASK_B32,
		    AMDRDNA_GFX10_V(temp[GFX10_CUBE_SIGN]), AMDRDNA_GFX10_MINUS_ONE,
		    AMDRDNA_GFX10_V(temp[GFX10_CUBE_SIGN]), AMDRDNA_GFX10_VCC_LO);

	/* sc's sign: -sign on an x face (0, 1), 1 on a y face (2, 3), sign on a z face (4, 5). */
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_SUB_F32,
		    AMDRDNA_GFX10_V(temp[GFX10_CUBE_SC_SIGN]), AMDRDNA_GFX10_ZERO, AMDRDNA_GFX10_V(temp[GFX10_CUBE_SIGN]), 0U);
	emit_compare_face(lowering, AMDRDNA_GFX10_V_CMP_LE_F32, AMDRDNA_GFX10_BITS_TWO, face);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_CNDMASK_B32,
		    AMDRDNA_GFX10_V(temp[GFX10_CUBE_SC_SIGN]), AMDRDNA_GFX10_V(temp[GFX10_CUBE_SC_SIGN]),
		    AMDRDNA_GFX10_V(temp[GFX10_CUBE_ONE]), AMDRDNA_GFX10_VCC_LO);
	emit_compare_face(lowering, AMDRDNA_GFX10_V_CMP_LE_F32, AMDRDNA_GFX10_BITS_FOUR, face);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_CNDMASK_B32,
		    AMDRDNA_GFX10_V(temp[GFX10_CUBE_SC_SIGN]), AMDRDNA_GFX10_V(temp[GFX10_CUBE_SC_SIGN]),
		    AMDRDNA_GFX10_V(temp[GFX10_CUBE_SIGN]), AMDRDNA_GFX10_VCC_LO);

	/* tc's sign: sign on a y face, -1 on the others. */
	emit_compare_face(lowering, AMDRDNA_GFX10_V_CMP_LE_F32, AMDRDNA_GFX10_BITS_TWO, face);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_CNDMASK_B32,
		    AMDRDNA_GFX10_V(temp[GFX10_CUBE_TC_SIGN]), AMDRDNA_GFX10_MINUS_ONE,
		    AMDRDNA_GFX10_V(temp[GFX10_CUBE_SIGN]), AMDRDNA_GFX10_VCC_LO);
	emit_compare_face(lowering, AMDRDNA_GFX10_V_CMP_GT_F32, AMDRDNA_GFX10_BITS_FOUR, face);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_CNDMASK_B32,
		    AMDRDNA_GFX10_V(temp[GFX10_CUBE_TC_SIGN]), AMDRDNA_GFX10_MINUS_ONE,
		    AMDRDNA_GFX10_V(temp[GFX10_CUBE_TC_SIGN]), AMDRDNA_GFX10_VCC_LO);

	/* The gradients along x, then along y. */
	for (index = 0U; index < 3U; index++)
		direction[index] = sample->gradient[index];
	cube_gradient(lowering, direction, temp, first, gradients);
	for (index = 0U; index < 3U; index++)
		direction[index] = sample->gradient[3U + index];
	cube_gradient(lowering, direction, temp, first, gradients + 2U);

	/* The face coordinates moved into [1, 2]. */
	emit_vector_literal(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_ADD_F32, s, AMDRDNA_GFX10_LITERAL, s,
			    GFX10_CUBE_CENTRE);
	emit_vector_literal(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_ADD_F32, t, AMDRDNA_GFX10_LITERAL, t,
			    GFX10_CUBE_CENTRE);

	/* An array of cubes adds 8 times the layer, rounded to the nearest integer, to the face. */
	layer = temp[GFX10_CUBE_MA];
	if (sample->layer != GFX10_NOT_DEFINED) {
		layer_value = value_operand(lowering, sample->layer);
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_ONE, AMDRDNA_GFX10_V_RNDNE_F32, AMDRDNA_GFX10_V(layer),
			    layer_value, 0U, 0U);
		emit_layer_face(lowering, face, AMDRDNA_GFX10_V(layer));
	}

	/* Gives the temporaries back. */
	for (index = 0U; index < GFX10_CUBE_TEMPS; index++)
		give_vgpr(lowering, temp[index]);

	/* Succeeded: the cube's address and gradients are computed. */
	return 0;
}

/*
 * Computes one direction's gradient on a cube's face into out and out + 1:
 * the gradient of sc (the direction's x on a y or z face, z on an x face)
 * and of tc (z on a y face, y on the others) by their signs, and of the
 * length along the axis (twice the absolute value of the gradient's
 * component on the face's axis), each over |2 ma|, the latter times the
 * face coordinate taken off.  The face's number is in the VGPR first + 2,
 * the face coordinates over |2 ma| in first and first + 1.
 */
static void
cube_gradient(
	struct gfx10_lowering *lowering,
	const uint32_t *direction,
	const uint32_t *temp,
	uint32_t first,
	uint32_t out)
{
	struct amdrdna_gfx10_inst inst;
	unsigned face;
	unsigned dx;
	unsigned dy;
	unsigned dz;
	unsigned a;
	unsigned b;
	unsigned c;

	/* The gradient's components, each in a VGPR (a select takes its second source from one). */
	face = AMDRDNA_GFX10_V(first + 2U);
	dx = AMDRDNA_GFX10_V(temp[GFX10_CUBE_DX]);
	dy = AMDRDNA_GFX10_V(temp[GFX10_CUBE_DY]);
	dz = AMDRDNA_GFX10_V(temp[GFX10_CUBE_DZ]);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_ONE, AMDRDNA_GFX10_V_MOV_B32, dx, value_operand(lowering, direction[0]), 0U, 0U);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_ONE, AMDRDNA_GFX10_V_MOV_B32, dy, value_operand(lowering, direction[1]), 0U, 0U);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_ONE, AMDRDNA_GFX10_V_MOV_B32, dz, value_operand(lowering, direction[2]), 0U, 0U);
	a = AMDRDNA_GFX10_V(out);
	b = AMDRDNA_GFX10_V(out + 1U);
	c = AMDRDNA_GFX10_V(temp[GFX10_CUBE_AXIS]);

	/* sc's: z on an x face, x on the others; tc's: z on a y face, y on the others. */
	emit_compare_face(lowering, AMDRDNA_GFX10_V_CMP_GT_F32, AMDRDNA_GFX10_BITS_TWO, face);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_CNDMASK_B32, a, dx, dz, AMDRDNA_GFX10_VCC_LO);
	emit_compare_face(lowering, AMDRDNA_GFX10_V_CMP_LE_F32, AMDRDNA_GFX10_BITS_TWO, face);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_CNDMASK_B32, b, dy, dz, AMDRDNA_GFX10_VCC_LO);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_CNDMASK_B32, c, dx, dy, AMDRDNA_GFX10_VCC_LO);
	emit_compare_face(lowering, AMDRDNA_GFX10_V_CMP_LE_F32, AMDRDNA_GFX10_BITS_FOUR, face);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_CNDMASK_B32, b, b, dy, AMDRDNA_GFX10_VCC_LO);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_CNDMASK_B32, c, c, dz, AMDRDNA_GFX10_VCC_LO);

	/* Their signs, and each over |2 ma|. */
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_MUL_F32, a, a, AMDRDNA_GFX10_V(temp[GFX10_CUBE_SC_SIGN]), 0U);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_MUL_F32, a, a, AMDRDNA_GFX10_V(temp[GFX10_CUBE_INVERSE]), 0U);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_MUL_F32, b, b, AMDRDNA_GFX10_V(temp[GFX10_CUBE_TC_SIGN]), 0U);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_MUL_F32, b, b, AMDRDNA_GFX10_V(temp[GFX10_CUBE_INVERSE]), 0U);

	/* The axis's: twice its absolute value over |2 ma|. */
	kern_memset(&inst, 0, sizeof(inst));
	inst.kind = AMDRDNA_GFX10_KIND_VECTOR_TWO;
	inst.opcode = AMDRDNA_GFX10_V_MUL_F32;
	inst.dst = (uint16_t)c;
	inst.src[0] = (uint16_t)c;
	inst.src[1] = AMDRDNA_GFX10_TWO;
	inst.modifiers = (uint8_t)AMDRDNA_GFX10_WIDE_ABS(0);
	emit(lowering, &inst);
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_MUL_F32, c, c, AMDRDNA_GFX10_V(temp[GFX10_CUBE_INVERSE]), 0U);

	/* Less the axis's times the face coordinate: a - c sc, b - c tc. */
	kern_memset(&inst, 0, sizeof(inst));
	inst.kind = AMDRDNA_GFX10_KIND_VECTOR_WIDE;
	inst.opcode = AMDRDNA_GFX10_V_FMA_F32;
	inst.dst = (uint16_t)a;
	inst.src[0] = (uint16_t)c;
	inst.src[1] = (uint16_t)AMDRDNA_GFX10_V(first);
	inst.src[2] = (uint16_t)a;
	inst.modifiers = (uint8_t)AMDRDNA_GFX10_WIDE_NEG(0);
	emit(lowering, &inst);
	inst.dst = (uint16_t)b;
	inst.src[1] = (uint16_t)AMDRDNA_GFX10_V(first + 1U);
	inst.src[2] = (uint16_t)b;
	emit(lowering, &inst);
}

/* Appends a comparison of a float constant with a cube's face number, into VCC_LO. */
static void
emit_compare_face(
	struct gfx10_lowering *lowering,
	unsigned opcode,
	uint32_t constant,
	uint32_t face)
{
	emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_COMPARE, opcode, AMDRDNA_GFX10_VCC_LO, constant_operand(constant), face, 0U);
}

/* Appends a one-source vector operation with the wide form's modifier bits. */
static void
emit_modified(
	struct gfx10_lowering *lowering,
	unsigned opcode,
	unsigned dst,
	unsigned src,
	unsigned modifiers)
{
	struct amdrdna_gfx10_inst inst;

	/* Builds the instruction; a modifier makes it wide. */
	kern_memset(&inst, 0, sizeof(inst));
	inst.kind = AMDRDNA_GFX10_KIND_VECTOR_ONE;
	inst.opcode = (uint16_t)opcode;
	inst.dst = (uint16_t)dst;
	inst.src[0] = (uint16_t)src;
	inst.modifiers = (uint8_t)modifiers;

	/* Places it. */
	emit(lowering, &inst);
}

/* Appends dst = a * b + the float constant, the fused multiply-add with the constant as the literal. */
static void
emit_fused(
	struct gfx10_lowering *lowering,
	unsigned dst,
	unsigned a,
	unsigned b,
	uint32_t constant)
{
	struct amdrdna_gfx10_inst inst;

	/* Builds the instruction with its literal. */
	kern_memset(&inst, 0, sizeof(inst));
	inst.kind = AMDRDNA_GFX10_KIND_VECTOR_WIDE;
	inst.opcode = AMDRDNA_GFX10_V_FMA_F32;
	inst.dst = (uint16_t)dst;
	inst.src[0] = (uint16_t)a;
	inst.src[1] = (uint16_t)b;
	inst.src[2] = AMDRDNA_GFX10_LITERAL;
	inst.literal = constant;

	/* Places it. */
	emit(lowering, &inst);
}

/* Appends face = layer * 8 + face, an array of cubes' layer folded into the face number. */
static void
emit_layer_face(
	struct gfx10_lowering *lowering,
	unsigned face,
	unsigned layer)
{
	struct amdrdna_gfx10_inst inst;

	/* Builds the instruction with 8.0 as its literal. */
	kern_memset(&inst, 0, sizeof(inst));
	inst.kind = AMDRDNA_GFX10_KIND_VECTOR_WIDE;
	inst.opcode = AMDRDNA_GFX10_V_FMA_F32;
	inst.dst = (uint16_t)face;
	inst.src[0] = (uint16_t)layer;
	inst.src[1] = AMDRDNA_GFX10_LITERAL;
	inst.src[2] = (uint16_t)face;
	inst.literal = GFX10_CUBE_LAYER_STRIDE;

	/* Places it. */
	emit(lowering, &inst);
}

/*
 * Reports a sample's constant texel offset packed as the access takes it
 * (u, v and r 8 bits apart, each from -8 to 7 in 6 bits), 0 for none.
 */
static uint32_t
sample_offset(
	const struct gfx10_sample *sample)
{
	uint32_t packed;
	uint32_t index;
	uint32_t count;
	int32_t moved;

	/* Packs u and v, and a 3D image's r. */
	packed = 0U;
	count = 2U;
	if (sample->volume != 0)
		count = 3U;
	for (index = 0U; index < count; index++) {
		moved = (int32_t)sample->offset[index];
		if (moved >= 8)
			moved -= 16;
		packed |= ((uint32_t)moved & GFX10_OFFSET_CHANNEL_MASK) << (GFX10_OFFSET_CHANNEL_SHIFT * index);
	}

	/* Succeeded: the packed offset. */
	return packed;
}

/*
 * Reports the dimension field of a sample's access: a 2D image, an array
 * of them, a 3D image, a cube, or a multisampled 2D image or an array of
 * them.
 */
static unsigned
sample_dimension(
	const struct gfx10_sample *sample)
{
	/* A cube, an array of cubes included. */
	if (sample->cube != 0)
		return AMDRDNA_GFX10_IMAGE_DIM_CUBE;

	/* A 3D image. */
	if (sample->volume != 0)
		return AMDRDNA_GFX10_IMAGE_DIM_3D;

	/* A multisampled 2D image, or an array of them. */
	if (sample->multisampled != 0 && sample->arrayed != 0)
		return AMDRDNA_GFX10_IMAGE_DIM_2D_MSAA_ARRAY;
	if (sample->multisampled != 0)
		return AMDRDNA_GFX10_IMAGE_DIM_2D_MSAA;

	/* An array of 2D images. */
	if (sample->arrayed != 0)
		return AMDRDNA_GFX10_IMAGE_DIM_2D_ARRAY;

	/* Succeeded: a 2D image. */
	return AMDRDNA_GFX10_IMAGE_DIM_2D;
}

/*
 * Picks the image opcode of a sample by what it has: a gather, a compare,
 * gradients, a level, a bias, the implicit derivatives, an offset.
 */
static unsigned
sample_opcode(
	const struct gfx10_sample *sample,
	int derivatives)
{
	uint32_t packed_offset;
	int offset;

	/* A level of detail's query. */
	if (sample->lod_query != 0)
		return AMDRDNA_GFX10_IMAGE_GET_LOD;

	/* A fetch (a multisampled image's without a level) and a size query. */
	if (sample->fetch != 0 && sample->multisampled != 0)
		return AMDRDNA_GFX10_IMAGE_LOAD;
	if (sample->fetch != 0)
		return AMDRDNA_GFX10_IMAGE_LOAD_MIP;
	if (sample->size_query != 0)
		return AMDRDNA_GFX10_IMAGE_GET_RESINFO;

	/* Reads the packed offset of the sample. */
	packed_offset = sample_offset(sample);

	/* Whether the access carries an offset. */
	offset = 0;
	if (packed_offset != 0U)
		offset = 1;

	/* A gather at level 0. */
	if (sample->gathered != 0) {
		/* A gather that compares against a depth reference. */
		if (sample->compared != 0) {
			if (offset != 0)
				return AMDRDNA_GFX10_IMAGE_GATHER4_C_LZ_O;
			return AMDRDNA_GFX10_IMAGE_GATHER4_C_LZ;
		}

		/* A plain gather. */
		if (offset != 0)
			return AMDRDNA_GFX10_IMAGE_GATHER4_LZ_O;
		return AMDRDNA_GFX10_IMAGE_GATHER4_LZ;
	}

	/* A depth compare. */
	if (sample->compared != 0) {
		/* A compare with gradients. */
		if (sample->gradients != 0) {
			if (offset != 0)
				return AMDRDNA_GFX10_IMAGE_SAMPLE_C_D_O;
			return AMDRDNA_GFX10_IMAGE_SAMPLE_C_D;
		}

		/* A compare at a level. */
		if (sample->leveled != 0) {
			if (offset != 0)
				return AMDRDNA_GFX10_IMAGE_SAMPLE_C_L_O;
			return AMDRDNA_GFX10_IMAGE_SAMPLE_C_L;
		}

		/* A compare with a bias. */
		if (sample->biased != 0) {
			if (offset != 0)
				return AMDRDNA_GFX10_IMAGE_SAMPLE_C_B_O;
			return AMDRDNA_GFX10_IMAGE_SAMPLE_C_B;
		}

		/* A compare with the implicit derivatives. */
		if (derivatives != 0) {
			if (offset != 0)
				return AMDRDNA_GFX10_IMAGE_SAMPLE_C_O;
			return AMDRDNA_GFX10_IMAGE_SAMPLE_C;
		}

		/* A compare at level 0. */
		if (offset != 0)
			return AMDRDNA_GFX10_IMAGE_SAMPLE_C_LZ_O;
		return AMDRDNA_GFX10_IMAGE_SAMPLE_C_LZ;
	}

	/* A plain sample with gradients. */
	if (sample->gradients != 0) {
		if (offset != 0)
			return AMDRDNA_GFX10_IMAGE_SAMPLE_D_O;
		return AMDRDNA_GFX10_IMAGE_SAMPLE_D;
	}

	/* A plain sample at a level. */
	if (sample->leveled != 0) {
		if (offset != 0)
			return AMDRDNA_GFX10_IMAGE_SAMPLE_L_O;
		return AMDRDNA_GFX10_IMAGE_SAMPLE_L;
	}

	/* A plain sample with a bias. */
	if (sample->biased != 0) {
		if (offset != 0)
			return AMDRDNA_GFX10_IMAGE_SAMPLE_B_O;
		return AMDRDNA_GFX10_IMAGE_SAMPLE_B;
	}

	/* A plain sample with the implicit derivatives. */
	if (derivatives != 0) {
		if (offset != 0)
			return AMDRDNA_GFX10_IMAGE_SAMPLE_O;
		return AMDRDNA_GFX10_IMAGE_SAMPLE;
	}

	/* A sample at level 0 with an offset. */
	if (offset != 0)
		return AMDRDNA_GFX10_IMAGE_SAMPLE_LZ_O;

	/* Succeeded: a sample at level 0. */
	return AMDRDNA_GFX10_IMAGE_SAMPLE_LZ;
}

/*
 * Lowers a difference across the pixel's group of four (a pixel
 * program's derivative): every lane takes the value in the lane the
 * difference ends at, less the one in the lane it starts at, both read
 * across the group.  A value the same in every lane has no difference.
 *
 * Procedure referenced: Mesa 25.0.7
 * src/amd/compiler/aco_instruction_selection.cpp (visit_intrinsic, the
 * derivatives: the values read across the quad by two lane-permuted
 * vector operations, lines 8027-8100).
 */
static int
lower_derivative(
	struct gfx10_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	struct amdrdna_gfx10_inst inst;
	unsigned source;
	unsigned start;
	unsigned end;
	unsigned zero;
	int error;

	/* Only a pixel program's lanes are laid out in groups of four. */
	if (lowering->ir->stage != DRV_GPU_STAGE_FRAGMENT) {
		error = refuse(lowering, EINVAL, "a derivative outside a fragment shader");
		return error;
	}

	/* A value without a VGPR of its own is uniform: its difference is 0. */
	if (lowering->operand_code[instruction->src[0]] != GFX10_NO_OPERAND) {
		zero = constant_operand(0U);
		emit_vector(lowering,
			    AMDRDNA_GFX10_KIND_VECTOR_ONE,
			    AMDRDNA_GFX10_V_MOV_B32,
			    AMDRDNA_GFX10_V(dst),
			    zero,
			    0U,
			    0U);
		return 0;
	}

	/* The lanes the difference starts and ends at, by the operation. */
	if (instruction->op == DRV_GPU_IR_DDX) {
		start = AMDRDNA_GFX10_QUAD_ALL_LANE0;
		end = AMDRDNA_GFX10_QUAD_ALL_LANE1;
	} else if (instruction->op == DRV_GPU_IR_DDY) {
		start = AMDRDNA_GFX10_QUAD_ALL_LANE0;
		end = AMDRDNA_GFX10_QUAD_ALL_LANE2;
	} else if (instruction->op == DRV_GPU_IR_DDX_FINE) {
		start = AMDRDNA_GFX10_QUAD_ROW_LEFT;
		end = AMDRDNA_GFX10_QUAD_ROW_RIGHT;
	} else {
		start = AMDRDNA_GFX10_QUAD_COLUMN_TOP;
		end = AMDRDNA_GFX10_QUAD_COLUMN_BOTTOM;
	}

	/* Copies the start's value into the destination. */
	source = AMDRDNA_GFX10_V(lowering->vgpr[instruction->src[0]]);
	kern_memset(&inst, 0, sizeof(inst));
	inst.kind = AMDRDNA_GFX10_KIND_VECTOR_ONE;
	inst.opcode = AMDRDNA_GFX10_V_MOV_B32;
	inst.dst = (uint16_t)AMDRDNA_GFX10_V(dst);
	inst.src[0] = (uint16_t)source;
	inst.quad_read = 1U;
	inst.quad_pattern = (uint8_t)start;
	emit(lowering, &inst);

	/* Takes the end's value less the start's. */
	kern_memset(&inst, 0, sizeof(inst));
	inst.kind = AMDRDNA_GFX10_KIND_VECTOR_TWO;
	inst.opcode = AMDRDNA_GFX10_V_SUB_F32;
	inst.dst = (uint16_t)AMDRDNA_GFX10_V(dst);
	inst.src[0] = (uint16_t)source;
	inst.src[1] = (uint16_t)AMDRDNA_GFX10_V(dst);
	inst.quad_read = 1U;
	inst.quad_pattern = (uint8_t)end;
	emit(lowering, &inst);

	/* Succeeded: the difference is in dst. */
	return 0;
}

/*
 * Exports a graphics program's results before it ends.
 *
 * A vertex program exports its position first (with the valid mask bit,
 * which Navi1x needs on position 0), its point size as position 1, the
 * last of them marked last; then each parameter.  A pixel program
 * exports each colour (to the colour exports in order, no holes), the
 * last marked last and valid, or nothing marked
 * so when it writes none; the lanes alive are those exported.
 *
 * Procedure referenced: Mesa 25.0.7
 * src/amd/common/nir/ac_nir_prerast_utils.c (ac_nir_export_position: the
 * position exports first, valid_mask on POS0 for GFX10, lines 235-300).
 */
static int
finish_exports(
	struct gfx10_lowering *lowering)
{
	struct amdrdna_gfx10_program *program;
	unsigned nothing[4];
	uint32_t location;
	uint32_t channel;
	uint32_t mask;
	uint32_t last;
	uint32_t index;
	uint32_t flags;
	uint32_t kind;
	int error;

	/* Names the program being exported. */
	program = lowering->program;
	nothing[0] = 0U;
	nothing[1] = 0U;
	nothing[2] = 0U;
	nothing[3] = 0U;

	/* A vertex program: the positions, then the parameters. */
	if (lowering->ir->stage == DRV_GPU_STAGE_VERTEX) {
		flags = AMDRDNA_GFX10_FLAG_VALID_MASK;
		if (program->writes_point_size == 0U)
			flags |= AMDRDNA_GFX10_FLAG_LAST;
		error = export_location(lowering, AMDRDNA_GFX10_EXPORT_POSITION, GFX10_OUTPUT_POSITION, 0xfU, flags);
		if (error != 0)
			return error;

		/* The point size, the last position. */
		if (program->writes_point_size != 0U) {
			error = export_location(lowering, AMDRDNA_GFX10_EXPORT_POSITION + 1U, GFX10_OUTPUT_POINT_SIZE, 0x1U, AMDRDNA_GFX10_FLAG_LAST);
			if (error != 0)
				return error;
		}

		/* Each parameter, the channels the program writes. */
		for (location = 0U; location < AMDRDNA_GFX10_LOCATIONS; location++) {
			if ((program->parameter_outputs & (1U << location)) == 0U)
				continue;
			mask = 0U;
			for (channel = 0U; channel < 4U; channel++) {
				if (lowering->output_written[GFX10_OUTPUT_LOCATIONS + 4U * location + channel] != 0U)
					mask |= 1U << channel;
			}

			/* Exports the location's parameter. */
			error = export_location(lowering,
						AMDRDNA_GFX10_EXPORT_PARAMETER + location,
						GFX10_OUTPUT_LOCATIONS + 4U * location,
						mask,
						0U);
			if (error != 0)
				return error;
		}

		/* Succeeded: every parameter is exported. */
		return 0;
	}

	/* Only a pixel program exports otherwise. */
	if (lowering->ir->stage != DRV_GPU_STAGE_FRAGMENT)
		return 0;

	/* The lanes exported are those alive. */
	if (program->discards != 0U || lowering->whole_quads != 0) {
		emit_scalar(lowering,
			    AMDRDNA_GFX10_KIND_SCALAR_ONE,
			    AMDRDNA_GFX10_S_MOV_B32,
			    AMDRDNA_GFX10_EXEC_LO,
			    lowering->live_sgpr,
			    0U);
	}

	/*
	 * The depth, the stencil reference and the sample mask first, in x,
	 * y and z of the depth target (32-bit channels, the stencil reference
	 * an integer), the last export when no colour follows.
	 *
	 * Procedure referenced: Mesa 25.0.7
	 * src/amd/compiler/aco_instruction_selection.cpp (export_fs_mrtz: the
	 * depth in x, the stencil in y and the sample mask in z of the 32-bit
	 * formats, before the colours, lines 10608-10675).
	 */
	mask = 0U;
	if (program->writes_depth != 0U)
		mask |= 1U << GFX10_OUTPUT_DEPTH;
	if (program->writes_stencil != 0U)
		mask |= 1U << GFX10_OUTPUT_STENCIL;
	if (program->writes_sample_mask != 0U)
		mask |= 1U << GFX10_OUTPUT_SAMPLE_MASK;
	if (mask != 0U) {
		flags = 0U;
		if (program->color_outputs == 0U)
			flags = AMDRDNA_GFX10_FLAG_LAST | AMDRDNA_GFX10_FLAG_VALID_MASK;
		error = export_location(lowering, AMDRDNA_GFX10_EXPORT_DEPTH, GFX10_OUTPUT_DEPTH, mask, flags);
		if (error != 0)
			return error;
		if (program->color_outputs == 0U)
			return 0;
	}

	/* A program that writes no colour, depth, stencil reference or sample mask exports nothing. */
	if (program->color_outputs == 0U) {
		emit_export(lowering, AMDRDNA_GFX10_EXPORT_NOTHING, nothing, 0U, AMDRDNA_GFX10_FLAG_LAST | AMDRDNA_GFX10_FLAG_VALID_MASK);
		return 0;
	}

	/* The last colour written. */
	last = 0U;
	for (location = 0U; location < GFX10_COLOR_TARGETS; location++) {
		if ((program->color_outputs & (1U << location)) != 0U)
			last = location;
	}

	/*
	 * Each colour, the last marked last and valid, to the colour exports
	 * in order without holes: the n-th location written goes to export n
	 * (the export format register has no holes; the colour unit gives the
	 * n-th export to the n-th target its export mask enables).
	 *
	 * Procedure referenced: Mesa 25.0.7
	 * src/amd/common/nir/ac_nir_lower_ps_late.c (get_ps_color_export_target:
	 * V_008DFC_SQ_EXP_MRT plus the compacted MRT index) and
	 * src/amd/vulkan/radv_cmd_buffer.c (radv_compact_spi_shader_col_format:
	 * the holes removed from SPI_SHADER_COL_FORMAT; CB_SHADER_MASK by target).
	 */
	index = 0U;
	for (location = 0U; location < GFX10_COLOR_TARGETS; location++) {
		if ((program->color_outputs & (1U << location)) == 0U)
			continue;
		flags = 0U;
		if (location == last)
			flags = AMDRDNA_GFX10_FLAG_LAST | AMDRDNA_GFX10_FLAG_VALID_MASK;
		kind = amdrdna_gfx10_colour_export(program, location);
		if (kind == AMDRDNA_GFX10_COLOUR_HALVES) {
			error = export_colour(lowering, AMDRDNA_GFX10_EXPORT_COLOR + index, GFX10_OUTPUT_LOCATIONS + 4U * location, flags);
		} else {
			error = export_wide_colour(lowering, AMDRDNA_GFX10_EXPORT_COLOR + index, GFX10_OUTPUT_LOCATIONS + 4U * location, kind, flags);
		}
		if (error != 0)
			return error;
		index++;
	}

	/* Succeeded: every result is exported. */
	return 0;
}

/* Exports the channels mask enables of the output channels from first_channel to a target. */
static int
export_location(
	struct gfx10_lowering *lowering,
	uint32_t target,
	uint32_t first_channel,
	uint32_t mask,
	uint32_t flags)
{
	unsigned sources[4];
	uint32_t channel;
	int error;

	/* The VGPR each enabled channel is read from. */
	for (channel = 0U; channel < 4U; channel++) {
		sources[channel] = 0U;
		if ((mask & (1U << channel)) == 0U)
			continue;
		error = export_source(lowering, first_channel + channel, &sources[channel]);
		if (error != 0)
			return error;
	}

	/* The export. */
	emit_export(lowering, target, sources, mask, flags);

	/* Succeeded: the channels are exported. */
	return 0;
}

/*
 * Exports a colour from the output channels from first_channel on as four
 * 16-bit floats: red and green packed into one VGPR, blue and alpha into
 * another (each rounded toward zero), in a compressed export.
 *
 * Procedure referenced: Mesa 25.0.7
 * src/amd/compiler/instruction_selection/aco_select_ps_epilog.cpp
 * (export_fs_mrt_color, the FP16_ABGR case of GFX10: v_cvt_pkrtz_f16_f32 of
 * channels 0 and 1, and 2 and 3, all four channels enabled, compressed).
 */
static int
export_colour(
	struct gfx10_lowering *lowering,
	uint32_t target,
	uint32_t first_channel,
	uint32_t flags)
{
	unsigned sources[4];
	unsigned packed[4];
	uint32_t channel;
	uint32_t reg;
	int error;

	/* The VGPR each channel is read from. */
	for (channel = 0U; channel < 4U; channel++) {
		error = export_source(lowering, first_channel + channel, &sources[channel]);
		if (error != 0)
			return error;
	}

	/* Two VGPRs, each with two channels packed. */
	packed[2] = 0U;
	packed[3] = 0U;
	for (channel = 0U; channel < 2U; channel++) {
		error = take_vgpr(lowering, &reg);
		if (error != 0)
			return error;
		packed[channel] = AMDRDNA_GFX10_V(reg);
		emit_vector(lowering,
			    AMDRDNA_GFX10_KIND_VECTOR_TWO,
			    AMDRDNA_GFX10_V_CVT_PKRTZ_F16_F32,
			    packed[channel],
			    sources[2U * channel],
			    sources[2U * channel + 1U],
			    0U);
	}

	/* The export of the four channels. */
	emit_export(lowering, target, packed, 0xfU, flags | AMDRDNA_GFX10_FLAG_COMPRESSED);

	/* Succeeded: the colour is exported. */
	return 0;
}

/*
 * Exports a colour from the output channels from first_channel on as the
 * target's format asks (kind, AMDRDNA_GFX10_COLOUR_*, not the halves):
 * the 32-bit channels it names as they are (red and alpha in the first
 * two channels of GFX10's 32_AR), or the four channels packed by twos into
 * 16-bit integers, saturated, after clamping each to an 8-bit integer's
 * range for an 8-bit target, in a compressed export.
 *
 * Procedure referenced: Mesa 25.0.7
 * src/amd/compiler/aco_instruction_selection.cpp (export_fs_mrt_color:
 * 32_R channel x, 32_GR x and y, 32_AR on GFX10 x and y with alpha in y,
 * 32_ABGR all four; UINT16_ABGR and SINT16_ABGR v_cvt_pk_u16_u32 and
 * v_cvt_pk_i16_i32 of channels 0 and 1, and 2 and 3, compressed, the 8-bit
 * integers first v_min_u32 255, or v_min_i32 127 and v_max_i32 -128).
 */
static int
export_wide_colour(
	struct gfx10_lowering *lowering,
	uint32_t target,
	uint32_t first_channel,
	uint32_t kind,
	uint32_t flags)
{
	unsigned sources[4];
	unsigned packed[4];
	uint32_t channel;
	uint32_t reg;
	uint32_t mask;
	unsigned opcode;
	int error;

	/* The VGPR each channel is read from. */
	for (channel = 0U; channel < 4U; channel++) {
		error = export_source(lowering, first_channel + channel, &sources[channel]);
		if (error != 0)
			return error;
	}

	/* The 32-bit channels as they are: red, red and green, red and alpha, or all four. */
	if (kind == AMDRDNA_GFX10_COLOUR_R || kind == AMDRDNA_GFX10_COLOUR_GR || kind == AMDRDNA_GFX10_COLOUR_AR ||
	    kind == AMDRDNA_GFX10_COLOUR_ABGR) {
		mask = 0xfU;
		if (kind == AMDRDNA_GFX10_COLOUR_R)
			mask = 0x1U;
		if (kind == AMDRDNA_GFX10_COLOUR_GR)
			mask = 0x3U;
		if (kind == AMDRDNA_GFX10_COLOUR_AR) {
			sources[1] = sources[3];
			mask = 0x3U;
		}
		emit_export(lowering, target, sources, mask, flags);

		/* Succeeded: the colour is exported. */
		return 0;
	}

	/* An 8-bit integer target's channels clamped to its range first. */
	if (kind == AMDRDNA_GFX10_COLOUR_UINT8 || kind == AMDRDNA_GFX10_COLOUR_SINT8) {
		for (channel = 0U; channel < 4U; channel++) {
			error = take_vgpr(lowering, &reg);
			if (error != 0)
				return error;
			if (kind == AMDRDNA_GFX10_COLOUR_UINT8) {
				emit_vector_literal(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_MIN_U32, AMDRDNA_GFX10_V(reg),
						    AMDRDNA_GFX10_LITERAL, sources[channel], GFX10_UINT8_MAX);
			} else {
				emit_vector_literal(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_MIN_I32, AMDRDNA_GFX10_V(reg),
						    AMDRDNA_GFX10_LITERAL, sources[channel], GFX10_SINT8_MAX);
				emit_vector_literal(lowering, AMDRDNA_GFX10_KIND_VECTOR_TWO, AMDRDNA_GFX10_V_MAX_I32, AMDRDNA_GFX10_V(reg),
						    AMDRDNA_GFX10_LITERAL, AMDRDNA_GFX10_V(reg), GFX10_SINT8_MIN);
			}
			sources[channel] = AMDRDNA_GFX10_V(reg);
		}
	}

	/* Two VGPRs, each with two channels packed into 16-bit integers. */
	opcode = AMDRDNA_GFX10_V_CVT_PK_U16_U32;
	if (kind == AMDRDNA_GFX10_COLOUR_SINT16 || kind == AMDRDNA_GFX10_COLOUR_SINT8)
		opcode = AMDRDNA_GFX10_V_CVT_PK_I16_I32;
	packed[2] = 0U;
	packed[3] = 0U;
	for (channel = 0U; channel < 2U; channel++) {
		error = take_vgpr(lowering, &reg);
		if (error != 0)
			return error;
		packed[channel] = AMDRDNA_GFX10_V(reg);
		emit_vector(lowering, AMDRDNA_GFX10_KIND_VECTOR_WIDE, opcode, packed[channel], sources[2U * channel], sources[2U * channel + 1U], 0U);
	}

	/* The export of the four channels. */
	emit_export(lowering, target, packed, 0xfU, flags | AMDRDNA_GFX10_FLAG_COMPRESSED);

	/* Succeeded: the colour is exported. */
	return 0;
}

/*
 * Finds the VGPR an output channel is exported from: its value's own,
 * the VGPR its stores copied into, or (a value without a VGPR, a channel
 * never stored) a VGPR taken now, the value copied into it.
 */
static int
export_source(
	struct gfx10_lowering *lowering,
	uint32_t channel,
	unsigned *code)
{
	unsigned source;
	uint32_t reg;
	int error;

	/* The value where it lives, when that is a VGPR. */
	source = GFX10_NO_OPERAND;
	if (lowering->output_value[channel] != GFX10_NOT_DEFINED) {
		source = value_operand(lowering, lowering->output_value[channel]);
		if (source >= AMDRDNA_GFX10_VGPR_FIRST) {
			*code = source;
			return 0;
		}
	}

	/* The channel's own VGPR, taken now when no store took it. */
	if (lowering->output_reg[channel] == GFX10_NO_REG) {
		error = take_vgpr(lowering, &reg);
		if (error != 0)
			return error;
		lowering->output_reg[channel] = (uint16_t)reg;
	}

	/* A value without a VGPR is copied into it. */
	if (source != GFX10_NO_OPERAND) {
		emit_vector(lowering,
			    AMDRDNA_GFX10_KIND_VECTOR_ONE,
			    AMDRDNA_GFX10_V_MOV_B32,
			    AMDRDNA_GFX10_V(lowering->output_reg[channel]),
			    source,
			    0U,
			    0U);
	}

	/* Succeeded: the channel's VGPR. */
	*code = AMDRDNA_GFX10_V(lowering->output_reg[channel]);
	return 0;
}

/* Appends an export of the channels mask enables, each from its VGPR in sources[]. */
static void
emit_export(
	struct gfx10_lowering *lowering,
	uint32_t target,
	const unsigned *sources,
	uint32_t mask,
	uint32_t flags)
{
	struct amdrdna_gfx10_inst inst;
	uint32_t channel;

	/* Builds the export; a disabled channel names no register. */
	kern_memset(&inst, 0, sizeof(inst));
	inst.kind = AMDRDNA_GFX10_KIND_EXPORT;
	inst.target = (uint8_t)target;
	inst.mask = (uint8_t)mask;
	inst.flags = (uint8_t)flags;
	for (channel = 0U; channel < 4U; channel++) {
		if ((mask & (1U << channel)) != 0U)
			inst.src[channel] = (uint16_t)sources[channel];
	}

	/* Places it. */
	emit(lowering, &inst);
}

/*
 * Ends the program's list, or the list of the part of it lowered, for
 * finish_code() (a culling part and a deferred part go to it together).
 */
static int
finish(
	struct gfx10_lowering *lowering)
{
	struct amdrdna_gfx10_program *program;
	struct amdrdna_gfx10_inst inst;
	unsigned nothing[4];
	unsigned sources[4];
	unsigned wave_count;
	uint32_t skip_label;
	int error;

	/* Names the program being finished. */
	program = lowering->program;

	/*
	 * A graphics program exports its results; a culling part's vertices
	 * leave their places for its primitives instead.
	 */
	if (program->primitive_culling == 0U) {
		error = finish_exports(lowering);
	} else {
		error = place_for_culling(lowering);
	}
	if (error != 0)
		return error;

	/*
	 * A primitive shader's wave without vertices comes here; a culling
	 * part's primitives are tested and compacted, and the deferred part
	 * follows.
	 */
	if (program->primitive_shader != 0U)
		emit_label(lowering, lowering->vertices_end_label);
	if (program->primitive_culling != 0U) {
		error = cull_and_compact(lowering);
		if (error != 0)
			return error;
		if (lowering->out_of_memory != 0)
			return ENOMEM;
		return 0;
	}

	/*
	 * A deferred part's primitive lanes export the word the culling part
	 * packed in v0, after the vertices' exports (none in a wave without
	 * primitives: an export with no lane running is not skipped).
	 */
	if (lowering->part == GFX10_PART_DEFERRED) {
		wave_count = next_scalar_temp(lowering);
		emit_field(lowering, wave_count, AMDRDNA_GFX10_S(AMDRDNA_GFX10_NGG_WAVE_INFO), AMDRDNA_GFX10_NGG_WAVE_PRIMITIVES);
		emit_lanes_below(lowering, wave_count);
		skip_label = lowering->next_label;
		lowering->next_label++;
		emit_branch(lowering, AMDRDNA_GFX10_S_CBRANCH_EXECZ, skip_label);
		sources[0] = AMDRDNA_GFX10_V(AMDRDNA_GFX10_NGG_PRIMITIVE_VGPR);
		sources[1] = 0U;
		sources[2] = 0U;
		sources[3] = 0U;
		emit_export(lowering, AMDRDNA_GFX10_EXPORT_PRIMITIVE, sources, 0x1U, AMDRDNA_GFX10_FLAG_LAST);
		emit_label(lowering, skip_label);
	}

	/* The program ends. */
	kern_memset(&inst, 0, sizeof(inst));
	inst.kind = AMDRDNA_GFX10_KIND_CONTROL;
	inst.opcode = AMDRDNA_GFX10_S_ENDPGM;
	emit(lowering, &inst);

	/*
	 * A pixel program that discards has an early end for a wave whose
	 * lanes are all discarded: it exports nothing, marked last and valid,
	 * and ends.
	 *
	 * Procedure referenced: Mesa 25.0.7
	 * src/amd/compiler/aco_lower_to_hw_instr.cpp (the discard early exit:
	 * a null export done vm with exec zero, then s_endpgm).
	 */
	if (program->discards != 0U) {
		emit_label(lowering, lowering->early_end_label);
		emit_scalar(lowering,
			    AMDRDNA_GFX10_KIND_SCALAR_ONE,
			    AMDRDNA_GFX10_S_MOV_B32,
			    AMDRDNA_GFX10_EXEC_LO,
			    AMDRDNA_GFX10_ZERO,
			    0U);
		nothing[0] = 0U;
		nothing[1] = 0U;
		nothing[2] = 0U;
		nothing[3] = 0U;
		emit_export(lowering, AMDRDNA_GFX10_EXPORT_NOTHING, nothing, 0U, AMDRDNA_GFX10_FLAG_LAST | AMDRDNA_GFX10_FLAG_VALID_MASK);
		emit(lowering, &inst);
	}

	/* A list that could not grow fails the compile. */
	if (lowering->out_of_memory != 0)
		return ENOMEM;

	/* Succeeded: the list is complete. */
	return 0;
}

/* Places the waits of a lowered program and writes its code and its register counts. */
static int
finish_code(
	struct gfx10_lowering *lowering)
{
	struct amdrdna_gfx10_program *program;
	struct amdrdna_gfx10_inst *waited;
	uint32_t waited_count;
	uint32_t failed;
	int error;

	/* Names the program being written. */
	program = lowering->program;

	/* The waits and the hazard breaks. */
	error = amdrdna_gfx10_place_waits(lowering->list, lowering->count, &waited, &waited_count);
	if (error != 0)
		return error;

	/* The code. */
	failed = 0U;
	error = amdrdna_gfx10_assemble(&waited, &waited_count, &program->code, &program->code_words, &failed);
	kern_free(waited);
	if (error == EINVAL) {
		error = refuse(lowering, EINVAL, "an instruction could not be encoded");
		return error;
	}

	/* Propagates any other failure of the assembly. */
	if (error != 0)
		return error;

	/* The registers the program uses. */
	program->vgpr_count = lowering->vgpr_highest;
	program->sgpr_count = lowering->sgpr_highest;

	/* Succeeded: the program is complete. */
	return 0;
}

/* Narrows the lane mask to the lanes whose Boolean is true, saving the old mask. */
static void
guard_begin(
	struct gfx10_lowering *lowering,
	uint32_t predicate)
{
	/* The predicate into VCC_LO, then the lanes ANDed with it and the old mask saved. */
	condition_from_boolean(lowering, predicate);
	emit_scalar(lowering,
		    AMDRDNA_GFX10_KIND_SCALAR_ONE,
		    AMDRDNA_GFX10_S_AND_SAVEEXEC_B32,
		    lowering->guard_save,
		    AMDRDNA_GFX10_VCC_LO,
		    0U);
	lowering->guard_condition = predicate;
}

/*
 * Narrows the lanes of a memory write to those whose predicate holds
 * (GFX10_NOT_DEFINED: every lane run) and, in a pixel program that runs
 * whole quads, to the lanes alive: the helper lanes write nothing.
 * Returns nonzero when the lanes were narrowed, for guard_end().
 */
static int
guard_writes(
	struct gfx10_lowering *lowering,
	uint32_t predicate)
{
	unsigned narrowed;

	/* Without helper lanes only a predicate narrows. */
	if (lowering->whole_quads == 0) {
		if (predicate == GFX10_NOT_DEFINED)
			return 0;
		guard_begin(lowering, predicate);
		return 1;
	}

	/* The alive lanes alone, VCC_LO left as it is. */
	if (predicate == GFX10_NOT_DEFINED) {
		lowering->guard_condition = lowering->vcc_value;
		emit_scalar(lowering,
			    AMDRDNA_GFX10_KIND_SCALAR_ONE,
			    AMDRDNA_GFX10_S_AND_SAVEEXEC_B32,
			    lowering->guard_save,
			    lowering->live_sgpr,
			    0U);
		return 1;
	}

	/* The predicate's lanes that are alive, through a scalar temporary so that VCC_LO keeps the predicate. */
	condition_from_boolean(lowering, predicate);
	narrowed = next_scalar_temp(lowering);
	emit_scalar(lowering,
		    AMDRDNA_GFX10_KIND_SCALAR_TWO,
		    AMDRDNA_GFX10_S_AND_B32,
		    narrowed,
		    AMDRDNA_GFX10_VCC_LO,
		    lowering->live_sgpr);
	emit_scalar(lowering,
		    AMDRDNA_GFX10_KIND_SCALAR_ONE,
		    AMDRDNA_GFX10_S_AND_SAVEEXEC_B32,
		    lowering->guard_save,
		    narrowed,
		    0U);
	lowering->guard_condition = predicate;

	/* Reports that the writes are guarded, so the caller restores the lanes afterwards. */
	return 1;
}

/* Restores the lane mask a guard saved. */
static void
guard_end(
	struct gfx10_lowering *lowering)
{
	/* The saved mask comes back. */
	emit_scalar(lowering,
		    AMDRDNA_GFX10_KIND_SCALAR_ONE,
		    AMDRDNA_GFX10_S_MOV_B32,
		    AMDRDNA_GFX10_EXEC_LO,
		    lowering->guard_save,
		    0U);

	/* The guarded access wrote no VCC_LO, and the lanes are those the predicate was taken for. */
	lowering->vcc_value = lowering->guard_condition;
}

/* Sets VCC_LO for the lanes where a Boolean value is true. */
static void
condition_from_boolean(
	struct gfx10_lowering *lowering,
	uint32_t value)
{
	unsigned operand;

	/* VCC_LO already holds it. */
	if (lowering->vcc_value == value)
		return;

	/* A Boolean kept only in VCC_LO has no VGPR to take it from. */
	if (lowering->condition_only[value] != 0U) {
		lowering->lost_condition = 1;
		return;
	}

	/* A lane's Boolean is true when it is not zero. */
	operand = value_operand(lowering, value);
	emit_vector(lowering,
		    AMDRDNA_GFX10_KIND_VECTOR_COMPARE,
		    AMDRDNA_GFX10_V_CMP_NE_U32,
		    AMDRDNA_GFX10_VCC_LO,
		    AMDRDNA_GFX10_ZERO,
		    operand,
		    0U);
	lowering->vcc_value = value;
}

/* Makes a Boolean value (all ones or zero) of VCC_LO. */
static void
boolean_from_condition(
	struct gfx10_lowering *lowering,
	uint32_t dst)
{
	unsigned all_ones;

	/* Each lane takes -1 where VCC_LO is set and 0 elsewhere. */
	all_ones = constant_operand(GFX10_TRUE);
	emit_vector(lowering,
		    AMDRDNA_GFX10_KIND_VECTOR_TWO,
		    AMDRDNA_GFX10_V_CNDMASK_B32,
		    AMDRDNA_GFX10_V(dst),
		    AMDRDNA_GFX10_ZERO,
		    all_ones,
		    AMDRDNA_GFX10_VCC_LO);
}

/* Appends an instruction, growing the list when it is full. */
static void
emit(
	struct gfx10_lowering *lowering,
	const struct amdrdna_gfx10_inst *inst)
{
	struct amdrdna_gfx10_inst *grown;
	struct amdrdna_gfx10_inst legal;
	uint32_t capacity;

	/* Once the list could not grow, nothing more is added. */
	if (lowering->out_of_memory != 0)
		return;

	/* A vector ALU instruction reads at most two distinct scalar values; the others are copied to VGPRs. */
	if (inst->kind == AMDRDNA_GFX10_KIND_VECTOR_ONE ||
	    inst->kind == AMDRDNA_GFX10_KIND_VECTOR_TWO ||
	    inst->kind == AMDRDNA_GFX10_KIND_VECTOR_COMPARE ||
	    inst->kind == AMDRDNA_GFX10_KIND_VECTOR_WIDE) {
		legal = *inst;
		legalize_scalars(lowering, &legal);
		inst = &legal;
		if (lowering->out_of_memory != 0)
			return;
	}

	/* A full list is copied into a larger one. */
	if (lowering->count == lowering->capacity) {
		capacity = lowering->capacity + GFX10_LIST_GROWTH;
		grown = kern_calloc(capacity, sizeof(*grown));
		if (grown == NULL) {
			lowering->out_of_memory = 1;
			return;
		}

		/* The entries so far move over. */
		if (lowering->list != NULL)
			kern_memcpy(grown, lowering->list, lowering->count * sizeof(*grown));
		kern_free(lowering->list);
		lowering->list = grown;
		lowering->capacity = capacity;
	}

	/* Appends the instruction. */
	lowering->list[lowering->count] = *inst;
	lowering->count++;

	/* A write of VCC_LO or of the lane mask, or a label, leaves VCC_LO holding no known Boolean. */
	forget_condition(lowering, inst);
}

/* Appends a vector ALU instruction. */
static void
emit_vector(
	struct gfx10_lowering *lowering,
	enum amdrdna_gfx10_kind kind,
	unsigned opcode,
	unsigned dst,
	unsigned src0,
	unsigned src1,
	unsigned src2)
{
	struct amdrdna_gfx10_inst inst;

	/* Builds the instruction. */
	kern_memset(&inst, 0, sizeof(inst));
	inst.kind = kind;
	inst.opcode = (uint16_t)opcode;
	inst.dst = (uint16_t)dst;
	inst.src[0] = (uint16_t)src0;
	inst.src[1] = (uint16_t)src1;
	inst.src[2] = (uint16_t)src2;

	/* Places it. */
	emit(lowering, &inst);
}

/* Appends a vector ALU instruction whose sources may name a literal. */
static void
emit_vector_literal(
	struct gfx10_lowering *lowering,
	enum amdrdna_gfx10_kind kind,
	unsigned opcode,
	unsigned dst,
	unsigned src0,
	unsigned src1,
	uint32_t literal)
{
	struct amdrdna_gfx10_inst inst;

	/* Builds the instruction with its literal. */
	kern_memset(&inst, 0, sizeof(inst));
	inst.kind = kind;
	inst.opcode = (uint16_t)opcode;
	inst.dst = (uint16_t)dst;
	inst.src[0] = (uint16_t)src0;
	inst.src[1] = (uint16_t)src1;
	inst.literal = literal;

	/* Places it. */
	emit(lowering, &inst);
}

/* Appends dst = factor * constant + addend, the 24-bit unsigned multiply-add. */
static void
emit_multiply_add(
	struct gfx10_lowering *lowering,
	unsigned dst,
	unsigned factor,
	uint32_t constant,
	unsigned addend)
{
	struct amdrdna_gfx10_inst inst;

	/* Builds the instruction, the constant inline or as the literal. */
	kern_memset(&inst, 0, sizeof(inst));
	inst.kind = AMDRDNA_GFX10_KIND_VECTOR_WIDE;
	inst.opcode = AMDRDNA_GFX10_V_MAD_U32_U24;
	inst.dst = (uint16_t)dst;
	inst.src[0] = (uint16_t)factor;
	inst.src[1] = (uint16_t)constant_operand(constant);
	inst.src[2] = (uint16_t)addend;
	inst.literal = constant;

	/* Places it. */
	emit(lowering, &inst);
}

/* Appends a scalar ALU instruction. */
static void
emit_scalar(
	struct gfx10_lowering *lowering,
	enum amdrdna_gfx10_kind kind,
	unsigned opcode,
	unsigned dst,
	unsigned src0,
	unsigned src1)
{
	struct amdrdna_gfx10_inst inst;

	/* Builds the instruction. */
	kern_memset(&inst, 0, sizeof(inst));
	inst.kind = kind;
	inst.opcode = (uint16_t)opcode;
	inst.dst = (uint16_t)dst;
	inst.src[0] = (uint16_t)src0;
	inst.src[1] = (uint16_t)src1;

	/* Places it. */
	emit(lowering, &inst);
}

/* Appends a scalar load of count words at a byte offset from a base (an address pair or a buffer descriptor). */
static void
emit_scalar_load(
	struct gfx10_lowering *lowering,
	unsigned opcode,
	unsigned dst,
	unsigned base,
	uint32_t offset,
	unsigned count)
{
	struct amdrdna_gfx10_inst inst;

	/* Builds the load. */
	kern_memset(&inst, 0, sizeof(inst));
	inst.kind = AMDRDNA_GFX10_KIND_SCALAR_LOAD;
	inst.opcode = (uint16_t)opcode;
	inst.dst = (uint16_t)dst;
	inst.src[0] = (uint16_t)base;
	inst.immediate = offset;
	inst.count = (uint8_t)count;

	/* Places it. */
	emit(lowering, &inst);
}

/* Appends a label. */
static void
emit_label(
	struct gfx10_lowering *lowering,
	uint32_t label)
{
	struct amdrdna_gfx10_inst inst;

	/* A label is a record of its own kind with its number. */
	kern_memset(&inst, 0, sizeof(inst));
	inst.kind = AMDRDNA_GFX10_KIND_LABEL;
	inst.label = label;

	/* Places it. */
	emit(lowering, &inst);
}

/* Appends a branch to a label. */
static void
emit_branch(
	struct gfx10_lowering *lowering,
	unsigned opcode,
	uint32_t label)
{
	struct amdrdna_gfx10_inst inst;

	/* The distance is resolved when the code is written. */
	kern_memset(&inst, 0, sizeof(inst));
	inst.kind = AMDRDNA_GFX10_KIND_BRANCH;
	inst.opcode = (uint16_t)opcode;
	inst.label = label;

	/* Places it. */
	emit(lowering, &inst);
}

/*
 * Reports the operand code of a 32-bit constant: an inline integer
 * (0 .. 64 and -16 .. -1), an inline float, or the literal (the caller
 * then puts the bits in the instruction's literal).
 */
static unsigned
constant_operand(
	uint32_t bits)
{
	/* The small non-negative integers. */
	if (bits <= 64U)
		return AMDRDNA_GFX10_INT_FIRST + bits;

	/* The small negative integers: -1 is 193, -16 is 208. */
	if (bits >= 0xfffffff0U)
		return AMDRDNA_GFX10_INT_LAST + (0U - bits);

	/* The inline floats. */
	switch (bits) {
	case AMDRDNA_GFX10_BITS_HALF:
		return AMDRDNA_GFX10_HALF;
	case AMDRDNA_GFX10_BITS_HALF | GFX10_SIGN_BIT:
		return AMDRDNA_GFX10_MINUS_HALF;
	case AMDRDNA_GFX10_BITS_ONE:
		return AMDRDNA_GFX10_ONE;
	case AMDRDNA_GFX10_BITS_ONE | GFX10_SIGN_BIT:
		return AMDRDNA_GFX10_MINUS_ONE;
	case AMDRDNA_GFX10_BITS_TWO:
		return AMDRDNA_GFX10_TWO;
	case AMDRDNA_GFX10_BITS_TWO | GFX10_SIGN_BIT:
		return AMDRDNA_GFX10_MINUS_TWO;
	case AMDRDNA_GFX10_BITS_FOUR:
		return AMDRDNA_GFX10_FOUR;
	case AMDRDNA_GFX10_BITS_FOUR | GFX10_SIGN_BIT:
		return AMDRDNA_GFX10_MINUS_FOUR;
	case AMDRDNA_GFX10_BITS_INV_TWO_PI:
		return AMDRDNA_GFX10_INV_TWO_PI;
	default:
		return AMDRDNA_GFX10_LITERAL;
	}
}

/* Reports the operand code of a value's VGPR. */
static unsigned
value_operand(
	const struct gfx10_lowering *lowering,
	uint32_t value)
{
	/* A constant's or a uniform word's own operand. */
	if (lowering->operand_code[value] != GFX10_NO_OPERAND)
		return lowering->operand_code[value];

	/* The value's VGPR as an operand. */
	return AMDRDNA_GFX10_V(lowering->vgpr[value]);
}

/*
 * Reports a VGPR holding a value, for an operand that must be a VGPR (a
 * memory address or data): the value's own, or a scratch copy of its
 * constant or SGPR.
 */
static unsigned
vgpr_operand(
	struct gfx10_lowering *lowering,
	uint32_t value)
{
	unsigned code;

	/* A value in a VGPR is its own operand. */
	code = value_operand(lowering, value);
	if (code >= AMDRDNA_GFX10_VGPR_FIRST)
		return code;

	/* Otherwise a scratch copy. */
	code = scratch_copy(lowering, code);

	/* Reports the operand of the scratch copy. */
	return code;
}

/*
 * Copies a scalar operand (an SGPR or an inline constant) into a scratch
 * VGPR the IR instruction keeps until it is lowered; returns the VGPR's
 * operand code.
 */
static unsigned
scratch_copy(
	struct gfx10_lowering *lowering,
	unsigned code)
{
	struct amdrdna_gfx10_inst copy;
	uint32_t reg;
	int error;

	/* A scratch VGPR; with none free the compile fails as out of registers. */
	if (lowering->scratch_count == GFX10_SCRATCH_VGPRS) {
		lowering->out_of_memory = 1;
		return code;
	}

	/* Takes a register for the copy. */
	error = take_vgpr(lowering, &reg);
	if (error != 0) {
		lowering->out_of_memory = 1;
		return code;
	}

	/* Records the register so it is released with the instruction. */
	lowering->scratch[lowering->scratch_count] = reg;
	lowering->scratch_count++;

	/* The copy, which reads one scalar value and so needs no legalizing. */
	kern_memset(&copy, 0, sizeof(copy));
	copy.kind = AMDRDNA_GFX10_KIND_VECTOR_ONE;
	copy.opcode = AMDRDNA_GFX10_V_MOV_B32;
	copy.dst = (uint16_t)AMDRDNA_GFX10_V(reg);
	copy.src[0] = (uint16_t)code;
	emit(lowering, &copy);

	/* The VGPR as the operand. */
	return AMDRDNA_GFX10_V(reg);
}

/* Gives back the scratch VGPRs of the IR instruction just lowered. */
static void
give_scratch(
	struct gfx10_lowering *lowering)
{
	/* Every scratch VGPR taken. */
	while (lowering->scratch_count != 0U) {
		lowering->scratch_count--;
		give_vgpr(lowering, lowering->scratch[lowering->scratch_count]);
	}
}

/*
 * Keeps a vector ALU instruction within the constant bus: at most two
 * distinct scalar values (SGPRs, VCC or the literal) among the sources it
 * reads; each further SGPR source is copied into a scratch VGPR.
 */
static void
legalize_scalars(
	struct gfx10_lowering *lowering,
	struct amdrdna_gfx10_inst *inst)
{
	unsigned order[3];
	unsigned seen[4];
	unsigned seen_count;
	unsigned reads;
	unsigned step;
	unsigned index;
	unsigned other;
	unsigned code;
	int known;

	/* Starts the count of distinct scalar sources. */
	reads = vector_reads(inst);
	seen_count = 0U;

	/* The sources in order, a selection's condition first so that it is never the one copied. */
	order[0] = 0U;
	order[1] = 1U;
	order[2] = 2U;
	if (reads == 3U && inst->kind == AMDRDNA_GFX10_KIND_VECTOR_TWO) {
		order[0] = 2U;
		order[1] = 0U;
		order[2] = 1U;
	}

	/* Each source read: a scalar register or the literal not seen yet counts once. */
	for (step = 0U; step < reads; step++) {
		index = order[step];
		code = inst->src[index];
		if (code >= AMDRDNA_GFX10_INT_FIRST && code != AMDRDNA_GFX10_LITERAL)
			continue;

		/* The same scalar read twice counts once. */
		known = 0;
		for (other = 0U; other < seen_count; other++) {
			if (seen[other] == code)
				known = 1;
		}

		/* A repeated read costs nothing more. */
		if (known != 0)
			continue;

		/* Within the limit it stays; past it an SGPR is copied. */
		if (seen_count < GFX10_CONSTANT_BUS_LIMIT) {
			seen[seen_count] = code;
			seen_count++;
			continue;
		}

		/* Reads a VGPR copy of the operand instead. */
		inst->src[index] = (uint16_t)scratch_copy(lowering, code);
	}
}

/* Reports how many sources a vector ALU instruction reads. */
static unsigned
vector_reads(
	const struct amdrdna_gfx10_inst *inst)
{
	/* Picks the count by the kind and the operation. */
	switch (inst->kind) {
	case AMDRDNA_GFX10_KIND_VECTOR_ONE:
		return 1U;
	case AMDRDNA_GFX10_KIND_VECTOR_TWO:
		/* The selection reads its condition third. */
		if (inst->opcode == AMDRDNA_GFX10_V_CNDMASK_B32)
			return 3U;
		return 2U;
	case AMDRDNA_GFX10_KIND_VECTOR_COMPARE:
		return 2U;
	default:
		/* The multiply-adds and the cube operations read three. */
		if (inst->opcode == AMDRDNA_GFX10_V_FMA_F32 || inst->opcode == AMDRDNA_GFX10_V_MAD_U32_U24)
			return 3U;
		if (inst->opcode >= AMDRDNA_GFX10_V_CUBEID_F32 && inst->opcode <= AMDRDNA_GFX10_V_CUBEMA_F32)
			return 3U;
		return 2U;
	}
}

/*
 * Gives each uniform word the program reads at a constant offset an SGPR,
 * loaded once at the start with the 16 bytes around it, while the SGPRs
 * left below VCC hold them.  A word a MOVE writes, or one past the SGPRs,
 * is read where it is used instead.
 */
static void
plan_uniforms(
	struct gfx10_lowering *lowering,
	uint32_t *sgpr)
{
	const struct drv_gpu_shader_ir_inst *instruction;
	uint32_t index;
	uint32_t chunk;
	uint32_t slot;
	uint32_t offset;
	uint32_t found;
	uint32_t base;

	/* The chunks start at a multiple of four SGPRs, after the cache (and its spare chunk) for the words that get none. */
	base = (*sgpr + 3U) & ~3U;
	lowering->cache_sgpr = base;
	base += 4U * (GFX10_CHUNK_CACHE + 1U);
	*sgpr = base;

	/* Each uniform read at a constant offset. */
	for (index = 0U; index < lowering->ir->instruction_count; index++) {
		instruction = &lowering->ir->instructions[index];
		if (instruction->op != DRV_GPU_IR_LOAD_PUSH && instruction->op != DRV_GPU_IR_LOAD_UBO)
			continue;
		if (lowering->moved[instruction->dst] != 0U)
			continue;
		if (lowering->dead[index] != 0U)
			continue;

		/* The buffer and the chunk. */
		slot = GFX10_NO_SLOT;
		if (instruction->op == DRV_GPU_IR_LOAD_UBO) {
			if (instruction->location >= lowering->ir->uniform_count)
				continue;
			if (lowering->ir->uniforms[instruction->location].kind != DRV_GPU_IR_UNIFORM_BLOCK)
				continue;
			slot = lowering->slot[instruction->location];
		} else if (instruction->immediate + 4U > lowering->ir->push_bytes) {
			continue;
		}

		/* The 16-byte chunk the word lies in starts at this offset. */
		offset = instruction->immediate & ~15U;

		/* The chunk already loaded, or a new one while the SGPRs last. */
		found = GFX10_NO_SLOT;
		for (chunk = 0U; chunk < lowering->chunk_count; chunk++) {
			if (lowering->chunk_slot[chunk] == slot && lowering->chunk_offset[chunk] == offset)
				found = chunk;
		}

		/* A chunk not loaded yet takes the next SGPRs if there are any. */
		if (found == GFX10_NO_SLOT) {
			if (lowering->chunk_count == GFX10_UNIFORM_CHUNKS)
				continue;
			if (base + 4U > AMDRDNA_GFX10_SGPR_COUNT)
				continue;
			found = lowering->chunk_count;
			lowering->chunk_slot[found] = slot;
			lowering->chunk_offset[found] = offset;
			lowering->chunk_sgpr[found] = base;
			lowering->chunk_count++;
			base += 4U;
		}

		/* The word's SGPR in its chunk. */
		lowering->operand_code[instruction->dst] =
			(uint16_t)AMDRDNA_GFX10_S(lowering->chunk_sgpr[found] + (instruction->immediate & 15U) / 4U);
	}

	/* The SGPRs the chunks took. */
	if (lowering->chunk_count != 0U)
		*sgpr = base;
}

/*
 * Finds the output channels exported where their values live: a channel
 * stored once in the program, with a value no MOVE writes that an
 * instruction makes in a VGPR of its own (not a constant, a uniform word
 * or a built-in value, which have other operands).  Such a value lives to
 * the end of the program.
 */
static int
plan_outputs(
	struct gfx10_lowering *lowering)
{
	const struct drv_gpu_shader_ir *ir;
	const struct drv_gpu_shader_ir_inst *instruction;
	uint32_t *definer;
	uint32_t stores[GFX10_OUTPUT_CHANNELS];
	uint32_t stored[GFX10_OUTPUT_CHANNELS];
	uint32_t index;
	uint32_t channel;
	uint32_t value;
	int defines;

	/* Names the module being scanned. */
	ir = lowering->ir;
	if (ir->stage == DRV_GPU_STAGE_COMPUTE)
		return 0;

	/* The operation that first defines each value, among those lowered. */
	definer = kern_calloc(ir->value_count + 1U, sizeof(*definer));
	if (definer == NULL)
		return ENOMEM;
	for (index = 0U; index <= ir->value_count; index++)
		definer[index] = GFX10_NOT_DEFINED;
	for (index = 0U; index < ir->instruction_count; index++) {
		instruction = &ir->instructions[index];
		if (lowering->dead[index] != 0U)
			continue;
		defines = ir_defines(instruction);
		for (channel = 0U; channel < (uint32_t)defines; channel++) {
			if (definer[instruction->dst + channel] == GFX10_NOT_DEFINED)
				definer[instruction->dst + channel] = (uint32_t)instruction->op;
		}
	}

	/* How many times each output channel is stored, and with what. */
	for (channel = 0U; channel < GFX10_OUTPUT_CHANNELS; channel++) {
		stores[channel] = 0U;
		stored[channel] = GFX10_NOT_DEFINED;
	}

	/* Counts the stores of each output channel. */
	for (index = 0U; index < ir->instruction_count; index++) {
		instruction = &ir->instructions[index];
		if (instruction->op != DRV_GPU_IR_STORE_OUTPUT || lowering->dead[index] != 0U)
			continue;
		channel = output_channel(lowering, instruction);
		if (channel == GFX10_OUTPUT_CHANNELS)
			continue;
		stores[channel]++;
		stored[channel] = instruction->src[0];
		lowering->output_written[channel] = 1U;
	}

	/* The primitive number a vertex program exports is written at the start. */
	if (lowering->program->exports_primitive_id != 0U)
		lowering->output_written[GFX10_OUTPUT_LOCATIONS + 4U * AMDRDNA_GFX10_PRIMITIVE_ID_PARAMETER] = 1U;

	/* Each channel stored once with a value made in a VGPR of its own. */
	for (channel = 0U; channel < GFX10_OUTPUT_CHANNELS; channel++) {
		if (stores[channel] != 1U)
			continue;
		value = stored[channel];
		if (lowering->moved[value] != 0U)
			continue;

		/* The operations whose values have operands of their own, or no definition. */
		switch (definer[value]) {
		case GFX10_NOT_DEFINED:
		case DRV_GPU_IR_CONST:
		case DRV_GPU_IR_ICONST:
		case DRV_GPU_IR_BOOL:
		case DRV_GPU_IR_LOAD_PUSH:
		case DRV_GPU_IR_LOAD_UBO:
		case DRV_GPU_IR_LOAD_SYSTEM:
		case DRV_GPU_IR_STORAGE_SIZE:
			continue;
		default:
			break;
		}

		/* The export reads the value, which lives to the end. */
		lowering->output_value[channel] = value;
		lowering->last_use[value] = ir->instruction_count + 1U;
	}

	/* Releases the definer table. */
	kern_free(definer);

	/* Succeeded: the channels exported in place are chosen. */
	return 0;
}

/*
 * Finds the Booleans that never need a VGPR: made by a comparison, read
 * only as the condition of selections, discards, a loop's end or guarded
 * accesses, with nothing between the comparison and the last read that
 * writes VCC_LO or widens the lane mask.  Such a Boolean stays in the
 * VCC_LO its comparison wrote.
 */
static void
plan_conditions(
	struct gfx10_lowering *lowering)
{
	const struct drv_gpu_shader_ir *ir;
	const struct drv_gpu_shader_ir_inst *instruction;
	const struct drv_gpu_shader_ir_inst *reader;
	uint32_t sources[GFX10_MAX_SOURCES];
	uint32_t index;
	uint32_t later;
	uint32_t last;
	uint32_t source;
	uint32_t condition;
	uint32_t value;
	int count;
	int defines;
	int kept;
	int touches;

	/* Names the module being scanned. */
	ir = lowering->ir;

	/* Each comparison. */
	for (index = 0U; index < ir->instruction_count; index++) {
		instruction = &ir->instructions[index];
		defines = defines_condition(lowering, instruction);
		if (defines == 0 || lowering->dead[index] != 0U)
			continue;
		value = instruction->dst;

		/* A Boolean a MOVE writes, or that nothing reads, keeps the usual VGPR. */
		if (lowering->moved[value] != 0U)
			continue;
		last = lowering->last_use[value];
		if (last <= index + 1U)
			continue;

		/* Each instruction up to the last read must leave VCC_LO and widen no lanes. */
		kept = 1;
		for (later = index + 1U; kept != 0 && later < last && later < ir->instruction_count; later++) {
			reader = &ir->instructions[later];
			if (lowering->dead[later] != 0U)
				continue;
			condition = condition_source(reader);

			/* The Boolean read as a value needs its VGPR. */
			count = ir_sources(reader, sources);
			for (source = 0U; count > 0 && source < (uint32_t)count; source++) {
				if (sources[source] != value)
					continue;
				if (condition != GFX10_NOT_DEFINED && source == condition)
					continue;
				kept = 0;
			}

			/* An instruction that never touches VCC_LO or the lanes keeps the Boolean. */
			touches = touches_condition(lowering, reader);
			if (touches == 0)
				continue;

			/* Another condition, or a discard before the last read, ends it. */
			if (condition == GFX10_NOT_DEFINED || reader->src[condition] != value) {
				kept = 0;
			} else if (reader->op == DRV_GPU_IR_KILL && later + 1U != last) {
				kept = 0;
			}
		}

		/* The Boolean lives in VCC_LO alone. */
		if (kept != 0)
			lowering->condition_only[value] = 1U;
	}
}

/* Reports whether an IR instruction makes a Boolean with a comparison into VCC_LO. */
static int
defines_condition(
	const struct gfx10_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction)
{
	/* Picks by the operation. */
	switch (instruction->op) {
	case DRV_GPU_IR_FLT:
	case DRV_GPU_IR_FGE:
	case DRV_GPU_IR_FEQ:
	case DRV_GPU_IR_FNEU:
	case DRV_GPU_IR_ILT:
	case DRV_GPU_IR_IGE:
	case DRV_GPU_IR_ULT:
	case DRV_GPU_IR_UGE:
	case DRV_GPU_IR_IEQ:
	case DRV_GPU_IR_INE:
		return 1;
	case DRV_GPU_IR_LOAD_INPUT:
		/* The face of a pixel is compared too. */
		if (lowering->ir->stage == DRV_GPU_STAGE_FRAGMENT &&
		    instruction->location == DRV_GPU_SHADER_LOCATION_FRONT_FACING)
			return 1;
		return 0;
	default:
		return 0;
	}
}

/*
 * Reports which source of an IR instruction is read as a condition (the
 * index into src[]), or GFX10_NOT_DEFINED when none is.
 */
static uint32_t
condition_source(
	const struct drv_gpu_shader_ir_inst *instruction)
{
	/* Picks by the operation and, for a guarded access, whether it is guarded. */
	switch (instruction->op) {
	case DRV_GPU_IR_SELECT:
	case DRV_GPU_IR_KILL:
	case DRV_GPU_IR_LOOP_END:
		return 0U;
	case DRV_GPU_IR_LOAD_STORAGE:
	case DRV_GPU_IR_LOAD_SHARED:
		if (instruction->component != 0U)
			return 1U;
		return GFX10_NOT_DEFINED;
	case DRV_GPU_IR_STORE_STORAGE:
	case DRV_GPU_IR_STORE_SHARED:
		if (instruction->component != 0U)
			return 2U;
		return GFX10_NOT_DEFINED;
	case DRV_GPU_IR_ATOMIC:
		if (instruction->component != DRV_GPU_IR_ATOMIC_PREDICATED)
			return GFX10_NOT_DEFINED;
		if (instruction->immediate == DRV_GPU_IR_ATOMIC_CMPXCHG)
			return 3U;
		return 2U;
	case DRV_GPU_IR_IMAGE_READ:
		if (instruction->component != 0U)
			return 2U;
		return GFX10_NOT_DEFINED;
	case DRV_GPU_IR_IMAGE_WRITE:
		if (instruction->component != 0U)
			return 3U;
		return GFX10_NOT_DEFINED;
	default:
		return GFX10_NOT_DEFINED;
	}
}

/* Reports whether lowering an IR instruction may write VCC_LO or change the lanes. */
static int
touches_condition(
	const struct gfx10_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction)
{
	uint32_t condition;
	uint32_t message;
	int defines;

	/* A comparison writes VCC_LO. */
	defines = defines_condition(lowering, instruction);
	if (defines != 0)
		return 1;

	/* An instruction that takes a condition sets VCC_LO from it. */
	condition = condition_source(instruction);
	if (condition != GFX10_NOT_DEFINED)
		return 1;

	/* A cube sampled with gradients chooses them by its face in VCC_LO. */
	message = instruction->component & DRV_GPU_IR_TEXTURE_MESSAGE_MASK;
	if (instruction->op == DRV_GPU_IR_TEXTURE &&
	    (instruction->image_shape & DRV_GPU_IR_IMAGE_KIND_MASK) == DRV_GPU_IR_IMAGE_CUBE &&
	    (message == DRV_GPU_IR_TEXTURE_SAMPLE_DERIVS || message == DRV_GPU_IR_TEXTURE_SAMPLE_DERIV_COMPARE))
		return 1;

	/* The loops and the divisions; everything else leaves VCC_LO and the lanes alone. */
	switch (instruction->op) {
	case DRV_GPU_IR_LOOP_BEGIN:
	case DRV_GPU_IR_UDIV:
	case DRV_GPU_IR_UMOD:
	case DRV_GPU_IR_IDIV:
	case DRV_GPU_IR_IREM:
		return 1;
	default:
		return 0;
	}
}

/* Forgets the Boolean VCC_LO held when an instruction writes it or the lane mask, or is a label. */
static void
forget_condition(
	struct gfx10_lowering *lowering,
	const struct amdrdna_gfx10_inst *inst)
{
	struct amdrdna_gfx10_access access;
	uint32_t index;
	uint32_t first;
	uint32_t end;

	/* A label is reached from elsewhere. */
	if (inst->kind == AMDRDNA_GFX10_KIND_LABEL) {
		lowering->vcc_value = GFX10_NOT_DEFINED;
		return;
	}

	/* Each register the instruction writes. */
	amdrdna_gfx10_describe(inst, &access);
	for (index = 0U; index < access.writes_used; index++) {
		first = access.writes[index].first;
		end = first + access.writes[index].count;
		if (first <= AMDRDNA_GFX10_VCC_LO && AMDRDNA_GFX10_VCC_LO < end)
			lowering->vcc_value = GFX10_NOT_DEFINED;
		if (first <= AMDRDNA_GFX10_EXEC_LO && AMDRDNA_GFX10_EXEC_LO < end)
			lowering->vcc_value = GFX10_NOT_DEFINED;
	}
}

/* Reports the output channel a store writes (GFX10_OUTPUT_CHANNELS for one the compiler does not export). */
static uint32_t
output_channel(
	const struct gfx10_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction)
{
	uint32_t location;

	/* Names the location the output is stored to. */
	location = instruction->location;

	/* Four channels to a location. */
	if (instruction->component > 3U)
		return GFX10_OUTPUT_CHANNELS;

	/* A pixel program's depth, stencil reference, sample mask and colours; the second colour is exported as colour 1. */
	if (lowering->ir->stage == DRV_GPU_STAGE_FRAGMENT) {
		if (location == DRV_GPU_IR_LOCATION_FRAG_DEPTH && instruction->component == 0U)
			return GFX10_OUTPUT_DEPTH;
		if (location == DRV_GPU_IR_LOCATION_FRAG_STENCIL && instruction->component == 0U)
			return GFX10_OUTPUT_STENCIL;
		if (location == DRV_GPU_IR_LOCATION_SAMPLE_MASK && instruction->component == 0U)
			return GFX10_OUTPUT_SAMPLE_MASK;
		if (location == DRV_GPU_IR_LOCATION_SECOND_COLOR)
			return GFX10_OUTPUT_LOCATIONS + 4U + instruction->component;
		if (location >= GFX10_COLOR_TARGETS)
			return GFX10_OUTPUT_CHANNELS;
		return GFX10_OUTPUT_LOCATIONS + 4U * location + instruction->component;
	}

	/* Only a vertex program has the others. */
	if (lowering->ir->stage != DRV_GPU_STAGE_VERTEX)
		return GFX10_OUTPUT_CHANNELS;

	/* The position, the point size, or a parameter. */
	if (location == DRV_GPU_IR_LOCATION_POSITION)
		return GFX10_OUTPUT_POSITION + instruction->component;
	if (location == DRV_GPU_IR_LOCATION_POINT_SIZE)
		return GFX10_OUTPUT_POINT_SIZE;
	if (location < AMDRDNA_GFX10_LOCATIONS)
		return GFX10_OUTPUT_LOCATIONS + 4U * location + instruction->component;

	/* Any other location has no channel in this stage. */
	return GFX10_OUTPUT_CHANNELS;
}

/*
 * Reports whether an IR instruction defines a value that has an operand
 * of its own and so needs no instruction, and gives it that operand: an
 * inline constant; a uniform word loaded at the start or into the cache;
 * a vertex program's fetched channel or vertex or instance number; a
 * compute program's workgroup id or local id; a storage buffer's size.
 * A value a MOVE writes always lives in a VGPR of its own.
 */
static int
has_operand(
	struct gfx10_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction)
{
	const struct drv_gpu_shader_ir_uniform *uniform;
	unsigned code;
	int aliased;
	int defines;

	/* A uniform word loaded at the start. */
	if (instruction->op == DRV_GPU_IR_LOAD_PUSH || instruction->op == DRV_GPU_IR_LOAD_UBO) {
		if (lowering->operand_code[instruction->dst] != GFX10_NO_OPERAND)
			return 1;
	}

	/* Counts the values the instruction defines. */
	defines = ir_defines(instruction);

	/* A value a MOVE writes, or an instruction that defines nothing, has no operand of its own. */
	if (defines != 1)
		return 0;
	if (lowering->moved[instruction->dst] != 0U)
		return 0;

	/* Picks the operand by the operation. */
	code = GFX10_NO_OPERAND;
	switch (instruction->op) {
	case DRV_GPU_IR_CONST:
	case DRV_GPU_IR_ICONST:
	case DRV_GPU_IR_BOOL:
		/* An inline constant is its own operand; another one needs its literal moved. */
		code = constant_operand(instruction->immediate);
		if (code == AMDRDNA_GFX10_LITERAL)
			return 0;
		break;
	case DRV_GPU_IR_LOAD_PUSH:
	case DRV_GPU_IR_LOAD_UBO:
		/* A uniform word read straight from the cache's SGPRs. */
		aliased = alias_uniform(lowering, instruction);
		return aliased;
	case DRV_GPU_IR_LOAD_INPUT:
		/* A vertex program's fetched channels and numbers stay in their VGPRs. */
		if (lowering->ir->stage != DRV_GPU_STAGE_VERTEX)
			return 0;
		if (instruction->location == DRV_GPU_SHADER_LOCATION_VERTEX_INDEX) {
			code = AMDRDNA_GFX10_V(lowering->vertex_index_vgpr);
		} else if (instruction->location == DRV_GPU_SHADER_LOCATION_INSTANCE_INDEX) {
			code = AMDRDNA_GFX10_V(lowering->instance_index_vgpr);
		} else if (instruction->location < AMDRDNA_GFX10_LOCATIONS && instruction->component < 4U) {
			code = AMDRDNA_GFX10_V(lowering->input_vgpr[instruction->location] + instruction->component);
		} else {
			return 0;
		}

		break;
	case DRV_GPU_IR_LOAD_SYSTEM:
		/* The ids a compute program receives stay where they arrive. */
		if (lowering->ir->stage != DRV_GPU_STAGE_COMPUTE)
			return 0;
		if (instruction->component == DRV_GPU_IR_SYSTEM_LOCAL_ID_X) {
			code = AMDRDNA_GFX10_V(GFX10_CS_LOCAL_ID_X);
		} else if (instruction->component == DRV_GPU_IR_SYSTEM_LOCAL_ID_Y) {
			code = AMDRDNA_GFX10_V(GFX10_CS_LOCAL_ID_Y);
		} else if (instruction->component == DRV_GPU_IR_SYSTEM_LOCAL_ID_Z) {
			code = AMDRDNA_GFX10_V(GFX10_CS_LOCAL_ID_Z);
		} else if (instruction->component == DRV_GPU_IR_SYSTEM_GROUP_ID_X) {
			code = AMDRDNA_GFX10_S(GFX10_CS_GROUP_ID_X);
		} else if (instruction->component == DRV_GPU_IR_SYSTEM_GROUP_ID_Y) {
			code = AMDRDNA_GFX10_S(GFX10_CS_GROUP_ID_X + 1U);
		} else if (instruction->component == DRV_GPU_IR_SYSTEM_GROUP_ID_Z) {
			code = AMDRDNA_GFX10_S(GFX10_CS_GROUP_ID_X + 2U);
		} else {
			return 0;
		}

		break;
	case DRV_GPU_IR_STORAGE_SIZE:
		/* The size is the record count word of the buffer's descriptor. */
		if (instruction->location >= lowering->ir->uniform_count)
			return 0;
		uniform = &lowering->ir->uniforms[instruction->location];
		if (uniform->kind != DRV_GPU_IR_UNIFORM_STORAGE)
			return 0;

		/* A descriptor loaded at its use is not kept: the size is copied out of the window instead. */
		if (lowering->storage_sgpr[lowering->slot[instruction->location]] == GFX10_DESCRIPTOR_AT_USE)
			return 0;
		code = AMDRDNA_GFX10_S(lowering->storage_sgpr[lowering->slot[instruction->location]] + 2U);
		break;
	default:
		return 0;
	}

	/* The value is its operand. */
	lowering->operand_code[instruction->dst] = (uint16_t)code;

	/* Succeeded: the instruction has an operand of its own. */
	return 1;
}

/*
 * Reads a uniform word straight from the cache's SGPRs: finds or loads
 * its chunk in an entry, which then stays until the value's last read.
 * Returns 0 when the read is malformed (lower_uniform() refuses it) or no
 * entry is free.
 */
static int
alias_uniform(
	struct gfx10_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction)
{
	const struct drv_gpu_shader_ir_uniform *uniform;
	uint32_t slot;
	uint32_t entry;

	/* The buffer, the push constants or a uniform buffer; a malformed read is left to the lowering's refusal. */
	slot = GFX10_NO_SLOT;
	if (instruction->op == DRV_GPU_IR_LOAD_PUSH) {
		if (instruction->immediate + 4U > lowering->ir->push_bytes)
			return 0;
	} else {
		if (instruction->location >= lowering->ir->uniform_count)
			return 0;
		uniform = &lowering->ir->uniforms[instruction->location];
		if (uniform->kind != DRV_GPU_IR_UNIFORM_BLOCK)
			return 0;
		slot = lowering->slot[instruction->location];
	}

	/* The chunk in an entry of the cache, without the spare chunk. */
	entry = cache_chunk(lowering, slot, instruction->immediate & ~15U, 0);
	if (entry == GFX10_NO_SLOT)
		return 0;

	/* The word's SGPR is the value; the entry stays until its last read. */
	lowering->operand_code[instruction->dst] =
		(uint16_t)AMDRDNA_GFX10_S(lowering->cache_sgpr + 4U * entry + (instruction->immediate & 15U) / 4U);
	if (lowering->last_use[instruction->dst] > lowering->cache_until[entry])
		lowering->cache_until[entry] = lowering->last_use[instruction->dst];

	/* Succeeded: the word needs no copy. */
	return 1;
}

/*
 * Finds a chunk of 16 uniform bytes in the cache, or loads it into an
 * entry no value still reads, the entries taken in turn.  When every
 * entry is still read, it is loaded into the spare chunk after them if
 * spare is nonzero (GFX10_CHUNK_CACHE is returned; the chunk is not kept),
 * or GFX10_NO_SLOT is returned.
 */
static uint32_t
cache_chunk(
	struct gfx10_lowering *lowering,
	uint32_t slot,
	uint32_t offset,
	int spare)
{
	uint32_t entry;
	uint32_t step;
	uint32_t found;
	uint32_t base;
	uint32_t descriptor;

	/* An entry holding the chunk. */
	for (entry = 0U; entry < GFX10_CHUNK_CACHE; entry++) {
		if (lowering->cache_valid[entry] != 0U &&
		    lowering->cache_slot[entry] == slot &&
		    lowering->cache_offset[entry] == offset)
			return entry;
	}

	/* An entry no value still reads, from the next in turn. */
	found = GFX10_NO_SLOT;
	for (step = 0U; step < GFX10_CHUNK_CACHE; step++) {
		entry = (lowering->cache_next + step) % GFX10_CHUNK_CACHE;
		if (lowering->cache_until[entry] <= lowering->index) {
			found = entry;
			break;
		}
	}

	/* Every entry is still read: the spare chunk, or nothing. */
	if (found == GFX10_NO_SLOT && spare == 0)
		return GFX10_NO_SLOT;
	if (found == GFX10_NO_SLOT) {
		found = GFX10_CHUNK_CACHE;
	} else {
		lowering->cache_next = (found + 1U) % GFX10_CHUNK_CACHE;
		lowering->cache_valid[found] = 1U;
		lowering->cache_slot[found] = slot;
		lowering->cache_offset[found] = offset;
		lowering->cache_until[found] = 0U;
	}

	/* Loads the chunk: the push constants from the table, a uniform buffer's through its descriptor. */
	base = lowering->cache_sgpr + 4U * found;
	if (slot == GFX10_NO_SLOT) {
		emit_scalar_load(lowering, AMDRDNA_GFX10_S_LOAD_DWORDX4, base, AMDRDNA_GFX10_ABI_TABLE, offset, 4U);
	} else {
		descriptor = uniform_descriptor(lowering, slot);
		emit_scalar_load(lowering,
				 AMDRDNA_GFX10_S_BUFFER_LOAD_DWORDX4,
				 base,
				 descriptor,
				 offset,
				 4U);
	}

	/* Reports where the chunk is. */
	return found;
}

/*
 * Finds how long a vertex program reads its fetched locations and its
 * vertex and instance numbers: up to the last read of each value that
 * stays in their VGPRs, or to the input read itself for a value a MOVE
 * writes (which is copied out).
 */
static void
plan_input_lifetimes(
	struct gfx10_lowering *lowering)
{
	const struct drv_gpu_shader_ir_inst *instruction;
	uint32_t index;
	uint32_t until;
	uint32_t location;

	/* Only a vertex program fetches. */
	if (lowering->ir->stage != DRV_GPU_STAGE_VERTEX)
		return;

	/* Each input read that is lowered. */
	for (index = 0U; index < lowering->ir->instruction_count; index++) {
		instruction = &lowering->ir->instructions[index];
		if (instruction->op != DRV_GPU_IR_LOAD_INPUT || lowering->dead[index] != 0U)
			continue;

		/* The read itself, or the value's last read when it stays in the input VGPR. */
		until = index + 1U;
		if (lowering->moved[instruction->dst] == 0U && lowering->last_use[instruction->dst] > until)
			until = lowering->last_use[instruction->dst];

		/* The input it reads. */
		location = instruction->location;
		if (location == DRV_GPU_SHADER_LOCATION_VERTEX_INDEX) {
			if (until > lowering->vertex_index_until)
				lowering->vertex_index_until = until;
		} else if (location == DRV_GPU_SHADER_LOCATION_INSTANCE_INDEX) {
			if (until > lowering->instance_index_until)
				lowering->instance_index_until = until;
		} else if (location < AMDRDNA_GFX10_LOCATIONS) {
			if (until > lowering->input_until[location])
				lowering->input_until[location] = until;
		}
	}
}

/*
 * Gives back a vertex program's input VGPRs read for the last time by now
 * (upto is the IR instruction just lowered plus one, 0 at the start).
 */
static void
return_inputs(
	struct gfx10_lowering *lowering,
	uint32_t upto)
{
	struct amdrdna_gfx10_program *program;
	uint32_t location;
	uint32_t channel;

	/* Names the program being fetched for. */
	program = lowering->program;

	/* Only a vertex program fetches. */
	if (lowering->ir->stage != DRV_GPU_STAGE_VERTEX)
		return;

	/* Each fetched location read no more. */
	for (location = 0U; location < AMDRDNA_GFX10_LOCATIONS; location++) {
		if ((program->fetched_locations & (1U << location)) == 0U)
			continue;
		if ((lowering->inputs_returned & (1U << location)) != 0U)
			continue;
		if (lowering->input_until[location] > upto)
			continue;
		for (channel = 0U; channel < 4U; channel++)
			give_vgpr(lowering, lowering->input_vgpr[location] + channel);
		lowering->inputs_returned |= 1U << location;
	}

	/* The vertex number, read no more. */
	if (lowering->vertex_index_returned == 0 && lowering->vertex_index_until <= upto) {
		give_vgpr(lowering, lowering->vertex_index_vgpr);
		lowering->vertex_index_returned = 1;
	}

	/* The instance number, read no more. */
	if (lowering->instance_index_returned == 0 && lowering->instance_index_until <= upto) {
		give_vgpr(lowering, lowering->instance_index_vgpr);
		lowering->instance_index_returned = 1;
	}
}

/* Reports the next scalar temporary, in turn. */
static unsigned
next_scalar_temp(
	struct gfx10_lowering *lowering)
{
	unsigned temp;

	/* Hands the temporaries out in a ring; the waits keep a reused one correct. */
	temp = lowering->scalar_temp[lowering->scalar_temp_next];
	lowering->scalar_temp_next = (lowering->scalar_temp_next + 1U) % GFX10_SCALAR_TEMPS;

	/* Reports the temporary. */
	return temp;
}

/* Takes the lowest free VGPR. */
static int
take_vgpr(
	struct gfx10_lowering *lowering,
	uint32_t *reg)
{
	uint32_t index;
	int error;

	/* The lowest free one from the first the allocator hands out. */
	for (index = lowering->vgpr_first; index < AMDRDNA_GFX10_VGPR_COUNT; index++) {
		if (lowering->taken[index] == 0U)
			break;
	}

	/* No VGPR is free: the program needs more than there are. */
	if (index == AMDRDNA_GFX10_VGPR_COUNT) {
		error = refuse(lowering, ENOTSUP, "the program needs more VGPRs than there are");
		return error;
	}

	/* Takes it, and counts it among those the program uses. */
	lowering->taken[index] = 1U;
	if (index + 1U > lowering->vgpr_highest)
		lowering->vgpr_highest = index + 1U;

	/* Succeeded: the VGPR is the caller's. */
	*reg = index;
	return 0;
}

/* Takes the lowest two consecutive free VGPRs; *reg is the first. */
static int
take_vgpr_pair(
	struct gfx10_lowering *lowering,
	uint32_t *reg)
{
	uint32_t index;
	int error;

	/* The lowest free one whose next is free too. */
	for (index = lowering->vgpr_first; index + 1U < AMDRDNA_GFX10_VGPR_COUNT; index++) {
		if (lowering->taken[index] == 0U && lowering->taken[index + 1U] == 0U)
			break;
	}

	/* No pair is free. */
	if (index + 1U >= AMDRDNA_GFX10_VGPR_COUNT) {
		error = refuse(lowering, ENOTSUP, "the program needs more VGPRs than there are");
		return error;
	}

	/* Takes both. */
	lowering->taken[index] = 1U;
	lowering->taken[index + 1U] = 1U;
	if (index + 2U > lowering->vgpr_highest)
		lowering->vgpr_highest = index + 2U;

	/* Succeeded: the pair is the caller's. */
	*reg = index;
	return 0;
}

/* Takes the lowest count consecutive free VGPRs; *reg is the first. */
static int
take_vgpr_block(
	struct gfx10_lowering *lowering,
	uint32_t count,
	uint32_t *reg)
{
	uint32_t index;
	uint32_t inner;
	int free_run;
	int error;

	/* The lowest start whose count registers are all free. */
	for (index = lowering->vgpr_first; index + count <= AMDRDNA_GFX10_VGPR_COUNT; index++) {
		free_run = 1;
		for (inner = 0U; inner < count; inner++) {
			if (lowering->taken[index + inner] != 0U) {
				free_run = 0;
				break;
			}
		}

		/* A run with no taken register ends the search. */
		if (free_run != 0)
			break;
	}

	/* No such run is free. */
	if (index + count > AMDRDNA_GFX10_VGPR_COUNT) {
		error = refuse(lowering, ENOTSUP, "the program needs more VGPRs than there are");
		return error;
	}

	/* Takes them all. */
	for (inner = 0U; inner < count; inner++)
		lowering->taken[index + inner] = 1U;
	if (index + count > lowering->vgpr_highest)
		lowering->vgpr_highest = index + count;

	/* Succeeded: the block is the caller's. */
	*reg = index;
	return 0;
}

/* Gives a VGPR back. */
static void
give_vgpr(
	struct gfx10_lowering *lowering,
	uint32_t reg)
{
	/* The VGPR is free again. */
	lowering->taken[reg] = 0U;
}

/* Gives back the VGPRs of the values no later instruction reads. */
static void
retire(
	struct gfx10_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction)
{
	uint32_t sources[GFX10_MAX_SOURCES];
	uint32_t source;
	uint32_t value;
	int count;
	int defines;

	/* Each source read here for the last time. */
	count = ir_sources(instruction, sources);
	for (source = 0U; count > 0 && source < (uint32_t)count; source++) {
		value = sources[source];
		if (lowering->last_use[value] != lowering->index + 1U)
			continue;
		if (lowering->vgpr[value] == GFX10_NO_REG)
			continue;
		give_vgpr(lowering, lowering->vgpr[value]);
		lowering->vgpr[value] = GFX10_NO_REG;
	}

	/* A destination no instruction reads goes back at once. */
	defines = ir_defines(instruction);
	for (source = 0U; source < (uint32_t)defines; source++) {
		value = instruction->dst + source;
		if (lowering->last_use[value] <= lowering->index + 1U && lowering->vgpr[value] != GFX10_NO_REG) {
			give_vgpr(lowering, lowering->vgpr[value]);
			lowering->vgpr[value] = GFX10_NO_REG;
		}
	}
}
