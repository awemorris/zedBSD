/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * egltest's OpenGL ES 3.0 blit and multisample scene (WS068 p029): the
 * start draws and blits into framebuffer objects, and each frame shows
 * what they hold as six squares in the first two rows.
 *
 *   top row       a red triangle drawn into a 4x multisampled
 *                 renderbuffer over blue and resolved by
 *                 glBlitFramebuffer (lower left red, upper right blue),
 *                 a texture of four quadrants (red, green, blue, yellow
 *                 from the lower left) blitted twice as large and mirrored
 *                 left to right (upper left yellow, lower right red), a
 *                 red square that a green one drawn after it does not
 *                 cover because the depth was blitted from another
 *                 framebuffer (ff0000), slice 1 of a 3D texture cleared
 *                 as a colour attachment (bf4080);
 *   middle row    the window's own framebuffer blitted into a texture at
 *                 the start, when its lower left corner was red and the
 *                 rest blue (lower left red, upper right blue), and the
 *                 quadrant texture blitted into the window itself each
 *                 frame (upper left blue, lower right green).
 *
 * The start also checks what the API reports: the samples of the
 * multisampled renderbuffer and framebuffer, the formats' sample counts,
 * the resolve's anti-aliased edge, reading a 3D slice attachment, and
 * the errors of wrong blits and reads.
 */

#include "blits.h"

#include <GLES3/gl3.h>

#include <stdio.h>
#include <string.h>

/* The programs, the textures, the framebuffer objects, the renderbuffers, and the points read back. */
#define BLITS_PROGRAMS		3U
#define BLITS_TEXTURES		6U
#define BLITS_FRAMEBUFFERS	8U
#define BLITS_RENDERBUFFERS	5U
#define BLITS_POINTS		10U

/* The programs: one colour, a 2D texture, a 3D texture. */
#define BLITS_SOLID		0U
#define BLITS_FLAT		1U
#define BLITS_VOLUME		2U

/* The textures, by what they hold. */
#define BLITS_T_RESOLVED	0U
#define BLITS_T_QUADRANTS	1U
#define BLITS_T_MIRRORED	2U
#define BLITS_T_DEPTH_TESTED	3U
#define BLITS_T_SLICES		4U
#define BLITS_T_WINDOW		5U

/* The framebuffer objects. */
#define BLITS_F_MULTISAMPLED	0U
#define BLITS_F_RESOLVED	1U
#define BLITS_F_QUADRANTS	2U
#define BLITS_F_MIRRORED	3U
#define BLITS_F_DEPTH_SOURCE	4U
#define BLITS_F_DEPTH_TESTED	5U
#define BLITS_F_SLICE		6U
#define BLITS_F_WINDOW		7U

/* The attribute location of the squares' corners. */
#define BLITS_POSITION		0U

/*
 * The programs and their uniforms, the textures, framebuffer objects and
 * renderbuffers, and the square's vertex array; made by
 * egltest_blits_start and kept for the run, with the number of the
 * start's checks that failed.
 */
static GLuint blits_programs[BLITS_PROGRAMS];
static GLint blits_rects[BLITS_PROGRAMS];
static GLint blits_depths[BLITS_PROGRAMS];
static GLint blits_colour;
static GLuint blits_textures[BLITS_TEXTURES];
static GLuint blits_framebuffers[BLITS_FRAMEBUFFERS];
static GLuint blits_renderbuffers[BLITS_RENDERBUFFERS];
static GLuint blits_array;
static int blits_failures;

/*
 * The vertex shader every program shares: a unit square placed by the
 * rectangle (centre, half size) at a depth, its texture coordinates from
 * 0 to 1.
 */
static const char blits_vertex_source[] =
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

/* The fragment shaders: one colour, a 2D texture, a 3D texture's middle slice. */
static const char *const blits_fragment_sources[BLITS_PROGRAMS] = {
	"#version 300 es\n"
	"precision mediump float;\n"
	"uniform vec4 u_colour;\n"
	"out vec4 o_colour;\n"
	"void main()\n"
	"{\n"
	"\to_colour = u_colour;\n"
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

	/* A 3D texture of three slices at slice 1. */
	"#version 300 es\n"
	"precision mediump float;\n"
	"uniform mediump sampler3D u_texture;\n"
	"in vec2 v_uv;\n"
	"out vec4 o_colour;\n"
	"void main()\n"
	"{\n"
	"\to_colour = vec4(texture(u_texture, vec3(v_uv, 0.5)).rgb, 1.0);\n"
	"}\n"
};

