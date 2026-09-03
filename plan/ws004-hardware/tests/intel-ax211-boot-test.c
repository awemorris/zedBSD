/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * Intel AX211 bounded first-boot coordinator fixture
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../../src/drivers/intel-ax211-boot.h"

#define TEST_PNVM_CAPACITY                         256U
#define TEST_EVENT_COUNT                            32U
#define TEST_TRACE_CAPACITY                       8192U
#define TEST_COMMAND_QUEUE                           4U

enum test_event_type {
	TEST_EVENT_ALIVE = 1,
	TEST_EVENT_PNVM = 2,
	TEST_EVENT_INIT = 3,
	TEST_EVENT_ACK_EXTENDED = 4,
	TEST_EVENT_ACK_ACCESS = 5,
	TEST_EVENT_ACK_NVM = 6,
	TEST_EVENT_MALFORMED = 7,
	TEST_EVENT_TIMEOUT = 8,
	TEST_EVENT_UNKNOWN = 9
};

enum test_fail_stage {
	TEST_FAIL_NONE = 0,
	TEST_FAIL_DMA_BOOT = 1,
	TEST_FAIL_EPOCH_BEGIN = 2,
	TEST_FAIL_MMIO_RESET = 3,
	TEST_FAIL_BIND = 4,
	TEST_FAIL_RINGS = 5,
	TEST_FAIL_RX_PUBLISH = 6,
	TEST_FAIL_GEN3 = 7,
	TEST_FAIL_PNVM_PUBLISH = 8,
	TEST_FAIL_POST_ALIVE = 9,
	TEST_FAIL_DRAIN = 10,
	TEST_FAIL_STOP = 11
};

struct test_event {
	uint8_t type;
	uint8_t stale;
	uint8_t fixed_index;
	uint8_t index;
	uint8_t fixed_generation;
	uint8_t queued_before_epoch;
	uint32_t generation;
};

struct test_fixture {
	struct intel_ax211_boot boot;
	struct intel_ax211_mmio mmio;
	struct intel_ax211_transport transport;
	struct intel_ax211_boot_ops ops;
	struct drv_dma_device *dma_device;
	struct test_event event[TEST_EVENT_COUNT];
	size_t event_count;
	size_t event_index;
	uint64_t clock;
	uint8_t firmware[INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES];
	uint8_t pnvm[TEST_PNVM_CAPACITY];
	size_t pnvm_length;
	uint8_t context[INTEL_AX211_CONTEXT_INFO_GEN3_SIZE];
	uint8_t iml[INTEL_AX211_MMIO_IML_SIZE];
	uint8_t rx_byte;
	char trace[TEST_TRACE_CAPACITY];
	size_t trace_length;
	int fail_stage;
	uint8_t stop_fails;
	size_t rx_publish_count;
	size_t epoch_begin_count;
	size_t queued_dropped;
	uint32_t receive_generation;
	uint32_t bound_generation;
	uint8_t receive_enabled;
	uint8_t source_released;
	uint8_t dma_released;
	uint8_t boot_images_released;
};

static struct test_fixture *active_fixture;

static void test_put_le16(uint8_t *bytes, uint16_t value);
static void test_put_le32(uint8_t *bytes, uint32_t value);
static size_t test_append_tlv(uint8_t *bytes, size_t capacity,
	size_t offset, uint32_t type, const void *payload, uint32_t length);
static size_t test_append_u32(uint8_t *bytes, size_t capacity,
	size_t offset, uint32_t type, uint32_t value);
static size_t test_append_sku(uint8_t *bytes, size_t capacity,
	size_t offset, const struct intel_ax211_sku_id *sku);
static size_t test_append_hw(uint8_t *bytes, size_t capacity,
	size_t offset, uint16_t mac, uint16_t rf);
static void test_trace(struct test_fixture *fixture, char value);
static size_t test_trace_find(const struct test_fixture *fixture, char value, size_t start);
static void test_command_table_build(struct test_fixture *fixture);
static void test_pnvm_build(struct test_fixture *fixture);
static void test_fixture_init(struct test_fixture *fixture);
static void test_event_add(struct test_fixture *fixture, uint8_t type, uint8_t stale);
static void test_event_add_index(struct test_fixture *fixture, uint8_t type, uint8_t index);
static void test_event_add_generation(struct test_fixture *fixture,
	uint8_t type, uint32_t generation, uint8_t queued_before_epoch);
static void test_success_events(struct test_fixture *fixture);
static size_t test_event_make(struct test_fixture *fixture,
	const struct test_event *event, uint8_t *bytes, size_t capacity,
	uint8_t *version, uint32_t *generation);
static size_t test_packet_make(uint8_t *bytes, size_t capacity,
	uint8_t opcode, uint8_t group, uint8_t index, uint8_t queue,
	const uint8_t *payload, size_t payload_length);
static void test_alive_payload(uint8_t payload[INTEL_AX211_PROTOCOL_ALIVE_SIZE]);
static void test_nvm_payload(uint8_t payload[INTEL_AX211_PROTOCOL_NVM_GET_INFO_SIZE]);
static int test_receive_epoch_begin(void *argument, uint32_t generation);
static int test_transport_bind(void *argument,
	struct intel_ax211_dma_resources *dma, struct intel_ax211_mmio *mmio,
	struct intel_ax211_transport *transport, uint32_t generation);
static int test_receive_event(void *argument, uint64_t deadline_us,
	uint8_t *bytes, size_t capacity,
	struct intel_ax211_boot_received_event *event);
