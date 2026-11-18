/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libpasskey's CTAPHID (hid.h; ws161-p004).
 *
 * A message is an initialization packet (channel, command, length, the
 * first PK_HID_INIT_DATA bytes) and continuation packets (channel,
 * sequence 0 to 127, PK_HID_CONT_DATA bytes each).  Reports are read
 * against one deadline for the whole transaction, KEEPALIVE included, so a
 * key that keeps the user waiting is given up at the caller's time.
 */

#include "hid.h"

#include "crypto.h"

#include <errno.h>
#include <string.h>
#include <time.h>

/* The size of INIT's nonce and of its answer. */
#define HID_NONCE		8U
#define HID_INIT_ANSWER		17U

static int hid_send(struct pk_hid *hid, uint32_t channel, uint8_t command, const uint8_t *data, size_t size);
static int hid_receive(struct pk_hid *hid, uint32_t channel, uint8_t *command, uint8_t *data, size_t capacity,
    size_t *size, uint64_t deadline, pk_hid_keepalive_t keepalive, void *keepalive_context);
static uint64_t hid_now_ms(void);
static unsigned hid_left(uint64_t deadline);
static uint32_t hid_be32(const uint8_t *bytes);
static void hid_put_be32(uint8_t *bytes, uint32_t value);

/*
 * Takes a channel of a key: INIT on the broadcast channel with a random
 * nonce, and the answer that carries the same nonce.  Returns 0, ETIMEDOUT,
 * EPROTO for a key whose answer is not INIT's, or the reports' error.
 */
int
pk_hid_open(
	struct pk_hid *hid,
	const struct pk_hid_io *io,
	unsigned timeout_ms)
{
	uint8_t nonce[HID_NONCE];
	uint8_t answer[HID_INIT_ANSWER + 16U];
	uint64_t deadline;
	uint8_t command;
	size_t size;
	int same;
	int error;

	/* The reports, and a nonce of its own. */
	memset(hid, 0, sizeof(*hid));
	hid->io = *io;
	error = pk_crypto_random(nonce, sizeof(nonce));
	if (error != 0)
		return error;

	/* INIT on the broadcast channel. */
	error = hid_send(hid, PK_HID_BROADCAST, PK_HID_INIT, nonce, sizeof(nonce));
	if (error != 0)
		return error;

	/* Its answer: the broadcast channel's INIT with this nonce (another program's answer is skipped). */
	deadline = hid_now_ms() + timeout_ms;
	for (;;) {
		error = hid_receive(hid, PK_HID_BROADCAST, &command, answer, sizeof(answer), &size, deadline, NULL, NULL);
		if (error != 0)
			return error;
		if (command != PK_HID_INIT || size < HID_INIT_ANSWER)
			continue;
		same = memcmp(answer, nonce, sizeof(nonce));
		if (same == 0)
			break;
	}

	/* The new channel and what the key says of itself. */
	hid->channel = hid_be32(answer + 8U);
	hid->protocol = answer[12];
	hid->version[0] = answer[13];
	hid->version[1] = answer[14];
	hid->version[2] = answer[15];
	hid->capabilities = answer[16];
	if (hid->channel == 0U || hid->channel == PK_HID_BROADCAST)
		return EPROTO;

	/* Succeeded: the channel is the caller's. */
	return 0;
}

/*
 * Sends one message on the channel and gives its answer (the command, and
 * the data in reply).  KEEPALIVE is told to keepalive and waited past, up
 * to the deadline.  Returns 0, ETIMEDOUT, EMSGSIZE, or the reports' error;
 * an ERROR answer is an answer (*reply_command PK_HID_ERROR, the code in
 * reply[0]).
 */
int
pk_hid_transact(
	struct pk_hid *hid,
	uint8_t command,
	const uint8_t *data,
	size_t size,
	uint8_t *reply_command,
	uint8_t *reply,
	size_t capacity,
	size_t *reply_size,
	unsigned timeout_ms,
	pk_hid_keepalive_t keepalive,
	void *keepalive_context)
{
	uint64_t deadline;
	int error;

	/* The message. */
	deadline = hid_now_ms() + timeout_ms;
	error = hid_send(hid, hid->channel, command, data, size);
	if (error != 0)
		return error;

	/* Its answer. */
	error = hid_receive(hid, hid->channel, reply_command, reply, capacity, reply_size, deadline, keepalive,
	    keepalive_context);
	if (error != 0)
		return error;

	/* Succeeded: the answer. */
	return 0;
}

/* Asks the key to stop the transaction under way on the channel (it then answers it with an error). */
int
pk_hid_cancel(
	struct pk_hid *hid)
{
	int error;

	/* CANCEL, with no data and no answer of its own. */
	error = hid_send(hid, hid->channel, PK_HID_CANCEL, NULL, 0U);
	if (error != 0)
		return error;

	/* Succeeded: asked. */
	return 0;
}

