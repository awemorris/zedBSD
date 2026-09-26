/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Xzed's GLX extension (WS069 p004).
 *
 * Rendering is direct: the client's libGL draws with EGL and OpenGL ES
 * on Vulkan and puts the frame into the window with XzedPutImageRGB24.
 * The extension only answers what a client asks before it draws: whether
 * GLX is there (QueryExtension), its version (1.4), and its strings.
 * Indirect rendering requests are refused with BadRequest.
 */

#include "glx.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

/* GLX's minor opcodes Xzed answers. */
#define GLX_QUERY_VERSION		7U
#define GLX_QUERY_EXTENSIONS_STRING	18U
#define GLX_QUERY_SERVER_STRING		19U
#define GLX_CLIENT_INFO			20U

/* The names QueryServerString asks for. */
#define GLX_VENDOR			1U
#define GLX_VERSION			2U
#define GLX_EXTENSIONS			3U

/* X's BadRequest and BadLength errors. */
#define X_BAD_REQUEST			1
#define X_BAD_LENGTH			16

/* The version and strings of the extension. */
#define GLX_SERVER_MAJOR		1U
#define GLX_SERVER_MINOR		4U
static const char glx_vendor[] = "zedBSD";
static const char glx_version[] = "1.4";
static const char glx_extensions[] = "GLX_ARB_get_proc_address";

static uint32_t glx_read32(const struct xzed_reply_target *target, const uint8_t *bytes);
static void glx_write16(const struct xzed_reply_target *target, uint8_t *bytes, uint16_t value);
static void glx_write32(const struct xzed_reply_target *target, uint8_t *bytes, uint32_t value);
static void glx_send(const struct xzed_reply_target *target, uint8_t *reply, size_t length);
static void glx_string_reply(const struct xzed_reply_target *target, const char *text);

/*
 * Answers QueryExtension: GLX is present at its major opcode, with no
 * events or errors of its own; any other name is absent.
 */
void
xzed_query_extension(
	const struct xzed_reply_target *target,
	const uint8_t *request,
	size_t length)
{
	uint8_t reply[32];
	size_t name_length;
	int differs;

	/* An absent extension unless the name is GLX. */
	memset(reply, 0, sizeof(reply));
	name_length = 0U;
	if (length >= 8U)
		name_length = (size_t)request[4] | ((size_t)request[5] << 8);
	if (target->msb && length >= 8U)
		name_length = ((size_t)request[4] << 8) | (size_t)request[5];

	/* The name must fit in the request and be "GLX". */
	differs = 1;
	if (name_length == 3U && length >= 8U + name_length)
		differs = memcmp(request + 8, "GLX", 3U);
	if (differs == 0) {
		reply[8] = 1U;
		reply[9] = (uint8_t)XZED_GLX_MAJOR;
	}

	/* The reply. */
	glx_send(target, reply, sizeof(reply));
}

/*
 * Answers a GLX request: the version, the strings, and the client's
 * introduction; every other request is refused.
 */
int
xzed_glx_request(
	const struct xzed_reply_target *target,
	const uint8_t *request,
	size_t length)
{
	uint8_t reply[32];
	uint32_t name;

	/* The minor opcode decides. */
	if (length < 4U)
		return X_BAD_LENGTH;
	memset(reply, 0, sizeof(reply));

	/* What the request asks. */
	switch (request[1]) {
	case GLX_QUERY_VERSION:
		glx_write32(target, reply + 8, GLX_SERVER_MAJOR);
		glx_write32(target, reply + 12, GLX_SERVER_MINOR);
		glx_send(target, reply, sizeof(reply));
		return 0;
	case GLX_QUERY_EXTENSIONS_STRING:
		glx_string_reply(target, glx_extensions);
		return 0;
	case GLX_QUERY_SERVER_STRING:
		if (length < 12U)
			return X_BAD_LENGTH;
		name = glx_read32(target, request + 8);
		if (name == GLX_VENDOR)
			glx_string_reply(target, glx_vendor);
		else if (name == GLX_VERSION)
			glx_string_reply(target, glx_version);
		else
			glx_string_reply(target, glx_extensions);
		return 0;
	case GLX_CLIENT_INFO:
		return 0;
	default:
		break;
	}

	/* Indirect rendering and the rest are not there. */
	return X_BAD_REQUEST;
}

/* Reads a 32-bit value in the client's byte order. */
static uint32_t
glx_read32(
	const struct xzed_reply_target *target,
	const uint8_t *bytes)
{
	/* Most significant byte first, or least. */
	if (target->msb)
		return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
	return ((uint32_t)bytes[3] << 24) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[1] << 8) | (uint32_t)bytes[0];
}

/* Writes a 16-bit value in the client's byte order. */
static void
glx_write16(
	const struct xzed_reply_target *target,
	uint8_t *bytes,
	uint16_t value)
{
	/* Most significant byte first, or least. */
	if (target->msb) {
		bytes[0] = (uint8_t)(value >> 8);
		bytes[1] = (uint8_t)value;
	} else {
		bytes[0] = (uint8_t)value;
		bytes[1] = (uint8_t)(value >> 8);
	}
}

/* Writes a 32-bit value in the client's byte order. */
static void
glx_write32(
	const struct xzed_reply_target *target,
	uint8_t *bytes,
	uint32_t value)
{
	/* The two halves, in the order the client reads them. */
	if (target->msb) {
		glx_write16(target, bytes, (uint16_t)(value >> 16));
		glx_write16(target, bytes + 2, (uint16_t)value);
	} else {
		glx_write16(target, bytes, (uint16_t)value);
		glx_write16(target, bytes + 2, (uint16_t)(value >> 16));
	}
}

/* Sends a reply: its type, sequence number and extra length filled in. */
static void
glx_send(
	const struct xzed_reply_target *target,
	uint8_t *reply,
	size_t length)
{
	size_t sent;
	ssize_t wrote;

	/* A reply to the request's sequence number, with the words after its first 32 bytes. */
	reply[0] = 1U;
	glx_write16(target, reply + 2, target->sequence);
	glx_write32(target, reply + 4, (uint32_t)((length - 32U) / 4U));

	/* Every byte, through interruptions. */
	sent = 0U;
	while (sent < length) {
		wrote = write(target->fd, reply + sent, length - sent);
		if (wrote < 0 && errno == EINTR)
			continue;
		if (wrote <= 0)
			return;
		sent += (size_t)wrote;
	}
}

/* Sends a string reply (QueryServerString's and QueryExtensionsString's form): its length with the terminator, then the padded bytes. */
static void
glx_string_reply(
	const struct xzed_reply_target *target,
	const char *text)
{
	uint8_t reply[32 + 64];
	size_t bytes;
	size_t padded;

	/* The string with its terminator, padded to words. */
	bytes = strlen(text) + 1U;
	padded = (bytes + 3U) & ~(size_t)3U;
	if (padded > sizeof(reply) - 32U)
		return;
	memset(reply, 0, sizeof(reply));
	glx_write32(target, reply + 12, (uint32_t)bytes);
	memcpy(reply + 32, text, bytes);

	/* The reply. */
	glx_send(target, reply, 32U + padded);
}
