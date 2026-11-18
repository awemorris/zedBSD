/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The history of changes behind u and CONTROL-R, and the one door through
 * which every edit reaches the buffer.
 */

#include "userland/base/vi/vi.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static void group_free(struct vi_undo_group *group);
static int group_reserve(struct vi_undo_group *group);
static int groups_reserve(struct vi_undo *undo);
static void forget_oldest(struct vi_undo *undo);
static unsigned long state_serial(const struct vi_undo *undo);
static size_t group_bytes(const struct vi_undo_group *group);
static int capture_text(const struct vi_editor *editor, struct vi_change *change);
static char *lines_text(const struct vi_buffer *buffer, size_t first, size_t count, size_t *length);
static int over_limit(const struct vi_editor *editor, enum vi_change_kind kind, size_t length);
static int can_merge_insert(const struct vi_undo_group *group, const struct vi_change *change);

/*
 * Prepares an empty history.
 */
void
vi_undo_init(
	struct vi_undo *undo)
{
	/* No change made, and the buffer, in state 0, is as it was read. */
	memset(undo, 0, sizeof(*undo));
	undo->next_serial = 1;
}

/*
 * Releases every change the history holds.
 */
void
vi_undo_free(
	struct vi_undo *undo)
{
	size_t index;

	/* Releases each kept group, then the group being filled. */
	for (index = 0; index < undo->count; index++)
		group_free(&undo->groups[index]);
	group_free(&undo->open);

	/* Releases the group table and starts over as unchanged. */
	free(undo->groups);
	vi_undo_init(undo);
}

/*
 * Starts the group that collects the changes of the next command.
 *
 * The cursor at this moment is where u later puts it back.
 */
int
vi_undo_begin(
	struct vi_editor *editor)
{
	struct vi_undo *undo;

	/* Finds the history of the session. */
	undo = &editor->undo;

	/* A group already open keeps collecting. */
	if (undo->open_active)
		return 0;

	/* Opens an empty group that remembers where the command started. */
	memset(&undo->open, 0, sizeof(undo->open));
	undo->open.cursor = editor->cursor;
	undo->open_active = 1;

	/* Succeeded. */
	return 0;
}

/*
 * Closes the group of the command that just ended and keeps it for u.
 *
 * A group without changes is dropped.  Keeping a new group forgets the
 * changes that u took back, as CONTROL-R can no longer reach them.
 */
void
vi_undo_end(
	struct vi_editor *editor)
{
	struct vi_undo *undo;
	size_t index;
	int error;

	/* Finds the history of the session. */
	undo = &editor->undo;

	/* Nothing to close between commands. */
	if (!undo->open_active)
		return;

	/* The group is closed whether it is kept or dropped. */
	undo->open_active = 0;

	/* A command that changed nothing leaves no trace. */
	if (undo->open.count == 0) {
		group_free(&undo->open);
		return;
	}

	/* The new state gets a name no other state ever had. */
	undo->open.serial = undo->next_serial;
	undo->next_serial++;

	/* Forgets the changes that were taken back. */
	for (index = undo->current; index < undo->count; index++) {
		undo->bytes -= group_bytes(&undo->groups[index]);
		group_free(&undo->groups[index]);
	}

	/* Shortens the history to the groups that are still applied. */
	undo->count = undo->current;

	/* Keeps the history within its limit by forgetting the oldest change. */
	if (undo->count >= VI_UNDO_GROUP_LIMIT)
		forget_oldest(undo);

	/* Makes room for the group in the table. */
	error = groups_reserve(undo);
	if (error != 0) {
		/*
		 * Without the group the older ones no longer describe the
		 * buffer, so the whole history is given up.
		 */
		undo->base_serial = undo->open.serial;
		group_free(&undo->open);
		for (index = 0; index < undo->count; index++)
			group_free(&undo->groups[index]);
		undo->count = 0;
		undo->current = 0;
		undo->bytes = 0;
		return;
	}

	/* Keeps the group as the newest change. */
	undo->bytes += group_bytes(&undo->open);
	undo->groups[undo->count] = undo->open;
	undo->count++;
	undo->current = undo->count;
	memset(&undo->open, 0, sizeof(undo->open));

	/* Forgets the oldest changes while the history holds too much text; the newest stays. */
	while (undo->bytes > VI_UNDO_BYTE_LIMIT) {
		if (undo->count <= 1)
			break;
		forget_oldest(undo);
	}
}

/*
 * Records that the buffer as it is now is what the file holds.
 */
void
vi_undo_mark_saved(
	struct vi_editor *editor)
{
	/* The written state is the one the buffer is in now. */
	editor->undo.saved_serial = state_serial(&editor->undo);
}

