/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libGL's calls of OpenGL 4.5 that its extensions bring to earlier
 * contexts: the clip control (GL_ARB_clip_control).  The origin picks
 * whether the last stage before the rasterizer turns gl_Position's y over
 * and which winding is the front; the depth mode picks whether it moves
 * gl_Position's z from [-w, w] to [0, w] (draw.c, program.c).
 */

#include "fixed.h"

/*
 * Sets where the clip space's y = 1 lands in window coordinates
 * (GL_LOWER_LEFT: the top, GL's own; GL_UPPER_LEFT: the bottom) and the
 * clip space's depth range (GL_NEGATIVE_ONE_TO_ONE, GL's own, or
 * GL_ZERO_TO_ONE).
 */
GL_APICALL void GL_APIENTRY
glClipControl(
	GLenum origin,
	GLenum depth)
{
	struct zegl_context *context;
	struct gles_state *state;

	/* A context with its state. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;

	/* One of the two origins and one of the two depth ranges. */
	if (origin != GL_LOWER_LEFT && origin != GL_UPPER_LEFT) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}
	if (depth != GL_NEGATIVE_ONE_TO_ONE && depth != GL_ZERO_TO_ONE) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* Succeeded: the next draws' pipelines take them (the pipeline's key holds them). */
	state->clip_origin = origin;
	state->clip_depth = depth;
}
