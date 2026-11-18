/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * egltest's ETC2 and EAC compressed texture scene: twelve squares in
 * three rows, one per compressed texture.  Each texture is two blocks
 * wide (8x4 texels; the array one is one block in each of two layers),
 * and its square's shader fetches one texel of each block (texelFetch)
 * and draws green when both are the values the blocks decode to, red
 * otherwise.
 *
 *   top row       RGB8_ETC2, SRGB8_ETC2, RGB8_PUNCHTHROUGH_ALPHA1_ETC2
 *                 (an opaque block and a transparent one),
 *                 SRGB8_PUNCHTHROUGH_ALPHA1_ETC2;
 *   middle row    RGBA8_ETC2_EAC, SRGB8_ALPHA8_ETC2_EAC, R11_EAC,
 *                 SIGNED_R11_EAC;
 *   bottom row    RG11_EAC, SIGNED_RG11_EAC, an RGB8_ETC2 texture whose
 *                 second block glCompressedTexSubImage2D replaced, and a
 *                 2D array of RGBA8_ETC2_EAC by glCompressedTexImage3D
 *                 whose second layer glCompressedTexSubImage3D replaced.
 *
 * The blocks are of the differential mode of ETC2 (every difference 0,
 * every texel of a block with the same index) and EAC blocks with every
 * texel of the same index, so each block is one value: its base colour
 * (5 bits widened to 8) plus the table's modifier, clamped; an EAC
 * value is its base plus the modifier times the multiplier (times 8,
 * with 4 added, for an unsigned 11-bit channel; a multiplier of 0 adds
 * the modifier alone), of 2047 or of 1023 when signed.  The decoding
 * rules are those of the Khronos Data Format Specification's ETC2
 * section (OpenGL ES 3.0, annex C).
 *
 * The start also checks the compressed formats the API lists and the
 * errors of wrong calls.
 */

#include "etc2.h"

#include <GLES3/gl3.h>

#include <stdio.h>
#include <string.h>

/* The squares, the programs (a 2D sampler and a 2D array one), and the bytes of a block. */
#define ETC2_SQUARES		12U
#define ETC2_PROGRAMS		2U
#define ETC2_BLOCK_BYTES	8U

/* The attribute location of the squares' corners. */
#define ETC2_POSITION		0U

/*
 * One square: its texture's internal format, the texel fetched in each
 * of its two blocks (the array one's layers), and the values expected
 * there (red, green, blue, alpha).
 */
struct etc2_square {
	const char *name;
	GLenum internal;
	GLint texels[4];
	GLint layers[2];
	GLfloat expected[2][4];
};

/*
 * The programs, their uniforms, the textures and the square's vertex
 * array; made by egltest_etc2_start and kept for the run, with the
 * number of the start's checks that failed.
 */
static GLuint etc2_programs[ETC2_PROGRAMS];
static GLint etc2_rects[ETC2_PROGRAMS];
static GLint etc2_texels[ETC2_PROGRAMS];
static GLint etc2_layers[ETC2_PROGRAMS];
static GLint etc2_expected0[ETC2_PROGRAMS];
static GLint etc2_expected1[ETC2_PROGRAMS];
static GLuint etc2_textures[ETC2_SQUARES];
static GLuint etc2_array;
static int etc2_failures;

/*
 * The vertex shader both programs share: a unit square placed by the
 * rectangle (centre, half size).
 */
static const char etc2_vertex_source[] =
	"#version 300 es\n"
	"layout(location = 0) in vec2 a_position;\n"
	"uniform vec4 u_rect;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\tgl_Position = vec4(u_rect.xy + a_position * u_rect.zw, 0.0, 1.0);\n"
	"}\n";

/*
 * The fragment shaders: two texels fetched (of a 2D texture, or of a 2D
 * array's layers), green when each is within 0.0019 (under half of one
 * step of 8 bits) of its expected value, red otherwise.
 */
