/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The keys of the window (ws074-p056): evdev key codes to the names the
 * DOM gives a key (its code, its key with and without Shift) and the text
 * it types, which the view takes (<browser/browser.h>, browser_view_key).
 *
 * zedDesktop sends evdev codes (userland/desktop/wayland/seat.c) and the
 * browser, like the terminal, carries the US layout itself.  A key missing
 * from the table is "Unidentified", as the DOM calls a key it cannot name.
 * When the window's backend tells the character a key types, that character
 * is the DOM's key instead of the table's (a layout other than the US one).
 */

#include "shell/internal.h"

#include <string.h>

/*
 * One key of the keyboard: its evdev code, its DOM code, its key without
 * Shift and with it (the same for a key that types nothing), and whether
 * it types its key as text.
 */
struct keys_entry {
	uint32_t evdev;
	const char *code;
	const char *key;
	const char *shifted;
	int types;
};

/*
 * The keys the window names, by evdev code, the US layout.  The table is
 * constant for the life of the program and ends with a NULL code.
 */
static const struct keys_entry keys_table[] = {
	{ 1U, "Escape", "Escape", "Escape", 0 },
	{ 2U, "Digit1", "1", "!", 1 },
	{ 3U, "Digit2", "2", "@", 1 },
	{ 4U, "Digit3", "3", "#", 1 },
	{ 5U, "Digit4", "4", "$", 1 },
	{ 6U, "Digit5", "5", "%", 1 },
	{ 7U, "Digit6", "6", "^", 1 },
	{ 8U, "Digit7", "7", "&", 1 },
	{ 9U, "Digit8", "8", "*", 1 },
	{ 10U, "Digit9", "9", "(", 1 },
	{ 11U, "Digit0", "0", ")", 1 },
	{ 12U, "Minus", "-", "_", 1 },
	{ 13U, "Equal", "=", "+", 1 },
	{ 14U, "Backspace", "Backspace", "Backspace", 0 },
	{ 15U, "Tab", "Tab", "Tab", 0 },
	{ 16U, "KeyQ", "q", "Q", 1 },
	{ 17U, "KeyW", "w", "W", 1 },
	{ 18U, "KeyE", "e", "E", 1 },
	{ 19U, "KeyR", "r", "R", 1 },
	{ 20U, "KeyT", "t", "T", 1 },
	{ 21U, "KeyY", "y", "Y", 1 },
	{ 22U, "KeyU", "u", "U", 1 },
	{ 23U, "KeyI", "i", "I", 1 },
	{ 24U, "KeyO", "o", "O", 1 },
	{ 25U, "KeyP", "p", "P", 1 },
	{ 26U, "BracketLeft", "[", "{", 1 },
	{ 27U, "BracketRight", "]", "}", 1 },
	{ 28U, "Enter", "Enter", "Enter", 0 },
	{ 29U, "ControlLeft", "Control", "Control", 0 },
	{ 30U, "KeyA", "a", "A", 1 },
	{ 31U, "KeyS", "s", "S", 1 },
	{ 32U, "KeyD", "d", "D", 1 },
	{ 33U, "KeyF", "f", "F", 1 },
	{ 34U, "KeyG", "g", "G", 1 },
	{ 35U, "KeyH", "h", "H", 1 },
	{ 36U, "KeyJ", "j", "J", 1 },
	{ 37U, "KeyK", "k", "K", 1 },
	{ 38U, "KeyL", "l", "L", 1 },
	{ 39U, "Semicolon", ";", ":", 1 },
	{ 40U, "Quote", "'", "\"", 1 },
	{ 41U, "Backquote", "`", "~", 1 },
	{ 42U, "ShiftLeft", "Shift", "Shift", 0 },
	{ 43U, "Backslash", "\\", "|", 1 },
	{ 44U, "KeyZ", "z", "Z", 1 },
	{ 45U, "KeyX", "x", "X", 1 },
	{ 46U, "KeyC", "c", "C", 1 },
	{ 47U, "KeyV", "v", "V", 1 },
	{ 48U, "KeyB", "b", "B", 1 },
	{ 49U, "KeyN", "n", "N", 1 },
	{ 50U, "KeyM", "m", "M", 1 },
	{ 51U, "Comma", ",", "<", 1 },
	{ 52U, "Period", ".", ">", 1 },
	{ 53U, "Slash", "/", "?", 1 },
	{ 54U, "ShiftRight", "Shift", "Shift", 0 },
	{ 55U, "NumpadMultiply", "*", "*", 1 },
	{ 56U, "AltLeft", "Alt", "Alt", 0 },
	{ 57U, "Space", " ", " ", 1 },
	{ 58U, "CapsLock", "CapsLock", "CapsLock", 0 },
	{ 59U, "F1", "F1", "F1", 0 },
	{ 60U, "F2", "F2", "F2", 0 },
	{ 61U, "F3", "F3", "F3", 0 },
	{ 62U, "F4", "F4", "F4", 0 },
	{ 63U, "F5", "F5", "F5", 0 },
	{ 64U, "F6", "F6", "F6", 0 },
	{ 65U, "F7", "F7", "F7", 0 },
	{ 66U, "F8", "F8", "F8", 0 },
	{ 67U, "F9", "F9", "F9", 0 },
	{ 68U, "F10", "F10", "F10", 0 },
	{ 87U, "F11", "F11", "F11", 0 },
	{ 88U, "F12", "F12", "F12", 0 },
	{ 96U, "NumpadEnter", "Enter", "Enter", 0 },
	{ 97U, "ControlRight", "Control", "Control", 0 },
	{ 100U, "AltRight", "Alt", "Alt", 0 },
	{ 102U, "Home", "Home", "Home", 0 },
	{ 103U, "ArrowUp", "ArrowUp", "ArrowUp", 0 },
	{ 104U, "PageUp", "PageUp", "PageUp", 0 },
	{ 105U, "ArrowLeft", "ArrowLeft", "ArrowLeft", 0 },
	{ 106U, "ArrowRight", "ArrowRight", "ArrowRight", 0 },
	{ 107U, "End", "End", "End", 0 },
	{ 108U, "ArrowDown", "ArrowDown", "ArrowDown", 0 },
	{ 109U, "PageDown", "PageDown", "PageDown", 0 },
	{ 110U, "Insert", "Insert", "Insert", 0 },
	{ 111U, "Delete", "Delete", "Delete", 0 },
	{ 125U, "MetaLeft", "Meta", "Meta", 0 },
	{ 126U, "MetaRight", "Meta", "Meta", 0 },
	{ 128U, "BrowserStop", "BrowserStop", "BrowserStop", 0 },
	{ 158U, "BrowserBack", "BrowserBack", "BrowserBack", 0 },
	{ 159U, "BrowserForward", "BrowserForward", "BrowserForward", 0 },
	{ 173U, "BrowserRefresh", "BrowserRefresh", "BrowserRefresh", 0 },
	{ 0U, NULL, NULL, NULL, 0 }
};

