/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * glxtest's OpenGL 3.1 scene (WS068 p031), in a 3.1 context made by
 * glXCreateContextAttribsARB: the start checks OpenGL 3.1's calls and
 * the shader-stage-free ones of 3.2 in a framebuffer object of 8x8
 * texels with a depth buffer, and each frame shows one square per
 * outcome, green when it is the expected one and red otherwise, and one
 * square drawn with the fixed function (GL_ARB_compatibility):
 *
 *   the version (GL_VERSION, GLSL 1.40, the extensions, GLSL 1.50
 *   refused), buffer textures (samplerBuffer of floats and integers, a
 *   change of the buffer seen), rectangle textures (sampler2DRect,
 *   texelFetch, textureSize, glGetTexImage, a framebuffer attachment),
 *   primitive restart at an index of its own, the base vertex of the
 *   three calls, the provoking vertex of flat inputs, depth clamping,
 *   and seamless cube maps with glGetActiveUniformName.
 *
 * The start's checks each print a line; a check whose value is not the
 * expected one fails the run.
 */

#include "gl31.h"

#include <GL/gl.h>

#include <stdio.h>
#include <string.h>

/* The outcomes shown, the side of the framebuffer object, and the attribute locations. */
#define GL31_OUTCOMES		8U
#define GL31_SIDE		8
#define GL31_POSITION		0U
#define GL31_COLOUR		1U

/* The index that restarts strips in the restart check (a vertex of its own that no strip should reach). */
#define GL31_RESTART		8U

/* The colours the checks read back, as 0xRRGGBB. */
#define GL31_BLACK		0x000000U
#define GL31_RED		0xff0000U
#define GL31_GREEN		0x00ff00U
#define GL31_BLUE		0x0000ffU
#define GL31_WHITE		0xffffffU

/*
 * The programs (one colour at a depth; the buffer textures; the rectangle
 * texture; the flat colour) with their uniforms, the vertex arrays (a
 * square, the restart check's nine vertices and their indices, the flat
 * check's two triangles), the framebuffer object with its texture and
 * depth buffer, and each outcome (nonzero: the expected one); kept for
 * the run with the number of the start's checks that failed.
 */
static GLuint gl31_plain;
static GLint gl31_plain_rect;
static GLint gl31_plain_depth;
static GLint gl31_plain_colour;
static GLuint gl31_buffers;
static GLint gl31_buffers_rect;
static GLuint gl31_rectangle;
static GLint gl31_rectangle_rect;
static GLuint gl31_flat;
static GLuint gl31_square;
static GLuint gl31_strips;
static GLuint gl31_triangles;
static GLuint gl31_framebuffer;
static GLuint gl31_texture;
static GLuint gl31_depth;
static int gl31_outcomes[GL31_OUTCOMES];
static int gl31_failures;

/* The vertex shader: a unit square placed by the rectangle (centre, half size) at a depth. */
static const char gl31_vertex_source[] =
	"#version 140\n"
	"in vec2 a_position;\n"
	"uniform vec4 u_rect;\n"
	"uniform float u_depth;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\tgl_Position = vec4(u_rect.xy + a_position * u_rect.zw, u_depth, 1.0);\n"
	"}\n";

/* The fragment shader of one colour. */
static const char gl31_plain_source[] =
	"#version 140\n"
	"uniform vec4 u_colour;\n"
	"out vec4 o_colour;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\to_colour = u_colour;\n"
	"}\n";

/* The fragment shader of the buffer textures: texel 1 of the colours when the sizes and the number are right, red otherwise. */
static const char gl31_buffers_source[] =
	"#version 140\n"
	"uniform samplerBuffer u_colours;\n"
	"uniform isamplerBuffer u_numbers;\n"
	"out vec4 o_colour;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\to_colour = vec4(1.0, 0.0, 0.0, 1.0);\n"
	"\tif (textureSize(u_colours) == 3 && textureSize(u_numbers) == 5 && texelFetch(u_numbers, 3).x == -7)\n"
	"\t\to_colour = texelFetch(u_colours, 1);\n"
	"}\n";