static const char *const etc2_fragment_sources[ETC2_PROGRAMS] = {
	"#version 300 es\n"
	"precision highp float;\n"
	"uniform highp sampler2D u_texture;\n"
	"uniform ivec4 u_texels;\n"
	"uniform vec4 u_expected0;\n"
	"uniform vec4 u_expected1;\n"
	"out vec4 o_colour;\n"
	"void main()\n"
	"{\n"
	"\tvec4 first = texelFetch(u_texture, u_texels.xy, 0);\n"
	"\tvec4 second = texelFetch(u_texture, u_texels.zw, 0);\n"
	"\tbvec4 near0 = lessThan(abs(first - u_expected0), vec4(0.0019));\n"
	"\tbvec4 near1 = lessThan(abs(second - u_expected1), vec4(0.0019));\n"
	"\to_colour = vec4(1.0, 0.0, 0.0, 1.0);\n"
	"\tif (all(near0) && all(near1))\n"
	"\t\to_colour = vec4(0.0, 1.0, 0.0, 1.0);\n"
	"}\n",

	/* A 2D array's two layers. */
	"#version 300 es\n"
	"precision highp float;\n"
	"uniform highp sampler2DArray u_texture;\n"
	"uniform ivec4 u_texels;\n"
	"uniform ivec2 u_layers;\n"
	"uniform vec4 u_expected0;\n"
	"uniform vec4 u_expected1;\n"
	"out vec4 o_colour;\n"
	"void main()\n"
	"{\n"
	"\tvec4 first = texelFetch(u_texture, ivec3(u_texels.xy, u_layers.x), 0);\n"
	"\tvec4 second = texelFetch(u_texture, ivec3(u_texels.zw, u_layers.y), 0);\n"
	"\tbvec4 near0 = lessThan(abs(first - u_expected0), vec4(0.0019));\n"
	"\tbvec4 near1 = lessThan(abs(second - u_expected1), vec4(0.0019));\n"
	"\to_colour = vec4(1.0, 0.0, 0.0, 1.0);\n"
	"\tif (all(near0) && all(near1))\n"
	"\t\to_colour = vec4(0.0, 1.0, 0.0, 1.0);\n"
	"}\n"
};

/*
 * The squares in order.  The values: 5-bit 16, 8 and 24 widen to 132,
 * 66 and 198, 31 to 255, 8 to 66 and 4 to 33; table 0's modifiers are
 * +2, +8, -2 and -8 by index; an sRGB 2 reads 0.000607, 8 reads
 * 0.002428 and 130 reads 0.223228.
 */
