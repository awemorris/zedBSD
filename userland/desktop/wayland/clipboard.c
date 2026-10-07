/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The clipboard's history (ws102-p018, plan/ws102/design.md section 2.10):
 * the text of the last KWL_CLIPBOARD_HISTORY selections, newest first, in
 * memory only.
 *
 * When a client sets a selection with a text type, the compositor reads the text
 * from it as any client would (wl_data_source.send into a pipe, read
 * without waiting in each pass of the event loop) and puts it first in the
 * history; the same text already there moves up instead.  Nothing is kept
 * of a selection made while the focused field is secret (the source marked
 * with the x-kde-passwordManagerHint type, the convention password
 * managers use), of an empty one, or of one larger than
 * KWL_CLIPBOARD_TEXT_MAX.  The lock screen and Log Out empty the history
 * (the texts are wiped before they are freed).
 *
 * kwl_clipboard_history_paste makes an item the selection -- the compositor's
 * own, offered as text (data.c) -- and sends the focused window the paste
 * operation (edit.c).  The on-screen keyboard's history tab (ws102-p016)
 * lists the items with kwl_clipboard_history_count and _get; until it
 * exists, Super+Alt+H logs the list and Super+Alt+1 ... 0 pastes an item
 * (edit.c).  The log names an item by its length and a checksum, never
 * its text.
 */

#include "data.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* How long a source may take to write its text before the read is given up, in milliseconds. */
#define CLIPBOARD_READ_MS	2000U

/* The text types read, in the order they are preferred. */
static const char *const clipboard_types[] = {
	"text/plain;charset=utf-8",
	"text/plain",
	"UTF8_STRING",
	"TEXT",
	"STRING",
};

/* The type a source marks secret text with (password managers' convention). */
#define CLIPBOARD_SECRET_TYPE	"x-kde-passwordManagerHint"

/* One item of the history: its text (not NUL-terminated) and its length. */
struct clipboard_item {
	char *text;
	size_t length;
};

/*
 * The history, newest first, and how many items it has; the event loop
 * alone touches it.
 */
static struct clipboard_item clipboard_history[KWL_CLIPBOARD_HISTORY];
static unsigned clipboard_count;

/*
 * The read under way: the pipe's end the compositor reads (-1 for none), the
 * text so far, its length, and when the read began.
 */
static struct {
	int fd;
	char *text;
	size_t length;
	uint64_t started;
} clipboard_read = { -1, NULL, 0U, 0U };

/*
 * The text the compositor offers while one of its history's items is the
 * selection (a copy: the history may change meanwhile), and its length.
 */
static char *clipboard_offered;
static size_t clipboard_offered_length;

static void clipboard_read_end(int keep);
static void clipboard_add(char *text, size_t length);
static void clipboard_wipe(char *text, size_t length);
static uint32_t clipboard_sum(const char *text, size_t length);

/*
 * Hears a new selection a client set (data.c): its text is read into the
 * history, unless it is secret or has no text type.  NULL (an empty
 * clipboard) changes nothing.
 */
void
kwl_clipboard_selected(
	struct kwl_server *server,
	struct kwl_object *source)
{
	const char *type;
	unsigned index;
	unsigned kind;
	int pipes[2];
	int error;
	int same;

	/* A read still under way is given up: the new selection replaces it. */
	(void)server;
	if (clipboard_read.fd >= 0)
		clipboard_read_end(0);

	/* An empty clipboard, or a source that went. */
	if (source == NULL || source->dead || source->client->fatal)
		return;

	/* A secret selection is not kept. */
	for (index = 0; index < source->mime_count; index++) {
		same = strcmp(source->mime_types[index], CLIPBOARD_SECRET_TYPE);
		if (same == 0) {
			printf("KWL CLIP skip reason=secret client=%llu\n", (unsigned long long)source->client->number);
			return;
		}
	}

	/* The text type the source has, the most preferred. */
	type = NULL;
	for (kind = 0; kind < sizeof(clipboard_types) / sizeof(clipboard_types[0]) && type == NULL; kind++) {
		for (index = 0; index < source->mime_count; index++) {
			same = strcmp(source->mime_types[index], clipboard_types[kind]);
			if (same == 0) {
				type = clipboard_types[kind];
				break;
			}
		}
	}
	if (type == NULL) {
		printf("KWL CLIP skip reason=no-text client=%llu\n", (unsigned long long)source->client->number);
		return;
	}

	/* The pipe: the source writes into one end, the event loop reads the other without waiting. */
	error = pipe(pipes);
	if (error != 0)
		return;
	(void)fcntl(pipes[0], F_SETFD, FD_CLOEXEC);
	(void)fcntl(pipes[0], F_SETFL, fcntl(pipes[0], F_GETFL) | O_NONBLOCK);
	(void)fcntl(pipes[1], F_SETFD, FD_CLOEXEC);

