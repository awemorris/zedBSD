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
	static const uint8_t cck_base[2][RTL8822B_2G_CCK_GROUP_COUNT] = {
		{32U, 33U, 34U, 35U, 36U, 37U},
		{28U, 29U, 30U, 31U, 32U, 33U}
	};
	static const uint8_t bw40_base[2][RTL8822B_2G_OFDM_GROUP_COUNT] = {
		{30U, 31U, 32U, 33U, 34U},
		{26U, 27U, 28U, 29U, 30U}
	};
	static const size_t power_offset[2] = {0x10U, 0x3aU};
	unsigned path;
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
	for (path = 0U; path < 2U; path++) {
		memcpy(wanted + power_offset[path], cck_base[path],
		    sizeof(cck_base[path]));
		memcpy(wanted + power_offset[path] +
		    RTL8822B_2G_CCK_GROUP_COUNT, bw40_base[path],
		    sizeof(bw40_base[path]));
	}
	/* Low nibbles are signed OFDM deltas: path A -2, path B +3. */
	wanted[0x1bU] = 0xaeU;
	wanted[0x45U] = 0xb3U;
	append_efuse_block(physical, &used, 2U, wanted);
	append_efuse_block(physical, &used, 3U, wanted);
	append_efuse_block(physical, &used, 7U, wanted);
	append_efuse_block(physical, &used, 8U, wanted);
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
	assert(memcmp(board.tx_power_2g[0].cck_base, wanted + 0x10U,
	    RTL8822B_2G_CCK_GROUP_COUNT) == 0);
	assert(memcmp(board.tx_power_2g[0].bw40_base, wanted + 0x16U,
	    RTL8822B_2G_OFDM_GROUP_COUNT) == 0);
	assert(board.tx_power_2g[0].ofdm_diff == -2);
	assert(memcmp(board.tx_power_2g[1].cck_base, wanted + 0x3aU,
	    RTL8822B_2G_CCK_GROUP_COUNT) == 0);
	assert(memcmp(board.tx_power_2g[1].bw40_base, wanted + 0x40U,
	    RTL8822B_2G_OFDM_GROUP_COUNT) == 0);
	assert(board.tx_power_2g[1].ofdm_diff == 3);
	logical[0x1bU] = 0x08U;
	logical[0x45U] = 0x07U;
	assert(rtl8822bu_board_parse(logical, sizeof(logical), sys_cfg,
	    &board) == 0);
	assert(board.tx_power_2g[0].ofdm_diff == -8);
	assert(board.tx_power_2g[1].ofdm_diff == 7);
	logical[0x1bU] = wanted[0x1bU];
	logical[0x45U] = wanted[0x45U];
	logical[0x10U] = 0x40U;
	assert(rtl8822bu_board_parse(logical, sizeof(logical), sys_cfg,
	    &board) == EINVAL);
	logical[0x10U] = wanted[0x10U];
	logical[0x3aU] = 0x40U;
	assert(rtl8822bu_board_parse(logical, sizeof(logical), sys_cfg,
	    &board) == EINVAL);
	/* A single-path part need not accept or consume path-B calibration. */
	assert(rtl8822bu_board_parse(logical, sizeof(logical), 1U << 12,
	    &board) == 0);
	assert(board.chip.rf_path_count == 1U);
	assert(board.tx_power_2g[1].cck_base[0] == 0U);
	logical[0x3aU] = wanted[0x3aU];
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
	assert(packet.encryption_type == 0U && !packet.software_decrypted &&
	    packet.mac_id == 0U && !packet.icv_error);
	put_le32(packet_bytes, (original & ~0x04000000U) |
	    (3U << 20) | 0x08000000U | 0x8000U);
	put_le32(packet_bytes + 4U, 4U);
	assert(rtl8822b_rx_packet_parse(packet_bytes, 88U, &packet) == 0);
	assert(packet.encryption_type == 3U && packet.software_decrypted &&
	    packet.mac_id == 4U && packet.icv_error);
	put_le32(packet_bytes + 4U, 0U);
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

#define RADIO_REGISTER_COUNT 65536U
#define RADIO_TRACE_MAX      16384U

struct radio_trace_entry {
	uint16_t address;
	uint8_t width;
	uint8_t reserved;
	uint32_t value;
};

struct fake_radio {
	uint32_t *registers;
	struct radio_trace_entry *trace;
	size_t trace_count;
	size_t write_count;
	size_t read_count;
	size_t fail_write_at;
	size_t fail_read_at;
	size_t delay_count;
	size_t one_us_delays;
	size_t five_us_delays;
	size_t thirteen_us_delays;
	size_t yields;
	uint64_t now;
	uint64_t delay_microseconds;
	int automatic_power_ack;
	int automatic_llt_ack;
	int automatic_rf_lut_ack;
};

