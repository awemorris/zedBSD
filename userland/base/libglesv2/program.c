/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Shaders and programs of zedBSD's OpenGL ES (WS068 p008).
 *
 * A shader is SPIR-V given by glShaderBinary; there is no GLSL ES
 * compiler yet (WS068 p003), so glCompileShader of a source fails with a
 * log that says so.  Linking reads both shaders' interfaces, applies the
 * attribute locations glBindAttribLocation gave, gives each fragment
 * input the location of the vertex output of the same name, rewrites the
 * vertex shader's gl_Position for Vulkan, merges the two uniform blocks
 * by name, numbers the uniform locations, and makes the Vulkan shaders
 * and layouts.  The uniform values live in a copy of the block that each
 * draw hands to the GPU.
 */

#include "gles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The length of a link or compile log. */
#define PROGRAM_LOG		1024U

/* The serial of the next link (0 is never one). */
static uint64_t program_serial = 1U;

static struct gles_shader *program_shader(struct zegl_context *context, GLuint name);
static struct gles_program *program_get(struct zegl_context *context, GLuint name);
static int program_link(struct gles_state *state, struct gles_program *program, char *log);
static int program_merge(struct gles_program *program, struct gles_spirv *spirv, char *log);
static int program_layout(struct gles_state *state, struct gles_program *program);
static int program_module(struct gles_state *state, const uint32_t *code, size_t words, VkShaderModule *module);
static void program_unlink(struct gles_state *state, struct gles_program *program);
static void program_uniform(GLint location, GLsizei count, unsigned components, const void *values, int integers);
static void program_matrix(GLint location, GLsizei count, GLboolean transpose, const GLfloat *values, unsigned size);
static struct gles_uniform *program_location(struct zegl_context *context, GLint location, unsigned *element);
static void program_copy_string(const char *text, GLsizei size, GLsizei *length, GLchar *out);
static void program_log(char **log, const char *text);

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
	if (program->vertex != NULL) {
		program->vertex->attached--;
		if (program->vertex->delete_pending && program->vertex->attached == 0U) {
			gles_names_remove(&state->objects, program->vertex->name);
			gles_shader_release(program->vertex);
		}
	}

	/* The fragment shader too. */
	if (program->fragment != NULL) {
		program->fragment->attached--;
		if (program->fragment->delete_pending && program->fragment->attached == 0U) {
			gles_names_remove(&state->objects, program->fragment->name);
			gles_shader_release(program->fragment);
		}
	}

	/* The program. */
	free(program->log);
	free(program);
}

/*
 * Frees a shader.
 */
void
gles_shader_release(
	struct gles_shader *shader)
{
	/* The code, the source, the log and the shader. */
	free(shader->code);
	free(shader->source);
	free(shader->log);
	free(shader);
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
	if (type != GL_VERTEX_SHADER && type != GL_FRAGMENT_SHADER) {
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
 * Gives a shader its GLSL source (kept for glGetShaderSource and the
 * compiler to come).
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
 * Compiles a shader's source: there is no GLSL ES compiler yet, so a
 * shader without a SPIR-V binary fails with a log saying so.
 */
GL_APICALL void GL_APIENTRY
glCompileShader(
	GLuint name)
{
	struct zegl_context *context;
	struct gles_shader *shader;

	/* The shader. */
	context = gles_context();
	shader = program_shader(context, name);
	if (shader == NULL)
		return;

	/* A binary already given stays compiled. */
	if (shader->code != NULL) {
		shader->compiled = 1;
		return;
	}

	/* Without a compiler the source cannot become SPIR-V. */
	shader->compiled = 0;
	program_log(&shader->log,
		    "zedBSD OpenGL ES has no GLSL ES compiler yet (plan/ws068 p003); "
		    "give the shader as SPIR-V with glShaderBinary(GL_SHADER_BINARY_FORMAT_SPIR_V)\n");
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

		/* The words replace any earlier ones; the shader counts as compiled. */
		memcpy(code, binary, (size_t)length);
		free(shader->code);
		shader->code = code;
		shader->words = (size_t)length / 4U;
		shader->compiled = 1;
		program_log(&shader->log, "");
	}
}

/*
 * Lets the compiler's resources go (there is no compiler).
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

	/* The program under the name. */
	program->kind = GLES_KIND_PROGRAM;
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

