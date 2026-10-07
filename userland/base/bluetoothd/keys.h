/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's bonds (ws143-p004, design section 6.3, D9: shared by the
 * whole system): one file a bonded device, under
 * FOLDER/<controller's address>/<device's address>-<type>, written whole
 * (a temporary file, fsync, rename, the folder's fsync) and read with
 * every field checked.  The folder is the daemon's account's alone (0700),
 * a file 0600.
 */

#ifndef BLUETOOTHD_KEYS_H
#define BLUETOOTHD_KEYS_H

#include "userland/base/bluetoothd/hci.h"

#include <stddef.h>
#include <stdint.h>

/* Where the bonds are. */
#define BTD_KEYS_FOLDER		"/var/db/bluetooth"

/* The longest path of a bond's file. */
#define BTD_KEYS_PATH_MAX	256U

/*
 * One bond: the device's address and type, its name, and its keys: the
 * BR/EDR link key and its type, or the LE LTK with its EDIV, Rand, size
 * and how it was made, and the LE identity (IRK and address).  Values as
 * HCI and the PDUs carry them (least significant byte first).
 */
struct btd_bond {
	uint8_t address[BTD_ADDRESS_BYTES];
	unsigned type;
	char name[BTD_NAME_MAX];
	int have_link_key;
	uint8_t link_key[16];
	uint8_t link_key_type;
	int have_ltk;
	uint8_t ltk[16];
	uint16_t ediv;
	uint8_t rand[8];
	uint8_t key_size;
	int authenticated;
	int secure;
	int legacy;
	int have_irk;
	uint8_t irk[16];
	int have_identity;
	uint8_t identity_type;
	uint8_t identity[BTD_ADDRESS_BYTES];
};

int btd_keys_path(const char *folder, const uint8_t *controller, const uint8_t *address, unsigned type, char *path, size_t size);
int btd_keys_write(const char *folder, const uint8_t *controller, const struct btd_bond *bond);
int btd_keys_read(const char *folder, const uint8_t *controller, const uint8_t *address, unsigned type, struct btd_bond *bond);
int btd_keys_forget(const char *folder, const uint8_t *controller, const uint8_t *address, unsigned type);
int btd_keys_format(const struct btd_bond *bond, char *text, size_t size);
int btd_keys_parse(const char *text, size_t length, struct btd_bond *bond);
int btd_address_parse(const char *text, uint8_t *address);
int btd_address_type_parse(const char *text, unsigned *type);

#endif
