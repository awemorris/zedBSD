/* Intel AX211 private Gen3 TX ring fixture.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <drivers/dma.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../src/drivers/intel-ax211-tx-ring.h"

#define TEST_CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "intel ax211 tx ring: check failed at %s:%d\n", \
		    __FILE__, __LINE__); \
		return __LINE__; \
	} \
} while (0)

#define FIXTURE_ALLOCATION_COUNT  514U
#define FIXTURE_RECORD_COUNT      520U
#define FIXTURE_HBUS_TARG_WRPTR  0x460U

struct drv_dma_device {
	unsigned marker;
};

struct fixture_dma_record {
	uint8_t *memory;
	size_t size;
	uint64_t device_address;
	unsigned sequence;
	unsigned active;
};

struct fixture_io {
	unsigned sync_count;
	unsigned write_count;
	unsigned fail_sync_call;
	unsigned fail_write;
	uint32_t last_offset;
	uint32_t last_value;
};

static struct drv_dma_device fixture_device = { 0x211U };
static struct fixture_dma_record fixture_record[FIXTURE_RECORD_COUNT];
static unsigned fixture_allocate_attempt;
static unsigned fixture_allocate_success;
static unsigned fixture_free_count;
static unsigned fixture_fail_allocate_attempt;
static unsigned fixture_reverse_failure;
static unsigned fixture_scrub_failure;
static uint64_t fixture_next_address;

static uint16_t get_le16(const uint8_t *bytes);
static uint32_t get_le32(const uint8_t *bytes);
static uint64_t get_le64(const uint8_t *bytes);
static void put_le16(uint8_t *bytes, uint16_t value);
static void put_le32(uint8_t *bytes, uint32_t value);
static uint64_t align_up(uint64_t value, size_t alignment);
static int bytes_are(const uint8_t *bytes, size_t length, uint8_t value);
static unsigned active_allocations(void);
static unsigned latest_active_sequence(void);
static int allocator_reset(void);
static int fixture_sync(void *argument,
	const struct drv_dma_buffer *buffer, size_t offset, size_t length);
static int fixture_write32(void *argument, uint32_t offset, uint32_t value);
static struct fixture_dma_record *record_from_buffer(
	const struct drv_dma_buffer *buffer);
static int fixture_ring_open(struct intel_ax211_tx_ring *ring,
	struct fixture_io *io, uint16_t write_pointer,
	uint32_t hardware_generation, uint64_t connection_generation);
static void fixture_request(struct intel_ax211_tx_request *request,
	uint8_t frame[64U], uint64_t connection_generation, uint64_t cookie);
static struct intel_ax211_protocol_message fixture_completion(
	uint8_t payload[48U], uint8_t index, uint16_t byte_count,
	uint16_t next_sequence, uint32_t hardware_generation, uint32_t status);
static int test_api_and_queue_config(void);
static int test_submit_and_completion(void);
static int test_wrap_and_ring_full(void);
static int test_order_stale_duplicate_and_failure(void);
static int test_timeout_reset_and_kick_failure(void);
static int test_allocation_rollback(void);

static const struct intel_ax211_tx_ring_ops fixture_ops = {
	fixture_sync,
	fixture_write32
};

int
drv_dma_device_is_coherent(
	const struct drv_dma_device *device)
{
	return device == &fixture_device;
}

int
drv_dma_alloc_coherent(
	struct drv_dma_device *device,
	size_t size,
	size_t alignment,
	struct drv_dma_buffer *buffer)
{
	struct fixture_dma_record *record;
	uint64_t address;
	unsigned index;

	if (device != &fixture_device || size == 0U || alignment == 0U ||
	    (alignment & (alignment - 1U)) != 0U || buffer == NULL)
		return EINVAL;
	fixture_allocate_attempt++;
	if (fixture_allocate_attempt == fixture_fail_allocate_attempt)
		return ENOMEM;
	record = NULL;
	for (index = 0U; index < FIXTURE_RECORD_COUNT; index++) {
		if (!fixture_record[index].active) {
			record = &fixture_record[index];
			break;
		}
	}
	if (record == NULL)
		return ENOMEM;
	record->memory = malloc(size);
	if (record->memory == NULL)
		return ENOMEM;
	address = align_up(fixture_next_address, alignment);
	fixture_next_address = address + size + 0x100U;
	memset(record->memory, 0xa5, size);
	record->size = size;
	record->device_address = address;
	record->sequence = ++fixture_allocate_success;
	record->active = 1U;
	memset(buffer, 0, sizeof(*buffer));
	buffer->address = record->memory;
	buffer->device_address = address;
	buffer->size = size;
	buffer->private_data[0] = (uintptr_t)(index + 1U);
	return 0;
}

void
drv_dma_free_coherent(
	struct drv_dma_device *device,
	struct drv_dma_buffer *buffer)
{
	struct fixture_dma_record *record;

	if (device != &fixture_device)
		fixture_reverse_failure++;
	record = record_from_buffer(buffer);
	if (record == NULL || !record->active) {
		fixture_reverse_failure++;
		return;
	}
	if (record->sequence != latest_active_sequence())
		fixture_reverse_failure++;
	if (!bytes_are(record->memory, record->size, 0U))
		fixture_scrub_failure++;
	free(record->memory);
	memset(record, 0, sizeof(*record));
	fixture_free_count++;
	memset(buffer, 0, sizeof(*buffer));
}

static int
fixture_sync(
	void *argument,
	const struct drv_dma_buffer *buffer,
	size_t offset,
	size_t length)
{
	struct fixture_io *io;
	struct fixture_dma_record *record;

	io = argument;
	record = record_from_buffer(buffer);
	if (io == NULL || record == NULL || !record->active || length == 0U ||
	    offset > record->size || length > record->size - offset)
		return -1;
	io->sync_count++;
	if (io->sync_count == io->fail_sync_call)
		return -1;
	return 0;
}

static int
fixture_write32(
	void *argument,
	uint32_t offset,
	uint32_t value)
{
	struct fixture_io *io;

	io = argument;
	if (io == NULL)
		return -1;
	io->write_count++;
	io->last_offset = offset;
	io->last_value = value;
	return io->fail_write ? -1 : 0;
}

static int
test_api_and_queue_config(void)
{
	uint8_t versions[8U];
	uint8_t response[8U];
	struct intel_ax211_protocol_command_table table;
	struct intel_ax211_protocol_message message;
	struct intel_ax211_protocol_pending_command pending;
	struct intel_ax211_tx_queue_config config;
	struct intel_ax211_tx_ring ring;
	struct fixture_io io;
	int result;

	memset(versions, 0, sizeof(versions));
	versions[0U] = INTEL_AX211_TX_QUEUE_CONFIG_OPCODE;
	versions[1U] = INTEL_AX211_TX_QUEUE_CONFIG_GROUP;
	versions[2U] = INTEL_AX211_TX_QUEUE_CONFIG_COMMAND_VERSION;
	versions[3U] = INTEL_AX211_TX_QUEUE_CONFIG_RESPONSE_VERSION;
	versions[4U] = INTEL_AX211_TX_OPCODE;
	versions[5U] = INTEL_AX211_TX_GROUP;
	versions[6U] = INTEL_AX211_TX_COMMAND_VERSION;
	versions[7U] = INTEL_AX211_TX_NOTIFICATION_VERSION;
	TEST_CHECK(intel_ax211_protocol_command_table_parse(versions,
	    sizeof(versions), &table) == INTEL_AX211_PROTOCOL_OK);
	TEST_CHECK(intel_ax211_tx_ring_api89_validate(&table) ==
	    INTEL_AX211_TX_RING_OK);
	versions[2U]--;
	TEST_CHECK(intel_ax211_protocol_command_table_parse(versions,
	    sizeof(versions), &table) == INTEL_AX211_PROTOCOL_OK);
	TEST_CHECK(intel_ax211_tx_ring_api89_validate(&table) ==
	    INTEL_AX211_TX_RING_UNSUPPORTED);
	versions[2U] = INTEL_AX211_TX_QUEUE_CONFIG_COMMAND_VERSION;
	versions[6U]--;
	TEST_CHECK(intel_ax211_protocol_command_table_parse(versions,
	    sizeof(versions), &table) == INTEL_AX211_PROTOCOL_OK);
	TEST_CHECK(intel_ax211_tx_ring_api89_validate(&table) ==
	    INTEL_AX211_TX_RING_UNSUPPORTED);

	TEST_CHECK(allocator_reset() == 0);
	memset(&ring, 0, sizeof(ring));
	memset(&io, 0, sizeof(io));
	TEST_CHECK(intel_ax211_tx_ring_allocate(&fixture_device, &fixture_ops,
	    &io, &ring) == INTEL_AX211_TX_RING_OK);
	TEST_CHECK(fixture_allocate_success == FIXTURE_ALLOCATION_COUNT);
	TEST_CHECK(ring.tfd.size == INTEL_AX211_TX_RING_TFD_RING_SIZE);
	TEST_CHECK(ring.byte_count.size ==
	    INTEL_AX211_TX_RING_BYTE_COUNT_SIZE);
	TEST_CHECK(intel_ax211_tx_ring_queue_add_build(&ring, 3U, 0U, 0U,
	    &config) == INTEL_AX211_TX_RING_OK);
	TEST_CHECK(intel_ax211_tx_ring_queue_add_build(&ring, 3U, 8U, 0U,
	    &config) == INTEL_AX211_TX_RING_INVALID);
	TEST_CHECK(intel_ax211_tx_ring_queue_add_build(&ring, 3U,
	    INTEL_AX211_TX_RING_MANAGEMENT_TID, 0U, &config) ==
	    INTEL_AX211_TX_RING_OK);
	TEST_CHECK(config.command[8U] == INTEL_AX211_TX_RING_MANAGEMENT_TID);
	TEST_CHECK(intel_ax211_tx_ring_queue_add_build(&ring, 3U, 0U, 0U,
	    &config) == INTEL_AX211_TX_RING_OK);
	TEST_CHECK(get_le32(config.command) == 0U);
	TEST_CHECK(get_le32(config.command + 4U) == 8U);
	TEST_CHECK(config.command[8U] == 0U);
	TEST_CHECK(get_le32(config.command + 12U) == 0U);
	TEST_CHECK(get_le32(config.command + 16U) == 5U);
	TEST_CHECK(get_le64(config.command + 20U) ==
	    ring.byte_count.device_address);
	TEST_CHECK(get_le64(config.command + 28U) == ring.tfd.device_address);
	memset(response, 0, sizeof(response));
	put_le16(response, INTEL_AX211_TX_RING_QUEUE);
	memset(&pending, 0, sizeof(pending));
	pending.group = INTEL_AX211_TX_QUEUE_CONFIG_GROUP;
	pending.opcode = INTEL_AX211_TX_QUEUE_CONFIG_OPCODE;
	pending.response_version =
	    INTEL_AX211_TX_QUEUE_CONFIG_RESPONSE_VERSION;
	pending.queue = 0U;
	pending.index = 9U;
	pending.generation = 17U;
	pending.minimum_response_length = sizeof(response);
	pending.maximum_response_length = sizeof(response);
	memset(&message, 0, sizeof(message));
	message.group = pending.group;
	message.opcode = pending.opcode;
	message.version = pending.response_version;
	message.queue = pending.queue;
	message.index = pending.index;
	message.generation = pending.generation;
	message.payload = response;
	message.payload_length = sizeof(response);
	TEST_CHECK(intel_ax211_tx_ring_queue_add_complete(&ring, &config, 17U,
	    23U, &message, &pending) == INTEL_AX211_TX_RING_OK);
	TEST_CHECK(ring.enabled && ring.station_id == 3U && ring.tid == 0U);
	TEST_CHECK(ring.hardware_generation == 17U &&
	    ring.connection_generation == 23U);
	TEST_CHECK(intel_ax211_tx_ring_release(&ring, 0) ==
	    INTEL_AX211_TX_RING_BARRIER_REQUIRED);
	TEST_CHECK(intel_ax211_tx_ring_release(&ring, 1) ==
	    INTEL_AX211_TX_RING_OK);
	TEST_CHECK(active_allocations() == 0U);
	TEST_CHECK(fixture_free_count == FIXTURE_ALLOCATION_COUNT);
	TEST_CHECK(fixture_reverse_failure == 0U && fixture_scrub_failure == 0U);

	TEST_CHECK(allocator_reset() == 0);
	memset(&ring, 0, sizeof(ring));
	memset(&io, 0, sizeof(io));
	TEST_CHECK(intel_ax211_tx_ring_allocate(&fixture_device, &fixture_ops,
	    &io, &ring) == INTEL_AX211_TX_RING_OK);
	TEST_CHECK(intel_ax211_tx_ring_queue_add_build(&ring, 3U, 0U, 0U,
	    &config) == INTEL_AX211_TX_RING_OK);
	put_le16(response + 4U, 1U);
	result = intel_ax211_tx_ring_queue_add_complete(&ring, &config, 17U,
	    23U, &message, &pending);
	TEST_CHECK(result == INTEL_AX211_TX_RING_MALFORMED);
	put_le16(response + 4U, 0U);
	put_le16(response + 2U, 1U);
	result = intel_ax211_tx_ring_queue_add_complete(&ring, &config, 17U,
	    23U, &message, &pending);
	TEST_CHECK(result == INTEL_AX211_TX_RING_MALFORMED);
	put_le16(response + 2U, 0U);
	config.command[8U] = 1U;
	TEST_CHECK(intel_ax211_tx_ring_queue_add_complete(&ring, &config, 17U,
	    23U, &message, &pending) == INTEL_AX211_TX_RING_INVALID);
	TEST_CHECK(intel_ax211_tx_ring_release(&ring, 0) ==
	    INTEL_AX211_TX_RING_OK);
	return 0;
}

static int
test_submit_and_completion(void)
{
	uint8_t frame[64U];
	uint8_t payload[48U];
	uint8_t *command;
	uint8_t *tfd;
	struct intel_ax211_protocol_message message;
	struct intel_ax211_tx_request request;
	struct intel_ax211_tx_ring_handle handle;
	struct intel_ax211_tx_ring_retired retired;
	struct intel_ax211_tx_ring ring;
	struct fixture_io io;

	TEST_CHECK(allocator_reset() == 0);
	TEST_CHECK(fixture_ring_open(&ring, &io, 0U, 31U, 41U) == 0);
	fixture_request(&request, frame, 41U, 51U);
	TEST_CHECK(intel_ax211_tx_ring_submit(&ring, &request, 100U, 50U,
	    &handle) == INTEL_AX211_TX_RING_OK);
	TEST_CHECK(handle.hardware_generation == 31U &&
	    handle.connection_generation == 41U && handle.cookie == 51U);
	TEST_CHECK(handle.scheduler_sequence == 0U && handle.index == 0U &&
	    handle.deadline == 150U);
	TEST_CHECK(ring.pending_count == 1U && ring.write_sequence == 1U);
	TEST_CHECK(io.sync_count == 4U && io.write_count == 1U);
	TEST_CHECK(INTEL_AX211_TX_RING_WRITE_POINTER_REGISTER ==
	    FIXTURE_HBUS_TARG_WRPTR);
	TEST_CHECK(io.last_offset == FIXTURE_HBUS_TARG_WRPTR);
	TEST_CHECK(io.last_value == UINT32_C(0x00010001));
	command = ring.slot[0U].command.address;
	TEST_CHECK(command[0U] == INTEL_AX211_TX_OPCODE && command[1U] == 0U &&
	    command[2U] == 0U && command[3U] == INTEL_AX211_TX_RING_QUEUE);
	TEST_CHECK(get_le16(command + 4U) == request.length);
	tfd = ring.tfd.address;
	TEST_CHECK(get_le16(tfd) == 3U);
	TEST_CHECK(get_le16(tfd + 2U) == INTEL_AX211_TX_RING_FIRST_TB_SIZE);
	TEST_CHECK(get_le64(tfd + 4U) == ring.slot[0U].command.device_address);
	TEST_CHECK(get_le16(tfd + 12U) == 36U);
	TEST_CHECK(get_le64(tfd + 14U) ==
	    ring.slot[0U].command.device_address + 20U);
	TEST_CHECK(get_le16(tfd + 22U) == 16U);
	TEST_CHECK(get_le64(tfd + 24U) ==
	    ring.slot[0U].payload.device_address);
	TEST_CHECK(get_le16(ring.byte_count.address) == request.length);
	TEST_CHECK(memcmp(ring.slot[0U].payload.address, frame + 24U, 16U) == 0);

	message = fixture_completion(payload, 0U, (uint16_t)request.length, 1U,
	    31U, 1U);
	TEST_CHECK(intel_ax211_tx_ring_complete(&ring, &message, &retired) ==
	    INTEL_AX211_TX_RING_OK);
	TEST_CHECK(retired.handle.cookie == 51U && retired.acknowledged == 1U);
	TEST_CHECK(retired.byte_count == request.length &&
	    ring.pending_count == 0U && ring.read_sequence == 1U);
	TEST_CHECK(bytes_are(ring.slot[0U].command.address,
	    ring.slot[0U].command.size, 0U));
	TEST_CHECK(intel_ax211_tx_ring_complete(&ring, &message, &retired) ==
	    INTEL_AX211_TX_RING_DUPLICATE);

	memset(frame, 0, sizeof(frame));
	frame[0U] = 0x08U;
	frame[1U] = 0x40U;
	frame[24U] = 1U;
	frame[25U] = 2U;
	frame[27U] = 0xa0U;
	frame[28U] = 3U;
	frame[29U] = 4U;
	frame[30U] = 5U;
	frame[31U] = 6U;
	memset(frame + 32U, 0x6c, 16U);
	memset(&request, 0, sizeof(request));
	request.connection_generation = 41U;
	request.cookie = 52U;
	request.key_generation = 53U;
	request.packet_number = UINT64_C(0x060504030201);
	request.frame = frame;
	request.length = 48U;
	request.frame_class = INTEL_AX211_TX_FRAME_DATA;
	request.encrypted = 1U;
	request.key_index = 2U;
	TEST_CHECK(intel_ax211_tx_ring_submit(&ring, &request, 200U, 50U,
	    &handle) == INTEL_AX211_TX_RING_OK);
	TEST_CHECK(handle.cookie == 52U && handle.key_generation == 53U &&
	    handle.packet_number == request.packet_number);
	/* Firmware inserts the 8-byte CCMP IV; only plaintext follows the MAC. */
	TEST_CHECK(memcmp(ring.slot[1U].payload.address, frame + 32U, 16U) == 0);
	TEST_CHECK(memcmp(ring.slot[1U].payload.address, frame + 24U, 8U) != 0);
	TEST_CHECK(get_le16((uint8_t *)ring.byte_count.address + 2U) == 40U);
	message = fixture_completion(payload, 1U, 40U, 2U, 31U, 1U);
	TEST_CHECK(intel_ax211_tx_ring_complete(&ring, &message, &retired) ==
	    INTEL_AX211_TX_RING_OK);
	TEST_CHECK(retired.handle.key_generation == 53U &&
	    retired.handle.packet_number == request.packet_number);
	TEST_CHECK(intel_ax211_tx_ring_release(&ring, 1) ==
	    INTEL_AX211_TX_RING_OK);
	return 0;
}

