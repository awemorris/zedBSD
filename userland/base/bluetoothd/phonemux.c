/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The profiles' share of the phone link (ws197-p005, see phonemux.h).
 */

#include "userland/base/bluetoothd/phonemux.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* The longest line the mux logs, with its NUL. */
#define PHONEMUX_LOG_MAX		128U

static void phonemux_ready(void *context);
static void phonemux_ended(void *context);
static void phonemux_sdp_done(void *context, const struct btd_sdp *sdp, int error);
static int phonemux_accept(void *context, unsigned server_channel);
static void phonemux_opened(void *context, unsigned dlci, unsigned server_channel, int ours);
static void phonemux_data(void *context, unsigned dlci, const uint8_t *data, size_t length);
static void phonemux_writable(void *context, unsigned dlci);
static void phonemux_closed(void *context, unsigned dlci, int reason);
static void phonemux_open_failed(void *context, unsigned server_channel);
static struct btd_phonemux_row *phonemux_waiting(struct btd_phonemux *mux, unsigned channel, int ours);
static struct btd_phonemux_row *phonemux_open_row(struct btd_phonemux *mux, unsigned dlci);
static struct btd_phonemux_row *phonemux_free_row(struct btd_phonemux *mux);
static int phonemux_channel_busy(const struct btd_phonemux *mux, unsigned channel);
static void phonemux_log(struct btd_phonemux *mux, const char *format, ...) __attribute__((format(printf, 2, 3)));

/*
 * Empties the mux and keeps its hooks.
 */
void
btd_phonemux_init(
	struct btd_phonemux *mux,
	const struct btd_phonemux_hooks *hooks)
{
	/* No child, no query, no DLC. */
	memset(mux, 0, sizeof(*mux));
	mux->hooks = *hooks;
	mux->sdp_child = BTD_PHONEMUX_NONE;
}

/*
 * Adds a child profile, told of the link after the ones added before it.
 * Returns 0 with its index (for btd_phonemux_sdp_query and
 * btd_phonemux_dlc_open), or ENOSPC past BTD_PHONEMUX_CHILDREN.
 */
int
btd_phonemux_add(
	struct btd_phonemux *mux,
	const struct btd_phone_profile *child,
	unsigned *index)
{
	/* No room for another. */
	if (mux->child_count >= BTD_PHONEMUX_CHILDREN)
		return ENOSPC;

	/* Succeeded: kept at the next index. */
	mux->children[mux->child_count] = *child;
	*index = mux->child_count;
	mux->child_count++;
	return 0;
}

/*
 * Fills the profile the phone link is given (btd_phone_set_profile): the
 * mux's own hooks, with the mux as their context.
 */
void
btd_phonemux_profile(
	struct btd_phonemux *mux,
	struct btd_phone_profile *profile)
{
	/* Every hook the mux's. */
	memset(profile, 0, sizeof(*profile));
	profile->context = mux;
	profile->ready = phonemux_ready;
	profile->ended = phonemux_ended;
	profile->sdp_done = phonemux_sdp_done;
	profile->accept = phonemux_accept;
	profile->opened = phonemux_opened;
	profile->data = phonemux_data;
	profile->writable = phonemux_writable;
	profile->closed = phonemux_closed;
	profile->open_failed = phonemux_open_failed;
}

/*
 * Starts a child's SDP query of the phone's records for a class: its
 * answer goes to that child alone.  Returns 0, EBUSY while a query runs
 * (the child asks again shortly), EINVAL for a child not added, or the
 * phone link's error.
 */
int
btd_phonemux_sdp_query(
	struct btd_phonemux *mux,
	unsigned child,
	uint16_t uuid)
{
	int error;

	/* A child of the mux. */
	if (child >= mux->child_count)
		return EINVAL;

	/* One query at a time. */
	if (mux->sdp_child != BTD_PHONEMUX_NONE)
		return EBUSY;

	/* The phone link's query. */
	error = mux->hooks.sdp_query(mux->hooks.context, uuid);
	if (error != 0)
		return error;

	/* Succeeded: its answer goes to the child. */
	mux->sdp_child = child;
	return 0;
}