static struct fake_radio *
fake_radio_create(void)
{
	struct fake_radio *fake = calloc(1, sizeof(*fake));

	assert(fake != NULL);
	fake->registers = calloc(RADIO_REGISTER_COUNT,
	    sizeof(fake->registers[0]));
	fake->trace = calloc(RADIO_TRACE_MAX, sizeof(fake->trace[0]));
	assert(fake->registers != NULL && fake->trace != NULL);
	fake->fail_write_at = SIZE_MAX;
	fake->fail_read_at = SIZE_MAX;
	fake->automatic_power_ack = 1;
	fake->automatic_llt_ack = 1;
	fake->automatic_rf_lut_ack = 1;
	fake->registers[0x0100U] = 0xeaU;
	fake->registers[0x0006U] = 0x02U;
	fake->registers[0x2860U] = 0x00000c01U;
	fake->registers[0x2c60U] = 0x00000c01U;
	fake->registers[0x0c50U] = 0x20U;
	fake->registers[0x0e50U] = 0x20U;
	return fake;
}

static void
fake_radio_destroy(struct fake_radio *fake)
{
	if (fake == NULL)
		return;
	free(fake->trace);
	free(fake->registers);
	free(fake);
}

static int
fake_radio_read(void *context, uint16_t address, unsigned width,
	uint32_t *value)
{
	struct fake_radio *fake = context;

	assert(value != NULL);
	assert(width == 1U || width == 2U || width == 4U);
	if (fake->read_count++ == fake->fail_read_at)
		return EIO;
	*value = fake->registers[address];
	if (width == 1U)
		*value &= 0xffU;
	else if (width == 2U)
		*value &= 0xffffU;
	fake->now++;
	return 0;
}

static int
fake_radio_write(void *context, uint16_t address, unsigned width,
	uint32_t value)
{
	struct fake_radio *fake = context;
	uint32_t mask;

	assert(width == 1U || width == 2U || width == 4U);
	if (fake->write_count++ == fake->fail_write_at)
		return EIO;
	assert(fake->trace_count < RADIO_TRACE_MAX);
	fake->trace[fake->trace_count].address = address;
	fake->trace[fake->trace_count].width = (uint8_t)width;
	fake->trace[fake->trace_count].value = value;
	fake->trace_count++;
	mask = width == 1U ? 0xffU : width == 2U ? 0xffffU : UINT32_MAX;
	fake->registers[address] = (fake->registers[address] & ~mask) |
	    (value & mask);
	if (address == 0x0c90U || address == 0x0e90U) {
		uint32_t rf_address = (value >> 20) & 0xffU;
		uint32_t direct = (address == 0x0c90U ? 0x2800U :
		    0x2c00U) + rf_address * 4U;

		if (fake->automatic_rf_lut_ack || rf_address != 0x33U ||
		    (value & 0x000fffffU) != 1U)
			fake->registers[direct] = value & 0x000fffffU;
	}
	if (fake->automatic_power_ack && address == 0x0005U)
		fake->registers[address] &= ~0x03U;
	if (fake->automatic_llt_ack && address == 0x0208U && width == 1U &&
	    (value & 0x01U) != 0U)
		fake->registers[address] &= ~0x01U;
	fake->now++;
	return 0;
}

static uint64_t
fake_radio_now(void *context)
{
	return ((struct fake_radio *)context)->now;
}

static int
fake_radio_delay(void *context, uint32_t microseconds, uint64_t deadline)
{
	struct fake_radio *fake = context;

	if (fake->now >= deadline || microseconds > deadline - fake->now)
		return ETIMEDOUT;
	fake->delay_count++;
	fake->delay_microseconds += microseconds;
	if (microseconds == 1U)
		fake->one_us_delays++;
	else if (microseconds == 5U)
		fake->five_us_delays++;
	else if (microseconds == 13U)
		fake->thirteen_us_delays++;
	fake->now += microseconds;
	return 0;
}

static void
fake_radio_yield(void *context)
{
	((struct fake_radio *)context)->yields++;
}

static struct rtl8822b_radio_transport
fake_radio_transport(struct fake_radio *fake)
{
	struct rtl8822b_radio_transport transport;

	memset(&transport, 0, sizeof(transport));
	transport.context = fake;
	transport.read = fake_radio_read;
	transport.write = fake_radio_write;
	transport.now_ticks = fake_radio_now;
	transport.delay_us = fake_radio_delay;
	transport.yield = fake_radio_yield;
	return transport;
}

