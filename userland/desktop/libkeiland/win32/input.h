/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The pointer and keyboard details of the Windows backend (input.c): what
 * the window procedure reads out of a message's wParam and lParam, turned
 * into the values of struct kl_window_event, and the rules of a window's
 * menu that need no Windows call (which item a key's shortcut chooses, and
 * the text an item shows).  Pure: no Windows header (the few constants are
 * given here with their <winuser.h> values), no state.
 */

#ifndef KEIUI_WIN32_INPUT_H
#define KEIUI_WIN32_INPUT_H

#include <stddef.h>
#include <stdint.h>

/* One wheel notch on Windows (WHEEL_DELTA). */
#define KEIUI_WIN32_WHEEL_DELTA		120

/*
 * The pixels one notch scrolls on Keiland: the compositor sends 15 units
 * a notch (wayland/seat.c WHEEL_STEP) and libkeiland's Wayland backend
 * scales units by 4 (ui/window.c WINDOW_SCROLL_SCALE), so 60.
 */
#define KEIUI_WIN32_NOTCH_PIXELS	60.0

/* The buttons of the mouse messages (which message, or XBUTTON1 and 2 of WM_XBUTTON*). */
#define KEIUI_WIN32_BUTTON_LEFT		0U	/* WM_LBUTTON* */
#define KEIUI_WIN32_BUTTON_RIGHT	1U	/* WM_RBUTTON* */
#define KEIUI_WIN32_BUTTON_MIDDLE	2U	/* WM_MBUTTON* */
#define KEIUI_WIN32_BUTTON_X1		3U	/* WM_XBUTTON* with XBUTTON1 */
#define KEIUI_WIN32_BUTTON_X2		4U	/* WM_XBUTTON* with XBUTTON2 */

/* The modifier keys held, as the caller reads them with GetKeyState (bit set: the key is down). */
#define KEIUI_WIN32_HELD_SHIFT		0x01U	/* VK_SHIFT */
#define KEIUI_WIN32_HELD_CONTROL	0x02U	/* VK_CONTROL */
#define KEIUI_WIN32_HELD_ALT		0x04U	/* VK_MENU */
#define KEIUI_WIN32_HELD_WIN		0x08U	/* VK_LWIN or VK_RWIN */
#define KEIUI_WIN32_HELD_ALTGR		0x10U	/* the right Alt of a layout with AltGr (VK_RMENU with VK_LCONTROL synthesized) */

/* The kinds of pointer of WM_POINTER* (POINTER_INPUT_TYPE values). */
#define KEIUI_WIN32_POINTER_GENERIC	1U	/* PT_POINTER */
#define KEIUI_WIN32_POINTER_TOUCH	2U	/* PT_TOUCH */
#define KEIUI_WIN32_POINTER_PEN		3U	/* PT_PEN */
#define KEIUI_WIN32_POINTER_MOUSE	4U	/* PT_MOUSE */
#define KEIUI_WIN32_POINTER_TOUCHPAD	5U	/* PT_TOUCHPAD */

/* What a key message's lParam says (keiui_win32_key_flags). */
struct keiui_win32_key_flags {
	unsigned scan;		/* bits 16 to 23 */
	int extended;		/* bit 24: 1 or 0 */
	int repeated;		/* bit 30 as it is (1 or 0): on a key down, the key was already down; Windows sets it on every key up */
	int released;		/* bit 31: 1 for a key up */
};

/* The longest label of a menu item kept by a window, with its NUL; a longer one is cut at a character. */
#define KEIUI_WIN32_MENU_LABEL		96U

/* No menu item (an index of none). */
#define KEIUI_WIN32_NO_ITEM		0xffffffffU

/*
 * One item of a window's menu (kl_window_set_menu, kl_window_popup_menu):
 * the fields of struct kl_menu_entry with its label copied, so that the
 * table lives as long as the window shows it.  The types, roles and
 * modifiers are those of <keiland/keiland_menu.h> (KL_MENU_ITEM_*,
 * KL_MENU_ROLE_*, KL_MENU_SHIFT and the others); the keysym is an XKB
 * keysym, 0 for no shortcut.
 */
