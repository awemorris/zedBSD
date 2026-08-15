/* MC68030 variable exception-frame parser tests. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <stdio.h>
#include <string.h>
#include "src/hal/m68k/exception.h"

#define CHECK(expr) do { if (!(expr)) { \
	fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #expr); return 1; \
} } while (0)

int
main(void)
{
	uint8_t frame[92];
	uintptr_t address;
	uint16_t ssw;

	CHECK(m68k_exception_frame_size(0x0008U) == 8U);
	CHECK(m68k_exception_frame_size(0x1008U) == 8U);
	CHECK(m68k_exception_frame_size(0x2008U) == 12U);
	CHECK(m68k_exception_frame_size(0x9008U) == 20U);
	CHECK(m68k_exception_frame_size(0xa008U) == 32U);
	CHECK(m68k_exception_frame_size(0xb008U) == 92U);
	CHECK(m68k_exception_frame_size(0x7008U) == 0U);

	memset(frame, 0, sizeof(frame));
	frame[6] = 0xa0;
	frame[7] = 0x08;
	frame[10] = 0x01;
	frame[11] = 0x40; /* data fault, read */
	frame[16] = 0x12;
	frame[17] = 0x34;
	frame[18] = 0x56;
	frame[19] = 0x78;
	CHECK(m68k_exception_fault_address(frame, 31, &address, &ssw) != 0);
	CHECK(m68k_exception_fault_address(frame, 32, &address, &ssw) == 0);
	CHECK(address == 0x12345678U && ssw == 0x0140U);
	frame[6] = 0xb0;
	CHECK(m68k_exception_fault_address(frame, 91, &address, &ssw) != 0);
	CHECK(m68k_exception_fault_address(frame, 92, &address, &ssw) == 0);

	CHECK(m68k_exception_cause(2, M68K030_MMUSR_INVALID) ==
		HAL_TRAP_CAUSE_PAGE_FAULT);
	CHECK(m68k_exception_cause(2, M68K030_MMUSR_BUS_ERROR) ==
		HAL_TRAP_CAUSE_BUS);
	CHECK(m68k_exception_cause(3, 0) == HAL_TRAP_CAUSE_ALIGNMENT);
	CHECK(m68k_exception_cause(4, 0) == HAL_TRAP_CAUSE_ILLEGAL_INSN);
	CHECK(m68k_exception_cause(5, 0) == HAL_TRAP_CAUSE_ARITHMETIC);
	CHECK(m68k_exception_cause(32, 0) == HAL_TRAP_CAUSE_SYSCALL);
	CHECK(m68k_exception_cause(47, 0) == HAL_TRAP_CAUSE_BREAKPOINT);
	CHECK(m68k_exception_access(0) == HAL_TRAP_MODE_EXEC);
	CHECK(m68k_exception_access(M68K_EXCEPTION_SSW_DF |
		M68K_EXCEPTION_SSW_RW) == HAL_TRAP_MODE_READ);
	CHECK(m68k_exception_access(M68K_EXCEPTION_SSW_DF) ==
		HAL_TRAP_MODE_WRITE);

	puts("m68k exception host tests passed");
	return 0;
}
