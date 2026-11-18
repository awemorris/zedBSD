/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Intel AX211 private BSS association-metadata implementation.
 */

#include "intel-ax211-bss.h"
#include <kern/kcrt.h>

#include <stddef.h>
#include <stdint.h>

/*
 * The frame control values that mark a frame the BSS cache accepts.
 *
 * Only a protocol version 0 management frame of subtype probe response (5) or
 * beacon (8), sent neither to nor from a distribution system and not
 * protected, describes a network.
 *
 * Values referenced: IEEE Std 802.11-2020 9.2.4.1.
 */
#define AX211_BSS_FRAME_VERSION_MASK 0x0003U
#define AX211_BSS_FRAME_TYPE_MASK 0x000cU
#define AX211_BSS_FRAME_TYPE_MANAGEMENT 0x0000U
#define AX211_BSS_FRAME_SUBTYPE_MASK 0x00f0U
#define AX211_BSS_FRAME_SUBTYPE_PROBE_RESPONSE 0x0050U
#define AX211_BSS_FRAME_SUBTYPE_BEACON 0x0080U
#define AX211_BSS_FRAME_TO_FROM_DS 0x0300U
#define AX211_BSS_FRAME_PROTECTED 0x4000U

/*
 * Where the fields of a beacon or probe response are.
 *
 * The management header is 24 bytes with the BSSID at byte 16; the fixed body
 * follows (timestamp 8 bytes, beacon interval at byte 32, capability
 * information at 34), and the elements start at byte 36.
 *
 * Values referenced: IEEE Std 802.11-2020 9.3.3.2 and 9.3.3.10.
 */
#define AX211_BSS_FIXED_BODY_OFFSET 24U
#define AX211_BSS_INFORMATION_ELEMENTS_OFFSET 36U
#define AX211_BSS_BSSID_OFFSET 16U

/*
 * The elements the cache reads.
 *
 * The element identifiers are SSID 0, DS parameter set 3, TIM 5, HT operation
 * 61 and vendor specific 221.  An SSID has at most 32 bytes, an HT operation
 * body 22 bytes and a TIM body at least 4 bytes.  WMM is a vendor element of
 * OUI 00-50-F2 and OUI type 2, whose subtype 0 is the 7-byte information
 * element and subtype 1 the 24-byte parameter element, both version 1.  The
 * DS parameter set gives the channel the network says it is on; a frame whose
 * element disagrees with the channel it was received on is refused as
 * malformed.
 *
 * Values referenced: IEEE Std 802.11-2020 9.4.2 (the element identifiers and
 * lengths) and the Wi-Fi Alliance WMM Specification (the WMM information and
 * parameter elements).
 */
#define AX211_BSS_IE_SSID 0U
#define AX211_BSS_IE_TIM 5U
#define AX211_BSS_IE_DS_PARAMETER 3U
#define AX211_BSS_IE_HT_OPERATION 61U
#define AX211_BSS_IE_VENDOR 221U
#define AX211_BSS_SSID_MAX 32U
#define AX211_BSS_HT_OPERATION_SIZE 22U
#define AX211_BSS_TIM_MIN_SIZE 4U
#define AX211_BSS_WMM_INFO_SIZE 7U
#define AX211_BSS_WMM_PARAMETER_SIZE 24U
#define AX211_BSS_WMM_TYPE 2U
#define AX211_BSS_WMM_INFO_SUBTYPE 0U
#define AX211_BSS_WMM_PARAMETER_SUBTYPE 1U
#define AX211_BSS_WMM_VERSION 1U

/*
 * What the element walk of one frame found.
 *
 * It lives on the stack of one decode; the seen flags refuse an element that
 * appears twice.
 */
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
static int ax211_bss_address_valid(const uint8_t address[6]);
static int ax211_bss_ies_decode(const uint8_t *bytes, size_t length, struct ax211_bss_ie_state *state);
static int ax211_bss_channel_observe(struct ax211_bss_ie_state *state, uint8_t channel, int ht);
static int ax211_bss_wmm_observe(struct ax211_bss_ie_state *state, const uint8_t *data, size_t length);
static uint64_t ax211_bss_get_le64(const uint8_t *bytes);
static int ax211_bss_entry_valid(const struct intel_ax211_bss_entry *entry);
static int ax211_bss_entry_worse(const struct intel_ax211_bss_entry *left, const struct intel_ax211_bss_entry *right);

