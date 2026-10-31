/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Event handlers: the onclick-like attributes of HTMLElement, Document and
 * Window, and the content attributes (onclick="...") compiled into them.
 *
 * A handler is one of its target's listeners, marked as the handler of
 * its type, placed where it was first set: setting it adds the listener
 * at the end, and a content attribute is compiled, the first time the
 * handler is needed, into a listener placed among the others as if it had
 * been added when its element was made (the parser sets the attributes
 * then).
 * The body element's handlers of the window's events (onload and the like)
 * are the window's.
 */

#include "bind/internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/*
 * The event types whose handlers the interfaces have (the common part of
 * GlobalEventHandlers and WindowEventHandlers).  The table is constant for
 * the life of the program and ends with NULL.
 */
static const char *const handler_types[] = {
	"abort", "blur", "change", "click", "contextmenu", "dblclick", "error", "focus", "input", "keydown",
	"keypress", "keyup", "load", "mousedown", "mouseenter", "mouseleave", "mousemove", "mouseout",
	"mouseover", "mouseup", "reset", "resize", "scroll", "select", "submit", "wheel",
	"beforeunload", "hashchange", "message", "pagehide", "pageshow", "popstate", "storage", "unload",
	"canplay", "durationchange", "ended", "loadeddata", "loadedmetadata", "pause", "play", "playing", "seeked",
	"seeking", "timeupdate",
	"auxclick", "beforeinput", "beforematch", "beforetoggle", "cancel", "canplaythrough", "close", "copy", "cut",
	"drag", "dragend", "dragenter", "dragleave", "dragover", "dragstart", "drop", "emptied", "formdata",
	"gotpointercapture", "invalid", "loadstart", "lostpointercapture", "paste", "pointercancel", "pointerdown",
	"pointerenter", "pointerleave", "pointermove", "pointerout", "pointerover", "pointerrawupdate", "pointerup",
	"progress", "ratechange", "scrollend", "securitypolicyviolation", "selectionchange", "selectstart",
	"slotchange", "stalled", "suspend", "toggle", "touchcancel", "touchend", "touchmove", "touchstart",
	"transitioncancel", "transitionend", "transitionrun", "transitionstart", "animationcancel", "animationend",
	"animationiteration", "animationstart", "volumechange", "waiting", "languagechange", "offline", "online",
	"rejectionhandled", "unhandledrejection", "afterprint", "beforeprint",
	NULL
};

/*
 * The event types whose handlers on the body element are the window's.
 * The table is constant for the life of the program and ends with NULL.
 */
static const char *const handler_window_types[] = {
	"beforeunload", "blur", "error", "focus", "hashchange", "load", "message", "pagehide", "pageshow",
	"popstate", "resize", "scroll", "storage", "unload",
	NULL
};

static int handler_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int handler_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int handler_define(struct bind_window *window, struct vm_object *prototype, const char *type);
static int handler_is_window_event(const struct vm_string *type);
static struct dom_element *handler_body(const struct bind_window *window);
static int handler_attribute_name(struct vm_realm *realm, const struct vm_string *type, struct vm_string **name);
static int handler_compile(struct bind_window *window, const struct vm_string *source, vm_value *handler);

/*
 * Defines the event handler attributes (onclick and the like) on an
 * interface's prototype.
 */
int
bind_define_handlers(
	struct bind_window *window,
	struct vm_object *prototype)
{
	size_t index;
	int error;

	/* One accessor per type. */
	for (index = 0; handler_types[index] != NULL; index++) {
		error = handler_define(window, prototype, handler_types[index]);
		if (error != 0)
			return error;
	}

	/* Succeeded: the handlers are defined. */
	return 0;
}

/*
 * Makes the handler of a type from its content attribute, when the
 * target (an element, or the window for its body's attributes) has the
 * attribute and no handler of the type yet.
 */
int
bind_handler_prepare(
	struct bind_window *window,
	vm_value current,
	struct vm_string *type)
{
	struct bind_listeners *listeners;
	struct bind_listener listener;
	struct dom_node *node;
	struct dom_element *element;
	struct dom_element *body;
	struct dom_attribute *attribute;
	struct vm_string *name;
	vm_value global;
	size_t index;
	int window_event;
	int found;
	int status;

	/* The element whose attribute it would be: the target, or the body for the window. */
	element = NULL;
	window_event = handler_is_window_event(type);
	node = bind_node_of(current);
	global = vm_value_cell(window->realm->global);
	if (current == global && window_event) {
		element = handler_body(window);
	} else if (node != NULL && node->type == DOM_ELEMENT) {
		element = (struct dom_element *)node;
	}

	/* The body's attributes of the window's events are not the body's own. */
	if (element != NULL &&
	    current != global &&
	    window_event) {
		/* Resolves the body only when this target could supply the window's handler. */
		body = handler_body(window);
		if (element == body)
			element = NULL;
	}

	/* A missing target has no content attribute to compile. */
	if (element == NULL)
		return 0;

