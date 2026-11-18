/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The shared display code (../display/) on the host: the EDID decode on
 * real sinks' EDIDs (the Latitude 5330 panel, an HDMI LCD) and on built
 * ones (a dummy plug whose preferred timing is 1920x1080 at 30 Hz, a
 * 2560x1440 monitor with and without HDMI, a 4K television, a monitor
 * with only range limits and standard timings), damaged EDIDs, the named
 * and computed timings, the link limits at their edges, and the mode list
 * with its default.
 */

#include "drivers/gpu/display/edid.h"
#include "drivers/gpu/display/link.h"
#include "drivers/gpu/display/modes.h"
#include "drivers/gpu/display/timing.h"
#include "drivers/gpu/i915/tests/display/dp-fixture-latitude5330.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uapi/errno.h>

/* The bytes a built EDID has room for: a base block and three extensions. */
#define TEST_EDID_BYTES		(GPU_EDID_BLOCK_BYTES * GPU_EDID_BLOCKS_MAX)

/*
 * One timing for the EDID builder: the pixel clock in kHz (a multiple of
 * 10), the line and the frame, and the descriptor's flags byte.
 */
struct test_detail {
	uint32_t pixel_khz;
	uint32_t h_active;
	uint32_t h_front;
	uint32_t h_sync;
	uint32_t h_back;
	uint32_t v_active;
	uint32_t v_front;
	uint32_t v_sync;
	uint32_t v_back;
	uint8_t flags;
};

/* The checks run, and the checks failed; the result of the whole run. */
static unsigned test_checks;
static unsigned test_failures;

/* The decode and list every test case writes into (large: kept off the stack). */
static struct gpu_edid test_edid;
static struct gpu_mode_list test_list;

/*
 * The HDMI LCD's EDID as read on the Latitude 5330's HDMI port
 * (plan/tools/i915-hw/hdmi/lcd-edid.hex): 1920x1280, an HDMI vendor block
 * that limits TMDS to 225 MHz, range limits of 48 to 60 Hz.
 */
static const uint8_t test_lcd_edid[256] = {
	0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x2a, 0x87, 0x30, 0x12, 0x00, 0x00, 0x00, 0x00,
	0x32, 0x23, 0x01, 0x03, 0x80, 0x1a, 0x11, 0x78, 0xea, 0x1e, 0xc5, 0xae, 0x4f, 0x34, 0xb1, 0x26,
	0x0e, 0x50, 0x54, 0x21, 0x08, 0x00, 0x81, 0xc0, 0x81, 0x00, 0x81, 0x80, 0x95, 0x00, 0xb3, 0x00,
	0xd1, 0xc0, 0x01, 0x01, 0x01, 0x01, 0x34, 0x40, 0x80, 0xa0, 0x70, 0x00, 0x25, 0x50, 0x30, 0x20,
	0x3a, 0x00, 0x03, 0xad, 0x10, 0x00, 0x00, 0x1a, 0x00, 0x00, 0x00, 0xff, 0x00, 0x4e, 0x41, 0x0a,
	0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfc, 0x00, 0x53,
	0x31, 0x32, 0x33, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfd,
	0x00, 0x30, 0x3c, 0x1e, 0xb4, 0x3c, 0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x01, 0x61,
	0x02, 0x03, 0x1a, 0xf1, 0x45, 0x01, 0x02, 0x03, 0x04, 0x10, 0x23, 0x09, 0x07, 0x07, 0x83, 0x01,
	0x00, 0x00, 0x67, 0x03, 0x0c, 0x00, 0x10, 0x00, 0x38, 0x2d, 0x02, 0x3a, 0x80, 0x18, 0x71, 0x38,
	0x2d, 0x40, 0x58, 0x2c, 0x45, 0x00, 0x03, 0xad, 0x10, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x57
};

/*
 * The built sinks' detailed timings: 1920x1080 at 30.002 Hz (the dummy
 * plug's preferred timing), 2560x1440 at 59.951 Hz (CVT reduced blanking,
 * 241.5 MHz) and at 29.976 Hz, and 3840x2160 at 60 Hz (CTA-861 format 97).
 */
static const struct test_detail test_plug_30 = { 69330U, 1920U, 48U, 32U, 80U, 1080U, 3U, 5U, 23U, 0x1aU };
static const struct test_detail test_qhd_60 = { 241500U, 2560U, 48U, 32U, 80U, 1440U, 3U, 5U, 33U, 0x1aU };
static const struct test_detail test_qhd_30 = { 120750U, 2560U, 48U, 32U, 80U, 1440U, 3U, 5U, 33U, 0x1aU };
static const struct test_detail test_uhd_60 = { 594000U, 3840U, 176U, 88U, 296U, 2160U, 8U, 10U, 72U, 0x1eU };

static void check(int good, const char *format, ...);
static void build_base(uint8_t *bytes, const char *vendor, uint32_t product, uint32_t serial);
static void build_detail(uint8_t *descriptor, const struct test_detail *detail);
static void build_text(uint8_t *descriptor, uint8_t tag, const char *text);
static void build_range(uint8_t *descriptor, uint32_t v_min, uint32_t v_max, uint32_t h_min, uint32_t h_max, uint32_t clock_10mhz);
static void build_standard(uint8_t *bytes, uint32_t slot, uint32_t width, uint32_t aspect, uint32_t refresh);
static void build_sum(uint8_t *block);
static void build_cta(uint8_t *block, const uint8_t *formats, uint32_t format_count, uint32_t hdmi_tmds_5mhz, uint32_t forum_tmds_5mhz, const struct test_detail *detail);
static void build_plug(uint8_t *bytes);
static void build_qhd(uint8_t *bytes, int hdmi);
static void build_tv(uint8_t *bytes);
static void build_range_only(uint8_t *bytes);
static void caps_set(struct gpu_link_caps *caps, uint32_t kind, uint32_t tmds_khz, uint32_t lanes, uint32_t symbol_khz);
static int list_has(uint32_t width, uint32_t height, uint32_t refresh_mhz);
static int list_ordered(void);
static void test_timings(void);
static void test_panel(void);
static void test_lcd(void);
static void test_plug(void);
static void test_qhd(void);
static void test_tv(void);
static void test_range_only(void);
static void test_damaged(void);
static void test_link_edges(void);
static int test_fits_1080(void *context, const struct gpu_display_timing *timing);
static void test_driver_limits(void);

/*
 * Runs every case and reports PASS or FAIL with the counts.
 */
int
main(void)
{
	/* The cases. */
	test_timings();
	test_panel();
	test_lcd();
	test_plug();
	test_qhd();
	test_tv();
	test_range_only();
	test_damaged();
	test_link_edges();
	test_driver_limits();

	/* The verdict. */
	if (test_failures != 0U) {
		printf("host-display-modes-test: FAIL (%u of %u checks)\n", test_failures, test_checks);
		return 1;
	}
	printf("host-display-modes-test: PASS (%u checks)\n", test_checks);
	return 0;
}

