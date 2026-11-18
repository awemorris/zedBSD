/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * egltest's OpenGL ES 3.0 3D texture, 2D array texture and pixel buffer
 * scene (WS068 p028): twelve squares in three rows.
 *
 *   top row       a 2x2x4 3D texture of one colour per slice read at
 *                 slice 2 (0000ff), a 1x1x2 3D texture of black and white
 *                 filtered linearly between them (808080), a 2D array of
 *                 three layers made by glTexStorage3D and glTexSubImage3D
 *                 read at layer 2 (c04080), an RGBA8UI 2D array through a
 *                 usampler2DArray at layer 1 (0ac81e);
 *   middle row    a 2D texture from a pixel unpack buffer at an offset
 *                 (20c0a0), a 2D texture of one texel picked out of a
 *                 larger image by the unpack row length and skips
 *                 (e0e020), a 3D texture of one texel picked out of a
 *                 2x2x2 image by the image height and the images skipped
 *                 (8040ff), layer 1 of a 2D array that glCopyTexSubImage3D
 *                 filled from a framebuffer object (30a050);
 *   bottom row    a 2x2x2 3D texture of red and blue mipmapped by
 *                 glGenerateMipmap and read from its base level 1
 *                 (800080), an RGBA16F 2D array mipmapped the same way at
 *                 layer 1 (808080), the first 3D texture's size and a
 *                 texel read by textureSize and texelFetch (green when
 *                 right), a 2D array of 32-bit float depth compared with
 *                 0.5 through a sampler2DArrayShadow at layer 1 (left half
 *                 black, right half white).
 *
 * The start also checks what the API reports: the bindings and limits,
 * the immutable levels, the errors of wrong calls, and glReadPixels into
 * a pixel pack buffer and by the pack row length and skips.
 */

#include "volumes.h"

#include <GLES3/gl3.h>

#include <stdio.h>
#include <string.h>

/* The squares, the programs (by the kind of sampler), the textures, and the points read back. */
#define VOLUMES_SQUARES		12U
#define VOLUMES_PROGRAMS	6U
#define VOLUMES_TEXTURES	13U
#define VOLUMES_POINTS		13U

/* The kinds of sampler a square is drawn with. */
#define VOLUMES_VOLUME		0U
#define VOLUMES_ARRAY		1U
#define VOLUMES_ARRAY_UINT	2U
#define VOLUMES_FLAT		3U
#define VOLUMES_FETCH		4U
#define VOLUMES_SHADOW		5U

/* The textures, by what they hold. */
#define VOLUMES_T_SLICES	0U
#define VOLUMES_T_LINEAR	1U
#define VOLUMES_T_LAYERS	2U
#define VOLUMES_T_UINT		3U
#define VOLUMES_T_UNPACK	4U
#define VOLUMES_T_SKIP		5U
#define VOLUMES_T_IMAGE		6U
#define VOLUMES_T_COPY		7U
#define VOLUMES_T_MIPMAP_3D	8U
#define VOLUMES_T_MIPMAP_ARRAY	9U
#define VOLUMES_T_SHADOW	10U
#define VOLUMES_T_FBO		11U
#define VOLUMES_T_SPARE		12U

/* The attribute location of the squares' corners. */
#define VOLUMES_POSITION	0U

/*
 * One square: the program it is drawn with, its texture and that
 * texture's target, and the slice or layer it reads.
 */
struct volumes_square {
	unsigned kind;
	unsigned texture;
	GLenum target;
	GLfloat layer;
};

/*
 * The programs, their uniforms, the textures and the square's vertex
 * array; made by egltest_volumes_start and kept for the run, with the
 * number of the start's checks that failed.
 */
static GLuint volumes_programs[VOLUMES_PROGRAMS];
static GLint volumes_rects[VOLUMES_PROGRAMS];
static GLint volumes_layers[VOLUMES_PROGRAMS];
static GLuint volumes_textures[VOLUMES_TEXTURES];
static GLuint volumes_array;
static int volumes_failures;

