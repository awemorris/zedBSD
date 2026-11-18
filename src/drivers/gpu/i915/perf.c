/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Frame timing of the node (see perf.h).
 */

#include "perf.h"
#include <kern/kcrt.h>

#include <kern/clock.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/sched.h>

#include <stdbool.h>
#include <stdint.h>

/* How long one window lasts before its totals are logged, in nanoseconds (five seconds). */
#define I915_PERF_WINDOW_NS	5000000000ULL

/* How many nanoseconds make one second. */
#define I915_PERF_NS_PER_SECOND	1000000000ULL

static void i915_perf_ms(uint64_t total, uint32_t count, uint32_t *whole, uint32_t *hundredths);

/*
 * Prepares the timing totals of a device: an empty window.
 */
void
drv_i915_perf_init(
	struct i915_perf *perf)
{
	/* Starts with no sample and prepares the lock. */
	kern_memset(perf, 0, sizeof(*perf));
	spin_init(&perf->lock, LOCK_RANK_DEVICE, "i915 perf");
	perf->ready = 1;
}

/*
 * Reads the monotonic counter in nanoseconds; zero when there is none.
 */
uint64_t
drv_i915_perf_now(void)
{
	uint64_t counter;
	uint64_t frequency;
	uint64_t seconds;
	uint64_t rest;
	bool available;

	/* Samples the counter and its rate; without either there is no time. */
	counter = 0U;
	frequency = 0U;
	available = kern_rtc_read_counter(&counter, &frequency);
	if (!available)
		return 0U;
	if (frequency == 0U)
		return 0U;

	/* Converts in two parts, so the product never overflows. */
	seconds = counter / frequency;
	rest = counter % frequency;

	/* Succeeded: the counter in nanoseconds. */
	return seconds * I915_PERF_NS_PER_SECOND + (rest * I915_PERF_NS_PER_SECOND) / frequency;
}

/*
 * Adds the time since `start` (from drv_i915_perf_now()) to one stage.
 *
 * A zero start, or a device whose totals are not prepared, adds nothing.
 */
void
drv_i915_perf_add(
	struct i915_perf *perf,
	enum i915_perf_stage stage,
	uint64_t start)
{
	unsigned long irq;
	uint64_t now;

	/* A sample without a start, or totals not yet prepared, is dropped. */
	if (start == 0U || perf->ready == 0)
		return;

	/* Reads the end of the stage; a counter that went back drops the sample. */
	now = drv_i915_perf_now();
	if (now < start)
		return;

	/* Adds the sample; the first sample of a window starts it. */
	irq = spin_lock_irqsave(&perf->lock);

	if (perf->window_start == 0U)
		perf->window_start = start;
	perf->total[stage] += now - start;
	perf->count[stage]++;

	spin_unlock_irqrestore(&perf->lock, irq);
}

/*
 * Logs the totals of the window once it has lasted five seconds, or at
 * once when `final` is set, and starts a new window.
 *
 * The submission stages are given per submission and the presentation
 * stages per presentation, in milliseconds; the build time is the part of a
 * submission not spent waiting for its batches.
 */