static const struct etc2_square etc2_squares[ETC2_SQUARES] = {
	/* 16,8,24 + 2 = 134,68,200; 31,0,0 - 8 = 247,0,0. */
	{ "rgb8", GL_COMPRESSED_RGB8_ETC2, { 1, 1, 5, 2 }, { 0, 0 },
	  { { 0.525490f, 0.266667f, 0.784314f, 1.0f }, { 0.968627f, 0.0f, 0.0f, 1.0f } } },

	/* 31,0,0 + 2 = 255,2,2; 16,16,16 - 2 = 130 each. */
	{ "srgb8", GL_COMPRESSED_SRGB8_ETC2, { 1, 1, 5, 2 }, { 0, 0 },
	  { { 1.0f, 0.000607f, 0.000607f, 1.0f }, { 0.223228f, 0.223228f, 0.223228f, 1.0f } } },

	/* Not opaque: index 0 is the base 66,255,33 with no modifier, index 2 transparent black. */
	{ "rgb8-punchthrough", GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2, { 1, 1, 5, 2 }, { 0, 0 },
	  { { 0.258824f, 1.0f, 0.129412f, 1.0f }, { 0.0f, 0.0f, 0.0f, 0.0f } } },

	/* Opaque: 0,0,31 + 8 = 8,8,255; then a transparent block. */
	{ "srgb8-punchthrough", GL_COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2, { 1, 1, 5, 2 }, { 0, 0 },
	  { { 0.002428f, 0.002428f, 1.0f, 1.0f }, { 0.0f, 0.0f, 0.0f, 0.0f } } },

	/* 134,68,200 with alpha 126 + 2 = 128; 0,31,0 + 2 = 2,255,2 with alpha 200 - 3 * 2 = 194. */
	{ "rgba8-eac", GL_COMPRESSED_RGBA8_ETC2_EAC, { 1, 1, 5, 2 }, { 0, 0 },
	  { { 0.525490f, 0.266667f, 0.784314f, 0.501961f }, { 0.007843f, 1.0f, 0.007843f, 0.760784f } } },

	/* 255,2,2 with alpha 255 + 14 clamped; 130 each with alpha 10 - 15 * 3 clamped to 0. */
	{ "srgb8-alpha8-eac", GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC, { 1, 1, 5, 2 }, { 0, 0 },
	  { { 1.0f, 0.000607f, 0.000607f, 1.0f }, { 0.223228f, 0.223228f, 0.223228f, 0.0f } } },

	/* 128 * 8 + 4 + 2 * 8 = 1044 of 2047; 0 * 8 + 4 + 14 (multiplier 0) = 18 of 2047. */
	{ "r11", GL_COMPRESSED_R11_EAC, { 1, 1, 5, 2 }, { 0, 0 },
	  { { 0.510015f, 0.0f, 0.0f, 1.0f }, { 0.008793f, 0.0f, 0.0f, 1.0f } } },

	/* 64 * 8 + 2 * 8 = 528 of 1023; -128 (taken as -127) * 8 - 3 * 8 clamped to -1023. */
	{ "signed-r11", GL_COMPRESSED_SIGNED_R11_EAC, { 1, 1, 5, 2 }, { 0, 0 },
	  { { 0.516129f, 0.0f, 0.0f, 1.0f }, { -1.0f, 0.0f, 0.0f, 1.0f } } },

	/* 255 * 8 + 4 + 14 * 15 * 8 clamped, 0 - 15 * 15 * 8 clamped; 1044, and 64 * 8 + 4 + 5 * 2 * 8 = 596 of 2047. */
	{ "rg11", GL_COMPRESSED_RG11_EAC, { 1, 1, 5, 2 }, { 0, 0 },
	  { { 1.0f, 0.0f, 0.0f, 1.0f }, { 0.510015f, 0.291158f, 0.0f, 1.0f } } },

	/* 528 and -64 * 8 + 2 * 8 = -496 of 1023; 127 * 8 + 14 * 15 * 8 clamped, and 0 - 3 (multiplier 0). */
	{ "signed-rg11", GL_COMPRESSED_SIGNED_RG11_EAC, { 1, 1, 5, 2 }, { 0, 0 },
	  { { 0.516129f, -0.484848f, 0.0f, 1.0f }, { 1.0f, -0.002933f, 0.0f, 1.0f } } },

	/* 247,0,0 kept in the first block; the second replaced by 2,255,2. */
	{ "rgb8-subimage", GL_COMPRESSED_RGB8_ETC2, { 1, 1, 5, 2 }, { 0, 0 },
	  { { 0.968627f, 0.0f, 0.0f, 1.0f }, { 0.007843f, 1.0f, 0.007843f, 1.0f } } },

	/* The RGBA8 square's blocks in layers 0 and 1 (layer 1 replaced). */
	{ "rgba8-eac-array", GL_COMPRESSED_RGBA8_ETC2_EAC, { 1, 1, 2, 2 }, { 0, 1 },
	  { { 0.525490f, 0.266667f, 0.784314f, 0.501961f }, { 0.007843f, 1.0f, 0.007843f, 0.760784f } } }
};

static GLuint etc2_shader(GLenum type, const char *source);
static void etc2_colour_block(unsigned red, unsigned green, unsigned blue, unsigned bit33, unsigned index, unsigned char *bytes);
static void etc2_eac_block(unsigned base, unsigned multiplier, unsigned index, unsigned char *bytes);
static void etc2_store(unsigned long long word, unsigned char *bytes);
static void etc2_textures_make(void);
static void etc2_texture_bind(GLenum target, GLuint texture);
static void etc2_checks(void);
static void etc2_expect(const char *what, long got, long expected);

