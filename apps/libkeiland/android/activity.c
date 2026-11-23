/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The glue between the system's NativeActivity and a libkeiland program: the
 * Activity's callbacks (thread U) queue what happened, and the program's one
 * native thread (M) takes it with keiland_android_next.
 *
 * The thread is made once for the process and outlives each Activity, so the
 * program's state survives an Activity being destroyed and made again.  What
 * U waits for is only the destruction of a window, of an input queue and of
 * the Activity: the system takes those away when the callback returns, so U
 * stays in the callback until M says it let go (keiland_android_done).
 */

#include "apps/libkeiland/android/keiland-android.h"

#include "apps/libkeiland/android/bridge.h"

#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#include <sys/eventfd.h>

/* The number of events that can wait for M. */
#define ACTIVITY_RING_SIZE	64

/* The value of activity_waiting_for when U waits for no answer. */
#define ACTIVITY_WAIT_NONE	(-1)

/*
 * The events waiting for M, oldest first, and the lock that guards them and
 * every other variable of this file that U and M both use.
 */
static struct keiland_android_event activity_ring[ACTIVITY_RING_SIZE];
static unsigned activity_ring_head;
static unsigned activity_ring_count;
static pthread_mutex_t activity_lock = PTHREAD_MUTEX_INITIALIZER;

/* Signalled when M answers the destruction U waits for, and when M ends. */
static pthread_cond_t activity_answered = PTHREAD_COND_INITIALIZER;

/*
 * The kind of the destruction event U waits to be answered, or
 * ACTIVITY_WAIT_NONE.  One slot is enough because the system calls the
 * callbacks one at a time.
 */
static int activity_waiting_for = ACTIVITY_WAIT_NONE;

/* The Activity the callbacks come from now, or NULL between two Activities. */
static ANativeActivity *activity_current;

/* Whether M exists, and the function it runs. */
static int activity_thread_running;
static void (*activity_main)(void);

/*
 * The file descriptor that wakes M's looper when an event is queued, or -1
 * while M has none.  M makes and closes it; U writes to it under the lock.
 */
static int activity_wake_fd = -1;

/* How many events were dropped because the ring was full; changed under the lock, read at any time. */
static unsigned activity_dropped;

/* The last number keiland_android_request gave out. */
static uint32_t activity_request_last;

/*
 * M's looper, the input queue attached to it, and M's JNI environment with the
 * identity of the thread it belongs to.  Only M changes them, and the other
 * threads only compare the identity, so they need no lock.
 */
static ALooper *activity_looper;
static AInputQueue *activity_input;
static JNIEnv *activity_env;
static pthread_t activity_thread_id;
static int activity_env_valid;

static int activity_push(enum keiland_android_event_kind kind, ANativeWindow *window, AInputQueue *queue);
static int activity_push_locked(enum keiland_android_event_kind kind, ANativeWindow *window, AInputQueue *queue);
static void activity_push_and_wait(enum keiland_android_event_kind kind, ANativeWindow *window, AInputQueue *queue);
static void activity_make_room(void);
static void activity_attach(const struct keiland_android_event *event);
static void activity_input_drain(void);
static void activity_wake_drain(void);
static int64_t activity_now_ms(void);
static void *activity_thread(void *argument);
static void activity_on_start(ANativeActivity *activity);
static void activity_on_resume(ANativeActivity *activity);
static void activity_on_pause(ANativeActivity *activity);
static void activity_on_stop(ANativeActivity *activity);
static void activity_on_destroy(ANativeActivity *activity);
static void activity_on_focus(ANativeActivity *activity, int focused);
static void activity_on_config(ANativeActivity *activity);
static void activity_on_low_memory(ANativeActivity *activity);
static void *activity_on_save(ANativeActivity *activity, size_t *size);
static void activity_on_window_created(ANativeActivity *activity, ANativeWindow *window);
static void activity_on_window_resized(ANativeActivity *activity, ANativeWindow *window);
static void activity_on_window_redraw(ANativeActivity *activity, ANativeWindow *window);
static void activity_on_window_destroyed(ANativeActivity *activity, ANativeWindow *window);
static void activity_on_input_created(ANativeActivity *activity, AInputQueue *queue);
static void activity_on_input_destroyed(ANativeActivity *activity, AInputQueue *queue);

