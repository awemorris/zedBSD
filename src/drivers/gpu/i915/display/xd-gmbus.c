/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GMBUS controller of the display 12 and 13 code (see xd-gmbus.h).
 */

#include "xd-gmbus.h"
#include "xd-io.h"

#include "../intel/display-regs-xd-south.h"
#include "../intel/display-regs-xd9.h"

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

/* The bytes the data register moves at once. */
#define I915_XD_GMBUS_WORD_BYTES	4U

/*
 * The patience of the controller: how long it may take to have the next
 * bytes ready or to reach the end of a message, and how long the bus
 * may stay active once a transfer has stopped or been refused.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_gmbus.c (gmbus_wait: 50 ms;
 * gmbus_wait_idle: 10 ms).
 */
#define I915_XD_GMBUS_READY_WAIT_US	50000U
#define I915_XD_GMBUS_IDLE_WAIT_US	10000U

/*
 * The DDC address an EDID is read from, and the bytes of one EDID block.
 *
 * Values referenced: VESA Enhanced Display Data Channel Standard (the
 * EDID's DDC address 0x50, the 128-byte block).
 */
#define I915_XD_GMBUS_DDC_EDID		0x50U
#define I915_XD_GMBUS_EDID_BLOCK	128U

static unsigned i915_xd_gmbus_run(struct i915_xd_io *io, const struct i915_xd_gmbus_request *request, unsigned *failed, uint32_t *status);
static int i915_xd_gmbus_is_offset(const struct i915_xd_gmbus_request *request, unsigned index);
static unsigned i915_xd_gmbus_offset_and_data(struct i915_xd_io *io, const struct i915_xd_gmbus_request *request, unsigned index, uint32_t *status);
static unsigned i915_xd_gmbus_read(struct i915_xd_io *io, const struct i915_xd_gmbus_request *request, const struct i915_xd_gmbus_message *message, uint32_t command, uint32_t *status);
static unsigned i915_xd_gmbus_write(struct i915_xd_io *io, const struct i915_xd_gmbus_request *request, const struct i915_xd_gmbus_message *message, uint32_t command, uint32_t *status);
static uint32_t i915_xd_gmbus_command(const struct i915_xd_gmbus_message *message, uint32_t command, unsigned count);
static unsigned i915_xd_gmbus_await_state(struct i915_xd_io *io, const struct i915_xd_gmbus_request *request, uint32_t ready, uint32_t interrupt, uint32_t *status);
static int i915_xd_gmbus_wait_idle(struct i915_xd_io *io, const struct i915_xd_gmbus_request *request);
static void i915_xd_gmbus_note(const struct i915_xd_gmbus_request *request, unsigned note, unsigned message);

/*
 * Prepares the GMBUS controller of a display: no transfer seen yet.
 */
void
drv_i915_xd_gmbus_init(
	struct i915_xd_gmbus *gmbus)
{
	/* Starts the behaviour counters from nothing. */
	gmbus->transfers = 0U;
	gmbus->refusals = 0U;
	gmbus->restarts = 0U;
	gmbus->timeouts = 0U;
}

/*
 * Leaves the controller idle: no pin selected and no interrupt raised.
 */
void
drv_i915_xd_gmbus_reset(
	struct i915_xd_io *io)
{
	/* Lets the pin go, then silences the controller's interrupts. */
	drv_i915_xd_io_write(io, I915_XD_GMBUS0, I915_XD_GMBUS_PIN_NONE);
	drv_i915_xd_io_write(io, I915_XD_GMBUS4, 0U);
}

/*
 * Tells whether a pin is one of the controller's: the pins of DDI A to
 * C and of the six Type-C ports, the same on the ICP, TGP and ADP south
 * displays.
 */
int
drv_i915_xd_gmbus_pin_valid(
	unsigned pin)
{
	/* The pins of the DDIs with their own DDC. */
	if (pin >= I915_XD_GMBUS_PIN_DDI(0U) && pin <= I915_XD_GMBUS_PIN_DDI(2U))
		return 1;

	/* The pins of the Type-C ports. */
	if (pin >= I915_XD_GMBUS_PIN_TC(0U) && pin <= I915_XD_GMBUS_PIN_TC(5U))
		return 1;

	/* Anything else has no wires behind it. */
	return 0;
}

