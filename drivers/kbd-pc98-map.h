/*
 * NEC PC-98 keyboard scancode translation
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * The pure-logic half of the PC-98 keyboard driver: scancode plus
 * modifier state to a normalized key code, and the real-time press
 * bitmap that BeUI.isKeyDown reads.  The interrupt-driven 8251A front
 * end that feeds this lives with the rest of the driver once the HAL is
 * in place; this part is host-testable on its own.
 *
 * Normalized keys are the namespace the old BIOS gateway produced:
 * printable ASCII plus the NOCT_BEUI_KEY_* codes shared with BeUI, so a
 * script sees identical Key.* values whatever the input path.
 */

#ifndef BOOTS_DRIVERS_KBD_PC98_MAP_H
#define BOOTS_DRIVERS_KBD_PC98_MAP_H

#include <stdint.h>

struct boots_kbd_pc98 {
	uint8_t shift;
	uint8_t ctrl;
	uint8_t graph;
	uint8_t caps;
	uint8_t kana;
	/* One bit per scancode (0x00-0x7f), press state. */
	uint8_t down[16];
};

void boots_kbd_pc98_reset(struct boots_kbd_pc98 *kb);

/*
 * Feed one raw scancode byte (bit 7 set on release).  Updates modifier
 * and press state, and returns the normalized key for the make of a
 * translatable key, or 0 for releases, modifiers, and keys with no
 * normalized form.
 */
int boots_kbd_pc98_feed(struct boots_kbd_pc98 *kb, uint8_t raw);

/*
 * Real-time press state for a normalized key: 1 held, 0 up, -1 when the
 * key has no PC-98 scancode.  Mirrors the old gateway KEY_STATE service.
 */
int boots_kbd_pc98_is_down(const struct boots_kbd_pc98 *kb, int key);

#endif
