/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * glxtest's OpenGL 3.0 scene (WS068 p013), in a context made by
 * glXCreateContextAttribsARB: the start checks OpenGL 3.0's calls in a
 * framebuffer object of two colour textures, and each frame shows one
 * square per outcome, green when it is the expected one and red
 * otherwise, and one square drawn with the fixed function (the context
 * is a compatibility one):
 *
 *   the version (GL_VERSION, GLSL 1.30, GL_MAJOR_VERSION), the outputs
 *   glBindFragDataLocation placed, a draw buffer's own colour mask, a
 *   draw buffer's own blending, conditional rendering, GL_SAMPLES_PASSED,
 *   glMapBuffer and glGetBufferSubData, glGetTexImage, the integer vertex
 *   attributes of fewer components, glFramebufferTexture3D, and
 *   glDrawBuffer with the integer texture parameters.
 *
 * The start's checks each print a line; a check whose value is not the
 * expected one fails the run.
 */

#include "gl3.h"

#include <GL/gl.h>

#include <stdio.h>
#include <string.h>

/* The outcomes shown, the side of the framebuffer object's textures, and the attribute locations. */
#define GL3_OUTCOMES		11U
#define GL3_SIDE		8
#define GL3_POSITION		0U
#define GL3_VALUE		1U

/* The colours the checks read back, as 0xRRGGBB. */
#define GL3_BLACK		0x000000U
#define GL3_RED			0xff0000U
#define GL3_GREEN		0x00ff00U
#define GL3_BLUE		0x0000ffU
#define GL3_YELLOW		0xffff00U
#define GL3_WHITE		0xffffffU

/*
 * The programs (one colour; two outputs placed by glBindFragDataLocation;
 * an integer attribute compared) with their rectangle and colour
 * uniforms, the square's vertex array, the framebuffer object with its
 * two textures, and each outcome (nonzero: the expected one); kept for
 * the run with the number of the start's checks that failed.
 */
static GLuint gl3_plain;
static GLint gl3_plain_rect;
static GLint gl3_plain_colour;
static GLuint gl3_outputs;
static GLint gl3_outputs_rect;
static GLint gl3_outputs_colour;
static GLuint gl3_integer;
static GLint gl3_integer_rect;
static GLuint gl3_array;
static GLuint gl3_framebuffer;
static GLuint gl3_textures[2];
static int gl3_outcomes[GL3_OUTCOMES];
static int gl3_failures;

/* The vertex shader: a unit square placed by the rectangle (centre, half size). */
static const char gl3_vertex_source[] =
	"#version 130\n"
	"in vec2 a_position;\n"
	"uniform vec4 u_rect;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\tgl_Position = vec4(u_rect.xy + a_position * u_rect.zw, 0.0, 1.0);\n"
	"}\n";

/* The fragment shader of one colour. */
static const char gl3_plain_source[] =
	"#version 130\n"
	"uniform vec4 u_colour;\n"
	"out vec4 o_colour;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\to_colour = u_colour;\n"
	"}\n";

/* The fragment shader of two outputs: the colour, and blue (glBindFragDataLocation puts them at 1 and 0). */
static const char gl3_outputs_source[] =
	"#version 130\n"
	"uniform vec4 u_colour;\n"
	"out vec4 o_first;\n"
	"out vec4 o_second;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\to_first = u_colour;\n"
	"\to_second = vec4(0.0, 0.0, 1.0, 1.0);\n"
	"}\n";

/* The vertex shader that hands on an integer attribute (its current value: the array is off). */
static const char gl3_integer_vertex_source[] =
	"#version 130\n"
	"in vec2 a_position;\n"
	"in ivec4 a_value;\n"
	"uniform vec4 u_rect;\n"
	"flat out ivec4 v_value;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\tv_value = a_value;\n"
	"\tgl_Position = vec4(u_rect.xy + a_position * u_rect.zw, 0.0, 1.0);\n"
	"}\n";

/* The fragment shader that shows green when the integer is (-3, 7, 11, 1), red otherwise. */
static const char gl3_integer_fragment_source[] =
	"#version 130\n"
	"flat in ivec4 v_value;\n"
	"out vec4 o_colour;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\to_colour = vec4(1.0, 0.0, 0.0, 1.0);\n"
	"\tif (v_value == ivec4(-3, 7, 11, 1))\n"
	"\t\to_colour = vec4(0.0, 1.0, 0.0, 1.0);\n"
	"}\n";

