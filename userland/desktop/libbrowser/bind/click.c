/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Script click owns its recursion guard and pending activation across arbitrary listeners. */

#include "bind/internal.h"

#include <errno.h>

static int click_html(const struct dom_element *element, int tag);
static int click_disabled(struct dom_element *element);
static int click_kind(struct dom_element *element);
static int click_event(struct bind_window *window, struct dom_element *element, const char *type, int interface, unsigned flags, struct vm_cell **root, int *canceled);
static int click_finish(struct bind_window *window, struct dom_element *element, struct vm_cell **roots, unsigned count, int status);
static int click_restore(struct dom_element *element, struct dom_element *previous, int checked, int indeterminate);
static int click_submit(struct bind_window *window, struct dom_element *element);
static int click_link(struct dom_element *element);
static int click_notify(struct bind_window *window, struct dom_element *element, int old_checked, struct vm_cell **root);

/*
 * Activates a script-clicked checkbox or radio around one untrusted click dispatch.
 * Registered roots retain a removed prior radio and the event graph without stack scanning.
 */
int
bind_click(
	struct bind_window *window,
	struct dom_element *element)
{
	struct dom_element *previous;
	struct dom_control *control;
	struct vm_cell *roots[3];
	unsigned index;
	int checked;
	int indeterminate;
	int kind;
	int disabled;
	int canceled;
	int restored;
	int status;

	/* A disabled built-in or an already executing click has no script-visible side effect. */
	disabled = click_disabled(element);
	if (disabled || element->click_in_progress)
		return 0;

	/* Snapshot native state before callbacks can change any member or ownership relation. */
	kind = click_kind(element);
	checked = dom_control_checked(element);
	indeterminate = 0;

	/* An existing control carries the preactivation indeterminate state. */
	if (element->control != NULL)
		indeterminate = element->control->indeterminate;

	/* Retains the previously selected radio when current group selection can change. */
	previous = NULL;
	if (kind == DOM_CONTROL_RADIO)
		previous = dom_input_checked_radio(element);

	/* Fixed root slots remain registered until every activation callback has completed. */
	roots[0] = &element->node.cell;
	roots[1] = NULL;
	if (previous != NULL)
		roots[1] = &previous->node.cell;
	roots[2] = NULL;
	for (index = 0; index < 3U; index++) {
		/* Registers each stable stack slot before any listener can collect. */
		status = vm_heap_add_root(window->realm->heap, &roots[index]);
		if (status != 0) {
			status = click_finish(window, element, roots, index, status);
			return status;
		}
	}

	/* The guard covers click, cancellation and subsequent input/change listeners. */
	element->click_in_progress = 1;

	/* Checkbox listeners observe the flipped checkedness and cleared indeterminateness. */
	if (kind == DOM_CONTROL_CHECKBOX) {
		status = dom_input_set_checked(element, !checked, 1);
		if (status != 0) {
			status = click_finish(window, element, roots, 3U, status);
			return status;
		}

		/* Checkbox preactivation clears the existing control's indeterminate flag. */
		control = element->control;
		control->indeterminate = 0;
	} else if (kind == DOM_CONTROL_RADIO) {
		/* Radio listeners observe the new actual group selection before any dispatch. */
		status = dom_input_set_checked(element, 1, 1);
		if (status != 0) {
			status = click_finish(window, element, roots, 3U, status);
			return status;
		}
	}

	/* The established MouseEvent interface supplies a bubbling cancelable synthetic click. */
	status = click_event(window, element, "click", BIND_MOUSE_EVENT, BIND_EVENT_BUBBLES | BIND_EVENT_CANCELABLE, &roots[2], &canceled);
	if (status != 0) {
		/* Infrastructure failure restores selected state before releasing retained cells. */
		restored = click_restore(element, previous, checked, indeterminate);
		if (restored != 0)
			status = restored;

		/* Releases the activation protocol with the original or restoration failure. */
		status = click_finish(window, element, roots, 3U, status);
		return status;
	}

	/* Cancellation restores native current state using the post-listener radio group. */
	if (canceled) {
		status = click_restore(element, previous, checked, indeterminate);
		if (status != 0) {
			status = click_finish(window, element, roots, 3U, status);
			return status;
		}

		/* Ends the canceled activation after restoring ordinary control state. */
		status = click_finish(window, element, roots, 3U, 0);
		if (status != 0)
			return status;

		/* Succeeded: canceled activation has restored state and released every root. */
		return 0;
	}

