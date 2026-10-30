/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host test of the Realtek USB Ethernet vendor-mode wire format
 * (src/drivers/usb/usb-rtl815x-frame.c).
 */

#include <drivers/usb/usb-rtl815x.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK_SIZE	65536U
#define FRAMES_MAX	64U

/* One frame the parse delivered, as the test records it. */
struct seen_frame {
	size_t offset;
	size_t length;
	unsigned checksums;
};

/* What a parse delivered, and when to stop it with an error. */
struct seen {
	const uint8_t *block;
	struct seen_frame frames[FRAMES_MAX];
	size_t count;
	size_t fail_at;
};

/* The block the tests build and parse. */
static uint8_t block[BLOCK_SIZE];

/* The number of checks that failed. */
static unsigned failures;

static void check(int condition, const char *what);
static void put_le32(uint8_t *bytes, uint32_t value);
static uint32_t get_le32(const uint8_t *bytes);
static int record(const void *frame, size_t length, unsigned checksums, void *argument);
static size_t rx_append(const struct drv_usb_rtl815x_format *format, size_t offset, size_t frame_length, uint32_t word2, uint32_t word3, uint8_t fill);
static void test_versions(void);
static void test_rx_v1(void);
static void test_rx_v1_corrupt(void);
static void test_rx_v2(void);
static void test_tx(unsigned generation);
static void test_link_and_access(void);

/*
 * Runs every test and reports PASS or the number of failures.
 */
int
main(void)
{
	/* Runs the tests. */
	test_versions();
	test_rx_v1();
	test_rx_v1_corrupt();
	test_rx_v2();
	test_tx(1);
	test_tx(2);
	test_link_and_access();

	/* Reports the outcome. */
	if (failures != 0) {
		printf("rtl815x-frame: FAIL (%u)\n", failures);
		return 1;
	}

	/* Succeeded. */
	printf("rtl815x-frame: PASS\n");
	return 0;
}

/* Counts and names a failed check. */
static void
check(
	int condition,
	const char *what)
{
	/* A check that held says nothing. */
	if (condition)
		return;

	/* Counts the failed check, and names it. */
	failures++;
	printf("rtl815x-frame: check failed: %s\n", what);
}

