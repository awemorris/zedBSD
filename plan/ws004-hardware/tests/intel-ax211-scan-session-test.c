/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * Intel AX211 live command/scan-session fixture
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../../src/drivers/intel-ax211-scan-session.h"

#define TEST_CSR_WORDS                                      3000U
#define TEST_HBUS_TARG_WRPTR                               0x460U
#define TEST_MSIX_FH_CAUSES                                0x2800U
#define TEST_MSIX_HW_CAUSES                                0x2808U
#define TEST_EPOCH                                                9U
#define TEST_COMMAND_TFD_SIZE                              65536U
#define TEST_COMMAND_BYTE_COUNT_SIZE                        2048U
#define TEST_COMMAND_SLOTS_SIZE                            82944U
#define TEST_COMMAND_EXTERNAL_SIZE                          4096U
#define TEST_RX_TRANSFER_SIZE                               8192U
#define TEST_RX_COMPLETION_SIZE                            16384U
#define TEST_RX_STATUS_SIZE                                    2U

struct test_backend {
	uint32_t csr[TEST_CSR_WORDS];
	uint64_t now;
	unsigned int lock_depth;
	unsigned int doorbell_count;
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
	struct intel_ax211_scan_session session;
	uint8_t versions[INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES];
	struct intel_ax211_protocol_command_table table;
	struct intel_ax211_protocol_nvm nvm;
	struct intel_ax211_runtime_mcc mcc;
};

static int test_csr_read32(void *argument, uint32_t offset,
	uint32_t *value);
static int test_csr_write32(void *argument, uint32_t offset,
	uint32_t value);
static int test_csr_write8(void *argument, uint32_t offset, uint8_t value);
static int test_nic_lock(void *argument);
static int test_nic_unlock(void *argument);
static int test_prph_read32(void *argument, uint32_t address,
	uint32_t *value);
static int test_prph_write32(void *argument, uint32_t address,
	uint32_t value);
static int test_dma_sync(void *argument,
	enum intel_ax211_transport_dma_region region, size_t offset,
	size_t length, enum intel_ax211_transport_dma_direction direction);
static int test_delay_us(void *argument, uint32_t duration_us);
static int test_clock_us(void *argument, uint64_t *time_us);
static void test_trace_deadline(void *argument,
	enum intel_ax211_transport_wait wait, uint64_t start_us,
	uint64_t deadline_us);
static void test_put_le32(uint8_t *bytes, uint32_t value);
static uint16_t test_get_le16(const uint8_t *bytes);
static void test_put_version(uint8_t *bytes, size_t index, uint8_t group,
	uint8_t opcode, uint8_t command_version,
	uint8_t notification_version);
static void test_make_api89_table(uint8_t *bytes);
static void test_make_nvm(struct intel_ax211_protocol_nvm *nvm);
static void test_make_mcc(struct intel_ax211_runtime_mcc *mcc);
static void test_fixture_init(struct test_fixture *fixture);
static size_t test_make_ack(uint8_t *bytes, size_t capacity,
	uint8_t opcode, const struct intel_ax211_command_handle *handle,
	const void *payload, size_t payload_length);
static size_t test_make_abort_ack(uint8_t *bytes, size_t capacity,
	const struct intel_ax211_command_handle *handle, uint32_t status);
static void test_make_iteration(struct intel_ax211_protocol_message *message,
	uint8_t *payload, size_t payload_length, uint32_t epoch,
	uint8_t channel, uint8_t status);
static void test_make_complete(struct intel_ax211_protocol_message *message,
	uint8_t payload[16], uint32_t epoch, uint8_t status);
static void test_start(struct test_fixture *fixture, uint64_t generation,
	uint8_t channel, uint64_t now);
static void test_success_and_generation_fence(void);
static void test_abort_and_finite_completion(void);
static void test_command_ack_timeout(void);
static void test_exact_ack_and_validation(void);

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
	test_success_and_generation_fence();
	test_abort_and_finite_completion();
	test_command_ack_timeout();
	test_exact_ack_and_validation();
	puts("intel ax211 scan session tests: PASS");
	return 0;
}

static int
test_csr_read32(void *argument, uint32_t offset, uint32_t *value)
{
	struct test_backend *backend;

	backend = argument;
	assert(offset / 4U < TEST_CSR_WORDS);
	*value = backend->csr[offset / 4U];
	return 0;
}

