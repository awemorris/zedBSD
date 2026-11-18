/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Intel AX211 transport backend adapter.
 */

#include "intel-ax211-transport-backend.h"
#include <kern/kcrt.h>

#include <stddef.h>
#include <stdint.h>
#include "kern/device-io.h"

/* Kept local so this C89-clean private boundary does not import HAL inlines. */
extern void kern_io_read_barrier(void);
extern void kern_io_write_barrier(void);

static int ax211_backend_csr_read32(void *argument, uint32_t offset, uint32_t *value);
static int ax211_backend_csr_write32(void *argument, uint32_t offset, uint32_t value);
static int ax211_backend_csr_write8(void *argument, uint32_t offset, uint8_t value);
static int ax211_backend_wake_hold(void *argument);
static int ax211_backend_wake_release(void *argument);
static int ax211_backend_prph_read32(void *argument, uint32_t address, uint32_t *value);
static int ax211_backend_prph_write32(void *argument, uint32_t address, uint32_t value);
static int ax211_backend_dma_sync(void *argument, enum intel_ax211_transport_dma_region region, size_t offset, size_t length, enum intel_ax211_transport_dma_direction direction);
static int ax211_backend_delay_us(void *argument, uint32_t duration_us);
static int ax211_backend_clock_us(void *argument, uint64_t *time_us);
static void ax211_backend_trace_deadline(void *argument, enum intel_ax211_transport_wait wait, uint64_t start_us, uint64_t deadline_us);

/*
 * The transport operations carried out on the device's BAR and DMA memory.
 *
 * The transport calls them with the backend as the argument; the table lives
 * as long as the driver.  The order is that of struct
 * intel_ax211_transport_ops.
 */
static const struct intel_ax211_transport_ops ax211_backend_operations = {
	ax211_backend_csr_read32,
	ax211_backend_csr_write32,
	ax211_backend_csr_write8,
	ax211_backend_wake_hold,
	ax211_backend_wake_release,
	ax211_backend_prph_read32,
	ax211_backend_prph_write32,
	ax211_backend_dma_sync,
	ax211_backend_delay_us,
	ax211_backend_clock_us,
	ax211_backend_trace_deadline};

static int ax211_backend_valid(const struct intel_ax211_transport_backend *backend);
static int ax211_backend_ready(const struct intel_ax211_transport_backend *backend);
static int ax211_backend_buffer_valid(const struct drv_dma_buffer *buffer, size_t exact_size, uint64_t alignment);
static void ax211_backend_fail(struct intel_ax211_transport_backend *backend);
static int ax211_backend_bar_byte_valid(const struct intel_ax211_pci_mmio_backend *pci_mmio, uint32_t offset);
static struct drv_dma_buffer *ax211_backend_region(struct intel_ax211_transport_backend *backend, enum intel_ax211_transport_dma_region region);
static size_t ax211_backend_region_size(enum intel_ax211_transport_dma_region region);
static int ax211_backend_direction_valid(enum intel_ax211_transport_dma_region region, enum intel_ax211_transport_dma_direction direction);
static int ax211_backend_range_valid(size_t offset, size_t length, size_t capacity);

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
	int valid;
	int ready;
	int coherent;

	/* Refuses missing objects. */
	if (backend == NULL || mmio == NULL || pci_mmio == NULL || dma == NULL)
		return INTEL_AX211_TRANSPORT_BACKEND_INVALID;

	/* Binds the objects into a candidate. */
	kern_memset(&candidate, 0, sizeof(candidate));
	candidate.mmio = mmio;
	candidate.pci_mmio = pci_mmio;
	candidate.dma = dma;
	candidate.initialized = 1U;

	/* Refuses objects that do not belong together. */
	valid = ax211_backend_valid(&candidate);
	if (!valid)
		return INTEL_AX211_TRANSPORT_BACKEND_INVALID;

	/* Refuses DMA memory that is not prepared for a boot. */
	ready = ax211_backend_ready(&candidate);
	if (!ready)
		return INTEL_AX211_TRANSPORT_BACKEND_NOT_READY;

	/* Refuses a device whose DMA is not coherent. */
	coherent = drv_dma_device_is_coherent(dma->device);
	if (!coherent)
		return INTEL_AX211_TRANSPORT_BACKEND_NOT_COHERENT;

	/* Succeeded: publishes the bound backend. */
	*backend = candidate;
	return INTEL_AX211_TRANSPORT_BACKEND_OK;
}