/*
 * Runs one transfer: selects the pin, sends the messages, ends with a
 * stop, and lets the pin go.
 *
 * A message the target refuses clears the controller; when it is the
 * first message, and only the first time, the transfer starts once
 * more, because a passive adapter on the DDC pins may refuse the very
 * first address it sees.  A controller that never becomes ready is
 * left at once, without a stop.  The result says what the transfer
 * ended with (I915_XD_GMBUS_*), which message failed, how many times
 * it was started, and the status word read last.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_gmbus.c (do_gmbus_xfer).
 */
void
drv_i915_xd_gmbus_transfer(
	struct i915_xd_gmbus *gmbus,
	struct i915_xd_io *io,
	const struct i915_xd_gmbus_request *request,
	struct i915_xd_gmbus_result *result)
{
	unsigned outcome;
	unsigned failed;
	uint32_t status;
	int valid;
	int idle;

	/* Starts from a result that says nothing went wrong. */
	result->outcome = I915_XD_GMBUS_DONE;
	result->failed = 0U;
	result->starts = 0U;
	result->status = 0U;
	gmbus->transfers++;

	/* Refuses a pin without wires, before touching the controller. */
	if (request->display9) {
		valid = drv_i915_xd9_gmbus_pin_valid(request->pin);
	} else {
		valid = drv_i915_xd_gmbus_pin_valid(request->pin);
	}
	if (valid == 0) {
		result->outcome = I915_XD_GMBUS_BAD_PIN;
		return;
	}

	/* Sends the messages, once more when the first one was refused the first time. */
	for (;;) {
		result->starts++;
		failed = 0U;
		status = 0U;
		outcome = i915_xd_gmbus_run(io, request, &failed, &status);
		result->status = status;

		/* A transfer that went through ends with a stop and lets the pin go once the bus is idle. */
		if (outcome == I915_XD_GMBUS_DONE) {
			drv_i915_xd_io_write(io,
					     I915_XD_GMBUS1,
					     I915_XD_GMBUS1_SW_READY |
					     i915_xd_field_put(I915_XD_GMBUS1_CYCLE, I915_XD_GMBUS1_CYCLE_STOP));
			idle = i915_xd_gmbus_wait_idle(io, request);
			drv_i915_xd_io_write(io, I915_XD_GMBUS0, I915_XD_GMBUS_PIN_NONE);
			if (idle != 0) {
				gmbus->timeouts++;
				result->outcome = I915_XD_GMBUS_LINGERED;
			}

			/* The transfer is over. */
			return;
		}

		/* A controller that never became ready is left at once. */
		if (outcome == I915_XD_GMBUS_NOT_READY) {
			gmbus->timeouts++;
			drv_i915_xd_io_write(io, I915_XD_GMBUS0, I915_XD_GMBUS_PIN_NONE);
			result->outcome = I915_XD_GMBUS_NOT_READY;
			result->failed = failed;
			return;
		}

		/*
		 * A refusal: the bus must be idle before the refusal is
		 * cleared, or it stays active and spoils the next transfer.
		 * The software clear resets the controller, which clears the
		 * refusal, and the pin is let go.
		 */
		gmbus->refusals++;
		idle = i915_xd_gmbus_wait_idle(io, request);
		if (idle != 0) {
			gmbus->timeouts++;
			i915_xd_gmbus_note(request, I915_XD_GMBUS_NOTE_STAYED_ACTIVE, failed);
		}

		/* Releases the controller: clears it and lets the pin go. */
		drv_i915_xd_io_write(io, I915_XD_GMBUS1, I915_XD_GMBUS1_SW_CLEAR);
		drv_i915_xd_io_write(io, I915_XD_GMBUS1, 0U);
		drv_i915_xd_io_write(io, I915_XD_GMBUS0, I915_XD_GMBUS_PIN_NONE);
		i915_xd_gmbus_note(request, I915_XD_GMBUS_NOTE_REFUSED, failed);
		result->failed = failed;

		/* A bus that stayed active is not tried again. */
		if (idle != 0) {
			result->outcome = I915_XD_GMBUS_HUNG;
			return;
		}

		/* Only a first message refused the first time starts the transfer again. */
		if (failed != 0U || result->starts > 1U) {
			result->outcome = I915_XD_GMBUS_NO_ACK;
			return;
		}

		/* Counts the restart and notes the retry. */
		gmbus->restarts++;
		i915_xd_gmbus_note(request, I915_XD_GMBUS_NOTE_RETRY, failed);
	}
}

