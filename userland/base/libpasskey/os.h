/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libpasskey's operating system layer (ws161-p004; plan/ws161/phase001
 * section 9.4): the security keys' raw HID nodes found, opened, and given
 * to the CTAPHID layer as report functions.
 *
 *   zedBSD  /dev/input/hidrawN, a node whose HIDRAW_GET_INFO says FIDO's
 *           usage page (0xF1D0) and usage 1 (CTAPHID); HIDRAW_GRAB takes it
 *           for one open (os-zedbsd.c)
 *   Linux   /dev/hidrawN, whose report descriptor's first application
 *           collection is FIDO's (os-linux.c; no grab)
 *
 * The report functions on an open node are the same on both (os-posix.c).
 *
 * The smart card slots (ws161-p005): on zedBSD /dev/smartcardN, a reader's
 * slot (an NFC reader's contactless one, where a key is held) whose card
 * is present; opened, its card powered and its APDUs exchanged through
 * CCID_TRANSMIT for the NFC transport (nfc.h).  Linux and FreeBSD list
 * none (NFC there is later, plan/ws161/phase001 section 9.4).
 *
 * For a key held to the reader while something waits (ws199-p001 section
 * 5, BUG-286), a slot is attached first, with or without a card: its
 * node open, the card not powered; its card's comings and goings are read
 * from it (pk_os_card_event, and poll on the descriptor), and a card is
 * then powered and its FIDO applet selected (pk_os_card_select), or
 * powered off again with the slot kept (pk_os_card_power_off).
 * The report descriptor's reading is a pure function (descriptor.c), so the
 * host tests try it alone.  Each system's file is built only for its own
 * system.
 */

#ifndef LIBPASSKEY_OS_H
#define LIBPASSKEY_OS_H

#include <stddef.h>
#include <stdint.h>

#include "hid.h"
#include "nfc.h"

/* The longest node path and product name kept (with the NUL), and the most keys listed. */
#define PK_OS_PATH_MAX		64U
#define PK_OS_NAME_MAX		64U
#define PK_OS_DEVICES_MAX	16U

/* A security key found: its node, its product's name, its vendor and product. */
struct pk_os_device {
	char path[PK_OS_PATH_MAX];
	char name[PK_OS_NAME_MAX];
	uint16_t vendor;
	uint16_t product;
};

/* An open key: the node's descriptor (-1 when closed). */
struct pk_os_hid {
	int descriptor;
};

int pk_os_list(struct pk_os_device *devices, size_t capacity, size_t *count);
int pk_os_open(struct pk_os_hid *handle, const char *path, int grab, struct pk_hid_io *io);
void pk_os_close(struct pk_os_hid *handle);
void pk_os_posix_io(struct pk_os_hid *handle, struct pk_hid_io *io);
int pk_os_descriptor_is_fido(const uint8_t *descriptor, size_t size);

/* An open smart card slot: its node (-1 when closed), its card powered while open. */
struct pk_os_card {
	int descriptor;
};

int pk_os_list_cards(struct pk_os_device *devices, size_t capacity, size_t *count);
int pk_os_card_open(struct pk_os_card *card, const char *path, struct pk_nfc_io *io);
void pk_os_card_close(struct pk_os_card *card);
int pk_os_list_slots(struct pk_os_device *devices, size_t capacity, size_t *count);
int pk_os_card_attach(struct pk_os_card *card, const char *path);
int pk_os_card_present(struct pk_os_card *card, int *present);
int pk_os_card_event(struct pk_os_card *card, int *inserted);
int pk_os_card_select(struct pk_os_card *card, struct pk_nfc *nfc, struct pk_transport *transport, unsigned timeout_ms);
void pk_os_card_power_off(struct pk_os_card *card);

#endif
