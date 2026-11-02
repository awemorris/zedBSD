/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The raw HID devices' class (ws161-p002; the node's interface is
 * <uapi/hidraw.h>).
 *
 * A transport (usb-hid, or the test kernel's loopback authenticator)
 * registers a raw device with what it is and one operation, the sending of
 * an output report, and hands every input report it receives to
 * drv_hidraw_input().  The class publishes hidrawN (devfs shows it as
 * /dev/input/hidrawN), keeps a queue for each open, and answers the
 * requests.  drv_hidraw_unregister() takes the transport away: no output
 * runs after it returns, and the files still open answer ENODEV.
 */

#ifndef DRIVERS_GENERIC_HIDRAW_H
#define DRIVERS_GENERIC_HIDRAW_H

#include <stddef.h>
#include <stdint.h>

#include <uapi/hidraw.h>

struct drv_hidraw;

/* The most raw devices published at once. */
#define DRV_HIDRAW_MAX		16U

/*
 * What a transport does for its raw device: sends one output report (the
 * report ID's byte first, 0 for a device that does not number its
 * reports) and returns when the device has taken it, or an errno value.
 * It runs in the writing process's context and may sleep.
 */
struct drv_hidraw_ops {
	int (*output)(void *context, const uint8_t *report, size_t length);
};

/*
 * What a transport says of its raw device at registration: the identity
 * the requests give out (info, the name and the place), and the report
 * descriptor, which is copied.
 */
struct drv_hidraw_description {
	struct hidraw_info info;
	const char *name;
	const char *physical_path;
	const uint8_t *descriptor;
	size_t descriptor_size;
};

/*
 * What drv_hidraw_describe() reads from a report descriptor: the top
 * collection's usage page and usage, whether the reports are numbered,
 * the bytes of the input and the output reports, and whether every top
 * collection is of a vendor-defined usage page (0xFF00 to 0xFFFF).
 */
struct drv_hidraw_layout {
	uint16_t usage_page;
	uint16_t usage;
	int numbered;
	uint32_t input_size;
	uint32_t output_size;
	int vendor_only;
};

int drv_hidraw_register(const struct drv_hidraw_description *description, const struct drv_hidraw_ops *ops, void *context, struct drv_hidraw **result);
void drv_hidraw_input(struct drv_hidraw *hidraw, const uint8_t *report, size_t length);
void drv_hidraw_unregister(struct drv_hidraw *hidraw);
int drv_hidraw_describe(const uint8_t *descriptor, size_t size, struct drv_hidraw_layout *layout);

/* The test kernel's loopback authenticator (CONFIG_SECURITY_KEY_TEST_LOOPBACK, hidraw-loopback.c). */
int drv_hidraw_loopback_register(void);

#endif