/*
 * Tells whether the buffer differs from what was last read or written.
 */
int
vi_undo_modified(
	const struct vi_editor *editor)
{
	const struct vi_undo *undo;
	unsigned long state;

	/* Finds the history of the session. */
	undo = &editor->undo;

	/* The running command has already changed the buffer. */
	if (undo->open_active && undo->open.count != 0)
		return 1;

	/* The buffer is in another state than the one read or written. */
	state = state_serial(undo);
	if (state != undo->saved_serial)
		return 1;

	/* Unchanged: the buffer is the written state. */
	return 0;
}

/*
 * Takes back the newest change still applied, which is what u does.
 *
 * Returns 0, ENOENT when there is nothing left to take back, or an errno
 * value when the buffer could not be restored.
 */
int
vi_undo_back(
	struct vi_editor *editor)
{
	struct vi_undo *undo;
	struct vi_undo_group *group;
	size_t index;
	int error;

	/* Finds the history of the session. */
	undo = &editor->undo;

	/* Nothing applied is left to take back. */
	if (undo->current == 0)
		return ENOENT;

	/* Takes back the changes of the group, the last one first. */
	group = &undo->groups[undo->current - 1];
	for (index = group->count; index > 0; index--) {
		error = vi_buffer_apply(&editor->buffer, &group->changes[index - 1], 1);
		if (error != 0)
			return error;
	}

	/* Moves to the older state and the place the command started. */
	undo->current--;
	editor->cursor = group->cursor;

	/* Succeeded: the buffer is as it was before the change. */
	return 0;
}

/*
 * Makes again the newest change u took back, which is what CONTROL-R does.
 *
 * Returns 0, ENOENT when nothing was taken back, or an errno value when
 * the buffer could not take the change.
 */
int
vi_undo_forward(
	struct vi_editor *editor)
{
	struct vi_undo *undo;
	struct vi_undo_group *group;
	size_t index;
	int error;

	/* Finds the history of the session. */
	undo = &editor->undo;

	/* Nothing was taken back. */
	if (undo->current == undo->count)
		return ENOENT;

	/* Makes the changes of the group again, in their first order. */
	group = &undo->groups[undo->current];
	for (index = 0; index < group->count; index++) {
		error = vi_buffer_apply(&editor->buffer, &group->changes[index], 0);
		if (error != 0)
			return error;
	}

	/* Moves to the newer state and the place the command started. */
	undo->current++;
	editor->cursor = group->cursor;

	/* Succeeded: the buffer shows the change again. */
	return 0;
}

/*
 * Makes one change to the buffer and records it in the running command.
 *
 * For an insertion of text the text is what goes in; for an insertion of
 * lines it is the lines, each ended by a newline.  For a deletion of text
 * the length is how many bytes go, and for a deletion of lines how many
 * lines go; a split and a join use neither.  An insertion that would take
 * the buffer past the file size limit is refused with EFBIG.  Returns 0,
 * or an errno value with the buffer and the history unchanged.
 */
int
vi_edit(
	struct vi_editor *editor,
	enum vi_change_kind kind,
	size_t line,
	size_t offset,
	const char *text,
	size_t length)
{
	struct vi_change change;
	struct vi_undo_group *group;
	struct vi_change *last;
	char *grown;
	int merged;
	int too_large;
	int error;

	/* Refuses a change that would take the buffer past the file size limit. */
	too_large = over_limit(editor, kind, length);
	if (too_large)
		return EFBIG;

	/* Makes sure the change has a group to go into. */
	error = vi_undo_begin(editor);
	if (error != 0)
		return error;

	/* Finds the group the change goes into; no earlier record is lengthened yet. */
	group = &editor->undo.open;
	last = NULL;

	/* Describes the change. */
	memset(&change, 0, sizeof(change));
	change.kind = kind;
	change.line = line;
	change.offset = offset;
	change.text = (char *)text;
	change.length = length;

	/* Typing extends the previous insertion instead of adding a change per key. */
	merged = 0;
	if (kind == VI_CHANGE_INSERT_TEXT)
		merged = can_merge_insert(group, &change);

	/* Makes room for the record before the buffer changes. */
	if (merged) {
		/* The previous insertion grows to take the new bytes. */
		last = &group->changes[group->count - 1];
		grown = realloc(last->text, last->length + length + 1);
		if (grown == NULL)
			return ENOMEM;

		/* Keeps the larger storage as the record's text. */
		last->text = grown;
	} else {
		/* A new record keeps its own copy of the text. */
		error = capture_text(editor, &change);
		if (error != 0)
			return error;

		/* Makes room for the record in the group, and gives back the copy if there is none. */
		error = group_reserve(group);
		if (error != 0) {
			free(change.text);
			return error;
		}
	}

	/* Makes the change itself. */
	error = vi_buffer_apply(&editor->buffer, &change, 0);
	if (error != 0) {
		/* A change the buffer refused is not recorded. */
		if (!merged)
			free(change.text);
		return error;
	}

	/* Records the change for u. */
	if (merged) {
		memcpy(last->text + last->length, text, length);
		last->length += length;
		last->text[last->length] = '\0';
	} else {
		group->changes[group->count] = change;
		group->count++;
	}

	/* Tells the main loop that the command changed the buffer, for the . command. */
	editor->changed = 1;

	/* Succeeded. */
	return 0;
}

