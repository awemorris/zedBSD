/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * egltest's OpenGL ES 3.0 3D and 2D array texture and pixel buffer scene
 * (WS068 p028), volumes.c.
 */

#ifndef EGLTEST_VOLUMES_H
#define EGLTEST_VOLUMES_H

/* Makes the programs and the textures, and checks what the API reports; nonzero on failure. */
int egltest_volumes_start(void);

/* Draws one square per texture over a window of a size. */
void egltest_volumes_draw(int width, int height);

/* Reads back the squares' colours and prints them; returns how many differ, with the failures of the start's checks. */
int egltest_volumes_check(int width, int height, const char *token);

#endif
