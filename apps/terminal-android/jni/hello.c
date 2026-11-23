/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The body of the native thread while the program is only a trial of the
 * Activity's glue: it takes each event of the Activity and paints the window
 * one colour, so that what a screenshot shows says the glue works.
 */

#include "apps/terminal-android/jni/hello.h"

#include "apps/libkeiland/android/keiland-android.h"

#include "apps/libkeiland/android/bridge.h"
#include "apps/libkeiland/android/bridge-jni.h"

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <android/hardware_buffer.h>

/* The colour, one byte each: red, green, blue, and fully opaque. */
#define HELLO_RED	0x33
#define HELLO_GREEN	0x66
#define HELLO_BLUE	0xCC
#define HELLO_ALPHA	0xFF

/* The window painted now, or NULL; it is the system's until its destruction is answered. */
static ANativeWindow *hello_window;

/*
 * The file that records what the input method told the program, one line each:
 * "C text" for a commit, "P cursor text" for text being composed, "D before
 * after" for a deletion and "F 0" or "F 1" for the focus.  It is emptied when
 * the program starts, and each line reaches the disk before the next event is
 * taken, so a test can read it at any time.  -1 while it is not open.
 */
static int hello_log = -1;

/*
 * Whether the keyboard has to be asked for when the window gains the focus.  It is asked for
 * again only after the focus was lost: showing the keyboard changes the window's focus, so
 * asking at every gain would never stop.
 */
static int hello_keyboard_due = 1;

static void hello_paint(void);
static void hello_log_open(void);
static void hello_log_line(const char *line);
static void hello_log_event(const struct keiland_bridge_event *event);

/*
 * The program has no Java classes of its own to prepare.
 */
jint
keiland_android_app_jni_load(
	JNIEnv *env)
{
	(void)env;

	/* Succeeded: nothing to register. */
	return JNI_OK;
}

/*
 * Runs the events of the Activity and paints each window; does not return.
 */
void
hello_main(void)
{
	struct keiland_android_event event;
	struct keiland_bridge_event *list;
	int32_t status;
	int got;

	/* Opens the record of the input method's events. */
	hello_log_open();

	/* Takes the events one at a time for as long as the process lives. */
	for (;;) {
		/* Writes down what Java told the program since the last look. */
		list = keiland_bridge_take_all();
		hello_log_event(list);
		keiland_bridge_free(list);

		/* Waits a short while, so that what Java queues is looked at soon. */
		got = keiland_android_next(100, &event);
		if (!got)
			continue;

		/* Reacts to what happened to the window and the Activity. */
		switch (event.kind) {
		case KEILAND_ANDROID_WINDOW_CREATED:
			/* Keeps the window and gives it the pixel layout that is painted. */
			ANativeWindow_acquire(event.window);
			hello_window = event.window;
			status = ANativeWindow_setBuffersGeometry(hello_window, 0, 0, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
			if (status < 0) {
				/* The window keeps its own layout, which is painted as it is. */
			}

			hello_paint();
			keiland_java_show_keyboard(1);
			hello_keyboard_due = 0;
			break;

		case KEILAND_ANDROID_FOCUS_LOST:
			/* The keyboard goes with the focus, and comes back with it. */
			hello_keyboard_due = 1;
			break;

		case KEILAND_ANDROID_FOCUS_GAINED:
			/* Asks for the keyboard when the focus comes back. */
			if (hello_keyboard_due) {
				keiland_java_show_keyboard(1);
				hello_keyboard_due = 0;
			}
			break;

		case KEILAND_ANDROID_WINDOW_RESIZED:
			/* The window has a new size. */
			hello_paint();
			break;

		case KEILAND_ANDROID_WINDOW_REDRAW:
			/* The system asks for the contents again. */
			hello_paint();
			break;

		case KEILAND_ANDROID_WINDOW_DESTROYED:
			/* Lets the window go before the system takes it away. */
			if (hello_window != NULL) {
				ANativeWindow_release(hello_window);
				hello_window = NULL;
			}
			keiland_android_done(&event);
			break;

		case KEILAND_ANDROID_INPUT_DESTROYED:
		case KEILAND_ANDROID_ACTIVITY_DESTROYED:
			/* Nothing here uses them, so U can go on at once. */
			keiland_android_done(&event);
			break;

		default:
			/* The other events change nothing on the screen. */
			break;
		}
	}
}

/*
 * Paints the whole window one colour and shows it.
 */
static void
hello_paint(void)
{
	ANativeWindow_Buffer buffer;
	uint8_t *row;
	int32_t y;
	int32_t x;
	int status;

	/* No window, nothing to paint. */
	if (hello_window == NULL)
		return;

	/* Takes the window's pixels for writing. */
	status = ANativeWindow_lock(hello_window, &buffer, NULL);
	if (status < 0)
		return;

	/* Writes the colour byte by byte, so that the byte order of the machine does not matter. */
	for (y = 0;
	     y < buffer.height;
	     y++) {
		row = (uint8_t *)buffer.bits + (size_t)y * (size_t)buffer.stride * 4;
		for (x = 0;
		     x < buffer.width;
		     x++) {
			row[x * 4 + 0] = HELLO_RED;
			row[x * 4 + 1] = HELLO_GREEN;
			row[x * 4 + 2] = HELLO_BLUE;
			row[x * 4 + 3] = HELLO_ALPHA;
		}
	}

	/* Shows what was written. */
	ANativeWindow_unlockAndPost(hello_window);
}

/*
 * Opens the record of the input method's events, emptying it.
 */
static void
hello_log_open(void)
{
	ANativeActivity *activity;
	char path[512];
	int length;

	/* The record lives in the program's own data folder. */
	activity = keiland_android_activity();
	if (activity == NULL)
		return;
	length = snprintf(path, sizeof(path), "%s/ime.txt", activity->internalDataPath);
	if (length < 0 || (size_t)length >= sizeof(path))
		return;

	/* Opens it empty; every line is appended. */
	hello_log = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0600);
}

