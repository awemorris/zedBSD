/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What a program on Android sees of libkeiland's own glue to the system: the
 * entry the system's NativeActivity loader reaches, and the events the
 * Activity's callbacks hand over to the program's one native thread.
 *
 * Three threads meet here.  U is the Activity's main thread, where the system
 * calls the callbacks (ANativeActivity_onCreate and the functions it sets).  M
 * is the program's native thread, made once for the process by
 * keiland_android_start and kept when an Activity goes away and a new one
 * comes, so the program's state outlives an Activity.  U never touches the
 * program's state: it queues events for M and wakes it.  The only things U
 * waits for are the three destructions (a window, an input queue, the
 * Activity), until M calls keiland_android_done, because the system takes the
 * object away when the callback returns.
 */

#ifndef KEILAND_ANDROID_H
#define KEILAND_ANDROID_H

#include <stddef.h>

#include <android/input.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>

/*
 * What happened to the Activity, as M sees it.
 *
 * WINDOW_CREATED and WINDOW_DESTROYED carry the window, INPUT_CREATED and
 * INPUT_DESTROYED the input queue; the other kinds carry neither.  The three
 * DESTROYED kinds must be answered with keiland_android_done.
 */
enum keiland_android_event_kind {
	KEILAND_ANDROID_WINDOW_CREATED,
	KEILAND_ANDROID_WINDOW_RESIZED,
	KEILAND_ANDROID_WINDOW_REDRAW,
	KEILAND_ANDROID_WINDOW_DESTROYED,
	KEILAND_ANDROID_INPUT_CREATED,
	KEILAND_ANDROID_INPUT_DESTROYED,
	KEILAND_ANDROID_START,
	KEILAND_ANDROID_RESUME,
	KEILAND_ANDROID_PAUSE,
	KEILAND_ANDROID_STOP,
	KEILAND_ANDROID_FOCUS_GAINED,
	KEILAND_ANDROID_FOCUS_LOST,
	KEILAND_ANDROID_CONFIG,
	KEILAND_ANDROID_LOW_MEMORY,
	KEILAND_ANDROID_ACTIVITY_DESTROYED
};

/*
 * One thing that happened.
 *
 * window is set for the WINDOW kinds and queue for the INPUT kinds; the
 * objects are the system's and stay valid until M answers the matching
 * DESTROYED event with keiland_android_done.
 */
struct keiland_android_event {
	enum keiland_android_event_kind kind;
	ANativeWindow *window;
	AInputQueue *queue;
};

/*
 * Starts the program's native thread the first time an Activity is created and
 * hooks the Activity's callbacks to the event queue.
 *
 * Called from ANativeActivity_onCreate, on U.  Each later Activity only
 * replaces the Activity the events come from.  main runs on M, once, and is
 * the program's body: it calls keiland_android_next in a loop.
 */
void keiland_android_start(ANativeActivity *activity, void *saved_state, size_t saved_state_size, void (*main)(void));

/*
 * Takes the next event, waiting at most timeout_ms (negative: without limit).
 *
 * Called on M.  Returns 1 with the event stored, or 0 when the time ran out.
 * While it waits, the input queue's events are handled: with no program to
 * take them they are given back unhandled, so the system does not report the
 * program as not responding.
 */
int keiland_android_next(int timeout_ms, struct keiland_android_event *event);

/*
 * Tells U that M has let go of what a DESTROYED event named.
 *
 * Called on M after it stops using the window or the input queue, or when the
 * Activity is destroyed.  For every other kind it does nothing.
 */
void keiland_android_done(const struct keiland_android_event *event);

/*
 * Returns the looper of M, for a program that waits for more than the events.
 *
 * Called on M.
 */
ALooper *keiland_android_looper(void);

/*
 * Returns the Activity the events come from now, or NULL when there is none.
 *
 * Called on M; the Activity can go away at any time, so the value is only for
 * a call that is made on the spot.
 */
ANativeActivity *keiland_android_activity(void);

#endif /* KEILAND_ANDROID_H */
