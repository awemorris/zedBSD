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
#include <stdint.h>

#include <jni.h>

#include <android/input.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>

/* The idents of the two things M's looper waits on besides its timeout. */
#define KEILAND_IDENT_WAKE	1
#define KEILAND_IDENT_INPUT	2

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
 * Takes the oldest queued event without waiting.
 *
 * Called on M.  Returns 1 with the event stored, or 0 when none is queued.  An
 * input queue is joined to M's looper (KEILAND_IDENT_INPUT) as it is handed
 * out and let go as its destruction is handed out; the input events themselves
 * are for the caller to read from the looper.
 */
int keiland_android_take(struct keiland_android_event *event);

/*
 * Wakes M's looper (KEILAND_IDENT_WAKE) so that it looks at what was queued.
 *
 * Called on any thread.  Does nothing while M has no wake-up descriptor yet; M
 * looks at its queues before it sleeps, so nothing is lost.
 */
void keiland_android_wake(void);

/*
 * Reads the wake-up descriptor so that M's looper stops reporting it.
 *
 * Called on M, when it hears KEILAND_IDENT_WAKE and before it looks at what was
 * queued: a wake-up that comes after the read is heard again.
 */
void keiland_android_wake_clear(void);

/*
 * Returns a number for a request whose answer comes later, from any thread.
 *
 * The numbers rise from 1 and skip 0, so 0 can mean "no request".
 */
uint32_t keiland_android_request(void);

/*
 * Returns how many queued things were dropped for lack of room or memory.
 *
 * Called on any thread.
 */
unsigned keiland_android_dropped(void);

/*
 * Returns the Java virtual machine that JNI_OnLoad was given.
 *
 * Called on any thread; NULL before the library is loaded.
 */
JavaVM *keiland_android_vm(void);

/*
 * Returns M's JNI environment, or NULL on any other thread.
 *
 * M is attached to the virtual machine when it starts and detached when it ends.
 */
JNIEnv *keiland_android_env(void);

/*
 * Prepares the program's own Java classes for native calls.
 *
 * The program defines this function.  JNI_OnLoad calls it on U, after libkeiland's
 * own registrations, to register the program's native methods and to cache its
 * classes and method ids (a native thread's own search cannot find them).
 * Returns JNI_OK, or JNI_ERR to make the load fail.
 */
jint keiland_android_app_jni_load(JNIEnv *env);

/*
 * Tells U that M has let go of what a DESTROYED event named.
 *
 * Called on M after it stops using the window or the input queue, or when the
 * Activity is destroyed.  For every other kind it does nothing.
 */
void keiland_android_done(const struct keiland_android_event *event);

/*
 * Hands the application an event of its own (KL_APP_POST with code and data),
 * from any thread, as kl_app_post does but without an application: the post
 * waits until one is open and dispatches.  Returns 0 or ENOMEM.
 */
int keiland_post_app(uint32_t code, void *data);

/*
 * Sets the hook that sees the posts made with keiland_post_app on the
 * program's thread, before they become KL_APP_POST.  The hook returns 1
 * when it took the post (the data is then the hook's), 0 to let it be queued
 * as KL_APP_POST.  NULL removes it.  Called on M only.
 */
void keiland_android_set_post_hook(int (*hook)(uint32_t code, void *data));

/*
 * Unpacks the program's read-only data (assets/share of the package) into the
 * folder kl_data_directory names, when the version the package holds is not the
 * one written there.
 *
 * Called on M, once at the start, with keiland_android_activity().  Returns 0,
 * ENOENT (the package holds none) or the error of a file.
 */
int android_places_unpack(ANativeActivity *activity);

/*
 * Returns the scale of the screen the Activity is on, in 96ths (96 is 100 %): its
 * density in dots per inch times 96 over 160.  Windows are drawn at this scale
 * (kl_window_scale), so a program that is asked for the scale before a window
 * is made (terminal_run's scale) gives this.
 *
 * Called on M; 96 while there is no Activity.
 */
unsigned keiland_android_scale(void);

/*
 * Returns the current logical window size in pixels and the screen scale
 * (96 = 100 %), or 0 if not yet laid out.  May be called from any thread.
 */
void keiland_android_screen(unsigned *width, unsigned *height, unsigned *scale);

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
