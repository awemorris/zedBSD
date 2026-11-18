/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * egltest's OpenGL ES 3.0 framebuffer object scene (WS068 p026): the
 * start draws into framebuffer objects, and each frame shows what they
 * hold as twelve squares in three rows.
 *
 *   top row       one draw into four colour attachments at once: RGBA8
 *                 (ff8000), RGBA16F (40bfff, blue 2.0 clamped), RGBA8UI
 *                 through a usampler2D (0a141e), and R8, which the draw
 *                 buffers leave out and glClearBufferfv made 0.25
 *                 (404040);
 *   middle row    the draw's DEPTH_COMPONENT24 texture (0.75 as bf0000),
 *                 level 1 of a texture cleared as an attachment
 *                 (c08040), layer 2 of a 2D array cleared through
 *                 glFramebufferTextureLayer (4080c0), a texture copied by
 *                 glCopyTexImage2D from the RGBA16F attachment the read
 *                 framebuffer's read buffer names while the draw
 *                 framebuffer is the window's (40bfff);
 *   bottom row    a stencil test with a DEPTH24_STENCIL8 renderbuffer on
 *                 GL_DEPTH_STENCIL_ATTACHMENT (left half green, right
 *                 half blue), an SRGB8_ALPHA8 attachment drawn 0.5 grey
 *                 (808080), a DEPTH_COMPONENT32F texture alone in its
 *                 framebuffer cleared to 0.25 by glClearBufferfv
 *                 (400000), and a depth test against a DEPTH24_STENCIL8
 *                 texture where a farther green square leaves a nearer
 *                 red one (ff0000).
 *
 * The start also checks what the API reports: glReadPixels of integer
 * and float attachments through glReadBuffer, the implementation's read
 * format, the bindings and limits, attachment parameters, and the errors
 * and statuses of wrong framebuffers.
 */

#include "targets.h"

#include <GLES3/gl3.h>

#include <stdio.h>
#include <string.h>

/* The squares, the programs, the textures, the framebuffer objects, and the points read back. */
#define TARGETS_SQUARES		12U
#define TARGETS_PROGRAMS	5U
#define TARGETS_TEXTURES	14U
#define TARGETS_FRAMEBUFFERS	8U
#define TARGETS_POINTS		13U

/* The programs: one colour, the four attachments' outputs, a 2D texture, an unsigned one, a 2D array. */
#define TARGETS_SOLID		0U
#define TARGETS_MRT		1U
#define TARGETS_TEXTURE		2U
#define TARGETS_UNSIGNED	3U
#define TARGETS_ARRAY		4U

/* How the 2D texture program shows a texel: its colour, its red as grey, its red alone. */
#define TARGETS_SHOW_RGB	0
#define TARGETS_SHOW_GREY	1
#define TARGETS_SHOW_RED	2

/* The textures, by what they hold. */
#define TARGETS_T_RGBA8		0U
#define TARGETS_T_FLOAT		1U
#define TARGETS_T_UINT		2U
#define TARGETS_T_R8		3U
#define TARGETS_T_DEPTH		4U
#define TARGETS_T_LEVELS	5U
#define TARGETS_T_LAYERS	6U
#define TARGETS_T_COPY		7U
#define TARGETS_T_STENCIL	8U
#define TARGETS_T_SRGB		9U
#define TARGETS_T_DEPTH32F	10U
#define TARGETS_T_TESTED	11U
#define TARGETS_T_PACKED	12U
#define TARGETS_T_VOLUME	13U

/* The attribute location of the squares' corners. */
#define TARGETS_POSITION	0U

/*
 * One square: the program it is drawn with, its texture, the texture's
 * target, how a 2D texture is shown, and the layer a 2D array's is read
 * at.
 */
struct targets_square {
	unsigned program;
	unsigned texture;
	GLenum target;
	GLint show;
	GLfloat layer;
};

/*
 * The programs and their uniforms, the textures, the framebuffer objects,
 * the stencil renderbuffer and the square's vertex array; made by
 * egltest_targets_start and kept for the run, with the number of the
 * start's checks that failed.
 */
