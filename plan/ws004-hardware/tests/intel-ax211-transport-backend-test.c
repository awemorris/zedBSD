/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/* Intel AX211 transport backend adapter fixture. SPDX-License-Identifier: Zlib */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../../src/drivers/intel-ax211-transport-backend.h"

#define TEST_BAR_SIZE                                      0x4000U
#define TEST_GP_CNTRL                                      0x0024U
#define TEST_GP_MAC_CLOCK_READY                        0x00000001U

struct drv_dma_device {
	int coherent;
};

struct test_lower {
	struct intel_ax211_pci_mmio_backend pci;
	uint64_t now;
	uint64_t clock_value;
	uint64_t trace_start;
	uint64_t trace_deadline;
	enum intel_ax211_mmio_wait trace_wait;
	uint32_t prph_value;
	unsigned int csr_reads;
	unsigned int csr_writes;
	unsigned int prph_reads;
	unsigned int prph_writes;
	unsigned int delays;
	unsigned int traces;
	int csr_read_error;
	int csr_write_error;
	int prph_read_error;
	int prph_write_error;
	int delay_error;
	int clock_error;
	int force_all_ones;
};

struct test_memory {
	uint8_t command_tfd[INTEL_AX211_COMMAND_TFD_RING_SIZE];
	uint8_t command_byte_count[INTEL_AX211_COMMAND_BC_TABLE_SIZE];
	uint8_t command_slots[INTEL_AX211_COMMAND_SLOTS_SIZE];
	uint8_t command_external[INTEL_AX211_COMMAND_EXTERNAL_SIZE];
	uint8_t rx_transfer[INTEL_AX211_RX_TRANSFER_RING_SIZE];
	uint8_t rx_completion[INTEL_AX211_RX_COMPLETION_RING_SIZE];
	uint8_t rx_status[INTEL_AX211_RX_STATUS_SIZE];
};

struct test_fixture {
	struct test_lower lower;
	struct drv_dma_device device;
	struct intel_ax211_mmio mmio;
	struct intel_ax211_dma_resources dma;
	struct intel_ax211_transport_backend backend;
	struct test_memory memory;
	uint32_t registers[TEST_BAR_SIZE / sizeof(uint32_t)];
};

static unsigned int io_rmb_count;
static unsigned int io_wmb_count;

static int test_csr_read32(void *argument, uint32_t offset, uint32_t *value);
static int test_csr_write32(void *argument, uint32_t offset, uint32_t value);
static int test_prph_read32(void *argument, uint32_t address, uint32_t *value);
static int test_prph_write32(void *argument, uint32_t address, uint32_t value);
static int test_delay_us(void *argument, uint32_t duration_us);
static int test_clock_us(void *argument, uint64_t *time_us);
static void test_trace_deadline(void *argument,
	enum intel_ax211_mmio_wait wait, uint64_t start_us,
	uint64_t deadline_us);
static void test_fixture_init(struct test_fixture *fixture);
static void test_buffer_init(struct drv_dma_buffer *buffer, void *address,
	size_t size, uint64_t device_address);
static uint32_t test_register_read(const struct test_fixture *fixture,
	uint32_t offset);
static void test_init_and_ring_view(void);
static void test_csr_and_byte_write(void);
static void test_clock_delay_and_trace(void);
static void test_nic_and_prph_delegation(void);
static void test_dma_region_fences(void);

static const struct intel_ax211_mmio_ops test_mmio_ops = {
	test_csr_read32,
	test_csr_write32,
	test_prph_read32,
	test_prph_write32,
	test_delay_us,
	test_clock_us,
	test_trace_deadline
};

int
main(void)
{
	test_init_and_ring_view();
	test_csr_and_byte_write();
	test_clock_delay_and_trace();
	test_nic_and_prph_delegation();
	test_dma_region_fences();
	puts("intel ax211 transport backend tests: PASS");
	return 0;
}

int
drv_dma_device_is_coherent(
	const struct drv_dma_device *device)
{
	return device != NULL && device->coherent;
}

void
hal_io_rmb(void)
{
	io_rmb_count++;
}

void
hal_io_wmb(void)
{
	io_wmb_count++;
}

