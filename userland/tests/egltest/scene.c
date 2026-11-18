/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * egltest's drawing scene (WS068 p008): OpenGL ES 2 draws in the four
 * quarters of the window, each with a colour the test can check.
 *
 *   top left      a triangle strip from a buffer object, colour from
 *                 normalized bytes: orange (ff8000);
 *   top right     a fan from client arrays, a 2x2 texture sampled nearest:
 *                 red, green (bottom), blue, white (top);
 *   bottom left   blue, then red at half alpha over its right half
 *                 (blended: 800080), the colour a disabled array's value;
 *   bottom right  a green square in front of a red one drawn after it
 *                 (depth test), from glDrawElements with a buffer; culling
 *                 keeps the green (counter-clockwise) and drops a yellow
 *                 clockwise triangle.
 *
 * The shaders are SPIR-V (shaders/, shaders.h) given with glShaderBinary,
 * or (--scene=glsl, WS068 p019) GLSL ES 1.00 source compiled by
 * glCompileShader, or (--scene=glsl3, WS068 p020) GLSL ES 3.00 source in
 * an OpenGL ES 3 context; all draw the same colours.  With --scene=fbo
 * (WS068 p022) the scene is drawn into a framebuffer object's texture
 * (with a depth renderbuffer), and that texture over the whole window, so
 * the window shows the same colours in the same places.
 */

#include "scene.h"

#include "shaders.h"

#include <GLES2/gl2.h>

#include <stdio.h>
#include <string.h>

/* SPIR-V's binary format for glShaderBinary (GL 4.6's value). */
#define SCENE_SPIR_V		0x9551

/* The size of the framebuffer object's texture and depth renderbuffer. */
#define SCENE_FBO_SIZE		256

/* The program, its uniforms, the buffers and the texture. */
static GLuint scene_program;
static GLint scene_matrix;
static GLint scene_tint;
static GLint scene_textured;
static GLint scene_sampler;
static GLuint scene_vertices;
static GLuint scene_elements;
static GLuint scene_texture;

/* The framebuffer object of --scene=fbo, the texture it draws into and its depth renderbuffer (0 without --scene=fbo). */
static GLuint scene_fbo;
static GLuint scene_fbo_texture;
static GLuint scene_fbo_depth;

/* The attribute locations the program is linked with. */
#define SCENE_POSITION		0U
#define SCENE_COLOR		1U
#define SCENE_UV		2U

/*
 * The scene's vertex shader in GLSL ES 1.00: the SPIR-V one's work, the
 * tint applied by a function.
 */
static const char scene_vertex_source[] =
	"attribute vec4 a_position;\n"
	"attribute vec4 a_color;\n"
	"attribute vec2 a_uv;\n"
	"varying vec4 v_color;\n"
	"varying vec2 v_uv;\n"
	"uniform mat4 u_matrix;\n"
	"uniform vec4 u_tint;\n"
	"\n"
	"vec4 tinted(vec4 color)\n"
	"{\n"
	"\treturn color * u_tint;\n"
	"}\n"
	"\n"
	"void main()\n"
	"{\n"
	"\tv_color = tinted(a_color);\n"
	"\tv_uv = a_uv;\n"
	"\tgl_Position = u_matrix * a_position;\n"
	"}\n";

/*
 * The scene's vertex shader in GLSL ES 3.00: the attributes where the
 * scene puts them by their layout locations.
 */
static const char scene_vertex_source3[] =
	"#version 300 es\n"
	"layout(location = 0) in vec4 a_position;\n"
	"layout(location = 1) in vec4 a_color;\n"
	"layout(location = 2) in vec2 a_uv;\n"
	"out vec4 v_color;\n"
	"out vec2 v_uv;\n"
	"uniform mat4 u_matrix;\n"
	"uniform vec4 u_tint;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\tv_color = a_color * u_tint;\n"
	"\tv_uv = a_uv;\n"
	"\tgl_Position = u_matrix * a_position;\n"
	"}\n";

/*
 * The scene's fragment shader in GLSL ES 3.00: a located output, texture().
 */
