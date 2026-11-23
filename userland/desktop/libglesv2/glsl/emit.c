/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GLSL compiler's emitter: one checked stage becomes a SPIR-V module
 * with main as its only function.
 *
 * Every variable is a Function variable of main that is stored before it
 * is read (a declaration without an initializer stores zero), every user
 * function is inlined where it is called, and the control flow is
 * structured: if is a selection, loops are loops with a header, a
 * condition block, a body, a continue block and a merge block, and a
 * switch or a function with early returns is a loop that runs once and
 * is left by branches to its merge block.  Values are read and written
 * through paths (a variable, an access chain, a swizzle), so a store to
 * a component is an access chain to it and a store to a swizzle is a
 * load, a shuffle and a store.
 */

#include "emit.h"

#include <string.h>

/* The most components a flattened value has (a 4x4 matrix), and the most elements an aggregate has. */
#define EMIT_MAX_SCALARS	16U
#define EMIT_MAX_PARTS		64U

/* GLSL.std.450's FrexpStruct: a value's significand and exponent as a struct of two members. */
#define EMIT_FREXP_STRUCT	52U

static unsigned emit_type_slot(struct emit_state *state, const struct glsl_type *type);
static uint32_t emit_layout_type(struct emit_state *state, const struct glsl_type *type);
static void emit_layout_members(struct emit_state *state, uint32_t structure, unsigned member, const struct glsl_type *type, uint32_t offset, const char *name, unsigned row_major);
static uint32_t emit_layout_row_major_array(struct emit_state *state, const struct glsl_type *type);
static void emit_uniform_block(struct emit_state *state, struct glsl_symbol *symbol);
static void emit_directions(struct emit_state *state);
static uint32_t emit_zed_load(struct emit_state *state);
static uint32_t emit_turned(struct emit_state *state, struct glsl_symbol *symbol, uint32_t zed, unsigned component, unsigned scale);
static uint32_t emit_constant(struct emit_state *state, const struct glsl_constant *constant);
static uint32_t emit_constant_part(struct emit_state *state, const struct glsl_type *type, const union glsl_scalar *values, unsigned *at);
static uint32_t emit_zero(struct emit_state *state, const struct glsl_type *type);
static uint32_t emit_variable(struct emit_state *state, const struct glsl_type *type);
static void emit_globals(struct emit_state *state);
static void emit_block(struct emit_state *state);
static void emit_sampler(struct emit_state *state, struct glsl_symbol *symbol);
static void emit_image(struct emit_state *state, struct glsl_symbol *symbol);
static uint32_t emit_storage_image_type(struct emit_state *state, const struct glsl_type *type);
static struct emit_value emit_image_call(struct emit_state *state, struct glsl_node *node);
static struct emit_value emit_image_access(struct emit_state *state, struct glsl_node *node, uint32_t variable, uint32_t image_type, const struct emit_value *arguments, unsigned count);
static uint32_t emit_image_valid(struct emit_state *state, const struct glsl_node *node, const struct glsl_type *type, uint32_t variable, uint32_t image_type, uint32_t coordinate);
static unsigned emit_image_unit(const struct glsl_node *node);
static struct glsl_symbol *emit_image_root(const struct glsl_node *node);
static void emit_interface(struct emit_state *state, struct glsl_symbol *symbol);
static void emit_interface_decorations(struct emit_state *state, struct glsl_symbol *symbol, uint32_t variable);
static void emit_block_members(struct emit_state *state, const struct glsl_type *type);
static void emit_per_vertex(struct emit_state *state, const struct glsl_type *type);
static void emit_geometry_modes(struct emit_state *state, uint32_t main_id);
static void emit_tess_modes(struct emit_state *state, uint32_t main_id);
static void emit_label(struct emit_state *state, uint32_t label);
static void emit_branch(struct emit_state *state, uint32_t target);
static void emit_conditional(struct emit_state *state, uint32_t condition, uint32_t if_true, uint32_t if_false);
static void emit_selection_merge(struct emit_state *state, uint32_t merge);
static void emit_loop_merge(struct emit_state *state, uint32_t merge, uint32_t continue_label);
static void emit_statements(struct emit_state *state, struct glsl_node *first);
static void emit_statement(struct emit_state *state, struct glsl_node *node);
static void emit_declaration(struct emit_state *state, struct glsl_node *node);
static void emit_if(struct emit_state *state, struct glsl_node *node);
static void emit_loop(struct emit_state *state, struct glsl_node *node);
static void emit_switch(struct emit_state *state, struct glsl_node *node);
static uint32_t emit_case_condition(struct emit_state *state, struct glsl_node *label, uint32_t selector, uint32_t fallen, uint32_t matched_any);
static void emit_return(struct emit_state *state, struct glsl_node *node);
static void emit_capture_buffer(struct emit_state *state);
static void emit_capture(struct emit_state *state);
static void emit_capture_value(struct emit_state *state, uint32_t value, const struct glsl_type *type, uint32_t base, unsigned *offset);
static void emit_capture_start(struct emit_state *state);
static uint32_t emit_capture_word(struct emit_state *state, uint32_t index);
static uint32_t emit_capture_add(struct emit_state *state, uint32_t index, uint32_t value);
static void emit_capture_store(struct emit_state *state, uint32_t index, uint32_t value);
static uint32_t emit_capture_number(struct emit_state *state, const struct glsl_symbol *symbol);
static void emit_capture_set(struct emit_state *state, uint32_t variable, uint32_t value);
static void emit_push_target(struct emit_state *state, unsigned kind, uint32_t merge, uint32_t continue_label);
static void emit_pop_target(struct emit_state *state);
static void emit_returned_check(struct emit_state *state);
static int emit_path(struct emit_state *state, struct glsl_node *node, struct emit_path *path);
static uint32_t emit_chain(struct emit_state *state, const struct emit_path *path);
static struct emit_value emit_load(struct emit_state *state, const struct emit_path *path);
static struct emit_value emit_load_block(struct emit_state *state, const struct emit_path *path);
static void emit_store(struct emit_state *state, const struct emit_path *path, struct emit_value value);
static struct emit_value emit_swizzle(struct emit_state *state, struct emit_value value, const unsigned *swizzle, unsigned count);
static struct emit_value emit_selection(struct emit_state *state, struct glsl_node *node);
static struct emit_value emit_unary(struct emit_state *state, struct glsl_node *node);
static struct emit_value emit_increment(struct emit_state *state, struct glsl_node *node);
static struct emit_value emit_binary(struct emit_state *state, struct glsl_node *node);
static struct emit_value emit_arithmetic(struct emit_state *state, unsigned op, struct emit_value left, struct emit_value right, const struct glsl_type *type);
static uint32_t emit_arithmetic_opcode(unsigned op, unsigned base);
static struct emit_value emit_columns(struct emit_state *state, uint32_t opcode, struct emit_value left, struct emit_value right, const struct glsl_type *type);
static uint32_t emit_compare(struct emit_state *state, unsigned op, struct emit_value left, struct emit_value right);
static uint32_t emit_equal(struct emit_state *state, struct emit_value left, struct emit_value right);
static struct emit_value emit_logical(struct emit_state *state, struct glsl_node *node);
static struct emit_value emit_assign(struct emit_state *state, struct glsl_node *node);
static unsigned emit_assign_operator(unsigned op);
static struct emit_value emit_ternary(struct emit_state *state, struct glsl_node *node);
static int emit_is_cheap(const struct glsl_node *node);
static struct emit_value emit_call(struct emit_state *state, struct glsl_node *node);
static struct emit_value emit_constructor(struct emit_state *state, struct glsl_node *node);
static unsigned emit_scalars(struct emit_state *state, struct emit_value value, uint32_t *ids, unsigned capacity);
static struct emit_value emit_matrix_resize(struct emit_state *state, struct emit_value value, const struct glsl_type *type);
static struct emit_value emit_inline(struct emit_state *state, struct glsl_node *node);
static struct emit_value emit_subroutine_call(struct emit_state *state, struct glsl_node *node);
static struct emit_value emit_value_of(uint32_t id, const struct glsl_type *type);
static uint32_t emit_layout430_type(struct emit_state *state, const struct glsl_type *type);
static void emit_storage_block(struct emit_state *state, struct glsl_symbol *symbol);
static void emit_shared(struct emit_state *state, struct glsl_symbol *symbol);
static void emit_store_block(struct emit_state *state, const struct emit_path *path, struct emit_value value);
static struct emit_value emit_array_length(struct emit_state *state, struct glsl_node *node);
static struct emit_value emit_compute_call(struct emit_state *state, struct glsl_node *node);
static struct emit_value emit_out_call(struct emit_state *state, struct glsl_node *node);
static const struct glsl_type *emit_pair_type(struct emit_state *state, const struct glsl_type *first, const struct glsl_type *second);
static uint32_t emit_uint(struct emit_state *state, uint32_t value);
static int emit_returns_in_loop(const struct glsl_node *node, int in_loop);
static void emit_main_once(struct emit_state *state, struct glsl_node *body);
static int emit_contractible(uint32_t opcode);
static struct emit_value emit_interpolate(struct emit_state *state, struct glsl_node *node);
static struct emit_value emit_derivative_y(struct emit_state *state, struct glsl_node *node);

/*
 * Emits one linked stage of a shader as a SPIR-V module (in the arena),
 * with the program's uniforms laid out as the link says.  Returns the
 * words and their count.
 */
uint32_t *
glsl_emit(
	struct glsl_shader *shader,
	struct glsl_arena *arena,
	struct glsl_link_uniform *uniforms,
	unsigned uniform_count,
	const struct glsl_link_capture *captures,
	unsigned capture_count,
	unsigned capture_stride,
	size_t *words)
{
	struct emit_state *state;
	struct glsl_module *module;
	uint32_t operands[GLSL_MAX_OPERANDS];
	uint32_t void_type;
	uint32_t function_type;
	uint32_t main_id;
	uint32_t *code;
	unsigned index;

	/* The state and the module. */
	state = glsl_alloc(arena, sizeof(*state));
	module = glsl_alloc(arena, sizeof(*module));
	glsl_module_init(module, arena);
	state->shader = shader;
	state->module = module;
	state->uniforms = uniforms;
	state->uniform_count = uniform_count;
	state->members = glsl_alloc(arena, (uniform_count + 1U) * sizeof(*state->members));
	for (index = 0U; index < uniform_count; index++)
		state->members[index] = -1;

	/*
	 * The outputs a vertex shader captures (transform feedback); a
	 * geometry shader that captures, or emits on a stream other than 0,
	 * counts what it emits in a capture buffer too (glsl.h).
	 */
	state->captures = captures;
	state->capture_count = capture_count;
	state->capture_stride = capture_stride;
	state->capture_geometry = glsl_capture_counted(shader, capture_count);

	/* main's type and its first instructions. */
	void_type = glsl_emit_type(state, glsl_type_void());
	operands[0] = 0U;
	operands[1] = void_type;
	function_type = glsl_module_declare(module, SPV_OP_TYPE_FUNCTION, operands, 2U, 0);
	main_id = glsl_module_id(module);
	operands[0] = void_type;
	operands[1] = main_id;
	operands[2] = 0U;
	operands[3] = function_type;
	glsl_words_add(module, &module->body, SPV_OP_FUNCTION, operands, 4U);
	emit_label(state, glsl_module_id(module));

	/* The globals the code uses (with the buffer captured outputs go into), then main's body. */
	emit_globals(state);
	if (state->capture_count != 0U || state->capture_geometry)
		emit_capture_buffer(state);
	if (shader->stage == GLSL_STAGE_COMPUTE && emit_returns_in_loop(shader->main->body->child[0], 0)) {
		emit_main_once(state, shader->main->body->child[0]);
	} else {
		emit_statements(state, shader->main->body->child[0]);
	}

	/* main ends with a return when its last block is still open, having captured its outputs. */
	if (!state->terminated) {
		emit_capture(state);
		glsl_words_add(module, &module->body, SPV_OP_RETURN, NULL, 0U);
	}

	/* The function ends. */
	glsl_words_add(module, &module->body, SPV_OP_FUNCTION_END, NULL, 0U);

	/* The entry point with its interface. */
	operands[0] = SPV_MODEL_VERTEX;
	if (shader->stage == GLSL_STAGE_FRAGMENT)
		operands[0] = SPV_MODEL_FRAGMENT;
	if (shader->stage == GLSL_STAGE_GEOMETRY)
		operands[0] = SPV_MODEL_GEOMETRY;
	if (shader->stage == GLSL_STAGE_COMPUTE)
		operands[0] = SPV_MODEL_GL_COMPUTE;
	if (shader->stage == GLSL_STAGE_TESS_CONTROL)
		operands[0] = SPV_MODEL_TESS_CONTROL;
	if (shader->stage == GLSL_STAGE_TESS_EVALUATION)
		operands[0] = SPV_MODEL_TESS_EVALUATION;
	operands[1] = main_id;
	operands[2] = 0x6e69616dU;
	operands[3] = 0U;
	memcpy(operands + 4, state->interface, state->interface_count * sizeof(uint32_t));
	glsl_words_add(module, &module->entry_points, SPV_OP_ENTRY_POINT, operands, 4U + state->interface_count);

	/* A geometry shader's capability, primitives, most vertices and one invocation. */
	if (shader->stage == GLSL_STAGE_GEOMETRY)
		emit_geometry_modes(state, main_id);

	/* A tessellation shader's capability and its patch's modes. */
	if (shader->stage == GLSL_STAGE_TESS_CONTROL || shader->stage == GLSL_STAGE_TESS_EVALUATION)
		emit_tess_modes(state, main_id);

	/* A compute shader's workgroup size (ws101-p008). */
	if (shader->stage == GLSL_STAGE_COMPUTE) {
		operands[0] = main_id;
		operands[1] = SPV_MODE_LOCAL_SIZE;
		operands[2] = shader->local_size[0];
		operands[3] = shader->local_size[1];
		operands[4] = shader->local_size[2];
		glsl_words_add(module, &module->modes, SPV_OP_EXECUTION_MODE, operands, 5U);
	}

	/* A fragment shader's origin, and depth replacing when it writes gl_FragDepth. */
	if (shader->stage == GLSL_STAGE_FRAGMENT) {
		operands[0] = main_id;
		operands[1] = SPV_MODE_ORIGIN_UPPER_LEFT;
		glsl_words_add(module, &module->modes, SPV_OP_EXECUTION_MODE, operands, 2U);
		if (state->depth_written) {
			operands[1] = SPV_MODE_DEPTH_REPLACING;
			glsl_words_add(module, &module->modes, SPV_OP_EXECUTION_MODE, operands, 2U);
		}
	}

	/* Succeeded: the module's words. */
	code = glsl_module_finish(module, words);
	return code;
}

/*
 * Returns the SPIR-V type of a GLSL type (as a local variable holds it).
 */
uint32_t
glsl_emit_type(
	struct emit_state *state,
	const struct glsl_type *type)
{
	uint32_t operands[GLSL_MAX_OPERANDS];
	uint32_t id;
	unsigned slot;
	unsigned index;

	/* A type given an id before. */
	slot = emit_type_slot(state, type);
	if (state->type_ids[slot] != 0U)
		return state->type_ids[slot];

	/* The kinds of types. */
	operands[0] = 0U;
	switch (type->kind) {
	case GLSL_KIND_VOID:
		id = glsl_module_declare(state->module, SPV_OP_TYPE_VOID, operands, 1U, 0);
		break;
	case GLSL_KIND_SCALAR:
		if (type->base == GLSL_BASE_BOOL) {
			id = glsl_module_declare(state->module, SPV_OP_TYPE_BOOL, operands, 1U, 0);
		} else if (type->base == GLSL_BASE_FLOAT) {
			operands[1] = 32U;
			id = glsl_module_declare(state->module, SPV_OP_TYPE_FLOAT, operands, 2U, 0);
		} else if (type->base == GLSL_BASE_DOUBLE) {
			/* A double needs the Float64 capability (and the device's shaderFloat64). */
			glsl_emit_capability(state, SPV_CAPABILITY_FLOAT64, &state->module->float64);
			operands[1] = 64U;
			id = glsl_module_declare(state->module, SPV_OP_TYPE_FLOAT, operands, 2U, 0);
		} else {
			operands[1] = 32U;
			operands[2] = 0U;
			if (type->base == GLSL_BASE_INT)
				operands[2] = 1U;
			id = glsl_module_declare(state->module, SPV_OP_TYPE_INT, operands, 3U, 0);
		}

		/* The scalar type is declared. */
		break;
	case GLSL_KIND_VECTOR:
		operands[1] = glsl_emit_type(state, glsl_type_scalar(type->base));
		operands[2] = type->components;
		id = glsl_module_declare(state->module, SPV_OP_TYPE_VECTOR, operands, 3U, 0);
		break;
	case GLSL_KIND_MATRIX:
		operands[1] = glsl_emit_type(state, glsl_type_column(type));
		operands[2] = type->columns;
		id = glsl_module_declare(state->module, SPV_OP_TYPE_MATRIX, operands, 3U, 0);
		break;
	case GLSL_KIND_SAMPLER:
		/* An image is a storage image of its format. */
		if (type->image) {
			id = emit_storage_image_type(state, type);
			break;
		}

		/* A buffer sampler is the image itself (a texel buffer); the others are sampled images. */
		if (type->sampler == GLSL_SAMPLER_BUFFER) {
			id = glsl_emit_image_type(state, type);
			break;
		}

		/* The sampled image of the image type. */
		operands[1] = glsl_emit_image_type(state, type);
		id = glsl_module_declare(state->module, SPV_OP_TYPE_SAMPLED_IMAGE, operands, 2U, 0);
		break;
	case GLSL_KIND_ARRAY:
		operands[1] = glsl_emit_type(state, type->element);
		operands[2] = glsl_module_constant(state->module, glsl_emit_type(state, glsl_type_scalar(GLSL_BASE_UINT)), type->length);
		id = glsl_module_declare(state->module, SPV_OP_TYPE_ARRAY, operands, 3U, 0);
		break;
	default:
		/* A struct: its members' types (a struct of its own, as two GLSL structs are two types). */
		for (index = 0U; index < type->field_count && index + 1U < GLSL_MAX_OPERANDS; index++)
			operands[index + 1U] = glsl_emit_type(state, type->fields[index].type);
		id = glsl_module_declare(state->module, SPV_OP_TYPE_STRUCT, operands, type->field_count + 1U, 1);
		break;
	}

	/* Succeeded: the id, remembered. */
	slot = emit_type_slot(state, type);
	state->type_ids[slot] = id;
	return id;
}

/*
 * Returns the pointer type to a type in a storage class.
 */
uint32_t
glsl_emit_pointer(
	struct emit_state *state,
	unsigned storage,
	uint32_t type)
{
	uint32_t operands[3];
	uint32_t id;

	/* OpTypePointer id storage type. */
	operands[0] = 0U;
	operands[1] = storage;
	operands[2] = type;
	id = glsl_module_declare(state->module, SPV_OP_TYPE_POINTER, operands, 3U, 0);

	/* Succeeded: the pointer type. */
	return id;
}

/*
 * Returns an int constant.
 */
uint32_t
glsl_emit_int(
	struct emit_state *state,
	int32_t value)
{
	uint32_t type;
	uint32_t id;

	/* A 32-bit signed constant. */
	type = glsl_emit_type(state, glsl_type_scalar(GLSL_BASE_INT));
	id = glsl_module_constant(state->module, type, (uint32_t)value);

	/* Succeeded: the constant. */
	return id;
}

/*
 * Returns a float constant.
 */
uint32_t
glsl_emit_float(
	struct emit_state *state,
	float value)
{
	union glsl_scalar bits;
	uint32_t type;
	uint32_t id;

	/* A 32-bit float constant of the value's bits. */
	bits.f = value;
	type = glsl_emit_type(state, glsl_type_scalar(GLSL_BASE_FLOAT));
	id = glsl_module_constant(state->module, type, bits.u);

	/* Succeeded: the constant. */
	return id;
}

/*
 * Returns a float or a double constant of a value, by the base.
 */
uint32_t
glsl_emit_floating(
	struct emit_state *state,
	unsigned base,
	double value)
{
	uint64_t wide;
	uint32_t type;
	uint32_t id;

	/* A float. */
	if (base != GLSL_BASE_DOUBLE) {
		id = glsl_emit_float(state, (float)value);
		return id;
	}

	/* A 64-bit float constant of the value's bits. */
	memcpy(&wide, &value, sizeof(wide));
	type = glsl_emit_type(state, glsl_type_scalar(GLSL_BASE_DOUBLE));
	id = glsl_module_constant64(state->module, type, wide);

	/* Succeeded: the constant. */
	return id;
}

/*
 * Returns a vector of a type with every component a scalar (the scalar
 * itself when the type is a scalar).
 */
uint32_t
glsl_emit_splat(
	struct emit_state *state,
	uint32_t scalar,
	const struct glsl_type *type)
{
	uint32_t parts[4];
	uint32_t id;
	unsigned index;

	/* A scalar needs nothing. */
	if (type->kind != GLSL_KIND_VECTOR)
		return scalar;

	/* The vector of copies (no insert: some compilers lack it). */
	for (index = 0U; index < type->components; index++)
		parts[index] = scalar;
	id = glsl_emit_op(state, SPV_OP_COMPOSITE_CONSTRUCT, glsl_emit_type(state, type), parts, type->components);

	/* Succeeded: the vector. */
	return id;
}

/*
 * Emits an instruction with a result into main's body and returns the
 * result id.
 */
uint32_t
glsl_emit_op(
	struct emit_state *state,
	uint32_t opcode,
	uint32_t result_type,
	const uint32_t *operands,
	unsigned count)
{
	uint32_t words[GLSL_MAX_OPERANDS];
	uint32_t id;
	int contractible;

	/* The result type, the result id, the operands. */
	id = glsl_module_id(state->module);
	words[0] = result_type;
	words[1] = id;
	if (count > GLSL_MAX_OPERANDS - 2U)
		count = GLSL_MAX_OPERANDS - 2U;
	memcpy(words + 2, operands, count * sizeof(uint32_t));
	glsl_words_add(state->module, &state->module->body, opcode, words, count + 2U);

	/* A precise shader's floating-point arithmetic is never contracted (fused into another operation). */
	contractible = 0;
	if (state->shader->precise)
		contractible = emit_contractible(opcode);
	if (contractible)
		glsl_module_decorate(state->module, id, SPV_DECORATION_NO_CONTRACTION, NULL, 0U);

	/* Succeeded: the result. */
	return id;
}

/*
 * Extracts one part of a composite value: a vector's component, a
 * matrix's column, an array's element or a struct's member.
 */
uint32_t
glsl_emit_extract(
	struct emit_state *state,
	struct emit_value value,
	unsigned index)
{
	const struct glsl_type *part;
	uint32_t operands[2];
	uint32_t id;

	/* The part's type. */
	if (value.type->kind == GLSL_KIND_STRUCT) {
		part = value.type->fields[index].type;
	} else {
		part = glsl_type_column(value.type);
	}

	/* OpCompositeExtract. */
	operands[0] = value.id;
	operands[1] = index;
	id = glsl_emit_op(state, SPV_OP_COMPOSITE_EXTRACT, glsl_emit_type(state, part), operands, 2U);

	/* Succeeded: the part. */
	return id;
}

/*
 * Emits an expression and returns its value.
 */
struct emit_value
glsl_emit_expression(
	struct emit_state *state,
	struct glsl_node *node)
{
	struct emit_path path;
	struct emit_value value;
	int status;

	/* A constant needs no code. */
	if (node->constant != NULL) {
		value = emit_value_of(emit_constant(state, node->constant), node->type);
		return value;
	}

	/* The kinds of expressions. */
	switch (node->kind) {
	case GLSL_N_IDENTIFIER:
	case GLSL_N_FIELD:
	case GLSL_N_INDEX:
		/* A variable, or a part of one, is read through its path; a part of another value is taken from it. */
		status = emit_path(state, node, &path);
		if (status == 0) {
			value = emit_load(state, &path);
			return value;
		}

		/* A part of a computed value. */
		value = emit_selection(state, node);
		return value;
	case GLSL_N_CONVERT:
		value = glsl_emit_expression(state, node->child[0]);
		value = glsl_emit_convert(state, value, node->type);
		return value;
	case GLSL_N_UNARY:
		value = emit_unary(state, node);
		return value;
	case GLSL_N_PREINC:
	case GLSL_N_PREDEC:
	case GLSL_N_POSTINC:
	case GLSL_N_POSTDEC:
		value = emit_increment(state, node);
		return value;
	case GLSL_N_BINARY:
		value = emit_binary(state, node);
		return value;
	case GLSL_N_ASSIGN:
		value = emit_assign(state, node);
		return value;
	case GLSL_N_TERNARY:
		value = emit_ternary(state, node);
		return value;
	case GLSL_N_COMMA:
		(void)glsl_emit_expression(state, node->child[0]);
		value = glsl_emit_expression(state, node->child[1]);
		return value;
	case GLSL_N_CALL:
		value = emit_call(state, node);
		return value;
	case GLSL_N_LENGTH:
		/* The length of a run-time array (ws101-p008): the others are constants. */
		value = emit_array_length(state, node);
		return value;
	default:
		break;
	}

	/* Nothing else is an expression the checker lets through. */
	value = emit_value_of(emit_zero(state, node->type), node->type);
	return value;
}

/*
 * Converts a scalar, vector or matrix value to another base of the same
 * shape (a matrix between float and double, column by column).
 */
struct emit_value
glsl_emit_convert(
	struct emit_state *state,
	struct emit_value value,
	const struct glsl_type *target)
{
	const struct glsl_type *type;
	struct emit_value column;
	uint32_t columns[4];
	uint32_t operands[3];
	uint32_t result_type;
	uint32_t opcode;
	unsigned from;
	unsigned to;
	unsigned index;
	int from_floating;
	int to_floating;
	struct emit_value converted;

	/* The same base needs nothing. */
	from = value.type->base;
	to = target->base;
	type = glsl_type_with_base(value.type, to);
	if (from == to)
		return value;
	result_type = glsl_emit_type(state, type);

	/* A matrix: each column converted. */
	if (value.type->kind == GLSL_KIND_MATRIX) {
		for (index = 0U; index < value.type->columns; index++) {
			column = emit_value_of(glsl_emit_extract(state, value, index), glsl_type_column(value.type));
			columns[index] = glsl_emit_convert(state, column, glsl_type_column(type)).id;
		}

		/* Succeeded: the converted matrix. */
		converted = emit_value_of(glsl_emit_op(state, SPV_OP_COMPOSITE_CONSTRUCT, result_type, columns, value.type->columns), type);
		return converted;
	}

	/* From a bool: a select of 1 or 0 of the target base. */
	from_floating = glsl_base_floating(from);
	to_floating = glsl_base_floating(to);
	if (from == GLSL_BASE_BOOL) {
		operands[0] = value.id;
		if (to_floating) {
			operands[1] = glsl_emit_splat(state, glsl_emit_floating(state, to, 1.0), type);
			operands[2] = glsl_emit_splat(state, glsl_emit_floating(state, to, 0.0), type);
		} else {
			operands[1] = glsl_emit_splat(state, glsl_module_constant(state->module, glsl_emit_type(state, glsl_type_scalar(to)), 1U), type);
			operands[2] = glsl_emit_splat(state, glsl_module_constant(state->module, glsl_emit_type(state, glsl_type_scalar(to)), 0U), type);
		}

		/* Succeeded: the number. */
		converted = emit_value_of(glsl_emit_op(state, SPV_OP_SELECT, result_type, operands, 3U), type);
		return converted;
	}

	/* To a bool: whether the value is not zero. */
	if (to == GLSL_BASE_BOOL) {
		operands[0] = value.id;
		if (from_floating) {
			opcode = SPV_OP_F_ORD_NOT_EQUAL;
			operands[1] = glsl_emit_splat(state, glsl_emit_floating(state, from, 0.0), value.type);
		} else {
			opcode = SPV_OP_I_NOT_EQUAL;
			operands[1] = glsl_emit_splat(state, glsl_module_constant(state->module, glsl_emit_type(state, glsl_type_scalar(from)), 0U), value.type);
		}

		/* Succeeded: the bool. */
		converted = emit_value_of(glsl_emit_op(state, opcode, result_type, operands, 2U), type);
		return converted;
	}

	/* Between numbers: the conversion instruction of the two bases. */
	if (from_floating && to_floating) {
		opcode = SPV_OP_F_CONVERT;
	} else if (to_floating && from == GLSL_BASE_INT) {
		opcode = SPV_OP_CONVERT_S_TO_F;
	} else if (to_floating) {
		opcode = SPV_OP_CONVERT_U_TO_F;
	} else if (from_floating && to == GLSL_BASE_INT) {
		opcode = SPV_OP_CONVERT_F_TO_S;
	} else if (from_floating) {
		opcode = SPV_OP_CONVERT_F_TO_U;
	} else {
		opcode = SPV_OP_BITCAST;
	}

	/* Succeeded: the converted value. */
	operands[0] = value.id;
	converted = emit_value_of(glsl_emit_op(state, opcode, result_type, operands, 1U), type);
	return converted;
}