static GLuint targets_programs[TARGETS_PROGRAMS];
static GLint targets_rects[TARGETS_PROGRAMS];
static GLint targets_depths[TARGETS_PROGRAMS];
static GLint targets_colour;
static GLint targets_show;
static GLint targets_layer;
static GLuint targets_textures[TARGETS_TEXTURES];
static GLuint targets_framebuffers[TARGETS_FRAMEBUFFERS];
static GLuint targets_renderbuffer;
static GLuint targets_array;
static int targets_failures;

/*
 * The vertex shader every program shares: a unit square placed by the
 * rectangle (centre, half size) at a depth, its texture coordinates from
 * 0 to 1.
 */
static const char targets_vertex_source[] =
	"#version 300 es\n"
	"layout(location = 0) in vec2 a_position;\n"
	"uniform vec4 u_rect;\n"
	"uniform float u_depth;\n"
	"out vec2 v_uv;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\tv_uv = a_position * 0.5 + 0.5;\n"
	"\tgl_Position = vec4(u_rect.xy + a_position * u_rect.zw, u_depth, 1.0);\n"
	"}\n";

/*
 * The fragment shaders: one colour, four outputs of four kinds, a 2D
 * texture shown three ways, an unsigned 2D texture over 255, and a 2D
 * array at a layer.
 */
static const char *const targets_fragment_sources[TARGETS_PROGRAMS] = {
	"#version 300 es\n"
	"precision mediump float;\n"
	"uniform vec4 u_colour;\n"
	"out vec4 o_colour;\n"
	"void main()\n"
	"{\n"
	"\to_colour = u_colour;\n"
	"}\n",

	/* Four outputs: RGBA8, RGBA16F (blue beyond 1), RGBA8UI, R8. */
	"#version 300 es\n"
	"precision mediump float;\n"
	"layout(location = 0) out vec4 o_first;\n"
	"layout(location = 1) out vec4 o_second;\n"
	"layout(location = 2) out uvec4 o_third;\n"
	"layout(location = 3) out vec4 o_fourth;\n"
	"void main()\n"
	"{\n"
	"\to_first = vec4(1.0, 0.5, 0.0, 1.0);\n"
	"\to_second = vec4(0.25, 0.75, 2.0, 1.0);\n"
	"\to_third = uvec4(10u, 20u, 30u, 40u);\n"
	"\to_fourth = vec4(1.0);\n"
	"}\n",

	/* A 2D texture: its colour, its red as grey, or its red alone. */
	"#version 300 es\n"
	"precision mediump float;\n"
	"uniform sampler2D u_texture;\n"
	"uniform int u_show;\n"
	"in vec2 v_uv;\n"
	"out vec4 o_colour;\n"
	"void main()\n"
	"{\n"
	"\tvec4 texel = texture(u_texture, v_uv);\n"
	"\to_colour = vec4(texel.rgb, 1.0);\n"
	"\tif (u_show == 1)\n"
	"\t\to_colour = vec4(texel.rrr, 1.0);\n"
	"\tif (u_show == 2)\n"
	"\t\to_colour = vec4(texel.r, 0.0, 0.0, 1.0);\n"
	"}\n",

	/* An unsigned 2D texture over 255. */
	"#version 300 es\n"
	"precision mediump float;\n"
	"uniform mediump usampler2D u_texture;\n"
	"in vec2 v_uv;\n"
	"out vec4 o_colour;\n"
	"void main()\n"
	"{\n"
	"\to_colour = vec4(vec3(texture(u_texture, v_uv).rgb) / 255.0, 1.0);\n"
	"}\n",

	/* A 2D array at a layer. */
	"#version 300 es\n"
	"precision mediump float;\n"
	"uniform mediump sampler2DArray u_texture;\n"
	"uniform float u_layer;\n"
	"in vec2 v_uv;\n"
	"out vec4 o_colour;\n"
	"void main()\n"
	"{\n"
	"\to_colour = vec4(texture(u_texture, vec3(v_uv, u_layer)).rgb, 1.0);\n"
	"}\n"
};

