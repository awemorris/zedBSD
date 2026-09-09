/* Shared bounded fstab records. Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_COMMAND_FSTAB_H
#define ZEDBSD_COMMAND_FSTAB_H

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifndef FSTAB_PATH
#define FSTAB_PATH "/etc/fstab"
#endif

struct command_fstab {
	char storage[4096];
	char *source;
	char *target;
	char *type;
	char *options;
};

/* Decodes field escapes after whitespace splitting, without introducing NUL. */
static __inline int
command_fstab_decode(char *text)
{
	char *readp;
	char *writep;
	unsigned value;
	unsigned index;

	writep = text;
	for (readp = text; *readp != '\0'; readp++) {
		if (*readp != '\\') {
			*writep++ = *readp;
			continue;
		}
		value = 0;
		for (index = 0; index < 3; index++) {
			readp++;
			if (*readp < '0' || *readp > '7')
				return EINVAL;
			value = value * 8U + (unsigned)(*readp - '0');
		}
		if (value == 0 || value > 255U)
			return EINVAL;
		*writep++ = (char)value;
	}
	*writep = '\0';
	return 0;
}

/* Returns one record, EOF, or -1; malformed physical lines are fully consumed. */
static __inline int
command_fstab_next(FILE *stream, struct command_fstab *entry, unsigned *line)
{
	char *fields[6];
	char *cursor;
	size_t used;
	unsigned count;
	unsigned index;
	int byte;
	int invalid;
	int error;

	for (;;) {
		used = 0;
		invalid = 0;
		while ((byte = fgetc(stream)) != EOF && byte != '\n') {
			if (byte == 0 || used == sizeof(entry->storage) - 1U) {
				invalid = 1;
				continue;
			}
			entry->storage[used++] = (char)byte;
		}
		if (ferror(stream)) {
			errno = EIO;
			return -1;
		}
		if (byte == EOF && used == 0 && !invalid)
			return 0;
		(*line)++;
		entry->storage[used] = '\0';
		if (invalid) {
			errno = EINVAL;
			return -1;
		}

		/* Splits only literal whitespace; escaped whitespace stays in a field. */
		count = 0;
		cursor = entry->storage;
		for (;;) {
			while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r')
				cursor++;
			if (*cursor == '\0' || *cursor == '#')
				break;
			if (count == 6U) {
				errno = EINVAL;
				return -1;
			}
			fields[count++] = cursor;
			while (*cursor != '\0' && *cursor != ' ' &&
			    *cursor != '\t' && *cursor != '\r')
				cursor++;
			if (*cursor == '\0')
				break;
			*cursor++ = '\0';
		}
		if (count == 0)
			continue;
		if (count < 4U) {
			errno = EINVAL;
			return -1;
		}
		for (index = 0; index < count; index++) {
			error = command_fstab_decode(fields[index]);
			if (error != 0) {
				errno = error;
				return -1;
			}
			if (index >= 4U) {
				for (cursor = fields[index]; *cursor != '\0'; cursor++) {
					if (*cursor < '0' || *cursor > '9') {
						errno = EINVAL;
						return -1;
					}
				}
			}
		}
		entry->source = fields[0];
		entry->target = fields[1];
		entry->type = fields[2];
		entry->options = fields[3];
		return 1;
	}
}
#endif
