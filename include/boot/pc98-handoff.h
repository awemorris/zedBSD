/* PC-98 loader parameter extension around the stable 24-byte handoff. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef KERN_BOOT_PC98_HANDOFF_H
#define KERN_BOOT_PC98_HANDOFF_H

#include "parameter-handoff.h"

#define KERN_PC98_HANDOFF_COMMON_SIZE 24
#define KERN_PC98_PARAMETER_RECORD_OFFSET KERN_PC98_HANDOFF_COMMON_SIZE
#define KERN_PC98_PARAMETER_HANDOFF_SIZE \
	(KERN_PC98_HANDOFF_COMMON_SIZE + KERN_BOOT_PARAMETER_RECORD_SIZE)

#ifndef __ASSEMBLER__
#include <kern/boot.h>

struct kern_pc98_parameter_handoff {
	struct boot_handoff common;
	struct kern_boot_parameter_record parameters;
} __attribute__((packed));

_Static_assert(sizeof(struct kern_pc98_parameter_handoff) ==
	       KERN_PC98_PARAMETER_HANDOFF_SIZE,
	       "PC-98 parameter handoff size");
_Static_assert(__builtin_offsetof(struct kern_pc98_parameter_handoff,
				 parameters) ==
	       KERN_PC98_PARAMETER_RECORD_OFFSET,
	       "PC-98 parameter record offset");
#endif

#endif
