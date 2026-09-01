/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * Intel AX211 logical-command/transport integration fixture
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../../src/drivers/intel-ax211-command.h"

#define TEST_CSR_WORDS                                      3000U
#define TEST_HBUS_TARG_WRPTR                               0x460U
#define TEST_MSIX_FH_CAUSES                                0x2800U
#define TEST_MSIX_HW_CAUSES                                0x2808U
#define TEST_UMAC_RX_STATUS                             0x00d07824U
#define TEST_RX_IDLE                                    0x80000000U
#define TEST_EPOCH                                                7U

#define TEST_COMMAND_TFD_SIZE                              65536U
#define TEST_COMMAND_BYTE_COUNT_SIZE                        2048U
#define TEST_COMMAND_SLOTS_SIZE                            82944U
#define TEST_COMMAND_EXTERNAL_SIZE                          4096U
#define TEST_RX_TRANSFER_SIZE                               8192U
#define TEST_RX_COMPLETION_SIZE                            16384U
#define TEST_RX_STATUS_SIZE                                    2U

struct test_backend {
	uint32_t csr[TEST_CSR_WORDS];
	struct intel_ax211_command_transaction *transaction;
	uint64_t now;
	unsigned int lock_depth;
	unsigned int doorbell_count;
	int metadata_seen_before_doorbell;
	int fail_doorbell;
};

struct test_memory {
	uint8_t command_tfd[TEST_COMMAND_TFD_SIZE];
	uint8_t command_byte_count[TEST_COMMAND_BYTE_COUNT_SIZE];
	uint8_t command_slots[TEST_COMMAND_SLOTS_SIZE];
	uint8_t command_external[TEST_COMMAND_EXTERNAL_SIZE];
	uint8_t rx_transfer[TEST_RX_TRANSFER_SIZE];
	uint8_t rx_completion[TEST_RX_COMPLETION_SIZE];
	uint8_t rx_status[TEST_RX_STATUS_SIZE];
};

struct test_fixture {
	struct test_backend backend;
	struct test_memory memory;
	struct intel_ax211_transport transport;
	struct intel_ax211_command_transaction command;
};

static int test_csr_read32(void *argument, uint32_t offset, uint32_t *value);
static int test_csr_write32(void *argument, uint32_t offset, uint32_t value);
static int test_csr_write8(void *argument, uint32_t offset, uint8_t value);
static int test_nic_lock(void *argument);
static int test_nic_unlock(void *argument);
static int test_prph_read32(void *argument, uint32_t address, uint32_t *value);
static int test_prph_write32(void *argument, uint32_t address, uint32_t value);
static int test_dma_sync(void *argument, enum intel_ax211_transport_dma_region region, size_t offset, size_t length, enum intel_ax211_transport_dma_direction direction);
static int test_delay_us(void *argument, uint32_t duration_us);
static int test_clock_us(void *argument, uint64_t *time_us);
static void test_trace_deadline(void *argument, enum intel_ax211_transport_wait wait, uint64_t start_us, uint64_t deadline_us);
static void test_fixture_init(struct test_fixture *fixture, size_t max_pending, uint32_t hardware_epoch);
static size_t make_event(uint8_t *bytes, size_t capacity, uint8_t group, uint8_t opcode, uint8_t queue, uint8_t index, int failed, const void *payload, size_t payload_length);
static void put_le32(uint8_t *bytes, uint32_t value);
static uint16_t get_le16(const uint8_t *bytes);
static uint64_t get_le64(const uint8_t *bytes);
static void assert_zero(const uint8_t *bytes, size_t length);
static void test_reset_boundary(struct test_fixture *fixture, uint32_t hardware_epoch, int quiesce_result);
static void test_request_encoders(void);
static void test_normal_submit_and_completion(void);
static void test_response_and_ordering(void);
static void test_timeout_poison_no_reuse_and_late_completion(void);
static void test_cancel_and_cancel_all_no_reuse(void);
static void test_ambiguous_doorbell_and_late_completion(void);
static void test_external_submit_timeout_and_reset(void);
static void test_variable_response_lengths(void);
static void test_generation_and_validation(void);

static const struct intel_ax211_transport_ops test_ops = {
	test_csr_read32,
	test_csr_write32,
	test_csr_write8,
	test_nic_lock,
	test_nic_unlock,
	test_prph_read32,
	test_prph_write32,
	test_dma_sync,
	test_delay_us,
	test_clock_us,
	test_trace_deadline
};

int
main(void)
{
	test_request_encoders();
	test_normal_submit_and_completion();
	test_response_and_ordering();
	test_timeout_poison_no_reuse_and_late_completion();
	test_cancel_and_cancel_all_no_reuse();
	test_ambiguous_doorbell_and_late_completion();
	test_external_submit_timeout_and_reset();
	test_variable_response_lengths();
	test_generation_and_validation();
	puts("intel ax211 command/transport tests: PASS");
	return 0;
}

static int
test_csr_read32(
	void *argument,
	uint32_t offset,
	uint32_t *value)
{
	struct test_backend *backend;

	backend = argument;
	assert(offset / 4U < TEST_CSR_WORDS);
	*value = backend->csr[offset / 4U];
	return 0;
}