/* The fragment shader of the rectangle texture: green when a lookup, a fetch and the size find its texels, red otherwise. */
static const char gl31_rectangle_source[] =
	"#version 140\n"
	"uniform sampler2DRect u_rect_texture;\n"
	"out vec4 o_colour;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\tvec4 looked = texture(u_rect_texture, vec2(2.5, 1.5));\n"
	"\tvec4 fetched = texelFetch(u_rect_texture, ivec2(3, 0));\n"
	"\tivec2 size = textureSize(u_rect_texture);\n"
	"\to_colour = vec4(1.0, 0.0, 0.0, 1.0);\n"
	"\tif (size == ivec2(4, 2) && looked.b > 0.9 && looked.r < 0.1 && fetched.r > 0.9 && fetched.g < 0.1)\n"
	"\t\to_colour = vec4(0.0, 1.0, 0.0, 1.0);\n"
	"}\n";

/* The flat shaders: each vertex's colour, the provoking vertex's over the whole triangle. */
static const char gl31_flat_vertex_source[] =
	"#version 140\n"
	"in vec2 a_position;\n"
	"in vec4 a_colour;\n"
	"flat out vec4 v_colour;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\tv_colour = a_colour;\n"
	"\tgl_Position = vec4(a_position, 0.0, 1.0);\n"
	"}\n";
static const char gl31_flat_fragment_source[] =
	"#version 140\n"
	"flat in vec4 v_colour;\n"
	"out vec4 o_colour;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\to_colour = v_colour;\n"
	"}\n";

/* A shader of GLSL 1.50, which a 3.1 context refuses. */
static const char gl31_later_source[] =
	"#version 150\n"
	"out vec4 o_colour;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\to_colour = vec4(1.0);\n"
	"}\n";

/* The names of the outcomes. */
static const char *const gl31_names[GL31_OUTCOMES] = {
	"version", "texture-buffer", "texture-rectangle", "primitive-restart", "base-vertex", "provoking-vertex",
	"depth-clamp", "seamless-uniform-name"
};

static GLuint gl31_program(const char *vertex_source, const char *fragment_source);
static GLuint gl31_shader(GLenum type, const char *source, int quiet);
static void gl31_quad(GLfloat x, GLfloat y, GLfloat width, GLfloat height, GLfloat depth, unsigned rgb);
static unsigned gl31_read(int x, int y);
static void gl31_clear(unsigned rgb);
static void gl31_arrays(void);
static void gl31_version(void);
static void gl31_texture_buffer(void);
static void gl31_texture_rectangle(void);
static void gl31_restart(void);
static void gl31_base_vertex(void);
static void gl31_provoking(void);
static void gl31_depth_clamp(void);
static void gl31_seamless(void);
static int gl31_expect(const char *what, long got, long expected);

/*
 * Makes the programs, the vertex arrays and the framebuffer object, and
 * runs the checks.  Returns 0, or -1 with a line saying what failed.
 */
int
glxtest_gl31_start(void)
{
	GLenum complete;
	GLenum error;

	/* The programs and their uniforms. */
	gl31_plain = gl31_program(gl31_vertex_source, gl31_plain_source);
	gl31_buffers = gl31_program(gl31_vertex_source, gl31_buffers_source);
	gl31_rectangle = gl31_program(gl31_vertex_source, gl31_rectangle_source);
	gl31_flat = gl31_program(gl31_flat_vertex_source, gl31_flat_fragment_source);
	if (gl31_plain == 0U || gl31_buffers == 0U || gl31_rectangle == 0U || gl31_flat == 0U)
		return -1;
	gl31_plain_rect = glGetUniformLocation(gl31_plain, "u_rect");
	gl31_plain_depth = glGetUniformLocation(gl31_plain, "u_depth");
	gl31_plain_colour = glGetUniformLocation(gl31_plain, "u_colour");
	gl31_buffers_rect = glGetUniformLocation(gl31_buffers, "u_rect");
	gl31_rectangle_rect = glGetUniformLocation(gl31_rectangle, "u_rect");

	/* The vertex arrays. */
	gl31_arrays();

	/* The framebuffer object: an RGBA8 texture and a 24-bit depth buffer. */
	glGenTextures(1, &gl31_texture);
	glBindTexture(GL_TEXTURE_2D, gl31_texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GL31_SIDE, GL31_SIDE, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glGenRenderbuffers(1, &gl31_depth);
	glBindRenderbuffer(GL_RENDERBUFFER, gl31_depth);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, GL31_SIDE, GL31_SIDE);
	glGenFramebuffers(1, &gl31_framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, gl31_framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gl31_texture, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, gl31_depth);
	glViewport(0, 0, GL31_SIDE, GL31_SIDE);
	complete = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (complete != GL_FRAMEBUFFER_COMPLETE) {
		printf("GLXTEST GL31 framebuffer incomplete 0x%x\n", (unsigned)complete);
		return -1;
	}

	/* The checks, each setting its outcome. */
	gl31_version();
	gl31_texture_buffer();
	gl31_texture_rectangle();
	gl31_restart();
	gl31_base_vertex();
	gl31_provoking();
	gl31_depth_clamp();
	gl31_seamless();

	/* The window again, and no error the checks did not expect. */
	glBindFramebuffer(GL_FRAMEBUFFER, 0U);
	glBindVertexArray(0U);
	error = glGetError();
	(void)gl31_expect("start-glerror", (long)error, (long)GL_NO_ERROR);

	/* Succeeded: ready to draw. */
	printf("GLXTEST GL31 ready failures=%d\n", gl31_failures);
	fflush(stdout);
	return 0;
}

