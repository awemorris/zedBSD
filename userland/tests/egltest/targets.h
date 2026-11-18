/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * egltest's OpenGL ES 3.0 framebuffer object scene (WS068 p026),
 * targets.c: several colour attachments, read and draw framebuffers,
 * attachments of other formats, levels and layers, depth and stencil.
 */

#ifndef EGLTEST_TARGETS_H
#define EGLTEST_TARGETS_H

/* Makes the programs and draws into the framebuffer objects, and checks what the API reports; nonzero on failure. */
int egltest_targets_start(void);

/* Draws one square per texture the framebuffer objects drew into over a window of a size. */
void egltest_targets_draw(int width, int height);

/* Reads back the squares' colours and prints them; returns how many differ, with the failures of the start's checks. */
int egltest_targets_check(int width, int height, const char *token);

#endif
