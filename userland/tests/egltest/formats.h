/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * egltest's OpenGL ES 3.0 texture format scene (WS068 p025), formats.c.
 */

#ifndef EGLTEST_FORMATS_H
#define EGLTEST_FORMATS_H

/* Makes the programs, the textures of each format and the sampler object, and checks what the API reports; nonzero on failure. */
int egltest_formats_start(void);

/* Draws one square per texture over a window of a size. */
void egltest_formats_draw(int width, int height);

/* Reads back the squares' colours and prints them; returns how many differ, with the failures of the start's checks. */
int egltest_formats_check(int width, int height, const char *token);

#endif