static struct rtl8822bu_board_info
fake_board(uint8_t rfe)
{
	static const uint8_t cck_base[2][RTL8822B_2G_CCK_GROUP_COUNT] = {
		{32U, 33U, 34U, 35U, 36U, 37U},
		{28U, 29U, 30U, 31U, 32U, 33U}
	};
	static const uint8_t bw40_base[2][RTL8822B_2G_OFDM_GROUP_COUNT] = {
		{30U, 31U, 32U, 33U, 34U},
		{26U, 27U, 28U, 29U, 30U}
	};
	struct rtl8822bu_board_info board;

	memset(&board, 0, sizeof(board));
	board.chip.cut = 1U;
	board.chip.rf_path_count = 2U;
	board.chip.mass_production = 1U;
	board.mac_address[0] = 0x6cU;
	board.mac_address[1] = 0x1fU;
	board.mac_address[2] = 0xf7U;
	board.mac_address[3] = 0x06U;
	board.mac_address[4] = 0x14U;
	board.mac_address[5] = 0x8aU;
	board.rfe_option = rfe;
	board.channel_plan = 0x7fU;
	board.crystal_cap = 0x21U;
	board.thermal_meter = 0x19U;
	board.country_code[0] = 'J';
	board.country_code[1] = 'P';
	memcpy(board.tx_power_2g[0].cck_base, cck_base[0],
	    sizeof(cck_base[0]));
	memcpy(board.tx_power_2g[0].bw40_base, bw40_base[0],
	    sizeof(bw40_base[0]));
	board.tx_power_2g[0].ofdm_diff = -2;
	memcpy(board.tx_power_2g[1].cck_base, cck_base[1],
	    sizeof(cck_base[1]));
	memcpy(board.tx_power_2g[1].bw40_base, bw40_base[1],
	    sizeof(bw40_base[1]));
	board.tx_power_2g[1].ofdm_diff = 3;
	return board;
}

static void
assert_legacy_txagc(const struct fake_radio *fake, uint16_t base,
	uint32_t cck, uint32_t ofdm_low, uint32_t ofdm_high)
{
	unsigned offset;

	assert(fake->registers[base] == cck);
	assert(fake->registers[base + 4U] == ofdm_low);
	assert(fake->registers[base + 8U] == ofdm_high);
	for (offset = 0x0cU; offset <= 0x3cU; offset += 4U)
		assert(fake->registers[base + offset] == 0U);
}

static size_t
trace_find(const struct fake_radio *fake, size_t start, uint16_t address,
	unsigned width, uint32_t value)
{
	size_t index;

	for (index = start; index < fake->trace_count; index++) {
		if (fake->trace[index].address == address &&
		    fake->trace[index].width == width &&
		    fake->trace[index].value == value)
			return index;
	}
	fprintf(stderr, "missing trace address=%04x width=%u value=%08x "
	    "start=%zu count=%zu\n", address, width, value, start,
	    fake->trace_count);
	assert(!"required radio stage missing from production trace");
	return SIZE_MAX;
}

static size_t
trace_find_address(const struct fake_radio *fake, size_t start,
	uint16_t address, unsigned width)
{
	size_t index;

	for (index = start; index < fake->trace_count; index++) {
		if (fake->trace[index].address == address &&
		    fake->trace[index].width == width)
			return index;
	}
	assert(!"required radio register missing from production trace");
	return SIZE_MAX;
}

static void
assert_radio_off(const struct fake_radio *fake,
	const struct rtl8822b_radio *radio)
{
	assert(radio->state == RTL8822B_RADIO_OFF);
	assert(radio->transport.read == NULL);
	assert(radio->power_limits_valid == 0U);
	assert((fake->registers[0x0100U] & 0xffU) == 0U);
	assert((fake->registers[0x001fU] & 0x07U) == 0U);
	assert((fake->registers[0x00ecU] & 0x07000000U) == 0U);
}

static void
test_radio_txagc_clamps(void)
{
	struct rtl8822bu_board_info board = fake_board(2U);
	struct fake_radio *fake = fake_radio_create();
	struct rtl8822b_radio_transport transport = fake_radio_transport(fake);
	struct rtl8822b_radio radio;

	board.chip.rf_path_count = 1U;
	memset(board.tx_power_2g[0].cck_base, 0,
	    sizeof(board.tx_power_2g[0].cck_base));
	memset(board.tx_power_2g[0].bw40_base, 0x3f,
	    sizeof(board.tx_power_2g[0].bw40_base));
	board.tx_power_2g[0].ofdm_diff = 7;
	memset(&radio, 0, sizeof(radio));
	assert(rtl8822b_radio_power_on(&radio, &transport, &board,
	    UINT64_MAX) == 0);
	assert(rtl8822b_radio_start(&radio, UINT64_MAX) == 0);
	assert(radio.power_limits_valid != 0U);
	assert_legacy_txagc(fake, 0x1d00U, 0U, 0x3f3f3f3fU,
	    0x3f3f3f3fU);
	assert(rtl8822b_radio_stop(&radio, UINT64_MAX) == 0);
	assert_radio_off(fake, &radio);
	fake_radio_destroy(fake);
}

