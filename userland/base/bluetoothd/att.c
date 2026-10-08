/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's Attribute Protocol (ws143-p005 i02, see att.h).
 *
 * A builder returns the PDU's length, or 0 when the buffer is too small.
 */

#include "userland/base/bluetoothd/att.h"

#include <errno.h>
#include <string.h>

/* The lengths of the fixed PDUs: an error response, an MTU exchange, a Find Information request, a Read Blob request. */
#define ATT_ERROR_LENGTH	5U
#define ATT_MTU_LENGTH		3U
#define ATT_RANGE_LENGTH	5U
#define ATT_BLOB_LENGTH		5U

static int att_parse_list(const uint8_t *pdu, size_t length, size_t element, size_t minimum, struct btd_att_pdu *parsed);
static uint16_t att_le16(const uint8_t *bytes);
static void att_put16(uint8_t *bytes, uint16_t value);

/*
 * Builds an Exchange MTU request or response.
 */
size_t
btd_att_build_mtu(
	uint8_t *out,
	size_t size,
	uint8_t opcode,
	uint16_t mtu)
{
	/* Refuses a buffer too small. */
	if (size < ATT_MTU_LENGTH)
		return 0U;

	/* The opcode and the MTU. */
	out[0] = opcode;
	att_put16(&out[1], mtu);
	return ATT_MTU_LENGTH;
}

/*
 * Builds a request over a range of handles: Find Information (no type),
 * or Read By Type and Read By Group Type with a 16-bit type.
 */
size_t
btd_att_build_range(
	uint8_t *out,
	size_t size,
	uint8_t opcode,
	uint16_t start,
	uint16_t end,
	uint16_t uuid)
{
	size_t length;

	/* The length, with the type unless Find Information. */
	length = ATT_RANGE_LENGTH;
	if (opcode != BTD_ATT_FIND_INFO_REQUEST)
		length += 2U;
	if (size < length)
		return 0U;

	/* The opcode, the range and the type. */
	out[0] = opcode;
	att_put16(&out[1], start);
	att_put16(&out[3], end);
	if (opcode != BTD_ATT_FIND_INFO_REQUEST)
		att_put16(&out[5], uuid);
	return length;
}

/*
 * Builds a Read request, or a Read Blob request at an offset.
 */
size_t
btd_att_build_read(
	uint8_t *out,
	size_t size,
	uint16_t handle,
	uint16_t offset,
	int blob)
{
	/* A Read Blob. */
	if (blob) {
		if (size < ATT_BLOB_LENGTH)
			return 0U;
		out[0] = BTD_ATT_READ_BLOB_REQUEST;
		att_put16(&out[1], handle);
		att_put16(&out[3], offset);
		return ATT_BLOB_LENGTH;
	}

	/* A Read. */
	if (size < 3U)
		return 0U;
	out[0] = BTD_ATT_READ_REQUEST;
	att_put16(&out[1], handle);
	return 3U;
}

/*
 * Builds a Write request or a Write command: the handle and the value.
 */
size_t
btd_att_build_write(
	uint8_t *out,
	size_t size,
	uint8_t opcode,
	uint16_t handle,
	const uint8_t *value,
	size_t length)
{
	/* Refuses a buffer too small. */
	if (size < 3U || length > size - 3U)
		return 0U;

	/* The opcode, the handle and the value. */
	out[0] = opcode;
	att_put16(&out[1], handle);
	memcpy(&out[3], value, length);
	return 3U + length;
}

/*
 * Builds a Handle Value Confirmation (the answer to an indication).
 */
size_t
btd_att_build_confirmation(
	uint8_t *out,
	size_t size)
{
	/* Refuses no room. */
	if (size < 1U)
		return 0U;

	/* The opcode alone. */
	out[0] = BTD_ATT_CONFIRMATION;
	return 1U;
}

/*
 * Builds an Error Response to a request.
 */
size_t
btd_att_build_error(
	uint8_t *out,
	size_t size,
	uint8_t request,
	uint16_t handle,
	uint8_t error)
{
	/* Refuses a buffer too small. */
	if (size < ATT_ERROR_LENGTH)
		return 0U;

	/* The opcode, the request in error, its handle and the code. */
	out[0] = BTD_ATT_ERROR;
	out[1] = request;
	att_put16(&out[2], handle);
	out[4] = error;
	return ATT_ERROR_LENGTH;
}

