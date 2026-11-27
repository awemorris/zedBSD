/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host unit tests for the UAC 1.0 / UAC 2.0 descriptor parser.
 *
 * Build and run:
 *   cc -Wall -Wextra -Werror -std=c89 -Iinclude \
 *     src/drivers/usb/usb-audio-desc.c tests/usb-audio-desc-host-test.c \
 *     -o build/usb-audio-desc-host-test && ./build/usb-audio-desc-host-test
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <drivers/usb/usb-audio-desc.h>

static void test_uac1_playback_and_capture(void);
static void test_uac1_continuous_rates_and_24bit(void);
static void test_uac2_topology_and_clock_ranges(void);
static void test_malformed_descriptors(void);

int
main(
	void)
{
	test_uac1_playback_and_capture();
	test_uac1_continuous_rates_and_24bit();
	test_uac2_topology_and_clock_ranges();
	test_malformed_descriptors();
	printf("usb-audio-desc-host-test: all tests passed\n");
	return 0;
}

/* Tests UAC 1.0 AudioControl topology and discrete AudioStreaming formats. */
static void
test_uac1_playback_and_capture(
	void)
{
	/* AC: Header (2 AS), Playback IT(1)->FU(2)->OT(3), Capture IT(4)->FU(5)->OT(6). */
	static const uint8_t ac_block[] = {
		/* AC_HEADER: 10 bytes, bcdADC=0x0100, wTotalLength=62, bInCollection=2, baInterfaceNr={1,2} */
		0x0a, 0x24, 0x01, 0x00, 0x01, 0x3e, 0x00, 0x02, 0x01, 0x02,
		/* INPUT_TERMINAL (ID=1, USB Streaming 0x0101, 2ch) */
		0x0c, 0x24, 0x02, 0x01, 0x01, 0x01, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00,
		/* FEATURE_UNIT (ID=2, src=1, bControlSize=1, master=Mute|Vol, ch1=0, ch2=0) */
		0x09, 0x24, 0x06, 0x02, 0x01, 0x01, 0x03, 0x00, 0x00,
		/* OUTPUT_TERMINAL (ID=3, Speaker 0x0301, src=2) */
		0x09, 0x24, 0x03, 0x03, 0x01, 0x03, 0x00, 0x02, 0x00,
		/* INPUT_TERMINAL (ID=4, Mic 0x0201, 1ch) */
		0x0c, 0x24, 0x02, 0x04, 0x01, 0x02, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00,
		/* FEATURE_UNIT (ID=5, src=4, bControlSize=1, master=Mute, ch1=Vol) */
		0x08, 0x24, 0x06, 0x05, 0x04, 0x01, 0x01, 0x02,
		/* OUTPUT_TERMINAL (ID=6, USB Streaming 0x0101, src=5) */
		0x09, 0x24, 0x03, 0x06, 0x01, 0x01, 0x00, 0x05, 0x00
	};
	static const uint8_t as1_gen[] = {
		0x07, 0x24, 0x01, 0x01, 0x01, 0x01, 0x00
	};
	static const uint8_t as1_fmt[] = {
		/* FORMAT_TYPE_I: 2ch, 2-byte subslot, 16-bit, 2 rates: 44100, 48000 */
		0x0e, 0x24, 0x02, 0x01, 0x02, 0x02, 0x10, 0x02,
		0x44, 0xac, 0x00, 0x80, 0xbb, 0x00
	};
	static const uint8_t as1_ep_cs[] = {
		0x07, 0x25, 0x01, 0x01, 0x00, 0x00, 0x00
	};
	static const uint8_t as2_gen[] = {
		0x07, 0x24, 0x01, 0x06, 0x01, 0x01, 0x00
	};
	static const uint8_t as2_fmt[] = {
		/* FORMAT_TYPE_I: 1ch, 2-byte subslot, 16-bit, 1 rate: 48000 */
		0x0b, 0x24, 0x02, 0x01, 0x01, 0x02, 0x10, 0x01,
		0x80, 0xbb, 0x00
	};
	const void *play_descs[3];
	size_t play_lens[3];
	const void *cap_descs[2];
	size_t cap_lens[2];
	struct uac_control_desc ac;
	struct uac_as_alt play_alt;
	struct uac_as_alt cap_alt;

	assert(uac_desc_parse_ac_block(UAC_VERSION_1, ac_block, sizeof(ac_block), &ac) == 0);
	assert(ac.version == UAC_VERSION_1);
	assert(ac.bcd_adc == 0x0100U);
	assert(ac.stream_interface_count == 2U);
	assert(ac.stream_interfaces[0] == 1U);
	assert(ac.stream_interfaces[1] == 2U);
	assert(ac.unit_count == 6U);

	play_descs[0] = as1_gen;
	play_descs[1] = as1_fmt;
	play_descs[2] = as1_ep_cs;
	play_lens[0] = sizeof(as1_gen);
	play_lens[1] = sizeof(as1_fmt);
	play_lens[2] = sizeof(as1_ep_cs);
	assert(uac_desc_parse_as_alt(UAC_VERSION_1, 1U, 1U, play_descs, play_lens, 3U, &play_alt) == 0);
	uac_desc_add_ep(&play_alt, 0x01U, 0x05U, 200U, 1U, 0x81U);
	uac_desc_add_ep(&play_alt, 0x81U, 0x11U, 4U, 4U, 0x00U);
	assert(uac_desc_resolve_path(&ac, &play_alt) == 0);

	assert(play_alt.capture == 0U);
	assert(play_alt.is_pcm == 1U);
	assert(play_alt.channels == 2U);
	assert(play_alt.subslot_size == 2U);
	assert(play_alt.bit_resolution == 16U);
	assert(play_alt.rate_count == 2U);
	assert(play_alt.rates[0] == 44100U);
	assert(play_alt.rates[1] == 48000U);
	assert(uac_desc_alt_supports_rate(&play_alt, 44100U) == 1);
	assert(uac_desc_alt_supports_rate(&play_alt, 96000U) == 0);
	assert(play_alt.ep_address == 0x01U);
	assert(play_alt.ep_freq_control == 1U);
	assert(play_alt.sync_ep_address == 0x81U);
	assert(play_alt.fu.unit_id == 2U);
	assert(play_alt.fu.has_mute == 1U);
	assert(play_alt.fu.has_volume == 1U);
	assert(play_alt.fu.master_volume == 1U);

	cap_descs[0] = as2_gen;
	cap_descs[1] = as2_fmt;
	cap_lens[0] = sizeof(as2_gen);
	cap_lens[1] = sizeof(as2_fmt);
	assert(uac_desc_parse_as_alt(UAC_VERSION_1, 2U, 1U, cap_descs, cap_lens, 2U, &cap_alt) == 0);
	uac_desc_add_ep(&cap_alt, 0x82U, 0x0dU, 100U, 1U, 0x00U);
	assert(uac_desc_resolve_path(&ac, &cap_alt) == 0);

	assert(cap_alt.capture == 1U);
	assert(cap_alt.channels == 1U);
	assert(cap_alt.fu.unit_id == 5U);
	assert(cap_alt.fu.has_mute == 1U);
	assert(cap_alt.fu.master_mute == 1U);
	assert(cap_alt.fu.has_volume == 1U);
	assert(cap_alt.fu.master_volume == 0U);
}

