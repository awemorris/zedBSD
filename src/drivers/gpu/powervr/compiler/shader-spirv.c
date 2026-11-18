/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PowerVR shader compiler's SPIR-V reader: from a module's entry
 * point to the scalar intermediate form.
 *
 * The first pass reads the module's declarations: decorations, types,
 * constants, variables and where each function and block starts.  The
 * second pass walks the entry point's structured control flow from its
 * first block, inlining calls, splitting vectors into words, turning
 * phis into moves on the incoming edges and local variables into plain
 * values, and writes the instructions in the order the shader nests
 * them.  Anything outside the supported subset is refused with ENOTSUP
 * rather than guessed at.
 *
 * This file holds the entry and the first pass, and the small steps
 * the other files share; the interface, the control flow and the
 * instructions are in shader-spirv-interface.c, shader-spirv-control.c
 * and shader-spirv-instruction.c.
 */

#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/powervr/compiler/shader.h"
#include "drivers/gpu/powervr/compiler/shader-ir.h"
#include "drivers/gpu/powervr/compiler/shader-private.h"
#include "drivers/gpu/powervr/compiler/shader-spirv.h"

static int texture_state_request(const struct powervr_shader_program *program, uint32_t index);
static int reader_declarations(struct shader_spirv_reader *reader);
static int reader_declaration(struct shader_spirv_reader *reader, const uint32_t *words, uint32_t length, uint32_t at);
static int reader_type(struct shader_spirv_reader *reader, const uint32_t *words, uint32_t length);
static int reader_global_variable(struct shader_spirv_reader *reader, const uint32_t *words, uint32_t length);
static void reader_decorate(struct shader_spirv_reader *reader, uint32_t target, uint32_t decoration, uint32_t value);
static struct powervr_ir_operand divide_unsigned(struct shader_spirv_reader *reader, struct powervr_ir_operand numerator, struct powervr_ir_operand denominator, uint32_t remainder);
static struct powervr_ir_operand integer_absolute(struct shader_spirv_reader *reader, struct powervr_ir_operand word, struct powervr_ir_operand *negative);

/*
 * Reports how many bytes of working memory the compiler needs.
 */
size_t
powervr_shader_workspace_size(void)
{
	size_t size;

	/* The workspace holds every table of the reader and the back end. */
	size = sizeof(struct powervr_shader_workspace);

	/* Succeeded: the size. */
	return size;
}

/*
 * Reads a SPIR-V module's entry point of the given stage into the
 * intermediate form and fills in the interface part of the program.
 *
 * The program's tables (requests, inputs, varyings, workgroup) are
 * filled; the code and register counts are the back end's.  On success
 * the shader is the workspace's.
 */
int
powervr_shader_read(
	const uint32_t *words,
	size_t count,
	uint32_t stage,
	const struct powervr_shader_key *key,
	struct powervr_shader_workspace *workspace,
	struct powervr_shader_program *program,
	struct powervr_ir_shader **shader)
{
	struct shader_spirv_reader *reader;
	uint32_t result;
	uint8_t *bytes;
	size_t index;
	int error;

	/* Refuses missing arguments and a stage the compiler does not take. */
	if (words == NULL || workspace == NULL || program == NULL || shader == NULL)
		return EINVAL;
	if (stage > POWERVR_SHADER_FRAGMENT)
		return EINVAL;

	/* Refuses a module too short for its header or with the wrong magic number. */
	if (count < SPIRV_HEADER_WORDS || words[0] != SPIRV_MAGIC)
		return EINVAL;
	if (words[3] > SHADER_SPIRV_IDS)
		return ENOTSUP;

	/* Clears the reader, the shader and the program. */
	bytes = (uint8_t *)&workspace->reader;
	for (index = 0; index < sizeof(workspace->reader); index++)
		bytes[index] = 0;
	bytes = (uint8_t *)program;
	for (index = 0; index < sizeof(*program); index++)
		bytes[index] = 0;
	workspace->shader.instruction_count = 0;
	workspace->shader.value_count = 1;
	workspace->shader.exact = 0;

	/* Ties the reader to the module, the shader and the program. */
	reader = &workspace->reader;
	reader->words = words;
	reader->count = count;
	reader->stage = stage;
	reader->bound = words[3];
	reader->shader = &workspace->shader;
	reader->program = program;
	reader->key = key;
	reader->push_request = POWERVR_SHADER_REQUESTS;
	program->stage = stage;
	program->vertex_index_reg = POWERVR_SHADER_NO_REG;
	program->instance_index_reg = POWERVR_SHADER_NO_REG;
	program->point_size_reg = POWERVR_SHADER_NO_REG;
	program->point_coord_coefficient = POWERVR_SHADER_NO_REG;
	program->workgroup[0] = 1;
	program->workgroup[1] = 1;
	program->workgroup[2] = 1;

	/* The first pass: declarations, types, constants, variables, blocks. */
	error = reader_declarations(reader);
	if (error != 0)
		return error;

	/* Refuses a module without an entry point of the stage, and samples other than one or four. */
	if (reader->entry == 0)
		return ENOTSUP;
	if (key != NULL && key->samples > 1 && key->samples != 4)
		return ENOTSUP;

	/* Places the interface: vertex inputs, varyings and requests known up front. */
	error = shader_spirv_reader_layout_interface(reader);
	if (error != 0)
		return error;

	/* Reads the inputs into values before the entry point's first block. */
	error = shader_spirv_reader_entry_prologue(reader);
	if (error != 0)
		return error;

	/* The second pass: the entry point's blocks, calls inlined. */
	error = shader_spirv_reader_function(reader, reader->entry, &result);
	if (error != 0)
		return error;

