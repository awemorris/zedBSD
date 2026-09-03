/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "../../../src/kern/net/wlan-l2.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const uint8_t station[6] = { 0x02U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U };
static const uint8_t bssid[6] = { 0x02U, 0xaaU, 0xbbU, 0xccU, 0xddU, 0xeeU };

static size_t
ethernet_frame(uint8_t frame[1514], int group)
{
	static const uint8_t peer[6] = { 0x02U, 0x90U, 0x91U, 0x92U, 0x93U, 0x94U };
	unsigned index;

	memcpy(frame, peer, 6U);
	if (group)
		memset(frame, 0xff, 6U);
	memcpy(frame + 6U, station, 6U);
	frame[12U] = 0x08U;
	frame[13U] = 0x00U;
	for (index = 14U; index < 78U; index++)
		frame[index] = (uint8_t)(index * 7U);
	return 78U;
}

static void
turn_into_from_ds(uint8_t *mpdu, const uint8_t *ethernet)
{
	mpdu[0] = 0x08U;
	mpdu[1] = (uint8_t)((mpdu[1] & 0x40U) | 0x02U);
	memcpy(mpdu + 4U, ethernet, 6U);
	memcpy(mpdu + 10U, bssid, 6U);
	memcpy(mpdu + 16U, ethernet + 6U, 6U);
}

static void
test_round_trip(void)
{
	uint8_t ethernet[1514], output[1514], mpdu[WLAN_L2_MPDU_MAX];
	struct wlan_l2_rx_security security;
	struct wlan_l2_rx_state state;
	size_t ethernet_length, output_length, mpdu_length;

	memset(&security, 0, sizeof(security));
	memset(&state, 0, sizeof(state));
	ethernet_length = ethernet_frame(ethernet, 0);
	assert(wlan_l2_build_data(station, bssid, ethernet, ethernet_length, 1,
	    0U, 0x010203040506ULL, mpdu, sizeof(mpdu), &mpdu_length) == 0);
	assert(mpdu_length == 24U + 8U + 8U + ethernet_length - 14U);
	assert(mpdu[0] == 0x08U && mpdu[1] == 0x41U);
	assert(mpdu[24U] == 0x06U && mpdu[25U] == 0x05U &&
	    mpdu[27U] == 0x20U && mpdu[28U] == 0x04U &&
	    mpdu[31U] == 0x01U);
	memset(mpdu + mpdu_length, 0x5a, WLAN_L2_CCMP_MIC_SIZE);
	mpdu_length += WLAN_L2_CCMP_MIC_SIZE;
	memcpy(ethernet, station, 6U);
	turn_into_from_ds(mpdu, ethernet);
	security.key_generation = 19U;
	security.packet_number = 0x010203040506ULL;
	security.decrypted = 1U;
	security.cipher_ccmp = 1U;
	state.pairwise_key_generation = 19U;
	assert(wlan_l2_parse_data(station, bssid, mpdu, mpdu_length,
	    &security, &state, output, sizeof(output), &output_length) == 0);
	assert(output_length == ethernet_length);
	assert(memcmp(output, ethernet, ethernet_length) == 0);
	assert(state.pairwise_packet_number == security.packet_number);
	assert(wlan_l2_parse_data(station, bssid, mpdu, mpdu_length,
	    &security, &state, output, sizeof(output), &output_length) == EALREADY);
}

static void
test_group_and_plain(void)
{
	uint8_t ethernet[1514], output[1514], mpdu[WLAN_L2_MPDU_MAX];
	struct wlan_l2_rx_security security;
	struct wlan_l2_rx_state state;
	size_t ethernet_length, output_length, mpdu_length;

	memset(&security, 0, sizeof(security));
	memset(&state, 0, sizeof(state));
	ethernet_length = ethernet_frame(ethernet, 1);
	assert(wlan_l2_build_data(station, bssid, ethernet, ethernet_length, 1,
	    2U, 7U, mpdu, sizeof(mpdu), &mpdu_length) == 0);
	memset(mpdu + mpdu_length, 0xa5, WLAN_L2_CCMP_MIC_SIZE);
	mpdu_length += WLAN_L2_CCMP_MIC_SIZE;
	turn_into_from_ds(mpdu, ethernet);
	security.key_generation = 3U;
	security.packet_number = 7U;
	security.decrypted = 1U;
	security.cipher_ccmp = 1U;
	security.key_index = 2U;
	state.group_key_generation[2U] = 3U;
	assert(wlan_l2_parse_data(station, bssid, mpdu, mpdu_length,
	    &security, &state, output, sizeof(output), &output_length) == 0);
	assert(state.group_packet_number[2] == 7U);

	ethernet_length = ethernet_frame(ethernet, 0);
	assert(wlan_l2_build_data(station, bssid, ethernet, ethernet_length, 0,
	    0U, 0U, mpdu, sizeof(mpdu), &mpdu_length) == 0);
	memcpy(ethernet, station, 6U);
	turn_into_from_ds(mpdu, ethernet);
	memset(&security, 0, sizeof(security));
	assert(wlan_l2_parse_data(station, bssid, mpdu, mpdu_length,
	    &security, &state, output, sizeof(output), &output_length) == 0);
}

