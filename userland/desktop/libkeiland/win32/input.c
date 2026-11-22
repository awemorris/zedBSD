/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The pointer and keyboard details of the Windows backend.
 *
 * The bit layouts are those of the Win32 messages: the wheel delta is a
 * signed 16-bit value in WM_MOUSEWHEEL and WM_MOUSEHWHEEL, a mouse
 * position is two signed 16-bit words of lParam, and a key message's
 * lParam carries the scan code in bits 16 to 23, the extended-key flag in
 * bit 24, the previous key state in bit 30 and the transition state in bit
 * 31.
 */

#include "userland/desktop/libkeiland/win32/input.h"

#include <keiland/keiland_key.h>
#include <keiland/keiland_menu.h>
#include <keiland/keiland_window.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The evdev code of the first side button (BTN_SIDE); the next one (BTN_EXTRA) follows it. */
#define EVDEV_BUTTON_SIDE	0x113U
#define EVDEV_BUTTON_EXTRA	0x114U

/* The sign bit of a 16-bit word, and the size of the 16-bit range. */
#define WORD_SIGN_BIT		0x8000U
#define WORD_RANGE		0x10000

/* The evdev codes of the keys that type no character and are named in a shortcut. */
#define EVDEV_KEY_ESC		1U
#define EVDEV_KEY_BACKSPACE	14U
#define EVDEV_KEY_TAB		15U
#define EVDEV_KEY_ENTER		28U
#define EVDEV_KEY_F1		59U
#define EVDEV_KEY_F10		68U
#define EVDEV_KEY_KPMINUS	74U
#define EVDEV_KEY_KPPLUS	78U
#define EVDEV_KEY_F11		87U
#define EVDEV_KEY_F12		88U
#define EVDEV_KEY_KPENTER	96U
#define EVDEV_KEY_HOME		102U
#define EVDEV_KEY_UP		103U
#define EVDEV_KEY_PAGEUP	104U
#define EVDEV_KEY_LEFT		105U
#define EVDEV_KEY_RIGHT		106U
#define EVDEV_KEY_END		107U
#define EVDEV_KEY_DOWN		108U
#define EVDEV_KEY_PAGEDOWN	109U
#define EVDEV_KEY_INSERT	110U
#define EVDEV_KEY_DELETE	111U

/* The XKB keysyms of the keys that type no character. */
#define KEYSYM_BACKSPACE	0xff08U
#define KEYSYM_TAB		0xff09U
#define KEYSYM_RETURN		0xff0dU
#define KEYSYM_ESCAPE		0xff1bU
#define KEYSYM_HOME		0xff50U
#define KEYSYM_LEFT		0xff51U
#define KEYSYM_UP		0xff52U
#define KEYSYM_RIGHT		0xff53U
#define KEYSYM_DOWN		0xff54U
#define KEYSYM_PAGEUP		0xff55U
#define KEYSYM_PAGEDOWN		0xff56U
#define KEYSYM_END		0xff57U
#define KEYSYM_INSERT		0xff63U
#define KEYSYM_F1		0xffbeU
#define KEYSYM_F12		0xffc9U
#define KEYSYM_DELETE		0xffffU

/* XKB's keysyms of the characters above Latin-1: this plus the code point. */
#define KEYSYM_UNICODE		0x01000000U

/* The last code point Unicode has. */
#define UNICODE_LAST		0x10ffffU

/* Between a small Latin letter and its capital (ASCII and Latin-1). */
#define LETTER_CASE_STEP	0x20U

/* The Latin-1 division sign, which sits among the small letters and is not one. */
#define LATIN1_DIVISION		0xf7U

/* The longest name of a key a shortcut's text shows ("Backspace", a character's UTF-8), with its NUL. */
#define SHORTCUT_NAME		16U

/* Forward declarations. */
static int32_t signed_word(uint64_t word);
static int small_letter(uint32_t code_point);
static void shortcut_key_name(uint32_t keysym, char *name, size_t size);
static size_t utf8_encode(uint32_t code_point, char *bytes);
static void text_append(char *text, size_t size, const char *more);

/*
 * Turns a wheel delta into Keiland's distance in pixels.
 */
