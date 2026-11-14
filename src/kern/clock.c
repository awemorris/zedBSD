/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Kernel time.
 *
 * The kernel counts its time in units of one millisecond: clock_ticks()
 * and every deadline are in these units, whatever rate the timer
 * interrupts come at.  Where the HAL has a counter of a fixed frequency
 * (hal_rtc_read_counter()), the units and the monotonic nanoseconds are
 * read from it each time, and the timer interrupt is only what wakes a CPU
 * to look at its deadlines.  Without one, CPU 0's interrupts count the
 * units, and the interrupt rate is never changed.
 *
 * Each CPU's interrupt rate follows what it does: the busy rate while it
 * runs threads, the idle rate while it idles and no deadline it watches is
 * near.  CPU 0 also watches the process timers and ITIMER_REAL, which only
 * its tick advances.
 *
 * The realtime clock is the monotonic time plus an offset seeded from the
 * RTC and adjusted by clock_settime(); the offset is published through a
 * sequence lock so that readers never see a torn value.  The remaining
 * functions are the timespec arithmetic shared by the timer and sleep
 * paths.
 */

#include "kern/random.h"
#include "kern/clock.h"
#include "kern/cpuperf.h"
#include "kern/atomic.h"
#include "kern/cred.h"
#include "kern/process-timer.h"
#include "kern/sched.h"
#include "kern/sched_private.h"
#include "kern/usync.h"
#include "kern/waitq.h"
#include "kern/lock.h"

#include <uapi/errno.h>
#include <hal/hal.h>
#include <limits.h>

#define KERN_REALTIME_EPOCH_2026 1767225600LL

/* The units in a second: one unit is a millisecond. */
#define CLOCK_UNIT_RATE		1000U

/* The interrupt rate a CPU asks for while it runs threads, and while it idles. */
#define CLOCK_TICK_BUSY_HZ	1000U
#define CLOCK_TICK_IDLE_HZ	100U

/*
 * How far off the nearest deadline must be for an idle CPU to slow down,
 * in units: one slow period, in which the CPU may not look, and two fast
 * ones, the first tick after it speeds up again and a tick a HAL may leave
 * armed at the old rate.
 */
#define CLOCK_TICK_IDLE_MARGIN	\
	((CLOCK_UNIT_RATE + CLOCK_TICK_IDLE_HZ - 1U) / CLOCK_TICK_IDLE_HZ + \
	 2U * ((CLOCK_UNIT_RATE + CLOCK_TICK_BUSY_HZ - 1U) / CLOCK_TICK_BUSY_HZ))

/*
 * The units in a second.  Only this file reads it: everything else asks
 * the functions below to convert.
 */
static const uint32_t unit_rate = CLOCK_UNIT_RATE;

/*
 * The units, as CPU 0's tick last saw them.  Before the counter is in use
 * CPU 0's interrupts count them, carrying the part of a unit an interrupt
 * is short of in interrupt_carry; afterwards CPU 0's tick stores the
 * counter's value.  Only CPU 0 writes either.
 */
static volatile uint64_t kernel_ticks;
static uint32_t interrupt_carry;

/*
 * The counter's value and frequency, the counts of one unit and the units,
 * at the moment CPU 0's tick took the counter up.  The base is put back to
 * a multiple of the unit's counts, where the HALs put their ticks, so that
 * a tick a little late still reads its own unit.  CPU 0 writes them once,
 * before it sets counter_mode with release; a reader that sees
 * counter_mode set with acquire reads them as written.
 */
static uint64_t base_counter;
static uint64_t base_frequency;
static uint64_t base_period;
static uint64_t base_units;
static volatile unsigned counter_mode;

/*
 * The nearest deadline of the timers CPU 0's tick advances, in units:
 * UINT64_MAX for none, 0 when one was armed since CPU 0 last looked.
 * CPU 0's tick sets it anew each time; any CPU lowers it.
 */
static volatile uint64_t global_next = UINT64_MAX;

/* Whether each CPU asked the HAL for the idle rate.  Only the CPU itself writes its entry. */
static volatile unsigned tick_lowered[HAL_CPU_MAX];

static volatile uint32_t cpu_notify_count[HAL_CPU_MAX];
static struct kern_timespec realtime_offset = {
	KERN_REALTIME_EPOCH_2026, 0
};
static int realtime_synchronized;
static volatile unsigned realtime_sequence;
static atomic_uint_t realtime_writer;

static void tick_cpu0(void);
static unsigned tick_far(uint64_t now, uint64_t next);
static bool counter_elapsed(uint64_t *elapsed, uint64_t *frequency);
static uint64_t global_next_swap(uint64_t value);
static void realtime_write_begin(void);
static void realtime_write_end(void);
static uint64_t scale_floor(uint64_t value, uint64_t multiplier, uint64_t divisor);
static uint64_t scale_ceil(uint64_t value, uint64_t multiplier, uint64_t divisor);

/*
 * Initializes the tick counter and seeds the realtime offset from the RTC.
 *
 * Without a readable RTC the realtime clock starts at the 2026 epoch and
 * stays unsynchronized until clock_settime() sets it.
 */
