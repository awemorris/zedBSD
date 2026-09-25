/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Time base, delays, register waits and completions (see sync.h).
 *
 * The time source is kern_rtc_read_counter(), which reports a monotonic
 * counter and its frequency.  The kernel clamps a small backwards step seen
 * across CPUs to the last value instead of disabling the source, so the
 * counter stays usable on a virtual machine with several CPUs; this file
 * therefore reads no CPU counter of its own and calibrates nothing.
 *
 * Each wait captures the counter and its frequency as one pair when it
 * starts, and refuses to go on computing elapsed time if the frequency later
 * changes or reads zero, rather than producing a meaningless duration.
 */

#include "sync.h"
#include "mmio.h"

#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/waitq.h>

#include <uapi/errno.h>
#include <stdbool.h>
#include <stddef.h>

/* How many microseconds make one second, for the tick conversion. */
#define I915_MICROSECONDS_PER_SECOND	1000000U

/* How many microseconds make one millisecond, for the slow stage budget. */
#define I915_MICROSECONDS_PER_MILLISECOND	1000U

/*
 * One register condition a wait polls for.
 *
 * It lives on the stack of drv_i915_wait_reg() for one wait and carries the
 * last value read from one stage to the next, so the caller always receives
 * the value the wait last saw.
 */
struct i915_reg_wait {
	/* The register block the register is read through. */
	struct i915_mmio *mmio;

	/* The register, the bits that matter and the value they must hold. */
	uint32_t reg;
	uint32_t mask;
	uint32_t value;

	/* The last value read from the register, or zero before any read. */
	uint32_t observed;
};

/*
 * The first reason the time base was found unusable.
 *
 * It holds one of enum i915_time_base_fault and lives for the whole kernel
 * lifetime.  It is written from any CPU and any context by an atomic
 * compare-exchange that only replaces I915_TIME_BASE_OK, so the first cause
 * is never overwritten by a later one; zero means no fault has happened.
 */
static volatile int i915_time_base_fault;

static void i915_time_base_latch(int cause);
static int i915_time_start(uint64_t *counter, uint64_t *frequency);
static int i915_time_now(uint64_t base_frequency, uint64_t *now);
static uint64_t i915_us_to_ticks(uint64_t microseconds, uint64_t frequency);
static int i915_wait_reg_fast(struct i915_reg_wait *wait, unsigned fast_us, uint64_t base, uint64_t frequency);
static int i915_wait_reg_slow(struct i915_reg_wait *wait, unsigned slow_ms);

/*
 * Reports whether a usable time base exists.
 *
 * Returns nonzero when the counter can be read and reports a nonzero
 * frequency.
 */
int
drv_i915_time_base_ok(void)
{
	uint64_t counter;
	uint64_t frequency;
	bool available;

	/* Samples the counter once to learn whether it runs. */
	counter = 0U;
	frequency = 0U;
	available = kern_rtc_read_counter(&counter, &frequency);
	if (!available)
		return 0;

	/* A counter without a rate cannot measure time. */
	if (frequency == 0U)
		return 0;

	/* Succeeded: the counter runs at a known rate. */
	return 1;
}

/*
 * Reports the first time base fault of the driver.
 *
 * Returns one of enum i915_time_base_fault; zero means the time base has
 * never failed.  The fault stays latched for the kernel lifetime.
 */
int
drv_i915_time_base_faulted(void)
{
	int cause;

	/* Reads the latch, which any CPU may have set. */
	cause = __atomic_load_n(&i915_time_base_fault, __ATOMIC_RELAXED);

	/* Succeeded: reports the latched cause, or zero. */
	return cause;
}

/*
 * Busy-waits for a number of microseconds on the monotonic counter.
 *
 * Returns 0 once the time has elapsed, or EIO when the time base fails
 * before or during the delay.  On EIO the caller must release any lock or
 * forcewake it holds and stop, never proceed as if the delay had elapsed.
 */
