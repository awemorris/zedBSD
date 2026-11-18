/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The media picker's model (ws197-p014): the library's items read from the
 * compositor's metadata snapshot, the selection and the answer, and the
 * thread that makes the thumbnails of the items in sight -- from the
 * desktop's shared cache, else with the application's decoder, keeping
 * what it made in the cache for the next time (Photos reads the same
 * squares).
 */

#include "picker.h"

#include "userland/desktop/picture/media-kind.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The cache folder under $XDG_CACHE_HOME (Photos' folder of thumbnails). */
#define MODEL_CACHE		"keiland/photos"

/* The longest line of a snapshot, with its newline and NUL, and the fields of a media row. */
#define MODEL_LINE_MAX		16384U
#define MODEL_FIELDS		12U

/* The bytes of a file's start that tell its kind. */
#define MODEL_HEAD		16U

static int model_add(struct keiui_picker *picker, char **fields);
static int model_compare(const void *left, const void *right);
static void model_clear(struct keiui_picker *picker);
static void model_thread(void *data);
static int model_next(struct keiui_picker *picker, size_t *index);
static void model_make(struct keiui_picker *picker, size_t index, struct kl_image *image, int *video, int *thumb);
static int model_kind(const char *path, int *kind);
static int model_cache_read(const struct keiui_picker *picker, const char *id, struct kl_image *image);
static void model_cache_write(const struct keiui_picker *picker, const char *id, const struct kl_image *image);
static int model_folders(const char *path);
static void model_renumber(struct keiui_picker *picker, unsigned removed);

/*
 * Checks the options and makes an empty picker with its cache folder.
 */
int
keiui_picker_init(
	struct keiui_picker *picker,
	const struct kl_media_picker_options *options)
{
	const char *title;
	int error;

	/* Nothing yet: no items, no selection, no answer. */
	memset(picker, 0, sizeof(*picker));
	picker->cursor = -1L;

	/* The title the window gets. */
	title = "Photos & Videos";
	if (options != NULL && options->title != NULL)
		title = options->title;
	(void)snprintf(picker->title, sizeof(picker->title), "%s", title);

	/* One item unless the application takes more, and never more than one answer carries. */
	picker->limit = 1U;
	if (options != NULL && options->limit != 0U)
		picker->limit = options->limit;
	if (picker->limit > KL_MEDIA_PICKER_LIMIT_MAX)
		return EINVAL;

	/* The application's decoder for the photos the cache does not have. */
	if (options != NULL) {
		picker->thumbnail = options->thumbnail;
		picker->thumbnail_data = options->thumbnail_data;
	}

	/* The shared cache folder; without one every photo is decoded each time. */
	error = keiui_picker_cache_folder(picker->cache, sizeof(picker->cache));
	if (error != 0)
		picker->cache[0] = '\0';

	/* The grid scrolls down. */
	error = kl_scroll_init(&picker->scroll, KL_SCROLL_Y);
	if (error != 0)
		return error;
	picker->scroll_made = 1;

	/* The lock the thread and the view share. */
	error = kl_mutex_init(&picker->lock);
	if (error != 0)
		return error;
	error = kl_cond_init(&picker->wake);
	if (error != 0) {
		kl_mutex_destroy(&picker->lock);
		return error;
	}

	/* The lock and the condition are made and released by keiui_picker_fini. */
	picker->lock_made = 1;

	/* Succeeded: the picker can be started. */
	return 0;
}

/*
 * Lists the library's items from a compositor metadata snapshot, the newest
 * first.  The caller keeps its descriptor.
 */