struct keiui_win32_menu_item {
	uint32_t id;
	uint32_t parent;
	unsigned type;
	char label[KEIUI_WIN32_MENU_LABEL];
	uint32_t action;
	unsigned role;
	unsigned modifiers;
	uint32_t keysym;
};

/*
 * An action's state (KL_ACTION_* bits of <keiland/keiland_window.h>), which
 * every menu item and title bar control of that action shows.  An action
 * never told is enabled, unchecked and shown (state 0).
 */
struct keiui_win32_action {
	uint32_t action;
	unsigned state;
};

/*
 * Turns one WM_MOUSEWHEEL or WM_MOUSEHWHEEL delta into Keiland's distance
 * in pixels.
 *
 * delta:      the signed 16-bit high word of wParam (a notch is 120; a
 *             precision touch pad sends smaller values).
 * horizontal: 1 for WM_MOUSEHWHEEL, 0 for WM_MOUSEWHEEL.
 * Returns the distance for the event's dy (vertical) or dx (horizontal):
 * KEIUI_WIN32_NOTCH_PIXELS * delta / 120, negated for the vertical wheel
 * (Windows' positive vertical delta turns the wheel away from the user,
 * which scrolls up; Keiland's positive dy scrolls down) and not negated for
 * the horizontal one (both positive to the right).
 */
double keiui_win32_wheel_pixels(int delta, int horizontal);

/*
 * Turns the modifier keys held into KL_MOD_* (<keiland/keiland_key.h>).
 *
 * held: KEIUI_WIN32_HELD_* bits; unknown bits are ignored.
 * AltGr is not Ctrl and Alt to Keiland: when KEIUI_WIN32_HELD_ALTGR is set,
 * the CONTROL and ALT bits that Windows synthesizes with it are dropped (an
 * application then sees the character AltGr types, not a shortcut).
 * Returns the KL_MOD_SHIFT, KL_MOD_CTRL, KL_MOD_ALT and KL_MOD_SUPER bits.
 */
unsigned keiui_win32_modifiers(unsigned held);

/*
 * Gives the evdev code of a mouse button (KL_BUTTON_LEFT 0x110,
 * KL_BUTTON_RIGHT 0x111, KL_BUTTON_MIDDLE 0x112, BTN_SIDE 0x113 for X1,
 * BTN_EXTRA 0x114 for X2), or 0 for an unknown button.
 */
uint32_t keiui_win32_button_code(unsigned button);

/*
 * Reads a point from a mouse message's lParam: x is the signed low word, y
 * the signed high word (GET_X_LPARAM and GET_Y_LPARAM; negative on a
 * second monitor to the left or above).  lparam is the message's LPARAM
 * converted to int64_t.  NULL outputs are not written.
 */
void keiui_win32_point(int64_t lparam, int32_t *x, int32_t *y);

/*
 * Reads a key message's lParam.  flags NULL does nothing.
 */
void keiui_win32_key_flags(int64_t lparam, struct keiui_win32_key_flags *flags);

/*
 * Reports an action's state among a window's (KL_ACTION_* bits), 0 for an
 * action not among them.  actions may be NULL when count is 0.
 */
unsigned keiui_win32_action_state(const struct keiui_win32_action *actions, unsigned count, uint32_t action);

/*
 * Gives the XKB keysym of a key that types no character, by its evdev
 * code: F1 to F12 (0xffbe to 0xffc9), Enter and the keypad's Enter
 * (0xff0d), Escape (0xff1b), Tab (0xff09), Backspace (0xff08), Delete
 * (0xffff), Insert (0xff63), Home (0xff50), End (0xff57), Page Up (0xff55),
 * Page Down (0xff56), the arrows (Left 0xff51, Up 0xff52, Right 0xff53,
 * Down 0xff54), and the keypad's + and - as the characters '+' and '-'
 * (what the main keys' + and - are).  Returns 0 for any other key (a key
 * whose keysym is the character its layout types).
 */