static void
test_radio_table_interpreter(void)
{
	static const uint32_t conditional[] = {
		0x80000002U, 0U, 0x40000000U, 0U,
		0x0120U, 0x11111111U,
		0xa0000000U, 0U,
		0x0120U, 0x22222222U,
		0xb0000000U, 0U
	};
	static const uint32_t malformed[] = {
		0x80000002U, 0U, 0x0120U, 1U
	};
	static const uint32_t bb_delays[] = {
		0xf9U, 0U, 0xfaU, 0U, 0xfbU, 0U, 0xfcU, 0U,
		0xfdU, 0U, 0xfeU, 0U
	};
	struct fake_radio *fake = fake_radio_create();
	struct rtl8822b_radio radio;

	memset(&radio, 0, sizeof(radio));
	radio.transport = fake_radio_transport(fake);
	radio.board = fake_board(2U);
	assert(rtl8822b_test_radio_table_apply(&radio,
	    RTL8822B_TABLE_DOMAIN_BB, RTL8822B_TABLE_WIDTH_32, 0U,
	    conditional, sizeof(conditional) / sizeof(conditional[0]),
	    UINT64_MAX) == 0);
	assert(fake->registers[0x0120U] == 0x11111111U);
	assert(rtl8822b_test_radio_table_apply(&radio,
	    RTL8822B_TABLE_DOMAIN_BB, RTL8822B_TABLE_WIDTH_32, 0U,
	    malformed, sizeof(malformed) / sizeof(malformed[0]),
	    UINT64_MAX) == EINVAL);
	assert(rtl8822b_test_radio_table_apply(&radio,
	    RTL8822B_TABLE_DOMAIN_BB, RTL8822B_TABLE_WIDTH_32, 0U,
	    bb_delays, sizeof(bb_delays) / sizeof(bb_delays[0]),
	    UINT64_MAX) == 0);
	assert(fake->one_us_delays == 1U);
	assert(fake->five_us_delays == 1U);
	assert(fake->delay_count == 6U);
	assert(rtl8822b_test_radio_table_apply(&radio,
	    RTL8822B_TABLE_DOMAIN_BB, RTL8822B_TABLE_WIDTH_32, 0U,
	    conditional, 1U, UINT64_MAX) == EINVAL);
	fake_radio_destroy(fake);
}

static void
make_probe_request(uint8_t frame[26],
	const struct rtl8822bu_board_info *board)
{
	memset(frame, 0, 26U);
	frame[0] = 0x40U;
	memset(frame + 4U, 0xff, 6U);
	memcpy(frame + 10U, board->mac_address, 6U);
	memset(frame + 16U, 0xff, 6U);
	frame[24U] = 0U;
	frame[25U] = 0U;
}

