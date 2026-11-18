/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Native submission events retain their submitter and guard one current form dispatch.
 */

#include "bind/internal.h"

#include <errno.h>

static int submit_construct(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int submit_getter(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);

/* The process-lifetime readonly table exposes native submitter state, not input properties. */
static const struct bind_attribute submit_attributes[] = {
	{ "submitter", submit_getter, NULL },
	{ NULL, NULL, NULL }
};

/* The immutable process-lifetime interface inherits Event with its own constructor and native brand. */
const struct bind_interface bind_submit_event_interface = {
	"SubmitEvent", BIND_EVENT, 1, submit_construct, submit_attributes, NULL, NULL
};

static int submit_read_init(struct vm_realm *realm, vm_value init, struct vm_cell **roots, int *bubbles, int *cancelable, int *composed, vm_value *submitter);
static int submit_release(struct vm_heap *heap, struct vm_cell **roots, unsigned count, int error);

/*
 * Dispatches the native event stage without pretending to supply host navigation.
 * Same-form reentrancy cannot create another submission-event attempt during callbacks.
 */
int
bind_submit_event(
	struct bind_window *window,
	struct dom_element *form,
	struct dom_element *submitter,
	int *canceled)
{
	struct bind_event *event;
	struct vm_cell *roots[3];
	vm_value value;
	vm_value target;
	unsigned index;
	int error;

	/* Continuation is suppressed until an actual event returns uncanceled. */
	*canceled = 1;
	if (form->firing_submission_events)
		return 0;

	/* Uses the actual current form Document's view when it owns one. */
	if (form->node.document->view != NULL)
		window = form->node.document->view;

	/* The form and input must survive removal while the original dispatch remains in progress. */
	roots[0] = &form->node.cell;
	roots[1] = &submitter->node.cell;
	roots[2] = NULL;

	/* Registers each original participant before event preparation may collect. */
	for (index = 0; index < 3U; index++) {
		error = vm_heap_add_root(window->realm->heap, &roots[index]);
		if (error != 0) {
			error = submit_release(window->realm->heap, roots, index, error);
			return error;
		}
	}

	/* Preparing an event has no script callbacks, so the guard starts only after preparation succeeds. */
	error = bind_event_prepare(window, &form->node, BIND_SUBMIT_EVENT, "submit", BIND_EVENT_BUBBLES | BIND_EVENT_CANCELABLE, &value, &target, &event);
	if (error != 0) {
		error = submit_release(window->realm->heap, roots, 3U, error);
		return error;
	}

	/* Roots the event object before constructing its independently traced submitter wrapper. */
	roots[2] = vm_value_as_cell(value);
	error = bind_wrap(window, &submitter->node, &event->submitter);
	if (error != 0) {
		error = submit_release(window->realm->heap, roots, 3U, error);
		return error;
	}

	/* Native state carries the actual submitter while the form rejects nested event attempts. */
	form->firing_submission_events = 1;
	error = bind_dispatch(window, target, value, canceled);

	/* Reopens later ordinary submission attempts even if the current dispatch failed. */
	form->firing_submission_events = 0;

	/* Releases all original participant slots before reporting this dispatch outcome. */
	error = submit_release(window->realm->heap, roots, 3U, error);
	if (error != 0)
		return error;

	/* Succeeded: this native submission dispatch and all temporary roots are finished. */
	return 0;
}

/* Converts a SubmitEvent's native dictionary before publishing its newly allocated object. */
static int
submit_construct(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct bind_event *event;
	struct vm_string *type;
	struct vm_cell *roots[3];
	vm_value init;
	vm_value submitter;
	unsigned index;
	int object;
	int bubbles;
	int cancelable;
	int composed;
	int error;

	UNUSED_PARAMETER(receiver);

	/* A required DOMString precedes the optional nullable dictionary conversion. */
	*result = VM_VALUE_UNDEFINED;
	if (count == 0) {
		error = vm_throw_type_error(realm, "SubmitEvent requires a type.");
		return error;
	}

	/* Prepares distinct roots for type text, dictionary and final native submitter. */
	init = js_argument(args, count, 1);
	roots[0] = NULL;
	roots[1] = NULL;
	roots[2] = NULL;
	object = vm_value_is_object(init);
	if (object)
		roots[1] = vm_value_as_cell(init);

	/* Converted strings and later submitter values stay rooted through arbitrary dictionary getters. */
	for (index = 0; index < 3U; index++) {
		error = vm_heap_add_root(realm->heap, &roots[index]);
		if (error != 0) {
			error = submit_release(realm->heap, roots, index, error);
			return error;
		}
	}

	/* Type conversion may itself invoke user code and throw before dictionary conversion begins. */
	error = bind_to_atom(realm, args[0], 0, &type);
	if (error != 0) {
		error = submit_release(realm->heap, roots, 3U, error);
		return error;
	}

	/* Retains the converted type before arbitrary inherited dictionary getters run. */
	roots[0] = &type->cell;

	/* Undefined/null are empty dictionaries; other nonobjects cannot stand for EventInit. */
	if (!object &&
	    init != VM_VALUE_NULL &&
	    init != VM_VALUE_UNDEFINED) {
		error = vm_throw_type_error(realm, "SubmitEvent init must be an object or null.");
		error = submit_release(realm->heap, roots, 3U, error);
		return error;
	}

	/* Conversion is complete before an Event object or observable subclass state is published. */
	error = submit_read_init(realm, init, roots, &bubbles, &cancelable, &composed, &submitter);
	if (error != 0) {
		error = submit_release(realm->heap, roots, 3U, error);
		return error;
	}

	/* The new event receives this constructor's actual creator interfaces. */
	window = bind_window_of(realm);
	error = bind_event_create(window, BIND_SUBMIT_EVENT, type, result, &event);
	if (error != 0) {
		error = submit_release(realm->heap, roots, 3U, error);
		return error;
	}

	/* Constructor events are untrusted and retain all successfully converted native members. */
	event->bubbles = bubbles;
	event->cancelable = cancelable;
	event->composed = composed;
	event->submitter = submitter;

	/* Releases conversion slots only after the complete event traces all native members. */
	error = submit_release(realm->heap, roots, 3U, 0);
	if (error != 0)
		return error;

	/* Succeeded: publishes the complete untrusted SubmitEvent. */
	return 0;
}

/* Reads inherited EventInit members before the nullable native HTMLElement submitter. */
static int
submit_read_init(
	struct vm_realm *realm,
	vm_value init,
	struct vm_cell **roots,
	int *bubbles,
	int *cancelable,
	int *composed,
	vm_value *submitter)
{
	struct dom_node *node;
	struct dom_element *element;
	vm_value value;
	int present;
	int error;

	/* WebIDL booleans do not invoke valueOf or toString on a member's returned object. */
	error = bind_get_option(realm, init, "bubbles", &present, &value);
	if (error != 0)
		return error;
	*bubbles = vm_to_boolean(value);

	/* Reads cancellation after bubbles in the required EventInit dictionary order. */
	error = bind_get_option(realm, init, "cancelable", &present, &value);
	if (error != 0)
		return error;
	*cancelable = vm_to_boolean(value);

	/* Reads boundary propagation after cancellation without coercing member objects. */
	error = bind_get_option(realm, init, "composed", &present, &value);
	if (error != 0)
		return error;
	*composed = vm_to_boolean(value);

	/* An absent, undefined or null submitter initializes the nullable native reference to null. */
	*submitter = VM_VALUE_NULL;
	error = bind_get_option(realm, init, "submitter", &present, &value);
	if (error != 0)
		return error;

	/* Undefined dictionary members are absent; explicit null is the same nullable state. */
	if (!present || value == VM_VALUE_NULL)
		return 0;

	/* Prototype forgery and foreign namespace elements cannot satisfy the HTMLElement contract. */
	node = bind_node_of(value);
	if (node == NULL || node->type != DOM_ELEMENT) {
		error = vm_throw_type_error(realm, "SubmitEvent submitter must be an HTMLElement.");
		return error;
	}

	/* Only the native HTML namespace satisfies the converted submitter contract. */
	element = (struct dom_element *)node;
	if (element->ns != DOM_NS_HTML) {
		error = vm_throw_type_error(realm, "SubmitEvent submitter must be an HTMLElement.");
		return error;
	}

	/* The final converted reference remains rooted while the actual event graph is allocated. */
	*submitter = value;
	roots[2] = vm_value_as_cell(value);

	/* Succeeded: the dictionary's complete native submitter remains retained for allocation. */
	return 0;
}

/* Reads the submitter only from actual native SubmitEvent state, including cross-realm instances. */
static int
submit_getter(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_event *event;
	int error;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* An ordinary Event or an object inheriting SubmitEvent.prototype has no subclass state. */
	event = bind_event_of(receiver);
	if (event == NULL) {
		error = vm_throw_type_error(realm, "SubmitEvent receiver required.");
		return error;
	}

	/* An ordinary Event has a genuine event cell but lacks SubmitEvent's native kind. */
	if (event->interface != BIND_SUBMIT_EVENT) {
		error = vm_throw_type_error(realm, "SubmitEvent receiver required.");
		return error;
	}

	/* The getter returns the retained initialization value without inspecting script properties. */
	*result = event->submitter;

	/* Succeeded: reports the traced nullable native submitter. */
	return 0;
}

/* Removes each slot registered by one constructor or one native submission attempt. */
static int
submit_release(
	struct vm_heap *heap,
	struct vm_cell **roots,
	unsigned count,
	int error)
{
	unsigned index;

	/* No constructor or event attempt may leave a root pointing into its expired native stack. */
	for (index = 0; index < count; index++)
		vm_heap_remove_root(heap, &roots[index]);

	/* Failed conversion, dispatch or allocation preserves its exact refusal code. */
	if (error != 0)
		return error;

	/* Succeeded: all registered slots were removed without changing a successful outcome. */
	return 0;
}
