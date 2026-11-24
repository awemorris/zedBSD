/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Virtual HID device (/dev/input/bridge) management and RDP input to USB HID
 * report conversion for rdpd.
 */

#include "rdpd.h"
#include "userland/desktop/librdp/rdp.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <uapi/input-bridge.h>
#include <unistd.h>

/* Composite HID Report Descriptor: Report ID 1 = Keyboard, Report ID 2 = Abs Mouse. */
static const uint8_t rdpd_hid_desc_bytes[] = {
	/* Keyboard (Report ID 1) */
	0x05U, 0x01U,		/* Usage Page (Generic Desktop) */
	0x09U, 0x06U,		/* Usage (Keyboard) */
	0xA1U, 0x01U,		/* Collection (Application) */
	0x85U, 0x01U,		/*   Report ID (1) */
	0x05U, 0x07U,		/*   Usage Page (Key Codes) */
	0x19U, 0xE0U,		/*   Usage Minimum (224) */
	0x29U, 0xE7U,		/*   Usage Maximum (231) */
	0x15U, 0x00U,		/*   Logical Minimum (0) */
	0x25U, 0x01U,		/*   Logical Maximum (1) */
	0x75U, 0x01U,		/*   Report Size (1) */
	0x95U, 0x08U,		/*   Report Count (8) */
	0x81U, 0x02U,		/*   Input (Data, Variable, Absolute) */
	0x95U, 0x01U,		/*   Report Count (1) */
	0x75U, 0x08U,		/*   Report Size (8) */
	0x81U, 0x01U,		/*   Input (Constant) */
	0x95U, 0x06U,		/*   Report Count (6) */
	0x75U, 0x08U,		/*   Report Size (8) */
	0x15U, 0x00U,		/*   Logical Minimum (0) */
	0x25U, 0xE7U,		/*   Logical Maximum (231) */
	0x05U, 0x07U,		/*   Usage Page (Key Codes) */
	0x19U, 0x00U,		/*   Usage Minimum (0) */
	0x29U, 0xE7U,		/*   Usage Maximum (231) */
	0x81U, 0x00U,		/*   Input (Data, Array) */
	0xC0U,			/* End Collection */

	/* Absolute Mouse + Wheel (Report ID 2) */
	0x05U, 0x01U,		/* Usage Page (Generic Desktop) */
	0x09U, 0x02U,		/* Usage (Mouse) */
	0xA1U, 0x01U,		/* Collection (Application) */
	0x85U, 0x02U,		/*   Report ID (2) */
	0x09U, 0x01U,		/*   Usage (Pointer) */
	0xA1U, 0x00U,		/*   Collection (Physical) */
	0x05U, 0x09U,		/*     Usage Page (Buttons) */
	0x19U, 0x01U,		/*     Usage Minimum (1) */
	0x29U, 0x03U,		/*     Usage Maximum (3) */
	0x15U, 0x00U,		/*     Logical Minimum (0) */
	0x25U, 0x01U,		/*     Logical Maximum (1) */
	0x95U, 0x03U,		/*     Report Count (3) */
	0x75U, 0x01U,		/*     Report Size (1) */
	0x81U, 0x02U,		/*     Input (Data, Variable, Absolute) */
	0x95U, 0x01U,		/*     Report Count (1) */
	0x75U, 0x05U,		/*     Report Size (5) */
	0x81U, 0x01U,		/*     Input (Constant) */
	0x05U, 0x01U,		/*     Usage Page (Generic Desktop) */
	0x09U, 0x30U,		/*     Usage (X) */
	0x09U, 0x31U,		/*     Usage (Y) */
	0x15U, 0x00U,		/*     Logical Minimum (0) */
	0x26U, 0xFFU, 0x7FU,	/*     Logical Maximum (32767) */
	0x75U, 0x10U,		/*     Report Size (16) */
	0x95U, 0x02U,		/*     Report Count (2) */
	0x81U, 0x02U,		/*     Input (Data, Variable, Absolute) */
	0x09U, 0x38U,		/*     Usage (Wheel) */
	0x15U, 0x81U,		/*     Logical Minimum (-127) */
	0x25U, 0x7FU,		/*     Logical Maximum (127) */
	0x75U, 0x08U,		/*     Report Size (8) */
	0x95U, 0x01U,		/*     Report Count (1) */
	0x81U, 0x06U,		/*     Input (Data, Variable, Relative) */
	0xC0U,			/*   End Collection */
	0xC0U			/* End Collection */
};