/*
 * Takes a PDU apart, its length checked against its opcode's form (a
 * list's elements must fill it exactly).  Returns 0, EINVAL for a PDU of
 * the wrong length, or EPROTO for an opcode bluetoothd does not know.
 */
int
btd_att_parse(
	const uint8_t *pdu,
	size_t length,
	struct btd_att_pdu *parsed)
{
	/* Refuses nothing at all. */
	if (length == 0U)
		return EINVAL;
	memset(parsed, 0, sizeof(*parsed));
	parsed->opcode = pdu[0];

	/* Each opcode's form. */
	switch (pdu[0]) {
	case BTD_ATT_ERROR:
		/* The request, its handle and the code. */
		if (length != ATT_ERROR_LENGTH)
			return EINVAL;
		parsed->request = pdu[1];
		parsed->handle = att_le16(&pdu[2]);
		parsed->error = pdu[4];
		return 0;
	case BTD_ATT_MTU_REQUEST:
	case BTD_ATT_MTU_RESPONSE:
		/* The MTU. */
		if (length != ATT_MTU_LENGTH)
			return EINVAL;
		parsed->mtu = att_le16(&pdu[1]);
		return 0;
	case BTD_ATT_FIND_INFO_REQUEST:
		/* The range. */
		if (length != ATT_RANGE_LENGTH)
			return EINVAL;
		parsed->handle = att_le16(&pdu[1]);
		parsed->end = att_le16(&pdu[3]);
		return 0;
	case BTD_ATT_READ_BY_TYPE_REQUEST:
	case BTD_ATT_READ_GROUP_REQUEST:
		/* The range and a 16-bit or 128-bit type (the short form kept). */
		if (length != ATT_RANGE_LENGTH + 2U && length != ATT_RANGE_LENGTH + 16U)
			return EINVAL;
		parsed->handle = att_le16(&pdu[1]);
		parsed->end = att_le16(&pdu[3]);
		if (length == ATT_RANGE_LENGTH + 2U)
			parsed->uuid = att_le16(&pdu[5]);
		return 0;
	case BTD_ATT_FIND_INFO_RESPONSE:
		/* The format, then handle and UUID pairs. */
		if (length < 2U)
			return EINVAL;
		parsed->format = pdu[1];
		if (pdu[1] == BTD_ATT_FORMAT_16)
			return att_parse_list(pdu, length, 4U, 4U, parsed);
		if (pdu[1] == BTD_ATT_FORMAT_128)
			return att_parse_list(pdu, length, 18U, 18U, parsed);
		return EINVAL;
	case BTD_ATT_READ_BY_TYPE_RESPONSE:
		/* Each element a handle and a value. */
		if (length < 2U)
			return EINVAL;
		return att_parse_list(pdu, length, pdu[1], 2U, parsed);
	case BTD_ATT_READ_GROUP_RESPONSE:
		/* Each element a handle, its group's end and a value. */
		if (length < 2U)
			return EINVAL;
		return att_parse_list(pdu, length, pdu[1], 4U, parsed);
	case BTD_ATT_READ_REQUEST:
		/* The handle. */
		if (length != 3U)
			return EINVAL;
		parsed->handle = att_le16(&pdu[1]);
		return 0;
	case BTD_ATT_READ_BLOB_REQUEST:
		/* The handle and the offset. */
		if (length != ATT_BLOB_LENGTH)
			return EINVAL;
		parsed->handle = att_le16(&pdu[1]);
		parsed->offset = att_le16(&pdu[3]);
		return 0;
	case BTD_ATT_READ_RESPONSE:
	case BTD_ATT_READ_BLOB_RESPONSE:
		/* The value (empty allowed). */
		parsed->value = &pdu[1];
		parsed->length = length - 1U;
		return 0;
	case BTD_ATT_WRITE_RESPONSE:
	case BTD_ATT_CONFIRMATION:
		/* The opcode alone. */
		if (length != 1U)
			return EINVAL;
		return 0;
	case BTD_ATT_FIND_BY_VALUE_REQUEST:
	case BTD_ATT_READ_MULTIPLE_REQUEST:
	case BTD_ATT_PREPARE_WRITE_REQUEST:
		/* A handle first, and more. */
		if (length < 5U)
			return EINVAL;
		parsed->handle = att_le16(&pdu[1]);
		parsed->value = &pdu[3];
		parsed->length = length - 3U;
		return 0;
	case BTD_ATT_EXECUTE_WRITE_REQUEST:
		/* The flags. */
		if (length != 2U)
			return EINVAL;
		return 0;
	case BTD_ATT_WRITE_REQUEST:
	case BTD_ATT_WRITE_COMMAND:
	case BTD_ATT_SIGNED_WRITE_COMMAND:
	case BTD_ATT_NOTIFICATION:
	case BTD_ATT_INDICATION:
		/* A handle and a value. */
		if (length < 3U)
			return EINVAL;
		parsed->handle = att_le16(&pdu[1]);
		parsed->value = &pdu[3];
		parsed->length = length - 3U;
		return 0;
	default:
		/* Not known. */
		return EPROTO;
	}
}

