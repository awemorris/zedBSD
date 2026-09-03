/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD Intel AX211 private Gen3 data transmit ring
 *
 * Wire constants and layouts are derived from OpenBSD
 * sys/dev/pci/if_iwxreg.h and if_iwx.c at commit
 * 0f464d413c50396e4e6cd70948f15613d6a73081.
 * Copyright (c) 2014, 2016 genua gmbh <info@genua.de>
 * Copyright (c) 2014 Fixup Software Ltd.
 * Copyright (c) 2017, 2019, 2020 Stefan Sperling <stsp@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Copyright(c) 2017 Intel Deutschland GmbH
 * Copyright(c) 2018 - 2019 Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name Intel Corporation nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * SPDX-License-Identifier: ISC AND BSD-3-Clause
 */

#include "intel-ax211-tx-ring.h"

#include <string.h>

#define AX211_TX_QUEUE_OPERATION_ADD                       0U
#define AX211_TX_QUEUE_CB_SIZE                             5U
#define AX211_TX_TFD_NUM_TBS_MASK                       0x1fU
#define AX211_TX_TFD_TB_OFFSET                             2U
#define AX211_TX_TFD_TB_SIZE                              10U
#define AX211_TX_BC_LENGTH_MASK                       0x3fffU
#define AX211_TX_NARROW_HEADER_SIZE                        4U

static void ax211_tx_ring_put_le16(uint8_t *bytes, uint16_t value);
static void ax211_tx_ring_put_le32(uint8_t *bytes, uint32_t value);
static void ax211_tx_ring_put_le64(uint8_t *bytes, uint64_t value);
static uint16_t ax211_tx_ring_get_le16(const uint8_t *bytes);
static void ax211_tx_ring_scrub(void *memory, size_t length);
static int ax211_tx_ring_buffer_allocate(struct intel_ax211_tx_ring *ring,
	struct drv_dma_buffer *buffer, size_t size, size_t alignment);
static void ax211_tx_ring_buffer_release(struct intel_ax211_tx_ring *ring,
	struct drv_dma_buffer *buffer);
static void ax211_tx_ring_allocations_release(
	struct intel_ax211_tx_ring *ring);
static int ax211_tx_ring_buffer_valid(const struct drv_dma_buffer *buffer,
	size_t size, uint64_t alignment);
static int ax211_tx_ring_valid(const struct intel_ax211_tx_ring *ring);
static int ax211_tx_queue_config_valid(
	const struct intel_ax211_tx_ring *ring,
	const struct intel_ax211_tx_queue_config *config);
static int ax211_tx_queue_tid_valid(uint8_t tid);
static void ax211_tx_queue_command_encode(uint8_t *command,
	uint8_t station_id, uint8_t tid, uint64_t byte_count_address,
	uint64_t tfd_address);
static int ax211_tx_protocol_result(int result);
static int ax211_tx_codec_result(int result);
static int ax211_tx_ring_cookie_active(
	const struct intel_ax211_tx_ring *ring, uint64_t cookie);
static int ax211_tx_ring_slot_stage(struct intel_ax211_tx_ring *ring,
	struct intel_ax211_tx_ring_slot *slot, uint8_t index,
	const struct intel_ax211_tx_request *request,
	const struct intel_ax211_tx_prepared *prepared, uint64_t deadline);
static int ax211_tx_ring_sync_submission(struct intel_ax211_tx_ring *ring,
	uint8_t index, size_t command_length, size_t payload_length);
static void ax211_tx_ring_tfd_tb_encode(uint8_t *tfd, unsigned tb,
	uint16_t length, uint64_t address);
static void ax211_tx_ring_slot_scrub(struct intel_ax211_tx_ring *ring,
	uint8_t index);
static int ax211_tx_ring_active_sequence(
	const struct intel_ax211_tx_ring *ring, uint16_t next_sequence);