/*
 * The vertex shader every program shares: a unit square placed by the
 * rectangle (centre, half size), its texture coordinates from 0 to 1.
 */
static const char volumes_vertex_source[] =
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
 * The fragment shaders: a 3D texture at a slice, a 2D array at a layer,
 * an unsigned 2D array over 255, a 2D texture, a 3D texture's size and
 * one texel checked (green when right, else red), and a shadow 2D array's
 * comparison with 0.5 as grey.
 */
static const char *const volumes_fragment_sources[VOLUMES_PROGRAMS] = {
	"#version 300 es\n"
	"precision mediump float;\n"
	"uniform mediump sampler3D u_texture;\n"
	"uniform float u_layer;\n"
	"in vec2 v_uv;\n"
	"out vec4 o_colour;\n"
	"void main()\n"
	"{\n"
	"\to_colour = vec4(texture(u_texture, vec3(v_uv, u_layer)).rgb, 1.0);\n"
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
	"}\n",

	/* An unsigned 2D array, over 255. */
	"#version 300 es\n"
	"precision mediump float;\n"
	"uniform mediump usampler2DArray u_texture;\n"
	"uniform float u_layer;\n"
	"in vec2 v_uv;\n"
	"out vec4 o_colour;\n"
	"void main()\n"
	"{\n"
	"\to_colour = vec4(vec3(texture(u_texture, vec3(v_uv, u_layer)).rgb) / 255.0, 1.0);\n"
	"}\n",

	/* A 2D texture. */
	"#version 300 es\n"
	"precision mediump float;\n"
	"uniform sampler2D u_texture;\n"
	"in vec2 v_uv;\n"
	"out vec4 o_colour;\n"
	"void main()\n"
	"{\n"
	"\to_colour = vec4(texture(u_texture, v_uv).rgb, 1.0);\n"
	"}\n",

	/* A 3D texture's size and its last texel (yellow). */
	"#version 300 es\n"
	"precision mediump float;\n"
	"uniform mediump sampler3D u_texture;\n"
	"in vec2 v_uv;\n"
	"out vec4 o_colour;\n"
	"void main()\n"
	"{\n"
	"\tvec4 texel = texelFetch(u_texture, ivec3(1, 1, 3), 0);\n"
	"\to_colour = vec4(1.0, 0.0, 0.0, 1.0);\n"
	"\tif (textureSize(u_texture, 0) == ivec3(2, 2, 4) && texel.r > 0.9 && texel.g > 0.9 && texel.b < 0.1)\n"
	"\t\to_colour = vec4(0.0, 1.0, 0.0, 1.0);\n"
	"}\n",

	/* A shadow 2D array: the comparison with 0.5 as grey. */
	"#version 300 es\n"
	"precision mediump float;\n"
	"uniform mediump sampler2DArrayShadow u_texture;\n"
	"uniform float u_layer;\n"
	"in vec2 v_uv;\n"
	"out vec4 o_colour;\n"
	"void main()\n"
	"{\n"
	"\tfloat lit = texture(u_texture, vec4(v_uv, u_layer, 0.5));\n"
	"\to_colour = vec4(lit, lit, lit, 1.0);\n"
	"}\n"
};

