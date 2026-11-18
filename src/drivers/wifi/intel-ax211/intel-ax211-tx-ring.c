/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Intel AX211 private Gen3 data transmit ring.
 */

#include "intel-ax211-tx-ring.h"
#include <kern/kcrt.h>

/*
 * The field values of the queue setup command and of a TFD.
 *
 * Queue operation 0 adds a queue, and size code 5 announces 256 descriptors
 * (log2 of the count minus 3).  A TFD starts with the buffer count in its low
 * 5 bits, then one 10-byte entry per buffer from byte 2: a 16-bit length and
 * a 64-bit address.  A byte-count table entry holds the length in its low 14
 * bits.  The narrow command header of the transmit command is 4 bytes.
 *
 * Values referenced: Linux v6.17
 * drivers/net/wireless/intel/iwlwifi/fw/api/datapath.h (the queue operation
 * and the size code) and drivers/net/wireless/intel/iwlwifi/iwl-fh.h (the TFD
 * and the byte-count table entry).
 */
#define AX211_TX_QUEUE_OPERATION_ADD 0U
#define AX211_TX_QUEUE_CB_SIZE 5U
#define AX211_TX_TFD_NUM_TBS_MASK 0x1fU
#define AX211_TX_TFD_TB_OFFSET 2U
#define AX211_TX_TFD_TB_SIZE 10U
#define AX211_TX_BC_LENGTH_MASK 0x3fffU
#define AX211_TX_NARROW_HEADER_SIZE 4U

static int ax211_tx_ring_buffer_allocate(struct intel_ax211_frame_ring *ring, struct drv_dma_buffer *buffer, size_t size, size_t alignment);
static int ax211_tx_ring_buffer_valid(const struct drv_dma_buffer *buffer, size_t size, uint64_t alignment);
static void ax211_tx_ring_scrub(void *memory, size_t length);
static void ax211_tx_ring_allocations_release(struct intel_ax211_frame_ring *ring);
static void ax211_tx_ring_buffer_release(struct intel_ax211_frame_ring *ring, struct drv_dma_buffer *buffer);
static int ax211_tx_ring_valid(const struct intel_ax211_frame_ring *ring);
static int ax211_tx_queue_tid_valid(uint8_t tid);
static void ax211_tx_queue_command_encode(uint8_t *command, uint8_t station_id, uint8_t tid, uint64_t byte_count_address, uint64_t tfd_address);
static void ax211_tx_ring_put_le32(uint8_t *bytes, uint32_t value);
static void ax211_tx_ring_put_le64(uint8_t *bytes, uint64_t value);
static int ax211_tx_queue_config_valid(const struct intel_ax211_frame_ring *ring, const struct intel_ax211_tx_queue_config *config);
static int ax211_tx_protocol_result(int result);
static uint16_t ax211_tx_ring_get_le16(const uint8_t *bytes);
static int ax211_tx_ring_cookie_active(const struct intel_ax211_frame_ring *ring, uint64_t cookie);
static int ax211_tx_codec_result(int result);
static int ax211_tx_ring_slot_stage(struct intel_ax211_frame_ring *ring, struct intel_ax211_tx_ring_slot *slot, uint8_t index, const struct intel_ax211_tx_request *request, const struct intel_ax211_tx_prepared *prepared, uint64_t deadline);
static void ax211_tx_ring_put_le16(uint8_t *bytes, uint16_t value);
static void ax211_tx_ring_tfd_tb_encode(uint8_t *tfd, unsigned tb, uint16_t length, uint64_t address);
static void ax211_tx_ring_slot_scrub(struct intel_ax211_frame_ring *ring, uint8_t index);
static int ax211_tx_ring_sync_submission(struct intel_ax211_frame_ring *ring, uint8_t index, size_t command_length, size_t payload_length);
static int ax211_tx_ring_active_sequence(const struct intel_ax211_frame_ring *ring, uint16_t next_sequence);

/*
 * Checks that the pinned image lists the transmit and queue setup commands in
 * the layouts used here.
 */
int
drv_intel_ax211_tx_ring_api89_validate(
	const struct intel_ax211_protocol_command_table *table)
{
	struct intel_ax211_protocol_command_version version;
	int result;

	/* Refuses a missing table. */
	if (table == NULL)
		return INTEL_AX211_TX_RING_INVALID;

	/* Requires the transmit command's versions. */
	result = drv_intel_ax211_tx_api89_validate(table);
	if (result != INTEL_AX211_TX_OK) {
		/*
		 * An invalid table stays invalid; anything else is unsupported.
		 */
		if (result == INTEL_AX211_TX_INVALID)
			return INTEL_AX211_TX_RING_INVALID;
		return INTEL_AX211_TX_RING_UNSUPPORTED;
	}

	/* Looks up the queue setup command. */
	result = drv_intel_ax211_protocol_command_version_lookup(
		table, INTEL_AX211_TX_QUEUE_SETUP_FAMILY,
		INTEL_AX211_TX_QUEUE_SETUP_OPCODE, &version);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return INTEL_AX211_TX_RING_UNSUPPORTED;

	/* Refuses queue setup versions other than the ones encoded here. */
	if (version.command_version !=
		    INTEL_AX211_TX_QUEUE_SETUP_COMMAND_VERSION ||
	    version.notification_version !=
		    INTEL_AX211_TX_QUEUE_SETUP_RESPONSE_VERSION) {
		return INTEL_AX211_TX_RING_UNSUPPORTED;
	}

	/* Succeeded: the image lists the commands as expected. */
	return INTEL_AX211_TX_RING_OK;
}

/*
 * Allocates the DMA memory of one data transmit ring: the TFD ring, the
 * byte-count table and two buffers per slot.
 */
int
drv_intel_ax211_tx_ring_allocate(
	struct drv_dma_device *dma_device,
	const struct intel_ax211_tx_ring_ops *ops,
	void *ops_argument,
	struct intel_ax211_frame_ring *ring)
{
	size_t index;
	int coherent;
	int result;

	/* Refuses missing arguments and a ring that is already allocated. */
	if (dma_device == NULL ||
	    ops == NULL ||
	    ops->sync_for_device == NULL ||
	    ops->write32 == NULL ||
	    ring == NULL ||
	    ring->allocated) {
		return INTEL_AX211_TX_RING_INVALID;
	}

