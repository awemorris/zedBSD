/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

package io.noctvm.keiland;

import android.app.NativeActivity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.res.Configuration;
import android.graphics.Insets;
import android.os.Build;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.widget.PopupWindow;

/**
 * The Activity of a libkeiland program on Android: a NativeActivity whose native library draws the
 * window, with an invisible view over it for the input method.  A program's own Activity derives
 * from it and loads the program's native library in a static initializer, so that the library finds
 * the program's Java classes.
 */
public class KeilandActivity extends NativeActivity {
	/** The most of the window a keyboard may cover and still be made room for, in percent. */
	private static final int KEYBOARD_MAX_PERCENT = 70;

	/** The view the input method types into. */
	private InputView inputView;

	/** The row of keys shown over the keyboard while it is up, and the window it is in (the Activity's own window is the native code's, so no view of it is drawn). */
	private KeyBar keyBar;
	private PopupWindow keyBarWindow;

	/** The system's clipboard, for the native code to read from. */
	private ClipboardManager clipboard;
	private ClipboardManager.OnPrimaryClipChangedListener clipListener;

	@Override
	protected void onCreate(Bundle savedInstanceState) {
		super.onCreate(savedInstanceState);

		/* Lays the input view over the window; setContentView would replace the surface. */
		inputView = new InputView(this);
		addContentView(inputView, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

		/*
		 * The key bar, in a window of its own (a view of this window would not be drawn, as the surface is the
		 * native code's) that takes no focus, so the keyboard stays up; it is shown while the keyboard is.
		 */
		keyBar = new KeyBar(this);
		inputView.setKeyBar(keyBar);
		keyBarWindow = new PopupWindow(keyBar, ViewGroup.LayoutParams.MATCH_PARENT, keyBar.heightPixels());
		keyBarWindow.setFocusable(false);
		keyBarWindow.setTouchable(true);
		keyBarWindow.setClippingEnabled(false);
		keyBarWindow.setInputMethodMode(PopupWindow.INPUT_METHOD_NOT_NEEDED);

		/* The window is under the system's bars; the native code is told how much of it is covered. */
		inputView.setOnApplyWindowInsetsListener(new View.OnApplyWindowInsetsListener() {
			@Override
			public WindowInsets onApplyWindowInsets(View view, WindowInsets insets) {
				sendInsets(insets);
				return insets;
			}
		});
		inputView.requestApplyInsets();

		/* The clipboard is read when it changes (only while the window has the focus, the only time the system lets it). */
		clipboard = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
		clipListener = new ClipboardManager.OnPrimaryClipChangedListener() {
			@Override
			public void onPrimaryClipChanged() {
				if (hasWindowFocus()) {
					sendClipboard();
				}
			}
		};
		if (clipboard != null) {
			clipboard.addPrimaryClipChangedListener(clipListener);
		}

		/* Lets the native code reach this Activity, and tells it the system's look. */
		KeilandBridge.attach(this);
		sendConfig(getResources().getConfiguration());

		onKeilandCreated();
	}

	@Override
	public void onConfigurationChanged(Configuration newConfig) {
		super.onConfigurationChanged(newConfig);
		sendConfig(newConfig);
	}

	@Override
	public void onWindowFocusChanged(boolean hasFocus) {
		super.onWindowFocusChanged(hasFocus);
		KeilandBridge.nativeFocus(hasFocus);

		/* The clipboard can be read only while the window has the focus. */
		if (hasFocus) {
			sendClipboard();
		}
	}

	@Override
	protected void onDestroy() {
		keyBarWindow.dismiss();
		if (clipboard != null) {
			clipboard.removePrimaryClipChangedListener(clipListener);
		}
		KeilandBridge.detach(this);
		super.onDestroy();
	}

	/** Called at the end of onCreate, for the program's own Activity to set up what it needs. */
	protected void onKeilandCreated() {
	}

	/** The view the input method types into. */
	InputView inputView() {
		return inputView;
	}

	/**
	 * Passes what covers the window to the native code: the system's bars and the cutout, and the
	 * keyboard together with the key bar above it.  The key bar sits on the keyboard and is shown
	 * while the keyboard is.
	 */
	@SuppressWarnings("deprecation")
	private void sendInsets(WindowInsets insets) {
		Insets bars;
		Insets keyboard;
		int covered;

		/* The keyboard's height is known from Android 11; before it the window's insets stand for the bars alone. */
		if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) {
			keyBarWindow.dismiss();
			KeilandBridge.nativeInsets(insets.getSystemWindowInsetLeft(), insets.getSystemWindowInsetTop(),
				insets.getSystemWindowInsetRight(), insets.getSystemWindowInsetBottom(), 0);
			return;
		}

		bars = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
		keyboard = insets.getInsets(WindowInsets.Type.ime());
		covered = 0;
		/* A keyboard that covers most of the window (a full-screen input method) leaves the program nothing to be shown in, so it is not made room for. */
		if (insets.isVisible(WindowInsets.Type.ime()) && keyboard.bottom <= inputView.getHeight() * KEYBOARD_MAX_PERCENT / 100) {
			covered = keyboard.bottom + keyBar.heightPixels();
			if (keyBarWindow.isShowing()) {
				keyBarWindow.update(0, keyboard.bottom, ViewGroup.LayoutParams.MATCH_PARENT, keyBar.heightPixels());
			} else if (inputView.getWindowToken() != null) {
				keyBarWindow.showAtLocation(inputView, Gravity.BOTTOM | Gravity.START, 0, keyboard.bottom);
			}
		} else {
			keyBarWindow.dismiss();
		}
		KeilandBridge.nativeInsets(bars.left, bars.top, bars.right, bars.bottom, covered);
	}

	/** Passes the look of the system to the native code. */
	private void sendConfig(Configuration configuration) {
		boolean dark;
		String locale;

		dark = (configuration.uiMode & Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_YES;
		locale = configuration.getLocales().get(0).toLanguageTag();
		KeilandBridge.nativeConfig(dark, -1, configuration.fontScale, locale);
	}

	/** Passes the clipboard's text, or null for none, to the native code. */
	private void sendClipboard() {
		ClipData clip;
		CharSequence text;

		text = null;
		if (clipboard != null && clipboard.hasPrimaryClip()) {
			clip = clipboard.getPrimaryClip();
			if (clip != null && clip.getItemCount() > 0) {
				text = clip.getItemAt(0).coerceToText(this);
			}
		}
		if (text == null || text.length() == 0) {
			KeilandBridge.nativeClipboard(null);
		} else {
			KeilandBridge.nativeClipboard(text.toString());
		}
	}
}