/* Tests UAC 1.0 continuous sample rate range and 24-bit packed subslot. */
static void
test_uac1_continuous_rates_and_24bit(
	void)
{
	static const uint8_t as_gen[] = {
		0x07, 0x24, 0x01, 0x01, 0x01, 0x01, 0x00
	};
	static const uint8_t as_fmt[] = {
		/* FORMAT_TYPE_I: 2ch, 3-byte subslot, 24-bit, continuous 8000..96000 */
		0x0e, 0x24, 0x02, 0x01, 0x02, 0x03, 0x18, 0x00,
		0x40, 0x1f, 0x00, 0x00, 0x77, 0x01
	};
	const void *descs[2];
	size_t lens[2];
	struct uac_as_alt alt;

	descs[0] = as_gen;
	descs[1] = as_fmt;
	lens[0] = sizeof(as_gen);
	lens[1] = sizeof(as_fmt);
	assert(uac_desc_parse_as_alt(UAC_VERSION_1, 1U, 2U, descs, lens, 2U, &alt) == 0);

	assert(alt.is_pcm == 1U);
	assert(alt.channels == 2U);
	assert(alt.subslot_size == 3U);
	assert(alt.bit_resolution == 24U);
	assert(alt.continuous_rates == 1U);
	assert(alt.min_rate == 8000U);
	assert(alt.max_rate == 96000U);
	assert(uac_desc_alt_supports_rate(&alt, 48000U) == 1);
	assert(uac_desc_alt_supports_rate(&alt, 192000U) == 0);
}

