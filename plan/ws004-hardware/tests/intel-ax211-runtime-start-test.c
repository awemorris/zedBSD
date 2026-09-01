/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * Intel AX211 operational firmware coordinator fixture
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../../src/drivers/intel-ax211-runtime-start.h"

#define TEST_EVENT_CAPACITY                            64U
#define TEST_TRACE_CAPACITY                          8192U
#define TEST_COMMAND_CAPACITY                          64U
#define TEST_COMMAND_QUEUE                              4U
#define TEST_PNVM_CAPACITY                            256U

enum test_event_type {
	TEST_EVENT_ALIVE = 1,
	TEST_EVENT_PNVM = 2,
	TEST_EVENT_ACK_EXTENDED = 3,
	TEST_EVENT_ACK_ACCESS = 4,
	TEST_EVENT_INIT = 5,
	TEST_EVENT_ACK_TX_ANT = 6,
	TEST_EVENT_ACK_BT = 7,
	TEST_EVENT_ACK_SOC = 8,
	TEST_EVENT_ACK_LTR = 9,
	TEST_EVENT_ACK_TEMP = 10,
	TEST_EVENT_ACK_POWER = 11,
	TEST_EVENT_ACK_MCC = 12,
	TEST_EVENT_ACK_SCAN = 13,
	TEST_EVENT_ACK_BEACON = 14,
	TEST_EVENT_MALFORMED = 15,
	TEST_EVENT_TIMEOUT = 16,
	TEST_EVENT_MALFORMED_MCC = 17,
	TEST_EVENT_POWER_STATUS_FAILED = 18,
	TEST_EVENT_MALFORMED_POWER = 19
};

enum test_fail_stage {
	TEST_FAIL_NONE = 0,
	TEST_FAIL_EPOCH = 1,
	TEST_FAIL_RUNTIME_INTERRUPTS = 2,
	TEST_FAIL_DRAIN = 3,
	TEST_FAIL_STOP = 4,
	TEST_FAIL_DQA_PROFILE = 5,
	TEST_FAIL_BIND = 6,
	TEST_FAIL_NIC_LOCK = 7,
	TEST_FAIL_NIC_UNLOCK = 8
};

struct test_event {
	uint8_t type;
	uint8_t stale;
	uint8_t failed;
	uint8_t queued_before_epoch;
	uint8_t fixed_generation;
	uint32_t generation;
};

struct test_command {
	uint8_t group;
	uint8_t opcode;
	uint8_t version;
};

struct test_fixture {
	struct intel_ax211_runtime_start session;
	struct intel_ax211_mmio mmio;
	struct intel_ax211_transport transport;
	struct intel_ax211_runtime_start_ops ops;
	struct drv_dma_device *dma_device;
	struct intel_ax211_protocol_command_table table;
	struct intel_ax211_protocol_nvm nvm;
	struct test_event event[TEST_EVENT_CAPACITY];
	size_t event_count;
	size_t event_index;
	struct test_command command[TEST_COMMAND_CAPACITY];
	size_t command_count;
	uint64_t clock;
	uint8_t firmware[INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES];
	uint8_t pnvm[TEST_PNVM_CAPACITY];
	size_t pnvm_length;
	uint8_t context[INTEL_AX211_CONTEXT_INFO_GEN3_SIZE];
	uint8_t iml[INTEL_AX211_MMIO_IML_SIZE];
	uint8_t rx_byte;
	char trace[TEST_TRACE_CAPACITY];
	size_t trace_length;
	uint32_t receive_generation;
	uint32_t bound_generation;
	int fail_stage;
	size_t epoch_begin_count;
	size_t queued_dropped;
	uint8_t receive_enabled;
	uint8_t source_released;
	uint8_t dma_released;
	uint8_t runtime_interrupts;
	uint8_t stop_fails;
	uint8_t nic_owned;
	uint8_t unlock_fails;
};

static struct test_fixture *active_fixture;

static void test_put_le16(uint8_t *bytes, uint16_t value);
static void test_put_le32(uint8_t *bytes, uint32_t value);
static size_t test_append_tlv(uint8_t *bytes, size_t capacity,
	size_t offset, uint32_t type, const void *payload, uint32_t length);
static void test_trace(struct test_fixture *fixture, char value);
static size_t test_trace_find(const struct test_fixture *fixture,
	char value, size_t start);
static void test_command_table_build(struct test_fixture *fixture);
static void test_pnvm_build(struct test_fixture *fixture);
static void test_fixture_init(struct test_fixture *fixture);
static void test_event_add(struct test_fixture *fixture, uint8_t type,
	uint8_t stale, uint8_t failed);
static void test_event_add_generation(struct test_fixture *fixture,
	uint8_t type, uint32_t generation, uint8_t queued_before_epoch);
static void test_success_events(struct test_fixture *fixture);
static size_t test_event_make(struct test_fixture *fixture,
	const struct test_event *script, uint8_t *bytes, size_t capacity,
	uint8_t *version, uint32_t *generation);
static size_t test_packet_make(uint8_t *bytes, size_t capacity,
	uint8_t opcode, uint8_t flags, uint8_t index, uint8_t queue,
	const uint8_t *payload, size_t payload_length);
static void test_alive_payload(
	uint8_t payload[INTEL_AX211_PROTOCOL_ALIVE_SIZE]);
static size_t test_mcc_payload(uint8_t *payload, size_t capacity,
	int malformed);
static int test_receive_epoch_begin(void *argument, uint32_t generation);
static int test_transport_bind(void *argument,
	struct intel_ax211_dma_resources *dma, struct intel_ax211_mmio *mmio,
	struct intel_ax211_transport *transport, uint32_t generation);
static int test_receive_event(void *argument, uint64_t deadline,
	uint8_t *bytes, size_t capacity,
	struct intel_ax211_boot_received_event *event);
static int test_publish_pnvm(void *argument,
	struct intel_ax211_dma_resources *dma);
static int test_post_alive(void *argument,
	const struct intel_ax211_protocol_alive *alive);
static int test_interrupt_drain(void *argument);
static int test_clock_us(void *argument, uint64_t *time_us);
static int test_nic_lock(void *argument);
static int test_nic_unlock(void *argument);
static void test_success_retains_then_stops(void);
static void test_malformed_timeout_and_stale(void);
static void test_runtime_failure_unwind(void);
static void test_stop_retry(void);
static void test_generation_isolation_and_wrap(void);
static void test_epoch_and_bind_failures(void);
static void test_nic_ownership_failures(void);

/* Returns one fresh exact synthetic firmware/PNVM pair. */
int
intel_ax211_firmware_files_load(
	struct intel_ax211_firmware_files *files)
{
	struct test_fixture *fixture;

