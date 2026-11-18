/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The AUX channels of the display 12 and 13 code (see xd-aux.h): one
 * transaction on one channel.
 */

#include "xd-aux.h"
#include "xd-io.h"

#include "../intel/display-dpcd-xd.h"
#include "../intel/display-regs-xd.h"

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

/* The bytes one data word of the channel holds. */
#define I915_XD_AUX_WORD_BYTES		4U

/*
 * The patience of a transaction: how many times the channel is looked
 * at, a millisecond apart, for an earlier transaction to end; how long
 * one try may take before it counts as stalled; how many tries one
 * request gets; and how long the channel rests after a damaged reply
 * before the next try.  A try the sink did not answer goes again at
 * once, because the hardware already waited the longest timeout for it.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dp_aux.c (intel_dp_aux_xfer: the
 * three busy reads 1 ms apart, the five tries, the 400 us after a
 * receive error; intel_dp_aux_wait_done: the 10 ms wait).
 */
#define I915_XD_AUX_IDLE_LOOKS		3U
#define I915_XD_AUX_IDLE_PAUSE_US	1000U
#define I915_XD_AUX_TRY_WAIT_US		10000U
#define I915_XD_AUX_TRIES		5U
#define I915_XD_AUX_DAMAGED_PAUSE_US	400U

/*
 * The sync pulses that open a transaction, counted in pulses: the
 * ordinary one is the longest precharge (16) and the preamble (16); the
 * one after a fast wake is the hardware's default precharge (10) and
 * the preamble (8).  The control word holds each count less one.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dp_aux.c (intel_dp_aux_sync_len,
 * intel_dp_aux_fw_sync_len).
 */
#define I915_XD_AUX_SYNC_PULSES		32U
#define I915_XD_AUX_FAST_WAKE_PULSES	18U

/*
 * The patience of a native DPCD access: the most messages one access
 * sends, and the rest between two messages when the last one did not
 * end in an unanswered try (the channel already waited for those); and
 * the most bytes one message carries.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/display/drm_dp_helper.c (drm_dp_dpcd_access: the 32
 * tries, AUX_RETRY_INTERVAL of 500 us); include/drm/display/drm_dp.h
 * (DP_AUX_MAX_PAYLOAD_BYTES).
 */
#define I915_XD_DPCD_TRIES		32U
#define I915_XD_DPCD_REST_US		500U
#define I915_XD_DPCD_CHUNK		16U

/*
 * The patience of an I2C message over the channel: the fewest tries one
 * AUX message gets and the most extra tries the sink's requests to wait
 * earn, the bit times the I2C bus and the AUX messages take (an I2C
 * start, a stop, an address byte and a data byte with their
 * acknowledgements; an AUX request's and reply's fixed parts at 1 MHz,
 * eight per data byte), and the I2C bus speed the tries are counted for.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/display/drm_dp_helper.c (drm_dp_i2c_do_msg: at least 7
 * tries and up to 7 more for I2C defers; AUX_PRECHARGE_LEN,
 * AUX_SYNC_LEN, AUX_STOP_LEN, AUX_CMD_LEN, AUX_ADDRESS_LEN,
 * AUX_REPLY_PAD_LEN, AUX_LENGTH_LEN, I2C_START_LEN, I2C_STOP_LEN,
 * I2C_ADDR_LEN, I2C_DATA_LEN; dp_aux_i2c_speed_khz of 10).
 */
#define I915_XD_I2C_LEAST_TRIES		7U
#define I915_XD_I2C_MOST_DEFERS		7U
#define I915_XD_I2C_START_BITS		1U
#define I915_XD_I2C_STOP_BITS		1U
#define I915_XD_I2C_ADDRESS_BITS	9U
#define I915_XD_I2C_DATA_BITS		9U
#define I915_XD_AUX_REQUEST_BITS	66U
#define I915_XD_AUX_REPLY_BITS		42U
#define I915_XD_I2C_SPEED_KHZ		10U

/*
 * The DDC address an EDID is read from, and the bytes of one EDID block.
 *
 * Values referenced: VESA Enhanced Display Data Channel Standard (the
 * EDID's DDC address 0x50, the 128-byte block).
 */
#define I915_XD_AUX_DDC_EDID		0x50U
#define I915_XD_AUX_EDID_BLOCK		128U

