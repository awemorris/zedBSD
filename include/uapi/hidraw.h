/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The raw HID devices: /dev/input/hidrawN (ws161-p002, the user's approval
 * U1 of 2026-10-05).
 *
 * A HID interface that is not an input device (first the FIDO
 * authenticators, usage page 0xF1D0) is published as a raw node instead of
 * an event device.  read() and write() mean what they mean on Linux's
 * hidraw, so a program reads and writes the same bytes on both:
 *
 *   read()   one input report; a device that numbers its reports puts the
 *            report ID in the first byte.  A buffer shorter than the
 *            report gets its first bytes.  It waits for a report unless
 *            the file is O_NONBLOCK (EAGAIN).
 *   write()  one output report; the first byte is the report ID, 0 for a
 *            device that does not number its reports (that byte is then
 *            not sent).  It returns when the device has taken the report.
 *   poll()   POLLIN while a report waits; POLLOUT always; POLLHUP once the
 *            device is gone (read and write then answer ENODEV).
 *
 * Every open has its own queue of HIDRAW_QUEUE input reports (the oldest
 * goes when it is full), so two programs each see every report.  The
 * request numbers and the structures are zedBSD's own.
 */

#ifndef KERN_UAPI_HIDRAW_H
#define KERN_UAPI_HIDRAW_H

#include <stdint.h>
#include <uapi/ioctl.h>

#define KERN_HIDRAW_IOC_GROUP	'H'

/* The largest report descriptor given out, a name's room, and the reports an open keeps. */
#define HIDRAW_DESCRIPTOR_MAX	4096U
#define HIDRAW_TEXT_MAX		64U
#define HIDRAW_QUEUE		64U

/* The largest report read or written, without the report ID's byte. */
#define HIDRAW_REPORT_MAX	1024U

/* The bus a device is on (the values of Linux's input bus types). */
#define HIDRAW_BUS_USB		0x03U
#define HIDRAW_BUS_VIRTUAL	0x06U

/* The FIDO Alliance's usage page and its CTAPHID usage. */
#define HIDRAW_USAGE_PAGE_FIDO	0xf1d0U
#define HIDRAW_USAGE_CTAPHID	0x01U

/* HIDRAW_INFO_NUMBERED: the device numbers its reports (the first byte read is an ID). */
#define HIDRAW_INFO_NUMBERED	0x0001U

/*
 * What a raw device is: its bus and identity, the interface, the top
 * collection's usage page and usage, the largest input and output report
 * (without the ID's byte), and the HIDRAW_INFO_* flags.
 */
struct hidraw_info {
	uint32_t bus;
	uint16_t vendor;
	uint16_t product;
	uint16_t version;
	uint16_t interface_number;
	uint16_t usage_page;
	uint16_t usage;
	uint32_t input_size;
	uint32_t output_size;
	uint32_t flags;
	uint32_t reserved[4];
};

/* The report descriptor as the device gave it: size bytes of value. */
struct hidraw_descriptor {
	uint32_t size;
	uint8_t value[HIDRAW_DESCRIPTOR_MAX];
};

/* A name (the product's) or a place ("usb1/port2/device3/interface1"), ended by a NUL. */
struct hidraw_text {
	char value[HIDRAW_TEXT_MAX];
};

#define HIDRAW_GET_INFO		_IOR(KERN_HIDRAW_IOC_GROUP, 0, struct hidraw_info)
#define HIDRAW_GET_DESCRIPTOR	_IOR(KERN_HIDRAW_IOC_GROUP, 1, struct hidraw_descriptor)
#define HIDRAW_GET_NAME		_IOR(KERN_HIDRAW_IOC_GROUP, 2, struct hidraw_text)
#define HIDRAW_GET_PHYS		_IOR(KERN_HIDRAW_IOC_GROUP, 3, struct hidraw_text)

/*
 * Takes the device for this open alone (nonzero) or gives it back (0)
 * (ws161 V1, the user's approval of 2026-10-05): while an open holds it,
 * the other opens get no input report and their writes fail with EBUSY; a
 * second grab fails with EBUSY.  The holding file's last close gives it
 * back.
 */
#define HIDRAW_GRAB		_IOW(KERN_HIDRAW_IOC_GROUP, 4, int)

#endif
