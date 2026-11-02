/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The record of USB HID reports
 *
 * Under the boot parameter usbhid.ring=1 the USB HID driver keeps every
 * report each interface's interrupt IN endpoint delivers, with when it came,
 * in a ring that overwrites the oldest record; root reads it from
 * /dev/usbhid-ring.  The ring itself (this header) takes no lock: its owner
 * serializes every call.
 */

#ifndef KERN_DRIVERS_USB_HID_RING_H
#define KERN_DRIVERS_USB_HID_RING_H

#include <stddef.h>
#include <stdint.h>

/* The most bytes of one report a record keeps. */
#define DRV_USB_HID_RING_BYTES		27U

/* A record's flags: what the interface is, and what the record stands for. */
#define DRV_USB_HID_RING_RAW		0x01U	/* a raw interface (hidraw) */
#define DRV_USB_HID_RING_DRAIN		0x02U	/* a vendor interface read and dropped */
#define DRV_USB_HID_RING_STALL		0x04U	/* the endpoint stalled; no report */
#define DRV_USB_HID_RING_ERROR		0x08U	/* a terminal transfer status in bytes[0]; no report */
#define DRV_USB_HID_RING_ATTACH		0x10U	/* the interface went into service; length is its transfer size */
#define DRV_USB_HID_RING_DETACH		0x20U	/* the interface left service */

/*
 * One report as the ring keeps it, and as a reader of /dev/usbhid-ring
 * gets it: 64 bytes in the kernel's byte order, one after another, oldest
 * first.
 *
 * counter_us is the platform's fixed-frequency counter in microseconds (fine
 * enough to space reports apart, its epoch the counter's own) when the
 * transfer's completion reached the HID driver; monotonic_us is
 * CLOCK_MONOTONIC in microseconds at the same moment, as fine as the
 * kernel's tick, the clock the input events and user space's logs carry.
 * sequence counts (modulo 2^32) every record the ring was given since it
 * started, so a gap between two read records is what was overwritten.
 *
 * What the host controller said of the transfer: queued_us, how long it
 * was on the controller (from its submission to the controller's event);
 * event_us, from that event to the completion (counter_us); code, the
 * controller's completion code (xHCI: 1 success, 13 short packet; 0 when
 * the controller's driver does not say); residual, the bytes it did not
 * move.  The submission of this transfer is at counter_us - event_us -
 * queued_us, so the previous record of the interface ended that long
 * before it was armed again.  queued_us and event_us are 0 when not known
 * and saturate at their largest value.
 */
struct drv_usb_hid_ring_record {
	uint64_t counter_us;
	uint64_t monotonic_us;
	uint32_t sequence;
	uint32_t queued_us;
	uint16_t event_us;
	uint16_t length;
	uint16_t residual;
	uint8_t bus;
	uint8_t address;
	uint8_t interface;
	uint8_t endpoint;
	uint8_t kept;
	uint8_t flags;
	uint8_t code;
	uint8_t bytes[DRV_USB_HID_RING_BYTES];
};

/*
 * The ring over caller-provided records.
 *
 * capacity is a power of two.  next is the sequence the next record gets,
 * which is also how many records the ring was ever given: the records from
 * next - capacity (or 0) to next - 1 are the ones it holds.
 */
struct drv_usb_hid_ring {
	struct drv_usb_hid_ring_record *records;
	uint64_t capacity;
	uint64_t next;
};

/*
 * Starts an empty ring over records (capacity of them, a power of two).
 * Returns 0, or EINVAL for no records or a capacity that is not a power of
 * two.
 */
int
drv_usb_hid_ring_init(
	struct drv_usb_hid_ring *ring,
	struct drv_usb_hid_ring_record *records,
	uint64_t capacity);

/*
 * Adds a record, overwriting the oldest when the ring is full; the record's
 * sequence is given by the ring.
 */
void
drv_usb_hid_ring_put(
	struct drv_usb_hid_ring *ring,
	const struct drv_usb_hid_ring_record *record);

/*
 * Copies the ring's bytes, the records laid one after another by sequence
 * (record N at N * sizeof(struct drv_usb_hid_ring_record)), from offset on
 * into out (at most size bytes).  An offset in a record already overwritten
 * starts at the oldest record held instead.  Gives in *next_offset the
 * offset after the last byte copied (or the start the copy moved to), and
 * returns how many bytes were copied: 0 when nothing newer is held.
 */
size_t
drv_usb_hid_ring_read(
	const struct drv_usb_hid_ring *ring,
	uint64_t offset,
	uint8_t *out,
	size_t size,
	uint64_t *next_offset);

#endif