/*
 * Decodes a received beacon or probe response into a BSS entry.
 */
int
drv_intel_ax211_bss_decode(
	const struct intel_ax211_rx_delivery *mpdu,
	uint64_t observation_generation,
	uint32_t hardware_epoch,
	struct intel_ax211_bss_entry *entry)
{
	struct intel_ax211_bss_entry decoded;
	struct ax211_bss_ie_state ies;
	const uint8_t *frame;
	uint16_t frame_control;
	uint16_t subtype;
	int valid;
	int result;

	/* Refuses missing arguments and unset generations. */
	if (mpdu == NULL ||
	    entry == NULL ||
	    mpdu->frame == NULL ||
	    observation_generation == 0U ||
	    hardware_epoch == 0U) {
		return INTEL_AX211_BSS_INVALID;
	}

	/* Refuses a frame without a channel or shorter than the fixed body. */
	if (mpdu->channel == 0U ||
	    mpdu->length < AX211_BSS_INFORMATION_ELEMENTS_OFFSET) {
		return INTEL_AX211_BSS_TRUNCATED;
	}

	/* Refuses a frame of another protocol version or type. */
	frame = mpdu->frame;
	frame_control = ax211_bss_get_le16(frame);
	if ((frame_control & AX211_BSS_FRAME_VERSION_MASK) != 0U ||
	    (frame_control & AX211_BSS_FRAME_TYPE_MASK) !=
		    AX211_BSS_FRAME_TYPE_MANAGEMENT) {
		return INTEL_AX211_BSS_UNSUPPORTED;
	}

	/*
	 * Refuses a frame to or from a distribution system, or a protected one.
	 */
	if ((frame_control &
	     (AX211_BSS_FRAME_TO_FROM_DS | AX211_BSS_FRAME_PROTECTED)) != 0U) {
		return INTEL_AX211_BSS_MALFORMED;
	}

	/*
	 * Refuses a management frame other than a beacon or a probe response.
	 */
	subtype = frame_control & AX211_BSS_FRAME_SUBTYPE_MASK;
	if (subtype != AX211_BSS_FRAME_SUBTYPE_BEACON &&
	    subtype != AX211_BSS_FRAME_SUBTYPE_PROBE_RESPONSE) {
		return INTEL_AX211_BSS_UNSUPPORTED;
	}

	/* Refuses a group or zero BSSID. */
	valid = ax211_bss_address_valid(frame + AX211_BSS_BSSID_OFFSET);
	if (!valid)
		return INTEL_AX211_BSS_MALFORMED;

	/* Walks the elements. */
	kern_memset(&ies, 0, sizeof(ies));
	result = ax211_bss_ies_decode(
		frame + AX211_BSS_INFORMATION_ELEMENTS_OFFSET,
		mpdu->length - AX211_BSS_INFORMATION_ELEMENTS_OFFSET, &ies);
	if (result != INTEL_AX211_BSS_OK)
		return result;

	/*
	 * Refuses a frame that names another channel than the one it came on.
	 */
	if (ies.channel != 0U && ies.channel != mpdu->channel)
		return INTEL_AX211_BSS_MALFORMED;

	/* Renders the frame and its receive context as one entry. */
	kern_memset(&decoded, 0, sizeof(decoded));
	kern_memcpy(decoded.bssid, frame + AX211_BSS_BSSID_OFFSET,
	       sizeof(decoded.bssid));
	decoded.observation_generation = observation_generation;
	decoded.frame_timestamp =
		ax211_bss_get_le64(frame + AX211_BSS_FIXED_BODY_OFFSET);
	decoded.receive_tsf = mpdu->tsf;
	decoded.hardware_epoch = hardware_epoch;
	decoded.air_start_clock = mpdu->air_start_clock;
	decoded.rssi_dbm = mpdu->rssi_dbm;
	decoded.beacon_interval_tu = ax211_bss_get_le16(frame + 32U);
	decoded.capability = ax211_bss_get_le16(frame + 34U);
	decoded.channel = mpdu->channel;
	decoded.dtim_count = ies.dtim_count;
	decoded.dtim_period = ies.dtim_period;
	decoded.tim_valid = ies.tim_seen;
	decoded.wmm_present = ies.wmm_present;
	decoded.receive_tsf_valid = mpdu->tsf_valid != 0U ? 1U : 0U;
	decoded.valid = 1U;

