/*
 * WS004 p022 NVMe protocol and Identify fixture.
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#include <drivers/pci-nvme-protocol.h>

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
put_le32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

static void
put_le64(uint8_t *bytes, uint64_t value)
{
	put_le32(bytes, (uint32_t)value);
	put_le32(bytes + 4U, (uint32_t)(value >> 32));
}

static uint64_t
valid_capability(void)
{
	return UINT64_C(63) | (UINT64_C(1) << 16) |
	    (UINT64_C(1) << 24) | (UINT64_C(1) << 37);
}

static void
test_capability(void)
{
	struct drv_nvme_capability_snapshot snapshot = {
		.mapping_size = 0x2000U,
		.capability = valid_capability(),
		.version = 0x00010400U,
		.page_size = 4096U,
		.requested_queue_entries = 64U,
		.maximum_queue_id = 1U
	};
	uint32_t reasons;
	size_t offset = 0, bytes = 0;

	CHECK(drv_nvme_capability_validate(&snapshot) == 0U);
	CHECK(drv_nvme_cap_queue_entries(snapshot.capability) == 64U);
	CHECK(drv_nvme_selected_queue_depth(snapshot.capability, 128U) == 64U);
	CHECK(drv_nvme_cap_timeout_ms(snapshot.capability) == 500U);
	CHECK(drv_nvme_doorbell_stride(snapshot.capability) == 4U);
	CHECK(drv_nvme_doorbell_offset(snapshot.capability, 0U, 0,
	    snapshot.mapping_size, &offset) && offset == 0x1000U);
	CHECK(drv_nvme_doorbell_offset(snapshot.capability, 1U, 1,
	    snapshot.mapping_size, &offset) && offset == 0x100cU);
	CHECK(!drv_nvme_doorbell_offset(snapshot.capability, 1U, 1,
	    0x100fU, &offset));
	CHECK(drv_nvme_controller_configuration(4096U, 1) == 0x00460001U);
	CHECK(drv_nvme_controller_configuration(8192U, 0) == 0x00460080U);
	CHECK(drv_nvme_controller_configuration(6144U, 1) == 0U);
	CHECK(drv_nvme_admin_queue_attributes(64U) == 0x003f003fU);
	CHECK(drv_nvme_admin_queue_attributes(1U) == 0U);
	CHECK(drv_nvme_admin_queue_attributes(4097U) == 0U);
	CHECK(drv_nvme_queue_bytes(64U, DRV_NVME_ADMIN_SQE_SIZE, &bytes) &&
	    bytes == 4096U);
	CHECK(!drv_nvme_queue_bytes(1U, DRV_NVME_ADMIN_SQE_SIZE, &bytes));
	CHECK(drv_nvme_controller_ready_state(0U, 0) ==
	    DRV_NVME_READY_MATCH);
	CHECK(drv_nvme_controller_ready_state(DRV_NVME_CSTS_READY, 1) ==
	    DRV_NVME_READY_MATCH);
	CHECK(drv_nvme_controller_ready_state(DRV_NVME_CSTS_FATAL, 1) ==
	    DRV_NVME_READY_FATAL);
	CHECK(drv_nvme_controller_ready_state(UINT32_MAX, 1) ==
	    DRV_NVME_READY_UNREACHABLE);
	CHECK(drv_nvme_controller_disable_state(DRV_NVME_CSTS_READY |
	    DRV_NVME_CSTS_FATAL) == DRV_NVME_READY_WAIT);
	CHECK(drv_nvme_controller_disable_state(DRV_NVME_CSTS_READY) ==
	    DRV_NVME_READY_WAIT);
	CHECK(drv_nvme_controller_disable_state(DRV_NVME_CSTS_FATAL) ==
	    DRV_NVME_READY_MATCH);
	CHECK(drv_nvme_controller_disable_state(0U) == DRV_NVME_READY_MATCH);
	CHECK(drv_nvme_controller_disable_state(UINT32_MAX) ==
	    DRV_NVME_READY_UNREACHABLE);

	snapshot.capability &= ~(UINT64_C(1) << 37);
	reasons = drv_nvme_capability_validate(&snapshot);
	CHECK((reasons & DRV_NVME_CAP_COMMAND_SET) != 0U);
	snapshot.capability = valid_capability() | (UINT64_C(1) << 48) |
	    (UINT64_C(1) << 52);
	reasons = drv_nvme_capability_validate(&snapshot);
	CHECK((reasons & DRV_NVME_CAP_PAGE_SIZE) != 0U);
	snapshot.capability = valid_capability();
	snapshot.version = 0U;
	CHECK((drv_nvme_capability_validate(&snapshot) &
	    DRV_NVME_CAP_VERSION) != 0U);
	snapshot.version = 0x00010400U;
	snapshot.requested_queue_entries = 1U;
	CHECK((drv_nvme_capability_validate(&snapshot) &
	    DRV_NVME_CAP_QUEUE_DEPTH) != 0U);
	snapshot.requested_queue_entries = 64U;
	snapshot.mapping_size = 0x100fU;
	CHECK((drv_nvme_capability_validate(&snapshot) &
	    DRV_NVME_CAP_DOORBELL_EXTENT) != 0U);
	CHECK(strcmp(drv_nvme_capability_reason_name(
	    DRV_NVME_CAP_DOORBELL_EXTENT), "doorbell-extent") == 0);
	CHECK(drv_nvme_capability_validate(NULL) ==
	    DRV_NVME_CAP_MAPPING_SHORT);
}

static void
test_command_and_completion(void)
{
	struct drv_nvme_command command;
	struct drv_nvme_completion completion;
	struct drv_nvme_completion_cursor cursor;

	memset(&command, 0xa5, sizeof(command));
	CHECK(drv_nvme_identify_command(&command, 7U, 0U,
	    DRV_NVME_IDENTIFY_CONTROLLER, UINT64_C(0x12345000)));
	CHECK(command.cdw0 == 0x00070006U && command.namespace_id == 0U &&
	    command.prp1 == UINT64_C(0x12345000) && command.prp2 == 0U &&
	    command.cdw10 == DRV_NVME_IDENTIFY_CONTROLLER && command.cdw15 == 0U);
	CHECK(drv_nvme_identify_command(&command, 8U, 3U,
	    DRV_NVME_IDENTIFY_NAMESPACE, UINT64_C(0x2000)));
	CHECK(command.cdw0 == 0x00080006U && command.namespace_id == 3U &&
	    command.cdw10 == DRV_NVME_IDENTIFY_NAMESPACE);
	CHECK(!drv_nvme_identify_command(&command, 0U, 0U,
	    DRV_NVME_IDENTIFY_NAMESPACE, UINT64_C(0x2000)));
	CHECK(!drv_nvme_identify_command(&command, 0U, 1U,
	    DRV_NVME_IDENTIFY_CONTROLLER, UINT64_C(0x2000)));
	CHECK(!drv_nvme_identify_command(&command, 0U, 0U,
	    DRV_NVME_IDENTIFY_CONTROLLER, 0U));
	CHECK(!drv_nvme_identify_command(&command, 9U, 0U,
	    DRV_NVME_IDENTIFY_CONTROLLER, UINT64_C(0x2001)));

	memset(&completion, 0, sizeof(completion));
	completion.status = 1U;
	completion.submission_head = 3U;
	completion.command_id = 7U;
	CHECK(drv_nvme_completion_available(&completion, 1U));
	CHECK(!drv_nvme_completion_available(&completion, 0U));
	CHECK(drv_nvme_completion_matches(&completion, 1U, 0U, 3U, 7U));
	CHECK(!drv_nvme_completion_matches(&completion, 0U, 0U, 3U, 7U));
	CHECK(!drv_nvme_completion_matches(&completion, 1U, 1U, 3U, 7U));
	CHECK(!drv_nvme_completion_matches(&completion, 1U, 0U, 2U, 7U));
	CHECK(!drv_nvme_completion_matches(&completion, 1U, 0U, 3U, 8U));
	CHECK(drv_nvme_completion_success(&completion));
	completion.status = (uint16_t)(1U | (2U << 1) | (1U << 9));
	CHECK(drv_nvme_completion_status_code(&completion) == 2U);
	CHECK(drv_nvme_completion_status_type(&completion) == 1U);
	CHECK(!drv_nvme_completion_success(&completion));

	CHECK(drv_nvme_completion_cursor_init(&cursor, 4U));
	CHECK(cursor.head == 0U && cursor.phase == 1U);
	drv_nvme_completion_cursor_advance(&cursor);
	drv_nvme_completion_cursor_advance(&cursor);
	drv_nvme_completion_cursor_advance(&cursor);
	CHECK(cursor.head == 3U && cursor.phase == 1U);
	drv_nvme_completion_cursor_advance(&cursor);
	CHECK(cursor.head == 0U && cursor.phase == 0U);
	CHECK(!drv_nvme_completion_cursor_init(&cursor, 1U));
	CHECK(drv_nvme_queue_index_advance(3U, 4U) == 0U);
	CHECK(drv_nvme_queue_index_advance(4U, 4U) == UINT32_MAX);
}

static void
test_identify(void)
{
	uint8_t controller[DRV_NVME_IDENTIFY_SIZE];
	uint8_t namespace_data[DRV_NVME_IDENTIFY_SIZE];
	uint8_t active[DRV_NVME_IDENTIFY_SIZE];
	struct drv_nvme_controller_profile controller_profile;
	struct drv_nvme_namespace_profile namespace_profile;
	uint32_t namespace_id = 0, reasons;

	memset(controller, 0, sizeof(controller));
	controller[77U] = 5U;
	controller[512U] = 0x66U;
	controller[513U] = 0x44U;
	put_le32(controller + 516U, 1U);
	CHECK(drv_nvme_identify_controller_validate(controller,
	    sizeof(controller), &controller_profile) == 0U);
	CHECK(controller_profile.namespace_count == 1U &&
	    controller_profile.maximum_transfer_shift == 5U);
	controller[512U] = 0x77U;
	CHECK((drv_nvme_identify_controller_validate(controller,
	    sizeof(controller), NULL) & DRV_NVME_ID_CTRL_SQ_ENTRY_SIZE) != 0U);
	controller[512U] = 0x66U;
	put_le32(controller + 516U, 0U);
	CHECK((drv_nvme_identify_controller_validate(controller,
	    sizeof(controller), NULL) & DRV_NVME_ID_CTRL_NO_NAMESPACES) != 0U);
	CHECK(drv_nvme_identify_controller_validate(controller, 512U, NULL) ==
	    DRV_NVME_ID_CTRL_BUFFER_SHORT);

	memset(active, 0, sizeof(active));
	/* NN is a count: a single active namespace may have a sparse NSID. */
	put_le32(active, 5U);
	CHECK(drv_nvme_active_namespace_validate(active, sizeof(active), 1U,
	    &namespace_id) == 0U && namespace_id == 5U);
	put_le32(active + 4U, 9U);
	reasons = drv_nvme_active_namespace_validate(active, sizeof(active), 1U,
	    &namespace_id);
	CHECK((reasons & DRV_NVME_ACTIVE_MULTIPLE) != 0U && namespace_id == 0U);
	memset(active, 0, sizeof(active));
	put_le32(active, UINT32_MAX);
	CHECK((drv_nvme_active_namespace_validate(active, sizeof(active), 1U,
	    NULL) & DRV_NVME_ACTIVE_RANGE) != 0U);
	memset(active, 0, sizeof(active));
	put_le32(active, UINT32_C(0xfffffffe));
	CHECK(drv_nvme_active_namespace_validate(active, sizeof(active), 1U,
	    &namespace_id) == 0U && namespace_id == UINT32_C(0xfffffffe));
	memset(active, 0, sizeof(active));
	CHECK((drv_nvme_active_namespace_validate(active, sizeof(active), 1U,
	    NULL) & DRV_NVME_ACTIVE_NONE) != 0U);

	memset(namespace_data, 0, sizeof(namespace_data));
	put_le64(namespace_data, UINT64_C(0x100000020));
	put_le64(namespace_data + 8U, UINT64_C(0x100000020));
	put_le64(namespace_data + 16U, UINT64_C(0x100));
	namespace_data[25U] = 0U;
	namespace_data[26U] = 0U;
	namespace_data[128U + 2U] = 9U;
	CHECK(drv_nvme_identify_namespace_validate(namespace_data,
	    sizeof(namespace_data), &namespace_profile) == 0U);
	CHECK(namespace_profile.block_count == UINT64_C(0x100000020) &&
	    namespace_profile.capacity_blocks == UINT64_C(0x100000020) &&
	    namespace_profile.used_blocks == UINT64_C(0x100) &&
	    namespace_profile.block_size_shift == 9U);
	CHECK(drv_nvme_max_transfer_bytes(5U, 4096U, 16U * 1024U * 1024U) ==
	    128U * 1024U);
	CHECK(drv_nvme_max_transfer_bytes(0U, 4096U, 128U * 1024U) ==
	    128U * 1024U);

	namespace_data[128U] = 8U;
	CHECK((drv_nvme_identify_namespace_validate(namespace_data,
	    sizeof(namespace_data), NULL) & DRV_NVME_ID_NS_METADATA) != 0U);
	namespace_data[128U] = 0U;
	namespace_data[130U] = 12U;
	CHECK((drv_nvme_identify_namespace_validate(namespace_data,
	    sizeof(namespace_data), NULL) & DRV_NVME_ID_NS_BLOCK_SIZE) != 0U);
	namespace_data[130U] = 9U;
	put_le64(namespace_data + 8U, UINT64_C(0x100000021));
	CHECK((drv_nvme_identify_namespace_validate(namespace_data,
	    sizeof(namespace_data), NULL) & DRV_NVME_ID_NS_CAPACITY) != 0U);
	CHECK(drv_nvme_identify_namespace_validate(namespace_data, 128U, NULL) ==
	    DRV_NVME_ID_NS_BUFFER_SHORT);
}

int
main(void)
{
	test_capability();
	test_command_and_completion();
	test_identify();
	if (failures != 0U) {
		printf("HW-T20 NVMe admin helper: %u failure(s)\n", failures);
		return 1;
	}
	puts("HW-T20 NVMe admin helper: PASS");
	return 0;
}