static int
test_csr_write32(void *argument, uint32_t offset, uint32_t value)
{
	struct test_backend *backend;

	backend = argument;
	assert(offset / 4U < TEST_CSR_WORDS);
	if (offset == TEST_MSIX_FH_CAUSES || offset == TEST_MSIX_HW_CAUSES)
		backend->csr[offset / 4U] &= ~value;
	else
		backend->csr[offset / 4U] = value;
	if (offset == TEST_HBUS_TARG_WRPTR)
		backend->doorbell_count++;
	return 0;
}

static int
test_csr_write8(void *argument, uint32_t offset, uint8_t value)
{
	struct test_backend *backend;
	uint32_t shift;
	uint32_t mask;

	backend = argument;
	assert(offset / 4U < TEST_CSR_WORDS);
	shift = (offset & 3U) * 8U;
	mask = UINT32_C(0xff) << shift;
	backend->csr[offset / 4U] =
	    (backend->csr[offset / 4U] & ~mask) |
	    ((uint32_t)value << shift);
	return 0;
}

static int
test_nic_lock(void *argument)
{
	struct test_backend *backend;

	backend = argument;
	backend->lock_depth++;
	return 0;
}

static int
test_nic_unlock(void *argument)
{
	struct test_backend *backend;

	backend = argument;
	assert(backend->lock_depth != 0U);
	backend->lock_depth--;
	return 0;
}

static int
test_prph_read32(void *argument, uint32_t address, uint32_t *value)
{
	(void)argument;
	(void)address;
	*value = 0U;
	return 0;
}

static int
test_prph_write32(void *argument, uint32_t address, uint32_t value)
{
	(void)argument;
	(void)address;
	(void)value;
	return 0;
}

static int
test_dma_sync(void *argument,
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
test_delay_us(void *argument, uint32_t duration_us)
{
	struct test_backend *backend;

	backend = argument;
	backend->now += duration_us;
	return 0;
}

static int
test_clock_us(void *argument, uint64_t *time_us)
{
	struct test_backend *backend;

	backend = argument;
	*time_us = backend->now;
	return 0;
}

static void
test_trace_deadline(void *argument, enum intel_ax211_transport_wait wait,
	uint64_t start_us, uint64_t deadline_us)
{
	(void)argument;
	(void)wait;
	(void)start_us;
	(void)deadline_us;
}

static void
test_put_le32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

static uint16_t
test_get_le16(const uint8_t *bytes)
{
	return (uint16_t)((uint16_t)bytes[0] |
	    ((uint16_t)bytes[1] << 8));
}

static void
test_put_version(uint8_t *bytes, size_t index, uint8_t group,
	uint8_t opcode, uint8_t command_version, uint8_t notification_version)
{
	uint8_t *entry;

	entry = bytes + index *
	    INTEL_AX211_PROTOCOL_COMMAND_VERSION_ENTRY_SIZE;
	entry[0] = opcode;
	entry[1] = group;
	entry[2] = command_version;
	entry[3] = notification_version;
}

static void
test_make_api89_table(uint8_t *bytes)
{
	size_t index;

	memset(bytes, 0, INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES);
	for (index = 0U;
	    index < INTEL_AX211_PROTOCOL_API89_COMMAND_COUNT - 1U; index++)
		test_put_version(bytes, index, 0x10U, (uint8_t)index, 1U, 1U);
	test_put_version(bytes, 0U, 0U, 0x01U, 99U, 6U);
	test_put_version(bytes, 1U, 1U, 0x0cU, 5U, 0U);
	test_put_version(bytes, 2U, 1U, 0x0dU, 17U, 0U);
	test_put_version(bytes, 3U, 1U, 0x0eU, 1U, 0U);
	test_put_version(bytes, 4U, 12U, 0x00U, 1U, 0U);
	test_put_version(bytes, 5U, 12U, 0x02U, 1U, 4U);
	test_put_version(bytes, 6U, 12U, 0xfeU, 99U, 1U);
	test_put_version(bytes,
	    INTEL_AX211_PROTOCOL_API89_COMMAND_COUNT - 1U,
	    0U, 0U, 0U, 0U);
}

static void
test_make_nvm(struct intel_ax211_protocol_nvm *nvm)
{
	static const uint8_t channels[3] = { 1U, 6U, 11U };
	size_t index;

	memset(nvm, 0, sizeof(*nvm));
	nvm->band_24_enabled = 1U;
	nvm->band_52_enabled = 1U;
	nvm->lar_enabled = 1U;
	nvm->channel_24ghz_count = 3U;
	for (index = 0U; index < 3U; index++) {
		nvm->channel_24ghz[index].number = channels[index];
		nvm->channel_24ghz[index].valid = 1U;
		nvm->channel_24ghz[index].active = 1U;
	}
	nvm->channel_5ghz_count = 1U;
	nvm->channel_5ghz[0].number = 36U;
	nvm->channel_5ghz[0].valid = 1U;
	nvm->channel_5ghz[0].active = 1U;
}

static void
test_make_mcc(struct intel_ax211_runtime_mcc *mcc)
{
	size_t index;

	memset(mcc, 0, sizeof(*mcc));
	mcc->status = 0U;
	mcc->channel_count = 15U;
	for (index = 0U; index < mcc->channel_count; index++)
		mcc->channel[index] = INTEL_AX211_PROTOCOL_NVM_CHANNEL_VALID;
}

static void
test_fixture_init(struct test_fixture *fixture)
{
	static const uint8_t address[6] = {
		0x02U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U
	};
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
	memory.command_external_size = sizeof(fixture->memory.command_external);
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
	    &fixture->transport, 1U, TEST_EPOCH) == INTEL_AX211_COMMAND_OK);
	test_make_api89_table(fixture->versions);
	assert(intel_ax211_protocol_command_table_parse(fixture->versions,
	    sizeof(fixture->versions), &fixture->table) ==
	    INTEL_AX211_PROTOCOL_OK);
	test_make_nvm(&fixture->nvm);
	test_make_mcc(&fixture->mcc);
	assert(intel_ax211_scan_session_init(&fixture->session,
	    &fixture->command, &fixture->table, &fixture->nvm,
	    &fixture->mcc, address, TEST_EPOCH) ==
	    INTEL_AX211_SCAN_SESSION_OK);
}

