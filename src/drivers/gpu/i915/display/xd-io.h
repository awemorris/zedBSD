/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The register window of the display 12 and 13 code (xd-io.c).
 *
 * Every register access of the xd display goes through one struct
 * i915_xd_io: reads, writes, read-modify-writes, waits for a register to
 * reach a value, the delays between steps, and one command to the GT
 * driver mailbox.  The window keeps a record of the last writes and of
 * named steps, which the log prints when a sequence fails and which the
 * host tests compare with the expected order.
 *
 * A host test replaces the register access and the passing of time with
 * hooks (struct i915_xd_io_hooks), so the same code runs against a model
 * of the registers.  The device leaves the hooks empty and the window
 * reaches the MMIO BAR.
 *
 * Lock order of the xd display, outermost first (the kernel's lock ranks
 * do not tell these apart, so the order is kept by convention and
 * checked by the host tests through the lock hook):
 *
 *   I915_XD_LOCK_DISPLAY   the display lock (hotplug, requests, DMC load)
 *   I915_XD_LOCK_AUX       one AUX channel
 *   I915_XD_LOCK_MAILBOX   the GT driver mailbox (the device's sideband lock)
 *   I915_XD_LOCK_DKL       the Dekel PHY window
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_XD_IO_H
#define DRIVERS_GPU_I915_DISPLAY_XD_IO_H

#include <stdint.h>

/* The number of entries the write record keeps; older entries are overwritten. */
#define I915_XD_IO_RECORDS		256U

/* What one entry of the write record holds. */
#define I915_XD_IO_RECORD_WRITE		1U
#define I915_XD_IO_RECORD_STEP		2U
#define I915_XD_IO_RECORD_TIMEOUT	3U

/* The locks of the xd display, in the order they may be taken (see above). */
#define I915_XD_LOCK_DISPLAY		1U
#define I915_XD_LOCK_AUX		2U
#define I915_XD_LOCK_MAILBOX		3U
#define I915_XD_LOCK_DKL		4U

struct i915_mmio;
struct mutex;

/*
 * The replacements a host test installs for the hardware.
 *
 * read and write stand for the register access, wait for the passing of
 * time (the model may change registers when time passes), and lock is
 * told every time an xd lock is taken (taken nonzero) or released.  A
 * NULL hook leaves the device's behaviour in place.
 */
struct i915_xd_io_hooks {
	uint32_t (*read)(void *context, uint32_t offset);
	void (*write)(void *context, uint32_t offset, uint32_t value);
	void (*wait)(void *context, unsigned microseconds);
	void (*lock)(void *context, unsigned lock, int taken);
	void *context;
};

/*
 * One entry of the write record: a register write, a named step, or a
 * wait that ran out.
 */
struct i915_xd_io_record {
	/* I915_XD_IO_RECORD_WRITE, _STEP or _TIMEOUT. */
	unsigned kind;

	/* The register written or waited on; 0 for a step. */
	uint32_t offset;

	/* The value written, or the last value read when a wait ran out. */
	uint32_t value;

	/* The step's name; NULL for a write or a timeout. */
	const char *step;
};

/*
 * The register window of one display.
 *
 * The display owns it for the device's life.  The record is written by
 * whichever thread holds the display lock, so it needs no lock of its
 * own; the interrupt handler does not write registers through it.
 */
struct i915_xd_io {
	/* The MMIO BAR of the device; NULL in a host test that hooks every access. */
	struct i915_mmio *mmio;

	/* The device's sideband lock, which serializes the GT driver mailbox; NULL in a host test. */
	struct mutex *mailbox_lock;

	/* The test's replacements; all NULL on the device. */
	struct i915_xd_io_hooks hooks;

	/* Nonzero while every write and step is also printed to the log. */
	int trace_log;

	/*
	 * The record: the slot written next, and how many entries were ever
	 * recorded (it keeps counting past the size of the ring, so the
	 * oldest kept entry is count - I915_XD_IO_RECORDS once it wrapped).
	 */
	unsigned record_next;
	unsigned record_count;
	struct i915_xd_io_record record[I915_XD_IO_RECORDS];
};

/*
 * Places a value into the field a mask describes, and takes it out of a
 * register word.  The field starts at the mask's lowest set bit; a mask
 * of 0 has no field.
 */
static __inline uint32_t
i915_xd_field_put(
	uint32_t mask,
	uint32_t value)
{
	/* A mask without bits holds nothing. */
	if (mask == 0U)
		return 0U;

	/* Succeeded: the value shifted to the field and cut to its width. */
	return (value << __builtin_ctz(mask)) & mask;
}

/* Takes the value of the field a mask describes out of a register word. */
static __inline uint32_t
i915_xd_field_get(
	uint32_t mask,
	uint32_t word)
{
	/* A mask without bits holds nothing. */
	if (mask == 0U)
		return 0U;

	/* Succeeded: the field's bits shifted down to bit 0. */
	return (word & mask) >> __builtin_ctz(mask);
}

void drv_i915_xd_io_init(struct i915_xd_io *io, struct i915_mmio *mmio, struct mutex *mailbox_lock);
void drv_i915_xd_io_set_hooks(struct i915_xd_io *io, const struct i915_xd_io_hooks *hooks);
uint32_t drv_i915_xd_io_read(struct i915_xd_io *io, uint32_t offset);
void drv_i915_xd_io_write(struct i915_xd_io *io, uint32_t offset, uint32_t value);
uint32_t drv_i915_xd_io_rmw(struct i915_xd_io *io, uint32_t offset, uint32_t clear, uint32_t set);
void drv_i915_xd_io_posting_read(struct i915_xd_io *io, uint32_t offset);
int drv_i915_xd_io_poll(struct i915_xd_io *io, uint32_t offset, uint32_t mask, uint32_t value, unsigned timeout_us, uint32_t *last);
int drv_i915_xd_io_poll_any(struct i915_xd_io *io, uint32_t offset, uint32_t bits, unsigned timeout_us, uint32_t *last);
int drv_i915_xd_io_delay(struct i915_xd_io *io, unsigned microseconds);
void drv_i915_xd_io_step(struct i915_xd_io *io, const char *name);
unsigned drv_i915_xd_io_records(const struct i915_xd_io *io, struct i915_xd_io_record *out, unsigned max);
void drv_i915_xd_io_log_records(const struct i915_xd_io *io, unsigned count);
int drv_i915_xd_io_mailbox(struct i915_xd_io *io, uint32_t command, uint32_t *data0, uint32_t *data1, unsigned timeout_us);
void drv_i915_xd_io_lock_note(struct i915_xd_io *io, unsigned lock, int taken);

#endif /* DRIVERS_GPU_I915_DISPLAY_XD_IO_H */
