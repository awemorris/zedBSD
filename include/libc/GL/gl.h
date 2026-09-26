/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * zedBSD's desktop OpenGL header (WS069): libGL.so has the OpenGL ES 2.0
 * functions (their declarations and types come from GLES2/gl2.h); the
 * fixed-function GL 1.x comes with WS069 p005.
 */

#ifndef LIBC_GL_GL_H
#define LIBC_GL_GL_H

#include <GLES2/gl2.h>

#ifndef APIENTRY
#define APIENTRY GL_APIENTRY
#endif

#ifndef GLAPI
#define GLAPI GL_APICALL
#endif

typedef double GLdouble;
typedef double GLclampd;

#define GL_VERSION_1_1 1

#endif
