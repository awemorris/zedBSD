/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

package io.noctvm.mikuterm;

import io.noctvm.keiland.KeilandActivity;

/**
 * mikuterm's Activity: the one the launcher starts.
 *
 * The native library is loaded here, by this application's class loader, so that its JNI_OnLoad
 * finds mikuterm's Java classes (a native thread's own search cannot).
 */
public class MikutermActivity extends KeilandActivity {
	static {
		System.loadLibrary("mikuterm");
	}
}