double
keiui_win32_wheel_pixels(
	int delta,
	int horizontal)
{
	double pixels;

	/* No movement is plain zero, never a negative zero. */
	if (delta == 0)
		return 0.0;

	/* A notch is KEIUI_WIN32_WHEEL_DELTA units and moves a fixed distance. */
	pixels = KEIUI_WIN32_NOTCH_PIXELS * (double)delta / (double)KEIUI_WIN32_WHEEL_DELTA;

	/* The horizontal wheel has the same direction on both systems. */
	if (horizontal != 0)
		return pixels;

	/* A positive vertical delta scrolls up, which is a negative distance on Keiland. */
	return -pixels;
}

/*
 * Turns the modifier keys held into KL_MOD_* bits.
 */
unsigned
keiui_win32_modifiers(
	unsigned held)
{
	unsigned modifiers;

	/* Starts with no modifier. */
	modifiers = 0;

	/* Shift and the Windows key map one to one. */
	if ((held & KEIUI_WIN32_HELD_SHIFT) != 0)
		modifiers |= KL_MOD_SHIFT;
	if ((held & KEIUI_WIN32_HELD_WIN) != 0)
		modifiers |= KL_MOD_SUPER;

	/* Control and Alt map one to one unless AltGr made them. */
	if ((held & KEIUI_WIN32_HELD_CONTROL) != 0)
		modifiers |= KL_MOD_CTRL;
	if ((held & KEIUI_WIN32_HELD_ALT) != 0)
		modifiers |= KL_MOD_ALT;

	/*
	 * AltGr is a key of its own that types characters.  Windows reports it
	 * as Control and Alt together, which would make every character it types
	 * a shortcut, so both are dropped.
	 */
	if ((held & KEIUI_WIN32_HELD_ALTGR) != 0)
		modifiers &= ~(unsigned)(KL_MOD_CTRL | KL_MOD_ALT);

	/* The modifiers Keiland sees. */
	return modifiers;
}

/*
 * Gives the evdev code of a mouse button.
 */
uint32_t
keiui_win32_button_code(
	unsigned button)
{
	/* Chooses the code of the button; an unknown one has none. */
	switch (button) {
	case KEIUI_WIN32_BUTTON_LEFT:
		return KL_BUTTON_LEFT;
	case KEIUI_WIN32_BUTTON_RIGHT:
		return KL_BUTTON_RIGHT;
	case KEIUI_WIN32_BUTTON_MIDDLE:
		return KL_BUTTON_MIDDLE;
	case KEIUI_WIN32_BUTTON_X1:
		return EVDEV_BUTTON_SIDE;
	case KEIUI_WIN32_BUTTON_X2:
		return EVDEV_BUTTON_EXTRA;
	default:
		break;
	}

	/* No such button. */
	return 0;
}

/*
 * Reads a point from a mouse message's lParam.
 */
void
keiui_win32_point(
	int64_t lparam,
	int32_t *x,
	int32_t *y)
{
	uint64_t bits;

	/* Works on the bit pattern, so a negative lParam shifts predictably. */
	bits = (uint64_t)lparam;

	/* The low word is x. */
	if (x != NULL)
		*x = signed_word(bits & 0xFFFFU);

	/* The next word is y. */
	if (y != NULL)
		*y = signed_word((bits >> 16) & 0xFFFFU);
}

/*
 * Reads a key message's lParam.
 */
void
keiui_win32_key_flags(
	int64_t lparam,
	struct keiui_win32_key_flags *flags)
{
	uint64_t bits;

	/* Does nothing when there is nowhere to put the flags. */
	if (flags == NULL)
		return;

	/* Takes the message's flag bits. */
	bits = (uint64_t)lparam;

	/* Bits 16 to 23 are the scan code. */
	flags->scan = (unsigned)((bits >> 16) & 0xFFU);

	/* Bit 24 says the key is an extended one (right Alt, the arrows). */
	flags->extended = (int)((bits >> 24) & 1U);

	/* Bit 30 is the previous state of the key, reported as it is. */
	flags->repeated = (int)((bits >> 30) & 1U);

