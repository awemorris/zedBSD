/*
 * WS004 HW-T02 checked legacy PCI HCD teardown lifecycle fixture
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

enum hcd_kind {
	HCD_EHCI,
	HCD_UHCI,
};

struct fixture {
	enum hcd_kind kind;
	int preflight_result;
	int halt_result;
	int bus_master_result;
	int irq_result;
	bool controller_interrupts;
	bool controller_running;
	bool quiesced;
	bool irq_cookie;
	bool irq_allocation;
	bool dma;
	bool pci_window;
	bool hcd_registered;
	bool bus_master;
	bool driver_data;
	bool controller;
	unsigned irq_remove_calls;
	unsigned irq_free_calls;
	unsigned dma_free_calls;
	unsigned window_release_calls;
	unsigned hcd_unregister_calls;
	unsigned quiesce_calls;
	unsigned stop_calls;
	unsigned controller_free_calls;
};

static void
fixture_init(struct fixture *fixture, enum hcd_kind kind)
{
	memset(fixture, 0, sizeof(*fixture));
	fixture->kind = kind;
	fixture->controller_interrupts = true;
	fixture->controller_running = true;
	fixture->irq_cookie = true;
	fixture->irq_allocation = true;
	fixture->dma = true;
	fixture->pci_window = true;
	fixture->hcd_registered = true;
	fixture->bus_master = true;
	fixture->driver_data = true;
	fixture->controller = true;
}

/*
 * This small state model is shared by the EHCI and UHCI cases. It captures
 * the production detach contract rather than controller register details:
 * hardware is made quiet before checked IRQ removal, and ownership is not
 * released until that removal succeeds.
 */
static int
checked_quiesce(struct fixture *fixture)
{
	fixture->quiesce_calls++;
	fixture->controller_interrupts = false;
	if (fixture->halt_result != 0) {
		/* EHCI additionally cuts off DMA after a failed halt wait. UHCI
		 * returns at the failed port-I/O halt boundary. Neither path may
		 * release ownership. */
		if (fixture->kind == HCD_EHCI &&
		    fixture->bus_master_result == 0)
			fixture->bus_master = false;
		return fixture->halt_result;
	}
	fixture->controller_running = false;
	if (fixture->bus_master_result != 0)
		return fixture->bus_master_result;
	fixture->bus_master = false;
	fixture->irq_remove_calls++;
	if (fixture->irq_result != 0)
		return fixture->irq_result;
	fixture->irq_cookie = false;
	fixture->quiesced = true;
	return 0;
}

static int
hcd_unregister(struct fixture *fixture)
{
	int error;

	fixture->hcd_unregister_calls++;
	if (fixture->preflight_result != 0)
		return fixture->preflight_result;
	error = checked_quiesce(fixture);
	if (error != 0)
		return error;
	fixture->hcd_registered = false;
	fixture->stop_calls++;
	assert(fixture->quiesced);
	assert(fixture->dma);
	fixture->dma = false;
	fixture->dma_free_calls++;
	return 0;
}

static int
detach(struct fixture *fixture)
{
	int error;

	if (!fixture->controller)
		return 0;
	error = hcd_unregister(fixture);
	if (error != 0)
		return error;
	assert(!fixture->irq_cookie);
	assert(fixture->irq_allocation);
	fixture->irq_allocation = false;
	fixture->irq_free_calls++;
	assert(fixture->pci_window);
	fixture->pci_window = false;
	fixture->window_release_calls++;
	fixture->driver_data = false;
	fixture->controller = false;
	fixture->controller_free_calls++;
	return 0;
}

static void
assert_failed_removal_retains_ownership(const struct fixture *fixture)
{
	assert(fixture->irq_cookie);
	assert(fixture->irq_allocation);
	assert(fixture->dma);
	assert(fixture->pci_window);
	assert(fixture->hcd_registered);
	assert(fixture->driver_data);
	assert(fixture->controller);
	assert(fixture->irq_free_calls == 0U);
	assert(fixture->dma_free_calls == 0U);
	assert(fixture->stop_calls == 0U);
	assert(fixture->window_release_calls == 0U);
	assert(fixture->controller_free_calls == 0U);
}

static void
assert_fully_detached_once(const struct fixture *fixture)
{
	assert(!fixture->controller_interrupts);
	assert(!fixture->controller_running);
	assert(fixture->quiesced);
	assert(!fixture->bus_master);
	assert(!fixture->irq_cookie);
	assert(!fixture->irq_allocation);
	assert(!fixture->dma);
	assert(!fixture->pci_window);
	assert(!fixture->hcd_registered);
	assert(!fixture->driver_data);
	assert(!fixture->controller);
	assert(fixture->irq_free_calls == 1U);
	assert(fixture->dma_free_calls == 1U);
	assert(fixture->stop_calls == 1U);
	assert(fixture->window_release_calls == 1U);
	assert(fixture->controller_free_calls == 1U);
}

