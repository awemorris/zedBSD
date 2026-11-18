/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Intel AX211 private Gen3 transport contract.
 */

#ifndef KERN_DRIVERS_INTEL_AX211_TRANSPORT_H
#define KERN_DRIVERS_INTEL_AX211_TRANSPORT_H

#include "intel-ax211-internal.h"
#include "intel-ax211-mmio.h"

#include <stddef.h>
#include <stdint.h>

/*
 * The command slots and receive descriptors shared with the boot code.
 *
 * A command slot of 324 bytes holds the 8-byte wide header and up to 316
 * payload bytes inline; a longer command uses the one 4 KiB external buffer,
 * which holds the header and up to 4088 payload bytes.  The receive ring has
 * 512 descriptors.  The slot sizes are zedBSD's.
 *
 * Values referenced: Linux v6.17 drivers/net/wireless/intel/iwlwifi/iwl-fh.h
 * (the 16-byte receive transfer descriptor and 32-byte completion
 * descriptor).
 */
#define INTEL_AX211_TRANSPORT_COMMAND_SLOT_SIZE 324U
#define INTEL_AX211_TRANSPORT_COMMAND_INLINE_PAYLOAD_MAX 316U
#define INTEL_AX211_TRANSPORT_COMMAND_EXTERNAL_SIZE 4096U
#define INTEL_AX211_TRANSPORT_COMMAND_EXTERNAL_PAYLOAD_MAX 4088U
#define INTEL_AX211_TRANSPORT_RX_DESCRIPTOR_COUNT 512U
#define INTEL_AX211_TRANSPORT_RX_DESCRIPTOR_SIZE 16U
#define INTEL_AX211_TRANSPORT_RX_COMPLETION_SIZE 32U

/*
 * The MSI-X causes that the boot and runtime code test for themselves.
 *
 * ALIVE is bit 0 of the event-cause register; the firmware faults are bits 5
 * and 25 and the hardware fault bit 29 of the same register; the DMA engine
 * fault is bit 21 of the queue-cause register.  A command that fails while one
 * of the fatal causes is pending makes the driver read the firmware error
 * tables (intel-ax211.c).
 *
 * Values referenced: Linux v6.17 drivers/net/wireless/intel/iwlwifi/iwl-csr.h
 * (enum msix_hw_int_causes and enum msix_fh_int_causes).
 */
#define INTEL_AX211_TRANSPORT_EVENT_CAUSE_ALIVE 0x00000001U
#define INTEL_AX211_TRANSPORT_QUEUE_CAUSE_FAULT 0x00200000U
#define INTEL_AX211_TRANSPORT_EVENT_CAUSE_FIRMWARE_FAULT_V2 0x00000020U
#define INTEL_AX211_TRANSPORT_EVENT_CAUSE_FIRMWARE_FAULT 0x02000000U
#define INTEL_AX211_TRANSPORT_EVENT_CAUSE_HARDWARE_FAULT 0x20000000U
#define INTEL_AX211_TRANSPORT_EVENT_FATAL_CAUSES                               \
	(INTEL_AX211_TRANSPORT_EVENT_CAUSE_FIRMWARE_FAULT_V2 |                 \
	 INTEL_AX211_TRANSPORT_EVENT_CAUSE_FIRMWARE_FAULT |                    \
	 INTEL_AX211_TRANSPORT_EVENT_CAUSE_HARDWARE_FAULT)

/* The results of the transport. */
enum intel_ax211_transport_result {
	INTEL_AX211_TRANSPORT_OK = 0,
	INTEL_AX211_TRANSPORT_INVALID = 1,
	INTEL_AX211_TRANSPORT_IO = 2,
	INTEL_AX211_TRANSPORT_ORDER = 3,
	INTEL_AX211_TRANSPORT_FULL = 4,
	INTEL_AX211_TRANSPORT_STALE = 5,
	INTEL_AX211_TRANSPORT_TIMEOUT = 6,
	INTEL_AX211_TRANSPORT_CLOCK = 7,
	INTEL_AX211_TRANSPORT_FAILED = 8,
	INTEL_AX211_TRANSPORT_AMBIGUOUS = 9
};