/*
 * Reports the transport operations of the backend.
 */
const struct intel_ax211_transport_ops *
drv_intel_ax211_transport_backend_ops(
	void)
{
	/* The table is the same for every device. */
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
	int valid;
	int ready;
	int coherent;

	/* Refuses a missing output. */
	if (memory == NULL)
		return INTEL_AX211_TRANSPORT_BACKEND_INVALID;

	/* Refuses an unbound backend. */
	valid = ax211_backend_valid(backend);
	if (!valid)
		return INTEL_AX211_TRANSPORT_BACKEND_INVALID;

	/* Refuses a failed backend. */
	if (backend->failed)
		return INTEL_AX211_TRANSPORT_BACKEND_NOT_READY;

	/* Refuses DMA memory that is not prepared for a boot. */
	ready = ax211_backend_ready(backend);
	if (!ready)
		return INTEL_AX211_TRANSPORT_BACKEND_NOT_READY;

	/* Refuses a device whose DMA is not coherent. */
	coherent = drv_dma_device_is_coherent(backend->dma->device);
	if (!coherent)
		return INTEL_AX211_TRANSPORT_BACKEND_NOT_COHERENT;

	/* Copies the address and size of every ring buffer into the view. */
	kern_memset(&candidate, 0, sizeof(candidate));
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

	/* Succeeded: publishes the complete view. */
	*memory = candidate;
	return INTEL_AX211_TRANSPORT_BACKEND_OK;
}

/*
 * Reports whether the backend's objects belong together: 1 when they do, 0 when
 * not.
 */
static int
ax211_backend_valid(
	const struct intel_ax211_transport_backend *backend)
{
	const struct intel_ax211_mmio_ops *ops;

	/* Rejects an unbound backend. */
	if (backend == NULL ||
	    !backend->initialized ||
	    backend->mmio == NULL ||
	    backend->pci_mmio == NULL ||
	    backend->dma == NULL) {
		return 0;
	}

	/* Rejects an MMIO layer that runs on another BAR backend. */
	ops = backend->mmio->ops;
	if (ops == NULL || backend->mmio->argument != backend->pci_mmio)
		return 0;

	/* Rejects an MMIO layer without the operations the transport calls. */
	if (ops->csr_read32 == NULL ||
	    ops->csr_write32 == NULL ||
	    ops->prph_read32 == NULL ||
	    ops->prph_write32 == NULL ||
	    ops->delay_us == NULL ||
	    ops->clock_us == NULL) {
		return 0;
	}

	/* Rejects an unmapped BAR. */
	if (backend->pci_mmio->registers == NULL ||
	    backend->pci_mmio->mapping_size == 0U) {
		return 0;
	}

	/* The backend is usable. */
	return 1;
}

/*
 * Reports whether all seven ring buffers are allocated as the rings need: 1
 * when they are, 0 when not.
 */
static int
ax211_backend_ready(
	const struct intel_ax211_transport_backend *backend)
{
	const struct intel_ax211_dma_resources *dma;
	int valid;

	/* Rejects an unbound backend. */
	valid = ax211_backend_valid(backend);
	if (!valid)
		return 0;

	/* Rejects DMA memory that is not prepared for a boot. */
	dma = backend->dma;
	if (dma->device == NULL || !dma->boot_prepared)
		return 0;

	/* Rejects a command TFD ring of another size or alignment. */
	valid = ax211_backend_buffer_valid(
		&dma->command_tfd, INTEL_AX211_COMMAND_TFD_RING_SIZE, 256U);
	if (!valid)
		return 0;

	/* Rejects a command byte-count table of another size or alignment. */
	valid = ax211_backend_buffer_valid(&dma->command_byte_count,
					   INTEL_AX211_COMMAND_LENGTH_TABLE_SIZE,
					   128U);
	if (!valid)
		return 0;

	/* Rejects command slots of another size or alignment. */
	valid = ax211_backend_buffer_valid(&dma->command_slots,
					   INTEL_AX211_COMMAND_SLOTS_SIZE, 64U);
	if (!valid)
		return 0;

	/* Rejects an external command buffer of another size or alignment. */
	valid = ax211_backend_buffer_valid(&dma->command_external,
					   INTEL_AX211_COMMAND_EXTERNAL_SIZE, 64U);
	if (!valid)
		return 0;

	/* Rejects a receive transfer ring of another size or alignment. */
	valid = ax211_backend_buffer_valid(
		&dma->rx_transfer, INTEL_AX211_RX_TRANSFER_RING_SIZE, 256U);
	if (!valid)
		return 0;

	/* Rejects a receive completion ring of another size or alignment. */
	valid = ax211_backend_buffer_valid(&dma->rx_completion,
					   INTEL_AX211_RX_COMPLETION_RING_SIZE,
					   256U);
	if (!valid)
		return 0;

	/* Rejects a receive status word of another size or alignment. */
	valid = ax211_backend_buffer_valid(
		&dma->rx_status, INTEL_AX211_RX_STATUS_SIZE, 16U);
	if (!valid)
		return 0;

	/* Every ring buffer is usable. */
	return 1;
}

