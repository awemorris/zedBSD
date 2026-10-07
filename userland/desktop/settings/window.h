/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parts of Settings that speak to the compositor (WS131 p019: through
 * libkeiland's application and window): the window and its input
 * (window.c), the frames shown (present.c), the menus (menu.c), the
 * titlebar (titlebar.c) and the glass (glass.c), each as a table or a call
 * of libkeiland's.  The host tests build the rest of the program without
 * them.
 */

#ifndef SETTINGS_WINDOW_H
#define SETTINGS_WINDOW_H

#include "settings.h"

#define VK_USE_PLATFORM_WAYLAND_KHR 1
#include <vulkan/vulkan.h>
#include <keiland/keiland.h>

/* How many inputs wait for the main loop at most. */
#define SE_WINDOW_EVENTS	256U

/*
 * The actions of the titlebar's controls: SE_TITLEBAR_ACTION plus the
 * control's ID (below SE_ACTION_PAGE_FIRST, apart from the menus'), so
 * that a control chosen goes to the titlebar's queue.
 */
#define SE_TITLEBAR_ACTION	90U

struct se_titlebar;

/*
 * The window: libkeiland's application and window, the connection and the
 * surface they own (borrowed, for the system and the activation), the
 * titlebar its controls' inputs go to, the first screen's mode (About and
 * Display show it), the size and the state the compositor gave, the
 * pointer's state, and the input waiting for the interface.
 *
 * One lives for the whole run.
 */
struct se_window {
	struct kl_app *app;
	struct kl_window *kui;
	struct wl_display *display;
	struct wl_surface *surface;
	struct se_titlebar *titlebar;

	/* The first screen's current mode (0 while unknown; the refresh in millihertz). */
	int32_t output_width;
	int32_t output_height;
	int32_t output_refresh;

	/* The size the compositor asked for and whether it changed since taken; whether it asked to close and the window is maximized. */
	uint32_t width;
	uint32_t height;
	int resized;
	int closed;
	int maximized;

	/* The pointer's place, the serial of its last press, and the modifiers held (SE_MOD_*). */
	int pointer_x;
	int pointer_y;
	uint32_t button_serial;
	uint32_t modifiers;

	/* The finger taken as the pointer (-1 when none): a touch is a click where it lands and lifts. */
	int32_t touch_id;

	/* The fraction of a pixel of scrolling not yet given (a touch pad scrolls a unit at a time). */
	double axis_remainder;

	/* The inputs waiting, a ring: the oldest's slot and how many. */
	struct se_event events[SE_WINDOW_EVENTS];
	unsigned event_first;
	unsigned event_count;

	/*
	 * Another descriptor the wait wakes for, -1 for none: the one copy's
	 * socket, which a later start of Settings makes readable (ws089-p016),
	 * and whether the application watches it.
	 */
	int extra_fd;
	int extra_watched;
};

/*
 * The window's menus (menu.c): the window, whether it shows them (not
 * without the compositor's System Menu), the state they last showed, and
 * whether it was sent.
 */
struct se_menu {
	struct se_window *window;
	int shown_once;
	struct se_menu_state shown;
	int sent;
};

/*
 * The window's titlebar (titlebar.c): the window, whether its controls are
 * shown, the state it last showed (and whether it was ever sent), and what
 * the user did with it and the main loop has not yet carried out, oldest
 * first.
 */
struct se_titlebar {
	struct se_window *window;
	int controls;
	struct se_titlebar_state shown;
	int sent;
	struct se_titlebar_event events[SE_TITLEBAR_EVENTS];
	unsigned event_count;
};

/*
 * The frames shown in the window (present.c, libkeiland's presenter): the
 * window, the size frames are drawn at, the call that failed last, the
 * device's name (About shows it), whether the compositor blends the frame by its
 * premultiplied alpha, and the last frame's copy, acquire, present and wait
 * times (milliseconds).
 */
struct se_present {
	struct se_window *window;
	VkExtent2D extent;
	const char *operation;
	char device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
	int premultiplied;
	unsigned copy_ms;
	unsigned acquire_ms;
	unsigned present_ms;
	unsigned wait_ms;
};

/*
 * The window's glass (glass.c): the window, whether it is glass, and the
 * panels it last sent.
 */
struct se_glass {
	struct se_window *window;
	int on;
	struct se_panel shown[SE_PANELS];
	size_t shown_count;
	int sent;
};

/* The window (window.c). */
int se_window_open(struct se_window *window, const char *display, uint32_t width, uint32_t height, const char *title, const char *application);
int se_window_dispatch(struct se_window *window, int timeout);
int se_window_take(struct se_window *window, struct se_event *event);
void se_window_close(struct se_window *window);
void se_window_action(struct se_window *window, uint32_t action);
void se_window_minimize(struct se_window *window);
void se_window_zoom(struct se_window *window);
struct se_event *se_window_push(struct se_window *window, unsigned type);
uint64_t se_clock(void);

/* The menus (menu.c). */
int se_menu_open(struct se_menu *menu, struct se_window *window, const struct se_menu_state *state);
void se_menu_refresh(struct se_menu *menu, const struct se_menu_state *state);
void se_menu_close(struct se_menu *menu);

/* The window's titlebar in the compositor (titlebar.c). */
int se_titlebar_open(struct se_titlebar *titlebar, struct se_window *window, const struct se_titlebar_state *state);
void se_titlebar_refresh(struct se_titlebar *titlebar, const struct se_titlebar_state *state);
void se_titlebar_input(struct se_titlebar *titlebar, const struct kl_window_event *event);
int se_titlebar_take(struct se_titlebar *titlebar, struct se_titlebar_event *event);
void se_titlebar_close(struct se_titlebar *titlebar);

/* The window's glass (glass.c). */
int se_glass_open(struct se_glass *glass, struct se_window *window, const struct se_present *present);
void se_glass_refresh(struct se_glass *glass, struct se_app *app);
void se_glass_close(struct se_glass *glass);

/* The presenter (present.c). */
VkResult se_present_open(struct se_present *present, struct se_window *window);
VkResult se_present_resize(struct se_present *present, uint32_t width, uint32_t height);
VkResult se_present_frame(struct se_present *present, const uint32_t *pixels, size_t stride);
void se_present_close(struct se_present *present);

#endif
