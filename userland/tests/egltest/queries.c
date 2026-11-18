/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * egltest's OpenGL ES 3.0 query and sync scene (WS068 p027): the start
 * runs four occlusion queries and a fence sync, and each frame shows one
 * square per outcome, green when it is the expected one and red
 * otherwise:
 *
 *   a square drawn behind the depth buffer (GL_ANY_SAMPLES_PASSED false),
 *   a square drawn in front of it (true), a conservative query over a
 *   hidden draw into a framebuffer object and a seen one into the window
 *   (true across the two passes), a query of no draw at all (false), and
 *   a fence sync waited for (signalled).
 *
 * The start also checks what the API reports: the current query, result
 * availability, the errors of wrong queries, the sync's properties,
 * glGetFragDataLocation, and the refused program binaries.
 */

#include "queries.h"

#include <GLES3/gl3.h>

#include <stdio.h>
#include <string.h>

/* The outcomes shown, and the attribute location of the squares' corners. */
#define QUERIES_OUTCOMES	5U
#define QUERIES_POSITION	0U

/*
 * The program and its uniforms, the square's vertex array, the queries,
 * the framebuffer object and its texture and depth, and each outcome
 * (nonzero: the expected one); kept for the run with the number of the
 * start's checks that failed.
 */
static GLuint queries_program;
static GLint queries_rect;
static GLint queries_depth;
static GLint queries_colour;
static GLuint queries_array;
static GLuint queries_ids[4];
static GLuint queries_framebuffer;
static GLuint queries_texture;
static GLuint queries_renderbuffer;
static int queries_outcomes[QUERIES_OUTCOMES];
static int queries_failures;

/* The vertex shader: a unit square placed by the rectangle (centre, half size) at a depth. */
static const char queries_vertex_source[] =
	"#version 300 es\n"
	"layout(location = 0) in vec2 a_position;\n"
	"uniform vec4 u_rect;\n"
	"uniform float u_depth;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\tgl_Position = vec4(u_rect.xy + a_position * u_rect.zw, u_depth, 1.0);\n"
	"}\n";

/* The fragment shader: one colour, at output location 0 named o_colour. */
static const char queries_fragment_source[] =
	"#version 300 es\n"
	"precision mediump float;\n"
	"uniform vec4 u_colour;\n"
	"layout(location = 0) out vec4 o_colour;\n"
	"void main()\n"
	"{\n"
	"\to_colour = u_colour;\n"
	"}\n";

/* The names of the outcomes. */
static const char *const queries_names[QUERIES_OUTCOMES] = {
	"occlusion-hidden", "occlusion-seen", "conservative-two-passes", "occlusion-no-draw", "fence-signalled"
};

static GLuint queries_shader(GLenum type, const char *source);
static void queries_quad(GLfloat x, GLfloat y, GLfloat width, GLfloat height, GLfloat depth, GLfloat red, GLfloat green);
static GLuint queries_result(GLuint id);
static void queries_occlusion(void);
static void queries_sync(void);
static void queries_program_checks(void);
static void queries_expect(const char *what, long got, long expected);

/*
 * Makes the program and the square, runs the queries and the sync, and
 * checks what the API reports.  Returns 0, or -1 with a line saying what
 * failed.
 */