/*
 * Reports whether a DMA buffer has the exact size and alignment: 1 when it has,
 * 0 when not.
 */
static int
ax211_backend_buffer_valid(
	const struct drv_dma_buffer *buffer,
	size_t exact_size,
	uint64_t alignment)
{
	/*
	 * Rejects a missing buffer, another size, an unset address or a
	 * misaligned one.
	 */
	if (buffer == NULL ||
	    buffer->address == NULL ||
	    buffer->size != exact_size ||
	    buffer->device_address == 0U ||
	    alignment == 0U ||
	    (buffer->device_address & (alignment - 1U)) != 0U) {
		return 0;
	}

	/*
	 * Rejects a buffer whose end would pass the top of the address space.
	 */
	if (buffer->device_address > UINT64_MAX - (uint64_t)(exact_size - 1U))
		return 0;

	/* The buffer is usable. */
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
	int valid;
	int error;

	/* Refuses a missing output. */
	backend = argument;
	if (value == NULL)
		return -1;

	/* Refuses an unbound backend. */
	valid = ax211_backend_valid(backend);
	if (!valid)
		return -1;

	/* Refuses a failed backend. */
	if (backend->failed)
		return -1;

	/* Reads the register through the MMIO layer. */
	candidate = 0U;
	error = backend->mmio->ops->csr_read32(backend->mmio->argument, offset,
					       &candidate);

	/*
	 * A failed read, or all ones, means the device is gone: the backend
	 * fails.
	 */
	if (error != 0 || candidate == UINT32_MAX) {
		ax211_backend_fail(backend);
		return -1;
	}

	/* Reports the register. */
	*value = candidate;

	/* Succeeded. */
	return 0;
}

/* Marks the backend as failed so that it refuses further access. */
static void
ax211_backend_fail(
	struct intel_ax211_transport_backend *backend)
{
	/* Nothing to mark without a backend. */
	if (backend != NULL)
		backend->failed = 1U;
}

