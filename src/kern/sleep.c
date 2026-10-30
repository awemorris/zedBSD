/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The system sleep to idle, S0ix (ws052-p006, plan/ws052/proposed/README-v2.md
 * §7, docs/architecture/power-management.md).
 *
 * The thread of the sleep request is the coordinator.  It freezes the user
 * processes, suspends the devices, tells the firmware (LPS0) and leaves
 * only the wake GPEs and the wake IRQs (the SCI), then sets the state to
 * ENTER and sleeps.  Each CPU's idle loop, finding ENTER, enters the HAL's
 * suspend-to-idle (its tick stopped); when all of them are in, the
 * platform lowers itself to SLP_S0.  The first CPU an interrupt brings
 * back turns ENTER into WAKE and wakes the coordinator, which brings the
 * other CPUs back and works out why the system woke: a note a driver left
 * (the power button, the lid, the AC adapter), a GPE that fired (the
 * embedded controller's is a key unless it only told of a battery; another
 * is a device's PME, USB), or nothing (spurious: everything stays
 * suspended and the CPUs go back to sleep, up to SLEEP_SPURIOUS_MAX times).
 * Then every step is undone in reverse order.
 *
 * CPU 0 brings the tick count forward around its own suspend-to-idle
 * (clock.c), from the counter that keeps running; the coordinator compares
 * the time slept by the counter with the RTC's seconds as a sanity check.
 *
 * The drivers' parts are weak: a platform without them builds this file,
 * and kern_sleep_supported() says it cannot sleep before anything is
 * touched.
 */

#include "kern/sleep.h"
#include "kern/atomic.h"
#include "kern/clock.h"
#include "kern/freeze.h"
#include "kern/kcrt.h"
#include "kern/klog.h"
#include "kern/lock.h"
#include "kern/sched.h"
#include "kern/system-event.h"
#include "kern/waitq.h"
#include <uapi/errno.h>
#include <uapi/system.h>

#include <stdbool.h>
#include <hal/hal.h>

/* The state of the sleep the coordinator runs. */
#define SLEEP_NONE		0U
#define SLEEP_ENTER		1U
#define SLEEP_WAKE		2U

/* The most spurious wakes in a row before the sleep ends with the reason "spurious". */
#define SLEEP_SPURIOUS_MAX	10U

/* How long the coordinator waits after a wake for the ACPI events to leave their notes, and how often it looks. */
#define SLEEP_SETTLE_MS		300U
#define SLEEP_SETTLE_STEP_MS	10U

/* The most the counter's and the RTC's times slept may differ, in seconds, before it is logged. */
#define SLEEP_CLOCK_SLACK	2U

/* What a GPE that is none of the ACPI's looks like. */
#define SLEEP_GPE_NONE		0xffffffffU

/*
 * The drivers' parts.  They are weak because only the platforms with ACPI
 * and PCI power management build them; each is tested before it is used.
 */
struct drv_pci_device;
extern int drv_pci_suspend_all(struct drv_pci_device **failed) __attribute__((weak));
extern int drv_pci_resume_all(void) __attribute__((weak));
extern void drv_pci_device_name(struct drv_pci_device *device, char *text, size_t size) __attribute__((weak));
extern int drv_acpi_s0_idle_capable(void) __attribute__((weak));
extern int drv_acpi_lps0_present(void) __attribute__((weak));
extern int drv_acpi_lps0_enter(void) __attribute__((weak));
extern int drv_acpi_lps0_exit(void) __attribute__((weak));
extern int drv_acpi_events_sleep_begin(void) __attribute__((weak));
extern int drv_acpi_events_sleep_end(unsigned *woken) __attribute__((weak));
extern int drv_acpi_events_sleep_woken(unsigned *woken) __attribute__((weak));
extern int drv_acpi_ec_gpe(unsigned *gpe) __attribute__((weak));

/*
 * The sleep's state, and the queue and lock the coordinator waits on.
 * sleep_state is NONE outside a sleep, ENTER while the CPUs are to suspend
 * to idle, WAKE from the first CPU back until the coordinator decides; the
 * coordinator sets ENTER and NONE, the CPUs' idle loops turn ENTER into
 * WAKE with a compare-and-swap.  The lock and queue are made by the first
 * sleep (one sleep at a time, system_sleeping).
 */
