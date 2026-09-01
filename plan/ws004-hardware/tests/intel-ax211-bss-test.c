/* Intel AX211 private BSS association-metadata fixture.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../../src/drivers/intel-ax211-bss.h"

#define FRAME_CAPACITY 256U
#define SCAN_GENERATION UINT64_C(41)
#define CONNECTION_GENERATION UINT64_C(42)
#define HIGH_CONNECTION_GENERATION \
	(UINT64_C(42) + (UINT64_C(1) << 32))
#define HARDWARE_EPOCH UINT32_C(0x10203040)

struct frame_builder {
	uint8_t bytes[FRAME_CAPACITY];
	size_t length;
};

static const uint8_t test_bssid[6U] = {
	0x02U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U
};

static void
put_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0U] = (uint8_t)value;
	bytes[1U] = (uint8_t)(value >> 8);
}

static void
put_le64(uint8_t *bytes, uint64_t value)
{
	bytes[0U] = (uint8_t)value;
	bytes[1U] = (uint8_t)(value >> 8);
	bytes[2U] = (uint8_t)(value >> 16);
	bytes[3U] = (uint8_t)(value >> 24);
	bytes[4U] = (uint8_t)(value >> 32);
	bytes[5U] = (uint8_t)(value >> 40);
	bytes[6U] = (uint8_t)(value >> 48);
	bytes[7U] = (uint8_t)(value >> 56);
}

static void
frame_begin(struct frame_builder *builder, uint16_t subtype)
{
	assert(builder != NULL);
	memset(builder, 0, sizeof(*builder));
	put_le16(builder->bytes, subtype);
	memset(builder->bytes + 4U, 0xff, 6U);
	memcpy(builder->bytes + 10U, test_bssid, sizeof(test_bssid));
	memcpy(builder->bytes + 16U, test_bssid, sizeof(test_bssid));
	put_le64(builder->bytes + 24U, UINT64_C(0x8877665544332211));
	put_le16(builder->bytes + 32U, 100U);
	put_le16(builder->bytes + 34U, 0x0431U);
	builder->length = 36U;
}

static void
frame_ie(struct frame_builder *builder, uint8_t identifier,
	const uint8_t *data, size_t length)
{
	assert(builder != NULL);
	assert(length <= UINT8_MAX);
	assert(length <= FRAME_CAPACITY - builder->length - 2U);
	builder->bytes[builder->length] = identifier;
	builder->bytes[builder->length + 1U] = (uint8_t)length;
	if (length != 0U) {
		assert(data != NULL);
		memcpy(builder->bytes + builder->length + 2U, data, length);
	}
	builder->length += 2U + length;
}

static struct intel_ax211_rx_mpdu
make_mpdu(const struct frame_builder *builder, uint8_t channel)
{
	struct intel_ax211_rx_mpdu mpdu;

	memset(&mpdu, 0, sizeof(mpdu));
	mpdu.frame = builder->bytes;
	mpdu.length = builder->length;
	mpdu.tsf = UINT64_C(0x0123456789abcdef);
	mpdu.gp2_on_air_rise = UINT32_C(0x89abcdef);
	mpdu.rssi_dbm = -37;
	mpdu.channel = channel;
	mpdu.tsf_valid = 1U;
	return mpdu;
}

static void
add_ssid(struct frame_builder *builder)
{
	static const uint8_t ssid[] = { 'n', 'o', 't', '-', 'r', 'e', 't', 'a',
		'i', 'n', 'e', 'd' };

	frame_ie(builder, 0U, ssid, sizeof(ssid));
}

static void
add_ds(struct frame_builder *builder, uint8_t channel)
{
	frame_ie(builder, 3U, &channel, 1U);
}

static void
add_ht(struct frame_builder *builder, uint8_t channel)
{
	uint8_t ht[22U];

	memset(ht, 0, sizeof(ht));
	ht[0U] = channel;
	frame_ie(builder, 61U, ht, sizeof(ht));
}

static void
add_tim(struct frame_builder *builder, uint8_t count, uint8_t period)
{
	uint8_t tim[4U] = { count, period, 0U, 0U };

	frame_ie(builder, 5U, tim, sizeof(tim));
}

static void
add_wmm(struct frame_builder *builder, uint8_t subtype)
{
	uint8_t wmm[24U];

	memset(wmm, 0, sizeof(wmm));
	wmm[0U] = 0x00U;
	wmm[1U] = 0x50U;
	wmm[2U] = 0xf2U;
	wmm[3U] = 2U;
	wmm[4U] = subtype;
	wmm[5U] = 1U;
	wmm[6U] = 0x80U;
	frame_ie(builder, 221U, wmm,
	    subtype == 0U ? 7U : sizeof(wmm));
}

static struct intel_ax211_bss_entry
decode_beacon(void)
{
	struct frame_builder frame;
	struct intel_ax211_rx_mpdu mpdu;
	struct intel_ax211_bss_entry entry;

	frame_begin(&frame, 0x0080U);
	add_ssid(&frame);
	add_ds(&frame, 6U);
	add_ht(&frame, 6U);
	add_tim(&frame, 1U, 3U);
	add_wmm(&frame, 1U);
	mpdu = make_mpdu(&frame, 6U);
	assert(intel_ax211_bss_decode(&mpdu, SCAN_GENERATION,
	    HARDWARE_EPOCH, &entry) == INTEL_AX211_BSS_OK);
	return entry;
}

static struct intel_ax211_bss_entry
decode_probe(void)
{
	struct frame_builder frame;
	struct intel_ax211_rx_mpdu mpdu;
	struct intel_ax211_bss_entry entry;

	frame_begin(&frame, 0x0050U);
	add_ssid(&frame);
	add_ds(&frame, 6U);
	add_wmm(&frame, 0U);
	mpdu = make_mpdu(&frame, 6U);
	assert(intel_ax211_bss_decode(&mpdu, SCAN_GENERATION,
	    HARDWARE_EPOCH, &entry) == INTEL_AX211_BSS_OK);
	return entry;
}

static void
test_beacon_decode(void)
{
	struct intel_ax211_bss_entry entry;

	entry = decode_beacon();
	assert(memcmp(entry.bssid, test_bssid, sizeof(test_bssid)) == 0);
	assert(entry.observation_generation == SCAN_GENERATION);
	assert(entry.hardware_epoch == HARDWARE_EPOCH);
	assert(entry.frame_timestamp == UINT64_C(0x8877665544332211));
	assert(entry.receive_tsf == UINT64_C(0x0123456789abcdef));
	assert(entry.gp2_on_air_rise == UINT32_C(0x89abcdef));
	assert(entry.rssi_dbm == -37);
	assert(entry.beacon_interval_tu == 100U);
	assert(entry.capability == 0x0431U);
	assert(entry.channel == 6U);
	assert(entry.dtim_count == 1U && entry.dtim_period == 3U);
	assert(entry.tim_valid == 1U && entry.wmm_present == 1U);
	assert(entry.receive_tsf_valid == 1U);
	assert(entry.source == INTEL_AX211_BSS_SOURCE_BEACON);
	assert(entry.valid == 1U);
}

static void
test_probe_decode(void)
{
	struct intel_ax211_bss_entry entry;

	entry = decode_probe();
	assert(entry.source == INTEL_AX211_BSS_SOURCE_PROBE_RESPONSE);
	assert(entry.tim_valid == 0U);
	assert(entry.dtim_count == 0U && entry.dtim_period == 0U);
	assert(entry.wmm_present == 1U);
}

static void
test_cache_and_metadata(void)
{
	struct intel_ax211_bss_assoc_metadata metadata;
	struct intel_ax211_bss_cache cache;
	struct intel_ax211_bss_entry probe;
	struct intel_ax211_bss_entry beacon;
	struct intel_ax211_bss_entry found;

	probe = decode_probe();
	beacon = decode_beacon();
	assert(intel_ax211_bss_cache_init(&cache, HARDWARE_EPOCH) ==
	    INTEL_AX211_BSS_OK);
	assert(intel_ax211_bss_cache_observe(&cache, &probe) ==
	    INTEL_AX211_BSS_OK);
	assert(cache.count == 1U);
	assert(intel_ax211_bss_cache_lookup(&cache, test_bssid, 6U,
	    HARDWARE_EPOCH, &found) == INTEL_AX211_BSS_OK);
	assert(found.source == INTEL_AX211_BSS_SOURCE_PROBE_RESPONSE);

	/* A beacon upgrades the probe entry and contributes DTIM state. */
	assert(intel_ax211_bss_cache_observe(&cache, &beacon) ==
	    INTEL_AX211_BSS_OK);
	assert(cache.count == 1U);
	assert(intel_ax211_bss_cache_lookup(&cache, test_bssid, 6U,
	    HARDWARE_EPOCH, &found) == INTEL_AX211_BSS_OK);
	assert(found.source == INTEL_AX211_BSS_SOURCE_BEACON);
	assert(found.tim_valid == 1U);

	/* A later probe must not erase authoritative beacon-only DTIM data. */
	probe.capability = 0U;
	assert(intel_ax211_bss_cache_observe(&cache, &probe) ==
	    INTEL_AX211_BSS_OK);
	assert(intel_ax211_bss_cache_lookup(&cache, test_bssid, 6U,
	    HARDWARE_EPOCH, &found) == INTEL_AX211_BSS_OK);
	assert(found.capability == 0x0431U && found.tim_valid == 1U);

	assert(intel_ax211_bss_assoc_metadata(&found, CONNECTION_GENERATION,
	    HARDWARE_EPOCH, &metadata) == INTEL_AX211_BSS_OK);
	assert(memcmp(metadata.bssid, test_bssid, sizeof(test_bssid)) == 0);
	assert(metadata.common_generation == CONNECTION_GENERATION);
	assert(metadata.observation_generation == SCAN_GENERATION);
	assert(metadata.hardware_epoch == HARDWARE_EPOCH);
	assert(metadata.beacon_tsf == UINT64_C(0x8877665544332211));
	assert(metadata.beacon_arrive_time == UINT32_C(0x89abcdef));
	assert(metadata.receive_tsf == UINT64_C(0x0123456789abcdef));
	assert(metadata.beacon_interval_tu == 100U);
	assert(metadata.channel == 6U && metadata.capability == 0x0431U);
	assert(metadata.tim_valid == 1U && metadata.dtim_count == 1U &&
	    metadata.dtim_period == 3U && metadata.wmm_present == 1U);

	/* Scan generation 41 may be selected by connection generation 42. */
	assert(intel_ax211_bss_assoc_metadata(&beacon,
	    HIGH_CONNECTION_GENERATION, HARDWARE_EPOCH, &metadata) ==
	    INTEL_AX211_BSS_OK);
	assert(metadata.common_generation == HIGH_CONNECTION_GENERATION);
	assert(metadata.common_generation > UINT32_MAX);
	assert(metadata.observation_generation == SCAN_GENERATION);

	/* A probe from a newer scan replaces an old beacon rather than carrying
	 * stale TIM/GP2 state into that scan's connection. */
	probe.observation_generation = SCAN_GENERATION + 1U;
	probe.gp2_on_air_rise++;
	assert(intel_ax211_bss_cache_observe(&cache, &probe) ==
	    INTEL_AX211_BSS_OK);
	assert(intel_ax211_bss_cache_lookup(&cache, test_bssid, 6U,
	    HARDWARE_EPOCH, &found) == INTEL_AX211_BSS_OK);
	assert(found.source == INTEL_AX211_BSS_SOURCE_PROBE_RESPONSE);
	assert(found.observation_generation == SCAN_GENERATION + 1U);
	assert(found.tim_valid == 0U);
	assert(intel_ax211_bss_cache_observe(&cache, &beacon) ==
	    INTEL_AX211_BSS_STALE);
	assert(intel_ax211_bss_cache_lookup(&cache, test_bssid, 6U,
	    HARDWARE_EPOCH + 1U, &found) ==
	    INTEL_AX211_BSS_STALE);
	beacon.hardware_epoch++;
	assert(intel_ax211_bss_cache_observe(&cache, &beacon) ==
	    INTEL_AX211_BSS_STALE);
}

