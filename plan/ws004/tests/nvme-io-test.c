/*
 * WS004 p023 NVMe I/O command and boundary fixture.
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#include <drivers/pci-nvme-protocol.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned failures;

#define CHECK(expression) do { \
	if (!(expression)) { \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); \
		failures++; \
	} \
} while (0)

static void
test_queue_commands(void)
{
	struct drv_nvme_command command;
	unsigned submission_count = 0, completion_count = 0;

	memset(&command, 0xa5, sizeof(command));
	CHECK(drv_nvme_set_queue_count_command(&command, 0x1234U, 1U, 1U));
	CHECK(command.cdw0 == UINT32_C(0x12340009));
	CHECK(command.namespace_id == 0U && command.prp1 == 0U);
	CHECK(command.cdw10 == DRV_NVME_FEATURE_NUMBER_OF_QUEUES);
	CHECK(command.cdw11 == 0U && command.cdw12 == 0U && command.cdw15 == 0U);
	CHECK(drv_nvme_set_queue_count_command(&command, 0U, 65536U,
	    65536U));
	CHECK(command.cdw11 == UINT32_MAX);
	CHECK(!drv_nvme_set_queue_count_command(&command, 0U, 0U, 1U));
	CHECK(!drv_nvme_set_queue_count_command(&command, 0U, 1U, 65537U));
	CHECK(!drv_nvme_set_queue_count_command(NULL, 0U, 1U, 1U));
	CHECK(drv_nvme_queue_count_result(UINT32_C(0x00020003),
	    &submission_count, &completion_count));
	CHECK(submission_count == 4U && completion_count == 3U);
	CHECK(drv_nvme_queue_count_result(UINT32_C(0xfffefffe),
	    &submission_count,
	    &completion_count));
	CHECK(submission_count == 65535U && completion_count == 65535U);
	CHECK(drv_nvme_queue_count_result(UINT32_MAX, &submission_count,
	    &completion_count));
	CHECK(submission_count == 65536U && completion_count == 65536U);
	CHECK(!drv_nvme_queue_count_result(0U, NULL, &completion_count));

	memset(&command, 0xa5, sizeof(command));
	CHECK(drv_nvme_create_io_cq_command(&command, 7U, 1U, 64U,
	    UINT64_C(0x12345000), 3U, 1));
	CHECK(command.cdw0 == UINT32_C(0x00070005));
	CHECK(command.namespace_id == 0U && command.prp1 ==
	    UINT64_C(0x12345000) && command.prp2 == 0U);
	CHECK(command.cdw10 == UINT32_C(0x003f0001));
	CHECK(command.cdw11 == UINT32_C(0x00030003));
	CHECK(command.cdw12 == 0U && command.cdw15 == 0U);
	CHECK(drv_nvme_create_io_cq_command(&command, 0U, UINT16_MAX,
	    65536U, UINT64_C(0x1000), UINT16_MAX, 0));
	CHECK(command.cdw10 == UINT32_MAX);
	CHECK(command.cdw11 == UINT32_C(0xffff0001));
	CHECK(!drv_nvme_create_io_cq_command(&command, 0U, 0U, 64U,
	    UINT64_C(0x1000), 0U, 1));
	CHECK(!drv_nvme_create_io_cq_command(&command, 0U, 1U, 1U,
	    UINT64_C(0x1000), 0U, 1));
	CHECK(!drv_nvme_create_io_cq_command(&command, 0U, 1U, 64U,
	    UINT64_C(0x1001), 0U, 1));
	CHECK(!drv_nvme_create_io_cq_command(&command, 0U, 1U, 64U,
	    UINT64_C(0x1000), 65536U, 1));
	CHECK(!drv_nvme_create_io_cq_command(&command, 0U, 1U, 64U,
	    UINT64_C(0x1000), 0U, 2));

	memset(&command, 0xa5, sizeof(command));
	CHECK(drv_nvme_create_io_sq_command(&command, 8U, 1U, 1U, 64U,
	    UINT64_C(0x56789000)));
	CHECK(command.cdw0 == UINT32_C(0x00080001));
	CHECK(command.namespace_id == 0U && command.prp1 ==
	    UINT64_C(0x56789000) && command.prp2 == 0U);
	CHECK(command.cdw10 == UINT32_C(0x003f0001));
	CHECK(command.cdw11 == UINT32_C(0x00010001));
	CHECK(command.cdw12 == 0U && command.cdw15 == 0U);
	CHECK(!drv_nvme_create_io_sq_command(&command, 0U, 1U, 0U, 64U,
	    UINT64_C(0x1000)));
	CHECK(!drv_nvme_create_io_sq_command(&command, 0U, 1U, 1U, 65537U,
	    UINT64_C(0x1000)));
	CHECK(!drv_nvme_create_io_sq_command(&command, 0U, 1U, 1U, 64U,
	    UINT64_C(0x1fff)));
}

static void
test_io_commands(void)
{
	struct drv_nvme_command command;
	const uint64_t lba = UINT64_C(0x1122334455667788);

	memset(&command, 0xa5, sizeof(command));
	CHECK(drv_nvme_io_command(&command, 9U, DRV_NVME_NVM_WRITE, 5U,
	    lba, 8U, UINT64_C(0x4000)));
	CHECK(command.cdw0 == UINT32_C(0x00090001));
	CHECK(command.namespace_id == 5U && command.metadata == 0U);
	CHECK(command.prp1 == UINT64_C(0x4000) && command.prp2 == 0U);
	CHECK(command.cdw10 == UINT32_C(0x55667788));
	CHECK(command.cdw11 == UINT32_C(0x11223344));
	CHECK(command.cdw12 == 7U && command.cdw13 == 0U &&
	    command.cdw15 == 0U);

	CHECK(drv_nvme_io_command(&command, 10U, DRV_NVME_NVM_READ, 5U,
	    0U, 65536U, UINT64_C(0x8000)));
	CHECK(command.cdw0 == UINT32_C(0x000a0002));
	CHECK(command.cdw12 == UINT32_C(0xffff));
	CHECK(drv_nvme_io_command(&command, 11U, DRV_NVME_NVM_FLUSH, 5U,
	    0U, 0U, 0U));
	CHECK(command.cdw0 == UINT32_C(0x000b0000));
	CHECK(command.namespace_id == 5U && command.prp1 == 0U &&
	    command.cdw10 == 0U && command.cdw12 == 0U);

	CHECK(!drv_nvme_io_command(&command, 0U, 0xffU, 5U, 0U, 1U,
	    UINT64_C(0x1000)));
	CHECK(!drv_nvme_io_command(&command, 0U, DRV_NVME_NVM_READ, 0U,
	    0U, 1U, UINT64_C(0x1000)));
	CHECK(!drv_nvme_io_command(&command, 0U, DRV_NVME_NVM_READ,
	    UINT32_MAX, 0U, 1U, UINT64_C(0x1000)));
	CHECK(!drv_nvme_io_command(&command, 0U, DRV_NVME_NVM_READ, 1U,
	    0U, 0U, UINT64_C(0x1000)));
	CHECK(!drv_nvme_io_command(&command, 0U, DRV_NVME_NVM_WRITE, 1U,
	    0U, 65537U, UINT64_C(0x1000)));
	CHECK(!drv_nvme_io_command(&command, 0U, DRV_NVME_NVM_WRITE, 1U,
	    0U, 1U, 0U));
	CHECK(!drv_nvme_io_command(&command, 0U, DRV_NVME_NVM_FLUSH, 1U,
	    1U, 0U, 0U));
	CHECK(!drv_nvme_io_command(NULL, 0U, DRV_NVME_NVM_FLUSH, 1U,
	    0U, 0U, 0U));
}

static void
test_transfer_boundaries(void)
{
	CHECK(drv_nvme_io_range_valid(0U, 1U, 1U));
	CHECK(drv_nvme_io_range_valid(1U, 9U, 10U));
	CHECK(drv_nvme_io_range_valid(UINT64_MAX - 7U, 7U, UINT64_MAX));
	CHECK(!drv_nvme_io_range_valid(0U, 0U, 1U));
	CHECK(!drv_nvme_io_range_valid(1U, 1U, 1U));
	CHECK(!drv_nvme_io_range_valid(9U, 2U, 10U));
	CHECK(!drv_nvme_io_range_valid(UINT64_MAX - 7U, 8U,
	    UINT64_MAX));

	CHECK(drv_nvme_single_prp_transfer_valid(UINT64_C(0x1000),
	    512U, 8U, 4096U));
	CHECK(drv_nvme_single_prp_transfer_valid(UINT64_C(0x1100),
	    512U, 7U, 4096U));
	CHECK(!drv_nvme_single_prp_transfer_valid(UINT64_C(0x1100),
	    512U, 8U, 4096U));
	CHECK(!drv_nvme_single_prp_transfer_valid(UINT64_C(0x1002),
	    512U, 1U, 4096U));
	CHECK(!drv_nvme_single_prp_transfer_valid(0U, 512U, 1U, 4096U));
	CHECK(!drv_nvme_single_prp_transfer_valid(UINT64_C(0x1000),
	    512U, 1U, 6144U));
	CHECK(drv_nvme_single_prp_transfer_valid(UINT64_MAX - 3U,
	    4U, 1U, 4096U));
	CHECK(!drv_nvme_single_prp_transfer_valid(UINT64_MAX - 3U,
	    4U, 2U, 4096U));

	CHECK(drv_nvme_io_chunk_blocks(17U, 512U, 4096U, 128U * 1024U) ==
	    8U);
	CHECK(drv_nvme_io_chunk_blocks(3U, 512U, 4096U, 128U * 1024U) ==
	    3U);
	CHECK(drv_nvme_io_chunk_blocks(17U, 512U, 4096U, 1024U) == 2U);
	CHECK(drv_nvme_io_chunk_blocks(UINT64_MAX, 1U, SIZE_MAX, SIZE_MAX) ==
	    65536U);
	CHECK(drv_nvme_io_chunk_blocks(1U, 4096U, 4096U, 4096U) == 1U);
	CHECK(drv_nvme_io_chunk_blocks(1U, 4096U, 4095U, 4096U) == 0U);
	CHECK(drv_nvme_io_chunk_blocks(0U, 512U, 4096U, 4096U) == 0U);
}

static uint16_t
completion_status(unsigned type, unsigned code)
{
	return (uint16_t)(1U | ((code & 0xffU) << 1) |
	    ((type & 7U) << 9));
}

static void
test_completion_translation(void)
{
	struct drv_nvme_completion completion;

	memset(&completion, 0, sizeof(completion));
	completion.status = completion_status(DRV_NVME_STATUS_GENERIC, 0x00U);
	CHECK(drv_nvme_completion_error(&completion) == 0);
	completion.status = completion_status(DRV_NVME_STATUS_GENERIC, 0x01U);
	CHECK(drv_nvme_completion_error(&completion) == EOPNOTSUPP);
	completion.status = completion_status(DRV_NVME_STATUS_GENERIC, 0x02U);
	CHECK(drv_nvme_completion_error(&completion) == EINVAL);
	completion.status = completion_status(DRV_NVME_STATUS_GENERIC, 0x07U);
	CHECK(drv_nvme_completion_error(&completion) == ECANCELED);
	completion.status = completion_status(DRV_NVME_STATUS_GENERIC, 0x0bU);
	CHECK(drv_nvme_completion_error(&completion) == ENXIO);
	completion.status = completion_status(DRV_NVME_STATUS_GENERIC, 0x20U);
	CHECK(drv_nvme_completion_error(&completion) == EROFS);
	completion.status = completion_status(DRV_NVME_STATUS_GENERIC, 0x80U);
	CHECK(drv_nvme_completion_error(&completion) == EOVERFLOW);
	completion.status = completion_status(DRV_NVME_STATUS_GENERIC, 0x81U);
	CHECK(drv_nvme_completion_error(&completion) == ENOSPC);
	completion.status = completion_status(DRV_NVME_STATUS_GENERIC, 0x82U);
	CHECK(drv_nvme_completion_error(&completion) == EBUSY);
	completion.status = completion_status(DRV_NVME_STATUS_COMMAND_SPECIFIC,
	    0x82U);
	CHECK(drv_nvme_completion_error(&completion) == EROFS);
	completion.status = completion_status(DRV_NVME_STATUS_COMMAND_SPECIFIC,
	    0x83U);
	CHECK(drv_nvme_completion_error(&completion) == EOVERFLOW);
	completion.status = completion_status(DRV_NVME_STATUS_MEDIA, 0x80U);
	CHECK(drv_nvme_completion_error(&completion) == EIO);
	completion.status = completion_status(DRV_NVME_STATUS_PATH, 0x00U);
	CHECK(drv_nvme_completion_error(&completion) == EIO);
	CHECK(drv_nvme_completion_error(NULL) == EINVAL);

	/* A serialized queue treats a completion for another CID as a fault. */
	completion.status = completion_status(DRV_NVME_STATUS_GENERIC, 0U);
	completion.submission_id = 1U;
	completion.submission_head = 7U;
	completion.command_id = 12U;
	CHECK(!drv_nvme_completion_matches(&completion, 1U, 1U, 7U, 11U));
	CHECK(drv_nvme_completion_matches(&completion, 1U, 1U, 7U, 12U));
	completion.submission_id = 0U;
	CHECK(!drv_nvme_completion_matches(&completion, 1U, 1U, 7U, 12U));
}