/* The squares, in their cells from the top left. */
static const struct targets_square targets_squares[TARGETS_SQUARES] = {
	{ TARGETS_TEXTURE, TARGETS_T_RGBA8, GL_TEXTURE_2D, TARGETS_SHOW_RGB, 0.0f },
	{ TARGETS_TEXTURE, TARGETS_T_FLOAT, GL_TEXTURE_2D, TARGETS_SHOW_RGB, 0.0f },
	{ TARGETS_UNSIGNED, TARGETS_T_UINT, GL_TEXTURE_2D, TARGETS_SHOW_RGB, 0.0f },
	{ TARGETS_TEXTURE, TARGETS_T_R8, GL_TEXTURE_2D, TARGETS_SHOW_GREY, 0.0f },
	{ TARGETS_TEXTURE, TARGETS_T_DEPTH, GL_TEXTURE_2D, TARGETS_SHOW_RED, 0.0f },
	{ TARGETS_TEXTURE, TARGETS_T_LEVELS, GL_TEXTURE_2D, TARGETS_SHOW_RGB, 0.0f },
	{ TARGETS_ARRAY, TARGETS_T_LAYERS, GL_TEXTURE_2D_ARRAY, TARGETS_SHOW_RGB, 2.0f },
	{ TARGETS_TEXTURE, TARGETS_T_COPY, GL_TEXTURE_2D, TARGETS_SHOW_RGB, 0.0f },
	{ TARGETS_TEXTURE, TARGETS_T_STENCIL, GL_TEXTURE_2D, TARGETS_SHOW_RGB, 0.0f },
	{ TARGETS_TEXTURE, TARGETS_T_SRGB, GL_TEXTURE_2D, TARGETS_SHOW_RGB, 0.0f },
	{ TARGETS_TEXTURE, TARGETS_T_DEPTH32F, GL_TEXTURE_2D, TARGETS_SHOW_RED, 0.0f },
	{ TARGETS_TEXTURE, TARGETS_T_TESTED, GL_TEXTURE_2D, TARGETS_SHOW_RGB, 0.0f }
};

/*
 * The points read back: the squares' centres (the stencil square's two
 * halves), in the window's normalized coordinates, and the colours
 * expected there as 0xRRGGBB.
 */
static const char *const targets_names[TARGETS_POINTS] = {
	"mrt-rgba8", "mrt-rgba16f", "mrt-rgba8ui", "mrt-r8-cleared",
	"depth-texture", "level-1", "array-layer", "copy-read-buffer",
	"stencil-inside", "stencil-outside", "srgb", "clear-depth32f", "depth-stencil-test"
};
static const GLfloat targets_points[TARGETS_POINTS][2] = {
	{ -0.75f, 0.66f }, { -0.25f, 0.66f }, { 0.25f, 0.66f }, { 0.75f, 0.66f },
	{ -0.75f, 0.0f }, { -0.25f, 0.0f }, { 0.25f, 0.0f }, { 0.75f, 0.0f },
	{ -0.85f, -0.66f }, { -0.65f, -0.66f }, { -0.25f, -0.66f }, { 0.25f, -0.66f }, { 0.75f, -0.66f }
};
static const unsigned targets_expected[TARGETS_POINTS] = {
	0xff8000U, 0x40bfffU, 0x0a141eU, 0x404040U,
	0xbf0000U, 0xc08040U, 0x4080c0U, 0x40bfffU,
	0x00ff00U, 0x0000ffU, 0x808080U, 0x400000U, 0xff0000U
};

static GLuint targets_shader(GLenum type, const char *source);
static void targets_texture(unsigned texture, GLenum target);
static void targets_framebuffer(unsigned framebuffer, GLsizei width, GLsizei height);
static void targets_quad(unsigned program, GLfloat x, GLfloat y, GLfloat width, GLfloat height, GLfloat depth);
static void targets_colour_set(GLfloat red, GLfloat green, GLfloat blue);
static void targets_mrt(void);
static void targets_mrt_checks(void);
static void targets_levels_and_layers(void);
static void targets_depth_and_stencil(void);
static void targets_errors(void);
static void targets_expect(const char *what, long got, long expected);
static int targets_close(unsigned got, unsigned expected);

/*
 * Makes the programs and the square, draws into the framebuffer objects,
 * and checks what the API reports.  Returns 0, or -1 with a line saying
 * what failed.
 */