static void
test_cache_capacity(void)
{
	struct intel_ax211_bss_cache cache;
	struct intel_ax211_bss_entry entry;
	struct intel_ax211_bss_entry found;
	uint8_t first_bssid[6U];
	uint8_t replacement_bssid[6U];
	uint8_t rejected_bssid[6U];
	size_t index;

	entry = decode_beacon();
	/* Must retain every BSS exposed by the common 64-entry snapshot. */
	assert(INTEL_AX211_BSS_CACHE_LIMIT == 64U);
	assert(intel_ax211_bss_cache_init(&cache, HARDWARE_EPOCH) ==
	    INTEL_AX211_BSS_OK);
	for (index = 0U; index < INTEL_AX211_BSS_CACHE_LIMIT; index++) {
		entry.bssid[5U] = (uint8_t)(index + 1U);
		entry.channel = (uint8_t)(index % 11U + 1U);
		entry.rssi_dbm = -30;
		entry.last_seen_ticks = index + 1U;
		assert(intel_ax211_bss_cache_observe(&cache, &entry) ==
		    INTEL_AX211_BSS_OK);
	}
	assert(cache.count == INTEL_AX211_BSS_CACHE_LIMIT);
	memcpy(first_bssid, test_bssid, sizeof(first_bssid));
	first_bssid[5U] = 1U;
	assert(intel_ax211_bss_cache_lookup(&cache, first_bssid, 1U,
	    HARDWARE_EPOCH, &found) == INTEL_AX211_BSS_OK);

	memcpy(replacement_bssid, test_bssid, sizeof(replacement_bssid));
	replacement_bssid[4U] = 0xaaU;
	replacement_bssid[5U] = 0xbbU;
	memcpy(entry.bssid, replacement_bssid, sizeof(entry.bssid));
	entry.channel = 11U;
	entry.rssi_dbm = -10;
	entry.last_seen_ticks = 100U;
	assert(intel_ax211_bss_cache_observe(&cache, &entry) ==
	    INTEL_AX211_BSS_OK);
	assert(cache.count == INTEL_AX211_BSS_CACHE_LIMIT);
	assert(intel_ax211_bss_cache_lookup(&cache, first_bssid, 1U,
	    HARDWARE_EPOCH, &found) ==
	    INTEL_AX211_BSS_NOT_FOUND);
	assert(intel_ax211_bss_cache_lookup(&cache, replacement_bssid, 11U,
	    HARDWARE_EPOCH, &found) == INTEL_AX211_BSS_OK);

	memcpy(rejected_bssid, test_bssid, sizeof(rejected_bssid));
	rejected_bssid[4U] = 0xccU;
	rejected_bssid[5U] = 0xddU;
	memcpy(entry.bssid, rejected_bssid, sizeof(entry.bssid));
	entry.rssi_dbm = -100;
	entry.last_seen_ticks = 101U;
	assert(intel_ax211_bss_cache_observe(&cache, &entry) ==
	    INTEL_AX211_BSS_OK);
	assert(intel_ax211_bss_cache_lookup(&cache, rejected_bssid, 11U,
	    HARDWARE_EPOCH, &found) == INTEL_AX211_BSS_NOT_FOUND);
}