	/* The source is asked for the text (the event takes the writing end with it). */
	clipboard_read.fd = pipes[0];
	clipboard_read.text = NULL;
	clipboard_read.length = 0U;
	clipboard_read.started = kwl_milliseconds();
	error = kwl_data_send(source, type, pipes[1]);
	if (error != 0) {
		clipboard_read_end(0);
		return;
	}

	/* Succeeded: the text comes through the pipe. */
	printf("KWL CLIP read client=%llu mime=%s\n", (unsigned long long)source->client->number, type);
}

/*
 * Reads what the source wrote so far (the pipe was readable); at the end
 * of the text it joins the history.  A read that takes too long is given
 * up (called each pass of the event loop).
 */
void
kwl_clipboard_poll(
	struct kwl_server *server)
{
	char chunk[1024];
	char *grown;
	ssize_t got;

	/* No read under way. */
	(void)server;
	if (clipboard_read.fd < 0)
		return;

	/* Everything the pipe has now. */
	for (;;) {
		got = read(clipboard_read.fd, chunk, sizeof(chunk));

		/* Nothing more for now: another pass, unless it has taken too long. */
		if (got < 0 && (errno == EAGAIN || errno == EINTR)) {
			if (kwl_milliseconds() - clipboard_read.started > CLIPBOARD_READ_MS) {
				printf("KWL CLIP skip reason=timeout\n");
				clipboard_read_end(0);
			}
			return;
		}

		/* A failed read gives up. */
		if (got < 0) {
			clipboard_read_end(0);
			return;
		}

		/* The end: the text joins the history. */
		if (got == 0) {
			clipboard_read_end(1);
			return;
		}

		/* Too much text is not kept. */
		if (clipboard_read.length + (size_t)got > KWL_CLIPBOARD_TEXT_MAX) {
			printf("KWL CLIP skip reason=size\n");
			clipboard_read_end(0);
			return;
		}

		/* The chunk is added. */
		grown = realloc(clipboard_read.text, clipboard_read.length + (size_t)got);
		if (grown == NULL) {
			clipboard_read_end(0);
			return;
		}
		clipboard_read.text = grown;
		memcpy(clipboard_read.text + clipboard_read.length, chunk, (size_t)got);
		clipboard_read.length += (size_t)got;
	}
}

/*
 * Reports how many items the history has.
 */
unsigned
kwl_clipboard_history_count(
	struct kwl_server *server)
{
	/* Succeeded: the count. */
	(void)server;
	return clipboard_count;
}

/*
 * Gives an item of the history (0: the newest): its text, not
 * NUL-terminated, and its length; NULL past the last.  The text stays the
 * history's (valid until the history changes).
 */
const char *
kwl_clipboard_history_get(
	struct kwl_server *server,
	unsigned index,
	size_t *length)
{
	/* Past the last. */
	(void)server;
	*length = 0U;
	if (index >= clipboard_count)
		return NULL;

	/* Succeeded: the item. */
	*length = clipboard_history[index].length;
	return clipboard_history[index].text;
}

/*
 * Pastes an item of the history into the focused window: it becomes the
 * selection (the compositor's own, first in the history again) and the window
 * is sent the paste operation.  Returns 0, ENOENT for no such item,
 * ENOMEM, or the paste's error (edit.c).
 */
