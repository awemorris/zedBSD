/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The HID Profile's transaction header (ws143-p005 i02, see hidp.h).
 */

#include "userland/base/bluetoothd/hidp.h"

#include <errno.h>

/*
 * Builds a header byte from a message type and its parameter.
 */
uint8_t
btd_hidp_header(
	unsigned type,
	unsigned parameter)
{
	/* The type above, the parameter below. */
	return (uint8_t)(((type & 0x0fU) << 4) | (parameter & 0x0fU));
}

/*
 * Takes a message apart.  Returns 0, EINVAL for an empty frame, or E2BIG
 * for a DATA report longer than bluetoothd passes on (the message is
 * still taken apart, for the caller to count).
 */
int
btd_hidp_parse(
	const uint8_t *frame,
	size_t length,
	struct btd_hidp *message)
{
	/* Refuses a frame without its header. */
	if (length == 0U)
		return EINVAL;

	/* The header, and what follows it. */
	message->type = frame[0] >> 4;
	message->parameter = frame[0] & 0x0fU;
	message->data = &frame[1];
	message->length = length - 1U;

	/* A report past the most the input bridge takes. */
	if (message->type == BTD_HIDP_DATA && message->length > BTD_HIDP_REPORT_MAX)
		return E2BIG;

	/* Succeeded. */
	return 0;
}