/* Counts one check and prints it when it fails. */
static void
check(
	int good,
	const char *format,
	...)
{
	va_list arguments;

	/* Counts the check. */
	test_checks++;
	if (good)
		return;

	/* Prints what failed. */
	test_failures++;
	printf("FAIL: ");
	va_start(arguments, format);
	vprintf(format, arguments);
	va_end(arguments);
	printf("\n");
}

/* Writes an EDID 1.4 digital base block with no timings and every standard slot unused. */
static void
build_base(
	uint8_t *bytes,
	const char *vendor,
	uint32_t product,
	uint32_t serial)
{
	static const uint8_t header[8] = { 0x00U, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0x00U };
	uint32_t letters;
	uint32_t index;

	/* An empty EDID with its header. */
	memset(bytes, 0, TEST_EDID_BYTES);
	memcpy(bytes, header, sizeof(header));

	/* The vendor letters, product and serial. */
	letters = ((uint32_t)(vendor[0] - 'A' + 1) << 10) | ((uint32_t)(vendor[1] - 'A' + 1) << 5) | (uint32_t)(vendor[2] - 'A' + 1);
	bytes[8] = (uint8_t)(letters >> 8);
	bytes[9] = (uint8_t)letters;
	bytes[10] = (uint8_t)product;
	bytes[11] = (uint8_t)(product >> 8);
	bytes[12] = (uint8_t)serial;
	bytes[13] = (uint8_t)(serial >> 8);
	bytes[14] = (uint8_t)(serial >> 16);
	bytes[15] = (uint8_t)(serial >> 24);

	/* Version 1.4, digital, 60 cm by 34 cm. */
	bytes[18] = 1U;
	bytes[19] = 4U;
	bytes[20] = 0xa5U;
	bytes[21] = 60U;
	bytes[22] = 34U;

	/* Every standard timing slot unused. */
	for (index = 0U; index < 16U; index++)
		bytes[38U + index] = 0x01U;

	/* Every descriptor a dummy one (tag 0x10). */
	for (index = 0U; index < 4U; index++)
		bytes[54U + index * 18U + 3U] = 0x10U;
}

/* Writes a detailed timing descriptor. */
static void
build_detail(
	uint8_t *descriptor,
	const struct test_detail *detail)
{
	uint32_t clock;
	uint32_t h_blank;
	uint32_t v_blank;

	/* The clock in 10 kHz, and the blanks. */
	clock = detail->pixel_khz / 10U;
	h_blank = detail->h_front + detail->h_sync + detail->h_back;
	v_blank = detail->v_front + detail->v_sync + detail->v_back;

	/* The fields, each split into its low byte and high bits. */
	memset(descriptor, 0, 18U);
	descriptor[0] = (uint8_t)clock;
	descriptor[1] = (uint8_t)(clock >> 8);
	descriptor[2] = (uint8_t)detail->h_active;
	descriptor[3] = (uint8_t)h_blank;
	descriptor[4] = (uint8_t)(((detail->h_active >> 8) << 4) | (h_blank >> 8));
	descriptor[5] = (uint8_t)detail->v_active;
	descriptor[6] = (uint8_t)v_blank;
	descriptor[7] = (uint8_t)(((detail->v_active >> 8) << 4) | (v_blank >> 8));
	descriptor[8] = (uint8_t)detail->h_front;
	descriptor[9] = (uint8_t)detail->h_sync;
	descriptor[10] = (uint8_t)(((detail->v_front & 0x0fU) << 4) | (detail->v_sync & 0x0fU));
	descriptor[11] = (uint8_t)(((detail->h_front >> 8) << 6) | ((detail->h_sync >> 8) << 4) | ((detail->v_front >> 4) << 2) | (detail->v_sync >> 4));
	descriptor[17] = detail->flags;
}

/* Writes a text display descriptor (name 0xfc, serial 0xff). */
static void
build_text(
	uint8_t *descriptor,
	uint8_t tag,
	const char *text)
{
	size_t length;
	size_t index;

	/* The header, then the text ended by a line feed and padded with spaces. */
	memset(descriptor, 0, 18U);
	descriptor[3] = tag;
	length = strlen(text);
	for (index = 0U; index < 13U; index++) {
		if (index < length) {
			descriptor[5U + index] = (uint8_t)text[index];
		} else if (index == length) {
			descriptor[5U + index] = 0x0aU;
		} else {
			descriptor[5U + index] = 0x20U;
		}
	}
}

/* Writes a range limits descriptor. */
static void
build_range(
	uint8_t *descriptor,
	uint32_t v_min,
	uint32_t v_max,
	uint32_t h_min,
	uint32_t h_max,
	uint32_t clock_10mhz)
{
	/* The tag and the limits, with no offsets. */
	memset(descriptor, 0, 18U);
	descriptor[3] = 0xfdU;
	descriptor[5] = (uint8_t)v_min;
	descriptor[6] = (uint8_t)v_max;
	descriptor[7] = (uint8_t)h_min;
	descriptor[8] = (uint8_t)h_max;
	descriptor[9] = (uint8_t)clock_10mhz;
	descriptor[10] = 0x01U;
}

/* Writes a standard timing (aspect 0 16:10, 1 4:3, 2 5:4, 3 16:9). */
static void
build_standard(
	uint8_t *bytes,
	uint32_t slot,
	uint32_t width,
	uint32_t aspect,
	uint32_t refresh)
{
	/* The width code, then the aspect and refresh. */
	bytes[38U + slot * 2U] = (uint8_t)(width / 8U - 31U);
	bytes[39U + slot * 2U] = (uint8_t)((aspect << 6) | (refresh - 60U));
}

/* Sets a block's checksum byte so the block sums to 0. */
static void
build_sum(
	uint8_t *block)
{
	uint32_t sum;
	uint32_t index;

	/* Adds up the other bytes. */
	sum = 0U;
	for (index = 0U; index < 127U; index++)
		sum += block[index];
	block[127] = (uint8_t)(256U - (sum & 0xffU));
}

/*
 * Writes a CTA-861 revision 3 extension: a video block with the formats,
 * an HDMI vendor block when hdmi_tmds_5mhz is not zero (with that TMDS
 * limit; 255 means no limit given), an HDMI Forum block when
 * forum_tmds_5mhz is not zero (with the control channel), then one
 * detailed timing when detail is not NULL.
 */
