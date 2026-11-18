/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Storage page's analysis of a folder's use (ws089-p023).
 *
 * The root's entries are read first: each folder becomes a group (beyond
 * SE_SCAN_GROUPS - 2 of them, "Other folders" takes the rest), and the
 * files directly in the root count for "Files here".  Each folder is then a
 * job that the workers take from a shared list: a worker reads its
 * entries, adds its files' bytes to its group, and puts its sub-folders on
 * the list for its group.  The bytes are those the file takes on the disk
 * (st_blocks, as du counts them); a file of a few links counts once; no
 * symbolic link is followed and no other file system is entered (as du
 * -x), so the totals agree with du -sx.  A folder that cannot be read is
 * counted apart and passed over.
 *
 * A stop is looked at between entries, so the workers end within a moment
 * and what they counted stays.  The workers are joined by se_scan_stop
 * or, once they ended by themselves, by se_scan_finish.
 */

#include "storage-scan.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* The groups the root's folders end in: "Files here" and "Other folders". */
#define SCAN_GROUP_HERE		0U
#define SCAN_GROUP_OTHER	1U
#define SCAN_GROUP_FIRST	2U

/* How many entries a worker reads before it looks at the stop and adds what it counted. */
#define SCAN_BATCH		256U

static void scan_worker(void *argument);
static int scan_take(struct se_scan *scan, struct se_scan_job *job);
static void scan_folder(struct se_scan *scan, const struct se_scan_job *job);
static int scan_push(struct se_scan *scan, const char *path, unsigned group);
static int scan_linked(struct se_scan *scan, dev_t device, ino_t inode);
static void scan_add(struct se_scan *scan, unsigned group, uint64_t bytes, uint64_t files);
static int scan_root(struct se_scan *scan);
static int scan_join(const char *folder, const char *name, char *path, size_t size);
static uint64_t scan_now_ms(void);
static int scan_larger(const void *left, const void *right);

/*
 * Starts analysing a folder: its entries read, its folders put on the list,
 * the workers started.  Returns 0, or an errno value (the root cannot be
 * read, no memory, no thread).
 */
int
se_scan_start(
	struct se_scan *scan,
	const char *root)
{
	struct kl_thread_options options;
	struct stat status;
	unsigned index;
	size_t length;
	int folder;
	int error;

	/* A root that fits and is a folder. */
	length = strlen(root);
	if (length == 0U || length >= sizeof(scan->root))
		return ENAMETOOLONG;
	error = lstat(root, &status);
	if (error != 0)
		return errno;
	folder = S_ISDIR(status.st_mode);
	if (!folder)
		return ENOTDIR;

	/* A fresh scan of it, with its lock. */
	memset(scan, 0, sizeof(*scan));
	(void)kl_mutex_init(&scan->lock);
	(void)kl_cond_init(&scan->wake);
	scan->initialized = 1;
	memcpy(scan->root, root, length + 1U);
	scan->device = status.st_dev;
	scan->state = SE_SCAN_RUNNING;
	scan->started_ms = scan_now_ms();
	(void)snprintf(scan->groups[SCAN_GROUP_HERE].name, sizeof(scan->groups[SCAN_GROUP_HERE].name), "%s", "Files here");
	(void)snprintf(scan->groups[SCAN_GROUP_OTHER].name, sizeof(scan->groups[SCAN_GROUP_OTHER].name), "%s", "Other folders");
	scan->group_count = SCAN_GROUP_FIRST;

	/* The root's own folder takes its bytes too (as du counts it). */
	scan->groups[SCAN_GROUP_HERE].bytes = (uint64_t)status.st_blocks * 512U;

	/* The root's entries: its files counted, its folders listed. */
	error = scan_root(scan);
	if (error != 0) {
		scan->state = SE_SCAN_IDLE;
		se_scan_finish(scan);
		return error;
	}

	/* The workers. */
	for (index = 0; index < SE_SCAN_THREADS; index++) {
		memset(&options, 0, sizeof(options));
		options.name = "storage-scan";
		options.run = scan_worker;
		options.argument = scan;
		options.priority = KL_THREAD_PRIORITY_NORMAL;
		error = kl_thread_start(&options, &scan->threads[index]);
		if (error != 0)
			break;
		scan->thread_count++;
	}

	/* Without any worker there is no scan. */
	if (scan->thread_count == 0U) {
		scan->state = SE_SCAN_IDLE;
		se_scan_finish(scan);
		return error;
	}

	/* Succeeded: the workers count. */
	return 0;
}

/*
 * Stops a scan: the workers end at their next entry and are joined; what
 * they counted stays.
 */
void
se_scan_stop(
	struct se_scan *scan)
{
	/* Nothing started. */
	if (!scan->initialized)
		return;