/*
 * Reads whole EDID blocks of the first segment over a display 9 GMBUS pin:
 * the offset and the 128 bytes of each block, in one transfer.  The PCH's
 * GMBUS unit is kept clocked while the transfer runs, which display 9's
 * GMBUS needs to read reliably.  Returns 0, ENXIO when nothing answers,
 * ETIMEDOUT when the controller does not, or EINVAL for a pin without
 * wires.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_gmbus.c (Display WA #0868).
 */
int
drv_i915_xd9_gmbus_edid_blocks(
	struct i915_xd_gmbus *gmbus,
	struct i915_xd_io *io,
	unsigned pin,
	uint8_t *edid,
	unsigned blocks)
{
	struct i915_xd_gmbus_message messages[4];
	struct i915_xd_gmbus_request request;
	struct i915_xd_gmbus_result result;
	uint8_t offsets[2];
	unsigned block;

	/* The first segment holds two blocks. */
	if (blocks > 2U)
		blocks = 2U;

	/* Each block: its offset, then its bytes. */
	for (block = 0U; block < blocks; block++) {
		offsets[block] = (uint8_t)(block * I915_XD_GMBUS_EDID_BLOCK);
		messages[2U * block].address = I915_XD_GMBUS_DDC_EDID;
		messages[2U * block].read = 0;
		messages[2U * block].data = &offsets[block];
		messages[2U * block].length = 1U;
		messages[2U * block + 1U].address = I915_XD_GMBUS_DDC_EDID;
		messages[2U * block + 1U].read = 1;
		messages[2U * block + 1U].data = edid + block * I915_XD_GMBUS_EDID_BLOCK;
		messages[2U * block + 1U].length = I915_XD_GMBUS_EDID_BLOCK;
	}

	/* The transfer on the pin, without the controller's interrupts. */
	request.pin = pin;
	request.messages = messages;
	request.count = 2U * blocks;
	request.interrupts = 0;
	request.display9 = 1;
	request.noted = NULL;
	request.context = NULL;

	/* The unit clocked for the transfer only. */
	(void)drv_i915_xd_io_rmw(io, I915_XD_SOUTH_DSPCLK_GATE_D, 0U, I915_XD9_GMBUS_UNIT_UNGATE);
	drv_i915_xd_gmbus_transfer(gmbus, io, &request, &result);
	(void)drv_i915_xd_io_rmw(io, I915_XD_SOUTH_DSPCLK_GATE_D, I915_XD9_GMBUS_UNIT_UNGATE, 0U);

	/* No monitor answered, or the pin has none. */
	if (result.outcome == I915_XD_GMBUS_NO_ACK)
		return ENXIO;
	if (result.outcome == I915_XD_GMBUS_BAD_PIN)
		return EINVAL;

	/* The controller failed. */
	if (result.outcome != I915_XD_GMBUS_DONE)
		return ETIMEDOUT;

	/* Succeeded: the blocks are read. */
	return 0;
}

/*
 * Tells whether a pin is one of display 9's controller's: the pins of
 * DDIs B to D on the Sunrise Point and Cannon Point families.
 */
int
drv_i915_xd9_gmbus_pin_valid(
	unsigned pin)
{
	/* The three pins. */
	if (pin >= I915_XD9_GMBUS_PIN_DDI_C && pin <= I915_XD9_GMBUS_PIN_DDI_D)
		return 1;

	/* Anything else has no wires behind it. */
	return 0;
}

/*
 * Reports the GMBUS pin of a display 9 DDI (B, C or D), or 0 for a DDI
 * without one.
 */
unsigned
drv_i915_xd9_gmbus_pin(
	unsigned ddi)
{
	/* DDI B. */
	if (ddi == 1U)
		return I915_XD9_GMBUS_PIN_DDI_B;

	/* DDI C. */
	if (ddi == 2U)
		return I915_XD9_GMBUS_PIN_DDI_C;

	/* DDI D. */
	if (ddi == 3U)
		return I915_XD9_GMBUS_PIN_DDI_D;

	/* Succeeded: the DDI has no pin. */
	return 0U;
}

/*
 * Selects the pin and sends every message, an offset write together
 * with the message after it; each message ends with the controller's
 * wait phase.  It returns DONE, NO_ACK or NOT_READY, with the index of
 * the message that failed in *failed (for an offset and its data, the
 * offset's).
 */
