/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The pictures of files: their thumbnails for the icons, the pictures the
 * preview, Quick Look and the Today page show, and the fitting of a
 * picture into a box.
 *
 * ws168-p004 (plan/ws168/phase001/phase.md section 5): Files decodes no
 * picture itself.  keiland-preview makes each one in a sandbox (on
 * zedBSD sandbox_spawn, on Linux a process that confines itself with
 * seccomp; on FreeBSD, until it confines itself with Capsicum, in this
 * process: userland/desktop/preview/client.h), and Files reads back only
 * the binary PPM it wrote.
 *
 * Thumbnails are made when an item is drawn and not yet kept: the drawing
 * asks, up to FM_THUMB_MAKERS children are started (ws177-p010), and the
 * main loop looks at them each round until they end (the window keeps
 * answering meanwhile); a child writes the thumbnail's record in the
 * cache (thumb-cache.c) beside its place, renamed into it when it
 * succeeded.  A file that could not be made into a thumbnail leaves a
 * failure's record instead, and is not tried again until it changes.
 * The last FM_THUMBS thumbnails are kept, the least recently drawn going
 * first; a file changed since it was read is read again.
 *
 * Quick Look's picture and the Today page's hero are made the same way
 * without waiting (fm_picture_begin, ws177-p010): the window draws on
 * while the child works and is drawn again when the picture is there.
 */

#include "files.h"

#include "../preview/client.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The size the Today page's picture is made within. */
#define THUMB_HERO_WIDTH	1920
#define THUMB_HERO_HEIGHT	1200

/*
 * A thumbnail being made: whether its child runs, its job, the slot it
 * goes in, the record's place in the cache and the file the child writes
 * beside it.
 */
struct thumb_making {
	int running;
	struct preview_job job;
	struct fm_thumb *thumb;
	char record[FM_PATH_MAX];
	char temporary[FM_PATH_MAX + 32];
};

/*
 * A picture being made without waiting (fm_picture_begin): whether its
 * child runs, and the child with its output.
 */
struct thumb_picture_job {
	int running;
	struct preview_pending pending;
};

/*
 * The thumbnails being made, one child each; a maker is free when it does
 * not run.  Only the main loop starts and follows them.
 */
static struct thumb_making thumb_makers[FM_THUMB_MAKERS];

/* The pictures being made without waiting, by FM_PICTURE_*; the main loop's alone. */
static struct thumb_picture_job thumb_pictures[FM_PICTURES];

static int thumb_start(struct thumb_making *making, struct fm_thumb *thumb);
static int thumb_follow(struct thumb_making *making);
static int thumb_picture(const char *path, int width, int height, struct kl_image *image);
static struct fm_thumb *thumb_find(struct fm_app *app, const char *path, time_t modified);
static struct fm_thumb *thumb_slot(struct fm_app *app);
static int thumb_take_wanted(struct fm_app *app, struct thumb_making *making);
static int thumb_failure_kept(int status, int error);

/*
 * Reads a picture file into an image of opaque pixels no larger than the
 * Today page's (the first page of a PDF too).
 *
 * Returns 0, EINVAL for a file that is not a picture read or a damaged
 * one, EFBIG for one too large, or another errno value.
 */
int
fm_image_load(
	const char *path,
	struct kl_image *image)
{
	/* Made by keiland-preview. */
	return thumb_picture(path, THUMB_HERO_WIDTH, THUMB_HERO_HEIGHT, image);
}

/*
 * Reads a picture and shrinks it to fit a square of a side, keeping its
 * shape; a picture smaller than the square keeps its size.
 *
 * Returns 0 or an errno value.
 */
int
fm_image_thumbnail(
	const char *path,
	int side,
	struct kl_image *thumbnail)
{
	/* Made by keiland-preview. */
	return thumb_picture(path, side, side, thumbnail);
}

