/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the xd register window (display/xd-io.c) on the
 * register model (xd-fake-regs.c): reads, writes and read-modify-writes
 * through the hooks, waits that are answered and waits that run out, the
 * write record and its wrap-around, the GT driver mailbox (an answer, a
 * busy mailbox, a refusal and no answer), and the lock order record.
 */

#include "host-test.h"
#include "xd-fake-regs.h"

#include "../../display/xd-io.h"
#include "../../intel/display-regs-xd.h"

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

/* An arbitrary register for the plain access checks. */
#define I915_XD_IO_TEST_REG		0x45400U

/*
 * The firmware's answer the mailbox test installs: the status it writes
 * back, the data word it answers with, and how long it takes.  A delay of
 * 0 means it never answers.
 */
struct i915_xd_io_test_firmware {
	uint32_t status;
	uint32_t answer;
	unsigned delay_us;
	unsigned commands;
	uint32_t last_command;
	uint32_t last_data0;
	uint32_t last_data1;
};

static void i915_xd_io_test_setup(struct i915_xd_io *io);
static void i915_xd_io_test_firmware(uint32_t offset, uint32_t value, void *context);
static void i915_xd_io_test_access(void);
static void i915_xd_io_test_poll(void);
static void i915_xd_io_test_record(void);
static void i915_xd_io_test_mailbox(void);
static void i915_xd_io_test_locks(void);

/*
 * Runs every check of the window and reports.
 */
int
main(void)
{
	int failures;

	/* Runs each group of checks on a fresh model. */
	i915_xd_io_test_access();
	i915_xd_io_test_poll();
	i915_xd_io_test_record();
	i915_xd_io_test_mailbox();
	i915_xd_io_test_locks();

	/* Reports the result. */
	failures = i915_host_report("host_xd_io_test");
	if (failures != 0)
		return 1;

	/* Succeeded: every check passed. */
	return 0;
}

/* Prepares a window that reaches a fresh register model. */
static void
i915_xd_io_test_setup(
	struct i915_xd_io *io)
{
	struct i915_xd_io_hooks hooks;

	/* Empties the model and points the window at it. */
	i915_xd_fake_reset();
	drv_i915_xd_io_init(io, NULL, NULL);
	i915_xd_fake_hooks(&hooks);
	drv_i915_xd_io_set_hooks(io, &hooks);
}

/* Answers a command written to the mailbox the way the test's firmware is set up to. */
static void
i915_xd_io_test_firmware(
	uint32_t offset,
	uint32_t value,
	void *context)
{
	struct i915_xd_io_test_firmware *firmware;

	UNUSED_PARAMETER(offset);
	firmware = context;

	/* Only a write that hands over a command is answered. */
	if ((value & I915_XD_GTDRIVER_MAILBOX_BUSY) == 0U)
		return;

	/* Notes the command and the data the driver sent. */
	firmware->commands++;
	firmware->last_command = value & 0xffU;
	firmware->last_data0 = i915_xd_fake_get(I915_XD_GTDRIVER_DATA0);
	firmware->last_data1 = i915_xd_fake_get(I915_XD_GTDRIVER_DATA1);

	/* A firmware that never answers leaves the busy bit set. */
	if (firmware->delay_us == 0U)
		return;

	/* Answers later: the data word, then the busy bit cleared with the status. */
	i915_xd_fake_later(firmware->delay_us, I915_XD_GTDRIVER_DATA0, 0xffffffffU, firmware->answer);
	i915_xd_fake_later(firmware->delay_us, I915_XD_GTDRIVER_MAILBOX, 0xffffffffU, firmware->status);
}

