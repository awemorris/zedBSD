/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * egltest's cube map scene (WS068 p023): six squares in two rows, each
 * sampling one axis direction of a cube map whose faces are one colour
 * each.  The faces are made three ways: +X, -X and +Y by glTexImage2D;
 * -Y and +Z drawn into as a framebuffer object's attachment (cleared);
 * -Z copied with glCopyTexSubImage2D from a framebuffer object's 2D
 * texture.  glGenerateMipmap then reads the drawn faces back and mipmaps
 * every face, which the squares sample.
 *
 *   top row       +X red, -X green, +Y blue;
 *   bottom row    -Y yellow, +Z magenta, -Z cyan.
 */

#include "cube.h"

#include <GLES2/gl2.h>

#include <stdio.h>
#include <string.h>

/* The size of each face, and of the 2D texture -Z is copied from. */
#define CUBE_SIZE		16

/* The attribute location of the squares' positions. */
#define CUBE_POSITION		0U

/*
 * The program, its uniforms, and the cube map; made by
 * egltest_cube_start and kept for the run.
 */
static GLuint cube_program;
static GLint cube_matrix;
static GLint cube_direction;
static GLint cube_sampler;
static GLuint cube_texture;

/*
 * The vertex shader: a square placed by the matrix.
 */
static const char cube_vertex_source[] =
	"attribute vec4 a_position;\n"
	"uniform mat4 u_matrix;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\tgl_Position = u_matrix * a_position;\n"
	"}\n";

/*
 * The fragment shader: the cube map in one direction.
 */
static const char cube_fragment_source[] =
	"precision mediump float;\n"
	"uniform samplerCube u_cube;\n"
	"uniform vec3 u_direction;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\tgl_FragColor = textureCube(u_cube, u_direction);\n"
	"}\n";

/*
 * Each face's colour as 0xRRGGBB, in GL's face order (+X, -X, +Y, -Y, +Z,
 * -Z), the direction that samples it, and where its square is (its
 * centre in the window's normalized coordinates).
 */
static const unsigned cube_colors[6] = {
	0xff0000U, 0x00ff00U, 0x0000ffU, 0xffff00U, 0xff00ffU, 0x00ffffU
};
static const GLfloat cube_directions[6][3] = {
	{ 1.0f, 0.0f, 0.0f }, { -1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f },
	{ 0.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, -1.0f }
};
static const GLfloat cube_centres[6][2] = {
	{ -0.6f, 0.5f }, { 0.0f, 0.5f }, { 0.6f, 0.5f },
	{ -0.6f, -0.5f }, { 0.0f, -0.5f }, { 0.6f, -0.5f }
};

static GLuint cube_shader(GLenum type, const char *source);
static int cube_solid(unsigned face);
static int cube_drawn(unsigned face);
static int cube_copied(unsigned face);
static void cube_clear_colour(unsigned color);
static int cube_expect(const char *token, unsigned face, int x, int y);

/*
 * Makes the program and the cube map (its faces made the three ways, then
 * mipmapped).  Returns 0, or -1 with a line saying what failed.
 */
int
egltest_cube_start(void)
{
	GLuint vertex;
	GLuint fragment;
	GLint linked;
	GLenum error;
	unsigned face;
	int status;

	/* The two shaders. */
	vertex = cube_shader(GL_VERTEX_SHADER, cube_vertex_source);
	fragment = cube_shader(GL_FRAGMENT_SHADER, cube_fragment_source);
	if (vertex == 0U || fragment == 0U)
		return -1;

	/* The program, the positions at their location. */
	cube_program = glCreateProgram();
	glAttachShader(cube_program, vertex);
	glAttachShader(cube_program, fragment);
	glBindAttribLocation(cube_program, CUBE_POSITION, "a_position");
	glLinkProgram(cube_program);
	glDeleteShader(vertex);
	glDeleteShader(fragment);
	linked = GL_FALSE;
	glGetProgramiv(cube_program, GL_LINK_STATUS, &linked);
	if (!linked) {
		printf("EGLTEST CUBE link failed\n");
		return -1;
	}

	/* Its uniforms; the sampler reads unit 0. */
	glUseProgram(cube_program);
	cube_matrix = glGetUniformLocation(cube_program, "u_matrix");
	cube_direction = glGetUniformLocation(cube_program, "u_direction");
	cube_sampler = glGetUniformLocation(cube_program, "u_cube");
	if (cube_matrix < 0 || cube_direction < 0 || cube_sampler < 0) {
		printf("EGLTEST CUBE uniforms missing: %d %d %d\n", cube_matrix, cube_direction, cube_sampler);
		return -1;
	}

	/* The sampler reads texture unit 0. */
	glUniform1i(cube_sampler, 0);

	/* The cube map, mipmapped and clamped. */
	glGenTextures(1, &cube_texture);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_CUBE_MAP, cube_texture);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	/* +X, -X and +Y from the CPU. */
	for (face = 0U; face < 3U; face++) {
		status = cube_solid(face);
		if (status != 0)
			return -1;
	}

	/* -Y and +Z drawn into by a framebuffer object. */
	for (face = 3U; face < 5U; face++) {
		status = cube_drawn(face);
		if (status != 0)
			return -1;
	}

	/* -Z copied from a framebuffer object. */
	status = cube_copied(5U);
	if (status != 0)
		return -1;

	/* Every face's levels (the drawn faces are read back first). */
	glBindTexture(GL_TEXTURE_CUBE_MAP, cube_texture);
	glGenerateMipmap(GL_TEXTURE_CUBE_MAP);

	/* GL reported no error. */
	error = glGetError();
	if (error != GL_NO_ERROR) {
		printf("EGLTEST CUBE setup glerror=0x%x\n", (unsigned)error);
		return -1;
	}

	/* Succeeded: the cube map is ready. */
	printf("EGLTEST CUBE ready\n");
	return 0;
}

