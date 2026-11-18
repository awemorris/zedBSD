/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libGL's calls of OpenGL 1.5 to 3.1 that OpenGL ES 3.0 does not have
 * (WS068 p013, p031), made of OpenGL ES 3.0's: a uniform's name alone,
 * the integer vertex attributes of
 * fewer than four components and of small types, the texture parameters
 * of integer border colours, the framebuffer attachments of 1D and 3D
 * textures, a whole buffer mapped and a buffer range read, a texture
 * level read back, the colour clamping, and an occlusion query's result
 * as a signed integer.
 *
 * There are no 1D textures and no border colours: a 1D attachment only
 * detaches, and the border colour is refused as OpenGL ES 3.0 refuses it.
 */

#include "fixed.h"

#include <string.h>

static struct gles_texture *gl3_texture(struct gles_state *state, GLenum target, unsigned *face, int *layered);
static int gl3_read_level(GLenum target, GLuint name, GLint level, GLint layer, int layered, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels);

/*
 * Returns the name of an active uniform of a program (desktop GL 3.1).
 */
GL_APICALL void GL_APIENTRY
glGetActiveUniformName(
	GLuint program,
	GLuint uniformIndex,
	GLsizei bufSize,
	GLsizei *length,
	GLchar *uniformName)
{
	GLint size;
	GLenum type;

	/* The uniform's description, of which only the name is kept. */
	size = 0;
	type = GL_NONE;
	glGetActiveUniform(program, uniformIndex, bufSize, length, &size, &type, uniformName);
}

/*
 * Sets an integer vertex attribute's current value from one integer.
 */
GL_APICALL void GL_APIENTRY
glVertexAttribI1i(
	GLuint index,
	GLint x)
{
	/* The missing components are 0, and w 1. */
	glVertexAttribI4i(index, x, 0, 0, 1);
}

/*
 * Sets an integer vertex attribute's current value from two integers.
 */
GL_APICALL void GL_APIENTRY
glVertexAttribI2i(
	GLuint index,
	GLint x,
	GLint y)
{
	/* The missing components are 0, and w 1. */
	glVertexAttribI4i(index, x, y, 0, 1);
}

/*
 * Sets an integer vertex attribute's current value from three integers.
 */
GL_APICALL void GL_APIENTRY
glVertexAttribI3i(
	GLuint index,
	GLint x,
	GLint y,
	GLint z)
{
	/* w is 1. */
	glVertexAttribI4i(index, x, y, z, 1);
}

/*
 * Sets an unsigned integer vertex attribute's current value from one
 * integer.
 */
GL_APICALL void GL_APIENTRY
glVertexAttribI1ui(
	GLuint index,
	GLuint x)
{
	/* The missing components are 0, and w 1. */
	glVertexAttribI4ui(index, x, 0U, 0U, 1U);
}

/*
 * Sets an unsigned integer vertex attribute's current value from two
 * integers.
 */
GL_APICALL void GL_APIENTRY
glVertexAttribI2ui(
	GLuint index,
	GLuint x,
	GLuint y)
{
	/* The missing components are 0, and w 1. */
	glVertexAttribI4ui(index, x, y, 0U, 1U);
}

/*
 * Sets an unsigned integer vertex attribute's current value from three
 * integers.
 */
GL_APICALL void GL_APIENTRY
glVertexAttribI3ui(
	GLuint index,
	GLuint x,
	GLuint y,
	GLuint z)
{
	/* w is 1. */
	glVertexAttribI4ui(index, x, y, z, 1U);
}

/*
 * Sets an integer vertex attribute's current value from an array of one.
 */
GL_APICALL void GL_APIENTRY
glVertexAttribI1iv(
	GLuint index,
	const GLint *v)
{
	/* The missing components are 0, and w 1. */
	glVertexAttribI4i(index, v[0], 0, 0, 1);
}

/*
 * Sets an integer vertex attribute's current value from an array of two.
 */
GL_APICALL void GL_APIENTRY
glVertexAttribI2iv(
	GLuint index,
	const GLint *v)
{
	/* The missing components are 0, and w 1. */
	glVertexAttribI4i(index, v[0], v[1], 0, 1);
}

/*
 * Sets an integer vertex attribute's current value from an array of three.
 */
GL_APICALL void GL_APIENTRY
glVertexAttribI3iv(
	GLuint index,
	const GLint *v)
{
	/* w is 1. */
	glVertexAttribI4i(index, v[0], v[1], v[2], 1);
}

/*
 * Sets an unsigned integer vertex attribute's current value from an
 * array of one.
 */