/*
 * Starts the program's native thread the first time and hooks the Activity's
 * callbacks to the event queue.
 */
void
keiland_android_start(
	ANativeActivity *activity,
	void *saved_state,
	size_t saved_state_size,
	void (*main)(void))
{
	pthread_attr_t attributes;
	pthread_t thread;
	int start_thread;
	int error;

	(void)saved_state;
	(void)saved_state_size;

	/* Hooks every callback of this Activity to the event queue. */
	activity->callbacks->onStart = activity_on_start;
	activity->callbacks->onResume = activity_on_resume;
	activity->callbacks->onPause = activity_on_pause;
	activity->callbacks->onStop = activity_on_stop;
	activity->callbacks->onDestroy = activity_on_destroy;
	activity->callbacks->onWindowFocusChanged = activity_on_focus;
	activity->callbacks->onConfigurationChanged = activity_on_config;
	activity->callbacks->onLowMemory = activity_on_low_memory;
	activity->callbacks->onSaveInstanceState = activity_on_save;
	activity->callbacks->onNativeWindowCreated = activity_on_window_created;
	activity->callbacks->onNativeWindowResized = activity_on_window_resized;
	activity->callbacks->onNativeWindowRedrawNeeded = activity_on_window_redraw;
	activity->callbacks->onNativeWindowDestroyed = activity_on_window_destroyed;
	activity->callbacks->onInputQueueCreated = activity_on_input_created;
	activity->callbacks->onInputQueueDestroyed = activity_on_input_destroyed;

	/* Makes this the Activity the events come from, and decides whether M is still to be made. */
	pthread_mutex_lock(&activity_lock);

	activity_current = activity;
	start_thread = 0;
	if (!activity_thread_running) {
		activity_thread_running = 1;
		activity_main = main;
		start_thread = 1;
	}

	pthread_mutex_unlock(&activity_lock);

	/* An earlier Activity already made M, which keeps running. */
	if (!start_thread)
		return;

	/* Makes M, which nobody joins. */
	pthread_attr_init(&attributes);
	pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
	error = pthread_create(&thread, &attributes, activity_thread, NULL);
	pthread_attr_destroy(&attributes);

	/* Allows a later Activity to try again when M could not be made. */
	if (error != 0) {
		pthread_mutex_lock(&activity_lock);

		activity_thread_running = 0;

		pthread_mutex_unlock(&activity_lock);
	}
}

/*
 * Takes the next event, waiting at most timeout_ms (negative: without
 * limit).  Returns 1 with the event stored, or 0 when the time ran out.
 */
int
keiland_android_next(
	int timeout_ms,
	struct keiland_android_event *event)
{
	int64_t deadline;
	int64_t left;
	int wait;
	int ident;
	int taken;

	/* The moment the wait ends, when it has a limit. */
	deadline = 0;
	if (timeout_ms > 0)
		deadline = activity_now_ms() + timeout_ms;

	/* Hands out queued events, and waits for more until the time is up. */
	for (;;) {
		/* Takes the oldest queued event. */
		taken = keiland_android_take(event);
		if (taken) {
			/* An event is stored. */
			return 1;
		}

		/* How long this wait may take. */
		wait = timeout_ms;
		if (timeout_ms > 0) {
			left = deadline - activity_now_ms();
			if (left <= 0)
				return 0;
			wait = (int)left;
		}

		/* Waits for a wake-up, for input, or for the time to run out. */
		ident = ALooper_pollOnce(wait, NULL, NULL, NULL);
		if (ident == KEILAND_IDENT_WAKE) {
			activity_wake_drain();
		} else if (ident == KEILAND_IDENT_INPUT) {
			activity_input_drain();
		} else if (ident == ALOOPER_POLL_TIMEOUT) {
			return 0;
		} else if (ident == ALOOPER_POLL_ERROR) {
			return 0;
		}
	}
}

/*
 * Takes the oldest queued event without waiting.  Returns 1 with it stored,
 * or 0 when none is queued.
 */
int
keiland_android_take(
	struct keiland_android_event *event)
{
	int taken;

	/* Takes the event under the lock. */
	pthread_mutex_lock(&activity_lock);