static int
test_wrap_and_ring_full(void)
{
	uint8_t frame[64U];
	uint8_t payload[48U];
	struct intel_ax211_protocol_message message;
	struct intel_ax211_tx_request request;
	struct intel_ax211_tx_ring_handle handle;
	struct intel_ax211_tx_ring_retired retired;
	struct intel_ax211_tx_ring ring;
	struct fixture_io io;
	unsigned index;

	TEST_CHECK(allocator_reset() == 0);
	TEST_CHECK(fixture_ring_open(&ring, &io, UINT16_MAX, 3U, 5U) == 0);
	fixture_request(&request, frame, 5U, 1U);
	TEST_CHECK(intel_ax211_tx_ring_submit(&ring, &request, 0U, 10U,
	    &handle) == INTEL_AX211_TX_RING_OK);
	TEST_CHECK(handle.index == UINT8_MAX &&
	    handle.scheduler_sequence == UINT16_MAX);
	TEST_CHECK(io.last_value == UINT32_C(0x00010000));
	message = fixture_completion(payload, UINT8_MAX,
	    (uint16_t)request.length, 0U, 3U, 1U);
	TEST_CHECK(intel_ax211_tx_ring_complete(&ring, &message, &retired) ==
	    INTEL_AX211_TX_RING_OK);
	TEST_CHECK(ring.read_sequence == 0U);
	TEST_CHECK(intel_ax211_tx_ring_release(&ring, 1) ==
	    INTEL_AX211_TX_RING_OK);

	TEST_CHECK(allocator_reset() == 0);
	TEST_CHECK(fixture_ring_open(&ring, &io, 0U, 7U, 9U) == 0);
	fixture_request(&request, frame, 9U, 1U);
	for (index = 0U; index < INTEL_AX211_TX_RING_INFLIGHT_LIMIT; index++) {
		request.cookie = (uint64_t)index + 1U;
		TEST_CHECK(intel_ax211_tx_ring_submit(&ring, &request, 0U, 10U,
		    &handle) == INTEL_AX211_TX_RING_OK);
	}
	request.cookie++;
	TEST_CHECK(intel_ax211_tx_ring_submit(&ring, &request, 0U, 10U,
	    &handle) == INTEL_AX211_TX_RING_FULL);
	TEST_CHECK(ring.pending_count == INTEL_AX211_TX_RING_INFLIGHT_LIMIT);
	TEST_CHECK(intel_ax211_tx_ring_reset(&ring, 0) ==
	    INTEL_AX211_TX_RING_BARRIER_REQUIRED);
	TEST_CHECK(intel_ax211_tx_ring_reset(&ring, 1) ==
	    INTEL_AX211_TX_RING_OK);
	TEST_CHECK(intel_ax211_tx_ring_release(&ring, 0) ==
	    INTEL_AX211_TX_RING_OK);
	return 0;
}

