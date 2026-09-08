/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD Intel AX211 transport backend adapter
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "intel-ax211-transport-backend.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Kept local so this C89-clean private boundary does not import HAL inlines. */
extern void hal_io_rmb(void);
extern void hal_io_wmb(void);

static int ax211_backend_valid(const struct intel_ax211_transport_backend *backend);
static int ax211_backend_ready(const struct intel_ax211_transport_backend *backend);
static int ax211_backend_buffer_valid(const struct drv_dma_buffer *buffer, size_t exact_size, uint64_t alignment);
static int ax211_backend_csr_read32(void *argument, uint32_t offset, uint32_t *value);
static void ax211_backend_fail(struct intel_ax211_transport_backend *backend);
static int ax211_backend_csr_write32(void *argument, uint32_t offset, uint32_t value);
static int ax211_backend_csr_write8(void *argument, uint32_t offset, uint8_t value);
static int ax211_backend_bar_byte_valid( const struct intel_ax211_pci_mmio_backend *pci_mmio, uint32_t offset);
static int ax211_backend_nic_lock(void *argument);
static int ax211_backend_nic_unlock(void *argument);
static int ax211_backend_prph_read32(void *argument, uint32_t address, uint32_t *value);
static int ax211_backend_prph_write32(void *argument, uint32_t address, uint32_t value);
static int ax211_backend_dma_sync(void *argument, enum intel_ax211_transport_dma_region region, size_t offset, size_t length, enum intel_ax211_transport_dma_direction direction);
static struct drv_dma_buffer * ax211_backend_region(struct intel_ax211_transport_backend *backend, enum intel_ax211_transport_dma_region region);
static size_t ax211_backend_region_size(enum intel_ax211_transport_dma_region region);
static int ax211_backend_direction_valid( enum intel_ax211_transport_dma_region region, enum intel_ax211_transport_dma_direction direction);
static int ax211_backend_range_valid(size_t offset, size_t length, size_t capacity);
static int ax211_backend_delay_us(void *argument, uint32_t duration_us);
static int ax211_backend_clock_us(void *argument, uint64_t *time_us);
static void ax211_backend_trace_deadline(void *argument, enum intel_ax211_transport_wait wait, uint64_t start_us, uint64_t deadline_us);

static const struct intel_ax211_transport_ops ax211_backend_operations = {
	ax211_backend_csr_read32,    ax211_backend_csr_write32,
	ax211_backend_csr_write8,    ax211_backend_nic_lock,
	ax211_backend_nic_unlock,    ax211_backend_prph_read32,
	ax211_backend_prph_write32,  ax211_backend_dma_sync,
	ax211_backend_delay_us,	     ax211_backend_clock_us,
	ax211_backend_trace_deadline};

/*
 * Binds three controller-owned objects without touching hardware or DMA.
 */
int
drv_intel_ax211_transport_backend_init(
	struct intel_ax211_transport_backend *backend,
	struct intel_ax211_mmio *mmio,
	struct intel_ax211_pci_mmio_backend *pci_mmio,
	struct intel_ax211_dma_resources *dma)
{
	struct intel_ax211_transport_backend candidate;

	/* Handles the backend availability. */
	if (backend == NULL || mmio == NULL || pci_mmio == NULL || dma == NULL)
		return INTEL_AX211_TRANSPORT_BACKEND_INVALID;
	memset(&candidate, 0, sizeof(candidate));
	candidate.mmio = mmio;
	candidate.pci_mmio = pci_mmio;
	candidate.dma = dma;
	candidate.initialized = 1U;

	/* Checks the ax211 backend valid result. */
	if (!ax211_backend_valid(&candidate))
		return INTEL_AX211_TRANSPORT_BACKEND_INVALID;

	/* Checks the ax211 backend ready result. */
	if (!ax211_backend_ready(&candidate))
		return INTEL_AX211_TRANSPORT_BACKEND_NOT_READY;

	/* Checks the drv dma device is coherent result. */
	if (!drv_dma_device_is_coherent(dma->device))
		return INTEL_AX211_TRANSPORT_BACKEND_NOT_COHERENT;
	*backend = candidate;
	/* Returns the computed result. */
	return INTEL_AX211_TRANSPORT_BACKEND_OK;
}