static int
test_csr_write32(
	void *argument,
	uint32_t offset,
	uint32_t value)
{
	struct test_backend *backend;
	uint16_t index;

	backend = argument;
	assert(offset / 4U < TEST_CSR_WORDS);
	if (offset == TEST_MSIX_FH_CAUSES || offset == TEST_MSIX_HW_CAUSES)
		backend->csr[offset / 4U] &= ~value;
	else
		backend->csr[offset / 4U] = value;
	if (offset != TEST_HBUS_TARG_WRPTR)
		return 0;
	backend->doorbell_count++;
	index = (uint16_t)((value - 1U) &
	    (INTEL_AX211_COMMAND_ENTRY_COUNT - 1U));
	if (backend->transaction != NULL &&
	    backend->transaction->entry[index].active)
		backend->metadata_seen_before_doorbell = 1;
	return backend->fail_doorbell ? -1 : 0;
}

static int
test_csr_write8(
	void *argument,
	uint32_t offset,
	uint8_t value)
{
	struct test_backend *backend;
	uint32_t shift;
	uint32_t mask;

	backend = argument;
	assert(offset / 4U < TEST_CSR_WORDS);
	shift = (offset & 3U) * 8U;
	mask = UINT32_C(0xff) << shift;
	backend->csr[offset / 4U] =
	    (backend->csr[offset / 4U] & ~mask) | ((uint32_t)value << shift);
	return 0;
}

static int
test_nic_lock(
	void *argument)
{
	struct test_backend *backend;

	backend = argument;
	backend->lock_depth++;
	return 0;
}

static int
test_nic_unlock(
	void *argument)
{
	struct test_backend *backend;

	backend = argument;
	assert(backend->lock_depth != 0U);
	backend->lock_depth--;
	return 0;
}

static int
test_prph_read32(
	void *argument,
	uint32_t address,
	uint32_t *value)
{
	struct test_backend *backend;

	backend = argument;
	assert(backend->lock_depth != 0U);
	(void)address;
	*value = TEST_RX_IDLE;
	return 0;
}

static int
test_prph_write32(
	void *argument,
	uint32_t address,
	uint32_t value)
{
	struct test_backend *backend;

	backend = argument;
	assert(backend->lock_depth != 0U);
	(void)address;
	(void)value;
	return 0;
}

static int
test_dma_sync(
	void *argument,
	enum intel_ax211_transport_dma_region region,
	size_t offset,
	size_t length,
	enum intel_ax211_transport_dma_direction direction)
{
	(void)argument;
	(void)region;
	(void)offset;
	(void)length;
	(void)direction;
	return 0;
}

static int
test_delay_us(
	void *argument,
	uint32_t duration_us)
{
	struct test_backend *backend;

	backend = argument;
	backend->now += duration_us;
	return 0;
}

static int
test_clock_us(
	void *argument,
	uint64_t *time_us)
{
	struct test_backend *backend;

	backend = argument;
	*time_us = backend->now;
	return 0;
}

static void
test_trace_deadline(
	void *argument,
	enum intel_ax211_transport_wait wait,
	uint64_t start_us,
	uint64_t deadline_us)
{
	(void)argument;
	(void)wait;
	(void)start_us;
	(void)deadline_us;
}

static void
test_fixture_init(
	struct test_fixture *fixture,
	size_t max_pending,
	uint32_t hardware_epoch)
{
	struct intel_ax211_transport_ring_memory memory;
	struct intel_ax211_mmio_profile profile;

	memset(fixture, 0, sizeof(*fixture));
	memset(&memory, 0, sizeof(memory));
	memory.command_tfd = fixture->memory.command_tfd;
	memory.command_tfd_size = sizeof(fixture->memory.command_tfd);
	memory.command_byte_count = fixture->memory.command_byte_count;
	memory.command_byte_count_size =
	    sizeof(fixture->memory.command_byte_count);
	memory.command_slots = fixture->memory.command_slots;
	memory.command_slots_size = sizeof(fixture->memory.command_slots);
	memory.command_slots_device_address = UINT64_C(0x10000000);
	memory.command_external = fixture->memory.command_external;
	memory.command_external_size =
	    sizeof(fixture->memory.command_external);
	memory.command_external_device_address = UINT64_C(0x20000000);
	memory.rx_transfer = fixture->memory.rx_transfer;
	memory.rx_transfer_size = sizeof(fixture->memory.rx_transfer);
	memory.rx_completion = fixture->memory.rx_completion;
	memory.rx_completion_size = sizeof(fixture->memory.rx_completion);
	memory.rx_status = fixture->memory.rx_status;
	memory.rx_status_size = sizeof(fixture->memory.rx_status);
	memset(&profile, 0, sizeof(profile));
	profile.mac_type = INTEL_AX211_MMIO_MAC_SO;
	profile.rf_type = INTEL_AX211_MMIO_RF_GF;
	profile.umac_prph_offset = INTEL_AX211_MMIO_UMAC_PRPH_OFFSET;
	assert(intel_ax211_transport_init(&fixture->transport, &test_ops,
	    &fixture->backend, &profile, &memory) ==
	    INTEL_AX211_TRANSPORT_OK);
	assert(intel_ax211_transport_configure_msix(&fixture->transport) ==
	    INTEL_AX211_TRANSPORT_OK);
	assert(intel_ax211_transport_initialize_rings(&fixture->transport) ==
	    INTEL_AX211_TRANSPORT_OK);
	assert(intel_ax211_transport_enable_runtime_interrupts(
	    &fixture->transport) == INTEL_AX211_TRANSPORT_OK);
	assert(intel_ax211_command_transaction_init(&fixture->command,
	    &fixture->transport, max_pending, hardware_epoch) ==
	    INTEL_AX211_COMMAND_OK);
	fixture->backend.transaction = &fixture->command;
}

