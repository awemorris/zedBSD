/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD Intel AX211 private BSS association-metadata contract
 *
 * Management-frame layout and information-element constants are derived
 * from OpenBSD sys/net80211/ieee80211.h and sys/dev/pci/if_iwx.c at commit
 * 0f464d413c50396e4e6cd70948f15613d6a73081.
 * Copyright (c) 2001 Atsushi Onoe
 * Copyright (c) 2002-2008 Sam Leffler, Errno Consulting
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

#ifndef ZEDBSD_DRIVERS_INTEL_AX211_BSS_H
#define ZEDBSD_DRIVERS_INTEL_AX211_BSS_H

#include <stddef.h>
#include <stdint.h>

#include "intel-ax211-rx.h"

/*
 * Keep every BSS which the common WLAN snapshot may offer back to connect().
 * A smaller private cache can evict the selected BSSID while it is still
 * present in the common 64-entry scan snapshot.
 */
#define INTEL_AX211_BSS_CACHE_LIMIT                        64U
#define INTEL_AX211_BSS_ADDRESS_SIZE                        6U

enum intel_ax211_bss_result {
	INTEL_AX211_BSS_OK = 0,
	INTEL_AX211_BSS_INVALID = 1,
	INTEL_AX211_BSS_UNSUPPORTED = 2,
	INTEL_AX211_BSS_TRUNCATED = 3,
	INTEL_AX211_BSS_MALFORMED = 4,
	INTEL_AX211_BSS_STALE = 5,
	INTEL_AX211_BSS_NOT_FOUND = 6
};

enum intel_ax211_bss_source {
	INTEL_AX211_BSS_SOURCE_PROBE_RESPONSE = 1,
	INTEL_AX211_BSS_SOURCE_BEACON = 2
};

/*
 * This private record intentionally retains neither an SSID nor credentials.
 * observation_generation records the scan operation which observed the BSS;
 * hardware_epoch independently identifies the firmware boot which produced
 * the RX descriptor.  A later connection has its own common generation.
 */
struct intel_ax211_bss_entry {
	uint8_t bssid[INTEL_AX211_BSS_ADDRESS_SIZE];
	uint64_t observation_generation;
	uint64_t last_seen_ticks;
	uint64_t frame_timestamp;
	uint64_t receive_tsf;
	uint32_t hardware_epoch;
	uint32_t gp2_on_air_rise;
	int32_t rssi_dbm;
	uint16_t beacon_interval_tu;
	uint16_t capability;
	uint8_t channel;
	uint8_t dtim_count;
	uint8_t dtim_period;
	uint8_t tim_valid;
	uint8_t wmm_present;
	uint8_t receive_tsf_valid;
	uint8_t source;
	uint8_t valid;
};

struct intel_ax211_bss_cache {
	struct intel_ax211_bss_entry entry[INTEL_AX211_BSS_CACHE_LIMIT];
	uint32_t hardware_epoch;
	size_t count;
	uint8_t initialized;
};

/*
 * Association-side values only.  Queue DMA addresses and other hardware-owned
 * state are deliberately absent.  tim_valid tells the caller whether the
 * DTIM fields can be used for the post-authentication association update.
 */
struct intel_ax211_bss_assoc_metadata {
	uint8_t bssid[INTEL_AX211_BSS_ADDRESS_SIZE];
	uint64_t common_generation;
	uint64_t observation_generation;
	uint64_t beacon_tsf;
	uint64_t receive_tsf;
	uint32_t hardware_epoch;
	uint32_t beacon_arrive_time;
	uint16_t beacon_interval_tu;
	uint16_t capability;
	uint8_t channel;
	uint8_t dtim_count;
	uint8_t dtim_period;
	uint8_t tim_valid;
	uint8_t wmm_present;
	uint8_t receive_tsf_valid;
};

int intel_ax211_bss_decode(const struct intel_ax211_rx_mpdu *mpdu,
	uint64_t observation_generation, uint32_t hardware_epoch,
	struct intel_ax211_bss_entry *entry);
int intel_ax211_bss_cache_init(struct intel_ax211_bss_cache *cache,
	uint32_t hardware_epoch);
int intel_ax211_bss_cache_observe(struct intel_ax211_bss_cache *cache,
	const struct intel_ax211_bss_entry *entry);
int intel_ax211_bss_cache_lookup(const struct intel_ax211_bss_cache *cache,
	const uint8_t bssid[INTEL_AX211_BSS_ADDRESS_SIZE], uint8_t channel,
	uint32_t hardware_epoch,
	struct intel_ax211_bss_entry *entry);
int intel_ax211_bss_assoc_metadata(const struct intel_ax211_bss_entry *entry,
	uint64_t connection_generation, uint32_t hardware_epoch,
	struct intel_ax211_bss_assoc_metadata *metadata);

#endif
