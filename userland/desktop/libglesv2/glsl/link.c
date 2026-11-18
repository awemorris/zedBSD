/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GLSL compiler's link: a vertex and a fragment shader, and a
 * geometry shader between them when there is one, are matched (uniforms
 * of one name must agree, every input a stage reads must be an output
 * the stage before declares: a geometry shader's inputs are arrays of
 * the vertex shader's outputs), the program's uniforms are laid out once
 * for all (std140 offsets in the default uniform block, sampler bindings
 * from 1), attributes and varyings get their locations, and each stage
 * is emitted as SPIR-V.
 *
 * The link writes what it decides into the shaders' symbols (locations,
 * uniform indices, emission ids), so two links of the same shader must
 * not run at the same time.
 */

#include "emit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The most uniforms and locations a program has. */
#define LINK_MAX_UNIFORMS	256U
#define LINK_MAX_LOCATIONS	32U

/* The most outputs a vertex shader captures (transform feedback). */
#define LINK_MAX_CAPTURES	64U

/* The name prefix of the inputs of the geometry shader the link adds after a capturing tessellation evaluation shader. */
#define LINK_PASS_INPUT		"zed_pass_input_"

/* The binding of the first uniform block (the default block is 0, the samplers 1 to 16). */
#define LINK_FIRST_BLOCK_BINDING 32U

/* The most named uniform blocks a program has (libGLESv2 binds them from 32 up to 55). */
#define LINK_MAX_BLOCKS		24U

/* The most leaves the API lists of a program's uniforms. */
#define LINK_MAX_LEAVES		1024U

/*
 * A link in progress: its arena, where failures jump to, its log, and the
 * program's uniforms.
 */
struct link_state {
	struct glsl_arena arena;
	jmp_buf failure;
	struct glsl_text log;
	unsigned errors;

	/* The uniforms in the order they are laid out. */
	struct glsl_link_uniform uniforms[LINK_MAX_UNIFORMS];
	unsigned uniform_count;

	/* The outputs the vertex shader captures, and the words of a vertex's record. */
	struct glsl_link_capture captures[LINK_MAX_CAPTURES];
	unsigned capture_count;
	unsigned capture_stride;

	/*
	 * The geometry shader the link adds after a capturing tessellation
	 * evaluation shader (NULL: none), its source while it is made, and
	 * the evaluation shader's outputs its inputs read, in order.
	 */
	struct glsl_shader *pass;
	struct glsl_text pass_source;
	struct glsl_symbol *pass_outputs[LINK_MAX_LOCATIONS];
	unsigned pass_output_count;
};

static void link_error(struct link_state *state, const char *format, ...);
static int link_same_type(const struct glsl_type *left, const struct glsl_type *right);
static void link_uniforms(struct link_state *state, struct glsl_shader *shader);
static void link_layout(struct link_state *state);
static void link_blocks(struct link_state *state, struct glsl_shader **stages, unsigned count);
static const char *link_block_name(const struct glsl_symbol *symbol);
static void link_attributes(struct link_state *state, struct glsl_shader *vertex, const struct glsl_binding *bindings, unsigned binding_count);
static void link_varyings(struct link_state *state, struct glsl_shader *producer, struct glsl_shader *consumer);
static const struct glsl_type *link_input_type(const struct glsl_shader *consumer, const struct glsl_symbol *input);
static const struct glsl_type *link_output_type(const struct glsl_shader *producer, const struct glsl_symbol *output);
static unsigned link_place_outputs(struct glsl_shader *producer, unsigned patch, unsigned location);
static void link_outputs(struct link_state *state, struct glsl_shader *fragment);
static struct glsl_symbol *link_find(struct glsl_shader *shader, const char *name, unsigned where);
static struct glsl_symbol *link_find_varying(struct glsl_shader *producer, struct glsl_symbol *input, const struct glsl_type *type);
static int link_take_output(struct link_state *state, struct glsl_symbol *symbol, unsigned char taken[2][LINK_MAX_LOCATIONS]);
static void link_info(struct link_state *state, struct glsl_program *program);
static void link_leaves(struct link_state *state, const struct glsl_type *type, const char *name, struct glsl_uniform_info *out, unsigned *count, unsigned capacity);
static void link_block_infos(struct link_state *state, struct glsl_shader *shader, unsigned stage, struct glsl_program *program);
static void link_block_element(struct link_state *state, const struct glsl_symbol *symbol, const struct glsl_type *type, unsigned element, unsigned count, unsigned stage, struct glsl_program *program);
static unsigned link_block_first(const struct glsl_program *program, unsigned binding);
static void link_block_order(struct glsl_program *program);
static int link_block_before(const struct glsl_block_info *block, const struct glsl_block_info *other);
static void link_member_leaves(struct link_state *state, const struct glsl_type *type, const char *name, unsigned offset, unsigned row_major, int block, struct glsl_program *program);
static void link_captures(struct link_state *state, struct glsl_shader *shader, const char *const *captures, unsigned capture_count);
static void link_capture_stage(const struct glsl_shader *shader, struct glsl_program *program);
static struct glsl_shader *link_capturing(struct link_state *state, struct glsl_shader *vertex, struct glsl_shader *evaluation, struct glsl_shader *geometry);
static void link_pass(struct link_state *state, struct glsl_shader *evaluation);
static void link_pass_names(struct link_state *state);
static void link_release(struct link_state *state);
static unsigned link_capture_words(const struct glsl_type *type);
static void link_capture_infos(struct link_state *state, struct glsl_program *program);
static char *link_name(struct link_state *state, const char *name);
static uint32_t *link_copy(const uint32_t *code, size_t words);
static void link_storages(struct link_state *state, struct glsl_shader *compute, struct glsl_program *program);
static void link_images(struct link_state *state, struct glsl_shader *compute, struct glsl_program *program);
static void link_buffer_top(struct link_state *state, const struct glsl_type *type, const char *name, unsigned offset, unsigned block, struct glsl_program *program);
static void link_buffer_leaves(struct link_state *state, const struct glsl_type *type, const char *name, unsigned offset, unsigned block, unsigned top_size, unsigned top_stride, struct glsl_program *program);

/*
 * Links a vertex and a fragment shader into SPIR-V.
 */
int
glsl_link(
	const struct glsl_shader *vertex_shader,
	const struct glsl_shader *fragment_shader,
	const struct glsl_binding *bindings,
	unsigned binding_count,
	struct glsl_program *program,
	char **log)
{
	int status;

	/* A link that captures nothing. */
	status = glsl_link_captured(vertex_shader, fragment_shader, bindings, binding_count, NULL, 0U, program, log);
	if (status != 0)
		return -1;

	/* Succeeded: the program. */
	return 0;
}

/*
 * Links a vertex and a fragment shader into SPIR-V, the vertex shader
 * capturing the outputs of the names given.
 */
int
glsl_link_captured(
	const struct glsl_shader *vertex_shader,
	const struct glsl_shader *fragment_shader,
	const struct glsl_binding *bindings,
	unsigned binding_count,
	const char *const *captures,
	unsigned capture_count,
	struct glsl_program *program,
	char **log)
{
	int status;

	/* A link without a geometry shader. */
	status = glsl_link_stages(vertex_shader, NULL, fragment_shader, bindings, binding_count, captures, capture_count, program, log);
	if (status != 0)
		return -1;

	/* Succeeded: the program. */
	return 0;
}

/*
 * Links a vertex shader, a geometry shader (NULL for none) and a fragment
 * shader into SPIR-V, the last of the first two capturing the outputs of
 * the names given.
 */
int
glsl_link_stages(
	const struct glsl_shader *vertex_shader,
	const struct glsl_shader *geometry_shader,
	const struct glsl_shader *fragment_shader,
	const struct glsl_binding *bindings,
	unsigned binding_count,
	const char *const *captures,
	unsigned capture_count,
	struct glsl_program *program,
	char **log)
{
	int status;

	/* A link without tessellation shaders. */
	status = glsl_link_pipeline(vertex_shader, NULL, NULL, geometry_shader, fragment_shader, bindings, binding_count, captures,
				    capture_count, program, log);
	if (status != 0)
		return -1;

	/* Succeeded: the program. */
	return 0;
}

/*
 * Links the stages of a draw into SPIR-V: a vertex shader, the two
 * tessellation shaders (both NULL for none), a geometry shader (NULL for
 * none) and a fragment shader, the last stage before the fragment shader
 * capturing the outputs of the names given (after a tessellation
 * evaluation shader, a geometry shader the link adds that passes its
 * primitives on).
 */
