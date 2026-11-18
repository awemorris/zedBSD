/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The private interface between libGL.so (OpenGL) and libGLX.so (GLX,
 * xserver's library, WS178): GL asks the window-system binding for the
 * desktop GL version, flags and profile of the calling thread's current
 * context, and GLX hands GL its answers and turns on the fixed-function
 * layer before it makes its first context.  Neither the names nor the
 * structure are a public API; only libGLX calls zgl_glx_attach.
 */

#ifndef LIBGL_ZGL_GLX_H
#define LIBGL_ZGL_GLX_H

#include <GL/gl.h>
#include <EGL/egl.h>

/* The desktop GL version of a context without a binding that says otherwise: 1.4, major * 10 + minor. */
#define ZGL_GL_LEGACY		14U

/*
 * The answers of a window-system binding about the calling thread's
 * current context.  The binding keeps one instance for the life of the
 * process; GL keeps a pointer to it.
 */
struct zgl_glx_query {
	/* The desktop GL version (major * 10 + minor) and GL_CONTEXT_FLAGS; ZGL_GL_LEGACY and 0 without a context. */
	unsigned (*version)(GLint *flags);

	/* The GL_CONTEXT_PROFILE_MASK; 0 before 3.2 and without a context. */
	GLint (*profile)(void);
};

/* libGL.so: takes the binding's answers and turns on the fixed-function layer (libGLX calls it before its first context). */
void zgl_glx_attach(const struct zgl_glx_query *query);

/*
 * libGL.so: the latest desktop GL version (major * 10 + minor) a context
 * on an initialized EGL display of libEGL's may have: what the display's
 * device does decides it (OpenGL 3.0 needs transform feedback, 3.1
 * buffer textures, 3.2 geometry shaders and depth clamping, 3.3
 * dual-source blending and timer queries, 4.0 more; without 3.0's, 1.4),
 * up to the latest version the library gives.
 */
unsigned zgl_glx_version_limit(EGLDisplay display);

#endif
