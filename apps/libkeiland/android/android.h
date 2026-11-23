/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the files of libkeiland's Android layer share: the application's
 * state and the functions the files call one another with.  A program does
 * not read this file (keiland-android.h is its).
 *
 * Everything here runs on M, the program's native thread, but the posts
 * (post_lock), which any thread makes.  Functions named android_ return 0 or
 * an error number (EINVAL, ENOMEM, EIO), as the files of this layer agree.
 */

#ifndef KEILAND_ANDROID_ANDROID_H
#define KEILAND_ANDROID_ANDROID_H

#include <keiland/keiland.h>
#include "apps/libkeiland/android/bridge.h"
#include "apps/libkeiland/android/keiland-android.h"

#include <android/native_window.h>

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

/* The insets' fields: the system bars' and the cutout's sides, and the keyboard's height. */
#define ANDROID_INSET_LEFT	0
#define ANDROID_INSET_TOP	1
#define ANDROID_INSET_RIGHT	2
#define ANDROID_INSET_BOTTOM	3
#define ANDROID_INSET_IME	4

/* A post of kl_app_post or kl_window_post that waits for the dispatch. */
struct android_post {
	struct android_post *next;
	struct kl_window *window;
	uint32_t code;
	void *data;
};

/*
 * The application: the events queued for the program (a ring only M touches),
 * the posts other threads made (the lock holds only them), the logical windows
 * (the first is the one shown), and what the system gave: the native window
 * and the input queue now, the insets in pixels, the scale and the language.
 */
struct kl_app {
	pthread_mutex_t lock;
	struct android_post *posts;
	struct android_post *posts_tail;
	struct kl_app_event *events;
	size_t event_head;
	size_t event_count;
	size_t event_capacity;
	struct kl_window *windows;
	ANativeWindow *native;
	AInputQueue *input;
	int32_t insets[5];
	unsigned scale;
	int focused;
	int started;
	char language[KL_TR_NAME_MAX + 1U];
	char application[64];
	struct kl_system *system;
};

/*
 * A logical window.  The system gives the program one window (the Activity's)
 * and the program may make several: the first of the application's list is
 * shown, with the size of the native window less the insets, and the others
 * wait behind it.
 */
struct kl_window {
	struct kl_window *next;
	struct kl_app *app;
	uint32_t width;
	uint32_t height;
	unsigned present;
	char *title;

	/* The last frame shown (the window's width by height words, no padding), for a new native window to be painted from. */
	uint32_t *shown;
	uint32_t shown_width;
	uint32_t shown_height;
	kl_window_resize_fn resize_callback;
	void *resize_data;
	int text_input;
};

/* app.c */

/* Returns the open application, or NULL when none is (one for the process). */
struct kl_app *android_app(void);

/* Appends an event to the application's ring (the ring grows).  Returns 0 or ENOMEM. */
int android_app_queue(struct kl_app *app, const struct kl_app_event *event);

/* Appends a window's input as a KL_APP_WINDOW event, timed now.  Returns 0 or ENOMEM. */
int android_window_queue(struct kl_window *window, const struct kl_window_event *input);

/* window.c */

/* Takes the system's window, makes the windows fit it. */
void android_window_attach(struct kl_app *app, ANativeWindow *native);

/* Lets the system's window go (the caller then answers keiland_android_done). */
void android_window_detach(struct kl_app *app);

/* Takes the insets (left, top, right, bottom, keyboard), makes the windows fit. */
void android_window_insets(struct kl_app *app, const int32_t insets[5]);

/* Fits the logical windows to the native window less the insets, telling the ones that changed. */
void android_window_layout(struct kl_app *app);

/* Returns 1 when the window is one of the application's open windows. */
int android_window_exists(const struct kl_app *app, const struct kl_window *window);

/* Returns the window on top (the one shown), or NULL. */
struct kl_window *android_window_top(struct kl_app *app);

/* present.c */

/*
 * Copies the parts of a frame of 0xAARRGGBB words (stride in words) into the
 * native window's buffer, as RGBA, at the insets' offset, and posts it.
 */
int android_present_copy(ANativeWindow *native, const int32_t insets[5], const uint32_t *pixels, size_t stride,
	uint32_t width, uint32_t height, const struct kl_rect *parts, unsigned count);

/* Paints the window on top again from the frame kept, when one of its size is there (a new native window has no picture). */
void android_present_repaint(struct kl_app *app);

/* Lets the frame kept go. */
void android_present_release(struct kl_window *window);

/* Turns count premultiplied 0xAARRGGBB words into R, G, B and an opaque A byte each. */
void android_pixels_row(uint8_t *out, const uint32_t *in, uint32_t count);

/* input.c */

/* Reads the input queue until it is empty, finishing each event. */
void android_input_drain(struct kl_app *app, AInputQueue *queue);

/* Turns a key event into a KL_WINDOW_KEY.  Returns 1 when it was taken, 0 to leave it to the system. */
int android_input_key(struct kl_app *app, const AInputEvent *event);

/* Turns a motion event into touches, motions, buttons and wheel axes.  Returns 1. */
int android_input_motion(struct kl_app *app, const AInputEvent *event);

/* Turns a key the key bar sent into a KL_WINDOW_KEY.  Returns 0 or ENOMEM. */
int android_input_bridge_key(struct kl_app *app, const struct keiland_bridge_event *event);

/* keys-table.c */

/* Returns the kl key code of an Android key, or 0 for none. */
uint32_t android_key_code(int32_t keycode, int32_t scancode);

/* Returns the KL_MOD_* bits of an Android meta state. */
unsigned android_key_modifiers(int32_t meta);

/* ime.c */

/* Turns the committed, composing and delete events of the input method into KL_WINDOW_TEXT_* and keys. */
void android_ime_bridge(struct kl_app *app, const struct keiland_bridge_event *event);

/* clipboard.c */

/* Keeps the clipboard's text the system told. */
void android_clipboard_bridge(struct kl_app *app, const struct keiland_bridge_event *event);

/* theme.c */

/* Takes the system's appearance, accent and language; tells the program of what changed. */
void android_theme_bridge(struct kl_app *app, const struct keiland_bridge_event *event);

/* titlebar.c */

/* Returns 1 when the strip of the window's titlebar took a press at (x, y) (pixels of the window). */
int android_titlebar_hit(struct kl_window *window, double x, double y, int pressed);

#endif /* KEILAND_ANDROID_ANDROID_H */