static int
test_order_stale_duplicate_and_failure(void)
{
	uint8_t frame[64U];
	uint8_t payload0[48U];
	uint8_t payload1[48U];
	struct intel_ax211_protocol_message message0;
	struct intel_ax211_protocol_message message1;
	struct intel_ax211_tx_request request;
	struct intel_ax211_tx_ring_handle handle;
	struct intel_ax211_tx_ring_retired retired;
	struct intel_ax211_tx_ring ring;
	struct fixture_io io;

	TEST_CHECK(allocator_reset() == 0);
	TEST_CHECK(fixture_ring_open(&ring, &io, 0U, 61U, 71U) == 0);
	fixture_request(&request, frame, 71U, 81U);
	TEST_CHECK(intel_ax211_tx_ring_submit(&ring, &request, 0U, 10U,
	    &handle) == INTEL_AX211_TX_RING_OK);
	TEST_CHECK(intel_ax211_tx_ring_submit(&ring, &request, 0U, 10U,
	    &handle) == INTEL_AX211_TX_RING_DUPLICATE);
	request.cookie = 82U;
	TEST_CHECK(intel_ax211_tx_ring_submit(&ring, &request, 0U, 10U,
	    &handle) == INTEL_AX211_TX_RING_OK);
	message0 = fixture_completion(payload0, 0U,
	    (uint16_t)request.length, 1U, 61U, 1U);
	message1 = fixture_completion(payload1, 1U,
	    (uint16_t)request.length, 2U, 61U, 1U);
	TEST_CHECK(intel_ax211_tx_ring_complete(&ring, &message1, &retired) ==
	    INTEL_AX211_TX_RING_OUT_OF_ORDER);
	message0.generation = 60U;
	TEST_CHECK(intel_ax211_tx_ring_complete(&ring, &message0, &retired) ==
	    INTEL_AX211_TX_RING_STALE);
	message0.generation = 61U;
	TEST_CHECK(intel_ax211_tx_ring_complete(&ring, &message0, &retired) ==
	    INTEL_AX211_TX_RING_OK);
	TEST_CHECK(intel_ax211_tx_ring_complete(&ring, &message0, &retired) ==
	    INTEL_AX211_TX_RING_DUPLICATE);
	put_le16(payload1 + 30U, 63U);
	TEST_CHECK(intel_ax211_tx_ring_complete(&ring, &message1, &retired) ==
	    INTEL_AX211_TX_RING_MALFORMED);
	put_le16(payload1 + 30U, (uint16_t)request.length);
	put_le32(payload1 + 40U, 0x83U);
	TEST_CHECK(intel_ax211_tx_ring_complete(&ring, &message1, &retired) ==
	    INTEL_AX211_TX_RING_TX_FAILED);
	TEST_CHECK(!retired.acknowledged && retired.handle.cookie == 82U);
	TEST_CHECK(ring.pending_count == 0U);

	request.connection_generation = 72U;
	request.cookie = 83U;
	TEST_CHECK(intel_ax211_tx_ring_submit(&ring, &request, 0U, 10U,
	    &handle) == INTEL_AX211_TX_RING_STALE);
	TEST_CHECK(intel_ax211_tx_ring_release(&ring, 1) ==
	    INTEL_AX211_TX_RING_OK);
	return 0;
}

