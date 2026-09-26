/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * zedBSD's OpenGL ES (WS068 p002): the calls every GLES program makes
 * before it draws anything, over the calling thread's EGL context.
 *
 * glClear records the colour a frame is cleared to; eglSwapBuffers clears
 * the window's image to it and presents it (libEGL, vulkan.c).  Buffers,
 * shaders, textures and draws come with the translation to Vulkan
 * (WS068 p003, whose way is still to be chosen: plan/ws068/design.md §4).
 */

#include "../libegl/zegl.h"

#include <GLES2/gl2.h>

#include <stddef.h>

static struct zegl_context *gles_context(void);
static void gles_error(struct zegl_context *context, GLenum error);

/*
 * Sets the colour glClear clears the colour buffer to (each channel clamped to [0, 1]).
 */
GL_APICALL void GL_APIENTRY
glClearColor(
	GLfloat red,
	GLfloat green,
	GLfloat blue,
	GLfloat alpha)
{
	struct zegl_context *context;
	GLfloat channels[4];
	unsigned index;

	/* Without a current context the call does nothing. */
	context = gles_context();
	if (context == NULL)
		return;

	/* Each channel, clamped. */
	channels[0] = red;
	channels[1] = green;
	channels[2] = blue;
	channels[3] = alpha;
	for (index = 0U; index < 4U; index++) {
		if (channels[index] < 0.0f)
			channels[index] = 0.0f;
		if (channels[index] > 1.0f)
			channels[index] = 1.0f;
		context->gles.clear_color[index] = channels[index];
	}
}

/*
 * Clears buffers of the frame: the colour buffer to the clear colour (the
 * depth and stencil buffers come with WS068 p003).
 */
GL_APICALL void GL_APIENTRY
glClear(
	GLbitfield mask)
{
	struct zegl_context *context;

	/* Without a current context the call does nothing. */
	context = gles_context();
	if (context == NULL)
		return;

	/* A bit other than the three buffers' is an error. */
	if ((mask & ~(GLbitfield)(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) != 0U) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* The frame starts from the clear colour. */
	if ((mask & GL_COLOR_BUFFER_BIT) != 0U)
		context->gles.clear_pending = 1;
}

/*
 * Sets the viewport.
 */
GL_APICALL void GL_APIENTRY
glViewport(
	GLint x,
	GLint y,
	GLsizei width,
	GLsizei height)
{
	struct zegl_context *context;

	/* Without a current context the call does nothing. */
	context = gles_context();
	if (context == NULL)
		return;

	/* A negative size is an error. */
	if (width < 0 || height < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* The viewport, for the draws to come. */
	context->gles.viewport[0] = x;
	context->gles.viewport[1] = y;
	context->gles.viewport[2] = width;
	context->gles.viewport[3] = height;
	context->gles.viewport_set = 1;
}

/*
 * Reports a few integer states: the viewport and its largest size.
 */
GL_APICALL void GL_APIENTRY
glGetIntegerv(
	GLenum pname,
	GLint *data)
{
	struct zegl_context *context;

	/* Without a current context, or a place for the value, the call does nothing. */
	context = gles_context();
	if (context == NULL || data == NULL)
		return;

	/* The state asked for. */
	switch (pname) {
	case GL_VIEWPORT:
		data[0] = context->gles.viewport[0];
		data[1] = context->gles.viewport[1];
		data[2] = context->gles.viewport[2];
		data[3] = context->gles.viewport[3];
		return;
	case GL_MAX_VIEWPORT_DIMS:
		data[0] = 16384;
		data[1] = 16384;
		return;
	default:
		break;
	}

	/* Any other state is not there yet. */
	gles_error(context, GL_INVALID_ENUM);
}

/*
 * Returns one of GLES's strings.
 */
GL_APICALL const GLubyte *GL_APIENTRY
glGetString(
	GLenum name)
{
	struct zegl_context *context;

	/* Without a current context there are no strings. */
	context = gles_context();
	if (context == NULL)
		return NULL;

	/* The string asked for. */
	switch (name) {
	case GL_VENDOR:
		return (const GLubyte *)"zedBSD";
	case GL_RENDERER:
		return (const GLubyte *)"zedBSD OpenGL ES on Vulkan";
	case GL_VERSION:
		return (const GLubyte *)"OpenGL ES 2.0 zedBSD";
	case GL_SHADING_LANGUAGE_VERSION:
		return (const GLubyte *)"OpenGL ES GLSL ES 1.00";
	case GL_EXTENSIONS:
		return (const GLubyte *)"";
	default:
		break;
	}

	/* Any other name is an error. */
	gles_error(context, GL_INVALID_ENUM);
	return NULL;
}

/*
 * Returns the first error since the last call, and clears it.
 */
GL_APICALL GLenum GL_APIENTRY
glGetError(void)
{
	struct zegl_context *context;
	GLenum error;

	/* Without a current context there is no error. */
	context = gles_context();
	if (context == NULL)
		return GL_NO_ERROR;

	/* Reading it clears it. */
	error = (GLenum)context->gles.error;
	context->gles.error = GL_NO_ERROR;
	return error;
}

/*
 * Sends the recorded commands on: eglSwapBuffers sends each frame, so there is nothing to send.
 */
GL_APICALL void GL_APIENTRY
glFlush(void)
{
	/* Nothing is waiting. */
	return;
}

/*
 * Waits for the recorded commands: every frame is waited for already.
 */
GL_APICALL void GL_APIENTRY
glFinish(void)
{
	/* Nothing is running. */
	return;
}

/* Returns the calling thread's current context, or NULL. */
static struct zegl_context *
gles_context(void)
{
	struct zegl_context *context;

	/* libEGL keeps it per thread. */
	context = zegl_current_context();
	return context;
}

/* Records an error, keeping the first one until glGetError reads it. */
static void
gles_error(
	struct zegl_context *context,
	GLenum error)
{
	/* Only the first counts. */
	if (context->gles.error == GL_NO_ERROR)
		context->gles.error = error;
}
