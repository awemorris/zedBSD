/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GLSL compiler's entry for compiling one shader: preprocess, parse
 * and check, with the info log, and the shader kept for the link.
 */

#include "internal.h"

#include <stdlib.h>
#include <string.h>

/*
 * GL's names of the geometry shader's primitives by GLSL_PRIMITIVE_*
 * (GL_POINTS, GL_LINES, GL_LINES_ADJACENCY, GL_TRIANGLES,
 * GL_TRIANGLES_ADJACENCY, GL_LINE_STRIP, GL_TRIANGLE_STRIP; the compiler
 * does not include GL's headers).
 */
static const unsigned glsl_gl_primitives[8] = { 0U, 0x0000U, 0x0001U, 0x000AU, 0x0004U, 0x000CU, 0x0003U, 0x0005U };

static void glsl_give_log(struct glsl_shader *shader, char **log);
static void glsl_stand_in_structs(struct glsl_text *text, jmp_buf *failure, const struct glsl_type *type, const struct glsl_type **declared, unsigned *count);
static void glsl_stand_in_variable(struct glsl_text *text, jmp_buf *failure, const struct glsl_type *type, const char *name);

/* The most struct types a stand-in shader declares. */
#define GLSL_STAND_IN_STRUCTS	32U

/*
 * Compiles a shader of a stage for a device of every optional feature.
 * Returns the shader, or NULL with the reasons in *log.
 */
struct glsl_shader *
glsl_compile(
	unsigned stage,
	const char *source,
	unsigned default_version,
	char **log)
{
	struct glsl_shader *shader;

	/* The compile with all the features. */
	shader = glsl_compile_features(stage, source, default_version, GLSL_FEATURES_ALL, log);

	/* Succeeded: the shader, or NULL with the log. */
	return shader;
}

/*
 * Compiles a shader of a stage for a device of the optional features
 * given (GLSL_FEATURE_*).  Returns the shader, or NULL with the reasons in
 * *log.
 */
struct glsl_shader *
glsl_compile_features(
	unsigned stage,
	const char *source,
	unsigned default_version,
	unsigned features,
	char **log)
{
	struct glsl_shader *volatile shader;

	/* No log until there is something to say. */
	*log = NULL;

	/* The shader, whose arena jumps back here when memory runs out. */
	shader = calloc(1U, sizeof(*shader));
	if (shader == NULL)
		return NULL;
	shader->arena.failure = &shader->failure;
	shader->stage = stage;
	shader->features = features & GLSL_FEATURES_ALL;

	/*
	 * Out of memory and fatal errors come back here.  setjmp is the whole
	 * controlling expression, the one form C allows it in besides a few
	 * comparisons.
	 */
	switch (setjmp(shader->failure)) {
	case 0:
		break;
	default:
		/* The compile ended early: an error was fatal, or an allocation failed (no error counted, no log). */
		glsl_give_log(shader, log);
		glsl_shader_free(shader);
		return NULL;
	}

	/* The three passes, each only after the one before succeeded. */
	glsl_preprocess(shader, source, default_version);
	if (shader->errors == 0U)
		glsl_parse(shader);
	if (shader->errors == 0U)
		glsl_check(shader);

	/* A shader with errors is not kept. */
	glsl_give_log(shader, log);
	if (shader->errors != 0U) {
		glsl_shader_free(shader);
		return NULL;
	}

	/* Succeeded: the checked shader. */
	return shader;
}

/*
 * Frees a compiled shader and everything its compile made.
 */
void
glsl_shader_free(
	struct glsl_shader *shader)
{
	/* Nothing to free. */
	if (shader == NULL)
		return;

	/* The arena, the log and the shader. */
	glsl_arena_free(&shader->arena);
	free(shader->log.data);
	free(shader);
}

/*
 * Reports a compiled shader's stage.
 */
unsigned
glsl_shader_stage(
	const struct glsl_shader *shader)
{
	/* The stage it was compiled for. */
	return shader->stage;
}