void
kern_clock_init(
	void)
{
	uint64_t seconds;

	/* Starts the clocks at the compiled-in epoch. */
	atomic_u64_store_release(&kernel_ticks, 0);
	atomic_raw_store_release(&realtime_sequence, 0);
	atomic_store_release(&realtime_writer, 0);
	realtime_offset.tv_sec = KERN_REALTIME_EPOCH_2026;
	realtime_offset.tv_nsec = 0;
	atomic_raw_store_release((volatile unsigned *)&realtime_synchronized, 0);
	process_timer_init();

	/* Adopts the RTC time when the platform can read it. */
	if (hal_rtc_read_epoch_time(&seconds)) {
		realtime_offset.tv_sec = (int64_t)seconds;
		atomic_raw_store_release((volatile unsigned *)&realtime_synchronized, 1);
	}
}

/*
 * Handles the periodic timer interrupt on one CPU.
 *
 * CPU 0 keeps the units and advances the process timers, noting how soon
 * they next fall due; every CPU feeds the time to its scheduler.
 */
void
kernel_timer_handler(
	hal_cpu_id_t cpu,
	hal_irq_ack_t acknowledge)
{
	uint64_t now;
	uint64_t next;

	hal_irq_send_eoi(acknowledge);

	/* CPU 0 counts the interrupt, or takes the counter up. */
	if (cpu == 0)
		tick_cpu0();

	/* The time now. */
	now = clock_ticks();

	/*
	 * CPU 0 starts the nearest deadline of its timers over, then lowers it
	 * with the process timers and, in the scheduler, ITIMER_REAL.  A timer
	 * armed meanwhile lowers it after the swap, and stays.
	 */
	if (cpu == 0) {
		(void)global_next_swap(UINT64_MAX);
		next = process_timer_tick(now);
		kern_clock_timer_due(next);
	}

	sched_clock_cpu(cpu, now);

	/* The tick's timing against the counter feeds the random pool. */
	kern_random_tick(cpu);
}

/*
 * Chooses the current CPU's interrupt rate as its idle loop halts, with
 * interrupts disabled.
 *
 * next is the nearest deadline this CPU watches, in units, or UINT64_MAX;
 * CPU 0 adds its timers' own.  The CPU slows down when that deadline is at
 * least CLOCK_TICK_IDLE_MARGIN off, and speeds up again when it is not:
 * the idle loop calls this at every slow tick, so the last slow tick
 * before a deadline brings the fast one back in time for it.  Without the
 * counter nothing changes.
 */
void
kern_clock_tick_idle(
	unsigned cpu,
	uint64_t next)
{
	uint64_t now;
	uint64_t global;
	unsigned lower;
	unsigned lowered;
	unsigned mode;
	bool confirmed;

	/* Only a counted time lets the interrupts slow down. */
	mode = atomic_raw_load_acquire(&counter_mode);
	if (cpu >= HAL_CPU_MAX)
		return;
	if (mode == 0U)
		return;

	/* The nearest deadline, CPU 0's timers included. */
	now = clock_ticks();
	global = UINT64_MAX;
	if (cpu == 0) {
		global = atomic_u64_load_acquire(&global_next);
		if (global < next)
			next = global;
	}

	/* Slows down when nothing falls due within the margin. */
	lower = tick_far(now, next);

	/* The HAL is asked only for a change. */
	lowered = atomic_raw_load_acquire(&tick_lowered[cpu]);
	if (lower == lowered)
		return;
	if (!lower) {
		(void)hal_rtc_set_tick_hz(CLOCK_TICK_BUSY_HZ);
		atomic_raw_store_release(&tick_lowered[cpu], 0U);
		return;
	}

	/* Slows this CPU down and publishes that it did. */
	(void)hal_rtc_set_tick_hz(CLOCK_TICK_IDLE_HZ);
	atomic_raw_store_release(&tick_lowered[cpu], 1U);

	/*
	 * CPU 0 confirms the deadline it slowed down for with an exchange on
	 * it, after it published the slowdown.  A timer armed meanwhile either
	 * changed the deadline, which fails the exchange and is looked at
	 * here, or comes after the exchange and sees the slowdown, and wakes
	 * this CPU if its timer is near.
	 */
	if (cpu != 0)
		return;
	confirmed = atomic_u64_compare_exchange(&global_next, &global, global);
	if (confirmed)
		return;

	/* A timer armed meanwhile may be nearer than the deadline slowed down for. */
	if (global < next)
		next = global;
	lower = tick_far(now, next);
	if (lower != 0U)
		return;

	/* The nearer deadline needs the busy rate back. */
	(void)hal_rtc_set_tick_hz(CLOCK_TICK_BUSY_HZ);
	atomic_raw_store_release(&tick_lowered[cpu], 0U);
}

/*
 * Brings the current CPU's interrupt rate back to the busy rate as it
 * leaves its idle thread for another, with interrupts disabled.
 *
 * Returns true when the CPU had slowed down, so that the scheduler can
 * account the idle time up to now.
 */
bool
kern_clock_tick_busy(
	unsigned cpu)
{
	unsigned lowered;

	/* Nothing to bring back. */
	if (cpu >= HAL_CPU_MAX)
		return false;
	lowered = atomic_raw_load_acquire(&tick_lowered[cpu]);
	if (lowered == 0U)
		return false;

	/* Succeeded: the busy rate again. */
	(void)hal_rtc_set_tick_hz(CLOCK_TICK_BUSY_HZ);
	atomic_raw_store_release(&tick_lowered[cpu], 0U);
	return true;
}

