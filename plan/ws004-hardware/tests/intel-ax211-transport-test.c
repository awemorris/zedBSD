/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * Intel AX211 private Gen3 transport fixture
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../../src/drivers/intel-ax211-transport.h"

#define TEST_CSR_WORDS                                      3000U
#define TEST_EVENT_CAPACITY                                12000U
#define TEST_NEVER                         UINT64_C(0xffffffffffffffff)
#define TEST_COMMAND_GROUP_LEGACY                             0U
#define TEST_COMMAND_GROUP_LONG                               1U

#define TEST_COMMAND_TFD_SIZE                              65536U
#define TEST_COMMAND_BYTE_COUNT_SIZE                        2048U
#define TEST_COMMAND_SLOTS_SIZE                            82944U
#define TEST_COMMAND_EXTERNAL_SIZE                          4096U
#define TEST_RX_TRANSFER_SIZE                               8192U
#define TEST_RX_COMPLETION_SIZE                            16384U
#define TEST_RX_STATUS_SIZE                                    2U

#define TEST_CSR_INT_COALESCING                             0x004U
#define TEST_CSR_UCODE_DRV_GP1_CLR                         0x05cU
#define TEST_CSR_MAC_SHADOW_REG_CTRL                       0x0a8U
#define TEST_HBUS_TARG_WRPTR                               0x460U
#define TEST_RFH_Q0_FRBDCB_WIDX_TRG                       0x1c80U
#define TEST_MSIX_FH_CAUSES                                0x2800U
#define TEST_MSIX_FH_MASK                                  0x2804U
#define TEST_MSIX_HW_CAUSES                                0x2808U
#define TEST_MSIX_HW_MASK                                  0x280cU
#define TEST_MSIX_AUTOMASK                                 0x2810U
#define TEST_MSIX_RX_IVAR                                  0x2880U
#define TEST_MSIX_IVAR                                     0x2890U

#define TEST_FH_Q0                                     0x00000001U
#define TEST_FH_Q1                                     0x00000002U
#define TEST_FH_D2S0                                   0x00010000U
#define TEST_FH_D2S1                                   0x00020000U
#define TEST_FH_S2D                                    0x00080000U
#define TEST_FH_ERROR                                  0x00200000U
#define TEST_FH_SUPPORTED (TEST_FH_Q0 | TEST_FH_Q1 | TEST_FH_D2S0 | \
	TEST_FH_D2S1 | TEST_FH_S2D | TEST_FH_ERROR)

#define TEST_HW_ALIVE                                  0x00000001U
#define TEST_HW_WAKEUP                                 0x00000002U
#define TEST_HW_RESET                                  0x00000004U
#define TEST_HW_TOP_FATAL                              0x00000008U
#define TEST_HW_SW_ERROR_V2                            0x00000020U
#define TEST_HW_CT_KILL                                0x00000040U
#define TEST_HW_RF_KILL                                0x00000080U
#define TEST_HW_PERIODIC                               0x00000100U
#define TEST_HW_SW_ERROR                               0x02000000U
#define TEST_HW_SCD                                    0x04000000U
#define TEST_HW_FH_TX                                  0x08000000U
#define TEST_HW_ERROR                                  0x20000000U
#define TEST_HW_HAP                                    0x40000000U
#define TEST_HW_SUPPORTED (TEST_HW_ALIVE | TEST_HW_WAKEUP | TEST_HW_RESET | \
	TEST_HW_SW_ERROR_V2 | TEST_HW_CT_KILL | TEST_HW_RF_KILL | \
	TEST_HW_PERIODIC | TEST_HW_SW_ERROR | TEST_HW_SCD | TEST_HW_FH_TX | \
	TEST_HW_ERROR | TEST_HW_HAP)

#define TEST_UMAC_CHICK                                 0x00d05c00U
#define TEST_UMAC_MSIX_ENABLE                           0x02000000U
#define TEST_UMAC_RX_STATUS                             0x00d07824U
#define TEST_UMAC_RX_CONFIG                             0x00d07880U
#define TEST_RX_IDLE                                    0x80000000U

enum test_event_type {
	TEST_EVENT_CSR_READ = 1,
	TEST_EVENT_CSR_WRITE = 2,
	TEST_EVENT_CSR_WRITE8 = 3,
	TEST_EVENT_LOCK = 4,
	TEST_EVENT_UNLOCK = 5,
	TEST_EVENT_PRPH_READ = 6,
	TEST_EVENT_PRPH_WRITE = 7,
	TEST_EVENT_DMA = 8,
	TEST_EVENT_DEADLINE = 9
};

struct test_event {
	enum test_event_type type;
	uint32_t address;
	uint32_t value;
	size_t offset;
	size_t length;
	uint64_t start;
	uint64_t end;
};

struct test_backend {
	uint32_t csr[TEST_CSR_WORDS];
	struct test_event event[TEST_EVENT_CAPACITY];
	size_t event_count;
	uint64_t now;
	uint64_t rx_idle_at;
	unsigned int lock_depth;
	int fail_lock;
	int fail_write32;
	uint32_t fail_write32_offset;
	int fail_write8;
	uint32_t fail_write8_offset;
	int fail_dma;
	enum intel_ax211_transport_dma_region fail_dma_region;
	int clock_stuck;
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
	struct test_memory storage;
	struct intel_ax211_transport transport;
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
static void test_record(struct test_backend *backend, enum test_event_type type, uint32_t address, uint32_t value, size_t offset, size_t length, uint64_t start, uint64_t end);
static void test_fixture_init(struct test_fixture *fixture);
static struct intel_ax211_mmio_profile test_profile(void);
static struct intel_ax211_transport_ring_memory test_ring_memory(struct test_fixture *fixture);
static void test_transport_init(struct test_fixture *fixture);
static void test_transport_ready(struct test_fixture *fixture);
static size_t test_find_event(const struct test_backend *backend, size_t start, enum test_event_type type, uint32_t address, uint32_t value);
static size_t test_count_event(const struct test_backend *backend, enum test_event_type type);
static uint16_t test_get_le16(const uint8_t *bytes);
static uint64_t test_get_le64(const uint8_t *bytes);
static void test_put_le16(uint8_t *bytes, uint16_t value);
static void test_exact_initialization(void);
static void test_msix_configuration(void);
static void test_ring_initialization(void);
static void test_interrupt_modes(void);
static void test_rx_transport(void);
static void test_command_transport(void);
static void test_external_command_transport(void);
static void test_quiesce(void);

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
	test_exact_initialization();
	test_msix_configuration();
	test_ring_initialization();
	test_interrupt_modes();
	test_rx_transport();
	test_command_transport();
	test_external_command_transport();
	test_quiesce();
	puts("intel ax211 transport tests: PASS");
	return 0;
}

