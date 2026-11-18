/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * egltest's OpenGL ES 3.0 blit and multisample scene (WS068 p029),
 * blits.c.
 */

#ifndef EGLTEST_BLITS_H
#define EGLTEST_BLITS_H

/* Makes the programs, draws and blits into the framebuffer objects, and checks what the API reports; nonzero on failure. */
int egltest_blits_start(void);

/* Draws the squares over a window of a size, one of them blitted into the window. */
void egltest_blits_draw(int width, int height);

/* Reads back the squares' colours and prints them; returns how many differ, with the failures of the start's checks. */
int egltest_blits_check(int width, int height, const char *token);

#endif