/*
 * Makes the programs, the square and the textures, and checks what the
 * API reports.  Returns 0, or -1 with a line saying what failed.
 */
int
egltest_etc2_start(void)
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
	for (kind = 0U; kind < ETC2_PROGRAMS; kind++) {
		vertex = etc2_shader(GL_VERTEX_SHADER, etc2_vertex_source);
		fragment = etc2_shader(GL_FRAGMENT_SHADER, etc2_fragment_sources[kind]);
		if (vertex == 0U || fragment == 0U)
			return -1;

		/* Linked. */
		etc2_programs[kind] = glCreateProgram();
		glAttachShader(etc2_programs[kind], vertex);
		glAttachShader(etc2_programs[kind], fragment);
		glLinkProgram(etc2_programs[kind]);
		glDeleteShader(vertex);
		glDeleteShader(fragment);
		linked = GL_FALSE;
		glGetProgramiv(etc2_programs[kind], GL_LINK_STATUS, &linked);
		if (!linked) {
			printf("EGLTEST ETC2 link failed: program %u\n", kind);
			return -1;
		}

		/* Its uniforms. */
		glUseProgram(etc2_programs[kind]);
		etc2_rects[kind] = glGetUniformLocation(etc2_programs[kind], "u_rect");
		etc2_texels[kind] = glGetUniformLocation(etc2_programs[kind], "u_texels");
		etc2_layers[kind] = glGetUniformLocation(etc2_programs[kind], "u_layers");
		etc2_expected0[kind] = glGetUniformLocation(etc2_programs[kind], "u_expected0");
		etc2_expected1[kind] = glGetUniformLocation(etc2_programs[kind], "u_expected1");
		sampler = glGetUniformLocation(etc2_programs[kind], "u_texture");
		if (etc2_rects[kind] < 0 || etc2_texels[kind] < 0 || etc2_expected0[kind] < 0 || etc2_expected1[kind] < 0 ||
		    sampler < 0) {
			printf("EGLTEST ETC2 uniforms missing: program %u\n", kind);
			return -1;
		}

		/* The texture is on unit 0. */
		glUniform1i(sampler, 0);
	}

	/* The 2D array program reads its layers. */
	if (etc2_layers[1] < 0) {
		printf("EGLTEST ETC2 uniforms missing: u_layers\n");
		return -1;
	}

	/* The square's corners in a vertex array of their own. */
	glGenVertexArrays(1, &etc2_array);
	glBindVertexArray(etc2_array);
	glGenBuffers(1, &buffer);
	glBindBuffer(GL_ARRAY_BUFFER, buffer);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(corners), corners, GL_STATIC_DRAW);
	glVertexAttribPointer(ETC2_POSITION, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glEnableVertexAttribArray(ETC2_POSITION);
	glBindVertexArray(0U);

	/* The textures, and the checks of what the API reports. */
	etc2_textures_make();
	etc2_checks();

	/* Everything went without an error the checks did not expect. */
	error = glGetError();
	etc2_expect("start-glerror", (long)error, (long)GL_NO_ERROR);

	/* Succeeded: ready to draw. */
	printf("EGLTEST ETC2 ready failures=%d\n", etc2_failures);
	fflush(stdout);
	return 0;
}

/*
 * Draws one square per texture over a window of a size, each with the
 * texels to fetch and the values expected there.
 */
