/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The X server process's side of the X windows shown in mikuterm's tabs
 * (x11-tab-child.h): the backend that wraps the Windows backend, keeps a
 * copy of each tab's pixels with the part not yet sent, and turns
 * mikuterm's input into the core's.
 */

#include "x11-tab-child.h"

#include "userland/desktop/xserver/win32/scale.h"

#include <windows.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The bytes queued for mikuterm past which no more pixels are queued (the rest waits for the next round). */
#define TAB_CHILD_HIGH		(1024U * 1024U)

/* How long a window whose tab the user closed may stay before it opens in a tab again (ms). */
#define TAB_CHILD_REOPEN_MS	2000U

/* The most pixels one frame of pixels carries. */
#define TAB_CHILD_PART_MAX	((X11_LINK_BODY_MAX - X11_LINK_TAB_PIXELS_FIXED) / 4U)

/* The longest window's class kept for a window shown on its own after its tab was refused, with its NUL. */
#define TAB_CHILD_CLASS_MAX	256U

/* X's bits of the buttons 1 to 3 in the pointer's state. */
#define TAB_CHILD_BUTTONS	3U

/*
 * One X window's surface as the core sees it: a window of the Windows
 * backend (inner), or a tab (inner NULL) with what it shows and what is
 * still to be sent.
 */
struct tab_surface {
	struct x11_surface *inner;
	uint32_t window;
	unsigned kind;

	/* The window's place on the root window and its size (X's pixels). */
	int32_t x;
	int32_t y;
	unsigned width;
	unsigned height;

	/*
	 * A tab's: the part of the window shown (the window, cut to a tab's
	 * limits) and its pixels, the box of them not yet sent (damaged 0:
	 * none), and its title.
	 */
	unsigned shown_width;
	unsigned shown_height;
	uint32_t *pixels;
	struct x11_win32_box damage;
	int damaged;
	char title[X11_LINK_TAB_TITLE_MAX + 1U];

	/*
	 * Whether mikuterm was told of the tab (an opening sent), whether the
	 * user closed it and when, and whether the core was told the pointer
	 * came into the window.
	 */
	int opened;
	int closing;
	uint32_t closed_at;
	int entered;

	/*
	 * The window's shape (rectangles of its pixels from its corner;
	 * shaped 0: the whole window): a tab shows the window's background
	 * outside it, as a framed window does.
	 */
	int shaped;
	struct x11_rect *shape;
	unsigned shape_count;

	/* What a window shown on its own after its tab was refused opens with. */
	char class_name[TAB_CHILD_CLASS_MAX];
	int has_class;
	int default_place;
	unsigned flags;
	struct x11_size_hints hints;
};

/*
 * The tabs of one X server: the Windows backend they wrap, the core, the
 * link to mikuterm, the X server's scale, the tabs by their places, the
 * buttons and keys the tabs' input holds, and the frame being built.
 */
struct x11_tab_child {
	struct x11_win32 *inner;
	struct x11server *server;
	struct x11_tab_child_link link;
	unsigned scale;
	struct tab_surface *tabs[X11_LINK_TABS];
	uint16_t buttons;
	uint8_t held[X11_KEYMAP_KEYCODES];
	uint8_t body[X11_LINK_BODY_MAX];
};

static struct x11_surface *tab_child_open(void *context, uint32_t window, const struct x11_surface_info *info);
static void tab_child_update(void *context, struct x11_surface *surface, const struct x11_surface_info *info, unsigned changed);
static void tab_child_close(void *context, struct x11_surface *surface);
static int tab_child_vulkan(void *context, struct x11_surface *surface, void *instance, void *result);
static int tab_child_present(void *context, struct x11_surface *surface, const uint32_t *pixels, size_t stride, const struct x11_rect *parts, unsigned count);
static int tab_child_clipboard_offer(void *context, unsigned selection, const uint8_t *text, size_t size);
static int tab_child_clipboard_read(void *context, unsigned selection, uint8_t **text, size_t *size);
static void tab_child_capture(void *context, struct x11_surface *surface, int on);
static int tab_child_pointer(void *context, int *x, int *y);
static int tab_child_warp(void *context, int x, int y);
static void tab_child_bell(void *context, int percent);
static void tab_child_cursor(void *context, struct x11_surface *surface, const struct x11_cursor_image *image);
static int tab_child_wanted(const struct x11_surface_info *info);
static int tab_child_free_place(const struct x11_tab_child *tabs);
static struct tab_surface *tab_child_find(const struct x11_tab_child *tabs, uint32_t window);
static void tab_child_keep(struct tab_surface *surface, const struct x11_surface_info *info);
static void tab_child_keep_shape(struct tab_surface *surface, const struct x11_surface_info *info);
static void tab_child_outside(struct tab_surface *surface, int32_t row, int32_t x0, int32_t x1, uint32_t background);
static uint32_t tab_child_background(void);
static int tab_child_shown(struct tab_surface *surface);
static void tab_child_damage(struct tab_surface *surface, int32_t x0, int32_t y0, int32_t x1, int32_t y1);
static void tab_child_send_open(struct x11_tab_child *tabs, struct tab_surface *surface);
static void tab_child_send_title(struct x11_tab_child *tabs, struct tab_surface *surface);
static int tab_child_send_pixels(struct x11_tab_child *tabs, struct tab_surface *surface);
static void tab_child_input(struct x11_tab_child *tabs, struct tab_surface *surface, const struct x11_link_frame *frame, uint32_t now);
static void tab_child_key(struct x11_tab_child *tabs, uint32_t code, int pressed, uint32_t now);
static void tab_child_text(struct x11_tab_child *tabs, const uint8_t *text, size_t size, uint32_t now);
static void tab_child_button(struct x11_tab_child *tabs, struct tab_surface *surface, uint32_t value, int32_t x, int32_t y, uint32_t now);
static void tab_child_place(struct x11_tab_child *tabs, struct tab_surface *surface, int32_t x, int32_t y);
static void tab_child_release_keys(struct x11_tab_child *tabs, uint32_t now);
static void tab_child_refuse(struct x11_tab_child *tabs, struct tab_surface *surface);
static void tab_child_forget(struct x11_tab_child *tabs, struct tab_surface *surface);
static struct x11_surface *tab_child_inner(struct x11_surface *surface);