/*
 * Reports a compiled shader's GLSL version and whether it is OpenGL ES's
 * language.
 */
unsigned
glsl_shader_version(
	const struct glsl_shader *shader,
	int *es)
{
	/* The version #version gave (or the default). */
	*es = (int)shader->es;
	return shader->version;
}

/*
 * Reports a compiled geometry shader's input and output primitives (GL's
 * names) and the most vertices it emits; all 0 for another stage.
 */
void
glsl_geometry_layout(
	const struct glsl_shader *shader,
	unsigned *input,
	unsigned *output,
	unsigned *max_vertices)
{
	/* Nothing for another stage. */
	*input = 0U;
	*output = 0U;
	*max_vertices = 0U;
	if (shader->stage != GLSL_STAGE_GEOMETRY)
		return;

	/* Succeeded: the layouts it declared. */
	*input = glsl_gl_primitives[shader->geometry_input];
	*output = glsl_gl_primitives[shader->geometry_output];
	*max_vertices = shader->max_vertices;
}

/*
 * Reports how many subroutine functions a compiled shader has (GLSL
 * 4.00); their indexes are 0 up, in declaration order.
 */
unsigned
glsl_subroutine_count(
	const struct glsl_shader *shader)
{
	/* Succeeded: the count. */
	return shader->subroutine_count;
}

/*
 * Returns the name of a compiled shader's subroutine function of an
 * index, NULL for an index it does not have.
 */
const char *
glsl_subroutine_name(
	const struct glsl_shader *shader,
	unsigned index)
{
	const struct glsl_function *subroutine;

	/* The subroutine of the index. */
	for (subroutine = shader->subroutines; subroutine != NULL; subroutine = subroutine->next_subroutine) {
		if (subroutine->subroutine_index == index)
			return subroutine->name;
	}

	/* No such subroutine. */
	return NULL;
}

/*
 * Reports how many subroutine uniforms a compiled shader declares (GLSL
 * 4.00).
 */
unsigned
glsl_subroutine_uniform_count(
	const struct glsl_shader *shader)
{
	/* Succeeded: the count. */
	return shader->subroutine_uniform_count;
}

/*
 * Returns the name of a compiled shader's subroutine uniform of an index
 * (in declaration order), with the name of its hidden uniform and its
 * size (1 unless an array); NULL for an index it does not have.
 */
const char *
glsl_subroutine_uniform(
	const struct glsl_shader *shader,
	unsigned index,
	const char **hidden,
	unsigned *size)
{
	const struct glsl_symbol *uniform;
	unsigned at;

	/* The uniform of the index. */
	at = 0U;
	for (uniform = shader->subroutine_uniforms; uniform != NULL && at < index; uniform = uniform->next_subroutine)
		at++;
	if (uniform == NULL)
		return NULL;

	/* Its hidden uniform and size. */
	*hidden = uniform->hidden->name;
	*size = 1U;
	if (uniform->type->kind == GLSL_KIND_ARRAY)
		*size = uniform->type->length;

	/* Succeeded: its name. */
	return uniform->name;
}

/*
 * Gives the indexes of the subroutines compatible with a compiled
 * shader's subroutine uniform (those one of whose types is the
 * uniform's), in index order, up to a capacity; returns how many there
 * are (0 for an index the shader does not have).
 */
unsigned
glsl_subroutine_compatible(
	const struct glsl_shader *shader,
	unsigned uniform,
	unsigned *indexes,
	unsigned capacity)
{
	const struct glsl_symbol *symbol;
	const struct glsl_function *subroutine;
	unsigned count;
	unsigned at;
	unsigned kind;

	/* The uniform of the index. */
	at = 0U;
	for (symbol = shader->subroutine_uniforms; symbol != NULL && at < uniform; symbol = symbol->next_subroutine)
		at++;
	if (symbol == NULL)
		return 0U;

	/* Each subroutine one of whose types is the uniform's. */
	count = 0U;
	for (subroutine = shader->subroutines; subroutine != NULL; subroutine = subroutine->next_subroutine) {
		for (kind = 0U; kind < subroutine->subroutine_type_count; kind++) {
			if (subroutine->subroutine_types[kind] != symbol->functions)
				continue;
			if (count < capacity)
				indexes[count] = subroutine->subroutine_index;
			count++;
			break;
		}
	}

	/* Succeeded: how many. */
	return count;
}

