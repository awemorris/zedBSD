/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Shaders and programs of zedBSD's OpenGL ES (WS068 p008).
 *
 * A shader is GLSL compiled by glCompileShader (glsl/, WS068 p015-p019:
 * checked when compiled, made SPIR-V when the program links, so that the
 * uniform block has one layout in both stages), or SPIR-V given by
 * glShaderBinary.  Linking reads both shaders' interfaces, applies the
 * attribute locations glBindAttribLocation gave (and the output locations
 * desktop GL's glBindFragDataLocation gave, libGL), gives each fragment
 * input the location of the vertex output of the same name, rewrites the
 * vertex shader's gl_Position for Vulkan, merges the two uniform blocks
 * by name, numbers the uniform locations, and makes the Vulkan shaders
 * and layouts.  The uniform values live in a copy of the block that each
 * draw hands to the GPU.
 *
 * Named uniform blocks (OpenGL ES 3, WS068 p024) are read from buffers:
 * the link lists each block at its binding (32 on) with its name, std140
 * size and members (from the GLSL compiler; a SPIR-V binary's blocks have
 * their type's name and no size or members), and a draw hands each block
 * the buffer range bound to the binding point glUniformBlockBinding gave
 * it.
 *
 * A compute shader (OpenGL ES 3.1, ws101-p009) is GLSL linked alone: the
 * program has its uniforms and named blocks as above, its shader storage
 * blocks (at bindings 56 on, from the GLSL compiler's link) and its
 * workgroup size, and one compute pipeline made at the link.
 *
 * Desktop GL 4.0's subroutines (libGL) are the GLSL compiler's: a
 * subroutine uniform is a hidden uint of the default block
 * (gl_ZedSubroutine<stage>_<name>) whose value is the index of the
 * subroutine a call runs.  The link keeps each stage's subroutines and
 * subroutine uniforms (with their locations, numbered in declaration
 * order) and the hidden uniforms' places; the context keeps the index
 * each location selects (glUniformSubroutinesuiv), and each draw writes
 * them into the block.
 */

#include "gles.h"
#include "glsl/glsl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The length of a link or compile log. */
#define PROGRAM_LOG		1024U

/* The version of a GLSL source without #version: OpenGL ES's 1.00, desktop GL's 1.10 (libGL). */
#define PROGRAM_ES_VERSION	100U
#define PROGRAM_DESKTOP_VERSION	110U

/* The first version of GLSL ES that needs an OpenGL ES 3 context. */
#define PROGRAM_ES3_VERSION	300U

/* The first version of GLSL ES that needs a context offering OpenGL ES 3.1's compute (ws101-p009). */
#define PROGRAM_ES31_VERSION	310U

/* SPIR-V's execution model of a compute shader (GLCompute). */
#define PROGRAM_MODEL_COMPUTE	5U

/* SPIR-V's OpDecorate and its Index decoration (a fragment output's dual-source blend input, desktop GL 3.3). */
#define PROGRAM_OP_DECORATE	71U
#define PROGRAM_DECORATION_INDEX 32U

/*
 * The kinds of values a glUniform call gives and a glGetUniform call
 * asks for (program_uniform, program_get_uniform), and gles_uniform.base
 * of a double (desktop GL 4.0, GL_ARB_gpu_shader_fp64).
 */
#define PROGRAM_FLOATS		0
#define PROGRAM_INTS		1
#define PROGRAM_UNSIGNEDS	2
#define PROGRAM_DOUBLES		3
#define PROGRAM_BASE_DOUBLE	4U

/* Desktop GL's names of its 1D sampler types, which OpenGL ES has none of. */
#define PROGRAM_SAMPLER_1D		0x8B5DU
#define PROGRAM_SAMPLER_1D_SHADOW	0x8B61U

/*
 * The program interfaces of OpenGL ES 3.1 section 7.3.1 as bits (which
 * interfaces a property is one of), and the room for a resource's name
 * and for the values of one property (a block's active variables).
 */
#define PROGRAM_IF_UNIFORM		0x01U
#define PROGRAM_IF_UNIFORM_BLOCK	0x02U
#define PROGRAM_IF_ATOMIC		0x04U
#define PROGRAM_IF_INPUT		0x08U
#define PROGRAM_IF_OUTPUT		0x10U
#define PROGRAM_IF_CAPTURE		0x20U
#define PROGRAM_IF_BUFFER_VARIABLE	0x40U
#define PROGRAM_IF_STORAGE_BLOCK	0x80U
#define PROGRAM_IF_ALL			0xffU
#define PROGRAM_RESOURCE_NAME		(GLES_NAME + 8U)
#define PROGRAM_RESOURCE_VALUES		1024U

/* The serial of the next link (0 is never one). */
static uint64_t program_serial = 1U;

static struct gles_shader *program_shader(struct zegl_context *context, GLuint name);
static void timed_compile_shader(GLuint name);
static void program_compile(struct zegl_context *context, struct gles_shader *shader);
static int program_link_alone(struct gles_state *state, struct gles_program *program, char *log);
static struct gles_shader *program_hidden_shader(struct zegl_context *context, GLenum type, const char *source);
static void program_keep_sources(struct gles_program *program);
static struct gles_program *program_uniform_program(struct gles_state *state);
static struct gles_program *program_combine(struct zegl_context *context, struct gles_state *state, struct gles_program *vertex, struct gles_program *fragment);
static char *program_renamed_source(const char *source, const struct gles_program *vertex, const struct gles_program *fragment);
static int program_name_taken(const struct gles_program *program, const char *name, size_t length);
static int program_combine_sources(struct gles_pipeline_object *pipeline, const struct gles_program *combined);
static void program_combine_values(const struct gles_pipeline_object *pipeline);
static void timed_link_program(GLuint name);
static struct gles_program *program_get(struct zegl_context *context, GLuint name);
static int program_link(struct gles_state *state, struct gles_program *program, char *log);
static int program_link_code(struct gles_state *state, struct gles_program *program, const uint32_t *vertex_input, size_t vertex_words, const uint32_t *fragment_input, size_t fragment_words, const uint32_t *geometry_input, size_t geometry_words, const struct glsl_program *tessellation, char *log);
static int program_link_geometry(struct gles_state *state, struct gles_program *program, const uint32_t *code, size_t words, char *log);
static int program_link_tessellation(struct gles_state *state, struct gles_program *program, const struct glsl_program *linked, int last, char *log);
static int program_stage_modules(struct gles_state *state, const uint32_t *code, size_t words, int rewrite, VkShaderModule *window, VkShaderModule *fbo, char *log);
static int program_link_glsl(struct gles_program *program, struct glsl_program *linked, char *log);
static int program_link_compute(struct gles_state *state, struct gles_program *program, char *log);
static int program_compute_images(struct gles_state *state, struct gles_program *program, const struct glsl_program *linked, char *log);
static int program_multisample_refused(struct gles_state *state, const struct gles_program *program, char *log);
static int program_compute_storages(struct gles_state *state, struct gles_program *program, const struct glsl_program *linked, char *log);
static int program_compute_pipeline(struct gles_state *state, struct gles_program *program, const uint32_t *code, size_t words, char *log);
static void program_let_go(struct gles_state *state, struct gles_shader *shader);
static int program_glsl_captures(struct gles_program *program, const struct glsl_program *linked, char *log);
static int program_capture_special(const char *name, unsigned *skipped);
static void program_glsl_types(struct gles_program *program, const struct glsl_program *linked);
static GLenum program_sampler_type(const struct glsl_uniform_info *info);
static int program_buffer_sampler(GLenum type);
static int program_flat_inputs(const uint32_t *code, size_t words);
static void program_bind_output(GLuint name, GLuint color, GLuint index, const GLchar *output);
static int program_output_indexes(struct gles_spirv *fragment, uint32_t **code, size_t *words, char *log);
static int program_glsl_blocks(struct gles_program *program, const struct glsl_program *linked, char *log);
static int program_spirv_blocks(struct gles_program *program, const struct gles_spirv *spirv, unsigned stage, char *log);
static unsigned program_block_slot(struct gles_program *program, uint32_t binding, uint32_t element);
static int program_merge(struct gles_program *program, struct gles_spirv *spirv, char *log);
static int program_interface_keep(struct gles_program *program, const struct gles_spirv *vertex, const struct gles_spirv *fragment);
static int program_buffer_variables(struct gles_program *program, const struct glsl_program *linked);
static struct gles_program *program_resource_program(struct zegl_context *context, GLuint name);
static unsigned program_interface_bit(GLenum interface);
static unsigned program_resource_count(const struct gles_program *program, unsigned interface);
static void program_resource_name(const struct gles_program *program, unsigned interface, unsigned index, char *out);
static unsigned program_resource_find(const struct gles_program *program, unsigned interface, const char *name);
static GLint program_resource_variables(const struct gles_program *program, unsigned interface, unsigned index, GLint *indices);
static int program_property_allowed(GLenum property, unsigned interface);
static unsigned program_resource_property(const struct gles_program *program, unsigned interface, unsigned index, GLenum property, GLint *values);
static GLbitfield program_resource_stages(const struct gles_program *program, unsigned interface, unsigned index);
static GLbitfield program_block_stages(unsigned bits);
static void program_uniform_resource(const struct gles_uniform *uniform, GLenum property, GLint *values);
static void program_variable_resource(const struct gles_program *program, unsigned interface, unsigned index, GLenum property, GLint *values);
static void program_buffer_resource(const struct gles_buffer_variable *variable, GLenum property, GLint *values);
static int program_builtin_uniform(struct gles_program *program, const struct gles_uniform *uniform);
static int program_layout(struct gles_state *state, struct gles_program *program);
static int program_module(struct gles_state *state, const uint32_t *code, size_t words, VkShaderModule *module);
static int program_keep_last(struct gles_program *program, const uint32_t *code, size_t words, char *log);
static void program_unlink(struct gles_state *state, struct gles_program *program);
static void program_uniform(GLint location, GLsizei count, unsigned components, const void *values, int kind);
static void program_matrix(GLint location, GLsizei count, GLboolean transpose, const void *values, int doubles, unsigned columns, unsigned rows);
static struct gles_uniform *program_location(struct zegl_context *context, GLint location, unsigned *element);
static void program_get_uniform(GLuint name, GLint location, int kind, void *params);
static struct gles_block *program_block(struct zegl_context *context, struct gles_program *program, GLuint index);
static int program_uniform_property(const struct gles_uniform *uniform, GLenum pname, GLint *value);
static void program_copy_string(const char *text, GLsizei size, GLsizei *length, GLchar *out);
static void program_log(char **log, const char *text);
static int program_link_subroutines(struct gles_program *program, char *log);
static struct gles_subroutines *program_stage_subroutines(const struct glsl_shader *shader);
static void program_subroutines_free(struct gles_subroutines *subroutines);
static int program_hidden_subroutine(struct gles_program *program, const struct gles_uniform *uniform);
static int program_subroutine_stage(struct zegl_context *context, GLenum shadertype, unsigned *stage);
static const struct gles_subroutines *program_stage_of(struct zegl_context *context, GLuint name, GLenum shadertype);
static const struct gles_subroutine_uniform *program_subroutine_at(const struct gles_subroutines *subroutines, unsigned location);
static GLint program_subroutine_max_length(const struct gles_subroutines *subroutines, int uniforms);

/*
 * Frees a program's link and the program; shaders it had attached are let
 * go (and freed when they were waiting for that).
 */
void
gles_program_release(
	struct gles_state *state,
	struct gles_program *program)
{
	/* The link's objects. */
	program_unlink(state, program);

	/* The attached shaders let go. */
	program_let_go(state, program->vertex);
	program_let_go(state, program->fragment);
	program_let_go(state, program->geometry);
	program_let_go(state, program->compute);
	program_let_go(state, program->tess_control);
	program_let_go(state, program->tess_evaluation);

	/* The program. */
	free(program->log);
	free(program);
}

/*
 * Returns the program of a name, recording the error when the name is not
 * one (GL_INVALID_VALUE) or names a shader (GL_INVALID_OPERATION).
 */
struct gles_program *
gles_program_named(
	struct zegl_context *context,
	GLuint name)
{
	struct gles_program *program;

	/* Succeeded: the program, or NULL. */
	program = program_get(context, name);
	return program;
}

/*
 * Returns the program draws and transform feedback use: the current
 * program, or without one the bound program pipeline object's (NULL with
 * the error recorded when it cannot draw), or NULL when neither is there.
 */
struct gles_program *
gles_program_current(
	struct zegl_context *context,
	struct gles_state *state)
{
	struct gles_program *program;

	/* The current program, or no pipeline. */
	if (state->program != NULL || state->pipeline == NULL)
		return state->program;

	/* Succeeded: the pipeline's. */
	program = gles_pipeline_program(context, state, state->pipeline);
	return program;
}

/*
 * Returns the program a program pipeline object draws with: its vertex
 * and fragment stages' programs (a stand-in fragment shader without one)
 * linked again together when a stage's program was linked again since,
 * with the stages' uniforms' values and blocks' bindings.  Returns NULL
 * with the error recorded when it cannot draw (GL_INVALID_OPERATION: no
 * vertex stage, a stage's program no longer linked or no longer
 * separable, or a link of the two that fails because their interfaces
 * do not match; GL_OUT_OF_MEMORY).
 */
struct gles_program *
gles_pipeline_program(
	struct zegl_context *context,
	struct gles_state *state,
	struct gles_pipeline_object *pipeline)
{
	int status;

	/* The program made, or why not. */
	status = gles_pipeline_make(context, state, pipeline);
	if (status != 0) {
		gles_error(context, (GLenum)status);
		return NULL;
	}

	/* Succeeded: the program (the vertex stage's when one program has both stages), with the stages' values. */
	if (pipeline->vertex == pipeline->fragment)
		return pipeline->vertex;
	program_combine_values(pipeline);
	return pipeline->combined;
}

/*
 * Makes the program a program pipeline object draws with when the
 * stages' links changed (gles_pipeline_program), recording no error.
 * Returns 0, or the error a draw records: GL_INVALID_OPERATION when it
 * cannot draw, GL_OUT_OF_MEMORY.
 */
int
gles_pipeline_make(
	struct zegl_context *context,
	struct gles_state *state,
	struct gles_pipeline_object *pipeline)
{
	struct gles_program *vertex;
	struct gles_program *fragment;
	struct gles_program *combined;
	uint64_t fragment_serial;
	int status;

	/* A vertex stage, and stages whose programs are still linked separable. */
	vertex = pipeline->vertex;
	fragment = pipeline->fragment;
	if (vertex == NULL || !vertex->linked || !vertex->linked_separable)
		return (int)GL_INVALID_OPERATION;
	if (fragment != NULL && (!fragment->linked || !fragment->linked_separable))
		return (int)GL_INVALID_OPERATION;

	/* One program of both stages draws as it is. */
	if (vertex == fragment)
		return 0;

	/* The program made for the stages' links, or a new one. */
	fragment_serial = 0U;
	if (fragment != NULL)
		fragment_serial = fragment->serial;
	if (pipeline->combined == NULL ||
	    pipeline->combined_serials[0] != vertex->serial ||
	    pipeline->combined_serials[1] != fragment_serial) {
		gles_pipeline_forget(state, pipeline);
		combined = program_combine(context, state, vertex, fragment);
		if (combined == NULL)
			return (int)GL_INVALID_OPERATION;

		/* Made for these links, with where its uniforms come from. */
		pipeline->combined = combined;
		pipeline->combined_serials[0] = vertex->serial;
		pipeline->combined_serials[1] = fragment_serial;
		status = program_combine_sources(pipeline, combined);
		if (status != 0) {
			gles_pipeline_forget(state, pipeline);
			return (int)GL_OUT_OF_MEMORY;
		}
	}

	/* Succeeded: the program is made. */
	return 0;
}

/*
 * Lets go of the program a program pipeline object drew with (its link
 * waits for the frame), so the next draw makes it again.
 */
void
gles_pipeline_forget(
	struct gles_state *state,
	struct gles_pipeline_object *pipeline)
{
	/* Nothing made. */
	if (pipeline->combined == NULL)
		return;

	/* The program and the table of its uniforms' sources. */
	gles_program_release(state, pipeline->combined);
	free(pipeline->uniform_sources);
	pipeline->combined = NULL;
	pipeline->uniform_sources = NULL;
	memset(pipeline->block_sources, 0, sizeof(pipeline->block_sources));
	pipeline->combined_serials[0] = 0U;
	pipeline->combined_serials[1] = 0U;
}

/*
 * Adds text to a program's info log (glCreateShaderProgramv's shader's
 * log after the link's).
 */
void
gles_program_append_log(
	struct gles_program *program,
	const char *text)
{
	char *joined;
	size_t had;
	size_t adds;

	/* Nothing to add. */
	if (text == NULL || text[0] == '\0')
		return;

	/* The log with the text after it. */
	had = 0U;
	if (program->log != NULL)
		had = strlen(program->log);
	adds = strlen(text);
	joined = malloc(had + adds + 1U);
	if (joined == NULL)
		return;
	if (had != 0U)
		memcpy(joined, program->log, had);
	memcpy(joined + had, text, adds + 1U);

	/* Succeeded: the longer log. */
	free(program->log);
	program->log = joined;
}

/*
 * Notes that a program pipeline object holds a program (as a stage or as
 * its active program), which glDeleteProgram then lets wait.
 */
void
gles_program_use(
	struct gles_program *program)
{
	/* One more. */
	if (program != NULL)
		program->pipeline_uses++;
}

/*
 * Notes that a program pipeline object no longer holds a program, which
 * goes when glDeleteProgram was waiting for that and it is not current.
 */
void
gles_program_unuse(
	struct gles_state *state,
	struct gles_program *program)
{
	/* No program. */
	if (program == NULL)
		return;

	/* One fewer; the last lets a deleted program go. */
	program->pipeline_uses--;
	if (program->pipeline_uses != 0U || !program->delete_pending || state->program == program)
		return;
	gles_names_remove(&state->objects, program->name);
	gles_program_release(state, program);
}

/*
 * Frees the program pipeline objects with what they made, before the
 * programs go (the context's state is going).
 */
void
gles_pipelines_release(
	struct gles_state *state)
{
	struct gles_pipeline_object *pipeline;
	GLuint name;

	/* Each object: its program, its log, the object. */
	state->pipeline = NULL;
	for (name = 1U; name < state->pipeline_objects.capacity; name++) {
		pipeline = state->pipeline_objects.objects[name];
		if (pipeline == NULL)
			continue;
		gles_pipeline_forget(state, pipeline);
		free(pipeline->log);
		free(pipeline);
		state->pipeline_objects.objects[name] = NULL;
	}

	/* The namespace. */
	free(state->pipeline_objects.objects);
	memset(&state->pipeline_objects, 0, sizeof(state->pipeline_objects));
}

/*
 * Returns a linked program's module of the last stage before the
 * rasterizer with gl_Position's z kept as the shader wrote it (desktop
 * GL's glClipControl(GL_ZERO_TO_ONE)) and y turned over when flip is
 * nonzero, made at its first use and kept until the program's link goes;
 * VK_NULL_HANDLE when it cannot be made.
 */
VkShaderModule
gles_program_zero_module(
	struct gles_state *state,
	struct gles_program *program,
	int flip)
{
	uint32_t *patched;
	size_t patched_words;
	unsigned index;
	int status;

	/* One made before. */
	index = 0U;
	if (flip)
		index = 1U;
	if (program->zero_modules[index] != VK_NULL_HANDLE)
		return program->zero_modules[index];

	/* Without the last stage's code there is none. */
	if (program->last_code == NULL)
		return VK_NULL_HANDLE;

	/* The code with gl_Position's y turned over or not, its z as it is. */
	patched = gles_spirv_position(program->last_code, program->last_words, flip, 0, &patched_words);
	if (patched == NULL)
		return VK_NULL_HANDLE;

	/* Its module. */
	status = program_module(state, patched, patched_words, &program->zero_modules[index]);
	free(patched);
	if (status != 0) {
		program->zero_modules[index] = VK_NULL_HANDLE;
		return VK_NULL_HANDLE;
	}

	/* Succeeded: the module. */
	return program->zero_modules[index];
}

/*
 * Frees a shader.
 */
void
gles_shader_release(
	struct gles_shader *shader)
{
	/* The code, the source, the compiled GLSL, the log and the shader. */
	free(shader->code);
	free(shader->source);
	glsl_shader_free(shader->glsl);
	free(shader->log);
	free(shader);
}

/*
 * Starts each subroutine uniform location of a program's stages at a
 * subroutine compatible with it (the first; 0 when none is), as
 * glUseProgram does (desktop GL 4.0).
 */
void
gles_subroutines_reset(
	struct gles_state *state,
	const struct gles_program *program)
{
	const struct gles_subroutines *subroutines;
	const struct gles_subroutine_uniform *uniform;
	unsigned stage;
	unsigned index;
	unsigned element;
	GLuint chosen;

	/* Each stage's uniforms, each element. */
	for (stage = 0U; stage < GLES_STAGES; stage++) {
		subroutines = program->subroutines[stage];
		if (subroutines == NULL)
			continue;
		for (index = 0U; index < subroutines->uniform_count; index++) {
			uniform = &subroutines->uniforms[index];
			chosen = 0U;
			if (uniform->compatible_count != 0U)
				chosen = (GLuint)uniform->compatible[0];

			/* The first compatible subroutine at each of its locations. */
			for (element = 0U; element < uniform->size; element++)
				state->subroutine_indices[stage][uniform->location + element] = chosen;
		}
	}
}

/*
 * Writes the subroutine each location selects into the hidden uniforms of
 * a program's block, before a draw or a dispatch hands the block to the
 * GPU (desktop GL 4.0).
 */
void
gles_subroutines_write(
	const struct gles_state *state,
	struct gles_program *program)
{
	const struct gles_subroutines *subroutines;
	const struct gles_subroutine_uniform *uniform;
	unsigned stage;
	unsigned index;
	unsigned element;
	uint32_t value;

	/* Each stage's uniforms a call reads, each element. */
	for (stage = 0U; stage < GLES_STAGES; stage++) {
		subroutines = program->subroutines[stage];
		if (subroutines == NULL || program->uniform_data == NULL)
			continue;
		for (index = 0U; index < subroutines->uniform_count; index++) {
			uniform = &subroutines->uniforms[index];
			if (uniform->offset < 0)
				continue;

			/* The index the element's location selects. */
			for (element = 0U; element < uniform->size; element++) {
				value = (uint32_t)state->subroutine_indices[stage][uniform->location + element];
				memcpy(program->uniform_data + uniform->offset + element * uniform->stride, &value, sizeof(value));
			}
		}
	}
}

/*
 * Makes a shader of a stage.
 */
GL_APICALL GLuint GL_APIENTRY
glCreateShader(
	GLenum type)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_shader *shader;
	int status;

	/* A context with its state, and a stage. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return 0U;
	if (type != GL_VERTEX_SHADER &&
	    type != GL_FRAGMENT_SHADER &&
	    (type != GL_GEOMETRY_SHADER ||
	     gles_fixed == NULL ||
	     !state->display->features.geometryShader) &&
	    ((type != GL_TESS_CONTROL_SHADER && type != GL_TESS_EVALUATION_SHADER) ||
	     gles_fixed == NULL ||
	     !state->display->features.tessellationShader) &&
	    (type != GL_COMPUTE_SHADER || !state->compute)) {
		gles_error(context, GL_INVALID_ENUM);
		return 0U;
	}

	/* The shader under a free name of the shared namespace. */
	shader = calloc(1U, sizeof(*shader));
	if (shader == NULL) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return 0U;
	}

	/* The shader under the name. */
	shader->kind = GLES_KIND_SHADER;
	shader->type = type;
	shader->name = gles_names_free(&state->objects);
	status = gles_names_add(&state->objects, shader->name, shader);
	if (status != 0) {
		free(shader);
		gles_error(context, GL_OUT_OF_MEMORY);
		return 0U;
	}

	/* Succeeded: its name. */
	return shader->name;
}

/*
 * Deletes a shader, once no program has it attached.
 */
GL_APICALL void GL_APIENTRY
glDeleteShader(
	GLuint name)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_shader *shader;

	/* Name 0 is ignored. */
	if (name == 0U)
		return;

	/* The shader. */
	context = gles_context();
	state = gles_state(context);
	shader = program_shader(context, name);
	if (shader == NULL)
		return;

	/* Attached: it waits; else it goes now. */
	shader->delete_pending = 1;
	if (shader->attached != 0U)
		return;
	gles_names_remove(&state->objects, name);
	gles_shader_release(shader);
}

/*
 * Gives a shader its GLSL source (kept for glGetShaderSource and
 * glCompileShader).
 */
GL_APICALL void GL_APIENTRY
glShaderSource(
	GLuint name,
	GLsizei count,
	const GLchar *const *string,
	const GLint *length)
{
	struct zegl_context *context;
	struct gles_shader *shader;
	char *source;
	size_t total;
	size_t part;
	GLsizei index;

	/* The shader and a count. */
	context = gles_context();
	shader = program_shader(context, name);
	if (shader == NULL)
		return;
	if (count < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* The parts' total length. */
	total = 0U;
	for (index = 0; index < count; index++) {
		if (length != NULL && length[index] >= 0) {
			total += (size_t)length[index];
		} else {
			total += strlen(string[index]);
		}
	}

	/* The parts joined. */
	source = malloc(total + 1U);
	if (source == NULL) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return;
	}

	/* The parts, one after another. */
	total = 0U;
	for (index = 0; index < count; index++) {
		if (length != NULL && length[index] >= 0) {
			part = (size_t)length[index];
		} else {
			part = strlen(string[index]);
		}

		/* The part. */
		memcpy(source + total, string[index], part);
		total += part;
	}

	/* Succeeded: the source replaces the old one. */
	source[total] = '\0';
	free(shader->source);
	shader->source = source;
}

/*
 * Compiles a shader's source (timed, timed_compile_shader).
 */
GL_APICALL void GL_APIENTRY
glCompileShader(
	GLuint name)
{
	uint64_t started;

	/* The call, timed when ZEDBSD_GLES_COMPUTE_TRACE is 2. */
	started = gles_time_begin();
	timed_compile_shader(name);
	gles_time_end("compile-shader", started, 0U);
}

/*
 * Compiles a shader's GLSL source (a shader given only a SPIR-V binary
 * stays compiled).  The log says what the compiler found.
 */
static void
timed_compile_shader(
	GLuint name)
{
	struct zegl_context *context;
	struct gles_shader *shader;

	/* The shader. */
	context = gles_context();
	shader = program_shader(context, name);
	if (shader == NULL)
		return;

	/* Its source compiled. */
	program_compile(context, shader);
}

/*
 * Compiles a shader's GLSL source for the current context (a shader given
 * only a SPIR-V binary stays compiled), also a shader of no name that a
 * program pipeline links; the log says what the compiler found.
 */
static void
program_compile(
	struct zegl_context *context,
	struct gles_shader *shader)
{
	struct gles_state *state;
	struct glsl_shader *compiled;
	unsigned stage;
	unsigned version;
	unsigned shader_version;
	unsigned latest;
	unsigned features;
	char message[96];
	int es;
	char *log;

	/* Without a source, a binary already given stays compiled. */
	if (shader->source == NULL) {
		shader->compiled = 0;
		if (shader->code != NULL)
			shader->compiled = 1;
		program_log(&shader->log, "");
		if (shader->code == NULL)
			program_log(&shader->log, "the shader has no source\n");
		return;
	}

	/* The source compiled for the shader's stage, in OpenGL ES's GLSL or (libGL) desktop GL's. */
	stage = GLSL_STAGE_VERTEX;
	if (shader->type == GL_FRAGMENT_SHADER)
		stage = GLSL_STAGE_FRAGMENT;
	if (shader->type == GL_GEOMETRY_SHADER)
		stage = GLSL_STAGE_GEOMETRY;
	if (shader->type == GL_COMPUTE_SHADER)
		stage = GLSL_STAGE_COMPUTE;
	if (shader->type == GL_TESS_CONTROL_SHADER)
		stage = GLSL_STAGE_TESS_CONTROL;
	if (shader->type == GL_TESS_EVALUATION_SHADER)
		stage = GLSL_STAGE_TESS_EVALUATION;
	version = PROGRAM_ES_VERSION;
	if (gles_fixed != NULL)
		version = PROGRAM_DESKTOP_VERSION;

	/*
	 * The optional features the device has: doubles (desktop GLSL 4.00,
	 * GL_ARB_gpu_shader_fp64), and computed gather offsets, interpolation
	 * functions and computed indices into arrays of samplers and of
	 * uniform blocks (GL_ARB_gpu_shader5).
	 */
	features = 0U;
	state = gles_state(context);
	if (state != NULL && state->display->features.shaderFloat64)
		features |= GLSL_FEATURE_FP64;
	if (state != NULL &&
	    state->display->features.shaderImageGatherExtended &&
	    state->display->features.sampleRateShading &&
	    state->display->features.shaderSampledImageArrayDynamicIndexing &&
	    state->display->features.shaderUniformBufferArrayDynamicIndexing)
		features |= GLSL_FEATURE_SHADER5;
	compiled = glsl_compile_features(stage, shader->source, version, features, &log);

	/* The log (errors, or the warnings of a shader that compiled). */
	if (log != NULL) {
		program_log(&shader->log, log);
		free(log);
	} else if (compiled == NULL) {
		program_log(&shader->log, "the compiler ran out of memory\n");
	} else {
		program_log(&shader->log, "");
	}