	/* The stop, told to every worker waiting for a job. */
	kl_mutex_lock(&scan->lock);

	if (scan->state == SE_SCAN_RUNNING) {
		scan->stop = 1;
		scan->state = SE_SCAN_STOPPED;
		scan->finished_ms = scan_now_ms();
		scan->generation++;
	}

	/* Every worker waiting hears it. */
	kl_cond_broadcast(&scan->wake);

	kl_mutex_unlock(&scan->lock);

	/* The workers end. */
	se_scan_finish(scan);
}

/*
 * Joins the workers of a scan that ended (or was stopped) and lets its
 * lists go; the totals stay for the page.  It waits for workers still
 * counting, so the page calls it once the state is no longer running.
 */
void
se_scan_finish(
	struct se_scan *scan)
{
	unsigned index;
	size_t job;

	/* Nothing started. */
	if (!scan->initialized)
		return;

	/* Every worker. */
	for (index = 0; index < scan->thread_count; index++) {
		(void)kl_thread_join(scan->threads[index]);
		scan->threads[index] = NULL;
	}

	/* No worker is left. */
	scan->thread_count = 0U;

	/* The folders left on the list, and the set of links. */
	for (job = 0; job < scan->job_count; job++)
		free(scan->jobs[job].path);
	free(scan->jobs);
	scan->jobs = NULL;
	scan->job_count = 0U;
	scan->job_capacity = 0U;
	free(scan->linked);
	scan->linked = NULL;
	scan->linked_count = 0U;
	scan->linked_capacity = 0U;
}

/*
 * Copies what a scan has counted so far: the groups that take anything,
 * the largest first, and the totals.
 */
void
se_scan_view(
	struct se_scan *scan,
	struct se_scan_view *view)
{
	unsigned index;

	/* Nothing started: an empty view. */
	memset(view, 0, sizeof(*view));
	if (!scan->initialized)
		return;

	/* The totals and the groups as they are now. */
	kl_mutex_lock(&scan->lock);

	memcpy(view->root, scan->root, sizeof(view->root));
	view->state = scan->state;
	view->generation = scan->generation;
	view->unreadable = scan->unreadable;
	for (index = 0; index < scan->group_count; index++) {
		view->bytes += scan->groups[index].bytes;
		view->files += scan->groups[index].files;
		if (scan->groups[index].bytes == 0U && scan->groups[index].files == 0U)
			continue;
		view->groups[view->group_count] = scan->groups[index];
		view->group_count++;
	}

	kl_mutex_unlock(&scan->lock);

	/* The largest first. */
	qsort(view->groups, view->group_count, sizeof(view->groups[0]), scan_larger);
}

/* Takes folders from the list and walks them until the list is empty and no worker walks any, or the scan stops. */
static void
scan_worker(
	void *argument)
{
	struct se_scan *scan;
	struct se_scan_job job;
	int taken;

	/* Each folder in turn. */
	scan = argument;
	for (;;) {
		taken = scan_take(scan, &job);
		if (!taken)
			break;
		scan_folder(scan, &job);
		free(job.path);

		/* The folder is done; the last worker to finish the last one ends the scan. */
		kl_mutex_lock(&scan->lock);

		scan->busy--;
		if (scan->job_count == 0U && scan->busy == 0U && scan->state == SE_SCAN_RUNNING) {
			scan->state = SE_SCAN_DONE;
			scan->finished_ms = scan_now_ms();
			scan->generation++;
		}

		/* The others hear the end, or that a folder is done. */
		kl_cond_broadcast(&scan->wake);

		kl_mutex_unlock(&scan->lock);
	}

	/* The worker ends. */
}

/* Waits for a folder to walk; returns 1 with it, 0 when the scan is over (stopped, or nothing is left). */
static int
scan_take(
	struct se_scan *scan,
	struct se_scan_job *job)
{
	int taken;

	/* A job, or the end. */
	taken = 0;
	kl_mutex_lock(&scan->lock);

	while (!scan->stop && scan->job_count == 0U && scan->busy != 0U)
		kl_cond_wait(&scan->wake, &scan->lock);
	if (!scan->stop && scan->job_count != 0U) {
		scan->job_count--;
		*job = scan->jobs[scan->job_count];
		scan->busy++;
		taken = 1;
	}

	/* Nothing left and nobody walking: the scan is done (a root without folders ends here). */
	if (!taken && scan->job_count == 0U && scan->busy == 0U && scan->state == SE_SCAN_RUNNING) {
		scan->state = SE_SCAN_DONE;
		scan->finished_ms = scan_now_ms();
		scan->generation++;
	}

	kl_mutex_unlock(&scan->lock);

	/* Reports whether one was taken. */
	return taken;
}