/* The functions of the backend that shows windows in mikuterm's tabs. */
const struct x11_backend_ops x11_tab_child_ops = {
	tab_child_open,
	tab_child_update,
	tab_child_close,
	tab_child_vulkan,
	tab_child_present,
	tab_child_clipboard_offer,
	tab_child_clipboard_read,
	tab_child_capture,
	tab_child_pointer,
	tab_child_warp,
	tab_child_bell,
	tab_child_cursor,
	X11_BACKEND_SHAPED_PRESENT
};

/*
 * Makes the tabs of an X server around its Windows backend, which goes on
 * showing every window not in a tab.  Returns 0, or ENOMEM.
 */
int
x11_tab_child_create(
	struct x11_win32 *inner,
	const struct x11_tab_child_link *link,
	struct x11_tab_child **result)
{
	struct x11_tab_child *tabs;

	/* No tab yet. */
	*result = NULL;
	tabs = calloc(1U, sizeof(*tabs));
	if (tabs == NULL)
		return ENOMEM;

	/* The backend wrapped, the link, and the X server's scale. */
	tabs->inner = inner;
	tabs->link = *link;
	tabs->scale = x11_win32_scale(inner);

	/* Succeeded: the tabs are made. */
	*result = tabs;
	return 0;
}

/* Gives the tabs the core they hand the input to. */
void
x11_tab_child_attach(
	struct x11_tab_child *tabs,
	struct x11server *server)
{
	/* The core, from now on. */
	tabs->server = server;
}

/* Ends the tabs, after the core (which closed every surface). */
void
x11_tab_child_destroy(
	struct x11_tab_child *tabs)
{
	unsigned place;

	/* A tab the core left open goes too. */
	for (place = 0U; place < X11_LINK_TABS; place++) {
		if (tabs->tabs[place] == NULL)
			continue;
		free(tabs->tabs[place]->pixels);
		free(tabs->tabs[place]);
	}
	free(tabs);
}

/*
 * Takes one of mikuterm's frames of a tab: the user's input in it, the
 * size it gives the window, its closing, its refusal.  A frame of a
 * window that has no tab (it crossed the window's end) is dropped.
 * Returns 0, or EPROTO for an input against the rules.
 */
int
x11_tab_child_take(
	struct x11_tab_child *tabs,
	const struct x11_link_frame *frame,
	uint32_t now)
{
	struct tab_surface *surface;
	uint32_t width;
	uint32_t height;
	unsigned kind;

	/* An input is a kind and three numbers, or the kind and a commit's text. */
	if (frame->type == X11_LINK_TAB_INPUT) {
		kind = x11_link_get32(frame->body);
		if (kind != X11_LINK_INPUT_TEXT && frame->size != X11_LINK_TAB_INPUT_BODY)
			return EPROTO;
	}

	/* The tab of the window, while it is one. */
	surface = tab_child_find(tabs, frame->id);
	if (surface == NULL || tabs->server == NULL)
		return 0;

	/* What it says. */
	switch (frame->type) {
	case X11_LINK_TAB_INPUT:
		/* Not while the window is asked to close. */
		if (!surface->closing)
			tab_child_input(tabs, surface, frame, now);
		break;
	case X11_LINK_TAB_CONFIGURE:
		/* The window at the tab's size, where it is (the root window keeps its own). */
		width = x11_link_get32(frame->body);
		height = x11_link_get32(frame->body + 4);
		if (surface->kind == X11_SURFACE_ROOT || surface->closing)
			break;
		if (width == 0U || height == 0U || width > X11_LINK_TAB_SIDE_MAX || height > X11_LINK_TAB_SIDE_MAX)
			break;
		x11server_backend_configure(tabs->server, surface->window, surface->x, surface->y, (int)width, (int)height);
		break;
	case X11_LINK_TAB_CLOSE:
		/* The window manager's close; a window still there a moment later opens in a tab again. */
		surface->closing = 1;
		surface->closed_at = now;
		surface->opened = 0;
		x11server_backend_close(tabs->server, surface->window);
		break;
	default:
		/* Refused: the window becomes one of the Windows backend's. */
		tab_child_refuse(tabs, surface);
		break;
	}

	/* Succeeded. */
	return 0;
}

/*
 * Sends mikuterm what the tabs have, while the link's queue has room:
 * each new tab's opening, a closed window still there opened again, and
 * the parts of the pixels not yet sent.
 */
void
x11_tab_child_send(
	struct x11_tab_child *tabs,
	uint32_t now)
{
	struct tab_surface *surface;
	unsigned place;
	size_t waiting;
	int error;

	/* Each tab. */
	for (place = 0U; place < X11_LINK_TABS; place++) {
		surface = tabs->tabs[place];
		if (surface == NULL)
			continue;

		/* A window the user closed that is still there after a moment opens in a tab again. */
		if (surface->closing && now - surface->closed_at > TAB_CHILD_REOPEN_MS)
			surface->closing = 0;
		if (surface->closing)
			continue;

		/* The tab's opening, once (its pixels all follow). */
		if (!surface->opened)
			tab_child_send_open(tabs, surface);

		/* Its pixels not yet sent, while the queue has room (a full queue leaves the rest for a later round). */
		while (surface->damaged) {
			waiting = tabs->link.waiting(tabs->link.context);
			if (waiting >= TAB_CHILD_HIGH)
				return;
			error = tab_child_send_pixels(tabs, surface);
			if (error != 0)
				return;
		}
	}
}