int
glsl_link_pipeline(
	const struct glsl_shader *vertex_shader,
	const struct glsl_shader *tess_control_shader,
	const struct glsl_shader *tess_evaluation_shader,
	const struct glsl_shader *geometry_shader,
	const struct glsl_shader *fragment_shader,
	const struct glsl_binding *bindings,
	unsigned binding_count,
	const char *const *captures,
	unsigned capture_count,
	struct glsl_program *program,
	char **log)
{
	struct link_state *volatile state;
	struct glsl_shader *vertex;
	struct glsl_shader *tess_control;
	struct glsl_shader *tess_evaluation;
	struct glsl_shader *geometry;
	struct glsl_shader *fragment;
	struct glsl_shader *capturing;
	struct glsl_shader *stages[5];
	const struct glsl_link_capture *stage_captures;
	unsigned stage_capture_count;
	int counted;
	uint32_t *code;
	size_t words;
	unsigned count;
	unsigned index;
	unsigned stage;

	/* Nothing yet. */
	*log = NULL;
	memset(program, 0, sizeof(*program));
	state = calloc(1U, sizeof(*state));
	if (state == NULL)
		return -1;
	state->arena.failure = &state->failure;

	/* The link records its decisions in the shaders' symbols. */
	vertex = (struct glsl_shader *)vertex_shader;
	tess_control = (struct glsl_shader *)tess_control_shader;
	tess_evaluation = (struct glsl_shader *)tess_evaluation_shader;
	geometry = (struct glsl_shader *)geometry_shader;
	fragment = (struct glsl_shader *)fragment_shader;

	/*
	 * Out of memory comes back here.  setjmp is the whole controlling
	 * expression, the form C allows it in.
	 */
	switch (setjmp(state->failure)) {
	case 0:
		break;
	default:
		/* The link failed: what it made goes, and the log (if any) tells why. */
		glsl_program_free(program);
		link_release(state);
		*log = state->log.data;
		free(state);
		return -1;
	}

	/* The stages, and one language for all. */
	if (vertex->stage != GLSL_STAGE_VERTEX || fragment->stage != GLSL_STAGE_FRAGMENT)
		link_error(state, "a program needs a vertex and a fragment shader");
	if (geometry != NULL && geometry->stage != GLSL_STAGE_GEOMETRY)
		link_error(state, "the shader between the vertex and the fragment shader is not a geometry shader");
	if (tess_control != NULL && tess_control->stage != GLSL_STAGE_TESS_CONTROL)
		link_error(state, "the tessellation control shader is of another stage");
	if (tess_evaluation != NULL && tess_evaluation->stage != GLSL_STAGE_TESS_EVALUATION)
		link_error(state, "the tessellation evaluation shader is of another stage");
	if (tess_control != NULL && tess_evaluation == NULL)
		link_error(state, "a tessellation control shader needs a tessellation evaluation shader");
	if (tess_control == NULL && tess_evaluation != NULL)
		link_error(state, "a tessellation evaluation shader without a control shader is not supported");
	if (vertex->es != fragment->es)
		link_error(state, "OpenGL ES and desktop GLSL shaders cannot be linked together");
	if (state->errors != 0U)
		longjmp(state->failure, 1);

	/*
	 * The shader that captures: the last before the fragment shader, a
	 * geometry shader the link makes when that is a tessellation
	 * evaluation shader (it passes each primitive on as it is).
	 */
	if (capture_count != 0U && geometry == NULL && tess_evaluation != NULL)
		link_pass(state, tess_evaluation);
	if (state->errors != 0U)
		longjmp(state->failure, 1);
	capturing = link_capturing(state, vertex, tess_evaluation, geometry);

	/* The stages in order: the vertex, the tessellation and the geometry shaders (if any) and the fragment shader. */
	count = 0U;
	stages[count++] = vertex;
	if (tess_control != NULL) {
		stages[count++] = tess_control;
		stages[count++] = tess_evaluation;
	}
	if (geometry != NULL)
		stages[count++] = geometry;
	if (state->pass != NULL)
		stages[count++] = state->pass;
	stages[count++] = fragment;

	/* The uniforms, laid out; the attributes, the varyings between each stage and the next, and the outputs, located. */
	for (index = 0U; index < count; index++)
		link_uniforms(state, stages[index]);
	link_layout(state);
	link_blocks(state, stages, count);
	link_attributes(state, vertex, bindings, binding_count);
	for (index = 0U; index + 1U < count; index++)
		link_varyings(state, stages[index], stages[index + 1U]);

	/* The fragment outputs and the captured outputs. */
	link_outputs(state, fragment);
	link_captures(state, capturing, captures, capture_count);
	if (state->errors != 0U)
		longjmp(state->failure, 1);

	/* Each stage's SPIR-V (the capturing stage's with its captures), copied out of the arena. */
	stage_captures = NULL;
	stage_capture_count = 0U;
	if (capturing == vertex) {
		stage_captures = state->captures;
		stage_capture_count = state->capture_count;
	}
	code = glsl_emit(vertex, &state->arena, state->uniforms, state->uniform_count, stage_captures, stage_capture_count,
			 state->capture_stride, &words);
	program->code[0] = link_copy(code, words);
	program->words[0] = words;
	code = glsl_emit(fragment, &state->arena, state->uniforms, state->uniform_count, NULL, 0U, 0U, &words);
	program->code[1] = link_copy(code, words);
	program->words[1] = words;
	if (program->code[0] == NULL || program->code[1] == NULL)
		longjmp(state->failure, 1);

	/* The tessellation and geometry stages', when there are some. */
	for (index = 1U; index + 1U < count; index++) {
		stage = stages[index]->stage;
		stage_captures = NULL;
		stage_capture_count = 0U;
		if (stages[index] == capturing) {
			stage_captures = state->captures;
			stage_capture_count = state->capture_count;
		}
		code = glsl_emit(stages[index], &state->arena, state->uniforms, state->uniform_count, stage_captures, stage_capture_count,
				 state->capture_stride, &words);
		program->code[stage] = link_copy(code, words);
		program->words[stage] = words;
		if (program->code[stage] == NULL)
			longjmp(state->failure, 1);
	}

	/* What the API reports of the uniforms, the uniform blocks and the captured outputs. */
	link_info(state, program);
	link_block_infos(state, vertex, GLSL_STAGE_VERTEX, program);
	link_block_infos(state, fragment, GLSL_STAGE_FRAGMENT, program);
	for (index = 1U; index + 1U < count; index++)
		link_block_infos(state, stages[index], stages[index]->stage, program);
	link_block_order(program);
	link_capture_infos(state, program);
	counted = glsl_capture_counted(capturing, state->capture_count);
	if (state->capture_count != 0U || counted)
		link_capture_stage(capturing, program);

	/* Succeeded: the program. */
	link_release(state);
	free(state->log.data);
	free(state);
	return 0;
}

/* Returns the shader that captures: the geometry shader (the application's, or the one the link added), the evaluation shader, or the vertex shader. */
static struct glsl_shader *
link_capturing(
	struct link_state *state,
	struct glsl_shader *vertex,
	struct glsl_shader *evaluation,
	struct glsl_shader *geometry)
{
	/* A geometry shader of the application's or the link's. */
	if (geometry != NULL)
		return geometry;
	if (state->pass != NULL)
		return state->pass;

	/* Else the last before the fragment shader. */
	if (evaluation != NULL)
		return evaluation;

	/* Succeeded: the vertex shader. */
	return vertex;
}

/* Frees what a link made besides the program and the log: the arena, and the geometry shader it added with its source. */
static void
link_release(
	struct link_state *state)
{
	/* The arena. */
	glsl_arena_free(&state->arena);

	/* The added geometry shader and its source. */
	if (state->pass != NULL)
		glsl_shader_free(state->pass);
	state->pass = NULL;
	free(state->pass_source.data);
	memset(&state->pass_source, 0, sizeof(state->pass_source));
}

/*
 * Makes the geometry shader a link adds after a tessellation evaluation
 * shader whose outputs are captured (transform feedback captures what a
 * geometry shader emits): GLSL 4.00 that takes the evaluation shader's
 * primitive (a point in point mode, a line of isolines, else a triangle)
 * and emits its vertices as they are, gl_Position and each output of a
 * number, vector or matrix (the interpolation kept), as one strip.  Its
 * inputs are named after the outputs they read once it compiles
 * (link_pass_names).  Errors go to the log.
 */
static void
link_pass(
	struct link_state *state,
	struct glsl_shader *evaluation)
{
	static const char *const qualifiers[4] = { "", "", "flat ", "noperspective " };
	struct glsl_symbol *symbol;
	const char *input;
	const char *output;
	unsigned vertices;
	unsigned kind;
	unsigned index;
	char *log;

	/* The primitive: a point, a line or a triangle. */
	input = "triangles";
	output = "triangle_strip";
	vertices = 3U;
	if (evaluation->tess_domain == GLSL_PRIMITIVE_ISOLINES) {
		input = "lines";
		output = "line_strip";
		vertices = 2U;
	}
	if (evaluation->tess_point_mode) {
		input = "points";
		output = "points";
		vertices = 1U;
	}
	glsl_text_printf(&state->pass_source, &state->failure,
			 "#version 400\nlayout(%s) in;\nlayout(%s, max_vertices = %u) out;\n", input, output, vertices);

	/* Each output of the evaluation shader: an input array of it and an output of its name. */
	state->pass_output_count = 0U;
	for (symbol = evaluation->globals; symbol != NULL; symbol = symbol->next_global) {
		if (symbol->kind != GLSL_SYMBOL_VARIABLE || symbol->where != GLSL_VAR_OUTPUT || symbol->builtin != GLSL_BUILTIN_NONE)
			continue;
		kind = symbol->type->kind;
		if (kind != GLSL_KIND_SCALAR && kind != GLSL_KIND_VECTOR && kind != GLSL_KIND_MATRIX) {
			link_error(state, "transform feedback after a tessellation evaluation shader cannot pass its output '%s' on",
				   symbol->name);
			continue;
		}
		if (state->pass_output_count == LINK_MAX_LOCATIONS) {
			link_error(state, "too many varyings (16 locations)");
			break;
		}

		/* The pair, the input numbered by its place. */
		glsl_text_printf(&state->pass_source, &state->failure, "in %s " LINK_PASS_INPUT "%u[];\n%sout %s %s;\n",
				 symbol->type->name, state->pass_output_count, qualifiers[symbol->interpolation & 3U], symbol->type->name,
				 symbol->name);
		state->pass_outputs[state->pass_output_count] = symbol;
		state->pass_output_count++;
	}
	if (state->errors != 0U)
		return;

