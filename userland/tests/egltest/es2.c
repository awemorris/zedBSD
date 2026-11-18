/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * egltest's OpenGL ES 2.0 built-ins scene (WS068 p004).  Each frame draws
 * gl_FragCoord.y / height as red over the window; the first frame's check
 * then reads back, in GL's coordinates (rows from the bottom):
 *
 *   gl_FragCoord.y on framebuffer 0 and in a framebuffer object: dark at
 *   the bottom row, bright at the top;
 *   gl_PointCoord.t of a large point on framebuffer 0 and in the
 *   framebuffer object: 0 at the point's top, 1 at its bottom (GL's
 *   default GL_POINT_SPRITE_COORD_ORIGIN, upper left);
 *   gl_DepthRange after glDepthRangef(0.25, 0.75): near, far and diff;
 *   an array of two samplers given units 0 and 1 by glUniform1iv (red and
 *   green add to yellow), then its second element unit 2 by glUniform1i
 *   (red and blue add to magenta), and what glGetUniformiv,
 *   glGetActiveUniform and glGetUniformLocation report of it and of the
 *   built-ins.
 */

#include "es2.h"

#include <GLES2/gl2.h>

#include <stdio.h>
#include <string.h>

/* The framebuffer object's size, the point's size, and the attribute location of the corners. */
#define ES2_TARGET	64
#define ES2_POINT	32.0f
#define ES2_POSITION	0U

/*
 * The programs (gl_FragCoord, gl_PointCoord, gl_DepthRange, the sampler
 * array) and their uniforms, the corners' buffer, the framebuffer object
 * and its texture, the three 1x1 textures, and the start's failed checks.
 */
static GLuint es2_coord_program;
static GLint es2_coord_height;
static GLuint es2_point_program;
static GLint es2_point_size;
static GLuint es2_range_program;
static GLuint es2_array_program;
static GLint es2_array_location;
static GLint es2_array_second;
static GLuint es2_corners;
static GLuint es2_framebuffer;
static GLuint es2_target;
static GLuint es2_textures[3];
static GLfloat es2_point_largest;
static int es2_failures;

/* The vertex shader of the quads: the corners as they are. */
static const char es2_quad_vertex[] =
	"attribute vec2 a_position;\n"
	"void main()\n"
	"{\n"
	"\tgl_Position = vec4(a_position, 0.0, 1.0);\n"
	"}\n";

/* gl_FragCoord.y over the target's height as red. */
static const char es2_coord_fragment[] =
	"precision highp float;\n"
	"uniform float u_height;\n"
	"void main()\n"
	"{\n"
	"\tgl_FragColor = vec4(gl_FragCoord.y / u_height, 0.0, 0.0, 1.0);\n"
	"}\n";

/* The vertex shader of the point: one point at the middle, of a size. */
static const char es2_point_vertex[] =
	"attribute vec2 a_position;\n"
	"uniform float u_size;\n"
	"void main()\n"
	"{\n"
	"\tgl_PointSize = u_size;\n"
	"\tgl_Position = vec4(0.0, 0.0, 0.0, 1.0) + vec4(a_position * 0.0, 0.0, 0.0);\n"
	"}\n";

/* gl_PointCoord as red and green. */
static const char es2_point_fragment[] =
	"precision mediump float;\n"
	"void main()\n"
	"{\n"
	"\tgl_FragColor = vec4(gl_PointCoord, 0.0, 1.0);\n"
	"}\n";

/* gl_DepthRange's near, far and diff as red, green and blue. */
static const char es2_range_fragment[] =
	"precision highp float;\n"
	"void main()\n"
	"{\n"
	"\tgl_FragColor = vec4(gl_DepthRange.near, gl_DepthRange.far, gl_DepthRange.diff, 1.0);\n"
	"}\n";

/* The two samplers' texels added. */
static const char es2_array_fragment[] =
	"precision mediump float;\n"
	"uniform sampler2D u_textures[2];\n"
	"void main()\n"
	"{\n"
	"\tgl_FragColor = texture2D(u_textures[0], vec2(0.5)) + texture2D(u_textures[1], vec2(0.5));\n"
	"}\n";

static GLuint es2_program(const char *vertex_source, const char *fragment_source);
static GLuint es2_shader(GLenum type, const char *source);
static void es2_quad(GLuint program);
static void es2_point(int width, int height);
static int es2_pixel(const char *token, const char *name, int x, int y, int channel, int low, int high);
static void es2_expect(const char *what, long got, long expected);
static void es2_reports(void);

/*
 * Makes the programs, the corners, the framebuffer object and the three
 * 1x1 textures (red, green, blue on units 0, 1 and 2).  Returns 0, or -1
 * with a line saying what failed.
 */