static size_t
test_make_ack(uint8_t *bytes, size_t capacity, uint8_t opcode,
	const struct intel_ax211_command_handle *handle,
	const void *payload, size_t payload_length)
{
	size_t frame_length;

	frame_length = 4U + payload_length;
	assert(capacity >= frame_length + 4U && frame_length <= 0x3fffU);
	assert(payload != NULL || payload_length == 0U);
	memset(bytes, 0, capacity);
	test_put_le32(bytes, (uint32_t)frame_length);
	bytes[4] = opcode;
	bytes[5] = INTEL_AX211_SCAN_GROUP_LONG;
	bytes[6] = handle->token.index;
	bytes[7] = handle->token.queue;
	if (payload_length != 0U)
		memcpy(bytes + 8U, payload, payload_length);
	return 8U + payload_length;
}

static size_t
test_make_abort_ack(uint8_t *bytes, size_t capacity,
	const struct intel_ax211_command_handle *handle, uint32_t status)
{
	uint8_t payload[4];

	test_put_le32(payload, status);
	return test_make_ack(bytes, capacity, INTEL_AX211_SCAN_ABORT_OPCODE,
	    handle, payload, sizeof(payload));
}

static void
test_make_iteration(struct intel_ax211_protocol_message *message,
	uint8_t *payload, size_t payload_length, uint32_t epoch,
	uint8_t channel, uint8_t status)
{
	assert(payload_length >= 24U);
	memset(payload, 0, payload_length);
	test_put_le32(payload, INTEL_AX211_SCAN_UID);
	payload[4] = 1U;
	payload[5] = status;
	payload[7] = channel;
	payload[16] = channel;
	payload[17] = 1U;
	test_put_le32(payload + 20U, 900U);
	memset(message, 0, sizeof(*message));
	message->opcode = INTEL_AX211_SCAN_ITERATION_COMPLETE_OPCODE;
	message->group = INTEL_AX211_SCAN_GROUP_LEGACY;
	message->version = INTEL_AX211_SCAN_NOTIFICATION_VERSION;
	message->generation = epoch;
	message->payload = payload;
	message->payload_length = payload_length;
}