int
keiui_picker_load(
	struct keiui_picker *picker,
	int descriptor)
{
	FILE *input;
	char *line;
	char *fields[MODEL_FIELDS];
	char *part;
	char *got;
	size_t count;
	size_t length;
	int copy;
	int same;
	int error;

	/* The items cannot change once the thread looks at them. */
	if (picker->thread_made)
		return EBUSY;

	/* The stream owns a duplicate, so the caller's descriptor stays open. */
	copy = dup(descriptor);
	if (copy < 0)
		return errno;
	input = fdopen(copy, "r");
	if (input == NULL) {
		error = errno;
		(void)close(copy);
		return error;
	}

	/* A line's room, too large for the stack of a thread that may be small. */
	line = malloc(MODEL_LINE_MAX);
	if (line == NULL) {
		(void)fclose(input);
		return ENOMEM;
	}

	/* Requires the versioned metadata header before accepting any path. */
	got = fgets(line, (int)MODEL_LINE_MAX, input);
	error = EPROTO;
	if (got != NULL) {
		same = strncmp(line, "# keiland-media 1\t", 18U);
		if (same == 0)
			error = 0;
	}

	/* Replaces the list with the snapshot's media rows; album rows belong to Photos. */
	model_clear(picker);
	while (error == 0) {
		got = fgets(line, (int)MODEL_LINE_MAX, input);
		if (got == NULL)
			break;

		/* A row must end in its newline; a cut row is a malformed snapshot. */
		length = strlen(line);
		if (length == 0U || line[length - 1U] != '\n') {
			error = EPROTO;
			break;
		}

		/* Only media rows become items. */
		if (line[0] != 'P' || line[1] != '\t')
			continue;

		/* Splits the row at its tabs. */
		line[length - 1U] = '\0';
		count = 0U;
		part = line;
		while (part != NULL && count < MODEL_FIELDS) {
			fields[count] = part;
			count++;
			part = strchr(part, '\t');
			if (part != NULL) {
				*part = '\0';
				part++;
			}
		}

		/* The twelve fields, with an absolute path. */
		if (count != MODEL_FIELDS || part != NULL || fields[2][0] != '/') {
			error = EPROTO;
			break;
		}

		/* The item. */
		error = model_add(picker, fields);
	}

	/* A stream's failure or a malformed snapshot never leaves a partial list. */
	same = ferror(input);
	if (same != 0 && error == 0)
		error = EIO;
	free(line);
	(void)fclose(input);
	if (error != 0) {
		model_clear(picker);
		return error;
	}

	/* The newest first. */
	if (picker->count > 1U)
		qsort(picker->items, picker->count, sizeof(picker->items[0]), model_compare);

	/* Succeeded: the items can be shown and chosen. */
	return 0;
}

/*
 * Starts the thread that makes the thumbnails; the items do not move after
 * this.
 */
int
keiui_picker_start(
	struct keiui_picker *picker)
{
	struct kl_thread_options options;
	int error;

	/* Once, after the lock is made. */
	if (picker->thread_made || !picker->lock_made)
		return EINVAL;

	/* The thread waits until the view wants items made: its name, what it runs and its priority. */
	memset(&options, 0, sizeof(options));
	options.name = "kl-picker-scan";
	options.run = model_thread;
	options.argument = picker;
	options.priority = KL_THREAD_PRIORITY_NORMAL;
	error = kl_thread_start(&options, &picker->thread);
	if (error != 0)
		return error;

	/* keiui_picker_fini stops and joins the thread. */
	picker->thread_made = 1;

	/* Succeeded: the thread runs. */
	return 0;
}

/*
 * Stops the thread and releases the items, the answer and the scroll.
 */
void
keiui_picker_fini(
	struct keiui_picker *picker)
{
	/* The thread finishes the thumbnail it makes, then ends. */
	if (picker->thread_made) {
		kl_mutex_lock(&picker->lock);

		picker->stop = 1;
		kl_cond_signal(&picker->wake);

		kl_mutex_unlock(&picker->lock);

		/* Waits for it to end. */
		(void)kl_thread_join(picker->thread);
		picker->thread = NULL;
		picker->thread_made = 0;
	}

	/* The items and their thumbnails, and the answer that borrowed their strings. */
	model_clear(picker);
	free(picker->answer);
	picker->answer = NULL;
	picker->answer_count = 0U;

	/* The scroll. */
	if (picker->scroll_made) {
		kl_scroll_release(&picker->scroll);
		picker->scroll_made = 0;
	}

	/* The lock, now that no thread uses it. */
	if (picker->lock_made) {
		kl_cond_destroy(&picker->wake);
		kl_mutex_destroy(&picker->lock);
		picker->lock_made = 0;
	}
}