static void
build_cta(
	uint8_t *block,
	const uint8_t *formats,
	uint32_t format_count,
	uint32_t hdmi_tmds_5mhz,
	uint32_t forum_tmds_5mhz,
	const struct test_detail *detail)
{
	uint32_t offset;
	uint32_t index;

	/* The header. */
	memset(block, 0, GPU_EDID_BLOCK_BYTES);
	block[0] = 0x02U;
	block[1] = 0x03U;
	offset = 4U;

	/* The video block. */
	block[offset] = (uint8_t)((2U << 5) | format_count);
	offset++;
	for (index = 0U; index < format_count; index++) {
		block[offset] = formats[index];
		offset++;
	}

	/* The HDMI vendor block: OUI, address 1.0.0.0, flags, and the TMDS limit unless none. */
	if (hdmi_tmds_5mhz != 0U) {
		if (hdmi_tmds_5mhz == 255U) {
			block[offset] = (uint8_t)((3U << 5) | 5U);
		} else {
			block[offset] = (uint8_t)((3U << 5) | 7U);
		}
		block[offset + 1U] = 0x03U;
		block[offset + 2U] = 0x0cU;
		block[offset + 3U] = 0x00U;
		block[offset + 4U] = 0x10U;
		block[offset + 5U] = 0x00U;
		if (hdmi_tmds_5mhz == 255U) {
			offset += 6U;
		} else {
			block[offset + 6U] = 0x00U;
			block[offset + 7U] = (uint8_t)hdmi_tmds_5mhz;
			offset += 8U;
		}
	}

	/* The HDMI Forum block: OUI, version, the TMDS rate, and the control channel. */
	if (forum_tmds_5mhz != 0U) {
		block[offset] = (uint8_t)((3U << 5) | 6U);
		block[offset + 1U] = 0xd8U;
		block[offset + 2U] = 0x5dU;
		block[offset + 3U] = 0xc4U;
		block[offset + 4U] = 0x01U;
		block[offset + 5U] = (uint8_t)forum_tmds_5mhz;
		block[offset + 6U] = 0x80U;
		offset += 7U;
	}

	/* The detailed timings start after the blocks. */
	block[2] = (uint8_t)offset;
	if (detail != NULL)
		build_detail(block + offset, detail);
	build_sum(block);
}

/*
 * The dummy plug: preferred 1920x1080 at 30.002 Hz, then 2560x1440 at
 * 59.951 Hz, standard timings 1920x1080 and 1280x720 at 60 Hz, the
 * established 640x480, 800x600 and 1024x768 at 60 Hz, and no extension
 * (a DVI sink).
 */
static void
build_plug(
	uint8_t *bytes)
{
	/* The base block. */
	build_base(bytes, "PLG", 0x1080U, 0U);
	build_detail(bytes + 54U, &test_plug_30);
	build_detail(bytes + 72U, &test_qhd_60);
	build_text(bytes + 90U, 0xfcU, "DUMMY PLUG");
	build_standard(bytes, 0U, 1920U, 3U, 60U);
	build_standard(bytes, 1U, 1280U, 3U, 60U);
	bytes[35] = 0x21U;
	bytes[36] = 0x08U;
	build_sum(bytes);
}

/*
 * The 2560x1440 monitor: preferred 2560x1440 at 59.951 Hz, 2560x1440 at
 * 29.976 Hz, range limits 24 to 75 Hz, 30 to 160 kHz, 250 MHz, standard
 * timings 1920x1080, 1680x1050 and 1280x1024 at 60 Hz; with hdmi, a CTA-861
 * extension with formats 16 and 4 and an HDMI vendor block without a TMDS
 * limit.
 */
static void
build_qhd(
	uint8_t *bytes,
	int hdmi)
{
	static const uint8_t formats[2] = { 0x90U, 0x04U };

	/* The base block. */
	build_base(bytes, "QHD", 0x2714U, 0x01234567U);
	build_detail(bytes + 54U, &test_qhd_60);
	build_detail(bytes + 72U, &test_qhd_30);
	build_range(bytes + 90U, 24U, 75U, 30U, 160U, 25U);
	build_text(bytes + 108U, 0xfcU, "QHD 27");
	build_standard(bytes, 0U, 1920U, 3U, 60U);
	build_standard(bytes, 1U, 1680U, 0U, 60U);
	build_standard(bytes, 2U, 1280U, 2U, 60U);

	/* The extension, for the HDMI sink. */
	if (hdmi) {
		bytes[126] = 1U;
		build_cta(bytes + 128U, formats, 2U, 255U, 0U, NULL);
	}
	build_sum(bytes);
}

/*
 * The 4K television: preferred 3840x2160 at 60 Hz, and a CTA-861 extension
 * with formats 97, 96, 95, 16, 31, 4 and 3, an HDMI vendor block of
 * 340 MHz and an HDMI Forum block of 600 MHz with the control channel.
 */
static void
build_tv(
	uint8_t *bytes)
{
	static const uint8_t formats[7] = { 0x61U, 0x60U, 0x5fU, 0x10U, 0x1fU, 0x04U, 0x03U };

	/* The base block, then the extension. */
	build_base(bytes, "TVS", 0x4000U, 0U);
	build_detail(bytes + 54U, &test_uhd_60);
	build_text(bytes + 72U, 0xfcU, "TELEVISION");
	bytes[126] = 1U;
	build_cta(bytes + 128U, formats, 7U, 68U, 120U, NULL);
	build_sum(bytes);
}

/*
 * A monitor with only range limits and standard timings: 56 to 76 Hz,
 * 30 to 83 kHz, 150 MHz; 1920x1200, 1680x1050, 1600x1200 and 1280x1024 at
 * 60 Hz, 1280x1024 at 75 Hz, and the established 1024x768 at 75 Hz.
 */
static void
build_range_only(
	uint8_t *bytes)
{
	/* The base block. */
	build_base(bytes, "RNG", 0x0001U, 0U);
	build_range(bytes + 54U, 56U, 76U, 30U, 83U, 15U);
	build_standard(bytes, 0U, 1920U, 0U, 60U);
	build_standard(bytes, 1U, 1680U, 0U, 60U);
	build_standard(bytes, 2U, 1600U, 1U, 60U);
	build_standard(bytes, 3U, 1280U, 2U, 60U);
	build_standard(bytes, 4U, 1280U, 2U, 75U);
	bytes[36] = 0x02U;
	build_sum(bytes);
}

/* Fills link caps of one kind with no controller limit. */
static void
caps_set(
	struct gpu_link_caps *caps,
	uint32_t kind,
	uint32_t tmds_khz,
	uint32_t lanes,
	uint32_t symbol_khz)
{
	/* The kind and its limits. */
	memset(caps, 0, sizeof(*caps));
	caps->kind = kind;
	caps->tmds_max_khz = tmds_khz;
	caps->dp_lanes = lanes;
	caps->dp_symbol_khz = symbol_khz;
}

/*
 * Tells whether the built list has a mode of a size and rate.
 *
 * Returns 1 when it has, 0 when it has not.
 */
static int
list_has(
	uint32_t width,
	uint32_t height,
	uint32_t refresh_mhz)
{
	uint32_t index;
	int error;

	/* Looks the mode up as a driver would. */
	error = gpu_modes_find(&test_list, width, height, refresh_mhz, &index);
	if (error != 0)
		return 0;
	return 1;
}

/*
 * Tells whether the built list after its default is in order: more pixels
 * first, then a higher refresh.
 *
 * Returns 1 when it is, 0 when it is not.
 */
