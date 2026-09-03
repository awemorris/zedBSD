/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD Intel AX211 private API89 transmit contract
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
 */

#ifndef ZEDBSD_DRIVERS_INTEL_AX211_TX_H
#define ZEDBSD_DRIVERS_INTEL_AX211_TX_H

#include <stddef.h>
#include <stdint.h>

#include "intel-ax211-protocol.h"

#define INTEL_AX211_TX_GROUP                              1U
#define INTEL_AX211_TX_OPCODE                          0x1cU
#define INTEL_AX211_TX_COMMAND_VERSION                    10U
#define INTEL_AX211_TX_NOTIFICATION_VERSION                7U

#define INTEL_AX211_TX_COMMAND_FIXED_SIZE                 28U
#define INTEL_AX211_TX_MAC_HEADER_MAX                     36U
#define INTEL_AX211_TX_COMMAND_BUFFER_SIZE \
	(INTEL_AX211_TX_COMMAND_FIXED_SIZE + \
	INTEL_AX211_TX_MAC_HEADER_MAX + 2U)
#define INTEL_AX211_TX_FRAME_MAX                        2304U
#define INTEL_AX211_TX_QUEUE_SIZE                        256U
#define INTEL_AX211_TX_QUEUE_MIN                           1U
#define INTEL_AX211_TX_QUEUE_MAX                         511U
#define INTEL_AX211_TX_SCHEDULER_SEQUENCE_LIMIT        65536U

enum intel_ax211_tx_result {
	INTEL_AX211_TX_OK = 0,
	INTEL_AX211_TX_INVALID = 1,
	INTEL_AX211_TX_UNSUPPORTED = 2,
	INTEL_AX211_TX_TRUNCATED = 3,
	INTEL_AX211_TX_OVERSIZED = 4,
	INTEL_AX211_TX_STALE = 5,
	INTEL_AX211_TX_FAILED = 6,
	INTEL_AX211_TX_BUFFER_TOO_SMALL = 7
};

enum intel_ax211_tx_frame_class {
	INTEL_AX211_TX_FRAME_MANAGEMENT = 1,
	INTEL_AX211_TX_FRAME_EAPOL = 2,
	INTEL_AX211_TX_FRAME_DATA = 3
};

struct intel_ax211_tx_request {
	uint64_t connection_generation;
	uint64_t cookie;
	uint64_t key_generation;
	uint64_t packet_number;
	const uint8_t *frame;
	size_t length;
	enum intel_ax211_tx_frame_class frame_class;
	uint8_t encrypted;
	uint8_t key_index;
	uint8_t band_5ghz;
	uint8_t reserved[5];
};

/*
 * command contains the Gen3 v10 fixed body, the copied 802.11 header, and
 * optional alignment padding.  payload_offset/payload_length select the
 * caller-owned frame bytes which form the following TFD transfer buffer.
 * For a protected CCMP frame the common WLAN boundary supplies and validates
 * the eight-byte IV, but Intel firmware inserts that IV and the MIC from its
 * installed key state.  The prepared transfer therefore skips the supplied
 * IV and frame_length is the firmware input length with that IV removed.
 */
struct intel_ax211_tx_prepared {
	uint8_t command[INTEL_AX211_TX_COMMAND_BUFFER_SIZE];
	size_t command_length;
	size_t payload_offset;
	size_t payload_length;
	uint64_t connection_generation;
	uint64_t cookie;
	uint64_t key_generation;
	uint64_t packet_number;
	uint16_t frame_length;
	uint8_t encrypted;
	uint8_t key_index;
};

struct intel_ax211_tx_completion {
	uint32_t hardware_generation;
	uint32_t scheduler_sequence;
	uint16_t queue;
	uint16_t sequence_control;
	uint16_t byte_count;
	uint8_t index;
	uint8_t acknowledged;
	uint8_t failure_rts;
	uint8_t failure_frame;
};

int intel_ax211_tx_api89_validate(
	const struct intel_ax211_protocol_command_table *table);
int intel_ax211_tx_prepare(const struct intel_ax211_tx_request *request,
	struct intel_ax211_tx_prepared *prepared);
int intel_ax211_tx_completion_decode(
	const struct intel_ax211_protocol_message *message,
	uint32_t hardware_generation, uint16_t expected_queue,
	uint8_t expected_index, struct intel_ax211_tx_completion *completion);

#endif