/*
 * Answers a request of the peer as a server without attributes: the MTU
 * exchange with bluetoothd's MTU, the discovery and read requests with
 * Attribute Not Found, the writes with Request Not Supported, a malformed
 * request with Invalid PDU; nothing to a command, a response, a
 * notification, an indication or a confirmation (out_length 0).
 * Returns 0, or ENOSPC.
 */
int
btd_att_answer(
	const uint8_t *request,
	size_t length,
	uint8_t *out,
	size_t size,
	size_t *out_length)
{
	struct btd_att_pdu parsed;
	uint8_t code;
	int error;

	/* Nothing to answer: a command, an odd opcode (a response or a peer's notice) or a confirmation. */
	*out_length = 0;
	if (length == 0U)
		return 0;
	if ((request[0] & BTD_ATT_COMMAND_FLAG) != 0U || (request[0] & 1U) != 0U || request[0] == BTD_ATT_CONFIRMATION)
		return 0;

	/* The request, a malformed one answered as such. */
	error = btd_att_parse(request, length, &parsed);
	if (error == EINVAL) {
		*out_length = btd_att_build_error(out, size, request[0], 0, BTD_ATT_INVALID_PDU);
		if (*out_length == 0U)
			return ENOSPC;
		return 0;
	}

	/* The MTU exchange. */
	if (request[0] == BTD_ATT_MTU_REQUEST) {
		*out_length = btd_att_build_mtu(out, size, BTD_ATT_MTU_RESPONSE, BTD_ATT_MTU);
		if (*out_length == 0U)
			return ENOSPC;
		return 0;
	}

	/* No attributes to find or read; nothing to write; any other request not supported. */
	switch (request[0]) {
	case BTD_ATT_FIND_INFO_REQUEST:
	case BTD_ATT_FIND_BY_VALUE_REQUEST:
	case BTD_ATT_READ_BY_TYPE_REQUEST:
	case BTD_ATT_READ_REQUEST:
	case BTD_ATT_READ_BLOB_REQUEST:
	case BTD_ATT_READ_MULTIPLE_REQUEST:
	case BTD_ATT_READ_GROUP_REQUEST:
		code = BTD_ATT_NOT_FOUND;
		break;
	default:
		code = BTD_ATT_REQUEST_NOT_SUPPORTED;
		break;
	}

	/* The error response at the request's (first) handle. */
	*out_length = btd_att_build_error(out, size, request[0], parsed.handle, code);
	if (*out_length == 0U)
		return ENOSPC;

	/* Succeeded: answered. */
	return 0;
}

/*
 * Takes apart a response's list: elements of one length (at least the
 * minimum) after the opcode and the length byte, filling it exactly.
 */
static int
att_parse_list(
	const uint8_t *pdu,
	size_t length,
	size_t element,
	size_t minimum,
	struct btd_att_pdu *parsed)
{
	/* Refuses elements too short, an empty list, or a list that is not whole elements. */
	if (element < minimum || length <= 2U)
		return EINVAL;
	if ((length - 2U) % element != 0U)
		return EINVAL;

	/* The list. */
	parsed->list = &pdu[2];
	parsed->element = element;
	parsed->count = (length - 2U) / element;
	return 0;
}

/* Reads a little-endian 16-bit number. */
static uint16_t
att_le16(
	const uint8_t *bytes)
{
	/* Least significant byte first. */
	return (uint16_t)(bytes[0] | ((unsigned)bytes[1] << 8));
}

/* Writes a little-endian 16-bit number. */
static void
att_put16(
	uint8_t *bytes,
	uint16_t value)
{
	/* Least significant byte first. */
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}