/* The squares, in their cells from the top left. */
static const struct volumes_square volumes_squares[VOLUMES_SQUARES] = {
	{ VOLUMES_VOLUME, VOLUMES_T_SLICES, GL_TEXTURE_3D, 0.625f },
	{ VOLUMES_VOLUME, VOLUMES_T_LINEAR, GL_TEXTURE_3D, 0.5f },
	{ VOLUMES_ARRAY, VOLUMES_T_LAYERS, GL_TEXTURE_2D_ARRAY, 2.0f },
	{ VOLUMES_ARRAY_UINT, VOLUMES_T_UINT, GL_TEXTURE_2D_ARRAY, 1.0f },
	{ VOLUMES_FLAT, VOLUMES_T_UNPACK, GL_TEXTURE_2D, 0.0f },
	{ VOLUMES_FLAT, VOLUMES_T_SKIP, GL_TEXTURE_2D, 0.0f },
	{ VOLUMES_VOLUME, VOLUMES_T_IMAGE, GL_TEXTURE_3D, 0.5f },
	{ VOLUMES_ARRAY, VOLUMES_T_COPY, GL_TEXTURE_2D_ARRAY, 1.0f },
	{ VOLUMES_VOLUME, VOLUMES_T_MIPMAP_3D, GL_TEXTURE_3D, 0.5f },
	{ VOLUMES_ARRAY, VOLUMES_T_MIPMAP_ARRAY, GL_TEXTURE_2D_ARRAY, 1.0f },
	{ VOLUMES_FETCH, VOLUMES_T_SLICES, GL_TEXTURE_3D, 0.0f },
	{ VOLUMES_SHADOW, VOLUMES_T_SHADOW, GL_TEXTURE_2D_ARRAY, 1.0f }
};

/*
 * The points read back: the squares' centres (the shadow square's two
 * halves), in the window's normalized coordinates, and the colours
 * expected there as 0xRRGGBB.
 */
static const char *const volumes_names[VOLUMES_POINTS] = {
	"3d-slice", "3d-linear", "array-layer", "array-uint",
	"unpack-buffer", "unpack-skip", "unpack-image", "copy-3d",
	"3d-mipmap", "array-mipmap", "size-fetch", "array-shadow-less", "array-shadow-more"
};
static const GLfloat volumes_points[VOLUMES_POINTS][2] = {
	{ -0.75f, 0.66f }, { -0.25f, 0.66f }, { 0.25f, 0.66f }, { 0.75f, 0.66f },
	{ -0.75f, 0.0f }, { -0.25f, 0.0f }, { 0.25f, 0.0f }, { 0.75f, 0.0f },
	{ -0.75f, -0.66f }, { -0.25f, -0.66f }, { 0.25f, -0.66f }, { 0.65f, -0.66f }, { 0.85f, -0.66f }
};
static const unsigned volumes_expected[VOLUMES_POINTS] = {
	0x0000ffU, 0x808080U, 0xc04080U, 0x0ac81eU,
	0x20c0a0U, 0xe0e020U, 0x8040ffU, 0x30a050U,
	0x800080U, 0x808080U, 0x00ff00U, 0x000000U, 0xffffffU
};

static GLuint volumes_shader(GLenum type, const char *source);
static void volumes_textures_make(void);
static void volumes_unpacked_make(void);
static void volumes_copy_make(void);
static void volumes_texture_bind(unsigned texture, GLenum target, GLenum min_filter, GLenum mag_filter);
static void volumes_checks(void);
static void volumes_pack_checks(void);
static void volumes_expect(const char *what, long got, long expected);
static int volumes_close(unsigned got, unsigned expected);

/*
 * Makes the programs, the square and the textures, and checks what the
 * API reports.  Returns 0, or -1 with a line saying what failed.
 */
