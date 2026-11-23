/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The link between mikuterm and its X server process: frames, channels
 * and credit.
 */

#include "x11-link.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The longest HELLO and OPEN bodies. */
#define LINK_HELLO_MAX		1024U
#define LINK_OPEN_MAX		256U

/* The body of a CREDIT: the bytes given, 32 bits. */
#define LINK_CREDIT_SIZE	4U

static int link_check_control(const struct x11_link_table *table, const struct x11_link_frame *frame);
static int link_check_open(struct x11_link_table *table, const struct x11_link_frame *frame);
static int link_check_tab(const struct x11_link_table *table, const struct x11_link_frame *frame);

/*
 * Reads one frame from the front of bytes (size of them).  Returns 0 with
 * *frame and *used (the frame's bytes) set, EAGAIN when the frame is not
 * whole yet, or EPROTO for a body longer than a frame carries (the link
 * cannot be read any further).
 */
int
x11_link_parse(
	const uint8_t *bytes,
	size_t size,
	struct x11_link_frame *frame,
	size_t *used)
{
	uint32_t length;

	/* The header first. */
	if (size < X11_LINK_HEADER)
		return EAGAIN;

	/* A body longer than any frame is the end of the link. */
	length = x11_link_get32(bytes + 5);
	if (length > X11_LINK_BODY_MAX)
		return EPROTO;

	/* The whole body. */
	if (size - X11_LINK_HEADER < length)
		return EAGAIN;

	/* Succeeded: the frame. */
	frame->type = bytes[0];
	frame->id = x11_link_get32(bytes + 1);
	frame->size = length;
	frame->body = bytes + X11_LINK_HEADER;
	*used = X11_LINK_HEADER + (size_t)length;
	return 0;
}

/*
 * Writes a frame's header (X11_LINK_HEADER bytes).
 */
void
x11_link_header(
	uint8_t *header,
	unsigned type,
	uint32_t id,
	uint32_t size)
{
	/* The type, the link id and the body's length. */
	header[0] = (uint8_t)type;
	x11_link_put32(header + 1, id);
	x11_link_put32(header + 5, size);
}

/*
 * Starts a side's table of a new link: no channel, and the first id the
 * parent gives is 1.
 */
void
x11_link_table_init(
	struct x11_link_table *table,
	int side)
{
	/* Nothing open. */
	memset(table, 0, sizeof(*table));
	table->side = side;
	table->next_id = 1;
}

/*
 * Finds an open channel by its id (NULL: not open).
 */
struct x11_link_channel *
x11_link_find(
	struct x11_link_table *table,
	uint32_t id)
{
	unsigned i;

	/* Looks through the slots. */
	for (i = 0; i < X11_LINK_CHANNELS; i++) {
		if (table->channels[i].used && table->channels[i].id == id)
			return &table->channels[i];
	}

	/* Not open. */
	return NULL;
}

/*
 * Opens a channel with its starting credits: on the parent's side the id
 * is the next one (id is ignored and the one given is set in the
 * channel), on the child's the one the OPEN named.  Returns 0 with
 * *channel set, or EMFILE when every slot is taken.
 */
int
x11_link_open(
	struct x11_link_table *table,
	uint32_t id,
	struct x11_link_channel **channel)
{
	unsigned i;
	struct x11_link_channel *slot;

	/* Finds a free slot. */
	slot = NULL;
	for (i = 0; i < X11_LINK_CHANNELS; i++) {
		if (!table->channels[i].used) {
			slot = &table->channels[i];
			break;
		}
	}
	if (slot == NULL)
		return EMFILE;

	/* The parent gives the next id; the child takes the one named. */
	if (table->side == X11_LINK_PARENT) {
		id = table->next_id;
		table->next_id++;
	}

	/* The channel and the credit each way. */
	memset(slot, 0, sizeof(*slot));
	slot->used = 1;
	slot->id = id;
	if (table->side == X11_LINK_PARENT) {
		slot->credit_out = X11_LINK_CREDIT_TO_CHILD;
		slot->credit_in = X11_LINK_CREDIT_TO_PARENT;
	} else {
		slot->credit_out = X11_LINK_CREDIT_TO_PARENT;
		slot->credit_in = X11_LINK_CREDIT_TO_CHILD;
	}

	/* Succeeded: the channel. */
	*channel = slot;
	return 0;
}

/*
 * Closes a channel: its slot is free, and its id is never used again on
 * the link.
 */
void
x11_link_close(
	struct x11_link_table *table,
	uint32_t id)
{
	struct x11_link_channel *channel;

	/* Frees the slot. */
	channel = x11_link_find(table, id);
	if (channel != NULL)
		memset(channel, 0, sizeof(*channel));
}

/*
 * Checks a frame received from the other side, and keeps the table in
 * step: an OPEN (child's side) opens its channel, DATA takes from the
 * credit given, EOF marks the end of the other side's data, CREDIT adds
 * to what this side may send.  Returns X11_LINK_TAKE, X11_LINK_DROP for
 * DATA or EOF of a channel already closed (counted), or
 * X11_LINK_VIOLATION.
 */