void
egltest_etc2_draw(
	int width,
	int height)
{
	const struct etc2_square *square;
	unsigned index;
	unsigned kind;
	GLenum target;
	GLfloat x;
	GLfloat y;

	/* The whole window, cleared to dark grey. */
	glViewport(0, 0, width, height);
	glClearColor(0.125f, 0.125f, 0.125f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glBindVertexArray(etc2_array);
	glActiveTexture(GL_TEXTURE0);

	/* Each square in its cell (four columns, three rows), with its texture on unit 0. */
	for (index = 0U; index < ETC2_SQUARES; index++) {
		square = &etc2_squares[index];

		/* The last square is the 2D array's. */
		kind = 0U;
		target = GL_TEXTURE_2D;
		if (index == ETC2_SQUARES - 1U) {
			kind = 1U;
			target = GL_TEXTURE_2D_ARRAY;
		}

		/* Its place, texels and expected values. */
		x = -0.75f + 0.5f * (GLfloat)(index % 4U);
		y = 0.66f - 0.66f * (GLfloat)(index / 4U);
		glUseProgram(etc2_programs[kind]);
		glUniform4f(etc2_rects[kind], x, y, 0.2f, 0.25f);
		glUniform4i(etc2_texels[kind], square->texels[0], square->texels[1], square->texels[2], square->texels[3]);
		if (etc2_layers[kind] >= 0)
			glUniform2i(etc2_layers[kind], square->layers[0], square->layers[1]);
		glUniform4fv(etc2_expected0[kind], 1, square->expected[0]);
		glUniform4fv(etc2_expected1[kind], 1, square->expected[1]);

		/* Drawn with its texture. */
		glBindTexture(target, etc2_textures[index]);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glBindTexture(target, 0U);
	}

	/* The vertex array is let go. */
	glBindVertexArray(0U);
}

/*
 * Reads back each square's centre and prints its colour (green when the
 * texels decoded as expected); returns how many differ, with the start's
 * failed checks and any error.
 */
int
egltest_etc2_check(
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
	failures = etc2_failures;
	for (index = 0U; index < ETC2_SQUARES; index++) {
		x = (int)((-0.75f + 0.5f * (GLfloat)(index % 4U) + 1.0f) * 0.5f * (float)width);
		y = (int)((0.66f - 0.66f * (GLfloat)(index / 4U) + 1.0f) * 0.5f * (float)height);
		memset(pixel, 0, sizeof(pixel));
		glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
		got = ((unsigned)pixel[0] << 16) | ((unsigned)pixel[1] << 8) | (unsigned)pixel[2];

		/* Green is the decoding expected. */
		verdict = "ok";
		if (got != 0x00ff00U) {
			verdict = "DIFFERS";
			failures++;
		}

		/* The square's line. */
		printf("EGLTEST PIXEL run=%s name=%s got=%06x expected=00ff00 %s\n", token, etc2_squares[index].name, got,
		       verdict);
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
etc2_shader(
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
		printf("EGLTEST ETC2 compile failed: %s\n", log);
		return 0U;
	}

	/* Succeeded: the shader. */
	return shader;
}

/*
 * Writes an ETC2 colour block of the differential mode with every
 * difference 0 and both halves of table 0: a base colour of 5 bits per
 * channel, bit 33 (the differential bit, or the opaque bit of a
 * punch-through format), and the one index (0 to 3) of every texel.
 */
static void
etc2_colour_block(
	unsigned red,
	unsigned green,
	unsigned blue,
	unsigned bit33,
	unsigned index,
	unsigned char *bytes)
{
	unsigned long long word;

	/* The base colour (bits 63, 55 and 47 down, each difference 0), the tables 0 and bit 33. */
	word = (unsigned long long)red << 59;
	word |= (unsigned long long)green << 51;
	word |= (unsigned long long)blue << 43;
	word |= (unsigned long long)bit33 << 33;

	/* The index's most significant bit of every texel (bits 31 to 16). */
	if ((index & 2U) != 0U)
		word |= 0xffffULL << 16;

	/* Its least significant bit of every texel (bits 15 to 0). */
	if ((index & 1U) != 0U)
		word |= 0xffffULL;

	/* The block's bytes, most significant first. */
	etc2_store(word, bytes);
}

/*
 * Writes an EAC block of table 0: a base codeword, a multiplier, and the
 * one 3-bit index of every texel (table 0's modifiers are -3, -6, -9,
 * -15, 2, 5, 8 and 14 by index).
 */
static void
etc2_eac_block(
	unsigned base,
	unsigned multiplier,
	unsigned index,
	unsigned char *bytes)
{
	unsigned long long word;
	unsigned texel;

	/* The base codeword, the multiplier and table 0. */
	word = (unsigned long long)base << 56;
	word |= (unsigned long long)multiplier << 52;

	/* Each texel's index, from bit 47 down. */
	for (texel = 0U; texel < 16U; texel++)
		word |= (unsigned long long)index << (45U - texel * 3U);

	/* The block's bytes, most significant first. */
	etc2_store(word, bytes);
}

/* Stores a block's 64 bits as 8 bytes, most significant first. */
static void
etc2_store(
	unsigned long long word,
	unsigned char *bytes)
{
	unsigned index;

	/* Each byte from the top. */
	for (index = 0U; index < ETC2_BLOCK_BYTES; index++)
		bytes[index] = (unsigned char)(word >> (56U - index * 8U));
}

/*
 * Makes each square's texture from its blocks: two blocks side by side
 * (8x4), the subimage square's second block replaced, and the array
 * square's two layers of one block (the second replaced).
 */
static void
etc2_textures_make(void)
{
	unsigned char data[4U * ETC2_BLOCK_BYTES];
	unsigned char one[2U * ETC2_BLOCK_BYTES];

	/* The names. */
	glGenTextures((GLsizei)ETC2_SQUARES, etc2_textures);

	/* RGB8: 16,8,24 index 0 (+2); 31,0,0 index 3 (-8). */
	etc2_colour_block(16U, 8U, 24U, 1U, 0U, data);
	etc2_colour_block(31U, 0U, 0U, 1U, 3U, data + ETC2_BLOCK_BYTES);
	etc2_texture_bind(GL_TEXTURE_2D, etc2_textures[0]);
	glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGB8_ETC2, 8, 4, 0, 16, data);

	/* SRGB8: 31,0,0 index 0 (+2); 16,16,16 index 2 (-2). */
	etc2_colour_block(31U, 0U, 0U, 1U, 0U, data);
	etc2_colour_block(16U, 16U, 16U, 1U, 2U, data + ETC2_BLOCK_BYTES);
	etc2_texture_bind(GL_TEXTURE_2D, etc2_textures[1]);
	glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_SRGB8_ETC2, 8, 4, 0, 16, data);

	/* RGB8 punch-through, not opaque: 8,31,4 index 0 (the base); index 2 (transparent). */
	etc2_colour_block(8U, 31U, 4U, 0U, 0U, data);
	etc2_colour_block(20U, 20U, 20U, 0U, 2U, data + ETC2_BLOCK_BYTES);
	etc2_texture_bind(GL_TEXTURE_2D, etc2_textures[2]);
	glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2, 8, 4, 0, 16, data);

	/* SRGB8 punch-through: opaque 0,0,31 index 1 (+8); not opaque, index 2 (transparent). */
	etc2_colour_block(0U, 0U, 31U, 1U, 1U, data);
	etc2_colour_block(5U, 5U, 5U, 0U, 2U, data + ETC2_BLOCK_BYTES);
	etc2_texture_bind(GL_TEXTURE_2D, etc2_textures[3]);
	glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2, 8, 4, 0, 16, data);

	/* RGBA8: alpha 126 (+2), colour 16,8,24 index 0; alpha 200 multiplier 2 index 0 (-3), colour 0,31,0 index 0. */
	etc2_eac_block(126U, 1U, 4U, data);
	etc2_colour_block(16U, 8U, 24U, 1U, 0U, data + ETC2_BLOCK_BYTES);
	etc2_eac_block(200U, 2U, 0U, data + 2U * ETC2_BLOCK_BYTES);
	etc2_colour_block(0U, 31U, 0U, 1U, 0U, data + 3U * ETC2_BLOCK_BYTES);
	etc2_texture_bind(GL_TEXTURE_2D, etc2_textures[4]);
	glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA8_ETC2_EAC, 8, 4, 0, 32, data);

	/* SRGB8_ALPHA8: alpha 255 index 7 (+14), colour 31,0,0 index 0; alpha 10 multiplier 3 index 3 (-15), colour 16,16,16 index 2. */
	etc2_eac_block(255U, 1U, 7U, data);
	etc2_colour_block(31U, 0U, 0U, 1U, 0U, data + ETC2_BLOCK_BYTES);
	etc2_eac_block(10U, 3U, 3U, data + 2U * ETC2_BLOCK_BYTES);
	etc2_colour_block(16U, 16U, 16U, 1U, 2U, data + 3U * ETC2_BLOCK_BYTES);
	etc2_texture_bind(GL_TEXTURE_2D, etc2_textures[5]);
	glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC, 8, 4, 0, 32, data);

	/* R11: 128 index 4 (+2); 0 multiplier 0 index 7 (+14 alone). */
	etc2_eac_block(128U, 1U, 4U, data);
	etc2_eac_block(0U, 0U, 7U, data + ETC2_BLOCK_BYTES);
	etc2_texture_bind(GL_TEXTURE_2D, etc2_textures[6]);
	glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_R11_EAC, 8, 4, 0, 16, data);

	/* Signed R11: 64 index 4 (+2); -128 index 0 (-3). */
	etc2_eac_block(64U, 1U, 4U, data);
	etc2_eac_block(0x80U, 1U, 0U, data + ETC2_BLOCK_BYTES);
	etc2_texture_bind(GL_TEXTURE_2D, etc2_textures[7]);
	glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_SIGNED_R11_EAC, 8, 4, 0, 16, data);

	/* RG11: red 255 multiplier 15 index 7, green 0 multiplier 15 index 3; red 128 index 4, green 64 multiplier 2 index 5. */
	etc2_eac_block(255U, 15U, 7U, data);
	etc2_eac_block(0U, 15U, 3U, data + ETC2_BLOCK_BYTES);
	etc2_eac_block(128U, 1U, 4U, data + 2U * ETC2_BLOCK_BYTES);
	etc2_eac_block(64U, 2U, 5U, data + 3U * ETC2_BLOCK_BYTES);
	etc2_texture_bind(GL_TEXTURE_2D, etc2_textures[8]);
	glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RG11_EAC, 8, 4, 0, 32, data);

	/* Signed RG11: red 64 index 4, green -64 index 4; red 127 multiplier 15 index 7, green 0 multiplier 0 index 0. */
	etc2_eac_block(64U, 1U, 4U, data);
	etc2_eac_block(0xc0U, 1U, 4U, data + ETC2_BLOCK_BYTES);
	etc2_eac_block(0x7fU, 15U, 7U, data + 2U * ETC2_BLOCK_BYTES);
	etc2_eac_block(0U, 0U, 0U, data + 3U * ETC2_BLOCK_BYTES);
	etc2_texture_bind(GL_TEXTURE_2D, etc2_textures[9]);
	glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_SIGNED_RG11_EAC, 8, 4, 0, 32, data);

	/* RGB8 of two 31,0,0 index 3 blocks, the second replaced by 0,31,0 index 0. */
	etc2_colour_block(31U, 0U, 0U, 1U, 3U, data);
	etc2_colour_block(31U, 0U, 0U, 1U, 3U, data + ETC2_BLOCK_BYTES);
	etc2_texture_bind(GL_TEXTURE_2D, etc2_textures[10]);
	glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGB8_ETC2, 8, 4, 0, 16, data);
	etc2_colour_block(0U, 31U, 0U, 1U, 0U, one);
	glCompressedTexSubImage2D(GL_TEXTURE_2D, 0, 4, 0, 4, 4, GL_COMPRESSED_RGB8_ETC2, 8, one);

	/* A 2D array of the RGBA8 square's first block in both layers, the second layer replaced by its second block. */
	etc2_eac_block(126U, 1U, 4U, data);
	etc2_colour_block(16U, 8U, 24U, 1U, 0U, data + ETC2_BLOCK_BYTES);
	memcpy(data + 2U * ETC2_BLOCK_BYTES, data, 2U * ETC2_BLOCK_BYTES);
	etc2_texture_bind(GL_TEXTURE_2D_ARRAY, etc2_textures[11]);
	glCompressedTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_COMPRESSED_RGBA8_ETC2_EAC, 4, 4, 2, 0, 32, data);
	etc2_eac_block(200U, 2U, 0U, one);
	etc2_colour_block(0U, 31U, 0U, 1U, 0U, one + ETC2_BLOCK_BYTES);
	glCompressedTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 1, 4, 4, 1, GL_COMPRESSED_RGBA8_ETC2_EAC, 16, one);

	/* Nothing left bound. */
	glBindTexture(GL_TEXTURE_2D_ARRAY, 0U);
	glBindTexture(GL_TEXTURE_2D, 0U);
}

