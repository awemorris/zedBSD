/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Intel AX211 private API89 receive contract.
 */

#ifndef KERN_DRIVERS_INTEL_AX211_RX_H
#define KERN_DRIVERS_INTEL_AX211_RX_H

#include <stddef.h>
#include <stdint.h>

#include "intel-ax211-protocol.h"

/*
 * The received-frame notification.
 *
 * Every received 802.11 frame arrives as this notification: a 64-byte
 * descriptor, then the frame, at most 4032 bytes, as the descriptor's first
 * 16-bit word says.  The receive buffer may hold bytes after the frame; they
 * are not part of it.  Version 5 is the one the pinned API 89 image lists.
 *
 * Values referenced: Linux v6.17
 * drivers/net/wireless/intel/iwlwifi/fw/api/commands.h (opcode 0xc1 of family
 * 0) and drivers/net/wireless/intel/iwlwifi/fw/api/rx.h (struct
 * iwl_rx_mpdu_desc).
 */
#define INTEL_AX211_RX_FRAME_FAMILY 0U
#define INTEL_AX211_RX_FRAME_OPCODE 0xc1U
#define INTEL_AX211_RX_FRAME_NOTIFICATION_VERSION 5U
#define INTEL_AX211_RX_FRAME_DESCRIPTOR_SIZE 64U
#define INTEL_AX211_RX_FRAME_BODY_MAX 4032U

/* How a delivered frame was protected: zedBSD's own codes. */
#define INTEL_AX211_RX_PROTECTION_NONE 0U
#define INTEL_AX211_RX_PROTECTION_CCMP 1U

/* The results of the receive decoder. */
enum intel_ax211_rx_result {
	INTEL_AX211_RX_OK = 0,
	INTEL_AX211_RX_INVALID = 1,
	INTEL_AX211_RX_UNSUPPORTED = 2,
	INTEL_AX211_RX_TRUNCATED = 3,
	INTEL_AX211_RX_OVERSIZED = 4,
	INTEL_AX211_RX_FAILED = 5,
	INTEL_AX211_RX_DUPLICATE = 6,
	INTEL_AX211_RX_BUFFER_TOO_SMALL = 7,
	INTEL_AX211_RX_STALE = 8
};

/*
 * One decoded received frame with its radio information and protection.
 *
 * The frame pointer refers to the caller-owned output buffer.
 */
struct intel_ax211_rx_delivery {
	const uint8_t *frame;
	size_t length;
	uint64_t packet_number;
	uint64_t tsf;
	uint32_t air_start_clock;
	uint32_t status;
	int32_t rssi_dbm;
	uint8_t channel;
	uint8_t cipher;
	uint8_t decrypted;
	uint8_t key_index;
	uint8_t tsf_valid;
};

int
drv_intel_ax211_rx_api89_validate(
	const struct intel_ax211_protocol_command_table *table);

int
drv_intel_ax211_rx_mpdu_decode(
	const struct intel_ax211_protocol_message *message,
	uint32_t generation,
	uint8_t *output,
	size_t output_capacity,
	struct intel_ax211_rx_delivery *mpdu);

#endif
