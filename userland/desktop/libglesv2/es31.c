/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * OpenGL ES 3.1's calls that libGLESv2 alone has, in a context that
 * offers OpenGL ES 3.1 (one of OpenGL ES 3.0 records GL_INVALID_OPERATION
 * for each, as the calls are not in its API): program pipeline objects
 * and glCreateShaderProgramv, glProgramUniform* (each writes its program
 * as glUniform* writes the current one), and the vertex attribute
 * binding calls, which check the vertex array object as OpenGL ES does
 * (draw.c does the rest, which libGL's desktop GL 4.3 calls share).
 * program.c links a pipeline's stages and keeps its programs' uses.
 */

#include "gles.h"

#include <stdlib.h>
#include <string.h>

static struct gles_state *es31_state(struct zegl_context *context);
static struct gles_pipeline_object *es31_pipeline(struct zegl_context *context, struct gles_state *state, GLuint name);
static void es31_stage(struct gles_state *state, struct gles_pipeline_object *pipeline, struct gles_program **slot, struct gles_program *program);
static struct gles_state *es31_uniform_program(GLuint name);
static struct gles_state *es31_vertex_state(struct zegl_context *context, GLuint index);

/*
 * Makes names of program pipeline objects, which have state from their
 * first bind or use.
 */
GL_APICALL void GL_APIENTRY
glGenProgramPipelines(
	GLsizei n,
	GLuint *pipelines)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_pipeline_object *pipeline;
	GLsizei index;
	int status;

	/* A context of 3.1, and a count. */
	context = gles_context();
	state = es31_state(context);
	if (state == NULL)
		return;
	if (n < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Each name gets an object. */
	for (index = 0; index < n; index++) {
		pipeline = calloc(1U, sizeof(*pipeline));
		if (pipeline == NULL) {
			gles_error(context, GL_OUT_OF_MEMORY);
			return;
		}

		/* The first free name. */
		pipeline->name = gles_names_free(&state->pipeline_objects);
		status = gles_names_add(&state->pipeline_objects, pipeline->name, pipeline);
		if (status != 0) {
			free(pipeline);
			gles_error(context, GL_OUT_OF_MEMORY);
			return;
		}

		/* The name goes back to the application. */
		pipelines[index] = pipeline->name;
	}
}

/*
 * Deletes program pipeline objects (a bound one is unbound first); names
 * that are not of one are ignored.
 */
GL_APICALL void GL_APIENTRY
glDeleteProgramPipelines(
	GLsizei n,
	const GLuint *pipelines)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_pipeline_object *pipeline;
	GLsizei index;

	/* A context of 3.1, and a count. */
	context = gles_context();
	state = es31_state(context);
	if (state == NULL)
		return;
	if (n < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Each object: unbound, its programs let go, what it made and the object. */
	for (index = 0; index < n; index++) {
		if (pipelines[index] == 0U)
			continue;
		pipeline = gles_names_get(&state->pipeline_objects, pipelines[index]);
		if (pipeline == NULL)
			continue;
		if (state->pipeline == pipeline)
			state->pipeline = NULL;
		gles_pipeline_forget(state, pipeline);
		es31_stage(state, pipeline, &pipeline->vertex, NULL);
		es31_stage(state, pipeline, &pipeline->fragment, NULL);
		es31_stage(state, pipeline, &pipeline->compute, NULL);
		es31_stage(state, pipeline, &pipeline->active, NULL);
		gles_names_remove(&state->pipeline_objects, pipeline->name);
		free(pipeline->log);
		free(pipeline);
	}
}

/*
 * Reports whether a name is of a program pipeline object that has state
 * (bound or used once).
 */
GL_APICALL GLboolean GL_APIENTRY
glIsProgramPipeline(
	GLuint name)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_pipeline_object *pipeline;

	/* A context of 3.1 with its state. */
	context = gles_context();
	state = es31_state(context);
	if (state == NULL || name == 0U)
		return GL_FALSE;

	/* An object of the name with state. */
	pipeline = gles_names_get(&state->pipeline_objects, name);
	if (pipeline == NULL || !pipeline->bound)
		return GL_FALSE;

	/* Succeeded: it is one. */
	return GL_TRUE;
}

/*
 * Binds a program pipeline object (0: none), which draws and takes
 * glUniform* while no program is current.
 */
GL_APICALL void GL_APIENTRY
glBindProgramPipeline(
	GLuint name)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_pipeline_object *pipeline;

	/* A context of 3.1 with its state. */
	context = gles_context();
	state = es31_state(context);
	if (state == NULL)
		return;

	/* None. */
	if (name == 0U) {
		state->pipeline = NULL;
		return;
	}

	/* Succeeded: an object of a name glGenProgramPipelines made, bound. */
	pipeline = es31_pipeline(context, state, name);
	if (pipeline != NULL)
		state->pipeline = pipeline;
}