int
intel_ax211_tx_ring_api89_validate(
	const struct intel_ax211_protocol_command_table *table)
{
	struct intel_ax211_protocol_command_version version;
	int result;

	if (table == NULL)
		return INTEL_AX211_TX_RING_INVALID;
	result = intel_ax211_tx_api89_validate(table);
	if (result != INTEL_AX211_TX_OK)
		return result == INTEL_AX211_TX_INVALID ?
		    INTEL_AX211_TX_RING_INVALID : INTEL_AX211_TX_RING_UNSUPPORTED;
	result = intel_ax211_protocol_command_version_lookup(table,
	    INTEL_AX211_TX_QUEUE_CONFIG_GROUP,
	    INTEL_AX211_TX_QUEUE_CONFIG_OPCODE, &version);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return INTEL_AX211_TX_RING_UNSUPPORTED;
	if (version.command_version !=
	    INTEL_AX211_TX_QUEUE_CONFIG_COMMAND_VERSION ||
	    version.notification_version !=
	    INTEL_AX211_TX_QUEUE_CONFIG_RESPONSE_VERSION)
		return INTEL_AX211_TX_RING_UNSUPPORTED;
	return INTEL_AX211_TX_RING_OK;
}

int
intel_ax211_tx_ring_allocate(
	struct drv_dma_device *dma_device,
	const struct intel_ax211_tx_ring_ops *ops,
	void *ops_argument,
	struct intel_ax211_tx_ring *ring)
{
	size_t index;
	int result;

	if (dma_device == NULL || ops == NULL ||
	    ops->sync_for_device == NULL || ops->write32 == NULL ||
	    ring == NULL || ring->allocated)
		return INTEL_AX211_TX_RING_INVALID;
	if (!drv_dma_device_is_coherent(dma_device))
		return INTEL_AX211_TX_RING_UNSUPPORTED;
	memset(ring, 0, sizeof(*ring));
	ring->dma_device = dma_device;
	ring->ops = ops;
	ring->ops_argument = ops_argument;
	result = ax211_tx_ring_buffer_allocate(ring, &ring->tfd,
	    INTEL_AX211_TX_RING_TFD_RING_SIZE, 256U);
	if (result == INTEL_AX211_TX_RING_OK)
		result = ax211_tx_ring_buffer_allocate(ring, &ring->byte_count,
		    INTEL_AX211_TX_RING_BYTE_COUNT_SIZE, 128U);
	index = 0U;
	while (result == INTEL_AX211_TX_RING_OK &&
	    index < INTEL_AX211_TX_RING_SLOT_COUNT) {
		result = ax211_tx_ring_buffer_allocate(ring,
		    &ring->slot[index].command,
		    INTEL_AX211_TX_RING_COMMAND_DMA_SIZE, 64U);
		if (result == INTEL_AX211_TX_RING_OK)
			result = ax211_tx_ring_buffer_allocate(ring,
			    &ring->slot[index].payload,
			    INTEL_AX211_TX_RING_PAYLOAD_DMA_SIZE, 4U);
		index++;
	}
	if (result != INTEL_AX211_TX_RING_OK) {
		ax211_tx_ring_allocations_release(ring);
		memset(ring, 0, sizeof(*ring));
		return result;
	}
	ring->allocated = 1U;
	return INTEL_AX211_TX_RING_OK;
}

int
intel_ax211_tx_ring_queue_add_build(
	const struct intel_ax211_tx_ring *ring,
	uint8_t station_id,
	uint8_t tid,
	struct intel_ax211_tx_queue_config *config)
{
	struct intel_ax211_tx_queue_config candidate;

	if (!ax211_tx_ring_valid(ring) || config == NULL || ring->enabled ||
	    ring->pending_count != 0U || ring->poisoned || station_id >= 32U ||
	    !ax211_tx_queue_tid_valid(tid))
		return INTEL_AX211_TX_RING_INVALID;
	memset(&candidate, 0, sizeof(candidate));
	candidate.tfd_address = ring->tfd.device_address;
	candidate.byte_count_address = ring->byte_count.device_address;
	candidate.station_id = station_id;
	candidate.tid = tid;
	ax211_tx_queue_command_encode(candidate.command, station_id, tid,
	    candidate.byte_count_address, candidate.tfd_address);
	*config = candidate;
	return INTEL_AX211_TX_RING_OK;
}