static int keys_single_character(const char *text);

/*
 * Names an evdev key for the view: its DOM code and key (the shifted key
 * with Shift held), and the text it types (a character key without
 * Control, Alt or Meta; "" otherwise).  typed is the character the backend
 * says the key types, in UTF-8 ("" when it does not say): when it is one
 * printable character it is the key and the text, in place of the table's.
 * The names live for the life of the program, except the key and text
 * taken from typed, which live in names.
 */
void
shell_key_names(
	uint32_t evdev,
	uint32_t modifiers,
	const char *typed,
	struct shell_key_names *names)
{
	const struct keys_entry *entry;
	size_t index;
	int printable;

	/* A key the table does not have. */
	names->code = "Unidentified";
	names->key = "Unidentified";
	names->text = "";
	names->typed[0] = '\0';
	printable = keys_single_character(typed);
	if (printable) {
		memcpy(names->typed, typed, strlen(typed) + 1U);
		names->key = names->typed;
	}

	/* The key's entry. */
	entry = NULL;
	for (index = 0; keys_table[index].code != NULL; index++) {
		if (keys_table[index].evdev == evdev) {
			entry = &keys_table[index];
			break;
		}
	}

	/* A key the table does not have keeps the names it was given (a character the backend named is its key and its text). */
	if (entry == NULL) {
		if (printable && (modifiers & (SHELL_MOD_CTRL | SHELL_MOD_ALT | SHELL_MOD_META)) == 0U)
			names->text = names->typed;
		return;
	}

