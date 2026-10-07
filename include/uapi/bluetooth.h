/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Bluetooth controllers' HCI: /dev/btN (ws143-p002, the user's
 * approval D2 of 2026-10-05).
 *
 * The kernel only carries the HCI packets between the controller and one
 * program, the Bluetooth daemon, which runs the host stack (the firmware's
 * load, HCI, L2CAP, SMP, SDP, GATT, HID).  Every packet is in the H4 form:
 * its type's byte first, then the packet as the Bluetooth Core
 * specification lays it out.
 *
 *   open()   one open at a time (a second one answers EBUSY).
 *   write()  one whole command or ACL packet; a packet whose length does
 *            not match its header, an ACL packet longer than the data
 *            limit, or another type answers EINVAL.  It returns when the
 *            controller has taken the packet.
 *   read()   one whole event or ACL packet, or a notice of the kernel's
 *            (type 0x80 and above, one byte long), in the order they came.
 *            A buffer shorter than the packet answers EMSGSIZE and the
 *            packet stays.  It waits for one unless the file is
 *            O_NONBLOCK (EAGAIN).
 *   poll()   POLLIN while a packet waits, POLLOUT while the controller is
 *            there, POLLHUP once it is gone (every call then answers
 *            ENODEV).
 *
 * When the program does not read, the kernel stops taking packets from
 * the controller, which holds them; no packet is dropped.  The request
 * numbers and the structures are zedBSD's own.
 */

#ifndef KERN_UAPI_BLUETOOTH_H
#define KERN_UAPI_BLUETOOTH_H

#include <stdint.h>
#include <uapi/ioctl.h>

#define KERN_BT_IOC_GROUP	'b'

/* The H4 packet types; SCO and ISO are reserved (not carried yet). */
#define BT_PACKET_COMMAND	0x01U
#define BT_PACKET_ACL		0x02U
#define BT_PACKET_SCO		0x03U
#define BT_PACKET_EVENT		0x04U
#define BT_PACKET_ISO		0x05U

/* The kernel's notice that BT_IOC_RESET has reset the controller (no body). */
#define BT_PACKET_NOTICE_RESET	0x80U

/* The longest command written and event read, the type's byte included. */
#define BT_COMMAND_PACKET_MAX	(1U + 3U + 255U)
#define BT_EVENT_PACKET_MAX	(1U + 2U + 255U)

/* The ACL data length limit: before the program sets one, the least and the most it may set. */
#define BT_ACL_DATA_DEFAULT	1021U
#define BT_ACL_DATA_MIN		27U
#define BT_ACL_DATA_MAX		4096U

/* The longest ACL packet, with the type's byte and the header. */
#define BT_ACL_PACKET_MAX	(1U + 4U + BT_ACL_DATA_MAX)

/* The bus a controller is on. */
#define BT_BUS_USB		1U

/* bt_info flags: the Intel bootloader's path is on. */
#define BT_INFO_BOOTLOADER	0x0001U

/* The room of a name and a place, the NUL included. */
#define BT_TEXT_MAX		64U

/*
 * What a controller is: the USB identity (vendor, product, bcdDevice),
 * the bus, the BT_INFO_* flags, the ACL data length limit now, the
 * product's name, and where it is ("usb1/port7/device3/interface0").
 */
struct bt_info {
	uint16_t vendor;
	uint16_t product;
	uint16_t version;
	uint16_t bus;
	uint32_t flags;
	uint32_t acl_data_max;
	uint32_t reserved[4];
	char name[BT_TEXT_MAX];
	char physical_path[BT_TEXT_MAX];
};

/*
 * The counts of a controller since it came: the events and the ACL
 * packets taken, the commands and the ACL packets sent, the malformed
 * receptions dropped, and the times the reception stopped because the
 * program did not read.
 */
struct bt_stats {
	uint64_t events_in;
	uint64_t acl_in;
	uint64_t commands_out;
	uint64_t acl_out;
	uint64_t malformed;
	uint64_t stalls;
	uint64_t reserved[4];
};

/*
 * BT_IOC_SET_BOOTLOADER: 1 sends the Intel Secure Send commands (opcode
 * 0xFC09) on the bulk OUT pipe and reads the bulk IN pipe as events, 0
 * goes back.  BT_IOC_RESET resets the controller's USB device in place
 * (ENOTSUP when it is not on a root port) and queues the RESET notice.
 * BT_IOC_SET_ACL_MAX sets the ACL data length limit (the controller's,
 * from HCI_Read_Buffer_Size); a value out of range answers EINVAL.
 */
#define BT_IOC_GET_INFO		_IOR(KERN_BT_IOC_GROUP, 0, struct bt_info)
#define BT_IOC_SET_BOOTLOADER	_IOW(KERN_BT_IOC_GROUP, 1, uint32_t)
#define BT_IOC_RESET		_IO(KERN_BT_IOC_GROUP, 2)
#define BT_IOC_SET_ACL_MAX	_IOW(KERN_BT_IOC_GROUP, 3, uint32_t)
#define BT_IOC_GET_STATS	_IOR(KERN_BT_IOC_GROUP, 4, struct bt_stats)

#endif