	taken = 0;
	if (activity_ring_count > 0) {
		*event = activity_ring[activity_ring_head];
		activity_ring_head = (activity_ring_head + 1) % ACTIVITY_RING_SIZE;
		activity_ring_count--;
		taken = 1;
	}

	pthread_mutex_unlock(&activity_lock);

	/* Joins an input queue to the looper, or lets it go, before the caller sees the event. */
	if (taken)
		activity_attach(event);

	/* Succeeded: whether there was an event. */
	return taken;
}

/*
 * Wakes M's looper so that it looks at what was queued.
 */
void
keiland_android_wake(void)
{
	uint64_t one;
	ssize_t written;
	int fd;

	/* Reads the descriptor M published, which is -1 before M made it. */
	fd = __atomic_load_n(&activity_wake_fd, __ATOMIC_SEQ_CST);
	if (fd < 0)
		return;

	/* Adds one to the counter, which makes the descriptor readable. */
	one = 1;
	written = write(fd, &one, sizeof(one));
	if (written < 0) {
		/* The counter is full, which means M is already awake. */
	}
}

/*
 * Reads the wake-up descriptor.
 */
void
keiland_android_wake_clear(void)
{
	/* The counter goes back to 0. */
	activity_wake_drain();
}

/*
 * Returns a number for a request whose answer comes later.
 */
uint32_t
keiland_android_request(void)
{
	uint32_t number;

	/* Takes the next number; 0 is kept for "no request". */
	number = __atomic_add_fetch(&activity_request_last, 1, __ATOMIC_SEQ_CST);
	if (number == 0)
		number = __atomic_add_fetch(&activity_request_last, 1, __ATOMIC_SEQ_CST);

	/* Succeeded: a number that is not 0. */
	return number;
}

/*
 * Returns how many queued things were dropped.
 */
unsigned
keiland_android_dropped(void)
{
	unsigned count;

	/* Adds the events the ring could not hold to those the bridge could not allocate. */
	count = __atomic_load_n(&activity_dropped, __ATOMIC_SEQ_CST);
	count += keiland_bridge_dropped();

	/* Succeeded: the number of things dropped. */
	return count;
}

/*
 * Returns M's JNI environment, or NULL on any other thread.
 */
JNIEnv *
keiland_android_env(void)
{
	/* No environment before M is attached, or after it ended. */
	if (!__atomic_load_n(&activity_env_valid, __ATOMIC_SEQ_CST))
		return NULL;

	/* Only M may use the environment. */
	if (!pthread_equal(pthread_self(), activity_thread_id))
		return NULL;

	/* Succeeded: M's own environment. */
	return activity_env;
}

/*
 * Tells U that M has let go of what a destruction event named.
 */
void
keiland_android_done(
	const struct keiland_android_event *event)
{
	/* Only a destruction is waited for. */
	if (event->kind != KEILAND_ANDROID_WINDOW_DESTROYED &&
	    event->kind != KEILAND_ANDROID_INPUT_DESTROYED &&
	    event->kind != KEILAND_ANDROID_ACTIVITY_DESTROYED)
		return;

	/* Lets U go if it waits for this kind. */
	pthread_mutex_lock(&activity_lock);

	if (activity_waiting_for == (int)event->kind) {
		activity_waiting_for = ACTIVITY_WAIT_NONE;
		pthread_cond_broadcast(&activity_answered);
	}

	pthread_mutex_unlock(&activity_lock);
}

/*
 * Returns the looper of M.
 */
ALooper *
keiland_android_looper(void)
{
	/* M's own looper, which only M reads. */
	return activity_looper;
}

/*
 * Returns the Activity the events come from now, or NULL.
 */
ANativeActivity *
keiland_android_activity(void)
{
	ANativeActivity *current;

	/* Reads the Activity under the lock U sets it with. */
	pthread_mutex_lock(&activity_lock);

	current = activity_current;

	pthread_mutex_unlock(&activity_lock);

	/* Succeeded: the Activity of this moment. */
	return current;
}

/*
 * Queues an event for M and wakes M.  Returns 0, or EAGAIN when the ring is
 * full and the event was dropped.
 */