	/* Current: it waits; else it goes now. */
	program->delete_pending = 1;
	if (state->program == program)
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
 * Links a program's shaders.
 */
GL_APICALL void GL_APIENTRY
glLinkProgram(
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

	/* The result and its log. */
	program->linked = 0;
	if (status == 0)
		program->linked = 1;
	program_log(&program->log, log);
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

	/* Current; the one before goes if it was deleted. */
	previous = state->program;
	state->program = program;
	if (previous != NULL && previous != program && previous->delete_pending) {
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
		return;
	case GL_ACTIVE_ATTRIBUTES:
		*params = (GLint)program->attribute_count;
		return;
	case GL_ACTIVE_UNIFORMS:
		*params = (GLint)program->uniform_count;
		return;
	case GL_ACTIVE_ATTRIBUTE_MAX_LENGTH:
	case GL_ACTIVE_UNIFORM_MAX_LENGTH:
		break;
	default:
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* The longest name with its terminator (and "[0]" for arrays). */
	longest = 0;
	if (pname == GL_ACTIVE_ATTRIBUTE_MAX_LENGTH) {
		for (index = 0U; index < program->attribute_count; index++) {
			length = (GLint)strlen(program->attributes[index].name) + 1;
			if (length > longest)
				longest = length;
		}
	} else {
		for (index = 0U; index < program->uniform_count; index++) {
			length = (GLint)strlen(program->uniforms[index].name) + 4;
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

	/* The uniform of that name, and an element it has. */
	for (index = 0U; index < program->uniform_count; index++) {
		entry = &program->uniforms[index];
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
glUniformMatrix2fv(
	GLint location,
	GLsizei count,
	GLboolean transpose,
	const GLfloat *value)
{
	/* 2x2 matrices. */
	program_matrix(location, count, transpose, value, 2U);
}

GL_APICALL void GL_APIENTRY
glUniformMatrix3fv(
	GLint location,
	GLsizei count,
	GLboolean transpose,
	const GLfloat *value)
{
	/* 3x3 matrices. */
	program_matrix(location, count, transpose, value, 3U);
}

GL_APICALL void GL_APIENTRY
glUniformMatrix4fv(
	GLint location,
	GLsizei count,
	GLboolean transpose,
	const GLfloat *value)
{
	/* 4x4 matrices. */
	program_matrix(location, count, transpose, value, 4U);
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
	struct zegl_context *context;
	struct gles_program *program;
	struct gles_uniform *uniform;
	struct gles_location *place;
	const unsigned char *data;
	unsigned column;
	unsigned component;
	int32_t integer;

	/* A linked program and one of its locations. */
	context = gles_context();
	program = program_get(context, name);
	if (program == NULL)
		return;
	if (!program->linked || location < 0 || (unsigned)location >= program->location_count) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* The uniform and element of the location. */
	place = &program->locations[location];
	uniform = &program->uniforms[place->uniform];

	/* A sampler's value is its unit. */
	if (uniform->sampler) {
		params[0] = (GLfloat)uniform->unit;
		return;
	}

	/* Each component of each column. */
	data = program->uniform_data + uniform->offset + place->element * uniform->array_stride;
	for (column = 0U; column < uniform->columns; column++) {
		for (component = 0U; component < uniform->components; component++) {
			if (uniform->base == 0U) {
				memcpy(&params[column * uniform->components + component],
				       data + column * uniform->matrix_stride + component * 4U, 4U);
			} else {
				memcpy(&integer, data + column * uniform->matrix_stride + component * 4U, 4U);
				params[column * uniform->components + component] = (GLfloat)integer;
			}
		}
	}
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
	GLfloat values[16];
	unsigned index;

	/* The floats, converted. */
	memset(values, 0, sizeof(values));
	glGetUniformfv(name, location, values);
	for (index = 0U; index < 16U; index++)
		params[index] = (GLint)values[index];
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

/* Links a program; nonzero with a line in the log when it cannot be linked. */
static int
program_link(
	struct gles_state *state,
	struct gles_program *program,
	char *log)
{
	struct gles_spirv vertex;
	struct gles_spirv fragment;
	struct gles_spirv_variable *input;
	uint32_t *vertex_code;
	uint32_t *fragment_code;
	uint32_t *patched;
	size_t patched_words;
	unsigned index;
	unsigned other;
	int status;
	int differs;

	/* Two compiled shaders. */
	if (program->vertex == NULL || program->fragment == NULL) {
		(void)snprintf(log, PROGRAM_LOG, "a program needs a vertex and a fragment shader\n");
		return -1;
	}

	/* Both compiled. */
	if (!program->vertex->compiled || !program->fragment->compiled || program->vertex->code == NULL || program->fragment->code == NULL) {
		(void)snprintf(log, PROGRAM_LOG, "a shader of the program is not compiled\n");
		return -1;
	}

	/* The two interfaces. */
	status = gles_spirv_reflect(program->vertex->code, program->vertex->words, &vertex, log, PROGRAM_LOG);
	if (status != 0)
		return -1;
	status = gles_spirv_reflect(program->fragment->code, program->fragment->words, &fragment, log, PROGRAM_LOG);
	if (status != 0) {
		gles_spirv_free(&vertex);
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
	vertex_code = malloc(program->vertex->words * sizeof(uint32_t));
	fragment_code = malloc(program->fragment->words * sizeof(uint32_t));
	if (vertex_code == NULL || fragment_code == NULL) {
		free(vertex_code);
		free(fragment_code);
		gles_spirv_free(&vertex);
		gles_spirv_free(&fragment);
		(void)snprintf(log, PROGRAM_LOG, "out of memory\n");
		return -1;
	}

	/* The codes copied. */
	memcpy(vertex_code, program->vertex->code, program->vertex->words * sizeof(uint32_t));
	memcpy(fragment_code, program->fragment->code, program->fragment->words * sizeof(uint32_t));

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

	/* Each fragment input takes the location of the vertex output of its name. */
	for (index = 0U; index < fragment.input_count; index++) {
		input = &fragment.inputs[index];
		for (other = 0U; other < vertex.output_count; other++) {
			differs = strcmp(input->name, vertex.outputs[other].name);
			if (differs != 0 || input->name[0] == '\0')
				continue;
			input->location = vertex.outputs[other].location;
			fragment_code[input->location_word] = vertex.outputs[other].location;
		}
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

	/* The vertex shader's gl_Position made Vulkan's. */
	patched = gles_spirv_position(vertex_code, program->vertex->words, &patched_words);
	free(vertex_code);
	if (patched == NULL) {
		free(fragment_code);
		gles_spirv_free(&vertex);
		gles_spirv_free(&fragment);
		(void)snprintf(log, PROGRAM_LOG, "the vertex shader's gl_Position could not be rewritten\n");
		return -1;
	}

	/* The uniforms of both stages, merged by name. */
	status = program_merge(program, &vertex, log);
	if (status == 0)
		status = program_merge(program, &fragment, log);
	gles_spirv_free(&vertex);
	gles_spirv_free(&fragment);

	/* The shader modules. */
	if (status == 0)
		status = program_module(state, patched, patched_words, &program->vertex_module);
	if (status == 0)
		status = program_module(state, fragment_code, program->fragment->words, &program->fragment_module);
	free(patched);
	free(fragment_code);
	if (status != 0) {
		if (log[0] == '\0')
			(void)snprintf(log, PROGRAM_LOG, "the device refused a shader\n");
		return -1;
	}

	/* The layouts. */
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
	int differs;

	/* Each uniform of the stage. */
	for (index = 0U; index < spirv->uniform_count; index++) {
		uniform = &spirv->uniforms[index];

		/* One the other stage has already. */
		for (other = 0U; other < program->uniform_count; other++) {
			differs = strcmp(program->uniforms[other].name, uniform->name);
			if (differs == 0)
				break;
		}

		/* Already there: it must be at the same place. */
		if (other < program->uniform_count) {
			if (program->uniforms[other].offset != uniform->offset || program->uniforms[other].binding != uniform->binding) {
				(void)snprintf(log, PROGRAM_LOG, "uniform %s is not at the same place in both stages\n", uniform->name);
				return -1;
			}

			continue;
		}

		/* A new one, with locations for its elements. */
		uniforms = realloc(program->uniforms, (program->uniform_count + 1U) * sizeof(*uniforms));
		if (uniforms == NULL)
			return -1;
		program->uniforms = uniforms;
		locations = realloc(program->locations, (program->location_count + (unsigned)uniform->size) * sizeof(*locations));
		if (locations == NULL)
			return -1;
		program->locations = locations;
		uniform->location = (GLint)program->location_count;
		for (element = 0; element < uniform->size; element++) {
			locations[program->location_count].uniform = program->uniform_count;
			locations[program->location_count].element = (unsigned)element;
			program->location_count++;
		}

		/* The uniform. */
		program->uniforms[program->uniform_count] = *uniform;
		program->uniform_count++;

		/* A leaf of the block makes the block at least that large. */
		if (uniform->sampler)
			continue;
		program->uniform_binding = spirv->block_binding;
		end = uniform->offset + (uint32_t)(uniform->size - 1) * uniform->array_stride;
		if (uniform->columns > 1U) {
			end += uniform->columns * uniform->matrix_stride;
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

/* Makes a program's descriptor set layout (the block and each sampler) and pipeline layout; nonzero on failure. */
static int
program_layout(
	struct gles_state *state,
	struct gles_program *program)
{
	VkDescriptorSetLayoutBinding bindings[GLES_UNITS + 1U];
	VkDescriptorSetLayoutCreateInfo set;
	VkPipelineLayoutCreateInfo layout;
	uint32_t count;
	unsigned index;
	VkResult result;

	/* The block, when the program has one (dynamic: each draw's copy is at its own offset). */
	count = 0U;
	memset(bindings, 0, sizeof(bindings));
	if (program->uniform_data != NULL) {
		bindings[count].binding = program->uniform_binding;
		bindings[count].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		bindings[count].descriptorCount = 1U;
		bindings[count].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
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
		bindings[count].descriptorCount = 1U;
		bindings[count].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
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
	if (result != VK_SUCCESS)
		return -1;

	/* Succeeded: the module. */
	return 0;
}

/* Undoes a program's link: its Vulkan objects wait for the frame, its pipelines go, its tables are freed. */
static void
program_unlink(
	struct gles_state *state,
	struct gles_program *program)
{
	struct gles_garbage objects;

	/* The pipelines made for it, and its Vulkan objects. */
	if (program->serial != 0U)
		gles_pipelines_forget(state, program->serial);
	memset(&objects, 0, sizeof(objects));
	objects.layout = program->layout;
	objects.set_layout = program->set_layout;
	objects.modules[0] = program->vertex_module;
	objects.modules[1] = program->fragment_module;
	gles_garbage_keep(state, &objects);

	/* The tables. */
	free(program->uniforms);
	free(program->locations);
	free(program->uniform_data);

	/* Nothing linked. */
	program->layout = VK_NULL_HANDLE;
	program->set_layout = VK_NULL_HANDLE;
	program->vertex_module = VK_NULL_HANDLE;
	program->fragment_module = VK_NULL_HANDLE;
	program->uniforms = NULL;
	program->uniform_count = 0U;
	program->locations = NULL;
	program->location_count = 0U;
	program->uniform_data = NULL;
	program->uniform_size = 0U;
	program->attribute_count = 0U;
	program->serial = 0U;
	program->linked = 0;
}

/*
 * Writes values of a vector size into the current program's uniform at a
 * location, count elements (a sampler takes its texture unit).
 */
static void
program_uniform(
	GLint location,
	GLsizei count,
	unsigned components,
	const void *values,
	int integers)
{
	struct zegl_context *context;
	struct gles_uniform *uniform;
	unsigned char *data;
	unsigned element;
	unsigned component;
	GLsizei index;
	GLint integer;
	GLfloat number;
	uint32_t word;

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

	/* A sampler takes one texture unit. */
	if (uniform->sampler) {
		if (!integers || components != 1U) {
			gles_error(context, GL_INVALID_OPERATION);
			return;
		}

		/* The unit. */
		uniform->unit = ((const GLint *)values)[0];
		return;
	}

	/* The size must match; a vector or scalar only. */
	if (uniform->components != components || uniform->columns != 1U || (count > 1 && uniform->size == 1)) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* Each element, each component converted to the uniform's base type. */
	for (index = 0; index < count && element + (unsigned)index < (unsigned)uniform->size; index++) {
		data = gles_state(context)->program->uniform_data + uniform->offset + (element + (unsigned)index) * uniform->array_stride;
		for (component = 0U; component < components; component++) {
			/* The value given. */
			if (integers) {
				integer = ((const GLint *)values)[(size_t)index * components + component];
				number = (GLfloat)integer;
			} else {
				number = ((const GLfloat *)values)[(size_t)index * components + component];
				integer = (GLint)number;
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
 * Writes square matrices of a size into the current program's uniform at a
 * location, column by column at the block's matrix stride.
 */
static void
program_matrix(
	GLint location,
	GLsizei count,
	GLboolean transpose,
	const GLfloat *values,
	unsigned size)
{
	struct zegl_context *context;
	struct gles_uniform *uniform;
	unsigned char *data;
	unsigned element;
	unsigned column;
	GLsizei index;

	/* Location -1 is ignored. */
	if (location == -1)
		return;

	/* The uniform and element of the location. */
	context = gles_context();
	uniform = program_location(context, location, &element);
	if (uniform == NULL)
		return;
	if (count < 0 || transpose != GL_FALSE) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* A matrix of that size. */
	if (uniform->sampler || uniform->columns != size || uniform->components != size || (count > 1 && uniform->size == 1)) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* Each matrix, each column at the stride. */
	for (index = 0; index < count && element + (unsigned)index < (unsigned)uniform->size; index++) {
		data = gles_state(context)->program->uniform_data + uniform->offset + (element + (unsigned)index) * uniform->array_stride;
		for (column = 0U; column < size; column++)
			memcpy(data + column * uniform->matrix_stride, values + ((size_t)index * size + column) * size, size * sizeof(GLfloat));
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

	/* A current program. */
	state = gles_state(context);
	if (state == NULL)
		return NULL;
	program = state->program;
	if (program == NULL || location < 0 || (unsigned)location >= program->location_count) {
		gles_error(context, GL_INVALID_OPERATION);
		return NULL;
	}

	/* Succeeded: the uniform and the element. */
	*element = program->locations[location].element;
	return &program->uniforms[program->locations[location].uniform];
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