int
intel_ax211_tx_ring_queue_add_complete(
	struct intel_ax211_tx_ring *ring,
	const struct intel_ax211_tx_queue_config *config,
	uint32_t hardware_generation,
	uint64_t connection_generation,
	const struct intel_ax211_protocol_message *message,
	const struct intel_ax211_protocol_pending_command *pending)
{
	const uint8_t *payload;
	uint16_t queue;
	uint16_t write_pointer;
	int result;

	if (!ax211_tx_ring_valid(ring) ||
	    !ax211_tx_queue_config_valid(ring, config) ||
	    hardware_generation == 0U || connection_generation == 0U ||
	    message == NULL || pending == NULL || ring->enabled ||
	    ring->pending_count != 0U || ring->poisoned)
		return INTEL_AX211_TX_RING_INVALID;
	if (pending->group != INTEL_AX211_TX_QUEUE_CONFIG_GROUP ||
	    pending->opcode != INTEL_AX211_TX_QUEUE_CONFIG_OPCODE ||
	    pending->response_version !=
	    INTEL_AX211_TX_QUEUE_CONFIG_RESPONSE_VERSION ||
	    pending->generation != hardware_generation ||
	    pending->minimum_response_length !=
	    INTEL_AX211_TX_QUEUE_CONFIG_RESPONSE_SIZE ||
	    pending->maximum_response_length !=
	    INTEL_AX211_TX_QUEUE_CONFIG_RESPONSE_SIZE)
		return INTEL_AX211_TX_RING_INVALID;
	result = intel_ax211_protocol_command_response_validate(message,
	    pending);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return ax211_tx_protocol_result(result);
	payload = message->payload;
	if (payload == NULL)
		return INTEL_AX211_TX_RING_MALFORMED;
	queue = ax211_tx_ring_get_le16(payload);
	write_pointer = ax211_tx_ring_get_le16(payload + 4U);
	/*
	 * API 89 hardware has been observed returning bit 0 in this field
	 * together with a valid queue number and write pointer.  Linux iwlwifi
	 * and OpenBSD iwx deliberately do not use the response flags as a
	 * completion status.  Match those production drivers so a valid
	 * dynamically assigned queue is not rejected locally.
	 */
	if (queue < INTEL_AX211_TX_QUEUE_MIN ||
	    queue > INTEL_AX211_TX_QUEUE_MAX ||
	    ax211_tx_ring_get_le16(payload + 6U) != 0U)
		return INTEL_AX211_TX_RING_MALFORMED;
	ring->hardware_generation = hardware_generation;
	ring->connection_generation = connection_generation;
	ring->queue = queue;
	ring->read_sequence = write_pointer &
	    (INTEL_AX211_TX_RING_SLOT_COUNT - 1U);
	ring->write_sequence = ring->read_sequence;
	ring->station_id = config->station_id;
	ring->tid = config->tid;
	ring->enabled = 1U;
	ring->has_last_completion = 0U;
	return INTEL_AX211_TX_RING_OK;
}

int
intel_ax211_tx_ring_submit(
	struct intel_ax211_tx_ring *ring,
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
	int result;

	if (!ax211_tx_ring_valid(ring) || request == NULL || handle == NULL ||
	    timeout == 0U || now > UINT64_MAX - timeout)
		return INTEL_AX211_TX_RING_INVALID;
	if (!ring->enabled)
		return INTEL_AX211_TX_RING_NOT_READY;
	if (ring->poisoned)
		return INTEL_AX211_TX_RING_POISONED;
	if (request->connection_generation != ring->connection_generation)
		return INTEL_AX211_TX_RING_STALE;
	if (ring->pending_count >= INTEL_AX211_TX_RING_INFLIGHT_LIMIT)
		return INTEL_AX211_TX_RING_FULL;
	if (ax211_tx_ring_cookie_active(ring, request->cookie))
		return INTEL_AX211_TX_RING_DUPLICATE;
	result = intel_ax211_tx_prepare(request, &prepared);
	if (result != INTEL_AX211_TX_OK)
		return ax211_tx_codec_result(result);
	deadline = now + timeout;
	sequence = ring->write_sequence;
	index = (uint8_t)sequence;
	slot = &ring->slot[index];
	if (slot->active)
		return INTEL_AX211_TX_RING_MALFORMED;
	result = ax211_tx_ring_slot_stage(ring, slot, index, request,
	    &prepared, deadline);
	if (result != INTEL_AX211_TX_RING_OK) {
		ax211_tx_ring_slot_scrub(ring, index);
		return result;
	}
	result = ax211_tx_ring_sync_submission(ring, index,
	    AX211_TX_NARROW_HEADER_SIZE + prepared.command_length,
	    prepared.payload_length);
	if (result != INTEL_AX211_TX_RING_OK) {
		ax211_tx_ring_slot_scrub(ring, index);
		return result;
	}
	slot->active = 1U;
	ring->pending_count++;
	ring->write_sequence = (uint16_t)(sequence + 1U);
	write_pointer = ((uint32_t)ring->queue << 16) |
	    ring->write_sequence;
	if (ring->ops->write32(ring->ops_argument,
	    INTEL_AX211_TX_RING_WRITE_POINTER_REGISTER, write_pointer) != 0) {
		slot->uncertain = 1U;
		ring->poisoned = 1U;
		ring->reset_barrier_required = 1U;
		return INTEL_AX211_TX_RING_KICK_FAILED;
	}
	*handle = slot->handle;
	return INTEL_AX211_TX_RING_OK;
}

