/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libGL's calls of OpenGL 4.3 that its extensions bring to earlier
 * contexts: the vertex attribute binding (GL_ARB_vertex_attrib_binding).
 * Each attribute index is also a binding point (a buffer, an offset, a
 * stride and a divisor); an attribute takes its format and an offset in
 * the elements of the binding point it reads.  The calls check the index
 * and the vertex array object as desktop GL does; the rest is libGLESv2's
 * draw.c, which OpenGL ES 3.1 shares.
 */

#include "fixed.h"

static struct gles_state *gl43_state(struct zegl_context *context, GLuint index);

/*
 * Binds a buffer object's elements, from an offset and a stride apart, to
 * a vertex buffer binding point (buffer 0: none).
 */
GL_APICALL void GL_APIENTRY
glBindVertexBuffer(
	GLuint bindingindex,
	GLuint buffer,
	GLintptr offset,
	GLsizei stride)
{
	struct zegl_context *context;
	struct gles_state *state;

	/* A context with its state and a binding point. */
	context = gles_context();
	state = gl43_state(context, bindingindex);
	if (state == NULL)
		return;

	/* The binding point's arrays. */
	gles_bind_vertex_buffer(context, state, bindingindex, buffer, offset, stride);
}

/*
 * Sets the format of an attribute's elements, read as floats, and their
 * offset in its binding point's elements.
 */
GL_APICALL void GL_APIENTRY
glVertexAttribFormat(
	GLuint attribindex,
	GLint size,
	GLenum type,
	GLboolean normalized,
	GLuint relativeoffset)
{
	struct zegl_context *context;
	struct gles_state *state;

	/* A context with its state and an attribute. */
	context = gles_context();
	state = gl43_state(context, attribindex);
	if (state == NULL)
		return;

	/* The format. */
	gles_vertex_attrib_format(context, state, attribindex, size, type, normalized, 0, relativeoffset);
}

/*
 * Sets the format of an attribute's elements, integers the shader reads
 * as they are, and their offset in its binding point's elements.
 */
GL_APICALL void GL_APIENTRY
glVertexAttribIFormat(
	GLuint attribindex,
	GLint size,
	GLenum type,
	GLuint relativeoffset)
{
	struct zegl_context *context;
	struct gles_state *state;

	/* A context with its state and an attribute. */
	context = gles_context();
	state = gl43_state(context, attribindex);
	if (state == NULL)
		return;

	/* The format. */
	gles_vertex_attrib_format(context, state, attribindex, size, type, GL_FALSE, 1, relativeoffset);
}

/*
 * Makes an attribute read the elements of a binding point.
 */
GL_APICALL void GL_APIENTRY
glVertexAttribBinding(
	GLuint attribindex,
	GLuint bindingindex)
{
	struct zegl_context *context;
	struct gles_state *state;

	/* A context with its state and an attribute. */
	context = gles_context();
	state = gl43_state(context, attribindex);
	if (state == NULL)
		return;

	/* The binding point it reads. */
	gles_vertex_attrib_binding(context, state, attribindex, bindingindex);
}

/*
 * Makes a binding point's elements advance once every divisor instances
 * (0: once per vertex).
 */
GL_APICALL void GL_APIENTRY
glVertexBindingDivisor(
	GLuint bindingindex,
	GLuint divisor)
{
	struct zegl_context *context;
	struct gles_state *state;

	/* A context with its state and a binding point. */
	context = gles_context();
	state = gl43_state(context, bindingindex);
	if (state == NULL)
		return;

	/* The divisor. */
	gles_vertex_binding_divisor(state, bindingindex, divisor);
}

/*
 * Returns the current context's state for a call on an attribute or a
 * binding point of an index; NULL without a context, or with the error
 * recorded for an index past the last (GL_INVALID_VALUE) or, in a core
 * profile, the default vertex array object (GL_INVALID_OPERATION).
 */
static struct gles_state *
gl43_state(
	struct zegl_context *context,
	GLuint index)
{
	struct gles_state *state;
	GLint profile;

	/* A context with its state. */
	state = gles_state(context);
	if (state == NULL)
		return NULL;

	/* An index there is. */
	if (index >= GLES_ATTRIBS) {
		gles_error(context, GL_INVALID_VALUE);
		return NULL;
	}

	/* A core profile has no default vertex array object to change. */
	profile = gl_context_profile();
	if ((profile & GL_CONTEXT_CORE_PROFILE_BIT) != 0 && state->vertex_array == 0U) {
		gles_error(context, GL_INVALID_OPERATION);
		return NULL;
	}

	/* Succeeded: the state. */
	return state;
}