/*
 * Draws one square per outcome over a window of a size, green when it is
 * the expected one and red otherwise, and a green square with glBegin in
 * the cell after them.
 */
void
glxtest_gl31_draw(
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
	glBindVertexArray(gl31_square);

	/* Each outcome in a cell of a grid of four columns. */
	for (index = 0U; index < GL31_OUTCOMES; index++) {
		rgb = GL31_RED;
		if (gl31_outcomes[index])
			rgb = GL31_GREEN;
		x = -0.75f + 0.5f * (GLfloat)(index % 4U);
		y = 0.66f - 0.66f * (GLfloat)(index / 4U);
		gl31_quad(x, y, 0.2f, 0.25f, 0.0f, rgb);
	}

	/* The next cell with the fixed function, as GL_ARB_compatibility keeps it. */
	glBindVertexArray(0U);
	glUseProgram(0U);
	glColor3f(0.0f, 1.0f, 0.0f);
	glBegin(GL_QUADS);
	glVertex2f(-0.95f, -0.91f);
	glVertex2f(-0.55f, -0.91f);
	glVertex2f(-0.55f, -0.41f);
	glVertex2f(-0.95f, -0.41f);
	glEnd();
}

/*
 * Reads back each square's centre and prints the colours; returns how
 * many are not green, with the start's failed checks and any error.
 */
int
glxtest_gl31_check(
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
	failures = gl31_failures;
	for (index = 0U; index <= GL31_OUTCOMES; index++) {
		x = (int)((-0.75f + 0.5f * (float)(index % 4U) + 1.0f) * 0.5f * (float)width);
		y = (int)((0.66f - 0.66f * (float)(index / 4U) + 1.0f) * 0.5f * (float)height);
		memset(pixel, 0, sizeof(pixel));
		glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
		got = ((unsigned)pixel[0] << 16) | ((unsigned)pixel[1] << 8) | (unsigned)pixel[2];

		/* Green is the expected outcome. */
		verdict = "ok";
		if (got != GL31_GREEN) {
			verdict = "DIFFERS";
			failures++;
		}

		/* The square's line. */
		name = "fixed-function";
		if (index < GL31_OUTCOMES)
			name = gl31_names[index];
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

/* Makes a program of two GLSL sources, the position at location 0 and the colour at 1; 0 with a line on failure. */
static GLuint
gl31_program(
	const char *vertex_source,
	const char *fragment_source)
{
	GLuint program;
	GLuint vertex;
	GLuint fragment;
	GLint linked;
	char log[512];

	/* The two shaders. */
	vertex = gl31_shader(GL_VERTEX_SHADER, vertex_source, 0);
	if (vertex == 0U)
		return 0U;
	fragment = gl31_shader(GL_FRAGMENT_SHADER, fragment_source, 0);
	if (fragment == 0U)
		return 0U;

	/* The program. */
	program = glCreateProgram();
	glAttachShader(program, vertex);
	glAttachShader(program, fragment);
	glBindAttribLocation(program, GL31_POSITION, "a_position");
	glBindAttribLocation(program, GL31_COLOUR, "a_colour");
	glLinkProgram(program);
	glDeleteShader(vertex);
	glDeleteShader(fragment);
	linked = GL_FALSE;
	glGetProgramiv(program, GL_LINK_STATUS, &linked);
	if (!linked) {
		log[0] = '\0';
		glGetProgramInfoLog(program, (GLsizei)sizeof(log), NULL, log);
		printf("GLXTEST GL31 link failed: %s\n", log);
		return 0U;
	}

	/* Succeeded: the program. */
	return program;
}

/* Makes a shader from GLSL source; 0 (with a line unless quiet) when it does not compile. */
static GLuint
gl31_shader(
	GLenum type,
	const char *source,
	int quiet)
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
		if (!quiet)
			printf("GLXTEST GL31 compile failed: %s\n", log);
		glDeleteShader(shader);
		return 0U;
	}

	/* Succeeded: the shader. */
	return shader;
}