	/* main: each vertex of the primitive emitted with its values, then the strip's end. */
	glsl_text_printf(&state->pass_source, &state->failure,
			 "void main()\n{\n\tint i;\n\tfor (i = 0; i < %u; i++) {\n\t\tgl_Position = gl_in[i].gl_Position;\n"
			 "\t\tgl_PrimitiveID = gl_PrimitiveIDIn;\n", vertices);
	for (index = 0U; index < state->pass_output_count; index++)
		glsl_text_printf(&state->pass_source, &state->failure, "\t\t%s = " LINK_PASS_INPUT "%u[i];\n",
				 state->pass_outputs[index]->name, index);
	glsl_text_printf(&state->pass_source, &state->failure, "\t\tEmitVertex();\n\t}\n\tEndPrimitive();\n}\n");

	/* Compiled with the evaluation shader's device features. */
	state->pass = glsl_compile_features(GLSL_STAGE_GEOMETRY, state->pass_source.data, 400U, evaluation->features, &log);
	if (state->pass == NULL) {
		link_error(state, "the geometry shader that passes the tessellation evaluation shader's primitives on did not compile");
		if (log != NULL)
			glsl_text_printf(&state->log, &state->failure, "%s", log);
		free(log);
		return;
	}
	free(log);

	/* Its inputs named after the outputs they read. */
	link_pass_names(state);
}

/* Names the added geometry shader's inputs after the evaluation shader's outputs they read (by the number in their names). */
static void
link_pass_names(
	struct link_state *state)
{
	struct glsl_symbol *symbol;
	size_t prefix;
	unsigned long number;
	int differs;

	/* Each input of the prefix. */
	prefix = strlen(LINK_PASS_INPUT);
	for (symbol = state->pass->globals; symbol != NULL; symbol = symbol->next_global) {
		if (symbol->where != GLSL_VAR_INPUT || symbol->builtin != GLSL_BUILTIN_NONE)
			continue;
		differs = strncmp(symbol->name, LINK_PASS_INPUT, prefix);
		if (differs != 0)
			continue;

		/* The output of its number. */
		number = strtoul(symbol->name + prefix, NULL, 10);
		if (number < state->pass_output_count)
			symbol->name = state->pass_outputs[number]->name;
	}
}

/*
 * Frees what a successful link gave.
 */
void
glsl_program_free(
	struct glsl_program *program)
{
	unsigned index;

	/* The SPIR-V of each stage. */
	for (index = 0U; index < GLSL_STAGES; index++) {
		free(program->code[index]);
		program->code[index] = NULL;
		program->words[index] = 0U;
	}

	/* The uniforms' names and the list. */
	for (index = 0U; index < program->uniform_count; index++)
		free(program->uniforms[index].name);
	free(program->uniforms);
	program->uniforms = NULL;
	program->uniform_count = 0U;

	/* The blocks' names and the list. */
	for (index = 0U; index < program->block_count; index++)
		free(program->blocks[index].name);
	free(program->blocks);
	program->blocks = NULL;
	program->block_count = 0U;

	/* The captured outputs' names and the list. */
	for (index = 0U; index < program->capture_count; index++)
		free(program->captures[index].name);
	free(program->captures);
	program->captures = NULL;
	program->capture_count = 0U;
	program->capture_stride = 0U;
	program->capture_stage = 0U;
	program->capture_primitive = 0U;
	program->capture_vertices = 0U;
	program->capture_invocations = 0U;

	/* A compute program's storage blocks' names and the list (ws101-p008). */
	for (index = 0U; index < program->storage_count; index++)
		free(program->storages[index].name);
	free(program->storages);
	program->storages = NULL;
	program->storage_count = 0U;

	/* A compute program's image uniforms' names and the list. */
	for (index = 0U; index < program->image_count; index++)
		free(program->images[index].name);
	free(program->images);
	program->images = NULL;
	program->image_count = 0U;

	/* A compute program's buffer variables' names and the list. */
	for (index = 0U; index < program->buffer_count; index++)
		free(program->buffers[index].name);
	free(program->buffers);
	program->buffers = NULL;
	program->buffer_count = 0U;
}

/* Reports a link error in the log. */
static void
link_error(
	struct link_state *state,
	const char *format,
	...)
{
	va_list arguments;

	/* The message and its end of line. */
	va_start(arguments, format);
	glsl_text_append(&state->log, "error: ", 7U, &state->failure);
	glsl_text_vprintf(&state->log, &state->failure, format, arguments);
	glsl_text_append(&state->log, "\n", 1U, &state->failure);
	va_end(arguments);

	/* Counted. */
	state->errors++;
}

/* Reports whether two types (possibly of two shaders) are the same type. */
static int
link_same_type(
	const struct glsl_type *left,
	const struct glsl_type *right)
{
	unsigned index;
	int differs;
	int same;

	/* Built-in types are the same objects. */
	if (left == right)
		return 1;
	if (left->kind != right->kind)
		return 0;

	/* Arrays of the same element and length. */
	if (left->kind == GLSL_KIND_ARRAY) {
		if (left->length != right->length)
			return 0;
		same = link_same_type(left->element, right->element);
		return same;
	}

	/* Structs of the same name and members. */
	if (left->kind != GLSL_KIND_STRUCT || left->field_count != right->field_count)
		return 0;
	differs = strcmp(left->name, right->name);
	if (differs != 0)
		return 0;
	for (index = 0U; index < left->field_count; index++) {
		differs = strcmp(left->fields[index].name, right->fields[index].name);
		if (differs != 0)
			return 0;
		same = link_same_type(left->fields[index].type, right->fields[index].type);
		if (!same)
			return 0;
	}

	/* Succeeded: the same. */
	return 1;
}

/* Adds a stage's used uniforms to the program's (one of a name, which both stages must agree on). */
static void
link_uniforms(
	struct link_state *state,
	struct glsl_shader *shader)
{
	struct glsl_symbol *symbol;
	unsigned index;
	int differs;
	int same;

	/* Each uniform the stage uses. */
	for (symbol = shader->globals; symbol != NULL; symbol = symbol->next_global) {
		if (!symbol->used || symbol->where != GLSL_VAR_UNIFORM)
			continue;

		/* The uniform of the same name the other stage added, if any. */
		for (index = 0U; index < state->uniform_count; index++) {
			differs = strcmp(state->uniforms[index].name, symbol->name);
			if (differs == 0)
				break;
		}

		/* One the other stage has: the same type. */
		if (index < state->uniform_count) {
			same = link_same_type(state->uniforms[index].type, symbol->type);
			if (!same)
				link_error(state, "uniform '%s' has different types in the two shaders", symbol->name);
			symbol->uniform = index;
			continue;
		}

		/* A new one. */
		if (state->uniform_count == LINK_MAX_UNIFORMS) {
			link_error(state, "too many uniforms");
			return;
		}

		/* The uniform, with its index. */
		state->uniforms[state->uniform_count].name = symbol->name;
		state->uniforms[state->uniform_count].type = symbol->type;
		state->uniforms[state->uniform_count].sampler = glsl_type_sampler_units(symbol->type);
		symbol->uniform = state->uniform_count;
		state->uniform_count++;
	}
}

/*
 * Lays the uniforms out: std140 offsets in the block, bindings from 1 for
 * samplers (an array of samplers is one binding with a descriptor an
 * element, ws068-p004).
 */
static void
link_layout(
	struct link_state *state)
{
	struct glsl_link_uniform *uniform;
	uint32_t offset;
	uint32_t binding;
	unsigned alignment;
	unsigned units;
	unsigned index;

	/* Each uniform in order. */
	offset = 0U;
	binding = 1U;
	units = 0U;
	for (index = 0U; index < state->uniform_count; index++) {
		uniform = &state->uniforms[index];

		/* A sampler (or an array of them) takes the next binding, and a unit an element. */
		if (uniform->sampler != 0U) {
			uniform->binding = binding;
			binding++;
			units += uniform->sampler;
			continue;
		}

		/* Any other takes the next offset of its alignment. */
		alignment = glsl_std140_alignment(uniform->type);
		offset = (offset + alignment - 1U) & ~(alignment - 1U);
		uniform->offset = offset;
		offset += glsl_std140_size(uniform->type);
	}

	/* libGLESv2 binds sixteen texture units. */
	if (units > 16U)
		link_error(state, "more than 16 samplers");
}

/*
 * Gives the uniform blocks their bindings: 32 on, in the order the stages
 * (vertex, geometry, fragment) use them; a block of one name in several
 * stages is one block (and must be the same).  An array of blocks is one
 * binding of a descriptor an element; the descriptors of all the
 * bindings are at most LINK_MAX_BLOCKS.
 */
static void
link_blocks(
	struct link_state *state,
	struct glsl_shader **stages,
	unsigned count)
{
	struct glsl_symbol *symbol;
	struct glsl_symbol *other;
	unsigned binding;
	unsigned descriptors;
	unsigned stage;
	unsigned earlier;
	int differs;
	int same;

	/* Each stage's blocks in order: an earlier stage's binding for a block of the same name, the next for a new one. */
	binding = LINK_FIRST_BLOCK_BINDING;
	descriptors = 0U;
	for (stage = 0U; stage < count; stage++) {
		for (symbol = stages[stage]->globals; symbol != NULL; symbol = symbol->next_global) {
			if (!symbol->used || symbol->where != GLSL_VAR_BLOCK)
				continue;

			/* The same block in an earlier stage. */
			other = NULL;
			for (earlier = 0U; earlier < stage && other == NULL; earlier++) {
				for (other = stages[earlier]->globals; other != NULL; other = other->next_global) {
					if (!other->used || other->where != GLSL_VAR_BLOCK)
						continue;
					differs = strcmp(link_block_name(other), link_block_name(symbol));
					if (differs == 0)
						break;
				}
			}

			/* A new block takes the next binding, and its descriptors (an array's length). */
			if (other == NULL) {
				symbol->binding = binding;
				binding++;
				descriptors++;
				if (symbol->type->kind == GLSL_KIND_ARRAY)
					descriptors += symbol->type->length - 1U;
				if (descriptors > LINK_MAX_BLOCKS) {
					link_error(state, "more than %u uniform blocks", LINK_MAX_BLOCKS);
					longjmp(state->failure, 1);
				}
				continue;
			}

			/* The same block: its binding, and the same members (and the same array's length). */
			same = link_same_type(other->type, symbol->type);
			if (!same)
				link_error(state, "uniform block '%s' differs between the shaders", link_block_name(symbol));
			symbol->binding = other->binding;
		}
	}
}

