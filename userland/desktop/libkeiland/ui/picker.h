/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The media picker's parts that know nothing of Wayland (ws197-p014): the
 * model -- the library's items from a metadata snapshot, the selection, the
 * thumbnails a thread of its own makes, up to the answer -- and the view,
 * which draws a frame of the window with the widgets and carries out what
 * they report.  picker.c puts them in a window; the host tests call them
 * directly.  Nothing here leaves the library.
 */

#ifndef KEIUI_PICKER_H
#define KEIUI_PICKER_H

#include "internal.h"

#include <keiland/keiland.h>

#include <stddef.h>
#include <stdint.h>

/* The length of a library identity, with its NUL. */
#define KEIUI_PICKER_ID_SIZE		33U

/* The longest title, and the longest message shown, with their NULs. */
#define KEIUI_PICKER_TITLE_MAX		64U
#define KEIUI_PICKER_MESSAGE_MAX	192U

/* The longest path of the thumbnails' cache folder, with its NUL. */
#define KEIUI_PICKER_PATH_MAX		4096U

/* The window's size unless the compositor says otherwise, and the smallest it may be. */
#define KEIUI_PICKER_WIDTH		720
#define KEIUI_PICKER_HEIGHT		520
#define KEIUI_PICKER_MIN_WIDTH		420
#define KEIUI_PICKER_MIN_HEIGHT		340

/* The widgets' ids in the picker's frame. */
#define KEIUI_PICKER_ID_GRID		1U
#define KEIUI_PICKER_ID_ITEMS		2U
#define KEIUI_PICKER_ID_CANCEL		3U
#define KEIUI_PICKER_ID_ACCEPT		4U

/* What an item's thumbnail is: not looked at yet, being made, made, or none (its kind's icon). */
#define KEIUI_PICKER_THUMB_NONE		0
#define KEIUI_PICKER_THUMB_LOADING	1
#define KEIUI_PICKER_THUMB_READY	2
#define KEIUI_PICKER_THUMB_ICON		3

/*
 * One item of the library: its identity, the path of the library's copy and
 * the name it was imported with (the item's own allocations), when it was
 * taken, and its place in the selection (0: not selected, else 1 for the
 * first chosen).
 *
 * The thread writes thumb, video and image under the picker's lock, once
 * for each item; the view reads them only after it saw the item made
 * (shown), when they no longer change.
 */
struct keiui_picker_item {
	char id[KEIUI_PICKER_ID_SIZE];
	char *path;
	char *name;
	int64_t taken;
	unsigned order;

	/* The thread's answer, under the lock: the thumbnail's state, whether it is a video, and the square. */
	int thumb;
	int video;
	struct kl_image image;

	/* The view's own copy of the state, taken under the lock once the thread is done with the item. */
	int shown;
};

/*
 * One media picker's state, from kl_media_picker_open to its destruction.
 * The items array is filled once from the snapshot and never moves after
 * the thread starts.  answered becomes 1 once, with result and the answer;
 * nothing changes after that.
 */
struct keiui_picker {
	char title[KEIUI_PICKER_TITLE_MAX];
	size_t limit;

	/* The items, how many are selected, and the item the keys move (-1: none). */
	struct keiui_picker_item *items;
	size_t count;
	size_t capacity;
	size_t selected;
	long cursor;

	/* The grid's scroll, its columns in the last frame, and the rows that showed then. */
	struct kl_scroll scroll;
	int scroll_made;
	int columns;
	size_t first;
	size_t last;

	/* A line that says why something was refused (empty when none). */
	char message[KEIUI_PICKER_MESSAGE_MAX];

	/*
	 * The library's list is being read (the window shows "Loading…"
	 * meanwhile, ws157-p014), or could not be read (the window says so).
	 */
	int loading;
	int unavailable;

	/* The answer, once it is given: the items chosen in their order (borrowing the items' strings). */
	int answered;
	unsigned result;
	struct kl_media_item *answer;
	size_t answer_count;

	/*
	 * The thumbnails' thread: the cache folder (empty: none), the
	 * application's decoder, and the range of items the view wants made.
	 * The lock guards want_first, want_last, stop and every item's thread
	 * fields; wake tells the thread the range changed or it must stop.
	 */
	char cache[KEIUI_PICKER_PATH_MAX];
	int (*thumbnail)(void *data, const char *path, int side, struct kl_image *image);
	void *thumbnail_data;
	struct kl_mutex lock;
	struct kl_cond wake;
	int lock_made;
	struct kl_thread *thread;
	int thread_made;
	int stop;
	size_t want_first;
	size_t want_last;
};

/* The model (picker-model.c). */
int keiui_picker_init(struct keiui_picker *picker, const struct kl_media_picker_options *options);
int keiui_picker_load(struct keiui_picker *picker, int descriptor);
int keiui_picker_start(struct keiui_picker *picker);
void keiui_picker_fini(struct keiui_picker *picker);
void keiui_picker_want(struct keiui_picker *picker, size_t first, size_t last);
int keiui_picker_collect(struct keiui_picker *picker, size_t first, size_t last);
void keiui_picker_toggle(struct keiui_picker *picker, size_t index);
void keiui_picker_choose(struct keiui_picker *picker, size_t index);
void keiui_picker_accept(struct keiui_picker *picker);
void keiui_picker_cancel(struct keiui_picker *picker);
int keiui_picker_cache_folder(char *out, size_t size);

/* The view (picker-view.c): one frame of the window at a size and a time, with its input (1: another frame is wanted); the glass panel of its card. */
int keiui_picker_frame(struct keiui_picker *picker, struct kl_ui *ui, const struct kl_style *style, int width, int height, uint64_t now_us);
size_t keiui_picker_panels(int width, int height, struct kl_rect *panels, size_t capacity);

#endif