/* Draws the square placed by a rectangle (centre, half size) at a depth in a colour, with the plain program. */
static void
gl31_quad(
	GLfloat x,
	GLfloat y,
	GLfloat width,
	GLfloat height,
	GLfloat depth,
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
	glUseProgram(gl31_plain);
	glUniform4f(gl31_plain_rect, x, y, width, height);
	glUniform1f(gl31_plain_depth, depth);
	glUniform4f(gl31_plain_colour, red, green, blue, 1.0f);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* Returns a texel of the framebuffer object's colour texture as 0xRRGGBB. */
static unsigned
gl31_read(
	int x,
	int y)
{
	GLubyte pixel[4];
	unsigned rgb;

	/* The texel. */
	memset(pixel, 0, sizeof(pixel));
	glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
	rgb = ((unsigned)pixel[0] << 16) | ((unsigned)pixel[1] << 8) | (unsigned)pixel[2];

	/* Succeeded: the colour. */
	return rgb;
}

/* Clears the framebuffer object to a colour and its depth to the far plane. */
static void
gl31_clear(
	unsigned rgb)
{
	GLfloat red;
	GLfloat green;
	GLfloat blue;

	/* The colour's channels, cleared with, and the depth. */
	red = (GLfloat)((rgb >> 16) & 0xffU) / 255.0f;
	green = (GLfloat)((rgb >> 8) & 0xffU) / 255.0f;
	blue = (GLfloat)(rgb & 0xffU) / 255.0f;
	glClearColor(red, green, blue, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

/*
 * Makes the vertex arrays: the unit square; nine vertices of two strips
 * (a quad at the left, x -1 to -0.5, vertices 0 to 3, one at the right,
 * 0.5 to 1, vertices 4 to 7, and vertex 8 in the middle, which no strip
 * should reach) with their indices; and two triangles over the whole
 * target, each with a red first, a green middle and a blue last vertex.
 */
static void
gl31_arrays(void)
{
	static const GLfloat corners[8] = { -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };
	static const GLfloat strips[18] = {
		-1.0f, -1.0f, -1.0f, 1.0f, -0.5f, -1.0f, -0.5f, 1.0f,
		0.5f, -1.0f, 0.5f, 1.0f, 1.0f, -1.0f, 1.0f, 1.0f, 0.0f, 0.0f
	};
	static const GLushort indices[9] = { 0U, 1U, 2U, 3U, GL31_RESTART, 4U, 5U, 6U, 7U };
	static const GLfloat triangles[6 * 6] = {
		-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f,
		1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f
	};
	static const GLfloat colours[6 * 4] = {
		1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f,
		1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f
	};
	GLuint buffers[5];

	/* The buffers. */
	glGenBuffers(5, buffers);

	/* The square. */
	glGenVertexArrays(1, &gl31_square);
	glBindVertexArray(gl31_square);
	glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(corners), corners, GL_STATIC_DRAW);
	glVertexAttribPointer(GL31_POSITION, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glEnableVertexAttribArray(GL31_POSITION);

	/* The strips' vertices and indices. */
	glGenVertexArrays(1, &gl31_strips);
	glBindVertexArray(gl31_strips);
	glBindBuffer(GL_ARRAY_BUFFER, buffers[1]);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(strips), strips, GL_STATIC_DRAW);
	glVertexAttribPointer(GL31_POSITION, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glEnableVertexAttribArray(GL31_POSITION);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[2]);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)sizeof(indices), indices, GL_STATIC_DRAW);

	/* The triangles' positions and colours. */
	glGenVertexArrays(1, &gl31_triangles);
	glBindVertexArray(gl31_triangles);
	glBindBuffer(GL_ARRAY_BUFFER, buffers[3]);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(12U * sizeof(GLfloat)), triangles, GL_STATIC_DRAW);
	glVertexAttribPointer(GL31_POSITION, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glEnableVertexAttribArray(GL31_POSITION);
	glBindBuffer(GL_ARRAY_BUFFER, buffers[4]);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(colours), colours, GL_STATIC_DRAW);
	glVertexAttribPointer(GL31_COLOUR, 4, GL_FLOAT, GL_FALSE, 0, NULL);
	glEnableVertexAttribArray(GL31_COLOUR);
	glBindBuffer(GL_ARRAY_BUFFER, 0U);
	glBindVertexArray(gl31_square);
}