static int
test_timeout_reset_and_kick_failure(void)
{
	uint8_t frame[64U];
	struct intel_ax211_tx_request request;
	struct intel_ax211_tx_ring_handle handle;
	struct intel_ax211_tx_ring ring;
	struct fixture_io io;

	TEST_CHECK(allocator_reset() == 0);
	TEST_CHECK(fixture_ring_open(&ring, &io, 0U, 91U, 92U) == 0);
	fixture_request(&request, frame, 92U, 93U);
	TEST_CHECK(intel_ax211_tx_ring_submit(&ring, &request, 100U, 5U,
	    &handle) == INTEL_AX211_TX_RING_OK);
	TEST_CHECK(intel_ax211_tx_ring_timeout_oldest(&ring, 104U, &handle) ==
	    INTEL_AX211_TX_RING_PENDING);
	TEST_CHECK(intel_ax211_tx_ring_timeout_oldest(&ring, 105U, &handle) ==
	    INTEL_AX211_TX_RING_TIMEOUT);
	TEST_CHECK(ring.poisoned && ring.pending_count == 1U);
	TEST_CHECK(intel_ax211_tx_ring_reset(&ring, 0) ==
	    INTEL_AX211_TX_RING_BARRIER_REQUIRED);
	TEST_CHECK(intel_ax211_tx_ring_reset(&ring, 1) ==
	    INTEL_AX211_TX_RING_OK);
	TEST_CHECK(!ring.enabled && ring.pending_count == 0U && !ring.poisoned);
	TEST_CHECK(intel_ax211_tx_ring_release(&ring, 0) ==
	    INTEL_AX211_TX_RING_OK);

	TEST_CHECK(allocator_reset() == 0);
	TEST_CHECK(fixture_ring_open(&ring, &io, 0U, 101U, 102U) == 0);
	fixture_request(&request, frame, 102U, 103U);
	io.fail_write = 1U;
	TEST_CHECK(intel_ax211_tx_ring_submit(&ring, &request, 0U, 5U,
	    &handle) == INTEL_AX211_TX_RING_KICK_FAILED);
	TEST_CHECK(ring.poisoned && ring.pending_count == 1U &&
	    ring.slot[0U].active && ring.slot[0U].uncertain);
	TEST_CHECK(intel_ax211_tx_ring_release(&ring, 0) ==
	    INTEL_AX211_TX_RING_BARRIER_REQUIRED);
	TEST_CHECK(intel_ax211_tx_ring_release(&ring, 1) ==
	    INTEL_AX211_TX_RING_OK);

	TEST_CHECK(allocator_reset() == 0);
	TEST_CHECK(fixture_ring_open(&ring, &io, 0U, 111U, 112U) == 0);
	fixture_request(&request, frame, 112U, 113U);
	io.fail_sync_call = 2U;
	TEST_CHECK(intel_ax211_tx_ring_submit(&ring, &request, 0U, 5U,
	    &handle) == INTEL_AX211_TX_RING_IO_ERROR);
	TEST_CHECK(!ring.poisoned && ring.pending_count == 0U &&
	    !ring.slot[0U].active);
	TEST_CHECK(intel_ax211_tx_ring_release(&ring, 1) ==
	    INTEL_AX211_TX_RING_OK);
	return 0;
}

