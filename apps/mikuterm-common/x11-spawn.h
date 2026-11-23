/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The system's part of mikuterm's X server process: how the program
 * starts itself again as the X server of one connection (mikuterm
 * --x11-server), hands it its end of the link, and ends it.
 *
 * x11-host.c makes the link, a connected pair of loopback sockets: the
 * parent keeps one end; the other is the child's.  x11_spawn_guard keeps
 * the pair's listener from being shared while the pair is made.
 * x11_spawn_start starts the child with its end (host->process,
 * host->job and host->pid as the system keeps them), and may close the
 * parent's descriptor of that end (host->child_end NULL) once the child
 * holds its own copy.  x11_spawn_stop, called after the link has closed,
 * gives the child a moment (X11_SPAWN_END_MS) to end by itself, then ends
 * it, and forgets it.  x11_host_foreground (x11-host.h) is the system's
 * too.
 *
 * Windows' is x11-spawn-win32.c; the POSIX systems' (Linux, and the other
 * systems with posix_spawn) is apps/terminal-linux/x11-spawn-posix.c.
 */

#ifndef MIKUTERM_X11_SPAWN_H
#define MIKUTERM_X11_SPAWN_H

#include "x11-host.h"

/* How long the child has to end by itself after its link closes (ms). */
#define X11_SPAWN_END_MS	3000U

/* The command line's switch that makes the program the X server process. */
#define X11_SPAWN_SWITCH	"--x11-server"

struct kl_socket;

void x11_spawn_guard(struct kl_socket *listener);
int x11_spawn_start(struct x11_host *host, struct kl_socket *child_end);
void x11_spawn_stop(struct x11_host *host);

#endif