/* Checks the version the context reports (OpenGL 3.1, GLSL 1.40), its extensions, and that GLSL 1.50 is refused. */
static void
gl31_version(void)
{
	const char *version;
	const char *language;
	const char *extensions;
	const char *name;
	const char *found;
	GLuint shader;
	GLint major;
	GLint minor;
	GLint count;
	GLint index;
	int passed;
	int differs;
	int named;

	/* The strings. */
	version = (const char *)glGetString(GL_VERSION);
	language = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
	printf("GLXTEST GL31 version=\"%s\" glsl=\"%s\"\n", version, language);
	passed = 1;
	differs = strncmp(version, "3.1 ", 4U);
	passed &= gl31_expect("version-string", (long)differs, 0L);
	differs = strcmp(language, "1.40");
	passed &= gl31_expect("glsl-version", (long)differs, 0L);

	/* The numbers. */
	major = 0;
	minor = -1;
	glGetIntegerv(GL_MAJOR_VERSION, &major);
	glGetIntegerv(GL_MINOR_VERSION, &minor);
	passed &= gl31_expect("major-version", (long)major, 3L);
	passed &= gl31_expect("minor-version", (long)minor, 1L);

	/* The extensions: GL_ARB_compatibility one of those glGetStringi names, the buffer textures in the string. */
	count = 0;
	glGetIntegerv(GL_NUM_EXTENSIONS, &count);
	extensions = (const char *)glGetString(GL_EXTENSIONS);
	printf("GLXTEST GL31 extensions=%d \"%s\"\n", (int)count, extensions);
	differs = 1;
	for (index = 0; index < count && differs != 0; index++) {
		name = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)index);
		if (name != NULL)
			differs = strcmp(name, "GL_ARB_compatibility");
	}
	passed &= gl31_expect("extension-compatibility", (long)differs, 0L);
	found = strstr(extensions, "GL_ARB_texture_buffer_object");
	named = 0;
	if (found != NULL)
		named = 1;
	passed &= gl31_expect("extension-texture-buffer", (long)named, 1L);
	named = 0;
	if (count > 10)
		named = 1;
	passed &= gl31_expect("extension-count", (long)named, 1L);

	/* GLSL 1.50 is later than the context's. */
	shader = gl31_shader(GL_FRAGMENT_SHADER, gl31_later_source, 1);
	passed &= gl31_expect("glsl-150-refused", (long)shader, 0L);
	gl31_outcomes[0] = passed;
}

