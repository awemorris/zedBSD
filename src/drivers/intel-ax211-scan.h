/*
 * zedBSD Intel AX211 private API89 passive-scan contract
 *
 * Portions derived from OpenBSD sys/dev/pci/if_iwxreg.h and if_iwx.c.
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
 * OpenBSD source provenance:
 *   sys/dev/pci/if_iwx.c, if_iwxreg.h at
 *   0f464d413c50396e4e6cd70948f15613d6a73081
 */

#ifndef ZEDBSD_DRIVERS_INTEL_AX211_SCAN_H
#define ZEDBSD_DRIVERS_INTEL_AX211_SCAN_H

#include <stddef.h>
#include <stdint.h>

#include "intel-ax211-protocol.h"
#include "intel-ax211-runtime.h"

#define INTEL_AX211_SCAN_GROUP_LONG                         1U
#define INTEL_AX211_SCAN_GROUP_LEGACY                       0U
#define INTEL_AX211_SCAN_REQUEST_OPCODE                  0x0dU
#define INTEL_AX211_SCAN_ABORT_OPCODE                    0x0eU
#define INTEL_AX211_SCAN_COMPLETE_OPCODE                 0x0fU
#define INTEL_AX211_SCAN_ITERATION_COMPLETE_OPCODE       0xb5U

#define INTEL_AX211_SCAN_CONFIG_VERSION                     5U
#define INTEL_AX211_SCAN_REQUEST_VERSION                    17U
#define INTEL_AX211_SCAN_ABORT_VERSION                       1U
#define INTEL_AX211_SCAN_NOTIFICATION_VERSION                1U

#define INTEL_AX211_SCAN_UID                                 0U
#define INTEL_AX211_SCAN_PRIORITY                            6U
#define INTEL_AX211_SCAN_ACTIVE_DWELL                       10U
#define INTEL_AX211_SCAN_PASSIVE_DWELL                     110U
#define INTEL_AX211_SCAN_ADAPTIVE_BUDGET                   300U
#define INTEL_AX211_SCAN_CHANNEL_WIDTH_MHZ                  20U
#define INTEL_AX211_SCAN_CHANNEL_LIMIT                      51U
#define INTEL_AX211_SCAN_FIRMWARE_CHANNEL_LIMIT             67U
#define INTEL_AX211_SCAN_PROBE_LIMIT                       512U
#define INTEL_AX211_SCAN_REQUEST_SIZE                     1940U
#define INTEL_AX211_SCAN_ITERATION_NOTIFICATION_SIZE       912U
#define INTEL_AX211_SCAN_ABORT_SIZE                          8U
#define INTEL_AX211_SCAN_ACK_TIMEOUT_US                1000000U
#define INTEL_AX211_SCAN_WATCHDOG_US                   5000000U

enum intel_ax211_scan_result {
	INTEL_AX211_SCAN_OK = 0,
	INTEL_AX211_SCAN_INVALID = 1,
	INTEL_AX211_SCAN_UNSUPPORTED = 2,
	INTEL_AX211_SCAN_TRUNCATED = 3,
	INTEL_AX211_SCAN_OVERSIZED = 4,
	INTEL_AX211_SCAN_STALE = 5,
	INTEL_AX211_SCAN_DUPLICATE = 6,
	INTEL_AX211_SCAN_OUT_OF_ORDER = 7,
	INTEL_AX211_SCAN_TIMEOUT = 8,
	INTEL_AX211_SCAN_COMPLETE = 9,
	INTEL_AX211_SCAN_ABORTED = 10,
	INTEL_AX211_SCAN_FAILED = 11
};

enum intel_ax211_scan_phase {
	INTEL_AX211_SCAN_PHASE_IDLE = 0,
	INTEL_AX211_SCAN_PHASE_WAIT_ACK = 1,
	INTEL_AX211_SCAN_PHASE_RUNNING = 2,
	INTEL_AX211_SCAN_PHASE_TERMINAL = 3
};

enum intel_ax211_scan_event_kind {
	INTEL_AX211_SCAN_EVENT_COMPLETE = 1,
	INTEL_AX211_SCAN_EVENT_ITERATION_COMPLETE = 2
};

struct intel_ax211_scan_profile {
	uint8_t station_address[6];
	uint8_t channel_width_mhz;
	uint8_t channel[INTEL_AX211_SCAN_CHANNEL_LIMIT];
	size_t channel_count;
};

struct intel_ax211_scan_channel_result {
	uint8_t channel;
	uint8_t probe_status;
	uint8_t probe_not_sent;
	uint32_t duration;
};

struct intel_ax211_scan_event {
	enum intel_ax211_scan_event_kind kind;
	uint8_t status;
	uint8_t ebs_status;
	uint8_t last_schedule;
	uint8_t last_iteration;
	uint8_t last_channel;
	uint8_t bluetooth_status;
	uint64_t tsf;
	size_t channel_count;
	struct intel_ax211_scan_channel_result
	    channel[INTEL_AX211_SCAN_CHANNEL_LIMIT];
};

struct intel_ax211_scan_state {
	uint32_t generation;
	uint64_t acknowledgement_deadline;
	uint64_t scan_deadline;
	uint8_t requested_channels[32U];
	enum intel_ax211_scan_phase phase;
	uint8_t abort_required;
};

int intel_ax211_scan_profile_from_nvm(
	const struct intel_ax211_protocol_nvm *nvm,
	const struct intel_ax211_runtime_mcc *mcc,
	const uint8_t station_address[6],
	struct intel_ax211_scan_profile *profile);
int intel_ax211_scan_api89_validate(
	const struct intel_ax211_protocol_command_table *table);
int intel_ax211_scan_request_encode(
	const struct intel_ax211_scan_profile *profile,
	uint8_t output[INTEL_AX211_SCAN_REQUEST_SIZE]);
int intel_ax211_scan_abort_encode(
	uint8_t output[INTEL_AX211_SCAN_ABORT_SIZE]);

int intel_ax211_scan_begin(struct intel_ax211_scan_state *state,
	const struct intel_ax211_protocol_command_table *table,
	const struct intel_ax211_scan_profile *profile,
	uint32_t generation, uint64_t now_us);
int intel_ax211_scan_request_ack(struct intel_ax211_scan_state *state,
	uint32_t generation, uint64_t now_us);
int intel_ax211_scan_event_accept(struct intel_ax211_scan_state *state,
	const struct intel_ax211_protocol_message *message,
	uint64_t now_us, struct intel_ax211_scan_event *event);
int intel_ax211_scan_expire(struct intel_ax211_scan_state *state,
	uint64_t now_us);

#endif