/* PS/2 Set 1 scancodes (0x00..0x58) to USB HID Usage ID (Page 0x07). */
static const uint8_t ps2_base_to_hid[0x59] = {
	0x00U, 0x29U, 0x1EU, 0x1FU, 0x20U, 0x21U, 0x22U, 0x23U,
	0x24U, 0x25U, 0x26U, 0x27U, 0x2DU, 0x2EU, 0x2AU, 0x2BU,
	0x14U, 0x1AU, 0x08U, 0x15U, 0x17U, 0x1CU, 0x18U, 0x0CU,
	0x12U, 0x13U, 0x2FU, 0x30U, 0x28U, 0xE0U, 0x04U, 0x16U,
	0x07U, 0x09U, 0x0AU, 0x0BU, 0x0DU, 0x0EU, 0x0FU, 0x33U,
	0x34U, 0x35U, 0xE1U, 0x31U, 0x1DU, 0x1BU, 0x06U, 0x19U,
	0x05U, 0x11U, 0x10U, 0x36U, 0x37U, 0x38U, 0xE5U, 0x55U,
	0xE2U, 0x2CU, 0x39U, 0x3AU, 0x3BU, 0x3CU, 0x3DU, 0x3EU,
	0x3FU, 0x40U, 0x41U, 0x42U, 0x43U, 0x53U, 0x47U, 0x5FU,
	0x60U, 0x61U, 0x56U, 0x5CU, 0x5DU, 0x5EU, 0x57U, 0x59U,
	0x5AU, 0x5BU, 0x62U, 0x63U, 0x00U, 0x00U, 0x64U, 0x44U,
	0x45U
};

/* Builds the composite HID report descriptor (keyboard + absolute mouse). */
size_t
rdpd_hid_descriptor(
	uint8_t *out,
	size_t cap)
{
	size_t sz;
	sz = sizeof(rdpd_hid_desc_bytes);
	if (out == NULL || cap < sz)
		return 0U;
	memcpy(out, rdpd_hid_desc_bytes, sz);
	return sz;
}

/* Maps a PS/2 Set 1 scancode + RDP_KEY_* flags to a USB HID Usage ID (0x07). */
uint8_t
rdpd_ps2_to_hid_usage(
	uint16_t scancode,
	uint16_t flags)
{
	if ((flags & RDP_KBD_FLAG_EXTENDED) != 0U) {
		switch (scancode) {
		case 0x1CU: return 0x58U; /* KP Enter */
		case 0x1DU: return 0xE4U; /* Right Ctrl */
		case 0x35U: return 0x54U; /* KP Divide */
		case 0x38U: return 0xE6U; /* Right Alt */
		case 0x47U: return 0x4AU; /* Home */
		case 0x48U: return 0x52U; /* Up */
		case 0x49U: return 0x4BU; /* Page Up */
		case 0x4BU: return 0x50U; /* Left */
		case 0x4DU: return 0x4FU; /* Right */
		case 0x4FU: return 0x4DU; /* End */
		case 0x50U: return 0x51U; /* Down */
		case 0x51U: return 0x4EU; /* Page Down */
		case 0x52U: return 0x49U; /* Insert */
		case 0x53U: return 0x4CU; /* Delete */
		case 0x5BU: return 0xE3U; /* Left GUI */
		case 0x5CU: return 0xE7U; /* Right GUI */
		default: return 0x00U;
		}
	}

	/* Falls back to the base PS/2 Set 1 table. */
	if (scancode < 0x59U)
		return ps2_base_to_hid[scancode];
	return 0x00U;
}