/*
 * Tells the thread which items are in sight, so that it makes their
 * thumbnails first and no others.
 */
void
keiui_picker_want(
	struct keiui_picker *picker,
	size_t first,
	size_t last)
{
	/* Kept within the items. */
	if (last > picker->count)
		last = picker->count;
	if (first > last)
		first = last;

	/* No lock before init made it. */
	if (!picker->lock_made)
		return;

	/* The range the thread reads, and a wake when it changed. */
	kl_mutex_lock(&picker->lock);

	if (first != picker->want_first || last != picker->want_last) {
		picker->want_first = first;
		picker->want_last = last;
		kl_cond_signal(&picker->wake);
	}

	kl_mutex_unlock(&picker->lock);
}

/*
 * Takes the thumbnails the thread finished among the items in sight; reports
 * 1 while some in sight are still to be made (the view draws again).
 */
int
keiui_picker_collect(
	struct keiui_picker *picker,
	size_t first,
	size_t last)
{
	struct keiui_picker_item *item;
	size_t index;
	int pending;

	/* Kept within the items. */
	if (last > picker->count)
		last = picker->count;

	/* No lock before init made it. */
	if (!picker->lock_made)
		return 0;

	/* Each item in sight: a finished one is the view's from now on, unchanged by the thread. */
	pending = 0;
	kl_mutex_lock(&picker->lock);

	for (index = first; index < last; index++) {
		item = &picker->items[index];
		if (item->shown)
			continue;
		if (item->thumb == KEIUI_PICKER_THUMB_READY || item->thumb == KEIUI_PICKER_THUMB_ICON) {
			item->shown = item->thumb;
			continue;
		}

		/* Not made yet: the view draws again until it is. */
		pending = 1;
	}

	kl_mutex_unlock(&picker->lock);

	/* Reports whether another frame is wanted for thumbnails still to come. */
	return pending;
}

/*
 * Selects or deselects an item (with a limit of one, moves the selection to
 * it); a selection past the limit is refused with a message.
 */
void
keiui_picker_toggle(
	struct keiui_picker *picker,
	size_t index)
{
	struct keiui_picker_item *item;
	unsigned removed;
	size_t other;

	/* An answered picker, or an item that is not there, changes nothing. */
	if (picker->answered || index >= picker->count)
		return;
	item = &picker->items[index];
	picker->cursor = (long)index;
	picker->message[0] = '\0';

	/* A single choice moves to the item clicked; clicking it again keeps it. */
	if (picker->limit == 1U) {
		for (other = 0U; other < picker->count; other++)
			picker->items[other].order = 0U;
		item->order = 1U;
		picker->selected = 1U;
		return;
	}

	/* A selected item leaves the selection, and the later ones close up their numbers. */
	if (item->order != 0U) {
		removed = item->order;
		item->order = 0U;
		picker->selected--;
		model_renumber(picker, removed);
		return;
	}

	/* The limit refuses one more. */
	if (picker->selected >= picker->limit) {
		(void)snprintf(picker->message, sizeof(picker->message), "You can choose up to %zu items.", picker->limit);
		return;
	}

	/* The item joins the selection after the others. */
	picker->selected++;
	item->order = (unsigned)picker->selected;
}

/*
 * Chooses an item at once (a double click): it joins the selection when it
 * can, and the selection is the answer.
 */
void
keiui_picker_choose(
	struct keiui_picker *picker,
	size_t index)
{
	/* An answered picker, or an item that is not there, changes nothing. */
	if (picker->answered || index >= picker->count)
		return;

	/* The item joins the selection unless it is in it already. */
	if (picker->items[index].order == 0U)
		keiui_picker_toggle(picker, index);

	/* A refused selection (the limit) leaves the picker open with its message. */
	if (picker->items[index].order == 0U)
		return;

	/* The selection is the answer. */
	keiui_picker_accept(picker);
}

/*
 * Answers with the selection in the order it was made; nothing happens while
 * nothing is selected.
 */
void
keiui_picker_accept(
	struct keiui_picker *picker)
{
	struct kl_media_item *answer;
	struct keiui_picker_item *item;
	size_t index;
	size_t slot;
	int error;
	int kind;