/*
 * Reduces a bool vector to one bool: whether all its components are true
 * (or any, when asked), by a chain of ands or ors of its components (no
 * OpAll or OpAny, which i915's compiler lacks).
 */
uint32_t
glsl_emit_all(
	struct emit_state *state,
	uint32_t vector,
	unsigned components,
	int any)
{
	const struct glsl_type *bool_type;
	struct emit_value value;
	uint32_t operands[2];
	uint32_t result;
	uint32_t opcode;
	unsigned index;

	/* A scalar is its own answer. */
	if (components <= 1U)
		return vector;

	/* The components joined one by one. */
	bool_type = glsl_type_scalar(GLSL_BASE_BOOL);
	value = emit_value_of(vector, glsl_type_vector(GLSL_BASE_BOOL, components));
	opcode = SPV_OP_LOGICAL_AND;
	if (any)
		opcode = SPV_OP_LOGICAL_OR;
	result = glsl_emit_extract(state, value, 0U);
	for (index = 1U; index < components; index++) {
		operands[0] = result;
		operands[1] = glsl_emit_extract(state, value, index);
		result = glsl_emit_op(state, opcode, glsl_emit_type(state, bool_type), operands, 2U);
	}

	/* Succeeded: the bool. */
	return result;
}

/*
 * Returns the image type a sampler type reads.
 */
uint32_t
glsl_emit_image_type(
	struct emit_state *state,
	const struct glsl_type *type)
{
	uint32_t operands[8];
	uint32_t id;

	/* The texel type, the dimension, depth, not arrayed, not multisampled, sampled, format unknown. */
	operands[0] = 0U;
	operands[1] = glsl_emit_type(state, glsl_type_scalar(type->base));
	switch (type->sampler) {
	case GLSL_SAMPLER_1D:
		operands[2] = 0U;
		glsl_emit_capability(state, SPV_CAPABILITY_SAMPLED_1D, &state->module->sampled_1d);
		break;
	case GLSL_SAMPLER_3D:
		operands[2] = 2U;
		break;
	case GLSL_SAMPLER_CUBE:
		operands[2] = 3U;
		if (type->arrayed)
			glsl_emit_capability(state, SPV_CAPABILITY_SAMPLED_CUBE_ARRAY, &state->module->sampled_cube_array);
		break;
	case GLSL_SAMPLER_BUFFER:
		operands[2] = 5U;
		glsl_emit_capability(state, SPV_CAPABILITY_SAMPLED_BUFFER, &state->module->sampled_buffer);
		break;
	default:
		/* 2D, and a rectangle's 2D image (Vulkan has no Rect images; its coordinates are made 2D's). */
		operands[2] = 1U;
		break;
	}

	/* A depth image for a shadow sampler; one level, not arrayed, multisampled for a multisample sampler, sampled, no format. */
	operands[3] = type->shadow;
	operands[4] = type->arrayed;
	operands[5] = 0U;
	if (type->sampler == GLSL_SAMPLER_MS)
		operands[5] = 1U;
	operands[6] = 1U;
	operands[7] = 0U;
	id = glsl_module_declare(state->module, SPV_OP_TYPE_IMAGE, operands, 8U, 0);

	/* Succeeded: the image type. */
	return id;
}

/* Declares a capability once. */
void
glsl_emit_capability(
	struct emit_state *state,
	uint32_t capability,
	unsigned *declared)
{
	/* Only once. */
	if (*declared)
		return;
	*declared = 1U;

	/* OpCapability. */
	glsl_words_add(state->module, &state->module->capabilities, SPV_OP_CAPABILITY, &capability, 1U);
}

/* Returns a type's slot in the id cache, making one when it has none. */
static unsigned
emit_type_slot(
	struct emit_state *state,
	const struct glsl_type *type)
{
	const struct glsl_type **keys;
	uint32_t *ids;
	uint32_t *layouts;
	uint32_t *layouts430;
	unsigned capacity;
	unsigned index;

	/* The slot the type has. */
	for (index = 0U; index < state->type_count; index++) {
		if (state->type_keys[index] == type)
			return index;
	}

	/* Room for one more. */
	if (state->type_count == state->type_capacity) {
		capacity = state->type_capacity * 2U;
		if (capacity < 64U)
			capacity = 64U;
		keys = glsl_alloc(state->module->arena, capacity * sizeof(*keys));
		ids = glsl_alloc(state->module->arena, capacity * sizeof(*ids));
		layouts = glsl_alloc(state->module->arena, capacity * sizeof(*layouts));
		layouts430 = glsl_alloc(state->module->arena, capacity * sizeof(*layouts430));
		if (state->type_count != 0U) {
			memcpy(keys, state->type_keys, state->type_count * sizeof(*keys));
			memcpy(ids, state->type_ids, state->type_count * sizeof(*ids));
			memcpy(layouts, state->layout_ids, state->type_count * sizeof(*layouts));
			memcpy(layouts430, state->layout430_ids, state->type_count * sizeof(*layouts430));
		}

		/* The larger tables replace the old ones. */
		state->type_keys = keys;
		state->type_ids = ids;
		state->layout_ids = layouts;
		state->layout430_ids = layouts430;
		state->type_capacity = capacity;
	}

	/* The new slot, without ids yet. */
	state->type_keys[state->type_count] = type;
	state->type_ids[state->type_count] = 0U;
	state->layout_ids[state->type_count] = 0U;
	state->layout430_ids[state->type_count] = 0U;
	state->type_count++;
	return state->type_count - 1U;
}

/*
 * Returns the type a GLSL type has inside the default uniform block:
 * bools as uints, arrays with their std140 stride, structs with their
 * std140 offsets (each a type of its own, apart from the local types).
 */
static uint32_t
emit_layout_type(
	struct emit_state *state,
	const struct glsl_type *type)
{
	uint32_t operands[GLSL_MAX_OPERANDS];
	const struct glsl_type *field;
	uint32_t stride;
	uint32_t id;
	uint32_t offset;
	unsigned slot;
	unsigned index;
	unsigned alignment;

	/* Scalars, vectors and matrices are the local types, but bools are uints. */
	if (type->kind == GLSL_KIND_SCALAR || type->kind == GLSL_KIND_VECTOR || type->kind == GLSL_KIND_MATRIX) {
		if (type->base == GLSL_BASE_BOOL)
			type = glsl_type_with_base(type, GLSL_BASE_UINT);
		id = glsl_emit_type(state, type);
		return id;
	}

	/* A layout given before. */
	slot = emit_type_slot(state, type);
	if (state->layout_ids[slot] != 0U)
		return state->layout_ids[slot];

	/* An array: its element's layout, its length, its stride. */
	operands[0] = 0U;
	if (type->kind == GLSL_KIND_ARRAY) {
		operands[1] = emit_layout_type(state, type->element);
		operands[2] = glsl_module_constant(state->module, glsl_emit_type(state, glsl_type_scalar(GLSL_BASE_UINT)), type->length);
		id = glsl_module_declare(state->module, SPV_OP_TYPE_ARRAY, operands, 3U, 1);
		stride = glsl_std140_stride(type);
		glsl_module_decorate(state->module, id, SPV_DECORATION_ARRAY_STRIDE, &stride, 1U);
		slot = emit_type_slot(state, type);
		state->layout_ids[slot] = id;
		return id;
	}

	/* A struct: its members' layouts (an array of row-major matrices has a stride of its own). */
	for (index = 0U; index < type->field_count && index + 1U < GLSL_MAX_OPERANDS; index++) {
		field = type->fields[index].type;
		if (type->fields[index].row_major && field->kind == GLSL_KIND_ARRAY && field->element->kind == GLSL_KIND_MATRIX) {
			operands[index + 1U] = emit_layout_row_major_array(state, field);
		} else {
			operands[index + 1U] = emit_layout_type(state, field);
		}
	}

	/* The struct, a type of its own. */
	id = glsl_module_declare(state->module, SPV_OP_TYPE_STRUCT, operands, type->field_count + 1U, 1);

	/* Each member's offset, name and matrix layout. */
	offset = 0U;
	for (index = 0U; index < type->field_count; index++) {
		alignment = glsl_std140_member_alignment(type->fields[index].type, type->fields[index].row_major);
		offset = (offset + alignment - 1U) & ~(alignment - 1U);
		emit_layout_members(state, id, index, type->fields[index].type, offset, type->fields[index].name, type->fields[index].row_major);
		offset += glsl_std140_member_size(type->fields[index].type, type->fields[index].row_major);
	}

	/* Succeeded: the struct's layout type, remembered. */
	slot = emit_type_slot(state, type);
	state->layout_ids[slot] = id;
	return id;
}

/* Decorates one member of a laid-out struct: its offset, its name, and a matrix's stride and order. */
static void
emit_layout_members(
	struct emit_state *state,
	uint32_t structure,
	unsigned member,
	const struct glsl_type *type,
	uint32_t offset,
	const char *name,
	unsigned row_major)
{
	const struct glsl_type *element;
	uint32_t stride;

	/* The offset and the name. */
	glsl_module_member_decorate(state->module, structure, member, SPV_DECORATION_OFFSET, offset);
	glsl_module_member_name(state->module, structure, member, name);

	/* A matrix (or an array of them): its order and the stride of its columns or rows. */
	element = type;
	if (element->kind == GLSL_KIND_ARRAY)
		element = element->element;
	if (element->kind != GLSL_KIND_MATRIX)
		return;
	if (row_major) {
		glsl_module_member_decorate(state->module, structure, member, SPV_DECORATION_ROW_MAJOR, 0xffffffffU);
	} else {
		glsl_module_member_decorate(state->module, structure, member, SPV_DECORATION_COL_MAJOR, 0xffffffffU);
	}

	/* Rows or columns a vec4 apart (a dvec3's or dvec4's for those of a double matrix). */
	stride = glsl_std140_matrix_stride(element, row_major);
	glsl_module_member_decorate(state->module, structure, member, SPV_DECORATION_MATRIX_STRIDE, stride);
}

/* Returns the laid-out type of an array of row-major matrices (a row per component, a row's stride apart). */
static uint32_t
emit_layout_row_major_array(
	struct emit_state *state,
	const struct glsl_type *type)
{
	uint32_t operands[3];
	uint32_t stride;
	uint32_t id;

	/* The array, a type of its own with its stride. */
	operands[0] = 0U;
	operands[1] = emit_layout_type(state, type->element);
	operands[2] = glsl_module_constant(state->module, glsl_emit_type(state, glsl_type_scalar(GLSL_BASE_UINT)), type->length);
	id = glsl_module_declare(state->module, SPV_OP_TYPE_ARRAY, operands, 3U, 1);
	stride = glsl_std140_member_stride(type, 1U);
	glsl_module_decorate(state->module, id, SPV_DECORATION_ARRAY_STRIDE, &stride, 1U);

	/* Succeeded: the type. */
	return id;
}

/* Declares a uniform block: its laid-out struct, a Block at set 0 and the link's binding, named by the block. */
static void
emit_uniform_block(
	struct emit_state *state,
	struct glsl_symbol *symbol)
{
	const struct glsl_type *type;
	uint32_t operands[3];
	uint32_t structure;
	uint32_t variable_type;
	uint32_t value;
	unsigned slot;

	/* The struct, a Block named by the block. */
	type = symbol->type;
	if (type->kind == GLSL_KIND_ARRAY)
		type = type->element;
	structure = emit_layout_type(state, type);
	glsl_module_decorate(state->module, structure, SPV_DECORATION_BLOCK, NULL, 0U);
	glsl_module_name(state->module, structure, type->name);

	/*
	 * An array of blocks is an array of the struct without a stride (each
	 * element a descriptor of the binding's array), the layout the access
	 * chains into it take.
	 */
	variable_type = structure;
	if (symbol->type->kind == GLSL_KIND_ARRAY) {
		operands[0] = 0U;
		operands[1] = structure;
		operands[2] = glsl_module_constant(state->module, glsl_emit_type(state, glsl_type_scalar(GLSL_BASE_UINT)), symbol->type->length);
		variable_type = glsl_module_declare(state->module, SPV_OP_TYPE_ARRAY, operands, 3U, 1);
		slot = emit_type_slot(state, symbol->type);
		state->layout_ids[slot] = variable_type;
	}

	/* The variable: set 0, the link's binding. */
	symbol->id = glsl_module_id(state->module);
	symbol->id_storage = SPV_STORAGE_UNIFORM;
	operands[0] = glsl_emit_pointer(state, SPV_STORAGE_UNIFORM, variable_type);
	operands[1] = symbol->id;
	operands[2] = SPV_STORAGE_UNIFORM;
	glsl_words_add(state->module, &state->module->globals, SPV_OP_VARIABLE, operands, 3U);
	value = 0U;
	glsl_module_decorate(state->module, symbol->id, SPV_DECORATION_DESCRIPTOR_SET, &value, 1U);
	value = symbol->binding;
	glsl_module_decorate(state->module, symbol->id, SPV_DECORATION_BINDING, &value, 1U);
	glsl_module_name(state->module, symbol->id, type->name);
}

/* Returns the id of a constant value. */
static uint32_t
emit_constant(
	struct emit_state *state,
	const struct glsl_constant *constant)
{
	uint32_t id;
	unsigned at;

	/* The value from its first scalar. */
	at = 0U;
	id = emit_constant_part(state, constant->type, constant->values, &at);

	/* Succeeded: the constant. */
	return id;
}

/* Returns the constant of a type made of the flattened scalars from values[*at]. */
static uint32_t
emit_constant_part(
	struct emit_state *state,
	const struct glsl_type *type,
	const union glsl_scalar *values,
	unsigned *at)
{
	uint32_t parts[EMIT_MAX_PARTS];
	uint32_t type_id;
	uint32_t id;
	uint64_t wide;
	unsigned count;
	unsigned index;

	/* A scalar: the next value. */
	type_id = glsl_emit_type(state, type);
	if (type->kind == GLSL_KIND_SCALAR) {
		if (type->base == GLSL_BASE_BOOL) {
			id = glsl_module_bool(state->module, type_id, values[*at].u != 0U);
		} else if (type->base == GLSL_BASE_DOUBLE) {
			memcpy(&wide, &values[*at].d, sizeof(wide));
			id = glsl_module_constant64(state->module, type_id, wide);
		} else {
			id = glsl_module_constant(state->module, type_id, values[*at].u);
		}

		/* The scalar is used up. */
		(*at)++;
		return id;
	}

	/* A composite: its parts in order. */
	if (type->kind == GLSL_KIND_VECTOR) {
		count = type->components;
	} else if (type->kind == GLSL_KIND_MATRIX) {
		count = type->columns;
	} else if (type->kind == GLSL_KIND_ARRAY) {
		count = type->length;
	} else {
		count = type->field_count;
	}

	/* At most the parts the table holds. */
	if (count > EMIT_MAX_PARTS)
		count = EMIT_MAX_PARTS;
	for (index = 0U; index < count; index++) {
		if (type->kind == GLSL_KIND_STRUCT) {
			parts[index] = emit_constant_part(state, type->fields[index].type, values, at);
		} else {
			parts[index] = emit_constant_part(state, glsl_type_column(type), values, at);
		}
	}

	/* Succeeded: the composite constant. */
	id = glsl_module_composite(state->module, type_id, parts, count);
	return id;
}

/* Returns the zero value of a type. */
static uint32_t
emit_zero(
	struct emit_state *state,
	const struct glsl_type *type)
{
	struct glsl_constant *zero;
	uint32_t id;

	/* A constant of zeros. */
	zero = glsl_constant_new(state->module->arena, type);
	id = emit_constant(state, zero);

	/* Succeeded: the zero. */
	return id;
}

/* Declares a Function variable of main of a type. */
static uint32_t
emit_variable(
	struct emit_state *state,
	const struct glsl_type *type)
{
	uint32_t operands[3];
	uint32_t id;

	/* OpVariable pointer id Function, among main's variables. */
	id = glsl_module_id(state->module);
	operands[0] = glsl_emit_pointer(state, SPV_STORAGE_FUNCTION, glsl_emit_type(state, type));
	operands[1] = id;
	operands[2] = SPV_STORAGE_FUNCTION;
	glsl_words_add(state->module, &state->module->variables, SPV_OP_VARIABLE, operands, 3U);

	/* Succeeded: the variable. */
	return id;
}

/* Declares the globals the code uses: the block, samplers, inputs and outputs, and main's copies of plain globals. */
static void
emit_globals(
	struct emit_state *state)
{
	struct glsl_symbol *symbol;
	uint32_t operands[2];
	uint32_t value;

	/* The default uniform block. */
	emit_block(state);

	/* Each global the code uses. */
	for (symbol = state->shader->globals; symbol != NULL; symbol = symbol->next_global) {
		if (!symbol->used)
			continue;

		/* Where it lives. */
		switch (symbol->where) {
		case GLSL_VAR_UNIFORM:
			if (state->uniforms[symbol->uniform].sampler != 0U)
				emit_sampler(state, symbol);
			break;
		case GLSL_VAR_INPUT:
		case GLSL_VAR_OUTPUT:
			emit_interface(state, symbol);
			break;
		case GLSL_VAR_BLOCK:
			emit_uniform_block(state, symbol);
			break;
		case GLSL_VAR_BUFFER:
			emit_storage_block(state, symbol);
			break;
		case GLSL_VAR_IMAGE:
			emit_image(state, symbol);
			break;
		case GLSL_VAR_SHARED:
			emit_shared(state, symbol);
			break;
		case GLSL_VAR_GLOBAL:
			/* A plain global is a variable of main, stored first with its value or zero. */
			symbol->id = emit_variable(state, symbol->type);
			symbol->id_storage = SPV_STORAGE_FUNCTION;
			if (symbol->initial != NULL) {
				value = emit_constant(state, symbol->initial);
			} else {
				value = emit_zero(state, symbol->type);
			}

			/* The first value stored. */
			operands[0] = symbol->id;
			operands[1] = value;
			glsl_words_add(state->module, &state->module->body, SPV_OP_STORE, operands, 2U);
			break;
		default:
			break;
		}
	}

	/* gl_FragCoord and gl_PointCoord in GL's directions (ws068-p004). */
	emit_directions(state);
}

/* Loads the hidden uniform gl_ZedFragment (a, b, c, d) from the default block. */
static uint32_t
emit_zed_load(
	struct emit_state *state)
{
	const struct glsl_type *vec4;
	struct glsl_symbol *zed;
	uint32_t operands[2];
	uint32_t target;
	uint32_t pointer;
	uint32_t value;

	/* An access chain to its member, and the load. */
	zed = state->shader->zed_fragment;
	vec4 = glsl_type_vector(GLSL_BASE_FLOAT, 4U);
	target = emit_layout_type(state, vec4);
	operands[0] = state->block;
	operands[1] = glsl_emit_int(state, state->members[zed->uniform]);
	pointer = glsl_emit_op(state, SPV_OP_ACCESS_CHAIN, glsl_emit_pointer(state, SPV_STORAGE_UNIFORM, target), operands, 2U);
	value = glsl_emit_op(state, SPV_OP_LOAD, target, &pointer, 1U);

	/* Succeeded: the vector. */
	return value;
}

/*
 * Turns a fragment shader's gl_FragCoord.y and gl_PointCoord.t into GL's
 * directions (ws068-p004): main starts by reading each from its input and
 * keeping a + b * y and c + d * t, with (a, b, c, d) the hidden uniform
 * gl_ZedFragment that libGLESv2 fills at each draw, in a variable of its
 * own, which the code then reads in place of the input.
 */
static void
emit_directions(
	struct emit_state *state)
{
	struct glsl_symbol *zed;
	struct glsl_symbol *symbol;
	uint32_t value;

	/* Only a fragment shader that reads either. */
	zed = state->shader->zed_fragment;
	if (zed == NULL || !zed->used || state->members[zed->uniform] < 0)
		return;

	/* The hidden uniform's value, from the default block. */
	value = emit_zed_load(state);

	/* Each of the two the code reads, turned. */
	for (symbol = state->shader->globals; symbol != NULL; symbol = symbol->next_global) {
		if (!symbol->used || symbol->where != GLSL_VAR_INPUT)
			continue;

		/* gl_FragCoord.y: a + b * y. */
		if (symbol->builtin == GLSL_BUILTIN_FRAG_COORD) {
			symbol->id = emit_turned(state, symbol, value, 1U, 0U);
			symbol->id_storage = SPV_STORAGE_FUNCTION;
		}

		/* gl_PointCoord.t: c + d * t. */
		if (symbol->builtin == GLSL_BUILTIN_POINT_COORD) {
			symbol->id = emit_turned(state, symbol, value, 1U, 2U);
			symbol->id_storage = SPV_STORAGE_FUNCTION;
		}
	}
}

/*
 * Reads a built-in input vector, replaces one component v with
 * zed[scale] + zed[scale + 1] * v, and returns the Function variable of
 * main the result is stored in.
 */
static uint32_t
emit_turned(
	struct emit_state *state,
	struct glsl_symbol *symbol,
	uint32_t zed,
	unsigned component,
	unsigned scale)
{
	struct emit_value input;
	struct emit_value factors;
	uint32_t parts[4];
	uint32_t operands[2];
	uint32_t scalar;
	uint32_t variable;
	unsigned count;
	unsigned index;

	/* The input as it is. */
	input.type = symbol->type;
	input.id = glsl_emit_op(state, SPV_OP_LOAD, glsl_emit_type(state, symbol->type), &symbol->id, 1U);
	factors.type = glsl_type_vector(GLSL_BASE_FLOAT, 4U);
	factors.id = zed;

	/* Its components, the one turned: zed[scale] + zed[scale + 1] * v. */
	count = symbol->type->components;
	scalar = glsl_emit_type(state, glsl_type_scalar(GLSL_BASE_FLOAT));
	for (index = 0U; index < count; index++)
		parts[index] = glsl_emit_extract(state, input, index);
	operands[0] = glsl_emit_extract(state, factors, scale + 1U);
	operands[1] = parts[component];
	operands[1] = glsl_emit_op(state, SPV_OP_F_MUL, scalar, operands, 2U);
	operands[0] = glsl_emit_extract(state, factors, scale);
	parts[component] = glsl_emit_op(state, SPV_OP_F_ADD, scalar, operands, 2U);

	/* The vector again, kept in a variable of main. */
	variable = emit_variable(state, symbol->type);
	operands[0] = variable;
	operands[1] = glsl_emit_op(state, SPV_OP_COMPOSITE_CONSTRUCT, glsl_emit_type(state, symbol->type), parts, count);
	glsl_words_add(state->module, &state->module->body, SPV_OP_STORE, operands, 2U);
	return variable;
}

/* Declares the default uniform block with the uniforms this stage uses, at the link's offsets. */
static void
emit_block(
	struct emit_state *state)
{
	struct glsl_symbol *symbol;
	uint32_t operands[GLSL_MAX_OPERANDS];
	uint32_t structure;
	uint32_t value;
	unsigned count;
	unsigned index;

	/* The uniforms this stage uses, in the link's order, as members. */
	count = 0U;
	for (index = 0U; index < state->uniform_count; index++) {
		for (symbol = state->shader->globals; symbol != NULL; symbol = symbol->next_global) {
			if (!symbol->used || symbol->where != GLSL_VAR_UNIFORM || symbol->uniform != index)
				continue;
			if (state->uniforms[index].sampler != 0U || count + 1U >= GLSL_MAX_OPERANDS)
				continue;
			state->members[index] = (int)count;
			operands[count + 1U] = emit_layout_type(state, symbol->type);
			count++;
			break;
		}
	}

	/* No uniform, no block. */
	if (count == 0U)
		return;

	/* The struct: members at the link's offsets, named, matrices column-major 16 bytes apart. */
	operands[0] = 0U;
	structure = glsl_module_declare(state->module, SPV_OP_TYPE_STRUCT, operands, count + 1U, 1);
	for (index = 0U; index < state->uniform_count; index++) {
		if (state->members[index] < 0)
			continue;
		emit_layout_members(state, structure, (unsigned)state->members[index], state->uniforms[index].type,
				    state->uniforms[index].offset, state->uniforms[index].name, 0U);
	}

	/* The struct is a block, named for the reflection. */
	glsl_module_decorate(state->module, structure, SPV_DECORATION_BLOCK, NULL, 0U);
	glsl_module_name(state->module, structure, "gl_DefaultUniformBlock");

	/* The variable: set 0, binding 0. */
	state->block = glsl_module_id(state->module);
	operands[0] = glsl_emit_pointer(state, SPV_STORAGE_UNIFORM, structure);
	operands[1] = state->block;
	operands[2] = SPV_STORAGE_UNIFORM;
	glsl_words_add(state->module, &state->module->globals, SPV_OP_VARIABLE, operands, 3U);
	value = 0U;
	glsl_module_decorate(state->module, state->block, SPV_DECORATION_DESCRIPTOR_SET, &value, 1U);
	glsl_module_decorate(state->module, state->block, SPV_DECORATION_BINDING, &value, 1U);
	glsl_module_name(state->module, state->block, "gl_DefaultUniformBlock");
}

/* Declares a sampler uniform: set 0 at the link's binding, named. */
static void
emit_sampler(
	struct emit_state *state,
	struct glsl_symbol *symbol)
{
	uint32_t operands[3];
	uint32_t value;

	/* The UniformConstant variable. */
	symbol->id = glsl_module_id(state->module);
	symbol->id_storage = SPV_STORAGE_UNIFORM_CONSTANT;
	operands[0] = glsl_emit_pointer(state, SPV_STORAGE_UNIFORM_CONSTANT, glsl_emit_type(state, symbol->type));
	operands[1] = symbol->id;
	operands[2] = SPV_STORAGE_UNIFORM_CONSTANT;
	glsl_words_add(state->module, &state->module->globals, SPV_OP_VARIABLE, operands, 3U);

	/* Set 0, its binding, its name. */
	value = 0U;
	glsl_module_decorate(state->module, symbol->id, SPV_DECORATION_DESCRIPTOR_SET, &value, 1U);
	value = state->uniforms[symbol->uniform].binding;
	glsl_module_decorate(state->module, symbol->id, SPV_DECORATION_BINDING, &value, 1U);
	glsl_module_name(state->module, symbol->id, symbol->name);
}