/*
 * Asks for a child's DLC to a server channel of the phone: followed from
 * now, told to the child as opened, or as closed (or open_failed) when
 * it does not open.  Returns 0; EBUSY while an earlier DLC this side
 * asked for on that channel is still followed (it is waiting to open, or
 * open or closing; the child asks again shortly), also when the phone
 * link says the DLC exists (EEXIST); ENOSPC with every row in use;
 * EINVAL for a child not added; or the phone link's other error.
 */
int
btd_phonemux_dlc_open(
	struct btd_phonemux *mux,
	unsigned child,
	unsigned server_channel)
{
	struct btd_phonemux_row *row;
	int busy;
	int error;

	/* A child of the mux. */
	if (child >= mux->child_count)
		return EINVAL;

	/* An earlier DLC of this side on the channel is still followed. */
	busy = phonemux_channel_busy(mux, server_channel);
	if (busy)
		return EBUSY;

	/* A row for it. */
	row = phonemux_free_row(mux);
	if (row == NULL)
		return ENOSPC;

	/*
	 * Followed before the phone link is asked, which may tell the DLC's
	 * failure inside the call.
	 */
	row->used = 1;
	row->channel = server_channel;
	row->ours = 1;
	row->dlci = 0U;
	row->child = child;

	/*
	 * The phone link's DLC.  Refused at once, neither an opening nor a
	 * close follows, so the row goes now (unless the phone link already
	 * told the DLC's failure inside the call and the row went then); an
	 * existing DLC on the channel is a busy channel for the child.
	 */
	error = mux->hooks.dlc_open(mux->hooks.context, server_channel);
	if (error != 0) {
		if (row->used &&
		    row->ours &&
		    row->dlci == 0U &&
		    row->channel == server_channel)
			memset(row, 0, sizeof(*row));
		if (error == EEXIST)
			return EBUSY;
		return error;
	}

	/* Succeeded: asked for, and followed. */
	return 0;
}

/* The link is ready: every child is told, in the order added. */
static void
phonemux_ready(
	void *context)
{
	struct btd_phonemux *mux;
	unsigned index;

	/* Each child. */
	mux = context;
	for (index = 0U; index < mux->child_count; index++) {
		/* Its ready hook, when it has one. */
		if (mux->children[index].ready != NULL)
			mux->children[index].ready(mux->children[index].context);
	}
}

/* The link ended: nothing is followed any more, and every child is told. */
static void
phonemux_ended(
	void *context)
{
	struct btd_phonemux *mux;
	unsigned index;

	/* The query and the DLCs went with the link. */
	mux = context;
	mux->sdp_child = BTD_PHONEMUX_NONE;
	memset(mux->rows, 0, sizeof(mux->rows));

	/* Each child. */
	for (index = 0U; index < mux->child_count; index++) {
		/* Its ended hook, when it has one. */
		if (mux->children[index].ended != NULL)
			mux->children[index].ended(mux->children[index].context);
	}
}

/* The SDP query's answer: to the child that asked (it may ask again inside the call). */
static void
phonemux_sdp_done(
	void *context,
	const struct btd_sdp *sdp,
	int error)
{
	struct btd_phonemux *mux;
	struct btd_phone_profile *child;
	unsigned index;

	/* The child that asked, the query over. */
	mux = context;
	index = mux->sdp_child;
	mux->sdp_child = BTD_PHONEMUX_NONE;
	if (index >= mux->child_count) {
		phonemux_log(mux, "phonemux: SDP answer nobody asked for");
		return;
	}

	/* Succeeded: the child hears it. */
	child = &mux->children[index];
	if (child->sdp_done != NULL)
		child->sdp_done(child->context, sdp, error);
}

/*
 * Whether one of bluetoothd's server channels is offered to the phone:
 * the first child that offers it takes the DLC that follows (its row is
 * written again if the phone asks twice, by its PN and its SABM).
 */
