/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv64 loader's reading of the boot hart from a flattened device tree.
 *
 * A firmware without the RISC-V boot protocol names the hart it booted on
 * in the tree's /chosen node instead.  This is the only thing the loader
 * reads out of the tree; the HAL reads the rest.
 */

#ifndef KERN_BOOTLOADER_UEFI_FDT_HARTID_H
#define KERN_BOOTLOADER_UEFI_FDT_HARTID_H

#include <stdint.h>

/* What reading /chosen's boot hart found. */
enum zbl_fdt_hartid_result {
	ZBL_FDT_HARTID_FOUND,
	ZBL_FDT_HARTID_NO_CHOSEN,
	ZBL_FDT_HARTID_NO_PROPERTY,
	ZBL_FDT_HARTID_BAD_LENGTH,
	ZBL_FDT_HARTID_MALFORMED
};

/*
 * Reads /chosen's boot-hartid from a flattened tree of size bytes.
 *
 * The property is one 32-bit or one 64-bit cell; *hart is written only
 * when the answer is ZBL_FDT_HARTID_FOUND.
 */
enum zbl_fdt_hartid_result
zbl_fdt_boot_hartid(
	const uint8_t *tree,
	uint32_t size,
	uint64_t *hart);

/* Names a reading's result for the console. */
const char *
zbl_fdt_hartid_result_name(
	enum zbl_fdt_hartid_result result);

#endif