static unsigned
i915_xd_gmbus_run(
	struct i915_xd_io *io,
	const struct i915_xd_gmbus_request *request,
	unsigned *failed,
	uint32_t *status)
{
	const struct i915_xd_gmbus_message *message;
	unsigned index;
	unsigned taken;
	unsigned outcome;
	int offset;

	/* Selects the pin at the standard rate. */
	drv_i915_xd_io_write(io, I915_XD_GMBUS0, request->pin | I915_XD_GMBUS0_RATE_100KHZ);

	/* Sends the messages in order. */
	for (index = 0U; index < request->count; index += taken) {
		message = &request->messages[index];

		/* An offset and the message after it go as one indexed message. */
		offset = i915_xd_gmbus_is_offset(request, index);
		if (offset != 0) {
			outcome = i915_xd_gmbus_offset_and_data(io, request, index, status);
			taken = 2U;
		} else if (message->read != 0) {
			outcome = i915_xd_gmbus_read(io, request, message, 0U, status);
			taken = 1U;
		} else {
			outcome = i915_xd_gmbus_write(io, request, message, 0U, status);
			taken = 1U;
		}

		/* The message is over when the controller reaches its wait phase. */
		if (outcome == I915_XD_GMBUS_DONE)
			outcome = i915_xd_gmbus_await_state(io, request, I915_XD_GMBUS2_WAIT_PHASE, I915_XD_GMBUS4_WAIT_PHASE, status);

		/* Stops at the message that failed. */
		if (outcome != I915_XD_GMBUS_DONE) {
			*failed = index;
			return outcome;
		}
	}

	/* Succeeded: every message went through. */
	return I915_XD_GMBUS_DONE;
}

/*
 * Tells whether a message is an offset the controller can send with the
 * message after it: a write of one or two bytes to the same target,
 * followed by a message that moves data.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_gmbus.c (gmbus_is_index_xfer).
 */
static int
i915_xd_gmbus_is_offset(
	const struct i915_xd_gmbus_request *request,
	unsigned index)
{
	const struct i915_xd_gmbus_message *offset;
	const struct i915_xd_gmbus_message *data;

	/* The offset needs a message after it. */
	if (index + 1U >= request->count)
		return 0;

	/* The offset message and the data message. */
	offset = &request->messages[index];
	data = &request->messages[index + 1U];

	/* Both go to the same target. */
	if (offset->address != data->address)
		return 0;

	/* The offset is written. */
	if (offset->read != 0)
		return 0;

	/* The offset is one or two bytes long. */
	if (offset->length != 1U && offset->length != 2U)
		return 0;

	/* The message after it moves data. */
	if (data->length == 0U)
		return 0;

	/* Succeeded: the two go as one indexed message. */
	return 1;
}

/*
 * Sends an offset and the message after it as one indexed message: a
 * one-byte offset rides in the command word, a two-byte offset in the
 * index register, which is cleared again afterwards whatever happened.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_gmbus.c (gmbus_index_xfer).
 */
static unsigned
i915_xd_gmbus_offset_and_data(
	struct i915_xd_io *io,
	const struct i915_xd_gmbus_request *request,
	unsigned index,
	uint32_t *status)
{
	const struct i915_xd_gmbus_message *offset;
	const struct i915_xd_gmbus_message *data;
	uint32_t command;
	uint32_t wide;
	unsigned outcome;

	/* Works out where the offset goes. */
	offset = &request->messages[index];
	data = &request->messages[index + 1U];
	command = 0U;
	wide = 0U;
	if (offset->length == 1U) {
		/* A one-byte offset is the command word's index. */
		command = i915_xd_field_put(I915_XD_GMBUS1_CYCLE, I915_XD_GMBUS1_CYCLE_INDEX) |
			i915_xd_field_put(I915_XD_GMBUS1_INDEX, offset->data[0]);
	} else {
		/* A two-byte offset, first byte high, goes to the index register. */
		wide = I915_XD_GMBUS5_TWO_BYTE_INDEX | ((uint32_t)offset->data[0] << 8) | offset->data[1];
	}

	/* Loads a two-byte offset before the message. */
	if (wide != 0U)
		drv_i915_xd_io_write(io, I915_XD_GMBUS5, wide);

	/* Moves the data with the offset. */
	if (data->read != 0) {
		outcome = i915_xd_gmbus_read(io, request, data, command, status);
	} else {
		outcome = i915_xd_gmbus_write(io, request, data, command, status);
	}

	/* Clears the index register again, so the next message has no offset. */
	if (wide != 0U)
		drv_i915_xd_io_write(io, I915_XD_GMBUS5, 0U);

	/* Reports a message that failed. */
	if (outcome != I915_XD_GMBUS_DONE)
		return outcome;

	/* Succeeded: the offset and the data went through. */
	return I915_XD_GMBUS_DONE;
}