/* The names of the outcomes. */
static const char *const gl3_names[GL3_OUTCOMES] = {
	"version", "frag-data-location", "color-mask-indexed", "blend-indexed", "conditional-render", "samples-passed",
	"map-buffer", "get-tex-image", "vertex-attrib-integer", "framebuffer-texture-3d", "draw-buffer-parameters"
};

static GLuint gl3_program(const char *vertex_source, const char *fragment_source, int placed);
static GLuint gl3_shader(GLenum type, const char *source);
static void gl3_quad(GLuint program, GLint rect, GLint colour, GLfloat x, GLfloat y, GLfloat width, GLfloat height, unsigned rgb);
static unsigned gl3_read(GLenum attachment);
static void gl3_clear(unsigned rgb);
static void gl3_version(void);
static void gl3_frag_data(void);
static void gl3_masks(void);
static void gl3_blend(void);
static void gl3_conditional(void);
static void gl3_buffers(void);
static void gl3_get_tex_image(void);
static void gl3_integers(void);
static void gl3_volume(void);
static void gl3_draw_buffer(void);
static int gl3_expect(const char *what, long got, long expected);

/*
 * Makes the programs, the square and the framebuffer object, and runs
 * the checks.  Returns 0, or -1 with a line saying what failed.
 */
int
glxtest_gl3_start(void)
{
	static const GLfloat corners[8] = { -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };
	static const GLenum buffers[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
	GLuint buffer;
	GLenum complete;
	GLenum error;
	unsigned index;

	/* The three programs and their uniforms. */
	gl3_plain = gl3_program(gl3_vertex_source, gl3_plain_source, 0);
	gl3_outputs = gl3_program(gl3_vertex_source, gl3_outputs_source, 1);
	gl3_integer = gl3_program(gl3_integer_vertex_source, gl3_integer_fragment_source, 0);
	if (gl3_plain == 0U || gl3_outputs == 0U || gl3_integer == 0U)
		return -1;
	gl3_plain_rect = glGetUniformLocation(gl3_plain, "u_rect");
	gl3_plain_colour = glGetUniformLocation(gl3_plain, "u_colour");
	gl3_outputs_rect = glGetUniformLocation(gl3_outputs, "u_rect");
	gl3_outputs_colour = glGetUniformLocation(gl3_outputs, "u_colour");
	gl3_integer_rect = glGetUniformLocation(gl3_integer, "u_rect");

	/* The square's corners in a vertex array of their own. */
	glGenVertexArrays(1, &gl3_array);
	glBindVertexArray(gl3_array);
	glGenBuffers(1, &buffer);
	glBindBuffer(GL_ARRAY_BUFFER, buffer);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(corners), corners, GL_STATIC_DRAW);
	glVertexAttribPointer(GL3_POSITION, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glEnableVertexAttribArray(GL3_POSITION);
	glBindBuffer(GL_ARRAY_BUFFER, 0U);

	/* The framebuffer object: two RGBA8 textures, both drawn into. */
	glGenTextures(2, gl3_textures);
	for (index = 0U; index < 2U; index++) {
		glBindTexture(GL_TEXTURE_2D, gl3_textures[index]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GL3_SIDE, GL3_SIDE, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	}

	/* The textures attached, both draw buffers written. */
	glGenFramebuffers(1, &gl3_framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, gl3_framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gl3_textures[0], 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, gl3_textures[1], 0);
	glDrawBuffers(2, buffers);
	glViewport(0, 0, GL3_SIDE, GL3_SIDE);
	complete = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (complete != GL_FRAMEBUFFER_COMPLETE) {
		printf("GLXTEST GL3 framebuffer incomplete 0x%x\n", (unsigned)complete);
		return -1;
	}

	/* The checks, each setting its outcome. */
	gl3_version();
	gl3_frag_data();
	gl3_masks();
	gl3_blend();
	gl3_conditional();
	gl3_buffers();
	gl3_get_tex_image();
	gl3_integers();
	gl3_volume();
	gl3_draw_buffer();

	/* The window again, and no error the checks did not expect. */
	glBindFramebuffer(GL_FRAMEBUFFER, 0U);
	glBindVertexArray(0U);
	error = glGetError();
	(void)gl3_expect("start-glerror", (long)error, (long)GL_NO_ERROR);

	/* Succeeded: ready to draw. */
	printf("GLXTEST GL3 ready failures=%d\n", gl3_failures);
	fflush(stdout);
	return 0;
}

/*
 * Draws one square per outcome over a window of a size, green when it is
 * the expected one and red otherwise, and a green square with glBegin in
 * the last cell.
 */
void
glxtest_gl3_draw(
	int width,
	int height)
{
	unsigned index;
	unsigned rgb;
	GLfloat x;
	GLfloat y;

	/* The whole window, cleared to dark grey. */
	glBindFramebuffer(GL_FRAMEBUFFER, 0U);
	glViewport(0, 0, width, height);
	glClearColor(0.125f, 0.125f, 0.125f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glBindVertexArray(gl3_array);

	/* Each outcome in a cell of a grid of four columns. */
	for (index = 0U; index < GL3_OUTCOMES; index++) {
		rgb = GL3_RED;
		if (gl3_outcomes[index])
			rgb = GL3_GREEN;
		x = -0.75f + 0.5f * (GLfloat)(index % 4U);
		y = 0.66f - 0.66f * (GLfloat)(index / 4U);
		gl3_quad(gl3_plain, gl3_plain_rect, gl3_plain_colour, x, y, 0.2f, 0.25f, rgb);
	}

	/* The last cell with the fixed function, as the compatibility profile keeps it. */
	glBindVertexArray(0U);
	glUseProgram(0U);
	glColor3f(0.0f, 1.0f, 0.0f);
	glBegin(GL_QUADS);
	glVertex2f(0.55f, -0.91f);
	glVertex2f(0.95f, -0.91f);
	glVertex2f(0.95f, -0.41f);
	glVertex2f(0.55f, -0.41f);
	glEnd();
}

/*
 * Reads back each square's centre and prints the colours; returns how
 * many are not green, with the start's failed checks and any error.
 */
int
glxtest_gl3_check(
	int width,
	int height,
	const char *token)
{
	GLubyte pixel[4];
	GLenum error;
	const char *verdict;
	const char *name;
	unsigned got;
	unsigned index;
	int failures;
	int x;
	int y;

	/* Each square's centre (the fixed-function one last), in GL's coordinates. */
	failures = gl3_failures;
	for (index = 0U; index <= GL3_OUTCOMES; index++) {
		x = (int)((-0.75f + 0.5f * (float)(index % 4U) + 1.0f) * 0.5f * (float)width);
		y = (int)((0.66f - 0.66f * (float)(index / 4U) + 1.0f) * 0.5f * (float)height);
		memset(pixel, 0, sizeof(pixel));
		glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
		got = ((unsigned)pixel[0] << 16) | ((unsigned)pixel[1] << 8) | (unsigned)pixel[2];

		/* Green is the expected outcome. */
		verdict = "ok";
		if (got != GL3_GREEN) {
			verdict = "DIFFERS";
			failures++;
		}

		/* The square's line. */
		name = "fixed-function";
		if (index < GL3_OUTCOMES)
			name = gl3_names[index];
		printf("EGLTEST PIXEL run=%s name=%s got=%06x expected=00ff00 %s\n", token, name, got, verdict);
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

/* Makes a program of two GLSL sources (placed: its outputs put at draw buffers 1 and 0 before the link); 0 with a line on failure. */
static GLuint
gl3_program(
	const char *vertex_source,
	const char *fragment_source,
	int placed)
{
	GLuint program;
	GLuint vertex;
	GLuint fragment;
	GLint linked;
	char log[512];

	/* The two shaders. */
	vertex = gl3_shader(GL_VERTEX_SHADER, vertex_source);
	if (vertex == 0U)
		return 0U;
	fragment = gl3_shader(GL_FRAGMENT_SHADER, fragment_source);
	if (fragment == 0U)
		return 0U;

	/* The program, the position at location 0 and the integer at 1. */
	program = glCreateProgram();
	glAttachShader(program, vertex);
	glAttachShader(program, fragment);
	glBindAttribLocation(program, GL3_POSITION, "a_position");
	glBindAttribLocation(program, GL3_VALUE, "a_value");

	/* The outputs the other way round from their declarations. */
	if (placed) {
		glBindFragDataLocation(program, 1U, "o_first");
		glBindFragDataLocation(program, 0U, "o_second");
	}

	/* Linked. */
	glLinkProgram(program);
	glDeleteShader(vertex);
	glDeleteShader(fragment);
	linked = GL_FALSE;
	glGetProgramiv(program, GL_LINK_STATUS, &linked);
	if (!linked) {
		log[0] = '\0';
		glGetProgramInfoLog(program, (GLsizei)sizeof(log), NULL, log);
		printf("GLXTEST GL3 link failed: %s\n", log);
		return 0U;
	}

	/* Succeeded: the program. */
	return program;
}

/* Makes a shader from GLSL source; 0 with a line when it does not compile. */
static GLuint
gl3_shader(
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
		printf("GLXTEST GL3 compile failed: %s\n", log);
		return 0U;
	}

	/* Succeeded: the shader. */
	return shader;
}

/* Draws the square placed by a rectangle (centre, half size) with a program, in a colour when it has a colour uniform. */
static void
gl3_quad(
	GLuint program,
	GLint rect,
	GLint colour,
	GLfloat x,
	GLfloat y,
	GLfloat width,
	GLfloat height,
	unsigned rgb)
{
	GLfloat red;
	GLfloat green;
	GLfloat blue;

	/* The colour's channels. */
	red = (GLfloat)((rgb >> 16) & 0xffU) / 255.0f;
	green = (GLfloat)((rgb >> 8) & 0xffU) / 255.0f;
	blue = (GLfloat)(rgb & 0xffU) / 255.0f;

	/* The uniforms, then the strip. */
	glUseProgram(program);
	glUniform4f(rect, x, y, width, height);
	if (colour >= 0)
		glUniform4f(colour, red, green, blue, 1.0f);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* Returns the centre texel of one of the framebuffer object's attachments as 0xRRGGBB. */
static unsigned
gl3_read(
	GLenum attachment)
{
	GLubyte pixel[4];
	unsigned rgb;

	/* The attachment's centre. */
	memset(pixel, 0, sizeof(pixel));
	glReadBuffer(attachment);
	glReadPixels(GL3_SIDE / 2, GL3_SIDE / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
	rgb = ((unsigned)pixel[0] << 16) | ((unsigned)pixel[1] << 8) | (unsigned)pixel[2];

	/* Succeeded: the colour. */
	return rgb;
}

/* Clears the draw buffers of the framebuffer object to a colour. */
static void
gl3_clear(
	unsigned rgb)
{
	GLfloat red;
	GLfloat green;
	GLfloat blue;

	/* The colour's channels, cleared with. */
	red = (GLfloat)((rgb >> 16) & 0xffU) / 255.0f;
	green = (GLfloat)((rgb >> 8) & 0xffU) / 255.0f;
	blue = (GLfloat)(rgb & 0xffU) / 255.0f;
	glClearColor(red, green, blue, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
}

/* Checks the version the context reports: OpenGL 3.0 with GLSL 1.30. */
static void
gl3_version(void)
{
	const char *version;
	const char *language;
	GLint major;
	GLint minor;
	int passed;
	int differs;

	/* The strings. */
	version = (const char *)glGetString(GL_VERSION);
	language = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
	printf("GLXTEST GL3 version=\"%s\" glsl=\"%s\"\n", version, language);
	passed = 1;
	differs = strncmp(version, "3.0 ", 4U);
	passed &= gl3_expect("version-string", (long)differs, 0L);
	differs = strcmp(language, "1.30");
	passed &= gl3_expect("glsl-version", (long)differs, 0L);

	/* The numbers. */
	major = 0;
	minor = -1;
	glGetIntegerv(GL_MAJOR_VERSION, &major);
	glGetIntegerv(GL_MINOR_VERSION, &minor);
	passed &= gl3_expect("major-version", (long)major, 3L);
	passed &= gl3_expect("minor-version", (long)minor, 0L);
	gl3_outcomes[0] = passed;
}

/* Checks the outputs glBindFragDataLocation placed: o_first at draw buffer 1, o_second at 0. */
static void
gl3_frag_data(void)
{
	GLint location;
	int passed;

	/* The locations the program reports. */
	passed = 1;
	location = glGetFragDataLocation(gl3_outputs, "o_first");
	passed &= gl3_expect("frag-data-first", (long)location, 1L);
	location = glGetFragDataLocation(gl3_outputs, "o_second");
	passed &= gl3_expect("frag-data-second", (long)location, 0L);

	/* A red draw: blue into attachment 0, red into 1. */
	gl3_clear(GL3_BLACK);
	gl3_quad(gl3_outputs, gl3_outputs_rect, gl3_outputs_colour, 0.0f, 0.0f, 1.0f, 1.0f, GL3_RED);
	passed &= gl3_expect("frag-data-attachment0", (long)gl3_read(GL_COLOR_ATTACHMENT0), (long)GL3_BLUE);
	passed &= gl3_expect("frag-data-attachment1", (long)gl3_read(GL_COLOR_ATTACHMENT1), (long)GL3_RED);

	/* A built-in's name is refused. */
	glBindFragDataLocation(gl3_outputs, 0U, "gl_FragColor");
	passed &= gl3_expect("frag-data-builtin", (long)glGetError(), (long)GL_INVALID_OPERATION);
	gl3_outcomes[1] = passed;
}

/* Checks a draw buffer's own colour mask: draws and clears leave buffer 1 alone, and glColorMask makes them alike again. */
static void
gl3_masks(void)
{
	GLboolean mask[4];
	int passed;

	/* Buffer 1 masked: what glGetBooleani_v reports for both. */
	passed = 1;
	gl3_clear(GL3_BLACK);
	glColorMaski(1U, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	memset(mask, 0xff, sizeof(mask));
	glGetBooleani_v(GL_COLOR_WRITEMASK, 1U, mask);
	passed &= gl3_expect("mask-1-red", (long)mask[0], (long)GL_FALSE);
	passed &= gl3_expect("mask-1-alpha", (long)mask[3], (long)GL_FALSE);
	glGetBooleani_v(GL_COLOR_WRITEMASK, 0U, mask);
	passed &= gl3_expect("mask-0-red", (long)mask[0], (long)GL_TRUE);

	/* A draw writes buffer 0 only. */
	gl3_quad(gl3_outputs, gl3_outputs_rect, gl3_outputs_colour, 0.0f, 0.0f, 1.0f, 1.0f, GL3_RED);
	passed &= gl3_expect("mask-draw-0", (long)gl3_read(GL_COLOR_ATTACHMENT0), (long)GL3_BLUE);
	passed &= gl3_expect("mask-draw-1", (long)gl3_read(GL_COLOR_ATTACHMENT1), (long)GL3_BLACK);

	/* A clear too. */
	gl3_clear(GL3_WHITE);
	passed &= gl3_expect("mask-clear-0", (long)gl3_read(GL_COLOR_ATTACHMENT0), (long)GL3_WHITE);
	passed &= gl3_expect("mask-clear-1", (long)gl3_read(GL_COLOR_ATTACHMENT1), (long)GL3_BLACK);

	/* glColorMask sets every buffer's again. */
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glGetBooleani_v(GL_COLOR_WRITEMASK, 1U, mask);
	passed &= gl3_expect("mask-reset", (long)mask[0], (long)GL_TRUE);
	gl3_outcomes[2] = passed;
}

/* Checks a draw buffer's own blending: added into buffer 1 only, and glDisable turns it off everywhere. */
static void
gl3_blend(void)
{
	GLboolean on;
	int passed;

	/* Both green; blending (adding) on buffer 1 only. */
	passed = 1;
	gl3_clear(GL3_GREEN);
	glBlendFunc(GL_ONE, GL_ONE);
	glEnablei(GL_BLEND, 1U);
	on = glIsEnabledi(GL_BLEND, 1U);
	passed &= gl3_expect("blend-on-1", (long)on, (long)GL_TRUE);
	on = glIsEnabledi(GL_BLEND, 0U);
	passed &= gl3_expect("blend-off-0", (long)on, (long)GL_FALSE);

	/* A red draw: blue replaces buffer 0's green, red adds to buffer 1's. */
	gl3_quad(gl3_outputs, gl3_outputs_rect, gl3_outputs_colour, 0.0f, 0.0f, 1.0f, 1.0f, GL3_RED);
	passed &= gl3_expect("blend-draw-0", (long)gl3_read(GL_COLOR_ATTACHMENT0), (long)GL3_BLUE);
	passed &= gl3_expect("blend-draw-1", (long)gl3_read(GL_COLOR_ATTACHMENT1), (long)GL3_YELLOW);

	/* Off everywhere. */
	glDisable(GL_BLEND);
	on = glIsEnabledi(GL_BLEND, 1U);
	passed &= gl3_expect("blend-reset", (long)on, (long)GL_FALSE);
	glBlendFunc(GL_ONE, GL_ZERO);
	gl3_outcomes[3] = passed;
}

/*
 * Checks conditional rendering and GL_SAMPLES_PASSED: a query of no draw
 * counts none and its conditional rendering skips a draw and a clear; a
 * query of a whole draw counts the texels and lets a draw through.
 */
static void
gl3_conditional(void)
{
	GLuint queries[2];
	GLuint counted;
	GLint signed_count;
	int passed;
	int counts;
	int within;

	/* A query of nothing, and one of a draw over the 64 texels. */
	passed = 1;
	counts = 1;
	glGenQueries(2, queries);
	glBeginQuery(GL_SAMPLES_PASSED, queries[0]);
	glEndQuery(GL_SAMPLES_PASSED);
	glBeginQuery(GL_SAMPLES_PASSED, queries[1]);
	gl3_quad(gl3_outputs, gl3_outputs_rect, gl3_outputs_colour, 0.0f, 0.0f, 1.0f, 1.0f, GL3_RED);
	glEndQuery(GL_SAMPLES_PASSED);

	/* Their counts (the draw's is all the texels when the device counts exactly, at least some otherwise). */
	counted = 1U;
	glGetQueryObjectuiv(queries[0], GL_QUERY_RESULT, &counted);
	counts &= gl3_expect("samples-none", (long)counted, 0L);
	signed_count = 0;
	glGetQueryObjectiv(queries[1], GL_QUERY_RESULT, &signed_count);
	printf("GLXTEST GL3 samples-passed=%d of %d\n", (int)signed_count, GL3_SIDE * GL3_SIDE);
	within = 0;
	if (signed_count > 0 && signed_count <= GL3_SIDE * GL3_SIDE)
		within = 1;
	counts &= gl3_expect("samples-some", (long)within, 1L);
	gl3_outcomes[5] = counts;

	/* Rendering on the query of nothing: neither a draw nor a clear happens. */
	gl3_clear(GL3_BLACK);
	glBeginConditionalRender(queries[0], GL_QUERY_WAIT);
	gl3_quad(gl3_outputs, gl3_outputs_rect, gl3_outputs_colour, 0.0f, 0.0f, 1.0f, 1.0f, GL3_RED);
	gl3_clear(GL3_WHITE);
	glEndConditionalRender();
	passed &= gl3_expect("conditional-skipped", (long)gl3_read(GL_COLOR_ATTACHMENT0), (long)GL3_BLACK);

	/* On the query of the draw: the draw happens. */
	glBeginConditionalRender(queries[1], GL_QUERY_NO_WAIT);
	gl3_quad(gl3_outputs, gl3_outputs_rect, gl3_outputs_colour, 0.0f, 0.0f, 1.0f, 1.0f, GL3_RED);
	glEndConditionalRender();
	passed &= gl3_expect("conditional-drawn", (long)gl3_read(GL_COLOR_ATTACHMENT1), (long)GL3_RED);

	/* Ending twice, and a query that is not one, are refused. */
	glEndConditionalRender();
	passed &= gl3_expect("conditional-end-twice", (long)glGetError(), (long)GL_INVALID_OPERATION);
	glBeginConditionalRender(12345U, GL_QUERY_WAIT);
	passed &= gl3_expect("conditional-no-query", (long)glGetError(), (long)GL_INVALID_OPERATION);
	glDeleteQueries(2, queries);
	gl3_outcomes[4] = passed;
}

/* Checks glMapBuffer (whole buffer, each access) and glGetBufferSubData. */
static void
gl3_buffers(void)
{
	GLuint buffer;
	GLuint words[16];
	GLuint *mapped;
	GLint state;
	unsigned index;
	int passed;
	int same;

	/* A buffer of 16 words, written through a whole mapping. */
	passed = 1;
	glGenBuffers(1, &buffer);
	glBindBuffer(GL_COPY_WRITE_BUFFER, buffer);
	glBufferData(GL_COPY_WRITE_BUFFER, (GLsizeiptr)sizeof(words), NULL, GL_STATIC_DRAW);
	mapped = glMapBuffer(GL_COPY_WRITE_BUFFER, GL_WRITE_ONLY);
	same = 0;
	if (mapped != NULL)
		same = 1;
	passed &= gl3_expect("map-write", (long)same, 1L);
	for (index = 0U; mapped != NULL && index < 16U; index++)
		mapped[index] = index * 3U + 1U;
	(void)glUnmapBuffer(GL_COPY_WRITE_BUFFER);

	/* Words 4 to 11 read back. */
	memset(words, 0, sizeof(words));
	glGetBufferSubData(GL_COPY_WRITE_BUFFER, 16, 32, words);
	same = 1;
	for (index = 0U; index < 8U; index++) {
		if (words[index] != (index + 4U) * 3U + 1U)
			same = 0;
	}

	/* The words as written. */
	passed &= gl3_expect("get-buffer-sub-data", (long)same, 1L);

	/* The whole buffer mapped for reading, reported mapped. */
	mapped = glMapBuffer(GL_COPY_WRITE_BUFFER, GL_READ_ONLY);
	state = GL_FALSE;
	glGetBufferParameteriv(GL_COPY_WRITE_BUFFER, GL_BUFFER_MAPPED, &state);
	passed &= gl3_expect("map-read-mapped", (long)state, (long)GL_TRUE);
	same = 0;
	if (mapped != NULL && mapped[15] == 46U)
		same = 1;
	passed &= gl3_expect("map-read", (long)same, 1L);
	(void)glUnmapBuffer(GL_COPY_WRITE_BUFFER);

	/* A wrong access is refused. */
	mapped = glMapBuffer(GL_COPY_WRITE_BUFFER, GL_STATIC_DRAW);
	passed &= gl3_expect("map-bad-access", (long)glGetError(), (long)GL_INVALID_ENUM);
	glBindBuffer(GL_COPY_WRITE_BUFFER, 0U);
	glDeleteBuffers(1, &buffer);
	gl3_outcomes[6] = passed;
}

/* Checks glGetTexImage: a texture given texels, and one the GPU drew into (attachment 1, red). */
static void
gl3_get_tex_image(void)
{
	GLubyte texels[4 * 4 * 4];
	GLubyte read[GL3_SIDE * GL3_SIDE * 4];
	GLuint texture;
	unsigned index;
	int passed;
	int same;
	int differs;

	/* A 4x4 texture of known bytes, read back the same. */
	passed = 1;
	for (index = 0U; index < sizeof(texels); index++)
		texels[index] = (GLubyte)(index * 7U + 3U);
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
	memset(read, 0, sizeof(read));
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, read);
	differs = memcmp(read, texels, sizeof(texels));
	same = 0;
	if (differs == 0)
		same = 1;
	passed &= gl3_expect("get-tex-image-texels", (long)same, 1L);
	glDeleteTextures(1, &texture);

	/* Attachment 1 (red from the conditional draw), every texel. */
	glBindTexture(GL_TEXTURE_2D, gl3_textures[1]);
	memset(read, 0, sizeof(read));
	glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, read);
	same = 1;
	for (index = 0U; index < GL3_SIDE * GL3_SIDE; index++) {
		if (read[index * 4U] != 0xffU ||
		    read[index * 4U + 1U] != 0U ||
		    read[index * 4U + 2U] != 0U)
			same = 0;
	}

	/* Red everywhere. */
	passed &= gl3_expect("get-tex-image-drawn", (long)same, 1L);

	/* The framebuffer object stays the draw framebuffer. */
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	passed &= gl3_expect("get-tex-image-glerror", (long)glGetError(), (long)GL_NO_ERROR);
	gl3_outcomes[7] = passed;
}

/* Checks the integer attributes of fewer components: (-3, 7, 11) with w 1 shows green, (0, 0, 0, 1) red. */
static void
gl3_integers(void)
{
	static const GLbyte bytes[4] = { -3, 7, 11, 1 };
	GLint current[4];
	int passed;

	/* Three components (w is 1). */
	passed = 1;
	gl3_clear(GL3_BLACK);
	glVertexAttribI3i(GL3_VALUE, -3, 7, 11);
	memset(current, 0, sizeof(current));
	glGetVertexAttribIiv(GL3_VALUE, GL_CURRENT_VERTEX_ATTRIB, current);
	passed &= gl3_expect("attrib-i3i-w", (long)current[3], 1L);
	gl3_quad(gl3_integer, gl3_integer_rect, -1, 0.0f, 0.0f, 1.0f, 1.0f, 0U);
	passed &= gl3_expect("attrib-i3i", (long)gl3_read(GL_COLOR_ATTACHMENT0), (long)GL3_GREEN);

	/* One component: the others 0 and 1. */
	glVertexAttribI1i(GL3_VALUE, -3);
	gl3_quad(gl3_integer, gl3_integer_rect, -1, 0.0f, 0.0f, 1.0f, 1.0f, 0U);
	passed &= gl3_expect("attrib-i1i", (long)gl3_read(GL_COLOR_ATTACHMENT0), (long)GL3_RED);

	/* Four signed bytes. */
	glVertexAttribI4bv(GL3_VALUE, bytes);
	gl3_quad(gl3_integer, gl3_integer_rect, -1, 0.0f, 0.0f, 1.0f, 1.0f, 0U);
	passed &= gl3_expect("attrib-i4bv", (long)gl3_read(GL_COLOR_ATTACHMENT0), (long)GL3_GREEN);
	gl3_outcomes[8] = passed;
}

/* Checks glFramebufferTexture3D: the middle slice of a 3D texture cleared green, the others left black. */
static void
gl3_volume(void)
{
	GLubyte texels[4 * 4 * 3 * 4];
	GLuint framebuffer;
	GLuint texture;
	GLenum complete;
	unsigned index;
	unsigned green;
	int passed;
	int same;

	/* A black 4x4x3 texture. */
	passed = 1;
	memset(texels, 0, sizeof(texels));
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_3D, texture);
	glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, 4, 4, 3, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);

	/* Its slice 1 as the colour attachment of a framebuffer, cleared green. */
	glGenFramebuffers(1, &framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glFramebufferTexture3D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_3D, texture, 0, 1);
	complete = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	passed &= gl3_expect("volume-complete", (long)complete, (long)GL_FRAMEBUFFER_COMPLETE);
	glViewport(0, 0, 4, 4);
	gl3_clear(GL3_GREEN);

	/* Every slice read back: only slice 1 green. */
	memset(texels, 0x55, sizeof(texels));
	glGetTexImage(GL_TEXTURE_3D, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
	same = 1;
	for (index = 0U; index < 4U * 4U * 3U; index++) {
		green = 0U;
		if (index / 16U == 1U)
			green = 0xffU;
		if (texels[index * 4U] != 0U || texels[index * 4U + 1U] != green)
			same = 0;
	}

	/* The slices as cleared. */
	passed &= gl3_expect("volume-slices", (long)same, 1L);

	/* A 3D attachment named with another target, and a 1D texture, are refused. */
	glFramebufferTexture3D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0, 0);
	passed &= gl3_expect("volume-wrong-target", (long)glGetError(), (long)GL_INVALID_OPERATION);
	glFramebufferTexture1D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_1D, texture, 0);
	passed &= gl3_expect("texture-1d", (long)glGetError(), (long)GL_INVALID_OPERATION);

	/* Back to the test's framebuffer object. */
	glBindFramebuffer(GL_FRAMEBUFFER, gl3_framebuffer);
	glViewport(0, 0, GL3_SIDE, GL3_SIDE);
	glDeleteFramebuffers(1, &framebuffer);
	glDeleteTextures(1, &texture);
	gl3_outcomes[9] = passed;
}

/*
 * Checks glDrawBuffer (one attachment of a framebuffer object), the
 * integer texture parameters and glClampColor.
 */
static void
gl3_draw_buffer(void)
{
	static const GLenum buffers[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
	static const GLint nearest = GL_NEAREST;
	static const GLuint linear = GL_LINEAR;
	static const GLint border[4] = { 1, 2, 3, 4 };
	GLint value;
	GLuint unsigned_value;
	int passed;

	/* Both black, then a red clear of attachment 1 alone. */
	passed = 1;
	gl3_clear(GL3_BLACK);
	glDrawBuffer(GL_COLOR_ATTACHMENT1);
	value = 0;
	glGetIntegerv(GL_DRAW_BUFFER0, &value);
	passed &= gl3_expect("draw-buffer-0", (long)value, (long)GL_COLOR_ATTACHMENT1);
	gl3_clear(GL3_RED);
	passed &= gl3_expect("draw-buffer-cleared", (long)gl3_read(GL_COLOR_ATTACHMENT1), (long)GL3_RED);
	passed &= gl3_expect("draw-buffer-kept", (long)gl3_read(GL_COLOR_ATTACHMENT0), (long)GL3_BLACK);
	glDrawBuffers(2, buffers);

	/* The integer texture parameters, and the border colour there is none of. */
	glBindTexture(GL_TEXTURE_2D, gl3_textures[0]);
	glTexParameterIiv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &nearest);
	value = 0;
	glGetTexParameterIiv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &value);
	passed &= gl3_expect("tex-parameter-iiv", (long)value, (long)GL_NEAREST);
	glTexParameterIuiv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &linear);
	unsigned_value = 0U;
	glGetTexParameterIuiv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &unsigned_value);
	passed &= gl3_expect("tex-parameter-iuiv", (long)unsigned_value, (long)GL_LINEAR);
	glTexParameterIiv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
	passed &= gl3_expect("tex-parameter-border", (long)glGetError(), (long)GL_INVALID_ENUM);

	/* Colour clamping is taken, a wrong target refused. */
	glClampColor(GL_CLAMP_READ_COLOR, GL_FALSE);
	passed &= gl3_expect("clamp-color", (long)glGetError(), (long)GL_NO_ERROR);
	glClampColor(GL_TEXTURE_2D, GL_FALSE);
	passed &= gl3_expect("clamp-color-bad", (long)glGetError(), (long)GL_INVALID_ENUM);
	gl3_outcomes[10] = passed;
}

/* Prints one of the start's checks, counting it when the value is not the one expected; returns 1 when it is. */
static int
gl3_expect(
	const char *what,
	long got,
	long expected)
{
	/* A value that differs is a failure. */
	if (got != expected) {
		gl3_failures++;
		printf("GLXTEST GL3 check %s got=%ld expected=%ld FAILED\n", what, got, expected);
		return 0;
	}

	/* Succeeded: the line. */
	printf("GLXTEST GL3 check %s got=%ld expected=%ld ok\n", what, got, expected);
	return 1;
}