static int
list_ordered(void)
{
	uint64_t previous;
	uint64_t current;
	uint32_t index;

	/* Compares each mode after the second with the one before it. */
	for (index = 2U; index < test_list.count; index++) {
		previous = (uint64_t)test_list.modes[index - 1U].h_active * test_list.modes[index - 1U].v_active;
		current = (uint64_t)test_list.modes[index].h_active * test_list.modes[index].v_active;
		if (current > previous)
			return 0;
		if (current == previous && test_list.modes[index].refresh_mhz > test_list.modes[index - 1U].refresh_mhz)
			return 0;
	}
	return 1;
}

/* The named and computed timings and the rate comparison. */
static void
test_timings(void)
{
	struct gpu_display_timing timing;
	int error;

	/* CVT reduced blanking: 2560x1440 at 60 Hz is 241.5 MHz, 2720x1481, 59.951 Hz. */
	error = gpu_display_timing_reduced(2560U, 1440U, 60U, &timing);
	check(error == 0 && timing.pixel_khz == 241500U, "cvt-rb 2560x1440: %d %u kHz", error, timing.pixel_khz);
	check(timing.h_active + timing.h_front + timing.h_sync + timing.h_back == 2720U, "cvt-rb 2560x1440: htotal");
	check(timing.v_active + timing.v_front + timing.v_sync + timing.v_back == 1481U && timing.v_sync == 5U,
	    "cvt-rb 2560x1440: vtotal %u vsync %u", timing.v_active + timing.v_front + timing.v_sync + timing.v_back, timing.v_sync);
	check(timing.refresh_mhz == 59951U, "cvt-rb 2560x1440: %u mHz", timing.refresh_mhz);
	check(timing.flags == GPU_DISPLAY_TIMING_H_POSITIVE, "cvt-rb: +h -v");

	/* CVT reduced blanking: 1920x1080 at 60 Hz is 138.5 MHz; 1920x1200 is 154 MHz with a 6-line sync. */
	error = gpu_display_timing_reduced(1920U, 1080U, 60U, &timing);
	check(error == 0 && timing.pixel_khz == 138500U, "cvt-rb 1920x1080: %u kHz", timing.pixel_khz);
	error = gpu_display_timing_reduced(1920U, 1200U, 60U, &timing);
	check(error == 0 && timing.pixel_khz == 154000U && timing.v_sync == 6U, "cvt-rb 1920x1200: %u kHz", timing.pixel_khz);

	/* CVT refuses nothing to work with. */
	error = gpu_display_timing_reduced(0U, 1080U, 60U, &timing);
	check(error == EINVAL, "cvt-rb: zero width");
	error = gpu_display_timing_reduced(1920U, 1080U, 0U, &timing);
	check(error == EINVAL, "cvt-rb: zero rate");
	error = gpu_display_timing_reduced(1920U, 1080U, 3000U, &timing);
	check(error == EINVAL, "cvt-rb: a frame shorter than the blank");

	/* Monitor timings: 1920x1080@60 is 148.5 MHz at exactly 60 Hz; 640x480@60 is 59.94 Hz. */
	error = gpu_display_timing_monitor(1920U, 1080U, 60U, &timing);
	check(error == 0 && timing.pixel_khz == 148500U && timing.refresh_mhz == 60000U, "dmt 1920x1080@60: %u mHz", timing.refresh_mhz);
	error = gpu_display_timing_monitor(640U, 480U, 60U, &timing);
	check(error == 0 && timing.refresh_mhz == 59940U, "dmt 640x480@60: %u mHz", timing.refresh_mhz);
	error = gpu_display_timing_monitor(2560U, 1440U, 60U, &timing);
	check(error == ENOENT, "dmt: no 2560x1440");

	/* CTA formats: 16 is 1080p60, 95 is 2160p30 at 297 MHz, 97 is 2160p60 at 594 MHz; 5 (interlaced) is not listed. */
	error = gpu_display_timing_format(16U, &timing);
	check(error == 0 && timing.h_active == 1920U && timing.refresh_mhz == 60000U, "vic 16");
	error = gpu_display_timing_format(95U, &timing);
	check(error == 0 && timing.pixel_khz == 297000U && timing.refresh_mhz == 30000U, "vic 95: %u mHz", timing.refresh_mhz);
	error = gpu_display_timing_format(97U, &timing);
	check(error == 0 && timing.pixel_khz == 594000U && timing.refresh_mhz == 60000U, "vic 97");
	error = gpu_display_timing_format(32U, &timing);
	check(error == 0 && timing.refresh_mhz == 24000U, "vic 32: %u mHz", timing.refresh_mhz);
	error = gpu_display_timing_format(5U, &timing);
	check(error == ENOENT, "vic 5 interlaced");

	/* The same rate: within 0.05 Hz. */
	check(gpu_display_timing_same_rate(60000U, 60049U) == 1, "60.049 is 60");
	check(gpu_display_timing_same_rate(60049U, 60000U) == 1, "60 is 60.049");
	check(gpu_display_timing_same_rate(60000U, 60050U) == 0, "60.050 is not 60");
	check(gpu_display_timing_same_rate(59940U, 60000U) == 0, "59.94 is not 60");
}

/* The Latitude 5330 panel on its eDP link (HBR, 2 lanes). */
static void
test_panel(void)
{
	struct gpu_link_caps caps;
	int error;

	/* The decode: AUO, two detailed timings of 1920x1080 at 60.011 and 48.009 Hz. */
	error = gpu_edid_decode(i915_dp_fixture_edid, sizeof(i915_dp_fixture_edid), &test_edid);
	check(error == 0, "panel: decode %d", error);
	check(strcmp(test_edid.vendor, "AUO") == 0 && test_edid.product == 0x2b99U, "panel: AUO 0x%x", test_edid.product);
	check(test_edid.width_mm == 290U && test_edid.height_mm == 170U && test_edid.digital == 1U, "panel: size");
	check(test_edid.has_preferred == 1U && test_edid.preferred.pixel_khz == 140800U && test_edid.preferred.refresh_mhz == 60011U,
	    "panel: preferred %u kHz %u mHz", test_edid.preferred.pixel_khz, test_edid.preferred.refresh_mhz);
	check(test_edid.timing_count == 2U && test_edid.timings[1].refresh_mhz == 48009U, "panel: two timings (%u)", test_edid.timing_count);
	check(test_edid.cta == 0U && test_edid.hdmi == 0U, "panel: no extension");

	/* The list on eDP: both rates, the 60 Hz one the default. */
	caps_set(&caps, GPU_LINK_EDP, 0U, 2U, 270000U);
	error = gpu_modes_build(&test_edid, &caps, NULL, NULL, &test_list);
	check(error == 0 && test_list.count == 2U, "panel: two modes (%d, %u)", error, test_list.count);
	check(test_list.modes[0].refresh_mhz == 60011U && test_list.modes[1].refresh_mhz == 48009U, "panel: default 60 Hz");

	/* A panel takes no standard or established timing. */
	error = gpu_display_timing_monitor(1280U, 720U, 60U, &test_list.modes[1]);
	test_list.modes[1].source = GPU_DISPLAY_TIMING_STANDARD;
	check(error == 0 && gpu_link_check(&caps, &test_edid, &test_list.modes[1]) == GPU_LINK_PANEL, "panel: no 720p");
}