	fixture = active_fixture;
	assert(fixture != NULL);
	test_trace(fixture, 'L');
	test_command_table_build(fixture);
	test_pnvm_build(fixture);
	fixture->source_released = 0U;
	memset(files, 0, sizeof(*files));
	files->ucode_bytes = fixture->firmware;
	files->ucode_size = sizeof(fixture->firmware);
	files->ucode_manifest.command_versions_offset = 0U;
	files->ucode_manifest.command_versions_length =
	    sizeof(fixture->firmware);
	files->ucode_manifest.api_changes[0] |= UINT32_C(1) << 9U;
	files->ucode_manifest.api_changes[1] |= UINT32_C(1) << 24U;
	files->ucode_manifest.api_changes[1] |= UINT32_C(1) << 26U;
	files->ucode_manifest.capabilities[0] |= UINT32_C(1) << 9U;
	files->ucode_manifest.capabilities[2] |= UINT32_C(1) << 10U;
	files->ucode_manifest.capabilities[2] |= UINT32_C(1) << 25U;
	if (fixture->fail_stage == TEST_FAIL_DQA_PROFILE)
		files->ucode_manifest.capabilities[0] |= UINT32_C(1) << 12U;
	files->pnvm_bytes = fixture->pnvm;
	files->pnvm_size = fixture->pnvm_length;
	return 0;
}

void
intel_ax211_firmware_files_release(
	struct intel_ax211_firmware_files *files)
{
	struct test_fixture *fixture;

	fixture = active_fixture;
	assert(fixture != NULL);
	assert(!fixture->source_released);
	test_trace(fixture, 'F');
	memset(fixture->firmware, 0xa5, sizeof(fixture->firmware));
	memset(fixture->pnvm, 0xa5, sizeof(fixture->pnvm));
	memset(files, 0, sizeof(*files));
	fixture->source_released = 1U;
}

int
intel_ax211_dma_prepare_boot(
	struct drv_dma_device *device,
	const uint8_t *firmware_bytes,
	size_t firmware_length,
	const struct intel_ax211_firmware_manifest *manifest,
	uint16_t hardware_revision,
	struct intel_ax211_dma_resources *resources)
{
	struct test_fixture *fixture;
	size_t index;

	fixture = active_fixture;
	assert(device == fixture->dma_device);
	assert(firmware_bytes == fixture->firmware);
	assert(firmware_length == sizeof(fixture->firmware));
	assert(manifest->command_versions_length == firmware_length);
	assert(hardware_revision == 0x0370U);
	test_trace(fixture, 'B');
	fixture->dma_released = 0U;
	memset(resources, 0, sizeof(*resources));
	resources->device = device;
	resources->context.address = fixture->context;
	resources->context.device_address = 0x100000U;
	resources->context.size = sizeof(fixture->context);
	resources->iml.address = fixture->iml;
	resources->iml.device_address = 0x200000U;
	resources->iml.size = sizeof(fixture->iml);
	resources->rx_buffer_count = INTEL_AX211_RX_RING_SIZE;
	for (index = 0U; index < resources->rx_buffer_count; index++) {
		resources->rx_buffer[index].address = &fixture->rx_byte;
		resources->rx_buffer[index].device_address = 0x300000U +
		    (uint64_t)index * INTEL_AX211_RX_BUFFER_SIZE;
		resources->rx_buffer[index].size = INTEL_AX211_RX_BUFFER_SIZE;
	}
	resources->boot_prepared = 1U;
	return 0;
}

void
intel_ax211_dma_release_boot_images(
	struct intel_ax211_dma_resources *resources)
{
	assert(resources->boot_prepared);
	assert(!resources->boot_images_released);
	assert(active_fixture->session.alive_accepted);
	test_trace(active_fixture, 'b');
	resources->boot_images_released = 1U;
}

int
intel_ax211_dma_prepare_pnvm(
	const uint8_t *pnvm_bytes,
	size_t pnvm_length,
	const struct intel_ax211_pnvm_manifest *manifest,
	struct intel_ax211_dma_resources *resources)
{
	assert(pnvm_bytes == active_fixture->pnvm);
	assert(pnvm_length == active_fixture->pnvm_length);
	assert(manifest->section_count == 1U);
	assert(resources->boot_images_released);
	test_trace(active_fixture, 'N');
	resources->pnvm_prepared = 1U;
	return 0;
}

void
intel_ax211_dma_release(
	struct intel_ax211_dma_resources *resources)
{
	assert(!active_fixture->dma_released);
	test_trace(active_fixture, 'f');
	memset(resources, 0, sizeof(*resources));
	active_fixture->dma_released = 1U;
}

int
intel_ax211_mmio_prepare_card_hw(
	struct intel_ax211_mmio *mmio)
{
	assert(mmio == &active_fixture->mmio);
	test_trace(active_fixture, 'h');
	return INTEL_AX211_MMIO_OK;
}

int
intel_ax211_mmio_sw_reset(
	struct intel_ax211_mmio *mmio)
{
	assert(mmio == &active_fixture->mmio);
	test_trace(active_fixture, 'r');
	return INTEL_AX211_MMIO_OK;
}

int
intel_ax211_mmio_apm_init(
	struct intel_ax211_mmio *mmio)
{
	assert(mmio == &active_fixture->mmio);
	test_trace(active_fixture, 'a');
	return INTEL_AX211_MMIO_OK;
}

int
intel_ax211_mmio_publish_gen3(
	struct intel_ax211_mmio *mmio,
	const struct intel_ax211_mmio_boot *boot)
{
	assert(mmio == &active_fixture->mmio);
	assert(boot->context_address == 0x100000U);
	assert(boot->iml_address == 0x200000U);
	assert(boot->iml_size == INTEL_AX211_MMIO_IML_SIZE);
	test_trace(active_fixture, 'G');
	return INTEL_AX211_MMIO_OK;
}

int
intel_ax211_mmio_stop(
	struct intel_ax211_mmio *mmio)
{
	assert(mmio == &active_fixture->mmio);
	test_trace(active_fixture, 's');
	if (active_fixture->fail_stage == TEST_FAIL_STOP ||
	    active_fixture->stop_fails)
		return INTEL_AX211_MMIO_TIMEOUT;
	active_fixture->nic_owned = 0U;
	return INTEL_AX211_MMIO_OK;
}

int
intel_ax211_transport_configure_msix(
	struct intel_ax211_transport *transport)
{
	test_trace(active_fixture, 'X');
	transport->msix_configured = 1U;
	return INTEL_AX211_TRANSPORT_OK;
}

int
intel_ax211_transport_initialize_rings(
	struct intel_ax211_transport *transport)
{
	int result;

	test_trace(active_fixture, 'R');
	result = intel_ax211_ring_init(&transport->command_ring,
	    TEST_COMMAND_QUEUE, INTEL_AX211_COMMAND_RING_SIZE);
	assert(result == INTEL_AX211_OK);
	transport->rings_initialized = 1U;
	transport->command_reset_completed = 1U;
	return INTEL_AX211_TRANSPORT_OK;
}

int
intel_ax211_transport_publish_rx_descriptor(
	struct intel_ax211_transport *transport,
	uint16_t index,
	uint64_t device_address)
{
	(void)transport;
	assert(index < INTEL_AX211_RX_RING_SIZE);
	assert(device_address != 0U);
	if (index == 0U)
		test_trace(active_fixture, 'D');
	return INTEL_AX211_TRANSPORT_OK;
}