static int
test_allocation_rollback(void)
{
	struct intel_ax211_tx_ring ring;
	struct fixture_io io;

	TEST_CHECK(allocator_reset() == 0);
	fixture_fail_allocate_attempt = 13U;
	memset(&ring, 0, sizeof(ring));
	memset(&io, 0, sizeof(io));
	TEST_CHECK(intel_ax211_tx_ring_allocate(&fixture_device, &fixture_ops,
	    &io, &ring) == INTEL_AX211_TX_RING_NO_MEMORY);
	TEST_CHECK(active_allocations() == 0U);
	TEST_CHECK(fixture_free_count == 12U);
	TEST_CHECK(fixture_reverse_failure == 0U && fixture_scrub_failure == 0U);
	TEST_CHECK(!ring.allocated);
	return 0;
}

static int
fixture_ring_open(
	struct intel_ax211_tx_ring *ring,
	struct fixture_io *io,
	uint16_t write_pointer,
	uint32_t hardware_generation,
	uint64_t connection_generation)
{
	uint8_t response[8U];
	struct intel_ax211_protocol_message message;
	struct intel_ax211_protocol_pending_command pending;
	struct intel_ax211_tx_queue_config config;

	memset(ring, 0, sizeof(*ring));
	memset(io, 0, sizeof(*io));
	TEST_CHECK(intel_ax211_tx_ring_allocate(&fixture_device, &fixture_ops,
	    io, ring) == INTEL_AX211_TX_RING_OK);
	TEST_CHECK(intel_ax211_tx_ring_queue_add_build(ring, 2U,
	    INTEL_AX211_TX_RING_MANAGEMENT_TID, write_pointer, &config) ==
	    INTEL_AX211_TX_RING_OK);
	memset(response, 0, sizeof(response));
	put_le16(response, INTEL_AX211_TX_RING_QUEUE);
	put_le16(response + 4U, write_pointer);
	memset(&pending, 0, sizeof(pending));
	pending.group = INTEL_AX211_TX_QUEUE_CONFIG_GROUP;
	pending.opcode = INTEL_AX211_TX_QUEUE_CONFIG_OPCODE;
	pending.response_version =
	    INTEL_AX211_TX_QUEUE_CONFIG_RESPONSE_VERSION;
	pending.queue = 0U;
	pending.index = 7U;
	pending.generation = hardware_generation;
	pending.minimum_response_length = sizeof(response);
	pending.maximum_response_length = sizeof(response);
	memset(&message, 0, sizeof(message));
	message.group = pending.group;
	message.opcode = pending.opcode;
	message.version = pending.response_version;
	message.queue = pending.queue;
	message.index = pending.index;
	message.generation = pending.generation;
	message.payload = response;
	message.payload_length = sizeof(response);
	TEST_CHECK(intel_ax211_tx_ring_queue_add_complete(ring, &config,
	    hardware_generation, connection_generation, &message, &pending) ==
	    INTEL_AX211_TX_RING_OK);
	return 0;
}