/* The HDMI LCD on an HDMI output, and on a DVI output. */
static void
test_lcd(void)
{
	struct gpu_link_caps caps;
	int error;

	/* The decode: JTG "S123", HDMI with a 225 MHz limit, range 48 to 60 Hz and 600 MHz. */
	error = gpu_edid_decode(test_lcd_edid, sizeof(test_lcd_edid), &test_edid);
	check(error == 0 && test_edid.blocks == 2U, "lcd: decode %d blocks %u", error, test_edid.blocks);
	check(strcmp(test_edid.vendor, "JTG") == 0 && strcmp(test_edid.name, "S123") == 0 && strcmp(test_edid.serial_text, "NA") == 0,
	    "lcd: '%s' '%s' '%s'", test_edid.vendor, test_edid.name, test_edid.serial_text);
	check(test_edid.cta == 1U && test_edid.hdmi == 1U && test_edid.hdmi_tmds_max_khz == 225000U, "lcd: hdmi %u", test_edid.hdmi_tmds_max_khz);
	check(test_edid.has_range == 1U && test_edid.v_min_hz == 48U && test_edid.v_max_hz == 60U && test_edid.range_pixel_max_khz == 600000U,
	    "lcd: range");
	check(test_edid.preferred.h_active == 1920U && test_edid.preferred.v_active == 1280U && test_edid.preferred.refresh_mhz == 59999U,
	    "lcd: preferred %ux%u@%u", test_edid.preferred.h_active, test_edid.preferred.v_active, test_edid.preferred.refresh_mhz);
	check(test_edid.timing_count == 11U, "lcd: eleven timings (%u)", test_edid.timing_count);

	/* On HDMI: all eleven, 1920x1280 the default, the rest in order. */
	caps_set(&caps, GPU_LINK_HDMI, 340000U, 0U, 0U);
	error = gpu_modes_build(&test_edid, &caps, NULL, NULL, &test_list);
	check(error == 0 && test_list.count == 11U, "lcd hdmi: %u modes", test_list.count);
	check(test_list.modes[0].h_active == 1920U && test_list.modes[0].v_active == 1280U, "lcd hdmi: default 1920x1280");
	check(test_list.modes[1].v_active == 1080U && test_list.modes[10].h_active == 640U, "lcd hdmi: order");
	check(list_ordered(), "lcd hdmi: ordered");
	check(list_has(720U, 480U, 59940U) && list_has(1440U, 900U, 0U) && list_has(1024U, 768U, 60000U), "lcd hdmi: 480p, 1440x900, 1024x768");
	check(list_has(1920U, 1080U, 60020U) == 1 && list_has(1920U, 1080U, 59940U) == 0, "lcd hdmi: 1080p at 60.02 is 60, at 59.94 is not");
}

/*
 * The dummy plug: its preferred 1920x1080 at 30 Hz is listed but is not
 * the default; 1920x1080 at 60 Hz is.
 */
static void
test_plug(void)
{
	uint8_t bytes[TEST_EDID_BYTES];
	struct gpu_link_caps caps;
	int error;

	/* The decode. */
	build_plug(bytes);
	error = gpu_edid_decode(bytes, GPU_EDID_BLOCK_BYTES, &test_edid);
	check(error == 0 && test_edid.preferred.refresh_mhz == 30002U, "plug: preferred %u mHz", test_edid.preferred.refresh_mhz);
	check(strcmp(test_edid.name, "DUMMY PLUG") == 0 && strcmp(test_edid.vendor, "PLG") == 0, "plug: name");

	/* On a single-link DVI output: 2560x1440 does not fit, 1920x1080@60 is the default (the preferred size). */
	caps_set(&caps, GPU_LINK_DVI, 165000U, 0U, 0U);
	error = gpu_modes_build(&test_edid, &caps, NULL, NULL, &test_list);
	check(error == 0 && test_list.modes[0].h_active == 1920U && test_list.modes[0].refresh_mhz == 60000U,
	    "plug dvi: default %ux%u@%u", test_list.modes[0].h_active, test_list.modes[0].v_active, test_list.modes[0].refresh_mhz);
	check(list_has(1920U, 1080U, 30002U) && !list_has(2560U, 1440U, 0U), "plug dvi: 30 Hz listed, 2560x1440 not");
	check(list_has(1280U, 720U, 60000U) && list_has(640U, 480U, 59940U) && list_has(1024U, 768U, 60000U), "plug dvi: 720p, 640, 1024");
	check(list_ordered(), "plug dvi: ordered");

	/* On an HDMI output the sink is still DVI: the same list. */
	caps_set(&caps, GPU_LINK_HDMI, 340000U, 0U, 0U);
	error = gpu_modes_build(&test_edid, &caps, NULL, NULL, &test_list);
	check(error == 0 && !list_has(2560U, 1440U, 0U) && test_list.modes[0].refresh_mhz == 60000U, "plug hdmi: a DVI sink");

	/* On DP HBR2 with 4 lanes: 2560x1440 fits, yet the default stays at the preferred size. */
	caps_set(&caps, GPU_LINK_DP, 0U, 4U, 540000U);
	error = gpu_modes_build(&test_edid, &caps, NULL, NULL, &test_list);
	check(error == 0 && list_has(2560U, 1440U, 59951U), "plug dp: 2560x1440 listed");
	check(test_list.modes[0].h_active == 1920U && test_list.modes[0].refresh_mhz == 60000U, "plug dp: default 1080p60");
	check(test_list.modes[1].h_active == 2560U, "plug dp: 2560x1440 next");
}

/*
 * The 2560x1440 monitor: as a DVI sink its preferred timing is too fast for
 * one link and the default falls to the largest 50 Hz mode; as an HDMI
 * sink its preferred timing is the default.
 */