int
intel_ax211_transport_activate_rx(
	struct intel_ax211_transport *transport)
{
	test_trace(active_fixture, 'A');
	transport->rx_active = 1U;
	return INTEL_AX211_TRANSPORT_OK;
}

int
intel_ax211_transport_enable_firmware_interrupts(
	struct intel_ax211_transport *transport)
{
	test_trace(active_fixture, 'I');
	transport->interrupts_enabled = 1U;
	return INTEL_AX211_TRANSPORT_OK;
}

int
intel_ax211_transport_enable_runtime_interrupts(
	struct intel_ax211_transport *transport)
{
	test_trace(active_fixture, 'J');
	if (active_fixture->fail_stage == TEST_FAIL_RUNTIME_INTERRUPTS)
		return INTEL_AX211_TRANSPORT_IO;
	transport->firmware_load_mode = 0U;
	active_fixture->runtime_interrupts = 1U;
	return INTEL_AX211_TRANSPORT_OK;
}

int
intel_ax211_transport_command_prepare_inline(
	struct intel_ax211_transport *transport,
	const struct intel_ax211_command_id *command,
	const void *payload,
	size_t payload_length,
	struct intel_ax211_ring_token *token)
{
	struct test_fixture *fixture;
	int result;

	(void)payload;
	fixture = active_fixture;
	assert(command != NULL);
	assert(command->version == 0U);
	assert(payload_length <= INTEL_AX211_COMMAND_INLINE_PAYLOAD_SIZE);
	assert(fixture->command_count < TEST_COMMAND_CAPACITY);
	result = intel_ax211_ring_reserve(&transport->command_ring, token);
	assert(result == INTEL_AX211_OK);
	transport->command_prepared_token = *token;
	transport->command_prepared = 1U;
	fixture->command[fixture->command_count].group = command->group;
	fixture->command[fixture->command_count].opcode = command->opcode;
	fixture->command[fixture->command_count].version = command->version;
	fixture->command_count++;
	test_trace(fixture, 'c');
	return INTEL_AX211_TRANSPORT_OK;
}

int
intel_ax211_transport_command_prepare_external(
	struct intel_ax211_transport *transport,
	const struct intel_ax211_command_id *command,
	const void *payload,
	size_t payload_length,
	struct intel_ax211_ring_token *token)
{
	(void)transport;
	(void)command;
	(void)payload;
	(void)payload_length;
	(void)token;
	assert(0);
	return INTEL_AX211_TRANSPORT_INVALID;
}

int
intel_ax211_transport_command_publish(
	struct intel_ax211_transport *transport,
	const struct intel_ax211_ring_token *token)
{
	assert(transport->command_prepared);
	assert(token->queue == transport->command_prepared_token.queue);
	assert(token->index == transport->command_prepared_token.index);
	transport->command_prepared = 0U;
	return INTEL_AX211_TRANSPORT_OK;
}

int
intel_ax211_transport_command_abort_prepared(
	struct intel_ax211_transport *transport,
	const struct intel_ax211_ring_token *token)
{
	(void)token;
	transport->command_prepared = 0U;
	transport->command_ring.head--;
	transport->command_ring.used--;
	return INTEL_AX211_TRANSPORT_OK;
}

int
intel_ax211_transport_command_complete(
	struct intel_ax211_transport *transport,
	const struct intel_ax211_ring_token *token)
{
	int result;

	result = intel_ax211_ring_complete(&transport->command_ring, token);
	return result == INTEL_AX211_OK ? INTEL_AX211_TRANSPORT_OK :
	    INTEL_AX211_TRANSPORT_STALE;
}

size_t
intel_ax211_transport_command_pending_count(
	const struct intel_ax211_transport *transport)
{
	return transport->command_ring.used;
}

int
intel_ax211_transport_command_oldest(
	const struct intel_ax211_transport *transport,
	struct intel_ax211_ring_token *token)
{
	if (transport->command_ring.used == 0U)
		return INTEL_AX211_TRANSPORT_STALE;
	token->queue = transport->command_ring.queue;
	token->index = (uint8_t)transport->command_ring.tail;
	return INTEL_AX211_TRANSPORT_OK;
}

int
intel_ax211_transport_quiesce(
	struct intel_ax211_transport *transport)
{
	test_trace(active_fixture, 'q');
	transport->quiesced = 1U;
	transport->rx_dma_idle = 1U;
	if (transport->command_ring.used != 0U) {
		transport->command_reset_required = 1U;
		return INTEL_AX211_TRANSPORT_FAILED;
	}
	return INTEL_AX211_TRANSPORT_OK;
}

int
intel_ax211_transport_command_after_device_reset(
	struct intel_ax211_transport *transport)
{
	int result;

	test_trace(active_fixture, 'z');
	result = intel_ax211_ring_init(&transport->command_ring,
	    TEST_COMMAND_QUEUE, INTEL_AX211_COMMAND_RING_SIZE);
	assert(result == INTEL_AX211_OK);
	transport->command_prepared = 0U;
	transport->command_reset_required = 0U;
	transport->command_reset_completed = 1U;
	return INTEL_AX211_TRANSPORT_OK;
}

