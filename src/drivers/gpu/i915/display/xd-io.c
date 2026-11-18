/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The register window of the display 12 and 13 code (see xd-io.h).
 *
 * The window is the only place the xd display touches a register, so a
 * host test can stand a register model in for the hardware by setting
 * the hooks, and the log can show the last writes before a failure.
 */

#include "display-internal.h"
#include "xd-io.h"

#include "../mmio.h"
#include "../mmio-record.h"
#include "../sync.h"
#include "../intel/display-regs-xd.h"

#include <kern/clock.h>
#include <kern/klog.h>
#include <kern/lock.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

/*
 * How long a wait spins before it starts to sleep, and the steps it
 * takes: a wait that is answered at once costs a microsecond, and a long
 * one does not keep the CPU.
 */
#define I915_XD_IO_SPIN_LIMIT_US	20U
#define I915_XD_IO_SPIN_STEP_US		1U
#define I915_XD_IO_SHORT_LIMIT_US	200U
#define I915_XD_IO_SHORT_STEP_US	10U
#define I915_XD_IO_LONG_STEP_US		100U

static void i915_xd_io_record(struct i915_xd_io *io, unsigned kind, uint32_t offset, uint32_t value, const char *step);
static int i915_xd_io_wait(struct i915_xd_io *io, uint32_t offset, uint32_t mask, uint32_t value, int any, unsigned timeout_us, uint32_t *last);
static unsigned i915_xd_io_poll_step(unsigned elapsed, unsigned timeout_us);
static int i915_xd_io_mailbox_status(uint32_t command, uint32_t reply);

/*
 * Prepares the register window of a display.
 *
 * The window starts without hooks, with an empty record and without the
 * log trace.
 */
void
drv_i915_xd_io_init(
	struct i915_xd_io *io,
	struct i915_mmio *mmio,
	struct mutex *mailbox_lock)
{
	unsigned index;

	/* Names the hardware the window reaches. */
	io->mmio = mmio;
	io->mailbox_lock = mailbox_lock;

	/* Starts without test replacements. */
	io->hooks.read = NULL;
	io->hooks.write = NULL;
	io->hooks.wait = NULL;
	io->hooks.lock = NULL;
	io->hooks.context = NULL;

	/* Starts with an empty record and the log trace off. */
	io->trace_log = 0;
	io->record_next = 0U;
	io->record_count = 0U;
	for (index = 0U; index < I915_XD_IO_RECORDS; index++) {
		io->record[index].kind = 0U;
		io->record[index].offset = 0U;
		io->record[index].value = 0U;
		io->record[index].step = NULL;
	}
}

/*
 * Installs a host test's replacements for the hardware.
 *
 * Only a test calls it; NULL removes every replacement.
 */
void
drv_i915_xd_io_set_hooks(
	struct i915_xd_io *io,
	const struct i915_xd_io_hooks *hooks)
{
	/* No hooks: the window goes back to the hardware. */
	if (hooks == NULL) {
		io->hooks.read = NULL;
		io->hooks.write = NULL;
		io->hooks.wait = NULL;
		io->hooks.lock = NULL;
		io->hooks.context = NULL;
		return;
	}

	/* Takes the test's replacements as given. */
	io->hooks = *hooks;
}

/*
 * Reads a display register.
 */
uint32_t
drv_i915_xd_io_read(
	struct i915_xd_io *io,
	uint32_t offset)
{
	uint32_t value;

	/* A host test answers from its register model. */
	if (io->hooks.read != NULL) {
		value = io->hooks.read(io->hooks.context, offset);
		return value;
	}

	/* Reads the register through the device's MMIO window. */
	value = drv_i915_read32(io->mmio, offset);

	/* Succeeded: reports the register's value. */
	return value;
}

/*
 * Writes a display register and records the write.
 */