/* Checks buffer textures: floats and integers read from buffers, and a change of a buffer seen by the next draw. */
static void
gl31_texture_buffer(void)
{
	static const GLfloat colours[12] = { 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f };
	static const GLint numbers[5] = { 10, 20, 30, -7, 50 };
	static const GLfloat blue[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
	GLuint buffers[2];
	GLuint textures[2];
	GLint bound;
	int passed;

	/* The two buffers and their buffer textures, on units 0 and 1. */
	passed = 1;
	glGenBuffers(2, buffers);
	glBindBuffer(GL_TEXTURE_BUFFER, buffers[0]);
	glBufferData(GL_TEXTURE_BUFFER, (GLsizeiptr)sizeof(colours), colours, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_TEXTURE_BUFFER, buffers[1]);
	glBufferData(GL_TEXTURE_BUFFER, (GLsizeiptr)sizeof(numbers), numbers, GL_STATIC_DRAW);
	glGenTextures(2, textures);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_BUFFER, textures[0]);
	glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, buffers[0]);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_BUFFER, textures[1]);
	glTexBuffer(GL_TEXTURE_BUFFER, GL_R32I, buffers[1]);
	bound = 0;
	glGetIntegerv(GL_TEXTURE_BINDING_BUFFER, &bound);
	passed &= gl31_expect("buffer-binding", (long)bound, (long)textures[1]);
	glActiveTexture(GL_TEXTURE0);

	/* The program's samplers on those units: texel 1 is green. */
	glUseProgram(gl31_buffers);
	glUniform1i(glGetUniformLocation(gl31_buffers, "u_colours"), 0);
	glUniform1i(glGetUniformLocation(gl31_buffers, "u_numbers"), 1);
	glUniform4f(gl31_buffers_rect, 0.0f, 0.0f, 1.0f, 1.0f);
	gl31_clear(GL31_BLACK);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	passed &= gl31_expect("buffer-texel", (long)gl31_read(4, 4), (long)GL31_GREEN);

	/* Texel 1 made blue in the buffer: the next draw reads it. */
	glBindBuffer(GL_TEXTURE_BUFFER, buffers[0]);
	glBufferSubData(GL_TEXTURE_BUFFER, 16, 16, blue);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	passed &= gl31_expect("buffer-changed", (long)gl31_read(4, 4), (long)GL31_BLUE);

	/* A format a buffer texture cannot have is refused. */
	glTexBuffer(GL_TEXTURE_BUFFER, GL_RGB8, buffers[0]);
	passed &= gl31_expect("buffer-bad-format", (long)glGetError(), (long)GL_INVALID_ENUM);
	glBindBuffer(GL_TEXTURE_BUFFER, 0U);
	gl31_outcomes[1] = passed;
}

/*
 * Checks rectangle textures: a 4x2 texture read by a lookup in texels, a
 * fetch and its size, read back with glGetTexImage, and drawn into as a
 * framebuffer attachment.
 */
static void
gl31_texture_rectangle(void)
{
	GLubyte texels[4 * 2 * 4];
	GLubyte read[4 * 2 * 4];
	GLuint texture;
	GLuint framebuffer;
	GLenum complete;
	unsigned index;
	int passed;
	int differs;
	int same;

	/* Texels (x, y): red at (3, 0), blue at (2, 1), grey elsewhere; read with nearest filters. */
	passed = 1;
	for (index = 0U; index < 8U; index++) {
		texels[index * 4U] = 0x80U;
		texels[index * 4U + 1U] = 0x80U;
		texels[index * 4U + 2U] = 0x80U;
		texels[index * 4U + 3U] = 0xffU;
	}

	/* The red and the blue texel. */
	texels[3U * 4U] = 0xffU;
	texels[3U * 4U + 1U] = 0U;
	texels[3U * 4U + 2U] = 0U;
	texels[6U * 4U] = 0U;
	texels[6U * 4U + 1U] = 0U;
	texels[6U * 4U + 2U] = 0xffU;

	/* The texture of them. */
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_RECTANGLE, texture);
	glTexImage2D(GL_TEXTURE_RECTANGLE, 0, GL_RGBA8, 4, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
	glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	/* The program reads it on unit 0. */
	glUseProgram(gl31_rectangle);
	glUniform1i(glGetUniformLocation(gl31_rectangle, "u_rect_texture"), 0);
	glUniform4f(gl31_rectangle_rect, 0.0f, 0.0f, 1.0f, 1.0f);
	gl31_clear(GL31_BLACK);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	passed &= gl31_expect("rectangle-sampled", (long)gl31_read(4, 4), (long)GL31_GREEN);

	/* Its texels read back. */
	memset(read, 0, sizeof(read));
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glGetTexImage(GL_TEXTURE_RECTANGLE, 0, GL_RGBA, GL_UNSIGNED_BYTE, read);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	differs = memcmp(read, texels, sizeof(texels));
	same = 0;
	if (differs == 0)
		same = 1;
	passed &= gl31_expect("rectangle-get-tex-image", (long)same, 1L);

	/* Attached to a framebuffer and cleared white. */
	glGenFramebuffers(1, &framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, texture, 0);
	complete = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	passed &= gl31_expect("rectangle-attached", (long)complete, (long)GL_FRAMEBUFFER_COMPLETE);
	glViewport(0, 0, 4, 2);
	glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	passed &= gl31_expect("rectangle-drawn", (long)gl31_read(1, 1), (long)GL31_WHITE);

	/* Level 1 of a rectangle texture is refused. */
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, texture, 1);
	passed &= gl31_expect("rectangle-level-1", (long)glGetError(), (long)GL_INVALID_VALUE);

	/* Back to the test's framebuffer object. */
	glBindFramebuffer(GL_FRAMEBUFFER, gl31_framebuffer);
	glViewport(0, 0, GL31_SIDE, GL31_SIDE);
	glDeleteFramebuffers(1, &framebuffer);
	gl31_outcomes[2] = passed;
}