int
intel_ax211_tx_ring_complete(
	struct intel_ax211_tx_ring *ring,
	const struct intel_ax211_protocol_message *message,
	struct intel_ax211_tx_ring_retired *retired)
{
	struct intel_ax211_tx_completion completion;
	struct intel_ax211_tx_ring_retired candidate;
	struct intel_ax211_tx_ring_slot *slot;
	uint16_t expected_sequence;
	uint8_t expected_index;
	int result;

	if (!ax211_tx_ring_valid(ring) || message == NULL || retired == NULL)
		return INTEL_AX211_TX_RING_INVALID;
	if (!ring->enabled)
		return INTEL_AX211_TX_RING_NOT_READY;
	if (ring->poisoned)
		return INTEL_AX211_TX_RING_POISONED;
	result = intel_ax211_tx_completion_decode(message,
	    ring->hardware_generation, ring->queue,
	    message->index, &completion);
	if (result != INTEL_AX211_TX_OK)
		return ax211_tx_codec_result(result);
	if (ring->pending_count == 0U) {
		if (ring->has_last_completion &&
		    completion.scheduler_sequence ==
		    ring->last_completion_sequence && message->index ==
		    (uint8_t)(ring->last_completion_sequence - 1U))
			return INTEL_AX211_TX_RING_DUPLICATE;
		return INTEL_AX211_TX_RING_STALE;
	}
	expected_index = (uint8_t)ring->read_sequence;
	if (message->index != expected_index) {
		if (ring->slot[message->index].active)
			return INTEL_AX211_TX_RING_OUT_OF_ORDER;
		if (ring->has_last_completion &&
		    completion.scheduler_sequence ==
		    ring->last_completion_sequence && message->index ==
		    (uint8_t)(ring->last_completion_sequence - 1U))
			return INTEL_AX211_TX_RING_DUPLICATE;
		return INTEL_AX211_TX_RING_STALE;
	}
	slot = &ring->slot[expected_index];
	if (!slot->active ||
	    slot->handle.hardware_generation != ring->hardware_generation ||
	    slot->handle.connection_generation != ring->connection_generation)
		return INTEL_AX211_TX_RING_MALFORMED;
	expected_sequence = (uint16_t)(slot->handle.scheduler_sequence + 1U);
	if (completion.scheduler_sequence != expected_sequence) {
		if (ax211_tx_ring_active_sequence(ring,
		    (uint16_t)completion.scheduler_sequence))
			return INTEL_AX211_TX_RING_OUT_OF_ORDER;
		if (ring->has_last_completion &&
		    completion.scheduler_sequence ==
		    ring->last_completion_sequence)
			return INTEL_AX211_TX_RING_DUPLICATE;
		return INTEL_AX211_TX_RING_STALE;
	}
	if (completion.byte_count != slot->frame_length)
		return INTEL_AX211_TX_RING_MALFORMED;
	memset(&candidate, 0, sizeof(candidate));
	candidate.handle = slot->handle;
	candidate.sequence_control = completion.sequence_control;
	candidate.byte_count = completion.byte_count;
	candidate.acknowledged = completion.acknowledged;
	candidate.failure_rts = completion.failure_rts;
	candidate.failure_frame = completion.failure_frame;
	ax211_tx_ring_slot_scrub(ring, expected_index);
	ring->pending_count--;
	ring->read_sequence = expected_sequence;
	ring->last_completion_sequence = expected_sequence;
	ring->has_last_completion = 1U;
	*retired = candidate;
	return candidate.acknowledged ? INTEL_AX211_TX_RING_OK :
	    INTEL_AX211_TX_RING_TX_FAILED;
}

int
intel_ax211_tx_ring_timeout_oldest(
	struct intel_ax211_tx_ring *ring,
	uint64_t now,
	struct intel_ax211_tx_ring_handle *handle)
{
	struct intel_ax211_tx_ring_slot *slot;
	uint8_t index;