/* Returns a uniform block's name: its struct's (an array of blocks' element's). */
static const char *
link_block_name(
	const struct glsl_symbol *symbol)
{
	/* An array's element. */
	if (symbol->type->kind == GLSL_KIND_ARRAY)
		return symbol->type->element->name;

	/* Succeeded: the struct's name. */
	return symbol->type->name;
}

/* Gives the vertex shader's attributes their locations: the bound ones, then the lowest free ones in order. */
static void
link_attributes(
	struct link_state *state,
	struct glsl_shader *vertex,
	const struct glsl_binding *bindings,
	unsigned binding_count)
{
	struct glsl_symbol *symbol;
	unsigned char taken[LINK_MAX_LOCATIONS];
	unsigned locations;
	unsigned location;
	unsigned index;
	unsigned slot;
	int bound;
	int differs;
	int free_run;

	/* The attributes with a layout location first. */
	memset(taken, 0, sizeof(taken));
	for (symbol = vertex->globals; symbol != NULL; symbol = symbol->next_global) {
		symbol->location = GLSL_NO_LOCATION;
		if (!symbol->used || symbol->where != GLSL_VAR_INPUT || symbol->builtin != GLSL_BUILTIN_NONE)
			continue;
		if (symbol->explicit_location == GLSL_NO_LOCATION)
			continue;
		locations = glsl_type_locations(symbol->type);
		if (symbol->explicit_location + locations > 16U) {
			link_error(state, "attribute '%s' has a location beyond the 16", symbol->name);
			continue;
		}

		/* The location, taken. */
		symbol->location = symbol->explicit_location;
		for (slot = 0U; slot < locations; slot++)
			taken[symbol->location + slot] = 1U;
	}

	/* Then the bound attributes. */
	for (symbol = vertex->globals; symbol != NULL; symbol = symbol->next_global) {
		if (!symbol->used || symbol->where != GLSL_VAR_INPUT || symbol->builtin != GLSL_BUILTIN_NONE)
			continue;
		if (symbol->location != GLSL_NO_LOCATION)
			continue;
		for (index = 0U; index < binding_count; index++) {
			differs = strcmp(bindings[index].name, symbol->name);
			if (differs != 0)
				continue;

			/* The bound location, inside the sixteen. */
			locations = glsl_type_locations(symbol->type);
			if (bindings[index].location + locations > 16U) {
				link_error(state, "attribute '%s' is bound beyond the 16 locations", symbol->name);
				break;
			}

			/* The bound location, taken. */
			symbol->location = bindings[index].location;
			for (slot = 0U; slot < locations; slot++)
				taken[symbol->location + slot] = 1U;
			break;
		}
	}

	/* The others at the lowest free locations that fit them. */
	for (symbol = vertex->globals; symbol != NULL; symbol = symbol->next_global) {
		if (!symbol->used || symbol->where != GLSL_VAR_INPUT || symbol->builtin != GLSL_BUILTIN_NONE)
			continue;
		if (symbol->location != GLSL_NO_LOCATION)
			continue;
		locations = glsl_type_locations(symbol->type);
		bound = 0;
		for (location = 0U; location + locations <= 16U && !bound; location++) {
			free_run = 1;
			for (slot = 0U; slot < locations; slot++) {
				if (taken[location + slot])
					free_run = 0;
			}

			/* A run of taken locations is passed over. */
			if (!free_run)
				continue;

			/* The attribute takes the run. */
			symbol->location = location;
			for (slot = 0U; slot < locations; slot++)
				taken[location + slot] = 1U;
			bound = 1;
		}

		/* No run fits it. */
		if (!bound)
			link_error(state, "too many attributes (16 locations)");
	}
}

/*
 * Gives the varyings between two stages their locations: the producer's
 * outputs in declaration order (those either stage uses), and each input
 * of the consumer the location of the output of its name (a geometry
 * shader's input is an array of the output's type).
 */
static void
link_varyings(
	struct link_state *state,
	struct glsl_shader *producer,
	struct glsl_shader *consumer)
{
	struct glsl_symbol *symbol;
	struct glsl_symbol *output;
	const struct glsl_type *type;
	const struct glsl_type *output_type;
	unsigned location;
	int same;

	/* An input needs an output of its name and type, which is then declared. */
	for (symbol = consumer->globals; symbol != NULL; symbol = symbol->next_global) {
		if (!symbol->used || symbol->where != GLSL_VAR_INPUT || symbol->builtin != GLSL_BUILTIN_NONE)
			continue;
		type = link_input_type(consumer, symbol);
		output = link_find_varying(producer, symbol, type);
		if (output == NULL) {
			link_error(state, "a shader reads '%s', which the shader before it does not declare", symbol->name);
			continue;
		}

		/* Of the same type, both per vertex or both per patch. */
		output_type = link_output_type(producer, output);
		same = link_same_type(output_type, type);
		if (!same) {
			link_error(state, "varying '%s' has different types in the two shaders", symbol->name);
			continue;
		}
		if (output->patch != symbol->patch) {
			link_error(state, "varying '%s' is a patch variable in one shader only", symbol->name);
			continue;
		}

		/* The output is declared even when the producer does not use it. */
		output->used = 1U;
	}

	/* The producer's outputs in order, the per-vertex ones first and then a control shader's patch ones (one space of locations). */
	location = link_place_outputs(producer, 0U, 0U);
	location = link_place_outputs(producer, 1U, location);

	/* Sixteen locations in all. */
	if (location > 16U)
		link_error(state, "too many varyings (16 locations)");

	/* The inputs take their outputs' locations (and interpolation). */
	for (symbol = consumer->globals; symbol != NULL; symbol = symbol->next_global) {
		if (!symbol->used || symbol->where != GLSL_VAR_INPUT || symbol->builtin != GLSL_BUILTIN_NONE)
			continue;
		type = link_input_type(consumer, symbol);
		output = link_find_varying(producer, symbol, type);
		if (output == NULL)
			continue;
		symbol->location = output->location;
		if (output->interpolation == GLSL_INTERP_FLAT)
			symbol->interpolation = GLSL_INTERP_FLAT;
	}
}

/*
 * Returns the type of the output an input reads: its own, or the element
 * of a geometry shader's input array or a tessellation shader's
 * per-vertex input array.
 */
static const struct glsl_type *
link_input_type(
	const struct glsl_shader *consumer,
	const struct glsl_symbol *input)
{
	/* A geometry shader reads an array of the vertices' values. */
	if (consumer->stage == GLSL_STAGE_GEOMETRY && input->type->kind == GLSL_KIND_ARRAY)
		return input->type->element;

	/* So does a tessellation shader, but for its patch inputs. */
	if ((consumer->stage == GLSL_STAGE_TESS_CONTROL || consumer->stage == GLSL_STAGE_TESS_EVALUATION) &&
	    !input->patch &&
	    input->type->kind == GLSL_KIND_ARRAY)
		return input->type->element;

	/* Succeeded: the input's own type. */
	return input->type;
}

/* Returns the type of one vertex's value of an output: its own, or the element of a tessellation control shader's per-vertex output array. */
static const struct glsl_type *
link_output_type(
	const struct glsl_shader *producer,
	const struct glsl_symbol *output)
{
	/* A control shader writes an array of its patch's vertices' values. */
	if (producer->stage == GLSL_STAGE_TESS_CONTROL && !output->patch && output->type->kind == GLSL_KIND_ARRAY)
		return output->type->element;

	/* Succeeded: the output's own type. */
	return output->type;
}

/*
 * Gives a stage's used outputs (its per-vertex ones, or its patch ones)
 * locations in order from a location, a per-vertex array of a control
 * shader as many as one vertex's value needs.  Returns the location after
 * them.
 */
static unsigned
link_place_outputs(
	struct glsl_shader *producer,
	unsigned patch,
	unsigned location)
{
	struct glsl_symbol *symbol;
	const struct glsl_type *type;

	/* Each output of the kind, in order. */
	for (symbol = producer->globals; symbol != NULL; symbol = symbol->next_global) {
		if (!symbol->used || symbol->where != GLSL_VAR_OUTPUT || symbol->builtin != GLSL_BUILTIN_NONE)
			continue;
		if (symbol->patch != patch)
			continue;
		symbol->location = location;
		type = link_output_type(producer, symbol);
		location += glsl_type_locations(type);
	}

	/* Succeeded: the next free location. */
	return location;
}

/* Gives the fragment shader's outputs their locations: gl_FragColor and gl_FragData 0, the others in order. */
static void
link_outputs(
	struct link_state *state,
	struct glsl_shader *fragment)
{
	struct glsl_symbol *symbol;
	unsigned char taken[2][LINK_MAX_LOCATIONS];
	unsigned location;
	unsigned builtins;
	unsigned outputs;
	unsigned unlocated;

	/* gl_FragColor and gl_FragData are colour output 0. */
	builtins = 0U;
	for (symbol = fragment->globals; symbol != NULL; symbol = symbol->next_global) {
		if (!symbol->used || symbol->where != GLSL_VAR_OUTPUT)
			continue;
		if (symbol->builtin == GLSL_BUILTIN_FRAG_COLOR || symbol->builtin == GLSL_BUILTIN_FRAG_DATA) {
			symbol->location = 0U;
			builtins++;
		}
	}

