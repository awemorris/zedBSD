/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * GT power management (see power.h).
 *
 * The GT driver mailbox is the driver's channel to the power controller
 * firmware.  The firmware owns the command register while its run/busy bit
 * is set; the driver posts a command by loading the two data registers and
 * then writing the command with run/busy set, and the firmware answers by
 * clearing run/busy, with the reply in the data registers and a status code
 * in the low byte of the command register.  The registers are always on, so
 * they are reached without forcewake.
 *
 * Every exchange holds the caller's sideband lock.  Its wait is split in two
 * stages: a short busy poll, and then a poll that may sleep.
 */

#include "power.h"
#include "mmio.h"
#include "sync.h"

#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/sched.h>

#include <uapi/errno.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * The mailbox: the command register (run/busy in bit 31, the status code in
 * 7:0 of the answer) and the two data registers.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (GEN6_PCODE_MAILBOX, GEN6_PCODE_READY, GEN6_PCODE_ERROR_MASK,
 * GEN6_PCODE_DATA, GEN6_PCODE_DATA1).
 */
#define I915_PCODE_COMMAND		0x138124U
#define I915_PCODE_DATA0		0x138128U
#define I915_PCODE_DATA1		0x13812cU
#define I915_PCODE_RUN_BUSY		0x80000000U
#define I915_PCODE_STATUS_MASK		0xffU

/*
 * The wait limits: the busy poll of every exchange, the sleeping poll of a
 * read, the last stretch of a request with preemption off, and the nap
 * between two asks of a request.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/intel_pcode.c
 * (snb_pcode_read, snb_pcode_write_timeout, skl_pcode_request).
 */
#define I915_PCODE_BUSY_POLL_US		500U
#define I915_PCODE_READ_SLEEP_MS	20U
#define I915_PCODE_LAST_STRETCH_MS	50U
#define I915_PCODE_NAP_MIN_US		10U
#define I915_PCODE_NAP_MAX_US		20U

/*
 * The firmware's status codes and the errno each one is reported as.  Zero
 * and a code not in the table count as success.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (GEN6_PCODE_ILLEGAL_CMD, GEN7_PCODE_TIMEOUT, GEN7_PCODE_ILLEGAL_DATA,
 * GEN11_PCODE_ILLEGAL_SUBCOMMAND, GEN11_PCODE_LOCKED,
 * GEN7_PCODE_MIN_FREQ_TABLE_GT_RATIO_OUT_OF_RANGE, GEN11_PCODE_REJECTED)
 * and drivers/gpu/drm/i915/intel_pcode.c (gen7_check_mailbox_status).
 */
static const struct {
	uint8_t code;
	int error;
} i915_pcode_statuses[] = {
	{ 0x01U, ENXIO },
	{ 0x02U, ETIMEDOUT },
	{ 0x03U, EINVAL },
	{ 0x04U, ENXIO },
	{ 0x06U, EBUSY },
	{ 0x10U, EOVERFLOW },
	{ 0x11U, EACCES },
};

/* One exchange: what is sent, how long to wait, and how many reply words are read. */
struct i915_pcode_exchange {
	uint32_t command;
	uint32_t data[2];
	unsigned busy_us;
	unsigned sleep_ms;
	unsigned reply_words;
};

static int i915_pcode_error(uint32_t answer);
static int i915_pcode_run(struct i915_mmio *mmio, struct i915_pcode_exchange *ex);
static bool i915_pcode_ask(struct i915_mmio *mmio, uint32_t command, uint32_t request, uint32_t reply_mask, uint32_t reply, int *status);
static int i915_pcode_repeat(struct i915_mmio *mmio, uint32_t command, uint32_t request, uint32_t reply_mask, uint32_t reply, int *status, unsigned ms, bool nap);

/*
 * Reads the reply to a mailbox command.
 *
 * *val is sent as the first data word, and *val1 (zero when val1 is NULL)
 * as the second.  The reply replaces *val and, when val1 is not NULL,
 * *val1; a reply read before a failing status code is handed back too.
 */