	/* Refuses a device whose DMA is not coherent. */
	coherent = drv_dma_device_is_coherent(dma_device);
	if (!coherent)
		return INTEL_AX211_TX_RING_UNSUPPORTED;

	/* Allocates the TFD ring and the byte-count table. */
	kern_memset(ring, 0, sizeof(*ring));
	ring->dma_device = dma_device;
	ring->ops = ops;
	ring->ops_argument = ops_argument;
	result = ax211_tx_ring_buffer_allocate(
		ring, &ring->tfd, INTEL_AX211_TX_RING_TFD_RING_SIZE, 256U);
	if (result == INTEL_AX211_TX_RING_OK) {
		result = ax211_tx_ring_buffer_allocate(
			ring, &ring->byte_count,
			INTEL_AX211_TX_RING_BYTE_COUNT_SIZE, 128U);
	}

	/*
	 * Allocates the command and payload buffers of every slot; the first
	 * failure stops.
	 */
	index = 0U;
	while (result == INTEL_AX211_TX_RING_OK &&
	       index < INTEL_AX211_TX_RING_DESCRIPTOR_SLOTS) {
		/*
		 * Allocates the slot's command buffer, then its payload buffer.
		 */
		result = ax211_tx_ring_buffer_allocate(
			ring, &ring->slot[index].command,
			INTEL_AX211_TX_RING_COMMAND_DMA_SIZE, 64U);
		if (result == INTEL_AX211_TX_RING_OK) {
			result = ax211_tx_ring_buffer_allocate(
				ring, &ring->slot[index].payload,
				INTEL_AX211_TX_RING_PAYLOAD_DMA_SIZE, 4U);
		}

		/* Moves to the next slot. */
		index++;
	}

	/* Releases everything when an allocation failed. */
	if (result != INTEL_AX211_TX_RING_OK) {
		ax211_tx_ring_allocations_release(ring);
		kern_memset(ring, 0, sizeof(*ring));
		return result;
	}

	/* Succeeded: the ring is allocated but not yet a firmware queue. */
	ring->allocated = 1U;
	return INTEL_AX211_TX_RING_OK;
}

/*
 * Builds the queue setup command that makes the ring a firmware queue for one
 * station and TID.
 */
int
drv_intel_ax211_tx_ring_queue_add_build(
	const struct intel_ax211_frame_ring *ring,
	uint8_t station_id,
	uint8_t tid,
	struct intel_ax211_tx_queue_config *config)
{
	struct intel_ax211_tx_queue_config candidate;
	int valid;

	/* Refuses an unusable ring. */
	valid = ax211_tx_ring_valid(ring);
	if (!valid)
		return INTEL_AX211_TX_RING_INVALID;

	/*
	 * Refuses a missing output, a ring in use and a station out of range.
	 */
	if (config == NULL ||
	    ring->enabled ||
	    ring->pending_count != 0U ||
	    ring->poisoned ||
	    station_id >= 32U) {
		return INTEL_AX211_TX_RING_INVALID;
	}

	/* Refuses a TID the ring cannot carry. */
	valid = ax211_tx_queue_tid_valid(tid);
	if (!valid)
		return INTEL_AX211_TX_RING_INVALID;

	/* Builds the command with the ring's addresses. */
	kern_memset(&candidate, 0, sizeof(candidate));
	candidate.tfd_address = ring->tfd.device_address;
	candidate.byte_count_address = ring->byte_count.device_address;
	candidate.station_id = station_id;
	candidate.tid = tid;
	ax211_tx_queue_command_encode(candidate.command, station_id, tid,
				      candidate.byte_count_address,
				      candidate.tfd_address);

	/* Succeeded: publishes the command. */
	*config = candidate;
	return INTEL_AX211_TX_RING_OK;
}

/*
 * Takes the answer to the queue setup command and enables the ring on the
 * queue the firmware assigned.
 */
int
drv_intel_ax211_tx_ring_queue_add_complete(
	struct intel_ax211_frame_ring *ring,
	const struct intel_ax211_tx_queue_config *config,
	uint32_t hardware_generation,
	uint64_t connection_generation,
	const struct intel_ax211_protocol_message *message,
	const struct intel_ax211_protocol_pending_command *pending)
{
	const uint8_t *payload;
	uint16_t queue;
	uint16_t write_pointer;
	uint16_t flags;
	int valid;
	int result;

	/* Refuses an unusable ring. */
	valid = ax211_tx_ring_valid(ring);
	if (!valid)
		return INTEL_AX211_TX_RING_INVALID;

	/* Refuses a command that was not built for this ring. */
	valid = ax211_tx_queue_config_valid(ring, config);
	if (!valid)
		return INTEL_AX211_TX_RING_INVALID;

	/* Refuses unset generations, missing arguments and a ring in use. */
	if (hardware_generation == 0U ||
	    connection_generation == 0U ||
	    message == NULL ||
	    pending == NULL ||
	    ring->enabled ||
	    ring->pending_count != 0U ||
	    ring->poisoned) {
		return INTEL_AX211_TX_RING_INVALID;
	}

	/* Refuses a pending command other than this epoch's queue setup. */
	if (pending->group != INTEL_AX211_TX_QUEUE_SETUP_FAMILY ||
	    pending->opcode != INTEL_AX211_TX_QUEUE_SETUP_OPCODE ||
	    pending->response_version !=
		    INTEL_AX211_TX_QUEUE_SETUP_RESPONSE_VERSION ||
	    pending->generation != hardware_generation ||
	    pending->minimum_response_length !=
		    INTEL_AX211_TX_QUEUE_SETUP_RESPONSE_SIZE ||
	    pending->maximum_response_length !=
		    INTEL_AX211_TX_QUEUE_SETUP_RESPONSE_SIZE) {
		return INTEL_AX211_TX_RING_INVALID;
	}

	/* Checks that the answer belongs to the command. */
	result = drv_intel_ax211_protocol_command_response_validate(message,
								    pending);
	if (result != INTEL_AX211_PROTOCOL_OK) {
		result = ax211_tx_protocol_result(result);
		return result;
	}

	/* Takes the queue and the write pointer from the answer. */
	payload = message->payload;
	if (payload == NULL)
		return INTEL_AX211_TX_RING_MALFORMED;
	queue = ax211_tx_ring_get_le16(payload);
	write_pointer = ax211_tx_ring_get_le16(payload + 4U);

	/* Refuses a queue number out of range. */
	if (queue < INTEL_AX211_TX_QUEUE_MIN ||
	    queue > INTEL_AX211_TX_QUEUE_MAX) {
		return INTEL_AX211_TX_RING_MALFORMED;
	}

	/*
	 * Refuses nonzero flags.  API 89 hardware has been observed returning
	 * bit 0 in this field together with a valid queue number and write
	 * pointer.  The response flags are not a completion status, so a valid
	 * dynamically assigned queue is not rejected for them.
	 *
	 * Procedure referenced: Linux v6.17
	 * drivers/net/wireless/intel/iwlwifi/pcie/gen1_2/tx-gen2.c
	 * (iwl_pcie_txq_alloc_response) and OpenBSD 0f464d413c50
	 * sys/dev/pci/if_iwx.c (iwx_enable_txq).
	 */
	flags = ax211_tx_ring_get_le16(payload + 6U);
	if (flags != 0U)
		return INTEL_AX211_TX_RING_MALFORMED;

	/*
	 * Enables the ring on the assigned queue from the firmware's write
	 * pointer.
	 */
	ring->hardware_generation = hardware_generation;
	ring->connection_generation = connection_generation;
	ring->queue = queue;
	ring->read_sequence =
		write_pointer & (INTEL_AX211_TX_RING_DESCRIPTOR_SLOTS - 1U);
	ring->write_sequence = ring->read_sequence;
	ring->station_id = config->station_id;
	ring->tid = config->tid;
	ring->enabled = 1U;
	ring->has_last_completion = 0U;

	/* Succeeded: the ring accepts frames. */
	return INTEL_AX211_TX_RING_OK;
}