/*
 * Draws the six squares over a window of a size, on dark grey.
 */
void
egltest_cube_draw(
	int width,
	int height)
{
	static const GLfloat square[] = {
		-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f
	};
	GLfloat matrix[16];
	unsigned face;

	/* The frame: dark grey, the whole window. */
	glBindFramebuffer(GL_FRAMEBUFFER, 0U);
	glViewport(0, 0, width, height);
	glClearColor(0.125f, 0.125f, 0.125f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	/* The program, the cube map on unit 0, the square's corners. */
	glUseProgram(cube_program);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_CUBE_MAP, cube_texture);
	glVertexAttribPointer(CUBE_POSITION, 2, GL_FLOAT, GL_FALSE, 0, square);
	glEnableVertexAttribArray(CUBE_POSITION);

	/* Each face's square, a quarter of the window's height across, sampling its direction. */
	memset(matrix, 0, sizeof(matrix));
	matrix[0] = 0.2f;
	matrix[5] = 0.3f;
	matrix[10] = 1.0f;
	matrix[15] = 1.0f;
	for (face = 0U; face < 6U; face++) {
		matrix[12] = cube_centres[face][0];
		matrix[13] = cube_centres[face][1];
		glUniformMatrix4fv(cube_matrix, 1, GL_FALSE, matrix);
		glUniform3fv(cube_direction, 1, cube_directions[face]);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	}
}

/*
 * Reads back each square's centre and the background, one line each.
 * Returns how many differ.
 */
