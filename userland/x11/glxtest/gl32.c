/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * glxtest's OpenGL 3.2 scene (WS068 p033), in a 3.2 compatibility
 * context made by glXCreateContextAttribsARB: the start checks OpenGL
 * 3.2's calls in framebuffer objects of 8x8 texels, and each frame shows
 * one square per outcome, green when it is the expected one and red
 * otherwise, and one square drawn with the fixed function:
 *
 *   the version (GL_VERSION, GLSL 1.50, the profile, the extensions), a
 *   geometry shader making a square of a point (and its program's
 *   parameters), a draw of another primitive refused and one of lines
 *   with adjacency, a layered framebuffer object drawn into by gl_Layer,
 *   a multisample texture drawn into, read by sampler2DMS and resolved,
 *   the sample mask, and the level parameters.
 *
 * The start's checks each print a line; a check whose value is not the
 * expected one fails the run.
 */

#include "gl32.h"

#include <GL/gl.h>

#include <stdio.h>
#include <string.h>

/* The outcomes shown, the side of the framebuffer objects, and the attribute location of the corners. */
#define GL32_OUTCOMES		7U
#define GL32_SIDE		8
#define GL32_POSITION		0U

/* The colours the checks read back, as 0xRRGGBB. */
#define GL32_BLACK		0x000000U
#define GL32_RED		0xff0000U
#define GL32_GREEN		0x00ff00U
#define GL32_BLUE		0x0000ffU

/*
 * The programs (one colour; a point made a square; lines with adjacency
 * made a square; a triangle drawn into two layers; a multisample texture
 * read) with the uniforms the scene sets, the vertex arrays (a square, a
 * point, four vertices), a framebuffer object with its texture, and each
 * outcome (nonzero: the expected one); kept for the run with the number of
 * the start's checks that failed.
 */
static GLuint gl32_plain;
static GLint gl32_plain_rect;
static GLint gl32_plain_colour;
static GLuint gl32_points;
static GLint gl32_points_colour;
static GLuint gl32_adjacent;
static GLuint gl32_layers;
static GLuint gl32_samples;
static GLint gl32_samples_mode;
static GLuint gl32_square;
static GLuint gl32_point;
static GLuint gl32_four;
static GLuint gl32_framebuffer;
static GLuint gl32_texture;
static int gl32_outcomes[GL32_OUTCOMES];
static int gl32_failures;

/* The vertex shader of the squares: a unit square placed by the rectangle (centre, half size). */
static const char gl32_vertex_source[] =
	"#version 150\n"
	"in vec2 a_position;\n"
	"uniform vec4 u_rect;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\tgl_Position = vec4(u_rect.xy + a_position * u_rect.zw, 0.0, 1.0);\n"
	"}\n";

/* The fragment shader of one colour. */
static const char gl32_plain_source[] =
	"#version 150\n"
	"uniform vec4 u_colour;\n"
	"out vec4 o_colour;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\to_colour = u_colour;\n"
	"}\n";

/* The vertex shader that passes its position on. */
static const char gl32_pass_source[] =
	"#version 150\n"
	"in vec2 a_position;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\tgl_Position = vec4(a_position, 0.0, 1.0);\n"
	"}\n";

/* The geometry shader that makes a point a square over the target, of the uniform's colour. */
static const char gl32_points_source[] =
	"#version 150\n"
	"layout(points) in;\n"
	"layout(triangle_strip, max_vertices = 4) out;\n"
	"uniform vec4 u_colour;\n"
	"out vec4 g_colour;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\tvec4 centre = gl_in[0].gl_Position;\n"
	"\tg_colour = u_colour;\n"
	"\tgl_Position = centre + vec4(-1.0, -1.0, 0.0, 0.0);\n"
	"\tEmitVertex();\n"
	"\tgl_Position = centre + vec4(1.0, -1.0, 0.0, 0.0);\n"
	"\tEmitVertex();\n"
	"\tgl_Position = centre + vec4(-1.0, 1.0, 0.0, 0.0);\n"
	"\tEmitVertex();\n"
	"\tgl_Position = centre + vec4(1.0, 1.0, 0.0, 0.0);\n"
	"\tEmitVertex();\n"
	"\tEndPrimitive();\n"
	"}\n";