	/* A desktop GLSL version no later than the context's (libGL). */
	if (compiled != NULL && gles_fixed != NULL) {
		shader_version = glsl_shader_version(compiled, &es);
		latest = gles_fixed->glsl_version();
		if (!es && latest != 0U && shader_version > latest) {
			(void)snprintf(message, sizeof(message), "0:0: error: GLSL %u is later than the context's %u\n", shader_version,
				       latest);
			program_log(&shader->log, message);
			glsl_shader_free(compiled);
			compiled = NULL;
		}
	}

	/* GLSL ES 3.00 needs an OpenGL ES 3 context (EGL_CONTEXT_CLIENT_VERSION 3). */
	if (compiled != NULL && gles_fixed == NULL) {
		shader_version = glsl_shader_version(compiled, &es);
		if (es && shader_version >= PROGRAM_ES3_VERSION && context->version < 3) {
			program_log(&shader->log, "0:0: error: GLSL ES 3.00 needs an OpenGL ES 3 context\n");
			glsl_shader_free(compiled);
			compiled = NULL;
		}
	}

	/* GLSL ES 3.10 needs a context that offers OpenGL ES 3.1's compute (ws101-p009). */
	if (compiled != NULL && gles_fixed == NULL) {
		shader_version = glsl_shader_version(compiled, &es);
		state = gles_state(context);
		if (es && shader_version >= PROGRAM_ES31_VERSION && (state == NULL || !state->compute)) {
			program_log(&shader->log, "0:0: error: GLSL ES 3.10 needs an OpenGL ES 3.1 context\n");
			glsl_shader_free(compiled);
			compiled = NULL;
		}
	}

	/* A failed compile leaves the shader without code. */
	glsl_shader_free(shader->glsl);
	shader->glsl = compiled;
	if (compiled == NULL) {
		shader->compiled = 0;
		return;
	}

	/* Succeeded: the GLSL replaces any binary. */
	free(shader->code);
	shader->code = NULL;
	shader->words = 0U;
	shader->compiled = 1;
}

/*
 * Gives shaders a binary: SPIR-V, the one format offered.
 */
GL_APICALL void GL_APIENTRY
glShaderBinary(
	GLsizei count,
	const GLuint *shaders,
	GLenum binaryformat,
	const void *binary,
	GLsizei length)
{
	struct zegl_context *context;
	struct gles_shader *shader;
	uint32_t *code;
	GLsizei index;

	/* A context, the format, and whole words. */
	context = gles_context();
	if (context == NULL)
		return;
	if (binaryformat != GL_SHADER_BINARY_FORMAT_SPIR_V) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* Whole words, at least a header. */
	if (count < 0 || length < 20 || (length % 4) != 0 || binary == NULL) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Each shader gets its own copy. */
	for (index = 0; index < count; index++) {
		shader = program_shader(context, shaders[index]);
		if (shader == NULL)
			return;
		code = malloc((size_t)length);
		if (code == NULL) {
			gles_error(context, GL_OUT_OF_MEMORY);
			return;
		}

		/* The words replace any earlier ones (and compiled GLSL); the shader counts as compiled. */
		memcpy(code, binary, (size_t)length);
		free(shader->code);
		glsl_shader_free(shader->glsl);
		shader->glsl = NULL;
		shader->code = code;
		shader->words = (size_t)length / 4U;
		shader->compiled = 1;
		program_log(&shader->log, "");
	}
}

/*
 * Lets the compiler's resources go (it keeps none between compiles).
 */
GL_APICALL void GL_APIENTRY
glReleaseShaderCompiler(void)
{
	/* Nothing is held. */
	return;
}

/*
 * Reports a property of a shader.
 */
GL_APICALL void GL_APIENTRY
glGetShaderiv(
	GLuint name,
	GLenum pname,
	GLint *params)
{
	struct zegl_context *context;
	struct gles_shader *shader;

	/* The shader. */
	context = gles_context();
	shader = program_shader(context, name);
	if (shader == NULL)
		return;

	/* The property asked for. */
	switch (pname) {
	case GL_SHADER_TYPE:
		*params = (GLint)shader->type;
		return;
	case GL_DELETE_STATUS:
		*params = shader->delete_pending;
		return;
	case GL_COMPILE_STATUS:
		*params = shader->compiled;
		return;
	case GL_INFO_LOG_LENGTH:
		*params = 0;
		if (shader->log != NULL && shader->log[0] != '\0')
			*params = (GLint)strlen(shader->log) + 1;
		return;
	case GL_SHADER_SOURCE_LENGTH:
		*params = 0;
		if (shader->source != NULL)
			*params = (GLint)strlen(shader->source) + 1;
		return;
	default:
		break;
	}

	/* Any other is an error. */
	gles_error(context, GL_INVALID_ENUM);
}

/*
 * Copies a shader's log.
 */
GL_APICALL void GL_APIENTRY
glGetShaderInfoLog(
	GLuint name,
	GLsizei bufSize,
	GLsizei *length,
	GLchar *infoLog)
{
	struct zegl_context *context;
	struct gles_shader *shader;

	/* The shader's log (empty when none). */
	context = gles_context();
	shader = program_shader(context, name);
	if (shader == NULL)
		return;
	program_copy_string(shader->log, bufSize, length, infoLog);
}

/*
 * Copies a shader's source.
 */
GL_APICALL void GL_APIENTRY
glGetShaderSource(
	GLuint name,
	GLsizei bufSize,
	GLsizei *length,
	GLchar *source)
{
	struct zegl_context *context;
	struct gles_shader *shader;

	/* The shader's source (empty when none). */
	context = gles_context();
	shader = program_shader(context, name);
	if (shader == NULL)
		return;
	program_copy_string(shader->source, bufSize, length, source);
}

/*
 * Reports the range and precision of a shader precision: every precision
 * is a 32-bit float or int.
 */
GL_APICALL void GL_APIENTRY
glGetShaderPrecisionFormat(
	GLenum shadertype,
	GLenum precisiontype,
	GLint *range,
	GLint *precision)
{
	/* IEEE single precision for floats, 32-bit two's complement for ints. */
	(void)shadertype;
	range[0] = 127;
	range[1] = 127;
	*precision = 23;
	if (precisiontype == GL_LOW_INT || precisiontype == GL_MEDIUM_INT || precisiontype == GL_HIGH_INT) {
		range[0] = 31;
		range[1] = 30;
		*precision = 0;
	}
}

/*
 * Reports whether a name is a shader.
 */
GL_APICALL GLboolean GL_APIENTRY
glIsShader(
	GLuint name)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_shader *shader;

	/* A context with its state. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return GL_FALSE;

	/* The name's object, if a shader. */
	shader = gles_names_get(&state->objects, name);
	if (shader == NULL || shader->kind != GLES_KIND_SHADER)
		return GL_FALSE;
	return GL_TRUE;
}

/*
 * Makes a program.
 */
GL_APICALL GLuint GL_APIENTRY
glCreateProgram(void)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_program *program;
	int status;

	/* A context with its state. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return 0U;

	/* The program under a free name of the shared namespace. */
	program = calloc(1U, sizeof(*program));
	if (program == NULL) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return 0U;
	}

	/* The program under the name, capturing interleaved outputs by default. */
	program->kind = GLES_KIND_PROGRAM;
	program->feedback_mode = GL_INTERLEAVED_ATTRIBS;
	program->name = gles_names_free(&state->objects);
	status = gles_names_add(&state->objects, program->name, program);
	if (status != 0) {
		free(program);
		gles_error(context, GL_OUT_OF_MEMORY);
		return 0U;
	}

	/* Succeeded: its name. */
	return program->name;
}

/*
 * Deletes a program, once it is no longer current.
 */
GL_APICALL void GL_APIENTRY
glDeleteProgram(
	GLuint name)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_program *program;

	/* Name 0 is ignored. */
	if (name == 0U)
		return;

	/* The program. */
	context = gles_context();
	state = gles_state(context);
	program = program_get(context, name);
	if (program == NULL)
		return;

	/* Current or held by a program pipeline object: it waits; else it goes now. */
	program->delete_pending = 1;
	if (state->program == program || program->pipeline_uses != 0U)
		return;
	gles_names_remove(&state->objects, name);
	gles_program_release(state, program);
}

/*
 * Attaches a shader to a program.
 */
GL_APICALL void GL_APIENTRY
glAttachShader(
	GLuint program_name,
	GLuint shader_name)
{
	struct zegl_context *context;
	struct gles_program *program;
	struct gles_shader *shader;
	struct gles_shader **slot;

	/* The program and the shader. */
	context = gles_context();
	program = program_get(context, program_name);
	if (program == NULL)
		return;
	shader = program_shader(context, shader_name);
	if (shader == NULL)
		return;

	/* Its stage's slot, which must be empty. */
	slot = &program->fragment;
	if (shader->type == GL_VERTEX_SHADER)
		slot = &program->vertex;
	if (shader->type == GL_GEOMETRY_SHADER)
		slot = &program->geometry;
	if (shader->type == GL_COMPUTE_SHADER)
		slot = &program->compute;
	if (shader->type == GL_TESS_CONTROL_SHADER)
		slot = &program->tess_control;
	if (shader->type == GL_TESS_EVALUATION_SHADER)
		slot = &program->tess_evaluation;
	if (*slot != NULL) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* Attached. */
	*slot = shader;
	shader->attached++;
}

/*
 * Detaches a shader from a program.
 */
GL_APICALL void GL_APIENTRY
glDetachShader(
	GLuint program_name,
	GLuint shader_name)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_program *program;
	struct gles_shader *shader;

	/* The program and the shader. */
	context = gles_context();
	state = gles_state(context);
	program = program_get(context, program_name);
	if (program == NULL)
		return;
	shader = program_shader(context, shader_name);
	if (shader == NULL)
		return;

	/* It must be attached. */
	if (program->vertex == shader) {
		program->vertex = NULL;
	} else if (program->fragment == shader) {
		program->fragment = NULL;
	} else if (program->geometry == shader) {
		program->geometry = NULL;
	} else if (program->compute == shader) {
		program->compute = NULL;
	} else if (program->tess_control == shader) {
		program->tess_control = NULL;
	} else if (program->tess_evaluation == shader) {
		program->tess_evaluation = NULL;
	} else {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* Let go; a deleted shader goes with its last program. */
	shader->attached--;
	if (shader->delete_pending && shader->attached == 0U) {
		gles_names_remove(&state->objects, shader_name);
		gles_shader_release(shader);
	}
}

/*
 * Links a program's shaders (timed, timed_link_program).
 */
GL_APICALL void GL_APIENTRY
glLinkProgram(
	GLuint name)
{
	uint64_t started;

	/* The call, timed when ZEDBSD_GLES_COMPUTE_TRACE is 2. */
	started = gles_time_begin();
	timed_link_program(name);
	gles_time_end("link-program", started, 0U);
}

/*
 * Links a program's shaders.
 */
static void
timed_link_program(
	GLuint name)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_program *program;
	char log[PROGRAM_LOG];
	int status;

	/* The program. */
	context = gles_context();
	state = gles_state(context);
	program = program_get(context, name);
	if (program == NULL)
		return;

	/* The earlier link goes, then the new one. */
	program_unlink(state, program);
	log[0] = '\0';
	status = program_link(state, program, log);
	if (status != 0)
		program_unlink(state, program);

	/* The result and its log; a separable program keeps its stages' sources for program pipelines. */
	program->linked = 0;
	program->linked_separable = 0;
	if (status == 0)
		program->linked = 1;
	if (status == 0 && program->separable) {
		program->linked_separable = 1;
		program_keep_sources(program);
	}
	program_log(&program->log, log);

	/* The current program linked again starts its subroutine uniforms again. */
	if (status == 0 && state->program == program)
		gles_subroutines_reset(state, program);
}

/*
 * Makes a program current (0: none), letting a deleted one go.
 */
GL_APICALL void GL_APIENTRY
glUseProgram(
	GLuint name)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_program *program;
	struct gles_program *previous;

	/* A context with its state. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;

	/* The program, which must be linked. */
	program = NULL;
	if (name != 0U) {
		program = program_get(context, name);
		if (program == NULL)
			return;
		if (!program->linked) {
			gles_error(context, GL_INVALID_OPERATION);
			return;
		}
	}

	/* Current, its subroutine uniforms each at a compatible subroutine; the one before goes if it was deleted. */
	previous = state->program;
	state->program = program;
	if (program != NULL)
		gles_subroutines_reset(state, program);
	if (previous != NULL && previous != program && previous->delete_pending && previous->pipeline_uses == 0U) {
		gles_names_remove(&state->objects, previous->name);
		gles_program_release(state, previous);
	}
}

/*
 * Checks a program for the current state: a linked program is valid.
 */
GL_APICALL void GL_APIENTRY
glValidateProgram(
	GLuint name)
{
	struct zegl_context *context;
	struct gles_program *program;

	/* The program; its link says it all. */
	context = gles_context();
	program = program_get(context, name);
	(void)program;
}

/*
 * Reports a property of a program.
 */
GL_APICALL void GL_APIENTRY
glGetProgramiv(
	GLuint name,
	GLenum pname,
	GLint *params)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_program *program;
	unsigned index;
	GLint longest;
	GLint length;

	/* The program. */
	context = gles_context();
	program = program_get(context, name);
	if (program == NULL)
		return;

	/* The property asked for. */
	switch (pname) {
	case GL_DELETE_STATUS:
		*params = program->delete_pending;
		return;
	case GL_PROGRAM_SEPARABLE:
		/* OpenGL ES 3.1's separable programs (a context of 3.0 has no such property). */
		state = gles_state(context);
		if (gles_fixed == NULL && !state->compute) {
			gles_error(context, GL_INVALID_ENUM);
			return;
		}
		*params = program->separable;
		return;
	case GL_LINK_STATUS:
	case GL_VALIDATE_STATUS:
		*params = program->linked;
		return;
	case GL_INFO_LOG_LENGTH:
		*params = 0;
		if (program->log != NULL && program->log[0] != '\0')
			*params = (GLint)strlen(program->log) + 1;
		return;
	case GL_ATTACHED_SHADERS:
		*params = 0;
		if (program->vertex != NULL)
			*params += 1;
		if (program->fragment != NULL)
			*params += 1;
		if (program->geometry != NULL)
			*params += 1;
		if (program->compute != NULL)
			*params += 1;
		if (program->tess_control != NULL)
			*params += 1;
		if (program->tess_evaluation != NULL)
			*params += 1;
		return;
	case GL_TESS_CONTROL_OUTPUT_VERTICES:
	case GL_TESS_GEN_MODE:
	case GL_TESS_GEN_SPACING:
	case GL_TESS_GEN_VERTEX_ORDER:
	case GL_TESS_GEN_POINT_MODE:
		/* A linked program's tessellation shaders' layouts (desktop GL 4.0, libGL). */
		if (gles_fixed == NULL) {
			gles_error(context, GL_INVALID_ENUM);
			return;
		}

		/* Linked with tessellation shaders. */
		if (!program->linked || program->tess_control_module == VK_NULL_HANDLE) {
			gles_error(context, GL_INVALID_OPERATION);
			return;
		}

		/* The layout asked for. */
		*params = program->tess_vertices;
		if (pname == GL_TESS_GEN_MODE)
			*params = (GLint)program->tess_mode;
		if (pname == GL_TESS_GEN_SPACING)
			*params = (GLint)program->tess_spacing;
		if (pname == GL_TESS_GEN_VERTEX_ORDER)
			*params = (GLint)program->tess_order;
		if (pname == GL_TESS_GEN_POINT_MODE)
			*params = (GLint)program->tess_point_mode;
		return;
	case GL_GEOMETRY_VERTICES_OUT:
	case GL_GEOMETRY_INPUT_TYPE:
	case GL_GEOMETRY_OUTPUT_TYPE:
		/* A linked program's geometry shader's (desktop GL, libGL). */
		if (gles_fixed == NULL) {
			gles_error(context, GL_INVALID_ENUM);
			return;
		}

		/* Linked with a geometry shader of the application's (not one the link added after a tessellation shader). */
		if (!program->linked || !program->geometry_own) {
			gles_error(context, GL_INVALID_OPERATION);
			return;
		}

		/* The most vertices, or a primitive. */
		*params = program->geometry_vertices;
		if (pname == GL_GEOMETRY_INPUT_TYPE)
			*params = (GLint)program->geometry_input;
		if (pname == GL_GEOMETRY_OUTPUT_TYPE)
			*params = (GLint)program->geometry_output;
		return;
	case GL_COMPUTE_WORK_GROUP_SIZE:
		/* A linked compute program's (OpenGL ES 3.1, ws101-p009; not desktop GL's, libGL). */
		if (gles_fixed != NULL) {
			gles_error(context, GL_INVALID_ENUM);
			return;
		}

		/* Linked with a compute shader. */
		if (!program->linked || program->compute_pipeline == VK_NULL_HANDLE) {
			gles_error(context, GL_INVALID_OPERATION);
			return;
		}

		/* The three dimensions. */
		memcpy(params, program->local_size, sizeof(program->local_size));
		return;
	case GL_ACTIVE_ATTRIBUTES:
		*params = (GLint)program->attribute_count;
		return;
	case GL_ACTIVE_UNIFORMS:
		*params = (GLint)program->uniform_count;
		return;
	case GL_ACTIVE_UNIFORM_BLOCKS:
		*params = (GLint)program->block_count;
		return;
	case GL_TRANSFORM_FEEDBACK_VARYINGS:
		*params = (GLint)program->capture_count;
		return;
	case GL_TRANSFORM_FEEDBACK_BUFFER_MODE:
		*params = (GLint)program->capture_mode;
		if (program->capture_count == 0U)
			*params = (GLint)program->feedback_mode;
		return;
	case GL_ACTIVE_ATTRIBUTE_MAX_LENGTH:
	case GL_ACTIVE_UNIFORM_MAX_LENGTH:
	case GL_ACTIVE_UNIFORM_BLOCK_MAX_NAME_LENGTH:
	case GL_TRANSFORM_FEEDBACK_VARYING_MAX_LENGTH:
		break;
	default:
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* The longest name with its terminator (and "[0]" for arrays). */
	longest = 0;
	if (pname == GL_TRANSFORM_FEEDBACK_VARYING_MAX_LENGTH) {
		for (index = 0U; index < program->capture_count; index++) {
			length = (GLint)strlen(program->captures[index].name) + 1;
			if (length > longest)
				longest = length;
		}
	} else if (pname == GL_ACTIVE_ATTRIBUTE_MAX_LENGTH) {
		for (index = 0U; index < program->attribute_count; index++) {
			length = (GLint)strlen(program->attributes[index].name) + 1;
			if (length > longest)
				longest = length;
		}
	} else if (pname == GL_ACTIVE_UNIFORM_MAX_LENGTH) {
		for (index = 0U; index < program->uniform_count; index++) {
			length = (GLint)strlen(program->uniforms[index].name) + 4;
			if (length > longest)
				longest = length;
		}
	} else {
		for (index = 0U; index < program->block_count; index++) {
			length = (GLint)strlen(program->blocks[index].name) + 1;
			if (length > longest)
				longest = length;
		}
	}

	/* The longest. */
	*params = longest;
}

/*
 * Copies a program's link log.
 */
GL_APICALL void GL_APIENTRY
glGetProgramInfoLog(
	GLuint name,
	GLsizei bufSize,
	GLsizei *length,
	GLchar *infoLog)
{

	struct zegl_context *context;
	struct gles_program *program;

	/* The program's log (empty when none). */
	context = gles_context();
	program = program_get(context, name);
	if (program == NULL)
		return;
	program_copy_string(program->log, bufSize, length, infoLog);
}

/*
 * Reports whether a name is a program.
 */
GL_APICALL GLboolean GL_APIENTRY
glIsProgram(
	GLuint name)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_program *program;

	/* A context with its state. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return GL_FALSE;

	/* The name's object, if a program. */
	program = gles_names_get(&state->objects, name);
	if (program == NULL || program->kind != GLES_KIND_PROGRAM)
		return GL_FALSE;
	return GL_TRUE;
}

/*
 * Reports the shaders attached to a program.
 */
GL_APICALL void GL_APIENTRY
glGetAttachedShaders(
	GLuint name,
	GLsizei maxCount,
	GLsizei *count,
	GLuint *shaders)
{
	struct zegl_context *context;
	struct gles_program *program;
	GLsizei found;

	/* The program. */
	context = gles_context();
	program = program_get(context, name);
	if (program == NULL)
		return;

	/* The vertex shader, then the fragment shader, as many as fit. */
	found = 0;
	if (program->vertex != NULL && found < maxCount)
		shaders[found++] = program->vertex->name;
	if (program->fragment != NULL && found < maxCount)
		shaders[found++] = program->fragment->name;
	if (program->geometry != NULL && found < maxCount)
		shaders[found++] = program->geometry->name;
	if (program->compute != NULL && found < maxCount)
		shaders[found++] = program->compute->name;
	if (program->tess_control != NULL && found < maxCount)
		shaders[found++] = program->tess_control->name;
	if (program->tess_evaluation != NULL && found < maxCount)
		shaders[found++] = program->tess_evaluation->name;
	if (count != NULL)
		*count = found;
}

/*
 * Gives an attribute a location for the program's next link.
 */
GL_APICALL void GL_APIENTRY
glBindAttribLocation(
	GLuint name,
	GLuint index,
	const GLchar *attribute)
{
	struct zegl_context *context;
	struct gles_program *program;
	unsigned slot;
	int differs;

	/* The program and a location it can have. */
	context = gles_context();
	program = program_get(context, name);
	if (program == NULL)
		return;
	if (index >= GLES_ATTRIBS) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* The name's earlier binding, else a new one. */
	for (slot = 0U; slot < program->bound_count; slot++) {
		differs = strcmp(program->bound_names[slot], attribute);
		if (differs == 0)
			break;
	}

	/* A new binding when the name had none. */
	if (slot == program->bound_count) {
		if (program->bound_count == GLES_ATTRIBS) {
			gles_error(context, GL_OUT_OF_MEMORY);
			return;
		}

		/* One more binding. */
		program->bound_count++;
	}

	/* Recorded for the next link. */
	(void)snprintf(program->bound_names[slot], GLES_NAME, "%s", attribute);
	program->bound_locations[slot] = index;
}

/*
 * Returns the location of an active attribute, or -1.
 */
GL_APICALL GLint GL_APIENTRY
glGetAttribLocation(
	GLuint name,
	const GLchar *attribute)
{
	struct zegl_context *context;
	struct gles_program *program;
	unsigned index;
	int differs;

	/* A linked program. */
	context = gles_context();
	program = program_get(context, name);
	if (program == NULL)
		return -1;
	if (!program->linked) {
		gles_error(context, GL_INVALID_OPERATION);
		return -1;
	}

	/* The attribute of that name. */
	for (index = 0U; index < program->attribute_count; index++) {
		differs = strcmp(program->attributes[index].name, attribute);
		if (differs == 0)
			return (GLint)program->attributes[index].location;
	}

	/* None. */
	return -1;
}

/*
 * Gives a fragment shader output a draw buffer for the program's next
 * link (desktop GL 3.0, libGL).
 */
GL_APICALL void GL_APIENTRY
glBindFragDataLocation(
	GLuint name,
	GLuint color,
	const GLchar *output)
{
	/* The first colour of the draw buffer. */
	program_bind_output(name, color, 0U, output);
}

/*
 * Gives a fragment shader output a draw buffer and which colour of a
 * dual-source blend it is (index 0 or 1) for the program's next link
 * (desktop GL 3.3, libGL).
 */
GL_APICALL void GL_APIENTRY
glBindFragDataLocationIndexed(
	GLuint name,
	GLuint color,
	GLuint index,
	const GLchar *output)
{
	/* The colour given. */
	program_bind_output(name, color, index, output);
}

/*
 * Returns which colour of a dual-source blend a fragment shader output of
 * a linked program is (0 or 1), or -1 (desktop GL 3.3, libGL).
 */
GL_APICALL GLint GL_APIENTRY
glGetFragDataIndex(
	GLuint name,
	const GLchar *output)
{
	struct zegl_context *context;
	struct gles_program *program;
	unsigned index;
	int differs;

	/* A linked program. */
	context = gles_context();
	program = program_get(context, name);
	if (program == NULL)
		return -1;
	if (!program->linked) {
		gles_error(context, GL_INVALID_OPERATION);
		return -1;
	}

	/* The output of that name. */
	for (index = 0U; index < program->output_count; index++) {
		differs = strcmp(program->output_names[index], output);
		if (differs == 0)
			return program->output_indexes[index];
	}

	/* None. */
	return -1;
}

/* Records a draw buffer and an index glBindFragDataLocation(Indexed) gave an output name, for the program's next link. */
static void
program_bind_output(
	GLuint name,
	GLuint color,
	GLuint index,
	const GLchar *output)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_program *program;
	unsigned slot;
	int differs;

	/* The program, a draw buffer the device has, and the first or second colour (the second only of the draw buffers a dual-source blend has). */
	context = gles_context();
	program = program_get(context, name);
	if (program == NULL)
		return;
	state = gles_state(context);
	if (state == NULL)
		return;
	if (color >= state->color_attachments ||
	    index > 1U ||
	    (index == 1U && color != 0U)) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* A name that is not a built-in's. */
	differs = strncmp(output, "gl_", 3U);
	if (differs == 0) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* The name's earlier binding, else a new one. */
	for (slot = 0U; slot < program->frag_bound_count; slot++) {
		differs = strcmp(program->frag_bound_names[slot], output);
		if (differs == 0)
			break;
	}

	/* A new binding when the name had none. */
	if (slot == program->frag_bound_count) {
		if (program->frag_bound_count == GLES_DRAW_BUFFERS) {
			gles_error(context, GL_OUT_OF_MEMORY);
			return;
		}

		/* One more binding. */
		program->frag_bound_count++;
	}

	/* Recorded for the next link. */
	(void)snprintf(program->frag_bound_names[slot], GLES_NAME, "%s", output);
	program->frag_bound_locations[slot] = color;
	program->frag_bound_indexes[slot] = index;
}

/*
 * Returns the location of a fragment shader output of a linked program,
 * or -1.
 */
GL_APICALL GLint GL_APIENTRY
glGetFragDataLocation(
	GLuint name,
	const GLchar *output)
{
	struct zegl_context *context;
	struct gles_program *program;
	unsigned index;
	int differs;

	/* A linked program. */
	context = gles_context();
	program = program_get(context, name);
	if (program == NULL)
		return -1;
	if (!program->linked) {
		gles_error(context, GL_INVALID_OPERATION);
		return -1;
	}

	/* The output of that name. */
	for (index = 0U; index < program->output_count; index++) {
		differs = strcmp(program->output_names[index], output);
		if (differs == 0)
			return program->output_locations[index];
	}

	/* None. */
	return -1;
}

/*
 * Sets a program parameter: GL_PROGRAM_SEPARABLE (OpenGL ES 3.1), and
 * GL_PROGRAM_BINARY_RETRIEVABLE_HINT, which is taken and has no effect
 * (no binary format is offered).
 */
GL_APICALL void GL_APIENTRY
glProgramParameteri(
	GLuint name,
	GLenum pname,
	GLint value)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_program *program;

	/* A program, a parameter (whether it is separable: OpenGL ES 3.1's), and a boolean. */
	context = gles_context();
	program = program_get(context, name);
	if (program == NULL)
		return;
	state = gles_state(context);
	if (pname != GL_PROGRAM_BINARY_RETRIEVABLE_HINT &&
	    (pname != GL_PROGRAM_SEPARABLE ||
	     (gles_fixed == NULL && !state->compute))) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* A boolean. */
	if (value != GL_FALSE && value != GL_TRUE) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Succeeded: separable from the next link (the binary hint has no effect). */
	if (pname == GL_PROGRAM_SEPARABLE)
		program->separable = value;
}

/*
 * Reports a property of one of a program's interfaces (OpenGL ES 3.1
 * section 7.3.1): how many resources it lists, the longest name, or the
 * most active variables a block of it has.
 */
GL_APICALL void GL_APIENTRY
glGetProgramInterfaceiv(
	GLuint name,
	GLenum programInterface,
	GLenum pname,
	GLint *params)
{
	struct zegl_context *context;
	struct gles_program *program;
	char resource[PROGRAM_RESOURCE_NAME];
	GLint length;
	GLint variables;
	unsigned count;
	unsigned index;
	unsigned interface;

	/* A program of a context of OpenGL ES 3.1, and an interface. */
	context = gles_context();
	program = program_resource_program(context, name);
	if (program == NULL)
		return;
	interface = program_interface_bit(programInterface);
	if (interface == 0U) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* The property. */
	count = program_resource_count(program, interface);
	switch (pname) {
	case GL_ACTIVE_RESOURCES:
		*params = (GLint)count;
		return;
	case GL_MAX_NAME_LENGTH:
		/* Atomic counter buffers have no names. */
		if (interface == PROGRAM_IF_ATOMIC) {
			gles_error(context, GL_INVALID_OPERATION);
			return;
		}

		/* The longest name with its terminator (0 for an empty list). */
		*params = 0;
		for (index = 0U; index < count; index++) {
			program_resource_name(program, interface, index, resource);
			length = (GLint)strlen(resource) + 1;
			if (length > *params)
				*params = length;
		}
		return;
	case GL_MAX_NUM_ACTIVE_VARIABLES:
		/* Blocks and atomic counter buffers only. */
		if ((interface & (PROGRAM_IF_ATOMIC | PROGRAM_IF_STORAGE_BLOCK | PROGRAM_IF_UNIFORM_BLOCK)) == 0U) {
			gles_error(context, GL_INVALID_OPERATION);
			return;
		}

		/* The most of any block's. */
		*params = 0;
		for (index = 0U; index < count; index++) {
			variables = program_resource_variables(program, interface, index, NULL);
			if (variables > *params)
				*params = variables;
		}
		return;
	default:
		break;
	}

	/* Not a property. */
	gles_error(context, GL_INVALID_ENUM);
}

