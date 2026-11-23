/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

package io.noctvm.keiland;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;

import java.lang.ref.WeakReference;

/**
 * What passes between the Java side of a libkeiland program and its native code.
 *
 * The native functions are called from Java's threads and only queue what they are given for the
 * native thread, returning at once.  The static methods without "native" are called from the
 * native thread through JNI; each hands its work to the main thread and returns at once, so the
 * native thread never waits for Java.  A request that has no Activity to act on does nothing.
 */
public final class KeilandBridge {
	/** The main thread's queue, where the work asked for by the native thread runs. */
	private static final Handler MAIN = new Handler(Looper.getMainLooper());

	/** The Activity that is on screen now; weak so that a destroyed one can be collected. */
	private static WeakReference<KeilandActivity> current = new WeakReference<KeilandActivity>(null);

	/** The application's context, which outlives any Activity (the clipboard is set through it). */
	private static Context applicationContext;

	private KeilandBridge() {
	}

	/** Remembers the Activity that the requests of the native thread are for. */
	static void attach(KeilandActivity activity) {
		current = new WeakReference<KeilandActivity>(activity);
		applicationContext = activity.getApplicationContext();
	}

	/** Forgets the Activity, if it is still the one remembered. */
	static void detach(KeilandActivity activity) {
		if (current.get() == activity) {
			current = new WeakReference<KeilandActivity>(null);
		}
	}

	/** The text that the input method committed. */
	static native void nativeCommit(String text);

	/** The text still being composed and the cursor in it, in code points (an empty text clears it). */
	static native void nativeCompose(String text, int cursor);

	/** The number of characters to delete before and after the cursor, in code points. */
	static native void nativeDelete(int before, int after);

	/** The window's insets and the keyboard's height, in pixels. */
	static native void nativeInsets(int left, int top, int right, int bottom, int keyboard);

	/** The look of the system: dark mode, the accent (0xRRGGBB, -1 for none), the font scale and the locale. */
	static native void nativeConfig(boolean dark, int accent, float fontScale, String locale);

	/** Whether the window has the input focus. */
	static native void nativeFocus(boolean focused);

	/** The clipboard's text (null when it holds none). */
	static native void nativeClipboard(String text);

	/** The answer to a menu: the request's number and the chosen item, or -1 when it was dismissed. */
	static native void nativeMenu(int request, int item);

	/** Runs the work on the main thread, where an exception is the work's own and never reaches the native thread. */
	private static void post(final Runnable work) {
		MAIN.post(new Runnable() {
			@Override
			public void run() {
				try {
					work.run();
				} catch (RuntimeException e) {
					/* The request failed; the native thread was never told to wait for it. */
				}
			}
		});
	}

	/** Shows or hides the soft keyboard. */
	static void showKeyboard(final boolean show) {
		post(new Runnable() {
			@Override
			public void run() {
				KeilandActivity activity = current.get();
				if (activity != null) {
					activity.inputView().showKeyboard(show);
				}
			}
		});
	}

	/** Chooses between letters (0) and a language's input (1) for the next input session. */
	static void setInputMode(final int mode) {
		post(new Runnable() {
			@Override
			public void run() {
				KeilandActivity activity = current.get();
				if (activity != null) {
					activity.inputView().setMode(mode);
				}
			}
		});
	}

	/** Tells the input method where the composing text is, so it can place its candidates. */
	static void setCursorAnchor(final float x, final float y, final float width, final float height) {
		post(new Runnable() {
			@Override
			public void run() {
				KeilandActivity activity = current.get();
				if (activity != null) {
					activity.inputView().setCursorAnchor(x, y, width, height);
				}
			}
		});
	}

	/** Sets the title that the task shows in the list of recent apps. */
	static void setTitle(final String title) {
		post(new Runnable() {
			@Override
			public void run() {
				KeilandActivity activity = current.get();
				if (activity != null) {
					activity.setTitle(title);
				}
			}
		});
	}

	/** Puts the text on the clipboard, or empties the clipboard when the text is null or empty. */
	static void clipboardSet(final String text) {
		post(new Runnable() {
			@Override
			public void run() {
				ClipboardManager clipboard;

				if (applicationContext == null) {
					return;
				}
				clipboard = (ClipboardManager) applicationContext.getSystemService(Context.CLIPBOARD_SERVICE);
				if (clipboard == null) {
					return;
				}
				if (text == null || text.isEmpty()) {
					clipboard.clearPrimaryClip();
				} else {
					clipboard.setPrimaryClip(ClipData.newPlainText("", text));
				}
			}
		});
	}

	/** Shows a menu at a place and answers it through nativeMenu (the menus themselves come later). */
	static void popupMenu(final int request, final int x, final int y, final String[] labels, final int[] parents, final int[] flags) {
		post(new Runnable() {
			@Override
			public void run() {
				nativeMenu(request, -1);
			}
		});
	}

	/** Opens a web address in the system's browser; other kinds of address are refused. */
	static void openUrl(final String url) {
		post(new Runnable() {
			@Override
			public void run() {
				KeilandActivity activity = current.get();
				Uri uri;
				String scheme;

				if (activity == null || url == null) {
					return;
				}
				uri = Uri.parse(url);
				scheme = uri.getScheme();
				if (!"https".equals(scheme) && !"http".equals(scheme)) {
					return;
				}
				activity.startActivity(new Intent(Intent.ACTION_VIEW, uri));
			}
		});
	}

	/** Closes the Activity and removes its task from the list of recent apps. */
	static void quit() {
		post(new Runnable() {
			@Override
			public void run() {
				Activity activity = current.get();
				if (activity != null) {
					activity.finishAndRemoveTask();
				}
			}
		});
	}
}
