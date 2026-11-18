/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the xd GMBUS controller (display/xd-gmbus.c) on the
 * register model (xd-fake-regs.c) with a DDC target behind it: the reset,
 * the pins, EDID reads of a block with an offset and with the segment
 * pointer, a two-byte offset, long writes and long reads, the interrupt
 * enables, and the target's refusals and the controller's faults (a
 * refusal retried once, a refusal of a later message, a bus that stays
 * active, a controller that never becomes ready), with the exact command
 * words and the waits each one costs.
 */

#include "host-test.h"
#include "xd-fake-regs.h"

#include "../../display/xd-gmbus.h"
#include "../../display/xd-io.h"
#include "../../intel/display-regs-xd-south.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The DDC addresses of an EDID and of its segment pointer. */
#define I915_XD_GMBUS_TEST_EDID		0x50U
#define I915_XD_GMBUS_TEST_SEGMENT	0x30U

/* The pin of DDI B, the HDMI port of the machines the display was written on. */
#define I915_XD_GMBUS_TEST_PIN		2U

/*
 * The command words the driver wrote before the controller was rewritten:
 * an indexed read of 128 bytes at offset 0 from the EDID, a one-byte
 * write of the segment pointer, and the stop.
 */
#define I915_XD_GMBUS_TEST_READ_BLOCK	0x468000a1U
#define I915_XD_GMBUS_TEST_SEGMENT_1	0x42010060U
#define I915_XD_GMBUS_TEST_STOP		0x48000000U

/* The most events one transfer reports in these checks. */
#define I915_XD_GMBUS_TEST_NOTES	8U

/*
 * One check run: the window, the controller, the target, and the events
 * the transfer told.
 */
struct i915_xd_gmbus_test {
	struct i915_xd_io io;
	struct i915_xd_gmbus gmbus;
	struct i915_xd_fake_gmbus target;
	unsigned notes[I915_XD_GMBUS_TEST_NOTES];
	unsigned note_messages[I915_XD_GMBUS_TEST_NOTES];
	unsigned note_count;
};

/*
 * The run the checks share; each group starts it afresh with
 * i915_xd_gmbus_test_setup().
 */
static struct i915_xd_gmbus_test i915_xd_gmbus_test_run;

static void i915_xd_gmbus_test_setup(void);
static void i915_xd_gmbus_test_noted(void *context, unsigned note, unsigned message);
static void i915_xd_gmbus_test_transfer(struct i915_xd_gmbus_message *messages, unsigned count, int interrupts, struct i915_xd_gmbus_result *result);
static void i915_xd_gmbus_test_block(uint8_t *offset, uint8_t *bytes, struct i915_xd_gmbus_message *messages);
static unsigned i915_xd_gmbus_test_writes(uint32_t *offsets, uint32_t *values, unsigned max, int with_interrupts);
static void i915_xd_gmbus_test_pins(void);
static void i915_xd_gmbus_test_reads(void);
static void i915_xd_gmbus_test_writing(void);
static void i915_xd_gmbus_test_refusals(void);
static void i915_xd_gmbus_test_faults(void);

/*
 * Runs every check of the GMBUS controller and reports.
 */
int
main(void)
{
	int failures;

	/* Runs each group of checks on a fresh model. */
	i915_xd_gmbus_test_pins();
	i915_xd_gmbus_test_reads();
	i915_xd_gmbus_test_writing();
	i915_xd_gmbus_test_refusals();
	i915_xd_gmbus_test_faults();

	/* Reports the result. */
	failures = i915_host_report("host_xd_gmbus_test");
	if (failures != 0)
		return 1;

	/* Succeeded: every check passed. */
	return 0;
}

/*
 * Starts a fresh model, a window on it, the controller, and an EDID
 * EEPROM behind it whose bytes are their own position times 7 plus 3.
 */
static void
i915_xd_gmbus_test_setup(void)
{
	struct i915_xd_gmbus_test *run;
	struct i915_xd_io_hooks hooks;
	unsigned index;

	/* A fresh model and a window that reaches it. */
	run = &i915_xd_gmbus_test_run;
	memset(run, 0, sizeof(*run));
	i915_xd_fake_reset();
	i915_xd_fake_hooks(&hooks);
	drv_i915_xd_io_init(&run->io, NULL, NULL);
	drv_i915_xd_io_set_hooks(&run->io, &hooks);

	/* The controller and the target. */
	drv_i915_xd_gmbus_init(&run->gmbus);
	for (index = 0U; index < I915_XD_FAKE_GMBUS_BYTES; index++)
		run->target.memory[index] = (uint8_t)(index * 7U + 3U);
	i915_xd_fake_gmbus_attach(&run->target, I915_XD_GMBUS_TEST_EDID);
}