	/* Once, and only with something selected. */
	if (picker->answered || picker->selected == 0U)
		return;

	/* The answer's room. */
	answer = calloc(picker->selected, sizeof(answer[0]));
	if (answer == NULL) {
		(void)snprintf(picker->message, sizeof(picker->message), "Not enough memory to choose these items.");
		return;
	}

	/* Each selected item at its place in the order, its kind read from its file. */
	for (index = 0U; index < picker->count; index++) {
		item = &picker->items[index];
		if (item->order == 0U)
			continue;
		slot = (size_t)item->order - 1U;
		answer[slot].id = item->id;
		answer[slot].path = item->path;
		answer[slot].name = item->name;
		error = model_kind(item->path, &kind);
		answer[slot].video = 0;
		if (error == 0 && kind == KL_MEDIA_VIDEO)
			answer[slot].video = 1;
	}

	/* The answer, given once. */
	picker->answer = answer;
	picker->answer_count = picker->selected;
	picker->result = KL_MEDIA_PICKER_CHOSEN;
	picker->answered = 1;
}

/*
 * Answers that nothing was chosen.
 */
void
keiui_picker_cancel(
	struct keiui_picker *picker)
{
	/* Once. */
	if (picker->answered)
		return;

	/* No items. */
	picker->answer_count = 0U;
	picker->result = KL_MEDIA_PICKER_CANCELLED;
	picker->answered = 1;
}

/*
 * Writes the thumbnails' cache folder: $XDG_CACHE_HOME, else ~/.cache, then
 * keiland/photos.  Returns ENOENT when neither is known.
 */
int
keiui_picker_cache_folder(
	char *out,
	size_t size)
{
	const char *cache;
	const char *home;
	int length;

	/* $XDG_CACHE_HOME when it is an absolute path. */
	cache = getenv("XDG_CACHE_HOME");
	if (cache != NULL && cache[0] == '/') {
		length = snprintf(out, size, "%s/%s", cache, MODEL_CACHE);
		if (length < 0 || (size_t)length >= size)
			return ENAMETOOLONG;

		/* Succeeded: the folder named by the environment. */
		return 0;
	}

	/* Else ~/.cache. */
	home = getenv("HOME");
	if (home == NULL || home[0] != '/')
		return ENOENT;
	length = snprintf(out, size, "%s/.cache/%s", home, MODEL_CACHE);
	if (length < 0 || (size_t)length >= size)
		return ENAMETOOLONG;

	/* Succeeded: the folder in the home folder. */
	return 0;
}

/* Adds one media row's item: its identity, path, name and time taken. */
static int
model_add(
	struct keiui_picker *picker,
	char **fields)
{
	struct keiui_picker_item *grown;
	struct keiui_picker_item *item;
	size_t length;
	size_t clean;
	size_t room;

	/* The identity is 32 lowercase hexadecimal digits, as the library makes them. */
	length = strlen(fields[1]);
	clean = strspn(fields[1], "0123456789abcdef");
	if (length != KEIUI_PICKER_ID_SIZE - 1U || clean != length)
		return EPROTO;

	/* Room for one more, doubling. */
	if (picker->count == picker->capacity) {
		room = picker->capacity * 2U;
		if (room == 0U)
			room = 16U;
		grown = realloc(picker->items, room * sizeof(grown[0]));
		if (grown == NULL)
			return ENOMEM;
		picker->items = grown;
		picker->capacity = room;
	}

	/* The item, its strings its own. */
	item = &picker->items[picker->count];
	memset(item, 0, sizeof(*item));
	memcpy(item->id, fields[1], KEIUI_PICKER_ID_SIZE);
	item->path = strdup(fields[2]);
	if (item->path == NULL)
		return ENOMEM;
	item->name = strdup(fields[11]);
	if (item->name == NULL) {
		free(item->path);
		return ENOMEM;
	}

	/* When it was taken, which orders the grid. */
	item->taken = (int64_t)strtoll(fields[5], NULL, 10);
	picker->count++;

	/* Succeeded: the item is listed. */
	return 0;
}