/*
 * Makes the executables of a separable program's stages (0: none) those
 * of a program pipeline object's stages named in a mask; a stage the
 * program has no executable of has none.
 */
GL_APICALL void GL_APIENTRY
glUseProgramStages(
	GLuint name,
	GLbitfield stages,
	GLuint program_name)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_pipeline_object *pipeline;
	struct gles_program *program;
	struct gles_program *vertex;
	struct gles_program *fragment;
	struct gles_program *compute;

	/* A context of 3.1, and stages there are. */
	context = gles_context();
	state = es31_state(context);
	if (state == NULL)
		return;
	if (stages != GL_ALL_SHADER_BITS &&
	    (stages & ~(GLbitfield)(GL_VERTEX_SHADER_BIT | GL_FRAGMENT_SHADER_BIT | GL_COMPUTE_SHADER_BIT)) != 0U) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* A program (0: none) linked separable. */
	program = NULL;
	if (program_name != 0U) {
		program = gles_program_named(context, program_name);
		if (program == NULL)
			return;
		if (!program->linked || !program->linked_separable) {
			gles_error(context, GL_INVALID_OPERATION);
			return;
		}
	}

	/* The object. */
	pipeline = es31_pipeline(context, state, name);
	if (pipeline == NULL)
		return;

	/* The program for each stage named that it has an executable of, none for the others. */
	vertex = NULL;
	fragment = NULL;
	compute = NULL;
	if (program != NULL && (program->linked_stages & GL_VERTEX_SHADER_BIT) != 0U)
		vertex = program;
	if (program != NULL && (program->linked_stages & GL_FRAGMENT_SHADER_BIT) != 0U)
		fragment = program;
	if (program != NULL && (program->linked_stages & GL_COMPUTE_SHADER_BIT) != 0U)
		compute = program;

	/* Succeeded: each stage named takes its program. */
	if ((stages & GL_VERTEX_SHADER_BIT) != 0U)
		es31_stage(state, pipeline, &pipeline->vertex, vertex);
	if ((stages & GL_FRAGMENT_SHADER_BIT) != 0U)
		es31_stage(state, pipeline, &pipeline->fragment, fragment);
	if ((stages & GL_COMPUTE_SHADER_BIT) != 0U)
		es31_stage(state, pipeline, &pipeline->compute, compute);
}

/*
 * Makes a linked program (0: none) the one glUniform* writes while a
 * program pipeline object is bound and no program is current.
 */
GL_APICALL void GL_APIENTRY
glActiveShaderProgram(
	GLuint name,
	GLuint program_name)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_pipeline_object *pipeline;
	struct gles_program *program;

	/* A context of 3.1, and the object. */
	context = gles_context();
	state = es31_state(context);
	if (state == NULL)
		return;
	pipeline = es31_pipeline(context, state, name);
	if (pipeline == NULL)
		return;

	/* A linked program (0: none). */
	program = NULL;
	if (program_name != 0U) {
		program = gles_program_named(context, program_name);
		if (program == NULL)
			return;
		if (!program->linked) {
			gles_error(context, GL_INVALID_OPERATION);
			return;
		}
	}

	/* Succeeded: the active program. */
	es31_stage(state, pipeline, &pipeline->active, program);
}

/*
 * Makes a separable program of one stage from GLSL source strings: a
 * shader compiled, attached, the program linked, the shader let go, the
 * shader's log after the link's.  Returns the program's name (0 when
 * the arguments are wrong).
 */
GL_APICALL GLuint GL_APIENTRY
glCreateShaderProgramv(
	GLenum type,
	GLsizei count,
	const GLchar *const *strings)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_program *program;
	char log[1024];
	GLuint shader;
	GLuint name;
	GLint compiled;
	GLsizei length;

	/* A context of 3.1, a stage and a count. */
	context = gles_context();
	state = es31_state(context);
	if (state == NULL)
		return 0U;
	if (type != GL_VERTEX_SHADER && type != GL_FRAGMENT_SHADER && type != GL_COMPUTE_SHADER) {
		gles_error(context, GL_INVALID_ENUM);
		return 0U;
	}
	if (count < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return 0U;
	}

	/* The shader, compiled. */
	shader = glCreateShader(type);
	if (shader == 0U)
		return 0U;
	glShaderSource(shader, count, strings, NULL);
	glCompileShader(shader);

	/* The program, separable, linked when the shader compiled. */
	name = glCreateProgram();
	if (name != 0U) {
		compiled = GL_FALSE;
		glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
		glProgramParameteri(name, GL_PROGRAM_SEPARABLE, GL_TRUE);
		if (compiled) {
			glAttachShader(name, shader);
			glLinkProgram(name);
			glDetachShader(name, shader);
		}

		/* The shader's log after the link's. */
		length = 0;
		log[0] = '\0';
		glGetShaderInfoLog(shader, (GLsizei)sizeof(log), &length, log);
		program = gles_program_named(context, name);
		if (program != NULL)
			gles_program_append_log(program, log);
	}

	/* Succeeded: the program (the shader goes). */
	glDeleteShader(shader);
	return name;
}

