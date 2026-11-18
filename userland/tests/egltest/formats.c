/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * egltest's OpenGL ES 3.0 texture format scene (WS068 p025): twelve
 * squares in three rows, each sampling a texture of another format.
 *
 *   top row       R8 by glTexStorage2D and glTexSubImage2D (800000),
 *                 RGBA16F from half floats (4080ff), RGBA32F with nearest
 *                 filters (00ff80), RGBA8UI through a usampler2D
 *                 (0ac81e);
 *   middle row    R32I -5 through an isampler2D (green when it reads -5),
 *                 RG8 swizzled green-red-one (4020ff), a 32-bit float
 *                 depth texture of 0.25 and 0.75 compared with 0.5
 *                 through a sampler2DShadow and a sampler object (left
 *                 half black, right half white), a 16-bit depth texture
 *                 of 0.75 read through a sampler2D (bf0000);
 *   bottom row    SRGB8_ALPHA8 of 0x80 (read as 373737), R11F_G11F_B10F
 *                 from floats (ff8040), RGBA16F mipmapped by
 *                 glGenerateMipmap and read from its base level 1
 *                 (808080), RGBA32F with a linear filter, which cannot be
 *                 filtered and reads black.
 *
 * The start also checks what the API reports of immutable textures,
 * sampler objects and the errors of wrong calls.
 */

#include "formats.h"

#include <GLES3/gl3.h>

#include <stdio.h>
#include <string.h>

/* The squares, the programs (by the kind of sampler), and the points read back. */
#define FORMATS_SQUARES		12U
#define FORMATS_PROGRAMS	4U
#define FORMATS_POINTS		13U

/* The kinds of sampler a square is drawn with. */
#define FORMATS_FLOAT		0U
#define FORMATS_UINT		1U
#define FORMATS_INT		2U
#define FORMATS_SHADOW		3U

/* The attribute location of the squares' corners. */
#define FORMATS_POSITION	0U

/*
 * The programs, their uniforms, the textures, the sampler object and the
 * square's vertex array; made by egltest_formats_start and kept for the
 * run, with the number of the start's checks that failed.
 */
static GLuint formats_programs[FORMATS_PROGRAMS];
static GLint formats_rects[FORMATS_PROGRAMS];
static GLint formats_reference;
static GLuint formats_textures[FORMATS_SQUARES];
static GLuint formats_sampler;
static GLuint formats_array;
static int formats_failures;

/*
 * The vertex shader every program shares: a unit square placed by the
 * rectangle (centre, half size), its texture coordinates from 0 to 1.
 */
static const char formats_vertex_source[] =
	"#version 300 es\n"
	"layout(location = 0) in vec2 a_position;\n"
	"uniform vec4 u_rect;\n"
	"out vec2 v_uv;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\tv_uv = a_position * 0.5 + 0.5;\n"
	"\tgl_Position = vec4(u_rect.xy + a_position * u_rect.zw, 0.0, 1.0);\n"
	"}\n";

/*
 * The fragment shaders: a float sampler as it reads, an unsigned one over
 * 255, a signed one green when it reads -5 (else red), and a shadow
 * sampler's comparison with a reference as grey.
 */
static const char *const formats_fragment_sources[FORMATS_PROGRAMS] = {
	"#version 300 es\n"
	"precision mediump float;\n"
	"uniform sampler2D u_texture;\n"
	"in vec2 v_uv;\n"
	"out vec4 o_colour;\n"
	"void main()\n"
	"{\n"
	"\to_colour = vec4(texture(u_texture, v_uv).rgb, 1.0);\n"
	"}\n",

	/* An unsigned sampler, over 255. */
	"#version 300 es\n"
	"precision mediump float;\n"
	"uniform mediump usampler2D u_texture;\n"
	"in vec2 v_uv;\n"
	"out vec4 o_colour;\n"
	"void main()\n"
	"{\n"
	"\to_colour = vec4(vec3(texture(u_texture, v_uv).rgb) / 255.0, 1.0);\n"
	"}\n",

	/* A signed sampler: green when it reads -5. */
	"#version 300 es\n"
	"precision mediump float;\n"
	"uniform mediump isampler2D u_texture;\n"
	"in vec2 v_uv;\n"
	"out vec4 o_colour;\n"
	"void main()\n"
	"{\n"
	"\to_colour = vec4(1.0, 0.0, 0.0, 1.0);\n"
	"\tif (texture(u_texture, v_uv).r == -5)\n"
	"\t\to_colour = vec4(0.0, 1.0, 0.0, 1.0);\n"
	"}\n",

	/* A shadow sampler: the comparison as grey. */
	"#version 300 es\n"
	"precision mediump float;\n"
	"uniform mediump sampler2DShadow u_texture;\n"
	"uniform float u_reference;\n"
	"in vec2 v_uv;\n"
	"out vec4 o_colour;\n"
	"void main()\n"
	"{\n"
	"\tfloat lit = texture(u_texture, vec3(v_uv, u_reference));\n"
	"\to_colour = vec4(lit, lit, lit, 1.0);\n"
	"}\n"
};