	/* Hands the outputs over after the last block. */
	error = shader_spirv_reader_entry_epilogue(reader);
	if (error != 0)
		return error;

	/* Sizes the push constant request to the words the shader read. */
	if (reader->push_request < POWERVR_SHADER_REQUESTS)
		program->requests[reader->push_request].words = (uint8_t)reader->push_words;

	/* Packs the requests into shared registers in table order, each texture state from a multiple of four. */
	program->shareds = 0;
	for (index = 0; index < program->request_count; index++) {
		if (texture_state_request(program, index))
			program->shareds = (program->shareds + POWERVR_SHADER_STATE_WORDS - 1U) & ~(POWERVR_SHADER_STATE_WORDS - 1U);
		program->requests[index].shared = (uint16_t)program->shareds;
		program->shareds += program->requests[index].words;
	}

	/* Succeeded: the shader. */
	*shader = reader->shader;
	return 0;
}

/*
 * Tells whether a request starts a texture state the texture unit reads
 * from shared registers: an image's or a sampler's state words, or the
 * first of the point sampler's four literals.  The unit reads the four
 * words from the multiple of four at or below the register an operation
 * names (seen on the board: an image state from register 2 had its first
 * word taken as the address word), so each starts on one.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/imagination/vulkan/pvr_arch_pipeline.c
 * (pvr_setup_descriptors: the descriptors' shared registers and the
 * point and input attachment samplers' state words each from a multiple
 * of four).
 */
static int
texture_state_request(
	const struct powervr_shader_program *program,
	uint32_t index)
{
	const struct powervr_shader_request *request;
	uint32_t word;

	/* An image's or a sampler's state words. */
	request = &program->requests[index];
	if (request->kind == POWERVR_SHADER_REQUEST_DESCRIPTOR)
		return request->part == POWERVR_SHADER_PART_IMAGE_STATE || request->part == POWERVR_SHADER_PART_SAMPLER_STATE;

	/* The point sampler's four literals, one after another. */
	if (index + POWERVR_SHADER_STATE_WORDS > program->request_count)
		return 0;
	for (word = 0; word < POWERVR_SHADER_STATE_WORDS; word++) {
		request = &program->requests[index + word];
		if (request->kind != POWERVR_SHADER_REQUEST_LITERAL || request->words != 1 || request->literal != shader_spirv_point_sampler_words[word])
			return 0;
	}
	return 1;
}

/*
 * Reads every declaration of the module and notes where each function
 * and block starts.
 */
static int
reader_declarations(
	struct shader_spirv_reader *reader)
{
	uint32_t at;
	uint32_t length;
	uint32_t opcode;
	int error;

	/* Walks the instructions after the header. */
	at = SPIRV_HEADER_WORDS;
	while (at < reader->count) {
		/* Each instruction's first word holds its length and opcode. */
		length = reader->words[at] >> 16;
		opcode = reader->words[at] & 0xffffU;
		if (length == 0 || at + length > reader->count)
			return EINVAL;

		/* Reads the declaration, or notes the function or block. */
		error = reader_declaration(reader, &reader->words[at], length, at);
		if (error != 0) {
			reader->refused = at;
			return error;
		}

		(void)opcode;
		at += length;
	}

	/* Succeeded: the declarations are read. */
	return 0;
}