/* Orders two items: the one taken later first, then by identity. */
static int
model_compare(
	const void *left,
	const void *right)
{
	const struct keiui_picker_item *first;
	const struct keiui_picker_item *second;
	int order;

	/* The later one first. */
	first = left;
	second = right;
	if (first->taken > second->taken)
		return -1;
	if (first->taken < second->taken)
		return 1;

	/* The same time: by identity, so that the order is stable. */
	order = strcmp(first->id, second->id);
	return order;
}

/* Releases every item and its thumbnail; the thread must not be running. */
static void
model_clear(
	struct keiui_picker *picker)
{
	size_t index;

	/* Each item's strings and square. */
	for (index = 0U; index < picker->count; index++) {
		free(picker->items[index].path);
		free(picker->items[index].name);
		kl_image_release(&picker->items[index].image);
	}

	/* The array, and nothing selected. */
	free(picker->items);
	picker->items = NULL;
	picker->count = 0U;
	picker->capacity = 0U;
	picker->selected = 0U;
	picker->cursor = -1L;
}

/* The thread: makes the thumbnail of each item in sight that has none, until told to stop. */
static void
model_thread(
	void *data)
{
	struct keiui_picker *picker;
	struct kl_image image;
	size_t index;
	int found;
	int video;
	int thumb;

	/* Runs until keiui_picker_fini stops it. */
	picker = data;
	for (;;) {
		/* Waits for an item in sight without a thumbnail, and claims it. */
		kl_mutex_lock(&picker->lock);

		found = model_next(picker, &index);
		while (!found && !picker->stop) {
			kl_cond_wait(&picker->wake, &picker->lock);
			found = model_next(picker, &index);
		}

		/* Told to stop: ends with the lock released. */
		if (picker->stop) {
			kl_mutex_unlock(&picker->lock);
			break;
		}

		/* Claimed: the view sees it as being made. */
		picker->items[index].thumb = KEIUI_PICKER_THUMB_LOADING;

		kl_mutex_unlock(&picker->lock);

		/* Makes it without the lock: reading and decoding may take long. */
		model_make(picker, index, &image, &video, &thumb);

		/* Publishes it; the view takes it on its next frame. */
		kl_mutex_lock(&picker->lock);

		picker->items[index].image = image;
		picker->items[index].video = video;
		picker->items[index].thumb = thumb;

		kl_mutex_unlock(&picker->lock);
	}
}

/* Finds an item in sight not looked at yet, under the lock: 1 with its index, 0 when none. */
static int
model_next(
	struct keiui_picker *picker,
	size_t *index)
{
	size_t at;

	/* The range the view wants, in order. */
	for (at = picker->want_first; at < picker->want_last; at++) {
		if (picker->items[at].thumb == KEIUI_PICKER_THUMB_NONE) {
			*index = at;
			return 1;
		}
	}

	/* Every item in sight has its thumbnail or is being made. */
	return 0;
}

/* Makes one item's thumbnail on the thread: its kind, then the cache, then the application's decoder. */
static void
model_make(
	struct keiui_picker *picker,
	size_t index,
	struct kl_image *image,
	int *video,
	int *thumb)
{
	const struct keiui_picker_item *item;
	int error;
	int kind;

	/* Nothing made yet: the kind's icon. */
	memset(image, 0, sizeof(*image));
	*video = 0;
	*thumb = KEIUI_PICKER_THUMB_ICON;
	item = &picker->items[index];

	/* The file's kind from its first bytes; a video or an unreadable file keeps its icon. */
	error = model_kind(item->path, &kind);
	if (error != 0)
		return;
	if (kind == KL_MEDIA_VIDEO) {
		*video = 1;
		return;
	}

	/* A file of no kind the desktop decodes keeps the photo's icon. */
	if (kind == KL_MEDIA_NONE)
		return;

	/* The shared cache first. */
	error = model_cache_read(picker, item->id, image);
	if (error == 0) {
		*thumb = KEIUI_PICKER_THUMB_READY;
		return;
	}

	/* Without the application's decoder the photo keeps its icon. */
	if (picker->thumbnail == NULL)
		return;

	/* The application's decoder makes the square. */
	error = picker->thumbnail(picker->thumbnail_data, item->path, KL_MEDIA_PICKER_THUMBNAIL, image);
	if (error != 0) {
		kl_image_release(image);
		memset(image, 0, sizeof(*image));
		return;
	}

	/* A square of another size is refused rather than drawn stretched. */
	if (image->width != KL_MEDIA_PICKER_THUMBNAIL || image->height != KL_MEDIA_PICKER_THUMBNAIL) {
		kl_image_release(image);
		memset(image, 0, sizeof(*image));
		return;
	}

	/* Kept for the next time, here and in Photos. */
	model_cache_write(picker, item->id, image);
	*thumb = KEIUI_PICKER_THUMB_READY;
}