static int test_publish_pnvm(void *argument, struct intel_ax211_dma_resources *dma);
static int test_post_alive(void *argument, const struct intel_ax211_protocol_alive *alive);
static int test_interrupt_drain(void *argument);
static int test_clock_us(void *argument, uint64_t *time_us);
static void test_success(void);
static void test_malformed_and_timeout(void);
static void test_stale_and_duplicate(void);
static void test_partial_failure_unwind(void);
static void test_stop_retry_retains_dma(void);
static void test_command_duplicate(void);
static void test_command_timeout(void);
static void test_repeat_generation_isolation(void);
static void test_generation_wrap(void);
static void test_epoch_begin_failure(void);
static void test_bind_stop_failure_requires_cleanup(void);

/* Supplies one exact synthetic firmware/PNVM pair to the coordinator. */
int
intel_ax211_firmware_files_load(
	struct intel_ax211_firmware_files *files)
{
	struct test_fixture *fixture;

	fixture = active_fixture;
	assert(fixture != NULL);
	test_trace(fixture, 'L');
	if (fixture->source_released) {
		test_command_table_build(fixture);
		test_pnvm_build(fixture);
		fixture->source_released = 0U;
	}
	memset(files, 0, sizeof(*files));
	files->ucode_bytes = fixture->firmware;
	files->ucode_size = sizeof(fixture->firmware);
	files->ucode_manifest.command_versions_offset = 0U;
	files->ucode_manifest.command_versions_length =
	    sizeof(fixture->firmware);
	files->pnvm_bytes = fixture->pnvm;
	files->pnvm_size = fixture->pnvm_length;
	return 0;
}

/* Scribbles source storage to prove the command table is coordinator-owned. */
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

/* Creates stable fake DMA ownership for all coordinator checks. */
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
	assert(fixture != NULL);
	assert(device == fixture->dma_device);
	assert(firmware_bytes == fixture->firmware);
	assert(firmware_length == sizeof(fixture->firmware));
	assert(manifest->command_versions_length == sizeof(fixture->firmware));
	assert(hardware_revision == 0x0370U);
	test_trace(fixture, 'B');
	if (fixture->fail_stage == TEST_FAIL_DMA_BOOT)
		return -1;
	fixture->dma_released = 0U;
	fixture->boot_images_released = 0U;
	memset(resources, 0, sizeof(*resources));
	resources->device = device;
	resources->context.address = fixture->context;
	resources->context.device_address = 0x100000U;
	resources->context.size = sizeof(fixture->context);
	resources->iml.address = fixture->iml;
	resources->iml.device_address = 0x200000U;
	resources->iml.size = sizeof(fixture->iml);
	resources->rx_buffer_count = INTEL_AX211_RX_RING_SIZE;

	/* Publishes exact nonzero identities for every RX DMA object. */
	for (index = 0U; index < resources->rx_buffer_count; index++) {
		resources->rx_buffer[index].address = &fixture->rx_byte;
		resources->rx_buffer[index].device_address =
		    0x300000U + (uint64_t)index * INTEL_AX211_RX_BUFFER_SIZE;
		resources->rx_buffer[index].size = INTEL_AX211_RX_BUFFER_SIZE;
	}
	resources->boot_prepared = 1U;
	return 0;
}

/* Records the accepted-ALIVE ownership transition. */
void
intel_ax211_dma_release_boot_images(
	struct intel_ax211_dma_resources *resources)
{
	struct test_fixture *fixture;

	fixture = active_fixture;
	assert(fixture != NULL);
	assert(resources->boot_prepared);
	assert(!resources->boot_images_released);
	assert(fixture->boot.alive_accepted);
	test_trace(fixture, 'b');
	resources->boot_images_released = 1U;
	fixture->boot_images_released = 1U;
}

/* Accepts only a copied PNVM after accepted-ALIVE image retirement. */
int
intel_ax211_dma_prepare_pnvm(
	const uint8_t *pnvm_bytes,
	size_t pnvm_length,
	const struct intel_ax211_pnvm_manifest *manifest,
	struct intel_ax211_dma_resources *resources)
{
	struct test_fixture *fixture;

	fixture = active_fixture;
	assert(fixture != NULL);
	assert(pnvm_bytes == fixture->pnvm);
	assert(pnvm_length == fixture->pnvm_length);
	assert(manifest->section_count == 1U);
	assert(manifest->total_length == 3U);
	assert(resources->boot_images_released);
	test_trace(fixture, 'N');
	resources->pnvm_prepared = 1U;
	return 0;
}

/* Releases fake DMA only after the test stop boundary. */
void
intel_ax211_dma_release(
	struct intel_ax211_dma_resources *resources)
{
	struct test_fixture *fixture;

	fixture = active_fixture;
	assert(fixture != NULL);
	assert(!fixture->dma_released);
	test_trace(fixture, 'f');
	memset(resources, 0, sizeof(*resources));
	fixture->dma_released = 1U;
}

/* Records the first card-ready transition. */
int
intel_ax211_mmio_prepare_card_hw(
	struct intel_ax211_mmio *mmio)
{
	assert(mmio == &active_fixture->mmio);
	test_trace(active_fixture, 'h');
	return INTEL_AX211_MMIO_OK;
}

/* Records or rejects the software-reset transition. */
int
intel_ax211_mmio_sw_reset(
	struct intel_ax211_mmio *mmio)
{
	assert(mmio == &active_fixture->mmio);
	test_trace(active_fixture, 'r');
	if (active_fixture->fail_stage == TEST_FAIL_MMIO_RESET)
		return INTEL_AX211_MMIO_IO;
	return INTEL_AX211_MMIO_OK;
}

/* Records the APM transition. */
int
intel_ax211_mmio_apm_init(
	struct intel_ax211_mmio *mmio)
{
	assert(mmio == &active_fixture->mmio);
	test_trace(active_fixture, 'a');
	return INTEL_AX211_MMIO_OK;
}

