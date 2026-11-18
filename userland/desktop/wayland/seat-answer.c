/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The answers the compositor owes the seat, kept until the input thread's
 * acknowledgements make them due (seat-answer.h).
 */

#include "seat-answer.h"

#include <errno.h>
#include <string.h>

/*
 * Empties the record: nothing owed, nothing asked.
 */
void
kwl_seat_answers_init(
	struct kwl_seat_answers *answers)
{
	/* Every field zero. */
	memset(answers, 0, sizeof(*answers));
}

/*
 * Notes a detach asked of the input thread (numbered sequence): the wait
 * for an acknowledgement starts now when none was awaited.
 */
void
kwl_seat_answers_asked(
	struct kwl_seat_answers *answers,
	uint64_t sequence,
	uint64_t now_ms)
{
	/* A wait begins with the first detach not acknowledged. */
	if (answers->asked <= answers->acked)
		answers->waiting_since_ms = now_ms;

	/* The sequence only grows. */
	if (sequence > answers->asked)
		answers->asked = sequence;
}

/*
 * Notes the input thread's last acknowledgement (acked): the answers up to
 * it become due, and the wait for the rest starts again now.
 */
void
kwl_seat_answers_heard(
	struct kwl_seat_answers *answers,
	uint64_t acked,
	uint64_t now_ms)
{
	/* Nothing new acknowledged leaves the wait as it is. */
	if (acked <= answers->acked)
		return;
	answers->acked = acked;

	/* The input thread answers: the next acknowledgement is awaited from now, or none is. */
	answers->waiting_since_ms = 0U;
	if (answers->asked > answers->acked)
		answers->waiting_since_ms = now_ms;
}

/*
 * Tells whether the detach numbered sequence is not acknowledged yet as the
 * main thread last looked (1), so the input thread may still read what it
 * stops.
 */
int
kwl_seat_answers_pending(
	const struct kwl_seat_answers *answers,
	uint64_t sequence)
{
	/* An acknowledgement later than it covers it. */
	if (sequence > answers->acked)
		return 1;
	return 0;
}

/*
 * Owes the seat an answer for descriptor once the detach numbered sequence
 * is acknowledged.  Returns 0, or ENOSPC when too many are owed (the caller
 * then waits for the acknowledgement itself).
 */
int
kwl_seat_answers_add(
	struct kwl_seat_answers *answers,
	uint64_t sequence,
	int descriptor)
{
	struct kwl_seat_answer *answer;

	/* A full record takes no more. */
	if (answers->count >= KWL_SEAT_ANSWERS_MAX)
		return ENOSPC;

	/*
	 * After every answer owed before: an answer's sequence is never below
	 * the one before it, as each callback asks no earlier detach.
	 */
	answer = &answers->list[answers->count];
	answer->sequence = sequence;
	answer->descriptor = descriptor;
	answers->count++;

	/* Succeeded: owed. */
	return 0;
}

/*
 * Takes the oldest answer owed when its detach is acknowledged: returns 1
 * with its descriptor, 0 when none is due yet.
 */
int
kwl_seat_answers_next(
	struct kwl_seat_answers *answers,
	int *descriptor)
{
	unsigned index;

	/* Only the oldest, so they leave in the callbacks' order. */
	if (answers->count == 0U)
		return 0;
	if (answers->list[0].sequence > answers->acked)
		return 0;
	*descriptor = answers->list[0].descriptor;

	/* The rest move up. */
	answers->count--;
	for (index = 0; index < answers->count; index++)
		answers->list[index] = answers->list[index + 1U];

	/* Succeeded: one answer due. */
	return 1;
}

/*
 * Tells whether an acknowledgement has been awaited longer than limit_ms
 * (1, with how long in waited_ms).
 */
int
kwl_seat_answers_late(
	const struct kwl_seat_answers *answers,
	uint64_t now_ms,
	uint64_t limit_ms,
	uint64_t *waited_ms)
{
	uint64_t waited;

	/* Nothing awaited is never late. */
	*waited_ms = 0U;
	if (answers->asked <= answers->acked || now_ms < answers->waiting_since_ms)
		return 0;
	waited = now_ms - answers->waiting_since_ms;
	*waited_ms = waited;

	/* Late past the limit. */
	if (waited > limit_ms)
		return 1;
	return 0;
}