/* The geometry shader of lines with adjacency: a blue square over the target when it has the four vertices. */
static const char gl32_adjacent_source[] =
	"#version 150\n"
	"layout(lines_adjacency) in;\n"
	"layout(triangle_strip, max_vertices = 4) out;\n"
	"out vec4 g_colour;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\tg_colour = vec4(1.0, 0.0, 0.0, 1.0);\n"
	"\tif (gl_in.length() == 4 && gl_in[3].gl_Position.x > 0.5)\n"
	"\t\tg_colour = vec4(0.0, 0.0, 1.0, 1.0);\n"
	"\tgl_Position = vec4(-1.0, -1.0, 0.0, 1.0);\n"
	"\tEmitVertex();\n"
	"\tgl_Position = vec4(1.0, -1.0, 0.0, 1.0);\n"
	"\tEmitVertex();\n"
	"\tgl_Position = vec4(-1.0, 1.0, 0.0, 1.0);\n"
	"\tEmitVertex();\n"
	"\tgl_Position = vec4(1.0, 1.0, 0.0, 1.0);\n"
	"\tEmitVertex();\n"
	"\tEndPrimitive();\n"
	"}\n";

/* The geometry shader that draws each triangle into layer 0 in green and layer 1 in red. */
static const char gl32_layers_source[] =
	"#version 150\n"
	"layout(triangles) in;\n"
	"layout(triangle_strip, max_vertices = 6) out;\n"
	"out vec4 g_colour;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\tint layer;\n"
	"\tint i;\n"
	"\tfor (layer = 0; layer < 2; layer++) {\n"
	"\t\tfor (i = 0; i < 3; i++) {\n"
	"\t\t\tgl_Layer = layer;\n"
	"\t\t\tg_colour = vec4(float(layer), float(1 - layer), 0.0, 1.0);\n"
	"\t\t\tgl_Position = gl_in[i].gl_Position;\n"
	"\t\t\tEmitVertex();\n"
	"\t\t}\n"
	"\t\tEndPrimitive();\n"
	"\t}\n"
	"}\n";

/* The fragment shader of the geometry shaders' colour. */
static const char gl32_colour_source[] =
	"#version 150\n"
	"in vec4 g_colour;\n"
	"out vec4 o_colour;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\to_colour = g_colour;\n"
	"}\n";

/*
 * The fragment shader that reads a multisample texture: mode 0 the mean
 * of its four samples, mode 1 green when sample 0 is red and sample 1
 * black (red otherwise).
 */
static const char gl32_samples_source[] =
	"#version 150\n"
	"uniform sampler2DMS u_texture;\n"
	"uniform int u_mode;\n"
	"out vec4 o_colour;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\tivec2 texel = ivec2(gl_FragCoord.xy) % textureSize(u_texture);\n"
	"\tvec4 first = texelFetch(u_texture, texel, 0);\n"
	"\tvec4 second = texelFetch(u_texture, texel, 1);\n"
	"\to_colour = (first + second + texelFetch(u_texture, texel, 2) + texelFetch(u_texture, texel, 3)) * 0.25;\n"
	"\tif (u_mode == 1) {\n"
	"\t\to_colour = vec4(1.0, 0.0, 0.0, 1.0);\n"
	"\t\tif (first.r > 0.9 && second.r < 0.1)\n"
	"\t\t\to_colour = vec4(0.0, 1.0, 0.0, 1.0);\n"
	"\t}\n"
	"}\n";

/* The names of the outcomes. */
static const char *const gl32_names[GL32_OUTCOMES] = {
	"version", "geometry-points", "geometry-modes", "layered", "multisample-texture", "sample-mask", "level-parameters"
};

static GLuint gl32_program(const char *vertex_source, const char *geometry_source, const char *fragment_source);
static GLuint gl32_shader(GLenum type, const char *source);
static void gl32_quad(GLfloat x, GLfloat y, GLfloat width, GLfloat height, unsigned rgb);
static unsigned gl32_read(int x, int y);
static void gl32_clear(unsigned rgb);
static void gl32_arrays(void);
static void gl32_version(void);
static void gl32_geometry_points(void);
static void gl32_geometry_modes(void);
static void gl32_layered(void);
static void gl32_multisample(void);
static void gl32_sample_mask(void);
static void gl32_level_parameters(void);
static int gl32_expect(const char *what, long got, long expected);

/*
 * Makes the programs, the vertex arrays and the framebuffer object, and
 * runs the checks.  Returns 0, or -1 with a line saying what failed.
 */