/* Records or rejects Gen3 context publication. */
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
	if (active_fixture->fail_stage == TEST_FAIL_GEN3)
		return INTEL_AX211_MMIO_IO;
	return INTEL_AX211_MMIO_OK;
}

/* Records or rejects the bus-master stop boundary. */
int
intel_ax211_mmio_stop(
	struct intel_ax211_mmio *mmio)
{
	assert(mmio == &active_fixture->mmio);
	test_trace(active_fixture, 's');
	if (active_fixture->fail_stage == TEST_FAIL_STOP ||
	    active_fixture->stop_fails)
		return INTEL_AX211_MMIO_TIMEOUT;
	return INTEL_AX211_MMIO_OK;
}

/* Initializes the minimum command-ring state used by the real command layer. */
int
intel_ax211_transport_configure_msix(
	struct intel_ax211_transport *transport)
{
	test_trace(active_fixture, 'X');
	transport->msix_configured = 1U;
	return INTEL_AX211_TRANSPORT_OK;
}

/* Initializes or rejects the fake transport rings. */
int
intel_ax211_transport_initialize_rings(
	struct intel_ax211_transport *transport)
{
	int result;

	test_trace(active_fixture, 'R');
	if (active_fixture->fail_stage == TEST_FAIL_RINGS)
		return INTEL_AX211_TRANSPORT_IO;
	result = intel_ax211_ring_init(&transport->command_ring,
	    TEST_COMMAND_QUEUE, INTEL_AX211_COMMAND_RING_SIZE);
	assert(result == INTEL_AX211_OK);
	transport->rings_initialized = 1U;
	transport->command_reset_completed = 1U;
	return INTEL_AX211_TRANSPORT_OK;
}

/* Counts every exact RX descriptor publication. */
int
intel_ax211_transport_publish_rx_descriptor(
	struct intel_ax211_transport *transport,
	uint16_t index,
	uint64_t device_address)
{
	(void)transport;
	assert(index < INTEL_AX211_RX_RING_SIZE);
	assert(device_address != 0U);
	active_fixture->rx_publish_count++;
	if (active_fixture->rx_publish_count == 1U)
		test_trace(active_fixture, 'D');
	if (active_fixture->fail_stage == TEST_FAIL_RX_PUBLISH && index == 7U)
		return INTEL_AX211_TRANSPORT_IO;
	return INTEL_AX211_TRANSPORT_OK;
}

/* Records RX activation. */
int
intel_ax211_transport_activate_rx(
	struct intel_ax211_transport *transport)
{
	test_trace(active_fixture, 'A');
	transport->rx_active = 1U;
	return INTEL_AX211_TRANSPORT_OK;
}

/* Admits command submission after firmware interrupts are enabled. */
int
intel_ax211_transport_enable_firmware_interrupts(
	struct intel_ax211_transport *transport)
{
	test_trace(active_fixture, 'I');
	transport->interrupts_enabled = 1U;
	return INTEL_AX211_TRANSPORT_OK;
}

/* Reserves one in-order fake hardware command slot. */
int
intel_ax211_transport_command_prepare_inline(
	struct intel_ax211_transport *transport,
	const struct intel_ax211_command_id *command,
	const void *payload,
	size_t payload_length,
	struct intel_ax211_ring_token *token)
{
	int result;

	(void)payload;
	assert(payload_length <= INTEL_AX211_COMMAND_INLINE_PAYLOAD_SIZE);
	assert(!transport->command_prepared);
	assert(command->version == 0U);
	result = intel_ax211_ring_reserve(&transport->command_ring, token);
	if (result == INTEL_AX211_FULL)
		return INTEL_AX211_TRANSPORT_FULL;
	assert(result == INTEL_AX211_OK);
	transport->command_prepared_token = *token;
	transport->command_prepared = 1U;
	if (command->group == INTEL_AX211_INIT_SYSTEM_GROUP)
		test_trace(active_fixture, 'c');
	else if (command->opcode ==
	    INTEL_AX211_PROTOCOL_NVM_ACCESS_COMPLETE_OPCODE)
		test_trace(active_fixture, 'd');
	else
		test_trace(active_fixture, 'e');
	return INTEL_AX211_TRANSPORT_OK;
}

/* The bounded NVM pass never needs the shared external command buffer. */
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

/* Rings one prepared fake command. */
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

/* Rolls back only the newest unrung fake command. */
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

/* Retires one in-order fake command slot. */
int
intel_ax211_transport_command_complete(
	struct intel_ax211_transport *transport,
	const struct intel_ax211_ring_token *token)
{
	int result;

	result = intel_ax211_ring_complete(&transport->command_ring, token);
	if (result != INTEL_AX211_OK)
		return INTEL_AX211_TRANSPORT_STALE;
	return INTEL_AX211_TRANSPORT_OK;
}

/* Reports exact fake hardware command ownership. */
size_t
intel_ax211_transport_command_pending_count(
	const struct intel_ax211_transport *transport)
{
	return transport->command_ring.used;
}

/* Returns the oldest fake hardware token. */
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

/* Stops submissions while preserving whether reset cleanup is required. */
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

/* Clears fake command ownership only after the stop callback succeeded. */
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

static size_t
test_append_u32(
	uint8_t *bytes,
	size_t capacity,
	size_t offset,
	uint32_t type,
	uint32_t value)
{
	uint8_t payload[4];

	test_put_le32(payload, value);
	return test_append_tlv(bytes, capacity, offset, type, payload,
	    sizeof(payload));
}