	/* Records which kind of frame the entry came from. */
	if (subtype == AX211_BSS_FRAME_SUBTYPE_BEACON) {
		decoded.source = INTEL_AX211_BSS_SOURCE_BEACON;
	} else {
		decoded.source = INTEL_AX211_BSS_SOURCE_PROBE_RESPONSE;
	}

	/* Refuses a zero beacon interval. */
	if (decoded.beacon_interval_tu == 0U)
		return INTEL_AX211_BSS_MALFORMED;

	/* Succeeded: publishes the entry. */
	*entry = decoded;
	return INTEL_AX211_BSS_OK;
}

/*
 * Empties the BSS cache and binds it to one firmware epoch.
 */
int
drv_intel_ax211_bss_cache_init(
	struct intel_ax211_bss_cache *cache,
	uint32_t hardware_epoch)
{
	/* Refuses a missing cache and an unset epoch. */
	if (cache == NULL || hardware_epoch == 0U)
		return INTEL_AX211_BSS_INVALID;

	/* Empties the cache. */
	kern_memset(cache, 0, sizeof(*cache));
	cache->hardware_epoch = hardware_epoch;
	cache->initialized = 1U;

	/* Succeeded. */
	return INTEL_AX211_BSS_OK;
}

/*
 * Records one observed BSS in the cache.
 *
 * A BSS already cached is updated; a new one takes a free slot, or replaces
 * the worst entry when the cache is full and it is better.
 */
int
drv_intel_ax211_bss_cache_observe(
	struct intel_ax211_bss_cache *cache,
	const struct intel_ax211_bss_entry *entry)
{
	size_t index;
	size_t available;
	int valid;
	int compared;
	int worse;

	/* Refuses missing arguments and an uninitialized cache. */
	if (cache == NULL || entry == NULL || cache->initialized == 0U)
		return INTEL_AX211_BSS_INVALID;

	/* Refuses an inconsistent entry. */
	valid = ax211_bss_entry_valid(entry);
	if (!valid)
		return INTEL_AX211_BSS_MALFORMED;

	/* Refuses an entry of another firmware epoch. */
	if (entry->hardware_epoch != cache->hardware_epoch)
		return INTEL_AX211_BSS_STALE;

	/* Updates the entry of the same BSSID if the cache holds one. */
	for (index = 0U; index < INTEL_AX211_BSS_CACHE_LIMIT; index++) {
		/* Skips a free slot. */
		if (cache->entry[index].valid == 0U)
			continue;

		/* Skips a slot of another BSSID. */
		compared = kern_memcmp(cache->entry[index].bssid, entry->bssid,
				       sizeof(entry->bssid));
		if (compared != 0)
			continue;

		/* Refuses an observation older than the cached one. */
		if (entry->observation_generation <
		    cache->entry[index].observation_generation) {
			return INTEL_AX211_BSS_STALE;
		}

		/*
		 * A probe response of the same scan pass after a beacon on the same
		 * channel only refreshes the fields the common scan admission
		 * selects by; the beacon's timing stays authoritative.
		 */
		if (entry->observation_generation ==
			    cache->entry[index].observation_generation &&
		    entry->channel == cache->entry[index].channel &&
		    cache->entry[index].source ==
			    INTEL_AX211_BSS_SOURCE_BEACON &&
		    entry->source == INTEL_AX211_BSS_SOURCE_PROBE_RESPONSE) {
			cache->entry[index].rssi_dbm = entry->rssi_dbm;
			cache->entry[index].last_seen_ticks =
				entry->last_seen_ticks;
			return INTEL_AX211_BSS_OK;
		}

		/* Replaces the cached entry with the newer one. */
		cache->entry[index] = *entry;
		return INTEL_AX211_BSS_OK;
	}

	/*
	 * Stores a new BSS in the first free slot while the cache is not full.
	 */
	if (cache->count < INTEL_AX211_BSS_CACHE_LIMIT) {
		/* Finds the first free slot. */
		available = INTEL_AX211_BSS_CACHE_LIMIT;
		for (index = 0U; index < INTEL_AX211_BSS_CACHE_LIMIT; index++) {
			/* Remembers only the first free slot. */
			if (cache->entry[index].valid == 0U &&
			    available == INTEL_AX211_BSS_CACHE_LIMIT)
				available = index;
		}

		/*
		 * A count below the limit with no free slot is a broken cache.
		 */
		if (available == INTEL_AX211_BSS_CACHE_LIMIT)
			return INTEL_AX211_BSS_MALFORMED;

		/* Stores the entry. */
		cache->entry[available] = *entry;
		cache->count++;
		return INTEL_AX211_BSS_OK;
	}

	/*
	 * Finds the worst cached entry with the eviction rule of the common
	 * 64-entry scan cache, so every BSS it can publish remains representable
	 * here.
	 */
	index = 0U;
	for (available = 1U; available < INTEL_AX211_BSS_CACHE_LIMIT;
	     available++) {
		/* Moves to an entry that would be evicted first. */
		worse = ax211_bss_entry_worse(&cache->entry[available],
					      &cache->entry[index]);
		if (worse)
			index = available;
	}

	/* Replaces the worst entry when the new one is better. */
	worse = ax211_bss_entry_worse(&cache->entry[index], entry);
	if (worse)
		cache->entry[index] = *entry;

	/* Succeeded: the cache holds the better entries. */
	return INTEL_AX211_BSS_OK;
}