	/* The shader's own outputs: those with a layout location first. */
	memset(taken, 0, sizeof(taken));
	outputs = 0U;
	unlocated = 0U;
	for (symbol = fragment->globals; symbol != NULL; symbol = symbol->next_global) {
		if (!symbol->used || symbol->where != GLSL_VAR_OUTPUT || symbol->builtin != GLSL_BUILTIN_NONE)
			continue;
		outputs++;
		symbol->location = GLSL_NO_LOCATION;
		if (symbol->explicit_location == GLSL_NO_LOCATION) {
			unlocated++;
			continue;
		}

		/* The layout's location, taken. */
		symbol->location = symbol->explicit_location;
		(void)link_take_output(state, symbol, taken);
	}

	/* Then the others at the lowest free locations, in order. */
	for (symbol = fragment->globals; symbol != NULL; symbol = symbol->next_global) {
		if (!symbol->used || symbol->where != GLSL_VAR_OUTPUT || symbol->builtin != GLSL_BUILTIN_NONE)
			continue;
		if (symbol->location != GLSL_NO_LOCATION)
			continue;
		for (location = 0U; location < LINK_MAX_LOCATIONS; location++) {
			if (taken[0][location])
				continue;
			symbol->location = location;
			break;
		}

		/* The locations taken. */
		(void)link_take_output(state, symbol, taken);
	}

	/* OpenGL ES 3.00 wants every output located when there are several. */
	if (fragment->es && outputs > 1U && unlocated != 0U)
		link_error(state, "with several fragment shader outputs, each needs a layout location");

	/* Both kinds cannot be written together. */
	if (builtins > 1U || (builtins != 0U && outputs != 0U))
		link_error(state, "the fragment shader writes more than one of gl_FragColor, gl_FragData and its own outputs");
}

/* Marks the locations a fragment output takes; -1 (reported) when one is taken already or out of range. */
static int
link_take_output(
	struct link_state *state,
	struct glsl_symbol *symbol,
	unsigned char taken[2][LINK_MAX_LOCATIONS])
{
	unsigned char *row;
	unsigned locations;
	unsigned slot;

	/* Inside the range. */
	locations = glsl_type_locations(symbol->type);
	if (symbol->location + locations > LINK_MAX_LOCATIONS) {
		link_error(state, "output '%s' has no room for its locations", symbol->name);
		return -1;
	}

	/* A dual-source blend's second colour goes to the one draw buffer such a blend has. */
	if (symbol->explicit_index != 0U && symbol->location + locations > 1U) {
		link_error(state, "output '%s' has index 1 at a location other than 0", symbol->name);
		return -1;
	}

	/* Each location once for each index. */
	row = taken[0];
	if (symbol->explicit_index != 0U)
		row = taken[1];
	for (slot = 0U; slot < locations; slot++) {
		if (row[symbol->location + slot]) {
			link_error(state, "two fragment shader outputs share location %u", symbol->location + slot);
			return -1;
		}

		/* Taken now. */
		row[symbol->location + slot] = 1U;
	}

	/* Succeeded: taken. */
	return 0;
}

/*
 * Finds the output of the stage before that an input (of the type it
 * reads: a geometry shader's input array's element) reads: a block by
 * its block name, anything else by its name.
 */
static struct glsl_symbol *
link_find_varying(
	struct glsl_shader *producer,
	struct glsl_symbol *input,
	const struct glsl_type *type)
{
	struct glsl_symbol *symbol;
	const struct glsl_type *output_type;
	int differs;

	/* Anything but a block, by name. */
	if (type->kind != GLSL_KIND_STRUCT || type->block == 0U) {
		symbol = link_find(producer, input->name, GLSL_VAR_OUTPUT);
		return symbol;
	}

	/* A block: the output block of the same block name (a control shader's per vertex). */
	for (symbol = producer->globals; symbol != NULL; symbol = symbol->next_global) {
		if (symbol->where != GLSL_VAR_OUTPUT)
			continue;
		output_type = link_output_type(producer, symbol);
		if (output_type->kind != GLSL_KIND_STRUCT || output_type->block == 0U)
			continue;
		differs = strcmp(output_type->name, type->name);
		if (differs == 0)
			return symbol;
	}

	/* None. */
	return NULL;
}

/* Finds a global of a stage by name and kind (GLSL_VAR_*). */
static struct glsl_symbol *
link_find(
	struct glsl_shader *shader,
	const char *name,
	unsigned where)
{
	struct glsl_symbol *symbol;
	int differs;

	/* The globals in order. */
	for (symbol = shader->globals; symbol != NULL; symbol = symbol->next_global) {
		if (symbol->where != where)
			continue;
		differs = strcmp(symbol->name, name);
		if (differs == 0)
			return symbol;
	}

	/* None. */
	return NULL;
}

/* Lists the uniforms as the API reports them: the block's leaves by their reflected names, and the samplers. */
static void
link_info(
	struct link_state *state,
	struct glsl_program *program)
{
	struct glsl_uniform_info *infos;
	unsigned index;

	/* Room for every leaf (a struct array's leaves are many), the blocks' members included. */
	infos = calloc(LINK_MAX_LEAVES, sizeof(*infos));
	if (infos == NULL)
		longjmp(state->failure, 1);
	program->uniforms = infos;

	/* Each uniform's leaves, counted as they are named so that a failure frees every name. */
	for (index = 0U; index < state->uniform_count; index++)
		link_leaves(state, state->uniforms[index].type, state->uniforms[index].name, infos, &program->uniform_count, LINK_MAX_LEAVES);

	/* None of them is in a named block. */
	for (index = 0U; index < program->uniform_count; index++)
		infos[index].block = -1;
}

/*
 * Lists the named uniform blocks a stage reads, with their members: a
 * block the other stage listed already only gains the stage.  Each
 * element of an array of blocks the stage reads is a block of its own
 * ("B[i]"); the members are listed once a binding, with the binding in
 * place of their block's index until link_block_order sorts the blocks.
 */
static void
link_block_infos(
	struct link_state *state,
	struct glsl_shader *shader,
	unsigned stage,
	struct glsl_program *program)
{
	struct glsl_block_info *blocks;
	struct glsl_symbol *symbol;
	const struct glsl_type *type;
	uint32_t elements;
	unsigned element;
	unsigned count;
	unsigned first_member;
	unsigned listed;
	unsigned first;
	int differs;

	/* Room for every block, made at the first stage. */
	if (program->blocks == NULL) {
		blocks = calloc(LINK_MAX_BLOCKS, sizeof(*blocks));
		if (blocks == NULL)
			longjmp(state->failure, 1);
		program->blocks = blocks;
	}

	/* Each block the stage reads. */
	for (symbol = shader->globals; symbol != NULL; symbol = symbol->next_global) {
		if (!symbol->used || symbol->where != GLSL_VAR_BLOCK)
			continue;

		/* Its struct, and the elements it reads: the one of a block that is not an array. */
		type = symbol->type;
		count = 1U;
		elements = 1U;
		if (type->kind == GLSL_KIND_ARRAY) {
			count = type->length;
			elements = symbol->elements;
			type = type->element;
		}

		/* Whether a block of its binding is listed already (its members are then). */
		first = link_block_first(program, symbol->binding);
		listed = 0U;
		if (first < program->block_count)
			listed = 1U;

		/* Each element read. */
		for (element = 0U; element < count && element < 32U; element++) {
			if ((elements & ((uint32_t)1U << element)) == 0U)
				continue;
			link_block_element(state, symbol, type, element, count, stage, program);
		}

		/*
		 * The members once a binding (none when no element is read), named
		 * after the block when it has an instance name, else by their own.
		 */
		if (listed)
			continue;
		first = link_block_first(program, symbol->binding);
		if (first == program->block_count)
			continue;
		first_member = program->uniform_count;
		differs = strcmp(symbol->name, type->name);
		if (differs != 0) {
			link_member_leaves(state, type, type->name, 0U, 0U, (int)symbol->binding, program);
		} else {
			link_member_leaves(state, type, NULL, 0U, 0U, (int)symbol->binding, program);
		}

		/* How many members it has, on the first block of the binding (link_block_order gives them to the others). */
		program->blocks[first].member_count = program->uniform_count - first_member;
	}
}

/* Returns the index of the first block listed of a binding, or the program's block count when there is none. */
static unsigned
link_block_first(
	const struct glsl_program *program,
	unsigned binding)
{
	unsigned index;

	/* The blocks listed so far. */
	for (index = 0U; index < program->block_count; index++) {
		if (program->blocks[index].binding == binding)
			return index;
	}

	/* None of the binding. */
	return program->block_count;
}

/*
 * Lists one element of a block a stage reads (the element 0 of a block
 * that is not an array of blocks): a new block, or the stage added to
 * the one another stage listed.
 */
static void
link_block_element(
	struct link_state *state,
	const struct glsl_symbol *symbol,
	const struct glsl_type *type,
	unsigned element,
	unsigned count,
	unsigned stage,
	struct glsl_program *program)
{
	struct glsl_block_info *block;
	char name[256];
	unsigned index;

	/* A block listed already (the vertex stage's) gains the stage. */
	for (index = 0U; index < program->block_count; index++) {
		block = &program->blocks[index];
		if (block->binding == symbol->binding && block->element == element) {
			block->stages |= 1U << stage;
			return;
		}
	}

	/* No more blocks than libGLESv2 binds. */
	if (program->block_count == LINK_MAX_BLOCKS) {
		link_error(state, "more than %u uniform blocks", LINK_MAX_BLOCKS);
		longjmp(state->failure, 1);
	}