	/* Only eligible connected current checkbox/radio state produces activation notifications. */
	status = click_notify(window, element, checked, &roots[2]);
	if (status != 0) {
		status = click_finish(window, element, roots, 3U, status);
		return status;
	}

	/* Runs submission activation only after ordinary notifications succeeded. */
	status = click_submit(window, element);
	if (status != 0) {
		status = click_finish(window, element, roots, 3U, status);
		return status;
	}

	/* A link the click landed in is followed. */
	status = bind_click_follow(window, element);
	if (status != 0) {
		status = click_finish(window, element, roots, 3U, status);
		return status;
	}

	/* Succeeded: releases the recursion protocol and all temporary graph roots. */
	status = click_finish(window, element, roots, 3U, 0);
	if (status != 0)
		return status;

	/* Succeeded: all activation callbacks finished and released their temporary roots. */
	return 0;
}

/*
 * Follows the link that is a clicked element or its nearest ancestor.
 *
 * A click's activation behavior; an input's own activation is the only one
 * it has.
 */
int
bind_click_follow(
	struct bind_window *window,
	struct dom_element *element)
{
	struct dom_node *walk;
	int status;

	/* An input has its own activation. */
	if (click_html(element, DOM_TAG_INPUT))
		return 0;

	/* The nearest inclusive ancestor that is a link. */
	for (walk = &element->node; walk != NULL; walk = walk->parent) {
		if (walk->type == DOM_ELEMENT && click_link((struct dom_element *)walk))
			break;
	}

	/* No link: nothing to follow. */
	if (walk == NULL)
		return 0;

	/* The session history navigates. */
	status = bind_follow_hyperlink(window, (struct dom_element *)walk);
	if (status != 0)
		return status;

	/* Succeeded: the link is followed. */
	return 0;
}

/* Tells whether an element is a hyperlink: an HTML a or area, or an SVG a. */
static int
click_link(
	struct dom_element *element)
{
	int html;
	int svg;

	/* The HTML links. */
	html = click_html(element, DOM_TAG_A) || click_html(element, DOM_TAG_AREA);
	if (html)
		return 1;

	/* SVG's a element. */
	svg = element->ns == DOM_NS_SVG && vm_string_equal_ascii(element->local_name, "a");
	if (svg)
		return 1;

	/* Anything else is not a link. */
	return 0;
}

/* Distinguishes actual HTML local names from folded XML tags or foreign namespaces. */
static int
click_html(
	const struct dom_element *element,
	int tag)
{
	const char *name;
	int same;

	/* Internal tag numbers alone cannot authorize native form-control behavior. */
	if (element->ns != DOM_NS_HTML || element->tag != tag)
		return 0;

	/* Verifies case-sensitive local identity rather than only the folded tag number. */
	name = dom_tag_name(tag);
	same = vm_string_equal_ascii(element->local_name, name);
	if (!same)
		return 0;

	/* Succeeded: the element has this exact HTML local identity. */
	return 1;
}

/* Checks effective disabling, including each disabled fieldset's first legend exception. */
static int
click_disabled(
	struct dom_element *element)
{
	struct dom_element *ancestor;
	struct dom_node *node;
	struct dom_node *child;
	struct dom_node *legend;
	struct dom_node *path;
	struct vm_string *attribute;
	int control;
	int fieldset;
	int actual;

	/* Only the built-in disabling-capable controls honor their disabled attribute. */
	control = 0;

	/* Selects only HTML interfaces that implement built-in disabling. */
	switch (element->tag) {
	case DOM_TAG_INPUT:
	case DOM_TAG_BUTTON:
	case DOM_TAG_SELECT:
	case DOM_TAG_TEXTAREA:
	case DOM_TAG_FIELDSET:
	case DOM_TAG_OPTGROUP:
	case DOM_TAG_OPTION:
		control = click_html(element, element->tag);
		break;
	default:
		break;
	}

	/* An ordinary element's unrelated disabled attribute does not suppress its click. */
	if (!control)
		return 0;

