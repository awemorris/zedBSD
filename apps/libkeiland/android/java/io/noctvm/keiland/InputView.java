/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

package io.noctvm.keiland;

import android.content.Context;
import android.graphics.Matrix;
import android.view.View;
import android.view.inputmethod.CursorAnchorInfo;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;

/**
 * The invisible view that the input method types into.
 *
 * It covers the window without drawing anything, so the program's own drawing shows through.  It
 * takes the focus when the keyboard is shown, and gives the input method a connection that passes
 * everything to the native code.
 */
final class InputView extends View {
	/** Letters only, with no suggestions or corrections (0), or a language's input (1). */
	private int mode;

	/** The key bar shown over the keyboard, whose Ctrl and Alt hold for the next character typed. */
	private KeyBar keyBar;

	/** The connection the input method has now, or null. */
	private KeilandInputConnection connection;

	/** Whether the keyboard was asked for before the window could show it (it has no focus yet). */
	private boolean showPending;

	InputView(Context context) {
		super(context);
		setFocusable(true);
		setFocusableInTouchMode(true);
	}

	@Override
	public boolean onCheckIsTextEditor() {
		return true;
	}

	@Override
	public InputConnection onCreateInputConnection(EditorInfo info) {
		if (mode == 0) {
			info.inputType = android.text.InputType.TYPE_CLASS_TEXT
				| android.text.InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
				| android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;
		} else {
			info.inputType = android.text.InputType.TYPE_CLASS_TEXT
				| android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;
		}
		info.imeOptions = EditorInfo.IME_FLAG_NO_FULLSCREEN | EditorInfo.IME_FLAG_NO_EXTRACT_UI;
		connection = new KeilandInputConnection(this);
		return connection;
	}

	/** Tells the input method's connection that the program's text moved on without it (a key of the key bar, Enter). */
	void forgetContext() {
		if (connection != null) {
			connection.forgetContext();
		}
	}

	/** Takes the key bar whose held modifiers the typed characters get. */
	void setKeyBar(KeyBar bar) {
		keyBar = bar;
	}

	/** Returns the meta bits the key bar holds for the next character, and lets them go (0 when it holds none). */
	int takeLatched() {
		if (keyBar == null) {
			return 0;
		}
		return keyBar.takeLatched();
	}

	/** Chooses the kind of input; a different one restarts the input method's session. */
	void setMode(int newMode) {
		InputMethodManager manager;

		if (newMode == mode) {
			return;
		}
		mode = newMode;
		forgetContext();
		manager = (InputMethodManager) getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
		if (manager != null) {
			manager.restartInput(this);
		}
	}

	/**
	 * Shows the soft keyboard for this view, or hides it.  The input method refuses to show while
	 * the window has no focus, so a request made too early is kept and carried out when the window
	 * gains the focus.
	 */
	void showKeyboard(boolean show) {
		InputMethodManager manager;

		manager = (InputMethodManager) getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
		if (manager == null) {
			return;
		}
		if (show) {
			requestFocus();
			showPending = true;
			if (hasWindowFocus()) {
				showPending = !manager.showSoftInput(this, 0);
			}
		} else {
			showPending = false;
			manager.hideSoftInputFromWindow(getWindowToken(), 0);
		}
	}

	@Override
	public void onWindowFocusChanged(boolean hasWindowFocus) {
		InputMethodManager manager;

		super.onWindowFocusChanged(hasWindowFocus);

		/* Carries out a request that had to wait for the focus. */
		if (hasWindowFocus && showPending) {
			manager = (InputMethodManager) getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
			if (manager != null) {
				showPending = !manager.showSoftInput(this, 0);
			}
		}
	}

	/** Tells the input method where the text being composed is, in the window's pixels. */
	void setCursorAnchor(float x, float y, float width, float height) {
		InputMethodManager manager;
		CursorAnchorInfo.Builder builder;
		int[] origin;
		Matrix matrix;

		manager = (InputMethodManager) getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
		if (manager == null) {
			return;
		}

		/* The matrix takes the window's pixels to the screen's. */
		origin = new int[2];
		getLocationOnScreen(origin);
		matrix = new Matrix();
		matrix.setTranslate(origin[0], origin[1]);

		builder = new CursorAnchorInfo.Builder();
		builder.setMatrix(matrix);
		builder.setInsertionMarkerLocation(x, y, y + height, y + height, CursorAnchorInfo.FLAG_HAS_VISIBLE_REGION);
		manager.updateCursorAnchorInfo(this, builder.build());
	}
}