static int
test_csr_read32(
	void *argument,
	uint32_t offset,
	uint32_t *value)
{
	struct test_lower *lower;

	lower = argument;
	lower->csr_reads++;
	if (value == NULL || lower->csr_read_error ||
	    (offset & 3U) != 0U || (size_t)offset > lower->pci.mapping_size ||
	    sizeof(uint32_t) > lower->pci.mapping_size - (size_t)offset)
		return -1;
	if (lower->force_all_ones)
		*value = UINT32_MAX;
	else
		*value = *(volatile uint32_t *)(lower->pci.registers + offset);
	return 0;
}

static int
test_csr_write32(
	void *argument,
	uint32_t offset,
	uint32_t value)
{
	struct test_lower *lower;

	lower = argument;
	lower->csr_writes++;
	if (lower->csr_write_error || (offset & 3U) != 0U ||
	    (size_t)offset > lower->pci.mapping_size ||
	    sizeof(uint32_t) > lower->pci.mapping_size - (size_t)offset)
		return -1;
	*(volatile uint32_t *)(lower->pci.registers + offset) = value;
	return 0;
}

static int
test_prph_read32(
	void *argument,
	uint32_t address,
	uint32_t *value)
{
	struct test_lower *lower;

	lower = argument;
	lower->prph_reads++;
	if (value == NULL || lower->prph_read_error || address > 0xffffffU)
		return -1;
	*value = lower->prph_value;
	return 0;
}

static int
test_prph_write32(
	void *argument,
	uint32_t address,
	uint32_t value)
{
	struct test_lower *lower;

	lower = argument;
	lower->prph_writes++;
	if (lower->prph_write_error || address > 0xffffffU)
		return -1;
	lower->prph_value = value;
	return 0;
}

static int
test_delay_us(
	void *argument,
	uint32_t duration_us)
{
	struct test_lower *lower;

	lower = argument;
	lower->delays++;
	if (lower->delay_error)
		return -1;
	lower->now += duration_us;
	lower->clock_value += duration_us;
	return 0;
}

static int
test_clock_us(
	void *argument,
	uint64_t *time_us)
{
	struct test_lower *lower;

	lower = argument;
	if (time_us == NULL || lower->clock_error)
		return -1;
	*time_us = lower->clock_value;
	return 0;
}

static void
test_trace_deadline(
	void *argument,
	enum intel_ax211_mmio_wait wait,
	uint64_t start_us,
	uint64_t deadline_us)
{
	struct test_lower *lower;

	lower = argument;
	lower->traces++;
	lower->trace_wait = wait;
	lower->trace_start = start_us;
	lower->trace_deadline = deadline_us;
}

static void
test_fixture_init(
	struct test_fixture *fixture)
{
	struct intel_ax211_mmio_profile profile;

	memset(fixture, 0, sizeof(*fixture));
	fixture->lower.pci.registers = (volatile uint8_t *)fixture->registers;
	fixture->lower.pci.mapping_size = sizeof(fixture->registers);
	fixture->lower.pci.counter_ready = 1U;
	fixture->device.coherent = 1;
	memset(&profile, 0, sizeof(profile));
	profile.mac_type = INTEL_AX211_MMIO_MAC_SO;
	profile.rf_type = INTEL_AX211_MMIO_RF_GF;
	profile.umac_prph_offset = INTEL_AX211_MMIO_UMAC_PRPH_OFFSET;
	assert(intel_ax211_mmio_init(&fixture->mmio, &test_mmio_ops,
	    &fixture->lower.pci, &profile) == INTEL_AX211_MMIO_OK);
	fixture->dma.device = &fixture->device;
	fixture->dma.boot_prepared = 1U;
	test_buffer_init(&fixture->dma.command_tfd,
	    fixture->memory.command_tfd, sizeof(fixture->memory.command_tfd),
	    0x100000U);
	test_buffer_init(&fixture->dma.command_byte_count,
	    fixture->memory.command_byte_count,
	    sizeof(fixture->memory.command_byte_count), 0x200000U);
	test_buffer_init(&fixture->dma.command_slots,
	    fixture->memory.command_slots,
	    sizeof(fixture->memory.command_slots), 0x300000U);
	test_buffer_init(&fixture->dma.command_external,
	    fixture->memory.command_external,
	    sizeof(fixture->memory.command_external), 0x700000U);
	test_buffer_init(&fixture->dma.rx_transfer,
	    fixture->memory.rx_transfer,
	    sizeof(fixture->memory.rx_transfer), 0x400000U);
	test_buffer_init(&fixture->dma.rx_completion,
	    fixture->memory.rx_completion,
	    sizeof(fixture->memory.rx_completion), 0x500000U);
	test_buffer_init(&fixture->dma.rx_status, fixture->memory.rx_status,
	    sizeof(fixture->memory.rx_status), 0x600000U);
	assert(intel_ax211_transport_backend_init(&fixture->backend,
	    &fixture->mmio, &fixture->lower.pci, &fixture->dma) ==
	    INTEL_AX211_TRANSPORT_BACKEND_OK);
}