/* Formats a 9-byte keyboard HID report (Report ID 1) into report[9]. */
int
rdpd_hid_format_key(
	struct rdpd_hid *hid,
	uint16_t scancode,
	uint16_t flags,
	uint8_t report[9])
{
	uint8_t usage;
	int release;
	size_t i;
	uint8_t bit;
	int already;
	if (hid == NULL || report == NULL)
		return EINVAL;
	usage = rdpd_ps2_to_hid_usage(scancode, flags);
	if (usage == 0U)
		return EINVAL;
	release = (flags & RDP_KBD_FLAG_RELEASE) != 0U;
	if (usage >= 0xE0U && usage <= 0xE7U) {
		bit = (uint8_t)(1U << (usage - 0xE0U));
		if (release)
			hid->modifiers &= (uint8_t)~bit;
		else
			hid->modifiers |= bit;
	} else {
		if (release) {
			for (i = 0U; i < 6U; i++) {
				if (hid->keys[i] == usage)
					hid->keys[i] = 0U;
			}
		} else {
			already = 0;
			for (i = 0U; i < 6U; i++) {
				if (hid->keys[i] == usage) {
					already = 1;
					break;
				}
			}

			/* Appends the newly pressed key into the first free 6KRO slot. */
			if (!already) {
				for (i = 0U; i < 6U; i++) {
					if (hid->keys[i] == 0U) {
						hid->keys[i] = usage;
						break;
					}
				}
			}
		}
	}

	/* Encodes Report ID 1 (modifiers, reserved, 6KRO keys). */
	report[0] = 0x01U;
	report[1] = hid->modifiers;
	report[2] = 0x00U;
	memcpy(report + 3U, hid->keys, 6U);
	return 0;
}

/* Formats a 7-byte absolute mouse HID report (Report ID 2) into report[7]. */
int
rdpd_hid_format_mouse(
	struct rdpd_hid *hid,
	uint16_t flags,
	uint16_t x,
	uint16_t y,
	uint8_t report[7])
{
	uint32_t sx;
	uint32_t sy;
	uint32_t w;
	uint32_t h;
	int8_t wheel;
	if (hid == NULL || report == NULL)
		return EINVAL;
	w = 1280U;
	if (hid->width > 1U)
		w = hid->width;
	h = 720U;
	if (hid->height > 1U)
		h = hid->height;
	if (x >= w)
		x = (uint16_t)(w - 1U);
	if (y >= h)
		y = (uint16_t)(h - 1U);
	sx = ((uint32_t)x * 32767U) / (w - 1U);
	sy = ((uint32_t)y * 32767U) / (h - 1U);
	if ((flags & RDP_PTR_FLAG_BUTTON1) != 0U) {
		if ((flags & RDP_PTR_FLAG_DOWN) != 0U)
			hid->buttons |= 0x01U;
		else
			hid->buttons &= (uint8_t)~0x01U;
	}

	/* Updates button 2 state. */
	if ((flags & RDP_PTR_FLAG_BUTTON2) != 0U) {
		if ((flags & RDP_PTR_FLAG_DOWN) != 0U)
			hid->buttons |= 0x02U;
		else
			hid->buttons &= (uint8_t)~0x02U;
	}

	/* Updates button 3 state. */
	if ((flags & RDP_PTR_FLAG_BUTTON3) != 0U) {
		if ((flags & RDP_PTR_FLAG_DOWN) != 0U)
			hid->buttons |= 0x04U;
		else
			hid->buttons &= (uint8_t)~0x04U;
	}

	/* Encodes Report ID 2 (buttons, scaled X/Y, wheel). */
	wheel = 0;
	if ((flags & RDP_PTR_FLAG_WHEEL) != 0U) {
		if ((flags & RDP_PTR_FLAG_WHEEL_NEGATIVE) != 0U)
			wheel = -1;
		else
			wheel = 1;
	}

	/* Writes the 7-byte absolute mouse report. */
	report[0] = 0x02U;
	report[1] = hid->buttons;
	report[2] = (uint8_t)(sx & 0xFFU);
	report[3] = (uint8_t)((sx >> 8) & 0xFFU);
	report[4] = (uint8_t)(sy & 0xFFU);
	report[5] = (uint8_t)((sy >> 8) & 0xFFU);
	report[6] = (uint8_t)wheel;
	return 0;
}