/*
 * The points read back (two in a square split in parts, one otherwise),
 * in the window's normalized coordinates, and the colours expected there
 * as 0xRRGGBB.
 */
static const char *const blits_names[BLITS_POINTS] = {
	"resolve-inside", "resolve-outside", "mirror-upper-left", "mirror-lower-right", "depth-blit",
	"slice-3d", "window-copy-lower-left", "window-copy-upper-right", "window-blit-upper-left", "window-blit-lower-right"
};
static const GLfloat blits_points[BLITS_POINTS][2] = {
	{ -0.85f, 0.535f }, { -0.65f, 0.785f }, { -0.35f, 0.785f }, { -0.15f, 0.535f }, { 0.25f, 0.66f },
	{ 0.75f, 0.66f }, { -0.85f, -0.125f }, { -0.65f, 0.125f }, { -0.35f, 0.125f }, { -0.15f, -0.125f }
};
static const unsigned blits_expected[BLITS_POINTS] = {
	0xff0000U, 0x0000ffU, 0xffff00U, 0xff0000U, 0xff0000U,
	0xbf4080U, 0xff0000U, 0x0000ffU, 0x0000ffU, 0x00ff00U
};

static GLuint blits_shader(GLenum type, const char *source);
static void blits_texture(unsigned texture, GLenum target, GLsizei width, GLsizei height, GLsizei depth);
static void blits_attach(unsigned framebuffer, unsigned texture, GLsizei width, GLsizei height);
static void blits_quad(unsigned program, GLfloat x, GLfloat y, GLfloat width, GLfloat height, GLfloat depth);
static void blits_colour_set(GLfloat red, GLfloat green, GLfloat blue);
static void blits_resolve(void);
static void blits_quadrants(void);
static void blits_depth(void);
static void blits_slice(void);
static void blits_window(void);
static void blits_expect(const char *what, long got, long expected);
static int blits_close(unsigned got, unsigned expected);

/*
 * Makes the programs and the square, draws and blits into the
 * framebuffer objects, and checks what the API reports.  Returns 0, or -1
 * with a line saying what failed.
 */
int
egltest_blits_start(void)
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
	for (program = 0U; program < BLITS_PROGRAMS; program++) {
		vertex = blits_shader(GL_VERTEX_SHADER, blits_vertex_source);
		fragment = blits_shader(GL_FRAGMENT_SHADER, blits_fragment_sources[program]);
		if (vertex == 0U || fragment == 0U)
			return -1;

		/* Linked. */
		blits_programs[program] = glCreateProgram();
		glAttachShader(blits_programs[program], vertex);
		glAttachShader(blits_programs[program], fragment);
		glLinkProgram(blits_programs[program]);
		glDeleteShader(vertex);
		glDeleteShader(fragment);
		linked = GL_FALSE;
		glGetProgramiv(blits_programs[program], GL_LINK_STATUS, &linked);
		if (!linked) {
			printf("EGLTEST BLITS link failed: program %u\n", program);
			return -1;
		}

		/* Its rectangle and depth, and its sampler (when it has one) on unit 0. */
		glUseProgram(blits_programs[program]);
		blits_rects[program] = glGetUniformLocation(blits_programs[program], "u_rect");
		blits_depths[program] = glGetUniformLocation(blits_programs[program], "u_depth");
		sampler = glGetUniformLocation(blits_programs[program], "u_texture");
		if (sampler >= 0)
			glUniform1i(sampler, 0);
	}

	/* The one-colour program's colour. */
	blits_colour = glGetUniformLocation(blits_programs[BLITS_SOLID], "u_colour");
	if (blits_colour < 0) {
		printf("EGLTEST BLITS uniforms missing\n");
		return -1;
	}

	/* The square's corners in a vertex array of their own. */
	glGenVertexArrays(1, &blits_array);
	glBindVertexArray(blits_array);
	glGenBuffers(1, &buffer);
	glBindBuffer(GL_ARRAY_BUFFER, buffer);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(corners), corners, GL_STATIC_DRAW);
	glVertexAttribPointer(BLITS_POSITION, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glEnableVertexAttribArray(BLITS_POSITION);

	/* The objects, each drawn and blitted into, with the checks of what the API reports. */
	glGenTextures((GLsizei)BLITS_TEXTURES, blits_textures);
	glGenFramebuffers((GLsizei)BLITS_FRAMEBUFFERS, blits_framebuffers);
	glGenRenderbuffers((GLsizei)BLITS_RENDERBUFFERS, blits_renderbuffers);
	blits_resolve();
	blits_quadrants();
	blits_depth();
	blits_slice();
	blits_window();

	/* The window's framebuffer again, for drawing and reading. */
	glBindFramebuffer(GL_FRAMEBUFFER, 0U);
	glBindVertexArray(0U);

	/* Everything went without an error the checks did not expect. */
	error = glGetError();
	blits_expect("start-glerror", (long)error, (long)GL_NO_ERROR);

	/* Succeeded: ready to draw. */
	printf("EGLTEST BLITS ready failures=%d\n", blits_failures);
	fflush(stdout);
	return 0;
}