int
glxtest_gl32_start(void)
{
	GLenum complete;
	GLenum error;
	GLint rect;

	/* The programs and the uniforms the scene sets. */
	gl32_plain = gl32_program(gl32_vertex_source, NULL, gl32_plain_source);
	gl32_points = gl32_program(gl32_pass_source, gl32_points_source, gl32_colour_source);
	gl32_adjacent = gl32_program(gl32_pass_source, gl32_adjacent_source, gl32_colour_source);
	gl32_layers = gl32_program(gl32_pass_source, gl32_layers_source, gl32_colour_source);
	gl32_samples = gl32_program(gl32_vertex_source, NULL, gl32_samples_source);
	if (gl32_plain == 0U || gl32_points == 0U || gl32_adjacent == 0U || gl32_layers == 0U || gl32_samples == 0U)
		return -1;
	gl32_plain_rect = glGetUniformLocation(gl32_plain, "u_rect");
	gl32_plain_colour = glGetUniformLocation(gl32_plain, "u_colour");
	gl32_points_colour = glGetUniformLocation(gl32_points, "u_colour");
	gl32_samples_mode = glGetUniformLocation(gl32_samples, "u_mode");

	/* The multisample reader's square covers the whole target. */
	rect = glGetUniformLocation(gl32_samples, "u_rect");
	glUseProgram(gl32_samples);
	glUniform4f(rect, 0.0f, 0.0f, 1.0f, 1.0f);

	/* The vertex arrays. */
	gl32_arrays();

	/* The framebuffer object of an RGBA8 texture. */
	glGenTextures(1, &gl32_texture);
	glBindTexture(GL_TEXTURE_2D, gl32_texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GL32_SIDE, GL32_SIDE, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glGenFramebuffers(1, &gl32_framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, gl32_framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gl32_texture, 0);
	glViewport(0, 0, GL32_SIDE, GL32_SIDE);
	complete = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (complete != GL_FRAMEBUFFER_COMPLETE) {
		printf("GLXTEST GL32 framebuffer incomplete 0x%x\n", (unsigned)complete);
		return -1;
	}

	/* The checks, each setting its outcome. */
	gl32_version();
	gl32_geometry_points();
	gl32_geometry_modes();
	gl32_layered();
	gl32_multisample();
	gl32_sample_mask();
	gl32_level_parameters();

	/* The window again, and no error the checks did not expect. */
	glBindFramebuffer(GL_FRAMEBUFFER, 0U);
	glBindVertexArray(0U);
	error = glGetError();
	(void)gl32_expect("start-glerror", (long)error, (long)GL_NO_ERROR);

	/* Succeeded: ready to draw. */
	printf("GLXTEST GL32 ready failures=%d\n", gl32_failures);
	fflush(stdout);
	return 0;
}

/*
 * Draws one square per outcome over a window of a size, green when it is
 * the expected one and red otherwise, and a green square with glBegin in
 * the cell after them.
 */
void
glxtest_gl32_draw(
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
	glBindVertexArray(gl32_square);

	/* Each outcome in a cell of a grid of four columns. */
	for (index = 0U; index < GL32_OUTCOMES; index++) {
		rgb = GL32_RED;
		if (gl32_outcomes[index])
			rgb = GL32_GREEN;
		x = -0.75f + 0.5f * (GLfloat)(index % 4U);
		y = 0.66f - 0.66f * (GLfloat)(index / 4U);
		gl32_quad(x, y, 0.2f, 0.25f, rgb);
	}

	/* The next cell (the last of the second row) with the fixed function, as the compatibility profile keeps it. */
	glBindVertexArray(0U);
	glUseProgram(0U);
	glColor3f(0.0f, 1.0f, 0.0f);
	glBegin(GL_QUADS);
	glVertex2f(0.55f, -0.25f);
	glVertex2f(0.95f, -0.25f);
	glVertex2f(0.95f, 0.25f);
	glVertex2f(0.55f, 0.25f);
	glEnd();
}

/*
 * Reads back each square's centre and prints the colours; returns how
 * many are not green, with the start's failed checks and any error.
 */
int
glxtest_gl32_check(
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
	failures = gl32_failures;
	for (index = 0U; index <= GL32_OUTCOMES; index++) {
		x = (int)((-0.75f + 0.5f * (float)(index % 4U) + 1.0f) * 0.5f * (float)width);
		y = (int)((0.66f - 0.66f * (float)(index / 4U) + 1.0f) * 0.5f * (float)height);
		memset(pixel, 0, sizeof(pixel));
		glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
		got = ((unsigned)pixel[0] << 16) | ((unsigned)pixel[1] << 8) | (unsigned)pixel[2];

		/* Green is the expected outcome. */
		verdict = "ok";
		if (got != GL32_GREEN) {
			verdict = "DIFFERS";
			failures++;
		}

		/* The square's line. */
		name = "fixed-function";
		if (index < GL32_OUTCOMES)
			name = gl32_names[index];
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

/* Makes a program of GLSL sources (a geometry shader when its source is not NULL), the corners at location 0; 0 with a line on failure. */
static GLuint
gl32_program(
	const char *vertex_source,
	const char *geometry_source,
	const char *fragment_source)
{
	GLuint program;
	GLuint vertex;
	GLuint geometry;
	GLuint fragment;
	GLint linked;
	char log[512];

	/* The vertex and the fragment shader. */
	vertex = gl32_shader(GL_VERTEX_SHADER, vertex_source);
	if (vertex == 0U)
		return 0U;
	fragment = gl32_shader(GL_FRAGMENT_SHADER, fragment_source);
	if (fragment == 0U)
		return 0U;

	/* The geometry shader, when there is one. */
	geometry = 0U;
	if (geometry_source != NULL) {
		geometry = gl32_shader(GL_GEOMETRY_SHADER, geometry_source);
		if (geometry == 0U)
			return 0U;
	}

	/* The program. */
	program = glCreateProgram();
	glAttachShader(program, vertex);
	glAttachShader(program, fragment);
	if (geometry != 0U)
		glAttachShader(program, geometry);
	glBindAttribLocation(program, GL32_POSITION, "a_position");
	glLinkProgram(program);
	glDeleteShader(vertex);
	glDeleteShader(fragment);
	if (geometry != 0U)
		glDeleteShader(geometry);

	/* Linked. */
	linked = GL_FALSE;
	glGetProgramiv(program, GL_LINK_STATUS, &linked);
	if (!linked) {
		log[0] = '\0';
		glGetProgramInfoLog(program, (GLsizei)sizeof(log), NULL, log);
		printf("GLXTEST GL32 link failed: %s\n", log);
		return 0U;
	}

	/* Succeeded: the program. */
	return program;
}

/* Makes a shader from GLSL source; 0 with a line when it does not compile. */
static GLuint
gl32_shader(
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
		printf("GLXTEST GL32 compile failed: %s\n", log);
		return 0U;
	}

	/* Succeeded: the shader. */
	return shader;
}

/* Draws the square placed by a rectangle (centre, half size) in a colour, with the plain program. */
static void
gl32_quad(
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
	glUseProgram(gl32_plain);
	glUniform4f(gl32_plain_rect, x, y, width, height);
	glUniform4f(gl32_plain_colour, red, green, blue, 1.0f);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* Returns a texel of the read framebuffer's read buffer as 0xRRGGBB. */
static unsigned
gl32_read(
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

/* Clears the draw framebuffer to a colour. */
static void
gl32_clear(
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

/* Makes the vertex arrays: the unit square, one point at the centre, and four vertices of lines with adjacency (the last at the right). */
static void
gl32_arrays(void)
{
	static const GLfloat corners[8] = { -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };
	static const GLfloat centre[2] = { 0.0f, 0.0f };
	static const GLfloat four[8] = { -1.0f, 0.0f, -0.5f, 0.0f, 0.5f, 0.0f, 1.0f, 0.0f };
	GLuint buffers[3];

	/* The buffers. */
	glGenBuffers(3, buffers);

	/* The square. */
	glGenVertexArrays(1, &gl32_square);
	glBindVertexArray(gl32_square);
	glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(corners), corners, GL_STATIC_DRAW);
	glVertexAttribPointer(GL32_POSITION, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glEnableVertexAttribArray(GL32_POSITION);

	/* The point. */
	glGenVertexArrays(1, &gl32_point);
	glBindVertexArray(gl32_point);
	glBindBuffer(GL_ARRAY_BUFFER, buffers[1]);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(centre), centre, GL_STATIC_DRAW);
	glVertexAttribPointer(GL32_POSITION, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glEnableVertexAttribArray(GL32_POSITION);

	/* The four vertices. */
	glGenVertexArrays(1, &gl32_four);
	glBindVertexArray(gl32_four);
	glBindBuffer(GL_ARRAY_BUFFER, buffers[2]);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(four), four, GL_STATIC_DRAW);
	glVertexAttribPointer(GL32_POSITION, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glEnableVertexAttribArray(GL32_POSITION);
	glBindBuffer(GL_ARRAY_BUFFER, 0U);
	glBindVertexArray(gl32_square);
}

/* Checks the version the context reports (OpenGL 3.2, GLSL 1.50, the compatibility profile) and its extensions. */
static void
gl32_version(void)
{
	const char *version;
	const char *language;
	const char *extensions;
	const char *found;
	GLint major;
	GLint minor;
	GLint profile;
	GLint samples;
	long multisample;
	int passed;
	int differs;
	int named;

	/* The strings. */
	version = (const char *)glGetString(GL_VERSION);
	language = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
	extensions = (const char *)glGetString(GL_EXTENSIONS);
	printf("GLXTEST GL32 version=\"%s\" glsl=\"%s\" extensions=\"%s\"\n", version, language, extensions);
	passed = 1;
	differs = strncmp(version, "3.2 ", 4U);
	passed &= gl32_expect("version-string", (long)differs, 0L);
	differs = strcmp(language, "1.50");
	passed &= gl32_expect("glsl-version", (long)differs, 0L);

	/* The numbers and the profile. */
	major = 0;
	minor = -1;
	profile = 0;
	glGetIntegerv(GL_MAJOR_VERSION, &major);
	glGetIntegerv(GL_MINOR_VERSION, &minor);
	glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile);
	passed &= gl32_expect("major-version", (long)major, 3L);
	passed &= gl32_expect("minor-version", (long)minor, 2L);
	passed &= gl32_expect("profile", (long)profile, (long)GL_CONTEXT_COMPATIBILITY_PROFILE_BIT);

	/* The multisample textures (on a device of more than one sample a pixel) and the compatibility among the extensions. */
	found = strstr(extensions, "GL_ARB_texture_multisample");
	named = 0;
	if (found != NULL)
		named = 1;
	samples = 0;
	glGetIntegerv(GL_MAX_SAMPLES, &samples);
	multisample = 0L;
	if (samples > 1)
		multisample = 1L;
	passed &= gl32_expect("extension-multisample", (long)named, multisample);
	found = strstr(extensions, "GL_ARB_compatibility");
	named = 0;
	if (found != NULL)
		named = 1;
	passed &= gl32_expect("extension-compatibility", (long)named, 1L);
	gl32_outcomes[0] = passed;
}

/* Checks a geometry shader making a point a square over the target, and its program's parameters. */
static void
gl32_geometry_points(void)
{
	GLint value;
	GLint attached;
	int passed;

	/* The program's geometry parameters. */
	passed = 1;
	value = 0;
	glGetProgramiv(gl32_points, GL_GEOMETRY_VERTICES_OUT, &value);
	passed &= gl32_expect("geometry-vertices-out", (long)value, 4L);
	glGetProgramiv(gl32_points, GL_GEOMETRY_INPUT_TYPE, &value);
	passed &= gl32_expect("geometry-input-type", (long)value, (long)GL_POINTS);
	glGetProgramiv(gl32_points, GL_GEOMETRY_OUTPUT_TYPE, &value);
	passed &= gl32_expect("geometry-output-type", (long)value, (long)GL_TRIANGLE_STRIP);
	attached = 0;
	glGetProgramiv(gl32_points, GL_ATTACHED_SHADERS, &attached);
	passed &= gl32_expect("attached-shaders", (long)attached, 3L);

	/* One point: the target green. */
	gl32_clear(GL32_BLACK);
	glBindVertexArray(gl32_point);
	glUseProgram(gl32_points);
	glUniform4f(gl32_points_colour, 0.0f, 1.0f, 0.0f, 1.0f);
	glDrawArrays(GL_POINTS, 0, 1);
	passed &= gl32_expect("geometry-corner", (long)gl32_read(0, 0), (long)GL32_GREEN);
	passed &= gl32_expect("geometry-centre", (long)gl32_read(4, 4), (long)GL32_GREEN);
	passed &= gl32_expect("geometry-far-corner", (long)gl32_read(7, 7), (long)GL32_GREEN);
	glBindVertexArray(gl32_square);
	gl32_outcomes[1] = passed;
}

/* Checks the draw modes a geometry shader takes: triangles refused by a points shader, lines with adjacency drawn by an adjacency one. */
static void
gl32_geometry_modes(void)
{
	GLenum error;
	int passed;

	/* Triangles into a shader of points. */
	passed = 1;
	glBindVertexArray(gl32_square);
	glUseProgram(gl32_points);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	error = glGetError();
	passed &= gl32_expect("mode-refused", (long)error, (long)GL_INVALID_OPERATION);

	/* Lines with adjacency: blue when the shader sees the four vertices. */
	gl32_clear(GL32_BLACK);
	glBindVertexArray(gl32_four);
	glUseProgram(gl32_adjacent);
	glDrawArrays(GL_LINES_ADJACENCY, 0, 4);
	passed &= gl32_expect("lines-adjacency", (long)gl32_read(4, 4), (long)GL32_BLUE);
	glBindVertexArray(gl32_square);
	gl32_outcomes[2] = passed;
}

/*
 * Checks a layered framebuffer object of a 2D array texture of two
 * layers: a clear reaches both, gl_Layer picks the layer a triangle goes
 * to, and a layered attachment with one that is not is incomplete.
 */
static void
gl32_layered(void)
{
	GLuint texture;
	GLuint framebuffer;
	GLuint reader;
	GLenum complete;
	int passed;

	/* The texture of two layers, layered into a framebuffer object. */
	passed = 1;
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D_ARRAY, texture);
	glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, GL32_SIDE, GL32_SIDE, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, texture, 0);
	complete = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	passed &= gl32_expect("layered-complete", (long)complete, (long)GL_FRAMEBUFFER_COMPLETE);

	/* A clear of both layers, then the square: green into layer 0, red into layer 1. */
	glViewport(0, 0, GL32_SIDE, GL32_SIDE);
	gl32_clear(GL32_BLUE);
	glBindVertexArray(gl32_square);
	glUseProgram(gl32_layers);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	/* Each layer read through a framebuffer of its own. */
	glGenFramebuffers(1, &reader);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, reader);
	glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, texture, 0, 0);
	passed &= gl32_expect("layer-0", (long)gl32_read(4, 4), (long)GL32_GREEN);
	glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, texture, 0, 1);
	passed &= gl32_expect("layer-1", (long)gl32_read(4, 4), (long)GL32_RED);

	/* A second attachment that is not layered makes it incomplete. */
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, gl32_texture, 0);
	complete = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	passed &= gl32_expect("layered-mixed", (long)complete, (long)GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS);

	/* Back to the test's framebuffer object. */
	glBindFramebuffer(GL_FRAMEBUFFER, gl32_framebuffer);
	glDeleteFramebuffers(1, &framebuffer);
	glDeleteFramebuffers(1, &reader);
	glDeleteTextures(1, &texture);
	gl32_outcomes[3] = passed;
}

