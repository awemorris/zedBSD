/* X68000 BSP boundary used by the reusable m68k HAL. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#ifndef ZEDBSD_HAL_M68K_X68K_BSP_H
#define ZEDBSD_HAL_M68K_X68K_BSP_H

#include <kern/boot.h>

void x68k_boot_init(const struct zedbsd_x68k_handoff *handoff);
const struct zedbsd_x68k_handoff *x68k_boot_handoff(void);
int x68k_boot_handoff_valid(const struct zedbsd_x68k_handoff *handoff);

#endif