/*
 * Opens a surface for an X window: a tab for a top-level window no other
 * window owns and that is not cut to a shape (or the root window), while
 * the link has a tab free; a window of the Windows backend otherwise.
 */
static struct x11_surface *
tab_child_open(
	void *context,
	uint32_t window,
	const struct x11_surface_info *info)
{
	struct x11_tab_child *tabs;
	struct tab_surface *surface;
	struct x11_surface_info inner_info;
	int place;
	int wanted;
	int error;

	/* The surface's memory. */
	tabs = context;
	surface = calloc(1U, sizeof(*surface));
	if (surface == NULL)
		return NULL;
	surface->window = window;
	surface->kind = info->kind;
	tab_child_keep(surface, info);

	/* A window that is not a tab's: the Windows backend's, owned by the owner's window. */
	wanted = tab_child_wanted(info);
	place = tab_child_free_place(tabs);
	if (!wanted || place < 0) {
		inner_info = *info;
		inner_info.owner = tab_child_inner(info->owner);
		surface->inner = x11_win32_ops.open(tabs->inner, window, &inner_info);
		if (surface->inner == NULL) {
			free(surface);
			return NULL;
		}
		return (struct x11_surface *)surface;
	}

	/* A tab: its shape, and its pixels, black until the core presents them. */
	tab_child_keep_shape(surface, info);
	error = tab_child_shown(surface);
	if (error != 0) {
		free(surface->shape);
		free(surface);
		return NULL;
	}

	/* Succeeded: the tab opens in mikuterm with the next frames. */
	tabs->tabs[place] = surface;
	return (struct x11_surface *)surface;
}

/*
 * Brings a surface in step with what changed: the Windows backend's
 * window, or a tab's title, size and place (sent to mikuterm when the tab
 * is open there).
 */
static void
tab_child_update(
	void *context,
	struct x11_surface *surface,
	const struct x11_surface_info *info,
	unsigned changed)
{
	struct x11_tab_child *tabs;
	struct tab_surface *shown;
	struct x11_surface_info inner_info;
	uint8_t size[X11_LINK_TAB_SIZE_BODY];
	uint32_t background;
	int32_t row;
	int error;

	/* The Windows backend's window, its owner as the backend knows it. */
	tabs = context;
	shown = (struct tab_surface *)surface;
	if (shown->inner != NULL) {
		inner_info = *info;
		inner_info.owner = tab_child_inner(info->owner);
		x11_win32_ops.update(tabs->inner, shown->inner, &inner_info, changed);
		return;
	}

	/* What a window shown on its own later would open with. */
	tab_child_keep(shown, info);

	/* A new shape: kept, and the window's background outside it. */
	if ((changed & X11_SURFACE_CHANGED_SHAPE) != 0U) {
		tab_child_keep_shape(shown, info);
		background = tab_child_background();
		for (row = 0; row < (int32_t)shown->shown_height; row++)
			tab_child_outside(shown, row, 0, (int32_t)shown->shown_width, background);
		tab_child_damage(shown, 0, 0, (int32_t)shown->shown_width, (int32_t)shown->shown_height);
	}

	/*
	 * A window now cut to its shape (shaped, and its client asked for no
	 * decoration) leaves its tab for a window of its own.
	 */
	if (shown->kind == X11_SURFACE_TOPLEVEL &&
	    !shown->closing &&
	    shown->shaped &&
	    (shown->flags & X11_SURFACE_UNDECORATED) != 0U) {
		if (shown->opened) {
			shown->opened = 0;
			(void)tabs->link.frame(tabs->link.context, X11_LINK_TAB_GONE, shown->window, NULL, 0);
		}
		tab_child_refuse(tabs, shown);
		return;
	}

	/* A new title. */
	if ((changed & X11_SURFACE_CHANGED_TITLE) != 0U && shown->opened && !shown->closing)
		tab_child_send_title(tabs, shown);

	/* A new size: its pixels made again (black until presented), and mikuterm told. */
	if ((changed & X11_SURFACE_CHANGED_SIZE) != 0U) {
		error = tab_child_shown(shown);
		if (error != 0)
			return;
		if (shown->opened && !shown->closing) {
			x11_link_put32(size, shown->shown_width);
			x11_link_put32(size + 4, shown->shown_height);
			(void)tabs->link.frame(tabs->link.context, X11_LINK_TAB_SIZE, shown->window, size, sizeof(size));
		}
	}
}

/* Ends a surface: the Windows backend's window, or a tab (mikuterm closes it). */
static void
tab_child_close(
	void *context,
	struct x11_surface *surface)
{
	struct x11_tab_child *tabs;
	struct tab_surface *shown;

	/* The Windows backend's window. */
	tabs = context;
	shown = (struct tab_surface *)surface;
	if (shown->inner != NULL) {
		x11_win32_ops.close(tabs->inner, shown->inner);
		free(shown->shape);
		free(shown);
		return;
	}

	/* A tab mikuterm has: its end. */
	if (shown->opened)
		(void)tabs->link.frame(tabs->link.context, X11_LINK_TAB_GONE, shown->window, NULL, 0);

	/* The tab's place and memory. */
	tab_child_forget(tabs, shown);
	free(shown->pixels);
	free(shown->shape);
	free(shown);
}

/* Makes a Vulkan surface of the Windows backend's window; a tab has none (its pixels are presented). */
static int
tab_child_vulkan(
	void *context,
	struct x11_surface *surface,
	void *instance,
	void *result)
{
	struct x11_tab_child *tabs;
	struct tab_surface *shown;
	int error;

	/* A tab is shown through the present. */
	tabs = context;
	shown = (struct tab_surface *)surface;
	if (shown->inner == NULL)
		return ENOTSUP;

	/* The Windows backend's. */
	error = x11_win32_ops.vulkan_surface(tabs->inner, shown->inner, instance, result);
	if (error != 0)
		return error;

	/* Succeeded: the surface. */
	return 0;
}

