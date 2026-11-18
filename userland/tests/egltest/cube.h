/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * egltest's cube map scene (WS068 p023), cube.c.
 */

#ifndef EGLTEST_CUBE_H
#define EGLTEST_CUBE_H

/* Makes the program and the cube map (faces from the CPU, drawn by framebuffer objects and copied, then mipmapped); nonzero on failure. */
int egltest_cube_start(void);

/* Draws the six squares, one per face, over a window of a size. */
void egltest_cube_draw(int width, int height);

/* Reads back the squares' colours and prints them; returns how many differ. */
int egltest_cube_check(int width, int height, const char *token);

#endif