static const char scene_fragment_source3[] =
	"#version 300 es\n"
	"precision mediump float;\n"
	"in vec4 v_color;\n"
	"in vec2 v_uv;\n"
	"uniform float u_textured;\n"
	"uniform sampler2D u_texture;\n"
	"layout(location = 0) out vec4 color;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\tvec4 texel = texture(u_texture, v_uv);\n"
	"\n"
	"\tcolor = u_textured > 0.5 ? v_color * texel : v_color;\n"
	"}\n";

/*
 * The scene's fragment shader in GLSL ES 1.00: the colour, times the
 * texture when u_textured is set.
 */
static const char scene_fragment_source[] =
	"precision mediump float;\n"
	"varying vec4 v_color;\n"
	"varying vec2 v_uv;\n"
	"uniform float u_textured;\n"
	"uniform sampler2D u_texture;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\tvec4 texel = texture2D(u_texture, v_uv);\n"
	"\n"
	"\tif (u_textured > 0.5)\n"
	"\t\tgl_FragColor = v_color * texel;\n"
	"\telse\n"
	"\t\tgl_FragColor = v_color;\n"
	"}\n";

static int scene_start(int glsl);
static GLuint scene_shader(GLenum type, const uint32_t *code, size_t size);
static GLuint scene_source(GLenum type, const char *source);
static void scene_place(float x, float y, float scale);
static void scene_colour(float red, float green, float blue, float alpha);
static int scene_expect(const char *token, const char *name, int x, int y, unsigned expected);

/*
 * Makes the program, the buffers and the texture.  Returns 0, or -1 with
 * a line saying what failed.
 */
int
egltest_scene_start(void)
{
	int status;

	/* The scene with SPIR-V shaders. */
	status = scene_start(0);
	if (status != 0)
		return -1;

	/* Succeeded. */
	return 0;
}

/*
 * Makes the program from GLSL source (compiled by glCompileShader), the
 * buffers and the texture.  Returns 0, or -1 with a line saying what
 * failed.
 */
int
egltest_scene_start_glsl(void)
{
	int status;

	/* The scene with GLSL shaders. */
	status = scene_start(1);
	if (status != 0)
		return -1;

	/* Succeeded. */
	return 0;
}

/*
 * Makes the scene with GLSL ES 1.00 shaders, and a framebuffer object that
 * draws into a texture of SCENE_FBO_SIZE with a 16-bit depth
 * renderbuffer.  Returns 0, or -1 with a line saying what failed.
 */
int
egltest_scene_start_fbo(void)
{
	GLenum status;
	GLenum error;
	int started;

	/* The scene with GLSL shaders. */
	started = scene_start(1);
	if (started != 0)
		return -1;

	/* The texture the framebuffer object draws into, sampled nearest and clamped. */
	glGenTextures(1, &scene_fbo_texture);
	glBindTexture(GL_TEXTURE_2D, scene_fbo_texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SCENE_FBO_SIZE, SCENE_FBO_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	/* Its depth buffer. */
	glGenRenderbuffers(1, &scene_fbo_depth);
	glBindRenderbuffer(GL_RENDERBUFFER, scene_fbo_depth);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, SCENE_FBO_SIZE, SCENE_FBO_SIZE);

	/* The framebuffer object with both attached. */
	glGenFramebuffers(1, &scene_fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, scene_fbo_texture, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, scene_fbo_depth);

	/* It must be complete. */
	status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	printf("EGLTEST FBO status=0x%x\n", (unsigned)status);
	if (status != GL_FRAMEBUFFER_COMPLETE)
		return -1;

	/* The window's framebuffer and the scene's own texture again. */
	glBindFramebuffer(GL_FRAMEBUFFER, 0U);
	glBindTexture(GL_TEXTURE_2D, scene_texture);

	/* GL reported no error. */
	error = glGetError();
	if (error != GL_NO_ERROR) {
		printf("EGLTEST FBO setup glerror=0x%x\n", (unsigned)error);
		return -1;
	}

	/* Succeeded: the scene can be drawn through the framebuffer object. */
	return 0;
}

/*
 * Draws the scene into the framebuffer object's texture, then that
 * texture over the whole window (so the window shows the scene).
 */