static void
test_buffer_init(
	struct drv_dma_buffer *buffer,
	void *address,
	size_t size,
	uint64_t device_address)
{
	memset(buffer, 0, sizeof(*buffer));
	buffer->address = address;
	buffer->size = size;
	buffer->device_address = device_address;
	buffer->private_data[0] = 1U;
}

static uint32_t
test_register_read(
	const struct test_fixture *fixture,
	uint32_t offset)
{
	return *(const volatile uint32_t *)((const uint8_t *)
	    fixture->registers + offset);
}

static void
test_init_and_ring_view(void)
{
	struct intel_ax211_transport_ring_memory memory;
	struct intel_ax211_transport_ring_memory snapshot;
	struct intel_ax211_transport transport;
	struct intel_ax211_transport_backend output;
	struct intel_ax211_transport_backend output_snapshot;
	struct test_fixture fixture;

	test_fixture_init(&fixture);
	memset(&memory, 0, sizeof(memory));
	assert(intel_ax211_transport_backend_ring_memory(&fixture.backend,
	    &memory) == INTEL_AX211_TRANSPORT_BACKEND_OK);
	assert(memory.command_tfd == fixture.memory.command_tfd);
	assert(memory.command_byte_count == fixture.memory.command_byte_count);
	assert(memory.command_slots == fixture.memory.command_slots);
	assert(memory.command_slots_device_address == 0x300000U);
	assert(memory.command_external == fixture.memory.command_external);
	assert(memory.command_external_size ==
	    sizeof(fixture.memory.command_external));
	assert(memory.command_external_device_address == 0x700000U);
	assert(memory.rx_transfer == fixture.memory.rx_transfer);
	assert(memory.rx_completion == fixture.memory.rx_completion);
	assert(memory.rx_status == fixture.memory.rx_status);
	assert(intel_ax211_transport_init(&transport,
	    intel_ax211_transport_backend_ops(), &fixture.backend,
	    &fixture.mmio.profile, &memory) == INTEL_AX211_TRANSPORT_OK);

	memset(&memory, 0xa5, sizeof(memory));
	snapshot = memory;
	fixture.dma.rx_status.size--;
	assert(intel_ax211_transport_backend_ring_memory(&fixture.backend,
	    &memory) == INTEL_AX211_TRANSPORT_BACKEND_NOT_READY);
	assert(memcmp(&memory, &snapshot, sizeof(memory)) == 0);
	fixture.dma.rx_status.size++;

	memset(&output, 0x5a, sizeof(output));
	output_snapshot = output;
	fixture.mmio.argument = NULL;
	assert(intel_ax211_transport_backend_init(&output, &fixture.mmio,
	    &fixture.lower.pci, &fixture.dma) ==
	    INTEL_AX211_TRANSPORT_BACKEND_INVALID);
	assert(memcmp(&output, &output_snapshot, sizeof(output)) == 0);
	fixture.mmio.argument = &fixture.lower.pci;
	fixture.device.coherent = 0;
	assert(intel_ax211_transport_backend_init(&output, &fixture.mmio,
	    &fixture.lower.pci, &fixture.dma) ==
	    INTEL_AX211_TRANSPORT_BACKEND_NOT_COHERENT);
	fixture.device.coherent = 1;
	fixture.dma.command_slots.device_address++;
	assert(intel_ax211_transport_backend_init(&output, &fixture.mmio,
	    &fixture.lower.pci, &fixture.dma) ==
	    INTEL_AX211_TRANSPORT_BACKEND_NOT_READY);
	fixture.dma.command_slots.device_address--;
	fixture.dma.command_external.device_address++;
	assert(intel_ax211_transport_backend_init(&output, &fixture.mmio,
	    &fixture.lower.pci, &fixture.dma) ==
	    INTEL_AX211_TRANSPORT_BACKEND_NOT_READY);
	fixture.dma.command_external.device_address--;
	fixture.dma.boot_prepared = 0U;
	assert(intel_ax211_transport_backend_init(&output, &fixture.mmio,
	    &fixture.lower.pci, &fixture.dma) ==
	    INTEL_AX211_TRANSPORT_BACKEND_NOT_READY);
}

