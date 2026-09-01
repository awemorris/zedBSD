/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD Intel AX211 private BSS association-metadata implementation
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

#include "intel-ax211-bss.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define AX211_BSS_FRAME_VERSION_MASK                  0x0003U
#define AX211_BSS_FRAME_TYPE_MASK                     0x000cU
#define AX211_BSS_FRAME_TYPE_MANAGEMENT               0x0000U
#define AX211_BSS_FRAME_SUBTYPE_MASK                  0x00f0U
#define AX211_BSS_FRAME_SUBTYPE_PROBE_RESPONSE        0x0050U
#define AX211_BSS_FRAME_SUBTYPE_BEACON                0x0080U
#define AX211_BSS_FRAME_TO_FROM_DS                    0x0300U
#define AX211_BSS_FRAME_PROTECTED                     0x4000U

#define AX211_BSS_FIXED_BODY_OFFSET                       24U
#define AX211_BSS_INFORMATION_ELEMENTS_OFFSET             36U
#define AX211_BSS_BSSID_OFFSET                            16U

#define AX211_BSS_IE_SSID                                  0U
#define AX211_BSS_IE_TIM                                   5U
#define AX211_BSS_IE_DS_PARAMETER                          3U
#define AX211_BSS_IE_HT_OPERATION                         61U
#define AX211_BSS_IE_VENDOR                              221U
#define AX211_BSS_SSID_MAX                                32U
#define AX211_BSS_HT_OPERATION_SIZE                       22U
#define AX211_BSS_TIM_MIN_SIZE                             4U
#define AX211_BSS_WMM_INFO_SIZE                            7U
#define AX211_BSS_WMM_PARAMETER_SIZE                      24U
#define AX211_BSS_WMM_TYPE                                 2U
#define AX211_BSS_WMM_INFO_SUBTYPE                         0U
#define AX211_BSS_WMM_PARAMETER_SUBTYPE                    1U
#define AX211_BSS_WMM_VERSION                              1U

struct ax211_bss_ie_state {
	uint8_t channel;
	uint8_t dtim_count;
	uint8_t dtim_period;
	uint8_t ds_seen;
	uint8_t ht_seen;
	uint8_t ssid_seen;
	uint8_t tim_seen;
	uint8_t wmm_present;
};

static uint16_t ax211_bss_get_le16(const uint8_t *bytes);
static uint64_t ax211_bss_get_le64(const uint8_t *bytes);
static int ax211_bss_address_valid(const uint8_t address[6]);
static int ax211_bss_entry_valid(const struct intel_ax211_bss_entry *entry);
static int ax211_bss_entry_worse(const struct intel_ax211_bss_entry *left,
	const struct intel_ax211_bss_entry *right);
static int ax211_bss_channel_observe(struct ax211_bss_ie_state *state,
	uint8_t channel, int ht);
static int ax211_bss_wmm_observe(struct ax211_bss_ie_state *state,
	const uint8_t *data, size_t length);
static int ax211_bss_ies_decode(const uint8_t *bytes, size_t length,
	struct ax211_bss_ie_state *state);

int
intel_ax211_bss_decode(
	const struct intel_ax211_rx_mpdu *mpdu,
	uint64_t observation_generation,
	uint32_t hardware_epoch,
	struct intel_ax211_bss_entry *entry)
{
	struct intel_ax211_bss_entry decoded;
	struct ax211_bss_ie_state ies;
	const uint8_t *frame;
	uint16_t frame_control;
	uint16_t subtype;
	int result;

