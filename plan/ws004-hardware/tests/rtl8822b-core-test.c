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

#define FIXTURE_REG_MCUFW_CTRL       0x0080U
#define FIXTURE_REG_SYS_STATUS1      0x00f4U
#define FIXTURE_REG_CR               0x0100U
#define FIXTURE_REG_USB_CPWM         0xfe57U
#define FIXTURE_REG_USB_RPWM         0xfe58U
#define FIXTURE_REG_RX_PACKET_NUMBER 0x0284U
#define FIXTURE_MCUFW_INIT_READY     0x8000U
#define FIXTURE_RPWM_ACK               0x40U
#define FIXTURE_RPWM_TOGGLE            0x80U
#define FIXTURE_RPWM_ACK_TIMEOUT_US   15000U
#define FIXTURE_RX_RELEASE_ENABLE 0x00040000U
#define FIXTURE_RXDMA_IDLE        0x00020000U

static void test_radio_usb_profiles(void);
static void test_usb_tx_boundaries(void);
static int check_firmware_boundary(void *context, const struct rtl8822b_firmware_chunk *chunk);
static void assert_management_rate(const struct rtl8822b_radio *radio, unsigned rate);
static void test_management_band_rates(void);
static void test_hardware_etsi_plan(void);
static void test_c2h_rx_metadata(void);
static void test_radio_trx_path_a(void);
static void test_radio_wlan_only(void);
static void test_radio_transport_deadline(void);

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
	static const uint8_t bw40_base_5g[2][RTL8822B_5G_OFDM_GROUP_COUNT] = {
		{24U, 25U, 26U, 27U, 28U, 29U, 30U,
		    31U, 32U, 33U, 34U, 35U, 36U, 37U},
		{20U, 21U, 22U, 23U, 24U, 25U, 26U,
		    27U, 28U, 29U, 30U, 31U, 32U, 33U}
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
		memcpy(wanted + power_offset[path] + 0x12U,
		    bw40_base_5g[path], sizeof(bw40_base_5g[path]));
	}
	/* Low nibbles are signed OFDM deltas: path A -2, path B +3. */
	wanted[0x1bU] = 0xaeU;
	wanted[0x45U] = 0xb3U;
	/* The 5-GHz legacy OFDM deltas are path A -1 and path B +2. */
	wanted[0x30U] = 0xafU;
	wanted[0x5aU] = 0xb2U;
	append_efuse_block(physical, &used, 2U, wanted);
	append_efuse_block(physical, &used, 3U, wanted);
	append_efuse_block(physical, &used, 4U, wanted);
	append_efuse_block(physical, &used, 5U, wanted);
	append_efuse_block(physical, &used, 6U, wanted);
	append_efuse_block(physical, &used, 7U, wanted);
	append_efuse_block(physical, &used, 8U, wanted);
	append_efuse_block(physical, &used, 9U, wanted);
	append_efuse_block(physical, &used, 10U, wanted);
	append_efuse_block(physical, &used, 11U, wanted);
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
	assert(memcmp(board.tx_power_5g[0].bw40_base, wanted + 0x22U,
	    RTL8822B_5G_OFDM_GROUP_COUNT) == 0);
	assert(board.tx_power_5g[0].ofdm_diff == -1);
	assert(memcmp(board.tx_power_5g[1].bw40_base, wanted + 0x4cU,
	    RTL8822B_5G_OFDM_GROUP_COUNT) == 0);
	assert(board.tx_power_5g[1].ofdm_diff == 2);
	assert(rtl8822b_board_active_channel_allowed(&board, 36U));
	assert(rtl8822b_board_active_channel_allowed(&board, 48U));
	assert(!rtl8822b_board_active_channel_allowed(&board, 52U));
	logical[0x22U] = 0x40U;
	assert(rtl8822bu_board_parse(logical, sizeof(logical), sys_cfg,
	    &board) == 0);
	assert(rtl8822b_board_active_channel_allowed(&board, 1U));
	assert(!rtl8822b_board_active_channel_allowed(&board, 36U));
	logical[0x22U] = wanted[0x22U];
	logical[0xcbU] = 'U';
	logical[0xccU] = 'S';
	assert(rtl8822bu_board_parse(logical, sizeof(logical), sys_cfg,
	    &board) == 0);
	assert(!rtl8822b_board_active_channel_allowed(&board, 36U));
	logical[0xcbU] = wanted[0xcbU];
	logical[0xccU] = wanted[0xccU];
	logical[0xb8U] = 0x30U;
	assert(rtl8822bu_board_parse(logical, sizeof(logical), sys_cfg,
	    &board) == 0);
	assert(!rtl8822b_board_active_channel_allowed(&board, 36U));
	logical[0xb8U] = wanted[0xb8U];
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