void
drv_i915_xd_io_write(
	struct i915_xd_io *io,
	uint32_t offset,
	uint32_t value)
{
	/* Records the write before it happens, so a write that hangs is still in the record. */
	i915_xd_io_record(io, I915_XD_IO_RECORD_WRITE, offset, value, NULL);

	/* A host test takes the write into its register model. */
	if (io->hooks.write != NULL) {
		io->hooks.write(io->hooks.context, offset, value);
		return;
	}

	/* Writes the register through the device's MMIO window. */
	drv_i915_write32(io->mmio, offset, value);
}

/*
 * Clears and sets bits of a display register and reports the new value.
 *
 * The register is written even when the value does not change, because
 * some registers act on the write itself.
 */
uint32_t
drv_i915_xd_io_rmw(
	struct i915_xd_io *io,
	uint32_t offset,
	uint32_t clear,
	uint32_t set)
{
	uint32_t old_value;
	uint32_t new_value;

	/* Reads the register and works out its new value. */
	old_value = drv_i915_xd_io_read(io, offset);
	new_value = (old_value & ~clear) | set;

	/* Writes the new value. */
	drv_i915_xd_io_write(io, offset, new_value);

	/* Succeeded: reports what was written. */
	return new_value;
}

/*
 * Reads a register only to push the writes before it out to the hardware.
 */
void
drv_i915_xd_io_posting_read(
	struct i915_xd_io *io,
	uint32_t offset)
{
	uint32_t value;

	/* Reads the register; its value is not needed. */
	value = drv_i915_xd_io_read(io, offset);
	(void)value;
}

/*
 * Waits until the masked bits of a register equal a value.
 *
 * The register is read at once and then again after each step, the
 * steps growing from 1 us to 100 us; the wait counts the time it slept,
 * not the time the reads took.  *last (when not NULL) receives the last
 * value read.  Returns 0, ETIMEDOUT when timeout_us passed without the
 * value (the timeout is recorded), or EIO when the time base failed.
 */
int
drv_i915_xd_io_poll(
	struct i915_xd_io *io,
	uint32_t offset,
	uint32_t mask,
	uint32_t value,
	unsigned timeout_us,
	uint32_t *last)
{
	int error;

	/* Waits for the field to read the value. */
	error = i915_xd_io_wait(io, offset, mask, value, 0, timeout_us, last);
	if (error != 0)
		return error;

	/* Succeeded: the register shows the value. */
	return 0;
}

/*
 * Waits until at least one of some bits of a register is set.
 *
 * It waits as drv_i915_xd_io_poll() does, for a register that reports
 * one of several outcomes in different bits (a controller that is
 * either ready or refused).  *last (when not NULL) receives the last
 * value read, which tells the outcomes apart.  Returns 0, ETIMEDOUT
 * (recorded), or EIO when the time base failed.
 */
int
drv_i915_xd_io_poll_any(
	struct i915_xd_io *io,
	uint32_t offset,
	uint32_t bits,
	unsigned timeout_us,
	uint32_t *last)
{
	int error;

	/* Waits for any of the bits. */
	error = i915_xd_io_wait(io, offset, bits, 0U, 1, timeout_us, last);
	if (error != 0)
		return error;

	/* Succeeded: one of the bits is set. */
	return 0;
}

/*
 * Lets time pass between two steps of a sequence.
 *
 * A short delay spins, a longer one sleeps; in a host test the model
 * decides what passing time means.  Returns 0, or EIO when the time base
 * failed during a spin.
 */
int
drv_i915_xd_io_delay(
	struct i915_xd_io *io,
	unsigned microseconds)
{
	int error;

	/* A host test advances its model's clock. */
	if (io->hooks.wait != NULL) {
		io->hooks.wait(io->hooks.context, microseconds);
		return 0;
	}

	/* A long delay sleeps, with a quarter of slack for the scheduler. */
	if (microseconds >= I915_XD_IO_SPIN_LIMIT_US) {
		kern_usleep_range(microseconds, microseconds + microseconds / 4U);
		return 0;
	}

	/* A short delay spins on the time base. */
	error = drv_i915_udelay(microseconds);
	if (error != 0)
		return EIO;

	/* Succeeded: the time has passed. */
	return 0;
}

