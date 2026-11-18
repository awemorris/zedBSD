/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * egltest's OpenGL ES 3.0 transform feedback scene (WS068 p030): the
 * start captures vertex shader outputs with rasterization discarded, and
 * each frame shows one square per outcome, green when it is the expected
 * one and red otherwise, and one square drawn from vertices captured:
 *
 *   interleaved outputs of a triangle (gl_Position, a colour, an int),
 *   a strip's two triangles in GL's order with the primitives-written
 *   query, separate outputs into two buffers, instances (gl_InstanceID),
 *   nothing captured while paused, a square drawn from captured positions
 *   used as a vertex array (green), and GL_VERSION 3.0.
 *
 * The start also checks what the API reports: the program's captured
 * outputs, the bindings, and the errors of draws and binds transform
 * feedback refuses.
 */

#include "feedback.h"

#include <GLES3/gl3.h>

#include <stdio.h>
#include <string.h>

/* The outcomes shown, the square drawn from captured vertices, and the attribute location of the corners. */
#define FEEDBACK_OUTCOMES	6U
#define FEEDBACK_DRAWN		6U
#define FEEDBACK_POSITION	0U

/* The words of a vertex's interleaved record: gl_Position, the colour, the int. */
#define FEEDBACK_RECORD		9U

/*
 * The programs (capturing interleaved, capturing separate, drawing one
 * colour, drawing captured positions), the one-colour program's uniforms,
 * the corners and captured buffers, and each outcome (nonzero: the
 * expected one); kept for the run with the number of the start's checks
 * that failed.
 */
static GLuint feedback_interleaved;
static GLuint feedback_separate;
static GLuint feedback_solid;
static GLuint feedback_replay;
static GLint feedback_rect;
static GLint feedback_colour;
static GLint feedback_shift;
static GLuint feedback_corners;
static GLuint feedback_buffers[3];
static GLuint feedback_arrays[2];
static int feedback_outcomes[FEEDBACK_OUTCOMES];
static int feedback_failures;

/* The capturing vertex shader: its position, a colour made from it and the instance, and the vertex number. */
static const char feedback_capture_source[] =
	"#version 300 es\n"
	"layout(location = 0) in vec2 a_position;\n"
	"uniform vec2 u_shift;\n"
	"out vec4 v_colour;\n"
	"flat out int v_index;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\tv_colour = vec4(a_position * 0.5 + 0.5, 0.25, float(gl_InstanceID));\n"
	"\tv_index = gl_VertexID;\n"
	"\tgl_Position = vec4(a_position + u_shift, 0.0, 1.0);\n"
	"}\n";

/* A fragment shader for the capturing programs (nothing is rasterized). */
static const char feedback_capture_fragment[] =
	"#version 300 es\n"
	"precision mediump float;\n"
	"in vec4 v_colour;\n"
	"flat in int v_index;\n"
	"out vec4 o_colour;\n"
	"void main()\n"
	"{\n"
	"\to_colour = v_colour + vec4(float(v_index));\n"
	"}\n";

/* The one-colour program: a unit square placed by the rectangle. */
static const char feedback_solid_vertex[] =
	"#version 300 es\n"
	"layout(location = 0) in vec2 a_position;\n"
	"uniform vec4 u_rect;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\tgl_Position = vec4(u_rect.xy + a_position * u_rect.zw, 0.0, 1.0);\n"
	"}\n";
static const char feedback_solid_fragment[] =
	"#version 300 es\n"
	"precision mediump float;\n"
	"uniform vec4 u_colour;\n"
	"out vec4 o_colour;\n"
	"void main()\n"
	"{\n"
	"\to_colour = u_colour;\n"
	"}\n";

/* The program that draws captured positions (vec4 each) in green. */
static const char feedback_replay_vertex[] =
	"#version 300 es\n"
	"layout(location = 0) in vec4 a_position;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\tgl_Position = a_position;\n"
	"}\n";
static const char feedback_replay_fragment[] =
	"#version 300 es\n"
	"precision mediump float;\n"
	"out vec4 o_colour;\n"
	"void main()\n"
	"{\n"
	"\to_colour = vec4(0.0, 1.0, 0.0, 1.0);\n"
	"}\n";

/* The names of the outcomes. */
static const char *const feedback_names[FEEDBACK_OUTCOMES + 1U] = {
	"interleaved", "strip-order-and-query", "separate", "instances", "paused", "version-3.0", "drawn-from-captured"
};