int
drv_i915_udelay(
	unsigned microseconds)
{
	uint64_t base;
	uint64_t frequency;
	uint64_t target;
	uint64_t now;
	int error;

	/* Captures the counter and its frequency the delay is measured against. */
	error = i915_time_start(&base, &frequency);
	if (error != 0) {
		kern_logf("i915: udelay: time-base fault latched (no counter)\n");
		return error;
	}

	/* Converts the delay into counter ticks, never fewer than asked. */
	target = i915_us_to_ticks(microseconds, frequency);

	/* Spins until the counter has advanced by the whole delay. */
	for (;;) {
		/* Reads the counter, refusing a failed read or a changed rate. */
		error = i915_time_now(frequency, &now);
		if (error != 0)
			return error;

		/* Stops once the whole delay has elapsed. */
		if (now - base >= target)
			break;

		/* Keeps the compiler from folding the counter reads together. */
		kern_compiler_barrier();
	}

	/* Succeeded: the requested time has elapsed. */
	return 0;
}

/*
 * Waits until the masked register holds the given value.
 *
 * The fast stage polls without sleeping for up to fast_us microseconds;
 * then, if slow_ms is nonzero, the slow stage polls for up to slow_ms
 * milliseconds and sleeps one scheduler tick between reads, so it may only
 * be used from a context that can sleep.  Registers are read raw: the
 * caller holds whatever forcewake the register needs.
 *
 * Returns 0 once the register matches, ETIMEDOUT when both stages expire,
 * or EIO when the time base fails.  When last is not NULL it receives the
 * last value read, whatever the outcome.
 */
int
drv_i915_wait_reg(
	struct i915_mmio *mmio,
	uint32_t reg,
	uint32_t mask,
	uint32_t value,
	unsigned fast_us,
	unsigned slow_ms,
	uint32_t *last)
{
	struct i915_reg_wait wait;
	uint64_t base;
	uint64_t frequency;
	int error;

	/* Describes the condition both stages poll for. */
	wait.mmio = mmio;
	wait.reg = reg;
	wait.mask = mask;
	wait.value = value;
	wait.observed = 0U;

	/* Captures the counter and its frequency the fast stage is measured against. */
	error = i915_time_start(&base, &frequency);
	if (error != 0) {
		/* No poll ran, so the caller receives the register as it stands. */
		if (last != NULL)
			*last = drv_i915_raw_read32(mmio, reg);

		return error;
	}

	/* Polls without sleeping, which is safe with a spinlock held or interrupts off. */
	error = ETIMEDOUT;
	if (fast_us != 0U)
		error = i915_wait_reg_fast(&wait, fast_us, base, frequency);

	/* Goes on polling with sleeps only after the fast stage expired. */
	if (error == ETIMEDOUT && slow_ms != 0U)
		error = i915_wait_reg_slow(&wait, slow_ms);

	/* Hands the last value read to the caller whatever the outcome. */
	if (last != NULL)
		*last = wait.observed;

	/* Reports an expired wait or a failed time base. */
	if (error != 0)
		return error;

	/* Succeeded: the register holds the value. */
	return 0;
}

/*
 * Prepares a completion with no permits.
 *
 * The name labels the lock and the wait queue in kernel diagnostics.
 */
void
drv_i915_completion_init(
	struct i915_completion *completion,
	const char *name)
{
	/* Prepares the lock and the queue waiters sleep on. */
	spin_init(&completion->lock, LOCK_RANK_DEVICE, name);
	waitq_init(&completion->waitq, name);

	/* Starts with no signal to consume. */
	completion->done = 0U;
}

/*
 * Signals a completion once and wakes its waiters.
 *
 * May be called from an interrupt handler.
 */
void
drv_i915_complete(
	struct i915_completion *completion)
{
	unsigned long enabled;

	/* Adds one permit, which exactly one wait will consume, and wakes the waiters. */
	enabled = spin_lock_irqsave(&completion->lock);

	completion->done++;
	waitq_wake_all(&completion->waitq);

	spin_unlock_irqrestore(&completion->lock, enabled);
}

/*
 * Discards every unconsumed signal of a completion.
 *
 * A signal delivered before the reinit is not seen by a later wait.
 */
void
drv_i915_reinit_completion(
	struct i915_completion *completion)
{
	unsigned long enabled;

	/* Forgets every permit no wait has consumed. */
	enabled = spin_lock_irqsave(&completion->lock);

