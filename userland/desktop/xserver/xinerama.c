/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * XINERAMA (version 1.1): the monitors on the root window (randr.c) as
 * its screens, one X screen spread over them.
 *
 * It answers as X.Org's XINERAMA over RANDR answers: active while there
 * is a monitor, each monitor a screen in the order RANDR lists them, and
 * the PanoramiX requests' screen size the root window's.
 */

#include "userland/desktop/xserver/internal.h"

#include <string.h>

/* The version the core speaks. */
#define XINERAMA_VERSION_MAJOR		1U
#define XINERAMA_VERSION_MINOR		1U

/* The requests, by their minor opcodes. */
#define XINERAMA_QUERY_VERSION		0U
#define XINERAMA_GET_STATE		1U
#define XINERAMA_GET_SCREEN_COUNT	2U
#define XINERAMA_GET_SCREEN_SIZE	3U
#define XINERAMA_IS_ACTIVE		4U
#define XINERAMA_QUERY_SCREENS		5U

/* The bytes of one screen in QueryScreens. */
#define XINERAMA_SCREEN_INFO_SIZE	8U

static unsigned xinerama_query_version(struct x11server *server, struct x11_client *client, size_t length);
static unsigned xinerama_get_state(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned xinerama_get_screen_size(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned xinerama_is_active(struct x11server *server, struct x11_client *client, size_t length);
static unsigned xinerama_query_screens(struct x11server *server, struct x11_client *client, size_t length);

/*
 * Answers a XINERAMA request.  Returns 0, or the error to send.
 */
unsigned
x11_xinerama_request(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	unsigned code;

	/* The client, and each request by its minor opcode. */
	client = &server->clients[index];
	switch (request[1]) {
	case XINERAMA_QUERY_VERSION:
		code = xinerama_query_version(server, client, length);
		break;
	case XINERAMA_GET_STATE:
	case XINERAMA_GET_SCREEN_COUNT:
		code = xinerama_get_state(server, client, request, length);
		break;
	case XINERAMA_GET_SCREEN_SIZE:
		code = xinerama_get_screen_size(server, client, request, length);
		break;
	case XINERAMA_IS_ACTIVE:
		code = xinerama_is_active(server, client, length);
		break;
	case XINERAMA_QUERY_SCREENS:
		code = xinerama_query_screens(server, client, length);
		break;
	default:
		code = X11_BAD_REQUEST;
		break;
	}

	/* Succeeded: answered, or the error to send. */
	return code;
}

/* QueryVersion (PanoramiX's): the version the core speaks, whatever the client's. */
static unsigned
xinerama_query_version(
	struct x11server *server,
	struct x11_client *client,
	size_t length)
{
	uint8_t reply[32];

	/* The client's version, two bytes. */
	if (length != 8U)
		return X11_BAD_LENGTH;

	/* Succeeded: the core's version. */
	memset(reply, 0, sizeof(reply));
	x11_write16(reply + 8, XINERAMA_VERSION_MAJOR, client->order);
	x11_write16(reply + 10, XINERAMA_VERSION_MINOR, client->order);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/*
 * GetState and GetScreenCount (PanoramiX's): on a window, whether the
 * screen is spread over monitors (always) or over how many, and the
 * window.
 */
static unsigned
xinerama_get_state(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	struct x11_window *window;
	uint8_t reply[32];

	/* A window there is. */
	if (length != 8U)
		return X11_BAD_LENGTH;
	window = x11_window_find(server, x11_read32(request + 4, client->order));
	if (window == NULL)
		return X11_BAD_WINDOW;

	/* The state (active) or the count of monitors. */
	memset(reply, 0, sizeof(reply));
	reply[1] = 1U;
	if (request[1] == XINERAMA_GET_SCREEN_COUNT)
		reply[1] = (uint8_t)server->monitor_count;

	/* Succeeded: with the window. */
	x11_write32(reply + 8, window->id, client->order);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/*
 * GetScreenSize (PanoramiX's): the root window's size, whichever screen
 * is named, with the window and the screen.
 */
static unsigned
xinerama_get_screen_size(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	struct x11_window *window;
	uint8_t reply[32];

	/* A window there is, and a screen. */
	if (length != 12U)
		return X11_BAD_LENGTH;
	window = x11_window_find(server, x11_read32(request + 4, client->order));
	if (window == NULL)
		return X11_BAD_WINDOW;

	/* Succeeded: the size, the window and the screen. */
	memset(reply, 0, sizeof(reply));
	x11_write32(reply + 8, server->width, client->order);
	x11_write32(reply + 12, server->height, client->order);
	x11_write32(reply + 16, window->id, client->order);
	x11_write32(reply + 20, x11_read32(request + 8, client->order), client->order);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/* IsActive: whether the screen is spread over monitors (while there is one). */
static unsigned
xinerama_is_active(
	struct x11server *server,
	struct x11_client *client,
	size_t length)
{
	uint8_t reply[32];

	/* The header alone. */
	if (length != 4U)
		return X11_BAD_LENGTH;

	/* Succeeded: active. */
	memset(reply, 0, sizeof(reply));
	if (server->monitor_count != 0U)
		x11_write32(reply + 8, 1U, client->order);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/* QueryScreens: each monitor's place and size on the root window. */
static unsigned
xinerama_query_screens(
	struct x11server *server,
	struct x11_client *client,
	size_t length)
{
	const struct x11_monitor *monitor;
	uint8_t reply[32U + X11_MONITORS_MAX * XINERAMA_SCREEN_INFO_SIZE];
	uint8_t *info;
	unsigned index;

	/* The header alone. */
	if (length != 4U)
		return X11_BAD_LENGTH;

	/* How many. */
	memset(reply, 0, sizeof(reply));
	x11_write32(reply + 8, server->monitor_count, client->order);

	/* Each one's place and size. */
	for (index = 0U; index < server->monitor_count; index++) {
		monitor = &server->monitors[index];
		info = reply + 32U + index * XINERAMA_SCREEN_INFO_SIZE;
		x11_write16(info, (uint16_t)monitor->x, client->order);
		x11_write16(info + 2, (uint16_t)monitor->y, client->order);
		x11_write16(info + 4, (uint16_t)monitor->width, client->order);
		x11_write16(info + 6, (uint16_t)monitor->height, client->order);
	}

	/* Succeeded: the screens. */
	x11_reply(server, client, reply, 32U + server->monitor_count * XINERAMA_SCREEN_INFO_SIZE);
	return 0U;
}
