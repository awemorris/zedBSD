/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

package io.noctvm.keiland;

import android.content.Context;
import android.graphics.Color;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.widget.Button;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;

/**
 * The row of keys that a soft keyboard has not got and a terminal needs: Esc, Tab, Ctrl, Alt, the
 * arrows and a few symbols.  It is a view of the system, shown over the keyboard while the keyboard is
 * up, so the native code draws nothing of it.
 *
 * Ctrl and Alt hold until the next key is pressed (pressing one again lets it go).  The other
 * buttons send the key at once, as a press and a release.  The buttons never take the focus, so the
 * keyboard stays up.
 */
final class KeyBar extends HorizontalScrollView {
	/** The bar's height, in dp. */
	static final int HEIGHT_DP = 44;

	private static final int BACKGROUND = Color.rgb(0x20, 0x21, 0x24);
	private static final int BUTTON = Color.rgb(0x3c, 0x40, 0x43);
	private static final int LATCHED = Color.rgb(0x8a, 0xb4, 0xf8);

	/** The meta bits that are held for the next key (KeyEvent.META_CTRL_ON and META_ALT_ON). */
	private int latched;

	private Button ctrlButton;
	private Button altButton;

	KeyBar(Context context) {
		super(context);
		LinearLayout row;

		setBackgroundColor(BACKGROUND);
		setHorizontalScrollBarEnabled(false);
		setFillViewport(true);
		row = new LinearLayout(context);
		row.setOrientation(LinearLayout.HORIZONTAL);
		row.setGravity(Gravity.CENTER_VERTICAL);
		addView(row, new LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.MATCH_PARENT));

