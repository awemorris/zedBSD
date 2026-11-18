/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * egltest's ETC2 and EAC compressed texture scene, etc2.c.
 */

#ifndef EGLTEST_ETC2_H
#define EGLTEST_ETC2_H

/* Makes the programs and the compressed textures, and checks what the API reports; nonzero on failure. */
int egltest_etc2_start(void);

/* Draws one square per texture over a window of a size. */
void egltest_etc2_draw(int width, int height);

/* Reads back the squares' colours and prints them; returns how many differ, with the failures of the start's checks. */
int egltest_etc2_check(int width, int height, const char *token);

#endif
