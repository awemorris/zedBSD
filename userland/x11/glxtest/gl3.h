/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * glxtest's OpenGL 3.0 scene (WS068 p013), gl3.c.
 */

#ifndef GLXTEST_GL3_H
#define GLXTEST_GL3_H

/* Makes the programs, runs the checks of OpenGL 3.0's calls, and reports them; nonzero on failure. */
int glxtest_gl3_start(void);

/* Draws one square per outcome over a window of a size (green: the expected one), and a fixed-function one. */
void glxtest_gl3_draw(int width, int height);

/* Reads back the squares' colours and prints them; returns how many differ, with the failures of the start's checks. */
int glxtest_gl3_check(int width, int height, const char *token);

#endif