/*
 * Reports a property of a program pipeline object: the program of a
 * stage or the active one (names), its validation status, or its log's
 * length.
 */
GL_APICALL void GL_APIENTRY
glGetProgramPipelineiv(
	GLuint name,
	GLenum pname,
	GLint *params)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_pipeline_object *pipeline;
	struct gles_program *program;

	/* A context of 3.1, and the object. */
	context = gles_context();
	state = es31_state(context);
	if (state == NULL)
		return;
	pipeline = es31_pipeline(context, state, name);
	if (pipeline == NULL)
		return;

	/* The program named, the status or the log's length. */
	switch (pname) {
	case GL_ACTIVE_PROGRAM:
		program = pipeline->active;
		break;
	case GL_VERTEX_SHADER:
		program = pipeline->vertex;
		break;
	case GL_FRAGMENT_SHADER:
		program = pipeline->fragment;
		break;
	case GL_COMPUTE_SHADER:
		program = pipeline->compute;
		break;
	case GL_VALIDATE_STATUS:
		*params = pipeline->validated;
		return;
	case GL_INFO_LOG_LENGTH:
		*params = 0;
		if (pipeline->log != NULL && pipeline->log[0] != '\0')
			*params = (GLint)strlen(pipeline->log) + 1;
		return;
	default:
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* Succeeded: the program's name (0: none). */
	*params = 0;
	if (program != NULL)
		*params = (GLint)program->name;
}

/*
 * Checks whether a program pipeline object can draw (or dispatch) as it
 * is, as its validation status and log say.
 */
GL_APICALL void GL_APIENTRY
glValidateProgramPipeline(
	GLuint name)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_pipeline_object *pipeline;
	const char *why;
	int status;

	/* A context of 3.1, and the object. */
	context = gles_context();
	state = es31_state(context);
	if (state == NULL)
		return;
	pipeline = es31_pipeline(context, state, name);
	if (pipeline == NULL)
		return;

	/* A compute program alone dispatches; the draw stages link together. */
	why = NULL;
	if (pipeline->vertex == NULL && pipeline->fragment == NULL) {
		if (pipeline->compute == NULL || !pipeline->compute->linked || !pipeline->compute->linked_separable)
			why = "the pipeline has no program to run\n";
	} else {
		status = gles_pipeline_make(context, state, pipeline);
		if (status != 0)
			why = "the pipeline's vertex and fragment stages cannot draw together (a stage missing or relinked, or their interfaces do not match)\n";
	}

	/* Succeeded: the status, and the log of why not. */
	free(pipeline->log);
	pipeline->log = NULL;
	pipeline->validated = 1;
	if (why != NULL) {
		pipeline->validated = 0;
		pipeline->log = strdup(why);
	}
}

/*
 * Copies a program pipeline object's log (of its last validation).
 */
GL_APICALL void GL_APIENTRY
glGetProgramPipelineInfoLog(
	GLuint name,
	GLsizei bufSize,
	GLsizei *length,
	GLchar *infoLog)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_pipeline_object *pipeline;
	const char *text;
	size_t copied;

	/* A context of 3.1, the object and a size. */
	context = gles_context();
	state = es31_state(context);
	if (state == NULL)
		return;
	pipeline = es31_pipeline(context, state, name);
	if (pipeline == NULL)
		return;
	if (bufSize < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* As much of the log as fits, with its terminator. */
	text = "";
	if (pipeline->log != NULL)
		text = pipeline->log;
	copied = 0U;
	if (bufSize > 0) {
		copied = strlen(text);
		if (copied > (size_t)bufSize - 1U)
			copied = (size_t)bufSize - 1U;
		memcpy(infoLog, text, copied);
		infoLog[copied] = '\0';
	}

	/* Succeeded: the length copied. */
	if (length != NULL)
		*length = (GLsizei)copied;
}

/*
 * Writes a uniform of a program as glUniform1i writes the current program's.
 */
GL_APICALL void GL_APIENTRY
glProgramUniform1i(
	GLuint program,
	GLint location,
	GLint v0)
{
	struct gles_state *state;

	/* The program glUniform1i writes for this call. */
	state = es31_uniform_program(program);
	if (state == NULL)
		return;

	/* Its uniform. */
	glUniform1i(location, v0);
	state->uniform_program = NULL;
}

/*
 * Writes a uniform of a program as glUniform2i writes the current program's.
 */
GL_APICALL void GL_APIENTRY
glProgramUniform2i(
	GLuint program,
	GLint location,
	GLint v0,
	GLint v1)
{
	struct gles_state *state;

	/* The program glUniform2i writes for this call. */
	state = es31_uniform_program(program);
	if (state == NULL)
		return;

	/* Its uniform. */
	glUniform2i(location, v0, v1);
	state->uniform_program = NULL;
}