/*
 * Records the name of the step a sequence is entering.
 *
 * The name must live as long as the display (a string literal).
 */
void
drv_i915_xd_io_step(
	struct i915_xd_io *io,
	const char *name)
{
	/* Adds the step to the record. */
	i915_xd_io_record(io, I915_XD_IO_RECORD_STEP, 0U, 0U, name);

	/* Names the device's next writes in the start's record of the display writes. */
	if (io->mmio != NULL)
		drv_i915_mmio_record_mark(name);
}

/*
 * Copies the kept part of the record, oldest first.
 *
 * Returns how many entries were copied: at most max, and at most the
 * size of the ring.
 */
unsigned
drv_i915_xd_io_records(
	const struct i915_xd_io *io,
	struct i915_xd_io_record *out,
	unsigned max)
{
	unsigned kept;
	unsigned first;
	unsigned index;

	/* Works out how many entries the ring still holds and where the oldest is. */
	kept = io->record_count;
	if (kept > I915_XD_IO_RECORDS)
		kept = I915_XD_IO_RECORDS;
	first = (io->record_next + I915_XD_IO_RECORDS - kept) % I915_XD_IO_RECORDS;

	/* Copies only the newest entries when the caller has less room. */
	if (kept > max) {
		first = (first + (kept - max)) % I915_XD_IO_RECORDS;
		kept = max;
	}

	/* Copies the entries in the order they were recorded. */
	for (index = 0U; index < kept; index++)
		out[index] = io->record[(first + index) % I915_XD_IO_RECORDS];

	/* Succeeded: reports the number of entries copied. */
	return kept;
}

/*
 * Prints the last entries of the record to the log, oldest first.
 */
void
drv_i915_xd_io_log_records(
	const struct i915_xd_io *io,
	unsigned count)
{
	const struct i915_xd_io_record *entry;
	unsigned kept;
	unsigned first;
	unsigned index;

	/* Works out where the entries to print begin. */
	kept = io->record_count;
	if (kept > I915_XD_IO_RECORDS)
		kept = I915_XD_IO_RECORDS;
	if (kept > count)
		kept = count;
	first = (io->record_next + I915_XD_IO_RECORDS - kept) % I915_XD_IO_RECORDS;

	/* Prints each entry in the form of its kind. */
	for (index = 0U; index < kept; index++) {
		entry = &io->record[(first + index) % I915_XD_IO_RECORDS];

		/* A step names itself, a timeout and a write name the register. */
		if (entry->kind == I915_XD_IO_RECORD_STEP) {
			kern_logf("i915: xd: step %s\n", entry->step);
		} else if (entry->kind == I915_XD_IO_RECORD_TIMEOUT) {
			kern_logf("i915: xd: timeout 0x%06x last 0x%08x\n", entry->offset, entry->value);
		} else {
			kern_logf("i915: xd: write 0x%06x 0x%08x\n", entry->offset, entry->value);
		}
	}
}

/*
 * Sends one command to the GT driver mailbox and waits for its answer.
 *
 * *data0 and *data1 (data1 may be NULL, then 0 is sent) carry the
 * command's data and receive the answer.  The device's sideband lock is
 * held for the whole exchange.  Returns 0, EAGAIN when the mailbox was
 * still busy with another command, ETIMEDOUT when no answer came within
 * timeout_us, or the firmware's refusal as an errno.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/intel_pcode.c
 * (__snb_pcode_rw, gen7_check_mailbox_status).
 */