static int
activity_push(
	enum keiland_android_event_kind kind,
	ANativeWindow *window,
	AInputQueue *queue)
{
	int error;

	/* Queues the event under the lock. */
	pthread_mutex_lock(&activity_lock);

	error = activity_push_locked(kind, window, queue);

	pthread_mutex_unlock(&activity_lock);

	/* Reports whether the event was queued. */
	if (error != 0)
		return error;

	/* Succeeded: M will see the event. */
	return 0;
}

/*
 * Queues an event with the lock held.  When the ring is full, the oldest
 * resize and redraw events go first because the newest one says it all.
 */
static int
activity_push_locked(
	enum keiland_android_event_kind kind,
	ANativeWindow *window,
	AInputQueue *queue)
{
	struct keiland_android_event *slot;
	unsigned index;

	/* Makes room by dropping events that a newer one replaces. */
	if (activity_ring_count == ACTIVITY_RING_SIZE)
		activity_make_room();

	/* Gives up when nothing could be dropped. */
	if (activity_ring_count == ACTIVITY_RING_SIZE) {
		__atomic_add_fetch(&activity_dropped, 1, __ATOMIC_SEQ_CST);

		return EAGAIN;
	}

	/* Stores the event after the newest one. */
	index = (activity_ring_head + activity_ring_count) % ACTIVITY_RING_SIZE;
	slot = &activity_ring[index];
	slot->kind = kind;
	slot->window = window;
	slot->queue = queue;
	activity_ring_count++;

	/* Wakes M's looper, when M has made it; M looks at the ring before it sleeps again. */
	keiland_android_wake();

	/* Succeeded: the event is queued. */
	return 0;
}

/*
 * Queues a destruction event and waits until M has let go of what it names,
 * or until M ends.  Waiting is only for the destructions, because the system
 * takes the object away when the callback returns.
 */
static void
activity_push_and_wait(
	enum keiland_android_event_kind kind,
	ANativeWindow *window,
	AInputQueue *queue)
{
	int error;

	/* Queues the event, and waits when M exists to answer. */
	pthread_mutex_lock(&activity_lock);

	error = activity_push_locked(kind, window, queue);
	if (error == 0 && activity_thread_running) {
		activity_waiting_for = (int)kind;

		/* Waits for the answer; M's end also lets U go. */
		while (activity_waiting_for != ACTIVITY_WAIT_NONE && activity_thread_running)
			pthread_cond_wait(&activity_answered, &activity_lock);
	}

	pthread_mutex_unlock(&activity_lock);
}

/*
 * Drops the oldest resize or redraw event from the full ring, with the lock
 * held.  A later event of the same window says what they said.
 */
static void
activity_make_room(void)
{
	unsigned i;
	unsigned from;
	unsigned to;
	unsigned index;
	unsigned kept;
	enum keiland_android_event_kind kind;

	/* Finds the oldest event that a newer one replaces. */
	for (i = 0;
	     i < activity_ring_count;
	     i++) {
		index = (activity_ring_head + i) % ACTIVITY_RING_SIZE;
		kind = activity_ring[index].kind;
		if (kind == KEILAND_ANDROID_WINDOW_RESIZED || kind == KEILAND_ANDROID_WINDOW_REDRAW)
			break;
	}

	/* Nothing in the ring can be dropped. */
	if (i == activity_ring_count)
		return;

	/* Closes the gap by moving the newer events one place toward the head. */
	kept = i;
	for (from = i + 1;
	     from < activity_ring_count;
	     from++) {
		to = (activity_ring_head + kept) % ACTIVITY_RING_SIZE;
		index = (activity_ring_head + from) % ACTIVITY_RING_SIZE;
		activity_ring[to] = activity_ring[index];
		kept++;
	}
	activity_ring_count--;
}

/*
 * Joins an input queue to M's looper when it appears and lets it go when it
 * is about to disappear, before M is told about either.
 */
static void
activity_attach(
	const struct keiland_android_event *event)
{
	/* Starts to wait for the new input queue's events. */
	if (event->kind == KEILAND_ANDROID_INPUT_CREATED) {
		activity_input = event->queue;
		AInputQueue_attachLooper(event->queue, activity_looper, KEILAND_IDENT_INPUT, NULL, NULL);
	}

	/* Stops waiting for the input queue that is going away. */
	if (event->kind == KEILAND_ANDROID_INPUT_DESTROYED) {
		AInputQueue_detachLooper(event->queue);
		activity_input = NULL;
	}
}