/* Reads one mock CSR, preserving hardware-owned cause contents. */
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
	test_record(backend, TEST_EVENT_CSR_READ, offset, *value,
	    0U, 0U, 0U, 0U);
	return 0;
}

/* Writes one mock CSR, including W1C cause semantics. */
static int
test_csr_write32(
	void *argument,
	uint32_t offset,
	uint32_t value)
{
	struct test_backend *backend;

	backend = argument;
	assert(offset / 4U < TEST_CSR_WORDS);
	test_record(backend, TEST_EVENT_CSR_WRITE, offset, value,
	    0U, 0U, 0U, 0U);
	if (backend->fail_write32 && offset == backend->fail_write32_offset)
		return -1;
	if (offset == TEST_MSIX_FH_CAUSES || offset == TEST_MSIX_HW_CAUSES)
		backend->csr[offset / 4U] &= ~value;
	else
		backend->csr[offset / 4U] = value;
	return 0;
}

/* Writes one mock byte-addressed CSR or IVAR entry. */
static int
test_csr_write8(
	void *argument,
	uint32_t offset,
	uint8_t value)
{
	struct test_backend *backend;

	backend = argument;
	test_record(backend, TEST_EVENT_CSR_WRITE8, offset, value,
	    0U, 0U, 0U, 0U);
	if (backend->fail_write8 && offset == backend->fail_write8_offset)
		return -1;
	return 0;
}

/* Acquires one deterministic mock NIC ownership reference. */
static int
test_nic_lock(
	void *argument)
{
	struct test_backend *backend;

	backend = argument;
	test_record(backend, TEST_EVENT_LOCK, 0U, 0U, 0U, 0U, 0U, 0U);
	if (backend->fail_lock)
		return -1;
	backend->lock_depth++;
	return 0;
}

/* Releases one deterministic mock NIC ownership reference. */
static int
test_nic_unlock(
	void *argument)
{
	struct test_backend *backend;

	backend = argument;
	test_record(backend, TEST_EVENT_UNLOCK, 0U, 0U, 0U, 0U, 0U, 0U);
	if (backend->lock_depth == 0U)
		return -1;
	backend->lock_depth--;
	return 0;
}

/* Reads one mock PRPH register while ownership is held. */
static int
test_prph_read32(
	void *argument,
	uint32_t address,
	uint32_t *value)
{
	struct test_backend *backend;

	backend = argument;
	assert(backend->lock_depth != 0U);
	*value = 0U;
	if (address == TEST_UMAC_RX_STATUS &&
	    backend->now >= backend->rx_idle_at)
		*value = TEST_RX_IDLE;
	test_record(backend, TEST_EVENT_PRPH_READ, address, *value,
	    0U, 0U, 0U, 0U);
	return 0;
}

/* Writes one mock PRPH register while ownership is held. */
static int
test_prph_write32(
	void *argument,
	uint32_t address,
	uint32_t value)
{
	struct test_backend *backend;

	backend = argument;
	assert(backend->lock_depth != 0U);
	test_record(backend, TEST_EVENT_PRPH_WRITE, address, value,
	    0U, 0U, 0U, 0U);
	return 0;
}

/* Records one checked DMA synchronization operation. */
static int
test_dma_sync(
	void *argument,
	enum intel_ax211_transport_dma_region region,
	size_t offset,
	size_t length,
	enum intel_ax211_transport_dma_direction direction)
{
	struct test_backend *backend;

	backend = argument;
	test_record(backend, TEST_EVENT_DMA, (uint32_t)region,
	    (uint32_t)direction, offset, length, 0U, 0U);
	if (backend->fail_dma && region == backend->fail_dma_region)
		return -1;
	return 0;
}

/* Advances the deterministic mock clock unless it is deliberately stuck. */
static int
test_delay_us(
	void *argument,
	uint32_t duration_us)
{
	struct test_backend *backend;

	backend = argument;
	if (!backend->clock_stuck)
		backend->now += duration_us;
	return 0;
}

/* Returns the deterministic mock clock. */
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

/* Records one finite transport deadline. */
static void
test_trace_deadline(
	void *argument,
	enum intel_ax211_transport_wait wait,
	uint64_t start_us,
	uint64_t deadline_us)
{
	struct test_backend *backend;

	backend = argument;
	test_record(backend, TEST_EVENT_DEADLINE, (uint32_t)wait, 0U,
	    0U, 0U, start_us, deadline_us);
}

/* Appends one bounded mock event. */
static void
test_record(
	struct test_backend *backend,
	enum test_event_type type,
	uint32_t address,
	uint32_t value,
	size_t offset,
	size_t length,
	uint64_t start,
	uint64_t end)
{
	struct test_event *event;

	assert(backend->event_count < TEST_EVENT_CAPACITY);
	event = &backend->event[backend->event_count++];
	event->type = type;
	event->address = address;
	event->value = value;
	event->offset = offset;
	event->length = length;
	event->start = start;
	event->end = end;
}

/* Initializes one zeroed fixture with no spontaneous RX-idle status. */
static void
test_fixture_init(
	struct test_fixture *fixture)
{
	memset(fixture, 0, sizeof(*fixture));
	fixture->backend.rx_idle_at = TEST_NEVER;
}

/* Returns the exact P038 SO/GF transport profile. */
static struct intel_ax211_mmio_profile
test_profile(void)
{
	struct intel_ax211_mmio_profile profile;

	memset(&profile, 0, sizeof(profile));
	profile.mac_type = INTEL_AX211_MMIO_MAC_SO;
	profile.rf_type = INTEL_AX211_MMIO_RF_GF;
	profile.umac_prph_offset = INTEL_AX211_MMIO_UMAC_PRPH_OFFSET;
	return profile;
}

/* Returns exact-size ring memory backed by the fixture arrays. */
static struct intel_ax211_transport_ring_memory
test_ring_memory(
	struct test_fixture *fixture)
{
	struct intel_ax211_transport_ring_memory memory;

	memset(&memory, 0, sizeof(memory));
	memory.command_tfd = fixture->storage.command_tfd;
	memory.command_tfd_size = sizeof(fixture->storage.command_tfd);
	memory.command_byte_count = fixture->storage.command_byte_count;
	memory.command_byte_count_size =
	    sizeof(fixture->storage.command_byte_count);
	memory.command_slots = fixture->storage.command_slots;
	memory.command_slots_size = sizeof(fixture->storage.command_slots);
	memory.command_slots_device_address = UINT64_C(0x10000000);
	memory.command_external = fixture->storage.command_external;
	memory.command_external_size =
	    sizeof(fixture->storage.command_external);
	memory.command_external_device_address = UINT64_C(0x20000000);
	memory.rx_transfer = fixture->storage.rx_transfer;
	memory.rx_transfer_size = sizeof(fixture->storage.rx_transfer);
	memory.rx_completion = fixture->storage.rx_completion;
	memory.rx_completion_size = sizeof(fixture->storage.rx_completion);
	memory.rx_status = fixture->storage.rx_status;
	memory.rx_status_size = sizeof(fixture->storage.rx_status);
	return memory;
}