/*
 * Reads one message from the target, in cycles of at most 511 bytes;
 * each cycle starts with its command and its bytes are taken from the
 * data register four at a time, lowest first, as the controller has
 * them ready.  A message of no bytes still sends its command.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_gmbus.c (gmbus_xfer_read,
 * gmbus_xfer_read_chunk).
 */
static unsigned
i915_xd_gmbus_read(
	struct i915_xd_io *io,
	const struct i915_xd_gmbus_request *request,
	const struct i915_xd_gmbus_message *message,
	uint32_t command,
	uint32_t *status)
{
	uint8_t *bytes;
	unsigned remaining;
	unsigned cycle;
	unsigned left;
	unsigned taken;
	unsigned outcome;
	uint32_t word;

	/* Reads the message one cycle after another. */
	bytes = message->data;
	remaining = message->length;
	do {
		/* Starts a cycle of as many bytes as one command may ask for. */
		cycle = remaining;
		if (cycle > I915_XD_GMBUS1_BYTE_COUNT_MAX)
			cycle = I915_XD_GMBUS1_BYTE_COUNT_MAX;
		drv_i915_xd_io_write(io, I915_XD_GMBUS1, i915_xd_gmbus_command(message, command, cycle));

		/* Takes the bytes four at a time once the controller has them. */
		left = cycle;
		while (left != 0U) {
			outcome = i915_xd_gmbus_await_state(io, request, I915_XD_GMBUS2_HW_READY, I915_XD_GMBUS4_HW_READY, status);
			if (outcome != I915_XD_GMBUS_DONE)
				return outcome;

			/* Stores the lowest byte first, up to four of them. */
			word = drv_i915_xd_io_read(io, I915_XD_GMBUS3);
			for (taken = 0U; taken < I915_XD_GMBUS_WORD_BYTES && left != 0U; taken++) {
				*bytes = (uint8_t)(word & 0xffU);
				bytes++;
				word >>= 8;
				left--;
			}
		}

		/* The cycle's bytes are moved. */
		remaining -= cycle;
	} while (remaining != 0U);

	/* Succeeded: the whole message has been read. */
	return I915_XD_GMBUS_DONE;
}

/*
 * Writes one message to the target, in cycles of at most 511 bytes: the
 * first four bytes are in the data register before the cycle's command,
 * the rest follow four at a time, each word waiting for the controller
 * to have taken it.  A message of no bytes still sends its command.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_gmbus.c (gmbus_xfer_write,
 * gmbus_xfer_write_chunk).
 */
static unsigned
i915_xd_gmbus_write(
	struct i915_xd_io *io,
	const struct i915_xd_gmbus_request *request,
	const struct i915_xd_gmbus_message *message,
	uint32_t command,
	uint32_t *status)
{
	const uint8_t *bytes;
	unsigned remaining;
	unsigned cycle;
	unsigned left;
	unsigned packed;
	unsigned outcome;
	uint32_t word;

	/* Writes the message one cycle after another. */
	bytes = message->data;
	remaining = message->length;
	do {
		cycle = remaining;
		if (cycle > I915_XD_GMBUS1_BYTE_COUNT_MAX)
			cycle = I915_XD_GMBUS1_BYTE_COUNT_MAX;
		left = cycle;

		/* Loads the first four bytes, lowest first, and starts the cycle. */
		word = 0U;
		for (packed = 0U; packed < I915_XD_GMBUS_WORD_BYTES && left != 0U; packed++) {
			word |= (uint32_t)*bytes << (8U * packed);
			bytes++;
			left--;
		}

		/* Writes the first word and starts the cycle. */
		drv_i915_xd_io_write(io, I915_XD_GMBUS3, word);
		drv_i915_xd_io_write(io, I915_XD_GMBUS1, i915_xd_gmbus_command(message, command, cycle));

		/* Feeds the rest four bytes at a time, each once the controller took the last. */
		while (left != 0U) {
			word = 0U;
			for (packed = 0U; packed < I915_XD_GMBUS_WORD_BYTES && left != 0U; packed++) {
				word |= (uint32_t)*bytes << (8U * packed);
				bytes++;
				left--;
			}

			/* Writes the word once the controller is ready. */
			drv_i915_xd_io_write(io, I915_XD_GMBUS3, word);
			outcome = i915_xd_gmbus_await_state(io, request, I915_XD_GMBUS2_HW_READY, I915_XD_GMBUS4_HW_READY, status);
			if (outcome != I915_XD_GMBUS_DONE)
				return outcome;
		}

		/* The cycle's bytes are moved. */
		remaining -= cycle;
	} while (remaining != 0U);

	/* Succeeded: the whole message has been written. */
	return I915_XD_GMBUS_DONE;
}