static size_t
test_append_sku(
	uint8_t *bytes,
	size_t capacity,
	size_t offset,
	const struct intel_ax211_sku_id *sku)
{
	uint8_t payload[12];

	test_put_le32(payload, sku->data[0]);
	test_put_le32(payload + 4U, sku->data[1]);
	test_put_le32(payload + 8U, sku->data[2]);
	return test_append_tlv(bytes, capacity, offset, 64U, payload,
	    sizeof(payload));
}

static size_t
test_append_hw(
	uint8_t *bytes,
	size_t capacity,
	size_t offset,
	uint16_t mac,
	uint16_t rf)
{
	uint8_t payload[4];

	test_put_le16(payload, mac);
	test_put_le16(payload + 2U, rf);
	return test_append_tlv(bytes, capacity, offset, 58U, payload,
	    sizeof(payload));
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
		{ 0xfeU, 0x0cU, 99U, 1U }
	};
	size_t index;

	memset(fixture->firmware, 0, sizeof(fixture->firmware));

	/* Installs every API89 version required by the first coordinator pass. */
	for (index = 0U;
	     index < sizeof(required) / sizeof(required[0]);
	     index++) {
		memcpy(fixture->firmware + index * 4U, required[index], 4U);
	}
}

static void
test_pnvm_build(
	struct test_fixture *fixture)
{
	struct intel_ax211_sku_id sku;
	uint8_t section[7];
	size_t offset;

	memset(&sku, 0, sizeof(sku));
	sku.data[0] = 1U;
	sku.data[1] = 2U;
	sku.data[2] = 3U;
	memset(section, 0, sizeof(section));
	test_put_le32(section, 0x5000U);
	section[4] = 1U;
	section[5] = 2U;
	section[6] = 3U;
	memset(fixture->pnvm, 0, sizeof(fixture->pnvm));
	offset = 0U;
	offset = test_append_sku(fixture->pnvm, sizeof(fixture->pnvm),
	    offset, &sku);
	offset = test_append_u32(fixture->pnvm, sizeof(fixture->pnvm),
	    offset, 62U, 1U);
	offset = test_append_hw(fixture->pnvm, sizeof(fixture->pnvm),
	    offset, INTEL_AX211_MAC_TYPE_SO, INTEL_AX211_RF_TYPE);
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
	fixture->dma_device = (struct drv_dma_device *)(void *)&fixture->rx_byte;
	fixture->mmio.profile.mac_type = INTEL_AX211_MAC_TYPE_SO;
	fixture->mmio.profile.rf_type = INTEL_AX211_RF_TYPE;
	fixture->ops.receive_epoch_begin = test_receive_epoch_begin;
	fixture->ops.transport_bind = test_transport_bind;
	fixture->ops.receive_event = test_receive_event;
	fixture->ops.publish_pnvm = test_publish_pnvm;
	fixture->ops.post_alive = test_post_alive;
	fixture->ops.interrupt_drain = test_interrupt_drain;
	fixture->ops.clock_us = test_clock_us;
	result = intel_ax211_boot_init(&fixture->boot, &fixture->ops, fixture,
	    fixture->dma_device, &fixture->mmio, &fixture->transport, 0x0370U,
	    INTEL_AX211_RF_TYPE, 7U);
	assert(result == INTEL_AX211_BOOT_OK);
}

static void
test_event_add(
	struct test_fixture *fixture,
	uint8_t type,
	uint8_t stale)
{
	assert(fixture->event_count < TEST_EVENT_COUNT);
	fixture->event[fixture->event_count].type = type;
	fixture->event[fixture->event_count].stale = stale;
	fixture->event_count++;
}

static void
test_event_add_index(
	struct test_fixture *fixture,
	uint8_t type,
	uint8_t index)
{
	assert(fixture->event_count < TEST_EVENT_COUNT);
	fixture->event[fixture->event_count].type = type;
	fixture->event[fixture->event_count].fixed_index = 1U;
	fixture->event[fixture->event_count].index = index;
	fixture->event_count++;
}

/* Adds one explicitly stamped event, optionally queued before epoch start. */
static void
test_event_add_generation(
	struct test_fixture *fixture,
	uint8_t type,
	uint32_t generation,
	uint8_t queued_before_epoch)
{
	assert(fixture->event_count < TEST_EVENT_COUNT);
	assert(generation != 0U);
	fixture->event[fixture->event_count].type = type;
	fixture->event[fixture->event_count].fixed_generation = 1U;
	fixture->event[fixture->event_count].queued_before_epoch =
	    queued_before_epoch;
	fixture->event[fixture->event_count].generation = generation;
	fixture->event_count++;
}

static void
test_success_events(
	struct test_fixture *fixture)
{
	test_event_add(fixture, TEST_EVENT_ALIVE, 0U);
	test_event_add(fixture, TEST_EVENT_PNVM, 0U);
	test_event_add(fixture, TEST_EVENT_ACK_EXTENDED, 0U);
	test_event_add(fixture, TEST_EVENT_ACK_ACCESS, 0U);
	test_event_add(fixture, TEST_EVENT_INIT, 0U);
	test_event_add(fixture, TEST_EVENT_ACK_NVM, 0U);
}

