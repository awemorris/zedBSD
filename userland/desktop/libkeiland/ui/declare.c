/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The model of a window's declarative menus and controls (WS131 p015,
 * declare.h): a table compared with the one shown, and the operations
 * that bring the compositor's copy to it.  A change that sends nothing
 * sends no transaction either.
 */

#include "declare.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/*
 * One change being sent: the sink and its data, whether the transaction
 * was begun, and the first refusal (the rest of the change is not sent,
 * but the transaction is still ended).
 */
struct declare_change {
	keiui_declare_sink sink;
	void *data;
	int begun;
	int error;
};

static int declare_make(struct keiui_declare_item **made, size_t count);
static void declare_free(struct keiui_declare_item *items, size_t count);
static int declare_structure(const struct keiui_declare_model *model, const struct keiui_declare_item *fresh, size_t count);
static void declare_send(struct declare_change *change, unsigned operation, const struct keiui_declare_item *item);
static void declare_rebuild(struct keiui_declare_model *model, const struct keiui_declare_item *fresh, size_t count, struct declare_change *change);
static void declare_update(struct keiui_declare_model *model, const struct keiui_declare_item *fresh, size_t count, struct declare_change *change);
static int declare_finish(struct keiui_declare_model *model, struct keiui_declare_item *fresh, size_t count, struct declare_change *change);
static int declare_is_parent(const struct keiui_declare_item *items, size_t count, uint32_t id);

/*
 * Makes an empty model of a kind (KEIUI_DECLARE_MENU or _CONTROLS).
 */
void
keiui_declare_init(
	struct keiui_declare_model *model,
	unsigned kind)
{
	/* Nothing shown yet. */
	memset(model, 0, sizeof(*model));
	model->kind = kind;
}

/*
 * Forgets a model's entries.
 */
void
keiui_declare_fini(
	struct keiui_declare_model *model)
{
	/* The entries and their labels, and nothing shown any more. */
	declare_free(model->items, model->count);
	model->items = NULL;
	model->count = 0;
	model->built = 0;
}

/*
 * Makes an empty table of action states.
 */
void
keiui_declare_states_init(
	struct keiui_declare_states *states)
{
	/* No action named: all enabled, unchecked and shown. */
	memset(states, 0, sizeof(*states));
}

/*
 * Reports the state of an action (0 for one never named, and always for
 * action 0, which is no action).
 */
unsigned
keiui_declare_state_of(
	const struct keiui_declare_states *states,
	uint32_t action)
{
	size_t index;

	/* No action. */
	if (action == 0U)
		return 0;

	/* The action's state when it was named. */
	for (index = 0; index < states->count; index++) {
		if (states->actions[index] == action)
			return states->states[index];
	}

	/* Never named: the default. */
	return 0;
}

/*
 * Sets the state of an action.  Returns 0, EINVAL for action 0, or ENOSPC
 * when the table is full.
 */
int
keiui_declare_set_state(
	struct keiui_declare_states *states,
	uint32_t action,
	unsigned state)
{
	size_t index;

	/* Action 0 is no action. */
	if (action == 0U)
		return EINVAL;

	/* An action named before takes its new state. */
	for (index = 0; index < states->count; index++) {
		if (states->actions[index] == action) {
			states->states[index] = state;
			return 0;
		}
	}

	/* A new one, while there is room. */
	if (states->count == KEIUI_DECLARE_ACTIONS)
		return ENOSPC;
	states->actions[states->count] = action;
	states->states[states->count] = state;
	states->count++;

	/* Succeeded. */
	return 0;
}

/*
 * Shows a table of menu entries: the changes from what is shown go
 * through the sink.  Returns 0, EINVAL, ENOMEM, or the sink's refusal.
 */
int
keiui_declare_menu(
	struct keiui_declare_model *model,
	const struct kl_menu_entry *entries,
	size_t count,
	const struct keiui_declare_states *states,
	keiui_declare_sink sink,
	void *data)
{
	struct keiui_declare_item *fresh;
	struct declare_change change;
	size_t index;
	int error;

	/* A table to read. */
	if (entries == NULL && count != 0U)
		return EINVAL;

	/* The entries as the model keeps them. */
	error = declare_make(&fresh, count);
	if (error != 0)
		return error;
	for (index = 0; index < count; index++) {
		fresh[index].id = entries[index].id;
		fresh[index].parent = entries[index].parent;
		fresh[index].type = entries[index].type;
		fresh[index].action = entries[index].action;
		fresh[index].role = entries[index].role;
		fresh[index].modifiers = entries[index].modifiers;
		fresh[index].keysym = entries[index].keysym;
		fresh[index].state = keiui_declare_state_of(states, entries[index].action);
		if (entries[index].label != NULL)
			fresh[index].label = strdup(entries[index].label);
		else
			fresh[index].label = strdup("");
		if (fresh[index].label == NULL) {
			declare_free(fresh, count);
			return ENOMEM;
		}
	}

	/* The changes, then the model takes the table. */
	memset(&change, 0, sizeof(change));
	change.sink = sink;
	change.data = data;
	error = declare_finish(model, fresh, count, &change);
	return error;
}

