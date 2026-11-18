/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * glxtest's OpenGL 3.1 scene (WS068 p031), gl31.c.
 */

#ifndef GLXTEST_GL31_H
#define GLXTEST_GL31_H

/* Makes the programs, runs the checks of OpenGL 3.1's and 3.2's calls, and reports them; nonzero on failure. */
int glxtest_gl31_start(void);

/* Draws one square per outcome over a window of a size (green: the expected one), and a fixed-function one. */
void glxtest_gl31_draw(int width, int height);

/* Reads back the squares' colours and prints them; returns how many differ, with the failures of the start's checks. */
int glxtest_gl31_check(int width, int height, const char *token);

#endif
