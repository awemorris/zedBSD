/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD Intel AX211 private API89 CCMP key contract
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

#ifndef ZEDBSD_DRIVERS_INTEL_AX211_KEY_H
#define ZEDBSD_DRIVERS_INTEL_AX211_KEY_H

#include <stddef.h>
#include <stdint.h>

#include "intel-ax211-protocol.h"

#define INTEL_AX211_KEY_GROUP                              5U
#define INTEL_AX211_KEY_OPCODE                          0x18U
#define INTEL_AX211_KEY_COMMAND_VERSION                    1U
#define INTEL_AX211_KEY_WIRE_VERSION                       0U
#define INTEL_AX211_KEY_RESPONSE_VERSION                   0U
#define INTEL_AX211_KEY_COMMAND_SIZE                      80U
#define INTEL_AX211_KEY_BYTES                             16U
#define INTEL_AX211_KEY_INDEX_LIMIT                        4U

enum intel_ax211_key_result {
	INTEL_AX211_KEY_OK = 0,
	INTEL_AX211_KEY_INVALID = 1,
	INTEL_AX211_KEY_UNSUPPORTED = 2,
	INTEL_AX211_KEY_STALE = 3,
	INTEL_AX211_KEY_DUPLICATE = 4,
	INTEL_AX211_KEY_MISSING = 5
};

enum intel_ax211_key_kind {
	INTEL_AX211_KEY_PAIRWISE = 1,
	INTEL_AX211_KEY_GROUP_KEY = 2
};

struct intel_ax211_key_request {
	uint64_t connection_generation;
	uint64_t key_generation;
	uint64_t receive_packet_number;
	uint8_t key[INTEL_AX211_KEY_BYTES];
	enum intel_ax211_key_kind kind;
	uint8_t key_index;
	uint8_t reserved[3];
};

/* This state contains identifiers only; it never retains key bytes. */
struct intel_ax211_key_state {
	uint64_t connection_generation;
	uint64_t staged_pairwise;
	uint64_t staged_group[INTEL_AX211_KEY_INDEX_LIMIT];
	uint64_t active_pairwise;
	uint64_t active_group[INTEL_AX211_KEY_INDEX_LIMIT];
	uint32_t hardware_epoch;
	uint8_t active_group_index;
	uint8_t initialized;
	uint8_t reserved[2];
};

int intel_ax211_key_api89_validate(
	const struct intel_ax211_protocol_command_table *table);
int intel_ax211_key_add_encode(const struct intel_ax211_key_request *request,
	uint8_t output[INTEL_AX211_KEY_COMMAND_SIZE]);
int intel_ax211_key_remove_encode(uint64_t connection_generation,
	uint64_t key_generation, enum intel_ax211_key_kind kind,
	uint8_t key_index, uint8_t output[INTEL_AX211_KEY_COMMAND_SIZE]);
void intel_ax211_key_command_scrub(
	uint8_t command[INTEL_AX211_KEY_COMMAND_SIZE]);

int intel_ax211_key_state_init(struct intel_ax211_key_state *state,
	uint32_t hardware_epoch, uint64_t connection_generation);
int intel_ax211_key_state_installed(struct intel_ax211_key_state *state,
	const struct intel_ax211_key_request *request,
	uint32_t hardware_epoch);
int intel_ax211_key_state_activate(struct intel_ax211_key_state *state,
	uint64_t connection_generation, uint64_t pairwise_generation,
	uint64_t group_generation, uint32_t hardware_epoch);
int intel_ax211_key_state_removed(struct intel_ax211_key_state *state,
	uint64_t connection_generation, enum intel_ax211_key_kind kind,
	uint8_t key_index, uint64_t key_generation,
	uint32_t hardware_epoch);
int intel_ax211_key_state_rx_generation(
	const struct intel_ax211_key_state *state,
	uint64_t connection_generation, enum intel_ax211_key_kind kind,
	uint8_t key_index, uint32_t hardware_epoch, uint64_t *key_generation);
int intel_ax211_key_state_tx_validate(
	const struct intel_ax211_key_state *state,
	uint64_t connection_generation, uint64_t key_generation,
	uint8_t key_index, uint64_t packet_number, uint32_t hardware_epoch);

#endif
