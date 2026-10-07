/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws071: checks files' model on the host in a temporary folder:
 * the tasks (copy, duplicate, move within and across file systems, trash
 * and put back, delete), the names taken (skip, replace, merge; replace
 * with the trash on the other file system), the free names, the trash's
 * records, the undo history and the clipboard.
 *
 *   files-model TEMPORARY-FOLDER [OTHER-FILE-SYSTEM-FOLDER]
 *
 * Prints one line a check and "files-model: PASS" or "FAIL" at the end.
 */

#include "files.h"

#include <keiland/keiland.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

static int failures;

static void check(int condition, const char *what);
static void make_file(const char *path, const char *text);
static int file_is(const char *path, const char *text);
static int exists(const char *path);
static int run(struct fm_task *task);
static void make_elf(const char *path, const char *library);

int
main(
	int argc,
	char **argv)
{
	char root[FM_PATH_MAX];
	char path[2 * FM_PATH_MAX];
	char other[2 * FM_PATH_MAX];
	char trash[FM_PATH_MAX];
	char original[FM_PATH_MAX];
	char value[64];
	char *sources[4];
	char *targets[4];
	char *sources_merge[2];
	char *volume_items[2];
	char text[512];
	int descriptor;
	char **paths;
	struct fm_task *task;
	struct fm_task *folder_task;
	struct fm_undo history;
	struct fm_undo_item item;
	struct fm_listing listing;
	unsigned mode;
	size_t count;
	size_t index;
	ssize_t length;
	int shown;
	time_t deleted;
	int error;

	if (argc < 2) {
		fprintf(stderr, "usage: files-model TEMPORARY-FOLDER [OTHER-FILE-SYSTEM-FOLDER [VOLUME-FOLDER]]\n");
		return 2;
	}
	mkdir(argv[1], 0755);
	if (realpath(argv[1], root) == NULL)
		return 2;
	snprintf(path, sizeof(path), "%s/home", root);
	mkdir(path, 0755);
	setenv("HOME", path, 1);
	snprintf(path, sizeof(path), "%s/data", root);
	setenv("XDG_DATA_HOME", path, 1);
	snprintf(path, sizeof(path), "%s/run", root);
	mkdir(path, 0700);
	setenv("XDG_RUNTIME_DIR", path, 1);

	/* A source tree: a file, a folder with a file, a nested folder and a link, an extended attribute on a file. */
	snprintf(path, sizeof(path), "%s/src", root);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/src/Report.pdf", root);
	make_file(path, "report");
	error = setxattr(path, "user.keiland.tags", "Work", 4, 0);
	check(error == 0, "xattr set on the source (the host's file system has user xattrs)");
	snprintf(path, sizeof(path), "%s/src/Folder.v1", root);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/src/Folder.v1/inner.txt", root);
	make_file(path, "inner");
	snprintf(path, sizeof(path), "%s/src/Folder.v1/deep", root);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/src/Folder.v1/deep/leaf.txt", root);
	make_file(path, "leaf");
	snprintf(path, sizeof(path), "%s/src/Folder.v1/link", root);
	symlink("inner.txt", path);
	snprintf(path, sizeof(path), "%s/dst", root);
	mkdir(path, 0755);

	/* 1. Copy a file and a folder; the copy keeps contents, the link and the extended attribute. */
	snprintf(path, sizeof(path), "%s/src/Report.pdf", root);
	sources[0] = strdup(path);
	snprintf(path, sizeof(path), "%s/src/Folder.v1", root);
	sources[1] = strdup(path);
	snprintf(path, sizeof(path), "%s/dst", root);
	task = fm_task_new(FM_TASK_COPY, sources, 2, path);
	check(run(task) == 0 && task->error_count == 0, "copy: no error");
	check(task->files_done == 4, "copy: four items (the top file, two nested files, a link)");
	snprintf(path, sizeof(path), "%s/dst/Report.pdf", root);
	check(file_is(path, "report"), "copy: the file's contents");
	length = getxattr(path, "user.keiland.tags", value, sizeof(value));
	check(length == 4 && memcmp(value, "Work", 4) == 0, "copy: the extended attribute came along");
	snprintf(path, sizeof(path), "%s/dst/Folder.v1/deep/leaf.txt", root);
	check(file_is(path, "leaf"), "copy: a nested file");
	snprintf(path, sizeof(path), "%s/dst/Folder.v1/link", root);
	length = readlink(path, value, sizeof(value) - 1);
	check(length == 9 && memcmp(value, "inner.txt", 9) == 0, "copy: the link is a link");
	fm_task_free(task);

	/* 2. Copying again keeps both: "Report 2.pdf" and "Folder.v1 2" (a folder's dots are its name). */
	snprintf(path, sizeof(path), "%s/dst", root);
	task = fm_task_new(FM_TASK_COPY, sources, 2, path);
	run(task);
	snprintf(path, sizeof(path), "%s/dst/Report 2.pdf", root);
	check(exists(path), "copy again: Report 2.pdf");
	snprintf(path, sizeof(path), "%s/dst/Folder.v1 2/inner.txt", root);
	check(exists(path), "copy again: Folder.v1 2");
	fm_task_free(task);

	/* 2b. Names taken (ws035-p106, F-041): skip leaves the item there, replace puts the source in its place. */
	snprintf(path, sizeof(path), "%s/dst/Report.pdf", root);
	make_file(path, "older");
	snprintf(path, sizeof(path), "%s/dst/Folder.v1/extra.txt", root);
	make_file(path, "extra");
	snprintf(path, sizeof(path), "%s/dst", root);
	task = fm_task_new(FM_TASK_COPY, sources, 2, path);
	check(fm_task_collides(task, 0) == 1 && fm_task_collides(task, 1) == 1, "collision: both names are taken");
	task->collisions[0] = FM_COLLISION_SKIP;
	task->collisions[1] = FM_COLLISION_SKIP;
	run(task);
	snprintf(path, sizeof(path), "%s/dst/Report.pdf", root);
	check(file_is(path, "older") && task->skip_count == 2 && task->error_count == 0, "collision: skip leaves the items there");
	check(task->results[0] == NULL && task->failed[0] == 0, "collision: a skipped source has no result and did not fail");
	snprintf(path, sizeof(path), "%s/dst/Report 3.pdf", root);
	check(!exists(path), "collision: skip makes no new name");
	fm_task_free(task);
	snprintf(path, sizeof(path), "%s/dst", root);
	task = fm_task_new(FM_TASK_COPY, sources, 2, path);
	task->collisions[0] = FM_COLLISION_REPLACE;
	task->collisions[1] = FM_COLLISION_REPLACE;
	check(run(task) == 0 && task->error_count == 0, "collision: replace, no error");
	snprintf(path, sizeof(path), "%s/dst/Report.pdf", root);
	check(file_is(path, "report") && task->results[0] != NULL && strcmp(task->results[0], path) == 0, "collision: replace puts the file in the item's place");
	snprintf(path, sizeof(path), "%s/dst/Folder.v1/extra.txt", root);
	check(!exists(path), "collision: replace removes the folder that was there (its extra file too)");
	snprintf(path, sizeof(path), "%s/dst/Folder.v1/deep/leaf.txt", root);
	check(file_is(path, "leaf"), "collision: replace copies the folder in its place");
	snprintf(path, sizeof(path), "%s/dst/Report 3.pdf", root);
	check(!exists(path), "collision: replace makes no new name");

	/* 2c. The replaced items went to the trash (ws035-p110, F-050), and the undo's two tasks put the file back. */
	check(task->replaced[0] != NULL && file_is(task->replaced[0], "older"), "replace: the replaced file is in the trash");
	snprintf(path, sizeof(path), "%s/extra.txt", task->replaced[1] != NULL ? task->replaced[1] : "-");
	check(task->replaced[1] != NULL && file_is(path, "extra"), "replace: the replaced folder is in the trash with its contents");
	targets[0] = strdup(task->results[0]);
	targets[1] = strdup(task->replaced[0]);
	fm_task_free(task);
	fm_trash_path(trash, sizeof(trash));
	task = fm_task_new(FM_TASK_TRASH, targets, 1, trash);
	run(task);
	fm_task_free(task);
	task = fm_task_new(FM_TASK_RESTORE, targets + 1, 1, trash);
	run(task);
	check(file_is(targets[0], "older") && !exists(targets[1]) && task->error_count == 0, "replace: undo puts the replaced file back under its name");
	fm_task_free(task);
	free(targets[0]);
	free(targets[1]);
	snprintf(path, sizeof(path), "%s/dst/Report.pdf", root);
	make_file(path, "report");
	snprintf(path, sizeof(path), "%s/src/Moved.txt", root);
	make_file(path, "moved");
	targets[0] = strdup(path);
	snprintf(path, sizeof(path), "%s/dst/Moved.txt", root);
	make_file(path, "there");
	snprintf(path, sizeof(path), "%s/dst", root);
	task = fm_task_new(FM_TASK_MOVE, targets, 1, path);
	task->collisions[0] = FM_COLLISION_REPLACE;
	run(task);
	snprintf(path, sizeof(path), "%s/dst/Moved.txt", root);
	check(file_is(path, "moved") && !exists(targets[0]) && task->error_count == 0, "collision: a move replaces the item there");
	fm_task_free(task);
	free(targets[0]);

	/*
	 * 2d. Folders merged (ws035-p115, F-050): a/Photos into b/Photos.  The merge puts the folder's items in its
	 * place, a sub-folder merges again, and the taken file inside is asked about on its own.
	 */
	snprintf(path, sizeof(path), "%s/merge/a/Photos/Sub", root);
	fm_ops_mkdir_parents(path);
	snprintf(path, sizeof(path), "%s/merge/b/Photos/Sub", root);
	fm_ops_mkdir_parents(path);
	snprintf(path, sizeof(path), "%s/merge/a/Photos/one.txt", root);
	make_file(path, "one-new");
	snprintf(path, sizeof(path), "%s/merge/a/Photos/two.txt", root);
	make_file(path, "two");
	snprintf(path, sizeof(path), "%s/merge/a/Photos/Sub/deep.txt", root);
	make_file(path, "deep");
	snprintf(path, sizeof(path), "%s/merge/b/Photos/one.txt", root);
	make_file(path, "one-old");
	snprintf(path, sizeof(path), "%s/merge/b/Photos/three.txt", root);
	make_file(path, "three");
	snprintf(path, sizeof(path), "%s/merge/b/Photos/Sub/keep.txt", root);
	make_file(path, "keep");
	snprintf(path, sizeof(path), "%s/merge/a/Photos", root);
	targets[0] = strdup(path);
	snprintf(path, sizeof(path), "%s/merge/b", root);
	task = fm_task_new(FM_TASK_COPY, targets, 1, path);
	check(fm_task_can_merge(task, 0) == 1, "merge: two folders can be merged");
	check(fm_task_merge(task, 0, &count) == 0 && count == 3 && task->source_count == 3, "merge: the folder's three items take its place");
	snprintf(path, sizeof(path), "%s/merge/b/Photos", root);
	check(strcmp(fm_task_folder(task, 0), path) == 0 && strcmp(fm_task_folder(task, 2), path) == 0, "merge: the items go into the merged folder");
	check(fm_task_can_merge(task, 0) == 1 && fm_task_merge(task, 0, &count) == 0 && count == 1, "merge: the sub-folder merges again");
	check(fm_task_collides(task, 0) == 0 && fm_task_collides(task, 1) == 1 && fm_task_can_merge(task, 1) == 0 && fm_task_collides(task, 2) == 0, "merge: only one.txt is taken inside, and it is a file");
	task->collisions[1] = FM_COLLISION_REPLACE;
	check(run(task) == 0 && task->error_count == 0, "merge: copy, no error");
	snprintf(path, sizeof(path), "%s/merge/b/Photos/one.txt", root);
	check(file_is(path, "one-new"), "merge: the taken file is replaced");
	snprintf(path, sizeof(path), "%s/merge/b/Photos/two.txt", root);
	snprintf(other, sizeof(other), "%s/merge/b/Photos/three.txt", root);
	check(file_is(path, "two") && file_is(other, "three"), "merge: the new file joins the one that was there");
	snprintf(path, sizeof(path), "%s/merge/b/Photos/Sub/deep.txt", root);
	snprintf(other, sizeof(other), "%s/merge/b/Photos/Sub/keep.txt", root);
	check(file_is(path, "deep") && file_is(other, "keep"), "merge: the sub-folders' contents are combined");
	snprintf(path, sizeof(path), "%s/merge/b/Photos 2", root);
	snprintf(other, sizeof(other), "%s/merge/b/Photos/Sub 2", root);
	check(!exists(path) && !exists(other), "merge: no second folder is made");
	snprintf(path, sizeof(path), "%s/merge/a/Photos/Sub/deep.txt", root);
	check(file_is(path, "deep"), "merge: a copy leaves the source");
	check(task->replaced[1] != NULL && file_is(task->replaced[1], "one-old"), "merge: the replaced file is in the trash");

	/* Undo of the merged copy: the copies to the trash, the replaced file back. */
	paths = calloc(4, sizeof(char *));
	for (count = 0; count < 3; count++)
		paths[count] = strdup(task->results[count]);
	targets[1] = strdup(task->replaced[1]);
	fm_task_free(task);
	fm_trash_path(trash, sizeof(trash));
	task = fm_task_new(FM_TASK_TRASH, paths, 3, trash);
	run(task);
	fm_task_free(task);
	task = fm_task_new(FM_TASK_RESTORE, targets + 1, 1, trash);
	run(task);
	fm_task_free(task);
	snprintf(path, sizeof(path), "%s/merge/b/Photos/one.txt", root);
	snprintf(other, sizeof(other), "%s/merge/b/Photos/two.txt", root);
	check(file_is(path, "one-old") && !exists(other), "merge: undo puts back what the folder held");
	snprintf(path, sizeof(path), "%s/merge/b/Photos/Sub/deep.txt", root);
	snprintf(other, sizeof(other), "%s/merge/b/Photos/Sub/keep.txt", root);
	check(!exists(path) && file_is(other, "keep"), "merge: undo keeps the merged sub-folder as it was");
	fm_paths_free(paths, 3);
	free(targets[1]);

	/* A merged move: one.txt skipped; the emptied sub-folder goes, the folder with one.txt left in it stays. */
	snprintf(path, sizeof(path), "%s/merge/b", root);
	task = fm_task_new(FM_TASK_MOVE, targets, 1, path);
	fm_task_merge(task, 0, &count);
	fm_task_merge(task, 0, &count);
	task->collisions[1] = FM_COLLISION_SKIP;
	check(run(task) == 0 && task->error_count == 0 && task->merged_count == 2, "merge: move, no error");
	snprintf(path, sizeof(path), "%s/merge/b/Photos/two.txt", root);
	snprintf(other, sizeof(other), "%s/merge/a/Photos/two.txt", root);
	check(file_is(path, "two") && !exists(other), "merge: a move moves the items");
	snprintf(path, sizeof(path), "%s/merge/a/Photos/Sub", root);
	snprintf(other, sizeof(other), "%s/merge/a/Photos/one.txt", root);
	check(!exists(path) && file_is(other, "one-new"), "merge: the emptied folder goes, the skipped item stays");

	/* Undo of the merged move, as actions.c does it: each item renamed back, its folder made again. */
	for (count = 0; count < task->source_count; count++) {
		if (task->results[count] == NULL)
			continue;
		snprintf(path, sizeof(path), "%s", task->sources[count]);
		*strrchr(path, '/') = '\0';
		fm_ops_mkdir_parents(path);
		rename(task->results[count], task->sources[count]);
	}
	snprintf(path, sizeof(path), "%s/merge/a/Photos/Sub/deep.txt", root);
	snprintf(other, sizeof(other), "%s/merge/b/Photos/Sub/deep.txt", root);
	check(file_is(path, "deep") && !exists(other), "merge: undo of the move puts the items back in their folders");

	/* A redo's pairs: each item into the folder it went to the first time. */
	sources_merge[0] = task->sources[0];
	sources_merge[1] = task->sources[2];
	snprintf(path, sizeof(path), "%s/merge/b/Photos/Sub", root);
	folder_task = fm_task_new(FM_TASK_COPY, sources_merge, 2, path);
	snprintf(path, sizeof(path), "%s/merge/b/Photos", root);
	check(fm_task_set_folder(folder_task, 1, path) == 0, "merge: redo gives an item its own folder");
	run(folder_task);
	snprintf(path, sizeof(path), "%s/merge/b/Photos/Sub/deep.txt", root);
	snprintf(other, sizeof(other), "%s/merge/b/Photos/two.txt", root);
	check(file_is(path, "deep") && file_is(other, "two") && folder_task->error_count == 0, "merge: a redo puts each item where it went");
	fm_task_free(folder_task);
	fm_task_free(task);
	free(targets[0]);

	/* 2e. Replace with the trash on another file system (ws035-p115): the replaced items are copied there and back. */
	if (argc > 2) {
		snprintf(path, sizeof(path), "%s/data-other", argv[2]);
		setenv("XDG_DATA_HOME", path, 1);
		snprintf(path, sizeof(path), "%s/xfs/src/Dir", root);
		fm_ops_mkdir_parents(path);
		snprintf(path, sizeof(path), "%s/xfs/dst/Dir", root);
		fm_ops_mkdir_parents(path);
		snprintf(path, sizeof(path), "%s/xfs/src/Old.txt", root);
		make_file(path, "new");
		targets[0] = strdup(path);
		snprintf(path, sizeof(path), "%s/xfs/src/Dir/y.txt", root);
		make_file(path, "y");
		snprintf(path, sizeof(path), "%s/xfs/src/Dir", root);
		targets[1] = strdup(path);
		snprintf(path, sizeof(path), "%s/xfs/dst/Old.txt", root);
		make_file(path, "old");
		snprintf(path, sizeof(path), "%s/xfs/dst/Dir/x.txt", root);
		make_file(path, "x");
		snprintf(path, sizeof(path), "%s/xfs/dst", root);
		task = fm_task_new(FM_TASK_COPY, targets, 2, path);
		task->collisions[0] = FM_COLLISION_REPLACE;
		task->collisions[1] = FM_COLLISION_REPLACE;
		check(run(task) == 0 && task->error_count == 0, "replace across file systems: no error");
		/* The replaced items go to the trash of their own volume (ws127-p003), renamed there rather than copied home. */
		snprintf(other, sizeof(other), "%s/xfs/dst/Old.txt", root);
		error = fm_trash_for(other, trash, sizeof(trash));
		snprintf(path, sizeof(path), "%s/files/", trash);
		check(task->replaced[0] != NULL && strncmp(task->replaced[0], path, strlen(path)) == 0 && file_is(task->replaced[0], "old"), "replace across file systems: the file is in the trash fm_trash_for chose (the home trash for /dev/shm)");
		snprintf(other, sizeof(other), "%s/x.txt", task->replaced[1] != NULL ? task->replaced[1] : "-");
		check(file_is(other, "x"), "replace across file systems: the folder is in that trash with its contents");
		snprintf(path, sizeof(path), "%s/xfs/dst/Old.txt", root);
		snprintf(other, sizeof(other), "%s/xfs/dst/Dir/x.txt", root);
		check(file_is(path, "new") && !exists(other), "replace across file systems: the sources took their places");
		paths = calloc(3, sizeof(char *));
		paths[0] = strdup(task->results[0]);
		paths[1] = strdup(task->results[1]);
		free(targets[0]);
		free(targets[1]);
		targets[0] = strdup(task->replaced[0]);
		targets[1] = strdup(task->replaced[1]);
		fm_task_free(task);
		fm_trash_path(trash, sizeof(trash));
		task = fm_task_new(FM_TASK_TRASH, paths, 2, trash);
		run(task);
		/* The copies trashed here are removed with their records, so no volume's trash keeps them (ws127-p003). */
		for (index = 0; index < 2; index++) {
			if (task->results[index] == NULL || fm_trash_of(task->results[index], original, sizeof(original)) != 0)
				continue;
			snprintf(path, sizeof(path), "%s/info/%s.trashinfo", original, strrchr(task->results[index], '/') + 1);
			unlink(path);
		}
		folder_task = fm_task_new(FM_TASK_DELETE, task->results, 2, NULL);
		run(folder_task);
		fm_task_free(folder_task);
		fm_task_free(task);
		task = fm_task_new(FM_TASK_RESTORE, targets, 2, trash);
		run(task);
		snprintf(path, sizeof(path), "%s/xfs/dst/Old.txt", root);
		snprintf(other, sizeof(other), "%s/xfs/dst/Dir/x.txt", root);
		check(file_is(path, "old") && file_is(other, "x") && task->error_count == 0, "replace across file systems: undo copies them back");
		check(!exists(targets[0]) && !exists(targets[1]), "replace across file systems: undo leaves nothing of them in the trash");
		fm_task_free(task);
		fm_paths_free(paths, 2);
		free(targets[0]);
		free(targets[1]);
		snprintf(path, sizeof(path), "%s/data", root);
		setenv("XDG_DATA_HOME", path, 1);
		fm_trash_path(trash, sizeof(trash));
	} else {
		printf("skip: replace across file systems (no second folder)\n");
	}

	/* 3. Duplicate beside itself. */
	snprintf(path, sizeof(path), "%s/src", root);
	task = fm_task_new(FM_TASK_DUPLICATE, sources, 1, path);
	run(task);
	snprintf(path, sizeof(path), "%s/src/Report copy.pdf", root);
	check(exists(path), "duplicate: Report copy.pdf");
	check(task->results[0] != NULL && strcmp(task->results[0], path) == 0, "duplicate: the result names the copy");
	fm_task_free(task);

	/* 4. A folder cannot be copied into itself. */
	snprintf(path, sizeof(path), "%s/src/Folder.v1/deep", root);
	task = fm_task_new(FM_TASK_COPY, sources + 1, 1, path);
	run(task);
	check(task->error == EINVAL && task->failed[0] != 0, "copy into itself: refused");
	fm_task_free(task);

	/* 5. Move within the file system: a rename. */
	snprintf(path, sizeof(path), "%s/dst/Report 2.pdf", root);
	targets[0] = strdup(path);
	snprintf(path, sizeof(path), "%s/src", root);
	task = fm_task_new(FM_TASK_MOVE, targets, 1, path);
	run(task);
	snprintf(path, sizeof(path), "%s/src/Report 2.pdf", root);
	check(exists(path) && !exists(targets[0]), "move: renamed into the folder");
	check(task->step_count == 1 && task->steps[0].kind == FM_STEP_RENAME, "move: one rename step");
	fm_task_free(task);
	free(targets[0]);

	/* 6. To the trash and back. */
	error = fm_trash_path(trash, sizeof(trash));
	check(error == 0, "trash: the trash's folders are made");
	snprintf(path, sizeof(path), "%s/src/Report 2.pdf", root);
	targets[0] = strdup(path);
	task = fm_task_new(FM_TASK_TRASH, targets, 1, trash);
	run(task);
	snprintf(path, sizeof(path), "%s/files/Report 2.pdf", trash);
	check(exists(path) && !exists(targets[0]), "trash: the file is in the trash");
	error = fm_trash_info_read(trash, "Report 2.pdf", original, sizeof(original), &deleted);
	check(error == 0 && strcmp(original, targets[0]) == 0 && deleted != 0, "trash: the record says where and when");
	snprintf(other, sizeof(other), "%s/info/Report 2.pdf.trashinfo", trash);
	check(exists(other), "trash: the record file");
	fm_task_free(task);

	/* 7. A second file of the same name gets "name.2" in the trash. */
	make_file(targets[0], "second");
	task = fm_task_new(FM_TASK_TRASH, targets, 1, trash);
	run(task);
	snprintf(path, sizeof(path), "%s/files/Report 2.pdf.2", trash);
	check(exists(path), "trash: the second of one name is name.2");
	fm_task_free(task);

	/* 7b. The trash's listing shows both under the name they had, of its kind (BUG-140, ws127-p002). */
	memset(&listing, 0, sizeof(listing));
	error = fm_dir_read_trash(&listing);
	shown = 0;
	for (index = 0; error == 0 && index < listing.count; index++) {
		if (strcmp(listing.entries[index].name, "Report 2.pdf.2") == 0)
			shown = -100;
		if (strcmp(listing.entries[index].name, "Report 2.pdf") != 0)
			continue;
		if (listing.entries[index].mime == NULL || listing.entries[index].mime != fm_mime_guess("Report 2.pdf", listing.entries[index].mode))
			continue;
		shown++;
	}
	check(error == 0 && shown == 2, "trash listing: name.2 shows as the name it had, of that kind");
	fm_dir_free(&listing);

	/* 8. Put back: the first one returns to where it was, its record goes. */
	snprintf(path, sizeof(path), "%s/files/Report 2.pdf", trash);
	sources[2] = strdup(path);
	task = fm_task_new(FM_TASK_RESTORE, sources + 2, 1, trash);
	run(task);
	check(file_is(targets[0], "report"), "put back: the file is where it was");
	check(!exists(other), "put back: the record is gone");
	fm_task_free(task);

	/* 8a. An item on a system's mount (the second folder, /dev/shm) goes to the home trash, not a trash of that mount (ws127-p008). */
	if (argc > 2) {
		snprintf(path, sizeof(path), "%s/System.txt", argv[2]);
		make_file(path, "system");
		error = fm_trash_for(path, other, sizeof(other));
		check(error == 0 && strcmp(other, trash) == 0, "system mount: an item of /dev/shm goes to the home trash");
		unlink(path);
	}

	/* 8b. An item on another volume (the third folder, a tmpfs of the user's) goes to that volume's trash, $topdir/.Trash-$uid (ws127-p003). */
	if (argc > 3) {
		snprintf(path, sizeof(path), "%s/volume", argv[3]);
		fm_ops_mkdir_parents(path);
		snprintf(path, sizeof(path), "%s/volume/Notes.txt", argv[3]);
		make_file(path, "notes");
		volume_items[0] = strdup(path);
		error = fm_trash_for(volume_items[0], other, sizeof(other));
		snprintf(value, sizeof(value), "/.Trash-%lu", (unsigned long)getuid());
		if (error == 0 && strstr(other, value) != NULL) {
			task = fm_task_new(FM_TASK_TRASH, volume_items, 1, trash);
			run(task);
			check(task->error_count == 0 && task->results[0] != NULL && strncmp(task->results[0], other, strlen(other)) == 0 && !exists(volume_items[0]), "volume trash: the item is in its volume's trash");
			snprintf(path, sizeof(path), "%s/info/Notes.txt.trashinfo", other);
			length = -1;
			descriptor = open(path, O_RDONLY);
			if (descriptor >= 0) {
				length = read(descriptor, text, sizeof(text) - 1);
				close(descriptor);
			}
			if (length > 0)
				text[length] = '\0';
			check(length > 0 && strstr(text, "\nPath=/") == NULL && strstr(text, "\nPath=") != NULL, "volume trash: the record's path is relative to the volume's top");
			error = fm_trash_info_read(other, "Notes.txt", original, sizeof(original), &deleted);
			check(error == 0 && strcmp(original, volume_items[0]) == 0, "volume trash: the record reads back as the whole path");
			memset(&listing, 0, sizeof(listing));
			error = fm_dir_read_trash(&listing);
			shown = 0;
			for (index = 0; error == 0 && index < listing.count; index++) {
				if (strcmp(listing.entries[index].path, task->results[0]) == 0 && listing.entries[index].detail != NULL && strstr(volume_items[0], listing.entries[index].detail) == volume_items[0])
					shown = 1;
			}
			check(shown == 1, "volume trash: the trash's listing shows the item and where it was");
			fm_dir_free(&listing);
			error = fm_trash_of(task->results[0], path, sizeof(path));
			check(error == 0 && strcmp(path, other) == 0, "volume trash: the item's trash is found from its path");
			error = fm_trash_of(volume_items[0], path, sizeof(path));
			check(error != 0, "volume trash: an item outside a trash is in none");
			volume_items[1] = strdup(task->results[0]);
			fm_task_free(task);
			task = fm_task_new(FM_TASK_RESTORE, volume_items + 1, 1, trash);
			run(task);
			snprintf(path, sizeof(path), "%s/info/Notes.txt.trashinfo", other);
			check(task->error_count == 0 && file_is(volume_items[0], "notes") && !exists(path), "volume trash: put back returns it and removes the record");
			fm_task_free(task);
			snprintf(path, sizeof(path), "%s/volume/Notes.txt", argv[3]);
			unlink(path);
			free(volume_items[1]);
		} else {
			check(0, "volume trash: the third folder's volume holds a trash");
		}
		free(volume_items[0]);
	} else {
		printf("skip: volume trash (no third folder on a volume of the user's)\n");
	}

	/* 9. Delete a folder tree for good. */
	snprintf(path, sizeof(path), "%s/dst/Folder.v1 2", root);
	targets[1] = strdup(path);
	task = fm_task_new(FM_TASK_DELETE, targets + 1, 1, NULL);
	run(task);
	check(!exists(targets[1]) && task->error_count == 0, "delete: the tree is gone");
	fm_task_free(task);

	/* 10. Move across file systems (when a second one is given): copied, then the source removed. */
	if (argc > 2) {
		snprintf(path, sizeof(path), "%s/dst/Folder.v1", root);
		targets[2] = strdup(path);
		task = fm_task_new(FM_TASK_MOVE, targets + 2, 1, argv[2]);
		run(task);
		snprintf(path, sizeof(path), "%s/Folder.v1/deep/leaf.txt", argv[2]);
		check(file_is(path, "leaf") && !exists(targets[2]), "move across file systems: copied and removed");
		fm_task_free(task);
		snprintf(path, sizeof(path), "%s/Folder.v1", argv[2]);
		targets[3] = strdup(path);
		task = fm_task_new(FM_TASK_DELETE, targets + 3, 1, NULL);
		run(task);
		fm_task_free(task);
	} else {
		printf("skip: move across file systems (no second folder)\n");
	}

	/* 11. The undo history: push, take, redo, and a new change empties the redo. */
	memset(&history, 0, sizeof(history));
	fm_undo_push(&history, FM_UNDO_RENAME, 1, sources, targets);
	fm_undo_push(&history, FM_UNDO_MOVE, 2, sources, targets);
	check(fm_undo_take(&history, 0, &item) == 1 && item.kind == FM_UNDO_MOVE && item.count == 2, "undo: the newest change");
	fm_undo_push_redo(&history, &item);
	check(history.redo_count == 1 && history.undo_count == 1, "undo: kept for redo");
	fm_undo_push(&history, FM_UNDO_COPY, 1, sources, targets);
	check(history.redo_count == 0 && history.undo_count == 2, "undo: a new change empties the redo");
	fm_undo_free(&history);

	/* 12. The clipboard. */
	error = fm_clip_set(FM_CLIP_CUT, sources, 2);
	check(error == 0, "clipboard: set");
	error = fm_clip_get(&mode, &paths, &count);
	check(error == 0 && mode == FM_CLIP_CUT && count == 2 && strcmp(paths[1], sources[1]) == 0, "clipboard: read back");
	fm_paths_free(paths, count);
	fm_clip_clear();
	fm_clip_get(&mode, &paths, &count);
	check(mode == FM_CLIP_NONE && count == 0, "clipboard: cleared");

	/* 13. Free names. */
	snprintf(path, sizeof(path), "%s/src", root);
	error = fm_unique_name(path, "Report.pdf", NULL, other, sizeof(other));
	snprintf(path, sizeof(path), "%s/src/Report 3.pdf", root);
	check(error == 0 && strcmp(other, path) == 0, "free name: Report 3.pdf after Report.pdf and Report 2.pdf");

	/* 14. The recent list (libkeiland): newest first, a path once, removal. */
	{
		static struct kl_recent_item items[8];

		snprintf(path, sizeof(path), "%s/src/Report.pdf", root);
		check(kl_recent_add(path, "test") == 0, "recent: add");
		snprintf(other, sizeof(other), "%s/src/Report copy.pdf", root);
		check(kl_recent_add(other, "test") == 0, "recent: add another");
		check(kl_recent_add(path, "test") == 0, "recent: add the first again");
		check(kl_recent_list(items, 8, &count) == 0 && count == 2 && strcmp(items[0].path, path) == 0 && strcmp(items[1].path, other) == 0, "recent: newest first, once each");
		check(kl_recent_remove(path) == 0, "recent: remove");
		check(kl_recent_list(items, 8, &count) == 0 && count == 1 && strcmp(items[0].path, other) == 0, "recent: the other is left");

		/* 15. (Tags, removed in ws127-p012.) */
	}

	/* 16. Pictures (p007): PPM and PGM read, damaged headers refused, thumbnails fitted; the peek of text. */
	{
		static struct fm_app pictures;
		struct kl_image image;
		struct fm_entry entry;
		const struct kl_image *thumb;
		unsigned char big[54 + 300 * 200 * 3];
		int width;
		int height;
		int header;
		int made;

		snprintf(path, sizeof(path), "%s/tiny.ppm", root);
		{
			static const char tiny[] = "P6\n# a comment\n2 1\n255\n\xff\x00\x00\x00\x00\xff";
			FILE *file = fopen(path, "wb");
			fwrite(tiny, 1, sizeof(tiny) - 1, file);
			fclose(file);
		}
		check(fm_image_load(path, &image) == 0 && image.width == 2 && image.height == 1 && image.pixels[0] == 0xffff0000U && image.pixels[1] == 0xff0000ffU, "picture: a PPM with a comment read, red then blue");
		kl_image_release(&image);
		snprintf(path, sizeof(path), "%s/grey.pgm", root);
		make_file(path, "P5 1 1 255\n\x80");
		check(fm_image_load(path, &image) == 0 && image.pixels[0] == 0xff808080U, "picture: a PGM read as grey");
		kl_image_release(&image);
		snprintf(path, sizeof(path), "%s/short.ppm", root);
		make_file(path, "P6\n4 4\n255\n\x01\x02");
		check(fm_image_load(path, &image) == EINVAL, "picture: missing pixels refused");
		snprintf(path, sizeof(path), "%s/deep.ppm", root);
		make_file(path, "P6\n1 1\n65535\n\x01\x02\x03\x04\x05\x06");
		check(fm_image_load(path, &image) == EINVAL, "picture: 16-bit values refused");
		snprintf(path, sizeof(path), "%s/huge.ppm", root);
		make_file(path, "P6\n99999999 1\n255\n\x01\x02\x03");
		check(fm_image_load(path, &image) == EFBIG, "picture: a huge width refused (too large, ws168-p004)");
		snprintf(path, sizeof(path), "%s/a.png", root);
		make_file(path, "\x89PNG\r\n\x1a\n....");
		check(fm_image_load(path, &image) == EINVAL, "picture: a damaged PNG refused");

		header = snprintf((char *)big, sizeof(big), "P6\n300 200\n255\n");
		memset(big + header, 0x40, 300 * 200 * 3);
		snprintf(path, sizeof(path), "%s/big.ppm", root);
		{
			FILE *file = fopen(path, "wb");
			fwrite(big, 1, (size_t)header + 300 * 200 * 3, file);
			fclose(file);
		}
		check(fm_image_thumbnail(path, 256, &image) == 0 && image.width == 256 && image.height == 171, "thumbnail: 300x200 shrunk to 256x171 (keiland-preview rounds, ws168-p004)");
		kl_image_release(&image);
		fm_image_fit(10, 40, 100, 100, &width, &height);
		check(width == 25 && height == 100, "fit: a tall picture fills the box's height");
		fm_image_fit(400, 100, 200, 200, &width, &height);
		check(width == 200 && height == 50, "fit: a wide picture fills the box's width");

		thumb = fm_thumb_get(&pictures, path, 7);
		check(thumb == NULL && strcmp(pictures.thumb_wanted[0], path) == 0, "thumbs: a new file is asked for");
		made = fm_thumb_tick(&pictures);
		thumb = fm_thumb_get(&pictures, path, 7);
		check(made == 1 && thumb != NULL && thumb->width == 256 && pictures.thumb_wanted[0][0] == '\0', "thumbs: made in a round, then kept");
		thumb = fm_thumb_get(&pictures, path, 8);
		check(thumb == NULL && pictures.thumb_wanted[0][0] != '\0', "thumbs: a changed file is asked for again");
		fm_thumb_release(&pictures);

		/* ws177-p010: two files asked for at once, a third waits; a damaged picture's failure is kept on disk. */
		{
			char damaged[FM_PATH_MAX + 32];
			char third[FM_PATH_MAX + 32];
			int error;

			snprintf(damaged, sizeof(damaged), "%s/damaged.png", root);
			make_file(damaged, "\x89PNG\r\n\x1a\n not a picture");
			snprintf(third, sizeof(third), "%s/third.ppm", root);
			make_file(third, "P6\n1 1\n255\nabc");
			(void)fm_thumb_get(&pictures, path, 8);
			(void)fm_thumb_get(&pictures, damaged, 1);
			(void)fm_thumb_get(&pictures, third, 1);
			check(strcmp(pictures.thumb_wanted[0], path) == 0 && strcmp(pictures.thumb_wanted[1], damaged) == 0, "thumbs: two files asked for at once, the third not");
			(void)fm_thumb_tick(&pictures);
			check(pictures.thumb_wanted[0][0] == '\0', "thumbs: both made in a round");
			check(fm_thumb_get(&pictures, damaged, 1) == NULL && fm_thumb_get(&pictures, damaged, 1) == NULL && pictures.thumb_wanted[0][0] == '\0',
			    "thumbs: a damaged picture is not asked for again");
			memset(&image, 0, sizeof(image));
			error = fm_thumb_cache_read(damaged, &image);
			check(error == EINVAL, "thumb cache: a damaged picture's failure is kept");
			fm_thumb_release(&pictures);
			(void)fm_thumb_get(&pictures, damaged, 1);
			made = fm_thumb_tick(&pictures);
			check(made == 1 && fm_thumb_get(&pictures, damaged, 1) == NULL && pictures.thumb_wanted[0][0] == '\0', "thumbs: the kept failure is read in a new session");
			fm_thumb_release(&pictures);
			{
				FILE *file = fopen(damaged, "ab");
				fputc(0, file);
				fclose(file);
			}
			check(fm_thumb_cache_read(damaged, &image) == ENOENT, "thumb cache: a changed damaged file is tried again");

			/* A picture made without waiting (Quick Look's, the hero's). */
			error = fm_picture_begin(FM_PICTURE_PEEK, path, 64, 64);
			made = 0;
			for (width = 0; width < 500 && made == 0; width++) {
				made = fm_picture_follow(FM_PICTURE_PEEK, &image, &error);
				if (made == 0)
					usleep(10000);
			}
			check(made == 1 && error == 0 && image.width == 64 && image.height == 43 && fm_picture_busy() == 0, "picture: made without waiting, 64x43");
			kl_image_release(&image);
			error = fm_picture_begin(FM_PICTURE_HERO, damaged, 64, 64);
			made = 0;
			for (width = 0; width < 500 && made == 0; width++) {
				made = fm_picture_follow(FM_PICTURE_HERO, &image, &error);
				if (made == 0)
					usleep(10000);
			}
			check(made == 1 && error == EINVAL && image.pixels == NULL, "picture: a damaged one made without waiting says EINVAL");
		}

		/* The thumbnail kept on disk (ws127-p002, F-035): written by the round above, read back alike; stale once the file changes. */
		memset(&image, 0, sizeof(image));
		check(fm_thumb_cache_read(path, &image) == 0 && image.width == 256 && image.height == 171 && (image.pixels[0] & 0x00ffffffU) == 0x404040U, "thumb cache: the made thumbnail is kept and read back");
		kl_image_release(&image);
		{
			FILE *file = fopen(path, "ab");
			fputc(0, file);
			fclose(file);
		}
		check(fm_thumb_cache_read(path, &image) == ENOENT, "thumb cache: a changed file's record is stale");

		snprintf(path, sizeof(path), "%s/notes", root);
		make_file(path, "one\ttwo\r\nthree\nfour");
		memset(&entry, 0, sizeof(entry));
		entry.path = path;
		entry.name = "notes";
		entry.modified = 5;
		entry.mime = fm_mime_guess("notes", 0100644);
		fm_peek_read(&pictures.peek, &entry);
		check(pictures.peek.text != NULL && strcmp(pictures.peek.text, "one two\nthree\nfour") == 0 && pictures.peek.line_count == 3, "peek: text without an extension sniffed, its lines kept (tab a space, CR gone)");
		check(strcmp(pictures.peek.mime->type, "text/plain") == 0, "peek: its type is text/plain");
		fm_peek_release(&pictures.peek);
	}

	/* 17. Opening (p012): the lists' order, the executable first, the quoting; the information and its checksum. */
	{
		static struct fm_info info;
		struct fm_opener openers[FM_OPENERS];
		char output[FM_PATH_MAX + 64];
		char config[2 * FM_PATH_MAX];
		char text[64];
		int count;
		int tries;

		snprintf(config, sizeof(config), "%s/config", root);
		mkdir(config, 0755);
		setenv("XDG_CONFIG_HOME", config, 1);
		snprintf(path, sizeof(path), "%s/config/keiland", root);
		mkdir(path, 0755);
		snprintf(output, sizeof(output), "%s/opened", root);
		snprintf(path, sizeof(path), "%s/config/keiland/open-with", root);
		{
			FILE *file = fopen(path, "w");
			fprintf(file, "# a comment\nmalformed line\ntext/plain, text/csv\tRecord\techo %%f > '%s'\nimage/png\tViewer\tview\n", output);
			fclose(file);
		}
		count = fm_apps_for("/x/a.txt", fm_mime_guess("a.txt", 0100644), 0100644, openers, FM_OPENERS);
		check(count >= 2 && strcmp(openers[0].name, "Record") == 0 && strcmp(openers[1].name, "Terminal (less)") == 0, "open: the user's list first, then the built-in viewer");
		count = fm_apps_for("/x/run", fm_mime_guess("run", 0100755), 0100755, openers, FM_OPENERS);
		check(count >= 2 && strcmp(openers[0].name, "Run in Terminal") == 0, "open: a program runs in a terminal first");
		check(strstr(openers[0].command, "Press Return") != NULL, "open: the terminal stays when the program ends (BUG-234)");
		count = fm_apps_for("/x/clip.mp4", fm_mime_guess("clip.mp4", 0100755), 0100755, openers, FM_OPENERS);
		check(count >= 1 && strcmp(openers[0].name, "Run in Terminal") != 0 && strcmp(openers[count - 1].name, "Run in Terminal") != 0, "open: a video with x bits does not run (BUG-233)");
		snprintf(path, sizeof(path), "%s/gui", root);
		make_elf(path, "libwayland-client.so");
		count = fm_apps_for(path, fm_mime_guess("gui", 0100755), 0100755, openers, FM_OPENERS);
		check(count >= 2 && strcmp(openers[0].name, "Open") == 0 && strcmp(openers[1].name, "Run in Terminal") == 0, "open: a Wayland program opens by itself, then in a terminal (BUG-234)");
		snprintf(path, sizeof(path), "%s/cli", root);
		make_elf(path, "libc.so");
		count = fm_apps_for(path, fm_mime_guess("cli", 0100755), 0100755, openers, FM_OPENERS);
		check(count >= 1 && strcmp(openers[0].name, "Run in Terminal") == 0, "open: a command-line program runs in a terminal (BUG-234)");
		count = fm_apps_for("/x/a.ppm", fm_mime_guess("a.ppm", 0100644), 0100644, openers, FM_OPENERS);
		check(count >= 1 && fm_apps_is_quicklook(&openers[0]) == 1, "open: a picture opens in Quick Look");
		count = fm_apps_for("/x/a.png", fm_mime_guess("a.png", 0100644), 0100644, openers, FM_OPENERS);
		check(count >= 2 && strcmp(openers[0].name, "Viewer") == 0 && fm_apps_is_quicklook(&openers[1]) == 1, "open: a list's line comes before the built-in way");

		snprintf(path, sizeof(path), "%s/it's a file.txt", root);
		make_file(path, "x");
		count = fm_apps_for(path, fm_mime_guess("it's a file.txt", 0100644), 0100644, openers, FM_OPENERS);
		check(fm_apps_launch(&openers[0], path) == 0, "open: launched");
		for (tries = 0; tries < 100 && !exists(output); tries++)
			usleep(20000);
		usleep(50000);
		snprintf(text, sizeof(text), "%s", "");
		{
			char expected[2 * FM_PATH_MAX + 2];
			snprintf(expected, sizeof(expected), "%s\n", path);
			check(file_is(output, expected), "open: the command got the path with its quote and spaces");
		}

		fm_mode_text(0104755, text, sizeof(text));
		check(strcmp(text, "-rwsr-xr-x (4755)") == 0, "info: set-user-ID shown as s");
		fm_mode_text(041777, text, sizeof(text));
		check(strcmp(text, "drwxrwxrwt (1777)") == 0, "info: a sticky folder shown as t");

		snprintf(path, sizeof(path), "%s/abc", root);
		make_file(path, "abc");
		setxattr(path, "user.note", "hello", 5, 0);
		fm_info_release(&info);
		check(fm_info_gather(&info, path) == 0 && info.size == 3 && info.attribute_count == 1 && strcmp(info.attributes[0].name, "user.note") == 0, "info: gathered, with the attribute's name");
		check(fm_info_checksum_start(&info) == 0 && info.checksum_state == FM_CHECKSUM_RUNNING, "info: checksum started");
		for (tries = 0; tries < 10 && info.checksum_state == FM_CHECKSUM_RUNNING; tries++)
			fm_info_checksum_step(&info, 10);
		check(info.checksum_state == FM_CHECKSUM_DONE && strcmp(info.checksum, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0, "info: SHA-256 of abc");
		snprintf(path, sizeof(path), "%s/no-such", root);
		check(fm_info_gather(&info, path) == ENOENT, "info: a missing path says ENOENT");
		fm_info_release(&info);
	}

	printf("files-model: %s\n", failures == 0 ? "PASS" : "FAIL");
	return failures != 0;
}

static void
check(
	int condition,
	const char *what)
{
	printf("%s: %s\n", condition ? "ok" : "FAIL", what);
	if (!condition)
		failures++;
}

/* Writes a small 64-bit little-endian ELF program that links one library, enough for Files to read its DT_NEEDED (BUG-234). */
static void
make_elf(
	const char *path,
	const char *library)
{
	unsigned char image[512];
	unsigned char *entry;
	size_t length;
	unsigned index;
	FILE *file;

	/* The ELF header: 64 bits, little-endian, two program headers at 64. */
	memset(image, 0, sizeof(image));
	memcpy(image, "\177ELF", 4);
	image[4] = 2U;
	image[5] = 1U;
	image[6] = 1U;
	image[0x20] = 64U;
	image[0x36] = 56U;
	image[0x38] = 2U;

	/* A loaded segment of the whole file at address 0x400000, and the dynamic segment at 256. */
	entry = image + 64;
	entry[0] = 1U;
	entry[18] = 0x40U;
	entry[32] = 0x00U;
	entry[33] = 0x02U;
	entry = image + 64 + 56;
	entry[0] = 2U;
	entry[8] = 0x00U;
	entry[9] = 0x01U;
	entry[32] = 64U;

	/* The dynamic entries: DT_NEEDED at string 1, DT_STRTAB at 0x400180 (file 384), DT_STRSZ, DT_NULL. */
	length = strlen(library) + 2U;
	entry = image + 256;
	entry[0] = 1U;
	entry[8] = 1U;
	entry = image + 256 + 16;
	entry[0] = 5U;
	entry[8] = 0x80U;
	entry[9] = 0x01U;
	entry[10] = 0x40U;
	entry = image + 256 + 32;
	entry[0] = 10U;
	entry[8] = (unsigned char)length;

	/* The string table: an empty name, then the library's. */
	for (index = 0; library[index] != '\0'; index++)
		image[384 + 1 + index] = (unsigned char)library[index];

	/* The file, which may run. */
	file = fopen(path, "wb");
	if (file == NULL)
		return;
	(void)fwrite(image, 1, sizeof(image), file);
	fclose(file);
	(void)chmod(path, 0755);
}

static void
make_file(
	const char *path,
	const char *text)
{
	FILE *file;

	file = fopen(path, "w");
	if (file == NULL)
		return;
	fputs(text, file);
	fclose(file);
}

static int
file_is(
	const char *path,
	const char *text)
{
	char buffer[256];
	FILE *file;
	size_t length;

	file = fopen(path, "r");
	if (file == NULL)
		return 0;
	length = fread(buffer, 1, sizeof(buffer) - 1, file);
	fclose(file);
	buffer[length] = '\0';
	return strcmp(buffer, text) == 0;
}

static int
exists(
	const char *path)
{
	struct stat status;

	return lstat(path, &status) == 0;
}

static int
run(
	struct fm_task *task)
{
	int rounds;

	for (rounds = 0; rounds < 10000; rounds++) {
		if (fm_task_step(task, 5) == 0)
			return 0;
	}
	return -1;
}