int
egltest_volumes_start(void)
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
	for (kind = 0U; kind < VOLUMES_PROGRAMS; kind++) {
		vertex = volumes_shader(GL_VERTEX_SHADER, volumes_vertex_source);
		fragment = volumes_shader(GL_FRAGMENT_SHADER, volumes_fragment_sources[kind]);
		if (vertex == 0U || fragment == 0U)
			return -1;

		/* Linked. */
		volumes_programs[kind] = glCreateProgram();
		glAttachShader(volumes_programs[kind], vertex);
		glAttachShader(volumes_programs[kind], fragment);
		glLinkProgram(volumes_programs[kind]);
		glDeleteShader(vertex);
		glDeleteShader(fragment);
		linked = GL_FALSE;
		glGetProgramiv(volumes_programs[kind], GL_LINK_STATUS, &linked);
		if (!linked) {
			printf("EGLTEST VOLUMES link failed: program %u\n", kind);
			return -1;
		}

		/* Its rectangle, its layer (not every program has one), and its sampler on unit 0. */
		glUseProgram(volumes_programs[kind]);
		volumes_rects[kind] = glGetUniformLocation(volumes_programs[kind], "u_rect");
		volumes_layers[kind] = glGetUniformLocation(volumes_programs[kind], "u_layer");
		sampler = glGetUniformLocation(volumes_programs[kind], "u_texture");
		if (volumes_rects[kind] < 0 || sampler < 0) {
			printf("EGLTEST VOLUMES uniforms missing: program %u\n", kind);
			return -1;
		}

		/* The texture is on unit 0. */
		glUniform1i(sampler, 0);
	}

	/* The square's corners in a vertex array of their own. */
	glGenVertexArrays(1, &volumes_array);
	glBindVertexArray(volumes_array);
	glGenBuffers(1, &buffer);
	glBindBuffer(GL_ARRAY_BUFFER, buffer);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(corners), corners, GL_STATIC_DRAW);
	glVertexAttribPointer(VOLUMES_POSITION, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glEnableVertexAttribArray(VOLUMES_POSITION);
	glBindVertexArray(0U);

	/* The textures, and the checks of what the API reports. */
	glGenTextures((GLsizei)VOLUMES_TEXTURES, volumes_textures);
	volumes_textures_make();
	volumes_unpacked_make();
	volumes_copy_make();
	volumes_checks();

	/* Everything went without an error the checks did not expect. */
	error = glGetError();
	volumes_expect("start-glerror", (long)error, (long)GL_NO_ERROR);

	/* Succeeded: ready to draw. */
	printf("EGLTEST VOLUMES ready failures=%d\n", volumes_failures);
	fflush(stdout);
	return 0;
}

/*
 * Draws one square per texture over a window of a size.
 */