/* Reads a file's first bytes and tells its kind (KL_MEDIA_*); an errno value when it cannot be read. */
static int
model_kind(
	const char *path,
	int *kind)
{
	unsigned char head[MODEL_HEAD];
	ssize_t count;
	int descriptor;

	/* Opens the library's copy without blocking on anything that is not a file. */
	descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	if (descriptor < 0)
		return errno;

	/* Its first bytes. */
	count = pread(descriptor, head, sizeof(head), 0);
	(void)close(descriptor);
	if (count < 0)
		return EIO;

	/* Succeeded: the kind the header tells. */
	*kind = kl_media_kind(head, (size_t)count);
	return 0;
}

/* Reads a thumbnail from the cache (Photos' P6 square); ENOENT when it is not there or not one. */
static int
model_cache_read(
	const struct keiui_picker *picker,
	const char *id,
	struct kl_image *image)
{
	unsigned char *bytes;
	char path[KEIUI_PICKER_PATH_MAX + 64U];
	char header[32];
	size_t want;
	size_t index;
	size_t at;
	ssize_t got;
	int length;
	int error;
	int same;
	int descriptor;

	/* The file in the cache. */
	if (picker->cache[0] == '\0')
		return ENOENT;
	length = snprintf(path, sizeof(path), "%s/%s.ppm", picker->cache, id);
	if (length < 0 || (size_t)length >= sizeof(path))
		return ENOENT;
	descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	if (descriptor < 0)
		return ENOENT;

	/* Its header is exactly the one the cache writes. */
	length = snprintf(header, sizeof(header), "P6\n%d %d\n255\n", KL_MEDIA_PICKER_THUMBNAIL, KL_MEDIA_PICKER_THUMBNAIL);
	want = (size_t)length + (size_t)KL_MEDIA_PICKER_THUMBNAIL * (size_t)KL_MEDIA_PICKER_THUMBNAIL * 3U;
	bytes = malloc(want);
	if (bytes == NULL) {
		(void)close(descriptor);
		return ENOMEM;
	}

	/* The whole file, which must be exactly the header and the square. */
	got = read(descriptor, bytes, want);
	(void)close(descriptor);
	same = -1;
	if (got == (ssize_t)want)
		same = memcmp(bytes, header, (size_t)length);
	if (same != 0) {
		free(bytes);
		return ENOENT;
	}

	/* The square's memory. */
	error = kl_image_create(image, KL_MEDIA_PICKER_THUMBNAIL, KL_MEDIA_PICKER_THUMBNAIL);
	if (error != 0) {
		free(bytes);
		return error;
	}

	/* Each pixel, opaque, from its three colours. */
	for (index = 0U; index < (size_t)KL_MEDIA_PICKER_THUMBNAIL * (size_t)KL_MEDIA_PICKER_THUMBNAIL; index++) {
		at = (size_t)length + index * 3U;
		image->pixels[(index / KL_MEDIA_PICKER_THUMBNAIL) * image->stride + index % KL_MEDIA_PICKER_THUMBNAIL] =
		    0xff000000U | ((uint32_t)bytes[at] << 16) | ((uint32_t)bytes[at + 1U] << 8) | (uint32_t)bytes[at + 2U];
	}

	/* Succeeded: the cached square. */
	free(bytes);
	return 0;
}

