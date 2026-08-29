/*
 * WS004 p023 NVMe I/O lifecycle fault-injection fixture.
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#include "../../../src/drivers/pci-nvme-io-lifecycle.h"

#include <errno.h>
#include <stdio.h>

static unsigned failures;

#define CHECK(expression) do { \
	if (!(expression)) { \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); \
		failures++; \
	} \
} while (0)

static void
test_split_and_flush(void)
{
	struct drv_nvme_io_lifecycle lifecycle;

	drv_nvme_io_lifecycle_init(&lifecycle);
	CHECK(drv_nvme_io_lifecycle_online(&lifecycle) == 0);
	CHECK(drv_nvme_io_lifecycle_begin_bio(&lifecycle) == 0);
	CHECK(drv_nvme_io_lifecycle_submit(&lifecycle, 1U, 1) == 0);
	CHECK(drv_nvme_io_lifecycle_submit(&lifecycle, 2U, 1) == EBUSY);
	CHECK(drv_nvme_io_lifecycle_complete_bio(&lifecycle) == EBUSY);
	CHECK(drv_nvme_io_lifecycle_complete_command(&lifecycle, 1U) == 0);
	CHECK(lifecycle.command_completion_count == 1U);
	CHECK(drv_nvme_io_lifecycle_submit(&lifecycle, 2U, 1) == 0);
	CHECK(drv_nvme_io_lifecycle_complete_command(&lifecycle, 2U) == 0);
	CHECK(drv_nvme_io_lifecycle_submit(&lifecycle, 3U, 1) == 0);
	CHECK(drv_nvme_io_lifecycle_complete_command(&lifecycle, 3U) == 0);
	CHECK(drv_nvme_io_lifecycle_complete_bio(&lifecycle) == 0);
	CHECK(lifecycle.bio_completion_count == 1U);
	CHECK(drv_nvme_io_lifecycle_complete_bio(&lifecycle) == EALREADY);

	/* Flush owns queue DMA but deliberately has no payload DMA. */
	CHECK(drv_nvme_io_lifecycle_begin_bio(&lifecycle) == 0);
	CHECK(drv_nvme_io_lifecycle_submit(&lifecycle, 4U, 0) == 0);
	CHECK(lifecycle.command_owned && !lifecycle.payload_dma_active);
	CHECK(drv_nvme_io_lifecycle_complete_command(&lifecycle, 4U) == 0);
	CHECK(drv_nvme_io_lifecycle_complete_bio(&lifecycle) == 0);
	CHECK(lifecycle.bio_completion_count == 2U);
}

static void
test_timeout_reset_recovery(void)
{
	struct drv_nvme_io_lifecycle lifecycle;

	drv_nvme_io_lifecycle_init(&lifecycle);
	CHECK(drv_nvme_io_lifecycle_online(&lifecycle) == 0);
	CHECK(drv_nvme_io_lifecycle_begin_bio(&lifecycle) == 0);
	CHECK(drv_nvme_io_lifecycle_submit(&lifecycle, 9U, 1) == 0);
	CHECK(drv_nvme_io_lifecycle_fault(&lifecycle) == 0);
	CHECK(!lifecycle.accepting && lifecycle.command_owned &&
	    lifecycle.payload_dma_active);
	/* The caller buffer is not a DMA target, so its BIO may fail now. */
	CHECK(drv_nvme_io_lifecycle_complete_bio(&lifecycle) == 0);
	CHECK(lifecycle.bio_completion_count == 1U);
	CHECK(drv_nvme_io_lifecycle_begin_bio(&lifecycle) == ENXIO);
	/* Bounce and queue memory remain owned until hardware is quiescent. */
	CHECK(drv_nvme_io_lifecycle_release(&lifecycle) == EBUSY);
	CHECK(drv_nvme_io_lifecycle_quiesced(&lifecycle) == 0);
	CHECK(!lifecycle.command_owned && !lifecycle.payload_dma_active);
	CHECK(drv_nvme_io_lifecycle_online(&lifecycle) == 0);
	CHECK(lifecycle.accepting && !lifecycle.faulted);

	CHECK(drv_nvme_io_lifecycle_begin_bio(&lifecycle) == 0);
	CHECK(drv_nvme_io_lifecycle_submit(&lifecycle, 10U, 1) == 0);
	CHECK(drv_nvme_io_lifecycle_complete_command(&lifecycle, 10U) == 0);
	CHECK(drv_nvme_io_lifecycle_complete_bio(&lifecycle) == 0);
	CHECK(lifecycle.bio_completion_count == 2U);
}