void
egltest_volumes_draw(
	int width,
	int height)
{
	const struct volumes_square *square;
	unsigned index;
	GLfloat x;
	GLfloat y;

	/* The whole window, cleared to dark grey. */
	glViewport(0, 0, width, height);
	glClearColor(0.125f, 0.125f, 0.125f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glBindVertexArray(volumes_array);
	glActiveTexture(GL_TEXTURE0);

	/* Each square in its cell (four columns, three rows), with its texture on unit 0 and its slice or layer. */
	for (index = 0U; index < VOLUMES_SQUARES; index++) {
		square = &volumes_squares[index];
		x = -0.75f + 0.5f * (GLfloat)(index % 4U);
		y = 0.66f - 0.66f * (GLfloat)(index / 4U);
		glUseProgram(volumes_programs[square->kind]);
		glUniform4f(volumes_rects[square->kind], x, y, 0.2f, 0.25f);
		if (volumes_layers[square->kind] >= 0)
			glUniform1f(volumes_layers[square->kind], square->layer);
		glBindTexture(square->target, volumes_textures[square->texture]);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	}

	/* The vertex array is let go. */
	glBindVertexArray(0U);
}

/*
 * Reads back each point and prints the colours; returns how many differ,
 * with the start's failed checks and any error.
 */
int
egltest_volumes_check(
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
	failures = volumes_failures;
	for (point = 0U; point < VOLUMES_POINTS; point++) {
		x = (int)((volumes_points[point][0] + 1.0f) * 0.5f * (float)width);
		y = (int)((volumes_points[point][1] + 1.0f) * 0.5f * (float)height);
		memset(pixel, 0, sizeof(pixel));
		glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
		got = ((unsigned)pixel[0] << 16) | ((unsigned)pixel[1] << 8) | (unsigned)pixel[2];

		/* Each channel within 2 of the expected one. */
		same = volumes_close(got, volumes_expected[point]);
		verdict = "ok";
		if (!same) {
			verdict = "DIFFERS";
			failures++;
		}

		/* The point's line. */
		printf("EGLTEST PIXEL run=%s name=%s got=%06x expected=%06x %s\n", token, volumes_names[point], got,
		       volumes_expected[point], verdict);
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
volumes_shader(
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
		printf("EGLTEST VOLUMES compile failed: %s\n", log);
		return 0U;
	}

	/* Succeeded: the shader. */
	return shader;
}

/*
 * Makes the 3D and 2D array textures given their texels directly: the
 * slices, the linear pair, the layers, the unsigned layers, the mipmapped
 * ones and the depth layers.
 */
static void
volumes_textures_make(void)
{
	static const GLubyte slices[64] = {
		0xffU, 0x00U, 0x00U, 0xffU, 0xffU, 0x00U, 0x00U, 0xffU, 0xffU, 0x00U, 0x00U, 0xffU, 0xffU, 0x00U, 0x00U, 0xffU,
		0x00U, 0xffU, 0x00U, 0xffU, 0x00U, 0xffU, 0x00U, 0xffU, 0x00U, 0xffU, 0x00U, 0xffU, 0x00U, 0xffU, 0x00U, 0xffU,
		0x00U, 0x00U, 0xffU, 0xffU, 0x00U, 0x00U, 0xffU, 0xffU, 0x00U, 0x00U, 0xffU, 0xffU, 0x00U, 0x00U, 0xffU, 0xffU,
		0xffU, 0xffU, 0x00U, 0xffU, 0xffU, 0xffU, 0x00U, 0xffU, 0xffU, 0xffU, 0x00U, 0xffU, 0xffU, 0xffU, 0x00U, 0xffU
	};
	static const GLubyte pair[8] = { 0x00U, 0x00U, 0x00U, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU };
	static const GLubyte layers[12] = { 0xffU, 0x00U, 0x00U, 0xffU, 0x40U, 0x80U, 0xc0U, 0xffU, 0xc0U, 0x40U, 0x80U, 0xffU };
	static const GLubyte unsigneds[8] = { 1U, 2U, 3U, 4U, 10U, 200U, 30U, 255U };
	static const GLubyte red_blue[32] = {
		0xffU, 0x00U, 0x00U, 0xffU, 0x00U, 0x00U, 0xffU, 0xffU, 0x00U, 0x00U, 0xffU, 0xffU, 0xffU, 0x00U, 0x00U, 0xffU,
		0x00U, 0x00U, 0xffU, 0xffU, 0xffU, 0x00U, 0x00U, 0xffU, 0xffU, 0x00U, 0x00U, 0xffU, 0x00U, 0x00U, 0xffU, 0xffU
	};
	static const GLushort checker[32] = {
		0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U,
		0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U, 0x0000U,
		0x0000U, 0x0000U, 0x0000U, 0x3c00U, 0x3c00U, 0x3c00U, 0x3c00U, 0x3c00U,
		0x3c00U, 0x3c00U, 0x3c00U, 0x3c00U, 0x0000U, 0x0000U, 0x0000U, 0x3c00U
	};
	static const GLfloat depths[4] = { 1.0f, 1.0f, 0.25f, 0.75f };

	/* Rows of single bytes. */
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

	/* A 2x2x4 3D texture, one colour per slice: red, green, blue, yellow. */
	volumes_texture_bind(VOLUMES_T_SLICES, GL_TEXTURE_3D, GL_NEAREST, GL_NEAREST);
	glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, 2, 2, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, slices);

	/* A 1x1x2 3D texture, black then white, filtered linearly. */
	volumes_texture_bind(VOLUMES_T_LINEAR, GL_TEXTURE_3D, GL_LINEAR, GL_LINEAR);
	glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA, 1, 1, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, pair);

	/* A 2D array of three layers, fixed by glTexStorage3D and filled layer by layer. */
	volumes_texture_bind(VOLUMES_T_LAYERS, GL_TEXTURE_2D_ARRAY, GL_NEAREST, GL_NEAREST);
	glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, 1, 1, 3);
	glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, layers);
	glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 1, 1, 1, 2, GL_RGBA, GL_UNSIGNED_BYTE, layers + 4);

	/* An RGBA8UI 2D array of two layers. */
	volumes_texture_bind(VOLUMES_T_UINT, GL_TEXTURE_2D_ARRAY, GL_NEAREST, GL_NEAREST);
	glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8UI, 1, 1, 2, 0, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, unsigneds);

	/* A 2x2x2 3D texture of red and blue in a checker, mipmapped, read from level 1 (the mean). */
	volumes_texture_bind(VOLUMES_T_MIPMAP_3D, GL_TEXTURE_3D, GL_NEAREST_MIPMAP_NEAREST, GL_NEAREST);
	glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, 2, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, red_blue);
	glGenerateMipmap(GL_TEXTURE_3D);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_BASE_LEVEL, 1);

	/* An RGBA16F 2D array: layer 0 black, layer 1 a black and white checker; mipmapped, read from level 1. */
	volumes_texture_bind(VOLUMES_T_MIPMAP_ARRAY, GL_TEXTURE_2D_ARRAY, GL_NEAREST_MIPMAP_NEAREST, GL_NEAREST);
	glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA16F, 2, 2, 2, 0, GL_RGBA, GL_HALF_FLOAT, checker);
	glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BASE_LEVEL, 1);

	/* A 2D array of 32-bit float depth, layer 1 0.25 on the left and 0.75 on the right, compared less-or-equal. */
	volumes_texture_bind(VOLUMES_T_SHADOW, GL_TEXTURE_2D_ARRAY, GL_NEAREST, GL_NEAREST);
	glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT32F, 2, 1, 2, 0, GL_DEPTH_COMPONENT, GL_FLOAT, depths);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);

	/* The unpack alignment back to GL's initial one. */
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
}