/*
 * Writes a uniform of a program as glUniform3i writes the current program's.
 */
GL_APICALL void GL_APIENTRY
glProgramUniform3i(
	GLuint program,
	GLint location,
	GLint v0,
	GLint v1,
	GLint v2)
{
	struct gles_state *state;

	/* The program glUniform3i writes for this call. */
	state = es31_uniform_program(program);
	if (state == NULL)
		return;

	/* Its uniform. */
	glUniform3i(location, v0, v1, v2);
	state->uniform_program = NULL;
}

/*
 * Writes a uniform of a program as glUniform4i writes the current program's.
 */
GL_APICALL void GL_APIENTRY
glProgramUniform4i(
	GLuint program,
	GLint location,
	GLint v0,
	GLint v1,
	GLint v2,
	GLint v3)
{
	struct gles_state *state;

	/* The program glUniform4i writes for this call. */
	state = es31_uniform_program(program);
	if (state == NULL)
		return;

	/* Its uniform. */
	glUniform4i(location, v0, v1, v2, v3);
	state->uniform_program = NULL;
}

/*
 * Writes a uniform of a program as glUniform1ui writes the current program's.
 */
GL_APICALL void GL_APIENTRY
glProgramUniform1ui(
	GLuint program,
	GLint location,
	GLuint v0)
{
	struct gles_state *state;

	/* The program glUniform1ui writes for this call. */
	state = es31_uniform_program(program);
	if (state == NULL)
		return;

	/* Its uniform. */
	glUniform1ui(location, v0);
	state->uniform_program = NULL;
}

/*
 * Writes a uniform of a program as glUniform2ui writes the current program's.
 */
GL_APICALL void GL_APIENTRY
glProgramUniform2ui(
	GLuint program,
	GLint location,
	GLuint v0,
	GLuint v1)
{
	struct gles_state *state;

	/* The program glUniform2ui writes for this call. */
	state = es31_uniform_program(program);
	if (state == NULL)
		return;

	/* Its uniform. */
	glUniform2ui(location, v0, v1);
	state->uniform_program = NULL;
}

/*
 * Writes a uniform of a program as glUniform3ui writes the current program's.
 */
GL_APICALL void GL_APIENTRY
glProgramUniform3ui(
	GLuint program,
	GLint location,
	GLuint v0,
	GLuint v1,
	GLuint v2)
{
	struct gles_state *state;

	/* The program glUniform3ui writes for this call. */
	state = es31_uniform_program(program);
	if (state == NULL)
		return;

	/* Its uniform. */
	glUniform3ui(location, v0, v1, v2);
	state->uniform_program = NULL;
}

/*
 * Writes a uniform of a program as glUniform4ui writes the current program's.
 */
GL_APICALL void GL_APIENTRY
glProgramUniform4ui(
	GLuint program,
	GLint location,
	GLuint v0,
	GLuint v1,
	GLuint v2,
	GLuint v3)
{
	struct gles_state *state;

	/* The program glUniform4ui writes for this call. */
	state = es31_uniform_program(program);
	if (state == NULL)
		return;

	/* Its uniform. */
	glUniform4ui(location, v0, v1, v2, v3);
	state->uniform_program = NULL;
}

/*
 * Writes a uniform of a program as glUniform1f writes the current program's.
 */
GL_APICALL void GL_APIENTRY
glProgramUniform1f(
	GLuint program,
	GLint location,
	GLfloat v0)
{
	struct gles_state *state;

	/* The program glUniform1f writes for this call. */
	state = es31_uniform_program(program);
	if (state == NULL)
		return;

	/* Its uniform. */
	glUniform1f(location, v0);
	state->uniform_program = NULL;
}

/*
 * Writes a uniform of a program as glUniform2f writes the current program's.
 */
GL_APICALL void GL_APIENTRY
glProgramUniform2f(
	GLuint program,
	GLint location,
	GLfloat v0,
	GLfloat v1)
{
	struct gles_state *state;

	/* The program glUniform2f writes for this call. */
	state = es31_uniform_program(program);
	if (state == NULL)
		return;

	/* Its uniform. */
	glUniform2f(location, v0, v1);
	state->uniform_program = NULL;
}

/*
 * Writes a uniform of a program as glUniform3f writes the current program's.
 */
GL_APICALL void GL_APIENTRY
glProgramUniform3f(
	GLuint program,
	GLint location,
	GLfloat v0,
	GLfloat v1,
	GLfloat v2)
{
	struct gles_state *state;

	/* The program glUniform3f writes for this call. */
	state = es31_uniform_program(program);
	if (state == NULL)
		return;

	/* Its uniform. */
	glUniform3f(location, v0, v1, v2);
	state->uniform_program = NULL;
}