int
egltest_cube_check(
	int width,
	int height,
	const char *token)
{
	GLubyte pixel[4];
	GLenum error;
	unsigned got;
	unsigned face;
	int failures;
	int x;
	int y;

	/* Each square's centre, in GL's coordinates. */
	failures = 0;
	for (face = 0U; face < 6U; face++) {
		x = (int)((cube_centres[face][0] + 1.0f) * 0.5f * (float)width);
		y = (int)((cube_centres[face][1] + 1.0f) * 0.5f * (float)height);
		failures += cube_expect(token, face, x, y);
	}

	/* The background between the squares. */
	memset(pixel, 0, sizeof(pixel));
	glReadPixels(width / 2, height / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
	got = ((unsigned)pixel[0] << 16) | ((unsigned)pixel[1] << 8) | (unsigned)pixel[2];
	if (got != 0x202020U)
		failures++;
	printf("EGLTEST PIXEL run=%s name=background got=%06x expected=202020\n", token, got);

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
cube_shader(
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
		printf("EGLTEST CUBE shader 0x%x did not compile: %s\n", (unsigned)type, log);
		return 0U;
	}

	/* Succeeded: the shader. */
	return shader;
}

/* Gives a face its colour from the CPU; nonzero on a GL error. */
static int
cube_solid(
	unsigned face)
{
	GLubyte texels[CUBE_SIZE * CUBE_SIZE * 4];
	unsigned color;
	unsigned texel;
	GLenum error;

	/* Every texel the face's colour, opaque. */
	color = cube_colors[face];
	for (texel = 0U; texel < CUBE_SIZE * CUBE_SIZE; texel++) {
		texels[texel * 4U] = (GLubyte)(color >> 16);
		texels[texel * 4U + 1U] = (GLubyte)(color >> 8);
		texels[texel * 4U + 2U] = (GLubyte)color;
		texels[texel * 4U + 3U] = 255U;
	}

	/* The face's level 0. */
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_RGBA, CUBE_SIZE, CUBE_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
	error = glGetError();
	if (error != GL_NO_ERROR) {
		printf("EGLTEST CUBE face %u glerror=0x%x\n", face, (unsigned)error);
		return -1;
	}

	/* Succeeded: the face has its colour. */
	return 0;
}

/* Gives a face its colour by clearing it as a framebuffer object's attachment; nonzero when the object is not complete. */
static int
cube_drawn(
	unsigned face)
{
	GLuint framebuffer;
	GLenum status;

	/* The face's level 0, with no data. */
	glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_RGBA, CUBE_SIZE, CUBE_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

	/* A framebuffer object drawing into it. */
	glGenFramebuffers(1, &framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, cube_texture, 0);
	status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	printf("EGLTEST CUBE face %u fbo status=0x%x\n", face, (unsigned)status);
	if (status != GL_FRAMEBUFFER_COMPLETE)
		return -1;

	/* Cleared to the face's colour. */
	glViewport(0, 0, CUBE_SIZE, CUBE_SIZE);
	cube_clear_colour(cube_colors[face]);
	glClear(GL_COLOR_BUFFER_BIT);

	/* The window's framebuffer again; the object goes (the face keeps what was drawn). */
	glBindFramebuffer(GL_FRAMEBUFFER, 0U);
	glDeleteFramebuffers(1, &framebuffer);

	/* Succeeded: the face was drawn. */
	return 0;
}

/* Gives a face its colour by copying it from a framebuffer object's 2D texture cleared to the colour; nonzero when the object is not complete. */
static int
cube_copied(
	unsigned face)
{
	GLuint framebuffer;
	GLuint source;
	GLenum status;

	/* The face's level 0, with no data yet. */
	glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_RGBA, CUBE_SIZE, CUBE_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

	/* A 2D texture of the same size in a framebuffer object, cleared to the colour. */
	glGenTextures(1, &source);
	glBindTexture(GL_TEXTURE_2D, source);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, CUBE_SIZE, CUBE_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, source, 0);
	status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	printf("EGLTEST CUBE copy fbo status=0x%x\n", (unsigned)status);
	if (status != GL_FRAMEBUFFER_COMPLETE)
		return -1;

	/* Cleared to the face's colour. */
	glViewport(0, 0, CUBE_SIZE, CUBE_SIZE);
	cube_clear_colour(cube_colors[face]);
	glClear(GL_COLOR_BUFFER_BIT);

	/* Copied from the framebuffer object into the face. */
	glBindTexture(GL_TEXTURE_CUBE_MAP, cube_texture);
	glCopyTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, 0, 0, 0, 0, CUBE_SIZE, CUBE_SIZE);

	/* The window's framebuffer again; the object and its texture go. */
	glBindFramebuffer(GL_FRAMEBUFFER, 0U);
	glDeleteFramebuffers(1, &framebuffer);
	glDeleteTextures(1, &source);

	/* Succeeded: the face was copied. */
	return 0;
}

/* Sets the clear colour from 0xRRGGBB, opaque. */
static void
cube_clear_colour(
	unsigned color)
{
	float red;
	float green;
	float blue;

	/* Each channel as a float. */
	red = (float)((color >> 16) & 0xffU) / 255.0f;
	green = (float)((color >> 8) & 0xffU) / 255.0f;
	blue = (float)(color & 0xffU) / 255.0f;
	glClearColor(red, green, blue, 1.0f);
}

/* Reads one square's centre and compares it with its face's colour (each channel within 3); 1 when it differs. */
static int
cube_expect(
	const char *token,
	unsigned face,
	int x,
	int y)
{
	GLubyte pixel[4];
	const char *verdict;
	unsigned got;
	unsigned expected;
	int differs;
	int channel;
	int delta;

	/* The pixel as RGB. */
	memset(pixel, 0, sizeof(pixel));
	glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
	got = ((unsigned)pixel[0] << 16) | ((unsigned)pixel[1] << 8) | (unsigned)pixel[2];

	/* Each channel within 3 of the face's colour. */
	expected = cube_colors[face];
	differs = 0;
	for (channel = 0; channel < 3; channel++) {
		delta = (int)((got >> (channel * 8)) & 0xffU) - (int)((expected >> (channel * 8)) & 0xffU);
		if (delta > 3 || delta < -3)
			differs = 1;
	}

	/* One line per square. */
	verdict = "ok";
	if (differs)
		verdict = "DIFFERS";
	printf("EGLTEST PIXEL run=%s name=face%u x=%d y=%d got=%06x expected=%06x %s\n", token, face, x, y, got, expected, verdict);

	/* Succeeded: whether it differs. */
	return differs;
}