/*
 * Implements the drv intel ax211 transport backend ops operation.
 */
const struct intel_ax211_transport_ops *
drv_intel_ax211_transport_backend_ops(
	void)
{
	/* Returns the computed result. */
	return &ax211_backend_operations;
}

/*
 * Creates a complete borrowed ring view only after every buffer validates.
 */
int
drv_intel_ax211_transport_backend_ring_memory(
	const struct intel_ax211_transport_backend *backend,
	struct intel_ax211_transport_ring_memory *memory)
{
	struct intel_ax211_transport_ring_memory candidate;

	/* Checks the ax211 backend valid result. */
	if (memory == NULL || !ax211_backend_valid(backend))
		return INTEL_AX211_TRANSPORT_BACKEND_INVALID;

	/* Checks the operation status. */
	if (backend->failed || !ax211_backend_ready(backend))
		return INTEL_AX211_TRANSPORT_BACKEND_NOT_READY;

	/* Checks the drv dma device is coherent result. */
	if (!drv_dma_device_is_coherent(backend->dma->device))
		return INTEL_AX211_TRANSPORT_BACKEND_NOT_COHERENT;
	memset(&candidate, 0, sizeof(candidate));
	candidate.command_tfd = backend->dma->command_tfd.address;
	candidate.command_tfd_size = backend->dma->command_tfd.size;
	candidate.command_byte_count = backend->dma->command_byte_count.address;
	candidate.command_byte_count_size =
		backend->dma->command_byte_count.size;
	candidate.command_slots = backend->dma->command_slots.address;
	candidate.command_slots_size = backend->dma->command_slots.size;
	candidate.command_slots_device_address =
		backend->dma->command_slots.device_address;
	candidate.command_external = backend->dma->command_external.address;
	candidate.command_external_size = backend->dma->command_external.size;
	candidate.command_external_device_address =
		backend->dma->command_external.device_address;
	candidate.rx_transfer = backend->dma->rx_transfer.address;
	candidate.rx_transfer_size = backend->dma->rx_transfer.size;
	candidate.rx_completion = backend->dma->rx_completion.address;
	candidate.rx_completion_size = backend->dma->rx_completion.size;
	candidate.rx_status = backend->dma->rx_status.address;
	candidate.rx_status_size = backend->dma->rx_status.size;
	*memory = candidate;
	/* Returns the computed result. */
	return INTEL_AX211_TRANSPORT_BACKEND_OK;
}

/* Checks the stable object identity shared with the controller MMIO state. */
static int
ax211_backend_valid(
	const struct intel_ax211_transport_backend *backend)
{
	const struct intel_ax211_mmio_ops *ops;

	/* Handles the backend availability. */
	if (backend == NULL || !backend->initialized || backend->mmio == NULL ||
	    backend->pci_mmio == NULL || backend->dma == NULL) {
		/* Succeeded. */
		return 0;
	}

	/* Handles the ops availability. */
	ops = backend->mmio->ops;
	if (ops == NULL || backend->mmio->argument != backend->pci_mmio)
		return 0;

	/* Handles the csr read32 availability. */
	if (ops->csr_read32 == NULL || ops->csr_write32 == NULL ||
	    ops->prph_read32 == NULL || ops->prph_write32 == NULL ||
	    ops->delay_us == NULL || ops->clock_us == NULL) {
		/* Succeeded. */
		return 0;
	}

	/* Handles the registers availability. */
	if (backend->pci_mmio->registers == NULL ||
	    backend->pci_mmio->mapping_size == 0U) {
		/* Succeeded. */
		return 0;
	}

	/* Reports operation failure. */
	return 1;
}