/*
 * Writes a uniform of a program as glUniform4f writes the current program's.
 */
GL_APICALL void GL_APIENTRY
glProgramUniform4f(
	GLuint program,
	GLint location,
	GLfloat v0,
	GLfloat v1,
	GLfloat v2,
	GLfloat v3)
{
	struct gles_state *state;

	/* The program glUniform4f writes for this call. */
	state = es31_uniform_program(program);
	if (state == NULL)
		return;

	/* Its uniform. */
	glUniform4f(location, v0, v1, v2, v3);
	state->uniform_program = NULL;
}

/*
 * Writes a uniform of a program as glUniform1iv writes the current program's.
 */
GL_APICALL void GL_APIENTRY
glProgramUniform1iv(
	GLuint program,
	GLint location,
	GLsizei count,
	const GLint *value)
{
	struct gles_state *state;

	/* The program glUniform1iv writes for this call. */
	state = es31_uniform_program(program);
	if (state == NULL)
		return;

	/* Its uniform. */
	glUniform1iv(location, count, value);
	state->uniform_program = NULL;
}

/*
 * Writes a uniform of a program as glUniform2iv writes the current program's.
 */
GL_APICALL void GL_APIENTRY
glProgramUniform2iv(
	GLuint program,
	GLint location,
	GLsizei count,
	const GLint *value)
{
	struct gles_state *state;

	/* The program glUniform2iv writes for this call. */
	state = es31_uniform_program(program);
	if (state == NULL)
		return;

	/* Its uniform. */
	glUniform2iv(location, count, value);
	state->uniform_program = NULL;
}

/*
 * Writes a uniform of a program as glUniform3iv writes the current program's.
 */
GL_APICALL void GL_APIENTRY
glProgramUniform3iv(
	GLuint program,
	GLint location,
	GLsizei count,
	const GLint *value)
{
	struct gles_state *state;

	/* The program glUniform3iv writes for this call. */
	state = es31_uniform_program(program);
	if (state == NULL)
		return;

	/* Its uniform. */
	glUniform3iv(location, count, value);
	state->uniform_program = NULL;
}

/*
 * Writes a uniform of a program as glUniform4iv writes the current program's.
 */
GL_APICALL void GL_APIENTRY
glProgramUniform4iv(
	GLuint program,
	GLint location,
	GLsizei count,
	const GLint *value)
{
	struct gles_state *state;

	/* The program glUniform4iv writes for this call. */
	state = es31_uniform_program(program);
	if (state == NULL)
		return;

	/* Its uniform. */
	glUniform4iv(location, count, value);
	state->uniform_program = NULL;
}

/*
 * Writes a uniform of a program as glUniform1uiv writes the current program's.
 */
GL_APICALL void GL_APIENTRY
glProgramUniform1uiv(
	GLuint program,
	GLint location,
	GLsizei count,
	const GLuint *value)
{
	struct gles_state *state;

	/* The program glUniform1uiv writes for this call. */
	state = es31_uniform_program(program);
	if (state == NULL)
		return;

	/* Its uniform. */
	glUniform1uiv(location, count, value);
	state->uniform_program = NULL;
}

/*
 * Writes a uniform of a program as glUniform2uiv writes the current program's.
 */
GL_APICALL void GL_APIENTRY
glProgramUniform2uiv(
	GLuint program,
	GLint location,
	GLsizei count,
	const GLuint *value)
{
	struct gles_state *state;

	/* The program glUniform2uiv writes for this call. */
	state = es31_uniform_program(program);
	if (state == NULL)
		return;

	/* Its uniform. */
	glUniform2uiv(location, count, value);
	state->uniform_program = NULL;
}

/*
 * Writes a uniform of a program as glUniform3uiv writes the current program's.
 */
GL_APICALL void GL_APIENTRY
glProgramUniform3uiv(
	GLuint program,
	GLint location,
	GLsizei count,
	const GLuint *value)
{
	struct gles_state *state;

	/* The program glUniform3uiv writes for this call. */
	state = es31_uniform_program(program);
	if (state == NULL)
		return;

	/* Its uniform. */
	glUniform3uiv(location, count, value);
	state->uniform_program = NULL;
}

/*
 * Writes a uniform of a program as glUniform4uiv writes the current program's.
 */
GL_APICALL void GL_APIENTRY
glProgramUniform4uiv(
	GLuint program,
	GLint location,
	GLsizei count,
	const GLuint *value)
{
	struct gles_state *state;

	/* The program glUniform4uiv writes for this call. */
	state = es31_uniform_program(program);
	if (state == NULL)
		return;

	/* Its uniform. */
	glUniform4uiv(location, count, value);
	state->uniform_program = NULL;
}

/*
 * Writes a uniform of a program as glUniform1fv writes the current program's.
 */