int
egltest_queries_start(void)
{
	static const GLfloat corners[8] = { -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };
	GLuint vertex;
	GLuint fragment;
	GLuint buffer;
	GLint linked;
	GLenum error;

	/* The program. */
	vertex = queries_shader(GL_VERTEX_SHADER, queries_vertex_source);
	fragment = queries_shader(GL_FRAGMENT_SHADER, queries_fragment_source);
	if (vertex == 0U || fragment == 0U)
		return -1;
	queries_program = glCreateProgram();
	glAttachShader(queries_program, vertex);
	glAttachShader(queries_program, fragment);
	glLinkProgram(queries_program);
	glDeleteShader(vertex);
	glDeleteShader(fragment);
	linked = GL_FALSE;
	glGetProgramiv(queries_program, GL_LINK_STATUS, &linked);
	if (!linked) {
		printf("EGLTEST QUERIES link failed\n");
		return -1;
	}

	/* Its uniforms. */
	glUseProgram(queries_program);
	queries_rect = glGetUniformLocation(queries_program, "u_rect");
	queries_depth = glGetUniformLocation(queries_program, "u_depth");
	queries_colour = glGetUniformLocation(queries_program, "u_colour");
	if (queries_rect < 0 || queries_depth < 0 || queries_colour < 0) {
		printf("EGLTEST QUERIES uniforms missing\n");
		return -1;
	}

	/* The square's corners in a vertex array of their own. */
	glGenVertexArrays(1, &queries_array);
	glBindVertexArray(queries_array);
	glGenBuffers(1, &buffer);
	glBindBuffer(GL_ARRAY_BUFFER, buffer);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(corners), corners, GL_STATIC_DRAW);
	glVertexAttribPointer(QUERIES_POSITION, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glEnableVertexAttribArray(QUERIES_POSITION);

	/* The queries, the sync and the program's checks. */
	queries_occlusion();
	queries_sync();
	queries_program_checks();
	glBindVertexArray(0U);

	/* Everything went without an error the checks did not expect. */
	error = glGetError();
	queries_expect("start-glerror", (long)error, (long)GL_NO_ERROR);

	/* Succeeded: ready to draw. */
	printf("EGLTEST QUERIES ready failures=%d\n", queries_failures);
	fflush(stdout);
	return 0;
}

/*
 * Draws one square per outcome over a window of a size, green when it is
 * the expected one and red otherwise.
 */
