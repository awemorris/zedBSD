/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The boot keys' rewrite of an assembled boot parameter record, shared by
 * the loaders.
 */

#ifndef KERN_BOOTLOADER_BOOT_OVERRIDE_H
#define KERN_BOOTLOADER_BOOT_OVERRIDE_H

/* Ctrl was held: the kernel messages go to the console and no logo is drawn. */
#define ZBL_BOOT_OVERRIDE_KMSG	0x1U

/* Shift was held: the login is on the console, not in a graphical session. */
#define ZBL_BOOT_OVERRIDE_LOGIN	0x2U

#ifndef __ASSEMBLER__
#include "../include/boot-parameter-handoff.h"

int
zbl_boot_override_apply(
	struct kern_boot_parameter_record *record,
	unsigned keys);

#endif
#endif
