/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The file chooser, part of <keiland/keiland.h>.
 */

#ifndef KEILAND_FILE_CHOOSER_H
#define KEILAND_FILE_CHOOSER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wl_display;
struct xdg_toplevel;

/*
 * The file chooser (libkeiland's own file chooser of KL_VERSION 12, moved
 * here and made of the widgets): the Open and Save As window every
 * application shares.  It shows the folders and files of a folder, the
 * sidebar's places (Recent, Home and its usual folders, Computer), the filter
 * chosen, and in Save mode takes a name and asks before a file is replaced.
 * The answer comes once, through the listener, while the application
 * dispatches its default Wayland queue; the application then destroys the
 * chooser.  The application keeps running meanwhile, and should take no input
 * of its own until the answer comes.
 *
 * The chooser is a window of its own on the application's connection, with
 * its own wl_seat objects.  Wayland sends a client's pointer, keyboard and
 * touch events to all of its objects of a seat, so an application ignores
 * the enter, key and touch events of surfaces that are not its own (as
 * kl_window does).
 */
struct kl_file_chooser;

/* What the chooser asks for: an existing file to open, or a folder and a name to save as. */
#define KL_FILE_CHOOSER_OPEN		0U
#define KL_FILE_CHOOSER_SAVE		1U
/* Lists the compositor media library; originals are accessed by their file paths. */
#define KL_FILE_CHOOSER_MEDIA 2U
/* Selects a directory; double clicking a directory still navigates into it. */
#define KL_FILE_CHOOSER_FOLDER 3U

/* How it ended: a path was chosen, or the user cancelled. */
#define KL_FILE_CHOOSER_CHOSEN		0U
#define KL_FILE_CHOOSER_CANCELLED	1U

/* The most filters one chooser offers. */
#define KL_FILE_CHOOSER_FILTERS_MAX	16U

/*
 * One filter: the label it is shown by, and the file name extensions it
 * shows, separated by spaces and without their dots ("txt md c h"),
 * compared without regard to case.  NULL or empty extensions show every
 * file.  Folders are always shown.
 */
struct kl_file_filter {
	const char *label;
	const char *extensions;
};

/*
 * What a chooser starts with.  Any pointer may be NULL.
 *
 * mode: KL_FILE_CHOOSER_OPEN, _SAVE, _MEDIA or _FOLDER.  title: the window's title ("Open"
 * or "Save As" when NULL).  application: the app_id the window gets, so
 * that compositor shows it as the application's.  folder: where it starts
 * (the home folder when NULL or not a folder).  name: the name Save starts
 * with, selected up to its extension.  filters, filter_count and filter:
 * the filters offered (at most KL_FILE_CHOOSER_FILTERS_MAX) and the one
 * chosen first; without filters every file is shown.  font and
 * fallback_font: the interface's font and the one for characters it lacks
 * (the system's when NULL).
 */
/* _MEDIA lists compositor-managed photos/videos and requires no filters; folder/name are ignored. */
struct kl_file_chooser_options {
	unsigned mode;
	const char *title;
	const char *application;
	const char *folder;
	const char *name;
	const struct kl_file_filter *filters;
	size_t filter_count;
	size_t filter;
	const char *font;
	const char *fallback_font;
};

/*
 * What a chooser tells the application, once and last: how it ended
 * (KL_FILE_CHOOSER_*), the absolute path chosen (empty when cancelled),
 * and the filter chosen last.  In Save mode the user has already agreed to
 * replace a file that exists.  The chooser's window is closed by then; the
 * application destroys the chooser, from the callback or later.
 */
struct kl_file_chooser_listener {
	void (*done)(void *data, struct kl_file_chooser *chooser, unsigned result, const char *path, size_t filter);
};

/*
 * Opens a file chooser over an application's window (parent may be NULL)
 * on the application's connection.
 *
 * Returns NULL with errno set: EINVAL (an unknown mode, too many filters,
 * a filter number past them, no listener), ENOTSUP (a compositor without
 * wl_shm or xdg_wm_base), an errno value of opening the font, ENOMEM.
 */
struct kl_file_chooser *kl_file_chooser_open(struct wl_display *display, struct xdg_toplevel *parent, const struct kl_file_chooser_options *options, const struct kl_file_chooser_listener *listener, void *data);

/*
 * Closes a chooser; one still open closes without telling.
 */
void kl_file_chooser_destroy(struct kl_file_chooser *chooser);

#ifdef __cplusplus
}
#endif

#endif
