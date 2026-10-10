/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Runs the production command waiter against controlled clock/IRQ/event inputs.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#define XHCI_TIMEOUT 10000000U
#define XHCI_COMMAND_WAIT_SECONDS 5U

/* One modeled DMA event, with the production hardware word layout. */
struct xhci_trb {
	uint32_t parameter_low;
	uint32_t parameter_high;
	uint32_t status;
	uint32_t control;
};

/* A command-ring identity whose backing never changes in these scenarios. */
struct xhci_ring {
	uint64_t address;
};

/* The command/event ownership fields consumed by the production function. */
struct xhci_controller {
	unsigned command_busy;
	unsigned command_failed;
	unsigned dma_quiesced;
	unsigned command_event_ready;
	uint64_t command_address;
	struct xhci_ring command;
	struct xhci_trb command_event;
};

/* One fixture thread owns the controller for the duration of each scenario. */
static struct xhci_controller controller;

/* Clock/event modes are test inputs, separate from production driver settings. */
static unsigned mode;

/* Models caller IRQ state and detects a poll critical section without protection. */
static bool irq_enabled;

/* Counts scheduling opportunities while the command owner waits. */
static unsigned yielded;

/* Records the event lock and verifies it is not held when IRQ delivery resumes. */
static unsigned event_locked;

/* The fixture clock advances at each hardware sample or scheduled yield. */
static uint64_t counter;

/* The reset fixture owns ticks until the next scenario. */
static uint64_t ticks;

/* Counts hardware polls, publications and timeout reports without touching DMA. */
static unsigned polled;

/* Counts publications by the current fixture; reset between scenarios. */
static unsigned rung;

/* Counts reported timeouts in the current scenario. */
static unsigned reported;

static bool kern_irq_disable(void);
static void kern_irq_enable(void);
static void sched_yield(void);
static bool kern_rtc_read_counter(uint64_t *sample, uint64_t *frequency);
static uint64_t sched_ticks(void);
static uint64_t kern_ms_to_ticks(uint64_t milliseconds);
static uint64_t ring_push(struct xhci_ring *ring, uint64_t parameter, uint32_t status, uint32_t control);
static void event_lock(struct xhci_controller *c);
static void event_unlock(struct xhci_controller *c);
static void xhci_ring_doorbell(struct xhci_controller *c, unsigned slot, uint32_t target);
static int event_take(struct xhci_controller *c, struct xhci_trb *event);
static int transfer_claim(struct xhci_controller *c, const struct xhci_trb *event);
static void port_change_defer(struct xhci_controller *c);
static uint64_t xhci_event_pointer(const struct xhci_trb *event);
static int drv_xhci_command_completion_matches(uint64_t expected, uint64_t actual);
static void xhci_completion_drain(struct xhci_controller *c);
static void xhci_command_timeout_report(struct xhci_controller *c);
static void kern_logf(const char *format, ...);
static void reset_fixture(unsigned next_mode, bool enable_irq);
static int command_ex(struct xhci_controller *c, uint64_t parameter, uint32_t status, uint32_t control, unsigned *slot, unsigned *completion);

/*
 * Runs completion, CPU progress, refusal and timeout scenarios.
 */