static void
test_foreign_completion_quarantine(void)
{
	struct drv_nvme_io_lifecycle lifecycle;

	drv_nvme_io_lifecycle_init(&lifecycle);
	CHECK(drv_nvme_io_lifecycle_online(&lifecycle) == 0);
	CHECK(drv_nvme_io_lifecycle_begin_bio(&lifecycle) == 0);
	CHECK(drv_nvme_io_lifecycle_submit(&lifecycle, 20U, 1) == 0);
	CHECK(drv_nvme_io_lifecycle_complete_command(&lifecycle, 21U) == EIO);
	CHECK(lifecycle.faulted && !lifecycle.accepting);
	CHECK(lifecycle.command_owned && lifecycle.payload_dma_active);
	CHECK(drv_nvme_io_lifecycle_complete_bio(&lifecycle) == 0);
	CHECK(drv_nvme_io_lifecycle_quarantine(&lifecycle) == 0);
	CHECK(drv_nvme_io_lifecycle_release(&lifecycle) == EBUSY);
	CHECK(lifecycle.payload_dma_active && !lifecycle.released);
	/* A late matching CQE cannot publish the BIO a second time. */
	CHECK(drv_nvme_io_lifecycle_complete_command(&lifecycle, 20U) == 0);
	CHECK(drv_nvme_io_lifecycle_complete_bio(&lifecycle) == EALREADY);
}

static void
test_quarantine_fresh_quiescence_retry(void)
{
	struct drv_nvme_io_lifecycle lifecycle;

	drv_nvme_io_lifecycle_init(&lifecycle);
	CHECK(drv_nvme_io_lifecycle_online(&lifecycle) == 0);
	CHECK(drv_nvme_io_lifecycle_begin_bio(&lifecycle) == 0);
	CHECK(drv_nvme_io_lifecycle_submit(&lifecycle, 24U, 1) == 0);
	CHECK(drv_nvme_io_lifecycle_fault(&lifecycle) == 0);
	CHECK(drv_nvme_io_lifecycle_quarantine(&lifecycle) == 0);
	CHECK(drv_nvme_io_lifecycle_resolve_quarantine(&lifecycle, 1, 1) ==
	    EBUSY);
	CHECK(lifecycle.quarantined && lifecycle.command_owned &&
	    lifecycle.payload_dma_active);
	CHECK(drv_nvme_io_lifecycle_complete_bio(&lifecycle) == 0);

	/* Neither an old quarantine nor either proof in isolation permits reuse. */
	CHECK(drv_nvme_io_lifecycle_resolve_quarantine(&lifecycle, 0, 0) ==
	    EBUSY);
	CHECK(drv_nvme_io_lifecycle_resolve_quarantine(&lifecycle, 1, 0) ==
	    EBUSY);
	CHECK(drv_nvme_io_lifecycle_resolve_quarantine(&lifecycle, 0, 1) ==
	    EBUSY);
	CHECK(lifecycle.quarantined && lifecycle.command_owned &&
	    lifecycle.payload_dma_active && !lifecycle.controller_quiesced);

	CHECK(drv_nvme_io_lifecycle_resolve_quarantine(&lifecycle, 1, 1) == 0);
	CHECK(!lifecycle.quarantined && !lifecycle.queues_online &&
	    !lifecycle.command_owned && !lifecycle.payload_dma_active &&
	    lifecycle.controller_quiesced);
	CHECK(drv_nvme_io_lifecycle_resolve_quarantine(&lifecycle, 1, 1) ==
	    EALREADY);
	CHECK(drv_nvme_io_lifecycle_release(&lifecycle) == 0);
	CHECK(drv_nvme_io_lifecycle_release(&lifecycle) == EALREADY);
	CHECK(drv_nvme_io_lifecycle_resolve_quarantine(&lifecycle, 1, 1) ==
	    EALREADY);
}