	if (mpdu == NULL || entry == NULL || mpdu->frame == NULL ||
	    observation_generation == 0U || hardware_epoch == 0U)
		return INTEL_AX211_BSS_INVALID;
	if (mpdu->channel == 0U || mpdu->length <
	    AX211_BSS_INFORMATION_ELEMENTS_OFFSET)
		return INTEL_AX211_BSS_TRUNCATED;
	frame = mpdu->frame;
	frame_control = ax211_bss_get_le16(frame);
	if ((frame_control & AX211_BSS_FRAME_VERSION_MASK) != 0U ||
	    (frame_control & AX211_BSS_FRAME_TYPE_MASK) !=
	    AX211_BSS_FRAME_TYPE_MANAGEMENT)
		return INTEL_AX211_BSS_UNSUPPORTED;
	if ((frame_control & (AX211_BSS_FRAME_TO_FROM_DS |
	    AX211_BSS_FRAME_PROTECTED)) != 0U)
		return INTEL_AX211_BSS_MALFORMED;
	subtype = frame_control & AX211_BSS_FRAME_SUBTYPE_MASK;
	if (subtype != AX211_BSS_FRAME_SUBTYPE_BEACON &&
	    subtype != AX211_BSS_FRAME_SUBTYPE_PROBE_RESPONSE)
		return INTEL_AX211_BSS_UNSUPPORTED;
	if (!ax211_bss_address_valid(frame + AX211_BSS_BSSID_OFFSET))
		return INTEL_AX211_BSS_MALFORMED;

	memset(&ies, 0, sizeof(ies));
	result = ax211_bss_ies_decode(frame +
	    AX211_BSS_INFORMATION_ELEMENTS_OFFSET,
	    mpdu->length - AX211_BSS_INFORMATION_ELEMENTS_OFFSET, &ies);
	if (result != INTEL_AX211_BSS_OK)
		return result;
	if (ies.channel != 0U && ies.channel != mpdu->channel)
		return INTEL_AX211_BSS_MALFORMED;

	memset(&decoded, 0, sizeof(decoded));
	memcpy(decoded.bssid, frame + AX211_BSS_BSSID_OFFSET,
	    sizeof(decoded.bssid));
	decoded.observation_generation = observation_generation;
	decoded.frame_timestamp = ax211_bss_get_le64(frame +
	    AX211_BSS_FIXED_BODY_OFFSET);
	decoded.receive_tsf = mpdu->tsf;
	decoded.hardware_epoch = hardware_epoch;
	decoded.gp2_on_air_rise = mpdu->gp2_on_air_rise;
	decoded.rssi_dbm = mpdu->rssi_dbm;
	decoded.beacon_interval_tu = ax211_bss_get_le16(frame + 32U);
	decoded.capability = ax211_bss_get_le16(frame + 34U);
	decoded.channel = mpdu->channel;
	decoded.dtim_count = ies.dtim_count;
	decoded.dtim_period = ies.dtim_period;
	decoded.tim_valid = ies.tim_seen;
	decoded.wmm_present = ies.wmm_present;
	decoded.receive_tsf_valid = mpdu->tsf_valid != 0U ? 1U : 0U;
	decoded.source = subtype == AX211_BSS_FRAME_SUBTYPE_BEACON ?
	    INTEL_AX211_BSS_SOURCE_BEACON :
	    INTEL_AX211_BSS_SOURCE_PROBE_RESPONSE;
	decoded.valid = 1U;
	if (decoded.beacon_interval_tu == 0U)
		return INTEL_AX211_BSS_MALFORMED;
	*entry = decoded;
	return INTEL_AX211_BSS_OK;
}

int
intel_ax211_bss_cache_init(
	struct intel_ax211_bss_cache *cache,
	uint32_t hardware_epoch)
{
	if (cache == NULL || hardware_epoch == 0U)
		return INTEL_AX211_BSS_INVALID;
	memset(cache, 0, sizeof(*cache));
	cache->hardware_epoch = hardware_epoch;
	cache->initialized = 1U;
	return INTEL_AX211_BSS_OK;
}