/*
 * Notes a deadline of the timers CPU 0's tick advances, in units, or
 * UINT64_MAX for none.  The nearest one counts.
 */
void
kern_clock_timer_due(
	uint64_t deadline)
{
	uint64_t seen;
	bool exchanged;

	/* Lowers the nearest deadline, never raises it. */
	seen = atomic_u64_load_acquire(&global_next);
	while (deadline < seen) {
		exchanged = atomic_u64_compare_exchange(&global_next, &seen, deadline);
		if (exchanged)
			break;
	}
}

/*
 * Notes that a timer CPU 0's tick advances was armed or moved, with its
 * deadline in ticks, or 0 when it is not known (a realtime one): then it
 * counts as due now, until CPU 0's next tick has looked at every timer
 * again.  A CPU 0 that slowed down is woken when the deadline is near; a
 * far one it sees at a later slow tick.  The caller has published the
 * timer before it calls this.
 */
void
kern_clock_timer_armed(
	uint64_t deadline)
{
	unsigned lowered;
	hal_cpu_id_t current;
	unsigned distant;
	uint64_t now;

	/* The nearest deadline, lowered with an exchange (kern_clock_tick_idle()). */
	kern_clock_timer_due(deadline);

	/* Wakes a slow CPU 0 to choose its rate again. */
	lowered = atomic_raw_load_acquire(&tick_lowered[0]);
	if (lowered == 0U)
		return;
	current = hal_cpu_current();
	if (current == 0)
		return;
	now = clock_ticks();
	distant = tick_far(now, deadline);
	if (distant == 0U)
		(void)hal_cpu_notify(0);
}

/*
 * Handles the cross-CPU notification interrupt on one CPU.
 */
void
kernel_cpu_notify_handler(
	hal_cpu_id_t cpu,
	hal_irq_ack_t acknowledge)
{
	hal_irq_send_eoi(acknowledge);

	/* Counts the notification for the delivery probe. */
	if (cpu < HAL_CPU_MAX)
		(void)atomic_raw_fetch_add_relaxed(&cpu_notify_count[cpu], 1U);

#if CONFIG_KERN_THERMAL
	/* Applies the CPU performance work posted for this CPU, if any. */
	kern_cpuperf_notify(cpu);
#endif

	sched_cpu_notify(cpu);
}

/*
 * Proves that every secondary CPU receives the notification interrupt.
 *
 * The probe sends one notification to every secondary CPU and waits, with
 * a bounded spin, for each of them to count it.
 */
int
kern_cpu_notify_probe(
	void)
{
	struct hal_cpu_mask targets;
	uint32_t before[HAL_CPU_MAX];
	hal_cpu_id_t cpu;
	unsigned timeout;

	/* Records each secondary CPU's count and targets it. */
	hal_cpu_mask_zero(&targets);
	for (cpu = 1; cpu < hal_cpu_count(); cpu++) {
		before[cpu] = atomic_raw_load_acquire(&cpu_notify_count[cpu]);
		hal_cpu_mask_set(&targets, cpu);
	}

	/* Passes trivially on a single CPU. */
	if (hal_cpu_count() <= 1)
		return HAL_OK;

	/* Sends one notification to every target. */
	if (hal_cpu_notify_mask(&targets) != HAL_OK)
		return HAL_ERR_IO;

	/* Waits until every target's count has advanced. */
	for (timeout = 0; timeout < 10000000U; timeout++) {
		for (cpu = 1; cpu < hal_cpu_count(); cpu++) {
			if (atomic_raw_load_acquire(&cpu_notify_count[cpu]) == before[cpu])
				break;
		}

		/* Reports success once no target is still pending. */
		if (cpu == hal_cpu_count())
			return HAL_OK;
		hal_compiler_barrier();
	}

	/* Reports a target that never counted the notification. */
	return HAL_ERR_TIMEOUT;
}

/*
 * Reads the monotonic time in units.
 *
 * With the counter it is read now, so that a deadline made from it is not
 * made early by a tick that has not come yet; without one it is the count
 * of CPU 0's interrupts.
 */
uint64_t
clock_ticks(
	void)
{
	uint64_t elapsed;
	uint64_t frequency;
	uint64_t ticks;
	bool counted;

	/* The counter's time since CPU 0 took it up. */
	counted = counter_elapsed(&elapsed, &frequency);
	if (counted) {
		ticks = base_units + elapsed / base_period;
		return ticks;
	}

	/* Reads the counted units with acquire ordering. */
	ticks = atomic_u64_load_acquire(&kernel_ticks);

	/* Reports the current time. */
	return ticks;
}

/*
 * Reads the monotonic time in units, rounded up to the next whole unit
 * when it is between two.
 *
 * clock_ticks() rounds down, so a deadline made from it by adding a
 * duration can fall due up to a unit early.  A wait that must not end
 * early (nanosleep(), a relative timer) adds its duration to this instead.
 * Without the counter the time between two interrupts is not known, and
 * it is the count plus one interrupt period.
 */
