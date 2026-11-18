/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The HID input glue (ws143-p005): what turns a HID report descriptor into
 * evdev devices and every input report into their events, for any transport
 * that carries HID reports (usb-hid, and /dev/input/bridge for the Bluetooth
 * daemon).
 *
 * A transport hands the report descriptor to drv_hid_input_prepare(), which
 * parses it and describes the devices it declares (capabilities, axes, a pen,
 * a touch screen or pad of its own), and registers nothing.  It then names
 * the devices and registers them with drv_hid_input_publish(), hands each
 * input report as it came to drv_hid_input_report(), and takes the devices
 * away with drv_hid_input_unpublish(), which lets the input layer release
 * every key and button still held.  drv_hid_input_destroy() frees the rest.
 *
 * The glue takes no lock: its caller hands it one report at a time and does
 * not unpublish while a report is being handed on (usb-hid's worker thread,
 * or the open file of /dev/input/bridge).
 */

#ifndef KERN_DRIVERS_HID_INPUT_H
#define KERN_DRIVERS_HID_INPUT_H

#include <drivers/generic/hid-report.h>
#include <uapi/input.h>

#include <stddef.h>
#include <stdint.h>

/*
 * What a device chiefly is, for the name a transport gives a device that
 * has none of its own: one with a pen, an absolute pointer (a tablet), a
 * relative pointer (a mouse), and anything else (a keyboard).
 */
#define HID_INPUT_KIND_KEYBOARD		0U
#define HID_INPUT_KIND_MOUSE		1U
#define HID_INPUT_KIND_TABLET		2U
#define HID_INPUT_KIND_PEN		3U

/*
 * struct hid_input_identity's flags: publish the touch screen's or pad's
 * device alone, and leave the rest of the descriptor (a touch pad's mouse
 * collection) unpublished, its reports dropped (i2c-hid, ws143-p005).
 */
#define HID_INPUT_TOUCH_ONLY		0x1U

/*
 * The interface is the mouse a touch screen emulates beside it (another
 * interface of the same device publishes the screen, BUG-313): nothing is
 * published, and its reports are dropped.
 */
#define HID_INPUT_EMULATION		0x2U

/*
 * The interface is a touch screen whose main device beside it (a pen, a
 * keyboard) may never report: BUG-313's Wacom screen declares both, with
 * no key on it and no pen known.  The touch device is published now, and
 * the main device on its first report that holds a keyboard's key or
 * brings a pen into range.
 */
#define HID_INPUT_MAIN_LATER		0x4U

/*
 * One descriptor's devices: the parsed layout, what they declare, the
 * states of the pen and touch state machines, the keys each report holds,
 * and the input devices once published.  Owned by the transport from
 * prepare to destroy.
 */
struct hid_input;

/*
 * How a transport names the devices it publishes: the main device's name,
 * physical path and unique ID (NULL for none), the touch screen's or pad's
 * name and physical path (it is a device of its own), the bus and the
 * vendor's numbers both carry, and HID_INPUT_* flags.  The strings are at
 * most 63 bytes and a NUL (the input layer refuses longer ones); the input
 * layer copies them.
 */
struct hid_input_identity {
	const char *name;
	const char *physical_path;
	const char *unique_id;
	const char *touch_name;
	const char *touch_physical_path;
	struct input_id id;
	unsigned flags;
};

int drv_hid_input_prepare(const void *descriptor, size_t size, struct hid_input **result);
size_t drv_hid_input_report_max(const struct hid_input *input);
int drv_hid_input_report_ids(const struct hid_input *input);
int drv_hid_input_report_short(const struct hid_input *input, const uint8_t *report, size_t length);
unsigned drv_hid_input_kind(const struct hid_input *input);
int drv_hid_input_touch(const struct hid_input *input, struct hid_report_touch_info *touch);

/* Reports nonzero when the descriptor declares a touch screen (a direct touch device, not a pad). */
int drv_hid_input_touch_screen(const struct hid_input *input);
int drv_hid_input_feature(const struct hid_input *input, uint32_t usage, struct hid_report_feature_info *feature);

/*
 * Reports nonzero when the main device declares a keyboard's key (an EV_KEY
 * that is not a button): a collection beside a touch screen worth publishing.
 */
int drv_hid_input_has_keys(const struct hid_input *input);

/*
 * Gives a touch screen or pad at least this many slots (its Contact Count
 * Maximum, at most HID_TOUCH_SLOTS_MAX), before it is published.  ENOENT
 * without one, EBUSY once published.
 */
int drv_hid_input_touch_slots(struct hid_input *input, unsigned contacts);
int drv_hid_input_publish(struct hid_input *input, const struct hid_input_identity *identity);
void drv_hid_input_report(struct hid_input *input, const uint8_t *report, size_t length, uint64_t milliseconds);
unsigned drv_hid_input_malformed(const struct hid_input *input);
void drv_hid_input_numbers(const struct hid_input *input, int *event, int *touch_event);
void drv_hid_input_unpublish(struct hid_input *input);
void drv_hid_input_destroy(struct hid_input *input);

#endif
