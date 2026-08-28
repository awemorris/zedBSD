/* Emit one fixed-size versioned parameter record into an assembly image. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "include/boot/parameter-handoff.h"

.macro ZEDBSD_EMIT_BOOT_PARAMETER_RECORD
	.long ZEDBSD_BOOT_PARAMETER_RECORD_MAGIC
	.word ZEDBSD_BOOT_PARAMETER_RECORD_VERSION
	.word ZEDBSD_BOOT_PARAMETER_RECORD_SIZE
	.word ZEDBSD_BOOT_PARAMETER_RECORD_FLAG_TEXT
	.word .Lzedbsd_boot_parameters_end\@ - \
	      .Lzedbsd_boot_parameters_start\@
	.long 0
.Lzedbsd_boot_parameters_start\@:
	.ascii ZEDBSD_IMAGE_BOOT_PARAMETERS_TEXT
.Lzedbsd_boot_parameters_end\@:
	.byte 0
	.space ZEDBSD_BOOT_PARAMETERS_STORAGE_SIZE - \
	       (.Lzedbsd_boot_parameters_end\@ - \
	        .Lzedbsd_boot_parameters_start\@) - 1
.endm
