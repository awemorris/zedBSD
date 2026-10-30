/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Storage page's Trash (ws089-p023).
 *
 * The trash is the freedesktop.org home trash Files keeps (files/trash.c):
 * $XDG_DATA_HOME/Trash, or ~/.local/share/Trash.  Emptying removes what is
 * in its files/ and info/ folders (the folders themselves stay, as Files
 * needs them), on a thread of its own so that the page goes on drawing: a
 * folder's contents first, never through a symbolic link (a link is
 * removed, not what it names), and nothing on another file system than
 * the trash's.  Files sees its Trash empty when it reads the folder again.
 */

#include "storage-trash.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The deepest folders emptied (a deeper one is left, and counted as an error). */
#define TRASH_DEPTH		64U

static void trash_worker(void *argument);
static int trash_clear(struct se_trash *trash, int parent, const char *name, dev_t device, unsigned depth);
static int trash_clear_folder(struct se_trash *trash, int folder, dev_t device, unsigned depth);

/*
 * Gives the home trash's folder: $XDG_DATA_HOME/Trash when it is an
 * absolute path, else $HOME/.local/share/Trash.  Returns 0, ENOENT without
 * a home, or ENAMETOOLONG.
 */
int
se_trash_path(
	char *path,
	size_t size)
{
	const char *data;
	const char *home;
	int written;

	/* $XDG_DATA_HOME/Trash. */
	data = getenv("XDG_DATA_HOME");
	if (data != NULL && data[0] == '/') {
		written = snprintf(path, size, "%s/Trash", data);
		if (written < 0 || (size_t)written >= size)
			return ENAMETOOLONG;
		return 0;
	}

	/* $HOME/.local/share/Trash. */
	home = getenv("HOME");
	if (home == NULL || home[0] != '/')
		return ENOENT;
	written = snprintf(path, size, "%s/.local/share/Trash", home);
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;

	/* Succeeded: the trash's folder. */
	return 0;
}

/*
 * Starts emptying a trash on a thread of its own.  Returns 0, EBUSY while
 * it empties already, ENAMETOOLONG, or the thread's errno value.
 */
int
se_trash_empty_start(
	struct se_trash *trash,
	const char *path)
{
	struct kl_thread_options options;
	size_t length;
	int error;

	/* One emptying at a time. */
	if (trash->started && trash->state == SE_TRASH_EMPTYING)
		return EBUSY;
	se_trash_finish(trash);

	/* The trash's folder. */
	length = strlen(path);
	if (length >= sizeof(trash->path))
		return ENAMETOOLONG;
	memset(trash, 0, sizeof(*trash));
	(void)kl_mutex_init(&trash->lock);
	memcpy(trash->path, path, length + 1U);
	trash->state = SE_TRASH_EMPTYING;

	/* The thread. */
	memset(&options, 0, sizeof(options));
	options.name = "storage-trash";
	options.run = trash_worker;
	options.argument = trash;
	options.priority = KL_THREAD_PRIORITY_NORMAL;
	error = kl_thread_start(&options, &trash->thread);
	if (error != 0) {
		trash->state = SE_TRASH_IDLE;
		kl_mutex_destroy(&trash->lock);
		return error;
	}

	/* The thread runs, and must be joined. */
	trash->started = 1;

	/* Succeeded: it empties. */
	return 0;
}

/*
 * Tells where emptying has got to, with how many items went and the first
 * error met.
 */
unsigned
se_trash_poll(
	struct se_trash *trash,
	uint64_t *removed,
	int *error)
{
	unsigned state;

	/* Nothing started. */
	*removed = 0U;
	*error = 0;
	if (!trash->started)
		return SE_TRASH_IDLE;

	/* The state as it is now. */
	kl_mutex_lock(&trash->lock);

	state = trash->state;
	*removed = trash->removed;
	*error = trash->error;

	kl_mutex_unlock(&trash->lock);

	/* Reports it. */
	return state;
}

