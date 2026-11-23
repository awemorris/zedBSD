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
		return new KeilandInputConnection(this);
	}

	/** Chooses the kind of input; a different one restarts the input method's session. */
	void setMode(int newMode) {
		InputMethodManager manager;

		if (newMode == mode) {
			return;
		}
		mode = newMode;
		manager = (InputMethodManager) getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
		if (manager != null) {
			manager.restartInput(this);
		}
	}

	/** Shows the soft keyboard for this view, or hides it. */
	void showKeyboard(boolean show) {
		InputMethodManager manager;

		manager = (InputMethodManager) getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
		if (manager == null) {
			return;
		}
		if (show) {
			requestFocus();
			manager.showSoftInput(this, 0);
		} else {
			manager.hideSoftInputFromWindow(getWindowToken(), 0);
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