uint64_t
clock_ticks_ceil(
	void)
{
	uint64_t elapsed;
	uint64_t frequency;
	uint64_t ticks;
	uint32_t hz;
	bool counted;

	/* The counter's time, rounded up. */
	counted = counter_elapsed(&elapsed, &frequency);
	if (counted) {
		ticks = base_units + elapsed / base_period;
		if (elapsed % base_period != 0U)
			ticks++;
		return ticks;
	}

	/* The count, and the period no interrupt has counted yet. */
	ticks = atomic_u64_load_acquire(&kernel_ticks);
	hz = hal_rtc_get_tick_hz();
	if (hz == 0U)
		hz = 1U;
	ticks += (unit_rate + hz - 1U) / hz;

	/* Reports the time rounded up. */
	return ticks;
}

/*
 * Reads the monotonic clock in milliseconds for callback-style users.
 */
uint64_t
clock_milliseconds(
	void *context)
{
	uint64_t ticks;
	uint64_t milliseconds;

	(void)context;

	/* Scales the tick counter to milliseconds. */
	ticks = clock_ticks();
	milliseconds = kern_ticks_to_ms(ticks);

	/* Reports the elapsed milliseconds. */
	return milliseconds;
}

/*
 * Converts a time in milliseconds to ticks, rounding up.
 *
 * At 100 Hz a 2 ms recovery interval becomes one tick, 10 ms, and never
 * zero, which would not wait at all.  Zero stays zero.
 */
uint64_t
kern_ms_to_ticks(
	uint64_t milliseconds)
{
	uint64_t ticks;

	/* Rounds the time up to the ticks that cover it. */
	ticks = scale_ceil(milliseconds, unit_rate, 1000U);

	/* Reports the ticks that cover the time. */
	return ticks;
}

/*
 * Converts a time in microseconds to ticks, rounding up.
 */
uint64_t
kern_us_to_ticks(
	uint64_t microseconds)
{
	uint64_t ticks;

	/* Rounds the time up to the ticks that cover it. */
	ticks = scale_ceil(microseconds, unit_rate, 1000000U);

	/* Reports the ticks that cover the time. */
	return ticks;
}

/*
 * Converts a time in nanoseconds to ticks, rounding up.
 */
uint64_t
kern_ns_to_ticks(
	uint64_t nanoseconds)
{
	uint64_t ticks;

	/* Rounds the time up to the ticks that cover it. */
	ticks = scale_ceil(nanoseconds, unit_rate, KERN_NSEC_PER_SEC);

	/* Reports the ticks that cover the time. */
	return ticks;
}

/*
 * Converts a number of ticks to milliseconds, rounding down.
 */
uint64_t
kern_ticks_to_ms(
	uint64_t ticks)
{
	uint64_t milliseconds;

	/* Rounds the ticks down to whole milliseconds. */
	milliseconds = scale_floor(ticks, 1000U, unit_rate);

	/* Reports no more time than the ticks have counted. */
	return milliseconds;
}

/*
 * Converts a number of ticks to microseconds, rounding down.
 */
uint64_t
kern_ticks_to_us(
	uint64_t ticks)
{
	uint64_t microseconds;

	/* Rounds the ticks down to whole microseconds. */
	microseconds = scale_floor(ticks, 1000000U, unit_rate);

	/* Reports no more time than the ticks have counted. */
	return microseconds;
}

/*
 * Converts a number of ticks to nanoseconds, rounding down.
 */
uint64_t
kern_ticks_to_ns(
	uint64_t ticks)
{
	uint64_t nanoseconds;

	/* Rounds the ticks down to whole nanoseconds. */
	nanoseconds = scale_floor(ticks, KERN_NSEC_PER_SEC, unit_rate);

	/* Reports no more time than the ticks have counted. */
	return nanoseconds;
}

/*
 * Converts a number of ticks to a count at another fixed rate, rounding down.
 *
 * This is how the kernel hands a CPU time to a user ABI whose unit is fixed
 * (times(2), the process list), whatever the tick is on this machine.  The
 * rate is at most one count per nanosecond.
 */
uint64_t
kern_ticks_to_rate(
	uint64_t ticks,
	uint64_t rate)
{
	uint64_t count;

	/* Rounds the ticks down to whole counts at the other rate. */
	count = scale_floor(ticks, rate, unit_rate);

	/* Reports the whole counts the ticks have reached. */
	return count;
}

/*
 * Converts a number of ticks to a timespec, rounding down.
 */
void
kern_ticks_to_timespec(
	uint64_t ticks,
	struct timespec *result)
{
	uint64_t fraction;

	/* Whole seconds apart from the rest, so the product cannot overflow. */
	fraction = ticks % unit_rate;
	result->tv_sec = (time_t)(ticks / unit_rate);
	result->tv_nsec = (long)(fraction * KERN_NSEC_PER_SEC / unit_rate);
}

/*
 * Reports how long a deadline may wait past its time before a busy CPU
 * notices it, in nanoseconds, rounded up: the longer of one tick and the
 * calling CPU's timer interrupt period.  A caller asks while it runs, at
 * the busy rate.  It is asked for each time and never kept as a constant.
 */
uint64_t
kern_clock_resolution_ns(
	void)
{
	uint64_t unit;
	uint64_t period;
	uint32_t hz;

	/* One tick, rounded up so it is never shorter than it is. */
	unit = (KERN_NSEC_PER_SEC + unit_rate - 1U) / unit_rate;

	/* The interrupt period, when it is longer. */
	hz = hal_rtc_get_tick_hz();
	if (hz == 0U)
		return unit;
	period = (KERN_NSEC_PER_SEC + hz - 1U) / hz;
	if (period > unit)
		return period;

	/* Reports one tick. */
	return unit;
}

