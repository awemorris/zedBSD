/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Realtek USB Ethernet vendor-mode wire format
 */

#include <drivers/usb/usb-rtl815x.h>
#include <uapi/errno.h>
#include <kern/kcrt.h>

/*
 * The fields of the first-generation receive descriptor: the frame length
 * in the first word, the kind of checksum the device checked in the second
 * and the checks that failed in the third.
 *
 * Values referenced: FreeBSD 15.0 sys/dev/usb/net/if_urereg.h
 * (URE_RXPKT_LEN_MASK, URE_RXPKT_IPV4_CS, URE_RXPKT_IPV6_CS,
 * URE_RXPKT_TCP_CS, URE_RXPKT_UDP_CS, URE_RXPKT_TCP_F, URE_RXPKT_UDP_F,
 * URE_RXPKT_IP_F).
 */
#define RX_V1_LENGTH_MASK	0x00007fffU
#define RX_V1_KIND_IPV4		(1U << 19)
#define RX_V1_KIND_IPV6		(1U << 20)
#define RX_V1_KIND_TCP		(1U << 22)
#define RX_V1_KIND_UDP		(1U << 23)
#define RX_V1_FAIL_TCP		(1U << 21)
#define RX_V1_FAIL_UDP		(1U << 22)
#define RX_V1_FAIL_IP		(1U << 23)

/*
 * The fields of the second-generation receive descriptor: the frame
 * length in the top bits of the first word, and the kinds and failed
 * checks together in the third.
 *
 * Values referenced: Linux 7.3 drivers/net/usb/r8152.c (struct
 * rx_desc_v2: RX_LEN_MASK_2, IPF_2, UDPF_2, TCPF_2, RD_IPV6_CS_2,
 * RD_IPV4_CS_2, RD_UDP_CS_2, RD_TCP_CS_2).
 */
#define RX_V2_LENGTH_SHIFT	17U
#define RX_V2_LENGTH_MASK	0x7fffU
#define RX_V2_KIND_IPV4		(1U << 14)
#define RX_V2_KIND_IPV6		(1U << 15)
#define RX_V2_KIND_TCP		(1U << 10)
#define RX_V2_KIND_UDP		(1U << 11)
#define RX_V2_FAIL_TCP		(1U << 24)
#define RX_V2_FAIL_UDP		(1U << 25)
#define RX_V2_FAIL_IP		(1U << 26)

/*
 * The fields of a transmit descriptor: the first and last segment marks
 * and the length in the first word, the checksum requests and the
 * transport offset in the second; the second generation repeats the
 * length in the third word and carries a signature in the fourth.
 *
 * Values referenced: FreeBSD 15.0 sys/dev/usb/net/if_urereg.h
 * (URE_TKPKT_TX_FS, URE_TKPKT_TX_LS, URE_TXPKT_IPV4_CS, URE_TXPKT_TCP_CS,
 * URE_L4_OFFSET_SHIFT); Linux 7.3 drivers/net/usb/r8152.c (TX_LEN_MAX,
 * tx_v2_set_len, TX_SIG).
 */
#define TX_FIRST_SEGMENT	(1U << 31)
#define TX_LAST_SEGMENT		(1U << 30)
#define TX_LENGTH_MAX		0x3ffffU
#define TX_CSUM_IPV4		(1U << 29)
#define TX_CSUM_TCP		(1U << 30)
#define TX_L4_OFFSET_SHIFT	17U
#define TX_V2_LENGTH_SHIFT	4U
#define TX_V2_SIGNATURE		(0x15U << 27)

static uint32_t load_le32(const uint8_t *bytes);
static void store_le32(uint8_t *bytes, uint32_t value);
static size_t align_up(size_t value, size_t alignment);
static unsigned rx_checksums_v1(const uint8_t *descriptor);
static unsigned rx_checksums_v2(const uint8_t *descriptor);
static size_t rx_frame_length(const struct drv_usb_rtl815x_format *format, const uint8_t *descriptor);
static int tx_descriptor_fill(const struct drv_usb_rtl815x_format *format, const struct drv_usb_rtl815x_tx_frame *frame, uint8_t *descriptor);

/*
 * Tells the chip from the value of its version register.
 *
 * The version is the upper half of the PLA register at 0xe610 (0xe612)
 * masked with 0x7cf0.
 */