/*
 * Finds the cached entry of one BSSID on one channel.
 */
int
drv_intel_ax211_bss_cache_lookup(
	const struct intel_ax211_bss_cache *cache,
	const uint8_t bssid[INTEL_AX211_BSS_ADDRESS_SIZE],
	uint8_t channel,
	uint32_t hardware_epoch,
	struct intel_ax211_bss_entry *entry)
{
	size_t index;
	int valid;
	int compared;

	/*
	 * Refuses missing arguments, an uninitialized cache and unset values.
	 */
	if (cache == NULL ||
	    bssid == NULL ||
	    entry == NULL ||
	    cache->initialized == 0U ||
	    channel == 0U ||
	    hardware_epoch == 0U) {
		return INTEL_AX211_BSS_INVALID;
	}

	/* Refuses a group or zero BSSID. */
	valid = ax211_bss_address_valid(bssid);
	if (!valid)
		return INTEL_AX211_BSS_INVALID;

	/* Refuses a lookup of another firmware epoch. */
	if (cache->hardware_epoch != hardware_epoch)
		return INTEL_AX211_BSS_STALE;

	/* Looks for the BSSID on the channel. */
	for (index = 0U; index < INTEL_AX211_BSS_CACHE_LIMIT; index++) {
		/* Skips a free slot and a slot on another channel. */
		if (cache->entry[index].valid == 0U ||
		    cache->entry[index].channel != channel)
			continue;

		/* Skips a slot of another BSSID. */
		compared = kern_memcmp(cache->entry[index].bssid, bssid,
				       INTEL_AX211_BSS_ADDRESS_SIZE);
		if (compared != 0)
			continue;

		/* Refuses an entry of another firmware epoch. */
		if (cache->entry[index].hardware_epoch != hardware_epoch)
			return INTEL_AX211_BSS_STALE;

		/* Succeeded: reports the entry. */
		*entry = cache->entry[index];
		return INTEL_AX211_BSS_OK;
	}

	/* The cache does not hold the BSS. */
	return INTEL_AX211_BSS_NOT_FOUND;
}

/*
 * Turns a cached entry into the association metadata of one connection.
 */
