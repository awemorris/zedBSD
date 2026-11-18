/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * keiland-preview's confinement on zedBSD (WS168 p003): nothing to do.
 * The caller starts the program with sandbox_spawn, so it is in its
 * sandbox from its first instruction (plan/ws168/preview-sandbox-design.md
 * section 3); the kernel, not the program, keeps it there.
 */

#include "../preview.h"

/*
 * Enters the system's confinement: on zedBSD the process is in it already.
 * Returns 0.
 */
int
preview_confine(void)
{
	/* The sandbox is the kernel's. */
	return 0;
}