int
intel_ax211_bss_cache_observe(
	struct intel_ax211_bss_cache *cache,
	const struct intel_ax211_bss_entry *entry)
{
	size_t index;
	size_t available;

	if (cache == NULL || entry == NULL || cache->initialized == 0U)
		return INTEL_AX211_BSS_INVALID;
	if (!ax211_bss_entry_valid(entry))
		return INTEL_AX211_BSS_MALFORMED;
	if (entry->hardware_epoch != cache->hardware_epoch)
		return INTEL_AX211_BSS_STALE;

	for (index = 0U; index < INTEL_AX211_BSS_CACHE_LIMIT; index++) {
		if (cache->entry[index].valid != 0U &&
		    memcmp(cache->entry[index].bssid, entry->bssid,
		    sizeof(entry->bssid)) == 0) {
			if (entry->observation_generation <
			    cache->entry[index].observation_generation)
				return INTEL_AX211_BSS_STALE;
			if (entry->observation_generation ==
			    cache->entry[index].observation_generation &&
			    entry->channel == cache->entry[index].channel &&
			    cache->entry[index].source ==
			    INTEL_AX211_BSS_SOURCE_BEACON &&
			    entry->source ==
			    INTEL_AX211_BSS_SOURCE_PROBE_RESPONSE) {
				/* Common scan admission refreshes these selection fields
				 * even when private beacon timing remains authoritative. */
				cache->entry[index].rssi_dbm = entry->rssi_dbm;
				cache->entry[index].last_seen_ticks =
				    entry->last_seen_ticks;
				return INTEL_AX211_BSS_OK;
			}
			cache->entry[index] = *entry;
			return INTEL_AX211_BSS_OK;
		}
	}

	if (cache->count < INTEL_AX211_BSS_CACHE_LIMIT) {
		available = INTEL_AX211_BSS_CACHE_LIMIT;
		for (index = 0U; index < INTEL_AX211_BSS_CACHE_LIMIT;
		    index++) {
			if (cache->entry[index].valid == 0U &&
			    available == INTEL_AX211_BSS_CACHE_LIMIT)
				available = index;
		}
		if (available == INTEL_AX211_BSS_CACHE_LIMIT)
			return INTEL_AX211_BSS_MALFORMED;
		cache->entry[available] = *entry;
		cache->count++;
		return INTEL_AX211_BSS_OK;
	}

	/* Mirror the common 64-entry scan cache's deterministic admission rule
	 * so every BSS it can publish remains representable here. */
	index = 0U;
	for (available = 1U; available < INTEL_AX211_BSS_CACHE_LIMIT;
	    available++) {
		if (ax211_bss_entry_worse(&cache->entry[available],
		    &cache->entry[index]))
			index = available;
	}
	if (ax211_bss_entry_worse(&cache->entry[index], entry))
		cache->entry[index] = *entry;
	return INTEL_AX211_BSS_OK;
}

int
intel_ax211_bss_cache_lookup(
	const struct intel_ax211_bss_cache *cache,
	const uint8_t bssid[INTEL_AX211_BSS_ADDRESS_SIZE],
	uint8_t channel,
	uint32_t hardware_epoch,
	struct intel_ax211_bss_entry *entry)
{
	size_t index;

	if (cache == NULL || bssid == NULL || entry == NULL ||
	    cache->initialized == 0U || channel == 0U ||
	    hardware_epoch == 0U)
		return INTEL_AX211_BSS_INVALID;
	if (!ax211_bss_address_valid(bssid))
		return INTEL_AX211_BSS_INVALID;
	if (cache->hardware_epoch != hardware_epoch)
		return INTEL_AX211_BSS_STALE;
	for (index = 0U; index < INTEL_AX211_BSS_CACHE_LIMIT; index++) {
		if (cache->entry[index].valid != 0U &&
		    cache->entry[index].channel == channel &&
		    memcmp(cache->entry[index].bssid, bssid,
		    INTEL_AX211_BSS_ADDRESS_SIZE) == 0) {
			if (cache->entry[index].hardware_epoch != hardware_epoch)
				return INTEL_AX211_BSS_STALE;
			*entry = cache->entry[index];
			return INTEL_AX211_BSS_OK;
		}
	}
	return INTEL_AX211_BSS_NOT_FOUND;
}

int
intel_ax211_bss_assoc_metadata(
	const struct intel_ax211_bss_entry *entry,
	uint64_t connection_generation,
	uint32_t hardware_epoch,
	struct intel_ax211_bss_assoc_metadata *metadata)
{
	struct intel_ax211_bss_assoc_metadata decoded;

