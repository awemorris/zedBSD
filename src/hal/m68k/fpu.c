/* MC68881/MC68882 state validation and C-side ownership. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <hal/hal.h>
#include "fpu.h"

_Static_assert(sizeof(((struct m68k6888x_state *)0)->internal) >= 216U,
	"MC68882 busy frame requires 216 bytes");
_Static_assert(__alignof__(struct m68k6888x_state) >= 16U,
	"FPU task state must be 16-byte aligned");

int
m68k6888x_probe(void)
{
	struct m68k6888x_state state;
	int result;

	if (!m68k6888x_probe_raw())
		return -1;
	m68k6888x_state_init(&state);
	/* FSAVE validates that the installed coprocessor produces one of the
	 * documented 68881/68882 state-frame lengths accepted below. */
	result = m68k6888x_save(&state);
	if (result != 0)
		return result;
	return m68k6888x_restore(&state);
}

size_t
m68k6888x_frame_size(const void *raw_frame, size_t available)
{
	const uint8_t *frame = raw_frame;
	size_t total;

	if (frame == NULL || available < 4U)
		return 0;
	if (frame[0] == 0 && frame[1] == 0 && frame[2] == 0 && frame[3] == 0)
		return 4U;
	total = (size_t)frame[1] + 4U;
	if (total > available ||
	    (total != 28U && total != 60U &&
	     total != 184U && total != 216U))
		return 0;
	return total;
}

void
m68k6888x_state_init(struct m68k6888x_state *state)
{
	if (state == NULL)
		return;
	hal_memset(state, 0, sizeof(*state));
	state->internal_size = 4U;
	state->initialized = 1U;
}

int
m68k6888x_save(struct m68k6888x_state *state)
{
	uint32_t controls[3];
	uint32_t size;

	if (state == NULL)
		return -1;
	size = m68k6888x_save_raw(state->internal, state->fp_registers,
		controls);
	if (size > M68K6888X_INTERNAL_MAX ||
	    m68k6888x_frame_size(state->internal, size) != size)
		return -1;
	state->fpcr = controls[0];
	state->fpsr = controls[1];
	state->fpiar = controls[2];
	state->internal_size = (uint16_t)size;
	state->initialized = 1U;
	return 0;
}

int
m68k6888x_restore(const struct m68k6888x_state *state)
{
	uint32_t controls[3];

	if (state == NULL || state->initialized == 0 ||
	    m68k6888x_frame_size(state->internal, state->internal_size) !=
	    state->internal_size)
		return -1;
	controls[0] = state->fpcr;
	controls[1] = state->fpsr;
	controls[2] = state->fpiar;
	m68k6888x_restore_raw(state->internal, state->fp_registers, controls);
	return 0;
}
