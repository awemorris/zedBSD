/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Intel AX211 private Gen3 data transmit ring.
 */

#ifndef KERN_DRIVERS_INTEL_AX211_TX_RING_H
#define KERN_DRIVERS_INTEL_AX211_TX_RING_H

#include <drivers/generic/dma.h>

#include <stddef.h>
#include <stdint.h>

#include "intel-ax211-protocol.h"
#include "intel-ax211-tx.h"

/*
 * The data transmit ring of the management queue.
 *
 * The ring carries the frames of TID 15 (management frames) on the queue the
 * firmware assigned in the queue setup answer.  It has 256 descriptors of
 * 256 bytes (64 KiB) and keeps at most 255 frames outstanding, so a full
 * ring is never confused with an empty one.  Each frame uses three transfer
 * buffers: the first 20 bytes of its command, which the device copies before
 * it reads the rest; the rest of the command (fixed part and MAC header) in
 * a 128-byte slot; and the frame body in a 4092-byte slot.  The byte-count
 * table entry of the descriptor gets the frame length the firmware will
 * send.  After filling a descriptor the driver writes the queue number
 * (bits 23:16) and the new write index to the doorbell.
 *
 * Values referenced: Linux v6.17 drivers/net/wireless/intel/iwlwifi/iwl-fh.h
 * (the 256-byte TFD, its 10-byte transfer buffer entries, the byte-count table
 * of 1024 16-bit entries, the 4092-byte buffer limit and the limit of 255 TFDs
 * waiting on a queue, in the description of the TFD),
 * drivers/net/wireless/intel/iwlwifi/iwl-csr.h (the transmit write-index
 * doorbell at 0x460) and drivers/net/wireless/intel/iwlwifi/fw/api/tx.h, and
 * OpenBSD 0f464d413c50 sys/dev/pci/if_iwxreg.h (the same limit of 255 TFDs,
 * above struct iwx_tfh_tfd).
 */
#define INTEL_AX211_TX_RING_MANAGEMENT_TID 15U
#define INTEL_AX211_TX_RING_DESCRIPTOR_SLOTS 256U
#define INTEL_AX211_TX_RING_INFLIGHT_LIMIT 255U
#define INTEL_AX211_TX_RING_TFD_SIZE 256U
#define INTEL_AX211_TX_RING_TFD_RING_SIZE 65536U
#define INTEL_AX211_TX_RING_BYTE_COUNT_ENTRIES 1024U
#define INTEL_AX211_TX_RING_BYTE_COUNT_SIZE 2048U
#define INTEL_AX211_TX_RING_COMMAND_DMA_SIZE 128U
#define INTEL_AX211_TX_RING_PAYLOAD_DMA_SIZE 4092U
#define INTEL_AX211_TX_RING_HEADER_COPY_SIZE 20U
#define INTEL_AX211_TX_RING_TB_SIZE_MAX 4092U

/* The transmit write-index doorbell named above. */
#define INTEL_AX211_TX_RING_WRITE_POINTER_REGISTER 0x460U

/*
 * The queue setup command as the ring sends it.
 *
 * The command is opcode 0x17 of family 5, and the pinned API 89 image lists
 * command version 3 and answer version 2.  The layout is described in
 * intel-ax211-assoc.h.
 *
 * Values referenced: Linux v6.17
 * drivers/net/wireless/intel/iwlwifi/fw/api/datapath.h
 * (SCD_QUEUE_CONFIG_CMD).
 */
#define INTEL_AX211_TX_QUEUE_SETUP_FAMILY 5U
#define INTEL_AX211_TX_QUEUE_SETUP_OPCODE 0x17U
#define INTEL_AX211_TX_QUEUE_SETUP_COMMAND_VERSION 3U
#define INTEL_AX211_TX_QUEUE_SETUP_RESPONSE_VERSION 2U
#define INTEL_AX211_TX_QUEUE_SETUP_COMMAND_SIZE 36U
#define INTEL_AX211_TX_QUEUE_SETUP_RESPONSE_SIZE 8U

/* The results of the data transmit ring. */
enum intel_ax211_tx_ring_result {
	INTEL_AX211_TX_RING_OK = 0,
	INTEL_AX211_TX_RING_INVALID = 1,
	INTEL_AX211_TX_RING_UNSUPPORTED = 2,
	INTEL_AX211_TX_RING_NOT_READY = 3,
	INTEL_AX211_TX_RING_NO_MEMORY = 4,
	INTEL_AX211_TX_RING_IO_ERROR = 5,
	INTEL_AX211_TX_RING_FULL = 6,
	INTEL_AX211_TX_RING_STALE = 7,
	INTEL_AX211_TX_RING_DUPLICATE = 8,
	INTEL_AX211_TX_RING_OUT_OF_ORDER = 9,
	INTEL_AX211_TX_RING_PENDING = 10,
	INTEL_AX211_TX_RING_TIMEOUT = 11,
	INTEL_AX211_TX_RING_TX_FAILED = 12,
	INTEL_AX211_TX_RING_POISONED = 13,
	INTEL_AX211_TX_RING_BARRIER_REQUIRED = 14,
	INTEL_AX211_TX_RING_KICK_FAILED = 15,
	INTEL_AX211_TX_RING_MALFORMED = 16
};