uint32_t keiui_win32_named_keysym(uint32_t code);

/*
 * Gives the XKB keysym of a character: itself up to U+00FF (Latin-1), and
 * 0x01000000 plus the code point above it (XKB's Unicode keysyms).  0 is
 * no character and gives 0.
 */
uint32_t keiui_win32_character_keysym(uint32_t code_point);

/*
 * Works out what a key press is to a menu's shortcuts, as the Keiland
 * compositor does (a letter keeps Shift beside it; a key whose character
 * Shift changes is the changed character, with Shift taken up by it).
 *
 * named:     the key's keysym when it types no character
 *            (keiui_win32_named_keysym), else 0.
 * plain:     the character the key types in the layout without any
 *            modifier (0 for none).
 * typed:     the character it types with Shift as held but without
 *            Control and Alt (0 for none; plain when Shift is not held).
 * modifiers: the KL_MOD_* bits held (keiui_win32_modifiers).
 * keysym:    receives the keysym to look the shortcut up by: named when
 *            it is not 0; else a letter (a character whose small form is
 *            plain, ASCII or Latin-1) as its small form; else the typed
 *            character.
 * menu:      receives the KL_MENU_* modifiers: Shift, Control, Alt and
 *            Super as held, less Shift when typed is not plain and is not
 *            plain's capital (Shift made another character).
 * Returns 1 with both set, or 0 when the key has no keysym (no named
 * keysym and no character) or an output is NULL.
 */
int keiui_win32_shortcut_key(uint32_t named, uint32_t plain, uint32_t typed, unsigned modifiers, uint32_t *keysym, unsigned *menu);

/*
 * Tells whether a menu's item can be chosen now: it is not a separator or
 * a submenu, and neither it nor any submenu above it is hidden or disabled
 * by its action's state.  A parent that is not in the table ends the
 * search (the root).  Returns 1 or 0 (0 for an index past the table).
 */
int keiui_win32_menu_usable(const struct keiui_win32_menu_item *items, unsigned count, const struct keiui_win32_action *actions, unsigned action_count, unsigned index);

/*
 * Finds the usable item whose shortcut a key is (keysym and KL_MENU_*
 * modifiers from keiui_win32_shortcut_key).  An item's keysym that is a
 * capital ASCII letter is taken as its small one.  The first such item of
 * the table is the answer.  Returns its index, or KEIUI_WIN32_NO_ITEM.
 */
unsigned keiui_win32_shortcut_find(const struct keiui_win32_menu_item *items, unsigned count, const struct keiui_win32_action *actions, unsigned action_count, uint32_t keysym, unsigned menu);

/*
 * Writes a shortcut as a menu shows it ("Ctrl+Shift+N", "Ctrl++", "F11"):
 * the modifiers in the order Ctrl, Alt, Shift, Super, then the key: a
 * letter in capitals, F1 to F12, Space, a printable ASCII symbol as
 * itself, Enter, Esc, Tab, Backspace, Del, Ins, Home, End, PgUp, PgDn,
 * Left, Up, Right, Down, a Latin-1 or Unicode keysym as its character in
 * UTF-8.  keysym 0 writes the empty text.  text always ends with a NUL
 * (size above 0); a text that does not fit is cut at a character.
 */
void keiui_win32_shortcut_text(unsigned menu, uint32_t keysym, char *text, size_t size);

/*
 * Writes a menu item's text for a Windows menu (UTF-8): its label with
 * each '&' doubled (Windows would take a single one as the mnemonic's
 * mark), then, when shortcut is not empty, a tab and the shortcut (which
 * Windows aligns at the menu's right).  text always ends with a NUL (size
 * above 0); what does not fit is cut at a character.
 */
void keiui_win32_menu_text(const char *label, const char *shortcut, char *text, size_t size);

#endif
