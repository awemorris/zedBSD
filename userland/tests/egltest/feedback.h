/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * egltest's OpenGL ES 3.0 transform feedback scene (WS068 p030),
 * feedback.c.
 */

#ifndef EGLTEST_FEEDBACK_H
#define EGLTEST_FEEDBACK_H

/* Makes the programs, captures outputs with transform feedback, and checks what the API reports; nonzero on failure. */
int egltest_feedback_start(void);

/* Draws one square per outcome, and one from captured vertices, over a window of a size. */
void egltest_feedback_draw(int width, int height);

/* Reads back the squares' colours and prints them; returns how many are not green, with the failures of the start's checks. */
int egltest_feedback_check(int width, int height, const char *token);

#endif