/*
 * Makes the textures whose texels the pixel store and the pixel unpack
 * buffer pick out: one from a buffer at an offset, one out of a larger
 * image by the row length and skips, one out of a 2x2x2 image by the
 * image height and the images skipped.
 */
static void
volumes_unpacked_make(void)
{
	static const GLubyte buffered[8] = { 0xffU, 0xffU, 0xffU, 0xffU, 0x20U, 0xc0U, 0xa0U, 0xffU };
	static const GLubyte wide[48] = {
		0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
		0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0xe0U, 0xe0U, 0x20U, 0xffU, 0U, 0U, 0U, 0U,
		0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U
	};
	static const GLubyte box[32] = {
		0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
		0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0x80U, 0x40U, 0xffU, 0xffU
	};
	GLuint buffer;

	/* A 2D texture from a pixel unpack buffer, 4 bytes in. */
	glGenBuffers(1, &buffer);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer);
	glBufferData(GL_PIXEL_UNPACK_BUFFER, (GLsizeiptr)sizeof(buffered), buffered, GL_STATIC_DRAW);
	volumes_texture_bind(VOLUMES_T_UNPACK, GL_TEXTURE_2D, GL_NEAREST, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, (const void *)4);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0U);

	/* The texel at column 2 of row 1 of a 4x3 image. */
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 4);
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, 2);
	glPixelStorei(GL_UNPACK_SKIP_ROWS, 1);
	volumes_texture_bind(VOLUMES_T_SKIP, GL_TEXTURE_2D, GL_NEAREST, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, wide);

	/* The texel at column 1 of row 1 of image 1 of a 2x2x2 image. */
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 2);
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, 1);
	glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 2);
	glPixelStorei(GL_UNPACK_SKIP_IMAGES, 1);
	volumes_texture_bind(VOLUMES_T_IMAGE, GL_TEXTURE_3D, GL_NEAREST, GL_NEAREST);
	glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, 1, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, box);

	/* The pixel store back to GL's initial one. */
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
	glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
	glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
	glPixelStorei(GL_UNPACK_SKIP_IMAGES, 0);
}

/*
 * Clears a framebuffer object's texture to 30a050, copies it into layer 1
 * of a 2D array by glCopyTexSubImage3D, and checks glReadPixels of it into
 * a pixel pack buffer and by the pack row length and skips.
 */
