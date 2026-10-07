/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The clipboard of files (Cut, Copy, Paste).
 *
 * The compositor has no clipboard between clients yet (no wl_data_device), and
 * each window of the file manager is a process of its own, so the
 * clipboard is a file: $XDG_RUNTIME_DIR/files.clipboard (or
 * /tmp/files-UID.clipboard), whose first line is "copy" or "cut"
 * and whose other lines are the paths.  It is replaced with a rename, so a
 * window never reads half of it.
 */

#include "ops.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The largest clipboard read. */
#define CLIP_MAX		(1024U * 1024U)

static void clip_path(char *path, size_t size);

/*
 * Puts paths on the clipboard, to be copied or moved (cut) at the paste.
 *
 * Returns 0, or an errno value.
 */
int
fm_clip_set(
	unsigned mode,
	char *const *paths,
	size_t count)
{
	char path[FM_OPS_PATH_MAX];
	char temporary[FM_OPS_PATH_MAX + 8];
	const char *word;
	FILE *file;
	size_t index;
	int status;

	/* A new file beside the clipboard. */
	clip_path(path, sizeof(path));
	snprintf(temporary, sizeof(temporary), "%s.new", path);
	file = fopen(temporary, "w");
	if (file == NULL)
		return errno;

	/* The mode, then a path a line. */
	word = "copy";
	if (mode == FM_CLIP_CUT)
		word = "cut";
	fprintf(file, "%s\n", word);
	for (index = 0; index < count; index++)
		fprintf(file, "%s\n", paths[index]);
	status = fclose(file);
	if (status != 0) {
		(void)unlink(temporary);
		return EIO;
	}

	/* It replaces the clipboard at once. */
	status = rename(temporary, path);
	if (status != 0) {
		(void)unlink(temporary);
		return errno;
	}

	/* Succeeded: the paths are on the clipboard. */
	return 0;
}

/*
 * Reads the clipboard: its mode and its paths (allocated; free them with
 * fm_paths_free).  An empty or missing clipboard is mode none.
 *
 * Returns 0, or an errno value.
 */
int
fm_clip_get(
	unsigned *mode,
	char ***paths,
	size_t *count)
{
	char path[FM_OPS_PATH_MAX];
	char line[FM_OPS_PATH_MAX];
	char *read;
	char **grown;
	FILE *file;
	size_t length;
	int match;

	/* Nothing yet. */
	*mode = FM_CLIP_NONE;
	*paths = NULL;
	*count = 0;

	/* The clipboard, if there is one. */
	clip_path(path, sizeof(path));
	file = fopen(path, "r");
	if (file == NULL)
		return 0;

	/* The mode. */
	read = fgets(line, sizeof(line), file);
	if (read == NULL) {
		fclose(file);
		return 0;
	}

	/* A cut, or else a copy. */
	match = strcmp(line, "cut\n");
	*mode = FM_CLIP_COPY;
	if (match == 0)
		*mode = FM_CLIP_CUT;

	/* Each path. */
	for (;;) {
		read = fgets(line, sizeof(line), file);
		if (read == NULL)
			break;
		length = strlen(line);
		if (length > 0 && line[length - 1U] == '\n')
			line[length - 1U] = '\0';
		if (line[0] != '/')
			continue;

		/* One more path. */
		grown = realloc(*paths, (*count + 1U) * sizeof(char *));
		if (grown == NULL)
			break;
		*paths = grown;
		(*paths)[*count] = strdup(line);
		if ((*paths)[*count] == NULL)
			break;
		(*count)++;
	}

	/* The file is not needed any more. */
	fclose(file);

	/* A clipboard of no paths is empty. */
	if (*count == 0)
		*mode = FM_CLIP_NONE;

	/* Succeeded: the clipboard is read. */
	return 0;
}

/*
 * Empties the clipboard (after a cut is pasted).
 */
void
fm_clip_clear(void)
{
	char path[FM_OPS_PATH_MAX];

	/* The file goes. */
	clip_path(path, sizeof(path));
	(void)unlink(path);
}

/*
 * Frees a table of paths.
 */
void
fm_paths_free(
	char **paths,
	size_t count)
{
	size_t index;

	/* Each path, then the table. */
	for (index = 0; index < count; index++)
		free(paths[index]);
	free(paths);
}

/* Writes the clipboard's path. */
static void
clip_path(
	char *path,
	size_t size)
{
	const char *runtime;

	/* In the runtime folder, or else in /tmp by the user's number. */
	runtime = getenv("XDG_RUNTIME_DIR");
	if (runtime != NULL && runtime[0] == '/') {
		snprintf(path, size, "%s/files.clipboard", runtime);
		return;
	}

	/* No runtime folder. */
	snprintf(path, size, "/tmp/files-%lu.clipboard", (unsigned long)getuid());
}
