/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Emit one fixed-size versioned parameter record into an assembly image.
 */

#include "include/kern/boot.h"

.macro KERN_EMIT_BOOT_PARAMETER_RECORD
	.long KERN_BOOT_PARAMETER_RECORD_MAGIC
	.word KERN_BOOT_PARAMETER_RECORD_VERSION
	.word KERN_BOOT_PARAMETER_RECORD_SIZE
	.word KERN_BOOT_PARAMETER_RECORD_FLAG_TEXT
	.word .Lkern_boot_parameters_end\@ - \
	      .Lkern_boot_parameters_start\@
	.long 0
.Lkern_boot_parameters_start\@:
	.ascii KERN_IMAGE_BOOT_PARAMETERS_TEXT
.Lkern_boot_parameters_end\@:
	.byte 0
	.space KERN_BOOT_PARAMETERS_STORAGE_SIZE - \
	       (.Lkern_boot_parameters_end\@ - \
	        .Lkern_boot_parameters_start\@) - 1
.endm