static void
volumes_copy_make(void)
{
	GLuint framebuffer;
	GLenum status;

	/* The framebuffer object's 4x4 texture. */
	volumes_texture_bind(VOLUMES_T_FBO, GL_TEXTURE_2D, GL_NEAREST, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, volumes_textures[VOLUMES_T_FBO], 0);
	status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	volumes_expect("fbo-complete", (long)status, (long)GL_FRAMEBUFFER_COMPLETE);

	/* Cleared to 30a050. */
	glViewport(0, 0, 4, 4);
	glClearColor((GLfloat)0x30 / 255.0f, (GLfloat)0xa0 / 255.0f, (GLfloat)0x50 / 255.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	/* A 4x4 2D array of two layers, layer 1 copied from the framebuffer. */
	volumes_texture_bind(VOLUMES_T_COPY, GL_TEXTURE_2D_ARRAY, GL_NEAREST, GL_NEAREST);
	glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, 4, 4, 2);
	glCopyTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 1, 0, 0, 4, 4);

	/* The framebuffer read into a pack buffer and by the pack store, then the window's framebuffer again. */
	volumes_pack_checks();
	glBindFramebuffer(GL_FRAMEBUFFER, 0U);
}

/* Binds a texture of a target to unit 0 and gives it filters and edge clamping. */
static void
volumes_texture_bind(
	unsigned texture,
	GLenum target,
	GLenum min_filter,
	GLenum mag_filter)
{
	/* The texture and its sampling state. */
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(target, volumes_textures[texture]);
	glTexParameteri(target, GL_TEXTURE_MIN_FILTER, (GLint)min_filter);
	glTexParameteri(target, GL_TEXTURE_MAG_FILTER, (GLint)mag_filter);
	glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(target, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
}

/*
 * Checks what the API reports of the 3D and 2D array textures and the
 * pixel store, and the errors of calls that must fail.
 */
static void
volumes_checks(void)
{
	static const GLubyte bytes[8] = { 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U };
	static const GLfloat depth[1] = { 0.5f };
	GLuint buffer;
	GLint value;
	GLenum error;

	/* The bindings of the new targets. */
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_3D, volumes_textures[VOLUMES_T_SLICES]);
	value = 0;
	glGetIntegerv(GL_TEXTURE_BINDING_3D, &value);
	volumes_expect("binding-3d", (long)value, (long)volumes_textures[VOLUMES_T_SLICES]);
	glBindTexture(GL_TEXTURE_2D_ARRAY, volumes_textures[VOLUMES_T_LAYERS]);
	value = 0;
	glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &value);
	volumes_expect("binding-2d-array", (long)value, (long)volumes_textures[VOLUMES_T_LAYERS]);

	/* OpenGL ES 3.0's least limits. */
	value = 0;
	glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &value);
	volumes_expect("max-3d-size-at-least-256", (long)(value >= 256), 1L);
	value = 0;
	glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &value);
	volumes_expect("max-layers-at-least-256", (long)(value >= 256), 1L);

	/* The storage array is immutable with one level, refuses another storage and a box beyond its layers. */
	value = 0;
	glGetTexParameteriv(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_IMMUTABLE_LEVELS, &value);
	volumes_expect("immutable-levels", (long)value, 1L);
	glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, 1, 1, 3);
	error = glGetError();
	volumes_expect("storage-twice", (long)error, (long)GL_INVALID_OPERATION);
	glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 2, 1, 1, 2, GL_RGBA, GL_UNSIGNED_BYTE, bytes);
	error = glGetError();
	volumes_expect("box-beyond-layers", (long)error, (long)GL_INVALID_VALUE);

	/* glTexImage3D takes no 2D target, and a 3D texture holds no depth. */
	glTexImage3D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, bytes);
	error = glGetError();
	volumes_expect("teximage3d-2d-target", (long)error, (long)GL_INVALID_ENUM);
	glBindTexture(GL_TEXTURE_3D, volumes_textures[VOLUMES_T_SPARE]);
	glTexImage3D(GL_TEXTURE_3D, 0, GL_DEPTH_COMPONENT32F, 1, 1, 1, 0, GL_DEPTH_COMPONENT, GL_FLOAT, depth);
	error = glGetError();
	volumes_expect("3d-depth", (long)error, (long)GL_INVALID_OPERATION);

	/* The pixel store reads back, and refuses a negative length. */
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 7);
	value = 0;
	glGetIntegerv(GL_UNPACK_ROW_LENGTH, &value);
	volumes_expect("unpack-row-length", (long)value, 7L);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, -1);
	error = glGetError();
	volumes_expect("negative-row-length", (long)error, (long)GL_INVALID_VALUE);

	/* Texels beyond the end of the unpack buffer are refused. */
	glGenBuffers(1, &buffer);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer);
	glBufferData(GL_PIXEL_UNPACK_BUFFER, (GLsizeiptr)sizeof(bytes), bytes, GL_STATIC_DRAW);
	glBindTexture(GL_TEXTURE_2D, volumes_textures[VOLUMES_T_UNPACK]);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, (const void *)8);
	error = glGetError();
	volumes_expect("unpack-beyond-buffer", (long)error, (long)GL_INVALID_OPERATION);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0U);
	glDeleteBuffers(1, &buffer);

	/* The targets let go. */
	glBindTexture(GL_TEXTURE_2D, 0U);
	glBindTexture(GL_TEXTURE_3D, 0U);
	glBindTexture(GL_TEXTURE_2D_ARRAY, 0U);
}