static void
test_qos_from_ds(void)
{
	uint8_t ethernet[1514], output[1514], mpdu[WLAN_L2_MPDU_MAX];
	struct wlan_l2_rx_security security;
	struct wlan_l2_rx_state state;
	size_t ethernet_length, output_length, mpdu_length;

	memset(&security, 0, sizeof(security));
	memset(&state, 0, sizeof(state));
	ethernet_length = ethernet_frame(ethernet, 0);
	assert(wlan_l2_build_data(station, bssid, ethernet, ethernet_length, 1,
	    0U, 11U, mpdu, sizeof(mpdu), &mpdu_length) == 0);
	memset(mpdu + mpdu_length, 0x5a, WLAN_L2_CCMP_MIC_SIZE);
	mpdu_length += WLAN_L2_CCMP_MIC_SIZE;
	memcpy(ethernet, station, 6U);
	turn_into_from_ds(mpdu, ethernet);
	memmove(mpdu + WLAN_L2_DATA_HEADER_SIZE + 2U,
	    mpdu + WLAN_L2_DATA_HEADER_SIZE,
	    mpdu_length - WLAN_L2_DATA_HEADER_SIZE);
	mpdu_length += 2U;
	mpdu[0U] = 0x88U;
	mpdu[24U] = 0U;
	mpdu[25U] = 0U;
	security.key_generation = 23U;
	security.packet_number = 11U;
	security.decrypted = 1U;
	security.cipher_ccmp = 1U;
	state.pairwise_key_generation = 23U;
	mpdu[24U] = 1U;
	assert(wlan_l2_parse_data(station, bssid, mpdu, mpdu_length,
	    &security, &state, output, sizeof(output), &output_length) ==
	    EPROTONOSUPPORT);
	assert(state.pairwise_packet_number == 0U);
	mpdu[24U] = 0U;
	assert(wlan_l2_parse_data(station, bssid, mpdu, mpdu_length,
	    &security, &state, output, sizeof(output), &output_length) == 0);
	assert(output_length == ethernet_length);
	assert(memcmp(output, ethernet, ethernet_length) == 0);
}

static void
test_rejections(void)
{
	uint8_t ethernet[1514], output[1514], mpdu[WLAN_L2_MPDU_MAX];
	struct wlan_l2_rx_security security;
	struct wlan_l2_rx_state state;
	size_t length, output_length, mpdu_length;

	memset(&security, 0, sizeof(security));
	memset(&state, 0, sizeof(state));
	length = ethernet_frame(ethernet, 0);
	assert(wlan_l2_build_data(station, bssid, ethernet, 13U, 0, 0U, 0U,
	    mpdu, sizeof(mpdu), &mpdu_length) == EINVAL);
	assert(wlan_l2_build_data(station, bssid, ethernet, length, 1, 0U, 0U,
	    mpdu, sizeof(mpdu), &mpdu_length) == EINVAL);
	assert(wlan_l2_build_data(station, bssid, ethernet, length, 1, 0U, 1U,
	    mpdu, 10U, &mpdu_length) == ENOSPC);
	assert(wlan_l2_build_data(station, bssid, ethernet, length, 1, 0U, 1U,
	    mpdu, sizeof(mpdu), &mpdu_length) == 0);
	memset(mpdu + mpdu_length, 0x3c, WLAN_L2_CCMP_MIC_SIZE);
	mpdu_length += WLAN_L2_CCMP_MIC_SIZE;
	memcpy(ethernet, station, 6U);
	turn_into_from_ds(mpdu, ethernet);
	security.key_generation = 1U;
	security.packet_number = 1U;
	security.decrypted = 1U;
	security.cipher_ccmp = 1U;
	state.pairwise_key_generation = 2U;
	assert(wlan_l2_parse_data(station, bssid, mpdu, mpdu_length,
	    &security, &state, output, sizeof(output), &output_length) == EACCES);
	state.pairwise_key_generation = 1U;
	security.key_index = 1U;
	assert(wlan_l2_parse_data(station, bssid, mpdu, mpdu_length,
	    &security, &state, output, sizeof(output), &output_length) == EACCES);
	security.key_index = 0U;
	mpdu[10U] ^= 1U;
	assert(wlan_l2_parse_data(station, bssid, mpdu, mpdu_length,
	    &security, &state, output, sizeof(output), &output_length) == EINVAL);
	mpdu[10U] ^= 1U;
	mpdu[32U] = 0U;
	assert(wlan_l2_parse_data(station, bssid, mpdu, mpdu_length,
	    &security, &state, output, sizeof(output), &output_length) ==
	    EPROTONOSUPPORT);

	assert(wlan_l2_build_data(station, bssid, ethernet, length, 0, 0U, 0U,
	    mpdu, sizeof(mpdu), &mpdu_length) == 0);
	memcpy(ethernet, station, 6U);
	turn_into_from_ds(mpdu, ethernet);
	memset(&security, 0, sizeof(security));
	security.key_generation = 1U;
	assert(wlan_l2_parse_data(station, bssid, mpdu, mpdu_length,
	    &security, &state, output, sizeof(output), &output_length) == EINVAL);
}

int
main(void)
{
	test_round_trip();
	test_group_and_plain();
	test_qos_from_ds();
	test_rejections();
	puts("wlan l2 fixture: PASS");
	return 0;
}
