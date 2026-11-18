/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * egltest's OpenGL ES 3.0 API scene (WS068 p024), es3.c.
 */

#ifndef EGLTEST_ES3_H
#define EGLTEST_ES3_H

/* Makes the program, the buffers and the vertex array objects, and checks what the API reports of them; nonzero on failure. */
int egltest_es3_start(void);

/* Draws the shapes, one vertex array object each, over a window of a size. */
void egltest_es3_draw(int width, int height);

/* Reads back the shapes' colours and prints them; returns how many differ, with the failures of the start's checks. */
int egltest_es3_check(int width, int height, const char *token);

#endif