enum drv_usb_rtl815x_chip
drv_usb_rtl815x_chip_from_version(
	uint16_t version)
{
	/*
	 * Values referenced: FreeBSD 15.0 sys/dev/usb/net/if_ure.c
	 * (ure_read_chipver); Linux 7.3 drivers/net/usb/r8152.c
	 * (__rtl_get_hw_ver: 0x6400, 0x7420, 0x1030, 0x2020).
	 */
	switch (version) {
	case 0x7020U:
	case 0x7030U:
		/* The first RTL8156. */
		return DRV_USB_RTL815X_CHIP_RTL8156;
	case 0x7400U:
	case 0x7410U:
	case 0x7420U:
		/* The RTL8156B and its later revisions. */
		return DRV_USB_RTL815X_CHIP_RTL8156B;
	case 0x1030U:
		/* The 5 Gbit/s RTL8157. */
		return DRV_USB_RTL815X_CHIP_RTL8157;
	case 0x2020U:
		/* The 10 Gbit/s RTL8159. */
		return DRV_USB_RTL815X_CHIP_RTL8159;
	case 0x4800U:
	case 0x4c00U:
	case 0x4c10U:
	case 0x5c00U:
	case 0x5c10U:
	case 0x5c20U:
	case 0x5c30U:
	case 0x6000U:
	case 0x6010U:
	case 0x6400U:
		/* The 100 Mbit/s and 1 Gbit/s RTL8152 and RTL8153 family. */
		return DRV_USB_RTL815X_CHIP_OLDER;
	default:
		break;
	}

	/* A version no table here names. */
	return DRV_USB_RTL815X_CHIP_UNKNOWN;
}

/*
 * Names a chip for the log.
 */
const char *
drv_usb_rtl815x_chip_name(
	enum drv_usb_rtl815x_chip chip)
{
	/* Picks the name of each chip the driver tells apart. */
	switch (chip) {
	case DRV_USB_RTL815X_CHIP_OLDER:
		return "RTL8152/RTL8153";
	case DRV_USB_RTL815X_CHIP_RTL8156:
		return "RTL8156";
	case DRV_USB_RTL815X_CHIP_RTL8156B:
		return "RTL8156B";
	case DRV_USB_RTL815X_CHIP_RTL8157:
		return "RTL8157";
	case DRV_USB_RTL815X_CHIP_RTL8159:
		return "RTL8159";
	default:
		break;
	}

	/* A chip the driver does not know. */
	return "unknown";
}

/*
 * Fills the wire format a chip uses.
 *
 * Returns ENODEV for a chip the driver does not drive in vendor mode.
 */
int
drv_usb_rtl815x_format_for(
	enum drv_usb_rtl815x_chip chip,
	struct drv_usb_rtl815x_format *format)
{
	/* Refuses a missing result. */
	if (format == NULL)
		return EINVAL;

	/* The RTL8156 family keeps the first-generation descriptors. */
	if (chip == DRV_USB_RTL815X_CHIP_RTL8156 ||
	    chip == DRV_USB_RTL815X_CHIP_RTL8156B) {
		format->generation = 1;
		format->rx_desc_size = DRV_USB_RTL815X_RX_DESC_V1_SIZE;
		format->rx_align = DRV_USB_RTL815X_RX_ALIGN_V1;
		format->tx_desc_size = DRV_USB_RTL815X_TX_DESC_V1_SIZE;
		format->tx_align = DRV_USB_RTL815X_TX_ALIGN_V1;
		format->l4_offset_max = DRV_USB_RTL815X_L4_OFFSET_MAX_V1;

		/* Succeeded: the RTL8156's format. */
		return 0;
	}

	/* The RTL8157 has the 16-byte second-generation descriptors. */
	if (chip == DRV_USB_RTL815X_CHIP_RTL8157) {
		format->generation = 2;
		format->rx_desc_size = DRV_USB_RTL815X_RX_DESC_V2_SIZE;
		format->rx_align = DRV_USB_RTL815X_RX_ALIGN_V2;
		format->tx_desc_size = DRV_USB_RTL815X_TX_DESC_V2_SIZE;
		format->tx_align = DRV_USB_RTL815X_TX_ALIGN_V2;
		format->l4_offset_max = DRV_USB_RTL815X_L4_OFFSET_MAX_V2;

		/* Succeeded: the RTL8157's format. */
		return 0;
	}

	/* Any other chip is not driven in vendor mode. */
	return ENODEV;
}

/*
 * Hands each frame of a receive block to deliver, in order.
 *
 * A descriptor that cannot be trusted (a length too short, too long or
 * reaching past the block) ends the parse with EINVAL: where the next
 * frame begins is then unknown, and reading on would let one frame's
 * bytes pose as another's.  The frames before it have been delivered.
 * Fewer bytes left than a descriptor are the block's padding.  count
 * receives the number of frames delivered.
 */
