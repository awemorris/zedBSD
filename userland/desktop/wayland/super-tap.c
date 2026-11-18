/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Windows key pressed alone (ws142-p002; the 2026-10-04 user request
 * "Windowsキーでアプリ一覧を出してほしい。", plan/ws142/phase001 C).
 *
 * A press of either Super with no Shift, Control or Alt held arms the tap.
 * Any other key pressed meanwhile (the other Super too), a pointer button,
 * a wheel turn or a touch disarms it (kwl_super_tap_cancel), so Super+Tab,
 * Super+L and the Super+Alt shortcuts stay what they are.  The release of
 * the key that armed it, within KWL_SUPER_TAP_MS of its press, is the tap.
 * A key's repeat (state 2) changes nothing.  The keys still go to the
 * client as before (plan/ws142/phase001 D9).
 */

#include "super-tap.h"

#include <stddef.h>

/*
 * Takes one key: state 1 pressed, 0 released, 2 repeated; others_held is
 * nonzero when Shift, Control or Alt is held.  Returns 1 when this release
 * completes a tap, 0 otherwise.
 */
int
kwl_super_tap_key(
	struct kwl_super_tap *tap,
	uint32_t key,
	uint32_t state,
	uint32_t others_held,
	uint64_t now_ms)
{
	uint32_t armed_key;
	uint64_t held_ms;
	int super;

	/* A repeat changes nothing. */
	if (state == 2U)
		return 0;

	/* Which key it is. */
	super = 0;
	if (key == KWL_SUPER_TAP_LEFT || key == KWL_SUPER_TAP_RIGHT)
		super = 1;

	/* A press: a Super alone arms the tap, anything else disarms it. */
	if (state != 0U) {
		if (super &&
		    !tap->armed &&
		    others_held == 0U) {
			tap->armed = 1U;
			tap->key = key;
			tap->down_ms = now_ms;
			return 0;
		}

		/* Another key, or the other Super, while armed. */
		kwl_super_tap_cancel(tap);
		return 0;
	}

	/* A release of a key that did not arm it changes nothing. */
	if (!tap->armed || key != tap->key)
		return 0;

	/* The release of the armed Super: a tap when it came soon enough. */
	armed_key = tap->key;
	held_ms = now_ms - tap->down_ms;
	kwl_super_tap_cancel(tap);
	if (armed_key != key || held_ms > KWL_SUPER_TAP_MS)
		return 0;

	/* Succeeded: a tap. */
	return 1;
}

/*
 * Disarms the tap: something else happened while Super was down.
 */
void
kwl_super_tap_cancel(
	struct kwl_super_tap *tap)
{
	/* Nothing armed. */
	tap->armed = 0U;
	tap->key = 0U;
	tap->down_ms = 0U;
}
