/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PowerVR shader compiler's working memory, shared between the
 * SPIR-V reader and the back end.  The same code runs on the build host.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_SHADER_PRIVATE_H
#define KERN_DRIVERS_GPU_POWERVR_SHADER_PRIVATE_H

#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/powervr/compiler/shader.h"
#include "drivers/gpu/powervr/compiler/shader-ir.h"
#include "drivers/gpu/powervr/compiler/usc-encode.h"

/* The highest id bound, struct member, and pooled operand a module may use. */
#define SHADER_SPIRV_IDS		4096U
#define SHADER_SPIRV_MEMBERS		512U
#define SHADER_SPIRV_MEMBER_DECORATIONS	1024U
#define SHADER_SPIRV_OPERANDS		16384U

/* How deep the reader inlines calls and nests loops. */
#define SHADER_SPIRV_CALLS		8U
#define SHADER_SPIRV_LOOPS		16U

/* The most machine operations, temporaries and labels of the back end. */
#define SHADER_MACHINE_OPERATIONS	4096U
#define SHADER_MACHINE_TEMPORARIES	96U

/*
 * What one SPIR-V id stands for while the reader works.
 *
 * A type keeps its shape; a constant or a computed result keeps its
 * scalar words as pooled operands; a pointer keeps the variable it
 * points into and where; a label keeps where its block starts and its
 * merge instruction; a function keeps where it starts.
 */
struct shader_spirv_id {
	uint8_t kind;
	uint8_t type_kind;
	uint8_t is_signed;
	uint8_t flags;
	uint32_t type;
	uint32_t count;
	uint32_t first;
	uint32_t scalars;
	uint32_t stride;
	uint32_t storage;
	uint32_t location;
	uint32_t component;
	uint32_t set;
	uint32_t binding;
	uint32_t builtin;
	uint32_t variable;
	uint32_t offset;
	uint32_t index_stride;
	uint32_t interface;
	uint32_t words;
	struct powervr_ir_operand index;
	uint32_t word;
	uint32_t merge;
	uint32_t continue_target;
};

/* One struct member: its type, byte offset, built-in and first scalar. */
struct shader_spirv_member {
	uint32_t type;
	uint32_t offset;
	uint32_t builtin;
	uint32_t has_builtin;
	uint32_t scalar_offset;
};

/* One member decoration, kept until its struct type is read. */
struct shader_spirv_member_decoration {
	uint32_t structure;
	uint32_t member;
	uint32_t decoration;
	uint32_t value;
};

/* One loop the reader is inside: its header, merge and continue blocks. */
struct shader_spirv_loop {
	uint32_t header;
	uint32_t merge;
	uint32_t continue_target;
	uint32_t ifs;
};

/*
 * The reader's state for one module.
 *
 * The id table and pools are filled by the first pass over the module's
 * declarations; the second pass walks the entry point's blocks and
 * writes the intermediate form.  Discard is the value a fragment
 * shader that discards keeps its discard in (0 for none).
 */
struct shader_spirv_reader {
	const uint32_t *words;
	size_t count;
	uint32_t stage;
	uint32_t bound;
	uint32_t entry;
	uint32_t glsl;
	uint32_t current_label;
	int error;
	struct shader_spirv_id ids[SHADER_SPIRV_IDS];
	struct shader_spirv_member members[SHADER_SPIRV_MEMBERS];
	uint32_t member_count;
	struct shader_spirv_member_decoration member_decorations[SHADER_SPIRV_MEMBER_DECORATIONS];
	uint32_t member_decoration_count;
	struct powervr_ir_operand operands[SHADER_SPIRV_OPERANDS];
	uint32_t operand_count;
	struct shader_spirv_loop loops[SHADER_SPIRV_LOOPS];
	uint32_t loop_count;
	uint32_t calls;
	uint32_t push_request;
	uint32_t push_words;
	uint32_t returned;
	uint32_t discard;
	uint32_t output_values[POWERVR_SHADER_OUTPUTS * 4U];
	struct powervr_ir_shader *shader;
	struct powervr_shader_program *program;
	const struct powervr_shader_key *key;
};

/*
 * One machine operation of the back end, on registers.
 *
 * Each becomes one instruction group.  Kind says which group; the
 * registers, modifiers and fields follow it.  Target is the operation a
 * branch goes to.  Half says an iterate and sample returns its channels
 * as half floats, two to a register.
 */
struct shader_machine_register {
	uint8_t bank;
	uint16_t index;
};

struct shader_machine_operation {
	uint8_t kind;
	uint8_t condition;
	uint8_t variant;
	uint8_t modifiers[3];
	uint8_t end;
	uint8_t saturate;
	uint8_t overlap;
	uint8_t pair;
	uint8_t half;
	struct shader_machine_register destination[2];
	struct shader_machine_register source[4];
	uint32_t immediate;
	uint32_t immediate_2;
	uint32_t target;
};

