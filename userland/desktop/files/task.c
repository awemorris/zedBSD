/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The background tasks of files (spec §33): copy, move, move to
 * the trash, put back, delete, duplicate and link.
 *
 * A task runs inside the main loop a slice of time at a time, so the
 * window keeps answering while a large folder is copied.  It first plans
 * its steps -- making a folder, copying a file, renaming, removing --
 * walking the folders a few entries at a time, and then carries the steps
 * out in order, a large file in chunks.  A move within one file system is
 * one rename; across file systems it is a copy followed by the removal of
 * the source, which is skipped for any source whose copy failed, so a
 * failure never loses the only copy.
 *
 * Names that are taken are not overwritten unless the user chose to: by
 * default the copy gets the next free name ("Report 2.pdf", or "Report
 * copy.pdf" for a duplicate).  A copy or a move may instead replace the
 * item there or skip the source, as its collisions table says (ws035-p106,
 * F-041).  A replaced item goes to the trash first, by steps planned before
 * the source's, so that undo can put it back (ws035-p110, F-050); only
 * when there is no trash is it removed.  A folder whose name a folder has
 * may be merged into it (ws035-p115, F-050): the source is replaced in the
 * task by its contents, each going into that folder and asked about again
 * when its own name is taken; a move removes the emptied folder at the end.
 */

#include "ops.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

/*
 * What task_resolve found for a source: its target is ready, it is
 * skipped, or the item it replaces is being removed first.
 */
enum task_target {
	TASK_TARGET_READY,
	TASK_TARGET_SKIP,
	TASK_TARGET_REMOVING
};

/* How many steps a task's table grows by, and how many walks. */
#define TASK_GROWTH		256U
#define TASK_WALK_GROWTH	16U

/* The largest list of extended attribute names, and the largest value, a copy carries. */
#define TASK_XATTR_NAMES	4096U
#define TASK_XATTR_VALUE	65536U

/*
 * The number the next task is given.
 *
 * It only increases, so a task is known by its number in the log for the
 * whole run; it starts at 1 so that 0 means no task.
 */
static unsigned task_next_id = 1U;

static int task_plan(struct fm_task *task, uint64_t deadline);
static int task_plan_source(struct fm_task *task);
static int task_resolve(struct fm_task *task, size_t owner, const char *source, const char *suffix, char *target, size_t size, unsigned *outcome);
static int task_plan_walk(struct fm_task *task);
static int task_plan_copy(struct fm_task *task, const char *source, const char *target, const struct stat *status);
static int task_plan_transfer(struct fm_task *task, const char *source, const char *target, const struct stat *status);
static int task_walk_finish(struct fm_task *task, int error);
static int task_plan_delete(struct fm_task *task, const char *source, const struct stat *status);
static int task_plan_trash(struct fm_task *task, const char *trash, const char *source, const struct stat *status, char *trashed, size_t size);
static int task_push_walk(struct fm_task *task, const char *source, const char *target, int post_order);
static struct fm_step *task_add(struct fm_task *task, unsigned kind, const char *source, const char *target);
static int task_run(struct fm_task *task, uint64_t deadline);
static int task_run_step(struct fm_task *task, struct fm_step *step, uint64_t deadline);
static int task_copy_chunk(struct fm_task *task, struct fm_step *step, uint64_t deadline);
static void task_copy_xattrs(int in, int out);
static void task_copy_close(struct fm_task *task, int keep);
static void task_fail(struct fm_task *task, size_t owner, int error, const char *path);
static char *task_join(const char *folder, const char *name);
static const char *task_base(const char *path);
static void task_parent(const char *path, char *parent, size_t size);
static int task_inside(const char *path, const char *folder);
static int task_mkdir_parents(const char *path);
static int task_grow(struct fm_task *task, size_t count);
static int task_read_names(const char *folder, char ***names, size_t *count);
static void task_free_names(char **names, size_t count);
static int task_compare_names(const void *left, const void *right);
static int task_plan_merged(struct fm_task *task);
static int task_merge_tables(const char *source, const char *target, char *const *names, size_t count, char ***items, char ***folders);

/*
 * Makes a task of a kind over sources (absolute paths, copied) into a
 * destination folder (the trash for a move to the trash; unused for a put
 * back or a delete); NULL when memory runs out.
 */
struct fm_task *
fm_task_new(
	unsigned kind,
	char *const *sources,
	size_t count,
	const char *destination)
{
	struct fm_task *task;
	size_t index;

	/* The task, planning. */
	task = calloc(1, sizeof(*task));
	if (task == NULL)
		return NULL;
	task->id = task_next_id;
	task_next_id++;
	task->kind = kind;
	task->state = FM_TASK_PLANNING;
	task->copy_in = -1;
	task->copy_out = -1;
	if (destination != NULL)
		snprintf(task->destination, sizeof(task->destination), "%s", destination);

	/* The tables of sources, results and failures. */
	task->sources = calloc(count + 1U, sizeof(task->sources[0]));
	task->results = calloc(count + 1U, sizeof(task->results[0]));
	task->failed = calloc(count + 1U, 1U);
	task->collisions = calloc(count + 1U, 1U);
	task->replaced = calloc(count + 1U, sizeof(task->replaced[0]));
	task->folders = calloc(count + 1U, sizeof(task->folders[0]));
	if (task->sources == NULL ||
	    task->results == NULL ||
	    task->failed == NULL ||
	    task->collisions == NULL ||
	    task->replaced == NULL ||
	    task->folders == NULL) {
		fm_task_free(task);
		return NULL;
	}

	/* The sources, copied. */
	for (index = 0; index < count; index++) {
		task->sources[index] = strdup(sources[index]);
		if (task->sources[index] == NULL) {
			fm_task_free(task);
			return NULL;
		}

		/* One more source is kept. */
		task->source_count++;
	}

	/* Succeeded: the task plans at its first step. */
	return task;
}

/*
 * Moves a task on for about a budget of milliseconds.
 *
 * Returns 1 while there is more to do, 0 once the task is finished (done
 * or cancelled).
 */
int
fm_task_step(
	struct fm_task *task,
	uint64_t budget_ms)
{
	uint64_t deadline;
	int more;

	/* A finished task has nothing to do. */
	if (task->state == FM_TASK_DONE || task->state == FM_TASK_CANCELLED)
		return 0;

	/* The time the slice ends. */
	deadline = fm_ops_clock() + budget_ms;

	/* Planning, until the plan is whole or the time is up. */
	if (task->state == FM_TASK_PLANNING) {
		more = task_plan(task, deadline);
		if (more != 0)
			return 1;
		task->state = FM_TASK_RUNNING;
	}

	/* Carrying out the steps. */
	more = task_run(task, deadline);
	if (more != 0)
		return 1;

	/* Succeeded: every step was carried out. */
	task->state = FM_TASK_DONE;
	return 0;
}

/*
 * Stops a task: a file being copied is removed, the steps not yet carried
 * out are not.
 */
void
fm_task_cancel(
	struct fm_task *task)
{
	/* A finished task stays as it is. */
	if (task->state == FM_TASK_DONE || task->state == FM_TASK_CANCELLED)
		return;

	/* The half-copied file goes, and the task ends. */
	task_copy_close(task, 0);
	task->state = FM_TASK_CANCELLED;
}

/*
 * Frees a task and everything it holds (its open folders and files too).
 */
void
fm_task_free(
	struct fm_task *task)
{
	size_t index;

	/* Nothing to free. */
	if (task == NULL)
		return;

	/* A file being copied is closed (and its half copy removed). */
	task_copy_close(task, 0);

	/* The folders still open, with their paths. */
	for (index = 0; index < task->walk_count; index++) {
		if (task->walks[index].directory != NULL)
			closedir(task->walks[index].directory);
		free(task->walks[index].source);
		free(task->walks[index].target);
	}

	/* The table of walks. */
	free(task->walks);

	/* The steps' paths and the table. */
	for (index = 0; index < task->step_count; index++) {
		free(task->steps[index].source);
		free(task->steps[index].target);
	}

	/* The table of steps. */
	free(task->steps);

	/* The sources, the results, the replaced items' places in the trash and the sources' own folders. */
	for (index = 0; index < task->source_count; index++) {
		free(task->sources[index]);
		if (task->results != NULL)
			free(task->results[index]);
		if (task->replaced != NULL)
			free(task->replaced[index]);
		if (task->folders != NULL)
			free(task->folders[index]);
	}

	/* The folders a move merged. */
	for (index = 0; index < task->merged_count; index++)
		free(task->merged[index]);

	/* The tables themselves. */
	free(task->sources);
	free(task->results);
	free(task->failed);
	free(task->collisions);
	free(task->replaced);
	free(task->folders);
	free(task->merged);
	free(task);
}

/*
 * Names what a kind of task does, for the log and the status pill.
 */
const char *
fm_task_verb(
	unsigned kind)
{
	/* Each kind's verb. */
	switch (kind) {
	case FM_TASK_COPY:
		return "copy";
	case FM_TASK_MOVE:
		return "move";
	case FM_TASK_TRASH:
		return "trash";
	case FM_TASK_RESTORE:
		return "restore";
	case FM_TASK_DELETE:
		return "delete";
	case FM_TASK_DUPLICATE:
		return "duplicate";
	case FM_TASK_LINK:
		return "link";
	default:
		break;
	}

	/* A kind this program does not know. */
	return "task";
}

/*
 * Tells whether a source of a copy or a move would land on a name its
 * destination already has (another item than the source itself): 1 when
 * it would, 0 otherwise.
 */
int
fm_task_collides(
	const struct fm_task *task,
	size_t index)
{
	struct stat status;
	char path[2 * FM_OPS_PATH_MAX + 32];
	int written;
	int taken;
	int same;

	/* Only a copy and a move can meet a taken name. */
	if (task->kind != FM_TASK_COPY && task->kind != FM_TASK_MOVE)
		return 0;
	if (index >= task->source_count)
		return 0;

	/* The path the source would take under its own name. */
	written = snprintf(path, sizeof(path), "%s/%s", fm_task_folder(task, index), task_base(task->sources[index]));
	if (written < 0 || (size_t)written >= sizeof(path))
		return 0;

	/* The source itself (a copy or a move into its own folder) is not in the way. */
	same = strcmp(path, task->sources[index]);
	if (same == 0)
		return 0;

	/* Nothing there: the name is free. */
	taken = lstat(path, &status);
	if (taken != 0)
		return 0;

	/* Succeeded: another item has the name. */
	return 1;
}

/*
 * Tells whether a source of a copy or a move can be merged into what has
 * its name in the destination (ws035-p115): both are folders (not links to
 * folders), and neither holds the other.  1 when it can, 0 otherwise.
 */
int
fm_task_can_merge(
	const struct fm_task *task,
	size_t index)
{
	struct stat status;
	char target[2 * FM_OPS_PATH_MAX + 32];
	int collides;
	int written;
	int error;
	int folder;
	int inside;

	/* Only a source whose name is taken by another item. */
	collides = fm_task_collides(task, index);
	if (collides == 0)
		return 0;

	/* The source is a folder. */
	error = lstat(task->sources[index], &status);
	if (error != 0)
		return 0;
	folder = S_ISDIR(status.st_mode);
	if (folder == 0)
		return 0;

	/* The item with its name is a folder too. */
	written = snprintf(target, sizeof(target), "%s/%s", fm_task_folder(task, index), task_base(task->sources[index]));
	if (written < 0 || (size_t)written >= sizeof(target))
		return 0;
	error = lstat(target, &status);
	if (error != 0)
		return 0;
	folder = S_ISDIR(status.st_mode);
	if (folder == 0)
		return 0;

	/* A folder that holds the other, either way round, would be merged into itself. */
	inside = task_inside(target, task->sources[index]);
	if (inside != 0)
		return 0;
	inside = task_inside(task->sources[index], target);
	if (inside != 0)
		return 0;

	/* Succeeded: the two folders can be merged. */
	return 1;
}

/*
 * Merges a source folder into the folder with its name in the source's
 * destination (ws035-p115): at its place in the task the source is
 * replaced by the items it holds, sorted by name, each going into that
 * folder and keeping both until an answer says otherwise.  A move also
 * keeps the source folder to remove once it is empty.
 *
 * Returns 0 with how many items took the source's place, or an errno
 * value with the task unchanged.
 */
int
fm_task_merge(
	struct fm_task *task,
	size_t index,
	size_t *added)
{
	char target[2 * FM_OPS_PATH_MAX + 32];
	char **names;
	char **items;
	char **item_folders;
	char **merged;
	char *merged_source;
	size_t name_count;
	size_t name_index;
	size_t count;
	size_t tail;
	int can_merge;
	int written;
	int error;

	/* Nothing is added until the merge is whole. */
	*added = 0U;

	/* Only two folders merge. */
	can_merge = fm_task_can_merge(task, index);
	if (can_merge == 0)
		return EINVAL;

	/* The folder the source merges into. */
	written = snprintf(target, sizeof(target), "%s/%s", fm_task_folder(task, index), task_base(task->sources[index]));
	if (written < 0 || (size_t)written >= sizeof(target))
		return ENAMETOOLONG;

	/* The names of the items the source holds, sorted. */
	error = task_read_names(task->sources[index], &names, &name_count);
	if (error != 0)
		return error;

	/* Each item's path in the source, and the merged folder it goes into; the names are not needed after. */
	error = task_merge_tables(task->sources[index], target, names, name_count, &items, &item_folders);
	task_free_names(names, name_count);
	if (error != 0)
		return error;

	/* A move keeps the source folder, removed once its contents have moved. */
	merged_source = NULL;
	if (task->kind == FM_TASK_MOVE) {
		merged_source = strdup(task->sources[index]);
		if (merged_source == NULL) {
			task_free_names(items, name_count);
			task_free_names(item_folders, name_count);
			return ENOMEM;
		}

		/* Room for it among the merged folders. */
		merged = realloc(task->merged, (task->merged_count + 1U) * sizeof(task->merged[0]));
		if (merged == NULL) {
			free(merged_source);
			task_free_names(items, name_count);
			task_free_names(item_folders, name_count);
			return ENOMEM;
		}

		/* The grown table of merged folders. */
		task->merged = merged;
	}

	/* Room in the task's tables for the items in place of the source. */
	count = task->source_count - 1U + name_count;
	error = task_grow(task, count);
	if (error != 0) {
		free(merged_source);
		task_free_names(items, name_count);
		task_free_names(item_folders, name_count);
		return error;
	}

	/* The source and its own folder go; it has no result and replaced nothing yet. */
	free(task->sources[index]);
	free(task->folders[index]);
	free(task->results[index]);
	free(task->replaced[index]);

	/* The sources after it move along to make room for the items. */
	tail = task->source_count - index - 1U;
	memmove(&task->sources[index + name_count], &task->sources[index + 1U], tail * sizeof(task->sources[0]));
	memmove(&task->folders[index + name_count], &task->folders[index + 1U], tail * sizeof(task->folders[0]));
	memmove(&task->results[index + name_count], &task->results[index + 1U], tail * sizeof(task->results[0]));
	memmove(&task->replaced[index + name_count], &task->replaced[index + 1U], tail * sizeof(task->replaced[0]));
	memmove(&task->failed[index + name_count], &task->failed[index + 1U], tail * sizeof(task->failed[0]));
	memmove(&task->collisions[index + name_count], &task->collisions[index + 1U], tail * sizeof(task->collisions[0]));

	/* The items in the source's place, each into the merged folder, keeping both until answered. */
	for (name_index = 0; name_index < name_count; name_index++) {
		task->sources[index + name_index] = items[name_index];
		task->folders[index + name_index] = item_folders[name_index];
		task->results[index + name_index] = NULL;
		task->replaced[index + name_index] = NULL;
		task->failed[index + name_index] = 0;
		task->collisions[index + name_index] = FM_COLLISION_KEEP_BOTH;
	}

	/* The entries past the new end are empty, as the tables' terminators. */
	task->sources[count] = NULL;
	task->folders[count] = NULL;
	task->results[count] = NULL;
	task->replaced[count] = NULL;
	task->source_count = count;

	/* The moved folder to remove at the end. */
	if (merged_source != NULL) {
		task->merged[task->merged_count] = merged_source;
		task->merged_count++;
	}

	/* The tables go; the paths in them now belong to the task. */
	free(items);
	free(item_folders);

	/* Succeeded: the items stand where the source stood. */
	*added = name_count;
	return 0;
}

/*
 * Returns the folder a source of a task goes into: its own when it has one
 * (a merged folder's items, a redo's pairs; ws035-p115), else the task's
 * destination.
 */
const char *
fm_task_folder(
	const struct fm_task *task,
	size_t index)
{
	/* A source past the table, or without a folder of its own, goes to the destination. */
	if (task->folders == NULL || index >= task->source_count)
		return task->destination;
	if (task->folders[index] == NULL)
		return task->destination;

	/* Reports the source's own folder. */
	return task->folders[index];
}

/*
 * Gives a source of a task a folder of its own to go into, in place of the
 * destination (ws035-p115: a redo puts each item where it first went).
 * Returns 0, or an errno value.
 */
int
fm_task_set_folder(
	struct fm_task *task,
	size_t index,
	const char *folder)
{
	char *copy;

	/* Only a source of the task. */
	if (index >= task->source_count)
		return EINVAL;

	/* The folder, copied. */
	copy = strdup(folder);
	if (copy == NULL)
		return ENOMEM;

	/* Succeeded: it replaces the one the source had. */
	free(task->folders[index]);
	task->folders[index] = copy;
	return 0;
}

/*
 * Makes a folder and the folders above it that are missing (mkdir -p).
 * Returns 0 once the folder is there, or an errno value.
 */
int
fm_ops_mkdir_parents(
	const char *path)
{
	int error;

	/* The folders, one level at a time. */
	error = task_mkdir_parents(path);
	if (error != 0)
		return error;

	/* Succeeded: the folder is there. */
	return 0;
}

/*
 * Finds a free path for a name in a folder: the name itself, or with the
 * suffix (" copy" for a duplicate), then with a number ("name 2.ext",
 * "name copy 2.ext").  A folder's name keeps its dots whole.
 *
 * Returns 0, or ENAMETOOLONG when no such path fits.
 */
int
fm_unique_name(
	const char *folder,
	const char *name,
	const char *suffix,
	char *path,
	size_t size)
{
	struct stat status;
	const char *dot;
	char base[FM_OPS_PATH_MAX];
	char extension[FM_OPS_PATH_MAX];
	size_t length;
	int written;
	int taken;
	int folder_name;
	unsigned number;

	/* A folder's name is not split (its dots are part of it). */
	written = snprintf(path, size, "%s/%s", folder, name);
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;
	folder_name = 0;
	taken = lstat(path, &status);
	if (taken == 0)
		folder_name = S_ISDIR(status.st_mode);

	/* The name's stem and extension (a leading dot is not an extension). */
	dot = strrchr(name, '.');
	if (dot == NULL || dot == name || folder_name != 0)
		dot = name + strlen(name);
	length = (size_t)(dot - name);
	if (length >= sizeof(base))
		return ENAMETOOLONG;
	memcpy(base, name, length);
	base[length] = '\0';
	snprintf(extension, sizeof(extension), "%s", dot);
	if (suffix == NULL)
		suffix = "";

	/* The name with its suffix, then with 2, 3, ... until one is free. */
	for (number = 1; number < 10000U; number++) {
		if (number == 1U)
			written = snprintf(path, size, "%s/%s%s%s", folder, base, suffix, extension);
		else
			written = snprintf(path, size, "%s/%s%s %u%s", folder, base, suffix, number, extension);
		if (written < 0 || (size_t)written >= size)
			return ENAMETOOLONG;

		/* A path nothing is at is free. */
		taken = lstat(path, &status);
		if (taken != 0)
			return 0;
	}

	/* Ten thousand copies of one name: give up. */
	return EEXIST;
}

/*
 * Returns a monotonic time in milliseconds.
 */
uint64_t
fm_ops_clock(void)
{
	struct timespec now;
	int status;

	/* The monotonic clock. */
	status = clock_gettime(CLOCK_MONOTONIC, &now);
	if (status != 0)
		return 0U;

	/* Reports it in milliseconds. */
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/* Plans until the plan is whole (0) or the deadline passes (1). */
static int
task_plan(
	struct fm_task *task,
	uint64_t deadline)
{
	uint64_t now;

	/* Walks and sources, one at a time (a failure is recorded with its source and the plan goes on). */
	for (;;) {
		/* A folder being walked goes on first, then the next source, until none is left. */
		if (task->walk_count != 0U) {
			(void)task_plan_walk(task);
		} else if (task->source_index < task->source_count) {
			(void)task_plan_source(task);

			/* A source whose replaced item is being removed is planned again once that is planned. */
			if (task->replacing == 0)
				task->source_index++;
		} else if (task->merged_count != 0U && task->merged_planned == 0) {
			/* Last, the folders a move merged are removed once empty (ws035-p115). */
			(void)task_plan_merged(task);
		} else {
			return 0;
		}

		/* The time is up: the rest in the next slice. */
		now = fm_ops_clock();
		if (now >= deadline)
			return 1;
	}
}

/* Plans the steps of the next source (a folder's contents follow in the walk); nonzero on failure. */
static int
task_plan_source(
	struct fm_task *task)
{
	struct stat status;
	struct stat target_status;
	struct fm_step *step;
	char target[2 * FM_OPS_PATH_MAX + 32];
	char parent[FM_OPS_PATH_MAX];
	char original[FM_OPS_PATH_MAX];
	char trash[FM_OPS_PATH_MAX];
	const char *source;
	const char *suffix;
	const char *folder;
	size_t owner;
	time_t deleted;
	unsigned outcome;
	int same_device;
	int same_folder;
	int error;

	/* The source, the folder it goes into (a copy's or a move's), and what it is; its steps are its own. */
	owner = task->source_index;
	task->owner = owner;
	source = task->sources[owner];
	folder = fm_task_folder(task, owner);
	error = lstat(source, &status);
	if (error != 0) {
		task_fail(task, owner, errno, source);
		return -1;
	}

	/* Each kind plans differently. */
	switch (task->kind) {
	case FM_TASK_COPY:
	case FM_TASK_DUPLICATE:
		/* A folder cannot be copied into itself. */
		error = task_inside(folder, source);
		if (error != 0) {
			task_fail(task, owner, EINVAL, source);
			return -1;
		}

		/* The target in the destination, as the name's collision says, then the copy of the tree. */
		suffix = NULL;
		if (task->kind == FM_TASK_DUPLICATE)
			suffix = " copy";
		error = task_resolve(task, owner, source, suffix, target, sizeof(target), &outcome);
		if (error != 0) {
			task_fail(task, owner, error, source);
			return -1;
		}

		/* A skipped source, or one whose replaced item is removed first, plans nothing of its own now. */
		if (outcome != TASK_TARGET_READY)
			return 0;

		/* The copy goes there; the tree is planned. */
		task->results[owner] = strdup(target);
		error = task_plan_copy(task, source, target, &status);
		break;
	case FM_TASK_MOVE:
		/* An item already in the destination stays where it is. */
		task_parent(source, parent, sizeof(parent));
		same_folder = strcmp(parent, folder);
		if (same_folder == 0) {
			task->results[owner] = strdup(source);
			return 0;
		}

		/* A folder cannot be moved into itself. */
		error = task_inside(folder, source);
		if (error != 0) {
			task_fail(task, owner, EINVAL, source);
			return -1;
		}

		/* The target in the destination, as the name's collision says. */
		error = task_resolve(task, owner, source, NULL, target, sizeof(target), &outcome);
		if (error != 0) {
			task_fail(task, owner, error, source);
			return -1;
		}

		/* A skipped source, or one whose replaced item is removed first, plans nothing of its own now. */
		if (outcome != TASK_TARGET_READY)
			return 0;

		/* The item goes there. */
		task->results[owner] = strdup(target);

		/* Within one file system a rename; across, a copy and the source's removal. */
		same_device = 0;
		error = stat(folder, &target_status);
		if (error == 0 && target_status.st_dev == status.st_dev)
			same_device = 1;
		if (same_device != 0) {
			step = task_add(task, FM_STEP_RENAME, source, target);
			error = 0;
			if (step == NULL)
				error = ENOMEM;
		} else {
			error = task_plan_transfer(task, source, target, &status);
		}

		/* The move is planned. */
		break;
	case FM_TASK_TRASH:
		/*
		 * The item into its trash: its volume's, or the home trash
		 * (ws127-p003); the destination when neither is there.
		 */
		error = fm_trash_for(source, trash, sizeof(trash));
		if (error != 0)
			snprintf(trash, sizeof(trash), "%s", task->destination);

		/* The item into that trash, with the record of where it was. */
		error = task_plan_trash(task, trash, source, &status, target, sizeof(target));
		if (error == 0)
			task->results[owner] = strdup(target);

		/* The move into the trash is planned. */
		break;
	case FM_TASK_RESTORE:
		/* The trash the item is in (a volume's, ws127-p003), else the destination. */
		error = fm_trash_of(source, trash, sizeof(trash));
		if (error != 0)
			snprintf(trash, sizeof(trash), "%s", task->destination);

		/* Where the item was, from its record in that trash. */
		error = fm_trash_info_read(trash, task_base(source), original, sizeof(original), &deleted);
		if (error != 0) {
			task_fail(task, owner, error, source);
			return -1;
		}

		/* A free name in the folder it was in (made again if it is gone). */
		task_parent(original, parent, sizeof(parent));
		(void)task_mkdir_parents(parent);
		error = fm_unique_name(parent, task_base(original), NULL, target, sizeof(target));
		if (error != 0) {
			task_fail(task, owner, error, source);
			return -1;
		}

		/* The item goes there. */
		task->results[owner] = strdup(target);

		/* The item back: a rename, or a copy and a removal across file systems. */
		same_device = 0;
		error = stat(parent, &target_status);
		if (error == 0 && target_status.st_dev == status.st_dev)
			same_device = 1;
		if (same_device != 0) {
			step = task_add(task, FM_STEP_RENAME, source, target);
			error = 0;
			if (step == NULL)
				error = ENOMEM;
		} else {
			error = task_plan_transfer(task, source, target, &status);
		}

		/* The record goes once the item is back. */
		snprintf(target, sizeof(target), "%s/info/%s.trashinfo", trash, task_base(source));
		step = task_add(task, FM_STEP_UNTRASHINFO, target, NULL);
		if (step == NULL)
			error = ENOMEM;
		break;
	case FM_TASK_DELETE:
		/* The tree, children before their folders. */
		task->results[owner] = strdup(source);
		error = task_plan_delete(task, source, &status);
		break;
	case FM_TASK_LINK:
		/* A symbolic link to the source under a free name. */
		error = fm_unique_name(task->destination, task_base(source), NULL, target, sizeof(target));
		if (error != 0) {
			task_fail(task, owner, error, source);
			return -1;
		}

		/* The link goes there. */
		task->results[owner] = strdup(target);
		step = task_add(task, FM_STEP_SYMLINK, source, target);
		error = 0;
		if (step == NULL)
			error = ENOMEM;
		break;
	default:
		error = EINVAL;
		break;
	}

	/* Reports a source that could not be planned. */
	if (error != 0) {
		task_fail(task, owner, error, source);
		return -1;
	}

	/* Succeeded: the source's first steps are planned. */
	return 0;
}

/*
 * Finds where a source of a task goes in its destination, as the source's
 * collision choice says: the next free name (keep both, or a name nobody
 * has), nowhere (skip), or the taken name itself once the item there is
 * removed (replace): its removal is planned now, and the source is planned
 * again next with outcome ready.  Returns 0 with the outcome, or an errno
 * value.
 */
static int
task_resolve(
	struct fm_task *task,
	size_t owner,
	const char *source,
	const char *suffix,
	char *target,
	size_t size,
	unsigned *outcome)
{
	struct stat status;
	char trash[FM_OPS_PATH_MAX];
	char trashed[2 * FM_OPS_PATH_MAX + 32];
	const char *folder;
	unsigned collision;
	int collides;
	int written;
	int inside;
	int error;

	/* Ready unless said otherwise; the source goes into its own folder (a merged one's, ws035-p115) or the destination. */
	*outcome = TASK_TARGET_READY;
	folder = fm_task_folder(task, owner);

	/* The replaced item's removal is planned: the source takes its name. */
	if (task->replacing != 0) {
		task->replacing = 0;
		written = snprintf(target, size, "%s/%s", folder, task_base(source));
		if (written < 0 || (size_t)written >= size)
			return ENAMETOOLONG;
		return 0;
	}

	/* What the user chose for this source's name, and whether it is taken. */
	collision = task->collisions[owner];
	collides = fm_task_collides(task, owner);

	/* A free name, or a choice to keep both, takes the next free name. */
	if (collides == 0 || collision == FM_COLLISION_KEEP_BOTH) {
		error = fm_unique_name(folder, task_base(source), suffix, target, size);
		if (error != 0)
			return error;
		return 0;
	}

	/* A skipped source goes nowhere; it is not a failure. */
	if (collision == FM_COLLISION_SKIP) {
		*outcome = TASK_TARGET_SKIP;
		task->skip_count++;
		return 0;
	}

	/* The item that has the name, which the source replaces. */
	written = snprintf(target, size, "%s/%s", folder, task_base(source));
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;
	error = lstat(target, &status);
	if (error != 0)
		return errno;

	/* An item that holds the source cannot be replaced by it (the source would go with it). */
	inside = task_inside(source, target);
	if (inside != 0)
		return EINVAL;

	/*
	 * It goes to its trash first (its volume's, ws127-p003), where undo
	 * finds it again; without a trash it is removed.  The source is
	 * planned again after it.
	 */
	error = fm_trash_for(target, trash, sizeof(trash));
	if (error == 0) {
		error = task_plan_trash(task, trash, target, &status, trashed, sizeof(trashed));
		if (error != 0)
			return error;

		/* Where the replaced item waits, for undo. */
		task->replaced[owner] = strdup(trashed);
	} else {
		error = task_plan_delete(task, target, &status);
		if (error != 0)
			return error;
	}

	/*
	 * The next planning of this source is its own: replacing tells
	 * task_resolve to give it the name just freed.
	 */
	task->replacing = 1;
	*outcome = TASK_TARGET_REMOVING;

	/* Succeeded: the replaced item's removal is planned. */
	return 0;
}

/* Plans the next entry of the folder being walked, or finishes the folder; nonzero on failure. */
static int
task_plan_walk(
	struct fm_task *task)
{
	struct fm_walk *walk;
	struct dirent *item;
	struct stat status;
	char *source;
	char *target;
	int error;
	int dot;

	/* The folder's entries are the folder's source's steps. */
	walk = &task->walks[task->walk_count - 1U];
	task->owner = walk->owner;

	/* The folder, opened the first time it is walked. */
	if (walk->directory == NULL) {
		walk->directory = opendir(walk->source);
		if (walk->directory == NULL) {
			task_fail(task, walk->owner, errno, walk->source);
			error = task_walk_finish(task, -1);
			return error;
		}
	}

	/* The next entry but . and .., or the end of the folder. */
	for (;;) {
		item = readdir(walk->directory);
		if (item == NULL) {
			error = task_walk_finish(task, 0);
			return error;
		}

		/* The folder itself and its parent are not entries. */
		dot = 0;
		if (item->d_name[0] == '.' && item->d_name[1] == '\0')
			dot = 1;
		if (item->d_name[0] == '.' && item->d_name[1] == '.' && item->d_name[2] == '\0')
			dot = 1;
		if (dot == 0)
			break;
	}

	/* The entry's paths. */
	source = task_join(walk->source, item->d_name);
	if (source == NULL)
		return ENOMEM;
	target = NULL;
	if (walk->target != NULL) {
		target = task_join(walk->target, item->d_name);
		if (target == NULL) {
			free(source);
			return ENOMEM;
		}
	}

	/* What the entry is, and its steps (a folder's contents are walked next). */
	error = lstat(source, &status);
	if (error != 0) {
		task_fail(task, walk->owner, errno, source);
	} else if (walk->post_order != 0) {
		error = task_plan_delete(task, source, &status);
	} else {
		error = task_plan_copy(task, source, target, &status);
	}

	/* The paths are not needed any more. */
	free(source);
	free(target);

	/* Reports a failed entry. */
	if (error != 0)
		return error;

	/* Succeeded: the entry is planned. */
	return 0;
}

/* Closes the folder at the top of the walk stack; a folder being removed is removed after its contents. */
static int
task_walk_finish(
	struct fm_task *task,
	int error)
{
	struct fm_walk *walk;
	struct fm_step *step;

	/* The folder is closed. */
	walk = &task->walks[task->walk_count - 1U];
	if (walk->directory != NULL)
		closedir(walk->directory);
	walk->directory = NULL;

	/* A folder whose contents were removed is removed itself. */
	if (walk->post_order != 0 && error == 0) {
		step = task_add(task, FM_STEP_RMDIR, walk->source, NULL);
		if (step == NULL)
			error = ENOMEM;
	}

	/* The walk goes off the stack. */
	free(walk->source);
	free(walk->target);
	task->walk_count--;

	/* Reports how the folder ended. */
	if (error != 0)
		return error;

	/* Succeeded: the folder is planned. */
	return 0;
}

/*
 * Plans an item's move by copying it and removing the source: for a file
 * the copy and then its removal; for a folder its removal first on the walk
 * stack, under its copy, so that the copy is walked and planned first.
 */
static int
task_plan_transfer(
	struct fm_task *task,
	const char *source,
	const char *target,
	const struct stat *status)
{
	int error;
	int folder;

	/* A folder: the removal's walk under the copy's. */
	folder = S_ISDIR(status->st_mode);
	if (folder != 0) {
		error = task_plan_delete(task, source, status);
		if (error != 0)
			return error;
		error = task_plan_copy(task, source, target, status);
		if (error != 0)
			return error;
		return 0;
	}

	/* Anything else: the copy, then the removal. */
	error = task_plan_copy(task, source, target, status);
	if (error != 0)
		return error;
	error = task_plan_delete(task, source, status);
	if (error != 0)
		return error;

	/* Succeeded: the move is planned. */
	return 0;
}

/* Plans the copy of one item (a folder's contents are walked afterwards); nonzero on failure. */
static int
task_plan_copy(
	struct fm_task *task,
	const char *source,
	const char *target,
	const struct stat *status)
{
	struct fm_step *step;
	char link[FM_OPS_PATH_MAX];
	ssize_t length;
	int regular;
	int folder;
	int link_kind;

	/* A folder: made, then its contents walked. */
	folder = S_ISDIR(status->st_mode);
	if (folder != 0) {
		step = task_add(task, FM_STEP_MKDIR, source, target);
		if (step == NULL)
			return ENOMEM;
		step->mode = status->st_mode & 07777;
		return task_push_walk(task, source, target, 0);
	}

	/* A symbolic link: a link to the same text. */
	link_kind = S_ISLNK(status->st_mode);
	if (link_kind != 0) {
		length = readlink(source, link, sizeof(link) - 1U);
		if (length < 0)
			return errno;
		link[length] = '\0';
		step = task_add(task, FM_STEP_SYMLINK, link, target);
		if (step == NULL)
			return ENOMEM;
		return 0;
	}

	/* Anything but a plain file (a device, a pipe) is not copied. */
	regular = S_ISREG(status->st_mode);
	if (regular == 0)
		return ENOTSUP;

	/* A file: its bytes, mode and times. */
	step = task_add(task, FM_STEP_COPY, source, target);
	if (step == NULL)
		return ENOMEM;
	step->size = (uint64_t)status->st_size;
	step->mode = status->st_mode & 07777;
	step->modified = status->st_mtime;
	step->accessed = status->st_atime;
	task->bytes_total += step->size;

	/* Succeeded: the file's copy is planned. */
	return 0;
}

/* Plans the removal of one item (a folder after its contents, which are walked afterwards); nonzero on failure. */
static int
task_plan_delete(
	struct fm_task *task,
	const char *source,
	const struct stat *status)
{
	struct fm_step *step;
	int folder;

	/* A folder: its contents first, walked; the folder when the walk ends. */
	folder = S_ISDIR(status->st_mode);
	if (folder != 0)
		return task_push_walk(task, source, NULL, 1);

	/* Anything else is unlinked. */
	step = task_add(task, FM_STEP_UNLINK, source, NULL);
	if (step == NULL)
		return ENOMEM;

	/* Succeeded: the removal is planned. */
	return 0;
}

/*
 * Plans the move of one item into a trash: the record of where it was,
 * then the item into the trash's files under a free name (a rename, or a
 * copy and a removal across file systems).  The item's path in the trash
 * is written to trashed; nonzero on failure.
 */
static int
task_plan_trash(
	struct fm_task *task,
	const char *trash,
	const char *source,
	const struct stat *status,
	char *trashed,
	size_t size)
{
	struct stat trash_status;
	struct fm_step *step;
	char name[FM_OPS_PATH_MAX];
	char record[2 * FM_OPS_PATH_MAX + 32];
	char files[FM_OPS_PATH_MAX + 8];
	int same_device;
	int error;

	/* A name in the trash nothing has. */
	error = fm_trash_name(trash, task_base(source), name, sizeof(name));
	if (error != 0)
		return error;

	/* The record of where the item was. */
	snprintf(record, sizeof(record), "%s/info/%s.trashinfo", trash, name);
	step = task_add(task, FM_STEP_TRASHINFO, source, record);
	if (step == NULL)
		return ENOMEM;

	/* The item's place among the trash's files. */
	snprintf(trashed, size, "%s/files/%s", trash, name);
	snprintf(files, sizeof(files), "%s/files", trash);

	/* Within one file system a rename, across a copy and a removal. */
	same_device = 0;
	error = stat(files, &trash_status);
	if (error == 0 && trash_status.st_dev == status->st_dev)
		same_device = 1;
	if (same_device != 0) {
		step = task_add(task, FM_STEP_RENAME, source, trashed);
		if (step == NULL)
			return ENOMEM;
	} else {
		error = task_plan_transfer(task, source, trashed, status);
		if (error != 0)
			return error;
	}

	/* Succeeded: the move into the trash is planned. */
	return 0;
}

/* Puts a folder on the walk stack (opened when it is walked); nonzero when memory runs out. */
static int
task_push_walk(
	struct fm_task *task,
	const char *source,
	const char *target,
	int post_order)
{
	struct fm_walk *grown;
	struct fm_walk *walk;
	size_t capacity;

	/* Room for one more folder. */
	if (task->walk_count == task->walk_capacity) {
		capacity = task->walk_capacity + TASK_WALK_GROWTH;
		grown = realloc(task->walks, capacity * sizeof(task->walks[0]));
		if (grown == NULL)
			return ENOMEM;
		task->walks = grown;
		task->walk_capacity = capacity;
	}

	/* The folder, its paths copied, owned by the source being planned. */
	walk = &task->walks[task->walk_count];
	memset(walk, 0, sizeof(*walk));
	walk->post_order = post_order;
	walk->owner = task->owner;
	walk->source = strdup(source);
	if (walk->source == NULL)
		return ENOMEM;
	if (target != NULL) {
		walk->target = strdup(target);
		if (walk->target == NULL) {
			free(walk->source);
			return ENOMEM;
		}
	}

	/* Succeeded: it is walked next. */
	task->walk_count++;
	return 0;
}

/* Adds a step after the others; NULL when memory runs out. */
static struct fm_step *
task_add(
	struct fm_task *task,
	unsigned kind,
	const char *source,
	const char *target)
{
	struct fm_step *grown;
	struct fm_step *step;
	size_t capacity;

	/* Room for one more step. */
	if (task->step_count == task->step_capacity) {
		capacity = task->step_capacity + TASK_GROWTH;
		grown = realloc(task->steps, capacity * sizeof(task->steps[0]));
		if (grown == NULL)
			return NULL;
		task->steps = grown;
		task->step_capacity = capacity;
	}

	/* The step, its paths copied, owned by the source being planned. */
	step = &task->steps[task->step_count];
	memset(step, 0, sizeof(*step));
	step->kind = kind;
	step->owner = task->owner;
	step->source = strdup(source);
	if (step->source == NULL)
		return NULL;
	if (target != NULL) {
		step->target = strdup(target);
		if (step->target == NULL) {
			free(step->source);
			return NULL;
		}
	}

	/* Every step but the making and removing of folders and records counts as one item. */
	if (kind != FM_STEP_MKDIR &&
	    kind != FM_STEP_RMDIR &&
	    kind != FM_STEP_RMDIR_EMPTY &&
	    kind != FM_STEP_TRASHINFO &&
	    kind != FM_STEP_UNTRASHINFO)
		task->files_total++;

	/* Succeeded: the step is planned. */
	task->step_count++;
	return step;
}

/* Carries out steps until they are all done (0) or the deadline passes (1). */
static int
task_run(
	struct fm_task *task,
	uint64_t deadline)
{
	struct fm_step *step;
	uint64_t now;
	int more;

	/* Each step in order. */
	while (task->step_index < task->step_count) {
		step = &task->steps[task->step_index];

		/* A step not finished in this slice goes on in the next. */
		more = task_run_step(task, step, deadline);
		if (more != 0)
			return 1;
		task->step_index++;

		/* The time is up: the rest in the next slice. */
		now = fm_ops_clock();
		if (now >= deadline)
			return task->step_index < task->step_count;
	}

	/* Every step is done. */
	return 0;
}

/* Carries out one step; returns 1 when a copy goes on in the next slice. */
static int
task_run_step(
	struct fm_task *task,
	struct fm_step *step,
	uint64_t deadline)
{
	const char *failed_path;
	int status;

	/* The removal of a source whose copy failed is skipped: it would lose the only copy. */
	if ((step->kind == FM_STEP_UNLINK || step->kind == FM_STEP_RMDIR) &&
	    task->kind != FM_TASK_DELETE &&
	    task->failed[step->owner] != 0)
		return 0;

	/* Each kind of step. */
	status = 0;
	switch (step->kind) {
	case FM_STEP_MKDIR:
		status = mkdir(step->target, step->mode | 0700);
		break;
	case FM_STEP_COPY:
		return task_copy_chunk(task, step, deadline);
	case FM_STEP_SYMLINK:
		status = symlink(step->source, step->target);
		break;
	case FM_STEP_RENAME:
		status = rename(step->source, step->target);
		break;
	case FM_STEP_UNLINK:
		status = unlink(step->source);
		break;
	case FM_STEP_RMDIR:
		status = rmdir(step->source);
		break;
	case FM_STEP_TRASHINFO:
		status = fm_trash_info_write(step->target, step->source, time(NULL));
		if (status != 0) {
			errno = status;
			status = -1;
		}

		/* The record is written. */
		break;
	case FM_STEP_UNTRASHINFO:
		status = unlink(step->source);
		break;
	case FM_STEP_RMDIR_EMPTY:
		/* A merged folder something stayed in (skipped, or failed to move) stays too; that is no failure. */
		(void)rmdir(step->source);
		return 0;
	default:
		break;
	}

	/* A failed step is counted against its source. */
	if (status != 0) {
		failed_path = step->source;
		if (step->target != NULL)
			failed_path = step->target;
		task_fail(task, step->owner, errno, failed_path);
		return 0;
	}

	/* Succeeded: one more item is done (folders and records are not counted). */
	if (step->kind != FM_STEP_MKDIR &&
	    step->kind != FM_STEP_RMDIR &&
	    step->kind != FM_STEP_TRASHINFO &&
	    step->kind != FM_STEP_UNTRASHINFO)
		task->files_done++;
	return 0;
}

/* Copies a file a chunk at a time until it is done (0) or the deadline passes (1). */
static int
task_copy_chunk(
	struct fm_task *task,
	struct fm_step *step,
	uint64_t deadline)
{
	struct timespec times[2];
	uint64_t now;
	char *buffer;
	ssize_t got;
	ssize_t put;
	size_t done;
	int status;

	/* The files, opened when the copy starts (the target must not exist). */
	if (task->copy_in < 0) {
		task->copy_in = open(step->source, O_RDONLY);
		if (task->copy_in < 0) {
			task_fail(task, step->owner, errno, step->source);
			return 0;
		}

		/* The target, which must not exist yet. */
		task->copy_out = open(step->target, O_WRONLY | O_CREAT | O_EXCL, 0600);
		if (task->copy_out < 0) {
			task_fail(task, step->owner, errno, step->target);
			close(task->copy_in);
			task->copy_in = -1;
			return 0;
		}

		/* Nothing is copied yet. */
		task->copy_done = 0;
	}

	/* A buffer for the chunks. */
	buffer = malloc(FM_TASK_CHUNK);
	if (buffer == NULL) {
		task_fail(task, step->owner, ENOMEM, step->target);
		task_copy_close(task, 0);
		return 0;
	}

	/* Chunks until the end of the file or of the slice. */
	for (;;) {
		got = read(task->copy_in, buffer, FM_TASK_CHUNK);
		if (got < 0) {
			task_fail(task, step->owner, errno, step->source);
			free(buffer);
			task_copy_close(task, 0);
			return 0;
		}

		/* The end of the file ends the copy. */
		if (got == 0)
			break;

		/* The chunk written in full. */
		done = 0;
		while (done < (size_t)got) {
			put = write(task->copy_out, buffer + done, (size_t)got - done);
			if (put <= 0) {
				task_fail(task, step->owner, errno, step->target);
				free(buffer);
				task_copy_close(task, 0);
				return 0;
			}

			/* On to the rest of the chunk. */
			done += (size_t)put;
		}

		/* The chunk counts. */
		task->copy_done += (uint64_t)got;
		task->bytes_done += (uint64_t)got;

		/* The slice is up: the rest of the file in the next. */
		now = fm_ops_clock();
		if (now >= deadline) {
			free(buffer);
			return 1;
		}
	}

	/* The buffer is not needed any more. */
	free(buffer);

	/* The copy gets the source's mode, times and extended attributes. */
	status = fchmod(task->copy_out, step->mode);
	(void)status;
	task_copy_xattrs(task->copy_in, task->copy_out);
	times[0].tv_sec = step->accessed;
	times[0].tv_nsec = 0;
	times[1].tv_sec = step->modified;
	times[1].tv_nsec = 0;
	status = futimens(task->copy_out, times);
	(void)status;

	/* Succeeded: the file is copied and closed. */
	task_copy_close(task, 1);
	task->files_done++;
	return 0;
}

/* Copies the extended attributes of one open file to another (those that cannot be set are left out). */
static void
task_copy_xattrs(
	int in,
	int out)
{
	char *names;
	char *value;
	ssize_t length;
	ssize_t size;
	size_t at;
	int status;

	/* The names of the source's attributes. */
	names = malloc(TASK_XATTR_NAMES);
	if (names == NULL)
		return;
	length = flistxattr(in, names, TASK_XATTR_NAMES);
	if (length <= 0) {
		free(names);
		return;
	}

	/* A buffer for one value at a time. */
	value = malloc(TASK_XATTR_VALUE);
	if (value == NULL) {
		free(names);
		return;
	}

	/* Each name (they are NUL-separated) and its value. */
	for (at = 0; at < (size_t)length; at += strlen(names + at) + 1U) {
		size = fgetxattr(in, names + at, value, TASK_XATTR_VALUE);
		if (size < 0)
			continue;
		status = fsetxattr(out, names + at, value, (size_t)size, 0);
		(void)status;
	}

	/* The buffers go. */
	free(value);
	free(names);
}

/* Closes the files of a copy; a copy not kept (failed or cancelled) is removed. */
static void
task_copy_close(
	struct fm_task *task,
	int keep)
{
	struct fm_step *step;

	/* No copy is open. */
	if (task->copy_in < 0)
		return;

	/* Both files closed. */
	close(task->copy_in);
	task->copy_in = -1;
	if (task->copy_out >= 0)
		close(task->copy_out);
	task->copy_out = -1;

	/* A half copy is not left behind. */
	if (keep == 0 && task->step_index < task->step_count) {
		step = &task->steps[task->step_index];
		if (step->kind == FM_STEP_COPY && step->target != NULL)
			(void)unlink(step->target);
	}
}

/* Records a failure of a source: the first one's reason and path are kept for the message. */
static void
task_fail(
	struct fm_task *task,
	size_t owner,
	int error,
	const char *path)
{
	/* The source failed (its removal will be skipped). */
	if (owner < task->source_count)
		task->failed[owner] = 1;
	task->error_count++;

	/* The first failure is the one told. */
	if (task->error != 0)
		return;
	task->error = error;
	if (task->error == 0)
		task->error = EIO;
	snprintf(task->error_path, sizeof(task->error_path), "%s", path);
}

/* Joins a folder and a name into an allocated path. */
static char *
task_join(
	const char *folder,
	const char *name)
{
	size_t length;
	char *path;
	int root;

	/* The folder, a slash (unless the folder is the root) and the name. */
	length = strlen(folder) + strlen(name) + 2U;
	path = malloc(length);
	if (path == NULL)
		return NULL;
	root = strcmp(folder, "/");
	if (root == 0)
		snprintf(path, length, "/%s", name);
	else
		snprintf(path, length, "%s/%s", folder, name);

	/* Reports the joined path. */
	return path;
}

/* Returns the last part of a path. */
static const char *
task_base(
	const char *path)
{
	const char *slash;

	/* After the last slash. */
	slash = strrchr(path, '/');
	if (slash == NULL)
		return path;

	/* Reports the part after it. */
	return slash + 1;
}

/* Writes the folder a path is in ("/" for the root's items). */
static void
task_parent(
	const char *path,
	char *parent,
	size_t size)
{
	const char *slash;
	size_t length;

	/* Up to the last slash. */
	slash = strrchr(path, '/');
	if (slash == NULL || slash == path) {
		snprintf(parent, size, "/");
		return;
	}

	/* The path up to it. */
	length = (size_t)(slash - path);
	if (length >= size)
		length = size - 1U;

	/* Reports that part. */
	memcpy(parent, path, length);
	parent[length] = '\0';
}

/* Tells whether a path is a folder or inside it. */
static int
task_inside(
	const char *path,
	const char *folder)
{
	size_t length;
	int match;

	/* The folder must start the path. */
	length = strlen(folder);
	match = strncmp(path, folder, length);
	if (match != 0)
		return 0;

	/* And end there or at a slash. */
	if (path[length] == '\0' || path[length] == '/')
		return 1;

	/* A longer name that only starts the same. */
	return 0;
}

/* Makes a folder and the folders above it that are missing (mkdir -p); nonzero when it cannot. */
static int
task_mkdir_parents(
	const char *path)
{
	char partial[FM_OPS_PATH_MAX];
	size_t index;
	int status;

	/* Each prefix that ends at a slash, and the whole path. */
	snprintf(partial, sizeof(partial), "%s", path);
	for (index = 1; partial[index] != '\0'; index++) {
		if (partial[index] != '/')
			continue;
		partial[index] = '\0';
		(void)mkdir(partial, 0755);
		partial[index] = '/';
	}

	/* The whole path, which must be a folder now. */
	status = mkdir(partial, 0755);
	if (status != 0 && errno != EEXIST)
		return errno;

	/* Succeeded: the folder is there. */
	return 0;
}

/*
 * Grows the task's tables of sources (and their results, failures,
 * answers, replaced items and folders) to hold count sources and a
 * terminator; the entries already there stay.  Returns 0, or ENOMEM with
 * the tables as large as they could be made and their contents unchanged.
 */
static int
task_grow(
	struct fm_task *task,
	size_t count)
{
	char **paths;
	unsigned char *flags;
	size_t size;

	/* One entry more than the sources, for the terminator. */
	size = count + 1U;

	/* The sources. */
	paths = realloc(task->sources, size * sizeof(task->sources[0]));
	if (paths == NULL)
		return ENOMEM;
	task->sources = paths;

	/* Where each went. */
	paths = realloc(task->results, size * sizeof(task->results[0]));
	if (paths == NULL)
		return ENOMEM;
	task->results = paths;

	/* What each replaced. */
	paths = realloc(task->replaced, size * sizeof(task->replaced[0]));
	if (paths == NULL)
		return ENOMEM;
	task->replaced = paths;

	/* The folder each goes into. */
	paths = realloc(task->folders, size * sizeof(task->folders[0]));
	if (paths == NULL)
		return ENOMEM;
	task->folders = paths;

	/* Which failed. */
	flags = realloc(task->failed, size);
	if (flags == NULL)
		return ENOMEM;
	task->failed = flags;

	/* What each does with a taken name. */
	flags = realloc(task->collisions, size);
	if (flags == NULL)
		return ENOMEM;
	task->collisions = flags;

	/* Succeeded: every table holds count sources. */
	return 0;
}

/*
 * Reads the names of the items a folder holds (not . and ..), sorted, into
 * an allocated table.  Returns 0 with the table and its length, or an
 * errno value with nothing allocated.
 */
static int
task_read_names(
	const char *folder,
	char ***names,
	size_t *count)
{
	DIR *directory;
	struct dirent *entry;
	char **table;
	char **grown;
	size_t capacity;
	size_t length;
	int dot;
	int error;

	/* Nothing read yet. */
	*names = NULL;
	*count = 0U;

	/* The folder. */
	directory = opendir(folder);
	if (directory == NULL)
		return errno;

	/* Each entry but the folder itself and its parent, the table grown as it fills. */
	table = NULL;
	capacity = 0U;
	length = 0U;
	error = 0;
	for (;;) {
		entry = readdir(directory);
		if (entry == NULL)
			break;

		/* The folder itself and its parent are not items. */
		dot = 0;
		if (entry->d_name[0] == '.' && entry->d_name[1] == '\0')
			dot = 1;
		if (entry->d_name[0] == '.' && entry->d_name[1] == '.' && entry->d_name[2] == '\0')
			dot = 1;
		if (dot != 0)
			continue;

		/* Room for one more name and the terminator. */
		if (length + 1U >= capacity) {
			capacity += TASK_WALK_GROWTH;
			grown = realloc(table, capacity * sizeof(table[0]));
			if (grown == NULL) {
				error = ENOMEM;
				break;
			}

			/* The grown table. */
			table = grown;
		}

		/* The name, copied; one more is kept. */
		table[length] = strdup(entry->d_name);
		if (table[length] == NULL) {
			error = ENOMEM;
			break;
		}

		/* The table holds one more name. */
		length++;
	}

	/* The folder is closed however the reading ended. */
	closedir(directory);

	/* A failure leaves nothing behind. */
	if (error != 0) {
		task_free_names(table, length);
		return error;
	}

	/* The names in order, so that the items are asked about as the folder lists them. */
	if (length > 1U)
		qsort(table, length, sizeof(table[0]), task_compare_names);

	/* Succeeded: the table and its length. */
	*names = table;
	*count = length;
	return 0;
}

/* Frees a table of names and the names in it. */
static void
task_free_names(
	char **names,
	size_t count)
{
	size_t index;

	/* No table, nothing to free. */
	if (names == NULL)
		return;

	/* Each name, then the table. */
	for (index = 0; index < count; index++)
		free(names[index]);
	free(names);
}

/* Orders two names of a table (for qsort) by their bytes. */
static int
task_compare_names(
	const void *left,
	const void *right)
{
	const char *const *left_name;
	const char *const *right_name;
	int order;

	/* The table's entries are pointers to the names. */
	left_name = left;
	right_name = right;
	order = strcmp(*left_name, *right_name);

	/* Reports the order. */
	return order;
}

/*
 * Plans the removal of the folders a move merged into others, once their
 * contents have moved (ws035-p115): the deepest first, and each only when
 * it is empty then (an item left behind keeps its folder).
 */
static int
task_plan_merged(
	struct fm_task *task)
{
	struct fm_step *step;
	size_t index;

	/* The steps belong to no source. */
	task->owner = task->source_count;

	/* The latest merged first: a folder merged inside another was merged after it. */
	for (index = task->merged_count; index > 0U; index--) {
		step = task_add(task, FM_STEP_RMDIR_EMPTY, task->merged[index - 1U], NULL);
		if (step == NULL)
			return ENOMEM;
	}

	/* Succeeded: the removals are planned. */
	task->merged_planned = 1;
	return 0;
}

/*
 * Builds the tables of a merged folder's items (ws035-p115): each item's
 * path in the source folder, and a copy of the folder it merges into for
 * each.  Returns 0 with both tables, or ENOMEM with neither.
 */
static int
task_merge_tables(
	const char *source,
	const char *target,
	char *const *names,
	size_t count,
	char ***items,
	char ***folders)
{
	char **paths;
	char **copies;
	size_t index;

	/* Nothing is given back until both are whole. */
	*items = NULL;
	*folders = NULL;

	/* The table of paths. */
	paths = calloc(count + 1U, sizeof(paths[0]));
	if (paths == NULL)
		return ENOMEM;

	/* The table of folders. */
	copies = calloc(count + 1U, sizeof(copies[0]));
	if (copies == NULL) {
		free(paths);
		return ENOMEM;
	}

	/* Each item's path and folder; a failure frees what was made. */
	for (index = 0; index < count; index++) {
		/* The item's path in the source. */
		paths[index] = task_join(source, names[index]);
		if (paths[index] == NULL) {
			task_free_names(paths, count);
			task_free_names(copies, count);
			return ENOMEM;
		}

		/* The folder it goes into. */
		copies[index] = strdup(target);
		if (copies[index] == NULL) {
			task_free_names(paths, count);
			task_free_names(copies, count);
			return ENOMEM;
		}
	}

	/* Succeeded: both tables. */
	*items = paths;
	*folders = copies;
	return 0;
}