void
drv_i915_perf_report(
	struct i915_perf *perf,
	int final)
{
	struct i915_perf window;
	unsigned long irq;
	uint64_t now;
	uint64_t elapsed;
	uint64_t build;
	uint32_t ms[I915_PERF_STAGES];
	uint32_t hundredths[I915_PERF_STAGES];
	uint32_t build_ms;
	uint32_t build_hundredths;
	uint32_t rate;
	uint32_t rate_hundredths;
	uint32_t per;
	unsigned stage;

	/* Totals not yet prepared have nothing to report. */
	if (perf->ready == 0)
		return;

	/* Takes the window and starts a new one once it is due. */
	now = drv_i915_perf_now();
	irq = spin_lock_irqsave(&perf->lock);

	elapsed = 0U;
	if (perf->window_start != 0U && now > perf->window_start)
		elapsed = now - perf->window_start;
	if (elapsed == 0U || (elapsed < I915_PERF_WINDOW_NS && final == 0)) {
		spin_unlock_irqrestore(&perf->lock, irq);
		return;
	}

	kern_memcpy(window.total, perf->total, sizeof(window.total));
	kern_memcpy(window.count, perf->count, sizeof(window.count));
	perf->window_start = now;
	kern_memset(perf->total, 0, sizeof(perf->total));
	kern_memset(perf->count, 0, sizeof(perf->count));

	spin_unlock_irqrestore(&perf->lock, irq);

	/* Converts every stage to milliseconds. */
	for (stage = 0U; stage < I915_PERF_STAGES; stage++) {
		/* The executor's stages are per submission, the display's per presentation, the markers per marker. */
		per = window.count[I915_PERF_PRESENT];
		if (stage < I915_PERF_PRESENT)
			per = window.count[I915_PERF_SUBMIT];
		if (stage >= I915_PERF_MARKER_WAIT)
			per = window.count[I915_PERF_MARKER_RUN];
		i915_perf_ms(window.total[stage], per, &ms[stage], &hundredths[stage]);
	}

	/* The part of a submission spent building rather than waiting for its GPU runs. */
	build = 0U;
	if (window.total[I915_PERF_SUBMIT] > window.total[I915_PERF_RUN])
		build = window.total[I915_PERF_SUBMIT] - window.total[I915_PERF_RUN];
	i915_perf_ms(build, window.count[I915_PERF_SUBMIT], &build_ms, &build_hundredths);

	/* The presentations per second, in hundredths. */
	rate = (uint32_t)(((uint64_t)window.count[I915_PERF_PRESENT] * I915_PERF_NS_PER_SECOND * 100U) / elapsed);
	rate_hundredths = rate % 100U;
	rate = rate / 100U;

	/* Says what the window came to. */
#if SCHED_WAKE_LATENCY
	sched_wake_latency_report();
#endif
	kern_logf("i915: perf: markers %u: wait %u.%02u ms, run %u.%02u ms\n",
	    window.count[I915_PERF_MARKER_RUN],
	    ms[I915_PERF_MARKER_WAIT], hundredths[I915_PERF_MARKER_WAIT],
	    ms[I915_PERF_MARKER_RUN], hundredths[I915_PERF_MARKER_RUN]);
	kern_logf("i915: perf: %u ms: %u presents (%u.%02u/s), %u submits, %u batches | per submit ms: %u.%02u "
	    "(build %u.%02u, run %u.%02u, gpu %u.%02u) | per present ms: %u.%02u (copy %u.%02u, publish %u.%02u, flip %u.%02u)%s\n",
	    (unsigned)(elapsed / 1000000U),
	    window.count[I915_PERF_PRESENT],
	    rate,
	    rate_hundredths,
	    window.count[I915_PERF_SUBMIT],
	    window.count[I915_PERF_RUN],
	    ms[I915_PERF_SUBMIT],
	    hundredths[I915_PERF_SUBMIT],
	    build_ms,
	    build_hundredths,
	    ms[I915_PERF_RUN],
	    hundredths[I915_PERF_RUN],
	    ms[I915_PERF_GPU],
	    hundredths[I915_PERF_GPU],
	    ms[I915_PERF_PRESENT],
	    hundredths[I915_PERF_PRESENT],
	    ms[I915_PERF_PRESENT_COPY],
	    hundredths[I915_PERF_PRESENT_COPY],
	    ms[I915_PERF_PRESENT_PUBLISH],
	    hundredths[I915_PERF_PRESENT_PUBLISH],
	    ms[I915_PERF_PRESENT_FLIP],
	    hundredths[I915_PERF_PRESENT_FLIP],
	    final ? " (final)" : "");
}

/* Divides a total of nanoseconds by a count into whole and hundredths of milliseconds. */
static void
i915_perf_ms(
	uint64_t total,
	uint32_t count,
	uint32_t *whole,
	uint32_t *hundredths)
{
	uint64_t per;

	/* A stage with no samples took no time. */
	if (count == 0U) {
		*whole = 0U;
		*hundredths = 0U;
		return;
	}

	/* The average in tens of microseconds, split into milliseconds and hundredths. */
	per = total / count / 10000U;
	*whole = (uint32_t)(per / 100U);
	*hundredths = (uint32_t)(per % 100U);
}
