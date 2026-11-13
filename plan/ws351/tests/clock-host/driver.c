/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host test of the kernel clock (src/kern/clock.c, compiled freestanding
 * and linked here) against a fake HAL counter, tick rate and scheduler:
 *  - without a counter, CPU 0's interrupts count the units at the HAL's
 *    rate, carrying a part of a unit;
 *  - the first CPU 0 tick at which the counter reads takes it up, at the
 *    unit's multiple before it, and the time goes on from the counted
 *    units without a step back;
 *  - clock_ticks(), clock_ticks_ceil(), the monotonic nanoseconds and the
 *    resolutions;
 *  - a counter that changes its frequency is not used;
 *  - the idle rate: slowed down only when the nearest deadline is at least
 *    the margin off, the HAL asked only on a change, CPU 0's own timers
 *    counted, an armed timer waking a slow CPU 0, the busy rate back;
 *  - nothing changes the rate without the counter.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* The kernel's clock, as clock.h declares it. */
void kern_clock_init(void);
void kernel_timer_handler(uint32_t cpu, uintptr_t acknowledge);
uint64_t clock_ticks(void);
uint64_t clock_ticks_ceil(void);
uint64_t kern_ms_to_ticks(uint64_t milliseconds);
uint64_t kern_ticks_to_ms(uint64_t ticks);
uint64_t kern_clock_resolution_ns(void);
uint32_t kern_clock_tick_rate(void);
int kern_clock_gettime(int clock, struct timespec *result);
int kern_clock_getres(int clock, struct timespec *result);
void kern_clock_tick_idle(unsigned cpu, uint64_t next);
bool kern_clock_tick_busy(unsigned cpu);
void kern_clock_timer_due(uint64_t deadline);
void kern_clock_timer_armed(uint64_t deadline);

/* zedBSD's clock numbers (uapi/time.h). */
#define ZB_CLOCK_MONOTONIC 1
#define ZB_CLOCK_REALTIME 2

/* The fake HAL. */
static bool counter_on;
static uint64_t counter_value;
static uint64_t counter_frequency;
static uint32_t tick_hz[4] = { 1000U, 1000U, 1000U, 1000U };
static uint32_t current_cpu;
static unsigned set_calls;
static uint32_t set_last;
static unsigned notify_calls;
static uint32_t notify_last;

/* What the fake process timers and scheduler report at CPU 0's tick. */
static uint64_t process_next = UINT64_MAX;
static uint64_t itimer_next = UINT64_MAX;
static unsigned sched_calls;

static unsigned failures;
static unsigned checks;

#define CHECK(condition, ...)						\
	do {								\
		checks++;						\
		if (!(condition)) {					\
			failures++;					\
			printf("FAIL %s:%d: ", __FILE__, __LINE__);	\
			printf(__VA_ARGS__);				\
			printf("\n");					\
		}							\
	} while (0)

bool
hal_rtc_read_counter(uint64_t *value, uint64_t *frequency)
{
	if (!counter_on)
		return false;
	*value = counter_value;
	*frequency = counter_frequency;
	return true;
}

bool
kern_rtc_read_counter(uint64_t *value, uint64_t *frequency)
{
	return hal_rtc_read_counter(value, frequency);
}

bool
hal_rtc_read_epoch_time(uint64_t *seconds)
{
	(void)seconds;
	return false;
}

uint32_t
hal_rtc_get_tick_hz(void)
{
	return tick_hz[current_cpu];
}

uint32_t
hal_rtc_set_tick_hz(uint32_t hz)
{
	set_calls++;
	set_last = hz;
	tick_hz[current_cpu] = hz;
	return hz;
}

uint32_t hal_cpu_current(void) { return current_cpu; }
unsigned hal_cpu_count(void) { return 4U; }
int hal_cpu_notify(uint32_t cpu) { notify_calls++; notify_last = cpu; return 0; }
int hal_cpu_notify_mask(const void *mask) { (void)mask; return 0; }
void hal_irq_send_eoi(uintptr_t acknowledge) { (void)acknowledge; }

