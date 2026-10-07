/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The applications' notifications as the compositor keeps them
 * (ws156-p002): notify.h says what the model is.
 */

#include "notify.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static struct kwl_notification *notify_item(struct kwl_notify_model *model, uint32_t id);
static size_t notify_count(const struct kwl_notify_model *model, unsigned where, uint64_t client, int any_client);
static struct kwl_notification *notify_oldest(struct kwl_notify_model *model, unsigned where);
static void notify_to_log(struct kwl_notify_model *model, struct kwl_notification *item, struct kwl_notify_closed *closed, size_t *closed_count);
static void notify_remove(struct kwl_notify_model *model, struct kwl_notification *item, unsigned reason, struct kwl_notify_closed *closed);
static void notify_words(struct kwl_notification *item, const char *app, const char *title, const char *body, unsigned flags);
static int notify_fits(const char *text, size_t most);
static size_t notify_sequence(const unsigned char *text, size_t at);

/*
 * Makes an empty model; numbers start at 1.
 */
void
kwl_notify_model_init(
	struct kwl_notify_model *model)
{
	/* Nothing kept. */
	memset(model, 0, sizeof(*model));
	model->next_id = 1U;
}

/*
 * Frees what the model holds.
 */
void
kwl_notify_model_free(
	struct kwl_notify_model *model)
{
	/* The table, then nothing. */
	free(model->items);
	memset(model, 0, sizeof(*model));
}

/*
 * Posts a notification of a client (0: the compositor's) by one of its
 * objects: a new one waits, or the client's earlier one numbered replaces
 * takes the new words where it is.  *id is its number.  A notification sent
 * on to the log to make room in the queue may push the log's oldest out:
 * the ones that closed so are in closed (room for 1), their count in
 * *closed_count.  Returns 0, EINVAL for words too long, ENOENT for a
 * replaces the client does not have, EBUSY when the client has as many as
 * it may, or ENOMEM.
 */
