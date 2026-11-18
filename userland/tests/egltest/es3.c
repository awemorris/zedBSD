/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * egltest's OpenGL ES 3.0 API scene (WS068 p024): shapes drawn from four
 * vertex array objects with one GLSL ES 3.00 program, their colours from
 * two uniform blocks read from buffers.
 *
 *   top row       A red: glDrawRangeElements, the colour index a current
 *                 value (glVertexAttribI4i);
 *                 B green: an interleaved buffer whose colour index is an
 *                 unsigned byte read as an int (glVertexAttribIPointer);
 *                 C blue and yellow: one triangle strip that the fixed
 *                 restart index splits in two, its indices ints; the gap
 *                 between them stays the background;
 *   bottom row    D: four instances (glDrawElementsInstanced) placed by
 *                 gl_InstanceID, magenta, cyan, grey and olive: a colour
 *                 per instance (divisor 1) times a gain per two instances
 *                 (divisor 2: 1.0, then 0.6).
 *
 * The palette block (four colours, an offset and a gain) is a range of a
 * buffer at the uniform buffer offset alignment (glBindBufferRange): its
 * colours written through glMapBufferRange and its offset and gain
 * copied in by glCopyBufferSubData.  The tint block (instance name, in
 * the fragment shader) reads binding point 2 (glUniformBlockBinding).
 * A uint uniform must be 7 for any colour to show.  The start also checks
 * what the API reports: block sizes and members, uniform offsets, indexed
 * bindings, glGetStringi, glGetInteger64v, a read mapping.
 */

#include "es3.h"

#include <GLES3/gl3.h>

#include <stdio.h>
#include <string.h>

/* The attribute locations the shaders give. */
#define ES3_POSITION		0U
#define ES3_INDEX		1U
#define ES3_COLOUR		2U
#define ES3_GAIN		3U

/* The number of shapes read back, and the vertex array objects. */
#define ES3_POINTS		9U
#define ES3_ARRAYS		4U

/* The palette block's std140 size, and where its offset and gain are in it. */
#define ES3_PALETTE_SIZE	80
#define ES3_PALETTE_TAIL	64

/* The binding point the tint block reads. */
#define ES3_TINT_BINDING	2U

/*
 * The program, its uniforms, the buffers and the vertex array objects;
 * made by egltest_es3_start and kept for the run, with the number of the
 * start's checks that failed.
 */
static GLuint es3_program;
static GLint es3_select;
static GLint es3_step;
static GLuint es3_arrays[ES3_ARRAYS];
static GLuint es3_palette;
static GLuint es3_tint;
static GLint es3_alignment;
static int es3_failures;

/*
 * The vertex shader: a position moved by the palette's offset and by the
 * instance, and a colour from the palette (by an integer index) or from
 * the per-instance array, times the gains; black unless u_select is 7.
 */
static const char es3_vertex_source[] =
	"#version 300 es\n"
	"layout(location = 0) in vec2 a_position;\n"
	"layout(location = 1) in int a_index;\n"
	"layout(location = 2) in vec4 a_colour;\n"
	"layout(location = 3) in float a_gain;\n"
	"layout(std140) uniform Palette {\n"
	"\tvec4 colours[4];\n"
	"\tvec2 offset;\n"
	"\tfloat gain;\n"
	"};\n"
	"uniform uint u_select;\n"
	"uniform vec2 u_step;\n"
	"out vec4 v_colour;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\tvec4 colour = a_colour;\n"
	"\tif (a_index >= 0)\n"
	"\t\tcolour = colours[a_index];\n"
	"\tif (u_select != 7u)\n"
	"\t\tcolour = vec4(0.0, 0.0, 0.0, 1.0);\n"
	"\tv_colour = vec4(colour.rgb * a_gain * gain, 1.0);\n"
	"\tgl_Position = vec4(a_position + offset + u_step * float(gl_InstanceID), 0.0, 1.0);\n"
	"}\n";