/*
 * Shows pixels: the Windows backend's window shows them, or a tab copies
 * the parts that changed and sends them with the next frames.  Returns 0,
 * or the Windows backend's answer.
 */
static int
tab_child_present(
	void *context,
	struct x11_surface *surface,
	const uint32_t *pixels,
	size_t stride,
	const struct x11_rect *parts,
	unsigned count)
{
	struct x11_tab_child *tabs;
	struct tab_surface *shown;
	const struct x11_rect *part;
	unsigned index;
	int32_t x0;
	int32_t y0;
	int32_t x1;
	int32_t y1;
	int32_t row;
	uint32_t background;
	int status;

	/* The Windows backend's window. */
	tabs = context;
	shown = (struct tab_surface *)surface;
	if (shown->inner != NULL) {
		status = x11_win32_ops.present(tabs->inner, shown->inner, pixels, stride, parts, count);
		if (status != 0)
			return status;
		return 0;
	}

	/* Each part, cut to what the tab shows, copied (the window's background outside its shape) and marked to be sent. */
	background = 0U;
	if (shown->shaped)
		background = tab_child_background();
	for (index = 0U; index < count; index++) {
		part = &parts[index];
		x0 = part->x;
		y0 = part->y;
		x1 = part->x + (int32_t)part->width;
		y1 = part->y + (int32_t)part->height;
		if (x0 < 0)
			x0 = 0;
		if (y0 < 0)
			y0 = 0;
		if (x1 > (int32_t)shown->shown_width)
			x1 = (int32_t)shown->shown_width;
		if (y1 > (int32_t)shown->shown_height)
			y1 = (int32_t)shown->shown_height;
		if (x0 >= x1 || y0 >= y1)
			continue;

		/* The part's rows. */
		for (row = y0; row < y1; row++) {
			memcpy(shown->pixels + (size_t)row * shown->shown_width + (size_t)x0,
			       pixels + (size_t)row * stride + (size_t)x0,
			       (size_t)(x1 - x0) * sizeof(*pixels));
			if (shown->shaped)
				tab_child_outside(shown, row, x0, x1, background);
		}
		tab_child_damage(shown, x0, y0, x1, y1);
	}

	/* Succeeded: the tab's pixels are up to date. */
	return 0;
}

