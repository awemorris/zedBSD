/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The output an engine fills for each key (plan/ws095/design.md section 6).
 */

#include "engine.h"

#include <errno.h>
#include <string.h>

/*
 * Empties an output: nothing committed, no preedit, no candidates, and
 * the key used.
 */
void
ime_output_clear(
	struct ime_output *out)
{
	/* Every field back to nothing. */
	out->commit_length = 0;
	out->commit[0] = '\0';
	out->preedit_length = 0;
	out->preedit[0] = '\0';
	out->cursor_begin = 0;
	out->cursor_end = 0;
	out->candidate_count = 0;
	out->candidate_selected = 0;
	out->candidates_shown = false;
	out->composing = false;
	out->pass_key = false;
}

/*
 * Adds text to what an output commits.
 *
 * Returns 0, or ENOSPC when it would pass the length one message carries;
 * nothing is added then.
 */
int
ime_output_append_commit(
	struct ime_output *out,
	const char *text,
	size_t length)
{
	/* The text must fit with the terminator. */
	if (out->commit_length + length >= IME_TEXT_MAX)
		return ENOSPC;

	/* Succeeded: the text follows what was committed before. */
	memcpy(out->commit + out->commit_length, text, length);
	out->commit_length += length;
	out->commit[out->commit_length] = '\0';
	return 0;
}
