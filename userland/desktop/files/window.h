/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parts of files that speak to the compositor (WS131 p020: through
 * libkeiland's application and window): the window or the desktop's
 * surface and its input (window.c), drag and drop (dnd.c), the frames
 * shown (present.c), the menus (menu.c), the titlebar (titlebar.c) and the
 * glass (glass.c).  The host tests build the rest of the program without
 * them.
 */

#ifndef FILES_WINDOW_H
#define FILES_WINDOW_H

#include "files.h"
#include "touch.h"

#define VK_USE_PLATFORM_WAYLAND_KHR 1
#include <vulkan/vulkan.h>
#include <keiland/keiland.h>

/* How many inputs wait for the main loop at most. */
#define FM_WINDOW_EVENTS	256U

/* How many touch inputs wait for the main loop at most (ws081-p010). */
#define FM_WINDOW_TOUCHES	256U

/*
 * The actions of the titlebar's controls: FM_TITLEBAR_ACTION plus the
 * control's ID, apart from the menus' (FM_ACTION_*), so that a control
 * chosen goes to the titlebar's queue.
 */
#define FM_TITLEBAR_ACTION	0x10000U

/* The actions of a context menu's rows: FM_CONTEXT_ACTION plus the row's action, so that their states are their own (menu.c). */
#define FM_CONTEXT_ACTION	0x20000U

struct fm_titlebar;
struct fm_menu;

/*
 * The window or the desktop's surface: libkeiland's application and
 * window, the connection and the surface they own (borrowed), the titlebar
 * and the menus its controls' and context menu's inputs go to, the size
 * and the state the compositor gave, the pointer's state, the touch inputs
 * and the input waiting for the interface, and drag and drop's state.
 *
 * One lives for the whole run.
 */
struct fm_window {
	struct kl_app *app;
	struct kl_window *kui;
	struct wl_display *display;
	struct wl_surface *surface;
	struct fm_titlebar *titlebar;
	struct fm_menu *menu;

	/* The touch inputs not yet taken by the main loop, oldest first (ws081-p010; a full queue drops the newest). */
	struct fm_touch_event touches[FM_WINDOW_TOUCHES];
	unsigned touch_count;

	/*
	 * Drag and drop (dnd.c, ws035-p084): whether this window's own drag is
	 * under way, whether a drag is over the window, and the serial a
	 * context menu after a drop answers (the drop's).
	 */
	int dragging;
	int drop_over;
	uint32_t drop_serial;

	/* Whether this is the desktop's surface (files --desktop, ws094-p003) instead of a window. */
	int desktop;

	/* The size the compositor asked for, and whether it changed since it was last taken. */
	uint32_t width;
	uint32_t height;
	int resized;

	/* Whether the compositor asked to close, the window has the focus, is maximized. */
	int closed;
	int activated;
	int maximized;

	/* The pointer's place, the serial of its last press, and the modifiers held (FM_MOD_*). */
	int pointer_x;
	int pointer_y;
	uint32_t button_serial;
	uint32_t modifiers;

	/* The inputs waiting, a ring: the oldest's slot and how many. */
	struct fm_event events[FM_WINDOW_EVENTS];
	unsigned event_first;
	unsigned event_count;
};

/*
 * The window's menus (menu.c): the window, whether it shows them (not
 * without the compositor's System Menu), the state they last showed and
 * whether it was sent, and whether the context menu closed.
 */
struct fm_menu {
	struct fm_window *window;
	int shown_once;
	struct fm_menu_state shown;
	int sent;

	/* Whether the context menu closed since it opened (an "ask" dismissed gives its drop up, main.c). */
	int context_done;
};

/* How many things done with the titlebar wait for the main loop at most. */
#define FM_TITLEBAR_EVENTS	16U

/*
 * The window's titlebar (titlebar.c): the window, whether its controls are
 * shown, the state it last showed (and whether it was ever sent), and what
 * the user did with it and the main loop has not yet carried out, oldest
 * first.
 */
struct fm_titlebar {
	struct fm_window *window;
	int controls;
	struct fm_titlebar_state shown;
	int sent;
	struct fm_titlebar_event events[FM_TITLEBAR_EVENTS];
	unsigned event_count;
};

/*
 * The frames shown in the window (present.c, libkeiland's presenter): the
 * window, the size frames are drawn at, whether zdesktop blends the frame
 * by its premultiplied alpha, the call that failed last, and the last
 * frame's copy, acquire, record and submit, present and wait times
 * (milliseconds).
 */
struct fm_present {
	struct fm_window *window;
	VkExtent2D extent;
	int premultiplied;
	const char *operation;
	unsigned copy_ms;
	unsigned acquire_ms;
	unsigned submit_ms;
	unsigned present_ms;
	unsigned wait_ms;
};

/* The window (window.c). */
int fm_window_open(struct fm_window *window, const char *display, uint32_t width, uint32_t height, const char *title, const char *application);
int fm_window_open_desktop(struct fm_window *window, const char *display, const char *token);
int fm_window_dispatch(struct fm_window *window, int timeout);
int fm_window_take(struct fm_window *window, struct fm_event *event);
void fm_window_close(struct fm_window *window);
void fm_window_action(struct fm_window *window, uint32_t action);
void fm_window_minimize(struct fm_window *window);
void fm_window_zoom(struct fm_window *window);
struct fm_event *fm_window_push(struct fm_window *window, unsigned type);
uint64_t fm_clock(void);

/* Drag and drop (dnd.c). */
int fm_dnd_start(struct fm_window *window, char *const *paths, size_t count);
void fm_dnd_answer(struct fm_window *window, int accept, uint32_t preferred);
int fm_dnd_receive(struct fm_window *window, char ***paths, size_t *count);
void fm_dnd_finish(struct fm_window *window, uint32_t action);
void fm_dnd_abort(struct fm_window *window);

/* The menus (menu.c). */
int fm_menu_open(struct fm_menu *menu, struct fm_window *window, const struct fm_menu_state *state);
void fm_menu_refresh(struct fm_menu *menu, const struct fm_menu_state *state);
void fm_menu_close(struct fm_menu *menu);
void fm_menu_context(struct fm_menu *menu, const struct fm_context *context, int x, int y, uint32_t context_serial);
void fm_menu_chosen(struct fm_menu *menu, const struct kl_window_event *event);

/* The window's titlebar in zdesktop (titlebar.c). */
int fm_titlebar_open(struct fm_titlebar *titlebar, struct fm_window *window, const struct fm_titlebar_state *state);
void fm_titlebar_refresh(struct fm_titlebar *titlebar, const struct fm_titlebar_state *state);
void fm_titlebar_input(struct fm_titlebar *titlebar, const struct kl_window_event *event);
int fm_titlebar_take(struct fm_titlebar *titlebar, struct fm_titlebar_event *event);
void fm_titlebar_close(struct fm_titlebar *titlebar);

/*
 * The window's glass (glass.c): the window, whether it is glass, and the
 * panels it last sent.
 */
struct fm_glass {
	struct fm_window *window;
	int on;
	struct fm_panel shown[FM_PANELS];
	size_t shown_count;
	int sent;
};

/* The window's glass (glass.c). */
int fm_glass_open(struct fm_glass *glass, struct fm_window *window, const struct fm_present *present);
void fm_glass_refresh(struct fm_glass *glass, struct fm_app *app);
void fm_glass_close(struct fm_glass *glass);

/* The presenter (present.c). */
VkResult fm_present_instance(struct fm_present *present);
VkResult fm_present_open(struct fm_present *present, struct fm_window *window);
VkResult fm_present_resize(struct fm_present *present, uint32_t width, uint32_t height);
VkResult fm_present_frame(struct fm_present *present, const uint32_t *pixels, size_t stride, const struct kl_rect *part);
void fm_present_close(struct fm_present *present);

#endif