static int i915_xd_dpcd_message(struct i915_xd_aux *aux, struct i915_xd_io *io, unsigned request, uint32_t address, uint8_t *data, unsigned size);
static int i915_xd_aux_wait_idle(struct i915_xd_io *io, uint32_t control, uint32_t *status);
static uint32_t i915_xd_aux_send(struct i915_xd_aux *aux, struct i915_xd_io *io, const struct i915_xd_aux_message *message, uint32_t control);
static uint32_t i915_xd_aux_try(struct i915_xd_aux *aux, struct i915_xd_io *io, const struct i915_xd_aux_message *message, uint32_t control_word);
static void i915_xd_aux_load(struct i915_xd_io *io, unsigned channel, const uint8_t *request, unsigned count);
static void i915_xd_aux_unload(struct i915_xd_io *io, unsigned channel, uint8_t *reply, unsigned count);
static uint32_t i915_xd_aux_word(const uint8_t *bytes, unsigned count);
static void i915_xd_aux_bytes(uint32_t word, uint8_t *bytes, unsigned count);
static int i915_xd_aux_i2c_once(struct i915_xd_aux *aux, struct i915_xd_io *io, unsigned request, unsigned address, uint8_t *data, unsigned size, unsigned *reply, unsigned *moved);
static unsigned i915_xd_aux_i2c_tries(unsigned size);
static int i915_xd_aux_i2c_message(struct i915_xd_aux *aux, struct i915_xd_io *io, unsigned *request, unsigned address, uint8_t *data, unsigned size, unsigned *moved);
static int i915_xd_aux_i2c_drain(struct i915_xd_aux *aux, struct i915_xd_io *io, unsigned request, unsigned address, uint8_t *data, unsigned size, unsigned *next_size);

/*
 * Prepares one AUX channel of a display.
 *
 * The channel is numbered as the display numbers them: 0 to 2 for A to
 * C, 3 to 8 for the Type-C channels.  It returns EINVAL for a channel
 * the display does not have, and leaves the channel unprepared.
 */
int
drv_i915_xd_aux_init(
	struct i915_xd_aux *aux,
	unsigned channel)
{
	/* Refuses a channel past the last one. */
	if (channel >= I915_XD_AUX_CHANNELS)
		return EINVAL;

	/* Names the channel; nobody listens for stalls yet. */
	aux->channel = channel;
	aux->stalled = NULL;
	aux->context = NULL;

	/* Starts the behaviour counters from nothing. */
	aux->transactions = 0U;
	aux->tries = 0U;
	aux->no_answers = 0U;
	aux->receive_errors = 0U;
	aux->stalls = 0U;

	/* Succeeded: the channel is ready for transactions. */
	return 0;
}

/*
 * Builds the control word that starts a message's transaction.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dp_aux.c (skl_get_aux_send_ctl).
 *
 * Writing the word starts the transaction and clears the done flag and
 * both errors left by the previous one.  The hardware waits for the
 * sink as long as it can (the longest timeout, 4 ms on these displays,
 * longer than the 600 us the specification asks for).
 */
uint32_t
drv_i915_xd_aux_control(
	const struct i915_xd_aux_message *message)
{
	uint32_t control;

	/* Starts the transaction and clears what the previous one left. */
	control = I915_XD_AUX_CTL_BUSY |
		I915_XD_AUX_CTL_DONE |
		I915_XD_AUX_CTL_INTERRUPT |
		I915_XD_AUX_CTL_TIMEOUT_ERROR |
		I915_XD_AUX_CTL_RECEIVE_ERROR;

	/* Waits the longest for the sink. */
	control |= i915_xd_field_put(I915_XD_AUX_CTL_TIMEOUT, I915_XD_AUX_CTL_TIMEOUT_MAX);

	/* Says how many bytes of the data words go out. */
	control |= i915_xd_field_put(I915_XD_AUX_CTL_SIZE, message->request_bytes);

	/* Opens the transaction with the sync pulses, both counts less one. */
	control |= i915_xd_field_put(I915_XD_AUX_CTL_FW_SYNC_PULSE, I915_XD_AUX_FAST_WAKE_PULSES - 1U);
	control |= i915_xd_field_put(I915_XD_AUX_CTL_SYNC_PULSE, I915_XD_AUX_SYNC_PULSES - 1U);

	/* A Type-C port in Thunderbolt mode reaches its sink through the Thunderbolt IO. */
	if (message->thunderbolt != 0)
		control |= I915_XD_AUX_CTL_TBT_IO;

	/* The hardware appends the HDCP transmitter key when the request asks for it. */
	if (message->hdcp_key != 0)
		control |= I915_XD_AUX_CTL_HDCP_KEY;

	/* Succeeded: the control word. */
	return control;
}

/*
 * Runs one transaction on a channel: sends the request and unloads the
 * sink's reply.
 *
 * The channel must be free of an earlier transaction; one that is still
 * busy after three looks a millisecond apart is left alone and the
 * request is not sent.  The request then gets up to five tries.  A try
 * the sink did not answer, or one that stalled, goes again at once; a
 * try whose reply arrived damaged goes again after a short rest; the
 * first try the channel reports done and undamaged ends the
 * transaction.  The result says what the transaction ended with
 * (I915_XD_AUX_*), the last control word, and how many reply bytes were
 * unloaded.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dp_aux.c (intel_dp_aux_xfer,
 * intel_dp_aux_wait_done).
 */
void
drv_i915_xd_aux_exchange(
	struct i915_xd_aux *aux,
	struct i915_xd_io *io,
	const struct i915_xd_aux_message *message,
	struct i915_xd_aux_result *result)
{
	uint32_t control_offset;
	uint32_t control;
	uint32_t status;
	unsigned size;
	int idle;

