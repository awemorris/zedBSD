/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The command ssh_config's RemoteCommand gives a host (remote-command.c):
 * its %-tokens put in their places as OpenSSH's ssh puts them, from the
 * values of the connection and of the local user the caller gathers.
 *
 * It does not include a header of Windows, so the tests run it on the
 * host.
 */

#ifndef MIKUTERM_REMOTE_COMMAND_H
#define MIKUTERM_REMOTE_COMMAND_H

#include "userland/base/ssh/ssh-buf.h"

#include <stddef.h>

/* The longest line remote_command_expand leaves about a token it cannot put in. */
#define REMOTE_COMMAND_ERROR_MAX	128U

/*
 * The values of the tokens: host_name the name connected to (%h), host
 * the name the user gave (%n, and %k: there is no HostKeyAlias), port the
 * remote port (%p), user the remote user (%r), local_user and home the
 * local user's name and home folder (%u, %d; NULL when they could not be
 * found), and local_host the local computer's name (%l, and up to its
 * first dot %L).
 */
struct remote_command_tokens {
	const char *host_name;
	const char *host;
	unsigned port;
	const char *user;
	const char *local_user;
	const char *home;
	const char *local_host;
};

int remote_command_expand(const char *text, const struct remote_command_tokens *tokens, struct ssh_buf *out, char *error, size_t error_size);

#endif