/* Records an event the transfer told. */
static void
i915_xd_gmbus_test_noted(
	void *context,
	unsigned note,
	unsigned message)
{
	struct i915_xd_gmbus_test *run;

	/* Keeps the event and its message, as long as there is room. */
	run = context;
	if (run->note_count == I915_XD_GMBUS_TEST_NOTES)
		return;

	/* Keeps the note and its message. */
	run->notes[run->note_count] = note;
	run->note_messages[run->note_count] = message;
	run->note_count++;
}

/* Runs one transfer on the HDMI pin of DDI B. */
static void
i915_xd_gmbus_test_transfer(
	struct i915_xd_gmbus_message *messages,
	unsigned count,
	int interrupts,
	struct i915_xd_gmbus_result *result)
{
	struct i915_xd_gmbus_request request;

	/* Describes the transfer, whose events are recorded. */
	request.pin = I915_XD_GMBUS_TEST_PIN;
	request.messages = messages;
	request.count = count;
	request.interrupts = interrupts;
	request.display9 = 0;
	request.noted = i915_xd_gmbus_test_noted;
	request.context = &i915_xd_gmbus_test_run;

	/* Runs it. */
	drv_i915_xd_gmbus_transfer(&i915_xd_gmbus_test_run.gmbus, &i915_xd_gmbus_test_run.io, &request, result);
}

/* Fills the two messages of an EDID block read: the offset, then 128 bytes. */
static void
i915_xd_gmbus_test_block(
	uint8_t *offset,
	uint8_t *bytes,
	struct i915_xd_gmbus_message *messages)
{
	/* The offset of the block. */
	messages[0].address = I915_XD_GMBUS_TEST_EDID;
	messages[0].read = 0;
	messages[0].data = offset;
	messages[0].length = 1U;

	/* The block itself. */
	messages[1].address = I915_XD_GMBUS_TEST_EDID;
	messages[1].read = 1;
	messages[1].data = bytes;
	messages[1].length = 128U;
}

/*
 * Copies the writes of the run's window, oldest first; the interrupt
 * enables are left out unless with_interrupts is nonzero.  Returns how
 * many were copied.
 */
static unsigned
i915_xd_gmbus_test_writes(
	uint32_t *offsets,
	uint32_t *values,
	unsigned max,
	int with_interrupts)
{
	struct i915_xd_io_record records[I915_XD_IO_RECORDS];
	unsigned count;
	unsigned index;
	unsigned kept;

	/* Takes the record and keeps the writes asked for. */
	count = drv_i915_xd_io_records(&i915_xd_gmbus_test_run.io, records, I915_XD_IO_RECORDS);
	kept = 0U;
	for (index = 0U; index < count && kept < max; index++) {
		if (records[index].kind != I915_XD_IO_RECORD_WRITE)
			continue;
		if (with_interrupts == 0 && records[index].offset == I915_XD_GMBUS4)
			continue;

		/* Keeps the write. */
		offsets[kept] = records[index].offset;
		values[kept] = records[index].value;
		kept++;
	}

	/* Succeeded: the writes copied. */
	return kept;
}