/* Writes a 32-bit field, least significant byte first. */
static void
put_le32(
	uint8_t *bytes,
	uint32_t value)
{
	/* Spreads the value over four bytes. */
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

/* Reads a 32-bit field, least significant byte first. */
static uint32_t
get_le32(
	const uint8_t *bytes)
{
	/* Assembles the four bytes. */
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	       ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/* Records one delivered frame, or stops the parse at the asked frame. */
static int
record(
	const void *frame,
	size_t length,
	unsigned checksums,
	void *argument)
{
	struct seen *seen = argument;

	/* Stops the parse where the test asked. */
	if (seen->count == seen->fail_at)
		return EIO;

	/* Keeps where the frame was, how long and its checksums. */
	if (seen->count < FRAMES_MAX) {
		seen->frames[seen->count].offset = (size_t)((const uint8_t *)frame - seen->block);
		seen->frames[seen->count].length = length;
		seen->frames[seen->count].checksums = checksums;
	}

	/* Counts the frame, whether or not there was room to keep it. */
	seen->count++;

	/* Succeeded. */
	return 0;
}

/*
 * Appends a receive descriptor and a frame of frame_length bytes (FCS
 * included) at offset and returns the next aligned offset.
 */
static size_t
rx_append(
	const struct drv_usb_rtl815x_format *format,
	size_t offset,
	size_t frame_length,
	uint32_t word2,
	uint32_t word3,
	uint8_t fill)
{
	size_t next;

	/* The descriptor. */
	memset(block + offset, 0, format->rx_desc_size);
	if (format->generation == 2) {
		put_le32(block + offset, (uint32_t)frame_length << 17);
		put_le32(block + offset + 8, word3);
	} else {
		put_le32(block + offset, (uint32_t)frame_length);
		put_le32(block + offset + 4, word2);
		put_le32(block + offset + 8, word3);
	}

	/* The frame bytes. */
	memset(block + offset + format->rx_desc_size, fill, frame_length);

	/* The next descriptor's aligned offset. */
	next = offset + format->rx_desc_size + frame_length;
	next = (next + format->rx_align - 1U) & ~(format->rx_align - 1U);
	return next;
}

/* Checks the version table and the formats. */
static void
test_versions(void)
{
	struct drv_usb_rtl815x_format format;

	/* The chips by version. */
	check(drv_usb_rtl815x_chip_from_version(0x7020) == DRV_USB_RTL815X_CHIP_RTL8156, "0x7020 is RTL8156");
	check(drv_usb_rtl815x_chip_from_version(0x7030) == DRV_USB_RTL815X_CHIP_RTL8156, "0x7030 is RTL8156");
	check(drv_usb_rtl815x_chip_from_version(0x7400) == DRV_USB_RTL815X_CHIP_RTL8156B, "0x7400 is RTL8156B");
	check(drv_usb_rtl815x_chip_from_version(0x7410) == DRV_USB_RTL815X_CHIP_RTL8156B, "0x7410 is RTL8156B");
	check(drv_usb_rtl815x_chip_from_version(0x7420) == DRV_USB_RTL815X_CHIP_RTL8156B, "0x7420 is RTL8156B");
	check(drv_usb_rtl815x_chip_from_version(0x1030) == DRV_USB_RTL815X_CHIP_RTL8157, "0x1030 is RTL8157");
	check(drv_usb_rtl815x_chip_from_version(0x2020) == DRV_USB_RTL815X_CHIP_RTL8159, "0x2020 is RTL8159");
	check(drv_usb_rtl815x_chip_from_version(0x5c30) == DRV_USB_RTL815X_CHIP_OLDER, "0x5c30 is older");
	check(drv_usb_rtl815x_chip_from_version(0x7010) == DRV_USB_RTL815X_CHIP_UNKNOWN, "0x7010 is unknown");

	/* The formats: the RTL8156 family, the RTL8157, and refused chips. */
	check(drv_usb_rtl815x_format_for(DRV_USB_RTL815X_CHIP_RTL8156, &format) == 0 &&
	      format.generation == 1 && format.rx_desc_size == 24 && format.rx_align == 8 &&
	      format.tx_desc_size == 8 && format.tx_align == 4, "RTL8156 format");
	check(drv_usb_rtl815x_format_for(DRV_USB_RTL815X_CHIP_RTL8157, &format) == 0 &&
	      format.generation == 2 && format.rx_desc_size == 16 && format.rx_align == 16 &&
	      format.tx_desc_size == 16 && format.tx_align == 16, "RTL8157 format");
	check(drv_usb_rtl815x_format_for(DRV_USB_RTL815X_CHIP_RTL8159, &format) == ENODEV, "RTL8159 refused");
	check(drv_usb_rtl815x_format_for(DRV_USB_RTL815X_CHIP_OLDER, &format) == ENODEV, "older refused");
}

/* Checks a first-generation receive block of several frames. */
static void
test_rx_v1(void)
{
	struct drv_usb_rtl815x_format format;
	static const size_t lengths[] = { 64, 1518, 61, 9018, 1000, 70 };
	static const uint32_t status_bits[] = {
		(1U << 19) | (1U << 22),
		(1U << 19) | (1U << 22),
		(1U << 20) | (1U << 23),
		(1U << 19),
		0,
		0
	};
	static const uint32_t status_bits_2[] = { 0, (1U << 21), 0, 0, 0, 0 };
	size_t offsets[6];
	struct seen seen;
	size_t offset;
	size_t count;
	size_t index;
	int error;

	/* Builds a block of frames of uneven lengths, each with checksum bits. */
	(void)drv_usb_rtl815x_format_for(DRV_USB_RTL815X_CHIP_RTL8156, &format);

	/* Appends each frame, remembering where it starts. */
	offset = 0;
	for (index = 0; index < 6; index++) {
		offsets[index] = offset;
		offset = rx_append(&format, offset, lengths[index],
				   status_bits[index],
				   status_bits_2[index],
				   (uint8_t)(index + 1));
	}

	/* The block ends with the last frame, its padding not included. */
	offset = offsets[5] + format.rx_desc_size + lengths[5];

	/* Parses it. */
	memset(&seen, 0, sizeof(seen));
	seen.block = block;
	seen.fail_at = (size_t)-1;
	error = drv_usb_rtl815x_parse_rx(&format, block, offset, record, &seen, &count);
	check(error == 0 && count == 6 && seen.count == 6, "v1 block of six frames");

	/* Each frame is where it was put, without its FCS. */
	for (index = 0; index < 6 && index < seen.count; index++) {
		check(seen.frames[index].offset == offsets[index] + 24, "v1 frame offset");
		check(seen.frames[index].length == lengths[index] - 4, "v1 frame length without FCS");
		check(block[seen.frames[index].offset] == (uint8_t)(index + 1), "v1 frame bytes");
	}

	/* The checksum bits: IPv4+TCP right, TCP failed, IPv6+UDP right, IPv4 only, none. */
	check(seen.frames[0].checksums == (DRV_USB_RTL815X_RX_CSUM_IPV4 | DRV_USB_RTL815X_RX_CSUM_L4), "v1 ipv4 tcp");
	check(seen.frames[1].checksums == DRV_USB_RTL815X_RX_CSUM_IPV4, "v1 tcp failed");
	check(seen.frames[2].checksums == DRV_USB_RTL815X_RX_CSUM_L4, "v1 ipv6 udp");
	check(seen.frames[3].checksums == DRV_USB_RTL815X_RX_CSUM_IPV4, "v1 ipv4 only");
	check(seen.frames[4].checksums == 0, "v1 no checksum");

	/* Trailing padding shorter than a descriptor is ignored. */
	memset(block + offset, 0, 7);
	memset(&seen, 0, sizeof(seen));
	seen.block = block;
	seen.fail_at = (size_t)-1;
	error = drv_usb_rtl815x_parse_rx(&format, block, offset + 7, record, &seen, &count);
	check(error == 0 && count == 6, "v1 trailing padding");

	/* A deliver that fails stops the parse with its error. */
	memset(&seen, 0, sizeof(seen));
	seen.block = block;
	seen.fail_at = 2;
	error = drv_usb_rtl815x_parse_rx(&format, block, offset, record, &seen, &count);
	check(error == EIO && count == 2, "v1 deliver error stops");

	/* An empty block has no frame. */
	error = drv_usb_rtl815x_parse_rx(&format, block, 0, record, &seen, &count);
	check(error == 0 && count == 0, "v1 empty block");
}

/* Checks that a descriptor that cannot be trusted ends the parse. */
static void
test_rx_v1_corrupt(void)
{
	struct drv_usb_rtl815x_format format;
	struct seen seen;
	size_t offset;
	size_t second;
	size_t count;
	int error;

	/* The first-generation layout. */
	(void)drv_usb_rtl815x_format_for(DRV_USB_RTL815X_CHIP_RTL8156, &format);

	/* A frame too short after a good one. */
	second = rx_append(&format, 0, 100, 0, 0, 0x11);
	offset = rx_append(&format, second, 59, 0, 0, 0x22);
	offset = rx_append(&format, offset, 100, 0, 0, 0x33);
	memset(&seen, 0, sizeof(seen));
	seen.block = block;
	seen.fail_at = (size_t)-1;
	error = drv_usb_rtl815x_parse_rx(&format, block, offset, record, &seen, &count);
	check(error == EINVAL && count == 1, "v1 short frame stops the block");

	/* A length at the mask, which no frame has. */
	(void)rx_append(&format, 0, 100, 0, 0, 0x11);
	put_le32(block, 0x7fffU);
	error = drv_usb_rtl815x_parse_rx(&format, block, BLOCK_SIZE, record, &seen, &count);
	check(error == EINVAL && count == 0, "v1 length at the mask");

	/* A frame reaching past the block. */
	offset = rx_append(&format, 0, 1000, 0, 0, 0x11);
	error = drv_usb_rtl815x_parse_rx(&format, block, offset - 1, record, &seen, &count);
	check(error == EINVAL && count == 0, "v1 frame past the block");

	/* A frame that ends exactly at the block's end is whole. */
	offset = rx_append(&format, 0, 1001, 0, 0, 0x11);
	memset(&seen, 0, sizeof(seen));
	seen.block = block;
	seen.fail_at = (size_t)-1;
	error = drv_usb_rtl815x_parse_rx(&format, block, 24 + 1001, record, &seen, &count);
	check(error == 0 && count == 1 && seen.frames[0].length == 997, "v1 frame to the end");
	(void)offset;
}

/* Checks a second-generation receive block. */
static void
test_rx_v2(void)
{
	struct drv_usb_rtl815x_format format;
	struct seen seen;
	size_t offset;
	size_t count;
	int error;

	/* Builds three frames with second-generation checksum bits. */
	(void)drv_usb_rtl815x_format_for(DRV_USB_RTL815X_CHIP_RTL8157, &format);
	offset = rx_append(&format, 0, 1518, 0, (1U << 14) | (1U << 10), 0x41);
	offset = rx_append(&format, offset, 65, 0, (1U << 15) | (1U << 11) | (1U << 25), 0x42);
	offset = rx_append(&format, offset, 9018, 0, (1U << 14) | (1U << 26), 0x43);
	check(offset % 16 == 0, "v2 alignment");

	/* Parses it. */
	memset(&seen, 0, sizeof(seen));
	seen.block = block;
	seen.fail_at = (size_t)-1;
	error = drv_usb_rtl815x_parse_rx(&format, block, offset, record, &seen, &count);
	check(error == 0 && count == 3, "v2 block of three frames");
	check(seen.frames[0].offset == 16 && seen.frames[0].length == 1514, "v2 first frame");
	check(seen.frames[1].offset == ((16 + 1518 + 15) & ~15U) + 16 && seen.frames[1].length == 61, "v2 second frame");
	check(seen.frames[0].checksums == (DRV_USB_RTL815X_RX_CSUM_IPV4 | DRV_USB_RTL815X_RX_CSUM_L4), "v2 ipv4 tcp");
	check(seen.frames[1].checksums == 0, "v2 ipv6 udp failed");
	check(seen.frames[2].checksums == 0, "v2 ip failed");
}

/* Checks the transmit block of one generation. */
static void
test_tx(
	unsigned generation)
{
	struct drv_usb_rtl815x_format format;
	struct drv_usb_rtl815x_tx_frame frames[8];
	enum drv_usb_rtl815x_chip chip;
	static uint8_t payload[9100];
	size_t length;
	size_t taken;
	size_t offset;
	size_t index;
	uint32_t word;
	int error;

	/* Prepares frames of uneven lengths, the second asking for checksums. */
	chip = DRV_USB_RTL815X_CHIP_RTL8156;
	if (generation == 2)
		chip = DRV_USB_RTL815X_CHIP_RTL8157;
	(void)drv_usb_rtl815x_format_for(chip, &format);
	memset(payload, 0x5a, sizeof(payload));
	memset(frames, 0, sizeof(frames));
	for (index = 0; index < 8; index++) {
		frames[index].data = payload;
		frames[index].length = 60 + index * 333;
	}

	/* Asks for the TCP and IPv4 checksums on the second frame. */
	frames[1].checksums = DRV_USB_RTL815X_TX_CSUM_TCP | DRV_USB_RTL815X_TX_CSUM_IPV4;
	frames[1].l4_offset = 34;

	/* Builds a block of all eight. */
	error = drv_usb_rtl815x_build_tx(&format, frames, 8, block, 16384, &length, &taken);
	check(error == 0 && taken == 8, "tx eight frames");

	/* Walks the descriptors: each aligned, each with its length and marks. */
	offset = 0;
	for (index = 0; index < taken; index++) {
		check(offset % format.tx_align == 0, "tx descriptor alignment");
		word = get_le32(block + offset);
		check((word & 0x3ffffU) == frames[index].length, "tx length");
		check((word & 0xc0000000U) == 0xc0000000U, "tx first and last segment");
		word = get_le32(block + offset + 4);
		if (index == 1)
			check(word == ((1U << 30) | (1U << 29) | (34U << 17)), "tx checksum request");
		else
			check(word == 0, "tx no checksum request");
		if (generation == 2) {
			check(get_le32(block + offset + 8) == (uint32_t)frames[index].length << 4, "tx v2 length");
			check(get_le32(block + offset + 12) == (0x15U << 27), "tx v2 signature");
		}

		/* The frame bytes follow the descriptor. */
		check(block[offset + format.tx_desc_size] == 0x5a, "tx frame bytes");
		offset += format.tx_desc_size + frames[index].length;
		if (index + 1 < taken)
			offset = (offset + format.tx_align - 1U) & ~(format.tx_align - 1U);
	}

	/* The block ends with the last frame. */
	check(length == offset, "tx block length");

	/* A small block takes the frames that fit and leaves the rest. */
	error = drv_usb_rtl815x_build_tx(&format, frames, 8, block, 1500, &length, &taken);
	check(error == 0 && taken >= 1 && taken < 8 && length <= 1500, "tx partial block");

	/* A first frame larger than the block is refused. */
	frames[0].length = 9014;
	error = drv_usb_rtl815x_build_tx(&format, frames, 8, block, 4096, &length, &taken);
	check(error == EMSGSIZE && taken == 0, "tx frame larger than the block");

	/* A jumbo frame fits a 16 KB block. */
	error = drv_usb_rtl815x_build_tx(&format, frames, 1, block, 16384, &length, &taken);
	check(error == 0 && taken == 1 && length == format.tx_desc_size + 9014, "tx jumbo frame");

	/* A transport offset beyond what the descriptor carries is refused. */
	frames[0].length = 3000;
	frames[0].checksums = DRV_USB_RTL815X_TX_CSUM_TCP;
	frames[0].l4_offset = format.l4_offset_max + 1U;
	error = drv_usb_rtl815x_build_tx(&format, frames, 1, block, 16384, &length, &taken);
	check(error == EINVAL && taken == 0, "tx transport offset too large");

	/* The space one frame takes. */
	check(drv_usb_rtl815x_tx_space(&format, 1514) ==
	      ((format.tx_desc_size + 1514 + format.tx_align - 1U) & ~(format.tx_align - 1U)), "tx space");
}

/* Checks the link speed and the register access layout. */
static void
test_link_and_access(void)
{
	struct drv_usb_rtl815x_access access;

	/* The link speed. */
	check(drv_usb_rtl815x_link_mbps(0x0000) == 0, "no link");
	check(drv_usb_rtl815x_link_mbps(0x0400) == 0, "speed without link");
	check(drv_usb_rtl815x_link_mbps(0x0403) == 2500, "2500");
	check(drv_usb_rtl815x_link_mbps(0x1003) == 5000, "5000");
	check(drv_usb_rtl815x_link_mbps(0x0013) == 1000, "1000");
	check(drv_usb_rtl815x_link_mbps(0x000b) == 100, "100");
	check(drv_usb_rtl815x_link_mbps(0x0007) == 10, "10");

	/* A byte at each lane, a word at each half, a dword. */
	check(drv_usb_rtl815x_access_for(0xe813, 1, &access) == 0 && access.address == 0xe810 &&
	      access.shift == 24 && access.byte_enable == 0x88, "byte lane 3");
	check(drv_usb_rtl815x_access_for(0xe810, 1, &access) == 0 && access.shift == 0 &&
	      access.byte_enable == 0x11, "byte lane 0");
	check(drv_usb_rtl815x_access_for(0xe612, 2, &access) == 0 && access.address == 0xe610 &&
	      access.shift == 16 && access.byte_enable == 0xcc, "word high half");
	check(drv_usb_rtl815x_access_for(0xd40c, 4, &access) == 0 && access.address == 0xd40c &&
	      access.shift == 0 && access.byte_enable == 0xff, "dword");
	check(drv_usb_rtl815x_access_for(0xd40d, 2, &access) == EINVAL, "misaligned word");
	check(drv_usb_rtl815x_access_for(0xd40e, 4, &access) == EINVAL, "misaligned dword");
	check(drv_usb_rtl815x_access_for(0xd40c, 3, &access) == EINVAL, "width 3");
}
