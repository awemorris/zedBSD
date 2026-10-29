/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Events: EventTarget with its listeners, the Event interfaces (Event,
 * UIEvent, MouseEvent, CustomEvent; the keyboard's, the wheel's and the
 * focus's are in input.c), and dispatch along the tree.
 *
 * Dispatch follows the DOM standard's shape: the path from the target up
 * to the document and the window (not the window for a load event), the
 * capture phase down the path, the target, and the bubble phase up it for
 * an event that bubbles.  An event handler (onclick and the like,
 * handler.c) is one of its target's listeners, in the place it was first
 * set, and cancels the event when it returns false.  A microtask
 * checkpoint follows each listener when no script is running (an event the
 * page fires), and not inside a script's dispatchEvent.
 */

#include "bind/internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The most nodes an event's path holds (the parser caps nesting well below this). */
#define EVENT_PATH_MAX		4096U

/* The listener kinds a phase invokes. */
#define EVENT_INVOKE_CAPTURE	0x1U
#define EVENT_INVOKE_BUBBLE	0x2U

static void event_trace(struct vm_heap *heap, struct vm_cell *cell);
static void listeners_trace(struct vm_heap *heap, struct vm_cell *cell);
static void listeners_finalize(struct vm_heap *heap, struct vm_cell *cell);
static int event_target_construct(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int event_add_listener(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int event_remove_listener(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int event_dispatch_method(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int event_construct(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int event_construct_mouse(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int event_construct_custom(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int event_construct_error(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int event_error_message(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int event_error_filename(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int event_error_line(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int event_error_column(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int event_error_value(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int event_construct_message(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int event_type(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int event_target(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int event_current_target(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int event_phase(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int event_composed(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int event_bubbles(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int event_cancelable(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int event_default_prevented(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int event_is_trusted(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int event_time_stamp(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int event_prevent_default(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int event_stop_propagation(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int event_stop_immediate(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int event_client_x(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int event_client_y(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int event_page_x(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int event_page_y(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int event_button(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int event_init(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int event_init_ui(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int event_ui_detail(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int event_view(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int event_detail(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int event_message_origin(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int event_message_last_id(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int event_message_source(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int event_message_ports(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);

/* The state of an event, which refers to its type, targets and detail. */
static const struct vm_cell_type event_type_cell = { "event", event_trace, NULL };

/* The listeners of a target, which refer to their types and callbacks and own their array. */
static const struct vm_cell_type listeners_type = { "event-listeners", listeners_trace, listeners_finalize };

/*
 * The operations of EventTarget.  The table is constant for the life of
 * the program.
 */
static const struct bind_operation event_target_operations[] = {
	{ "addEventListener", 2, event_add_listener },
	{ "removeEventListener", 2, event_remove_listener },
	{ "dispatchEvent", 1, event_dispatch_method },
	{ NULL, 0, NULL }
};

/*
 * The EventTarget interface (new EventTarget() makes a target of its
 * own).
 */
const struct bind_interface bind_event_target_interface = {
	"EventTarget", BIND_NO_PARENT, 0, event_target_construct, NULL, event_target_operations, NULL
};

/*
 * The attributes of Event.  The table is constant for the life of the
 * program.
 */
static const struct bind_attribute event_attributes[] = {
	{ "type", event_type, NULL },
	{ "target", event_target, NULL },
	{ "srcElement", event_target, NULL },
	{ "currentTarget", event_current_target, NULL },
	{ "eventPhase", event_phase, NULL },
	{ "bubbles", event_bubbles, NULL },
	{ "cancelable", event_cancelable, NULL },
	{ "composed", event_composed, NULL },
	{ "defaultPrevented", event_default_prevented, NULL },
	{ "isTrusted", event_is_trusted, NULL },
	{ "timeStamp", event_time_stamp, NULL },
	{ NULL, NULL, NULL }
};

/*
 * The operations of Event.  The table is constant for the life of the
 * program.
 */
static const struct bind_operation event_operations[] = {
	{ "initEvent", 1, event_init },
	{ "preventDefault", 0, event_prevent_default },
	{ "stopPropagation", 0, event_stop_propagation },
	{ "stopImmediatePropagation", 0, event_stop_immediate },
	{ NULL, 0, NULL }
};

/*
 * The constants of Event: the phases.  The table is constant for the life
 * of the program.
 */
static const struct bind_constant event_constants[] = {
	{ "NONE", BIND_PHASE_NONE },
	{ "CAPTURING_PHASE", BIND_PHASE_CAPTURING },
	{ "AT_TARGET", BIND_PHASE_AT_TARGET },
	{ "BUBBLING_PHASE", BIND_PHASE_BUBBLING },
	{ NULL, 0 }
};

/*
 * The Event interface.
 */
const struct bind_interface bind_event_interface = {
	"Event", BIND_NO_PARENT, 1, event_construct, event_attributes, event_operations, event_constants
};

/* Read-only UI data is stored on the traced event state, not in wrapper expandos. */
static const struct bind_attribute ui_event_attributes[] = {
	{ "view", event_view, NULL },
	{ "detail", event_ui_detail, NULL },
	{ NULL, NULL, NULL }
};

/* The legacy UI initializer reuses the existing event dispatch state. */
static const struct bind_operation ui_event_operations[] = {
	{ "initUIEvent", 1, event_init_ui },
	{ NULL, 0, NULL }
};

/* The UIEvent prototype contributes the legacy initializer and UI data views. */
const struct bind_interface bind_ui_event_interface = {
	"UIEvent", BIND_EVENT, 1, NULL, ui_event_attributes, ui_event_operations, NULL
};

/*
 * The attributes of MouseEvent.  The table is constant for the life of
 * the program.
 */
static const struct bind_attribute mouse_event_attributes[] = {
	{ "clientX", event_client_x, NULL },
	{ "clientY", event_client_y, NULL },
	{ "x", event_client_x, NULL },
	{ "y", event_client_y, NULL },
	{ "pageX", event_page_x, NULL },
	{ "pageY", event_page_y, NULL },
	{ "button", event_button, NULL },
	{ "shiftKey", bind_event_shift_key, NULL },
	{ "ctrlKey", bind_event_ctrl_key, NULL },
	{ "altKey", bind_event_alt_key, NULL },
	{ "metaKey", bind_event_meta_key, NULL },
	{ NULL, NULL, NULL }
};

/*
 * The MouseEvent interface.
 */
const struct bind_interface bind_mouse_event_interface = {
	"MouseEvent", BIND_UI_EVENT, 1, event_construct_mouse, mouse_event_attributes, NULL, NULL
};

/*
 * The attributes of CustomEvent.  The table is constant for the life of
 * the program.
 */
static const struct bind_attribute custom_event_attributes[] = {
	{ "detail", event_detail, NULL },
	{ NULL, NULL, NULL }
};

/*
 * The CustomEvent interface.
 */
const struct bind_interface bind_custom_event_interface = {
	"CustomEvent", BIND_EVENT, 1, event_construct_custom, custom_event_attributes, NULL, NULL
};

/*
 * The attributes of MessageEvent.  data shares the event state's generic
 * detail slot; the remaining values are kept separately because pages use
 * them to distinguish messages from different browsing contexts.
 */
static const struct bind_attribute message_event_attributes[] = {
	{ "data", event_detail, NULL },
	{ "origin", event_message_origin, NULL },
	{ "lastEventId", event_message_last_id, NULL },
	{ "source", event_message_source, NULL },
	{ "ports", event_message_ports, NULL },
	{ NULL, NULL, NULL }
};

/* The MessageEvent interface. */
const struct bind_interface bind_message_event_interface = {
	"MessageEvent", BIND_EVENT, 1, event_construct_message, message_event_attributes, NULL, NULL
};

/* ErrorEvent exposes immutable native state rather than script-owned expandos. */
static const struct bind_attribute error_event_attributes[] = {
	{ "message", event_error_message, NULL },
	{ "filename", event_error_filename, NULL },
	{ "lineno", event_error_line, NULL },
	{ "colno", event_error_column, NULL },
	{ "error", event_error_value, NULL },
	{ NULL, NULL, NULL }
};

/* The native ErrorEvent prototype inherits the ordinary Event interface. */
const struct bind_interface bind_error_event_interface = {
	"ErrorEvent", BIND_EVENT, 1, event_construct_error, error_event_attributes, NULL, NULL
};

/* Helpers not referenced by constant initializers follow the file-scope tables. */
static int event_this(struct vm_realm *realm, vm_value this_value, struct bind_event **event);
static int event_listener_options(struct vm_realm *realm, vm_value options, int *capture, int *once);
static void event_forget_listener(struct bind_listeners *listeners, size_t index);
static int event_error_this(struct vm_realm *realm, vm_value receiver, struct bind_event **event);
static int event_error_scalar(struct vm_realm *realm, vm_value given, struct vm_string **text);
static int event_is_ui(int interface);
static int event_ui_this(struct vm_realm *realm, vm_value this_value, struct bind_event **event);
static int event_window_value(struct vm_realm *realm, vm_value given, vm_value *view);
static void event_initialize(struct bind_event *event, struct vm_string *type, int bubbles, int cancelable);
static int event_build_path(struct bind_window *window, vm_value target, const struct bind_event *event, struct vm_object *path);
static int event_invoke(struct bind_window *window, vm_value current, vm_value event_value, int phase, unsigned kinds);
static int event_call_listener(struct bind_window *window, vm_value callback, vm_value current, vm_value event_value, int legacy_error, vm_value *returned);
static int event_is_window_error(struct bind_window *window, vm_value current, const struct bind_event *event, int handler);
static int event_fire(struct bind_window *window, struct dom_node *target, int interface, const char *type, unsigned flags, const struct bind_mouse *mouse, int *canceled);

/*
 * Reports the state of the event an Event object stands for, or NULL
 * when the value is not an Event.
 */
struct bind_event *
bind_event_of(
	vm_value value)
{
	struct vm_object *object;
	struct vm_cell *cell;
	int is_object;
	int is_cell;

	/* Only an object can be an event. */
	is_object = vm_value_is_object(value);
	if (!is_object)
		return NULL;

	/* A platform object whose cell is an event's state. */
	object = (struct vm_object *)vm_value_as_cell(value);
	if (object->kind != VM_KIND_PLATFORM)
		return NULL;
	is_cell = vm_value_is_cell(object->internal);
	if (!is_cell)
		return NULL;
	cell = vm_value_as_cell(object->internal);
	if (cell->type != &event_type_cell)
		return NULL;

	/* The state. */
	return (struct bind_event *)cell;
}

/*
 * Makes an event of an interface (BIND_EVENT or one derived from it) with
 * a type: its state, not dispatched, and its object.
 */
int
bind_event_create(
	struct bind_window *window,
	int interface,
	struct vm_string *type,
	vm_value *value,
	struct bind_event **event)
{
	struct bind_event *state;
	struct vm_object *object;
	struct vm_cell *state_root;
	int status;

	/* The state, zeroed: no flags, no phase. */
	state = vm_heap_alloc(window->realm->heap, &event_type_cell, sizeof(*state));
	if (state == NULL)
		return ENOMEM;
	state->type = type;
	state->target = VM_VALUE_NULL;
	state->current_target = VM_VALUE_NULL;
	state->detail = VM_VALUE_NULL;

	/* A missing ErrorEventInit.error is undefined, including factory-created state. */
	if (interface == BIND_ERROR_EVENT)
		state->detail = VM_VALUE_UNDEFINED;

	/* Initialize the remaining native references before publishing the wrapper. */
	state->view = VM_VALUE_NULL;
	state->submitter = VM_VALUE_NULL;
	state->interface = interface;
	state->initialized = 1;

	/* UI-derived events start with numerical detail while custom payloads keep null. */
	if (interface == BIND_UI_EVENT ||
	    interface == BIND_MOUSE_EVENT ||
	    interface == BIND_KEYBOARD_EVENT ||
	    interface == BIND_FOCUS_EVENT ||
	    interface == BIND_WHEEL_EVENT)
		state->detail = vm_value_int32(0);

	/* Other message and dispatch references begin empty. */
	state->source = VM_VALUE_NULL;
	state->ports = VM_VALUE_NULL;
	state->time_stamp = window->now;

	/* The object with the interface's prototype. */
	state_root = &state->cell;
	status = vm_heap_add_root(window->realm->heap, &state_root);
	if (status != 0)
		return status;
	object = vm_object_create(window->realm->heap, window->prototypes[interface]);
	if (object == NULL) {
		vm_heap_remove_root(window->realm->heap, &state_root);
		return ENOMEM;
	}

	/* The wrapper now owns the event state through its internal slot. */
	object->kind = VM_KIND_PLATFORM;
	object->internal = vm_value_cell(state);
	vm_heap_remove_root(window->realm->heap, &state_root);

	/* Succeeded: the event is made. */
	*value = vm_value_cell(object);
	*event = state;
	return 0;
}

/*
 * Dispatches an event at a target (a node's object, the window, or an
 * EventTarget of its own); *canceled says whether a listener canceled it.
 */
int
bind_dispatch(
	struct bind_window *window,
	vm_value target,
	vm_value event_value,
	int *canceled)
{
	struct bind_event *event;
	struct vm_object *path;
	struct vm_cell *roots[3];
	unsigned root_index;
	vm_value current;
	uint32_t index;
	int status;

	/* Retain target, event and the detached path through arbitrary listener-triggered GC. */
	roots[0] = vm_value_as_cell(target);
	roots[1] = vm_value_as_cell(event_value);
	roots[2] = NULL;
	for (root_index = 0; root_index < 3U; root_index++) {
		status = vm_heap_add_root(window->realm->heap, &roots[root_index]);
		if (status != 0) {
			/* Failed registration unwinds only slots which this dispatch actually owns. */
			for (index = 0; index < root_index; index++)
				vm_heap_remove_root(window->realm->heap, &roots[index]);
			return status;
		}
	}

	/* The event is being dispatched at the target. */
	*canceled = 0;
	event = bind_event_of(event_value);
	event->dispatching = 1;
	event->target = target;
	if (event->target_override != VM_VALUE_EMPTY)
		event->target = event->target_override;
	event->stop = 0;
	event->stop_immediate = 0;

	/* The path from the target up. */
	path = NULL;
	status = bind_array_create(window->realm, &path);
	if (status == 0) {
		roots[2] = &path->cell;
		status = event_build_path(window, target, event, path);
	}

	/* The capture phase, from the top down to the target's parent. */
	index = 0;
	if (status == 0)
		index = path->length;
	for (;
	     status == 0 && index > 1U;
	     index--) {
		current = path->elements[index - 1U];
		status = event_invoke(window, current, event_value, BIND_PHASE_CAPTURING, EVENT_INVOKE_CAPTURE);
	}

	/* The target: its capture listeners, then its handler and the others. */
	if (status == 0)
		status = event_invoke(window, target, event_value, BIND_PHASE_AT_TARGET, EVENT_INVOKE_CAPTURE);
	if (status == 0)
		status = event_invoke(window, target, event_value, BIND_PHASE_AT_TARGET, EVENT_INVOKE_BUBBLE);

	/* The bubble phase, from the target's parent up, for an event that bubbles. */
	for (index = 1; status == 0 && event->bubbles && index < path->length; index++) {
		current = path->elements[index];
		status = event_invoke(window, current, event_value, BIND_PHASE_BUBBLING, EVENT_INVOKE_BUBBLE);
	}

	/* The dispatch is over: no phase, no current target. */
	event->dispatching = 0;
	event->phase = BIND_PHASE_NONE;
	event->current_target = VM_VALUE_NULL;
	event->stop = 0;
	event->stop_immediate = 0;

	/* Every path/event root is removed after dispatch state is reset, including failures. */
	for (root_index = 0; root_index < 3U; root_index++)
		vm_heap_remove_root(window->realm->heap, &roots[root_index]);
	if (status != 0)
		return status;

	/* Succeeded: whether it was canceled is reported. */
	*canceled = event->canceled;
	return 0;
}

/*
 * Finds the listeners of an event target, making the empty set when
 * create is set and there is none; *listeners is NULL for a target without
 * any.  Returns EINVAL for a value that is not an event target.
 */
int
bind_listeners_of(
	struct bind_window *window,
	vm_value target,
	int create,
	struct bind_listeners **listeners)
{
	struct dom_node *node;
	struct vm_object *object;
	struct vm_cell *cell;
	struct vm_cell **slot;
	vm_value global;
	int is_object;
	int is_cell;

	/* A node keeps its listeners, and so does the window. */
	*listeners = NULL;
	slot = NULL;
	node = bind_node_of(target);
	global = vm_value_cell(window->realm->global);
	if (node != NULL) {
		slot = &node->listeners;
	} else if (target == global) {
		slot = &window->listeners;
	}

	/* An EventTarget of its own is a platform object whose cell is the listeners. */
	is_object = vm_value_is_object(target);
	if (slot == NULL && is_object) {
		object = (struct vm_object *)vm_value_as_cell(target);
		is_cell = vm_value_is_cell(object->internal);
		if (object->kind == VM_KIND_PLATFORM && is_cell) {
			cell = vm_value_as_cell(object->internal);
			if (cell->type == &listeners_type) {
				*listeners = (struct bind_listeners *)cell;
				return 0;
			}
		}
	}

	/* Anything else is not an event target. */
	if (slot == NULL)
		return EINVAL;

	/* A target without listeners gets an empty set when one is wanted. */
	if (*slot == NULL && create) {
		*slot = vm_heap_alloc(window->realm->heap, &listeners_type, sizeof(struct bind_listeners));
		if (*slot == NULL)
			return ENOMEM;
	}

	/* Succeeded: the listeners (or NULL) are found. */
	*listeners = (struct bind_listeners *)*slot;
	return 0;
}

/*
 * Adds a listener to a target's listeners at a position (the count for
 * the end), the listeners from there on moving up one place.
 */
int
bind_listeners_add(
	struct bind_listeners *listeners,
	const struct bind_listener *listener,
	size_t position)
{
	struct bind_listener *items;
	size_t capacity;

	/* The array grows when it is full. */
	if (listeners->count == listeners->capacity) {
		capacity = listeners->capacity * 2U;
		if (capacity < 4U)
			capacity = 4U;
		items = realloc(listeners->items, capacity * sizeof(*items));
		if (items == NULL)
			return ENOMEM;
		listeners->items = items;
		listeners->capacity = capacity;
	}

	/* The listener at its position, the ones after it moved up one place. */
	if (position > listeners->count)
		position = listeners->count;
	memmove(&listeners->items[position + 1U], &listeners->items[position], (listeners->count - position) * sizeof(listeners->items[0]));
	listeners->items[position] = *listener;

	/* Succeeded: the target has one listener more. */
	listeners->count++;
	return 0;
}

/*
 * Finds a listener by its type, callback, capture flag and whether it is
 * an event handler; nonzero when found.
 */
int
bind_listeners_find(
	const struct bind_listeners *listeners,
	const struct vm_string *type,
	vm_value callback,
	int capture,
	int handler,
	size_t *index)
{
	const struct bind_listener *listener;
	size_t item;

	/* Compares each listener. */
	for (item = 0; item < listeners->count; item++) {
		listener = &listeners->items[item];
		if (listener->type != type)
			continue;
		if (listener->callback != callback)
			continue;
		if (listener->capture != capture)
			continue;
		if (listener->handler != handler)
			continue;
		*index = item;
		return 1;
	}

	/* No such listener. */
	return 0;
}

/*
 * Finds a target's event handler listener for a type; nonzero when
 * found.
 */
int
bind_listeners_find_handler(
	const struct bind_listeners *listeners,
	const struct vm_string *type,
	size_t *index)
{
	size_t item;

	/* The listener marked as the type's handler. */
	for (item = 0; item < listeners->count; item++) {
		if (!listeners->items[item].handler)
			continue;
		if (listeners->items[item].type != type)
			continue;
		*index = item;
		return 1;
	}

	/* The target has no handler for the type. */
	return 0;
}

/*
 * Fires a trusted event of a type at a node, or at the window for NULL
 * (the page's load events and the like).
 */
int
bind_fire_event(
	struct bind_window *window,
	struct dom_node *target,
	const char *type,
	unsigned flags,
	int *canceled)
{
	int status;

	/* A plain Event. */
	status = event_fire(window, target, BIND_EVENT, type, flags, NULL, canceled);
	if (status != 0)
		return status;

	/* Succeeded: the event is dispatched. */
	return 0;
}

/*
 * Fires a trusted mouse event (it bubbles and can be canceled) at a node.
 */
int
bind_fire_mouse_event(
	struct bind_window *window,
	struct dom_node *target,
	const char *type,
	const struct bind_mouse *mouse,
	int *canceled)
{
	int status;

	/* A MouseEvent at the pointer's place. */
	status = event_fire(window, target, BIND_MOUSE_EVENT, type, BIND_EVENT_BUBBLES | BIND_EVENT_CANCELABLE, mouse, canceled);
	if (status != 0)
		return status;

	/* Succeeded: the event is dispatched. */
	return 0;
}

/*
 * Makes an event of an interface from a constructor's type and init
 * (bubbles, cancelable), for the constructors of the Event interfaces.
 */
int
bind_event_construct(
	struct vm_realm *realm,
	int interface,
	const vm_value *args,
	unsigned count,
	vm_value *result,
	struct bind_event **event)
{
	struct bind_window *window;
	struct vm_string *type;
	struct vm_cell *event_root;
	struct vm_object *event_object;
	struct vm_object *subclass;
	vm_value value;
	int present;
	int status;

	/* The type is required. */
	window = bind_window_of(realm);
	if (count < 1U) {
		status = vm_throw_type_error(realm, "Failed to construct the event: 1 argument required, but only 0 present.");
		return status;
	}

	/* The type's atom. */
	status = bind_to_atom(realm, args[0], 0, &type);
	if (status != 0)
		return status;

	/* The event. */
	status = bind_event_create(window, interface, type, result, event);
	if (status != 0)
		return status;
	event_root = vm_value_as_cell(*result);
	status = vm_heap_add_root(realm->heap, &event_root);
	if (status != 0)
		return status;

	/* A class that extends the interface gets its own prototype (new.target's). */
	event_object = (struct vm_object *)vm_value_as_cell(*result);
	status = vm_construct_prototype(realm, realm->new_target, event_object->prototype, &subclass);
	if (status != 0)
		goto cleanup;
	event_object->prototype = subclass;

	/* The init's flags. */
	status = bind_get_option(realm, js_argument(args, count, 1), "bubbles", &present, &value);
	if (status != 0)
		goto cleanup;
	(*event)->bubbles = vm_to_boolean(value);
	status = bind_get_option(realm, js_argument(args, count, 1), "cancelable", &present, &value);
	if (status != 0)
		goto cleanup;
	(*event)->cancelable = vm_to_boolean(value);

	/* The constructed wrapper now carries the initialized state. */
cleanup:
	vm_heap_remove_root(realm->heap, &event_root);
	if (status != 0)
		return status;

	/* Succeeded: the event is made. */
	return 0;
}

/*
 * Takes a MouseEvent's init (clientX, clientY, button and the modifiers)
 * into an event made by a constructor; the page coordinates copy the
 * client ones, as there is no scroll to add.
 */
int
bind_event_init_mouse(
	struct vm_realm *realm,
	vm_value init,
	struct bind_event *event)
{
	vm_value value;
	double number;
	int present;
	int status;

	/* The horizontal place. */
	status = bind_get_option(realm, init, "clientX", &present, &value);
	if (status != 0)
		return status;
	if (present) {
		status = vm_to_number(realm, value, &number);
		if (status != 0)
			return status;
		event->mouse.client_x = number;
		event->mouse.page_x = number;
	}

	/* The vertical place. */
	status = bind_get_option(realm, init, "clientY", &present, &value);
	if (status != 0)
		return status;
	if (present) {
		status = vm_to_number(realm, value, &number);
		if (status != 0)
			return status;
		event->mouse.client_y = number;
		event->mouse.page_y = number;
	}

	/* The button. */
	status = bind_get_option(realm, init, "button", &present, &value);
	if (status != 0)
		return status;
	if (present) {
		status = vm_to_number(realm, value, &number);
		if (status != 0)
			return status;
		event->mouse.button = (int)number;
	}

	/* The modifiers. */
	status = bind_event_init_modifiers(realm, init, event);
	if (status != 0)
		return status;

	/* Succeeded: the init is taken. */
	return 0;
}

/*
 * Makes a trusted event of an interface for the browser to fire at a
 * node, or at the window for NULL, with the flags asked for (bubbles,
 * cancelable, the document as the load's target): the event's object and
 * state, and the target's object, for the caller to fill in and pass to
 * bind_dispatch.
 */
int
bind_event_prepare(
	struct bind_window *window,
	struct dom_node *target,
	int interface,
	const char *type,
	unsigned flags,
	vm_value *event_value,
	vm_value *target_value,
	struct bind_event **event)
{
	struct vm_string *name;
	struct vm_cell *event_root;
	struct vm_cell *target_root;
	int status;

	/* The type's atom and the event. */
	name = vm_atom_from_ascii(window->realm->heap, type);
	if (name == NULL)
		return ENOMEM;
	status = bind_event_create(window, interface, name, event_value, event);
	if (status != 0)
		return status;
	event_root = vm_value_as_cell(*event_value);
	status = vm_heap_add_root(window->realm->heap, &event_root);
	if (status != 0)
		return status;
	*target_value = vm_value_cell(window->realm->global);
	target_root = vm_value_as_cell(*target_value);
	status = vm_heap_add_root(window->realm->heap, &target_root);
	if (status != 0) {
		vm_heap_remove_root(window->realm->heap, &event_root);
		return status;
	}

	/*
	 * The browser fires it: trusted, with the flags asked for.  bubbles and
	 * cancelable are what listeners see and what preventDefault honors.
	 */
	(*event)->trusted = 1;
	if ((flags & BIND_EVENT_BUBBLES) != 0)
		(*event)->bubbles = 1;
	if ((flags & BIND_EVENT_CANCELABLE) != 0)
		(*event)->cancelable = 1;

	/* The target: the node's object, or the window. */
	if (target != NULL) {
		status = bind_wrap(window, target, target_value);
		if (status != 0)
			goto cleanup;
		target_root = vm_value_as_cell(*target_value);
	}

	/* The window's load reports the document as its target. */
	if ((flags & BIND_EVENT_DOCUMENT) != 0) {
		status = bind_wrap(window, &window->document->node, &(*event)->target_override);
		if (status != 0)
			goto cleanup;
	}

	/* Both values are handed to dispatch after wrapping is complete. */
cleanup:
	vm_heap_remove_root(window->realm->heap, &target_root);
	vm_heap_remove_root(window->realm->heap, &event_root);
	if (status != 0)
		return status;

	/* Succeeded: the event is ready to dispatch. */
	return 0;
}

/*
 * Creates an uninitialized legacy Event or UIEvent for a branded Document.
 */
int
bind_document_create_event(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *node;
	struct bind_window *window;
	struct bind_event *event;
	struct vm_string *name;
	struct vm_string *type;
	int interface;
	int matched;
	int error;

	/* A borrowed factory still requires an actual Document receiver. */
	node = bind_node_of(this_value);
	if (node == NULL || node->type != DOM_DOCUMENT) {
		error = bind_throw_illegal(realm);
		if (error != 0)
			return error;

		/* Invalid hosts cannot proceed without a real Document. */
		return EINVAL;
	}

	/* The required interface name is converted exactly once with ASCII folding. */
	if (count < 1U) {
		error = vm_throw_type_error(realm, "createEvent requires an interface name.");
		return error;
	}

	/* Folded atoms let aliases match without running any additional user conversions. */
	error = bind_to_atom(realm, args[0], 1, &name);
	if (error != 0)
		return error;

	/* The implemented Event aliases are the standard legacy spellings. */
	interface = BIND_NO_PARENT;
	matched = vm_string_equal_ascii(name, "event");
	if (matched)
		interface = BIND_EVENT;

	/* The historical plural aliases still produce an ordinary Event. */
	matched = vm_string_equal_ascii(name, "events");
	if (matched)
		interface = BIND_EVENT;

	/* HTML and SVG event aliases do not select separate interfaces. */
	matched = vm_string_equal_ascii(name, "htmlevents");
	if (matched)
		interface = BIND_EVENT;

	/* SVGEvents also aliases the base Event interface. */
	matched = vm_string_equal_ascii(name, "svgevents");
	if (matched)
		interface = BIND_EVENT;

	/* UIEvent names select the interface with view, detail and initUIEvent. */
	matched = vm_string_equal_ascii(name, "uievent");
	if (matched)
		interface = BIND_UI_EVENT;

	/* Its historical plural is accepted with the same ASCII case folding. */
	matched = vm_string_equal_ascii(name, "uievents");
	if (matched)
		interface = BIND_UI_EVENT;

	/* Other legacy factories remain outside this scoped foundation. */
	if (interface == BIND_NO_PARENT) {
		error = bind_throw_dom(realm, "NotSupportedError", "This legacy event interface is not supported.");
		return error;
	}

	/* The receiver's relevant Window supplies its event prototype when it has one. */
	window = node->document->view;
	if (window == NULL)
		window = bind_window_of(realm);

	/* Factories begin with an empty type and an unset initialized flag. */
	type = vm_atom_from_ascii(realm->heap, "");
	if (type == NULL)
		return ENOMEM;

	/* Builds the normal traced state and wrapper before changing its factory readiness. */
	error = bind_event_create(window, interface, type, result, &event);
	if (error != 0)
		return error;

	/* dispatchEvent observes this flag until a legacy initializer succeeds. */
	event->initialized = 0;

	/* Succeeded: the caller owns a real, initially uninitialized event. */
	return 0;
}

/* Marks what an event's state refers to. */
static void
event_trace(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct bind_event *event;

	/* The type, the targets and the values carried by an event. */
	event = (struct bind_event *)cell;
	if (event->type != NULL)
		vm_heap_mark(heap, &event->type->cell);
	vm_heap_mark_value(heap, event->target);
	vm_heap_mark_value(heap, event->current_target);
	vm_heap_mark_value(heap, event->detail);
	vm_heap_mark_value(heap, event->view);
	vm_heap_mark_value(heap, event->submitter);
	vm_heap_mark_value(heap, event->source);
	vm_heap_mark_value(heap, event->ports);
	vm_heap_mark_value(heap, event->target_override);
	if (event->origin != NULL)
		vm_heap_mark(heap, &event->origin->cell);
	if (event->last_event_id != NULL)
		vm_heap_mark(heap, &event->last_event_id->cell);

	/* ErrorEvent strings remain reachable through every retained event wrapper. */
	if (event->error_message != NULL)
		vm_heap_mark(heap, &event->error_message->cell);

	/* Filename is independently converted to Unicode scalar text. */
	if (event->error_filename != NULL)
		vm_heap_mark(heap, &event->error_filename->cell);

	/* A key event's key and code. */
	if (event->key != NULL)
		vm_heap_mark(heap, &event->key->cell);
	if (event->code != NULL)
		vm_heap_mark(heap, &event->code->cell);
}

/* Marks the types and callbacks of a target's listeners. */
static void
listeners_trace(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct bind_listeners *listeners;
	size_t index;

	/* Each listener's type and callback. */
	listeners = (struct bind_listeners *)cell;
	for (index = 0; index < listeners->count; index++) {
		vm_heap_mark(heap, &listeners->items[index].type->cell);
		vm_heap_mark_value(heap, listeners->items[index].callback);
	}
}

/* Frees the array of a dead target's listeners. */
static void
listeners_finalize(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct bind_listeners *listeners;

	UNUSED_PARAMETER(heap);

	/* The array is the only thing outside the heap. */
	listeners = (struct bind_listeners *)cell;
	free(listeners->items);
}

/* Finds the event state a method's this value stands for, throwing a TypeError otherwise. */
static int
event_this(
	struct vm_realm *realm,
	vm_value this_value,
	struct bind_event **event)
{
	int status;

	/* The state, or the error of a method on the wrong object. */
	*event = bind_event_of(this_value);
	if (*event == NULL) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Succeeded: the event is found. */
	return 0;
}

/* Makes an event target of its own, with an empty set of listeners (new EventTarget()). */
static int
event_target_construct(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct vm_object *object;
	struct vm_object *prototype;
	struct vm_cell *listeners;
	struct vm_cell *listeners_root;
	int status;

	UNUSED_PARAMETER(this_value);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The empty set of listeners. */
	window = bind_window_of(realm);
	listeners = vm_heap_alloc(realm->heap, &listeners_type, sizeof(struct bind_listeners));
	if (listeners == NULL)
		return ENOMEM;

	/* The object that holds them. */
	listeners_root = listeners;
	status = vm_heap_add_root(realm->heap, &listeners_root);
	if (status != 0)
		return status;
	status = vm_construct_prototype(realm, realm->new_target, window->prototypes[BIND_EVENT_TARGET], &prototype);
	if (status != 0) {
		vm_heap_remove_root(realm->heap, &listeners_root);
		return status;
	}
	object = vm_object_create(realm->heap, prototype);
	if (object == NULL) {
		vm_heap_remove_root(realm->heap, &listeners_root);
		return ENOMEM;
	}

	/* The wrapper now owns the listener cell through its internal slot. */
	object->kind = VM_KIND_PLATFORM;
	object->internal = vm_value_cell(listeners);
	vm_heap_remove_root(realm->heap, &listeners_root);

	/* Succeeded: the target is made. */
	*result = vm_value_cell(object);
	return 0;
}

/* Adds a listener for a type, unless the same one is there (addEventListener). */
static int
event_add_listener(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct bind_listeners *listeners;
	struct bind_listener listener;
	struct vm_string *type;
	vm_value target;
	vm_value callback;
	size_t index;
	int capture;
	int once;
	int found;
	int status;

	/* The target (a function called plainly is the window's). */
	*result = VM_VALUE_UNDEFINED;
	window = bind_window_of(realm);
	target = this_value;
	if (target == VM_VALUE_UNDEFINED || target == VM_VALUE_NULL)
		target = vm_value_cell(realm->global);

	/* The type, the callback and the options. */
	status = bind_to_atom(realm, js_argument(args, count, 0), 0, &type);
	if (status != 0)
		return status;
	callback = js_argument(args, count, 1);
	status = event_listener_options(realm, js_argument(args, count, 2), &capture, &once);
	if (status != 0)
		return status;

	/* A null callback adds nothing. */
	if (callback == VM_VALUE_NULL || callback == VM_VALUE_UNDEFINED)
		return 0;

	/* The target's listeners. */
	status = bind_listeners_of(window, target, 1, &listeners);
	if (status == EINVAL) {
		status = bind_throw_illegal(realm);
		return status;
	} else if (status != 0) {
		return status;
	}

	/* The same listener is added once. */
	found = bind_listeners_find(listeners, type, callback, capture, 0, &index);
	if (found)
		return 0;

	/* The listener at the end. */
	listener.type = type;
	listener.callback = callback;
	listener.capture = capture;
	listener.once = once;
	listener.handler = 0;
	listener.generation = window->document->generation;
	status = bind_listeners_add(listeners, &listener, listeners->count);
	if (status != 0)
		return status;

	/* Succeeded: the listener is added. */
	return 0;
}

/* Removes a listener for a type (removeEventListener). */
static int
event_remove_listener(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct bind_listeners *listeners;
	struct vm_string *type;
	vm_value target;
	size_t index;
	int capture;
	int once;
	int found;
	int status;

	/* The target (a function called plainly is the window's). */
	*result = VM_VALUE_UNDEFINED;
	window = bind_window_of(realm);
	target = this_value;
	if (target == VM_VALUE_UNDEFINED || target == VM_VALUE_NULL)
		target = vm_value_cell(realm->global);

	/* The type and the options. */
	status = bind_to_atom(realm, js_argument(args, count, 0), 0, &type);
	if (status != 0)
		return status;
	status = event_listener_options(realm, js_argument(args, count, 2), &capture, &once);
	if (status != 0)
		return status;

	/* The target's listeners. */
	status = bind_listeners_of(window, target, 0, &listeners);
	if (status == EINVAL) {
		status = bind_throw_illegal(realm);
		return status;
	} else if (status != 0) {
		return status;
	}

	/* The listener, if the target has it. */
	if (listeners == NULL)
		return 0;
	found = bind_listeners_find(listeners, type, js_argument(args, count, 1), capture, 0, &index);
	if (found)
		event_forget_listener(listeners, index);

	/* Succeeded: the listener is gone. */
	return 0;
}

/* Dispatches an event a script made, and reports whether no listener canceled it (dispatchEvent). */
static int
event_dispatch_method(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct bind_listeners *listeners;
	struct bind_event *event;
	struct dom_node *node;
	vm_value target;
	int canceled;
	int status;

	/* The target (a function called plainly is the window's). */
	window = bind_window_of(realm);
	target = this_value;
	if (target == VM_VALUE_UNDEFINED || target == VM_VALUE_NULL)
		target = vm_value_cell(realm->global);
	status = bind_listeners_of(window, target, 0, &listeners);
	if (status == EINVAL) {
		status = bind_throw_illegal(realm);
		return status;
	} else if (status != 0) {
		return status;
	}

	/* The event, which must not be in a dispatch already. */
	event = bind_event_of(js_argument(args, count, 0));
	if (event == NULL) {
		status = vm_throw_type_error(realm, "Failed to execute 'dispatchEvent': parameter 1 is not of type 'Event'.");
		return status;
	} else if (event->dispatching || !event->initialized) {
		status = bind_throw_dom(realm, "InvalidStateError", "The event is uninitialized or already being dispatched.");
		return status;
	}

	/* A script's event is not trusted. */
	event->trusted = 0;
	status = bind_dispatch(window, target, js_argument(args, count, 0), &canceled);
	if (status != 0)
		return status;

	/* A click nobody canceled activates the link it landed in. */
	node = bind_node_of(target);
	if (!canceled && event->interface == BIND_MOUSE_EVENT && node != NULL && node->type == DOM_ELEMENT &&
	    vm_string_equal_ascii(event->type, "click")) {
		status = bind_click_follow(window, (struct dom_element *)node);
		if (status != 0)
			return status;
	}

	/* True unless a listener canceled it. */
	*result = VM_VALUE_TRUE;
	if (canceled)
		*result = VM_VALUE_FALSE;

	/* Succeeded: the event is dispatched. */
	return 0;
}

/* Reads addEventListener's third argument: a Boolean capture, or a dictionary with capture and once. */
static int
event_listener_options(
	struct vm_realm *realm,
	vm_value options,
	int *capture,
	int *once)
{
	vm_value value;
	int present;
	int is_object;
	int status;

	/* A value that is not an object is the capture flag. */
	*capture = 0;
	*once = 0;
	is_object = vm_value_is_object(options);
	if (!is_object) {
		*capture = vm_to_boolean(options);
		return 0;
	}

	/* The dictionary's members. */
	status = bind_get_option(realm, options, "capture", &present, &value);
	if (status != 0)
		return status;
	*capture = vm_to_boolean(value);
	status = bind_get_option(realm, options, "once", &present, &value);
	if (status != 0)
		return status;
	*once = vm_to_boolean(value);

	/* Succeeded: the options are read. */
	return 0;
}

/* Removes the listener at an index, keeping the others in order. */
static void
event_forget_listener(
	struct bind_listeners *listeners,
	size_t index)
{
	size_t after;

	/* The listeners after it move down one place. */
	after = listeners->count - index - 1U;
	memmove(&listeners->items[index], &listeners->items[index + 1U], after * sizeof(listeners->items[0]));
	listeners->count--;
}

/* Makes an Event (new Event(type, init)). */
static int
event_construct(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_event *event;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The event with its type and init. */
	status = bind_event_construct(realm, BIND_EVENT, args, count, result, &event);
	if (status != 0)
		return status;

	/* Succeeded: the event is made. */
	return 0;
}

/* Makes a MouseEvent (new MouseEvent(type, init)) with the init's place, button and modifiers. */
static int
event_construct_mouse(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_event *event;
	struct vm_cell *event_root;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The event with its type and init. */
	status = bind_event_construct(realm, BIND_MOUSE_EVENT, args, count, result, &event);
	if (status != 0)
		return status;

	/* The pointer's part of the init. */
	event_root = vm_value_as_cell(*result);
	status = vm_heap_add_root(realm->heap, &event_root);
	if (status != 0)
		return status;
	status = bind_event_init_mouse(realm, js_argument(args, count, 1), event);
	vm_heap_remove_root(realm->heap, &event_root);
	if (status != 0)
		return status;

	/* Succeeded: the event is made. */
	return 0;
}

/* Makes a CustomEvent (new CustomEvent(type, init)) with the init's detail. */
static int
event_construct_custom(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_event *event;
	struct vm_cell *event_root;
	vm_value value;
	int present;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The event with its type and init. */
	status = bind_event_construct(realm, BIND_CUSTOM_EVENT, args, count, result, &event);
	if (status != 0)
		return status;

	/* The detail. */
	event_root = vm_value_as_cell(*result);
	status = vm_heap_add_root(realm->heap, &event_root);
	if (status != 0)
		return status;
	status = bind_get_option(realm, js_argument(args, count, 1), "detail", &present, &value);
	if (status == 0 && present)
		event->detail = value;
	vm_heap_remove_root(realm->heap, &event_root);
	if (status != 0)
		return status;

	/* Succeeded: the event is made. */
	return 0;
}

/* Makes a MessageEvent (new MessageEvent(type, init)) with its message fields. */
static int
event_construct_message(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_event *event;
	struct vm_string *text;
	struct vm_cell *event_root;
	vm_value value;
	int present;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The event with its type and common init. */
	status = bind_event_construct(realm, BIND_MESSAGE_EVENT, args, count, result, &event);
	if (status != 0)
		return status;
	event_root = vm_value_as_cell(*result);
	status = vm_heap_add_root(realm->heap, &event_root);
	if (status != 0)
		return status;

	/* Its cloned value (the constructor keeps the value itself). */
	status = bind_get_option(realm, js_argument(args, count, 1), "data", &present, &value);
	if (status != 0)
		goto cleanup;
	if (present)
		event->detail = value;

	/* The origin and the last event id are strings. */
	status = bind_get_option(realm, js_argument(args, count, 1), "origin", &present, &value);
	if (status != 0)
		goto cleanup;
	if (present) {
		status = bind_to_string(realm, value, &text);
		if (status != 0)
			goto cleanup;
		event->origin = text;
	}

	/* The second string follows the same conversion. */
	status = bind_get_option(realm, js_argument(args, count, 1), "lastEventId", &present, &value);
	if (status != 0)
		goto cleanup;
	if (present) {
		status = bind_to_string(realm, value, &text);
		if (status != 0)
			goto cleanup;
		event->last_event_id = text;
	}

	/* The sending context and transferred ports. */
	status = bind_get_option(realm, js_argument(args, count, 1), "source", &present, &value);
	if (status != 0)
		goto cleanup;
	if (present)
		event->source = value;
	status = bind_get_option(realm, js_argument(args, count, 1), "ports", &present, &value);
	if (status != 0)
		goto cleanup;
	if (present) {
		event->ports = value;
	} else {
		status = js_builtin_array(realm, NULL, 0, &event->ports);
		if (status != 0)
			goto cleanup;
	}

	/* The message fields are now stored on its traced event state. */
cleanup:
	vm_heap_remove_root(realm->heap, &event_root);
	if (status != 0)
		return status;

	/* Succeeded: the event is made. */
	return 0;
}

/* Constructs a genuine ErrorEvent after converting its complete inherited dictionary. */
static int
event_construct_error(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_event *event;
	struct vm_string *type;
	struct vm_cell *event_root;
	struct vm_object *wrapper;
	struct vm_object *prototype;
	vm_value init;
	vm_value member;
	int object;
	int present;
	int status;

	UNUSED_PARAMETER(this_value);

	/* A required type is converted before the optional dictionary is examined. */
	if (count < 1U) {
		status = vm_throw_type_error(realm, "ErrorEvent requires an event type.");
		return status;
	}

	/* Intern the converted type before allocating its traced native state. */
	status = bind_to_atom(realm, args[0], 0, &type);
	if (status != 0)
		return status;

	/* WebIDL dictionaries accept null and undefined, but reject other primitives. */
	init = js_argument(args, count, 1);
	object = vm_value_is_object(init);
	if (!object &&
	    init != VM_VALUE_NULL &&
	    init != VM_VALUE_UNDEFINED) {
		status = vm_throw_type_error(realm, "ErrorEventInit must be a dictionary.");
		return status;
	}

	/* Retain the wrapper and its entire partially converted state through reentrant getters. */
	status = bind_event_create(bind_window_of(realm), BIND_ERROR_EVENT, type, result, &event);
	if (status != 0)
		return status;
	event_root = vm_value_as_cell(*result);
	status = vm_heap_add_root(realm->heap, &event_root);
	if (status != 0)
		return status;

	/* Convert inherited EventInit members in lexical order before derived members. */
	status = bind_get_option(realm, init, "bubbles", &present, &member);
	if (status != 0)
		goto cleanup;
	event->bubbles = vm_to_boolean(member);
	status = bind_get_option(realm, init, "cancelable", &present, &member);
	if (status != 0)
		goto cleanup;
	event->cancelable = vm_to_boolean(member);
	status = bind_get_option(realm, init, "composed", &present, &member);
	if (status != 0)
		goto cleanup;
	event->composed = vm_to_boolean(member);

	/* Derived members follow colno, error, filename, lineno, message. */
	status = bind_get_option(realm, init, "colno", &present, &member);
	if (status != 0)
		goto cleanup;

	/* Convert an explicitly supplied column number. */
	if (present) {
		status = vm_to_uint32(realm, member, &event->error_column);
		if (status != 0)
			goto cleanup;
	}

	/* Store the exact arbitrary error value before any later getter can collect. */
	status = bind_get_option(realm, init, "error", &present, &member);
	if (status != 0)
		goto cleanup;
	event->detail = member;

	/* Filename is a USVString rather than an unmodified DOMString. */
	status = bind_get_option(realm, init, "filename", &present, &member);
	if (status != 0)
		goto cleanup;

	/* Convert an explicitly supplied source filename. */
	if (present) {
		status = event_error_scalar(realm, member, &event->error_filename);
		if (status != 0)
			goto cleanup;
	}

	/* Line numbers use ordinary WebIDL unsigned-long modulo conversion. */
	status = bind_get_option(realm, init, "lineno", &present, &member);
	if (status != 0)
		goto cleanup;

	/* Convert an explicitly supplied line number. */
	if (present) {
		status = vm_to_uint32(realm, member, &event->error_line);
		if (status != 0)
			goto cleanup;
	}

	/* Message preserves DOMString code units, including unmatched surrogates. */
	status = bind_get_option(realm, init, "message", &present, &member);
	if (status != 0)
		goto cleanup;

	/* Convert an explicitly supplied message. */
	if (present) {
		status = bind_to_string(realm, member, &event->error_message);
		if (status != 0)
			goto cleanup;
	}

	/* Only after argument conversion does subclass construction consult new.target.prototype. */
	wrapper = (struct vm_object *)vm_value_as_cell(*result);
	status = vm_construct_prototype(realm, realm->new_target, wrapper->prototype, &prototype);
	if (status != 0)
		goto cleanup;
	wrapper->prototype = prototype;

cleanup:
	/* The returned wrapper owns its state; failure releases the same temporary root. */
	vm_heap_remove_root(realm->heap, &event_root);
	if (status != 0)
		return status;

	/* Succeeded: the native immutable payload is ready for normal EventTarget dispatch. */
	return 0;
}

/* Repairs unmatched surrogate code units for ErrorEventInit.filename. */
static int
event_error_scalar(
	struct vm_realm *realm,
	vm_value given,
	struct vm_string **text)
{
	struct vm_string *converted;
	struct wb_buffer bytes;
	int status;

	/* Complete script conversion before extracting stable scalar bytes. */
	status = bind_to_string(realm, given, &converted);
	if (status != 0)
		return status;
	wb_buffer_init(&bytes);
	status = vm_string_to_utf8(converted, &bytes);
	if (status != 0) {
		wb_buffer_release(&bytes);
		return status;
	}

	/* The native state takes ownership before any later script getter runs. */
	*text = vm_string_from_utf8(realm->heap, wb_buffer_string(&bytes), bytes.length);
	wb_buffer_release(&bytes);
	if (*text == NULL)
		return ENOMEM;

	/* Succeeded: valid surrogate pairs survive and lone surrogates become U+FFFD. */
	return 0;
}

/* Requires the actual native ErrorEvent interface rather than a mutable prototype chain. */
static int
event_error_this(
	struct vm_realm *realm,
	vm_value receiver,
	struct bind_event **event)
{
	int status;

	/* Event identity is recovered only from the platform wrapper's native cell. */
	*event = bind_event_of(receiver);
	if (*event == NULL) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Other genuine Event subclasses cannot borrow ErrorEvent getters. */
	if ((*event)->interface != BIND_ERROR_EVENT) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Succeeded: the payload belongs to this interface. */
	return 0;
}

/* Reads the native ErrorEvent message without consulting script-owned properties. */
static int
event_error_message(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_event *event;
	struct vm_string *text;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Reject forged receivers before reading any native payload. */
	status = event_error_this(realm, this_value, &event);
	if (status != 0)
		return status;

	/* An omitted string member has the empty-string default. */
	text = event->error_message;
	if (text == NULL) {
		text = vm_atom_from_ascii(realm->heap, "");
		if (text == NULL)
			return ENOMEM;
	}

	/* Succeeded: the event retains the published string. */
	*result = vm_value_cell(text);
	return 0;
}

/* Reads the native ErrorEvent filename without consulting script-owned properties. */
static int
event_error_filename(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_event *event;
	struct vm_string *text;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Reject forged receivers before reading any native payload. */
	status = event_error_this(realm, this_value, &event);
	if (status != 0)
		return status;

	/* An omitted string member has the empty-string default. */
	text = event->error_filename;
	if (text == NULL) {
		text = vm_atom_from_ascii(realm->heap, "");
		if (text == NULL)
			return ENOMEM;
	}

	/* Succeeded: the event retains the published string. */
	*result = vm_value_cell(text);
	return 0;
}

/* Reads the native ErrorEvent line without consulting script-owned properties. */
static int
event_error_line(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_event *event;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Reject forged receivers before reading any native payload. */
	status = event_error_this(realm, this_value, &event);
	if (status != 0)
		return status;

	/* Succeeded: the full unsigned range is represented exactly as a Number. */
	*result = vm_value_number((double)event->error_line);
	return 0;
}

/* Reads the native ErrorEvent column without consulting script-owned properties. */
static int
event_error_column(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_event *event;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Reject forged receivers before reading any native payload. */
	status = event_error_this(realm, this_value, &event);
	if (status != 0)
		return status;

	/* Succeeded: the full unsigned range is represented exactly as a Number. */
	*result = vm_value_number((double)event->error_column);
	return 0;
}

/* Reads the native ErrorEvent value without consulting script-owned properties. */
static int
event_error_value(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_event *event;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Reject forged receivers before reading any native payload. */
	status = event_error_this(realm, this_value, &event);
	if (status != 0)
		return status;

	/* Succeeded: arbitrary values preserve their original identity. */
	*result = event->detail;
	return 0;
}

/* Reports the event's type (type). */
static int
event_type(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_event *event;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The event. */
	status = event_this(realm, this_value, &event);
	if (status != 0)
		return status;

	/* Succeeded: the type is reported. */
	*result = vm_value_cell(event->type);
	return 0;
}

/* Reports the event's target (target, srcElement). */
static int
event_target(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_event *event;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The event. */
	status = event_this(realm, this_value, &event);
	if (status != 0)
		return status;

	/* Succeeded: the target is reported. */
	*result = event->target;
	return 0;
}

/* Reports the target whose listeners run now (currentTarget). */
static int
event_current_target(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_event *event;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The event. */
	status = event_this(realm, this_value, &event);
	if (status != 0)
		return status;

	/* Succeeded: the current target is reported. */
	*result = event->current_target;
	return 0;
}

/* Reports the dispatch's phase (eventPhase). */
static int
event_phase(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_event *event;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The event. */
	status = event_this(realm, this_value, &event);
	if (status != 0)
		return status;

	/* Succeeded: the phase is reported. */
	*result = vm_value_int32(event->phase);
	return 0;
}

/* Reports whether the event bubbles (bubbles). */
static int
event_bubbles(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_event *event;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The event. */
	status = event_this(realm, this_value, &event);
	if (status != 0)
		return status;

	/* Succeeded: the flag is reported. */
	*result = vm_value_boolean(event->bubbles);
	return 0;
}

/* Reports whether the event can be canceled (cancelable). */
static int
event_cancelable(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_event *event;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The event. */
	status = event_this(realm, this_value, &event);
	if (status != 0)
		return status;

	/* Succeeded: the flag is reported. */
	*result = vm_value_boolean(event->cancelable);
	return 0;
}

/* Reports whether a listener canceled the event (defaultPrevented). */
static int
event_default_prevented(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_event *event;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The event. */
	status = event_this(realm, this_value, &event);
	if (status != 0)
		return status;

	/* Succeeded: the flag is reported. */
	*result = vm_value_boolean(event->canceled);
	return 0;
}

/* Reports whether the browser fired the event rather than a script (isTrusted). */
static int
event_is_trusted(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_event *event;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The event. */
	status = event_this(realm, this_value, &event);
	if (status != 0)
		return status;

	/* Succeeded: the flag is reported. */
	*result = vm_value_boolean(event->trusted);
	return 0;
}

/* Reports when the event was made, in the window's milliseconds (timeStamp). */
static int
event_time_stamp(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_event *event;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The event. */
	status = event_this(realm, this_value, &event);
	if (status != 0)
		return status;

	/* Succeeded: the time is reported. */
	*result = vm_value_number(event->time_stamp);
	return 0;
}

/* Cancels the event, when it can be canceled (preventDefault). */
static int
event_prevent_default(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_event *event;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The event. */
	*result = VM_VALUE_UNDEFINED;
	status = event_this(realm, this_value, &event);
	if (status != 0)
		return status;

	/* An event that can be canceled is. */
	if (event->cancelable)
		event->canceled = 1;

	/* Succeeded: the default action will not happen. */
	return 0;
}

/* Stops the dispatch after the current target's listeners (stopPropagation). */
static int
event_stop_propagation(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_event *event;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The event. */
	*result = VM_VALUE_UNDEFINED;
	status = event_this(realm, this_value, &event);
	if (status != 0)
		return status;

	/* Succeeded: no further target gets the event. */
	event->stop = 1;
	return 0;
}

/* Stops the dispatch after the current listener (stopImmediatePropagation). */
static int
event_stop_immediate(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_event *event;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The event. */
	*result = VM_VALUE_UNDEFINED;
	status = event_this(realm, this_value, &event);
	if (status != 0)
		return status;

	/* Succeeded: no further listener gets the event. */
	event->stop = 1;
	event->stop_immediate = 1;
	return 0;
}

/* Reports the pointer's horizontal place in the viewport (clientX, x). */
static int
event_client_x(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_event *event;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The event. */
	status = event_this(realm, this_value, &event);
	if (status != 0)
		return status;

	/* Succeeded: the place is reported. */
	*result = vm_value_number(event->mouse.client_x);
	return 0;
}

/* Reports the pointer's vertical place in the viewport (clientY, y). */
static int
event_client_y(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_event *event;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The event. */
	status = event_this(realm, this_value, &event);
	if (status != 0)
		return status;

	/* Succeeded: the place is reported. */
	*result = vm_value_number(event->mouse.client_y);
	return 0;
}

/* Reports the pointer's horizontal place in the document (pageX). */
static int
event_page_x(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_event *event;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The event. */
	status = event_this(realm, this_value, &event);
	if (status != 0)
		return status;

	/* Succeeded: the place is reported. */
	*result = vm_value_number(event->mouse.page_x);
	return 0;
}

/* Reports the pointer's vertical place in the document (pageY). */
static int
event_page_y(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_event *event;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The event. */
	status = event_this(realm, this_value, &event);
	if (status != 0)
		return status;

	/* Succeeded: the place is reported. */
	*result = vm_value_number(event->mouse.page_y);
	return 0;
}

/* Reports the button that changed (button: 0 is the main one). */
static int
event_button(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_event *event;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The event. */
	status = event_this(realm, this_value, &event);
	if (status != 0)
		return status;

	/* Succeeded: the button is reported. */
	*result = vm_value_int32(event->mouse.button);
	return 0;
}

/* Initializes base Event fields after all argument conversions have succeeded. */
static int
event_init(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_event *event;
	struct vm_string *type;
	int bubbles;
	int cancelable;
	int error;

	/* Branding must precede any user conversion of the required type. */
	*result = VM_VALUE_UNDEFINED;
	error = event_this(realm, this_value, &event);
	if (error != 0)
		return error;

	/* The legacy signature still requires its first argument. */
	if (count < 1U) {
		error = vm_throw_type_error(realm, "initEvent requires a type.");
		return error;
	}

	/* Converts the type before reading the Boolean arguments. */
	error = bind_to_atom(realm, args[0], 0, &type);
	if (error != 0)
		return error;

	/* Missing flags become false through the ordinary Boolean conversion. */
	bubbles = vm_to_boolean(js_argument(args, count, 1));
	cancelable = vm_to_boolean(js_argument(args, count, 2));

	/* The method body runs after conversion and cannot rewrite a dispatch in progress. */
	if (event->dispatching)
		return 0;

	/* Resets only the base Event state, retaining subclass data. */
	event_initialize(event, type, bubbles, cancelable);

	/* Succeeded: the event may be dispatched with its newly initialized base fields. */
	return 0;
}

/* Initializes UI fields without publishing a partial conversion or changing an active event. */
static int
event_init_ui(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_event *event;
	struct vm_string *type;
	vm_value view;
	int32_t detail;
	int bubbles;
	int cancelable;
	int error;

	/* The internal interface brand cannot be forged through an inherited prototype. */
	*result = VM_VALUE_UNDEFINED;
	error = event_ui_this(realm, this_value, &event);
	if (error != 0)
		return error;

	/* UI initialization also requires an event type. */
	if (count < 1U) {
		error = vm_throw_type_error(realm, "initUIEvent requires a type.");
		return error;
	}

	/* WebIDL argument conversion follows type, flags, nullable Window, then long detail. */
	error = bind_to_atom(realm, args[0], 0, &type);
	if (error != 0)
		return error;

	/* Boolean flags never invoke script conversion hooks. */
	bubbles = vm_to_boolean(js_argument(args, count, 1));
	cancelable = vm_to_boolean(js_argument(args, count, 2));

	/* Only an actual Window global or null may become the stored view. */
	error = event_window_value(realm, js_argument(args, count, 3), &view);
	if (error != 0)
		return error;

	/* The long detail uses the existing signed 32-bit WebIDL conversion. */
	error = vm_to_int32(realm, js_argument(args, count, 4), &detail);
	if (error != 0)
		return error;

	/* Reentrant conversion may have dispatched this same event before this point. */
	if (event->dispatching)
		return 0;

	/* Publishes all initialized fields together after fallible conversions. */
	event_initialize(event, type, bubbles, cancelable);
	event->view = view;
	event->detail = vm_value_int32(detail);

	/* Succeeded: both base and UI fields describe the newly initialized event. */
	return 0;
}

/* Reports numerical detail only for a real UI-derived event. */
static int
event_ui_detail(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_event *event;
	int error;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Custom payloads cannot be read through a borrowed UI detail accessor. */
	error = event_ui_this(realm, this_value, &event);
	if (error != 0)
		return error;

	/* Succeeded: detail contains the UI event's signed numerical value. */
	*result = event->detail;
	return 0;
}

/* Reports the actual retained Window global carried by a UI-derived event. */
static int
event_view(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_event *event;
	int error;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* A base or custom event cannot impersonate UI view through prototype changes. */
	error = event_ui_this(realm, this_value, &event);
	if (error != 0)
		return error;

	/* Succeeded: the traced view remains readable after its frame is removed. */
	*result = event->view;
	return 0;
}

/* Identifies the implemented interfaces that actually inherit UIEvent state. */
static int
event_is_ui(
	int interface)
{
	/* These interface identities are fixed when bind_event_create allocates state. */
	switch (interface) {
	case BIND_UI_EVENT:
	case BIND_MOUSE_EVENT:
	case BIND_KEYBOARD_EVENT:
	case BIND_FOCUS_EVENT:
	case BIND_WHEEL_EVENT:
		return 1;
	default:
		break;
	}

	/* Base, custom and message events have no UI-specific state. */
	return 0;
}

/* Brands UI receivers from their internal event interface, not their prototypes. */
static int
event_ui_this(
	struct vm_realm *realm,
	vm_value this_value,
	struct bind_event **event)
{
	int is_ui;
	int error;

	/* Resolves the normal traced event state before applying the narrower UI brand. */
	error = event_this(realm, this_value, event);
	if (error != 0)
		return error;

	/* Forging UIEvent.prototype on a plain event does not change its native identity. */
	is_ui = event_is_ui((*event)->interface);
	if (!is_ui) {
		error = bind_throw_illegal(realm);
		if (error != 0)
			return error;

		/* A host without a published exception still cannot accept a false UI brand. */
		return EINVAL;
	}

	/* Succeeded: the receiver has actual UI-derived state. */
	return 0;
}

/* Converts a nullable Window by checking immutable own Document and global identity. */
static int
event_window_value(
	struct vm_realm *realm,
	vm_value given,
	vm_value *view)
{
	struct vm_object *object;
	struct vm_property property;
	struct dom_node *node;
	struct bind_window *window;
	vm_value key;
	int is_object;
	int found;
	int error;

	/* Omitted and explicitly nullable arguments both name no view. */
	*view = VM_VALUE_NULL;
	if (given == VM_VALUE_NULL || given == VM_VALUE_UNDEFINED)
		return 0;

	/* The object must be a global belonging to the Document it owns. */
	is_object = vm_value_is_object(given);
	if (!is_object) {
		error = vm_throw_type_error(realm, "UIEvent view requires a Window or null.");
		return error;
	}

	/* An own data property is read without evaluating a forged document getter. */
	object = (struct vm_object *)vm_value_as_cell(given);
	key = vm_key_from_ascii(realm->heap, "document");
	if (key == VM_VALUE_EMPTY)
		return ENOMEM;

	/* Missing or accessor document properties cannot establish a Window brand. */
	found = vm_object_get_own_ordinary(object, key, &property);
	if (!found || (property.attributes & VM_PROPERTY_ACCESSOR) != 0) {
		error = vm_throw_type_error(realm, "UIEvent view requires an actual Window.");
		return error;
	}

	/* The property's value must wrap an actual Document node. */
	node = bind_node_of(*property.value);
	if (node == NULL || node->type != DOM_DOCUMENT) {
		error = vm_throw_type_error(realm, "UIEvent view requires a Window document.");
		return error;
	}

	/* Copied documents or released primary views do not identify a global owner. */
	window = node->document->view;
	if (window == NULL || window->realm->global != object) {
		error = vm_throw_type_error(realm, "UIEvent view requires its document's Window.");
		return error;
	}

	/* Succeeded: storing this value retains the actual global and its traced owner. */
	*view = given;
	return 0;
}

/* Resets the DOM initialization fields while preserving subclass-specific payloads. */
static void
event_initialize(
	struct bind_event *event,
	struct vm_string *type,
	int bubbles,
	int cancelable)
{
	/* Initialization clears dispatch-control history before making the event ready. */
	event->initialized = 1;
	event->stop = 0;
	event->stop_immediate = 0;
	event->canceled = 0;
	event->trusted = 0;
	event->target = VM_VALUE_NULL;
	event->target_override = VM_VALUE_EMPTY;
	event->type = type;
	event->bubbles = bubbles;
	event->cancelable = cancelable;

	/* Succeeded: the base Event fields are ready for a fresh dispatch. */
	return;
}

/* Observes stored composed state without introducing shadow-path behavior. */
static int
event_composed(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_event *event;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Every genuine Event subclass shares this native boolean state. */
	status = event_this(realm, this_value, &event);
	if (status != 0)
		return status;
	*result = vm_value_boolean(event->composed);
	return 0;
}

/* Reports a custom event's detail (detail). */
static int
event_detail(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_event *event;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The event. */
	status = event_this(realm, this_value, &event);
	if (status != 0)
		return status;

	/* Succeeded: the detail is reported. */
	*result = event->detail;
	return 0;
}

/* Reports the origin of a message (origin). */
static int
event_message_origin(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_event *event;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);
	status = event_this(realm, this_value, &event);
	if (status != 0)
		return status;
	if (event->origin == NULL) {
		status = bind_string(realm, "", result);
		if (status != 0)
			return status;
		return 0;
	}

	/* A stored origin is already owned by the event state. */
	*result = vm_value_cell(event->origin);
	return 0;
}

/* Reports a message's last event id (lastEventId). */
static int
event_message_last_id(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_event *event;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);
	status = event_this(realm, this_value, &event);
	if (status != 0)
		return status;
	if (event->last_event_id == NULL) {
		status = bind_string(realm, "", result);
		if (status != 0)
			return status;
		return 0;
	}

	/* A stored id is already owned by the event state. */
	*result = vm_value_cell(event->last_event_id);
	return 0;
}

/* Reports the context that sent a message (source). */
static int
event_message_source(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_event *event;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);
	status = event_this(realm, this_value, &event);
	if (status != 0)
		return status;
	*result = event->source;
	return 0;
}

/* Reports the ports transferred with a message (ports). */
static int
event_message_ports(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_event *event;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);
	status = event_this(realm, this_value, &event);
	if (status != 0)
		return status;
	if (event->ports == VM_VALUE_NULL) {
		status = js_builtin_array(realm, NULL, 0, result);
		if (status != 0)
			return status;
		return 0;
	}

	/* Transferred ports remain owned by the event state. */
	*result = event->ports;
	return 0;
}

/* Builds an event's path: the target, its ancestors, and the window above a document (not for load). */
static int
event_build_path(
	struct bind_window *window,
	vm_value target,
	const struct bind_event *event,
	struct vm_object *path)
{
	struct dom_node *node;
	struct dom_node *walk;
	uint32_t length;
	int is_load;
	int status;

	/* The target is the path's start. */
	status = vm_object_define(window->realm->heap, path, vm_value_int32(0), target, VM_PROPERTY_DEFAULT);
	if (status != 0)
		return status;

	/* A target that is not a node has no ancestors. */
	node = bind_node_of(target);
	if (node == NULL)
		return 0;

	/* The ancestors, nearest first. */
	length = 1;
	for (walk = node->parent; walk != NULL && length < EVENT_PATH_MAX; walk = walk->parent) {
		status = bind_array_push_node(window, path, walk);
		if (status != 0)
			return status;
		node = walk;
		length++;
	}

	/* Above the document is the window, except for a load event. */
	is_load = vm_string_equal_ascii(event->type, "load");
	if (node->type != DOM_DOCUMENT || is_load)
		return 0;
	status = vm_object_define(window->realm->heap, path, vm_value_int32((int32_t)path->length), vm_value_cell(window->realm->global), VM_PROPERTY_DEFAULT);
	if (status != 0)
		return status;

	/* Succeeded: the path is built. */
	return 0;
}

/* Recognizes the HTML legacy error handler using actual event and current Window identity. */
static int
event_is_window_error(
	struct bind_window *window,
	vm_value current,
	const struct bind_event *event,
	int handler)
{
	vm_value global;
	int error_type;

	/* Ordinary listeners, element handlers and other event interfaces keep normal semantics. */
	global = vm_value_cell(window->realm->global);
	if (!handler ||
	    current != global ||
	    event->interface != BIND_ERROR_EVENT)
		return 0;

	/* Only the exact error event type invokes Window.onerror's legacy signature. */
	error_type = vm_string_equal_ascii(event->type, "error");
	if (!error_type)
		return 0;

	/* Succeeded: dispatch supplies five arguments and true cancellation. */
	return 1;
}

/* Runs the listeners of one target of the path, of the kinds a phase invokes (handlers are of the bubble kind). */
static int
event_invoke(
	struct bind_window *window,
	vm_value current,
	vm_value event_value,
	int phase,
	unsigned kinds)
{
	struct bind_event *event;
	struct bind_listeners *listeners;
	struct bind_listener *listener;
	struct vm_object *snapshot;
	struct vm_cell *snapshot_root;
	vm_value callback;
	vm_value returned;
	size_t index;
	size_t found;
	uint32_t item;
	int capture;
	int handler;
	int legacy_error;
	int present;
	int status;

	/* A stopped event reaches no further target. */
	event = bind_event_of(event_value);
	if (event->stop)
		return 0;
	event->current_target = current;
	event->phase = phase;

	/* A handler attribute (onclick="...") becomes the target's handler before the listeners run. */
	if ((kinds & EVENT_INVOKE_BUBBLE) != 0) {
		status = bind_handler_prepare(window, current, event->type);
		if (status != 0)
			return status;
	}

	/* The listeners of the event's type and the phase's kind as they are now (ones added while they run wait). */
	status = bind_listeners_of(window, current, 0, &listeners);
	if (status == EINVAL || listeners == NULL)
		return 0;
	if (status != 0)
		return status;
	status = bind_array_create(window->realm, &snapshot);
	if (status != 0)
		return status;
	snapshot_root = &snapshot->cell;
	status = vm_heap_add_root(window->realm->heap, &snapshot_root);
	if (status != 0)
		return status;
	for (index = 0; status == 0 && index < listeners->count; index++) {
		listener = &listeners->items[index];
		if (listener->type != event->type)
			continue;
		if (listener->capture && (kinds & EVENT_INVOKE_CAPTURE) == 0)
			continue;
		if (!listener->capture && (kinds & EVENT_INVOKE_BUBBLE) == 0)
			continue;
		if (listener->callback == VM_VALUE_NULL)
			continue;

		/* Each is kept as its callback and whether it is a handler. */
		status = vm_object_define(window->realm->heap, snapshot, vm_value_int32((int32_t)snapshot->length), listener->callback, VM_PROPERTY_DEFAULT);
		if (status == 0)
			status = vm_object_define(window->realm->heap, snapshot, vm_value_int32((int32_t)snapshot->length), vm_value_boolean(listener->handler), VM_PROPERTY_DEFAULT);
	}

	/* The copy may have run out of memory. */
	if (status != 0)
		goto cleanup;

	/* Each one that is still there, in order, until a listener stops the event at once. */
	capture = 0;
	if ((kinds & EVENT_INVOKE_CAPTURE) != 0)
		capture = 1;
	for (item = 0; item + 1U < snapshot->length && !event->stop_immediate; item += 2U) {
		callback = snapshot->elements[item];
		handler = 0;
		if (snapshot->elements[item + 1U] == VM_VALUE_TRUE)
			handler = 1;

		/* A listener removed by an earlier one does not run; a once listener is removed before it runs. */
		status = bind_listeners_of(window, current, 0, &listeners);
		if (status != 0)
			goto cleanup;
		present = bind_listeners_find(listeners, event->type, callback, capture, handler, &found);
		if (!present)
			continue;
		if (listeners->items[found].once)
			event_forget_listener(listeners, found);

		/* Window's ErrorEvent handler uses its legacy five arguments and true cancellation. */
		legacy_error = event_is_window_error(window, current, event, handler);
		status = event_call_listener(window, callback, current, event_value, legacy_error, &returned);
		if (status != 0)
			goto cleanup;

		/* Ordinary handlers retain false cancellation; addEventListener return values never cancel. */
		if (handler && event->cancelable) {
			/* Select the legacy inversion only for this actual Window ErrorEvent handler. */
			if (legacy_error) {
				/* Only the Boolean true suppresses the legacy default report. */
				if (returned == VM_VALUE_TRUE)
					event->canceled = 1;
			} else if (returned == VM_VALUE_FALSE) {
				event->canceled = 1;
			}
		}
	}

	/* The detached callback snapshot no longer spans user code. */
cleanup:
	vm_heap_remove_root(window->realm->heap, &snapshot_root);
	if (status != 0)
		return status;

	/* Succeeded: the target's listeners have run. */
	return 0;
}

/* Calls a listener (a function, or an object's handleEvent) with the event; an exception is reported. */
static int
event_call_listener(
	struct bind_window *window,
	vm_value callback,
	vm_value current,
	vm_value event_value,
	int legacy_error,
	vm_value *returned)
{
	struct vm_realm *realm;
	vm_value method;
	vm_value key;
	vm_value this_value;
	vm_value arguments[5];
	struct bind_event *event;
	struct vm_string *empty;
	unsigned argument_count;
	int callable;
	int status;

	/* A function is called with the current target as this. */
	*returned = VM_VALUE_UNDEFINED;
	realm = window->realm;
	method = callback;
	this_value = current;
	callable = vm_value_is_callable(callback);

	/* An object's handleEvent is called with the object as this. */
	status = 0;
	if (!callable) {
		key = vm_key_from_ascii(realm->heap, "handleEvent");
		if (key == VM_VALUE_EMPTY)
			return ENOMEM;
		status = vm_get(realm, callback, key, &method);
		this_value = callback;
		if (status == 0) {
			callable = vm_value_is_callable(method);
			if (!callable)
				status = vm_throw_type_error(realm, "The listener's handleEvent is not a function.");
		}
	}

	/* A normal listener receives the event; only a Window error handler receives payload values. */
	arguments[0] = event_value;
	argument_count = 1;
	if (status == 0 && legacy_error) {
		event = bind_event_of(event_value);
		empty = vm_atom_from_ascii(realm->heap, "");
		if (empty == NULL)
			return ENOMEM;
		arguments[0] = vm_value_cell(empty);
		arguments[1] = vm_value_cell(empty);

		/* Native payloads override the empty defaults without consulting event expandos. */
		if (event->error_message != NULL)
			arguments[0] = vm_value_cell(event->error_message);

		/* The source string is separately retained by the event. */
		if (event->error_filename != NULL)
			arguments[1] = vm_value_cell(event->error_filename);

		/* Numeric positions and the arbitrary error retain their native payload values. */
		arguments[2] = vm_value_number((double)event->error_line);
		arguments[3] = vm_value_number((double)event->error_column);
		arguments[4] = event->detail;
		argument_count = 5;
	}

	/* Call with the exact native argument list after all defaults are ready. */
	if (status == 0)
		status = vm_call(realm, method, this_value, arguments, argument_count, returned);

	/* An exception is reported, and the dispatch goes on. */
	if (status == VM_THROWN) {
		bind_report_exception(window, realm->exception);
		realm->exception = VM_VALUE_UNDEFINED;
		status = 0;
	}

	/* Running out of memory stops the dispatch. */
	if (status != 0)
		return status;

	/* The microtasks run after each listener when no script is running. */
	if (realm->depth == 0) {
		status = bind_checkpoint(window);
		if (status != 0)
			return status;
	}

	/* Succeeded: the listener has run. */
	return 0;
}

/* Makes and dispatches a trusted event of an interface at a node, or at the window for NULL. */
static int
event_fire(
	struct bind_window *window,
	struct dom_node *target,
	int interface,
	const char *type,
	unsigned flags,
	const struct bind_mouse *mouse,
	int *canceled)
{
	struct bind_event *event;
	vm_value event_value;
	vm_value target_value;
	int status;

	/* The event and its target. */
	*canceled = 0;
	status = bind_event_prepare(window, target, interface, type, flags, &event_value, &target_value, &event);
	if (status != 0)
		return status;

	/* The pointer, for a mouse event. */
	if (mouse != NULL)
		event->mouse = *mouse;

	/* The dispatch. */
	status = bind_dispatch(window, target_value, event_value, canceled);
	if (status != 0)
		return status;

	/* Succeeded: the event is dispatched. */
	return 0;
}