int
drv_intel_ax211_bss_assoc_metadata(
	const struct intel_ax211_bss_entry *entry,
	uint64_t connection_generation,
	uint32_t hardware_epoch,
	struct drv_intel_ax211_bss_assoc_metadata *metadata)
{
	struct drv_intel_ax211_bss_assoc_metadata decoded;
	int valid;

	/* Refuses missing arguments and unset generations. */
	if (entry == NULL ||
	    metadata == NULL ||
	    connection_generation == 0U ||
	    hardware_epoch == 0U) {
		return INTEL_AX211_BSS_INVALID;
	}

	/* Refuses an inconsistent entry. */
	valid = ax211_bss_entry_valid(entry);
	if (!valid)
		return INTEL_AX211_BSS_MALFORMED;

	/* Refuses an entry of another firmware epoch. */
	if (entry->hardware_epoch != hardware_epoch)
		return INTEL_AX211_BSS_STALE;

	/* Copies what the association needs from the entry. */
	kern_memset(&decoded, 0, sizeof(decoded));
	kern_memcpy(decoded.bssid, entry->bssid, sizeof(decoded.bssid));
	decoded.common_generation = connection_generation;
	decoded.observation_generation = entry->observation_generation;
	decoded.beacon_timestamp = entry->frame_timestamp;
	decoded.receive_tsf = entry->receive_tsf;
	decoded.hardware_epoch = entry->hardware_epoch;
	decoded.beacon_arrive_time = entry->air_start_clock;
	decoded.beacon_interval_tu = entry->beacon_interval_tu;
	decoded.capability = entry->capability;
	decoded.channel = entry->channel;
	decoded.dtim_count = entry->dtim_count;
	decoded.dtim_period = entry->dtim_period;
	decoded.tim_valid = entry->tim_valid;
	decoded.wmm_present = entry->wmm_present;
	decoded.receive_tsf_valid = entry->receive_tsf_valid;

	/* Succeeded: publishes the metadata. */
	*metadata = decoded;
	return INTEL_AX211_BSS_OK;
}

/* Reads a 16-bit little-endian value. */
static uint16_t
ax211_bss_get_le16(
	const uint8_t *bytes)
{
	/* Assembles the value from its two bytes, least significant first. */
	return (uint16_t)((uint16_t)bytes[0U] | ((uint16_t)bytes[1U] << 8));
}

/*
 * Reports whether an address can be a BSSID: 1 when it is an individual,
 * nonzero address, 0 when not.
 */
static int
ax211_bss_address_valid(
	const uint8_t address[6])
{
	size_t index;
	uint8_t combined;

	/* Rejects a missing address and a group address. */
	if (address == NULL || (address[0U] & 1U) != 0U)
		return 0;

	/* Combines the six bytes to tell a zero address. */
	combined = 0U;
	for (index = 0U; index < 6U; index++)
		combined |= address[index];

	/* Rejects the zero address. */
	if (combined == 0U)
		return 0;

	/* The address can be a BSSID. */
	return 1;
}

