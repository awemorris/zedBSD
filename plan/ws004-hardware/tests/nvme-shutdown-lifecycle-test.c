/*
 * WS004 p023 NVMe terminal-shutdown fault-injection fixture.
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#include "../../../src/drivers/pci-nvme-shutdown-lifecycle.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static unsigned failures;

#define CHECK(expression) do { \
	if (!(expression)) { \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); \
		failures++; \
	} \
} while (0)

enum shutdown_step {
	STEP_ADMISSION,
	STEP_SHUTDOWN,
	STEP_DISABLE,
	STEP_MASTER,
	STEP_COUNT
};

struct fixture {
	int errors[STEP_COUNT];
	unsigned permanent_admin_pending;
	unsigned permanent_probe_busy;
	unsigned calls[STEP_COUNT];
	unsigned sequence[STEP_COUNT];
	unsigned sequence_count;
};

static int
step_call(struct fixture *fixture, enum shutdown_step step)
{
	CHECK(fixture->sequence_count < STEP_COUNT);
	if (fixture->sequence_count < STEP_COUNT)
		fixture->sequence[fixture->sequence_count++] = (unsigned)step;
	fixture->calls[step]++;
	return fixture->errors[step];
}

static int
stop_admission(void *context)
{
	struct fixture *fixture = context;
	int error = step_call(fixture, STEP_ADMISSION);

	if (error != 0)
		return error;
	return fixture->permanent_admin_pending ||
	    fixture->permanent_probe_busy ? EBUSY : 0;
}
static int shutdown_normal(void *context)
{ return step_call(context, STEP_SHUTDOWN); }
static int controller_disable(void *context)
{ return step_call(context, STEP_DISABLE); }
static int bus_master_disable(void *context)
{ return step_call(context, STEP_MASTER); }

static const struct drv_nvme_shutdown_ops operations = {
	.stop_admission = stop_admission,
	.shutdown_normal = shutdown_normal,
	.controller_disable = controller_disable,
	.bus_master_disable = bus_master_disable,
};

static void
check_all_steps_once(const struct fixture *fixture)
{
	unsigned index;

	CHECK(fixture->sequence_count == STEP_COUNT);
	for (index = 0; index < STEP_COUNT; index++) {
		CHECK(fixture->calls[index] == 1U);
		CHECK(fixture->sequence[index] == index);
	}
}

static void
test_success_and_idempotence(void)
{
	struct drv_nvme_shutdown_lifecycle lifecycle;
	struct fixture fixture;
	unsigned calls_before[STEP_COUNT];

	memset(&fixture, 0, sizeof(fixture));
	drv_nvme_shutdown_lifecycle_init(&lifecycle);
	CHECK(drv_nvme_shutdown_lifecycle_run(&lifecycle, &operations,
	    &fixture) == 0);
	check_all_steps_once(&fixture);
	CHECK(lifecycle.completed && !lifecycle.running &&
	    lifecycle.admission_stopped && lifecycle.shutdown_completed &&
	    lifecycle.controller_disabled && lifecycle.master_disabled &&
	    lifecycle.hardware_dma_safe && lifecycle.failure_count == 0U);
	memcpy(calls_before, fixture.calls, sizeof(calls_before));
	CHECK(drv_nvme_shutdown_lifecycle_run(&lifecycle, &operations,
	    &fixture) == 0);
	CHECK(memcmp(calls_before, fixture.calls, sizeof(calls_before)) == 0);
}

static void
test_each_failure_continues(void)
{
	static const int injected[STEP_COUNT] = {EBUSY, ETIMEDOUT, EIO, EACCES};
	unsigned failed_step;

	for (failed_step = 0; failed_step < STEP_COUNT; failed_step++) {
		struct drv_nvme_shutdown_lifecycle lifecycle;
		struct fixture fixture;

		memset(&fixture, 0, sizeof(fixture));
		fixture.errors[failed_step] = injected[failed_step];
		drv_nvme_shutdown_lifecycle_init(&lifecycle);
		CHECK(drv_nvme_shutdown_lifecycle_run(&lifecycle, &operations,
		    &fixture) == injected[failed_step]);
		check_all_steps_once(&fixture);
		CHECK(lifecycle.completed && lifecycle.failure_count == 1U &&
		    lifecycle.first_error == injected[failed_step]);
		CHECK(lifecycle.admission_stopped == (failed_step != STEP_ADMISSION));
		CHECK(lifecycle.shutdown_completed == (failed_step != STEP_SHUTDOWN));
		CHECK(lifecycle.controller_disabled == (failed_step != STEP_DISABLE));
		CHECK(lifecycle.master_disabled == (failed_step != STEP_MASTER));
		/* Either hard boundary succeeds when only one operation fails. */
		CHECK(lifecycle.hardware_dma_safe);
	}
}