static void
test_radio_lifecycle(void)
{
	struct rtl8822bu_board_info board = fake_board(2U);
	struct rtl8822b_radio_transport transport;
	struct rtl8822b_radio radio;
	struct fake_radio *baseline = fake_radio_create();
	size_t stages[16];
	size_t stage_count = 0U;
	uint8_t frame[26];
	uint8_t padded_frame[464];
	uint8_t padded_wire[513];
	uint8_t wire[128];
	size_t wire_length;
	size_t index;
	uint16_t checksum;

	memset(&radio, 0, sizeof(radio));
	transport = fake_radio_transport(baseline);
	assert(rtl8822b_radio_power_on(&radio, &transport, &board,
	    UINT64_MAX) == 0);
	assert(radio.state == RTL8822B_RADIO_POWERED);
	assert(rtl8822b_radio_start(&radio, UINT64_MAX) == 0);
	assert(radio.state == RTL8822B_RADIO_STARTED && radio.channel == 1U);
	assert((baseline->registers[0x010cU] & 0xffffU) == 0xf5a5U);
	assert((baseline->registers[0x0290U] & 0xffU) == 0x1eU);
	assert((baseline->registers[0x020cU] & 0x0200U) != 0U);
	assert((baseline->registers[0x0280U] & 0xffffU) == 0x2005U);
	assert((baseline->registers[0x0230U] & 0xffffU) == 64U);
	assert((baseline->registers[0x0234U] & 0xffffU) == 64U);
	assert((baseline->registers[0x0238U] & 0xffffU) == 64U);
	assert((baseline->registers[0x023cU] & 0xffffU) == 0U);
	assert((baseline->registers[0x0240U] & 0xffffU) == 1803U);
	assert((baseline->registers[0x0204U] & 0xffffU) == 1996U);
	assert((baseline->registers[0x011cU]) == 0x5effU);
	assert((baseline->registers[0x0244U] & 0x0003ffffU) == 0x3fa00U);
	assert((baseline->registers[0x0248U] & 0x0003ffffU) == 0x3fe00U);
	assert((baseline->registers[0x024cU] & 0x0003ffffU) == 0x3fa00U);
	assert((baseline->registers[0x0610U]) == 0x06f71f6cU);
	assert((baseline->registers[0x0614U] & 0xffffU) == 0x8a14U);
	assert((baseline->registers[0x051bU] & 0xffU) == 0x09U);
	/* The later pinned MAC table intentionally replaces PIFS 0x19 by 0x1c. */
	assert((baseline->registers[0x0512U] & 0xffU) == 0x1cU);
	assert(baseline->registers[0x0514U] == 0x10100e0aU);
	/* The pinned MAC table subsequently replaces each TXOP low byte. */
	assert((baseline->registers[0x0502U] & 0xffffU) == 0x012fU);
	assert((baseline->registers[0x0506U] & 0xffffU) == 0x035eU);
	assert(baseline->registers[0x0544U] == 0x001b0005U);
	assert((baseline->registers[0x055eU] & 0xffffU) == 0x3030U);
	assert((baseline->registers[0x0550U] & 0x08U) != 0U);
	assert(baseline->registers[0x0540U] == 0x00006404U);
	assert((baseline->registers[0x1990U] & 0x00000c3fU) == 0x00000c30U);
	assert((baseline->registers[0x0974U] & 0x00000c3fU) == 0x00000c3fU);
	assert((baseline->registers[0x0c20U] & 0x80000000U) != 0U);
	assert((baseline->registers[0x0e20U] & 0x80000000U) != 0U);
	assert((baseline->registers[0x0cb0U] & 0x00ffffffU) == 0x00705770U);
	assert((baseline->registers[0x0eb0U] & 0x00ffffffU) == 0x00705770U);
	assert((baseline->registers[0x0ca0U] & 0xffffU) == 0xa501U);
	assert((baseline->registers[0x0ea0U] & 0xffffU) == 0xa501U);
	assert((baseline->registers[0x0608U] & 0x10000000U) != 0U);
	assert((baseline->registers[0x0522U] & 0xffU) == 0U);
	{
		size_t pause = trace_find(baseline, 0U, 0x0522U, 1U, 0xffU);
		size_t slot = trace_find(baseline, pause + 1U, 0x051bU, 1U,
		    0x09U);
		size_t pifs = trace_find(baseline, slot + 1U, 0x0512U, 1U,
		    0x19U);
		size_t vo = trace_find(baseline, pifs + 1U, 0x0502U, 2U,
		    0x0186U);
		size_t txagc = trace_find(baseline, vo + 1U, 0x1d00U, 4U,
		    0x1c1c1c1cU);
		size_t unpause = trace_find(baseline, txagc + 1U, 0x0522U, 1U,
		    0U);

		assert(pause < slot && slot < pifs && pifs < vo && vo < txagc &&
		    txagc < unpause);
	}
	assert(baseline->one_us_delays != 0U);
	assert(baseline->thirteen_us_delays != 0U);
	assert(baseline->now < 100000000U);
	assert(radio.power_limits_valid != 0U);
	assert_legacy_txagc(baseline, 0x1d00U, 0x1c1c1c1cU,
	    0x1a1a1a1aU, 0x1a1a1a1aU);
	assert_legacy_txagc(baseline, 0x1d80U, 0x18181818U,
	    0x1b1b1b1bU, 0x1b1b1b1bU);
	assert(rtl8822b_radio_active_scan_allowed(&radio, 1U));
	assert(rtl8822b_radio_active_scan_allowed(&radio, 11U));
	assert(!rtl8822b_radio_active_scan_allowed(&radio, 12U));

	make_probe_request(frame, &board);
	assert(rtl8822b_radio_management_frame_prepare(&radio, wire,
	    sizeof(wire), frame, sizeof(frame), &wire_length) == 0);
	assert(wire_length == 48U + sizeof(frame));
	assert((get_le32(wire) & 0xffffU) == sizeof(frame));
	assert(((get_le32(wire + 4U) >> 8) & 0x1fU) == 18U);
	assert((get_le32(wire + 12U) & ((1U << 8) | (1U << 10))) ==
	    ((1U << 8) | (1U << 10)));
	assert((get_le32(wire + 16U) & 0x7fU) == 0U);
	checksum = 0U;
	for (index = 0U; index < 16U; index++)
		checksum ^= get_le16(wire + index * 2U);
	assert(checksum == 0U);
	assert(memcmp(wire + 48U, frame, sizeof(frame)) == 0);
	assert(rtl8822b_radio_management_frame_prepare(&radio, wire, 60U,
	    frame, sizeof(frame), &wire_length) == ENOSPC);
	frame[4U] = 0U;
	assert(rtl8822b_radio_management_frame_prepare(&radio, wire,
	    sizeof(wire), frame, sizeof(frame), &wire_length) == EINVAL);
	make_probe_request(frame, &board);
	frame[10U] ^= 1U;
	assert(rtl8822b_radio_management_frame_prepare(&radio, wire,
	    sizeof(wire), frame, sizeof(frame), &wire_length) == EINVAL);
	make_probe_request(frame, &board);
	frame[1U] = 1U;
	assert(rtl8822b_radio_management_frame_prepare(&radio, wire,
	    sizeof(wire), frame, sizeof(frame), &wire_length) == EINVAL);
	memset(padded_frame, 0, sizeof(padded_frame));
	padded_frame[0] = 0x40U;
	memset(padded_frame + 4U, 0xff, 6U);
	memcpy(padded_frame + 10U, board.mac_address, 6U);
	memset(padded_frame + 16U, 0xff, 6U);
	assert(rtl8822b_radio_management_frame_prepare(&radio, padded_wire,
	    sizeof(padded_wire), padded_frame, sizeof(padded_frame),
	    &wire_length) == 0);
	assert(wire_length == sizeof(padded_wire));
	assert(padded_wire[sizeof(padded_wire) - 1U] == 0U);
	make_probe_request(frame, &board);

	/* The four channel-power groups change exactly at 2/3, 5/6, and 8/9. */
	assert(rtl8822b_radio_set_channel(&radio, 2U, UINT64_MAX) == 0);
	assert_legacy_txagc(baseline, 0x1d00U, 0x1c1c1c1cU,
	    0x1e1e1e1eU, 0x1c1e1e1eU);
	assert(rtl8822b_radio_set_channel(&radio, 3U, UINT64_MAX) == 0);
	assert_legacy_txagc(baseline, 0x1d00U, 0x1d1d1d1dU,
	    0x1f1f1f1fU, 0x1d1f1f1fU);
	assert(rtl8822b_radio_set_channel(&radio, 5U, UINT64_MAX) == 0);
	assert_legacy_txagc(baseline, 0x1d00U, 0x1d1d1d1dU,
	    0x1f1f1f1fU, 0x1d1f1f1fU);
	assert(rtl8822b_radio_set_channel(&radio, 6U, UINT64_MAX) == 0);
	assert(radio.channel == 6U);
	assert((baseline->registers[0x2860U] & 0xffU) == 6U);
	assert((baseline->registers[0x2c60U] & 0xffU) == 6U);
	assert_legacy_txagc(baseline, 0x1d00U, 0x1e1e1e1eU,
	    0x20202020U, 0x1e202020U);
	assert_legacy_txagc(baseline, 0x1d80U, 0x1a1a1a1aU,
	    0x21212121U, 0x1f212121U);
	assert(rtl8822b_radio_set_channel(&radio, 8U, UINT64_MAX) == 0);
	assert_legacy_txagc(baseline, 0x1d00U, 0x1e1e1e1eU,
	    0x20202020U, 0x1e202020U);
	assert(rtl8822b_radio_set_channel(&radio, 9U, UINT64_MAX) == 0);
	assert_legacy_txagc(baseline, 0x1d00U, 0x1f1f1f1fU,
	    0x21212121U, 0x1f212121U);
	assert(rtl8822b_radio_set_channel(&radio, 11U, UINT64_MAX) == 0);
	assert(radio.channel == 11U);
	assert_legacy_txagc(baseline, 0x1d00U, 0x1f1f1f1fU,
	    0x1f1f1f1fU, 0x1f1f1f1fU);
	assert_legacy_txagc(baseline, 0x1d80U, 0x1b1b1b1bU,
	    0x20202020U, 0x20202020U);
	assert(rtl8822b_radio_set_channel(&radio, 0U, UINT64_MAX) ==
	    EOPNOTSUPP);
	assert(rtl8822b_radio_set_channel(&radio, 12U, UINT64_MAX) ==
	    EOPNOTSUPP);
	assert(radio.channel == 11U);

	/* One representative failing write at every externally visible stage. */
	stages[stage_count++] = 0U; /* pre-power */
	stages[stage_count++] = trace_find(baseline, 1U, 0x004aU, 1U, 0U);
	stages[stage_count++] = trace_find(baseline, 1U, 0x010cU, 2U,
	    0xf5a0U);
	stages[stage_count++] = trace_find(baseline, 1U, 0x0610U, 4U,
	    0x06f71f6cU);
	stages[stage_count++] = trace_find(baseline, 1U, 0x051bU, 1U,
	    0x09U);
	stages[stage_count++] = trace_find(baseline, 1U, 0x060fU, 1U, 4U);
	stages[stage_count++] = trace_find(baseline, 1U, 0x06a0U, 2U,
	    0xffffU);
	stages[stage_count++] = trace_find(baseline, 1U, 0x0800U, 4U,
	    0x9020d010U);
	stages[stage_count++] = trace_find(baseline, 1U, 0x081cU, 4U,
	    0xff000003U);
	stages[stage_count++] = trace_find(baseline, 1U, 0x0c90U, 4U,
	    0x00030000U);
	stages[stage_count++] = trace_find(baseline, 1U, 0x0e90U, 4U,
	    0x00030000U);
	stages[stage_count++] = trace_find_address(baseline, 1U, 0x0c08U, 4U);
	stages[stage_count++] = trace_find_address(baseline, 1U, 0x1990U, 4U);
	stages[stage_count++] = trace_find(baseline, 1U, 0x0483U, 1U, 0U);
	stages[stage_count++] = trace_find(baseline, 1U, 0x1d00U, 4U,
	    0x1c1c1c1cU);

	for (index = 0U; index < stage_count; index++) {
		struct fake_radio *failed = fake_radio_create();
		struct rtl8822b_radio_transport failed_transport =
		    fake_radio_transport(failed);
		struct rtl8822b_radio failed_radio;
		int error;

		memset(&failed_radio, 0, sizeof(failed_radio));
		failed->fail_write_at = stages[index];
		error = rtl8822b_radio_power_on(&failed_radio, &failed_transport,
		    &board, UINT64_MAX);
		if (error == 0)
			error = rtl8822b_radio_start(&failed_radio, UINT64_MAX);
		assert(error == EIO);
		assert_radio_off(failed, &failed_radio);
		fake_radio_destroy(failed);
	}

	/* A channel transport failure is fail-closed even if rollback succeeds. */
	{
		size_t fail_at = baseline->write_count + 8U;

		baseline->fail_write_at = fail_at;
		assert(rtl8822b_radio_set_channel(&radio, 6U,
		    UINT64_MAX) == EIO);
		assert_radio_off(baseline, &radio);
		baseline->fail_write_at = SIZE_MAX;
	}

	assert(rtl8822b_radio_stop(&radio, UINT64_MAX) == 0);
	assert_radio_off(baseline, &radio);
	assert(rtl8822b_radio_stop(&radio, UINT64_MAX) == 0);
	fake_radio_destroy(baseline);
}

