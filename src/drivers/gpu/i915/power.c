/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * GT power management (see power.h).
 *
 * PCODE mailbox: a direct port of Linux's intel_pcode.c.  The caller-owned
 * sideband lock serializes the transaction, the fast stage is an atomic poll
 * and the slow stage may sleep, and the Gen7 mailbox status is turned into an
 * errno.  The mailbox registers are always on, so they are reached without
 * forcewake.
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

/* PCODE mailbox: the GEN6_PCODE_* registers. */
#define I915_PCODE_MAILBOX		0x138124U
#define I915_PCODE_DATA			0x138128U
#define I915_PCODE_DATA1		0x13812cU

/* PCODE mailbox: set by the driver to post a command, cleared by the firmware when done. */
#define I915_PCODE_READY		0x80000000U

/* PCODE mailbox: the status field of the mailbox register. */
#define I915_PCODE_ERROR_MASK		0xffU

/* PCODE mailbox: the Gen6/Gen7/Gen11 status codes. */
#define I915_PCODE_SUCCESS				0x0U
#define I915_PCODE_ILLEGAL_CMD				0x1U
#define I915_PCODE_TIMEOUT				0x2U
#define I915_PCODE_ILLEGAL_DATA				0x3U
#define I915_PCODE_ILLEGAL_SUBCOMMAND			0x4U
#define I915_PCODE_LOCKED				0x6U
#define I915_PCODE_MIN_FREQ_TABLE_GT_RATIO_OUT_OF_RANGE	0x10U
#define I915_PCODE_REJECTED				0x11U

/* PCODE mailbox: the fast and slow poll limits of snb_pcode_read(). */
#define I915_PCODE_FAST_US		500U
#define I915_PCODE_SLOW_MS		20U

/* PCODE mailbox: how long the final, non-preemptible re-request phase lasts. */
#define I915_PCODE_FINAL_POLL_MS	50U

/* PCODE mailbox: the sleep between re-requests in the preemptible phase (usleep_range(10, 20)). */
#define I915_PCODE_REQUEST_SLEEP_MIN_US	10U
#define I915_PCODE_REQUEST_SLEEP_MAX_US	20U

/* PCODE mailbox. */
static int i915_pcode_status(uint32_t mailbox);
static int i915_pcode_rw(struct i915_mmio *mmio, uint32_t mbox, uint32_t *val, uint32_t *val1, unsigned fast_us, unsigned slow_ms, int is_read);
static int i915_pcode_write_timeout(struct mutex *sb_lock, struct i915_mmio *mmio, uint32_t mbox, uint32_t val, unsigned fast_us, unsigned slow_ms);
static int i915_pcode_try_request(struct i915_mmio *mmio, uint32_t mbox, uint32_t request, uint32_t reply_mask, uint32_t reply, int *status);
static int i915_pcode_poll(struct i915_mmio *mmio, uint32_t mbox, uint32_t request, uint32_t reply_mask, uint32_t reply, int *status, unsigned ms, int sleep_between);

/*
 * Reads the response of a PCODE mailbox command (snb_pcode_read()).
 *
 * The command's input is *val (and zero for the second data word); the
 * response replaces *val and, when val1 is not NULL, *val1.  Returns 0, the
 * mailbox status as an errno, ETIMEDOUT, EAGAIN or EIO.
 */
int
drv_i915_pcode_read(
	struct mutex *sb_lock,
	struct i915_mmio *mmio,
	uint32_t mbox,
	uint32_t *val,
	uint32_t *val1)
{
	int error;

	/* Runs the read transaction under the sideband lock. */
	mutex_lock(sb_lock);

	error = i915_pcode_rw(mmio, mbox, val, val1, I915_PCODE_FAST_US, I915_PCODE_SLOW_MS, 1);

	mutex_unlock(sb_lock);

	/* Reports a transaction that failed. */
	if (error != 0) {
		kern_logf("i915: pcode read mbox=0x%08x err=%d\n", mbox, error);
		return error;
	}

	/* Succeeded: *val (and *val1) hold the response. */
	return 0;
}

/*
 * Writes one value to a PCODE mailbox command (snb_pcode_write()).
 *
 * The transaction waits only in the atomic stage, for 500 microseconds.
 * Returns 0, the mailbox status as an errno, ETIMEDOUT, EAGAIN or EIO.
 */
int
drv_i915_snb_pcode_write(
	struct mutex *sb_lock,
	struct i915_mmio *mmio,
	uint32_t mbox,
	uint32_t val)
{
	int error;

	/* Writes with the reference's default limits: 500 microseconds, no slow stage. */
	error = i915_pcode_write_timeout(sb_lock, mmio, mbox, val, I915_PCODE_FAST_US, 0U);
	if (error != 0)
		return error;

	/* Succeeded: the firmware accepted the value. */
	return 0;
}

