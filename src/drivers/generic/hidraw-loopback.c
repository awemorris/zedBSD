/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The test kernel's loopback authenticator (ws161-p002, the user's
 * approval U4): a raw HID device with a FIDO report descriptor and no
 * hardware, so that the raw node can be tried in QEMU, which has no
 * CTAP2 device.  Test builds only (CONFIG_SECURITY_KEY_TEST_LOOPBACK).
 *
 * It answers the CTAPHID framing (FIDO CTAP 2.1 section 11.2) as far as
 * the node's tests need: INIT on the broadcast channel gives a new
 * channel, PING echoes its data (over as many packets as it takes), WINK
 * answers empty, and anything else answers ERROR with ERR_INVALID_CMD.
 * A packet out of order answers ERR_INVALID_SEQ.  The answer is handed to
 * the readers from the writing process's output, before its write
 * returns.  It holds no key and signs nothing.
 */

#include <drivers/generic/hidraw.h>

#include "kern/klog.h"
#include "kern/lock.h"
#include <kern/kcrt.h>
#include <uapi/errno.h>

/* The report's size, and the data an initialization and a continuation packet carry. */
#define LOOPBACK_REPORT		64U
#define LOOPBACK_INIT_DATA	(LOOPBACK_REPORT - 7U)
#define LOOPBACK_CONT_DATA	(LOOPBACK_REPORT - 5U)

/* The largest message it takes (the initialization packet and 128 continuations). */
#define LOOPBACK_MESSAGE_MAX	(LOOPBACK_INIT_DATA + 128U * LOOPBACK_CONT_DATA)

/* The CTAPHID commands and errors it knows (the command byte has its top bit set). */
#define LOOPBACK_CMD_PING	0x81U
#define LOOPBACK_CMD_INIT	0x86U
#define LOOPBACK_CMD_WINK	0x88U
#define LOOPBACK_CMD_ERROR	0xbfU
#define LOOPBACK_ERR_INVALID_CMD	0x01U
#define LOOPBACK_ERR_INVALID_LEN	0x03U
#define LOOPBACK_ERR_INVALID_SEQ	0x04U
#define LOOPBACK_BROADCAST	0xffffffffU

/* The CTAPHID protocol version and the capabilities INIT announces (WINK only). */
#define LOOPBACK_PROTOCOL	2U
#define LOOPBACK_CAPABILITY_WINK	0x01U

/*
 * The FIDO report descriptor of a security key: usage page 0xF1D0, usage
 * CTAPHID, 64 bytes of input (usage 0x20) and 64 of output (usage 0x21).
 */
static const uint8_t loopback_descriptor[] = {
	0x06, 0xd0, 0xf1,	/* Usage Page (FIDO Alliance) */
	0x09, 0x01,		/* Usage (CTAPHID) */
	0xa1, 0x01,		/* Collection (Application) */
	0x09, 0x20,		/*   Usage (Input Report Data) */
	0x15, 0x00,		/*   Logical Minimum (0) */
	0x26, 0xff, 0x00,	/*   Logical Maximum (255) */
	0x75, 0x08,		/*   Report Size (8) */
	0x95, 0x40,		/*   Report Count (64) */
	0x81, 0x02,		/*   Input (Data, Variable, Absolute) */
	0x09, 0x21,		/*   Usage (Output Report Data) */
	0x15, 0x00,		/*   Logical Minimum (0) */
	0x26, 0xff, 0x00,	/*   Logical Maximum (255) */
	0x75, 0x08,		/*   Report Size (8) */
	0x95, 0x40,		/*   Report Count (64) */
	0x91, 0x02,		/*   Output (Data, Variable, Absolute) */
	0xc0			/* End Collection */
};

/*
 * The authenticator's state, one for the kernel's life, guarded by the
 * device's output lock (hidraw runs one output at a time): the message
 * being received (its channel, command, length, the bytes so far and the
 * next sequence number) and the last channel given out.
 */
struct loopback_state {
	struct drv_hidraw *hidraw;
	uint32_t channel;
	uint8_t command;
	size_t length;
	size_t received;
	uint8_t next_sequence;
	unsigned receiving;
	uint32_t last_channel;
	uint8_t message[LOOPBACK_MESSAGE_MAX];
	uint8_t reply[LOOPBACK_REPORT];
};

