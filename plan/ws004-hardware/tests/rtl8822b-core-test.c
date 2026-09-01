/*
 * RTL8822B production-code codec fixture
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../src/drivers/rtl8822b-internal.h"

static void
put_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

static void
put_le32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

static uint16_t
get_le16(const uint8_t *bytes)
{
	return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint32_t
get_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	    ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void
test_sha256(void)
{
	static const uint8_t empty_digest[32] = {
		0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14,
		0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
		0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c,
		0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55
	};
	static const uint8_t abc_digest[32] = {
		0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
		0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
		0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
		0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
	};
	static const uint8_t block_digest[32] = {
		0x24, 0x8d, 0x6a, 0x61, 0xd2, 0x06, 0x38, 0xb8,
		0xe5, 0xc0, 0x26, 0x93, 0x0c, 0x3e, 0x60, 0x39,
		0xa3, 0x3c, 0xe4, 0x59, 0x64, 0xff, 0x21, 0x67,
		0xf6, 0xec, 0xed, 0xd4, 0x19, 0xdb, 0x06, 0xc1
	};
	static const char block[] =
	    "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	uint8_t digest[32];

	assert(rtl8822b_sha256(NULL, 0, digest) == 0);
	assert(memcmp(digest, empty_digest, sizeof(digest)) == 0);
	assert(rtl8822b_sha256("abc", 3, digest) == 0);
	assert(memcmp(digest, abc_digest, sizeof(digest)) == 0);
	assert(rtl8822b_sha256(block, sizeof(block) - 1U, digest) == 0);
	assert(memcmp(digest, block_digest, sizeof(digest)) == 0);
	assert(rtl8822b_sha256(NULL, 1, digest) == EINVAL);
	assert(rtl8822b_sha256("x", 1, NULL) == EINVAL);
}

static void
make_firmware(uint8_t *firmware)
{
	memset(firmware, 0x5a, RTL8822B_FIRMWARE_SIZE);
	memset(firmware, 0, RTL8822B_FIRMWARE_HEADER_SIZE);
	put_le16(firmware, 0x8822U);
	put_le16(firmware + 4U, 30U);
	firmware[6] = 20U;
	firmware[24] = 0x08U;
	put_le16(firmware + 28U, 14U);
	put_le32(firmware + 32U, 0x80200000U);
	put_le32(firmware + 36U, RTL8822B_FIRMWARE_DMEM_SIZE);
	put_le32(firmware + 48U, RTL8822B_FIRMWARE_IMEM_SIZE);
	put_le32(firmware + 60U, 0x80000000U);
}

struct walk_state {
	size_t count;
	size_t dmem_bytes;
	size_t imem_bytes;
	size_t next_dmem_file;
	size_t next_imem_file;
	uint32_t next_dmem_destination;
	uint32_t next_imem_destination;
	size_t fail_at;
};

static int
check_chunk(void *opaque, const struct rtl8822b_firmware_chunk *chunk)
{
	struct walk_state *state = opaque;
	uint8_t descriptor[RTL8822B_FIRMWARE_TX_DESCRIPTOR_SIZE];
	uint16_t checksum = 0;
	unsigned word;

	if (state->count == state->fail_at)
		return EIO;
	assert(chunk->length != 0U);
	assert(chunk->length <= RTL8822B_FIRMWARE_CHUNK_MAX);
	assert(chunk->wire_payload_length == chunk->length ||
	    chunk->wire_payload_length == chunk->length + 1U);
	assert(chunk->wire_payload_length == chunk->length +
	    (((chunk->length + RTL8822B_FIRMWARE_TX_DESCRIPTOR_SIZE) %
	    512U) == 0U));
	assert(rtl8822b_firmware_tx_descriptor(descriptor,
	    chunk->wire_payload_length) == 0);
	assert((get_le32(descriptor) & 0xffffU) ==
	    chunk->wire_payload_length);
	assert(((get_le32(descriptor) >> 16) & 0xffU) ==
	    RTL8822B_FIRMWARE_TX_DESCRIPTOR_SIZE);
	assert((get_le32(descriptor) & (1U << 26)) != 0U);
	assert(((get_le32(descriptor + 4U) >> 8) & 0x1fU) == 16U);
	for (word = 0; word < 16U; word++)
		checksum ^= get_le16(descriptor + word * 2U);
	assert(checksum == 0U);

	if (chunk->segment == RTL8822B_FIRMWARE_SEGMENT_DMEM) {
		assert(chunk->file_offset == state->next_dmem_file);
		assert(chunk->destination == state->next_dmem_destination);
		assert(chunk->first == (state->dmem_bytes == 0U));
		assert(chunk->checksum_continue ==
		    (state->dmem_bytes != 0U));
		state->dmem_bytes += chunk->length;
		state->next_dmem_file += chunk->length;
		state->next_dmem_destination += chunk->length;
		assert(chunk->last == (state->dmem_bytes ==
		    RTL8822B_FIRMWARE_DMEM_SIZE +
		    RTL8822B_FIRMWARE_CHECKSUM_SIZE));
	} else {
		assert(chunk->segment == RTL8822B_FIRMWARE_SEGMENT_IMEM);
		assert(state->dmem_bytes == RTL8822B_FIRMWARE_DMEM_SIZE +
		    RTL8822B_FIRMWARE_CHECKSUM_SIZE);
		assert(chunk->file_offset == state->next_imem_file);
		assert(chunk->destination == state->next_imem_destination);
		assert(chunk->first == (state->imem_bytes == 0U));
		assert(chunk->checksum_continue ==
		    (state->imem_bytes != 0U));
		state->imem_bytes += chunk->length;
		state->next_imem_file += chunk->length;
		state->next_imem_destination += chunk->length;
		assert(chunk->last == (state->imem_bytes ==
		    RTL8822B_FIRMWARE_IMEM_SIZE +
		    RTL8822B_FIRMWARE_CHECKSUM_SIZE));
	}
	state->count++;
	return 0;
}

static void
test_firmware(void)
{
	struct rtl8822b_firmware_blob blob;
	struct rtl8822b_firmware_view view;
	struct walk_state state;
	uint8_t digest[32];
	uint8_t descriptor[48];
	uint8_t *firmware = malloc(RTL8822B_FIRMWARE_SIZE);
	int owned;

	assert(firmware != NULL);
	make_firmware(firmware);
	assert(rtl8822b_sha256(firmware, RTL8822B_FIRMWARE_SIZE,
	    digest) == 0);
	assert(rtl8822b_test_firmware_validate(firmware,
	    RTL8822B_FIRMWARE_SIZE, digest, &view) == 0);
	assert(view.dmem_offset == RTL8822B_FIRMWARE_HEADER_SIZE);
	assert(view.dmem_length == RTL8822B_FIRMWARE_DMEM_SIZE + 8U);
	assert(view.imem_offset == view.dmem_offset + view.dmem_length);
	assert(view.imem_length == RTL8822B_FIRMWARE_IMEM_SIZE + 8U);
	assert(view.imem_offset + view.imem_length == view.size);
	assert(rtl8822b_firmware_validate(firmware,
	    RTL8822B_FIRMWARE_SIZE, &view) == EILSEQ);
	assert(rtl8822b_test_firmware_validate(firmware,
	    RTL8822B_FIRMWARE_SIZE - 1U, digest, &view) == EINVAL);

	firmware[6] = 19U;
	assert(rtl8822b_test_firmware_validate(firmware,
	    RTL8822B_FIRMWARE_SIZE, digest, &view) == EINVAL);
	firmware[6] = 20U;
	put_le32(firmware + 36U, RTL8822B_FIRMWARE_DMEM_SIZE - 1U);
	assert(rtl8822b_test_firmware_validate(firmware,
	    RTL8822B_FIRMWARE_SIZE, digest, &view) == EINVAL);
	put_le32(firmware + 36U, RTL8822B_FIRMWARE_DMEM_SIZE);
	put_le32(firmware + 52U, 1U);
	assert(rtl8822b_test_firmware_validate(firmware,
	    RTL8822B_FIRMWARE_SIZE, digest, &view) == EINVAL);
	put_le32(firmware + 52U, 0U);
	firmware[RTL8822B_FIRMWARE_HEADER_SIZE] ^= 1U;
	assert(rtl8822b_test_firmware_validate(firmware,
	    RTL8822B_FIRMWARE_SIZE, digest, &view) == EILSEQ);
	firmware[RTL8822B_FIRMWARE_HEADER_SIZE] ^= 1U;
	assert(rtl8822b_test_firmware_validate(firmware,
	    RTL8822B_FIRMWARE_SIZE, digest, &view) == 0);

	memset(&state, 0, sizeof(state));
	state.next_dmem_file = view.dmem_offset;
	state.next_dmem_destination = view.dmem_address;
	state.next_imem_file = view.imem_offset;
	state.next_imem_destination = view.imem_address;
	state.fail_at = SIZE_MAX;
	assert(rtl8822b_test_firmware_walk(&view, digest, check_chunk,
	    &state) == 0);
	assert(state.count == 40U);
	assert(state.dmem_bytes == RTL8822B_FIRMWARE_DMEM_SIZE + 8U);
	assert(state.imem_bytes == RTL8822B_FIRMWARE_IMEM_SIZE + 8U);
	view.version--;
	assert(rtl8822b_test_firmware_walk(&view, digest, check_chunk,
	    &state) == EINVAL);
	view.version++;
	firmware[RTL8822B_FIRMWARE_HEADER_SIZE] ^= 1U;
	assert(rtl8822b_test_firmware_walk(&view, digest, check_chunk,
	    &state) == EILSEQ);
	firmware[RTL8822B_FIRMWARE_HEADER_SIZE] ^= 1U;
	assert(rtl8822b_firmware_walk(&view, check_chunk, &state) == EILSEQ);
	memset(&state, 0, sizeof(state));
	state.next_dmem_file = view.dmem_offset;
	state.next_dmem_destination = view.dmem_address;
	state.next_imem_file = view.imem_offset;
	state.next_imem_destination = view.imem_address;
	state.fail_at = 3U;
	assert(rtl8822b_test_firmware_walk(&view, digest, check_chunk,
	    &state) == EIO);

	assert(rtl8822b_firmware_tx_descriptor(descriptor, 0) == EINVAL);
	assert(rtl8822b_firmware_tx_descriptor(NULL, 1) == EINVAL);
	memset(&blob, 0, sizeof(blob));
	assert(rtl8822b_test_firmware_blob_state(&blob, &owned) == 0);
	assert(owned == 0);
	blob.bytes = firmware;
	blob.size = RTL8822B_FIRMWARE_SIZE;
	blob.view = view;
	assert(rtl8822b_test_firmware_blob_state(&blob, &owned) == 0);
	assert(owned == 1 && blob.bytes == firmware);
	blob.view.bytes = NULL;
	assert(rtl8822b_test_firmware_blob_state(&blob, &owned) == EINVAL);
	assert(blob.bytes == firmware && blob.size == RTL8822B_FIRMWARE_SIZE);
	free(firmware);
}

static void
append_efuse_block(uint8_t *physical, size_t *used, unsigned block,
	const uint8_t *logical)
{
	unsigned index;

	assert(block < 96U);
	assert(*used + 10U < RTL8822B_EFUSE_PHYSICAL_SIZE -
	    RTL8822B_EFUSE_PROTECTED_SIZE);
	if (block < 16U) {
		physical[(*used)++] = (uint8_t)(block << 4);
	} else {
		physical[(*used)++] =
		    (uint8_t)(((block & 7U) << 5) | 0x0fU);
		physical[(*used)++] = (uint8_t)((block & 0x78U) << 1);
	}
	for (index = 0; index < 8U; index++)
		physical[(*used)++] = logical[block * 8U + index];
}

static void
make_efuse(uint8_t physical[RTL8822B_EFUSE_PHYSICAL_SIZE],
	uint8_t wanted[RTL8822B_EFUSE_LOGICAL_SIZE])
{
	static const uint8_t mac[6] = {0x6c, 0x1f, 0xf7, 0x06, 0x14, 0x8a};
	size_t used = 0;

	memset(physical, 0xff, RTL8822B_EFUSE_PHYSICAL_SIZE);
	memset(wanted, 0xff, RTL8822B_EFUSE_LOGICAL_SIZE);
	wanted[0xb8U] = 0x7fU;
	wanted[0xb9U] = 0x21U;
	wanted[0xbaU] = 0x19U;
	wanted[0xc1U] = 0x20U;
	wanted[0xcaU] = 2U;
	wanted[0xcbU] = 'J';
	wanted[0xccU] = 'P';
	memcpy(wanted + RTL8822BU_EFUSE_MAC_OFFSET, mac, sizeof(mac));
	append_efuse_block(physical, &used, 23U, wanted);
	append_efuse_block(physical, &used, 24U, wanted);
	append_efuse_block(physical, &used, 25U, wanted);
	append_efuse_block(physical, &used, 32U, wanted);
	append_efuse_block(physical, &used, 33U, wanted);
	physical[used] = 0xffU;
}

static void
test_efuse(void)
{
	struct rtl8822bu_board_info board;
	struct rtl8822b_chip_identity identity;
	uint8_t physical[RTL8822B_EFUSE_PHYSICAL_SIZE];
	uint8_t wanted[RTL8822B_EFUSE_LOGICAL_SIZE];
	uint8_t logical[RTL8822B_EFUSE_LOGICAL_SIZE];
	uint32_t sys_cfg = (1U << 12) | (1U << 27);
	size_t index;

	make_efuse(physical, wanted);
	assert(rtl8822b_efuse_decode(physical, sizeof(physical), logical,
	    sizeof(logical)) == 0);
	assert(memcmp(logical, wanted, sizeof(logical)) == 0);
	assert(rtl8822bu_board_parse(logical, sizeof(logical), sys_cfg,
	    &board) == 0);
	assert(board.chip.cut == 1U);
	assert(board.chip.rf_path_count == 2U);
	assert(board.rfe_option == 2U);
	assert(board.country_code[0] == 'J' && board.country_code[1] == 'P');
	assert(memcmp(board.mac_address,
	    wanted + RTL8822BU_EFUSE_MAC_OFFSET, 6U) == 0);
	assert(rtl8822b_efuse_decode(physical, sizeof(physical) - 1U,
	    logical, sizeof(logical)) == EINVAL);
	assert(rtl8822b_efuse_decode(physical, sizeof(physical), logical,
	    sizeof(logical) - 1U) == EINVAL);

	logical[0xcaU] = 4U;
	assert(rtl8822bu_board_parse(logical, sizeof(logical), sys_cfg,
	    &board) == EOPNOTSUPP);
	logical[0xcaU] = 2U;
	logical[RTL8822BU_EFUSE_MAC_OFFSET] |= 1U;
	assert(rtl8822bu_board_parse(logical, sizeof(logical), sys_cfg,
	    &board) == EINVAL);
	logical[RTL8822BU_EFUSE_MAC_OFFSET] &= (uint8_t)~1U;
	assert(rtl8822bu_board_parse(logical, sizeof(logical), 7U << 12,
	    &board) == EOPNOTSUPP);
	assert(rtl8822b_chip_identity_parse(sys_cfg, &identity) == 0);
	assert(rtl8822b_chip_identity_parse(7U << 12, &identity) ==
	    EOPNOTSUPP);

	memset(physical, 0xff, sizeof(physical));
	physical[0] = 0xefU;
	physical[1] = 0xfeU;
	physical[2] = 0x11U;
	physical[3] = 0x22U;
	assert(rtl8822b_efuse_decode(physical, sizeof(physical), logical,
	    sizeof(logical)) == EINVAL);
	memset(physical, 0x1f, sizeof(physical));
	physical[RTL8822B_EFUSE_PHYSICAL_SIZE -
	    RTL8822B_EFUSE_PROTECTED_SIZE - 1U] = 0x0fU;
	assert(rtl8822b_efuse_decode(physical, sizeof(physical), logical,
	    sizeof(logical)) == EINVAL);
	memset(physical, 0xff, sizeof(physical));
	for (index = RTL8822B_EFUSE_PHYSICAL_SIZE -
	    RTL8822B_EFUSE_PROTECTED_SIZE; index < sizeof(physical); index++)
		physical[index] = 0U;
	assert(rtl8822b_efuse_decode(physical, sizeof(physical), logical,
	    sizeof(logical)) == 0);
}

struct rx_state {
	unsigned index;
	unsigned fail_at;
};

static int
check_rx_packet(void *opaque, const struct rtl8822b_rx_packet *packet)
{
	struct rx_state *state = opaque;

	if (state->index == state->fail_at)
		return EIO;
	if (state->index == 0U) {
		assert(packet->kind == RTL8822B_RX_FRAME);
		assert(packet->payload_length == 24U);
		assert(packet->payload[0] == 0x80U);
		assert(packet->phy_info_length == RTL8822B_RX_PHY_INFO_SIZE);
		assert(packet->rssi_dbm == -40);
		assert(packet->rate == 4U);
		assert(packet->bandwidth == 1U);
		assert(packet->tsf_low == 0x12345678U);
		assert(packet->aggregate_length == 88U);
	} else {
		assert(state->index == 1U);
		assert(packet->kind == RTL8822B_RX_C2H);
		assert(packet->payload_length == 5U);
		assert(packet->c2h_id == 0x32U);
		assert(packet->c2h_sequence == 0x9aU);
		assert(packet->aggregate_length == 32U);
	}
	state->index++;
	return 0;
}

static void
make_rx_aggregate(uint8_t aggregate[120])
{
	uint8_t *second = aggregate + 88U;
	uint32_t word0;

	memset(aggregate, 0, 120U);
	word0 = 28U | (4U << 16) | (2U << 24) | 0x04000000U;
	put_le32(aggregate, word0);
	put_le32(aggregate + 12U, 4U);
	put_le32(aggregate + 16U, 1U << 4);
	put_le32(aggregate + 20U, 0x12345678U);
	aggregate[24U + 2U] = 0U;
	aggregate[24U + 2U + 1U] = 70U;
	aggregate[24U + 2U + RTL8822B_RX_PHY_INFO_SIZE] = 0x80U;
	memset(aggregate + 24U + 2U + RTL8822B_RX_PHY_INFO_SIZE + 1U,
	    0x44, 23U);
	memset(aggregate + 24U + 2U + RTL8822B_RX_PHY_INFO_SIZE + 24U,
	    0xee, RTL8822B_RX_FCS_SIZE);
	put_le32(second, 5U);
	put_le32(second + 8U, 0x10000000U);
	second[24U] = 0x32U;
	second[25U] = 0x9aU;
	second[26U] = 1U;
	second[27U] = 2U;
	second[28U] = 3U;
}

static void
test_rx(void)
{
	struct rtl8822b_rx_packet packet;
	struct rx_state state;
	uint8_t aggregate[120];
	uint8_t packet_bytes[96];
	size_t count;
	uint32_t original;

	make_rx_aggregate(aggregate);
	memset(&state, 0, sizeof(state));
	state.fail_at = UINT_MAX;
	assert(rtl8822b_rx_aggregate_walk(aggregate, sizeof(aggregate),
	    check_rx_packet, &state, &count) == 0);
	assert(count == 2U && state.index == 2U);
	assert(rtl8822b_rx_packet_parse(aggregate, 87U, &packet) == EINVAL);
	assert(rtl8822b_rx_packet_parse(aggregate, 23U, &packet) == EINVAL);
	assert(rtl8822b_rx_aggregate_walk(aggregate,
	    RTL8822B_RX_AGGREGATE_MAX + 1U, check_rx_packet, &state,
	    &count) == EINVAL);
	memset(&state, 0, sizeof(state));
	state.fail_at = 0U;
	assert(rtl8822b_rx_aggregate_walk(aggregate, sizeof(aggregate),
	    check_rx_packet, &state, &count) == EIO);
	assert(count == 0U);
	memset(&state, 0, sizeof(state));
	state.fail_at = 1U;
	assert(rtl8822b_rx_aggregate_walk(aggregate, sizeof(aggregate),
	    check_rx_packet, &state, &count) == EIO);
	assert(count == 1U && state.index == 1U);
	original = get_le32(aggregate + 88U);
	put_le32(aggregate + 88U, 0U);
	memset(&state, 0, sizeof(state));
	state.fail_at = UINT_MAX;
	assert(rtl8822b_rx_aggregate_walk(aggregate, sizeof(aggregate),
	    check_rx_packet, &state, &count) == EINVAL);
	assert(count == 0U && state.index == 0U);
	put_le32(aggregate + 88U, original);

	memcpy(packet_bytes, aggregate, 88U);
	original = get_le32(packet_bytes);
	put_le32(packet_bytes, original | 0x4000U);
	assert(rtl8822b_rx_packet_parse(packet_bytes, 88U, &packet) == EINVAL);
	put_le32(packet_bytes, (original & ~(0x0fU << 16)) | (3U << 16));
	assert(rtl8822b_rx_packet_parse(packet_bytes, 88U, &packet) == EINVAL);
	put_le32(packet_bytes, original & ~0x04000000U);
	packet_bytes[24U + 2U] = 2U;
	assert(rtl8822b_rx_packet_parse(packet_bytes, 88U, &packet) == 0);
	assert(packet.phy_info == NULL && packet.phy_info_length == 0U);
	put_le32(packet_bytes, original);
	assert(rtl8822b_rx_packet_parse(packet_bytes, 88U, &packet) == EINVAL);
	packet_bytes[24U + 2U] = 0U;
	put_le32(packet_bytes + 12U, 0x54U);
	assert(rtl8822b_rx_packet_parse(packet_bytes, 88U, &packet) == EINVAL);
	put_le32(packet_bytes + 12U, 4U);
	put_le32(packet_bytes, (original & ~0x3fffU) | 4U);
	assert(rtl8822b_rx_packet_parse(packet_bytes, 88U, &packet) == EINVAL);

	memset(packet_bytes, 0, sizeof(packet_bytes));
	put_le32(packet_bytes, 1U);
	put_le32(packet_bytes + 8U, 0x10000000U);
	assert(rtl8822b_rx_packet_parse(packet_bytes, 32U, &packet) == EINVAL);

	{
		size_t large_length = RTL8822B_RX_DESCRIPTOR_SIZE +
		    RTL8822B_RX_MPDU_MAX + 1U;
		uint8_t *large = calloc(1, large_length);

		assert(large != NULL);
		put_le32(large, RTL8822B_RX_MPDU_MAX + 1U);
		assert(rtl8822b_rx_packet_parse(large, large_length, &packet) ==
		    EINVAL);
		put_le32(large, RTL8822B_RX_MPDU_MAX);
		assert(rtl8822b_rx_packet_parse(large, large_length - 1U,
		    &packet) == 0);
		free(large);
	}
}

static uint32_t
fixture_random(uint32_t *state)
{
	*state = *state * 1664525U + 1013904223U;
	return *state;
}

static void
test_bounded_random_inputs(void)
{
	struct rtl8822b_rx_packet packet;
	uint8_t physical[RTL8822B_EFUSE_PHYSICAL_SIZE];
	uint8_t logical[RTL8822B_EFUSE_LOGICAL_SIZE];
	uint8_t rx[256];
	uint32_t state = 0x8822b210U;
	unsigned iteration;

	for (iteration = 0; iteration < 256U; iteration++) {
		size_t index;
		size_t length = fixture_random(&state) % sizeof(rx);
		int error;

		for (index = 0; index < sizeof(rx); index++)
			rx[index] = (uint8_t)fixture_random(&state);
		error = rtl8822b_rx_packet_parse(rx, length, &packet);
		if (error == 0) {
			assert(packet.payload >= rx);
			assert((size_t)(packet.payload - rx) <= length);
			assert(packet.payload_length <=
			    length - (size_t)(packet.payload - rx));
			assert(packet.aggregate_length <= length);
		}
	}
	for (iteration = 0; iteration < 64U; iteration++) {
		size_t index;

		for (index = 0; index < sizeof(physical); index++)
			physical[index] = (uint8_t)fixture_random(&state);
		(void)rtl8822b_efuse_decode(physical, sizeof(physical), logical,
		    sizeof(logical));
	}
}

static void
test_pinned_blob(const char *path)
{
	struct rtl8822b_firmware_view view;
	struct walk_state state;
	uint8_t *firmware;
	FILE *file;
	int trailing;

	file = fopen(path, "rb");
	assert(file != NULL);
	firmware = malloc(RTL8822B_FIRMWARE_SIZE);
	assert(firmware != NULL);
	assert(fread(firmware, 1, RTL8822B_FIRMWARE_SIZE, file) ==
	    RTL8822B_FIRMWARE_SIZE);
	trailing = fgetc(file);
	assert(trailing == EOF);
	assert(fclose(file) == 0);
	assert(rtl8822b_firmware_validate(firmware,
	    RTL8822B_FIRMWARE_SIZE, &view) == 0);
	memset(&state, 0, sizeof(state));
	state.next_dmem_file = view.dmem_offset;
	state.next_dmem_destination = view.dmem_address;
	state.next_imem_file = view.imem_offset;
	state.next_imem_destination = view.imem_address;
	state.fail_at = SIZE_MAX;
	assert(rtl8822b_firmware_walk(&view, check_chunk, &state) == 0);
	assert(state.count == 40U);
	free(firmware);
}

int
main(int argc, char **argv)
{
	test_sha256();
	test_firmware();
	test_efuse();
	test_rx();
	test_bounded_random_inputs();
	if (argc == 2)
		test_pinned_blob(argv[1]);
	else
		assert(argc == 1);
	puts("rtl8822b core: PASS");
	return 0;
}