int
x11_link_check(
	struct x11_link_table *table,
	const struct x11_link_frame *frame)
{
	struct x11_link_channel *channel;
	uint32_t given;
	int verdict;

	/* The frames of the link as a whole. */
	switch (frame->type) {
	case X11_LINK_HELLO:
	case X11_LINK_PING:
	case X11_LINK_PONG:
	case X11_LINK_SHUTDOWN:
		verdict = link_check_control(table, frame);
		return verdict;
	case X11_LINK_OPEN:
		verdict = link_check_open(table, frame);
		return verdict;
	case X11_LINK_TAB_OPEN:
	case X11_LINK_TAB_TITLE:
	case X11_LINK_TAB_SIZE:
	case X11_LINK_TAB_PIXELS:
	case X11_LINK_TAB_GONE:
	case X11_LINK_TAB_INPUT:
	case X11_LINK_TAB_CONFIGURE:
	case X11_LINK_TAB_CLOSE:
	case X11_LINK_TAB_REFUSE:
		verdict = link_check_tab(table, frame);
		return verdict;
	case X11_LINK_DATA:
	case X11_LINK_EOF:
	case X11_LINK_CLOSE:
	case X11_LINK_CREDIT:
		break;
	default:
		/* A type no side sends. */
		return X11_LINK_VIOLATION;
	}

	/* An id the parent never gave is a violation. */
	if (frame->id == 0 || frame->id >= table->next_id)
		return X11_LINK_VIOLATION;

	/* A channel closed already: what crossed its close is dropped. */
	channel = x11_link_find(table, frame->id);
	if (channel == NULL) {
		table->dropped++;
		return X11_LINK_DROP;
	}

	/* The frame of an open channel. */
	switch (frame->type) {
	case X11_LINK_DATA:
		/* Data after the sender's EOF, or past the credit it was given, is a violation. */
		if (channel->eof_in)
			return X11_LINK_VIOLATION;
		if (frame->size > channel->credit_in)
			return X11_LINK_VIOLATION;
		channel->credit_in -= frame->size;
		return X11_LINK_TAKE;
	case X11_LINK_EOF:
		/* The sender sends no more. */
		if (frame->size != 0)
			return X11_LINK_VIOLATION;
		channel->eof_in = 1;
		return X11_LINK_TAKE;
	case X11_LINK_CLOSE:
		/* The caller closes the channel. */
		if (frame->size != 0)
			return X11_LINK_VIOLATION;
		return X11_LINK_TAKE;
	default:
		break;
	}

	/* CREDIT: 32 bits, adding to what may be sent, never past the most a channel is given. */
	if (frame->size != LINK_CREDIT_SIZE)
		return X11_LINK_VIOLATION;
	given = x11_link_get32(frame->body);
	if (given > X11_LINK_CREDIT_MAX - channel->credit_out)
		return X11_LINK_VIOLATION;
	channel->credit_out += given;

	/* Succeeded: the credit is added. */
	return X11_LINK_TAKE;
}

/*
 * Gives how many bytes this side may send on a channel now (0 for a
 * channel not open).
 */
uint32_t
x11_link_room(
	const struct x11_link_table *table,
	uint32_t id)
{
	unsigned i;

	/* The channel's credit. */
	for (i = 0; i < X11_LINK_CHANNELS; i++) {
		if (table->channels[i].used && table->channels[i].id == id)
			return table->channels[i].credit_out;
	}

	/* Not open: nothing. */
	return 0;
}

/*
 * Takes bytes sent on a channel from this side's credit.
 */
void
x11_link_sent(
	struct x11_link_table *table,
	uint32_t id,
	uint32_t size)
{
	struct x11_link_channel *channel;

	/* The credit goes down by what was sent. */
	channel = x11_link_find(table, id);
	if (channel == NULL)
		return;
	if (size > channel->credit_out)
		size = channel->credit_out;
	channel->credit_out -= size;
}

/*
 * Adds credit this side gave the other on a channel (its CREDIT frame).
 */
void
x11_link_gave(
	struct x11_link_table *table,
	uint32_t id,
	uint32_t size)
{
	struct x11_link_channel *channel;

	/* The other side may send that much more. */
	channel = x11_link_find(table, id);
	if (channel == NULL)
		return;
	channel->credit_in += size;
}

/*
 * Writes a 32-bit value, little endian.
 */
void
x11_link_put32(
	uint8_t *bytes,
	uint32_t value)
{
	/* The low byte first. */
	bytes[0] = (uint8_t)(value & 0xFFU);
	bytes[1] = (uint8_t)((value >> 8) & 0xFFU);
	bytes[2] = (uint8_t)((value >> 16) & 0xFFU);
	bytes[3] = (uint8_t)((value >> 24) & 0xFFU);
}

/*
 * Reads a 32-bit value, little endian.
 */