	/* A target with a handler of the type already has it. */
	status = bind_listeners_of(window, current, 0, &listeners);
	if (status != 0)
		return 0;
	if (listeners != NULL) {
		found = bind_listeners_find_handler(listeners, type, &index);
		if (found)
			return 0;
	}

	/* The attribute, if the element has it. */
	status = handler_attribute_name(window->realm, type, &name);
	if (status != 0)
		return status;
	attribute = dom_element_find_attribute(element, DOM_NS_NONE, name);
	if (attribute == NULL)
		return 0;

	/* The handler it compiles to (null when it does not compile). */
	listener.type = type;
	listener.capture = 0;
	listener.once = 0;
	listener.handler = 1;
	listener.generation = element->created;
	status = handler_compile(window, attribute->value, &listener.callback);
	if (status != 0)
		return status;
	status = bind_listeners_of(window, current, 1, &listeners);
	if (status != 0)
		return status;

	/* It goes before the listeners added after its element was made. */
	for (index = 0; index < listeners->count; index++) {
		if (listeners->items[index].generation > element->created)
			break;
	}

	/* Publishes the compiled handler at its original listener position. */
	status = bind_listeners_add(listeners, &listener, index);
	if (status != 0)
		return status;

	/* Succeeded: the target has its handler. */
	return 0;
}

