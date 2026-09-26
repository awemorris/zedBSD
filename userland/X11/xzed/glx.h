/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Xzed's extensions (WS069 p004): QueryExtension and GLX's queries.
 */

#ifndef XZED_GLX_H
#define XZED_GLX_H

#include <stddef.h>
#include <stdint.h>

/* GLX's major opcode. */
#define XZED_GLX_MAJOR		144U

/*
 * Where a reply goes: the client's socket, the request's sequence number,
 * and the client's byte order (nonzero: most significant byte first).
 */
struct xzed_reply_target {
	int fd;
	uint16_t sequence;
	int msb;
};

/* Answers QueryExtension: GLX is the one extension. */
void xzed_query_extension(const struct xzed_reply_target *target, const uint8_t *request, size_t length);

/* Answers a GLX request; returns 0, or the X error code to send when it is not one Xzed answers. */
int xzed_glx_request(const struct xzed_reply_target *target, const uint8_t *request, size_t length);

#endif