	if (!ax211_tx_ring_valid(ring) || handle == NULL)
		return INTEL_AX211_TX_RING_INVALID;
	if (!ring->enabled)
		return INTEL_AX211_TX_RING_NOT_READY;
	if (ring->poisoned)
		return INTEL_AX211_TX_RING_POISONED;
	if (ring->pending_count == 0U)
		return INTEL_AX211_TX_RING_NOT_READY;
	index = (uint8_t)ring->read_sequence;
	slot = &ring->slot[index];
	if (!slot->active)
		return INTEL_AX211_TX_RING_MALFORMED;
	*handle = slot->handle;
	if (now < slot->handle.deadline)
		return INTEL_AX211_TX_RING_PENDING;
	slot->uncertain = 1U;
	ring->poisoned = 1U;
	ring->reset_barrier_required = 1U;
	return INTEL_AX211_TX_RING_TIMEOUT;
}

int
intel_ax211_tx_ring_reset(
	struct intel_ax211_tx_ring *ring,
	int dma_quiesced)
{
	size_t index;

	if (!ax211_tx_ring_valid(ring) ||
	    (dma_quiesced != 0 && dma_quiesced != 1))
		return INTEL_AX211_TX_RING_INVALID;
	if ((ring->enabled || ring->pending_count != 0U ||
	    ring->reset_barrier_required) && !dma_quiesced)
		return INTEL_AX211_TX_RING_BARRIER_REQUIRED;
	for (index = 0U; index < INTEL_AX211_TX_RING_SLOT_COUNT; index++)
		ax211_tx_ring_slot_scrub(ring, (uint8_t)index);
	ax211_tx_ring_scrub(ring->tfd.address, ring->tfd.size);
	ax211_tx_ring_scrub(ring->byte_count.address, ring->byte_count.size);
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
	return INTEL_AX211_TX_RING_OK;
}

int
intel_ax211_tx_ring_release(
	struct intel_ax211_tx_ring *ring,
	int dma_quiesced)
{
	int result;

	if (!ax211_tx_ring_valid(ring) ||
	    (dma_quiesced != 0 && dma_quiesced != 1))
		return INTEL_AX211_TX_RING_INVALID;
	result = intel_ax211_tx_ring_reset(ring, dma_quiesced);
	if (result != INTEL_AX211_TX_RING_OK)
		return result;
	ax211_tx_ring_allocations_release(ring);
	memset(ring, 0, sizeof(*ring));
	return INTEL_AX211_TX_RING_OK;
}

static int
ax211_tx_ring_buffer_allocate(
	struct intel_ax211_tx_ring *ring,
	struct drv_dma_buffer *buffer,
	size_t size,
	size_t alignment)
{
	int error;

	error = drv_dma_alloc_coherent(ring->dma_device, size, alignment,
	    buffer);
	if (error != 0)
		return INTEL_AX211_TX_RING_NO_MEMORY;
	if (!ax211_tx_ring_buffer_valid(buffer, size, alignment)) {
		if (buffer->address != NULL && buffer->size != 0U)
			ax211_tx_ring_scrub(buffer->address, buffer->size);
		drv_dma_free_coherent(ring->dma_device, buffer);
		memset(buffer, 0, sizeof(*buffer));
		return INTEL_AX211_TX_RING_IO_ERROR;
	}
	memset(buffer->address, 0, buffer->size);
	return INTEL_AX211_TX_RING_OK;
}

static void
ax211_tx_ring_buffer_release(
	struct intel_ax211_tx_ring *ring,
	struct drv_dma_buffer *buffer)
{
	if (buffer->address != NULL) {
		ax211_tx_ring_scrub(buffer->address, buffer->size);
		drv_dma_free_coherent(ring->dma_device, buffer);
	}
	memset(buffer, 0, sizeof(*buffer));
}

static void
ax211_tx_ring_allocations_release(
	struct intel_ax211_tx_ring *ring)
{
	size_t index;

	index = INTEL_AX211_TX_RING_SLOT_COUNT;
	while (index != 0U) {
		index--;
		ax211_tx_ring_buffer_release(ring, &ring->slot[index].payload);
		ax211_tx_ring_buffer_release(ring, &ring->slot[index].command);
	}
	ax211_tx_ring_buffer_release(ring, &ring->byte_count);
	ax211_tx_ring_buffer_release(ring, &ring->tfd);
}