/* The program each square is drawn with. */
static const unsigned formats_kinds[FORMATS_SQUARES] = {
	FORMATS_FLOAT, FORMATS_FLOAT, FORMATS_FLOAT, FORMATS_UINT,
	FORMATS_INT, FORMATS_FLOAT, FORMATS_SHADOW, FORMATS_FLOAT,
	FORMATS_FLOAT, FORMATS_FLOAT, FORMATS_FLOAT, FORMATS_FLOAT
};

/*
 * The points read back: the squares' centres (the shadow square's two
 * halves), in the window's normalized coordinates, and the colours
 * expected there as 0xRRGGBB.
 */
static const char *const formats_names[FORMATS_POINTS] = {
	"r8-storage", "rgba16f", "rgba32f-nearest", "rgba8ui",
	"r32i", "rg8-swizzle", "shadow-less", "shadow-more", "depth16",
	"srgb8-alpha8", "r11f-g11f-b10f", "base-level-mipmap", "rgba32f-linear"
};
static const GLfloat formats_points[FORMATS_POINTS][2] = {
	{ -0.75f, 0.66f }, { -0.25f, 0.66f }, { 0.25f, 0.66f }, { 0.75f, 0.66f },
	{ -0.75f, 0.0f }, { -0.25f, 0.0f }, { 0.15f, 0.0f }, { 0.35f, 0.0f }, { 0.75f, 0.0f },
	{ -0.75f, -0.66f }, { -0.25f, -0.66f }, { 0.25f, -0.66f }, { 0.75f, -0.66f }
};
static const unsigned formats_expected[FORMATS_POINTS] = {
	0x800000U, 0x4080ffU, 0x00ff80U, 0x0ac81eU,
	0x00ff00U, 0x4020ffU, 0x000000U, 0xffffffU, 0xbf0000U,
	0x373737U, 0xff8040U, 0x808080U, 0x000000U
};

static GLuint formats_shader(GLenum type, const char *source);
static void formats_textures_make(void);
static void formats_texture_bind(unsigned square, GLenum min_filter, GLenum mag_filter);
static void formats_checks(void);
static void formats_expect(const char *what, long got, long expected);
static int formats_close(unsigned got, unsigned expected);

/*
 * Makes the programs, the square, the textures and the sampler object,
 * and checks what the API reports.  Returns 0, or -1 with a line saying
 * what failed.
 */