static void
test_make_complete(struct intel_ax211_protocol_message *message,
	uint8_t payload[16], uint32_t epoch, uint8_t status)
{
	memset(payload, 0, 16U);
	test_put_le32(payload, INTEL_AX211_SCAN_UID);
	payload[6] = status;
	memset(message, 0, sizeof(*message));
	message->opcode = INTEL_AX211_SCAN_COMPLETE_OPCODE;
	message->group = INTEL_AX211_SCAN_GROUP_LEGACY;
	message->version = INTEL_AX211_SCAN_NOTIFICATION_VERSION;
	message->generation = epoch;
	message->payload = payload;
	message->payload_length = 16U;
}

static void
test_start(struct test_fixture *fixture, uint64_t generation,
	uint8_t channel, uint64_t now)
{
	uint8_t ack[8];
	size_t ack_length;

	assert(intel_ax211_scan_session_begin_channel(&fixture->session,
	    generation, channel, now) == INTEL_AX211_SCAN_SESSION_OK);
	ack_length = test_make_ack(ack, sizeof(ack),
	    INTEL_AX211_SCAN_REQUEST_OPCODE,
	    &fixture->session.command_handle, NULL, 0U);
	assert(intel_ax211_scan_session_start_ack(&fixture->session, ack,
	    ack_length, TEST_EPOCH, now + 1U) ==
	    INTEL_AX211_SCAN_SESSION_OK);
}

static void
test_success_and_generation_fence(void)
{
	struct test_fixture fixture;
	struct intel_ax211_protocol_message message;
	struct intel_ax211_scan_session_event reported;
	uint8_t payload[INTEL_AX211_SCAN_ITERATION_NOTIFICATION_SIZE];
	uint8_t ack[8];
	uint64_t generation;
	size_t ack_length;

	test_fixture_init(&fixture);
	memset(fixture.versions, 0xa5, sizeof(fixture.versions));
	generation = UINT64_C(0x100000005);
	assert(intel_ax211_scan_session_begin_channel(&fixture.session,
	    generation, 6U, 100U) == INTEL_AX211_SCAN_SESSION_OK);
	assert(fixture.session.phase ==
	    INTEL_AX211_SCAN_SESSION_WAIT_START_ACK);
	assert(fixture.memory.command_external[0] ==
	    INTEL_AX211_SCAN_REQUEST_OPCODE);
	assert(fixture.memory.command_external[1] ==
	    INTEL_AX211_SCAN_GROUP_LONG);
	assert(fixture.memory.command_external[7] == 0U);
	assert(test_get_le16(fixture.memory.command_external + 4U) ==
	    INTEL_AX211_SCAN_REQUEST_SIZE);
	assert(fixture.memory.command_external[
	    INTEL_AX211_WIDE_COMMAND_HEADER_SIZE + 45U] == 1U);
	assert(fixture.memory.command_external[
	    INTEL_AX211_WIDE_COMMAND_HEADER_SIZE + 52U] == 6U);

	ack_length = test_make_ack(ack, sizeof(ack),
	    INTEL_AX211_SCAN_REQUEST_OPCODE,
	    &fixture.session.command_handle, NULL, 0U);
	ack[6]++;
	assert(intel_ax211_scan_session_start_ack(&fixture.session, ack,
	    ack_length, TEST_EPOCH, 101U) ==
	    INTEL_AX211_SCAN_SESSION_OUT_OF_ORDER);
	ack[6]--;
	assert(intel_ax211_scan_session_start_ack(&fixture.session, ack,
	    ack_length, TEST_EPOCH + 1U, 101U) ==
	    INTEL_AX211_SCAN_SESSION_STALE);
	assert(intel_ax211_scan_session_start_ack(&fixture.session, ack,
	    ack_length, TEST_EPOCH, 101U) == INTEL_AX211_SCAN_SESSION_OK);
	assert(fixture.session.phase == INTEL_AX211_SCAN_SESSION_RUNNING);
	assert(intel_ax211_scan_session_start_ack(&fixture.session, ack,
	    ack_length, TEST_EPOCH, 102U) ==
	    INTEL_AX211_SCAN_SESSION_DUPLICATE);
	assert(intel_ax211_command_pending_count(&fixture.command) == 0U);

	/* API89 publishes fixed 112-entry storage while only the count prefix is
	 * live.  Exercise the observed 912-byte notification shape directly. */
	test_make_iteration(&message, payload, sizeof(payload), TEST_EPOCH + 1U,
	    6U, 1U);
	assert(intel_ax211_scan_session_notification(&fixture.session,
	    &message, 200U, &reported) == INTEL_AX211_SCAN_SESSION_STALE);
	message.generation = TEST_EPOCH;
	message.version = 2U;
	assert(intel_ax211_scan_session_notification(&fixture.session,
	    &message, 200U, &reported) ==
	    INTEL_AX211_SCAN_SESSION_UNSUPPORTED);
	message.version = INTEL_AX211_SCAN_NOTIFICATION_VERSION;
	assert(intel_ax211_scan_session_notification(&fixture.session,
	    &message, 200U, &reported) ==
	    INTEL_AX211_SCAN_SESSION_OK);
	assert(reported.common_generation == generation);
	assert(reported.channel == 6U);
	assert(reported.firmware.kind ==
	    INTEL_AX211_SCAN_EVENT_ITERATION_COMPLETE);
	assert(fixture.session.phase == INTEL_AX211_SCAN_SESSION_RUNNING);
	assert(intel_ax211_scan_session_notification(&fixture.session,
	    &message, 201U, &reported) ==
	    INTEL_AX211_SCAN_SESSION_OK);
	test_make_complete(&message, payload, TEST_EPOCH, 1U);
	assert(intel_ax211_scan_session_notification(&fixture.session,
	    &message, 202U, &reported) ==
	    INTEL_AX211_SCAN_SESSION_COMPLETE);
	assert(fixture.session.phase == INTEL_AX211_SCAN_SESSION_TERMINAL);

	/* The next common step may reuse its 64-bit generation on a new channel. */
	assert(intel_ax211_scan_session_begin_channel(&fixture.session,
	    generation, 36U, 300U) == INTEL_AX211_SCAN_SESSION_OK);
	assert(fixture.session.channel == 36U);
	assert(fixture.memory.command_external[
	    INTEL_AX211_WIDE_COMMAND_HEADER_SIZE + 48U] == 0U);
	assert(fixture.memory.command_external[
	    INTEL_AX211_WIDE_COMMAND_HEADER_SIZE + 49U] == 0U);
	assert(fixture.memory.command_external[
	    INTEL_AX211_WIDE_COMMAND_HEADER_SIZE + 50U] == 0U);
	assert(fixture.memory.command_external[
	    INTEL_AX211_WIDE_COMMAND_HEADER_SIZE + 51U] == 0U);
	assert(fixture.memory.command_external[
	    INTEL_AX211_WIDE_COMMAND_HEADER_SIZE + 52U] == 36U);
	ack_length = test_make_ack(ack, sizeof(ack),
	    INTEL_AX211_SCAN_REQUEST_OPCODE,
	    &fixture.session.command_handle, NULL, 0U);
	assert(intel_ax211_scan_session_start_ack(&fixture.session, ack,
	    ack_length, TEST_EPOCH, 301U) == INTEL_AX211_SCAN_SESSION_OK);
}

