/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * OpenFirmware to zedBSD sun4u handoff builder.
 */

#ifndef KERN_SPARCV9_HANDOFF_H
#define KERN_SPARCV9_HANDOFF_H

#include <kern/sun4u/boot.h>

int sparcv9_handoff_build(struct sun4u_boot_handoff *handoff,
	const char *bootpath);

#endif