/* Makes an X client's text the system's clipboard (the Windows backend's). */
static int
tab_child_clipboard_offer(
	void *context,
	unsigned selection,
	const uint8_t *text,
	size_t size)
{
	struct x11_tab_child *tabs;
	int error;

	/* The Windows backend's clipboard. */
	tabs = context;
	error = x11_win32_ops.clipboard_offer(tabs->inner, selection, text, size);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Reads another program's text of the system's clipboard (the Windows backend's). */
static int
tab_child_clipboard_read(
	void *context,
	unsigned selection,
	uint8_t **text,
	size_t *size)
{
	struct x11_tab_child *tabs;
	int error;

	/* The Windows backend's clipboard. */
	tabs = context;
	error = x11_win32_ops.clipboard_read(tabs->inner, selection, text, size);
	if (error != 0)
		return error;

	/* Succeeded: the text. */
	return 0;
}

/*
 * Keeps the pointer on a window of the Windows backend while an X client
 * grabs it; a tab's pointer is mikuterm's, whose input comes here anyway.
 */
static void
tab_child_capture(
	void *context,
	struct x11_surface *surface,
	int on)
{
	struct x11_tab_child *tabs;
	struct tab_surface *shown;

	/* Only the Windows backend's windows (NULL lets go). */
	tabs = context;
	shown = (struct tab_surface *)surface;
	if (shown != NULL && shown->inner == NULL)
		return;
	x11_win32_ops.capture(tabs->inner, tab_child_inner(surface), on);
}

/* Tells where the system's pointer is on the root window (the Windows backend's). */
static int
tab_child_pointer(
	void *context,
	int *x,
	int *y)
{
	struct x11_tab_child *tabs;
	int error;

	/* The Windows backend's answer. */
	tabs = context;
	error = x11_win32_ops.pointer(tabs->inner, x, y);
	if (error != 0)
		return error;

	/* Succeeded: the place. */
	return 0;
}

/* Moves the system's pointer (the Windows backend's). */
static int
tab_child_warp(
	void *context,
	int x,
	int y)
{
	struct x11_tab_child *tabs;
	int error;

	/* The Windows backend's. */
	tabs = context;
	error = x11_win32_ops.warp(tabs->inner, x, y);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Rings the bell (the Windows backend's). */
static void
tab_child_bell(
	void *context,
	int percent)
{
	struct x11_tab_child *tabs;

	/* The Windows backend's. */
	tabs = context;
	x11_win32_ops.bell(tabs->inner, percent);
}

/* Sets the cursor over a window of the Windows backend; over a tab mikuterm shows its own. */
static void
tab_child_cursor(
	void *context,
	struct x11_surface *surface,
	const struct x11_cursor_image *image)
{
	struct x11_tab_child *tabs;
	struct tab_surface *shown;

	/* Only the Windows backend's windows. */
	tabs = context;
	shown = (struct tab_surface *)surface;
	if (shown == NULL || shown->inner == NULL)
		return;
	x11_win32_ops.cursor(tabs->inner, shown->inner, image);
}

/*
 * Tells whether a window is shown in a tab: the root window, or a
 * top-level window that no other window owns (not a dialog) and that is
 * not cut to a shape (one whose client asked for no decoration).  A
 * decorated shaped window is a tab, its background outside its shape.
 */
static int
tab_child_wanted(
	const struct x11_surface_info *info)
{
	/* The root window of a rooted server. */
	if (info->kind == X11_SURFACE_ROOT)
		return 1;

	/* Not a menu, a tooltip, a dialog or a window cut to its shape. */
	if (info->kind != X11_SURFACE_TOPLEVEL)
		return 0;
	if (info->owner != NULL)
		return 0;
	if (info->shaped && (info->flags & X11_SURFACE_UNDECORATED) != 0U)
		return 0;

	/* A plain top-level window. */
	return 1;
}

/* Gives a free place for a tab, or -1 when the link has as many as it may. */
static int
tab_child_free_place(
	const struct x11_tab_child *tabs)
{
	unsigned place;

	/* The first free one. */
	for (place = 0U; place < X11_LINK_TABS; place++) {
		if (tabs->tabs[place] == NULL)
			return (int)place;
	}

	/* None. */
	return -1;
}

/* Finds the tab of an X window (NULL: the window has none). */
static struct tab_surface *
tab_child_find(
	const struct x11_tab_child *tabs,
	uint32_t window)
{
	unsigned place;

	/* Looks through the tabs. */
	for (place = 0U; place < X11_LINK_TABS; place++) {
		if (tabs->tabs[place] != NULL && tabs->tabs[place]->window == window)
			return tabs->tabs[place];
	}

	/* None. */
	return NULL;
}

/* Keeps what a tab shows and what a window shown on its own later would open with. */
static void
tab_child_keep(
	struct tab_surface *surface,
	const struct x11_surface_info *info)
{
	size_t length;

	/* The place and the size. */
	surface->x = info->x;
	surface->y = info->y;
	surface->width = info->width;
	surface->height = info->height;

	/* The title, cut to a tab's longest (never inside a character). */
	length = 0U;
	if (info->title != NULL)
		length = strlen(info->title);
	if (length > X11_LINK_TAB_TITLE_MAX) {
		length = X11_LINK_TAB_TITLE_MAX;
		while (length > 0U && ((unsigned char)info->title[length] & 0xC0U) == 0x80U)
			length--;
	}
	if (length != 0U)
		memcpy(surface->title, info->title, length);
	surface->title[length] = '\0';

	/* The class, the state, the limits and whether the system places it. */
	surface->has_class = 0;
	if (info->class_name != NULL) {
		length = strlen(info->class_name);
		if (length < sizeof(surface->class_name)) {
			memcpy(surface->class_name, info->class_name, length + 1U);
			surface->has_class = 1;
		}
	}
	surface->flags = info->flags;
	surface->hints = info->hints;
	surface->default_place = info->default_place;
}

/* Keeps a copy of a tab's shape as the core gave it (none: the whole window). */
static void
tab_child_keep_shape(
	struct tab_surface *surface,
	const struct x11_surface_info *info)
{
	struct x11_rect *copy;

	/* The old shape goes. */
	free(surface->shape);
	surface->shape = NULL;
	surface->shape_count = 0U;
	surface->shaped = 0;

	/* Unshaped. */
	if (!info->shaped)
		return;

	/* Succeeded: the new one copied (a shape not kept shows the whole window). */
	copy = malloc(((size_t)info->shape_count + 1U) * sizeof(*copy));
	if (copy == NULL)
		return;
	if (info->shape_count != 0U)
		memcpy(copy, info->shape, (size_t)info->shape_count * sizeof(*copy));
	surface->shape = copy;
	surface->shape_count = info->shape_count;
	surface->shaped = 1;
}

/*
 * Paints the background into the pixels of one row of a tab, from x0 to
 * x1, that lie outside its shape: X leaves them undefined.
 */
static void
tab_child_outside(
	struct tab_surface *surface,
	int32_t row,
	int32_t x0,
	int32_t x1,
	uint32_t background)
{
	const struct x11_rect *rect;
	uint32_t *line;
	int32_t x;
	int32_t next;
	unsigned index;
	int covered;

	/* Run by run: one in a rectangle of the shape is skipped to its end, one outside them painted up to the next. */
	line = surface->pixels + (size_t)row * surface->shown_width;
	x = x0;
	while (x < x1) {
		/* The rectangle the run starts in, or the nearest one after it on the row. */
		covered = 0;
		next = x1;
		for (index = 0U; index < surface->shape_count && !covered; index++) {
			rect = &surface->shape[index];
			if (row < rect->y || row >= rect->y + (int32_t)rect->height) {
				/* Not on the row. */
			} else if (x >= rect->x && x < rect->x + (int32_t)rect->width) {
				/* The run is inside it. */
				covered = 1;
				next = rect->x + (int32_t)rect->width;
			} else if (rect->x > x && rect->x < next) {
				/* The run ends where it starts. */
				next = rect->x;
			}
		}

		/* A run outside the shape: the background. */
		if (!covered) {
			while (x < next) {
				line[x] = background;
				x++;
			}
		}

		x = next;
	}
}

/*
 * Gives the colour of a window's background as an X pixel: the system's,
 * as a framed window of the Windows backend shows outside its shape.
 */
static uint32_t
tab_child_background(void)
{
	COLORREF colour;
	uint32_t pixel;

	/* The system's colour (0x00BBGGRR). */
	colour = GetSysColor(COLOR_WINDOW);

	/* Succeeded: as X's 0x00RRGGBB. */
	pixel = ((uint32_t)GetRValue(colour) << 16) | ((uint32_t)GetGValue(colour) << 8) | (uint32_t)GetBValue(colour);
	return pixel;
}

/*
 * Makes a tab's pixels for the window's size, cut to a tab's limits: the
 * pixels both sizes have are kept, the rest black, and every pixel is to
 * be sent.  Returns 0, or ENOMEM (the tab is as it was).
 */
static int
tab_child_shown(
	struct tab_surface *surface)
{
	uint32_t *pixels;
	unsigned width;
	unsigned height;
	unsigned rows;
	unsigned columns;
	unsigned row;

	/* The size shown: the window within a tab's longest side and most pixels. */
	width = surface->width;
	if (width == 0U)
		width = 1U;
	if (width > X11_LINK_TAB_SIDE_MAX)
		width = X11_LINK_TAB_SIDE_MAX;
	height = surface->height;
	if (height == 0U)
		height = 1U;
	if (height > X11_LINK_TAB_SIDE_MAX)
		height = X11_LINK_TAB_SIDE_MAX;
	if (height > X11_LINK_TAB_AREA_MAX / width)
		height = X11_LINK_TAB_AREA_MAX / width;

	/* The same size: nothing to make. */
	if (surface->pixels != NULL && width == surface->shown_width && height == surface->shown_height)
		return 0;

	/* The pixels, black. */
	pixels = calloc((size_t)width * (size_t)height, sizeof(*pixels));
	if (pixels == NULL)
		return ENOMEM;

	/* The old ones where both sizes have them. */
	rows = surface->shown_height;
	if (rows > height)
		rows = height;
	columns = surface->shown_width;
	if (columns > width)
		columns = width;
	for (row = 0U; row < rows; row++)
		memcpy(pixels + (size_t)row * width, surface->pixels + (size_t)row * surface->shown_width, (size_t)columns * sizeof(*pixels));

	/* Succeeded: the new pixels, all to be sent. */
	free(surface->pixels);
	surface->pixels = pixels;
	surface->shown_width = width;
	surface->shown_height = height;
	surface->damaged = 0;
	tab_child_damage(surface, 0, 0, (int32_t)width, (int32_t)height);
	return 0;
}

/* Adds a box of a tab's pixels to the part to be sent (one box around them all). */
static void
tab_child_damage(
	struct tab_surface *surface,
	int32_t x0,
	int32_t y0,
	int32_t x1,
	int32_t y1)
{
	/* The first box is the part. */
	if (!surface->damaged) {
		surface->damage.x0 = x0;
		surface->damage.y0 = y0;
		surface->damage.x1 = x1;
		surface->damage.y1 = y1;
		surface->damaged = 1;
		return;
	}

	/* Another grows the part round both. */
	if (x0 < surface->damage.x0)
		surface->damage.x0 = x0;
	if (y0 < surface->damage.y0)
		surface->damage.y0 = y0;
	if (x1 > surface->damage.x1)
		surface->damage.x1 = x1;
	if (y1 > surface->damage.y1)
		surface->damage.y1 = y1;
}

/* Tells mikuterm of a tab: the size it shows, the X server's scale and the title; all its pixels follow. */
static void
tab_child_send_open(
	struct x11_tab_child *tabs,
	struct tab_surface *surface)
{
	size_t length;
	int error;

	/* The fixed part, then the title. */
	length = strlen(surface->title);
	x11_link_put32(tabs->body, surface->shown_width);
	x11_link_put32(tabs->body + 4, surface->shown_height);
	x11_link_put32(tabs->body + 8, tabs->scale);
	memcpy(tabs->body + X11_LINK_TAB_OPEN_FIXED, surface->title, length);
	error = tabs->link.frame(tabs->link.context, X11_LINK_TAB_OPEN, surface->window, tabs->body, (uint32_t)(X11_LINK_TAB_OPEN_FIXED + length));
	if (error != 0)
		return;

	/* Succeeded: the tab is open there, and every pixel is to be sent. */
	surface->opened = 1;
	surface->entered = 0;
	tab_child_damage(surface, 0, 0, (int32_t)surface->shown_width, (int32_t)surface->shown_height);
}

/* Sends a tab's new title. */
static void
tab_child_send_title(
	struct x11_tab_child *tabs,
	struct tab_surface *surface)
{
	size_t length;

	/* The title as it is (at most a tab's longest). */
	length = strlen(surface->title);
	(void)tabs->link.frame(tabs->link.context, X11_LINK_TAB_TITLE, surface->window, surface->title, (uint32_t)length);
}

/*
 * Sends the top rows of the part of a tab's pixels not yet sent, as many
 * as one frame carries; the part left shrinks by them.  Returns 0, or the
 * error of queueing the frame (the part stays).
 */
static int
tab_child_send_pixels(
	struct x11_tab_child *tabs,
	struct tab_surface *surface)
{
	uint8_t *to;
	const uint32_t *line;
	uint32_t width;
	uint32_t rows;
	uint32_t row;
	uint32_t column;
	int error;

	/* The part's width, and the rows a frame carries (at least one: a tab's widest row fits). */
	width = (uint32_t)(surface->damage.x1 - surface->damage.x0);
	rows = TAB_CHILD_PART_MAX / width;
	if (rows > (uint32_t)(surface->damage.y1 - surface->damage.y0))
		rows = (uint32_t)(surface->damage.y1 - surface->damage.y0);

	/* The rows' place and size, then their pixels. */
	x11_link_put32(tabs->body, (uint32_t)surface->damage.x0);
	x11_link_put32(tabs->body + 4, (uint32_t)surface->damage.y0);
	x11_link_put32(tabs->body + 8, width);
	x11_link_put32(tabs->body + 12, rows);
	to = tabs->body + X11_LINK_TAB_PIXELS_FIXED;
	for (row = 0U; row < rows; row++) {
		line = surface->pixels + (size_t)(surface->damage.y0 + (int32_t)row) * surface->shown_width + (size_t)surface->damage.x0;
		for (column = 0U; column < width; column++) {
			x11_link_put32(to, line[column]);
			to += 4;
		}
	}
	error = tabs->link.frame(tabs->link.context, X11_LINK_TAB_PIXELS, surface->window, tabs->body, X11_LINK_TAB_PIXELS_FIXED + rows * width * 4U);
	if (error != 0)
		return error;

	/* Succeeded: the part left is below them. */
	surface->damage.y0 += (int32_t)rows;
	if (surface->damage.y0 >= surface->damage.y1)
		surface->damaged = 0;
	return 0;
}

/* Gives the core one of the user's inputs in a tab: a key, text, the pointer, the focus, the pointer's leaving. */
static void
tab_child_input(
	struct x11_tab_child *tabs,
	struct tab_surface *surface,
	const struct x11_link_frame *frame,
	uint32_t now)
{
	unsigned kind;
	uint32_t first;
	int32_t x;
	int32_t y;

	/* The kind and the numbers. */
	kind = x11_link_get32(frame->body);
	first = 0U;
	x = 0;
	y = 0;
	if (kind != X11_LINK_INPUT_TEXT) {
		first = x11_link_get32(frame->body + 4);
		x = (int32_t)x11_link_get32(frame->body + 8);
		y = (int32_t)x11_link_get32(frame->body + 12);
	}

	/* What it is. */
	switch (kind) {
	case X11_LINK_INPUT_KEY:
		tab_child_key(tabs, first, x != 0, now);
		break;
	case X11_LINK_INPUT_TEXT:
		tab_child_text(tabs, frame->body + 4, frame->size - 4U, now);
		break;
	case X11_LINK_INPUT_POINTER:
		tab_child_button(tabs, surface, first, x, y, now);
		break;
	case X11_LINK_INPUT_FOCUS:
		/* The keyboard comes into the window, or leaves it with its keys let go. */
		if (first != 0U) {
			x11server_backend_enter(tabs->server, surface->window, 1);
		} else {
			tab_child_release_keys(tabs, now);
			x11server_backend_leave(tabs->server, surface->window, 1);
		}
		break;
	case X11_LINK_INPUT_LEAVE:
		/* The pointer left the tab for mikuterm's own. */
		if (surface->entered) {
			surface->entered = 0;
			x11server_backend_leave(tabs->server, surface->window, 0);
		}
		break;
	default:
		/* An input this X server does not know is dropped. */
		break;
	}
}

/*
 * Gives the core a key of a tab by its evdev code: a press of a key held
 * is X's repeat (a release and a press), and a release of a key not held
 * is dropped.  The Windows backend notes it for the clipboard's policy.
 */
static void
tab_child_key(
	struct x11_tab_child *tabs,
	uint32_t code,
	int pressed,
	uint32_t now)
{
	uint8_t keycode;

	/* A key X can name. */
	if (code == 0U || code + X11_KEYMAP_EVDEV_OFFSET >= X11_KEYMAP_KEYCODES)
		return;
	keycode = (uint8_t)(code + X11_KEYMAP_EVDEV_OFFSET);

	/* A release of a key not held went elsewhere. */
	if (!pressed && !tabs->held[keycode])
		return;

	/* A held key's repeat is released first, as X repeats. */
	if (pressed && tabs->held[keycode])
		x11server_backend_key(tabs->server, keycode, 0, now);

	/* The key, held or not from now on, and noted. */
	tabs->held[keycode] = (uint8_t)pressed;
	x11_win32_note(tabs->inner, X11_WIN32_NOTE_KEY, code, pressed, now);
	x11server_backend_key(tabs->server, keycode, pressed, now);
}

/* Gives the core the characters an input method committed in a tab (UTF-8; a broken byte is passed over). */
static void
tab_child_text(
	struct x11_tab_child *tabs,
	const uint8_t *text,
	size_t size,
	uint32_t now)
{
	size_t offset;
	size_t length;
	size_t index;
	uint32_t character;
	uint8_t first;

	/* Each character in turn. */
	offset = 0U;
	while (offset < size) {
		first = text[offset];

		/* The length its first byte says, and its first bits. */
		length = 0U;
		character = 0U;
		if (first < 0x80U) {
			length = 1U;
			character = first;
		} else if (first >= 0xC2U && first <= 0xDFU) {
			length = 2U;
			character = first & 0x1FU;
		} else if (first >= 0xE0U && first <= 0xEFU) {
			length = 3U;
			character = first & 0x0FU;
		} else if (first >= 0xF0U && first <= 0xF4U) {
			length = 4U;
			character = first & 0x07U;
		}

		/* A byte that starts no character, or a character cut short. */
		if (length == 0U || length > size - offset) {
			offset++;
			continue;
		}

		/* The rest of its bits; a byte that does not continue it breaks it. */
		for (index = 1U; index < length; index++) {
			if ((text[offset + index] & 0xC0U) != 0x80U)
				break;
			character = (character << 6) | (text[offset + index] & 0x3FU);
		}
		if (index != length) {
			offset++;
			continue;
		}

		/* The character, unless a control one. */
		offset += length;
		if (character >= 0x20U && character != 0x7FU)
			x11server_backend_text(tabs->server, character, now);
	}
}

/*
 * Gives the core the pointer in a tab: its place on the root window (the
 * window's place and the place on it), and at most one button that went
 * down or up.  The window's place follows the screen when the pointer
 * comes in or a button goes down, and the core hears that the pointer
 * came in (which raises the window, so that its hits find it).
 */
static void
tab_child_button(
	struct x11_tab_child *tabs,
	struct tab_surface *surface,
	uint32_t value,
	int32_t x,
	int32_t y,
	uint32_t now)
{
	struct x11_pointer_frame frame;
	unsigned button;
	int pressed;
	uint16_t bit;

	/* The button and whether it went down; the place within a tab's longest side. */
	button = value & 0xFFU;
	pressed = 0;
	if ((value & X11_LINK_INPUT_PRESSED) != 0U)
		pressed = 1;
	if (button > 9U)
		return;
	if (x < -(int32_t)X11_LINK_TAB_SIDE_MAX || x > (int32_t)X11_LINK_TAB_SIDE_MAX)
		return;
	if (y < -(int32_t)X11_LINK_TAB_SIDE_MAX || y > (int32_t)X11_LINK_TAB_SIDE_MAX)
		return;

	/* The pointer coming in, or a press: the window where its picture is, raised. */
	if (!surface->entered || (button != 0U && pressed)) {
		tab_child_place(tabs, surface, x, y);
		surface->entered = 1;
		x11server_backend_enter(tabs->server, surface->window, 0);
	}

	/* The bit of buttons 1 to 3 (the others have none in the state). */
	bit = 0U;
	if (button >= 1U && button <= TAB_CHILD_BUTTONS)
		bit = (uint16_t)(1U << (button - 1U));

	/* The frame on the root window (the root window's own place is its corner). */
	memset(&frame, 0, sizeof(frame));
	frame.x = (int)x;
	frame.y = (int)y;
	if (surface->kind != X11_SURFACE_ROOT) {
		frame.x = (int)(surface->x + x);
		frame.y = (int)(surface->y + y);
	}
	frame.time = now;
	frame.buttons_before = tabs->buttons;
	frame.button = (uint8_t)button;
	frame.pressed = pressed;
	if (button != 0U && pressed) {
		tabs->buttons |= bit;
	} else if (button != 0U) {
		tabs->buttons &= (uint16_t)~bit;
	}
	frame.buttons_after = tabs->buttons;

	/* Noted for the clipboard's policy, and the core hears it. */
	if (button != 0U)
		x11_win32_note(tabs->inner, X11_WIN32_NOTE_BUTTON, button, pressed, now);
	x11server_backend_pointer(tabs->server, &frame);
}

/*
 * Puts a tab's window where its picture is on the screen: the system's
 * pointer on the root window less its place on the window.  The root
 * window stays where it is.
 */
static void
tab_child_place(
	struct x11_tab_child *tabs,
	struct tab_surface *surface,
	int32_t x,
	int32_t y)
{
	int pointer_x;
	int pointer_y;
	int error;

	/* The root window has no place but its own. */
	if (surface->kind == X11_SURFACE_ROOT)
		return;

	/* The system's pointer on the root window. */
	error = x11_win32_ops.pointer(tabs->inner, &pointer_x, &pointer_y);
	if (error != 0)
		return;

	/* The window's place under it; nothing to tell when it is there. */
	if ((int32_t)pointer_x - x == surface->x && (int32_t)pointer_y - y == surface->y)
		return;
	surface->x = (int32_t)pointer_x - x;
	surface->y = (int32_t)pointer_y - y;
	x11server_backend_configure(tabs->server, surface->window, surface->x, surface->y, (int)surface->width, (int)surface->height);
}

/* Lets go of every key a tab's input holds, as releases (the tab lost the keyboard). */
static void
tab_child_release_keys(
	struct x11_tab_child *tabs,
	uint32_t now)
{
	unsigned keycode;

	/* Each key held. */
	for (keycode = 0U; keycode < X11_KEYMAP_KEYCODES; keycode++) {
		if (!tabs->held[keycode])
			continue;
		tabs->held[keycode] = 0;
		x11_win32_note(tabs->inner, X11_WIN32_NOTE_KEY, keycode - X11_KEYMAP_EVDEV_OFFSET, 0, now);
		x11server_backend_key(tabs->server, (uint8_t)keycode, 0, now);
	}
}

/*
 * Shows a window mikuterm had no tab for as a window of the Windows
 * backend: opened with what the tab kept, and given its pixels.  A window
 * the backend cannot open is asked to close.
 */
static void
tab_child_refuse(
	struct x11_tab_child *tabs,
	struct tab_surface *surface)
{
	struct x11_surface_info info;
	struct x11_rect part;

	/* The window as the core last described it, with its shape, without an owner or an icon. */
	memset(&info, 0, sizeof(info));
	info.kind = surface->kind;
	info.x = surface->x;
	info.y = surface->y;
	info.default_place = surface->default_place;
	info.width = surface->width;
	info.height = surface->height;
	info.title = surface->title;
	info.class_name = NULL;
	if (surface->has_class)
		info.class_name = surface->class_name;
	info.owner = NULL;
	info.flags = surface->flags;
	info.hints = surface->hints;
	info.shaped = surface->shaped;
	info.shape = surface->shape;
	info.shape_count = surface->shape_count;

	/* The tab's place is free; the window is the backend's from now on. */
	tab_child_forget(tabs, surface);
	surface->inner = x11_win32_ops.open(tabs->inner, surface->window, &info);
	if (surface->inner == NULL) {
		x11server_backend_close(tabs->server, surface->window);
		return;
	}

	/* Its pixels as the tab had them (the core presents the next ones itself). */
	part.x = 0;
	part.y = 0;
	part.width = surface->shown_width;
	part.height = surface->shown_height;
	if (surface->width == surface->shown_width && surface->height == surface->shown_height)
		(void)x11_win32_ops.present(tabs->inner, surface->inner, surface->pixels, surface->shown_width, &part, 1U);
	free(surface->pixels);
	surface->pixels = NULL;
}

/* Frees a tab's place (the surface stays). */
static void
tab_child_forget(
	struct x11_tab_child *tabs,
	struct tab_surface *surface)
{
	unsigned place;

	/* Its place. */
	for (place = 0U; place < X11_LINK_TABS; place++) {
		if (tabs->tabs[place] == surface)
			tabs->tabs[place] = NULL;
	}
}

/* Gives the Windows backend's window of a surface (NULL for none, or for a tab). */
static struct x11_surface *
tab_child_inner(
	struct x11_surface *surface)
{
	struct tab_surface *shown;

	/* None. */
	if (surface == NULL)
		return NULL;

	/* The backend's window, NULL for a tab. */
	shown = (struct tab_surface *)surface;
	return shown->inner;
}