/*
 * Checks a multisample texture of four samples: cleared green through a
 * framebuffer object, the mean of its samples read by sampler2DMS, and
 * resolved into a texture by a blit.
 */
static void
gl32_multisample(void)
{
	GLuint texture;
	GLuint framebuffer;
	GLint samples;
	GLint bound;
	GLenum complete;
	int passed;
	int enough;

	/* The texture, and what it reports. */
	passed = 1;
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, texture);
	glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, 4, GL_RGBA8, GL32_SIDE, GL32_SIDE, GL_TRUE);
	bound = 0;
	glGetIntegerv(GL_TEXTURE_BINDING_2D_MULTISAMPLE, &bound);
	passed &= gl32_expect("multisample-binding", (long)bound, (long)texture);
	samples = 0;
	glGetTexLevelParameteriv(GL_TEXTURE_2D_MULTISAMPLE, 0, GL_TEXTURE_SAMPLES, &samples);
	printf("GLXTEST GL32 texture-samples=%d\n", (int)samples);
	enough = 0;
	if (samples >= 4)
		enough = 1;
	passed &= gl32_expect("multisample-samples", (long)enough, 1L);

	/* Drawn into: cleared green. */
	glGenFramebuffers(1, &framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE, texture, 0);
	complete = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	passed &= gl32_expect("multisample-complete", (long)complete, (long)GL_FRAMEBUFFER_COMPLETE);
	gl32_clear(GL32_GREEN);

	/* Read by the shader into the test's texture: the mean of the samples is green. */
	glBindFramebuffer(GL_FRAMEBUFFER, gl32_framebuffer);
	gl32_clear(GL32_BLACK);
	glUseProgram(gl32_samples);
	glUniform1i(gl32_samples_mode, 0);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	passed &= gl32_expect("multisample-sampled", (long)gl32_read(4, 4), (long)GL32_GREEN);

	/* Resolved into the test's texture by a blit. */
	gl32_clear(GL32_BLACK);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
	glBlitFramebuffer(0, 0, GL32_SIDE, GL32_SIDE, 0, 0, GL32_SIDE, GL32_SIDE, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, gl32_framebuffer);
	passed &= gl32_expect("multisample-resolved", (long)gl32_read(4, 4), (long)GL32_GREEN);

	/* The texture stays bound on unit 0 for the sample mask's check. */
	glDeleteFramebuffers(1, &framebuffer);
	gl32_outcomes[4] = passed;
}

