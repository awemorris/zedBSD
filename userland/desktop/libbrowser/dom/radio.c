/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Explicit checkedness updates derive radio groups from current ordinary DOM ownership. */

#include "dom/dom.h"

#include <errno.h>

static struct dom_node *radio_next(struct dom_node *node, const struct dom_node *root);
static int radio_group_member(struct dom_node *node, struct dom_element *target, struct dom_element *owner, struct vm_string *name);

/*
 * Tests actual HTML input identity and Radio state without observing script properties.
 */
int
dom_input_is_radio(
	const struct dom_element *element)
{
	int input;
	int kind;

	/* A folded internal tag cannot make an uppercase XML or foreign input a radio. */
	if (element == NULL || element->ns != DOM_NS_HTML)
		return 0;

	/* Require the exact HTML local name rather than a parser-folded tag identity. */
	input = vm_string_equal_ascii(element->local_name, "input");
	if (!input)
		return 0;

	/* Derive the current Radio state from the native input type. */
	kind = dom_control_kind(element);
	if (kind != DOM_CONTROL_RADIO)
		return 0;

	/* Succeeded: the current actual input type is Radio. */
	return 1;
}

/*
 * Finds a selected member of the current ordinary radio group before activation.
 */
struct dom_element *
dom_input_checked_radio(
	struct dom_element *element)
{
	struct dom_element *owner;
	struct dom_element *other;
	struct dom_node *root;
	struct dom_node *node;
	struct vm_string *name;
	int matches;
	int checked;

	/* A selected target itself is the previous selection, including unnamed radios. */
	checked = dom_control_checked(element);
	if (checked)
		return element;

	/* An unnamed target has no other radio peer to restore during activation. */
	name = dom_attribute_ascii(element, "name");
	if (name == NULL || name->length == 0)
		return NULL;

	/* Find the current tree root, including controls associated from outside the form subtree. */
	root = &element->node;
	while (root->parent != NULL)
		root = root->parent;

	/* Compare every candidate with the target's current ordinary form owner. */
	owner = dom_form_owner(element);

	/* Select the first actual checked peer without observing script properties. */
	node = root;
	while (node != NULL) {
		matches = radio_group_member(node, element, owner, name);
		if (matches) {
			other = (struct dom_element *)node;
			checked = dom_control_checked(other);
			if (checked)
				return other;
		}

		/* Advance only within this ordinary tree, excluding template contents. */
		node = radio_next(node, root);
	}

	/* No member was selected before activation. */
	return NULL;
}

/*
 * Rechecks the saved selection against the target's group after arbitrary listeners.
 */
int
dom_input_same_radio_group(
	struct dom_element *first,
	struct dom_element *second)
{
	struct dom_node *first_root;
	struct dom_node *second_root;
	struct dom_element *owner;
	struct vm_string *name;
	int radio;
	int matches;

	/* A missing or changed-type prior selection cannot be restored as a group peer. */
	if (second == NULL)
		return 0;

	/* The activation target must still have its actual Radio type. */
	radio = dom_input_is_radio(first);
	if (!radio)
		return 0;

	/* The saved selection must also remain a genuine native radio. */
	radio = dom_input_is_radio(second);
	if (!radio)
		return 0;

	/* A radio which was already selected can restore itself without a named peer. */
	if (first == second)
		return 1;

	/* Distinct radios require a nonempty group name. */
	name = dom_attribute_ascii(first, "name");
	if (name == NULL || name->length == 0)
		return 0;

	/* Find the activation target's current tree independently of its ownerDocument. */
	first_root = &first->node;
	while (first_root->parent != NULL)
		first_root = first_root->parent;

	/* Find the saved selection's tree after listeners may have moved it. */
	second_root = &second->node;
	while (second_root->parent != NULL)
		second_root = second_root->parent;

	/* Separate ordinary trees isolate otherwise matching radio names and form owners. */
	if (first_root != second_root)
		return 0;

	/* The existing group predicate checks exact names, actual type and ordinary owner. */
	owner = dom_form_owner(first);
	matches = radio_group_member(&second->node, first, owner, name);

	/* Succeeded: report current membership after all native group constraints were compared. */
	return matches;
}

/*
 * Sets current checkedness and optionally its dirty flag, unchecking the actual radio group.
 */