/* Proves that all seven runtime ring allocations still belong to DMA. */
static int
ax211_backend_ready(
	const struct intel_ax211_transport_backend *backend)
{
	int error;
	const struct intel_ax211_dma_resources *dma;

	/* Checks the ax211 backend valid result. */
	if (!ax211_backend_valid(backend))
		return 0;

	/* Handles the device availability. */
	dma = backend->dma;
	if (dma->device == NULL || !dma->boot_prepared)
		return 0;

	/* Checks the ax211 backend buffer valid result. */
	if (!ax211_backend_buffer_valid(
		    &dma->command_tfd, INTEL_AX211_COMMAND_TFD_RING_SIZE, 256U)) {
		/* Succeeded. */
		return 0;
	}

	/* Checks the ax211 backend buffer valid result. */
	if (!ax211_backend_buffer_valid(&dma->command_byte_count,
					INTEL_AX211_COMMAND_BC_TABLE_SIZE,
					128U)) {
		/* Succeeded. */
		return 0;
	}

	/* Checks the ax211 backend buffer valid result. */
	if (!ax211_backend_buffer_valid(&dma->command_slots,
					INTEL_AX211_COMMAND_SLOTS_SIZE, 64U)) {
		/* Succeeded. */
		return 0;
	}

	/* Checks the ax211 backend buffer valid result. */
	if (!ax211_backend_buffer_valid(&dma->command_external,
					INTEL_AX211_COMMAND_EXTERNAL_SIZE, 64U)) {
		/* Succeeded. */
		return 0;
	}

	/* Checks the ax211 backend buffer valid result. */
	if (!ax211_backend_buffer_valid(
		    &dma->rx_transfer, INTEL_AX211_RX_TRANSFER_RING_SIZE, 256U)) {
		/* Succeeded. */
		return 0;
	}

	/* Checks the ax211 backend buffer valid result. */
	if (!ax211_backend_buffer_valid(&dma->rx_completion,
					INTEL_AX211_RX_COMPLETION_RING_SIZE,
					256U)) {
		/* Succeeded. */
		return 0;
	}

	/* Obtains the ax211 backend buffer valid result. */
	error = ax211_backend_buffer_valid(
		&dma->rx_status, INTEL_AX211_RX_STATUS_SIZE, 16U);

	/* Returns the computed result. */
	return error;
}

/* Supports the ax211 backend buffer valid operation. */
static int
ax211_backend_buffer_valid(
	const struct drv_dma_buffer *buffer,
	size_t exact_size,
	uint64_t alignment)
{
	/* Handles the buffer availability. */
	if (buffer == NULL || buffer->address == NULL ||
	    buffer->size != exact_size || buffer->device_address == 0U ||
	    alignment == 0U ||
	    (buffer->device_address & (alignment - 1U)) != 0U) {
		/* Succeeded. */
		return 0;
	}

	/* Handles the buffer condition. */
	if (buffer->device_address > UINT64_MAX - (uint64_t)(exact_size - 1U))
		return 0;

	/* Reports operation failure. */
	return 1;
}

/* Delegates a checked CSR read and treats all ones as device removal. */
static int
ax211_backend_csr_read32(
	void *argument,
	uint32_t offset,
	uint32_t *value)
{
	struct intel_ax211_transport_backend *backend;
	uint32_t candidate;

	/* Checks the operation status. */
	backend = argument;
	if (value == NULL || !ax211_backend_valid(backend) || backend->failed)
		return -1;

	/* Checks the csr read32 result. */
	candidate = 0U;
	if (backend->mmio->ops->csr_read32(backend->mmio->argument, offset,
					   &candidate) != 0 ||
	    candidate == UINT32_MAX) {
		ax211_backend_fail(backend);

		/* Reports operation failure. */
		return -1;
	}

	*value = candidate;
	/* Succeeded. */
	return 0;
}

/* Supports the ax211 backend fail operation. */
static void
ax211_backend_fail(
	struct intel_ax211_transport_backend *backend)
{
	/* Handles the backend availability. */
	if (backend != NULL)
		backend->failed = 1U;
}

/* Supports the ax211 backend csr write32 operation. */
static int
ax211_backend_csr_write32(
	void *argument,
	uint32_t offset,
	uint32_t value)
{
	struct intel_ax211_transport_backend *backend;

	/* Checks the operation status. */
	backend = argument;
	if (!ax211_backend_valid(backend) || backend->failed)
		return -1;

	/* Checks the csr write32 result. */
	if (backend->mmio->ops->csr_write32(backend->mmio->argument, offset,
					    value) != 0) {
		ax211_backend_fail(backend);

		/* Reports operation failure. */
		return -1;
	}

	/* Succeeded. */
	return 0;
}