/*
 * Hands every input event back unhandled, since no program takes them yet.
 * The system reports a program that leaves input unanswered as not
 * responding.
 */
static void
activity_input_drain(void)
{
	AInputEvent *input;
	int status;
	int consumed;

	/* Nothing to do without a queue. */
	if (activity_input == NULL)
		return;

	/* Answers every event that is waiting. */
	for (;;) {
		status = AInputQueue_getEvent(activity_input, &input);
		if (status < 0)
			break;

		/* The input method took this event, and finishes it itself. */
		consumed = AInputQueue_preDispatchEvent(activity_input, input);
		if (consumed != 0)
			continue;

		AInputQueue_finishEvent(activity_input, input, 0);
	}
}

/*
 * Empties the wake counter so the file descriptor stops reporting input.
 */
static void
activity_wake_drain(void)
{
	uint64_t count;
	ssize_t got;

	/* Reads the counter, which holds the number of wake-ups since the last read. */
	got = read(activity_wake_fd, &count, sizeof(count));
	if (got < 0) {
		/* Nothing was left: another wake-up already took it. */
	}
}

/*
 * Returns a time of the monotonic clock in milliseconds.
 */
static int64_t
activity_now_ms(void)
{
	struct timespec now;

	/* Reads the monotonic clock. */
	clock_gettime(CLOCK_MONOTONIC, &now);

	/* Succeeded: the time as milliseconds. */
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/*
 * The body of M: makes its looper and the wake file descriptor, runs the
 * program's main, and lets U go when main returns.
 */
static void *
activity_thread(
	void *argument)
{
	void (*main)(void);
	JavaVM *vm;
	JNIEnv *env;
	int fd;
	int status;

	(void)argument;

	/* Makes M's looper, which can be polled with idents. */
	activity_looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);

	/* Makes the file descriptor that U writes to wake M. */
	fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (fd >= 0) {
		status = ALooper_addFd(activity_looper, fd, KEILAND_IDENT_WAKE, ALOOPER_EVENT_INPUT, NULL, NULL);
		if (status < 0) {
			close(fd);
			fd = -1;
		}
	}

	/* Attaches M to the Java machine, so that it can call Java; the bridge wakes M when Java queues something. */
	env = NULL;
	vm = keiland_android_vm();
	if (vm != NULL) {
		status = (*vm)->AttachCurrentThread(vm, &env, NULL);
		if (status != JNI_OK)
			env = NULL;
	}
	activity_thread_id = pthread_self();
	activity_env = env;
	__atomic_store_n(&activity_env_valid, env != NULL, __ATOMIC_SEQ_CST);
	keiland_bridge_set_wake(keiland_android_wake);

	/* Lets U wake M, and takes the function to run. */
	pthread_mutex_lock(&activity_lock);

	__atomic_store_n(&activity_wake_fd, fd, __ATOMIC_SEQ_CST);
	main = activity_main;

	pthread_mutex_unlock(&activity_lock);

	/* Runs the program, when M can be woken. */
	if (fd >= 0)
		main();

	/* Detaches M from the Java machine. */
	__atomic_store_n(&activity_env_valid, 0, __ATOMIC_SEQ_CST);
	if (env != NULL)
		(*vm)->DetachCurrentThread(vm);

	/* Lets U go: nobody is left to answer, and U must not wait for ever. */
	pthread_mutex_lock(&activity_lock);

	__atomic_store_n(&activity_wake_fd, -1, __ATOMIC_SEQ_CST);
	activity_thread_running = 0;
	activity_waiting_for = ACTIVITY_WAIT_NONE;
	pthread_cond_broadcast(&activity_answered);

	pthread_mutex_unlock(&activity_lock);

	/* Closes the file descriptor of the wake-up. */
	if (fd >= 0) {
		ALooper_removeFd(activity_looper, fd);
		close(fd);
	}

	/* Succeeded: M ends. */
	return NULL;
}