static void
test_shutdown_and_single_release(void)
{
	struct drv_nvme_io_lifecycle lifecycle;

	drv_nvme_io_lifecycle_init(&lifecycle);
	CHECK(drv_nvme_io_lifecycle_online(&lifecycle) == 0);
	CHECK(drv_nvme_io_lifecycle_begin_bio(&lifecycle) == 0);
	CHECK(drv_nvme_io_lifecycle_submit(&lifecycle, 30U, 1) == 0);
	CHECK(drv_nvme_io_lifecycle_stop(&lifecycle) == EBUSY);
	CHECK(!lifecycle.accepting);
	CHECK(drv_nvme_io_lifecycle_complete_command(&lifecycle, 30U) == 0);
	CHECK(drv_nvme_io_lifecycle_complete_bio(&lifecycle) == 0);
	CHECK(drv_nvme_io_lifecycle_stop(&lifecycle) == 0);
	CHECK(drv_nvme_io_lifecycle_quiesced(&lifecycle) == 0);
	CHECK(drv_nvme_io_lifecycle_release(&lifecycle) == 0);
	CHECK(drv_nvme_io_lifecycle_release(&lifecycle) == EALREADY);
	CHECK(drv_nvme_io_lifecycle_online(&lifecycle) == EINVAL);
	CHECK(drv_nvme_io_lifecycle_begin_bio(&lifecycle) == ENXIO);
}

static void
test_invalid_order(void)
{
	struct drv_nvme_io_lifecycle lifecycle;

	drv_nvme_io_lifecycle_init(&lifecycle);
	CHECK(drv_nvme_io_lifecycle_begin_bio(&lifecycle) == ENXIO);
	CHECK(drv_nvme_io_lifecycle_submit(&lifecycle, 0U, 1) == ENXIO);
	CHECK(drv_nvme_io_lifecycle_complete_command(&lifecycle, 0U) == ENOENT);
	CHECK(drv_nvme_io_lifecycle_complete_bio(&lifecycle) == EALREADY);
	CHECK(drv_nvme_io_lifecycle_quiesced(&lifecycle) == 0);
	CHECK(drv_nvme_io_lifecycle_online(&lifecycle) == 0);
	CHECK(drv_nvme_io_lifecycle_online(&lifecycle) == EINVAL);
	CHECK(drv_nvme_io_lifecycle_begin_bio(&lifecycle) == 0);
	CHECK(drv_nvme_io_lifecycle_submit(&lifecycle, 1U, 2) == EINVAL);
}

