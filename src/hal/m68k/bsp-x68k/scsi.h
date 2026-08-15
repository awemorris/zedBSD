/* X68000 internal SCSI controller board wiring. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#ifndef ZEDBSD_HAL_M68K_X68K_SCSI_H
#define ZEDBSD_HAL_M68K_X68K_SCSI_H

#include "drivers/x68k-mb89352.h"

void x68k_bsp_spc_bus(struct x68k_spc_bus *bus);
unsigned x68k_bsp_scsi_initiator_id(void);

#endif