	completion->done = 0U;

	spin_unlock_irqrestore(&completion->lock, enabled);
}

/*
 * Waits for a completion until an absolute scheduler-tick deadline.
 *
 * Returns 1 when a signal arrived, consuming one permit, or 0 when the
 * deadline passed first.
 */
int
drv_i915_wait_for_completion(
	struct i915_completion *completion,
	uint64_t deadline)
{
	unsigned long enabled;
	uint64_t observed;
	uint64_t now;
	int completed;

	/* Sleeps until a permit can be consumed or the deadline passes. */
	enabled = spin_lock_irqsave(&completion->lock);

	completed = 0;
	for (;;) {
		/* Consumes one permit of a signal that has arrived. */
		if (completion->done > 0U) {
			completion->done--;
			completed = 1;
			break;
		}

		/* Gives up once the deadline has passed. */
		now = sched_ticks();
		if (now >= deadline)
			break;

		/* Sleeps until a signal or the deadline; the loop re-checks both. */
		observed = waitq_sequence(&completion->waitq);
		(void)waitq_sleep(&completion->waitq, &completion->lock, observed, deadline, 0U);
	}

	spin_unlock_irqrestore(&completion->lock, enabled);

	/* The deadline passed before any signal arrived. */
	if (completed == 0)
		return 0;

	/* Succeeded: one signal was consumed. */
	return 1;
}

/* Records the first cause of a time base fault and keeps it forever. */
static void
i915_time_base_latch(
	int cause)
{
	int expected;

	/* Replaces only the no-fault value, so a later fault never hides the first. */
	expected = I915_TIME_BASE_OK;
	(void)__atomic_compare_exchange_n(&i915_time_base_fault,
					  &expected,
					  cause,
					  0,
					  __ATOMIC_RELAXED,
					  __ATOMIC_RELAXED);
}

/* Captures the counter and its frequency as one pair at the start of a wait. */
static int
i915_time_start(
	uint64_t *counter,
	uint64_t *frequency)
{
	bool available;

	/* Samples the counter and the rate it runs at. */
	*counter = 0U;
	*frequency = 0U;
	available = kern_rtc_read_counter(counter, frequency);

	/* A missing counter, or one without a rate, cannot time the wait. */
	if (!available || *frequency == 0U) {
		i915_time_base_latch(I915_TIME_BASE_NO_COUNTER);
		return EIO;
	}

	/* Succeeded: the pair is the base of the wait. */
	return 0;
}

/* Reads the counter again, requiring the frequency the wait started with. */
static int
i915_time_now(
	uint64_t base_frequency,
	uint64_t *now)
{
	uint64_t now_frequency;
	bool available;

	/* Samples the counter and the rate it now reports. */
	now_frequency = 0U;
	available = kern_rtc_read_counter(now, &now_frequency);

	/* A failed read or a changed rate makes any elapsed time meaningless. */
	if (!available || now_frequency != base_frequency) {
		i915_time_base_latch(I915_TIME_BASE_READ_FAIL);
		return EIO;
	}

	/* Succeeded: the sample is comparable with the base. */
	return 0;
}

/* Converts microseconds into counter ticks, rounded up and at least one. */
static uint64_t
i915_us_to_ticks(
	uint64_t microseconds,
	uint64_t frequency)
{
	uint64_t product;
	uint64_t ticks;

	/* An empty span or an unknown rate still waits one tick. */
	if (microseconds == 0U)
		return 1U;
	if (frequency == 0U)
		return 1U;

	/* Saturates a span whose tick count would wrap, rather than wrapping it. */
	if (microseconds > UINT64_MAX / frequency)
		return UINT64_MAX;

	/* Measures the span in microsecond-ticks. */
	product = microseconds * frequency;

	/* Rounds up by adding one tick where adding the rounding offset would wrap. */
	if (product > UINT64_MAX - (I915_MICROSECONDS_PER_SECOND - 1U))
		return product / I915_MICROSECONDS_PER_SECOND + 1U;

	/* Rounds up so the wait is never shorter than asked. */
	ticks = (product + (I915_MICROSECONDS_PER_SECOND - 1U)) / I915_MICROSECONDS_PER_SECOND;
	if (ticks == 0U)
		return 1U;

	/* Succeeded: the span in whole ticks. */
	return ticks;
}