int
egltest_formats_start(void)
{
	static const GLfloat corners[8] = { -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };
	GLuint vertex;
	GLuint fragment;
	GLuint buffer;
	GLint linked;
	GLint sampler;
	GLenum error;
	unsigned kind;

	/* Each program: the shared vertex shader and its fragment shader. */
	for (kind = 0U; kind < FORMATS_PROGRAMS; kind++) {
		vertex = formats_shader(GL_VERTEX_SHADER, formats_vertex_source);
		fragment = formats_shader(GL_FRAGMENT_SHADER, formats_fragment_sources[kind]);
		if (vertex == 0U || fragment == 0U)
			return -1;

		/* Linked. */
		formats_programs[kind] = glCreateProgram();
		glAttachShader(formats_programs[kind], vertex);
		glAttachShader(formats_programs[kind], fragment);
		glLinkProgram(formats_programs[kind]);
		glDeleteShader(vertex);
		glDeleteShader(fragment);
		linked = GL_FALSE;
		glGetProgramiv(formats_programs[kind], GL_LINK_STATUS, &linked);
		if (!linked) {
			printf("EGLTEST FORMATS link failed: program %u\n", kind);
			return -1;
		}

		/* Its rectangle, and its sampler on unit 0. */
		glUseProgram(formats_programs[kind]);
		formats_rects[kind] = glGetUniformLocation(formats_programs[kind], "u_rect");
		sampler = glGetUniformLocation(formats_programs[kind], "u_texture");
		if (formats_rects[kind] < 0 || sampler < 0) {
			printf("EGLTEST FORMATS uniforms missing: program %u\n", kind);
			return -1;
		}

		/* The texture is on unit 0. */
		glUniform1i(sampler, 0);
	}

	/* The shadow program's reference depth. */
	formats_reference = glGetUniformLocation(formats_programs[FORMATS_SHADOW], "u_reference");
	glUseProgram(formats_programs[FORMATS_SHADOW]);
	glUniform1f(formats_reference, 0.5f);

	/* The square's corners in a vertex array of their own. */
	glGenVertexArrays(1, &formats_array);
	glBindVertexArray(formats_array);
	glGenBuffers(1, &buffer);
	glBindBuffer(GL_ARRAY_BUFFER, buffer);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(corners), corners, GL_STATIC_DRAW);
	glVertexAttribPointer(FORMATS_POSITION, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glEnableVertexAttribArray(FORMATS_POSITION);
	glBindVertexArray(0U);

	/* The textures, the sampler object, and the checks of what the API reports. */
	formats_textures_make();
	formats_checks();

	/* Everything went without an error the checks did not expect. */
	error = glGetError();
	formats_expect("start-glerror", (long)error, (long)GL_NO_ERROR);

	/* Succeeded: ready to draw. */
	printf("EGLTEST FORMATS ready failures=%d\n", formats_failures);
	fflush(stdout);
	return 0;
}

/*
 * Draws one square per texture over a window of a size, the shadow one
 * with the sampler object on its unit.
 */