		addKey(row, "Esc", KeyEvent.KEYCODE_ESCAPE, 0);
		addKey(row, "Tab", KeyEvent.KEYCODE_TAB, 0);
		ctrlButton = addLatch(row, "Ctrl", KeyEvent.META_CTRL_ON);
		altButton = addLatch(row, "Alt", KeyEvent.META_ALT_ON);
		addKey(row, "←", KeyEvent.KEYCODE_DPAD_LEFT, 0);
		addKey(row, "↓", KeyEvent.KEYCODE_DPAD_DOWN, 0);
		addKey(row, "↑", KeyEvent.KEYCODE_DPAD_UP, 0);
		addKey(row, "→", KeyEvent.KEYCODE_DPAD_RIGHT, 0);
		addKey(row, "Home", KeyEvent.KEYCODE_MOVE_HOME, 0);
		addKey(row, "End", KeyEvent.KEYCODE_MOVE_END, 0);
		addKey(row, "PgUp", KeyEvent.KEYCODE_PAGE_UP, 0);
		addKey(row, "PgDn", KeyEvent.KEYCODE_PAGE_DOWN, 0);
		addKey(row, "|", KeyEvent.KEYCODE_BACKSLASH, KeyEvent.META_SHIFT_ON);
		addKey(row, "~", KeyEvent.KEYCODE_GRAVE, KeyEvent.META_SHIFT_ON);
		addKey(row, "/", KeyEvent.KEYCODE_SLASH, 0);
		addKey(row, "-", KeyEvent.KEYCODE_MINUS, 0);
	}

	/** Returns the bar's height in pixels. */
	int heightPixels() {
		return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, HEIGHT_DP, getResources().getDisplayMetrics()));
	}

	/** Returns the meta bits held now and lets them go: Ctrl and Alt hold for one key. */
	int takeLatched() {
		int meta;

		meta = latched;
		if (meta != 0) {
			latched = 0;
			showLatched();
		}
		return meta;
	}

	/** A button that sends a key at once. */
	private void addKey(LinearLayout row, String label, final int keycode, final int meta) {
		Button button;

		button = makeButton(label);
		button.setOnClickListener(new View.OnClickListener() {
			@Override
			public void onClick(View view) {
				int held;

				held = takeLatched() | meta;
				KeilandBridge.nativeKey(keycode, held, true);
				KeilandBridge.nativeKey(keycode, held, false);
			}
		});
		row.addView(button, new LinearLayout.LayoutParams(buttonWidth(label), LayoutParams.MATCH_PARENT));
	}

	/** A button that holds a modifier for the next key. */
	private Button addLatch(LinearLayout row, String label, final int bit) {
		Button button;

		button = makeButton(label);
		button.setOnClickListener(new View.OnClickListener() {
			@Override
			public void onClick(View view) {
				latched ^= bit;
				showLatched();
			}
		});
		row.addView(button, new LinearLayout.LayoutParams(buttonWidth(label), LayoutParams.MATCH_PARENT));
		return button;
	}

	private Button makeButton(String label) {
		Button button;

		button = new Button(getContext());
		button.setText(label);
		button.setAllCaps(false);
		button.setTextColor(Color.WHITE);
		button.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
		button.setBackgroundColor(BUTTON);
		button.setMinWidth(0);
		button.setMinimumWidth(0);
		button.setPadding(0, 0, 0, 0);
		button.setFocusable(false);
		button.setFocusableInTouchMode(false);
		return button;
	}

	/** A button's width in pixels: wider for a long label. */
	private int buttonWidth(String label) {
		int dp;

		dp = label.length() > 2 ? 56 : 44;
		return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, dp, getResources().getDisplayMetrics()));
	}

	/** Shows which of Ctrl and Alt are held. */
	private void showLatched() {
		ctrlButton.setBackgroundColor((latched & KeyEvent.META_CTRL_ON) != 0 ? LATCHED : BUTTON);
		ctrlButton.setTextColor((latched & KeyEvent.META_CTRL_ON) != 0 ? Color.BLACK : Color.WHITE);
		altButton.setBackgroundColor((latched & KeyEvent.META_ALT_ON) != 0 ? LATCHED : BUTTON);
		altButton.setTextColor((latched & KeyEvent.META_ALT_ON) != 0 ? Color.BLACK : Color.WHITE);
	}

	/**
	 * Returns the Android key code of an ASCII character on a US keyboard, 0 for none; the bits of the
	 * meta state that the character needs (Shift for a capital and for the shifted symbols) are put in
	 * need[0].
	 */
	static int keycodeOf(char c, int[] need) {
		String shifted;
		String plain;
		int index;

		need[0] = 0;
		if (c >= 'a' && c <= 'z') {
			return KeyEvent.KEYCODE_A + (c - 'a');
		}
		if (c >= 'A' && c <= 'Z') {
			need[0] = KeyEvent.META_SHIFT_ON;
			return KeyEvent.KEYCODE_A + (c - 'A');
		}
		if (c >= '0' && c <= '9') {
			return KeyEvent.KEYCODE_0 + (c - '0');
		}
		switch (c) {
		case ' ':
			return KeyEvent.KEYCODE_SPACE;
		case '\n':
			return KeyEvent.KEYCODE_ENTER;
		case '\t':
			return KeyEvent.KEYCODE_TAB;
		case '-':
			return KeyEvent.KEYCODE_MINUS;
		case '=':
			return KeyEvent.KEYCODE_EQUALS;
		case '[':
			return KeyEvent.KEYCODE_LEFT_BRACKET;
		case ']':
			return KeyEvent.KEYCODE_RIGHT_BRACKET;
		case '\\':
			return KeyEvent.KEYCODE_BACKSLASH;
		case ';':
			return KeyEvent.KEYCODE_SEMICOLON;
		case '\'':
			return KeyEvent.KEYCODE_APOSTROPHE;
		case '`':
			return KeyEvent.KEYCODE_GRAVE;
		case ',':
			return KeyEvent.KEYCODE_COMMA;
		case '.':
			return KeyEvent.KEYCODE_PERIOD;
		case '/':
			return KeyEvent.KEYCODE_SLASH;
		default:
			break;
		}

		/* The shifted symbols of the number row and of the others. */
		shifted = ")!@#$%^&*(";
		index = shifted.indexOf(c);
		if (index >= 0) {
			need[0] = KeyEvent.META_SHIFT_ON;
			return KeyEvent.KEYCODE_0 + index;
		}
		plain = "_+{}|:\"~<>?";
		index = plain.indexOf(c);
		if (index >= 0) {
			int[] codes = {
				KeyEvent.KEYCODE_MINUS, KeyEvent.KEYCODE_EQUALS, KeyEvent.KEYCODE_LEFT_BRACKET,
				KeyEvent.KEYCODE_RIGHT_BRACKET, KeyEvent.KEYCODE_BACKSLASH, KeyEvent.KEYCODE_SEMICOLON,
				KeyEvent.KEYCODE_APOSTROPHE, KeyEvent.KEYCODE_GRAVE, KeyEvent.KEYCODE_COMMA,
				KeyEvent.KEYCODE_PERIOD, KeyEvent.KEYCODE_SLASH
			};
			need[0] = KeyEvent.META_SHIFT_ON;
			return codes[index];
		}
		return 0;
	}
}
