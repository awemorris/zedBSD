/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The AUX channels of the display 12 and 13 code (xd-aux.c): one
 * transaction on one channel, from loading the request into the data
 * words to unloading the sink's reply.
 *
 * A transaction is only the hardware exchange.  The caller has already
 * made the channel usable: the display is initialized, the channel's
 * power well is on, the display is held out of its deep power states,
 * and an eDP panel's VDD is on.  The caller also holds the channel's
 * lock for the whole transaction, so no two transactions of one device
 * overlap.  What a reply means (an acknowledgement, a refusal, a request
 * to retry later) is in its first byte, which the caller reads; the
 * channel only reports whether the bytes went out and came back.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_XD_AUX_H
#define DRIVERS_GPU_I915_DISPLAY_XD_AUX_H

#include <stdint.h>

#include "xd-io.h"

/*
 * The most bytes one transaction carries each way: the five data words
 * of the channel.
 */
#define I915_XD_AUX_MESSAGE_MAX		20U

/*
 * The channels the display has: A, B and C, then the six Type-C
 * channels (display 13 calls the fourth and fifth of them D and E).
 */
#define I915_XD_AUX_CHANNELS		9U

/*
 * What became of a transaction (struct i915_xd_aux_result).
 *
 *   SENT            the sink answered; the reply was unloaded
 *   BUSY            an earlier transaction still held the channel, so
 *                   this one was not started
 *   TOO_LONG        the request or the room for the reply is larger
 *                   than the data words
 *   UNFINISHED      the channel never reported the transaction done
 *   RECEIVE_ERROR   the reply arrived damaged on every try
 *   NO_ANSWER       the sink did not answer in time (normal when
 *                   nothing is connected)
 *   BAD_SIZE        the channel reported a reply of no bytes or of more
 *                   than the data words hold
 */
#define I915_XD_AUX_SENT		0U
#define I915_XD_AUX_BUSY		1U
#define I915_XD_AUX_TOO_LONG		2U
#define I915_XD_AUX_UNFINISHED		3U
#define I915_XD_AUX_RECEIVE_ERROR	4U
#define I915_XD_AUX_NO_ANSWER		5U
#define I915_XD_AUX_BAD_SIZE		6U

/*
 * One request to send over a channel and the room for its reply.
 *
 * thunderbolt is nonzero when the channel's Type-C port is in
 * Thunderbolt mode, which routes the channel through the Thunderbolt
 * IO.  hdcp_key is nonzero when the hardware is to send the
 * transmitter's HDCP key, which software cannot read, after the request.
 */
struct i915_xd_aux_message {
	const uint8_t *request;
	unsigned request_bytes;
	uint8_t *reply;
	unsigned reply_room;
	int thunderbolt;
	int hdcp_key;
};

/*
 * What a transaction ended with.
 *
 * status is the last control word read from the channel.  reply_bytes
 * is the number of bytes unloaded into the reply when the transaction
 * was SENT, and the size the channel reported when it was BAD_SIZE.
 */
struct i915_xd_aux_result {
	unsigned outcome;
	uint32_t status;
	unsigned reply_bytes;
};

/*
 * One AUX channel of a display.
 *
 * The owner prepares it once with drv_i915_xd_aux_init() and keeps it
 * for as long as the port exists.  stalled, when not NULL, is told each
 * time one try of a transaction did not finish within the wait for it,
 * with the control word last read; the owner uses it to report the
 * stall while the channel's state can still be read.  The counters only
 * grow, under the channel's lock, and say how the channel has been
 * behaving: transactions started, tries sent, tries that met no answer,
 * tries whose reply arrived damaged, and tries that stalled.
 */
struct i915_xd_aux {
	unsigned channel;
	void (*stalled)(void *context, uint32_t status);
	void *context;
	unsigned transactions;
	unsigned tries;
	unsigned no_answers;
	unsigned receive_errors;
	unsigned stalls;
};

int drv_i915_xd_aux_init(struct i915_xd_aux *aux, unsigned channel);
uint32_t drv_i915_xd_aux_control(const struct i915_xd_aux_message *message);
void drv_i915_xd_aux_exchange(struct i915_xd_aux *aux, struct i915_xd_io *io, const struct i915_xd_aux_message *message, struct i915_xd_aux_result *result);

/*
 * Reads or writes size bytes of a sink's DPCD at address over native AUX
 * messages of at most 16 bytes, retrying refused, deferred and damaged
 * messages as the specification allows; a read first wakes the sink
 * with a thrown-away read of its revision.  The caller holds the channel
 * usable as for a transaction.  Returns 0, EIO, ETIMEDOUT or EBUSY.
 */
int drv_i915_xd_dpcd_read(struct i915_xd_aux *aux, struct i915_xd_io *io, uint32_t address, uint8_t *buffer, unsigned size);
int drv_i915_xd_dpcd_write(struct i915_xd_aux *aux, struct i915_xd_io *io, uint32_t address, const uint8_t *buffer, unsigned size);

/*
 * One I2C message carried over a channel: the device's 7-bit address,
 * whether the message reads (nonzero) or writes, and its bytes.
 */
struct i915_xd_aux_i2c_message {
	unsigned address;
	int read;
	uint8_t *bytes;
	unsigned length;
};

/*
 * Runs I2C messages as one I2C transaction over a channel (an EDID's
 * offset write and its read, say), each message's bytes in AUX messages
 * of at most 16, retrying as the sink asks.  The caller holds the
 * channel usable as for a transaction.  Returns 0, EIO for a refusal or
 * a reply the specification does not allow, ETIMEDOUT when the sink never
 * answered, or EPROTO for an acknowledgement that moved no byte.
 */
int drv_i915_xd_aux_edid_blocks(struct i915_xd_aux *aux, struct i915_xd_io *io, uint8_t *edid, unsigned blocks);
int drv_i915_xd_aux_i2c(struct i915_xd_aux *aux, struct i915_xd_io *io, const struct i915_xd_aux_i2c_message *messages, unsigned count);

#endif /* DRIVERS_GPU_I915_DISPLAY_XD_AUX_H */