/*
 * Places one frame in the next slot and rings the queue's doorbell.
 */
int
drv_intel_ax211_tx_ring_submit(
	struct intel_ax211_frame_ring *ring,
	const struct intel_ax211_tx_request *request,
	uint64_t now,
	uint64_t timeout,
	struct intel_ax211_tx_ring_handle *handle)
{
	struct intel_ax211_tx_prepared prepared;
	struct intel_ax211_tx_ring_slot *slot;
	uint64_t deadline;
	uint32_t write_pointer;
	uint16_t sequence;
	uint8_t index;
	int valid;
	int active;
	int result;

	/* Refuses an unusable ring. */
	valid = ax211_tx_ring_valid(ring);
	if (!valid)
		return INTEL_AX211_TX_RING_INVALID;

	/* Refuses missing arguments and a timeout that is zero or overflows. */
	if (request == NULL ||
	    handle == NULL ||
	    timeout == 0U ||
	    now > UINT64_MAX - timeout) {
		return INTEL_AX211_TX_RING_INVALID;
	}

	/* Refuses a ring that is not a firmware queue yet. */
	if (!ring->enabled)
		return INTEL_AX211_TX_RING_NOT_READY;

	/* Refuses a ring that a failure left untrusted. */
	if (ring->poisoned)
		return INTEL_AX211_TX_RING_POISONED;

	/* Refuses a frame of another connection. */
	if (request->connection_generation != ring->connection_generation)
		return INTEL_AX211_TX_RING_STALE;

	/* Refuses a frame beyond the in-flight limit. */
	if (ring->pending_count >= INTEL_AX211_TX_RING_INFLIGHT_LIMIT)
		return INTEL_AX211_TX_RING_FULL;

	/* Refuses a frame whose cookie is already in flight. */
	active = ax211_tx_ring_cookie_active(ring, request->cookie);
	if (active)
		return INTEL_AX211_TX_RING_DUPLICATE;

	/* Builds the transmit command. */
	result = drv_intel_ax211_tx_prepare(request, &prepared);
	if (result != INTEL_AX211_TX_OK) {
		result = ax211_tx_codec_result(result);
		return result;
	}

	/*
	 * Takes the slot at the write pointer; a busy slot means a broken ring.
	 */
	deadline = now + timeout;
	sequence = ring->write_sequence;
	index = (uint8_t)sequence;
	slot = &ring->slot[index];
	if (slot->active)
		return INTEL_AX211_TX_RING_MALFORMED;

	/* Fills the slot's buffers and descriptor. */
	result = ax211_tx_ring_slot_stage(ring, slot, index, request, &prepared,
					  deadline);
	if (result != INTEL_AX211_TX_RING_OK) {
		ax211_tx_ring_slot_scrub(ring, index);
		return result;
	}

	/* Hands the slot's memory to the device. */
	result = ax211_tx_ring_sync_submission(ring, index,
					       AX211_TX_NARROW_HEADER_SIZE +
						       prepared.command_length,
					       prepared.payload_length);
	if (result != INTEL_AX211_TX_RING_OK) {
		ax211_tx_ring_slot_scrub(ring, index);
		return result;
	}

	/* Counts the frame as in flight and advances the write pointer. */
	slot->active = 1U;
	ring->pending_count++;
	ring->write_sequence = (uint16_t)(sequence + 1U);

	/*
	 * Rings the doorbell.  A failed doorbell may still have reached the device,
	 * so the slot is uncertain and the ring needs a reset.
	 */
	write_pointer = ((uint32_t)ring->queue << 16) | ring->write_sequence;
	result = ring->ops->write32(ring->ops_argument,
				    INTEL_AX211_TX_RING_WRITE_POINTER_REGISTER,
				    write_pointer);
	if (result != 0) {
		slot->uncertain = 1U;
		ring->poisoned = 1U;
		ring->reset_barrier_required = 1U;
		return INTEL_AX211_TX_RING_KICK_FAILED;
	}

	/* Succeeded: reports the frame's handle. */
	*handle = slot->handle;
	return INTEL_AX211_TX_RING_OK;
}

/*
 * Takes the completion of the oldest frame in flight and frees its slot.
 */
int
drv_intel_ax211_tx_ring_complete(
	struct intel_ax211_frame_ring *ring,
	const struct intel_ax211_protocol_message *message,
	struct intel_ax211_tx_ring_retired *retired)
{
	struct intel_ax211_tx_completion completion;
	struct intel_ax211_tx_ring_retired candidate;
	struct intel_ax211_tx_ring_slot *slot;
	uint16_t expected_sequence;
	uint8_t expected_index;
	int valid;
	int active;
	int result;