/*
 * Returns the kept thumbnail of a file as it is now, or NULL.
 *
 * A file that has no thumbnail yet is asked for, and its thumbnail is made
 * in later rounds of the main loop (fm_thumb_tick); only as many files
 * are asked for at a time as are made at once, so the drawing asks again
 * for the others.
 */
const struct kl_image *
fm_thumb_get(
	struct fm_app *app,
	const char *path,
	time_t modified)
{
	struct fm_thumb *thumb;
	int index;
	int differs;

	/* A thumbnail of the same file as it is now is used, and counts as recently drawn. */
	thumb = thumb_find(app, path, modified);
	if (thumb != NULL) {
		app->thumb_clock++;
		thumb->used = app->thumb_clock;

		/* A file that could not be read, or one still being made, has no picture. */
		if (thumb->failed != 0 || thumb->pending != 0)
			return NULL;

		/* The picture. */
		return &thumb->image;
	}

	/* Otherwise the file is asked for, when the list of those asked for has room and lacks it. */
	for (index = 0; index < FM_THUMB_MAKERS; index++) {
		/* The end of the list: the file goes there. */
		if (app->thumb_wanted[index][0] == '\0') {
			snprintf(app->thumb_wanted[index], sizeof(app->thumb_wanted[index]), "%s", path);
			app->thumb_wanted_modified[index] = modified;
			break;
		}

		/* A file asked for already. */
		differs = strcmp(app->thumb_wanted[index], path);
		if (differs == 0)
			break;
	}

	/* No picture yet. */
	return NULL;
}

/*
 * Follows the thumbnails being made, and starts those asked for while a
 * maker is free (each taken from the cache when it is there).
 *
 * Returns nonzero when one was made, so that the window is drawn again.
 */
int
fm_thumb_tick(
	struct fm_app *app)
{
	struct thumb_making *making;
	unsigned index;
	int made;
	int ended;

	/* Follows each child running. */
	made = 0;
	for (index = 0U; index < FM_THUMB_MAKERS; index++) {
		making = &thumb_makers[index];
		if (!making->running)
			continue;
		ended = thumb_follow(making);
		if (ended)
			made = 1;
	}

	/* Gives each free maker a file asked for. */
	for (index = 0U; index < FM_THUMB_MAKERS; index++) {
		/* Nothing more is asked for. */
		if (app->thumb_wanted[0][0] == '\0')
			break;

		/* A maker at work. */
		making = &thumb_makers[index];
		if (making->running)
			continue;

		/* The file taken: made at once from the cache, or a child started. */
		ended = thumb_take_wanted(app, making);
		if (ended)
			made = 1;
	}

	/* Reports whether a thumbnail is new. */
	return made;
}

/*
 * Tells whether a thumbnail is being made (the main loop then looks at it
 * again soon).
 */
int
fm_thumb_busy(void)
{
	unsigned index;

	/* Looks for a child running. */
	for (index = 0U; index < FM_THUMB_MAKERS; index++) {
		if (thumb_makers[index].running)
			return 1;
	}

	/* None runs. */
	return 0;
}

/*
 * Frees the kept thumbnails (the children still running are ended, and
 * what they wrote goes).
 */
void
fm_thumb_release(
	struct fm_app *app)
{
	struct thumb_making *making;
	unsigned index;
	int slot;

	/* Ends each child running, and removes what it wrote. */
	for (index = 0U; index < FM_THUMB_MAKERS; index++) {
		making = &thumb_makers[index];
		if (!making->running)
			continue;
		making->job.limit_ms = 0U;
		making->job.started_ms = 0U;
		(void)preview_poll(&making->job);
		(void)unlink(making->temporary);
		making->running = 0;
	}

	/* Each slot's picture, and the slot emptied. */
	for (slot = 0; slot < FM_THUMBS; slot++) {
		kl_image_release(&app->thumbs[slot].image);
		memset(&app->thumbs[slot], 0, sizeof(app->thumbs[slot]));
	}

	/* Nothing is asked for any more. */
	memset(app->thumb_wanted, 0, sizeof(app->thumb_wanted));
}