int
egltest_es2_start(void)
{
	static const GLfloat corners[8] = { -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };
	static const GLubyte texels[3][4] = { { 255U, 0U, 0U, 255U }, { 0U, 255U, 0U, 255U }, { 0U, 0U, 255U, 255U } };
	GLfloat range[2];
	GLenum status;
	unsigned index;

	/* The programs. */
	es2_coord_program = es2_program(es2_quad_vertex, es2_coord_fragment);
	es2_point_program = es2_program(es2_point_vertex, es2_point_fragment);
	es2_range_program = es2_program(es2_quad_vertex, es2_range_fragment);
	es2_array_program = es2_program(es2_quad_vertex, es2_array_fragment);
	if (es2_coord_program == 0U || es2_point_program == 0U || es2_range_program == 0U || es2_array_program == 0U)
		return -1;
	es2_coord_height = glGetUniformLocation(es2_coord_program, "u_height");
	es2_point_size = glGetUniformLocation(es2_point_program, "u_size");
	es2_array_location = glGetUniformLocation(es2_array_program, "u_textures");
	es2_array_second = glGetUniformLocation(es2_array_program, "u_textures[1]");
	if (es2_coord_height < 0 || es2_point_size < 0 || es2_array_location < 0 || es2_array_second < 0) {
		printf("EGLTEST ES2 uniforms missing\n");
		return -1;
	}

	/* The corners. */
	glGenBuffers(1, &es2_corners);
	glBindBuffer(GL_ARRAY_BUFFER, es2_corners);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(corners), corners, GL_STATIC_DRAW);
	glVertexAttribPointer(ES2_POSITION, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glEnableVertexAttribArray(ES2_POSITION);

	/* The framebuffer object with its colour texture. */
	glGenTextures(1, &es2_target);
	glBindTexture(GL_TEXTURE_2D, es2_target);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ES2_TARGET, ES2_TARGET, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &es2_framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, es2_framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, es2_target, 0);
	status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	glBindFramebuffer(GL_FRAMEBUFFER, 0U);
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		printf("EGLTEST ES2 framebuffer incomplete 0x%x\n", (unsigned)status);
		return -1;
	}

	/* The three 1x1 textures, each on its unit. */
	glGenTextures(3, es2_textures);
	for (index = 0U; index < 3U; index++) {
		glActiveTexture(GL_TEXTURE0 + index);
		glBindTexture(GL_TEXTURE_2D, es2_textures[index]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels[index]);
	}

	/* Unit 0 active again. */
	glActiveTexture(GL_TEXTURE0);

	/* The largest point the device draws (at least the size the check needs, else the point checks are noted and skipped). */
	range[0] = 1.0f;
	range[1] = 1.0f;
	glGetFloatv(GL_ALIASED_POINT_SIZE_RANGE, range);
	es2_point_largest = range[1];

	/* What the API reports of the sampler array and the built-ins. */
	es2_reports();
	es2_expect("start-glerror", (long)glGetError(), (long)GL_NO_ERROR);

	/* Succeeded: ready to draw. */
	printf("EGLTEST ES2 ready point-largest=%.0f failures=%d\n", (double)es2_point_largest, es2_failures);
	fflush(stdout);
	return 0;
}

/*
 * Draws gl_FragCoord.y / height as red over the window.
 */
void
egltest_es2_draw(
	int width,
	int height)
{
	/* The whole window. */
	glBindFramebuffer(GL_FRAMEBUFFER, 0U);
	glViewport(0, 0, width, height);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	/* The gradient. */
	glUseProgram(es2_coord_program);
	glUniform1f(es2_coord_height, (GLfloat)height);
	es2_quad(es2_coord_program);
}

/*
 * Reads back the window's gradient, then draws and reads the framebuffer
 * object's gradient, the points, the depth range and the sampler array.
 * Returns how many readings differ, with the start's failed checks and
 * any error.
 */