	/* Refuses an unusable ring. */
	valid = ax211_tx_ring_valid(ring);
	if (!valid)
		return INTEL_AX211_TX_RING_INVALID;

	/* Refuses missing arguments. */
	if (message == NULL || retired == NULL)
		return INTEL_AX211_TX_RING_INVALID;

	/* Refuses a ring that is not a firmware queue. */
	if (!ring->enabled)
		return INTEL_AX211_TX_RING_NOT_READY;

	/* Refuses a ring that a failure left untrusted. */
	if (ring->poisoned)
		return INTEL_AX211_TX_RING_POISONED;

	/* Decodes the completion. */
	result = drv_intel_ax211_tx_completion_decode(
		message, ring->hardware_generation, ring->queue, message->index,
		&completion);
	if (result != INTEL_AX211_TX_OK) {
		result = ax211_tx_codec_result(result);
		return result;
	}

	/*
	 * With nothing in flight, the completion is a repeat of the last one or
	 * stale.
	 */
	if (ring->pending_count == 0U) {
		/* The completion of the last retired slot again. */
		if (ring->has_last_completion &&
		    completion.scheduler_sequence ==
			    ring->last_completion_sequence &&
		    message->index ==
			    (uint8_t)(ring->last_completion_sequence - 1U)) {
			return INTEL_AX211_TX_RING_DUPLICATE;
		}

		/* Any other completion is stale. */
		return INTEL_AX211_TX_RING_STALE;
	}

	/*
	 * A completion of another slot than the oldest is out of order, a
	 * repeat or stale.
	 */
	expected_index = (uint8_t)ring->read_sequence;
	if (message->index != expected_index) {
		/* A younger frame in flight completed first. */
		if (ring->slot[message->index].active)
			return INTEL_AX211_TX_RING_OUT_OF_ORDER;

		/* The completion of the last retired slot again. */
		if (ring->has_last_completion &&
		    completion.scheduler_sequence ==
			    ring->last_completion_sequence &&
		    message->index ==
			    (uint8_t)(ring->last_completion_sequence - 1U)) {
			return INTEL_AX211_TX_RING_DUPLICATE;
		}

		/* Any other completion is stale. */
		return INTEL_AX211_TX_RING_STALE;
	}

	/*
	 * Refuses an oldest slot that is idle or belongs to another epoch or
	 * connection.
	 */
	slot = &ring->slot[expected_index];
	if (!slot->active ||
	    slot->handle.hardware_generation != ring->hardware_generation ||
	    slot->handle.connection_generation != ring->connection_generation) {
		return INTEL_AX211_TX_RING_MALFORMED;
	}

	/*
	 * A completion with another scheduler sequence is out of order, a
	 * repeat or stale.
	 */
	expected_sequence = (uint16_t)(slot->handle.scheduler_sequence + 1U);
	if (completion.scheduler_sequence != expected_sequence) {
		/* It names a younger frame in flight. */
		active = ax211_tx_ring_active_sequence(
			ring, (uint16_t)completion.scheduler_sequence);
		if (active)
			return INTEL_AX211_TX_RING_OUT_OF_ORDER;

		/* It repeats the last retired sequence. */
		if (ring->has_last_completion &&
		    completion.scheduler_sequence ==
			    ring->last_completion_sequence) {
			return INTEL_AX211_TX_RING_DUPLICATE;
		}

		/* Any other sequence is stale. */
		return INTEL_AX211_TX_RING_STALE;
	}

	/* Refuses a completion of another length than the frame sent. */
	if (completion.byte_count != slot->frame_length)
		return INTEL_AX211_TX_RING_MALFORMED;

	/* Records the outcome and retires the slot. */
	kern_memset(&candidate, 0, sizeof(candidate));
	candidate.handle = slot->handle;
	candidate.sequence_control = completion.sequence_control;
	candidate.byte_count = completion.byte_count;
	candidate.acknowledged = completion.acknowledged;
	candidate.rts_retries = completion.rts_retries;
	candidate.frame_retries = completion.frame_retries;
	ax211_tx_ring_slot_scrub(ring, expected_index);
	ring->pending_count--;
	ring->read_sequence = expected_sequence;
	ring->last_completion_sequence = expected_sequence;
	ring->has_last_completion = 1U;
	*retired = candidate;

	/* Reports a frame that was not sent. */
	if (!candidate.acknowledged)
		return INTEL_AX211_TX_RING_TX_FAILED;

	/* Succeeded: the frame was sent. */
	return INTEL_AX211_TX_RING_OK;
}

/*
 * Times out the oldest frame in flight, which leaves the ring untrusted.
 */
int
drv_intel_ax211_tx_ring_timeout_oldest(
	struct intel_ax211_frame_ring *ring,
	uint64_t now,
	struct intel_ax211_tx_ring_handle *handle)
{
	struct intel_ax211_tx_ring_slot *slot;
	uint8_t index;
	int valid;

	/* Refuses an unusable ring. */
	valid = ax211_tx_ring_valid(ring);
	if (!valid)
		return INTEL_AX211_TX_RING_INVALID;

	/* Refuses a missing handle. */
	if (handle == NULL)
		return INTEL_AX211_TX_RING_INVALID;

	/* Refuses a ring that is not a firmware queue. */
	if (!ring->enabled)
		return INTEL_AX211_TX_RING_NOT_READY;

	/* Refuses a ring that a failure left untrusted. */
	if (ring->poisoned)
		return INTEL_AX211_TX_RING_POISONED;

	/* Nothing is in flight. */
	if (ring->pending_count == 0U)
		return INTEL_AX211_TX_RING_NOT_READY;

	/* Refuses an oldest slot that is idle. */
	index = (uint8_t)ring->read_sequence;
	slot = &ring->slot[index];
	if (!slot->active)
		return INTEL_AX211_TX_RING_MALFORMED;

	/* Reports the oldest frame; it still has time until its deadline. */
	*handle = slot->handle;
	if (now < slot->handle.deadline)
		return INTEL_AX211_TX_RING_PENDING;