/*
 * Draws the squares over a window of a size, and blits the quadrant
 * texture into the window where the sixth square is.
 */
void
egltest_blits_draw(
	int width,
	int height)
{
	static const unsigned textures[5] = {
		BLITS_T_RESOLVED, BLITS_T_MIRRORED, BLITS_T_DEPTH_TESTED, BLITS_T_SLICES, BLITS_T_WINDOW
	};
	unsigned index;
	unsigned program;
	GLenum target;
	GLfloat x;
	GLfloat y;
	GLint left;
	GLint right;
	GLint bottom;
	GLint top;

	/* The whole window, cleared to dark grey. */
	glBindFramebuffer(GL_FRAMEBUFFER, 0U);
	glViewport(0, 0, width, height);
	glClearColor(0.125f, 0.125f, 0.125f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glBindVertexArray(blits_array);
	glActiveTexture(GL_TEXTURE0);

	/* Each texture's square in its cell (the 3D one through its own program). */
	for (index = 0U; index < 5U; index++) {
		x = -0.75f + 0.5f * (GLfloat)(index % 4U);
		y = 0.66f - 0.66f * (GLfloat)(index / 4U);
		program = BLITS_FLAT;
		target = GL_TEXTURE_2D;
		if (textures[index] == BLITS_T_SLICES) {
			program = BLITS_VOLUME;
			target = GL_TEXTURE_3D;
		}

		/* The texture on unit 0, and the square. */
		glBindTexture(target, blits_textures[textures[index]]);
		blits_quad(program, x, y, 0.2f, 0.25f, 0.0f);
	}

	/* The quadrant texture blitted into the window where the sixth square is (x -0.25, y 0). */
	left = (GLint)((-0.45f + 1.0f) * 0.5f * (float)width);
	right = (GLint)((-0.05f + 1.0f) * 0.5f * (float)width);
	bottom = (GLint)((-0.25f + 1.0f) * 0.5f * (float)height);
	top = (GLint)((0.25f + 1.0f) * 0.5f * (float)height);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, blits_framebuffers[BLITS_F_QUADRANTS]);
	glBlitFramebuffer(0, 0, 4, 4, left, bottom, right, top, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0U);

	/* The vertex array is let go. */
	glBindVertexArray(0U);
}

/*
 * Reads back each point and prints the colours; returns how many differ,
 * with the start's failed checks and any error.
 */