/*
 * Returns the index of a resource of a program's interface by its name
 * (an array's also without "[0]"), GL_INVALID_INDEX when none has it.
 */
GL_APICALL GLuint GL_APIENTRY
glGetProgramResourceIndex(
	GLuint name,
	GLenum programInterface,
	const GLchar *resourceName)
{
	struct zegl_context *context;
	struct gles_program *program;
	unsigned interface;
	unsigned index;

	/* A program of a context of OpenGL ES 3.1, and an interface with names. */
	context = gles_context();
	program = program_resource_program(context, name);
	if (program == NULL)
		return GL_INVALID_INDEX;
	interface = program_interface_bit(programInterface);
	if (interface == 0U || interface == PROGRAM_IF_ATOMIC) {
		gles_error(context, GL_INVALID_ENUM);
		return GL_INVALID_INDEX;
	}

	/* The resource of the name. */
	index = program_resource_find(program, interface, resourceName);
	if (index == UINT32_MAX)
		return GL_INVALID_INDEX;

	/* Succeeded: its index. */
	return (GLuint)index;
}

/*
 * Copies the name of a resource of a program's interface.
 */
GL_APICALL void GL_APIENTRY
glGetProgramResourceName(
	GLuint name,
	GLenum programInterface,
	GLuint index,
	GLsizei bufSize,
	GLsizei *length,
	GLchar *resourceName)
{
	struct zegl_context *context;
	struct gles_program *program;
	char resource[PROGRAM_RESOURCE_NAME];
	unsigned count;
	unsigned interface;

	/* A program of a context of OpenGL ES 3.1, an interface with names, a resource and a size. */
	context = gles_context();
	program = program_resource_program(context, name);
	if (program == NULL)
		return;
	interface = program_interface_bit(programInterface);
	if (interface == 0U || interface == PROGRAM_IF_ATOMIC) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}
	count = program_resource_count(program, interface);
	if (index >= count || bufSize < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Succeeded: the name, as much as fits. */
	program_resource_name(program, interface, index, resource);
	program_copy_string(resource, bufSize, length, resourceName);
}

/*
 * Writes properties of a resource of a program's interface, as many
 * values as fit.
 */
GL_APICALL void GL_APIENTRY
glGetProgramResourceiv(
	GLuint name,
	GLenum programInterface,
	GLuint index,
	GLsizei propCount,
	const GLenum *props,
	GLsizei bufSize,
	GLsizei *length,
	GLint *params)
{
	struct zegl_context *context;
	struct gles_program *program;
	GLint values[PROGRAM_RESOURCE_VALUES];
	GLsizei written;
	GLsizei prop;
	unsigned count;
	unsigned interface;
	unsigned taken;
	unsigned value;
	int status;

	/* A program of a context of OpenGL ES 3.1, an interface, a resource and counts. */
	context = gles_context();
	program = program_resource_program(context, name);
	if (program == NULL)
		return;
	interface = program_interface_bit(programInterface);
	if (interface == 0U) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}
	count = program_resource_count(program, interface);
	if (index >= count || propCount <= 0 || bufSize < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Every property one the interface has (checked before anything is written). */
	for (prop = 0; prop < propCount; prop++) {
		status = program_property_allowed(props[prop], interface);
		if (status < 0) {
			gles_error(context, GL_INVALID_ENUM);
			return;
		}
		if (status == 0) {
			gles_error(context, GL_INVALID_OPERATION);
			return;
		}
	}

	/* Each property's values, as many as fit. */
	written = 0;
	for (prop = 0; prop < propCount && written < bufSize; prop++) {
		taken = program_resource_property(program, interface, index, props[prop], values);
		for (value = 0U; value < taken && written < bufSize; value++) {
			params[written] = values[value];
			written++;
		}
	}

	/* Succeeded: how many were written. */
	if (length != NULL)
		*length = written;
}

/*
 * Returns the location of a variable of a program's uniforms, inputs or
 * outputs by its name (an array's element by "name[i]"), -1 when it has
 * none.
 */
GL_APICALL GLint GL_APIENTRY
glGetProgramResourceLocation(
	GLuint name,
	GLenum programInterface,
	const GLchar *resourceName)
{
	struct zegl_context *context;
	struct gles_program *program;
	GLint location;

	/* A linked program of a context of OpenGL ES 3.1. */
	context = gles_context();
	program = program_resource_program(context, name);
	if (program == NULL)
		return -1;
	if (!program->linked) {
		gles_error(context, GL_INVALID_OPERATION);
		return -1;
	}

	/* The interfaces with locations: as glGetUniformLocation, glGetAttribLocation and glGetFragDataLocation say. */
	switch (programInterface) {
	case GL_UNIFORM:
		location = glGetUniformLocation(name, resourceName);
		return location;
	case GL_PROGRAM_INPUT:
		/* Only a vertex shader's inputs have locations. */
		if ((program->linked_stages & GL_VERTEX_SHADER_BIT) == 0U)
			return -1;
		location = glGetAttribLocation(name, resourceName);
		return location;
	case GL_PROGRAM_OUTPUT:
		/* Only a fragment shader's outputs have locations. */
		if ((program->linked_stages & GL_FRAGMENT_SHADER_BIT) == 0U)
			return -1;
		location = glGetFragDataLocation(name, resourceName);
		return location;
	default:
		break;
	}

	/* Not an interface with locations. */
	gles_error(context, GL_INVALID_ENUM);
	return -1;
}

/*
 * Refuses to give a program's binary: GL_NUM_PROGRAM_BINARY_FORMATS is 0.
 */
GL_APICALL void GL_APIENTRY
glGetProgramBinary(
	GLuint name,
	GLsizei bufSize,
	GLsizei *length,
	GLenum *binaryFormat,
	void *binary)
{
	struct zegl_context *context;
	struct gles_program *program;

	/* A program; its binary has no bytes. */
	(void)bufSize;
	(void)binaryFormat;
	(void)binary;
	context = gles_context();
	program = program_get(context, name);
	if (program == NULL)
		return;
	if (length != NULL)
		*length = 0;
	gles_error(context, GL_INVALID_OPERATION);
}

/*
 * Refuses a program binary: no binary format is offered.
 */
GL_APICALL void GL_APIENTRY
glProgramBinary(
	GLuint name,
	GLenum binaryFormat,
	const void *binary,
	GLsizei length)
{
	struct zegl_context *context;
	struct gles_program *program;

	/* A program; no format is one. */
	(void)binaryFormat;
	(void)binary;
	(void)length;
	context = gles_context();
	program = program_get(context, name);
	if (program == NULL)
		return;
	gles_error(context, GL_INVALID_ENUM);
}

/*
 * Names the outputs the program's vertex shader captures at its next link
 * (transform feedback), and whether they go into one buffer interleaved
 * or each into its own.
 */
GL_APICALL void GL_APIENTRY
glTransformFeedbackVaryings(
	GLuint name,
	GLsizei count,
	const GLchar *const *varyings,
	GLenum bufferMode)
{
	struct zegl_context *context;
	struct gles_program *program;
	GLsizei index;

	/* A program, a buffer mode, and no more names than there are places for. */
	context = gles_context();
	program = program_get(context, name);
	if (program == NULL)
		return;
	if (bufferMode != GL_INTERLEAVED_ATTRIBS && bufferMode != GL_SEPARATE_ATTRIBS) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* No more names than there are places for. */
	if (count < 0 || count > (GLsizei)GLES_CAPTURES) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Separate outputs each have a binding point. */
	if (bufferMode == GL_SEPARATE_ATTRIBS && count > (GLsizei)GLES_FEEDBACK_BINDINGS) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Kept for the next link. */
	for (index = 0; index < count; index++)
		(void)snprintf(program->feedback_names[index], GLES_NAME, "%s", varyings[index]);
	program->feedback_count = (unsigned)count;
	program->feedback_mode = bufferMode;
}

/*
 * Reports one of the outputs a linked program captures.
 */
GL_APICALL void GL_APIENTRY
glGetTransformFeedbackVarying(
	GLuint name,
	GLuint index,
	GLsizei bufSize,
	GLsizei *length,
	GLsizei *size,
	GLenum *type,
	GLchar *varying)
{
	struct zegl_context *context;
	struct gles_program *program;
	const struct gles_capture *capture;
	size_t written;

	/* A linked program and one of its captured outputs. */
	context = gles_context();
	program = program_get(context, name);
	if (program == NULL)
		return;
	if (!program->linked) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* One of its captured outputs. */
	if (index >= program->capture_count) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Its size and type. */
	capture = &program->captures[index];
	*size = capture->size;
	*type = capture->type;

	/* Its name, cut to the room given. */
	written = 0U;
	if (bufSize > 0 && varying != NULL) {
		(void)snprintf(varying, (size_t)bufSize, "%s", capture->name);
		written = strlen(varying);
	}

	/* The name's length. */
	if (length != NULL)
		*length = (GLsizei)written;
}

/*
 * Reports one of a program's active attributes.
 */
GL_APICALL void GL_APIENTRY
glGetActiveAttrib(
	GLuint name,
	GLuint index,
	GLsizei bufSize,
	GLsizei *length,
	GLint *size,
	GLenum *type,
	GLchar *attribute)
{
	struct zegl_context *context;
	struct gles_program *program;

	/* The program and an attribute it has. */
	context = gles_context();
	program = program_get(context, name);
	if (program == NULL)
		return;
	if (index >= program->attribute_count) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Its size, type and name. */
	*size = program->attributes[index].size;
	*type = program->attributes[index].type;
	program_copy_string(program->attributes[index].name, bufSize, length, attribute);
}

/*
 * Reports one of a program's active uniforms.
 */
