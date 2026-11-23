/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GLX extension: its presence (QueryExtension), its version and its
 * strings.
 *
 * Rendering is the client's own: libGL draws with EGL and OpenGL ES on
 * Vulkan and hands each frame over as an image (PutImageRGB24), so the
 * server answers only what a client asks before it draws.  Indirect
 * rendering is not there.
 */

#include "userland/desktop/xserver/internal.h"

#include "include/kern/os-name.h"

#include <string.h>

/* The GLX minor opcodes answered. */
#define GLX_QUERY_VERSION		7U
#define GLX_QUERY_EXTENSIONS_STRING	18U
#define GLX_QUERY_SERVER_STRING		19U
#define GLX_CLIENT_INFO			20U

/* The names QueryServerString asks for. */
#define GLX_VENDOR			1U
#define GLX_VERSION			2U

/* The version and strings of the extension. */
#define GLX_SERVER_MAJOR		1U
#define GLX_SERVER_MINOR		4U
#define GLX_VENDOR_STRING		ZEDBSD_OS_NAME
#define GLX_VERSION_STRING		"1.4"
#define GLX_EXTENSIONS_STRING		"GLX_ARB_get_proc_address"

/* The longest string answered, with its terminator and padding. */
#define GLX_STRING_MAX			64U

/* The extension's name, as QueryExtension asks for it and ListExtensions names it. */
#define GLX_NAME			"GLX"

static void glx_string_reply(struct x11server *server, struct x11_client *client, const char *text);

/*
 * Answers QueryExtension: GLX is present at its major opcode, with its
 * first event and first error; any other name is absent.
 */
void
x11_glx_query_extension(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	uint8_t reply[32];
	size_t name_length;
	int differs;

	/* An absent extension unless the name is GLX. */
	memset(reply, 0, sizeof(reply));
	name_length = x11_read16(request + 4, client->order);

	/* The name must fit the request and be GLX's. */
	differs = 1;
	if (name_length == sizeof(GLX_NAME) - 1U && length >= 8U + name_length)
		differs = memcmp(request + 8, GLX_NAME, name_length);

	/*
	 * Present: the major opcode, and the first event and first error the
	 * client's Xlib names GLX's events and errors from (none is sent, but
	 * a core error must not be named as GLX's).
	 */
	if (differs == 0) {
		reply[8] = 1U;
		reply[9] = (uint8_t)X11_GLX_MAJOR;
		reply[10] = (uint8_t)X11_GLX_FIRST_EVENT;
		reply[11] = (uint8_t)X11_GLX_FIRST_ERROR;
	}

	/* The reply. */
	x11_reply(server, client, reply, sizeof(reply));
}

/*
 * Writes GLX's entry of a ListExtensions reply: the name as a counted
 * string.  Returns the bytes written, or 0 when the list has no room.
 */
size_t
x11_glx_list_name(
	uint8_t *list,
	size_t room)
{
	size_t name_length;

	/* The count and the name must fit what is left of the list. */
	name_length = sizeof(GLX_NAME) - 1U;
	if (room < 1U + name_length)
		return 0U;

	/* The count, then the name's bytes. */
	list[0] = (uint8_t)name_length;
	memcpy(list + 1, GLX_NAME, name_length);

	/* Succeeded: the entry's size. */
	return 1U + name_length;
}

/*
 * Answers a GLX request: the version, the strings, and the client's
 * introduction.  Returns 0, or the X error of a request that is refused.
 */
unsigned
x11_glx_request(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	uint8_t reply[32];
	uint32_t name;

	/* The minor opcode decides. */
	memset(reply, 0, sizeof(reply));
	switch (request[1]) {
	case GLX_QUERY_VERSION:
		x11_write32(reply + 8, GLX_SERVER_MAJOR, client->order);
		x11_write32(reply + 12, GLX_SERVER_MINOR, client->order);
		x11_reply(server, client, reply, sizeof(reply));
		break;
	case GLX_QUERY_EXTENSIONS_STRING:
		glx_string_reply(server, client, GLX_EXTENSIONS_STRING);
		break;
	case GLX_QUERY_SERVER_STRING:
		if (length < 12U)
			return X11_BAD_LENGTH;
		name = x11_read32(request + 8, client->order);
		if (name == GLX_VENDOR) {
			glx_string_reply(server, client, GLX_VENDOR_STRING);
		} else if (name == GLX_VERSION) {
			glx_string_reply(server, client, GLX_VERSION_STRING);
		} else {
			glx_string_reply(server, client, GLX_EXTENSIONS_STRING);
		}

		/* Answered. */
		break;
	case GLX_CLIENT_INFO:
		/* The client's own version and extensions need no answer. */
		break;
	default:
		/* Indirect rendering and the rest are not there. */
		return X11_BAD_REQUEST;
	}

	/* Succeeded: the request is answered. */
	return 0U;
}

/* Sends a string reply (QueryServerString's and QueryExtensionsString's form): its length with the terminator, then the padded bytes. */
static void
glx_string_reply(
	struct x11server *server,
	struct x11_client *client,
	const char *text)
{
	uint8_t reply[32 + GLX_STRING_MAX];
	size_t bytes;
	size_t padded;

	/* The string with its terminator, padded to words. */
	bytes = strlen(text) + 1U;
	padded = (bytes + 3U) & ~(size_t)3U;
	if (padded > GLX_STRING_MAX)
		return;
	memset(reply, 0, sizeof(reply));
	x11_write32(reply + 12, (uint32_t)bytes, client->order);
	memcpy(reply + 32, text, bytes);

	/* The reply. */
	x11_reply(server, client, reply, 32U + padded);
}