/* Releases the changes of one group. */
static void
group_free(
	struct vi_undo_group *group)
{
	size_t index;

	/* Releases the text kept by each change. */
	for (index = 0; index < group->count; index++)
		free(group->changes[index].text);

	/* Releases the change table. */
	free(group->changes);
	memset(group, 0, sizeof(*group));
}

/* Makes room for one more change in a group. */
static int
group_reserve(
	struct vi_undo_group *group)
{
	struct vi_change *changes;
	size_t capacity;

	/* A group with a free slot needs nothing. */
	if (group->count < group->capacity)
		return 0;

	/* Doubles the table, starting small since most commands make one change. */
	capacity = group->capacity * 2;
	if (capacity == 0)
		capacity = 4;

	/* Moves the table into the larger storage. */
	changes = realloc(group->changes, capacity * sizeof(*changes));
	if (changes == NULL)
		return ENOMEM;

	/* Keeps the larger table as the group's own. */
	group->changes = changes;
	group->capacity = capacity;

	/* Succeeded. */
	return 0;
}

/* Makes room for one more group in the history. */
static int
groups_reserve(
	struct vi_undo *undo)
{
	struct vi_undo_group *groups;
	size_t capacity;

	/* A history with a free slot needs nothing. */
	if (undo->count < undo->capacity)
		return 0;

	/* Doubles the table up to the limit of the history. */
	capacity = undo->capacity * 2;
	if (capacity == 0)
		capacity = 16;
	if (capacity > VI_UNDO_GROUP_LIMIT)
		capacity = VI_UNDO_GROUP_LIMIT;

	/* Moves the table into the larger storage. */
	groups = realloc(undo->groups, capacity * sizeof(*groups));
	if (groups == NULL)
		return ENOMEM;

	/* Keeps the larger table as the history's own. */
	undo->groups = groups;
	undo->capacity = capacity;

	/* Succeeded. */
	return 0;
}

/* Forgets the oldest group, which u can then no longer reach. */
static void
forget_oldest(
	struct vi_undo *undo)
{
	/*
	 * The state after the oldest group becomes the base; the state before
	 * it can no longer be reached, so a file written in it now counts as
	 * changed for good.
	 */
	undo->base_serial = undo->groups[0].serial;

	/* Releases the oldest group and closes its slot. */
	undo->bytes -= group_bytes(&undo->groups[0]);
	group_free(&undo->groups[0]);
	memmove(&undo->groups[0], &undo->groups[1], (undo->count - 1) * sizeof(undo->groups[0]));
	undo->count--;
	undo->current--;
}

/* Gives the name of the state the buffer is in. */
static unsigned long
state_serial(
	const struct vi_undo *undo)
{
	/* No applied group: the base state. */
	if (undo->current == 0)
		return undo->base_serial;

	/* Otherwise the state after the last applied group. */
	return undo->groups[undo->current - 1].serial;
}

/* Gives the bytes of text a group keeps. */
static size_t
group_bytes(
	const struct vi_undo_group *group)
{
	size_t total;
	size_t index;

	/* Adds up the text of each change. */
	total = 0;
	for (index = 0; index < group->count; index++)
		total += group->changes[index].length;

	/* Reports the bytes the group keeps. */
	return total;
}

/*
 * Copies the text a change needs to be taken back or made again.
 *
 * An insertion copies the text it is given.  A deletion copies what the
 * buffer holds there now.  A join records the length of the first line,
 * where u later splits it again.
 */
static int
capture_text(
	const struct vi_editor *editor,
	struct vi_change *change)
{
	const struct vi_line *line;
	const char *source;
	char *copy;
	size_t index;

	/* Refuses a change on a line the buffer does not have. */
	if (change->line >= editor->buffer.count) {
		if (change->kind != VI_CHANGE_INSERT_LINE)
			return EINVAL;
	}