/*
 * Declares an image uniform: set 0 at its unit's binding (an array of
 * images is one binding with a descriptor an element, its first unit's),
 * named, with its memory qualifiers as decorations (readonly NonWritable,
 * writeonly NonReadable).
 */
static void
emit_image(
	struct emit_state *state,
	struct glsl_symbol *symbol)
{
	uint32_t operands[3];
	uint32_t value;

	/* The UniformConstant variable of its storage image type. */
	symbol->id = glsl_module_id(state->module);
	symbol->id_storage = SPV_STORAGE_UNIFORM_CONSTANT;
	operands[0] = glsl_emit_pointer(state, SPV_STORAGE_UNIFORM_CONSTANT, glsl_emit_type(state, symbol->type));
	operands[1] = symbol->id;
	operands[2] = SPV_STORAGE_UNIFORM_CONSTANT;
	glsl_words_add(state->module, &state->module->globals, SPV_OP_VARIABLE, operands, 3U);

	/* Set 0, its unit's binding, its name. */
	value = 0U;
	glsl_module_decorate(state->module, symbol->id, SPV_DECORATION_DESCRIPTOR_SET, &value, 1U);
	value = GLSL_IMAGE_FIRST_BINDING + symbol->layout_binding;
	glsl_module_decorate(state->module, symbol->id, SPV_DECORATION_BINDING, &value, 1U);
	glsl_module_name(state->module, symbol->id, symbol->name);

	/* Its memory qualifiers. */
	if ((symbol->memory & GLSL_MEMORY_READONLY) != 0U)
		glsl_module_decorate(state->module, symbol->id, SPV_DECORATION_NON_WRITABLE, NULL, 0U);
	if ((symbol->memory & GLSL_MEMORY_WRITEONLY) != 0U)
		glsl_module_decorate(state->module, symbol->id, SPV_DECORATION_NON_READABLE, NULL, 0U);
	if ((symbol->memory & GLSL_MEMORY_COHERENT) != 0U)
		glsl_module_decorate(state->module, symbol->id, SPV_DECORATION_COHERENT, NULL, 0U);
	if ((symbol->memory & GLSL_MEMORY_VOLATILE) != 0U)
		glsl_module_decorate(state->module, symbol->id, SPV_DECORATION_VOLATILE, NULL, 0U);
	if ((symbol->memory & GLSL_MEMORY_RESTRICT) != 0U)
		glsl_module_decorate(state->module, symbol->id, SPV_DECORATION_RESTRICT, NULL, 0U);
}

/*
 * Returns the storage image type of an image variable's type: its texels,
 * its dimension and layers, read and written without a sampler, of its
 * format (SPIR-V's numbers of GLSL ES 3.10's formats).
 */
static uint32_t
emit_storage_image_type(
	struct emit_state *state,
	const struct glsl_type *type)
{
	static const uint32_t formats[] = {
		0U,	/* none: Unknown */
		1U,	/* rgba32f */
		2U,	/* rgba16f */
		3U,	/* r32f */
		4U,	/* rgba8 */
		5U,	/* rgba8_snorm */
		21U,	/* rgba32i */
		22U,	/* rgba16i */
		23U,	/* rgba8i */
		24U,	/* r32i */
		30U,	/* rgba32ui */
		31U,	/* rgba16ui */
		32U,	/* rgba8ui */
		33U	/* r32ui */
	};
	uint32_t operands[9];
	uint32_t id;

	/* The texel type and the dimension (2D, 3D, cube; a 2D array is 2D arrayed). */
	operands[0] = 0U;
	operands[1] = glsl_emit_type(state, glsl_type_scalar(type->base));
	operands[2] = 1U;
	if (type->sampler == GLSL_SAMPLER_3D)
		operands[2] = 2U;
	if (type->sampler == GLSL_SAMPLER_CUBE)
		operands[2] = 3U;

	/* Not depth, arrayed or not, single-sampled, a storage image (2), its format. */
	operands[3] = 0U;
	operands[4] = type->arrayed;
	operands[5] = 0U;
	operands[6] = 2U;
	operands[7] = 0U;
	if (type->image_format < sizeof(formats) / sizeof(formats[0]))
		operands[7] = formats[type->image_format];
	id = glsl_module_declare(state->module, SPV_OP_TYPE_IMAGE, operands, 8U, 0);

	/* Succeeded: the image type. */
	return id;
}

/* Declares an input or an output (a built-in, or located and named). */
static void
emit_interface(
	struct emit_state *state,
	struct glsl_symbol *symbol)
{
	uint32_t operands[3];
	unsigned storage;

	/* The variable. */
	storage = SPV_STORAGE_INPUT;
	if (symbol->where == GLSL_VAR_OUTPUT)
		storage = SPV_STORAGE_OUTPUT;
	symbol->id = glsl_module_id(state->module);
	symbol->id_storage = storage;
	operands[0] = glsl_emit_pointer(state, storage, glsl_emit_type(state, symbol->type));
	operands[1] = symbol->id;
	operands[2] = storage;
	glsl_words_add(state->module, &state->module->globals, SPV_OP_VARIABLE, operands, 3U);

	/* Its decorations, and its place in the entry point's interface. */
	emit_interface_decorations(state, symbol, symbol->id);
	if (state->interface_count < 64U) {
		state->interface[state->interface_count] = symbol->id;
		state->interface_count++;
	}
}

/* Decorates an input or output: its built-in, or its location, name and interpolation. */
static void
emit_interface_decorations(
	struct emit_state *state,
	struct glsl_symbol *symbol,
	uint32_t variable)
{
	uint32_t value;

	/* The built-ins (gl_FragColor and gl_FragData are colour output 0). */
	value = 0xffffffffU;
	switch (symbol->builtin) {
	case GLSL_BUILTIN_POSITION:
		value = SPV_BUILT_IN_POSITION;
		break;
	case GLSL_BUILTIN_POINT_SIZE:
		value = SPV_BUILT_IN_POINT_SIZE;
		break;
	case GLSL_BUILTIN_VERTEX_ID:
		value = SPV_BUILT_IN_VERTEX_INDEX;
		break;
	case GLSL_BUILTIN_INSTANCE_ID:
		value = SPV_BUILT_IN_INSTANCE_INDEX;
		break;
	case GLSL_BUILTIN_FRAG_COORD:
		value = SPV_BUILT_IN_FRAG_COORD;
		break;
	case GLSL_BUILTIN_FRONT_FACING:
		value = SPV_BUILT_IN_FRONT_FACING;
		break;
	case GLSL_BUILTIN_POINT_COORD:
		value = SPV_BUILT_IN_POINT_COORD;
		break;
	case GLSL_BUILTIN_FRAG_DEPTH:
		value = SPV_BUILT_IN_FRAG_DEPTH;
		state->depth_written = 1U;
		break;
	case GLSL_BUILTIN_PRIMITIVE_ID_IN:
	case GLSL_BUILTIN_PRIMITIVE_ID:
		/* A tessellation shader's needs its own capability, not the geometry one (a device feature of its own). */
		value = SPV_BUILT_IN_PRIMITIVE_ID;
		if (state->shader->stage == GLSL_STAGE_TESS_CONTROL || state->shader->stage == GLSL_STAGE_TESS_EVALUATION)
			break;
		glsl_emit_capability(state, SPV_CAPABILITY_GEOMETRY, &state->module->geometry);
		break;
	case GLSL_BUILTIN_SAMPLE_ID:
		value = SPV_BUILT_IN_SAMPLE_ID;
		glsl_emit_capability(state, SPV_CAPABILITY_SAMPLE_RATE_SHADING, &state->module->sample_rate_shading);
		break;
	case GLSL_BUILTIN_SAMPLE_POSITION:
		value = SPV_BUILT_IN_SAMPLE_POSITION;
		glsl_emit_capability(state, SPV_CAPABILITY_SAMPLE_RATE_SHADING, &state->module->sample_rate_shading);
		break;
	case GLSL_BUILTIN_SAMPLE_MASK_IN:
	case GLSL_BUILTIN_SAMPLE_MASK:
		value = SPV_BUILT_IN_SAMPLE_MASK;
		break;
	case GLSL_BUILTIN_TESS_LEVEL_OUTER:
		value = SPV_BUILT_IN_TESS_LEVEL_OUTER;
		break;
	case GLSL_BUILTIN_TESS_LEVEL_INNER:
		value = SPV_BUILT_IN_TESS_LEVEL_INNER;
		break;
	case GLSL_BUILTIN_TESS_COORD:
		value = SPV_BUILT_IN_TESS_COORD;
		break;
	case GLSL_BUILTIN_PATCH_VERTICES:
		value = SPV_BUILT_IN_PATCH_VERTICES;
		break;
	case GLSL_BUILTIN_INVOCATION_ID:
		value = SPV_BUILT_IN_INVOCATION_ID;
		break;
	case GLSL_BUILTIN_LAYER:
		value = SPV_BUILT_IN_LAYER;
		glsl_emit_capability(state, SPV_CAPABILITY_GEOMETRY, &state->module->geometry);
		break;
	case GLSL_BUILTIN_GLOBAL_INVOCATION_ID:
		value = SPV_BUILT_IN_GLOBAL_INVOCATION_ID;
		break;
	case GLSL_BUILTIN_LOCAL_INVOCATION_ID:
		value = SPV_BUILT_IN_LOCAL_INVOCATION_ID;
		break;
	case GLSL_BUILTIN_WORK_GROUP_ID:
		value = SPV_BUILT_IN_WORKGROUP_ID;
		break;
	case GLSL_BUILTIN_NUM_WORK_GROUPS:
		value = SPV_BUILT_IN_NUM_WORKGROUPS;
		break;
	case GLSL_BUILTIN_LOCAL_INVOCATION_INDEX:
		value = SPV_BUILT_IN_LOCAL_INVOCATION_INDEX;
		break;
	case GLSL_BUILTIN_PER_VERTEX:
		/* gl_in: its block's members are the built-ins; the array has no location. */
		emit_per_vertex(state, symbol->type->element);
		return;
	default:
		break;
	}

	/* A fragment shader's gl_PrimitiveID, gl_SampleID and gl_SampleMaskIn are integer inputs, so flat. */
	if (symbol->builtin == GLSL_BUILTIN_PRIMITIVE_ID && state->shader->stage == GLSL_STAGE_FRAGMENT)
		glsl_module_decorate(state->module, variable, SPV_DECORATION_FLAT, NULL, 0U);
	if (symbol->builtin == GLSL_BUILTIN_SAMPLE_ID || symbol->builtin == GLSL_BUILTIN_SAMPLE_MASK_IN)
		glsl_module_decorate(state->module, variable, SPV_DECORATION_FLAT, NULL, 0U);

	/* A built-in has no location, only its built-in and invariance (the levels are one per patch). */
	if (value != 0xffffffffU) {
		glsl_module_decorate(state->module, variable, SPV_DECORATION_BUILT_IN, &value, 1U);
		if (symbol->invariant)
			glsl_module_decorate(state->module, variable, SPV_DECORATION_INVARIANT, NULL, 0U);
		if (symbol->builtin == GLSL_BUILTIN_TESS_LEVEL_OUTER || symbol->builtin == GLSL_BUILTIN_TESS_LEVEL_INNER)
			glsl_module_decorate(state->module, variable, SPV_DECORATION_PATCH, NULL, 0U);
		return;
	}

	/* A located variable, named for the link's reflection. */
	value = symbol->location;
	glsl_module_decorate(state->module, variable, SPV_DECORATION_LOCATION, &value, 1U);
	glsl_module_name(state->module, variable, symbol->name);

	/* A tessellation shader's patch variable. */
	if (symbol->patch)
		glsl_module_decorate(state->module, variable, SPV_DECORATION_PATCH, NULL, 0U);

	/* A fragment output that is a dual-source blend's second colour. */
	if (symbol->where == GLSL_VAR_OUTPUT && symbol->explicit_index != 0U) {
		value = symbol->explicit_index;
		glsl_module_decorate(state->module, variable, SPV_DECORATION_INDEX, &value, 1U);
	}

	/* Interpolation (integers into a fragment shader are always flat). */
	if (symbol->interpolation == GLSL_INTERP_FLAT) {
		glsl_module_decorate(state->module, variable, SPV_DECORATION_FLAT, NULL, 0U);
	} else if (symbol->interpolation == GLSL_INTERP_NOPERSPECTIVE) {
		glsl_module_decorate(state->module, variable, SPV_DECORATION_NO_PERSPECTIVE, NULL, 0U);
	}

	/*
	 * A block's members (a geometry shader's input block is an array of
	 * it): flat when the member is (or holds an integer: a fragment
	 * shader's integer input must be).  A struct that is not a block is a
	 * plain variable: its own Flat (the check made it flat when a member
	 * is an integer) covers its members, which take the locations after
	 * its own.
	 */
	if (symbol->type->kind == GLSL_KIND_STRUCT && symbol->type->block != 0U)
		emit_block_members(state, symbol->type);
	if (symbol->type->kind == GLSL_KIND_ARRAY &&
	    symbol->type->element->kind == GLSL_KIND_STRUCT &&
	    symbol->type->element->block != 0U)
		emit_block_members(state, symbol->type->element);

	/* Centroid and invariance. */
	if (symbol->centroid)
		glsl_module_decorate(state->module, variable, SPV_DECORATION_CENTROID, NULL, 0U);
	if (symbol->invariant)
		glsl_module_decorate(state->module, variable, SPV_DECORATION_INVARIANT, NULL, 0U);
}

/*
 * Decorates an in or out block's struct: a Block (Vulkan takes the
 * interpolation of members only in a Block), and its flat and
 * noperspective members.
 */
static void
emit_block_members(
	struct emit_state *state,
	const struct glsl_type *type)
{
	uint32_t structure;
	unsigned index;
	unsigned interpolation;

	/* The Block. */
	structure = glsl_emit_type(state, type);
	glsl_module_decorate(state->module, structure, SPV_DECORATION_BLOCK, NULL, 0U);

	/* Each member. */
	for (index = 0U; index < type->field_count; index++) {
		interpolation = type->fields[index].interpolation;
		if (type->fields[index].type->kind != GLSL_KIND_ARRAY && type->fields[index].type->base != GLSL_BASE_FLOAT)
			interpolation = GLSL_INTERP_FLAT;

		/* The member's interpolation. */
		if (interpolation == GLSL_INTERP_FLAT) {
			glsl_module_member_decorate(state->module, structure, index, SPV_DECORATION_FLAT, 0xffffffffU);
		} else if (interpolation == GLSL_INTERP_NOPERSPECTIVE) {
			glsl_module_member_decorate(state->module, structure, index, SPV_DECORATION_NO_PERSPECTIVE, 0xffffffffU);
		}
	}
}

/* Decorates the gl_PerVertex block of gl_in: a Block whose members are the built-ins of their names. */
static void
emit_per_vertex(
	struct emit_state *state,
	const struct glsl_type *type)
{
	uint32_t structure;
	unsigned index;
	int differs;

	/* The Block. */
	structure = glsl_emit_type(state, type);
	glsl_module_decorate(state->module, structure, SPV_DECORATION_BLOCK, NULL, 0U);

	/* Each member: gl_Position is the one there is. */
	for (index = 0U; index < type->field_count; index++) {
		differs = strcmp(type->fields[index].name, "gl_Position");
		if (differs == 0)
			glsl_module_member_decorate(state->module, structure, index, SPV_DECORATION_BUILT_IN, SPV_BUILT_IN_POSITION);
	}
}

/*
 * Declares a geometry shader's capability and execution modes: its input
 * primitive, its output primitive, the most vertices it emits, and its
 * invocations of each primitive (one unless its layout says).
 */
static void
emit_geometry_modes(
	struct emit_state *state,
	uint32_t main_id)
{
	struct glsl_shader *shader;
	uint32_t operands[3];

	/* The capability. */
	shader = state->shader;
	glsl_emit_capability(state, SPV_CAPABILITY_GEOMETRY, &state->module->geometry);

	/* The input primitive: InputPoints, InputLines, InputLinesAdjacency, Triangles, InputTrianglesAdjacency in order. */
	operands[0] = main_id;
	operands[1] = SPV_MODE_INPUT_POINTS + shader->geometry_input - GLSL_PRIMITIVE_POINTS;
	glsl_words_add(state->module, &state->module->modes, SPV_OP_EXECUTION_MODE, operands, 2U);

	/* The output primitive. */
	operands[1] = SPV_MODE_OUTPUT_TRIANGLE_STRIP;
	if (shader->geometry_output == GLSL_PRIMITIVE_POINTS)
		operands[1] = SPV_MODE_OUTPUT_POINTS;
	if (shader->geometry_output == GLSL_PRIMITIVE_LINE_STRIP)
		operands[1] = SPV_MODE_OUTPUT_LINE_STRIP;
	glsl_words_add(state->module, &state->module->modes, SPV_OP_EXECUTION_MODE, operands, 2U);

	/* The most vertices, and the invocations. */
	operands[1] = SPV_MODE_OUTPUT_VERTICES;
	operands[2] = shader->max_vertices;
	glsl_words_add(state->module, &state->module->modes, SPV_OP_EXECUTION_MODE, operands, 3U);
	operands[1] = SPV_MODE_INVOCATIONS;
	operands[2] = shader->geometry_invocations;
	if (operands[2] == 0U)
		operands[2] = 1U;
	glsl_words_add(state->module, &state->module->modes, SPV_OP_EXECUTION_MODE, operands, 3U);
}

/*
 * Declares a tessellation shader's capability and execution modes: a
 * control shader's output vertices; an evaluation shader's domain,
 * spacing (equal by default), vertex order and point mode.
 *
 * Vulkan 1.0's tessellation domain has its origin at the upper left and
 * GL's at the lower left (VK_KHR_maintenance2's domain origin is not
 * there), so the same vertices of the domain wind the other way: GL's
 * ccw (the default) is Vulkan's cw, as Mesa's Zink turns it without
 * that extension.
 */
static void
emit_tess_modes(
	struct emit_state *state,
	uint32_t main_id)
{
	struct glsl_shader *shader;
	uint32_t operands[3];

	/* The capability. */
	shader = state->shader;
	glsl_emit_capability(state, SPV_CAPABILITY_TESSELLATION, &state->module->tessellation);

	/* A control shader's output patch. */
	operands[0] = main_id;
	if (shader->stage == GLSL_STAGE_TESS_CONTROL) {
		operands[1] = SPV_MODE_OUTPUT_VERTICES;
		operands[2] = shader->tess_vertices;
		glsl_words_add(state->module, &state->module->modes, SPV_OP_EXECUTION_MODE, operands, 3U);
		return;
	}

	/* The domain. */
	operands[1] = SPV_MODE_TRIANGLES;
	if (shader->tess_domain == GLSL_PRIMITIVE_QUADS)
		operands[1] = SPV_MODE_QUADS;
	if (shader->tess_domain == GLSL_PRIMITIVE_ISOLINES)
		operands[1] = SPV_MODE_ISOLINES;
	glsl_words_add(state->module, &state->module->modes, SPV_OP_EXECUTION_MODE, operands, 2U);

	/* The spacing. */
	operands[1] = SPV_MODE_SPACING_EQUAL;
	if (shader->tess_spacing == GLSL_SPACING_FRACTIONAL_EVEN)
		operands[1] = SPV_MODE_SPACING_FRACTIONAL_EVEN;
	if (shader->tess_spacing == GLSL_SPACING_FRACTIONAL_ODD)
		operands[1] = SPV_MODE_SPACING_FRACTIONAL_ODD;
	glsl_words_add(state->module, &state->module->modes, SPV_OP_EXECUTION_MODE, operands, 2U);

	/* The vertex order, turned for Vulkan's domain origin. */
	operands[1] = SPV_MODE_VERTEX_ORDER_CW;
	if (shader->tess_order == GLSL_ORDER_CW)
		operands[1] = SPV_MODE_VERTEX_ORDER_CCW;
	glsl_words_add(state->module, &state->module->modes, SPV_OP_EXECUTION_MODE, operands, 2U);

	/* Points instead of the domain's primitives. */
	if (shader->tess_point_mode) {
		operands[1] = SPV_MODE_POINT_MODE;
		glsl_words_add(state->module, &state->module->modes, SPV_OP_EXECUTION_MODE, operands, 2U);
	}
}

/* Starts a block. */
static void
emit_label(
	struct emit_state *state,
	uint32_t label)
{
	/* OpLabel; the block is open. */
	glsl_words_add(state->module, &state->module->body, SPV_OP_LABEL, &label, 1U);
	state->terminated = 0;
}

/* Ends the open block with a branch (nothing when it already ended). */
static void
emit_branch(
	struct emit_state *state,
	uint32_t target)
{
	/* An ended block takes no second terminator. */
	if (state->terminated)
		return;

	/* OpBranch; the block has ended. */
	glsl_words_add(state->module, &state->module->body, SPV_OP_BRANCH, &target, 1U);
	state->terminated = 1;
}

/* Ends the open block with a conditional branch. */
static void
emit_conditional(
	struct emit_state *state,
	uint32_t condition,
	uint32_t if_true,
	uint32_t if_false)
{
	uint32_t operands[3];

	/* OpBranchConditional; the block has ended. */
	operands[0] = condition;
	operands[1] = if_true;
	operands[2] = if_false;
	glsl_words_add(state->module, &state->module->body, SPV_OP_BRANCH_CONDITIONAL, operands, 3U);
	state->terminated = 1;
}

/* Declares the merge block of the selection the open block starts. */
static void
emit_selection_merge(
	struct emit_state *state,
	uint32_t merge)
{
	uint32_t operands[2];

	/* OpSelectionMerge merge None. */
	operands[0] = merge;
	operands[1] = 0U;
	glsl_words_add(state->module, &state->module->body, SPV_OP_SELECTION_MERGE, operands, 2U);
}

/* Declares the merge and continue blocks of the loop the open block heads. */
static void
emit_loop_merge(
	struct emit_state *state,
	uint32_t merge,
	uint32_t continue_label)
{
	uint32_t operands[3];

	/* OpLoopMerge merge continue None. */
	operands[0] = merge;
	operands[1] = continue_label;
	operands[2] = 0U;
	glsl_words_add(state->module, &state->module->body, SPV_OP_LOOP_MERGE, operands, 3U);
}

/* Emits a list of statements, up to the first that ends the block (the rest cannot run). */
static void
emit_statements(
	struct emit_state *state,
	struct glsl_node *first)
{
	struct glsl_node *statement;

	/* Each statement while the block is open. */
	for (statement = first; statement != NULL; statement = statement->next) {
		if (state->terminated)
			break;
		emit_statement(state, statement);
	}
}

/* Emits a statement. */
static void
emit_statement(
	struct emit_state *state,
	struct glsl_node *node)
{
	/* The kinds of statements. */
	switch (node->kind) {
	case GLSL_N_BLOCK:
		emit_statements(state, node->child[0]);
		break;
	case GLSL_N_DECLARATION:
		emit_declaration(state, node);
		break;
	case GLSL_N_EXPRESSION:
		(void)glsl_emit_expression(state, node->child[0]);
		break;
	case GLSL_N_IF:
		emit_if(state, node);
		break;
	case GLSL_N_FOR:
	case GLSL_N_WHILE:
	case GLSL_N_DO:
		emit_loop(state, node);
		break;
	case GLSL_N_SWITCH:
		emit_switch(state, node);
		break;
	case GLSL_N_RETURN:
		emit_return(state, node);
		break;
	case GLSL_N_BREAK:
		emit_branch(state, state->targets[state->target_count - 1U].merge);
		break;
	case GLSL_N_CONTINUE:
		/* The innermost loop's continue block (the checker allows no switch in between). */
		emit_branch(state, state->targets[state->target_count - 1U].continue_label);
		break;
	case GLSL_N_DISCARD:
		glsl_words_add(state->module, &state->module->body, SPV_OP_KILL, NULL, 0U);
		state->terminated = 1;
		break;
	default:
		break;
	}
}

/* Emits a local declaration: a variable for each name, stored with its initializer or zero. */
static void
emit_declaration(
	struct emit_state *state,
	struct glsl_node *node)
{
	struct glsl_node *variable;
	struct glsl_symbol *symbol;
	struct emit_value value;
	uint32_t operands[2];

	/* Each declared variable (a const one is a constant and needs none). */
	for (variable = node->child[1]; variable != NULL; variable = variable->next) {
		symbol = variable->symbol;
		if (symbol == NULL || symbol->where == GLSL_VAR_CONST)
			continue;

		/* The variable. */
		symbol->id = emit_variable(state, symbol->type);
		symbol->id_storage = SPV_STORAGE_FUNCTION;

		/* Its first value: the initializer, or zero (i915 reads nothing never stored). */
		if (variable->child[1] != NULL) {
			value = glsl_emit_expression(state, variable->child[1]);
		} else {
			value = emit_value_of(emit_zero(state, symbol->type), symbol->type);
		}

		/* The first value stored. */
		operands[0] = symbol->id;
		operands[1] = value.id;
		glsl_words_add(state->module, &state->module->body, SPV_OP_STORE, operands, 2U);
	}
}

/* Emits if and else as a structured selection (a constant condition emits only its branch). */
static void
emit_if(
	struct emit_state *state,
	struct glsl_node *node)
{
	struct emit_value condition;
	uint32_t then_label;
	uint32_t else_label;
	uint32_t merge;
	int then_ended;
	int else_ended;

	/* A constant condition picks its branch now. */
	if (node->child[0]->constant != NULL) {
		if (node->child[0]->constant->values[0].u != 0U) {
			emit_statement(state, node->child[1]);
		} else if (node->child[2] != NULL) {
			emit_statement(state, node->child[2]);
		}

		/* The branch not taken emits nothing. */
		return;
	}

	/* The condition and the labels. */
	condition = glsl_emit_expression(state, node->child[0]);
	then_label = glsl_module_id(state->module);
	merge = glsl_module_id(state->module);
	else_label = merge;
	if (node->child[2] != NULL)
		else_label = glsl_module_id(state->module);
	emit_selection_merge(state, merge);
	emit_conditional(state, condition.id, then_label, else_label);

	/* The then branch. */
	emit_label(state, then_label);
	emit_statement(state, node->child[1]);
	then_ended = state->terminated;
	emit_branch(state, merge);

	/* The else branch. */
	else_ended = 0;
	if (node->child[2] != NULL) {
		emit_label(state, else_label);
		emit_statement(state, node->child[2]);
		else_ended = state->terminated;
		emit_branch(state, merge);
	}

	/* The merge block, which nothing reaches when both branches left the construct. */
	emit_label(state, merge);
	if (then_ended && else_ended) {
		glsl_words_add(state->module, &state->module->body, SPV_OP_UNREACHABLE, &merge, 0U);
		state->terminated = 1;
	}
}

/*
 * Emits for, while and do-while as a structured loop: a header, a block
 * that tests the condition, the body, the continue block (the increment,
 * or do-while's condition) and the merge block.
 */
static void
emit_loop(
	struct emit_state *state,
	struct glsl_node *node)
{
	struct glsl_node *condition_node;
	struct glsl_node *body;
	struct emit_value condition;
	uint32_t header;
	uint32_t check;
	uint32_t body_label;
	uint32_t continue_label;
	uint32_t merge;

	/* A for loop's init comes first. */
	condition_node = NULL;
	body = NULL;
	if (node->kind == GLSL_N_FOR) {
		if (node->child[0] != NULL)
			emit_statement(state, node->child[0]);
		condition_node = node->child[1];
		body = node->child[3];
	} else if (node->kind == GLSL_N_WHILE) {
		condition_node = node->child[0];
		body = node->child[1];
	} else {
		body = node->child[0];
	}

