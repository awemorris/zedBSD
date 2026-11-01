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
#include "userland/desktop/libkeiland/win32/wide.h"

#include <windows.h>

#include <stddef.h>
#include <stdint.h>

/* How many inputs a window keeps queued, and how many posts the application keeps. */
#define KEIUI_WIN32_EVENTS		512U

/* The title bar's height at 96 dpi. */
#define KEIUI_WIN32_CAPTION		32U

/* The code and the data a second start of the program sends its URLs with (WM_COPYDATA). */
#define KEIUI_WIN32_COPY_URLS		0x4F474D31U

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
 * (under post_lock, post_wake rung by the first), and whether the system
 * is in the dark appearance.
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
 * title bar's (a DIB section of the frame's width).
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
};

/* app.c */
uint64_t keiui_win32_next_order(struct kl_app *app);

/* window.c */
LRESULT CALLBACK keiui_win32_window_procedure(HWND handle, UINT message, WPARAM wparam, LPARAM lparam);
struct kl_window_event *keiui_win32_push(struct kl_window *window, unsigned kind);
int keiui_win32_take(struct kl_window *window, uint64_t *order, struct kl_window_event *input);
int keiui_win32_peek(const struct kl_window *window, uint64_t *order);
int keiui_win32_classes(struct kl_app *app);
void keiui_win32_classes_drop(struct kl_app *app);

/* present.c */
void keiui_win32_paint_content(struct kl_window *window);
void keiui_win32_paint_caption(struct kl_window *window);
void keiui_win32_present_release(struct kl_window *window);
uint32_t keiui_win32_caption_pixels(const struct kl_window *window);

/* theme.c */
int keiui_win32_dark(void);

#endif