int
main(
	void)
{
	unsigned scenario;
	unsigned slot;
	unsigned completion;
	int error;

	/* Tests both direct polling and an IRQ completion handed to the owner. */
	for (scenario = 1; scenario <= 4; scenario++) {
		reset_fixture(scenario, true);
		error = command_ex(
		    &controller,
		    0,
		    0,
		    9U << 10,
		    &slot,
		    &completion);
		assert(irq_enabled);
		assert(controller.command_busy == 0);
		assert(yielded != 0);

		/* The missing-event mode must retain unresolved command ownership. */
		if (scenario == 2) {
			assert(error == ETIMEDOUT);
			assert(controller.command_failed == 1);
			assert(polled < 10);
			assert(reported == 1);
		} else {
			assert(error == 0);
			assert(slot == 2);
			assert(completion == 1);
			assert(controller.command_failed == 0);
		}
	}

	/* An IRQ-disabled caller times out from hardware time without yielding. */
	reset_fixture(2, false);
	error = command_ex(&controller, 0, 0, 9U << 10, NULL, NULL);
	assert(error == ETIMEDOUT);
	assert(!irq_enabled);
	assert(yielded == 0);
	assert(polled < 10);

	/* A missing counter falls back to scheduler time; a changed clock retires no DMA. */
	for (scenario = 5; scenario <= 6; scenario++) {
		reset_fixture(scenario, true);
		error = command_ex(&controller, 0, 0, 9U << 10, NULL, NULL);
		assert(error == ETIMEDOUT);
		assert(irq_enabled);
		assert(controller.command_failed == 1);
		assert(polled < 10);
	}

	/* A hardware refusal is a completion, rather than an unresolved command. */
	reset_fixture(7, true);
	error = command_ex(&controller, 0, 0, 9U << 10, NULL, NULL);
	assert(error == EIO);
	assert(controller.command_failed == 0);

	/* No new doorbell may be published after an uncompleted command. */
	reset_fixture(1, true);
	controller.command_failed = 1;
	error = command_ex(&controller, 0, 0, 9U << 10, NULL, NULL);
	assert(error == EIO);
	assert(rung == 0);
	assert(irq_enabled);

	/* Reports that the actual command wait function met the scenario checks. */
	puts("production xHCI command wait: PASS (9 scenarios)");

	/* Succeeded: all production waiter scenarios met their assertions. */
	return 0;
}

/* Saves IRQ state exactly as the production critical sections require. */
static bool
kern_irq_disable(
	void)
{
	bool previous;

	/* Retains the caller state for the next matching restore. */
	previous = irq_enabled;
	irq_enabled = false;

	/* Reports the caller's prior delivery state. */
	return previous;
}

/* Enables modeled delivery only outside the shared event lock. */
static void
kern_irq_enable(
	void)
{
	/* IRQ delivery must not reenter a protected consumer. */
	assert(event_locked == 0);
	irq_enabled = true;
}

/* Models unrelated CPU progress and an IRQ completion handed to the owner. */
static void
sched_yield(
	void)
{
	/* Advancing a worker must leave unrelated interrupts serviceable. */
	assert(irq_enabled);
	yielded++;
	ticks += 1000;

	/* Models the handler publishing a completion after the doorbell. */
	if (mode == 3 && rung != 0) {
		controller.command_event.parameter_low = 0x1000;
		controller.command_event.status = 1U << 24;
		controller.command_event.control = (33U << 10) | (2U << 24);
		controller.command_event_ready = 1;
	}
}

/* Supplies an advancing, absent or changed-frequency hardware counter. */
static bool
kern_rtc_read_counter(
	uint64_t *sample,
	uint64_t *frequency)
{
	/* Advances the hardware clock independently of scheduler ticks. */
	counter += 1000;
	*sample = counter;
	*frequency = 1000;

	/* Models a HAL without a hardware counter. */
	if (mode == 5)
		return false;

	/* A changed frequency must not allow a false completion or indefinite wait. */
	if (mode == 6 && counter > 1000)
		*frequency = 2000;

	/* Succeeded: the sample is available even while IRQ is disabled. */
	return true;
}

/* Reports scheduler progress separately from the hardware counter. */
static uint64_t
sched_ticks(
	void)
{
	/* Supplies the modeled periodic tick count. */
	return ticks;
}

/* Uses the fixture's 1000Hz scheduler conversion. */
static uint64_t
kern_ms_to_ticks(
	uint64_t milliseconds)
{
	/* One millisecond is one fixture tick. */
	return milliseconds;
}

/* Publishes one descriptor in the modeled ring. */
static uint64_t
ring_push(
	struct xhci_ring *ring,
	uint64_t parameter,
	uint32_t status,
	uint32_t control)
{
	(void)parameter;
	(void)status;
	(void)control;

	/* Reports the descriptor address used by all completions. */
	return ring->address;
}

/* Requires IRQ protection for each shared event-ring critical section. */
static void
event_lock(
	struct xhci_controller *c)
{
	(void)c;

