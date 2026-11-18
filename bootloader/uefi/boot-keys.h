/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The boot keys (Ctrl and Shift) as the UEFI loader reads them from the
 * firmware's extended console input.
 */

#ifndef BOOTLOADER_BOOTLOADER_UEFI_BOOT_KEYS_H
#define BOOTLOADER_BOOTLOADER_UEFI_BOOT_KEYS_H

#include "include/uefi.h"

/*
 * The loader's view of the keyboard while it runs.
 *
 * The caller owns one instance for the whole loader run: it is filled by
 * zbl_uefi_boot_keys_open() and gathers the keys every later sample sees,
 * so a key seen early is not forgotten when a later sample finds the queue
 * empty.
 */
struct zbl_uefi_boot_keys {
	/* The firmware's extended console input, or 0 when it has none. */
	EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *input;

	/* Whether the firmware agreed to report modifier keys pressed alone. */
	int exposed;

	/* The ZBL_BOOT_OVERRIDE_* bits of every key event read and every modifier seen held so far. */
	unsigned held;
};

void
zbl_uefi_boot_keys_open(
	struct zbl_uefi_boot_keys *keys,
	EFI_SYSTEM_TABLE *system);

unsigned
zbl_uefi_boot_keys_sample(
	struct zbl_uefi_boot_keys *keys);

unsigned
zbl_uefi_boot_keys_from_state(
	const EFI_KEY_DATA *data);

#endif