int
egltest_blits_check(
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
	failures = blits_failures;
	for (point = 0U; point < BLITS_POINTS; point++) {
		x = (int)((blits_points[point][0] + 1.0f) * 0.5f * (float)width);
		y = (int)((blits_points[point][1] + 1.0f) * 0.5f * (float)height);
		memset(pixel, 0, sizeof(pixel));
		glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
		got = ((unsigned)pixel[0] << 16) | ((unsigned)pixel[1] << 8) | (unsigned)pixel[2];

		/* Each channel within 2 of the expected one. */
		same = blits_close(got, blits_expected[point]);
		verdict = "ok";
		if (!same) {
			verdict = "DIFFERS";
			failures++;
		}

		/* The point's line. */
		printf("EGLTEST PIXEL run=%s name=%s got=%06x expected=%06x %s\n", token, blits_names[point], got,
		       blits_expected[point], verdict);
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
blits_shader(
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
		printf("EGLTEST BLITS compile failed: %s\n", log);
		return 0U;
	}

	/* Succeeded: the shader. */
	return shader;
}

/* Gives a texture of a target RGBA8 storage of a size (a depth for a 3D one), with nearest filters and edge clamping. */
static void
blits_texture(
	unsigned texture,
	GLenum target,
	GLsizei width,
	GLsizei height,
	GLsizei depth)
{
	/* The texture and its sampling state. */
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(target, blits_textures[texture]);
	glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	/* Its storage. */
	if (target == GL_TEXTURE_3D) {
		glTexStorage3D(target, 1, GL_RGBA8, width, height, depth);
	} else {
		glTexStorage2D(target, 1, GL_RGBA8, width, height);
	}
}

/* Binds a framebuffer object for drawing and reading with a 2D texture as its colour, and a viewport of its size. */
static void
blits_attach(
	unsigned framebuffer,
	unsigned texture,
	GLsizei width,
	GLsizei height)
{
	/* The object, its colour and the viewport. */
	glBindFramebuffer(GL_FRAMEBUFFER, blits_framebuffers[framebuffer]);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, blits_textures[texture], 0);
	glViewport(0, 0, width, height);
}

/* Draws the square with a program, placed by a rectangle (centre, half size) at a depth. */
static void
blits_quad(
	unsigned program,
	GLfloat x,
	GLfloat y,
	GLfloat width,
	GLfloat height,
	GLfloat depth)
{
	/* The program's rectangle and depth, then the strip. */
	glUseProgram(blits_programs[program]);
	glUniform4f(blits_rects[program], x, y, width, height);
	if (blits_depths[program] >= 0)
		glUniform1f(blits_depths[program], depth);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* Sets the colour the one-colour program draws. */
static void
blits_colour_set(
	GLfloat red,
	GLfloat green,
	GLfloat blue)
{
	/* The uniform. */
	glUseProgram(blits_programs[BLITS_SOLID]);
	glUniform4f(blits_colour, red, green, blue, 1.0f);
}

/*
 * Draws a red triangle over blue into a 4x multisampled renderbuffer and
 * resolves it into a texture, checking the samples reported, the
 * anti-aliased edge, and the errors of reading a multisampled framebuffer
 * and resolving into another size.
 */
static void
blits_resolve(void)
{
	GLubyte edge[4];
	GLubyte pixel[4];
	GLint value;
	GLenum status;
	int mixed;

	/* The limits: at least 4 samples, and RGBA8 has some counts. */
	value = 0;
	glGetIntegerv(GL_MAX_SAMPLES, &value);
	blits_expect("max-samples-at-least-4", (long)(value >= 4), 1L);
	value = 0;
	glGetInternalformativ(GL_RENDERBUFFER, GL_RGBA8, GL_NUM_SAMPLE_COUNTS, 1, &value);
	blits_expect("rgba8-sample-counts", (long)(value >= 1), 1L);

	/* A 16x16 RGBA8 renderbuffer of 4 samples. */
	glBindRenderbuffer(GL_RENDERBUFFER, blits_renderbuffers[0]);
	glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_RGBA8, 16, 16);
	value = 0;
	glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_SAMPLES, &value);
	blits_expect("renderbuffer-samples", (long)(value >= 4), 1L);
	glBindFramebuffer(GL_FRAMEBUFFER, blits_framebuffers[BLITS_F_MULTISAMPLED]);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, blits_renderbuffers[0]);
	status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	blits_expect("multisampled-complete", (long)status, (long)GL_FRAMEBUFFER_COMPLETE);
	value = 0;
	glGetIntegerv(GL_SAMPLES, &value);
	blits_expect("framebuffer-samples", (long)(value >= 4), 1L);

	/* Blue, and the red triangle below the diagonal from the lower right to the upper left. */
	glViewport(0, 0, 16, 16);
	glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	blits_colour_set(1.0f, 0.0f, 0.0f);
	glUniform4f(blits_rects[BLITS_SOLID], 0.0f, 0.0f, 1.0f, 1.0f);
	glUniform1f(blits_depths[BLITS_SOLID], 0.0f);
	glDrawArrays(GL_TRIANGLES, 0, 3);

	/* A multisampled framebuffer is not read directly. */
	glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
	blits_expect("read-multisampled", (long)glGetError(), (long)GL_INVALID_OPERATION);

	/* Resolved into a 16x16 texture (not into another size). */
	blits_texture(BLITS_T_RESOLVED, GL_TEXTURE_2D, 16, 16, 0);
	blits_attach(BLITS_F_RESOLVED, BLITS_T_RESOLVED, 16, 16);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, blits_framebuffers[BLITS_F_MULTISAMPLED]);
	glBlitFramebuffer(0, 0, 16, 16, 0, 0, 8, 8, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	blits_expect("resolve-other-size", (long)glGetError(), (long)GL_INVALID_OPERATION);
	glBlitFramebuffer(0, 0, 16, 16, 0, 0, 16, 16, GL_COLOR_BUFFER_BIT, GL_NEAREST);

	/* A pixel on the diagonal is partly red, partly blue. */
	glBindFramebuffer(GL_FRAMEBUFFER, blits_framebuffers[BLITS_F_RESOLVED]);
	memset(edge, 0, sizeof(edge));
	glReadPixels(7, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, edge);
	mixed = 1;
	if (edge[0] <= 0x30U || edge[0] >= 0xd0U)
		mixed = 0;
	if (edge[2] <= 0x30U || edge[2] >= 0xd0U)
		mixed = 0;
	blits_expect("resolve-edge-mixed", (long)mixed, 1L);
}