/* The reset and the pins the controller has. */
static void
i915_xd_gmbus_test_pins(void)
{
	struct i915_xd_gmbus_message message;
	struct i915_xd_gmbus_request request;
	struct i915_xd_gmbus_result result;
	uint32_t offsets[4];
	uint32_t values[4];
	unsigned count;
	uint8_t byte;
	int valid;
	int good;
	unsigned pin;

	/* The reset lets the pin go, then silences the interrupts. */
	i915_xd_gmbus_test_setup();
	drv_i915_xd_gmbus_reset(&i915_xd_gmbus_test_run.io);
	count = i915_xd_gmbus_test_writes(offsets, values, 4U, 1);
	i915_host_check(count == 2U &&
			offsets[0] == I915_XD_GMBUS0 && values[0] == 0U &&
			offsets[1] == I915_XD_GMBUS4 && values[1] == 0U,
			"gmbus: the reset writes GMBUS0 then GMBUS4 with 0");

	/* The pins of DDI A to C and TC1 to TC6 are the controller's, no others. */
	good = 1;
	for (pin = 0U; pin < 16U; pin++) {
		valid = drv_i915_xd_gmbus_pin_valid(pin);
		if ((pin >= 1U &&
		     pin <= 3U) ||
		    (pin >= 9U &&
		     pin <= 14U)) {
			if (valid == 0)
				good = 0;
		} else if (valid != 0) {
			good = 0;
		}
	}

	/* Checks pins 1 to 3 and 9 to 14 only. */
	i915_host_check(good, "gmbus: pins 1 to 3 and 9 to 14 only");

	/* A transfer on a pin without wires touches nothing. */
	i915_xd_gmbus_test_setup();
	message.address = I915_XD_GMBUS_TEST_EDID;
	message.read = 1;
	message.data = &byte;
	message.length = 1U;
	request.pin = 4U;
	request.messages = &message;
	request.count = 1U;
	request.interrupts = 1;
	request.display9 = 0;
	request.noted = NULL;
	request.context = NULL;
	drv_i915_xd_gmbus_transfer(&i915_xd_gmbus_test_run.gmbus, &i915_xd_gmbus_test_run.io, &request, &result);
	i915_host_check(result.outcome == I915_XD_GMBUS_BAD_PIN && i915_xd_fake_writes() == 0U, "gmbus: pin 4 is refused without a write");
}

