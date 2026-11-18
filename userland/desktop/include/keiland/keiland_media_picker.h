/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The media picker, part of <keiland/keiland.h>.
 */

#ifndef KEILAND_MEDIA_PICKER_H
#define KEILAND_MEDIA_PICKER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct kl_image;
struct wl_display;
struct xdg_toplevel;

/*
 * The media picker (KL_VERSION 85): the window every application shares to
 * choose photos and videos of the user's media library, shown as a grid of
 * thumbnails.  Like the file chooser it is a window of its own on the
 * application's connection (a sheet over a parent where the compositor has
 * sheets), its input runs while the application dispatches its default queue,
 * and the answer comes once through the listener; the application then
 * destroys the picker.
 *
 * The list is the library's metadata from the compositor
 * (kl_system_media_list); the picker never reads the library's database.
 * The newest photo comes first.  A click selects or deselects an item, up
 * to limit items (one when 0; with one, a click moves the selection), and
 * a double click chooses the item at once; Add chooses the selection, in
 * the order it was made.
 *
 * Thumbnails are the desktop's shared cache ($XDG_CACHE_HOME/keiland/photos/
 * <id>.ppm, Photos' squares of KL_MEDIA_PICKER_THUMBNAIL pixels).  For a
 * photo not in it the picker calls thumbnail, when given, on a thread of
 * its own (so it must not touch the application's other state): it makes
 * a square of side pixels from the middle of the photo at path with
 * kl_image_create and returns 0, or an errno value; the picker keeps the
 * square in the cache and releases it.  Without thumbnail, or for a video,
 * an item shows its kind's icon.
 */
struct kl_media_picker;

/* How it ended: items were chosen, or the user cancelled. */
#define KL_MEDIA_PICKER_CHOSEN		0U
#define KL_MEDIA_PICKER_CANCELLED	1U

/* The side of a thumbnail, and the most items one answer carries. */
#define KL_MEDIA_PICKER_THUMBNAIL	160
#define KL_MEDIA_PICKER_LIMIT_MAX	64U

/*
 * One item chosen: the library's stable identity, the absolute path of
 * the library's copy, the name it was imported with, and 1 for a video.
 * The strings belong to the picker and last until the listener returns.
 */
struct kl_media_item {
	const char *id;
	const char *path;
	const char *name;
	int video;
};

/*
 * What a picker starts with.  Any pointer may be NULL.  title: the
 * window's title ("Photos & Videos" when NULL); application: the app_id the
 * window gets; limit: the most items chosen at once (0: one; at most
 * KL_MEDIA_PICKER_LIMIT_MAX); thumbnail and thumbnail_data: the photo
 * decoder described above; font and fallback_font: as the file chooser's.
 */
struct kl_media_picker_options {
	const char *title;
	const char *application;
	size_t limit;
	int (*thumbnail)(void *data, const char *path, int side, struct kl_image *image);
	void *thumbnail_data;
	const char *font;
	const char *fallback_font;
};

/*
 * What a picker tells the application, once and last: how it ended
 * (KL_MEDIA_PICKER_*) and the items chosen (none when cancelled).  The
 * window is closed by then; the application destroys the picker, from the
 * callback or later.
 */
struct kl_media_picker_listener {
	void (*done)(void *data, struct kl_media_picker *picker, unsigned result, const struct kl_media_item *items, size_t count);
};

/*
 * Opens a media picker over an application's window (parent may be NULL)
 * on the application's connection.
 *
 * Returns NULL with errno set: EINVAL (no listener, a limit too large),
 * ENOTSUP (a compositor without the media library, wl_shm or xdg_wm_base),
 * an errno value of the library's answer or of opening the font, ENOMEM.
 */
struct kl_media_picker *kl_media_picker_open(struct wl_display *display, struct xdg_toplevel *parent, const struct kl_media_picker_options *options, const struct kl_media_picker_listener *listener, void *data);

/*
 * Closes a picker; one still open closes without telling.  It waits for a
 * thumbnail being made.
 */
void kl_media_picker_destroy(struct kl_media_picker *picker);

#ifdef __cplusplus
}
#endif

#endif
