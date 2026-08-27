/* Versioned bounded text record shared by x86 loaders and HALs. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_BOOT_PARAMETER_HANDOFF_H
#define ZEDBSD_BOOT_PARAMETER_HANDOFF_H

#include "parameters.h"

#define ZEDBSD_BOOT_PARAMETER_RECORD_MAGIC 0x31525042 /* "BPR1" */
#define ZEDBSD_BOOT_PARAMETER_RECORD_VERSION 1
#define ZEDBSD_BOOT_PARAMETER_RECORD_FLAG_TEXT (1 << 0)
#define ZEDBSD_BOOT_PARAMETER_RECORD_HEADER_SIZE 16
#define ZEDBSD_BOOT_PARAMETER_RECORD_SIZE \
	(ZEDBSD_BOOT_PARAMETER_RECORD_HEADER_SIZE + \
	 ZEDBSD_BOOT_PARAMETERS_STORAGE_SIZE)

#define ZEDBSD_BOOT_PARAMETER_RECORD_MAGIC_OFFSET 0U
#define ZEDBSD_BOOT_PARAMETER_RECORD_VERSION_OFFSET 4U
#define ZEDBSD_BOOT_PARAMETER_RECORD_SIZE_OFFSET 6U
#define ZEDBSD_BOOT_PARAMETER_RECORD_FLAGS_OFFSET 8U
#define ZEDBSD_BOOT_PARAMETER_RECORD_LENGTH_OFFSET 10U
#define ZEDBSD_BOOT_PARAMETER_RECORD_RESERVED_OFFSET 12U
#define ZEDBSD_BOOT_PARAMETER_RECORD_TEXT_OFFSET 16U

#ifndef __ASSEMBLER__
#include <stdint.h>

struct zedbsd_boot_parameter_record {
	uint32_t magic;
	uint16_t version;
	uint16_t size;
	uint16_t flags;
	uint16_t length;
	uint32_t reserved;
	char text[ZEDBSD_BOOT_PARAMETERS_STORAGE_SIZE];
} __attribute__((packed));

_Static_assert(sizeof(struct zedbsd_boot_parameter_record) ==
	       ZEDBSD_BOOT_PARAMETER_RECORD_SIZE,
	       "boot-parameter record size");
#endif

#endif