/*
 * Makes the source of the shader a separable program of one stage is
 * linked with in place of the stage it does not have: for a vertex
 * shader, a fragment shader that does nothing; for a fragment shader, a
 * vertex shader that declares each of its inputs as an output of the
 * same name, type and interpolation (structs declared first), in the
 * same version of the language.  Returns the malloc'ed source, or NULL
 * when there is no memory or the shader is of another stage.
 */
char *
glsl_stand_in_source(
	const struct glsl_shader *shader)
{
	struct glsl_text text;
	jmp_buf failure;
	const struct glsl_type *declared[GLSL_STAND_IN_STRUCTS];
	const struct glsl_symbol *symbol;
	unsigned count;

	/* Out of memory comes back here.  setjmp is the whole controlling expression, the form C allows it in. */
	memset(&text, 0, sizeof(text));
	count = 0U;
	switch (setjmp(failure)) {
	case 0:
		break;
	default:
		free(text.data);
		return NULL;
	}

	/* The version: OpenGL ES's (1.00 has no "es"), or desktop GL's. */
	if (shader->es && shader->version != 100U) {
		glsl_text_printf(&text, &failure, "#version %u es\n", shader->version);
	} else {
		glsl_text_printf(&text, &failure, "#version %u\n", shader->version);
	}

	/* A vertex shader's stand-in: a fragment shader that does nothing. */
	if (shader->stage == GLSL_STAGE_VERTEX) {
		glsl_text_printf(&text, &failure, "void main()\n{\n}\n");
		return text.data;
	}

	/* Only a fragment shader has the other. */
	if (shader->stage != GLSL_STAGE_FRAGMENT) {
		free(text.data);
		return NULL;
	}

	/* Each input of the fragment shader, an output of the stand-in. */
	for (symbol = shader->globals; symbol != NULL; symbol = symbol->next_global) {
		if (symbol->where != GLSL_VAR_INPUT || symbol->builtin != GLSL_BUILTIN_NONE)
			continue;

		/* The structs its type names, then the output with the input's interpolation. */
		glsl_stand_in_structs(&text, &failure, symbol->type, declared, &count);
		if (symbol->interpolation == GLSL_INTERP_FLAT)
			glsl_text_printf(&text, &failure, "flat ");
		if (symbol->centroid)
			glsl_text_printf(&text, &failure, "centroid ");
		if (shader->version == 100U) {
			glsl_text_printf(&text, &failure, "varying ");
		} else {
			glsl_text_printf(&text, &failure, "out ");
		}
		glsl_stand_in_variable(&text, &failure, symbol->type, symbol->name);
	}

	/* Succeeded: a main that writes only the position. */
	glsl_text_printf(&text, &failure, "void main()\n{\n\tgl_Position = vec4(0.0);\n}\n");
	return text.data;
}

/* Hands the shader's log to the caller (NULL when empty), leaving the shader without one. */
static void
glsl_give_log(
	struct glsl_shader *shader,
	char **log)
{
	/* An empty log is no log. */
	if (shader->log.length == 0U)
		return;

	/* The log changes hands. */
	*log = shader->log.data;
	shader->log.data = NULL;
	shader->log.length = 0U;
	shader->log.capacity = 0U;
}

/*
 * Reports a compiled compute shader's workgroup size (ws101-p008); all 0
 * for another stage.
 */
void
glsl_compute_layout(
	const struct glsl_shader *shader,
	unsigned size[3])
{
	unsigned axis;

	/* The layout's size, 0s for another stage. */
	for (axis = 0U; axis < 3U; axis++) {
		size[axis] = 0U;
		if (shader->stage == GLSL_STAGE_COMPUTE)
			size[axis] = shader->local_size[axis];
	}
}

