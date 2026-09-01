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

#ifndef ZEDBSD_DRIVERS_INTEL_AX211_TX_RING_H
#define ZEDBSD_DRIVERS_INTEL_AX211_TX_RING_H

#include <drivers/dma.h>

#include <stddef.h>
#include <stdint.h>

#include "intel-ax211-protocol.h"
#include "intel-ax211-tx.h"

#define INTEL_AX211_TX_RING_QUEUE                         1U
#define INTEL_AX211_TX_RING_MANAGEMENT_TID               15U
#define INTEL_AX211_TX_RING_SLOT_COUNT                  256U
/* Intel documents that at most 255 of the 256 TFDs may be outstanding. */
#define INTEL_AX211_TX_RING_INFLIGHT_LIMIT              255U
#define INTEL_AX211_TX_RING_TFD_SIZE                    256U
#define INTEL_AX211_TX_RING_TFD_RING_SIZE             65536U
#define INTEL_AX211_TX_RING_BYTE_COUNT_ENTRIES         1024U
#define INTEL_AX211_TX_RING_BYTE_COUNT_SIZE            2048U
#define INTEL_AX211_TX_RING_COMMAND_DMA_SIZE            128U
#define INTEL_AX211_TX_RING_PAYLOAD_DMA_SIZE           4092U
#define INTEL_AX211_TX_RING_FIRST_TB_SIZE                 20U
#define INTEL_AX211_TX_RING_TB_SIZE_MAX                 4092U

#define INTEL_AX211_TX_RING_WRITE_POINTER_REGISTER     0x460U

#define INTEL_AX211_TX_QUEUE_CONFIG_GROUP                 5U
#define INTEL_AX211_TX_QUEUE_CONFIG_OPCODE             0x17U
#define INTEL_AX211_TX_QUEUE_CONFIG_COMMAND_VERSION       3U
#define INTEL_AX211_TX_QUEUE_CONFIG_RESPONSE_VERSION      2U
#define INTEL_AX211_TX_QUEUE_CONFIG_COMMAND_SIZE          36U
#define INTEL_AX211_TX_QUEUE_CONFIG_RESPONSE_SIZE          8U

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
 * All callbacks run while the owning controller serializes the TX ring.
 * sync_for_device must establish coherent-DMA visibility before returning.
 */
struct intel_ax211_tx_ring_ops {
	int (*sync_for_device)(void *argument,
	    const struct drv_dma_buffer *buffer, size_t offset, size_t length);
	int (*write32)(void *argument, uint32_t offset, uint32_t value);
};

struct intel_ax211_tx_queue_config {
	uint8_t command[INTEL_AX211_TX_QUEUE_CONFIG_COMMAND_SIZE];
	uint64_t tfd_address;
	uint64_t byte_count_address;
	uint16_t expected_write_pointer;
	uint8_t station_id;
	uint8_t tid;
};

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

struct intel_ax211_tx_ring_retired {
	struct intel_ax211_tx_ring_handle handle;
	uint16_t sequence_control;
	uint16_t byte_count;
	uint8_t acknowledged;
	uint8_t failure_rts;
	uint8_t failure_frame;
	uint8_t reserved;
};

struct intel_ax211_tx_ring_slot {
	struct drv_dma_buffer command;
	struct drv_dma_buffer payload;
	struct intel_ax211_tx_ring_handle handle;
	uint16_t frame_length;
	uint8_t active;
	uint8_t uncertain;
};

/* Private controller-owned object; do not copy after allocation. */
struct intel_ax211_tx_ring {
	struct drv_dma_device *dma_device;
	const struct intel_ax211_tx_ring_ops *ops;
	void *ops_argument;
	struct drv_dma_buffer tfd;
	struct drv_dma_buffer byte_count;
	struct intel_ax211_tx_ring_slot slot[INTEL_AX211_TX_RING_SLOT_COUNT];
	uint64_t connection_generation;
	uint32_t hardware_generation;
	uint16_t read_sequence;
	uint16_t write_sequence;
	uint16_t last_completion_sequence;
	uint16_t pending_count;
	uint8_t station_id;
	uint8_t tid;
	uint8_t allocated;
	uint8_t enabled;
	uint8_t poisoned;
	uint8_t reset_barrier_required;
	uint8_t has_last_completion;
	uint8_t reserved[2];
};

int intel_ax211_tx_ring_api89_validate(
	const struct intel_ax211_protocol_command_table *table);

int intel_ax211_tx_ring_allocate(struct drv_dma_device *dma_device,
	const struct intel_ax211_tx_ring_ops *ops, void *ops_argument,
	struct intel_ax211_tx_ring *ring);

int intel_ax211_tx_ring_queue_add_build(
	const struct intel_ax211_tx_ring *ring, uint8_t station_id, uint8_t tid,
	uint16_t expected_write_pointer,
	struct intel_ax211_tx_queue_config *config);

int intel_ax211_tx_ring_queue_add_complete(struct intel_ax211_tx_ring *ring,
	const struct intel_ax211_tx_queue_config *config,
	uint32_t hardware_generation, uint64_t connection_generation,
	const struct intel_ax211_protocol_message *message,
	const struct intel_ax211_protocol_pending_command *pending);

int intel_ax211_tx_ring_submit(struct intel_ax211_tx_ring *ring,
	const struct intel_ax211_tx_request *request, uint64_t now,
	uint64_t timeout, struct intel_ax211_tx_ring_handle *handle);

int intel_ax211_tx_ring_complete(struct intel_ax211_tx_ring *ring,
	const struct intel_ax211_protocol_message *message,
	struct intel_ax211_tx_ring_retired *retired);

int intel_ax211_tx_ring_timeout_oldest(struct intel_ax211_tx_ring *ring,
	uint64_t now, struct intel_ax211_tx_ring_handle *handle);

/* dma_quiesced is proof that queue 1 can no longer access host memory. */
int intel_ax211_tx_ring_reset(struct intel_ax211_tx_ring *ring,
	int dma_quiesced);

/* Release has the same barrier rule and frees allocations in reverse order. */
int intel_ax211_tx_ring_release(struct intel_ax211_tx_ring *ring,
	int dma_quiesced);

#endif
