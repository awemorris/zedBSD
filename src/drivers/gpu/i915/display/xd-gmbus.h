/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GMBUS controller of the display 12 and 13 code (xd-gmbus.c): the
 * south display's I2C master, which reads the EDID of an HDMI monitor
 * over the DDC pins of a port.
 *
 * One controller serves every pin, so a transfer selects its pin, runs
 * its messages and lets the pin go again.  The caller holds the
 * controller's lock for the whole transfer and the power the controller
 * needs; the controller only moves the bytes and reports whether the
 * target acknowledged them.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_XD_GMBUS_H
#define DRIVERS_GPU_I915_DISPLAY_XD_GMBUS_H

#include <stdint.h>

#include "xd-io.h"

/* The pins a transfer may select: 1 to 3 for DDI A to C, 9 to 14 for TC1 to TC6. */
#define I915_XD_GMBUS_PINS		15U

/*
 * What became of a transfer (struct i915_xd_gmbus_result).
 *
 *   DONE        every message went through and the bus went idle
 *   NO_ACK      a target did not acknowledge (normal when nothing is
 *               connected); the bus went idle afterwards
 *   HUNG        a target did not acknowledge and the bus stayed active
 *   NOT_READY   the controller never reported the next bytes or the
 *               end of a message, and no target refused
 *   LINGERED    every message went through but the bus stayed active
 *               after the stop
 *   BAD_PIN     the pin is not one of the controller's
 */
#define I915_XD_GMBUS_DONE		0U
#define I915_XD_GMBUS_NO_ACK		1U
#define I915_XD_GMBUS_HUNG		2U
#define I915_XD_GMBUS_NOT_READY		3U
#define I915_XD_GMBUS_LINGERED		4U
#define I915_XD_GMBUS_BAD_PIN		5U

/*
 * What a transfer tells its caller while it runs, in the order it
 * happens (struct i915_xd_gmbus_request noted):
 *
 *   STAYED_ACTIVE   after a refusal the bus did not go idle
 *   REFUSED         a target refused a message (its index is given);
 *                   the controller has been cleared
 *   RETRY           the first message was refused, and the transfer
 *                   starts once more from it
 */
#define I915_XD_GMBUS_NOTE_STAYED_ACTIVE	1U
#define I915_XD_GMBUS_NOTE_REFUSED		2U
#define I915_XD_GMBUS_NOTE_RETRY		3U

/*
 * One message of a transfer: bytes read from, or written to, the
 * target at a 7-bit address.
 */
struct i915_xd_gmbus_message {
	unsigned address;
	int read;
	uint8_t *data;
	unsigned length;
};

/*
 * One transfer: the pin, the messages in order, whether the
 * controller's interrupts are to be raised while it waits (the
 * device's interrupts are on), and whether the pin is one of display
 * 9's south display (whose pins are numbered otherwise).
 *
 * noted, when not NULL, is told of the events of the transfer with
 * context, the event (I915_XD_GMBUS_NOTE_*) and the index of the
 * message concerned.
 */
struct i915_xd_gmbus_request {
	unsigned pin;
	struct i915_xd_gmbus_message *messages;
	unsigned count;
	int interrupts;
	int display9;
	void (*noted)(void *context, unsigned note, unsigned message);
	void *context;
};

/*
 * What a transfer ended with: the outcome (I915_XD_GMBUS_*), the index
 * of the message that failed, how many times the transfer was started
 * (2 when the first message was refused once), and the controller's
 * status word read last.
 */
struct i915_xd_gmbus_result {
	unsigned outcome;
	unsigned failed;
	unsigned starts;
	uint32_t status;
};

/*
 * The GMBUS controller of a display.
 *
 * The owner prepares it once and keeps it as long as the display; it
 * holds only counters, which grow under the controller's lock and say
 * how the controller has been behaving: transfers asked for, messages
 * refused, transfers started again, and waits that ran out.
 */
struct i915_xd_gmbus {
	unsigned transfers;
	unsigned refusals;
	unsigned restarts;
	unsigned timeouts;
};

void drv_i915_xd_gmbus_init(struct i915_xd_gmbus *gmbus);
void drv_i915_xd_gmbus_reset(struct i915_xd_io *io);
int drv_i915_xd_gmbus_pin_valid(unsigned pin);
int drv_i915_xd9_gmbus_edid_blocks(struct i915_xd_gmbus *gmbus, struct i915_xd_io *io, unsigned pin, uint8_t *edid, unsigned blocks);
unsigned drv_i915_xd9_gmbus_pin(unsigned ddi);
int drv_i915_xd9_gmbus_pin_valid(unsigned pin);
void drv_i915_xd_gmbus_transfer(struct i915_xd_gmbus *gmbus, struct i915_xd_io *io, const struct i915_xd_gmbus_request *request, struct i915_xd_gmbus_result *result);

#endif /* DRIVERS_GPU_I915_DISPLAY_XD_GMBUS_H */
