/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Keeps unsent attachments separate from the message store and transport. */
#include "phone.h"
#include "userland/desktop/picture/media-kind.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int draft_kind(const char *path, int *video);
static int draft_uri(const char *uri, size_t length, char *path, size_t size);
static int draft_hex(unsigned char byte);

/*
 * Attaches one readable photo or video path to a contact's unsent draft.
 */
int
ph_draft_add(
	struct ph_view *view,
	const char *path,
	const char *contact,
	int temporary)
{
	struct ph_attachment *attachment;
	size_t index;
	int same;
	int video;
	int error;

	/* A finite draft cannot consume unbounded files or attach to an unknown recipient. */
	if (path == NULL || path[0] != '/' || contact == NULL || contact[0] == '\0')
		return EINVAL;
	error = draft_kind(path, &video);
	if (error != 0)
		return error;

	/* Repeated selection of the same path does not duplicate its attachment. */
	for (index = 0U; index < view->attachment_count; index++) {
		same = strcmp(view->attachments[index].contact, contact);
		if (same != 0)
			continue;
		same = strcmp(view->attachments[index].path, path);
		if (same == 0)
			return 0;
	}

	/* An existing attachment remains idempotent even when all draft slots are occupied. */
	if (view->attachment_count >= PH_DRAFT_MAX)
		return E2BIG;

	/* Owns a copy of the path and stable contact ID, independently of chooser lifetime. */
	attachment = &view->attachments[view->attachment_count];
	attachment->path = strdup(path);
	if (attachment->path == NULL)
		return ENOMEM;
	attachment->contact = strdup(contact);
	if (attachment->contact == NULL) {
		free(attachment->path);
		attachment->path = NULL;
		return ENOMEM;
	}

	/* Only temporary image drops are deleted when the draft is removed. */
	attachment->video = video;
	attachment->temporary = temporary;
	view->attachment_count++;

	/* Succeeded: no message is sent or entered into the timeline. */
	return 0;
}

/*
 * Removes one unsent attachment and only its owned temporary original.
 */
void
ph_draft_remove(
	struct ph_view *view,
	size_t index)
{
	struct ph_attachment *attachment;

	/* Nothing outside the retained draft belongs to this operation. */
	if (index >= view->attachment_count)
		return;
	attachment = &view->attachments[index];
	if (attachment->temporary)
		(void)unlink(attachment->path);
	free(attachment->path);
	free(attachment->contact);

	/* Packs live entries so cancellation immediately makes the slot reusable. */
	view->attachment_count--;
	view->attachment_page = 0U;
	memmove(attachment, attachment + 1, (view->attachment_count - index) * sizeof(*attachment));
	memset(&view->attachments[view->attachment_count], 0, sizeof(*attachment));
}

/*
 * Releases every draft at application close without sending any of them.
 */
void
ph_draft_release(
	struct ph_view *view)
{
	/* Removes the last entry until no temporary file or allocation remains. */
	while (view->attachment_count != 0U)
		ph_draft_remove(view, view->attachment_count - 1U);
}

/*
 * Counts only attachments belonging to the conversation currently displayed.
 */
size_t
ph_draft_count(
	const struct ph_view *view,
	const char *contact)
{
	size_t count;
	size_t index;
	int same;

	/* Stable IDs keep attachments with their original recipient across list changes. */
	count = 0U;
	for (index = 0U; index < view->attachment_count; index++) {
		same = strcmp(view->attachments[index].contact, contact);
		if (same == 0)
			count++;
	}

	/* Succeeded: other conversations' drafts do not affect this composer. */
	return count;
}

/*
 * Appends dropped text to the composer without submitting it.
 */
int
ph_draft_text(
	struct ph_view *view,
	const char *text,
	size_t length)
{
	char *joined;
	size_t limit;
	const void *zero;

	/* Rejects binary or oversized drops without truncating existing text. */
	zero = memchr(text, '\0', length);
	if (zero != NULL)
		return EINVAL;
	limit = sizeof(view->message.text) - 1U;
	if (view->message.limit != 0U && view->message.limit < limit)
		limit = view->message.limit;
	if (view->message.length > limit || length > limit - view->message.length)
		return E2BIG;
	joined = malloc(view->message.length + length + 1U);
	if (joined == NULL)
		return ENOMEM;

	/* Preserves the existing draft and appends the exact UTF-8 bytes. */
	memcpy(joined, view->message.text, view->message.length);
	memcpy(joined + view->message.length, text, length);
	joined[view->message.length + length] = '\0';
	kl_field_set(&view->message, joined);
	free(joined);

	/* Succeeded: only the editable text changed. */
	return 0;
}