int
drv_i915_pcode_read(
	struct mutex *sb_lock,
	struct i915_mmio *mmio,
	uint32_t mbox,
	uint32_t *val,
	uint32_t *val1)
{
	struct i915_pcode_exchange ex;
	int error;

	/* A read waits through both stages and takes the reply. */
	ex.command = mbox;
	ex.data[0] = *val;
	ex.data[1] = 0U;
	if (val1 != NULL)
		ex.data[1] = *val1;
	ex.busy_us = I915_PCODE_BUSY_POLL_US;
	ex.sleep_ms = I915_PCODE_READ_SLEEP_MS;
	ex.reply_words = val1 != NULL ? 2U : 1U;

	mutex_lock(sb_lock);
	error = i915_pcode_run(mmio, &ex);
	mutex_unlock(sb_lock);

	/* The data words hold the reply once it was read, and what was sent otherwise. */
	*val = ex.data[0];
	if (val1 != NULL)
		*val1 = ex.data[1];

	if (error != 0) {
		kern_logf("i915: pcode read mbox=0x%08x err=%d\n", mbox, error);
		return error;
	}
	return 0;
}

/* Sends one value with a mailbox command; only the busy poll is waited through. */
int
drv_i915_pcode_write(
	struct mutex *sb_lock,
	struct i915_mmio *mmio,
	uint32_t mbox,
	uint32_t val)
{
	struct i915_pcode_exchange ex;
	int error;

	ex.command = mbox;
	ex.data[0] = val;
	ex.data[1] = 0U;
	ex.busy_us = I915_PCODE_BUSY_POLL_US;
	ex.sleep_ms = 0U;
	ex.reply_words = 0U;

	mutex_lock(sb_lock);
	error = i915_pcode_run(mmio, &ex);
	mutex_unlock(sb_lock);

	if (error != 0) {
		kern_logf("i915: pcode write mbox=0x%08x err=%d\n", mbox, error);
		return error;
	}
	return 0;
}

/*
 * Asks a mailbox command again and again until the masked reply is the one
 * wanted.
 *
 * The firmware may take a while to agree (a clock change waits for the
 * hardware to settle), so the question is asked at once, then repeated with
 * a nap between two asks for timeout_base_ms, and then repeated without
 * napping, with preemption off, for a last 50 ms.  The whole run holds the
 * sideband lock.  The status code of the last ask is reported before a
 * timeout.
 */
int
drv_i915_pcode_request(
	struct mutex *sb_lock,
	struct i915_mmio *mmio,
	uint32_t mbox,
	uint32_t request,
	uint32_t reply_mask,
	uint32_t reply,
	int timeout_base_ms)
{
	int status;
	int error;

	mutex_lock(sb_lock);

	status = 0;
	if (i915_pcode_ask(mmio, mbox, request, reply_mask, reply, &status)) {
		error = 0;
	} else if (status == EIO) {
		/* The time base failed and can bound nothing more. */
		error = EIO;
	} else {
		error = i915_pcode_repeat(mmio, mbox, request, reply_mask, reply, &status, (unsigned)timeout_base_ms, true);

		/* The last stretch: as many asks as fit, nothing sleeps. */
		if (error == ETIMEDOUT) {
			kern_logf("i915: pcode request mbox=0x%08x timeout, retrying (preempt off)\n", mbox);
			kern_preempt_disable();
			error = i915_pcode_repeat(mmio, mbox, request, reply_mask, reply, &status, I915_PCODE_LAST_STRETCH_MS, false);
			kern_preempt_enable();
		}
	}

	mutex_unlock(sb_lock);

	if (status != 0)
		return status;
	return error;
}

/* The errno of the status code in an answered command register. */
static int
i915_pcode_error(
	uint32_t answer)
{
	unsigned code;
	size_t i;

	code = answer & I915_PCODE_STATUS_MASK;
	for (i = 0; i < sizeof(i915_pcode_statuses) / sizeof(i915_pcode_statuses[0]); i++) {
		if (i915_pcode_statuses[i].code == code)
			return i915_pcode_statuses[i].error;
	}
	return 0;
}