/* Verifies that firmware events ignore frame status but retain layout bounds. */
static void
test_c2h_rx_metadata(void)
{
	struct rtl8822b_rx_packet packet;
	struct rx_state state;
	uint8_t bytes[160];
	uint8_t aggregate[120];
	size_t offset;
	size_t occupied;
	size_t aligned;
	size_t count;
	uint32_t word0;
	unsigned info_units;
	unsigned shift;
	int error;

	/* Marks every irrelevant frame-status field while keeping a valid C2H. */
	memset(bytes, 0, sizeof(bytes));
	word0 = 5U | 0x0000c000U | (7U << 20) | 0x0c000000U;
	put_le32(bytes, word0);
	put_le32(bytes + 4U, 0x7fU);
	put_le32(bytes + 8U, 0x10000000U);
	put_le32(bytes + 12U, 0x7fU);
	put_le32(bytes + 16U, 0x30U);
	put_le32(bytes + 20U, UINT32_MAX);
	bytes[24U] = 0x03U;
	bytes[25U] = 0x9aU;
	error = rtl8822b_rx_packet_parse(bytes, 32U, &packet);
	assert(error == 0);
	assert(packet.kind == RTL8822B_RX_C2H);
	assert(packet.payload == bytes + 24U);
	assert(packet.payload_length == 5U);
	assert(packet.c2h_id == 0x03U && packet.c2h_sequence == 0x9aU);
	assert(packet.aggregate_length == 32U);
	assert(packet.rate == 0U && packet.bandwidth == 0U);
	assert(packet.tsf_low == 0U && packet.rssi_dbm == -128);
	assert(packet.phy_info == NULL && packet.phy_info_length == 0U);
	assert(packet.encryption_type == 0U && !packet.software_decrypted);
	assert(packet.mac_id == 0U && !packet.icv_error);

	/* Rejects the same frame-status violations without the C2H discriminator. */
	put_le32(bytes + 8U, 0U);
	error = rtl8822b_rx_packet_parse(bytes, 32U, &packet);
	assert(error == EINVAL);
	assert(packet.payload == NULL && packet.payload_length == 0U);

	/* Exercises every encoded padding offset without interpreting PHY bytes. */
	for (info_units = 0U; info_units < 16U; info_units++) {
		/* Covers each shift and both complete and truncated record ends. */
		for (shift = 0U; shift < 4U; shift++) {
			/* Supplies a command after bounded, deliberately unknown PHY data. */
			memset(bytes, 0xff, sizeof(bytes));
			word0 = 5U | (info_units << 16) | (shift << 24) |
			    0x04000000U;
			put_le32(bytes, word0);
			put_le32(bytes + 8U, 0x10000000U);
			offset = 24U + info_units * 8U + shift;
			bytes[offset] = 0x03U;
			bytes[offset + 1U] = 0x9aU;
			occupied = offset + 5U;
			aligned = (occupied + 7U) & ~(size_t)7U;
			error = rtl8822b_rx_packet_parse(bytes, aligned, &packet);
			assert(error == 0);
			assert(packet.kind == RTL8822B_RX_C2H);
			assert(packet.payload == bytes + offset);
			assert(packet.payload_length == 5U);
			assert(packet.c2h_id == 0x03U);
			assert(packet.c2h_sequence == 0x9aU);
			assert(packet.aggregate_length == aligned);
			assert(packet.phy_info == NULL && packet.phy_info_length == 0U);
			assert(packet.rate == 0U && packet.rssi_dbm == -128);

			/* Accepts a complete unpadded final record. */
			error = rtl8822b_rx_packet_parse(bytes, occupied, &packet);
			assert(error == 0);
			assert(packet.aggregate_length == occupied);

			/* Rejects a truncated command without publishing partial metadata. */
			error = rtl8822b_rx_packet_parse(bytes, occupied - 1U, &packet);
			assert(error == EINVAL);
			assert(packet.payload == NULL && packet.payload_length == 0U);

			/* Retains the rule against a partially present alignment suffix. */
			if (aligned > occupied + 1U) {
				error = rtl8822b_rx_packet_parse(bytes, occupied + 1U, &packet);
				assert(error == EINVAL);
			}
		}
	}

	/* Keeps a valid frame and C2H visible through whole-aggregate preflight. */
	make_rx_aggregate(aggregate);
	put_le32(aggregate + 88U, 5U | 0x04004000U);
	put_le32(aggregate + 88U + 12U, 0x7fU);
	memset(&state, 0, sizeof(state));
	state.fail_at = UINT_MAX;
	error = rtl8822b_rx_aggregate_walk(
		aggregate,
		sizeof(aggregate),
		check_rx_packet,
		&state,
		&count);
	assert(error == 0);
	assert(count == 2U && state.index == 2U);
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
	uint64_t expected_deadline;
	size_t finite_deadline_calls;
	size_t cleanup_deadline_calls;
	uint64_t delay_microseconds;
	int automatic_power_ack;
	int automatic_rpwm_ack;
	int automatic_llt_ack;
	int automatic_rf_lut_ack;
	uint32_t coex_grants;
	size_t coex_reads;
	size_t coex_fail_read_at;
	size_t coex_commands;
	uint64_t coex_first_read_tick;
	int coex_busy;
	int coex_drop_write;
	int coex_drop_owner;
	int coex_drop_dpdt;
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
	fake->automatic_rpwm_ack = 1;
	fake->automatic_llt_ack = 1;
	fake->automatic_rf_lut_ack = 1;
	fake->coex_fail_read_at = SIZE_MAX;
	fake->registers[0x1700U] = 0x20000000U;
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
	uint32_t *value, uint64_t deadline_ticks)
{
	struct fake_radio *fake = context;

	assert(value != NULL);
	assert(width == 1U || width == 2U || width == 4U);

	/* Verify exact caller deadlines separately from bounded emergency cleanup. */
	if (deadline_ticks == UINT64_MAX) {
		fake->cleanup_deadline_calls++;
	} else {
		fake->finite_deadline_calls++;
		if (fake->expected_deadline != 0U)
			assert(deadline_ticks == fake->expected_deadline);
	}
	if (fake->now >= deadline_ticks)
		return ETIMEDOUT;
	if (fake->read_count++ == fake->fail_read_at)
		return EIO;

	/* Model independent transport failures and the indirect port's ready signal. */
	if (address == 0x1700U || address == 0x1708U) {
		/* Retain the start of grant polling for a real shared-deadline test. */
		if (fake->coex_reads == 0U)
			fake->coex_first_read_tick = fake->now;

		/* Fail the requested grant-port operation without changing its output. */
		if (fake->coex_reads++ == fake->coex_fail_read_at)
			return EIO;
	}
	*value = fake->registers[address];

	/* A busy port never admits a new command. */
	if (address == 0x1700U && fake->coex_busy)
		*value &= ~0x20000000U;
	if (width == 1U)
		*value &= 0xffU;
	else if (width == 2U)
		*value &= 0xffffU;
	fake->now++;
	return 0;
}