/* Sends a message: the initialization packet, then the continuations. */
static int
hid_send(
	struct pk_hid *hid,
	uint32_t channel,
	uint8_t command,
	const uint8_t *data,
	size_t size)
{
	uint8_t report[PK_HID_REPORT + 1U];
	size_t sent;
	size_t take;
	uint8_t sequence;
	int error;

	/* A message that fits. */
	if (size > PK_HID_MESSAGE_MAX)
		return EMSGSIZE;

	/* The initialization packet, after the report ID's byte (0). */
	memset(report, 0, sizeof(report));
	hid_put_be32(report + 1U, channel);
	report[5] = command;
	report[6] = (uint8_t)(size >> 8U);
	report[7] = (uint8_t)size;
	take = size;
	if (take > PK_HID_INIT_DATA)
		take = PK_HID_INIT_DATA;
	if (take != 0U)
		memcpy(report + 8U, data, take);
	error = hid->io.write(hid->io.context, report, sizeof(report));
	if (error != 0)
		return error;

	/* The continuations, numbered from 0. */
	sent = take;
	sequence = 0U;
	while (sent < size) {
		memset(report, 0, sizeof(report));
		hid_put_be32(report + 1U, channel);
		report[5] = sequence;
		take = size - sent;
		if (take > PK_HID_CONT_DATA)
			take = PK_HID_CONT_DATA;
		memcpy(report + 6U, data + sent, take);
		error = hid->io.write(hid->io.context, report, sizeof(report));
		if (error != 0)
			return error;
		sent += take;
		sequence++;
	}

	/* Succeeded: the message went. */
	return 0;
}

/*
 * Receives the channel's next message by the deadline: other channels'
 * reports and stray continuations are skipped, KEEPALIVE is told and
 * skipped.  Returns 0, ETIMEDOUT, EMSGSIZE, EPROTO for continuations out of
 * order, or the reports' error.
 */
static int
hid_receive(
	struct pk_hid *hid,
	uint32_t channel,
	uint8_t *command,
	uint8_t *data,
	size_t capacity,
	size_t *size,
	uint64_t deadline,
	pk_hid_keepalive_t keepalive,
	void *keepalive_context)
{
	uint8_t report[PK_HID_REPORT];
	size_t total;
	size_t received;
	size_t take;
	uint8_t sequence;
	int error;

	/* The channel's initialization packet, KEEPALIVE told and skipped. */
	for (;;) {
		error = hid->io.read(hid->io.context, report, sizeof(report), hid_left(deadline));
		if (error != 0)
			return error;
		if (hid_be32(report) != channel || (report[4] & 0x80U) == 0U)
			continue;
		if (report[4] != PK_HID_KEEPALIVE)
			break;
		if (keepalive != NULL)
			keepalive(keepalive_context, report[7]);
	}

	/* The command, the length and the first bytes. */
	*command = report[4];
	total = ((size_t)report[5] << 8U) | report[6];
	if (total > capacity || total > PK_HID_MESSAGE_MAX)
		return EMSGSIZE;
	take = total;
	if (take > PK_HID_INIT_DATA)
		take = PK_HID_INIT_DATA;
	memcpy(data, report + 7U, take);
	received = take;

	/* The continuations, in order. */
	sequence = 0U;
	while (received < total) {
		error = hid->io.read(hid->io.context, report, sizeof(report), hid_left(deadline));
		if (error != 0)
			return error;
		if (hid_be32(report) != channel)
			continue;
		if (report[4] != sequence)
			return EPROTO;
		take = total - received;
		if (take > PK_HID_CONT_DATA)
			take = PK_HID_CONT_DATA;
		memcpy(data + received, report + 5U, take);
		received += take;
		sequence++;
	}

	/* Succeeded: the whole message. */
	*size = total;
	return 0;
}

/* Gives the monotonic clock in milliseconds. */
static uint64_t
hid_now_ms(
	void)
{
	struct timespec now;

	/* The clock, which a change of the time of day does not move. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/* Gives the milliseconds left before a deadline (0 once it passed: a read then answers ETIMEDOUT). */
static unsigned
hid_left(
	uint64_t deadline)
{
	uint64_t now;

	/* Now, against the deadline. */
	now = hid_now_ms();
	if (now >= deadline)
		return 0U;

	/* What is left. */
	return (unsigned)(deadline - now);
}

/* Reads a 32-bit big-endian number (a channel). */
static uint32_t
hid_be32(
	const uint8_t *bytes)
{
	/* The most significant byte first. */
	return ((uint32_t)bytes[0] << 24U) | ((uint32_t)bytes[1] << 16U) | ((uint32_t)bytes[2] << 8U) |
	    (uint32_t)bytes[3];
}

/* Writes a 32-bit big-endian number (a channel). */
static void
hid_put_be32(
	uint8_t *bytes,
	uint32_t value)
{
	/* The most significant byte first. */
	bytes[0] = (uint8_t)(value >> 24U);
	bytes[1] = (uint8_t)(value >> 16U);
	bytes[2] = (uint8_t)(value >> 8U);
	bytes[3] = (uint8_t)value;
}