	/* A new block: its name ("B[i]" for an element of an array), binding, element, size and stage. */
	block = &program->blocks[program->block_count];
	if (symbol->type->kind == GLSL_KIND_ARRAY) {
		(void)snprintf(name, sizeof(name), "%s[%u]", type->name, element);
	} else {
		(void)snprintf(name, sizeof(name), "%s", type->name);
	}
	block->name = link_name(state, name);
	program->block_count++;
	block->binding = symbol->binding;
	block->element = element;
	block->count = count;
	block->size = glsl_std140_size(type);
	block->stages = 1U << stage;
}

/*
 * Orders the named uniform blocks by their bindings and elements (an
 * array's elements one after another), gives each block of a binding the
 * members the first listed (their index among the blocks), and turns the
 * members' bindings into that index.
 */
static void
link_block_order(
	struct glsl_program *program)
{
	struct glsl_block_info moved;
	unsigned index;
	unsigned other;
	unsigned first;
	unsigned members;
	int before;

	/* The blocks in order of binding and element (an insertion sort of at most LINK_MAX_BLOCKS). */
	for (index = 1U; index < program->block_count; index++) {
		moved = program->blocks[index];

		/* The blocks before it that sort after it move up one. */
		other = index;
		while (other > 0U) {
			before = link_block_before(&moved, &program->blocks[other - 1U]);
			if (!before)
				break;
			program->blocks[other] = program->blocks[other - 1U];
			other--;
		}

		/* It takes the place they left. */
		program->blocks[other] = moved;
	}

	/* Each block's members: the first block of its binding's, and how many the binding has. */
	first = 0U;
	for (index = 0U; index < program->block_count; index++) {
		if (index == 0U || program->blocks[index - 1U].binding != program->blocks[index].binding)
			first = index;
		program->blocks[index].members = first;
	}

	/* The binding's member count on each of its blocks (the first listed has it). */
	for (index = 0U; index < program->block_count; index++) {
		members = program->blocks[index].member_count;
		if (members != 0U) {
			for (other = 0U; other < program->block_count; other++) {
				if (program->blocks[other].members == program->blocks[index].members)
					program->blocks[other].member_count = members;
			}
		}
	}

	/* Each member's binding becomes the index of its binding's first block. */
	for (index = 0U; index < program->uniform_count; index++) {
		if (program->uniforms[index].block < 0)
			continue;
		for (other = 0U; other < program->block_count; other++) {
			if ((int)program->blocks[other].binding == program->uniforms[index].block) {
				program->uniforms[index].block = (int)program->blocks[other].members;
				break;
			}
		}
	}
}

/* Reports whether a block sorts before another: a lower binding, or a lower element of the same binding. */
static int
link_block_before(
	const struct glsl_block_info *block,
	const struct glsl_block_info *other)
{
	/* A lower binding. */
	if (block->binding < other->binding)
		return 1;

	/* A lower element of the same binding. */
	if (block->binding == other->binding && block->element < other->element)
		return 1;

	/* Anything else sorts after. */
	return 0;
}

/*
 * Adds the leaves of a named block's member of a type at a std140 offset
 * under a name (NULL: the block itself without an instance name, whose
 * members take their own names): struct members "name.member", struct
 * array elements "name[i]", and a leaf with its offset and strides.
 */
static void
link_member_leaves(
	struct link_state *state,
	const struct glsl_type *type,
	const char *name,
	unsigned offset,
	unsigned row_major,
	int block,
	struct glsl_program *program)
{
	struct glsl_uniform_info *info;
	const struct glsl_type *leaf;
	char member[256];
	unsigned alignment;
	unsigned stride;
	unsigned index;

	/* A struct: each member at its offset, with its own matrix order. */
	if (type->kind == GLSL_KIND_STRUCT) {
		for (index = 0U; index < type->field_count; index++) {
			alignment = glsl_std140_member_alignment(type->fields[index].type, type->fields[index].row_major);
			offset = (offset + alignment - 1U) & ~(alignment - 1U);
			if (name != NULL) {
				(void)snprintf(member, sizeof(member), "%s.%s", name, type->fields[index].name);
			} else {
				(void)snprintf(member, sizeof(member), "%s", type->fields[index].name);
			}

			/* The member, then the next member's offset. */
			link_member_leaves(state, type->fields[index].type, member, offset, type->fields[index].row_major, block, program);
			offset += glsl_std140_member_size(type->fields[index].type, type->fields[index].row_major);
		}

		/* The members are in. */
		return;
	}

	/* An array of structs: each element at its stride. */
	if (type->kind == GLSL_KIND_ARRAY && type->element->kind == GLSL_KIND_STRUCT) {
		stride = glsl_std140_stride(type);
		for (index = 0U; index < type->length; index++) {
			(void)snprintf(member, sizeof(member), "%s[%u]", name, index);
			link_member_leaves(state, type->element, member, offset + index * stride, row_major, block, program);
		}

		/* The elements are in. */
		return;
	}

	/* A leaf (an array of leaves is one uniform of its length); without room it is left out. */
	if (program->uniform_count == LINK_MAX_LEAVES)
		return;
	info = &program->uniforms[program->uniform_count];
	info->name = link_name(state, name);
	program->uniform_count++;

	/* Its size, and the stride of an array's elements. */
	leaf = type;
	info->size = 1U;
	if (type->kind == GLSL_KIND_ARRAY) {
		leaf = type->element;
		info->size = type->length;
		info->array_stride = glsl_std140_member_stride(type, row_major);
	}

	/* Its kind, as a leaf of the default block has it. */
	info->components = leaf->components;
	info->columns = leaf->columns;
	info->base = GLSL_INFO_FLOAT;
	if (leaf->base == GLSL_BASE_INT)
		info->base = GLSL_INFO_INT;
	if (leaf->base == GLSL_BASE_UINT)
		info->base = GLSL_INFO_UINT;
	if (leaf->base == GLSL_BASE_BOOL)
		info->base = GLSL_INFO_BOOL;
	if (leaf->base == GLSL_BASE_DOUBLE)
		info->base = GLSL_INFO_DOUBLE;

	/* Where it is in the block: its offset, and a matrix's columns (or rows) 16 bytes apart. */
	info->block = block;
	info->offset = offset;
	if (leaf->kind == GLSL_KIND_MATRIX) {
		info->matrix_stride = glsl_std140_matrix_stride(leaf, row_major);
		info->row_major = row_major;
	}
}

/*
 * Finds the capturing shader's outputs of the names a program captures
 * (transform feedback), in order, and lays out a vertex's record of
 * them; a vertex shader's vertex and instance numbers, which place the
 * records, are used too, and a geometry shader's primitive and
 * invocation numbers, which order them.  Errors go to the log.
 */
static void
link_captures(
	struct link_state *state,
	struct glsl_shader *shader,
	const char *const *captures,
	unsigned capture_count)
{
	struct glsl_symbol *symbol;
	struct glsl_symbol *vertex_id;
	struct glsl_symbol *instance_id;
	unsigned index;
	unsigned other;
	unsigned words;

	/* Nothing captured. */
	state->capture_count = 0U;
	state->capture_stride = 0U;
	if (capture_count == 0U)
		return;
	if (capture_count > LINK_MAX_CAPTURES) {
		link_error(state, "too many transform feedback varyings");
		return;
	}

	/* A geometry shader's primitive's and invocation's numbers, emitted: its records are ordered by them (it has the first). */
	if (shader->stage == GLSL_STAGE_GEOMETRY) {
		symbol = link_find(shader, "gl_PrimitiveIDIn", GLSL_VAR_INPUT);
		if (symbol != NULL)
			symbol->used = 1;
		symbol = link_find(shader, "gl_InvocationID", GLSL_VAR_INPUT);
		if (symbol != NULL)
			symbol->used = 1;
	}

	/* The vertex's and the instance's numbers (GLSL ES 3.00 and GLSL 1.30 on have them). */
	if (shader->stage == GLSL_STAGE_VERTEX) {
		vertex_id = link_find(shader, "gl_VertexID", GLSL_VAR_INPUT);
		instance_id = link_find(shader, "gl_InstanceID", GLSL_VAR_INPUT);
		if (vertex_id == NULL || instance_id == NULL) {
			link_error(state, "transform feedback needs a vertex shader of GLSL ES 3.00 or GLSL 1.30 on");
			return;
		}

		/* Emitted: the records are placed by them. */
		vertex_id->used = 1;
		instance_id->used = 1;
	}

	/* Each name: an output of the capturing shader of numbers, named once. */
	for (index = 0U; index < capture_count; index++) {
		symbol = link_find(shader, captures[index], GLSL_VAR_OUTPUT);
		if (symbol == NULL) {
			link_error(state, "transform feedback varying '%s' is not an output of the last shader before the fragment shader",
				   captures[index]);
			continue;
		}

		/* The words it takes. */
		words = link_capture_words(symbol->type);
		if (words == 0U) {
			link_error(state, "transform feedback varying '%s' cannot be captured", captures[index]);
			continue;
		}

		/* Named once. */
		for (other = 0U; other < state->capture_count; other++) {
			if (state->captures[other].symbol == symbol)
				link_error(state, "transform feedback varying '%s' is named twice", captures[index]);
		}

		/* Its place in the record; it is emitted even when the shader does not write it. */
		symbol->used = 1;
		state->captures[state->capture_count].symbol = symbol;
		state->captures[state->capture_count].offset = state->capture_stride;
		state->capture_count++;
		state->capture_stride += words;
	}
}