static GLuint feedback_program(const char *vertex_source, const char *fragment_source, const GLchar *const *varyings, GLsizei count, GLenum mode);
static void feedback_capture(void);
static void feedback_more(void);
static void feedback_errors(void);
static int feedback_close(float got, float expected);
static void feedback_expect(const char *what, long got, long expected);

/*
 * Makes the programs and buffers, captures outputs, and checks what the
 * API reports.  Returns 0, or -1 with a line saying what failed.
 */
int
egltest_feedback_start(void)
{
	static const GLchar *const interleaved[3] = { "gl_Position", "v_colour", "v_index" };
	static const GLchar *const separate[2] = { "v_colour", "v_index" };
	static const GLfloat corners[8] = { -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };
	const GLubyte *version;
	GLenum error;
	int same;

	/* The programs. */
	feedback_interleaved = feedback_program(feedback_capture_source, feedback_capture_fragment, interleaved, 3, GL_INTERLEAVED_ATTRIBS);
	feedback_separate = feedback_program(feedback_capture_source, feedback_capture_fragment, separate, 2, GL_SEPARATE_ATTRIBS);
	feedback_solid = feedback_program(feedback_solid_vertex, feedback_solid_fragment, NULL, 0, GL_INTERLEAVED_ATTRIBS);
	feedback_replay = feedback_program(feedback_replay_vertex, feedback_replay_fragment, NULL, 0, GL_INTERLEAVED_ATTRIBS);
	if (feedback_interleaved == 0U || feedback_separate == 0U || feedback_solid == 0U || feedback_replay == 0U)
		return -1;
	feedback_rect = glGetUniformLocation(feedback_solid, "u_rect");
	feedback_colour = glGetUniformLocation(feedback_solid, "u_colour");
	feedback_shift = glGetUniformLocation(feedback_interleaved, "u_shift");

	/* The corners, and the buffers captured into. */
	glGenBuffers(1, &feedback_corners);
	glBindBuffer(GL_ARRAY_BUFFER, feedback_corners);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(corners), corners, GL_STATIC_DRAW);
	glGenBuffers(3, feedback_buffers);
	glGenVertexArrays(2, feedback_arrays);
	glBindVertexArray(feedback_arrays[0]);
	glVertexAttribPointer(FEEDBACK_POSITION, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glEnableVertexAttribArray(FEEDBACK_POSITION);

	/* The captures and the checks. */
	feedback_capture();
	feedback_more();
	feedback_errors();

	/* OpenGL ES 3.0 or later (3.1 where the device offers compute, ws101-p009). */
	version = glGetString(GL_VERSION);
	same = 1;
	if (version != NULL)
		same = strncmp((const char *)version, "OpenGL ES 3.", 12U);
	feedback_outcomes[5] = 0;
	if (same == 0)
		feedback_outcomes[5] = 1;

	/* Everything went without an error the checks did not expect. */
	glBindVertexArray(0U);
	error = glGetError();
	feedback_expect("start-glerror", (long)error, (long)GL_NO_ERROR);

	/* Succeeded: ready to draw. */
	printf("EGLTEST FEEDBACK ready failures=%d\n", feedback_failures);
	fflush(stdout);
	return 0;
}

/*
 * Draws one square per outcome, green when it is the expected one and red
 * otherwise, and the square whose positions were captured.
 */
