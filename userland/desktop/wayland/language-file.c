/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The system's language file as the compositor reads it (WS158,
 * ws188-p002): the login screen's language (language.c) and the machine's
 * answer (machine-shell.c, on its thread) share the reading.  It knows no
 * server, so that the host tests build it alone.
 */

#include "userland/desktop/paths.h"

#include "language.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The longest line of the system's language file read. */
#define LANGUAGE_FILE_LINE_MAX	32U

/*
 * Reads the first line of the system's language file without its end, as
 * it is (ws188-p002: the machine's answer and the login screen share it).
 * Returns 0 with the line, or ENOENT when the file cannot be read or has
 * no line (the word is then empty).  It only reads a file, so the
 * machine's thread may call it.
 */
int
kwl_language_system_word(
	char *word,
	size_t size)
{
	char line[LANGUAGE_FILE_LINE_MAX];
	FILE *file;
	char *read;
	size_t length;

	/* Nothing yet. */
	word[0] = '\0';

	/* The file. */
	file = fopen(KWL_LANGUAGE_SYSTEM_PATH, "r");
	if (file == NULL)
		return ENOENT;

	/* Its first line. */
	read = fgets(line, sizeof(line), file);
	(void)fclose(file);
	if (read == NULL)
		return ENOENT;

	/* The line without its end, cut to the room. */
	length = strcspn(line, "\r\n");
	if (length >= size)
		length = size - 1U;
	memcpy(word, line, length);
	word[length] = '\0';

	/* Succeeded: the line as it is. */
	return 0;
}