	/* The device may still own the slot: the ring needs a reset. */
	slot->uncertain = 1U;
	ring->poisoned = 1U;
	ring->reset_barrier_required = 1U;

	/* Reports the timeout. */
	return INTEL_AX211_TX_RING_TIMEOUT;
}

/*
 * Returns the ring to its allocated, disabled state once its DMA is quiet.
 */
int
drv_intel_ax211_tx_ring_reset(
	struct intel_ax211_frame_ring *ring,
	int dma_quiesced)
{
	size_t index;
	int valid;

	/* Refuses an unusable ring. */
	valid = ax211_tx_ring_valid(ring);
	if (!valid)
		return INTEL_AX211_TX_RING_INVALID;

	/* Refuses a quiesced flag other than 0 or 1. */
	if (dma_quiesced != 0 && dma_quiesced != 1)
		return INTEL_AX211_TX_RING_INVALID;

	/* A ring the device may still use needs its DMA quiesced first. */
	if ((ring->enabled || ring->pending_count != 0U ||
	     ring->reset_barrier_required) &&
	    !dma_quiesced) {
		return INTEL_AX211_TX_RING_BARRIER_REQUIRED;
	}

	/* Clears every slot, the TFD ring and the byte-count table. */
	for (index = 0U; index < INTEL_AX211_TX_RING_DESCRIPTOR_SLOTS; index++)
		ax211_tx_ring_slot_scrub(ring, (uint8_t)index);
	ax211_tx_ring_scrub(ring->tfd.address, ring->tfd.size);
	ax211_tx_ring_scrub(ring->byte_count.address, ring->byte_count.size);

	/* Forgets the queue, the sequences and every failure. */
	ring->connection_generation = 0U;
	ring->hardware_generation = 0U;
	ring->read_sequence = 0U;
	ring->write_sequence = 0U;
	ring->last_completion_sequence = 0U;
	ring->pending_count = 0U;
	ring->queue = 0U;
	ring->station_id = 0U;
	ring->tid = 0U;
	ring->enabled = 0U;
	ring->poisoned = 0U;
	ring->reset_barrier_required = 0U;
	ring->has_last_completion = 0U;

	/* Succeeded: the ring can be set up as a queue again. */
	return INTEL_AX211_TX_RING_OK;
}

/*
 * Resets the ring and frees its DMA memory.
 */
int
drv_intel_ax211_tx_ring_release(
	struct intel_ax211_frame_ring *ring,
	int dma_quiesced)
{
	int valid;
	int result;

	/* Refuses an unusable ring. */
	valid = ax211_tx_ring_valid(ring);
	if (!valid)
		return INTEL_AX211_TX_RING_INVALID;

	/* Refuses a quiesced flag other than 0 or 1. */
	if (dma_quiesced != 0 && dma_quiesced != 1)
		return INTEL_AX211_TX_RING_INVALID;

	/* Resets the ring first. */
	result = drv_intel_ax211_tx_ring_reset(ring, dma_quiesced);
	if (result != INTEL_AX211_TX_RING_OK)
		return result;

	/* Frees the memory and forgets the ring. */
	ax211_tx_ring_allocations_release(ring);
	kern_memset(ring, 0, sizeof(*ring));

	/* Succeeded. */
	return INTEL_AX211_TX_RING_OK;
}

/* Allocates one zeroed coherent DMA buffer of an exact size and alignment. */
static int
ax211_tx_ring_buffer_allocate(
	struct intel_ax211_frame_ring *ring,
	struct drv_dma_buffer *buffer,
	size_t size,
	size_t alignment)
{
	int error;
	int valid;

	/* Allocates the buffer. */
	error = drv_dma_alloc_coherent(ring->dma_device, size, alignment,
				       buffer);
	if (error != 0)
		return INTEL_AX211_TX_RING_NO_MEMORY;

	/* Frees a buffer of another size or alignment. */
	valid = ax211_tx_ring_buffer_valid(buffer, size, alignment);
	if (!valid) {
		/* Clears what was allocated before it is freed. */
		if (buffer->address != NULL && buffer->size != 0U)
			ax211_tx_ring_scrub(buffer->address, buffer->size);
		drv_dma_free_coherent(ring->dma_device, buffer);
		kern_memset(buffer, 0, sizeof(*buffer));
		return INTEL_AX211_TX_RING_IO_ERROR;
	}

	/* Zeroes the buffer. */
	kern_memset(buffer->address, 0, buffer->size);

	/* Succeeded. */
	return INTEL_AX211_TX_RING_OK;
}

/*
 * Reports whether a DMA buffer has the exact size and a power-of-two
 * alignment: 1 when it has, 0 when not.
 */
static int
ax211_tx_ring_buffer_valid(
	const struct drv_dma_buffer *buffer,
	size_t size,
	uint64_t alignment)
{
	/*
	 * Rejects a missing buffer, another size, an unset address or a bad
	 * alignment.
	 */
	if (buffer == NULL ||
	    buffer->address == NULL ||
	    buffer->size != size ||
	    buffer->device_address == 0U ||
	    alignment == 0U ||
	    (alignment & (alignment - 1U)) != 0U ||
	    (buffer->device_address & (alignment - 1U)) != 0U) {
		return 0;
	}

	/*
	 * Rejects a buffer whose end would pass the top of the address space.
	 */
	if (buffer->device_address > UINT64_MAX - (uint64_t)(size - 1U))
		return 0;

	/* The buffer is usable. */
	return 1;
}

/*
 * Clears memory through a volatile pointer so the clearing is not optimized
 * away.
 */
static void
ax211_tx_ring_scrub(
	void *memory,
	size_t length)
{
	volatile uint8_t *bytes;

	/* Nothing to clear without memory. */
	if (memory == NULL)
		return;

	/* Clears every byte. */
	bytes = memory;
	while (length != 0U) {
		*bytes++ = 0U;
		length--;
	}
}

/* Frees every DMA buffer of the ring, the slots first. */
static void
ax211_tx_ring_allocations_release(
	struct intel_ax211_frame_ring *ring)
{
	size_t index;

	/* Frees the buffers of every slot in reverse order. */
	index = INTEL_AX211_TX_RING_DESCRIPTOR_SLOTS;
	while (index != 0U) {
		index--;
		ax211_tx_ring_buffer_release(ring, &ring->slot[index].payload);
		ax211_tx_ring_buffer_release(ring, &ring->slot[index].command);
	}

