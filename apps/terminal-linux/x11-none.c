/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The X server side of SSH's X11 forwarding on Linux: none yet.  The
 * Windows program runs zedBSD's X server in a process of its own
 * (x11-host.c); the Linux program has no X server of its own, so a host
 * whose settings ask for ForwardX11 has each X client refused, and the
 * tab says so once.  No X window's tab is ever opened.
 */

#include "x11-host.h"
#include "x11-tab.h"

#include <errno.h>
#include <string.h>

/* Readies a connection's X server: only its settings are kept. */
void
x11_host_init(
	struct x11_host *host,
	const struct x11_host_settings *settings)
{
	/* Nothing runs; the settings for the record. */
	memset(host, 0, sizeof(*host));
	host->settings = *settings;
}

/* Ends a connection's X server: there is nothing to end. */
void
x11_host_end(
	struct x11_host *host,
	struct ssh_engine *engine)
{
	(void)host;
	(void)engine;
}

/* Takes an X client the server forwards: refused (ENOTSUP), as no X server runs. */
int
x11_host_accept(
	struct x11_host *host,
	uint32_t channel,
	uint64_t now)
{
	(void)host;
	(void)channel;
	(void)now;

	/* No X server takes it. */
	return ENOTSUP;
}

/* Hands an X client's bytes to the X server: none is ever taken, so there are none. */
void
x11_host_data(
	struct x11_host *host,
	struct ssh_engine *engine,
	uint32_t channel,
	const uint8_t *bytes,
	size_t size)
{
	(void)host;
	(void)engine;
	(void)channel;
	(void)bytes;
	(void)size;
}

/* Hears an X client's end of input: none is ever taken. */
void
x11_host_eof(
	struct x11_host *host,
	uint32_t channel)
{
	(void)host;
	(void)channel;
}

/* Hears an X client's channel closed: none is ever taken. */
void
x11_host_closed(
	struct x11_host *host,
	uint32_t channel)
{
	(void)host;
	(void)channel;
}

/* Moves the X server's bytes: there are none.  Returns 0. */
int
x11_host_pump(
	struct x11_host *host,
	struct ssh_engine *engine,
	uint64_t now)
{
	(void)host;
	(void)engine;
	(void)now;

	/* Nothing moved, nothing failed. */
	return 0;
}

/* Gives the socket to the X server the thread waits on: none. */
struct kl_socket *
x11_host_socket(
	const struct x11_host *host,
	int *writable)
{
	(void)host;

	/* No socket, nothing to write. */
	*writable = 0;
	return NULL;
}

/* Counts the X clients the X server holds: none. */
int
x11_host_open_count(
	const struct x11_host *host)
{
	(void)host;

	/* None is open. */
	return 0;
}

/* Takes an X window's tab the X server opened: none is ever opened. */
struct x11_tab *
x11_host_new_tab(
	struct x11_host *host)
{
	(void)host;

	/* No tab. */
	return NULL;
}

/* Lets the X server bring its windows forward: it has none. */
void
x11_host_foreground(
	const struct x11_host *host)
{
	(void)host;
}

/* Gives an X window's tab its session: no tab is ever made. */
void
x11_tab_attach(
	struct x11_tab *tab,
	struct mikuterm_session *session,
	uint32_t number)
{
	(void)tab;
	(void)session;
	(void)number;
}