/*
 * Reads the framebuffer object (30a050) into a pixel pack buffer 4 bytes
 * in and into memory by the pack row length and skips, and checks where
 * the bytes went.
 */
static void
volumes_pack_checks(void)
{
	GLubyte memory[16];
	const GLubyte *mapped;
	GLuint buffer;
	unsigned index;
	int untouched;

	/* Into a pack buffer, 4 bytes in, read back through a mapping. */
	glGenBuffers(1, &buffer);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, buffer);
	glBufferData(GL_PIXEL_PACK_BUFFER, 16, NULL, GL_STREAM_READ);
	glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, (void *)4);
	mapped = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, 16, GL_MAP_READ_BIT);
	if (mapped == NULL) {
		volumes_expect("pack-buffer-mapped", 0L, 1L);
	} else {
		volumes_expect("pack-buffer-pixel", (long)(((unsigned)mapped[4] << 16) | ((unsigned)mapped[5] << 8) | mapped[6]), 0x30a050L);
		(void)glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
	}

	/* The pack buffer let go. */
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0U);
	glDeleteBuffers(1, &buffer);

	/* Into memory at column 1 of row 1 of rows two pixels long: bytes 12 to 15, the others untouched. */
	memset(memory, 0, sizeof(memory));
	glPixelStorei(GL_PACK_ROW_LENGTH, 2);
	glPixelStorei(GL_PACK_SKIP_PIXELS, 1);
	glPixelStorei(GL_PACK_SKIP_ROWS, 1);
	glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, memory);
	glPixelStorei(GL_PACK_ROW_LENGTH, 0);
	glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
	glPixelStorei(GL_PACK_SKIP_ROWS, 0);
	volumes_expect("pack-skip-pixel", (long)(((unsigned)memory[12] << 16) | ((unsigned)memory[13] << 8) | memory[14]), 0x30a050L);

	/* The bytes before it are untouched. */
	untouched = 1;
	for (index = 0U; index < 12U; index++) {
		if (memory[index] != 0U)
			untouched = 0;
	}

	/* Reported. */
	volumes_expect("pack-skip-untouched", (long)untouched, 1L);
}

/* Prints one of the start's checks, counting it when the value is not the one expected. */
static void
volumes_expect(
	const char *what,
	long got,
	long expected)
{
	const char *verdict;

	/* A value that differs is a failure. */
	verdict = "ok";
	if (got != expected) {
		verdict = "FAILED";
		volumes_failures++;
	}

	/* The line. */
	printf("EGLTEST VOLUMES check %s got=%ld expected=%ld %s\n", what, got, expected, verdict);
}

/* Reports whether two 0xRRGGBB colours are within 2 in every channel. */
static int
volumes_close(
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