static volatile unsigned sleep_state;
static struct spinlock sleep_lock;
static struct wait_queue sleep_waitq;
static unsigned sleep_ready;

/*
 * The notes the drivers leave while a sleep is under way (sleep_noting):
 * the first wake reason, and whether a battery told of news.  The
 * coordinator clears them before each entry and reads them after.
 */
static volatile unsigned sleep_noting;
static volatile unsigned sleep_note;
static volatile unsigned sleep_battery_note;

static void sleep_init(void);
static int sleep_suspend(struct kern_sleep_outcome *outcome);
static unsigned sleep_wait_wake(void);
static unsigned sleep_reason(void);
static void sleep_notify_others(void);
static void sleep_post(const char *subject, const char *detail);
static const char *sleep_reason_name(unsigned reason);

/*
 * Reports whether the machine can sleep to idle: the HAL has a
 * suspend-safe idle state, the firmware declares low-power S0 idle, and
 * the LPS0 device attached.  Returns 1 or 0; nothing is touched.
 */
int
kern_sleep_supported(
	void)
{
	int status;
	int declared;
	int present;

	/* The processors' state. */
	status = hal_cpu_idle_suspend_supported();
	if (status != HAL_OK)
		return 0;

	/* The firmware's declaration, and the parts that suspend and resume the devices. */
	if (drv_acpi_s0_idle_capable == NULL || drv_acpi_lps0_present == NULL)
		return 0;
	if (drv_pci_suspend_all == NULL || drv_pci_resume_all == NULL)
		return 0;
	if (drv_acpi_events_sleep_begin == NULL || drv_acpi_events_sleep_woken == NULL)
		return 0;
	declared = drv_acpi_s0_idle_capable();
	if (!declared)
		return 0;

	/* The LPS0 device that tells the platform. */
	present = drv_acpi_lps0_present();
	if (!present)
		return 0;

	/* Succeeded: the machine can sleep to idle. */
	return 1;
}

/*
 * Sleeps to idle until a wake event, and reports what happened in
 * outcome.  The caller has checked kern_sleep_supported() and holds the
 * one sleep under way.
 */
void
kern_sleep_s0idle(
	struct kern_sleep_outcome *outcome)
{
	unsigned reason;
	int status;

	/* Nothing yet. */
	kern_memset(outcome, 0, sizeof(*outcome));
	outcome->wake = KERN_SLEEP_WAKE_NONE;
	sleep_init();

	/* The begin, before the user processes are frozen. */
	sleep_post("sleep.begin", "");
	kern_logf("sleep: entering S0 idle\n");
	kern_freeze_user();

	/* The devices, the firmware and the interrupts; a step that fails is undone with the ones before. */
	status = sleep_suspend(outcome);
	if (status != 0) {
		kern_thaw_user();
		outcome->result = status;
		kern_logf("sleep: abandoned: error %d device \"%s\"\n", status, outcome->device);
		sleep_post("sleep.failed", outcome->device);
		return;
	}

	/* The CPUs sleep until a real wake, or the limit of spurious ones. */
	reason = sleep_wait_wake();
	outcome->wake = reason;

	/* The interrupts, the GPEs, the firmware and the devices back, in reverse order. */
	(void)hal_irq_resume();
	(void)drv_acpi_events_sleep_end(NULL);
	status = drv_acpi_lps0_exit();
	if (status != 0)
		kern_logf("sleep: the LPS0 exit failed (error %d)\n", status);
	outcome->resume_result = drv_pci_resume_all();

	/* The user processes run again, and the end tells why the system woke. */
	kern_thaw_user();
	kern_logf("sleep: woke, reason %s, resume %d\n", sleep_reason_name(reason), outcome->resume_result);
	sleep_post("sleep.end", sleep_reason_name(reason));
}

/*
 * Runs in place of hal_cpu_idle() in each CPU's idle loop (interrupts
 * masked): while the sleep's state is ENTER the CPU suspends to idle, CPU 0
 * bringing the tick count forward around it, and the first CPU back wakes
 * the coordinator.
 */