void
egltest_feedback_draw(
	int width,
	int height)
{
	unsigned index;
	GLfloat green;

	/* The whole window, cleared to dark grey. */
	glBindFramebuffer(GL_FRAMEBUFFER, 0U);
	glViewport(0, 0, width, height);
	glClearColor(0.125f, 0.125f, 0.125f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	/* Each outcome in a cell of the top row and the next. */
	glBindVertexArray(feedback_arrays[0]);
	glUseProgram(feedback_solid);
	for (index = 0U; index < FEEDBACK_OUTCOMES; index++) {
		green = 0.0f;
		if (feedback_outcomes[index])
			green = 1.0f;
		glUniform4f(feedback_rect, -0.75f + 0.5f * (GLfloat)(index % 4U), 0.66f - 0.66f * (GLfloat)(index / 4U), 0.2f, 0.25f);
		glUniform4f(feedback_colour, 1.0f - green, green, 0.0f, 1.0f);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	}

	/* The square from captured positions, in the seventh cell. */
	glBindVertexArray(feedback_arrays[1]);
	glUseProgram(feedback_replay);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	glBindVertexArray(0U);
}

/*
 * Reads back each square's centre and prints the colours; returns how
 * many are not green, with the start's failed checks and any error.
 */
int
egltest_feedback_check(
	int width,
	int height,
	const char *token)
{
	GLubyte pixel[4];
	GLenum error;
	const char *verdict;
	unsigned got;
	unsigned index;
	int failures;
	int x;
	int y;

	/* Each square's centre, in GL's coordinates. */
	failures = feedback_failures;
	for (index = 0U; index <= FEEDBACK_OUTCOMES; index++) {
		x = (int)((-0.75f + 0.5f * (float)(index % 4U) + 1.0f) * 0.5f * (float)width);
		y = (int)((0.66f - 0.66f * (float)(index / 4U) + 1.0f) * 0.5f * (float)height);
		memset(pixel, 0, sizeof(pixel));
		glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
		got = ((unsigned)pixel[0] << 16) | ((unsigned)pixel[1] << 8) | (unsigned)pixel[2];

		/* Green is the expected outcome. */
		verdict = "ok";
		if (got != 0x00ff00U) {
			verdict = "DIFFERS";
			failures++;
		}

		/* The square's line. */
		printf("EGLTEST PIXEL run=%s name=%s got=%06x expected=00ff00 %s\n", token, feedback_names[index], got, verdict);
	}

	/* The readbacks raised no error. */
	error = glGetError();
	printf("EGLTEST CHECK run=%s failures=%d glerror=0x%x\n", token, failures, (unsigned)error);
	fflush(stdout);
	if (error != GL_NO_ERROR)
		failures++;

	/* Succeeded: how many differed. */
	return failures;
}

/* Makes a program of two shaders' sources, capturing outputs of names in a mode; 0 with a line when it fails. */
static GLuint
feedback_program(
	const char *vertex_source,
	const char *fragment_source,
	const GLchar *const *varyings,
	GLsizei count,
	GLenum mode)
{
	static const GLenum types[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
	const char *sources[2];
	GLuint shaders[2];
	GLuint program;
	GLint status;
	unsigned index;
	char log[512];

	/* The two shaders. */
	sources[0] = vertex_source;
	sources[1] = fragment_source;
	program = glCreateProgram();
	for (index = 0U; index < 2U; index++) {
		shaders[index] = glCreateShader(types[index]);
		glShaderSource(shaders[index], 1, &sources[index], NULL);
		glCompileShader(shaders[index]);
		status = GL_FALSE;
		glGetShaderiv(shaders[index], GL_COMPILE_STATUS, &status);
		if (!status) {
			log[0] = '\0';
			glGetShaderInfoLog(shaders[index], (GLsizei)sizeof(log), NULL, log);
			printf("EGLTEST FEEDBACK compile failed: %s\n", log);
			return 0U;
		}

		/* Attached. */
		glAttachShader(program, shaders[index]);
		glDeleteShader(shaders[index]);
	}

	/* The outputs captured, then the link. */
	if (count > 0)
		glTransformFeedbackVaryings(program, count, varyings, mode);
	glLinkProgram(program);
	status = GL_FALSE;
	glGetProgramiv(program, GL_LINK_STATUS, &status);
	if (!status) {
		log[0] = '\0';
		glGetProgramInfoLog(program, (GLsizei)sizeof(log), NULL, log);
		printf("EGLTEST FEEDBACK link failed: %s\n", log);
		return 0U;
	}

	/* Succeeded: the program. */
	return program;
}

/*
 * Captures a triangle's interleaved outputs, then a strip's two triangles
 * with the primitives-written query, and checks the values.
 */
static void
feedback_capture(void)
{
	const GLfloat *words;
	const GLint *ints;
	GLuint query;
	GLuint primitives;
	GLint value;
	GLsizei size;
	GLenum type;
	GLchar name[32];
	int right;

	/* The program's captured outputs. */
	value = 0;
	glGetProgramiv(feedback_interleaved, GL_TRANSFORM_FEEDBACK_VARYINGS, &value);
	feedback_expect("varyings", (long)value, 3L);
	memset(name, 0, sizeof(name));
	glGetTransformFeedbackVarying(feedback_interleaved, 1U, (GLsizei)sizeof(name), NULL, &size, &type, name);
	feedback_expect("varying-1-type", (long)type, (long)GL_FLOAT_VEC4);
	feedback_expect("varying-1-name", (long)strcmp(name, "v_colour"), 0L);

	/* A triangle captured with rasterization discarded: 3 vertices of 9 words. */
	glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, feedback_buffers[0]);
	glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 64 * 4 * FEEDBACK_RECORD, NULL, GL_DYNAMIC_READ);
	glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0U, feedback_buffers[0]);
	glUseProgram(feedback_interleaved);
	glUniform2f(feedback_shift, 0.0f, 0.0f);
	glEnable(GL_RASTERIZER_DISCARD);
	glBeginTransformFeedback(GL_TRIANGLES);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glEndTransformFeedback();

	/* Vertex 1: position (1, -1, 0, 1), colour (1, 0, 0.25, 0), index 1. */
	words = glMapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 3 * 4 * FEEDBACK_RECORD, GL_MAP_READ_BIT);
	right = 0;
	if (words != NULL) {
		ints = (const GLint *)(const void *)words;
		right = feedback_close(words[9], 1.0f) && feedback_close(words[10], -1.0f) && feedback_close(words[12], 1.0f) &&
			feedback_close(words[13], 1.0f) && feedback_close(words[15], 0.25f) && ints[17] == 1 && ints[26] == 2;
		(void)glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER);
	}

	/* The outcome. */
	feedback_outcomes[0] = right;

	/* A strip of four vertices: two triangles, six vertices, counted by the query. */
	glGenQueries(1, &query);
	glBeginQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, query);
	glBeginTransformFeedback(GL_TRIANGLES);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glEndTransformFeedback();
	glEndQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN);
	primitives = 0U;
	glGetQueryObjectuiv(query, GL_QUERY_RESULT, &primitives);
	feedback_expect("primitives-written", (long)primitives, 2L);

	/* The first triangle is vertices 0, 1, 2; the second holds vertex 3 (its index somewhere among its three). */
	words = glMapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 6 * 4 * FEEDBACK_RECORD, GL_MAP_READ_BIT);
	right = 0;
	if (words != NULL) {
		ints = (const GLint *)(const void *)words;
		right = primitives == 2U && ints[8] == 0 && ints[17] == 1 && ints[26] == 2 &&
			(ints[35] == 3 || ints[44] == 3 || ints[53] == 3);
		(void)glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER);
	}

	/* The outcome. */
	feedback_outcomes[1] = right;
	glDisable(GL_RASTERIZER_DISCARD);
}