/*
 * The fragment shader: the colour times the tint block's tint.
 */
static const char es3_fragment_source[] =
	"#version 300 es\n"
	"precision mediump float;\n"
	"in vec4 v_colour;\n"
	"uniform Tint {\n"
	"\tvec4 tint;\n"
	"} tint_block;\n"
	"out vec4 o_colour;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\to_colour = v_colour * tint_block.tint;\n"
	"}\n";

/* The palette's colours (red, green, blue, yellow), its offset (none) and gain (1), as the std140 block holds them. */
static const GLfloat es3_colours[16] = {
	1.0f, 0.0f, 0.0f, 1.0f,
	0.0f, 1.0f, 0.0f, 1.0f,
	0.0f, 0.0f, 1.0f, 1.0f,
	1.0f, 1.0f, 0.0f, 1.0f
};
static const GLfloat es3_tail[4] = { 0.0f, 0.0f, 1.0f, 0.0f };

/*
 * The points read back: each shape's centre in the window's normalized
 * coordinates, and the colour expected there as 0xRRGGBB.
 */
static const char *const es3_names[ES3_POINTS] = {
	"A", "B", "C1", "gap", "C2", "D0", "D1", "D2", "D3"
};
static const GLfloat es3_centres[ES3_POINTS][2] = {
	{ -0.7f, 0.6f }, { -0.2f, 0.6f }, { 0.25f, 0.6f }, { 0.45f, 0.6f }, { 0.7f, 0.6f },
	{ -0.725f, -0.6f }, { -0.275f, -0.6f }, { 0.175f, -0.6f }, { 0.625f, -0.6f }
};
static const unsigned es3_expected[ES3_POINTS] = {
	0xff0000U, 0x00ff00U, 0x0000ffU, 0x202020U, 0xffff00U,
	0xff00ffU, 0x00ffffU, 0x999999U, 0x999900U
};

static GLuint es3_shader(GLenum type, const char *source);
static int es3_buffers(void);
static void es3_arrays_make(void);
static void es3_expect(const char *what, long got, long expected);
static int es3_blocks_check(void);
static int es3_close(unsigned got, unsigned expected);

/*
 * Makes the program, the uniform buffers and the vertex array objects,
 * and checks what the API reports of them.  Returns 0, or -1 with a line
 * saying what failed.
 */