void
egltest_scene_draw_fbo(
	int width,
	int height)
{
	static const GLfloat fan[] = {
		-1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f
	};
	static const GLfloat fan_uv[] = {
		0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f
	};

	/* The scene into the texture, with its depth buffer. */
	glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo);
	egltest_scene_draw(SCENE_FBO_SIZE, SCENE_FBO_SIZE);

	/* The window, cleared to black. */
	glBindFramebuffer(GL_FRAMEBUFFER, 0U);
	glViewport(0, 0, width, height);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	/* The texture over all of it, untinted. */
	glUseProgram(scene_program);
	scene_place(0.0f, 0.0f, 1.0f);
	scene_colour(1.0f, 1.0f, 1.0f, 1.0f);
	glUniform1f(scene_textured, 1.0f);
	glBindTexture(GL_TEXTURE_2D, scene_fbo_texture);
	glDisableVertexAttribArray(SCENE_COLOR);
	glVertexAttrib4f(SCENE_COLOR, 1.0f, 1.0f, 1.0f, 1.0f);
	glVertexAttribPointer(SCENE_POSITION, 2, GL_FLOAT, GL_FALSE, 0, fan);
	glVertexAttribPointer(SCENE_UV, 2, GL_FLOAT, GL_FALSE, 0, fan_uv);
	glEnableVertexAttribArray(SCENE_POSITION);
	glEnableVertexAttribArray(SCENE_UV);
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

	/* The scene's own state again for the next frame. */
	glDisableVertexAttribArray(SCENE_UV);
	glUniform1f(scene_textured, 0.0f);
	glBindTexture(GL_TEXTURE_2D, scene_texture);
}

/*
 * Reads back the scene's colours from the framebuffer object (a line of
 * its own, run TOKEN-fbo) and from the window.  Returns how many differ.
 */
int
egltest_scene_check_fbo(
	int width,
	int height,
	const char *token)
{
	char fbo_token[64];
	int failures;

	/* The framebuffer object's texture, read through the framebuffer object. */
	(void)snprintf(fbo_token, sizeof(fbo_token), "%s-fbo", token);
	glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo);
	failures = egltest_scene_check(SCENE_FBO_SIZE, SCENE_FBO_SIZE, fbo_token);

	/* The window, which shows the texture. */
	glBindFramebuffer(GL_FRAMEBUFFER, 0U);
	failures += egltest_scene_check(width, height, token);

	/* Succeeded: how many of both differed. */
	return failures;
}

/*
 * Draws the scene over a window of a size.
 */
