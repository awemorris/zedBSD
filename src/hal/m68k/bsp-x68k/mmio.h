/* Typed X68000 device apertures in the MC68030 high direct map. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#ifndef ZEDBSD_HAL_M68K_X68K_MMIO_H
#define ZEDBSD_HAL_M68K_X68K_MMIO_H

#include <hal/hal.h>

#define X68K_DIRECT_BASE       0x80000000U
#define X68K_MFP_PHYSICAL      0x00e88000U
#define X68K_SYSPORT_PHYSICAL  0x00e8e000U
#define X68K_SPC_PHYSICAL      0x00e96020U
#define X68K_IOC_PHYSICAL      0x00e9c000U
#define X68K_TVRAM_PHYSICAL    0x00e00000U
#define X68K_CRTC_PHYSICAL     0x00e80000U
#define X68K_SYSPORT_POWEROFF_REG 7U

/* MFP and internal SPC are 8-bit devices wired to the odd byte lane. */
#define X68K_ODD8_ADDRESS(base, reg) ((uintptr_t)(base) + 1U + \
	(uintptr_t)(reg) * 2U)
#define X68K_DEVICE_ADDRESS(physical) \
	((uintptr_t)X68K_DIRECT_BASE + (uintptr_t)(physical))
#define X68K_MFP_ADDRESS(reg) X68K_DEVICE_ADDRESS( \
	X68K_ODD8_ADDRESS(X68K_MFP_PHYSICAL, (reg)))
#define X68K_SPC_ADDRESS(reg) X68K_DEVICE_ADDRESS( \
	X68K_ODD8_ADDRESS(X68K_SPC_PHYSICAL, (reg)))
#define X68K_SYSPORT_ADDRESS(reg) X68K_DEVICE_ADDRESS( \
	X68K_ODD8_ADDRESS(X68K_SYSPORT_PHYSICAL, (reg)))

static inline uint8_t x68k_mfp_read(unsigned reg)
{ return hal_mmio_read8((const volatile void *)X68K_MFP_ADDRESS(reg)); }
static inline void x68k_mfp_write(unsigned reg, uint8_t value)
{ hal_mmio_write8((volatile void *)X68K_MFP_ADDRESS(reg), value); }
static inline uint8_t x68k_spc_read(unsigned reg)
{ return hal_mmio_read8((const volatile void *)X68K_SPC_ADDRESS(reg)); }
static inline void x68k_spc_write(unsigned reg, uint8_t value)
{ hal_mmio_write8((volatile void *)X68K_SPC_ADDRESS(reg), value); }
static inline uint8_t x68k_sysport_read(unsigned reg)
{ return hal_mmio_read8((const volatile void *)X68K_SYSPORT_ADDRESS(reg)); }
static inline void x68k_sysport_write(unsigned reg, uint8_t value)
{ hal_mmio_write8((volatile void *)X68K_SYSPORT_ADDRESS(reg), value); }

#endif
