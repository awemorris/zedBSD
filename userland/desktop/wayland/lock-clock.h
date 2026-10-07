/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The place and the size of the large clock of the lock screen and the
 * login screen (lock-clock.c, ws187-p001): the time large, the date under
 * it, above the middle of the output and clear of the card with the
 * password field, on a portrait output as on a landscape one.
 *
 * It knows nothing of the server or of the fonts: the caller hands it the
 * output's size and the card's top, and draws what it says (greeter.c).
 * So the host tests run it alone.  Every size is in logical pixels
 * (Guardrail "寸法の単位").
 */

#ifndef KWL_LOCK_CLOCK_H
#define KWL_LOCK_CLOCK_H

#include <stdint.h>

/*
 * Where the clock goes: the time's size in pixels (its em), the baselines
 * of the time and of the date, and the top and the bottom of the two lines
 * together (the time's digits' top, the date's descent).
 */
struct kwl_lock_clock {
	int32_t pixels;
	int32_t time_baseline;
	int32_t date_baseline;
	int32_t top;
	int32_t bottom;
};

void kwl_lock_clock_layout(int32_t width, int32_t height, int32_t card_top, struct kwl_lock_clock *clock);

#endif