	/* Frees the byte-count table and the TFD ring. */
	ax211_tx_ring_buffer_release(ring, &ring->byte_count);
	ax211_tx_ring_buffer_release(ring, &ring->tfd);
}

/* Clears and frees one DMA buffer, and forgets it. */
static void
ax211_tx_ring_buffer_release(
	struct intel_ax211_frame_ring *ring,
	struct drv_dma_buffer *buffer)
{
	/* Only an allocated buffer is cleared and freed. */
	if (buffer->address != NULL) {
		ax211_tx_ring_scrub(buffer->address, buffer->size);
		drv_dma_free_coherent(ring->dma_device, buffer);
	}

	/* Forgets the buffer. */
	kern_memset(buffer, 0, sizeof(*buffer));
}

/*
 * Reports whether a ring is allocated with every buffer as it needs: 1 when
 * it is, 0 when not.
 */
static int
ax211_tx_ring_valid(
	const struct intel_ax211_frame_ring *ring)
{
	size_t index;
	int valid;

	/* Rejects a missing or unallocated ring and missing operations. */
	if (ring == NULL ||
	    !ring->allocated ||
	    ring->dma_device == NULL ||
	    ring->ops == NULL ||
	    ring->ops->sync_for_device == NULL ||
	    ring->ops->write32 == NULL) {
		return 0;
	}

	/* Rejects a TFD ring of another size or alignment. */
	valid = ax211_tx_ring_buffer_valid(
		&ring->tfd, INTEL_AX211_TX_RING_TFD_RING_SIZE, 256U);
	if (!valid)
		return 0;

	/* Rejects a byte-count table of another size or alignment. */
	valid = ax211_tx_ring_buffer_valid(&ring->byte_count,
					   INTEL_AX211_TX_RING_BYTE_COUNT_SIZE,
					   128U);
	if (!valid)
		return 0;

	/* Rejects more frames in flight than the limit. */
	if (ring->pending_count > INTEL_AX211_TX_RING_INFLIGHT_LIMIT)
		return 0;

	/* Checks the buffers of every slot. */
	for (index = 0U; index < INTEL_AX211_TX_RING_DESCRIPTOR_SLOTS; index++) {
		/* Rejects a command buffer of another size or alignment. */
		valid = ax211_tx_ring_buffer_valid(
			&ring->slot[index].command,
			INTEL_AX211_TX_RING_COMMAND_DMA_SIZE, 64U);
		if (!valid)
			return 0;

		/* Rejects a payload buffer of another size or alignment. */
		valid = ax211_tx_ring_buffer_valid(
			&ring->slot[index].payload,
			INTEL_AX211_TX_RING_PAYLOAD_DMA_SIZE, 4U);
		if (!valid)
			return 0;
	}

	/* The ring is usable. */
	return 1;
}

/*
 * Reports whether the ring can carry a TID: 1 for a data TID or the
 * management TID, 0 otherwise.
 */
static int
ax211_tx_queue_tid_valid(
	uint8_t tid)
{
	/* The eight data TIDs. */
	if (tid < 8U)
		return 1;

	/* The management TID. */
	if (tid == INTEL_AX211_TX_RING_MANAGEMENT_TID)
		return 1;

	/* No other TID. */
	return 0;
}

/* Encodes the queue setup command that adds a queue for one station and TID. */
static void
ax211_tx_queue_command_encode(
	uint8_t *command,
	uint8_t station_id,
	uint8_t tid,
	uint64_t byte_count_address,
	uint64_t tfd_address)
{
	kern_memset(command, 0, INTEL_AX211_TX_QUEUE_SETUP_COMMAND_SIZE);
	ax211_tx_ring_put_le32(command, AX211_TX_QUEUE_OPERATION_ADD);
	ax211_tx_ring_put_le32(command + 4U, UINT32_C(1) << station_id);
	command[8U] = tid;
	ax211_tx_ring_put_le32(command + 12U, 0U);
	ax211_tx_ring_put_le32(command + 16U, AX211_TX_QUEUE_CB_SIZE);
	ax211_tx_ring_put_le64(command + 20U, byte_count_address);
	ax211_tx_ring_put_le64(command + 28U, tfd_address);
}

/* Stores a 32-bit value in little-endian byte order. */
static void
ax211_tx_ring_put_le32(
	uint8_t *bytes,
	uint32_t value)
{
	bytes[0U] = (uint8_t)value;
	bytes[1U] = (uint8_t)(value >> 8);
	bytes[2U] = (uint8_t)(value >> 16);
	bytes[3U] = (uint8_t)(value >> 24);
}

/* Stores a 64-bit value in little-endian byte order. */
static void
ax211_tx_ring_put_le64(
	uint8_t *bytes,
	uint64_t value)
{
	ax211_tx_ring_put_le32(bytes, (uint32_t)value);
	ax211_tx_ring_put_le32(bytes + 4U, (uint32_t)(value >> 32));
}

/*
 * Reports whether a queue setup command was built for this ring: 1 when it
 * was, 0 when not.
 */
static int
ax211_tx_queue_config_valid(
	const struct intel_ax211_frame_ring *ring,
	const struct intel_ax211_tx_queue_config *config)
{
	uint8_t expected[INTEL_AX211_TX_QUEUE_SETUP_COMMAND_SIZE];
	int valid;
	int compared;

	/* Rejects a missing command and a station out of range. */
	if (config == NULL || config->station_id >= 32U)
		return 0;

	/* Rejects a TID the ring cannot carry. */
	valid = ax211_tx_queue_tid_valid(config->tid);
	if (!valid)
		return 0;

	/* Rejects a command for another ring's memory. */
	if (config->tfd_address != ring->tfd.device_address ||
	    config->byte_count_address != ring->byte_count.device_address) {
		return 0;
	}

	/* Rejects a command whose bytes differ from what the fields encode. */
	ax211_tx_queue_command_encode(expected, config->station_id, config->tid,
				      config->byte_count_address,
				      config->tfd_address);
	compared = kern_memcmp(expected, config->command, sizeof(expected));
	if (compared != 0)
		return 0;

	/* The command belongs to the ring. */
	return 1;
}

