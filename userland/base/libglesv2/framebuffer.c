/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Framebuffer and renderbuffer objects of zedBSD's OpenGL ES (WS068
 * p008): names only.  Rendering into textures and renderbuffers comes
 * with a later Phase; until then a framebuffer other than 0 reports
 * GL_FRAMEBUFFER_UNSUPPORTED and draws into it fail with
 * GL_INVALID_FRAMEBUFFER_OPERATION.
 */

#include "gles.h"

/*
 * Makes framebuffer names.
 */
GL_APICALL void GL_APIENTRY
glGenFramebuffers(
	GLsizei n,
	GLuint *framebuffers)
{
	struct zegl_context *context;
	struct gles_state *state;
	GLsizei index;

	/* A context with its state and a count. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (n < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Names counted up from 1. */
	for (index = 0; index < n; index++)
		framebuffers[index] = ++state->next_framebuffer;
}

/*
 * Deletes framebuffer names, binding 0 in place of a deleted one.
 */
GL_APICALL void GL_APIENTRY
glDeleteFramebuffers(
	GLsizei n,
	const GLuint *framebuffers)
{
	struct gles_state *state;
	GLsizei index;

	/* A context with its state. */
	state = gles_state(gles_context());
	if (state == NULL)
		return;

	/* The bound one goes back to 0. */
	for (index = 0; index < n; index++) {
		if (framebuffers[index] != 0U && framebuffers[index] == state->framebuffer)
			state->framebuffer = 0U;
	}
}

/*
 * Binds a framebuffer (0: the window's).
 */
GL_APICALL void GL_APIENTRY
glBindFramebuffer(
	GLenum target,
	GLuint framebuffer)
{
	struct zegl_context *context;
	struct gles_state *state;

	/* A context with its state and the one target. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (target != GL_FRAMEBUFFER) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* Bound. */
	state->framebuffer = framebuffer;
}

/*
 * Reports whether a name is a framebuffer.
 */
GL_APICALL GLboolean GL_APIENTRY
glIsFramebuffer(
	GLuint framebuffer)
{
	struct gles_state *state;

	/* A name that was made. */
	state = gles_state(gles_context());
	if (state == NULL || framebuffer == 0U || framebuffer > state->next_framebuffer)
		return GL_FALSE;
	return GL_TRUE;
}

/*
 * Reports whether the bound framebuffer can be drawn into: only the
 * window's can.
 */
GL_APICALL GLenum GL_APIENTRY
glCheckFramebufferStatus(
	GLenum target)
{
	struct gles_state *state;

	/* The window's framebuffer is complete. */
	(void)target;
	state = gles_state(gles_context());
	if (state == NULL || state->framebuffer == 0U)
		return GL_FRAMEBUFFER_COMPLETE;

	/* Framebuffer objects are not there yet. */
	return GL_FRAMEBUFFER_UNSUPPORTED;
}

/*
 * Attaches a texture to the bound framebuffer (accepted, not drawn into).
 */
GL_APICALL void GL_APIENTRY
glFramebufferTexture2D(
	GLenum target,
	GLenum attachment,
	GLenum textarget,
	GLuint texture,
	GLint level)
{
	struct zegl_context *context;
	struct gles_state *state;

	/* The window's framebuffer takes no attachments. */
	(void)target;
	(void)attachment;
	(void)textarget;
	(void)texture;
	(void)level;
	context = gles_context();
	state = gles_state(context);
	if (state != NULL && state->framebuffer == 0U)
		gles_error(context, GL_INVALID_OPERATION);
}

/*
 * Attaches a renderbuffer to the bound framebuffer (accepted, not drawn
 * into).
 */
GL_APICALL void GL_APIENTRY
glFramebufferRenderbuffer(
	GLenum target,
	GLenum attachment,
	GLenum renderbuffertarget,
	GLuint renderbuffer)
{
	struct zegl_context *context;
	struct gles_state *state;

	/* The window's framebuffer takes no attachments. */
	(void)target;
	(void)attachment;
	(void)renderbuffertarget;
	(void)renderbuffer;
	context = gles_context();
	state = gles_state(context);
	if (state != NULL && state->framebuffer == 0U)
		gles_error(context, GL_INVALID_OPERATION);
}

/*
 * Reports an attachment of the bound framebuffer: there are none.
 */
GL_APICALL void GL_APIENTRY
glGetFramebufferAttachmentParameteriv(
	GLenum target,
	GLenum attachment,
	GLenum pname,
	GLint *params)
{
	/* Nothing is attached. */
	(void)target;
	(void)attachment;
	(void)pname;
	*params = GL_NONE;
}

/*
 * Makes renderbuffer names.
 */
GL_APICALL void GL_APIENTRY
glGenRenderbuffers(
	GLsizei n,
	GLuint *renderbuffers)
{
	struct zegl_context *context;
	struct gles_state *state;
	GLsizei index;

	/* A context with its state and a count. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (n < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Names counted up from 1. */
	for (index = 0; index < n; index++)
		renderbuffers[index] = ++state->next_renderbuffer;
}

/*
 * Deletes renderbuffer names.
 */
GL_APICALL void GL_APIENTRY
glDeleteRenderbuffers(
	GLsizei n,
	const GLuint *renderbuffers)
{
	struct gles_state *state;
	GLsizei index;

	/* A context with its state. */
	state = gles_state(gles_context());
	if (state == NULL)
		return;

	/* The bound one goes back to 0. */
	for (index = 0; index < n; index++) {
		if (renderbuffers[index] != 0U && renderbuffers[index] == state->renderbuffer)
			state->renderbuffer = 0U;
	}
}

/*
 * Binds a renderbuffer.
 */
GL_APICALL void GL_APIENTRY
glBindRenderbuffer(
	GLenum target,
	GLuint renderbuffer)
{
	struct zegl_context *context;
	struct gles_state *state;

	/* A context with its state and the one target. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (target != GL_RENDERBUFFER) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* Bound. */
	state->renderbuffer = renderbuffer;
}

/*
 * Reports whether a name is a renderbuffer.
 */
GL_APICALL GLboolean GL_APIENTRY
glIsRenderbuffer(
	GLuint renderbuffer)
{
	struct gles_state *state;

	/* A name that was made. */
	state = gles_state(gles_context());
	if (state == NULL || renderbuffer == 0U || renderbuffer > state->next_renderbuffer)
		return GL_FALSE;
	return GL_TRUE;
}

/*
 * Gives the bound renderbuffer storage (accepted, not made).
 */
GL_APICALL void GL_APIENTRY
glRenderbufferStorage(
	GLenum target,
	GLenum internalformat,
	GLsizei width,
	GLsizei height)
{
	/* Nothing is made until framebuffer objects are there. */
	(void)target;
	(void)internalformat;
	(void)width;
	(void)height;
}

/*
 * Reports a parameter of the bound renderbuffer: all are 0.
 */
GL_APICALL void GL_APIENTRY
glGetRenderbufferParameteriv(
	GLenum target,
	GLenum pname,
	GLint *params)
{
	/* No storage. */
	(void)target;
	(void)pname;
	*params = 0;
}
