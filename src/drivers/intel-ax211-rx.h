/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD Intel AX211 private API89 receive contract
 *
 * Wire constants and descriptor layout are derived from OpenBSD
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

#ifndef ZEDBSD_DRIVERS_INTEL_AX211_RX_H
#define ZEDBSD_DRIVERS_INTEL_AX211_RX_H

#include <stddef.h>
#include <stdint.h>

#include "intel-ax211-protocol.h"

#define INTEL_AX211_RX_MPDU_GROUP                         0U
#define INTEL_AX211_RX_MPDU_OPCODE                     0xc1U
#define INTEL_AX211_RX_MPDU_NOTIFICATION_VERSION          5U
#define INTEL_AX211_RX_MPDU_DESCRIPTOR_SIZE              64U
#define INTEL_AX211_RX_MPDU_FRAME_MAX                  4032U

#define INTEL_AX211_RX_CIPHER_NONE                        0U
#define INTEL_AX211_RX_CIPHER_CCMP                        1U

enum intel_ax211_rx_result {
	INTEL_AX211_RX_OK = 0,
	INTEL_AX211_RX_INVALID = 1,
	INTEL_AX211_RX_UNSUPPORTED = 2,
	INTEL_AX211_RX_TRUNCATED = 3,
	INTEL_AX211_RX_OVERSIZED = 4,
	INTEL_AX211_RX_FAILED = 5,
	INTEL_AX211_RX_DUPLICATE = 6,
	INTEL_AX211_RX_BUFFER_TOO_SMALL = 7,
	INTEL_AX211_RX_STALE = 8
};

/* The frame pointer refers to the caller-owned output buffer. */
struct intel_ax211_rx_mpdu {
	const uint8_t *frame;
	size_t length;
	uint64_t packet_number;
	uint64_t tsf;
	uint32_t gp2_on_air_rise;
	uint32_t status;
	int32_t rssi_dbm;
	uint8_t channel;
	uint8_t cipher;
	uint8_t decrypted;
	uint8_t key_index;
	uint8_t tsf_valid;
};

int intel_ax211_rx_api89_validate(
	const struct intel_ax211_protocol_command_table *table);
int intel_ax211_rx_mpdu_decode(
	const struct intel_ax211_protocol_message *message,
	uint32_t generation, uint8_t *output, size_t output_capacity,
	struct intel_ax211_rx_mpdu *mpdu);

#endif