GL_APICALL void GL_APIENTRY
glVertexAttribI1uiv(
	GLuint index,
	const GLuint *v)
{
	/* The missing components are 0, and w 1. */
	glVertexAttribI4ui(index, v[0], 0U, 0U, 1U);
}

/*
 * Sets an unsigned integer vertex attribute's current value from an
 * array of two.
 */
GL_APICALL void GL_APIENTRY
glVertexAttribI2uiv(
	GLuint index,
	const GLuint *v)
{
	/* The missing components are 0, and w 1. */
	glVertexAttribI4ui(index, v[0], v[1], 0U, 1U);
}

/*
 * Sets an unsigned integer vertex attribute's current value from an
 * array of three.
 */
GL_APICALL void GL_APIENTRY
glVertexAttribI3uiv(
	GLuint index,
	const GLuint *v)
{
	/* w is 1. */
	glVertexAttribI4ui(index, v[0], v[1], v[2], 1U);
}

/*
 * Sets an integer vertex attribute's current value from four bytes.
 */
GL_APICALL void GL_APIENTRY
glVertexAttribI4bv(
	GLuint index,
	const GLbyte *v)
{
	/* Each byte widened with its sign. */
	glVertexAttribI4i(index, v[0], v[1], v[2], v[3]);
}

/*
 * Sets an integer vertex attribute's current value from four shorts.
 */
GL_APICALL void GL_APIENTRY
glVertexAttribI4sv(
	GLuint index,
	const GLshort *v)
{
	/* Each short widened with its sign. */
	glVertexAttribI4i(index, v[0], v[1], v[2], v[3]);
}

/*
 * Sets an unsigned integer vertex attribute's current value from four
 * unsigned bytes.
 */
GL_APICALL void GL_APIENTRY
glVertexAttribI4ubv(
	GLuint index,
	const GLubyte *v)
{
	/* Each byte widened. */
	glVertexAttribI4ui(index, v[0], v[1], v[2], v[3]);
}

/*
 * Sets an unsigned integer vertex attribute's current value from four
 * unsigned shorts.
 */
GL_APICALL void GL_APIENTRY
glVertexAttribI4usv(
	GLuint index,
	const GLushort *v)
{
	/* Each short widened. */
	glVertexAttribI4ui(index, v[0], v[1], v[2], v[3]);
}

/*
 * Sets texture parameters from signed integers (a border colour's is
 * refused: there are no border colours).
 */
GL_APICALL void GL_APIENTRY
glTexParameterIiv(
	GLenum target,
	GLenum pname,
	const GLint *params)
{
	/* The integer parameters are the same as glTexParameteriv's. */
	glTexParameteriv(target, pname, params);
}

/*
 * Sets texture parameters from unsigned integers.
 */
GL_APICALL void GL_APIENTRY
glTexParameterIuiv(
	GLenum target,
	GLenum pname,
	const GLuint *params)
{
	GLint value;

	/* One value (the only parameters of more are border colours, which are refused). */
	value = (GLint)params[0];
	glTexParameteriv(target, pname, &value);
}

/*
 * Reports texture parameters as signed integers.
 */
GL_APICALL void GL_APIENTRY
glGetTexParameterIiv(
	GLenum target,
	GLenum pname,
	GLint *params)
{
	/* The same as glGetTexParameteriv's. */
	glGetTexParameteriv(target, pname, params);
}

/*
 * Reports texture parameters as unsigned integers.
 */
GL_APICALL void GL_APIENTRY
glGetTexParameterIuiv(
	GLenum target,
	GLenum pname,
	GLuint *params)
{
	/* The same as glGetTexParameteriv's, whose values are not negative. */
	glGetTexParameteriv(target, pname, (GLint *)params);
}

/*
 * Attaches a level of a 1D texture to a framebuffer: there are no 1D
 * textures, so only detaching (texture 0) is done.
 */
GL_APICALL void GL_APIENTRY
glFramebufferTexture1D(
	GLenum target,
	GLenum attachment,
	GLenum textarget,
	GLuint texture,
	GLint level)
{
	struct zegl_context *context;

	/* Detaching, as a 2D attachment of nothing. */
	(void)textarget;
	if (texture == 0U) {
		glFramebufferTexture2D(target, attachment, GL_TEXTURE_2D, 0U, level);
		return;
	}

	/* A texture, which cannot be a 1D one. */
	context = gles_context();
	if (context != NULL)
		gles_error(context, GL_INVALID_OPERATION);
}

/*
 * Attaches a slice of a level of a 3D texture to a framebuffer.
 */