/*
 * Captures separate outputs into two buffers, instances, nothing while
 * paused, and a square's positions that the frames draw from.
 */
static void
feedback_more(void)
{
	static const GLfloat quad[12] = {
		0.05f, -0.25f, 0.45f, -0.25f, 0.05f, 0.25f, 0.05f, 0.25f, 0.45f, -0.25f, 0.45f, 0.25f
	};
	static const GLfloat corners[8] = { -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };
	const GLfloat *words;
	const GLint *ints;
	GLuint scratch;
	int right;

	/* Separate outputs: the colours into buffer 1, the indices into buffer 2. */
	glEnable(GL_RASTERIZER_DISCARD);
	glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, feedback_buffers[1]);
	glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 256, NULL, GL_DYNAMIC_READ);
	glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, feedback_buffers[2]);
	glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 256, NULL, GL_DYNAMIC_READ);
	glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0U, feedback_buffers[1]);
	glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 1U, feedback_buffers[2]);
	glUseProgram(feedback_separate);
	glBeginTransformFeedback(GL_TRIANGLES);
	glDrawArraysInstanced(GL_TRIANGLES, 0, 3, 2);
	glEndTransformFeedback();

	/* Buffer 2: indices 0 1 2 for each instance; buffer 1: instance 1's colours have alpha 1. */
	glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, feedback_buffers[2]);
	ints = glMapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 6 * 4, GL_MAP_READ_BIT);
	right = 0;
	if (ints != NULL) {
		right = ints[0] == 0 && ints[1] == 1 && ints[2] == 2 && ints[3] == 0 && ints[5] == 2;
		(void)glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER);
	}

	/* The outcome. */
	feedback_outcomes[2] = right;
	glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, feedback_buffers[1]);
	words = glMapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 6 * 16, GL_MAP_READ_BIT);
	right = 0;
	if (words != NULL) {
		right = feedback_close(words[3], 0.0f) && feedback_close(words[15], 1.0f) && feedback_close(words[23], 1.0f);
		(void)glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER);
	}

	/* The outcome. */
	feedback_outcomes[3] = right;

	/* Paused: the draw captures nothing (the buffer keeps the marker written before). */
	glGenBuffers(1, &scratch);
	glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, scratch);
	glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 3 * 4 * FEEDBACK_RECORD, NULL, GL_DYNAMIC_READ);
	glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0U, scratch);
	glUseProgram(feedback_interleaved);
	glBeginTransformFeedback(GL_TRIANGLES);
	glPauseTransformFeedback();
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glResumeTransformFeedback();
	glEndTransformFeedback();
	words = glMapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 4 * FEEDBACK_RECORD, GL_MAP_READ_BIT);
	right = 0;
	if (words != NULL) {
		right = feedback_close(words[3], 0.0f);
		(void)glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER);
	}

	/* The outcome. */
	feedback_outcomes[4] = right;

	/* The six corners of a square in the seventh cell (x 0.25, y 0; half size 0.2 by 0.25) captured. */
	glBindBuffer(GL_ARRAY_BUFFER, feedback_corners);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(quad), quad, GL_STATIC_DRAW);
	glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, feedback_buffers[0]);
	glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 6 * 4 * FEEDBACK_RECORD, NULL, GL_STATIC_DRAW);
	glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0U, feedback_buffers[0]);
	glUseProgram(feedback_interleaved);
	glUniform2f(feedback_shift, 0.0f, 0.0f);
	glBeginTransformFeedback(GL_TRIANGLES);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	glEndTransformFeedback();
	glDisable(GL_RASTERIZER_DISCARD);

	/* The captured positions (vec4, a record apart) are what the frames draw the square from. */
	glBindVertexArray(feedback_arrays[1]);
	glBindBuffer(GL_ARRAY_BUFFER, feedback_buffers[0]);
	glVertexAttribPointer(FEEDBACK_POSITION, 4, GL_FLOAT, GL_FALSE, 4 * FEEDBACK_RECORD, NULL);
	glEnableVertexAttribArray(FEEDBACK_POSITION);
	glBindVertexArray(feedback_arrays[0]);

	/* The corners back to the unit square's strip. */
	glBindBuffer(GL_ARRAY_BUFFER, feedback_corners);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(corners), corners, GL_STATIC_DRAW);
}