static void
fixture_request(
	struct intel_ax211_tx_request *request,
	uint8_t frame[64U],
	uint64_t connection_generation,
	uint64_t cookie)
{
	memset(frame, 0, 64U);
	frame[0U] = 0x08U;
	memset(request, 0, sizeof(*request));
	request->connection_generation = connection_generation;
	request->cookie = cookie;
	request->frame = frame;
	request->length = 40U;
	request->frame_class = INTEL_AX211_TX_FRAME_DATA;
}

static struct intel_ax211_protocol_message
fixture_completion(
	uint8_t payload[48U],
	uint8_t index,
	uint16_t byte_count,
	uint16_t next_sequence,
	uint32_t hardware_generation,
	uint32_t status)
{
	struct intel_ax211_protocol_message message;

	memset(payload, 0, 48U);
	payload[0U] = 1U;
	payload[2U] = 2U;
	payload[3U] = 3U;
	put_le16(payload + 28U, 0x1230U);
	put_le16(payload + 30U, byte_count);
	put_le16(payload + 36U, INTEL_AX211_TX_RING_QUEUE);
	put_le32(payload + 40U, status);
	put_le32(payload + 44U, next_sequence);
	memset(&message, 0, sizeof(message));
	message.group = INTEL_AX211_TX_GROUP;
	message.opcode = INTEL_AX211_TX_OPCODE;
	message.version = INTEL_AX211_TX_NOTIFICATION_VERSION;
	message.queue = INTEL_AX211_TX_RING_QUEUE;
	message.index = index;
	message.generation = hardware_generation;
	message.payload = payload;
	message.payload_length = 48U;
	return message;
}