int
drv_usb_rtl815x_parse_rx(
	const struct drv_usb_rtl815x_format *format,
	const uint8_t *block,
	size_t length,
	drv_usb_rtl815x_frame_fn deliver,
	void *argument,
	size_t *count)
{
	const uint8_t *descriptor;
	unsigned checksums;
	size_t frame_length;
	size_t offset;
	size_t delivered;
	int error;

	/* Refuses missing arguments. */
	if (format == NULL || deliver == NULL || count == NULL)
		return EINVAL;
	*count = 0;

	/* Refuses a missing block that claims bytes. */
	if (block == NULL && length != 0)
		return EINVAL;

	/* Takes one descriptor and its frame after another. */
	offset = 0;
	delivered = 0;
	while (length - offset >= format->rx_desc_size) {
		descriptor = block + offset;
		frame_length = rx_frame_length(format, descriptor);

		/* Refuses a length no frame has, which leaves the next descriptor unknown. */
		if (frame_length < DRV_USB_RTL815X_RX_FRAME_MIN ||
		    frame_length > DRV_USB_RTL815X_RX_FRAME_MAX) {
			*count = delivered;
			return EINVAL;
		}

		/* Refuses a frame that reaches past the block. */
		if (frame_length > length - offset - format->rx_desc_size) {
			*count = delivered;
			return EINVAL;
		}

		/* Reads what the device found of the checksums. */
		if (format->generation == 2)
			checksums = rx_checksums_v2(descriptor);
		else
			checksums = rx_checksums_v1(descriptor);

		/* Hands the frame over without its FCS. */
		error = deliver(descriptor + format->rx_desc_size,
				frame_length - DRV_USB_RTL815X_FCS_SIZE,
				checksums,
				argument);
		if (error != 0) {
			*count = delivered;
			return error;
		}

		/* Counts the frame handed over. */
		delivered++;

		/* The next descriptor starts at the next aligned offset. */
		offset = align_up(offset + format->rx_desc_size + frame_length,
				  format->rx_align);
		if (offset >= length)
			break;
	}

	/* Succeeded: every frame of the block was delivered. */
	*count = delivered;
	return 0;
}

/*
 * Builds a transmit block of frames, in order, as many as fit capacity.
 *
 * Each frame goes behind a descriptor at the next aligned offset.  taken
 * receives how many frames the block carries and length its size.  The
 * first frame not fitting an empty block is refused with EMSGSIZE, a
 * checksum request the descriptor cannot carry with EINVAL; in both cases
 * nothing is taken.
 */
int
drv_usb_rtl815x_build_tx(
	const struct drv_usb_rtl815x_format *format,
	const struct drv_usb_rtl815x_tx_frame *frames,
	size_t count,
	uint8_t *block,
	size_t capacity,
	size_t *length,
	size_t *taken)
{
	size_t offset;
	size_t end;
	size_t index;
	int error;

	/* Refuses missing arguments. */
	if (format == NULL || block == NULL || length == NULL || taken == NULL)
		return EINVAL;
	*length = 0;
	*taken = 0;

	/* Refuses frames that are missing. */
	if (frames == NULL && count != 0)
		return EINVAL;

	/* Appends one frame after another while the block has room. */
	offset = 0;
	end = 0;
	for (index = 0; index < count; index++) {
		/* Refuses an empty frame or one no descriptor can describe. */
		if (frames[index].data == NULL ||
		    frames[index].length == 0 ||
		    frames[index].length > TX_LENGTH_MAX)
			return EINVAL;

		/* Stops before a frame the block has no room for. */
		if (format->tx_desc_size > capacity - offset ||
		    frames[index].length > capacity - offset - format->tx_desc_size) {
			if (index == 0)
				return EMSGSIZE;
			break;
		}

		/* Writes the descriptor. */
		error = tx_descriptor_fill(format, &frames[index], block + offset);
		if (error != 0)
			return error;

		/* Copies the frame behind it. */
		kern_memcpy(block + offset + format->tx_desc_size,
			    frames[index].data,
			    frames[index].length);
		end = offset + format->tx_desc_size + frames[index].length;

		/* The next descriptor starts at the next aligned offset. */
		offset = align_up(end, format->tx_align);
		if (offset > capacity)
			offset = capacity;
	}

	/* Succeeded: the block carries the frames it took. */
	*length = end;
	*taken = index;
	return 0;
}

/*
 * Reports the block space one frame takes at most: its descriptor, the
 * frame and the alignment padding before the next descriptor.
 */
