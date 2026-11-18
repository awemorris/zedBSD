/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The rules for reading one control request: when the bytes a connection
 * has sent make a whole request, and which requests carry the fields the
 * single-interface decoder reads.
 */

#include "userland/base/networkd/request-frame.h"

#include <errno.h>
#include <string.h>

/*
 * Reports whether a request ends with the client closing its direction.
 *
 * Every request but a subscription does, which is what proves there is
 * nothing more behind it.  A watcher must not: once one direction of a
 * stream is closed the kernel reports the socket hung up on every poll, so
 * a watcher keeps both directions open for as long as it watches, and its
 * request is whole as soon as its frame is.
 */
int
networkd_request_ends_with_close(
	uint32_t opcode)
{
	/* A subscription stays open after its frame. */
	if (opcode == NETWORKD_OP_SUBSCRIBE)
		return 0;

	/* Every other request is followed by the client's close. */
	return 1;
}

/*
 * Reports whether a request's payload is read by the single-interface decoder.
 *
 * A subscription carries nothing to decode, and a wired-management request
 * carries one record per interface, which does not fit a decoder for
 * operations that name a single one: it is read where it is acted on.
 */
int
networkd_request_fields_decoded(
	uint32_t opcode)
{
	/* A subscription has no fields. */
	if (opcode == NETWORKD_OP_SUBSCRIBE)
		return 0;

	/* The wired-management requests carry records of their own. */
	if (opcode == NETWORKD_OP_LAN_ENABLE)
		return 0;
	if (opcode == NETWORKD_OP_LAN_DISABLE)
		return 0;

	/* Every other request names its interface and options as fields. */
	return 1;
}

/*
 * Tells how far the bytes received on one connection go towards a request.
 *
 * bytes holds the used bytes received so far and closed says whether the
 * client has closed its direction.  A whole request gives its decoded
 * header and the length of the header line; the payload follows that line
 * in bytes.  An invalid one gives the error to answer it with.
 */
enum networkd_request_frame_state
networkd_request_frame_parse(
	const unsigned char *bytes,
	size_t used,
	int closed,
	struct networkd_protocol_header *header,
	size_t *header_length,
	int *error)
{
	const unsigned char *newline;
	size_t searched;
	size_t line;
	size_t payload;
	int decoded;
	int ends;

	*error = 0;
	*header_length = 0U;

	/* Finds the end of the header line within the longest header there is. */
	searched = used;
	if (searched > NETWORKD_PROTOCOL_HEADER_MAX)
		searched = NETWORKD_PROTOCOL_HEADER_MAX;
	newline = memchr(bytes, '\n', searched);

	/* Waits for the rest of a header line that may still come. */
	if (newline == NULL && used < NETWORKD_PROTOCOL_HEADER_MAX && !closed)
		return NETWORKD_REQUEST_FRAME_PARTIAL;

	/* Refuses a header line that is too long or was cut off by the close. */
	if (newline == NULL) {
		*error = EINVAL;
		return NETWORKD_REQUEST_FRAME_INVALID;
	}

	/* Decodes the header line. */
	line = (size_t)(newline - bytes) + 1U;
	errno = 0;
	decoded = networkd_protocol_header_decode((const char *)bytes, line,
	    header);
	if (decoded != 0) {
		*error = errno != 0 ? errno : EINVAL;
		return NETWORKD_REQUEST_FRAME_INVALID;
	}

	/* Refuses a payload larger than any request may carry. */
	if (header->payload_length > NETWORKD_REQUEST_MAX) {
		*error = EMSGSIZE;
		return NETWORKD_REQUEST_FRAME_INVALID;
	}
	payload = used - line;

	/* Waits for the rest of a payload that may still come. */
	if (payload < header->payload_length && !closed)
		return NETWORKD_REQUEST_FRAME_PARTIAL;

	/* Refuses a payload cut off by the close, or bytes after the frame. */
	if (payload != header->payload_length) {
		*error = EMSGSIZE;
		return NETWORKD_REQUEST_FRAME_INVALID;
	}

	/* Waits for the close that ends every request but a subscription. */
	ends = networkd_request_ends_with_close(header->opcode);
	if (ends && !closed)
		return NETWORKD_REQUEST_FRAME_PARTIAL;

	/* Succeeded: the request is whole. */
	*header_length = line;
	return NETWORKD_REQUEST_FRAME_COMPLETE;
}