/* Byte writes are the one operation absent from the shared MMIO contract. */
static int
ax211_backend_csr_write8(
	void *argument,
	uint32_t offset,
	uint8_t value)
{
	struct intel_ax211_transport_backend *backend;

	/* Checks the operation status. */
	backend = argument;
	if (!ax211_backend_valid(backend) || backend->failed ||
	    !ax211_backend_bar_byte_valid(backend->pci_mmio, offset)) {
		/* Handles the backend availability. */
		if (backend != NULL)
			ax211_backend_fail(backend);

		/* Reports operation failure. */
		return -1;
	}

	*(volatile uint8_t *)(backend->pci_mmio->registers + offset) = value;
	hal_io_wmb();

	/* Succeeded. */
	return 0;
}

/* Supports the ax211 backend bar byte valid operation. */
static int
ax211_backend_bar_byte_valid(
	const struct intel_ax211_pci_mmio_backend *pci_mmio,
	uint32_t offset)
{
	/* Handles the pci mmio availability. */
	if (pci_mmio == NULL || pci_mmio->registers == NULL)
		return 0;

	/* Returns the computed result. */
	return (size_t)offset < pci_mmio->mapping_size;
}

/* Supports the ax211 backend nic lock operation. */
static int
ax211_backend_nic_lock(
	void *argument)
{
	struct intel_ax211_transport_backend *backend;

	/* Checks the operation status. */
	backend = argument;
	if (!ax211_backend_valid(backend) || backend->failed)
		return -1;

	/* Checks the drv intel ax211 mmio nic lock result. */
	if (drv_intel_ax211_mmio_nic_lock(backend->mmio) != INTEL_AX211_MMIO_OK)
		return -1;

	/* Succeeded. */
	return 0;
}

/* Unlock remains available after a failure so ownership cannot leak. */
static int
ax211_backend_nic_unlock(
	void *argument)
{
	struct intel_ax211_transport_backend *backend;

	/* Checks the ax211 backend valid result. */
	backend = argument;
	if (!ax211_backend_valid(backend))
		return -1;

	/* Checks the drv intel ax211 mmio nic unlock result. */
	if (drv_intel_ax211_mmio_nic_unlock(backend->mmio) !=
	    INTEL_AX211_MMIO_OK) {
		/* Reports operation failure. */
		return -1;
	}

	/* Succeeded. */
	return 0;
}

/* Supports the ax211 backend prph read32 operation. */
static int
ax211_backend_prph_read32(
	void *argument,
	uint32_t address,
	uint32_t *value)
{
	struct intel_ax211_transport_backend *backend;

	/* Checks the operation status. */
	backend = argument;
	if (value == NULL || !ax211_backend_valid(backend) || backend->failed)
		return -1;

	/* Checks the drv intel ax211 mmio prph read32 result. */
	if (drv_intel_ax211_mmio_prph_read32(backend->mmio, address, value) !=
	    INTEL_AX211_MMIO_OK) {
		ax211_backend_fail(backend);

		/* Reports operation failure. */
		return -1;
	}

	/* Succeeded. */
	return 0;
}

/* Supports the ax211 backend prph write32 operation. */
static int
ax211_backend_prph_write32(
	void *argument,
	uint32_t address,
	uint32_t value)
{
	struct intel_ax211_transport_backend *backend;

	/* Checks the operation status. */
	backend = argument;
	if (!ax211_backend_valid(backend) || backend->failed)
		return -1;

	/* Checks the drv intel ax211 mmio prph write32 result. */
	if (drv_intel_ax211_mmio_prph_write32(backend->mmio, address, value) !=
	    INTEL_AX211_MMIO_OK) {
		ax211_backend_fail(backend);

		/* Reports operation failure. */
		return -1;
	}

	/* Succeeded. */
	return 0;
}

/* Validates an exact coherent allocation, then applies its ownership fence. */
static int
ax211_backend_dma_sync(
	void *argument,
	enum intel_ax211_transport_dma_region region,
	size_t offset,
	size_t length,
	enum intel_ax211_transport_dma_direction direction)
{
	struct intel_ax211_transport_backend *backend;
	struct drv_dma_buffer *buffer;
	size_t exact_size;

