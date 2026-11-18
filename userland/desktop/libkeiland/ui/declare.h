/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The declarative menus and controls of a window (WS131 p015): what the
 * application last gave as a table, and the changes that bring what the
 * compositor shows to a new table or a new action state, as a list of
 * operations a sink carries out (window-declare.c sends them to the menu
 * and the titlebar; the host test records them).  Nothing here talks to
 * the compositor, so the model is tested on the host alone.
 *
 * A menu's or a control's table is compared with the one shown: when an
 * entry's place in the tree changed (its ID, its parent, its type; for a
 * control also its role, priority and group), the whole is taken away and
 * built again; otherwise only what changed is sent.  An action's state
 * (KL_ACTION_* bits) applies to every entry of that action, and only the
 * entries whose state changes are sent.
 */

#ifndef KEIUI_DECLARE_H
#define KEIUI_DECLARE_H

#include <keiland/keiland.h>

#include <stddef.h>
#include <stdint.h>

/* What a model shows: a menu, or a titlebar's controls. */
#define KEIUI_DECLARE_MENU	0U
#define KEIUI_DECLARE_CONTROLS	1U

/* The operations, in the order a change sends them (begin first, commit last). */
#define KEIUI_DECLARE_BEGIN	1U
#define KEIUI_DECLARE_REMOVE	2U
#define KEIUI_DECLARE_APPEND	3U
#define KEIUI_DECLARE_LABEL	4U
#define KEIUI_DECLARE_ACTION	5U
#define KEIUI_DECLARE_ROLE	6U
#define KEIUI_DECLARE_SHORTCUT	7U
#define KEIUI_DECLARE_STATE	8U
#define KEIUI_DECLARE_COMMIT	9U

/* The most actions whose state a window keeps (an action not kept is enabled, unchecked and shown). */
#define KEIUI_DECLARE_ACTIONS	128U

/*
 * One entry as the model keeps it: a menu item's fields, or a control's
 * (its role in role, its priority in type, its group in parent), its own
 * copy of the label, and the state the compositor was last told.
 */
struct keiui_declare_item {
	uint32_t id;
	uint32_t parent;
	unsigned type;
	char *label;
	uint32_t action;
	unsigned role;
	unsigned modifiers;
	uint32_t keysym;
	unsigned state;
};

/* The entries shown, and whether the compositor has them. */
struct keiui_declare_model {
	unsigned kind;
	struct keiui_declare_item *items;
	size_t count;
	int built;
};

/* The state of each action the application named. */
struct keiui_declare_states {
	uint32_t actions[KEIUI_DECLARE_ACTIONS];
	unsigned states[KEIUI_DECLARE_ACTIONS];
	size_t count;
};

/* Carries out one operation on an entry (none for begin and commit); 0 or an errno value. */
typedef int (*keiui_declare_sink)(void *data, unsigned operation, const struct keiui_declare_item *item);

/* A model, an action state table, and their end. */
void keiui_declare_init(struct keiui_declare_model *model, unsigned kind);
void keiui_declare_fini(struct keiui_declare_model *model);
void keiui_declare_states_init(struct keiui_declare_states *states);
unsigned keiui_declare_state_of(const struct keiui_declare_states *states, uint32_t action);
int keiui_declare_set_state(struct keiui_declare_states *states, uint32_t action, unsigned state);

/* A new table: menu entries or controls, sent through the sink. */
int keiui_declare_menu(struct keiui_declare_model *model, const struct kl_menu_entry *entries, size_t count, const struct keiui_declare_states *states, keiui_declare_sink sink, void *data);
int keiui_declare_controls(struct keiui_declare_model *model, const struct kl_control_entry *entries, size_t count, const struct keiui_declare_states *states, keiui_declare_sink sink, void *data);

/* The action states applied again (after one changed). */
int keiui_declare_refresh(struct keiui_declare_model *model, const struct keiui_declare_states *states, keiui_declare_sink sink, void *data);

/* The entry of an ID, or NULL. */
const struct keiui_declare_item *keiui_declare_find(const struct keiui_declare_model *model, uint32_t id);

#endif