/* The DMA objects the transport asks the controller to synchronize. */
enum intel_ax211_transport_dma_region {
	INTEL_AX211_TRANSPORT_DMA_COMMAND_TFD = 1,
	INTEL_AX211_TRANSPORT_DMA_COMMAND_BYTE_COUNT = 2,
	INTEL_AX211_TRANSPORT_DMA_COMMAND_SLOTS = 3,
	INTEL_AX211_TRANSPORT_DMA_RX_TRANSFER = 4,
	INTEL_AX211_TRANSPORT_DMA_RX_COMPLETION = 5,
	INTEL_AX211_TRANSPORT_DMA_RX_STATUS = 6,
	INTEL_AX211_TRANSPORT_DMA_COMMAND_EXTERNAL = 7
};

/*
 * The direction of a synchronization: before the device reads, before it
 * writes, and after it wrote.
 */
enum intel_ax211_transport_dma_direction {
	INTEL_AX211_TRANSPORT_DMA_PREWRITE = 1,
	INTEL_AX211_TRANSPORT_DMA_PREREAD = 2,
	INTEL_AX211_TRANSPORT_DMA_POSTREAD = 3
};

/* The bounded waits the transport reports to a tracer. */
enum intel_ax211_transport_wait {
	INTEL_AX211_TRANSPORT_WAIT_RX_IDLE = 1
};

/*
 * What the transport needs from the PCI controller: register and periphery
 * access, NIC ownership, DMA synchronization, time, and an optional tracer.
 */
struct intel_ax211_transport_ops {
	int (*csr_read32)(void *argument, uint32_t offset, uint32_t *value);
	int (*csr_write32)(void *argument, uint32_t offset, uint32_t value);
	int (*csr_write8)(void *argument, uint32_t offset, uint8_t value);
	int (*wake_hold)(void *argument);
	int (*wake_release)(void *argument);
	int (*prph_read32)(void *argument, uint32_t address, uint32_t *value);
	int (*prph_write32)(void *argument, uint32_t address, uint32_t value);
	int (*dma_sync)(void *argument,
			enum intel_ax211_transport_dma_region region,
			size_t offset, size_t length,
			enum intel_ax211_transport_dma_direction direction);
	int (*delay_us)(void *argument, uint32_t duration_us);
	int (*clock_us)(void *argument, uint64_t *time_us);
	void (*trace_deadline)(void *argument,
			       enum intel_ax211_transport_wait wait,
			       uint64_t start_us, uint64_t deadline_us);
};

/*
 * The controller-allocated ring memory: each object's host address and size,
 * and the device addresses of the command slots and the external buffer.
 */
struct intel_ax211_transport_ring_memory {
	uint8_t *command_tfd;
	size_t command_tfd_size;
	uint8_t *command_byte_count;
	size_t command_byte_count_size;
	uint8_t *command_slots;
	size_t command_slots_size;
	uint64_t command_slots_device_address;
	uint8_t *command_external;
	size_t command_external_size;
	uint64_t command_external_device_address;
	uint8_t *rx_transfer;
	size_t rx_transfer_size;
	uint8_t *rx_completion;
	size_t rx_completion_size;
	uint8_t *rx_status;
	size_t rx_status_size;
};

/*
 * One claimed interrupt: the admitted queue and event causes, and the raw
 * causes that were acknowledged.
 */
struct intel_ax211_transport_causes {
	uint32_t queue_causes;
	uint32_t hardware;
	uint32_t raw_queue_causes;
	uint32_t raw_hardware;
};

/*
 * One received completion: its ring index, the buffer it filled, and its
 * flags.
 */
struct intel_ax211_transport_rx_completion {
	uint16_t completion_index;
	uint16_t buffer_id;
	uint8_t flags;
};

/*
 * One firmware epoch's transport: the command ring, the receive rings, the
 * MSI-X routing, and the ownership state of each.
 */
struct intel_ax211_transport {
	const struct intel_ax211_transport_ops *ops;
	void *argument;
	struct intel_ax211_mmio_profile profile;
	struct intel_ax211_transport_ring_memory memory;
	struct intel_ax211_ring command_ring;
	struct intel_ax211_ring_token command_prepared_token;
	struct intel_ax211_ring_token command_external_token;
	uint32_t enabled_queue_causes;
	uint32_t enabled_event_causes;

