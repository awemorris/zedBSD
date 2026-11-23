/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The entry point the system looks up in the native library: the NativeActivity
 * that mikuterm's Activity derives from calls it once, on the Activity's main
 * thread, when the Activity is created.
 */

#include "apps/libkeiland/android/keiland-android.h"

#include "apps/terminal-android/jni/hello.h"

#include <jni.h>

/*
 * Hands the new Activity to libkeiland, with the function the program's native
 * thread runs.
 */
JNIEXPORT void
ANativeActivity_onCreate(
	ANativeActivity *activity,
	void *saved_state,
	size_t saved_state_size)
{
	/* Starts the native thread (the first time) and hooks the Activity to it. */
	keiland_android_start(activity, saved_state, saved_state_size, hello_main);
}