static void
test_put_le16(
	uint8_t *bytes,
	uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

static void
test_put_le32(
	uint8_t *bytes,
	uint32_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

static size_t
test_append_tlv(
	uint8_t *bytes,
	size_t capacity,
	size_t offset,
	uint32_t type,
	const void *payload,
	uint32_t length)
{
	size_t padded;

	padded = ((size_t)length + 3U) & ~(size_t)3U;
	assert(offset <= capacity);
	assert(capacity - offset >= 8U + padded);
	test_put_le32(bytes + offset, type);
	test_put_le32(bytes + offset + 4U, length);
	if (length != 0U)
		memcpy(bytes + offset + 8U, payload, length);
	if (padded != length)
		memset(bytes + offset + 8U + length, 0, padded - length);
	return offset + 8U + padded;
}

static void
test_trace(
	struct test_fixture *fixture,
	char value)
{
	assert(fixture->trace_length + 1U < sizeof(fixture->trace));
	fixture->trace[fixture->trace_length++] = value;
	fixture->trace[fixture->trace_length] = '\0';
}

static size_t
test_trace_find(
	const struct test_fixture *fixture,
	char value,
	size_t start)
{
	size_t index;

	for (index = start; index < fixture->trace_length; index++) {
		if (fixture->trace[index] == value)
			return index;
	}
	fprintf(stderr, "missing trace %c after %lu in %s\n", value,
	    (unsigned long)start, fixture->trace);
	assert(0);
	return 0U;
}

static void
test_command_table_build(
	struct test_fixture *fixture)
{
	static const uint8_t required[][4] = {
		{ 0x01U, 0x00U, 99U, 6U },
		{ 0x0cU, 0x01U, 5U, 0U },
		{ 0x0dU, 0x01U, 17U, 0U },
		{ 0x00U, 0x0cU, 1U, 0U },
		{ 0x02U, 0x0cU, 1U, 4U },
		{ 0xfeU, 0x0cU, 99U, 1U },
		{ 0x98U, 0x01U, 1U, 0U },
		{ 0x9bU, 0x01U, 6U, 0U },
		{ 0x01U, 0x02U, 2U, 0U },
		{ 0xeeU, 0x01U, 3U, 0U },
		{ 0x04U, 0x04U, 1U, 0U },
		{ 0x77U, 0x01U, 7U, 0U },
		{ 0xc8U, 0x01U, 1U, 6U },
		{ 0xd2U, 0x01U, 4U, 0U },
		{ 0x0eU, 0x01U, 1U, 0U }
	};
	size_t index;

	memset(fixture->firmware, 0, sizeof(fixture->firmware));
	for (index = 0U;
	     index < sizeof(required) / sizeof(required[0]); index++)
		memcpy(fixture->firmware + index * 4U, required[index], 4U);
}

static void
test_pnvm_build(
	struct test_fixture *fixture)
{
	uint8_t hardware[4];
	uint8_t section[7];
	uint8_t sku[12];
	uint8_t version[4];
	size_t offset;

	test_put_le32(sku, 1U);
	test_put_le32(sku + 4U, 2U);
	test_put_le32(sku + 8U, 3U);
	test_put_le32(version, 1U);
	test_put_le16(hardware, INTEL_AX211_MAC_TYPE_SO);
	test_put_le16(hardware + 2U, INTEL_AX211_RF_TYPE);
	memset(section, 0, sizeof(section));
	test_put_le32(section, 0x5000U);
	section[4] = 1U;
	section[5] = 2U;
	section[6] = 3U;
	memset(fixture->pnvm, 0, sizeof(fixture->pnvm));
	offset = 0U;
	offset = test_append_tlv(fixture->pnvm, sizeof(fixture->pnvm),
	    offset, 64U, sku, sizeof(sku));
	offset = test_append_tlv(fixture->pnvm, sizeof(fixture->pnvm),
	    offset, 62U, version, sizeof(version));
	offset = test_append_tlv(fixture->pnvm, sizeof(fixture->pnvm),
	    offset, 58U, hardware, sizeof(hardware));
	offset = test_append_tlv(fixture->pnvm, sizeof(fixture->pnvm),
	    offset, 19U, section, sizeof(section));
	fixture->pnvm_length = offset;
}

static void
test_fixture_init(
	struct test_fixture *fixture)
{
	int result;

	memset(fixture, 0, sizeof(*fixture));
	active_fixture = fixture;
	test_command_table_build(fixture);
	test_pnvm_build(fixture);
	fixture->clock = 100U;
	fixture->dma_device =
	    (struct drv_dma_device *)(void *)&fixture->rx_byte;
	fixture->mmio.profile.mac_type = INTEL_AX211_MAC_TYPE_SO;
	fixture->mmio.profile.rf_type = INTEL_AX211_RF_TYPE;
	result = intel_ax211_protocol_command_table_parse(fixture->firmware,
	    sizeof(fixture->firmware), &fixture->table);
	assert(result == INTEL_AX211_PROTOCOL_OK);
	fixture->nvm.nvm_version = 0x1234U;
	fixture->nvm.tx_chain_mask = 1U;
	fixture->nvm.rx_chain_mask = 1U;
	fixture->nvm.lar_enabled = 1U;
	fixture->ops.boot.receive_epoch_begin = test_receive_epoch_begin;
	fixture->ops.boot.transport_bind = test_transport_bind;
	fixture->ops.boot.receive_event = test_receive_event;
	fixture->ops.boot.publish_pnvm = test_publish_pnvm;
	fixture->ops.boot.post_alive = test_post_alive;
	fixture->ops.boot.interrupt_drain = test_interrupt_drain;
	fixture->ops.boot.clock_us = test_clock_us;
	fixture->ops.nic_lock = test_nic_lock;
	fixture->ops.nic_unlock = test_nic_unlock;
	result = intel_ax211_runtime_start_init(&fixture->session,
	    &fixture->ops, fixture, fixture->dma_device, &fixture->mmio,
	    &fixture->transport, 0x0370U, INTEL_AX211_RF_TYPE,
	    &fixture->table, &fixture->nvm, 1, 7U);
	assert(result == INTEL_AX211_RUNTIME_START_OK);
}

static void
test_event_add(
	struct test_fixture *fixture,
	uint8_t type,
	uint8_t stale,
	uint8_t failed)
{
	assert(fixture->event_count < TEST_EVENT_CAPACITY);
	fixture->event[fixture->event_count].type = type;
	fixture->event[fixture->event_count].stale = stale;
	fixture->event[fixture->event_count].failed = failed;
	fixture->event_count++;
}

static void
test_event_add_generation(
	struct test_fixture *fixture,
	uint8_t type,
	uint32_t generation,
	uint8_t queued_before_epoch)
{
	assert(fixture->event_count < TEST_EVENT_CAPACITY);
	assert(generation != 0U);
	fixture->event[fixture->event_count].type = type;
	fixture->event[fixture->event_count].fixed_generation = 1U;
	fixture->event[fixture->event_count].generation = generation;
	fixture->event[fixture->event_count].queued_before_epoch =
	    queued_before_epoch;
	fixture->event_count++;
}

static void
test_success_events(
	struct test_fixture *fixture)
{
	test_event_add(fixture, TEST_EVENT_ALIVE, 0U, 0U);
	test_event_add(fixture, TEST_EVENT_PNVM, 0U, 0U);
	test_event_add(fixture, TEST_EVENT_ACK_EXTENDED, 0U, 0U);
	test_event_add(fixture, TEST_EVENT_ACK_ACCESS, 0U, 0U);
	test_event_add(fixture, TEST_EVENT_INIT, 0U, 0U);
	test_event_add(fixture, TEST_EVENT_ACK_TX_ANT, 0U, 0U);
	test_event_add(fixture, TEST_EVENT_ACK_BT, 0U, 0U);
	test_event_add(fixture, TEST_EVENT_ACK_SOC, 0U, 0U);
	test_event_add(fixture, TEST_EVENT_ACK_LTR, 0U, 0U);
	test_event_add(fixture, TEST_EVENT_ACK_TEMP, 0U, 0U);
	test_event_add(fixture, TEST_EVENT_ACK_POWER, 0U, 0U);
	test_event_add(fixture, TEST_EVENT_ACK_MCC, 0U, 0U);
	test_event_add(fixture, TEST_EVENT_ACK_SCAN, 0U, 0U);
	test_event_add(fixture, TEST_EVENT_ACK_BEACON, 0U, 0U);
}

static size_t
test_event_make(
	struct test_fixture *fixture,
	const struct test_event *script,
	uint8_t *bytes,
	size_t capacity,
	uint8_t *version,
	uint32_t *generation)
{
	uint8_t alive[INTEL_AX211_PROTOCOL_ALIVE_SIZE];
	uint8_t mcc[INTEL_AX211_RUNTIME_START_MCC_RESPONSE_MAX];
	uint8_t generic_status[INTEL_AX211_RUNTIME_START_GENERIC_RESPONSE_SIZE];
	uint8_t flags;
	uint8_t opcode;
	uint8_t queue;
	const uint8_t *payload;
	size_t payload_length;
	const struct test_command *command;

	flags = 0U;
	opcode = 0U;
	queue = 0x80U;
	payload = NULL;
	payload_length = 0U;
	*version = 0U;
	if (script->fixed_generation)
		*generation = script->generation;
	else if (script->stale) {
		*generation = fixture->session.generation - 1U;
		if (*generation == 0U)
			*generation = UINT32_MAX;
	} else
		*generation = fixture->session.generation;

	if (script->type == TEST_EVENT_ALIVE) {
		opcode = INTEL_AX211_PROTOCOL_ALIVE_OPCODE;
		*version = INTEL_AX211_PROTOCOL_ALIVE_VERSION;
		test_alive_payload(alive);
		payload = alive;
		payload_length = sizeof(alive);
	} else if (script->type == TEST_EVENT_PNVM) {
		opcode = INTEL_AX211_PROTOCOL_PNVM_INIT_COMPLETE_OPCODE;
		flags = INTEL_AX211_PROTOCOL_GROUP_REGULATORY_NVM;
		*version = INTEL_AX211_PROTOCOL_PNVM_INIT_COMPLETE_VERSION;
	} else if (script->type == TEST_EVENT_INIT) {
		opcode = INTEL_AX211_PROTOCOL_INIT_COMPLETE_OPCODE;
		*version = INTEL_AX211_PROTOCOL_UNKNOWN_VERSION;
	} else if (script->type == TEST_EVENT_MALFORMED) {
		assert(capacity >= 7U);
		memset(bytes, 0, 7U);
		return 7U;
	} else {
		assert(fixture->command_count != 0U);
		command = &fixture->command[fixture->command_count - 1U];
		opcode = command->opcode;
		flags = command->group;
		queue = TEST_COMMAND_QUEUE;
		if (script->type == TEST_EVENT_ACK_MCC ||
		    script->type == TEST_EVENT_MALFORMED_MCC) {
			payload_length = test_mcc_payload(mcc, sizeof(mcc),
			    script->type == TEST_EVENT_MALFORMED_MCC);
			payload = mcc;
		} else if (script->type == TEST_EVENT_ACK_POWER ||
		    script->type == TEST_EVENT_POWER_STATUS_FAILED ||
		    script->type == TEST_EVENT_MALFORMED_POWER) {
			memset(generic_status, 0, sizeof(generic_status));
			if (script->type == TEST_EVENT_POWER_STATUS_FAILED)
				test_put_le32(generic_status, 1U);
			payload = generic_status;
			payload_length = script->type == TEST_EVENT_MALFORMED_POWER ?
			    sizeof(generic_status) - 1U : sizeof(generic_status);
		}
	}
	if (script->failed)
		flags |= INTEL_AX211_PROTOCOL_COMMAND_FAILED_MASK;
	return test_packet_make(bytes, capacity, opcode, flags,
	    queue == 0x80U ? 0U :
	    (uint8_t)fixture->transport.command_ring.tail,
	    queue, payload, payload_length);
}

static size_t
test_packet_make(
	uint8_t *bytes,
	size_t capacity,
	uint8_t opcode,
	uint8_t flags,
	uint8_t index,
	uint8_t queue,
	const uint8_t *payload,
	size_t payload_length)
{
	size_t length;

	length = INTEL_AX211_EVENT_HEADER_SIZE + payload_length;
	assert(length <= capacity);
	memset(bytes, 0, length);
	test_put_le32(bytes, (uint32_t)(4U + payload_length));
	bytes[4] = opcode;
	bytes[5] = flags;
	bytes[6] = index;
	bytes[7] = queue;
	if (payload_length != 0U)
		memcpy(bytes + INTEL_AX211_EVENT_HEADER_SIZE, payload,
		    payload_length);
	return length;
}

static void
test_alive_payload(
	uint8_t payload[INTEL_AX211_PROTOCOL_ALIVE_SIZE])
{
	memset(payload, 0, INTEL_AX211_PROTOCOL_ALIVE_SIZE);
	test_put_le16(payload, INTEL_AX211_PROTOCOL_ALIVE_STATUS_OK);
	test_put_le32(payload + 116U, 1U);
	test_put_le32(payload + 120U, 2U);
	test_put_le32(payload + 124U, 3U);
}

static size_t
test_mcc_payload(
	uint8_t *payload,
	size_t capacity,
	int malformed)
{
	size_t length;

	length = malformed ? 20U : 24U;
	assert(capacity >= length);
	memset(payload, 0, length);
	test_put_le16(payload + 4U, (uint16_t)(('U' << 8) | 'S'));
	payload[12] = 1U;
	test_put_le32(payload + 16U, 1U);
	if (!malformed)
		test_put_le32(payload + 20U, 0x12345678U);
	return length;
}

static int
test_receive_epoch_begin(
	void *argument,
	uint32_t generation)
{
	struct test_fixture *fixture;
	size_t read_index;
	size_t write_index;

	fixture = argument;
	assert(generation != 0U);
	assert(generation == fixture->session.generation);
	fixture->epoch_begin_count++;
	fixture->receive_enabled = 0U;
	test_trace(fixture, 'u');
	if (fixture->fail_stage == TEST_FAIL_EPOCH)
		return -1;
	write_index = fixture->event_index;
	for (read_index = fixture->event_index;
	     read_index < fixture->event_count; read_index++) {
		if (fixture->event[read_index].queued_before_epoch) {
			fixture->queued_dropped++;
			continue;
		}
		if (write_index != read_index)
			fixture->event[write_index] = fixture->event[read_index];
		write_index++;
	}
	fixture->event_count = write_index;
	fixture->receive_generation = generation;
	fixture->receive_enabled = 1U;
	return 0;
}

static int
test_transport_bind(
	void *argument,
	struct intel_ax211_dma_resources *dma,
	struct intel_ax211_mmio *mmio,
	struct intel_ax211_transport *transport,
	uint32_t generation)
{
	struct test_fixture *fixture;

	fixture = argument;
	assert(dma == &fixture->session.dma);
	assert(mmio == &fixture->mmio);
	assert(transport == &fixture->transport);
	assert(fixture->receive_enabled);
	assert(generation == fixture->receive_generation);
	test_trace(fixture, 'T');
	if (fixture->fail_stage == TEST_FAIL_BIND)
		return -1;
	memset(transport, 0, sizeof(*transport));
	fixture->bound_generation = generation;
	return 0;
}

static int
test_receive_event(
	void *argument,
	uint64_t deadline,
	uint8_t *bytes,
	size_t capacity,
	struct intel_ax211_boot_received_event *event)
{
	struct test_fixture *fixture;
	const struct test_event *script;

	fixture = argument;
	assert(fixture->receive_enabled);
	assert(deadline >= fixture->clock);
	test_trace(fixture, 'E');
	if (fixture->event_index >= fixture->event_count)
		return INTEL_AX211_BOOT_RECEIVE_TIMEOUT;
	script = &fixture->event[fixture->event_index++];
	if (script->type == TEST_EVENT_TIMEOUT) {
		fixture->clock = deadline;
		return INTEL_AX211_BOOT_RECEIVE_TIMEOUT;
	}
	event->length = test_event_make(fixture, script, bytes, capacity,
	    &event->notification_version, &event->generation);
	fixture->clock++;
	return INTEL_AX211_BOOT_RECEIVE_OK;
}

static int
test_publish_pnvm(
	void *argument,
	struct intel_ax211_dma_resources *dma)
{
	struct test_fixture *fixture;

	fixture = argument;
	assert(dma == &fixture->session.dma);
	assert(dma->pnvm_prepared);
	test_trace(fixture, 'V');
	return 0;
}

static int
test_post_alive(
	void *argument,
	const struct intel_ax211_protocol_alive *alive)
{
	struct test_fixture *fixture;

	fixture = argument;
	assert(alive->status == INTEL_AX211_PROTOCOL_ALIVE_STATUS_OK);
	test_trace(fixture, 'O');
	return 0;
}

static int
test_interrupt_drain(
	void *argument)
{
	struct test_fixture *fixture;

	fixture = argument;
	test_trace(fixture, 'i');
	if (fixture->fail_stage == TEST_FAIL_DRAIN)
		return -1;
	fixture->receive_enabled = 0U;
	return 0;
}

static int
test_clock_us(
	void *argument,
	uint64_t *time_us)
{
	struct test_fixture *fixture;

	fixture = argument;
	*time_us = fixture->clock;
	return 0;
}

static int
test_nic_lock(
	void *argument)
{
	struct test_fixture *fixture;

	fixture = argument;
	assert(!fixture->nic_owned);
	test_trace(fixture, 'k');
	if (fixture->fail_stage == TEST_FAIL_NIC_LOCK)
		return -1;
	fixture->nic_owned = 1U;
	return 0;
}

static int
test_nic_unlock(
	void *argument)
{
	struct test_fixture *fixture;

	fixture = argument;
	assert(fixture->nic_owned);
	test_trace(fixture, 'l');
	if (fixture->fail_stage == TEST_FAIL_NIC_UNLOCK ||
	    fixture->unlock_fails)
		return -1;
	fixture->nic_owned = 0U;
	return 0;
}

static void
test_success_retains_then_stops(void)
{
	static const uint8_t group[] = {
		2U, 12U, 1U, 1U, 2U, 1U, 4U, 1U, 1U, 1U, 1U
	};
	static const uint8_t opcode[] = {
		0x03U, 0x00U, 0x98U, 0x9bU, 0x01U, 0xeeU,
		0x04U, 0x77U, 0xc8U, 0x0cU, 0xd2U
	};
	struct intel_ax211_runtime_mcc mcc;
	struct test_fixture fixture;
	size_t command_index;
	size_t lock;
	size_t unlock;
	size_t runtime_irq;
	size_t runtime_commands;
	size_t trace_index;
	size_t drain;
	size_t quiesce;
	size_t stop;
	size_t release;
	int result;

	test_fixture_init(&fixture);
	test_success_events(&fixture);
	result = intel_ax211_runtime_start_run(&fixture.session);
	assert(result == INTEL_AX211_RUNTIME_START_OK);
	assert(fixture.session.state == INTEL_AX211_RUNTIME_START_STATE_RUNNING);
	assert(fixture.session.generation == 8U);
	assert(fixture.session.dma_prepared);
	assert(!fixture.dma_released);
	assert(fixture.source_released);
	assert(fixture.runtime_interrupts);
	assert(!fixture.nic_owned);
	assert(fixture.receive_generation == 8U);
	assert(fixture.bound_generation == 8U);
	assert(fixture.session.commands.hardware_epoch == 8U);
	assert(fixture.command_count == sizeof(group) / sizeof(group[0]));
	for (command_index = 0U; command_index < fixture.command_count;
	     command_index++) {
		assert(fixture.command[command_index].group ==
		    group[command_index]);
		assert(fixture.command[command_index].opcode ==
		    opcode[command_index]);
		assert(fixture.command[command_index].version == 0U);
	}
	assert(intel_ax211_runtime_start_mcc(&fixture.session, &mcc) ==
	    INTEL_AX211_RUNTIME_START_OK);
	assert(mcc.channel_count == 1U);
	assert(mcc.channel[0] == 0x12345678U);

	lock = test_trace_find(&fixture, 'k', 0U);
	unlock = test_trace_find(&fixture, 'l', lock + 1U);
	runtime_irq = test_trace_find(&fixture, 'J', unlock + 1U);
	runtime_commands = 0U;
	for (trace_index = lock + 1U; trace_index < unlock; trace_index++) {
		if (fixture.trace[trace_index] == 'c')
			runtime_commands++;
	}
	assert(lock < unlock);
	assert(runtime_commands == 9U);
	assert(unlock < runtime_irq);
	result = intel_ax211_runtime_start_stop(&fixture.session);
	assert(result == INTEL_AX211_RUNTIME_START_OK);
	assert(fixture.session.state == INTEL_AX211_RUNTIME_START_STATE_IDLE);
	assert(fixture.dma_released);
	drain = test_trace_find(&fixture, 'i', runtime_irq + 1U);
	quiesce = test_trace_find(&fixture, 'q', drain + 1U);
	stop = test_trace_find(&fixture, 's', quiesce + 1U);
	release = test_trace_find(&fixture, 'f', stop + 1U);
	assert(drain < quiesce);
	assert(quiesce < stop);
	assert(stop < release);
}

static void
test_malformed_timeout_and_stale(void)
{
	struct test_fixture fixture;
	int result;

	test_fixture_init(&fixture);
	test_event_add(&fixture, TEST_EVENT_MALFORMED, 0U, 0U);
	result = intel_ax211_runtime_start_run(&fixture.session);
	assert(result == INTEL_AX211_RUNTIME_START_PROTOCOL);
	assert(fixture.dma_released);
	assert(fixture.session.state == INTEL_AX211_RUNTIME_START_STATE_IDLE);

	test_fixture_init(&fixture);
	test_event_add(&fixture, TEST_EVENT_TIMEOUT, 0U, 0U);
	result = intel_ax211_runtime_start_run(&fixture.session);
	assert(result == INTEL_AX211_RUNTIME_START_TIMEOUT);
	assert(fixture.dma_released);

	test_fixture_init(&fixture);
	test_event_add(&fixture, TEST_EVENT_ALIVE, 1U, 0U);
	test_success_events(&fixture);
	result = intel_ax211_runtime_start_run(&fixture.session);
	assert(result == INTEL_AX211_RUNTIME_START_OK);
	assert(intel_ax211_runtime_start_stop(&fixture.session) ==
	    INTEL_AX211_RUNTIME_START_OK);

	test_fixture_init(&fixture);
	test_event_add(&fixture, TEST_EVENT_ALIVE, 0U, 0U);
	test_event_add(&fixture, TEST_EVENT_ALIVE, 0U, 0U);
	result = intel_ax211_runtime_start_run(&fixture.session);
	assert(result == INTEL_AX211_RUNTIME_START_DUPLICATE);
	assert(fixture.dma_released);

	test_fixture_init(&fixture);
	test_event_add(&fixture, TEST_EVENT_ALIVE, 0U, 0U);
	test_event_add(&fixture, TEST_EVENT_PNVM, 0U, 1U);
	result = intel_ax211_runtime_start_run(&fixture.session);
	assert(result == INTEL_AX211_RUNTIME_START_PROTOCOL);
	assert(fixture.dma_released);
}

static void
test_runtime_failure_unwind(void)
{
	struct test_fixture fixture;
	size_t index;
	int result;

	/* POWER_TABLE consumes the firmware's exact generic status reply. */
	test_fixture_init(&fixture);
	test_success_events(&fixture);
	for (index = 0U; index < fixture.event_count; index++) {
		if (fixture.event[index].type == TEST_EVENT_ACK_POWER) {
			fixture.event[index].type = TEST_EVENT_POWER_STATUS_FAILED;
			break;
		}
	}
	assert(index < fixture.event_count);
	result = intel_ax211_runtime_start_run(&fixture.session);
	assert(result == INTEL_AX211_RUNTIME_START_COMMAND);
	assert(fixture.dma_released);
	assert(!fixture.nic_owned);

	test_fixture_init(&fixture);
	test_success_events(&fixture);
	for (index = 0U; index < fixture.event_count; index++) {
		if (fixture.event[index].type == TEST_EVENT_ACK_POWER) {
			fixture.event[index].type = TEST_EVENT_MALFORMED_POWER;
			break;
		}
	}
	assert(index < fixture.event_count);
	result = intel_ax211_runtime_start_run(&fixture.session);
	assert(result == INTEL_AX211_RUNTIME_START_PROTOCOL);
	assert(fixture.dma_released);
	assert(!fixture.nic_owned);

	/* Exact MCC framing rejects count=1 with only the 20-byte prefix. */
	test_fixture_init(&fixture);
	test_event_add(&fixture, TEST_EVENT_ALIVE, 0U, 0U);
	test_event_add(&fixture, TEST_EVENT_PNVM, 0U, 0U);
	test_event_add(&fixture, TEST_EVENT_ACK_EXTENDED, 0U, 0U);
	test_event_add(&fixture, TEST_EVENT_ACK_ACCESS, 0U, 0U);
	test_event_add(&fixture, TEST_EVENT_INIT, 0U, 0U);
	test_event_add(&fixture, TEST_EVENT_ACK_TX_ANT, 0U, 0U);
	test_event_add(&fixture, TEST_EVENT_ACK_BT, 0U, 0U);
	test_event_add(&fixture, TEST_EVENT_ACK_SOC, 0U, 0U);
	test_event_add(&fixture, TEST_EVENT_ACK_LTR, 0U, 0U);
	test_event_add(&fixture, TEST_EVENT_ACK_TEMP, 0U, 0U);
	test_event_add(&fixture, TEST_EVENT_ACK_POWER, 0U, 0U);
	test_event_add(&fixture, TEST_EVENT_MALFORMED_MCC, 0U, 0U);
	result = intel_ax211_runtime_start_run(&fixture.session);
	assert(result == INTEL_AX211_RUNTIME_START_PROTOCOL);
	assert(fixture.dma_released);
	assert(!fixture.nic_owned);

	/* A runtime command cannot extend its finite per-command deadline. */
	test_fixture_init(&fixture);
	test_event_add(&fixture, TEST_EVENT_ALIVE, 0U, 0U);
	test_event_add(&fixture, TEST_EVENT_PNVM, 0U, 0U);
	test_event_add(&fixture, TEST_EVENT_ACK_EXTENDED, 0U, 0U);
	test_event_add(&fixture, TEST_EVENT_ACK_ACCESS, 0U, 0U);
	test_event_add(&fixture, TEST_EVENT_INIT, 0U, 0U);
	test_event_add(&fixture, TEST_EVENT_TIMEOUT, 0U, 0U);
	result = intel_ax211_runtime_start_run(&fixture.session);
	assert(result == INTEL_AX211_RUNTIME_START_TIMEOUT);
	assert(fixture.dma_released);
	assert(!fixture.nic_owned);

	/* Runtime interrupt publication failure unwinds an otherwise live pass. */
	test_fixture_init(&fixture);
	test_success_events(&fixture);
	fixture.fail_stage = TEST_FAIL_RUNTIME_INTERRUPTS;
	result = intel_ax211_runtime_start_run(&fixture.session);
	assert(result == INTEL_AX211_RUNTIME_START_TRANSPORT);
	assert(fixture.dma_released);
	assert(fixture.session.state == INTEL_AX211_RUNTIME_START_STATE_IDLE);

	/* DQA is denied before DMA or hardware ownership is acquired. */
	test_fixture_init(&fixture);
	fixture.fail_stage = TEST_FAIL_DQA_PROFILE;
	result = intel_ax211_runtime_start_run(&fixture.session);
	assert(result == INTEL_AX211_RUNTIME_START_PROTOCOL);
	assert(fixture.source_released);
	assert(!fixture.dma_released);
	assert(memchr(fixture.trace, 'h', fixture.trace_length) == NULL);
}

static void
test_stop_retry(void)
{
	struct test_fixture fixture;
	int result;

	test_fixture_init(&fixture);
	test_success_events(&fixture);
	assert(intel_ax211_runtime_start_run(&fixture.session) ==
	    INTEL_AX211_RUNTIME_START_OK);
	fixture.fail_stage = TEST_FAIL_DRAIN;
	result = intel_ax211_runtime_start_stop(&fixture.session);
	assert(result == INTEL_AX211_RUNTIME_START_STOP_REQUIRED);
	assert(fixture.session.state ==
	    INTEL_AX211_RUNTIME_START_STATE_STOP_REQUIRED);
	assert(fixture.session.dma_prepared);
	assert(!fixture.dma_released);
	fixture.fail_stage = TEST_FAIL_NONE;
	result = intel_ax211_runtime_start_cleanup(&fixture.session);
	assert(result == INTEL_AX211_RUNTIME_START_OK);
	assert(fixture.dma_released);
	assert(fixture.session.state == INTEL_AX211_RUNTIME_START_STATE_IDLE);
}

static void
test_generation_isolation_and_wrap(void)
{
	static const uint8_t events[] = {
		TEST_EVENT_ALIVE, TEST_EVENT_PNVM,
		TEST_EVENT_ACK_EXTENDED, TEST_EVENT_ACK_ACCESS,
		TEST_EVENT_INIT, TEST_EVENT_ACK_TX_ANT, TEST_EVENT_ACK_BT,
		TEST_EVENT_ACK_SOC, TEST_EVENT_ACK_LTR, TEST_EVENT_ACK_TEMP,
		TEST_EVENT_ACK_POWER, TEST_EVENT_ACK_MCC,
		TEST_EVENT_ACK_SCAN, TEST_EVENT_ACK_BEACON
	};
	struct test_fixture fixture;
	uint32_t old_generation;
	size_t index;
	int result;

	test_fixture_init(&fixture);
	test_success_events(&fixture);
	assert(intel_ax211_runtime_start_run(&fixture.session) ==
	    INTEL_AX211_RUNTIME_START_OK);
	old_generation = fixture.session.generation;
	assert(intel_ax211_runtime_start_stop(&fixture.session) ==
	    INTEL_AX211_RUNTIME_START_OK);

	/* Queued old work is flushed; later old notifications and replies drop. */
	test_event_add_generation(&fixture, TEST_EVENT_ALIVE,
	    old_generation, 1U);
	for (index = 0U; index < sizeof(events) / sizeof(events[0]); index++) {
		test_event_add_generation(&fixture, events[index],
		    old_generation, 0U);
		test_event_add(&fixture, events[index], 0U, 0U);
	}
	result = intel_ax211_runtime_start_run(&fixture.session);
	assert(result == INTEL_AX211_RUNTIME_START_OK);
	assert(fixture.session.generation == old_generation + 1U);
	assert(fixture.epoch_begin_count == 2U);
	assert(fixture.queued_dropped == 1U);
	assert(fixture.receive_generation == fixture.session.generation);
	assert(fixture.bound_generation == fixture.session.generation);
	assert(fixture.session.commands.hardware_epoch ==
	    fixture.session.generation);
	assert(fixture.event_index == fixture.event_count);
	assert(intel_ax211_runtime_start_stop(&fixture.session) ==
	    INTEL_AX211_RUNTIME_START_OK);

	/* Wrap consumes epoch one only after flushing any pre-existing alias. */
	test_fixture_init(&fixture);
	fixture.session.generation = UINT32_MAX;
	test_event_add_generation(&fixture, TEST_EVENT_ALIVE, 1U, 1U);
	test_success_events(&fixture);
	result = intel_ax211_runtime_start_run(&fixture.session);
	assert(result == INTEL_AX211_RUNTIME_START_OK);
	assert(fixture.session.generation == 1U);
	assert(fixture.receive_generation == 1U);
	assert(fixture.bound_generation == 1U);
	assert(fixture.queued_dropped == 1U);
	assert(intel_ax211_runtime_start_stop(&fixture.session) ==
	    INTEL_AX211_RUNTIME_START_OK);
}

static void
test_epoch_and_bind_failures(void)
{
	struct test_fixture fixture;
	int result;

	/* A failed flush consumes its epoch and never touches the controller. */
	test_fixture_init(&fixture);
	fixture.fail_stage = TEST_FAIL_EPOCH;
	result = intel_ax211_runtime_start_run(&fixture.session);
	assert(result == INTEL_AX211_RUNTIME_START_IO);
	assert(fixture.session.generation == 8U);
	assert(fixture.dma_released);
	assert(!fixture.receive_enabled);
	assert(memchr(fixture.trace, 'h', fixture.trace_length) == NULL);
	fixture.fail_stage = TEST_FAIL_NONE;
	test_success_events(&fixture);
	result = intel_ax211_runtime_start_run(&fixture.session);
	assert(result == INTEL_AX211_RUNTIME_START_OK);
	assert(fixture.session.generation == 9U);
	assert(fixture.receive_generation == 9U);
	assert(intel_ax211_runtime_start_stop(&fixture.session) ==
	    INTEL_AX211_RUNTIME_START_OK);

	/* Bind failure frees unexposed DMA but a failed reset stays sticky. */
	test_fixture_init(&fixture);
	fixture.fail_stage = TEST_FAIL_BIND;
	fixture.stop_fails = 1U;
	result = intel_ax211_runtime_start_run(&fixture.session);
	assert(result == INTEL_AX211_RUNTIME_START_STOP_REQUIRED);
	assert(fixture.session.state ==
	    INTEL_AX211_RUNTIME_START_STATE_STOP_REQUIRED_NO_DMA);
	assert(!fixture.session.dma_prepared);
	assert(fixture.session.hardware_touched);
	assert(fixture.dma_released);
	assert(intel_ax211_runtime_start_run(&fixture.session) ==
	    INTEL_AX211_RUNTIME_START_INVALID);
	fixture.stop_fails = 0U;
	fixture.fail_stage = TEST_FAIL_NONE;
	result = intel_ax211_runtime_start_cleanup(&fixture.session);
	assert(result == INTEL_AX211_RUNTIME_START_OK);
	assert(fixture.session.state == INTEL_AX211_RUNTIME_START_STATE_IDLE);
	assert(!fixture.session.hardware_touched);
}

static void
test_nic_ownership_failures(void)
{
	struct test_fixture fixture;
	size_t lock;
	size_t unlock;
	size_t stop;
	int result;

	/* Lock failure submits no runtime command and still resets the device. */
	test_fixture_init(&fixture);
	test_success_events(&fixture);
	fixture.fail_stage = TEST_FAIL_NIC_LOCK;
	result = intel_ax211_runtime_start_run(&fixture.session);
	assert(result == INTEL_AX211_RUNTIME_START_IO);
	assert(fixture.command_count == 2U);
	assert(!fixture.nic_owned);
	assert(fixture.dma_released);
	lock = test_trace_find(&fixture, 'k', 0U);
	stop = test_trace_find(&fixture, 's', lock + 1U);
	assert(lock < stop);

	/* Unlock failure cannot publish runtime IRQs and is resolved by reset. */
	test_fixture_init(&fixture);
	test_success_events(&fixture);
	fixture.fail_stage = TEST_FAIL_NIC_UNLOCK;
	result = intel_ax211_runtime_start_run(&fixture.session);
	assert(result == INTEL_AX211_RUNTIME_START_IO);
	assert(!fixture.runtime_interrupts);
	assert(!fixture.nic_owned);
	assert(fixture.dma_released);
	lock = test_trace_find(&fixture, 'k', 0U);
	unlock = test_trace_find(&fixture, 'l', lock + 1U);
	stop = test_trace_find(&fixture, 's', unlock + 1U);
	assert(lock < unlock);
	assert(unlock < stop);
}

int
main(void)
{
	test_success_retains_then_stops();
	test_malformed_timeout_and_stale();
	test_runtime_failure_unwind();
	test_stop_retry();
	test_generation_isolation_and_wrap();
	test_epoch_and_bind_failures();
	test_nic_ownership_failures();
	puts("intel ax211 operational runtime start: PASS");
	return 0;
}