static void
test_qhd(void)
{
	uint8_t bytes[TEST_EDID_BYTES];
	struct gpu_link_caps caps;
	int error;

	/* The DVI sink on a single-link DVI output: 1920x1080@60. */
	build_qhd(bytes, 0);
	error = gpu_edid_decode(bytes, GPU_EDID_BLOCK_BYTES, &test_edid);
	check(error == 0 && test_edid.serial == 0x01234567U && test_edid.has_range == 1U, "qhd: decode");
	caps_set(&caps, GPU_LINK_DVI, 165000U, 0U, 0U);
	error = gpu_modes_build(&test_edid, &caps, NULL, NULL, &test_list);
	check(error == 0 && test_list.modes[0].h_active == 1920U && test_list.modes[0].refresh_mhz == 60000U,
	    "qhd dvi: default %ux%u@%u", test_list.modes[0].h_active, test_list.modes[0].v_active, test_list.modes[0].refresh_mhz);
	check(list_has(2560U, 1440U, 29976U) && !list_has(2560U, 1440U, 59951U), "qhd dvi: 1440p30 listed, 1440p60 not");
	check(list_has(1680U, 1050U, 0U) && list_has(1280U, 1024U, 60000U), "qhd dvi: standard timings");

	/* On a dual-link DVI output the preferred timing fits. */
	caps_set(&caps, GPU_LINK_DVI, 330000U, 0U, 0U);
	caps.flags = GPU_LINK_DUAL_LINK;
	error = gpu_modes_build(&test_edid, &caps, NULL, NULL, &test_list);
	check(error == 0 && test_list.modes[0].h_active == 2560U && test_list.modes[0].refresh_mhz == 59951U, "qhd dual: default 1440p60");

	/* The HDMI sink on an HDMI output: 2560x1440@59.951, and the CTA formats 1080p60 and 720p60. */
	build_qhd(bytes, 1);
	error = gpu_edid_decode(bytes, 2U * GPU_EDID_BLOCK_BYTES, &test_edid);
	check(error == 0 && test_edid.hdmi == 1U && test_edid.hdmi_tmds_max_khz == 0U, "qhd hdmi: decode");
	caps_set(&caps, GPU_LINK_HDMI, 340000U, 0U, 0U);
	error = gpu_modes_build(&test_edid, &caps, NULL, NULL, &test_list);
	check(error == 0 && test_list.modes[0].h_active == 2560U && test_list.modes[0].refresh_mhz == 59951U, "qhd hdmi: default 1440p60");
	check(list_has(1280U, 720U, 60000U) && list_ordered(), "qhd hdmi: 720p, ordered");

	/* The range limits drop a standard timing above 250 MHz: none here; 1280x1024@60 at 64 kHz passes. */
	check(list_has(1280U, 1024U, 60000U), "qhd hdmi: 1280x1024 in range");
}

/*
 * The 4K television: without scrambling 2160p60 and 2160p50 do not fit
 * and the default falls to 1080p60 (2160p30 is listed); with scrambling
 * 2160p60 is the default.
 */
static void
test_tv(void)
{
	uint8_t bytes[TEST_EDID_BYTES];
	struct gpu_link_caps caps;
	int error;

	/* The decode: HDMI 340 MHz, HDMI Forum 600 MHz with the control channel. */
	build_tv(bytes);
	error = gpu_edid_decode(bytes, 2U * GPU_EDID_BLOCK_BYTES, &test_edid);
	check(error == 0 && test_edid.hdmi_tmds_max_khz == 340000U && test_edid.forum_tmds_max_khz == 600000U && test_edid.scdc == 1U,
	    "tv: decode %d %u %u %u", error, test_edid.hdmi_tmds_max_khz, test_edid.forum_tmds_max_khz, test_edid.scdc);

	/* HDMI without scrambling. */
	caps_set(&caps, GPU_LINK_HDMI, 340000U, 0U, 0U);
	error = gpu_modes_build(&test_edid, &caps, NULL, NULL, &test_list);
	check(error == 0 && test_list.modes[0].h_active == 1920U && test_list.modes[0].refresh_mhz == 60000U,
	    "tv 340: default %ux%u@%u", test_list.modes[0].h_active, test_list.modes[0].v_active, test_list.modes[0].refresh_mhz);
	check(list_has(3840U, 2160U, 30000U) && !list_has(3840U, 2160U, 60000U) && !list_has(3840U, 2160U, 50000U), "tv 340: 2160p30 only");
	check(test_list.modes[1].h_active == 3840U, "tv 340: 2160p30 after the default");
	check(list_has(1920U, 1080U, 50000U) && list_has(720U, 480U, 59940U), "tv 340: 1080p50, 480p");

	/* HDMI with scrambling up to 600 MHz. */
	caps_set(&caps, GPU_LINK_HDMI, 600000U, 0U, 0U);
	caps.flags = GPU_LINK_SCRAMBLING;
	error = gpu_modes_build(&test_edid, &caps, NULL, NULL, &test_list);
	check(error == 0 && test_list.modes[0].h_active == 3840U && test_list.modes[0].refresh_mhz == 60000U, "tv 600: default 2160p60");
	check(list_has(3840U, 2160U, 50000U) && list_ordered(), "tv 600: 2160p50, ordered");

	/* Without the control channel the sink cannot scramble. */
	test_edid.scdc = 0U;
	error = gpu_modes_build(&test_edid, &caps, NULL, NULL, &test_list);
	check(error == 0 && !list_has(3840U, 2160U, 60000U), "tv 600 without scdc: no 2160p60");
}

/*
 * A monitor with only range limits and standard timings: no preferred
 * timing, so the default is the largest 50 Hz mode the range lets through.
 */
static void
test_range_only(void)
{
	uint8_t bytes[TEST_EDID_BYTES];
	struct gpu_link_caps caps;
	int error;

	/* The decode: no preferred timing, six timings. */
	build_range_only(bytes);
	error = gpu_edid_decode(bytes, GPU_EDID_BLOCK_BYTES, &test_edid);
	check(error == 0 && test_edid.has_preferred == 0U && test_edid.timing_count == 6U, "range: decode %d %u", error, test_edid.timing_count);

	/* 1920x1200 RB (154 MHz) and 1600x1200 (162 MHz) are above 150 MHz; 1280x1024@75 is 80 kHz, inside 83. */
	caps_set(&caps, GPU_LINK_DVI, 165000U, 0U, 0U);
	error = gpu_modes_build(&test_edid, &caps, NULL, NULL, &test_list);
	check(error == 0 && !list_has(1920U, 1200U, 0U) && !list_has(1600U, 1200U, 0U), "range: 150 MHz limit");
	check(test_list.modes[0].h_active == 1680U && test_list.modes[0].v_active == 1050U, "range: default 1680x1050");
	check(list_has(1280U, 1024U, 75025U) && list_has(1024U, 768U, 75029U), "range: 75 Hz modes");

	/* A tighter horizontal limit drops 1280x1024@75 (80 kHz). */
	test_edid.h_max_khz = 79U;
	error = gpu_modes_build(&test_edid, &caps, NULL, NULL, &test_list);
	check(error == 0 && !list_has(1280U, 1024U, 75025U) && list_has(1280U, 1024U, 60020U), "range: 79 kHz limit");

	/* A vertical limit of 56 to 70 Hz drops the 75 Hz modes. */
	test_edid.h_max_khz = 83U;
	test_edid.v_max_hz = 70U;
	error = gpu_modes_build(&test_edid, &caps, NULL, NULL, &test_list);
	check(error == 0 && !list_has(1024U, 768U, 0U) && list_has(1680U, 1050U, 0U), "range: 70 Hz limit");
}