/*
 * Starts making a picture of a file within a size without waiting
 * (ws177-p010), as one of FM_PICTURE_* (one given up if it was still
 * being made).  Returns 0, or an errno value (no picture is then made).
 */
int
fm_picture_begin(
	unsigned which,
	const char *path,
	int width,
	int height)
{
	struct thumb_picture_job *job;
	struct preview_request request;
	int error;

	/* Gives up the one made before. */
	job = &thumb_pictures[which];
	fm_picture_cancel(which);

	/* Starts the child. */
	memset(&request, 0, sizeof(request));
	request.width = width;
	request.height = height;
	error = preview_picture_begin(path, &request, &job->pending);
	if (error != 0)
		return error;

	/* Followed by fm_picture_follow from now on. */
	job->running = 1;

	/* Succeeded: the picture is being made. */
	return 0;
}

/*
 * Looks at a picture being made without waiting.  Returns 0 while it is
 * made (or when none is), 1 when it is done: the image (the caller's) and
 * *error 0, or *error the reason there is none.
 */
int
fm_picture_follow(
	unsigned which,
	struct kl_image *image,
	int *error)
{
	struct thumb_picture_job *job;
	struct preview_picture picture;
	int finished;

	/* Nothing yet. */
	memset(image, 0, sizeof(*image));
	*error = 0;

	/* No picture is being made. */
	job = &thumb_pictures[which];
	if (!job->running)
		return 0;

	/* A child still at work. */
	finished = preview_picture_follow(&job->pending, &picture, error);
	if (!finished)
		return 0;
	job->running = 0;

	/* The image takes the picture's pixels (no padding between the rows). */
	if (*error == 0) {
		image->pixels = picture.pixels;
		image->width = picture.width;
		image->height = picture.height;
		image->stride = (size_t)picture.width;
	}

	/* Succeeded: the picture is done, made or not. */
	return 1;
}

/*
 * Gives up a picture being made without waiting: its child is ended.
 */
void
fm_picture_cancel(
	unsigned which)
{
	struct thumb_picture_job *job;

	/* Nothing is being made. */
	job = &thumb_pictures[which];
	if (!job->running)
		return;

	/* The child ended and its output gone. */
	preview_picture_cancel(&job->pending);
	job->running = 0;
}

/*
 * Tells whether a picture is being made without waiting (the main loop
 * then looks at it again soon).
 */
int
fm_picture_busy(void)
{
	unsigned index;

	/* Looks for a child running. */
	for (index = 0U; index < FM_PICTURES; index++) {
		if (thumb_pictures[index].running)
			return 1;
	}

	/* None runs. */
	return 0;
}

/*
 * Works out the size of a picture fitted in a box, keeping its shape; the
 * fitted picture is at least a pixel each way.
 */
void
fm_image_fit(
	int width,
	int height,
	int box_width,
	int box_height,
	int *fit_width,
	int *fit_height)
{
	/* An empty picture fills nothing. */
	if (width <= 0 || height <= 0) {
		*fit_width = 1;
		*fit_height = 1;
		return;
	}

	/* The box's width decides when the picture is wider in shape than the box; its height otherwise. */
	if ((long)width * box_height >= (long)height * box_width) {
		*fit_width = box_width;
		*fit_height = (int)((long)height * box_width / width);
	} else {
		*fit_height = box_height;
		*fit_width = (int)((long)width * box_height / height);
	}

	/* At least a pixel each way. */
	if (*fit_width < 1)
		*fit_width = 1;
	if (*fit_height < 1)
		*fit_height = 1;
}

/*
 * Takes the first file asked for into the slot least recently drawn and
 * into a free maker: from the cache when the cache has it (or a failure
 * kept for it), else by a child.  Returns nonzero when the slot is done
 * now (a thumbnail, or none), zero while its child works.
 */
static int
thumb_take_wanted(
	struct fm_app *app,
	struct thumb_making *making)
{
	struct fm_thumb *thumb;
	int error;
	int ended;

