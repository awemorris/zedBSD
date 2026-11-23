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

#include <stddef.h>

#include <android/native_activity.h>
#include <jni.h>

/*
 * Called by the system when the Activity is created.  Nothing is set up yet, so
 * the Activity starts and shows nothing.
 */
JNIEXPORT void ANativeActivity_onCreate(ANativeActivity *activity, void *saved_state, size_t saved_state_size)
{
	/* No state is kept yet, so none is restored. */
	(void)activity;
	(void)saved_state;
	(void)saved_state_size;
}