	/* Starts from a result that says nothing was received. */
	result->outcome = I915_XD_AUX_SENT;
	result->status = 0U;
	result->reply_bytes = 0U;
	aux->transactions++;

	/* Leaves a channel an earlier transaction still holds. */
	control_offset = I915_XD_DDI_AUX_CTL(aux->channel);
	idle = i915_xd_aux_wait_idle(io, control_offset, &status);
	if (idle != 0) {
		result->outcome = I915_XD_AUX_BUSY;
		result->status = status;
		return;
	}

	/* Refuses a request or a reply room the data words cannot hold. */
	if (message->request_bytes > I915_XD_AUX_MESSAGE_MAX ||
	    message->reply_room > I915_XD_AUX_MESSAGE_MAX) {
		result->outcome = I915_XD_AUX_TOO_LONG;
		result->status = status;
		return;
	}

	/* Sends the request until the channel is done with it or the tries run out. */
	control = drv_i915_xd_aux_control(message);
	status = i915_xd_aux_send(aux, io, message, control);
	result->status = status;

	/* Reports a transaction the channel never finished. */
	if ((status & I915_XD_AUX_CTL_DONE) == 0U) {
		result->outcome = I915_XD_AUX_UNFINISHED;
		return;
	}

	/* Reports a reply that arrived damaged on the last try. */
	if ((status & I915_XD_AUX_CTL_RECEIVE_ERROR) != 0U) {
		result->outcome = I915_XD_AUX_RECEIVE_ERROR;
		return;
	}

	/* Reports a sink that did not answer the last try. */
	if ((status & I915_XD_AUX_CTL_TIMEOUT_ERROR) != 0U) {
		result->outcome = I915_XD_AUX_NO_ANSWER;
		return;
	}

	/*
	 * Refuses a reply of no bytes or of more than the data words hold,
	 * which the hardware must never report; nothing is unloaded.
	 */
	size = i915_xd_field_get(I915_XD_AUX_CTL_SIZE, status);
	if (size == 0U || size > I915_XD_AUX_MESSAGE_MAX) {
		result->outcome = I915_XD_AUX_BAD_SIZE;
		result->reply_bytes = size;
		return;
	}

	/* Unloads no more of the reply than the caller has room for. */
	if (size > message->reply_room)
		size = message->reply_room;

	/* Copies the reply out of the data words. */
	i915_xd_aux_unload(io, aux->channel, message->reply, size);

	/* Succeeded: the reply is in the caller's room. */
	result->reply_bytes = size;
}

/*
 * Reads size bytes of the sink's DPCD from address with native AUX
 * reads of at most 16 bytes each.  A one-byte read of the DPCD revision
 * goes first and its byte is thrown away: some sinks corrupt the first
 * access after they woke from power saving.  The caller holds the
 * channel usable (see xd-aux.h).  Returns 0, or the error of the first
 * message that did not succeed (EIO for a refusal or a damaged reply,
 * ETIMEDOUT when the sink never answered, EBUSY for a busy channel).
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/display/drm_dp_helper.c (drm_dp_dpcd_read,
 * drm_dp_dpcd_probe).
 */
int
drv_i915_xd_dpcd_read(
	struct i915_xd_aux *aux,
	struct i915_xd_io *io,
	uint32_t address,
	uint8_t *buffer,
	unsigned size)
{
	uint8_t thrown;
	unsigned done;
	unsigned chunk;
	int error;

	/* The throw-away read of the revision wakes the sink. */
	error = i915_xd_dpcd_message(aux, io, I915_XD_AUX_NATIVE_READ, I915_XD_DPCD_REV, &thrown, 1U);
	if (error != 0)
		return error;

	/* Reads the range a message at a time. */
	for (done = 0U; done < size; done += chunk) {
		chunk = size - done;
		if (chunk > I915_XD_DPCD_CHUNK)
			chunk = I915_XD_DPCD_CHUNK;

		/* Reads the chunk. */
		error = i915_xd_dpcd_message(aux, io, I915_XD_AUX_NATIVE_READ, address + done, buffer + done, chunk);
		if (error != 0)
			return error;
	}

	/* Succeeded: the bytes are in the buffer. */
	return 0;
}

/*
 * Writes size bytes to the sink's DPCD at address with native AUX writes
 * of at most 16 bytes each.  Returns 0 or an error as
 * drv_i915_xd_dpcd_read().
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/display/drm_dp_helper.c (drm_dp_dpcd_write).
 */
int
drv_i915_xd_dpcd_write(
	struct i915_xd_aux *aux,
	struct i915_xd_io *io,
	uint32_t address,
	const uint8_t *buffer,
	unsigned size)
{
	uint8_t data[I915_XD_DPCD_CHUNK];
	unsigned done;
	unsigned chunk;
	unsigned index;
	int error;

	/* Writes the range a message at a time, from a copy the message may carry. */
	for (done = 0U; done < size; done += chunk) {
		chunk = size - done;
		if (chunk > I915_XD_DPCD_CHUNK)
			chunk = I915_XD_DPCD_CHUNK;

		/* Copies the chunk to send. */
		for (index = 0U; index < chunk; index++)
			data[index] = buffer[done + index];

		/* Writes the chunk. */
		error = i915_xd_dpcd_message(aux, io, I915_XD_AUX_NATIVE_WRITE, address + done, data, chunk);
		if (error != 0)
			return error;
	}

	/* Succeeded: the sink took the bytes. */
	return 0;
}

