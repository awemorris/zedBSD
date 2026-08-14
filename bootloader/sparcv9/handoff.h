/* OpenFirmware to zedBSD sun4u handoff builder. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#ifndef ZEDBSD_SPARCV9_HANDOFF_H
#define ZEDBSD_SPARCV9_HANDOFF_H

#include <kern/sun4u/boot.h>

int sparcv9_handoff_build(struct zedbsd_sun4u_handoff *handoff,
	const char *bootpath);

#endif