static int
phonemux_accept(
	void *context,
	unsigned server_channel)
{
	struct btd_phonemux *mux;
	struct btd_phonemux_row *row;
	struct btd_phone_profile *child;
	unsigned index;
	int offered;

	/* Each child in turn, until one offers it. */
	mux = context;
	for (index = 0U; index < mux->child_count; index++) {
		/* Not this child's. */
		child = &mux->children[index];
		if (child->accept == NULL)
			continue;
		offered = child->accept(child->context, server_channel);
		if (!offered)
			continue;

		/* The row of the DLC to come: the one already there, or a free one. */
		row = phonemux_waiting(mux, server_channel, 0);
		if (row == NULL)
			row = phonemux_free_row(mux);
		if (row == NULL) {
			phonemux_log(mux, "phonemux: no row for channel %u", server_channel);
			return 0;
		}

		/* Succeeded: offered, the DLC to come the child's. */
		row->used = 1;
		row->channel = server_channel;
		row->ours = 0;
		row->dlci = 0U;
		row->child = index;
		return 1;
	}

	/* No child offers it. */
	return 0;
}

/* A DLC opened: to the child that asked for it or accepted it; one nobody owns is closed. */
static void
phonemux_opened(
	void *context,
	unsigned dlci,
	unsigned server_channel,
	int ours)
{
	struct btd_phonemux *mux;
	struct btd_phonemux_row *row;
	struct btd_phone_profile *child;

	/* The row that waited for it. */
	mux = context;
	row = phonemux_waiting(mux, server_channel, ours);
	if (row == NULL) {
		phonemux_log(mux, "phonemux: DLC %u opened for nobody", dlci);
		if (mux->hooks.dlc_close != NULL)
			mux->hooks.dlc_close(mux->hooks.context, dlci);
		return;
	}

	/* Succeeded: followed by its DLCI, and the child hears it. */
	row->dlci = dlci;
	child = &mux->children[row->child];
	if (child->opened != NULL)
		child->opened(child->context, dlci, server_channel, ours);
}

/* A DLC's data: to its child. */
static void
phonemux_data(
	void *context,
	unsigned dlci,
	const uint8_t *data,
	size_t length)
{
	struct btd_phonemux *mux;
	struct btd_phonemux_row *row;
	struct btd_phone_profile *child;

	/* The DLC's row. */
	mux = context;
	row = phonemux_open_row(mux, dlci);
	if (row == NULL)
		return;

	/* Succeeded: the child hears it. */
	child = &mux->children[row->child];
	if (child->data != NULL)
		child->data(child->context, dlci, data, length);
}

/* A DLC may be written again: to its child. */
static void
phonemux_writable(
	void *context,
	unsigned dlci)
{
	struct btd_phonemux *mux;
	struct btd_phonemux_row *row;
	struct btd_phone_profile *child;

	/* The DLC's row. */
	mux = context;
	row = phonemux_open_row(mux, dlci);
	if (row == NULL)
		return;

	/* Succeeded: the child hears it. */
	child = &mux->children[row->child];
	if (child->writable != NULL)
		child->writable(child->context, dlci);
}

/*
 * A DLC closed: to its child, and no longer followed.  A DLC this side
 * asked for that closed without opening (refused, or given up) is found
 * by its server channel, the DLCI's upper five bits.
 */
static void
phonemux_closed(
	void *context,
	unsigned dlci,
	int reason)
{
	struct btd_phonemux *mux;
	struct btd_phonemux_row *row;
	struct btd_phone_profile *child;
	unsigned index;

	/* The open DLC's row, or the row of this side's DLC that never opened. */
	mux = context;
	row = phonemux_open_row(mux, dlci);
	if (row == NULL)
		row = phonemux_waiting(mux, dlci >> 1, 1);
	if (row == NULL) {
		phonemux_log(mux, "phonemux: DLC %u closed for nobody", dlci);
		return;
	}

	/* No longer followed, then the child hears it (it may ask for another DLC inside the call). */
	index = row->child;
	memset(row, 0, sizeof(*row));
	child = &mux->children[index];
	if (child->closed != NULL)
		child->closed(child->context, dlci, reason);
}

