/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Which falcons the GSP boot stops when one of its programs did not end.
 *
 * Each point where the boot can be left with a falcon still running names
 * the falcon (or the two) that may be: FWSEC runs on the GSP's falcon,
 * the booters on the secure processor's, and the GSP's firmware on the
 * GSP's own RISC-V core.  The boot stops those, cuts the GPU off the
 * system memory, and gives the firmware's picture its window back
 * (gsp.c).  This file touches no hardware, so the host test links it.
 */

#include <stdint.h>

#include "drivers/gpu/nvrtx/nvrtx-gsp.h"

/*
 * Names the falcons that may still run at a point the boot is left at.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/fwsec.c (nvkm_gsp_fwsec: FWSEC on
 * the GSP's falcon) and nvkm/subdev/gsp/tu102.c (tu102_gsp_booter_load:
 * the booters on the secure processor's falcon).
 */
uint32_t
nvrtx_gsp_hold_falcons(
	enum nvrtx_gsp_hold point)
{
	/* Each point's programs, and the falcon each ran on. */
	switch (point) {
	case NVRTX_GSP_HOLD_FWSEC:
		return NVRTX_GSP_HOLD_GSP_FALCON;
	case NVRTX_GSP_HOLD_BOOTER:
		return NVRTX_GSP_HOLD_GSP_FALCON | NVRTX_GSP_HOLD_SEC2_FALCON;
	case NVRTX_GSP_HOLD_INIT:
		return NVRTX_GSP_HOLD_GSP_FALCON;
	case NVRTX_GSP_HOLD_UNLOAD:
		return NVRTX_GSP_HOLD_GSP_FALCON;
	case NVRTX_GSP_HOLD_TEAR_DOWN:
		return NVRTX_GSP_HOLD_GSP_FALCON | NVRTX_GSP_HOLD_SEC2_FALCON;
	}

	/* An unknown point stops both. */
	return NVRTX_GSP_HOLD_GSP_FALCON | NVRTX_GSP_HOLD_SEC2_FALCON;
}
