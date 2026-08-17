/* Pure MC68030 exception decoding shared by target code and host tests. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "exception.h"

static uint16_t
read_be16(const uint8_t *field)
{
	return (uint16_t)((uint16_t)field[0] << 8 | field[1]);
}

static uint32_t
read_be32(const uint8_t *field)
{
	return (uint32_t)field[0] << 24 | (uint32_t)field[1] << 16 |
		(uint32_t)field[2] << 8 | field[3];
}

size_t
m68k_exception_frame_size(uint16_t format_vector)
{
	switch (format_vector >> 12) {
	case 0x0:
	case 0x1:
		return M68K_EXCEPTION_BASIC_SIZE;
	case 0x2:
		return 12U;
	case 0x9:
		return 20U;
	case M68K_EXCEPTION_FORMAT_A:
		return 32U;
	case M68K_EXCEPTION_FORMAT_B:
		return 92U;
	default:
		return 0;
	}
}

int
m68k_exception_fault_address(const void *raw_frame, size_t available,
			     uintptr_t *address, uint16_t *ssw)
{
	const uint8_t *frame = raw_frame;
	uint16_t format_vector;
	size_t size;

	if (frame == NULL || address == NULL || ssw == NULL || available < 8U)
		return -1;
	format_vector = read_be16(frame + 6U);
	size = m68k_exception_frame_size(format_vector);
	if (size == 0 || available < size ||
	    ((format_vector >> 12) != M68K_EXCEPTION_FORMAT_A &&
	     (format_vector >> 12) != M68K_EXCEPTION_FORMAT_B))
		return -1;
	/* Formats A and B share ir0, SSW, stage C/B, and data-cycle fault
	 * address at the head of the extension (MC68030UM exception frames). */
	*ssw = read_be16(frame + 10U);
	*address = read_be32(frame + 16U);
	return 0;
}

uint32_t
m68k_exception_cause(unsigned vector, uint16_t mmusr)
{
	if (vector == 2U) {
		if ((mmusr & M68K030_MMUSR_BUS_ERROR) == 0 &&
		    (mmusr & (M68K030_MMUSR_LIMIT |
		     M68K030_MMUSR_SUPERVISOR | M68K030_MMUSR_ACCESS |
		     M68K030_MMUSR_WRITE_PROT | M68K030_MMUSR_INVALID)) != 0)
			return HAL_TRAP_CAUSE_PAGE_FAULT;
		return HAL_TRAP_CAUSE_MACHINE_CHECK;
	}
	if (vector == 3U)
		return HAL_TRAP_CAUSE_ALIGNMENT;
	if (vector == 4U || vector == 8U || vector == 11U || vector == 13U)
		return HAL_TRAP_CAUSE_ILLEGAL_INSN;
	if (vector == 5U || vector == 6U || vector == 7U ||
	    (vector >= 48U && vector <= 55U))
		return HAL_TRAP_CAUSE_MACHINE_CHECK;
	if (vector == 9U || vector == 47U)
		return HAL_TRAP_CAUSE_BREAKPOINT;
	return HAL_TRAP_CAUSE_MACHINE_CHECK;
}

uint32_t
m68k_exception_access(uint16_t ssw)
{
	if ((ssw & M68K_EXCEPTION_SSW_DF) == 0)
		return HAL_TRAP_MODE_EXEC;
	return (ssw & M68K_EXCEPTION_SSW_RW) != 0 ? HAL_TRAP_MODE_READ :
		HAL_TRAP_MODE_WRITE;
}