static void
activity_on_start(
	ANativeActivity *activity)
{
	(void)activity;

	/* The Activity became visible. */
	activity_push(KEILAND_ANDROID_START, NULL, NULL);
}

static void
activity_on_resume(
	ANativeActivity *activity)
{
	(void)activity;

	/* The Activity is in the foreground. */
	activity_push(KEILAND_ANDROID_RESUME, NULL, NULL);
}

static void
activity_on_pause(
	ANativeActivity *activity)
{
	(void)activity;

	/* The Activity left the foreground. */
	activity_push(KEILAND_ANDROID_PAUSE, NULL, NULL);
}

static void
activity_on_stop(
	ANativeActivity *activity)
{
	(void)activity;

	/* The Activity is no longer visible. */
	activity_push(KEILAND_ANDROID_STOP, NULL, NULL);
}

/*
 * The Activity is going away: M must stop using it before U returns.
 */
static void
activity_on_destroy(
	ANativeActivity *activity)
{
	/* Tells M and waits until it let go. */
	activity_push_and_wait(KEILAND_ANDROID_ACTIVITY_DESTROYED, NULL, NULL);

	/* Forgets the Activity, unless a newer one already took its place. */
	pthread_mutex_lock(&activity_lock);

	if (activity_current == activity)
		activity_current = NULL;

	pthread_mutex_unlock(&activity_lock);
}

static void
activity_on_focus(
	ANativeActivity *activity,
	int focused)
{
	(void)activity;

	/* The window gained or lost the input focus. */
	if (focused)
		activity_push(KEILAND_ANDROID_FOCUS_GAINED, NULL, NULL);
	else
		activity_push(KEILAND_ANDROID_FOCUS_LOST, NULL, NULL);
}

static void
activity_on_config(
	ANativeActivity *activity)
{
	(void)activity;

	/* The device's configuration (language, keyboard, density) changed. */
	activity_push(KEILAND_ANDROID_CONFIG, NULL, NULL);
}

static void
activity_on_low_memory(
	ANativeActivity *activity)
{
	(void)activity;

	/* The system is short of memory. */
	activity_push(KEILAND_ANDROID_LOW_MEMORY, NULL, NULL);
}

/*
 * The program keeps no state for the system to save.
 */
static void *
activity_on_save(
	ANativeActivity *activity,
	size_t *size)
{
	(void)activity;

	/* Says there is nothing to save. */
	*size = 0;

	/* Succeeded: no saved state. */
	return NULL;
}

static void
activity_on_window_created(
	ANativeActivity *activity,
	ANativeWindow *window)
{
	(void)activity;

	/* A window to draw on appeared. */
	activity_push(KEILAND_ANDROID_WINDOW_CREATED, window, NULL);
}

static void
activity_on_window_resized(
	ANativeActivity *activity,
	ANativeWindow *window)
{
	(void)activity;

	/* The window's size changed. */
	activity_push(KEILAND_ANDROID_WINDOW_RESIZED, window, NULL);
}

static void
activity_on_window_redraw(
	ANativeActivity *activity,
	ANativeWindow *window)
{
	(void)activity;

	/* The window's contents must be drawn again. */
	activity_push(KEILAND_ANDROID_WINDOW_REDRAW, window, NULL);
}

/*
 * The window goes away when this returns, so M must stop drawing on it first.
 */
static void
activity_on_window_destroyed(
	ANativeActivity *activity,
	ANativeWindow *window)
{
	(void)activity;

	/* Tells M and waits until it let go. */
	activity_push_and_wait(KEILAND_ANDROID_WINDOW_DESTROYED, window, NULL);
}

static void
activity_on_input_created(
	ANativeActivity *activity,
	AInputQueue *queue)
{
	(void)activity;

	/* An input queue appeared. */
	activity_push(KEILAND_ANDROID_INPUT_CREATED, NULL, queue);
}

/*
 * The input queue goes away when this returns, so M must detach it first.
 */
static void
activity_on_input_destroyed(
	ANativeActivity *activity,
	AInputQueue *queue)
{
	(void)activity;

	/* Tells M and waits until it let go. */
	activity_push_and_wait(KEILAND_ANDROID_INPUT_DESTROYED, NULL, queue);
}