/* Reads one instruction of the first pass. */
static int
reader_declaration(
	struct shader_spirv_reader *reader,
	const uint32_t *words,
	uint32_t length,
	uint32_t at)
{
	struct shader_spirv_member_decoration *decoration;
	struct shader_spirv_id *id;
	uint32_t opcode;
	uint32_t model;
	int error;

	/* Dispatches on the opcode. */
	opcode = words[0] & 0xffffU;
	error = 0;
	switch (opcode) {
	case OP_EXT_INST_IMPORT:
		/* Only GLSL.std.450 is known; its name starts "GLSL". */
		if (length < 3 || words[1] >= reader->bound)
			return EINVAL;
		reader->ids[words[1]].kind = ID_EXTENSION;
		if (words[2] == 0x4c534c47U)
			reader->glsl = words[1];
		break;
	case OP_ENTRY_POINT:
		/* Keeps the entry point of the stage asked for. */
		if (length < 4)
			return EINVAL;
		model = words[1];
		if ((reader->stage == POWERVR_SHADER_COMPUTE && model == MODEL_GL_COMPUTE) ||
		    (reader->stage == POWERVR_SHADER_VERTEX && model == MODEL_VERTEX) ||
		    (reader->stage == POWERVR_SHADER_FRAGMENT && model == MODEL_FRAGMENT)) {
			if (reader->entry == 0)
				reader->entry = words[2];
		}
		break;
	case OP_EXECUTION_MODE:
		/* The workgroup's size. */
		if (length >= 6 && words[1] == reader->entry && words[2] == MODE_LOCAL_SIZE) {
			reader->program->workgroup[0] = words[3];
			reader->program->workgroup[1] = words[4];
			reader->program->workgroup[2] = words[5];
		}
		break;
	case OP_DECORATE:
		/* A decoration of an id, with its one value where it has one. */
		if (length < 3 || words[1] >= reader->bound)
			return EINVAL;
		reader_decorate(reader, words[1], words[2], length > 3 ? words[3] : 0);
		break;
	case OP_MEMBER_DECORATE:
		/* Kept until the struct is read. */
		if (length < 4)
			return EINVAL;
		if (reader->member_decoration_count >= SHADER_SPIRV_MEMBER_DECORATIONS)
			return ENOTSUP;
		decoration = &reader->member_decorations[reader->member_decoration_count++];
		decoration->structure = words[1];
		decoration->member = words[2];
		decoration->decoration = words[3];
		decoration->value = length > 4 ? words[4] : 0;
		break;
	case OP_TYPE_VOID:
	case OP_TYPE_BOOL:
	case OP_TYPE_INT:
	case OP_TYPE_FLOAT:
	case OP_TYPE_VECTOR:
	case OP_TYPE_MATRIX:
	case OP_TYPE_IMAGE:
	case OP_TYPE_SAMPLER:
	case OP_TYPE_SAMPLED_IMAGE:
	case OP_TYPE_ARRAY:
	case OP_TYPE_RUNTIME_ARRAY:
	case OP_TYPE_STRUCT:
	case OP_TYPE_POINTER:
	case OP_TYPE_FUNCTION:
		error = reader_type(reader, words, length);
		break;
	case OP_CONSTANT_TRUE:
	case OP_CONSTANT_FALSE:
	case OP_CONSTANT:
	case OP_CONSTANT_COMPOSITE:
	case OP_CONSTANT_NULL:
	case OP_SPEC_CONSTANT_TRUE:
	case OP_SPEC_CONSTANT_FALSE:
	case OP_SPEC_CONSTANT:
	case OP_SPEC_CONSTANT_COMPOSITE:
	case OP_UNDEF:
		/* A function-local undefined value is read in the second pass too, harmlessly. */
		error = shader_spirv_reader_constant(reader, words, length);
		break;
	case OP_VARIABLE:
		/* Global variables now; function variables when their block is walked. */
		if (length < 4)
			return EINVAL;
		if (words[3] != STORAGE_FUNCTION)
			error = reader_global_variable(reader, words, length);
		break;
	case OP_FUNCTION:
		/* Where the function starts. */
		if (length < 5 || words[2] >= reader->bound)
			return EINVAL;
		id = &reader->ids[words[2]];
		id->kind = ID_FUNCTION;
		id->word = at;
		break;
	case OP_LABEL:
		/* Where the block starts. */
		if (length < 2 || words[1] >= reader->bound)
			return EINVAL;
		id = &reader->ids[words[1]];
		id->kind = ID_LABEL;
		id->word = at;
		reader->current_label = words[1];
		break;
	case OP_SELECTION_MERGE:
		/* The current block's merge. */
		if (length < 3 || reader->current_label == 0)
			return EINVAL;
		reader->ids[reader->current_label].merge = words[1];
		break;
	case OP_LOOP_MERGE:
		/* The current block is a loop header with a merge and a continue target. */
		if (length < 4 || reader->current_label == 0)
			return EINVAL;
		reader->ids[reader->current_label].merge = words[1];
		reader->ids[reader->current_label].continue_target = words[2];
		reader->ids[reader->current_label].flags |= FLAG_LOOP;
		break;
	default:
		break;
	}

	/* Reports what the declaration came to. */
	return error;
}

/* Notes one decoration of an id. */
static void
reader_decorate(
	struct shader_spirv_reader *reader,
	uint32_t target,
	uint32_t decoration,
	uint32_t value)
{
	struct shader_spirv_id *id;

	/* Records the decorations the compiler uses; the others do not change the code. */
	id = &reader->ids[target];
	switch (decoration) {
	case DECORATION_ARRAY_STRIDE:
		id->stride = value;
		id->flags |= FLAG_STRIDE;
		break;
	case DECORATION_BUILT_IN:
		id->builtin = value;
		id->flags |= FLAG_BUILT_IN;
		break;
	case DECORATION_FLAT:
		id->flags |= FLAG_FLAT;
		break;
	case DECORATION_NO_PERSPECTIVE:
		id->linear = 1;
		break;
	case DECORATION_LOCATION:
		id->location = value;
		id->flags |= FLAG_LOCATION;
		break;
	case DECORATION_COMPONENT:
		id->component = value;
		break;
	case DECORATION_BINDING:
		id->binding = value;
		break;
	case DECORATION_DESCRIPTOR_SET:
		id->set = value;
		break;
	case DECORATION_NO_CONTRACTION:
		/* Some result must stay as written: no fused multiply-add in the shader. */
		reader->shader->exact = 1;
		break;
	default:
		break;
	}
}