static size_t
test_event_make(
	struct test_fixture *fixture,
	const struct test_event *event,
	uint8_t *bytes,
	size_t capacity,
	uint8_t *version,
	uint32_t *generation)
{
	uint8_t alive[INTEL_AX211_PROTOCOL_ALIVE_SIZE];
	uint8_t generic_status[4];
	uint8_t init[INTEL_AX211_PROTOCOL_INIT_COMPLETE_SIZE];
	uint8_t nvm[INTEL_AX211_PROTOCOL_NVM_GET_INFO_SIZE];
	uint8_t pnvm[INTEL_AX211_PROTOCOL_PNVM_INIT_COMPLETE_SIZE];
	uint8_t command_index;
	uint8_t group;
	uint8_t opcode;
	uint8_t queue;
	const uint8_t *payload;
	size_t payload_length;

	group = 0U;
	opcode = 0U;
	queue = 0x80U;
	payload = NULL;
	payload_length = 0U;
	*version = 0U;
	if (event->fixed_generation) {
		*generation = event->generation;
	} else if (event->stale) {
		*generation = fixture->boot.generation - 1U;
		if (*generation == 0U)
			*generation = (uint32_t)-1;
	} else {
		*generation = fixture->boot.generation;
	}
	command_index = event->fixed_index ? event->index :
	    (uint8_t)fixture->transport.command_ring.tail;

	/* Builds the exact packet selected by the deterministic event script. */
	if (event->type == TEST_EVENT_ALIVE) {
		opcode = INTEL_AX211_PROTOCOL_ALIVE_OPCODE;
		*version = INTEL_AX211_PROTOCOL_ALIVE_VERSION;
		test_alive_payload(alive);
		payload = alive;
		payload_length = sizeof(alive);
	} else if (event->type == TEST_EVENT_PNVM) {
		opcode = INTEL_AX211_PROTOCOL_PNVM_INIT_COMPLETE_OPCODE;
		group = INTEL_AX211_PROTOCOL_GROUP_REGULATORY_NVM;
		*version = INTEL_AX211_PROTOCOL_PNVM_INIT_COMPLETE_VERSION;
		memset(pnvm, 0, sizeof(pnvm));
		payload = pnvm;
		payload_length = sizeof(pnvm);
	} else if (event->type == TEST_EVENT_INIT) {
		opcode = INTEL_AX211_PROTOCOL_INIT_COMPLETE_OPCODE;
		*version = INTEL_AX211_PROTOCOL_UNKNOWN_VERSION;
		memset(init, 0, sizeof(init));
		payload = init;
		payload_length = sizeof(init);
	} else if (event->type == TEST_EVENT_ACK_EXTENDED) {
		opcode = INTEL_AX211_INIT_EXTENDED_CFG_OPCODE;
		group = INTEL_AX211_INIT_SYSTEM_GROUP;
		queue = TEST_COMMAND_QUEUE;
		memset(generic_status, 0, sizeof(generic_status));
		payload = generic_status;
		payload_length = sizeof(generic_status);
	} else if (event->type == TEST_EVENT_ACK_ACCESS) {
		opcode = INTEL_AX211_PROTOCOL_NVM_ACCESS_COMPLETE_OPCODE;
		group = INTEL_AX211_PROTOCOL_GROUP_REGULATORY_NVM;
		queue = TEST_COMMAND_QUEUE;
	} else if (event->type == TEST_EVENT_ACK_NVM) {
		opcode = INTEL_AX211_PROTOCOL_NVM_GET_INFO_OPCODE;
		group = INTEL_AX211_PROTOCOL_GROUP_REGULATORY_NVM;
		queue = TEST_COMMAND_QUEUE;
		test_nvm_payload(nvm);
		payload = nvm;
		payload_length = sizeof(nvm);
	} else if (event->type == TEST_EVENT_UNKNOWN) {
		opcode = 0x7fU;
		group = 1U;
		*version = 1U;
	} else {
		assert(event->type == TEST_EVENT_MALFORMED);
		assert(capacity >= 7U);
		memset(bytes, 0, 7U);
		return 7U;
	}
	return test_packet_make(bytes, capacity, opcode, group, command_index,
	    queue, payload, payload_length);
}