/*
 * Runs I2C messages as one transaction over the channel.  Each message
 * starts with its address alone, then moves its bytes in AUX messages as
 * large as the channel carries (smaller after the sink took fewer); every
 * AUX message keeps the I2C transaction open, and an address alone
 * without that bit closes it at the end, whatever became of the
 * messages.  Returns 0 or the first message's error (see xd-aux.h).
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/display/drm_dp_helper.c (drm_dp_i2c_xfer,
 * drm_dp_i2c_msg_set_request).
 */
int
drv_i915_xd_aux_i2c(
	struct i915_xd_aux *aux,
	struct i915_xd_io *io,
	const struct i915_xd_aux_i2c_message *messages,
	unsigned count)
{
	unsigned request;
	unsigned address;
	unsigned index;
	unsigned done;
	unsigned chunk;
	unsigned size;
	unsigned moved;
	int error;

	/* Moves every message in turn. */
	error = 0;
	request = I915_XD_AUX_I2C_WRITE | I915_XD_AUX_I2C_MOT;
	address = 0U;
	for (index = 0U; index < count; index++) {
		/* The message's request, which keeps the transaction open. */
		address = messages[index].address;
		request = I915_XD_AUX_I2C_WRITE | I915_XD_AUX_I2C_MOT;
		if (messages[index].read)
			request = I915_XD_AUX_I2C_READ | I915_XD_AUX_I2C_MOT;

		/* The address alone starts the message. */
		error = i915_xd_aux_i2c_message(aux, io, &request, address, NULL, 0U, &moved);
		if (error != 0)
			break;

		/* The bytes, in chunks as large as the sink takes. */
		request = I915_XD_AUX_I2C_WRITE | I915_XD_AUX_I2C_MOT;
		if (messages[index].read)
			request = I915_XD_AUX_I2C_READ | I915_XD_AUX_I2C_MOT;
		size = I915_XD_DPCD_CHUNK;
		for (done = 0U; done < messages[index].length; done += chunk) {
			chunk = messages[index].length - done;
			if (chunk > size)
				chunk = size;

			/* Moves the chunk over the channel. */
			error = i915_xd_aux_i2c_drain(aux, io, request, address, messages[index].bytes + done, chunk, &size);
			if (error != 0)
				break;
		}

		/* A message that failed ends the transfer. */
		if (error != 0)
			break;
	}

	/* The address alone without the open bit ends the transaction; its outcome does not count. */
	request &= ~I915_XD_AUX_I2C_MOT;
	(void)i915_xd_aux_i2c_message(aux, io, &request, address, NULL, 0U, &moved);

	/* Reports the first message that failed. */
	if (error != 0)
		return error;

	/* Succeeded: every message moved its bytes. */
	return 0;
}

/*
 * Reads whole EDID blocks of the first segment over the AUX channel's I2C:
 * for each block its offset is written to the DDC address and its 128
 * bytes read back, all in one I2C transaction.  At most the two blocks of
 * the first segment.  Returns 0, or the error of the transaction.
 */
int
drv_i915_xd_aux_edid_blocks(
	struct i915_xd_aux *aux,
	struct i915_xd_io *io,
	uint8_t *edid,
	unsigned blocks)
{
	struct i915_xd_aux_i2c_message messages[4];
	uint8_t offsets[2];
	unsigned block;
	int error;

	/* The first segment holds two blocks. */
	if (blocks > 2U)
		blocks = 2U;
	if (blocks == 0U)
		return 0;

	/* Each block: its offset, then its bytes. */
	for (block = 0U; block < blocks; block++) {
		offsets[block] = (uint8_t)(block * I915_XD_AUX_EDID_BLOCK);
		messages[2U * block].address = I915_XD_AUX_DDC_EDID;
		messages[2U * block].read = 0;
		messages[2U * block].bytes = &offsets[block];
		messages[2U * block].length = 1U;
		messages[2U * block + 1U].address = I915_XD_AUX_DDC_EDID;
		messages[2U * block + 1U].read = 1;
		messages[2U * block + 1U].bytes = edid + block * I915_XD_AUX_EDID_BLOCK;
		messages[2U * block + 1U].length = I915_XD_AUX_EDID_BLOCK;
	}

	/* The transaction. */
	error = drv_i915_xd_aux_i2c(aux, io, messages, 2U * blocks);
	if (error != 0)
		return error;

	/* Succeeded: the blocks are read. */
	return 0;
}