int
egltest_es3_start(void)
{
	const GLubyte *extension;
	GLuint vertex;
	GLuint fragment;
	GLint linked;
	GLint count;
	GLint64 largest;
	GLuint select;
	GLint current[4];
	GLenum error;
	unsigned index;
	int status;

	/* The two shaders. */
	vertex = es3_shader(GL_VERTEX_SHADER, es3_vertex_source);
	fragment = es3_shader(GL_FRAGMENT_SHADER, es3_fragment_source);
	if (vertex == 0U || fragment == 0U)
		return -1;

	/* The program (the shaders give the attributes' locations). */
	es3_program = glCreateProgram();
	glAttachShader(es3_program, vertex);
	glAttachShader(es3_program, fragment);
	glLinkProgram(es3_program);
	glDeleteShader(vertex);
	glDeleteShader(fragment);
	linked = GL_FALSE;
	glGetProgramiv(es3_program, GL_LINK_STATUS, &linked);
	if (!linked) {
		printf("EGLTEST ES3 link failed\n");
		return -1;
	}

	/* Its uniforms: the selector must be 7 for colours to show. */
	glUseProgram(es3_program);
	es3_select = glGetUniformLocation(es3_program, "u_select");
	es3_step = glGetUniformLocation(es3_program, "u_step");
	if (es3_select < 0 || es3_step < 0) {
		printf("EGLTEST ES3 uniforms missing: %d %d\n", es3_select, es3_step);
		return -1;
	}

	/* The selector's value. */
	glUniform1ui(es3_select, 7U);

	/* The selector reads back as the unsigned int it was given. */
	select = 0U;
	glGetUniformuiv(es3_program, es3_select, &select);
	es3_expect("uniform-uint", (long)select, 7L);

	/* The uniform buffers and the blocks' bindings. */
	status = es3_buffers();
	if (status != 0)
		return -1;
	status = es3_blocks_check();
	if (status != 0)
		return -1;

	/* The vertex array objects, each bound once. */
	es3_arrays_make();
	for (index = 0U; index < ES3_ARRAYS; index++)
		es3_expect("is-vertex-array", (long)glIsVertexArray(es3_arrays[index]), (long)GL_TRUE);
	count = 0;
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &count);
	es3_expect("vertex-array-binding", (long)count, (long)es3_arrays[ES3_ARRAYS - 1U]);
	glBindVertexArray(0U);

	/* An integer current value reads back as the ints it was given. */
	glVertexAttribI4i(ES3_INDEX, -1, 2, 3, 4);
	memset(current, 0, sizeof(current));
	glGetVertexAttribIiv(ES3_INDEX, GL_CURRENT_VERTEX_ATTRIB, current);
	es3_expect("current-int", (long)current[0], -1L);

	/* The extensions one by one, and a 64-bit state. */
	count = 0;
	glGetIntegerv(GL_NUM_EXTENSIONS, &count);
	extension = glGetStringi(GL_EXTENSIONS, 0U);
	es3_expect("num-extensions", (long)(count > 0), 1L);
	es3_expect("string-i", (long)(extension != NULL), 1L);
	largest = 0;
	glGetInteger64v(GL_MAX_ELEMENT_INDEX, &largest);
	es3_expect("max-element-index", (long)(largest > 0), 1L);

	/* Everything went without an error. */
	error = glGetError();
	es3_expect("start-glerror", (long)error, (long)GL_NO_ERROR);

	/* Succeeded: ready to draw. */
	printf("EGLTEST ES3 ready failures=%d\n", es3_failures);
	fflush(stdout);
	return 0;
}

/*
 * Draws the shapes over a window of a size: A, B and C one draw each from
 * their vertex array objects, D as four instances.
 */
