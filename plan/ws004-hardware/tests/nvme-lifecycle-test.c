/*
 * WS004 p022 NVMe production cleanup transaction fixture
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#include "drivers/pci-nvme-lifecycle.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

enum injected_action {
	INJECT_NONE,
	INJECT_CONTROLLER_DISABLE,
	INJECT_MASTER_DISABLE,
	INJECT_IRQ_DISESTABLISH,
	INJECT_IRQ_DRAIN,
	INJECT_BAR_RESTORE,
	INJECT_PCI_RESTORE
};

struct fixture {
	enum injected_action injected;
	int injected_error;
	unsigned calls[10];
	unsigned sequence[10];
	unsigned sequence_count;
};

enum call_index {
	CALL_CONTROLLER_DISABLE,
	CALL_MASTER_DISABLE,
	CALL_IRQ_DISESTABLISH,
	CALL_IRQ_DRAIN,
	CALL_IRQ_FREE,
	CALL_DMA_FREE,
	CALL_BAR_UNMAP,
	CALL_BAR_RESTORE,
	CALL_PCI_RESTORE,
	CALL_BAR_RELEASE
};

static int
fallible_call(struct fixture *fixture, enum call_index call,
	enum injected_action action)
{
	assert(fixture->sequence_count <
	    sizeof(fixture->sequence) / sizeof(fixture->sequence[0]));
	fixture->calls[call]++;
	fixture->sequence[fixture->sequence_count++] = call;
	return fixture->injected == action ? fixture->injected_error : 0;
}

static void
infallible_call(struct fixture *fixture, enum call_index call)
{
	assert(fixture->sequence_count <
	    sizeof(fixture->sequence) / sizeof(fixture->sequence[0]));
	fixture->calls[call]++;
	fixture->sequence[fixture->sequence_count++] = call;
}

static int controller_disable(void *p) { return fallible_call(p,
	CALL_CONTROLLER_DISABLE, INJECT_CONTROLLER_DISABLE); }
static int master_disable(void *p) { return fallible_call(p,
	CALL_MASTER_DISABLE, INJECT_MASTER_DISABLE); }
static int irq_disestablish(void *p) { return fallible_call(p,
	CALL_IRQ_DISESTABLISH, INJECT_IRQ_DISESTABLISH); }
static int irq_drain(void *p) { return fallible_call(p,
	CALL_IRQ_DRAIN, INJECT_IRQ_DRAIN); }
static void irq_free(void *p) { infallible_call(p, CALL_IRQ_FREE); }
static void dma_free(void *p) { infallible_call(p, CALL_DMA_FREE); }
static void bar_unmap(void *p) { infallible_call(p, CALL_BAR_UNMAP); }
static int bar_restore(void *p) { return fallible_call(p,
	CALL_BAR_RESTORE, INJECT_BAR_RESTORE); }
static int pci_restore(void *p) { return fallible_call(p,
	CALL_PCI_RESTORE, INJECT_PCI_RESTORE); }
static void bar_release(void *p) { infallible_call(p, CALL_BAR_RELEASE); }

static const struct drv_nvme_lifecycle_ops operations = {
	.controller_disable = controller_disable,
	.bus_master_disable = master_disable,
	.irq_disestablish = irq_disestablish,
	.irq_drain = irq_drain,
	.irq_free = irq_free,
	.dma_free = dma_free,
	.bar_unmap = bar_unmap,
	.bar_restore = bar_restore,
	.pci_state_restore = pci_restore,
	.bar_release = bar_release,
};

static const enum drv_nvme_lifecycle_event attach_events[] = {
	DRV_NVME_LIFECYCLE_BAR_CLAIMED,
	DRV_NVME_LIFECYCLE_BAR_SNAPSHOTTED,
	DRV_NVME_LIFECYCLE_PCI_STATE_SAVED,
	DRV_NVME_LIFECYCLE_PCI_COMMAND_CHANGED,
	DRV_NVME_LIFECYCLE_BAR_MAPPED,
	DRV_NVME_LIFECYCLE_CONTROLLER_CLAIMED,
	DRV_NVME_LIFECYCLE_DMA_ALLOCATED,
	DRV_NVME_LIFECYCLE_IRQ_ALLOCATED,
	DRV_NVME_LIFECYCLE_IRQ_ESTABLISHED,
	DRV_NVME_LIFECYCLE_CONTROLLER_ENABLED
};

static enum call_index
injected_call(enum injected_action action);

static void
acquire_prefix(struct drv_nvme_lifecycle *lifecycle, unsigned count)
{
	unsigned index;

	drv_nvme_lifecycle_init(lifecycle);
	assert(count <= sizeof(attach_events) / sizeof(attach_events[0]));
	for (index = 0; index < count; index++)
		assert(drv_nvme_lifecycle_record(lifecycle,
		    attach_events[index]) == 0);
}

static void
assert_exactly_once(const struct fixture *fixture)
{
	unsigned index;

	for (index = 0; index < sizeof(fixture->calls) /
	    sizeof(fixture->calls[0]); index++)
		assert(fixture->calls[index] == 1U);
	for (index = 0; index < sizeof(fixture->sequence) /
	    sizeof(fixture->sequence[0]); index++)
		assert(fixture->sequence[index] == index);
}

static void
test_every_attach_stage(void)
{
	unsigned stage;

	for (stage = 0; stage <= sizeof(attach_events) /
	    sizeof(attach_events[0]); stage++) {
		struct drv_nvme_lifecycle lifecycle;
		struct fixture fixture;
		unsigned calls_before[10];

		memset(&fixture, 0, sizeof(fixture));
		acquire_prefix(&lifecycle, stage);
		assert(drv_nvme_lifecycle_cleanup(&lifecycle, &operations,
		    &fixture) == 0);
		assert(lifecycle.completed && !lifecycle.quarantined);
		assert(fixture.calls[CALL_CONTROLLER_DISABLE] ==
		    (stage >= 6U));
		assert(fixture.calls[CALL_MASTER_DISABLE] == (stage >= 6U));
		assert(fixture.calls[CALL_IRQ_DISESTABLISH] == (stage >= 9U));
		assert(fixture.calls[CALL_IRQ_DRAIN] == (stage >= 9U));
		assert(fixture.calls[CALL_IRQ_FREE] == (stage >= 8U));
		assert(fixture.calls[CALL_DMA_FREE] == (stage >= 7U));
		assert(fixture.calls[CALL_BAR_UNMAP] == (stage >= 5U));
		assert(fixture.calls[CALL_BAR_RESTORE] == (stage >= 2U));
		assert(fixture.calls[CALL_PCI_RESTORE] == (stage >= 4U));
		assert(fixture.calls[CALL_BAR_RELEASE] == (stage >= 1U));
		for (unsigned index = 1; index < fixture.sequence_count; index++)
			assert(fixture.sequence[index - 1U] <
			    fixture.sequence[index]);
		memcpy(calls_before, fixture.calls, sizeof(calls_before));
		assert(drv_nvme_lifecycle_cleanup(&lifecycle, &operations,
		    &fixture) == 0);
		assert(memcmp(calls_before, fixture.calls,
		    sizeof(calls_before)) == 0);
	}
}

static void
test_persistent_failure(enum injected_action action, int error)
{
	struct drv_nvme_lifecycle lifecycle;
	struct fixture fixture;
	enum call_index failed = injected_call(action);
	unsigned first_calls[10];
	unsigned index;

	memset(&fixture, 0, sizeof(fixture));
	fixture.injected = action;
	fixture.injected_error = error;
	acquire_prefix(&lifecycle,
	    sizeof(attach_events) / sizeof(attach_events[0]));
	assert(drv_nvme_lifecycle_cleanup(&lifecycle, &operations,
	    &fixture) == error);
	memcpy(first_calls, fixture.calls, sizeof(first_calls));
	fixture.sequence_count = 0;
	assert(drv_nvme_lifecycle_cleanup(&lifecycle, &operations,
	    &fixture) == error);
	assert(lifecycle.quarantined && !lifecycle.completed);
	assert(lifecycle.failure_count == 2U && lifecycle.last_error == error);
	for (index = 0; index < sizeof(fixture.calls) /
	    sizeof(fixture.calls[0]); index++) {
		if (index < (unsigned)failed)
			assert(fixture.calls[index] == first_calls[index]);
		else if (index == (unsigned)failed)
			assert(fixture.calls[index] == first_calls[index] + 1U);
		else
			assert(fixture.calls[index] == 0U);
	}
}

static void
test_full_success_order(void)
{
	struct drv_nvme_lifecycle lifecycle;
	struct fixture fixture;

	memset(&fixture, 0, sizeof(fixture));
	acquire_prefix(&lifecycle,
	    sizeof(attach_events) / sizeof(attach_events[0]));
	assert(drv_nvme_lifecycle_cleanup(&lifecycle, &operations,
	    &fixture) == 0);
	assert_exactly_once(&fixture);
}

static enum call_index
injected_call(enum injected_action action)
{
	switch (action) {
	case INJECT_CONTROLLER_DISABLE: return CALL_CONTROLLER_DISABLE;
	case INJECT_MASTER_DISABLE: return CALL_MASTER_DISABLE;
	case INJECT_IRQ_DISESTABLISH: return CALL_IRQ_DISESTABLISH;
	case INJECT_IRQ_DRAIN: return CALL_IRQ_DRAIN;
	case INJECT_BAR_RESTORE: return CALL_BAR_RESTORE;
	case INJECT_PCI_RESTORE: return CALL_PCI_RESTORE;
	case INJECT_NONE: break;
	}
	assert(0);
	return CALL_CONTROLLER_DISABLE;
}

static void
test_failure_then_retry(enum injected_action action, int error)
{
	struct drv_nvme_lifecycle lifecycle;
	struct fixture fixture;
	enum call_index failed = injected_call(action);
	unsigned index;

	memset(&fixture, 0, sizeof(fixture));
	fixture.injected = action;
	fixture.injected_error = error;
	acquire_prefix(&lifecycle,
	    sizeof(attach_events) / sizeof(attach_events[0]));
	assert(drv_nvme_lifecycle_cleanup(&lifecycle, &operations,
	    &fixture) == error);
	assert(!lifecycle.completed && lifecycle.quarantined);
	assert(lifecycle.last_error == error && lifecycle.failure_count == 1U);
	/* No operation after the failed boundary may have run. */
	for (index = (unsigned)failed + 1U;
	    index < sizeof(fixture.calls) / sizeof(fixture.calls[0]); index++)
		assert(fixture.calls[index] == 0U);

	fixture.injected = INJECT_NONE;
	fixture.sequence_count = 0;
	assert(drv_nvme_lifecycle_cleanup(&lifecycle, &operations,
	    &fixture) == 0);
	assert(lifecycle.completed && !lifecycle.quarantined);
	assert(lifecycle.failure_count == 1U && lifecycle.last_error == 0);
	/* The failed action is retried; every successful predecessor remains one. */
	for (index = 0; index < sizeof(fixture.calls) /
	    sizeof(fixture.calls[0]); index++)
		assert(fixture.calls[index] == (index == (unsigned)failed ? 2U : 1U));
}