/* Damaged and odd EDIDs. */
static void
test_damaged(void)
{
	uint8_t bytes[TEST_EDID_BYTES];
	struct gpu_link_caps caps;
	int error;

	/* A base block with a wrong checksum is refused whole. */
	build_plug(bytes);
	bytes[127] ^= 1U;
	error = gpu_edid_decode(bytes, GPU_EDID_BLOCK_BYTES, &test_edid);
	check(error == EINVAL && test_edid.timing_count == 0U, "damaged base: %d", error);

	/* A wrong header, a wrong version, a short buffer, none at all. */
	build_plug(bytes);
	bytes[1] = 0x00U;
	build_sum(bytes);
	error = gpu_edid_decode(bytes, GPU_EDID_BLOCK_BYTES, &test_edid);
	check(error == EINVAL, "no header: %d", error);
	build_plug(bytes);
	bytes[18] = 2U;
	build_sum(bytes);
	error = gpu_edid_decode(bytes, GPU_EDID_BLOCK_BYTES, &test_edid);
	check(error == EINVAL, "version 2: %d", error);
	error = gpu_edid_decode(bytes, 127U, &test_edid);
	check(error == EINVAL, "short: %d", error);
	error = gpu_edid_decode(NULL, 128U, &test_edid);
	check(error == EINVAL, "null: %d", error);

	/* An extension with a wrong checksum is dropped; the base block stays. */
	build_tv(bytes);
	bytes[255] ^= 1U;
	error = gpu_edid_decode(bytes, 2U * GPU_EDID_BLOCK_BYTES, &test_edid);
	check(error == 0 && test_edid.blocks == 1U && test_edid.bad_blocks == 1U && test_edid.hdmi == 0U, "damaged extension: %d", error);
	check(test_edid.timing_count == 1U && test_edid.preferred.h_active == 3840U, "damaged extension: base timing kept");

	/* An extension count beyond the bytes read reads only what is there. */
	build_tv(bytes);
	error = gpu_edid_decode(bytes, GPU_EDID_BLOCK_BYTES, &test_edid);
	check(error == 0 && test_edid.blocks == 1U && test_edid.cta == 0U, "count beyond the bytes: %d", error);

	/* A video block that claims more bytes than the collection holds ends the walk without reading past it. */
	build_tv(bytes);
	bytes[128U + 4U] = (uint8_t)((2U << 5) | 31U);
	build_sum(bytes + 128U);
	error = gpu_edid_decode(bytes, 2U * GPU_EDID_BLOCK_BYTES, &test_edid);
	check(error == 0 && test_edid.cta == 1U && test_edid.hdmi == 0U && test_edid.timing_count == 1U, "overlong data block: %u", test_edid.timing_count);

	/* An interlaced preferred timing is not decoded, and with no EDID nothing is listed. */
	build_plug(bytes);
	bytes[54U + 17U] |= 0x80U;
	build_sum(bytes);
	error = gpu_edid_decode(bytes, GPU_EDID_BLOCK_BYTES, &test_edid);
	check(error == 0 && test_edid.has_preferred == 0U && !(test_edid.timings[0].refresh_mhz == 30002U), "interlaced preferred");
	caps_set(&caps, GPU_LINK_DVI, 165000U, 0U, 0U);
	error = gpu_modes_build(NULL, &caps, NULL, NULL, &test_list);
	check(error == ENOENT && test_list.count == 0U, "no edid: ENOENT");

	/* Only modes below 50 Hz: the largest is the default. */
	build_base(bytes, "SLO", 1U, 0U);
	build_detail(bytes + 54U, &test_plug_30);
	build_detail(bytes + 72U, &test_qhd_30);
	build_sum(bytes);
	error = gpu_edid_decode(bytes, GPU_EDID_BLOCK_BYTES, &test_edid);
	caps_set(&caps, GPU_LINK_DP, 0U, 4U, 270000U);
	error |= gpu_modes_build(&test_edid, &caps, NULL, NULL, &test_list);
	check(error == 0 && test_list.count == 2U && test_list.modes[0].h_active == 2560U, "slow only: default 1440p30");
}