	if (entry == NULL || metadata == NULL || connection_generation == 0U ||
	    hardware_epoch == 0U)
		return INTEL_AX211_BSS_INVALID;
	if (!ax211_bss_entry_valid(entry))
		return INTEL_AX211_BSS_MALFORMED;
	if (entry->hardware_epoch != hardware_epoch)
		return INTEL_AX211_BSS_STALE;
	memset(&decoded, 0, sizeof(decoded));
	memcpy(decoded.bssid, entry->bssid, sizeof(decoded.bssid));
	decoded.common_generation = connection_generation;
	decoded.observation_generation = entry->observation_generation;
	decoded.beacon_tsf = entry->frame_timestamp;
	decoded.receive_tsf = entry->receive_tsf;
	decoded.hardware_epoch = entry->hardware_epoch;
	decoded.beacon_arrive_time = entry->gp2_on_air_rise;
	decoded.beacon_interval_tu = entry->beacon_interval_tu;
	decoded.capability = entry->capability;
	decoded.channel = entry->channel;
	decoded.dtim_count = entry->dtim_count;
	decoded.dtim_period = entry->dtim_period;
	decoded.tim_valid = entry->tim_valid;
	decoded.wmm_present = entry->wmm_present;
	decoded.receive_tsf_valid = entry->receive_tsf_valid;
	*metadata = decoded;
	return INTEL_AX211_BSS_OK;
}

static uint16_t
ax211_bss_get_le16(const uint8_t *bytes)
{
	return (uint16_t)((uint16_t)bytes[0U] |
	    ((uint16_t)bytes[1U] << 8));
}

static uint64_t
ax211_bss_get_le64(const uint8_t *bytes)
{
	uint64_t value;

	value = bytes[0U];
	value |= (uint64_t)bytes[1U] << 8;
	value |= (uint64_t)bytes[2U] << 16;
	value |= (uint64_t)bytes[3U] << 24;
	value |= (uint64_t)bytes[4U] << 32;
	value |= (uint64_t)bytes[5U] << 40;
	value |= (uint64_t)bytes[6U] << 48;
	value |= (uint64_t)bytes[7U] << 56;
	return value;
}

static int
ax211_bss_address_valid(const uint8_t address[6])
{
	size_t index;
	uint8_t combined;

	if (address == NULL || (address[0U] & 1U) != 0U)
		return 0;
	combined = 0U;
	for (index = 0U; index < 6U; index++)
		combined |= address[index];
	return combined != 0U;
}

static int
ax211_bss_entry_valid(const struct intel_ax211_bss_entry *entry)
{
	if (entry == NULL || entry->valid != 1U ||
	    entry->observation_generation == 0U ||
	    entry->hardware_epoch == 0U ||
	    entry->channel == 0U || entry->beacon_interval_tu == 0U ||
	    !ax211_bss_address_valid(entry->bssid))
		return 0;
	if (entry->source != INTEL_AX211_BSS_SOURCE_BEACON &&
	    entry->source != INTEL_AX211_BSS_SOURCE_PROBE_RESPONSE)
		return 0;
	if (entry->tim_valid > 1U || entry->wmm_present > 1U ||
	    entry->receive_tsf_valid > 1U)
		return 0;
	if (entry->tim_valid != 0U && (entry->dtim_period == 0U ||
	    entry->dtim_count >= entry->dtim_period))
		return 0;
	return 1;
}

/* True when left is evicted before right, matching the common WLAN cache. */
static int
ax211_bss_entry_worse(
	const struct intel_ax211_bss_entry *left,
	const struct intel_ax211_bss_entry *right)
{
	if (left->rssi_dbm != right->rssi_dbm)
		return left->rssi_dbm < right->rssi_dbm;
	if (left->last_seen_ticks != right->last_seen_ticks)
		return left->last_seen_ticks < right->last_seen_ticks;
	return memcmp(left->bssid, right->bssid,
	    INTEL_AX211_BSS_ADDRESS_SIZE) > 0;
}

static int
ax211_bss_channel_observe(
	struct ax211_bss_ie_state *state,
	uint8_t channel,
	int ht)
{
	if (channel == 0U)
		return INTEL_AX211_BSS_MALFORMED;
	if (ht != 0) {
		if (state->ht_seen != 0U)
			return INTEL_AX211_BSS_MALFORMED;
		state->ht_seen = 1U;
	} else {
		if (state->ds_seen != 0U)
			return INTEL_AX211_BSS_MALFORMED;
		state->ds_seen = 1U;
	}
	if (state->channel != 0U && state->channel != channel)
		return INTEL_AX211_BSS_MALFORMED;
	state->channel = channel;
	return INTEL_AX211_BSS_OK;
}