	/* The labels, and the header. */
	header = glsl_module_id(state->module);
	check = glsl_module_id(state->module);
	body_label = glsl_module_id(state->module);
	continue_label = glsl_module_id(state->module);
	merge = glsl_module_id(state->module);
	emit_branch(state, header);
	emit_label(state, header);
	emit_loop_merge(state, merge, continue_label);

	/* The condition of for and while, tested before the body; do-while goes to the body first. */
	if (node->kind == GLSL_N_DO) {
		emit_branch(state, body_label);
	} else {
		emit_branch(state, check);
		emit_label(state, check);
		if (condition_node != NULL) {
			condition = glsl_emit_expression(state, condition_node);
			emit_conditional(state, condition.id, body_label, merge);
		} else {
			emit_branch(state, body_label);
		}
	}

	/* The body, as a target of break and continue. */
	emit_label(state, body_label);
	emit_push_target(state, EMIT_TARGET_LOOP, merge, continue_label);
	emit_statement(state, body);
	emit_branch(state, continue_label);
	emit_pop_target(state);

	/* The continue block: for's increment, or do-while's condition. */
	emit_label(state, continue_label);
	if (node->kind == GLSL_N_FOR && node->child[2] != NULL)
		(void)glsl_emit_expression(state, node->child[2]);
	if (node->kind == GLSL_N_DO) {
		condition = glsl_emit_expression(state, node->child[1]);
		emit_conditional(state, condition.id, header, merge);
	} else {
		emit_branch(state, header);
	}

	/* The merge block; an inlined function that returned inside the loop goes on out. */
	emit_label(state, merge);
	emit_returned_check(state);
}

/*
 * Emits a switch as a loop that runs once: each group of labels guards
 * its statements with "fallen through or matched", and break leaves the
 * loop (no OpSwitch, which i915's compiler lacks).
 */
static void
emit_switch(
	struct emit_state *state,
	struct glsl_node *node)
{
	const struct glsl_type *bool_type;
	struct glsl_node *statement;
	struct glsl_node *group_end;
	struct emit_value selector;
	uint32_t operands[2];
	uint32_t header;
	uint32_t body_label;
	uint32_t continue_label;
	uint32_t merge;
	uint32_t fallen;
	uint32_t matched_any;
	uint32_t condition;
	uint32_t run;
	uint32_t next;
	uint32_t comparison;

	/* The selector, and the flag that a group ran (execution falls into the next). */
	bool_type = glsl_type_scalar(GLSL_BASE_BOOL);
	selector = glsl_emit_expression(state, node->child[0]);
	fallen = emit_variable(state, bool_type);
	operands[0] = fallen;
	operands[1] = glsl_module_bool(state->module, glsl_emit_type(state, bool_type), 0);
	glsl_words_add(state->module, &state->module->body, SPV_OP_STORE, operands, 2U);

	/* Whether any case label matches (the default runs otherwise). */
	matched_any = glsl_module_bool(state->module, glsl_emit_type(state, bool_type), 0);
	for (statement = node->child[1]->child[0]; statement != NULL; statement = statement->next) {
		if (statement->kind != GLSL_N_CASE)
			continue;
		operands[0] = selector.id;
		operands[1] = emit_constant(state, statement->child[0]->constant);
		comparison = glsl_emit_op(state, SPV_OP_I_EQUAL, glsl_emit_type(state, bool_type), operands, 2U);
		operands[0] = matched_any;
		operands[1] = comparison;
		matched_any = glsl_emit_op(state, SPV_OP_LOGICAL_OR, glsl_emit_type(state, bool_type), operands, 2U);
	}

	/* The loop that runs once. */
	header = glsl_module_id(state->module);
	body_label = glsl_module_id(state->module);
	continue_label = glsl_module_id(state->module);
	merge = glsl_module_id(state->module);
	emit_branch(state, header);
	emit_label(state, header);
	emit_loop_merge(state, merge, continue_label);
	emit_branch(state, body_label);
	emit_label(state, body_label);
	emit_push_target(state, EMIT_TARGET_SWITCH, merge, continue_label);

	/* Each group: its labels, then its statements up to the next label. */
	statement = node->child[1]->child[0];
	while (statement != NULL) {
		/* The group's condition, from its labels. */
		condition = glsl_module_bool(state->module, glsl_emit_type(state, bool_type), 0);
		while (statement != NULL && (statement->kind == GLSL_N_CASE || statement->kind == GLSL_N_DEFAULT)) {
			comparison = emit_case_condition(state, statement, selector.id, fallen, matched_any);
			operands[0] = condition;
			operands[1] = comparison;
			condition = glsl_emit_op(state, SPV_OP_LOGICAL_OR, glsl_emit_type(state, bool_type), operands, 2U);
			statement = statement->next;
		}

		/* The group's statements run when it matched or execution fell into it; they mark the fall. */
		group_end = statement;
		while (group_end != NULL && group_end->kind != GLSL_N_CASE && group_end->kind != GLSL_N_DEFAULT)
			group_end = group_end->next;
		run = glsl_module_id(state->module);
		next = glsl_module_id(state->module);
		emit_selection_merge(state, next);
		emit_conditional(state, condition, run, next);
		emit_label(state, run);
		operands[0] = fallen;
		operands[1] = glsl_module_bool(state->module, glsl_emit_type(state, bool_type), 1);
		glsl_words_add(state->module, &state->module->body, SPV_OP_STORE, operands, 2U);
		for (; statement != group_end; statement = statement->next) {
			if (state->terminated)
				continue;
			emit_statement(state, statement);
		}

		/* The next group follows. */
		emit_branch(state, next);
		emit_label(state, next);
	}

	/* The end of the body leaves the loop; the continue block is never reached. */
	emit_branch(state, merge);
	emit_pop_target(state);
	emit_label(state, continue_label);
	emit_branch(state, header);

	/* The merge block; an inlined function that returned inside the switch goes on out. */
	emit_label(state, merge);
	emit_returned_check(state);
}

/* Returns the condition one label adds: the selector equals the case, or (default) nothing matched; or fallen through. */
static uint32_t
emit_case_condition(
	struct emit_state *state,
	struct glsl_node *label,
	uint32_t selector,
	uint32_t fallen,
	uint32_t matched_any)
{
	const struct glsl_type *bool_type;
	uint32_t operands[2];
	uint32_t matched;
	uint32_t was_fallen;
	uint32_t condition;

	/* Whether the label matches. */
	bool_type = glsl_type_scalar(GLSL_BASE_BOOL);
	if (label->kind == GLSL_N_CASE) {
		operands[0] = selector;
		operands[1] = emit_constant(state, label->child[0]->constant);
		matched = glsl_emit_op(state, SPV_OP_I_EQUAL, glsl_emit_type(state, bool_type), operands, 2U);
	} else {
		operands[0] = matched_any;
		matched = glsl_emit_op(state, SPV_OP_LOGICAL_NOT, glsl_emit_type(state, bool_type), operands, 1U);
	}

	/* Or execution fell into it. */
	operands[0] = fallen;
	was_fallen = glsl_emit_op(state, SPV_OP_LOAD, glsl_emit_type(state, bool_type), operands, 1U);
	operands[0] = matched;
	operands[1] = was_fallen;
	condition = glsl_emit_op(state, SPV_OP_LOGICAL_OR, glsl_emit_type(state, bool_type), operands, 2U);

	/* Succeeded: the condition. */
	return condition;
}

/* Emits return: from main a return; from an inlined function the value stored and the function left. */
static void
emit_return(
	struct emit_state *state,
	struct glsl_node *node)
{
	struct emit_value value;
	struct emit_frame *frame;
	uint32_t operands[2];

	/* main returns, having captured its outputs. */
	frame = state->frame;
	if (frame == NULL) {
		emit_capture(state);
		glsl_words_add(state->module, &state->module->body, SPV_OP_RETURN, NULL, 0U);
		state->terminated = 1;
		return;
	}

	/* The value goes to the function's result. */
	if (node->child[0] != NULL) {
		value = glsl_emit_expression(state, node->child[0]);
		operands[0] = frame->result;
		operands[1] = value.id;
		glsl_words_add(state->module, &state->module->body, SPV_OP_STORE, operands, 2U);
	}

	/* A function returning only at its end simply ends. */
	if (frame->returned == 0U)
		return;

	/* Otherwise it says it returned and leaves the innermost construct (loops check the flag on the way out). */
	operands[0] = frame->returned;
	operands[1] = glsl_module_bool(state->module, glsl_emit_type(state, glsl_type_scalar(GLSL_BASE_BOOL)), 1);
	glsl_words_add(state->module, &state->module->body, SPV_OP_STORE, operands, 2U);
	emit_branch(state, state->targets[state->target_count - 1U].merge);
}

/*
 * Declares the storage buffer a vertex shader writes the outputs it
 * captures into (transform feedback): a buffer block of words at
 * descriptor set 0 and GLSL_CAPTURE_BINDING, and finds the vertex's and
 * the instance's numbers.
 */
static void
emit_capture_buffer(
	struct emit_state *state)
{
	struct glsl_symbol *symbol;
	uint32_t operands[3];
	uint32_t word_type;
	uint32_t words;
	uint32_t structure;
	uint32_t pointer;
	uint32_t value;

	/* A run of words, four bytes apart, the block's one member at offset 0. */
	word_type = glsl_emit_type(state, glsl_type_scalar(GLSL_BASE_UINT));
	operands[0] = 0U;
	operands[1] = word_type;
	words = glsl_module_declare(state->module, SPV_OP_TYPE_RUNTIME_ARRAY, operands, 2U, 1);
	value = 4U;
	glsl_module_decorate(state->module, words, SPV_DECORATION_ARRAY_STRIDE, &value, 1U);
	operands[0] = 0U;
	operands[1] = words;
	structure = glsl_module_declare(state->module, SPV_OP_TYPE_STRUCT, operands, 2U, 1);
	glsl_module_decorate(state->module, structure, SPV_DECORATION_BUFFER_BLOCK, NULL, 0U);
	glsl_module_member_decorate(state->module, structure, 0U, SPV_DECORATION_OFFSET, 0U);

	/* The variable, at its descriptor set and binding. */
	pointer = glsl_emit_pointer(state, SPV_STORAGE_UNIFORM, structure);
	state->capture_buffer = glsl_module_id(state->module);
	operands[0] = pointer;
	operands[1] = state->capture_buffer;
	operands[2] = SPV_STORAGE_UNIFORM;
	glsl_words_add(state->module, &state->module->globals, SPV_OP_VARIABLE, operands, 3U);
	value = 0U;
	glsl_module_decorate(state->module, state->capture_buffer, SPV_DECORATION_DESCRIPTOR_SET, &value, 1U);
	value = GLSL_CAPTURE_BINDING;
	glsl_module_decorate(state->module, state->capture_buffer, SPV_DECORATION_BINDING, &value, 1U);

	/* The vertex's and the instance's numbers, or a geometry shader's primitive's and invocation's (the link made them used). */
	for (symbol = state->shader->globals; symbol != NULL; symbol = symbol->next_global) {
		if (symbol->builtin == GLSL_BUILTIN_VERTEX_ID)
			state->vertex_id = symbol;
		if (symbol->builtin == GLSL_BUILTIN_INSTANCE_ID)
			state->instance_id = symbol;
		if (symbol->builtin == GLSL_BUILTIN_PRIMITIVE_ID_IN)
			state->primitive_id = symbol;
		if (symbol->builtin == GLSL_BUILTIN_INVOCATION_ID && symbol->where == GLSL_VAR_INPUT)
			state->invocation_id = symbol;
	}

	/* A geometry shader keeps its own count of what it emits, from main's start. */
	if (state->capture_geometry)
		emit_capture_start(state);
}

/*
 * Reports whether a stage counts what it emits in a capture buffer
 * (glsl.h): a geometry shader whose outputs are captured, or that emits
 * on a vertex stream other than 0 (whose primitives only it can count).
 */
int
glsl_capture_counted(
	const struct glsl_shader *shader,
	unsigned capture_count)
{
	/* Only a geometry shader. */
	if (shader->stage != GLSL_STAGE_GEOMETRY)
		return 0;

	/* One that captures. */
	if (capture_count != 0U)
		return 1;

	/* One that emits on another stream. */
	if ((shader->geometry_streams & ~1U) != 0U)
		return 1;

	/* Nothing to count. */
	return 0;
}

/*
 * Starts a capturing geometry shader's main: its variables (the
 * invocation's number, taken from the header's count of invocations; the
 * next vertex's place 0; every stream's next vertex starting a strip;
 * no vertex in any strip), and the vertices of a primitive it outputs.
 */
static void
emit_capture_start(
	struct emit_state *state)
{
	const struct glsl_type *word;
	uint32_t token;
	unsigned stream;

	/* The vertices of the output primitive: a point's 1, a line strip's 2, a triangle strip's 3. */
	state->capture_primitive_vertices = 1U;
	if (state->shader->geometry_output == GLSL_PRIMITIVE_LINE_STRIP)
		state->capture_primitive_vertices = 2U;
	if (state->shader->geometry_output == GLSL_PRIMITIVE_TRIANGLE_STRIP)
		state->capture_primitive_vertices = 3U;

	/* The variables. */
	word = glsl_type_scalar(GLSL_BASE_UINT);
	state->capture_token = emit_variable(state, word);
	state->capture_sequence = emit_variable(state, word);
	state->capture_starts = emit_variable(state, word);
	for (stream = 0U; stream < 4U; stream++)
		state->capture_strip[stream] = emit_variable(state, word);

	/* The invocation's number: the header's count of invocations, one more. */
	token = emit_capture_add(state, emit_uint(state, GLSL_CAPTURE_INVOCATIONS), emit_uint(state, 1U));
	emit_capture_set(state, state->capture_token, token);

	/* Nothing emitted yet: place 0, each stream at a strip's start with no vertex in it. */
	emit_capture_set(state, state->capture_sequence, emit_uint(state, 0U));
	emit_capture_set(state, state->capture_starts, emit_uint(state, 0xfU));
	for (stream = 0U; stream < 4U; stream++)
		emit_capture_set(state, state->capture_strip[stream], emit_uint(state, 0U));
}

/*
 * Writes what a capturing geometry shader emits on a stream, before the
 * vertex is emitted (glsl.h): the stream's strip has one more vertex (the
 * first of a new one when the stream's strip ended), and the stream's
 * count of primitives one more when that makes a primitive; on a stream
 * with captured outputs, a record of the next number the header's
 * counter gives (the one past the room when there is none left) with the
 * key and the stream's outputs; then the next vertex's place.
 */
void
glsl_emit_capture_vertex(
	struct emit_state *state,
	unsigned stream)
{
	struct glsl_symbol *symbol;
	uint32_t operands[3];
	uint32_t word_type;
	uint32_t bool_type;
	uint32_t starts;
	uint32_t start;
	uint32_t strip;
	uint32_t made;
	uint32_t slot;
	uint32_t room;
	uint32_t fits;
	uint32_t base;
	uint32_t key;
	uint32_t sequence;
	uint32_t value;
	unsigned offset;
	unsigned index;
	int captured;

	/* Nothing for a shader that does not count what it emits. */
	if (!state->capture_geometry)
		return;
	word_type = glsl_emit_type(state, glsl_type_scalar(GLSL_BASE_UINT));
	bool_type = glsl_emit_type(state, glsl_type_scalar(GLSL_BASE_BOOL));

	/* Whether the vertex starts the stream's strip (its bit of the starts), which then no longer does. */
	starts = glsl_emit_op(state, SPV_OP_LOAD, word_type, &state->capture_starts, 1U);
	operands[0] = starts;
	operands[1] = emit_uint(state, stream);
	value = glsl_emit_op(state, SPV_OP_SHIFT_RIGHT_LOGICAL, word_type, operands, 2U);
	operands[0] = value;
	operands[1] = emit_uint(state, 1U);
	start = glsl_emit_op(state, SPV_OP_BITWISE_AND, word_type, operands, 2U);
	operands[0] = starts;
	operands[1] = emit_uint(state, ~(1U << stream));
	value = glsl_emit_op(state, SPV_OP_BITWISE_AND, word_type, operands, 2U);
	emit_capture_set(state, state->capture_starts, value);

	/* The vertices in the strip: 1 at its start, else one more. */
	strip = glsl_emit_op(state, SPV_OP_LOAD, word_type, &state->capture_strip[stream], 1U);
	operands[0] = strip;
	operands[1] = emit_uint(state, 1U);
	strip = glsl_emit_op(state, SPV_OP_I_ADD, word_type, operands, 2U);
	operands[0] = start;
	operands[1] = emit_uint(state, 0U);
	value = glsl_emit_op(state, SPV_OP_I_NOT_EQUAL, bool_type, operands, 2U);
	operands[0] = value;
	operands[1] = emit_uint(state, 1U);
	operands[2] = strip;
	strip = glsl_emit_op(state, SPV_OP_SELECT, word_type, operands, 3U);
	emit_capture_set(state, state->capture_strip[stream], strip);

	/* A primitive made once the strip has a primitive's vertices: the stream's count one more (0 added otherwise). */
	operands[0] = strip;
	operands[1] = emit_uint(state, state->capture_primitive_vertices);
	value = glsl_emit_op(state, SPV_OP_U_GREATER_THAN_EQUAL, bool_type, operands, 2U);
	operands[0] = value;
	operands[1] = emit_uint(state, 1U);
	operands[2] = emit_uint(state, 0U);
	made = glsl_emit_op(state, SPV_OP_SELECT, word_type, operands, 3U);
	(void)emit_capture_add(state, emit_uint(state, GLSL_CAPTURE_GENERATED + stream), made);

	/* Whether the stream has captured outputs. */
	captured = 0;
	for (index = 0U; index < state->capture_count; index++) {
		if (state->captures[index].symbol->stream == stream)
			captured = 1;
	}

	/* A record when it has: the counter's next number, or the one past the room when the room is full. */
	sequence = glsl_emit_op(state, SPV_OP_LOAD, word_type, &state->capture_sequence, 1U);
	if (captured) {
		slot = emit_capture_add(state, emit_uint(state, GLSL_CAPTURE_RECORDS), emit_uint(state, 1U));
		value = emit_capture_word(state, emit_uint(state, GLSL_CAPTURE_ROOM));
		room = glsl_emit_op(state, SPV_OP_LOAD, word_type, &value, 1U);
		operands[0] = slot;
		operands[1] = room;
		fits = glsl_emit_op(state, SPV_OP_U_LESS_THAN, bool_type, operands, 2U);
		operands[0] = fits;
		operands[1] = slot;
		operands[2] = room;
		slot = glsl_emit_op(state, SPV_OP_SELECT, word_type, operands, 3U);

		/* The record's first word: after the header, records of the key and the outputs. */
		operands[0] = slot;
		operands[1] = emit_uint(state, GLSL_CAPTURE_KEY + state->capture_stride);
		value = glsl_emit_op(state, SPV_OP_I_MUL, word_type, operands, 2U);
		operands[0] = value;
		operands[1] = emit_uint(state, GLSL_CAPTURE_GEOMETRY_HEADER);
		base = glsl_emit_op(state, SPV_OP_I_ADD, word_type, operands, 2U);

		/* The key: the primitive, the invocation, the invocation's number, and the place, stream and strip start. */
		emit_capture_store(state, base, emit_capture_number(state, state->primitive_id));
		operands[0] = base;
		operands[1] = emit_uint(state, 1U);
		emit_capture_store(state, glsl_emit_op(state, SPV_OP_I_ADD, word_type, operands, 2U),
				   emit_capture_number(state, state->invocation_id));
		operands[1] = emit_uint(state, 2U);
		emit_capture_store(state, glsl_emit_op(state, SPV_OP_I_ADD, word_type, operands, 2U),
				   glsl_emit_op(state, SPV_OP_LOAD, word_type, &state->capture_token, 1U));
		operands[0] = start;
		operands[1] = emit_uint(state, 31U);
		key = glsl_emit_op(state, SPV_OP_SHIFT_LEFT_LOGICAL, word_type, operands, 2U);
		operands[0] = key;
		operands[1] = emit_uint(state, stream << GLSL_CAPTURE_STREAM_SHIFT);
		key = glsl_emit_op(state, SPV_OP_BITWISE_OR, word_type, operands, 2U);
		operands[0] = key;
		operands[1] = sequence;
		key = glsl_emit_op(state, SPV_OP_BITWISE_OR, word_type, operands, 2U);
		operands[0] = base;
		operands[1] = emit_uint(state, 3U);
		emit_capture_store(state, glsl_emit_op(state, SPV_OP_I_ADD, word_type, operands, 2U), key);

		/* The stream's outputs after the key, component by component, at their places. */
		operands[0] = base;
		operands[1] = emit_uint(state, GLSL_CAPTURE_KEY);
		base = glsl_emit_op(state, SPV_OP_I_ADD, word_type, operands, 2U);
		for (index = 0U; index < state->capture_count; index++) {
			symbol = state->captures[index].symbol;
			if (symbol->stream != stream)
				continue;
			offset = state->captures[index].offset;
			value = glsl_emit_op(state, SPV_OP_LOAD, glsl_emit_type(state, symbol->type), &symbol->id, 1U);
			emit_capture_value(state, value, symbol->type, base, &offset);
		}
	}

	/* The next vertex's place. */
	operands[0] = sequence;
	operands[1] = emit_uint(state, 1U);
	value = glsl_emit_op(state, SPV_OP_I_ADD, word_type, operands, 2U);
	emit_capture_set(state, state->capture_sequence, value);
}

/*
 * Ends a capturing geometry shader's strip on a stream: the stream's next
 * vertex starts another.
 */
void
glsl_emit_capture_end(
	struct emit_state *state,
	unsigned stream)
{
	uint32_t operands[2];
	uint32_t word_type;
	uint32_t value;

	/* Nothing for a shader that does not count what it emits. */
	if (!state->capture_geometry)
		return;

	/* The stream's bit of the starts set. */
	word_type = glsl_emit_type(state, glsl_type_scalar(GLSL_BASE_UINT));
	operands[0] = glsl_emit_op(state, SPV_OP_LOAD, word_type, &state->capture_starts, 1U);
	operands[1] = emit_uint(state, 1U << stream);
	value = glsl_emit_op(state, SPV_OP_BITWISE_OR, word_type, operands, 2U);
	emit_capture_set(state, state->capture_starts, value);
}

/* Returns a pointer to a word of the capture buffer of an index (a uint id). */
static uint32_t
emit_capture_word(
	struct emit_state *state,
	uint32_t index)
{
	uint32_t operands[3];
	uint32_t word_type;
	uint32_t pointer;

	/* The buffer's member 0, the word of the index. */
	word_type = glsl_emit_type(state, glsl_type_scalar(GLSL_BASE_UINT));
	operands[0] = state->capture_buffer;
	operands[1] = glsl_emit_int(state, 0);
	operands[2] = index;
	pointer = glsl_emit_op(state, SPV_OP_ACCESS_CHAIN, glsl_emit_pointer(state, SPV_STORAGE_UNIFORM, word_type), operands, 3U);

	/* Succeeded: the word's pointer. */
	return pointer;
}

/* Adds a value to a word of the capture buffer atomically (on the device); returns the word as it was. */
static uint32_t
emit_capture_add(
	struct emit_state *state,
	uint32_t index,
	uint32_t value)
{
	uint32_t operands[4];
	uint32_t word_type;
	uint32_t before;

	/* OpAtomicIAdd on the device's scope, relaxed. */
	word_type = glsl_emit_type(state, glsl_type_scalar(GLSL_BASE_UINT));
	operands[0] = emit_capture_word(state, index);
	operands[1] = emit_uint(state, 1U);
	operands[2] = emit_uint(state, 0U);
	operands[3] = value;
	before = glsl_emit_op(state, SPV_OP_ATOMIC_I_ADD, word_type, operands, 4U);

	/* Succeeded: the word before. */
	return before;
}

/* Stores a uint value into a word of the capture buffer. */
static void
emit_capture_store(
	struct emit_state *state,
	uint32_t index,
	uint32_t value)
{
	uint32_t operands[2];

	/* OpStore through the word's pointer. */
	operands[0] = emit_capture_word(state, index);
	operands[1] = value;
	glsl_words_add(state->module, &state->module->body, SPV_OP_STORE, operands, 2U);
}

/* Stores a value into one of main's variables. */
static void
emit_capture_set(
	struct emit_state *state,
	uint32_t variable,
	uint32_t value)
{
	uint32_t operands[2];

	/* OpStore into the variable. */
	operands[0] = variable;
	operands[1] = value;
	glsl_words_add(state->module, &state->module->body, SPV_OP_STORE, operands, 2U);
}

/* Returns an int input's value (a primitive's or an invocation's number) as a uint, 0 for none. */
static uint32_t
emit_capture_number(
	struct emit_state *state,
	const struct glsl_symbol *symbol)
{
	uint32_t word_type;
	uint32_t value;

	/* None is 0. */
	word_type = glsl_emit_type(state, glsl_type_scalar(GLSL_BASE_UINT));
	if (symbol == NULL)
		return emit_uint(state, 0U);

	/* The input's bits. */
	value = glsl_emit_op(state, SPV_OP_LOAD, glsl_emit_type(state, glsl_type_scalar(GLSL_BASE_INT)), &symbol->id, 1U);
	value = glsl_emit_op(state, SPV_OP_BITCAST, word_type, &value, 1U);

	/* Succeeded: the number. */
	return value;
}

/*
 * Writes the outputs a vertex shader captures into its record in the
 * storage buffer (instance * vertices + vertex, after the header), before
 * main returns.
 */
static void
emit_capture(
	struct emit_state *state)
{
	struct glsl_symbol *symbol;
	uint32_t operands[3];
	uint32_t word_type;
	uint32_t int_type;
	uint32_t word_pointer;
	uint32_t vertices;
	uint32_t vertex;
	uint32_t instance;
	uint32_t record;
	uint32_t base;
	uint32_t value;
	unsigned offset;
	unsigned index;

	/* Nothing captured (a geometry shader captures as it emits), or the numbers missing. */
	if (state->capture_count == 0U || state->shader->stage != GLSL_STAGE_VERTEX)
		return;
	if (state->vertex_id == NULL || state->instance_id == NULL)
		return;

	/* The vertices each instance has: header word 0. */
	word_type = glsl_emit_type(state, glsl_type_scalar(GLSL_BASE_UINT));
	int_type = glsl_emit_type(state, glsl_type_scalar(GLSL_BASE_INT));
	word_pointer = glsl_emit_pointer(state, SPV_STORAGE_UNIFORM, word_type);
	operands[0] = state->capture_buffer;
	operands[1] = glsl_emit_int(state, 0);
	operands[2] = glsl_emit_int(state, 0);
	value = glsl_emit_op(state, SPV_OP_ACCESS_CHAIN, word_pointer, operands, 3U);
	vertices = glsl_emit_op(state, SPV_OP_LOAD, word_type, &value, 1U);

	/* The vertex's and the instance's numbers as words. */
	value = glsl_emit_op(state, SPV_OP_LOAD, int_type, &state->vertex_id->id, 1U);
	vertex = glsl_emit_op(state, SPV_OP_BITCAST, word_type, &value, 1U);
	value = glsl_emit_op(state, SPV_OP_LOAD, int_type, &state->instance_id->id, 1U);
	instance = glsl_emit_op(state, SPV_OP_BITCAST, word_type, &value, 1U);

	/* The record: instance * vertices + vertex, its first word after the header. */
	operands[0] = instance;
	operands[1] = vertices;
	value = glsl_emit_op(state, SPV_OP_I_MUL, word_type, operands, 2U);
	operands[0] = value;
	operands[1] = vertex;
	record = glsl_emit_op(state, SPV_OP_I_ADD, word_type, operands, 2U);
	operands[0] = record;
	operands[1] = glsl_module_constant(state->module, word_type, state->capture_stride);
	value = glsl_emit_op(state, SPV_OP_I_MUL, word_type, operands, 2U);
	operands[0] = value;
	operands[1] = glsl_module_constant(state->module, word_type, GLSL_CAPTURE_HEADER);
	base = glsl_emit_op(state, SPV_OP_I_ADD, word_type, operands, 2U);

