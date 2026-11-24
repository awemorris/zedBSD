/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libkeiland's Windows backend: the shapes of struct kl_app and struct
 * kl_window on Windows, private to the files of this directory (the
 * Wayland backend's ui/window.h is never included here).
 *
 * A window is two HWNDs: the frame (class "KeilandWindow": its border,
 * its shadow, the title bar that libkeiland draws, the hit test) and,
 * below the title bar, the content (class "KeilandContent", a child that
 * covers the rest of the frame's client area).  The program's drawing goes
 * to the content: its Vulkan surface, or the frames shown on the CPU.  The
 * window's size, a resize's size and the pointer's places are the
 * content's, as a Wayland window's are without the compositor's title bar.
 *
 * Everything here belongs to the application's thread, except the posts
 * (post_lock), which any thread makes.
 */

#ifndef KEIUI_WIN32_WINDOW_WIN32_H
#define KEIUI_WIN32_WINDOW_WIN32_H

/*
 * libkeiland's headers come before <windows.h>, whose macros (interface,
 * near) would rename their fields and parameters.
 */
#include <keiland/keiland.h>
#include "userland/desktop/libkeiland/platform/platform.h"
#include "apps/libkeiland/win32/frame.h"
#include "apps/libkeiland/win32/input.h"
#include "apps/libkeiland/win32/wide.h"

#include <windows.h>

#include <stddef.h>
#include <stdint.h>

/* How many inputs a window keeps queued, and how many posts the application keeps. */
#define KEIUI_WIN32_EVENTS		512U

/* The title bar's height at 96 dpi. */
#define KEIUI_WIN32_CAPTION		32U

/* The code and the data a second start of the program sends its URLs with (WM_COPYDATA). */
#define KEIUI_WIN32_COPY_URLS		0x4F474D31U

/* The most title bar controls a window keeps. */
#define KEIUI_WIN32_CONTROLS		16U

/* The most action states a window keeps (its menu's and its controls'). */
#define KEIUI_WIN32_ACTIONS		128U

/* The most items a window's menu (or a popup) has, and the most tabs its title bar shows. */
#define KEIUI_WIN32_MENU_ITEMS		256U
#define KEIUI_WIN32_TABS		64U

/* The longest title of a tab kept, with its NUL; a longer one is cut at a character. */
#define KEIUI_WIN32_TAB_TITLE		256U

/*
 * The places of the title bar the hit test calls the client: each control,
 * the menu's button, and the tab strip as one.
 */
#define KEIUI_WIN32_PLACES		(KEIUI_WIN32_CONTROLS + 2U)

/*
 * What of the title bar is under the pointer or pressed, beside a control
 * (whose index is below KEIUI_WIN32_CONTROLS): the menu's button, a tab
 * (its index added), a tab's close button (its index added), the new
 * tab's button, the arrows of a scrolling tab strip, and the button of the
 * list of the tabs out of sight.
 */
#define KEIUI_WIN32_MENU_BUTTON		0x10000U
#define KEIUI_WIN32_TAB_BODY		0x20000U
#define KEIUI_WIN32_TAB_CLOSE_BUTTON	0x30000U
#define KEIUI_WIN32_TAB_NEW_BUTTON	0x40000U
#define KEIUI_WIN32_TAB_BACK_BUTTON	0x50000U
#define KEIUI_WIN32_TAB_FORWARD_BUTTON	0x50001U
#define KEIUI_WIN32_TAB_LIST_BUTTON	0x50002U

/* The longest text of a control (a URL with its NUL), of a label, and of a placeholder. */
#define KEIUI_WIN32_CONTROL_TEXT	2048U
#define KEIUI_WIN32_CONTROL_LABEL	64U
#define KEIUI_WIN32_CONTROL_HINT	96U

/* The message a drag's start is posted to its frame with (drag.c). */
#define KEIUI_WIN32_DRAG_MESSAGE	(WM_APP + 1U)

/* A drag waiting for its start (drag.c). */
struct keiui_win32_drag;

/* A window's TSF (ime.c) and its touch pad's DirectManipulation (pointer.c). */
struct keiui_win32_tsf;
struct keiui_win32_manipulation;

/* No control (an index of none). */
#define KEIUI_WIN32_NO_CONTROL		0xffffffffU

/*
 * One title bar control (kl_window_set_controls): its entry's fields, its
 * text and placeholder and value (a text control's, a breadcrumb's), and
 * where the last layout put it (shown 0 when it gave way or is hidden).
 */
struct keiui_win32_control {
	uint32_t id;
	unsigned role;
	unsigned priority;
	unsigned group;
	char label[KEIUI_WIN32_CONTROL_LABEL];
	uint32_t action;
	char text[KEIUI_WIN32_CONTROL_TEXT];
	char placeholder[KEIUI_WIN32_CONTROL_HINT];
	unsigned value;
	int shown;
	int32_t x;
	int32_t y;
	uint32_t width;
	uint32_t height;
};

/*
 * One tab of a window's title bar (kl_window_set_tabs): its ID, its title
 * (UTF-8, cut to fit), its KL_TAB_* flags, and where the last layout put
 * it.
 */
struct keiui_win32_tab {
	uint32_t id;
	char title[KEIUI_WIN32_TAB_TITLE];
	unsigned flags;
	struct keiui_win32_tab_place place;
};

/*
 * The field a control is edited in (the location's box): the control's
 * index, the text being edited (UTF-8), the caret's and the selection's
 * other end's byte offsets, and the input method's text being composed
 * (UTF-8, with its cursor's byte offset).
 */
struct keiui_win32_field {
	unsigned control;
	char text[KEIUI_WIN32_CONTROL_TEXT];
	size_t caret;
	size_t anchor;
	char composing[KL_WINDOW_TEXT_MAX];
	size_t composing_caret;
};

/* One queued input of a window, with its place among the application's events. */
struct keiui_win32_input {
	uint64_t order;
	struct kl_window_event input;
};

/* One queued post of the application, with its place among the application's events. */
struct keiui_win32_posted {
	uint64_t order;
	struct kl_app_event event;
};

/* A post made from another thread, waiting for the application's dispatch. */
struct keiui_win32_post {
	struct kl_app_event event;
	struct keiui_win32_post *next;
};

/*
 * The application: its name (UTF-8, and as UTF-16 for the windows'
 * property), its module, its windows, its thread, the counter that orders
 * every event queued, its own events (posts), the posts other threads made
 * (under post_lock, post_wake rung by the first), whether the system
 * is in the dark appearance, whether OLE was started on its thread
 * (for a drag, drag.c), and the primary selection: Windows has none, so
 * it is the application's own, the text its windows last selected (NULL
 * before any; clipboard.c), shared by its windows on its thread.
 */
struct kl_app {
	char *application;
	WCHAR *application_wide;
	HINSTANCE module;
	struct kl_window *windows;
	keiui_thread_id thread;
	uint64_t order;
	struct keiui_win32_posted posts[KEIUI_WIN32_EVENTS];
	unsigned post_head;
	unsigned post_count;
	struct keiui_lock post_lock;
	struct keiui_wake post_wake;
	struct keiui_win32_post *post_first;
	struct keiui_win32_post *post_last;
	int dark;
	int ole;
	char *primary;
	size_t primary_length;
};

/*
 * A window: its application and the next of the application's windows,
 * the frame and the content, how frames are shown, the content's size as
 * last told (a RESIZE), the dpi, the states of the frame, the queued
 * inputs (the oldest motion goes when the queue is full; dropped counts
 * the inputs that found no room), the surrogate a WM_CHAR waits with, the
 * pointer's tracking and buttons held, the caption button under the
 * pointer and the one pressed, the modal size loop, the title (UTF-8),
 * the frame shown on the CPU (a DIB section of the content's size) and the
 * title bar's (a DIB section of the frame's width); the title bar's
 * controls, the action states, the control under the pointer and the one
 * pressed, the field being edited (editing 0 when none), the font the
 * title bar is written with (text_open 0 when it could not be opened), and
 * the text input: whether the program asked for it and its caret's
 * rectangle (the content's pixels); whether the program takes the pen
 * as a tablet (tablet, pointer.c); the drag waiting for its start
 * (NULL when none, drag.c); the program's callback for the sizes
 * during the modal size loop (NULL when none) with its data; the input
 * method's TSF (NULL when the window uses IMM32, ime.c); the touch pad's
 * DirectManipulation (NULL when the wheel's messages bring it, pointer.c);
 * whether the title bar is drawn over the system's backdrop (glass:
 * its ground left transparent, present.c); the menu (kl_window_set_menu:
 * its items, NULL when it has none) and its button's place; the tabs
 * (kl_window_set_tabs: NULL when there are none), their options, the
 * new tab's button's place and whether the program ever declared tabs (the
 * tabs' keys are then the window's even while it shows none); the tab
 * strip's scroll: the first tab a scrolling strip shows (the arrows' and
 * the wheel's, kept within the tabs by each layout), how the last layout
 * laid the strip out, whether the next layout with room brings the active
 * tab into sight (set when the active tab or the number of tabs changed,
 * or when the active tab was chosen again; the strip is then moved by the
 * least), the active tab's ID it was last brought to (0 for none), how
 * many tabs the last layout with room showed (a change brings the active
 * tab into sight), and the wheel's turn over the strip not yet a whole
 * notch (frame.c, keiui_win32_tab_wheel); the key
 * whose press went to the menu's shortcut or to the tabs or was a dead
 * key, so that its release is not given either (0 for none); and the
 * frame's style and placement before full screen.
 */
struct kl_window {
	struct kl_app *app;
	struct kl_window *next;
	HWND frame;
	HWND content;
	unsigned present;
	uint32_t width;
	uint32_t height;
	unsigned dpi;
	int maximized;
	int fullscreen;
	struct keiui_win32_input events[KEIUI_WIN32_EVENTS];
	unsigned event_head;
	unsigned event_count;
	uint64_t dropped;
	struct keiui_win32_surrogate surrogate;
	int tracking;
	int frame_tracking;
	unsigned buttons;
	unsigned hover;
	unsigned pressed;
	int sizing;
	int resize_waiting;
	char *title;
	HBITMAP shown;
	uint32_t *shown_pixels;
	uint32_t shown_width;
	uint32_t shown_height;
	HBITMAP caption;
	uint32_t *caption_pixels;
	uint32_t caption_width;
	uint32_t caption_height;
	struct keiui_win32_control controls[KEIUI_WIN32_CONTROLS];
	unsigned control_count;
	struct keiui_win32_rect control_places[KEIUI_WIN32_PLACES];
	unsigned control_place_count;
	struct keiui_win32_action actions[KEIUI_WIN32_ACTIONS];
	unsigned action_count;
	unsigned control_hover;
	unsigned control_pressed;
	int title_tracking;
	int editing;
	struct keiui_win32_field field;
	struct kl_text text;
	int text_open;
	int text_wanted;
	RECT text_caret;
	int tablet;
	struct keiui_win32_drag *drag;
	int dragging;
	unsigned drop_types;
	unsigned drop_offered;
	int drop_answered;
	unsigned drop_answer_actions;
	unsigned drop_answer_preferred;
	int32_t drop_x;
	int32_t drop_y;
	int drop_pending;
	int drop_error;
	unsigned drop_data_type;
	char *drop_data;
	size_t drop_length;
	void *drop_target;
	kl_window_resize_fn resize_callback;
	void *resize_data;
	struct keiui_win32_tsf *tsf;
	struct keiui_win32_manipulation *manipulation;
	int glass;
	struct keiui_win32_menu_item *menu;
	unsigned menu_count;
	struct keiui_win32_rect menu_place;
	struct keiui_win32_tab *tabs;
	unsigned tab_count;
	unsigned tab_options;
	struct keiui_win32_rect tab_new_place;
	int tabs_declared;
	unsigned tab_first;
	struct keiui_win32_tab_scroll tab_scroll;
	int tab_reveal;
	uint32_t tab_revealed;
	unsigned tab_shown_last;
	int tab_wheel;
	uint32_t eaten_key;
	LONG_PTR restore_style;
	WINDOWPLACEMENT restore_place;
};

/* app.c */
uint64_t keiui_win32_next_order(struct kl_app *app);
void keiui_win32_app_theme(struct kl_app *app);

/* window.c */
LRESULT CALLBACK keiui_win32_window_procedure(HWND handle, UINT message, WPARAM wparam, LPARAM lparam);
struct kl_window_event *keiui_win32_push(struct kl_window *window, unsigned kind);
int keiui_win32_take(struct kl_window *window, uint64_t *order, struct kl_window_event *input);
int keiui_win32_peek(const struct kl_window *window, uint64_t *order);
int keiui_win32_classes(struct kl_app *app);
void keiui_win32_classes_drop(struct kl_app *app);
void keiui_win32_window_theme(struct kl_window *window);

/* present.c */
void keiui_win32_paint_content(struct kl_window *window);
void keiui_win32_paint_caption(struct kl_window *window);
void keiui_win32_present_release(struct kl_window *window);
uint32_t keiui_win32_caption_pixels(const struct kl_window *window);

/* theme.c */
int keiui_win32_dark(void);
int keiui_win32_theme_reread(struct kl_app *app);

/* titlebar.c */
void keiui_win32_titlebar_layout(struct kl_window *window, uint32_t width, uint32_t height);
void keiui_win32_titlebar_draw(struct kl_window *window, struct kl_canvas *canvas, uint32_t width, uint32_t height);
unsigned keiui_win32_titlebar_at(const struct kl_window *window, int32_t x, int32_t y);
void keiui_win32_titlebar_press(struct kl_window *window, unsigned index);
void keiui_win32_titlebar_release(struct kl_window *window, unsigned index);
int keiui_win32_field_key(struct kl_window *window, uint32_t code, unsigned modifiers, const char *text);
void keiui_win32_field_insert(struct kl_window *window, const char *text);
void keiui_win32_field_compose(struct kl_window *window, const char *text, size_t caret);
void keiui_win32_field_end(struct kl_window *window, unsigned how);
void keiui_win32_titlebar_release_all(struct kl_window *window);
int keiui_win32_menu_shortcut(struct kl_window *window, uint32_t keysym, unsigned menu);
int keiui_win32_tab_key(struct kl_window *window, uint32_t code, unsigned held);
int keiui_win32_titlebar_wheel(struct kl_window *window, WPARAM wparam, LPARAM lparam, int horizontal);
void keiui_win32_titlebar_free(struct kl_window *window);

/* ime.c */
int keiui_win32_ime_message(struct kl_window *window, UINT message, WPARAM wparam, LPARAM lparam, LRESULT *result);
void keiui_win32_ime_place(struct kl_window *window);
void keiui_win32_tsf_open(struct kl_window *window);
void keiui_win32_tsf_close(struct kl_window *window);

/* drag.c */
void keiui_win32_drag_run(struct kl_window *window);
void keiui_win32_drag_drop(struct kl_window *window);

/* drop.c */
void keiui_win32_drop_close(struct kl_window *window);

/* pointer.c */
int keiui_win32_pointer_message(struct kl_window *window, UINT message, WPARAM wparam, LPARAM lparam);
void keiui_win32_manipulation_open(struct kl_window *window);
void keiui_win32_manipulation_close(struct kl_window *window);
int keiui_win32_manipulation_message(struct kl_window *window, UINT message, WPARAM wparam, LPARAM lparam);

/* clipboard.c */
int keiui_win32_clipboard_put(struct kl_window *window, const char *text, size_t length);
size_t keiui_win32_clipboard_get(struct kl_window *window, char *text, size_t size);

#endif