void
egltest_scene_draw(
	int width,
	int height)
{
	static const GLfloat fan[] = {
		-1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f
	};
	static const GLfloat fan_uv[] = {
		0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f
	};
	static const GLfloat whole[] = {
		-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f
	};
	static const GLfloat right_half[] = {
		0.0f, -1.0f, 1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 1.0f
	};
	static const GLfloat front[] = {
		-0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f, -0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f
	};
	static const GLfloat behind[] = {
		-1.0f, -1.0f, 0.8f, 1.0f, -1.0f, 0.8f, -1.0f, 1.0f, 0.8f, 1.0f, 1.0f, 0.8f
	};
	static const GLfloat clockwise[] = {
		0.5f, 0.2f, 0.0f, 0.9f, 0.9f, 0.0f, 0.9f, 0.2f, 0.0f
	};

	/* The frame: dark grey, depth 1, the whole window. */
	glViewport(0, 0, width, height);
	glClearColor(0.125f, 0.125f, 0.125f, 1.0f);
	glClearDepthf(1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glUseProgram(scene_program);
	glUniform1f(scene_textured, 0.0f);
	scene_colour(1.0f, 1.0f, 1.0f, 1.0f);
	glDisableVertexAttribArray(SCENE_UV);

	/* Top left: a strip from the buffer object, colours from normalized bytes. */
	scene_place(-0.5f, 0.5f, 0.4f);
	glBindBuffer(GL_ARRAY_BUFFER, scene_vertices);
	glVertexAttribPointer(SCENE_POSITION, 2, GL_FLOAT, GL_FALSE, 0, (const void *)0);
	glVertexAttribPointer(SCENE_COLOR, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, (const void *)(8U * sizeof(GLfloat)));
	glEnableVertexAttribArray(SCENE_POSITION);
	glEnableVertexAttribArray(SCENE_COLOR);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glBindBuffer(GL_ARRAY_BUFFER, 0U);

	/* Top right: a fan from client arrays, textured (the colour a disabled array's white). */
	scene_place(0.5f, 0.5f, 0.4f);
	glUniform1f(scene_textured, 1.0f);
	glDisableVertexAttribArray(SCENE_COLOR);
	glVertexAttrib4f(SCENE_COLOR, 1.0f, 1.0f, 1.0f, 1.0f);
	glVertexAttribPointer(SCENE_POSITION, 2, GL_FLOAT, GL_FALSE, 0, fan);
	glVertexAttribPointer(SCENE_UV, 2, GL_FLOAT, GL_FALSE, 0, fan_uv);
	glEnableVertexAttribArray(SCENE_UV);
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
	glDisableVertexAttribArray(SCENE_UV);
	glUniform1f(scene_textured, 0.0f);

	/* Bottom left: blue, then red at half alpha blended over its right half. */
	scene_place(-0.5f, -0.5f, 0.4f);
	glVertexAttrib4f(SCENE_COLOR, 0.0f, 0.0f, 1.0f, 1.0f);
	glVertexAttribPointer(SCENE_POSITION, 2, GL_FLOAT, GL_FALSE, 0, whole);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glVertexAttrib4f(SCENE_COLOR, 1.0f, 0.0f, 0.0f, 0.5f);
	glVertexAttribPointer(SCENE_POSITION, 2, GL_FLOAT, GL_FALSE, 0, right_half);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisable(GL_BLEND);

	/* Bottom right: green in front (culling keeps it), then red behind, by indices from a buffer. */
	scene_place(0.5f, -0.5f, 0.4f);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	glEnable(GL_CULL_FACE);
	glCullFace(GL_BACK);
	glFrontFace(GL_CCW);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, scene_elements);
	glVertexAttrib4f(SCENE_COLOR, 0.0f, 1.0f, 0.0f, 1.0f);
	glVertexAttribPointer(SCENE_POSITION, 3, GL_FLOAT, GL_FALSE, 0, front);
	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (const void *)0);
	glVertexAttrib4f(SCENE_COLOR, 1.0f, 0.0f, 0.0f, 1.0f);
	glVertexAttribPointer(SCENE_POSITION, 3, GL_FLOAT, GL_FALSE, 0, behind);
	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (const void *)0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0U);

	/* A yellow clockwise triangle in front of the red: culled. */
	glVertexAttrib4f(SCENE_COLOR, 1.0f, 1.0f, 0.0f, 1.0f);
	glVertexAttribPointer(SCENE_POSITION, 3, GL_FLOAT, GL_FALSE, 0, clockwise);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glDisable(GL_CULL_FACE);
	glDisable(GL_DEPTH_TEST);
}

/*
 * Reads back the scene's colours (after egltest_scene_draw, before the
 * swap) and prints one line each.  Returns how many differ.
 */
int
egltest_scene_check(
	int width,
	int height,
	const char *token)
{
	GLenum error;
	int failures;

	/* Each quarter's colours, in GL's coordinates (from the bottom left). */
	failures = 0;
	failures += scene_expect(token, "strip", width / 4, height * 3 / 4, 0xff8000U);
	failures += scene_expect(token, "texture-red", width * 65 / 100, height * 65 / 100, 0xff0000U);
	failures += scene_expect(token, "texture-green", width * 85 / 100, height * 65 / 100, 0x00ff00U);
	failures += scene_expect(token, "texture-blue", width * 65 / 100, height * 85 / 100, 0x0000ffU);
	failures += scene_expect(token, "texture-white", width * 85 / 100, height * 85 / 100, 0xffffffU);
	failures += scene_expect(token, "opaque", width * 15 / 100, height / 4, 0x0000ffU);
	failures += scene_expect(token, "blend", width * 35 / 100, height / 4, 0x800080U);
	failures += scene_expect(token, "depth-front", width * 3 / 4, height / 4, 0x00ff00U);
	failures += scene_expect(token, "depth-behind", width * 58 / 100, height * 12 / 100, 0xff0000U);
	failures += scene_expect(token, "culled", width * 92 / 100, height * 325 / 1000, 0xff0000U);
	failures += scene_expect(token, "background", width / 2, height / 2, 0x202020U);

	/* The readbacks raised no error. */
	error = glGetError();
	printf("EGLTEST CHECK run=%s failures=%d glerror=0x%x\n", token, failures, (unsigned)error);
	fflush(stdout);
	if (error != GL_NO_ERROR)
		failures++;
	return failures;
}