/*
 * Sends one native DPCD message until the sink acknowledges it or the
 * tries run out: the header (request and address, then the byte count
 * less one), the data of a write, and for a read the reply's data copied
 * out.  A refused message, a deferred one, a damaged reply and a busy
 * channel are sent again after a rest; one the sink did not answer is
 * sent again at once.  Returns 0 or the error of the first failed try.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/display/drm_dp_helper.c (drm_dp_dpcd_access);
 * drivers/gpu/drm/i915/display/intel_dp_aux.c (intel_dp_aux_transfer,
 * intel_dp_aux_header).
 */
static int
i915_xd_dpcd_message(
	struct i915_xd_aux *aux,
	struct i915_xd_io *io,
	unsigned request,
	uint32_t address,
	uint8_t *data,
	unsigned size)
{
	struct i915_xd_aux_message message;
	struct i915_xd_aux_result result;
	uint8_t send[4U + I915_XD_DPCD_CHUNK];
	uint8_t reply[1U + I915_XD_DPCD_CHUNK];
	unsigned reply_code;
	unsigned index;
	unsigned try;
	int first;
	int error;

	/* The header, then a write's data. */
	send[0] = (uint8_t)((request << 4) | ((address >> 16) & 0xfU));
	send[1] = (uint8_t)((address >> 8) & 0xffU);
	send[2] = (uint8_t)(address & 0xffU);
	send[3] = (uint8_t)(size - 1U);
	message.request = send;
	message.request_bytes = 4U;
	message.reply = reply;
	message.reply_room = 1U;
	if (request == I915_XD_AUX_NATIVE_WRITE) {
		for (index = 0U; index < size; index++)
			send[4U + index] = data[index];
		message.request_bytes = 4U + size;
		message.reply_room = 2U;
	} else {
		message.reply_room = 1U + size;
	}

	/* A native transaction over the channel's own wires, without HDCP keys. */
	message.thunderbolt = 0;
	message.hdcp_key = 0;

	/* Sends the message until the sink takes it. */
	first = 0;
	error = 0;
	for (try = 0U; try < I915_XD_DPCD_TRIES; try++) {
		/* Rests after a try that the sink answered, or that never left. */
		if (try != 0U && error != ETIMEDOUT)
			(void)drv_i915_xd_io_delay(io, I915_XD_DPCD_REST_US);

		/* Sends the message and acts on how it went. */
		drv_i915_xd_aux_exchange(aux, io, &message, &result);
		switch (result.outcome) {
		case I915_XD_AUX_SENT:
			error = EIO;
			reply_code = ((unsigned)reply[0] >> 4) & I915_XD_AUX_NATIVE_REPLY_MASK;
			if (reply_code == I915_XD_AUX_NATIVE_ACK) {
				/* A read must bring back every byte asked for. */
				if (request == I915_XD_AUX_NATIVE_READ && result.reply_bytes != 1U + size)
					break;

				/* Copies the data a read brought back. */
				if (request == I915_XD_AUX_NATIVE_READ) {
					for (index = 0U; index < size; index++)
						data[index] = reply[1U + index];
				}

				/* Succeeded: the sink took the message. */
				return 0;
			}

			break;
		case I915_XD_AUX_NO_ANSWER:
			error = ETIMEDOUT;
			break;
		case I915_XD_AUX_BUSY:
		case I915_XD_AUX_UNFINISHED:
		case I915_XD_AUX_BAD_SIZE:
			error = EBUSY;
			break;
		default:
			error = EIO;
			break;
		}

		/* The error returned is the first one seen. */
		if (first == 0)
			first = error;
	}

	/* Reports the first error of a message the sink never took. */
	return first;
}

/*
 * Waits for an earlier transaction to leave the channel.
 *
 * The control word is read three times, a millisecond apart.  It returns
 * 0 with the idle word, or EBUSY with the word read once more after the
 * last look.
 */
static int
i915_xd_aux_wait_idle(
	struct i915_xd_io *io,
	uint32_t control,
	uint32_t *status)
{
	unsigned look;

	/* Looks at the busy flag until it is clear or the looks run out. */
	for (look = 0U; look < I915_XD_AUX_IDLE_LOOKS; look++) {
		*status = drv_i915_xd_io_read(io, control);
		if ((*status & I915_XD_AUX_CTL_BUSY) == 0U)
			return 0;

		/* Waits a little before reading again. */
		(void)drv_i915_xd_io_delay(io, I915_XD_AUX_IDLE_PAUSE_US);
	}

	/* Reports the channel still busy, with the word as it reads now. */
	*status = drv_i915_xd_io_read(io, control);
	return EBUSY;
}

/*
 * Sends a request up to five times, until a try ends done and
 * undamaged, and reports the control word of the last try.
 */