/* Opens /dev/input/bridge and registers the virtual HID device. */
int
rdpd_hid_open(
	struct rdpd_hid *hid,
	uint32_t width,
	uint32_t height)
{
	struct input_bridge_setup setup;
	size_t dlen;
	uint32_t dev;
	ssize_t wr;
	int err;
	int rc;
	memset(hid, 0, sizeof(*hid));
	hid->fd = -1;
	hid->width = 1280U;
	if (width != 0U)
		hid->width = width;
	hid->height = 720U;
	if (height != 0U)
		hid->height = height;
	hid->fd = open(RDPD_INPUT_BRIDGE, O_RDWR | O_CLOEXEC);
	if (hid->fd < 0) {
		err = errno;
		if (err == 0)
			err = EIO;
		return err;
	}

	/* Registers the composite HID report descriptor. */
	memset(&setup, 0, sizeof(setup));
	setup.magic = INPUT_BRIDGE_MAGIC;
	setup.version = INPUT_BRIDGE_VERSION;
	setup.bus = 0x06U;
	setup.vendor = 0x1D6BU;
	setup.product = 0x0001U;
	(void)snprintf(setup.name, sizeof(setup.name), "zedBSD RDP Virtual Input");
	dlen = rdpd_hid_descriptor(setup.descriptor, sizeof(setup.descriptor));
	setup.descriptor_size = (uint32_t)dlen;
	wr = write(hid->fd, &setup, sizeof(setup));
	if (wr != (ssize_t)sizeof(setup)) {
		err = errno;
		if (err == 0)
			err = EIO;
		close(hid->fd);
		hid->fd = -1;
		return err;
	}

	/* Queries the assigned /dev/input/eventN index. */
	dev = 0U;
	rc = ioctl(hid->fd, INPUT_BRIDGE_GET_DEVICE, &dev);
	if (rc == 0)
		hid->event_dev = dev;
	return 0;
}

/* Updates desktop dimensions used for mouse coordinate scaling. */
void
rdpd_hid_set_size(
	struct rdpd_hid *hid,
	uint32_t width,
	uint32_t height)
{
	if (hid == NULL)
		return;
	if (width > 1U)
		hid->width = width;
	if (height > 1U)
		hid->height = height;
}

/* Injects a keyboard event into /dev/input/bridge. */
void
rdpd_hid_key(
	struct rdpd_hid *hid,
	uint16_t scancode,
	uint16_t flags)
{
	uint8_t report[9];
	int rc;
	if (hid == NULL || hid->fd < 0)
		return;
	rc = rdpd_hid_format_key(hid, scancode, flags, report);
	if (rc == 0)
		(void)write(hid->fd, report, sizeof(report));
}

/* Injects a mouse event into /dev/input/bridge. */
void
rdpd_hid_mouse(
	struct rdpd_hid *hid,
	uint16_t flags,
	uint16_t x,
	uint16_t y)
{
	uint8_t report[7];
	int rc;
	if (hid == NULL || hid->fd < 0)
		return;
	rc = rdpd_hid_format_mouse(hid, flags, x, y, report);
	if (rc == 0)
		(void)write(hid->fd, report, sizeof(report));
}

/* Closes the /dev/input/bridge descriptor. */
void
rdpd_hid_close(
	struct rdpd_hid *hid)
{
	if (hid == NULL)
		return;
	if (hid->fd >= 0) {
		close(hid->fd);
		hid->fd = -1;
	}
}