/*
 * Makes the program from GLSL ES 3.00 source in an OpenGL ES 3 context,
 * the buffers and the texture.  Returns 0, or -1 with a line saying what
 * failed.
 */
int
egltest_scene_start_glsl3(void)
{
	int status;

	/* The scene with GLSL ES 3.00 shaders. */
	status = scene_start(2);
	if (status != 0)
		return -1;

	/* Succeeded. */
	return 0;
}

/* Makes the program (from SPIR-V, GLSL ES 1.00 or GLSL ES 3.00 source), the buffers and the texture; -1 with a line when something fails. */
static int
scene_start(
	int glsl)
{
	static const GLfloat strip[] = {
		-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f
	};
	static const GLubyte strip_colors[] = {
		255, 128, 0, 255, 255, 128, 0, 255, 255, 128, 0, 255, 255, 128, 0, 255
	};
	static const GLushort square[] = {
		0, 1, 2, 2, 1, 3
	};
	static const GLubyte texels[] = {
		255, 0, 0, 255, 0, 255, 0, 255,
		0, 0, 255, 255, 255, 255, 255, 255
	};
	GLuint vertex;
	GLuint fragment;
	GLint linked;
	char log[256];

	/* The two shaders, from GLSL source or from SPIR-V. */
	if (glsl == 2) {
		vertex = scene_source(GL_VERTEX_SHADER, scene_vertex_source3);
		fragment = scene_source(GL_FRAGMENT_SHADER, scene_fragment_source3);
	} else if (glsl) {
		vertex = scene_source(GL_VERTEX_SHADER, scene_vertex_source);
		fragment = scene_source(GL_FRAGMENT_SHADER, scene_fragment_source);
	} else {
		vertex = scene_shader(GL_VERTEX_SHADER, egltest_scene_vert, sizeof(egltest_scene_vert));
		fragment = scene_shader(GL_FRAGMENT_SHADER, egltest_scene_frag, sizeof(egltest_scene_frag));
	}

	/* Both must be there. */
	if (vertex == 0U || fragment == 0U)
		return -1;

	/* The program with the attributes where the scene puts them. */
	scene_program = glCreateProgram();
	glAttachShader(scene_program, vertex);
	glAttachShader(scene_program, fragment);
	glBindAttribLocation(scene_program, SCENE_POSITION, "a_position");
	glBindAttribLocation(scene_program, SCENE_COLOR, "a_color");
	glBindAttribLocation(scene_program, SCENE_UV, "a_uv");
	glLinkProgram(scene_program);
	glDeleteShader(vertex);
	glDeleteShader(fragment);
	linked = GL_FALSE;
	glGetProgramiv(scene_program, GL_LINK_STATUS, &linked);
	if (!linked) {
		log[0] = '\0';
		glGetProgramInfoLog(scene_program, (GLsizei)sizeof(log), NULL, log);
		printf("EGLTEST SCENE link failed: %s\n", log);
		return -1;
	}

	/* Its uniforms. */
	glUseProgram(scene_program);
	scene_matrix = glGetUniformLocation(scene_program, "u_matrix");
	scene_tint = glGetUniformLocation(scene_program, "u_tint");
	scene_textured = glGetUniformLocation(scene_program, "u_textured");
	scene_sampler = glGetUniformLocation(scene_program, "u_texture");
	if (scene_matrix < 0 || scene_tint < 0 || scene_textured < 0 || scene_sampler < 0) {
		printf("EGLTEST SCENE uniforms missing: %d %d %d %d\n", scene_matrix, scene_tint, scene_textured, scene_sampler);
		return -1;
	}

	/* The sampler reads texture unit 0. */
	glUniform1i(scene_sampler, 0);

	/* The strip's positions, then its colours, in one buffer object. */
	glGenBuffers(1, &scene_vertices);
	glBindBuffer(GL_ARRAY_BUFFER, scene_vertices);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(strip) + sizeof(strip_colors)), NULL, GL_STATIC_DRAW);
	glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)sizeof(strip), strip);
	glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)sizeof(strip), (GLsizeiptr)sizeof(strip_colors), strip_colors);
	glBindBuffer(GL_ARRAY_BUFFER, 0U);

	/* The square's indices in an element buffer. */
	glGenBuffers(1, &scene_elements);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, scene_elements);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)sizeof(square), square, GL_STATIC_DRAW);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0U);

	/* The 2x2 texture, sampled nearest and clamped. */
	glGenTextures(1, &scene_texture);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, scene_texture);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	/* Succeeded unless GL reported an error. */
	linked = (GLint)glGetError();
	if (linked != GL_NO_ERROR) {
		printf("EGLTEST SCENE setup glerror=0x%x\n", (unsigned)linked);
		return -1;
	}

	/* No error. */
	return 0;
}