/* Returns the words a captured output of a type takes (a scalar, vector or matrix of numbers, or an array of one), 0 for a type that cannot be captured. */
static unsigned
link_capture_words(
	const struct glsl_type *type)
{
	unsigned words;

	/* An array: its elements' words. */
	if (type->kind == GLSL_KIND_ARRAY) {
		if (type->element->kind == GLSL_KIND_ARRAY)
			return 0U;
		words = link_capture_words(type->element);
		return words * type->length;
	}

	/* Numbers only: floats, ints and unsigned ints. */
	if (type->kind != GLSL_KIND_SCALAR && type->kind != GLSL_KIND_VECTOR && type->kind != GLSL_KIND_MATRIX)
		return 0U;
	if (type->base != GLSL_BASE_FLOAT && type->base != GLSL_BASE_INT && type->base != GLSL_BASE_UINT)
		return 0U;

	/* Succeeded: a word per component. */
	return type->components * type->columns;
}

/* Lists the captured outputs as the API reports them, with their places in a vertex's record. */
static void
link_capture_infos(
	struct link_state *state,
	struct glsl_program *program)
{
	struct glsl_capture_info *info;
	const struct glsl_type *type;
	unsigned index;

	/* Nothing captured. */
	if (state->capture_count == 0U)
		return;

	/* The list. */
	program->captures = calloc(state->capture_count, sizeof(*program->captures));
	if (program->captures == NULL)
		longjmp(state->failure, 1);

	/* Each output: its name, its type (an array's element's, with the length), and its place. */
	for (index = 0U; index < state->capture_count; index++) {
		info = &program->captures[index];
		type = state->captures[index].symbol->type;
		info->size = 1U;
		if (type->kind == GLSL_KIND_ARRAY) {
			info->size = type->length;
			type = type->element;
		}

		/* Its name. */
		info->name = link_name(state, state->captures[index].symbol->name);
		program->capture_count = index + 1U;
		info->base = GLSL_INFO_FLOAT;
		if (type->base == GLSL_BASE_INT)
			info->base = GLSL_INFO_INT;
		if (type->base == GLSL_BASE_UINT)
			info->base = GLSL_INFO_UINT;
		info->components = type->components;
		info->columns = type->columns;
		info->offset = state->captures[index].offset;
		info->words = type->components * type->columns * info->size;
		info->stream = state->captures[index].symbol->stream;
	}

	/* The record's words. */
	program->capture_stride = state->capture_stride;
}

/*
 * Notes the stage that captures, and a geometry shader's output primitive
 * as transform feedback names its kind, its most vertices and its
 * invocations.
 */
static void
link_capture_stage(
	const struct glsl_shader *shader,
	struct glsl_program *program)
{
	/* A vertex shader's records are placed by its numbers. */
	program->capture_stage = shader->stage;
	if (shader->stage != GLSL_STAGE_GEOMETRY)
		return;

	/* A point's, a strip of lines' or a strip of triangles' kind. */
	program->capture_primitive = 0x0000U;
	if (shader->geometry_output == GLSL_PRIMITIVE_LINE_STRIP)
		program->capture_primitive = 0x0001U;
	if (shader->geometry_output == GLSL_PRIMITIVE_TRIANGLE_STRIP)
		program->capture_primitive = 0x0004U;

	/* The most vertices of an invocation, and the invocations of a primitive (1 without the layout). */
	program->capture_vertices = shader->max_vertices;
	program->capture_invocations = shader->geometry_invocations;
	if (program->capture_invocations == 0U)
		program->capture_invocations = 1U;
}

/* Copies a name into memory of its own, which glsl_program_free frees. */
static char *
link_name(
	struct link_state *state,
	const char *name)
{
	char *copy;
	size_t length;

	/* The copy with its terminator. */
	length = strlen(name);
	copy = malloc(length + 1U);
	if (copy == NULL)
		longjmp(state->failure, 1);
	memcpy(copy, name, length + 1U);

	/* Succeeded: the copy. */
	return copy;
}

/* Adds the leaves of a uniform of a type under a name: struct members "name.member", struct array elements "name[i]". */
static void
link_leaves(
	struct link_state *state,
	const struct glsl_type *type,
	const char *name,
	struct glsl_uniform_info *out,
	unsigned *count,
	unsigned capacity)
{
	struct glsl_uniform_info *info;
	const struct glsl_type *leaf;
	char member[256];
	unsigned index;
	size_t length;

	/* A struct: each member. */
	if (type->kind == GLSL_KIND_STRUCT) {
		for (index = 0U; index < type->field_count; index++) {
			(void)snprintf(member, sizeof(member), "%s.%s", name, type->fields[index].name);
			link_leaves(state, type->fields[index].type, member, out, count, capacity);
		}

		/* The members are in. */
		return;
	}

	/* An array of structs: each element. */
	if (type->kind == GLSL_KIND_ARRAY && type->element->kind == GLSL_KIND_STRUCT) {
		for (index = 0U; index < type->length; index++) {
			(void)snprintf(member, sizeof(member), "%s[%u]", name, index);
			link_leaves(state, type->element, member, out, count, capacity);
		}

		/* The elements are in. */
		return;
	}

	/* A leaf (an array of leaves is one uniform of its length). */
	if (*count == capacity)
		return;
	info = &out[*count];
	leaf = type;
	info->size = 1U;
	if (type->kind == GLSL_KIND_ARRAY) {
		leaf = type->element;
		info->size = type->length;
	}

	/* Its name. */
	length = strlen(name);
	info->name = malloc(length + 1U);
	if (info->name == NULL)
		longjmp(state->failure, 1);
	memcpy(info->name, name, length + 1U);
	(*count)++;

	/* Its kind. */
	info->components = leaf->components;
	info->columns = leaf->columns;
	info->sampler = leaf->sampler;
	info->shadow = leaf->shadow;
	info->arrayed = leaf->arrayed;
	switch (leaf->base) {
	case GLSL_BASE_INT:
		info->base = GLSL_INFO_INT;
		break;
	case GLSL_BASE_UINT:
		info->base = GLSL_INFO_UINT;
		break;
	case GLSL_BASE_BOOL:
		info->base = GLSL_INFO_BOOL;
		break;
	case GLSL_BASE_DOUBLE:
		info->base = GLSL_INFO_DOUBLE;
		break;
	default:
		info->base = GLSL_INFO_FLOAT;
		break;
	}
}

/* Copies words out of the arena into memory of their own (NULL when there is none). */
static uint32_t *
link_copy(
	const uint32_t *code,
	size_t words)
{
	uint32_t *copy;

	/* The copy. */
	copy = malloc(words * sizeof(uint32_t));
	if (copy == NULL)
		return NULL;
	memcpy(copy, code, words * sizeof(uint32_t));

	/* Succeeded: the copy. */
	return copy;
}

/*
 * Links a compute shader alone into SPIR-V (ws101-p008): its default
 * uniform block and uniform blocks are laid out as a draw program's, its
 * storage blocks take their descriptor bindings from their layouts, and
 * the one stage is emitted.
 */
int
glsl_link_compute(
	const struct glsl_shader *compute_shader,
	struct glsl_program *program,
	char **log)
{
	struct link_state *volatile state;
	struct glsl_shader *compute;
	struct glsl_shader *stages[1];
	uint32_t *code;
	size_t words;

	/* Nothing yet. */
	*log = NULL;
	memset(program, 0, sizeof(*program));
	state = calloc(1U, sizeof(*state));
	if (state == NULL)
		return -1;
	state->arena.failure = &state->failure;
	compute = (struct glsl_shader *)compute_shader;

	/*
	 * Out of memory comes back here.  setjmp is the whole controlling
	 * expression, the form C allows it in.
	 */
	switch (setjmp(state->failure)) {
	case 0:
		break;
	default:
		/* The link failed: what it made goes, and the log (if any) tells why. */
		glsl_program_free(program);
		glsl_arena_free(&state->arena);
		*log = state->log.data;
		free(state);
		return -1;
	}

	/* A compute shader alone. */
	if (compute->stage != GLSL_STAGE_COMPUTE)
		link_error(state, "a compute program needs a compute shader and no other");
	if (state->errors != 0U)
		longjmp(state->failure, 1);

	/* The uniforms and uniform blocks, laid out; the storage blocks, bound. */
	stages[0] = compute;
	link_uniforms(state, compute);
	link_layout(state);
	link_blocks(state, stages, 1U);
	link_storages(state, compute, program);
	if (state->errors != 0U)
		longjmp(state->failure, 1);

	/* The stage's SPIR-V, copied out of the arena, and its workgroup size. */
	code = glsl_emit(compute, &state->arena, state->uniforms, state->uniform_count, NULL, 0U, 0U, &words);
	program->code[GLSL_STAGE_COMPUTE] = link_copy(code, words);
	program->words[GLSL_STAGE_COMPUTE] = words;
	if (program->code[GLSL_STAGE_COMPUTE] == NULL)
		longjmp(state->failure, 1);
	program->local_size[0] = compute->local_size[0];
	program->local_size[1] = compute->local_size[1];
	program->local_size[2] = compute->local_size[2];

	/* The image uniforms (the emitter marked the ones atomics use). */
	link_images(state, compute, program);
	if (state->errors != 0U)
		longjmp(state->failure, 1);

	/* What the API reports of the uniforms and the uniform blocks. */
	link_info(state, program);
	link_block_infos(state, compute, GLSL_STAGE_COMPUTE, program);
	link_block_order(program);

	/* Succeeded: the program. */
	glsl_arena_free(&state->arena);
	free(state->log.data);
	free(state);
	return 0;
}

/*
 * Binds a compute shader's storage blocks (ws101-p008): each at set 0,
 * GLSL_STORAGE_FIRST_BINDING plus its layout's binding, one block a
 * binding; lists them for the API: name, binding, the bytes before a
 * run-time array, its stride, and whether it is readonly.
 */