static int
fake_radio_write(void *context, uint16_t address, unsigned width,
	uint32_t value, uint64_t deadline_ticks)
{
	struct fake_radio *fake = context;
	uint32_t mask;

	assert(width == 1U || width == 2U || width == 4U);

	/* Preserve the absolute budget instead of inventing a fresh callback deadline. */
	if (deadline_ticks == UINT64_MAX) {
		fake->cleanup_deadline_calls++;
	} else {
		fake->finite_deadline_calls++;
		if (fake->expected_deadline != 0U)
			assert(deadline_ticks == fake->expected_deadline);
	}
	if (fake->now >= deadline_ticks)
		return ETIMEDOUT;
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

	/* Model the hardware indirect grant register separately from its data window. */
	if (address == 0x1700U && width == 4U) {
		assert(value == 0x800f0038U || value == 0xc00f0038U);
		fake->coex_commands++;

		/* Complete reads and writes without fabricating a requested grant state. */
		if (value == 0x800f0038U) {
			fake->registers[0x1708U] = fake->coex_grants;
		} else if (!fake->coex_drop_write) {
			fake->coex_grants = fake->registers[0x1704U];
		}
		fake->registers[address] |= 0x20000000U;
	}

	/* Accepted but ineffective ownership writes must fail the production readback. */
	if (address == 0x0073U && fake->coex_drop_owner)
		fake->registers[address] &= ~0x04U;

	/* Keep the DPDT selection stale only when the WLAN-only profile selects it. */
	if (address == 0x004cU && fake->coex_drop_dpdt &&
	    (value & 0x01800000U) == 0x01000000U)
		fake->registers[address] &= ~0x01800000U;
	if (address == FIXTURE_REG_RX_PACKET_NUMBER && width == 4U &&
	    (value & FIXTURE_RX_RELEASE_ENABLE) != 0U)
		fake->registers[address] |= FIXTURE_RXDMA_IDLE;
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
	if (fake->automatic_rpwm_ack && address == FIXTURE_REG_USB_RPWM &&
	    width == 1U)
		fake->registers[FIXTURE_REG_USB_CPWM] =
		    (fake->registers[FIXTURE_REG_USB_CPWM] &
		    ~FIXTURE_RPWM_TOGGLE) |
		    (value & FIXTURE_RPWM_TOGGLE);
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
	transport.usb_bulk_max_packet_size = 512U;
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
	static const uint8_t bw40_base_5g[2][RTL8822B_5G_OFDM_GROUP_COUNT] = {
		{24U, 25U, 26U, 27U, 28U, 29U, 30U,
		    31U, 32U, 33U, 34U, 35U, 36U, 37U},
		{20U, 21U, 22U, 23U, 24U, 25U, 26U,
		    27U, 28U, 29U, 30U, 31U, 32U, 33U}
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
	memcpy(board.tx_power_5g[0].bw40_base, bw40_base_5g[0],
	    sizeof(bw40_base_5g[0]));
	board.tx_power_5g[0].ofdm_diff = -1;
	memcpy(board.tx_power_5g[1].bw40_base, bw40_base_5g[1],
	    sizeof(bw40_base_5g[1]));
	board.tx_power_5g[1].ofdm_diff = 2;
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

/* Emergency cleanup closes admission without proving the full stop transaction. */
static void
assert_radio_unavailable(const struct fake_radio *fake,
	const struct rtl8822b_radio *radio)
{
	if (radio->state == RTL8822B_RADIO_OFF) {
		assert_radio_off(fake, radio);
		return;
	}
	assert(radio->state == RTL8822B_RADIO_STOPPING);
	assert(radio->transport.read != NULL);
	assert(!rtl8822b_radio_active_scan_allowed(radio, 1U));
	assert((fake->registers[0x0100U] & 0xffU) == 0U);
	assert((fake->registers[0x001fU] & 0x07U) == 0U);
	assert((fake->registers[0x00ecU] & 0x07000000U) == 0U);
}

/* Verifies absolute deadlines through startup, retuning and expired cleanup. */
static void
test_radio_transport_deadline(
	void)
{
	struct rtl8822bu_board_info board;
	struct rtl8822b_radio_transport transport;
	struct rtl8822b_radio radio;
	struct fake_radio *fake;
	size_t calls;

	/* Require every startup register callback to retain the same caller budget. */
	fake = fake_radio_create();
	board = fake_board(3U);
	board.chip.cut = 3U;
	transport = fake_radio_transport(fake);
	transport.usb_bulk_max_packet_size = 1024U;
	memset(&radio, 0, sizeof(radio));
	fake->expected_deadline = 1000000U;
	assert(rtl8822b_radio_power_on(&radio, &transport, &board,
	    fake->expected_deadline) == 0);
	assert(rtl8822b_radio_start(&radio, fake->expected_deadline) == 0);
	assert(fake->finite_deadline_calls != 0U);
	assert(fake->cleanup_deadline_calls == 0U);

	/* Carry a different channel deadline through every register and RF operation. */
	calls = fake->finite_deadline_calls;
	fake->expected_deadline = fake->now + 100000U;
	assert(rtl8822b_radio_set_channel(&radio, 44U,
	    fake->expected_deadline) == 0);
	assert(fake->finite_deadline_calls > calls);
	assert(fake->cleanup_deadline_calls == 0U);

	/* Reject an expired operation while still allowing the bounded emergency stop. */
	calls = fake->finite_deadline_calls;
	fake->expected_deadline = fake->now;
	assert(rtl8822b_radio_set_channel(&radio, 6U,
	    fake->expected_deadline) == ETIMEDOUT);
	assert(fake->finite_deadline_calls == calls);
	assert(fake->cleanup_deadline_calls != 0U);
	assert_radio_unavailable(fake, &radio);
	fake_radio_destroy(fake);
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
	size_t stages[18];
	size_t stage_count = 0U;
	uint8_t frame[26];
	uint8_t directed_frame[64];
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
	assert(rtl8822b_radio_rx_generation_pause(&radio, UINT64_MAX) == 0);
	assert(radio.rx_generation_paused &&
	    (baseline->registers[0x0100U] & 0x8aU) == 0U &&
	    (baseline->registers[FIXTURE_REG_RX_PACKET_NUMBER] &
	    (FIXTURE_RX_RELEASE_ENABLE | FIXTURE_RXDMA_IDLE)) ==
	    (FIXTURE_RX_RELEASE_ENABLE | FIXTURE_RXDMA_IDLE));
	assert(rtl8822b_radio_rx_generation_pause(&radio, UINT64_MAX) == 0);
	assert(rtl8822b_radio_rx_generation_resume(&radio, UINT64_MAX) == 0);
	assert(!radio.rx_generation_paused &&
	    (baseline->registers[0x0100U] & 0x8aU) == 0x8aU &&
	    (baseline->registers[FIXTURE_REG_RX_PACKET_NUMBER] &
	    FIXTURE_RX_RELEASE_ENABLE) == 0U);
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
	assert(rtl8822b_radio_active_scan_allowed(&radio, 36U));
	assert(rtl8822b_radio_active_scan_allowed(&radio, 44U));
	assert(rtl8822b_radio_active_scan_allowed(&radio, 48U));
	assert(!rtl8822b_radio_active_scan_allowed(&radio, 12U));
	assert(!rtl8822b_radio_active_scan_allowed(&radio, 52U));

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
	/* The common reconnect scan emits the same broadcast DA/BSSID Probe
	 * Request with one non-empty SSID IE and the legacy basic rates IE. */
	memset(directed_frame, 0, sizeof(directed_frame));
	directed_frame[0] = 0x40U;
	memset(directed_frame + 4U, 0xff, 6U);
	memcpy(directed_frame + 10U, board.mac_address, 6U);
	memset(directed_frame + 16U, 0xff, 6U);
	directed_frame[24U] = 0U;
	directed_frame[25U] = 6U;
	memcpy(directed_frame + 26U, "zedBSD", 6U);
	directed_frame[32U] = 1U;
	directed_frame[33U] = 4U;
	memcpy(directed_frame + 34U,
	    (const uint8_t[]){ 0x82U, 0x84U, 0x8bU, 0x96U }, 4U);
	assert(rtl8822b_radio_management_frame_prepare(&radio, wire,
	    sizeof(wire), directed_frame, 38U, &wire_length) == 0);
	assert(wire_length == 48U + 38U &&
	    memcmp(wire + 48U, directed_frame, 38U) == 0);
	/* More than 32 bytes and duplicate SSID IEs are not valid directed
	 * requests, even if one of the duplicates is a wildcard. */
	directed_frame[25U] = 33U;
	assert(rtl8822b_radio_management_frame_prepare(&radio, wire,
	    sizeof(wire), directed_frame, 38U, &wire_length) == EINVAL);
	directed_frame[25U] = 6U;
	directed_frame[38U] = 0U;
	directed_frame[39U] = 0U;
	assert(rtl8822b_radio_management_frame_prepare(&radio, wire,
	    sizeof(wire), directed_frame, 40U, &wire_length) == EINVAL);
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
	/* One wildcard SSID followed by two bounded vendor IEs fills the padded
	 * frame without accidentally creating duplicate zero-length SSID IEs. */
	padded_frame[24U] = 0U;
	padded_frame[25U] = 0U;
	padded_frame[26U] = 221U;
	padded_frame[27U] = 255U;
	padded_frame[283U] = 221U;
	padded_frame[284U] = 179U;
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
	assert(rtl8822b_radio_set_channel(&radio, 44U, UINT64_MAX) == 0);
	assert(radio.channel == 44U);
	assert((baseline->registers[0x0454U] & 0x80U) != 0U);
	assert((baseline->registers[0x0808U] & 0x10000000U) == 0U);
	assert((baseline->registers[0x0a80U] & 0x00040000U) != 0U);
	assert((baseline->registers[0x0814U] & 0x0000fc00U) == 0x00008800U);
	assert((baseline->registers[0x0958U] & 0x1fU) == 1U);
	assert((baseline->registers[0x0860U] & 0x1ffe0000U) == 0x09280000U);
	assert((baseline->registers[0x2860U] & 0x00070fffU) == 0x00010d2cU);
	assert((baseline->registers[0x2c60U] & 0x00070fffU) == 0x00010d2cU);
	assert((baseline->registers[0x2af8U] & 0x00038000U) == 0U);
	assert(baseline->registers[0x082cU] == 0x75b76010U);
	assert(baseline->registers[0x0830U] == 0x79a0eaaaU);
	assert(baseline->registers[0x0838U] == 0x87766431U);
	assert((baseline->registers[0x0cb0U] & 0x00ffffffU) == 0x00177517U);
	assert((baseline->registers[0x0eb0U] & 0x00ffffffU) == 0x00177517U);
	assert((baseline->registers[0x0cb4U] & 0x0000ff00U) == 0x00007500U);
	assert((baseline->registers[0x0ca0U] & 0xffffU) == 0xa501U);
	assert_legacy_txagc(baseline, 0x1d00U, 0U, 0x16161616U,
	    0x16161616U);
	assert_legacy_txagc(baseline, 0x1d80U, 0U, 0x15151515U,
	    0x15151515U);
	assert(rtl8822b_radio_set_channel(&radio, 1U, UINT64_MAX) == 0);
	assert(radio.channel == 1U);
	assert((baseline->registers[0x0454U] & 0x80U) == 0U);
	assert((baseline->registers[0x0808U] & 0x10000000U) != 0U);
	assert((baseline->registers[0x0a80U] & 0x00040000U) == 0U);
	assert_legacy_txagc(baseline, 0x1d00U, 0x1c1c1c1cU,
	    0x1a1a1a1aU, 0x1a1a1a1aU);
	assert(rtl8822b_radio_set_channel(&radio, 0U, UINT64_MAX) ==
	    EOPNOTSUPP);
	assert(rtl8822b_radio_set_channel(&radio, 12U, UINT64_MAX) ==
	    EOPNOTSUPP);
	assert(rtl8822b_radio_set_channel(&radio, 52U, UINT64_MAX) ==
	    EOPNOTSUPP);
	assert(radio.channel == 1U);

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
	stages[stage_count++] = trace_find(baseline, 1U, 0x093cU, 4U,
	    0x001c0642U);
	stages[stage_count] = trace_find(baseline,
	    stages[stage_count - 1U] + 1U, 0x0a04U, 4U, 0x81ff800cU);
	stage_count++;
	stages[stage_count++] = trace_find(baseline, 1U, 0x0a04U, 4U,
	    0x80ff800cU);
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
		assert_radio_unavailable(failed, &failed_radio);
		fake_radio_destroy(failed);
	}

	/* A channel transport failure is fail-closed even if rollback succeeds. */
	{
		size_t fail_at = baseline->write_count + 8U;

		baseline->fail_write_at = fail_at;
		assert(rtl8822b_radio_set_channel(&radio, 6U,
		    UINT64_MAX) == EIO);
		assert_radio_unavailable(baseline, &radio);
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
	size_t stop_writes;

	memset(&radio, 0, sizeof(radio));
	fake->automatic_power_ack = 0;
	assert(rtl8822b_radio_power_on(&radio, &transport, &board, 200U) ==
	    ETIMEDOUT);
	assert_radio_unavailable(fake, &radio);
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
	assert_radio_unavailable(fake, &radio);
	assert(fake->read_count < RTL8822B_EFUSE_PHYSICAL_SIZE);

	fake_radio_destroy(fake);
	fake = fake_radio_create();
	transport = fake_radio_transport(fake);
	memset(&radio, 0, sizeof(radio));
	assert(rtl8822b_radio_power_on(&radio, &transport, &board,
	    UINT64_MAX) == 0);
	fake->automatic_rf_lut_ack = 0;
	assert(rtl8822b_radio_start(&radio, UINT64_MAX) == ETIMEDOUT);
	assert_radio_unavailable(fake, &radio);
	assert(fake->write_count < RADIO_TRACE_MAX);

	fake_radio_destroy(fake);
	fake = fake_radio_create();
	transport = fake_radio_transport(fake);
	memset(&radio, 0, sizeof(radio));
	assert(rtl8822b_radio_power_on(&radio, &transport, &board,
	    UINT64_MAX) == 0);
	fake->fail_write_at = fake->write_count;
	assert(rtl8822b_radio_stop(&radio, UINT64_MAX) == EIO);
	assert(radio.state == RTL8822B_RADIO_STOPPING);
	stop_writes = fake->write_count;
	assert(rtl8822b_radio_power_on(&radio, &transport, &board, UINT64_MAX) == EINVAL);
	assert(rtl8822b_radio_start(&radio, UINT64_MAX) == EINVAL);
	assert(rtl8822b_radio_set_channel(&radio, 1U, UINT64_MAX) == EINVAL);
	assert(fake->write_count == stop_writes);
	/* A failed stop retains the transport; retry must issue the inverse again
	 * without a new power-on that could hide incomplete teardown. */
	stop_writes = fake->write_count;
	fake->fail_write_at = SIZE_MAX;
	assert(rtl8822b_radio_stop(&radio, UINT64_MAX) == 0);
	assert(fake->write_count > stop_writes);
	assert_radio_off(fake, &radio);
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

	/* Retained firmware must acknowledge the USB RPWM toggle before the
	 * checked disable/enable transaction.  Other request bits are cleared. */
	fake = fake_radio_create();
	transport = fake_radio_transport(fake);
	memset(&radio, 0, sizeof(radio));
	fake->registers[FIXTURE_REG_CR] = 0xffU;
	fake->registers[FIXTURE_REG_SYS_STATUS1 + 1U] = 0U;
	fake->registers[FIXTURE_REG_MCUFW_CTRL] = FIXTURE_MCUFW_INIT_READY;
	fake->registers[FIXTURE_REG_USB_RPWM] = 0x35U;
	fake->registers[FIXTURE_REG_USB_CPWM] = FIXTURE_RPWM_TOGGLE;
	assert(rtl8822b_radio_power_on(&radio, &transport, &board,
	    UINT64_MAX) == 0);
	disable_start = trace_find(fake, 0U, FIXTURE_REG_USB_RPWM, 1U,
	    FIXTURE_RPWM_ACK | FIXTURE_RPWM_TOGGLE);
	enable_tail = trace_find(fake, disable_start + 1U, 0x0029U, 1U,
	    0xf9U);
	assert(disable_start < enable_tail);
	assert(rtl8822b_radio_stop(&radio, UINT64_MAX) == 0);
	fake_radio_destroy(fake);

	/* A stalled warm firmware is never reused.  The finite handshake expires,
	 * then the full card-disable transaction recovers the object for a fresh
	 * firmware download by the USB owner. */
	fake = fake_radio_create();
	transport = fake_radio_transport(fake);
	memset(&radio, 0, sizeof(radio));
	fake->registers[FIXTURE_REG_CR] = 0xffU;
	fake->registers[FIXTURE_REG_SYS_STATUS1 + 1U] = 0U;
	fake->registers[FIXTURE_REG_MCUFW_CTRL] = FIXTURE_MCUFW_INIT_READY;
	fake->automatic_rpwm_ack = 0;
	assert(rtl8822b_radio_power_on(&radio, &transport, &board,
	    UINT64_MAX) == 0);
	assert(fake->delay_microseconds >= FIXTURE_RPWM_ACK_TIMEOUT_US);
	assert(radio.state == RTL8822B_RADIO_POWERED);
	assert(rtl8822b_radio_stop(&radio, UINT64_MAX) == 0);
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
	assert(maximum_writes == 3117U);
	assert(maximum_reads == 151U);
	assert(maximum_delay == 412326U);
}

/* Checks the one-stream and CCK path selection on both supported RF layouts. */
static void
test_radio_trx_path_a(
	void)
{
	static const uint8_t rfes[] = { 2U, 3U };
	static const uint8_t cuts[] = { 1U, 3U };
	static const uint16_t packets[] = { 512U, 1024U };
	struct rtl8822bu_board_info board;
	struct rtl8822b_radio_transport transport;
	struct rtl8822b_radio radio;
	struct fake_radio *fake;
	size_t profile;
	size_t speed;
	size_t selection;
	size_t transmit;
	unsigned paths;

	/* Cover the older RFE2 and measured cut-D/RFE3 board at both USB speeds. */
	for (profile = 0U; profile < sizeof(rfes); profile++) {
		for (speed = 0U; speed < sizeof(packets) / sizeof(packets[0]);
		    speed++) {
			for (paths = 1U; paths <= 2U; paths++) {
				/* Run the production table and post-table initialization. */
				fake = fake_radio_create();
				transport = fake_radio_transport(fake);
				transport.usb_bulk_max_packet_size = packets[speed];
				board = fake_board(rfes[profile]);
				board.chip.cut = cuts[profile];
				board.chip.rf_path_count = (uint8_t)paths;
				memset(&radio, 0, sizeof(radio));
				assert(rtl8822b_radio_power_on(&radio, &transport,
				    &board, UINT64_MAX) == 0);
				assert(rtl8822b_radio_start(&radio, UINT64_MAX) == 0);

				/* Require path A for one stream and CCK, preserving other bits. */
				assert(fake->registers[0x093cU] == 0x001c0642U);
				assert(fake->registers[0x0a04U] == 0x80ff800cU);
				assert((fake->registers[0x0940U] & 0xfff0U) ==
				    (paths == 1U ? 0x0010U : 0x0430U));
				assert((fake->registers[0x080cU] & 0xffU) ==
				    (paths == 1U ? 0x11U : 0x33U));
				selection = trace_find(fake, 0U, 0x0a04U, 4U,
				    0x80ff800cU);
				transmit = trace_find(fake, selection + 1U,
				    0x0522U, 1U, 0U);
				assert(selection < transmit);

				/* Preserve the selection across either band's channel setup. */
				assert(rtl8822b_radio_set_channel(&radio, 44U,
				    UINT64_MAX) == 0);
				assert(rtl8822b_radio_set_channel(&radio, 6U,
				    UINT64_MAX) == 0);
				assert(fake->registers[0x093cU] == 0x001c0642U);
				assert(fake->registers[0x0a04U] == 0x80ff800cU);

				/* Release each full startup before exercising another profile. */
				assert(rtl8822b_radio_stop(&radio, UINT64_MAX) == 0);
				assert_radio_off(fake, &radio);
				fake_radio_destroy(fake);
			}
		}
	}
}

/* Verifies known WLAN-only grants, switch selection and startup failure boundaries. */
static void
test_radio_wlan_only(
	void)
{
	static const uint8_t options[] = { 0x00U, 0x01U, 0xffU, 0x20U, 0x3fU };
	struct rtl8822bu_board_info board;
	struct rtl8822b_radio_transport transport;
	struct rtl8822b_radio radio;
	struct fake_radio *fake;
	uint8_t physical[RTL8822B_EFUSE_PHYSICAL_SIZE];
	uint8_t logical[RTL8822B_EFUSE_LOGICAL_SIZE];
	size_t stages[6];
	size_t first;
	size_t grant_write;
	size_t owner;
	size_t dpdt;
	size_t band;
	size_t unpause;
	size_t index;
	size_t stage;
	uint64_t grant_tick;
	uint64_t deadline;
	unsigned failure;
	int eligible;
	int error;

	/* Parse the cut-D/RFE3 hardware policy with explicit synthetic board-option bytes. */
	make_efuse(physical, logical);
	logical[0xb8U] = 0xa6U;
	logical[0xcaU] = 3U;
	logical[0xcbU] = 0xffU;
	logical[0xccU] = 0xffU;
	grant_tick = 0U;

	/* Exercise programmed WLAN-only, unknown and Bluetooth-combination boards. */
	for (index = 0U; index < sizeof(options); index++) {
		/* Enter the production path through efuse parsing at SuperSpeed. */
		logical[0xc1U] = options[index];
		assert(rtl8822bu_board_parse(logical, sizeof(logical),
		    (3U << 12) | (1U << 27), &board) == 0);
		assert(board.rf_board_option == options[index]);
		fake = fake_radio_create();
		fake->coex_grants = 0x1234a580U;
		fake->registers[0x0073U] = 0xabU;
		fake->registers[0x004cU] = 0x00812345U;
		transport = fake_radio_transport(fake);
		transport.usb_bulk_max_packet_size = 1024U;
		memset(&radio, 0, sizeof(radio));
		assert(rtl8822b_radio_power_on(&radio, &transport, &board, UINT64_MAX) == 0);
		assert(rtl8822b_radio_start(&radio, UINT64_MAX) == 0);
		eligible = index < 2U;

		/* Preserve all unrelated grant and ownership bits on eligible boards. */
		if (eligible) {
			assert(fake->coex_commands == 3U);
			assert(fake->coex_grants == 0x12347700U);
			assert(fake->registers[0x0073U] == 0xafU);
			assert((fake->registers[0x004cU] & 0x01800000U) == 0x01000000U);
			assert((fake->registers[0x004cU] & 0x007fffffU) == 0x00012345U);
			assert((fake->registers[0x0cbcU] & 0x300U) == 0x200U);
		} else {
			assert(fake->coex_commands == 0U && fake->coex_reads == 0U);
			assert(fake->coex_grants == 0x1234a580U);
			assert(fake->registers[0x0073U] == 0xabU);
			assert((fake->registers[0x004cU] & 0x01800000U) == 0x00800000U);
			assert((fake->registers[0x0cbcU] & 0x300U) == 0x200U);
		}

		/* Retain each new acquisition boundary for independent failing-write runs. */
		if (index == 1U) {
			first = trace_find(fake, 0U, 0x1700U, 4U, 0x800f0038U);
			grant_write = trace_find(fake, first, 0x1704U, 4U, 0x12347700U);
			owner = trace_find(fake, grant_write, 0x0073U, 1U, 0xafU);
			dpdt = trace_find_address(fake, owner, 0x004cU, 4U);
			band = trace_find(fake, dpdt, 0x0cbcU, 4U, 0x200U);
			unpause = trace_find(fake, band, 0x0522U, 1U, 0U);
			assert(first < grant_write && grant_write < owner && owner < dpdt);
			assert(dpdt < band && band < unpause);
			stages[0] = first;
			stages[1] = grant_write;
			stages[2] = grant_write + 1U;
			stages[3] = grant_write + 2U;
			stages[4] = owner;
			stages[5] = dpdt;
			grant_tick = fake->coex_first_read_tick;
		}

		/* Preserve unrelated switch bits through W52 and 2.4-GHz round trips. */
		fake->registers[0x0cbcU] |= 0x80000000U;
		assert(rtl8822b_radio_set_channel(&radio, 44U, UINT64_MAX) == 0);
		assert((fake->registers[0x0cbcU] & 0x300U) == 0x100U);
		assert((fake->registers[0x0cbcU] & 0x80000000U) != 0U);
		assert(rtl8822b_radio_set_channel(&radio, 6U, UINT64_MAX) == 0);
		assert((fake->registers[0x0cbcU] & 0x300U) == 0x200U);
		assert((fake->registers[0x0cbcU] & 0x80000000U) != 0U);
		assert(rtl8822b_radio_stop(&radio, UINT64_MAX) == 0);
		assert_radio_off(fake, &radio);
		fake_radio_destroy(fake);
	}

	/* Parse one explicit WLAN-only board for all transport-failure cases. */
	logical[0xc1U] = 0x01U;
	assert(rtl8822bu_board_parse(logical, sizeof(logical),
	    (3U << 12) | (1U << 27), &board) == 0);

	/* Fail every new startup write and require the existing checked radio reset. */
	for (stage = 0U; stage < sizeof(stages) / sizeof(stages[0]); stage++) {
		fake = fake_radio_create();
		fake->coex_grants = 0x1234a580U;
		fake->registers[0x0073U] = 0xabU;
		fake->registers[0x004cU] = 0x00812345U;
		fake->fail_write_at = stages[stage];
		transport = fake_radio_transport(fake);
		transport.usb_bulk_max_packet_size = 1024U;
		memset(&radio, 0, sizeof(radio));
		assert(rtl8822b_radio_power_on(&radio, &transport, &board, UINT64_MAX) == 0);
		assert(rtl8822b_radio_start(&radio, UINT64_MAX) == EIO);
		assert_radio_unavailable(fake, &radio);
		fake_radio_destroy(fake);
	}

	/* Reject every grant-port read failure, ineffective writes and bounded busy ports. */
	for (failure = 0U; failure < 12U; failure++) {
		fake = fake_radio_create();
		fake->coex_grants = 0x1234a580U;
		transport = fake_radio_transport(fake);
		transport.usb_bulk_max_packet_size = 1024U;
		memset(&radio, 0, sizeof(radio));
		assert(rtl8822b_radio_power_on(&radio, &transport, &board, UINT64_MAX) == 0);
		deadline = UINT64_MAX;
		error = EIO;

		/* Select one real failure boundary without changing production behavior. */
		if (failure < 7U) {
			fake->coex_fail_read_at = failure;
		} else if (failure == 7U) {
			fake->coex_drop_write = 1;
		} else if (failure == 8U) {
			fake->coex_drop_owner = 1;
		} else if (failure == 9U) {
			fake->coex_drop_dpdt = 1;
		} else {
			fake->coex_busy = 1;
			error = ETIMEDOUT;

			/* Let the shared deadline expire during grant readiness polling. */
			if (failure == 11U)
				deadline = grant_tick + 50U;
		}
		assert(rtl8822b_radio_start(&radio, deadline) == error);
		assert_radio_unavailable(fake, &radio);

		/* A permanently busy port admits no command and stops at either bound. */
		if (failure >= 10U) {
			assert(fake->coex_commands == 0U);
			assert(fake->coex_reads <= 1000U);
			assert(fake->coex_reads == 1000U || failure == 11U);
			assert(fake->coex_reads > 0U);
		}
		fake_radio_destroy(fake);
	}
}

/* Checks probe and deauthentication rates from the radio's current channel. */
static void
assert_management_rate(
	const struct rtl8822b_radio *radio,
	unsigned rate)
{
	static const uint8_t bssid[6] = { 0x02U, 1U, 2U, 3U, 4U, 5U };
	uint8_t frame[26];
	uint8_t wire[80];
	size_t length;
	unsigned kind;
	unsigned index;
	uint16_t checksum;

	/* Check both management paths which share the production encoder. */
	make_probe_request(frame, &radio->board);
	for (kind = 0U; kind < 2U; kind++) {
		/* Construct each frame through its ordinary public entry point. */
		if (kind == 0U) {
			assert(rtl8822b_radio_management_frame_prepare(radio, wire,
			    sizeof(wire), frame, sizeof(frame), &length) == 0);
		} else {
			assert(rtl8822b_radio_deauthentication_prepare(radio, wire,
			    sizeof(wire), bssid, radio->board.mac_address,
			    3U, &length) == 0);
			assert(wire[48U] == 0xc0U);
			assert(get_le16(wire + 72U) == 3U);
		}

		/* Require fixed basic-rate selection and unchanged descriptor shape. */
		assert(length == 74U);
		assert((get_le32(wire + 12U) & (1U << 8)) != 0U);
		assert((get_le32(wire + 16U) & 0x7fU) == rate);
		assert(((get_le32(wire + 4U) >> 8) & 0x1fU) == 18U);

		/* Include the selected rate in the hardware descriptor checksum. */
		checksum = 0U;
		for (index = 0U; index < 16U; index++)
			checksum ^= get_le16(wire + index * 2U);
		assert(checksum == 0U);
	}
}

/* Checks the measured forced-ETSI board without inferring a country code. */
static void
test_hardware_etsi_plan(
	void)
{
	static const uint8_t channels[] = { 36U, 40U, 44U, 48U };
	static const uint8_t rejected_channels[] = {
		0U, 12U, 14U, 35U, 37U, 49U, 52U, 64U,
		100U, 140U, 149U, 165U, 255U
	};
	static const uint8_t rejected_profiles[][3] = {
		{ 0x26U, 0xffU, 0xffU }, { 0x27U, 0xffU, 0xffU },
		{ 0x7fU, 0xffU, 0xffU }, { 0xffU, 0xffU, 0xffU },
		{ 0xa7U, 0xffU, 0xffU }, { 0xa5U, 0xffU, 0xffU },
		{ 0x80U, 0xffU, 0xffU }, { 0xa6U, 'J', 'P' },
		{ 0xa6U, 'U', 'S' }, { 0xa6U, 'D', 'E' },
		{ 0xa6U, 0xffU, 'P' }, { 0xa6U, 'J', 0xffU },
		{ 0xa6U, 0U, 0U }
	};
	struct rtl8822bu_board_info board;
	struct rtl8822bu_board_info changed;
	struct rtl8822b_radio_transport transport;
	struct rtl8822b_radio radio;
	struct fake_radio *fake;
	uint8_t physical[RTL8822B_EFUSE_PHYSICAL_SIZE];
	uint8_t logical[RTL8822B_EFUSE_LOGICAL_SIZE];
	size_t index;
	unsigned path;
	unsigned group;

	/* Parse cut D/RFE 3 with the observed raw hardware policy bytes. */
	make_efuse(physical, logical);
	logical[0xb8U] = 0xa6U;
	logical[0xcaU] = 3U;
	logical[0xcbU] = 0xffU;
	logical[0xccU] = 0xffU;
	assert(rtl8822bu_board_parse(logical, sizeof(logical),
	    (3U << 12) | (1U << 27), &board) == 0);
	assert(board.channel_plan == 0xa6U);
	assert(board.country_code[0] == 0xffU && board.country_code[1] == 0xffU);
	assert(board.chip.cut == 3U && board.rfe_option == 3U);

	/* Admit only the existing non-DFS 5-GHz subset. */
	for (index = 0U; index < sizeof(channels); index++)
		assert(rtl8822b_board_active_channel_allowed(&board, channels[index]));
	for (index = 0U; index < sizeof(rejected_channels); index++)
		assert(!rtl8822b_board_active_channel_allowed(&board,
		    rejected_channels[index]));
	assert(rtl8822b_board_active_channel_allowed(&board, 1U));
	assert(rtl8822b_board_active_channel_allowed(&board, 11U));

	/* Reject unforced, unknown, missing and contradictory policy profiles. */
	for (index = 0U; index < sizeof(rejected_profiles) /
	    sizeof(rejected_profiles[0]); index++) {
		changed = board;
		changed.channel_plan = rejected_profiles[index][0];
		changed.country_code[0] = rejected_profiles[index][1];
		changed.country_code[1] = rejected_profiles[index][2];
		assert(!rtl8822b_board_active_channel_allowed(&changed, 44U));
		assert(rtl8822b_board_active_channel_allowed(&changed, 1U));
	}

	/* Keep both original Japan plan cases admitted. */
	changed = board;
	changed.country_code[0] = 'J';
	changed.country_code[1] = 'P';
	changed.channel_plan = 0x27U;
	assert(rtl8822b_board_active_channel_allowed(&changed, 44U));
	changed.channel_plan = 0x7fU;
	assert(rtl8822b_board_active_channel_allowed(&changed, 44U));

	/* Check every consumed calibration group and path before policy admission. */
	for (path = 0U; path < 2U; path++) {
		/* Reject missing and out-of-range W52 group calibration. */
		for (group = 0U; group < 2U; group++) {
			changed = board;
			changed.tx_power_5g[path].bw40_base[group] = 0x40U;
			assert(!rtl8822b_board_active_channel_allowed(&changed, 44U));
			changed.tx_power_5g[path].bw40_base[group] = 0xffU;
			assert(!rtl8822b_board_active_channel_allowed(&changed, 44U));
		}

		/* Reject invalid signed OFDM differences from direct board callers. */
		changed = board;
		changed.tx_power_5g[path].ofdm_diff = -9;
		assert(!rtl8822b_board_active_channel_allowed(&changed, 44U));
		changed.tx_power_5g[path].ofdm_diff = 8;
		assert(!rtl8822b_board_active_channel_allowed(&changed, 44U));
	}

	/* Exercise the new board through ordinary SuperSpeed radio startup. */
	fake = fake_radio_create();
	transport = fake_radio_transport(fake);
	transport.usb_bulk_max_packet_size = 1024U;
	memset(&radio, 0, sizeof(radio));
	assert(rtl8822b_radio_power_on(&radio, &transport, &board, UINT64_MAX) == 0);
	assert(rtl8822b_radio_start(&radio, UINT64_MAX) == 0);
	assert(rtl8822b_radio_set_channel(&radio, 44U, UINT64_MAX) == 0);
	assert_management_rate(&radio, 4U);
	assert(radio.board.channel_plan == 0xa6U);
	assert(radio.board.country_code[0] == 0xffU &&
	    radio.board.country_code[1] == 0xffU);
	assert(rtl8822b_radio_stop(&radio, UINT64_MAX) == 0);
	assert_radio_off(fake, &radio);
	fake_radio_destroy(fake);
}

/* Verifies W52 rates, the 2.4-GHz return and failed channel transitions. */
static void
test_management_band_rates(
	void)
{
	static const uint8_t channels[] = { 1U, 36U, 40U, 44U, 48U, 11U, 1U };
	static const uint8_t bssid[6] = { 0x02U, 1U, 2U, 3U, 4U, 5U };
	struct rtl8822bu_board_info board;
	struct rtl8822b_radio_transport transport;
	struct rtl8822b_radio radio;
	struct fake_radio *fake;
	uint8_t frame[26];
	uint8_t wire[80];
	size_t length;
	size_t index;
	unsigned speed;

	/* Select rates solely from the live channel at both USB packet sizes. */
	for (speed = 0U; speed < 2U; speed++) {
		/* Start the real radio transaction with a calibrated Japan board. */
		fake = fake_radio_create();
		board = fake_board(2U);
		transport = fake_radio_transport(fake);
		transport.usb_bulk_max_packet_size = speed == 0U ? 512U : 1024U;
		memset(&radio, 0, sizeof(radio));
		assert(rtl8822b_radio_power_on(&radio, &transport, &board,
		    UINT64_MAX) == 0);
		assert(rtl8822b_radio_start(&radio, UINT64_MAX) == 0);

		/* Visit every W52 channel and return to the accepted CCK profile. */
		for (index = 0U; index < sizeof(channels); index++) {
			assert(rtl8822b_radio_set_channel(&radio, channels[index],
			    UINT64_MAX) == 0);
			assert(radio.channel == channels[index]);
			assert_management_rate(&radio, index >= 1U && index <= 4U ?
			    4U : 0U);
		}

		/* A rejected unsupported channel must not change the selected rate. */
		assert(rtl8822b_radio_set_channel(&radio, 52U,
		    UINT64_MAX) == EOPNOTSUPP);
		assert(radio.channel == 1U);
		assert_management_rate(&radio, 0U);
		assert(rtl8822b_radio_set_channel(&radio, 44U, UINT64_MAX) == 0);
		assert_management_rate(&radio, 4U);

		/* Fail an actual W52-to-2.4-GHz register transaction. */
		make_probe_request(frame, &board);
		fake->fail_write_at = fake->write_count + 8U;
		assert(rtl8822b_radio_set_channel(&radio, 1U, UINT64_MAX) == EIO);
		assert_radio_unavailable(fake, &radio);

		/* Refuse both encoders after rollback leaves the radio unavailable. */
		length = sizeof(wire);
		assert(rtl8822b_radio_management_frame_prepare(&radio, wire,
		    sizeof(wire), frame, sizeof(frame), &length) != 0);
		assert(length == 0U);
		length = sizeof(wire);
		assert(rtl8822b_radio_deauthentication_prepare(&radio, wire,
		    sizeof(wire), bssid, board.mac_address, 3U, &length) == EPERM);
		assert(length == 0U);
		fake_radio_destroy(fake);
	}
}

/* Checks both packet sizes, every supported cut and PHY failure rollback. */
static void
test_radio_usb_profiles(
	void)
{
	struct rtl8822bu_board_info board;
	struct rtl8822b_radio_transport transport;
	struct rtl8822b_radio radio;
	struct fake_radio *fake;
	size_t phy_start;
	size_t index;
	unsigned speed;
	unsigned cut;
	unsigned stage;
	unsigned phy_writes;

	/* Reject unsupported transport sizes before any register access. */
	fake = fake_radio_create();
	board = fake_board(2U);
	transport = fake_radio_transport(fake);
	memset(&radio, 0, sizeof(radio));
	transport.usb_bulk_max_packet_size = 0U;
	assert(rtl8822b_radio_power_on(&radio, &transport, &board,
	    UINT64_MAX) == EINVAL);
	transport.usb_bulk_max_packet_size = 64U;
	assert(rtl8822b_radio_power_on(&radio, &transport, &board,
	    UINT64_MAX) == EINVAL);
	assert(fake->write_count == 0U && fake->read_count == 0U);
	fake_radio_destroy(fake);

	/* Exercise the cut table at both enumerated USB speeds. */
	phy_start = SIZE_MAX;
	for (speed = 0U; speed < 2U; speed++) {
		/* Preserve non-D cuts and check D's ordered three-register write. */
		for (cut = 0U; cut <= 6U; cut++) {
			/* Power up through the same private transport used by USB. */
			fake = fake_radio_create();
			board = fake_board(2U);
			board.chip.cut = (uint8_t)cut;
			transport = fake_radio_transport(fake);
			transport.usb_bulk_max_packet_size = speed == 0U ?
			    512U : 1024U;
			memset(&radio, 0, sizeof(radio));
			assert(rtl8822b_radio_power_on(&radio, &transport, &board,
			    UINT64_MAX) == 0);

			/* Count only USB PHY register writes in the complete trace. */
			phy_writes = 0U;
			for (index = 0U; index < fake->trace_count; index++) {
				/* Identify the PHY data and trigger registers. */
				if (fake->trace[index].address >= 0xff0cU &&
				    fake->trace[index].address <= 0xff0eU)
					phy_writes++;
			}

			/* Check the exact cut-D value and write order. */
			if (cut == 3U) {
				assert(phy_writes == 3U);
				phy_start = trace_find(fake, 0U, 0xff0dU, 1U,
				    0x41U);
				assert(fake->trace[phy_start + 1U].address ==
				    0xff0eU);
				assert(fake->trace[phy_start + 1U].value == 0xa8U);
				assert(fake->trace[phy_start + 2U].address ==
				    0xff0cU);
				assert(fake->trace[phy_start + 2U].value == 0x81U);
			} else {
				assert(phy_writes == 0U);
			}

			/* Check speed-specific DMA and speed-independent v1 aggregate. */
			assert(rtl8822b_radio_start(&radio, UINT64_MAX) == 0);
			assert((fake->registers[0x0290U] & 0xffU) ==
			    (speed == 0U ? 0x1eU : 0x0eU));
			assert((fake->registers[0x0280U] & 0xffffU) == 0x2005U);
			assert(rtl8822b_radio_set_channel(&radio, 44U,
			    UINT64_MAX) == 0);
			assert(rtl8822b_radio_active_scan_allowed(&radio, 44U));
			assert(rtl8822b_radio_stop(&radio, UINT64_MAX) == 0);
			assert_radio_off(fake, &radio);
			fake_radio_destroy(fake);
		}
	}

	/* Fail each cut-D PHY write and require complete radio rollback. */
	assert(phy_start != SIZE_MAX);
	for (stage = 0U; stage < 3U; stage++) {
		/* Inject one failure at the selected production write. */
		fake = fake_radio_create();
		board = fake_board(2U);
		board.chip.cut = 3U;
		transport = fake_radio_transport(fake);
		transport.usb_bulk_max_packet_size = 1024U;
		fake->fail_write_at = phy_start + stage;
		memset(&radio, 0, sizeof(radio));
		assert(rtl8822b_radio_power_on(&radio, &transport, &board,
		    UINT64_MAX) == EIO);
		assert_radio_unavailable(fake, &radio);
		assert(fake->registers[0xff0cU] == 0U);
		fake_radio_destroy(fake);
	}
}

/* Verifies the actual firmware chunk planner and descriptor at a boundary. */
static int
check_firmware_boundary(
	void *context,
	const struct rtl8822b_firmware_chunk *chunk)
{
	size_t *expected;
	uint8_t descriptor[RTL8822B_FIRMWARE_TX_DESCRIPTOR_SIZE];
	uint16_t checksum;
	size_t total;
	unsigned index;

	/* Preserve the memory destination length independently of USB padding. */
	expected = context;
	assert(chunk->length == expected[0]);
	assert(chunk->wire_payload_length == expected[1]);
	assert(chunk->file_offset == 0U && chunk->destination == 0x10000U);
	assert(chunk->first && chunk->last && !chunk->checksum_continue);
	total = chunk->wire_payload_length + sizeof(descriptor);
	assert(total % 512U != 0U && total % 1024U != 0U);
	assert(rtl8822b_firmware_tx_descriptor(descriptor,
	    chunk->wire_payload_length) == 0);
	assert(get_le16(descriptor) == expected[1]);

	/* Check descriptor integrity after the padded payload length is stored. */
	checksum = 0U;
	for (index = 0U; index < 16U; index++)
		checksum ^= get_le16(descriptor + index * 2U);
	assert(checksum == 0U);
	expected[2]++;

	/* Report this accepted chunk. */
	return 0;
}

/* Checks short-packet termination and payload preservation around USB sizes. */
static void
test_usb_tx_boundaries(
	void)
{
	struct rtl8822b_firmware_view view;
	struct rtl8822b_radio radio;
	uint8_t frame[1536];
	uint8_t wire[1540];
	size_t expected[3];
	size_t frame_length;
	size_t expected_wire;
	size_t wire_length;
	size_t offset;
	size_t ie_length;
	size_t remaining;
	size_t boundary;
	size_t wire_base;
	unsigned neighbor;
	unsigned speed;
	unsigned index;
	uint16_t checksum;

	/* Exercise each neighborhood using valid production probe requests. */
	memset(&radio, 0, sizeof(radio));
	radio.board = fake_board(2U);
	radio.state = RTL8822B_RADIO_STARTED;
	radio.channel = 1U;
	radio.power_limits_valid = 1U;
	memset(&view, 0, sizeof(view));
	view.size = sizeof(frame);
	for (boundary = 512U; boundary <= 1536U; boundary += 512U) {
		/* Test one byte below, exactly at and one byte above each boundary. */
		for (neighbor = 0U; neighbor < 3U; neighbor++) {
			/* Keep the firmware's destination count independent of padding. */
			wire_base = boundary - 1U + neighbor;
			frame_length = wire_base - 48U;
			expected_wire = wire_base + (neighbor == 1U ? 1U : 0U);
			expected[0] = frame_length;
			expected[1] = expected_wire - 48U;
			expected[2] = 0U;
			assert(rtl8822b_test_firmware_segment(&view, frame_length,
			    check_firmware_boundary, expected) == 0);
			assert(expected[2] == 1U);

			/* Fill a valid wildcard probe with bounded vendor information. */
			memset(frame, 0x5a, sizeof(frame));
			make_probe_request(frame, &radio.board);
			offset = 26U;
			while (offset < frame_length) {
				/* Avoid leaving an incomplete final information element. */
				remaining = frame_length - offset;
				assert(remaining >= 2U);
				ie_length = remaining > 257U ? 255U : remaining - 2U;

				/* Reserve two bytes when the next element would be short. */
				if (remaining - ie_length - 2U == 1U)
					ie_length--;
				frame[offset] = 221U;
				frame[offset + 1U] = (uint8_t)ie_length;
				offset += ie_length + 2U;
			}

			/* Check the same encoder on both validated USB profiles. */
			for (speed = 0U; speed < 2U; speed++) {
				radio.transport.usb_bulk_max_packet_size =
				    speed == 0U ? 512U : 1024U;
				memset(wire, 0xa5, sizeof(wire));
				assert(rtl8822b_radio_management_frame_prepare(&radio,
				    wire, sizeof(wire), frame, frame_length,
				    &wire_length) == 0);
				assert(wire_length == expected_wire);
				assert(wire_length %
				    radio.transport.usb_bulk_max_packet_size != 0U);
				assert(get_le16(wire) == frame_length);
				assert(memcmp(wire + 48U, frame, frame_length) == 0);
				assert(wire[wire_length] == 0xa5U);

				/* Require zero padding only when the unpadded size was full. */
				if (neighbor == 1U)
					assert(wire[wire_length - 1U] == 0U);

				/* Validate descriptor checksum and exact-capacity refusal. */
				checksum = 0U;
				for (index = 0U; index < 16U; index++)
					checksum ^= get_le16(wire + index * 2U);
				assert(checksum == 0U);
				assert(rtl8822b_radio_management_frame_prepare(&radio,
				    wire, expected_wire - 1U, frame, frame_length,
				    &wire_length) == ENOSPC);
				assert(wire_length == 0U);
			}
		}
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
	test_c2h_rx_metadata();
	test_bounded_random_inputs();
	test_radio_table_interpreter();
	test_radio_transport_deadline();
	test_radio_txagc_clamps();
	test_radio_lifecycle();
	test_radio_deadline_and_stop_retry();
	test_radio_already_powered_rebind();
	test_radio_profile_costs();
	test_radio_trx_path_a();
	test_radio_wlan_only();
	test_management_band_rates();
	test_hardware_etsi_plan();
	test_radio_usb_profiles();
	test_usb_tx_boundaries();
	if (argc == 2)
		test_pinned_blob(argv[1]);
	else
		assert(argc == 1);
	puts("rtl8822b core: PASS");
	return 0;
}