int
egltest_targets_start(void)
{
	static const GLfloat corners[8] = { -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };
	GLuint vertex;
	GLuint fragment;
	GLuint buffer;
	GLint linked;
	GLint sampler;
	GLenum error;
	unsigned program;

	/* Each program: the shared vertex shader and its fragment shader. */
	for (program = 0U; program < TARGETS_PROGRAMS; program++) {
		vertex = targets_shader(GL_VERTEX_SHADER, targets_vertex_source);
		fragment = targets_shader(GL_FRAGMENT_SHADER, targets_fragment_sources[program]);
		if (vertex == 0U || fragment == 0U)
			return -1;

		/* Linked. */
		targets_programs[program] = glCreateProgram();
		glAttachShader(targets_programs[program], vertex);
		glAttachShader(targets_programs[program], fragment);
		glLinkProgram(targets_programs[program]);
		glDeleteShader(vertex);
		glDeleteShader(fragment);
		linked = GL_FALSE;
		glGetProgramiv(targets_programs[program], GL_LINK_STATUS, &linked);
		if (!linked) {
			printf("EGLTEST TARGETS link failed: program %u\n", program);
			return -1;
		}

		/* Its rectangle and depth, and its sampler (when it has one) on unit 0. */
		glUseProgram(targets_programs[program]);
		targets_rects[program] = glGetUniformLocation(targets_programs[program], "u_rect");
		targets_depths[program] = glGetUniformLocation(targets_programs[program], "u_depth");
		sampler = glGetUniformLocation(targets_programs[program], "u_texture");
		if (sampler >= 0)
			glUniform1i(sampler, 0);
	}

	/* The other uniforms. */
	targets_colour = glGetUniformLocation(targets_programs[TARGETS_SOLID], "u_colour");
	targets_show = glGetUniformLocation(targets_programs[TARGETS_TEXTURE], "u_show");
	targets_layer = glGetUniformLocation(targets_programs[TARGETS_ARRAY], "u_layer");
	if (targets_colour < 0 || targets_show < 0 || targets_layer < 0) {
		printf("EGLTEST TARGETS uniforms missing\n");
		return -1;
	}

	/* The square's corners in a vertex array of their own. */
	glGenVertexArrays(1, &targets_array);
	glBindVertexArray(targets_array);
	glGenBuffers(1, &buffer);
	glBindBuffer(GL_ARRAY_BUFFER, buffer);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(corners), corners, GL_STATIC_DRAW);
	glVertexAttribPointer(TARGETS_POSITION, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glEnableVertexAttribArray(TARGETS_POSITION);

	/* The textures and framebuffer objects, each drawn into, and the checks of what the API reports. */
	glGenTextures((GLsizei)TARGETS_TEXTURES, targets_textures);
	glGenFramebuffers((GLsizei)TARGETS_FRAMEBUFFERS, targets_framebuffers);
	targets_mrt();
	targets_mrt_checks();
	targets_levels_and_layers();
	targets_depth_and_stencil();
	targets_errors();

	/* The window's framebuffer again, for drawing and reading. */
	glBindFramebuffer(GL_FRAMEBUFFER, 0U);
	glBindVertexArray(0U);

	/* Everything went without an error the checks did not expect. */
	error = glGetError();
	targets_expect("start-glerror", (long)error, (long)GL_NO_ERROR);

	/* Succeeded: ready to draw. */
	printf("EGLTEST TARGETS ready failures=%d\n", targets_failures);
	fflush(stdout);
	return 0;
}

/*
 * Draws one square per texture the framebuffer objects drew into over a
 * window of a size.
 */