static void
test_radio_deadline_and_stop_retry(void)
{
	struct rtl8822bu_board_info board = fake_board(3U);
	struct fake_radio *fake = fake_radio_create();
	struct rtl8822b_radio_transport transport = fake_radio_transport(fake);
	struct rtl8822b_radio radio;

	memset(&radio, 0, sizeof(radio));
	fake->automatic_power_ack = 0;
	assert(rtl8822b_radio_power_on(&radio, &transport, &board, 200U) ==
	    ETIMEDOUT);
	assert_radio_off(fake, &radio);
	assert(fake->read_count < RTL8822B_EFUSE_PHYSICAL_SIZE);

	fake_radio_destroy(fake);
	fake = fake_radio_create();
	transport = fake_radio_transport(fake);
	memset(&radio, 0, sizeof(radio));
	assert(rtl8822b_radio_power_on(&radio, &transport, &board,
	    UINT64_MAX) == 0);
	fake->automatic_llt_ack = 0;
	assert(rtl8822b_radio_start(&radio, fake->now + 200U) ==
	    ETIMEDOUT);
	assert_radio_off(fake, &radio);
	assert(fake->read_count < RTL8822B_EFUSE_PHYSICAL_SIZE);

	fake_radio_destroy(fake);
	fake = fake_radio_create();
	transport = fake_radio_transport(fake);
	memset(&radio, 0, sizeof(radio));
	assert(rtl8822b_radio_power_on(&radio, &transport, &board,
	    UINT64_MAX) == 0);
	fake->automatic_rf_lut_ack = 0;
	assert(rtl8822b_radio_start(&radio, UINT64_MAX) == ETIMEDOUT);
	assert_radio_off(fake, &radio);
	assert(fake->write_count < RADIO_TRACE_MAX);

	fake_radio_destroy(fake);
	fake = fake_radio_create();
	transport = fake_radio_transport(fake);
	memset(&radio, 0, sizeof(radio));
	assert(rtl8822b_radio_power_on(&radio, &transport, &board,
	    UINT64_MAX) == 0);
	fake->fail_write_at = fake->write_count;
	assert(rtl8822b_radio_stop(&radio, UINT64_MAX) == EIO);
	assert_radio_off(fake, &radio);
	/* A disconnected/error stop still leaves the object reusable. */
	fake->fail_write_at = SIZE_MAX;
	transport = fake_radio_transport(fake);
	assert(rtl8822b_radio_power_on(&radio, &transport, &board,
	    UINT64_MAX) == 0);
	assert(rtl8822b_radio_stop(&radio, UINT64_MAX) == 0);
	fake_radio_destroy(fake);
}