/* The machine operations, one instruction group each. */
enum machine_kind {
	MACHINE_IMMEDIATE = 0,
	MACHINE_MOVE,
	MACHINE_MOVE_SPECIAL,
	MACHINE_FLOAT_ADD,
	MACHINE_FLOAT_MULTIPLY,
	MACHINE_FLOAT_MULTIPLY_ADD,
	MACHINE_SINGLE,
	MACHINE_MINMAX,
	MACHINE_COMPARE,
	MACHINE_SELECT,
	MACHINE_INTEGER,
	MACHINE_ADD64,
	MACHINE_LOGIC,
	MACHINE_SHIFT,
	MACHINE_PACK,
	MACHINE_UNPACK,
	MACHINE_LOAD,
	MACHINE_STORE,
	MACHINE_WAIT,
	MACHINE_ITERATE,
	MACHINE_SAMPLE,
	MACHINE_VERTEX_WRITE,
	MACHINE_VERTEX_END,
	MACHINE_END,
	MACHINE_TEST_ZERO,
	MACHINE_CONDITION,
	MACHINE_BRANCH,
	MACHINE_ITERATE_SAMPLE,
	MACHINE_SUM_OF_PRODUCTS,
	MACHINE_FEEDBACK
};

/*
 * The back end's state for one shader.  Forms are the key's program
 * forms it takes (POWERVR_SHADER_FORM_ITERATE_SAMPLE, _SUM_OF_PRODUCTS,
 * _SAMPLE_WAITS);
 * coefficient_alias names, one more than its number, the coefficient
 * register a flat varying only float adds, multiplies and multiply-adds
 * read is read from in place (0 for none); vertex_input_alias the same
 * for a vertex input word and its vertex input register;
 * iterate_sample marks the iterated varyings a sample iterates itself;
 * sample_half marks the samples so iterated whose four channels only one
 * sum of products reads, as they are (the sample returns half floats,
 * which the operation reads).
 */
struct shader_backend {
	struct shader_machine_operation operations[SHADER_MACHINE_OPERATIONS];
	unsigned operation_count;
	uint32_t offsets[SHADER_MACHINE_OPERATIONS];
	uint16_t group_first[SHADER_MACHINE_OPERATIONS];
	unsigned group_count;
	uint16_t value_register[POWERVR_IR_VALUES];
	uint16_t shared_alias[POWERVR_IR_VALUES];
	uint16_t coefficient_alias[POWERVR_IR_VALUES];
	uint16_t vertex_input_alias[POWERVR_IR_VALUES];
	uint32_t first_use[POWERVR_IR_VALUES];
	uint32_t last_use[POWERVR_IR_VALUES];
	uint32_t uses[POWERVR_IR_VALUES];
	uint32_t free_after[SHADER_MACHINE_TEMPORARIES];
	uint32_t temps;
	uint32_t control_flow;
	const uint32_t *inline_values;
	uint32_t inline_count;
	uint32_t forms;
	uint8_t iterate_sample[POWERVR_IR_VALUES];
	uint8_t sample_half[POWERVR_IR_VALUES];
	int error;
};

/*
 * The most words a fragment program's shared program computes, and the
 * most machine operations and code bytes it may have.
 */
#define SHADER_SHARED_WORDS		16U
#define SHADER_SHARED_OPERATIONS	512U
#define SHADER_SHARED_CODE		8192U

/*
 * What the split of a fragment program into its shared program knows of
 * each value: how often it is written, whether only uniform words decide
 * it (SHADER_UNIFORM_FREE: a shared word or a constant the program
 * already reads as it is; SHADER_UNIFORM_COMPUTED: worked out from
 * them), the computed word it became (its slot plus one) and whether a
 * kept instruction reads it; and of each instruction, whether it moves
 * to the shared program, the word of the colour it packs, and whether
 * it is kept.
 */
#define SHADER_UNIFORM_VARYING		0U
#define SHADER_UNIFORM_FREE		1U
#define SHADER_UNIFORM_COMPUTED		2U

struct shader_uniform {
	uint16_t writes[POWERVR_IR_VALUES];
	uint8_t kind[POWERVR_IR_VALUES];
	uint8_t slot[POWERVR_IR_VALUES];
	uint8_t needed[POWERVR_IR_VALUES];
	uint8_t moved[POWERVR_IR_INSTRUCTIONS];
	uint8_t pack_slot[POWERVR_IR_INSTRUCTIONS];
	uint8_t kept[POWERVR_IR_INSTRUCTIONS];
};

/*
 * The compiler's working memory.  The shared part holds a fragment
 * program's shared program while it is built: its intermediate form, the
 * program description it is selected with (a compute task's, with the
 * fragment program's requests), its machine operations and its code.
 */
struct powervr_shader_workspace {
	struct shader_spirv_reader reader;
	struct powervr_ir_shader shader;
	struct shader_backend backend;
	struct shader_uniform uniform;
	struct powervr_ir_shader shared_shader;
	struct powervr_shader_program shared_program;
	struct shader_machine_operation shared_operations[SHADER_SHARED_OPERATIONS];
	unsigned shared_operation_count;
	uint8_t shared_code[SHADER_SHARED_CODE];
};

int shader_backend_build(const struct powervr_ir_shader *shader, struct powervr_shader_program *program, struct shader_backend *backend);
int shader_backend_group(const struct shader_machine_operation *operation, int32_t offset, struct powervr_usc_group *group);
int shader_backend_encode(struct shader_backend *backend, uint8_t *code, size_t room, size_t *length);

#endif /* KERN_DRIVERS_GPU_POWERVR_SHADER_PRIVATE_H */