/*
 * Waits for the emptying to end and lets its thread go (the page calls it
 * once it is emptied, and at its end).
 */
void
se_trash_finish(
	struct se_trash *trash)
{
	/* Nothing started. */
	if (!trash->started)
		return;

	/* The thread joined. */
	(void)kl_thread_join(trash->thread);
	trash->thread = NULL;
	kl_mutex_destroy(&trash->lock);
	trash->started = 0;
}

/* Empties the trash's files/ and info/ folders, then says it is done. */
static void
trash_worker(
	void *argument)
{
	struct se_trash *trash;
	struct stat status;
	int descriptor;
	int error;
	int first;

	/* The trash's folder and its file system; a trash that is not there is empty. */
	trash = argument;
	first = 0;
	descriptor = open(trash->path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	if (descriptor >= 0) {
		error = fstat(descriptor, &status);
		if (error == 0) {
			first = trash_clear(trash, descriptor, "files", status.st_dev, 0U);
			error = trash_clear(trash, descriptor, "info", status.st_dev, 0U);
			if (first == 0)
				first = error;
		}

		/* The trash's folder closed. */
		(void)close(descriptor);
	}

	/* Done, with the first error met. */
	kl_mutex_lock(&trash->lock);

	trash->state = SE_TRASH_EMPTIED;
	if (trash->error == 0)
		trash->error = first;

	kl_mutex_unlock(&trash->lock);

	/* The thread ends. */
}

/* Empties one folder of the trash by its name under a parent (a missing one is empty); returns 0 or the first errno value. */
static int
trash_clear(
	struct se_trash *trash,
	int parent,
	const char *name,
	dev_t device,
	unsigned depth)
{
	int descriptor;
	int error;

	/* The folder, never through a link. */
	descriptor = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	if (descriptor < 0) {
		error = errno;
		if (error == ENOENT)
			return 0;
		return error;
	}

	/* Its contents. */
	error = trash_clear_folder(trash, descriptor, device, depth);
	return error;
}

/* Removes everything in an open folder (it takes the descriptor); returns 0 or the first errno value. */
static int
trash_clear_folder(
	struct se_trash *trash,
	int folder,
	dev_t device,
	unsigned depth)
{
	struct dirent *entry;
	struct stat status;
	int first;
	int error;
	int dot;
	int dotdot;
	int is_folder;
	DIR *listing;

	/* The listing. */
	listing = fdopendir(folder);
	if (listing == NULL) {
		error = errno;
		(void)close(folder);
		return error;
	}

	/* Each entry: a folder emptied and removed, anything else removed. */
	first = 0;
	for (;;) {
		entry = readdir(listing);
		if (entry == NULL)
			break;
		dot = strcmp(entry->d_name, ".");
		dotdot = strcmp(entry->d_name, "..");
		if (dot == 0 || dotdot == 0)
			continue;

		/* The entry itself, on the trash's file system. */
		error = fstatat(folder, entry->d_name, &status, AT_SYMLINK_NOFOLLOW);
		if (error == 0 && status.st_dev != device)
			error = EXDEV;
		is_folder = 0;
		if (error == 0)
			is_folder = S_ISDIR(status.st_mode);
		if (error == 0 && is_folder) {
			error = EMLINK;
			if (depth + 1U < TRASH_DEPTH)
				error = trash_clear(trash, folder, entry->d_name, device, depth + 1U);
			if (error == 0)
				error = unlinkat(folder, entry->d_name, AT_REMOVEDIR);
		} else if (error == 0) {
			error = unlinkat(folder, entry->d_name, 0);
		}

		/* Counted, or the first error kept. */
		if (error == 0) {
			kl_mutex_lock(&trash->lock);

			trash->removed++;

			kl_mutex_unlock(&trash->lock);
		} else if (first == 0) {
			first = errno;
			if (error > 0)
				first = error;
		}
	}

	/* The listing goes. */
	(void)closedir(listing);
	return first;
}