static void
test_failures(void)
{
	test_failure_then_retry(INJECT_CONTROLLER_DISABLE, ETIMEDOUT);
	test_failure_then_retry(INJECT_MASTER_DISABLE, EIO);
	test_failure_then_retry(INJECT_IRQ_DISESTABLISH, EBUSY);
	test_failure_then_retry(INJECT_IRQ_DRAIN, EBUSY);
	test_failure_then_retry(INJECT_BAR_RESTORE, EIO);
	test_failure_then_retry(INJECT_PCI_RESTORE, EIO);
	test_persistent_failure(INJECT_CONTROLLER_DISABLE, ETIMEDOUT);
	test_persistent_failure(INJECT_MASTER_DISABLE, EIO);
	test_persistent_failure(INJECT_IRQ_DISESTABLISH, EBUSY);
	test_persistent_failure(INJECT_IRQ_DRAIN, EBUSY);
	test_persistent_failure(INJECT_BAR_RESTORE, EIO);
	test_persistent_failure(INJECT_PCI_RESTORE, EIO);
}

static void
test_record_contract(void)
{
	struct drv_nvme_lifecycle lifecycle;
	struct fixture fixture;
	struct drv_nvme_lifecycle_ops missing = operations;

	drv_nvme_lifecycle_init(&lifecycle);
	assert(drv_nvme_lifecycle_record(&lifecycle,
	    DRV_NVME_LIFECYCLE_DMA_ALLOCATED) == EINVAL);
	assert(drv_nvme_lifecycle_record(&lifecycle,
	    DRV_NVME_LIFECYCLE_BAR_CLAIMED) == 0);
	assert(drv_nvme_lifecycle_record(&lifecycle,
	    DRV_NVME_LIFECYCLE_BAR_CLAIMED) == EALREADY);

	memset(&fixture, 0, sizeof(fixture));
	acquire_prefix(&lifecycle,
	    sizeof(attach_events) / sizeof(attach_events[0]));
	missing.irq_drain = NULL;
	assert(drv_nvme_lifecycle_cleanup(&lifecycle, &missing,
	    &fixture) == EINVAL);
	assert(lifecycle.quarantined && lifecycle.irq_may_be_busy);
}

int
main(void)
{
	test_record_contract();
	test_every_attach_stage();
	test_full_success_order();
	test_failures();
	puts("HW-T20 NVMe cleanup lifecycle: PASS");
	return 0;
}