/* Reports a target's handler of the accessor's type, or null (onclick and the like). */
static int
handler_get(
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
	int found;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The target (a plain call is the window's) and the type the accessor keeps. */
	*result = VM_VALUE_NULL;
	window = bind_window_of(realm);
	target = this_value;
	if (target == VM_VALUE_UNDEFINED || target == VM_VALUE_NULL)
		target = vm_value_cell(realm->global);
	type = (struct vm_string *)vm_value_as_cell(js_builtin_callee(realm)->data);

	/* A content attribute not compiled yet is compiled now. */
	status = bind_handler_prepare(window, target, type);
	if (status != 0)
		return status;

	/* The handler listener's callback. */
	status = bind_listeners_of(window, target, 0, &listeners);
	if (status == EINVAL) {
		status = bind_throw_illegal(realm);
		return status;
	} else if (status != 0 || listeners == NULL) {
		return status;
	}

	/* Finds the handler callback in this target's listener list. */
	found = bind_listeners_find_handler(listeners, type, &index);
	if (found)
		*result = listeners->items[index].callback;

	/* Succeeded: the handler (or null) is reported. */
	return 0;
}

/* Sets a target's handler of the accessor's type; a value that is not callable is null (onclick and the like). */
static int
handler_set(
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
	vm_value value;
	size_t index;
	int callable;
	int found;
	int status;

	/* The target (a plain call is the window's) and the type the accessor keeps. */
	*result = VM_VALUE_UNDEFINED;
	window = bind_window_of(realm);
	target = this_value;
	if (target == VM_VALUE_UNDEFINED || target == VM_VALUE_NULL)
		target = vm_value_cell(realm->global);
	type = (struct vm_string *)vm_value_as_cell(js_builtin_callee(realm)->data);

	/* The new handler: a function, or null. */
	value = js_argument(args, count, 0);
	callable = vm_value_is_callable(value);
	if (!callable)
		value = VM_VALUE_NULL;

	/* The target's listeners. */
	status = bind_listeners_of(window, target, 1, &listeners);
	if (status == EINVAL) {
		status = bind_throw_illegal(realm);
		return status;
	} else if (status != 0) {
		return status;
	}

	/* A handler already set keeps its place and takes the new value. */
	found = bind_listeners_find_handler(listeners, type, &index);
	if (found) {
		listeners->items[index].callback = value;
		return 0;
	}

	/* A first handler is added at the end, unless it is null. */
	if (value == VM_VALUE_NULL)
		return 0;
	listener.type = type;
	listener.callback = value;
	listener.capture = 0;
	listener.once = 0;
	listener.handler = 1;
	listener.generation = window->document->generation;
	status = bind_listeners_add(listeners, &listener, listeners->count);
	if (status != 0)
		return status;

	/* Succeeded: the handler is set. */
	return 0;
}

/* Defines the accessor of one event handler (on and the type) whose functions keep the type's atom. */
static int
handler_define(
	struct bind_window *window,
	struct vm_object *prototype,
	const char *type)
{
	struct vm_realm *realm;
	struct vm_function *getter;
	struct vm_function *setter;
	struct vm_accessor *accessor;
	struct vm_string *atom;
	char name[64];
	vm_value key;
	int error;

	/* The type's atom, which the accessor's functions keep. */
	realm = window->realm;
	atom = vm_atom_from_ascii(realm->heap, type);
	if (atom == NULL)
		return ENOMEM;

	/* The getter. */
	snprintf(name, sizeof(name), "get on%s", type);
	getter = vm_function_create_native(realm, name, 0, handler_get);
	if (getter == NULL)
		return ENOMEM;
	getter->data = vm_value_cell(atom);

	/* The setter. */
	snprintf(name, sizeof(name), "set on%s", type);
	setter = vm_function_create_native(realm, name, 1, handler_set);
	if (setter == NULL)
		return ENOMEM;
	setter->data = vm_value_cell(atom);

	/* The accessor property, configurable and enumerable as WebIDL's attributes are. */
	accessor = vm_accessor_create(realm->heap, vm_value_cell(getter), vm_value_cell(setter));
	if (accessor == NULL)
		return ENOMEM;
	snprintf(name, sizeof(name), "on%s", type);
	key = vm_key_from_ascii(realm->heap, name);
	if (key == VM_VALUE_EMPTY)
		return ENOMEM;
	error = vm_object_define(realm->heap, prototype, key, vm_value_cell(accessor),
	    VM_PROPERTY_ACCESSOR | VM_PROPERTY_CONFIGURABLE | VM_PROPERTY_ENUMERABLE);
	if (error != 0)
		return error;

	/* Succeeded: the handler's accessor is defined. */
	return 0;
}

/* Tells whether the body's handler of a type is the window's. */
static int
handler_is_window_event(
	const struct vm_string *type)
{
	size_t index;
	int same;

	/* One of the window's types. */
	for (index = 0; handler_window_types[index] != NULL; index++) {
		same = vm_string_equal_ascii(type, handler_window_types[index]);
		if (same)
			return 1;
	}

	/* Another type is the body's own. */
	return 0;
}

/* Finds the document's body element (the first <body> child of <html>), or NULL. */
static struct dom_element *
handler_body(
	const struct bind_window *window)
{
	struct dom_node *root;
	struct dom_node *child;
	int is_html;
	int is_body;

	/* The root element must be an HTML <html>. */
	for (root = window->document->node.first_child; root != NULL; root = root->next) {
		if (root->type == DOM_ELEMENT)
			break;
	}

	/* Rejects a document whose first element cannot contain the HTML body. */
	is_html = dom_element_is(root, DOM_NS_HTML, DOM_TAG_HTML);
	if (!is_html)
		return NULL;

	/* Its first <body> child. */
	for (child = root->first_child; child != NULL; child = child->next) {
		is_body = dom_element_is(child, DOM_NS_HTML, DOM_TAG_BODY);
		if (is_body)
			return (struct dom_element *)child;
	}

	/* The document has no body. */
	return NULL;
}

/* Makes the atom of a handler's content attribute: on and the type. */
static int
handler_attribute_name(
	struct vm_realm *realm,
	const struct vm_string *type,
	struct vm_string **name)
{
	struct wb_units units;
	int error;

	/* The units of on and the type. */
	wb_units_init(&units);
	error = wb_units_append_code_point(&units, 'o');
	if (error == 0)
		error = wb_units_append_code_point(&units, 'n');
	if (error == 0)
		error = vm_string_append_units(type, &units);
	if (error != 0) {
		wb_units_release(&units);
		return error;
	}

	/* Their atom. */
	*name = vm_atom_from_units(realm->heap, units.data, units.length);
	wb_units_release(&units);
	if (*name == NULL)
		return ENOMEM;

	/* Succeeded: the name is made. */
	return 0;
}

/* Compiles a handler attribute's text into a function of event; null when it does not compile (reported). */
static int
handler_compile(
	struct bind_window *window,
	const struct vm_string *source,
	vm_value *handler)
{
	static const char prefix[] = "(function (event) {\n";
	static const char suffix[] = "\n})";
	struct js_syntax_error syntax;
	struct wb_units units;
	struct wb_buffer line;
	size_t index;
	int status;

	/* The function's source: the attribute's text as its body. */
	*handler = VM_VALUE_NULL;
	wb_units_init(&units);
	status = 0;
	for (index = 0; status == 0 && prefix[index] != '\0'; index++)
		status = wb_units_append_code_point(&units, (unsigned char)prefix[index]);
	if (status == 0)
		status = vm_string_append_units(source, &units);
	for (index = 0; status == 0 && suffix[index] != '\0'; index++)
		status = wb_units_append_code_point(&units, (unsigned char)suffix[index]);
	if (status != 0) {
		wb_units_release(&units);
		return status;
	}

	/* The function, or the report of why it does not compile. */
	status = js_run_script(window->realm, units.data, units.length, 0, handler, &syntax);
	wb_units_release(&units);
	if (status == EINVAL) {
		*handler = VM_VALUE_NULL;
		wb_buffer_init(&line);
		status = wb_buffer_printf(&line, "Uncaught SyntaxError: %s (an event handler attribute)", syntax.message);
		if (status == 0)
			bind_console(window, BIND_CONSOLE_ERROR, wb_buffer_string(&line));
		wb_buffer_release(&line);
		return 0;
	} else if (status == VM_THROWN) {
		/* An exception while making the function is reported too. */
		*handler = VM_VALUE_NULL;
		bind_report_exception(window, window->realm->exception);
		window->realm->exception = VM_VALUE_UNDEFINED;
		return 0;
	} else if (status != 0) {
		return status;
	}

	/* Succeeded: the handler is compiled. */
	return 0;
}