static uint32_t
i915_xd_aux_send(
	struct i915_xd_aux *aux,
	struct i915_xd_io *io,
	const struct i915_xd_aux_message *message,
	uint32_t control)
{
	uint32_t status;
	unsigned attempt;

	/* Tries the request until the channel is done with it. */
	status = 0U;
	for (attempt = 0U; attempt < I915_XD_AUX_TRIES; attempt++) {
		status = i915_xd_aux_try(aux, io, message, control);

		/* A try the sink did not answer goes again at once. */
		if ((status & I915_XD_AUX_CTL_TIMEOUT_ERROR) != 0U) {
			aux->no_answers++;
			continue;
		}

		/* A damaged reply rests the channel before the next try. */
		if ((status & I915_XD_AUX_CTL_RECEIVE_ERROR) != 0U) {
			aux->receive_errors++;
			(void)drv_i915_xd_io_delay(io, I915_XD_AUX_DAMAGED_PAUSE_US);
			continue;
		}

		/* A try the channel finished ends the transaction. */
		if ((status & I915_XD_AUX_CTL_DONE) != 0U)
			return status;
	}

	/* Succeeded: the control word of the last try, which the caller judges. */
	return status;
}

/*
 * Runs one try of a request: loads the data words, starts the channel,
 * waits for it to finish, and acknowledges the done flag and the errors
 * by writing them back.  It reports the control word the wait ended
 * with.
 */
static uint32_t
i915_xd_aux_try(
	struct i915_xd_aux *aux,
	struct i915_xd_io *io,
	const struct i915_xd_aux_message *message,
	uint32_t control_word)
{
	uint32_t control;
	uint32_t status;
	int waited;

	/* Loads the request and starts the channel. */
	control = I915_XD_DDI_AUX_CTL(aux->channel);
	aux->tries++;
	i915_xd_aux_load(io, aux->channel, message->request, message->request_bytes);
	drv_i915_xd_io_write(io, control, control_word);

	/* Waits for the channel to drop its busy flag; a stall is told to the owner. */
	status = 0U;
	waited = drv_i915_xd_io_poll(io, control, I915_XD_AUX_CTL_BUSY, 0U, I915_XD_AUX_TRY_WAIT_US, &status);
	if (waited == ETIMEDOUT) {
		aux->stalls++;

		/* The owner reports the stall while the channel can still be read. */
		if (aux->stalled != NULL)
			aux->stalled(aux->context, status);
	}

	/* Acknowledges the done flag and both errors, keeping the rest of the word read. */
	drv_i915_xd_io_write(io,
			     control,
			     status | I915_XD_AUX_CTL_DONE | I915_XD_AUX_CTL_TIMEOUT_ERROR | I915_XD_AUX_CTL_RECEIVE_ERROR);

	/* Succeeded: the word the try ended with. */
	return status;
}

/* Loads a request into the channel's data words, four bytes a word. */
static void
i915_xd_aux_load(
	struct i915_xd_io *io,
	unsigned channel,
	const uint8_t *request,
	unsigned count)
{
	uint32_t word;
	unsigned offset;

	/* Fills one data word for every four bytes, the last one partly. */
	for (offset = 0U; offset < count; offset += I915_XD_AUX_WORD_BYTES) {
		word = i915_xd_aux_word(request + offset, count - offset);
		drv_i915_xd_io_write(io, I915_XD_DDI_AUX_DATA(channel, offset / I915_XD_AUX_WORD_BYTES), word);
	}
}

/* Unloads a reply from the channel's data words, four bytes a word. */
static void
i915_xd_aux_unload(
	struct i915_xd_io *io,
	unsigned channel,
	uint8_t *reply,
	unsigned count)
{
	uint32_t word;
	unsigned offset;

	/* Reads one data word for every four bytes, the last one partly. */
	for (offset = 0U; offset < count; offset += I915_XD_AUX_WORD_BYTES) {
		word = drv_i915_xd_io_read(io, I915_XD_DDI_AUX_DATA(channel, offset / I915_XD_AUX_WORD_BYTES));
		i915_xd_aux_bytes(word, reply + offset, count - offset);
	}
}

/*
 * Makes a data word of up to four bytes: the channel sends the word's
 * highest byte first, so the first byte goes to the top.
 */
static uint32_t
i915_xd_aux_word(
	const uint8_t *bytes,
	unsigned count)
{
	uint32_t word;
	unsigned index;

	/* A word holds four bytes; the rest belong to the next word. */
	if (count > I915_XD_AUX_WORD_BYTES)
		count = I915_XD_AUX_WORD_BYTES;

	/* Places the bytes from the top of the word down; missing bytes are 0. */
	word = 0U;
	for (index = 0U; index < count; index++)
		word |= (uint32_t)bytes[index] << (8U * (I915_XD_AUX_WORD_BYTES - 1U - index));

	/* Succeeded: the word to load. */
	return word;
}

/* Takes up to four bytes out of a data word, the first from its top. */
static void
i915_xd_aux_bytes(
	uint32_t word,
	uint8_t *bytes,
	unsigned count)
{
	unsigned index;

	/* A word holds four bytes; the rest come from the next word. */
	if (count > I915_XD_AUX_WORD_BYTES)
		count = I915_XD_AUX_WORD_BYTES;

	/* Takes the bytes from the top of the word down. */
	for (index = 0U; index < count; index++)
		bytes[index] = (uint8_t)(word >> (8U * (I915_XD_AUX_WORD_BYTES - 1U - index)));
}