int
egltest_es2_check(
	int width,
	int height,
	const char *token)
{
	static const GLint units[2] = { 0, 1 };
	GLenum error;
	int failures;
	int middle;

	/* gl_FragCoord.y on framebuffer 0: the window's bottom row dark, its top row bright. */
	failures = es2_failures;
	middle = width / 2;
	failures += es2_pixel(token, "fragcoord-window-bottom", middle, 1, 0, 0, 16);
	failures += es2_pixel(token, "fragcoord-window-top", middle, height - 2, 0, 239, 255);

	/* gl_PointCoord.t on framebuffer 0: small near the point's top, large near its bottom. */
	if (es2_point_largest >= ES2_POINT) {
		es2_point(width, height);
		failures += es2_pixel(token, "pointcoord-window-top", middle, height / 2 + 12, 1, 0, 64);
		failures += es2_pixel(token, "pointcoord-window-bottom", middle, height / 2 - 12, 1, 191, 255);
	}

	/* The same two in the framebuffer object. */
	glBindFramebuffer(GL_FRAMEBUFFER, es2_framebuffer);
	glViewport(0, 0, ES2_TARGET, ES2_TARGET);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glUseProgram(es2_coord_program);
	glUniform1f(es2_coord_height, (GLfloat)ES2_TARGET);
	es2_quad(es2_coord_program);
	failures += es2_pixel(token, "fragcoord-fbo-bottom", ES2_TARGET / 2, 1, 0, 0, 16);
	failures += es2_pixel(token, "fragcoord-fbo-top", ES2_TARGET / 2, ES2_TARGET - 2, 0, 239, 255);
	if (es2_point_largest >= ES2_POINT) {
		es2_point(ES2_TARGET, ES2_TARGET);
		failures += es2_pixel(token, "pointcoord-fbo-top", ES2_TARGET / 2, ES2_TARGET / 2 + 12, 1, 0, 64);
		failures += es2_pixel(token, "pointcoord-fbo-bottom", ES2_TARGET / 2, ES2_TARGET / 2 - 12, 1, 191, 255);
	}

	/* gl_DepthRange after glDepthRangef(0.25, 0.75): 64, 191 and 128 (within one). */
	glDepthRangef(0.25f, 0.75f);
	es2_quad(es2_range_program);
	glDepthRangef(0.0f, 1.0f);
	failures += es2_pixel(token, "depthrange-near", 8, 8, 0, 63, 65);
	failures += es2_pixel(token, "depthrange-far", 8, 8, 1, 190, 192);
	failures += es2_pixel(token, "depthrange-diff", 8, 8, 2, 127, 129);

	/* The sampler array on units 0 and 1: red and green, yellow. */
	glUseProgram(es2_array_program);
	glUniform1iv(es2_array_location, 2, units);
	es2_quad(es2_array_program);
	failures += es2_pixel(token, "samplers-red", 8, 8, 0, 254, 255);
	failures += es2_pixel(token, "samplers-green", 8, 8, 1, 254, 255);

	/* Its second element on unit 2: red and blue, magenta. */
	glUniform1i(es2_array_second, 2);
	es2_quad(es2_array_program);
	failures += es2_pixel(token, "samplers-second-green", 8, 8, 1, 0, 1);
	failures += es2_pixel(token, "samplers-second-blue", 8, 8, 2, 254, 255);
	glBindFramebuffer(GL_FRAMEBUFFER, 0U);
	glViewport(0, 0, width, height);

	/* The readbacks raised no error. */
	error = glGetError();
	printf("EGLTEST CHECK run=%s failures=%d glerror=0x%x\n", token, failures, (unsigned)error);
	fflush(stdout);
	if (error != GL_NO_ERROR)
		failures++;

	/* Succeeded: how many differed. */
	return failures;
}

/* Makes a program of two shaders' sources with the corners at location 0; 0 with a line when it fails. */
static GLuint
es2_program(
	const char *vertex_source,
	const char *fragment_source)
{
	GLuint vertex;
	GLuint fragment;
	GLuint program;
	GLint linked;

	/* The shaders. */
	vertex = es2_shader(GL_VERTEX_SHADER, vertex_source);
	fragment = es2_shader(GL_FRAGMENT_SHADER, fragment_source);
	if (vertex == 0U || fragment == 0U)
		return 0U;

	/* Linked with the corners at location 0. */
	program = glCreateProgram();
	glAttachShader(program, vertex);
	glAttachShader(program, fragment);
	glBindAttribLocation(program, ES2_POSITION, "a_position");
	glLinkProgram(program);
	glDeleteShader(vertex);
	glDeleteShader(fragment);
	linked = GL_FALSE;
	glGetProgramiv(program, GL_LINK_STATUS, &linked);
	if (!linked) {
		printf("EGLTEST ES2 link failed\n");
		return 0U;
	}

	/* Succeeded: the program. */
	return program;
}

/* Makes a shader from GLSL ES 1.00 source; 0 with a line when it does not compile. */
static GLuint
es2_shader(
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
		printf("EGLTEST ES2 compile failed: %s\n", log);
		return 0U;
	}

	/* Succeeded: the shader. */
	return shader;
}