void
egltest_es3_draw(
	int width,
	int height)
{
	/* The whole window, cleared to dark grey. */
	glViewport(0, 0, width, height);
	glClearColor(0.125f, 0.125f, 0.125f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glUseProgram(es3_program);
	glUniform2f(es3_step, 0.0f, 0.0f);

	/* A: palette colour 0 from the current value, the gain 1, the indices in the object's element buffer. */
	glVertexAttribI4i(ES3_INDEX, 0, 0, 0, 0);
	glVertexAttrib1f(ES3_GAIN, 1.0f);
	glBindVertexArray(es3_arrays[0]);
	glDrawRangeElements(GL_TRIANGLES, 0U, 3U, 6, GL_UNSIGNED_SHORT, NULL);

	/* B: a strip of the interleaved buffer. */
	glBindVertexArray(es3_arrays[1]);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	/* C: one strip, split in two by the restart index. */
	glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
	glBindVertexArray(es3_arrays[2]);
	glDrawElements(GL_TRIANGLE_STRIP, 9, GL_UNSIGNED_SHORT, NULL);
	glDisable(GL_PRIMITIVE_RESTART_FIXED_INDEX);

	/* D: four instances a step apart, their colours and gains from arrays with divisors. */
	glVertexAttribI4i(ES3_INDEX, -1, 0, 0, 0);
	glUniform2f(es3_step, 0.45f, 0.0f);
	glBindVertexArray(es3_arrays[3]);
	glDrawElementsInstanced(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_BYTE, NULL, 4);
	glBindVertexArray(0U);
}

/*
 * Reads back each shape's centre and the gap, and prints the colours;
 * returns how many differ, with the start's failed checks and any error.
 */
int
egltest_es3_check(
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
	failures = es3_failures;
	for (point = 0U; point < ES3_POINTS; point++) {
		x = (int)((es3_centres[point][0] + 1.0f) * 0.5f * (float)width);
		y = (int)((es3_centres[point][1] + 1.0f) * 0.5f * (float)height);
		memset(pixel, 0, sizeof(pixel));
		glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
		got = ((unsigned)pixel[0] << 16) | ((unsigned)pixel[1] << 8) | (unsigned)pixel[2];

		/* Each channel within 2 of the expected one. */
		same = es3_close(got, es3_expected[point]);
		verdict = "ok";
		if (!same) {
			verdict = "DIFFERS";
			failures++;
		}

		/* The point's line. */
		printf("EGLTEST PIXEL run=%s name=%s got=%06x expected=%06x %s\n", token, es3_names[point], got,
		       es3_expected[point], verdict);
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
es3_shader(
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
		printf("EGLTEST ES3 compile failed: %s\n", log);
		return 0U;
	}

	/* Succeeded: the shader. */
	return shader;
}

/*
 * Makes the palette buffer (the block a range at the offset alignment,
 * its colours written through a mapping and its offset and gain copied
 * from another buffer) and the tint buffer, and binds both.  Returns 0,
 * or -1 with a line when a mapping fails.
 */
static int
es3_buffers(void)
{
	static const GLfloat white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	GLuint staging;
	GLuint tint_index;
	GLfloat gain;
	GLint64 size;
	GLint value;
	void *mapped;
	GLboolean unmapped;

	/* The block's place: the offset alignment (at least 16 bytes) into the buffer. */
	es3_alignment = 0;
	glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &es3_alignment);
	if (es3_alignment < 16)
		es3_alignment = 16;

	/* The palette buffer, room for the alignment and the block. */
	glGenBuffers(1, &es3_palette);
	glBindBuffer(GL_UNIFORM_BUFFER, es3_palette);
	glBufferData(GL_UNIFORM_BUFFER, es3_alignment + ES3_PALETTE_SIZE, NULL, GL_DYNAMIC_DRAW);
	size = 0;
	glGetBufferParameteri64v(GL_UNIFORM_BUFFER, GL_BUFFER_SIZE, &size);
	es3_expect("buffer-size-64", (long)size, (long)(es3_alignment + ES3_PALETTE_SIZE));

	/* The colours, written through a mapping of their range. */
	mapped = glMapBufferRange(GL_UNIFORM_BUFFER, es3_alignment, (GLsizeiptr)sizeof(es3_colours),
				  GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
	if (mapped == NULL) {
		printf("EGLTEST ES3 glMapBufferRange for writing failed\n");
		return -1;
	}

	/* Written, then unmapped. */
	memcpy(mapped, es3_colours, sizeof(es3_colours));
	unmapped = glUnmapBuffer(GL_UNIFORM_BUFFER);
	es3_expect("unmap", (long)unmapped, (long)GL_TRUE);

	/* The offset and the gain, copied from a buffer of their own. */
	glGenBuffers(1, &staging);
	glBindBuffer(GL_COPY_READ_BUFFER, staging);
	glBufferData(GL_COPY_READ_BUFFER, (GLsizeiptr)sizeof(es3_tail), es3_tail, GL_STATIC_DRAW);
	glBindBuffer(GL_COPY_WRITE_BUFFER, es3_palette);
	glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, es3_alignment + ES3_PALETTE_TAIL,
			    (GLsizeiptr)sizeof(es3_tail));
	glDeleteBuffers(1, &staging);

	/* The gain reads back through a mapping for reading. */
	mapped = glMapBufferRange(GL_UNIFORM_BUFFER, es3_alignment + ES3_PALETTE_TAIL + 8, 4, GL_MAP_READ_BIT);
	if (mapped == NULL) {
		printf("EGLTEST ES3 glMapBufferRange for reading failed\n");
		return -1;
	}

	/* Read, then unmapped. */
	memcpy(&gain, mapped, sizeof(gain));
	(void)glUnmapBuffer(GL_UNIFORM_BUFFER);
	es3_expect("mapped-gain-x100", (long)(gain * 100.0f), 100L);

	/* The palette block's range at binding point 0 (every block reads 0 after the link). */
	glBindBufferRange(GL_UNIFORM_BUFFER, 0U, es3_palette, es3_alignment, ES3_PALETTE_SIZE);
	value = -1;
	glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, 0U, &value);
	es3_expect("indexed-binding", (long)value, (long)es3_palette);
	value = -1;
	glGetIntegeri_v(GL_UNIFORM_BUFFER_START, 0U, &value);
	es3_expect("indexed-start", (long)value, (long)es3_alignment);
	value = -1;
	glGetIntegeri_v(GL_UNIFORM_BUFFER_SIZE, 0U, &value);
	es3_expect("indexed-size", (long)value, (long)ES3_PALETTE_SIZE);

	/* The tint: white, the whole buffer at binding point 2, which the tint block reads. */
	glGenBuffers(1, &es3_tint);
	glBindBuffer(GL_UNIFORM_BUFFER, es3_tint);
	glBufferData(GL_UNIFORM_BUFFER, (GLsizeiptr)sizeof(white), white, GL_STATIC_DRAW);
	glBindBufferBase(GL_UNIFORM_BUFFER, ES3_TINT_BINDING, es3_tint);
	tint_index = glGetUniformBlockIndex(es3_program, "Tint");
	if (tint_index == GL_INVALID_INDEX) {
		printf("EGLTEST ES3 no Tint block\n");
		return -1;
	}

	/* The block reads the tint's binding point. */
	glUniformBlockBinding(es3_program, tint_index, ES3_TINT_BINDING);

	/* Succeeded: both blocks have their buffers. */
	return 0;
}