/*
 * Makes a texture of four quadrants and blits it twice as large, mirrored
 * left to right, into another; a linear filter on depth is refused.
 */
static void
blits_quadrants(void)
{
	static const GLubyte texels[64] = {
		0xffU, 0x00U, 0x00U, 0xffU, 0xffU, 0x00U, 0x00U, 0xffU, 0x00U, 0xffU, 0x00U, 0xffU, 0x00U, 0xffU, 0x00U, 0xffU,
		0xffU, 0x00U, 0x00U, 0xffU, 0xffU, 0x00U, 0x00U, 0xffU, 0x00U, 0xffU, 0x00U, 0xffU, 0x00U, 0xffU, 0x00U, 0xffU,
		0x00U, 0x00U, 0xffU, 0xffU, 0x00U, 0x00U, 0xffU, 0xffU, 0xffU, 0xffU, 0x00U, 0xffU, 0xffU, 0xffU, 0x00U, 0xffU,
		0x00U, 0x00U, 0xffU, 0xffU, 0x00U, 0x00U, 0xffU, 0xffU, 0xffU, 0xffU, 0x00U, 0xffU, 0xffU, 0xffU, 0x00U, 0xffU
	};

	/* The 4x4 quadrants: red, green on the bottom rows, blue, yellow on the top ones. */
	blits_texture(BLITS_T_QUADRANTS, GL_TEXTURE_2D, 4, 4, 0);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, texels);
	blits_attach(BLITS_F_QUADRANTS, BLITS_T_QUADRANTS, 4, 4);

	/* Blitted 8x8 and mirrored into another texture. */
	blits_texture(BLITS_T_MIRRORED, GL_TEXTURE_2D, 8, 8, 0);
	blits_attach(BLITS_F_MIRRORED, BLITS_T_MIRRORED, 8, 8);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, blits_framebuffers[BLITS_F_QUADRANTS]);
	glBlitFramebuffer(4, 0, 0, 4, 0, 0, 8, 8, GL_COLOR_BUFFER_BIT, GL_NEAREST);

	/* Depth is not filtered. */
	glBlitFramebuffer(0, 0, 4, 4, 0, 0, 8, 8, GL_DEPTH_BUFFER_BIT, GL_LINEAR);
	blits_expect("linear-depth", (long)glGetError(), (long)GL_INVALID_OPERATION);
}

/*
 * Blits a depth of 0.25 from one framebuffer's depth renderbuffer into
 * another's, where a red square at that depth passes a less-or-equal
 * test and a green one drawn after it farther away fails: red stays.
 */
