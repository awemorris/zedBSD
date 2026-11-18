/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parts of volumed without system calls (ws132-p004): a label made
 * safe for a line and for a folder name, a client's line read, the rule of
 * who may mount and eject, and a volume written as a line.
 */

#include "userland/base/volumed/volumed.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int names_plain(unsigned char character);

/*
 * Writes text with every byte that is not a plain word character as %xx,
 * so that a label stays one word of a line.  Returns 0, or ENAMETOOLONG
 * when it does not fit (the output is then empty).
 */
int
volumed_escape(
	const char *text,
	char *output,
	size_t size)
{
	const unsigned char *byte;
	size_t used;
	int plain;

	/* Somewhere to write, at least the NUL. */
	if (output == NULL || size == 0U)
		return EINVAL;

	/* Each byte as itself or as %xx. */
	used = 0U;
	for (byte = (const unsigned char *)text; *byte != '\0'; byte++) {
		plain = names_plain(*byte);
		if (plain && used + 1U < size) {
			output[used] = (char)*byte;
			used++;
			continue;
		}

		/* An escaped byte takes three. */
		if (!plain && used + 3U < size) {
			(void)snprintf(output + used, 4U, "%%%02x", (unsigned)*byte);
			used += 3U;
			continue;
		}

		/* It does not fit. */
		output[0] = '\0';
		return ENAMETOOLONG;
	}

	/* Succeeded. */
	output[used] = '\0';
	return 0;
}

/*
 * Makes the folder name a volume is mounted as: its label, or its disk's
 * name without one, with '/', control characters and a leading '.' turned
 * into '_' and the surrounding spaces dropped.  Returns 0 or EINVAL.
 */
int
volumed_mount_name(
	const char *label,
	const char *id,
	char *output,
	size_t size)
{
	const char *source;
	size_t length;
	size_t index;
	unsigned char character;

	/* Somewhere to write, and a disk's name to fall back on. */
	if (output == NULL || size < 2U || id == NULL || id[0] == '\0')
		return EINVAL;

	/* The label without its leading spaces, or the disk's name. */
	source = id;
	if (label != NULL) {
		while (*label == ' ')
			label++;
		if (*label != '\0')
			source = label;
	}

	/* Copied, the characters a name cannot hold replaced. */
	length = 0U;
	for (index = 0U; source[index] != '\0' && length + 1U < size; index++) {
		character = (unsigned char)source[index];
		if (character == '/' || character < 0x20U || character == 0x7fU)
			character = '_';
		output[length] = (char)character;
		length++;
	}

	/* Without the trailing spaces. */
	while (length > 0U && output[length - 1U] == ' ')
		length--;
	output[length] = '\0';

	/* A name that is empty, or begins with a dot ("." and ".." among them), starts with '_'. */
	if (length == 0U) {
		(void)snprintf(output, size, "%s", id);
		return 0;
	}

	/* A leading dot would hide the folder, or name "." or "..". */
	if (output[0] == '.')
		output[0] = '_';

	/* Succeeded. */
	return 0;
}

/*
 * Reads a client's line: "HELLO 1", "MOUNT <request> <id>" or
 * "EJECT <request> <id>".  Returns 0 with what it asks, or EINVAL.
 */
int
volumed_parse(
	const char *line,
	int *ask,
	unsigned *request,
	char *id,
	size_t id_size)
{
	char word[8];
	char name[VOLUMED_NAME_MAX];
	unsigned long number;
	size_t length;
	char *end;
	int count;
	int same;

	/* The greeting. */
	same = strcmp(line, "HELLO 1");
	if (same == 0) {
		*ask = VOLUMED_ASK_HELLO;
		*request = 0U;
		return 0;
	}

	/* A request: its word, its number and the volume. */
	number = 0UL;
	count = sscanf(line, "%7s %lu %31s", word, &number, name);
	if (count != 3 || number > 0xffffffffUL)
		return EINVAL;

	/* Nothing may follow the volume's name. */
	length = strlen(name);
	end = strstr(line, name);
	if (end == NULL || end[length] != '\0')
		return EINVAL;

	/* Which request. */
	*ask = 0;
	same = strcmp(word, "MOUNT");
	if (same == 0)
		*ask = VOLUMED_ASK_MOUNT;
	same = strcmp(word, "EJECT");
	if (same == 0)
		*ask = VOLUMED_ASK_EJECT;
	if (*ask == 0)
		return EINVAL;

	/* The volume's name must fit. */
	if (length + 1U > id_size)
		return EINVAL;

	/* Succeeded. */
	*request = (unsigned)number;
	(void)snprintf(id, id_size, "%s", name);
	return 0;
}

/*
 * Tells whether a client may mount and eject: root, or the seat's user
 * (the owner of the display, whom sessiond gives it) unless that is the
 * login screen's account.  Returns 1 or 0.
 */
int
volumed_permitted(
	uid_t peer,
	uid_t seat,
	uid_t greeter)
{
	/* Root may. */
	if (peer == 0)
		return 1;

	/* Another user than the seat's may not. */
	if (peer != seat)
		return 0;

	/* The login screen has no session to mount for. */
	if (peer == greeter)
		return 0;

	/* Succeeded: the session's user. */
	return 1;
}

/*
 * Writes a volume as its VOLUME line (without the newline).  Returns 0 or
 * ENAMETOOLONG.
 */
int
volumed_format_volume(
	const struct volumed_volume *volume,
	char *output,
	size_t size)
{
	char label[VOLUMED_LABEL_MAX * 3U];
	char path[VOLUMED_PATH_MAX * 3U];
	const char *state;
	int written;
	int error;

	/* The label and the path as single words. */
	error = volumed_escape(volume->label, label, sizeof(label));
	if (error != 0)
		return error;
	error = volumed_escape(volume->path, path, sizeof(path));
	if (error != 0)
		return error;

	/* An unmounted volume has no path, shown as "-". */
	state = "available";
	if (volume->path[0] != '\0')
		state = "mounted";
	if (path[0] == '\0')
		(void)snprintf(path, sizeof(path), "-");

	/* The line. */
	written = snprintf(output, size, "VOLUME id=%s state=%s fs=%s size=%llu label=%s path=%s new=%u",
	    volume->id,
	    state,
	    volume->fs,
	    (unsigned long long)volume->bytes,
	    label,
	    path,
	    volume->fresh);
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;

	/* Succeeded. */
	return 0;
}

/* Tells whether a byte stays itself in a word of a line. */
static int
names_plain(
	unsigned char character)
{
	/* Letters, digits and a few marks; not the space, '%', '=' or a control character. */
	if (character >= 'a' && character <= 'z')
		return 1;
	if (character >= 'A' && character <= 'Z')
		return 1;
	if (character >= '0' && character <= '9')
		return 1;
	if (character == '-' || character == '_' || character == '.' || character == '/')
		return 1;

	/* Everything else is escaped (UTF-8 bytes too). */
	return 0;
}
