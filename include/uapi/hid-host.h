/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * /dev/hid-host: HID devices made by a program (ws143-p005, design D3): the
 * Bluetooth daemon's keyboards, mice and the like, which reach readers as
 * /dev/input/eventN like a USB device of the same report descriptor.
 *
 * Only root may open the node (the daemon's privileged part opens it and
 * hands the descriptor on), and at most HID_HOST_OPENS_MAX opens at a time,
 * each one device.
 *
 * The first write on an open is one struct hid_host_setup, exactly its
 * size: the bus (BUS_BLUETOOTH or BUS_VIRTUAL), the vendor's numbers, the
 * names and the report descriptor.  The kernel parses the descriptor as it
 * parses a USB device's and makes the devices it declares (a touch screen
 * or pad is a device of its own).  It is refused with EINVAL (a setup that
 * is malformed), ENXIO (a FIDO descriptor: a security key is not an input
 * device), the parser's EINVAL, E2BIG, ENOMEM or EOPNOTSUPP, ENODEV (the
 * descriptor declares no input device) or ENOSPC (the input layer is
 * full); after a refusal the open may declare again.
 *
 * Every later write is one input report as the device sent it, its report
 * ID's byte first when the descriptor numbers its reports (as a hidraw read
 * gives it), at most HID_HOST_REPORT_MAX bytes.  A report shorter than its
 * report ID declares is refused with EINVAL; a longer one is taken and its
 * rest not read.  A report the descriptor's layout cannot decode is counted
 * (struct hid_host_device's malformed) and dropped; the write still answers
 * its size.  A report before the setup is refused with EINVAL.
 *
 * A read answers EAGAIN: no output report (a keyboard's lights) is passed
 * back yet.  poll() says the node is writable and never readable.
 *
 * HID_HOST_GET_DEVICE tells what the open made: the numbers N of the
 * /dev/input/eventN nodes (-1 for none), the reports dropped, and what the
 * layout is.
 *
 * Closing the file removes the devices, and the input layer releases every
 * key and button they held.
 */

#ifndef KERN_UAPI_HID_HOST_H
#define KERN_UAPI_HID_HOST_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <uapi/ioctl.h>

/* The requests' group: 'h' is free ('H' is hidraw's). */
#define KERN_HID_HOST_IOC_GROUP	'h'

/* The setup's magic ("hiht" in memory order) and the version of its form. */
#define HID_HOST_MAGIC		0x74686968U
#define HID_HOST_VERSION	1U

/* The longest report descriptor (the parser's), report, and text (the input layer's 63 bytes and a NUL). */
#define HID_HOST_DESCRIPTOR_MAX	4096U
#define HID_HOST_REPORT_MAX	512U
#define HID_HOST_TEXT_MAX	64U

/* The most opens (devices) at once. */
#define HID_HOST_OPENS_MAX	6U

/*
 * The first write on an open: the device it makes.  The texts end with a
 * NUL; an empty name lets the kernel name the device after what it is.
 * physical_path says where the device is ("bluetooth/<controller>/<peer>"),
 * unique_id is the same across reconnections (the peer's address).
 */
struct hid_host_setup {
	uint32_t magic;
	uint32_t version;
	uint16_t bus;
	uint16_t vendor;
	uint16_t product;
	uint16_t release;
	uint32_t descriptor_size;
	uint32_t reserved[4];
	char name[HID_HOST_TEXT_MAX];
	char physical_path[HID_HOST_TEXT_MAX];
	char unique_id[HID_HOST_TEXT_MAX];
	uint8_t descriptor[HID_HOST_DESCRIPTOR_MAX];
};

/* The setup has no padding: its size is its fields', on every ABI. */
_Static_assert(sizeof(struct hid_host_setup) == 4324U,
    "the hid-host setup is the same on ILP32 and LP64");

/* struct hid_host_device's flags: the layout numbers its reports. */
#define HID_HOST_FLAG_REPORT_IDS	0x1U

/*
 * What an open made: the main device's and the touch device's node numbers
 * (-1 for none, before the setup too), the reports the layout refused, the
 * layout's flags, and its longest input report in bytes (the ID's byte
 * included).
 */
struct hid_host_device {
	int32_t event;
	int32_t touch_event;
	uint32_t malformed;
	uint32_t flags;
	uint32_t report_max;
	uint32_t reserved[3];
};

#define HID_HOST_GET_DEVICE	_IOR(KERN_HID_HOST_IOC_GROUP, 0, struct hid_host_device)

#ifdef __cplusplus
}
#endif

#endif