/* Initializes the production transport against one mock fixture. */
static void
test_transport_init(
	struct test_fixture *fixture)
{
	struct intel_ax211_mmio_profile profile;
	struct intel_ax211_transport_ring_memory memory;

	profile = test_profile();
	memory = test_ring_memory(fixture);
	assert(intel_ax211_transport_init(&fixture->transport, &test_ops,
	    &fixture->backend, &profile, &memory) ==
	    INTEL_AX211_TRANSPORT_OK);
}

/* Configures MSI-X and initializes rings for one focused operation. */
static void
test_transport_ready(
	struct test_fixture *fixture)
{
	test_transport_init(fixture);
	assert(intel_ax211_transport_configure_msix(&fixture->transport) ==
	    INTEL_AX211_TRANSPORT_OK);
	assert(intel_ax211_transport_initialize_rings(&fixture->transport) ==
	    INTEL_AX211_TRANSPORT_OK);
}

/* Finds one exact event in production order. */
static size_t
test_find_event(
	const struct test_backend *backend,
	size_t start,
	enum test_event_type type,
	uint32_t address,
	uint32_t value)
{
	size_t index;

	/* Searches the bounded event trace. */
	for (index = start; index < backend->event_count; index++) {
		if (backend->event[index].type == type &&
		    backend->event[index].address == address &&
		    backend->event[index].value == value)
			return index;
	}
	return backend->event_count;
}

/* Counts one event class in the bounded trace. */
static size_t
test_count_event(
	const struct test_backend *backend,
	enum test_event_type type)
{
	size_t index;
	size_t count;

	/* Counts every matching event without interpreting its payload. */
	count = 0U;
	for (index = 0U; index < backend->event_count; index++) {
		if (backend->event[index].type == type)
			count++;
	}
	return count;
}

/* Decodes one little-endian test word. */
static uint16_t
test_get_le16(
	const uint8_t *bytes)
{
	return (uint16_t)((uint16_t)bytes[0] |
	    ((uint16_t)bytes[1] << 8));
}

/* Decodes one little-endian test double word. */
static uint64_t
test_get_le64(
	const uint8_t *bytes)
{
	uint64_t value;
	unsigned int index;

	/* Decodes all eight bytes without alignment assumptions. */
	value = 0U;
	for (index = 0U; index < 8U; index++)
		value |= (uint64_t)bytes[index] << (index * 8U);
	return value;
}

