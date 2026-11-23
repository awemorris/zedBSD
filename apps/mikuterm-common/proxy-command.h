/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The text of ssh_config's ProxyCommand (proxy-command.c): its %-tokens
 * put in their places as OpenSSH's ssh puts them, and the program it
 * names (its first word) taken apart from the rest of the line, which is
 * handed to the program as its command line is.
 *
 * On the POSIX systems the whole line is split into the program's
 * words as a shell splits a simple command, without a shell.
 *
 * It does not include a header of Windows, so the tests run it on the
 * host; proxy.c finds and starts the program on Windows,
 * apps/terminal-linux/proxy-posix.c on the POSIX systems.
 */

#ifndef MIKUTERM_PROXY_COMMAND_H
#define MIKUTERM_PROXY_COMMAND_H

#include <stddef.h>

/* The longest line proxy_command_expand leaves about a token it cannot put in. */
#define PROXY_COMMAND_ERROR_MAX		128U

/*
 * The values of the tokens: host_name the name connected to (%h), host
 * the name the user gave (%n), port the remote port (%p) and user the
 * remote user (%r).
 */
struct proxy_command_tokens {
	const char *host_name;
	const char *host;
	unsigned port;
	const char *user;
};

int proxy_command_expand(const char *text, const struct proxy_command_tokens *tokens, char *out, size_t size, char *error, size_t error_size);
int proxy_command_words(const char *command, char *buffer, size_t size, char **words, unsigned most, unsigned *count, char *error, size_t error_size);
int proxy_command_program(const char *command, unsigned words, char *program, size_t size, const char **rest, int *quoted);

#endif