/*
 * What the ring needs from its owner: handing memory to the device and
 * writing the doorbell.
 *
 * All callbacks run while the owning controller serializes the TX ring.
 * sync_for_device must establish coherent-DMA visibility before returning.
 */
struct intel_ax211_tx_ring_ops {
	int (*sync_for_device)(void *argument,
			       const struct drv_dma_buffer *buffer,
			       size_t offset, size_t length);
	int (*write32)(void *argument, uint32_t offset, uint32_t value);
};

/*
 * The queue setup command built for one ring, with the fields it was built
 * from so that its answer can be checked against them.
 */
struct intel_ax211_tx_queue_config {
	uint8_t command[INTEL_AX211_TX_QUEUE_SETUP_COMMAND_SIZE];
	uint64_t tfd_address;
	uint64_t byte_count_address;
	uint8_t station_id;
	uint8_t tid;
};

/* What a caller holds for one frame in flight. */
struct intel_ax211_tx_ring_handle {
	uint64_t connection_generation;
	uint64_t cookie;
	uint64_t key_generation;
	uint64_t packet_number;
	uint64_t deadline;
	uint32_t hardware_generation;
	uint16_t scheduler_sequence;
	uint8_t index;
	uint8_t reserved;
};

/* The outcome of one retired frame. */
struct intel_ax211_tx_ring_retired {
	struct intel_ax211_tx_ring_handle handle;
	uint16_t sequence_control;
	uint16_t byte_count;
	uint8_t acknowledged;
	uint8_t rts_retries;
	uint8_t frame_retries;
	uint8_t reserved;
};

/*
 * One slot of the ring: its command and payload buffers and the frame in
 * flight in it.
 *
 * An uncertain slot may still be owned by the device; it stays until reset.
 */
struct intel_ax211_tx_ring_slot {
	struct drv_dma_buffer command;
	struct drv_dma_buffer payload;
	struct intel_ax211_tx_ring_handle handle;
	uint16_t frame_length;
	uint8_t active;
	uint8_t uncertain;
};

/*
 * One data transmit ring of the device.
 *
 * Private controller-owned object; do not copy after allocation.
 */
struct intel_ax211_frame_ring {
	struct drv_dma_device *dma_device;
	const struct intel_ax211_tx_ring_ops *ops;
	void *ops_argument;
	struct drv_dma_buffer tfd;
	struct drv_dma_buffer byte_count;
	struct intel_ax211_tx_ring_slot slot[INTEL_AX211_TX_RING_DESCRIPTOR_SLOTS];
	uint64_t connection_generation;
	uint32_t hardware_generation;
	uint16_t read_sequence;
	uint16_t write_sequence;
	uint16_t last_completion_sequence;
	uint16_t pending_count;
	uint16_t queue;
	uint8_t station_id;
	uint8_t tid;
	uint8_t allocated;
	uint8_t enabled;
	uint8_t poisoned;
	uint8_t reset_barrier_required;
	uint8_t has_last_completion;
	uint8_t reserved[2];
};

int
drv_intel_ax211_tx_ring_api89_validate(
	const struct intel_ax211_protocol_command_table *table);

int
drv_intel_ax211_tx_ring_allocate(
	struct drv_dma_device *dma_device,
	const struct intel_ax211_tx_ring_ops *ops,
	void *ops_argument,
	struct intel_ax211_frame_ring *ring);

int
drv_intel_ax211_tx_ring_queue_add_build(
	const struct intel_ax211_frame_ring *ring,
	uint8_t station_id,
	uint8_t tid,
	struct intel_ax211_tx_queue_config *config);

int
drv_intel_ax211_tx_ring_queue_add_complete(
	struct intel_ax211_frame_ring *ring,
	const struct intel_ax211_tx_queue_config *config,
	uint32_t hardware_generation,
	uint64_t connection_generation,
	const struct intel_ax211_protocol_message *message,
	const struct intel_ax211_protocol_pending_command *pending);

int
drv_intel_ax211_tx_ring_submit(
	struct intel_ax211_frame_ring *ring,
	const struct intel_ax211_tx_request *request,
	uint64_t now,
	uint64_t timeout,
	struct intel_ax211_tx_ring_handle *handle);

int
drv_intel_ax211_tx_ring_complete(
	struct intel_ax211_frame_ring *ring,
	const struct intel_ax211_protocol_message *message,
	struct intel_ax211_tx_ring_retired *retired);

int
drv_intel_ax211_tx_ring_timeout_oldest(
	struct intel_ax211_frame_ring *ring,
	uint64_t now,
	struct intel_ax211_tx_ring_handle *handle);

/* dma_quiesced is proof that queue 1 can no longer access host memory. */
int
drv_intel_ax211_tx_ring_reset(
	struct intel_ax211_frame_ring *ring,
	int dma_quiesced);

/* Release has the same barrier rule and frees allocations in reverse order. */
int
drv_intel_ax211_tx_ring_release(
	struct intel_ax211_frame_ring *ring,
	int dma_quiesced);

#endif
