/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The interface between mikuterm's in-process X server thread and the
 * application that hosts it: thread limits, memory caps, and the query
 * for the logical window size and scale.
 */

#ifndef MIKUTERM_X11_THREAD_H
#define MIKUTERM_X11_THREAD_H

/* The name of each connection's X server thread. */
#define X11_THREAD_NAME			"mikuterm-x11"

/* The stack size of each X server thread (2 MiB). */
#define X11_THREAD_STACK		(2U * 1024U * 1024U)

/* The memory caps enforced by the X server core (128 MiB per client, 256 MiB total). */
#define X11_THREAD_CLIENT_MEMORY	(128U * 1024U * 1024U)
#define X11_THREAD_SERVER_MEMORY	(256U * 1024U * 1024U)

/*
 * Returns the logical window size in pixels and the X scale in percent;
 * sets outputs to 0 if not yet known.  May be called from any thread.
 */
void mikuterm_x11_screen(unsigned *width, unsigned *height, unsigned *percent);

#endif