static void
test_csr_and_byte_write(void)
{
	const struct intel_ax211_transport_ops *ops;
	struct test_fixture fixture;
	uint32_t value;
	unsigned int reads;

	test_fixture_init(&fixture);
	ops = intel_ax211_transport_backend_ops();
	assert(ops != NULL);
	assert(ops->csr_write32(&fixture.backend, 0x20U, 0x11223344U) == 0);
	assert(test_register_read(&fixture, 0x20U) == 0x11223344U);
	value = 0U;
	assert(ops->csr_read32(&fixture.backend, 0x20U, &value) == 0);
	assert(value == 0x11223344U);
	io_wmb_count = 0U;
	assert(ops->csr_write8(&fixture.backend, TEST_BAR_SIZE - 1U, 0x5aU)
	    == 0);
	assert(((uint8_t *)fixture.registers)[TEST_BAR_SIZE - 1U] == 0x5aU);
	assert(io_wmb_count == 1U);

	fixture.lower.force_all_ones = 1;
	value = 0xabcdef01U;
	assert(ops->csr_read32(&fixture.backend, 0x20U, &value) != 0);
	assert(value == 0xabcdef01U);
	assert(fixture.backend.failed);
	reads = fixture.lower.csr_reads;
	assert(ops->csr_read32(&fixture.backend, 0x20U, &value) != 0);
	assert(fixture.lower.csr_reads == reads);

	test_fixture_init(&fixture);
	assert(ops->csr_write8(&fixture.backend, TEST_BAR_SIZE, 0U) != 0);
	assert(fixture.backend.failed);
	test_fixture_init(&fixture);
	fixture.lower.csr_write_error = 1;
	assert(ops->csr_write32(&fixture.backend, 0x20U, 1U) != 0);
	assert(fixture.backend.failed);
}

static void
test_clock_delay_and_trace(void)
{
	const struct intel_ax211_transport_ops *ops;
	struct test_fixture fixture;
	uint64_t now;

	test_fixture_init(&fixture);
	ops = intel_ax211_transport_backend_ops();
	fixture.lower.clock_value = 10U;
	assert(ops->clock_us(&fixture.backend, &now) == 0 && now == 10U);
	fixture.lower.clock_value = 20U;
	assert(ops->clock_us(&fixture.backend, &now) == 0 && now == 20U);
	assert(ops->delay_us(&fixture.backend, 5U) == 0);
	assert(fixture.lower.delays == 1U);
	assert(fixture.lower.clock_value == 25U);
	ops->trace_deadline(&fixture.backend,
	    INTEL_AX211_TRANSPORT_WAIT_RX_IDLE, 25U, 125U);
	assert(fixture.lower.traces == 1U);
	assert(fixture.lower.trace_wait ==
	    INTEL_AX211_MMIO_WAIT_MASTER_DISABLED);
	assert(fixture.lower.trace_start == 25U);
	assert(fixture.lower.trace_deadline == 125U);

	fixture.lower.clock_value = 19U;
	now = 0xfeedU;
	assert(ops->clock_us(&fixture.backend, &now) != 0);
	assert(now == 0xfeedU);
	assert(fixture.backend.failed);
	test_fixture_init(&fixture);
	fixture.lower.delay_error = 1;
	assert(ops->delay_us(&fixture.backend, 1U) != 0);
	assert(fixture.backend.failed);
}