void
egltest_queries_draw(
	int width,
	int height)
{
	unsigned index;
	GLfloat green;

	/* The whole window, cleared to dark grey. */
	glBindFramebuffer(GL_FRAMEBUFFER, 0U);
	glViewport(0, 0, width, height);
	glClearColor(0.125f, 0.125f, 0.125f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glBindVertexArray(queries_array);

	/* Each outcome in a cell of the top row and the next. */
	for (index = 0U; index < QUERIES_OUTCOMES; index++) {
		green = 0.0f;
		if (queries_outcomes[index])
			green = 1.0f;
		queries_quad(-0.75f + 0.5f * (GLfloat)(index % 4U), 0.66f - 0.66f * (GLfloat)(index / 4U), 0.2f, 0.25f, 0.0f,
			     1.0f - green, green);
	}

	/* The vertex array is let go. */
	glBindVertexArray(0U);
}

/*
 * Reads back each square's centre and prints the colours; returns how
 * many are not green, with the start's failed checks and any error.
 */
int
egltest_queries_check(
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
	failures = queries_failures;
	for (index = 0U; index < QUERIES_OUTCOMES; index++) {
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
		printf("EGLTEST PIXEL run=%s name=%s got=%06x expected=00ff00 %s\n", token, queries_names[index], got, verdict);
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

/* Makes a shader from GLSL source; 0 with a line when it does not compile. */
static GLuint
queries_shader(
	GLenum type,
	const char *source)
{
	GLuint shader;
	GLint compiled;
	char log[512];

	/* The shader compiled from the source. */
	shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	compiled = GL_FALSE;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
	if (!compiled) {
		log[0] = '\0';
		glGetShaderInfoLog(shader, (GLsizei)sizeof(log), NULL, log);
		printf("EGLTEST QUERIES compile failed: %s\n", log);
		return 0U;
	}

	/* Succeeded: the shader. */
	return shader;
}

/* Draws the square placed by a rectangle (centre, half size) at a depth, in red and green. */
static void
queries_quad(
	GLfloat x,
	GLfloat y,
	GLfloat width,
	GLfloat height,
	GLfloat depth,
	GLfloat red,
	GLfloat green)
{
	/* The uniforms, then the strip. */
	glUseProgram(queries_program);
	glUniform4f(queries_rect, x, y, width, height);
	glUniform1f(queries_depth, depth);
	glUniform4f(queries_colour, red, green, 0.0f, 1.0f);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* Returns a query's result once it is available. */
static GLuint
queries_result(
	GLuint id)
{
	GLuint available;
	GLuint result;

	/* Available, then the result. */
	available = GL_FALSE;
	glGetQueryObjectuiv(id, GL_QUERY_RESULT_AVAILABLE, &available);
	queries_expect("result-available", (long)available, (long)GL_TRUE);
	result = 0U;
	glGetQueryObjectuiv(id, GL_QUERY_RESULT, &result);

	/* Succeeded: the result. */
	return result;
}

/*
 * Runs the occlusion queries: a square behind the depth buffer, one in
 * front, a conservative one over a hidden draw into a framebuffer object
 * and a seen one into the window, and one of no draw; checks the current
 * query and the errors of wrong queries.
 */
static void
queries_occlusion(void)
{
	GLuint result;
	GLint value;

	/* The window's depth made near (0.25) over its whole size, tested with less. */
	glGenQueries(4, queries_ids);
	glBindFramebuffer(GL_FRAMEBUFFER, 0U);
	glClearDepthf(0.25f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glClearDepthf(1.0f);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);

	/* Behind it (0.75): no sample passes. */
	glBeginQuery(GL_ANY_SAMPLES_PASSED, queries_ids[0]);
	value = 0;
	glGetQueryiv(GL_ANY_SAMPLES_PASSED, GL_CURRENT_QUERY, &value);
	queries_expect("current-query", (long)value, (long)queries_ids[0]);
	glGetQueryObjectuiv(queries_ids[0], GL_QUERY_RESULT, &result);
	queries_expect("result-of-active", (long)glGetError(), (long)GL_INVALID_OPERATION);
	glBeginQuery(GL_ANY_SAMPLES_PASSED_CONSERVATIVE, queries_ids[1]);
	queries_expect("second-occlusion", (long)glGetError(), (long)GL_INVALID_OPERATION);
	queries_quad(0.0f, 0.0f, 0.5f, 0.5f, 0.5f, 1.0f, 0.0f);
	glEndQuery(GL_ANY_SAMPLES_PASSED);
	result = queries_result(queries_ids[0]);
	queries_outcomes[0] = 0;
	if (result == GL_FALSE)
		queries_outcomes[0] = 1;

	/* In front of it (-0.75, depth 0.125): samples pass. */
	glBeginQuery(GL_ANY_SAMPLES_PASSED, queries_ids[1]);
	queries_quad(0.0f, 0.0f, 0.5f, 0.5f, -0.75f, 1.0f, 0.0f);
	glEndQuery(GL_ANY_SAMPLES_PASSED);
	result = queries_result(queries_ids[1]);
	queries_outcomes[1] = 0;
	if (result == GL_TRUE)
		queries_outcomes[1] = 1;

	/* A framebuffer object with an RGBA8 texture and a depth renderbuffer cleared near. */
	glGenTextures(1, &queries_texture);
	glBindTexture(GL_TEXTURE_2D, queries_texture);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 8, 8);
	glGenRenderbuffers(1, &queries_renderbuffer);
	glBindRenderbuffer(GL_RENDERBUFFER, queries_renderbuffer);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, 8, 8);
	glGenFramebuffers(1, &queries_framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, queries_framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, queries_texture, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, queries_renderbuffer);
	glViewport(0, 0, 8, 8);
	glClearDepthf(0.25f);
	glClear(GL_DEPTH_BUFFER_BIT);
	glClearDepthf(1.0f);

	/* A conservative query over a hidden draw there, then a seen one in the window: true across both passes. */
	glBeginQuery(GL_ANY_SAMPLES_PASSED_CONSERVATIVE, queries_ids[2]);
	queries_quad(0.0f, 0.0f, 1.0f, 1.0f, 0.5f, 1.0f, 0.0f);
	glBindFramebuffer(GL_FRAMEBUFFER, 0U);
	queries_quad(0.0f, 0.0f, 0.5f, 0.5f, -0.75f, 1.0f, 0.0f);
	glEndQuery(GL_ANY_SAMPLES_PASSED_CONSERVATIVE);
	result = queries_result(queries_ids[2]);
	queries_outcomes[2] = 0;
	if (result == GL_TRUE)
		queries_outcomes[2] = 1;

	/* A query of no draw: false. */
	glBeginQuery(GL_ANY_SAMPLES_PASSED, queries_ids[3]);
	glEndQuery(GL_ANY_SAMPLES_PASSED);
	result = queries_result(queries_ids[3]);
	queries_outcomes[3] = 0;
	if (result == GL_FALSE)
		queries_outcomes[3] = 1;

	/* The names are queries now; name 0 cannot be begun. */
	queries_expect("is-query", (long)glIsQuery(queries_ids[0]), (long)GL_TRUE);
	glBeginQuery(GL_ANY_SAMPLES_PASSED, 0U);
	queries_expect("begin-zero", (long)glGetError(), (long)GL_INVALID_OPERATION);
	glDisable(GL_DEPTH_TEST);
}

/*
 * Makes a fence sync, waits for it, and checks its properties and its
 * deletion.
 */
static void
queries_sync(void)
{
	GLsync sync;
	GLenum waited;
	GLint value;
	GLsizei length;

	/* A fence after a draw, waited for with a flush. */
	queries_quad(0.0f, 0.0f, 0.1f, 0.1f, 0.0f, 0.0f, 0.0f);
	sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0U);
	queries_expect("is-sync", (long)glIsSync(sync), (long)GL_TRUE);
	value = 0;
	length = 0;
	glGetSynciv(sync, GL_OBJECT_TYPE, 1, &length, &value);
	queries_expect("sync-type", (long)value, (long)GL_SYNC_FENCE);
	waited = glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000U);
	queries_outcomes[4] = 0;
	if (waited == GL_CONDITION_SATISFIED || waited == GL_ALREADY_SIGNALED)
		queries_outcomes[4] = 1;

	/* Signalled now; a server wait is taken; deleted it is no sync. */
	value = 0;
	glGetSynciv(sync, GL_SYNC_STATUS, 1, &length, &value);
	queries_expect("sync-status", (long)value, (long)GL_SIGNALED);
	glWaitSync(sync, 0U, GL_TIMEOUT_IGNORED);
	glDeleteSync(sync);
	queries_expect("deleted-sync", (long)glIsSync(sync), (long)GL_FALSE);
}

/*
 * Checks glGetFragDataLocation and that program binaries are refused.
 */
static void
queries_program_checks(void)
{
	GLint value;
	GLsizei length;
	GLenum format;
	GLubyte binary[4];

	/* The output's location, and none for another name. */
	queries_expect("frag-data-location", (long)glGetFragDataLocation(queries_program, "o_colour"), 0L);
	queries_expect("frag-data-missing", (long)glGetFragDataLocation(queries_program, "o_other"), -1L);

	/* No binary format. */
	value = -1;
	glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &value);
	queries_expect("binary-formats", (long)value, 0L);
	glGetProgramBinary(queries_program, (GLsizei)sizeof(binary), &length, &format, binary);
	queries_expect("get-binary", (long)glGetError(), (long)GL_INVALID_OPERATION);
}

/* Prints one of the start's checks, counting it when the value is not the one expected. */
static void
queries_expect(
	const char *what,
	long got,
	long expected)
{
	const char *verdict;

	/* A value that differs is a failure. */
	verdict = "ok";
	if (got != expected) {
		verdict = "FAILED";
		queries_failures++;
	}

	/* The line. */
	printf("EGLTEST QUERIES check %s got=%ld expected=%ld %s\n", what, got, expected, verdict);
}