	/* An eligible control's own disabled attribute suppresses activation. */
	attribute = dom_attribute_ascii(element, "disabled");
	if (attribute != NULL)
		return 1;

	/* Options and optgroups do not inherit fieldset disabling like interactive controls. */
	if (element->tag == DOM_TAG_OPTION || element->tag == DOM_TAG_OPTGROUP)
		return 0;

	/* Each disabled ancestor independently decides whether its first legend exempts this node. */
	for (node = element->node.parent; node != NULL; node = node->parent) {
		/* A fragment or non-element ancestor cannot be a disabling fieldset. */
		if (node->type != DOM_ELEMENT)
			continue;

		/* An actual HTML fieldset can impose inherited disabling. */
		ancestor = (struct dom_element *)node;
		fieldset = click_html(ancestor, DOM_TAG_FIELDSET);
		if (!fieldset)
			continue;

		/* Only a disabled ancestor needs a legend exception check. */
		attribute = dom_attribute_ascii(ancestor, "disabled");
		if (attribute == NULL)
			continue;

		/* Only the first direct actual HTML legend supplies an exception subtree. */
		legend = NULL;
		for (child = node->first_child; child != NULL; child = child->next) {
			/* Non-elements and foreign legends do not occupy the first legend slot. */
			if (child->type != DOM_ELEMENT)
				continue;

			/* The first genuine HTML legend alone occupies the exemption slot. */
			actual = click_html((struct dom_element *)child, DOM_TAG_LEGEND);
			if (actual) {
				legend = child;
				break;
			}
		}

		/* Climb to this ancestor's direct child to test the legend exception. */
		path = &element->node;
		while (path->parent != node)
			path = path->parent;

		/* An ordinary child outside the first legend subtree remains disabled. */
		if (path != legend)
			return 1;
	}

	/* No own attribute or non-exempt disabled fieldset suppresses activation. */
	return 0;
}

/* Selects checkbox/radio behavior from actual input identity and current content type. */
static int
click_kind(
	struct dom_element *element)
{
	int input;
	int kind;

	/* Script type expandos and uppercase XML INPUT never acquire input activation. */
	input = click_html(element, DOM_TAG_INPUT);
	if (!input)
		return DOM_CONTROL_NONE;

	/* Returns the genuine control kind for this current HTML input. */
	kind = dom_control_kind(element);

	/* Succeeded: the caller receives the current native input kind. */
	return kind;
}

/* Prepares and roots one event before native dispatch can invoke arbitrary callbacks. */
static int
click_event(
	struct bind_window *window,
	struct dom_element *element,
	const char *type,
	int interface,
	unsigned flags,
	struct vm_cell **root,
	int *canceled)
{
	struct bind_event *event;
	vm_value value;
	vm_value target;
	int status;

	/* The target wrapper is already retained by its explicitly rooted native node. */
	status = bind_event_prepare(window, &element->node, interface, type, flags, &value, &target, &event);
	if (status != 0)
		return status;

	/* The registered slot retains the prepared event across listener callbacks. */
	*root = vm_value_as_cell(value);

	/* Script click is untrusted; activation-generated input/change retain trusted defaults. */
	if (interface == BIND_MOUSE_EVENT) {
		event->trusted = 0;
		event->view = vm_value_cell(window->realm->global);
	}

	/* The root slot follows this event through every listener and dispatcher checkpoint. */
	status = bind_dispatch(window, target, value, canceled);
	if (status != 0)
		return status;

	/* Succeeded: every listener saw the retained actual event graph. */
	return 0;
}

/* Releases the recursion protocol and every root registered by this attempt. */
static int
click_finish(
	struct bind_window *window,
	struct dom_element *element,
	struct vm_cell **roots,
	unsigned count,
	int status)
{
	unsigned index;

	/* A subsequent independent click is eligible even after cancellation or allocation failure. */
	element->click_in_progress = 0;

	/* Registered slots refer to this call's stack, so none may escape its return. */
	for (index = 0; index < count; index++)
		vm_heap_remove_root(window->realm->heap, &roots[index]);

	/* The caller receives the original dispatch or state-update outcome. */
	if (status != 0)
		return status;

	/* Succeeded: the recursion guard and registered root slots are released. */
	return 0;
}