/* The kernel around the clock. */
uint64_t process_timer_tick(uint64_t now) { (void)now; return process_next; }
void process_timer_init(void) { }
void sched_clock_cpu(uint32_t cpu, uint64_t now)
{
	(void)now;
	sched_calls++;
	if (cpu == 0)
		kern_clock_timer_due(itimer_next);
}
void sched_cpu_notify(uint32_t cpu) { (void)cpu; }
uint64_t sched_ticks(void) { return clock_ticks(); }
void kern_random_tick(uint32_t cpu) { (void)cpu; }
void usync_realtime_changed(void) { }
int cred_is_superuser(const void *cred) { (void)cred; return 1; }
void spin_init(void *lock, int rank, const char *name) { (void)lock; (void)rank; (void)name; }
unsigned long spin_lock_irqsave(void *lock) { (void)lock; return 0; }
void spin_unlock_irqrestore(void *lock, unsigned long irq) { (void)lock; (void)irq; }
void waitq_init(void *queue, const char *name) { (void)queue; (void)name; }
uint64_t waitq_sequence(void *queue) { (void)queue; return 0; }
int waitq_sleep(void *queue, void *lock, uint64_t sequence, uint64_t deadline, unsigned flags)
{
	(void)queue; (void)lock; (void)sequence; (void)deadline; (void)flags;
	return 0;
}

/* Takes one interrupt on a CPU. */
static void
tick(uint32_t cpu)
{
	current_cpu = cpu;
	kernel_timer_handler(cpu, 0);
}

static void
test_interrupt_mode(void)
{
	struct timespec res;
	unsigned i;
	unsigned before;

	/* The units are milliseconds whatever the interrupt rate. */
	CHECK(kern_clock_tick_rate() == 1000U, "unit rate %u", kern_clock_tick_rate());
	CHECK(kern_ms_to_ticks(5U) == 5U, "5 ms");
	CHECK(kern_ticks_to_ms(7U) == 7U, "7 ticks");

	/* 100 Hz: ten units an interrupt. */
	tick_hz[0] = 100U;
	for (i = 0; i < 5U; i++)
		tick(0);
	CHECK(clock_ticks() == 50U, "100 Hz: 5 interrupts give %llu", (unsigned long long)clock_ticks());
	CHECK(clock_ticks_ceil() == 60U, "100 Hz: rounded up %llu", (unsigned long long)clock_ticks_ceil());
	CHECK(kern_clock_resolution_ns() == 10000000U, "100 Hz resolution %llu", (unsigned long long)kern_clock_resolution_ns());
	(void)kern_clock_getres(ZB_CLOCK_MONOTONIC, &res);
	CHECK(res.tv_sec == 0 && res.tv_nsec == 10000000L, "100 Hz getres %ld", res.tv_nsec);

	/* 300 Hz: 3.33 units an interrupt, the part carried. */
	tick_hz[0] = 300U;
	for (i = 0; i < 3U; i++)
		tick(0);
	CHECK(clock_ticks() == 60U, "300 Hz: 3 interrupts give %llu", (unsigned long long)(clock_ticks()));

	/* No counter: the idle loop changes no rate. */
	tick_hz[0] = 1000U;
	before = set_calls;
	current_cpu = 1;
	kern_clock_tick_idle(1, UINT64_MAX);
	CHECK(set_calls == before, "rate changed without a counter");
	CHECK(!kern_clock_tick_busy(1), "busy without a slowdown");
}

