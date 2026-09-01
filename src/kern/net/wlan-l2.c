/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "wlan-l2.h"

#include <errno.h>
#include <string.h>

#define WLAN_FC_TYPE_MASK       0x000cU
#define WLAN_FC_DATA            0x0008U
#define WLAN_FC_SUBTYPE_MASK    0x00f0U
#define WLAN_FC_TO_DS           0x0100U
#define WLAN_FC_FROM_DS         0x0200U
#define WLAN_FC_MORE_FRAGMENTS  0x0400U
#define WLAN_FC_PROTECTED       0x4000U

static uint16_t
load_le16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static void
store_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

static int
mac_equal(const uint8_t left[6], const uint8_t right[6])
{
	uint8_t difference = 0U;
	unsigned index;

	for (index = 0U; index < 6U; index++)
		difference |= left[index] ^ right[index];
	return difference == 0U;
}

static int
mac_group(const uint8_t address[6])
{
	return (address[0] & 1U) != 0U;
}

static void
ccmp_header_store(uint8_t header[WLAN_L2_CCMP_HEADER_SIZE],
	uint8_t key_index, uint64_t packet_number)
{
	header[0] = (uint8_t)packet_number;
	header[1] = (uint8_t)(packet_number >> 8);
	header[2] = 0U;
	header[3] = (uint8_t)(0x20U | ((key_index & 3U) << 6));
	header[4] = (uint8_t)(packet_number >> 16);
	header[5] = (uint8_t)(packet_number >> 24);
	header[6] = (uint8_t)(packet_number >> 32);
	header[7] = (uint8_t)(packet_number >> 40);
}

static int
ccmp_header_parse(const uint8_t header[WLAN_L2_CCMP_HEADER_SIZE],
	uint8_t *key_index, uint64_t *packet_number)
{
	if (header[2] != 0U || (header[3] & 0x20U) == 0U ||
	    (header[3] & 0x1fU) != 0U)
		return EINVAL;
	*key_index = (uint8_t)(header[3] >> 6);
	*packet_number = (uint64_t)header[0] |
	    ((uint64_t)header[1] << 8) | ((uint64_t)header[4] << 16) |
	    ((uint64_t)header[5] << 24) | ((uint64_t)header[6] << 32) |
	    ((uint64_t)header[7] << 40);
	return *packet_number == 0U ? EINVAL : 0;
}

int
wlan_l2_build_data(const uint8_t station[6], const uint8_t bssid[6],
	const uint8_t *ethernet, size_t ethernet_length, int protected_frame,
	uint8_t key_index, uint64_t packet_number, uint8_t *mpdu,
	size_t capacity, size_t *mpdu_length)
{
	size_t offset;
	size_t payload_length;
	size_t required;
	uint16_t frame_control = WLAN_FC_DATA | WLAN_FC_TO_DS;

	if (mpdu_length == NULL)
		return EINVAL;
	*mpdu_length = 0U;
	if (station == NULL || bssid == NULL || ethernet == NULL || mpdu == NULL ||
	    ethernet_length < WLAN_L2_ETHERNET_HEADER_SIZE ||
	    ethernet_length > WLAN_L2_ETHERNET_MAX || key_index > 3U ||
	    (protected_frame && (packet_number == 0U ||
	    packet_number > 0x0000ffffffffffffULL)) ||
	    (!protected_frame && packet_number != 0U))
		return EINVAL;
	payload_length = ethernet_length - WLAN_L2_ETHERNET_HEADER_SIZE;
	required = WLAN_L2_DATA_HEADER_SIZE + WLAN_L2_LLC_SNAP_SIZE +
	    payload_length;
	if (protected_frame)
		required += WLAN_L2_CCMP_HEADER_SIZE;
	if (required > capacity)
		return ENOSPC;
	memset(mpdu, 0, required);
	if (protected_frame)
		frame_control |= WLAN_FC_PROTECTED;
	store_le16(mpdu, frame_control);
	memcpy(mpdu + 4U, bssid, 6U);
	memcpy(mpdu + 10U, station, 6U);
	memcpy(mpdu + 16U, ethernet, 6U);
	offset = WLAN_L2_DATA_HEADER_SIZE;
	if (protected_frame) {
		ccmp_header_store(mpdu + offset, key_index, packet_number);
		offset += WLAN_L2_CCMP_HEADER_SIZE;
	}
	mpdu[offset++] = 0xaaU;
	mpdu[offset++] = 0xaaU;
	mpdu[offset++] = 0x03U;
	mpdu[offset++] = 0x00U;
	mpdu[offset++] = 0x00U;
	mpdu[offset++] = 0x00U;
	mpdu[offset++] = ethernet[12U];
	mpdu[offset++] = ethernet[13U];
	memcpy(mpdu + offset, ethernet + WLAN_L2_ETHERNET_HEADER_SIZE,
	    payload_length);
	*mpdu_length = required;
	return 0;
}