/* Maps a protocol validation result to the ring's result. */
static int
ax211_tx_protocol_result(
	int result)
{
	/* An answer of an earlier epoch or another slot is stale. */
	if (result == INTEL_AX211_PROTOCOL_STALE ||
	    result == INTEL_AX211_PROTOCOL_TOKEN_MISMATCH) {
		return INTEL_AX211_TX_RING_STALE;
	}

	/* Another layout is unsupported. */
	if (result == INTEL_AX211_PROTOCOL_UNSUPPORTED)
		return INTEL_AX211_TX_RING_UNSUPPORTED;

	/* A command the firmware failed. */
	if (result == INTEL_AX211_PROTOCOL_FAILED)
		return INTEL_AX211_TX_RING_TX_FAILED;

	/* Invalid arguments stay invalid. */
	if (result == INTEL_AX211_PROTOCOL_INVALID)
		return INTEL_AX211_TX_RING_INVALID;

	/* Anything else is a malformed answer. */
	return INTEL_AX211_TX_RING_MALFORMED;
}

/* Reads a 16-bit little-endian value. */
static uint16_t
ax211_tx_ring_get_le16(
	const uint8_t *bytes)
{
	/* Assembles the value from its two bytes, least significant first. */
	return (uint16_t)bytes[0U] | ((uint16_t)bytes[1U] << 8);
}

/*
 * Reports whether a frame with the cookie is in flight: 1 when one is, 0 when
 * not.
 */
static int
ax211_tx_ring_cookie_active(
	const struct intel_ax211_frame_ring *ring,
	uint64_t cookie)
{
	size_t index;

	/* Looks at every slot. */
	for (index = 0U; index < INTEL_AX211_TX_RING_DESCRIPTOR_SLOTS; index++) {
		/* A slot in flight with the same cookie. */
		if (ring->slot[index].active &&
		    ring->slot[index].handle.cookie == cookie) {
			return 1;
		}
	}

	/* No frame with the cookie is in flight. */
	return 0;
}

/* Maps a transmit codec result to the ring's result. */
static int
ax211_tx_codec_result(
	int result)
{
	/* A stale completion or frame. */
	if (result == INTEL_AX211_TX_STALE)
		return INTEL_AX211_TX_RING_STALE;

	/* An unsupported frame or layout. */
	if (result == INTEL_AX211_TX_UNSUPPORTED)
		return INTEL_AX211_TX_RING_UNSUPPORTED;

	/* A completion the firmware failed. */
	if (result == INTEL_AX211_TX_FAILED)
		return INTEL_AX211_TX_RING_TX_FAILED;

	/* Invalid arguments stay invalid. */
	if (result == INTEL_AX211_TX_INVALID)
		return INTEL_AX211_TX_RING_INVALID;

	/* Anything else is malformed. */
	return INTEL_AX211_TX_RING_MALFORMED;
}

/*
 * Fills one slot: the command with its header, the payload, the TFD and the
 * byte-count entry.
 */
static int
ax211_tx_ring_slot_stage(
	struct intel_ax211_frame_ring *ring,
	struct intel_ax211_tx_ring_slot *slot,
	uint8_t index,
	const struct intel_ax211_tx_request *request,
	const struct intel_ax211_tx_prepared *prepared,
	uint64_t deadline)
{
	uint8_t *command;
	uint8_t *payload;
	uint8_t *tfd;
	uint8_t *byte_count;
	size_t command_length;
	unsigned num_tbs;

	/*
	 * Refuses a command or payload that does not fit its buffer or transfer
	 * buffer.
	 */
	command_length = AX211_TX_NARROW_HEADER_SIZE + prepared->command_length;
	if (command_length <= INTEL_AX211_TX_RING_HEADER_COPY_SIZE ||
	    command_length > slot->command.size ||
	    command_length - INTEL_AX211_TX_RING_HEADER_COPY_SIZE >
		    INTEL_AX211_TX_RING_TB_SIZE_MAX ||
	    prepared->payload_length > slot->payload.size ||
	    prepared->payload_length > INTEL_AX211_TX_RING_TB_SIZE_MAX ||
	    prepared->payload_offset > request->length ||
	    prepared->payload_length >
		    request->length - prepared->payload_offset) {
		return INTEL_AX211_TX_RING_MALFORMED;
	}

	/*
	 * Writes the narrow header and the command into the slot's command
	 * buffer.
	 */
	command = slot->command.address;
	payload = slot->payload.address;
	tfd = (uint8_t *)ring->tfd.address +
	      (size_t)index * INTEL_AX211_TX_RING_TFD_SIZE;
	byte_count = (uint8_t *)ring->byte_count.address +
		     (size_t)index * sizeof(uint16_t);
	kern_memset(command, 0, slot->command.size);
	kern_memset(payload, 0, slot->payload.size);
	kern_memset(tfd, 0, INTEL_AX211_TX_RING_TFD_SIZE);
	command[0U] = INTEL_AX211_TX_OPCODE;
	command[1U] = 0U;
	command[2U] = index;
	command[3U] = (uint8_t)(ring->queue & 0x1fU);
	kern_memcpy(command + AX211_TX_NARROW_HEADER_SIZE, prepared->command,
	       prepared->command_length);

	/* Copies the frame body into the payload buffer. */
	if (prepared->payload_length != 0U) {
		kern_memcpy(payload, request->frame + prepared->payload_offset,
		       prepared->payload_length);
	}

	/*
	 * Writes the TFD: the command's first bytes, the rest of the command, and
	 * the payload when there is one.
	 */
	num_tbs = prepared->payload_length == 0U ? 2U : 3U;
	ax211_tx_ring_put_le16(tfd,
			       (uint16_t)(num_tbs & AX211_TX_TFD_NUM_TBS_MASK));
	ax211_tx_ring_tfd_tb_encode(tfd, 0U, INTEL_AX211_TX_RING_HEADER_COPY_SIZE,
				    slot->command.device_address);
	ax211_tx_ring_tfd_tb_encode(
		tfd, 1U,
		(uint16_t)(command_length - INTEL_AX211_TX_RING_HEADER_COPY_SIZE),
		slot->command.device_address +
			INTEL_AX211_TX_RING_HEADER_COPY_SIZE);

	/* Adds the payload's transfer buffer. */
	if (prepared->payload_length != 0U) {
		ax211_tx_ring_tfd_tb_encode(tfd, 2U,
					    (uint16_t)prepared->payload_length,
					    slot->payload.device_address);
	}

	/* Refuses a frame length the byte-count entry cannot hold. */
	if (prepared->frame_length > AX211_TX_BC_LENGTH_MASK)
		return INTEL_AX211_TX_RING_MALFORMED;

	/* Writes the byte-count entry and the slot's handle. */
	ax211_tx_ring_put_le16(byte_count, prepared->frame_length);
	kern_memset(&slot->handle, 0, sizeof(slot->handle));
	slot->handle.connection_generation = prepared->connection_generation;
	slot->handle.cookie = prepared->cookie;
	slot->handle.key_generation = prepared->key_generation;
	slot->handle.packet_number = prepared->packet_number;
	slot->handle.deadline = deadline;
	slot->handle.hardware_generation = ring->hardware_generation;
	slot->handle.scheduler_sequence = ring->write_sequence;
	slot->handle.index = index;
	slot->frame_length = prepared->frame_length;
	slot->uncertain = 0U;

	/* Succeeded: the slot is ready to hand to the device. */
	return INTEL_AX211_TX_RING_OK;
}

