/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The file chooser's parts that know nothing of Wayland (ws090-p006, from
 * libkeiland's chooser of ws092-p003): the model -- the folder listed, the
 * places, the filters, the name typed, up to the answer -- and the view,
 * which draws a frame of the window with the widgets and carries out what
 * they report.  chooser.c puts them in a window; the host tests call them
 * directly.  Nothing here leaves the library.
 */

#ifndef KEIUI_CHOOSER_H
#define KEIUI_CHOOSER_H

#include "internal.h"

#include <keiland/keiland.h>

#include <stddef.h>
#include <stdint.h>

/* The longest path the chooser handles, with its NUL. */
#define KEIUI_CHOOSER_PATH_MAX		4096

/* The longest label of a place or a filter, and of a filter's extensions, with the NUL. */
#define KEIUI_CHOOSER_LABEL_MAX		64
#define KEIUI_CHOOSER_EXTENSIONS_MAX	256

/* The most places in the sidebar, and the most recent files shown. */
#define KEIUI_CHOOSER_PLACES_MAX	8
#define KEIUI_CHOOSER_RECENT_MAX	64

/* The longest message shown, with its NUL. */
#define KEIUI_CHOOSER_MESSAGE_MAX	192

/* The window's size unless the compositor says otherwise, and the smallest it may be. */
#define KEIUI_CHOOSER_WIDTH		760
#define KEIUI_CHOOSER_HEIGHT		480
#define KEIUI_CHOOSER_MIN_WIDTH		520
#define KEIUI_CHOOSER_MIN_HEIGHT	340

/* The widgets' ids in the chooser's frame. */
#define KEIUI_CHOOSER_ID_PLACES		1U
#define KEIUI_CHOOSER_ID_UP		2U
#define KEIUI_CHOOSER_ID_LOCATION	3U
#define KEIUI_CHOOSER_ID_PATH		4U
#define KEIUI_CHOOSER_ID_LIST		5U
#define KEIUI_CHOOSER_ID_NAME		6U
#define KEIUI_CHOOSER_ID_FILTER		7U
#define KEIUI_CHOOSER_ID_CANCEL		8U
#define KEIUI_CHOOSER_ID_ACCEPT		9U
#define KEIUI_CHOOSER_ID_CONFIRM	10U

/* The index a list gives itself among its records (list.c's). */
#define KEIUI_CHOOSER_LIST_SELF		0xffffffffU

/*
 * One item of the list: a folder or a file of the folder shown, or a
 * recent file (whose whole path is its path).  The name and path are the
 * item's own allocations.
 */
struct keiui_chooser_entry {
	char *name;
	char *path;
	int folder;
	int64_t size;
	int64_t modified;
};

/* One place of the sidebar: its label, its icon, and its folder (empty for Recent). */
struct keiui_chooser_place {
	char label[KEIUI_CHOOSER_LABEL_MAX];
	char path[KEIUI_CHOOSER_PATH_MAX];
	enum kl_icon icon;
};

/* One filter: its label and its extensions (empty: every file). */
struct keiui_chooser_filter {
	char label[KEIUI_CHOOSER_LABEL_MAX];
	char extensions[KEIUI_CHOOSER_EXTENSIONS_MAX];
};

/*
 * One file chooser's state, from kl_file_chooser_open to its destruction.
 * The entries array belongs to it and is refilled whenever another folder
 * is shown; the list holds the selection and the scroll.  answered becomes
 * 1 once, with result and answer; nothing changes after that.
 */
struct keiui_chooser {
	unsigned mode;
	char title[KEIUI_CHOOSER_LABEL_MAX];

	/* The folder shown (a real path), or Recent when recent is 1; why it could not be read. */
	char folder[KEIUI_CHOOSER_PATH_MAX];
	int recent;
	int list_error;

	/* Its items, the list (the selection and the scroll), and whether hidden items show. */
	struct keiui_chooser_entry *entries;
	size_t count;
	size_t capacity;
	struct kl_list list;
	int show_hidden;

	/* The sidebar and the filters. */
	struct keiui_chooser_place places[KEIUI_CHOOSER_PLACES_MAX];
	size_t place_count;
	struct keiui_chooser_filter filters[KL_FILE_CHOOSER_FILTERS_MAX];
	size_t filter_count;
	size_t filter;

	/* The name typed (Save), the path typed (Ctrl+L) while its field shows. */
	struct kl_field name;
	struct kl_field path;
	int typing_path;

	/* The widget that takes the keyboard in the next frame (0: none asked). */
	uint32_t want_focus;

	/* The question before a file is replaced, and the path it would replace. */
	int confirm;
	char confirm_path[KEIUI_CHOOSER_PATH_MAX];

	/* A line that says why something was refused (empty when none). */
	char message[KEIUI_CHOOSER_MESSAGE_MAX];

	/* The time of the frame (the list glides by it). */
	uint64_t now_us;

	/* The answer, once it is given. */
	int answered;
	unsigned result;
	char answer[KEIUI_CHOOSER_PATH_MAX];
};

/* The model (chooser-model.c). */
int keiui_chooser_init(struct keiui_chooser *chooser, const struct kl_file_chooser_options *options);
int keiui_chooser_media(struct keiui_chooser *chooser, int descriptor);
void keiui_chooser_fini(struct keiui_chooser *chooser);
int keiui_chooser_go(struct keiui_chooser *chooser, const char *path);
void keiui_chooser_go_recent(struct keiui_chooser *chooser);
void keiui_chooser_go_up(struct keiui_chooser *chooser);
void keiui_chooser_go_place(struct keiui_chooser *chooser, size_t place);
void keiui_chooser_select(struct keiui_chooser *chooser, long index);
void keiui_chooser_activate(struct keiui_chooser *chooser, long index);
void keiui_chooser_accept(struct keiui_chooser *chooser);
void keiui_chooser_accept_path(struct keiui_chooser *chooser);
void keiui_chooser_open_path(struct keiui_chooser *chooser);
void keiui_chooser_close_path(struct keiui_chooser *chooser);
void keiui_chooser_next_filter(struct keiui_chooser *chooser);
void keiui_chooser_toggle_hidden(struct keiui_chooser *chooser);
void keiui_chooser_type_select(struct keiui_chooser *chooser, uint32_t character);
void keiui_chooser_replace(struct keiui_chooser *chooser);
void keiui_chooser_keep(struct keiui_chooser *chooser);
void keiui_chooser_cancel(struct keiui_chooser *chooser);
int keiui_chooser_can_accept(const struct keiui_chooser *chooser);
int keiui_chooser_can_go_up(const struct keiui_chooser *chooser);
int keiui_chooser_is_place(const struct keiui_chooser *chooser, size_t place);
const char *keiui_chooser_home(void);

/* The view (chooser-view.c): one frame of the window at a size and a time, with its input (1: another frame is wanted); the glass panels under it. */
int keiui_chooser_frame(struct keiui_chooser *chooser, struct kl_ui *ui, const struct kl_style *style, int width, int height, uint64_t now_us);
size_t keiui_chooser_panels(int width, int height, struct kl_rect *panels, size_t capacity);

#endif
