/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

package io.noctvm.keiland;

import android.view.inputmethod.BaseInputConnection;

/**
 * The input method's connection to the program: what it commits, composes and deletes is passed on
 * to the native code, and nothing of the program's text is shown to the input method.
 *
 * The program keeps its own text, so the editable behind this connection stays empty.  The text
 * still being composed is kept here only to turn "finish composing" into a commit.
 */
final class KeilandInputConnection extends BaseInputConnection {
	/** The text being composed now, or the empty string. */
	private String composing = "";

	/** The view, whose key bar holds Ctrl and Alt for the next character. */
	private final InputView inputView;

	KeilandInputConnection(InputView view) {
		super(view, true);
		inputView = view;
	}

	@Override
	public boolean commitText(CharSequence text, int newCursorPosition) {
		String committed;
		int[] need;
		int code;
		int meta;

		/* The committed text replaces whatever was being composed. */
		composing = "";
		committed = text.toString();

		/* A character typed while Ctrl or Alt is held is that key with them, not text. */
		meta = inputView.takeLatched();
		if (meta != 0 && committed.length() == 1) {
			need = new int[1];
			code = KeyBar.keycodeOf(committed.charAt(0), need);
			if (code != 0) {
				KeilandBridge.nativeKey(code, meta | need[0], true);
				KeilandBridge.nativeKey(code, meta | need[0], false);
				return true;
			}
		}
		KeilandBridge.nativeCommit(committed);
		return true;
	}

	@Override
	public boolean setComposingText(CharSequence text, int newCursorPosition) {
		String composed;
		int length;
		int cursor;

		composed = text.toString();
		composing = composed;

		/* The cursor is counted from the end of the text when positive and from its start otherwise. */
		length = composed.codePointCount(0, composed.length());
		if (newCursorPosition > 0) {
			cursor = length + newCursorPosition - 1;
		} else {
			cursor = newCursorPosition;
		}
		if (cursor < 0) {
			cursor = 0;
		}
		if (cursor > length) {
			cursor = length;
		}

		KeilandBridge.nativeCompose(composed, cursor);
		return true;
	}

	@Override
	public boolean finishComposingText() {
		String pending;

		/* What was composed stays, so it becomes a commit. */
		pending = composing;
		composing = "";
		if (!pending.isEmpty()) {
			KeilandBridge.nativeCommit(pending);
		}
		return true;
	}

	@Override
	public boolean deleteSurroundingText(int beforeLength, int afterLength) {
		KeilandBridge.nativeDelete(beforeLength, afterLength);
		return true;
	}

	@Override
	public boolean deleteSurroundingTextInCodePoints(int beforeLength, int afterLength) {
		KeilandBridge.nativeDelete(beforeLength, afterLength);
		return true;
	}
}