void
egltest_targets_draw(
	int width,
	int height)
{
	const struct targets_square *square;
	unsigned index;
	GLfloat x;
	GLfloat y;

	/* The whole window, cleared to dark grey. */
	glBindFramebuffer(GL_FRAMEBUFFER, 0U);
	glViewport(0, 0, width, height);
	glClearColor(0.125f, 0.125f, 0.125f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glBindVertexArray(targets_array);
	glActiveTexture(GL_TEXTURE0);

	/* Each square in its cell (four columns, three rows), with its texture on unit 0. */
	for (index = 0U; index < TARGETS_SQUARES; index++) {
		square = &targets_squares[index];
		x = -0.75f + 0.5f * (GLfloat)(index % 4U);
		y = 0.66f - 0.66f * (GLfloat)(index / 4U);
		glUseProgram(targets_programs[square->program]);
		if (square->program == TARGETS_TEXTURE)
			glUniform1i(targets_show, square->show);
		if (square->program == TARGETS_ARRAY)
			glUniform1f(targets_layer, square->layer);
		glBindTexture(square->target, targets_textures[square->texture]);
		targets_quad(square->program, x, y, 0.2f, 0.25f, 0.0f);
	}

	/* The vertex array is let go. */
	glBindVertexArray(0U);
}

/*
 * Reads back each point and prints the colours; returns how many differ,
 * with the start's failed checks and any error.
 */
int
egltest_targets_check(
	int width,
	int height,
	const char *token)
{
	GLubyte pixel[4];
	GLenum error;
	const char *verdict;
	unsigned got;
	unsigned point;
	int failures;
	int same;
	int x;
	int y;

	/* Each point, in GL's coordinates. */
	failures = targets_failures;
	for (point = 0U; point < TARGETS_POINTS; point++) {
		x = (int)((targets_points[point][0] + 1.0f) * 0.5f * (float)width);
		y = (int)((targets_points[point][1] + 1.0f) * 0.5f * (float)height);
		memset(pixel, 0, sizeof(pixel));
		glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
		got = ((unsigned)pixel[0] << 16) | ((unsigned)pixel[1] << 8) | (unsigned)pixel[2];

		/* Each channel within 2 of the expected one. */
		same = targets_close(got, targets_expected[point]);
		verdict = "ok";
		if (!same) {
			verdict = "DIFFERS";
			failures++;
		}

		/* The point's line. */
		printf("EGLTEST PIXEL run=%s name=%s got=%06x expected=%06x %s\n", token, targets_names[point], got,
		       targets_expected[point], verdict);
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
targets_shader(
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
		printf("EGLTEST TARGETS compile failed: %s\n", log);
		return 0U;
	}

	/* Succeeded: the shader. */
	return shader;
}

/* Binds a texture of a target to unit 0 with nearest filters and edge clamping. */
static void
targets_texture(
	unsigned texture,
	GLenum target)
{
	/* The texture and its sampling state. */
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(target, targets_textures[texture]);
	glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/* Binds a framebuffer object for drawing and reading, with a viewport of its size. */
static void
targets_framebuffer(
	unsigned framebuffer,
	GLsizei width,
	GLsizei height)
{
	/* The object and the viewport. */
	glBindFramebuffer(GL_FRAMEBUFFER, targets_framebuffers[framebuffer]);
	glViewport(0, 0, width, height);
}

/* Draws the square with a program, placed by a rectangle (centre, half size) at a depth. */
static void
targets_quad(
	unsigned program,
	GLfloat x,
	GLfloat y,
	GLfloat width,
	GLfloat height,
	GLfloat depth)
{
	/* The program's rectangle and depth, then the strip. */
	glUseProgram(targets_programs[program]);
	glUniform4f(targets_rects[program], x, y, width, height);
	if (targets_depths[program] >= 0)
		glUniform1f(targets_depths[program], depth);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* Sets the colour the one-colour program draws. */
static void
targets_colour_set(
	GLfloat red,
	GLfloat green,
	GLfloat blue)
{
	/* The uniform. */
	glUseProgram(targets_programs[TARGETS_SOLID]);
	glUniform4f(targets_colour, red, green, blue, 1.0f);
}

/*
 * Draws once into four colour attachments of four formats with a depth
 * texture, the fourth left out by the draw buffers after glClearBufferfv
 * made it 0.25, then copies the RGBA16F attachment (the read buffer) into
 * a texture while the draw framebuffer is the window's.
 */
static void
targets_mrt(void)
{
	static const GLenum all[4] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3 };
	static const GLenum three[4] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_NONE };
	static const GLfloat zeros[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	static const GLuint unsigned_zeros[4] = { 0U, 0U, 0U, 0U };
	static const GLfloat quarter[4] = { 0.25f, 0.25f, 0.25f, 1.0f };
	static const GLfloat far_depth = 1.0f;
	GLenum status;
	GLint value;

	/* The four colour textures and the depth texture, 8x8. */
	targets_texture(TARGETS_T_RGBA8, GL_TEXTURE_2D);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 8, 8);
	targets_texture(TARGETS_T_FLOAT, GL_TEXTURE_2D);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16F, 8, 8);
	targets_texture(TARGETS_T_UINT, GL_TEXTURE_2D);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8UI, 8, 8);
	targets_texture(TARGETS_T_R8, GL_TEXTURE_2D);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, 8, 8);
	targets_texture(TARGETS_T_DEPTH, GL_TEXTURE_2D);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, 8, 8, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);

	/* Attached to framebuffer 0 of the scene. */
	targets_framebuffer(0U, 8, 8);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, targets_textures[TARGETS_T_RGBA8], 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, targets_textures[TARGETS_T_FLOAT], 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, targets_textures[TARGETS_T_UINT], 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT3, GL_TEXTURE_2D, targets_textures[TARGETS_T_R8], 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, targets_textures[TARGETS_T_DEPTH], 0);
	status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	targets_expect("mrt-complete", (long)status, (long)GL_FRAMEBUFFER_COMPLETE);

	/* Every buffer cleared by its own call: zeros, the fourth 0.25, the depth far. */
	glDrawBuffers(4, all);
	glClearBufferfv(GL_COLOR, 0, zeros);
	glClearBufferfv(GL_COLOR, 1, zeros);
	glClearBufferuiv(GL_COLOR, 2, unsigned_zeros);
	glClearBufferfv(GL_COLOR, 3, quarter);
	glClearBufferfv(GL_DEPTH, 0, &far_depth);

	/* One draw into the first three at depth 0.75. */
	glDrawBuffers(4, three);
	value = 0;
	glGetIntegerv(GL_DRAW_BUFFER3, &value);
	targets_expect("draw-buffer3-none", (long)value, (long)GL_NONE);
	glEnable(GL_DEPTH_TEST);
	targets_quad(TARGETS_MRT, 0.0f, 0.0f, 1.0f, 1.0f, 0.5f);
	glDisable(GL_DEPTH_TEST);

	/* The RGBA16F attachment copied through the read framebuffer while the window's is the draw framebuffer. */
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0U);
	glReadBuffer(GL_COLOR_ATTACHMENT1);
	value = -1;
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &value);
	targets_expect("read-binding", (long)value, (long)targets_framebuffers[0]);
	value = -1;
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &value);
	targets_expect("draw-binding", (long)value, 0L);
	targets_texture(TARGETS_T_COPY, GL_TEXTURE_2D);
	glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 0, 0, 4, 4, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, targets_framebuffers[0]);
}

