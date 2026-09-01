/*
 * zedBSD RTL8822B private chip contract
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_DRIVERS_RTL8822B_INTERNAL_H
#define ZEDBSD_DRIVERS_RTL8822B_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#define RTL8822B_FIRMWARE_SIZE             161240U
#define RTL8822B_FIRMWARE_HEADER_SIZE          64U
#define RTL8822B_FIRMWARE_CHECKSUM_SIZE         8U
#define RTL8822B_FIRMWARE_DMEM_SIZE         11208U
#define RTL8822B_FIRMWARE_IMEM_SIZE        149952U
#define RTL8822B_FIRMWARE_DMEM_ADDRESS 0x00200000U
#define RTL8822B_FIRMWARE_IMEM_ADDRESS 0x00000000U
#define RTL8822B_FIRMWARE_CHUNK_MAX          0x1000U
#define RTL8822B_FIRMWARE_PATH \
	"/lib/firmware/rtw88/rtw8822b_fw.bin"

#define RTL8822B_EFUSE_PHYSICAL_SIZE          1024U
#define RTL8822B_EFUSE_LOGICAL_SIZE            768U
#define RTL8822B_EFUSE_PROTECTED_SIZE           96U
#define RTL8822BU_EFUSE_MAC_OFFSET            0x107U

#define RTL8822B_FIRMWARE_TX_DESCRIPTOR_SIZE     48U
#define RTL8822B_RX_DESCRIPTOR_SIZE               24U
#define RTL8822B_RX_AGGREGATE_MAX              32768U
#define RTL8822B_RX_PHY_INFO_SIZE                  32U
#define RTL8822B_RX_FCS_SIZE                        4U
#define RTL8822B_RX_MPDU_MAX                    11454U

enum rtl8822b_firmware_segment {
	RTL8822B_FIRMWARE_SEGMENT_DMEM = 1,
	RTL8822B_FIRMWARE_SEGMENT_IMEM = 2
};

struct rtl8822b_firmware_view {
	const uint8_t *bytes;
	size_t size;
	uint16_t version;
	uint8_t subversion;
	uint8_t subindex;
	size_t dmem_offset;
	size_t dmem_length;
	uint32_t dmem_address;
	size_t imem_offset;
	size_t imem_length;
	uint32_t imem_address;
};

struct rtl8822b_firmware_blob {
	uint8_t *bytes;
	size_t size;
	struct rtl8822b_firmware_view view;
};

struct rtl8822b_firmware_chunk {
	enum rtl8822b_firmware_segment segment;
	size_t file_offset;
	uint32_t destination;
	uint32_t length;
	uint32_t wire_payload_length;
	uint8_t first;
	uint8_t last;
	uint8_t checksum_continue;
	uint8_t reserved;
};

typedef int (*rtl8822b_firmware_chunk_fn)(void *context,
	const struct rtl8822b_firmware_chunk *chunk);

struct rtl8822b_chip_identity {
	uint8_t cut;
	uint8_t rf_path_count;
	uint8_t mass_production;
	uint8_t reserved;
};

struct rtl8822bu_board_info {
	struct rtl8822b_chip_identity chip;
	uint8_t mac_address[6];
	uint8_t rfe_option;
	uint8_t channel_plan;
	uint8_t crystal_cap;
	uint8_t thermal_meter;
	uint8_t rf_board_option;
	uint8_t country_code[2];
};

enum rtl8822b_rx_kind {
	RTL8822B_RX_FRAME = 1,
	RTL8822B_RX_C2H = 2
};

struct rtl8822b_rx_packet {
	enum rtl8822b_rx_kind kind;
	const uint8_t *payload;
	size_t payload_length;
	const uint8_t *phy_info;
	size_t phy_info_length;
	size_t aggregate_length;
	int32_t rssi_dbm;
	uint32_t tsf_low;
	uint8_t rate;
	uint8_t bandwidth;
	uint8_t c2h_id;
	uint8_t c2h_sequence;
};

typedef int (*rtl8822b_rx_packet_fn)(void *context,
	const struct rtl8822b_rx_packet *packet);

int rtl8822b_sha256(const void *data, size_t length, uint8_t digest[32]);
int rtl8822b_firmware_validate(const uint8_t *data, size_t length,
	struct rtl8822b_firmware_view *view);
/* firmware must be zero initialized or owned by a previous successful load. */
int rtl8822b_firmware_load(struct rtl8822b_firmware_blob *firmware);
void rtl8822b_firmware_release(struct rtl8822b_firmware_blob *firmware);
int rtl8822b_firmware_walk(const struct rtl8822b_firmware_view *view,
	rtl8822b_firmware_chunk_fn callback, void *context);
int rtl8822b_firmware_tx_descriptor(uint8_t descriptor[48],
	size_t payload_length);

int rtl8822b_efuse_decode(const uint8_t *physical, size_t physical_length,
	uint8_t *logical, size_t logical_length);
int rtl8822b_chip_identity_parse(uint32_t sys_cfg1,
	struct rtl8822b_chip_identity *identity);
int rtl8822bu_board_parse(const uint8_t *logical, size_t logical_length,
	uint32_t sys_cfg1, struct rtl8822bu_board_info *board);

int rtl8822b_rx_packet_parse(const uint8_t *bytes, size_t length,
	struct rtl8822b_rx_packet *packet);
int rtl8822b_rx_aggregate_walk(const uint8_t *bytes, size_t length,
	rtl8822b_rx_packet_fn callback, void *context, size_t *packet_count);

#ifdef RTL8822B_TESTING
int rtl8822b_test_firmware_validate(const uint8_t *data, size_t length,
	const uint8_t expected_digest[32],
	struct rtl8822b_firmware_view *view);
int rtl8822b_test_firmware_walk(const struct rtl8822b_firmware_view *view,
	const uint8_t expected_digest[32],
	rtl8822b_firmware_chunk_fn callback, void *context);
int rtl8822b_test_firmware_blob_state(
	const struct rtl8822b_firmware_blob *firmware, int *owned);
#endif

#endif