static int
ax211_tx_ring_buffer_valid(
	const struct drv_dma_buffer *buffer,
	size_t size,
	uint64_t alignment)
{
	if (buffer == NULL || buffer->address == NULL || buffer->size != size ||
	    buffer->device_address == 0U || alignment == 0U ||
	    (alignment & (alignment - 1U)) != 0U ||
	    (buffer->device_address & (alignment - 1U)) != 0U)
		return 0;
	if (buffer->device_address > UINT64_MAX - (uint64_t)(size - 1U))
		return 0;
	return 1;
}

static int
ax211_tx_ring_valid(
	const struct intel_ax211_tx_ring *ring)
{
	size_t index;

	if (ring == NULL || !ring->allocated || ring->dma_device == NULL ||
	    ring->ops == NULL || ring->ops->sync_for_device == NULL ||
	    ring->ops->write32 == NULL ||
	    !ax211_tx_ring_buffer_valid(&ring->tfd,
	    INTEL_AX211_TX_RING_TFD_RING_SIZE, 256U) ||
	    !ax211_tx_ring_buffer_valid(&ring->byte_count,
	    INTEL_AX211_TX_RING_BYTE_COUNT_SIZE, 128U) ||
	    ring->pending_count > INTEL_AX211_TX_RING_INFLIGHT_LIMIT)
		return 0;
	for (index = 0U; index < INTEL_AX211_TX_RING_SLOT_COUNT; index++) {
		if (!ax211_tx_ring_buffer_valid(&ring->slot[index].command,
		    INTEL_AX211_TX_RING_COMMAND_DMA_SIZE, 64U) ||
		    !ax211_tx_ring_buffer_valid(&ring->slot[index].payload,
		    INTEL_AX211_TX_RING_PAYLOAD_DMA_SIZE, 4U))
			return 0;
	}
	return 1;
}

static int
ax211_tx_queue_config_valid(
	const struct intel_ax211_tx_ring *ring,
	const struct intel_ax211_tx_queue_config *config)
{
	uint8_t expected[INTEL_AX211_TX_QUEUE_CONFIG_COMMAND_SIZE];

	if (config == NULL || config->station_id >= 32U ||
	    !ax211_tx_queue_tid_valid(config->tid) ||
	    config->tfd_address != ring->tfd.device_address ||
	    config->byte_count_address != ring->byte_count.device_address)
		return 0;
	ax211_tx_queue_command_encode(expected, config->station_id, config->tid,
	    config->byte_count_address, config->tfd_address);
	return memcmp(expected, config->command, sizeof(expected)) == 0;
}

static int
ax211_tx_queue_tid_valid(
	uint8_t tid)
{
	return tid < 8U || tid == INTEL_AX211_TX_RING_MANAGEMENT_TID;
}

static void
ax211_tx_queue_command_encode(
	uint8_t *command,
	uint8_t station_id,
	uint8_t tid,
	uint64_t byte_count_address,
	uint64_t tfd_address)
{
	memset(command, 0, INTEL_AX211_TX_QUEUE_CONFIG_COMMAND_SIZE);
	ax211_tx_ring_put_le32(command, AX211_TX_QUEUE_OPERATION_ADD);
	ax211_tx_ring_put_le32(command + 4U, UINT32_C(1) << station_id);
	command[8U] = tid;
	ax211_tx_ring_put_le32(command + 12U, 0U);
	ax211_tx_ring_put_le32(command + 16U, AX211_TX_QUEUE_CB_SIZE);
	ax211_tx_ring_put_le64(command + 20U, byte_count_address);
	ax211_tx_ring_put_le64(command + 28U, tfd_address);
}

static int
ax211_tx_protocol_result(
	int result)
{
	if (result == INTEL_AX211_PROTOCOL_STALE ||
	    result == INTEL_AX211_PROTOCOL_TOKEN_MISMATCH)
		return INTEL_AX211_TX_RING_STALE;
	if (result == INTEL_AX211_PROTOCOL_UNSUPPORTED)
		return INTEL_AX211_TX_RING_UNSUPPORTED;
	if (result == INTEL_AX211_PROTOCOL_FAILED)
		return INTEL_AX211_TX_RING_TX_FAILED;
	if (result == INTEL_AX211_PROTOCOL_INVALID)
		return INTEL_AX211_TX_RING_INVALID;
	return INTEL_AX211_TX_RING_MALFORMED;
}