size_t
drv_usb_rtl815x_tx_space(
	const struct drv_usb_rtl815x_format *format,
	size_t frame_length)
{
	size_t space;

	/* A descriptor, the frame, and the padding that aligns the next. */
	space = align_up(format->tx_desc_size + frame_length, format->tx_align);

	/* Reports the space. */
	return space;
}

/*
 * Reports the link speed in megabits a second the PHY status register
 * gives, or 0 without a link or with a speed it does not name.
 */
unsigned
drv_usb_rtl815x_link_mbps(
	uint16_t phy_status)
{
	/* No link has no speed. */
	if ((phy_status & DRV_USB_RTL815X_PHY_STATUS_LINK) == 0)
		return 0;

	/* Names the speed from the fastest bit down. */
	if ((phy_status & DRV_USB_RTL815X_PHY_STATUS_5000) != 0)
		return 5000;
	if ((phy_status & DRV_USB_RTL815X_PHY_STATUS_2500) != 0)
		return 2500;
	if ((phy_status & DRV_USB_RTL815X_PHY_STATUS_1000) != 0)
		return 1000;
	if ((phy_status & DRV_USB_RTL815X_PHY_STATUS_100) != 0)
		return 100;
	if ((phy_status & DRV_USB_RTL815X_PHY_STATUS_10) != 0)
		return 10;

	/* A link at a speed no bit names. */
	return 0;
}

/*
 * Lays out an access of width bytes (1, 2 or 4) to register reg: the
 * device reads and writes aligned dwords, so a byte or a word is shifted
 * into place and selected with its byte enable.
 */
int
drv_usb_rtl815x_access_for(
	uint16_t reg,
	unsigned width,
	struct drv_usb_rtl815x_access *access)
{
	/* Refuses a missing result. */
	if (access == NULL)
		return EINVAL;

	/*
	 * Procedure referenced: FreeBSD 15.0 sys/dev/usb/net/if_ure.c
	 * (ure_read_1, ure_read_2, ure_write_1, ure_write_2, ure_write_4).
	 */
	access->address = (uint16_t)(reg & ~3U);
	access->shift = (unsigned)(reg & 3U) * 8U;

	/* Picks the byte enable of the width, refusing a misaligned word or dword. */
	switch (width) {
	case 1:
		access->byte_enable = (uint16_t)(DRV_USB_RTL815X_BYTE_ENABLE_BYTE << (reg & 3U));
		break;
	case 2:
		if ((reg & 1U) != 0)
			return EINVAL;
		access->byte_enable = (uint16_t)(DRV_USB_RTL815X_BYTE_ENABLE_WORD << (reg & 2U));
		break;
	case 4:
		if ((reg & 3U) != 0)
			return EINVAL;
		access->byte_enable = DRV_USB_RTL815X_BYTE_ENABLE_DWORD;
		break;
	default:
		return EINVAL;
	}

	/* Succeeded: the access is laid out. */
	return 0;
}

/* Reads a 32-bit field, least significant byte first. */
static uint32_t
load_le32(
	const uint8_t *bytes)
{
	uint32_t value;

	/* Assembles the four bytes. */
	value = (uint32_t)bytes[0];
	value |= (uint32_t)bytes[1] << 8;
	value |= (uint32_t)bytes[2] << 16;
	value |= (uint32_t)bytes[3] << 24;

	/* Reports the field. */
	return value;
}