	/* Bit 31 is set when the message is for a key going up. */
	flags->released = (int)((bits >> 31) & 1U);
}

/*
 * Reports an action's state among a window's.
 */
unsigned
keiui_win32_action_state(
	const struct keiui_win32_action *actions,
	unsigned count,
	uint32_t action)
{
	unsigned index;

	/* The action's entry. */
	for (index = 0U; index < count; index++) {
		if (actions[index].action == action)
			return actions[index].state;
	}

	/* An action never told is enabled and shown. */
	return 0U;
}

/*
 * Gives the XKB keysym of a key that types no character.
 */
uint32_t
keiui_win32_named_keysym(
	uint32_t code)
{
	/* F1 to F10 follow each other in both numberings. */
	if (code >= EVDEV_KEY_F1 && code <= EVDEV_KEY_F10)
		return KEYSYM_F1 + (code - EVDEV_KEY_F1);

	/* The other keys one by one; a key that types a character has none. */
	switch (code) {
	case EVDEV_KEY_F11:
		return KEYSYM_F1 + 10U;
	case EVDEV_KEY_F12:
		return KEYSYM_F12;
	case EVDEV_KEY_ENTER:
	case EVDEV_KEY_KPENTER:
		return KEYSYM_RETURN;
	case EVDEV_KEY_ESC:
		return KEYSYM_ESCAPE;
	case EVDEV_KEY_TAB:
		return KEYSYM_TAB;
	case EVDEV_KEY_BACKSPACE:
		return KEYSYM_BACKSPACE;
	case EVDEV_KEY_DELETE:
		return KEYSYM_DELETE;
	case EVDEV_KEY_INSERT:
		return KEYSYM_INSERT;
	case EVDEV_KEY_HOME:
		return KEYSYM_HOME;
	case EVDEV_KEY_END:
		return KEYSYM_END;
	case EVDEV_KEY_PAGEUP:
		return KEYSYM_PAGEUP;
	case EVDEV_KEY_PAGEDOWN:
		return KEYSYM_PAGEDOWN;
	case EVDEV_KEY_LEFT:
		return KEYSYM_LEFT;
	case EVDEV_KEY_UP:
		return KEYSYM_UP;
	case EVDEV_KEY_RIGHT:
		return KEYSYM_RIGHT;
	case EVDEV_KEY_DOWN:
		return KEYSYM_DOWN;
	case EVDEV_KEY_KPPLUS:
		return '+';
	case EVDEV_KEY_KPMINUS:
		return '-';
	default:
		break;
	}

	/* A key whose keysym is its character. */
	return 0U;
}

/*
 * Gives the XKB keysym of a character.
 */
uint32_t
keiui_win32_character_keysym(
	uint32_t code_point)
{
	/* No character. */
	if (code_point == 0U)
		return 0U;

	/* Latin-1 is its own keysym. */
	if (code_point <= 0xffU)
		return code_point;

	/* The rest are Unicode's keysyms. */
	return KEYSYM_UNICODE + code_point;
}

/*
 * Works out the keysym and the menu's modifiers of a key press.
 */
int
keiui_win32_shortcut_key(
	uint32_t named,
	uint32_t plain,
	uint32_t typed,
	unsigned modifiers,
	uint32_t *keysym,
	unsigned *menu)
{
	unsigned held;
	int letter;

	/* Somewhere to put the answer. */
	if (keysym == NULL || menu == NULL)
		return 0;

	/* The modifiers in the menu's bits, as held. */
	held = 0U;
	if ((modifiers & KL_MOD_SHIFT) != 0U)
		held |= KL_MENU_SHIFT;
	if ((modifiers & KL_MOD_CTRL) != 0U)
		held |= KL_MENU_CTRL;
	if ((modifiers & KL_MOD_ALT) != 0U)
		held |= KL_MENU_ALT;
	if ((modifiers & KL_MOD_SUPER) != 0U)
		held |= KL_MENU_SUPER;

	/* A key without a character is its name, with every modifier. */
	if (named != 0U) {
		*keysym = named;
		*menu = held;
		return 1;
	}

	/* A key that types nothing in this layout is no shortcut. */
	if (plain == 0U && typed == 0U)
		return 0;

	/* Without what Shift types, the plain character with Shift beside it. */
	if (typed == 0U) {
		*keysym = keiui_win32_character_keysym(plain);
		*menu = held;
		return 1;
	}

	/* A letter is its small form, Shift staying a modifier (Ctrl+Shift+C). */
	letter = small_letter(plain);
	if (letter) {
		if (typed == plain || typed == plain - LETTER_CASE_STEP) {
			*keysym = keiui_win32_character_keysym(plain);
			*menu = held;
			return 1;
		}
	}

	/* Another character: the one typed; when Shift made it, Shift is taken up by it (Ctrl+Shift+= is Ctrl++). */
	*keysym = keiui_win32_character_keysym(typed);
	*menu = held;
	if (typed != plain)
		*menu = held & ~(unsigned)KL_MENU_SHIFT;

	/* Succeeded: the key's keysym and modifiers. */
	return 1;
}