static int
ax211_tx_codec_result(
	int result)
{
	if (result == INTEL_AX211_TX_STALE)
		return INTEL_AX211_TX_RING_STALE;
	if (result == INTEL_AX211_TX_UNSUPPORTED)
		return INTEL_AX211_TX_RING_UNSUPPORTED;
	if (result == INTEL_AX211_TX_FAILED)
		return INTEL_AX211_TX_RING_TX_FAILED;
	if (result == INTEL_AX211_TX_INVALID)
		return INTEL_AX211_TX_RING_INVALID;
	return INTEL_AX211_TX_RING_MALFORMED;
}

static int
ax211_tx_ring_cookie_active(
	const struct intel_ax211_tx_ring *ring,
	uint64_t cookie)
{
	size_t index;

	for (index = 0U; index < INTEL_AX211_TX_RING_SLOT_COUNT; index++) {
		if (ring->slot[index].active &&
		    ring->slot[index].handle.cookie == cookie)
			return 1;
	}
	return 0;
}

static int
ax211_tx_ring_slot_stage(
	struct intel_ax211_tx_ring *ring,
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

	command_length = AX211_TX_NARROW_HEADER_SIZE +
	    prepared->command_length;
	if (command_length <= INTEL_AX211_TX_RING_FIRST_TB_SIZE ||
	    command_length > slot->command.size ||
	    command_length - INTEL_AX211_TX_RING_FIRST_TB_SIZE >
	    INTEL_AX211_TX_RING_TB_SIZE_MAX ||
	    prepared->payload_length > slot->payload.size ||
	    prepared->payload_length > INTEL_AX211_TX_RING_TB_SIZE_MAX ||
	    prepared->payload_offset > request->length ||
	    prepared->payload_length >
	    request->length - prepared->payload_offset)
		return INTEL_AX211_TX_RING_MALFORMED;
	command = slot->command.address;
	payload = slot->payload.address;
	tfd = (uint8_t *)ring->tfd.address +
	    (size_t)index * INTEL_AX211_TX_RING_TFD_SIZE;
	byte_count = (uint8_t *)ring->byte_count.address +
	    (size_t)index * sizeof(uint16_t);
	memset(command, 0, slot->command.size);
	memset(payload, 0, slot->payload.size);
	memset(tfd, 0, INTEL_AX211_TX_RING_TFD_SIZE);
	command[0U] = INTEL_AX211_TX_OPCODE;
	command[1U] = 0U;
	command[2U] = index;
	command[3U] = (uint8_t)(ring->queue & 0x1fU);
	memcpy(command + AX211_TX_NARROW_HEADER_SIZE, prepared->command,
	    prepared->command_length);
	if (prepared->payload_length != 0U)
		memcpy(payload, request->frame + prepared->payload_offset,
		    prepared->payload_length);
	num_tbs = prepared->payload_length == 0U ? 2U : 3U;
	ax211_tx_ring_put_le16(tfd,
	    (uint16_t)(num_tbs & AX211_TX_TFD_NUM_TBS_MASK));
	ax211_tx_ring_tfd_tb_encode(tfd, 0U,
	    INTEL_AX211_TX_RING_FIRST_TB_SIZE,
	    slot->command.device_address);
	ax211_tx_ring_tfd_tb_encode(tfd, 1U,
	    (uint16_t)(command_length - INTEL_AX211_TX_RING_FIRST_TB_SIZE),
	    slot->command.device_address +
	    INTEL_AX211_TX_RING_FIRST_TB_SIZE);
	if (prepared->payload_length != 0U)
		ax211_tx_ring_tfd_tb_encode(tfd, 2U,
		    (uint16_t)prepared->payload_length,
		    slot->payload.device_address);
	if (prepared->frame_length > AX211_TX_BC_LENGTH_MASK)
		return INTEL_AX211_TX_RING_MALFORMED;
	ax211_tx_ring_put_le16(byte_count, prepared->frame_length);
	memset(&slot->handle, 0, sizeof(slot->handle));
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
	return INTEL_AX211_TX_RING_OK;
}