static void
link_storages(
	struct link_state *state,
	struct glsl_shader *compute,
	struct glsl_program *program)
{
	struct glsl_storage_info *infos;
	struct glsl_storage_info *info;
	struct glsl_buffer_info *buffers;
	struct glsl_symbol *symbol;
	const struct glsl_type *type;
	const struct glsl_type *last;
	char member[256];
	unsigned taken;
	unsigned offset;
	unsigned alignment;
	unsigned index;
	int differs;

	/* Room for every binding, and for the buffer variables. */
	infos = calloc(GLSL_STORAGE_BINDINGS, sizeof(*infos));
	if (infos == NULL)
		longjmp(state->failure, 1);
	program->storages = infos;
	buffers = calloc(LINK_MAX_LEAVES, sizeof(*buffers));
	if (buffers == NULL)
		longjmp(state->failure, 1);
	program->buffers = buffers;

	/* Each block the code uses. */
	taken = 0U;
	for (symbol = compute->globals; symbol != NULL; symbol = symbol->next_global) {
		if (!symbol->used || symbol->where != GLSL_VAR_BUFFER)
			continue;

		/* One block a binding. */
		if ((taken & (1U << symbol->layout_binding)) != 0U) {
			link_error(state, "two buffer blocks at binding %u", symbol->layout_binding);
			continue;
		}
		taken |= 1U << symbol->layout_binding;
		symbol->binding = GLSL_STORAGE_FIRST_BINDING + symbol->layout_binding;

		/* What the API reports of it. */
		type = symbol->type;
		info = &infos[program->storage_count];
		info->name = link_name(state, type->name);
		info->binding = symbol->layout_binding;
		info->size = glsl_std430_size(type);
		info->array_stride = 0U;
		if (type->field_count != 0U) {
			last = type->fields[type->field_count - 1U].type;
			if (last->kind == GLSL_KIND_ARRAY && last->length == 0U)
				info->array_stride = glsl_std430_stride(last);
		}
		info->readonly = ((symbol->memory & GLSL_MEMORY_READONLY) != 0U);
		program->storage_count++;

		/* Its buffer variables, named after the block when it has an instance name, else by their own. */
		differs = strcmp(symbol->name, type->name);
		offset = 0U;
		for (index = 0U; index < type->field_count; index++) {
			alignment = glsl_std430_alignment(type->fields[index].type);
			offset = (offset + alignment - 1U) & ~(alignment - 1U);
			if (differs != 0) {
				(void)snprintf(member, sizeof(member), "%.200s.%.50s", type->name, type->fields[index].name);
			} else {
				(void)snprintf(member, sizeof(member), "%.250s", type->fields[index].name);
			}

			/* The member, then the next member's offset. */
			link_buffer_top(state, type->fields[index].type, member, offset, program->storage_count - 1U, program);
			offset += glsl_std430_size(type->fields[index].type);
		}
	}
}

/*
 * Lists the buffer variables of a shader storage block's top-level
 * member at an offset: an array of an aggregate type (a struct or an
 * array) only by its first element, with the array's elements and
 * stride; anything else as it is.
 */
static void
link_buffer_top(
	struct link_state *state,
	const struct glsl_type *type,
	const char *name,
	unsigned offset,
	unsigned block,
	struct glsl_program *program)
{
	char element[256];

	/* An array of an aggregate type: its first element. */
	if (type->kind == GLSL_KIND_ARRAY &&
	    (type->element->kind == GLSL_KIND_STRUCT ||
	     type->element->kind == GLSL_KIND_ARRAY)) {
		(void)snprintf(element, sizeof(element), "%.240s[0]", name);
		link_buffer_leaves(state, type->element, element, offset, block, type->length, glsl_std430_stride(type), program);
		return;
	}

	/* Anything else. */
	link_buffer_leaves(state, type, name, offset, block, 1U, 0U, program);
}

/*
 * Lists the buffer variables of a part of a shader storage block's member
 * at an offset (std430): a struct's members and an array of an aggregate
 * type's elements each on their own, a leaf (an array of a basic type is
 * one) as one variable; without room a variable is left out.
 */
static void
link_buffer_leaves(
	struct link_state *state,
	const struct glsl_type *type,
	const char *name,
	unsigned offset,
	unsigned block,
	unsigned top_size,
	unsigned top_stride,
	struct glsl_program *program)
{
	struct glsl_buffer_info *info;
	const struct glsl_type *leaf;
	char member[256];
	unsigned alignment;
	unsigned stride;
	unsigned index;

	/* A struct: each member at its offset. */
	if (type->kind == GLSL_KIND_STRUCT) {
		for (index = 0U; index < type->field_count; index++) {
			alignment = glsl_std430_alignment(type->fields[index].type);
			offset = (offset + alignment - 1U) & ~(alignment - 1U);
			(void)snprintf(member, sizeof(member), "%.200s.%.50s", name, type->fields[index].name);
			link_buffer_leaves(state, type->fields[index].type, member, offset, block, top_size, top_stride, program);
			offset += glsl_std430_size(type->fields[index].type);
		}

		/* The members are in. */
		return;
	}

	/* An array of an aggregate type: each element at its stride. */
	if (type->kind == GLSL_KIND_ARRAY &&
	    (type->element->kind == GLSL_KIND_STRUCT ||
	     type->element->kind == GLSL_KIND_ARRAY)) {
		stride = glsl_std430_stride(type);
		for (index = 0U; index < type->length; index++) {
			(void)snprintf(member, sizeof(member), "%.240s[%u]", name, index);
			link_buffer_leaves(state, type->element, member, offset + index * stride, block, top_size, top_stride, program);
		}

		/* The elements are in. */
		return;
	}

	/* A leaf, with room for it. */
	if (program->buffer_count == LINK_MAX_LEAVES)
		return;
	info = &program->buffers[program->buffer_count];
	program->buffer_count++;

	/* Its name ("[0]" for an array of a basic type), elements and stride. */
	leaf = type;
	info->size = 1U;
	(void)snprintf(member, sizeof(member), "%.250s", name);
	if (type->kind == GLSL_KIND_ARRAY) {
		leaf = type->element;
		info->size = type->length;
		info->array_stride = glsl_std430_stride(type);
		(void)snprintf(member, sizeof(member), "%.240s[0]", name);
	}
	info->name = link_name(state, member);

	/* Its kind, as a uniform's. */
	info->components = leaf->components;
	info->columns = leaf->columns;
	info->base = GLSL_INFO_FLOAT;
	if (leaf->base == GLSL_BASE_INT)
		info->base = GLSL_INFO_INT;
	if (leaf->base == GLSL_BASE_UINT)
		info->base = GLSL_INFO_UINT;
	if (leaf->base == GLSL_BASE_BOOL)
		info->base = GLSL_INFO_BOOL;
	if (leaf->base == GLSL_BASE_DOUBLE)
		info->base = GLSL_INFO_DOUBLE;

	/* Where it is: its offset, a matrix's column stride, the block and the top-level array. */
	info->offset = offset;
	if (leaf->kind == GLSL_KIND_MATRIX)
		info->matrix_stride = glsl_std430_column_stride(leaf);
	info->block = block;
	info->top_size = top_size;
	info->top_stride = top_stride;
}

/*
 * Lists a compute shader's image uniforms the code uses, one image an
 * image unit (each uniform at set 0, GLSL_IMAGE_FIRST_BINDING plus its
 * first unit; an array's elements on the next units): name, unit, count,
 * dimension, layers, texel kind, format, memory qualifiers, and whether
 * an atomic function uses it.
 */
static void
link_images(
	struct link_state *state,
	struct glsl_shader *compute,
	struct glsl_program *program)
{
	struct glsl_image_info *infos;
	struct glsl_image_info *info;
	struct glsl_symbol *symbol;
	const struct glsl_type *type;
	unsigned taken;
	unsigned units;
	unsigned count;

	/* Room for every unit. */
	infos = calloc(GLSL_IMAGE_UNITS, sizeof(*infos));
	if (infos == NULL)
		longjmp(state->failure, 1);
	program->images = infos;

	/* Each image the code uses. */
	taken = 0U;
	for (symbol = compute->globals; symbol != NULL; symbol = symbol->next_global) {
		if (!symbol->used || symbol->where != GLSL_VAR_IMAGE || symbol->layout_binding >= GLSL_IMAGE_UNITS)
			continue;

		/* Its images: one, or an array's elements (the check kept them below GLSL_IMAGE_UNITS). */
		type = symbol->type;
		count = 1U;
		if (type->kind == GLSL_KIND_ARRAY) {
			count = type->length;
			type = type->element;
		}
		if (count == 0U || count > GLSL_IMAGE_UNITS - symbol->layout_binding)
			continue;

		/* One image a unit. */
		units = ((1U << count) - 1U) << symbol->layout_binding;
		if ((taken & units) != 0U) {
			link_error(state, "two images at unit %u", symbol->layout_binding);
			continue;
		}
		taken |= units;
		symbol->binding = GLSL_IMAGE_FIRST_BINDING + symbol->layout_binding;

		/* What the API reports of it. */
		info = &infos[program->image_count];
		info->name = link_name(state, symbol->name);
		info->unit = symbol->layout_binding;
		info->count = count;
		info->dimension = type->sampler;
		info->arrayed = type->arrayed;
		info->base = GLSL_INFO_FLOAT;
		if (type->base == GLSL_BASE_INT)
			info->base = GLSL_INFO_INT;
		if (type->base == GLSL_BASE_UINT)
			info->base = GLSL_INFO_UINT;
		info->format = type->image_format;
		info->readonly = ((symbol->memory & GLSL_MEMORY_READONLY) != 0U);
		info->writeonly = ((symbol->memory & GLSL_MEMORY_WRITEONLY) != 0U);
		info->atomic = symbol->image_atomic;
		program->image_count++;
	}
}
