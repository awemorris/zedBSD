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
import android.os.Bundle;
import android.view.ViewGroup;

/**
 * The Activity of a libkeiland program on Android: a NativeActivity whose native library draws the
 * window, with an invisible view over it for the input method.  A program's own Activity derives
 * from it and loads the program's native library in a static initializer, so that the library finds
 * the program's Java classes.
 */
public class KeilandActivity extends NativeActivity {
	/** The view the input method types into. */
	private InputView inputView;

	@Override
	protected void onCreate(Bundle savedInstanceState) {
		super.onCreate(savedInstanceState);

		/* Lays the input view over the window; setContentView would replace the surface. */
		inputView = new InputView(this);
		addContentView(inputView, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

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
		ClipboardManager clipboard;
		ClipData clip;
		CharSequence text;

		text = null;
		clipboard = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
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
