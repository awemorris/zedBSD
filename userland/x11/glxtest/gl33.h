/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * glxtest's OpenGL 3.3 scene and its OpenGL 4.0 one, gl33.c.
 */

#ifndef GLXTEST_GL33_H
#define GLXTEST_GL33_H

/* Makes the programs, runs the checks of OpenGL 3.3's calls, and reports them; nonzero on failure. */
int glxtest_gl33_start(void);

/* As glxtest_gl33_start in an OpenGL 4.0 context, with 4.0's checks and outcomes after 3.3's; nonzero on failure. */
int glxtest_gl40_start(void);

/* Draws one square per outcome over a window of a size (green: the expected one), and a fixed-function one. */
void glxtest_gl33_draw(int width, int height);

/* Reads back the squares' colours and prints them; returns how many differ, with the failures of the start's checks. */
int glxtest_gl33_check(int width, int height, const char *token);

#endif
