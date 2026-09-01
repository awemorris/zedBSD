/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD Intel AX211 private Gen3 transport contract
 *
 * Portions derived from OpenBSD sys/dev/pci/if_iwxreg.h and if_iwx.c at
 * commit 0f464d413c50396e4e6cd70948f15613d6a73081.
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
 */

#ifndef ZEDBSD_DRIVERS_INTEL_AX211_TRANSPORT_H
#define ZEDBSD_DRIVERS_INTEL_AX211_TRANSPORT_H

#include "intel-ax211-internal.h"
#include "intel-ax211-mmio.h"

#include <stddef.h>
#include <stdint.h>

#define INTEL_AX211_TRANSPORT_COMMAND_SLOT_SIZE             324U
#define INTEL_AX211_TRANSPORT_COMMAND_INLINE_PAYLOAD_MAX     316U
#define INTEL_AX211_TRANSPORT_COMMAND_EXTERNAL_SIZE         4096U
#define INTEL_AX211_TRANSPORT_COMMAND_EXTERNAL_PAYLOAD_MAX  4088U
#define INTEL_AX211_TRANSPORT_RX_DESCRIPTOR_COUNT            512U
#define INTEL_AX211_TRANSPORT_RX_DESCRIPTOR_SIZE              16U
#define INTEL_AX211_TRANSPORT_RX_COMPLETION_SIZE              32U

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

enum intel_ax211_transport_dma_region {
	INTEL_AX211_TRANSPORT_DMA_COMMAND_TFD = 1,
	INTEL_AX211_TRANSPORT_DMA_COMMAND_BYTE_COUNT = 2,
	INTEL_AX211_TRANSPORT_DMA_COMMAND_SLOTS = 3,
	INTEL_AX211_TRANSPORT_DMA_RX_TRANSFER = 4,
	INTEL_AX211_TRANSPORT_DMA_RX_COMPLETION = 5,
	INTEL_AX211_TRANSPORT_DMA_RX_STATUS = 6,
	INTEL_AX211_TRANSPORT_DMA_COMMAND_EXTERNAL = 7
};

enum intel_ax211_transport_dma_direction {
	INTEL_AX211_TRANSPORT_DMA_PREWRITE = 1,
	INTEL_AX211_TRANSPORT_DMA_PREREAD = 2,
	INTEL_AX211_TRANSPORT_DMA_POSTREAD = 3
};

enum intel_ax211_transport_wait {
	INTEL_AX211_TRANSPORT_WAIT_RX_IDLE = 1
};

struct intel_ax211_transport_ops {
	int (*csr_read32)(void *argument, uint32_t offset, uint32_t *value);
	int (*csr_write32)(void *argument, uint32_t offset, uint32_t value);
	int (*csr_write8)(void *argument, uint32_t offset, uint8_t value);
	int (*nic_lock)(void *argument);
	int (*nic_unlock)(void *argument);
	int (*prph_read32)(void *argument, uint32_t address, uint32_t *value);
	int (*prph_write32)(void *argument, uint32_t address, uint32_t value);
	int (*dma_sync)(void *argument,
		enum intel_ax211_transport_dma_region region, size_t offset,
		size_t length, enum intel_ax211_transport_dma_direction direction);
	int (*delay_us)(void *argument, uint32_t duration_us);
	int (*clock_us)(void *argument, uint64_t *time_us);
	void (*trace_deadline)(void *argument,
		enum intel_ax211_transport_wait wait, uint64_t start_us,
		uint64_t deadline_us);
};

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

struct intel_ax211_transport_causes {
	uint32_t flow_handler;
	uint32_t hardware;
};

struct intel_ax211_transport_rx_completion {
	uint16_t completion_index;
	uint16_t buffer_id;
	uint8_t flags;
};

struct intel_ax211_transport {
	const struct intel_ax211_transport_ops *ops;
	void *argument;
	struct intel_ax211_mmio_profile profile;
	struct intel_ax211_transport_ring_memory memory;
	struct intel_ax211_ring command_ring;
	struct intel_ax211_ring_token command_prepared_token;
	struct intel_ax211_ring_token command_external_token;
	uint32_t enabled_fh_causes;
	uint32_t enabled_hw_causes;
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

int intel_ax211_transport_init(struct intel_ax211_transport *transport,
	const struct intel_ax211_transport_ops *ops, void *argument,
	const struct intel_ax211_mmio_profile *profile,
	const struct intel_ax211_transport_ring_memory *memory);
int intel_ax211_transport_configure_msix(struct intel_ax211_transport *transport);
int intel_ax211_transport_initialize_rings(struct intel_ax211_transport *transport);
int intel_ax211_transport_enable_firmware_interrupts(struct intel_ax211_transport *transport);
int intel_ax211_transport_enable_runtime_interrupts(struct intel_ax211_transport *transport);
int intel_ax211_transport_disable_interrupts(struct intel_ax211_transport *transport);
int intel_ax211_transport_interrupt_claim(struct intel_ax211_transport *transport,
	struct intel_ax211_transport_causes *causes);
int intel_ax211_transport_interrupt_rearm(struct intel_ax211_transport *transport);
int intel_ax211_transport_publish_rx_descriptor(struct intel_ax211_transport *transport,
	uint16_t index, uint64_t device_address);
int intel_ax211_transport_activate_rx(struct intel_ax211_transport *transport);
int intel_ax211_transport_rx_refresh(struct intel_ax211_transport *transport);
int intel_ax211_transport_rx_next(struct intel_ax211_transport *transport,
	struct intel_ax211_transport_rx_completion *completion);
int intel_ax211_transport_rx_replenish(struct intel_ax211_transport *transport,
	uint64_t device_address);
int intel_ax211_transport_command_prepare_inline(
	struct intel_ax211_transport *transport,
	const struct intel_ax211_command_id *command, const void *payload,
	size_t payload_length, struct intel_ax211_ring_token *token);
int intel_ax211_transport_command_prepare_external(
	struct intel_ax211_transport *transport,
	const struct intel_ax211_command_id *command, const void *payload,
	size_t payload_length, struct intel_ax211_ring_token *token);
int intel_ax211_transport_command_publish(
	struct intel_ax211_transport *transport,
	const struct intel_ax211_ring_token *token);
int intel_ax211_transport_command_abort_prepared(
	struct intel_ax211_transport *transport,
	const struct intel_ax211_ring_token *token);
int intel_ax211_transport_command_submit_inline(struct intel_ax211_transport *transport,
	const struct intel_ax211_command_id *command, const void *payload,
	size_t payload_length, struct intel_ax211_ring_token *token);
int intel_ax211_transport_command_complete(struct intel_ax211_transport *transport,
	const struct intel_ax211_ring_token *token);
size_t intel_ax211_transport_command_pending_count(
	const struct intel_ax211_transport *transport);
int intel_ax211_transport_command_oldest(
	const struct intel_ax211_transport *transport,
	struct intel_ax211_ring_token *token);
int intel_ax211_transport_command_after_device_reset(
	struct intel_ax211_transport *transport);
int intel_ax211_transport_quiesce(struct intel_ax211_transport *transport);

#endif
