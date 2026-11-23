/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A connection through ssh_config's ProxyCommand (proxy.c): the command
 * runs as a program of its own, and what it reads and writes is the
 * connection to the server.  The connection is given to the caller as a
 * socket like any other, so the SSH engine's loop waits on it as on a
 * host's socket.
 */

#ifndef MIKUTERM_PROXY_H
#define MIKUTERM_PROXY_H

#include "mikuterm.h"

#include <stddef.h>

/* The longest line proxy_start leaves about a command it cannot run. */
#define PROXY_ERROR_MAX		(MIKUTERM_PATH_MAX + 96U)

/* The most of what the command writes on its errors that is kept, the last of it. */
#define PROXY_ERRORS_MAX	2048U

struct proxy;
struct kl_socket;

int proxy_start(const char *command, struct proxy **proxy, struct kl_socket **socket, char *error, size_t error_size);
void proxy_errors(struct proxy *proxy, char *text, size_t size);
void proxy_stop(struct proxy *proxy);

#endif