/*
 * Checks glReadPixels of the integer and the float attachments through
 * glReadBuffer, the read format the implementation offers, and an
 * attachment's parameters.
 */
static void
targets_mrt_checks(void)
{
	GLuint integers[4];
	GLfloat floats[4];
	GLint value;

	/* The unsigned attachment read as unsigned integers. */
	glReadBuffer(GL_COLOR_ATTACHMENT2);
	value = 0;
	glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &value);
	targets_expect("read-format-integer", (long)value, (long)GL_RGBA_INTEGER);
	memset(integers, 0, sizeof(integers));
	glReadPixels(1, 1, 1, 1, GL_RGBA_INTEGER, GL_UNSIGNED_INT, integers);
	targets_expect("read-uint-blue", (long)integers[2], 30L);
	targets_expect("read-uint-alpha", (long)integers[3], 40L);

	/* The float attachment read as floats, its blue beyond 1 kept. */
	glReadBuffer(GL_COLOR_ATTACHMENT1);
	memset(floats, 0, sizeof(floats));
	glReadPixels(1, 1, 1, 1, GL_RGBA, GL_FLOAT, floats);
	targets_expect("read-float-blue-x100", (long)(floats[2] * 100.0f + 0.5f), 200L);

	/* The unsigned attachment's component type, and bytes from an integer attachment are refused. */
	value = 0;
	glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &value);
	targets_expect("component-type-uint", (long)value, (long)GL_UNSIGNED_INT);
	glReadBuffer(GL_COLOR_ATTACHMENT2);
	glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, integers);
	targets_expect("read-uint-as-bytes", (long)glGetError(), (long)GL_INVALID_OPERATION);
	glReadBuffer(GL_COLOR_ATTACHMENT0);
}