/*
 * Checks the sample mask: a red square drawn into a black multisample
 * texture with only sample 0 kept reaches sample 0 and not sample 1; and
 * the sample positions lie inside the pixel.
 */
static void
gl32_sample_mask(void)
{
	GLuint texture;
	GLuint framebuffer;
	GLint value;
	GLfloat position[2];
	int passed;
	int inside;

	/* A multisample texture of four samples, black. */
	passed = 1;
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, texture);
	glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, 4, GL_RGBA8, GL32_SIDE, GL32_SIDE, GL_TRUE);
	glGenFramebuffers(1, &framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE, texture, 0);
	gl32_clear(GL32_BLACK);

	/* The positions of its samples. */
	position[0] = -1.0f;
	position[1] = -1.0f;
	glGetMultisamplefv(GL_SAMPLE_POSITION, 1U, position);
	inside = 0;
	if (position[0] >= 0.0f && position[0] <= 1.0f && position[1] >= 0.0f && position[1] <= 1.0f)
		inside = 1;
	passed &= gl32_expect("sample-position", (long)inside, 1L);

	/* Red into sample 0 alone. */
	glEnable(GL_SAMPLE_MASK);
	glSampleMaski(0U, 0x1U);
	value = 0;
	glGetIntegerv(GL_SAMPLE_MASK, &value);
	passed &= gl32_expect("sample-mask-enabled", (long)value, 1L);
	gl32_quad(0.0f, 0.0f, 1.0f, 1.0f, GL32_RED);
	glDisable(GL_SAMPLE_MASK);

	/* Read by the shader: green when sample 0 is red and sample 1 black. */
	glBindFramebuffer(GL_FRAMEBUFFER, gl32_framebuffer);
	gl32_clear(GL32_BLACK);
	glUseProgram(gl32_samples);
	glUniform1i(gl32_samples_mode, 1);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	passed &= gl32_expect("sample-mask", (long)gl32_read(4, 4), (long)GL32_GREEN);
	glDeleteFramebuffers(1, &framebuffer);
	glDeleteTextures(1, &texture);
	gl32_outcomes[5] = passed;
}