	/* Requires the modeled protocol invariant before altering its state. */
	assert(!irq_enabled);
	assert(event_locked == 0);
	event_locked = 1;
}

/* Ends the modeled event-ring critical section. */
static void
event_unlock(
	struct xhci_controller *c)
{
	(void)c;

	/* Requires the modeled protocol invariant before altering its state. */
	assert(event_locked == 1);
	event_locked = 0;
}

/* Records command publication without simulating PCI transport. */
static void
xhci_ring_doorbell(
	struct xhci_controller *c,
	unsigned slot,
	uint32_t target)
{
	(void)c;

	/* Requires the modeled protocol invariant before altering its state. */
	assert(slot == 0);
	assert(target == 0);
	rung++;
}

/* Supplies a completion, a stale event or an empty ring. */
static int
event_take(
	struct xhci_controller *c,
	struct xhci_trb *event)
{
	(void)c;

	/* Counts this consumer visit before selecting the modeled event. */
	polled++;
	memset(event, 0, sizeof(*event));

	/* These inputs provide completions instead of an empty ring. */
	if (mode == 1 || mode == 4 || mode == 7) {
		event->parameter_low = 0x1000;

		/* The first stale descriptor must not complete this command. */
		if (mode == 4 && polled == 1)
			event->parameter_low += 16;

		/* Supplies success unless the hardware-refusal scenario overrides it. */
		event->status = 1U << 24;

		/* Models a matching completion with a hardware error. */
		if (mode == 7)
			event->status = 2U << 24;

		/* Publishes the completion kind and returned slot. */
		event->control = (33U << 10) | (2U << 24);

		/* Succeeded: one modeled event was supplied. */
		return 1;
	}

	/* No controller event is available for the timeout scenarios. */
	return 0;
}

/* Leaves transfer ownership unchanged in this command-only fixture. */
static int
transfer_claim(
	struct xhci_controller *c,
	const struct xhci_trb *event)
{
	(void)c;
	(void)event;

	/* Succeeded: there is no transfer payload in the model. */
	return 0;
}

/* Leaves unrelated port notification outside this command-only fixture. */
static void
port_change_defer(
	struct xhci_controller *c)
{
	(void)c;
}

/* Reads the hardware descriptor address named by a completion. */
static uint64_t
xhci_event_pointer(
	const struct xhci_trb *event)
{
	/* Supplies the event's complete pointer word. */
	return ((uint64_t)event->parameter_high << 32) | event->parameter_low;
}

/* Checks pointer identity independently of event availability. */
static int
drv_xhci_command_completion_matches(
	uint64_t expected,
	uint64_t actual)
{
	/* Refuses a completion for another descriptor. */
	if (expected != actual)
		return 0;

	/* The modeled event names this command. */
	return 1;
}

/* Requires the command gate to be open before deferred callbacks run. */
static void
xhci_completion_drain(
	struct xhci_controller *c)
{
	/* Deferred callbacks must not recursively enter a retained command gate. */
	assert(c->command_busy == 0);
	assert(event_locked == 0);
}

/* Counts timeout reporting without controller MMIO or resource release. */
static void
xhci_command_timeout_report(
	struct xhci_controller *c)
{
	/* Timeout reporting must observe the poisoned command gate. */
	assert(c->command_failed == 1);
	reported++;
}

/* Keeps diagnostic text outside the fixture's pass/fail decisions. */
static void
kern_logf(
	const char *format,
	...)
{
	(void)format;
}

/* The runner extracts this function unchanged from production source. */
#include "command-wait-under-test.inc"

/* Starts one scenario with no state from previous completions. */
static void
reset_fixture(
	unsigned next_mode,
	bool enable_irq)
{
	/* Starts the controller with no outstanding command or handoff. */
	memset(&controller, 0, sizeof(controller));
	controller.command.address = 0x1000;

	/* Resets clocks and observations for this fixture owner. */
	mode = next_mode;
	irq_enabled = enable_irq;
	yielded = 0;
	event_locked = 0;
	counter = 0;
	ticks = 0;
	polled = 0;
	rung = 0;
	reported = 0;
}