int
dom_input_set_checked(
	struct dom_element *element,
	int checked,
	int dirty)
{
	struct dom_control *control;
	struct dom_control *other_control;
	struct dom_element *owner;
	struct dom_element *other;
	struct dom_node *root;
	struct dom_node *node;
	struct vm_string *name;
	int radio;
	int matches;
	int current;

	/* Target storage must exist before any group's current checkedness changes. */
	control = dom_control_of(element);
	if (control == NULL)
		return ENOMEM;

	/* Only a checked radio with a nonempty actual name participates in exclusivity. */
	radio = dom_input_is_radio(element);
	name = dom_attribute_ascii(element, "name");
	if (!checked ||
	    !radio ||
	    name == NULL ||
	    name->length == 0) {
		/* Current-state accessors now use this value instead of deriving it from content. */
		control->checked = checked;
		control->checked_initialized = 1;

		/* Script assignment prevents later checked-attribute changes from replacing this state. */
		if (dirty)
			control->checked_dirty = 1;

		/* Invalidate generation-based Document observers after the native state change. */
		dom_note_change(&element->node, DOM_CHANGE_STATE, NULL, NULL);

		/* Succeeded: a non-group assignment updates only this control. */
		return 0;
	}

	/* Resolve the live form association before selecting ordinary-tree peers. */
	owner = dom_form_owner(element);

	/* Find the actual current tree rather than using ownerDocument or an ID as its bounds. */
	root = &element->node;
	while (root->parent != NULL)
		root = root->parent;

	/* Prepare every affected member's storage before committing any checkedness changes. */
	node = root;
	while (node != NULL) {
		matches = radio_group_member(node, element, owner, name);
		if (matches) {
			/* Unselected peers need no mutable storage during the later commit. */
			other = (struct dom_element *)node;
			current = dom_control_checked(other);
			if (current) {
				other_control = dom_control_of(other);
				if (other_control == NULL)
					return ENOMEM;
			}
		}

		/* No allocation or user script mutates DOM links during this preparation. */
		node = radio_next(node, root);
	}

	/* Uncheck only current group members, preserving their independent dirty flags. */
	node = root;
	while (node != NULL) {
		matches = radio_group_member(node, element, owner, name);
		if (matches) {
			/* Preserve unselected peers and each peer's independent dirty flag. */
			other = (struct dom_element *)node;
			current = dom_control_checked(other);
			if (current) {
				/* Native readers use the committed value and Document observers see its new generation. */
				other_control = other->control;
				other_control->checked = 0;
				other_control->checked_initialized = 1;
				dom_note_change(&other->node, DOM_CHANGE_STATE, NULL, NULL);
			}
		}

		/* The allocation-free commit follows the same unchanged tree membership. */
		node = radio_next(node, root);
	}

	/* Native readers use the committed target value instead of deriving it from content. */
	control->checked = checked;
	control->checked_initialized = 1;

	/* Script assignment overrides later content changes; list-value assignment preserves this flag. */
	if (dirty)
		control->checked_dirty = 1;

	/* Invalidate generation-based Document observers after committing target checkedness. */
	dom_note_change(&element->node, DOM_CHANGE_STATE, NULL, NULL);

	/* Succeeded: target and every affected radio now expose consistent current checkedness. */
	return 0;
}

/*
 * Copies current initialized and dirty input checkedness without triggering group updates.
 */
int
dom_input_clone_checked(
	struct dom_element *destination,
	const struct dom_element *source)
{
	struct dom_control *control;
	int input;

	/* Cloning another element must not allocate or modify form-control state. */
	if (source->ns != DOM_NS_HTML || source->tag != DOM_TAG_INPUT)
		return 0;

	/* Reject XML case lookalikes even when their parser tag was folded to Input. */
	input = vm_string_equal_ascii(source->local_name, "input");
	if (!input)
		return 0;

	/* A clean, uninitialized input still derives checkedness from copied attributes. */
	if (source->control == NULL)
		return 0;

	/* Default state needs no independent clone storage before its first native observation. */
	if (!source->control->checked_initialized &&
	    !source->control->checked_dirty &&
	    !source->control->indeterminate)
		return 0;

	/* Allocate destination state before publishing any copied current or dirty flags. */
	control = dom_control_of(destination);
	if (control == NULL)
		return ENOMEM;

	/* Preserve native observed state and the dirty flag which prevents content from overwriting it. */
	control->checked = source->control->checked;
	control->checked_initialized = source->control->checked_initialized;
	control->checked_dirty = source->control->checked_dirty;
	control->indeterminate = source->control->indeterminate;

	/* Succeeded: disconnected cloning has no side effect on the original radio group. */
	return 0;
}

/* Finds the next node within one unchanged actual tree, excluding independent template contents. */
static struct dom_node *
radio_next(
	struct dom_node *node,
	const struct dom_node *root)
{
	/* Descendants precede every following subtree in ordinary tree order. */
	if (node->first_child != NULL)
		return node->first_child;

	/* Climb only within this group tree until a following sibling appears. */
	while (node != root) {
		/* A sibling supplies the next subtree before the traversal climbs farther. */
		if (node->next != NULL)
			return node->next;

		/* Search this parent's following subtree without leaving the group tree. */
		node = node->parent;
	}

	/* Exhausted: the actual root has no following in-tree member. */
	return NULL;
}

/* Compares current type, nonempty name and ordinary form owner for an in-tree candidate. */
static int
radio_group_member(
	struct dom_node *node,
	struct dom_element *target,
	struct dom_element *owner,
	struct vm_string *name)
{
	struct dom_element *element;
	struct dom_element *actual_owner;
	struct vm_string *actual_name;
	int radio;
	int same;

	/* The target itself is committed separately from other group members. */
	if (node == &target->node || node->type != DOM_ELEMENT)
		return 0;

	/* A peer must be an actual current radio rather than an ordinary element. */
	element = (struct dom_element *)node;
	radio = dom_input_is_radio(element);
	if (!radio)
		return 0;

	/* Names, including case, are content attributes rather than id or script aliases. */
	actual_name = dom_attribute_ascii(element, "name");
	if (actual_name == NULL || actual_name->length == 0)
		return 0;

	/* Compare complete case-sensitive native names to preserve independent radio groups. */
	same = vm_string_equal(name, actual_name);
	if (!same)
		return 0;

	/* Both null owners form a group, while different actual forms isolate their radios. */
	actual_owner = dom_form_owner(element);
	if (actual_owner != owner)
		return 0;

	/* Succeeded: this in-tree current radio belongs to the target's actual group. */
	return 1;
}