static void
test_abort_and_finite_completion(void)
{
	struct test_fixture fixture;
	struct intel_ax211_protocol_message message;
	struct intel_ax211_scan_session_event reported;
	uint8_t payload[16];
	uint8_t ack[12];
	uint8_t *slot;
	size_t ack_length;

	test_fixture_init(&fixture);
	test_start(&fixture, UINT64_C(77), 1U, 1000U);
	assert(intel_ax211_scan_session_abort(&fixture.session, 78U,
	    1100U) == INTEL_AX211_SCAN_SESSION_STALE);
	assert(intel_ax211_scan_session_abort(&fixture.session, 77U,
	    1100U) == INTEL_AX211_SCAN_SESSION_OK);
	assert(fixture.session.phase ==
	    INTEL_AX211_SCAN_SESSION_WAIT_ABORT_ACK);
	slot = fixture.memory.command_slots +
	    (size_t)fixture.session.command_handle.token.index *
	    INTEL_AX211_TRANSPORT_COMMAND_SLOT_SIZE;
	assert(slot[0] == INTEL_AX211_SCAN_ABORT_OPCODE);
	assert(slot[1] == INTEL_AX211_SCAN_GROUP_LONG);
	assert(slot[7] == 0U);
	assert(test_get_le16(slot + 4U) == INTEL_AX211_SCAN_ABORT_SIZE);

	test_make_complete(&message, payload, TEST_EPOCH, 2U);
	assert(intel_ax211_scan_session_notification(&fixture.session,
	    &message, 1101U, &reported) ==
	    INTEL_AX211_SCAN_SESSION_OUT_OF_ORDER);
	ack_length = test_make_abort_ack(ack, sizeof(ack),
	    &fixture.session.command_handle, 1U);
	assert(intel_ax211_scan_session_abort_ack(&fixture.session, ack,
	    ack_length, TEST_EPOCH, 1102U) == INTEL_AX211_SCAN_SESSION_OK);
	assert(fixture.session.phase ==
	    INTEL_AX211_SCAN_SESSION_WAIT_ABORT_COMPLETE);
	assert(intel_ax211_scan_session_abort_ack(&fixture.session, ack,
	    ack_length, TEST_EPOCH, 1102U) ==
	    INTEL_AX211_SCAN_SESSION_DUPLICATE);
	assert(intel_ax211_scan_session_notification(&fixture.session,
	    &message, 1103U, &reported) ==
	    INTEL_AX211_SCAN_SESSION_ABORTED);
	assert(reported.common_generation == 77U && reported.channel == 1U);

	/* An acknowledged abort which never terminates is bounded. */
	test_fixture_init(&fixture);
	test_start(&fixture, UINT64_C(88), 6U, 2000U);
	assert(intel_ax211_scan_session_abort(&fixture.session, 88U,
	    2100U) == INTEL_AX211_SCAN_SESSION_OK);
	ack_length = test_make_abort_ack(ack, sizeof(ack),
	    &fixture.session.command_handle, 0U);
	assert(intel_ax211_scan_session_abort_ack(&fixture.session, ack,
	    ack_length, TEST_EPOCH, 2101U) == INTEL_AX211_SCAN_SESSION_OK);
	assert(intel_ax211_scan_session_expire(&fixture.session,
	    2100U + INTEL_AX211_SCAN_SESSION_ABORT_TIMEOUT_US) ==
	    INTEL_AX211_SCAN_SESSION_TIMEOUT);

	/* A scan watchdog timeout admits one checked abort before reuse. */
	test_fixture_init(&fixture);
	test_start(&fixture, UINT64_C(99), 11U, 3000U);
	assert(intel_ax211_scan_session_expire(&fixture.session,
	    3000U + INTEL_AX211_SCAN_WATCHDOG_US) ==
	    INTEL_AX211_SCAN_SESSION_TIMEOUT);
	assert(intel_ax211_scan_session_begin_channel(&fixture.session, 100U,
	    1U, 9000000U) == INTEL_AX211_SCAN_SESSION_BUSY);
	assert(intel_ax211_scan_session_abort(&fixture.session, 99U,
	    9000001U) == INTEL_AX211_SCAN_SESSION_OK);
	ack_length = test_make_abort_ack(ack, sizeof(ack),
	    &fixture.session.command_handle, 0U);
	assert(intel_ax211_scan_session_abort_ack(&fixture.session, ack,
	    ack_length, TEST_EPOCH, 9000002U) == INTEL_AX211_SCAN_SESSION_OK);
	assert(fixture.session.phase ==
	    INTEL_AX211_SCAN_SESSION_WAIT_ABORT_COMPLETE);
	assert(fixture.session.scan.phase == INTEL_AX211_SCAN_PHASE_RUNNING);
	assert(fixture.session.scan.abort_required == 1U);
	assert(fixture.session.scan.scan_deadline ==
	    fixture.session.command_deadline);
	assert(intel_ax211_scan_session_begin_channel(&fixture.session, 100U,
	    1U, 9000003U) == INTEL_AX211_SCAN_SESSION_BUSY);
	test_make_complete(&message, payload, TEST_EPOCH, 2U);
	assert(intel_ax211_scan_session_notification(&fixture.session,
	    &message, 9000003U, &reported) ==
	    INTEL_AX211_SCAN_SESSION_ABORTED);
	assert(fixture.session.scan.abort_required == 0U);
	test_start(&fixture, UINT64_C(100), 1U, 9100000U);

	/* NOT_FOUND is a successful finite stop with no completion to await. */
	test_fixture_init(&fixture);
	test_start(&fixture, UINT64_C(101), 6U, 9200000U);
	assert(intel_ax211_scan_session_abort(&fixture.session, 101U,
	    9200100U) == INTEL_AX211_SCAN_SESSION_OK);
	ack_length = test_make_abort_ack(ack, sizeof(ack),
	    &fixture.session.command_handle, 2U);
	assert(intel_ax211_scan_session_abort_ack(&fixture.session, ack,
	    ack_length, TEST_EPOCH, 9200101U) ==
	    INTEL_AX211_SCAN_SESSION_ABORTED);
	assert(fixture.session.phase == INTEL_AX211_SCAN_SESSION_TERMINAL);

	/* Missing or unknown status payloads cannot silently stop a scan. */
	test_fixture_init(&fixture);
	test_start(&fixture, UINT64_C(102), 6U, 9300000U);
	assert(intel_ax211_scan_session_abort(&fixture.session, 102U,
	    9300100U) == INTEL_AX211_SCAN_SESSION_OK);
	ack_length = test_make_ack(ack, sizeof(ack),
	    INTEL_AX211_SCAN_ABORT_OPCODE, &fixture.session.command_handle,
	    NULL, 0U);
	assert(intel_ax211_scan_session_abort_ack(&fixture.session, ack,
	    ack_length, TEST_EPOCH, 9300101U) ==
	    INTEL_AX211_SCAN_SESSION_COMMAND);

	test_fixture_init(&fixture);
	test_start(&fixture, UINT64_C(103), 6U, 9400000U);
	assert(intel_ax211_scan_session_abort(&fixture.session, 103U,
	    9400100U) == INTEL_AX211_SCAN_SESSION_OK);
	ack_length = test_make_abort_ack(ack, sizeof(ack),
	    &fixture.session.command_handle, 3U);
	assert(intel_ax211_scan_session_abort_ack(&fixture.session, ack,
	    ack_length, TEST_EPOCH, 9400101U) ==
	    INTEL_AX211_SCAN_SESSION_COMMAND);
}