static int
decode_custom(struct frame_builder *frame, uint8_t channel)
{
	struct intel_ax211_rx_mpdu mpdu;
	struct intel_ax211_bss_entry entry;

	mpdu = make_mpdu(frame, channel);
	return intel_ax211_bss_decode(&mpdu, SCAN_GENERATION,
	    HARDWARE_EPOCH, &entry);
}

static void
test_frame_rejections(void)
{
	struct frame_builder frame;
	struct intel_ax211_rx_mpdu mpdu;
	struct intel_ax211_bss_entry entry;
	uint8_t bytes[40U];

	frame_begin(&frame, 0x0080U);
	mpdu = make_mpdu(&frame, 6U);
	mpdu.length = 35U;
	assert(intel_ax211_bss_decode(&mpdu, SCAN_GENERATION,
	    HARDWARE_EPOCH, &entry) == INTEL_AX211_BSS_TRUNCATED);
	frame_begin(&frame, 0x0008U);
	assert(decode_custom(&frame, 6U) == INTEL_AX211_BSS_UNSUPPORTED);
	frame_begin(&frame, 0x0000U);
	assert(decode_custom(&frame, 6U) == INTEL_AX211_BSS_UNSUPPORTED);
	frame_begin(&frame, 0x0180U);
	assert(decode_custom(&frame, 6U) == INTEL_AX211_BSS_MALFORMED);
	frame_begin(&frame, 0x4080U);
	assert(decode_custom(&frame, 6U) == INTEL_AX211_BSS_MALFORMED);
	frame_begin(&frame, 0x0080U);
	frame.bytes[16U] |= 1U;
	assert(decode_custom(&frame, 6U) == INTEL_AX211_BSS_MALFORMED);
	frame_begin(&frame, 0x0080U);
	put_le16(frame.bytes + 32U, 0U);
	assert(decode_custom(&frame, 6U) == INTEL_AX211_BSS_MALFORMED);
	frame_begin(&frame, 0x0080U);
	add_ds(&frame, 11U);
	assert(decode_custom(&frame, 6U) == INTEL_AX211_BSS_MALFORMED);
	frame_begin(&frame, 0x0080U);
	frame.bytes[frame.length++] = 3U;
	assert(decode_custom(&frame, 6U) == INTEL_AX211_BSS_TRUNCATED);
	frame_begin(&frame, 0x0080U);
	frame.bytes[frame.length++] = 3U;
	frame.bytes[frame.length++] = 2U;
	frame.bytes[frame.length++] = 6U;
	assert(decode_custom(&frame, 6U) == INTEL_AX211_BSS_TRUNCATED);

	memset(bytes, 0, sizeof(bytes));
	frame_begin(&frame, 0x0080U);
	frame_ie(&frame, 3U, bytes, 2U);
	assert(decode_custom(&frame, 6U) == INTEL_AX211_BSS_MALFORMED);
	frame_begin(&frame, 0x0080U);
	add_ds(&frame, 6U);
	add_ds(&frame, 6U);
	assert(decode_custom(&frame, 6U) == INTEL_AX211_BSS_MALFORMED);
	frame_begin(&frame, 0x0080U);
	frame_ie(&frame, 61U, bytes, 21U);
	assert(decode_custom(&frame, 6U) == INTEL_AX211_BSS_MALFORMED);
	frame_begin(&frame, 0x0080U);
	add_ds(&frame, 6U);
	add_ht(&frame, 11U);
	assert(decode_custom(&frame, 6U) == INTEL_AX211_BSS_MALFORMED);

	frame_begin(&frame, 0x0080U);
	frame_ie(&frame, 5U, bytes, 3U);
	assert(decode_custom(&frame, 6U) == INTEL_AX211_BSS_MALFORMED);
	frame_begin(&frame, 0x0080U);
	add_tim(&frame, 0U, 0U);
	assert(decode_custom(&frame, 6U) == INTEL_AX211_BSS_MALFORMED);
	frame_begin(&frame, 0x0080U);
	add_tim(&frame, 3U, 3U);
	assert(decode_custom(&frame, 6U) == INTEL_AX211_BSS_MALFORMED);
	frame_begin(&frame, 0x0080U);
	add_tim(&frame, 0U, 1U);
	add_tim(&frame, 0U, 1U);
	assert(decode_custom(&frame, 6U) == INTEL_AX211_BSS_MALFORMED);

	frame_begin(&frame, 0x0080U);
	memset(bytes, 0, sizeof(bytes));
	bytes[0U] = 0x00U;
	bytes[1U] = 0x50U;
	bytes[2U] = 0xf2U;
	bytes[3U] = 2U;
	frame_ie(&frame, 221U, bytes, 6U);
	assert(decode_custom(&frame, 6U) == INTEL_AX211_BSS_MALFORMED);
	frame_begin(&frame, 0x0080U);
	bytes[4U] = 9U;
	bytes[5U] = 1U;
	bytes[6U] = 0U;
	frame_ie(&frame, 221U, bytes, 7U);
	assert(decode_custom(&frame, 6U) == INTEL_AX211_BSS_MALFORMED);
	frame_begin(&frame, 0x0080U);
	bytes[4U] = 1U;
	bytes[5U] = 1U;
	frame_ie(&frame, 221U, bytes, 7U);
	assert(decode_custom(&frame, 6U) == INTEL_AX211_BSS_MALFORMED);
	frame_begin(&frame, 0x0080U);
	add_wmm(&frame, 0U);
	add_wmm(&frame, 1U);
	assert(decode_custom(&frame, 6U) == INTEL_AX211_BSS_MALFORMED);

	frame_begin(&frame, 0x0080U);
	memset(bytes, 'x', 33U);
	frame_ie(&frame, 0U, bytes, 33U);
	assert(decode_custom(&frame, 6U) == INTEL_AX211_BSS_MALFORMED);
	frame_begin(&frame, 0x0080U);
	add_ssid(&frame);
	add_ssid(&frame);
	assert(decode_custom(&frame, 6U) == INTEL_AX211_BSS_MALFORMED);
}

