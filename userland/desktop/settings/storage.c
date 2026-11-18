/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Storage page's analysis and Trash, without their drawing
 * (page-storage.c, ws089-p023): an analysis started on the home or a
 * folder in it and stopped on request (storage-scan.c's workers), the
 * trash's size counted when the page is first shown and again after it is
 * emptied, and the emptying (storage-trash.c's thread).  The page is drawn
 * again while a count moves, at most every STORAGE_DRAW_MS.
 */

#include "settings.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How often a moving count draws the page again, and how often the loop looks while one moves (milliseconds). */
#define STORAGE_DRAW_MS		200U
#define STORAGE_POLL_MS		100

static void storage_home(struct se_storage *storage);
static void storage_trash_count(struct se_app *app);

/*
 * Starts analysing a folder (NULL: the home); one under way stops first.
 */
void
se_storage_analyze(
	struct se_app *app,
	const char *root)
{
	struct se_storage *storage;
	int error;

	/* The home known, and the folder: the home unless one is named. */
	storage = &app->storage;
	storage_home(storage);
	if (root == NULL)
		root = storage->home;
	if (root[0] == '\0') {
		(void)snprintf(storage->message, sizeof(storage->message), "%s", "There is no home folder to analyse.");
		storage->message_bad = 1;
		app->dirty = 1;
		return;
	}

	/* The last analysis ends, and the new one starts. */
	se_scan_stop(&storage->scan);
	(void)snprintf(storage->root, sizeof(storage->root), "%s", root);
	error = se_scan_start(&storage->scan, storage->root);
	se_log("STORAGE analyze root=%s error=%d", storage->root, error);
	storage->message[0] = '\0';
	if (error != 0) {
		(void)snprintf(storage->message, sizeof(storage->message), "The folder could not be read (%s).", strerror(error));
		storage->message_bad = 1;
	}

	/* The page shows it from now on. */
	se_scan_view(&storage->scan, &storage->view);
	app->dirty = 1;
}

/*
 * Stops the analysis under way; what it counted stays.
 */
void
se_storage_stop(
	struct se_app *app)
{
	struct se_storage *storage;

	/* Stopped, and shown as it stands. */
	storage = &app->storage;
	se_scan_stop(&storage->scan);
	se_scan_view(&storage->scan, &storage->view);
	se_log("STORAGE stopped root=%s bytes=%llu files=%llu", storage->root, (unsigned long long)storage->view.bytes, (unsigned long long)storage->view.files);
	app->dirty = 1;
}

/*
 * Empties the trash (after the page's confirmation).
 */
void
se_storage_empty_trash(
	struct se_app *app)
{
	struct se_storage *storage;
	int error;

	/* The trash's folder, and the emptying started. */
	storage = &app->storage;
	storage->confirming = 0;
	error = ENOENT;
	if (storage->trash_path[0] != '\0')
		error = se_trash_empty_start(&storage->trash, storage->trash_path);
	se_log("STORAGE empty-trash path=%s error=%d", storage->trash_path, error);
	if (error != 0) {
		(void)snprintf(storage->message, sizeof(storage->message), "The Trash could not be emptied (%s).", strerror(error));
		storage->message_bad = 1;
	}

	/* The page shows it. */
	app->dirty = 1;
}

/*
 * Follows the counts and the emptying: the page drawn again while a count
 * moves, the workers joined once they ended, the trash counted when the
 * page is first shown and again once it is emptied.
 */
void
se_storage_poll(
	struct se_app *app,
	uint64_t now)
{
	struct se_storage *storage;
	uint64_t removed;
	unsigned state;
	int error;

	/* The trash's size, counted the first time the page is shown. */
	storage = &app->storage;
	if (app->page == SE_PAGE_STORAGE && !storage->trash_asked)
		storage_trash_count(app);

	/* The analysis as it stands, drawn again when it moved (not more often than STORAGE_DRAW_MS). */
	se_scan_view(&storage->scan, &storage->view);
	se_scan_view(&storage->trash_scan, &storage->trash_view);
	if (storage->view.generation + storage->trash_view.generation != storage->drawn_generation &&
	    now - storage->drawn_at >= STORAGE_DRAW_MS) {
		storage->drawn_generation = storage->view.generation + storage->trash_view.generation;
		storage->drawn_at = now;
		if (app->page == SE_PAGE_STORAGE)
			app->dirty = 1;
	}

	/* Workers that ended by themselves are joined; the end is logged once. */
	if (storage->view.state == SE_SCAN_DONE && storage->scan.thread_count != 0U) {
		se_scan_finish(&storage->scan);
		se_log("STORAGE done root=%s bytes=%llu files=%llu unreadable=%llu", storage->root, (unsigned long long)storage->view.bytes,
		    (unsigned long long)storage->view.files, (unsigned long long)storage->view.unreadable);
		app->dirty = 1;
	}

	/* The trash's count, likewise. */
	if (storage->trash_view.state == SE_SCAN_DONE && storage->trash_scan.thread_count != 0U) {
		se_scan_finish(&storage->trash_scan);
		se_log("STORAGE trash bytes=%llu items=%llu", (unsigned long long)storage->trash_view.bytes, (unsigned long long)storage->trash_view.files);
		app->dirty = 1;
	}

	/* The emptying, once it is over: said, and the trash counted again. */
	state = se_trash_poll(&storage->trash, &removed, &error);
	if (state == SE_TRASH_EMPTIED) {
		se_trash_finish(&storage->trash);
		se_log("STORAGE emptied removed=%llu errno=%d", (unsigned long long)removed, error);
		storage->message_bad = 0;
		(void)snprintf(storage->message, sizeof(storage->message), "%s", "The Trash is empty.");
		if (error != 0) {
			(void)snprintf(storage->message, sizeof(storage->message), "Some items could not be removed from the Trash (%s).", strerror(error));
			storage->message_bad = 1;
		}

		/* Counted again. */
		storage_trash_count(app);
		app->dirty = 1;
	}
}

/*
 * Tells how long the loop may sleep before the storage wants a poll: a
 * short while while a count moves or the trash empties, else -1.
 */
int
se_storage_wait(
	const struct se_app *app)
{
	const struct se_storage *storage;

	/* A count moving, or the trash emptying. */
	storage = &app->storage;
	if (storage->view.state == SE_SCAN_RUNNING || storage->trash_view.state == SE_SCAN_RUNNING)
		return STORAGE_POLL_MS;
	if (storage->trash.started)
		return STORAGE_POLL_MS;
	if (storage->scan.thread_count != 0U || storage->trash_scan.thread_count != 0U)
		return STORAGE_POLL_MS;

	/* Nothing due. */
	return -1;
}

/*
 * Stops every count and waits for the emptying, at Settings' end.
 */
void
se_storage_close(
	struct se_app *app)
{
	struct se_storage *storage;

	/* The counts stop, and the emptying ends. */
	storage = &app->storage;
	se_scan_stop(&storage->scan);
	se_scan_stop(&storage->trash_scan);
	se_trash_finish(&storage->trash);
}

/* Finds the home folder once ($HOME). */
static void
storage_home(
	struct se_storage *storage)
{
	const char *home;

	/* Known already. */
	if (storage->home[0] != '\0')
		return;

	/* $HOME, when it is an absolute path. */
	home = getenv("HOME");
	if (home != NULL && home[0] == '/')
		(void)snprintf(storage->home, sizeof(storage->home), "%s", home);
}

/* Counts the trash's files (its size), on the scan's workers. */
static void
storage_trash_count(
	struct se_app *app)
{
	struct se_storage *storage;
	char files[SE_SCAN_PATH + 8U];
	int error;

	/* Its folder, once. */
	storage = &app->storage;
	storage->trash_asked = 1;
	if (storage->trash_path[0] == '\0') {
		error = se_trash_path(storage->trash_path, sizeof(storage->trash_path));
		if (error != 0)
			storage->trash_path[0] = '\0';
	}

	/* No home, no trash. */
	if (storage->trash_path[0] == '\0')
		return;

	/* Its files/ folder counted (none there: an empty trash). */
	se_scan_stop(&storage->trash_scan);
	(void)snprintf(files, sizeof(files), "%s/files", storage->trash_path);
	error = se_scan_start(&storage->trash_scan, files);
	if (error != 0)
		memset(&storage->trash_scan, 0, sizeof(storage->trash_scan));
	se_scan_view(&storage->trash_scan, &storage->trash_view);
}