/* Makes a shader from SPIR-V; 0 with a line when it fails. */
static GLuint
scene_shader(
	GLenum type,
	const uint32_t *code,
	size_t size)
{
	GLuint shader;
	GLint compiled;

	/* The shader with the binary. */
	shader = glCreateShader(type);
	glShaderBinary(1, &shader, SCENE_SPIR_V, code, (GLsizei)size);
	compiled = GL_FALSE;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
	if (!compiled) {
		printf("EGLTEST SCENE shader 0x%x refused glerror=0x%x\n", (unsigned)type, (unsigned)glGetError());
		return 0U;
	}

	/* Succeeded: the shader. */
	return shader;
}

/* Makes a shader from GLSL source; 0 with a line (and the compiler's log) when it fails. */
static GLuint
scene_source(
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
		printf("EGLTEST SCENE shader 0x%x did not compile: %s\n", (unsigned)type, log);
		return 0U;
	}

	/* Succeeded: the shader. */
	printf("EGLTEST SCENE shader 0x%x compiled from GLSL\n", (unsigned)type);
	return shader;
}

/* Sets the matrix that puts the unit square at a centre with a scale. */
static void
scene_place(
	float x,
	float y,
	float scale)
{
	GLfloat matrix[16];

	/* Column-major: the scale on the diagonal, the move in the last column. */
	memset(matrix, 0, sizeof(matrix));
	matrix[0] = scale;
	matrix[5] = scale;
	matrix[10] = 1.0f;
	matrix[12] = x;
	matrix[13] = y;
	matrix[15] = 1.0f;
	glUniformMatrix4fv(scene_matrix, 1, GL_FALSE, matrix);
}

/* Sets the tint. */
static void
scene_colour(
	float red,
	float green,
	float blue,
	float alpha)
{
	/* The uniform's four floats. */
	glUniform4f(scene_tint, red, green, blue, alpha);
}

/* Reads one pixel and compares it (each channel within 3); returns 1 when it differs. */
static int
scene_expect(
	const char *token,
	const char *name,
	int x,
	int y,
	unsigned expected)
{
	GLubyte pixel[4];
	const char *verdict;
	unsigned got;
	int differs;
	int channel;
	int delta;

	/* The pixel as RGB. */
	memset(pixel, 0, sizeof(pixel));
	glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
	got = ((unsigned)pixel[0] << 16) | ((unsigned)pixel[1] << 8) | (unsigned)pixel[2];

	/* Each channel within 3 of the expected one. */
	differs = 0;
	for (channel = 0; channel < 3; channel++) {
		delta = (int)((got >> (channel * 8)) & 0xffU) - (int)((expected >> (channel * 8)) & 0xffU);
		if (delta > 3 || delta < -3)
			differs = 1;
	}

	/* One line per pixel. */
	verdict = "ok";
	if (differs)
		verdict = "DIFFERS";
	printf("EGLTEST PIXEL run=%s name=%s x=%d y=%d got=%06x expected=%06x %s\n", token, name, x, y, got, expected, verdict);
	return differs;
}