/* Reads one type declaration. */
static int
reader_type(
	struct shader_spirv_reader *reader,
	const uint32_t *words,
	uint32_t length)
{
	struct shader_spirv_member_decoration *decoration;
	struct shader_spirv_member *member;
	struct shader_spirv_id *id;
	uint32_t opcode;
	uint32_t index;
	uint32_t scalars;
	uint32_t member_index;

	/* The type's id. */
	opcode = words[0] & 0xffffU;
	if (length < 2 || words[1] >= reader->bound)
		return EINVAL;
	id = &reader->ids[words[1]];
	id->kind = ID_TYPE;

	/* Fills in the shape of each kind of type. */
	switch (opcode) {
	case OP_TYPE_VOID:
		id->type_kind = TYPE_VOID;
		break;
	case OP_TYPE_BOOL:
		id->type_kind = TYPE_BOOL;
		id->scalars = 1;
		break;
	case OP_TYPE_INT:
		/* Only 32-bit integers. */
		if (length < 4 || words[2] != 32)
			return ENOTSUP;
		id->type_kind = TYPE_INT;
		id->is_signed = (uint8_t)words[3];
		id->scalars = 1;
		break;
	case OP_TYPE_FLOAT:
		/* Only 32-bit floats. */
		if (length < 3 || words[2] != 32)
			return ENOTSUP;
		id->type_kind = TYPE_FLOAT;
		id->scalars = 1;
		break;
	case OP_TYPE_VECTOR:
	case OP_TYPE_MATRIX:
		/* A vector of scalars, or a matrix of column vectors. */
		if (length < 4 || words[2] >= reader->bound)
			return EINVAL;
		id->type_kind = opcode == OP_TYPE_VECTOR ? TYPE_VECTOR : TYPE_MATRIX;
		id->type = words[2];
		id->count = words[3];
		id->scalars = reader->ids[words[2]].scalars * words[3];
		break;
	case OP_TYPE_IMAGE:
		/* Its sampled type, Dim and whether it is arrayed or multisampled (the image's shape a lookup needs). */
		if (length < 9 || words[2] >= reader->bound)
			return EINVAL;
		id->type_kind = TYPE_IMAGE;
		id->type = words[2];
		id->dimension = (uint8_t)(words[3] > 0xffU ? 0xffU : words[3]);
		id->arrayed = (uint8_t)(words[5] != 0);
		id->multisampled = (uint8_t)(words[6] != 0);
		break;
	case OP_TYPE_SAMPLER:
		id->type_kind = TYPE_SAMPLER;
		break;
	case OP_TYPE_SAMPLED_IMAGE:
		if (length < 3 || words[2] >= reader->bound)
			return EINVAL;
		id->type_kind = TYPE_SAMPLED_IMAGE;
		id->type = words[2];
		break;
	case OP_TYPE_ARRAY:
		/* An array's length is a constant id. */
		if (length < 4 || words[2] >= reader->bound || words[3] >= reader->bound)
			return EINVAL;
		if (reader->ids[words[3]].kind != ID_CONSTANT)
			return ENOTSUP;
		id->type_kind = TYPE_ARRAY;
		id->type = words[2];
		id->count = reader->operands[reader->ids[words[3]].first].constant;
		id->scalars = reader->ids[words[2]].scalars * id->count;
		break;
	case OP_TYPE_RUNTIME_ARRAY:
		if (length < 3 || words[2] >= reader->bound)
			return EINVAL;
		id->type_kind = TYPE_RUNTIME_ARRAY;
		id->type = words[2];
		break;
	case OP_TYPE_STRUCT:
		/* The members, with the member decorations kept so far. */
		id->type_kind = TYPE_STRUCT;
		id->count = length - 2;
		id->first = reader->member_count;
		if (reader->member_count + id->count > SHADER_SPIRV_MEMBERS)
			return ENOTSUP;
		scalars = 0;
		for (member_index = 0; member_index < id->count; member_index++) {
			member = &reader->members[reader->member_count++];
			member->type = words[2 + member_index];
			if (member->type >= reader->bound)
				return EINVAL;
			member->scalar_offset = scalars;
			scalars += reader->ids[member->type].scalars;
		}
		id->scalars = scalars;
		for (index = 0; index < reader->member_decoration_count; index++) {
			decoration = &reader->member_decorations[index];
			if (decoration->structure != words[1] || decoration->member >= id->count)
				continue;
			member = &reader->members[id->first + decoration->member];
			if (decoration->decoration == DECORATION_OFFSET) {
				member->offset = decoration->value;
			} else if (decoration->decoration == DECORATION_BUILT_IN) {
				member->builtin = decoration->value;
				member->has_builtin = 1;
			} else if (decoration->decoration == DECORATION_MATRIX_STRIDE) {
				reader->ids[member->type].stride = decoration->value;
			}
		}
		break;
	case OP_TYPE_POINTER:
		if (length < 4 || words[3] >= reader->bound)
			return EINVAL;
		id->type_kind = TYPE_POINTER;
		id->storage = words[2];
		id->type = words[3];
		break;
	case OP_TYPE_FUNCTION:
		id->type_kind = TYPE_FUNCTION;
		break;
	default:
		return ENOTSUP;
	}

	/* Succeeded: the type is known. */
	return 0;
}

/* Reads one constant (or undefined value) as its scalar words. */
int
shader_spirv_reader_constant(
	struct shader_spirv_reader *reader,
	const uint32_t *words,
	uint32_t length)
{
	struct powervr_ir_operand *operands;
	struct powervr_ir_operand *part;
	uint32_t opcode;
	uint32_t count;
	uint32_t parts;
	uint32_t index;
	uint32_t at;
	int error;

	/* The result's type and id, and its words. */
	opcode = words[0] & 0xffffU;
	if (length < 3 || words[1] >= reader->bound || words[2] >= reader->bound)
		return EINVAL;
	count = reader->ids[words[1]].scalars;
	if (count == 0 || count > COMPOSITE_MAX)
		return ENOTSUP;
	error = shader_spirv_result_begin(reader, words[2], words[1], count, &operands);
	if (error != 0)
		return error;
	reader->ids[words[2]].kind = ID_CONSTANT;

	/* Fills the words: each kind of constant its own way. */
	if (opcode == OP_CONSTANT || opcode == OP_SPEC_CONSTANT) {
		if (length < 4)
			return EINVAL;
		operands[0] = shader_spirv_constant_operand(words[3]);
	} else if (opcode == OP_CONSTANT_TRUE || opcode == OP_SPEC_CONSTANT_TRUE) {
		operands[0] = shader_spirv_constant_operand(TRUE_WORD);
	} else if (opcode == OP_CONSTANT_COMPOSITE || opcode == OP_SPEC_CONSTANT_COMPOSITE) {
		/* The constituents' words, in order. */
		at = 0;
		for (index = 3; index < length; index++) {
			if (words[index] >= reader->bound)
				return EINVAL;
			error = shader_spirv_id_operands(reader, words[index], &part, &parts);
			if (error != 0)
				return error;
			if (at + parts > count)
				return EINVAL;
			while (parts-- > 0)
				operands[at++] = *part++;
		}
		if (at != count)
			return EINVAL;
	} else {
		/* False, null and undefined are all zero words. */
		for (index = 0; index < count; index++)
			operands[index] = shader_spirv_constant_operand(0);
	}

	/* Succeeded: the constant. */
	return 0;
}

