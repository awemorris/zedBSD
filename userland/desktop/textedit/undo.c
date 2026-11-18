/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The undo history of Text Editor (plan/ws092/design.md section 7).
 *
 * Every change is a step: text inserted or deleted at a position, with the
 * cursor and the selection before and after it.  Steps of one group are
 * undone and redone together (a paste over a selection is a deletion and
 * an insertion).  Characters typed one after another, and Backspace or
 * Delete pressed again and again, grow the step before instead of adding
 * one, so that Undo takes back a word or a run of deletions at once.  The
 * history remembers how many steps were done when the document was saved,
 * which tells whether it has unsaved changes.
 */

#include "textedit.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* How long after a keystroke the next one still joins it, in milliseconds. */
#define UNDO_JOIN_MS		1000U

/* The most steps and the most bytes of text the history keeps. */
#define UNDO_STEPS_MAX		10000U
#define UNDO_BYTES_MAX		(32UL * 1024UL * 1024UL)

/* The first room of the list of steps. */
#define UNDO_INITIAL		64U

static int undo_join(struct te_undo *undo, const struct te_undo_step *step);
static int undo_joinable(const struct te_undo *undo, const struct te_undo_step *step);
static void undo_drop_redo(struct te_undo *undo);
static void undo_trim(struct te_undo *undo);
static int undo_room(struct te_undo *undo);

/*
 * Starts an empty history, whose empty document counts as saved.
 */
void
te_undo_init(
	struct te_undo *undo)
{
	/* No steps; the empty history is the saved state. */
	memset(undo, 0, sizeof(*undo));
	undo->next_group = 1U;
	undo->saved_valid = 1;
}

/*
 * Frees the history's steps.
 */
void
te_undo_free(
	struct te_undo *undo)
{
	size_t index;

	/* Each step's text, then the list. */
	for (index = 0; index < undo->count; index++)
		free(undo->steps[index].text);
	free(undo->steps);
	memset(undo, 0, sizeof(*undo));
}

/*
 * Gives a new group number, for the steps of one change.
 */
unsigned
te_undo_group(
	struct te_undo *undo)
{
	unsigned group;

	/* The next number; 0 is never given. */
	group = undo->next_group;
	undo->next_group++;
	if (undo->next_group == 0U)
		undo->next_group = 1U;

	/* Succeeded: the group. */
	return group;
}

/*
 * Records a change that was just made: steps that could have been redone
 * are forgotten, and the change joins the step before it when its merge
 * allows (typing, Backspace, Delete).  The text is copied.
 *
 * Returns 0, or ENOMEM (the change is then not undoable).
 */
int
te_undo_record(
	struct te_undo *undo,
	const struct te_undo_step *step)
{
	struct te_undo_step *added;
	int joined;
	int error;

	/* What could have been redone is gone once something new is done. */
	undo_drop_redo(undo);

	/* A change that continues the step before grows it. */
	joined = undo_join(undo, step);
	if (joined < 0)
		return ENOMEM;
	if (joined > 0)
		return 0;

	/* Room for a new step. */
	error = undo_room(undo);
	if (error != 0)
		return error;

	/* The step, with its own copy of the text. */
	added = &undo->steps[undo->count];
	*added = *step;
	added->text = NULL;
	if (step->length != 0U) {
		added->text = malloc(step->length);
		if (added->text == NULL)
			return ENOMEM;
		memcpy(added->text, step->text, step->length);
	}

	/* The step is done, and the oldest go when the history is too large. */
	undo->count++;
	undo->done = undo->count;
	undo->bytes += step->length;
	undo_trim(undo);

	/* Succeeded: the change can be undone. */
	return 0;
}

/*
 * Takes the last group done back into the steps to redo, and gives its
 * steps [first, last) (the caller undoes them from the last to the first).
 *
 * Returns 1, or 0 when there is nothing to undo.
 */
int
te_undo_undo_range(
	struct te_undo *undo,
	size_t *first,
	size_t *last)
{
	unsigned group;
	size_t start;

	/* Nothing is done. */
	if (undo->done == 0U)
		return 0;

	/* Back over the steps of the last group. */
	group = undo->steps[undo->done - 1U].group;
	start = undo->done - 1U;
	while (start > 0U && undo->steps[start - 1U].group == group)
		start--;

	/* Succeeded: those steps are undone. */
	*first = start;
	*last = undo->done;
	undo->done = start;
	return 1;
}

/*
 * Does the next group of steps that were undone again, and gives its
 * steps [first, last) (the caller redoes them in order).
 *
 * Returns 1, or 0 when there is nothing to redo.
 */
int
te_undo_redo_range(
	struct te_undo *undo,
	size_t *first,
	size_t *last)
{
	unsigned group;
	size_t end;

	/* Nothing waits to be redone. */
	if (undo->done >= undo->count)
		return 0;

	/* Forward over the steps of the next group. */
	group = undo->steps[undo->done].group;
	end = undo->done + 1U;
	while (end < undo->count && undo->steps[end].group == group)
		end++;

	/* Succeeded: those steps are done again. */
	*first = undo->done;
	*last = end;
	undo->done = end;
	return 1;
}

/*
 * Gives a step of the history.
 */
const struct te_undo_step *
te_undo_step(
	const struct te_undo *undo,
	size_t index)
{
	/* The step at the index. */
	return &undo->steps[index];
}

/*
 * Reports whether a change can be undone.
 */
int
te_undo_can_undo(
	const struct te_undo *undo)
{
	/* Anything done. */
	if (undo->done > 0U)
		return 1;

	/* Nothing done. */
	return 0;
}

/*
 * Reports whether a change can be redone.
 */
