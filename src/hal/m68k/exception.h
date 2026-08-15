/* MC68030 exception-frame parsing and generic trap classification. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#ifndef ZEDBSD_HAL_M68K_EXCEPTION_H
#define ZEDBSD_HAL_M68K_EXCEPTION_H

#include <hal/hal.h>

#define M68K_EXCEPTION_BASIC_SIZE 8U
#define M68K_EXCEPTION_FORMAT_A   0x0aU
#define M68K_EXCEPTION_FORMAT_B   0x0bU
#define M68K_EXCEPTION_SSW_DF     0x0100U
#define M68K_EXCEPTION_SSW_RW     0x0040U

#define M68K030_MMUSR_BUS_ERROR   0x8000U
#define M68K030_MMUSR_LIMIT       0x4000U
#define M68K030_MMUSR_SUPERVISOR  0x2000U
#define M68K030_MMUSR_ACCESS      0x1000U
#define M68K030_MMUSR_WRITE_PROT  0x0800U
#define M68K030_MMUSR_INVALID     0x0400U

size_t m68k_exception_frame_size(uint16_t format_vector);
int m68k_exception_fault_address(const void *frame, size_t available,
	uintptr_t *address, uint16_t *ssw);
uint32_t m68k_exception_cause(unsigned vector, uint16_t mmusr);
uint32_t m68k_exception_access(uint16_t ssw);

#endif