/*
 * Clears level 1 of a texture and layer 2 of a 2D array as colour
 * attachments.
 */
static void
targets_levels_and_layers(void)
{
	GLenum status;
	GLint value;

	/* Level 1 of a 4x4 texture of three levels, read from its base level 1. */
	targets_texture(TARGETS_T_LEVELS, GL_TEXTURE_2D);
	glTexStorage2D(GL_TEXTURE_2D, 3, GL_RGBA8, 4, 4);
	targets_framebuffer(1U, 2, 2);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, targets_textures[TARGETS_T_LEVELS], 1);
	status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	targets_expect("level-complete", (long)status, (long)GL_FRAMEBUFFER_COMPLETE);
	glClearColor(0.75f, 0.5f, 0.25f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 1);

	/* Layer 2 of a 4x4 2D array of three layers. */
	targets_texture(TARGETS_T_LAYERS, GL_TEXTURE_2D_ARRAY);
	glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, 4, 4, 3);
	targets_framebuffer(2U, 4, 4);
	glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, targets_textures[TARGETS_T_LAYERS], 0, 2);
	status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	targets_expect("layer-complete", (long)status, (long)GL_FRAMEBUFFER_COMPLETE);
	value = -1;
	glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER, &value);
	targets_expect("attachment-layer", (long)value, 2L);
	glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
}

/*
 * Draws with a stencil renderbuffer, into an sRGB attachment, clears a
 * lone depth texture, and draws with a depth test against a depth and
 * stencil texture.
 */
static void
targets_depth_and_stencil(void)
{
	static const GLfloat quarter_depth = 0.25f;
	GLenum status;

	/* An RGBA8 texture with a DEPTH24_STENCIL8 renderbuffer on both depth and stencil. */
	targets_texture(TARGETS_T_STENCIL, GL_TEXTURE_2D);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 8, 8);
	glGenRenderbuffers(1, &targets_renderbuffer);
	glBindRenderbuffer(GL_RENDERBUFFER, targets_renderbuffer);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 8, 8);
	targets_framebuffer(3U, 8, 8);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, targets_textures[TARGETS_T_STENCIL], 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, targets_renderbuffer);
	status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	targets_expect("stencil-complete", (long)status, (long)GL_FRAMEBUFFER_COMPLETE);

	/* Blue, with stencil 0; the left half's stencil made 1 without colour; green where the stencil is 1. */
	glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
	glClearStencil(0);
	glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	glEnable(GL_STENCIL_TEST);
	glStencilFunc(GL_ALWAYS, 1, 0xff);
	glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	targets_colour_set(1.0f, 0.0f, 0.0f);
	targets_quad(TARGETS_SOLID, -0.5f, 0.0f, 0.5f, 1.0f, 0.0f);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glStencilFunc(GL_EQUAL, 1, 0xff);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	targets_colour_set(0.0f, 1.0f, 0.0f);
	targets_quad(TARGETS_SOLID, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f);
	glDisable(GL_STENCIL_TEST);

	/* An SRGB8_ALPHA8 attachment drawn 0.5 grey (kept encoded, read back as 0.5). */
	targets_texture(TARGETS_T_SRGB, GL_TEXTURE_2D);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_SRGB8_ALPHA8, 4, 4);
	targets_framebuffer(4U, 4, 4);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, targets_textures[TARGETS_T_SRGB], 0);
	targets_colour_set(0.5f, 0.5f, 0.5f);
	targets_quad(TARGETS_SOLID, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f);

	/* A DEPTH_COMPONENT32F texture alone in its framebuffer, cleared to 0.25. */
	targets_texture(TARGETS_T_DEPTH32F, GL_TEXTURE_2D);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH_COMPONENT32F, 4, 4);
	targets_framebuffer(5U, 4, 4);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, targets_textures[TARGETS_T_DEPTH32F], 0);
	status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	targets_expect("depth-only-complete", (long)status, (long)GL_FRAMEBUFFER_COMPLETE);
	glClearBufferfv(GL_DEPTH, 0, &quarter_depth);

	/* An RGBA8 texture with a DEPTH24_STENCIL8 texture: a red square at 0.5 hides a green one at 0.75 drawn after it. */
	targets_texture(TARGETS_T_TESTED, GL_TEXTURE_2D);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 4, 4);
	targets_texture(TARGETS_T_PACKED, GL_TEXTURE_2D);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH24_STENCIL8, 4, 4);
	targets_framebuffer(6U, 4, 4);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, targets_textures[TARGETS_T_TESTED], 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, targets_textures[TARGETS_T_PACKED], 0);
	status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	targets_expect("depth-stencil-texture-complete", (long)status, (long)GL_FRAMEBUFFER_COMPLETE);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClearDepthf(1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	targets_colour_set(1.0f, 0.0f, 0.0f);
	targets_quad(TARGETS_SOLID, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f);
	targets_colour_set(0.0f, 1.0f, 0.0f);
	targets_quad(TARGETS_SOLID, 0.0f, 0.0f, 1.0f, 1.0f, 0.5f);
	glDisable(GL_DEPTH_TEST);
}