static void
test_command_ack_timeout(void)
{
	struct test_fixture fixture;

	test_fixture_init(&fixture);
	assert(intel_ax211_scan_session_begin_channel(&fixture.session,
	    UINT64_C(91), 6U, 5000U) == INTEL_AX211_SCAN_SESSION_OK);
	assert(intel_ax211_scan_session_expire(&fixture.session,
	    5000U + INTEL_AX211_SCAN_SESSION_COMMAND_TIMEOUT_US - 1U) ==
	    INTEL_AX211_SCAN_SESSION_OK);
	assert(intel_ax211_scan_session_expire(&fixture.session,
	    5000U + INTEL_AX211_SCAN_SESSION_COMMAND_TIMEOUT_US) ==
	    INTEL_AX211_SCAN_SESSION_TIMEOUT);
	assert(intel_ax211_command_is_poisoned(&fixture.command));
	assert(intel_ax211_scan_session_begin_channel(&fixture.session,
	    UINT64_C(92), 11U, 7000U) == INTEL_AX211_SCAN_SESSION_BUSY);
}

static void
test_exact_ack_and_validation(void)
{
	static const uint8_t address[6] = {
		0x02U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U
	};
	struct test_fixture fixture;
	uint8_t ack[9];
	uint8_t extra;
	size_t ack_length;

	test_fixture_init(&fixture);
	assert(intel_ax211_scan_session_begin_channel(&fixture.session,
	    UINT64_C(101), 2U, 100U) ==
	    INTEL_AX211_SCAN_SESSION_UNSUPPORTED);
	assert(intel_ax211_scan_session_begin_channel(&fixture.session,
	    UINT64_C(101), 1U, 100U) == INTEL_AX211_SCAN_SESSION_OK);
	extra = 0U;
	ack_length = test_make_ack(ack, sizeof(ack),
	    INTEL_AX211_SCAN_REQUEST_OPCODE, &fixture.session.command_handle,
	    &extra, 1U);
	assert(intel_ax211_scan_session_start_ack(&fixture.session, ack,
	    ack_length, TEST_EPOCH, 101U) == INTEL_AX211_SCAN_SESSION_COMMAND);
	assert(fixture.session.phase == INTEL_AX211_SCAN_SESSION_TERMINAL);

	test_fixture_init(&fixture);
	fixture.versions[2U * 4U + 2U] = 18U;
	assert(intel_ax211_protocol_command_table_parse(fixture.versions,
	    sizeof(fixture.versions), &fixture.table) ==
	    INTEL_AX211_PROTOCOL_OK);
	memset(&fixture.session, 0, sizeof(fixture.session));
	assert(intel_ax211_scan_session_init(&fixture.session,
	    &fixture.command, &fixture.table, &fixture.nvm, &fixture.mcc,
	    address, TEST_EPOCH) == INTEL_AX211_SCAN_SESSION_UNSUPPORTED);
}