/*
 * Tells whether a menu's item can be chosen now.
 */
int
keiui_win32_menu_usable(
	const struct keiui_win32_menu_item *items,
	unsigned count,
	const struct keiui_win32_action *actions,
	unsigned action_count,
	unsigned index)
{
	const struct keiui_win32_menu_item *item;
	unsigned state;
	unsigned steps;
	unsigned look;
	uint32_t parent;

	/* An item of the table. */
	if (items == NULL || index >= count)
		return 0;
	item = &items[index];

	/* A separator or a submenu is not chosen. */
	if (item->type == KL_MENU_ITEM_SEPARATOR || item->type == KL_MENU_ITEM_SUBMENU)
		return 0;

	/* Nor an item hidden or disabled. */
	state = keiui_win32_action_state(actions, action_count, item->action);
	if ((state & (KL_ACTION_HIDDEN | KL_ACTION_DISABLED)) != 0U)
		return 0;

	/*
	 * Nor one under a submenu hidden or disabled: the parents up to the
	 * root, at most as many steps as the table has items (a table whose
	 * parents make a loop ends there).
	 */
	parent = item->parent;
	for (steps = 0U; steps < count && parent != KL_MENU_ROOT; steps++) {
		/* The parent's item. */
		for (look = 0U; look < count; look++) {
			if (items[look].id == parent)
				break;
		}

		/* A parent the table does not have is the root's. */
		if (look == count)
			break;

		/* A submenu that cannot be opened keeps its items from being chosen. */
		state = keiui_win32_action_state(actions, action_count, items[look].action);
		if (items[look].action != 0U && (state & (KL_ACTION_HIDDEN | KL_ACTION_DISABLED)) != 0U)
			return 0;
		parent = items[look].parent;
	}

	/* Succeeded: the item can be chosen. */
	return 1;
}

/*
 * Finds the usable item whose shortcut a key is.
 */
unsigned
keiui_win32_shortcut_find(
	const struct keiui_win32_menu_item *items,
	unsigned count,
	const struct keiui_win32_action *actions,
	unsigned action_count,
	uint32_t keysym,
	unsigned menu)
{
	uint32_t wanted;
	unsigned index;
	int usable;

	/* No key. */
	if (keysym == 0U || items == NULL)
		return KEIUI_WIN32_NO_ITEM;

	/* Every item with a shortcut of the same modifiers, its capital letter taken as its small one. */
	for (index = 0U; index < count; index++) {
		/* An item without a shortcut, or with other modifiers, is passed. */
		if (items[index].keysym == 0U || items[index].modifiers != menu)
			continue;

		/* Its key in small letters, as the key's keysym is. */
		wanted = items[index].keysym;
		if (wanted >= 'A' && wanted <= 'Z')
			wanted = wanted + LETTER_CASE_STEP;

		/* An item of another key is passed. */
		if (wanted != keysym)
			continue;

		/* The item, when it can be chosen now. */
		usable = keiui_win32_menu_usable(items, count, actions, action_count, index);
		if (usable)
			return index;
	}

	/* No usable item has the shortcut. */
	return KEIUI_WIN32_NO_ITEM;
}

/*
 * Writes a shortcut as a menu shows it.
 */