GL_APICALL void GL_APIENTRY
glProgramUniform1fv(
	GLuint program,
	GLint location,
	GLsizei count,
	const GLfloat *value)
{
	struct gles_state *state;

	/* The program glUniform1fv writes for this call. */
	state = es31_uniform_program(program);
	if (state == NULL)
		return;

	/* Its uniform. */
	glUniform1fv(location, count, value);
	state->uniform_program = NULL;
}

/*
 * Writes a uniform of a program as glUniform2fv writes the current program's.
 */
GL_APICALL void GL_APIENTRY
glProgramUniform2fv(
	GLuint program,
	GLint location,
	GLsizei count,
	const GLfloat *value)
{
	struct gles_state *state;

	/* The program glUniform2fv writes for this call. */
	state = es31_uniform_program(program);
	if (state == NULL)
		return;

	/* Its uniform. */
	glUniform2fv(location, count, value);
	state->uniform_program = NULL;
}

/*
 * Writes a uniform of a program as glUniform3fv writes the current program's.
 */
GL_APICALL void GL_APIENTRY
glProgramUniform3fv(
	GLuint program,
	GLint location,
	GLsizei count,
	const GLfloat *value)
{
	struct gles_state *state;

	/* The program glUniform3fv writes for this call. */
	state = es31_uniform_program(program);
	if (state == NULL)
		return;

	/* Its uniform. */
	glUniform3fv(location, count, value);
	state->uniform_program = NULL;
}

/*
 * Writes a uniform of a program as glUniform4fv writes the current program's.
 */
GL_APICALL void GL_APIENTRY
glProgramUniform4fv(
	GLuint program,
	GLint location,
	GLsizei count,
	const GLfloat *value)
{
	struct gles_state *state;

	/* The program glUniform4fv writes for this call. */
	state = es31_uniform_program(program);
	if (state == NULL)
		return;

	/* Its uniform. */
	glUniform4fv(location, count, value);
	state->uniform_program = NULL;
}

/*
 * Writes a uniform of a program as glUniformMatrix2fv writes the current program's.
 */
GL_APICALL void GL_APIENTRY
glProgramUniformMatrix2fv(
	GLuint program,
	GLint location,
	GLsizei count,
	GLboolean transpose,
	const GLfloat *value)
{
	struct gles_state *state;

	/* The program glUniformMatrix2fv writes for this call. */
	state = es31_uniform_program(program);
	if (state == NULL)
		return;

	/* Its uniform. */
	glUniformMatrix2fv(location, count, transpose, value);
	state->uniform_program = NULL;
}

/*
 * Writes a uniform of a program as glUniformMatrix3fv writes the current program's.
 */
GL_APICALL void GL_APIENTRY
glProgramUniformMatrix3fv(
	GLuint program,
	GLint location,
	GLsizei count,
	GLboolean transpose,
	const GLfloat *value)
{
	struct gles_state *state;

	/* The program glUniformMatrix3fv writes for this call. */
	state = es31_uniform_program(program);
	if (state == NULL)
		return;

	/* Its uniform. */
	glUniformMatrix3fv(location, count, transpose, value);
	state->uniform_program = NULL;
}

/*
 * Writes a uniform of a program as glUniformMatrix4fv writes the current program's.
 */
GL_APICALL void GL_APIENTRY
glProgramUniformMatrix4fv(
	GLuint program,
	GLint location,
	GLsizei count,
	GLboolean transpose,
	const GLfloat *value)
{
	struct gles_state *state;

	/* The program glUniformMatrix4fv writes for this call. */
	state = es31_uniform_program(program);
	if (state == NULL)
		return;

	/* Its uniform. */
	glUniformMatrix4fv(location, count, transpose, value);
	state->uniform_program = NULL;
}

/*
 * Writes a uniform of a program as glUniformMatrix2x3fv writes the current program's.
 */
GL_APICALL void GL_APIENTRY
glProgramUniformMatrix2x3fv(
	GLuint program,
	GLint location,
	GLsizei count,
	GLboolean transpose,
	const GLfloat *value)
{
	struct gles_state *state;

	/* The program glUniformMatrix2x3fv writes for this call. */
	state = es31_uniform_program(program);
	if (state == NULL)
		return;

	/* Its uniform. */
	glUniformMatrix2x3fv(location, count, transpose, value);
	state->uniform_program = NULL;
}

/*
 * Writes a uniform of a program as glUniformMatrix3x2fv writes the current program's.
 */
GL_APICALL void GL_APIENTRY
glProgramUniformMatrix3x2fv(
	GLuint program,
	GLint location,
	GLsizei count,
	GLboolean transpose,
	const GLfloat *value)
{
	struct gles_state *state;

	/* The program glUniformMatrix3x2fv writes for this call. */
	state = es31_uniform_program(program);
	if (state == NULL)
		return;

	/* Its uniform. */
	glUniformMatrix3x2fv(location, count, transpose, value);
	state->uniform_program = NULL;
}

