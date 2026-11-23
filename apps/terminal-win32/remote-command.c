/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The %-tokens of ssh_config's RemoteCommand (remote-command.h), put in
 * their places as zedBSD's ssh puts them (ssh_client_remote_command),
 * which follows OpenSSH's ssh: %%, %d, %h, %j (empty: there is no
 * ProxyJump), %k, %L, %l, %n, %p, %r and %u.  %C (a SHA-1 hash, which
 * the SSH library does not compute) and %i (a numeric user id, which
 * Windows does not have) are refused, as is an unknown token and a '%'
 * at the end.
 *
 * Procedure referenced: OpenSSH's ssh, openssh-portable
 * fa4b2a82be7df9768272cfabc2d78e4a12c2b3c0, ssh.c (the expansion of
 * RemoteCommand with percent_expand) and ssh_config(5) TOKENS.
 */

#include "remote-command.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The longest local computer's name kept for %L, with its NUL. */
#define REMOTE_COMMAND_HOST_MAX	256U

static int remote_command_token(char token, const struct remote_command_tokens *tokens, const char *short_host, char *number, size_t number_size, const char **value, char *error, size_t error_size);

/*
 * Appends text to out with each %-token replaced by its value.  Returns
 * 0, EINVAL with the reason in error (a token that cannot be put in), or
 * the error of out (EMSGSIZE for a command longer than out takes).
 */
int
remote_command_expand(
	const char *text,
	const struct remote_command_tokens *tokens,
	struct ssh_buf *out,
	char *error,
	size_t error_size)
{
	char short_host[REMOTE_COMMAND_HOST_MAX];
	char number[24];
	const char *value;
	size_t length;
	int status;

	/* The local computer's name up to its first dot (%L). */
	length = strcspn(tokens->local_host, ".");
	if (length >= sizeof(short_host))
		length = sizeof(short_host) - 1U;
	memcpy(short_host, tokens->local_host, length);
	short_host[length] = '\0';

	/* Copies the text, putting each token's value in its place. */
	error[0] = '\0';
	while (*text != '\0') {
		/* A byte that starts no token is copied as it is. */
		if (*text != '%') {
			ssh_buf_put_bytes(out, text, 1U);
			text++;
			continue;
		}

		/* Reads the token after the '%'. */
		text++;
		status = remote_command_token(*text, tokens, short_host, number, sizeof(number), &value, error, error_size);
		if (status != 0)
			return status;

		/* Appends its value and steps over it. */
		ssh_buf_put_bytes(out, value, strlen(value));
		text++;
	}

	/* Reports a command longer than the buffer takes. */
	status = ssh_buf_error(out);
	if (status != 0) {
		(void)snprintf(error, error_size, "RemoteCommand: the command is too long");
		return status;
	}

	/* Succeeded: out holds the command. */
	return 0;
}

/*
 * Gives the value of one token.  Returns 0 with *value, or EINVAL with
 * the reason in error.
 */
static int
remote_command_token(
	char token,
	const struct remote_command_tokens *tokens,
	const char *short_host,
	char *number,
	size_t number_size,
	const char **value,
	char *error,
	size_t error_size)
{
	/* Gives the token's value. */
	switch (token) {
	case '%':
		/* A literal '%'. */
		*value = "%";
		return 0;
	case 'h':
		/* The remote host's name, as connected to. */
		*value = tokens->host_name;
		return 0;
	case 'n':
	case 'k':
		/* The host's name as given (also the host key alias, which is not set). */
		*value = tokens->host;
		return 0;
	case 'p':
		/* The remote port. */
		(void)snprintf(number, number_size, "%u", tokens->port);
		*value = number;
		return 0;
	case 'r':
		/* The remote user. */
		*value = tokens->user;
		return 0;
	case 'j':
		/* The jump host, of which there is none. */
		*value = "";
		return 0;
	case 'l':
		/* The local computer's name. */
		*value = tokens->local_host;
		return 0;
	case 'L':
		/* The local computer's name up to its first dot. */
		*value = short_host;
		return 0;
	case 'u':
		/* The local user's name, which must have been found. */
		if (tokens->local_user == NULL) {
			(void)snprintf(error, error_size, "RemoteCommand: the local user's name is not known");
			return EINVAL;
		}
		*value = tokens->local_user;
		return 0;
	case 'd':
		/* The local user's home folder, which must have been found. */
		if (tokens->home == NULL) {
			(void)snprintf(error, error_size, "RemoteCommand: the local user's home folder is not known");
			return EINVAL;
		}
		*value = tokens->home;
		return 0;
	case 'C':
		/* The connection's hash is SHA-1, which is not computed. */
		(void)snprintf(error, error_size, "RemoteCommand: %%C is not supported");
		return EINVAL;
	case 'i':
		/* A numeric user id, which Windows does not have. */
		(void)snprintf(error, error_size, "RemoteCommand: %%i is not supported");
		return EINVAL;
	case '\0':
		/* A '%' at the end has no token. */
		(void)snprintf(error, error_size, "RemoteCommand: invalid format");
		return EINVAL;
	default:
		/* Any other token is unknown, as in OpenSSH. */
		(void)snprintf(error, error_size, "RemoteCommand: unknown key %%%c", token);
		return EINVAL;
	}
}