void
egltest_formats_draw(
	int width,
	int height)
{
	unsigned square;
	unsigned kind;
	GLfloat x;
	GLfloat y;

	/* The whole window, cleared to dark grey. */
	glViewport(0, 0, width, height);
	glClearColor(0.125f, 0.125f, 0.125f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glBindVertexArray(formats_array);
	glActiveTexture(GL_TEXTURE0);

	/* Each square in its cell (four columns, three rows), with its texture on unit 0. */
	for (square = 0U; square < FORMATS_SQUARES; square++) {
		kind = formats_kinds[square];
		x = -0.75f + 0.5f * (GLfloat)(square % 4U);
		y = 0.66f - 0.66f * (GLfloat)(square / 4U);
		glUseProgram(formats_programs[kind]);
		glUniform4f(formats_rects[kind], x, y, 0.2f, 0.25f);
		glBindTexture(GL_TEXTURE_2D, formats_textures[square]);

		/* The shadow square reads through the sampler object. */
		if (kind == FORMATS_SHADOW)
			glBindSampler(0U, formats_sampler);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		if (kind == FORMATS_SHADOW)
			glBindSampler(0U, 0U);
	}

	/* The vertex array is let go. */
	glBindVertexArray(0U);
}

/*
 * Reads back each point and prints the colours; returns how many differ,
 * with the start's failed checks and any error.
 */
int
egltest_formats_check(
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
	failures = formats_failures;
	for (point = 0U; point < FORMATS_POINTS; point++) {
		x = (int)((formats_points[point][0] + 1.0f) * 0.5f * (float)width);
		y = (int)((formats_points[point][1] + 1.0f) * 0.5f * (float)height);
		memset(pixel, 0, sizeof(pixel));
		glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
		got = ((unsigned)pixel[0] << 16) | ((unsigned)pixel[1] << 8) | (unsigned)pixel[2];

		/* Each channel within 2 of the expected one. */
		same = formats_close(got, formats_expected[point]);
		verdict = "ok";
		if (!same) {
			verdict = "DIFFERS";
			failures++;
		}

		/* The point's line. */
		printf("EGLTEST PIXEL run=%s name=%s got=%06x expected=%06x %s\n", token, formats_names[point], got,
		       formats_expected[point], verdict);
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
formats_shader(
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
		printf("EGLTEST FORMATS compile failed: %s\n", log);
		return 0U;
	}

	/* Succeeded: the shader. */
	return shader;
}

/*
 * Makes the texture of each square, with the texels of its format, and
 * the sampler object that compares the shadow square's depth.
 */
static void
formats_textures_make(void)
{
	static const GLubyte red_half = 0x80U;
	static const GLushort halves[4] = { 0x3400U, 0x3800U, 0x3c00U, 0x3c00U };
	static const GLfloat floats[4] = { 0.0f, 1.0f, 0.5f, 1.0f };
	static const GLubyte unsigneds[4] = { 10U, 200U, 30U, 255U };
	static const GLint minus_five = -5;
	static const GLubyte red_green[2] = { 0x20U, 0x40U };
	static const GLfloat depths[2] = { 0.25f, 0.75f };
	static const GLushort depth16 = 0xc000U;
	static const GLubyte srgb[4] = { 0x80U, 0x80U, 0x80U, 0xffU };
	static const GLfloat small_floats[3] = { 1.0f, 0.5f, 0.25f };
	static const GLushort checker[16] = {
		0x0000U, 0x0000U, 0x0000U, 0x3c00U, 0x3c00U, 0x3c00U, 0x3c00U, 0x3c00U,
		0x3c00U, 0x3c00U, 0x3c00U, 0x3c00U, 0x0000U, 0x0000U, 0x0000U, 0x3c00U
	};

	/* The names, and rows of single bytes. */
	glGenTextures((GLsizei)FORMATS_SQUARES, formats_textures);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

	/* R8: fixed by glTexStorage2D, its one texel given by glTexSubImage2D. */
	formats_texture_bind(0U, GL_NEAREST, GL_NEAREST);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, 1, 1);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RED, GL_UNSIGNED_BYTE, &red_half);

	/* RGBA16F from half floats, linearly filtered. */
	formats_texture_bind(1U, GL_LINEAR, GL_LINEAR);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, 1, 1, 0, GL_RGBA, GL_HALF_FLOAT, halves);

	/* RGBA32F read with nearest filters. */
	formats_texture_bind(2U, GL_NEAREST, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 1, 1, 0, GL_RGBA, GL_FLOAT, floats);

	/* RGBA8UI. */
	formats_texture_bind(3U, GL_NEAREST, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8UI, 1, 1, 0, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, unsigneds);

	/* R32I of -5. */
	formats_texture_bind(4U, GL_NEAREST, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R32I, 1, 1, 0, GL_RED_INTEGER, GL_INT, &minus_five);

	/* RG8 read green, red, one. */
	formats_texture_bind(5U, GL_NEAREST, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, 1, 1, 0, GL_RG, GL_UNSIGNED_BYTE, red_green);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_GREEN);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_RED);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_ONE);

	/* A 32-bit float depth texture, 0.25 on the left and 0.75 on the right (compared through the sampler object). */
	formats_texture_bind(6U, GL_NEAREST, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, 2, 1, 0, GL_DEPTH_COMPONENT, GL_FLOAT, depths);

	/* A 16-bit depth texture of 0.75, read without comparison. */
	formats_texture_bind(7U, GL_NEAREST, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT16, 1, 1, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, &depth16);

	/* SRGB8_ALPHA8 of 0x80, which reads as linear 0.216. */
	formats_texture_bind(8U, GL_NEAREST, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, srgb);

	/* R11F_G11F_B10F from floats. */
	formats_texture_bind(9U, GL_NEAREST, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R11F_G11F_B10F, 1, 1, 0, GL_RGB, GL_FLOAT, small_floats);

	/* RGBA16F black and white in a 2x2 checker, mipmapped, read from level 1 (the mean). */
	formats_texture_bind(10U, GL_NEAREST_MIPMAP_NEAREST, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, 2, 2, 0, GL_RGBA, GL_HALF_FLOAT, checker);
	glGenerateMipmap(GL_TEXTURE_2D);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 1);

	/* RGBA32F with a linear filter, which cannot filter it: incomplete, black. */
	formats_texture_bind(11U, GL_LINEAR, GL_LINEAR);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 1, 1, 0, GL_RGBA, GL_FLOAT, floats);

	/* The sampler object: nearest texels compared less-or-equal with the reference. */
	glGenSamplers(1, &formats_sampler);
	glSamplerParameteri(formats_sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glSamplerParameteri(formats_sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glSamplerParameteri(formats_sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glSamplerParameteri(formats_sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glSamplerParameteri(formats_sampler, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
	glSamplerParameteri(formats_sampler, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
	glSamplerParameterf(formats_sampler, GL_TEXTURE_MIN_LOD, 0.5f);

	/* The unpack alignment back to GL's initial one. */
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glBindTexture(GL_TEXTURE_2D, 0U);
}

/* Binds a square's texture to unit 0 and gives it filters and edge clamping. */
static void
formats_texture_bind(
	unsigned square,
	GLenum min_filter,
	GLenum mag_filter)
{
	/* The texture and its sampling state. */
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, formats_textures[square]);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)min_filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)mag_filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/*
 * Checks what the API reports of the textures and the sampler object, and
 * the errors of calls that must fail.
 */
static void
formats_checks(void)
{
	static const GLubyte bytes[4] = { 1U, 2U, 3U, 4U };
	GLint value;
	GLfloat number;
	GLenum error;

	/* The storage texture is immutable with one level, and refuses glTexImage2D. */
	glBindTexture(GL_TEXTURE_2D, formats_textures[0]);
	value = 0;
	glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_IMMUTABLE_FORMAT, &value);
	formats_expect("immutable-format", (long)value, (long)GL_TRUE);
	value = 0;
	glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_IMMUTABLE_LEVELS, &value);
	formats_expect("immutable-levels", (long)value, 1L);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 1, 1, 0, GL_RED, GL_UNSIGNED_BYTE, bytes);
	error = glGetError();
	formats_expect("immutable-teximage", (long)error, (long)GL_INVALID_OPERATION);

	/* The swizzle reads back. */
	glBindTexture(GL_TEXTURE_2D, formats_textures[5]);
	value = 0;
	glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, &value);
	formats_expect("swizzle-r", (long)value, (long)GL_GREEN);

	/* An integer format refuses a format of normalized values. */
	glBindTexture(GL_TEXTURE_2D, formats_textures[3]);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, bytes);
	error = glGetError();
	formats_expect("integer-from-normalized", (long)error, (long)GL_INVALID_OPERATION);

	/* An integer texture has no mipmaps made. */
	glBindTexture(GL_TEXTURE_2D, formats_textures[4]);
	glGenerateMipmap(GL_TEXTURE_2D);
	error = glGetError();
	formats_expect("integer-mipmap", (long)error, (long)GL_INVALID_OPERATION);

	/* The sampler object and its parameters. */
	formats_expect("is-sampler", (long)glIsSampler(formats_sampler), (long)GL_TRUE);
	value = 0;
	glGetSamplerParameteriv(formats_sampler, GL_TEXTURE_COMPARE_FUNC, &value);
	formats_expect("sampler-compare-func", (long)value, (long)GL_LEQUAL);
	number = 0.0f;
	glGetSamplerParameterfv(formats_sampler, GL_TEXTURE_MIN_LOD, &number);
	formats_expect("sampler-min-lod-x10", (long)(number * 10.0f), 5L);

	/* The unit's sampler binding. */
	glActiveTexture(GL_TEXTURE0);
	glBindSampler(0U, formats_sampler);
	value = 0;
	glGetIntegerv(GL_SAMPLER_BINDING, &value);
	formats_expect("sampler-binding", (long)value, (long)formats_sampler);
	glBindSampler(0U, 0U);
	glBindTexture(GL_TEXTURE_2D, 0U);
}

/* Prints one of the start's checks, counting it when the value is not the one expected. */
static void
formats_expect(
	const char *what,
	long got,
	long expected)
{
	const char *verdict;

	/* A value that differs is a failure. */
	verdict = "ok";
	if (got != expected) {
		verdict = "FAILED";
		formats_failures++;
	}

	/* The line. */
	printf("EGLTEST FORMATS check %s got=%ld expected=%ld %s\n", what, got, expected, verdict);
}

/* Reports whether two 0xRRGGBB colours are within 2 in every channel. */
static int
formats_close(
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