	/* Empties the slot of the thumbnail it held; it is now the first file asked for's. */
	thumb = thumb_slot(app);
	kl_image_release(&thumb->image);
	snprintf(thumb->path, sizeof(thumb->path), "%s", app->thumb_wanted[0]);
	thumb->modified = app->thumb_wanted_modified[0];
	app->thumb_clock++;
	thumb->used = app->thumb_clock;
	thumb->failed = 0;
	thumb->pending = 0;

	/* Moves the rest of the list up. */
	memmove(&app->thumb_wanted[0], &app->thumb_wanted[1], sizeof(app->thumb_wanted) - sizeof(app->thumb_wanted[0]));
	memmove(&app->thumb_wanted_modified[0], &app->thumb_wanted_modified[1], sizeof(app->thumb_wanted_modified) - sizeof(app->thumb_wanted_modified[0]));
	app->thumb_wanted[FM_THUMB_MAKERS - 1][0] = '\0';

	/* Reads the thumbnail kept on disk for the file as it is now (ws127-p002). */
	error = fm_thumb_cache_read(thumb->path, &thumb->image);
	if (error == 0) {
		fm_log("THUMB path=%s error=0 width=%d height=%d cached=1", thumb->path, thumb->image.width, thumb->image.height);
		return 1;
	}

	/* A failure kept for the file as it is now: not tried again (ws177-p010). */
	if (error == EINVAL) {
		thumb->failed = 1;
		fm_log("THUMB path=%s error=%d width=0 height=0 cached=1 failed=1", thumb->path, error);
		return 1;
	}

	/* Starts a child to make it (made at once in this process on FreeBSD: followed now). */
	error = thumb_start(making, thumb);
	if (error == 0) {
		ended = thumb_follow(making);

		/* Succeeded: the child runs, or was done at once. */
		return ended;
	}

	/* A file whose child cannot be started has no thumbnail. */
	thumb->failed = 1;
	fm_log("THUMB path=%s error=%d width=0 height=0 cached=0", thumb->path, error);

	/* Succeeded: the slot is done, without a thumbnail. */
	return 1;
}

/*
 * Starts the child that writes a slot's thumbnail as its record in the
 * cache (the file beside the record, renamed when it succeeds).  Returns
 * 0, or an errno value.
 */
static int
thumb_start(
	struct thumb_making *making,
	struct fm_thumb *thumb)
{
	struct preview_request request;
	int error;
	int fd;

	/* The record's place and the stamp the cache reads (the file's time and size). */
	memset(&request, 0, sizeof(request));
	request.width = FM_THUMB_SIDE;
	request.height = FM_THUMB_SIDE;
	error = fm_thumb_cache_target(thumb->path, making->record, sizeof(making->record), request.stamp, sizeof(request.stamp));
	if (error != 0)
		return error;

	/* The file the child writes, new, named for this process and this maker. */
	(void)snprintf(making->temporary, sizeof(making->temporary), "%s.%ld.%d", making->record, (long)getpid(), (int)(making - thumb_makers));
	fd = open(making->temporary, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
	if (fd < 0)
		return errno;

	/* The child. */
	error = preview_start(thumb->path, fd, &request, &making->job);
	(void)close(fd);
	if (error != 0) {
		(void)unlink(making->temporary);
		return error;
	}

	/* Followed from now on; the slot is kept from reuse while it is made. */
	making->running = 1;
	making->thumb = thumb;
	thumb->pending = 1;
	return 0;
}

/*
 * Looks at a maker's child: when it has ended, its record is put in place
 * and read into the slot, or the slot is marked failed (and the failure
 * kept on disk when the file is to blame).  Returns nonzero when it ended.
 */
static int
thumb_follow(
	struct thumb_making *making)
{
	struct fm_thumb *thumb;
	int finished;
	int status;
	int error;
	int kept;

	/* Still running. */
	finished = preview_poll(&making->job);
	if (!finished)
		return 0;