int
drv_i915_xd_io_mailbox(
	struct i915_xd_io *io,
	uint32_t command,
	uint32_t *data0,
	uint32_t *data1,
	unsigned timeout_us)
{
	uint32_t request;
	uint32_t reply;
	uint32_t second;
	int error;

	/* Works out the second data word, which a command without one sends as 0. */
	second = 0U;
	if (data1 != NULL)
		second = *data1;

	/* Takes the sideband lock, which every user of the mailbox holds. */
	drv_i915_xd_io_lock_note(io, I915_XD_LOCK_MAILBOX, 1);
	if (io->mailbox_lock != NULL)
		mutex_lock(io->mailbox_lock);

	/* A mailbox still busy with another command is not taken over. */
	request = drv_i915_xd_io_read(io, I915_XD_GTDRIVER_MAILBOX);
	if ((request & I915_XD_GTDRIVER_MAILBOX_BUSY) != 0U) {
		if (io->mailbox_lock != NULL)
			mutex_unlock(io->mailbox_lock);
		drv_i915_xd_io_lock_note(io, I915_XD_LOCK_MAILBOX, 0);

		return EAGAIN;
	}

	/* Puts the data in place and hands the command to the firmware. */
	drv_i915_xd_io_write(io, I915_XD_GTDRIVER_DATA0, *data0);
	drv_i915_xd_io_write(io, I915_XD_GTDRIVER_DATA1, second);
	drv_i915_xd_io_write(io, I915_XD_GTDRIVER_MAILBOX, I915_XD_GTDRIVER_MAILBOX_BUSY | command);

	/* Waits until the firmware clears the busy bit, which means it has answered. */
	reply = 0U;
	error = drv_i915_xd_io_poll(io, I915_XD_GTDRIVER_MAILBOX, I915_XD_GTDRIVER_MAILBOX_BUSY, 0U, timeout_us, &reply);
	if (error != 0) {
		if (io->mailbox_lock != NULL)
			mutex_unlock(io->mailbox_lock);
		drv_i915_xd_io_lock_note(io, I915_XD_LOCK_MAILBOX, 0);

		return error;
	}

	/* Takes the answer out of the data words. */
	*data0 = drv_i915_xd_io_read(io, I915_XD_GTDRIVER_DATA0);
	if (data1 != NULL)
		*data1 = drv_i915_xd_io_read(io, I915_XD_GTDRIVER_DATA1);

	/* Releases the sideband lock. */
	if (io->mailbox_lock != NULL)
		mutex_unlock(io->mailbox_lock);
	drv_i915_xd_io_lock_note(io, I915_XD_LOCK_MAILBOX, 0);

	/* Turns the firmware's status into the answer. */
	error = i915_xd_io_mailbox_status(command, reply);
	if (error != 0)
		return error;

	/* Succeeded: the firmware accepted the command and the data words hold its answer. */
	return 0;
}

/*
 * Tells a host test that an xd lock was taken or released.
 *
 * The device has no lock hook and this does nothing there; the lock
 * itself is taken by the caller.
 */
void
drv_i915_xd_io_lock_note(
	struct i915_xd_io *io,
	unsigned lock,
	int taken)
{
	/* Only a host test watches the lock order. */
	if (io->hooks.lock == NULL)
		return;

	/* Reports the lock to the test. */
	io->hooks.lock(io->hooks.context, lock, taken);
}

/* Adds one entry to the record, overwriting the oldest when the ring is full. */
static void
i915_xd_io_record(
	struct i915_xd_io *io,
	unsigned kind,
	uint32_t offset,
	uint32_t value,
	const char *step)
{
	struct i915_xd_io_record *entry;

	/* Fills the next slot of the ring. */
	entry = &io->record[io->record_next];
	entry->kind = kind;
	entry->offset = offset;
	entry->value = value;
	entry->step = step;

	/*
	 * Moves the ring on.  The count keeps growing past the ring's size so
	 * the reader can tell that older entries were overwritten.
	 */
	io->record_next = (io->record_next + 1U) % I915_XD_IO_RECORDS;
	io->record_count++;

	/* Prints the entry as well while the log trace is on. */
	if (io->trace_log == 0)
		return;

	/* A step names itself; a write and a timeout name the register. */
	if (kind == I915_XD_IO_RECORD_STEP) {
		kern_logf("i915: xd: step %s\n", step);
	} else if (kind == I915_XD_IO_RECORD_TIMEOUT) {
		kern_logf("i915: xd: timeout 0x%06x last 0x%08x\n", offset, value);
	} else {
		kern_logf("i915: xd: write 0x%06x 0x%08x\n", offset, value);
	}
}

