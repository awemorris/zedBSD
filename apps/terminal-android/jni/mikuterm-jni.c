/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * mikuterm's side of the Java calls: libkeiland's JNI_OnLoad calls
 * keiland_android_app_jni_load once the library is loaded, for the program to
 * register its own native methods and look up its own classes.
 */

#include "apps/libkeiland/android/keiland-android.h"

/*
 * Prepares the program's Java classes for native calls: it has none that
 * calls the native code yet.
 */
jint
keiland_android_app_jni_load(
	JNIEnv *env)
{
	(void)env;

	/* Succeeded: nothing to register. */
	return JNI_OK;
}