uint32_t
x11_link_get32(
	const uint8_t *bytes)
{
	uint32_t value;

	/* The low byte first. */
	value = (uint32_t)bytes[0];
	value |= (uint32_t)bytes[1] << 8;
	value |= (uint32_t)bytes[2] << 16;
	value |= (uint32_t)bytes[3] << 24;
	return value;
}

/*
 * Checks a frame of the link as a whole: HELLO both ways (its body at most
 * LINK_HELLO_MAX bytes), PING and SHUTDOWN from the parent only, PONG from
 * the child only, all with link id 0 and (but HELLO) no body.
 */
static int
link_check_control(
	const struct x11_link_table *table,
	const struct x11_link_frame *frame)
{
	/* The link as a whole is id 0. */
	if (frame->id != 0)
		return X11_LINK_VIOLATION;

	/* HELLO carries the version and the settings. */
	if (frame->type == X11_LINK_HELLO) {
		if (frame->size > LINK_HELLO_MAX)
			return X11_LINK_VIOLATION;
		return X11_LINK_TAKE;
	}

	/* The rest carry nothing. */
	if (frame->size != 0)
		return X11_LINK_VIOLATION;

	/* PING and SHUTDOWN come from the parent, PONG from the child. */
	if (frame->type == X11_LINK_PONG) {
		if (table->side == X11_LINK_PARENT)
			return X11_LINK_TAKE;
		return X11_LINK_VIOLATION;
	}
	if (table->side == X11_LINK_CHILD)
		return X11_LINK_TAKE;

	return X11_LINK_VIOLATION;
}

/*
 * Checks an OPEN: only the parent sends it, with a new id above every one
 * before, a short body, and a free slot (the parent opens no more than
 * X11_LINK_CHANNELS).  The child's table opens the channel.
 */
static int
link_check_open(
	struct x11_link_table *table,
	const struct x11_link_frame *frame)
{
	struct x11_link_channel *channel;
	int error;

	/* Only the parent opens channels. */
	if (table->side != X11_LINK_CHILD)
		return X11_LINK_VIOLATION;

	/* A new id, above every one before, and a short body. */
	if (frame->id == 0 || frame->id < table->next_id)
		return X11_LINK_VIOLATION;
	if (frame->size > LINK_OPEN_MAX)
		return X11_LINK_VIOLATION;

	/* Opens it; no free slot means the parent opened too many. */
	error = x11_link_open(table, frame->id, &channel);
	if (error != 0)
		return X11_LINK_VIOLATION;
	table->next_id = frame->id + 1;

	/* Succeeded: the channel is open. */
	return X11_LINK_TAKE;
}

/*
 * Checks a frame of a tab's X window as a whole: an X window's id, the
 * side that sends it (the child its window's opening, title, size, pixels
 * and end; the parent the input, the size, the closing and the refusal),
 * and a body of the frame's size.  The receiver checks the rest (a window
 * it knows, a part within the window, an input it knows).
 */
static int
link_check_tab(
	const struct x11_link_table *table,
	const struct x11_link_frame *frame)
{
	int from_child;

	/* An X window is never id 0. */
	if (frame->id == 0)
		return X11_LINK_VIOLATION;

	/* Which side sends it. */
	from_child = 0;
	if (frame->type <= X11_LINK_TAB_GONE)
		from_child = 1;
	if (from_child && table->side != X11_LINK_PARENT)
		return X11_LINK_VIOLATION;
	if (!from_child && table->side != X11_LINK_CHILD)
		return X11_LINK_VIOLATION;

	/* The body each one has. */
	switch (frame->type) {
	case X11_LINK_TAB_OPEN:
		if (frame->size < X11_LINK_TAB_OPEN_FIXED)
			return X11_LINK_VIOLATION;
		if (frame->size > X11_LINK_TAB_OPEN_FIXED + X11_LINK_TAB_TITLE_MAX)
			return X11_LINK_VIOLATION;
		break;
	case X11_LINK_TAB_TITLE:
		if (frame->size > X11_LINK_TAB_TITLE_MAX)
			return X11_LINK_VIOLATION;
		break;
	case X11_LINK_TAB_SIZE:
	case X11_LINK_TAB_CONFIGURE:
		if (frame->size != X11_LINK_TAB_SIZE_BODY)
			return X11_LINK_VIOLATION;
		break;
	case X11_LINK_TAB_PIXELS:
		if (frame->size < X11_LINK_TAB_PIXELS_FIXED)
			return X11_LINK_VIOLATION;
		break;
	case X11_LINK_TAB_INPUT:
		if (frame->size < 4U)
			return X11_LINK_VIOLATION;
		if (frame->size > X11_LINK_TAB_TITLE_MAX)
			return X11_LINK_VIOLATION;
		break;
	default:
		/* The end of a window, and the closing and the refusal of a tab, carry nothing. */
		if (frame->size != 0)
			return X11_LINK_VIOLATION;
		break;
	}

	/* Succeeded: the frame is taken. */
	return X11_LINK_TAKE;
}