GL_APICALL void GL_APIENTRY
glFramebufferTexture3D(
	GLenum target,
	GLenum attachment,
	GLenum textarget,
	GLuint texture,
	GLint level,
	GLint zoffset)
{
	struct zegl_context *context;

	/* A texture named with the 3D target. */
	if (texture != 0U && textarget != GL_TEXTURE_3D) {
		context = gles_context();
		if (context != NULL)
			gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* The slice as a layer. */
	glFramebufferTextureLayer(target, attachment, texture, level, zoffset);
}

/*
 * Maps the whole of a buffer for reading, writing or both.
 */
GL_APICALL void *GL_APIENTRY
glMapBuffer(
	GLenum target,
	GLenum access)
{
	struct zegl_context *context;
	GLbitfield bits;
	GLint64 size;
	void *mapped;

	/* The access as glMapBufferRange's bits. */
	context = gles_context();
	if (context == NULL)
		return NULL;
	switch (access) {
	case GL_READ_ONLY:
		bits = GL_MAP_READ_BIT;
		break;
	case GL_WRITE_ONLY:
		bits = GL_MAP_WRITE_BIT;
		break;
	case GL_READ_WRITE:
		bits = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT;
		break;
	default:
		gles_error(context, GL_INVALID_ENUM);
		return NULL;
	}

	/* The buffer's size (an empty buffer has nothing to map). */
	size = 0;
	glGetBufferParameteri64v(target, GL_BUFFER_SIZE, &size);
	if (size <= 0)
		return NULL;

	/* The whole range. */
	mapped = glMapBufferRange(target, 0, (GLsizeiptr)size, bits);
	if (mapped == NULL)
		return NULL;

	/* Succeeded: the buffer's bytes. */
	return mapped;
}

/*
 * Copies a range of a buffer's bytes out.
 */
GL_APICALL void GL_APIENTRY
glGetBufferSubData(
	GLenum target,
	GLintptr offset,
	GLsizeiptr size,
	void *data)
{
	void *mapped;

	/* Nothing to copy. */
	if (size == 0)
		return;

	/* The range mapped for reading (glMapBufferRange refuses a bad range or a mapped buffer). */
	mapped = glMapBufferRange(target, offset, size, GL_MAP_READ_BIT);
	if (mapped == NULL)
		return;

	/* Copied, and unmapped. */
	memcpy(data, mapped, (size_t)size);
	(void)glUnmapBuffer(target);
}

/*
 * Reads a level of the bound texture of a target back as pixels of a
 * format and type (every slice of a 3D texture and every layer of an
 * array, one after the other): the level is attached to a framebuffer of
 * its own and read with glReadPixels, so the pack state and a pixel pack
 * buffer apply, and only colour formats that can be drawn into are read.
 */
GL_APICALL void GL_APIENTRY
glGetTexImage(
	GLenum target,
	GLint level,
	GLenum format,
	GLenum type,
	void *pixels)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_texture *texture;
	struct gles_level *image;
	unsigned char *place;
	size_t pixel;
	size_t row;
	size_t stride;
	GLint layer;
	unsigned face;
	int layered;
	int status;

	/* A context with its state, and a texture of the target bound. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	texture = gl3_texture(state, target, &face, &layered);
	if (texture == NULL || target == GL_TEXTURE_2D_MULTISAMPLE) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* A level there can be. */
	if (level < 0 || level >= (GLint)GLES_LEVELS) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* The level, specified (an unspecified one has no pixels). */
	image = &texture->levels[face * GLES_LEVELS + (unsigned)level];
	if (image->width == 0 || image->format == NULL)
		return;

	/* A colour format with a type (depth and stencil cannot be read). */
	pixel = gles_pixel_size(format, type);
	if (pixel == 0U ||
	    format == GL_DEPTH_COMPONENT ||
	    format == GL_DEPTH_STENCIL) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* The bytes of one slice as glReadPixels packs it: rows of the row length, aligned. */
	row = (size_t)image->width * pixel;
	if (state->pack_row_length > 0)
		row = (size_t)state->pack_row_length * pixel;
	row = (row + (size_t)state->pack_alignment - 1U) / (size_t)state->pack_alignment * (size_t)state->pack_alignment;
	stride = row * (size_t)image->height;

	/* Each slice or layer (one of a 2D texture's level and a cube map's face), after the one before. */
	place = pixels;
	for (layer = 0; layer < image->depth; layer++) {
		status = gl3_read_level(target, texture->name, level, layer, layered, image->width, image->height, format, type, place);
		if (status != 0) {
			gles_error(context, GL_INVALID_OPERATION);
			return;
		}

		/* The next one's place. */
		place += stride;
	}
}

/*
 * Sets whether colours are clamped: nothing changes (reads of float
 * buffers are never clamped, and colours are clamped by the format they
 * are written in).
 */