static void
test_api_rejections(void)
{
	struct intel_ax211_bss_assoc_metadata metadata;
	struct intel_ax211_bss_cache cache;
	struct intel_ax211_bss_entry entry;

	entry = decode_beacon();
	assert(intel_ax211_bss_cache_init(NULL, HARDWARE_EPOCH) ==
	    INTEL_AX211_BSS_INVALID);
	assert(intel_ax211_bss_cache_init(&cache, 0U) ==
	    INTEL_AX211_BSS_INVALID);
	memset(&cache, 0, sizeof(cache));
	assert(intel_ax211_bss_cache_observe(&cache, &entry) ==
	    INTEL_AX211_BSS_INVALID);
	assert(intel_ax211_bss_cache_init(&cache, HARDWARE_EPOCH) ==
	    INTEL_AX211_BSS_OK);
	entry.tim_valid = 1U;
	entry.dtim_period = 0U;
	assert(intel_ax211_bss_cache_observe(&cache, &entry) ==
	    INTEL_AX211_BSS_MALFORMED);
	assert(intel_ax211_bss_assoc_metadata(&entry, CONNECTION_GENERATION,
	    HARDWARE_EPOCH, &metadata) == INTEL_AX211_BSS_MALFORMED);
}

int
main(void)
{
	test_beacon_decode();
	test_probe_decode();
	test_cache_and_metadata();
	test_cache_capacity();
	test_frame_rejections();
	test_api_rejections();
	puts("intel ax211 BSS metadata fixture: PASS");
	return 0;
}
