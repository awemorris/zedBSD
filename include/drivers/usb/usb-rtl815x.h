/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Realtek USB Ethernet (RTL8156 and RTL8157) vendor-mode wire format
 *
 * The parts of the driver that touch no hardware: telling the chip from its
 * version register, the receive and transmit block layouts, the checksum
 * bits of a descriptor, the link speed of the PHY status register and how
 * a register access is laid out in a control transfer.
 */

#ifndef KERN_DRIVERS_USB_RTL815X_H
#define KERN_DRIVERS_USB_RTL815X_H

#include <stddef.h>
#include <stdint.h>

/*
 * The descriptor sizes and alignments of the two generations of the
 * format.  A receive block is a run of [descriptor][frame and its FCS],
 * each descriptor at a multiple of the receive alignment from the block's
 * start; a transmit block is a run of [descriptor][frame], each descriptor
 * at a multiple of the transmit alignment.
 *
 * Values referenced: FreeBSD 15.0 sys/dev/usb/net/if_urereg.h
 * (struct ure_rxpkt, struct ure_txpkt, URE_RXPKT_ALIGN, URE_TXPKT_ALIGN);
 * Linux 7.3 drivers/net/usb/r8152.c (struct rx_desc_v2, struct
 * tx_desc_v2, the 16-byte alignment of the second generation).
 */
#define DRV_USB_RTL815X_RX_DESC_V1_SIZE		24U
#define DRV_USB_RTL815X_RX_ALIGN_V1		8U
#define DRV_USB_RTL815X_TX_DESC_V1_SIZE		8U
#define DRV_USB_RTL815X_TX_ALIGN_V1		4U
#define DRV_USB_RTL815X_RX_DESC_V2_SIZE		16U
#define DRV_USB_RTL815X_RX_ALIGN_V2		16U
#define DRV_USB_RTL815X_TX_DESC_V2_SIZE		16U
#define DRV_USB_RTL815X_TX_ALIGN_V2		16U

/* The frame check sequence a received frame still carries. */
#define DRV_USB_RTL815X_FCS_SIZE		4U

/*
 * The shortest frame the device hands over (a minimum Ethernet frame with
 * its FCS is longer) and the longest a descriptor can describe.
 */
#define DRV_USB_RTL815X_RX_FRAME_MIN		60U
#define DRV_USB_RTL815X_RX_FRAME_MAX		0x7ffeU

/*
 * The largest transport-header offset a transmit descriptor can carry for
 * a checksum the device completes.
 *
 * Values referenced: FreeBSD 15.0 sys/dev/usb/net/if_urereg.h
 * (URE_L4_OFFSET_MAX); Linux 7.3 drivers/net/usb/r8152.c (TCPHO_MAX,
 * TCPHO_MAX_2).
 */
#define DRV_USB_RTL815X_L4_OFFSET_MAX_V1	0x7ffU
#define DRV_USB_RTL815X_L4_OFFSET_MAX_V2	0x3ffU

/*
 * The PHY status register's link bit and speed bits.
 *
 * Values referenced: FreeBSD 15.0 sys/dev/usb/net/if_urereg.h
 * (URE_PHYSTATUS_*); Linux 7.3 drivers/net/usb/r8152.c (_5000bps).
 */
#define DRV_USB_RTL815X_PHY_STATUS_FULL_DUPLEX	0x0001U
#define DRV_USB_RTL815X_PHY_STATUS_LINK		0x0002U
#define DRV_USB_RTL815X_PHY_STATUS_10		0x0004U
#define DRV_USB_RTL815X_PHY_STATUS_100		0x0008U
#define DRV_USB_RTL815X_PHY_STATUS_1000		0x0010U
#define DRV_USB_RTL815X_PHY_STATUS_2500		0x0400U
#define DRV_USB_RTL815X_PHY_STATUS_5000		0x1000U

/* The checksums the device found right on a received frame. */
#define DRV_USB_RTL815X_RX_CSUM_IPV4		0x1U
#define DRV_USB_RTL815X_RX_CSUM_L4		0x2U

/* The checksums the device is asked to complete on a transmitted frame. */
#define DRV_USB_RTL815X_TX_CSUM_TCP		0x1U
#define DRV_USB_RTL815X_TX_CSUM_IPV4		0x2U

