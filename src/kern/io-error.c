/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Persistent failure observations independent of dirty-data retry state.
 */

#include <kern/io-error.h>
#include <hal/hal.h>

static bool io_error_lock(struct io_error_state *state);
static void io_error_unlock(struct io_error_state *state, bool enabled);

/*
 * Records a new actual writeback failure without clearing earlier observations.
 */
void
io_error_record(
	struct io_error_state *state,
	int error)
{
	bool enabled;

	/* A successful retry changes dirty ownership, not historical notification. */
	if (state == NULL || error == 0)
		return;
	enabled = io_error_lock(state);
	if (state->sequence != UINT64_MAX)
		state->sequence++;
	state->error = error;
	io_error_unlock(state, enabled);
}

/*
 * Captures one matching sequence and errno before an observer advances its cursor.
 */
void
io_error_snapshot(
	struct io_error_state *state,
	struct io_error_snapshot *snapshot)
{
	bool enabled;

	/* Supports owners without a ledger as an empty observation. */
	snapshot->sequence = 0;
	snapshot->error = 0;
	if (state == NULL)
		return;
	enabled = io_error_lock(state);
	snapshot->sequence = state->sequence;
	snapshot->error = state->error;
	io_error_unlock(state, enabled);
}

/*
 * Reports a captured failure once per observer without consuming a later event.
 */
int
io_error_observe(
	const struct io_error_snapshot *snapshot,
	volatile uint64_t *cursor)
{
	uint64_t current;

	/* Empty snapshots have nothing to acknowledge. */
	if (snapshot->sequence == 0 || snapshot->error == 0)
		return 0;

	/* Saturation cannot distinguish new events, so it stays conservatively sticky. */
	if (snapshot->sequence == UINT64_MAX) {
		atomic_u64_store_release(cursor, UINT64_MAX);
		return snapshot->error;
	}

	/* Advances only to this snapshot, preserving concurrently observed newer ones. */
	current = atomic_u64_load_acquire(cursor);
	while (current < snapshot->sequence) {
		if (atomic_u64_compare_exchange(cursor, &current, snapshot->sequence))
			return snapshot->error;
	}
	return 0;
}

/* Guards the paired sequence and errno without sleeping or entering another owner. */
static bool
io_error_lock(
	struct io_error_state *state)
{
	bool enabled;
	unsigned expected;

	/* Keeps completion-context writers from interrupting an owner of this guard. */
	enabled = hal_irq_disable();
	expected = 0;
	while (!atomic_compare_exchange(&state->guard, &expected, 1)) {
		expected = 0;
		hal_compiler_barrier();
	}
	return enabled;
}

/* Publishes a coherent pair before restoring the caller's interrupt state. */
static void
io_error_unlock(
	struct io_error_state *state,
	bool enabled)
{
	atomic_store_release(&state->guard, 0);
	if (enabled)
		hal_irq_enable();
}