/* Reads one global variable declaration. */
static int
reader_global_variable(
	struct shader_spirv_reader *reader,
	const uint32_t *words,
	uint32_t length)
{
	struct shader_spirv_id *id;
	struct shader_spirv_id *pointer_type;
	uint32_t bytes;

	/* The variable is a pointer of its type into itself; a global's initializer is outside the subset. */
	if (length > 4)
		return ENOTSUP;
	if (words[1] >= reader->bound || words[2] >= reader->bound)
		return EINVAL;
	pointer_type = &reader->ids[words[1]];
	if (pointer_type->type_kind != TYPE_POINTER)
		return EINVAL;
	id = &reader->ids[words[2]];
	id->kind = ID_POINTER;
	id->storage = words[3];
	id->type = pointer_type->type;
	id->variable = words[2];
	id->offset = 0;

	/* Refuses storage the compiler does not place. */
	switch (id->storage) {
	case STORAGE_UNIFORM_CONSTANT:
	case STORAGE_INPUT:
	case STORAGE_UNIFORM:
	case STORAGE_OUTPUT:
	case STORAGE_PRIVATE:
	case STORAGE_PUSH_CONSTANT:
	case STORAGE_STORAGE_BUFFER:
		break;
	case STORAGE_WORKGROUP:
		/* Workgroup memory: the next bytes of the workgroup's slice, its words packed. */
		bytes = shader_spirv_byte_size(reader, id->type);
		if (bytes == 0 || bytes > SHADER_SHARED_BYTES_MAX - reader->program->shared_bytes)
			return ENOTSUP;
		id->offset = reader->program->shared_bytes;
		reader->program->shared_bytes += (bytes + 3U) & ~3U;
		break;
	default:
		return ENOTSUP;
	}

	/* Succeeded: the variable is known. */
	return 0;
}

/* Counts the 32-bit words a type flattens to. */
uint32_t
shader_spirv_scalar_count(
	struct shader_spirv_reader *reader,
	uint32_t type)
{
	/* An unknown id has no words. */
	if (type >= reader->bound)
		return 0;
	return reader->ids[type].scalars;
}

/* Works out a type's size in buffer memory, from its words and strides. */
uint32_t
shader_spirv_byte_size(
	struct shader_spirv_reader *reader,
	uint32_t type)
{
	struct shader_spirv_id *id;

	/* An array with a stride is its stride times its length; anything else four bytes a word. */
	if (type >= reader->bound)
		return 0;
	id = &reader->ids[type];
	if (id->type_kind == TYPE_ARRAY && id->stride != 0)
		return id->stride * id->count;
	return id->scalars * 4U;
}

/* Finds where a block's label is. */
int
shader_spirv_find_block(
	struct shader_spirv_reader *reader,
	uint32_t label,
	uint32_t *at)
{
	/* The first pass noted it. */
	if (label >= reader->bound || reader->ids[label].kind != ID_LABEL)
		return EINVAL;
	*at = reader->ids[label].word;
	return 0;
}

/* Makes a new value of a width, or notes that there is no room. */
uint32_t
shader_spirv_new_value(
	struct shader_spirv_reader *reader,
	uint32_t width)
{
	struct powervr_ir_shader *shader;
	uint32_t value;

	/* Refuses a shader with more values than the table holds. */
	shader = reader->shader;
	if (shader->value_count >= POWERVR_IR_VALUES) {
		if (reader->error == 0)
			reader->error = ENOTSUP;
		return 0;
	}

	/* The next value. */
	value = shader->value_count++;
	shader->values[value].width = (uint8_t)width;
	return value;
}

/* Appends one instruction, or notes that there is no room. */
void
shader_spirv_emit(
	struct shader_spirv_reader *reader,
	uint32_t opcode,
	uint32_t destination,
	const struct powervr_ir_operand *a,
	const struct powervr_ir_operand *b,
	const struct powervr_ir_operand *c,
	uint32_t immediate,
	uint32_t immediate_2)
{
	struct powervr_ir_instruction *instruction;
	struct powervr_ir_shader *shader;
	unsigned index;

	/* Refuses a shader with more instructions than the table holds. */
	shader = reader->shader;
	if (shader->instruction_count >= POWERVR_IR_INSTRUCTIONS) {
		if (reader->error == 0)
			reader->error = ENOTSUP;
		return;
	}

	/* Fills in the instruction. */
	instruction = &shader->instructions[shader->instruction_count++];
	instruction->opcode = (uint8_t)opcode;
	instruction->flags = 0;
	instruction->destination = (uint16_t)destination;
	for (index = 0; index < POWERVR_IR_SOURCES; index++)
		instruction->source[index].kind = POWERVR_IR_OPERAND_NONE;
	if (a != NULL)
		instruction->source[0] = *a;
	if (b != NULL)
		instruction->source[1] = *b;
	if (c != NULL)
		instruction->source[2] = *c;
	instruction->immediate = immediate;
	instruction->immediate_2 = immediate_2;
}

/* Appends one instruction writing a new one-word value, and reports an operand reading it. */
struct powervr_ir_operand
shader_spirv_emit_value(
	struct shader_spirv_reader *reader,
	uint32_t opcode,
	const struct powervr_ir_operand *a,
	const struct powervr_ir_operand *b,
	const struct powervr_ir_operand *c)
{
	struct powervr_ir_operand operand;
	uint32_t value;

	/* A new value and the instruction that writes it. */
	value = shader_spirv_new_value(reader, 1);
	shader_spirv_emit(reader, opcode, value, a, b, c, 0, 0);
	operand = shader_spirv_value_operand(value, 0);

	/* Succeeded: the new value as an operand. */
	return operand;
}