static void
test_out_of_order_slots_and_cid_reuse(void)
{
	struct drv_nvme_io_lifecycle slots[3];
	static const uint16_t command_ids[3] = {0xfffeU, 1U, 2U};
	static const unsigned completion_order[3] = {2U, 0U, 1U};
	unsigned index;

	for (index = 0; index < 3U; index++) {
		drv_nvme_io_lifecycle_init(&slots[index]);
		CHECK(drv_nvme_io_lifecycle_online(&slots[index]) == 0);
		CHECK(drv_nvme_io_lifecycle_begin_bio(&slots[index]) == 0);
		CHECK(drv_nvme_io_lifecycle_submit(&slots[index],
		    command_ids[index], 1) == 0);
	}
	for (index = 0; index < 3U; index++) {
		unsigned owner = completion_order[index];

		CHECK(drv_nvme_io_lifecycle_complete_command(&slots[owner],
		    command_ids[owner]) == 0);
		CHECK(drv_nvme_io_lifecycle_complete_bio(&slots[owner]) == 0);
	}
	for (index = 0; index < 3U; index++) {
		CHECK(slots[index].command_completion_count == 1U);
		CHECK(slots[index].bio_completion_count == 1U);
	}

	/* CID wrap/reuse is legal only after the previous owner has retired. */
	CHECK(drv_nvme_io_lifecycle_begin_bio(&slots[0]) == 0);
	CHECK(drv_nvme_io_lifecycle_submit(&slots[0], UINT16_MAX, 1) == 0);
	CHECK(drv_nvme_io_lifecycle_complete_command(&slots[0],
	    UINT16_MAX) == 0);
	CHECK(drv_nvme_io_lifecycle_complete_bio(&slots[0]) == 0);
	CHECK(drv_nvme_io_lifecycle_complete_command(&slots[0],
	    UINT16_MAX) == ENOENT);
	CHECK(drv_nvme_io_lifecycle_begin_bio(&slots[0]) == 0);
	CHECK(drv_nvme_io_lifecycle_submit(&slots[0], 1U, 1) == 0);
	CHECK(drv_nvme_io_lifecycle_complete_command(&slots[0], 1U) == 0);
	CHECK(drv_nvme_io_lifecycle_complete_bio(&slots[0]) == 0);
	CHECK(slots[0].command_completion_count == 3U);
	CHECK(slots[0].bio_completion_count == 3U);
}

static void
test_detach_flush_retry(void)
{
	struct drv_nvme_detach_flush_lifecycle lifecycle;

	drv_nvme_detach_flush_init(&lifecycle);
	CHECK(drv_nvme_detach_flush_begin(&lifecycle, 1) == EALREADY);
	CHECK(drv_nvme_detach_flush_require(&lifecycle) == 0);
	CHECK(lifecycle.required && !lifecycle.completed);
	CHECK(drv_nvme_detach_flush_begin(&lifecycle, 1) == 0);
	CHECK(lifecycle.attempts == 1U);
	CHECK(drv_nvme_detach_flush_finish(&lifecycle, EIO) == EIO);
	CHECK(lifecycle.required && !lifecycle.attempt_active &&
	    !lifecycle.completed);
	/* The already-gone disk still owns the durability obligation. */
	CHECK(drv_nvme_detach_flush_begin(&lifecycle, 1) == 0);
	CHECK(lifecycle.attempts == 2U);
	CHECK(drv_nvme_detach_flush_finish(&lifecycle, 0) == 0);
	CHECK(!lifecycle.required && lifecycle.completed);
	CHECK(drv_nvme_detach_flush_begin(&lifecycle, 1) == EALREADY);
}

static void
test_detach_flush_unavailable(void)
{
	struct drv_nvme_detach_flush_lifecycle lifecycle;

	drv_nvme_detach_flush_init(&lifecycle);
	CHECK(drv_nvme_detach_flush_require(&lifecycle) == 0);
	CHECK(drv_nvme_detach_flush_begin(&lifecycle, 0) == ENXIO);
	CHECK(lifecycle.required && lifecycle.unavailable &&
	    !lifecycle.attempt_active && !lifecycle.completed);
	/* No completion can be fabricated without an actual submitted command. */
	CHECK(drv_nvme_detach_flush_finish(&lifecycle, 0) == EINVAL);
	CHECK(lifecycle.required && !lifecycle.completed);
}

int
main(void)
{
	test_split_and_flush();
	test_timeout_reset_recovery();
	test_foreign_completion_quarantine();
	test_quarantine_fresh_quiescence_retry();
	test_shutdown_and_single_release();
	test_invalid_order();
	test_out_of_order_slots_and_cid_reuse();
	test_detach_flush_retry();
	test_detach_flush_unavailable();
	if (failures != 0U) {
		printf("HW-T20 NVMe I/O lifecycle: %u failure(s)\n", failures);
		return 1;
	}
	puts("HW-T20 NVMe I/O lifecycle: PASS");
	return 0;
}
