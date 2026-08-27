/* PC-98 loader parameter extension around the stable 24-byte handoff. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_BOOT_PC98_HANDOFF_H
#define ZEDBSD_BOOT_PC98_HANDOFF_H

#include "parameter-handoff.h"

#define ZEDBSD_PC98_HANDOFF_COMMON_SIZE 24
#define ZEDBSD_PC98_PARAMETER_RECORD_OFFSET ZEDBSD_PC98_HANDOFF_COMMON_SIZE
#define ZEDBSD_PC98_PARAMETER_HANDOFF_SIZE \
	(ZEDBSD_PC98_HANDOFF_COMMON_SIZE + ZEDBSD_BOOT_PARAMETER_RECORD_SIZE)

#ifndef __ASSEMBLER__
#include <kern/boot.h>

struct zedbsd_pc98_parameter_handoff {
	struct boot_handoff common;
	struct zedbsd_boot_parameter_record parameters;
} __attribute__((packed));

_Static_assert(sizeof(struct zedbsd_pc98_parameter_handoff) ==
	       ZEDBSD_PC98_PARAMETER_HANDOFF_SIZE,
	       "PC-98 parameter handoff size");
_Static_assert(__builtin_offsetof(struct zedbsd_pc98_parameter_handoff,
				 parameters) ==
	       ZEDBSD_PC98_PARAMETER_RECORD_OFFSET,
	       "PC-98 parameter record offset");
#endif

#endif