/* EDID reads: a block with an offset, a block behind the segment pointer, a two-byte offset, a long read. */
static void
i915_xd_gmbus_test_reads(void)
{
	struct i915_xd_gmbus_message messages[3];
	struct i915_xd_gmbus_result result;
	uint32_t offsets[128];
	uint32_t values[128];
	uint8_t bytes[600];
	uint8_t offset;
	uint8_t segment;
	uint8_t wide[2];
	unsigned count;
	unsigned enables;
	unsigned index;
	int same;

	/* The base block: the pin, one indexed read, the stop, the pin let go. */
	i915_xd_gmbus_test_setup();
	offset = 0U;
	i915_xd_gmbus_test_block(&offset, bytes, messages);
	i915_xd_gmbus_test_transfer(messages, 2U, 1, &result);
	count = i915_xd_gmbus_test_writes(offsets, values, 128U, 0);
	i915_host_check(result.outcome == I915_XD_GMBUS_DONE && result.starts == 1U, "gmbus: the base block is read");
	i915_host_check(count == 4U &&
			offsets[0] == I915_XD_GMBUS0 && values[0] == I915_XD_GMBUS_TEST_PIN &&
			offsets[1] == I915_XD_GMBUS1 && values[1] == I915_XD_GMBUS_TEST_READ_BLOCK &&
			offsets[2] == I915_XD_GMBUS1 && values[2] == I915_XD_GMBUS_TEST_STOP &&
			offsets[3] == I915_XD_GMBUS0 && values[3] == 0U,
			"gmbus: the base block's command words are the driver's old ones");
	same = 1;
	for (index = 0U; index < 128U; index++) {
		if (bytes[index] != i915_xd_gmbus_test_run.target.memory[index])
			same = 0;
	}

	/* Checks the base block's bytes, lowest of each word first. */
	i915_host_check(same, "gmbus: the base block's bytes, lowest of each word first");

	/* Every wait raises its interrupt and silences it: 32 data, the wait phase, the idle. */
	count = i915_xd_gmbus_test_writes(offsets, values, 128U, 1);
	enables = 0U;
	same = 1;
	for (index = 0U; index < count; index++) {
		if (offsets[index] != I915_XD_GMBUS4)
			continue;
		if (enables % 2U == 1U && values[index] != 0U)
			same = 0;
		if (enables % 2U == 0U &&
		    enables < 64U &&
		    values[index] != I915_XD_GMBUS4_HW_READY)
			same = 0;
		if (enables == 64U && values[index] != I915_XD_GMBUS4_WAIT_PHASE)
			same = 0;
		if (enables == 66U && values[index] != I915_XD_GMBUS4_IDLE)
			same = 0;
		enables++;
	}

	/* Checks the interrupts of the 32 data waits, the wait phase and the idle. */
	i915_host_check(enables == 68U && same, "gmbus: the interrupts of the 32 data waits, the wait phase and the idle");

	/* Without interrupts the enables are all 0. */
	i915_xd_gmbus_test_setup();
	i915_xd_gmbus_test_transfer(messages, 2U, 0, &result);
	count = i915_xd_gmbus_test_writes(offsets, values, 128U, 1);
	same = 1;
	for (index = 0U; index < count; index++) {
		if (offsets[index] == I915_XD_GMBUS4 && values[index] != 0U)
			same = 0;
	}

	/* Checks no interrupt is raised while the device takes none. */
	i915_host_check(result.outcome == I915_XD_GMBUS_DONE && same, "gmbus: no interrupt is raised while the device takes none");

	/* Block 2: the segment pointer is written first, then the indexed read. */
	i915_xd_gmbus_test_setup();
	segment = 1U;
	offset = 0U;
	messages[0].address = I915_XD_GMBUS_TEST_SEGMENT;
	messages[0].read = 0;
	messages[0].data = &segment;
	messages[0].length = 1U;
	i915_xd_gmbus_test_block(&offset, bytes, &messages[1]);
	i915_xd_gmbus_test_transfer(messages, 3U, 1, &result);
	count = i915_xd_gmbus_test_writes(offsets, values, 128U, 0);
	i915_host_check(result.outcome == I915_XD_GMBUS_DONE &&
			count == 6U &&
			offsets[1] == I915_XD_GMBUS3 && values[1] == 1U &&
			offsets[2] == I915_XD_GMBUS1 && values[2] == I915_XD_GMBUS_TEST_SEGMENT_1 &&
			offsets[3] == I915_XD_GMBUS1 && values[3] == I915_XD_GMBUS_TEST_READ_BLOCK,
			"gmbus: block 2 writes the segment pointer, then reads indexed");
	i915_host_check(bytes[0] == i915_xd_gmbus_test_run.target.memory[256] && bytes[127] == i915_xd_gmbus_test_run.target.memory[383],
			"gmbus: block 2 comes from the second segment");

	/* A two-byte offset goes to the index register, cleared after the data. */
	i915_xd_gmbus_test_setup();
	wide[0] = 0x01U;
	wide[1] = 0x20U;
	messages[0].address = I915_XD_GMBUS_TEST_EDID;
	messages[0].read = 0;
	messages[0].data = wide;
	messages[0].length = 2U;
	messages[1].address = I915_XD_GMBUS_TEST_EDID;
	messages[1].read = 1;
	messages[1].data = bytes;
	messages[1].length = 6U;
	i915_xd_gmbus_test_transfer(messages, 2U, 1, &result);
	count = i915_xd_gmbus_test_writes(offsets, values, 128U, 0);
	i915_host_check(result.outcome == I915_XD_GMBUS_DONE &&
			count == 6U &&
			offsets[1] == I915_XD_GMBUS5 && values[1] == 0x80000120U &&
			offsets[2] == I915_XD_GMBUS1 && values[2] == 0x420600a1U &&
			offsets[3] == I915_XD_GMBUS5 && values[3] == 0U &&
			bytes[0] == i915_xd_gmbus_test_run.target.memory[0x120],
			"gmbus: a two-byte offset in the index register");

	/* A read of 600 bytes goes in two cycles, 511 and 89 bytes. */
	i915_xd_gmbus_test_setup();
	messages[0].address = I915_XD_GMBUS_TEST_EDID;
	messages[0].read = 1;
	messages[0].data = bytes;
	messages[0].length = 600U;
	i915_xd_gmbus_test_transfer(messages, 1U, 1, &result);
	same = 1;
	for (index = 0U; index < 600U; index++) {
		if (bytes[index] != i915_xd_gmbus_test_run.target.memory[index])
			same = 0;
	}

	/* Checks the 600 bytes read in two cycles. */
	i915_host_check(result.outcome == I915_XD_GMBUS_DONE &&
			i915_xd_gmbus_test_run.target.cycles == 2U &&
			i915_xd_gmbus_test_run.target.commands[0] == 0x43ff00a1U &&
			i915_xd_gmbus_test_run.target.commands[1] == 0x425900a1U &&
			same,
			"gmbus: a 600-byte read goes as 511 and 89 bytes");
}