/*
 * Sends a PCODE request until its reply matches (skl_pcode_request()).
 *
 * The request is re-sent until (reply & reply_mask) == reply, for
 * timeout_base_ms with a short sleep between requests, and then for another
 * 50 ms with preemption off to send as many requests as possible.  Returns 0,
 * the mailbox status as an errno, ETIMEDOUT, or EIO when the time base failed.
 */
int
drv_i915_skl_pcode_request(
	struct mutex *sb_lock,
	struct i915_mmio *mmio,
	uint32_t mbox,
	uint32_t request,
	uint32_t reply_mask,
	uint32_t reply,
	int timeout_base_ms)
{
	int status;
	int matched;
	int error;

	/* Serializes the whole request sequence. */
	mutex_lock(sb_lock);

	/* Primes the firmware with a first request, sent at once as the reference does. */
	status = 0;
	matched = i915_pcode_try_request(mmio, mbox, request, reply_mask, reply, &status);
	if (matched != 0) {
		error = 0;
		goto out;
	}

	/* A time base that failed cannot bound any further request. */
	if (status == EIO) {
		error = EIO;
		goto out;
	}

	/* Re-sends the request, sleeping between requests, for the base timeout. */
	error = i915_pcode_poll(mmio, mbox, request, reply_mask, reply, &status, (unsigned)timeout_base_ms, 1);
	if (error == 0)
		goto out;
	if (error == EIO)
		goto out;

	/*
	 * Retries for another 50 ms with preemption disabled, to maximize the
	 * number of requests (reference).  Preemption is really disabled -- a
	 * scheduler preempt count, not an interrupts-off spin -- and nothing
	 * sleeps in this phase.
	 */
	kern_logf("i915: pcode request mbox=0x%08x timeout, retrying (preempt off)\n", mbox);
	kern_preempt_disable();
	error = i915_pcode_poll(mmio, mbox, request, reply_mask, reply, &status, I915_PCODE_FINAL_POLL_MS, 0);
	kern_preempt_enable();

out:
	mutex_unlock(sb_lock);

	/* Reports the mailbox status first, as the reference does. */
	if (status != 0)
		return status;

	/* Reports why no reply matched. */
	if (error != 0)
		return error;

	/* Succeeded: the firmware replied with the expected value. */
	return 0;
}

/* Translates a Gen7+ mailbox status into an errno (gen7_check_mailbox_status()). */
static int
i915_pcode_status(
	uint32_t mailbox)
{
	/* Picks the errno of the status field; Alder Lake-P is graphics 12. */
	switch (mailbox & I915_PCODE_ERROR_MASK) {
	case I915_PCODE_SUCCESS:
		return 0;
	case I915_PCODE_ILLEGAL_CMD:
		return ENXIO;
	case I915_PCODE_TIMEOUT:
		return ETIMEDOUT;
	case I915_PCODE_ILLEGAL_DATA:
		return EINVAL;
	case I915_PCODE_ILLEGAL_SUBCOMMAND:
		return ENXIO;
	case I915_PCODE_LOCKED:
		return EBUSY;
	case I915_PCODE_REJECTED:
		return EACCES;
	case I915_PCODE_MIN_FREQ_TABLE_GT_RATIO_OUT_OF_RANGE:
		return EOVERFLOW;
	default:
		/* A status the reference does not know counts as success. */
		return 0;
	}
}

/*
 * Runs one mailbox transaction with the sideband lock held (__snb_pcode_rw()).
 *
 * Returns 0, EAGAIN when the mailbox is still busy, the wait's ETIMEDOUT or
 * EIO, or the mailbox status as an errno.
 */
static int
i915_pcode_rw(
	struct i915_mmio *mmio,
	uint32_t mbox,
	uint32_t *val,
	uint32_t *val1,
	unsigned fast_us,
	unsigned slow_ms,
	int is_read)
{
	uint32_t mailbox;
	uint32_t second_word;
	uint32_t observed;
	int error;

	/* Refuses to post a command while the previous one is still in the mailbox. */
	mailbox = drv_i915_raw_read32(mmio, I915_PCODE_MAILBOX);
	if ((mailbox & I915_PCODE_READY) != 0U)
		return EAGAIN;

	/* The second data word is zero when the caller has none. */
	second_word = 0U;
	if (val1 != NULL)
		second_word = *val1;

	/* Loads the data words and posts the command. */
	drv_i915_raw_write32(mmio, I915_PCODE_DATA, *val);
	drv_i915_raw_write32(mmio, I915_PCODE_DATA1, second_word);
	drv_i915_raw_write32(mmio, I915_PCODE_MAILBOX, I915_PCODE_READY | mbox);

	/*
	 * Waits for the firmware to clear READY: an atomic poll, then a
	 * sleepable one, which the held mutex allows.  A timeout and a time-base
	 * failure are passed back as they are.
	 */
	observed = 0U;
	error = drv_i915_wait_reg(mmio, I915_PCODE_MAILBOX, I915_PCODE_READY, 0U, fast_us, slow_ms, &observed);
	if (error != 0)
		return error;

	/* Collects the response of a read. */
	if (is_read != 0) {
		*val = drv_i915_raw_read32(mmio, I915_PCODE_DATA);
		if (val1 != NULL)
			*val1 = drv_i915_raw_read32(mmio, I915_PCODE_DATA1);
	}

	/* Translates the final mailbox status (graphics version above 6). */
	error = i915_pcode_status(observed);
	if (error != 0)
		return error;

	/* Succeeded: the firmware completed the command. */
	return 0;
}