/* Checks reads, writes, read-modify-writes and the posting read. */
static void
i915_xd_io_test_access(void)
{
	struct i915_xd_io io;
	uint32_t value;

	/* A fresh window on the model. */
	i915_xd_io_test_setup(&io);

	/* A register the model holds reads back through the window. */
	i915_xd_fake_set(I915_XD_IO_TEST_REG, 0x12345678U);
	value = drv_i915_xd_io_read(&io, I915_XD_IO_TEST_REG);
	i915_host_check(value == 0x12345678U, "access: read through the hook");

	/* A write lands in the model. */
	drv_i915_xd_io_write(&io, I915_XD_IO_TEST_REG, 0xcafef00dU);
	i915_host_check(i915_xd_fake_get(I915_XD_IO_TEST_REG) == 0xcafef00dU, "access: write through the hook");

	/* A read-modify-write clears, sets and reports the new value. */
	value = drv_i915_xd_io_rmw(&io, I915_XD_IO_TEST_REG, 0x0000ff00U, 0x00000011U);
	i915_host_check(value == 0xcafe001dU, "access: rmw reports the new value");
	i915_host_check(i915_xd_fake_get(I915_XD_IO_TEST_REG) == 0xcafe001dU, "access: rmw writes the new value");

	/* A read-modify-write that changes nothing still writes. */
	(void)drv_i915_xd_io_rmw(&io, I915_XD_IO_TEST_REG, 0U, 0U);
	i915_host_check(i915_xd_fake_writes() == 3U, "access: rmw writes even when nothing changes");

	/* A posting read reads without writing. */
	drv_i915_xd_io_posting_read(&io, I915_XD_IO_TEST_REG);
	i915_host_check(i915_xd_fake_writes() == 3U, "access: a posting read does not write");

	/* The field helpers place and take values at the mask's position. */
	i915_host_check(i915_xd_field_put(0x00700000U, 5U) == 0x00500000U, "access: field put");
	i915_host_check(i915_xd_field_put(0x00700000U, 9U) == 0x00100000U, "access: field put cuts to the width");
	i915_host_check(i915_xd_field_get(0x00700000U, 0xffffffffU) == 7U, "access: field get");
	i915_host_check(i915_xd_field_put(0U, 5U) == 0U, "access: an empty mask has no field");
}

/* Checks waits that are answered at once, answered later, and never answered. */
static void
i915_xd_io_test_poll(void)
{
	struct i915_xd_io io;
	struct i915_xd_io_record records[4];
	uint32_t last;
	unsigned count;
	int error;

	/* A fresh window on the model. */
	i915_xd_io_test_setup(&io);

	/* A register already at the value ends the wait without time passing. */
	i915_xd_fake_set(I915_XD_PWR_WELL_CTL, I915_XD_PWR_WELL_STATE(I915_XD_WELL_PW_1));
	error = drv_i915_xd_io_poll(&io, I915_XD_PWR_WELL_CTL, I915_XD_PWR_WELL_STATE(I915_XD_WELL_PW_1), I915_XD_PWR_WELL_STATE(I915_XD_WELL_PW_1), 1000U, &last);
	i915_host_check(error == 0, "poll: a register already there");
	i915_host_check(i915_xd_fake_now() == 0U, "poll: no time passes for a register already there");

	/* A request answered by the state bit 30 us later ends the wait at 30 us. */
	i915_xd_io_test_setup(&io);
	i915_xd_fake_rule_mirror(I915_XD_PWR_WELL_CTL, I915_XD_PWR_WELL_REQUEST(I915_XD_WELL_PW_2), I915_XD_PWR_WELL_CTL, -1, 30U);
	drv_i915_xd_io_write(&io, I915_XD_PWR_WELL_CTL, I915_XD_PWR_WELL_REQUEST(I915_XD_WELL_PW_2));
	error = drv_i915_xd_io_poll(&io, I915_XD_PWR_WELL_CTL, I915_XD_PWR_WELL_STATE(I915_XD_WELL_PW_2), I915_XD_PWR_WELL_STATE(I915_XD_WELL_PW_2), 1000U, &last);
	i915_host_check(error == 0, "poll: a state bit that comes later");
	i915_host_check(i915_xd_fake_now() == 30U, "poll: the wait ends when the state bit comes");
	i915_host_check(last == (I915_XD_PWR_WELL_REQUEST(I915_XD_WELL_PW_2) | I915_XD_PWR_WELL_STATE(I915_XD_WELL_PW_2)), "poll: the last value is handed back");

	/* A register that never gets there runs the wait out after the whole timeout. */
	i915_xd_io_test_setup(&io);
	error = drv_i915_xd_io_poll(&io, I915_XD_DBUF_CTL_S1, I915_XD_DBUF_POWER_STATE, I915_XD_DBUF_POWER_STATE, 1000U, &last);
	i915_host_check(error == ETIMEDOUT, "poll: a register that never gets there times out");
	i915_host_check(i915_xd_fake_now() == 1000U, "poll: the timeout is the time slept");

	/* The timeout is in the record with the register and its last value. */
	count = drv_i915_xd_io_records(&io, records, 4U);
	i915_host_check(count == 1U, "poll: the timeout is recorded");
	i915_host_check(records[0].kind == I915_XD_IO_RECORD_TIMEOUT && records[0].offset == I915_XD_DBUF_CTL_S1, "poll: the record names the register");

	/* A wait of 0 us reads once and does not sleep. */
	i915_xd_io_test_setup(&io);
	error = drv_i915_xd_io_poll(&io, I915_XD_DBUF_CTL_S1, I915_XD_DBUF_POWER_STATE, I915_XD_DBUF_POWER_STATE, 0U, NULL);
	i915_host_check(error == ETIMEDOUT && i915_xd_fake_now() == 0U && i915_xd_fake_reads() == 1U, "poll: a wait of 0 us reads once");

	/* A wait for any of two bits ends at the first one that comes, either of them. */
	i915_xd_io_test_setup(&io);
	i915_xd_fake_later(40U, I915_XD_DBUF_CTL_S1, 0U, 0x00000400U);
	error = drv_i915_xd_io_poll_any(&io, I915_XD_DBUF_CTL_S1, 0x00000800U | 0x00000400U, 1000U, &last);
	i915_host_check(error == 0 && i915_xd_fake_now() == 40U && last == 0x00000400U, "poll any: the second bit ends the wait when it comes");

	/* A wait for any of two bits times out like a wait for a value, and is recorded. */
	i915_xd_io_test_setup(&io);
	i915_xd_fake_set(I915_XD_DBUF_CTL_S1, 0x00000200U);
	error = drv_i915_xd_io_poll_any(&io, I915_XD_DBUF_CTL_S1, 0x00000800U | 0x00000400U, 1000U, &last);
	count = drv_i915_xd_io_records(&io, records, 4U);
	i915_host_check(error == ETIMEDOUT && i915_xd_fake_now() == 1000U && last == 0x00000200U && count == 1U &&
			records[0].kind == I915_XD_IO_RECORD_TIMEOUT,
			"poll any: none of the bits times out after the whole wait, recorded");
}