/* Makes an operand of a constant word. */
struct powervr_ir_operand
shader_spirv_constant_operand(
	uint32_t bits)
{
	struct powervr_ir_operand operand;

	/* A constant with no modifiers. */
	operand.kind = POWERVR_IR_OPERAND_CONSTANT;
	operand.modifiers = 0;
	operand.component = 0;
	operand.value = 0;
	operand.constant = bits;
	return operand;
}

/* Makes an operand reading one word of a value. */
struct powervr_ir_operand
shader_spirv_value_operand(
	uint32_t value,
	uint32_t component)
{
	struct powervr_ir_operand operand;

	/* A value's word with no modifiers. */
	operand.kind = POWERVR_IR_OPERAND_VALUE;
	operand.modifiers = 0;
	operand.component = (uint8_t)component;
	operand.value = (uint16_t)value;
	operand.constant = 0;
	return operand;
}

/*
 * Makes an operand without float modifiers, for an integer or bit
 * operation: a constant has them applied, a value is copied through a
 * move that applies them.
 */
struct powervr_ir_operand
shader_spirv_plain(
	struct shader_spirv_reader *reader,
	struct powervr_ir_operand operand)
{
	struct powervr_ir_operand result;

	/* An operand without modifiers is already plain. */
	if (operand.modifiers == 0)
		return operand;

	/* A constant takes the modifiers into its bits. */
	if (operand.kind == POWERVR_IR_OPERAND_CONSTANT) {
		if ((operand.modifiers & POWERVR_IR_ABSOLUTE) != 0)
			operand.constant &= ~FLOAT_SIGN;
		if ((operand.modifiers & POWERVR_IR_NEGATE) != 0)
			operand.constant ^= FLOAT_SIGN;
		operand.modifiers = 0;
		return operand;
	}

	/* A value goes through a move. */
	result = shader_spirv_emit_value(reader, POWERVR_IR_MOVE, &operand, NULL, NULL);
	return result;
}

/* Negates an operand as a float. */
struct powervr_ir_operand
shader_spirv_float_negate(
	struct powervr_ir_operand operand)
{
	/* A constant flips its sign; a value toggles its negate modifier. */
	if (operand.kind == POWERVR_IR_OPERAND_CONSTANT) {
		operand.constant ^= FLOAT_SIGN;
		return operand;
	}
	operand.modifiers ^= POWERVR_IR_NEGATE;
	return operand;
}

/* Takes the absolute value of an operand as a float. */
struct powervr_ir_operand
shader_spirv_float_absolute(
	struct powervr_ir_operand operand)
{
	/* A constant clears its sign; a value takes the absolute modifier, dropping any negate. */
	if (operand.kind == POWERVR_IR_OPERAND_CONSTANT) {
		operand.constant &= ~FLOAT_SIGN;
		return operand;
	}
	operand.modifiers = POWERVR_IR_ABSOLUTE;
	return operand;
}

/*
 * Divides two integer words as kind says (DIVIDE_*), the core having no
 * integer divide: by a power of two as a shift or a mask, else by the
 * reciprocal of the float denominator refined in integers.  A zero
 * denominator gives an undefined word, as SPIR-V allows.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/compiler/nir/nir_lower_idiv.c (emit_udiv and
 * emit_idiv, ported there from LLVM's AMDGPUTargetLowering), which
 * Mesa's PowerVR compiler runs (src/imagination/pco/pco_nir.c).
 */
struct powervr_ir_operand
shader_spirv_divide(
	struct shader_spirv_reader *reader,
	struct powervr_ir_operand numerator,
	struct powervr_ir_operand denominator,
	uint32_t kind)
{
	struct powervr_ir_operand numerator_sign;
	struct powervr_ir_operand denominator_sign;
	struct powervr_ir_operand numerator_size;
	struct powervr_ir_operand denominator_size;
	struct powervr_ir_operand result;
	struct powervr_ir_operand negated;
	struct powervr_ir_operand differ;
	struct powervr_ir_operand zero;
	struct powervr_ir_operand all_ones;
	struct powervr_ir_operand keep;
	struct powervr_ir_operand added;
	uint32_t divisor;
	uint32_t shift;

	zero = shader_spirv_constant_operand(0);
	all_ones = shader_spirv_constant_operand(TRUE_WORD);

	/* An unsigned division by a constant power of two: a shift or a mask. */
	if ((kind == DIVIDE_UNSIGNED || kind == DIVIDE_UNSIGNED_REMAINDER) && denominator.kind == POWERVR_IR_OPERAND_CONSTANT) {
		divisor = denominator.constant;
		if (divisor != 0 && (divisor & (divisor - 1U)) == 0) {
			if (kind == DIVIDE_UNSIGNED_REMAINDER) {
				keep = shader_spirv_constant_operand(divisor - 1U);
				result = shader_spirv_emit_value(reader, POWERVR_IR_BIT_AND, &numerator, &keep, NULL);
				return result;
			}
			shift = 0;
			while ((1U << shift) != divisor)
				shift++;
			keep = shader_spirv_constant_operand(shift);
			result = shader_spirv_emit_value(reader, POWERVR_IR_SHIFT_RIGHT, &numerator, &keep, NULL);
			return result;
		}
	}

	/* The unsigned forms as they are. */
	if (kind == DIVIDE_UNSIGNED || kind == DIVIDE_UNSIGNED_REMAINDER) {
		result = divide_unsigned(reader, numerator, denominator, kind == DIVIDE_UNSIGNED_REMAINDER);
		return result;
	}

	/* The signed forms divide the sizes, then give the result its sign. */
	numerator_size = integer_absolute(reader, numerator, &numerator_sign);
	denominator_size = integer_absolute(reader, denominator, &denominator_sign);
	if (kind == DIVIDE_SIGNED) {
		/* A quotient is negative when the signs differ. */
		differ = shader_spirv_emit_value(reader, POWERVR_IR_BIT_XOR, &numerator_sign, &denominator_sign, NULL);
		result = divide_unsigned(reader, numerator_size, denominator_size, 0);
		negated = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_MULTIPLY, &result, &all_ones, NULL);
		result = shader_spirv_emit_value(reader, POWERVR_IR_SELECT, &differ, &negated, &result);
		return result;
	}

	/* A remainder takes the numerator's sign. */
	result = divide_unsigned(reader, numerator_size, denominator_size, 1);
	negated = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_MULTIPLY, &result, &all_ones, NULL);
	result = shader_spirv_emit_value(reader, POWERVR_IR_SELECT, &numerator_sign, &negated, &result);
	if (kind == DIVIDE_SIGNED_REMAINDER)
		return result;

	/* A modulo of different signs and not zero moves by the denominator to its sign. */
	differ = shader_spirv_emit_value(reader, POWERVR_IR_BIT_XOR, &numerator_sign, &denominator_sign, NULL);
	keep = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_NOT_EQUAL, &result, &zero, NULL);
	differ = shader_spirv_emit_value(reader, POWERVR_IR_BIT_AND, &differ, &keep, NULL);
	added = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_ADD, &result, &denominator, NULL);
	result = shader_spirv_emit_value(reader, POWERVR_IR_SELECT, &differ, &added, &result);
	return result;
}

