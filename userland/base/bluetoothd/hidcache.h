/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's records of HID devices (ws143-p005 i02, plan/ws143/
 * phase005/phase.md section 9.3, review B4): one file a device beside its
 * bond, FOLDER/<controller>/<address>-<type>.hid, apart from the bond's
 * file (whose size and fields stay as they are).  The record says whether
 * the device is only a candidate (handed over after pairing) or confirmed
 * by SDP, how it reconnects, its PnP numbers, its name, and for BR/EDR its
 * report descriptor (in hex lines of 128 bytes), used when the device
 * connects by itself and sends reports right after its channels open.
 * Written whole (a temporary file, fsync, rename); a record with a line
 * missing, repeated or not matching its size is refused whole.
 */

#ifndef BLUETOOTHD_HIDCACHE_H
#define BLUETOOTHD_HIDCACHE_H

#include "userland/base/bluetoothd/hci.h"

#include <stddef.h>
#include <stdint.h>

/* The longest report descriptor kept, and the bytes of one of its lines. */
#define BTD_HIDCACHE_DESCRIPTOR_MAX	4096U
#define BTD_HIDCACHE_LINE_BYTES		128U

/* The most a record's file holds (its descriptor's 32 lines and the rest). */
#define BTD_HIDCACHE_TEXT_MAX		12288U

/* The record of one HID device. */
struct btd_hidcache {
	uint8_t address[BTD_ADDRESS_BYTES];
	unsigned type;
	int confirmed;
	int le;
	int reconnect_initiate;
	int normally_connectable;
	int virtual_cable;
	int boot_device;
	uint16_t vendor;
	uint16_t product;
	uint16_t version;
	uint8_t country;
	uint32_t device_class;
	uint16_t appearance;
	char name[BTD_NAME_MAX];
	size_t descriptor_size;
	uint8_t descriptor[BTD_HIDCACHE_DESCRIPTOR_MAX];
};

int btd_hidcache_path(const char *folder, const uint8_t *controller, const uint8_t *address, unsigned type, char *path, size_t size);
int btd_hidcache_format(const struct btd_hidcache *record, char *text, size_t size);
int btd_hidcache_parse(const char *text, size_t length, struct btd_hidcache *record);
int btd_hidcache_write(const char *folder, const uint8_t *controller, const struct btd_hidcache *record);
int btd_hidcache_read(const char *folder, const uint8_t *controller, const uint8_t *address, unsigned type, struct btd_hidcache *record);
int btd_hidcache_forget(const char *folder, const uint8_t *controller, const uint8_t *address, unsigned type);

#endif
