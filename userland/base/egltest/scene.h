/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * egltest's drawing scene (WS068 p008), scene.c.
 */

#ifndef EGLTEST_SCENE_H
#define EGLTEST_SCENE_H

/* Makes the scene's program, buffers and texture; nonzero on failure. */
int egltest_scene_start(void);

/* Draws the scene over a window of a size. */
void egltest_scene_draw(int width, int height);

/* Reads back the scene's colours and prints them; returns how many differ. */
int egltest_scene_check(int width, int height, const char *token);

#endif