/*
 * Writes a uniform of a program as glUniformMatrix2x4fv writes the current program's.
 */
GL_APICALL void GL_APIENTRY
glProgramUniformMatrix2x4fv(
	GLuint program,
	GLint location,
	GLsizei count,
	GLboolean transpose,
	const GLfloat *value)
{
	struct gles_state *state;

	/* The program glUniformMatrix2x4fv writes for this call. */
	state = es31_uniform_program(program);
	if (state == NULL)
		return;

	/* Its uniform. */
	glUniformMatrix2x4fv(location, count, transpose, value);
	state->uniform_program = NULL;
}

/*
 * Writes a uniform of a program as glUniformMatrix4x2fv writes the current program's.
 */
GL_APICALL void GL_APIENTRY
glProgramUniformMatrix4x2fv(
	GLuint program,
	GLint location,
	GLsizei count,
	GLboolean transpose,
	const GLfloat *value)
{
	struct gles_state *state;

	/* The program glUniformMatrix4x2fv writes for this call. */
	state = es31_uniform_program(program);
	if (state == NULL)
		return;

	/* Its uniform. */
	glUniformMatrix4x2fv(location, count, transpose, value);
	state->uniform_program = NULL;
}

/*
 * Writes a uniform of a program as glUniformMatrix3x4fv writes the current program's.
 */
GL_APICALL void GL_APIENTRY
glProgramUniformMatrix3x4fv(
	GLuint program,
	GLint location,
	GLsizei count,
	GLboolean transpose,
	const GLfloat *value)
{
	struct gles_state *state;

	/* The program glUniformMatrix3x4fv writes for this call. */
	state = es31_uniform_program(program);
	if (state == NULL)
		return;

	/* Its uniform. */
	glUniformMatrix3x4fv(location, count, transpose, value);
	state->uniform_program = NULL;
}

/*
 * Writes a uniform of a program as glUniformMatrix4x3fv writes the current program's.
 */
GL_APICALL void GL_APIENTRY
glProgramUniformMatrix4x3fv(
	GLuint program,
	GLint location,
	GLsizei count,
	GLboolean transpose,
	const GLfloat *value)
{
	struct gles_state *state;

	/* The program glUniformMatrix4x3fv writes for this call. */
	state = es31_uniform_program(program);
	if (state == NULL)
		return;

	/* Its uniform. */
	glUniformMatrix4x3fv(location, count, transpose, value);
	state->uniform_program = NULL;
}

/*
 * Binds a buffer object's elements, from an offset and a stride apart, to
 * a vertex buffer binding point of the bound vertex array object (buffer
 * 0: none).
 */
GL_APICALL void GL_APIENTRY
glBindVertexBuffer(
	GLuint bindingindex,
	GLuint buffer,
	GLintptr offset,
	GLsizei stride)
{
	struct zegl_context *context;
	struct gles_state *state;

	/* A context of 3.1 with a binding point and a vertex array object. */
	context = gles_context();
	state = es31_vertex_state(context, bindingindex);
	if (state == NULL)
		return;

	/* The binding point's arrays (draw.c). */
	gles_bind_vertex_buffer(context, state, bindingindex, buffer, offset, stride);
}

/*
 * Sets the format of an attribute's elements, read as floats, and their
 * offset in its binding point's elements.
 */
GL_APICALL void GL_APIENTRY
glVertexAttribFormat(
	GLuint attribindex,
	GLint size,
	GLenum type,
	GLboolean normalized,
	GLuint relativeoffset)
{
	struct zegl_context *context;
	struct gles_state *state;

	/* A context of 3.1 with an attribute and a vertex array object. */
	context = gles_context();
	state = es31_vertex_state(context, attribindex);
	if (state == NULL)
		return;

	/* The format (draw.c). */
	gles_vertex_attrib_format(context, state, attribindex, size, type, normalized, 0, relativeoffset);
}

/*
 * Sets the format of an attribute's elements, integers the shader reads
 * as they are, and their offset in its binding point's elements.
 */
GL_APICALL void GL_APIENTRY
glVertexAttribIFormat(
	GLuint attribindex,
	GLint size,
	GLenum type,
	GLuint relativeoffset)
{
	struct zegl_context *context;
	struct gles_state *state;

	/* A context of 3.1 with an attribute and a vertex array object. */
	context = gles_context();
	state = es31_vertex_state(context, attribindex);
	if (state == NULL)
		return;

	/* The format (draw.c). */
	gles_vertex_attrib_format(context, state, attribindex, size, type, GL_FALSE, 1, relativeoffset);
}

/*
 * Makes an attribute read the elements of a binding point.
 */
