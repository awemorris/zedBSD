/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

package io.noctvm.mikuterm;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;

import java.lang.ref.WeakReference;

/**
 * What passes between mikuterm's Java side and its native code, besides what the keyboard, the
 * clipboard and the menus need (those are libkeiland's).
 *
 * The native methods are called from Java's threads and only queue what they are given for the
 * native thread, returning at once.  The static methods without "native" are called from the native
 * thread through JNI and return at once too: the work they start runs on other threads.
 */
final class MikutermBridge {
	private static final Handler MAIN = new Handler(Looper.getMainLooper());
	private static WeakReference<MikutermActivity> current = new WeakReference<MikutermActivity>(null);
	private static Context applicationContext;

	private MikutermBridge() {
	}

	/** Remembers the current Activity and the application context for service updates. */
	static void attach(MikutermActivity activity) {
		current = new WeakReference<MikutermActivity>(activity);
		applicationContext = activity.getApplicationContext();
	}

	/** Forgets the Activity if it is still the one remembered. */
	static void detach(MikutermActivity activity) {
		if (current.get() == activity) {
			current = new WeakReference<MikutermActivity>(null);
		}
	}

	/** The answer to a secretWrap or secretUnwrap: the request's number and the bytes (null when it failed). */
	static native void nativeSecret(int request, byte[] data);

	/** The default network came up (true) or went away (false), with the system's number for it. */
	static native void nativeNetwork(boolean up, long handle);

	/** The user asked to close every connection. */
	static native void nativeStopAll();

	/** Wraps a passphrase with the Keystore's key; the answer comes through nativeSecret. */
	static void secretWrap(int request, byte[] plain) {
		Secrets.wrap(request, plain);
	}

	/** Opens a wrapped passphrase; the answer comes through nativeSecret. */
	static void secretUnwrap(int request, byte[] wrapped) {
		Secrets.unwrap(request, wrapped);
	}

	/** Starts, updates or stops the foreground service for the current number of active connections. */
	static void serviceUpdate(final int sessions) {
		MAIN.post(new Runnable() {
			@Override
			public void run() {
				try {
					SessionService.update(applicationContext, sessions);
				} catch (RuntimeException e) {
					/* The service could not be updated; the native side does not wait. */
				}
			}
		});
	}
}