static struct fixture_dma_record *
record_from_buffer(
	const struct drv_dma_buffer *buffer)
{
	uintptr_t identity;

	if (buffer == NULL)
		return NULL;
	identity = buffer->private_data[0];
	if (identity == 0U || identity > FIXTURE_RECORD_COUNT)
		return NULL;
	return &fixture_record[identity - 1U];
}

static int
allocator_reset(void)
{
	TEST_CHECK(active_allocations() == 0U);
	memset(fixture_record, 0, sizeof(fixture_record));
	fixture_allocate_attempt = 0U;
	fixture_allocate_success = 0U;
	fixture_free_count = 0U;
	fixture_fail_allocate_attempt = 0U;
	fixture_reverse_failure = 0U;
	fixture_scrub_failure = 0U;
	fixture_next_address = UINT64_C(0x100000000);
	return 0;
}

static unsigned
active_allocations(void)
{
	unsigned count;
	unsigned index;

	count = 0U;
	for (index = 0U; index < FIXTURE_RECORD_COUNT; index++) {
		if (fixture_record[index].active)
			count++;
	}
	return count;
}

static unsigned
latest_active_sequence(void)
{
	unsigned latest;
	unsigned index;

	latest = 0U;
	for (index = 0U; index < FIXTURE_RECORD_COUNT; index++) {
		if (fixture_record[index].active &&
		    fixture_record[index].sequence > latest)
			latest = fixture_record[index].sequence;
	}
	return latest;
}

