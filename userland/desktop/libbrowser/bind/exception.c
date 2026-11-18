/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * DOMException (ws074-p083): the exception the DOM's operations throw.
 *
 * A DOMException is a platform object whose cell holds its name, its
 * message and its legacy code (the code the name has in WebIDL's table,
 * 0 for a name that has none).  Its prototype inherits from
 * Error.prototype, so it is an instance of Error, and Error's toString
 * writes it as "Name: message", as the console writes an uncaught one.
 * The interface object and the prototype carry the legacy code constants
 * (INDEX_SIZE_ERR and the rest).  new DOMException(message, name) makes
 * one; the name defaults to "Error".
 */

#include "bind/internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/*
 * The state of a DOMException: its name and message (strings) and its
 * code.
 */
struct exception_state {
	struct vm_cell cell;
	struct vm_string *name;
	struct vm_string *message;
	int code;
};

/*
 * One name of WebIDL's table of DOMException names with its legacy code.
 */
struct exception_name {
	const char *name;
	int code;
};

static void exception_trace(struct vm_heap *heap, struct vm_cell *cell);
static int exception_create(struct bind_window *window, struct vm_string *name, struct vm_string *message, vm_value *value);
static int exception_code_of(const struct vm_string *name);
static int exception_of(struct vm_realm *realm, vm_value value, struct exception_state **state);
static int exception_construct(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int exception_name_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int exception_message_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int exception_code_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);

/* The state of a DOMException, which marks its strings. */
static const struct vm_cell_type exception_state_type = { "dom-exception", exception_trace, NULL };

/*
 * The names that have a legacy code.  The table is constant for the life
 * of the program.
 */
static const struct exception_name exception_names[] = {
	{ "IndexSizeError", 1 },
	{ "HierarchyRequestError", 3 },
	{ "WrongDocumentError", 4 },
	{ "InvalidCharacterError", 5 },
	{ "NoModificationAllowedError", 7 },
	{ "NotFoundError", 8 },
	{ "NotSupportedError", 9 },
	{ "InUseAttributeError", 10 },
	{ "InvalidStateError", 11 },
	{ "SyntaxError", 12 },
	{ "InvalidModificationError", 13 },
	{ "NamespaceError", 14 },
	{ "InvalidAccessError", 15 },
	{ "TypeMismatchError", 17 },
	{ "SecurityError", 18 },
	{ "NetworkError", 19 },
	{ "AbortError", 20 },
	{ "URLMismatchError", 21 },
	{ "QuotaExceededError", 22 },
	{ "TimeoutError", 23 },
	{ "InvalidNodeTypeError", 24 },
	{ "DataCloneError", 25 },
	{ NULL, 0 }
};

/*
 * The attributes of DOMException.  The table is constant for the life of
 * the program.
 */
static const struct bind_attribute exception_attributes[] = {
	{ "name", exception_name_get, NULL },
	{ "message", exception_message_get, NULL },
	{ "code", exception_code_get, NULL },
	{ NULL, NULL, NULL }
};

/*
 * The legacy code constants of DOMException.  The table is constant for
 * the life of the program.
 */
static const struct bind_constant exception_constants[] = {
	{ "INDEX_SIZE_ERR", 1 },
	{ "DOMSTRING_SIZE_ERR", 2 },
	{ "HIERARCHY_REQUEST_ERR", 3 },
	{ "WRONG_DOCUMENT_ERR", 4 },
	{ "INVALID_CHARACTER_ERR", 5 },
	{ "NO_DATA_ALLOWED_ERR", 6 },
	{ "NO_MODIFICATION_ALLOWED_ERR", 7 },
	{ "NOT_FOUND_ERR", 8 },
	{ "NOT_SUPPORTED_ERR", 9 },
	{ "INUSE_ATTRIBUTE_ERR", 10 },
	{ "INVALID_STATE_ERR", 11 },
	{ "SYNTAX_ERR", 12 },
	{ "INVALID_MODIFICATION_ERR", 13 },
	{ "NAMESPACE_ERR", 14 },
	{ "INVALID_ACCESS_ERR", 15 },
	{ "VALIDATION_ERR", 16 },
	{ "TYPE_MISMATCH_ERR", 17 },
	{ "SECURITY_ERR", 18 },
	{ "NETWORK_ERR", 19 },
	{ "ABORT_ERR", 20 },
	{ "URL_MISMATCH_ERR", 21 },
	{ "QUOTA_EXCEEDED_ERR", 22 },
	{ "TIMEOUT_ERR", 23 },
	{ "INVALID_NODE_TYPE_ERR", 24 },
	{ "DATA_CLONE_ERR", 25 },
	{ NULL, 0 }
};

/*
 * The DOMException interface (window.c gives its prototype
 * Error.prototype's as parent).
 */
const struct bind_interface bind_dom_exception_interface = {
	"DOMException", BIND_NO_PARENT, 0, exception_construct, exception_attributes, NULL, exception_constants
};

/*
 * Throws a DOMException of a name (one of WebIDL's, such as
 * "SyntaxError") with a message.  Before the window has the interface, an
 * Error whose message starts with the name is thrown instead.
 */