/* The one loopback authenticator, zero until it is registered. */
static struct loopback_state loopback;

static int loopback_output(void *context, const uint8_t *report, size_t length);
static void loopback_answer(struct loopback_state *state, uint32_t channel, uint8_t command, const uint8_t *data, size_t length);
static void loopback_error(struct loopback_state *state, uint32_t channel, uint8_t code);
static void loopback_dispatch(struct loopback_state *state);
static uint32_t loopback_be32(const uint8_t *bytes);
static void loopback_put_be32(uint8_t *bytes, uint32_t value);

/* What the loopback authenticator's transport does: its output reports. */
static const struct drv_hidraw_ops loopback_ops = {
	.output = loopback_output
};

/*
 * Publishes the loopback authenticator (test builds: called once at boot).
 * Returns 0 or the error of publishing it.
 */
int
drv_hidraw_loopback_register(
	void)
{
	struct drv_hidraw_description description;
	int error;

	/* What the device is: a virtual FIDO key with 64-byte reports. */
	kern_memset(&description, 0, sizeof(description));
	description.info.bus = HIDRAW_BUS_VIRTUAL;
	description.info.vendor = 0x1209U;
	description.info.product = 0xf1d0U;
	description.info.version = 0x0100U;
	description.info.usage_page = HIDRAW_USAGE_PAGE_FIDO;
	description.info.usage = HIDRAW_USAGE_CTAPHID;
	description.info.input_size = LOOPBACK_REPORT;
	description.info.output_size = LOOPBACK_REPORT;
	description.name = "Loopback FIDO authenticator (test)";
	description.physical_path = "virtual/hidraw-loopback";
	description.descriptor = loopback_descriptor;
	description.descriptor_size = sizeof(loopback_descriptor);

	/* The node. */
	loopback.last_channel = 0x10000000U;
	error = drv_hidraw_register(&description, &loopback_ops, &loopback, &loopback.hidraw);
	if (error != 0)
		return error;

	/* Succeeded: the test authenticator is there. */
	kern_logf("hidraw-loopback: test authenticator published\n");
	return 0;
}

/*
 * Takes one output report (the ID's byte, 0, then 64 bytes): a packet of
 * a message, which is answered once it is whole.
 */
static int
loopback_output(
	void *context,
	const uint8_t *report,
	size_t length)
{
	struct loopback_state *state;
	const uint8_t *packet;
	uint32_t channel;
	size_t take;
	uint8_t first;

	/* A whole packet after the report ID's byte. */
	state = context;
	if (length != LOOPBACK_REPORT + 1U)
		return EINVAL;
	packet = report + 1U;
	channel = loopback_be32(packet);
	first = packet[4];

	/* An initialization packet starts a message (and drops one unfinished). */
	if ((first & 0x80U) != 0U) {
		state->channel = channel;
		state->command = first;
		state->length = ((size_t)packet[5] << 8U) | packet[6];
		state->received = 0U;
		state->next_sequence = 0U;
		state->receiving = 1U;
		if (state->length > LOOPBACK_MESSAGE_MAX) {
			state->receiving = 0U;
			loopback_error(state, channel, LOOPBACK_ERR_INVALID_LEN);
			return 0;
		}

		/* Its data, as much as the message has. */
		take = state->length;
		if (take > LOOPBACK_INIT_DATA)
			take = LOOPBACK_INIT_DATA;
		kern_memcpy(state->message, packet + 7U, take);
		state->received = take;
	} else {
		/* A continuation belongs to the message of its channel, in order. */
		if (!state->receiving || channel != state->channel)
			return 0;
		if (first != state->next_sequence) {
			state->receiving = 0U;
			loopback_error(state, channel, LOOPBACK_ERR_INVALID_SEQ);
			return 0;
		}

		/* The next continuation's number. */
		state->next_sequence++;

		/* Its data, as much as the message still needs. */
		take = state->length - state->received;
		if (take > LOOPBACK_CONT_DATA)
			take = LOOPBACK_CONT_DATA;
		kern_memcpy(state->message + state->received, packet + 5U, take);
		state->received += take;
	}

	/* A whole message is answered. */
	if (state->received == state->length) {
		state->receiving = 0U;
		loopback_dispatch(state);
	}

	/* Succeeded: the packet was taken. */
	return 0;
}