/*
 * Builds the command word of one cycle: ready, a wait at its end, the
 * byte count, the target's address with the direction, and the offset
 * the caller put in command.
 */
static uint32_t
i915_xd_gmbus_command(
	const struct i915_xd_gmbus_message *message,
	uint32_t command,
	unsigned count)
{
	uint32_t word;

	/* Starts the cycle and has it wait at its end. */
	word = command |
		I915_XD_GMBUS1_SW_READY |
		i915_xd_field_put(I915_XD_GMBUS1_CYCLE, I915_XD_GMBUS1_CYCLE_WAIT);

	/* Says how many bytes move and with which target. */
	word |= i915_xd_field_put(I915_XD_GMBUS1_BYTE_COUNT, count);
	word |= i915_xd_field_put(I915_XD_GMBUS1_ADDRESS, message->address);

	/* Reads from the target, or writes to it. */
	if (message->read != 0)
		word |= I915_XD_GMBUS1_READ;

	/* Succeeded: the command word. */
	return word;
}

/*
 * Waits until the controller reports a state (the next bytes ready, or
 * the wait phase at the end of a message) or a refusal, with the
 * matching interrupt raised meanwhile when the device takes interrupts.
 * A refusal wins over the state.  It returns DONE, NO_ACK or NOT_READY.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_gmbus.c (gmbus_wait).
 */
static unsigned
i915_xd_gmbus_await_state(
	struct i915_xd_io *io,
	const struct i915_xd_gmbus_request *request,
	uint32_t ready,
	uint32_t interrupt,
	uint32_t *status)
{
	uint32_t enable;
	int waited;

	/* Raises the one interrupt of the state, when interrupts are taken. */
	enable = 0U;
	if (request->interrupts != 0)
		enable = interrupt;
	drv_i915_xd_io_write(io, I915_XD_GMBUS4, enable);

	/* Waits for the state or a refusal. */
	waited = drv_i915_xd_io_poll_any(io, I915_XD_GMBUS2, ready | I915_XD_GMBUS2_NAK, I915_XD_GMBUS_READY_WAIT_US, status);

	/* Silences the interrupt again. */
	drv_i915_xd_io_write(io, I915_XD_GMBUS4, 0U);

	/* A refusal by the target. */
	if ((*status & I915_XD_GMBUS2_NAK) != 0U)
		return I915_XD_GMBUS_NO_ACK;

	/* A controller that never reached the state. */
	if (waited != 0)
		return I915_XD_GMBUS_NOT_READY;

	/* Succeeded: the controller reached the state. */
	return I915_XD_GMBUS_DONE;
}

/*
 * Waits until the bus is no longer active, with the idle interrupt
 * raised meanwhile when the device takes interrupts.  It returns 0, or
 * ETIMEDOUT for a bus that stayed active.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_gmbus.c (gmbus_wait_idle).
 */
static int
i915_xd_gmbus_wait_idle(
	struct i915_xd_io *io,
	const struct i915_xd_gmbus_request *request)
{
	uint32_t enable;
	int waited;

	/* Raises the idle interrupt, when interrupts are taken. */
	enable = 0U;
	if (request->interrupts != 0)
		enable = I915_XD_GMBUS4_IDLE;
	drv_i915_xd_io_write(io, I915_XD_GMBUS4, enable);

	/* Waits for the bus to go idle. */
	waited = drv_i915_xd_io_poll(io, I915_XD_GMBUS2, I915_XD_GMBUS2_ACTIVE, 0U, I915_XD_GMBUS_IDLE_WAIT_US, NULL);

	/* Silences the interrupt again. */
	drv_i915_xd_io_write(io, I915_XD_GMBUS4, 0U);

	/* Reports a bus that stayed active. */
	if (waited != 0)
		return ETIMEDOUT;

	/* Succeeded: the bus is idle. */
	return 0;
}

/* Tells the caller of an event of its transfer, when it listens. */
static void
i915_xd_gmbus_note(
	const struct i915_xd_gmbus_request *request,
	unsigned note,
	unsigned message)
{
	/* A caller that does not listen is not told. */
	if (request->noted == NULL)
		return;

	/* Hands the event over. */
	request->noted(request->context, note, message);
}