/* Walks the elements of a beacon or probe response. */
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

	/* Refuses missing arguments. */
	if (bytes == NULL || state == NULL)
		return INTEL_AX211_BSS_INVALID;

	/* Reads one element after another until the end of the frame. */
	offset = 0U;
	while (offset < length) {
		/* Refuses an element header cut off by the end of the frame. */
		if (length - offset < 2U)
			return INTEL_AX211_BSS_TRUNCATED;

		/* Takes the identifier and the length of the element. */
		identifier = bytes[offset];
		element_length = bytes[offset + 1U];
		offset += 2U;

		/* Refuses an element body cut off by the end of the frame. */
		if (element_length > length - offset)
			return INTEL_AX211_BSS_TRUNCATED;

		/* Reads the elements the cache uses; others are skipped. */
		data = bytes + offset;
		if (identifier == AX211_BSS_IE_SSID) {
			/*
			 * Refuses an overlong SSID and a second SSID element.
			 */
			if (element_length > AX211_BSS_SSID_MAX ||
			    state->ssid_seen != 0U) {
				return INTEL_AX211_BSS_MALFORMED;
			}

			/* Notes the SSID. */
			state->ssid_seen = 1U;
		} else if (identifier == AX211_BSS_IE_DS_PARAMETER) {
			/* Refuses a DS parameter set of another length. */
			if (element_length != 1U)
				return INTEL_AX211_BSS_MALFORMED;

			/* Records the channel it names. */
			result = ax211_bss_channel_observe(state, data[0U], 0);
			if (result != INTEL_AX211_BSS_OK)
				return result;
		} else if (identifier == AX211_BSS_IE_HT_OPERATION) {
			/* Refuses an HT operation element of another length. */
			if (element_length != AX211_BSS_HT_OPERATION_SIZE)
				return INTEL_AX211_BSS_MALFORMED;

			/* Records its primary channel. */
			result = ax211_bss_channel_observe(state, data[0U], 1);
			if (result != INTEL_AX211_BSS_OK)
				return result;
		} else if (identifier == AX211_BSS_IE_TIM) {
			/* Refuses a short TIM and a second TIM element. */
			if (element_length < AX211_BSS_TIM_MIN_SIZE ||
			    state->tim_seen != 0U) {
				return INTEL_AX211_BSS_MALFORMED;
			}

			/*
			 * Refuses a zero DTIM period and a count not below it.
			 */
			if (data[1U] == 0U || data[0U] >= data[1U])
				return INTEL_AX211_BSS_MALFORMED;

			/* Records the DTIM count and period. */
			state->dtim_count = data[0U];
			state->dtim_period = data[1U];
			state->tim_seen = 1U;
		} else if (identifier == AX211_BSS_IE_VENDOR) {
			/* Records a WMM element. */
			result = ax211_bss_wmm_observe(state, data,
						       element_length);
			if (result != INTEL_AX211_BSS_OK)
				return result;
		}

		/* Moves to the next element. */
		offset += element_length;
	}

	/* Succeeded: every element was read. */
	return INTEL_AX211_BSS_OK;
}

/* Records the channel a DS parameter set or an HT operation element names. */
static int
ax211_bss_channel_observe(
	struct ax211_bss_ie_state *state,
	uint8_t channel,
	int ht)
{
	/* Refuses channel zero. */
	if (channel == 0U)
		return INTEL_AX211_BSS_MALFORMED;

	/* Refuses a second element of the same kind. */
	if (ht != 0) {
		/* A second HT operation element. */
		if (state->ht_seen != 0U)
			return INTEL_AX211_BSS_MALFORMED;

		/* Notes the HT operation element. */
		state->ht_seen = 1U;
	} else {
		/* A second DS parameter set. */
		if (state->ds_seen != 0U)
			return INTEL_AX211_BSS_MALFORMED;

		/* Notes the DS parameter set. */
		state->ds_seen = 1U;
	}

	/* Refuses elements that name different channels. */
	if (state->channel != 0U && state->channel != channel)
		return INTEL_AX211_BSS_MALFORMED;

	/* Records the channel. */
	state->channel = channel;

	/* Succeeded. */
	return INTEL_AX211_BSS_OK;
}

/* Records a WMM element among the vendor elements. */
static int
ax211_bss_wmm_observe(
	struct ax211_bss_ie_state *state,
	const uint8_t *data,
	size_t length)
{
	/* A vendor element other than WMM is skipped. */
	if (length < 4U ||
	    data[0U] != 0x00U ||
	    data[1U] != 0x50U ||
	    data[2U] != 0xf2U ||
	    data[3U] != AX211_BSS_WMM_TYPE) {
		return INTEL_AX211_BSS_OK;
	}

	/* Refuses a WMM element shorter than the information element. */
	if (length < AX211_BSS_WMM_INFO_SIZE)
		return INTEL_AX211_BSS_MALFORMED;

	/*
	 * Refuses a WMM subtype other than information or parameter, or another
	 * version.
	 */
	if ((data[4U] != AX211_BSS_WMM_INFO_SUBTYPE &&
	     data[4U] != AX211_BSS_WMM_PARAMETER_SUBTYPE) ||
	    data[5U] != AX211_BSS_WMM_VERSION) {
		return INTEL_AX211_BSS_MALFORMED;
	}

	/* Refuses a WMM element whose length does not match its subtype. */
	if ((data[4U] == AX211_BSS_WMM_INFO_SUBTYPE &&
	     length != AX211_BSS_WMM_INFO_SIZE) ||
	    (data[4U] == AX211_BSS_WMM_PARAMETER_SUBTYPE &&
	     length != AX211_BSS_WMM_PARAMETER_SIZE)) {
		return INTEL_AX211_BSS_MALFORMED;
	}

	/* Refuses a second WMM element. */
	if (state->wmm_present != 0U)
		return INTEL_AX211_BSS_MALFORMED;

	/* Notes the WMM element. */
	state->wmm_present = 1U;

	/* Succeeded. */
	return INTEL_AX211_BSS_OK;
}