/* A DLC asked for whose RFCOMM session never came up: to the child that asked. */
static void
phonemux_open_failed(
	void *context,
	unsigned server_channel)
{
	struct btd_phonemux *mux;
	struct btd_phonemux_row *row;
	struct btd_phone_profile *child;
	unsigned index;

	/* The row that waited for it. */
	mux = context;
	row = phonemux_waiting(mux, server_channel, 1);
	if (row == NULL)
		return;

	/* No longer followed, then the child hears it. */
	index = row->child;
	memset(row, 0, sizeof(*row));
	child = &mux->children[index];
	if (child->open_failed != NULL)
		child->open_failed(child->context, server_channel);
}

/* Finds the row of a DLC not opened yet, by its server channel and side, or NULL. */
static struct btd_phonemux_row *
phonemux_waiting(
	struct btd_phonemux *mux,
	unsigned channel,
	int ours)
{
	struct btd_phonemux_row *row;
	unsigned index;

	/* Each row in use. */
	for (index = 0U; index < BTD_PHONEMUX_ROWS; index++) {
		/* A free row, or one already open. */
		row = &mux->rows[index];
		if (!row->used || row->dlci != 0U)
			continue;

		/* Its channel and side. */
		if (row->channel == channel && row->ours == ours)
			return row;
	}

	/* None. */
	return NULL;
}

/* Finds the row of an open DLC by its DLCI, or NULL. */
static struct btd_phonemux_row *
phonemux_open_row(
	struct btd_phonemux *mux,
	unsigned dlci)
{
	struct btd_phonemux_row *row;
	unsigned index;

	/* DLCI 0 is the session's, never a row's. */
	if (dlci == 0U)
		return NULL;

	/* Each row in use. */
	for (index = 0U; index < BTD_PHONEMUX_ROWS; index++) {
		/* The DLC's. */
		row = &mux->rows[index];
		if (row->used && row->dlci == dlci)
			return row;
	}

	/* None. */
	return NULL;
}

/* Finds a free row, or NULL. */
static struct btd_phonemux_row *
phonemux_free_row(
	struct btd_phonemux *mux)
{
	unsigned index;

	/* The first free one. */
	for (index = 0U; index < BTD_PHONEMUX_ROWS; index++) {
		/* Free. */
		if (!mux->rows[index].used)
			return &mux->rows[index];
	}

	/* All in use. */
	return NULL;
}

/*
 * Says whether a DLC this side asked for on a server channel is still
 * followed: waiting to open, or open (a DLC's server channel is its
 * DLCI's upper five bits).
 */
static int
phonemux_channel_busy(
	const struct btd_phonemux *mux,
	unsigned channel)
{
	const struct btd_phonemux_row *row;
	unsigned index;

	/* Each row in use of this side. */
	for (index = 0U; index < BTD_PHONEMUX_ROWS; index++) {
		/* A free row, or the phone's DLC. */
		row = &mux->rows[index];
		if (!row->used || !row->ours)
			continue;

		/* Waiting to open on the channel. */
		if (row->dlci == 0U && row->channel == channel)
			return 1;

		/* Open (or closing) on the channel. */
		if (row->dlci != 0U && (row->dlci >> 1) == channel)
			return 1;
	}

	/* The channel is free. */
	return 0;
}

/* Logs a line of the mux's, when the daemon takes them. */
static void
phonemux_log(
	struct btd_phonemux *mux,
	const char *format,
	...)
{
	char line[PHONEMUX_LOG_MAX];
	va_list arguments;

	/* No log. */
	if (mux->hooks.log == NULL)
		return;

	/* The line, written and handed over. */
	va_start(arguments, format);
	(void)vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	mux->hooks.log(mux->hooks.context, line);
}