/* Reads one word of a shared register request into a value. */
struct powervr_ir_operand
shader_spirv_shared_word(
	struct shader_spirv_reader *reader,
	uint32_t request,
	uint32_t word)
{
	struct powervr_ir_operand operand;
	uint32_t value;

	/* The word as a new value, read where it is used. */
	value = shader_spirv_new_value(reader, 1);
	shader_spirv_emit(reader, POWERVR_IR_SHARED, value, NULL, NULL, NULL, request, word);
	operand = shader_spirv_value_operand(value, 0);

	/* Succeeded: the shared word as an operand. */
	return operand;
}

/*
 * Finds or adds a shared register request.  A request for the same
 * thing again is the same request.
 */
int
shader_spirv_request_add(
	struct shader_spirv_reader *reader,
	uint32_t kind,
	uint32_t part,
	uint32_t set,
	uint32_t binding,
	uint32_t words,
	uint32_t *index)
{
	int error;

	/* The binding's first element. */
	error = shader_spirv_request_element(reader, kind, part, set, binding, 0, words, index);
	return error;
}

/*
 * Finds or adds a shared register request for one element of a
 * binding's array of descriptors.
 */
int
shader_spirv_request_element(
	struct shader_spirv_reader *reader,
	uint32_t kind,
	uint32_t part,
	uint32_t set,
	uint32_t binding,
	uint32_t element,
	uint32_t words,
	uint32_t *index)
{
	struct powervr_shader_program *program;
	struct powervr_shader_request *request;
	uint32_t at;

	/* An existing request for the same thing. */
	program = reader->program;
	for (at = 0; at < program->request_count; at++) {
		request = &program->requests[at];
		if (request->kind == kind &&
		    request->part == part &&
		    request->set == set &&
		    request->binding == binding &&
		    request->array_index == element) {
			*index = at;
			return 0;
		}
	}

	/* A new one, if the table has room. */
	if (program->request_count >= POWERVR_SHADER_REQUESTS)
		return ENOTSUP;
	request = &program->requests[program->request_count];
	request->kind = (uint8_t)kind;
	request->part = (uint8_t)part;
	request->words = (uint8_t)words;
	request->set = set;
	request->binding = binding;
	request->array_index = element;
	request->offset = 0;
	request->literal = 0;
	*index = program->request_count++;
	return 0;
}

/* Gives a result id room for its words in the operand pool. */
int
shader_spirv_result_begin(
	struct shader_spirv_reader *reader,
	uint32_t result,
	uint32_t type,
	uint32_t count,
	struct powervr_ir_operand **operands)
{
	struct shader_spirv_id *id;

	/* Refuses an id out of range or a pool without room. */
	if (result >= reader->bound)
		return EINVAL;
	if (reader->operand_count + count > SHADER_SPIRV_OPERANDS)
		return ENOTSUP;

	/* The result's words, at the pool's end. */
	id = &reader->ids[result];
	id->kind = ID_VALUE;
	id->type = type;
	id->first = reader->operand_count;
	id->count = count;
	reader->operand_count += count;
	*operands = &reader->operands[id->first];
	return 0;
}

/* Finds an id's words. */
int
shader_spirv_id_operands(
	struct shader_spirv_reader *reader,
	uint32_t id,
	struct powervr_ir_operand **operands,
	uint32_t *count)
{
	struct shader_spirv_id *entry;

	/* Only constants and computed results have words. */
	if (id >= reader->bound)
		return EINVAL;
	entry = &reader->ids[id];
	if (entry->kind != ID_VALUE && entry->kind != ID_CONSTANT)
		return EINVAL;
	*operands = &reader->operands[entry->first];
	*count = entry->count;
	return 0;
}

/* Finds one word of an id. */
int
shader_spirv_operand_word(
	struct shader_spirv_reader *reader,
	uint32_t id,
	uint32_t component,
	struct powervr_ir_operand *operand)
{
	struct powervr_ir_operand *operands;
	uint32_t count;
	int error;

	/* The id's words, and the one asked for. */
	error = shader_spirv_id_operands(reader, id, &operands, &count);
	if (error != 0)
		return error;
	if (component >= count)
		return EINVAL;
	*operand = operands[component];
	return 0;
}

