/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * mikuterm's X server without GLX.
 *
 * mikuterm's server draws no OpenGL, so it does not name GLX: a remote
 * OpenGL program that found GLX would choose indirect GLX rendering and draw
 * nothing.  mikuterm leaves the X server's glx.c out and gives the core
 * these instead, which answer every extension name absent to QueryExtension,
 * name nothing to ListExtensions and refuse a request at GLX's major opcode.
 */

#include "userland/desktop/xserver/internal.h"

#include <string.h>

/*
 * Answers QueryExtension for a name the core does not know: absent.
 */
void
x11_glx_query_extension(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	uint8_t reply[32];

	(void)request;
	(void)length;

	/* Absent: no major opcode, no events, no errors. */
	memset(reply, 0, sizeof(reply));
	x11_reply(server, client, reply, sizeof(reply));
}

/*
 * Writes no GLX entry into a ListExtensions reply.
 */
size_t
x11_glx_list_name(
	uint8_t *list,
	size_t room)
{
	(void)list;
	(void)room;

	/* Nothing written: GLX is not named. */
	return 0U;
}

/*
 * Refuses a request at GLX's major opcode, which no extension has here.
 */
unsigned
x11_glx_request(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	(void)server;
	(void)client;
	(void)request;
	(void)length;

	/* An unknown major opcode. */
	return X11_BAD_REQUEST;
}