	/* Each output's value, component by component, at its place. */
	for (index = 0U; index < state->capture_count; index++) {
		symbol = state->captures[index].symbol;
		offset = state->captures[index].offset;
		value = glsl_emit_op(state, SPV_OP_LOAD, glsl_emit_type(state, symbol->type), &symbol->id, 1U);
		emit_capture_value(state, value, symbol->type, base, &offset);
	}
}

/* Writes a captured value of a type (a number, vector, matrix or array of them) word by word from a place in the record on. */
static void
emit_capture_value(
	struct emit_state *state,
	uint32_t value,
	const struct glsl_type *type,
	uint32_t base,
	unsigned *offset)
{
	const struct glsl_type *part;
	uint32_t operands[3];
	uint32_t word_type;
	uint32_t word;
	uint32_t index;
	uint32_t pointer;
	unsigned count;
	unsigned item;

	/* An array's elements, a matrix's columns, a vector's components, each in turn. */
	count = 0U;
	part = NULL;
	if (type->kind == GLSL_KIND_ARRAY) {
		count = type->length;
		part = type->element;
	} else if (type->kind == GLSL_KIND_MATRIX) {
		count = type->columns;
		part = glsl_type_vector(type->base, type->components);
	} else if (type->kind == GLSL_KIND_VECTOR) {
		count = type->components;
		part = glsl_type_scalar(type->base);
	}

	/* Each part in turn. */
	for (item = 0U; item < count; item++) {
		operands[0] = value;
		operands[1] = item;
		word = glsl_emit_op(state, SPV_OP_COMPOSITE_EXTRACT, glsl_emit_type(state, part), operands, 2U);
		emit_capture_value(state, word, part, base, offset);
	}

	/* A composite is written through its parts. */
	if (count != 0U)
		return;

	/* A number's bits as a word (a float's and an int's bit cast). */
	word_type = glsl_emit_type(state, glsl_type_scalar(GLSL_BASE_UINT));
	word = value;
	if (type->base != GLSL_BASE_UINT)
		word = glsl_emit_op(state, SPV_OP_BITCAST, word_type, &value, 1U);

	/* Stored at the record's word. */
	operands[0] = base;
	operands[1] = glsl_module_constant(state->module, word_type, *offset);
	index = glsl_emit_op(state, SPV_OP_I_ADD, word_type, operands, 2U);
	operands[0] = state->capture_buffer;
	operands[1] = glsl_emit_int(state, 0);
	operands[2] = index;
	pointer = glsl_emit_op(state, SPV_OP_ACCESS_CHAIN, glsl_emit_pointer(state, SPV_STORAGE_UNIFORM, word_type), operands, 3U);
	operands[0] = pointer;
	operands[1] = word;
	glsl_words_add(state->module, &state->module->body, SPV_OP_STORE, operands, 2U);
	(*offset)++;
}

/* Pushes a break target (a loop, a switch, or an inlined function's once-loop). */
static void
emit_push_target(
	struct emit_state *state,
	unsigned kind,
	uint32_t merge,
	uint32_t continue_label)
{
	struct emit_target *target;

	/* The target on top (the checker limits the nesting to 64). */
	if (state->target_count == 64U)
		return;
	target = &state->targets[state->target_count];
	state->target_count++;
	target->kind = kind;
	target->merge = merge;
	target->continue_label = continue_label;
	target->frame = state->frame;

	/* A continue inside a switch goes to the loop around it (never used: the checker refuses it). */
	if (kind == EMIT_TARGET_SWITCH && state->target_count > 1U)
		target->continue_label = state->targets[state->target_count - 2U].continue_label;
}

/* Pops the innermost break target. */
static void
emit_pop_target(
	struct emit_state *state)
{
	/* The one below becomes the innermost. */
	if (state->target_count > 0U)
		state->target_count--;
}

/*
 * After a loop or switch inside an inlined function with early returns:
 * when the function returned inside it, leaves the construct around it
 * too (a branch to its merge block).
 */
static void
emit_returned_check(
	struct emit_state *state)
{
	struct emit_target *outer;
	uint32_t operands[1];
	uint32_t returned;
	uint32_t leave;
	uint32_t stay;

	/* Only inside a function with early returns, in one of its constructs, in an open block. */
	if (state->frame == NULL || state->frame->returned == 0U || state->target_count == 0U || state->terminated)
		return;
	outer = &state->targets[state->target_count - 1U];
	if (outer->frame != state->frame)
		return;

	/* if (returned) leave the construct around. */
	operands[0] = state->frame->returned;
	returned = glsl_emit_op(state, SPV_OP_LOAD, glsl_emit_type(state, glsl_type_scalar(GLSL_BASE_BOOL)), operands, 1U);
	leave = glsl_module_id(state->module);
	stay = glsl_module_id(state->module);
	emit_selection_merge(state, stay);
	emit_conditional(state, returned, leave, stay);
	emit_label(state, leave);
	emit_branch(state, outer->merge);
	emit_label(state, stay);
}

/*
 * Builds the path of an expression that names a variable or a part of
 * one: the variable, an access chain and a swizzle.  Returns -1 for any
 * other expression (a value computed, or a constant).
 */
static int
emit_path(
	struct emit_state *state,
	struct glsl_node *node,
	struct emit_path *path)
{
	struct glsl_symbol *symbol;
	struct emit_value index;
	int status;

	/* A variable. */
	if (node->kind == GLSL_N_IDENTIFIER) {
		symbol = node->symbol;
		if (symbol == NULL || symbol->where == GLSL_VAR_CONST)
			return -1;
		memset(path, 0, sizeof(*path));
		path->type = symbol->type;

		/* A uniform block (by its instance name) is read as a laid-out struct. */
		if (symbol->where == GLSL_VAR_BLOCK) {
			path->base = symbol->id;
			path->storage = SPV_STORAGE_UNIFORM;
			path->block = EMIT_LAYOUT_STD140;
			return 0;
		}

		/* A storage block (by its instance name), a std430 struct (ws101-p008). */
		if (symbol->where == GLSL_VAR_BUFFER) {
			path->base = symbol->id;
			path->storage = SPV_STORAGE_UNIFORM;
			path->block = EMIT_LAYOUT_STD430;
			return 0;
		}

		/* A member of a storage block without an instance name (ws101-p008). */
		if (symbol->where == GLSL_VAR_BUFFER_MEMBER) {
			path->base = symbol->block->id;
			path->storage = SPV_STORAGE_UNIFORM;
			path->block = EMIT_LAYOUT_STD430;
			path->indices[0] = glsl_emit_int(state, (int32_t)symbol->member);
			path->index_count = 1U;
			return 0;
		}

		/* A member of a uniform block without an instance name is a member of that block. */
		if (symbol->where == GLSL_VAR_BLOCK_MEMBER) {
			path->base = symbol->block->id;
			path->storage = SPV_STORAGE_UNIFORM;
			path->block = EMIT_LAYOUT_STD140;
			path->indices[0] = glsl_emit_int(state, (int32_t)symbol->member);
			path->index_count = 1U;
			return 0;
		}

		/* A uniform other than a sampler (or an array of them) is a member of the default block. */
		if (symbol->where == GLSL_VAR_UNIFORM && state->uniforms[symbol->uniform].sampler == 0U) {
			path->base = state->block;
			path->storage = SPV_STORAGE_UNIFORM;
			path->block = EMIT_LAYOUT_STD140;
			path->indices[0] = glsl_emit_int(state, state->members[symbol->uniform]);
			path->index_count = 1U;
			return 0;
		}

		/* Anything else is its own variable. */
		path->base = symbol->id;
		path->storage = symbol->id_storage;
		return 0;
	}

	/* A member or a swizzle of a path. */
	if (node->kind == GLSL_N_FIELD) {
		status = emit_path(state, node->child[0], path);
		if (status != 0 || path->swizzle_count != 0U)
			return -1;

		/* A swizzle ends the path. */
		if ((node->flags & GLSL_NODE_SWIZZLE) != 0U) {
			memcpy(path->swizzle, node->swizzle, sizeof(path->swizzle));
			path->swizzle_count = node->swizzle_count;
			path->vector = path->type;
			path->type = node->type;
			return 0;
		}

		/* A struct member extends the chain. */
		if (path->index_count == 16U)
			return -1;
		path->indices[path->index_count] = glsl_emit_int(state, (int32_t)node->field);
		path->index_count++;
		path->type = node->type;
		return 0;
	}

	/* An element of a path. */
	if (node->kind == GLSL_N_INDEX) {
		status = emit_path(state, node->child[0], path);
		if (status != 0 || path->swizzle_count != 0U || path->index_count == 16U)
			return -1;

		/* The index, constant or computed. */
		index = glsl_emit_expression(state, node->child[1]);
		path->indices[path->index_count] = index.id;
		path->index_count++;
		path->type = node->type;
		return 0;
	}

	/* Not a path. */
	return -1;
}

/* Returns a pointer to where a path's chain leads (the variable itself when there is no chain). */
static uint32_t
emit_chain(
	struct emit_state *state,
	const struct emit_path *path)
{
	uint32_t operands[GLSL_MAX_OPERANDS];
	uint32_t target;
	uint32_t pointer;

	/* No chain: the variable. */
	if (path->index_count == 0U)
		return path->base;

	/* The type reached, as the storage holds it. */
	if (path->block == EMIT_LAYOUT_STD430) {
		target = emit_layout430_type(state, path->type);
	} else if (path->block) {
		target = emit_layout_type(state, path->type);
	} else {
		target = glsl_emit_type(state, path->type);
	}

	/* OpAccessChain pointer base indices. */
	operands[0] = path->base;
	memcpy(operands + 1, path->indices, path->index_count * sizeof(uint32_t));
	pointer = glsl_emit_op(state, SPV_OP_ACCESS_CHAIN, glsl_emit_pointer(state, path->storage, target), operands, path->index_count + 1U);

	/* Succeeded: the pointer. */
	return pointer;
}

/* Reads a path: the value, and the swizzle of it. */
static struct emit_value
emit_load(
	struct emit_state *state,
	const struct emit_path *path)
{
	struct emit_path whole;
	struct emit_value value;
	uint32_t pointer;

	/* The path without its swizzle reaches the vector the swizzle picks from. */
	whole = *path;
	if (path->swizzle_count != 0U) {
		whole.swizzle_count = 0U;
		whole.type = path->vector;
	}

	/* A path into the block reads its leaves; any other is a load through the chain. */
	if (whole.block) {
		value = emit_load_block(state, &whole);
	} else {
		pointer = emit_chain(state, &whole);
		value = emit_value_of(glsl_emit_op(state, SPV_OP_LOAD, glsl_emit_type(state, whole.type), &pointer, 1U), whole.type);
	}

	/* The swizzle's components of it. */
	if (path->swizzle_count != 0U)
		value = emit_swizzle(state, value, path->swizzle, path->swizzle_count);

	/* Succeeded: the value. */
	return value;
}

/*
 * Reads a path into the default uniform block: a leaf is loaded (a bool
 * converted from its uint), a struct or an array is built from its
 * leaves in the local type.
 */
static struct emit_value
emit_load_block(
	struct emit_state *state,
	const struct emit_path *path)
{
	const struct glsl_type *type;
	const struct glsl_type *stored;
	struct emit_path part;
	struct emit_value value;
	uint32_t parts[EMIT_MAX_PARTS];
	uint32_t operands[2];
	uint32_t pointer;
	unsigned count;
	unsigned index;

	/* A leaf: loaded, a bool from its uint (a storage block's matrix is read column by column, ws101-p008). */
	type = path->type;
	if (type->kind == GLSL_KIND_SCALAR ||
	    type->kind == GLSL_KIND_VECTOR ||
	    (type->kind == GLSL_KIND_MATRIX && path->block != EMIT_LAYOUT_STD430)) {
		pointer = emit_chain(state, path);
		stored = type;
		if (type->base == GLSL_BASE_BOOL)
			stored = glsl_type_with_base(type, GLSL_BASE_UINT);
		value = emit_value_of(glsl_emit_op(state, SPV_OP_LOAD, glsl_emit_type(state, stored), &pointer, 1U), stored);
		if (type->base == GLSL_BASE_BOOL) {
			operands[0] = value.id;
			operands[1] = glsl_emit_splat(state, glsl_module_constant(state->module, glsl_emit_type(state, glsl_type_scalar(GLSL_BASE_UINT)), 0U), stored);
			value = emit_value_of(glsl_emit_op(state, SPV_OP_I_NOT_EQUAL, glsl_emit_type(state, type), operands, 2U), type);
		}

		/* Succeeded: the leaf. */
		return value;
	}

	/* An aggregate: each part read on its own. */
	count = type->length;
	if (type->kind == GLSL_KIND_STRUCT)
		count = type->field_count;
	if (type->kind == GLSL_KIND_MATRIX)
		count = type->columns;
	if (count > EMIT_MAX_PARTS)
		count = EMIT_MAX_PARTS;
	for (index = 0U; index < count; index++) {
		part = *path;
		if (part.index_count == 16U)
			break;
		part.indices[part.index_count] = glsl_emit_int(state, (int32_t)index);
		part.index_count++;
		if (type->kind == GLSL_KIND_STRUCT) {
			part.type = type->fields[index].type;
		} else if (type->kind == GLSL_KIND_MATRIX) {
			part.type = glsl_type_column(type);
		} else {
			part.type = type->element;
		}

		/* The part read. */
		value = emit_load_block(state, &part);
		parts[index] = value.id;
	}

	/* The aggregate in the local type. */
	value = emit_value_of(glsl_emit_op(state, SPV_OP_COMPOSITE_CONSTRUCT, glsl_emit_type(state, type), parts, count), type);
	return value;
}

/* Writes a value through a path: a component through its own chain, a swizzle as load, shuffle and store. */
static void
emit_store(
	struct emit_state *state,
	const struct emit_path *path,
	struct emit_value value)
{
	struct emit_path whole;
	struct emit_value old;
	uint32_t operands[GLSL_MAX_OPERANDS];
	uint32_t pointer;
	const struct glsl_type *vector;
	unsigned component;
	unsigned index;

	/* A storage block's memory takes its own layout (ws101-p008). */
	if (path->block == EMIT_LAYOUT_STD430 && path->swizzle_count == 0U) {
		emit_store_block(state, path, value);
		return;
	}

	/* No swizzle: a store through the chain. */
	if (path->swizzle_count == 0U) {
		pointer = emit_chain(state, path);
		operands[0] = pointer;
		operands[1] = value.id;
		glsl_words_add(state->module, &state->module->body, SPV_OP_STORE, operands, 2U);
		return;
	}

	/* The vector the swizzle writes into. */
	whole = *path;
	whole.swizzle_count = 0U;
	whole.type = path->vector;

	/* One component: a chain to it. */
	if (path->swizzle_count == 1U && whole.index_count < 16U) {
		whole.indices[whole.index_count] = glsl_emit_int(state, (int32_t)path->swizzle[0]);
		whole.index_count++;
		whole.type = path->type;
		if (whole.block == EMIT_LAYOUT_STD430) {
			emit_store_block(state, &whole, value);
			return;
		}
		pointer = emit_chain(state, &whole);
		operands[0] = pointer;
		operands[1] = value.id;
		glsl_words_add(state->module, &state->module->body, SPV_OP_STORE, operands, 2U);
		return;
	}

	/* Several components: the vector read, the new components shuffled in (no insert, which i915 lacks). */
	old = emit_load(state, &whole);
	vector = old.type;
	operands[0] = old.id;
	operands[1] = value.id;
	for (component = 0U; component < vector->components; component++) {
		operands[2U + component] = component;
		for (index = 0U; index < path->swizzle_count; index++) {
			if (path->swizzle[index] == component)
				operands[2U + component] = vector->components + index;
		}
	}

	/* The shuffled vector. */
	old = emit_value_of(glsl_emit_op(state, SPV_OP_VECTOR_SHUFFLE, glsl_emit_type(state, vector), operands, 2U + vector->components), vector);

	/* The vector written back. */
	if (whole.block == EMIT_LAYOUT_STD430) {
		emit_store_block(state, &whole, old);
		return;
	}
	pointer = emit_chain(state, &whole);
	operands[0] = pointer;
	operands[1] = old.id;
	glsl_words_add(state->module, &state->module->body, SPV_OP_STORE, operands, 2U);
}

/* Takes the components a swizzle names from a vector value. */
static struct emit_value
emit_swizzle(
	struct emit_state *state,
	struct emit_value value,
	const unsigned *swizzle,
	unsigned count)
{
	const struct glsl_type *type;
	uint32_t operands[6];
	unsigned index;
	struct emit_value result;

	/* One component is an extract. */
	type = glsl_type_vector(value.type->base, count);
	if (count == 1U) {
		result = emit_value_of(glsl_emit_extract(state, value, swizzle[0]), type);
		return result;
	}

	/* More are a shuffle of the vector with itself. */
	operands[0] = value.id;
	operands[1] = value.id;
	for (index = 0U; index < count; index++)
		operands[2U + index] = swizzle[index];
	result = emit_value_of(glsl_emit_op(state, SPV_OP_VECTOR_SHUFFLE, glsl_emit_type(state, type), operands, 2U + count), type);

	/* Succeeded: the swizzled value. */
	return result;
}

/*
 * Reads a member, a swizzle or an element of a value that is not a
 * variable (a call's result, an arithmetic result).
 */
static struct emit_value
emit_selection(
	struct emit_state *state,
	struct glsl_node *node)
{
	struct emit_value object;
	struct emit_value index;
	struct emit_value result;
	uint32_t operands[3];
	uint32_t temporary;
	uint32_t pointer;

	/* The object's value. */
	object = glsl_emit_expression(state, node->child[0]);

	/* A swizzle of a vector (a variable's swizzle is a path, but a computed one comes here). */
	if (node->kind == GLSL_N_FIELD && (node->flags & GLSL_NODE_SWIZZLE) != 0U) {
		result = emit_swizzle(state, object, node->swizzle, node->swizzle_count);
		return result;
	}

	/* A struct member. */
	if (node->kind == GLSL_N_FIELD) {
		result = emit_value_of(glsl_emit_extract(state, object, node->field), node->type);
		return result;
	}

	/* An element at a constant index. */
	if (node->child[1]->constant != NULL) {
		result = emit_value_of(glsl_emit_extract(state, object, node->child[1]->constant->values[0].u), node->type);
		return result;
	}

	/* A vector's component at a computed index. */
	index = glsl_emit_expression(state, node->child[1]);
	if (object.type->kind == GLSL_KIND_VECTOR) {
		operands[0] = object.id;
		operands[1] = index.id;
		result = emit_value_of(glsl_emit_op(state, SPV_OP_VECTOR_EXTRACT_DYNAMIC, glsl_emit_type(state, node->type), operands, 2U), node->type);
		return result;
	}

	/* An array's element or a matrix's column at a computed index: through a temporary variable. */
	temporary = emit_variable(state, object.type);
	operands[0] = temporary;
	operands[1] = object.id;
	glsl_words_add(state->module, &state->module->body, SPV_OP_STORE, operands, 2U);
	operands[0] = temporary;
	operands[1] = index.id;
	pointer = glsl_emit_op(state, SPV_OP_ACCESS_CHAIN, glsl_emit_pointer(state, SPV_STORAGE_FUNCTION, glsl_emit_type(state, node->type)), operands, 2U);
	result = emit_value_of(glsl_emit_op(state, SPV_OP_LOAD, glsl_emit_type(state, node->type), &pointer, 1U), node->type);

	/* Succeeded: the element. */
	return result;
}

/* Emits a unary operator: negation, not, complement (plus changes nothing). */
static struct emit_value
emit_unary(
	struct emit_state *state,
	struct glsl_node *node)
{
	struct emit_value operand;
	struct emit_value result;
	struct emit_value zero;
	uint32_t opcode;
	int floating;

	/* The operand. */
	operand = glsl_emit_expression(state, node->child[0]);

	/* Plus. */
	if (node->op == GLSL_P_PLUS)
		return operand;

	/* Not and complement. */
	if (node->op == GLSL_P_BANG || node->op == GLSL_P_TILDE) {
		opcode = SPV_OP_LOGICAL_NOT;
		if (node->op == GLSL_P_TILDE)
			opcode = SPV_OP_NOT;
		result = emit_value_of(glsl_emit_op(state, opcode, glsl_emit_type(state, node->type), &operand.id, 1U), node->type);
		return result;
	}

	/* A matrix negates column by column (0 minus each). */
	if (operand.type->kind == GLSL_KIND_MATRIX) {
		zero = emit_value_of(emit_zero(state, operand.type), operand.type);
		result = emit_columns(state, SPV_OP_F_SUB, zero, operand, operand.type);
		return result;
	}

	/* A float, a double or an integer negated. */
	opcode = SPV_OP_S_NEGATE;
	floating = glsl_base_floating(operand.type->base);
	if (floating)
		opcode = SPV_OP_F_NEGATE;
	result = emit_value_of(glsl_emit_op(state, opcode, glsl_emit_type(state, node->type), &operand.id, 1U), node->type);

	/* Succeeded: the negated value. */
	return result;
}

/* Emits ++ and --: the variable read, changed by one, written; the value before (postfix) or after. */
static struct emit_value
emit_increment(
	struct emit_state *state,
	struct glsl_node *node)
{
	struct emit_path path;
	struct emit_value old;
	struct emit_value one;
	struct emit_value changed;
	struct glsl_constant *constant;
	unsigned op;
	unsigned index;
	int status;

	/* The variable's path and value. */
	status = emit_path(state, node->child[0], &path);
	if (status != 0)
		return emit_value_of(emit_zero(state, node->type), node->type);
	old = emit_load(state, &path);

	/* One of the same type. */
	constant = glsl_constant_new(state->module->arena, node->type);
	for (index = 0U; index < constant->count; index++) {
		if (node->type->base == GLSL_BASE_FLOAT) {
			constant->values[index].f = 1.0f;
		} else if (node->type->base == GLSL_BASE_DOUBLE) {
			constant->values[index].d = 1.0;
		} else {
			constant->values[index].u = 1U;
		}
	}

	/* The one. */
	one = emit_value_of(emit_constant(state, constant), node->type);

	/* The new value, written back. */
	op = GLSL_P_PLUS;
	if (node->kind == GLSL_N_PREDEC || node->kind == GLSL_N_POSTDEC)
		op = GLSL_P_MINUS;
	changed = emit_arithmetic(state, op, old, one, node->type);
	emit_store(state, &path, changed);

	/* Prefix gives the new value, postfix the old one. */
	if (node->kind == GLSL_N_POSTINC || node->kind == GLSL_N_POSTDEC)
		return old;

	/* Succeeded: the new value. */
	return changed;
}

/* Emits a binary operator. */
static struct emit_value
emit_binary(
	struct emit_state *state,
	struct glsl_node *node)
{
	struct emit_value left;
	struct emit_value right;
	struct emit_value result;
	uint32_t operands[2];
	uint32_t id;

	/* The logical operators, which may skip their right side. */
	if (node->op == GLSL_P_AND_AND || node->op == GLSL_P_OR_OR) {
		result = emit_logical(state, node);
		return result;
	}

	/* Both sides, left first. */
	left = glsl_emit_expression(state, node->child[0]);
	right = glsl_emit_expression(state, node->child[1]);

	/* The comparisons and the logical exclusive or give a bool. */
	switch (node->op) {
	case GLSL_P_EQ:
		id = emit_equal(state, left, right);
		result = emit_value_of(id, node->type);
		return result;
	case GLSL_P_NE:
		id = emit_equal(state, left, right);
		id = glsl_emit_op(state, SPV_OP_LOGICAL_NOT, glsl_emit_type(state, node->type), &id, 1U);
		result = emit_value_of(id, node->type);
		return result;
	case GLSL_P_LT:
	case GLSL_P_GT:
	case GLSL_P_LE:
	case GLSL_P_GE:
		id = emit_compare(state, node->op, left, right);
		result = emit_value_of(id, node->type);
		return result;
	case GLSL_P_XOR_XOR:
		operands[0] = left.id;
		operands[1] = right.id;
		id = glsl_emit_op(state, SPV_OP_LOGICAL_NOT_EQUAL, glsl_emit_type(state, node->type), operands, 2U);
		result = emit_value_of(id, node->type);
		return result;
	default:
		break;
	}

	/* Arithmetic. */
	result = emit_arithmetic(state, node->op, left, right, node->type);
	return result;
}

/*
 * Emits an arithmetic operator (+ - * / % & | ^ << >>) on two values of
 * one base, whatever their shapes: scalars with vectors and matrices,
 * and the linear algebra products.
 */
static struct emit_value
emit_arithmetic(
	struct emit_state *state,
	unsigned op,
	struct emit_value left,
	struct emit_value right,
	const struct glsl_type *type)
{
	struct emit_value result;
	uint32_t operands[2];
	uint32_t opcode;
	uint32_t result_type;
	int floating;

	/* The products with a matrix. */
	result_type = glsl_emit_type(state, type);
	if (op == GLSL_P_STAR && (left.type->kind == GLSL_KIND_MATRIX || right.type->kind == GLSL_KIND_MATRIX)) {
		opcode = 0U;
		operands[0] = left.id;
		operands[1] = right.id;
		if (left.type->kind == GLSL_KIND_MATRIX && right.type->kind == GLSL_KIND_MATRIX) {
			opcode = SPV_OP_MATRIX_TIMES_MATRIX;
		} else if (left.type->kind == GLSL_KIND_MATRIX && right.type->kind == GLSL_KIND_VECTOR) {
			opcode = SPV_OP_MATRIX_TIMES_VECTOR;
		} else if (left.type->kind == GLSL_KIND_VECTOR) {
			opcode = SPV_OP_VECTOR_TIMES_MATRIX;
		} else if (left.type->kind == GLSL_KIND_MATRIX) {
			opcode = SPV_OP_MATRIX_TIMES_SCALAR;
		} else {
			/* A scalar times a matrix is the matrix times the scalar. */
			opcode = SPV_OP_MATRIX_TIMES_SCALAR;
			operands[0] = right.id;
			operands[1] = left.id;
		}

		/* Succeeded: the product. */
		result = emit_value_of(glsl_emit_op(state, opcode, result_type, operands, 2U), type);
		return result;
	}

	/* Any other operator on a matrix works column by column. */
	opcode = emit_arithmetic_opcode(op, type->base);
	if (type->kind == GLSL_KIND_MATRIX) {
		result = emit_columns(state, opcode, left, right, type);
		return result;
	}

	/* A float or double vector times a scalar has its own instruction. */
	floating = glsl_base_floating(type->base);
	if (op == GLSL_P_STAR && floating && type->kind == GLSL_KIND_VECTOR) {
		if (left.type->kind == GLSL_KIND_SCALAR || right.type->kind == GLSL_KIND_SCALAR) {
			operands[0] = left.id;
			operands[1] = right.id;
			if (left.type->kind == GLSL_KIND_SCALAR) {
				operands[0] = right.id;
				operands[1] = left.id;
			}

			/* Succeeded: the scaled vector. */
			result = emit_value_of(glsl_emit_op(state, SPV_OP_VECTOR_TIMES_SCALAR, result_type, operands, 2U), type);
			return result;
		}
	}

	/* A scalar with a vector becomes a vector (a shift's right side keeps its own base). */
	operands[0] = left.id;
	operands[1] = right.id;
	if (left.type->kind == GLSL_KIND_SCALAR && type->kind == GLSL_KIND_VECTOR)
		operands[0] = glsl_emit_splat(state, left.id, glsl_type_with_base(type, left.type->base));
	if (right.type->kind == GLSL_KIND_SCALAR && type->kind == GLSL_KIND_VECTOR)
		operands[1] = glsl_emit_splat(state, right.id, glsl_type_with_base(type, right.type->base));

	/* Succeeded: the operation. */
	result = emit_value_of(glsl_emit_op(state, opcode, result_type, operands, 2U), type);
	return result;
}