int
wlan_l2_parse_data(const uint8_t station[6], const uint8_t bssid[6],
	const uint8_t *mpdu, size_t mpdu_length,
	const struct wlan_l2_rx_security *security,
	struct wlan_l2_rx_state *state, uint8_t *ethernet, size_t capacity,
	size_t *ethernet_length)
{
	static const uint8_t llc_prefix[6] = { 0xaaU, 0xaaU, 0x03U, 0U, 0U, 0U };
	uint16_t frame_control;
	size_t offset = WLAN_L2_DATA_HEADER_SIZE;
	size_t payload_length;
	uint64_t packet_number = 0U;
	uint64_t expected_key_generation;
	uint64_t *last_packet_number = NULL;
	uint8_t key_index = 0U;
	int protected_frame;
	int group;
	int error;

	if (ethernet_length == NULL)
		return EINVAL;
	*ethernet_length = 0U;
	if (station == NULL || bssid == NULL || mpdu == NULL ||
	    security == NULL || state == NULL || ethernet == NULL ||
	    mpdu_length < WLAN_L2_DATA_HEADER_SIZE + WLAN_L2_LLC_SNAP_SIZE)
		return EINVAL;
	frame_control = load_le16(mpdu);
	if ((frame_control & WLAN_FC_TYPE_MASK) != WLAN_FC_DATA ||
	    (frame_control & WLAN_FC_SUBTYPE_MASK) != 0U ||
	    (frame_control & (WLAN_FC_TO_DS | WLAN_FC_FROM_DS)) !=
	    WLAN_FC_FROM_DS || (frame_control & WLAN_FC_MORE_FRAGMENTS) != 0U ||
	    (load_le16(mpdu + 22U) & 0x000fU) != 0U ||
	    !mac_equal(mpdu + 10U, bssid) ||
	    (!mac_equal(mpdu + 4U, station) && !mac_group(mpdu + 4U)))
		return EINVAL;
	protected_frame = (frame_control & WLAN_FC_PROTECTED) != 0U;
	if (protected_frame) {
		if (mpdu_length < WLAN_L2_DATA_HEADER_SIZE +
		    WLAN_L2_CCMP_HEADER_SIZE + WLAN_L2_LLC_SNAP_SIZE +
		    WLAN_L2_CCMP_MIC_SIZE ||
		    !security->decrypted || !security->cipher_ccmp ||
		    security->key_generation == 0U)
			return EACCES;
		error = ccmp_header_parse(mpdu + offset, &key_index,
		    &packet_number);
		if (error != 0 || key_index != security->key_index ||
		    packet_number != security->packet_number)
			return EACCES;
		group = mac_group(mpdu + 4U);
		if (!group && key_index != 0U)
			return EACCES;
		expected_key_generation = group ?
		    state->group_key_generation[key_index] :
		    state->pairwise_key_generation;
		if (security->key_generation != expected_key_generation)
			return EACCES;
		last_packet_number = group ?
		    &state->group_packet_number[key_index] :
		    &state->pairwise_packet_number;
		if (packet_number <= *last_packet_number)
			return EALREADY;
		offset += WLAN_L2_CCMP_HEADER_SIZE;
		/* RTL8822B reports a decrypted payload with the verified CCMP MIC
		 * still appended.  Integrity is a driver-reported hardware result;
		 * the trailer is not part of the Ethernet payload. */
		mpdu_length -= WLAN_L2_CCMP_MIC_SIZE;
	} else if (security->key_generation != 0U || security->decrypted ||
	    security->cipher_ccmp || security->key_index != 0U ||
	    security->packet_number != 0U) {
		return EINVAL;
	}
	if (memcmp(mpdu + offset, llc_prefix, sizeof(llc_prefix)) != 0)
		return EPROTONOSUPPORT;
	payload_length = mpdu_length - offset - WLAN_L2_LLC_SNAP_SIZE;
	if (payload_length > WLAN_L2_ETHERNET_MAX -
	    WLAN_L2_ETHERNET_HEADER_SIZE)
		return EMSGSIZE;
	if (WLAN_L2_ETHERNET_HEADER_SIZE + payload_length > capacity)
		return ENOSPC;
	memcpy(ethernet, mpdu + 4U, 6U);
	memcpy(ethernet + 6U, mpdu + 16U, 6U);
	ethernet[12U] = mpdu[offset + 6U];
	ethernet[13U] = mpdu[offset + 7U];
	memcpy(ethernet + WLAN_L2_ETHERNET_HEADER_SIZE,
	    mpdu + offset + WLAN_L2_LLC_SNAP_SIZE, payload_length);
	if (protected_frame)
		*last_packet_number = packet_number;
	*ethernet_length = WLAN_L2_ETHERNET_HEADER_SIZE + payload_length;
	return 0;
}