/* Walks one folder: its files counted for its group, its folders listed. */
static void
scan_folder(
	struct se_scan *scan,
	const struct se_scan_job *job)
{
	struct dirent *entry;
	struct stat status;
	char path[SE_SCAN_PATH];
	uint64_t bytes;
	uint64_t files;
	unsigned batch;
	int descriptor;
	int error;
	int counted;
	int is_folder;
	int dot;
	int dotdot;
	int stop;
	DIR *folder;

	/* The folder, never through a link; one that cannot be read is counted apart. */
	descriptor = open(job->path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	folder = NULL;
	if (descriptor >= 0)
		folder = fdopendir(descriptor);
	if (folder == NULL) {
		if (descriptor >= 0)
			(void)close(descriptor);

		/* The folder is counted as unreadable. */
		kl_mutex_lock(&scan->lock);

		scan->unreadable++;

		kl_mutex_unlock(&scan->lock);

		/* Nothing of it is walked. */
		return;
	}

	/* Each entry, the totals added now and then, until the stop. */
	bytes = 0U;
	files = 0U;
	batch = 0U;
	stop = 0;
	for (;;) {
		entry = readdir(folder);
		if (entry == NULL || stop)
			break;
		dot = strcmp(entry->d_name, ".");
		dotdot = strcmp(entry->d_name, "..");
		if (dot == 0 || dotdot == 0)
			continue;

		/* The entry itself, not what a link names. */
		error = fstatat(descriptor, entry->d_name, &status, AT_SYMLINK_NOFOLLOW);
		if (error != 0 || status.st_dev != scan->device)
			continue;

		/* A folder goes on the list (its own bytes count too). */
		is_folder = S_ISDIR(status.st_mode);
		if (is_folder) {
			error = scan_join(job->path, entry->d_name, path, sizeof(path));
			if (error == 0)
				(void)scan_push(scan, path, job->group);
			bytes += (uint64_t)status.st_blocks * 512U;
		} else {
			/* A file (of a few links, once). */
			counted = 1;
			if (status.st_nlink > 1)
				counted = scan_linked(scan, status.st_dev, status.st_ino);
			if (counted) {
				bytes += (uint64_t)status.st_blocks * 512U;
				files++;
			}
		}

		/* Now and then: what was counted, and whether to stop. */
		batch++;
		if (batch < SCAN_BATCH)
			continue;
		batch = 0U;
		scan_add(scan, job->group, bytes, files);
		bytes = 0U;
		files = 0U;

		/* Whether the scan was stopped meanwhile. */
		kl_mutex_lock(&scan->lock);

		stop = scan->stop;

		kl_mutex_unlock(&scan->lock);
	}

	/* The rest, and the folder closed. */
	scan_add(scan, job->group, bytes, files);
	(void)closedir(folder);
}

/* Puts a folder on the list for a group; returns 0 or ENOMEM. */
static int
scan_push(
	struct se_scan *scan,
	const char *path,
	unsigned group)
{
	struct se_scan_job *grown;
	size_t capacity;
	char *copy;

	/* The path's copy. */
	copy = strdup(path);
	if (copy == NULL)
		return ENOMEM;

	/* Room on the list, and the job; a worker waiting takes it. */
	kl_mutex_lock(&scan->lock);

	if (scan->job_count == scan->job_capacity) {
		capacity = scan->job_capacity * 2U;
		if (capacity == 0U)
			capacity = 64U;
		grown = realloc(scan->jobs, capacity * sizeof(scan->jobs[0]));
		if (grown == NULL) {
			kl_mutex_unlock(&scan->lock);
			free(copy);
			return ENOMEM;
		}

		/* The list grown. */
		scan->jobs = grown;
		scan->job_capacity = capacity;
	}

	/* The job, told to a worker. */
	scan->jobs[scan->job_count].path = copy;
	scan->jobs[scan->job_count].group = group;
	scan->job_count++;
	kl_cond_signal(&scan->wake);

	kl_mutex_unlock(&scan->lock);

	/* Succeeded: the folder waits. */
	return 0;
}

/*
 * Records a file of a few links; returns 1 the first time it is seen (it
 * is counted), 0 after (or 1 when there is no room to remember it).
 */
static int
scan_linked(
	struct se_scan *scan,
	dev_t device,
	ino_t inode)
{
	uint64_t *grown;
	uint64_t key;
	size_t capacity;
	size_t index;
	int first;

	/* The device and the inode as one key. */
	key = ((uint64_t)device << 40) ^ (uint64_t)inode;

	/* Seen already, or remembered now. */
	first = 1;
	kl_mutex_lock(&scan->lock);

	for (index = 0; index < scan->linked_count; index++) {
		if (scan->linked[index] == key) {
			first = 0;
			break;
		}
	}

	/* Room for a new one. */
	if (first && scan->linked_count == scan->linked_capacity) {
		capacity = scan->linked_capacity * 2U;
		if (capacity == 0U)
			capacity = 64U;
		grown = realloc(scan->linked, capacity * sizeof(scan->linked[0]));
		if (grown != NULL) {
			scan->linked = grown;
			scan->linked_capacity = capacity;
		}
	}

	/* The new one kept. */
	if (first && scan->linked_count < scan->linked_capacity) {
		scan->linked[scan->linked_count] = key;
		scan->linked_count++;
	}

	kl_mutex_unlock(&scan->lock);

	/* Reports whether it counts. */
	return first;
}

/* Adds bytes and files to a group (the page sees a new generation). */
static void
scan_add(
	struct se_scan *scan,
	unsigned group,
	uint64_t bytes,
	uint64_t files)
{
	/* Nothing to add. */
	if (bytes == 0U && files == 0U)
		return;

	/* Added. */
	kl_mutex_lock(&scan->lock);

	scan->groups[group].bytes += bytes;
	scan->groups[group].files += files;
	scan->generation++;

	kl_mutex_unlock(&scan->lock);
}

/* Reads the root's entries: its files for "Files here", a group and a job for each folder.  Returns 0 or an errno value. */
static int
scan_root(
	struct se_scan *scan)
{
	struct dirent *entry;
	struct stat status;
	struct se_scan_group *group;
	char path[SE_SCAN_PATH];
	unsigned which;
	int descriptor;
	int error;
	int counted;
	int is_folder;
	int dot;
	int dotdot;
	DIR *folder;

	/* The root, never through a link. */
	descriptor = open(scan->root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	if (descriptor < 0)
		return errno;
	folder = fdopendir(descriptor);
	if (folder == NULL) {
		error = errno;
		(void)close(descriptor);
		return error;
	}

	/* Each entry. */
	for (;;) {
		entry = readdir(folder);
		if (entry == NULL)
			break;
		dot = strcmp(entry->d_name, ".");
		dotdot = strcmp(entry->d_name, "..");
		if (dot == 0 || dotdot == 0)
			continue;

		/* The entry itself, on the root's file system. */
		error = fstatat(descriptor, entry->d_name, &status, AT_SYMLINK_NOFOLLOW);
		if (error != 0 || status.st_dev != scan->device)
			continue;

		/* A file of the root counts for "Files here" (one of a few links, once). */
		is_folder = S_ISDIR(status.st_mode);
		if (!is_folder) {
			counted = 1;
			if (status.st_nlink > 1)
				counted = scan_linked(scan, status.st_dev, status.st_ino);
			if (counted) {
				scan->groups[SCAN_GROUP_HERE].bytes += (uint64_t)status.st_blocks * 512U;
				scan->groups[SCAN_GROUP_HERE].files++;
			}

			/* The next entry. */
			continue;
		}

		/* A folder: a group of its own while there is room, else "Other folders"; its own bytes count, and it is walked. */
		which = SCAN_GROUP_OTHER;
		if (scan->group_count < SE_SCAN_GROUPS) {
			which = scan->group_count;
			group = &scan->groups[which];
			(void)snprintf(group->name, sizeof(group->name), "%s", entry->d_name);
			group->folder = 1;
			scan->group_count++;
		}

		/* Its own bytes, and it is walked. */
		scan->groups[which].bytes += (uint64_t)status.st_blocks * 512U;
		error = scan_join(scan->root, entry->d_name, path, sizeof(path));
		if (error == 0)
			error = scan_push(scan, path, which);
		if (error == ENOMEM) {
			(void)closedir(folder);
			return error;
		}
	}

	/* The root closed. */
	(void)closedir(folder);

	/* Succeeded: the root is read. */
	return 0;
}

/* Puts a folder's path and a name together; returns 0 or ENAMETOOLONG. */
static int
scan_join(
	const char *folder,
	const char *name,
	char *path,
	size_t size)
{
	int written;
	int top;

	/* The root "/" takes no second slash. */
	top = strcmp(folder, "/");
	if (top == 0)
		written = snprintf(path, size, "/%s", name);
	else
		written = snprintf(path, size, "%s/%s", folder, name);
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;

	/* Succeeded: the path. */
	return 0;
}

/* Gives a steady clock in milliseconds. */
static uint64_t
scan_now_ms(void)
{
	struct timespec now;

	/* The monotonic clock. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/* Orders groups the largest first (then by name). */
static int
scan_larger(
	const void *left,
	const void *right)
{
	const struct se_scan_group *a;
	const struct se_scan_group *b;

	/* By bytes, then by name. */
	a = left;
	b = right;
	if (a->bytes > b->bytes)
		return -1;
	if (a->bytes < b->bytes)
		return 1;
	return strcmp(a->name, b->name);
}