static void
test_counter_mode(void)
{
	struct timespec now;
	struct timespec res;
	uint64_t base;
	uint64_t ticks;
	uint64_t ns;

	/* The first tick with a counter takes it up from the units counted. */
	base = clock_ticks();
	counter_on = true;
	counter_frequency = 24000000U;
	counter_value = 1000000000U;
	tick(0);
	CHECK(clock_ticks() == base, "taken up at %llu, was %llu", (unsigned long long)clock_ticks(), (unsigned long long)base);

	/*
	 * Read now, between interrupts.  The base went back to the multiple of
	 * the unit's 24000 counts before it (1000000000 is 16000 past one), where
	 * the HAL's ticks are: 16000 + 7 * 24000 + 7999 counts are 7.99 ms.
	 */
	counter_value += 24000U * 7U + 7999U;
	CHECK(clock_ticks() == base + 7U, "7.99 ms later %llu", (unsigned long long)(clock_ticks() - base));
	CHECK(clock_ticks_ceil() == base + 8U, "7.99 ms later rounded up %llu", (unsigned long long)(clock_ticks_ceil() - base));
	(void)kern_clock_gettime(ZB_CLOCK_MONOTONIC, &now);
	ns = (uint64_t)now.tv_sec * 1000000000U + (uint64_t)now.tv_nsec;
	CHECK(ns == base * 1000000U + 7999958U, "monotonic ns %llu", (unsigned long long)(ns - base * 1000000U));
	CHECK(ns / 1000000U == clock_ticks(), "ns and ticks agree");

	/* The resolutions: one unit, and the counter's step. */
	CHECK(kern_clock_resolution_ns() == 1000000U, "counter resolution %llu", (unsigned long long)kern_clock_resolution_ns());
	(void)kern_clock_getres(ZB_CLOCK_MONOTONIC, &res);
	CHECK(res.tv_sec == 0 && res.tv_nsec == 42L, "counter getres %ld", res.tv_nsec);

	/* Seconds on, the nanoseconds carry into seconds. */
	counter_value += 24000000U * 3U;
	(void)kern_clock_gettime(ZB_CLOCK_MONOTONIC, &now);
	CHECK(now.tv_nsec < 1000000000L, "normalized");
	CHECK((uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U == clock_ticks(), "seconds agree");

	/* A counter at another frequency is not used: the last tick's value. */
	tick(0);
	ticks = clock_ticks();
	counter_frequency = 25000000U;
	counter_value += 24000000U;
	CHECK(clock_ticks() == ticks, "changed frequency gives %llu, tick had %llu", (unsigned long long)clock_ticks(), (unsigned long long)ticks);
	counter_frequency = 24000000U;
	counter_value -= 24000000U;
}

static void
test_idle_rate(void)
{
	uint64_t now;
	unsigned before;

	/* CPU 1 with nothing to wait for slows down, once. */
	current_cpu = 1;
	before = set_calls;
	kern_clock_tick_idle(1, UINT64_MAX);
	CHECK(set_calls == before + 1U && set_last == 100U, "idle: %u calls, last %u", set_calls - before, set_last);
	kern_clock_tick_idle(1, UINT64_MAX);
	CHECK(set_calls == before + 1U, "idle again: %u calls", set_calls - before);

	/* A deadline within the margin brings the fast rate back; one at the margin lets it slow. */
	now = clock_ticks();
	kern_clock_tick_idle(1, now + 11U);
	CHECK(set_last == 1000U && tick_hz[1] == 1000U, "11 ms off: %u Hz", tick_hz[1]);
	kern_clock_tick_idle(1, now + 12U);
	CHECK(tick_hz[1] == 100U, "12 ms off: %u Hz", tick_hz[1]);
	kern_clock_tick_idle(1, now);
	CHECK(tick_hz[1] == 1000U, "due now: %u Hz", tick_hz[1]);
	kern_clock_tick_idle(1, now - 1U);
	CHECK(tick_hz[1] == 1000U, "overdue: %u Hz", tick_hz[1]);

	/* Leaving idle: the busy rate, reported only after a slowdown. */
	kern_clock_tick_idle(1, UINT64_MAX);
	CHECK(kern_clock_tick_busy(1) && tick_hz[1] == 1000U, "busy after idle");
	CHECK(!kern_clock_tick_busy(1), "busy twice");

	/* CPU 0 counts its own timers: a process timer near keeps it fast. */
	now = clock_ticks();
	process_next = now + 5U;
	itimer_next = UINT64_MAX;
	tick(0);
	kern_clock_tick_idle(0, UINT64_MAX);
	CHECK(tick_hz[0] == 1000U, "process timer near: %u Hz", tick_hz[0]);

	/* ITIMER_REAL far, process timer far: slow. */
	process_next = now + 1000U;
	itimer_next = now + 500U;
	tick(0);
	kern_clock_tick_idle(0, UINT64_MAX);
	CHECK(tick_hz[0] == 100U, "both far: %u Hz", tick_hz[0]);

	/* ITIMER_REAL near: fast again at the next look. */
	itimer_next = now + 3U;
	tick(0);
	kern_clock_tick_idle(0, UINT64_MAX);
	CHECK(tick_hz[0] == 1000U, "itimer near: %u Hz", tick_hz[0]);

	/* A timer armed on CPU 2 while CPU 0 is slow wakes CPU 0, which goes fast. */
	itimer_next = UINT64_MAX;
	process_next = UINT64_MAX;
	tick(0);
	kern_clock_tick_idle(0, UINT64_MAX);
	CHECK(tick_hz[0] == 100U, "nothing: %u Hz", tick_hz[0]);
	current_cpu = 2;
	before = notify_calls;
	kern_clock_timer_armed(0U);
	CHECK(notify_calls == before + 1U && notify_last == 0U, "armed: %u notifications to %u", notify_calls - before, notify_last);
	current_cpu = 0;
	kern_clock_tick_idle(0, UINT64_MAX);
	CHECK(tick_hz[0] == 1000U, "armed: %u Hz", tick_hz[0]);

	/* CPU 0's next tick looks again and finds nothing near. */
	tick(0);
	kern_clock_tick_idle(0, UINT64_MAX);
	CHECK(tick_hz[0] == 100U, "looked again: %u Hz", tick_hz[0]);

	/* A far timer does not wake a slow CPU 0, which slows down again at its next look; a near one does. */
	now = clock_ticks();
	current_cpu = 2;
	before = notify_calls;
	kern_clock_timer_armed(now + 40U);
	CHECK(notify_calls == before, "far timer notified");
	current_cpu = 0;
	kern_clock_tick_idle(0, UINT64_MAX);
	CHECK(tick_hz[0] == 100U, "far timer: %u Hz", tick_hz[0]);
	current_cpu = 2;
	kern_clock_timer_armed(now + 11U);
	CHECK(notify_calls == before + 1U, "near timer not notified");
	current_cpu = 0;
	kern_clock_tick_idle(0, UINT64_MAX);
	CHECK(tick_hz[0] == 1000U, "near timer: %u Hz", tick_hz[0]);

	/* A fast CPU 0 is not notified. */
	current_cpu = 2;
	before = notify_calls;
	kern_clock_timer_armed(0U);
	CHECK(notify_calls == before, "fast CPU 0 notified");

	/* Another CPU ignores CPU 0's timers. */
	current_cpu = 3;
	kern_clock_tick_idle(3, UINT64_MAX);
	CHECK(tick_hz[3] == 100U, "CPU 3 with CPU 0's timer due: %u Hz", tick_hz[3]);
}

static void
test_counter_failure(void)
{
	uint64_t before;

	/* A counter that stops reading leaves the units to CPU 0's interrupts, at the rate. */
	current_cpu = 0;
	tick(0);
	before = clock_ticks();
	counter_on = false;
	tick_hz[0] = 1000U;
	tick(0);
	tick(0);
	CHECK(clock_ticks() == before + 2U, "failed counter: %llu", (unsigned long long)(clock_ticks() - before));
	counter_on = true;
}

int
main(void)
{
	kern_clock_init();
	test_interrupt_mode();
	test_counter_mode();
	test_idle_rate();
	test_counter_failure();
	printf("clock-host: %u checks, %u failures\n", checks, failures);
	if (failures != 0) {
		printf("clock-host: FAIL\n");
		return 1;
	}
	printf("clock-host: PASS\n");
	return 0;
}