GL_APICALL void GL_APIENTRY
glVertexAttribBinding(
	GLuint attribindex,
	GLuint bindingindex)
{
	struct zegl_context *context;
	struct gles_state *state;

	/* A context of 3.1 with an attribute and a vertex array object. */
	context = gles_context();
	state = es31_vertex_state(context, attribindex);
	if (state == NULL)
		return;

	/* The binding point it reads (draw.c). */
	gles_vertex_attrib_binding(context, state, attribindex, bindingindex);
}

/*
 * Makes a binding point's elements advance once every divisor instances
 * (0: once per vertex).
 */
GL_APICALL void GL_APIENTRY
glVertexBindingDivisor(
	GLuint bindingindex,
	GLuint divisor)
{
	struct zegl_context *context;
	struct gles_state *state;

	/* A context of 3.1 with a binding point and a vertex array object. */
	context = gles_context();
	state = es31_vertex_state(context, bindingindex);
	if (state == NULL)
		return;

	/* The divisor (draw.c). */
	gles_vertex_binding_divisor(state, bindingindex, divisor);
}

/*
 * Returns the current context's state when it offers OpenGL ES 3.1; NULL
 * without a context, or with GL_INVALID_OPERATION recorded in one of
 * OpenGL ES 3.0, which has no such call.
 */
static struct gles_state *
es31_state(
	struct zegl_context *context)
{
	struct gles_state *state;

	/* A context with its state. */
	state = gles_state(context);
	if (state == NULL)
		return NULL;

	/* Of OpenGL ES 3.1. */
	if (!state->compute) {
		gles_error(context, GL_INVALID_OPERATION);
		return NULL;
	}

	/* Succeeded: the state. */
	return state;
}

/*
 * Returns the program pipeline object of a name glGenProgramPipelines
 * made, which has state from now on; NULL with GL_INVALID_OPERATION
 * recorded for any other name.
 */
static struct gles_pipeline_object *
es31_pipeline(
	struct zegl_context *context,
	struct gles_state *state,
	GLuint name)
{
	struct gles_pipeline_object *pipeline;

	/* An object of the name. */
	pipeline = NULL;
	if (name != 0U)
		pipeline = gles_names_get(&state->pipeline_objects, name);
	if (pipeline == NULL) {
		gles_error(context, GL_INVALID_OPERATION);
		return NULL;
	}

	/* Succeeded: the object, with state. */
	pipeline->bound = 1;
	return pipeline;
}

/*
 * Puts a program in a slot of a program pipeline object (a stage's or the
 * active one): the program in it before is let go and the new one held;
 * the program the object draws with is made again for a new vertex or
 * fragment stage.
 */
static void
es31_stage(
	struct gles_state *state,
	struct gles_pipeline_object *pipeline,
	struct gles_program **slot,
	struct gles_program *program)
{
	struct gles_program *previous;

	/* The same program stays. */
	previous = *slot;
	if (previous == program)
		return;

	/* A draw stage changes what the object draws with. */
	if (slot == &pipeline->vertex || slot == &pipeline->fragment)
		gles_pipeline_forget(state, pipeline);

	/* Succeeded: the new program held, the one before let go. */
	gles_program_use(program);
	*slot = program;
	gles_program_unuse(state, previous);
}

/*
 * Returns the current context's state with a program made the one
 * glUniform* writes for a glProgramUniform* call; NULL with the error
 * recorded (a context of OpenGL ES 3.0, a name that is not a program's).
 */
static struct gles_state *
es31_uniform_program(
	GLuint name)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_program *program;

	/* A context of 3.1, and the program. */
	context = gles_context();
	state = es31_state(context);
	if (state == NULL)
		return NULL;
	program = gles_program_named(context, name);
	if (program == NULL)
		return NULL;

	/* Succeeded: the program glUniform* writes until the call ends. */
	state->uniform_program = program;
	return state;
}

/*
 * Returns the current context's state for a vertex attribute binding call
 * on an attribute or a binding point of an index; NULL without a context,
 * or with the error recorded: a context of OpenGL ES 3.0, which has no
 * such call, or the default vertex array object bound
 * (GL_INVALID_OPERATION), an index past the last (GL_INVALID_VALUE).
 */
static struct gles_state *
es31_vertex_state(
	struct zegl_context *context,
	GLuint index)
{
	struct gles_state *state;

	/* A context of 3.1 with its state. */
	state = gles_state(context);
	if (state == NULL)
		return NULL;
	if (!state->compute) {
		gles_error(context, GL_INVALID_OPERATION);
		return NULL;
	}

	/* An index there is. */
	if (index >= GLES_ATTRIBS) {
		gles_error(context, GL_INVALID_VALUE);
		return NULL;
	}

	/* A vertex array object of the application's (the default one cannot be changed so). */
	if (state->vertex_array == 0U) {
		gles_error(context, GL_INVALID_OPERATION);
		return NULL;
	}

	/* Succeeded: the state. */
	return state;
}