static void
test_both_hard_boundaries_fail(void)
{
	struct drv_nvme_shutdown_lifecycle lifecycle;
	struct fixture fixture;

	memset(&fixture, 0, sizeof(fixture));
	fixture.errors[STEP_ADMISSION] = EBUSY;
	fixture.errors[STEP_SHUTDOWN] = ETIMEDOUT;
	fixture.errors[STEP_DISABLE] = EIO;
	fixture.errors[STEP_MASTER] = EACCES;
	drv_nvme_shutdown_lifecycle_init(&lifecycle);
	CHECK(drv_nvme_shutdown_lifecycle_run(&lifecycle, &operations,
	    &fixture) == EBUSY);
	check_all_steps_once(&fixture);
	CHECK(lifecycle.failure_count == STEP_COUNT &&
	    lifecycle.admission_error == EBUSY &&
	    lifecycle.shutdown_error == ETIMEDOUT &&
	    lifecycle.disable_error == EIO && lifecycle.master_error == EACCES &&
	    !lifecycle.controller_disabled && !lifecycle.master_disabled &&
	    !lifecycle.hardware_dma_safe);
}

static void
test_terminal_admin_probe_busy_does_not_gate_safety(void)
{
	unsigned scenario;

	/* Production's terminal-only claim owns detach first, but a permanently
	 * pending admin command or namespace probe is reported by admission drain
	 * rather than being allowed to gate the hardware stop sequence. */
	for (scenario = 0; scenario < 2U; scenario++) {
		struct drv_nvme_shutdown_lifecycle lifecycle;
		struct fixture fixture;

		memset(&fixture, 0, sizeof(fixture));
		fixture.permanent_admin_pending = scenario == 0U;
		fixture.permanent_probe_busy = scenario == 1U;
		drv_nvme_shutdown_lifecycle_init(&lifecycle);
		CHECK(drv_nvme_shutdown_lifecycle_run(&lifecycle, &operations,
		    &fixture) == EBUSY);
		check_all_steps_once(&fixture);
		CHECK(lifecycle.admission_attempted &&
		    !lifecycle.admission_stopped);
		CHECK(lifecycle.shutdown_attempted &&
		    lifecycle.shutdown_completed);
		CHECK(lifecycle.disable_attempted &&
		    lifecycle.controller_disabled);
		CHECK(lifecycle.master_attempted && lifecycle.master_disabled);
		CHECK(lifecycle.hardware_dma_safe);
	}
}

static void
test_invalid_contract(void)
{
	struct drv_nvme_shutdown_lifecycle lifecycle;
	struct drv_nvme_shutdown_ops missing = operations;
	struct fixture fixture;

	memset(&fixture, 0, sizeof(fixture));
	drv_nvme_shutdown_lifecycle_init(&lifecycle);
	missing.bus_master_disable = NULL;
	CHECK(drv_nvme_shutdown_lifecycle_run(NULL, &operations, &fixture) ==
	    EINVAL);
	CHECK(drv_nvme_shutdown_lifecycle_run(&lifecycle, NULL, &fixture) ==
	    EINVAL);
	CHECK(drv_nvme_shutdown_lifecycle_run(&lifecycle, &missing, &fixture) ==
	    EINVAL);
	CHECK(fixture.sequence_count == 0U && !lifecycle.completed);
}

int
main(void)
{
	test_invalid_contract();
	test_success_and_idempotence();
	test_each_failure_continues();
	test_both_hard_boundaries_fail();
	test_terminal_admin_probe_busy_does_not_gate_safety();
	if (failures != 0U) {
		printf("HW-T20 NVMe shutdown lifecycle: %u failure(s)\n", failures);
		return 1;
	}
	puts("HW-T20 NVMe shutdown lifecycle: PASS");
	return 0;
}