/*
 * Reports a compiled tessellation shader's layouts as GL names them; 0s
 * for what the shader's stage does not have.
 */
void
glsl_tess_layout(
	const struct glsl_shader *shader,
	unsigned *vertices,
	unsigned *domain,
	unsigned *spacing,
	unsigned *order,
	unsigned *point_mode)
{
	/* Nothing for another stage. */
	*vertices = 0U;
	*domain = 0U;
	*spacing = 0U;
	*order = 0U;
	*point_mode = 0U;

	/* A control shader's output vertices. */
	if (shader->stage == GLSL_STAGE_TESS_CONTROL) {
		*vertices = shader->tess_vertices;
		return;
	}
	if (shader->stage != GLSL_STAGE_TESS_EVALUATION)
		return;

	/* An evaluation shader's domain: GL_TRIANGLES, GL_QUADS or GL_ISOLINES. */
	*domain = 0x0004U;
	if (shader->tess_domain == GLSL_PRIMITIVE_QUADS)
		*domain = 0x0007U;
	if (shader->tess_domain == GLSL_PRIMITIVE_ISOLINES)
		*domain = 0x8E7AU;

	/* Its spacing: GL_EQUAL (the default), GL_FRACTIONAL_EVEN, GL_FRACTIONAL_ODD. */
	*spacing = 0x0202U;
	if (shader->tess_spacing == GLSL_SPACING_FRACTIONAL_EVEN)
		*spacing = 0x8E7CU;
	if (shader->tess_spacing == GLSL_SPACING_FRACTIONAL_ODD)
		*spacing = 0x8E7BU;

	/* Its vertex order: GL_CCW (the default) or GL_CW, and the point mode. */
	*order = 0x0901U;
	if (shader->tess_order == GLSL_ORDER_CW)
		*order = 0x0900U;
	*point_mode = shader->tess_point_mode;
}

/* Declares the struct types a type names (a struct's members' first), each once. */
static void
glsl_stand_in_structs(
	struct glsl_text *text,
	jmp_buf *failure,
	const struct glsl_type *type,
	const struct glsl_type **declared,
	unsigned *count)
{
	unsigned index;

	/* An array's element type. */
	while (type->kind == GLSL_KIND_ARRAY)
		type = type->element;
	if (type->kind != GLSL_KIND_STRUCT)
		return;

	/* A struct declared already. */
	for (index = 0U; index < *count; index++) {
		if (declared[index] == type)
			return;
	}

	/* Its members' structs first. */
	for (index = 0U; index < type->field_count; index++)
		glsl_stand_in_structs(text, failure, type->fields[index].type, declared, count);

	/* The struct, remembered. */
	glsl_text_printf(text, failure, "struct %s\n{\n", type->name);
	for (index = 0U; index < type->field_count; index++) {
		glsl_text_printf(text, failure, "\t");
		glsl_stand_in_variable(text, failure, type->fields[index].type, type->fields[index].name);
	}
	glsl_text_printf(text, failure, "};\n");
	if (*count < GLSL_STAND_IN_STRUCTS) {
		declared[*count] = type;
		(*count)++;
	}
}

/* Writes a declaration's type and name with an array's lengths after the name ("vec4 v[2][3];"). */
static void
glsl_stand_in_variable(
	struct glsl_text *text,
	jmp_buf *failure,
	const struct glsl_type *type,
	const char *name)
{
	const struct glsl_type *element;

	/* The innermost element's type, and the name. */
	element = type;
	while (element->kind == GLSL_KIND_ARRAY)
		element = element->element;
	glsl_text_printf(text, failure, "%s %s", element->name, name);

	/* Each array's length, the outermost first. */
	for (element = type;
	     element->kind == GLSL_KIND_ARRAY;
	     element = element->element)
		glsl_text_printf(text, failure, "[%u]", element->length);

	/* The declaration ends. */
	glsl_text_printf(text, failure, ";\n");
}