/* Writes a 32-bit field, least significant byte first. */
static void
store_le32(
	uint8_t *bytes,
	uint32_t value)
{
	/* Spreads the value over four bytes. */
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

/* Rounds an offset up to a multiple of a power-of-two alignment. */
static size_t
align_up(
	size_t value,
	size_t alignment)
{
	/* Reports the rounded offset. */
	return (value + alignment - 1U) & ~(alignment - 1U);
}

/* Reads which checksums a first-generation descriptor found right. */
static unsigned
rx_checksums_v1(
	const uint8_t *descriptor)
{
	uint32_t kinds;
	uint32_t failed;
	unsigned checksums;

	/*
	 * Procedure referenced: FreeBSD 15.0 sys/dev/usb/net/if_ure.c
	 * (ure_rxcsum): the second word says what was checked, the third
	 * what failed.
	 */
	kinds = load_le32(descriptor + 4);
	failed = load_le32(descriptor + 8);
	checksums = 0;

	/* An IPv4 header checked and not failed, and its transport checksum. */
	if ((kinds & RX_V1_KIND_IPV4) != 0) {
		if ((failed & RX_V1_FAIL_IP) != 0)
			return 0;
		checksums |= DRV_USB_RTL815X_RX_CSUM_IPV4;
	} else if ((kinds & RX_V1_KIND_IPV6) == 0) {
		/* Neither IPv4 nor IPv6: nothing was checked. */
		return 0;
	}

	/* A TCP or UDP checksum checked and not failed. */
	if ((kinds & RX_V1_KIND_TCP) != 0 && (failed & RX_V1_FAIL_TCP) == 0)
		checksums |= DRV_USB_RTL815X_RX_CSUM_L4;
	else if ((kinds & RX_V1_KIND_UDP) != 0 && (failed & RX_V1_FAIL_UDP) == 0)
		checksums |= DRV_USB_RTL815X_RX_CSUM_L4;

	/* Reports the checksums found right. */
	return checksums;
}

/* Reads which checksums a second-generation descriptor found right. */
static unsigned
rx_checksums_v2(
	const uint8_t *descriptor)
{
	uint32_t status;
	unsigned checksums;

	/*
	 * Procedure referenced: Linux 7.3 drivers/net/usb/r8152.c
	 * (r8157_rx_csum): the third word has both what was checked and what
	 * failed.
	 */
	status = load_le32(descriptor + 8);
	checksums = 0;

	/* An IPv4 header checked and not failed. */
	if ((status & RX_V2_KIND_IPV4) != 0) {
		if ((status & RX_V2_FAIL_IP) != 0)
			return 0;
		checksums |= DRV_USB_RTL815X_RX_CSUM_IPV4;
	} else if ((status & RX_V2_KIND_IPV6) == 0) {
		/* Neither IPv4 nor IPv6: nothing was checked. */
		return 0;
	}

	/* A TCP or UDP checksum checked and not failed. */
	if ((status & RX_V2_KIND_TCP) != 0 && (status & RX_V2_FAIL_TCP) == 0)
		checksums |= DRV_USB_RTL815X_RX_CSUM_L4;
	else if ((status & RX_V2_KIND_UDP) != 0 && (status & RX_V2_FAIL_UDP) == 0)
		checksums |= DRV_USB_RTL815X_RX_CSUM_L4;

	/* Reports the checksums found right. */
	return checksums;
}

/* Reads the length of the frame (with its FCS) a receive descriptor describes. */
static size_t
rx_frame_length(
	const struct drv_usb_rtl815x_format *format,
	const uint8_t *descriptor)
{
	uint32_t word;

	/* The first word holds the length in both generations, in different bits. */
	word = load_le32(descriptor);
	if (format->generation == 2)
		return (size_t)((word >> RX_V2_LENGTH_SHIFT) & RX_V2_LENGTH_MASK);

	/* The first generation's length is the low bits. */
	return (size_t)(word & RX_V1_LENGTH_MASK);
}

/*
 * Writes the transmit descriptor of one frame, asking the device for the
 * checksums the frame wants completed.
 */
static int
tx_descriptor_fill(
	const struct drv_usb_rtl815x_format *format,
	const struct drv_usb_rtl815x_tx_frame *frame,
	uint8_t *descriptor)
{
	uint32_t command;
	uint32_t checksum;

	/*
	 * Procedure referenced: FreeBSD 15.0 sys/dev/usb/net/if_ure.c
	 * (ure_bulk_write_callback, ure_txcsum); Linux 7.3
	 * drivers/net/usb/r8152.c (r8152_tx_csum, r8157_tx_len).
	 */
	command = (uint32_t)frame->length | TX_FIRST_SEGMENT | TX_LAST_SEGMENT;
	checksum = 0;

	/* A TCP checksum, which needs the transport header's offset. */
	if ((frame->checksums & DRV_USB_RTL815X_TX_CSUM_TCP) != 0) {
		if (frame->l4_offset > format->l4_offset_max ||
		    frame->l4_offset >= frame->length)
			return EINVAL;
		checksum |= TX_CSUM_TCP | TX_CSUM_IPV4;
		checksum |= frame->l4_offset << TX_L4_OFFSET_SHIFT;
	}

	/* The IPv4 header's checksum. */
	if ((frame->checksums & DRV_USB_RTL815X_TX_CSUM_IPV4) != 0)
		checksum |= TX_CSUM_IPV4;

	/* The first two words, common to both generations. */
	store_le32(descriptor, command);
	store_le32(descriptor + 4, checksum);

	/* The second generation repeats the length and signs the descriptor. */
	if (format->generation == 2) {
		store_le32(descriptor + 8, (uint32_t)frame->length << TX_V2_LENGTH_SHIFT);
		store_le32(descriptor + 12, TX_V2_SIGNATURE);
	}

	/* Succeeded: the descriptor is written. */
	return 0;
}