/* Restores checkbox state or the retained radio only when it still belongs to the current group. */
static int
click_restore(
	struct dom_element *element,
	struct dom_element *previous,
	int checked,
	int indeterminate)
{
	int kind;
	int same;
	int status;

	/* Current type controls cancellation after a listener changes the input. */
	kind = click_kind(element);
	if (kind == DOM_CONTROL_CHECKBOX) {
		status = dom_input_set_checked(element, checked, 0);
		if (status != 0)
			return status;

		/* Restores indeterminateness after checkedness has been restored. */
		element->control->indeterminate = indeterminate;

		/* Succeeded: both checkbox state fields match the preactivation snapshot. */
		return 0;
	}

	/* Other current input types have no selected cancellation behavior in this increment. */
	if (kind != DOM_CONTROL_RADIO)
		return 0;

	/* Current ordinary group membership decides which radio can be restored. */
	same = dom_input_same_radio_group(element, previous);
	if (same) {
		status = dom_input_set_checked(previous, 1, 0);
		if (status != 0)
			return status;
	} else {
		status = dom_input_set_checked(element, 0, 0);
		if (status != 0)
			return status;
	}

	/* Restore uses current ordinary membership, retaining existing dirty flags. */
	return 0;
}

/* Emits native activation notifications only for a connected eligible current control. */
static int
click_notify(
	struct bind_window *window,
	struct dom_element *element,
	int old_checked,
	struct vm_cell **root)
{
	struct dom_node *node;
	int kind;
	int checked;
	int canceled;
	int status;

	/* A type changed by click listeners can remove checkbox/radio activation behavior. */
	kind = click_kind(element);
	if (kind != DOM_CONTROL_CHECKBOX && kind != DOM_CONTROL_RADIO)
		return 0;

	/* An already selected or listener-unchecked radio produces no value-change notifications. */
	if (kind == DOM_CONTROL_RADIO) {
		checked = dom_control_checked(element);
		if (old_checked || !checked)
			return 0;
	}

	/* Detached and independent template trees can toggle but cannot emit activation notifications. */
	node = &element->node;
	while (node->parent != NULL)
		node = node->parent;

	/* Only a node connected to an actual Document emits these notifications. */
	if (node->type != DOM_DOCUMENT)
		return 0;

	/* Input precedes change; both bubble and neither can cancel completed activation. */
	status = click_event(window, element, "input", BIND_EVENT, BIND_EVENT_BUBBLES, root, &canceled);
	if (status != 0)
		return status;

	/* Delivers change only after input listeners finish successfully. */
	status = click_event(window, element, "change", BIND_EVENT, BIND_EVENT_BUBBLES, root, &canceled);
	if (status != 0)
		return status;

	/* Succeeded: connected current activation delivered input followed by change. */
	return 0;
}

/* Invokes the finite native submission-event stage for a current actual input Submit button. */
static int
click_submit(
	struct bind_window *window,
	struct dom_element *element)
{
	struct dom_element *form;
	struct dom_node *root;
	int kind;
	int disabled;
	int canceled;
	int status;

	/* Current type and ordinary live owner are recomputed after every click listener. */
	kind = click_kind(element);
	if (kind != DOM_CONTROL_SUBMIT)
		return 0;

	/* A listener can disable this nonmutable submit input before activation begins. */
	disabled = click_disabled(element);
	if (disabled)
		return 0;

	/* Recomputes the submitter's live form owner after arbitrary click listeners. */
	form = dom_form_owner(element);
	if (form == NULL)
		return 0;

	/* Only a connected form in an actual browsing document has submission events here. */
	root = &element->node;
	while (root->parent != NULL)
		root = root->parent;

	/* A connected ordinary form needs a real browsing Document. */
	if (root->type != DOM_DOCUMENT || root->document->view == NULL)
		return 0;

	/* The actual form guard and submitter graph remain retained through arbitrary submit callbacks. */
	status = bind_submit_event(window, form, element, &canceled);
	if (status != 0)
		return status;

	/* A canceled submission has no subsequent navigation stage. */
	if (canceled)
		return 0;

	/* The host currently has no script navigation protocol; preserve that explicit capability boundary. */
	bind_console(window, BIND_CONSOLE_WARN, "form: script submission navigation is not implemented");

	/* Succeeded: the event stage completed within the existing navigation boundary. */
	return 0;
}