/* The link limits at their edges. */
static void
test_link_edges(void)
{
	struct gpu_display_timing timing;
	struct gpu_link_caps caps;
	uint32_t answer;
	int error;

	/* A 1080p60 timing to change the clock of. */
	error = gpu_display_timing_format(16U, &timing);
	check(error == 0, "edges: vic 16");

	/* DVI single link: 165.000 MHz fits, 165.001 does not, from an HDMI output to a DVI sink alike. */
	caps_set(&caps, GPU_LINK_DVI, 0U, 0U, 0U);
	timing.pixel_khz = 165000U;
	check(gpu_link_check(&caps, NULL, &timing) == GPU_LINK_FITS, "dvi 165.000");
	timing.pixel_khz = 165001U;
	check(gpu_link_check(&caps, NULL, &timing) == GPU_LINK_DVI_CLOCK, "dvi 165.001");
	caps_set(&caps, GPU_LINK_HDMI, 340000U, 0U, 0U);
	caps.flags = GPU_LINK_DUAL_LINK;
	check(gpu_link_check(&caps, NULL, &timing) == GPU_LINK_DVI_CLOCK, "hdmi to dvi sink 165.001, dual flag ignored");

	/* DVI dual link: 330.000 fits, 330.001 does not. */
	caps_set(&caps, GPU_LINK_DVI, 0U, 0U, 0U);
	caps.flags = GPU_LINK_DUAL_LINK;
	timing.pixel_khz = 330000U;
	check(gpu_link_check(&caps, NULL, &timing) == GPU_LINK_FITS, "dual 330.000");
	timing.pixel_khz = 330001U;
	check(gpu_link_check(&caps, NULL, &timing) == GPU_LINK_DVI_CLOCK, "dual 330.001");

	/* HDMI to an HDMI sink: 340.000 fits, 340.001 does not without scrambling. */
	memset(&test_edid, 0, sizeof(test_edid));
	test_edid.hdmi = 1U;
	caps_set(&caps, GPU_LINK_HDMI, 600000U, 0U, 0U);
	timing.pixel_khz = 340000U;
	check(gpu_link_check(&caps, &test_edid, &timing) == GPU_LINK_FITS, "hdmi 340.000");
	timing.pixel_khz = 340001U;
	check(gpu_link_check(&caps, &test_edid, &timing) == GPU_LINK_TMDS_SOURCE, "hdmi 340.001");

	/* With scrambling the sink's Forum block decides; without it, no more than 340. */
	caps.flags = GPU_LINK_SCRAMBLING;
	check(gpu_link_check(&caps, &test_edid, &timing) == GPU_LINK_TMDS_SINK, "hdmi 340.001 to a sink without a Forum block");
	test_edid.forum_tmds_max_khz = 600000U;
	test_edid.scdc = 1U;
	check(gpu_link_check(&caps, &test_edid, &timing) == GPU_LINK_FITS, "hdmi 340.001 scrambled");
	timing.pixel_khz = 600001U;
	check(gpu_link_check(&caps, &test_edid, &timing) == GPU_LINK_TMDS_SOURCE, "hdmi 600.001");

	/* The sink's HDMI block lower limit (225 MHz). */
	test_edid.hdmi_tmds_max_khz = 225000U;
	timing.pixel_khz = 225000U;
	check(gpu_link_check(&caps, &test_edid, &timing) == GPU_LINK_FITS, "hdmi sink 225.000");
	timing.pixel_khz = 225001U;
	answer = gpu_link_check(&caps, &test_edid, &timing);
	check(answer == GPU_LINK_TMDS_SINK, "hdmi sink 225.001: %u", answer);

	/* DP, 24 bits a pixel: RBR x4 216 MHz, HBR x1 90 MHz, HBR x2 180 MHz, HBR2 x4 720 MHz, HBR3 x4 1080 MHz. */
	caps_set(&caps, GPU_LINK_DP, 0U, 4U, 162000U);
	timing.pixel_khz = 216000U;
	check(gpu_link_check(&caps, NULL, &timing) == GPU_LINK_FITS, "dp rbr x4 216.000");
	timing.pixel_khz = 216001U;
	check(gpu_link_check(&caps, NULL, &timing) == GPU_LINK_DP_BANDWIDTH, "dp rbr x4 216.001");
	caps_set(&caps, GPU_LINK_DP, 0U, 1U, 270000U);
	timing.pixel_khz = 90000U;
	check(gpu_link_check(&caps, NULL, &timing) == GPU_LINK_FITS, "dp hbr x1 90.000");
	timing.pixel_khz = 90001U;
	check(gpu_link_check(&caps, NULL, &timing) == GPU_LINK_DP_BANDWIDTH, "dp hbr x1 90.001");
	caps_set(&caps, GPU_LINK_DP, 0U, 2U, 270000U);
	timing.pixel_khz = 180000U;
	check(gpu_link_check(&caps, NULL, &timing) == GPU_LINK_FITS, "dp hbr x2 180.000");
	timing.pixel_khz = 180001U;
	check(gpu_link_check(&caps, NULL, &timing) == GPU_LINK_DP_BANDWIDTH, "dp hbr x2 180.001");
	caps_set(&caps, GPU_LINK_DP, 0U, 4U, 540000U);
	timing.pixel_khz = 720000U;
	check(gpu_link_check(&caps, NULL, &timing) == GPU_LINK_FITS, "dp hbr2 x4 720.000");
	timing.pixel_khz = 720001U;
	check(gpu_link_check(&caps, NULL, &timing) == GPU_LINK_DP_BANDWIDTH, "dp hbr2 x4 720.001");
	caps_set(&caps, GPU_LINK_DP, 0U, 4U, 810000U);
	timing.pixel_khz = 1080000U;
	check(gpu_link_check(&caps, NULL, &timing) == GPU_LINK_FITS, "dp hbr3 x4 1080.000");
	timing.pixel_khz = 1080001U;
	check(gpu_link_check(&caps, NULL, &timing) == GPU_LINK_DP_BANDWIDTH, "dp hbr3 x4 1080.001");

	/* No lanes, an unknown kind, a virtual output. */
	caps_set(&caps, GPU_LINK_DP, 0U, 0U, 270000U);
	check(gpu_link_check(&caps, NULL, &timing) == GPU_LINK_NO_LINK, "dp no lanes");
	caps_set(&caps, 99U, 0U, 0U, 0U);
	check(gpu_link_check(&caps, NULL, &timing) == GPU_LINK_NO_LINK, "unknown kind");
	caps_set(&caps, GPU_LINK_VIRTUAL, 0U, 0U, 0U);
	check(gpu_link_check(&caps, NULL, &timing) == GPU_LINK_FITS, "virtual");
}

/* A driver's own check that takes only 1920x1080, as a table-driven controller does. */
static int
test_fits_1080(
	void *context,
	const struct gpu_display_timing *timing)
{
	unsigned *calls;

	/* Counts the call. */
	calls = context;
	(*calls)++;

	/* Only 1920x1080 is in the table. */
	if (timing->h_active == 1920U && timing->v_active == 1080U)
		return 0;
	return 1;
}

/* The controller's limits and the driver's own check. */
static void
test_driver_limits(void)
{
	struct gpu_display_timing timing;
	struct gpu_link_caps caps;
	unsigned calls;
	uint32_t answer;
	int error;

	/* The LCD on a virtual output limited to 1680 pixels wide and 150 MHz. */
	error = gpu_edid_decode(test_lcd_edid, sizeof(test_lcd_edid), &test_edid);
	caps_set(&caps, GPU_LINK_VIRTUAL, 0U, 0U, 0U);
	caps.width_max = 1680U;
	caps.pixel_max_khz = 150000U;
	error |= gpu_modes_build(&test_edid, &caps, NULL, NULL, &test_list);
	check(error == 0 && !list_has(1920U, 1280U, 0U) && !list_has(1920U, 1080U, 0U) && list_has(1680U, 1050U, 0U), "limits: width");
	check(test_list.modes[0].h_active == 1680U, "limits: default 1680x1050 (%u)", test_list.modes[0].h_active);

	/* The check answers name the limit. */
	error = gpu_display_timing_format(16U, &timing);
	answer = gpu_modes_check(&test_edid, &caps, NULL, NULL, &timing);
	check(error == 0 && answer == GPU_MODES_SIZE, "check: size %u", answer);
	caps.width_max = 0U;
	answer = gpu_modes_check(&test_edid, &caps, NULL, NULL, &timing);
	check(answer == GPU_MODES_FITS, "check: 148.5 MHz fits 150 (%u)", answer);
	caps.pixel_max_khz = 148499U;
	answer = gpu_modes_check(&test_edid, &caps, NULL, NULL, &timing);
	check(answer == GPU_MODES_CLOCK, "check: clock %u", answer);
	caps_set(&caps, GPU_LINK_DVI, 0U, 0U, 0U);
	timing.pixel_khz = 200000U;
	answer = gpu_modes_check(&test_edid, &caps, NULL, NULL, &timing);
	check(answer == GPU_MODES_LINK + GPU_LINK_DVI_CLOCK, "check: link %u", answer);
	error = gpu_display_timing_format(32U, &timing);
	timing.pixel_khz = 70000U;
	timing.refresh_mhz = gpu_display_timing_refresh(&timing);
	answer = gpu_modes_check(&test_edid, &caps, NULL, NULL, &timing);
	check(error == 0 && timing.refresh_mhz < 23000U && answer == GPU_MODES_SLOW, "check: slow %u mHz %u", timing.refresh_mhz, answer);

	/* The driver's check: only 1920x1080 remains, and it was asked once per passing timing. */
	calls = 0U;
	caps_set(&caps, GPU_LINK_HDMI, 340000U, 0U, 0U);
	error = gpu_modes_build(&test_edid, &caps, test_fits_1080, &calls, &test_list);
	check(error == 0 && test_list.count == 1U && test_list.modes[0].v_active == 1080U && calls == 11U,
	    "driver check: %u modes, %u calls", test_list.count, calls);

	/* A driver that refuses everything leaves no list. */
	caps.width_max = 100U;
	error = gpu_modes_build(&test_edid, &caps, NULL, NULL, &test_list);
	check(error == ENOENT && test_list.count == 0U, "nothing fits: ENOENT");
}