/*
 * Shows a table of a titlebar's controls: the changes from what is shown
 * go through the sink.  Returns 0, EINVAL, ENOMEM, or the sink's refusal.
 */
int
keiui_declare_controls(
	struct keiui_declare_model *model,
	const struct kl_control_entry *entries,
	size_t count,
	const struct keiui_declare_states *states,
	keiui_declare_sink sink,
	void *data)
{
	struct keiui_declare_item *fresh;
	struct declare_change change;
	size_t index;
	int error;

	/* A table to read. */
	if (entries == NULL && count != 0U)
		return EINVAL;

	/* The controls as the model keeps them (the priority as the type, the group as the parent). */
	error = declare_make(&fresh, count);
	if (error != 0)
		return error;
	for (index = 0; index < count; index++) {
		fresh[index].id = entries[index].id;
		fresh[index].parent = entries[index].group;
		fresh[index].type = entries[index].priority;
		fresh[index].role = entries[index].role;
		fresh[index].action = entries[index].action;
		fresh[index].state = keiui_declare_state_of(states, entries[index].action);
		if (entries[index].label != NULL)
			fresh[index].label = strdup(entries[index].label);
		else
			fresh[index].label = strdup("");
		if (fresh[index].label == NULL) {
			declare_free(fresh, count);
			return ENOMEM;
		}
	}

	/* The changes, then the model takes the table. */
	memset(&change, 0, sizeof(change));
	change.sink = sink;
	change.data = data;
	error = declare_finish(model, fresh, count, &change);
	return error;
}

/*
 * Applies the action states again: the entries whose state changed go
 * through the sink.  Returns 0 or the sink's refusal.
 */
int
keiui_declare_refresh(
	struct keiui_declare_model *model,
	const struct keiui_declare_states *states,
	keiui_declare_sink sink,
	void *data)
{
	struct declare_change change;
	unsigned state;
	size_t index;

	/* Nothing shown, nothing to change. */
	if (!model->built)
		return 0;

	/* Each entry whose action's state is not the one shown. */
	memset(&change, 0, sizeof(change));
	change.sink = sink;
	change.data = data;
	for (index = 0; index < model->count; index++) {
		state = keiui_declare_state_of(states, model->items[index].action);
		if (state == model->items[index].state)
			continue;
		model->items[index].state = state;
		declare_send(&change, KEIUI_DECLARE_STATE, &model->items[index]);
	}

	/* The transaction ends when one was begun. */
	if (change.begun)
		(void)sink(data, KEIUI_DECLARE_COMMIT, NULL);

	/* A refusal leaves the model rebuilt next time. */
	if (change.error != 0) {
		model->built = 0;
		return change.error;
	}

	/* Succeeded. */
	return 0;
}

/*
 * Reports the entry of an ID, or NULL when the model has none.
 */
const struct keiui_declare_item *
keiui_declare_find(
	const struct keiui_declare_model *model,
	uint32_t id)
{
	size_t index;

	/* The first entry of the ID. */
	for (index = 0; index < model->count; index++) {
		if (model->items[index].id == id)
			return &model->items[index];
	}

	/* None. */
	return NULL;
}

/* Allocates a table of entries (none for 0); 0 or ENOMEM. */
static int
declare_make(
	struct keiui_declare_item **made,
	size_t count)
{
	/* An empty table needs no memory. */
	*made = NULL;
	if (count == 0U)
		return 0;

	/* The entries, zeroed. */
	*made = calloc(count, sizeof(**made));
	if (*made == NULL)
		return ENOMEM;

	/* Succeeded. */
	return 0;
}

/* Frees a table of entries and their labels. */
static void
declare_free(
	struct keiui_declare_item *items,
	size_t count)
{
	size_t index;

	/* Each label, then the table. */
	for (index = 0; index < count; index++)
		free(items[index].label);
	free(items);
}

/* Tells whether a new table moves an entry in the tree (then the whole is built again). */
static int
declare_structure(
	const struct keiui_declare_model *model,
	const struct keiui_declare_item *fresh,
	size_t count)
{
	size_t index;

	/* Nothing shown yet, or another number of entries. */
	if (!model->built || model->count != count)
		return 1;

	/* An entry of another ID, parent or type at a place; a control of another role. */
	for (index = 0; index < count; index++) {
		if (fresh[index].id != model->items[index].id ||
		    fresh[index].parent != model->items[index].parent ||
		    fresh[index].type != model->items[index].type)
			return 1;
		if (model->kind == KEIUI_DECLARE_CONTROLS && fresh[index].role != model->items[index].role)
			return 1;
	}

	/* The same tree. */
	return 0;
}