static void
test_radio_already_powered_rebind(void)
{
	struct rtl8822bu_board_info board = fake_board(2U);
	struct rtl8822bu_board_info invalid_board = board;
	struct fake_radio *fake = fake_radio_create();
	struct rtl8822b_radio_transport transport = fake_radio_transport(fake);
	struct rtl8822b_radio radio;
	size_t disable_start;
	size_t disable_32k;
	size_t disable_usb;
	size_t disable_gpio;
	size_t enable_tail;

	memset(&radio, 0, sizeof(radio));
	invalid_board.mac_address[0] |= 1U;
	assert(rtl8822b_radio_power_on(&radio, &transport, &invalid_board,
	    UINT64_MAX) == EINVAL);
	invalid_board = board;
	invalid_board.tx_power_2g[0].cck_base[0] = 0x40U;
	assert(rtl8822b_radio_power_on(&radio, &transport, &invalid_board,
	    UINT64_MAX) == EINVAL);
	invalid_board = board;
	invalid_board.tx_power_2g[1].bw40_base[3] = 0x40U;
	assert(rtl8822b_radio_power_on(&radio, &transport, &invalid_board,
	    UINT64_MAX) == EINVAL);
	invalid_board = board;
	invalid_board.tx_power_2g[0].ofdm_diff = -9;
	assert(rtl8822b_radio_power_on(&radio, &transport, &invalid_board,
	    UINT64_MAX) == EINVAL);
	invalid_board = board;
	invalid_board.tx_power_2g[1].ofdm_diff = 8;
	assert(rtl8822b_radio_power_on(&radio, &transport, &invalid_board,
	    UINT64_MAX) == EINVAL);
	/* Validation rejects direct-board callers before any device access. */
	assert(fake->write_count == 0U && fake->read_count == 0U);
	fake->registers[0x0100U] = 0xffU;
	fake->registers[0x00f5U] = 0U;
	assert(rtl8822b_radio_power_on(&radio, &transport, &board,
	    UINT64_MAX) == 0);
	assert(radio.state == RTL8822B_RADIO_POWERED);
	disable_start = trace_find(fake, 0U, 0x0093U, 1U, 0U);
	disable_32k = trace_find(fake, disable_start + 1U, 0x0000U, 1U,
	    0x20U);
	disable_usb = trace_find(fake, disable_32k + 1U, 0x0007U, 1U,
	    0x20U);
	disable_gpio = trace_find(fake, disable_usb + 1U, 0x0067U, 1U,
	    0U);
	enable_tail = trace_find(fake, disable_gpio + 1U, 0x0029U, 1U,
	    0xf9U);
	assert(disable_start < disable_32k && disable_32k < disable_usb &&
	    disable_usb < disable_gpio && disable_gpio < enable_tail);
	assert(rtl8822b_radio_stop(&radio, UINT64_MAX) == 0);
	assert_radio_off(fake, &radio);
	fake_radio_destroy(fake);
}