/* Reads a 64-bit little-endian value. */
static uint64_t
ax211_bss_get_le64(
	const uint8_t *bytes)
{
	uint64_t value;

	/* Assembles the eight little-endian bytes. */
	value = bytes[0U];
	value |= (uint64_t)bytes[1U] << 8;
	value |= (uint64_t)bytes[2U] << 16;
	value |= (uint64_t)bytes[3U] << 24;
	value |= (uint64_t)bytes[4U] << 32;
	value |= (uint64_t)bytes[5U] << 40;
	value |= (uint64_t)bytes[6U] << 48;
	value |= (uint64_t)bytes[7U] << 56;

	/* Reports the value. */
	return value;
}

/* Reports whether a BSS entry is consistent: 1 when it is, 0 when not. */
static int
ax211_bss_entry_valid(
	const struct intel_ax211_bss_entry *entry)
{
	int valid;

	/* Rejects a missing or unused entry and unset fields. */
	if (entry == NULL ||
	    entry->valid != 1U ||
	    entry->observation_generation == 0U ||
	    entry->hardware_epoch == 0U ||
	    entry->channel == 0U ||
	    entry->beacon_interval_tu == 0U) {
		return 0;
	}

	/* Rejects a group or zero BSSID. */
	valid = ax211_bss_address_valid(entry->bssid);
	if (!valid)
		return 0;

	/* Rejects an unknown source. */
	if (entry->source != INTEL_AX211_BSS_SOURCE_BEACON &&
	    entry->source != INTEL_AX211_BSS_SOURCE_PROBE_RESPONSE) {
		return 0;
	}

	/* Rejects flags other than 0 or 1. */
	if (entry->tim_valid > 1U ||
	    entry->wmm_present > 1U ||
	    entry->receive_tsf_valid > 1U) {
		return 0;
	}

	/* Rejects a TIM with a zero period or a count not below it. */
	if (entry->tim_valid != 0U &&
	    (entry->dtim_period == 0U ||
	     entry->dtim_count >= entry->dtim_period)) {
		return 0;
	}

	/* The entry is consistent. */
	return 1;
}

/*
 * Reports whether left is evicted before right: by weaker signal, then by
 * older sighting, then by the larger BSSID, as in the common WLAN cache.
 */
static int
ax211_bss_entry_worse(
	const struct intel_ax211_bss_entry *left,
	const struct intel_ax211_bss_entry *right)
{
	int compared;

	/* A weaker signal is worse. */
	if (left->rssi_dbm != right->rssi_dbm) {
		/* The weaker of the two is worse. */
		if (left->rssi_dbm < right->rssi_dbm)
			return 1;
		return 0;
	}

	/* With equal signals, an older sighting is worse. */
	if (left->last_seen_ticks != right->last_seen_ticks) {
		/* The one seen earlier is worse. */
		if (left->last_seen_ticks < right->last_seen_ticks)
			return 1;
		return 0;
	}

	/* Otherwise the larger BSSID is worse. */
	compared = kern_memcmp(left->bssid, right->bssid,
			       INTEL_AX211_BSS_ADDRESS_SIZE);
	if (compared > 0)
		return 1;

	/* The left entry is not worse. */
	return 0;
}
