/*
 * WS001 p015: AF_UNIX pathname publication source contract.
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *
ordered(const char *at, const char *limit, const char *text)
{
	const char *found = strstr(at, text);

	return found != NULL && found < limit ? found + strlen(text) : NULL;
}

int
main(void)
{
	const char *path = "src/kern/net/unix-socket.c";
	char *source;
	const char *at, *limit;
	FILE *file;
	long length;

	file = fopen(path, "rb");
	if (file == NULL || fseek(file, 0, SEEK_END) != 0 ||
	    (length = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0)
		return 1;
	source = malloc((size_t)length + 1U);
	if (source == NULL || fread(source, 1U, (size_t)length, file) !=
	    (size_t)length) {
		(void)fclose(file);
		free(source);
		return 1;
	}
	(void)fclose(file);
	source[length] = '\0';

	/* A pathname lookup may retain i_special only after the endpoint confirms
	 * that this exact path is its committed bind publication. */
	at = strstr(source, "unix_resolve_endpoint(");
	limit = at != NULL ? strstr(at, "unix_peer_ref(") : NULL;
	if (at == NULL || limit == NULL ||
	    (at = ordered(at, limit, "socket_tryref(socket)")) == NULL ||
	    (at = ordered(at, limit,
	    "unix_socket_bound_path_matches(socket, &resolved)")) == NULL ||
	    (at = ordered(at, limit, "socket_release(socket)")) == NULL)
		goto fail;

	/* bind() takes path references before the spin lock and publishes the
	 * referenced path, printable path, and bound bit before unlocking. */
	at = strstr(source, "unix_socket_bind_path(");
	limit = at != NULL ? strstr(at, "unix_socket_listen(") : NULL;
	if (at == NULL || limit == NULL ||
	    (at = ordered(at, limit, "path_set(&committed_path")) == NULL ||
	    (at = ordered(at, limit, "spin_lock_irqsave(&socket->lock)")) == NULL ||
	    (at = ordered(at, limit,
	    "endpoint->bound_path = committed_path")) == NULL ||
	    (at = ordered(at, limit, "strcpy(endpoint->path, path)")) == NULL ||
	    (at = ordered(at, limit, "endpoint->bound = 1")) == NULL ||
	    (at = ordered(at, limit,
	    "spin_unlock_irqrestore(&socket->lock, irq)")) == NULL)
		goto fail;

	free(source);
	puts("ws001-p015 AF_UNIX publication contract: PASS");
	return 0;
fail:
	free(source);
	fputs("ws001-p015 AF_UNIX publication contract: FAIL\n", stderr);
	return 1;
}
