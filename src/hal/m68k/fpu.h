/* MC68881/MC68882 complete task state. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#ifndef ZEDBSD_HAL_M68K_FPU_H
#define ZEDBSD_HAL_M68K_FPU_H

#include <hal/types.h>

#define M68K6888X_INTERNAL_MAX 216U
#define M68K6888X_FP_REG_BYTES 96U

struct m68k6888x_state {
	uint8_t internal[M68K6888X_INTERNAL_MAX] __attribute__((aligned(16)));
	uint32_t fp_registers[8U * 3U];
	uint32_t fpcr;
	uint32_t fpsr;
	uint32_t fpiar;
	uint16_t internal_size;
	uint8_t initialized;
	uint8_t reserved;
} __attribute__((aligned(16)));

size_t m68k6888x_frame_size(const void *frame, size_t available);
void m68k6888x_state_init(struct m68k6888x_state *state);
int m68k6888x_save(struct m68k6888x_state *state);
int m68k6888x_restore(const struct m68k6888x_state *state);
int m68k6888x_probe(void);

uint32_t m68k6888x_save_raw(void *internal, void *fp_registers,
	uint32_t *controls);
void m68k6888x_restore_raw(const void *internal, const void *fp_registers,
	const uint32_t *controls);
int m68k6888x_probe_raw(void);

#endif