/* Checks the level parameters of a 2D texture: its size and internal format. */
static void
gl32_level_parameters(void)
{
	GLint value;
	int passed;

	/* The test's texture's level 0. */
	passed = 1;
	glBindTexture(GL_TEXTURE_2D, gl32_texture);
	value = 0;
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &value);
	passed &= gl32_expect("level-width", (long)value, (long)GL32_SIDE);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &value);
	passed &= gl32_expect("level-height", (long)value, (long)GL32_SIDE);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &value);
	passed &= gl32_expect("level-format", (long)value, (long)GL_RGBA8);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 1, GL_TEXTURE_WIDTH, &value);
	passed &= gl32_expect("level-1-width", (long)value, 0L);
	gl32_outcomes[6] = passed;
}

/* Prints one of the start's checks, counting it when the value is not the one expected; returns 1 when it is. */
static int
gl32_expect(
	const char *what,
	long got,
	long expected)
{
	/* A value that differs is a failure. */
	if (got != expected) {
		gl32_failures++;
		printf("GLXTEST GL32 check %s got=%ld expected=%ld FAILED\n", what, got, expected);
		return 0;
	}

	/* Succeeded: the line. */
	printf("GLXTEST GL32 check %s got=%ld expected=%ld ok\n", what, got, expected);
	return 1;
}