/* Returns the instruction of an arithmetic operator on a base. */
static uint32_t
emit_arithmetic_opcode(
	unsigned op,
	unsigned base)
{
	/* A double takes the float instructions. */
	if (base == GLSL_BASE_DOUBLE)
		base = GLSL_BASE_FLOAT;

	/* The operators. */
	switch (op) {
	case GLSL_P_PLUS:
		if (base == GLSL_BASE_FLOAT)
			return SPV_OP_F_ADD;
		return SPV_OP_I_ADD;
	case GLSL_P_MINUS:
		if (base == GLSL_BASE_FLOAT)
			return SPV_OP_F_SUB;
		return SPV_OP_I_SUB;
	case GLSL_P_STAR:
		if (base == GLSL_BASE_FLOAT)
			return SPV_OP_F_MUL;
		return SPV_OP_I_MUL;
	case GLSL_P_SLASH:
		if (base == GLSL_BASE_FLOAT)
			return SPV_OP_F_DIV;
		if (base == GLSL_BASE_INT)
			return SPV_OP_S_DIV;
		return SPV_OP_U_DIV;
	case GLSL_P_PERCENT:
		if (base == GLSL_BASE_INT)
			return SPV_OP_S_REM;
		return SPV_OP_U_MOD;
	case GLSL_P_AMP:
		return SPV_OP_BITWISE_AND;
	case GLSL_P_BAR:
		return SPV_OP_BITWISE_OR;
	case GLSL_P_CARET:
		return SPV_OP_BITWISE_XOR;
	case GLSL_P_SHL:
		return SPV_OP_SHIFT_LEFT_LOGICAL;
	case GLSL_P_SHR:
		if (base == GLSL_BASE_INT)
			return SPV_OP_SHIFT_RIGHT_ARITHMETIC;
		return SPV_OP_SHIFT_RIGHT_LOGICAL;
	default:
		break;
	}

	/* Not an arithmetic operator. */
	return SPV_OP_F_ADD;
}

/* Applies a vector instruction to a matrix column by column (a scalar side stands for every column). */
static struct emit_value
emit_columns(
	struct emit_state *state,
	uint32_t opcode,
	struct emit_value left,
	struct emit_value right,
	const struct glsl_type *type)
{
	const struct glsl_type *column_type;
	struct emit_value result;
	uint32_t columns[4];
	uint32_t operands[2];
	unsigned column;

	/* Each column. */
	column_type = glsl_type_column(type);
	for (column = 0U; column < type->columns; column++) {
		if (left.type->kind == GLSL_KIND_MATRIX) {
			operands[0] = glsl_emit_extract(state, left, column);
		} else {
			operands[0] = glsl_emit_splat(state, left.id, column_type);
		}

		/* The right side's column, or the scalar as one. */
		if (right.type->kind == GLSL_KIND_MATRIX) {
			operands[1] = glsl_emit_extract(state, right, column);
		} else {
			operands[1] = glsl_emit_splat(state, right.id, column_type);
		}

		/* The operation on the column. */
		columns[column] = glsl_emit_op(state, opcode, glsl_emit_type(state, column_type), operands, 2U);
	}

	/* Succeeded: the columns made a matrix again. */
	result = emit_value_of(glsl_emit_op(state, SPV_OP_COMPOSITE_CONSTRUCT, glsl_emit_type(state, type), columns, type->columns), type);
	return result;
}

/* Emits < > <= >= of two scalars of one base. */
static uint32_t
emit_compare(
	struct emit_state *state,
	unsigned op,
	struct emit_value left,
	struct emit_value right)
{
	static const uint32_t floats[4] = { SPV_OP_F_ORD_LESS_THAN, SPV_OP_F_ORD_GREATER_THAN, SPV_OP_F_ORD_LESS_THAN_EQUAL, SPV_OP_F_ORD_GREATER_THAN_EQUAL };
	static const uint32_t ints[4] = { SPV_OP_S_LESS_THAN, SPV_OP_S_GREATER_THAN, SPV_OP_S_LESS_THAN_EQUAL, SPV_OP_S_GREATER_THAN_EQUAL };
	static const uint32_t uints[4] = { SPV_OP_U_LESS_THAN, SPV_OP_U_GREATER_THAN, SPV_OP_U_LESS_THAN_EQUAL, SPV_OP_U_GREATER_THAN_EQUAL };
	uint32_t operands[2];
	uint32_t opcode;
	unsigned which;
	uint32_t id;

	/* Which comparison. */
	which = 3U;
	if (op == GLSL_P_LT)
		which = 0U;
	if (op == GLSL_P_GT)
		which = 1U;
	if (op == GLSL_P_LE)
		which = 2U;

	/* Its instruction for the base. */
	opcode = uints[which];
	if (left.type->base == GLSL_BASE_FLOAT || left.type->base == GLSL_BASE_DOUBLE)
		opcode = floats[which];
	if (left.type->base == GLSL_BASE_INT)
		opcode = ints[which];

	/* Succeeded: the bool. */
	operands[0] = left.id;
	operands[1] = right.id;
	id = glsl_emit_op(state, opcode, glsl_emit_type(state, glsl_type_scalar(GLSL_BASE_BOOL)), operands, 2U);
	return id;
}

/* Emits whether two values of one type are equal (every component, member and element). */
static uint32_t
emit_equal(
	struct emit_state *state,
	struct emit_value left,
	struct emit_value right)
{
	const struct glsl_type *type;
	const struct glsl_type *part_type;
	struct emit_value left_part;
	struct emit_value right_part;
	uint32_t operands[2];
	uint32_t opcode;
	uint32_t result;
	uint32_t part;
	unsigned count;
	unsigned index;

	/* Scalars and vectors compare component by component, then all must be equal. */
	type = left.type;
	if (type->kind == GLSL_KIND_SCALAR || type->kind == GLSL_KIND_VECTOR) {
		opcode = SPV_OP_I_EQUAL;
		if (type->base == GLSL_BASE_FLOAT || type->base == GLSL_BASE_DOUBLE)
			opcode = SPV_OP_F_ORD_EQUAL;
		if (type->base == GLSL_BASE_BOOL)
			opcode = SPV_OP_LOGICAL_EQUAL;
		operands[0] = left.id;
		operands[1] = right.id;
		result = glsl_emit_op(state, opcode, glsl_emit_type(state, glsl_type_vector(GLSL_BASE_BOOL, type->components)), operands, 2U);
		result = glsl_emit_all(state, result, type->components, 0);
		return result;
	}

	/* Matrices, arrays and structs: each part equal. */
	count = type->columns;
	if (type->kind == GLSL_KIND_ARRAY)
		count = type->length;
	if (type->kind == GLSL_KIND_STRUCT)
		count = type->field_count;
	result = glsl_module_bool(state->module, glsl_emit_type(state, glsl_type_scalar(GLSL_BASE_BOOL)), 1);
	for (index = 0U; index < count; index++) {
		part_type = glsl_type_column(type);
		if (type->kind == GLSL_KIND_STRUCT)
			part_type = type->fields[index].type;
		left_part = emit_value_of(glsl_emit_extract(state, left, index), part_type);
		right_part = emit_value_of(glsl_emit_extract(state, right, index), part_type);
		part = emit_equal(state, left_part, right_part);
		operands[0] = result;
		operands[1] = part;
		result = glsl_emit_op(state, SPV_OP_LOGICAL_AND, glsl_emit_type(state, glsl_type_scalar(GLSL_BASE_BOOL)), operands, 2U);
	}

	/* Succeeded: the bool. */
	return result;
}

/*
 * Emits && and ||: one instruction when the right side has no side
 * effects, otherwise the right side only runs when it decides.
 */
static struct emit_value
emit_logical(
	struct emit_state *state,
	struct glsl_node *node)
{
	const struct glsl_type *type;
	struct emit_value left;
	struct emit_value right;
	struct emit_value result;
	uint32_t operands[2];
	uint32_t variable;
	uint32_t condition;
	uint32_t opcode;
	uint32_t run;
	uint32_t merge;

	/* The left side. */
	type = node->type;
	left = glsl_emit_expression(state, node->child[0]);

	/* A right side without side effects is evaluated anyway. */
	if ((node->child[1]->flags & GLSL_NODE_SIDE_EFFECTS) == 0U) {
		right = glsl_emit_expression(state, node->child[1]);
		operands[0] = left.id;
		operands[1] = right.id;
		opcode = SPV_OP_LOGICAL_OR;
		if (node->op == GLSL_P_AND_AND)
			opcode = SPV_OP_LOGICAL_AND;
		result = emit_value_of(glsl_emit_op(state, opcode, glsl_emit_type(state, type), operands, 2U), type);
		return result;
	}

	/* Otherwise the result is the left side, replaced by the right side when the left one does not decide. */
	variable = emit_variable(state, type);
	operands[0] = variable;
	operands[1] = left.id;
	glsl_words_add(state->module, &state->module->body, SPV_OP_STORE, operands, 2U);
	condition = left.id;
	if (node->op == GLSL_P_OR_OR)
		condition = glsl_emit_op(state, SPV_OP_LOGICAL_NOT, glsl_emit_type(state, type), &left.id, 1U);
	run = glsl_module_id(state->module);
	merge = glsl_module_id(state->module);
	emit_selection_merge(state, merge);
	emit_conditional(state, condition, run, merge);
	emit_label(state, run);
	right = glsl_emit_expression(state, node->child[1]);
	operands[0] = variable;
	operands[1] = right.id;
	glsl_words_add(state->module, &state->module->body, SPV_OP_STORE, operands, 2U);
	emit_branch(state, merge);
	emit_label(state, merge);

	/* Succeeded: the result read back. */
	result = emit_value_of(glsl_emit_op(state, SPV_OP_LOAD, glsl_emit_type(state, type), &variable, 1U), type);
	return result;
}

/* Emits an assignment: the path, the value (with the operator on the old value), the store. */
static struct emit_value
emit_assign(
	struct emit_state *state,
	struct glsl_node *node)
{
	struct emit_path path;
	struct emit_value old;
	struct emit_value value;
	int status;

	/* Where to write. */
	status = emit_path(state, node->child[0], &path);
	if (status != 0) {
		value = glsl_emit_expression(state, node->child[1]);
		return value;
	}

	/* The value: the right side, or the operator on the old value and the right side. */
	if (node->op == GLSL_P_ASSIGN) {
		value = glsl_emit_expression(state, node->child[1]);
	} else {
		old = emit_load(state, &path);
		value = glsl_emit_expression(state, node->child[1]);
		value = emit_arithmetic(state, emit_assign_operator(node->op), old, value, node->type);
	}

	/* Succeeded: stored, and the value is the expression's. */
	emit_store(state, &path, value);
	return value;
}

/* Returns the operator of a compound assignment. */
static unsigned
emit_assign_operator(
	unsigned op)
{
	/* The operator the assignment applies. */
	switch (op) {
	case GLSL_P_ADD_ASSIGN:
		return GLSL_P_PLUS;
	case GLSL_P_SUB_ASSIGN:
		return GLSL_P_MINUS;
	case GLSL_P_MUL_ASSIGN:
		return GLSL_P_STAR;
	case GLSL_P_DIV_ASSIGN:
		return GLSL_P_SLASH;
	case GLSL_P_MOD_ASSIGN:
		return GLSL_P_PERCENT;
	case GLSL_P_SHL_ASSIGN:
		return GLSL_P_SHL;
	case GLSL_P_SHR_ASSIGN:
		return GLSL_P_SHR;
	case GLSL_P_AND_ASSIGN:
		return GLSL_P_AMP;
	case GLSL_P_OR_ASSIGN:
		return GLSL_P_BAR;
	default:
		break;
	}

	/* ^=. */
	return GLSL_P_CARET;
}

/*
 * Emits "condition ? a : b": a select when both choices are cheap and
 * safe to evaluate (scalars and vectors), otherwise a selection storing
 * the chosen one in a variable.
 */
static struct emit_value
emit_ternary(
	struct emit_state *state,
	struct glsl_node *node)
{
	const struct glsl_type *type;
	struct emit_value condition;
	struct emit_value choice;
	struct emit_value result;
	uint32_t operands[3];
	uint32_t variable;
	uint32_t first;
	uint32_t second;
	uint32_t merge;
	int cheap;

	/* The condition. */
	type = node->type;
	condition = glsl_emit_expression(state, node->child[0]);

	/* A select of two cheap scalars or vectors (the condition a vector of its size). */
	cheap = 0;
	if (type->kind == GLSL_KIND_SCALAR || type->kind == GLSL_KIND_VECTOR) {
		cheap = emit_is_cheap(node->child[1]);
		if (cheap)
			cheap = emit_is_cheap(node->child[2]);
	}

	/* Two cheap choices are a select. */
	if (cheap) {
		operands[0] = glsl_emit_splat(state, condition.id, glsl_type_with_base(type, GLSL_BASE_BOOL));
		operands[1] = glsl_emit_expression(state, node->child[1]).id;
		operands[2] = glsl_emit_expression(state, node->child[2]).id;
		result = emit_value_of(glsl_emit_op(state, SPV_OP_SELECT, glsl_emit_type(state, type), operands, 3U), type);
		return result;
	}

	/* Otherwise a selection, each branch storing its choice. */
	variable = emit_variable(state, type);
	first = glsl_module_id(state->module);
	second = glsl_module_id(state->module);
	merge = glsl_module_id(state->module);
	emit_selection_merge(state, merge);
	emit_conditional(state, condition.id, first, second);
	emit_label(state, first);
	choice = glsl_emit_expression(state, node->child[1]);
	operands[0] = variable;
	operands[1] = choice.id;
	glsl_words_add(state->module, &state->module->body, SPV_OP_STORE, operands, 2U);
	emit_branch(state, merge);
	emit_label(state, second);
	choice = glsl_emit_expression(state, node->child[2]);
	operands[0] = variable;
	operands[1] = choice.id;
	glsl_words_add(state->module, &state->module->body, SPV_OP_STORE, operands, 2U);
	emit_branch(state, merge);
	emit_label(state, merge);

	/* Succeeded: the choice read back. */
	result = emit_value_of(glsl_emit_op(state, SPV_OP_LOAD, glsl_emit_type(state, type), &variable, 1U), type);
	return result;
}

/* Reports whether an expression is cheap and safe to evaluate when its value may be thrown away. */
static int
emit_is_cheap(
	const struct glsl_node *node)
{
	const struct glsl_node *argument;
	int cheap;

	/* Constants and variables. */
	if (node->constant != NULL || node->kind == GLSL_N_IDENTIFIER)
		return 1;

	/* Side effects are never thrown away. */
	if ((node->flags & GLSL_NODE_SIDE_EFFECTS) != 0U)
		return 0;

	/* Selections, and subscripts at constant indices, of cheap objects. */
	if (node->kind == GLSL_N_FIELD || node->kind == GLSL_N_CONVERT || node->kind == GLSL_N_UNARY) {
		cheap = emit_is_cheap(node->child[0]);
		return cheap;
	}

	/* A subscript at a constant index of a cheap object. */
	if (node->kind == GLSL_N_INDEX) {
		if (node->child[1]->constant == NULL)
			return 0;
		cheap = emit_is_cheap(node->child[0]);
		return cheap;
	}

	/* Arithmetic of cheap operands (no division: an integer division by zero is not safe). */
	if (node->kind == GLSL_N_BINARY) {
		if (node->op == GLSL_P_SLASH || node->op == GLSL_P_PERCENT)
			return 0;
		cheap = emit_is_cheap(node->child[0]);
		if (cheap)
			cheap = emit_is_cheap(node->child[1]);
		return cheap;
	}

	/* Constructors of cheap arguments. */
	if (node->kind == GLSL_N_CALL && (node->flags & GLSL_NODE_CONSTRUCTOR) != 0U) {
		for (argument = node->child[1]; argument != NULL; argument = argument->next) {
			cheap = emit_is_cheap(argument);
			if (!cheap)
				return 0;
		}

		/* Succeeded: every argument is cheap. */
		return 1;
	}

	/* Anything else is not. */
	return 0;
}

/* Emits a call: a constructor, a built-in or an inlined user function. */
static struct emit_value
emit_call(
	struct emit_state *state,
	struct glsl_node *node)
{
	struct emit_value arguments[8];
	struct emit_value result;
	struct glsl_node *argument;
	unsigned count;

	/* A constructor. */
	if ((node->flags & GLSL_NODE_CONSTRUCTOR) != 0U) {
		result = emit_constructor(state, node);
		return result;
	}

	/* A call through a subroutine uniform runs the subroutine it selects. */
	if ((node->flags & GLSL_NODE_SUBROUTINE_CALL) != 0U) {
		result = emit_subroutine_call(state, node);
		return result;
	}

	/* A user function is inlined. */
	if (node->function != NULL) {
		result = emit_inline(state, node);
		return result;
	}

	/* A compute shader's atomics and barriers (ws101-p008): an atomic's first argument is its memory, not a value. */
	if (node->builtin != NULL &&
	    node->builtin->operation == GLSL_BI_SPECIAL &&
	    node->builtin->number >= GLSL_SPECIAL_ATOMIC_ADD &&
	    node->builtin->number <= GLSL_SPECIAL_GROUP_MEMORY_BARRIER) {
		result = emit_compute_call(state, node);
		return result;
	}

	/* An image function, whose first argument is the image's variable (and an atomic's texel's place). */
	if (node->builtin != NULL &&
	    node->builtin->operation == GLSL_BI_SPECIAL &&
	    node->builtin->number >= GLSL_SPECIAL_IMAGE_SIZE &&
	    node->builtin->number <= GLSL_SPECIAL_IMAGE_ATOMIC_COMP_SWAP) {
		result = emit_image_call(state, node);
		return result;
	}

	/* The barrier of image memory. */
	if (node->builtin != NULL &&
	    node->builtin->operation == GLSL_BI_SPECIAL &&
	    node->builtin->number == GLSL_SPECIAL_MEMORY_BARRIER_IMAGE) {
		result = emit_compute_call(state, node);
		return result;
	}

	/* A built-in that writes out arguments (frexp, uaddCarry, usubBorrow, umulExtended, imulExtended). */
	if (node->builtin != NULL &&
	    node->builtin->operation == GLSL_BI_SPECIAL &&
	    node->builtin->number >= GLSL_SPECIAL_FREXP &&
	    node->builtin->number <= GLSL_SPECIAL_IMUL_EXTENDED) {
		result = emit_out_call(state, node);
		return result;
	}

	/* An interpolation of a fragment input, whose first argument is the input's place, not its value. */
	if (node->builtin != NULL &&
	    node->builtin->operation == GLSL_BI_SPECIAL &&
	    node->builtin->number >= GLSL_SPECIAL_INTERPOLATE_AT_CENTROID &&
	    node->builtin->number <= GLSL_SPECIAL_INTERPOLATE_AT_OFFSET) {
		result = emit_interpolate(state, node);
		return result;
	}

	/* dFdy, along GL's rows. */
	if (node->builtin != NULL &&
	    node->builtin->operation == GLSL_BI_SPECIAL &&
	    node->builtin->number == GLSL_SPECIAL_DFDY) {
		result = emit_derivative_y(state, node);
		return result;
	}

	/* A built-in: its arguments in order, then its instructions. */
	count = 0U;
	for (argument = node->child[1]; argument != NULL && count < 8U; argument = argument->next) {
		arguments[count] = glsl_emit_expression(state, argument);
		count++;
	}

	/* Succeeded: the built-in's instructions. */
	result = glsl_emit_builtin(state, node, arguments, count);

	/* Succeeded: the built-in's value. */
	return result;
}

/* Emits a constructor. */
static struct emit_value
emit_constructor(
	struct emit_state *state,
	struct glsl_node *node)
{
	const struct glsl_type *type;
	const struct glsl_type *scalar;
	struct glsl_node *argument;
	struct emit_value value;
	struct emit_value result;
	uint32_t parts[EMIT_MAX_PARTS];
	uint32_t scalars[EMIT_MAX_SCALARS * 4U];
	uint32_t columns[4];
	uint32_t zero;
	unsigned count;
	unsigned added;
	unsigned index;
	unsigned column;
	unsigned row;

	/* Structs and arrays: the arguments as they are. */
	type = node->type;
	if (type->kind == GLSL_KIND_STRUCT || type->kind == GLSL_KIND_ARRAY) {
		count = 0U;
		for (argument = node->child[1]; argument != NULL && count < EMIT_MAX_PARTS; argument = argument->next) {
			value = glsl_emit_expression(state, argument);
			parts[count] = value.id;
			count++;
		}

		/* Succeeded: the aggregate. */
		result = emit_value_of(glsl_emit_op(state, SPV_OP_COMPOSITE_CONSTRUCT, glsl_emit_type(state, type), parts, count), type);
		return result;
	}

	/* A matrix from one matrix is converted to the type's base, then resized. */
	if (type->kind == GLSL_KIND_MATRIX && node->child[1]->next == NULL && node->child[1]->type->kind == GLSL_KIND_MATRIX) {
		value = glsl_emit_expression(state, node->child[1]);
		value = glsl_emit_convert(state, value, glsl_type_with_base(value.type, type->base));
		result = emit_matrix_resize(state, value, type);
		return result;
	}

	/* The arguments' scalars, each converted to the type's base. */
	scalar = glsl_type_scalar(type->base);
	count = 0U;
	for (argument = node->child[1]; argument != NULL; argument = argument->next) {
		value = glsl_emit_expression(state, argument);
		added = emit_scalars(state, value, scalars + count, EMIT_MAX_SCALARS * 4U - count);
		for (index = count; index < count + added; index++)
			scalars[index] = glsl_emit_convert(state, emit_value_of(scalars[index], glsl_type_scalar(value.type->base)), scalar).id;
		count += added;
	}

	/* A scalar: the first scalar. */
	if (type->kind == GLSL_KIND_SCALAR) {
		result = emit_value_of(scalars[0], type);
		return result;
	}

	/* A vector: one scalar for every component, or the scalars in order. */
	if (type->kind == GLSL_KIND_VECTOR) {
		for (index = 0U; index < type->components; index++) {
			parts[index] = scalars[0];
			if (count > 1U)
				parts[index] = scalars[index];
		}

		/* Succeeded: the vector. */
		result = emit_value_of(glsl_emit_op(state, SPV_OP_COMPOSITE_CONSTRUCT, glsl_emit_type(state, type), parts, type->components), type);
		return result;
	}

	/* A matrix: one scalar on the diagonal (zero elsewhere), or the scalars column by column. */
	zero = glsl_emit_floating(state, type->base, 0.0);
	for (column = 0U; column < type->columns; column++) {
		for (row = 0U; row < type->components; row++) {
			if (count == 1U) {
				parts[row] = zero;
				if (row == column)
					parts[row] = scalars[0];
			} else {
				parts[row] = scalars[column * type->components + row];
			}
		}

		/* The column. */
		columns[column] = glsl_emit_op(state, SPV_OP_COMPOSITE_CONSTRUCT, glsl_emit_type(state, glsl_type_column(type)), parts, type->components);
	}

	/* Succeeded: the matrix. */
	result = emit_value_of(glsl_emit_op(state, SPV_OP_COMPOSITE_CONSTRUCT, glsl_emit_type(state, type), columns, type->columns), type);
	return result;
}

/*
 * Flattens a scalar, vector or matrix value into scalar ids, converted to
 * nothing (their own base).  Returns how many.
 */
static unsigned
emit_scalars(
	struct emit_state *state,
	struct emit_value value,
	uint32_t *ids,
	unsigned capacity)
{
	struct emit_value column;
	unsigned count;
	unsigned index;

	/* A scalar is itself. */
	if (value.type->kind == GLSL_KIND_SCALAR) {
		if (capacity == 0U)
			return 0U;
		ids[0] = value.id;
		return 1U;
	}

	/* A vector's components. */
	if (value.type->kind == GLSL_KIND_VECTOR) {
		count = 0U;
		for (index = 0U; index < value.type->components && count < capacity; index++) {
			ids[count] = glsl_emit_extract(state, value, index);
			count++;
		}

		/* Succeeded: the components. */
		return count;
	}

	/* A matrix's columns' components. */
	count = 0U;
	for (index = 0U; index < value.type->columns; index++) {
		column = emit_value_of(glsl_emit_extract(state, value, index), glsl_type_column(value.type));
		count += emit_scalars(state, column, ids + count, capacity - count);
	}

	/* Succeeded: the scalars. */
	return count;
}

/* Resizes a matrix: its columns and rows that exist, the identity elsewhere. */
static struct emit_value
emit_matrix_resize(
	struct emit_state *state,
	struct emit_value value,
	const struct glsl_type *type)
{
	struct emit_value column;
	struct emit_value result;
	uint32_t columns[4];
	uint32_t parts[4];
	unsigned index;
	unsigned row;

	/* Each column of the new shape. */
	for (index = 0U; index < type->columns; index++) {
		column.id = 0U;
		column.type = NULL;
		if (index < value.type->columns)
			column = emit_value_of(glsl_emit_extract(state, value, index), glsl_type_column(value.type));
		for (row = 0U; row < type->components; row++) {
			if (column.type != NULL && row < value.type->components) {
				parts[row] = glsl_emit_extract(state, column, row);
			} else if (row == index) {
				parts[row] = glsl_emit_floating(state, type->base, 1.0);
			} else {
				parts[row] = glsl_emit_floating(state, type->base, 0.0);
			}
		}

		/* The column. */
		columns[index] = glsl_emit_op(state, SPV_OP_COMPOSITE_CONSTRUCT, glsl_emit_type(state, glsl_type_column(type)), parts, type->components);
	}

	/* Succeeded: the resized matrix. */
	result = emit_value_of(glsl_emit_op(state, SPV_OP_COMPOSITE_CONSTRUCT, glsl_emit_type(state, type), columns, type->columns), type);
	return result;
}

/*
 * Inlines a call of a user function: the arguments evaluated, the
 * parameters as variables (copied in; a sampler parameter is the
 * sampler itself), the body (inside a loop that runs once when it
 * returns early), the out parameters copied back, and the result read.
 */
static struct emit_value
emit_inline(
	struct emit_state *state,
	struct glsl_node *node)
{
	struct glsl_function *function;
	struct glsl_symbol *parameter;
	struct glsl_node *argument;
	struct emit_frame *frame;
	struct emit_path paths[32];
	struct emit_value values[32];
	struct glsl_node *argument_nodes[32];
	struct emit_value result;
	uint32_t operands[2];
	uint32_t header;
	uint32_t body_label;
	uint32_t continue_label;
	uint32_t merge;
	unsigned index;
	unsigned count;
	int status;

	/* The arguments, left to right: values of in parameters, paths of out ones (inout read too). */
	function = node->function;
	count = 0U;
	for (argument = node->child[1]; argument != NULL && count < 32U; argument = argument->next) {
		parameter = function->parameters[count];
		argument_nodes[count] = argument;
		values[count].id = 0U;
		values[count].type = parameter->type;
		if (parameter->type->kind == GLSL_KIND_SAMPLER) {
			/* A sampler is passed as the sampler variable itself. */
			status = emit_path(state, argument, &paths[count]);
			if (status != 0)
				paths[count].base = 0U;
		} else if (parameter->storage == GLSL_STORAGE_IN) {
			values[count] = glsl_emit_expression(state, argument);
		} else {
			status = emit_path(state, argument, &paths[count]);
			if (status != 0)
				memset(&paths[count], 0, sizeof(paths[count]));
			if (status == 0 && parameter->storage == GLSL_STORAGE_INOUT)
				values[count] = emit_load(state, &paths[count]);
		}

		/* The next argument. */
		count++;
	}

