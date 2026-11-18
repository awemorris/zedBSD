/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv64 loader's list of the RAM the firmware keeps after boot
 * services end, which the handoff passes to the HAL.
 */

#ifndef KERN_BOOTLOADER_UEFI_RISCV64_RESERVED_H
#define KERN_BOOTLOADER_UEFI_RISCV64_RESERVED_H

#include "include/uefi.h"
#include "bootloader/include/riscv64-handoff.h"

int
zbl_riscv64_firmware_keeps(
	UINT32 type);

void
zbl_riscv64_reserved_add(
	struct zbl6_riscv64_handoff *handoff,
	uint64_t base,
	uint64_t size);

void
zbl_riscv64_reserved_collect(
	struct zbl6_riscv64_handoff *handoff,
	const void *map,
	UINTN map_size,
	UINTN descriptor_size);

#endif