/*
 * Sends one I2C AUX message once: the header (request and address, then
 * the byte count less one, or the address alone for no bytes), a
 * write's data, and for a read the reply's data copied out.  Returns 0
 * with the reply's code (the top four bits of its first byte) and the
 * data bytes moved, or the channel's error: EBUSY for a busy or stuck
 * channel or a forbidden reply size (sent again at once), ETIMEDOUT when
 * the sink did not answer, EIO for a damaged reply, E2BIG for a message
 * the data words cannot hold.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dp_aux.c (intel_dp_aux_transfer,
 * intel_dp_aux_header).
 */
static int
i915_xd_aux_i2c_once(
	struct i915_xd_aux *aux,
	struct i915_xd_io *io,
	unsigned request,
	unsigned address,
	uint8_t *data,
	unsigned size,
	unsigned *reply,
	unsigned *moved)
{
	struct i915_xd_aux_message message;
	struct i915_xd_aux_result result;
	uint8_t send[4U + I915_XD_DPCD_CHUNK];
	uint8_t answer[1U + I915_XD_DPCD_CHUNK];
	unsigned index;
	unsigned base;
	int reading;

	/* Nothing came back or moved yet. */
	*reply = 0U;
	*moved = 0U;

	/* The header: the request and the address, then the count less one when bytes move. */
	send[0] = (uint8_t)((request << 4) | ((address >> 16) & 0xfU));
	send[1] = (uint8_t)((address >> 8) & 0xffU);
	send[2] = (uint8_t)(address & 0xffU);
	message.request = send;
	message.request_bytes = 3U;
	if (size != 0U) {
		send[3] = (uint8_t)(size - 1U);
		message.request_bytes = 4U;
	}

	/* A read's reply carries its bytes; a write carries them and is answered with at most one. */
	reading = 0;
	if ((request & I915_XD_AUX_I2C_READ) != 0U)
		reading = 1;
	message.reply = answer;
	message.reply_room = 2U;
	if (reading) {
		message.reply_room = 1U + size;
	} else {
		for (index = 0U; index < size; index++)
			send[4U + index] = data[index];
		message.request_bytes += size;
	}

	/* An I2C transaction over the channel's own wires, without HDCP keys. */
	message.thunderbolt = 0;
	message.hdcp_key = 0;

	/* The transaction, and what the channel made of it. */
	drv_i915_xd_aux_exchange(aux, io, &message, &result);
	switch (result.outcome) {
	case I915_XD_AUX_SENT:
		break;
	case I915_XD_AUX_NO_ANSWER:
		return ETIMEDOUT;
	case I915_XD_AUX_RECEIVE_ERROR:
		return EIO;
	case I915_XD_AUX_TOO_LONG:
		return E2BIG;
	default:
		return EBUSY;
	}

	/* The reply's code. */
	*reply = ((unsigned)answer[0] >> 4) & 0xfU;

	/* A read moved the bytes after the code. */
	if (reading) {
		base = result.reply_bytes - 1U;
		for (index = 0U; index < base; index++)
			data[index] = answer[1U + index];
		*moved = base;

		/* Succeeded: the read's bytes. */
		return 0;
	}

	/* A write's second byte, when there is one, says how many bytes the sink took. */
	*moved = size;
	if (result.reply_bytes > 1U && answer[1] < size)
		*moved = answer[1];

	/* Succeeded: the sink answered. */
	return 0;
}

/*
 * Counts the tries an I2C AUX message deserves: enough AUX round trips,
 * with their rests, to cover the message on the slow I2C bus, and never
 * fewer than seven.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/display/drm_dp_helper.c (drm_dp_i2c_retry_count,
 * drm_dp_aux_req_duration, drm_dp_aux_reply_duration,
 * drm_dp_i2c_msg_duration).
 */
static unsigned
i915_xd_aux_i2c_tries(
	unsigned size)
{
	unsigned aux_us;
	unsigned i2c_us;
	unsigned bits;
	unsigned tries;

	/* The AUX request and reply, a read's bytes in the reply and a write's in the request. */
	aux_us = I915_XD_AUX_REQUEST_BITS + I915_XD_AUX_REPLY_BITS + size * 8U;

	/* The I2C bus's bits at its speed, rounded up. */
	bits = I915_XD_I2C_START_BITS + I915_XD_I2C_ADDRESS_BITS + size * I915_XD_I2C_DATA_BITS + I915_XD_I2C_STOP_BITS;
	i2c_us = (bits * 1000U + I915_XD_I2C_SPEED_KHZ - 1U) / I915_XD_I2C_SPEED_KHZ;

	/* The round trips with their rests that cover the bus, rounded up. */
	tries = (i2c_us + aux_us + I915_XD_DPCD_REST_US - 1U) / (aux_us + I915_XD_DPCD_REST_US);
	if (tries < I915_XD_I2C_LEAST_TRIES)
		tries = I915_XD_I2C_LEAST_TRIES;

	/* Succeeded: the tries. */
	return tries;
}

