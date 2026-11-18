/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The rules for reading one control request, shared by every path that
 * reads one.
 *
 * networkd reads a request in two places: the event loop reads one
 * connection at a time with a blocking timed read, and a wait inside long
 * work gathers the bytes of several connections without blocking.  Both
 * must agree on when a request is whole and on which requests have their
 * fields decoded, or a request read on one path is refused on the other.
 * This part knows only the bytes and the opcodes, nothing of the sockets,
 * so the host tests run it alone.
 */

#ifndef KERN_NETWORKD_REQUEST_FRAME_H
#define KERN_NETWORKD_REQUEST_FRAME_H

#include "userland/base/net/protocol.h"

#include <stddef.h>
#include <stdint.h>

/* How far the bytes a connection has sent go towards one request. */
enum networkd_request_frame_state {
	/* More bytes, or the client's closing its direction, are still to come. */
	NETWORKD_REQUEST_FRAME_PARTIAL,

	/* The request is whole: the header is decoded and the payload follows it. */
	NETWORKD_REQUEST_FRAME_COMPLETE,

	/* The bytes can never become a request; the error says why. */
	NETWORKD_REQUEST_FRAME_INVALID
};

int networkd_request_ends_with_close(uint32_t opcode);
int networkd_request_fields_decoded(uint32_t opcode);
enum networkd_request_frame_state networkd_request_frame_parse(const unsigned char *bytes, size_t used, int closed, struct networkd_protocol_header *header, size_t *header_length, int *error);

#endif
