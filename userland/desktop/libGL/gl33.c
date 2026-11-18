/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libGL's calls of OpenGL 3.3 that OpenGL ES 3.0 does not have and that
 * the translation's own files do not hold: a vertex attribute's current
 * value from a packed 2_10_10_10 word (GL_ARB_vertex_type_2_10_10_10_rev).
 * The packed arrays themselves (glVertexAttribPointer with the packed
 * types) are OpenGL ES 3.0's already.
 *
 * The timer queries are query.c's, dual-source blending program.c's and
 * draw.c's.
 */

#include "fixed.h"

static int gl33_unpack(GLenum type, GLboolean normalized, GLuint value, GLfloat *components);
static void gl33_attrib(GLuint index, GLenum type, GLboolean normalized, GLuint value, unsigned count);

/*
 * Sets a vertex attribute's current value from a packed word's x
 * component.
 */
GL_APICALL void GL_APIENTRY
glVertexAttribP1ui(
	GLuint index,
	GLenum type,
	GLboolean normalized,
	GLuint value)
{
	/* One component, the rest 0 and w 1. */
	gl33_attrib(index, type, normalized, value, 1U);
}

/*
 * Sets a vertex attribute's current value from a packed word's x and y.
 */
GL_APICALL void GL_APIENTRY
glVertexAttribP2ui(
	GLuint index,
	GLenum type,
	GLboolean normalized,
	GLuint value)
{
	/* Two components. */
	gl33_attrib(index, type, normalized, value, 2U);
}

/*
 * Sets a vertex attribute's current value from a packed word's x, y and
 * z.
 */
GL_APICALL void GL_APIENTRY
glVertexAttribP3ui(
	GLuint index,
	GLenum type,
	GLboolean normalized,
	GLuint value)
{
	/* Three components. */
	gl33_attrib(index, type, normalized, value, 3U);
}

/*
 * Sets a vertex attribute's current value from a packed word's four
 * components.
 */
GL_APICALL void GL_APIENTRY
glVertexAttribP4ui(
	GLuint index,
	GLenum type,
	GLboolean normalized,
	GLuint value)
{
	/* All four. */
	gl33_attrib(index, type, normalized, value, 4U);
}

/*
 * Sets a vertex attribute's current value from a packed word's x
 * component, the word read through a pointer.
 */
GL_APICALL void GL_APIENTRY
glVertexAttribP1uiv(
	GLuint index,
	GLenum type,
	GLboolean normalized,
	const GLuint *value)
{
	/* The word it points to. */
	gl33_attrib(index, type, normalized, value[0], 1U);
}

/*
 * Sets a vertex attribute's current value from a packed word's x and y,
 * the word read through a pointer.
 */
GL_APICALL void GL_APIENTRY
glVertexAttribP2uiv(
	GLuint index,
	GLenum type,
	GLboolean normalized,
	const GLuint *value)
{
	/* The word it points to. */
	gl33_attrib(index, type, normalized, value[0], 2U);
}

/*
 * Sets a vertex attribute's current value from a packed word's x, y and
 * z, the word read through a pointer.
 */
GL_APICALL void GL_APIENTRY
glVertexAttribP3uiv(
	GLuint index,
	GLenum type,
	GLboolean normalized,
	const GLuint *value)
{
	/* The word it points to. */
	gl33_attrib(index, type, normalized, value[0], 3U);
}

/*
 * Sets a vertex attribute's current value from a packed word's four
 * components, the word read through a pointer.
 */
GL_APICALL void GL_APIENTRY
glVertexAttribP4uiv(
	GLuint index,
	GLenum type,
	GLboolean normalized,
	const GLuint *value)
{
	/* The word it points to. */
	gl33_attrib(index, type, normalized, value[0], 4U);
}

/*
 * Unpacks a 2_10_10_10 word into its four components (x in the low ten
 * bits, w in the top two): as integers, or normalized, the signed ones by
 * OpenGL 4.2's rule (c / (2^(b-1) - 1), at least -1).  Returns 0, or -1
 * for a type that is not a packed one.
 */
static int
gl33_unpack(
	GLenum type,
	GLboolean normalized,
	GLuint value,
	GLfloat *components)
{
	static const unsigned shifts[4] = { 0U, 10U, 20U, 30U };
	static const unsigned bits[4] = { 10U, 10U, 10U, 2U };
	GLuint field;
	GLint signed_field;
	GLfloat largest;
	unsigned index;

	/* The two packed types. */
	if (type != GL_INT_2_10_10_10_REV && type != GL_UNSIGNED_INT_2_10_10_10_REV)
		return -1;

	/* Each component's field. */
	for (index = 0U; index < 4U; index++) {
		field = (value >> shifts[index]) & ((1U << bits[index]) - 1U);

		/* An unsigned one: as it is, or over its largest value. */
		if (type == GL_UNSIGNED_INT_2_10_10_10_REV) {
			components[index] = (GLfloat)field;
			if (normalized)
				components[index] = (GLfloat)field / (GLfloat)((1U << bits[index]) - 1U);
			continue;
		}

		/* A signed one: its sign extended. */
		signed_field = (GLint)field;
		if ((field & (1U << (bits[index] - 1U))) != 0U)
			signed_field -= (GLint)(1U << bits[index]);
		components[index] = (GLfloat)signed_field;

		/* Normalized, the most negative value clamped to -1. */
		if (normalized) {
			largest = (GLfloat)((1U << (bits[index] - 1U)) - 1U);
			components[index] = (GLfloat)signed_field / largest;
			if (components[index] < -1.0f)
				components[index] = -1.0f;
		}
	}

	/* Succeeded: the four components. */
	return 0;
}

/* Sets a vertex attribute's current value from the first components of a packed word, the others 0 and w 1. */
static void
gl33_attrib(
	GLuint index,
	GLenum type,
	GLboolean normalized,
	GLuint value,
	unsigned count)
{
	struct zegl_context *context;
	GLfloat components[4];
	unsigned component;
	int status;

	/* The word unpacked. */
	status = gl33_unpack(type, normalized, value, components);
	if (status != 0) {
		context = gles_context();
		if (context != NULL)
			gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* The components not given are 0, and w 1. */
	for (component = count; component < 3U; component++)
		components[component] = 0.0f;
	if (count < 4U)
		components[3] = 1.0f;

	/* The current value. */
	glVertexAttrib4f(index, components[0], components[1], components[2], components[3]);
}
