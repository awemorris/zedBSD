/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PC-98 native boot loader's handoff to the HAL.
 *
 * The native loader (BOOTZBSD.EXE) hands the HAL the common boot record
 * followed by the boot-parameter record.  The HAL checks it, copies what it
 * keeps, and gives the kernel only the common record; the layout below is
 * known to the loader and the HAL alone.  The loader's assembly reads the
 * constants, so everything but them is hidden from the assembler.
 */

#ifndef KERN_HAL_I386_PC98_HANDOFF_H
#define KERN_HAL_I386_PC98_HANDOFF_H

#include "include/kern/boot.h"

/*
 * The version the native loader writes into the common record.  The
 * loader's assembler reads it, so it carries no integer suffix.
 */
#define PC98_HANDOFF_VERSION_NATIVE		2

/* The size of the common record that starts every PC-98 handoff. */
#define PC98_HANDOFF_COMMON_SIZE		24

/* Where the boot-parameter record starts in a native handoff. */
#define PC98_PARAMETER_RECORD_OFFSET		PC98_HANDOFF_COMMON_SIZE

/* The size of a native handoff that carries boot parameters. */
#define PC98_PARAMETER_HANDOFF_SIZE		(PC98_HANDOFF_COMMON_SIZE + KERN_BOOT_PARAMETER_RECORD_SIZE)

#ifndef __ASSEMBLER__

/*
 * A native handoff that carries boot parameters.
 *
 * The loader builds one in its own memory and passes its address; the HAL
 * reads it once during early boot and never keeps a pointer to it.
 */
struct pc98_parameter_handoff {
	struct kern_boot_handoff common;
	struct kern_boot_parameter_record parameters;
} __attribute__((packed));

#endif /* __ASSEMBLER__ */

#endif