/* Polls the register without sleeping until it matches or the stage expires. */
static int
i915_wait_reg_fast(
	struct i915_reg_wait *wait,
	unsigned fast_us,
	uint64_t base,
	uint64_t frequency)
{
	uint64_t target;
	uint64_t now;
	int error;

	/* Converts the stage budget into counter ticks, never fewer than asked. */
	target = i915_us_to_ticks(fast_us, frequency);

	/* Reads the register until it matches or the budget is spent. */
	for (;;) {
		/* Stops as soon as the register holds the value. */
		wait->observed = drv_i915_raw_read32(wait->mmio, wait->reg);
		if ((wait->observed & wait->mask) == wait->value)
			break;

		/* Reads the counter, refusing a failed read or a changed rate. */
		error = i915_time_now(frequency, &now);
		if (error != 0)
			return error;

		/* The stage expired without a match. */
		if (now - base >= target)
			return ETIMEDOUT;

		/* Keeps the compiler from folding the register reads together. */
		kern_compiler_barrier();
	}

	/* Succeeded: the register holds the value. */
	return 0;
}

/* Polls the register with one-tick sleeps until it matches or the stage expires. */
static int
i915_wait_reg_slow(
	struct i915_reg_wait *wait,
	unsigned slow_ms)
{
	struct spinlock lock;
	struct wait_queue queue;
	uint64_t base;
	uint64_t frequency;
	uint64_t target;
	uint64_t now;
	uint64_t ticks;
	uint64_t sleep_deadline;
	uint64_t observed;
	int error;
	int slept;

	/*
	 * Prepares a wait queue nobody wakes: each sleep simply lasts until its
	 * one-tick deadline.
	 */
	spin_init(&lock, LOCK_RANK_DEVICE, "i915-wait-slow");
	waitq_init(&queue, "i915-wait-slow");

	/* Captures the counter and its frequency again as one pair for this stage. */
	error = i915_time_start(&base, &frequency);
	if (error != 0)
		return error;

	/* Converts the stage budget into counter ticks, never fewer than asked. */
	target = i915_us_to_ticks((uint64_t)slow_ms * I915_MICROSECONDS_PER_MILLISECOND, frequency);

	/* Reads the register, sleeping between reads, until it matches or the budget is spent. */
	for (;;) {
		/* Stops as soon as the register holds the value. */
		wait->observed = drv_i915_raw_read32(wait->mmio, wait->reg);
		if ((wait->observed & wait->mask) == wait->value)
			break;

		/* Reads the counter, refusing a failed read or a changed rate. */
		error = i915_time_now(frequency, &now);
		if (error != 0)
			return error;

		/* The stage expired without a match. */
		if (now - base >= target)
			return ETIMEDOUT;

		/*
		 * Computes the end of a one-tick sleep.  The kernel wait's
		 * granularity is one scheduler tick; a finer sleep range would
		 * need a finer kernel timer.
		 */
		ticks = sched_ticks();
		sleep_deadline = 0U;
		error = kern_deadline_after(ticks, 1U, &sleep_deadline);
		if (error != 0) {
			i915_time_base_latch(I915_TIME_BASE_READ_FAIL);
			return EIO;
		}

		/* Sleeps for the one tick. */
		spin_lock(&lock);

		observed = waitq_sequence(&queue);
		slept = waitq_sleep(&queue, &lock, observed, sleep_deadline, 0U);

		spin_unlock(&lock);

		/*
		 * An elapsed interval (0, ETIMEDOUT) or a spurious wake (EAGAIN)
		 * only returns to the poll, where the stage's own budget decides a
		 * hardware timeout.  Anything else is an error of the wait
		 * interface: it is latched and reported as EIO so the caller
		 * stops instead of spinning or reporting a timeout.
		 */
		if (slept != 0 &&
		    slept != ETIMEDOUT &&
		    slept != EAGAIN) {
			i915_time_base_latch(I915_TIME_BASE_WAIT_API);
			return EIO;
		}
	}

	/* Succeeded: the register holds the value. */
	return 0;
}