/* Encodes one little-endian test word. */
static void
test_put_le16(
	uint8_t *bytes,
	uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

/* Proves exact profile, operation, and fixed-memory validation. */
static void
test_exact_initialization(void)
{
	struct test_fixture fixture;
	struct intel_ax211_mmio_profile profile;
	struct intel_ax211_transport_ring_memory memory;
	struct intel_ax211_transport_ops ops;

	test_fixture_init(&fixture);
	profile = test_profile();
	memory = test_ring_memory(&fixture);
	assert(intel_ax211_transport_init(&fixture.transport, &test_ops,
	    &fixture.backend, &profile, &memory) ==
	    INTEL_AX211_TRANSPORT_OK);
	profile.mac_type = INTEL_AX211_MMIO_MAC_SOF;
	assert(intel_ax211_transport_init(&fixture.transport, &test_ops,
	    &fixture.backend, &profile, &memory) ==
	    INTEL_AX211_TRANSPORT_OK);
	profile = test_profile();
	profile.rf_type++;
	assert(intel_ax211_transport_init(&fixture.transport, &test_ops,
	    &fixture.backend, &profile, &memory) ==
	    INTEL_AX211_TRANSPORT_INVALID);
	profile = test_profile();
	profile.cdb = 1U;
	assert(intel_ax211_transport_init(&fixture.transport, &test_ops,
	    &fixture.backend, &profile, &memory) ==
	    INTEL_AX211_TRANSPORT_INVALID);
	profile = test_profile();
	profile.integrated = 1U;
	assert(intel_ax211_transport_init(&fixture.transport, &test_ops,
	    &fixture.backend, &profile, &memory) ==
	    INTEL_AX211_TRANSPORT_INVALID);
	profile = test_profile();
	memory.rx_status_size++;
	assert(intel_ax211_transport_init(&fixture.transport, &test_ops,
	    &fixture.backend, &profile, &memory) ==
	    INTEL_AX211_TRANSPORT_INVALID);
	memory = test_ring_memory(&fixture);
	memory.command_slots_device_address++;
	assert(intel_ax211_transport_init(&fixture.transport, &test_ops,
	    &fixture.backend, &profile, &memory) ==
	    INTEL_AX211_TRANSPORT_INVALID);
	memory = test_ring_memory(&fixture);
	memory.command_slots_device_address = UINT64_MAX - 63U;
	assert(intel_ax211_transport_init(&fixture.transport, &test_ops,
	    &fixture.backend, &profile, &memory) ==
	    INTEL_AX211_TRANSPORT_INVALID);
	memory = test_ring_memory(&fixture);
	memory.command_external_size--;
	assert(intel_ax211_transport_init(&fixture.transport, &test_ops,
	    &fixture.backend, &profile, &memory) ==
	    INTEL_AX211_TRANSPORT_INVALID);
	memory = test_ring_memory(&fixture);
	memory.command_external_device_address++;
	assert(intel_ax211_transport_init(&fixture.transport, &test_ops,
	    &fixture.backend, &profile, &memory) ==
	    INTEL_AX211_TRANSPORT_INVALID);
	memory = test_ring_memory(&fixture);
	memory.command_external_device_address = UINT64_MAX - 63U;
	assert(intel_ax211_transport_init(&fixture.transport, &test_ops,
	    &fixture.backend, &profile, &memory) ==
	    INTEL_AX211_TRANSPORT_INVALID);
	memory = test_ring_memory(&fixture);
	ops = test_ops;
	ops.nic_lock = NULL;
	assert(intel_ax211_transport_init(&fixture.transport, &ops,
	    &fixture.backend, &profile, &memory) ==
	    INTEL_AX211_TRANSPORT_INVALID);
	assert(fixture.backend.event_count == 0U);
}

/* Proves exact single-vector MSI-X route and mask ordering. */
static void
test_msix_configuration(void)
{
	struct test_fixture fixture;
	static const uint32_t route[] = {
		TEST_MSIX_RX_IVAR,
		TEST_MSIX_RX_IVAR + 1U,
		TEST_MSIX_IVAR + 0x00U,
		TEST_MSIX_IVAR + 0x01U,
		TEST_MSIX_IVAR + 0x03U,
		TEST_MSIX_IVAR + 0x05U,
		TEST_MSIX_IVAR + 0x10U,
		TEST_MSIX_IVAR + 0x11U,
		TEST_MSIX_IVAR + 0x12U,
		TEST_MSIX_IVAR + 0x16U,
		TEST_MSIX_IVAR + 0x17U,
		TEST_MSIX_IVAR + 0x18U,
		TEST_MSIX_IVAR + 0x29U,
		TEST_MSIX_IVAR + 0x15U,
		TEST_MSIX_IVAR + 0x2aU,
		TEST_MSIX_IVAR + 0x2bU,
		TEST_MSIX_IVAR + 0x2dU,
		TEST_MSIX_IVAR + 0x2eU
	};
	size_t index;
	size_t position;

	test_fixture_init(&fixture);
	test_transport_init(&fixture);
	assert(intel_ax211_transport_configure_msix(&fixture.transport) ==
	    INTEL_AX211_TRANSPORT_OK);
	position = test_find_event(&fixture.backend, 0U, TEST_EVENT_LOCK,
	    0U, 0U) + 1U;
	position = test_find_event(&fixture.backend, position,
	    TEST_EVENT_PRPH_WRITE, TEST_UMAC_CHICK,
	    TEST_UMAC_MSIX_ENABLE) + 1U;
	position = test_find_event(&fixture.backend, position,
	    TEST_EVENT_UNLOCK, 0U, 0U) + 1U;
	position = test_find_event(&fixture.backend, position,
	    TEST_EVENT_CSR_WRITE, TEST_MSIX_FH_MASK, UINT32_MAX) + 1U;
	position = test_find_event(&fixture.backend, position,
	    TEST_EVENT_CSR_WRITE, TEST_MSIX_HW_MASK, UINT32_MAX) + 1U;
	position = test_find_event(&fixture.backend, position,
	    TEST_EVENT_CSR_WRITE8, TEST_MSIX_RX_IVAR, 0x80U);
	assert(position < fixture.backend.event_count);
	for (index = 0U; index < sizeof(route) / sizeof(route[0]); index++) {
		assert(position + index < fixture.backend.event_count);
		assert(fixture.backend.event[position + index].type ==
		    TEST_EVENT_CSR_WRITE8);
		assert(fixture.backend.event[position + index].address ==
		    route[index]);
		assert(fixture.backend.event[position + index].value == 0x80U);
	}
	assert(test_count_event(&fixture.backend, TEST_EVENT_CSR_WRITE8) == 18U);
	assert(fixture.backend.csr[TEST_MSIX_FH_MASK / 4U] == UINT32_MAX);
	assert(fixture.backend.csr[TEST_MSIX_HW_MASK / 4U] == UINT32_MAX);
	assert(fixture.transport.msix_configured);
	assert(!fixture.transport.interrupts_enabled);

	test_fixture_init(&fixture);
	test_transport_init(&fixture);
	fixture.backend.fail_write8 = 1;
	fixture.backend.fail_write8_offset = TEST_MSIX_RX_IVAR;
	assert(intel_ax211_transport_configure_msix(&fixture.transport) ==
	    INTEL_AX211_TRANSPORT_IO);
	assert(fixture.transport.failed && fixture.transport.quiesced);
	assert(fixture.backend.csr[TEST_MSIX_FH_MASK / 4U] == UINT32_MAX);
	assert(fixture.backend.csr[TEST_MSIX_HW_MASK / 4U] == UINT32_MAX);

	test_fixture_init(&fixture);
	test_transport_init(&fixture);
	fixture.backend.fail_lock = 1;
	assert(intel_ax211_transport_configure_msix(&fixture.transport) ==
	    INTEL_AX211_TRANSPORT_IO);
	assert(fixture.backend.event_count == 1U);
	assert(fixture.backend.event[0].type == TEST_EVENT_LOCK);
}

/* Proves ring clearing, DMA direction, and hardware setup order. */
static void
test_ring_initialization(void)
{
	struct test_fixture fixture;
	size_t position;

	test_fixture_init(&fixture);
	memset(&fixture.storage, 0xa5, sizeof(fixture.storage));
	test_transport_init(&fixture);
	assert(intel_ax211_transport_initialize_rings(&fixture.transport) ==
	    INTEL_AX211_TRANSPORT_OK);
	assert(fixture.storage.command_tfd[0] == 0U);
	assert(fixture.storage.command_slots[TEST_COMMAND_SLOTS_SIZE - 1U] == 0U);
	assert(fixture.storage.rx_completion[0] == 0U);
	assert(fixture.storage.rx_status[0] == 0U);
	assert(fixture.storage.rx_transfer[0] == 0xa5U);
	position = test_find_event(&fixture.backend, 0U, TEST_EVENT_DMA,
	    INTEL_AX211_TRANSPORT_DMA_COMMAND_TFD,
	    INTEL_AX211_TRANSPORT_DMA_PREWRITE) + 1U;
	position = test_find_event(&fixture.backend, position, TEST_EVENT_DMA,
	    INTEL_AX211_TRANSPORT_DMA_COMMAND_BYTE_COUNT,
	    INTEL_AX211_TRANSPORT_DMA_PREWRITE) + 1U;
	position = test_find_event(&fixture.backend, position, TEST_EVENT_DMA,
	    INTEL_AX211_TRANSPORT_DMA_COMMAND_SLOTS,
	    INTEL_AX211_TRANSPORT_DMA_PREWRITE) + 1U;
	position = test_find_event(&fixture.backend, position,
	    TEST_EVENT_CSR_WRITE8, TEST_CSR_INT_COALESCING, 0x40U) + 1U;
	assert(position <= fixture.backend.event_count);
	assert((fixture.backend.csr[TEST_CSR_MAC_SHADOW_REG_CTRL / 4U] &
	    0x800fffffU) == 0x800fffffU);

	test_fixture_init(&fixture);
	test_transport_init(&fixture);
	fixture.backend.fail_dma = 1;
	fixture.backend.fail_dma_region =
	    INTEL_AX211_TRANSPORT_DMA_RX_STATUS;
	assert(intel_ax211_transport_initialize_rings(&fixture.transport) ==
	    INTEL_AX211_TRANSPORT_IO);
	assert(fixture.transport.failed && fixture.transport.quiesced);
}

/* Proves firmware/runtime cause masks, W1C acknowledgement, and rearm. */
static void
test_interrupt_modes(void)
{
	struct test_fixture fixture;
	struct intel_ax211_transport_causes causes;
	size_t first_clear;
	size_t second_clear;

	test_fixture_init(&fixture);
	test_transport_ready(&fixture);
	fixture.backend.csr[TEST_MSIX_FH_CAUSES / 4U] = TEST_FH_D2S0;
	fixture.backend.csr[TEST_MSIX_HW_CAUSES / 4U] = TEST_HW_ALIVE;
	assert(intel_ax211_transport_enable_firmware_interrupts(
	    &fixture.transport) == INTEL_AX211_TRANSPORT_OK);
	assert(fixture.transport.enabled_fh_causes == TEST_FH_SUPPORTED);
	assert(fixture.transport.enabled_hw_causes == TEST_HW_ALIVE);
	assert(fixture.backend.csr[TEST_MSIX_FH_MASK / 4U] ==
	    ~TEST_FH_SUPPORTED);
	assert(fixture.backend.csr[TEST_MSIX_HW_MASK / 4U] ==
	    ~TEST_HW_ALIVE);
	first_clear = test_find_event(&fixture.backend, 0U,
	    TEST_EVENT_CSR_WRITE, TEST_CSR_UCODE_DRV_GP1_CLR, 2U);
	second_clear = test_find_event(&fixture.backend, first_clear + 1U,
	    TEST_EVENT_CSR_WRITE, TEST_CSR_UCODE_DRV_GP1_CLR, 4U);
	assert(first_clear < second_clear);

	assert(intel_ax211_transport_enable_runtime_interrupts(
	    &fixture.transport) == INTEL_AX211_TRANSPORT_OK);
	fixture.backend.csr[TEST_MSIX_FH_CAUSES / 4U] =
	    TEST_FH_Q0 | 0x80000000U;
	fixture.backend.csr[TEST_MSIX_HW_CAUSES / 4U] =
	    TEST_HW_RF_KILL | TEST_HW_TOP_FATAL;
	assert(intel_ax211_transport_interrupt_claim(&fixture.transport,
	    &causes) == INTEL_AX211_TRANSPORT_OK);
	assert(causes.flow_handler == TEST_FH_Q0);
	assert(causes.hardware == TEST_HW_RF_KILL);
	assert(fixture.backend.csr[TEST_MSIX_FH_CAUSES / 4U] == 0U);
	assert(fixture.backend.csr[TEST_MSIX_HW_CAUSES / 4U] == 0U);
	assert(intel_ax211_transport_interrupt_rearm(&fixture.transport) ==
	    INTEL_AX211_TRANSPORT_OK);
	assert(fixture.backend.csr[TEST_MSIX_AUTOMASK / 4U] == 1U);
	assert(intel_ax211_transport_disable_interrupts(&fixture.transport) ==
	    INTEL_AX211_TRANSPORT_OK);
	assert(!fixture.transport.interrupts_enabled);
	assert(fixture.backend.csr[TEST_MSIX_FH_MASK / 4U] == UINT32_MAX);
	assert(fixture.backend.csr[TEST_MSIX_HW_MASK / 4U] == UINT32_MAX);
}

/* Proves all-descriptor activation and completion/replenishment bookkeeping. */
static void
test_rx_transport(void)
{
	struct test_fixture fixture;
	struct intel_ax211_transport_rx_completion completion;
	uint64_t address;
	size_t completion_postread;
	size_t completion_preread;
	size_t status_postread;
	size_t status_preread;
	uint16_t index;
	uint8_t *descriptor;

	test_fixture_init(&fixture);
	test_transport_ready(&fixture);
	for (index = 0U;
	     index < INTEL_AX211_TRANSPORT_RX_DESCRIPTOR_COUNT - 1U;
	     index++) {
		address = UINT64_C(0x20000000) + (uint64_t)index * 4096U;
		assert(intel_ax211_transport_publish_rx_descriptor(
		    &fixture.transport, index, address) ==
		    INTEL_AX211_TRANSPORT_OK);
	}
	assert(intel_ax211_transport_activate_rx(&fixture.transport) ==
	    INTEL_AX211_TRANSPORT_ORDER);
	index = INTEL_AX211_TRANSPORT_RX_DESCRIPTOR_COUNT - 1U;
	address = UINT64_C(0x20000000) + (uint64_t)index * 4096U;
	assert(intel_ax211_transport_publish_rx_descriptor(&fixture.transport,
	    index, address) == INTEL_AX211_TRANSPORT_OK);
	assert(intel_ax211_transport_activate_rx(&fixture.transport) ==
	    INTEL_AX211_TRANSPORT_OK);
	assert(fixture.backend.csr[TEST_RFH_Q0_FRBDCB_WIDX_TRG / 4U] == 8U);

	test_put_le16(fixture.storage.rx_status, 1U);
	descriptor = fixture.storage.rx_completion;
	test_put_le16(descriptor + 4U, 7U);
	descriptor[6] = 0xa5U;
	fixture.backend.event_count = 0U;
	assert(intel_ax211_transport_rx_next(&fixture.transport, &completion) ==
	    INTEL_AX211_TRANSPORT_OK);
	status_postread = test_find_event(&fixture.backend, 0U, TEST_EVENT_DMA,
	    INTEL_AX211_TRANSPORT_DMA_RX_STATUS,
	    INTEL_AX211_TRANSPORT_DMA_POSTREAD);
	status_preread = test_find_event(&fixture.backend, status_postread + 1U,
	    TEST_EVENT_DMA, INTEL_AX211_TRANSPORT_DMA_RX_STATUS,
	    INTEL_AX211_TRANSPORT_DMA_PREREAD);
	completion_postread = test_find_event(&fixture.backend,
	    status_preread + 1U, TEST_EVENT_DMA,
	    INTEL_AX211_TRANSPORT_DMA_RX_COMPLETION,
	    INTEL_AX211_TRANSPORT_DMA_POSTREAD);
	assert(status_postread < status_preread &&
	    status_preread < completion_postread);
	assert(completion.completion_index == 0U);
	assert(completion.buffer_id == 7U && completion.flags == 0xa5U);
	assert(intel_ax211_transport_rx_next(&fixture.transport, &completion) ==
	    INTEL_AX211_TRANSPORT_ORDER);
	assert(intel_ax211_transport_rx_replenish(&fixture.transport,
	    UINT64_C(0x30007001)) == INTEL_AX211_TRANSPORT_INVALID);
	assert(fixture.transport.rx_pending && !fixture.transport.failed);
	address = UINT64_C(0x30007000);
	assert(intel_ax211_transport_rx_replenish(&fixture.transport, address) ==
	    INTEL_AX211_TRANSPORT_OK);
	completion_preread = test_find_event(&fixture.backend,
	    completion_postread + 1U, TEST_EVENT_DMA,
	    INTEL_AX211_TRANSPORT_DMA_RX_COMPLETION,
	    INTEL_AX211_TRANSPORT_DMA_PREREAD);
	assert(completion_preread < fixture.backend.event_count);
	assert(fixture.transport.rx_tail == 1U);
	assert(fixture.backend.csr[TEST_RFH_Q0_FRBDCB_WIDX_TRG / 4U] == 0U);
	descriptor = fixture.storage.rx_transfer +
	    7U * INTEL_AX211_TRANSPORT_RX_DESCRIPTOR_SIZE;
	assert(test_get_le16(descriptor) == 7U);
	assert(test_get_le64(descriptor + 8U) == address);
}

/* Proves inline command publication, doorbells, and in-order retirement. */
static void
test_command_transport(void)
{
	struct test_fixture fixture;
	struct intel_ax211_command_id command;
	struct intel_ax211_ring_token first;
	struct intel_ax211_ring_token second;
	uint8_t payload[3];
	uint8_t split_payload[20];
	uint8_t *slot;
	uint8_t *tfd;
	size_t slots_sync;
	size_t tfd_sync;
	size_t doorbell;

	test_fixture_init(&fixture);
	test_transport_ready(&fixture);
	assert(intel_ax211_transport_enable_runtime_interrupts(
	    &fixture.transport) == INTEL_AX211_TRANSPORT_OK);
	command.opcode = 0x0dU;
	command.group = TEST_COMMAND_GROUP_LONG;
	command.version = 0U;
	payload[0] = 1U;
	payload[1] = 2U;
	payload[2] = 3U;
	fixture.backend.event_count = 0U;
	assert(intel_ax211_transport_command_submit_inline(&fixture.transport,
	    &command, payload, sizeof(payload), &first) ==
	    INTEL_AX211_TRANSPORT_OK);
	assert(first.queue == 0U && first.index == 0U);
	slot = fixture.storage.command_slots;
	assert(slot[0] == 0x0dU && slot[1] == 1U);
	assert(slot[2] == 0U && slot[3] == 0U);
	assert(test_get_le16(slot + 4U) == sizeof(payload));
	assert(slot[7] == 0U);
	assert(memcmp(slot + 8U, payload, sizeof(payload)) == 0);
	tfd = fixture.storage.command_tfd;
	assert(test_get_le16(tfd) == 1U);
	assert(test_get_le16(tfd + 2U) == 11U);
	assert(test_get_le64(tfd + 4U) == UINT64_C(0x10000000));
	slots_sync = test_find_event(&fixture.backend, 0U, TEST_EVENT_DMA,
	    INTEL_AX211_TRANSPORT_DMA_COMMAND_SLOTS,
	    INTEL_AX211_TRANSPORT_DMA_PREWRITE);
	tfd_sync = test_find_event(&fixture.backend, slots_sync + 1U,
	    TEST_EVENT_DMA, INTEL_AX211_TRANSPORT_DMA_COMMAND_TFD,
	    INTEL_AX211_TRANSPORT_DMA_PREWRITE);
	doorbell = test_find_event(&fixture.backend, tfd_sync + 1U,
	    TEST_EVENT_CSR_WRITE, TEST_HBUS_TARG_WRPTR, 1U);
	assert(slots_sync < tfd_sync && tfd_sync < doorbell);

	memset(split_payload, 0x5a, sizeof(split_payload));
	command.group = TEST_COMMAND_GROUP_LEGACY;
	command.version = 3U;
	assert(intel_ax211_transport_command_submit_inline(&fixture.transport,
	    &command, split_payload, sizeof(split_payload), &second) ==
	    INTEL_AX211_TRANSPORT_OK);
	assert(second.index == 1U);
	slot = fixture.storage.command_slots +
	    INTEL_AX211_TRANSPORT_COMMAND_SLOT_SIZE;
	assert(slot[0] == 0x0dU);
	assert(slot[1] == TEST_COMMAND_GROUP_LONG);
	assert(test_get_le16(slot + 4U) == sizeof(split_payload));
	assert(slot[7] == 0U);
	assert(memcmp(slot + INTEL_AX211_WIDE_COMMAND_HEADER_SIZE,
	    split_payload, sizeof(split_payload)) == 0);
	tfd = fixture.storage.command_tfd + INTEL_AX211_TFD_SIZE;
	assert(test_get_le16(tfd) == 2U);
	assert(test_get_le16(tfd + 2U) == 20U);
	assert(test_get_le64(tfd + 4U) == UINT64_C(0x10000144));
	assert(test_get_le16(tfd + 12U) == 8U);
	assert(test_get_le64(tfd + 14U) == UINT64_C(0x10000158));
	assert(intel_ax211_transport_command_complete(&fixture.transport,
	    &second) == INTEL_AX211_TRANSPORT_STALE);
	assert(intel_ax211_transport_command_complete(&fixture.transport,
	    &first) == INTEL_AX211_TRANSPORT_OK);
	assert(fixture.storage.command_slots[0] == 0U);
	assert(intel_ax211_transport_command_complete(&fixture.transport,
	    &second) == INTEL_AX211_TRANSPORT_OK);
	assert(fixture.transport.command_ring.used == 0U);
	assert(intel_ax211_transport_command_submit_inline(&fixture.transport,
	    &command, payload,
	    INTEL_AX211_TRANSPORT_COMMAND_INLINE_PAYLOAD_MAX + 1U,
	    &first) == INTEL_AX211_TRANSPORT_INVALID);
}

/* Proves bounded external-command ownership from prepare through reset. */
static void
test_external_command_transport(void)
{
	struct test_fixture fixture;
	struct intel_ax211_command_id command;
	struct intel_ax211_ring_token external;
	struct intel_ax211_ring_token inline_token;
	uint8_t payload[1940];
	uint8_t *tfd;
	size_t external_sync;
	size_t tfd_sync;
	size_t doorbell;
	size_t index;

	for (index = 0U; index < sizeof(payload); index++)
		payload[index] = (uint8_t)(index * 13U + 7U);
	command.opcode = 0x0dU;
	command.group = TEST_COMMAND_GROUP_LEGACY;
	command.version = 3U;
	test_fixture_init(&fixture);
	test_transport_ready(&fixture);
	assert(intel_ax211_transport_enable_runtime_interrupts(
	    &fixture.transport) == INTEL_AX211_TRANSPORT_OK);
	fixture.backend.event_count = 0U;
	assert(intel_ax211_transport_command_prepare_external(
	    &fixture.transport, &command, payload, sizeof(payload),
	    &external) == INTEL_AX211_TRANSPORT_OK);
	assert(external.index == 0U && fixture.transport.command_prepared);
	assert(fixture.transport.command_external_active);
	assert(fixture.storage.command_external[0] == command.opcode);
	assert(fixture.storage.command_external[1] ==
	    TEST_COMMAND_GROUP_LONG);
	assert(fixture.storage.command_external[7] == 0U);
	assert(test_get_le16(fixture.storage.command_external + 4U) ==
	    sizeof(payload));
	assert(memcmp(fixture.storage.command_external +
	    INTEL_AX211_WIDE_COMMAND_HEADER_SIZE, payload,
	    sizeof(payload)) == 0);
	for (index = INTEL_AX211_WIDE_COMMAND_HEADER_SIZE + sizeof(payload);
	     index < sizeof(fixture.storage.command_external); index++)
		assert(fixture.storage.command_external[index] == 0U);
	tfd = fixture.storage.command_tfd;
	assert(test_get_le16(tfd) == 2U);
	assert(test_get_le16(tfd + 2U) == 20U);
	assert(test_get_le64(tfd + 4U) == UINT64_C(0x20000000));
	assert(test_get_le16(tfd + 12U) == 1928U);
	assert(test_get_le64(tfd + 14U) == UINT64_C(0x20000014));
	external_sync = test_find_event(&fixture.backend, 0U, TEST_EVENT_DMA,
	    INTEL_AX211_TRANSPORT_DMA_COMMAND_EXTERNAL,
	    INTEL_AX211_TRANSPORT_DMA_PREWRITE);
	tfd_sync = test_find_event(&fixture.backend, external_sync + 1U,
	    TEST_EVENT_DMA, INTEL_AX211_TRANSPORT_DMA_COMMAND_TFD,
	    INTEL_AX211_TRANSPORT_DMA_PREWRITE);
	assert(external_sync < tfd_sync);
	assert(fixture.backend.event[external_sync].length ==
	    sizeof(payload) + INTEL_AX211_WIDE_COMMAND_HEADER_SIZE);
	assert(intel_ax211_transport_command_prepare_external(
	    &fixture.transport, &command, payload, sizeof(payload),
	    &inline_token) == INTEL_AX211_TRANSPORT_ORDER);
	assert(intel_ax211_transport_command_publish(&fixture.transport,
	    &external) == INTEL_AX211_TRANSPORT_OK);
	doorbell = test_find_event(&fixture.backend, tfd_sync + 1U,
	    TEST_EVENT_CSR_WRITE, TEST_HBUS_TARG_WRPTR, 1U);
	assert(tfd_sync < doorbell);
	assert(intel_ax211_transport_command_prepare_external(
	    &fixture.transport, &command, payload, sizeof(payload),
	    &inline_token) == INTEL_AX211_TRANSPORT_FULL);
	assert(intel_ax211_transport_command_submit_inline(&fixture.transport,
	    &command, payload, 1U, &inline_token) ==
	    INTEL_AX211_TRANSPORT_OK);
	assert(intel_ax211_transport_command_complete(&fixture.transport,
	    &inline_token) == INTEL_AX211_TRANSPORT_STALE);
	assert(intel_ax211_transport_command_complete(&fixture.transport,
	    &external) == INTEL_AX211_TRANSPORT_OK);
	assert(!fixture.transport.command_external_active);
	for (index = 0U; index < sizeof(fixture.storage.command_external);
	     index++)
		assert(fixture.storage.command_external[index] == 0U);
	assert(intel_ax211_transport_command_complete(&fixture.transport,
	    &inline_token) == INTEL_AX211_TRANSPORT_OK);

	/* External preparation rejects malformed and oversized payloads. */
	assert(intel_ax211_transport_command_prepare_external(
	    &fixture.transport, &command, NULL, sizeof(payload),
	    &external) == INTEL_AX211_TRANSPORT_INVALID);
	assert(intel_ax211_transport_command_prepare_external(
	    &fixture.transport, &command, payload,
	    INTEL_AX211_TRANSPORT_COMMAND_INLINE_PAYLOAD_MAX,
	    &external) == INTEL_AX211_TRANSPORT_INVALID);
	assert(intel_ax211_transport_command_prepare_external(
	    &fixture.transport, &command, payload,
	    INTEL_AX211_TRANSPORT_COMMAND_EXTERNAL_PAYLOAD_MAX + 1U,
	    &external) == INTEL_AX211_TRANSPORT_INVALID);

	/* An un-rung command is scrubbed, synchronized, and reusable. */
	assert(intel_ax211_transport_command_prepare_external(
	    &fixture.transport, &command, payload, sizeof(payload),
	    &external) == INTEL_AX211_TRANSPORT_OK);
	assert(intel_ax211_transport_command_abort_prepared(
	    &fixture.transport, &external) == INTEL_AX211_TRANSPORT_OK);
	assert(!fixture.transport.command_external_active);
	assert(fixture.transport.command_ring.used == 0U);

	/* A failed stop retains bytes and ownership until the reset boundary. */
	assert(intel_ax211_transport_command_prepare_external(
	    &fixture.transport, &command, payload, sizeof(payload),
	    &external) == INTEL_AX211_TRANSPORT_OK);
	assert(intel_ax211_transport_command_publish(&fixture.transport,
	    &external) == INTEL_AX211_TRANSPORT_OK);
	fixture.backend.rx_idle_at = fixture.backend.now;
	assert(intel_ax211_transport_quiesce(&fixture.transport) ==
	    INTEL_AX211_TRANSPORT_FAILED);
	assert(fixture.transport.command_external_active);
	assert(memcmp(fixture.storage.command_external +
	    INTEL_AX211_WIDE_COMMAND_HEADER_SIZE, payload,
	    sizeof(payload)) == 0);
	assert(intel_ax211_transport_command_after_device_reset(
	    &fixture.transport) == INTEL_AX211_TRANSPORT_OK);
	assert(!fixture.transport.command_external_active);
	for (index = 0U; index < sizeof(fixture.storage.command_external);
	     index++)
		assert(fixture.storage.command_external[index] == 0U);

	/* A hardware-stop failure cannot release or overwrite external DMA. */
	test_fixture_init(&fixture);
	test_transport_ready(&fixture);
	assert(intel_ax211_transport_enable_runtime_interrupts(
	    &fixture.transport) == INTEL_AX211_TRANSPORT_OK);
	assert(intel_ax211_transport_command_prepare_external(
	    &fixture.transport, &command, payload, sizeof(payload),
	    &external) == INTEL_AX211_TRANSPORT_OK);
	assert(intel_ax211_transport_command_publish(&fixture.transport,
	    &external) == INTEL_AX211_TRANSPORT_OK);
	fixture.backend.fail_lock = 1;
	assert(intel_ax211_transport_quiesce(&fixture.transport) ==
	    INTEL_AX211_TRANSPORT_IO);
	assert(fixture.transport.command_external_active);
	assert(memcmp(fixture.storage.command_external +
	    INTEL_AX211_WIDE_COMMAND_HEADER_SIZE, payload,
	    sizeof(payload)) == 0);
	fixture.backend.fail_lock = 0;
	assert(intel_ax211_transport_command_after_device_reset(
	    &fixture.transport) == INTEL_AX211_TRANSPORT_OK);
	assert(!fixture.transport.command_external_active);

	/* A preparation fence failure rolls back the token and fails closed. */
	test_fixture_init(&fixture);
	test_transport_ready(&fixture);
	assert(intel_ax211_transport_enable_runtime_interrupts(
	    &fixture.transport) == INTEL_AX211_TRANSPORT_OK);
	fixture.backend.fail_dma = 1;
	fixture.backend.fail_dma_region =
	    INTEL_AX211_TRANSPORT_DMA_COMMAND_EXTERNAL;
	assert(intel_ax211_transport_command_prepare_external(
	    &fixture.transport, &command, payload, sizeof(payload),
	    &external) == INTEL_AX211_TRANSPORT_IO);
	assert(fixture.transport.failed && fixture.transport.quiesced);
	assert(!fixture.transport.command_external_active);
	assert(fixture.transport.command_ring.used == 0U);

	/* A completion-fence failure retains ownership for reset cleanup. */
	test_fixture_init(&fixture);
	test_transport_ready(&fixture);
	assert(intel_ax211_transport_enable_runtime_interrupts(
	    &fixture.transport) == INTEL_AX211_TRANSPORT_OK);
	assert(intel_ax211_transport_command_prepare_external(
	    &fixture.transport, &command, payload, sizeof(payload),
	    &external) == INTEL_AX211_TRANSPORT_OK);
	assert(intel_ax211_transport_command_publish(&fixture.transport,
	    &external) == INTEL_AX211_TRANSPORT_OK);
	fixture.backend.fail_dma = 1;
	fixture.backend.fail_dma_region =
	    INTEL_AX211_TRANSPORT_DMA_COMMAND_EXTERNAL;
	assert(intel_ax211_transport_command_complete(&fixture.transport,
	    &external) == INTEL_AX211_TRANSPORT_IO);
	assert(fixture.transport.command_external_active);
	assert(fixture.transport.command_ring.used == 1U);
	assert(intel_ax211_transport_command_after_device_reset(
	    &fixture.transport) == INTEL_AX211_TRANSPORT_IO);
	assert(fixture.transport.command_external_active);
	fixture.backend.fail_dma = 0;
	assert(intel_ax211_transport_command_after_device_reset(
	    &fixture.transport) == INTEL_AX211_TRANSPORT_OK);
	assert(!fixture.transport.command_external_active);
}

/* Proves mask/ack/ownership/RX-idle shutdown order and finite failure. */
static void
test_quiesce(void)
{
	struct test_fixture fixture;
	struct intel_ax211_command_id command;
	struct intel_ax211_ring_token token;
	size_t mask;
	size_t lock;
	size_t disable;
	size_t status;
	size_t unlock;
	size_t deadline;

	test_fixture_init(&fixture);
	test_transport_ready(&fixture);
	assert(intel_ax211_transport_enable_runtime_interrupts(
	    &fixture.transport) == INTEL_AX211_TRANSPORT_OK);
	fixture.backend.rx_idle_at = fixture.backend.now + 30U;
	fixture.backend.event_count = 0U;
	assert(intel_ax211_transport_quiesce(&fixture.transport) ==
	    INTEL_AX211_TRANSPORT_OK);
	mask = test_find_event(&fixture.backend, 0U, TEST_EVENT_CSR_WRITE,
	    TEST_MSIX_FH_MASK, UINT32_MAX);
	lock = test_find_event(&fixture.backend, mask + 1U, TEST_EVENT_LOCK,
	    0U, 0U);
	disable = test_find_event(&fixture.backend, lock + 1U,
	    TEST_EVENT_PRPH_WRITE, TEST_UMAC_RX_CONFIG, 0U);
	status = test_find_event(&fixture.backend, disable + 1U,
	    TEST_EVENT_PRPH_READ, TEST_UMAC_RX_STATUS, TEST_RX_IDLE);
	unlock = test_find_event(&fixture.backend, status + 1U,
	    TEST_EVENT_UNLOCK, 0U, 0U);
	assert(mask < lock && lock < disable && disable < status &&
	    status < unlock);
	deadline = test_find_event(&fixture.backend, 0U,
	    TEST_EVENT_DEADLINE, INTEL_AX211_TRANSPORT_WAIT_RX_IDLE, 0U);
	assert(deadline < fixture.backend.event_count);
	assert(fixture.backend.event[deadline].end -
	    fixture.backend.event[deadline].start == 10000U);
	assert(fixture.transport.rx_dma_idle);
	assert(fixture.backend.lock_depth == 0U);

	test_fixture_init(&fixture);
	test_transport_ready(&fixture);
	assert(intel_ax211_transport_quiesce(&fixture.transport) ==
	    INTEL_AX211_TRANSPORT_TIMEOUT);
	assert(fixture.transport.failed && !fixture.transport.rx_dma_idle);
	assert(fixture.backend.lock_depth == 0U);

	test_fixture_init(&fixture);
	test_transport_ready(&fixture);
	assert(intel_ax211_transport_enable_runtime_interrupts(
	    &fixture.transport) == INTEL_AX211_TRANSPORT_OK);
	command.opcode = 1U;
	command.group = 1U;
	command.version = 1U;
	assert(intel_ax211_transport_command_submit_inline(&fixture.transport,
	    &command, NULL, 0U, &token) == INTEL_AX211_TRANSPORT_OK);
	fixture.backend.rx_idle_at = fixture.backend.now;
	assert(intel_ax211_transport_quiesce(&fixture.transport) ==
	    INTEL_AX211_TRANSPORT_FAILED);
	assert(fixture.transport.rx_dma_idle && fixture.transport.failed);

	test_fixture_init(&fixture);
	test_transport_ready(&fixture);
	fixture.backend.clock_stuck = 1;
	assert(intel_ax211_transport_quiesce(&fixture.transport) ==
	    INTEL_AX211_TRANSPORT_CLOCK);
	assert(fixture.backend.lock_depth == 0U);
}