/* Keeps a thumbnail in the cache: written to a file of its own, then renamed over the old one; a failure keeps nothing. */
static void
model_cache_write(
	const struct keiui_picker *picker,
	const char *id,
	const struct kl_image *image)
{
	unsigned char *bytes;
	char path[KEIUI_PICKER_PATH_MAX + 64U];
	char temporary[KEIUI_PICKER_PATH_MAX + 64U];
	uint32_t pixel;
	size_t size;
	size_t index;
	size_t at;
	ssize_t wrote;
	int length;
	int status;
	int error;
	int descriptor;

	/* A cache folder, made when it is not there yet. */
	if (picker->cache[0] == '\0')
		return;
	error = model_folders(picker->cache);
	if (error != 0)
		return;

	/* The final name and a private one beside it. */
	length = snprintf(path, sizeof(path), "%s/%s.ppm", picker->cache, id);
	if (length < 0 || (size_t)length >= sizeof(path))
		return;
	length = snprintf(temporary, sizeof(temporary), "%s/.%s-XXXXXX", picker->cache, id);
	if (length < 0 || (size_t)length >= sizeof(temporary))
		return;

	/* The bytes: the header and each pixel's three colours. */
	size = 32U + (size_t)KL_MEDIA_PICKER_THUMBNAIL * (size_t)KL_MEDIA_PICKER_THUMBNAIL * 3U;
	bytes = malloc(size);
	if (bytes == NULL)
		return;
	length = snprintf((char *)bytes, 32U, "P6\n%d %d\n255\n", KL_MEDIA_PICKER_THUMBNAIL, KL_MEDIA_PICKER_THUMBNAIL);
	for (index = 0U; index < (size_t)KL_MEDIA_PICKER_THUMBNAIL * (size_t)KL_MEDIA_PICKER_THUMBNAIL; index++) {
		pixel = image->pixels[(index / KL_MEDIA_PICKER_THUMBNAIL) * image->stride + index % KL_MEDIA_PICKER_THUMBNAIL];
		at = (size_t)length + index * 3U;
		bytes[at] = (unsigned char)(pixel >> 16);
		bytes[at + 1U] = (unsigned char)(pixel >> 8);
		bytes[at + 2U] = (unsigned char)pixel;
	}

	/* The bytes written: the header as long as it came out, then the pixels. */
	size = (size_t)length + (size_t)KL_MEDIA_PICKER_THUMBNAIL * (size_t)KL_MEDIA_PICKER_THUMBNAIL * 3U;

	/* The private file. */
	descriptor = mkstemp(temporary);
	if (descriptor < 0) {
		free(bytes);
		return;
	}

	/* Written whole, else the private file goes. */
	wrote = write(descriptor, bytes, size);
	free(bytes);
	status = close(descriptor);
	if (wrote != (ssize_t)size || status != 0) {
		(void)unlink(temporary);
		return;
	}

	/* In place of the old one; a refused rename leaves no private file behind. */
	status = rename(temporary, path);
	if (status != 0)
		(void)unlink(temporary);
}

/* Makes each folder of a path that is not there yet (mkdir -p); an errno value when one cannot be made. */
static int
model_folders(
	const char *path)
{
	char folder[KEIUI_PICKER_PATH_MAX];
	char *slash;
	int length;
	int status;

	/* A copy to cut at each slash. */
	length = snprintf(folder, sizeof(folder), "%s", path);
	if (length < 0 || (size_t)length >= sizeof(folder))
		return ENAMETOOLONG;

	/* Each folder from the top, the last one included. */
	slash = strchr(folder + 1, '/');
	while (slash != NULL) {
		*slash = '\0';
		status = mkdir(folder, 0700);
		if (status != 0 && errno != EEXIST)
			return errno;
		*slash = '/';
		slash = strchr(slash + 1, '/');
	}

	/* The last folder. */
	status = mkdir(folder, 0700);
	if (status != 0 && errno != EEXIST)
		return errno;

	/* Succeeded: the folders are there. */
	return 0;
}

/* Closes up the selection's numbers after one left it. */
static void
model_renumber(
	struct keiui_picker *picker,
	unsigned removed)
{
	size_t index;

	/* Each later number moves down by one. */
	for (index = 0U; index < picker->count; index++) {
		if (picker->items[index].order > removed)
			picker->items[index].order--;
	}
}