/*
 * Checks the limits, and the errors and statuses of wrong framebuffers:
 * a draw buffer naming another attachment, GL_BACK read from an object,
 * a layer of a 2D texture, an empty framebuffer; a 3D texture's slice is
 * complete.
 */
static void
targets_errors(void)
{
	static const GLenum second[1] = { GL_COLOR_ATTACHMENT1 };
	GLenum status;
	GLint value;

	/* OpenGL ES 3.0's least limits. */
	value = 0;
	glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &value);
	targets_expect("max-color-attachments", (long)(value >= 4), 1L);
	value = 0;
	glGetIntegerv(GL_MAX_DRAW_BUFFERS, &value);
	targets_expect("max-draw-buffers", (long)(value >= 4), 1L);

	/* Draw buffer 0 cannot name the second attachment, nor an object's read buffer GL_BACK. */
	targets_framebuffer(0U, 8, 8);
	glDrawBuffers(1, second);
	targets_expect("draw-buffer-other", (long)glGetError(), (long)GL_INVALID_OPERATION);
	glReadBuffer(GL_BACK);
	targets_expect("read-back-object", (long)glGetError(), (long)GL_INVALID_OPERATION);

	/* A layer of a 2D texture is refused. */
	targets_framebuffer(7U, 4, 4);
	glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, targets_textures[TARGETS_T_RGBA8], 0, 0);
	targets_expect("layer-of-2d", (long)glGetError(), (long)GL_INVALID_OPERATION);

	/* An empty framebuffer is missing its attachments. */
	status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	targets_expect("empty-status", (long)status, (long)GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT);

	/* A 3D texture's slice is drawn into (through a 2D image, WS068 p029). */
	targets_texture(TARGETS_T_VOLUME, GL_TEXTURE_3D);
	glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, 4, 4, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, targets_textures[TARGETS_T_VOLUME], 0, 1);
	status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	targets_expect("volume-slice-status", (long)status, (long)GL_FRAMEBUFFER_COMPLETE);
}

/* Prints one of the start's checks, counting it when the value is not the one expected. */
static void
targets_expect(
	const char *what,
	long got,
	long expected)
{
	const char *verdict;

	/* A value that differs is a failure. */
	verdict = "ok";
	if (got != expected) {
		verdict = "FAILED";
		targets_failures++;
	}

	/* The line. */
	printf("EGLTEST TARGETS check %s got=%ld expected=%ld %s\n", what, got, expected, verdict);
}

/* Reports whether two 0xRRGGBB colours are within 2 in every channel. */
static int
targets_close(
	unsigned got,
	unsigned expected)
{
	unsigned shift;
	int difference;

	/* Each channel. */
	for (shift = 0U; shift < 24U; shift += 8U) {
		difference = (int)((got >> shift) & 0xffU) - (int)((expected >> shift) & 0xffU);
		if (difference > 2 || difference < -2)
			return 0;
	}

	/* Every channel is close. */
	return 1;
}