	/* Ended: the record in place and read, or nothing. */
	making->running = 0;
	thumb = making->thumb;
	thumb->pending = 0;
	status = making->job.status;
	error = preview_status_error(status);
	if (status == PREVIEW_OK) {
		error = rename(making->temporary, making->record);
		if (error == 0)
			error = fm_thumb_cache_read(thumb->path, &thumb->image);
		if (error == 0)
			(void)fm_thumb_cache_trim(FM_THUMB_RECORDS_MAX, FM_THUMB_RECORDS_KEEP);
	}

	/* A failure leaves nothing, and the file is not tried again until it changes. */
	if (error != 0) {
		(void)unlink(making->temporary);
		thumb->failed = 1;
	}

	/* A file to blame for the failure is remembered as one on disk (ws177-p010). */
	kept = thumb_failure_kept(status, error);
	if (kept)
		(void)fm_thumb_cache_fail(thumb->path);

	/* The log line the tests wait for. */
	fm_log("THUMB path=%s error=%d width=%d height=%d cached=0 status=%d pid=%ld", thumb->path, error, thumb->image.width, thumb->image.height, status,
	    (long)making->job.pid);
	return 1;
}

/*
 * Tells whether a thumbnail's failure is the file's, to be remembered on
 * disk: a file of no kind read, damaged, too large or too costly, one
 * whose child ran out of time, or one whose output did not read back.  A
 * failure to write the output (EIO) is this computer's, and is tried
 * again in another session.
 */
static int
thumb_failure_kept(
	int status,
	int error)
{
	/* A thumbnail made. */
	if (error == 0)
		return 0;

	/* An output the child wrote that does not read (it succeeded, it said). */
	if (status == PREVIEW_OK)
		return 1;

	/* The output could not be written, or the child could not run. */
	if (error == EIO)
		return 0;

	/* Succeeded: the file is to blame. */
	return 1;
}

/* Has keiland-preview make a picture within a size and reads it into an image; 0 or an errno value. */
static int
thumb_picture(
	const char *path,
	int width,
	int height,
	struct kl_image *image)
{
	struct preview_request request;
	struct preview_picture picture;
	int error;

	/* The picture, waited for. */
	memset(image, 0, sizeof(*image));
	memset(&request, 0, sizeof(request));
	request.width = width;
	request.height = height;
	error = preview_picture(path, &request, &picture);
	if (error != 0)
		return error;

	/* The image takes its pixels (no padding between the rows). */
	image->pixels = picture.pixels;
	image->width = picture.width;
	image->height = picture.height;
	image->stride = (size_t)picture.width;
	return 0;
}

/* Finds the kept thumbnail of a file as it is now; NULL when there is none. */
static struct fm_thumb *
thumb_find(
	struct fm_app *app,
	const char *path,
	time_t modified)
{
	struct fm_thumb *thumb;
	int index;
	int match;

	/* Each slot in use, for the same path and the same modification time. */
	for (index = 0; index < FM_THUMBS; index++) {
		thumb = &app->thumbs[index];
		if (thumb->path[0] == '\0')
			continue;
		if (thumb->modified != modified)
			continue;

		/* The same file. */
		match = strcmp(thumb->path, path);
		if (match == 0)
			return thumb;
	}

	/* No slot holds it. */
	return NULL;
}

/* Chooses the slot a new thumbnail goes in: a free one, else the least recently drawn (never one being made). */
static struct fm_thumb *
thumb_slot(
	struct fm_app *app)
{
	int oldest;
	int index;

	/* Starts from the first slot not being made (there are more slots than makers). */
	oldest = 0;
	while (app->thumbs[oldest].pending != 0)
		oldest++;

	/* Finds a free slot, or the one drawn longest ago (a free slot was never drawn, so it is the oldest). */
	for (index = oldest + 1; index < FM_THUMBS; index++) {
		if (app->thumbs[index].pending != 0)
			continue;
		if (app->thumbs[index].used < app->thumbs[oldest].used)
			oldest = index;
	}

	/* Reports the slot. */
	return &app->thumbs[oldest];
}