static void
test_radio_profile_costs(void)
{
	static const uint8_t cuts[] = {0U, 1U, 2U, 3U, 6U};
	static const uint8_t rfes[] = {2U, 3U, 5U};
	size_t maximum_writes = 0U;
	size_t maximum_reads = 0U;
	uint64_t maximum_delay = 0U;
	size_t cut_index;
	size_t rfe_index;

	for (rfe_index = 0U; rfe_index < sizeof(rfes); rfe_index++) {
		for (cut_index = 0U; cut_index < sizeof(cuts); cut_index++) {
			struct fake_radio *fake = fake_radio_create();
			struct rtl8822b_radio_transport transport =
			    fake_radio_transport(fake);
			struct rtl8822bu_board_info board =
			    fake_board(rfes[rfe_index]);
			struct rtl8822b_radio radio;

			board.chip.cut = cuts[cut_index];
			memset(&radio, 0, sizeof(radio));
			assert(rtl8822b_radio_power_on(&radio, &transport, &board,
			    UINT64_MAX) == 0);
			assert(rtl8822b_radio_start(&radio, UINT64_MAX) == 0);
			if (fake->write_count > maximum_writes)
				maximum_writes = fake->write_count;
			if (fake->read_count > maximum_reads)
				maximum_reads = fake->read_count;
			if (fake->delay_microseconds > maximum_delay)
				maximum_delay = fake->delay_microseconds;
			assert(fake->write_count < 5000U);
			assert(fake->delay_microseconds < 1000000U);
			assert(rtl8822b_radio_stop(&radio, UINT64_MAX) == 0);
			fake_radio_destroy(fake);
		}
	}
	{
		struct fake_radio *fake = fake_radio_create();
		struct rtl8822b_radio_transport transport =
		    fake_radio_transport(fake);
		struct rtl8822bu_board_info board = fake_board(2U);
		struct rtl8822b_radio radio;
		size_t start;
		size_t index;

		board.chip.rf_path_count = 1U;
		memset(&radio, 0, sizeof(radio));
		assert(rtl8822b_radio_power_on(&radio, &transport, &board,
		    UINT64_MAX) == 0);
		assert(rtl8822b_radio_start(&radio, UINT64_MAX) == 0);
		start = fake->trace_count;
		assert(rtl8822b_radio_set_channel(&radio, 6U, UINT64_MAX) == 0);
		for (index = start; index < fake->trace_count; index++) {
			uint16_t address = fake->trace[index].address;

			assert(address != 0x0e20U && address != 0x0ea0U &&
			    address != 0x0eb0U && address != 0x0eb4U &&
			    address != 0x0eb8U && address != 0x0ebcU);
			assert(address < 0x1d80U || address > 0x1dbcU);
		}
		assert(rtl8822b_radio_stop(&radio, UINT64_MAX) == 0);
		fake_radio_destroy(fake);
	}
	printf("rtl8822b radio profiles: max-writes=%zu max-reads=%zu "
	    "max-delay=%llu us\n", maximum_writes, maximum_reads,
	    (unsigned long long)maximum_delay);
	/* Pinned tables make this an executable 15-second timeout rationale. */
	assert(maximum_writes == 3108U);
	assert(maximum_reads == 137U);
	assert(maximum_delay == 412326U);
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
	test_radio_table_interpreter();
	test_radio_txagc_clamps();
	test_radio_lifecycle();
	test_radio_deadline_and_stop_retry();
	test_radio_already_powered_rebind();
	test_radio_profile_costs();
	if (argc == 2)
		test_pinned_blob(argv[1]);
	else
		assert(argc == 1);
	puts("rtl8822b core: PASS");
	return 0;
}
