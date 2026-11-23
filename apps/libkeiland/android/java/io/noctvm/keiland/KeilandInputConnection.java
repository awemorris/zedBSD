/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

package io.noctvm.keiland;

import android.view.KeyEvent;
import android.view.inputmethod.BaseInputConnection;

/**
 * The input method's connection to the program: what it commits, composes and deletes is passed on
 * to the native code.
 *
 * The program keeps its own text, so the editable behind this connection stays empty; what the input
 * method is told of the text around its cursor is what this connection itself kept: the text the
 * input method committed lately (the context), and the text being composed after it.  A keyboard that
 * predicts and corrects reads that context, marks the word it means to change as composing again
 * (setComposingRegion) and commits another in its place; the word it marks has already gone to the
 * program, so it is taken back (as backspaces) and shown as composing text again.  Without this the
 * corrected word would be added after the one that was typed.  The cursor is always at the end: a
 * program's own cursor cannot be moved from here.
 */
final class KeilandInputConnection extends BaseInputConnection {
	/** The most the context keeps, in characters: a prediction keyboard looks back a few words only. */
	private static final int CONTEXT_MAX = 200;

	/** The text committed lately (what the input method may take back), before the composing text. */
	private final StringBuilder context = new StringBuilder();

	/** The text being composed now, or the empty string. */
	private String composing = "";

	/** The view, whose key bar holds Ctrl and Alt for the next character. */
	private final InputView inputView;

	KeilandInputConnection(InputView view) {
		super(view, true);
		inputView = view;
	}

	/** Forgets the text kept: a key that did not come through here (Enter, an arrow) moved the program's text on. */
	void forgetContext() {
		context.setLength(0);
		composing = "";
	}

	@Override
	public CharSequence getTextBeforeCursor(int n, int flags) {
		String all;

		all = context.toString() + composing;
		if (n >= all.length()) {
			return all;
		}
		return all.substring(all.length() - n);
	}

	@Override
	public CharSequence getTextAfterCursor(int n, int flags) {
		return "";
	}

	@Override
	public CharSequence getSelectedText(int flags) {
		return null;
	}

	@Override
	public boolean setSelection(int start, int end) {
		/* The cursor stays at the end. */
		return true;
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
		keep(committed);
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
	public boolean setComposingRegion(int start, int end) {
		String all;
		String taken;
		int from;
		int to;
		int mark;

		/* Only the end of the text can be taken back (the program's text before it is out of reach). */
		all = context.toString() + composing;
		from = Math.max(0, Math.min(start, end));
		to = Math.min(all.length(), Math.max(start, end));
		if (to != all.length() || from > to) {
			return false;
		}
		if (from == to) {
			return true;
		}

		/* The part of the region that was committed goes back from the program to be composed again. */
		if (from < context.length()) {
			taken = context.substring(from);
			KeilandBridge.nativeDelete(taken.codePointCount(0, taken.length()), 0);
			context.setLength(from);
			composing = taken + composing;
		} else {
			composing = all.substring(from);
		}
		mark = composing.codePointCount(0, composing.length());
		KeilandBridge.nativeCompose(composing, mark);
		return true;
	}

	@Override
	public boolean finishComposingText() {
		String pending;

		/* What was composed stays, so it becomes a commit. */
		pending = composing;
		composing = "";
		if (!pending.isEmpty()) {
			keep(pending);
			KeilandBridge.nativeCommit(pending);
		}
		return true;
	}

	@Override
	public boolean deleteSurroundingText(int beforeLength, int afterLength) {
		String taken;
		int left;
		int fromComposing;

		/* The characters before the cursor go: the composing text's end first, then the committed text's. */
		left = Math.max(0, beforeLength);
		if (left > 0 && !composing.isEmpty()) {
			fromComposing = Math.min(left, composing.length());
			composing = composing.substring(0, composing.length() - fromComposing);
			left -= fromComposing;
			KeilandBridge.nativeCompose(composing, composing.codePointCount(0, composing.length()));
		}
		if (left > 0) {
			left = Math.min(left, context.length());
			taken = context.substring(context.length() - left);
			context.setLength(context.length() - left);
			KeilandBridge.nativeDelete(taken.codePointCount(0, taken.length()), 0);
		}
		return true;
	}

	@Override
	public boolean deleteSurroundingTextInCodePoints(int beforeLength, int afterLength) {
		int units;

		/* The same, in the characters the context counts. */
		units = 0;
		if (beforeLength > 0) {
			String all = context.toString() + composing;
			int index = all.length();
			int count = 0;

			while (count < beforeLength && index > 0) {
				index = all.offsetByCodePoints(index, -1);
				count++;
			}
			units = all.length() - index;
		}
		return deleteSurroundingText(units, 0);
	}

	@Override
	public boolean sendKeyEvent(KeyEvent event) {
		/* A Backspace takes the last character of the context; Enter and the keys that move the cursor end it. */
		if (event.getAction() == KeyEvent.ACTION_DOWN) {
			if (event.getKeyCode() == KeyEvent.KEYCODE_DEL && context.length() > 0 && composing.isEmpty()) {
				context.setLength(context.length() - 1);
			} else if (event.getKeyCode() != KeyEvent.KEYCODE_SHIFT_LEFT && event.getKeyCode() != KeyEvent.KEYCODE_SHIFT_RIGHT) {
				if (event.getKeyCode() != KeyEvent.KEYCODE_DEL) {
					forgetContext();
				}
			}
		}
		return super.sendKeyEvent(event);
	}

	/** Adds committed text to the context, which keeps its last characters. */
	private void keep(String committed) {
		int excess;

		context.append(committed);
		excess = context.length() - CONTEXT_MAX;
		if (excess > 0) {
			context.delete(0, excess);
		}
	}
}