	/* Chooses the text that is kept with the change. */
	source = change->text;
	change->text = NULL;
	switch (change->kind) {
	case VI_CHANGE_INSERT_TEXT:
		break;
	case VI_CHANGE_INSERT_LINE:
		/* The lines are counted by their newlines. */
		change->count = 0;
		for (index = 0; index < change->length; index++) {
			if (source[index] == '\n')
				change->count++;
		}

		break;
	case VI_CHANGE_DELETE_TEXT:
		/* The deleted bytes are copied from the line, which must hold them. */
		line = &editor->buffer.lines[change->line];
		if (change->offset > line->length)
			return EINVAL;
		if (change->length > line->length - change->offset)
			return EINVAL;
		source = line->text + change->offset;
		break;
	case VI_CHANGE_DELETE_LINE:
		/* The caller gives the number of lines; the copy is the lines, each with its newline. */
		change->count = change->length;

		/* Refuses a count of no lines, or of more lines than the buffer holds from there. */
		if (change->count == 0)
			return EINVAL;
		if (change->count > editor->buffer.count - change->line)
			return EINVAL;

		/* Copies the lines that go. */
		copy = lines_text(&editor->buffer, change->line, change->count, &change->length);
		if (copy == NULL)
			return ENOMEM;
		change->text = copy;
		return 0;
	case VI_CHANGE_JOIN_LINE:
		change->offset = editor->buffer.lines[change->line].length;
		change->length = 0;
		return 0;
	case VI_CHANGE_SPLIT_LINE:
		change->length = 0;
		return 0;
	}

	/* Copies the text, with room for at least one byte so the copy is never empty. */
	copy = malloc(change->length + 1);
	if (copy == NULL)
		return ENOMEM;

	/* Fills the copy and ends it with a NUL. */
	if (change->length != 0)
		memcpy(copy, source, change->length);
	copy[change->length] = '\0';
	change->text = copy;

	/* Succeeded: the change owns its copy of the text. */
	return 0;
}

/*
 * Tells whether an insertion goes right after the previous insertion of
 * the group, on the same line, so that the two can be one change.
 */
static int
can_merge_insert(
	const struct vi_undo_group *group,
	const struct vi_change *change)
{
	const struct vi_change *last;

	/* An empty group has nothing to lengthen. */
	if (group->count == 0)
		return 0;

	/* Only an insertion on the same line, ending where this one starts, is lengthened. */
	last = &group->changes[group->count - 1];
	if (last->kind != VI_CHANGE_INSERT_TEXT)
		return 0;
	if (last->line != change->line)
		return 0;
	if (last->offset + last->length != change->offset)
		return 0;

	/* The two insertions meet. */
	return 1;
}

/* Copies count lines from first on, each with a newline after it. */
static char *
lines_text(
	const struct vi_buffer *buffer,
	size_t first,
	size_t count,
	size_t *length)
{
	const struct vi_line *line;
	size_t index;
	size_t size;
	char *text;

	/* Measures the lines and their newlines. */
	size = 0;
	for (index = first; index < first + count; index++)
		size += buffer->lines[index].length + 1;

	/* Allocates the copy. */
	text = malloc(size + 1);
	if (text == NULL)
		return NULL;

	/* Copies each line and its newline. */
	size = 0;
	for (index = first; index < first + count; index++) {
		line = &buffer->lines[index];
		if (line->length != 0)
			memcpy(text + size, line->text, line->length);
		size += line->length;
		text[size] = '\n';
		size++;
	}

	/* Ends the copy with a NUL and tells the caller its length. */
	text[size] = '\0';
	*length = size;

	/* Succeeded: the caller owns the copy. */
	return text;
}

/*
 * Tells whether a change would make the buffer larger than a file vi
 * opens, so that vi never writes a file it could not read back.
 */
static int
over_limit(
	const struct vi_editor *editor,
	enum vi_change_kind kind,
	size_t length)
{
	size_t added;

	/* Only insertions and splits make the buffer larger. */
	added = 0;
	if (kind == VI_CHANGE_INSERT_TEXT)
		added = length;
	else if (kind == VI_CHANGE_INSERT_LINE)
		added = length;
	else if (kind == VI_CHANGE_SPLIT_LINE)
		added = 1;

	/* Compares the size after the change with the limit. */
	if (added > VI_FILE_SIZE_LIMIT)
		return 1;
	if (editor->buffer.bytes > VI_FILE_SIZE_LIMIT - added)
		return 1;

	/* Succeeded: the change fits. */
	return 0;
}