static void
test_nic_and_prph_delegation(void)
{
	const struct intel_ax211_transport_ops *ops;
	struct test_fixture fixture;
	uint32_t ready;
	uint32_t value;

	test_fixture_init(&fixture);
	ops = intel_ax211_transport_backend_ops();
	fixture.mmio.apm_ready = 1;
	ready = TEST_GP_MAC_CLOCK_READY;
	*(uint32_t *)((uint8_t *)fixture.registers + TEST_GP_CNTRL) = ready;
	assert(ops->nic_lock(&fixture.backend) == 0);
	assert(fixture.mmio.nic_lock_depth == 1U);
	assert(ops->prph_write32(&fixture.backend, 0x1234U,
	    0x55667788U) == 0);
	assert(fixture.lower.prph_writes == 1U);
	assert(ops->prph_read32(&fixture.backend, 0x1234U, &value) == 0);
	assert(value == 0x55667788U);
	assert(fixture.lower.prph_reads == 1U);
	assert(ops->nic_unlock(&fixture.backend) == 0);
	assert(fixture.mmio.nic_lock_depth == 0U);
	assert((test_register_read(&fixture, TEST_GP_CNTRL) &
	    TEST_GP_MAC_CLOCK_READY) != 0U);

	assert(ops->nic_lock(&fixture.backend) == 0);
	fixture.lower.prph_read_error = 1;
	assert(ops->prph_read32(&fixture.backend, 0x1234U, &value) != 0);
	assert(fixture.backend.failed);
	/* Failure latching must not prevent release of an acquired scope. */
	assert(ops->nic_unlock(&fixture.backend) == 0);
	assert(fixture.mmio.nic_lock_depth == 0U);
}

static void
test_dma_region_fences(void)
{
	const struct intel_ax211_transport_ops *ops;
	struct test_fixture fixture;

	test_fixture_init(&fixture);
	ops = intel_ax211_transport_backend_ops();
	io_rmb_count = 0U;
	io_wmb_count = 0U;
	assert(ops->dma_sync(&fixture.backend,
	    INTEL_AX211_TRANSPORT_DMA_COMMAND_TFD, 0U,
	    INTEL_AX211_COMMAND_TFD_RING_SIZE,
	    INTEL_AX211_TRANSPORT_DMA_PREWRITE) == 0);
	assert(io_wmb_count == 1U);
	assert(ops->dma_sync(&fixture.backend,
	    INTEL_AX211_TRANSPORT_DMA_COMMAND_EXTERNAL, 0U,
	    INTEL_AX211_COMMAND_EXTERNAL_SIZE,
	    INTEL_AX211_TRANSPORT_DMA_PREWRITE) == 0);
	assert(io_wmb_count == 2U);
	assert(ops->dma_sync(&fixture.backend,
	    INTEL_AX211_TRANSPORT_DMA_RX_COMPLETION, 16U, 32U,
	    INTEL_AX211_TRANSPORT_DMA_PREREAD) == 0);
	assert(io_wmb_count == 3U);
	assert(ops->dma_sync(&fixture.backend,
	    INTEL_AX211_TRANSPORT_DMA_RX_STATUS, 0U, 2U,
	    INTEL_AX211_TRANSPORT_DMA_POSTREAD) == 0);
	assert(io_rmb_count == 1U);
	assert(ops->dma_sync(&fixture.backend,
	    INTEL_AX211_TRANSPORT_DMA_RX_STATUS, 2U, 1U,
	    INTEL_AX211_TRANSPORT_DMA_POSTREAD) != 0);
	assert(ops->dma_sync(&fixture.backend,
	    INTEL_AX211_TRANSPORT_DMA_COMMAND_TFD, 0U, 1U,
	    INTEL_AX211_TRANSPORT_DMA_POSTREAD) != 0);
	assert(ops->dma_sync(&fixture.backend,
	    INTEL_AX211_TRANSPORT_DMA_RX_COMPLETION, 0U, 1U,
	    INTEL_AX211_TRANSPORT_DMA_PREWRITE) != 0);
	assert(ops->dma_sync(&fixture.backend,
	    (enum intel_ax211_transport_dma_region)99, 0U, 1U,
	    INTEL_AX211_TRANSPORT_DMA_PREWRITE) != 0);
	assert(ops->dma_sync(&fixture.backend,
	    INTEL_AX211_TRANSPORT_DMA_RX_STATUS, 0U, 1U,
	    (enum intel_ax211_transport_dma_direction)99) != 0);
	fixture.device.coherent = 0;
	assert(ops->dma_sync(&fixture.backend,
	    INTEL_AX211_TRANSPORT_DMA_RX_STATUS, 0U, 1U,
	    INTEL_AX211_TRANSPORT_DMA_POSTREAD) != 0);
	fixture.device.coherent = 1;
	fixture.dma.boot_prepared = 0U;
	assert(ops->dma_sync(&fixture.backend,
	    INTEL_AX211_TRANSPORT_DMA_RX_STATUS, 0U, 1U,
	    INTEL_AX211_TRANSPORT_DMA_POSTREAD) != 0);
}
