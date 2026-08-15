/* MC68881/MC68882 state-frame validation and state layout tests. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <stdio.h>
#include <string.h>
#include "src/hal/m68k/fpu.h"

#define CHECK(expr) do { if (!(expr)) { \
	fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #expr); return 1; \
} } while (0)

void *hal_memset(void *p, int c, size_t n) { return memset(p, c, n); }
uint32_t m68k6888x_save_raw(void *a, void *b, uint32_t *c)
{ (void)a; (void)b; (void)c; return 0; }
void m68k6888x_restore_raw(const void *a, const void *b, const uint32_t *c)
{ (void)a; (void)b; (void)c; }
int m68k6888x_probe_raw(void) { return 1; }

int
main(void)
{
	struct m68k6888x_state state;
	uint8_t frame[M68K6888X_INTERNAL_MAX];
	static const unsigned totals[] = { 28U, 60U, 184U, 216U };
	unsigned index;

	CHECK(sizeof(state.internal) == 216U);
	CHECK(__alignof__(state) >= 16U);
	CHECK(((uintptr_t)&state.internal[0] & 15U) == 0);
	m68k6888x_state_init(&state);
	CHECK(state.initialized == 1U && state.internal_size == 4U);
	CHECK(m68k6888x_frame_size(state.internal, 4U) == 4U);
	CHECK(m68k6888x_frame_size(state.internal, 3U) == 0U);

	for (index = 0; index < sizeof(totals) / sizeof(totals[0]); index++) {
		memset(frame, 0, sizeof(frame));
		frame[0] = 0x1f;
		frame[1] = (uint8_t)(totals[index] - 4U);
		CHECK(m68k6888x_frame_size(frame, totals[index] - 1U) == 0U);
		CHECK(m68k6888x_frame_size(frame, totals[index]) == totals[index]);
	}
	frame[1] = 1U;
	CHECK(m68k6888x_frame_size(frame, sizeof(frame)) == 0U);

	puts("m68k FPU frame host tests passed");
	return 0;
}