static size_t
make_event(
	uint8_t *bytes,
	size_t capacity,
	uint8_t group,
	uint8_t opcode,
	uint8_t queue,
	uint8_t index,
	int failed,
	const void *payload,
	size_t payload_length)
{
	size_t frame_length;

	frame_length = 4U + payload_length;
	assert(bytes != NULL && capacity >= 4U + frame_length);
	assert(frame_length <= 0x3fffU);
	assert(payload != NULL || payload_length == 0U);
	memset(bytes, 0, capacity);
	put_le32(bytes, (uint32_t)frame_length);
	bytes[4] = opcode;
	bytes[5] = group;
	if (failed)
		bytes[5] |= INTEL_AX211_PROTOCOL_COMMAND_FAILED_MASK;
	bytes[6] = index;
	bytes[7] = queue;
	if (payload_length != 0U)
		memcpy(bytes + 8U, payload, payload_length);
	return 8U + payload_length;
}

static void
put_le32(
	uint8_t *bytes,
	uint32_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

static uint16_t
get_le16(
	const uint8_t *bytes)
{
	return (uint16_t)((uint16_t)bytes[0] |
	    ((uint16_t)bytes[1] << 8));
}

static uint64_t
get_le64(
	const uint8_t *bytes)
{
	uint64_t value;
	unsigned int index;

	value = 0U;
	for (index = 0U; index < 8U; index++)
		value |= (uint64_t)bytes[index] << (index * 8U);
	return value;
}

static void
assert_zero(
	const uint8_t *bytes,
	size_t length)
{
	size_t index;

	for (index = 0U; index < length; index++)
		assert(bytes[index] == 0U);
}

static void
test_reset_boundary(
	struct test_fixture *fixture,
	uint32_t hardware_epoch,
	int quiesce_result)
{
	assert(intel_ax211_command_after_device_reset(&fixture->command,
	    hardware_epoch) == INTEL_AX211_COMMAND_TRANSPORT_FAILED);
	assert(intel_ax211_transport_quiesce(&fixture->transport) ==
	    quiesce_result);
	assert(intel_ax211_transport_command_after_device_reset(
	    &fixture->transport) == INTEL_AX211_TRANSPORT_OK);
	assert_zero(fixture->memory.command_slots,
	    sizeof(fixture->memory.command_slots));
	assert_zero(fixture->memory.command_tfd,
	    sizeof(fixture->memory.command_tfd));
	assert_zero(fixture->memory.command_external,
	    sizeof(fixture->memory.command_external));
	assert(intel_ax211_command_after_device_reset(&fixture->command,
	    hardware_epoch) == INTEL_AX211_COMMAND_OK);
	assert(!intel_ax211_command_is_poisoned(&fixture->command));
	assert(intel_ax211_command_pending_count(&fixture->command) == 0U);
}

static void
test_request_encoders(void)
{
	uint8_t payload[4];

	memset(payload, 0xa5, sizeof(payload));
	assert(intel_ax211_command_nvm_access_complete_encode(payload) ==
	    INTEL_AX211_COMMAND_OK);
	assert_zero(payload, sizeof(payload));
	memset(payload, 0xa5, sizeof(payload));
	assert(intel_ax211_command_nvm_get_info_encode(payload) ==
	    INTEL_AX211_COMMAND_OK);
	assert_zero(payload, sizeof(payload));
	assert(intel_ax211_command_nvm_access_complete_encode(NULL) ==
	    INTEL_AX211_COMMAND_INVALID);
}

static void
test_normal_submit_and_completion(void)
{
	struct test_fixture fixture;
	struct intel_ax211_command_handle handle;
	uint8_t event[8];
	size_t event_length;
	size_t response_length;

	test_fixture_init(&fixture, 2U, TEST_EPOCH);
	assert(intel_ax211_command_submit_nvm_access_complete(&fixture.command,
	    100U, 10U, &handle) == INTEL_AX211_COMMAND_OK);
	assert(handle.token.queue == 0U && handle.token.index == 0U);
	assert(handle.generation == 1U && handle.hardware_epoch == TEST_EPOCH);
	assert(fixture.backend.metadata_seen_before_doorbell);
	assert(fixture.backend.doorbell_count == 1U);
	assert(intel_ax211_command_pending_count(&fixture.command) == 1U);
	assert(intel_ax211_transport_command_pending_count(
	    &fixture.transport) == 1U);
	assert(fixture.memory.command_slots[0] ==
	    INTEL_AX211_PROTOCOL_NVM_ACCESS_COMPLETE_OPCODE);
	assert(fixture.memory.command_slots[7] == 0U);
	assert(get_le16(fixture.memory.command_slots + 4U) == 4U);
	assert(get_le16(fixture.memory.command_tfd) == 1U);
	assert(get_le16(fixture.memory.command_tfd + 2U) == 12U);
	assert(get_le64(fixture.memory.command_tfd + 4U) ==
	    UINT64_C(0x10000000));

	event_length = make_event(event, sizeof(event),
	    INTEL_AX211_PROTOCOL_GROUP_REGULATORY_NVM,
	    INTEL_AX211_PROTOCOL_NVM_ACCESS_COMPLETE_OPCODE, 0U,
	    handle.token.index, 0, NULL, 0U);
	response_length = 99U;
	assert(intel_ax211_command_complete(&fixture.command, event,
	    event_length, TEST_EPOCH, NULL, 0U, &response_length) ==
	    INTEL_AX211_COMMAND_OK);
	assert(response_length == 0U);
	assert(intel_ax211_command_pending_count(&fixture.command) == 0U);
	assert(intel_ax211_transport_command_pending_count(
	    &fixture.transport) == 0U);
	assert_zero(fixture.memory.command_slots,
	    INTEL_AX211_TRANSPORT_COMMAND_SLOT_SIZE);
	assert_zero(fixture.memory.command_tfd, INTEL_AX211_TFD_SIZE);
	assert(intel_ax211_command_complete(&fixture.command, event,
	    event_length, TEST_EPOCH, NULL, 0U, &response_length) ==
	    INTEL_AX211_COMMAND_DUPLICATE);
}

static void
test_response_and_ordering(void)
{
	struct test_fixture fixture;
	struct intel_ax211_command_handle first;
	struct intel_ax211_command_handle second;
	struct intel_ax211_command_handle third;
	uint8_t event[8U + INTEL_AX211_PROTOCOL_NVM_GET_INFO_SIZE];
	uint8_t payload[INTEL_AX211_PROTOCOL_NVM_GET_INFO_SIZE];
	uint8_t response[INTEL_AX211_PROTOCOL_NVM_GET_INFO_SIZE];
	size_t event_length;
	size_t response_length;

	test_fixture_init(&fixture, 2U, TEST_EPOCH);
	memset(payload, 0x3c, sizeof(payload));
	assert(intel_ax211_command_submit_nvm_get_info(&fixture.command,
	    10U, 20U, &first) == INTEL_AX211_COMMAND_OK);
	assert(fixture.memory.command_slots[7] == 0U);
	assert(intel_ax211_command_submit_nvm_access_complete(&fixture.command,
	    11U, 20U, &second) == INTEL_AX211_COMMAND_OK);
	assert(intel_ax211_command_submit_nvm_access_complete(&fixture.command,
	    12U, 20U, &third) == INTEL_AX211_COMMAND_FULL);
	event_length = make_event(event, sizeof(event),
	    INTEL_AX211_PROTOCOL_GROUP_REGULATORY_NVM,
	    INTEL_AX211_PROTOCOL_NVM_ACCESS_COMPLETE_OPCODE, 0U,
	    second.token.index, 0, NULL, 0U);
	assert(intel_ax211_command_complete(&fixture.command, event,
	    event_length, TEST_EPOCH, NULL, 0U, &response_length) ==
	    INTEL_AX211_COMMAND_OUT_OF_ORDER);

	event_length = make_event(event, sizeof(event),
	    INTEL_AX211_PROTOCOL_GROUP_REGULATORY_NVM,
	    INTEL_AX211_PROTOCOL_NVM_GET_INFO_OPCODE, 0U,
	    first.token.index, 0, payload, sizeof(payload));
	assert(intel_ax211_command_complete(&fixture.command, event,
	    event_length, TEST_EPOCH - 1U, response, sizeof(response),
	    &response_length) == INTEL_AX211_COMMAND_STALE);
	assert(intel_ax211_command_complete(&fixture.command, event,
	    event_length, TEST_EPOCH, response, sizeof(response) - 1U,
	    &response_length) == INTEL_AX211_COMMAND_BUFFER_TOO_SMALL);
	assert(intel_ax211_command_complete(&fixture.command, event,
	    event_length, TEST_EPOCH, response, sizeof(response),
	    &response_length) == INTEL_AX211_COMMAND_OK);
	assert(response_length == sizeof(response));
	assert(memcmp(response, payload, sizeof(response)) == 0);

	event_length = make_event(event, sizeof(event),
	    INTEL_AX211_PROTOCOL_GROUP_REGULATORY_NVM,
	    INTEL_AX211_PROTOCOL_NVM_ACCESS_COMPLETE_OPCODE, 0U,
	    second.token.index, 0, NULL, 0U);
	assert(intel_ax211_command_complete(&fixture.command, event,
	    event_length, TEST_EPOCH, NULL, 0U, &response_length) ==
	    INTEL_AX211_COMMAND_OK);
	assert(intel_ax211_command_pending_count(&fixture.command) == 0U);
}

static void
test_timeout_poison_no_reuse_and_late_completion(void)
{
	struct test_fixture fixture;
	struct intel_ax211_command_handle handle;
	struct intel_ax211_command_handle observed;
	uint8_t event[8];
	uint8_t slot_copy[INTEL_AX211_TRANSPORT_COMMAND_SLOT_SIZE];
	size_t event_length;
	size_t response_length;
	uint16_t head;

	test_fixture_init(&fixture, 2U, TEST_EPOCH);
	assert(intel_ax211_command_submit_nvm_access_complete(&fixture.command,
	    100U, 10U, &handle) == INTEL_AX211_COMMAND_OK);
	memcpy(slot_copy, fixture.memory.command_slots, sizeof(slot_copy));
	head = fixture.transport.command_ring.head;
	assert(intel_ax211_command_timeout_oldest(&fixture.command, 109U,
	    &observed) == INTEL_AX211_COMMAND_PENDING);
	assert(observed.generation == handle.generation);
	assert(intel_ax211_command_timeout_oldest(&fixture.command, 110U,
	    &observed) == INTEL_AX211_COMMAND_TIMEOUT);
	assert(intel_ax211_command_is_poisoned(&fixture.command));
	assert(intel_ax211_command_pending_count(&fixture.command) == 1U);
	assert(intel_ax211_transport_command_pending_count(
	    &fixture.transport) == 1U);
	assert(intel_ax211_command_submit_nvm_access_complete(&fixture.command,
	    120U, 10U, &observed) == INTEL_AX211_COMMAND_POISONED);
	assert(fixture.transport.command_ring.head == head);
	assert(memcmp(slot_copy, fixture.memory.command_slots,
	    sizeof(slot_copy)) == 0);

	event_length = make_event(event, sizeof(event),
	    INTEL_AX211_PROTOCOL_GROUP_REGULATORY_NVM,
	    INTEL_AX211_PROTOCOL_NVM_ACCESS_COMPLETE_OPCODE, 0U,
	    handle.token.index, 0, NULL, 0U);
	assert(intel_ax211_command_complete(&fixture.command, event,
	    event_length, TEST_EPOCH, NULL, 0U, &response_length) ==
	    INTEL_AX211_COMMAND_OK);
	assert(intel_ax211_command_pending_count(&fixture.command) == 0U);
	assert(intel_ax211_command_is_poisoned(&fixture.command));
	assert(intel_ax211_command_submit_nvm_access_complete(&fixture.command,
	    130U, 10U, &observed) == INTEL_AX211_COMMAND_POISONED);
	test_reset_boundary(&fixture, TEST_EPOCH + 1U,
	    INTEL_AX211_TRANSPORT_OK);
	assert(intel_ax211_command_complete(&fixture.command, event,
	    event_length, TEST_EPOCH, NULL, 0U, &response_length) ==
	    INTEL_AX211_COMMAND_STALE);
}

static void
test_cancel_and_cancel_all_no_reuse(void)
{
	struct test_fixture fixture;
	struct intel_ax211_command_handle first;
	struct intel_ax211_command_handle second;
	uint8_t slot_copy[INTEL_AX211_TRANSPORT_COMMAND_SLOT_SIZE];

	test_fixture_init(&fixture, 2U, TEST_EPOCH);
	assert(intel_ax211_command_submit_nvm_access_complete(&fixture.command,
	    1U, 10U, &first) == INTEL_AX211_COMMAND_OK);
	memcpy(slot_copy, fixture.memory.command_slots, sizeof(slot_copy));
	assert(intel_ax211_command_cancel(&fixture.command, &first) ==
	    INTEL_AX211_COMMAND_OK);
	assert(intel_ax211_command_cancel(&fixture.command, &first) ==
	    INTEL_AX211_COMMAND_DUPLICATE);
	assert(intel_ax211_command_is_poisoned(&fixture.command));
	assert(intel_ax211_command_pending_count(&fixture.command) == 1U);
	assert(memcmp(slot_copy, fixture.memory.command_slots,
	    sizeof(slot_copy)) == 0);
	test_reset_boundary(&fixture, TEST_EPOCH + 1U,
	    INTEL_AX211_TRANSPORT_FAILED);

	test_fixture_init(&fixture, 2U, TEST_EPOCH);
	assert(intel_ax211_command_submit_nvm_access_complete(&fixture.command,
	    1U, 10U, &first) == INTEL_AX211_COMMAND_OK);
	assert(intel_ax211_command_submit_nvm_access_complete(&fixture.command,
	    2U, 10U, &second) == INTEL_AX211_COMMAND_OK);
	intel_ax211_command_cancel_all(&fixture.command);
	assert(intel_ax211_command_is_poisoned(&fixture.command));
	assert(intel_ax211_command_pending_count(&fixture.command) == 2U);
	assert(fixture.command.entry[first.token.index].abandoned);
	assert(fixture.command.entry[second.token.index].abandoned);
	assert(intel_ax211_transport_command_pending_count(
	    &fixture.transport) == 2U);
	test_reset_boundary(&fixture, TEST_EPOCH + 1U,
	    INTEL_AX211_TRANSPORT_FAILED);
}

static void
test_ambiguous_doorbell_and_late_completion(void)
{
	struct test_fixture fixture;
	struct intel_ax211_command_handle handle;
	struct intel_ax211_command_handle rejected;
	uint8_t event[8];
	uint8_t slot_copy[INTEL_AX211_TRANSPORT_COMMAND_SLOT_SIZE];
	size_t event_length;
	size_t response_length;
	uint16_t head;

	test_fixture_init(&fixture, 2U, TEST_EPOCH);
	fixture.backend.fail_doorbell = 1;
	assert(intel_ax211_command_submit_nvm_access_complete(&fixture.command,
	    10U, 10U, &handle) == INTEL_AX211_COMMAND_DOORBELL_FAILED);
	assert(fixture.backend.metadata_seen_before_doorbell);
	assert(fixture.transport.command_reset_required);
	assert(intel_ax211_command_is_poisoned(&fixture.command));
	assert(intel_ax211_command_pending_count(&fixture.command) == 1U);
	assert(intel_ax211_transport_command_pending_count(
	    &fixture.transport) == 1U);
	memcpy(slot_copy, fixture.memory.command_slots, sizeof(slot_copy));
	head = fixture.transport.command_ring.head;
	assert(intel_ax211_command_submit_nvm_access_complete(&fixture.command,
	    20U, 10U, &rejected) == INTEL_AX211_COMMAND_POISONED);
	assert(fixture.transport.command_ring.head == head);
	assert(memcmp(slot_copy, fixture.memory.command_slots,
	    sizeof(slot_copy)) == 0);

	fixture.backend.fail_doorbell = 0;
	event_length = make_event(event, sizeof(event),
	    INTEL_AX211_PROTOCOL_GROUP_REGULATORY_NVM,
	    INTEL_AX211_PROTOCOL_NVM_ACCESS_COMPLETE_OPCODE, 0U,
	    handle.token.index, 0, NULL, 0U);
	assert(intel_ax211_command_complete(&fixture.command, event,
	    event_length, TEST_EPOCH, NULL, 0U, &response_length) ==
	    INTEL_AX211_COMMAND_OK);
	assert(intel_ax211_command_pending_count(&fixture.command) == 0U);
	assert(intel_ax211_command_is_poisoned(&fixture.command));
	assert(fixture.transport.command_reset_required);
	test_reset_boundary(&fixture, TEST_EPOCH + 1U,
	    INTEL_AX211_TRANSPORT_OK);
}

/* Proves that logical timeout and ambiguity never release external DMA. */
static void
test_external_submit_timeout_and_reset(void)
{
	struct test_fixture fixture;
	struct intel_ax211_command_request request;
	struct intel_ax211_command_handle handle;
	struct intel_ax211_command_handle observed;
	uint8_t payload[1940];
	uint8_t snapshot[sizeof(payload)];
	uint8_t event[8];
	size_t event_length;
	size_t response_length;
	size_t index;

	for (index = 0U; index < sizeof(payload); index++)
		payload[index] = (uint8_t)(index * 19U + 3U);
	memset(&request, 0, sizeof(request));
	request.command.group = 1U;
	request.command.opcode = 0x0dU;
	request.command.version = 0U;
	request.payload = payload;
	request.payload_length = sizeof(payload);
	request.response_version = 0U;

	/* The 1940-byte scan-sized payload automatically selects external DMA. */
	test_fixture_init(&fixture, 2U, TEST_EPOCH);
	assert(intel_ax211_command_submit(&fixture.command, &request,
	    100U, 10U, &handle) == INTEL_AX211_COMMAND_OK);
	assert(fixture.transport.command_external_active);
	assert(memcmp(fixture.memory.command_external +
	    INTEL_AX211_WIDE_COMMAND_HEADER_SIZE, payload,
	    sizeof(payload)) == 0);
	assert(intel_ax211_command_submit(&fixture.command, &request,
	    101U, 10U, &observed) == INTEL_AX211_COMMAND_FULL);
	event_length = make_event(event, sizeof(event), 1U, 0x0dU, 0U,
	    handle.token.index, 0, NULL, 0U);
	assert(intel_ax211_command_complete(&fixture.command, event,
	    event_length, TEST_EPOCH, NULL, 0U, &response_length) ==
	    INTEL_AX211_COMMAND_OK);
	assert(!fixture.transport.command_external_active);
	assert_zero(fixture.memory.command_external,
	    sizeof(fixture.memory.command_external));

	/* Oversized requests fail before reserving a token or copying bytes. */
	request.payload_length = INTEL_AX211_COMMAND_EXTERNAL_PAYLOAD_SIZE + 1U;
	assert(intel_ax211_command_submit(&fixture.command, &request,
	    120U, 10U, &observed) == INTEL_AX211_COMMAND_INVALID);
	assert(intel_ax211_command_pending_count(&fixture.command) == 0U);
	request.payload_length = sizeof(payload);

	/* Timeout poisons admission while retaining the exact DMA contents. */
	assert(intel_ax211_command_submit(&fixture.command, &request,
	    200U, 10U, &handle) == INTEL_AX211_COMMAND_OK);
	memcpy(snapshot, fixture.memory.command_external +
	    INTEL_AX211_WIDE_COMMAND_HEADER_SIZE, sizeof(snapshot));
	assert(intel_ax211_command_timeout_oldest(&fixture.command, 210U,
	    &observed) == INTEL_AX211_COMMAND_TIMEOUT);
	assert(fixture.transport.command_external_active);
	assert(memcmp(snapshot, fixture.memory.command_external +
	    INTEL_AX211_WIDE_COMMAND_HEADER_SIZE, sizeof(snapshot)) == 0);
	assert(intel_ax211_command_submit(&fixture.command, &request,
	    211U, 10U, &observed) == INTEL_AX211_COMMAND_POISONED);
	test_reset_boundary(&fixture, TEST_EPOCH + 1U,
	    INTEL_AX211_TRANSPORT_FAILED);

	/* An ambiguous doorbell retains ownership until the same reset proof. */
	test_fixture_init(&fixture, 2U, TEST_EPOCH);
	fixture.backend.fail_doorbell = 1;
	assert(intel_ax211_command_submit(&fixture.command, &request,
	    300U, 10U, &handle) ==
	    INTEL_AX211_COMMAND_DOORBELL_FAILED);
	assert(fixture.transport.command_external_active);
	assert(memcmp(fixture.memory.command_external +
	    INTEL_AX211_WIDE_COMMAND_HEADER_SIZE, payload,
	    sizeof(payload)) == 0);
	test_reset_boundary(&fixture, TEST_EPOCH + 1U,
	    INTEL_AX211_TRANSPORT_FAILED);
}

/* Proves the checked 20..460-byte MCC-v6 response-length contract. */
static void
test_variable_response_lengths(void)
{
	static const size_t accepted_length[3] = { 20U, 24U, 460U };
	struct test_fixture fixture;
	struct intel_ax211_command_request request;
	struct intel_ax211_command_handle handle;
	uint8_t payload[461];
	uint8_t event[8U + sizeof(payload)];
	uint8_t response[460];
	size_t event_length;
	size_t response_length;
	size_t index;
	size_t length;

	for (index = 0U; index < sizeof(payload); index++)
		payload[index] = (uint8_t)(index * 31U + 1U);
	memset(&request, 0, sizeof(request));
	request.command.group = 1U;
	request.command.opcode = 0x08U;
	request.command.version = 0U;
	request.response_version = 6U;
	request.minimum_response_length = 20U;
	request.maximum_response_length = 460U;

	for (index = 0U; index < 3U; index++) {
		length = accepted_length[index];
		test_fixture_init(&fixture, 1U, TEST_EPOCH);
		assert(intel_ax211_command_submit(&fixture.command, &request,
		    10U, 20U, &handle) == INTEL_AX211_COMMAND_OK);
		event_length = make_event(event, sizeof(event), 1U, 0x08U, 0U,
		    handle.token.index, 0, payload, length);
		response_length = 0U;
		assert(intel_ax211_command_complete(&fixture.command, event,
		    event_length, TEST_EPOCH, response, sizeof(response),
		    &response_length) == INTEL_AX211_COMMAND_OK);
		assert(response_length == length);
		assert(memcmp(response, payload, length) == 0);
	}

	/* Both ends of the range are fail-closed and retire the bad command. */
	test_fixture_init(&fixture, 1U, TEST_EPOCH);
	assert(intel_ax211_command_submit(&fixture.command, &request,
	    10U, 20U, &handle) == INTEL_AX211_COMMAND_OK);
	event_length = make_event(event, sizeof(event), 1U, 0x08U, 0U,
	    handle.token.index, 0, payload, 19U);
	response_length = 99U;
	assert(intel_ax211_command_complete(&fixture.command, event,
	    event_length, TEST_EPOCH, response, sizeof(response),
	    &response_length) == INTEL_AX211_COMMAND_MALFORMED);
	assert(response_length == 0U);
	assert(intel_ax211_command_pending_count(&fixture.command) == 0U);

	test_fixture_init(&fixture, 1U, TEST_EPOCH);
	assert(intel_ax211_command_submit(&fixture.command, &request,
	    10U, 20U, &handle) == INTEL_AX211_COMMAND_OK);
	event_length = make_event(event, sizeof(event), 1U, 0x08U, 0U,
	    handle.token.index, 0, payload, 461U);
	response_length = 99U;
	assert(intel_ax211_command_complete(&fixture.command, event,
	    event_length, TEST_EPOCH, response, sizeof(response),
	    &response_length) == INTEL_AX211_COMMAND_MALFORMED);
	assert(response_length == 0U);
	assert(intel_ax211_command_pending_count(&fixture.command) == 0U);

	/* Capacity is checked against the accepted actual response length. */
	test_fixture_init(&fixture, 1U, TEST_EPOCH);
	assert(intel_ax211_command_submit(&fixture.command, &request,
	    10U, 20U, &handle) == INTEL_AX211_COMMAND_OK);
	event_length = make_event(event, sizeof(event), 1U, 0x08U, 0U,
	    handle.token.index, 0, payload, 24U);
	response_length = 99U;
	assert(intel_ax211_command_complete(&fixture.command, event,
	    event_length, TEST_EPOCH, response, 23U, &response_length) ==
	    INTEL_AX211_COMMAND_BUFFER_TOO_SMALL);
	assert(response_length == 0U);
	assert(intel_ax211_command_pending_count(&fixture.command) == 1U);
	assert(intel_ax211_command_complete(&fixture.command, event,
	    event_length, TEST_EPOCH, response, 24U, &response_length) ==
	    INTEL_AX211_COMMAND_OK);
	assert(response_length == 24U);
}

static void
test_generation_and_validation(void)
{
	struct test_fixture fixture;
	struct intel_ax211_command_handle handle;
	struct intel_ax211_command_request request;
	uint8_t event[8];
	size_t event_length;
	size_t response_length;

	test_fixture_init(&fixture, 1U, TEST_EPOCH);
	memset(&request, 0, sizeof(request));
	request.command.group = 1U;
	request.command.opcode = 2U;
	request.command.version = 3U;
	request.response_version = 4U;
	fixture.command.next_generation = UINT32_MAX;
	assert(intel_ax211_command_submit(&fixture.command, &request,
	    1U, 2U, &handle) == INTEL_AX211_COMMAND_OK);
	assert(handle.generation == UINT32_MAX);
	event_length = make_event(event, sizeof(event), 1U, 2U, 0U,
	    handle.token.index, 0, NULL, 0U);
	assert(intel_ax211_command_complete(&fixture.command, event,
	    event_length, TEST_EPOCH, NULL, 0U, &response_length) ==
	    INTEL_AX211_COMMAND_OK);
	assert(intel_ax211_command_submit(&fixture.command, &request,
	    3U, 2U, &handle) == INTEL_AX211_COMMAND_OK);
	assert(handle.generation == 1U);
	event_length = make_event(event, sizeof(event), 1U, 2U, 0U,
	    handle.token.index, 0, NULL, 0U);
	assert(intel_ax211_command_complete(&fixture.command, event,
	    event_length, TEST_EPOCH, NULL, 0U, &response_length) ==
	    INTEL_AX211_COMMAND_OK);

	/* API89 carries a logical legacy command as a wide LONG_GROUP command. */
	request.command.group = INTEL_AX211_PROTOCOL_GROUP_LEGACY;
	assert(intel_ax211_command_submit(&fixture.command, &request,
	    4U, 2U, &handle) == INTEL_AX211_COMMAND_OK);
	assert(fixture.memory.command_slots[
	    (size_t)handle.token.index *
	    INTEL_AX211_TRANSPORT_COMMAND_SLOT_SIZE] == 2U);
	assert(fixture.memory.command_slots[
	    (size_t)handle.token.index *
	    INTEL_AX211_TRANSPORT_COMMAND_SLOT_SIZE + 1U] ==
	    INTEL_AX211_PROTOCOL_GROUP_LONG);
	assert(get_le16(fixture.memory.command_slots +
	    (size_t)handle.token.index *
	    INTEL_AX211_TRANSPORT_COMMAND_SLOT_SIZE + 4U) == 0U);
	assert(fixture.memory.command_slots[
	    (size_t)handle.token.index *
	    INTEL_AX211_TRANSPORT_COMMAND_SLOT_SIZE + 7U] == 0U);
	event_length = make_event(event, sizeof(event),
	    INTEL_AX211_PROTOCOL_GROUP_LONG, 2U, 0U,
	    handle.token.index, 0, NULL, 0U);
	assert(intel_ax211_command_complete(&fixture.command, event,
	    event_length, TEST_EPOCH, NULL, 0U, &response_length) ==
	    INTEL_AX211_COMMAND_OK);
	request.command.group = 1U;
	request.command.version = INTEL_AX211_PROTOCOL_UNKNOWN_VERSION;
	assert(intel_ax211_command_submit(&fixture.command, &request,
	    4U, 2U, &handle) == INTEL_AX211_COMMAND_INVALID);
	request.command.version = 3U;
	request.minimum_response_length = 2U;
	request.maximum_response_length = 1U;
	assert(intel_ax211_command_submit(&fixture.command, &request,
	    4U, 2U, &handle) == INTEL_AX211_COMMAND_INVALID);
	request.minimum_response_length = 0U;
	request.maximum_response_length =
	    INTEL_AX211_COMMAND_EXTERNAL_PAYLOAD_SIZE + 1U;
	assert(intel_ax211_command_submit(&fixture.command, &request,
	    4U, 2U, &handle) == INTEL_AX211_COMMAND_INVALID);
	assert(intel_ax211_command_transaction_init(NULL, &fixture.transport,
	    1U, TEST_EPOCH) == INTEL_AX211_COMMAND_INVALID);
}