int
kwl_clipboard_history_paste(
	struct kwl_server *server,
	unsigned index)
{
	struct clipboard_item item;
	char *copy;
	unsigned slot;
	int error;

	/* No such item. */
	if (index >= clipboard_count)
		return ENOENT;

	/* The text the compositor will offer, a copy of its own. */
	item = clipboard_history[index];
	copy = malloc(item.length + 1U);
	if (copy == NULL)
		return ENOMEM;
	memcpy(copy, item.text, item.length);
	if (clipboard_offered != NULL)
		clipboard_wipe(clipboard_offered, clipboard_offered_length);
	clipboard_offered = copy;
	clipboard_offered_length = item.length;

	/* The item comes first. */
	for (slot = index; slot > 0; slot--)
		clipboard_history[slot] = clipboard_history[slot - 1U];
	clipboard_history[0] = item;

	/* It is the selection, and the focused window pastes it. */
	kwl_data_select_offered(server);
	printf("KWL CLIP paste index=%u length=%zu sum=%08x\n", index, item.length, clipboard_sum(item.text, item.length));
	error = kwl_edit_action(server, KWL_EDIT_PASTE);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Writes the text the compositor offers into a descriptor a client reads (data.c,
 * a receive of the compositor's selection), and closes it.  A reader that does
 * not take it all within a moment gets what fitted.
 */
void
kwl_clipboard_offer_write(
	int descriptor)
{
	uint64_t started;
	size_t written;
	ssize_t count;

	/* The descriptor must not hold the event loop long. */
	(void)fcntl(descriptor, F_SETFL, fcntl(descriptor, F_GETFL) | O_NONBLOCK);
	written = 0U;
	started = kwl_milliseconds();

	/* All of the text, in as many writes as it takes (a full pipe is tried again for a moment). */
	while (clipboard_offered != NULL && written < clipboard_offered_length) {
		count = write(descriptor, clipboard_offered + written, clipboard_offered_length - written);
		if (count > 0) {
			written += (size_t)count;
			continue;
		}
		if (count < 0 && errno != EAGAIN && errno != EINTR)
			break;
		if (kwl_milliseconds() - started > 100U)
			break;
	}

	/* Succeeded: the reader sees the end. */
	close(descriptor);
	printf("KWL CLIP offer bytes=%zu\n", written);
}

/*
 * Empties the history, wiping each text (the lock screen, Log Out).
 */
void
kwl_clipboard_history_clear(
	struct kwl_server *server,
	const char *reason)
{
	unsigned index;
	unsigned count;

	/* A read under way is given up too. */
	(void)server;
	if (clipboard_read.fd >= 0)
		clipboard_read_end(0);

	/* Each item. */
	count = clipboard_count;
	for (index = 0; index < clipboard_count; index++) {
		clipboard_wipe(clipboard_history[index].text, clipboard_history[index].length);
		clipboard_history[index].text = NULL;
		clipboard_history[index].length = 0U;
	}
	clipboard_count = 0U;

	/* The text the compositor offers, too. */
	if (clipboard_offered != NULL) {
		clipboard_wipe(clipboard_offered, clipboard_offered_length);
		clipboard_offered = NULL;
		clipboard_offered_length = 0U;
	}

	/* Succeeded: the log line the tests read. */
	printf("KWL CLIP clear reason=%s count=%u\n", reason, count);
}

/*
 * Logs the history, newest first, each item by its length and checksum
 * (Super+Alt+H, edit.c).
 */
void
kwl_clipboard_history_log(
	struct kwl_server *server)
{
	unsigned index;

	/* The count, then each item. */
	(void)server;
	printf("KWL CLIP history count=%u", clipboard_count);
	for (index = 0; index < clipboard_count; index++)
		printf(" %u:%zu:%08x", index, clipboard_history[index].length, clipboard_sum(clipboard_history[index].text, clipboard_history[index].length));
	printf("\n");
}

/* Ends the read under way: the text joins the history (keep) or is dropped. */
static void
clipboard_read_end(
	int keep)
{
	/* The pipe goes. */
	close(clipboard_read.fd);
	clipboard_read.fd = -1;

	/* The text: into the history, or wiped (an empty one is not kept). */
	if (keep && clipboard_read.length > 0U) {
		clipboard_add(clipboard_read.text, clipboard_read.length);
	} else if (clipboard_read.text != NULL) {
		clipboard_wipe(clipboard_read.text, clipboard_read.length);
	}
	clipboard_read.text = NULL;
	clipboard_read.length = 0U;
}

/* Puts a text first in the history (taking it); the same text already there moves up, the oldest item goes when it is full. */
static void
clipboard_add(
	char *text,
	size_t length)
{
	unsigned index;
	unsigned last;
	int same;

	/* The same text already there leaves its place (it comes first below). */
	last = clipboard_count;
	for (index = 0; index < clipboard_count; index++) {
		if (clipboard_history[index].length != length)
			continue;
		same = memcmp(clipboard_history[index].text, text, length);
		if (same == 0) {
			clipboard_wipe(clipboard_history[index].text, length);
			last = index;
			clipboard_count--;
			break;
		}
	}

	/* A full history lets its oldest item go. */
	if (last == clipboard_count && clipboard_count == KWL_CLIPBOARD_HISTORY) {
		clipboard_wipe(clipboard_history[KWL_CLIPBOARD_HISTORY - 1U].text, clipboard_history[KWL_CLIPBOARD_HISTORY - 1U].length);
		clipboard_count--;
		last = clipboard_count;
	}

	/* The items before the gap move down one, and the text comes first. */
	for (index = last; index > 0; index--)
		clipboard_history[index] = clipboard_history[index - 1U];
	clipboard_history[0].text = text;
	clipboard_history[0].length = length;
	clipboard_count++;

	/* Succeeded: the log line the tests read. */
	printf("KWL CLIP add length=%zu sum=%08x count=%u\n", length, clipboard_sum(text, length), clipboard_count);
}

/* Overwrites a text before it is freed, so that a copied secret does not stay in freed memory. */
static void
clipboard_wipe(
	char *text,
	size_t length)
{
	volatile char *byte;
	size_t index;

	/* Nothing to wipe. */
	if (text == NULL)
		return;

	/* Each byte, through a volatile pointer (not optimized away), then the memory. */
	byte = text;
	for (index = 0; index < length; index++)
		byte[index] = 0;
	free(text);
}

/* The FNV-1a checksum of a text (the log names items by it, never by their text). */
static uint32_t
clipboard_sum(
	const char *text,
	size_t length)
{
	uint32_t sum;
	size_t index;

	/* Each byte. */
	sum = 2166136261U;
	for (index = 0; index < length; index++) {
		sum ^= (unsigned char)text[index];
		sum *= 16777619U;
	}

	/* Succeeded: the checksum. */
	return sum;
}
