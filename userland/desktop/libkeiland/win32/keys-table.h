/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The keys of the Windows backend (keys-table.c): which key a keyboard
 * message names, as the evdev code every Keiland application already
 * understands (struct kl_window_event's code for KL_WINDOW_KEY).
 *
 * A Windows key message (WM_KEYDOWN, WM_KEYUP, WM_SYSKEYDOWN, WM_SYSKEYUP)
 * carries in its lParam the key's scan code of scan code set 1 (bits 16 to
 * 23) and whether the key sent the 0xE0 prefix (the extended bit, bit 24),
 * and in its wParam the virtual-key code.  The scan code names the key's
 * place on the keyboard whatever the layout, which is what the evdev code
 * names too; the virtual-key code depends on the layout and is used only
 * where Windows gives two keys the same scan code.
 *
 * The table maps each set 1 make code to the USB HID usage of the key
 * (Keyboard/Keypad page 0x07, and the Consumer page 0x0C for the media
 * keys) and that usage to its evdev code; keys-table.c names where the
 * values come from beside the table.  Pure: no Windows header, no system
 * call, no state; the tests run on the host.
 */

#ifndef KEIUI_WIN32_KEYS_TABLE_H
#define KEIUI_WIN32_KEYS_TABLE_H

#include <stdint.h>

/* The two virtual-key codes the mapping needs (<winuser.h> values). */
#define KEIUI_WIN32_VK_PAUSE		0x13U
#define KEIUI_WIN32_VK_NUMLOCK		0x90U

/*
 * Gives the evdev code of the key a key message names.
 *
 * scan:        the scan code, lParam bits 16 to 23 (0 to 255); 0 is "no scan
 *              code" (some synthesized messages) and gives 0.
 * extended:    lParam bit 24: 1 when the key sends the 0xE0 prefix, else 0
 *              (any other value is treated as 1).
 * virtual_key: wParam, the virtual-key code; read only for the two
 *              ambiguous keys below.
 * Returns the evdev code (1 to 0x2ff), or 0 (KEY_RESERVED) for a scan code
 * the table does not know; never fails otherwise.
 *
 * The cases Windows reports unlike scan code set 1, which the table must
 * follow:
 *  - Pause sends E1 1D 45 E1 9D C5 on the wire; Windows reports it as scan
 *    0x45 without the extended bit and virtual key VK_PAUSE, while Num Lock
 *    comes as scan 0x45 with the extended bit (VK_NUMLOCK).  So 0x45 with
 *    virtual key VK_PAUSE is KEY_PAUSE (119) whatever the extended bit, and
 *    0x45 otherwise is KEY_NUMLOCK (69).
 *  - Ctrl+Pause (Break) comes as scan 0x46 with the extended bit
 *    (VK_CANCEL): KEY_PAUSE.
 *  - Print Screen comes as 0x37 with the extended bit: KEY_SYSRQ (99);
 *    Alt+Print Screen comes as 0x54 without it: KEY_SYSRQ too.
 *  - The Japanese and Korean keys: 0x29 Hankaku/Zenkaku (KEY_GRAVE, as the
 *    HID table maps it; Windows gives the same scan for the grave key),
 *    0x70 Katakana/Hiragana (KEY_KATAKANAHIRAGANA), 0x73 Ro
 *    (KEY_RO), 0x79 Henkan (KEY_HENKAN), 0x7B Muhenkan (KEY_MUHENKAN), 0x7D
 *    Yen (KEY_YEN), each without the extended bit; 0x72 and 0xF2 Hangul
 *    (KEY_HANGEUL) and 0x71 and 0xF1 Hanja (KEY_HANJA) with or without it
 *    (keyboards differ in how they report them).
 *  - 0x76 without the extended bit is both F24 and LANG5 in the scan code
 *    table; it is KEY_F24 (LANG5 is rarely sent, and F24 is what the make
 *    code means on the keyboards that have it).
 * Every other entry is the set 1 make code of the Microsoft scan code
 * table (the extended ones are "0xE0 xx" there) mapped through its HID
 * usage.
 */
uint32_t keiui_win32_key_code(unsigned scan, int extended, unsigned virtual_key);

#endif