/*
 * The register spaces a control transfer names in its index, and the
 * vendor request that reads or writes them.
 *
 * Values referenced: FreeBSD 15.0 sys/dev/usb/net/if_urereg.h
 * (URE_MCU_TYPE_PLA, URE_MCU_TYPE_USB, URE_BYTE_EN_*) and if_ure.c
 * (ure_ctl: request 0x05, vendor device request types).
 */
#define DRV_USB_RTL815X_SPACE_PLA		0x0100U
#define DRV_USB_RTL815X_SPACE_USB		0x0000U
#define DRV_USB_RTL815X_REQUEST			0x05U
#define DRV_USB_RTL815X_BYTE_ENABLE_DWORD	0x00ffU
#define DRV_USB_RTL815X_BYTE_ENABLE_WORD	0x0033U
#define DRV_USB_RTL815X_BYTE_ENABLE_BYTE	0x0011U
#define DRV_USB_RTL815X_BYTE_ENABLE_SIX		0x003fU

/* The chips the driver tells apart. */
enum drv_usb_rtl815x_chip {
	DRV_USB_RTL815X_CHIP_UNKNOWN,
	DRV_USB_RTL815X_CHIP_OLDER,
	DRV_USB_RTL815X_CHIP_RTL8156,
	DRV_USB_RTL815X_CHIP_RTL8156B,
	DRV_USB_RTL815X_CHIP_RTL8157,
	DRV_USB_RTL815X_CHIP_RTL8159
};

/*
 * The wire format one chip uses: the generation of its descriptors and
 * the sizes and alignments that follow from it.
 */
struct drv_usb_rtl815x_format {
	unsigned generation;
	size_t rx_desc_size;
	size_t rx_align;
	size_t tx_desc_size;
	size_t tx_align;
	uint32_t l4_offset_max;
};

/*
 * One frame for a transmit block: its bytes, and the checksums the device
 * is asked to complete (DRV_USB_RTL815X_TX_CSUM_*) with the offset of the
 * transport header from the frame's start.
 */
struct drv_usb_rtl815x_tx_frame {
	const void *data;
	size_t length;
	unsigned checksums;
	uint32_t l4_offset;
};

/*
 * One register access as a control transfer carries it: the 4-byte
 * aligned address, the byte enable that selects the bytes written, and the
 * shift of the value within the dword.
 */
struct drv_usb_rtl815x_access {
	uint16_t address;
	uint16_t byte_enable;
	unsigned shift;
};

/*
 * Called for each frame of a receive block, with the frame without its
 * FCS and the checksums the device found right.  A nonzero return stops
 * the parse and is passed back.
 */
typedef int (*drv_usb_rtl815x_frame_fn)(
	const void *frame,
	size_t length,
	unsigned checksums,
	void *argument);

enum drv_usb_rtl815x_chip
drv_usb_rtl815x_chip_from_version(
	uint16_t version);

const char *
drv_usb_rtl815x_chip_name(
	enum drv_usb_rtl815x_chip chip);

int
drv_usb_rtl815x_format_for(
	enum drv_usb_rtl815x_chip chip,
	struct drv_usb_rtl815x_format *format);

int
drv_usb_rtl815x_parse_rx(
	const struct drv_usb_rtl815x_format *format,
	const uint8_t *block,
	size_t length,
	drv_usb_rtl815x_frame_fn deliver,
	void *argument,
	size_t *count);

int
drv_usb_rtl815x_build_tx(
	const struct drv_usb_rtl815x_format *format,
	const struct drv_usb_rtl815x_tx_frame *frames,
	size_t count,
	uint8_t *block,
	size_t capacity,
	size_t *length,
	size_t *taken);

size_t
drv_usb_rtl815x_tx_space(
	const struct drv_usb_rtl815x_format *format,
	size_t frame_length);

unsigned
drv_usb_rtl815x_link_mbps(
	uint16_t phy_status);

int
drv_usb_rtl815x_access_for(
	uint16_t reg,
	unsigned width,
	struct drv_usb_rtl815x_access *access);

/* Registers the vendor-mode driver with the USB subsystem. */
int
drv_usb_rtl815x_driver_register(
	void);

#endif