/*
 * Reads a register until its masked bits equal a value, or (any
 * nonzero) until one of them is set, or the time is up; the waits of
 * drv_i915_xd_io_poll() and drv_i915_xd_io_poll_any().
 */
static int
i915_xd_io_wait(
	struct i915_xd_io *io,
	uint32_t offset,
	uint32_t mask,
	uint32_t value,
	int any,
	unsigned timeout_us,
	uint32_t *last)
{
	uint32_t word;
	unsigned elapsed;
	unsigned step;
	int error;

	/* Reads the register until it shows what is waited for or the time is up. */
	elapsed = 0U;
	for (;;) {
		/* Reads the register and hands back what it held. */
		word = drv_i915_xd_io_read(io, offset);
		if (last != NULL)
			*last = word;

		/* A wait for any of the bits is over when one is set. */
		if (any != 0 && (word & mask) != 0U)
			return 0;

		/* A wait for a value is over when the field shows it. */
		if (any == 0 && (word & mask) == value)
			return 0;

		/* The time is up: the wait failed. */
		if (elapsed >= timeout_us)
			break;

		/* Lets the next step of time pass. */
		step = i915_xd_io_poll_step(elapsed, timeout_us);
		error = drv_i915_xd_io_delay(io, step);
		if (error != 0)
			return error;
		elapsed += step;
	}

	/* Records the register and its last value, and reports the timeout. */
	i915_xd_io_record(io, I915_XD_IO_RECORD_TIMEOUT, offset, word, NULL);

	/* The wait ran out. */
	return ETIMEDOUT;
}

/* Chooses the length of the next step of a wait. */
static unsigned
i915_xd_io_poll_step(
	unsigned elapsed,
	unsigned timeout_us)
{
	unsigned step;

	/* Spins in single microseconds first, then in tens, then in hundreds. */
	if (elapsed < I915_XD_IO_SPIN_LIMIT_US) {
		step = I915_XD_IO_SPIN_STEP_US;
	} else if (elapsed < I915_XD_IO_SHORT_LIMIT_US) {
		step = I915_XD_IO_SHORT_STEP_US;
	} else {
		step = I915_XD_IO_LONG_STEP_US;
	}

	/* The last step only goes to the end of the wait. */
	if (step > timeout_us - elapsed)
		step = timeout_us - elapsed;

	/* Succeeded: reports the step's length. */
	return step;
}

/*
 * Turns the status the firmware wrote into the mailbox into an errno.
 *
 * A status this table does not know is logged and taken as success,
 * because the command was answered.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/intel_pcode.c
 * (gen7_check_mailbox_status).
 */
static int
i915_xd_io_mailbox_status(
	uint32_t command,
	uint32_t reply)
{
	uint32_t status;

	/* Takes the status out of the request word. */
	status = reply & I915_XD_GTDRIVER_MAILBOX_STATUS;

	/* Names the firmware's answer. */
	switch (status) {
	case I915_XD_GTDRIVER_STATUS_OK:
		return 0;
	case I915_XD_GTDRIVER_STATUS_BAD_COMMAND:
		return ENXIO;
	case I915_XD_GTDRIVER_STATUS_TIMEOUT:
		return ETIMEDOUT;
	case I915_XD_GTDRIVER_STATUS_BAD_DATA:
		return EINVAL;
	case I915_XD_GTDRIVER_STATUS_BAD_SUBCOMMAND:
		return ENXIO;
	case I915_XD_GTDRIVER_STATUS_LOCKED:
		return EBUSY;
	case I915_XD_GTDRIVER_STATUS_REJECTED:
		return EACCES;
	case I915_XD_GTDRIVER_STATUS_OUT_OF_RANGE:
		return EOVERFLOW;
	default:
		break;
	}

	/* Logs a status the table does not know. */
	kern_logf("i915: xd: mailbox command 0x%02x answered with unknown status 0x%02x\n", command & 0xffU, status);

	/* Succeeded: the command was answered. */
	return 0;
}