/* Checks the write record: order, steps, and the wrap-around. */
static void
i915_xd_io_test_record(void)
{
	struct i915_xd_io io;
	struct i915_xd_io_record records[I915_XD_IO_RECORDS];
	unsigned count;
	unsigned index;
	int in_order;

	/* A fresh window on the model. */
	i915_xd_io_test_setup(&io);

	/* Writes and steps are recorded in the order they happened. */
	drv_i915_xd_io_step(&io, "first");
	drv_i915_xd_io_write(&io, 0x1000U, 1U);
	drv_i915_xd_io_write(&io, 0x1004U, 2U);
	drv_i915_xd_io_step(&io, "second");
	count = drv_i915_xd_io_records(&io, records, I915_XD_IO_RECORDS);
	i915_host_check(count == 4U, "record: four entries");
	i915_host_check(records[0].kind == I915_XD_IO_RECORD_STEP && records[0].step != NULL && records[0].step[0] == 'f', "record: the first step");
	i915_host_check(records[1].kind == I915_XD_IO_RECORD_WRITE && records[1].offset == 0x1000U && records[1].value == 1U, "record: the first write");
	i915_host_check(records[2].offset == 0x1004U && records[2].value == 2U, "record: the second write");
	i915_host_check(records[3].kind == I915_XD_IO_RECORD_STEP && records[3].step[0] == 's', "record: the second step");

	/* Only the newest entries come back when the caller has less room. */
	count = drv_i915_xd_io_records(&io, records, 2U);
	i915_host_check(count == 2U && records[0].offset == 0x1004U && records[1].kind == I915_XD_IO_RECORD_STEP, "record: the newest entries for a small buffer");

	/* After more writes than the ring holds, the newest ones are kept in order. */
	i915_xd_io_test_setup(&io);
	for (index = 0U; index < I915_XD_IO_RECORDS + 10U; index++)
		drv_i915_xd_io_write(&io, 0x2000U, index);
	count = drv_i915_xd_io_records(&io, records, I915_XD_IO_RECORDS);
	in_order = 1;
	for (index = 0U; index < count; index++) {
		if (records[index].value != index + 10U)
			in_order = 0;
	}

	/* Checks the ring keeps the newest writes in order. */
	i915_host_check(count == I915_XD_IO_RECORDS && in_order != 0, "record: the ring keeps the newest writes in order");
}