/*
 * Reports the units in a second.
 *
 * Only a report whose ABI states its unit as a rate (hw.cputimes), or
 * arithmetic that must stay in whole units, asks for it; a time is
 * converted with the functions above instead.  It is not the timer
 * interrupt's rate, which changes as the CPUs idle.  A caller asks each
 * time it needs it and never keeps it as a constant.
 */
uint32_t
kern_clock_tick_rate(
	void)
{
	/* Reports the rate the tick counter advances at. */
	return unit_rate;
}

/*
 * Validates a user-supplied timespec.
 */
int
kern_timespec_validate(
	const struct timespec *value)
{
	/* Rejects a missing, negative, or denormalized value. */
	if (value == NULL ||
	    value->tv_sec < 0 ||
	    value->tv_nsec < 0 ||
	    (uint64_t)value->tv_nsec >= KERN_NSEC_PER_SEC)
		return EINVAL;

	/* Reports a valid timespec. */
	return 0;
}

/*
 * Normalizes a timespec so its nanoseconds lie in [0, 1s).
 */
int
kern_timespec_normalize(
	struct kern_timespec *value)
{
	int64_t carry;

	/* Rejects a missing value. */
	if (value == NULL)
		return EINVAL;

	/* Moves whole seconds out of the nanosecond field. */
	carry = value->tv_nsec / (int64_t)KERN_NSEC_PER_SEC;
	value->tv_nsec %= (int32_t)KERN_NSEC_PER_SEC;
	if (value->tv_nsec < 0) {
		value->tv_nsec += (int32_t)KERN_NSEC_PER_SEC;
		carry--;
	}

	/* Rejects a carry that would overflow the seconds. */
	if ((carry > 0 && value->tv_sec > INT64_MAX - carry) ||
	    (carry < 0 && value->tv_sec < INT64_MIN - carry))
		return EOVERFLOW;

	value->tv_sec += carry;

	/* Reports the normalized value. */
	return 0;
}

/*
 * Adds two timespecs with overflow detection.
 */
int
kern_timespec_add(
	const struct kern_timespec *a,
	const struct kern_timespec *b,
	struct kern_timespec *result)
{
	int error;

	/* Rejects a missing operand or result. */
	if (a == NULL || b == NULL || result == NULL)
		return EINVAL;

	/* Rejects a seconds sum that would overflow. */
	if ((b->tv_sec > 0 && a->tv_sec > INT64_MAX - b->tv_sec) ||
	    (b->tv_sec < 0 && a->tv_sec < INT64_MIN - b->tv_sec))
		return EOVERFLOW;

	/* Adds the fields and normalizes the nanosecond carry. */
	result->tv_sec = a->tv_sec + b->tv_sec;
	result->tv_nsec = a->tv_nsec + b->tv_nsec;

	/* Reports why the normalization failed. */
	error = kern_timespec_normalize(result);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Subtracts one timespec from another with overflow detection.
 */
int
kern_timespec_sub(
	const struct kern_timespec *a,
	const struct kern_timespec *b,
	struct kern_timespec *result)
{
	struct kern_timespec negated;
	int error;

	/* Rejects a missing operand or result. */
	if (a == NULL || b == NULL || result == NULL)
		return EINVAL;

	/* Rejects the one subtrahend that cannot be negated. */
	if (b->tv_sec == INT64_MIN)
		return EOVERFLOW;

	/* Subtracts by adding the negated subtrahend. */
	negated.tv_sec = -b->tv_sec;
	negated.tv_nsec = -b->tv_nsec;