/* Works out the first word of a composite's part named by literal indices. */
int
shader_spirv_composite_offset(
	struct shader_spirv_reader *reader,
	uint32_t type,
	const uint32_t *indices,
	uint32_t count,
	uint32_t *offset,
	uint32_t *member_type)
{
	struct shader_spirv_member *member;
	struct shader_spirv_id *id;
	uint32_t at;

	/* Steps down through each index. */
	*offset = 0;
	for (at = 0; at < count; at++) {
		if (type >= reader->bound)
			return EINVAL;
		id = &reader->ids[type];
		if (id->type_kind == TYPE_STRUCT) {
			if (indices[at] >= id->count)
				return EINVAL;
			member = &reader->members[id->first + indices[at]];
			*offset += member->scalar_offset;
			type = member->type;
		} else if (id->type_kind == TYPE_VECTOR || id->type_kind == TYPE_MATRIX || id->type_kind == TYPE_ARRAY) {
			if (indices[at] >= id->count)
				return EINVAL;
			*offset += indices[at] * shader_spirv_scalar_count(reader, id->type);
			type = id->type;
		} else {
			return EINVAL;
		}
	}

	/* Succeeded: the part's first word and its type. */
	*member_type = type;
	return 0;
}

/*
 * Divides two unsigned words: an estimate of the reciprocal from the
 * float one, sharpened by one Newton step in integers, gives a quotient
 * at most two short, which two corrections settle (shader_spirv_divide).
 */
static struct powervr_ir_operand
divide_unsigned(
	struct shader_spirv_reader *reader,
	struct powervr_ir_operand numerator,
	struct powervr_ir_operand denominator,
	uint32_t remainder)
{
	struct powervr_ir_operand reciprocal;
	struct powervr_ir_operand negated;
	struct powervr_ir_operand error_word;
	struct powervr_ir_operand quotient;
	struct powervr_ir_operand rest;
	struct powervr_ir_operand over;
	struct powervr_ir_operand stepped;
	struct powervr_ir_operand scale;
	struct powervr_ir_operand one;
	struct powervr_ir_operand all_ones;

	scale = shader_spirv_constant_operand(0x4f7ffffeU);
	one = shader_spirv_constant_operand(1);
	all_ones = shader_spirv_constant_operand(TRUE_WORD);

	/* The reciprocal as a 0.32 fixed-point word, from the float one scaled by 2^32 less 512. */
	reciprocal = shader_spirv_emit_value(reader, POWERVR_IR_UNSIGNED_TO_FLOAT, &denominator, NULL, NULL);
	reciprocal = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_RECIPROCAL, &reciprocal, NULL, NULL);
	reciprocal = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY, &reciprocal, &scale, NULL);
	reciprocal = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_TO_UNSIGNED, &reciprocal, NULL, NULL);

	/* One Newton step: the reciprocal plus its error times itself. */
	negated = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_MULTIPLY, &denominator, &all_ones, NULL);
	error_word = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_MULTIPLY, &reciprocal, &negated, NULL);
	error_word = shader_spirv_emit_value(reader, POWERVR_IR_UNSIGNED_MULTIPLY_HIGH, &reciprocal, &error_word, NULL);
	reciprocal = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_ADD, &reciprocal, &error_word, NULL);

	/* The quotient's estimate and what is left of the numerator. */
	quotient = shader_spirv_emit_value(reader, POWERVR_IR_UNSIGNED_MULTIPLY_HIGH, &numerator, &reciprocal, NULL);
	rest = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_MULTIPLY, &quotient, &negated, NULL);
	rest = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_ADD, &numerator, &rest, NULL);

	/* The first correction: one more when the rest still holds the denominator. */
	over = shader_spirv_emit_value(reader, POWERVR_IR_UNSIGNED_GREATER_EQUAL, &rest, &denominator, NULL);
	if (remainder == 0) {
		stepped = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_ADD, &quotient, &one, NULL);
		quotient = shader_spirv_emit_value(reader, POWERVR_IR_SELECT, &over, &stepped, &quotient);
	}
	stepped = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_ADD, &rest, &negated, NULL);
	rest = shader_spirv_emit_value(reader, POWERVR_IR_SELECT, &over, &stepped, &rest);

	/* The second correction, of the remainder or of the quotient. */
	over = shader_spirv_emit_value(reader, POWERVR_IR_UNSIGNED_GREATER_EQUAL, &rest, &denominator, NULL);
	if (remainder != 0) {
		stepped = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_ADD, &rest, &negated, NULL);
		rest = shader_spirv_emit_value(reader, POWERVR_IR_SELECT, &over, &stepped, &rest);
		return rest;
	}
	stepped = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_ADD, &quotient, &one, NULL);
	quotient = shader_spirv_emit_value(reader, POWERVR_IR_SELECT, &over, &stepped, &quotient);
	return quotient;
}

/* Takes an integer word's size, noting in negative whether it was below zero (all ones) or not (zero). */
static struct powervr_ir_operand
integer_absolute(
	struct shader_spirv_reader *reader,
	struct powervr_ir_operand word,
	struct powervr_ir_operand *negative)
{
	struct powervr_ir_operand negated;
	struct powervr_ir_operand zero;
	struct powervr_ir_operand all_ones;
	struct powervr_ir_operand size;

	zero = shader_spirv_constant_operand(0);
	all_ones = shader_spirv_constant_operand(TRUE_WORD);

	/* The sign, then the word or its negation. */
	*negative = shader_spirv_emit_value(reader, POWERVR_IR_SIGNED_LESS, &word, &zero, NULL);
	negated = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_MULTIPLY, &word, &all_ones, NULL);
	size = shader_spirv_emit_value(reader, POWERVR_IR_SELECT, negative, &negated, &word);
	return size;
}
