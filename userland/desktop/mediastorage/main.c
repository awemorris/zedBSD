/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Owns media-library persistence; desktop clients access it through the compositor. */
#include "userland/desktop/photos/photos.h"
#include "notify.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv);
static int media_execute(const char *root, const char *command, const char *argument);
static int media_add_list(const char *root);
static int media_remove_list(const char *root);

/*
 * Executes one locked metadata query, path-list import or metadata update.
 */
int
main(
	int argc,
	char **argv)
{
	char root[PH_PATH_MAX];
	char lockpath[PH_PATH_MAX];
	char *words[4];
	const char *command;
	const char *argument;
	int arguments;
	int same;
	int descriptor;
	int error;
	int length;
	int changed;
	int notify;
	int index;
	int lock;
	int importing;

	/*
	 * Takes "--no-notify" out of the arguments: the compositor's own runs
	 * pass it, because the compositor tells its watchers itself when the
	 * run ends (a second wakeup would make every app read the library
	 * twice).
	 */
	notify = 1;
	arguments = 0;
	for (index = 0; index < argc; index++) {
		/* The compositor's flag, not kept as a word. */
		same = strcmp(argv[index], "--no-notify");
		if (index >= 2 && same == 0) {
			notify = 0;
			continue;
		}

		/* More words than any command takes. */
		if (arguments == 4) {
			arguments = 5;
			break;
		}

		/* The word kept. */
		words[arguments] = argv[index];
		arguments++;
	}

	/* Requires one documented command; an optional root supports recovery and offline use. */
	if (arguments < 2 || arguments > 4) {
		fprintf(stderr, "usage: mediastorage list|add PATH|add-list|apply|remove-list [--root=PATH] [--no-notify]\n");
		return 2;
	}

	/* The words without the flag are read as the arguments from here on. */
	argc = arguments;
	argv = words;

	/* Derives the default library without guessing a home for a missing session. */
	error = ph_library_root(root, sizeof(root));
	command = argv[1];
	argument = "";
	if (argc > 2)
		argument = argv[2];
	arguments = argc;
	if (argc > 2) {
		same = strncmp(argv[argc - 1], "--root=", 7U);
		if (same == 0) {
			arguments--;
			length = snprintf(root, sizeof(root), "%s", argv[argc - 1] + 7U);
			error = 0;
			if (length < 0 || (size_t)length >= sizeof(root) || root[0] != '/')
				error = EINVAL;
		}
	}

	/* A single add has one path; list, add-list and apply carry no path arguments. */
	same = strcmp(command, "add");
	if (same == 0) {
		if (arguments != 3)
			error = EINVAL;
	} else {
		if (arguments != 2)
			error = EINVAL;
	}

	/* Creates and locks a stable inode shared by every invocation of the CLI. */
	if (error == 0) {
		length = snprintf(lockpath, sizeof(lockpath), "%s/.lock", root);
		if (length < 0 || (size_t)length >= sizeof(lockpath))
			error = ENAMETOOLONG;
		else
			error = ph_db_folders(lockpath);
	}

	/* The lock survives monthly metadata replacement and serializes read/modify/write. */
	descriptor = -1;
	if (error == 0) {
		descriptor = open(lockpath, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
		if (descriptor < 0)
			error = errno;
	}

	/*
	 * A change takes the lock alone; a list shares it with other lists.
	 * A list only reads, and metadata.db is replaced by rename, so it sees
	 * one completed change; it still waits for a change under way.
	 */
	if (error == 0) {
		lock = LOCK_EX;
		same = strcmp(command, "list");
		if (same == 0)
			lock = LOCK_SH;
		error = flock(descriptor, lock);
		if (error != 0)
			error = errno;
	}

	/* Loads the latest model under the lock, including changes from other applications. */
	if (error == 0)
		error = ph_db_load(root);

	/* An import first empties the copies an import killed earlier left (none can be under way: the lock is ours). */
	importing = strncmp(command, "add", 3U);
	if (error == 0 && importing == 0)
		ph_import_clean(root);
	if (error == 0)
		error = media_execute(root, command, argument);
	if (error == 0) {
		error = fflush(stdout);
		if (error != 0)
			error = EIO;
	}

	/* Ends persistence before telling the compositor to refresh its watchers. */
	if (descriptor >= 0)
		close(descriptor);
	changed = strcmp(command, "add");
	if (changed != 0)
		changed = strcmp(command, "add-list");
	if (changed != 0)
		changed = strcmp(command, "apply");
	if (changed != 0)
		changed = strcmp(command, "remove-list");
	if (changed == 0 && notify)
		media_db_notify();
	ph_library_release();
	media_database_release();
	if (error != 0) {
		fprintf(stderr, "mediastorage: %s failed: %s\n", command, strerror(error));
		if (error < 126)
			return error;
		return 1;
	}

	/* Succeeded: a complete metadata snapshot is on standard output. */
	return 0;
}

/*
 * Provides the legacy library's logging hook without revealing paths or content.
 */
void
ph_log(
	const char *format,
	...)
{
	(void)format;
}

/* Runs one CLI operation without allowing application code to access the database. */
static int
media_execute(
	const char *root,
	const char *command,
	const char *argument)
{
	struct ph_import_result result;
	int same;
	int error;

	/* Removal resolves stable IDs from the current database rather than arbitrary paths. */
	same = strcmp(command, "remove-list");
	if (same == 0) {
		error = media_remove_list(root);
		if (error != 0)
			return error;
		error = ph_snapshot_write(stdout, root);
		if (error != 0)
			return error;

		/* Succeeded: the response excludes committed deleted identities. */
		return 0;
	}

	/* Adds files or folders by copying, never moving the user's originals. */
	same = strcmp(command, "add");
	if (same == 0) {
		if (argument[0] == '\0')
			return EINVAL;
		error = ph_import(root, argument, &result);
		if (error != 0)
			return error;
		if (result.failed != 0U || (result.imported == 0U && result.duplicates == 0U)) {
			/* Retains successful imports even if another file in the folder failed. */
			error = ph_db_save(root);
			if (error != 0)
				return error;
			return ENOTSUP;
		}
	} else {
		/* A newline-separated path list is supplied on standard input. */
		same = strcmp(command, "add-list");
		if (same == 0) {
			error = media_add_list(root);
			if (error != 0)
				return error;
		} else {
			/* Applies id-based favorite, rotation and album changes to the fresh model. */
			same = strcmp(command, "apply");
			if (same == 0) {
				error = ph_snapshot_apply(stdin);
				if (error != 0)
					return error;
			} else {
				/* A list only reads the model; unknown commands cannot mutate it. */
				same = strcmp(command, "list");
				if (same != 0)
					return EINVAL;
			}
		}
	}

	/* Saves changed metadata through atomic JSON replacement. */
	error = ph_db_save(root);
	if (error != 0)
		return error;
	error = ph_snapshot_write(stdout, root);
	if (error != 0)
		return error;

	/* Succeeded: the response describes the newest persistent library. */
	return 0;
}

/* Imports an ordered path list while retaining partial successful additions. */
static int
media_add_list(
	const char *root)
{
	struct ph_import_result result;
	char path[PH_PATH_MAX];
	char *got;
	size_t length;
	unsigned accepted;
	unsigned count;
	int error;
	int first;

	/* Requires complete path lines and bounds one request's number of sources. */
	first = 0;
	accepted = 0U;
	count = 0U;
	for (;;) {
		got = fgets(path, sizeof(path), stdin);
		if (got == NULL)
			break;
		length = strlen(path);
		if (length == 0U || path[length - 1U] != '\n' || ++count > 256U) {
			first = EINVAL;
			break;
		}

		/* A path is data, including spaces; it is never passed to a shell. */
		path[length - 1U] = '\0';
		error = ph_import(root, path, &result);
		if (error == 0 && result.failed != 0U)
			error = EIO;
		if (error == 0 && result.imported == 0U && result.duplicates == 0U)
			error = ENOTSUP;
		if (error != 0 && first == 0)
			first = error;
		accepted += result.imported + result.duplicates;
	}

	/* A failed input stream cannot report an incomplete path list as successful. */
	error = ferror(stdin);
	if (error != 0 && first == 0)
		first = EIO;

	/* Commits accepted paths before reporting another path's failure. */
	error = ph_db_save(root);
	if (error != 0)
		return error;
	if (first != 0)
		return first;
	if (accepted == 0U)
		return ENOTSUP;

	/* Succeeded: every accepted original has a recoverable copy and metadata. */
	return 0;
}

/* Validates the full identity list before committing any managed-file deletion. */
static int
media_remove_list(
	const char *root)
{
	char ids[256U][PH_ID_SIZE];
	char line[PH_ID_SIZE + 2U];
	char *got;
	size_t count;
	size_t index;
	size_t prior;
	size_t length;
	size_t clean;
	long found;
	int same;
	int error;

	/* Complete lowercase hexadecimal identities are the CLI's only deletion input. */
	count = 0U;
	for (;;) {
		got = fgets(line, sizeof(line), stdin);
		if (got == NULL)
			break;
		length = strlen(line);
		clean = strspn(line, "0123456789abcdef");
		if (length != PH_ID_SIZE ||
		    clean != PH_ID_SIZE - 1U ||
		    line[length - 1U] != '\n' ||
		    count == 256U)
			return EINVAL;
		line[length - 1U] = '\0';
		found = ph_library_find_id(line);
		if (found < 0)
			return ENOENT;

		/* Duplicate IDs in one request must not turn a successful deletion into failure. */
		for (prior = 0U; prior < count; prior++) {
			same = strcmp(ids[prior], line);
			if (same == 0)
				break;
		}

		/* Only unique identities consume a deletion slot. */
		if (prior == count) {
			memcpy(ids[count], line, PH_ID_SIZE);
			count++;
		}
	}

	/* A malformed or truncated input stream never authorizes a partial request. */
	error = ferror(stdin);
	if (error != 0)
		return EIO;
	if (count == 0U)
		return EINVAL;
	for (index = 0U; index < count; index++) {
		error = media_database_remove(root, ids[index]);
		if (error != 0)
			return error;
	}

	/* Succeeded: each identity has a committed metadata removal and deleted copy. */
	return 0;
}