/* Sends one operation, beginning the transaction first; nothing after a refusal. */
static void
declare_send(
	struct declare_change *change,
	unsigned operation,
	const struct keiui_declare_item *item)
{
	int error;

	/* Nothing more after a refusal. */
	if (change->error != 0)
		return;

	/* The transaction, once. */
	if (!change->begun) {
		error = change->sink(change->data, KEIUI_DECLARE_BEGIN, NULL);
		if (error != 0) {
			change->error = error;
			return;
		}

		/* Begun. */
		change->begun = 1;
	}

	/* The operation. */
	error = change->sink(change->data, operation, item);
	if (error != 0)
		change->error = error;
}

/* Takes away what is shown and builds the new table. */
static void
declare_rebuild(
	struct keiui_declare_model *model,
	const struct keiui_declare_item *fresh,
	size_t count,
	struct declare_change *change)
{
	size_t index;
	int nested;

	/* Each shown entry not under another (removing one takes what is under it too). */
	for (index = 0; index < model->count; index++) {
		nested = 0;
		if (model->kind == KEIUI_DECLARE_MENU)
			nested = declare_is_parent(model->items, model->count, model->items[index].parent);
		if (!nested)
			declare_send(change, KEIUI_DECLARE_REMOVE, &model->items[index]);
	}

	/* Each new entry in order, with what the append does not carry. */
	for (index = 0; index < count; index++) {
		declare_send(change, KEIUI_DECLARE_APPEND, &fresh[index]);
		if (model->kind == KEIUI_DECLARE_MENU && fresh[index].role != KL_MENU_ROLE_NONE)
			declare_send(change, KEIUI_DECLARE_ROLE, &fresh[index]);
		if (model->kind == KEIUI_DECLARE_MENU && (fresh[index].keysym != 0U || fresh[index].modifiers != 0U))
			declare_send(change, KEIUI_DECLARE_SHORTCUT, &fresh[index]);
		if (fresh[index].state != 0U)
			declare_send(change, KEIUI_DECLARE_STATE, &fresh[index]);
	}
}

/* Sends what changed in each entry of the same tree. */
static void
declare_update(
	struct keiui_declare_model *model,
	const struct keiui_declare_item *fresh,
	size_t count,
	struct declare_change *change)
{
	const struct keiui_declare_item *shown;
	size_t index;
	int same;

	/* Each entry against the one shown at its place. */
	for (index = 0; index < count; index++) {
		/* The label. */
		shown = &model->items[index];
		same = strcmp(fresh[index].label, shown->label);
		if (same != 0)
			declare_send(change, KEIUI_DECLARE_LABEL, &fresh[index]);

		/* A menu item's action, role and shortcut (a control's action is the library's own). */
		if (model->kind == KEIUI_DECLARE_MENU) {
			if (fresh[index].action != shown->action)
				declare_send(change, KEIUI_DECLARE_ACTION, &fresh[index]);
			if (fresh[index].role != shown->role)
				declare_send(change, KEIUI_DECLARE_ROLE, &fresh[index]);
			if (fresh[index].modifiers != shown->modifiers || fresh[index].keysym != shown->keysym)
				declare_send(change, KEIUI_DECLARE_SHORTCUT, &fresh[index]);
		}

		/* The state. */
		if (fresh[index].state != shown->state)
			declare_send(change, KEIUI_DECLARE_STATE, &fresh[index]);
	}
}

/* Sends the change to a new table and makes it the model's; 0 or the first refusal. */
static int
declare_finish(
	struct keiui_declare_model *model,
	struct keiui_declare_item *fresh,
	size_t count,
	struct declare_change *change)
{
	int structure;

	/* The whole again, or only what changed. */
	structure = declare_structure(model, fresh, count);
	if (structure)
		declare_rebuild(model, fresh, count, change);
	else
		declare_update(model, fresh, count, change);

	/* The transaction ends when one was begun, refused or not. */
	if (change->begun)
		(void)change->sink(change->data, KEIUI_DECLARE_COMMIT, NULL);

	/* The new table is the model's either way. */
	declare_free(model->items, model->count);
	model->items = fresh;
	model->count = count;
	model->built = 1;

	/* A refusal leaves the compositor's copy unknown: the next table is built again. */
	if (change->error != 0) {
		model->built = 0;
		return change->error;
	}

	/* Succeeded. */
	return 0;
}

/* Tells whether an ID is one of the entries'. */
static int
declare_is_parent(
	const struct keiui_declare_item *items,
	size_t count,
	uint32_t id)
{
	size_t index;

	/* Any entry of the ID. */
	for (index = 0; index < count; index++) {
		if (items[index].id == id)
			return 1;
	}

	/* None. */
	return 0;
}