void
keiui_win32_shortcut_text(
	unsigned menu,
	uint32_t keysym,
	char *text,
	size_t size)
{
	char name[SHORTCUT_NAME];

	/* Nowhere to write. */
	if (text == NULL || size == 0U)
		return;

	/* The text starts empty, and an item without a shortcut keeps it so. */
	text[0] = '\0';
	if (keysym == 0U)
		return;

	/* The modifiers in a fixed order: Ctrl, Alt, Shift, Super. */
	if ((menu & KL_MENU_CTRL) != 0U)
		text_append(text, size, "Ctrl+");
	if ((menu & KL_MENU_ALT) != 0U)
		text_append(text, size, "Alt+");
	if ((menu & KL_MENU_SHIFT) != 0U)
		text_append(text, size, "Shift+");
	if ((menu & KL_MENU_SUPER) != 0U)
		text_append(text, size, "Super+");

	/* Then the key's name. */
	shortcut_key_name(keysym, name, sizeof(name));
	text_append(text, size, name);
}

/*
 * Writes a menu item's text for a Windows menu.
 */
void
keiui_win32_menu_text(
	const char *label,
	const char *shortcut,
	char *text,
	size_t size)
{
	char character[5];
	size_t length;
	size_t index;

	/* Nowhere to write. */
	if (text == NULL || size == 0U)
		return;

	/* The label, a character at a time, each '&' doubled. */
	text[0] = '\0';
	index = 0U;
	while (label != NULL && label[index] != '\0') {
		/* The mnemonic's mark written twice is the character itself. */
		if (label[index] == '&')
			text_append(text, size, "&");

		/* The character's bytes: its lead byte and the continuation bytes after it (a NUL is none). */
		length = 1U;
		while (length < 4U && ((unsigned char)label[index + length] & 0xc0U) == 0x80U)
			length++;
		memcpy(character, label + index, length);
		character[length] = '\0';
		text_append(text, size, character);
		index += length;
	}

	/* The shortcut after a tab, which Windows aligns at the right. */
	if (shortcut != NULL && shortcut[0] != '\0') {
		text_append(text, size, "\t");
		text_append(text, size, shortcut);
	}
}

/*
 * Reads a 16-bit word as a signed number.
 */
static int32_t
signed_word(
	uint64_t word)
{
	/* A word with its top bit set is negative in two's complement. */
	if ((word & WORD_SIGN_BIT) != 0)
		return (int32_t)word - WORD_RANGE;

	/* A word without it is the number itself. */
	return (int32_t)word;
}

/* Tells whether a character is a small letter with a capital (ASCII or Latin-1): 1 or 0. */
static int
small_letter(
	uint32_t code_point)
{
	/* The ASCII letters. */
	if (code_point >= 'a' && code_point <= 'z')
		return 1;

	/* The Latin-1 letters from a-grave to thorn, but the division sign. */
	if (code_point >= 0xe0U && code_point <= 0xfeU && code_point != LATIN1_DIVISION)
		return 1;

	/* No letter, or one without a capital. */
	return 0;
}