int
kwl_notify_post(
	struct kwl_notify_model *model,
	uint64_t client,
	uint32_t object,
	uint32_t replaces,
	const char *app,
	const char *title,
	const char *body,
	unsigned flags,
	uint32_t *id,
	struct kwl_notify_closed *closed,
	size_t *closed_count)
{
	struct kwl_notification *item;
	struct kwl_notification *grown;
	size_t capacity;
	size_t held;
	int fits;

	/* Words that fit. */
	*closed_count = 0;
	fits = notify_fits(app, KWL_NOTIFY_APP_MAX) && notify_fits(title, KWL_NOTIFY_TITLE_MAX) && notify_fits(body, KWL_NOTIFY_BODY_MAX);
	if (!fits)
		return EINVAL;

	/* A replacement: the client's own, where it is. */
	if (replaces != 0U) {
		item = notify_item(model, replaces);
		if (item == NULL || item->client != client)
			return ENOENT;
		notify_words(item, app, title, body, flags);
		item->object = object;
		*id = item->id;
		return 0;
	}

	/* A client's own limit (the compositor has none). */
	held = notify_count(model, 0U, client, 0);
	if (client != 0U && held >= KWL_NOTIFY_PER_CLIENT)
		return EBUSY;

	/* Room in the table. */
	if (model->count == model->capacity) {
		capacity = model->capacity * 2U;
		if (capacity == 0U)
			capacity = 16U;
		grown = realloc(model->items, capacity * sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;
		model->items = grown;
		model->capacity = capacity;
	}

	/* A full queue sends its oldest on to the log. */
	held = notify_count(model, KWL_NOTIFY_WAITING, 0U, 1);
	if (held >= KWL_NOTIFY_QUEUE)
		notify_to_log(model, notify_oldest(model, KWL_NOTIFY_WAITING), closed, closed_count);

	/* The new one, waiting after the others. */
	item = &model->items[model->count];
	model->count++;
	memset(item, 0, sizeof(*item));
	item->id = model->next_id;
	model->next_id++;
	if (model->next_id == 0U)
		model->next_id = 1U;
	item->client = client;
	item->object = object;
	item->where = KWL_NOTIFY_WAITING;
	model->serial++;
	item->order = model->serial;
	notify_words(item, app, title, body, flags);

	/* Succeeded: its number. */
	*id = item->id;
	return 0;
}

/*
 * Leaves a client's notifications with nobody to tell (ws177-p005): the
 * client went, or its notify object (object; 0 for every one of the
 * client).  They stay shown and logged, but a click is told to nobody, so
 * they lose KWL_NOTIFY_ACTION, and their close is told to nobody.
 * Returns how many were left so.
 */
size_t
kwl_notify_orphan(
	struct kwl_notify_model *model,
	uint64_t client,
	uint32_t object)
{
	struct kwl_notification *item;
	size_t index;
	size_t count;

	/* The compositor's own are never left. */
	if (client == 0U)
		return 0U;

	/* Each of the client's, of the object when one is named. */
	count = 0U;
	for (index = 0U; index < model->count; index++) {
		item = &model->items[index];
		if (item->client != client)
			continue;
		if (object != 0U && item->object != object)
			continue;

		/* No action and no one to tell. */
		item->flags &= ~KWL_NOTIFY_ACTION;
		item->object = 0U;
		count++;
	}

	/* Succeeded: how many. */
	return count;
}

/*
 * Takes one post from a client's rate (ws177-p005): at most
 * KWL_NOTIFY_RATE_POSTS in a span of KWL_NOTIFY_RATE_MS from its first.
 * Returns 0 when the post may go, or EBUSY.  The compositor's own are not
 * counted.  A client not kept takes the row of the one whose span is
 * oldest.
 */
int
kwl_notify_rate_take(
	struct kwl_notify_model *model,
	uint64_t client,
	uint64_t now_ms)
{
	struct kwl_notify_rate *rate;
	struct kwl_notify_rate *oldest;
	size_t index;

	/* The compositor's own posts have no rate. */
	if (client == 0U)
		return 0;

	/* The client's row, else the oldest row (a free row is oldest of all). */
	rate = NULL;
	oldest = &model->rates[0];
	for (index = 0U; index < KWL_NOTIFY_RATE_CLIENTS; index++) {
		if (model->rates[index].client == client) {
			rate = &model->rates[index];
			break;
		}

		/* A free row is taken first, else the row whose span began first. */
		if (model->rates[index].client == 0U) {
			oldest = &model->rates[index];
		} else if (oldest->client != 0U && model->rates[index].start_ms < oldest->start_ms) {
			oldest = &model->rates[index];
		}
	}

	/* A client new to the rows starts a span. */
	if (rate == NULL) {
		rate = oldest;
		rate->client = client;
		rate->start_ms = now_ms;
		rate->posts = 0U;
	}

	/* A span that is over starts again. */
	if (now_ms - rate->start_ms >= KWL_NOTIFY_RATE_MS) {
		rate->start_ms = now_ms;
		rate->posts = 0U;
	}

	/* Too many in the span. */
	if (rate->posts >= KWL_NOTIFY_RATE_POSTS)
		return EBUSY;

	/* Succeeded: one more in the span. */
	rate->posts++;
	return 0;
}

/*
 * Copies text into room bytes (with the NUL) as whole UTF-8 (ws177-p005):
 * a byte that does not begin a valid sequence becomes U+FFFD, a control
 * character (C0, DEL and C1) a space, but for a line end when lines is
 * set.  The copy stops before a character that does not fit.  Returns its
 * length.
 */
size_t
kwl_notify_clean(
	char *out,
	size_t room,
	const char *text,
	int lines)
{
	static const char replacement[] = "\xef\xbf\xbd";
	const unsigned char *bytes;
	const char *piece;
	size_t length;
	size_t used;
	size_t at;
	size_t step;
	uint32_t code;

	/* Nothing fits in no room. */
	if (room == 0U)
		return 0U;

	/* Each character, valid or not. */
	bytes = (const unsigned char *)text;
	used = 0U;
	at = 0U;
	while (bytes[at] != '\0') {
		/* The length of a valid sequence here, 0 for an invalid byte. */
		step = notify_sequence(bytes, at);

		/* What it becomes: U+FFFD for one byte that is invalid, the sequence as it is otherwise. */
		piece = (const char *)(bytes + at);
		length = step;
		if (step == 0U) {
			piece = replacement;
			length = 3U;
			step = 1U;
		}

		/* A control character (one byte below 0x20 or DEL, or C1's two bytes) is a space, a line end may stay. */
		code = bytes[at];
		if (length == 1U && (code < 0x20U || code == 0x7fU)) {
			piece = " ";
			if (lines && code == '\n')
				piece = "\n";
		}

		/* C1's controls (U+0080 to U+009F, two bytes) are a space too. */
		if (length == 2U && code == 0xc2U && bytes[at + 1U] < 0xa0U) {
			piece = " ";
			length = 1U;
		}

		/* The copy ends before a character that does not fit. */
		if (used + length + 1U > room)
			break;
		memcpy(out + used, piece, length);
		used += length;
		at += step;
	}

	/* Succeeded: ended. */
	out[used] = '\0';
	return used;
}

/*
 * Withdraws a client's notification wherever it is (not logged).  Returns
 * 0 with closed filled, or ENOENT when the client has none of the number.
 */
int
kwl_notify_withdraw(
	struct kwl_notify_model *model,
	uint64_t client,
	uint32_t id,
	struct kwl_notify_closed *closed)
{
	struct kwl_notification *item;

	/* The client's own. */
	item = notify_item(model, id);
	if (item == NULL || item->client != client)
		return ENOENT;

	/* Gone. */
	notify_remove(model, item, KWL_NOTIFY_WITHDRAWN, closed);
	return 0;
}

/*
 * Shows the oldest waiting notification.  Returns 0 with its number, EBUSY
 * while one is shown, or ENOENT when none waits.
 */
int
kwl_notify_show_next(
	struct kwl_notify_model *model,
	uint32_t *id)
{
	const struct kwl_notification *shown;
	struct kwl_notification *item;

	/* One at a time. */
	shown = kwl_notify_shown(model);
	if (shown != NULL)
		return EBUSY;

	/* The oldest waiting. */
	item = notify_oldest(model, KWL_NOTIFY_WAITING);
	if (item == NULL)
		return ENOENT;

	/* Shown. */
	item->where = KWL_NOTIFY_SHOWN;
	*id = item->id;
	return 0;
}

/*
 * Ends the show of the notification shown: it goes to the log (which may
 * push its oldest out, given back in closed, room for 1, and
 * *closed_count).  Returns 0, or ENOENT when none is shown.
 */
int
kwl_notify_hide(
	struct kwl_notify_model *model,
	struct kwl_notify_closed *closed,
	size_t *closed_count)
{
	struct kwl_notification *item;
	size_t index;

	/* The one shown. */
	*closed_count = 0;
	item = NULL;
	for (index = 0; index < model->count; index++) {
		if (model->items[index].where == KWL_NOTIFY_SHOWN)
			item = &model->items[index];
	}

	/* None shown. */
	if (item == NULL)
		return ENOENT;

	/* Into the log. */
	notify_to_log(model, item, closed, closed_count);
	return 0;
}

/*
 * Dismisses a notification (its x): it goes wherever it is and is not
 * logged.  Returns 0 with closed filled, or ENOENT.
 */
int
kwl_notify_dismiss(
	struct kwl_notify_model *model,
	uint32_t id,
	struct kwl_notify_closed *closed)
{
	struct kwl_notification *item;

	/* The notification. */
	item = notify_item(model, id);
	if (item == NULL)
		return ENOENT;

	/* Gone. */
	notify_remove(model, item, KWL_NOTIFY_DISMISSED, closed);
	return 0;
}

/*
 * Clears the log: every logged notification goes, the first capacity of
 * them given back in closed.  Returns how many went.
 */
size_t
kwl_notify_clear(
	struct kwl_notify_model *model,
	struct kwl_notify_closed *closed,
	size_t capacity)
{
	struct kwl_notify_closed one;
	size_t cleared;
	size_t index;

	/* Each logged one, from the end so that the removal keeps the rest in place. */
	cleared = 0;
	index = model->count;
	while (index > 0) {
		index--;
		if (model->items[index].where != KWL_NOTIFY_LOGGED)
			continue;
		notify_remove(model, &model->items[index], KWL_NOTIFY_CLEARED, &one);
		if (cleared < capacity)
			closed[cleared] = one;
		cleared++;
	}

	/* How many went. */
	return cleared;
}

/* Gives the notification shown, or NULL. */
const struct kwl_notification *
kwl_notify_shown(
	const struct kwl_notify_model *model)
{
	size_t index;

	/* The one in the shown place. */
	for (index = 0; index < model->count; index++) {
		if (model->items[index].where == KWL_NOTIFY_SHOWN)
			return &model->items[index];
	}

	/* None. */
	return NULL;
}

/*
 * Lists the log, newest first, into log (at most capacity).  Returns how
 * many the log holds.
 */
size_t
kwl_notify_log(
	const struct kwl_notify_model *model,
	const struct kwl_notification **log,
	size_t capacity)
{
	const struct kwl_notification *item;
	size_t count;
	size_t index;
	size_t at;

	/* Each logged one, put in place by its order (newest first). */
	count = 0;
	for (index = 0; index < model->count; index++) {
		item = &model->items[index];
		if (item->where != KWL_NOTIFY_LOGGED)
			continue;
		at = count;
		if (at > capacity)
			at = capacity;
		while (at > 0 && log[at - 1U]->order < item->order) {
			if (at < capacity)
				log[at] = log[at - 1U];
			at--;
		}

		/* In its place, when it is among the newest capacity. */
		if (at < capacity)
			log[at] = item;
		count++;
	}

	/* How many the log holds. */
	return count;
}

/* Reports how many notifications wait. */
size_t
kwl_notify_waiting(
	const struct kwl_notify_model *model)
{
	/* The waiting ones of every client. */
	return notify_count(model, KWL_NOTIFY_WAITING, 0U, 1);
}

/* Finds a notification by its number; NULL when there is none. */
const struct kwl_notification *
kwl_notify_find(
	const struct kwl_notify_model *model,
	uint32_t id)
{
	size_t index;

	/* By number. */
	for (index = 0; index < model->count; index++) {
		if (model->items[index].id == id)
			return &model->items[index];
	}

	/* None. */
	return NULL;
}

/* Finds a notification by its number, to change; NULL when there is none. */
static struct kwl_notification *
notify_item(
	struct kwl_notify_model *model,
	uint32_t id)
{
	size_t index;

	/* By number (0 is none). */
	if (id == 0U)
		return NULL;
	for (index = 0; index < model->count; index++) {
		if (model->items[index].id == id)
			return &model->items[index];
	}

	/* None. */
	return NULL;
}

/* Counts the notifications in a place (0: any place) of a client (or of any). */
static size_t
notify_count(
	const struct kwl_notify_model *model,
	unsigned where,
	uint64_t client,
	int any_client)
{
	size_t count;
	size_t index;

	/* Each that matches. */
	count = 0;
	for (index = 0; index < model->count; index++) {
		if (where != 0U && model->items[index].where != where)
			continue;
		if (!any_client && model->items[index].client != client)
			continue;
		count++;
	}

	/* How many. */
	return count;
}

/* Finds the oldest notification in a place; NULL when there is none. */
static struct kwl_notification *
notify_oldest(
	struct kwl_notify_model *model,
	unsigned where)
{
	struct kwl_notification *oldest;
	size_t index;

	/* The smallest order of the place. */
	oldest = NULL;
	for (index = 0; index < model->count; index++) {
		if (model->items[index].where != where)
			continue;
		if (oldest == NULL || model->items[index].order < oldest->order)
			oldest = &model->items[index];
	}

	/* Found, or none. */
	return oldest;
}

/* Puts a notification in the log, newest; a log past its size loses its oldest (expired, given back). */
static void
notify_to_log(
	struct kwl_notify_model *model,
	struct kwl_notification *item,
	struct kwl_notify_closed *closed,
	size_t *closed_count)
{
	struct kwl_notification *oldest;
	size_t logged;
	uint32_t id;

	/* Logged, newest. */
	id = item->id;
	item->where = KWL_NOTIFY_LOGGED;
	model->serial++;
	item->order = model->serial;

	/* A log past its size: the oldest other than this one goes. */
	logged = notify_count(model, KWL_NOTIFY_LOGGED, 0U, 1);
	if (logged <= KWL_NOTIFY_LOG)
		return;
	oldest = notify_oldest(model, KWL_NOTIFY_LOGGED);
	if (oldest == NULL || oldest->id == id)
		return;
	notify_remove(model, oldest, KWL_NOTIFY_EXPIRED, closed);
	*closed_count = 1;
}

/* Takes a notification out of the table, saying who it was and why it went. */
static void
notify_remove(
	struct kwl_notify_model *model,
	struct kwl_notification *item,
	unsigned reason,
	struct kwl_notify_closed *closed)
{
	size_t index;

	/* Who, and why. */
	closed->id = item->id;
	closed->client = item->client;
	closed->object = item->object;
	closed->reason = reason;

	/* The last in its place (the order is kept in each item, not by place in the table). */
	index = (size_t)(item - model->items);
	model->count--;
	if (index != model->count)
		model->items[index] = model->items[model->count];
}

/* Copies a notification's words and flags (NULL as empty). */
static void
notify_words(
	struct kwl_notification *item,
	const char *app,
	const char *title,
	const char *body,
	unsigned flags)
{
	/*
	 * Each, made whole UTF-8 with no control character (ws177-p005): an
	 * invalid byte becomes U+FFFD, a control character a space; the body
	 * keeps its line ends.
	 */
	item->app[0] = '\0';
	item->title[0] = '\0';
	item->body[0] = '\0';
	if (app != NULL)
		(void)kwl_notify_clean(item->app, sizeof(item->app), app, 0);
	if (title != NULL)
		(void)kwl_notify_clean(item->title, sizeof(item->title), title, 0);
	if (body != NULL)
		(void)kwl_notify_clean(item->body, sizeof(item->body), body, 1);
	item->flags = flags & (KWL_NOTIFY_URGENT | KWL_NOTIFY_ACTION);
}

/* Tells whether words (NULL as empty) are at most so many bytes. */
static int
notify_fits(
	const char *text,
	size_t most)
{
	/* Empty fits. */
	if (text == NULL)
		return 1;
	return strlen(text) <= most;
}

/*
 * Gives the length of the valid UTF-8 sequence at a byte of a text (1 to
 * 4), or 0 when the byte begins none: a stray continuation, an overlong
 * form, a surrogate, past U+10FFFF, or one cut short.
 */
static size_t
notify_sequence(
	const unsigned char *text,
	size_t at)
{
	unsigned char lead;
	unsigned char low;
	unsigned char high;
	size_t length;
	size_t index;

	/* ASCII. */
	lead = text[at];
	if (lead < 0x80U)
		return 1U;

	/* The lead byte names the length and the range of the second byte. */
	low = 0x80U;
	high = 0xbfU;
	if (lead >= 0xc2U && lead <= 0xdfU) {
		length = 2U;
	} else if (lead == 0xe0U) {
		length = 3U;
		low = 0xa0U;
	} else if (lead >= 0xe1U && lead <= 0xecU) {
		length = 3U;
	} else if (lead == 0xedU) {
		length = 3U;
		high = 0x9fU;
	} else if (lead >= 0xeeU && lead <= 0xefU) {
		length = 3U;
	} else if (lead == 0xf0U) {
		length = 4U;
		low = 0x90U;
	} else if (lead >= 0xf1U && lead <= 0xf3U) {
		length = 4U;
	} else if (lead == 0xf4U) {
		length = 4U;
		high = 0x8fU;
	} else {
		/* A continuation, C0, C1 or past F4 begins nothing. */
		return 0U;
	}

	/* The second byte in its range. */
	if (text[at + 1U] < low || text[at + 1U] > high)
		return 0U;

	/* The rest are continuations (a NUL ends the text first). */
	for (index = 2U; index < length; index++) {
		if (text[at + index] < 0x80U || text[at + index] > 0xbfU)
			return 0U;
	}

	/* Succeeded: the sequence's length. */
	return length;
}
