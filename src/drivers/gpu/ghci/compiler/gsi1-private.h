/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GSI1 compiler's state and the functions its files share.
 *
 * gsi1-compile.c drives a compilation: it checks the IR, finds how long
 * each value lives, hands out the registers, lowers the arithmetic and
 * the control flow, and fills in the program record.  gsi1-io.c lowers
 * the inputs and outputs of the vertex and fragment stages, gsi1-memory.c
 * the uniforms, the buffers, the shared memory and the images (and lays
 * out the pipeline's slots and resource table), and gsi1-texture.c the
 * sampled images.  A program that does not fit its
 * registers is compiled again with its pure instructions moved down to
 * their readers (gsi1-schedule.c), and then with values kept in the
 * warp's scratch memory (gsi1-spill.c).
 */

#ifndef DRIVERS_GPU_GHCI_COMPILER_GSI1_PRIVATE_H
#define DRIVERS_GPU_GHCI_COMPILER_GSI1_PRIVATE_H

#include "gsi1-abi.h"
#include "gsi1-compile.h"
#include "gsi1-encode.h"
#include "gsi1_isa.h"

#include "drivers/gpu/compiler/ir.h"

#include <stddef.h>
#include <stdint.h>

/* A value or an input without an operand, and an IR instruction or label that is not there. */
#define GSI1_NO_OPERAND			0xffU
#define GSI1_NOT_DEFINED		UINT32_MAX

/* The deepest loop nest and skip nest the compiler follows. */
#define GSI1_LOOP_DEPTH			16U
#define GSI1_SKIP_DEPTH			32U

/* The most values one IR instruction reads, and the scratch registers it may take. */
#define GSI1_MAX_SOURCES		DRV_GPU_IR_TEXTURE_MAX_PARAMS
#define GSI1_SCRATCH_MAX		32U

/* The instructions the code list grows by when it is full, and the labels likewise. */
#define GSI1_LIST_GROWTH		256U
#define GSI1_LABEL_GROWTH		64U

/*
 * The output channels a program stores, by number.  A vertex program:
 * the position (0-3), the point size (4), and four channels for each
 * location from 8.  A fragment program: four for each colour attachment
 * (0-31), the depth (32), the sample mask (33) and the four of the
 * second colour of attachment 0 (34-37).
 */
#define GSI1_OUTPUT_POSITION		0U
#define GSI1_OUTPUT_POINT_SIZE		4U
#define GSI1_OUTPUT_LOCATIONS		8U
#define GSI1_OUTPUT_COLOR		0U
#define GSI1_OUTPUT_DEPTH		(4U * GSI1_COLOR_ATTACHMENTS)
#define GSI1_OUTPUT_SAMPLE_MASK		(GSI1_OUTPUT_DEPTH + 1U)
#define GSI1_OUTPUT_SECOND_COLOR	(GSI1_OUTPUT_SAMPLE_MASK + 1U)
#define GSI1_OUTPUT_CHANNELS		(GSI1_OUTPUT_LOCATIONS + 4U * GHCI_GSI1_LOCATIONS)

/* The IR instruction index that stands for "after the last instruction" (the exports at the end). */
#define GSI1_END_OF_PROGRAM(ir)		((ir)->instruction_count + 1U)

/*
 * How a value is reached.
 *
 * A register value lives in a register from its definition to its last
 * read.  An operand value is a special operand or a constant-bank word
 * that is always there.  A constant is put into a scratch register where
 * an instruction reads it, so that a constant the parser shares over the
 * whole program does not hold a register all along.
 */
enum gsi1_value_kind {
	GSI1_VALUE_NONE = 0,
	GSI1_VALUE_REGISTER,
	GSI1_VALUE_OPERAND,
	GSI1_VALUE_CONSTANT
};

/*
 * The bytes of one word of the scratch memory for the 32 lanes of a warp:
 * word s of lane l is at 128 s + 4 l of the warp's part.
 */
#define GSI1_SPILL_SLOT_BYTES		128U

/*
 * One place a register value was defined: the value, the register it was
 * written to, and the code index right after the instructions that wrote
 * it, where a store into the scratch memory goes when the value is
 * spilled later.
 */
struct gsi1_definition {
	uint32_t value;
	uint32_t reg;
	uint32_t position;
};

/*
 * What a compile with spills keeps.
 *
 * Only a compile that may spill fills it (enabled is nonzero); its tables
 * are allocated in gsi1_spill_prepare() and freed with the lowering's.
 */
struct gsi1_spill {
	/* Nonzero when values may be kept in the scratch memory. */
	int enabled;

	/* The register of the lane's address in the warp's part, and the code index of its movi of the part's words. */
	uint32_t base;
	uint32_t patch;

	/* The bytes of a warp's part, once the code is done. */
	uint32_t bytes;

	/* Each value: its scratch word plus one (0: in a register), and the first IR instruction that defines it. */
	uint32_t *slot;
	uint32_t *first_definition;

	/* Each value: the IR instruction after which nothing reads or writes it, plus one. */
	uint32_t *last_touch;

	/* Each value: nonzero once a definition of it is in the list below. */
	uint8_t *recorded;

	/* Each value: the instruction the lowering is at when it was last read, so that it is not spilled under that instruction. */
	uint32_t *pinned;
	uint32_t pin_stamp;

	/* Each value's readers, in order: value v's are reader[reader_start[v]] to reader[reader_start[v + 1] - 1]. */
	uint32_t *reader_start;
	uint32_t *reader;

	/* Each scratch word: the IR instruction after which its values are no longer needed, plus one. */
	uint32_t *slot_busy;
	uint32_t slot_count;
	uint32_t slot_capacity;

	/* The places where register values were defined, in code order. */
	struct gsi1_definition *definition;
	uint32_t definition_count;
	uint32_t definition_capacity;
};

/*
 * The registers the hardware or the program's start fills, which IR
 * instructions read: a fragment warp's barycentrics, depth, position and
 * sample mask, the centroid barycentrics, the screen-linear barycentrics
 * and the reciprocal of w the start computes, and a vertex warp's system
 * values and attribute components.
 */
enum gsi1_input {
	GSI1_INPUT_B0 = 0,
	GSI1_INPUT_B1,
	GSI1_INPUT_B2,
	GSI1_INPUT_Z,
	GSI1_INPUT_X,
	GSI1_INPUT_Y,
	GSI1_INPUT_SAMPLE_MASK,
	GSI1_INPUT_CENTROID_B0,
	GSI1_INPUT_CENTROID_B1,
	GSI1_INPUT_CENTROID_B2,
	GSI1_INPUT_LINEAR_B0,
	GSI1_INPUT_LINEAR_B1,
	GSI1_INPUT_LINEAR_B2,
	GSI1_INPUT_LINEAR_CENTROID_B0,
	GSI1_INPUT_LINEAR_CENTROID_B1,
	GSI1_INPUT_LINEAR_CENTROID_B2,
	GSI1_INPUT_RECIPROCAL_W,
	GSI1_INPUT_ATTRIBUTE,
	GSI1_INPUT_COUNT = GSI1_INPUT_ATTRIBUTE + 4 * GHCI_GSI1_LOCATIONS
};

/* What one uniform of the IR's list is to the program (GSI1_NO_BINDING: nothing it reaches). */
enum gsi1_binding_kind {
	GSI1_NO_BINDING = 0,
	GSI1_BINDING_SAMPLED_IMAGE,
	GSI1_BINDING_UNIFORM_BUFFER,
	GSI1_BINDING_STORAGE_BUFFER,
	GSI1_BINDING_STORAGE_IMAGE,
	GSI1_BINDING_TEXEL_BUFFER,
	GSI1_BINDING_INPUT_ATTACHMENT
};

/*
 * The descriptor fields a program reads from a resource table entry:
 * a buffer's address and bytes, a texel buffer's elements (bytes over
 * stride), and an image's address, row and layer bytes, width | height
 * << 16, layers (or depth) and samples.
 */
enum gsi1_field {
	GSI1_FIELD_BASE = 0,
	GSI1_FIELD_BYTES,
	GSI1_FIELD_ELEMENTS,
	GSI1_FIELD_ROW_BYTES,
	GSI1_FIELD_LAYER_BYTES,
	GSI1_FIELD_EXTENT,
	GSI1_FIELD_LAYERS,
	GSI1_FIELD_SAMPLES
};

/*
 * How the program reaches one uniform of the IR's list: its kind, the
 * byte offset of its entry in the resource table (a sampled image has one
 * only when it is a uniform texel buffer, for its element count;
 * GSI1_NOT_DEFINED otherwise), a sampled image's texture and sampler
 * slots, the index of a buffer's dynamic offset (GSI1_NOT_DEFINED for
 * none), and an image's VkFormat and samples.
 */
struct gsi1_binding {
	uint32_t kind;
	uint32_t entry;
	uint32_t slot;
	uint32_t sampler;
	uint32_t dynamic;
	uint32_t format;
	uint32_t samples;
};

/*
 * The state of one compilation.
 *
 * Lives on the stack of ghci_gsi1_compile_ir(); its arrays are allocated
 * there and freed before it returns.
 */
struct gsi1_lowering {
	/* The program, the options it is compiled with, and where a refusal is described. */
	const struct drv_gpu_shader_ir *ir;
	struct ghci_gsi1_options options;
	struct ghci_gsi1_diagnostic *diagnostic;

	/* The IR instruction being lowered. */
	uint32_t index;

	/*
	 * Each value: how it is reached, its operand (a register, a special
	 * operand or a bank word), its bits when it is a constant, the IR
	 * instruction that last reads it plus one (0: none; the end of the
	 * program for a value the exports read), and whether a MOVE writes
	 * it (it then always has a register of its own).
	 */
	uint8_t *kind;
	uint8_t *operand;
	uint32_t *constant;
	uint32_t *last_use;
	uint8_t *moved;

	/* Each IR instruction whose results nothing reads and that has no other effect: it is not lowered. */
	uint8_t *dead;

	/* The registers a lane has (at most 63: r63 is the systolic queue), which are taken, and one past the highest ever taken. */
	uint32_t register_limit;
	uint8_t taken[GSI1_REGISTERS_MAX];
	uint32_t register_highest;

	/*
	 * Each input: its register (GSI1_NO_OPERAND when the program has none)
	 * and the IR instruction after which nothing reads it, plus one (0:
	 * only the start).  An input's register goes back at that point.
	 */
	uint8_t input_reg[GSI1_INPUT_COUNT];
	uint32_t input_until[GSI1_INPUT_COUNT];

	/* The scratch registers taken for the IR instruction being lowered, given back after it. */
	uint32_t scratch[GSI1_SCRATCH_MAX];
	uint32_t scratch_count;

	/* The code: instructions, room, and each instruction's branch label plus one (0: not a branch). */
	uint64_t *code;
	uint32_t *branch_label;
	uint32_t count;
	uint32_t capacity;

	/* The labels: each one's instruction index (GSI1_NOT_DEFINED until placed), and room. */
	uint32_t *label_at;
	uint32_t label_count;
	uint32_t label_capacity;

	/* Nonzero when a list could not grow. */
	int out_of_memory;

	/*
	 * The loops being lowered: each one's label at its start, the register
	 * its EXEC is saved in, and its LOOP_BEGIN instruction, innermost last.
	 */
	uint32_t loop_label[GSI1_LOOP_DEPTH];
	uint32_t loop_save[GSI1_LOOP_DEPTH];
	uint32_t loop_start[GSI1_LOOP_DEPTH];
	uint32_t loop_depth;

	/* The depth of the skippable blocks being lowered. */
	uint32_t skip_depth;

	/* Each uniform of the IR's list: how the program reaches it. */
	struct gsi1_binding *binding;

	/* The pipeline's slots and resource table, from the options and with this program's added. */
	struct ghci_gsi1_bindings bindings;

	/* The dynamic offsets the program reads (bit i: bank word 40 + i). */
	uint32_t dynamic_used;

	/* The fragment program's GHCI_GSI1_FLAG_* and colour outputs. */
	uint32_t flags;
	uint32_t color_outputs;

	/* The vec4 varyings the program writes or reads, and a vertex program's attribute registers. */
	uint32_t varying_count;
	uint32_t attribute_registers;

	/*
	 * Each output channel of a fragment program, exported at the end: the
	 * value its export reads where it lives (GSI1_NOT_DEFINED for none),
	 * when the channel is stored once outside any loop; otherwise the
	 * register the stores copy into (GSI1_NO_OPERAND until the first
	 * store takes one, kept to the end).  And, in either stage, whether
	 * the program stores it at all.
	 */
	uint32_t output_value[GSI1_OUTPUT_CHANNELS];
	uint8_t output_reg[GSI1_OUTPUT_CHANNELS];
	uint8_t output_written[GSI1_OUTPUT_CHANNELS];

	/*
	 * Each output channel of a vertex program: the IR instruction of its
	 * last store, which exports the value stored (GSI1_NOT_DEFINED for a
	 * channel the program does not store).
	 */
	uint32_t output_last[GSI1_OUTPUT_CHANNELS];

	/* A vertex program: each attribute location's first register and its bound components (0: none). */
	uint32_t attribute_base[GHCI_GSI1_LOCATIONS];
	uint32_t attribute_components[GHCI_GSI1_LOCATIONS];

	/*
	 * Nonzero once a register was asked for when none was free and none
	 * could be spilled: the compile is then tried again in a way that
	 * needs fewer registers.
	 */
	int short_of_registers;

	/* The values kept in the scratch memory. */
	struct gsi1_spill spill;
};

/* Describes a refusal at the instruction being lowered and returns its error. */
int gsi1_refuse(struct gsi1_lowering *lowering, int error, const char *reason);

/* Lists the values an IR instruction reads; -1 for an operation the compiler does not lower. */
int gsi1_ir_sources(const struct drv_gpu_shader_ir_inst *instruction, uint32_t *sources);

/* Reports how many values an IR instruction defines from its destination: 0, 1 or 4. */
int gsi1_ir_defines(const struct drv_gpu_shader_ir_inst *instruction);

/* Appends one instruction to the code. */
void gsi1_emit(struct gsi1_lowering *lowering, uint64_t instruction);

/* Inserts one instruction at a code index, moving the later code, labels and definitions up. */
void gsi1_insert(struct gsi1_lowering *lowering, uint32_t position, uint64_t instruction);

/* Appends a branch to a label. */
void gsi1_emit_branch(struct gsi1_lowering *lowering, uint32_t funct3, uint32_t rs1, uint32_t label);

/* Makes a new label, not yet placed; GSI1_NOT_DEFINED when the table could not grow. */
uint32_t gsi1_new_label(struct gsi1_lowering *lowering);

/* Places a label at the next instruction. */
void gsi1_place_label(struct gsi1_lowering *lowering, uint32_t label);

/* Takes the lowest free register for the whole of a value's life. */
int gsi1_take_register(struct gsi1_lowering *lowering, uint32_t *reg);

/* Takes the lowest run of free registers. */
int gsi1_take_block(struct gsi1_lowering *lowering, uint32_t count, uint32_t *reg);

/* Gives a register back. */
void gsi1_give_register(struct gsi1_lowering *lowering, uint32_t reg);

/* Takes a scratch register for the instruction being lowered. */
int gsi1_scratch(struct gsi1_lowering *lowering, uint32_t *reg);

/* Takes a run of scratch registers for the instruction being lowered. */
int gsi1_scratch_block(struct gsi1_lowering *lowering, uint32_t count, uint32_t *reg);

/* Reports the scratch registers taken so far, for gsi1_scratch_release(). */
uint32_t gsi1_scratch_mark(const struct gsi1_lowering *lowering);

/* Gives back the scratch registers taken after a mark. */
void gsi1_scratch_release(struct gsi1_lowering *lowering, uint32_t mark);

/* Finds the operand an instruction reads a value through, putting a constant into a scratch register. */
int gsi1_source(struct gsi1_lowering *lowering, uint32_t value, uint32_t *operand);

/* Reports the register a value defined by the instruction being lowered lives in. */
uint32_t gsi1_value_register(const struct gsi1_lowering *lowering, uint32_t value);

/* Reports whether a value is a constant, with its bits. */
int gsi1_value_constant(const struct gsi1_lowering *lowering, uint32_t value, uint32_t *bits);

/* Emits a copy of an operand into a register. */
void gsi1_emit_move(struct gsi1_lowering *lowering, uint32_t rd, uint32_t operand);

/* Emits a register set to a constant. */
void gsi1_emit_constant(struct gsi1_lowering *lowering, uint32_t rd, uint32_t bits);

/* Emits an integer instruction of the OP opcode. */
void gsi1_emit_alu(struct gsi1_lowering *lowering, uint32_t funct3, uint32_t funct7, uint32_t rd, uint32_t rs1, uint32_t rs2);

/* Emits an integer instruction of the OP-IMM opcode with a 22-bit immediate. */
void gsi1_emit_alu_immediate(struct gsi1_lowering *lowering, uint32_t funct3, uint32_t rd, uint32_t rs1, int32_t immediate);

/* Emits a float instruction of the OP-FP opcode with its modifiers. */
void gsi1_emit_float(struct gsi1_lowering *lowering, uint32_t funct7, uint32_t funct3, uint32_t rd, uint32_t rs1, uint32_t rs2, uint32_t modifiers);

/* Emits a fused multiply-add family instruction. */
void gsi1_emit_fused(struct gsi1_lowering *lowering, uint32_t opcode, uint32_t rd, uint32_t rs1, uint32_t rs2, uint32_t rs3, uint32_t modifiers);

/* Emits one of the transcendental unit's operations. */
void gsi1_emit_transcendental(struct gsi1_lowering *lowering, uint32_t sub, uint32_t rd, uint32_t rs1);

/* Emits an exec.* operation: exec.save writes rd, the others read rs1. */
void gsi1_emit_exec(struct gsi1_lowering *lowering, uint32_t sub, uint32_t operand);

/* Computes a buffer access's address: the descriptor's address, the dynamic offset and the offset, clamped to the buffer when asked. */
int gsi1_buffer_address(struct gsi1_lowering *lowering, const struct gsi1_binding *binding, uint32_t offset, int clamp, uint32_t *address);

/* Reads a constant-bank word as an operand: c0-c31 directly, a later word through a scratch register. */
int gsi1_bank_operand(struct gsi1_lowering *lowering, uint32_t bank, uint32_t *operand);

/* Reads a field of a uniform's resource table entry into a scratch register. */
int gsi1_resource_field(struct gsi1_lowering *lowering, const struct gsi1_binding *binding, uint32_t field, uint32_t *operand);

/* Reads a word of the driver's table (set 3) into a scratch register. */
int gsi1_driver_word(struct gsi1_lowering *lowering, uint32_t offset, uint32_t *operand);

/* Finds the interface record of an input location; NULL when the program has none. */
const struct drv_gpu_shader_ir_io *gsi1_find_input(const struct drv_gpu_shader_ir *ir, uint32_t location);

/* Finds the interface record of an output location; NULL when the program has none. */
const struct drv_gpu_shader_ir_io *gsi1_find_output(const struct drv_gpu_shader_ir *ir, uint32_t location);

/* gsi1-io.c: lists the inputs an IR instruction reads, as GSI1_INPUT_* numbers. */
uint32_t gsi1_io_inputs(const struct gsi1_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t *inputs);

/* gsi1-io.c: checks a graphics program's inputs and outputs and plans their registers and varying words. */
int gsi1_io_prepare(struct gsi1_lowering *lowering);

/* gsi1-io.c: plans where each output channel is exported and what the export reads. */
void gsi1_io_plan_outputs(struct gsi1_lowering *lowering);

/* gsi1-io.c: takes the inputs' registers and computes those the start makes. */
int gsi1_io_prologue(struct gsi1_lowering *lowering);

/* gsi1-io.c: lowers the read of an input. */
int gsi1_io_lower_input(struct gsi1_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);

/* gsi1-io.c: lowers the write of an output. */
int gsi1_io_lower_output(struct gsi1_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction);

/* gsi1-io.c: emits the exports at the end of the program. */
int gsi1_io_finish(struct gsi1_lowering *lowering);

/* gsi1-memory.c: gives the uniforms their slots, resource table entries and dynamic offsets. */
int gsi1_memory_prepare(struct gsi1_lowering *lowering);

/* gsi1-memory.c: lowers a uniform word, a buffer access, an atomic, a shared access or a synchronization. */
int gsi1_memory_lower(struct gsi1_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);

/* gsi1-image.c: lowers a read or a write of a storage image, a texel buffer or an input attachment. */
int gsi1_image_lower(struct gsi1_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);

/* gsi1-image.c: computes an image atomic's texel address and whether it is inside the image. */
int gsi1_image_texel(struct gsi1_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, const struct gsi1_binding *binding, uint32_t *address, uint32_t *inside);

/* gsi1-texture.c: lowers a sample, a fetch, a gather or a query of a sampled image. */
int gsi1_texture_lower(struct gsi1_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);

/* gsi1-schedule.c: makes a copy of the instructions with each pure one moved down to its first reader. */
int gsi1_schedule_sink(const struct drv_gpu_shader_ir *ir, struct drv_gpu_shader_ir_inst **instructions);

/* gsi1-spill.c: plans the spills of a compile that may spill: the readers and lives of the values. */
int gsi1_spill_prepare(struct gsi1_lowering *lowering);

/* gsi1-spill.c: computes the lane's address in the warp's scratch memory at the program's start. */
int gsi1_spill_prologue(struct gsi1_lowering *lowering);

/* gsi1-spill.c: starts the lowering of an instruction: the values read before it may be spilled again. */
void gsi1_spill_next(struct gsi1_lowering *lowering);

/* gsi1-spill.c: keeps a value in its register until the instruction being lowered is done. */
void gsi1_spill_pin(struct gsi1_lowering *lowering, uint32_t value);

/* gsi1-spill.c: frees a run of registers by moving values into the scratch memory. */
int gsi1_spill_make_room(struct gsi1_lowering *lowering, uint32_t count, uint32_t *first);

/* gsi1-spill.c: reports whether a value is kept in the scratch memory. */
int gsi1_spill_is_spilled(const struct gsi1_lowering *lowering, uint32_t value);

/* gsi1-spill.c: reads a spilled value into a scratch register. */
int gsi1_spill_reload(struct gsi1_lowering *lowering, uint32_t value, uint32_t *operand);

/* gsi1-spill.c: gives a spilled value a scratch register to be defined in, stored after its instruction. */
int gsi1_spill_destination(struct gsi1_lowering *lowering, uint32_t value, uint32_t *reg);

/* gsi1-spill.c: records where the values an instruction defined live, and stores the spilled ones. */
int gsi1_spill_defined(struct gsi1_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction);

/* gsi1-spill.c: fills in the size of the warp's scratch memory once the code is done. */
int gsi1_spill_finish(struct gsi1_lowering *lowering);

/* gsi1-spill.c: frees the spill tables. */
void gsi1_spill_free(struct gsi1_lowering *lowering);

#endif /* DRIVERS_GPU_GHCI_COMPILER_GSI1_PRIVATE_H */