/* Writes the name a keysym is shown by: a letter in capitals, a symbol itself, F1 to F12, or a key's name. */
static void
shortcut_key_name(
	uint32_t keysym,
	char *name,
	size_t size)
{
	size_t length;

	/* The function keys. */
	name[0] = '\0';
	if (keysym >= KEYSYM_F1 && keysym <= KEYSYM_F12) {
		(void)snprintf(name, size, "F%u", (unsigned)(keysym - KEYSYM_F1 + 1U));
		return;
	}

	/* A small ASCII letter in capitals. */
	if (keysym >= 'a' && keysym <= 'z') {
		(void)snprintf(name, size, "%c", (int)(keysym - LETTER_CASE_STEP));
		return;
	}

	/* A space by its name. */
	if (keysym == ' ') {
		(void)snprintf(name, size, "Space");
		return;
	}

	/* Any other printable ASCII symbol as itself. */
	if (keysym > ' ' && keysym < 0x7fU) {
		(void)snprintf(name, size, "%c", (int)keysym);
		return;
	}

	/* The named keys. */
	switch (keysym) {
	case KEYSYM_RETURN:
		(void)snprintf(name, size, "Enter");
		return;
	case KEYSYM_ESCAPE:
		(void)snprintf(name, size, "Esc");
		return;
	case KEYSYM_TAB:
		(void)snprintf(name, size, "Tab");
		return;
	case KEYSYM_BACKSPACE:
		(void)snprintf(name, size, "Backspace");
		return;
	case KEYSYM_DELETE:
		(void)snprintf(name, size, "Del");
		return;
	case KEYSYM_INSERT:
		(void)snprintf(name, size, "Ins");
		return;
	case KEYSYM_HOME:
		(void)snprintf(name, size, "Home");
		return;
	case KEYSYM_END:
		(void)snprintf(name, size, "End");
		return;
	case KEYSYM_PAGEUP:
		(void)snprintf(name, size, "PgUp");
		return;
	case KEYSYM_PAGEDOWN:
		(void)snprintf(name, size, "PgDn");
		return;
	case KEYSYM_LEFT:
		(void)snprintf(name, size, "Left");
		return;
	case KEYSYM_UP:
		(void)snprintf(name, size, "Up");
		return;
	case KEYSYM_RIGHT:
		(void)snprintf(name, size, "Right");
		return;
	case KEYSYM_DOWN:
		(void)snprintf(name, size, "Down");
		return;
	default:
		break;
	}

	/* A Latin-1 character (a small letter in capitals) or a Unicode keysym, as its character. */
	if (keysym >= 0xa0U && keysym <= 0xffU) {
		if (small_letter(keysym))
			keysym -= LETTER_CASE_STEP;
		length = utf8_encode(keysym, name);
		name[length] = '\0';
		return;
	}

	/* A Unicode keysym's character. */
	if (keysym > KEYSYM_UNICODE && keysym - KEYSYM_UNICODE <= UNICODE_LAST) {
		length = utf8_encode(keysym - KEYSYM_UNICODE, name);
		name[length] = '\0';
	}
}

/* Writes a code point's UTF-8 (at most 4 bytes, no NUL); its length, 0 for a surrogate or past Unicode. */
static size_t
utf8_encode(
	uint32_t code_point,
	char *bytes)
{
	/* One byte. */
	if (code_point < 0x80U) {
		bytes[0] = (char)code_point;
		return 1U;
	}

	/* Two bytes. */
	if (code_point < 0x800U) {
		bytes[0] = (char)(0xc0U | (code_point >> 6));
		bytes[1] = (char)(0x80U | (code_point & 0x3fU));
		return 2U;
	}

	/* No surrogate is a character. */
	if (code_point >= 0xd800U && code_point <= 0xdfffU)
		return 0U;

	/* Three bytes. */
	if (code_point < 0x10000U) {
		bytes[0] = (char)(0xe0U | (code_point >> 12));
		bytes[1] = (char)(0x80U | ((code_point >> 6) & 0x3fU));
		bytes[2] = (char)(0x80U | (code_point & 0x3fU));
		return 3U;
	}

	/* Nothing past Unicode. */
	if (code_point > UNICODE_LAST)
		return 0U;

	/* Four bytes. */
	bytes[0] = (char)(0xf0U | (code_point >> 18));
	bytes[1] = (char)(0x80U | ((code_point >> 12) & 0x3fU));
	bytes[2] = (char)(0x80U | ((code_point >> 6) & 0x3fU));
	bytes[3] = (char)(0x80U | (code_point & 0x3fU));
	return 4U;
}

/* Appends text after a text of a size, as many whole characters as fit. */
static void
text_append(
	char *text,
	size_t size,
	const char *more)
{
	size_t length;
	size_t adding;

	/* What is there, and what would be added. */
	length = strlen(text);
	adding = strlen(more);

	/* Back to a character's start until the addition fits with the NUL. */
	while (adding != 0U && length + adding + 1U > size) {
		adding--;
		while (adding != 0U && ((unsigned char)more[adding] & 0xc0U) == 0x80U)
			adding--;
	}

	/* The bytes that fit, and the end. */
	memcpy(text + length, more, adding);
	text[length + adding] = '\0';
}