/*
 * Makes the four vertex array objects: A (positions and an element
 * buffer), B (an interleaved buffer with an unsigned byte colour index),
 * C (two quads in one strip with int colour indices and a restart index),
 * D (a quad, colours per instance, gains per two instances).
 */
static void
es3_arrays_make(void)
{
	static const GLfloat quad_a[8] = { -0.9f, 0.3f, -0.5f, 0.3f, -0.9f, 0.9f, -0.5f, 0.9f };
	static const GLushort elements_a[6] = { 0U, 1U, 2U, 2U, 1U, 3U };
	static const GLfloat quads_c[16] = {
		0.1f, 0.3f, 0.4f, 0.3f, 0.1f, 0.9f, 0.4f, 0.9f,
		0.5f, 0.3f, 0.9f, 0.3f, 0.5f, 0.9f, 0.9f, 0.9f
	};
	static const GLint indices_c[8] = { 2, 2, 2, 2, 3, 3, 3, 3 };
	static const GLushort elements_c[9] = { 0U, 1U, 2U, 3U, 0xffffU, 4U, 5U, 6U, 7U };
	static const GLfloat quad_d[8] = { -0.9f, -0.9f, -0.55f, -0.9f, -0.9f, -0.3f, -0.55f, -0.3f };
	static const GLubyte elements_d[4] = { 0U, 1U, 2U, 3U };
	static const GLfloat colours_d[16] = {
		1.0f, 0.0f, 1.0f, 1.0f,
		0.0f, 1.0f, 1.0f, 1.0f,
		1.0f, 1.0f, 1.0f, 1.0f,
		1.0f, 1.0f, 0.0f, 1.0f
	};
	static const GLfloat gains_d[2] = { 1.0f, 0.6f };
	unsigned char interleaved[4 * 12];
	GLfloat corner[2];
	GLuint buffers[10];
	unsigned vertex;

	/* The buffers and the objects. */
	glGenBuffers(10, buffers);
	glGenVertexArrays((GLsizei)ES3_ARRAYS, es3_arrays);

	/* A: positions and the element buffer, which the object keeps. */
	glBindVertexArray(es3_arrays[0]);
	glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(quad_a), quad_a, GL_STATIC_DRAW);
	glVertexAttribPointer(ES3_POSITION, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glEnableVertexAttribArray(ES3_POSITION);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[1]);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)sizeof(elements_a), elements_a, GL_STATIC_DRAW);

	/* B's vertices: two floats and an unsigned byte colour index (1), twelve bytes each. */
	memset(interleaved, 0, sizeof(interleaved));
	for (vertex = 0U; vertex < 4U; vertex++) {
		corner[0] = -0.4f;
		if ((vertex & 1U) != 0U)
			corner[0] = 0.0f;
		corner[1] = 0.3f;
		if (vertex >= 2U)
			corner[1] = 0.9f;
		memcpy(interleaved + vertex * 12U, corner, sizeof(corner));
		interleaved[vertex * 12U + 8U] = 1U;
	}

	/* B: the interleaved buffer, the index read as an integer. */
	glBindVertexArray(es3_arrays[1]);
	glBindBuffer(GL_ARRAY_BUFFER, buffers[2]);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(interleaved), interleaved, GL_STATIC_DRAW);
	glVertexAttribPointer(ES3_POSITION, 2, GL_FLOAT, GL_FALSE, 12, NULL);
	glEnableVertexAttribArray(ES3_POSITION);
	glVertexAttribIPointer(ES3_INDEX, 1, GL_UNSIGNED_BYTE, 12, (const void *)8);
	glEnableVertexAttribArray(ES3_INDEX);

	/* C: positions, int colour indices, and elements with a restart index between the quads. */
	glBindVertexArray(es3_arrays[2]);
	glBindBuffer(GL_ARRAY_BUFFER, buffers[3]);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(quads_c), quads_c, GL_STATIC_DRAW);
	glVertexAttribPointer(ES3_POSITION, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glEnableVertexAttribArray(ES3_POSITION);
	glBindBuffer(GL_ARRAY_BUFFER, buffers[4]);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(indices_c), indices_c, GL_STATIC_DRAW);
	glVertexAttribIPointer(ES3_INDEX, 1, GL_INT, 0, NULL);
	glEnableVertexAttribArray(ES3_INDEX);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[5]);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)sizeof(elements_c), elements_c, GL_STATIC_DRAW);

	/* D: the quad, the colours per instance and the gains per two instances. */
	glBindVertexArray(es3_arrays[3]);
	glBindBuffer(GL_ARRAY_BUFFER, buffers[6]);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(quad_d), quad_d, GL_STATIC_DRAW);
	glVertexAttribPointer(ES3_POSITION, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glEnableVertexAttribArray(ES3_POSITION);
	glBindBuffer(GL_ARRAY_BUFFER, buffers[7]);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(colours_d), colours_d, GL_STATIC_DRAW);
	glVertexAttribPointer(ES3_COLOUR, 4, GL_FLOAT, GL_FALSE, 0, NULL);
	glVertexAttribDivisor(ES3_COLOUR, 1U);
	glEnableVertexAttribArray(ES3_COLOUR);
	glBindBuffer(GL_ARRAY_BUFFER, buffers[8]);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(gains_d), gains_d, GL_STATIC_DRAW);
	glVertexAttribPointer(ES3_GAIN, 1, GL_FLOAT, GL_FALSE, 0, NULL);
	glVertexAttribDivisor(ES3_GAIN, 2U);
	glEnableVertexAttribArray(ES3_GAIN);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[9]);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)sizeof(elements_d), elements_d, GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0U);
}