	/* Reports why the addition failed. */
	error = kern_timespec_add(a, &negated, result);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Orders two normalized timespecs.
 */
int
kern_timespec_compare(
	const struct kern_timespec *a,
	const struct kern_timespec *b)
{
	/* Orders by whole seconds first. */
	if (a->tv_sec < b->tv_sec)
		return -1;
	if (a->tv_sec > b->tv_sec)
		return 1;

	/* Orders equal seconds by nanoseconds. */
	if (a->tv_nsec < b->tv_nsec)
		return -1;
	if (a->tv_nsec > b->tv_nsec)
		return 1;

	/* Reports equal times. */
	return 0;
}

/*
 * Converts a duration to ticks, rounding up.
 */
int
kern_duration_to_ticks_ceil(
	const struct timespec *duration,
	uint64_t *ticks)
{
	uint64_t seconds;
	uint64_t fraction;
	int error;

	/* Rejects an invalid duration or a missing result. */
	error = kern_timespec_validate(duration);
	if (error != 0)
		return error;
	if (ticks == NULL)
		return EINVAL;

	/* Rejects whole seconds that would overflow the tick count. */
	seconds = (uint64_t)duration->tv_sec;
	if (seconds > UINT64_MAX / unit_rate)
		return EOVERFLOW;

	/* Rounds the nanosecond fraction up to whole ticks. */
	fraction = ((uint64_t)duration->tv_nsec * unit_rate +
	    KERN_NSEC_PER_SEC - 1U) / KERN_NSEC_PER_SEC;

	/* Rejects a total that would overflow the tick count. */
	if (seconds * unit_rate > UINT64_MAX - fraction)
		return EOVERFLOW;

	/* Converts the whole seconds and the rounded fraction into ticks. */
	*ticks = seconds * unit_rate + fraction;

	/* Reports the converted tick count. */
	return 0;
}

/*
 * Computes a deadline a fixed number of ticks after a moment.
 */
int
kern_deadline_after(
	uint64_t now,
	uint64_t delta,
	uint64_t *deadline)
{
	/* Rejects a missing result. */
	if (deadline == NULL)
		return EINVAL;

	/* Rejects a deadline beyond the tick range. */
	if (now > UINT64_MAX - delta)
		return EOVERFLOW;

	*deadline = now + delta;

	/* Reports the computed deadline. */
	return 0;
}

/*
 * Reports the ticks that remain until a deadline, or zero once it passed.
 */
uint64_t
kern_deadline_remaining(
	uint64_t now,
	uint64_t deadline)
{
	/* Reports an expired deadline as no remaining time. */
	if (now >= deadline)
		return 0;

	/* Reports the remaining ticks. */
	return deadline - now;
}

/*
 * Reads the monotonic or realtime clock.
 */
int
kern_clock_gettime(
	clockid_t clock,
	struct timespec *result)
{
	struct kern_timespec monotonic;
	struct kern_timespec value;
	struct kern_timespec offset;
	struct timespec uptime;
	uint64_t elapsed;
	uint64_t frequency;
	uint64_t nanoseconds;
	uint64_t ticks;
	unsigned before;
	unsigned after;
	bool counted;
	int error;

	/* Rejects a missing result or an unsupported clock. */
	if (result == NULL)
		return EINVAL;
	if (clock != CLOCK_MONOTONIC && clock != CLOCK_REALTIME)
		return EINVAL;

	/* The monotonic time from the counter, to its own step; without one, from the units. */
	counted = counter_elapsed(&elapsed, &frequency);
	if (counted) {
		nanoseconds = base_units * (KERN_NSEC_PER_SEC / unit_rate) +
		    scale_floor(elapsed, KERN_NSEC_PER_SEC, frequency);
		uptime.tv_sec = (time_t)(nanoseconds / KERN_NSEC_PER_SEC);
		uptime.tv_nsec = (long)(nanoseconds % KERN_NSEC_PER_SEC);
	} else {
		ticks = clock_ticks();
		kern_ticks_to_timespec(ticks, &uptime);
	}
	monotonic.tv_sec = (int64_t)uptime.tv_sec;
	monotonic.tv_nsec = (int32_t)uptime.tv_nsec;
	value = monotonic;

	/* Adds the realtime offset read consistently under the sequence lock. */
	if (clock == CLOCK_REALTIME) {
		for (;;) {
			before = atomic_raw_load_acquire(&realtime_sequence);
			if (before & 1U)
				continue;
			offset = realtime_offset;
			after = atomic_raw_load_acquire(&realtime_sequence);
			if (before == after)
				break;
		}

		error = kern_timespec_add(&monotonic, &offset, &value);
		if (error != 0)
			return error;
	}

	/* Publishes the time in the public representation. */
	result->tv_sec = (time_t)value.tv_sec;
	result->tv_nsec = (long)value.tv_nsec;

	/* Reports the time. */
	return 0;
}

/*
 * Reports the resolution of the monotonic and realtime clocks: one step of
 * the counter, rounded up, or without one the step of the units.
 */
int
kern_clock_getres(
	clockid_t clock,
	struct timespec *result)
{
	uint64_t step;
	unsigned mode;

	/* Rejects an unsupported clock. */
	if (clock != CLOCK_MONOTONIC && clock != CLOCK_REALTIME)
		return EINVAL;

	/* The step the clocks are read to. */
	mode = atomic_raw_load_acquire(&counter_mode);
	if (mode != 0U)
		step = (KERN_NSEC_PER_SEC + base_frequency - 1U) / base_frequency;
	else
		step = kern_clock_resolution_ns();

	/* Reports the step when a result is wanted. */
	if (result != NULL) {
		result->tv_sec = (time_t)(step / KERN_NSEC_PER_SEC);
		result->tv_nsec = (long)(step % KERN_NSEC_PER_SEC);
	}