/* Answers a whole message by its command. */
static void
loopback_dispatch(
	struct loopback_state *state)
{
	uint8_t init[17];

	/* Each command it knows. */
	switch (state->command) {
	case LOOPBACK_CMD_INIT:
		/* The nonce back, a new channel, the version and the capabilities. */
		if (state->length != 8U) {
			loopback_error(state, state->channel, LOOPBACK_ERR_INVALID_LEN);
			return;
		}

		/* The answer's 17 bytes. */
		kern_memcpy(init, state->message, 8U);
		state->last_channel++;
		loopback_put_be32(init + 8U, state->last_channel);
		init[12] = LOOPBACK_PROTOCOL;
		init[13] = 1U;
		init[14] = 0U;
		init[15] = 0U;
		init[16] = LOOPBACK_CAPABILITY_WINK;
		loopback_answer(state, state->channel, LOOPBACK_CMD_INIT, init, sizeof(init));
		return;
	case LOOPBACK_CMD_PING:
		/* The data back. */
		loopback_answer(state, state->channel, LOOPBACK_CMD_PING, state->message, state->length);
		return;
	case LOOPBACK_CMD_WINK:
		/* Nothing to show; an empty answer. */
		loopback_answer(state, state->channel, LOOPBACK_CMD_WINK, NULL, 0U);
		return;
	default:
		break;
	}

	/* Anything else is a command it does not have. */
	loopback_error(state, state->channel, LOOPBACK_ERR_INVALID_CMD);
}

/* Sends a message to the readers: an initialization packet and as many continuations as it takes. */
static void
loopback_answer(
	struct loopback_state *state,
	uint32_t channel,
	uint8_t command,
	const uint8_t *data,
	size_t length)
{
	size_t sent;
	size_t take;
	uint8_t sequence;

	/* The initialization packet: the channel, the command, the length and the first bytes. */
	kern_memset(state->reply, 0, sizeof(state->reply));
	loopback_put_be32(state->reply, channel);
	state->reply[4] = command;
	state->reply[5] = (uint8_t)(length >> 8U);
	state->reply[6] = (uint8_t)length;
	take = length;
	if (take > LOOPBACK_INIT_DATA)
		take = LOOPBACK_INIT_DATA;
	if (take != 0U)
		kern_memcpy(state->reply + 7U, data, take);
	drv_hidraw_input(state->hidraw, state->reply, sizeof(state->reply));
	sent = take;

	/* The continuations, numbered from 0. */
	sequence = 0U;
	while (sent < length) {
		kern_memset(state->reply, 0, sizeof(state->reply));
		loopback_put_be32(state->reply, channel);
		state->reply[4] = sequence;
		take = length - sent;
		if (take > LOOPBACK_CONT_DATA)
			take = LOOPBACK_CONT_DATA;
		kern_memcpy(state->reply + 5U, data + sent, take);
		drv_hidraw_input(state->hidraw, state->reply, sizeof(state->reply));
		sent += take;
		sequence++;
	}
}

/* Sends an ERROR message with one code. */
static void
loopback_error(
	struct loopback_state *state,
	uint32_t channel,
	uint8_t code)
{
	/* The one byte of the error. */
	loopback_answer(state, channel, LOOPBACK_CMD_ERROR, &code, 1U);
}

/* Reads a 32-bit big-endian number (a channel). */
static uint32_t
loopback_be32(
	const uint8_t *bytes)
{
	/* The most significant byte first. */
	return ((uint32_t)bytes[0] << 24U) | ((uint32_t)bytes[1] << 16U) |
	    ((uint32_t)bytes[2] << 8U) | (uint32_t)bytes[3];
}

/* Writes a 32-bit big-endian number (a channel). */
static void
loopback_put_be32(
	uint8_t *bytes,
	uint32_t value)
{
	/* The most significant byte first. */
	bytes[0] = (uint8_t)(value >> 24U);
	bytes[1] = (uint8_t)(value >> 16U);
	bytes[2] = (uint8_t)(value >> 8U);
	bytes[3] = (uint8_t)value;
}