	/* Checks the operation status. */
	backend = argument;
	if (!ax211_backend_valid(backend) || backend->failed ||
	    !ax211_backend_ready(backend) ||
	    !drv_dma_device_is_coherent(backend->dma->device)) {
		/* Reports operation failure. */
		return -1;
	}
	buffer = ax211_backend_region(backend, region);

	/* Checks the ax211 backend buffer valid result. */
	exact_size = ax211_backend_region_size(region);
	if (!ax211_backend_buffer_valid(buffer, exact_size, 1U) ||
	    !ax211_backend_direction_valid(region, direction) || length == 0U ||
	    !ax211_backend_range_valid(offset, length, buffer->size)) {
		/* Reports operation failure. */
		return -1;
	}

	/* Handles the direction condition. */
	if (direction == INTEL_AX211_TRANSPORT_DMA_PREWRITE)
		hal_io_wmb();
	else if (direction == INTEL_AX211_TRANSPORT_DMA_PREREAD)
		hal_io_wmb();
	else if (direction == INTEL_AX211_TRANSPORT_DMA_POSTREAD)
		hal_io_rmb();
	else {
		/* Reports operation failure. */
		return -1;
	}

	/* Succeeded. */
	return 0;
}

/* Supports the ax211 backend region operation. */
static struct drv_dma_buffer *
ax211_backend_region(
	struct intel_ax211_transport_backend *backend,
	enum intel_ax211_transport_dma_region region)
{
	/* Dispatch the selected operation case. */
	switch (region) {
	case INTEL_AX211_TRANSPORT_DMA_COMMAND_TFD:
		/* Returns the computed result. */
		return &backend->dma->command_tfd;
	case INTEL_AX211_TRANSPORT_DMA_COMMAND_BYTE_COUNT:
		/* Returns the computed result. */
		return &backend->dma->command_byte_count;
	case INTEL_AX211_TRANSPORT_DMA_COMMAND_SLOTS:
		/* Returns the computed result. */
		return &backend->dma->command_slots;
	case INTEL_AX211_TRANSPORT_DMA_COMMAND_EXTERNAL:
		/* Returns the computed result. */
		return &backend->dma->command_external;
	case INTEL_AX211_TRANSPORT_DMA_RX_TRANSFER:
		/* Returns the computed result. */
		return &backend->dma->rx_transfer;
	case INTEL_AX211_TRANSPORT_DMA_RX_COMPLETION:
		/* Returns the computed result. */
		return &backend->dma->rx_completion;
	case INTEL_AX211_TRANSPORT_DMA_RX_STATUS:
		/* Returns the computed result. */
		return &backend->dma->rx_status;
	default:
		/* Reports that no result is available. */
		return NULL;
	}
}

/* Supports the ax211 backend region size operation. */
static size_t
ax211_backend_region_size(
	enum intel_ax211_transport_dma_region region)
{
	/* Dispatch the selected operation case. */
	switch (region) {
	case INTEL_AX211_TRANSPORT_DMA_COMMAND_TFD:
		/* Returns the computed result. */
		return INTEL_AX211_COMMAND_TFD_RING_SIZE;
	case INTEL_AX211_TRANSPORT_DMA_COMMAND_BYTE_COUNT:
		/* Returns the computed result. */
		return INTEL_AX211_COMMAND_BC_TABLE_SIZE;
	case INTEL_AX211_TRANSPORT_DMA_COMMAND_SLOTS:
		/* Returns the computed result. */
		return INTEL_AX211_COMMAND_SLOTS_SIZE;
	case INTEL_AX211_TRANSPORT_DMA_COMMAND_EXTERNAL:
		/* Returns the computed result. */
		return INTEL_AX211_COMMAND_EXTERNAL_SIZE;
	case INTEL_AX211_TRANSPORT_DMA_RX_TRANSFER:
		/* Returns the computed result. */
		return INTEL_AX211_RX_TRANSFER_RING_SIZE;
	case INTEL_AX211_TRANSPORT_DMA_RX_COMPLETION:
		/* Returns the computed result. */
		return INTEL_AX211_RX_COMPLETION_RING_SIZE;
	case INTEL_AX211_TRANSPORT_DMA_RX_STATUS:
		/* Returns the computed result. */
		return INTEL_AX211_RX_STATUS_SIZE;
	default:
		/* Returns the computed result. */
		return 0U;
	}
}