/* Prints one of the start's checks, counting it when the value is not the one expected. */
static void
es3_expect(
	const char *what,
	long got,
	long expected)
{
	const char *verdict;

	/* A value that differs is a failure. */
	verdict = "ok";
	if (got != expected) {
		verdict = "FAILED";
		es3_failures++;
	}

	/* The line. */
	printf("EGLTEST ES3 check %s got=%ld expected=%ld %s\n", what, got, expected, verdict);
}

/*
 * Checks what the API reports of the program's uniform blocks and their
 * members.  Returns 0, or -1 with a line when a block is missing.
 */
static int
es3_blocks_check(void)
{
	static const GLchar *const names[3] = { "gain", "colours[0]", "Tint.tint" };
	GLuint palette;
	GLuint tint;
	GLuint indices[3];
	GLint values[3];
	GLint value;
	GLint count;
	char name[32];
	GLsizei length;
	unsigned member;

	/* Both blocks, by name. */
	palette = glGetUniformBlockIndex(es3_program, "Palette");
	tint = glGetUniformBlockIndex(es3_program, "Tint");
	if (palette == GL_INVALID_INDEX || tint == GL_INVALID_INDEX) {
		printf("EGLTEST ES3 blocks missing: %u %u\n", palette, tint);
		return -1;
	}

	/* How many, and the palette's name read back. */
	count = 0;
	glGetProgramiv(es3_program, GL_ACTIVE_UNIFORM_BLOCKS, &count);
	es3_expect("active-blocks", (long)count, 2L);
	name[0] = '\0';
	length = 0;
	glGetActiveUniformBlockName(es3_program, palette, (GLsizei)sizeof(name), &length, name);
	es3_expect("block-name-length", (long)length, 7L);

	/* The palette's std140 size, members and stage. */
	value = 0;
	glGetActiveUniformBlockiv(es3_program, palette, GL_UNIFORM_BLOCK_DATA_SIZE, &value);
	es3_expect("palette-size", (long)value, (long)ES3_PALETTE_SIZE);
	value = 0;
	glGetActiveUniformBlockiv(es3_program, palette, GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS, &value);
	es3_expect("palette-members", (long)value, 3L);
	value = 0;
	glGetActiveUniformBlockiv(es3_program, palette, GL_UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER, &value);
	es3_expect("palette-in-vertex", (long)value, 1L);

	/* The tint's size and stage. */
	value = 0;
	glGetActiveUniformBlockiv(es3_program, tint, GL_UNIFORM_BLOCK_DATA_SIZE, &value);
	es3_expect("tint-size", (long)value, 16L);
	value = 0;
	glGetActiveUniformBlockiv(es3_program, tint, GL_UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER, &value);
	es3_expect("tint-in-fragment", (long)value, 1L);

	/* Three members by name, each found. */
	glGetUniformIndices(es3_program, 3, names, indices);
	for (member = 0U; member < 3U; member++) {
		es3_expect(names[member], (long)(indices[member] != GL_INVALID_INDEX), 1L);
		if (indices[member] == GL_INVALID_INDEX)
			return 0;
	}

	/* Their offsets, blocks and the colours' stride. */
	glGetActiveUniformsiv(es3_program, 3, indices, GL_UNIFORM_OFFSET, values);
	es3_expect("gain-offset", (long)values[0], 72L);
	es3_expect("colours-offset", (long)values[1], 0L);
	es3_expect("tint-offset", (long)values[2], 0L);
	glGetActiveUniformsiv(es3_program, 3, indices, GL_UNIFORM_ARRAY_STRIDE, values);
	es3_expect("colours-stride", (long)values[1], 16L);
	glGetActiveUniformsiv(es3_program, 3, indices, GL_UNIFORM_BLOCK_INDEX, values);
	es3_expect("tint-block", (long)values[2], (long)tint);

	/* Succeeded: both blocks are there. */
	return 0;
}

/* Reports whether two 0xRRGGBB colours are within 2 in every channel. */
static int
es3_close(
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