int
te_undo_can_redo(
	const struct te_undo *undo)
{
	/* Anything undone and not replaced. */
	if (undo->done < undo->count)
		return 1;

	/* Nothing to redo. */
	return 0;
}

/*
 * Notes that the document was just saved as it is now.
 */
void
te_undo_mark_saved(
	struct te_undo *undo)
{
	/* The steps done now are the saved state. */
	undo->saved = undo->done;
	undo->saved_valid = 1;
}

/*
 * Reports whether the document differs from what was last saved.
 */
int
te_undo_modified(
	const struct te_undo *undo)
{
	/* The saved state was dropped from the history, or replaced. */
	if (!undo->saved_valid)
		return 1;

	/* Another number of steps done is another text. */
	if (undo->saved != undo->done)
		return 1;

	/* The saved text. */
	return 0;
}

/*
 * Joins a change to the step before it when it continues it; returns 1
 * when joined, 0 when not, and -1 without memory.
 */
static int
undo_join(
	struct te_undo *undo,
	const struct te_undo_step *step)
{
	struct te_undo_step *last;
	char *grown;
	int joinable;

	/* Only a change that continues the step before. */
	joinable = undo_joinable(undo, step);
	if (!joinable)
		return 0;

	/* The step's text grows by the change's. */
	last = &undo->steps[undo->count - 1U];
	grown = realloc(last->text, last->length + step->length);
	if (grown == NULL)
		return -1;
	last->text = grown;

	/* Backspace deletes before the step's text; typing and Delete add after it. */
	if (step->merge == TE_MERGE_BACKSPACE) {
		memmove(last->text + step->length, last->text, last->length);
		memcpy(last->text, step->text, step->length);
		last->position = step->position;
	} else {
		memcpy(last->text + last->length, step->text, step->length);
	}

	/* The step ends where the change leaves the cursor, at the change's time. */
	last->length += step->length;
	last->cursor_after = step->cursor_after;
	last->anchor_after = step->anchor_after;
	last->time = step->time;
	undo->bytes += step->length;

	/* Succeeded: joined. */
	return 1;
}

/* Tells whether a change continues the last step closely enough to join it. */
static int
undo_joinable(
	const struct te_undo *undo,
	const struct te_undo_step *step)
{
	const struct te_undo_step *last;
	const char *found;
	int newline;

	/* Only a change that may join, after a step still done and not saved since. */
	if (step->merge == TE_MERGE_NONE || undo->count == 0U)
		return 0;
	if (undo->saved_valid && undo->saved == undo->count)
		return 0;

	/* The same kind of keystroke, soon after. */
	last = &undo->steps[undo->count - 1U];
	if (last->merge != step->merge || last->kind != step->kind)
		return 0;
	if (step->time < last->time || step->time - last->time > UNDO_JOIN_MS)
		return 0;

	/* A newline typed starts a new step. */
	newline = 0;
	found = memchr(step->text, '\n', step->length);
	if (found != NULL)
		newline = 1;
	if (newline)
		return 0;

	/* Typing continues where the last text ended; a blank after a word starts a new step. */
	if (step->merge == TE_MERGE_TYPING) {
		if (last->position + last->length != step->position)
			return 0;
		if (step->text[0] == ' ' && last->text[last->length - 1U] != ' ')
			return 0;
		return 1;
	}

	/* Backspace deletes just before what it deleted last. */
	if (step->merge == TE_MERGE_BACKSPACE) {
		if (step->position + step->length != last->position)
			return 0;
		return 1;
	}

	/* Delete deletes at the same place again. */
	if (step->position != last->position)
		return 0;

	/* Joinable. */
	return 1;
}

/* Forgets the steps that could have been redone; a saved state among them is lost. */
static void
undo_drop_redo(
	struct te_undo *undo)
{
	size_t index;

	/* Each step past the done ones. */
	for (index = undo->done; index < undo->count; index++) {
		undo->bytes -= undo->steps[index].length;
		free(undo->steps[index].text);
	}

	/* The saved state was one of them. */
	if (undo->saved_valid && undo->saved > undo->done)
		undo->saved_valid = 0;
	undo->count = undo->done;
}

/* Drops the oldest groups while the history holds too many steps or bytes. */
static void
undo_trim(
	struct te_undo *undo)
{
	unsigned group;
	size_t end;
	size_t index;

	/* Whole groups from the front, never the step just done. */
	while ((undo->count > UNDO_STEPS_MAX || undo->bytes > UNDO_BYTES_MAX) && undo->count > 1U) {
		group = undo->steps[0].group;
		end = 1U;
		while (end < undo->count - 1U && undo->steps[end].group == group)
			end++;

		/* The group's text goes. */
		for (index = 0; index < end; index++) {
			undo->bytes -= undo->steps[index].length;
			free(undo->steps[index].text);
		}

		/* The rest move to the front; a saved state that was dropped is lost. */
		memmove(undo->steps, undo->steps + end, (undo->count - end) * sizeof(undo->steps[0]));
		undo->count -= end;
		undo->done -= end;
		if (undo->saved_valid && undo->saved < end)
			undo->saved_valid = 0;
		if (undo->saved_valid)
			undo->saved -= end;
	}
}

/* Makes room for one more step; returns 0 or ENOMEM. */
static int
undo_room(
	struct te_undo *undo)
{
	struct te_undo_step *larger;
	size_t capacity;

	/* A list with room stays. */
	if (undo->count < undo->capacity)
		return 0;

	/* Twice the room. */
	capacity = undo->capacity * 2U;
	if (capacity == 0U)
		capacity = UNDO_INITIAL;
	larger = realloc(undo->steps, capacity * sizeof(larger[0]));
	if (larger == NULL)
		return ENOMEM;

	/* Succeeded: the list has the room. */
	undo->steps = larger;
	undo->capacity = capacity;
	return 0;
}