/* Writes: a six-byte write, and a read of no bytes after an offset that is not paired with it. */
static void
i915_xd_gmbus_test_writing(void)
{
	static const uint8_t six[6] = { 0xa0U, 0xa1U, 0xa2U, 0xa3U, 0xa4U, 0xa5U };
	struct i915_xd_gmbus_message messages[2];
	struct i915_xd_gmbus_result result;
	uint32_t offsets[16];
	uint32_t values[16];
	uint8_t data[6];
	uint8_t offset;
	uint8_t none;
	unsigned count;

	/* Six bytes: four before the command, two after it. */
	i915_xd_gmbus_test_setup();
	memcpy(data, six, sizeof(data));
	messages[0].address = I915_XD_GMBUS_TEST_EDID;
	messages[0].read = 0;
	messages[0].data = data;
	messages[0].length = 6U;
	i915_xd_gmbus_test_transfer(messages, 1U, 1, &result);
	count = i915_xd_gmbus_test_writes(offsets, values, 16U, 0);
	i915_host_check(result.outcome == I915_XD_GMBUS_DONE &&
			count == 6U &&
			offsets[1] == I915_XD_GMBUS3 && values[1] == 0xa3a2a1a0U &&
			offsets[2] == I915_XD_GMBUS1 && values[2] == 0x420600a0U &&
			offsets[3] == I915_XD_GMBUS3 && values[3] == 0x0000a5a4U,
			"gmbus: a six-byte write loads four bytes, starts, then feeds two");
	i915_host_check(i915_xd_gmbus_test_run.target.written_bytes == 6U &&
			memcmp(i915_xd_gmbus_test_run.target.written, six, sizeof(six)) == 0,
			"gmbus: the target took the six bytes");

	/* An offset before a read of no bytes is not paired: two plain cycles. */
	i915_xd_gmbus_test_setup();
	offset = 0U;
	messages[0].address = I915_XD_GMBUS_TEST_EDID;
	messages[0].read = 0;
	messages[0].data = &offset;
	messages[0].length = 1U;
	messages[1].address = I915_XD_GMBUS_TEST_EDID;
	messages[1].read = 1;
	messages[1].data = &none;
	messages[1].length = 0U;
	i915_xd_gmbus_test_transfer(messages, 2U, 1, &result);
	count = i915_xd_gmbus_test_writes(offsets, values, 16U, 0);
	i915_host_check(result.outcome == I915_XD_GMBUS_DONE &&
			count == 6U &&
			values[2] == 0x420100a0U &&
			values[3] == 0x420000a1U,
			"gmbus: an offset before a read of no bytes goes as two cycles");
}

/* Refusals: retried once on the first message, not retried on a later one or a second time. */
static void
i915_xd_gmbus_test_refusals(void)
{
	struct i915_xd_gmbus_message messages[3];
	struct i915_xd_gmbus_result result;
	struct i915_xd_gmbus_test *run;
	uint32_t offsets[32];
	uint32_t values[32];
	uint8_t bytes[128];
	uint8_t offset;
	uint8_t segment;
	unsigned count;

	/* The run of the test. */
	run = &i915_xd_gmbus_test_run;

	/* Refused once: cleared, let go, started again, and read. */
	i915_xd_gmbus_test_setup();
	i915_xd_fake_gmbus_script(&run->target, I915_XD_FAKE_GMBUS_NAK, 1U);
	offset = 0U;
	i915_xd_gmbus_test_block(&offset, bytes, messages);
	i915_xd_gmbus_test_transfer(messages, 2U, 1, &result);
	count = i915_xd_gmbus_test_writes(offsets, values, 32U, 0);
	i915_host_check(result.outcome == I915_XD_GMBUS_DONE && result.starts == 2U && bytes[1] == run->target.memory[1],
			"gmbus: a first refusal is retried and the block read");
	i915_host_check(count == 9U &&
			values[2] == I915_XD_GMBUS1_SW_CLEAR &&
			offsets[3] == I915_XD_GMBUS1 && values[3] == 0U &&
			offsets[4] == I915_XD_GMBUS0 && values[4] == 0U &&
			offsets[5] == I915_XD_GMBUS0 && values[5] == I915_XD_GMBUS_TEST_PIN,
			"gmbus: the refusal is cleared, the pin let go and selected again");
	i915_host_check(run->note_count == 2U &&
			run->notes[0] == I915_XD_GMBUS_NOTE_REFUSED && run->note_messages[0] == 0U &&
			run->notes[1] == I915_XD_GMBUS_NOTE_RETRY,
			"gmbus: the refusal and the retry are told");

	/* Refused twice: no third start. */
	i915_xd_gmbus_test_setup();
	i915_xd_fake_gmbus_script(&run->target, I915_XD_FAKE_GMBUS_NAK, 2U);
	i915_xd_gmbus_test_transfer(messages, 2U, 1, &result);
	i915_host_check(result.outcome == I915_XD_GMBUS_NO_ACK && result.starts == 2U && result.failed == 0U &&
			run->gmbus.refusals == 2U && run->note_count == 3U,
			"gmbus: a second refusal ends the transfer");

	/* Nobody at the address: the same as two refusals. */
	i915_xd_gmbus_test_setup();
	messages[0].address = 0x51U;
	messages[1].address = 0x51U;
	i915_xd_gmbus_test_transfer(messages, 2U, 1, &result);
	i915_host_check(result.outcome == I915_XD_GMBUS_NO_ACK && result.starts == 2U, "gmbus: an address nobody answers");

	/* A later message refused: no retry, its index reported. */
	i915_xd_gmbus_test_setup();
	segment = 1U;
	messages[0].address = I915_XD_GMBUS_TEST_SEGMENT;
	messages[0].read = 0;
	messages[0].data = &segment;
	messages[0].length = 1U;
	i915_xd_gmbus_test_block(&offset, bytes, &messages[1]);
	messages[1].address = 0x52U;
	messages[2].address = 0x52U;
	i915_xd_gmbus_test_transfer(messages, 3U, 1, &result);
	i915_host_check(result.outcome == I915_XD_GMBUS_NO_ACK && result.starts == 1U && result.failed == 1U &&
			run->note_count == 1U && run->note_messages[0] == 1U,
			"gmbus: a refusal of a later message is not retried");
}