	/* The parameters: variables holding their first values (an out one starts as zero). */
	for (index = 0U; index < count; index++) {
		parameter = function->parameters[index];
		if (parameter->type->kind == GLSL_KIND_SAMPLER) {
			/* The variable, or the element of an array of them the argument names. */
			parameter->id = paths[index].base;
			if (paths[index].base != 0U)
				parameter->id = emit_chain(state, &paths[index]);
			parameter->id_storage = SPV_STORAGE_UNIFORM_CONSTANT;

			/* An image parameter is the image uniform passed: its type of its format, its symbol, its unit. */
			if (parameter->type->image && paths[index].base != 0U) {
				parameter->type = paths[index].type;
				parameter->image_root = emit_image_root(argument_nodes[index]);
				parameter->image_unit = emit_image_unit(argument_nodes[index]);
			}
			continue;
		}

		/* Any other parameter is a variable of its own. */
		parameter->id = emit_variable(state, parameter->type);
		parameter->id_storage = SPV_STORAGE_FUNCTION;
		if (values[index].id == 0U)
			values[index].id = emit_zero(state, parameter->type);
		operands[0] = parameter->id;
		operands[1] = values[index].id;
		glsl_words_add(state->module, &state->module->body, SPV_OP_STORE, operands, 2U);
	}

	/* The frame: the result's variable, and the flag of an early return. */
	frame = glsl_alloc(state->module->arena, sizeof(*frame));
	frame->function = function;
	frame->outer = state->frame;
	if (function->return_type->kind != GLSL_KIND_VOID) {
		frame->result = emit_variable(state, function->return_type);
		operands[0] = frame->result;
		operands[1] = emit_zero(state, function->return_type);
		glsl_words_add(state->module, &state->module->body, SPV_OP_STORE, operands, 2U);
	}

	/* The flag that says the function returned early. */
	if (function->early_return) {
		frame->returned = emit_variable(state, glsl_type_scalar(GLSL_BASE_BOOL));
		operands[0] = frame->returned;
		operands[1] = glsl_module_bool(state->module, glsl_emit_type(state, glsl_type_scalar(GLSL_BASE_BOOL)), 0);
		glsl_words_add(state->module, &state->module->body, SPV_OP_STORE, operands, 2U);
	}

	/* The body runs in the frame. */
	state->frame = frame;

	/* A function with early returns runs inside a loop that runs once; a return leaves it. */
	header = 0U;
	merge = 0U;
	continue_label = 0U;
	if (function->early_return) {
		header = glsl_module_id(state->module);
		body_label = glsl_module_id(state->module);
		continue_label = glsl_module_id(state->module);
		merge = glsl_module_id(state->module);
		emit_branch(state, header);
		emit_label(state, header);
		emit_loop_merge(state, merge, continue_label);
		emit_branch(state, body_label);
		emit_label(state, body_label);
		emit_push_target(state, EMIT_TARGET_FRAME, merge, continue_label);
		frame->target = state->target_count;
	}

	/* The body. */
	emit_statements(state, function->body->child[0]);

	/* The once-loop ends (its continue block is never reached). */
	if (function->early_return) {
		emit_branch(state, merge);
		emit_pop_target(state);
		emit_label(state, continue_label);
		emit_branch(state, header);
		emit_label(state, merge);
	}

	/* Back in the caller's frame. */
	state->frame = frame->outer;

	/* The out and inout parameters copied back. */
	for (index = 0U; index < count; index++) {
		parameter = function->parameters[index];
		if (parameter->storage == GLSL_STORAGE_IN || paths[index].base == 0U)
			continue;
		result = emit_value_of(glsl_emit_op(state, SPV_OP_LOAD, glsl_emit_type(state, parameter->type), &parameter->id, 1U), parameter->type);
		emit_store(state, &paths[index], result);
	}

	/* A void function has no value. */
	if (frame->result == 0U)
		return emit_value_of(0U, function->return_type);

	/* Succeeded: the result read back. */
	result = emit_value_of(glsl_emit_op(state, SPV_OP_LOAD, glsl_emit_type(state, function->return_type), &frame->result, 1U), function->return_type);
	return result;
}

/*
 * Emits a call through a subroutine uniform (GLSL 4.00): each subroutine
 * of the uniform's type inlined in a selection on the index the hidden
 * uniform holds (the last one for any other index), its value kept in a
 * variable.  Only the branch taken evaluates the arguments.
 */
static struct emit_value
emit_subroutine_call(
	struct emit_state *state,
	struct glsl_node *node)
{
	struct glsl_function *type;
	struct glsl_function *subroutine;
	struct glsl_function *chosen[GLSL_MAX_SUBROUTINES];
	struct emit_value selector;
	struct emit_value value;
	uint32_t merges[GLSL_MAX_SUBROUTINES];
	uint32_t operands[2];
	uint32_t variable;
	uint32_t condition;
	uint32_t taken;
	uint32_t other;
	unsigned count;
	unsigned index;
	unsigned kind;

	/* The subroutines of the type, in index order. */
	type = node->function;
	count = 0U;
	for (subroutine = state->shader->subroutines; subroutine != NULL; subroutine = subroutine->next_subroutine) {
		for (kind = 0U; kind < subroutine->subroutine_type_count; kind++) {
			if (subroutine->subroutine_types[kind] != type)
				continue;
			chosen[count] = subroutine;
			count++;
			break;
		}
	}

	/* The index the hidden uniform holds, and the variable of the value. */
	selector = glsl_emit_expression(state, node->child[0]);
	variable = 0U;
	if (type->return_type->kind != GLSL_KIND_VOID)
		variable = emit_variable(state, type->return_type);

	/* Each subroutine but the last: run when the index is its own. */
	for (index = 0U; index + 1U < count; index++) {
		operands[0] = selector.id;
		operands[1] = glsl_module_constant(state->module, glsl_emit_type(state, glsl_type_scalar(GLSL_BASE_UINT)),
						   chosen[index]->subroutine_index);
		condition = glsl_emit_op(state, SPV_OP_I_EQUAL, glsl_emit_type(state, glsl_type_scalar(GLSL_BASE_BOOL)), operands, 2U);
		taken = glsl_module_id(state->module);
		other = glsl_module_id(state->module);
		merges[index] = glsl_module_id(state->module);
		emit_selection_merge(state, merges[index]);
		emit_conditional(state, condition, taken, other);

		/* The subroutine inlined, its value kept. */
		emit_label(state, taken);
		node->function = chosen[index];
		value = emit_inline(state, node);
		if (variable != 0U && !state->terminated) {
			operands[0] = variable;
			operands[1] = value.id;
			glsl_words_add(state->module, &state->module->body, SPV_OP_STORE, operands, 2U);
		}
		emit_branch(state, merges[index]);

		/* Otherwise the next. */
		emit_label(state, other);
	}

	/* The last (or only) subroutine runs for any other index. */
	if (count != 0U) {
		node->function = chosen[count - 1U];
		value = emit_inline(state, node);
		if (variable != 0U && !state->terminated) {
			operands[0] = variable;
			operands[1] = value.id;
			glsl_words_add(state->module, &state->module->body, SPV_OP_STORE, operands, 2U);
		}
	}
	node->function = type;

	/* The selections closed, innermost first. */
	for (index = count; index > 1U; index--) {
		emit_branch(state, merges[index - 2U]);
		emit_label(state, merges[index - 2U]);
	}

	/* A void call has no value. */
	if (variable == 0U)
		return emit_value_of(0U, type->return_type);

	/* Succeeded: the value read back. */
	value = emit_value_of(glsl_emit_op(state, SPV_OP_LOAD, glsl_emit_type(state, type->return_type), &variable, 1U), type->return_type);
	return value;
}

/* Makes a value from an id and a type. */
static struct emit_value
emit_value_of(
	uint32_t id,
	const struct glsl_type *type)
{
	struct emit_value value;

	/* The pair. */
	value.id = id;
	value.type = type;

	/* Succeeded: the value. */
	return value;
}

/*
 * Returns the type a GLSL type has inside a shader storage block (std430,
 * ws101-p008): bools as uints, arrays with their std430 stride (a
 * run-time array, length 0, as OpTypeRuntimeArray), structs with their
 * std430 offsets and column-major matrices' column strides.
 */
static uint32_t
emit_layout430_type(
	struct emit_state *state,
	const struct glsl_type *type)
{
	uint32_t operands[GLSL_MAX_OPERANDS];
	const struct glsl_type *field;
	const struct glsl_type *element;
	uint32_t stride;
	uint32_t id;
	uint32_t offset;
	unsigned slot;
	unsigned index;
	unsigned alignment;

	/* Scalars, vectors and matrices are the local types, but bools are uints. */
	if (type->kind == GLSL_KIND_SCALAR || type->kind == GLSL_KIND_VECTOR || type->kind == GLSL_KIND_MATRIX) {
		if (type->base == GLSL_BASE_BOOL)
			type = glsl_type_with_base(type, GLSL_BASE_UINT);
		id = glsl_emit_type(state, type);
		return id;
	}

	/* A layout given before. */
	slot = emit_type_slot(state, type);
	if (state->layout430_ids[slot] != 0U)
		return state->layout430_ids[slot];

	/* An array: its element's layout, its length (none for a run-time array), its stride. */
	operands[0] = 0U;
	if (type->kind == GLSL_KIND_ARRAY) {
		operands[1] = emit_layout430_type(state, type->element);
		if (type->length == 0U) {
			id = glsl_module_declare(state->module, SPV_OP_TYPE_RUNTIME_ARRAY, operands, 2U, 1);
		} else {
			operands[2] = emit_uint(state, type->length);
			id = glsl_module_declare(state->module, SPV_OP_TYPE_ARRAY, operands, 3U, 1);
		}
		stride = glsl_std430_stride(type);
		glsl_module_decorate(state->module, id, SPV_DECORATION_ARRAY_STRIDE, &stride, 1U);
		slot = emit_type_slot(state, type);
		state->layout430_ids[slot] = id;
		return id;
	}

	/* A struct: its members' layouts, a type of its own. */
	for (index = 0U; index < type->field_count && index + 1U < GLSL_MAX_OPERANDS; index++)
		operands[index + 1U] = emit_layout430_type(state, type->fields[index].type);
	id = glsl_module_declare(state->module, SPV_OP_TYPE_STRUCT, operands, type->field_count + 1U, 1);

	/* Each member's offset and name, and a matrix's column-major stride. */
	offset = 0U;
	for (index = 0U; index < type->field_count; index++) {
		field = type->fields[index].type;
		alignment = glsl_std430_alignment(field);
		offset = (offset + alignment - 1U) & ~(alignment - 1U);
		glsl_module_member_decorate(state->module, id, index, SPV_DECORATION_OFFSET, offset);
		glsl_module_member_name(state->module, id, index, type->fields[index].name);
		element = field;
		while (element->kind == GLSL_KIND_ARRAY)
			element = element->element;
		if (element->kind == GLSL_KIND_MATRIX) {
			glsl_module_member_decorate(state->module, id, index, SPV_DECORATION_COL_MAJOR, 0xffffffffU);
			glsl_module_member_decorate(state->module, id, index, SPV_DECORATION_MATRIX_STRIDE, glsl_std430_column_stride(element));
		}
		offset += glsl_std430_size(field);
	}

	/* Succeeded: the struct's layout type, remembered. */
	slot = emit_type_slot(state, type);
	state->layout430_ids[slot] = id;
	return id;
}

/*
 * Declares a shader storage block (ws101-p008): its std430 struct, a
 * BufferBlock (SPIR-V 1.0's storage buffer: Uniform storage) with its
 * memory qualifiers on every member, at set 0 and the link's binding.
 */
static void
emit_storage_block(
	struct emit_state *state,
	struct glsl_symbol *symbol)
{
	uint32_t operands[3];
	uint32_t structure;
	uint32_t value;
	unsigned index;

	/* The struct, a BufferBlock named by the block. */
	structure = emit_layout430_type(state, symbol->type);
	glsl_module_decorate(state->module, structure, SPV_DECORATION_BUFFER_BLOCK, NULL, 0U);
	glsl_module_name(state->module, structure, symbol->type->name);

	/* The block's memory qualifiers, on each member. */
	for (index = 0U; index < symbol->type->field_count; index++) {
		if ((symbol->memory & GLSL_MEMORY_READONLY) != 0U)
			glsl_module_member_decorate(state->module, structure, index, SPV_DECORATION_NON_WRITABLE, 0xffffffffU);
		if ((symbol->memory & GLSL_MEMORY_WRITEONLY) != 0U)
			glsl_module_member_decorate(state->module, structure, index, SPV_DECORATION_NON_READABLE, 0xffffffffU);
		if ((symbol->memory & GLSL_MEMORY_COHERENT) != 0U)
			glsl_module_member_decorate(state->module, structure, index, SPV_DECORATION_COHERENT, 0xffffffffU);
		if ((symbol->memory & GLSL_MEMORY_VOLATILE) != 0U)
			glsl_module_member_decorate(state->module, structure, index, SPV_DECORATION_VOLATILE, 0xffffffffU);
		if ((symbol->memory & GLSL_MEMORY_RESTRICT) != 0U)
			glsl_module_member_decorate(state->module, structure, index, SPV_DECORATION_RESTRICT, 0xffffffffU);
	}

	/* The variable: set 0, the link's binding. */
	symbol->id = glsl_module_id(state->module);
	symbol->id_storage = SPV_STORAGE_UNIFORM;
	operands[0] = glsl_emit_pointer(state, SPV_STORAGE_UNIFORM, structure);
	operands[1] = symbol->id;
	operands[2] = SPV_STORAGE_UNIFORM;
	glsl_words_add(state->module, &state->module->globals, SPV_OP_VARIABLE, operands, 3U);
	value = 0U;
	glsl_module_decorate(state->module, symbol->id, SPV_DECORATION_DESCRIPTOR_SET, &value, 1U);
	value = symbol->binding;
	glsl_module_decorate(state->module, symbol->id, SPV_DECORATION_BINDING, &value, 1U);
	glsl_module_name(state->module, symbol->id, symbol->type->name);
}

/* Declares a compute shader's shared variable: a Workgroup variable of its type, named (ws101-p008). */
static void
emit_shared(
	struct emit_state *state,
	struct glsl_symbol *symbol)
{
	uint32_t operands[3];

	/* The variable, which every invocation of the group shares. */
	symbol->id = glsl_module_id(state->module);
	symbol->id_storage = SPV_STORAGE_WORKGROUP;
	operands[0] = glsl_emit_pointer(state, SPV_STORAGE_WORKGROUP, glsl_emit_type(state, symbol->type));
	operands[1] = symbol->id;
	operands[2] = SPV_STORAGE_WORKGROUP;
	glsl_words_add(state->module, &state->module->globals, SPV_OP_VARIABLE, operands, 3U);
	glsl_module_name(state->module, symbol->id, symbol->name);
}

/*
 * Writes a value through a path into a shader storage block (ws101-p008):
 * a leaf through its chain (a bool as its uint), an aggregate part by part
 * (a matrix column by column: i915 moves scalars and vectors of memory).
 */
static void
emit_store_block(
	struct emit_state *state,
	const struct emit_path *path,
	struct emit_value value)
{
	const struct glsl_type *type;
	const struct glsl_type *stored;
	struct emit_path part;
	struct emit_value element;
	uint32_t operands[3];
	uint32_t pointer;
	unsigned count;
	unsigned index;

	/* A leaf: stored, a bool as a uint. */
	type = path->type;
	if (type->kind == GLSL_KIND_SCALAR || type->kind == GLSL_KIND_VECTOR) {
		if (type->base == GLSL_BASE_BOOL) {
			stored = glsl_type_with_base(type, GLSL_BASE_UINT);
			operands[0] = value.id;
			operands[1] = glsl_emit_splat(state, emit_uint(state, 1U), stored);
			operands[2] = glsl_emit_splat(state, emit_uint(state, 0U), stored);
			value = emit_value_of(glsl_emit_op(state, SPV_OP_SELECT, glsl_emit_type(state, stored), operands, 3U), stored);
		}
		pointer = emit_chain(state, path);
		operands[0] = pointer;
		operands[1] = value.id;
		glsl_words_add(state->module, &state->module->body, SPV_OP_STORE, operands, 2U);
		return;
	}

	/* An aggregate: each part taken from the value and stored on its own. */
	count = type->length;
	if (type->kind == GLSL_KIND_STRUCT)
		count = type->field_count;
	if (type->kind == GLSL_KIND_MATRIX)
		count = type->columns;
	for (index = 0U; index < count && index < EMIT_MAX_PARTS; index++) {
		part = *path;
		if (part.index_count == 16U)
			return;
		part.indices[part.index_count] = glsl_emit_int(state, (int32_t)index);
		part.index_count++;
		if (type->kind == GLSL_KIND_STRUCT) {
			part.type = type->fields[index].type;
		} else if (type->kind == GLSL_KIND_MATRIX) {
			part.type = glsl_type_column(type);
		} else {
			part.type = type->element;
		}
		element = emit_value_of(glsl_emit_extract(state, value, index), part.type);
		emit_store_block(state, &part, element);
	}
}

/*
 * Emits .length() of a storage block's run-time array (ws101-p008): its
 * block's OpArrayLength, as an int.  The array is the block's last member:
 * "instance.member" or a member without an instance name.
 */
static struct emit_value
emit_array_length(
	struct emit_state *state,
	struct glsl_node *node)
{
	struct glsl_node *array;
	struct glsl_symbol *block;
	struct emit_value value;
	uint32_t operands[3];
	uint32_t member;
	uint32_t length;

	/* The block and the member. */
	array = node->child[0];
	block = NULL;
	member = 0U;
	if (array->kind == GLSL_N_IDENTIFIER && array->symbol != NULL && array->symbol->where == GLSL_VAR_BUFFER_MEMBER) {
		block = array->symbol->block;
		member = array->symbol->member;
	} else if (array->kind == GLSL_N_FIELD &&
		   array->child[0]->kind == GLSL_N_IDENTIFIER &&
		   array->child[0]->symbol != NULL &&
		   array->child[0]->symbol->where == GLSL_VAR_BUFFER) {
		block = array->child[0]->symbol;
		member = array->field;
	}

	/* Anything else has a constant length the checker gave. */
	if (block == NULL) {
		value = emit_value_of(emit_zero(state, node->type), node->type);
		return value;
	}

	/* OpArrayLength of the block's member, as an int. */
	operands[0] = block->id;
	operands[1] = member;
	length = glsl_emit_op(state, SPV_OP_ARRAY_LENGTH, glsl_emit_type(state, glsl_type_scalar(GLSL_BASE_UINT)), operands, 2U);
	operands[0] = length;
	value = emit_value_of(glsl_emit_op(state, SPV_OP_BITCAST, glsl_emit_type(state, node->type), operands, 1U), node->type);
	return value;
}

/*
 * Emits a compute shader's atomic function or barrier (ws101-p008).
 *
 * An atomic works on the word its first argument's path leads to (a
 * buffer's at Device scope, a shared variable's at Workgroup scope),
 * relaxed, and gives the old value; atomicCompSwap(mem, compare, data)
 * is OpAtomicCompareExchange with data as the value and compare as the
 * comparator.  barrier() is OpControlBarrier of the workgroup, acquiring
 * and releasing shared memory (as glslang emits it); the memory barriers
 * are OpMemoryBarrier of the memory they name.
 */
static struct emit_value
emit_compute_call(
	struct emit_state *state,
	struct glsl_node *node)
{
	struct emit_path path;
	struct emit_value arguments[2];
	struct glsl_node *argument;
	uint32_t operands[8];
	uint32_t opcode;
	uint32_t type;
	uint32_t pointer;
	uint32_t scope;
	unsigned special;
	unsigned count;
	int status;
	int is_signed;

	/* The barriers: scope and semantics. */
	special = node->builtin->number;
	switch (special) {
	case GLSL_SPECIAL_BARRIER:
		/* A tessellation control shader's barrier orders its patch's invocations; their outputs need no memory semantics then. */
		operands[0] = emit_uint(state, SPV_SCOPE_WORKGROUP);
		operands[1] = emit_uint(state, SPV_SCOPE_WORKGROUP);
		operands[2] = emit_uint(state, SPV_SEMANTICS_ACQUIRE_RELEASE | SPV_SEMANTICS_WORKGROUP_MEMORY);
		if (state->shader->stage == GLSL_STAGE_TESS_CONTROL) {
			operands[1] = emit_uint(state, SPV_SCOPE_INVOCATION);
			operands[2] = emit_uint(state, 0U);
		}
		glsl_words_add(state->module, &state->module->body, SPV_OP_CONTROL_BARRIER, operands, 3U);
		return emit_value_of(0U, node->type);
	case GLSL_SPECIAL_MEMORY_BARRIER:
		operands[0] = emit_uint(state, SPV_SCOPE_DEVICE);
		operands[1] = emit_uint(state, SPV_SEMANTICS_ACQUIRE_RELEASE | SPV_SEMANTICS_UNIFORM_MEMORY | SPV_SEMANTICS_WORKGROUP_MEMORY |
					       SPV_SEMANTICS_IMAGE_MEMORY);
		glsl_words_add(state->module, &state->module->body, SPV_OP_MEMORY_BARRIER, operands, 2U);
		return emit_value_of(0U, node->type);
	case GLSL_SPECIAL_MEMORY_BARRIER_IMAGE:
		operands[0] = emit_uint(state, SPV_SCOPE_DEVICE);
		operands[1] = emit_uint(state, SPV_SEMANTICS_ACQUIRE_RELEASE | SPV_SEMANTICS_IMAGE_MEMORY);
		glsl_words_add(state->module, &state->module->body, SPV_OP_MEMORY_BARRIER, operands, 2U);
		return emit_value_of(0U, node->type);
	case GLSL_SPECIAL_MEMORY_BARRIER_BUFFER:
		operands[0] = emit_uint(state, SPV_SCOPE_DEVICE);
		operands[1] = emit_uint(state, SPV_SEMANTICS_ACQUIRE_RELEASE | SPV_SEMANTICS_UNIFORM_MEMORY);
		glsl_words_add(state->module, &state->module->body, SPV_OP_MEMORY_BARRIER, operands, 2U);
		return emit_value_of(0U, node->type);
	case GLSL_SPECIAL_MEMORY_BARRIER_SHARED:
		operands[0] = emit_uint(state, SPV_SCOPE_WORKGROUP);
		operands[1] = emit_uint(state, SPV_SEMANTICS_ACQUIRE_RELEASE | SPV_SEMANTICS_WORKGROUP_MEMORY);
		glsl_words_add(state->module, &state->module->body, SPV_OP_MEMORY_BARRIER, operands, 2U);
		return emit_value_of(0U, node->type);
	case GLSL_SPECIAL_GROUP_MEMORY_BARRIER:
		operands[0] = emit_uint(state, SPV_SCOPE_WORKGROUP);
		operands[1] = emit_uint(state, SPV_SEMANTICS_ACQUIRE_RELEASE | SPV_SEMANTICS_UNIFORM_MEMORY | SPV_SEMANTICS_WORKGROUP_MEMORY);
		glsl_words_add(state->module, &state->module->body, SPV_OP_MEMORY_BARRIER, operands, 2U);
		return emit_value_of(0U, node->type);
	default:
		break;
	}

	/* An atomic: the pointer to its word, then the values after it. */
	argument = node->child[1];
	status = emit_path(state, argument, &path);
	if (status != 0 || path.swizzle_count > 1U)
		return emit_value_of(emit_zero(state, node->type), node->type);
	if (path.swizzle_count == 1U) {
		path.indices[path.index_count] = glsl_emit_int(state, (int32_t)path.swizzle[0]);
		path.index_count++;
		path.swizzle_count = 0U;
	}
	pointer = emit_chain(state, &path);
	count = 0U;
	for (argument = argument->next; argument != NULL && count < 2U; argument = argument->next) {
		arguments[count] = glsl_emit_expression(state, argument);
		count++;
	}

	/* The operation, signed or unsigned. */
	is_signed = (node->type->base == GLSL_BASE_INT);
	switch (special) {
	case GLSL_SPECIAL_ATOMIC_ADD:
		opcode = SPV_OP_ATOMIC_I_ADD;
		break;
	case GLSL_SPECIAL_ATOMIC_MIN:
		opcode = is_signed ? SPV_OP_ATOMIC_S_MIN : SPV_OP_ATOMIC_U_MIN;
		break;
	case GLSL_SPECIAL_ATOMIC_MAX:
		opcode = is_signed ? SPV_OP_ATOMIC_S_MAX : SPV_OP_ATOMIC_U_MAX;
		break;
	case GLSL_SPECIAL_ATOMIC_AND:
		opcode = SPV_OP_ATOMIC_AND;
		break;
	case GLSL_SPECIAL_ATOMIC_OR:
		opcode = SPV_OP_ATOMIC_OR;
		break;
	case GLSL_SPECIAL_ATOMIC_XOR:
		opcode = SPV_OP_ATOMIC_XOR;
		break;
	case GLSL_SPECIAL_ATOMIC_EXCHANGE:
		opcode = SPV_OP_ATOMIC_EXCHANGE;
		break;
	default:
		opcode = SPV_OP_ATOMIC_COMPARE_EXCHANGE;
		break;
	}

	/* The scope of the memory, relaxed semantics, the value (and a compare and exchange's comparator). */
	scope = SPV_SCOPE_DEVICE;
	if (path.storage == SPV_STORAGE_WORKGROUP)
		scope = SPV_SCOPE_WORKGROUP;
	type = glsl_emit_type(state, node->type);
	operands[0] = pointer;
	operands[1] = emit_uint(state, scope);
	operands[2] = emit_uint(state, 0U);
	if (opcode == SPV_OP_ATOMIC_COMPARE_EXCHANGE && count == 2U) {
		operands[3] = emit_uint(state, 0U);
		operands[4] = arguments[1].id;
		operands[5] = arguments[0].id;
		return emit_value_of(glsl_emit_op(state, opcode, type, operands, 6U), node->type);
	}
	operands[3] = arguments[0].id;
	return emit_value_of(glsl_emit_op(state, opcode, type, operands, 4U), node->type);
}

/*
 * Emits an image function: the image's variable (an image uniform, an
 * element of an array of them, or the one an image parameter stands
 * for), and its size (OpImageQuerySize), or a texel read, written or
 * changed by an atomic.  A read, a write or an atomic happens only when
 * the access is valid (OpenGL ES 3.1 section 8.22: the unit holds an
 * image the uniform may access and the texel exists); an invalid read
 * or atomic returns zeros, and an invalid write does nothing.
 */
static struct emit_value
emit_image_call(
	struct emit_state *state,
	struct glsl_node *node)
{
	struct emit_path path;
	struct emit_value arguments[3];
	struct emit_value value;
	struct glsl_node *argument;
	uint32_t operands[2];
	uint32_t image_type;
	uint32_t variable;
	uint32_t image;
	uint32_t valid;
	uint32_t result;
	uint32_t access;
	uint32_t merge;
	unsigned special;
	unsigned count;
	int status;