/* Draws the whole viewport with a program. */
static void
es2_quad(
	GLuint program)
{
	/* The corners as a strip. */
	glUseProgram(program);
	glBindBuffer(GL_ARRAY_BUFFER, es2_corners);
	glVertexAttribPointer(ES2_POSITION, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glEnableVertexAttribArray(ES2_POSITION);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* Draws the large point at the middle of a viewport of a size. */
static void
es2_point(
	int width,
	int height)
{
	/* The viewport, and the one point. */
	glViewport(0, 0, width, height);
	glUseProgram(es2_point_program);
	glUniform1f(es2_point_size, ES2_POINT);
	glBindBuffer(GL_ARRAY_BUFFER, es2_corners);
	glVertexAttribPointer(ES2_POSITION, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glEnableVertexAttribArray(ES2_POSITION);
	glDrawArrays(GL_POINTS, 0, 1);
}

/* Reads one channel of a pixel (GL's coordinates) and prints it; returns 1 when it is outside [low, high]. */
static int
es2_pixel(
	const char *token,
	const char *name,
	int x,
	int y,
	int channel,
	int low,
	int high)
{
	GLubyte pixel[4];
	const char *verdict;
	int value;

	/* The pixel. */
	memset(pixel, 0, sizeof(pixel));
	glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
	value = (int)pixel[channel];

	/* Inside the range is the expected value. */
	verdict = "ok";
	if (value < low || value > high)
		verdict = "DIFFERS";

	/* The line. */
	printf("EGLTEST PIXEL run=%s name=%s x=%d y=%d channel=%d got=%d expected=%d..%d %s\n", token, name, x, y, channel, value, low,
	       high, verdict);
	if (value < low || value > high)
		return 1;

	/* As expected. */
	return 0;
}

/* Prints one of the start's checks, counting it when the value is not the one expected. */
static void
es2_expect(
	const char *what,
	long got,
	long expected)
{
	const char *verdict;

	/* A value that differs is a failure. */
	verdict = "ok";
	if (got != expected) {
		verdict = "FAILED";
		es2_failures++;
	}

	/* The line. */
	printf("EGLTEST ES2 check %s got=%ld expected=%ld %s\n", what, got, expected, verdict);
}

/*
 * Checks what the API reports: the sampler array's units, size and name,
 * the location of its second element, and that the built-in uniforms
 * have no location and the hidden one is not listed.
 */
static void
es2_reports(void)
{
	static const GLint units[2] = { 0, 1 };
	GLint value;
	GLint count;
	GLint size;
	GLenum type;
	GLint index;
	GLint hidden;
	char name[64];
	int differs;

	/* The units glUniform1iv gives, read back per element. */
	glUseProgram(es2_array_program);
	glUniform1iv(es2_array_location, 2, units);
	value = -1;
	glGetUniformiv(es2_array_program, es2_array_second, &value);
	es2_expect("array-second-unit", (long)value, 1L);
	es2_expect("array-second-location", (long)es2_array_second, (long)(es2_array_location + 1));

	/* The array listed as one uniform of two samplers, "u_textures[0]". */
	count = 0;
	glGetProgramiv(es2_array_program, GL_ACTIVE_UNIFORMS, &count);
	es2_expect("array-active-uniforms", (long)count, 1L);
	size = 0;
	type = GL_NONE;
	name[0] = '\0';
	glGetActiveUniform(es2_array_program, 0U, (GLsizei)sizeof(name), NULL, &size, &type, name);
	es2_expect("array-size", (long)size, 2L);
	es2_expect("array-type", (long)type, (long)GL_SAMPLER_2D);
	differs = strcmp(name, "u_textures[0]");
	es2_expect("array-name", (long)differs, 0L);

	/* The built-ins have no location; the hidden uniform is not listed. */
	es2_expect("depthrange-location", (long)glGetUniformLocation(es2_range_program, "gl_DepthRange.near"), -1L);
	es2_expect("zed-location", (long)glGetUniformLocation(es2_coord_program, "gl_ZedFragment"), -1L);
	count = 0;
	glGetProgramiv(es2_coord_program, GL_ACTIVE_UNIFORMS, &count);
	hidden = 0;
	for (index = 0; index < count; index++) {
		name[0] = '\0';
		glGetActiveUniform(es2_coord_program, (GLuint)index, (GLsizei)sizeof(name), NULL, &size, &type, name);
		differs = strncmp(name, "gl_Zed", 6U);
		if (differs == 0)
			hidden++;
	}

	/* None hidden is listed; the coordinate program has its one uniform. */
	es2_expect("zed-listed", (long)hidden, 0L);
	es2_expect("coord-active-uniforms", (long)count, 1L);
}