static int
ax211_bss_wmm_observe(
	struct ax211_bss_ie_state *state,
	const uint8_t *data,
	size_t length)
{
	if (length < 4U || data[0U] != 0x00U || data[1U] != 0x50U ||
	    data[2U] != 0xf2U || data[3U] != AX211_BSS_WMM_TYPE)
		return INTEL_AX211_BSS_OK;
	if (length < AX211_BSS_WMM_INFO_SIZE)
		return INTEL_AX211_BSS_MALFORMED;
	if ((data[4U] != AX211_BSS_WMM_INFO_SUBTYPE &&
	    data[4U] != AX211_BSS_WMM_PARAMETER_SUBTYPE) ||
	    data[5U] != AX211_BSS_WMM_VERSION)
		return INTEL_AX211_BSS_MALFORMED;
	if ((data[4U] == AX211_BSS_WMM_INFO_SUBTYPE &&
	    length != AX211_BSS_WMM_INFO_SIZE) ||
	    (data[4U] == AX211_BSS_WMM_PARAMETER_SUBTYPE &&
	    length != AX211_BSS_WMM_PARAMETER_SIZE))
		return INTEL_AX211_BSS_MALFORMED;
	if (state->wmm_present != 0U)
		return INTEL_AX211_BSS_MALFORMED;
	state->wmm_present = 1U;
	return INTEL_AX211_BSS_OK;
}

static int
ax211_bss_ies_decode(
	const uint8_t *bytes,
	size_t length,
	struct ax211_bss_ie_state *state)
{
	size_t offset;
	size_t element_length;
	const uint8_t *data;
	uint8_t identifier;
	int result;

	if (bytes == NULL || state == NULL)
		return INTEL_AX211_BSS_INVALID;
	offset = 0U;
	while (offset < length) {
		if (length - offset < 2U)
			return INTEL_AX211_BSS_TRUNCATED;
		identifier = bytes[offset];
		element_length = bytes[offset + 1U];
		offset += 2U;
		if (element_length > length - offset)
			return INTEL_AX211_BSS_TRUNCATED;
		data = bytes + offset;
		if (identifier == AX211_BSS_IE_SSID) {
			if (element_length > AX211_BSS_SSID_MAX ||
			    state->ssid_seen != 0U)
				return INTEL_AX211_BSS_MALFORMED;
			state->ssid_seen = 1U;
		} else if (identifier == AX211_BSS_IE_DS_PARAMETER) {
			if (element_length != 1U)
				return INTEL_AX211_BSS_MALFORMED;
			result = ax211_bss_channel_observe(state, data[0U], 0);
			if (result != INTEL_AX211_BSS_OK)
				return result;
		} else if (identifier == AX211_BSS_IE_HT_OPERATION) {
			if (element_length != AX211_BSS_HT_OPERATION_SIZE)
				return INTEL_AX211_BSS_MALFORMED;
			result = ax211_bss_channel_observe(state, data[0U], 1);
			if (result != INTEL_AX211_BSS_OK)
				return result;
		} else if (identifier == AX211_BSS_IE_TIM) {
			if (element_length < AX211_BSS_TIM_MIN_SIZE ||
			    state->tim_seen != 0U)
				return INTEL_AX211_BSS_MALFORMED;
			if (data[1U] == 0U || data[0U] >= data[1U])
				return INTEL_AX211_BSS_MALFORMED;
			state->dtim_count = data[0U];
			state->dtim_period = data[1U];
			state->tim_seen = 1U;
		} else if (identifier == AX211_BSS_IE_VENDOR) {
			result = ax211_bss_wmm_observe(state, data,
			    element_length);
			if (result != INTEL_AX211_BSS_OK)
				return result;
		}
		offset += element_length;
	}
	return INTEL_AX211_BSS_OK;
}