	/* The image's variable and its type of its format. */
	special = node->builtin->number;
	status = emit_path(state, node->child[1], &path);
	if (status != 0 || path.base == 0U || path.swizzle_count != 0U) {
		if (node->type->kind == GLSL_KIND_VOID)
			return emit_value_of(0U, node->type);
		return emit_value_of(emit_zero(state, node->type), node->type);
	}
	image_type = glsl_emit_type(state, path.type);

	/* The variable's pointer, or an element's of an array of images (a constant index). */
	variable = emit_chain(state, &path);

	/* The other arguments: the coordinate, then a texel or an atomic's values. */
	count = 0U;
	for (argument = node->child[1]->next; argument != NULL && count < 3U; argument = argument->next) {
		arguments[count] = glsl_emit_expression(state, argument);
		count++;
	}

	/* The size, which is not an access of a texel. */
	if (special == GLSL_SPECIAL_IMAGE_SIZE) {
		glsl_emit_capability(state, SPV_CAPABILITY_IMAGE_QUERY, &state->module->image_query);
		image = glsl_emit_op(state, SPV_OP_LOAD, image_type, &variable, 1U);
		return emit_value_of(glsl_emit_op(state, SPV_OP_IMAGE_QUERY_SIZE, glsl_emit_type(state, node->type), &image, 1U),
				     node->type);
	}

	/* Whether the access is valid; without the hidden uniform (a shader that cannot tell) it always happens. */
	valid = emit_image_valid(state, node->child[1], path.type, variable, image_type, arguments[0].id);
	if (valid == 0U) {
		value = emit_image_access(state, node, variable, image_type, arguments, count);
		return value;
	}

	/* The result's variable, zeros unless the access happens. */
	result = 0U;
	if (node->type->kind != GLSL_KIND_VOID) {
		result = emit_variable(state, node->type);
		operands[0] = result;
		operands[1] = emit_zero(state, node->type);
		glsl_words_add(state->module, &state->module->body, SPV_OP_STORE, operands, 2U);
	}

	/* The access in a block of its own that only a valid access reaches. */
	access = glsl_module_id(state->module);
	merge = glsl_module_id(state->module);
	emit_selection_merge(state, merge);
	emit_conditional(state, valid, access, merge);
	emit_label(state, access);
	value = emit_image_access(state, node, variable, image_type, arguments, count);
	if (result != 0U) {
		operands[0] = result;
		operands[1] = value.id;
		glsl_words_add(state->module, &state->module->body, SPV_OP_STORE, operands, 2U);
	}
	emit_branch(state, merge);
	emit_label(state, merge);

	/* A store returns nothing. */
	if (result == 0U)
		return emit_value_of(0U, node->type);

	/* Succeeded: what the access returned, or zeros. */
	value = emit_value_of(glsl_emit_op(state, SPV_OP_LOAD, glsl_emit_type(state, node->type), &result, 1U), node->type);
	return value;
}

/*
 * Emits an image access that happens: a texel read (OpImageRead) or
 * written (OpImageWrite), or an atomic on a texel through its pointer
 * (OpImageTexelPointer, sample 0) at device scope with relaxed semantics.
 * An atomic marks its image uniform, which the device's format must let
 * atomics use.
 */
static struct emit_value
emit_image_access(
	struct emit_state *state,
	struct glsl_node *node,
	uint32_t variable,
	uint32_t image_type,
	const struct emit_value *arguments,
	unsigned count)
{
	struct glsl_symbol *root;
	uint32_t operands[8];
	uint32_t image;
	uint32_t pointer;
	uint32_t opcode;
	uint32_t type;
	unsigned special;
	int is_signed;

	/* A read, a write. */
	special = node->builtin->number;
	switch (special) {
	case GLSL_SPECIAL_IMAGE_LOAD:
		image = glsl_emit_op(state, SPV_OP_LOAD, image_type, &variable, 1U);
		operands[0] = image;
		operands[1] = arguments[0].id;
		return emit_value_of(glsl_emit_op(state, SPV_OP_IMAGE_READ, glsl_emit_type(state, node->type), operands, 2U), node->type);
	case GLSL_SPECIAL_IMAGE_STORE:
		image = glsl_emit_op(state, SPV_OP_LOAD, image_type, &variable, 1U);
		operands[0] = image;
		operands[1] = arguments[0].id;
		operands[2] = arguments[1].id;
		glsl_words_add(state->module, &state->module->body, SPV_OP_IMAGE_WRITE, operands, 3U);
		return emit_value_of(0U, node->type);
	default:
		break;
	}

	/* An atomic: its image uniform is one atomics use. */
	root = emit_image_root(node->child[1]);
	if (root != NULL)
		root->image_atomic = 1U;

	/* The texel's pointer. */
	type = glsl_emit_type(state, node->type);
	operands[0] = variable;
	operands[1] = arguments[0].id;
	operands[2] = emit_uint(state, 0U);
	pointer = glsl_emit_op(state, SPV_OP_IMAGE_TEXEL_POINTER, glsl_emit_pointer(state, SPV_STORAGE_IMAGE, type), operands, 3U);

	/* The operation, signed or unsigned. */
	is_signed = (node->type->base == GLSL_BASE_INT);
	switch (special) {
	case GLSL_SPECIAL_IMAGE_ATOMIC_ADD:
		opcode = SPV_OP_ATOMIC_I_ADD;
		break;
	case GLSL_SPECIAL_IMAGE_ATOMIC_MIN:
		opcode = is_signed ? SPV_OP_ATOMIC_S_MIN : SPV_OP_ATOMIC_U_MIN;
		break;
	case GLSL_SPECIAL_IMAGE_ATOMIC_MAX:
		opcode = is_signed ? SPV_OP_ATOMIC_S_MAX : SPV_OP_ATOMIC_U_MAX;
		break;
	case GLSL_SPECIAL_IMAGE_ATOMIC_AND:
		opcode = SPV_OP_ATOMIC_AND;
		break;
	case GLSL_SPECIAL_IMAGE_ATOMIC_OR:
		opcode = SPV_OP_ATOMIC_OR;
		break;
	case GLSL_SPECIAL_IMAGE_ATOMIC_XOR:
		opcode = SPV_OP_ATOMIC_XOR;
		break;
	case GLSL_SPECIAL_IMAGE_ATOMIC_EXCHANGE:
		opcode = SPV_OP_ATOMIC_EXCHANGE;
		break;
	default:
		opcode = SPV_OP_ATOMIC_COMPARE_EXCHANGE;
		break;
	}

	/* Device scope, relaxed semantics, the value (a compare and exchange's value is the data, its comparator the compare). */
	operands[0] = pointer;
	operands[1] = emit_uint(state, SPV_SCOPE_DEVICE);
	operands[2] = emit_uint(state, 0U);
	if (opcode == SPV_OP_ATOMIC_COMPARE_EXCHANGE && count == 3U) {
		operands[3] = emit_uint(state, 0U);
		operands[4] = arguments[2].id;
		operands[5] = arguments[1].id;
		return emit_value_of(glsl_emit_op(state, opcode, type, operands, 6U), node->type);
	}
	operands[3] = arguments[1].id;
	return emit_value_of(glsl_emit_op(state, opcode, type, operands, 4U), node->type);
}

/*
 * Returns whether an image access is valid (a bool id), 0 when the shader
 * has no hidden uniform gl_ZedImages to tell: the unit of the image (its
 * bit in gl_ZedImages, which libGLESv2 clears for a unit whose binding
 * the uniform may not access) and a texel the image has (each coordinate
 * from 0 to below the size; a cube map's face below 6).
 */
static uint32_t
emit_image_valid(
	struct emit_state *state,
	const struct glsl_node *node,
	const struct glsl_type *type,
	uint32_t variable,
	uint32_t image_type,
	uint32_t coordinate)
{
	const struct glsl_type *uint_type;
	struct glsl_symbol *zed;
	uint32_t operands[3];
	uint32_t target;
	uint32_t pointer;
	uint32_t mask;
	uint32_t bit;
	uint32_t has;
	uint32_t image;
	uint32_t size;
	uint32_t int_type;
	uint32_t bool_type;
	uint32_t place;
	uint32_t bound;
	uint32_t inside;
	uint32_t exists;
	uint32_t valid;
	unsigned sizes;
	unsigned places;
	unsigned component;
	unsigned unit;

	/* The hidden uniform, in the default block. */
	zed = state->shader->zed_images;
	if (zed == NULL || !zed->used || state->members[zed->uniform] < 0)
		return 0U;

	/* The image unit's bit (an image parameter of no known unit has every bit). */
	uint_type = glsl_type_scalar(GLSL_BASE_UINT);
	target = emit_layout_type(state, uint_type);
	operands[0] = state->block;
	operands[1] = glsl_emit_int(state, state->members[zed->uniform]);
	pointer = glsl_emit_op(state, SPV_OP_ACCESS_CHAIN, glsl_emit_pointer(state, SPV_STORAGE_UNIFORM, target), operands, 2U);
	mask = glsl_emit_op(state, SPV_OP_LOAD, target, &pointer, 1U);
	unit = emit_image_unit(node);
	has = glsl_module_bool(state->module, glsl_emit_type(state, glsl_type_scalar(GLSL_BASE_BOOL)), 1);
	if (unit < 32U) {
		operands[0] = mask;
		operands[1] = emit_uint(state, (uint32_t)1U << unit);
		bit = glsl_emit_op(state, SPV_OP_BITWISE_AND, glsl_emit_type(state, uint_type), operands, 2U);
		operands[0] = bit;
		operands[1] = emit_uint(state, 0U);
		has = glsl_emit_op(state, SPV_OP_I_NOT_EQUAL, glsl_emit_type(state, glsl_type_scalar(GLSL_BASE_BOOL)), operands, 2U);
	}

	/*
	 * The size the image reports, and the coordinate's components: a 2D
	 * image's two, the others' three (a cube map's third is its face, of
	 * which there are 6).
	 */
	sizes = 2U;
	places = 2U;
	if (type->sampler == GLSL_SAMPLER_3D || type->arrayed) {
		sizes = 3U;
		places = 3U;
	} else if (type->sampler == GLSL_SAMPLER_CUBE) {
		places = 3U;
	}
	int_type = glsl_emit_type(state, glsl_type_scalar(GLSL_BASE_INT));
	bool_type = glsl_emit_type(state, glsl_type_scalar(GLSL_BASE_BOOL));
	glsl_emit_capability(state, SPV_CAPABILITY_IMAGE_QUERY, &state->module->image_query);
	image = glsl_emit_op(state, SPV_OP_LOAD, image_type, &variable, 1U);
	size = glsl_emit_op(state, SPV_OP_IMAGE_QUERY_SIZE, glsl_emit_type(state, glsl_type_vector(GLSL_BASE_INT, sizes)), &image, 1U);

	/* Every component of the coordinate from 0 to below its bound (compared unsigned, so a negative one is past it), one by one. */
	exists = 0U;
	for (component = 0U; component < places; component++) {
		operands[0] = coordinate;
		operands[1] = component;
		place = glsl_emit_op(state, SPV_OP_COMPOSITE_EXTRACT, int_type, operands, 2U);
		bound = glsl_emit_int(state, 6);
		if (component < sizes) {
			operands[0] = size;
			bound = glsl_emit_op(state, SPV_OP_COMPOSITE_EXTRACT, int_type, operands, 2U);
		}
		operands[0] = place;
		operands[1] = bound;
		inside = glsl_emit_op(state, SPV_OP_U_LESS_THAN, bool_type, operands, 2U);

		/* With the components before it. */
		if (exists != 0U) {
			operands[0] = exists;
			operands[1] = inside;
			inside = glsl_emit_op(state, SPV_OP_LOGICAL_AND, bool_type, operands, 2U);
		}
		exists = inside;
	}

	/* Both. */
	operands[0] = has;
	operands[1] = exists;
	valid = glsl_emit_op(state, SPV_OP_LOGICAL_AND, bool_type, operands, 2U);

	/* Succeeded: the condition. */
	return valid;
}

/* Returns the image uniform an image argument names (an element's array, an image parameter's while its function is inlined), NULL for none. */
static struct glsl_symbol *
emit_image_root(
	const struct glsl_node *node)
{
	struct glsl_symbol *symbol;

	/* Down an element of an array of images. */
	while (node != NULL && node->kind == GLSL_N_INDEX)
		node = node->child[0];

	/* An identifier of an image uniform, or of a parameter standing for one. */
	if (node == NULL || node->kind != GLSL_N_IDENTIFIER || node->symbol == NULL)
		return NULL;
	symbol = node->symbol;
	if (symbol->where == GLSL_VAR_PARAMETER)
		symbol = symbol->image_root;
	if (symbol == NULL || symbol->where != GLSL_VAR_IMAGE)
		return NULL;

	/* Succeeded: the uniform. */
	return symbol;
}

/*
 * Returns the image unit an image argument names: an image uniform's (an
 * element's of an array of them, by its constant index), or the one of
 * the image an image parameter stands for while its function is inlined;
 * UINT32_MAX when it cannot be known.
 */
static unsigned
emit_image_unit(
	const struct glsl_node *node)
{
	const struct glsl_symbol *symbol;
	unsigned element;

	/* An element of an array of images: its constant index. */
	element = 0U;
	if (node != NULL && node->kind == GLSL_N_INDEX) {
		if (node->child[1] == NULL || node->child[1]->constant == NULL)
			return UINT32_MAX;
		element = node->child[1]->constant->values[0].u;
		node = node->child[0];
	}

	/* An identifier of an image uniform, or of a parameter standing for one. */
	if (node == NULL || node->kind != GLSL_N_IDENTIFIER || node->symbol == NULL)
		return UINT32_MAX;
	symbol = node->symbol;
	if (symbol->where == GLSL_VAR_PARAMETER) {
		if (symbol->image_root == NULL || symbol->image_unit == UINT32_MAX)
			return UINT32_MAX;
		return symbol->image_unit + element;
	}
	if (symbol->where != GLSL_VAR_IMAGE)
		return UINT32_MAX;

	/* Succeeded: the uniform's unit, and the element's after it. */
	return symbol->layout_binding + element;
}

/*
 * Emits a built-in that writes out arguments: one instruction whose
 * result is a struct of two members (frexp's significand and exponent,
 * a sum's or a difference's value and carry or borrow, a product's low
 * and high halves), the out arguments stored from it.
 */
static struct emit_value
emit_out_call(
	struct emit_state *state,
	struct glsl_node *node)
{
	struct emit_path paths[4];
	struct emit_value values[4];
	struct emit_value pair;
	struct emit_value part;
	struct glsl_node *argument;
	const struct glsl_type *member;
	uint32_t operands[4];
	uint32_t opcode;
	unsigned special;
	unsigned count;
	unsigned index;
	int written[4];
	int writes;
	int status;

	/* The arguments left to right: the values of the ones read, the places of the ones written. */
	count = 0U;
	for (argument = node->child[1]; argument != NULL && count < 4U; argument = argument->next) {
		writes = glsl_builtin_writes(node->builtin, count);
		written[count] = 0;
		values[count] = emit_value_of(0U, argument->type);
		if (writes) {
			status = emit_path(state, argument, &paths[count]);
			if (status == 0)
				written[count] = 1;
		} else {
			values[count] = glsl_emit_expression(state, argument);
		}

		/* The next argument. */
		count++;
	}

	/* The instruction and its struct: frexp's of the value's type and an int of its size, the others' of two of the integers. */
	special = node->builtin->number;
	member = values[0].type;
	if (special == GLSL_SPECIAL_FREXP) {
		member = glsl_type_with_base(values[0].type, GLSL_BASE_INT);
		pair.type = emit_pair_type(state, values[0].type, member);
		operands[0] = state->module->std450;
		operands[1] = EMIT_FREXP_STRUCT;
		operands[2] = values[0].id;
		pair.id = glsl_emit_op(state, SPV_OP_EXT_INST, glsl_emit_type(state, pair.type), operands, 3U);
	} else {
		/* The opcode of the integer operation. */
		switch (special) {
		case GLSL_SPECIAL_UADD_CARRY:
			opcode = SPV_OP_I_ADD_CARRY;
			break;
		case GLSL_SPECIAL_USUB_BORROW:
			opcode = SPV_OP_I_SUB_BORROW;
			break;
		case GLSL_SPECIAL_UMUL_EXTENDED:
			opcode = SPV_OP_U_MUL_EXTENDED;
			break;
		default:
			opcode = SPV_OP_S_MUL_EXTENDED;
			break;
		}

		/* The two operands' struct. */
		pair.type = emit_pair_type(state, member, member);
		operands[0] = values[0].id;
		operands[1] = values[1].id;
		pair.id = glsl_emit_op(state, opcode, glsl_emit_type(state, pair.type), operands, 2U);
	}

	/* The product's halves: the high one into the first out argument, the low one into the second; no value. */
	if (special == GLSL_SPECIAL_UMUL_EXTENDED || special == GLSL_SPECIAL_IMUL_EXTENDED) {
		part = emit_value_of(glsl_emit_extract(state, pair, 1U), member);
		if (written[2])
			emit_store(state, &paths[2], part);
		part = emit_value_of(glsl_emit_extract(state, pair, 0U), member);
		if (written[3])
			emit_store(state, &paths[3], part);
		return emit_value_of(0U, node->type);
	}

	/* The second member into the out argument (the last one). */
	index = count - 1U;
	part = emit_value_of(glsl_emit_extract(state, pair, 1U), pair.type->fields[1].type);
	if (written[index])
		emit_store(state, &paths[index], part);

	/* Succeeded: the first member is the call's value. */
	part = emit_value_of(glsl_emit_extract(state, pair, 0U), node->type);
	return part;
}

/*
 * Returns a struct type of two members, for an instruction's result of
 * two values (each call its own; SPIR-V allows equal struct types).
 */
static const struct glsl_type *
emit_pair_type(
	struct emit_state *state,
	const struct glsl_type *first,
	const struct glsl_type *second)
{
	struct glsl_field *fields;
	struct glsl_type *type;

	/* The two members (the arena's memory is zeroed). */
	fields = glsl_alloc(state->module->arena, 2U * sizeof(*fields));
	fields[0].name = "first";
	fields[0].type = first;
	fields[1].name = "second";
	fields[1].type = second;

	/* The struct of them. */
	type = glsl_alloc(state->module->arena, sizeof(*type));
	type->kind = GLSL_KIND_STRUCT;
	type->base = first->base;
	type->components = 1U;
	type->columns = 1U;
	type->name = "pair";
	type->fields = fields;
	type->field_count = 2U;

	/* Succeeded: the struct. */
	return type;
}

/* Returns the id of a 32-bit unsigned constant (ws101-p008). */
static uint32_t
emit_uint(
	struct emit_state *state,
	uint32_t value)
{
	/* The constant of the uint type. */
	return glsl_module_constant(state->module, glsl_emit_type(state, glsl_type_scalar(GLSL_BASE_UINT)), value);
}

/*
 * Reports whether a list of statements returns from inside a loop
 * (ws101-p008): i915's native compiler takes no OpReturn in a loop, so a
 * compute shader's main that does so runs as an inlined function with
 * early returns does.
 */
static int
emit_returns_in_loop(
	const struct glsl_node *node,
	int in_loop)
{
	int inside;
	int found;
	unsigned index;

	/* Each statement, and the statements below it. */
	for (; node != NULL; node = node->next) {
		if (node->kind == GLSL_N_RETURN && in_loop)
			return 1;
		if (node->kind < GLSL_N_BLOCK)
			continue;
		inside = in_loop || node->kind == GLSL_N_FOR || node->kind == GLSL_N_WHILE || node->kind == GLSL_N_DO;
		for (index = 0U; index < 4U; index++) {
			if (node->child[index] == NULL)
				continue;
			found = emit_returns_in_loop(node->child[index], inside);
			if (found)
				return 1;
		}
	}

	/* No return inside a loop. */
	return 0;
}

/*
 * Emits a compute shader's main body inside a loop that runs once
 * (ws101-p008): a return says so in a flag and leaves the innermost
 * construct, loops leave on the flag, and main returns once after the
 * loop, as an inlined function with early returns does.
 */
static void
emit_main_once(
	struct emit_state *state,
	struct glsl_node *body)
{
	struct emit_frame *frame;
	uint32_t operands[2];
	uint32_t header;
	uint32_t body_label;
	uint32_t continue_label;
	uint32_t merge;

	/* The frame and its flag. */
	frame = glsl_alloc(state->module->arena, sizeof(*frame));
	frame->function = state->shader->main;
	frame->returned = emit_variable(state, glsl_type_scalar(GLSL_BASE_BOOL));
	operands[0] = frame->returned;
	operands[1] = glsl_module_bool(state->module, glsl_emit_type(state, glsl_type_scalar(GLSL_BASE_BOOL)), 0);
	glsl_words_add(state->module, &state->module->body, SPV_OP_STORE, operands, 2U);
	state->frame = frame;

	/* The loop that runs once. */
	header = glsl_module_id(state->module);
	body_label = glsl_module_id(state->module);
	continue_label = glsl_module_id(state->module);
	merge = glsl_module_id(state->module);
	emit_branch(state, header);
	emit_label(state, header);
	emit_loop_merge(state, merge, continue_label);
	emit_branch(state, body_label);
	emit_label(state, body_label);
	emit_push_target(state, EMIT_TARGET_FRAME, merge, continue_label);
	frame->target = state->target_count;

	/* The body, then the loop's end (its continue block is never reached). */
	emit_statements(state, body);
	emit_branch(state, merge);
	emit_pop_target(state);
	emit_label(state, continue_label);
	emit_branch(state, header);
	emit_label(state, merge);

	/* Back outside the frame: main returns after the loop. */
	state->frame = NULL;
}

/*
 * Reports whether an instruction is floating-point arithmetic a
 * compiler could contract with another (and NoContraction may decorate).
 */
static int
emit_contractible(
	uint32_t opcode)
{
	/* The additions, subtractions, products and quotients of floats. */
	switch (opcode) {
	case SPV_OP_F_NEGATE:
	case SPV_OP_F_ADD:
	case SPV_OP_F_SUB:
	case SPV_OP_F_MUL:
	case SPV_OP_F_DIV:
	case SPV_OP_VECTOR_TIMES_SCALAR:
	case SPV_OP_MATRIX_TIMES_SCALAR:
	case SPV_OP_VECTOR_TIMES_MATRIX:
	case SPV_OP_MATRIX_TIMES_VECTOR:
	case SPV_OP_MATRIX_TIMES_MATRIX:
	case SPV_OP_DOT:
		return 1;
	default:
		break;
	}

	/* Anything else is not contracted. */
	return 0;
}

/*
 * Emits interpolateAtCentroid, interpolateAtSample or interpolateAtOffset:
 * GLSL.std.450's instruction on the input's place (the variable, or an
 * access chain into it), and the components a swizzle of it picks.  An
 * offset's y points up GL's rows; it is turned into Vulkan's as
 * gl_FragCoord.y is (by gl_ZedFragment's b).
 */
static struct emit_value
emit_interpolate(
	struct emit_state *state,
	struct glsl_node *node)
{
	struct emit_path path;
	struct emit_path whole;
	struct emit_value value;
	struct emit_value extra;
	struct emit_value factors;
	struct glsl_node *interpolant;
	struct glsl_symbol *zed;
	uint32_t operands[4];
	uint32_t parts[2];
	uint32_t turn[2];
	uint32_t scalar;
	unsigned count;
	int status;

	/* The input's place (the checker allows nothing else), without its swizzle. */
	interpolant = node->child[1];
	status = emit_path(state, interpolant, &path);
	if (status != 0) {
		value = emit_value_of(emit_zero(state, node->type), node->type);
		return value;
	}
	whole = path;
	if (path.swizzle_count != 0U) {
		whole.swizzle_count = 0U;
		whole.type = path.vector;
	}

	/* The capability, the set, the instruction, the place. */
	glsl_emit_capability(state, SPV_CAPABILITY_INTERPOLATION_FUNCTION, &state->module->interpolation_function);
	operands[0] = state->module->std450;
	operands[1] = EMIT_INTERPOLATE_AT_CENTROID;
	if (node->builtin->number == GLSL_SPECIAL_INTERPOLATE_AT_SAMPLE)
		operands[1] = EMIT_INTERPOLATE_AT_SAMPLE;
	if (node->builtin->number == GLSL_SPECIAL_INTERPOLATE_AT_OFFSET)
		operands[1] = EMIT_INTERPOLATE_AT_OFFSET;
	operands[2] = emit_chain(state, &whole);
	count = 3U;

	/* The sample's number or the offset (its y turned into Vulkan's rows: b * y, as gl_FragCoord.y turns). */
	extra = emit_value_of(0U, node->type);
	if (interpolant->next != NULL) {
		extra = glsl_emit_expression(state, interpolant->next);
		operands[3] = extra.id;
		count = 4U;
	}
	zed = state->shader->zed_fragment;
	if (node->builtin->number == GLSL_SPECIAL_INTERPOLATE_AT_OFFSET && zed != NULL && zed->used && state->members[zed->uniform] >= 0) {
		factors = emit_value_of(emit_zed_load(state), glsl_type_vector(GLSL_BASE_FLOAT, 4U));
		scalar = glsl_emit_type(state, glsl_type_scalar(GLSL_BASE_FLOAT));
		parts[0] = glsl_emit_extract(state, extra, 0U);
		turn[0] = glsl_emit_extract(state, factors, 1U);
		turn[1] = glsl_emit_extract(state, extra, 1U);
		parts[1] = glsl_emit_op(state, SPV_OP_F_MUL, scalar, turn, 2U);
		operands[3] = glsl_emit_op(state, SPV_OP_COMPOSITE_CONSTRUCT, glsl_emit_type(state, extra.type), parts, 2U);
	}

	/* The interpolated value of the whole vector. */
	value = emit_value_of(glsl_emit_op(state, SPV_OP_EXT_INST, glsl_emit_type(state, whole.type), operands, count), whole.type);

	/* Succeeded: the components a swizzle picks, or the value. */
	if (path.swizzle_count != 0U)
		value = emit_swizzle(state, value, path.swizzle, path.swizzle_count);
	return value;
}

/*
 * Emits dFdy: Vulkan's derivative along the target's rows, times
 * gl_ZedFragment's b, which turns it into GL's rows as gl_FragCoord.y
 * turns (-1 on framebuffer 0, whose rows are the target's turned over).
 * fwidth needs nothing: it adds the derivatives' absolute values.
 */
static struct emit_value
emit_derivative_y(
	struct emit_state *state,
	struct glsl_node *node)
{
	struct emit_value argument;
	struct emit_value factors;
	struct glsl_symbol *zed;
	uint32_t operands[2];
	uint32_t type;
	uint32_t derivative;

	/* The derivative along the target's rows. */
	argument = glsl_emit_expression(state, node->child[1]);
	type = glsl_emit_type(state, node->type);
	derivative = glsl_emit_op(state, SPV_OP_DPDY, type, &argument.id, 1U);

	/* Without the hidden uniform (no fragment shader's default block), the rows are taken as they are. */
	zed = state->shader->zed_fragment;
	if (zed == NULL || !zed->used || state->members[zed->uniform] < 0)
		return emit_value_of(derivative, node->type);

	/* Times b: a scalar's product, or a vector's times a scalar. */
	factors = emit_value_of(emit_zed_load(state), glsl_type_vector(GLSL_BASE_FLOAT, 4U));
	operands[0] = derivative;
	operands[1] = glsl_emit_extract(state, factors, 1U);
	if (node->type->kind == GLSL_KIND_VECTOR) {
		derivative = glsl_emit_op(state, SPV_OP_VECTOR_TIMES_SCALAR, type, operands, 2U);
	} else {
		derivative = glsl_emit_op(state, SPV_OP_F_MUL, type, operands, 2U);
	}

	/* Succeeded: the derivative along GL's rows. */
	return emit_value_of(derivative, node->type);
}