/*
 * Sends one I2C AUX message until both the channel's sink and the I2C
 * device acknowledge it.  A busy channel is tried again at once; the
 * sink's or the device's request to wait rests and tries again (each of
 * the device's, up to seven, earns one more try); a short acknowledged
 * write, or one the device asked to wait, is asked for its status from
 * then on (the request becomes a status request, which the caller sees).
 * Returns 0 with the bytes moved, or an error (see xd-aux.h).
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/display/drm_dp_helper.c (drm_dp_i2c_do_msg,
 * drm_dp_i2c_msg_write_status_update).
 */
static int
i915_xd_aux_i2c_message(
	struct i915_xd_aux *aux,
	struct i915_xd_io *io,
	unsigned *request,
	unsigned address,
	uint8_t *data,
	unsigned size,
	unsigned *moved)
{
	unsigned reply;
	unsigned tries;
	unsigned defers;
	unsigned try;
	int error;

	/* The tries the message deserves; the device's waits add to them. */
	tries = i915_xd_aux_i2c_tries(size);
	defers = 0U;
	*moved = 0U;
	for (try = 0U; try < tries + defers; try++) {
		error = i915_xd_aux_i2c_once(aux, io, *request, address, data, size, &reply, moved);

		/* A busy channel is simply tried again. */
		if (error == EBUSY)
			continue;

		/* Any other failure of the channel ends the message. */
		if (error != 0)
			return error;

		/* The channel's sink refused, or answered with a code it may not use. */
		if ((reply & I915_XD_AUX_NATIVE_REPLY_MASK) == I915_XD_AUX_NATIVE_NACK)
			return EIO;
		if ((reply & I915_XD_AUX_NATIVE_REPLY_MASK) != I915_XD_AUX_NATIVE_ACK &&
		    (reply & I915_XD_AUX_NATIVE_REPLY_MASK) != I915_XD_AUX_NATIVE_DEFER)
			return EIO;

		/* The channel's sink asks to be asked later. */
		if ((reply & I915_XD_AUX_NATIVE_REPLY_MASK) == I915_XD_AUX_NATIVE_DEFER) {
			(void)drv_i915_xd_io_delay(io, I915_XD_DPCD_REST_US);
			continue;
		}

		/* The I2C device took the message: a short write asks its status from now on. */
		if ((reply & I915_XD_AUX_I2C_REPLY_MASK) == I915_XD_AUX_I2C_ACK) {
			if (*moved != size && (*request & ~I915_XD_AUX_I2C_MOT) == I915_XD_AUX_I2C_WRITE)
				*request = (*request & I915_XD_AUX_I2C_MOT) | I915_XD_AUX_I2C_WRITE_STATUS;
			return 0;
		}

		/* The I2C device refused. */
		if ((reply & I915_XD_AUX_I2C_REPLY_MASK) == I915_XD_AUX_I2C_NACK)
			return EIO;

		/* A reply code the specification does not have. */
		if ((reply & I915_XD_AUX_I2C_REPLY_MASK) != I915_XD_AUX_I2C_DEFER)
			return EIO;

		/* The I2C device asks to wait: one more try, a rest, and a write asks its status. */
		if (defers < I915_XD_I2C_MOST_DEFERS)
			defers++;
		(void)drv_i915_xd_io_delay(io, I915_XD_DPCD_REST_US);
		if ((*request & ~I915_XD_AUX_I2C_MOT) == I915_XD_AUX_I2C_WRITE)
			*request = (*request & I915_XD_AUX_I2C_MOT) | I915_XD_AUX_I2C_WRITE_STATUS;
	}

	/* The tries ran out. */
	return EIO;
}

/*
 * Moves one chunk of an I2C message until all of it is through, the sink
 * taking or giving fewer bytes at a time if it must.  Returns 0 with the
 * chunk size the next chunk should use (the smallest the sink moved), or
 * an error; a message that moved no byte is EPROTO.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/display/drm_dp_helper.c (drm_dp_i2c_drain_msg).
 */
static int
i915_xd_aux_i2c_drain(
	struct i915_xd_aux *aux,
	struct i915_xd_io *io,
	unsigned request,
	unsigned address,
	uint8_t *data,
	unsigned size,
	unsigned *next_size)
{
	unsigned left;
	unsigned moved;
	int error;

	/* Moves what is left until nothing is. */
	*next_size = size;
	left = size;
	while (left > 0U) {
		error = i915_xd_aux_i2c_message(aux, io, &request, address, data, left, &moved);
		if (error != 0)
			return error;
		if (moved == 0U)
			return EPROTO;

		/* A short move lowers the size of the next chunks. */
		if (moved < left && moved < *next_size)
			*next_size = moved;

		/* Moves past what was moved. */
		data += moved;
		left -= moved;
	}

	/* Succeeded: the chunk is through. */
	return 0;
}