void
kern_sleep_idle(
	hal_cpu_id_t cpu)
{
	unsigned long irq;
	unsigned expected;
	unsigned state;
	bool swapped;
	int status;

	/* No sleep: the ordinary idle. */
	state = __atomic_load_n(&sleep_state, __ATOMIC_ACQUIRE);
	if (state != SLEEP_ENTER) {
		hal_cpu_idle();
		return;
	}

	/* The suspend-to-idle, CPU 0 keeping the time it slept. */
	if (cpu == 0)
		kern_clock_idle_suspend_begin();
	status = hal_cpu_idle_suspend();
	if (cpu == 0)
		kern_clock_idle_suspend_end();

	/* The coordinator checked the probe, so this does not happen; it ends the sleep rather than spin. */
	if (status != HAL_OK)
		kern_logf("sleep: CPU %u cannot suspend to idle (%d)\n", (unsigned)cpu, status);

	/* The first CPU back turns ENTER into WAKE and wakes the coordinator. */
	expected = SLEEP_ENTER;
	swapped = __atomic_compare_exchange_n(&sleep_state, &expected, SLEEP_WAKE, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
	if (!swapped)
		return;

	/* The coordinator looks at the state again. */
	irq = spin_lock_irqsave(&sleep_lock);

	waitq_wake_all(&sleep_waitq);

	spin_unlock_irqrestore(&sleep_lock, irq);
}

/*
 * Notes why the system woke, while a sleep is under way (a driver's
 * handler calls it: the power button, the lid, the AC adapter, or a
 * battery's news with KERN_SLEEP_NOTE_BATTERY).  The first reason is kept.
 */
void
kern_sleep_note_wake(
	unsigned reason)
{
	unsigned expected;
	unsigned noting;

	/* Outside a sleep a note means nothing. */
	noting = __atomic_load_n(&sleep_noting, __ATOMIC_ACQUIRE);
	if (!noting)
		return;

	/* A battery's news is kept apart: it is not a reason by itself. */
	if (reason == KERN_SLEEP_NOTE_BATTERY) {
		__atomic_store_n(&sleep_battery_note, 1U, __ATOMIC_RELEASE);
		return;
	}

	/* The first reason only. */
	expected = KERN_SLEEP_WAKE_NONE;
	(void)__atomic_compare_exchange_n(&sleep_note, &expected, reason, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/* Makes the lock and the queue once (the first sleep, which is the only one under way). */
static void
sleep_init(
	void)
{
	/* Made already. */
	if (sleep_ready)
		return;

	/* The lock and the queue. */
	spin_init(&sleep_lock, LOCK_RANK_DEVICE, "sleep");
	waitq_init(&sleep_waitq, "sleep");
	sleep_ready = 1U;
}

/*
 * Suspends the devices, tells the firmware and holds every interrupt but
 * the wake ones.  Returns 0, or the error of the step that failed with the
 * steps before it undone (the device that refused named in outcome).
 */
static int
sleep_suspend(
	struct kern_sleep_outcome *outcome)
{
	struct drv_pci_device *failed;
	int status;

	/* The devices, children first; a refusal resumes the ones suspended and names the device. */
	failed = NULL;
	status = drv_pci_suspend_all(&failed);
	if (status != 0) {
		if (failed != NULL && drv_pci_device_name != NULL)
			drv_pci_device_name(failed, outcome->device, sizeof(outcome->device));
		return status;
	}

	/* The firmware: the displays off and the low-power entry. */
	status = drv_acpi_lps0_enter();
	if (status != 0) {
		(void)drv_acpi_lps0_exit();
		(void)drv_pci_resume_all();
		kern_strcpy(outcome->device, "lps0");
		return status;
	}

	/* Only the wake GPEs raise SCIs. */
	status = drv_acpi_events_sleep_begin();
	if (status != 0) {
		(void)drv_acpi_lps0_exit();
		(void)drv_pci_resume_all();
		kern_strcpy(outcome->device, "acpi events");
		return status;
	}

	/* Only the wake IRQs (the SCI) deliver. */
	status = hal_irq_suspend();
	if (status != HAL_OK) {
		(void)drv_acpi_events_sleep_end(NULL);
		(void)drv_acpi_lps0_exit();
		(void)drv_pci_resume_all();
		kern_strcpy(outcome->device, "interrupts");
		return EIO;
	}

	/* Succeeded: the platform may lower itself once the CPUs are in. */
	return 0;
}

/*
 * Lets the CPUs suspend to idle until a real wake or the limit of
 * spurious wakes, and returns the reason.  The counter's and the RTC's
 * times slept are compared at the end.
 */
static unsigned
sleep_wait_wake(
	void)
{
	unsigned long irq;
	uint64_t observed;
	uint64_t counter_start;
	uint64_t counter_end;
	uint64_t frequency;
	uint64_t rtc_start;
	uint64_t rtc_end;
	uint64_t counter_seconds;
	uint64_t rtc_seconds;
	unsigned spurious;
	unsigned reason;
	unsigned state;
	bool counted;
	bool dated;

	/* The two clocks before the first entry. */
	counter_start = 0U;
	counted = hal_rtc_read_counter(&counter_start, &frequency);
	rtc_start = 0U;
	dated = hal_rtc_read_epoch_time(&rtc_start);

	/* Each entry: the notes cleared, ENTER, the CPUs told, and the coordinator asleep until a CPU is back. */
	spurious = 0U;
	for (;;) {
		/* The notes of this entry. */
		__atomic_store_n(&sleep_note, KERN_SLEEP_WAKE_NONE, __ATOMIC_RELEASE);
		__atomic_store_n(&sleep_battery_note, 0U, __ATOMIC_RELEASE);
		__atomic_store_n(&sleep_noting, 1U, __ATOMIC_RELEASE);

		/* ENTER, and every other CPU looks at it (its idle loop then suspends). */
		__atomic_store_n(&sleep_state, SLEEP_ENTER, __ATOMIC_RELEASE);
		sleep_notify_others();

		/* Sleeps until a CPU turns ENTER into WAKE; this CPU then suspends too. */
		irq = spin_lock_irqsave(&sleep_lock);

		state = __atomic_load_n(&sleep_state, __ATOMIC_ACQUIRE);
		while (state == SLEEP_ENTER) {
			observed = waitq_sequence(&sleep_waitq);
			(void)waitq_sleep(&sleep_waitq, &sleep_lock, observed, 0, 0);
			state = __atomic_load_n(&sleep_state, __ATOMIC_ACQUIRE);
		}

		spin_unlock_irqrestore(&sleep_lock, irq);

		/* Every CPU still suspended comes back, its tick running again. */
		sleep_notify_others();

		/* Why: a real reason ends the sleep, a spurious wake goes back to sleep until the limit. */
		reason = sleep_reason();
		if (reason != KERN_SLEEP_WAKE_SPURIOUS)
			break;
		spurious++;
		if (spurious >= SLEEP_SPURIOUS_MAX)
			break;
		kern_logf("sleep: spurious wake %u, sleeping again\n", spurious);
	}

	/* The sleep is over: no more entries, no more notes. */
	__atomic_store_n(&sleep_noting, 0U, __ATOMIC_RELEASE);
	__atomic_store_n(&sleep_state, SLEEP_NONE, __ATOMIC_RELEASE);

	/* The counter's time slept against the RTC's, a sanity check of the counter through the sleep. */
	counter_end = 0U;
	rtc_end = 0U;
	if (counted)
		counted = hal_rtc_read_counter(&counter_end, &frequency);
	if (dated)
		dated = hal_rtc_read_epoch_time(&rtc_end);
	if (counted && dated && frequency != 0U) {
		counter_seconds = (counter_end - counter_start) / frequency;
		rtc_seconds = rtc_end - rtc_start;
		kern_logf("sleep: slept %llu s by the counter, %llu s by the RTC\n", (unsigned long long)counter_seconds, (unsigned long long)rtc_seconds);
		if (counter_seconds + SLEEP_CLOCK_SLACK < rtc_seconds || rtc_seconds + SLEEP_CLOCK_SLACK < counter_seconds)
			kern_logf("sleep: WARNING the counter and the RTC disagree across the sleep\n");
	}

	/* Succeeded: the reason the system woke. */
	return reason;
}

/*
 * Works out why the system woke: waits up to SLEEP_SETTLE_MS for the ACPI
 * events to leave a note, then judges by the note, else by the GPE that
 * fired.  Returns a KERN_SLEEP_WAKE_* reason, SPURIOUS when nothing real
 * happened.
 */
static unsigned
sleep_reason(
	void)
{
	unsigned waited;
	unsigned note;
	unsigned battery;
	unsigned gpe;
	unsigned ec_gpe;
	uint64_t settle_ticks;
	int status;

	/* A note from a driver's handler, which runs in the ACPI event thread after the SCI. */
	note = KERN_SLEEP_WAKE_NONE;

	/* Waits for the note in short steps, up to the settle time. */
	settle_ticks = kern_ms_to_ticks(SLEEP_SETTLE_STEP_MS);
	for (waited = 0U; waited < SLEEP_SETTLE_MS; waited += SLEEP_SETTLE_STEP_MS) {
		note = __atomic_load_n(&sleep_note, __ATOMIC_ACQUIRE);
		if (note != KERN_SLEEP_WAKE_NONE)
			break;
		sched_sleep(sched_ticks() + settle_ticks);
	}

	/* A note is the reason. */
	if (note != KERN_SLEEP_WAKE_NONE)
		return note;

	/* Otherwise the GPE that fired, if any. */
	gpe = SLEEP_GPE_NONE;
	status = drv_acpi_events_sleep_woken(&gpe);
	if (status != 0 || gpe == SLEEP_GPE_NONE)
		return KERN_SLEEP_WAKE_SPURIOUS;

	/* The embedded controller's GPE is a key, unless all it told of was a battery. */
	ec_gpe = SLEEP_GPE_NONE;
	if (drv_acpi_ec_gpe != NULL)
		(void)drv_acpi_ec_gpe(&ec_gpe);
	if (gpe == ec_gpe) {
		battery = __atomic_load_n(&sleep_battery_note, __ATOMIC_ACQUIRE);
		if (battery)
			return KERN_SLEEP_WAKE_SPURIOUS;
		return KERN_SLEEP_WAKE_KEYBOARD;
	}

	/* Any other GPE is a device's PME (the xHCI's, a PCIe port's). */
	return KERN_SLEEP_WAKE_USB;
}

/* Interrupts every online CPU but this one, so that its idle loop looks at the sleep's state. */
static void
sleep_notify_others(
	void)
{
	struct hal_cpu_mask targets;
	hal_cpu_id_t self;

	/* The online CPUs but this one. */
	hal_cpu_ready_mask(&targets);
	self = hal_cpu_current();
	hal_cpu_mask_clear(&targets, self);

	/* Delivered even to a CPU in suspend-to-idle and while the IRQs are held (the HAL's contract). */
	(void)hal_cpu_notify_mask(&targets);
}

/* Posts a sleep event of the power class (power.sleep.begin, .end reason=, .failed device=). */
static void
sleep_post(
	const char *subject,
	const char *detail)
{
	char text[KERN_SYSTEM_EVENT_DETAIL_MAX];
	int same;

	/* The detail: the end's reason, the failure's device, nothing for the begin. */
	text[0] = '\0';
	same = kern_strcmp(subject, "sleep.end");
	if (same == 0)
		kern_snprintf(text, sizeof(text), "reason=%s", detail);
	same = kern_strcmp(subject, "sleep.failed");
	if (same == 0)
		kern_snprintf(text, sizeof(text), "device=%s", detail);

	/* The event. */
	kern_system_event_post(KERN_SYSTEM_EVENT_POWER, KERN_SYSTEM_EVENT_CHANGE, 0, subject, text);
}

/* Names a wake reason for the log and the end event. */
static const char *
sleep_reason_name(
	unsigned reason)
{
	/* Each reason. */
	switch (reason) {
	case KERN_SLEEP_WAKE_POWER_BUTTON:
		return "power-button";
	case KERN_SLEEP_WAKE_LID:
		return "lid";
	case KERN_SLEEP_WAKE_KEYBOARD:
		return "keyboard";
	case KERN_SLEEP_WAKE_USB:
		return "usb";
	case KERN_SLEEP_WAKE_AC:
		return "ac";
	case KERN_SLEEP_WAKE_TIMER:
		return "timer";
	case KERN_SLEEP_WAKE_SPURIOUS:
		return "spurious";
	default:
		break;
	}

	/* Anything else. */
	return "other";
}