static size_t
test_packet_make(
	uint8_t *bytes,
	size_t capacity,
	uint8_t opcode,
	uint8_t group,
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
	bytes[5] = group;
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

static void
test_nvm_payload(
	uint8_t payload[INTEL_AX211_PROTOCOL_NVM_GET_INFO_SIZE])
{
	memset(payload, 0, INTEL_AX211_PROTOCOL_NVM_GET_INFO_SIZE);
	test_put_le16(payload + 4U, 0x1234U);
	payload[6] = 2U;
	payload[7] = 1U;
	test_put_le32(payload + 8U, INTEL_AX211_PROTOCOL_NVM_BAND_24_ENABLED |
	    INTEL_AX211_PROTOCOL_NVM_11N_ENABLED);
	test_put_le32(payload + 12U, 1U);
	test_put_le32(payload + 16U, 1U);
	test_put_le32(payload + 20U, 1U);
	test_put_le32(payload + 24U, 1U);
	test_put_le32(payload + 28U, INTEL_AX211_PROTOCOL_NVM_CHANNEL_VALID |
	    INTEL_AX211_PROTOCOL_NVM_CHANNEL_ACTIVE);
}

/* Drains pre-existing events and atomically installs one receive epoch. */
static int
test_receive_epoch_begin(
	void *argument,
	uint32_t generation)
{
	struct test_fixture *fixture;
	size_t read_index;
	size_t write_index;

	fixture = argument;
	assert(fixture != NULL);
	assert(generation != 0U);
	assert(generation == fixture->boot.generation);
	fixture->epoch_begin_count++;
	fixture->receive_enabled = 0U;
	test_trace(fixture, 'u');
	if (fixture->fail_stage == TEST_FAIL_EPOCH_BEGIN)
		return -1;

	/* Only events already queued are drained; later old events stay stamped. */
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
	assert(dma == &fixture->boot.dma);
	assert(mmio == &fixture->mmio);
	assert(transport == &fixture->transport);
	assert(fixture->receive_enabled);
	assert(generation == fixture->receive_generation);
	test_trace(fixture, 'T');
	if (fixture->fail_stage == TEST_FAIL_BIND)
		return -1;
	memset(transport, 0, sizeof(*transport));
	fixture->bound_generation = generation;
	fixture->rx_publish_count = 0U;
	return 0;
}

static int
test_receive_event(
	void *argument,
	uint64_t deadline_us,
	uint8_t *bytes,
	size_t capacity,
	struct intel_ax211_boot_received_event *event)
{
	struct test_fixture *fixture;
	const struct test_event *script;

	fixture = argument;
	assert(deadline_us >= fixture->clock);
	assert(fixture->receive_enabled);
	test_trace(fixture, 'E');
	if (fixture->event_index >= fixture->event_count)
		return INTEL_AX211_BOOT_RECEIVE_TIMEOUT;
	script = &fixture->event[fixture->event_index++];
	if (script->type == TEST_EVENT_TIMEOUT) {
		fixture->clock = deadline_us;
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
	assert(dma->pnvm_prepared);
	test_trace(fixture, 'V');
	if (fixture->fail_stage == TEST_FAIL_PNVM_PUBLISH)
		return -1;
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
	if (fixture->fail_stage == TEST_FAIL_POST_ALIVE)
		return -1;
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

static void
test_success(void)
{
	struct intel_ax211_protocol_command_table table;
	struct intel_ax211_protocol_nvm nvm;
	struct test_fixture fixture;
	size_t boot_release;
	size_t file_release;
	size_t pnvm_prepare;
	size_t drain;
	size_t stop;
	size_t dma_release;
	int result;

	test_fixture_init(&fixture);
	test_success_events(&fixture);
	memset(&nvm, 0, sizeof(nvm));
	result = intel_ax211_boot_run(&fixture.boot, &nvm);
	if (result != INTEL_AX211_BOOT_OK)
		fprintf(stderr, "boot result=%d last=%u events=%lu trace=%s\n",
		    result, fixture.boot.last_error,
		    (unsigned long)fixture.event_index, fixture.trace);
	assert(result == INTEL_AX211_BOOT_OK);
	assert(fixture.boot.state == INTEL_AX211_BOOT_STATE_COMPLETE);
	assert(fixture.boot.generation == 8U);
	assert(fixture.receive_generation == fixture.boot.generation);
	assert(fixture.bound_generation == fixture.boot.generation);
	assert(fixture.boot.commands.hardware_epoch == fixture.boot.generation);
	assert(fixture.rx_publish_count == INTEL_AX211_RX_RING_SIZE);
	assert(strchr(fixture.trace, 'A') == NULL);
	assert(fixture.source_released);
	assert(fixture.dma_released);
	assert(nvm.nvm_version == 0x1234U);
	assert(nvm.valid_24ghz_count == 1U);
	assert(intel_ax211_boot_command_table(&fixture.boot, &table) ==
	    INTEL_AX211_BOOT_OK);
	assert(table.bytes == fixture.boot.command_version_bytes);
	assert(table.bytes[0] == INTEL_AX211_PROTOCOL_ALIVE_OPCODE);
	assert(table.bytes[0] != fixture.firmware[0]);

	/* Proves critical ownership transitions occurred in safe order. */
	boot_release = test_trace_find(&fixture, 'b', 0U);
	pnvm_prepare = test_trace_find(&fixture, 'N', 0U);
	file_release = test_trace_find(&fixture, 'F', 0U);
	drain = test_trace_find(&fixture, 'i', 0U);
	stop = test_trace_find(&fixture, 's', drain + 1U);
	dma_release = test_trace_find(&fixture, 'f', stop + 1U);
	assert(boot_release < pnvm_prepare);
	assert(pnvm_prepare < file_release);
	assert(drain < stop);
	assert(stop < dma_release);
}

static void
test_malformed_and_timeout(void)
{
	struct intel_ax211_protocol_nvm nvm;
	struct test_fixture fixture;
	int result;

	test_fixture_init(&fixture);
	test_event_add(&fixture, TEST_EVENT_MALFORMED, 0U);
	memset(&nvm, 0x5a, sizeof(nvm));
	result = intel_ax211_boot_run(&fixture.boot, &nvm);
	assert(result == INTEL_AX211_BOOT_PROTOCOL);
	assert(fixture.dma_released);
	assert(fixture.boot.state == INTEL_AX211_BOOT_STATE_IDLE);

	test_fixture_init(&fixture);
	test_event_add(&fixture, TEST_EVENT_TIMEOUT, 0U);
	result = intel_ax211_boot_run(&fixture.boot, &nvm);
	assert(result == INTEL_AX211_BOOT_TIMEOUT);
	assert(fixture.dma_released);
}

static void
test_stale_and_duplicate(void)
{
	struct intel_ax211_protocol_nvm nvm;
	struct test_fixture fixture;
	int result;

	test_fixture_init(&fixture);
	test_event_add(&fixture, TEST_EVENT_ALIVE, 1U);
	test_success_events(&fixture);
	result = intel_ax211_boot_run(&fixture.boot, &nvm);
	assert(result == INTEL_AX211_BOOT_OK);
	assert(fixture.event_index == fixture.event_count);

	test_fixture_init(&fixture);
	test_event_add(&fixture, TEST_EVENT_ALIVE, 0U);
	test_event_add(&fixture, TEST_EVENT_ALIVE, 0U);
	result = intel_ax211_boot_run(&fixture.boot, &nvm);
	assert(result == INTEL_AX211_BOOT_DUPLICATE);
	assert(fixture.dma_released);
}

static void
test_partial_failure_unwind(void)
{
	static const int failures[] = {
		TEST_FAIL_DMA_BOOT,
		TEST_FAIL_EPOCH_BEGIN,
		TEST_FAIL_MMIO_RESET,
		TEST_FAIL_BIND,
		TEST_FAIL_RINGS,
		TEST_FAIL_RX_PUBLISH,
		TEST_FAIL_GEN3,
		TEST_FAIL_PNVM_PUBLISH,
		TEST_FAIL_POST_ALIVE
	};
	struct intel_ax211_protocol_nvm nvm;
	struct test_fixture fixture;
	size_t failure_index;
	size_t drain;
	size_t stop;
	size_t release;
	int result;

	/* Exercises reverse unwind at every partial-ownership boundary. */
	for (failure_index = 0U;
	     failure_index < sizeof(failures) / sizeof(failures[0]);
	     failure_index++) {
		test_fixture_init(&fixture);
		fixture.fail_stage = failures[failure_index];
		test_success_events(&fixture);
		result = intel_ax211_boot_run(&fixture.boot, &nvm);
		assert(result != INTEL_AX211_BOOT_OK);
		assert(result != INTEL_AX211_BOOT_STOP_REQUIRED);
		assert(fixture.boot.generation == 8U);
		if (failures[failure_index] == TEST_FAIL_DMA_BOOT) {
			assert(!fixture.dma_released);
			continue;
		}
		assert(fixture.dma_released);
		if (failures[failure_index] > TEST_FAIL_BIND) {
			drain = test_trace_find(&fixture, 'i', 0U);
			stop = test_trace_find(&fixture, 's', drain + 1U);
			release = test_trace_find(&fixture, 'f', stop + 1U);
			assert(drain < stop);
			assert(stop < release);
		}
	}
}

static void
test_stop_retry_retains_dma(void)
{
	struct intel_ax211_protocol_nvm nvm;
	struct test_fixture fixture;
	int result;

	test_fixture_init(&fixture);
	test_success_events(&fixture);
	fixture.fail_stage = TEST_FAIL_DRAIN;
	result = intel_ax211_boot_run(&fixture.boot, &nvm);
	assert(result == INTEL_AX211_BOOT_STOP_REQUIRED);
	assert(fixture.boot.state == INTEL_AX211_BOOT_STATE_STOP_REQUIRED);
	assert(fixture.boot.dma_prepared);
	assert(!fixture.dma_released);
	fixture.fail_stage = TEST_FAIL_NONE;
	result = intel_ax211_boot_cleanup(&fixture.boot);
	assert(result == INTEL_AX211_BOOT_OK);
	assert(fixture.dma_released);
	assert(fixture.boot.state == INTEL_AX211_BOOT_STATE_IDLE);

	/* Applies the same retention rule when checked stop/reset itself fails. */
	test_fixture_init(&fixture);
	test_success_events(&fixture);
	fixture.fail_stage = TEST_FAIL_STOP;
	result = intel_ax211_boot_run(&fixture.boot, &nvm);
	assert(result == INTEL_AX211_BOOT_STOP_REQUIRED);
	assert(fixture.boot.dma_prepared);
	assert(!fixture.dma_released);
	fixture.fail_stage = TEST_FAIL_NONE;
	result = intel_ax211_boot_cleanup(&fixture.boot);
	assert(result == INTEL_AX211_BOOT_OK);
	assert(fixture.dma_released);
}

static void
test_command_duplicate(void)
{
	struct intel_ax211_protocol_nvm nvm;
	struct test_fixture fixture;
	int result;

	test_fixture_init(&fixture);
	test_event_add(&fixture, TEST_EVENT_ALIVE, 0U);
	test_event_add(&fixture, TEST_EVENT_PNVM, 0U);
	test_event_add(&fixture, TEST_EVENT_ACK_EXTENDED, 0U);
	test_event_add_index(&fixture, TEST_EVENT_ACK_EXTENDED, 0U);
	result = intel_ax211_boot_run(&fixture.boot, &nvm);
	assert(result == INTEL_AX211_BOOT_DUPLICATE);
	assert(fixture.dma_released);
}

static void
test_command_timeout(void)
{
	struct intel_ax211_protocol_nvm nvm;
	struct test_fixture fixture;
	int result;

	test_fixture_init(&fixture);
	test_event_add(&fixture, TEST_EVENT_ALIVE, 0U);
	test_event_add(&fixture, TEST_EVENT_PNVM, 0U);
	test_event_add(&fixture, TEST_EVENT_TIMEOUT, 0U);
	result = intel_ax211_boot_run(&fixture.boot, &nvm);
	assert(result == INTEL_AX211_BOOT_TIMEOUT);
	assert(fixture.dma_released);
	assert(fixture.transport.command_ring.used == 0U);
	assert(test_trace_find(&fixture, 's', 0U) <
	    test_trace_find(&fixture, 'f', 0U));
}

/* Rejects every delayed event from the completed run during the next run. */
static void
test_repeat_generation_isolation(void)
{
	struct intel_ax211_protocol_nvm nvm;
	struct test_fixture fixture;
	uint32_t old_generation;
	int result;

	test_fixture_init(&fixture);
	test_success_events(&fixture);
	result = intel_ax211_boot_run(&fixture.boot, &nvm);
	assert(result == INTEL_AX211_BOOT_OK);
	old_generation = fixture.boot.generation;

	/* This old ALIVE was already queued and must disappear at epoch begin. */
	test_event_add_generation(&fixture, TEST_EVENT_ALIVE,
	    old_generation, 1U);
	/* These old events arrive later and must be discarded by their stamp. */
	test_event_add_generation(&fixture, TEST_EVENT_ALIVE,
	    old_generation, 0U);
	test_event_add(&fixture, TEST_EVENT_ALIVE, 0U);
	test_event_add_generation(&fixture, TEST_EVENT_PNVM,
	    old_generation, 0U);
	test_event_add(&fixture, TEST_EVENT_PNVM, 0U);
	test_event_add_generation(&fixture, TEST_EVENT_ACK_EXTENDED,
	    old_generation, 0U);
	test_event_add(&fixture, TEST_EVENT_ACK_EXTENDED, 0U);
	test_event_add_generation(&fixture, TEST_EVENT_ACK_ACCESS,
	    old_generation, 0U);
	test_event_add(&fixture, TEST_EVENT_ACK_ACCESS, 0U);
	test_event_add_generation(&fixture, TEST_EVENT_INIT,
	    old_generation, 0U);
	test_event_add(&fixture, TEST_EVENT_INIT, 0U);
	test_event_add_generation(&fixture, TEST_EVENT_ACK_NVM,
	    old_generation, 0U);
	test_event_add(&fixture, TEST_EVENT_ACK_NVM, 0U);

	result = intel_ax211_boot_run(&fixture.boot, &nvm);
	assert(result == INTEL_AX211_BOOT_OK);
	assert(fixture.boot.generation == old_generation + 1U);
	assert(fixture.epoch_begin_count == 2U);
	assert(fixture.queued_dropped == 1U);
	assert(fixture.receive_generation == fixture.boot.generation);
	assert(fixture.bound_generation == fixture.boot.generation);
	assert(fixture.boot.commands.hardware_epoch == fixture.boot.generation);
	assert(fixture.event_index == fixture.event_count);
}

/* Skips the invalid zero epoch when the run counter wraps. */
static void
test_generation_wrap(void)
{
	struct intel_ax211_protocol_nvm nvm;
	struct test_fixture fixture;
	int result;

	test_fixture_init(&fixture);
	fixture.boot.generation = (uint32_t)-1;
	/* A queued ancient epoch-1 packet must not alias the wrapped epoch. */
	test_event_add_generation(&fixture, TEST_EVENT_ALIVE, 1U, 1U);
	test_success_events(&fixture);
	result = intel_ax211_boot_run(&fixture.boot, &nvm);
	assert(result == INTEL_AX211_BOOT_OK);
	assert(fixture.boot.generation == 1U);
	assert(fixture.receive_generation == 1U);
	assert(fixture.bound_generation == 1U);
	assert(fixture.boot.commands.hardware_epoch == 1U);
	assert(fixture.queued_dropped == 1U);
}

/* Fails closed and releases never-exposed DMA if epoch flush cannot bind. */
static void
test_epoch_begin_failure(void)
{
	struct intel_ax211_protocol_nvm nvm;
	struct test_fixture fixture;
	int result;

	test_fixture_init(&fixture);
	fixture.fail_stage = TEST_FAIL_EPOCH_BEGIN;
	result = intel_ax211_boot_run(&fixture.boot, &nvm);
	assert(result == INTEL_AX211_BOOT_IO);
	assert(fixture.boot.generation == 8U);
	assert(fixture.epoch_begin_count == 1U);
	assert(!fixture.receive_enabled);
	assert(fixture.dma_released);
	assert(fixture.boot.state == INTEL_AX211_BOOT_STATE_IDLE);
	assert(memchr(fixture.trace, 'h', fixture.trace_length) == NULL);

	/* The failed epoch is consumed; retry binds a new epoch instead. */
	fixture.fail_stage = TEST_FAIL_NONE;
	test_success_events(&fixture);
	result = intel_ax211_boot_run(&fixture.boot, &nvm);
	assert(result == INTEL_AX211_BOOT_OK);
	assert(fixture.boot.generation == 9U);
	assert(fixture.receive_generation == 9U);
	assert(fixture.bound_generation == 9U);
}

/* Keeps a dirty pre-bind controller blocked after releasing safe DMA. */
static void
test_bind_stop_failure_requires_cleanup(void)
{
	struct intel_ax211_protocol_nvm nvm;
	struct test_fixture fixture;
	int result;

	test_fixture_init(&fixture);
	fixture.fail_stage = TEST_FAIL_BIND;
	fixture.stop_fails = 1U;
	result = intel_ax211_boot_run(&fixture.boot, &nvm);
	assert(result == INTEL_AX211_BOOT_STOP_REQUIRED);
	assert(fixture.boot.state ==
	    INTEL_AX211_BOOT_STATE_STOP_REQUIRED_NO_DMA);
	assert(!fixture.boot.dma_prepared);
	assert(fixture.boot.hardware_touched);
	assert(fixture.dma_released);
	assert(intel_ax211_boot_run(&fixture.boot, &nvm) ==
	    INTEL_AX211_BOOT_INVALID);

	fixture.stop_fails = 0U;
	fixture.fail_stage = TEST_FAIL_NONE;
	result = intel_ax211_boot_cleanup(&fixture.boot);
	assert(result == INTEL_AX211_BOOT_OK);
	assert(fixture.boot.state == INTEL_AX211_BOOT_STATE_IDLE);
	assert(!fixture.boot.hardware_touched);
}

int
main(void)
{
	test_success();
	test_malformed_and_timeout();
	test_stale_and_duplicate();
	test_partial_failure_unwind();
	test_stop_retry_retains_dma();
	test_command_duplicate();
	test_command_timeout();
	test_repeat_generation_isolation();
	test_generation_wrap();
	test_epoch_begin_failure();
	test_bind_stop_failure_requires_cleanup();
	puts("intel ax211 bounded first boot: PASS");
	return 0;
}