static int
bytes_are(
	const uint8_t *bytes,
	size_t length,
	uint8_t value)
{
	size_t index;

	for (index = 0U; index < length; index++) {
		if (bytes[index] != value)
			return 0;
	}
	return 1;
}

static uint64_t
align_up(
	uint64_t value,
	size_t alignment)
{
	uint64_t mask;

	mask = (uint64_t)alignment - 1U;
	return (value + mask) & ~mask;
}

static uint16_t
get_le16(
	const uint8_t *bytes)
{
	return (uint16_t)bytes[0U] | ((uint16_t)bytes[1U] << 8);
}

static uint32_t
get_le32(
	const uint8_t *bytes)
{
	return (uint32_t)bytes[0U] | ((uint32_t)bytes[1U] << 8) |
	    ((uint32_t)bytes[2U] << 16) | ((uint32_t)bytes[3U] << 24);
}

static uint64_t
get_le64(
	const uint8_t *bytes)
{
	return (uint64_t)get_le32(bytes) |
	    ((uint64_t)get_le32(bytes + 4U) << 32);
}

static void
put_le16(
	uint8_t *bytes,
	uint16_t value)
{
	bytes[0U] = (uint8_t)value;
	bytes[1U] = (uint8_t)(value >> 8);
}

static void
put_le32(
	uint8_t *bytes,
	uint32_t value)
{
	bytes[0U] = (uint8_t)value;
	bytes[1U] = (uint8_t)(value >> 8);
	bytes[2U] = (uint8_t)(value >> 16);
	bytes[3U] = (uint8_t)(value >> 24);
}

int
main(void)
{
	int result;

	result = test_api_and_queue_config();
	if (result == 0)
		result = test_submit_and_completion();
	if (result == 0)
		result = test_wrap_and_ring_full();
	if (result == 0)
		result = test_order_stale_duplicate_and_failure();
	if (result == 0)
		result = test_timeout_reset_and_kick_failure();
	if (result == 0)
		result = test_allocation_rollback();
	if (result != 0)
		return 1;
	puts("intel ax211 Gen3 TX ring tests passed");
	return 0;
}