/*
 * Attaches local file URIs, decoding spaces and UTF-8 names as ordinary paths.
 */
int
ph_draft_uris(
	struct ph_view *view,
	const char *text,
	size_t length,
	const char *contact)
{
	char path[4096];
	size_t start;
	size_t end;
	unsigned added;
	int error;
	int first;

	/* URI lists contain newline-separated entries and optional comment lines. */
	start = 0U;
	added = 0U;
	first = 0;
	while (start < length) {
		end = start;
		while (end < length && text[end] != '\r' && text[end] != '\n')
			end++;
		if (end != start && text[start] != '#') {
			error = draft_uri(text + start, end - start, path, sizeof(path));
			if (error == 0)
				error = ph_draft_add(view, path, contact, 0);
			if (error == 0)
				added++;
			else if (first == 0)
				first = error;
		}

		/* Skips both halves of CRLF without creating an empty filename. */
		start = end;
		while (start < length && (text[start] == '\r' || text[start] == '\n'))
			start++;
	}

	/* Partial success stays visible, while the caller can explain rejected entries. */
	if (first != 0)
		return first;
	if (added == 0U)
		return EINVAL;

	/* Succeeded: the list contributed only unsent local media attachments. */
	return 0;
}

/* Determines media kind from the file header instead of trusting its suffix. */
static int
draft_kind(
	const char *path,
	int *video)
{
	struct stat status;
	unsigned char head[16];
	ssize_t count;
	int descriptor;
	int error;
	int kind;
	int regular;

	/* Opens an original path without blocking on a FIFO or special device. */
	descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	if (descriptor < 0)
		return errno;
	error = fstat(descriptor, &status);
	if (error != 0) {
		error = errno;
		close(descriptor);
		return error;
	}

	/* Only a regular file can be retained as an original media attachment. */
	regular = S_ISREG(status.st_mode);
	if (!regular) {
		close(descriptor);
		return EINVAL;
	}

	/* Header classification is shared with the media-library import command. */
	count = pread(descriptor, head, sizeof(head), 0);
	close(descriptor);
	if (count < 0)
		return EIO;
	kind = kl_media_kind(head, (size_t)count);
	if (kind == KL_MEDIA_NONE)
		return ENOTSUP;
	*video = 0;
	if (kind == KL_MEDIA_VIDEO)
		*video = 1;

	/* Succeeded: the path describes a supported photo or video container. */
	return 0;
}

/* Decodes one local file URI without interpreting it as a network resource. */
static int
draft_uri(
	const char *uri,
	size_t length,
	char *path,
	size_t size)
{
	size_t at;
	size_t used;
	int same;
	int high;
	int low;
	unsigned char byte;

	/* Only local absolute files can become path-based attachments. */
	if (length < 8U)
		return EINVAL;
	same = memcmp(uri, "file://", 7U);
	if (same != 0)
		return EINVAL;
	at = 7U;
	if (uri[at] != '/') {
		if (length < 17U)
			return EINVAL;
		same = memcmp(uri + at, "localhost/", 10U);
		if (same != 0)
			return EINVAL;
		at += 9U;
	}

	/* Percent escapes are decoded exactly once; NUL and row delimiters are invalid paths. */
	used = 0U;
	while (at < length) {
		byte = (unsigned char)uri[at++];
		if (byte == '%') {
			if (length - at < 2U)
				return EINVAL;
			high = draft_hex((unsigned char)uri[at++]);
			low = draft_hex((unsigned char)uri[at++]);
			if (high < 0 || low < 0)
				return EINVAL;
			byte = (unsigned char)((high << 4) | low);
		}

		/* A decoded path cannot contain metadata row delimiters or NUL. */
		if (byte == 0U || byte == '\n' || byte == '\r' || byte == '\t')
			return EINVAL;
		if (used + 1U >= size)
			return ENAMETOOLONG;
		path[used++] = (char)byte;
	}

	/* Succeeded: the output is a terminated absolute original-file path. */
	path[used] = '\0';
	return 0;
}

/* Converts one ASCII hexadecimal digit, or reports an invalid escape. */
static int
draft_hex(
	unsigned char byte)
{
	/* Decimal digits and either letter case describe the same encoded byte. */
	if (byte >= '0' && byte <= '9')
		return (int)(byte - '0');
	if (byte >= 'a' && byte <= 'f')
		return (int)(byte - 'a') + 10;
	if (byte >= 'A' && byte <= 'F')
		return (int)(byte - 'A') + 10;
	return -1;
}