/* Stores a 16-bit value in little-endian byte order. */
static void
ax211_tx_ring_put_le16(
	uint8_t *bytes,
	uint16_t value)
{
	bytes[0U] = (uint8_t)value;
	bytes[1U] = (uint8_t)(value >> 8);
}

/* Writes one transfer buffer entry of a TFD: its length and its address. */
static void
ax211_tx_ring_tfd_tb_encode(
	uint8_t *tfd,
	unsigned tb,
	uint16_t length,
	uint64_t address)
{
	size_t offset;

	/* The entries follow the buffer count, 10 bytes each. */
	offset = AX211_TX_TFD_TB_OFFSET + (size_t)tb * AX211_TX_TFD_TB_SIZE;
	ax211_tx_ring_put_le16(tfd + offset, length);
	ax211_tx_ring_put_le64(tfd + offset + 2U, address);
}

/* Clears one slot's buffers and descriptor and frees the slot. */
static void
ax211_tx_ring_slot_scrub(
	struct intel_ax211_frame_ring *ring,
	uint8_t index)
{
	struct intel_ax211_tx_ring_slot *slot;
	uint8_t *tfd;
	uint8_t *byte_count;

	/* Locates every buffer this slot owns. */
	slot = &ring->slot[index];
	tfd = (uint8_t *)ring->tfd.address +
	      (size_t)index * INTEL_AX211_TX_RING_TFD_SIZE;
	byte_count = (uint8_t *)ring->byte_count.address +
		     (size_t)index * sizeof(uint16_t);

	/* Erases them and returns the slot to the free state. */
	ax211_tx_ring_scrub(slot->command.address, slot->command.size);
	ax211_tx_ring_scrub(slot->payload.address, slot->payload.size);
	ax211_tx_ring_scrub(tfd, INTEL_AX211_TX_RING_TFD_SIZE);
	ax211_tx_ring_scrub(byte_count, sizeof(uint16_t));
	kern_memset(&slot->handle, 0, sizeof(slot->handle));
	slot->frame_length = 0U;
	slot->active = 0U;
	slot->uncertain = 0U;
}

/* Hands one slot's command, payload, TFD and byte-count entry to the device. */
static int
ax211_tx_ring_sync_submission(
	struct intel_ax211_frame_ring *ring,
	uint8_t index,
	size_t command_length,
	size_t payload_length)
{
	struct intel_ax211_tx_ring_slot *slot;
	size_t tfd_offset;
	size_t byte_count_offset;
	int result;

	/* Locates the slot's buffers. */
	slot = &ring->slot[index];
	tfd_offset = (size_t)index * INTEL_AX211_TX_RING_TFD_SIZE;
	byte_count_offset = (size_t)index * sizeof(uint16_t);

	/* Hands over the command. */
	result = ring->ops->sync_for_device(ring->ops_argument, &slot->command,
					    0U, command_length);
	if (result != 0)
		return INTEL_AX211_TX_RING_IO_ERROR;

	/* Hands over the payload when there is one. */
	if (payload_length != 0U) {
		/* Refuses a failed hand-over. */
		result = ring->ops->sync_for_device(ring->ops_argument,
						    &slot->payload, 0U,
						    payload_length);
		if (result != 0)
			return INTEL_AX211_TX_RING_IO_ERROR;
	}

	/* Hands over the TFD. */
	result = ring->ops->sync_for_device(ring->ops_argument, &ring->tfd,
					    tfd_offset,
					    INTEL_AX211_TX_RING_TFD_SIZE);
	if (result != 0)
		return INTEL_AX211_TX_RING_IO_ERROR;

	/* Hands over the byte-count entry. */
	result = ring->ops->sync_for_device(ring->ops_argument,
					    &ring->byte_count,
					    byte_count_offset,
					    sizeof(uint16_t));
	if (result != 0)
		return INTEL_AX211_TX_RING_IO_ERROR;

	/* Succeeded: the device may read the slot. */
	return INTEL_AX211_TX_RING_OK;
}

/*
 * Reports whether a frame in flight completes with a scheduler sequence: 1
 * when one does, 0 when not.
 */
static int
ax211_tx_ring_active_sequence(
	const struct intel_ax211_frame_ring *ring,
	uint16_t next_sequence)
{
	const struct intel_ax211_tx_ring_slot *slot;
	uint16_t sequence;
	uint16_t count;

	/* Walks the frames in flight from the oldest. */
	sequence = ring->read_sequence;
	count = 0U;
	while (count < ring->pending_count) {
		/*
		 * A frame in flight whose completion would carry the sequence.
		 */
		slot = &ring->slot[(uint8_t)sequence];
		if (slot->active &&
		    (uint16_t)(slot->handle.scheduler_sequence + 1U) ==
			    next_sequence) {
			return 1;
		}

		/* Moves to the next frame. */
		sequence = (uint16_t)(sequence + 1U);
		count++;
	}

	/* No frame in flight completes with the sequence. */
	return 0;
}