/* The controller's faults: a bus that stays active, one that lingers, a controller never ready. */
static void
i915_xd_gmbus_test_faults(void)
{
	struct i915_xd_gmbus_message messages[2];
	struct i915_xd_gmbus_result result;
	struct i915_xd_gmbus_test *run;
	uint32_t offsets[16];
	uint32_t values[16];
	uint8_t bytes[128];
	uint8_t offset;
	unsigned count;

	/* The run of the test, reading from offset 0. */
	run = &i915_xd_gmbus_test_run;
	offset = 0U;

	/* A refusal with the bus still active: 10 ms of waiting, told before the refusal, no retry. */
	i915_xd_gmbus_test_setup();
	run->target.stays_active = 1;
	i915_xd_fake_gmbus_script(&run->target, I915_XD_FAKE_GMBUS_NAK, 1U);
	i915_xd_gmbus_test_block(&offset, bytes, messages);
	i915_xd_gmbus_test_transfer(messages, 2U, 1, &result);
	i915_host_check(result.outcome == I915_XD_GMBUS_HUNG && result.starts == 1U &&
			i915_xd_fake_now() == 10000U &&
			run->note_count == 2U &&
			run->notes[0] == I915_XD_GMBUS_NOTE_STAYED_ACTIVE &&
			run->notes[1] == I915_XD_GMBUS_NOTE_REFUSED,
			"gmbus: a bus active after a refusal is waited 10 ms and not retried");

	/* A bus that stays active after the stop: the bytes arrived, the pin is let go anyway. */
	i915_xd_gmbus_test_setup();
	run->target.lingers = 1;
	i915_xd_gmbus_test_transfer(messages, 2U, 1, &result);
	count = i915_xd_gmbus_test_writes(offsets, values, 16U, 0);
	i915_host_check(result.outcome == I915_XD_GMBUS_LINGERED &&
			i915_xd_fake_now() == 10000U &&
			count == 4U && offsets[3] == I915_XD_GMBUS0 && values[3] == 0U &&
			bytes[5] == run->target.memory[5],
			"gmbus: a bus active after the stop is waited 10 ms, then let go");

	/* A controller never ready: 50 ms, no stop, the pin let go, no retry. */
	i915_xd_gmbus_test_setup();
	i915_xd_fake_gmbus_script(&run->target, I915_XD_FAKE_GMBUS_STUCK, 1U);
	i915_xd_gmbus_test_transfer(messages, 2U, 1, &result);
	count = i915_xd_gmbus_test_writes(offsets, values, 16U, 0);
	i915_host_check(result.outcome == I915_XD_GMBUS_NOT_READY && result.starts == 1U &&
			i915_xd_fake_now() == 50000U &&
			run->gmbus.timeouts == 1U &&
			count == 3U &&
			values[1] == I915_XD_GMBUS_TEST_READ_BLOCK &&
			offsets[2] == I915_XD_GMBUS0 && values[2] == 0U,
			"gmbus: a controller never ready is left after 50 ms without a stop");
}