GL_APICALL void GL_APIENTRY
glClampColor(
	GLenum target,
	GLenum clamp)
{
	struct zegl_context *context;

	/* A context, and a target there is. */
	context = gles_context();
	if (context == NULL)
		return;
	if (target != GL_CLAMP_READ_COLOR &&
	    target != GL_CLAMP_VERTEX_COLOR &&
	    target != GL_CLAMP_FRAGMENT_COLOR) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* A setting there is. */
	if (clamp != GL_TRUE &&
	    clamp != GL_FALSE &&
	    clamp != GL_FIXED_ONLY)
		gles_error(context, GL_INVALID_ENUM);
}

/*
 * Reports an ended query's result, or whether it is available, as a
 * signed integer.
 */
GL_APICALL void GL_APIENTRY
glGetQueryObjectiv(
	GLuint id,
	GLenum pname,
	GLint *params)
{
	GLuint value;

	/* The unsigned answer (left as it was when the call is refused). */
	value = (GLuint)params[0];
	glGetQueryObjectuiv(id, pname, &value);
	params[0] = (GLint)value;
}

/*
 * Returns the texture bound on the active unit for glGetTexImage's
 * target, with the face it means and whether its levels have layers
 * (3D and 2D array textures); NULL for a target that is not one or has
 * no texture bound.
 */
static struct gles_texture *
gl3_texture(
	struct gles_state *state,
	GLenum target,
	unsigned *face,
	int *layered)
{
	/* The unit's texture of the target. */
	*face = 0U;
	*layered = 0;
	switch (target) {
	case GL_TEXTURE_2D:
		return state->units[state->active_unit];
	case GL_TEXTURE_CUBE_MAP_POSITIVE_X:
	case GL_TEXTURE_CUBE_MAP_NEGATIVE_X:
	case GL_TEXTURE_CUBE_MAP_POSITIVE_Y:
	case GL_TEXTURE_CUBE_MAP_NEGATIVE_Y:
	case GL_TEXTURE_CUBE_MAP_POSITIVE_Z:
	case GL_TEXTURE_CUBE_MAP_NEGATIVE_Z:
		*face = target - GL_TEXTURE_CUBE_MAP_POSITIVE_X;
		return state->cube_units[state->active_unit];
	case GL_TEXTURE_3D:
		*layered = 1;
		return state->volume_units[state->active_unit];
	case GL_TEXTURE_2D_ARRAY:
		*layered = 1;
		return state->array_units[state->active_unit];
	case GL_TEXTURE_CUBE_MAP_ARRAY:
		*layered = 1;
		return state->cube_array_units[state->active_unit];
	case GL_TEXTURE_RECTANGLE:
		return state->rect_units[state->active_unit];
	case GL_TEXTURE_2D_MULTISAMPLE:
		return state->ms_units[state->active_unit];
	default:
		break;
	}

	/* Not a target glGetTexImage reads. */
	return NULL;
}

/*
 * Reads one layer of a texture level into pixels through a framebuffer
 * of its own, the read framebuffer and read buffer given back afterwards.
 * Returns 0, or -1 when the level cannot be read (a format that cannot
 * be drawn into).
 */
static int
gl3_read_level(
	GLenum target,
	GLuint name,
	GLint level,
	GLint layer,
	int layered,
	GLsizei width,
	GLsizei height,
	GLenum format,
	GLenum type,
	void *pixels)
{
	GLint read_framebuffer;
	GLuint framebuffer;
	GLenum complete;

	/* The read framebuffer bound now. */
	read_framebuffer = 0;
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_framebuffer);

	/* A framebuffer of its own, bound for reading. */
	framebuffer = 0U;
	glGenFramebuffers(1, &framebuffer);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);

	/* The layer of a 3D or array texture's level, or the level (of a cube map's face), as its colour attachment, read from. */
	if (layered) {
		glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, name, level, layer);
	} else {
		glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target, name, level);
	}

	/* Read from that attachment. */
	glReadBuffer(GL_COLOR_ATTACHMENT0);

	/* Read when the level can be a colour attachment. */
	complete = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
	if (complete == GL_FRAMEBUFFER_COMPLETE)
		glReadPixels(0, 0, width, height, format, type, pixels);

	/* The application's read framebuffer bound again (with its read buffer, which it keeps), and this one gone. */
	glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_framebuffer);
	glDeleteFramebuffers(1, &framebuffer);

	/* A level that could not be read. */
	if (complete != GL_FRAMEBUFFER_COMPLETE)
		return -1;

	/* Succeeded: the layer's pixels. */
	return 0;
}
