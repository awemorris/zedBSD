/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Drag and drop with other windows through the compositor (ws035-p084): the
 * compositor's side of ui-drag.c, through libkeiland's window (WS131
 * p020).
 *
 * Items dragged out of the window offer their file names as
 * "text/uri-list" (file:// URIs, one per line) and as text (a path a line)
 * with the move and copy actions, and the compositor carries the drag (answering
 * the press that started it); its end (dropped or not) comes among the
 * window's inputs (window.c).  A drag of file names coming over the window
 * comes as the drop events: the interface finds its target and the main
 * loop answers the compositor (the names taken or not, move preferred); at the
 * drop the names are read (the window's own drag's directly) and the drop
 * is finished with the action carried out, or given up.
 */

#include "window.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The type that carries file names, and the text of the paths (a line each) for applications that take text. */
#define DND_URI_LIST		"text/uri-list"
#define DND_TEXT		"text/plain;charset=utf-8"

static int dnd_uris(char *const *paths, size_t count, char **text, size_t *length);
static int dnd_parse(const char *text, size_t length, char ***paths, size_t *count);
static int dnd_lines(char *const *paths, size_t count, char **text, size_t *length);
static int dnd_hex(int character);

/*
 * Starts a drag and drop of paths from the window, answering the press
 * that started the drag within it.  Returns 0, or an errno value (the
 * interface then ends its drag).
 */
int
fm_dnd_start(
	struct fm_window *window,
	char *const *paths,
	size_t count)
{
	struct kl_drag_data data[2];
	char *uris;
	char *text;
	size_t uris_length;
	size_t text_length;
	int error;

	/* With a drag already, there is none. */
	if (window->dragging)
		return ENOTSUP;

	/* The names it offers, as URIs and as text. */
	error = dnd_uris(paths, count, &uris, &uris_length);
	if (error != 0)
		return error;
	error = dnd_lines(paths, count, &text, &text_length);
	if (error != 0) {
		free(uris);
		return error;
	}

	/* The drag, which moves, copies or asks (the window keeps copies of the names). */
	data[0].type = DND_URI_LIST;
	data[0].data = uris;
	data[0].length = uris_length;
	data[1].type = DND_TEXT;
	data[1].data = text;
	data[1].length = text_length;
	error = kl_window_start_drag(window->kui, data, 2U, KL_DND_COPY | KL_DND_MOVE | KL_DND_ASK, window->button_serial);
	free(uris);
	free(text);
	if (error != 0)
		return error;

	/* Succeeded: the compositor carries it from the window's surface, with its own badge. */
	window->dragging = 1;
	fm_log("DND start items=%lu bytes=%lu serial=%u", (unsigned long)count, (unsigned long)uris_length, window->button_serial);
	return 0;
}

/*
 * Answers the compositor for the drag over the window: its file names are taken
 * (with move preferred, or copy) or not.
 */
void
fm_dnd_answer(
	struct fm_window *window,
	int accept,
	uint32_t preferred)
{
	/* Move, copy or ask with the one preferred, or nothing. */
	if (accept != 0) {
		kl_window_answer_drop(window->kui, KL_DND_COPY | KL_DND_MOVE | KL_DND_ASK, preferred);
	} else {
		kl_window_answer_drop(window->kui, 0U, 0U);
	}
}

/*
 * Reads the file names of the drag dropped on the window.  Returns 0 with
 * the paths (fm_paths_free frees them), or an errno value.
 */
int
fm_dnd_receive(
	struct fm_window *window,
	char ***paths,
	size_t *count)
{
	unsigned type;
	size_t length;
	char *text;
	int error;

	/* Nothing yet. */
	*paths = NULL;
	*count = 0;

	/* The names, as the source wrote them. */
	error = kl_window_receive_drop(window->kui, &text, &length, &type);
	if (error != 0)
		return error;
	if (type != KL_DROP_URIS) {
		free(text);
		return ENOENT;
	}

	/* The names from the text. */
	error = 0;
	if (text != NULL)
		error = dnd_parse(text, length, paths, count);
	free(text);
	if (error != 0)
		return error;

	/* Succeeded: the paths dropped. */
	fm_log("DND receive bytes=%lu items=%lu", (unsigned long)length, (unsigned long)*count);
	return 0;
}

/*
 * Tells the compositor that the drop is done with an action (the one chosen
 * after "ask" is said first), and lets its offer go.
 */
void
fm_dnd_finish(
	struct fm_window *window,
	uint32_t action)
{
	/* Finished with the action. */
	kl_window_finish_drop(window->kui, action);
}

/*
 * Gives up a dropped drag (its "ask" was cancelled): the offer goes without
 * its finish, and the compositor cancels the drag's source.
 */
void
fm_dnd_abort(
	struct fm_window *window)
{
	/* Given up. */
	kl_window_finish_drop(window->kui, 0U);
}

/*
 * Writes paths as a "text/uri-list": a file:// URI a line (CRLF), with the
 * bytes outside the unreserved set and "/" written as %XX.  Returns 0 with
 * the text (the caller frees it), or ENOMEM.
 */
static int
dnd_uris(
	char *const *paths,
	size_t count,
	char **text,
	size_t *length)
{
	static const char hex[] = "0123456789ABCDEF";
	const unsigned char *byte;
	size_t capacity;
	size_t used;
	size_t index;
	char *out;
	int plain;

	/* Room for the worst case: every byte as %XX, the scheme and the line's end. */
	capacity = 1;
	for (index = 0; index < count; index++)
		capacity += strlen(paths[index]) * 3U + 16U;
	out = malloc(capacity);
	if (out == NULL)
		return ENOMEM;

	/* Each path. */
	used = 0;
	for (index = 0; index < count; index++) {
		/* The scheme. */
		memcpy(out + used, "file://", 7U);
		used += 7U;

		/* Each byte, plain or written as %XX. */
		for (byte = (const unsigned char *)paths[index]; *byte != '\0'; byte++) {
			plain = 0;
			if ((*byte >= 'a' && *byte <= 'z') || (*byte >= 'A' && *byte <= 'Z') || (*byte >= '0' && *byte <= '9'))
				plain = 1;
			if (*byte == '/' || *byte == '-' || *byte == '_' || *byte == '.' || *byte == '~')
				plain = 1;
			if (plain) {
				out[used++] = (char)*byte;
			} else {
				out[used++] = '%';
				out[used++] = hex[*byte >> 4];
				out[used++] = hex[*byte & 15U];
			}
		}

		/* The line's end. */
		out[used++] = '\r';
		out[used++] = '\n';
	}

	/* Succeeded: the text. */
	out[used] = '\0';
	*text = out;
	*length = used;
	return 0;
}

/*
 * Reads a "text/uri-list" into paths: each file:// line (comments and other
 * schemes left out), its %XX bytes decoded.  Returns 0 with the paths, or
 * ENOMEM.
 */
static int
dnd_parse(
	const char *text,
	size_t length,
	char ***paths,
	size_t *count)
{
	const char *line;
	const char *end;
	const char *from;
	char **grown;
	char *path;
	size_t used;
	int scheme;
	int high;
	int low;

	/* Each line. */
	line = text;
	while (line < text + length) {
		/* The line's end (LF, with or without CR). */
		end = line;
		while (end < text + length && *end != '\n')
			end++;

		/* Only a file URI with a path ("file://" and an optional host, then "/"). */
		from = NULL;
		scheme = 1;
		if ((size_t)(end - line) > 7U)
			scheme = strncmp(line, "file://", 7U);
		if (scheme == 0) {
			from = line + 7;
			while (from < end && *from != '/')
				from++;
		}

		/* The path, decoded. */
		if (from != NULL && from < end) {
			path = malloc((size_t)(end - from) + 1U);
			if (path == NULL)
				return ENOMEM;
			used = 0;
			while (from < end && *from != '\r') {
				/* A %XX byte's two digits, when there are. */
				high = -1;
				low = -1;
				if (*from == '%' && from + 2 < end) {
					high = dnd_hex(from[1]);
					low = dnd_hex(from[2]);
				}

				/* The byte it stands for, or the character as it is. */
				if (high >= 0 && low >= 0) {
					path[used++] = (char)(high * 16 + low);
					from += 3;
				} else {
					path[used++] = *from;
					from++;
				}
			}

			/* The end of the path. */
			path[used] = '\0';

			/* One more path. */
			grown = realloc(*paths, (*count + 1U) * sizeof(*grown));
			if (grown == NULL) {
				free(path);
				return ENOMEM;
			}

			/* The path joins the others. */
			*paths = grown;
			(*paths)[*count] = path;
			(*count)++;
		}

		/* The next line. */
		line = end + 1;
	}

	/* Succeeded: the paths. */
	return 0;
}

/*
 * Writes paths as text: each on a line of its own.  Returns 0 with the
 * text (the caller frees it), or ENOMEM.
 */
static int
dnd_lines(
	char *const *paths,
	size_t count,
	char **text,
	size_t *length)
{
	size_t capacity;
	size_t used;
	size_t size;
	size_t index;
	char *out;

	/* Room for every path and its line break. */
	capacity = 1;
	for (index = 0; index < count; index++)
		capacity += strlen(paths[index]) + 1U;
	out = malloc(capacity);
	if (out == NULL)
		return ENOMEM;

	/* Each path and its line break. */
	used = 0;
	for (index = 0; index < count; index++) {
		size = strlen(paths[index]);
		memcpy(out + used, paths[index], size);
		used += size;
		out[used++] = '\n';
	}

	/* Succeeded: the text. */
	out[used] = '\0';
	*text = out;
	*length = used;
	return 0;
}

/* Returns a hexadecimal digit's value, or -1. */
static int
dnd_hex(
	int character)
{
	/* The three ranges. */
	if (character >= '0' && character <= '9')
		return character - '0';
	if (character >= 'a' && character <= 'f')
		return character - 'a' + 10;
	if (character >= 'A' && character <= 'F')
		return character - 'A' + 10;

	/* Not a digit. */
	return -1;
}
