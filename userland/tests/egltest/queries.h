/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * egltest's OpenGL ES 3.0 query and sync scene (WS068 p027), queries.c.
 */

#ifndef EGLTEST_QUERIES_H
#define EGLTEST_QUERIES_H

/* Makes the program, runs the queries and syncs, and checks what the API reports; nonzero on failure. */
int egltest_queries_start(void);

/* Draws one square per outcome over a window of a size: green when it is the expected one, red otherwise. */
void egltest_queries_draw(int width, int height);

/* Reads back the squares' colours and prints them; returns how many differ, with the failures of the start's checks. */
int egltest_queries_check(int width, int height, const char *token);

#endif