/* Tests UAC 2.0 Clock Source, Feature Unit 2-bit controls, and clock range parsing. */
static void
test_uac2_topology_and_clock_ranges(
	void)
{
	static const uint8_t ac_block[] = {
		/* UAC2 AC_HEADER (9 bytes, bcdADC=0x0200) */
		0x09, 0x24, 0x01, 0x00, 0x02, 0x04, 0x40, 0x00, 0x00,
		/* UAC2 CLOCK_SOURCE (ID=10) */
		0x08, 0x24, 0x0a, 0x0a, 0x01, 0x07, 0x00, 0x00,
		/* UAC2 CLOCK_SELECTOR (ID=11, 1 pin -> ID=10) */
		0x07, 0x24, 0x0b, 0x0b, 0x01, 0x0a, 0x00,
		/* UAC2 INPUT_TERMINAL (ID=1, USB Streaming 0x0101, bCSourceID=11, 2ch) */
		0x11, 0x24, 0x02, 0x01, 0x01, 0x01, 0x00, 0x0b,
		0x02, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		/* UAC2 FEATURE_UNIT (ID=2, src=1, master=0x0f (Mute RW | Vol RW), ch1=0, ch2=0, iFeature=0) */
		0x12, 0x24, 0x06, 0x02, 0x01,
		0x0f, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,
		0x00,
		/* UAC2 OUTPUT_TERMINAL (ID=3, Headphones 0x0302, src=2, bCSourceID=11) */
		0x0c, 0x24, 0x03, 0x03, 0x02, 0x03, 0x00, 0x02, 0x0b, 0x00, 0x00, 0x00
	};
	static const uint8_t as_gen[] = {
		0x10, 0x24, 0x01, 0x01, 0x00, 0x01,
		0x01, 0x00, 0x00, 0x00,
		0x02, 0x03, 0x00, 0x00, 0x00, 0x00
	};
	static const uint8_t as_fmt[] = {
		0x06, 0x24, 0x02, 0x01, 0x04, 0x20
	};
	/* UAC2 Layout 3 RANGE reply: 2 discrete rates (48000, 96000) */
	static const uint8_t clock_ranges[] = {
		0x02, 0x00,
		0x80, 0xbb, 0x00, 0x00, 0x80, 0xbb, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x77, 0x01, 0x00, 0x00, 0x77, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00
	};
	const void *descs[2];
	size_t lens[2];
	struct uac_control_desc ac;
	struct uac_as_alt alt;

	assert(uac_desc_parse_ac_block(UAC_VERSION_2, ac_block, sizeof(ac_block), &ac) == 0);
	assert(ac.bcd_adc == 0x0200U);

	descs[0] = as_gen;
	descs[1] = as_fmt;
	lens[0] = sizeof(as_gen);
	lens[1] = sizeof(as_fmt);
	assert(uac_desc_parse_as_alt(UAC_VERSION_2, 1U, 1U, descs, lens, 2U, &alt) == 0);
	assert(uac_desc_resolve_path(&ac, &alt) == 0);
	assert(uac_desc_parse_clock_ranges(clock_ranges, sizeof(clock_ranges), &alt) == 0);

	assert(alt.capture == 0U);
	assert(alt.is_pcm == 1U);
	assert(alt.channels == 2U);
	assert(alt.subslot_size == 4U);
	assert(alt.bit_resolution == 32U);
	assert(alt.clock_id == 10U);
	assert(alt.fu.unit_id == 2U);
	assert(alt.fu.has_mute == 1U && alt.fu.mute_rw == 1U);
	assert(alt.fu.has_volume == 1U && alt.fu.volume_rw == 1U);
	assert(alt.rate_count == 2U);
	assert(alt.rates[0] == 48000U);
	assert(alt.rates[1] == 96000U);
}

/* Tests truncated or invalid descriptors fail cleanly without out-of-bounds access. */
static void
test_malformed_descriptors(
	void)
{
	static const uint8_t short_header[] = { 0x05, 0x24, 0x01, 0x00, 0x01 };
	const void *descs[1];
	size_t lens[1];
	struct uac_control_desc ac;
	struct uac_as_alt alt;

	descs[0] = short_header;
	lens[0] = sizeof(short_header);
	assert(uac_desc_parse_ac(UAC_VERSION_1, descs, lens, 1U, &ac) != 0);
	assert(uac_desc_parse_as_alt(UAC_VERSION_1, 1U, 1U, descs, lens, 1U, &alt) != 0);
}
