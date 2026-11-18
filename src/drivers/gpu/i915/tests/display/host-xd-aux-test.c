/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the xd AUX channel (display/xd-aux.c) on the register
 * model (xd-fake-regs.c) with an AUX sink behind the channel: the control
 * word, the registers of each channel, native reads and writes and
 * I2C-over-AUX through the data words, the sink's refusals and requests to
 * wait, and the channel's own faults (no answer, a damaged reply, a stuck
 * channel, a forbidden reply size, a channel still busy, a request too
 * long), with the number of tries and the waits each one costs.
 */

#include "host-test.h"
#include "xd-fake-regs.h"

#include "../../display/xd-aux.h"
#include "../../display/xd-io.h"
#include "../../intel/display-regs-xd.h"

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The channels the checks use: A (the built-in panel's), and the second Type-C channel. */
#define I915_XD_AUX_TEST_A		0U
#define I915_XD_AUX_TEST_TC2		4U

/*
 * The control word of a four-byte request on an ordinary port: start,
 * clear done, the done interrupt and both errors, the longest timeout,
 * the size 4, 18 fast-wake and 32 ordinary sync pulses (each less one).
 * It is the word the driver wrote before the AUX channel was rewritten.
 */
#define I915_XD_AUX_TEST_CONTROL_4	0xfe40023fU

/*
 * One check run: the model, the window, the channel and its sink, and
 * the stalls the channel told its owner about.
 */
struct i915_xd_aux_test {
	struct i915_xd_io io;
	struct i915_xd_aux aux;
	struct i915_xd_fake_aux sink;
	unsigned stalled;
	uint32_t stalled_status;
};

/*
 * The run the checks share; each group starts it afresh with
 * i915_xd_aux_test_setup().
 */
static struct i915_xd_aux_test i915_xd_aux_test_run;

static void i915_xd_aux_test_setup(unsigned channel);
static void i915_xd_aux_test_stalled(void *context, uint32_t status);
static void i915_xd_aux_test_exchange(const uint8_t *request, unsigned request_bytes, uint8_t *reply, unsigned reply_room, struct i915_xd_aux_result *result);
static void i915_xd_aux_test_control(void);
static void i915_xd_aux_test_native(void);
static void i915_xd_aux_test_i2c(void);
static void i915_xd_aux_test_replies(void);
static void i915_xd_aux_test_faults(void);
static void i915_xd_aux_test_refusals(void);
static void i915_xd_aux_test_dpcd(void);

/*
 * Runs every check of the AUX channel and reports.
 */
int
main(void)
{
	int failures;

	/* Runs each group of checks on a fresh model. */
	i915_xd_aux_test_control();
	i915_xd_aux_test_native();
	i915_xd_aux_test_i2c();
	i915_xd_aux_test_replies();
	i915_xd_aux_test_faults();
	i915_xd_aux_test_refusals();
	i915_xd_aux_test_dpcd();

	/* Reports the result. */
	failures = i915_host_report("host_xd_aux_test");
	if (failures != 0)
		return 1;

	/* Succeeded: every check passed. */
	return 0;
}

/*
 * Starts a fresh model, a window on it, a channel and a sink behind the
 * channel, with a listener for the channel's stalls.
 */
static void
i915_xd_aux_test_setup(
	unsigned channel)
{
	struct i915_xd_aux_test *run;
	struct i915_xd_io_hooks hooks;
	int error;

	/* A fresh model and a window that reaches it. */
	run = &i915_xd_aux_test_run;
	memset(run, 0, sizeof(*run));
	i915_xd_fake_reset();
	i915_xd_fake_hooks(&hooks);
	drv_i915_xd_io_init(&run->io, NULL, NULL);
	drv_i915_xd_io_set_hooks(&run->io, &hooks);

	/* The channel, whose stalls are counted here. */
	error = drv_i915_xd_aux_init(&run->aux, channel);
	i915_host_check(error == 0, "aux: a channel the display has is prepared");
	run->aux.stalled = i915_xd_aux_test_stalled;
	run->aux.context = run;

	/* The sink behind the channel. */
	i915_xd_fake_aux_attach(&run->sink, channel);
}

/* Counts a stall the channel reports and keeps its control word. */
static void
i915_xd_aux_test_stalled(
	void *context,
	uint32_t status)
{
	struct i915_xd_aux_test *run;

	/* Records the stall. */
	run = context;
	run->stalled++;
	run->stalled_status = status;
}

/* Runs one transaction of an ordinary port on the run's channel. */
static void
i915_xd_aux_test_exchange(
	const uint8_t *request,
	unsigned request_bytes,
	uint8_t *reply,
	unsigned reply_room,
	struct i915_xd_aux_result *result)
{
	struct i915_xd_aux_message message;

	/* Describes the message: no Thunderbolt IO, no HDCP key. */
	message.request = request;
	message.request_bytes = request_bytes;
	message.reply = reply;
	message.reply_room = reply_room;
	message.thunderbolt = 0;
	message.hdcp_key = 0;

	/* Runs it. */
	drv_i915_xd_aux_exchange(&i915_xd_aux_test_run.aux, &i915_xd_aux_test_run.io, &message, result);
}

/*
 * The control word: the ordinary one, the Thunderbolt IO and the HDCP
 * key bits, and the channels the display has.
 */
static void
i915_xd_aux_test_control(void)
{
	struct i915_xd_aux_message message;
	struct i915_xd_aux aux;
	uint32_t control;
	int error;

	/* The ordinary word of a four-byte request is the one the driver always wrote. */
	memset(&message, 0, sizeof(message));
	message.request_bytes = 4U;
	control = drv_i915_xd_aux_control(&message);
	i915_host_check(control == I915_XD_AUX_TEST_CONTROL_4, "aux: the control word of a four-byte request");

	/* A three-byte bare address changes only the size. */
	message.request_bytes = 3U;
	control = drv_i915_xd_aux_control(&message);
	i915_host_check(control == ((I915_XD_AUX_TEST_CONTROL_4 & ~I915_XD_AUX_CTL_SIZE) | 0x00300000U),
			"aux: the size field follows the request");

	/* A Type-C port in Thunderbolt mode adds the Thunderbolt IO bit. */
	message.request_bytes = 4U;
	message.thunderbolt = 1;
	control = drv_i915_xd_aux_control(&message);
	i915_host_check(control == (I915_XD_AUX_TEST_CONTROL_4 | 0x00000800U), "aux: the Thunderbolt IO select");

	/* The HDCP key adds its select bit. */
	message.thunderbolt = 0;
	message.hdcp_key = 1;
	control = drv_i915_xd_aux_control(&message);
	i915_host_check(control == (I915_XD_AUX_TEST_CONTROL_4 | 0x00008000U), "aux: the HDCP key select");

	/* The last Type-C channel is the display's; the one after it is not. */
	error = drv_i915_xd_aux_init(&aux, 8U);
	i915_host_check(error == 0 && aux.channel == 8U, "aux: channel 8 (the sixth Type-C channel) exists");
	error = drv_i915_xd_aux_init(&aux, 9U);
	i915_host_check(error == EINVAL, "aux: channel 9 is refused");
}

/*
 * Native reads and writes through the data words, on channel A and on a
 * Type-C channel, with the exact writes of one transaction.
 */
static void
i915_xd_aux_test_native(void)
{
	static const uint8_t read_caps[4] = { 0x90U, 0x00U, 0x00U, 0x0eU };
	static const uint8_t write_five[9] = { 0x80U, 0x01U, 0x00U, 0x04U, 0x0aU, 0x84U, 0x01U, 0x02U, 0x03U };
	struct i915_xd_aux_result result;
	struct i915_xd_io_record records[8];
	uint8_t reply[20];
	unsigned count;
	unsigned index;
	int same;

	/* A 15-byte read of the receiver capabilities on channel A. */
	i915_xd_aux_test_setup(I915_XD_AUX_TEST_A);
	for (index = 0U; index < 15U; index++)
		i915_xd_aux_test_run.sink.dpcd[index] = (uint8_t)(0x11U * (index + 1U));
	i915_xd_aux_test_exchange(read_caps, 4U, reply, 16U, &result);
	same = memcmp(reply + 1, i915_xd_aux_test_run.sink.dpcd, 15U);
	i915_host_check(result.outcome == I915_XD_AUX_SENT && result.reply_bytes == 16U && reply[0] == 0U && same == 0,
			"aux: a native read returns the acknowledgement and the 15 bytes");

	/* The transaction wrote the request word, the control word, and the acknowledgement of the status. */
	count = drv_i915_xd_io_records(&i915_xd_aux_test_run.io, records, 8U);
	i915_host_check(count == 3U &&
			records[0].offset == 0x64014U && records[0].value == 0x9000000eU &&
			records[1].offset == 0x64010U && records[1].value == I915_XD_AUX_TEST_CONTROL_4 &&
			records[2].offset == 0x64010U && records[2].value == (result.status | 0x52000000U),
			"aux: a read writes the request word, the control word, then acknowledges the status");
	i915_host_check(i915_xd_aux_test_run.aux.tries == 1U && i915_xd_fake_now() == 0U,
			"aux: an answered read takes one try and no wait");

	/* A five-byte native write on the second Type-C channel loads three data words. */
	i915_xd_aux_test_setup(I915_XD_AUX_TEST_TC2);
	i915_xd_aux_test_exchange(write_five, 9U, reply, 2U, &result);
	same = memcmp(i915_xd_aux_test_run.sink.dpcd + 0x100U, write_five + 4, 5U);
	i915_host_check(result.outcome == I915_XD_AUX_SENT && result.reply_bytes == 1U && reply[0] == 0U && same == 0,
			"aux: a native write is taken and acknowledged");
	count = drv_i915_xd_io_records(&i915_xd_aux_test_run.io, records, 8U);
	i915_host_check(count == 5U &&
			records[0].offset == 0x64414U && records[0].value == 0x80010004U &&
			records[1].offset == 0x64418U && records[1].value == 0x0a840102U &&
			records[2].offset == 0x6441cU && records[2].value == 0x03000000U &&
			records[3].offset == 0x64410U,
			"aux: the second Type-C channel's registers, the last data word partly filled");
}

/*
 * I2C-over-AUX: a bare address, a one-byte write of the EEPROM offset,
 * and a 16-byte read.
 */
static void
i915_xd_aux_test_i2c(void)
{
	static const uint8_t bare[3] = { 0x40U, 0x00U, 0x50U };
	static const uint8_t offset[5] = { 0x40U, 0x00U, 0x50U, 0x00U, 0x10U };
	static const uint8_t read16[4] = { 0x50U, 0x00U, 0x50U, 0x0fU };
	struct i915_xd_aux_result result;
	uint8_t reply[20];
	unsigned index;
	int same;

	/* An EEPROM with distinct bytes. */
	i915_xd_aux_test_setup(I915_XD_AUX_TEST_A);
	for (index = 0U; index < I915_XD_FAKE_AUX_I2C_BYTES; index++)
		i915_xd_aux_test_run.sink.i2c[index] = (uint8_t)(index ^ 0x5aU);

	/* The bare address is a three-byte request, one partly filled data word. */
	i915_xd_aux_test_exchange(bare, 3U, reply, 2U, &result);
	same = memcmp(i915_xd_aux_test_run.sink.last_request, bare, 3U);
	i915_host_check(result.outcome == I915_XD_AUX_SENT && result.reply_bytes == 1U && reply[0] == 0U &&
			i915_xd_aux_test_run.sink.last_request_bytes == 3U && same == 0,
			"aux: an I2C bare address is sent as three bytes");

	/* The offset write, then the read from it. */
	i915_xd_aux_test_exchange(offset, 5U, reply, 2U, &result);
	i915_xd_aux_test_exchange(read16, 4U, reply, 17U, &result);
	same = memcmp(reply + 1, i915_xd_aux_test_run.sink.i2c + 0x10U, 16U);
	i915_host_check(result.outcome == I915_XD_AUX_SENT && result.reply_bytes == 17U && reply[0] == 0U && same == 0,
			"aux: an I2C read after the offset write returns the EEPROM's bytes");
}

/*
 * The sink's refusals and requests to wait are replies like any other:
 * the channel hands them over after one try.
 */
static void
i915_xd_aux_test_replies(void)
{
	static const uint8_t read1[4] = { 0x90U, 0x00U, 0x00U, 0x00U };
	static const uint8_t i2c_read1[4] = { 0x50U, 0x00U, 0x50U, 0x00U };
	struct i915_xd_aux_result result;
	uint8_t reply[20];

	/* A native refusal. */
	i915_xd_aux_test_setup(I915_XD_AUX_TEST_A);
	i915_xd_fake_aux_script(&i915_xd_aux_test_run.sink, I915_XD_FAKE_AUX_NACK, 0U, 1U);
	i915_xd_aux_test_exchange(read1, 4U, reply, 2U, &result);
	i915_host_check(result.outcome == I915_XD_AUX_SENT && result.reply_bytes == 1U && reply[0] == 0x10U &&
			i915_xd_aux_test_run.aux.tries == 1U,
			"aux: a native NACK is a reply, after one try");

	/* A native request to wait. */
	i915_xd_fake_aux_script(&i915_xd_aux_test_run.sink, I915_XD_FAKE_AUX_DEFER, 0U, 1U);
	i915_xd_aux_test_exchange(read1, 4U, reply, 2U, &result);
	i915_host_check(result.outcome == I915_XD_AUX_SENT && reply[0] == 0x20U && i915_xd_aux_test_run.aux.tries == 2U,
			"aux: a native DEFER is a reply, after one try");

	/* The I2C ones. */
	i915_xd_fake_aux_script(&i915_xd_aux_test_run.sink, I915_XD_FAKE_AUX_NACK, 0U, 1U);
	i915_xd_fake_aux_script(&i915_xd_aux_test_run.sink, I915_XD_FAKE_AUX_DEFER, 0U, 1U);
	i915_xd_aux_test_exchange(i2c_read1, 4U, reply, 2U, &result);
	i915_host_check(result.outcome == I915_XD_AUX_SENT && reply[0] == 0x40U, "aux: an I2C NACK is a reply");
	i915_xd_aux_test_exchange(i2c_read1, 4U, reply, 2U, &result);
	i915_host_check(result.outcome == I915_XD_AUX_SENT && reply[0] == 0x80U, "aux: an I2C DEFER is a reply");
}

/*
 * The channel's faults and the tries and waits each costs: the counts
 * the eDP's host test on its own model expects of the whole transfer
 * (two damaged replies, then success; a forbidden size; a channel stuck
 * for 25 ms; one that stays stuck).
 */
static void
i915_xd_aux_test_faults(void)
{
	static const uint8_t read1[4] = { 0x90U, 0x00U, 0x00U, 0x00U };
	struct i915_xd_aux_result result;
	uint8_t reply[20];

	/* No answer: five tries at once, then the timeout. */
	i915_xd_aux_test_setup(I915_XD_AUX_TEST_A);
	i915_xd_fake_aux_script(&i915_xd_aux_test_run.sink, I915_XD_FAKE_AUX_TIMEOUT, 0U, 5U);
	i915_xd_aux_test_exchange(read1, 4U, reply, 2U, &result);
	i915_host_check(result.outcome == I915_XD_AUX_NO_ANSWER &&
			i915_xd_aux_test_run.aux.tries == 5U &&
			i915_xd_aux_test_run.aux.no_answers == 5U &&
			i915_xd_aux_test_run.sink.transactions == 5U &&
			i915_xd_fake_now() == 0U,
			"aux: a sink that never answers: five tries without a pause, then no answer");

	/* Four tries without an answer, then one: the fifth try's reply. */
	i915_xd_aux_test_setup(I915_XD_AUX_TEST_A);
	i915_xd_fake_aux_script(&i915_xd_aux_test_run.sink, I915_XD_FAKE_AUX_TIMEOUT, 0U, 4U);
	i915_xd_aux_test_exchange(read1, 4U, reply, 2U, &result);
	i915_host_check(result.outcome == I915_XD_AUX_SENT && i915_xd_aux_test_run.aux.tries == 5U,
			"aux: an answer on the fifth try is taken");

	/* Two damaged replies, then the answer: 400 us of rest after each. */
	i915_xd_aux_test_setup(I915_XD_AUX_TEST_A);
	i915_xd_fake_aux_script(&i915_xd_aux_test_run.sink, I915_XD_FAKE_AUX_DAMAGED, 0U, 2U);
	i915_xd_aux_test_exchange(read1, 4U, reply, 2U, &result);
	i915_host_check(result.outcome == I915_XD_AUX_SENT &&
			i915_xd_aux_test_run.aux.tries == 3U &&
			i915_xd_aux_test_run.aux.receive_errors == 2U &&
			i915_xd_fake_now() == 800U,
			"aux: two damaged replies are tried again after 400 us each, then the answer");

	/* Damaged every time: the receive error, after five rests. */
	i915_xd_aux_test_setup(I915_XD_AUX_TEST_A);
	i915_xd_fake_aux_script(&i915_xd_aux_test_run.sink, I915_XD_FAKE_AUX_DAMAGED, 0U, 5U);
	i915_xd_aux_test_exchange(read1, 4U, reply, 2U, &result);
	i915_host_check(result.outcome == I915_XD_AUX_RECEIVE_ERROR &&
			i915_xd_aux_test_run.aux.tries == 5U &&
			i915_xd_fake_now() == 2000U,
			"aux: a reply damaged on every try is a receive error");

	/* A forbidden reply size of 0, then of 21: nothing is unloaded. */
	i915_xd_aux_test_setup(I915_XD_AUX_TEST_A);
	i915_xd_fake_aux_script(&i915_xd_aux_test_run.sink, I915_XD_FAKE_AUX_SIZE, 0U, 1U);
	i915_xd_fake_aux_script(&i915_xd_aux_test_run.sink, I915_XD_FAKE_AUX_SIZE, 21U, 1U);
	i915_xd_aux_test_exchange(read1, 4U, reply, 2U, &result);
	i915_host_check(result.outcome == I915_XD_AUX_BAD_SIZE && result.reply_bytes == 0U,
			"aux: a reply size of 0 is forbidden");
	i915_xd_aux_test_exchange(read1, 4U, reply, 2U, &result);
	i915_host_check(result.outcome == I915_XD_AUX_BAD_SIZE && result.reply_bytes == 21U &&
			i915_xd_aux_test_run.aux.tries == 2U,
			"aux: a reply size of 21 is forbidden, after one try");

	/*
	 * A channel stuck for 25 ms: two tries stall for 10 ms each and are
	 * told to the owner, the third ends with the timeout error when the
	 * channel lets go, and the fourth is answered.
	 */
	i915_xd_aux_test_setup(I915_XD_AUX_TEST_A);
	i915_xd_fake_aux_script(&i915_xd_aux_test_run.sink, I915_XD_FAKE_AUX_STUCK, 25000U, 1U);
	i915_xd_aux_test_exchange(read1, 4U, reply, 2U, &result);
	i915_host_check(result.outcome == I915_XD_AUX_SENT &&
			i915_xd_aux_test_run.aux.tries == 4U &&
			i915_xd_aux_test_run.aux.stalls == 2U &&
			i915_xd_aux_test_run.stalled == 2U &&
			(i915_xd_aux_test_run.stalled_status & I915_XD_AUX_CTL_BUSY) != 0U &&
			i915_xd_aux_test_run.aux.no_answers == 1U &&
			i915_xd_fake_now() >= 25000U,
			"aux: a channel stuck for 25 ms stalls twice, then recovers");

	/* A channel that stays stuck: five stalls, then unfinished. */
	i915_xd_aux_test_setup(I915_XD_AUX_TEST_A);
	i915_xd_fake_aux_script(&i915_xd_aux_test_run.sink, I915_XD_FAKE_AUX_STUCK, 250000U, 1U);
	i915_xd_aux_test_exchange(read1, 4U, reply, 2U, &result);
	i915_host_check(result.outcome == I915_XD_AUX_UNFINISHED &&
			i915_xd_aux_test_run.aux.tries == 5U &&
			i915_xd_aux_test_run.stalled == 5U &&
			i915_xd_fake_now() >= 50000U && i915_xd_fake_now() < 60000U,
			"aux: a channel that stays stuck: five bounded waits, then unfinished");
}

/*
 * What is refused before anything is sent: a channel an earlier
 * transaction still holds, and a request or a reply room larger than the
 * data words; and a reply room smaller than the reply.
 */
static void
i915_xd_aux_test_refusals(void)
{
	static const uint8_t read15[4] = { 0x90U, 0x00U, 0x00U, 0x0eU };
	struct i915_xd_aux_result result;
	uint8_t request[21];
	uint8_t reply[21];
	unsigned writes;

	/* A busy channel: three looks a millisecond apart, no write, and the word read once more. */
	i915_xd_aux_test_setup(I915_XD_AUX_TEST_A);
	i915_xd_fake_set(0x64010U, I915_XD_AUX_CTL_BUSY | 0x123U);
	i915_xd_aux_test_exchange(read15, 4U, reply, 16U, &result);
	i915_host_check(result.outcome == I915_XD_AUX_BUSY &&
			result.status == (I915_XD_AUX_CTL_BUSY | 0x123U) &&
			i915_xd_fake_writes() == 0U &&
			i915_xd_fake_reads() == 4U &&
			i915_xd_fake_now() == 3000U,
			"aux: a busy channel is looked at three times 1 ms apart, then left alone");

	/* A request of 21 bytes, then a reply room of 21: nothing written. */
	i915_xd_aux_test_setup(I915_XD_AUX_TEST_A);
	memset(request, 0, sizeof(request));
	i915_xd_aux_test_exchange(request, 21U, reply, 2U, &result);
	writes = i915_xd_fake_writes();
	i915_host_check(result.outcome == I915_XD_AUX_TOO_LONG && writes == 0U, "aux: a 21-byte request is too long");
	i915_xd_aux_test_exchange(read15, 4U, reply, 21U, &result);
	writes = i915_xd_fake_writes();
	i915_host_check(result.outcome == I915_XD_AUX_TOO_LONG && writes == 0U, "aux: a 21-byte reply room is too long");

	/* A reply larger than the room: only the room is filled. */
	i915_xd_aux_test_setup(I915_XD_AUX_TEST_A);
	memset(reply, 0xeeU, sizeof(reply));
	i915_xd_aux_test_run.sink.dpcd[0] = 0x12U;
	i915_xd_aux_test_exchange(read15, 4U, reply, 2U, &result);
	i915_host_check(result.outcome == I915_XD_AUX_SENT && result.reply_bytes == 2U &&
			reply[0] == 0U && reply[1] == 0x12U && reply[2] == 0xeeU,
			"aux: a reply larger than the room fills only the room");
}

/*
 * The native DPCD accesses over the channel: a read with its wake-up
 * read first, a read longer than one message, a write, a deferred and a
 * refused message sent again, and a sink that never answers.
 */
static void
i915_xd_aux_test_dpcd(void)
{
	struct i915_xd_aux_test *run;
	uint8_t bytes[20];
	uint8_t value;
	unsigned index;
	int error;

	/* A sink with a known DPCD. */
	i915_xd_aux_test_setup(I915_XD_AUX_TEST_A);
	run = &i915_xd_aux_test_run;
	for (index = 0U; index < 20U; index++)
		run->sink.dpcd[0x100U + index] = (uint8_t)(0xa0U + index);
	run->sink.dpcd[0] = 0x14U;

	/* Two bytes: the wake-up read, then the bytes. */
	error = drv_i915_xd_dpcd_read(&run->aux, &run->io, 0x100U, bytes, 2U);
	i915_host_check(error == 0 && bytes[0] == 0xa0U && bytes[1] == 0xa1U && run->sink.transactions == 2U,
			"dpcd: a two-byte read after the wake-up read of the revision");

	/* Twenty bytes: the wake-up read and two messages of 16 and 4. */
	run->sink.transactions = 0U;
	error = drv_i915_xd_dpcd_read(&run->aux, &run->io, 0x100U, bytes, 20U);
	i915_host_check(error == 0 && bytes[15] == 0xafU && bytes[19] == 0xb3U && run->sink.transactions == 3U,
			"dpcd: a 20-byte read in messages of 16 and 4");

	/* A write. */
	run->sink.transactions = 0U;
	value = 0x02U;
	error = drv_i915_xd_dpcd_write(&run->aux, &run->io, 0x600U, &value, 1U);
	i915_host_check(error == 0 && run->sink.dpcd[0x600U] == 0x02U && run->sink.transactions == 1U && run->sink.last_request[0] == 0x80U &&
			run->sink.last_request[1] == 0x06U && run->sink.last_request[3] == 0x00U && run->sink.last_request[4] == 0x02U,
			"dpcd: a one-byte write, its header and byte");

	/* A deferral and a refusal, then the sink takes the write. */
	run->sink.transactions = 0U;
	i915_xd_fake_aux_script(&run->sink, I915_XD_FAKE_AUX_DEFER, 0U, 1U);
	i915_xd_fake_aux_script(&run->sink, I915_XD_FAKE_AUX_NACK, 0U, 1U);
	value = 0x01U;
	error = drv_i915_xd_dpcd_write(&run->aux, &run->io, 0x600U, &value, 1U);
	i915_host_check(error == 0 && run->sink.dpcd[0x600U] == 0x01U && run->sink.transactions == 3U,
			"dpcd: a deferred and a refused write sent again until taken");

	/* A message the sink did not answer on any of its five tries is sent again, and taken. */
	run->sink.transactions = 0U;
	i915_xd_fake_aux_script(&run->sink, I915_XD_FAKE_AUX_TIMEOUT, 0U, 5U);
	value = 0x02U;
	error = drv_i915_xd_dpcd_write(&run->aux, &run->io, 0x600U, &value, 1U);
	i915_host_check(error == 0 && run->sink.dpcd[0x600U] == 0x02U && run->sink.transactions == 6U,
			"dpcd: an unanswered message sent again and taken");
}