/*
 * Checks the errors of draws and binds transform feedback refuses while
 * it is active, and of beginning it without a buffer.
 */
static void
feedback_errors(void)
{
	static const GLushort indices[3] = { 0U, 1U, 2U };
	GLint value;

	/* Active: indices, another kind of primitive, and a rebinding are refused. */
	glEnable(GL_RASTERIZER_DISCARD);
	glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0U, feedback_buffers[0]);
	glUseProgram(feedback_interleaved);
	glBeginTransformFeedback(GL_TRIANGLES);
	value = 0;
	glGetIntegerv(GL_TRANSFORM_FEEDBACK_ACTIVE, &value);
	feedback_expect("active", (long)value, 1L);
	glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, indices);
	feedback_expect("draw-elements", (long)glGetError(), (long)GL_INVALID_OPERATION);
	glDrawArrays(GL_LINES, 0, 2);
	feedback_expect("other-mode", (long)glGetError(), (long)GL_INVALID_OPERATION);
	glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0U, feedback_buffers[1]);
	feedback_expect("rebind-active", (long)glGetError(), (long)GL_INVALID_OPERATION);
	glEndTransformFeedback();
	glDisable(GL_RASTERIZER_DISCARD);

	/* No buffer bound: beginning is refused. */
	glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0U, 0U);
	glBeginTransformFeedback(GL_TRIANGLES);
	feedback_expect("begin-without-buffer", (long)glGetError(), (long)GL_INVALID_OPERATION);
}

/* Reports whether a float is within a small distance of the one expected. */
static int
feedback_close(
	float got,
	float expected)
{
	float difference;

	/* The distance. */
	difference = got - expected;
	if (difference < 0.0f)
		difference = -difference;
	if (difference > 0.001f)
		return 0;

	/* Close. */
	return 1;
}

/* Prints one of the start's checks, counting it when the value is not the one expected. */
static void
feedback_expect(
	const char *what,
	long got,
	long expected)
{
	const char *verdict;

	/* A value that differs is a failure. */
	verdict = "ok";
	if (got != expected) {
		verdict = "FAILED";
		feedback_failures++;
	}

	/* The line. */
	printf("EGLTEST FEEDBACK check %s got=%ld expected=%ld %s\n", what, got, expected, verdict);
}
