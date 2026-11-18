/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * egltest's OpenGL ES 2.0 built-ins scene (WS068 p004), es2.c.
 */

#ifndef EGLTEST_ES2_H
#define EGLTEST_ES2_H

/* Makes the programs, the framebuffer object and the textures; nonzero on failure. */
int egltest_es2_start(void);

/* Draws gl_FragCoord.y as red over a window of a size (dark at the bottom, bright at the top). */
void egltest_es2_draw(int width, int height);

/* Reads back the window's and the framebuffer object's built-ins and the sampler array, and prints them; returns how many differ. */
int egltest_es2_check(int width, int height, const char *token);

#endif
