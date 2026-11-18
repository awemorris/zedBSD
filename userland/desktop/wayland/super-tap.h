/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Windows key pressed alone (super-tap.c, ws142-p002): tells when a
 * press and release of Super made a tap of its own, which opens or closes
 * App Home.
 *
 * It knows nothing of the seat: the caller hands it each key and each
 * pointer button, wheel turn or touch, and acts on the tap it reports
 * (seat.c).  So the host tests run it alone.
 */

#ifndef KWL_SUPER_TAP_H
#define KWL_SUPER_TAP_H

#include <stdint.h>

/* The evdev codes of the left and the right Super (Meta) keys. */
#define KWL_SUPER_TAP_LEFT	125U
#define KWL_SUPER_TAP_RIGHT	126U

/* The longest a Super may be held and still be a tap (milliseconds). */
#define KWL_SUPER_TAP_MS	1000U

/*
 * The state: armed while a Super went down alone and nothing else has
 * happened since, the key that armed it, and when it went down.  All zero
 * when nothing is armed; it lives in the compositor's server.
 */
struct kwl_super_tap {
	uint32_t armed;
	uint32_t key;
	uint64_t down_ms;
};

int kwl_super_tap_key(struct kwl_super_tap *tap, uint32_t key, uint32_t state, uint32_t others_held, uint64_t now_ms);
void kwl_super_tap_cancel(struct kwl_super_tap *tap);

#endif