/* Writes one value to a PCODE mailbox command with explicit limits (snb_pcode_write_timeout()). */
static int
i915_pcode_write_timeout(
	struct mutex *sb_lock,
	struct i915_mmio *mmio,
	uint32_t mbox,
	uint32_t val,
	unsigned fast_us,
	unsigned slow_ms)
{
	uint32_t data;
	int error;

	/* Runs the write transaction under the sideband lock. */
	data = val;
	mutex_lock(sb_lock);

	error = i915_pcode_rw(mmio, mbox, &data, NULL, fast_us, slow_ms, 0);

	mutex_unlock(sb_lock);

	/* Reports a transaction that failed. */
	if (error != 0) {
		kern_logf("i915: pcode write mbox=0x%08x err=%d\n", mbox, error);
		return error;
	}

	/* Succeeded: the firmware accepted the value. */
	return 0;
}

/*
 * Sends one request and reports whether its reply matches (skl_pcode_try_request()).
 *
 * The transaction waits only in the atomic stage.  *status receives its
 * result: 0, the mailbox status as an errno, ETIMEDOUT, EAGAIN or EIO.
 */
static int
i915_pcode_try_request(
	struct i915_mmio *mmio,
	uint32_t mbox,
	uint32_t request,
	uint32_t reply_mask,
	uint32_t reply,
	int *status)
{
	uint32_t response;

	/* Sends the request and reads the reply back (__snb_pcode_rw(mbox, &request, NULL, 500, 0, true)). */
	response = request;
	*status = i915_pcode_rw(mmio, mbox, &response, NULL, I915_PCODE_FAST_US, 0U, 1);

	/* A failed transaction has no reply to compare. */
	if (*status != 0)
		return 0;

	/* A reply outside the expected value does not match. */
	if ((response & reply_mask) != reply)
		return 0;

	/* Succeeded: the reply matches. */
	return 1;
}

/*
 * Re-sends a request for a bounded time until its reply matches.
 *
 * The bound is measured on the real-time counter.  Between requests the
 * poll sleeps briefly when sleep_between is nonzero, and only stops the
 * compiler from folding the loop otherwise.  Returns 0 on a match, ETIMEDOUT,
 * or EIO when the time base failed.
 */
static int
i915_pcode_poll(
	struct i915_mmio *mmio,
	uint32_t mbox,
	uint32_t request,
	uint32_t reply_mask,
	uint32_t reply,
	int *status,
	unsigned ms,
	int sleep_between)
{
	uint64_t start;
	uint64_t frequency;
	uint64_t now;
	uint64_t now_frequency;
	uint64_t limit;
	bool counter_ok;
	int matched;

	/* Reads the start of the time bound and turns the bound into counter ticks. */
	start = 0U;
	frequency = 0U;
	counter_ok = kern_rtc_read_counter(&start, &frequency);
	if (!counter_ok)
		return EIO;
	if (frequency == 0U)
		return EIO;

	limit = ((uint64_t)ms * frequency) / 1000U;

	/* Re-sends the request until it matches, the time runs out, or the time base fails. */
	now = 0U;
	now_frequency = 0U;
	for (;;) {
		/* Stops at the first matching reply. */
		matched = i915_pcode_try_request(mmio, mbox, request, reply_mask, reply, status);
		if (matched != 0)
			return 0;

		/* A time-base failure is reported as such, not as a firmware timeout. */
		if (*status == EIO)
			return EIO;

		/* Reads the time again; a failed or changed counter is a time-base failure. */
		counter_ok = kern_rtc_read_counter(&now, &now_frequency);
		if (!counter_ok)
			return EIO;
		if (now_frequency != frequency)
			return EIO;

		/* Gives up once the bound has passed. */
		if (now - start >= limit)
			return ETIMEDOUT;

		/* Sleeps between requests only where sleeping is allowed. */
		if (sleep_between != 0) {
			kern_usleep_range(I915_PCODE_REQUEST_SLEEP_MIN_US, I915_PCODE_REQUEST_SLEEP_MAX_US);
		} else {
			kern_compiler_barrier();
		}
	}
}