GL_APICALL void GL_APIENTRY
glGetActiveUniform(
	GLuint name,
	GLuint index,
	GLsizei bufSize,
	GLsizei *length,
	GLint *size,
	GLenum *type,
	GLchar *uniform)
{
	struct zegl_context *context;
	struct gles_program *program;
	char full[GLES_NAME + 4U];

	/* The program and a uniform it has. */
	context = gles_context();
	program = program_get(context, name);
	if (program == NULL)
		return;
	if (index >= program->uniform_count) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Its size, type and name (an array's with "[0]"). */
	*size = program->uniforms[index].size;
	*type = program->uniforms[index].type;
	(void)snprintf(full, sizeof(full), "%s", program->uniforms[index].name);
	if (program->uniforms[index].size > 1)
		(void)snprintf(full, sizeof(full), "%s[0]", program->uniforms[index].name);
	program_copy_string(full, bufSize, length, uniform);
}

/*
 * Returns the location of a uniform (or of an element: "name[i]"), or -1.
 */
GL_APICALL GLint GL_APIENTRY
glGetUniformLocation(
	GLuint name,
	const GLchar *uniform)
{
	struct zegl_context *context;
	struct gles_program *program;
	struct gles_uniform *entry;
	char base[GLES_NAME];
	const char *bracket;
	unsigned long element;
	char *end;
	size_t length;
	unsigned index;
	int differs;

	/* A linked program. */
	context = gles_context();
	program = program_get(context, name);
	if (program == NULL)
		return -1;
	if (!program->linked) {
		gles_error(context, GL_INVALID_OPERATION);
		return -1;
	}

	/* The name without a last "[i]", and i. */
	element = 0UL;
	length = strlen(uniform);
	bracket = strrchr(uniform, '[');
	if (bracket != NULL && length > 0U && uniform[length - 1U] == ']') {
		element = strtoul(bracket + 1, &end, 10);
		if (end != uniform + length - 1U)
			return -1;
		length = (size_t)(bracket - uniform);
	}

	/* A name that fits. */
	if (length >= sizeof(base))
		return -1;
	memcpy(base, uniform, length);
	base[length] = '\0';

	/* The uniform of that name (a named block's members and the built-ins have no location), and an element it has. */
	for (index = 0U; index < program->uniform_count; index++) {
		entry = &program->uniforms[index];
		if (entry->block >= 0 || entry->location < 0)
			continue;
		differs = strcmp(entry->name, base);
		if (differs != 0)
			continue;
		if (element >= (unsigned long)entry->size)
			return -1;
		return entry->location + (GLint)element;
	}

	/* None. */
	return -1;
}

/* The glUniform calls: each writes values of a size into the current program's uniform at a location. */

GL_APICALL void GL_APIENTRY
glUniform1f(
	GLint location,
	GLfloat v0)
{
	/* One float. */
	program_uniform(location, 1, 1U, &v0, 0);
}

GL_APICALL void GL_APIENTRY
glUniform2f(
	GLint location,
	GLfloat v0,
	GLfloat v1)
{
	GLfloat values[2];

	/* Two floats. */
	values[0] = v0;
	values[1] = v1;
	program_uniform(location, 1, 2U, values, 0);
}

GL_APICALL void GL_APIENTRY
glUniform3f(
	GLint location,
	GLfloat v0,
	GLfloat v1,
	GLfloat v2)
{
	GLfloat values[3];

	/* Three floats. */
	values[0] = v0;
	values[1] = v1;
	values[2] = v2;
	program_uniform(location, 1, 3U, values, 0);
}

GL_APICALL void GL_APIENTRY
glUniform4f(
	GLint location,
	GLfloat v0,
	GLfloat v1,
	GLfloat v2,
	GLfloat v3)
{
	GLfloat values[4];

	/* Four floats. */
	values[0] = v0;
	values[1] = v1;
	values[2] = v2;
	values[3] = v3;
	program_uniform(location, 1, 4U, values, 0);
}

GL_APICALL void GL_APIENTRY
glUniform1i(
	GLint location,
	GLint v0)
{
	/* One int. */
	program_uniform(location, 1, 1U, &v0, 1);
}

GL_APICALL void GL_APIENTRY
glUniform2i(
	GLint location,
	GLint v0,
	GLint v1)
{
	GLint values[2];

	/* Two ints. */
	values[0] = v0;
	values[1] = v1;
	program_uniform(location, 1, 2U, values, 1);
}

GL_APICALL void GL_APIENTRY
glUniform3i(
	GLint location,
	GLint v0,
	GLint v1,
	GLint v2)
{
	GLint values[3];

	/* Three ints. */
	values[0] = v0;
	values[1] = v1;
	values[2] = v2;
	program_uniform(location, 1, 3U, values, 1);
}

GL_APICALL void GL_APIENTRY
glUniform4i(
	GLint location,
	GLint v0,
	GLint v1,
	GLint v2,
	GLint v3)
{
	GLint values[4];

	/* Four ints. */
	values[0] = v0;
	values[1] = v1;
	values[2] = v2;
	values[3] = v3;
	program_uniform(location, 1, 4U, values, 1);
}

GL_APICALL void GL_APIENTRY
glUniform1fv(
	GLint location,
	GLsizei count,
	const GLfloat *value)
{
	/* Floats, one per element. */
	program_uniform(location, count, 1U, value, 0);
}

GL_APICALL void GL_APIENTRY
glUniform2fv(
	GLint location,
	GLsizei count,
	const GLfloat *value)
{
	/* Floats, two per element. */
	program_uniform(location, count, 2U, value, 0);
}

GL_APICALL void GL_APIENTRY
glUniform3fv(
	GLint location,
	GLsizei count,
	const GLfloat *value)
{
	/* Floats, three per element. */
	program_uniform(location, count, 3U, value, 0);
}

GL_APICALL void GL_APIENTRY
glUniform4fv(
	GLint location,
	GLsizei count,
	const GLfloat *value)
{
	/* Floats, four per element. */
	program_uniform(location, count, 4U, value, 0);
}

GL_APICALL void GL_APIENTRY
glUniform1iv(
	GLint location,
	GLsizei count,
	const GLint *value)
{
	/* Ints, one per element. */
	program_uniform(location, count, 1U, value, 1);
}

GL_APICALL void GL_APIENTRY
glUniform2iv(
	GLint location,
	GLsizei count,
	const GLint *value)
{
	/* Ints, two per element. */
	program_uniform(location, count, 2U, value, 1);
}

GL_APICALL void GL_APIENTRY
glUniform3iv(
	GLint location,
	GLsizei count,
	const GLint *value)
{
	/* Ints, three per element. */
	program_uniform(location, count, 3U, value, 1);
}

GL_APICALL void GL_APIENTRY
glUniform4iv(
	GLint location,
	GLsizei count,
	const GLint *value)
{
	/* Ints, four per element. */
	program_uniform(location, count, 4U, value, 1);
}

GL_APICALL void GL_APIENTRY
glUniform1ui(
	GLint location,
	GLuint v0)
{
	/* One unsigned int. */
	program_uniform(location, 1, 1U, &v0, 2);
}

GL_APICALL void GL_APIENTRY
glUniform2ui(
	GLint location,
	GLuint v0,
	GLuint v1)
{
	GLuint values[2];

	/* Two unsigned ints. */
	values[0] = v0;
	values[1] = v1;
	program_uniform(location, 1, 2U, values, 2);
}

GL_APICALL void GL_APIENTRY
glUniform3ui(
	GLint location,
	GLuint v0,
	GLuint v1,
	GLuint v2)
{
	GLuint values[3];

	/* Three unsigned ints. */
	values[0] = v0;
	values[1] = v1;
	values[2] = v2;
	program_uniform(location, 1, 3U, values, 2);
}

GL_APICALL void GL_APIENTRY
glUniform4ui(
	GLint location,
	GLuint v0,
	GLuint v1,
	GLuint v2,
	GLuint v3)
{
	GLuint values[4];

	/* Four unsigned ints. */
	values[0] = v0;
	values[1] = v1;
	values[2] = v2;
	values[3] = v3;
	program_uniform(location, 1, 4U, values, 2);
}

GL_APICALL void GL_APIENTRY
glUniform1uiv(
	GLint location,
	GLsizei count,
	const GLuint *value)
{
	/* Unsigned ints, one per element. */
	program_uniform(location, count, 1U, value, 2);
}

GL_APICALL void GL_APIENTRY
glUniform2uiv(
	GLint location,
	GLsizei count,
	const GLuint *value)
{
	/* Unsigned ints, two per element. */
	program_uniform(location, count, 2U, value, 2);
}

GL_APICALL void GL_APIENTRY
glUniform3uiv(
	GLint location,
	GLsizei count,
	const GLuint *value)
{
	/* Unsigned ints, three per element. */
	program_uniform(location, count, 3U, value, 2);
}

GL_APICALL void GL_APIENTRY
glUniform4uiv(
	GLint location,
	GLsizei count,
	const GLuint *value)
{
	/* Unsigned ints, four per element. */
	program_uniform(location, count, 4U, value, 2);
}

GL_APICALL void GL_APIENTRY
glUniformMatrix2fv(
	GLint location,
	GLsizei count,
	GLboolean transpose,
	const GLfloat *value)
{
	/* 2x2 matrices. */
	program_matrix(location, count, transpose, value, 0, 2U, 2U);
}

GL_APICALL void GL_APIENTRY
glUniformMatrix3fv(
	GLint location,
	GLsizei count,
	GLboolean transpose,
	const GLfloat *value)
{
	/* 3x3 matrices. */
	program_matrix(location, count, transpose, value, 0, 3U, 3U);
}

GL_APICALL void GL_APIENTRY
glUniformMatrix4fv(
	GLint location,
	GLsizei count,
	GLboolean transpose,
	const GLfloat *value)
{
	/* 4x4 matrices. */
	program_matrix(location, count, transpose, value, 0, 4U, 4U);
}

GL_APICALL void GL_APIENTRY
glUniformMatrix2x3fv(
	GLint location,
	GLsizei count,
	GLboolean transpose,
	const GLfloat *value)
{
	/* Two columns of three rows. */
	program_matrix(location, count, transpose, value, 0, 2U, 3U);
}

GL_APICALL void GL_APIENTRY
glUniformMatrix3x2fv(
	GLint location,
	GLsizei count,
	GLboolean transpose,
	const GLfloat *value)
{
	/* Three columns of two rows. */
	program_matrix(location, count, transpose, value, 0, 3U, 2U);
}

GL_APICALL void GL_APIENTRY
glUniformMatrix2x4fv(
	GLint location,
	GLsizei count,
	GLboolean transpose,
	const GLfloat *value)
{
	/* Two columns of four rows. */
	program_matrix(location, count, transpose, value, 0, 2U, 4U);
}

GL_APICALL void GL_APIENTRY
glUniformMatrix4x2fv(
	GLint location,
	GLsizei count,
	GLboolean transpose,
	const GLfloat *value)
{
	/* Four columns of two rows. */
	program_matrix(location, count, transpose, value, 0, 4U, 2U);
}

GL_APICALL void GL_APIENTRY
glUniformMatrix3x4fv(
	GLint location,
	GLsizei count,
	GLboolean transpose,
	const GLfloat *value)
{
	/* Three columns of four rows. */
	program_matrix(location, count, transpose, value, 0, 3U, 4U);
}

GL_APICALL void GL_APIENTRY
glUniformMatrix4x3fv(
	GLint location,
	GLsizei count,
	GLboolean transpose,
	const GLfloat *value)
{
	/* Four columns of three rows. */
	program_matrix(location, count, transpose, value, 0, 4U, 3U);
}

/* Desktop GL 4.0's glUniform calls of doubles (GL_ARB_gpu_shader_fp64): each writes into a double uniform only. */

GL_APICALL void GL_APIENTRY
glUniform1d(
	GLint location,
	double x)
{
	/* One double. */
	program_uniform(location, 1, 1U, &x, PROGRAM_DOUBLES);
}

GL_APICALL void GL_APIENTRY
glUniform2d(
	GLint location,
	double x,
	double y)
{
	double values[2];

	/* Two doubles. */
	values[0] = x;
	values[1] = y;
	program_uniform(location, 1, 2U, values, PROGRAM_DOUBLES);
}

GL_APICALL void GL_APIENTRY
glUniform3d(
	GLint location,
	double x,
	double y,
	double z)
{
	double values[3];

	/* Three doubles. */
	values[0] = x;
	values[1] = y;
	values[2] = z;
	program_uniform(location, 1, 3U, values, PROGRAM_DOUBLES);
}

GL_APICALL void GL_APIENTRY
glUniform4d(
	GLint location,
	double x,
	double y,
	double z,
	double w)
{
	double values[4];

	/* Four doubles. */
	values[0] = x;
	values[1] = y;
	values[2] = z;
	values[3] = w;
	program_uniform(location, 1, 4U, values, PROGRAM_DOUBLES);
}

GL_APICALL void GL_APIENTRY
glUniform1dv(
	GLint location,
	GLsizei count,
	const double *value)
{
	/* Doubles. */
	program_uniform(location, count, 1U, value, PROGRAM_DOUBLES);
}

GL_APICALL void GL_APIENTRY
glUniform2dv(
	GLint location,
	GLsizei count,
	const double *value)
{
	/* Doubles, two per element. */
	program_uniform(location, count, 2U, value, PROGRAM_DOUBLES);
}

GL_APICALL void GL_APIENTRY
glUniform3dv(
	GLint location,
	GLsizei count,
	const double *value)
{
	/* Doubles, three per element. */
	program_uniform(location, count, 3U, value, PROGRAM_DOUBLES);
}

GL_APICALL void GL_APIENTRY
glUniform4dv(
	GLint location,
	GLsizei count,
	const double *value)
{
	/* Doubles, four per element. */
	program_uniform(location, count, 4U, value, PROGRAM_DOUBLES);
}

GL_APICALL void GL_APIENTRY
glUniformMatrix2dv(
	GLint location,
	GLsizei count,
	GLboolean transpose,
	const double *value)
{
	/* 2x2 matrices of doubles. */
	program_matrix(location, count, transpose, value, 1, 2U, 2U);
}

GL_APICALL void GL_APIENTRY
glUniformMatrix3dv(
	GLint location,
	GLsizei count,
	GLboolean transpose,
	const double *value)
{
	/* 3x3 matrices of doubles. */
	program_matrix(location, count, transpose, value, 1, 3U, 3U);
}

GL_APICALL void GL_APIENTRY
glUniformMatrix4dv(
	GLint location,
	GLsizei count,
	GLboolean transpose,
	const double *value)
{
	/* 4x4 matrices of doubles. */
	program_matrix(location, count, transpose, value, 1, 4U, 4U);
}

GL_APICALL void GL_APIENTRY
glUniformMatrix2x3dv(
	GLint location,
	GLsizei count,
	GLboolean transpose,
	const double *value)
{
	/* Two columns of three rows of doubles. */
	program_matrix(location, count, transpose, value, 1, 2U, 3U);
}

GL_APICALL void GL_APIENTRY
glUniformMatrix3x2dv(
	GLint location,
	GLsizei count,
	GLboolean transpose,
	const double *value)
{
	/* Three columns of two rows of doubles. */
	program_matrix(location, count, transpose, value, 1, 3U, 2U);
}

GL_APICALL void GL_APIENTRY
glUniformMatrix2x4dv(
	GLint location,
	GLsizei count,
	GLboolean transpose,
	const double *value)
{
	/* Two columns of four rows of doubles. */
	program_matrix(location, count, transpose, value, 1, 2U, 4U);
}

GL_APICALL void GL_APIENTRY
glUniformMatrix4x2dv(
	GLint location,
	GLsizei count,
	GLboolean transpose,
	const double *value)
{
	/* Four columns of two rows of doubles. */
	program_matrix(location, count, transpose, value, 1, 4U, 2U);
}

GL_APICALL void GL_APIENTRY
glUniformMatrix3x4dv(
	GLint location,
	GLsizei count,
	GLboolean transpose,
	const double *value)
{
	/* Three columns of four rows of doubles. */
	program_matrix(location, count, transpose, value, 1, 3U, 4U);
}

GL_APICALL void GL_APIENTRY
glUniformMatrix4x3dv(
	GLint location,
	GLsizei count,
	GLboolean transpose,
	const double *value)
{
	/* Four columns of three rows of doubles. */
	program_matrix(location, count, transpose, value, 1, 4U, 3U);
}

/*
 * Reads a uniform's value of the given program as floats.
 */
GL_APICALL void GL_APIENTRY
glGetUniformfv(
	GLuint name,
	GLint location,
	GLfloat *params)
{
	/* Floats. */
	program_get_uniform(name, location, 0, params);
}

/*
 * Reads a uniform's value of the given program as ints.
 */
GL_APICALL void GL_APIENTRY
glGetUniformiv(
	GLuint name,
	GLint location,
	GLint *params)
{
	/* Ints. */
	program_get_uniform(name, location, 1, params);
}

/*
 * Reads a uniform's value of the given program as unsigned ints.
 */
GL_APICALL void GL_APIENTRY
glGetUniformuiv(
	GLuint name,
	GLint location,
	GLuint *params)
{
	/* Unsigned ints. */
	program_get_uniform(name, location, 2, params);
}

/*
 * Reads a uniform's value of the given program as doubles (desktop GL
 * 4.0, GL_ARB_gpu_shader_fp64).
 */
GL_APICALL void GL_APIENTRY
glGetUniformdv(
	GLuint name,
	GLint location,
	double *params)
{
	/* Doubles. */
	program_get_uniform(name, location, PROGRAM_DOUBLES, params);
}

/*
 * Returns the index of a program's named uniform block, or
 * GL_INVALID_INDEX.
 */
GL_APICALL GLuint GL_APIENTRY
glGetUniformBlockIndex(
	GLuint name,
	const GLchar *block)
{
	struct zegl_context *context;
	struct gles_program *program;
	unsigned index;
	int differs;

	/* The program (an unlinked one has no blocks). */
	context = gles_context();
	program = program_get(context, name);
	if (program == NULL)
		return GL_INVALID_INDEX;

	/* The block of that name. */
	for (index = 0U; index < program->block_count; index++) {
		if (program->blocks[index].stages == 0U)
			continue;
		differs = strcmp(program->blocks[index].name, block);
		if (differs == 0)
			return index;
	}

	/* None. */
	return GL_INVALID_INDEX;
}

/*
 * Reports a property of one of a program's named uniform blocks.
 */
GL_APICALL void GL_APIENTRY
glGetActiveUniformBlockiv(
	GLuint name,
	GLuint index,
	GLenum pname,
	GLint *params)
{
	struct zegl_context *context;
	struct gles_program *program;
	struct gles_block *block;
	unsigned uniform;
	unsigned found;

	/* The program and the block. */
	context = gles_context();
	program = program_get(context, name);
	if (program == NULL)
		return;
	block = program_block(context, program, index);
	if (block == NULL)
		return;

	/* The property asked for. */
	switch (pname) {
	case GL_UNIFORM_BLOCK_BINDING:
		*params = (GLint)block->buffer_binding;
		return;
	case GL_UNIFORM_BLOCK_DATA_SIZE:
		*params = (GLint)block->size;
		return;
	case GL_UNIFORM_BLOCK_NAME_LENGTH:
		*params = (GLint)strlen(block->name) + 1;
		return;
	case GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS:
		*params = (GLint)block->member_count;
		return;
	case GL_UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER:
		*params = (GLint)(block->stages & 1U);
		return;
	case GL_UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER:
		*params = (GLint)((block->stages >> 1) & 1U);
		return;
	case GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES:
		break;
	default:
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* The indices of its members among the program's uniforms (an array's elements share the first element's). */
	found = 0U;
	for (uniform = 0U; uniform < program->uniform_count; uniform++) {
		if (program->uniforms[uniform].block != (GLint)block->members)
			continue;
		params[found] = (GLint)uniform;
		found++;
	}
}

/*
 * Copies the name of one of a program's named uniform blocks.
 */
GL_APICALL void GL_APIENTRY
glGetActiveUniformBlockName(
	GLuint name,
	GLuint index,
	GLsizei bufSize,
	GLsizei *length,
	GLchar *uniformBlockName)
{
	struct zegl_context *context;
	struct gles_program *program;
	struct gles_block *block;

	/* The program and the block. */
	context = gles_context();
	program = program_get(context, name);
	if (program == NULL)
		return;
	block = program_block(context, program, index);
	if (block == NULL)
		return;

	/* Its name. */
	program_copy_string(block->name, bufSize, length, uniformBlockName);
}

/*
 * Makes one of a program's named uniform blocks read the buffer bound to
 * an indexed uniform buffer binding point.
 */
GL_APICALL void GL_APIENTRY
glUniformBlockBinding(
	GLuint name,
	GLuint index,
	GLuint binding)
{
	struct zegl_context *context;
	struct gles_program *program;
	struct gles_block *block;

	/* The program and the block. */
	context = gles_context();
	program = program_get(context, name);
	if (program == NULL)
		return;
	block = program_block(context, program, index);
	if (block == NULL)
		return;

	/* A binding point there is. */
	if (binding >= GLES_UNIFORM_BINDINGS) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* The next draws read that binding point's buffer. */
	block->buffer_binding = binding;
}

/*
 * Finds the indices of a program's active uniforms by name
 * (GL_INVALID_INDEX for a name that is not one).
 */
GL_APICALL void GL_APIENTRY
glGetUniformIndices(
	GLuint name,
	GLsizei count,
	const GLchar *const *names,
	GLuint *indices)
{
	struct zegl_context *context;
	struct gles_program *program;
	const char *wanted;
	size_t length;
	size_t own;
	unsigned index;
	GLsizei which;
	int differs;

	/* The program, and a count. */
	context = gles_context();
	program = program_get(context, name);
	if (program == NULL)
		return;
	if (count < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Each name: a uniform's own, or an array's with "[0]". */
	for (which = 0; which < count; which++) {
		wanted = names[which];
		length = strlen(wanted);
		indices[which] = GL_INVALID_INDEX;
		for (index = 0U; index < program->uniform_count; index++) {
			own = strlen(program->uniforms[index].name);
			differs = strncmp(program->uniforms[index].name, wanted, own);
			if (differs != 0)
				continue;

			/* The whole name, or the array's with "[0]". */
			if (length == own) {
				indices[which] = index;
				break;
			}

			/* An array's first element names the array. */
			differs = strcmp(wanted + own, "[0]");
			if (differs == 0 && program->uniforms[index].size > 1) {
				indices[which] = index;
				break;
			}
		}
	}
}

/*
 * Reports a property of each of a list of a program's active uniforms.
 */
GL_APICALL void GL_APIENTRY
glGetActiveUniformsiv(
	GLuint name,
	GLsizei count,
	const GLuint *indices,
	GLenum pname,
	GLint *params)
{
	struct zegl_context *context;
	struct gles_program *program;
	GLint value;
	GLsizei which;
	int status;

	/* The program, a count, and indices of its uniforms. */
	context = gles_context();
	program = program_get(context, name);
	if (program == NULL)
		return;
	if (count < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Every index must be one of the program's uniforms. */
	for (which = 0; which < count; which++) {
		if (indices[which] >= program->uniform_count) {
			gles_error(context, GL_INVALID_VALUE);
			return;
		}
	}

	/* Each uniform's property. */
	for (which = 0; which < count; which++) {
		status = program_uniform_property(&program->uniforms[indices[which]], pname, &value);
		if (status != 0) {
			gles_error(context, GL_INVALID_ENUM);
			return;
		}

		/* The value. */
		params[which] = value;
	}
}

/*
 * Returns the location of a subroutine uniform of a program's stage by
 * its name, or of an element of an array of them ("NAME[i]"); -1 for a
 * name the stage does not have (desktop GL 4.0).
 */
GL_APICALL GLint GL_APIENTRY
glGetSubroutineUniformLocation(
	GLuint program,
	GLenum shadertype,
	const GLchar *name)
{
	const struct gles_subroutines *subroutines;
	const struct gles_subroutine_uniform *uniform;
	const char *rest;
	char *end;
	unsigned long element;
	unsigned index;
	size_t length;
	int differs;

	/* The stage's tables. */
	subroutines = program_stage_of(gles_context(), program, shadertype);
	if (subroutines == NULL || name == NULL)
		return -1;

	/* Each uniform: its name, or its name with an element's index. */
	for (index = 0U; index < subroutines->uniform_count; index++) {
		uniform = &subroutines->uniforms[index];
		length = strlen(uniform->name);
		differs = strncmp(name, uniform->name, length);
		if (differs != 0)
			continue;
		rest = name + length;
		if (*rest == '\0')
			return (GLint)uniform->location;
		if (*rest != '[' || rest[1] < '0' || rest[1] > '9')
			continue;

		/* "[i]" of an element there is. */
		element = strtoul(rest + 1, &end, 10);
		if (end[0] != ']' || end[1] != '\0' || element >= uniform->size)
			continue;
		return (GLint)(uniform->location + (unsigned)element);
	}

	/* No such uniform. */
	return -1;
}

/*
 * Returns the index of a subroutine of a program's stage by its name;
 * GL_INVALID_INDEX for a name the stage does not have (desktop GL 4.0).
 */
GL_APICALL GLuint GL_APIENTRY
glGetSubroutineIndex(
	GLuint program,
	GLenum shadertype,
	const GLchar *name)
{
	const struct gles_subroutines *subroutines;
	unsigned index;
	int differs;

	/* The stage's tables. */
	subroutines = program_stage_of(gles_context(), program, shadertype);
	if (subroutines == NULL || name == NULL)
		return GL_INVALID_INDEX;

	/* The subroutine of the name. */
	for (index = 0U; index < subroutines->count; index++) {
		differs = strcmp(subroutines->names[index], name);
		if (differs == 0)
			return (GLuint)index;
	}

	/* No such subroutine. */
	return GL_INVALID_INDEX;
}

/*
 * Reports a property of a subroutine uniform of a program's stage: how
 * many subroutines are compatible with it and their indexes, its size,
 * and the length of its name with the terminator (desktop GL 4.0).
 */
GL_APICALL void GL_APIENTRY
glGetActiveSubroutineUniformiv(
	GLuint program,
	GLenum shadertype,
	GLuint index,
	GLenum pname,
	GLint *values)
{
	struct zegl_context *context;
	const struct gles_subroutines *subroutines;
	const struct gles_subroutine_uniform *uniform;
	unsigned which;

	/* The stage's tables, and a uniform of them. */
	context = gles_context();
	subroutines = program_stage_of(context, program, shadertype);
	if (subroutines == NULL)
		return;
	if (index >= subroutines->uniform_count) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}
	uniform = &subroutines->uniforms[index];

	/* The property. */
	switch (pname) {
	case GL_NUM_COMPATIBLE_SUBROUTINES:
		values[0] = (GLint)uniform->compatible_count;
		break;
	case GL_COMPATIBLE_SUBROUTINES:
		for (which = 0U; which < uniform->compatible_count; which++)
			values[which] = (GLint)uniform->compatible[which];
		break;
	case GL_UNIFORM_SIZE:
		values[0] = (GLint)uniform->size;
		break;
	case GL_UNIFORM_NAME_LENGTH:
		values[0] = (GLint)strlen(uniform->name) + 1;
		break;
	default:
		gles_error(context, GL_INVALID_ENUM);
		break;
	}
}

/*
 * Gives the name of a subroutine uniform of a program's stage (desktop GL
 * 4.0).
 */
GL_APICALL void GL_APIENTRY
glGetActiveSubroutineUniformName(
	GLuint program,
	GLenum shadertype,
	GLuint index,
	GLsizei bufSize,
	GLsizei *length,
	GLchar *name)
{
	struct zegl_context *context;
	const struct gles_subroutines *subroutines;

	/* The stage's tables, a uniform of them, and a size. */
	context = gles_context();
	subroutines = program_stage_of(context, program, shadertype);
	if (subroutines == NULL)
		return;
	if (index >= subroutines->uniform_count || bufSize < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Succeeded: the name, as much as fits. */
	program_copy_string(subroutines->uniforms[index].name, bufSize, length, name);
}

/*
 * Gives the name of a subroutine of a program's stage by its index
 * (desktop GL 4.0).
 */
GL_APICALL void GL_APIENTRY
glGetActiveSubroutineName(
	GLuint program,
	GLenum shadertype,
	GLuint index,
	GLsizei bufSize,
	GLsizei *length,
	GLchar *name)
{
	struct zegl_context *context;
	const struct gles_subroutines *subroutines;

	/* The stage's tables, a subroutine of them, and a size. */
	context = gles_context();
	subroutines = program_stage_of(context, program, shadertype);
	if (subroutines == NULL)
		return;
	if (index >= subroutines->count || bufSize < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Succeeded: the name, as much as fits. */
	program_copy_string(subroutines->names[index], bufSize, length, name);
}

/*
 * Reports a property of a program's stage: how many subroutine uniforms
 * and locations of them, how many subroutines, and the longest names of
 * each with the terminator (desktop GL 4.0).
 */
GL_APICALL void GL_APIENTRY
glGetProgramStageiv(
	GLuint program,
	GLenum shadertype,
	GLenum pname,
	GLint *values)
{
	struct zegl_context *context;
	const struct gles_subroutines *subroutines;

	/* The stage's tables. */
	context = gles_context();
	subroutines = program_stage_of(context, program, shadertype);
	if (subroutines == NULL)
		return;

	/* The property. */
	switch (pname) {
	case GL_ACTIVE_SUBROUTINE_UNIFORMS:
		values[0] = (GLint)subroutines->uniform_count;
		break;
	case GL_ACTIVE_SUBROUTINE_UNIFORM_LOCATIONS:
		values[0] = (GLint)subroutines->locations;
		break;
	case GL_ACTIVE_SUBROUTINES:
		values[0] = (GLint)subroutines->count;
		break;
	case GL_ACTIVE_SUBROUTINE_UNIFORM_MAX_LENGTH:
		values[0] = program_subroutine_max_length(subroutines, 1);
		break;
	case GL_ACTIVE_SUBROUTINE_MAX_LENGTH:
		values[0] = program_subroutine_max_length(subroutines, 0);
		break;
	default:
		gles_error(context, GL_INVALID_ENUM);
		break;
	}
}

/*
 * Chooses the subroutine of every subroutine uniform location of a stage
 * of the current program: count is the stage's locations, and each index
 * a subroutine compatible with its location's uniform (desktop GL 4.0).
 */
GL_APICALL void GL_APIENTRY
glUniformSubroutinesuiv(
	GLenum shadertype,
	GLsizei count,
	const GLuint *indices)
{
	static const struct gles_subroutines none;
	struct zegl_context *context;
	struct gles_state *state;
	const struct gles_subroutines *subroutines;
	const struct gles_subroutine_uniform *uniform;
	unsigned stage;
	unsigned location;
	unsigned which;
	int compatible;
	int status;

	/* A context with its state, a shader type, a current program. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	status = program_subroutine_stage(context, shadertype, &stage);
	if (status != 0)
		return;
	if (state->program == NULL) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}
	subroutines = state->program->subroutines[stage];
	if (subroutines == NULL)
		subroutines = &none;

	/* Every location of the stage. */
	if (count < 0 || (unsigned)count != subroutines->locations) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Each index a subroutine compatible with its location's uniform. */
	for (location = 0U; location < subroutines->locations; location++) {
		uniform = program_subroutine_at(subroutines, location);
		compatible = 0;
		for (which = 0U; uniform != NULL && which < uniform->compatible_count; which++) {
			if (uniform->compatible[which] == indices[location])
				compatible = 1;
		}
		if (!compatible) {
			gles_error(context, GL_INVALID_VALUE);
			return;
		}
	}

	/* Succeeded: the indexes chosen. */
	for (location = 0U; location < subroutines->locations; location++)
		state->subroutine_indices[stage][location] = indices[location];
}

/*
 * Reports the subroutine a subroutine uniform location of a stage of the
 * current program selects (desktop GL 4.0).
 */
GL_APICALL void GL_APIENTRY
glGetUniformSubroutineuiv(
	GLenum shadertype,
	GLint location,
	GLuint *params)
{
	struct zegl_context *context;
	struct gles_state *state;
	const struct gles_subroutines *subroutines;
	unsigned stage;
	int status;

	/* A context with its state, a shader type, a current program. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	status = program_subroutine_stage(context, shadertype, &stage);
	if (status != 0)
		return;
	if (state->program == NULL) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* A location of the stage. */
	subroutines = state->program->subroutines[stage];
	if (subroutines == NULL || location < 0 || (unsigned)location >= subroutines->locations) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Succeeded: the subroutine it selects. */
	*params = state->subroutine_indices[stage][location];
}

/* Returns the shader of a name, recording the error when the name is not one. */
static struct gles_shader *
program_shader(
	struct zegl_context *context,
	GLuint name)
{
	struct gles_state *state;
	struct gles_shader *shader;

	/* A context with its state. */
	state = gles_state(context);
	if (state == NULL)
		return NULL;

	/* The name's object: none is GL_INVALID_VALUE, a program GL_INVALID_OPERATION. */
	shader = gles_names_get(&state->objects, name);
	if (shader == NULL) {
		gles_error(context, GL_INVALID_VALUE);
		return NULL;
	}

	/* A shader, not a program. */
	if (shader->kind != GLES_KIND_SHADER) {
		gles_error(context, GL_INVALID_OPERATION);
		return NULL;
	}

	/* Succeeded: the shader. */
	return shader;
}

/* Returns the program of a name, recording the error when the name is not one. */
static struct gles_program *
program_get(
	struct zegl_context *context,
	GLuint name)
{
	struct gles_state *state;
	struct gles_program *program;

	/* A context with its state. */
	state = gles_state(context);
	if (state == NULL)
		return NULL;

	/* The name's object: none is GL_INVALID_VALUE, a shader GL_INVALID_OPERATION. */
	program = gles_names_get(&state->objects, name);
	if (program == NULL) {
		gles_error(context, GL_INVALID_VALUE);
		return NULL;
	}

	/* A program, not a shader. */
	if (program->kind != GLES_KIND_PROGRAM) {
		gles_error(context, GL_INVALID_OPERATION);
		return NULL;
	}

	/* Succeeded: the program. */
	return program;
}

/*
 * Links a program: its shaders' SPIR-V (made by the GLSL compiler's link
 * when both are GLSL, or given as binaries), then the interfaces, the
 * uniforms and the Vulkan objects.  Returns 0, or -1 with the log.
 */
static int
program_link(
	struct gles_state *state,
	struct gles_program *program,
	char *log)
{
	struct glsl_program linked;
	unsigned input;
	unsigned output;
	unsigned vertices;
	unsigned mode;
	unsigned spacing;
	unsigned order;
	unsigned point_mode;
	int status;

	/*
	 * A separable program of a vertex or a fragment shader alone (OpenGL
	 * ES 3.1) is linked with a stand-in for the other stage, which no
	 * pipeline draws with.
	 */
	if (program->separable &&
	    program->compute == NULL &&
	    program->geometry == NULL &&
	    program->tess_control == NULL &&
	    program->tess_evaluation == NULL &&
	    (program->vertex == NULL) != (program->fragment == NULL)) {
		status = program_link_alone(state, program, log);
		return status;
	}

	/* The stages the link has: a compute shader alone, or the vertex and fragment shaders (and those between them). */
	if (program->linked_stages == 0U)
		program->linked_stages = GL_VERTEX_SHADER_BIT | GL_FRAGMENT_SHADER_BIT;
	if (program->compute != NULL)
		program->linked_stages = GL_COMPUTE_SHADER_BIT;

	/* A compute shader is linked alone (ws101-p009). */
	if (program->compute != NULL) {
		if (program->vertex != NULL ||
		    program->fragment != NULL ||
		    program->geometry != NULL ||
		    program->tess_control != NULL ||
		    program->tess_evaluation != NULL) {
			(void)snprintf(log, PROGRAM_LOG, "a compute shader cannot be linked with shaders of other stages\n");
			return -1;
		}

		/* Its link. */
		status = program_link_compute(state, program, log);
		return status;
	}

	/* Two compiled shaders. */
	if (program->vertex == NULL || program->fragment == NULL) {
		(void)snprintf(log, PROGRAM_LOG, "a program needs a vertex and a fragment shader\n");
		return -1;
	}

	/* Both compiled, and a geometry shader too (GLSL only). */
	if (!program->vertex->compiled || !program->fragment->compiled) {
		(void)snprintf(log, PROGRAM_LOG, "a shader of the program is not compiled\n");
		return -1;
	}

	/* A geometry shader compiled from GLSL. */
	if (program->geometry != NULL && (!program->geometry->compiled || program->geometry->glsl == NULL)) {
		(void)snprintf(log, PROGRAM_LOG, "the geometry shader is not compiled GLSL\n");
		return -1;
	}

	/* Tessellation shaders compiled from GLSL (the compiler's link checks that both are there). */
	if ((program->tess_control != NULL && (!program->tess_control->compiled || program->tess_control->glsl == NULL)) ||
	    (program->tess_evaluation != NULL && (!program->tess_evaluation->compiled || program->tess_evaluation->glsl == NULL))) {
		(void)snprintf(log, PROGRAM_LOG, "a tessellation shader is not compiled GLSL\n");
		return -1;
	}

	/* Two binaries are linked as they are. */
	if (program->vertex->glsl == NULL &&
	    program->fragment->glsl == NULL &&
	    program->geometry == NULL &&
	    program->tess_control == NULL &&
	    program->tess_evaluation == NULL) {
		status = program_link_code(state, program, program->vertex->code, program->vertex->words, program->fragment->code,
					   program->fragment->words, NULL, 0U, NULL, log);
		return status;
	}

	/* GLSL is made SPIR-V by the compiler's link first, with what its vertex shader captures. */
	status = program_link_glsl(program, &linked, log);
	if (status != 0)
		return -1;

	/* What the vertex shader captures, and where each name goes. */
	status = program_glsl_captures(program, &linked, log);
	if (status != 0) {
		glsl_program_free(&linked);
		return -1;
	}

	/* Each stage's subroutines and subroutine uniforms (desktop GL 4.0). */
	status = program_link_subroutines(program, log);
	if (status != 0) {
		glsl_program_free(&linked);
		return -1;
	}

	/* A geometry shader's primitives and most vertices, as GL names them. */
	program->geometry_input = GL_NONE;
	program->geometry_output = GL_NONE;
	program->geometry_vertices = 0;
	program->geometry_own = 0;
	if (program->geometry != NULL) {
		glsl_geometry_layout(program->geometry->glsl, &input, &output, &vertices);
		program->geometry_own = 1;
		program->geometry_input = input;
		program->geometry_output = output;
		program->geometry_vertices = (GLint)vertices;
	}

	/* The tessellation shaders' layouts, as GL names them. */
	program->tess_vertices = 0;
	program->tess_mode = GL_NONE;
	program->tess_spacing = GL_NONE;
	program->tess_order = GL_NONE;
	program->tess_point_mode = GL_FALSE;
	if (program->tess_control != NULL && program->tess_evaluation != NULL) {
		glsl_tess_layout(program->tess_control->glsl, &vertices, &mode, &spacing, &order, &point_mode);
		program->tess_vertices = (GLint)vertices;
		glsl_tess_layout(program->tess_evaluation->glsl, &vertices, &mode, &spacing, &order, &point_mode);
		program->tess_mode = mode;
		program->tess_spacing = spacing;
		program->tess_order = order;
		program->tess_point_mode = (GLboolean)point_mode;
	}

	/* The SPIR-V linked, then the uniforms' GL types the SPIR-V does not tell, then the named blocks' members. */
	status = program_link_code(state, program, linked.code[0], linked.words[0], linked.code[1], linked.words[1],
				   linked.code[GLSL_STAGE_GEOMETRY], linked.words[GLSL_STAGE_GEOMETRY], &linked, log);
	if (status == 0)
		program_glsl_types(program, &linked);
	if (status == 0)
		status = program_glsl_blocks(program, &linked, log);
	glsl_program_free(&linked);

	/* Reports why the link failed. */
	if (status != 0)
		return -1;

	/* Succeeded: the program is linked. */
	return 0;
}

/*
 * Links both shaders' GLSL into SPIR-V, with the attribute locations
 * glBindAttribLocation gave.  Returns 0, or -1 with the log.
 */
static int
program_link_glsl(
	struct gles_program *program,
	struct glsl_program *linked,
	char *log)
{
	struct glsl_binding bindings[GLES_ATTRIBS];
	const char *captures[GLES_CAPTURES];
	const struct glsl_shader *geometry;
	const struct glsl_shader *tess_control;
	const struct glsl_shader *tess_evaluation;
	unsigned capture_count;
	unsigned skipped;
	unsigned index;
	char *glsl_log;
	int special;
	int status;

	/* Both shaders GLSL (a GLSL shader does not link with a SPIR-V binary). */
	if (program->vertex->glsl == NULL || program->fragment->glsl == NULL) {
		(void)snprintf(log, PROGRAM_LOG, "a GLSL shader cannot be linked with a SPIR-V binary\n");
		return -1;
	}

	/* The bound attribute locations. */
	for (index = 0U; index < program->bound_count; index++) {
		bindings[index].name = program->bound_names[index];
		bindings[index].location = program->bound_locations[index];
	}

	/* The outputs transform feedback captures, by name (desktop GL's gl_NextBuffer and gl_SkipComponents are not outputs). */
	capture_count = 0U;
	for (index = 0U; index < program->feedback_count; index++) {
		special = program_capture_special(program->feedback_names[index], &skipped);
		if (special)
			continue;
		captures[capture_count] = program->feedback_names[index];
		capture_count++;
	}

	/* The compiler's link (with the tessellation and geometry shaders between the two when there are some). */
	geometry = NULL;
	if (program->geometry != NULL)
		geometry = program->geometry->glsl;
	tess_control = NULL;
	if (program->tess_control != NULL)
		tess_control = program->tess_control->glsl;
	tess_evaluation = NULL;
	if (program->tess_evaluation != NULL)
		tess_evaluation = program->tess_evaluation->glsl;
	status = glsl_link_pipeline(program->vertex->glsl, tess_control, tess_evaluation, geometry, program->fragment->glsl,
				    bindings, program->bound_count, captures, capture_count, linked, &glsl_log);
	if (status != 0) {
		if (glsl_log != NULL) {
			(void)snprintf(log, PROGRAM_LOG, "%s", glsl_log);
		} else {
			(void)snprintf(log, PROGRAM_LOG, "the GLSL link ran out of memory\n");
		}

		/* The compiler's log is copied. */
		free(glsl_log);
		return -1;
	}

	/* Succeeded: the SPIR-V of both stages. */
	return 0;
}

/*
 * Links a compute shader alone (ws101-p009): the GLSL compiler's link,
 * the workgroup size and the shader storage blocks, the uniforms and the
 * named blocks, the layouts and the compute pipeline.  Returns 0, or -1
 * with the log.
 */
static int
program_link_compute(
	struct gles_state *state,
	struct gles_program *program,
	char *log)
{
	struct glsl_program linked;
	struct gles_spirv spirv;
	char *glsl_log;
	unsigned index;
	int status;

	/* Compiled GLSL (a SPIR-V binary of a compute shader is not taken). */
	if (!program->compute->compiled || program->compute->glsl == NULL) {
		(void)snprintf(log, PROGRAM_LOG, "the compute shader is not compiled GLSL\n");
		return -1;
	}

	/* The compiler's link. */
	status = glsl_link_compute(program->compute->glsl, &linked, &glsl_log);
	if (status != 0) {
		if (glsl_log != NULL) {
			(void)snprintf(log, PROGRAM_LOG, "%s", glsl_log);
		} else {
			(void)snprintf(log, PROGRAM_LOG, "the GLSL link ran out of memory\n");
		}

		/* The compiler's log is copied. */
		free(glsl_log);
		return -1;
	}

	/* The workgroup size, and the shader storage blocks at the binding points there are. */
	for (index = 0U; index < 3U; index++)
		program->local_size[index] = (GLint)linked.local_size[index];
	status = program_compute_storages(state, program, &linked, log);

	/* The uniforms and the named blocks the SPIR-V has, which must be a compute shader's. */
	if (status == 0) {
		status = gles_spirv_reflect(linked.code[GLSL_STAGE_COMPUTE], linked.words[GLSL_STAGE_COMPUTE], &spirv, log, PROGRAM_LOG);
		if (status == 0) {
			if (spirv.model != PROGRAM_MODEL_COMPUTE) {
				(void)snprintf(log, PROGRAM_LOG, "the SPIR-V stage is not a compute shader\n");
				status = -1;
			}

			/* The named blocks at their bindings, and the uniforms. */
			if (status == 0)
				status = program_spirv_blocks(program, &spirv, GLSL_STAGE_COMPUTE, log);
			if (status == 0)
				status = program_merge(program, &spirv, log);
			gles_spirv_free(&spirv);
		}
	}

	/* The uniforms' GL types the SPIR-V does not tell, then the named blocks' members, then the image uniforms. */
	if (status == 0) {
		program_glsl_types(program, &linked);
		status = program_glsl_blocks(program, &linked, log);
	}
	if (status == 0)
		status = program_compute_images(state, program, &linked, log);

	/* The layouts, read by the compute stage. */
	if (status == 0)
		status = program_multisample_refused(state, program, log);
	if (status == 0) {
		program->stages = VK_SHADER_STAGE_COMPUTE_BIT;
		status = program_layout(state, program);
		if (status != 0)
			(void)snprintf(log, PROGRAM_LOG, "the device refused the program's layout\n");
	}

	/* The pipeline. */
	if (status == 0)
		status = program_compute_pipeline(state, program, linked.code[GLSL_STAGE_COMPUTE], linked.words[GLSL_STAGE_COMPUTE], log);
	glsl_program_free(&linked);

	/* Reports why the link failed. */
	if (status != 0) {
		if (log[0] == '\0')
			(void)snprintf(log, PROGRAM_LOG, "out of memory\n");
		return -1;
	}

	/* Succeeded: a serial of its own. */
	program->serial = program_serial++;
	return 0;
}

/*
 * Refuses a program with a multisample sampler of a kind the device samples
 * no multisample image of (its most samples a texture of the kind has is
 * 1): Vulkan reads such a sampler only from an image of several samples.
 * A float one reads colours and depth (GL_MAX_COLOR_TEXTURE_SAMPLES,
 * GL_MAX_DEPTH_TEXTURE_SAMPLES), an integer one integers
 * (GL_MAX_INTEGER_SAMPLES).  Returns 0, or -1 with the log.
 */
static int
program_multisample_refused(
	struct gles_state *state,
	const struct gles_program *program,
	char *log)
{
	GLenum type;
	unsigned index;
	uint32_t color_most;
	uint32_t depth_most;
	uint32_t integer_most;
	int floats;
	int integers;

	/* The most samples of the device's multisample textures of each kind. */
	color_most = gles_color_texture_samples_max(state);
	depth_most = gles_depth_texture_samples_max(state);
	integer_most = gles_integer_samples_max(state);

	/* The kinds it samples several samples of: colours or depth (float samplers), integers. */
	floats = 0;
	if (color_most >= 2U || depth_most >= 2U)
		floats = 1;
	integers = 0;
	if (integer_most >= 2U)
		integers = 1;

	/* A multisample sampler of another kind is refused. */
	for (index = 0U; index < program->uniform_count; index++) {
		type = program->uniforms[index].type;
		if (!program->uniforms[index].sampler)
			continue;
		if ((type == GL_SAMPLER_2D_MULTISAMPLE &&
		     !floats) ||
		    ((type == GL_INT_SAMPLER_2D_MULTISAMPLE ||
		      type == GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE) &&
		     !integers)) {
			(void)snprintf(log, PROGRAM_LOG, "sampler %s reads a multisample texture, which the device does not sample\n",
				       program->uniforms[index].name);
			return -1;
		}
	}

	/* Succeeded: no such sampler. */
	return 0;
}

/*
 * Keeps a linked compute program's image uniforms: each image (an array's
 * elements, one an image unit) at a unit the context has, of a format the
 * device stores (and runs atomics on, when the shader does), and each
 * uniform among the uniforms with its GL type and its units as its
 * values.  Returns 0, or -1 with the log.
 */
static int
program_compute_images(
	struct gles_state *state,
	struct gles_program *program,
	const struct glsl_program *linked,
	char *log)
{
	static const GLenum types[3][4] = {
		{ GL_IMAGE_2D, GL_IMAGE_3D, GL_IMAGE_CUBE, GL_IMAGE_2D_ARRAY },
		{ GL_INT_IMAGE_2D, GL_INT_IMAGE_3D, GL_INT_IMAGE_CUBE, GL_INT_IMAGE_2D_ARRAY },
		{ GL_UNSIGNED_INT_IMAGE_2D, GL_UNSIGNED_INT_IMAGE_3D, GL_UNSIGNED_INT_IMAGE_CUBE, GL_UNSIGNED_INT_IMAGE_2D_ARRAY }
	};
	const struct glsl_image_info *info;
	struct gles_uniform *uniforms;
	struct gles_location *locations;
	struct gles_uniform *uniform;
	struct gles_image *image;
	VkFormat format;
	GLenum gl_format;
	uint32_t features;
	uint32_t wanted;
	unsigned images;
	unsigned shape;
	unsigned kind;
	unsigned base;
	unsigned index;
	unsigned element;
	int status;

	/* No more images (an array's elements each) than the context has units. */
	program->image_count = 0U;
	if (linked->image_count == 0U)
		return 0;
	images = 0U;
	for (index = 0U; index < linked->image_count; index++)
		images += linked->images[index].count;
	if (images > state->image_units || images > GLES_IMAGE_UNITS) {
		(void)snprintf(log, PROGRAM_LOG, "the compute shader has %u images, more than the %u image units\n", images,
			       state->image_units);
		return -1;
	}

	/* Room among the uniforms and the locations of their elements. */
	uniforms = realloc(program->uniforms, (program->uniform_count + linked->image_count) * sizeof(*uniforms));
	if (uniforms == NULL)
		return -1;
	program->uniforms = uniforms;
	locations = realloc(program->locations, (program->location_count + images) * sizeof(*locations));
	if (locations == NULL)
		return -1;
	program->locations = locations;

	/* Each image uniform: units there are, a format the device stores (and runs atomics on, when the shader does). */
	for (index = 0U; index < linked->image_count; index++) {
		info = &linked->images[index];
		status = gles_image_format(info->format, &format, &gl_format);
		if (status != 0 || info->count == 0U || info->unit + info->count > state->image_units) {
			(void)snprintf(log, PROGRAM_LOG, "image %s is at unit %u, past the last unit %u\n", info->name,
				       info->unit + info->count - 1U, state->image_units - 1U);
			return -1;
		}
		features = gles_image_features(state, format);
		wanted = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
		if (info->atomic)
			wanted |= VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT;
		if ((features & wanted) != wanted) {
			(void)snprintf(log, PROGRAM_LOG, "the device does not store%s images of image %s's format\n",
				       info->atomic ? " (with atomics)" : "", info->name);
			return -1;
		}

		/* Its shape. */
		shape = GLES_SHAPE_2D;
		kind = 0U;
		if (info->dimension == GLSL_SAMPLER_3D) {
			shape = GLES_SHAPE_3D;
			kind = 1U;
		} else if (info->dimension == GLSL_SAMPLER_CUBE) {
			shape = GLES_SHAPE_CUBE;
			kind = 2U;
		} else if (info->arrayed) {
			shape = GLES_SHAPE_ARRAY;
			kind = 3U;
		}

		/* The uniform: its GL type, its elements, a unit each for their values, from its location on. */
		base = info->base;
		if (base > 2U)
			base = 0U;
		uniform = &program->uniforms[program->uniform_count];
		memset(uniform, 0, sizeof(*uniform));
		(void)snprintf(uniform->name, sizeof(uniform->name), "%s", info->name);
		uniform->type = types[base][kind];
		uniform->size = (GLint)info->count;
		uniform->components = 1U;
		uniform->columns = 1U;
		uniform->image = 1;
		uniform->binding = GLES_IMAGE_FIRST_BINDING + info->unit;
		uniform->block = -1;
		uniform->location = (GLint)program->location_count;

		/* Each element: an image on its unit, the binding's descriptor of its index, with a location. */
		for (element = 0U; element < info->count; element++) {
			image = &program->images[program->image_count];
			image->unit = info->unit + element;
			image->binding = GLES_IMAGE_FIRST_BINDING + info->unit;
			image->element = element;
			image->count = 0U;
			if (element == 0U)
				image->count = info->count;
			image->format = format;
			image->gl_format = gl_format;
			image->shape = shape;
			image->writes = !info->readonly;
			image->uniform = program->uniform_count;
			program->image_count++;

			/* Its value and location. */
			uniform->units[element] = (GLint)image->unit;
			program->locations[program->location_count].uniform = program->uniform_count;
			program->locations[program->location_count].element = element;
			program->location_count++;
		}
		program->uniform_count++;
	}

	/* Succeeded: the images are kept. */
	return 0;
}

/*
 * Keeps a linked compute program's shader storage blocks: each at a
 * binding point a context has, no more than a stage of the device reads.
 * Returns 0, or -1 with the log.
 */
static int
program_compute_storages(
	struct gles_state *state,
	struct gles_program *program,
	const struct glsl_program *linked,
	char *log)
{
	const struct glsl_storage_info *info;
	struct gles_storage *storage;
	const char *name;
	unsigned index;
	int status;

	/* No more blocks than a stage of the device reads. */
	if (linked->storage_count > GLES_STORAGE_BINDINGS ||
	    linked->storage_count > state->limits.maxPerStageDescriptorStorageBuffers) {
		(void)snprintf(log, PROGRAM_LOG, "the compute shader has %u buffer blocks, more than %u\n", linked->storage_count,
			       GLES_STORAGE_BINDINGS);
		return -1;
	}

	/* Each block, at a binding point there is. */
	program->storage_count = 0U;
	for (index = 0U; index < linked->storage_count; index++) {
		info = &linked->storages[index];
		name = "";
		if (info->name != NULL)
			name = info->name;
		if (info->binding >= GLES_STORAGE_BINDINGS) {
			(void)snprintf(log, PROGRAM_LOG, "buffer block %s is at binding %u, past the last binding point %u\n", name,
				       info->binding, GLES_STORAGE_BINDINGS - 1U);
			return -1;
		}

		/* The block. */
		storage = &program->storages[program->storage_count];
		(void)snprintf(storage->name, sizeof(storage->name), "%s", name);
		storage->binding = info->binding;
		storage->size = info->size;
		storage->readonly = (int)info->readonly;
		storage->array_stride = info->array_stride;
		program->storage_count++;
	}

	/* The blocks' buffer variables. */
	status = program_buffer_variables(program, linked);
	if (status != 0) {
		(void)snprintf(log, PROGRAM_LOG, "no memory for the buffer variables\n");
		return -1;
	}

	/* Succeeded: the blocks are kept. */
	return 0;
}

/* Keeps a compute program's buffer variables as the API lists them; -1 when there is no memory. */
static int
program_buffer_variables(
	struct gles_program *program,
	const struct glsl_program *linked)
{
	const struct glsl_buffer_info *info;
	struct gles_buffer_variable *variable;
	unsigned index;

	/* None. */
	program->buffer_variables = NULL;
	program->buffer_variable_count = 0U;
	if (linked->buffer_count == 0U)
		return 0;

	/* The list. */
	program->buffer_variables = calloc(linked->buffer_count, sizeof(*program->buffer_variables));
	if (program->buffer_variables == NULL)
		return -1;

	/* Each variable, with GL's type. */
	for (index = 0U; index < linked->buffer_count; index++) {
		info = &linked->buffers[index];
		variable = &program->buffer_variables[index];
		(void)snprintf(variable->name, sizeof(variable->name), "%s", info->name);
		variable->type = gles_gl_type(info->base, info->components, info->columns);
		variable->size = (GLint)info->size;
		variable->offset = (GLint)info->offset;
		variable->array_stride = (GLint)info->array_stride;
		variable->matrix_stride = (GLint)info->matrix_stride;
		variable->block = (GLint)info->block;
		variable->top_size = (GLint)info->top_size;
		variable->top_stride = (GLint)info->top_stride;
	}

	/* Succeeded: the variables are kept. */
	program->buffer_variable_count = linked->buffer_count;
	return 0;
}

/*
 * Makes a compute program's pipeline from its SPIR-V (the module goes
 * once the pipeline is made).  Returns 0, or -1 with the log.
 */
static int
program_compute_pipeline(
	struct gles_state *state,
	struct gles_program *program,
	const uint32_t *code,
	size_t words,
	char *log)
{
	VkComputePipelineCreateInfo create;
	VkShaderModule module;
	VkResult result;
	int status;

	/* The module. */
	status = program_module(state, code, words, &module);
	if (status != 0) {
		(void)snprintf(log, PROGRAM_LOG, "the device refused the compute shader\n");
		return -1;
	}

	/* The pipeline over it, with the program's layout. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	create.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	create.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	create.stage.module = module;
	create.stage.pName = "main";
	create.layout = program->layout;
	result = vkCreateComputePipelines(state->device, VK_NULL_HANDLE, 1U, &create, NULL, &program->compute_pipeline);
	vkDestroyShaderModule(state->device, module, NULL);
	if (result != VK_SUCCESS) {
		gles_report("vkCreateComputePipelines", (int)result);
		program->compute_pipeline = VK_NULL_HANDLE;
		(void)snprintf(log, PROGRAM_LOG, "the device refused the compute pipeline\n");
		return -1;
	}

	/* Succeeded: the pipeline. */
	return 0;
}

/* Lets a program's attached shader (NULL: none) go; a deleted shader goes with its last program. */
static void
program_let_go(
	struct gles_state *state,
	struct gles_shader *shader)
{
	/* No shader. */
	if (shader == NULL)
		return;

	/* One program fewer. */
	shader->attached--;
	if (shader->delete_pending && shader->attached == 0U) {
		gles_names_remove(&state->objects, shader->name);
		gles_shader_release(shader);
	}
}

/*
 * Keeps what the linked program captures (transform feedback): each
 * name glTransformFeedbackVaryings gave, with an output's GL type, size,
 * vertex stream and place in the record (the compiler's outputs come in
 * the names' order, without desktop GL's gl_NextBuffer and
 * gl_SkipComponents), the binding point it goes into and its place in
 * that buffer's vertex, the stream of each binding point, the link's
 * buffer mode, and the stage that captures (with a geometry shader's
 * primitive, most vertices and invocations).  Returns 0, or -1 with the
 * log (a special name with separate attributes, a fifth binding point,
 * more components than one buffer takes, or outputs of two streams in
 * one buffer).
 */
static int
program_glsl_captures(
	struct gles_program *program,
	const struct glsl_program *linked,
	char *log)
{
	const struct glsl_capture_info *info;
	struct gles_capture *capture;
	unsigned output;
	unsigned buffer;
	unsigned skipped;
	unsigned streamed;
	unsigned index;
	int special;

	/* Nothing captured yet, with the record's words, the buffer mode the link was made with, and the stage that captures. */
	program->capture_count = 0U;
	program->capture_buffers = 0U;
	memset(program->capture_buffer_words, 0, sizeof(program->capture_buffer_words));
	memset(program->capture_buffer_streams, 0, sizeof(program->capture_buffer_streams));
	program->capture_stride = linked->capture_stride;
	program->capture_mode = program->feedback_mode;
	program->capture_stage = linked->capture_stage;
	program->capture_primitive = (GLenum)linked->capture_primitive;
	program->capture_vertices = linked->capture_vertices;
	program->capture_invocations = linked->capture_invocations;

	/* Each name in order, from the first binding point (no buffer's stream known yet: bit i binding point i). */
	output = 0U;
	buffer = 0U;
	streamed = 0U;
	for (index = 0U; index < program->feedback_count; index++) {
		capture = &program->captures[index];
		memset(capture, 0, sizeof(*capture));
		(void)snprintf(capture->name, GLES_NAME, "%s", program->feedback_names[index]);
		special = program_capture_special(capture->name, &skipped);

		/* Separate attributes are outputs only, each into a binding point of its own. */
		if (program->feedback_mode == GL_SEPARATE_ATTRIBS) {
			if (special) {
				(void)snprintf(log, PROGRAM_LOG, "%s is only captured with GL_INTERLEAVED_ATTRIBS\n", capture->name);
				return -1;
			}
			buffer = index;
		}

		/* gl_NextBuffer moves on to the next binding point, which must be one of the context's. */
		if (special && skipped == 0U) {
			buffer++;
			if (buffer >= GLES_FEEDBACK_BINDINGS) {
				(void)snprintf(log, PROGRAM_LOG, "transform feedback writes more than %u buffers\n",
					       (unsigned)GLES_FEEDBACK_BINDINGS);
				return -1;
			}
		}

		/* A special name has no type, and gl_SkipComponents' size is the components it skips. */
		capture->buffer = buffer;
		capture->place = program->capture_buffer_words[buffer];
		if (special) {
			capture->type = GL_NONE;
			capture->size = (GLint)skipped;
			program->capture_buffer_words[buffer] += skipped;
		} else {
			/* The compiler's next output. */
			if (output >= linked->capture_count) {
				(void)snprintf(log, PROGRAM_LOG, "transform feedback varying %s was not linked\n", capture->name);
				return -1;
			}
			info = &linked->captures[output];
			output++;

			/* Its type, size, stream and words in the record. */
			capture->type = gles_gl_type(info->base, info->components, info->columns);
			capture->size = (GLint)info->size;
			capture->offset = info->offset;
			capture->words = info->words;
			capture->stream = info->stream;

			/* A buffer takes the outputs of one stream (the first output's). */
			if ((streamed & (1U << buffer)) != 0U && program->capture_buffer_streams[buffer] != info->stream) {
				(void)snprintf(log, PROGRAM_LOG, "transform feedback buffer %u gets outputs of two vertex streams\n", buffer);
				return -1;
			}
			program->capture_buffer_streams[buffer] = info->stream;
			streamed |= 1U << buffer;
			program->capture_buffer_words[buffer] += info->words;
		}

		/* No more components in one buffer than GL_MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS. */
		if (program->capture_buffer_words[buffer] > GLES_CAPTURE_COMPONENTS) {
			(void)snprintf(log, PROGRAM_LOG, "transform feedback writes more than %u components into one buffer\n",
				       (unsigned)GLES_CAPTURE_COMPONENTS);
			return -1;
		}

		/* The name is the capture's, with the binding points up to its own. */
		program->capture_buffers = buffer + 1U;
		program->capture_count = index + 1U;
	}

	/* Succeeded: the names and their places. */
	return 0;
}

/*
 * Reports whether a name of glTransformFeedbackVaryings is one of desktop
 * GL's special names (GL_ARB_transform_feedback3, libGL): gl_NextBuffer
 * (*skipped 0) or gl_SkipComponents1 to 4 (*skipped the components).
 * OpenGL ES has none: there they are names of outputs like any other.
 */
static int
program_capture_special(
	const char *name,
	unsigned *skipped)
{
	int differs;

	/* Not desktop GL. */
	*skipped = 0U;
	if (gles_fixed == NULL)
		return 0;

	/* The next binding point. */
	differs = strcmp(name, "gl_NextBuffer");
	if (differs == 0)
		return 1;

	/* Anything else is an output's name unless it is gl_SkipComponents with one digit after it. */
	differs = strncmp(name, "gl_SkipComponents", 17U);
	if (differs != 0)
		return 0;
	if (name[17] < '1' || name[17] > '4')
		return 0;
	if (name[18] != '\0')
		return 0;

	/* Succeeded: the components skipped, one to four. */
	*skipped = (unsigned)(name[17] - '0');
	return 1;
}

/*
 * Gives the uniforms the GL types the SPIR-V does not say: bools (which
 * the block holds as uints) and the kinds of samplers.
 */
static void
program_glsl_types(
	struct gles_program *program,
	const struct glsl_program *linked)
{
	static const GLenum bools[4] = { GL_BOOL, GL_BOOL_VEC2, GL_BOOL_VEC3, GL_BOOL_VEC4 };
	const struct glsl_uniform_info *info;
	struct gles_uniform *uniform;
	unsigned index;
	unsigned other;
	int differs;

	/* Each uniform of the program, by its name among the compiler's. */
	for (index = 0U; index < program->uniform_count; index++) {
		uniform = &program->uniforms[index];
		info = NULL;
		for (other = 0U; other < linked->uniform_count; other++) {
			differs = strcmp(linked->uniforms[other].name, uniform->name);
			if (differs != 0)
				continue;
			info = &linked->uniforms[other];
			break;
		}

		/* A uniform the compiler does not know needs nothing. */
		if (info == NULL)
			continue;

		/* A bool: its values are bools. */
		if (info->base == GLSL_INFO_BOOL && info->components >= 1U && info->components <= 4U) {
			uniform->base = 3U;
			uniform->type = bools[info->components - 1U];
			continue;
		}

		/* A sampler: its dimension, layers, comparison and texel kind. */
		if (info->sampler != GLSL_SAMPLER_NONE)
			uniform->type = program_sampler_type(info);
	}
}

/* Returns GL's type of a sampler the compiler describes (its dimension, layers, comparison and texel kind). */
static GLenum
program_sampler_type(
	const struct glsl_uniform_info *info)
{
	/* A shadow sampler compares depth. */
	if (info->shadow && info->sampler != GLSL_SAMPLER_RECT) {
		if (info->sampler == GLSL_SAMPLER_CUBE && info->arrayed)
			return GL_SAMPLER_CUBE_MAP_ARRAY_SHADOW;
		if (info->sampler == GLSL_SAMPLER_CUBE)
			return GL_SAMPLER_CUBE_SHADOW;
		if (info->sampler == GLSL_SAMPLER_1D)
			return PROGRAM_SAMPLER_1D_SHADOW;
		if (info->arrayed)
			return GL_SAMPLER_2D_ARRAY_SHADOW;
		return GL_SAMPLER_2D_SHADOW;
	}

	/* The dimension, for each kind of texel (desktop GL's rectangle and buffer samplers too). */
	switch (info->sampler) {
	case GLSL_SAMPLER_RECT:
		if (info->shadow)
			return GL_SAMPLER_2D_RECT_SHADOW;
		if (info->base == GLSL_INFO_INT)
			return GL_INT_SAMPLER_2D_RECT;
		if (info->base == GLSL_INFO_UINT)
			return GL_UNSIGNED_INT_SAMPLER_2D_RECT;
		return GL_SAMPLER_2D_RECT;
	case GLSL_SAMPLER_BUFFER:
		if (info->base == GLSL_INFO_INT)
			return GL_INT_SAMPLER_BUFFER;
		if (info->base == GLSL_INFO_UINT)
			return GL_UNSIGNED_INT_SAMPLER_BUFFER;
		return GL_SAMPLER_BUFFER;
	case GLSL_SAMPLER_MS:
		if (info->base == GLSL_INFO_INT)
			return GL_INT_SAMPLER_2D_MULTISAMPLE;
		if (info->base == GLSL_INFO_UINT)
			return GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE;
		return GL_SAMPLER_2D_MULTISAMPLE;
	case GLSL_SAMPLER_CUBE:
		if (info->arrayed && info->base == GLSL_INFO_INT)
			return GL_INT_SAMPLER_CUBE_MAP_ARRAY;
		if (info->arrayed && info->base == GLSL_INFO_UINT)
			return GL_UNSIGNED_INT_SAMPLER_CUBE_MAP_ARRAY;
		if (info->arrayed)
			return GL_SAMPLER_CUBE_MAP_ARRAY;
		if (info->base == GLSL_INFO_INT)
			return GL_INT_SAMPLER_CUBE;
		if (info->base == GLSL_INFO_UINT)
			return GL_UNSIGNED_INT_SAMPLER_CUBE;
		return GL_SAMPLER_CUBE;
	case GLSL_SAMPLER_3D:
		if (info->base == GLSL_INFO_INT)
			return GL_INT_SAMPLER_3D;
		if (info->base == GLSL_INFO_UINT)
			return GL_UNSIGNED_INT_SAMPLER_3D;
		return GL_SAMPLER_3D;
	case GLSL_SAMPLER_1D:
		return PROGRAM_SAMPLER_1D;
	default:
		break;
	}

	/* A 2D sampler, or one of an array of 2D layers. */
	if (info->arrayed) {
		if (info->base == GLSL_INFO_INT)
			return GL_INT_SAMPLER_2D_ARRAY;
		if (info->base == GLSL_INFO_UINT)
			return GL_UNSIGNED_INT_SAMPLER_2D_ARRAY;
		return GL_SAMPLER_2D_ARRAY;
	}

	/* A plain 2D one. */
	if (info->base == GLSL_INFO_INT)
		return GL_INT_SAMPLER_2D;
	if (info->base == GLSL_INFO_UINT)
		return GL_UNSIGNED_INT_SAMPLER_2D;
	return GL_SAMPLER_2D;
}

/*
 * Gives the named uniform blocks what the GLSL compiler knows of them
 * (their names, sizes and stages, which the SPIR-V's reflection gave only
 * in part) and adds their members to the uniforms, without locations.
 * Returns 0, or -1 with the log when there is no memory.
 */
static int
program_glsl_blocks(
	struct gles_program *program,
	const struct glsl_program *linked,
	char *log)
{
	const struct glsl_block_info *info;
	const struct glsl_uniform_info *member;
	struct gles_uniform *uniforms;
	struct gles_uniform *uniform;
	struct gles_block *block;
	unsigned members;
	unsigned index;

	/* The compiler's list replaces the SPIR-V's (it knows which elements of an array of blocks the code reads). */
	memset(program->blocks, 0, sizeof(program->blocks));
	program->block_count = 0U;
	for (index = 0U; index < linked->block_count && index < GLES_NAMED_BLOCKS; index++) {
		info = &linked->blocks[index];
		block = &program->blocks[index];
		(void)snprintf(block->name, sizeof(block->name), "%s", info->name);
		block->binding = info->binding;
		block->size = info->size;
		block->element = info->element;
		block->count = info->count;
		block->members = info->members;
		block->stages = info->stages;
		block->member_count = info->member_count;
		program->block_count = index + 1U;
	}

	/* How many members there are. */
	members = 0U;
	for (index = 0U; index < linked->uniform_count; index++) {
		if (linked->uniforms[index].block >= 0)
			members++;
	}

	/* No members: nothing to add. */
	if (members == 0U)
		return 0;

	/* Room for them after the other uniforms. */
	uniforms = realloc(program->uniforms, (program->uniform_count + members) * sizeof(*uniforms));
	if (uniforms == NULL) {
		(void)snprintf(log, PROGRAM_LOG, "out of memory\n");
		return -1;
	}

	/* The larger table is the program's. */
	program->uniforms = uniforms;

	/* Each member, with its block, offset and strides and no location. */
	for (index = 0U; index < linked->uniform_count; index++) {
		member = &linked->uniforms[index];
		if (member->block < 0)
			continue;
		uniform = &program->uniforms[program->uniform_count];
		memset(uniform, 0, sizeof(*uniform));
		(void)snprintf(uniform->name, sizeof(uniform->name), "%s", member->name);
		uniform->type = gles_gl_type(member->base, member->components, member->columns);
		uniform->size = (GLint)member->size;
		uniform->base = member->base;
		uniform->components = member->components;
		uniform->columns = member->columns;
		uniform->offset = member->offset;
		uniform->array_stride = member->array_stride;
		uniform->matrix_stride = member->matrix_stride;
		uniform->binding = program->blocks[member->block].binding;
		uniform->location = -1;
		uniform->block = member->block;
		uniform->row_major = (int)member->row_major;
		program->uniform_count++;
	}

	/* Succeeded: the members are in. */
	return 0;
}

/*
 * Records the named uniform blocks a stage's SPIR-V reads (bindings 32
 * on), in order of binding and element: each block, or each element of an
 * array of blocks ("B[i]", a descriptor of its binding an element), with
 * the block's type name, and the stage.  Returns 0, or -1 with the log
 * for a binding outside the ones a program has or too many blocks.
 */
static int
program_spirv_blocks(
	struct gles_program *program,
	const struct gles_spirv *spirv,
	unsigned stage,
	char *log)
{
	struct gles_block *block;
	uint32_t binding;
	uint32_t element;
	uint32_t count;
	unsigned index;
	unsigned slot;

	/* Each named block of the stage. */
	for (index = 0U; index < spirv->named_count; index++) {
		binding = spirv->named_bindings[index];
		count = spirv->named_counts[index];
		if (binding < GLES_FIRST_BLOCK_BINDING || binding - GLES_FIRST_BLOCK_BINDING >= GLES_NAMED_BLOCKS) {
			(void)snprintf(log, PROGRAM_LOG, "a uniform block is at binding %u, outside %u to %u\n", (unsigned)binding,
				       GLES_FIRST_BLOCK_BINDING, GLES_FIRST_BLOCK_BINDING + GLES_NAMED_BLOCKS - 1U);
			return -1;
		}

		/* Each of its elements: the block of that binding and element, named by the first stage that has it, read by each. */
		for (element = 0U; element < count; element++) {
			slot = program_block_slot(program, binding, element);
			if (slot == GLES_NAMED_BLOCKS) {
				(void)snprintf(log, PROGRAM_LOG, "more than %u uniform blocks\n", GLES_NAMED_BLOCKS);
				return -1;
			}
			block = &program->blocks[slot];
			if (block->stages == 0U && count == 1U) {
				(void)snprintf(block->name, sizeof(block->name), "%s", spirv->named_names[index]);
			} else if (block->stages == 0U) {
				(void)snprintf(block->name, sizeof(block->name), "%s[%u]", spirv->named_names[index], (unsigned)element);
			}
			block->binding = binding;
			block->element = element;
			block->count = count;
			block->stages |= 1U << stage;
		}
	}

	/* Each block's members are its own (a SPIR-V binary's blocks have none listed). */
	for (index = 0U; index < program->block_count; index++)
		program->blocks[index].members = index;

	/* Succeeded: the stage's blocks are recorded. */
	return 0;
}

/*
 * Returns the place among a program's named blocks (in order of binding
 * and element) of the block of a binding and element: the one there is,
 * or a new one moved in at its place (cleared, without stages).
 * GLES_NAMED_BLOCKS when there is no room.
 */
static unsigned
program_block_slot(
	struct gles_program *program,
	uint32_t binding,
	uint32_t element)
{
	unsigned slot;
	unsigned index;

	/* The first block not before it. */
	for (slot = 0U; slot < program->block_count; slot++) {
		if (program->blocks[slot].binding > binding)
			break;
		if (program->blocks[slot].binding == binding && program->blocks[slot].element >= element)
			break;
	}

	/* The block itself. */
	if (slot < program->block_count &&
	    program->blocks[slot].binding == binding &&
	    program->blocks[slot].element == element)
		return slot;

	/* No room for another. */
	if (program->block_count == GLES_NAMED_BLOCKS)
		return GLES_NAMED_BLOCKS;

	/* The later ones move up one, and the new one is cleared at its place. */
	for (index = program->block_count; index > slot; index--)
		program->blocks[index] = program->blocks[index - 1U];
	memset(&program->blocks[slot], 0, sizeof(program->blocks[slot]));
	program->block_count++;

	/* Succeeded: the new block's place. */
	return slot;
}

/*
 * Checks the fragment outputs' dual-source blend indexes after the
 * bindings, and gives an Index decoration to each output that is the
 * second colour without one (an OpDecorate after its Location's, so the
 * words after it move: nothing may use the reflected words afterwards).
 * Replaces *code with the longer copy when one is made.  Returns 0, or -1
 * with the log.
 */
static int
program_output_indexes(
	struct gles_spirv *fragment,
	uint32_t **code,
	size_t *words,
	char *log)
{
	const struct gles_spirv_variable *output;
	uint32_t *longer;
	size_t ends[GLES_ATTRIBS * 2U];
	uint32_t ids[GLES_ATTRIBS * 2U];
	size_t end;
	uint32_t id;
	size_t from;
	size_t to;
	unsigned count;
	unsigned index;
	unsigned place;

	/* The second colours: only at location 0 (the one draw buffer a dual-source blend has), and those to decorate in the order of their words. */
	count = 0U;
	for (index = 0U; index < fragment->output_count; index++) {
		output = &fragment->outputs[index];
		if (output->index == 0U)
			continue;
		if (output->location != 0U || output->size > 1) {
			(void)snprintf(log, PROGRAM_LOG, "output '%s' is a second colour at a location other than 0\n", output->name);
			return -1;
		}

		/* One with the decoration already, or without a Location to follow. */
		if (output->index_word != 0U || output->location_word == 0U)
			continue;

		/* After its Location's OpDecorate (4 words, the location the last), kept sorted. */
		end = output->location_word + 1U;
		id = output->id;
		for (place = count; place > 0U && ends[place - 1U] > end; place--) {
			ends[place] = ends[place - 1U];
			ids[place] = ids[place - 1U];
		}

		/* Its place in the sorted list. */
		ends[place] = end;
		ids[place] = id;
		count++;
	}

	/* Nothing to add. */
	if (count == 0U)
		return 0;

	/* A copy with room for the new decorations. */
	longer = malloc((*words + count * 4U) * sizeof(uint32_t));
	if (longer == NULL) {
		(void)snprintf(log, PROGRAM_LOG, "out of memory\n");
		return -1;
	}

	/* The words copied in order, each new OpDecorate %id Index 1 after the instruction it follows. */
	from = 0U;
	to = 0U;
	for (index = 0U; index < count; index++) {
		memcpy(longer + to, *code + from, (ends[index] - from) * sizeof(uint32_t));
		to += ends[index] - from;
		from = ends[index];
		longer[to++] = (4U << 16) | PROGRAM_OP_DECORATE;
		longer[to++] = ids[index];
		longer[to++] = PROGRAM_DECORATION_INDEX;
		longer[to++] = 1U;
	}

	/* The rest. */
	memcpy(longer + to, *code + from, (*words - from) * sizeof(uint32_t));
	to += *words - from;

	/* Succeeded: the code is the longer copy. */
	free(*code);
	*code = longer;
	*words = to;
	return 0;
}

/*
 * Links SPIR-V of the two stages (and of a geometry stage between them,
 * NULL for none): the interfaces, the attribute and varying locations,
 * gl_Position made Vulkan's (in the last stage before the rasterizer),
 * the uniforms, the shader modules and the layouts.  Returns 0, or -1
 * with the log.
 */
static int
program_link_code(
	struct gles_state *state,
	struct gles_program *program,
	const uint32_t *vertex_input,
	size_t vertex_words,
	const uint32_t *fragment_input,
	size_t fragment_words,
	const uint32_t *geometry_input,
	size_t geometry_words,
	const struct glsl_program *tessellation,
	char *log)
{
	struct gles_spirv vertex;
	struct gles_spirv fragment;
	struct gles_spirv_variable *input;
	int later;
	uint32_t *vertex_code;
	uint32_t *fragment_code;
	uint32_t *patched;
	uint32_t *patched_fbo;
	size_t patched_words;
	size_t patched_fbo_words;
	unsigned index;
	unsigned other;
	int status;
	int differs;

	/* The two interfaces. */
	status = gles_spirv_reflect(vertex_input, vertex_words, &vertex, log, PROGRAM_LOG);
	if (status != 0)
		return -1;
	status = gles_spirv_reflect(fragment_input, fragment_words, &fragment, log, PROGRAM_LOG);
	if (status != 0) {
		gles_spirv_free(&vertex);
		return -1;
	}

	/* The named uniform blocks of both stages, at their bindings. */
	status = program_spirv_blocks(program, &vertex, 0U, log);
	if (status == 0)
		status = program_spirv_blocks(program, &fragment, 1U, log);
	if (status != 0) {
		gles_spirv_free(&vertex);
		gles_spirv_free(&fragment);
		return -1;
	}

	/* The stages must be the ones the shaders say. */
	if (vertex.model != 0U || fragment.model != 4U) {
		(void)snprintf(log, PROGRAM_LOG, "the SPIR-V stages do not match the shader types\n");
		gles_spirv_free(&vertex);
		gles_spirv_free(&fragment);
		return -1;
	}

	/* Copies of the code that the link may change. */
	vertex_code = malloc(vertex_words * sizeof(uint32_t));
	fragment_code = malloc(fragment_words * sizeof(uint32_t));
	if (vertex_code == NULL || fragment_code == NULL) {
		free(vertex_code);
		free(fragment_code);
		gles_spirv_free(&vertex);
		gles_spirv_free(&fragment);
		(void)snprintf(log, PROGRAM_LOG, "out of memory\n");
		return -1;
	}

	/* The codes copied. */
	memcpy(vertex_code, vertex_input, vertex_words * sizeof(uint32_t));
	memcpy(fragment_code, fragment_input, fragment_words * sizeof(uint32_t));

	/* The locations glBindFragDataLocation gave the fragment shader's outputs (libGL). */
	for (index = 0U; index < fragment.output_count; index++) {
		for (other = 0U; other < program->frag_bound_count; other++) {
			differs = strcmp(fragment.outputs[index].name, program->frag_bound_names[other]);
			if (differs != 0 || fragment.outputs[index].location_word == 0U)
				continue;
			fragment.outputs[index].location = program->frag_bound_locations[other];
			fragment_code[fragment.outputs[index].location_word] = program->frag_bound_locations[other];
			fragment.outputs[index].index = program->frag_bound_indexes[other];
			if (fragment.outputs[index].index_word != 0U)
				fragment_code[fragment.outputs[index].index_word] = program->frag_bound_indexes[other];
		}
	}

	/* Whether a fragment input is flat, so draws follow GL's provoking vertex. */
	program->flat_inputs = program_flat_inputs(fragment_code, fragment_words);

	/* The fragment shader's outputs, by name and location. */
	program->output_count = 0U;
	for (index = 0U; index < fragment.output_count && program->output_count < GLES_DRAW_BUFFERS; index++) {
		(void)snprintf(program->output_names[program->output_count], GLES_NAME, "%s", fragment.outputs[index].name);
		program->output_locations[program->output_count] = (GLint)fragment.outputs[index].location;
		program->output_indexes[program->output_count] = (GLint)fragment.outputs[index].index;
		program->output_count++;
	}

	/* The attribute locations glBindAttribLocation gave. */
	for (index = 0U; index < vertex.input_count; index++) {
		for (other = 0U; other < program->bound_count; other++) {
			differs = strcmp(vertex.inputs[index].name, program->bound_names[other]);
			if (differs != 0)
				continue;
			vertex.inputs[index].location = program->bound_locations[other];
			vertex_code[vertex.inputs[index].location_word] = program->bound_locations[other];
		}
	}

	/* Stages after the vertex stage (the GLSL link located their varyings, and the last of them rewrites gl_Position). */
	later = 0;
	if (geometry_input != NULL)
		later = 1;
	if (tessellation != NULL && tessellation->code[GLSL_STAGE_TESS_CONTROL] != NULL)
		later = 1;

	/* Each fragment input takes the location of the vertex output of its name, when the vertex stage is the last. */
	for (index = 0U; index < fragment.input_count && !later; index++) {
		input = &fragment.inputs[index];
		for (other = 0U; other < vertex.output_count; other++) {
			differs = strcmp(input->name, vertex.outputs[other].name);
			if (differs != 0 || input->name[0] == '\0')
				continue;
			input->location = vertex.outputs[other].location;
			fragment_code[input->location_word] = vertex.outputs[other].location;
		}
	}

	/* The second colours glBindFragDataLocationIndexed gave outputs without an Index decoration get one (last: the words move). */
	status = program_output_indexes(&fragment, &fragment_code, &fragment_words, log);
	if (status != 0) {
		free(vertex_code);
		free(fragment_code);
		gles_spirv_free(&vertex);
		gles_spirv_free(&fragment);
		return -1;
	}

	/* The attributes. */
	program->attribute_count = 0U;
	for (index = 0U; index < vertex.input_count && index < GLES_ATTRIBS; index++) {
		if (vertex.inputs[index].location >= GLES_ATTRIBS)
			continue;
		(void)snprintf(program->attributes[program->attribute_count].name, GLES_NAME, "%s", vertex.inputs[index].name);
		program->attributes[program->attribute_count].type = vertex.inputs[index].type;
		program->attributes[program->attribute_count].size = vertex.inputs[index].size;
		program->attributes[program->attribute_count].location = vertex.inputs[index].location;
		program->attributes[program->attribute_count].components = vertex.inputs[index].components;
		program->attribute_count++;
	}

	/* The vertex shader's gl_Position made Vulkan's: for a window, y turned over (unchanged before a later stage, which does it). */
	patched = gles_spirv_position(vertex_code, vertex_words, 1, 1, &patched_words);
	if (later && patched != NULL) {
		memcpy(patched, vertex_code, vertex_words * sizeof(uint32_t));
		patched_words = vertex_words;
	}

	/* A copy that could not be made. */
	if (patched == NULL) {
		free(vertex_code);
		free(fragment_code);
		gles_spirv_free(&vertex);
		gles_spirv_free(&fragment);
		(void)snprintf(log, PROGRAM_LOG, "the vertex shader's gl_Position could not be rewritten\n");
		return -1;
	}

	/* And for a framebuffer object, whose image keeps GL's rows. */
	patched_fbo = gles_spirv_position(vertex_code, vertex_words, 0, 1, &patched_fbo_words);
	if (later && patched_fbo != NULL) {
		memcpy(patched_fbo, vertex_code, vertex_words * sizeof(uint32_t));
		patched_fbo_words = vertex_words;
	}

	/* The vertex shader's code kept when it is the last stage (glClipControl's depth range makes other modules of it). */
	status = 0;
	if (!later)
		status = program_keep_last(program, vertex_code, vertex_words, log);
	if (status != 0) {
		free(vertex_code);
		free(patched);
		free(patched_fbo);
		free(fragment_code);
		gles_spirv_free(&vertex);
		gles_spirv_free(&fragment);
		return -1;
	}

	/* The code is in the copies now. */
	free(vertex_code);
	if (patched_fbo == NULL) {
		free(patched);
		free(fragment_code);
		gles_spirv_free(&vertex);
		gles_spirv_free(&fragment);
		(void)snprintf(log, PROGRAM_LOG, "the vertex shader's gl_Position could not be rewritten\n");
		return -1;
	}

	/* The uniforms of both stages, merged by name, and the interface the API lists. */
	status = program_merge(program, &vertex, log);
	if (status == 0)
		status = program_merge(program, &fragment, log);
	if (status == 0)
		status = program_interface_keep(program, &vertex, &fragment);
	gles_spirv_free(&vertex);
	gles_spirv_free(&fragment);

	/* The shader modules. */
	if (status == 0)
		status = program_module(state, patched, patched_words, &program->vertex_module);
	if (status == 0)
		status = program_module(state, patched_fbo, patched_fbo_words, &program->vertex_module_fbo);
	if (status == 0)
		status = program_module(state, fragment_code, fragment_words, &program->fragment_module);
	free(patched);
	free(patched_fbo);
	free(fragment_code);
	if (status != 0) {
		if (log[0] == '\0')
			(void)snprintf(log, PROGRAM_LOG, "the device refused a shader\n");
		return -1;
	}

	/* The stages that read descriptors, and the geometry stage (its uniforms, blocks and modules). */
	program->stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	if (geometry_input != NULL) {
		status = program_link_geometry(state, program, geometry_input, geometry_words, log);
		if (status != 0)
			return -1;
		program->stages |= VK_SHADER_STAGE_GEOMETRY_BIT;
	}

	/* The tessellation stages, the evaluation one the last before the rasterizer without a geometry stage. */
	if (tessellation != NULL && tessellation->code[GLSL_STAGE_TESS_CONTROL] != NULL) {
		status = program_link_tessellation(state, program, tessellation, geometry_input == NULL, log);
		if (status != 0)
			return -1;
		program->stages |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
	}

	/* The layouts. */
	status = program_multisample_refused(state, program, log);
	if (status != 0)
		return -1;
	status = program_layout(state, program);
	if (status != 0) {
		(void)snprintf(log, PROGRAM_LOG, "the device refused the program's layout\n");
		return -1;
	}

	/* Succeeded: a serial of its own. */
	program->serial = program_serial++;
	return 0;
}

/*
 * Links a program's geometry stage (after the vertex and the fragment
 * stage): its named blocks and uniforms added to the program's, and its
 * modules with gl_Position made Vulkan's at each vertex it emits (for a
 * window and for a framebuffer object).  Returns 0, or -1 with the log.
 */
static int
program_link_geometry(
	struct gles_state *state,
	struct gles_program *program,
	const uint32_t *code,
	size_t words,
	char *log)
{
	struct gles_spirv geometry;
	uint32_t *patched;
	size_t patched_words;
	int status;

	/* Its interface, blocks and uniforms. */
	status = gles_spirv_reflect(code, words, &geometry, log, PROGRAM_LOG);
	if (status != 0)
		return -1;
	status = program_spirv_blocks(program, &geometry, GLSL_STAGE_GEOMETRY, log);
	if (status == 0)
		status = program_merge(program, &geometry, log);
	gles_spirv_free(&geometry);
	if (status != 0)
		return -1;

	/* Its code kept (glClipControl's depth range makes other modules of it). */
	status = program_keep_last(program, code, words, log);
	if (status != 0)
		return -1;

	/* The module for a window: y turned over. */
	patched = gles_spirv_position(code, words, 1, 1, &patched_words);
	if (patched == NULL) {
		(void)snprintf(log, PROGRAM_LOG, "the geometry shader's gl_Position could not be rewritten\n");
		return -1;
	}

	/* Its module. */
	status = program_module(state, patched, patched_words, &program->geometry_module);
	free(patched);
	if (status != 0) {
		(void)snprintf(log, PROGRAM_LOG, "the device refused the geometry shader\n");
		return -1;
	}

	/* And for a framebuffer object. */
	patched = gles_spirv_position(code, words, 0, 1, &patched_words);
	if (patched == NULL) {
		(void)snprintf(log, PROGRAM_LOG, "the geometry shader's gl_Position could not be rewritten\n");
		return -1;
	}

	/* Its module. */
	status = program_module(state, patched, patched_words, &program->geometry_module_fbo);
	free(patched);
	if (status != 0) {
		(void)snprintf(log, PROGRAM_LOG, "the device refused the geometry shader\n");
		return -1;
	}

	/* Succeeded: the geometry stage is linked. */
	return 0;
}

/*
 * Links a program's tessellation stages (after the vertex, the fragment
 * and any geometry stage): their named blocks and uniforms added to the
 * program's, the control stage's module, and the evaluation stage's
 * modules (for a window and for a framebuffer object) with gl_Position
 * made Vulkan's when it is the last stage before the rasterizer.
 * Returns 0, or -1 with the log.
 */
static int
program_link_tessellation(
	struct gles_state *state,
	struct gles_program *program,
	const struct glsl_program *linked,
	int last,
	char *log)
{
	static const unsigned stages[2] = { GLSL_STAGE_TESS_CONTROL, GLSL_STAGE_TESS_EVALUATION };
	struct gles_spirv spirv;
	unsigned index;
	int status;

	/* Each stage's interface, blocks and uniforms. */
	for (index = 0U; index < 2U; index++) {
		status = gles_spirv_reflect(linked->code[stages[index]], linked->words[stages[index]], &spirv, log, PROGRAM_LOG);
		if (status != 0)
			return -1;
		status = program_spirv_blocks(program, &spirv, stages[index], log);
		if (status == 0)
			status = program_merge(program, &spirv, log);
		gles_spirv_free(&spirv);
		if (status != 0)
			return -1;
	}

	/* The control stage's module, as it is. */
	status = program_module(state, linked->code[GLSL_STAGE_TESS_CONTROL], linked->words[GLSL_STAGE_TESS_CONTROL],
				&program->tess_control_module);
	if (status != 0) {
		(void)snprintf(log, PROGRAM_LOG, "the device refused the tessellation control shader\n");
		return -1;
	}

	/* The evaluation stage's code kept when it is the last stage (glClipControl's depth range makes other modules of it). */
	if (last) {
		status = program_keep_last(program, linked->code[GLSL_STAGE_TESS_EVALUATION], linked->words[GLSL_STAGE_TESS_EVALUATION], log);
		if (status != 0)
			return -1;
	}

	/* The evaluation stage's two. */
	status = program_stage_modules(state, linked->code[GLSL_STAGE_TESS_EVALUATION], linked->words[GLSL_STAGE_TESS_EVALUATION],
				       last, &program->tess_evaluation_module, &program->tess_evaluation_module_fbo, log);
	if (status != 0)
		return -1;

	/* Succeeded: the tessellation stages are linked. */
	return 0;
}

/*
 * Makes a stage's two modules, for a window and for a framebuffer object:
 * with gl_Position made Vulkan's (y turned over for the window) when
 * rewrite is nonzero, else both as the code is.  Returns 0, or -1 with
 * the log.
 */
static int
program_stage_modules(
	struct gles_state *state,
	const uint32_t *code,
	size_t words,
	int rewrite,
	VkShaderModule *window,
	VkShaderModule *fbo,
	char *log)
{
	uint32_t *patched;
	size_t patched_words;
	int status;

	/* Without the rewrite, one code for both. */
	if (!rewrite) {
		status = program_module(state, code, words, window);
		if (status == 0)
			status = program_module(state, code, words, fbo);
		if (status != 0) {
			(void)snprintf(log, PROGRAM_LOG, "the device refused a shader\n");
			return -1;
		}

		/* Succeeded: both modules of the code as it is. */
		return 0;
	}

	/* The module for a window: y turned over. */
	patched = gles_spirv_position(code, words, 1, 1, &patched_words);
	if (patched == NULL) {
		(void)snprintf(log, PROGRAM_LOG, "a shader's gl_Position could not be rewritten\n");
		return -1;
	}
	status = program_module(state, patched, patched_words, window);
	free(patched);
	if (status != 0) {
		(void)snprintf(log, PROGRAM_LOG, "the device refused a shader\n");
		return -1;
	}

	/* And for a framebuffer object. */
	patched = gles_spirv_position(code, words, 0, 1, &patched_words);
	if (patched == NULL) {
		(void)snprintf(log, PROGRAM_LOG, "a shader's gl_Position could not be rewritten\n");
		return -1;
	}
	status = program_module(state, patched, patched_words, fbo);
	free(patched);
	if (status != 0) {
		(void)snprintf(log, PROGRAM_LOG, "the device refused a shader\n");
		return -1;
	}

	/* Succeeded: both modules. */
	return 0;
}

/*
 * Keeps the interface the API lists of a linked pair of stages: the
 * inputs of the first stage the link had (the vertex shader's, or a
 * separable fragment shader's) and the outputs of the last (the fragment
 * shader's, or a separable vertex shader's).  Returns 0, or -1 when there
 * is no memory.
 */
static int
program_interface_keep(
	struct gles_program *program,
	const struct gles_spirv *vertex,
	const struct gles_spirv *fragment)
{
	const struct gles_spirv *first;
	const struct gles_spirv *last;

	/* The first and the last of the stages the link had. */
	first = vertex;
	if ((program->linked_stages & GL_VERTEX_SHADER_BIT) == 0U)
		first = fragment;
	last = fragment;
	if ((program->linked_stages & GL_FRAGMENT_SHADER_BIT) == 0U)
		last = vertex;

	/* The first stage's inputs. */
	program->program_input_count = 0U;
	if (first->input_count != 0U) {
		program->program_inputs = malloc(first->input_count * sizeof(*program->program_inputs));
		if (program->program_inputs == NULL)
			return -1;
		memcpy(program->program_inputs, first->inputs, first->input_count * sizeof(*program->program_inputs));
		program->program_input_count = first->input_count;
	}

	/* The last stage's outputs. */
	program->program_output_count = 0U;
	if (last->output_count != 0U) {
		program->program_outputs = malloc(last->output_count * sizeof(*program->program_outputs));
		if (program->program_outputs == NULL)
			return -1;
		memcpy(program->program_outputs, last->outputs, last->output_count * sizeof(*program->program_outputs));
		program->program_output_count = last->output_count;
	}

	/* Succeeded: the interface is kept. */
	return 0;
}

/*
 * Adds a stage's uniforms to a program: a name already there must be at
 * the same place; the block grows to hold every leaf; each new uniform
 * gets locations for its elements.  Nonzero with the log when the stages
 * disagree.
 */
static int
program_merge(
	struct gles_program *program,
	struct gles_spirv *spirv,
	char *log)
{
	struct gles_uniform *uniforms;
	struct gles_location *locations;
	struct gles_uniform *uniform;
	unsigned char *data;
	uint32_t end;
	uint32_t size;
	unsigned index;
	unsigned other;
	GLint element;
	GLbitfield stage;
	int builtin;
	int differs;

	/* The stage's bit (OpenGL ES 3.1's GL_REFERENCED_BY_*: vertex, fragment, compute; none for the others). */
	stage = 0U;
	if (spirv->model == 0U)
		stage = GL_VERTEX_SHADER_BIT;
	if (spirv->model == 4U)
		stage = GL_FRAGMENT_SHADER_BIT;
	if (spirv->model == 5U)
		stage = GL_COMPUTE_SHADER_BIT;

	/* Each uniform of the stage. */
	for (index = 0U; index < spirv->uniform_count; index++) {
		uniform = &spirv->uniforms[index];
		uniform->stages = stage;

		/* One the other stage has already. */
		for (other = 0U; other < program->uniform_count; other++) {
			differs = strcmp(program->uniforms[other].name, uniform->name);
			if (differs == 0)
				break;
		}

		/* Already there: it must be at the same place, and this stage reads it too. */
		if (other < program->uniform_count) {
			if (program->uniforms[other].offset != uniform->offset || program->uniforms[other].binding != uniform->binding) {
				(void)snprintf(log, PROGRAM_LOG, "uniform %s is not at the same place in both stages\n", uniform->name);
				return -1;
			}
			program->uniforms[other].stages |= stage;
			continue;
		}

		/* A built-in uniform libGLESv2 fills (ws068-p004): no location, and gl_ZedFragment not listed at all. */
		builtin = program_builtin_uniform(program, uniform);

		/* A new one, with locations for its elements (none for a built-in). */
		uniforms = realloc(program->uniforms, (program->uniform_count + 1U) * sizeof(*uniforms));
		if (uniforms == NULL)
			return -1;
		program->uniforms = uniforms;
		locations = realloc(program->locations, (program->location_count + (unsigned)uniform->size) * sizeof(*locations));
		if (locations == NULL)
			return -1;
		program->locations = locations;
		uniform->location = (GLint)program->location_count;
		if (builtin)
			uniform->location = -1;
		for (element = 0; !builtin && element < uniform->size; element++) {
			locations[program->location_count].uniform = program->uniform_count;
			locations[program->location_count].element = (unsigned)element;
			program->location_count++;
		}

		/* The uniform, a leaf of the default block or a sampler (each element on unit 0 until glUniform1i). */
		if (builtin != 2) {
			program->uniforms[program->uniform_count] = *uniform;
			program->uniforms[program->uniform_count].block = -1;
			program->uniforms[program->uniform_count].row_major = 0;
			memset(program->uniforms[program->uniform_count].units, 0, sizeof(uniform->units));
			program->uniform_count++;
		}

		/* A leaf of the block makes the block at least that large. */
		if (uniform->sampler)
			continue;
		program->uniform_binding = spirv->block_binding;
		end = uniform->offset + (uint32_t)(uniform->size - 1) * uniform->array_stride;
		if (uniform->columns > 1U) {
			end += uniform->columns * uniform->matrix_stride;
		} else if (uniform->base == PROGRAM_BASE_DOUBLE) {
			end += uniform->components * 8U;
		} else {
			end += uniform->components * 4U;
		}

		/* The block's size, rounded to 16 bytes. */
		size = (end + 15U) & ~15U;
		if (size > program->uniform_size) {
			data = realloc(program->uniform_data, size);
			if (data == NULL)
				return -1;
			memset(data + program->uniform_size, 0, size - program->uniform_size);
			program->uniform_data = data;
			program->uniform_size = size;
		}
	}

	/* Succeeded: the stage's uniforms are in. */
	return 0;
}

/*
 * Notes a built-in uniform of the default block that libGLESv2 fills at
 * each draw (ws068-p004): returns 2 for gl_ZedFragment, gl_ZedImages and
 * a subroutine uniform's hidden uint (hidden: not one of the program's
 * uniforms), 1 for a member of gl_DepthRange (listed, but with no
 * location), 0 for an application's uniform.
 */
static int
program_builtin_uniform(
	struct gles_program *program,
	const struct gles_uniform *uniform)
{
	static const char *const ranges[3] = { "gl_DepthRange.near", "gl_DepthRange.far", "gl_DepthRange.diff" };
	unsigned index;
	int differs;
	int hidden;

	/* The hidden vec4. */
	differs = strcmp(uniform->name, "gl_ZedFragment");
	if (differs == 0) {
		program->zed_fragment = (int32_t)uniform->offset;
		return 2;
	}

	/* The hidden uint of a compute program's valid image units. */
	differs = strcmp(uniform->name, "gl_ZedImages");
	if (differs == 0) {
		program->zed_images = (int32_t)uniform->offset;
		return 2;
	}

	/* A subroutine uniform's hidden uint (desktop GL 4.0). */
	hidden = program_hidden_subroutine(program, uniform);
	if (hidden)
		return 2;

	/* The depth range's members. */
	for (index = 0U; index < 3U; index++) {
		differs = strcmp(uniform->name, ranges[index]);
		if (differs == 0) {
			program->depth_range[index] = (int32_t)uniform->offset;
			return 1;
		}
	}

	/* An application's uniform. */
	return 0;
}

/* Makes a program's descriptor set layout (the default block, each sampler, each named block, each shader storage block and image) and pipeline layout; nonzero on failure. */
static int
program_layout(
	struct gles_state *state,
	struct gles_program *program)
{
	VkDescriptorSetLayoutBinding bindings[GLES_UNITS + 2U + GLES_NAMED_BLOCKS + GLES_STORAGE_BINDINGS + GLES_IMAGE_UNITS];
	VkDescriptorSetLayoutCreateInfo set;
	VkPipelineLayoutCreateInfo layout;
	uint32_t count;
	uint32_t descriptors;
	unsigned index;
	VkResult result;
	int buffer;

	/* The block, when the program has one (dynamic: each draw's copy is at its own offset). */
	count = 0U;
	memset(bindings, 0, sizeof(bindings));
	if (program->uniform_data != NULL) {
		bindings[count].binding = program->uniform_binding;
		bindings[count].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		bindings[count].descriptorCount = 1U;
		bindings[count].stageFlags = program->stages;
		count++;
	}

	/* Each sampler. */
	for (index = 0U; index < program->uniform_count; index++) {
		if (!program->uniforms[index].sampler)
			continue;
		if (count == GLES_UNITS + 1U)
			return -1;
		bindings[count].binding = program->uniforms[index].binding;
		bindings[count].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		buffer = program_buffer_sampler(program->uniforms[index].type);
		if (buffer)
			bindings[count].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
		bindings[count].descriptorCount = (uint32_t)program->uniforms[index].size;
		bindings[count].stageFlags = program->stages;
		count++;
	}

	/* The storage buffer the vertex or geometry shader writes captured outputs (or a geometry shader its counts) into. */
	if (program->capture_count != 0U || program->capture_stage == GLSL_STAGE_GEOMETRY) {
		bindings[count].binding = GLES_CAPTURE_BINDING;
		bindings[count].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		bindings[count].descriptorCount = 1U;
		bindings[count].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
		if (program->capture_stage == GLSL_STAGE_GEOMETRY)
			bindings[count].stageFlags = VK_SHADER_STAGE_GEOMETRY_BIT;
		count++;
	}

	/*
	 * Each named block's binding, once (the blocks are in order of binding):
	 * a buffer range of its own (not dynamic) a block, an array of blocks a
	 * descriptor an element; no more descriptors than a draw's set has.
	 */
	descriptors = 0U;
	for (index = 0U; index < program->block_count; index++) {
		if (program->blocks[index].stages == 0U)
			continue;
		if (index > 0U && program->blocks[index - 1U].binding == program->blocks[index].binding)
			continue;
		descriptors += program->blocks[index].count;
		if (descriptors > GLES_NAMED_BLOCKS)
			return -1;
		bindings[count].binding = program->blocks[index].binding;
		bindings[count].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		bindings[count].descriptorCount = program->blocks[index].count;
		bindings[count].stageFlags = program->stages;
		count++;
	}

	/* Each shader storage block of a compute program (ws101-p009). */
	for (index = 0U; index < program->storage_count; index++) {
		bindings[count].binding = GLES_STORAGE_FIRST_BINDING + program->storages[index].binding;
		bindings[count].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		bindings[count].descriptorCount = 1U;
		bindings[count].stageFlags = program->stages;
		count++;
	}

	/* Each image uniform of a compute program, storage images (an array's one an element) at its first unit's binding. */
	for (index = 0U; index < program->image_count; index++) {
		/* An array's later elements are descriptors of its first element's binding. */
		if (program->images[index].count == 0U)
			continue;
		bindings[count].binding = program->images[index].binding;
		bindings[count].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		bindings[count].descriptorCount = program->images[index].count;
		bindings[count].stageFlags = program->stages;
		count++;
	}

	/* The set layout. */
	memset(&set, 0, sizeof(set));
	set.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set.bindingCount = count;
	set.pBindings = bindings;
	result = vkCreateDescriptorSetLayout(state->device, &set, NULL, &program->set_layout);
	if (result != VK_SUCCESS)
		return -1;

	/* The pipeline layout with the one set. */
	memset(&layout, 0, sizeof(layout));
	layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout.setLayoutCount = 1U;
	layout.pSetLayouts = &program->set_layout;
	result = vkCreatePipelineLayout(state->device, &layout, NULL, &program->layout);
	if (result != VK_SUCCESS)
		return -1;

	/* Succeeded: the layouts. */
	return 0;
}

/* Makes a shader module; nonzero when the device refuses the code. */
static int
program_module(
	struct gles_state *state,
	const uint32_t *code,
	size_t words,
	VkShaderModule *module)
{
	VkShaderModuleCreateInfo create;
	VkResult result;

	/* The module over the words. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	create.codeSize = words * sizeof(uint32_t);
	create.pCode = code;
	result = vkCreateShaderModule(state->device, &create, NULL, module);
	if (result != VK_SUCCESS) {
		gles_report("vkCreateShaderModule", (int)result);
		return -1;
	}

	/* Succeeded: the module. */
	return 0;
}

/*
 * Keeps a copy of the code of a program's last stage before the
 * rasterizer, in place of one kept before.  Returns 0, or -1 with the log.
 */
static int
program_keep_last(
	struct gles_program *program,
	const uint32_t *code,
	size_t words,
	char *log)
{
	uint32_t *copy;

	/* The copy. */
	copy = malloc(words * sizeof(uint32_t));
	if (copy == NULL) {
		(void)snprintf(log, PROGRAM_LOG, "out of memory\n");
		return -1;
	}
	memcpy(copy, code, words * sizeof(uint32_t));

	/* Succeeded: it replaces the one before. */
	free(program->last_code);
	program->last_code = copy;
	program->last_words = words;
	return 0;
}

/* Undoes a program's link: its Vulkan objects wait for the frame, its pipelines go, its tables are freed. */
static void
program_unlink(
	struct gles_state *state,
	struct gles_program *program)
{
	struct gles_garbage objects;
	unsigned stage;

	/* The pipelines made for it, and its Vulkan objects. */
	if (program->serial != 0U)
		gles_pipelines_forget(state, program->serial);
	memset(&objects, 0, sizeof(objects));
	objects.layout = program->layout;
	objects.set_layout = program->set_layout;
	objects.modules[0] = program->vertex_module;
	objects.modules[1] = program->fragment_module;
	objects.modules[2] = program->vertex_module_fbo;
	objects.modules[3] = program->geometry_module;
	objects.modules[4] = program->geometry_module_fbo;
	objects.modules[5] = program->tess_control_module;
	objects.modules[6] = program->tess_evaluation_module;
	objects.modules[7] = program->tess_evaluation_module_fbo;
	objects.modules[8] = program->zero_modules[0];
	objects.modules[9] = program->zero_modules[1];
	objects.pipeline = program->compute_pipeline;
	gles_garbage_keep(state, &objects);

	/* The tables, the last stage's code, the interface lists, and the subroutines. */
	free(program->uniforms);
	free(program->locations);
	free(program->uniform_data);
	free(program->last_code);
	free(program->buffer_variables);
	free(program->program_inputs);
	free(program->program_outputs);
	program->buffer_variables = NULL;
	program->buffer_variable_count = 0U;
	program->program_inputs = NULL;
	program->program_input_count = 0U;
	program->program_outputs = NULL;
	program->program_output_count = 0U;
	program->linked_stages = 0U;
	free(program->stage_sources[0]);
	free(program->stage_sources[1]);
	program->stage_sources[0] = NULL;
	program->stage_sources[1] = NULL;
	for (stage = 0U; stage < GLES_STAGES; stage++) {
		program_subroutines_free(program->subroutines[stage]);
		program->subroutines[stage] = NULL;
	}

	/* Nothing linked. */
	program->layout = VK_NULL_HANDLE;
	program->set_layout = VK_NULL_HANDLE;
	program->vertex_module = VK_NULL_HANDLE;
	program->vertex_module_fbo = VK_NULL_HANDLE;
	program->fragment_module = VK_NULL_HANDLE;
	program->geometry_module = VK_NULL_HANDLE;
	program->geometry_module_fbo = VK_NULL_HANDLE;
	program->tess_control_module = VK_NULL_HANDLE;
	program->tess_evaluation_module = VK_NULL_HANDLE;
	program->tess_evaluation_module_fbo = VK_NULL_HANDLE;
	program->zero_modules[0] = VK_NULL_HANDLE;
	program->zero_modules[1] = VK_NULL_HANDLE;
	program->last_code = NULL;
	program->last_words = 0U;
	program->uniforms = NULL;
	program->uniform_count = 0U;
	program->locations = NULL;
	program->location_count = 0U;
	program->uniform_data = NULL;
	program->uniform_size = 0U;
	program->zed_fragment = -1;
	program->zed_images = -1;
	program->depth_range[0] = -1;
	program->depth_range[1] = -1;
	program->depth_range[2] = -1;
	program->attribute_count = 0U;
	program->serial = 0U;
	program->linked = 0;
	memset(program->blocks, 0, sizeof(program->blocks));
	program->block_count = 0U;
	program->capture_count = 0U;
	program->capture_stride = 0U;
	program->capture_buffers = 0U;
	program->capture_stage = GLSL_STAGE_VERTEX;
	program->capture_primitive = GL_NONE;
	program->geometry_own = 0;
	program->compute_pipeline = VK_NULL_HANDLE;
	memset(program->local_size, 0, sizeof(program->local_size));
	memset(program->storages, 0, sizeof(program->storages));
	program->storage_count = 0U;
	memset(program->images, 0, sizeof(program->images));
	program->image_count = 0U;
}

/*
 * Writes values of a vector size into the current program's uniform at a
 * location, count elements (a sampler takes its texture unit): the values
 * are floats, ints, unsigned ints or doubles (kind, PROGRAM_*), and only
 * doubles go into a double uniform and only into one.
 */
static void
program_uniform(
	GLint location,
	GLsizei count,
	unsigned components,
	const void *values,
	int kind)
{
	struct zegl_context *context;
	struct gles_uniform *uniform;
	unsigned char *data;
	unsigned element;
	unsigned component;
	GLsizei index;
	GLint integer;
	GLuint unsigned_integer;
	GLfloat number;
	double precise;
	uint32_t word;
	int doubles;

	/* Location -1 is ignored. */
	if (location == -1)
		return;

	/* The uniform and element of the location. */
	context = gles_context();
	uniform = program_location(context, location, &element);
	if (uniform == NULL)
		return;
	if (count < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* An image's unit is its layout's, which no call changes (OpenGL ES 3.1). */
	if (uniform->image) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* A sampler takes one texture unit an element, as ints (an array's from the element named on, ws068-p004). */
	if (uniform->sampler) {
		if (kind != PROGRAM_INTS || components != 1U || (count > 1 && uniform->size == 1)) {
			gles_error(context, GL_INVALID_OPERATION);
			return;
		}

		/* The units. */
		for (index = 0; index < count && element + (unsigned)index < (unsigned)uniform->size; index++)
			uniform->units[element + (unsigned)index] = ((const GLint *)values)[index];
		return;
	}

	/* The size must match; a vector or scalar only; doubles for a double uniform and for nothing else. */
	doubles = uniform->base == PROGRAM_BASE_DOUBLE;
	if (uniform->components != components ||
	    uniform->columns != 1U ||
	    (count > 1 && uniform->size == 1) ||
	    doubles != (kind == PROGRAM_DOUBLES)) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* Each element of a double uniform, 8 bytes a component. */
	if (doubles) {
		for (index = 0; index < count && element + (unsigned)index < (unsigned)uniform->size; index++) {
			data = program_uniform_program(gles_state(context))->uniform_data + uniform->offset + (element + (unsigned)index) * uniform->array_stride;
			for (component = 0U; component < components; component++) {
				precise = ((const double *)values)[(size_t)index * components + component];
				memcpy(data + component * 8U, &precise, 8U);
			}
		}

		/* The doubles are in. */
		return;
	}

	/* Each element, each component converted to the uniform's base type. */
	for (index = 0; index < count && element + (unsigned)index < (unsigned)uniform->size; index++) {
		data = program_uniform_program(gles_state(context))->uniform_data + uniform->offset + (element + (unsigned)index) * uniform->array_stride;
		for (component = 0U; component < components; component++) {
			/* The value given: an int, an unsigned int, or a float. */
			if (kind == PROGRAM_INTS) {
				integer = ((const GLint *)values)[(size_t)index * components + component];
				number = (GLfloat)integer;
			} else if (kind == PROGRAM_UNSIGNEDS) {
				unsigned_integer = ((const GLuint *)values)[(size_t)index * components + component];
				integer = (GLint)unsigned_integer;
				number = (GLfloat)unsigned_integer;
			} else {
				number = ((const GLfloat *)values)[(size_t)index * components + component];
				integer = (GLint)number;
				if (uniform->base == 2U)
					integer = (GLint)(GLuint)number;
			}

			/* As a float, an int, or a bool (0 or 1). */
			if (uniform->base == 0U) {
				memcpy(data + component * 4U, &number, 4U);
				continue;
			}

			/* An int or an unsigned int. */
			word = (uint32_t)integer;
			if (uniform->base == 3U && number != 0.0f)
				word = 1U;
			else if (uniform->base == 3U)
				word = 0U;
			memcpy(data + component * 4U, &word, 4U);
		}
	}
}

/*
 * Writes matrices of columns x rows into the current program's uniform at
 * a location, column by column at the block's matrix stride (the values
 * given row by row when transpose is set, which OpenGL ES 2 does not
 * allow): floats into a float matrix, doubles (desktop GL 4.0) into a
 * double one.
 */
static void
program_matrix(
	GLint location,
	GLsizei count,
	GLboolean transpose,
	const void *values,
	int doubles,
	unsigned columns,
	unsigned rows)
{
	struct zegl_context *context;
	struct gles_uniform *uniform;
	unsigned char *data;
	unsigned element;
	unsigned column;
	unsigned row;
	unsigned given;
	unsigned width;
	size_t at;
	GLsizei index;

	/* Location -1 is ignored. */
	if (location == -1)
		return;

	/* The uniform and element of the location. */
	context = gles_context();
	uniform = program_location(context, location, &element);
	if (uniform == NULL)
		return;
	if (count < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Transposed values need OpenGL ES 3 (or desktop GL). */
	if (transpose != GL_FALSE &&
	    gles_fixed == NULL &&
	    context->version < 3) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* A matrix of that size and of that kind of number. */
	if (uniform->sampler ||
	    uniform->image ||
	    uniform->columns != columns ||
	    uniform->components != rows ||
	    (count > 1 && uniform->size == 1) ||
	    (uniform->base == PROGRAM_BASE_DOUBLE) != (doubles != 0)) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* A float's or a double's bytes. */
	width = 4U;
	if (doubles)
		width = 8U;

	/* Each matrix, each column at the stride, each row of it a number. */
	for (index = 0; index < count && element + (unsigned)index < (unsigned)uniform->size; index++) {
		data = program_uniform_program(gles_state(context))->uniform_data + uniform->offset + (element + (unsigned)index) * uniform->array_stride;
		for (column = 0U; column < columns; column++) {
			for (row = 0U; row < rows; row++) {
				given = column * rows + row;
				if (transpose != GL_FALSE)
					given = row * columns + column;
				at = ((size_t)index * columns * rows + given) * width;
				memcpy(data + column * uniform->matrix_stride + row * width, (const unsigned char *)values + at, width);
			}
		}
	}
}

/* Returns the uniform and element of a location of the current program, recording the error when there is none. */
static struct gles_uniform *
program_location(
	struct zegl_context *context,
	GLint location,
	unsigned *element)
{
	struct gles_state *state;
	struct gles_program *program;

	/* The program glUniform* writes (the current one, a pipeline's active one, or glProgramUniform*'s). */
	state = gles_state(context);
	if (state == NULL)
		return NULL;
	program = program_uniform_program(state);
	if (program == NULL || location < 0 || (unsigned)location >= program->location_count) {
		gles_error(context, GL_INVALID_OPERATION);
		return NULL;
	}

	/* Succeeded: the uniform and the element. */
	*element = program->locations[location].element;
	return &program->uniforms[program->locations[location].uniform];
}

/*
 * Reads a uniform's value of a program into the application's array: as
 * floats, ints, unsigned ints or doubles (kind, PROGRAM_*), each component
 * converted from the uniform's own type.
 */
static void
program_get_uniform(
	GLuint name,
	GLint location,
	int kind,
	void *params)
{
	struct zegl_context *context;
	struct gles_program *program;
	struct gles_uniform *uniform;
	struct gles_location *place;
	const unsigned char *data;
	unsigned column;
	unsigned component;
	unsigned slot;
	unsigned width;
	uint32_t word;
	GLfloat number;
	GLint integer;
	double precise;

	/* A linked program and one of its locations. */
	context = gles_context();
	program = program_get(context, name);
	if (program == NULL)
		return;
	if (!program->linked ||
	    location < 0 ||
	    (unsigned)location >= program->location_count) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* The uniform and element of the location. */
	place = &program->locations[location];
	uniform = &program->uniforms[place->uniform];

	/* A sampler's value is its element's unit (an image's its image unit). */
	if (uniform->sampler || uniform->image) {
		if (kind == PROGRAM_FLOATS)
			((GLfloat *)params)[0] = (GLfloat)uniform->units[place->element];
		else if (kind == PROGRAM_DOUBLES)
			((double *)params)[0] = (double)uniform->units[place->element];
		else
			((GLint *)params)[0] = uniform->units[place->element];
		return;
	}

	/* A double's 8 bytes, the other kinds' 4. */
	width = 4U;
	if (uniform->base == PROGRAM_BASE_DOUBLE)
		width = 8U;

	/* Each component of each column, from the uniform's type into the kind asked for. */
	data = program->uniform_data + uniform->offset + place->element * uniform->array_stride;
	for (column = 0U; column < uniform->columns; column++) {
		for (component = 0U; component < uniform->components; component++) {
			slot = column * uniform->components + component;
			word = 0U;
			precise = 0.0;
			if (width == 8U)
				memcpy(&precise, data + column * uniform->matrix_stride + component * 8U, 8U);
			else
				memcpy(&word, data + column * uniform->matrix_stride + component * 4U, 4U);

			/* The value as a double: a double's, a float's, an int's, or an unsigned int's (a bool's is 0 or 1). */
			if (uniform->base == PROGRAM_BASE_DOUBLE) {
				integer = (GLint)precise;
			} else if (uniform->base == 0U) {
				memcpy(&number, &word, 4U);
				precise = (double)number;
				integer = (GLint)number;
			} else if (uniform->base == 1U) {
				integer = (GLint)word;
				precise = (double)integer;
			} else {
				integer = (GLint)word;
				precise = (double)word;
			}

			/* Written as the kind asked for. */
			if (kind == PROGRAM_FLOATS) {
				((GLfloat *)params)[slot] = (GLfloat)precise;
			} else if (kind == PROGRAM_DOUBLES) {
				((double *)params)[slot] = precise;
			} else {
				((GLint *)params)[slot] = integer;
			}
		}
	}
}

/* Returns a program's named uniform block of an index, recording the error when there is none. */
static struct gles_block *
program_block(
	struct zegl_context *context,
	struct gles_program *program,
	GLuint index)
{
	/* An index among the blocks the link found. */
	if (index >= program->block_count) {
		gles_error(context, GL_INVALID_VALUE);
		return NULL;
	}

	/* Succeeded: the block. */
	return &program->blocks[index];
}

/* Reads one property glGetActiveUniformsiv asks for of a uniform; nonzero for a name that is not one. */
static int
program_uniform_property(
	const struct gles_uniform *uniform,
	GLenum pname,
	GLint *value)
{
	/* The property: a default-block uniform or sampler has no block, offset or strides (-1). */
	switch (pname) {
	case GL_UNIFORM_TYPE:
		*value = (GLint)uniform->type;
		return 0;
	case GL_UNIFORM_SIZE:
		*value = uniform->size;
		return 0;
	case GL_UNIFORM_NAME_LENGTH:
		*value = (GLint)strlen(uniform->name) + 1;
		if (uniform->size > 1)
			*value += 3;
		return 0;
	case GL_UNIFORM_BLOCK_INDEX:
		*value = uniform->block;
		return 0;
	case GL_UNIFORM_OFFSET:
		*value = -1;
		if (uniform->block >= 0)
			*value = (GLint)uniform->offset;
		return 0;
	case GL_UNIFORM_ARRAY_STRIDE:
		*value = -1;
		if (uniform->block >= 0)
			*value = (GLint)uniform->array_stride;
		return 0;
	case GL_UNIFORM_MATRIX_STRIDE:
		*value = -1;
		if (uniform->block >= 0)
			*value = (GLint)uniform->matrix_stride;
		return 0;
	case GL_UNIFORM_IS_ROW_MAJOR:
		*value = uniform->row_major;
		return 0;
	default:
		break;
	}

	/* Not a property. */
	return -1;
}

/* Copies a string into an application's buffer of a size, reporting the length copied. */
static void
program_copy_string(
	const char *text,
	GLsizei size,
	GLsizei *length,
	GLchar *out)
{
	size_t copied;

	/* Nothing fits in no buffer. */
	if (text == NULL)
		text = "";
	copied = 0U;
	if (size > 0 && out != NULL) {
		copied = strlen(text);
		if (copied > (size_t)size - 1U)
			copied = (size_t)size - 1U;
		memcpy(out, text, copied);
		out[copied] = '\0';
	}

	/* The length without the terminator. */
	if (length != NULL)
		*length = (GLsizei)copied;
}

/* Replaces a log with a copy of a text. */
static void
program_log(
	char **log,
	const char *text)
{
	char *copy;

	/* The copy; without memory the log is left empty. */
	copy = malloc(strlen(text) + 1U);
	if (copy != NULL)
		memcpy(copy, text, strlen(text) + 1U);
	free(*log);
	*log = copy;
}

/*
 * Keeps each GLSL stage's subroutines and subroutine uniforms for the API
 * (desktop GL 4.0), their hidden uniforms' places unknown until the
 * uniforms are merged.  Returns 0, or -1 with the log.
 */
static int
program_link_subroutines(
	struct gles_program *program,
	char *log)
{
	const struct gles_shader *shaders[GLES_STAGES];
	unsigned stage;

	/* The shaders by the compiler's stage numbers. */
	memset(shaders, 0, sizeof(shaders));
	shaders[GLSL_STAGE_VERTEX] = program->vertex;
	shaders[GLSL_STAGE_FRAGMENT] = program->fragment;
	shaders[GLSL_STAGE_GEOMETRY] = program->geometry;
	shaders[GLSL_STAGE_TESS_CONTROL] = program->tess_control;
	shaders[GLSL_STAGE_TESS_EVALUATION] = program->tess_evaluation;

	/* Each GLSL stage with subroutine uniforms or subroutines. */
	for (stage = 0U; stage < GLES_STAGES; stage++) {
		if (shaders[stage] == NULL || shaders[stage]->glsl == NULL)
			continue;
		if (glsl_subroutine_count(shaders[stage]->glsl) == 0U && glsl_subroutine_uniform_count(shaders[stage]->glsl) == 0U)
			continue;

		/* Its tables. */
		program->subroutines[stage] = program_stage_subroutines(shaders[stage]->glsl);
		if (program->subroutines[stage] == NULL) {
			(void)snprintf(log, PROGRAM_LOG, "out of memory for the subroutines\n");
			return -1;
		}
	}

	/* Succeeded: the subroutines are kept. */
	return 0;
}

/*
 * Makes the tables of a compiled stage's subroutines: the subroutines'
 * names by index, and each subroutine uniform's name, locations (in
 * declaration order), compatible subroutines and hidden uniform.
 * Returns them, or NULL without memory.
 */
static struct gles_subroutines *
program_stage_subroutines(
	const struct glsl_shader *shader)
{
	struct gles_subroutines *subroutines;
	struct gles_subroutine_uniform *uniform;
	const char *name;
	const char *hidden;
	unsigned index;
	unsigned size;
	unsigned count;

	/* The tables. */
	subroutines = calloc(1U, sizeof(*subroutines));
	if (subroutines == NULL)
		return NULL;
	subroutines->count = glsl_subroutine_count(shader);
	subroutines->uniform_count = glsl_subroutine_uniform_count(shader);
	subroutines->names = calloc(subroutines->count + 1U, sizeof(*subroutines->names));
	subroutines->uniforms = calloc(subroutines->uniform_count + 1U, sizeof(*subroutines->uniforms));
	if (subroutines->names == NULL || subroutines->uniforms == NULL) {
		program_subroutines_free(subroutines);
		return NULL;
	}

	/* The subroutines' names by index. */
	for (index = 0U; index < subroutines->count; index++) {
		name = glsl_subroutine_name(shader, index);
		subroutines->names[index] = strdup(name);
		if (subroutines->names[index] == NULL) {
			program_subroutines_free(subroutines);
			return NULL;
		}
	}

	/* Each uniform: its name, locations, compatible subroutines and hidden uniform (no place until the merge). */
	for (index = 0U; index < subroutines->uniform_count; index++) {
		uniform = &subroutines->uniforms[index];
		name = glsl_subroutine_uniform(shader, index, &hidden, &size);
		count = glsl_subroutine_compatible(shader, index, NULL, 0U);
		uniform->name = strdup(name);
		uniform->hidden = strdup(hidden);
		uniform->compatible = calloc(count + 1U, sizeof(*uniform->compatible));
		if (uniform->name == NULL || uniform->hidden == NULL || uniform->compatible == NULL) {
			program_subroutines_free(subroutines);
			return NULL;
		}
		uniform->compatible_count = glsl_subroutine_compatible(shader, index, uniform->compatible, count);
		uniform->location = subroutines->locations;
		uniform->size = size;
		uniform->offset = -1;
		subroutines->locations += size;
	}

	/* Succeeded: the tables. */
	return subroutines;
}

/* Frees a stage's subroutine tables (NULL: none). */
static void
program_subroutines_free(
	struct gles_subroutines *subroutines)
{
	unsigned index;

	/* None. */
	if (subroutines == NULL)
		return;

	/* The names, the uniforms' strings and lists, and the tables. */
	for (index = 0U; subroutines->names != NULL && index < subroutines->count; index++)
		free(subroutines->names[index]);
	for (index = 0U; subroutines->uniforms != NULL && index < subroutines->uniform_count; index++) {
		free(subroutines->uniforms[index].name);
		free(subroutines->uniforms[index].hidden);
		free(subroutines->uniforms[index].compatible);
	}
	free(subroutines->names);
	free(subroutines->uniforms);
	free(subroutines);
}

/*
 * Notes the place of a subroutine uniform's hidden uint in the block
 * when a uniform is one: returns 1 for one, 0 for any other uniform.
 */
static int
program_hidden_subroutine(
	struct gles_program *program,
	const struct gles_uniform *uniform)
{
	struct gles_subroutine_uniform *subroutine;
	unsigned stage;
	unsigned index;
	int differs;

	/* A name of the hidden uniforms' form. */
	differs = strncmp(uniform->name, "gl_ZedSubroutine", 16U);
	if (differs != 0)
		return 0;

	/* The stage's subroutine uniform of the name. */
	for (stage = 0U; stage < GLES_STAGES; stage++) {
		if (program->subroutines[stage] == NULL)
			continue;
		for (index = 0U; index < program->subroutines[stage]->uniform_count; index++) {
			subroutine = &program->subroutines[stage]->uniforms[index];
			differs = strcmp(subroutine->hidden, uniform->name);
			if (differs != 0)
				continue;

			/* Its place and stride. */
			subroutine->offset = (int32_t)uniform->offset;
			subroutine->stride = uniform->array_stride;
			return 1;
		}
	}

	/* Succeeded: a hidden uniform no stage lists (not listed either). */
	return 1;
}

/*
 * Finds the compiler's stage number of a shader type the subroutine
 * calls name.  Returns 0, or -1 with GL_INVALID_ENUM recorded.
 */
static int
program_subroutine_stage(
	struct zegl_context *context,
	GLenum shadertype,
	unsigned *stage)
{
	/* The shader types of the stages that have subroutines. */
	switch (shadertype) {
	case GL_VERTEX_SHADER:
		*stage = GLSL_STAGE_VERTEX;
		return 0;
	case GL_FRAGMENT_SHADER:
		*stage = GLSL_STAGE_FRAGMENT;
		return 0;
	case GL_GEOMETRY_SHADER:
		*stage = GLSL_STAGE_GEOMETRY;
		return 0;
	case GL_TESS_CONTROL_SHADER:
		*stage = GLSL_STAGE_TESS_CONTROL;
		return 0;
	case GL_TESS_EVALUATION_SHADER:
		*stage = GLSL_STAGE_TESS_EVALUATION;
		return 0;
	default:
		break;
	}

	/* Not a shader type of these. */
	gles_error(context, GL_INVALID_ENUM);
	return -1;
}

/*
 * Returns the subroutine tables of a linked program's stage, after the
 * shader type and the program are checked: an empty table for a stage
 * without subroutines, NULL with the error recorded (GL_INVALID_ENUM,
 * GL_INVALID_VALUE, GL_INVALID_OPERATION for a shader or a program not
 * linked).
 */
static const struct gles_subroutines *
program_stage_of(
	struct zegl_context *context,
	GLuint name,
	GLenum shadertype)
{
	static const struct gles_subroutines none;
	struct gles_program *program;
	unsigned stage;
	int status;

	/* A shader type, a program, linked. */
	status = program_subroutine_stage(context, shadertype, &stage);
	if (status != 0)
		return NULL;
	program = program_get(context, name);
	if (program == NULL)
		return NULL;
	if (!program->linked) {
		gles_error(context, GL_INVALID_OPERATION);
		return NULL;
	}

	/* The stage's tables, or none. */
	if (program->subroutines[stage] == NULL)
		return &none;

	/* Succeeded: the stage's tables. */
	return program->subroutines[stage];
}

/* Returns the subroutine uniform a location of a stage's tables is of (NULL for none). */
static const struct gles_subroutine_uniform *
program_subroutine_at(
	const struct gles_subroutines *subroutines,
	unsigned location)
{
	const struct gles_subroutine_uniform *uniform;
	unsigned index;

	/* The uniform whose locations hold it. */
	for (index = 0U; index < subroutines->uniform_count; index++) {
		uniform = &subroutines->uniforms[index];
		if (location >= uniform->location && location < uniform->location + uniform->size)
			return uniform;
	}

	/* No uniform has it. */
	return NULL;
}

/* Returns the longest name with its terminator of a stage's subroutine uniforms (or subroutines), 0 for none. */
static GLint
program_subroutine_max_length(
	const struct gles_subroutines *subroutines,
	int uniforms)
{
	size_t longest;
	size_t length;
	unsigned index;

	/* The uniforms' names, or the subroutines'. */
	longest = 0U;
	if (uniforms) {
		for (index = 0U; index < subroutines->uniform_count; index++) {
			length = strlen(subroutines->uniforms[index].name) + 1U;
			if (length > longest)
				longest = length;
		}
	} else {
		for (index = 0U; index < subroutines->count; index++) {
			length = strlen(subroutines->names[index]) + 1U;
			if (length > longest)
				longest = length;
		}
	}

	/* Succeeded: the longest. */
	return (GLint)longest;
}

/* Reports whether a sampler type reads a buffer texture (desktop GL's samplerBuffer of each kind), which is a texel buffer. */
static int
program_buffer_sampler(
	GLenum type)
{
	/* The three kinds of buffer samplers. */
	switch (type) {
	case GL_SAMPLER_BUFFER:
	case GL_INT_SAMPLER_BUFFER:
	case GL_UNSIGNED_INT_SAMPLER_BUFFER:
		return 1;
	default:
		break;
	}

	/* A sampler of an image. */
	return 0;
}

/* Reports whether a fragment shader's SPIR-V decorates anything Flat (an input taking the provoking vertex's value). */
static int
program_flat_inputs(
	const uint32_t *code,
	size_t words)
{
	size_t at;
	uint32_t length;

	/* Each instruction after the header: an OpDecorate (71) of Flat (14). */
	for (at = 5U; at < words; at += length) {
		length = code[at] >> 16;
		if (length == 0U)
			return 0;
		if ((code[at] & 0xffffU) == 71U && length >= 3U && code[at + 2U] == 14U)
			return 1;
	}

	/* None is flat. */
	return 0;
}

/*
 * Returns the program of a name for a program interface query, recording
 * the error when the name is not one or the context is of OpenGL ES 3.0,
 * which has no such call (GL_INVALID_OPERATION).
 */
static struct gles_program *
program_resource_program(
	struct zegl_context *context,
	GLuint name)
{
	struct gles_state *state;
	struct gles_program *program;

	/* A context of OpenGL ES 3.1 (or desktop GL) with its state. */
	state = gles_state(context);
	if (state == NULL)
		return NULL;
	if (gles_fixed == NULL && !state->compute) {
		gles_error(context, GL_INVALID_OPERATION);
		return NULL;
	}

	/* Succeeded: the program of the name. */
	program = program_get(context, name);
	return program;
}

/* Returns the PROGRAM_IF_* bit of a program interface, 0 for a name that is not one. */
static unsigned
program_interface_bit(
	GLenum interface)
{
	/* The interfaces of OpenGL ES 3.1 section 7.3.1. */
	switch (interface) {
	case GL_UNIFORM:
		return PROGRAM_IF_UNIFORM;
	case GL_UNIFORM_BLOCK:
		return PROGRAM_IF_UNIFORM_BLOCK;
	case GL_ATOMIC_COUNTER_BUFFER:
		return PROGRAM_IF_ATOMIC;
	case GL_PROGRAM_INPUT:
		return PROGRAM_IF_INPUT;
	case GL_PROGRAM_OUTPUT:
		return PROGRAM_IF_OUTPUT;
	case GL_TRANSFORM_FEEDBACK_VARYING:
		return PROGRAM_IF_CAPTURE;
	case GL_BUFFER_VARIABLE:
		return PROGRAM_IF_BUFFER_VARIABLE;
	case GL_SHADER_STORAGE_BLOCK:
		return PROGRAM_IF_STORAGE_BLOCK;
	default:
		break;
	}

	/* Not an interface. */
	return 0U;
}

/* Returns how many resources a program's interface lists (atomic counter buffers: none, as atomic counters are not offered). */
static unsigned
program_resource_count(
	const struct gles_program *program,
	unsigned interface)
{
	/* The list of each interface. */
	switch (interface) {
	case PROGRAM_IF_UNIFORM:
		return program->uniform_count;
	case PROGRAM_IF_UNIFORM_BLOCK:
		return program->block_count;
	case PROGRAM_IF_INPUT:
		/* A vertex shader's inputs are the attributes (as glGetActiveAttrib lists them). */
		if ((program->linked_stages & GL_VERTEX_SHADER_BIT) != 0U)
			return program->attribute_count;
		return program->program_input_count;
	case PROGRAM_IF_OUTPUT:
		return program->program_output_count;
	case PROGRAM_IF_CAPTURE:
		return program->capture_count;
	case PROGRAM_IF_BUFFER_VARIABLE:
		return program->buffer_variable_count;
	case PROGRAM_IF_STORAGE_BLOCK:
		return program->storage_count;
	default:
		break;
	}

	/* Atomic counter buffers. */
	return 0U;
}

/* Writes the name of a resource of a program's interface ("[0]" after an array of a basic type's). */
static void
program_resource_name(
	const struct gles_program *program,
	unsigned interface,
	unsigned index,
	char *out)
{
	const char *base;
	GLint size;

	/* The name, and the elements of a variable that has them. */
	base = "";
	size = 1;
	switch (interface) {
	case PROGRAM_IF_UNIFORM:
		base = program->uniforms[index].name;
		size = program->uniforms[index].size;
		break;
	case PROGRAM_IF_UNIFORM_BLOCK:
		base = program->blocks[index].name;
		break;
	case PROGRAM_IF_INPUT:
		/* A vertex shader's attributes, or another stage's inputs. */
		if ((program->linked_stages & GL_VERTEX_SHADER_BIT) != 0U) {
			base = program->attributes[index].name;
			size = program->attributes[index].size;
		} else {
			base = program->program_inputs[index].name;
			size = program->program_inputs[index].size;
		}
		break;
	case PROGRAM_IF_OUTPUT:
		base = program->program_outputs[index].name;
		size = program->program_outputs[index].size;
		break;
	case PROGRAM_IF_CAPTURE:
		base = program->captures[index].name;
		break;
	case PROGRAM_IF_BUFFER_VARIABLE:
		base = program->buffer_variables[index].name;
		break;
	case PROGRAM_IF_STORAGE_BLOCK:
		base = program->storages[index].name;
		break;
	default:
		break;
	}

	/* Succeeded: the name, an array's with its first element. */
	(void)snprintf(out, PROGRAM_RESOURCE_NAME, "%s", base);
	if (size > 1)
		(void)snprintf(out, PROGRAM_RESOURCE_NAME, "%s[0]", base);
}

/* Returns the index of the resource of a program's interface a name names (also an array's without "[0]"), UINT32_MAX for none. */
static unsigned
program_resource_find(
	const struct gles_program *program,
	unsigned interface,
	const char *name)
{
	char resource[PROGRAM_RESOURCE_NAME];
	size_t length;
	unsigned count;
	unsigned index;
	int differs;

	/* No name names nothing. */
	if (name == NULL)
		return UINT32_MAX;

	/* Each resource's name, as it is or as the name with "[0]". */
	length = strlen(name);
	count = program_resource_count(program, interface);
	for (index = 0U; index < count; index++) {
		program_resource_name(program, interface, index, resource);
		differs = strcmp(resource, name);
		if (differs == 0)
			return index;
		if (interface == PROGRAM_IF_CAPTURE)
			continue;
		differs = strncmp(resource, name, length);
		if (differs == 0 && strcmp(resource + length, "[0]") == 0)
			return index;
	}

	/* None. */
	return UINT32_MAX;
}

/*
 * Returns how many active variables a block (or atomic counter buffer)
 * of a program's interface has, writing their indices when indices is
 * not NULL; 0 for a resource of another interface.
 */
static GLint
program_resource_variables(
	const struct gles_program *program,
	unsigned interface,
	unsigned index,
	GLint *indices)
{
	GLint found;
	unsigned variable;

	/* A uniform block's members (an array's elements share the first element's). */
	found = 0;
	if (interface == PROGRAM_IF_UNIFORM_BLOCK) {
		for (variable = 0U; variable < program->uniform_count; variable++) {
			if (program->uniforms[variable].block != (GLint)program->blocks[index].members)
				continue;
			if (indices != NULL)
				indices[found] = (GLint)variable;
			found++;
		}
	}

	/* A shader storage block's buffer variables. */
	if (interface == PROGRAM_IF_STORAGE_BLOCK) {
		for (variable = 0U; variable < program->buffer_variable_count; variable++) {
			if (program->buffer_variables[variable].block != (GLint)index)
				continue;
			if (indices != NULL)
				indices[found] = (GLint)variable;
			found++;
		}
	}

	/* Succeeded: how many. */
	return found;
}

/*
 * Reports whether a property is one a program interface has (OpenGL ES
 * 3.1's table 7.2): 1 when it is, 0 when the interface does not have it,
 * -1 when it is not a property.
 */
static int
program_property_allowed(
	GLenum property,
	unsigned interface)
{
	unsigned allowed;

	/* The interfaces that have each property. */
	switch (property) {
	case GL_ACTIVE_VARIABLES:
	case GL_BUFFER_BINDING:
	case GL_NUM_ACTIVE_VARIABLES:
	case GL_BUFFER_DATA_SIZE:
		allowed = PROGRAM_IF_ATOMIC | PROGRAM_IF_STORAGE_BLOCK | PROGRAM_IF_UNIFORM_BLOCK;
		break;
	case GL_ARRAY_SIZE:
	case GL_TYPE:
		allowed = PROGRAM_IF_BUFFER_VARIABLE | PROGRAM_IF_INPUT | PROGRAM_IF_OUTPUT | PROGRAM_IF_CAPTURE | PROGRAM_IF_UNIFORM;
		break;
	case GL_ARRAY_STRIDE:
	case GL_BLOCK_INDEX:
	case GL_IS_ROW_MAJOR:
	case GL_MATRIX_STRIDE:
	case GL_OFFSET:
		allowed = PROGRAM_IF_BUFFER_VARIABLE | PROGRAM_IF_UNIFORM;
		break;
	case GL_ATOMIC_COUNTER_BUFFER_INDEX:
		allowed = PROGRAM_IF_UNIFORM;
		break;
	case GL_LOCATION:
		allowed = PROGRAM_IF_INPUT | PROGRAM_IF_OUTPUT | PROGRAM_IF_UNIFORM;
		break;
	case GL_NAME_LENGTH:
		allowed = PROGRAM_IF_ALL & ~PROGRAM_IF_ATOMIC;
		break;
	case GL_REFERENCED_BY_VERTEX_SHADER:
	case GL_REFERENCED_BY_FRAGMENT_SHADER:
	case GL_REFERENCED_BY_COMPUTE_SHADER:
		allowed = PROGRAM_IF_ALL & ~PROGRAM_IF_CAPTURE;
		break;
	case GL_TOP_LEVEL_ARRAY_SIZE:
	case GL_TOP_LEVEL_ARRAY_STRIDE:
		allowed = PROGRAM_IF_BUFFER_VARIABLE;
		break;
	default:
		return -1;
	}

	/* Whether the interface is one of them. */
	if ((allowed & interface) == 0U)
		return 0;

	/* Succeeded: allowed. */
	return 1;
}

/*
 * Writes the values of a property of a resource of a program's interface
 * (the caller checked the interface has it); returns how many (several
 * for GL_ACTIVE_VARIABLES, one for the others).
 */
static unsigned
program_resource_property(
	const struct gles_program *program,
	unsigned interface,
	unsigned index,
	GLenum property,
	GLint *values)
{
	char resource[PROGRAM_RESOURCE_NAME];
	GLbitfield stages;
	GLint count;

	/* The properties every interface's resource has the same way. */
	switch (property) {
	case GL_NAME_LENGTH:
		program_resource_name(program, interface, index, resource);
		values[0] = (GLint)strlen(resource) + 1;
		return 1U;
	case GL_ACTIVE_VARIABLES:
		count = program_resource_variables(program, interface, index, values);
		return (unsigned)count;
	case GL_NUM_ACTIVE_VARIABLES:
		values[0] = program_resource_variables(program, interface, index, NULL);
		return 1U;
	case GL_REFERENCED_BY_VERTEX_SHADER:
	case GL_REFERENCED_BY_FRAGMENT_SHADER:
	case GL_REFERENCED_BY_COMPUTE_SHADER:
		/* Whether the stage reads it. */
		stages = program_resource_stages(program, interface, index);
		values[0] = 0;
		if (property == GL_REFERENCED_BY_VERTEX_SHADER && (stages & GL_VERTEX_SHADER_BIT) != 0U)
			values[0] = 1;
		if (property == GL_REFERENCED_BY_FRAGMENT_SHADER && (stages & GL_FRAGMENT_SHADER_BIT) != 0U)
			values[0] = 1;
		if (property == GL_REFERENCED_BY_COMPUTE_SHADER && (stages & GL_COMPUTE_SHADER_BIT) != 0U)
			values[0] = 1;
		return 1U;
	default:
		break;
	}

	/* Then each interface's own. */
	values[0] = 0;
	switch (interface) {
	case PROGRAM_IF_UNIFORM:
		program_uniform_resource(&program->uniforms[index], property, values);
		break;
	case PROGRAM_IF_UNIFORM_BLOCK:
		/* Its binding point and its std140 size. */
		values[0] = (GLint)program->blocks[index].buffer_binding;
		if (property == GL_BUFFER_DATA_SIZE)
			values[0] = (GLint)program->blocks[index].size;
		break;
	case PROGRAM_IF_STORAGE_BLOCK:
		/* Its binding point, and its size with one element of a run-time array. */
		values[0] = (GLint)program->storages[index].binding;
		if (property == GL_BUFFER_DATA_SIZE)
			values[0] = (GLint)(program->storages[index].size + program->storages[index].array_stride);
		break;
	case PROGRAM_IF_INPUT:
	case PROGRAM_IF_OUTPUT:
		program_variable_resource(program, interface, index, property, values);
		break;
	case PROGRAM_IF_CAPTURE:
		/* A captured output's type and elements. */
		values[0] = (GLint)program->captures[index].type;
		if (property == GL_ARRAY_SIZE)
			values[0] = program->captures[index].size;
		break;
	case PROGRAM_IF_BUFFER_VARIABLE:
		program_buffer_resource(&program->buffer_variables[index], property, values);
		break;
	default:
		break;
	}

	/* Succeeded: one value. */
	return 1U;
}

/* Returns the stages that read a resource of a program's interface (GL's shader bits). */
static GLbitfield
program_resource_stages(
	const struct gles_program *program,
	unsigned interface,
	unsigned index)
{
	const struct gles_uniform *uniform;
	unsigned bits;

	/* A uniform of the default block, by the stages it was merged from; a block's member by its block's. */
	bits = 0U;
	switch (interface) {
	case PROGRAM_IF_UNIFORM:
		uniform = &program->uniforms[index];
		if (uniform->block < 0 || (unsigned)uniform->block >= program->block_count)
			return uniform->stages;
		bits = program->blocks[uniform->block].stages;
		break;
	case PROGRAM_IF_UNIFORM_BLOCK:
		bits = program->blocks[index].stages;
		break;
	case PROGRAM_IF_INPUT:
		/* The first stage's. */
		if ((program->linked_stages & GL_VERTEX_SHADER_BIT) != 0U)
			return GL_VERTEX_SHADER_BIT;
		return program->linked_stages & GL_FRAGMENT_SHADER_BIT;
	case PROGRAM_IF_OUTPUT:
		/* The last stage's. */
		if ((program->linked_stages & GL_FRAGMENT_SHADER_BIT) != 0U)
			return GL_FRAGMENT_SHADER_BIT;
		return program->linked_stages & GL_VERTEX_SHADER_BIT;
	case PROGRAM_IF_BUFFER_VARIABLE:
	case PROGRAM_IF_STORAGE_BLOCK:
		/* Only compute shaders have storage blocks. */
		return GL_COMPUTE_SHADER_BIT;
	default:
		return 0U;
	}

	/* A block's stages are the compiler's (bit 0 vertex, 1 fragment, GLSL_STAGE_COMPUTE compute). */
	return program_block_stages(bits);
}

/* Returns GL's shader bits of a block's stages as the compiler numbers them. */
static GLbitfield
program_block_stages(
	unsigned bits)
{
	GLbitfield stages;

	/* Each stage's bit. */
	stages = 0U;
	if ((bits & (1U << GLSL_STAGE_VERTEX)) != 0U)
		stages |= GL_VERTEX_SHADER_BIT;
	if ((bits & (1U << GLSL_STAGE_FRAGMENT)) != 0U)
		stages |= GL_FRAGMENT_SHADER_BIT;
	if ((bits & (1U << GLSL_STAGE_COMPUTE)) != 0U)
		stages |= GL_COMPUTE_SHADER_BIT;

	/* Succeeded: the bits. */
	return stages;
}

/* Writes a property of a uniform (GL_UNIFORM): a default-block uniform or sampler has no block, offset or strides. */
static void
program_uniform_resource(
	const struct gles_uniform *uniform,
	GLenum property,
	GLint *values)
{
	/* The property. */
	switch (property) {
	case GL_TYPE:
		values[0] = (GLint)uniform->type;
		return;
	case GL_ARRAY_SIZE:
		values[0] = uniform->size;
		return;
	case GL_LOCATION:
		values[0] = uniform->location;
		return;
	case GL_BLOCK_INDEX:
		values[0] = uniform->block;
		return;
	case GL_ATOMIC_COUNTER_BUFFER_INDEX:
		values[0] = -1;
		return;
	case GL_IS_ROW_MAJOR:
		values[0] = 0;
		if (uniform->block >= 0 && uniform->columns > 1U && uniform->row_major)
			values[0] = 1;
		return;
	default:
		break;
	}

	/* The rest are a block member's place: -1 for a uniform not backed by a buffer object. */
	values[0] = -1;
	if (uniform->block < 0)
		return;
	values[0] = (GLint)uniform->offset;
	if (property == GL_ARRAY_STRIDE)
		values[0] = (GLint)uniform->array_stride;
	if (property == GL_MATRIX_STRIDE)
		values[0] = (GLint)uniform->matrix_stride;
}

/* Writes a property of a program's input or output (GL_PROGRAM_INPUT, GL_PROGRAM_OUTPUT). */
static void
program_variable_resource(
	const struct gles_program *program,
	unsigned interface,
	unsigned index,
	GLenum property,
	GLint *values)
{
	const struct gles_spirv_variable *variable;

	/* A vertex shader's attribute. */
	if (interface == PROGRAM_IF_INPUT && (program->linked_stages & GL_VERTEX_SHADER_BIT) != 0U) {
		values[0] = (GLint)program->attributes[index].type;
		if (property == GL_ARRAY_SIZE)
			values[0] = program->attributes[index].size;
		if (property == GL_LOCATION)
			values[0] = (GLint)program->attributes[index].location;
		return;
	}

	/* Another stage's input, or an output. */
	variable = &program->program_outputs[index];
	if (interface == PROGRAM_IF_INPUT)
		variable = &program->program_inputs[index];
	values[0] = (GLint)variable->type;
	if (property == GL_ARRAY_SIZE)
		values[0] = variable->size;

	/* Only a fragment shader's outputs have locations here (a varying's is the link's own). */
	if (property != GL_LOCATION)
		return;
	values[0] = -1;
	if (interface == PROGRAM_IF_OUTPUT && (program->linked_stages & GL_FRAGMENT_SHADER_BIT) != 0U)
		values[0] = (GLint)variable->location;
}

/* Writes a property of a buffer variable (GL_BUFFER_VARIABLE). */
static void
program_buffer_resource(
	const struct gles_buffer_variable *variable,
	GLenum property,
	GLint *values)
{
	/* The property. */
	switch (property) {
	case GL_TYPE:
		values[0] = (GLint)variable->type;
		return;
	case GL_ARRAY_SIZE:
		values[0] = variable->size;
		return;
	case GL_OFFSET:
		values[0] = variable->offset;
		return;
	case GL_ARRAY_STRIDE:
		values[0] = variable->array_stride;
		return;
	case GL_MATRIX_STRIDE:
		values[0] = variable->matrix_stride;
		return;
	case GL_BLOCK_INDEX:
		values[0] = variable->block;
		return;
	case GL_TOP_LEVEL_ARRAY_SIZE:
		values[0] = variable->top_size;
		return;
	case GL_TOP_LEVEL_ARRAY_STRIDE:
		values[0] = variable->top_stride;
		return;
	default:
		break;
	}

	/* Column-major only. */
	values[0] = 0;
}

/*
 * Links a separable program of a vertex or a fragment shader alone
 * (OpenGL ES 3.1): with a stand-in for the other stage made from it (a
 * fragment shader that does nothing, or a vertex shader that writes the
 * fragment shader's inputs), its own stage the one the API lists.
 * Returns 0, or -1 with the log.
 */
static int
program_link_alone(
	struct gles_state *state,
	struct gles_program *program,
	char *log)
{
	struct gles_shader *own;
	struct gles_shader *stand_in;
	char *source;
	GLenum type;
	int status;

	/* The stage it has, compiled from GLSL. */
	own = program->vertex;
	type = GL_FRAGMENT_SHADER;
	if (own == NULL) {
		own = program->fragment;
		type = GL_VERTEX_SHADER;
	}
	if (!own->compiled || own->glsl == NULL) {
		(void)snprintf(log, PROGRAM_LOG, "the shader of the separable program is not compiled GLSL\n");
		return -1;
	}

	/* The stand-in for the other stage. */
	source = glsl_stand_in_source(own->glsl);
	if (source == NULL) {
		(void)snprintf(log, PROGRAM_LOG, "no stand-in could be made for the stage the separable program does not have\n");
		return -1;
	}
	stand_in = program_hidden_shader(gles_context(), type, source);
	free(source);
	if (stand_in == NULL) {
		(void)snprintf(log, PROGRAM_LOG, "the stand-in for the stage the separable program does not have did not compile\n");
		return -1;
	}

	/* Linked as a program of both stages, which lists its own stage's interface. */
	program->linked_stages = GL_VERTEX_SHADER_BIT;
	if (type == GL_VERTEX_SHADER)
		program->linked_stages = GL_FRAGMENT_SHADER_BIT;
	if (type == GL_FRAGMENT_SHADER) {
		program->fragment = stand_in;
	} else {
		program->vertex = stand_in;
	}
	status = program_link(state, program, log);

	/* The stand-in goes again. */
	if (type == GL_FRAGMENT_SHADER) {
		program->fragment = NULL;
	} else {
		program->vertex = NULL;
	}
	gles_shader_release(stand_in);

	/* Reports why the link failed. */
	if (status != 0)
		return -1;

	/* Succeeded: the stage is linked. */
	return 0;
}

/* Makes and compiles a shader of no name from a source, for a link no application sees; NULL when it does not compile. */
static struct gles_shader *
program_hidden_shader(
	struct zegl_context *context,
	GLenum type,
	const char *source)
{
	struct gles_shader *shader;

	/* The shader with a copy of the source. */
	shader = calloc(1U, sizeof(*shader));
	if (shader == NULL)
		return NULL;
	shader->kind = GLES_KIND_SHADER;
	shader->type = type;
	shader->source = strdup(source);
	if (shader->source == NULL) {
		free(shader);
		return NULL;
	}

	/* Compiled. */
	program_compile(context, shader);
	if (!shader->compiled || shader->glsl == NULL) {
		gles_shader_release(shader);
		return NULL;
	}

	/* Succeeded: the shader. */
	return shader;
}

/* Keeps the sources of a separable program's linked vertex and fragment shaders (a program pipeline links them again). */
static void
program_keep_sources(
	struct gles_program *program)
{
	/* The vertex shader's. */
	free(program->stage_sources[0]);
	program->stage_sources[0] = NULL;
	if ((program->linked_stages & GL_VERTEX_SHADER_BIT) != 0U && program->vertex != NULL && program->vertex->source != NULL)
		program->stage_sources[0] = strdup(program->vertex->source);

	/* The fragment shader's. */
	free(program->stage_sources[1]);
	program->stage_sources[1] = NULL;
	if ((program->linked_stages & GL_FRAGMENT_SHADER_BIT) != 0U && program->fragment != NULL && program->fragment->source != NULL)
		program->stage_sources[1] = strdup(program->fragment->source);
}

/* Returns the program glUniform* writes: glProgramUniform*'s, the current one, or the bound pipeline's active one (NULL: none). */
static struct gles_program *
program_uniform_program(
	struct gles_state *state)
{
	/* glProgramUniform*'s. */
	if (state->uniform_program != NULL)
		return state->uniform_program;

	/* The current program. */
	if (state->program != NULL)
		return state->program;

	/* The pipeline's active program. */
	if (state->pipeline != NULL)
		return state->pipeline->active;

	/* None. */
	return NULL;
}

/*
 * Links a program pipeline object's vertex stage's shader and fragment
 * stage's shader (a stand-in without one) again into one program of no
 * name, with the vertex program's attribute locations and captures and
 * the fragment program's output locations; the fragment shader's
 * uniforms and blocks of names the vertex program also has are renamed.
 * Returns the program, or NULL when a shader is not GLSL or the link
 * fails.
 */
static struct gles_program *
program_combine(
	struct zegl_context *context,
	struct gles_state *state,
	struct gles_program *vertex,
	struct gles_program *fragment)
{
	struct gles_program *combined;
	struct gles_shader *vertex_shader;
	struct gles_shader *fragment_shader;
	char log[PROGRAM_LOG];
	char *source;
	int status;

	/* The vertex stage's shader again. */
	if (vertex->stage_sources[0] == NULL)
		return NULL;
	vertex_shader = program_hidden_shader(context, GL_VERTEX_SHADER, vertex->stage_sources[0]);
	if (vertex_shader == NULL)
		return NULL;

	/* The fragment stage's, its names the vertex program has renamed, or a stand-in. */
	source = NULL;
	if (fragment != NULL && fragment->stage_sources[1] != NULL)
		source = program_renamed_source(fragment->stage_sources[1], vertex, fragment);
	if (fragment == NULL)
		source = glsl_stand_in_source(vertex_shader->glsl);
	fragment_shader = NULL;
	if (source != NULL)
		fragment_shader = program_hidden_shader(context, GL_FRAGMENT_SHADER, source);
	free(source);
	if (fragment_shader == NULL) {
		gles_shader_release(vertex_shader);
		return NULL;
	}

	/* The program, with the stages' programs' attribute locations, captures and output locations. */
	combined = calloc(1U, sizeof(*combined));
	if (combined == NULL) {
		gles_shader_release(vertex_shader);
		gles_shader_release(fragment_shader);
		return NULL;
	}
	combined->kind = GLES_KIND_PROGRAM;
	combined->zed_fragment = -1;
	combined->zed_images = -1;
	combined->depth_range[0] = -1;
	combined->depth_range[1] = -1;
	combined->depth_range[2] = -1;
	memcpy(combined->bound_names, vertex->bound_names, sizeof(combined->bound_names));
	memcpy(combined->bound_locations, vertex->bound_locations, sizeof(combined->bound_locations));
	combined->bound_count = vertex->bound_count;
	memcpy(combined->feedback_names, vertex->feedback_names, sizeof(combined->feedback_names));
	combined->feedback_count = vertex->feedback_count;
	combined->feedback_mode = vertex->feedback_mode;
	if (fragment != NULL) {
		memcpy(combined->frag_bound_names, fragment->frag_bound_names, sizeof(combined->frag_bound_names));
		memcpy(combined->frag_bound_locations, fragment->frag_bound_locations, sizeof(combined->frag_bound_locations));
		memcpy(combined->frag_bound_indexes, fragment->frag_bound_indexes, sizeof(combined->frag_bound_indexes));
		combined->frag_bound_count = fragment->frag_bound_count;
	}

	/* The link, then the shaders go (the program keeps what it made of them). */
	combined->vertex = vertex_shader;
	combined->fragment = fragment_shader;
	log[0] = '\0';
	status = program_link(state, combined, log);
	combined->vertex = NULL;
	combined->fragment = NULL;
	gles_shader_release(vertex_shader);
	gles_shader_release(fragment_shader);
	if (status != 0) {
		gles_report(log, -1);
		program_unlink(state, combined);
		free(combined);
		return NULL;
	}

	/* Succeeded: the program, linked. */
	combined->linked = 1;
	return combined;
}

/*
 * Returns a copy of a fragment shader's source in which each uniform's
 * and uniform block's name that the vertex program also has is renamed
 * (a #define after the #version line), so that the two stages' uniforms
 * stay apart when they are linked together; NULL without memory.
 */
static char *
program_renamed_source(
	const char *source,
	const struct gles_program *vertex,
	const struct gles_program *fragment)
{
	char defines[PROGRAM_LOG];
	char line[GLES_NAME * 2U + 32U];
	const char *version;
	const char *name;
	const char *found;
	char *renamed;
	size_t length;
	size_t head;
	size_t used;
	size_t bytes;
	unsigned count;
	unsigned index;
	int builtin;
	int taken;

	/* Each name of the fragment program's uniforms of the default block and blocks that the vertex program has. */
	defines[0] = '\0';
	used = 0U;
	count = fragment->uniform_count + fragment->block_count;
	for (index = 0U; index < count; index++) {
		/* A uniform of the default block, or a block. */
		if (index < fragment->uniform_count) {
			if (fragment->uniforms[index].block >= 0)
				continue;
			name = fragment->uniforms[index].name;
		} else {
			name = fragment->blocks[index - fragment->uniform_count].name;
		}

		/* Its name up to a member or an element, not a built-in's, that the vertex program has. */
		length = strcspn(name, ".[");
		builtin = strncmp(name, "gl_", 3U);
		if (length == 0U || builtin == 0)
			continue;
		taken = program_name_taken(vertex, name, length);
		if (!taken)
			continue;

		/* Renamed once. */
		(void)snprintf(line, sizeof(line), "#define %.*s zed_fs_%.*s\n", (int)length, name, (int)length, name);
		bytes = strlen(line);
		found = strstr(defines, line);
		if (found != NULL || used + bytes >= sizeof(defines))
			continue;
		memcpy(defines + used, line, bytes + 1U);
		used += bytes;
	}

	/* Where the defines go: after the #version line (which comes first), or first without one. */
	head = 0U;
	version = strstr(source, "#version");
	if (version != NULL) {
		head = (size_t)(version - source) + strcspn(version, "\n");
		if (source[head] == '\n')
			head++;
	}

	/* The source with them. */
	bytes = strlen(source);
	renamed = malloc(bytes + used + 2U);
	if (renamed == NULL)
		return NULL;
	memcpy(renamed, source, head);
	length = head;
	if (head != 0U && source[head - 1U] != '\n') {
		renamed[length] = '\n';
		length++;
	}
	memcpy(renamed + length, defines, used);
	memcpy(renamed + length + used, source + head, bytes - head + 1U);

	/* Succeeded: the source. */
	return renamed;
}

/* Reports whether a program has a uniform or a uniform block whose name (up to a member or an element) is the one given. */
static int
program_name_taken(
	const struct gles_program *program,
	const char *name,
	size_t length)
{
	const char *other;
	size_t other_length;
	unsigned index;
	int differs;

	/* Each uniform's name. */
	for (index = 0U; index < program->uniform_count; index++) {
		other = program->uniforms[index].name;
		other_length = strcspn(other, ".[");
		if (other_length != length)
			continue;
		differs = strncmp(other, name, length);
		if (differs == 0)
			return 1;
	}

	/* Each block's name. */
	for (index = 0U; index < program->block_count; index++) {
		other = program->blocks[index].name;
		other_length = strcspn(other, ".[");
		if (other_length != length)
			continue;
		differs = strncmp(other, name, length);
		if (differs == 0)
			return 1;
	}

	/* None has it. */
	return 0;
}

/*
 * Finds where each uniform and block of the program a program pipeline
 * object draws with takes its value or binding from: a renamed one from
 * the fragment stage's program, another from the vertex stage's when its
 * vertex shader reads it, else the fragment stage's.  Returns 0, or -1
 * when there is no memory.
 */
static int
program_combine_sources(
	struct gles_pipeline_object *pipeline,
	const struct gles_program *combined)
{
	struct gles_pipeline_source *source;
	struct gles_program *program;
	const char *name;
	GLbitfield stages;
	unsigned count;
	unsigned index;
	unsigned other;
	int renamed;
	int differs;

	/* The table, a source a uniform. */
	pipeline->uniform_sources = calloc(combined->uniform_count + 1U, sizeof(*pipeline->uniform_sources));
	if (pipeline->uniform_sources == NULL)
		return -1;

	/* Each uniform, then each block. */
	for (index = 0U; index < combined->uniform_count + combined->block_count; index++) {
		/* Its name and the stages that read it. */
		if (index < combined->uniform_count) {
			name = combined->uniforms[index].name;
			stages = combined->uniforms[index].stages;
			source = &pipeline->uniform_sources[index];
		} else {
			name = combined->blocks[index - combined->uniform_count].name;
			stages = program_block_stages(combined->blocks[index - combined->uniform_count].stages);
			source = &pipeline->block_sources[index - combined->uniform_count];
		}

		/* The stage's program it comes from. */
		program = pipeline->fragment;
		renamed = strncmp(name, "zed_fs_", 7U);
		if (renamed == 0) {
			name += 7;
		} else if ((stages & GL_VERTEX_SHADER_BIT) != 0U || program == NULL) {
			program = pipeline->vertex;
		}

		/* The uniform or block of the name there. */
		count = program->uniform_count;
		if (index >= combined->uniform_count)
			count = program->block_count;
		for (other = 0U; other < count; other++) {
			if (index < combined->uniform_count) {
				differs = strcmp(program->uniforms[other].name, name);
			} else {
				differs = strcmp(program->blocks[other].name, name);
			}
			if (differs != 0)
				continue;
			source->program = program;
			source->index = other;
			break;
		}
	}

	/* Succeeded: the sources are found. */
	return 0;
}

/*
 * Gives the program a program pipeline object draws with its stages'
 * programs' values: each uniform of the default block's elements, each
 * sampler's units, and each block's binding point.
 */
static void
program_combine_values(
	const struct gles_pipeline_object *pipeline)
{
	const struct gles_pipeline_source *source;
	const struct gles_uniform *from;
	struct gles_program *combined;
	struct gles_uniform *to;
	size_t bytes;
	GLint element;
	unsigned index;

	/* Each uniform with a source. */
	combined = pipeline->combined;
	for (index = 0U; index < combined->uniform_count; index++) {
		source = &pipeline->uniform_sources[index];
		if (source->program == NULL)
			continue;
		from = &source->program->uniforms[source->index];
		to = &combined->uniforms[index];

		/* A sampler's units. */
		if (to->sampler) {
			memcpy(to->units, from->units, sizeof(to->units));
			continue;
		}

		/* Only the default block's values are the program's (a block's are in a buffer). */
		if (to->block >= 0 || from->block >= 0 || to->size != from->size)
			continue;
		if (combined->uniform_data == NULL || source->program->uniform_data == NULL)
			continue;

		/* An element's bytes: a matrix's columns, or a vector's components. */
		bytes = (size_t)to->components * 4U;
		if (to->base == PROGRAM_BASE_DOUBLE)
			bytes = (size_t)to->components * 8U;
		if (to->columns > 1U)
			bytes = (size_t)to->columns * to->matrix_stride;

		/* Each element. */
		for (element = 0; element < to->size; element++) {
			memcpy(combined->uniform_data + to->offset + (uint32_t)element * to->array_stride,
			       source->program->uniform_data + from->offset + (uint32_t)element * from->array_stride,
			       bytes);
		}
	}

	/* Each block's binding point. */
	for (index = 0U; index < combined->block_count; index++) {
		source = &pipeline->block_sources[index];
		if (source->program == NULL)
			continue;
		combined->blocks[index].buffer_binding = source->program->blocks[source->index].buffer_binding;
	}
}