static int
ax211_tx_ring_sync_submission(
	struct intel_ax211_tx_ring *ring,
	uint8_t index,
	size_t command_length,
	size_t payload_length)
{
	struct intel_ax211_tx_ring_slot *slot;
	size_t tfd_offset;
	size_t byte_count_offset;

	slot = &ring->slot[index];
	tfd_offset = (size_t)index * INTEL_AX211_TX_RING_TFD_SIZE;
	byte_count_offset = (size_t)index * sizeof(uint16_t);
	if (ring->ops->sync_for_device(ring->ops_argument, &slot->command,
	    0U, command_length) != 0)
		return INTEL_AX211_TX_RING_IO_ERROR;
	if (payload_length != 0U &&
	    ring->ops->sync_for_device(ring->ops_argument, &slot->payload,
	    0U, payload_length) != 0)
		return INTEL_AX211_TX_RING_IO_ERROR;
	if (ring->ops->sync_for_device(ring->ops_argument, &ring->tfd,
	    tfd_offset, INTEL_AX211_TX_RING_TFD_SIZE) != 0)
		return INTEL_AX211_TX_RING_IO_ERROR;
	if (ring->ops->sync_for_device(ring->ops_argument, &ring->byte_count,
	    byte_count_offset, sizeof(uint16_t)) != 0)
		return INTEL_AX211_TX_RING_IO_ERROR;
	return INTEL_AX211_TX_RING_OK;
}

static void
ax211_tx_ring_tfd_tb_encode(
	uint8_t *tfd,
	unsigned tb,
	uint16_t length,
	uint64_t address)
{
	size_t offset;

	offset = AX211_TX_TFD_TB_OFFSET + (size_t)tb * AX211_TX_TFD_TB_SIZE;
	ax211_tx_ring_put_le16(tfd + offset, length);
	ax211_tx_ring_put_le64(tfd + offset + 2U, address);
}

static void
ax211_tx_ring_slot_scrub(
	struct intel_ax211_tx_ring *ring,
	uint8_t index)
{
	struct intel_ax211_tx_ring_slot *slot;
	uint8_t *tfd;
	uint8_t *byte_count;

	slot = &ring->slot[index];
	tfd = (uint8_t *)ring->tfd.address +
	    (size_t)index * INTEL_AX211_TX_RING_TFD_SIZE;
	byte_count = (uint8_t *)ring->byte_count.address +
	    (size_t)index * sizeof(uint16_t);
	ax211_tx_ring_scrub(slot->command.address, slot->command.size);
	ax211_tx_ring_scrub(slot->payload.address, slot->payload.size);
	ax211_tx_ring_scrub(tfd, INTEL_AX211_TX_RING_TFD_SIZE);
	ax211_tx_ring_scrub(byte_count, sizeof(uint16_t));
	memset(&slot->handle, 0, sizeof(slot->handle));
	slot->frame_length = 0U;
	slot->active = 0U;
	slot->uncertain = 0U;
}

static int
ax211_tx_ring_active_sequence(
	const struct intel_ax211_tx_ring *ring,
	uint16_t next_sequence)
{
	uint16_t sequence;
	uint16_t count;

	sequence = ring->read_sequence;
	count = 0U;
	while (count < ring->pending_count) {
		const struct intel_ax211_tx_ring_slot *slot;

		slot = &ring->slot[(uint8_t)sequence];
		if (slot->active &&
		    (uint16_t)(slot->handle.scheduler_sequence + 1U) ==
		    next_sequence)
			return 1;
		sequence = (uint16_t)(sequence + 1U);
		count++;
	}
	return 0;
}

static void
ax211_tx_ring_scrub(
	void *memory,
	size_t length)
{
	volatile uint8_t *bytes;

	if (memory == NULL)
		return;
	bytes = memory;
	while (length != 0U) {
		*bytes++ = 0U;
		length--;
	}
}

static void
ax211_tx_ring_put_le16(
	uint8_t *bytes,
	uint16_t value)
{
	bytes[0U] = (uint8_t)value;
	bytes[1U] = (uint8_t)(value >> 8);
}

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

static void
ax211_tx_ring_put_le64(
	uint8_t *bytes,
	uint64_t value)
{
	ax211_tx_ring_put_le32(bytes, (uint32_t)value);
	ax211_tx_ring_put_le32(bytes + 4U, (uint32_t)(value >> 32));
}

static uint16_t
ax211_tx_ring_get_le16(
	const uint8_t *bytes)
{
	return (uint16_t)bytes[0U] | ((uint16_t)bytes[1U] << 8);
}