/*
 * Appends one line to the record and makes sure it reached the disk.
 */
static void
hello_log_line(
	const char *line)
{
	ssize_t written;

	/* Nothing is recorded when the file could not be opened. */
	if (hello_log < 0)
		return;

	/* Writes the line and a newline in one piece, then syncs. */
	written = write(hello_log, line, strlen(line));
	if (written < 0)
		return;
	written = write(hello_log, "\n", 1);
	if (written < 0)
		return;
	fsync(hello_log);
}

/*
 * Records each event of a list that Java queued.  Backslashes and newlines in
 * a text are written as \\ and \n so that each event stays on one line.
 */
static void
hello_log_event(
	const struct keiland_bridge_event *event)
{
	char line[1024];
	char text[640];
	size_t used;
	const char *p;

	/* Goes through the events oldest first. */
	for (;
	     event != NULL;
	     event = event->next) {
		/* Escapes the text, which is empty for the events that carry none. */
		used = 0;
		for (p = event->text;
		     p != NULL && *p != '\0' && used + 3 < sizeof(text);
		     p++) {
			if (*p == '\n') {
				text[used++] = '\\';
				text[used++] = 'n';
			} else if (*p == '\\') {
				text[used++] = '\\';
				text[used++] = '\\';
			} else {
				text[used++] = *p;
			}
		}
		text[used] = '\0';

		/* Writes the line for the kinds this test cares about. */
		if (event->kind == KEILAND_BRIDGE_COMMIT) {
			snprintf(line, sizeof(line), "C %s", text);
			hello_log_line(line);
		} else if (event->kind == KEILAND_BRIDGE_COMPOSE) {
			snprintf(line, sizeof(line), "P %d %s", (int)event->a, text);
			hello_log_line(line);
		} else if (event->kind == KEILAND_BRIDGE_DELETE) {
			snprintf(line, sizeof(line), "D %d %d", (int)event->a, (int)event->b);
			hello_log_line(line);
		} else if (event->kind == KEILAND_BRIDGE_FOCUS) {
			snprintf(line, sizeof(line), "F %d", (int)event->a);
			hello_log_line(line);
		}
	}
}