/* Checks the GT driver mailbox. */
static void
i915_xd_io_test_mailbox(void)
{
	struct i915_xd_io io;
	struct i915_xd_io_test_firmware firmware;
	uint32_t data0;
	uint32_t data1;
	int error;

	/* A command the firmware answers after 40 us hands back the answer. */
	i915_xd_io_test_setup(&io);
	firmware.status = I915_XD_GTDRIVER_STATUS_OK;
	firmware.answer = 0x0a080402U;
	firmware.delay_us = 40U;
	firmware.commands = 0U;
	i915_xd_fake_rule_call(I915_XD_GTDRIVER_MAILBOX, i915_xd_io_test_firmware, &firmware);
	data0 = 1U;
	data1 = 0U;
	error = drv_i915_xd_io_mailbox(&io, I915_XD_MAILBOX_READ_LATENCY, &data0, &data1, 500U);
	i915_host_check(error == 0, "mailbox: an answered command");
	i915_host_check(data0 == 0x0a080402U, "mailbox: the answer comes back in data 0");
	i915_host_check(firmware.commands == 1U && firmware.last_command == I915_XD_MAILBOX_READ_LATENCY, "mailbox: the command reached the firmware");
	i915_host_check(firmware.last_data0 == 1U && firmware.last_data1 == 0U, "mailbox: the data reached the firmware");
	i915_host_check(i915_xd_fake_locks_held() == 0U, "mailbox: the lock is released");

	/* A command without a second data word sends 0 there. */
	i915_xd_io_test_setup(&io);
	firmware.commands = 0U;
	i915_xd_fake_set(I915_XD_GTDRIVER_DATA1, 0x55U);
	i915_xd_fake_rule_call(I915_XD_GTDRIVER_MAILBOX, i915_xd_io_test_firmware, &firmware);
	data0 = 3U;
	error = drv_i915_xd_io_mailbox(&io, I915_XD_MAILBOX_CDCLK_CONTROL, &data0, NULL, 500U);
	i915_host_check(error == 0 && firmware.last_data1 == 0U, "mailbox: no second data word sends 0");

	/* A mailbox still busy is not taken over and nothing is written. */
	i915_xd_io_test_setup(&io);
	i915_xd_fake_set(I915_XD_GTDRIVER_MAILBOX, I915_XD_GTDRIVER_MAILBOX_BUSY | 0x07U);
	data0 = 0U;
	error = drv_i915_xd_io_mailbox(&io, I915_XD_MAILBOX_READ_LATENCY, &data0, NULL, 500U);
	i915_host_check(error == EAGAIN, "mailbox: a busy mailbox is refused");
	i915_host_check(i915_xd_fake_writes() == 0U, "mailbox: nothing is written to a busy mailbox");
	i915_host_check(i915_xd_fake_locks_held() == 0U, "mailbox: the lock is released after a busy mailbox");

	/* A refusal comes back as its errno. */
	i915_xd_io_test_setup(&io);
	firmware.status = I915_XD_GTDRIVER_STATUS_LOCKED;
	firmware.delay_us = 10U;
	i915_xd_fake_rule_call(I915_XD_GTDRIVER_MAILBOX, i915_xd_io_test_firmware, &firmware);
	data0 = 0U;
	error = drv_i915_xd_io_mailbox(&io, I915_XD_MAILBOX_TCCOLD, &data0, NULL, 500U);
	i915_host_check(error == EBUSY, "mailbox: a locked refusal is EBUSY");

	/* An unknown command is ENXIO, bad data EINVAL, a rejection EACCES. */
	i915_xd_io_test_setup(&io);
	firmware.status = I915_XD_GTDRIVER_STATUS_BAD_COMMAND;
	i915_xd_fake_rule_call(I915_XD_GTDRIVER_MAILBOX, i915_xd_io_test_firmware, &firmware);
	error = drv_i915_xd_io_mailbox(&io, 0x7fU, &data0, NULL, 500U);
	i915_host_check(error == ENXIO, "mailbox: an unknown command is ENXIO");
	firmware.status = I915_XD_GTDRIVER_STATUS_BAD_DATA;
	error = drv_i915_xd_io_mailbox(&io, I915_XD_MAILBOX_SAGV_CONFIG, &data0, NULL, 500U);
	i915_host_check(error == EINVAL, "mailbox: bad data is EINVAL");
	firmware.status = I915_XD_GTDRIVER_STATUS_REJECTED;
	error = drv_i915_xd_io_mailbox(&io, I915_XD_MAILBOX_SAGV_CONFIG, &data0, NULL, 500U);
	i915_host_check(error == EACCES, "mailbox: a rejection is EACCES");

	/* A firmware that never answers runs the wait out. */
	i915_xd_io_test_setup(&io);
	firmware.delay_us = 0U;
	i915_xd_fake_rule_call(I915_XD_GTDRIVER_MAILBOX, i915_xd_io_test_firmware, &firmware);
	error = drv_i915_xd_io_mailbox(&io, I915_XD_MAILBOX_READ_LATENCY, &data0, NULL, 500U);
	i915_host_check(error == ETIMEDOUT, "mailbox: no answer times out");
	i915_host_check(i915_xd_fake_now() == 500U, "mailbox: the wait lasts the timeout");
	i915_host_check(i915_xd_fake_locks_held() == 0U, "mailbox: the lock is released after a timeout");
}