	/* Counts published commands modulo 65536; reset starts at zero. */
	uint16_t command_write_sequence;
	uint16_t rx_head;
	uint16_t rx_tail;
	uint16_t rx_pending_buffer;
	uint16_t rx_pending_index;
	uint16_t rx_last_credit;
	uint8_t rx_published[INTEL_AX211_TRANSPORT_RX_DESCRIPTOR_COUNT / 8U];
	uint8_t msix_configured;
	uint8_t rings_initialized;
	uint8_t interrupts_enabled;
	uint8_t firmware_load_mode;
	uint8_t command_prepared;
	uint8_t command_external_active;
	uint8_t command_reset_required;
	uint8_t command_reset_completed;
	uint8_t rx_active;
	uint8_t rx_pending;
	uint8_t quiesced;
	uint8_t failed;
	uint8_t rx_dma_idle;
};

int drv_intel_ax211_transport_init(
	struct intel_ax211_transport *transport,
	const struct intel_ax211_transport_ops *ops, void *argument,
	const struct intel_ax211_mmio_profile *profile,
	const struct intel_ax211_transport_ring_memory *memory);
int drv_intel_ax211_transport_configure_msix(
	struct intel_ax211_transport *transport);
int drv_intel_ax211_transport_initialize_rings(
	struct intel_ax211_transport *transport);
int drv_intel_ax211_transport_enable_firmware_interrupts(
	struct intel_ax211_transport *transport);
int drv_intel_ax211_transport_enable_runtime_interrupts(
	struct intel_ax211_transport *transport);
int drv_intel_ax211_transport_disable_interrupts(
	struct intel_ax211_transport *transport);
int drv_intel_ax211_transport_interrupt_claim(
	struct intel_ax211_transport *transport,
	struct intel_ax211_transport_causes *causes);
int drv_intel_ax211_transport_interrupt_rearm(
	struct intel_ax211_transport *transport);
int drv_intel_ax211_transport_publish_rx_descriptor(
	struct intel_ax211_transport *transport, uint16_t index,
	uint64_t device_address);
/*
 * Call only after the Gen3 hardware-ALIVE cause says firmware configured RFH.
 */
int drv_intel_ax211_transport_activate_rx(
	struct intel_ax211_transport *transport);
int drv_intel_ax211_transport_rx_refresh(
	struct intel_ax211_transport *transport);
int drv_intel_ax211_transport_rx_next(
	struct intel_ax211_transport *transport,
	struct intel_ax211_transport_rx_completion *completion);
int drv_intel_ax211_transport_rx_replenish(
	struct intel_ax211_transport *transport, uint64_t device_address);
int drv_intel_ax211_transport_command_prepare_inline(
	struct intel_ax211_transport *transport,
	const struct intel_ax211_command_id *command, const void *payload,
	size_t payload_length, struct intel_ax211_ring_token *token);
int drv_intel_ax211_transport_command_prepare_external(
	struct intel_ax211_transport *transport,
	const struct intel_ax211_command_id *command, const void *payload,
	size_t payload_length, struct intel_ax211_ring_token *token);
int drv_intel_ax211_transport_command_publish(
	struct intel_ax211_transport *transport,
	const struct intel_ax211_ring_token *token);
int drv_intel_ax211_transport_command_abort_prepared(
	struct intel_ax211_transport *transport,
	const struct intel_ax211_ring_token *token);
int drv_intel_ax211_transport_command_submit_inline(
	struct intel_ax211_transport *transport,
	const struct intel_ax211_command_id *command, const void *payload,
	size_t payload_length, struct intel_ax211_ring_token *token);
int drv_intel_ax211_transport_command_complete(
	struct intel_ax211_transport *transport,
	const struct intel_ax211_ring_token *token);
size_t drv_intel_ax211_transport_command_pending_count(
	const struct intel_ax211_transport *transport);
int drv_intel_ax211_transport_command_oldest(
	const struct intel_ax211_transport *transport,
	struct intel_ax211_ring_token *token);
int drv_intel_ax211_transport_command_after_device_reset(
	struct intel_ax211_transport *transport);
int drv_intel_ax211_transport_quiesce(struct intel_ax211_transport *transport);

#endif
