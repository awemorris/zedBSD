/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libGL's calls of OpenGL 4.0 that its extensions bring to earlier
 * contexts and that the translation's own files do not hold: the ARB
 * spellings of the calls of GL_ARB_draw_buffers_blend and
 * GL_ARB_sample_shading, which a 3.x context's application finds by those
 * names.  The indirect draws (GL_ARB_draw_indirect) are libGLESv2's
 * draw.c, which OpenGL ES 3.1 shares.
 */

#include "fixed.h"

/*
 * The ARB spelling of glBlendFunci (GL_ARB_draw_buffers_blend).
 */
GL_APICALL void GL_APIENTRY
glBlendFunciARB(
	GLuint buf,
	GLenum src,
	GLenum dst)
{
	/* The same. */
	glBlendFunci(buf, src, dst);
}

/*
 * The ARB spelling of glBlendFuncSeparatei (GL_ARB_draw_buffers_blend).
 */
GL_APICALL void GL_APIENTRY
glBlendFuncSeparateiARB(
	GLuint buf,
	GLenum srcRGB,
	GLenum dstRGB,
	GLenum srcAlpha,
	GLenum dstAlpha)
{
	/* The same. */
	glBlendFuncSeparatei(buf, srcRGB, dstRGB, srcAlpha, dstAlpha);
}

/*
 * The ARB spelling of glBlendEquationi (GL_ARB_draw_buffers_blend).
 */
GL_APICALL void GL_APIENTRY
glBlendEquationiARB(
	GLuint buf,
	GLenum mode)
{
	/* The same. */
	glBlendEquationi(buf, mode);
}

/*
 * The ARB spelling of glBlendEquationSeparatei (GL_ARB_draw_buffers_blend).
 */
GL_APICALL void GL_APIENTRY
glBlendEquationSeparateiARB(
	GLuint buf,
	GLenum modeRGB,
	GLenum modeAlpha)
{
	/* The same. */
	glBlendEquationSeparatei(buf, modeRGB, modeAlpha);
}

/*
 * The ARB spelling of glMinSampleShading (GL_ARB_sample_shading).
 */
GL_APICALL void GL_APIENTRY
glMinSampleShadingARB(
	GLfloat value)
{
	/* The same. */
	glMinSampleShading(value);
}