/* Checks the lock order record. */
static void
i915_xd_io_test_locks(void)
{
	struct i915_xd_io io;

	/* A fresh window on the model. */
	i915_xd_io_test_setup(&io);

	/* Taking the locks in the documented order is no violation. */
	drv_i915_xd_io_lock_note(&io, I915_XD_LOCK_DISPLAY, 1);
	drv_i915_xd_io_lock_note(&io, I915_XD_LOCK_AUX, 1);
	drv_i915_xd_io_lock_note(&io, I915_XD_LOCK_MAILBOX, 1);
	drv_i915_xd_io_lock_note(&io, I915_XD_LOCK_DKL, 1);
	i915_host_check(i915_xd_fake_lock_violations() == 0U && i915_xd_fake_locks_held() == 4U, "locks: the documented order");
	drv_i915_xd_io_lock_note(&io, I915_XD_LOCK_DKL, 0);
	drv_i915_xd_io_lock_note(&io, I915_XD_LOCK_MAILBOX, 0);
	drv_i915_xd_io_lock_note(&io, I915_XD_LOCK_AUX, 0);
	drv_i915_xd_io_lock_note(&io, I915_XD_LOCK_DISPLAY, 0);
	i915_host_check(i915_xd_fake_locks_held() == 0U, "locks: all released");

	/* Taking the display lock under the Dekel window lock breaks the order. */
	drv_i915_xd_io_lock_note(&io, I915_XD_LOCK_DKL, 1);
	drv_i915_xd_io_lock_note(&io, I915_XD_LOCK_DISPLAY, 1);
	i915_host_check(i915_xd_fake_lock_violations() == 1U, "locks: an inverted order is counted");
	drv_i915_xd_io_lock_note(&io, I915_XD_LOCK_DISPLAY, 0);
	drv_i915_xd_io_lock_note(&io, I915_XD_LOCK_DKL, 0);

	/* Two AUX channels at once break the order as well. */
	drv_i915_xd_io_lock_note(&io, I915_XD_LOCK_AUX, 1);
	drv_i915_xd_io_lock_note(&io, I915_XD_LOCK_AUX, 1);
	i915_host_check(i915_xd_fake_lock_violations() == 2U, "locks: two AUX channels at once are counted");
	drv_i915_xd_io_lock_note(&io, I915_XD_LOCK_AUX, 0);
	drv_i915_xd_io_lock_note(&io, I915_XD_LOCK_AUX, 0);

	/* The mailbox takes its lock under the display lock without a violation. */
	drv_i915_xd_io_lock_note(&io, I915_XD_LOCK_DISPLAY, 1);
	i915_xd_fake_set(I915_XD_GTDRIVER_MAILBOX, I915_XD_GTDRIVER_MAILBOX_BUSY);
	(void)drv_i915_xd_io_mailbox(&io, I915_XD_MAILBOX_READ_LATENCY, &(uint32_t){ 0U }, NULL, 10U);
	drv_i915_xd_io_lock_note(&io, I915_XD_LOCK_DISPLAY, 0);
	i915_host_check(i915_xd_fake_lock_violations() == 2U && i915_xd_fake_locks_held() == 0U, "locks: the mailbox lock nests under the display lock");
}