/* Binds a texture to unit 0 and gives it nearest filters (one level: complete) and edge clamping. */
static void
etc2_texture_bind(
	GLenum target,
	GLuint texture)
{
	/* The texture and its sampling state. */
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(target, texture);
	glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/*
 * Checks the compressed formats the API lists and the errors of calls
 * that must fail.
 */
static void
etc2_checks(void)
{
	unsigned char block[2U * ETC2_BLOCK_BYTES];
	GLint formats[32];
	GLint count;
	GLint listed;
	GLint index;
	GLenum error;
	GLuint texture;

	/* The ten formats of OpenGL ES 3.0, the RGBA8 one among them. */
	count = 0;
	glGetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &count);
	etc2_expect("format-count", (long)count, 10L);
	listed = 0;
	if (count > 0 && count <= 32) {
		memset(formats, 0, sizeof(formats));
		glGetIntegerv(GL_COMPRESSED_TEXTURE_FORMATS, formats);
		for (index = 0; index < count; index++) {
			if (formats[index] == (GLint)GL_COMPRESSED_RGBA8_ETC2_EAC)
				listed = 1;
		}
	}

	/* The RGBA8 format was in the list. */
	etc2_expect("format-listed", (long)listed, 1L);

	/* A texture of one block to try the errors on. */
	memset(block, 0, sizeof(block));
	glGenTextures(1, &texture);
	etc2_texture_bind(GL_TEXTURE_2D, texture);
	glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGB8_ETC2, 8, 4, 0, 16, block);
	error = glGetError();
	etc2_expect("image-glerror", (long)error, (long)GL_NO_ERROR);

	/* An image size that is not the blocks' is refused. */
	glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGB8_ETC2, 8, 4, 0, 12, block);
	error = glGetError();
	etc2_expect("wrong-size", (long)error, (long)GL_INVALID_VALUE);

	/* An uncompressed internal format is not one. */
	glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, 16, block);
	error = glGetError();
	etc2_expect("not-compressed", (long)error, (long)GL_INVALID_ENUM);

	/* A rectangle off the blocks' corners is refused. */
	glCompressedTexSubImage2D(GL_TEXTURE_2D, 0, 2, 0, 4, 4, GL_COMPRESSED_RGB8_ETC2, 8, block);
	error = glGetError();
	etc2_expect("sub-misaligned", (long)error, (long)GL_INVALID_OPERATION);

	/* A rectangle of another format than the level's is refused. */
	glCompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 4, GL_COMPRESSED_R11_EAC, 8, block);
	error = glGetError();
	etc2_expect("sub-other-format", (long)error, (long)GL_INVALID_OPERATION);

	/* The texture let go. */
	glBindTexture(GL_TEXTURE_2D, 0U);
	glDeleteTextures(1, &texture);
}

/* Prints one of the start's checks, counting it when the value is not the one expected. */
static void
etc2_expect(
	const char *what,
	long got,
	long expected)
{
	const char *verdict;

	/* A value that differs is a failure. */
	verdict = "ok";
	if (got != expected) {
		verdict = "FAILED";
		etc2_failures++;
	}

	/* The line. */
	printf("EGLTEST ETC2 check %s got=%ld expected=%ld %s\n", what, got, expected, verdict);
}
