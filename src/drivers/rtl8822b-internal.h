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
#define RTL8822B_MANAGEMENT_TX_DESCRIPTOR_SIZE     48U
#define RTL8822B_MANAGEMENT_MPDU_MAX              2304U

#define RTL8822B_TABLE_DOMAIN_MAC                    1U
#define RTL8822B_TABLE_DOMAIN_AGC                    2U
#define RTL8822B_TABLE_DOMAIN_BB                     3U
#define RTL8822B_TABLE_DOMAIN_RF                     4U
#define RTL8822B_TABLE_WIDTH_8                       1U
#define RTL8822B_TABLE_WIDTH_32                      4U
#define RTL8822B_TABLE_WIDTH_RF20                    5U

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

/*
 * Register callbacks return a positive errno value.  delay_us() must check
 * deadline_ticks before and while waiting; it must never sleep beyond that
 * absolute monotonic deadline.  The radio object copies this structure, so a
 * caller may use a temporary transport value.  A radio object must be zero
 * initialized before its first power_on call.
 */
struct rtl8822b_radio_transport {
	void *context;
	int (*read)(void *context, uint16_t address, unsigned width,
	    uint32_t *value);
	int (*write)(void *context, uint16_t address, unsigned width,
	    uint32_t value);
	uint64_t (*now_ticks)(void *context);
	int (*delay_us)(void *context, uint32_t microseconds,
	    uint64_t deadline_ticks);
	void (*yield)(void *context);
};

enum rtl8822b_radio_state {
	RTL8822B_RADIO_OFF = 0,
	RTL8822B_RADIO_POWERED = 1,
	RTL8822B_RADIO_STARTED = 2
};

struct rtl8822b_radio {
	struct rtl8822b_radio_transport transport;
	struct rtl8822bu_board_info board;
	uint8_t state;
	uint8_t channel;
	uint8_t power_limits_valid;
	uint8_t reserved;
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

/*
 * The staged order is power_on, caller-owned firmware download, start, RX
 * arm, and finally WLAN publication.  start never claims success for tables
 * alone: it completes the three-bulk-OUT/HS USB queues, minimum MAC timing,
 * MAC/BB/AGC/RF profile, channel 1, and an all-rate/path TXAGC index-0 floor.
 * Consequently the only transmitted frame accepted here is a 1 Mbps
 * wildcard probe request on channels 1-11; general, HT, VHT, and data TX are
 * outside this bounded first profile.  A channel transport fault fails the
 * radio closed.  power_on normalizes a still-powered rebind through the
 * checked disable sequence before enabling it again.  stop always clears the
 * software object, including after a disconnected transport error, so the
 * caller may zero/rebind it safely.
 */
int rtl8822b_radio_power_on(struct rtl8822b_radio *radio,
	const struct rtl8822b_radio_transport *transport,
	const struct rtl8822bu_board_info *board, uint64_t deadline_ticks);
int rtl8822b_radio_start(struct rtl8822b_radio *radio,
	uint64_t deadline_ticks);
int rtl8822b_radio_set_channel(struct rtl8822b_radio *radio,
	uint8_t channel, uint64_t deadline_ticks);
int rtl8822b_radio_stop(struct rtl8822b_radio *radio,
	uint64_t deadline_ticks);
int rtl8822b_radio_active_scan_allowed(const struct rtl8822b_radio *radio,
	uint8_t channel);
int rtl8822b_radio_management_frame_prepare(
	const struct rtl8822b_radio *radio, uint8_t *wire, size_t capacity,
	const uint8_t *frame, size_t frame_length, size_t *wire_length);

#ifdef RTL8822B_TESTING
int rtl8822b_test_firmware_validate(const uint8_t *data, size_t length,
	const uint8_t expected_digest[32],
	struct rtl8822b_firmware_view *view);
int rtl8822b_test_firmware_walk(const struct rtl8822b_firmware_view *view,
	const uint8_t expected_digest[32],
	rtl8822b_firmware_chunk_fn callback, void *context);
int rtl8822b_test_firmware_blob_state(
	const struct rtl8822b_firmware_blob *firmware, int *owned);
int rtl8822b_test_radio_table_apply(struct rtl8822b_radio *radio,
	uint8_t domain, uint8_t width, uint8_t rf_path,
	const uint32_t *words, size_t word_count, uint64_t deadline_ticks);
#endif

#endif
