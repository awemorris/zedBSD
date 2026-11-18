/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Intel AX211 private BSS association-metadata contract.
 */

#ifndef KERN_DRIVERS_INTEL_AX211_BSS_H
#define KERN_DRIVERS_INTEL_AX211_BSS_H

#include <stddef.h>
#include <stdint.h>

#include "intel-ax211-rx.h"

/*
 * Keep every BSS which the common WLAN snapshot may offer back to connect().
 * A smaller private cache can evict the selected BSSID while it is still
 * present in the common 64-entry scan snapshot.
 */
#define INTEL_AX211_BSS_CACHE_LIMIT 64U
#define INTEL_AX211_BSS_ADDRESS_SIZE 6U

/* The results of the BSS decoder and cache. */
enum intel_ax211_bss_result {
	INTEL_AX211_BSS_OK = 0,
	INTEL_AX211_BSS_INVALID = 1,
	INTEL_AX211_BSS_UNSUPPORTED = 2,
	INTEL_AX211_BSS_TRUNCATED = 3,
	INTEL_AX211_BSS_MALFORMED = 4,
	INTEL_AX211_BSS_STALE = 5,
	INTEL_AX211_BSS_NOT_FOUND = 6
};

/* The kind of frame a BSS entry came from. */
enum intel_ax211_bss_source {
	INTEL_AX211_BSS_SOURCE_PROBE_RESPONSE = 1,
	INTEL_AX211_BSS_SOURCE_BEACON = 2
};

/*
 * One BSS as the driver last saw it in a beacon or a probe response.
 *
 * This private record intentionally retains neither an SSID nor credentials.
 * observation_generation records the scan operation which observed the BSS;
 * hardware_epoch independently identifies the firmware boot which produced
 * the RX descriptor.  A later connection has its own common generation.
 */
struct intel_ax211_bss_entry {
	uint8_t bssid[INTEL_AX211_BSS_ADDRESS_SIZE];
	uint64_t observation_generation;
	uint64_t last_seen_ticks;
	uint64_t frame_timestamp;
	uint64_t receive_tsf;
	uint32_t hardware_epoch;
	uint32_t air_start_clock;
	int32_t rssi_dbm;
	uint16_t beacon_interval_tu;
	uint16_t capability;
	uint8_t channel;
	uint8_t dtim_count;
	uint8_t dtim_period;
	uint8_t tim_valid;
	uint8_t wmm_present;
	uint8_t receive_tsf_valid;
	uint8_t source;
	uint8_t valid;
};

/*
 * The BSSs seen in the scans of one firmware epoch.
 *
 * It holds as many entries as the common scan snapshot, so a BSS that snapshot
 * offers is still here when a connection is asked for.
 */
struct intel_ax211_bss_cache {
	struct intel_ax211_bss_entry entry[INTEL_AX211_BSS_CACHE_LIMIT];
	uint32_t hardware_epoch;
	size_t count;
	uint8_t initialized;
};

/*
 * What the association needs to know about the chosen BSS.
 *
 * Association-side values only.  Queue DMA addresses and other hardware-owned
 * state are deliberately absent.  tim_valid tells the caller whether the
 * DTIM fields can be used for the post-authentication association update.
 */
struct drv_intel_ax211_bss_assoc_metadata {
	uint8_t bssid[INTEL_AX211_BSS_ADDRESS_SIZE];
	uint64_t common_generation;
	uint64_t observation_generation;
	uint64_t beacon_timestamp;
	uint64_t receive_tsf;
	uint32_t hardware_epoch;
	uint32_t beacon_arrive_time;
	uint16_t beacon_interval_tu;
	uint16_t capability;
	uint8_t channel;
	uint8_t dtim_count;
	uint8_t dtim_period;
	uint8_t tim_valid;
	uint8_t wmm_present;
	uint8_t receive_tsf_valid;
};

int
drv_intel_ax211_bss_decode(
	const struct intel_ax211_rx_delivery *mpdu,
	uint64_t observation_generation,
	uint32_t hardware_epoch,
	struct intel_ax211_bss_entry *entry);

int
drv_intel_ax211_bss_cache_init(
	struct intel_ax211_bss_cache *cache,
	uint32_t hardware_epoch);

int
drv_intel_ax211_bss_cache_observe(
	struct intel_ax211_bss_cache *cache,
	const struct intel_ax211_bss_entry *entry);

int
drv_intel_ax211_bss_cache_lookup(
	const struct intel_ax211_bss_cache *cache,
	const uint8_t bssid[INTEL_AX211_BSS_ADDRESS_SIZE],
	uint8_t channel,
	uint32_t hardware_epoch,
	struct intel_ax211_bss_entry *entry);

int
drv_intel_ax211_bss_assoc_metadata(
	const struct intel_ax211_bss_entry *entry,
	uint64_t connection_generation,
	uint32_t hardware_epoch,
	struct drv_intel_ax211_bss_assoc_metadata *metadata);

#endif