	/* Its code, and its key with or without Shift, or the character the backend named. */
	names->code = entry->code;
	if (!printable) {
		names->key = entry->key;
		if ((modifiers & SHELL_MOD_SHIFT) != 0U)
			names->key = entry->shifted;
	}

	/* A character key types its key unless Control, Alt or Meta is held. */
	if (!entry->types && !printable)
		return;
	if ((modifiers & (SHELL_MOD_CTRL | SHELL_MOD_ALT | SHELL_MOD_META)) != 0U)
		return;
	names->text = names->key;
}

/*
 * Tells whether a text is one printable character in UTF-8: a well-formed
 * sequence that is not a control character (C0, DEL, or C1).
 */
static int
keys_single_character(
	const char *text)
{
	const unsigned char *bytes;
	size_t length;
	size_t index;

	/* Nothing is not a character. */
	if (text == NULL || text[0] == '\0')
		return 0;

	/* The sequence's length by its first byte; a control character is not printable. */
	bytes = (const unsigned char *)text;
	if (bytes[0] < 0x20U || bytes[0] == 0x7FU)
		return 0;
	if (bytes[0] < 0x80U)
		length = 1U;
	else if (bytes[0] >= 0xC2U && bytes[0] <= 0xDFU)
		length = 2U;
	else if (bytes[0] >= 0xE0U && bytes[0] <= 0xEFU)
		length = 3U;
	else if (bytes[0] >= 0xF0U && bytes[0] <= 0xF4U)
		length = 4U;
	else
		return 0;

	/* Its continuation bytes, and nothing after it. */
	for (index = 1U; index < length; index++) {
		if ((bytes[index] & 0xC0U) != 0x80U)
			return 0;
	}
	if (bytes[length] != 0U)
		return 0;

	/* A C1 control character (U+0080 to U+009F) is not printable. */
	if (bytes[0] == 0xC2U && bytes[1] < 0xA0U)
		return 0;

	/* Succeeded: one printable character. */
	return 1;
}

/* Turns the window's modifier bits into the view's. */
uint32_t
shell_key_modifiers(
	uint32_t modifiers)
{
	uint32_t bits;

	/* Each modifier held. */
	bits = 0U;
	if ((modifiers & SHELL_MOD_SHIFT) != 0U)
		bits |= BROWSER_MOD_SHIFT;
	if ((modifiers & SHELL_MOD_CTRL) != 0U)
		bits |= BROWSER_MOD_CTRL;
	if ((modifiers & SHELL_MOD_ALT) != 0U)
		bits |= BROWSER_MOD_ALT;
	if ((modifiers & SHELL_MOD_META) != 0U)
		bits |= BROWSER_MOD_META;

	/* The view's bits. */
	return bits;
}

/*
 * Turns an evdev pointer button into the DOM's number (BROWSER_BUTTON_*);
 * -1 for a button the DOM does not number.
 */
int
shell_key_button(
	uint32_t button)
{
	/* The left, right and middle buttons, and the side buttons that go back and forward. */
	switch (button) {
	case SHELL_BUTTON_LEFT:
		return BROWSER_BUTTON_PRIMARY;
	case SHELL_BUTTON_RIGHT:
		return BROWSER_BUTTON_SECONDARY;
	case SHELL_BUTTON_MIDDLE:
		return BROWSER_BUTTON_MIDDLE;
	case SHELL_BUTTON_SIDE:
		return BROWSER_BUTTON_BACK;
	case SHELL_BUTTON_EXTRA:
		return BROWSER_BUTTON_FORWARD;
	default:
		break;
	}

	/* Any other button. */
	return -1;
}