/* Writes a 32-bit register through the MMIO layer. */
static int
ax211_backend_csr_write32(
	void *argument,
	uint32_t offset,
	uint32_t value)
{
	struct intel_ax211_transport_backend *backend;
	int valid;
	int error;

	/* Refuses an unbound backend. */
	backend = argument;
	valid = ax211_backend_valid(backend);
	if (!valid)
		return -1;

	/* Refuses a failed backend. */
	if (backend->failed)
		return -1;

	/* Writes the register; a failed write fails the backend. */
	error = backend->mmio->ops->csr_write32(backend->mmio->argument, offset,
						value);
	if (error != 0) {
		ax211_backend_fail(backend);
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
	int valid;

	/* Refuses an unbound backend, and fails it if there is one. */
	backend = argument;
	valid = ax211_backend_valid(backend);
	if (!valid) {
		/* A backend that exists is failed so that it stays refused. */
		if (backend != NULL)
			ax211_backend_fail(backend);
		return -1;
	}

	/* Refuses a failed backend. */
	if (backend->failed) {
		ax211_backend_fail(backend);
		return -1;
	}

	/* Refuses a byte outside the mapping, and fails the backend. */
	valid = ax211_backend_bar_byte_valid(backend->pci_mmio, offset);
	if (!valid) {
		ax211_backend_fail(backend);
		return -1;
	}

	/* Writes the byte, ordered after earlier writes. */
	*(volatile uint8_t *)(backend->pci_mmio->registers + offset) = value;
	kern_io_write_barrier();

	/* Succeeded. */
	return 0;
}

/*
 * Reports whether a byte register lies inside the mapping: 1 when it does, 0
 * when not.
 */
static int
ax211_backend_bar_byte_valid(
	const struct intel_ax211_pci_mmio_backend *pci_mmio,
	uint32_t offset)
{
	/* Rejects an unmapped BAR. */
	if (pci_mmio == NULL || pci_mmio->registers == NULL)
		return 0;

	/* Rejects an offset past the end of the mapping. */
	if ((size_t)offset >= pci_mmio->mapping_size)
		return 0;

	/* The byte lies inside the mapping. */
	return 1;
}

/* Holds the device awake for register access. */
static int
ax211_backend_wake_hold(
	void *argument)
{
	struct intel_ax211_transport_backend *backend;
	int valid;
	int result;

	/* Refuses an unbound backend. */
	backend = argument;
	valid = ax211_backend_valid(backend);
	if (!valid)
		return -1;

	/* Refuses a failed backend. */
	if (backend->failed)
		return -1;

	/* Takes the hold through the MMIO layer. */
	result = drv_intel_ax211_mmio_wake_hold(backend->mmio);
	if (result != INTEL_AX211_MMIO_OK)
		return -1;

	/* Succeeded: the device stays awake until the hold is released. */
	return 0;
}

/*
 * Releases a wake hold; it works after a failure too, so that a hold cannot
 * leak.
 */
static int
ax211_backend_wake_release(
	void *argument)
{
	struct intel_ax211_transport_backend *backend;
	int valid;
	int result;

	/* Refuses an unbound backend. */
	backend = argument;
	valid = ax211_backend_valid(backend);
	if (!valid)
		return -1;

	/* Releases the hold through the MMIO layer. */
	result = drv_intel_ax211_mmio_wake_release(backend->mmio);
	if (result != INTEL_AX211_MMIO_OK)
		return -1;

	/* Succeeded. */
	return 0;
}

/* Reads a periphery register through the MMIO layer. */
static int
ax211_backend_prph_read32(
	void *argument,
	uint32_t address,
	uint32_t *value)
{
	struct intel_ax211_transport_backend *backend;
	int valid;
	int result;

	/* Refuses a missing output. */
	backend = argument;
	if (value == NULL)
		return -1;

	/* Refuses an unbound backend. */
	valid = ax211_backend_valid(backend);
	if (!valid)
		return -1;

	/* Refuses a failed backend. */
	if (backend->failed)
		return -1;

	/* Reads the register; a failed read fails the backend. */
	result = drv_intel_ax211_mmio_prph_read32(backend->mmio, address, value);
	if (result != INTEL_AX211_MMIO_OK) {
		ax211_backend_fail(backend);
		return -1;
	}

	/* Succeeded. */
	return 0;
}

/* Writes a periphery register through the MMIO layer. */
static int
ax211_backend_prph_write32(
	void *argument,
	uint32_t address,
	uint32_t value)
{
	struct intel_ax211_transport_backend *backend;
	int valid;
	int result;

	/* Refuses an unbound backend. */
	backend = argument;
	valid = ax211_backend_valid(backend);
	if (!valid)
		return -1;

	/* Refuses a failed backend. */
	if (backend->failed)
		return -1;

	/* Writes the register; a failed write fails the backend. */
	result = drv_intel_ax211_mmio_prph_write32(backend->mmio, address, value);
	if (result != INTEL_AX211_MMIO_OK) {
		ax211_backend_fail(backend);
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
	int valid;

	/* Refuses an unbound backend. */
	backend = argument;
	valid = ax211_backend_valid(backend);
	if (!valid)
		return -1;

	/* Refuses a failed backend. */
	if (backend->failed)
		return -1;

	/* Refuses DMA memory that is not prepared for a boot. */
	valid = ax211_backend_ready(backend);
	if (!valid)
		return -1;

	/* Refuses a device whose DMA is not coherent. */
	valid = drv_dma_device_is_coherent(backend->dma->device);
	if (!valid)
		return -1;

	/* Refuses a region that is not allocated as the ring needs. */
	buffer = ax211_backend_region(backend, region);
	exact_size = ax211_backend_region_size(region);
	valid = ax211_backend_buffer_valid(buffer, exact_size, 1U);
	if (!valid)
		return -1;

	/* Refuses a direction the region never moves in. */
	valid = ax211_backend_direction_valid(region, direction);
	if (!valid)
		return -1;

	/* Refuses an empty range. */
	if (length == 0U)
		return -1;

	/* Refuses a range outside the buffer. */
	valid = ax211_backend_range_valid(offset, length, buffer->size);
	if (!valid)
		return -1;

	/*
	 * Applies the fence of the direction: memory is coherent, so ordering is all
	 * a hand-over needs.
	 */
	if (direction == INTEL_AX211_TRANSPORT_DMA_PREWRITE) {
		kern_io_write_barrier();
	} else if (direction == INTEL_AX211_TRANSPORT_DMA_PREREAD) {
		kern_io_write_barrier();
	} else if (direction == INTEL_AX211_TRANSPORT_DMA_POSTREAD) {
		kern_io_read_barrier();
	} else {
		return -1;
	}

	/* Succeeded. */
	return 0;
}

/* Finds the DMA buffer of a region. */
static struct drv_dma_buffer *
ax211_backend_region(
	struct intel_ax211_transport_backend *backend,
	enum intel_ax211_transport_dma_region region)
{
	/* Maps each region to its buffer; an unknown region has none. */
	switch (region) {
	case INTEL_AX211_TRANSPORT_DMA_COMMAND_TFD:
		return &backend->dma->command_tfd;
	case INTEL_AX211_TRANSPORT_DMA_COMMAND_BYTE_COUNT:
		return &backend->dma->command_byte_count;
	case INTEL_AX211_TRANSPORT_DMA_COMMAND_SLOTS:
		return &backend->dma->command_slots;
	case INTEL_AX211_TRANSPORT_DMA_COMMAND_EXTERNAL:
		return &backend->dma->command_external;
	case INTEL_AX211_TRANSPORT_DMA_RX_TRANSFER:
		return &backend->dma->rx_transfer;
	case INTEL_AX211_TRANSPORT_DMA_RX_COMPLETION:
		return &backend->dma->rx_completion;
	case INTEL_AX211_TRANSPORT_DMA_RX_STATUS:
		return &backend->dma->rx_status;
	default:
		return NULL;
	}
}

/* Reports the exact size of a region's buffer. */
static size_t
ax211_backend_region_size(
	enum intel_ax211_transport_dma_region region)
{
	/* Maps each region to its size; an unknown region has size zero. */
	switch (region) {
	case INTEL_AX211_TRANSPORT_DMA_COMMAND_TFD:
		return INTEL_AX211_COMMAND_TFD_RING_SIZE;
	case INTEL_AX211_TRANSPORT_DMA_COMMAND_BYTE_COUNT:
		return INTEL_AX211_COMMAND_LENGTH_TABLE_SIZE;
	case INTEL_AX211_TRANSPORT_DMA_COMMAND_SLOTS:
		return INTEL_AX211_COMMAND_SLOTS_SIZE;
	case INTEL_AX211_TRANSPORT_DMA_COMMAND_EXTERNAL:
		return INTEL_AX211_COMMAND_EXTERNAL_SIZE;
	case INTEL_AX211_TRANSPORT_DMA_RX_TRANSFER:
		return INTEL_AX211_RX_TRANSFER_RING_SIZE;
	case INTEL_AX211_TRANSPORT_DMA_RX_COMPLETION:
		return INTEL_AX211_RX_COMPLETION_RING_SIZE;
	case INTEL_AX211_TRANSPORT_DMA_RX_STATUS:
		return INTEL_AX211_RX_STATUS_SIZE;
	default:
		return 0U;
	}
}

/*
 * Reports whether the transport may move a region in a direction: 1 when it
 * may, 0 when not.
 */
static int
ax211_backend_direction_valid(
	enum intel_ax211_transport_dma_region region,
	enum intel_ax211_transport_dma_direction direction)
{
	/*
	 * The command rings and the receive transfer ring are only written by
	 * the host.
	 */
	if (region == INTEL_AX211_TRANSPORT_DMA_COMMAND_TFD ||
	    region == INTEL_AX211_TRANSPORT_DMA_COMMAND_BYTE_COUNT ||
	    region == INTEL_AX211_TRANSPORT_DMA_COMMAND_SLOTS ||
	    region == INTEL_AX211_TRANSPORT_DMA_COMMAND_EXTERNAL ||
	    region == INTEL_AX211_TRANSPORT_DMA_RX_TRANSFER) {
		/* They are handed to the device after the host wrote them. */
		if (direction == INTEL_AX211_TRANSPORT_DMA_PREWRITE)
			return 1;

		/* No other direction applies to them. */
		return 0;
	}

	/*
	 * The receive completion ring and the status word are written by the
	 * device.
	 */
	if (region == INTEL_AX211_TRANSPORT_DMA_RX_COMPLETION ||
	    region == INTEL_AX211_TRANSPORT_DMA_RX_STATUS) {
		/* They are handed to the device before it writes them. */
		if (direction == INTEL_AX211_TRANSPORT_DMA_PREREAD)
			return 1;

		/* They are taken back by the host after the device wrote them. */
		if (direction == INTEL_AX211_TRANSPORT_DMA_POSTREAD)
			return 1;

		/* No other direction applies to them. */
		return 0;
	}

	/* An unknown region moves in no direction. */
	return 0;
}

/* Reports whether a range lies inside a buffer: 1 when it does, 0 when not. */
static int
ax211_backend_range_valid(
	size_t offset,
	size_t length,
	size_t capacity)
{
	/* Rejects an offset past the end. */
	if (offset > capacity)
		return 0;

	/* Rejects a length that runs past the end. */
	if (length > capacity - offset)
		return 0;

	/* The range lies inside the buffer. */
	return 1;
}

/* Waits through the MMIO layer. */
static int
ax211_backend_delay_us(
	void *argument,
	uint32_t duration_us)
{
	struct intel_ax211_transport_backend *backend;
	int valid;
	int error;

	/* Refuses an unbound backend. */
	backend = argument;
	valid = ax211_backend_valid(backend);
	if (!valid)
		return -1;

	/* Refuses a failed backend. */
	if (backend->failed)
		return -1;

	/* Waits; a failed wait fails the backend. */
	error = backend->mmio->ops->delay_us(backend->mmio->argument,
					     duration_us);
	if (error != 0) {
		ax211_backend_fail(backend);
		return -1;
	}

	/* Succeeded. */
	return 0;
}

/* Reads the clock through the MMIO layer and refuses a clock that goes back. */
static int
ax211_backend_clock_us(
	void *argument,
	uint64_t *time_us)
{
	struct intel_ax211_transport_backend *backend;
	uint64_t candidate;
	int valid;
	int error;

	/* Refuses a missing output. */
	backend = argument;
	if (time_us == NULL)
		return -1;

	/* Refuses an unbound backend. */
	valid = ax211_backend_valid(backend);
	if (!valid)
		return -1;

	/* Refuses a failed backend. */
	if (backend->failed)
		return -1;

	/* Reads the clock. */
	candidate = 0U;
	error = backend->mmio->ops->clock_us(backend->mmio->argument, &candidate);

	/* A failed read, or a time behind the last one, fails the backend. */
	if (error != 0 ||
	    (backend->clock_observed && candidate < backend->last_clock_us)) {
		ax211_backend_fail(backend);
		return -1;
	}

	/* Remembers the time as the new lower bound and reports it. */
	backend->last_clock_us = candidate;
	backend->clock_observed = 1U;
	*time_us = candidate;

	/* Succeeded. */
	return 0;
}

/* Passes the deadline of the receive-idle wait to the MMIO layer's trace. */
static void
ax211_backend_trace_deadline(
	void *argument,
	enum intel_ax211_transport_wait wait,
	uint64_t start_us,
	uint64_t deadline_us)
{
	struct intel_ax211_transport_backend *backend;
	int valid;

	/* Nothing to trace without a bound backend. */
	backend = argument;
	valid = ax211_backend_valid(backend);
	if (!valid)
		return;

	/* Nothing to trace on a failed backend. */
	if (backend->failed)
		return;

	/* Nothing to trace without a trace hook. */
	if (backend->mmio->ops->trace_deadline == NULL)
		return;

	/*
	 * The transport traces only the receive-idle wait; any other is a
	 * fault.
	 */
	if (wait != INTEL_AX211_TRANSPORT_WAIT_RX_IDLE) {
		ax211_backend_fail(backend);
		return;
	}

	/* RX quiescence is the transport-specific master-disable wait. */
	backend->mmio->ops->trace_deadline(
		backend->mmio->argument, INTEL_AX211_MMIO_WAIT_MASTER_DISABLED,
		start_us, deadline_us);
}