int
bind_throw_dom(
	struct vm_realm *realm,
	const char *name,
	const char *message)
{
	struct bind_window *window;
	struct vm_string *name_string;
	struct vm_string *message_string;
	vm_value exception;
	char text[256];
	int status;

	/* Without the interface, an Error. */
	window = bind_window_of(realm);
	if (window == NULL || window->prototypes[BIND_DOM_EXCEPTION] == NULL) {
		snprintf(text, sizeof(text), "%s: %s", name, message);
		status = vm_throw_error(realm, VM_ERROR_PLAIN, text);
		return status;
	}

	/* The name and the message as strings. */
	name_string = vm_atom_from_ascii(realm->heap, name);
	if (name_string == NULL)
		return ENOMEM;
	message_string = vm_string_from_utf8(realm->heap, message, strlen(message));
	if (message_string == NULL)
		return ENOMEM;

	/* The exception. */
	status = exception_create(window, name_string, message_string, &exception);
	if (status != 0)
		return status;

	/* Succeeded: it is thrown. */
	status = vm_throw(realm, exception);
	return status;
}

/* Marks a DOMException's name and message. */
static void
exception_trace(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct exception_state *state;

	/* The two strings. */
	state = (struct exception_state *)cell;
	if (state->name != NULL)
		vm_heap_mark(heap, &state->name->cell);
	if (state->message != NULL)
		vm_heap_mark(heap, &state->message->cell);
}

/* Makes a DOMException with a name and a message; its code follows from the name. */
static int
exception_create(
	struct bind_window *window,
	struct vm_string *name,
	struct vm_string *message,
	vm_value *value)
{
	struct exception_state *state;
	struct vm_object *object;

	/* The state. */
	state = vm_heap_alloc(window->realm->heap, &exception_state_type, sizeof(*state));
	if (state == NULL)
		return ENOMEM;
	state->name = name;
	state->message = message;
	state->code = exception_code_of(name);

	/* The object with DOMException's prototype. */
	object = vm_object_create(window->realm->heap, window->prototypes[BIND_DOM_EXCEPTION]);
	if (object == NULL)
		return ENOMEM;
	object->kind = VM_KIND_PLATFORM;
	object->internal = vm_value_cell(state);

	/* Succeeded: the exception. */
	*value = vm_value_cell(object);
	return 0;
}

/* Finds the legacy code of a name, 0 for a name that has none. */
static int
exception_code_of(
	const struct vm_string *name)
{
	size_t index;
	int same;

	/* Each name of the table. */
	for (index = 0; exception_names[index].name != NULL; index++) {
		same = vm_string_equal_ascii(name, exception_names[index].name);
		if (same)
			return exception_names[index].code;
	}

	/* A name without a code. */
	return 0;
}

/* Finds the state of the DOMException a value is, throwing a TypeError otherwise. */
static int
exception_of(
	struct vm_realm *realm,
	vm_value value,
	struct exception_state **state)
{
	struct vm_object *object;
	struct vm_cell *cell;
	int is_object;
	int is_cell;
	int status;

	/* A platform object. */
	is_object = vm_value_is_object(value);
	if (!is_object) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* One the binding made. */
	object = (struct vm_object *)vm_value_as_cell(value);
	if (object->kind != VM_KIND_PLATFORM) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Whose cell is a DOMException's state. */
	is_cell = vm_value_is_cell(object->internal);
	if (!is_cell) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* A cell of the state's type. */
	cell = vm_value_as_cell(object->internal);
	if (cell->type != &exception_state_type) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Succeeded: the state. */
	*state = (struct exception_state *)cell;
	return 0;
}

/* Makes a DOMException from a message and a name, "" and "Error" when not given (new DOMException). */
static int
exception_construct(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_string *message;
	struct vm_string *name;
	vm_value given;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The message, empty when not given. */
	given = js_argument(args, count, 0);
	message = vm_atom_from_ascii(realm->heap, "");
	if (message == NULL)
		return ENOMEM;
	if (given != VM_VALUE_UNDEFINED) {
		status = bind_to_string(realm, given, &message);
		if (status != 0)
			return status;
	}

	/* The name, Error when not given. */
	given = js_argument(args, count, 1);
	name = vm_atom_from_ascii(realm->heap, "Error");
	if (name == NULL)
		return ENOMEM;
	if (given != VM_VALUE_UNDEFINED) {
		status = bind_to_string(realm, given, &name);
		if (status != 0)
			return status;
	}

	/* Succeeded: the exception. */
	status = exception_create(bind_window_of(realm), name, message, result);
	if (status != 0)
		return status;
	return 0;
}

/* Reports a DOMException's name (name). */
static int
exception_name_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct exception_state *state;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The exception. */
	status = exception_of(realm, this_value, &state);
	if (status != 0)
		return status;

	/* Succeeded: its name. */
	*result = vm_value_cell(state->name);
	return 0;
}

/* Reports a DOMException's message (message). */
static int
exception_message_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct exception_state *state;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The exception. */
	status = exception_of(realm, this_value, &state);
	if (status != 0)
		return status;

	/* Succeeded: its message. */
	*result = vm_value_cell(state->message);
	return 0;
}

/* Reports a DOMException's legacy code, 0 for a name without one (code). */
static int
exception_code_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct exception_state *state;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The exception. */
	status = exception_of(realm, this_value, &state);
	if (status != 0)
		return status;

	/* Succeeded: its code. */
	*result = vm_value_int32(state->code);
	return 0;
}