/* Checks primitive restart at an index of its own: the two quads are drawn, the middle between them is not. */
static void
gl31_restart(void)
{
	GLint index;
	GLboolean on;
	int passed;

	/* Restarts on at index 7. */
	passed = 1;
	glEnable(GL_PRIMITIVE_RESTART);
	glPrimitiveRestartIndex(GL31_RESTART);
	on = glIsEnabled(GL_PRIMITIVE_RESTART);
	passed &= gl31_expect("restart-enabled", (long)on, (long)GL_TRUE);
	index = 0;
	glGetIntegerv(GL_PRIMITIVE_RESTART_INDEX, &index);
	passed &= gl31_expect("restart-index", (long)index, (long)GL31_RESTART);

	/* The two strips in green. */
	gl31_clear(GL31_BLACK);
	glBindVertexArray(gl31_strips);
	glUseProgram(gl31_plain);
	glUniform4f(gl31_plain_rect, 0.0f, 0.0f, 1.0f, 1.0f);
	glUniform1f(gl31_plain_depth, 0.0f);
	glUniform4f(gl31_plain_colour, 0.0f, 1.0f, 0.0f, 1.0f);
	glDrawElements(GL_TRIANGLE_STRIP, 9, GL_UNSIGNED_SHORT, NULL);
	passed &= gl31_expect("restart-left", (long)gl31_read(0, 4), (long)GL31_GREEN);
	passed &= gl31_expect("restart-right", (long)gl31_read(7, 4), (long)GL31_GREEN);
	passed &= gl31_expect("restart-middle", (long)gl31_read(4, 4), (long)GL31_BLACK);

	/* Off again. */
	glDisable(GL_PRIMITIVE_RESTART);
	glBindVertexArray(gl31_square);
	gl31_outcomes[3] = passed;
}

/* Checks the base vertex of the three calls: the left quad's indices (0 to 3) with base 4 draw the right quad. */
static void
gl31_base_vertex(void)
{
	int passed;

	/* glDrawElementsBaseVertex. */
	passed = 1;
	glBindVertexArray(gl31_strips);
	glUseProgram(gl31_plain);
	glUniform4f(gl31_plain_rect, 0.0f, 0.0f, 1.0f, 1.0f);
	glUniform1f(gl31_plain_depth, 0.0f);
	glUniform4f(gl31_plain_colour, 0.0f, 1.0f, 0.0f, 1.0f);
	gl31_clear(GL31_BLACK);
	glDrawElementsBaseVertex(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_SHORT, NULL, 4);
	passed &= gl31_expect("base-vertex-right", (long)gl31_read(7, 4), (long)GL31_GREEN);
	passed &= gl31_expect("base-vertex-left", (long)gl31_read(0, 4), (long)GL31_BLACK);

	/* glDrawRangeElementsBaseVertex. */
	gl31_clear(GL31_BLACK);
	glDrawRangeElementsBaseVertex(GL_TRIANGLE_STRIP, 0U, 3U, 4, GL_UNSIGNED_SHORT, NULL, 4);
	passed &= gl31_expect("range-base-vertex", (long)gl31_read(7, 4), (long)GL31_GREEN);

	/* glDrawElementsInstancedBaseVertex, one instance. */
	gl31_clear(GL31_BLACK);
	glDrawElementsInstancedBaseVertex(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_SHORT, NULL, 1, 4);
	passed &= gl31_expect("instanced-base-vertex", (long)gl31_read(7, 4), (long)GL31_GREEN);
	passed &= gl31_expect("instanced-base-left", (long)gl31_read(0, 4), (long)GL31_BLACK);
	glBindVertexArray(gl31_square);
	gl31_outcomes[4] = passed;
}