static void
test_queue_wrap_and_phase(void)
{
	struct drv_nvme_completion_cursor cursor;
	struct drv_nvme_completion completion;
	unsigned index;

	CHECK(drv_nvme_completion_cursor_init(&cursor, 4U));
	CHECK(cursor.head == 0U && cursor.depth == 4U && cursor.phase == 1U);
	memset(&completion, 0, sizeof(completion));
	completion.status = 1U;
	for (index = 0; index < 4U; index++) {
		CHECK(drv_nvme_completion_available(&completion, cursor.phase));
		CHECK(cursor.head == index && cursor.phase == 1U);
		drv_nvme_completion_cursor_advance(&cursor);
	}
	CHECK(cursor.head == 0U && cursor.phase == 0U);
	CHECK(!drv_nvme_completion_available(&completion, cursor.phase));
	completion.status = 0U;
	CHECK(drv_nvme_completion_available(&completion, cursor.phase));
	drv_nvme_completion_cursor_advance(&cursor);
	CHECK(cursor.head == 1U && cursor.phase == 0U);

	/* A maximum-sized CQ has 65,536 entries although its head is 16-bit. */
	CHECK(drv_nvme_completion_cursor_init(&cursor, 65536U));
	cursor.head = UINT16_MAX;
	cursor.phase = 1U;
	drv_nvme_completion_cursor_advance(&cursor);
	CHECK(cursor.head == 0U && cursor.phase == 0U);
	CHECK(!drv_nvme_completion_cursor_init(&cursor, 65537U));
}

int
main(void)
{
	test_queue_commands();
	test_io_commands();
	test_transfer_boundaries();
	test_completion_translation();
	test_queue_wrap_and_phase();
	if (failures != 0U) {
		printf("HW-T20 NVMe I/O helper: %u failure(s)\n", failures);
		return 1;
	}
	puts("HW-T20 NVMe I/O helper: PASS");
	return 0;
}