/* Supports the ax211 backend direction valid operation. */
static int
ax211_backend_direction_valid(
	enum intel_ax211_transport_dma_region region,
	enum intel_ax211_transport_dma_direction direction)
{
	/* Handles the region condition. */
	if (region == INTEL_AX211_TRANSPORT_DMA_COMMAND_TFD ||
	    region == INTEL_AX211_TRANSPORT_DMA_COMMAND_BYTE_COUNT ||
	    region == INTEL_AX211_TRANSPORT_DMA_COMMAND_SLOTS ||
	    region == INTEL_AX211_TRANSPORT_DMA_COMMAND_EXTERNAL ||
	    region == INTEL_AX211_TRANSPORT_DMA_RX_TRANSFER) {
		/* Returns the computed result. */
		return direction == INTEL_AX211_TRANSPORT_DMA_PREWRITE;
	}

	/* Handles the region condition. */
	if (region == INTEL_AX211_TRANSPORT_DMA_RX_COMPLETION ||
	    region == INTEL_AX211_TRANSPORT_DMA_RX_STATUS) {
		/* Returns the computed result. */
		return direction == INTEL_AX211_TRANSPORT_DMA_PREREAD ||
		       direction == INTEL_AX211_TRANSPORT_DMA_POSTREAD;
	}

	/* Succeeded. */
	return 0;
}

/* Supports the ax211 backend range valid operation. */
static int
ax211_backend_range_valid(
	size_t offset,
	size_t length,
	size_t capacity)
{
	/* Returns the computed result. */
	return offset <= capacity && length <= capacity - offset;
}

/* Supports the ax211 backend delay us operation. */
static int
ax211_backend_delay_us(
	void *argument,
	uint32_t duration_us)
{
	struct intel_ax211_transport_backend *backend;

	/* Checks the operation status. */
	backend = argument;
	if (!ax211_backend_valid(backend) || backend->failed)
		return -1;

	/* Checks the delay us result. */
	if (backend->mmio->ops->delay_us(backend->mmio->argument,
					 duration_us) != 0) {
		ax211_backend_fail(backend);

		/* Reports operation failure. */
		return -1;
	}

	/* Succeeded. */
	return 0;
}

/* Adds an adapter-level monotonicity check to the shared clock source. */
static int
ax211_backend_clock_us(
	void *argument,
	uint64_t *time_us)
{
	struct intel_ax211_transport_backend *backend;
	uint64_t candidate;

	/* Checks the operation status. */
	backend = argument;
	if (time_us == NULL || !ax211_backend_valid(backend) || backend->failed)
		return -1;

	/* Checks the clock us result. */
	candidate = 0U;
	if (backend->mmio->ops->clock_us(backend->mmio->argument, &candidate) !=
		    0 ||
	    (backend->clock_observed && candidate < backend->last_clock_us)) {
		ax211_backend_fail(backend);

		/* Reports operation failure. */
		return -1;
	}

	backend->last_clock_us = candidate;
	backend->clock_observed = 1U;
	*time_us = candidate;
	/* Succeeded. */
	return 0;
}

/* Supports the ax211 backend trace deadline operation. */
static void
ax211_backend_trace_deadline(
	void *argument,
	enum intel_ax211_transport_wait wait,
	uint64_t start_us,
	uint64_t deadline_us)
{
	struct intel_ax211_transport_backend *backend;

	/* Checks the operation status. */
	backend = argument;
	if (!ax211_backend_valid(backend) || backend->failed ||
	    backend->mmio->ops->trace_deadline == NULL) {
		/* Returns the computed result. */
		return;
	}

	/* Handles the wait condition. */
	if (wait != INTEL_AX211_TRANSPORT_WAIT_RX_IDLE) {
		ax211_backend_fail(backend);

		/* Returns the computed result. */
		return;
	}

	/* RX quiescence is the transport-specific master-disable wait. */
	backend->mmio->ops->trace_deadline(
		backend->mmio->argument, INTEL_AX211_MMIO_WAIT_MASTER_DISABLED,
		start_us, deadline_us);
}