/*
 * Runs one exchange with the sideband lock held: refuses a mailbox the
 * firmware still owns (EAGAIN), posts the command, waits for the answer
 * (ETIMEDOUT, or EIO when the time base failed), reads the reply words asked,
 * and reports the status code as an errno.
 */
static int
i915_pcode_run(
	struct i915_mmio *mmio,
	struct i915_pcode_exchange *ex)
{
	uint32_t answer;
	int error;

	if ((drv_i915_raw_read32(mmio, I915_PCODE_COMMAND) & I915_PCODE_RUN_BUSY) != 0U)
		return EAGAIN;

	/* The data first; the command then hands the mailbox to the firmware. */
	drv_i915_raw_write32(mmio, I915_PCODE_DATA0, ex->data[0]);
	drv_i915_raw_write32(mmio, I915_PCODE_DATA1, ex->data[1]);
	drv_i915_raw_write32(mmio, I915_PCODE_COMMAND, I915_PCODE_RUN_BUSY | ex->command);

	answer = 0U;
	error = drv_i915_wait_reg(mmio, I915_PCODE_COMMAND, I915_PCODE_RUN_BUSY, 0U, ex->busy_us, ex->sleep_ms, &answer);
	if (error != 0)
		return error;

	if (ex->reply_words >= 1U)
		ex->data[0] = drv_i915_raw_read32(mmio, I915_PCODE_DATA0);
	if (ex->reply_words >= 2U)
		ex->data[1] = drv_i915_raw_read32(mmio, I915_PCODE_DATA1);

	return i915_pcode_error(answer);
}

/*
 * Asks once, waiting only through the busy poll.  *status receives the
 * exchange's result; true when the exchange succeeded and its masked reply
 * is the one wanted.
 */
static bool
i915_pcode_ask(
	struct i915_mmio *mmio,
	uint32_t command,
	uint32_t request,
	uint32_t reply_mask,
	uint32_t reply,
	int *status)
{
	struct i915_pcode_exchange ex;

	ex.command = command;
	ex.data[0] = request;
	ex.data[1] = 0U;
	ex.busy_us = I915_PCODE_BUSY_POLL_US;
	ex.sleep_ms = 0U;
	ex.reply_words = 1U;

	*status = i915_pcode_run(mmio, &ex);
	if (*status != 0)
		return false;
	return (ex.data[0] & reply_mask) == reply;
}

/*
 * Asks until the reply is the one wanted or ms milliseconds of the
 * real-time counter have passed, napping between two asks when nap is set.
 * Returns 0, ETIMEDOUT, or EIO when the time base failed.
 */
static int
i915_pcode_repeat(
	struct i915_mmio *mmio,
	uint32_t command,
	uint32_t request,
	uint32_t reply_mask,
	uint32_t reply,
	int *status,
	unsigned ms,
	bool nap)
{
	uint64_t start;
	uint64_t hz;
	uint64_t now;
	uint64_t now_hz;
	uint64_t span;

	start = 0U;
	hz = 0U;
	if (!kern_rtc_read_counter(&start, &hz) || hz == 0U)
		return EIO;
	span = ((uint64_t)ms * hz) / 1000U;

	for (;;) {
		if (i915_pcode_ask(mmio, command, request, reply_mask, reply, status))
			return 0;
		if (*status == EIO)
			return EIO;

		/* A counter that fails or changes its rate is a time-base failure. */
		now = 0U;
		now_hz = 0U;
		if (!kern_rtc_read_counter(&now, &now_hz) || now_hz != hz)
			return EIO;
		if (now - start >= span)
			return ETIMEDOUT;

		if (nap)
			kern_usleep_range(I915_PCODE_NAP_MIN_US, I915_PCODE_NAP_MAX_US);
		else
			kern_compiler_barrier();
	}
}