static void
blits_depth(void)
{
	GLenum status;

	/* The source: an RGBA8 texture and a DEPTH_COMPONENT24 renderbuffer, the depth cleared to 0.25. */
	blits_texture(BLITS_T_DEPTH_TESTED, GL_TEXTURE_2D, 8, 8, 0);
	glBindRenderbuffer(GL_RENDERBUFFER, blits_renderbuffers[1]);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, 8, 8);
	glBindRenderbuffer(GL_RENDERBUFFER, blits_renderbuffers[2]);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 8, 8);
	glBindFramebuffer(GL_FRAMEBUFFER, blits_framebuffers[BLITS_F_DEPTH_SOURCE]);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, blits_renderbuffers[2]);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, blits_renderbuffers[1]);
	glClearDepthf(0.25f);
	glClear(GL_DEPTH_BUFFER_BIT);
	glClearDepthf(1.0f);

	/* The destination: the texture and another DEPTH_COMPONENT24 renderbuffer, cleared black and far. */
	glBindRenderbuffer(GL_RENDERBUFFER, blits_renderbuffers[3]);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, 8, 8);
	blits_attach(BLITS_F_DEPTH_TESTED, BLITS_T_DEPTH_TESTED, 8, 8);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, blits_renderbuffers[3]);
	status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	blits_expect("depth-destination-complete", (long)status, (long)GL_FRAMEBUFFER_COMPLETE);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	/* The depth blitted in, then red at 0.25 (equal: passes with LEQUAL), green at 0.75 (behind: fails). */
	glBindFramebuffer(GL_READ_FRAMEBUFFER, blits_framebuffers[BLITS_F_DEPTH_SOURCE]);
	glBlitFramebuffer(0, 0, 8, 8, 0, 0, 8, 8, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, blits_framebuffers[BLITS_F_DEPTH_TESTED]);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);
	blits_colour_set(1.0f, 0.0f, 0.0f);
	blits_quad(BLITS_SOLID, 0.0f, 0.0f, 1.0f, 1.0f, -0.5f);
	blits_colour_set(0.0f, 1.0f, 0.0f);
	blits_quad(BLITS_SOLID, 0.0f, 0.0f, 1.0f, 1.0f, 0.5f);
	glDepthFunc(GL_LESS);
	glDisable(GL_DEPTH_TEST);
}

/*
 * Clears slice 1 of a 3D texture as a colour attachment, and reads it
 * back.
 */
static void
blits_slice(void)
{
	GLubyte pixel[4];
	GLenum status;

	/* A 4x4x3 3D texture, its slice 1 attached. */
	blits_texture(BLITS_T_SLICES, GL_TEXTURE_3D, 4, 4, 3);
	glBindFramebuffer(GL_FRAMEBUFFER, blits_framebuffers[BLITS_F_SLICE]);
	glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, blits_textures[BLITS_T_SLICES], 0, 1);
	status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	blits_expect("slice-complete", (long)status, (long)GL_FRAMEBUFFER_COMPLETE);

	/* Cleared to bf4080, and read back. */
	glViewport(0, 0, 4, 4);
	glClearColor(0.75f, 0.25f, 0.5f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	memset(pixel, 0, sizeof(pixel));
	glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
	blits_expect("slice-read", (long)(((unsigned)pixel[0] << 16) | ((unsigned)pixel[1] << 8) | pixel[2]), 0xbf4080L);
}

/*
 * Clears the window's framebuffer blue with its lower left 8x8 corner
 * red, and blits its lower left 16x16 into a texture.
 */
static void
blits_window(void)
{
	/* The window: blue, the corner red. */
	glBindFramebuffer(GL_FRAMEBUFFER, 0U);
	glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glEnable(GL_SCISSOR_TEST);
	glScissor(0, 0, 8, 8);
	glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glDisable(GL_SCISSOR_TEST);

	/* Its lower left 16x16 into a texture. */
	blits_texture(BLITS_T_WINDOW, GL_TEXTURE_2D, 16, 16, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, blits_framebuffers[BLITS_F_WINDOW]);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, blits_textures[BLITS_T_WINDOW], 0);
	glBlitFramebuffer(0, 0, 16, 16, 0, 0, 16, 16, GL_COLOR_BUFFER_BIT, GL_NEAREST);
}

/* Prints one of the start's checks, counting it when the value is not the one expected. */
static void
blits_expect(
	const char *what,
	long got,
	long expected)
{
	const char *verdict;

	/* A value that differs is a failure. */
	verdict = "ok";
	if (got != expected) {
		verdict = "FAILED";
		blits_failures++;
	}

	/* The line. */
	printf("EGLTEST BLITS check %s got=%ld expected=%ld %s\n", what, got, expected, verdict);
}

/* Reports whether two 0xRRGGBB colours are within 2 in every channel. */
static int
blits_close(
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