/* Checks the provoking vertex of flat inputs: GL's last (blue) by default, the first (red) after glProvokingVertex. */
static void
gl31_provoking(void)
{
	GLint mode;
	int passed;

	/* The default: the last vertex. */
	passed = 1;
	mode = 0;
	glGetIntegerv(GL_PROVOKING_VERTEX, &mode);
	passed &= gl31_expect("provoking-default", (long)mode, (long)GL_LAST_VERTEX_CONVENTION);
	glBindVertexArray(gl31_triangles);
	glUseProgram(gl31_flat);
	gl31_clear(GL31_BLACK);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	passed &= gl31_expect("provoking-last", (long)gl31_read(2, 2), (long)GL31_BLUE);
	passed &= gl31_expect("provoking-last-2", (long)gl31_read(6, 6), (long)GL31_BLUE);

	/* The first. */
	glProvokingVertex(GL_FIRST_VERTEX_CONVENTION);
	gl31_clear(GL31_BLACK);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	passed &= gl31_expect("provoking-first", (long)gl31_read(2, 2), (long)GL31_RED);
	glProvokingVertex(GL_LAST_VERTEX_CONVENTION);
	glBindVertexArray(gl31_square);
	gl31_outcomes[5] = passed;
}

/*
 * Checks depth clamping: a square beyond the far plane is clipped away,
 * and drawn with GL_DEPTH_CLAMP on.  Skipped (green) without
 * GL_ARB_depth_clamp (a device that does not clamp).
 */
static void
gl31_depth_clamp(void)
{
	const char *extensions;
	const char *found;
	int passed;

	/* Without the extension, nothing to check. */
	extensions = (const char *)glGetString(GL_EXTENSIONS);
	found = NULL;
	if (extensions != NULL)
		found = strstr(extensions, "GL_ARB_depth_clamp");
	if (found == NULL) {
		printf("GLXTEST GL31 depth-clamp skipped (no GL_ARB_depth_clamp)\n");
		gl31_outcomes[6] = 1;
		return;
	}

	/* Beyond the far plane, clipped. */
	passed = 1;
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);
	gl31_clear(GL31_BLACK);
	gl31_quad(0.0f, 0.0f, 1.0f, 1.0f, 1.5f, GL31_GREEN);
	passed &= gl31_expect("depth-clipped", (long)gl31_read(4, 4), (long)GL31_BLACK);

	/* Clamped to it, drawn. */
	glEnable(GL_DEPTH_CLAMP);
	gl31_clear(GL31_BLACK);
	gl31_quad(0.0f, 0.0f, 1.0f, 1.0f, 1.5f, GL31_GREEN);
	passed &= gl31_expect("depth-clamped", (long)gl31_read(4, 4), (long)GL31_GREEN);
	glDisable(GL_DEPTH_CLAMP);
	glDisable(GL_DEPTH_TEST);
	gl31_outcomes[6] = passed;
}

/* Checks that seamless cube maps are taken, and glGetActiveUniformName names a uniform as glGetActiveUniform does. */
static void
gl31_seamless(void)
{
	char name[64];
	char other[64];
	GLsizei length;
	GLint size;
	GLenum type;
	GLboolean on;
	int passed;
	int differs;
	int named;

	/* Seamless cube maps on. */
	passed = 1;
	glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
	on = glIsEnabled(GL_TEXTURE_CUBE_MAP_SEAMLESS);
	passed &= gl31_expect("seamless", (long)on, (long)GL_TRUE);

	/* Uniform 0's name both ways. */
	memset(name, 0, sizeof(name));
	memset(other, 0, sizeof(other));
	length = 0;
	glGetActiveUniformName(gl31_plain, 0U, (GLsizei)sizeof(name), &length, name);
	glGetActiveUniform(gl31_plain, 0U, (GLsizei)sizeof(other), NULL, &size, &type, other);
	printf("GLXTEST GL31 uniform-name=\"%s\"\n", name);
	differs = strcmp(name, other);
	passed &= gl31_expect("uniform-name", (long)differs, 0L);
	named = 0;
	if (length > 0)
		named = 1;
	passed &= gl31_expect("uniform-name-length", (long)named, 1L);
	gl31_outcomes[7] = passed;
}

/* Prints one of the start's checks, counting it when the value is not the one expected; returns 1 when it is. */
static int
gl31_expect(
	const char *what,
	long got,
	long expected)
{
	/* A value that differs is a failure. */
	if (got != expected) {
		gl31_failures++;
		printf("GLXTEST GL31 check %s got=%ld expected=%ld FAILED\n", what, got, expected);
		return 0;
	}

	/* Succeeded: the line. */
	printf("GLXTEST GL31 check %s got=%ld expected=%ld ok\n", what, got, expected);
	return 1;
}