static void
test_busy_then_retry(enum hcd_kind kind)
{
	struct fixture fixture;
	unsigned irq_remove_calls, hcd_unregister_calls;

	fixture_init(&fixture, kind);
	fixture.irq_result = EBUSY;
	assert(detach(&fixture) == EBUSY);
	assert(fixture.irq_remove_calls == 1U);
	assert(fixture.hcd_unregister_calls == 1U);
	assert_failed_removal_retains_ownership(&fixture);
	assert(!fixture.controller_interrupts);
	assert(!fixture.controller_running);
	assert(!fixture.bus_master);
	assert(!fixture.quiesced);

	fixture.irq_result = 0;
	assert(detach(&fixture) == 0);
	assert(fixture.irq_remove_calls == 2U);
	assert(fixture.hcd_unregister_calls == 2U);
	assert_fully_detached_once(&fixture);

	irq_remove_calls = fixture.irq_remove_calls;
	hcd_unregister_calls = fixture.hcd_unregister_calls;
	assert(detach(&fixture) == 0);
	assert(fixture.irq_remove_calls == irq_remove_calls);
	assert(fixture.hcd_unregister_calls == hcd_unregister_calls);
	assert_fully_detached_once(&fixture);
}

static void
test_persistent_failure(enum hcd_kind kind, int failure)
{
	struct fixture fixture;

	fixture_init(&fixture, kind);
	fixture.irq_result = failure;
	assert(detach(&fixture) == failure);
	assert_failed_removal_retains_ownership(&fixture);
	assert(detach(&fixture) == failure);
	assert(fixture.irq_remove_calls == 2U);
	assert(fixture.hcd_unregister_calls == 2U);
	assert_failed_removal_retains_ownership(&fixture);
	assert(!fixture.controller_interrupts);
	assert(!fixture.controller_running);
	assert(!fixture.bus_master);
	assert(!fixture.quiesced);
}

static void
test_unregister_preflight_busy(enum hcd_kind kind)
{
	struct fixture fixture;

	fixture_init(&fixture, kind);
	fixture.preflight_result = EBUSY;
	assert(detach(&fixture) == EBUSY);
	assert(fixture.hcd_unregister_calls == 1U);
	assert(fixture.quiesce_calls == 0U);
	assert(fixture.irq_remove_calls == 0U);
	assert(fixture.controller_interrupts);
	assert(fixture.controller_running);
	assert(fixture.bus_master);
	assert(!fixture.quiesced);
	assert_failed_removal_retains_ownership(&fixture);
}

static void
test_halt_failure(enum hcd_kind kind)
{
	struct fixture fixture;

	fixture_init(&fixture, kind);
	fixture.halt_result = ETIMEDOUT;
	assert(detach(&fixture) == ETIMEDOUT);
	assert(fixture.quiesce_calls == 1U);
	assert(fixture.irq_remove_calls == 0U);
	assert(!fixture.controller_interrupts);
	assert(fixture.controller_running);
	assert(fixture.bus_master == (kind == HCD_UHCI));
	assert(!fixture.quiesced);
	assert_failed_removal_retains_ownership(&fixture);
}

static void
test_bus_master_failure(enum hcd_kind kind)
{
	struct fixture fixture;

	fixture_init(&fixture, kind);
	fixture.bus_master_result = EIO;
	assert(detach(&fixture) == EIO);
	assert(fixture.quiesce_calls == 1U);
	assert(fixture.irq_remove_calls == 0U);
	assert(!fixture.controller_interrupts);
	assert(!fixture.controller_running);
	assert(fixture.bus_master);
	assert(!fixture.quiesced);
	assert_failed_removal_retains_ownership(&fixture);
}

static void
test_controller(enum hcd_kind kind)
{
	test_unregister_preflight_busy(kind);
	test_halt_failure(kind);
	test_bus_master_failure(kind);
	test_busy_then_retry(kind);
	test_persistent_failure(kind, EBUSY);
	test_persistent_failure(kind, EIO);
}

int
main(void)
{
	test_controller(HCD_EHCI);
	test_controller(HCD_UHCI);
	puts("PCI EHCI/UHCI checked IRQ teardown lifecycle test: PASS");
	return 0;
}