	/* Reports a supported clock. */
	return 0;
}

/*
 * Sets the realtime clock.
 *
 * Only the superuser may set it.  The new offset is published under the
 * sequence lock and marks the clock synchronized.
 */
int
kern_clock_settime(
	clockid_t clock,
	const struct timespec *requested,
	const struct ucred *cred)
{
	struct timespec now;
	struct kern_timespec target;
	struct kern_timespec monotonic;
	int error;

	/* Rejects any clock but realtime. */
	if (clock != CLOCK_REALTIME)
		return EINVAL;

	/* Rejects an unprivileged caller. */
	if (!cred_is_superuser(cred))
		return EPERM;

	/* Rejects an invalid time. */
	error = kern_timespec_validate(requested);
	if (error != 0)
		return error;

	/* Reads the monotonic time the offset is relative to. */
	error = kern_clock_gettime(CLOCK_MONOTONIC, &now);
	if (error != 0)
		return error;

	/* Converts both times to the kernel representation. */
	target.tv_sec = requested->tv_sec;
	target.tv_nsec = (int32_t)requested->tv_nsec;
	monotonic.tv_sec = now.tv_sec;
	monotonic.tv_nsec = (int32_t)now.tv_nsec;

	/* Publishes the new offset under the sequence lock. */
	realtime_write_begin();
	error = kern_timespec_sub(&target, &monotonic, &realtime_offset);
	if (error == 0)
		atomic_raw_store_release((volatile unsigned *)&realtime_synchronized, 1);
	realtime_write_end();

	/* Wakes absolute-time waiters, and has CPU 0 look at its realtime timers again, after a successful change. */
	if (error == 0) {
		usync_realtime_changed();
		kern_clock_timer_armed(0U);
	}

	/* Reports why the change failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Tests whether the realtime clock was set from the RTC or by a caller.
 */
int
kern_clock_realtime_synchronized(
	void)
{
	unsigned synchronized;

	/* Reads the synchronization flag. */
	synchronized = atomic_raw_load_acquire((volatile unsigned *)&realtime_synchronized);

	/* Reports whether the clock is synchronized. */
	return synchronized != 0;
}

/*
 * Reads the realtime clock into separate second and nanosecond fields.
 */
void
clock_realtime(
	time_t *seconds,
	long *nanoseconds)
{
	struct timespec now;

	/* Reads the realtime clock, which cannot fail for a valid result. */
	(void)kern_clock_gettime(CLOCK_REALTIME, &now);

	/* Publishes the fields the caller asked for. */
	if (seconds != NULL)
		*seconds = now.tv_sec;
	if (nanoseconds != NULL)
		*nanoseconds = now.tv_nsec;
}

/*
 * Sleeps for at least min_us microseconds, yielding the CPU.
 *
 * The earliest return is computed once from the monotonic counter and a
 * wake never extends it.  Until it is reached the thread sleeps on a
 * private wait queue for the time that is left, and re-reads the counter
 * when it wakes: a deadline counts from the last tick, so a wake may come
 * up to a tick early.  The wake is at tick granularity
 * (kern_clock_resolution_ns()) and may be later than max_us, which is
 * advisory; callers re-check their condition after the sleep.  The wait
 * queue's token is removed on return, so no timer state outlives the call.
 */
void
kern_usleep_range(
	unsigned min_us,
	unsigned max_us)
{
	struct wait_queue queue;
	struct spinlock lock;
	unsigned long enabled;
	uint64_t base;
	uint64_t frequency;
	uint64_t now;
	uint64_t now_frequency;
	uint64_t earliest;
	uint64_t left_us;
	uint64_t sequence;
	uint64_t deadline;
	bool counted;

	/* The upper bound is advisory: the tick decides when the thread wakes. */
	(void)max_us;

	/* Reads the counter the earliest return is measured on; without one there is nothing to wait on. */
	base = 0U;
	frequency = 0U;
	counted = kern_rtc_read_counter(&base, &frequency);
	if (!counted)
		return;
	if (frequency == 0U)
		return;

	/* The counter value at which min_us has passed. */
	earliest = base + ((uint64_t)min_us * frequency) / 1000000U;

	/* Prepares the private wait queue the thread sleeps on. */
	waitq_init(&queue, "usleep_range");
	spin_init(&lock, LOCK_RANK_USYNC, "usleep_range");

	/* Sleeps for what is left until the counter passes the earliest return. */
	for (;;) {
		/* A counter that fails or changes its rate ends the wait rather than spinning on it. */
		now = 0U;
		now_frequency = 0U;
		counted = kern_rtc_read_counter(&now, &now_frequency);
		if (!counted)
			return;
		if (now_frequency != frequency)
			return;

		/* Returns once min_us has passed. */
		if ((int64_t)(now - earliest) >= 0)
			return;

		/* The time left, rounded up to a whole microsecond. */
		left_us = scale_ceil(earliest - now, 1000000U, frequency);

		/*
		 * Sleeps for the time left.  The lock is taken with interrupts
		 * disabled, as the locked sleep requires: the spinlock does not
		 * hold off preemption, and a thread preempted between the lock
		 * and the sleep may resume on another CPU, whose release traps.
		 */
		enabled = spin_lock_irqsave(&lock);

		sequence = waitq_sequence(&queue);
		deadline = sched_ticks() + kern_us_to_ticks(left_us);
		(void)waitq_sleep(&queue, &lock, sequence, deadline, 0U);

		spin_unlock_irqrestore(&lock, enabled);
	}
}

/*
 * Advances the units at CPU 0's interrupt.
 *
 * The first interrupt at which the counter reads takes it up: the units
 * counted so far become the base the counter's time is added to, so the
 * time goes on from where the count left it.  From then on the units are
 * the counter's, which this stores for a reader the counter fails.  Before
 * then, and whenever the counter fails, each interrupt adds the units of
 * one period at the current rate, carrying the rest.
 */
static void
tick_cpu0(
	void)
{
	uint64_t counter;
	uint64_t frequency;
	uint64_t elapsed;
	uint64_t units;
	uint32_t hz;
	unsigned mode;
	bool counted;

	/* The counter's units, once taken up. */
	mode = atomic_raw_load_acquire(&counter_mode);
	if (mode != 0U) {
		counted = counter_elapsed(&elapsed, &frequency);
		if (counted) {
			atomic_u64_store_release(&kernel_ticks,
			    base_units + elapsed / base_period);
			return;
		}
	}

	/* Takes the counter up when it reads, at the unit's multiple before now. */
	mode = atomic_raw_load_acquire(&counter_mode);
	if (mode == 0U) {
		counted = hal_rtc_read_counter(&counter, &frequency);
		if (counted && frequency != 0U) {
			base_period = frequency / unit_rate;
			if (base_period == 0U)
				base_period = 1U;
			base_counter = counter - counter % base_period;
			base_frequency = frequency;
			base_units = atomic_u64_load_acquire(&kernel_ticks);
			atomic_raw_store_release(&counter_mode, 1U);
			return;
		}
	}

	/* One period of units, at least one interrupt a second assumed. */
	hz = hal_rtc_get_tick_hz();
	if (hz == 0U)
		hz = 1U;
	interrupt_carry += unit_rate;
	units = interrupt_carry / hz;
	interrupt_carry %= hz;
	(void)atomic_u64_fetch_add_relaxed(&kernel_ticks, units);
}

/*
 * Reads the counter's time since CPU 0 took it up, in counts, and its
 * frequency.  Returns false before then, and when the counter does not
 * read or reads at another frequency than the one it was taken up at.
 */
static bool
counter_elapsed(
	uint64_t *elapsed,
	uint64_t *frequency)
{
	uint64_t counter;
	uint64_t rate;
	unsigned mode;
	bool counted;

	/* Not taken up yet. */
	mode = atomic_raw_load_acquire(&counter_mode);
	if (mode == 0U)
		return false;

	/* The counter now, at the frequency it was taken up at. */
	counted = hal_rtc_read_counter(&counter, &rate);
	if (!counted)
		return false;
	if (rate != base_frequency)
		return false;

	/* Succeeded: the counts since the base. */
	*elapsed = counter - base_counter;
	*frequency = rate;
	return true;
}

/* Reports 1 when a deadline is at least the margin off, so that a CPU may slow down. */
static unsigned
tick_far(
	uint64_t now,
	uint64_t next)
{
	/* None at all. */
	if (next == UINT64_MAX)
		return 1U;

	/* Due, overdue, or near. */
	if (next <= now || next - now < CLOCK_TICK_IDLE_MARGIN)
		return 0U;

	/* Succeeded: far enough. */
	return 1U;
}

/* Replaces the nearest deadline of CPU 0's timers, reporting the one it had. */
static uint64_t
global_next_swap(
	uint64_t value)
{
	uint64_t seen;
	bool exchanged;

	/* An exchange, so that a deadline lowered meanwhile is ordered against it. */
	seen = atomic_u64_load_acquire(&global_next);
	exchanged = atomic_u64_compare_exchange(&global_next, &seen, value);
	while (!exchanged) {
		hal_compiler_barrier();
		exchanged = atomic_u64_compare_exchange(&global_next, &seen, value);
	}

	/* Succeeded: the deadline replaced. */
	return seen;
}

/* Enters the realtime offset write side of the sequence lock. */
static void
realtime_write_begin(
	void)
{
	/* Waits for exclusive writer ownership. */
	while (!atomic_try_acquire_zero(&realtime_writer))
		hal_compiler_barrier();

	/* Marks the offset as being written. */
	(void)atomic_raw_fetch_add_relaxed(&realtime_sequence, 1U);
}

/* Leaves the realtime offset write side of the sequence lock. */
static void
realtime_write_end(
	void)
{
	/* Marks the offset as stable again and releases the writer. */
	(void)atomic_raw_fetch_add_relaxed(&realtime_sequence, 1U);
	atomic_store_release(&realtime_writer, 0);
}

/*
 * Scales a count by multiplier / divisor, rounding down and saturating.
 *
 * Whole divisors are taken apart from the rest so that the product of the
 * rest cannot overflow; both factors are at most a billion.
 */
static uint64_t
scale_floor(
	uint64_t value,
	uint64_t multiplier,
	uint64_t divisor)
{
	uint64_t whole;
	uint64_t part;

	/* A count of whole divisors too large to scale saturates. */
	whole = value / divisor;
	if (multiplier != 0U && whole > UINT64_MAX / multiplier)
		return UINT64_MAX;

	/* The rest, scaled on its own; the sum saturates as well. */
	part = value % divisor * multiplier / divisor;
	if (whole * multiplier > UINT64_MAX - part)
		return UINT64_MAX;

	/* Succeeded: the scaled count. */
	return whole * multiplier + part;
}

/*
 * Scales a count by multiplier / divisor, rounding up and saturating.
 */
static uint64_t
scale_ceil(
	uint64_t value,
	uint64_t multiplier,
	uint64_t divisor)
{
	uint64_t whole;
	uint64_t part;

	/* A count of whole divisors too large to scale saturates. */
	whole = value / divisor;
	if (multiplier != 0U && whole > UINT64_MAX / multiplier)
		return UINT64_MAX;

	/* The rest, scaled on its own and rounded up; the sum saturates as well. */
	part = (value % divisor * multiplier + divisor - 1U) / divisor;
	if (whole * multiplier > UINT64_MAX - part)
		return UINT64_MAX;

	/* Succeeded: the scaled count. */
	return whole * multiplier + part;
}
