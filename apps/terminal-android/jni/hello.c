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

#include <stddef.h>
#include <stdint.h>

#include <android/hardware_buffer.h>

/* The colour, one byte each: red, green, blue, and fully opaque. */
#define HELLO_RED	0x33
#define HELLO_GREEN	0x66
#define HELLO_BLUE	0xCC
#define HELLO_ALPHA	0xFF

/* The window painted now, or NULL; it is the system's until its destruction is answered. */
static ANativeWindow *hello_window;

static void hello_paint(void);

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
	int32_t status;
	int got;

	/* Takes the events one at a time for as long as the process lives. */
	for (;;) {
		got = keiland_android_next(-1, &event);
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
