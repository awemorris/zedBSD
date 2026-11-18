/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The events of the user's input besides the mouse's (ws074-p056):
 * KeyboardEvent (key, code, repeat, the modifiers, and the legacy keyCode
 * and which that many pages still read), WheelEvent (a MouseEvent with the
 * distances), FocusEvent, the modifiers' getters MouseEvent shares, and the
 * functions the page fires them with.
 *
 * The key and the code are the DOM's names ("a" and "KeyA", "Enter"): the
 * program around the engine turns its keyboard's codes into them, so the
 * page sees the same names whatever the keyboard.
 */

#include "bind/internal.h"

#include <errno.h>
#include <string.h>

/*
 * A key's legacy number (KeyboardEvent.keyCode) by its code, for the keys
 * whose number is not a letter's or a digit's.
 */
struct input_key_number {
	const char *code;
	int number;
};

static int input_key_code(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int input_key_key(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int input_key_repeat(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int input_key_location(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int input_key_composing(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int input_key_number(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int input_delta_x(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int input_delta_y(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int input_delta_zero(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int input_related_target(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int input_construct_keyboard(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int input_construct_wheel(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int input_construct_focus(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int input_event_this(struct vm_realm *realm, vm_value this_value, struct bind_event **event);
static int input_modifier(struct vm_realm *realm, vm_value this_value, unsigned modifier, vm_value *result);
static int input_init_string(struct vm_realm *realm, vm_value init, const char *name, struct vm_string **string);
static int input_init_number(struct vm_realm *realm, vm_value init, const char *name, double *number);
static int input_string_code(const struct vm_string *code);

/*
 * The legacy numbers of the named keys, by code.  The table is constant
 * for the life of the program and ends with a NULL code.
 */
static const struct input_key_number input_key_numbers[] = {
	{ "Backspace", 8 },
	{ "Tab", 9 },
	{ "Enter", 13 },
	{ "NumpadEnter", 13 },
	{ "ShiftLeft", 16 },
	{ "ShiftRight", 16 },
	{ "ControlLeft", 17 },
	{ "ControlRight", 17 },
	{ "AltLeft", 18 },
	{ "AltRight", 18 },
	{ "CapsLock", 20 },
	{ "Escape", 27 },
	{ "Space", 32 },
	{ "PageUp", 33 },
	{ "PageDown", 34 },
	{ "End", 35 },
	{ "Home", 36 },
	{ "ArrowLeft", 37 },
	{ "ArrowUp", 38 },
	{ "ArrowRight", 39 },
	{ "ArrowDown", 40 },
	{ "Insert", 45 },
	{ "Delete", 46 },
	{ "MetaLeft", 91 },
	{ "MetaRight", 92 },
	{ "NumpadMultiply", 106 },
	{ "F1", 112 },
	{ "F2", 113 },
	{ "F3", 114 },
	{ "F4", 115 },
	{ "F5", 116 },
	{ "F6", 117 },
	{ "F7", 118 },
	{ "F8", 119 },
	{ "F9", 120 },
	{ "F10", 121 },
	{ "F11", 122 },
	{ "F12", 123 },
	{ "Semicolon", 186 },
	{ "Equal", 187 },
	{ "Comma", 188 },
	{ "Minus", 189 },
	{ "Period", 190 },
	{ "Slash", 191 },
	{ "Backquote", 192 },
	{ "BracketLeft", 219 },
	{ "Backslash", 220 },
	{ "BracketRight", 221 },
	{ "Quote", 222 },
	{ NULL, 0 }
};

/*
 * The attributes of KeyboardEvent.  The table is constant for the life of
 * the program.
 */
static const struct bind_attribute keyboard_event_attributes[] = {
	{ "key", input_key_key, NULL },
	{ "code", input_key_code, NULL },
	{ "repeat", input_key_repeat, NULL },
	{ "location", input_key_location, NULL },
	{ "isComposing", input_key_composing, NULL },
	{ "keyCode", input_key_number, NULL },
	{ "which", input_key_number, NULL },
	{ "shiftKey", bind_event_shift_key, NULL },
	{ "ctrlKey", bind_event_ctrl_key, NULL },
	{ "altKey", bind_event_alt_key, NULL },
	{ "metaKey", bind_event_meta_key, NULL },
	{ NULL, NULL, NULL }
};

/*
 * The constants of KeyboardEvent: the locations of a key.  The table is
 * constant for the life of the program.
 */
static const struct bind_constant keyboard_event_constants[] = {
	{ "DOM_KEY_LOCATION_STANDARD", 0 },
	{ "DOM_KEY_LOCATION_LEFT", 1 },
	{ "DOM_KEY_LOCATION_RIGHT", 2 },
	{ "DOM_KEY_LOCATION_NUMPAD", 3 },
	{ NULL, 0 }
};

/*
 * The KeyboardEvent interface.
 */
const struct bind_interface bind_keyboard_event_interface = {
	"KeyboardEvent", BIND_UI_EVENT, 1, input_construct_keyboard, keyboard_event_attributes, NULL, keyboard_event_constants
};

/*
 * The attributes of FocusEvent.  The table is constant for the life of
 * the program.
 */
static const struct bind_attribute focus_event_attributes[] = {
	{ "relatedTarget", input_related_target, NULL },
	{ NULL, NULL, NULL }
};

/*
 * The FocusEvent interface.
 */
const struct bind_interface bind_focus_event_interface = {
	"FocusEvent", BIND_UI_EVENT, 1, input_construct_focus, focus_event_attributes, NULL, NULL
};

/*
 * The attributes of WheelEvent: the distances, in pixels (deltaMode 0).
 * The table is constant for the life of the program.
 */
static const struct bind_attribute wheel_event_attributes[] = {
	{ "deltaX", input_delta_x, NULL },
	{ "deltaY", input_delta_y, NULL },
	{ "deltaZ", input_delta_zero, NULL },
	{ "deltaMode", input_delta_zero, NULL },
	{ NULL, NULL, NULL }
};

/*
 * The constants of WheelEvent: the units of the distances.  The table is
 * constant for the life of the program.
 */
static const struct bind_constant wheel_event_constants[] = {
	{ "DOM_DELTA_PIXEL", 0 },
	{ "DOM_DELTA_LINE", 1 },
	{ "DOM_DELTA_PAGE", 2 },
	{ NULL, 0 }
};

/*
 * The WheelEvent interface.
 */
const struct bind_interface bind_wheel_event_interface = {
	"WheelEvent", BIND_MOUSE_EVENT, 1, input_construct_wheel, wheel_event_attributes, NULL, wheel_event_constants
};

/*
 * Fires a trusted key event (keydown, keyup or keypress; it bubbles and
 * can be canceled) at a node.
 */
int
bind_fire_key_event(
	struct bind_window *window,
	struct dom_node *target,
	const char *type,
	const struct bind_key *key,
	int *canceled)
{
	struct bind_event *event;
	struct vm_string *name;
	vm_value event_value;
	vm_value target_value;
	int status;

	/* The event and its target. */
	*canceled = 0;
	status = bind_event_prepare(
		window,
		target,
		BIND_KEYBOARD_EVENT,
		type,
		BIND_EVENT_BUBBLES | BIND_EVENT_CANCELABLE,
		&event_value,
		&target_value,
		&event);
	if (status != 0)
		return status;

	/* The key's value. */
	name = vm_string_from_utf8(window->realm->heap, key->key, strlen(key->key));
	if (name == NULL)
		return ENOMEM;
	event->key = name;

	/* The key's code. */
	name = vm_string_from_utf8(window->realm->heap, key->code, strlen(key->code));
	if (name == NULL)
		return ENOMEM;
	event->code = name;

	/* Whether it repeats, and the modifiers held. */
	event->repeat = key->repeat;
	event->mouse.modifiers = key->modifiers;

	/* The dispatch. */
	status = bind_dispatch(window, target_value, event_value, canceled);
	if (status != 0)
		return status;

	/* Succeeded: the event is dispatched. */
	return 0;
}

/*
 * Fires a trusted wheel event (it bubbles and can be canceled) at a node,
 * with the pointer's place and the distances.
 */
int
bind_fire_wheel_event(
	struct bind_window *window,
	struct dom_node *target,
	const struct bind_mouse *mouse,
	int *canceled)
{
	struct bind_event *event;
	vm_value event_value;
	vm_value target_value;
	int status;

	/* The event and its target. */
	*canceled = 0;
	status = bind_event_prepare(
		window,
		target,
		BIND_WHEEL_EVENT,
		"wheel",
		BIND_EVENT_BUBBLES | BIND_EVENT_CANCELABLE,
		&event_value,
		&target_value,
		&event);
	if (status != 0)
		return status;

	/* The pointer and the distances. */
	event->mouse = *mouse;

	/* The dispatch. */
	status = bind_dispatch(window, target_value, event_value, canceled);
	if (status != 0)
		return status;

	/* Succeeded: the event is dispatched. */
	return 0;
}

/*
 * Fires a trusted focus event at a node: focus and blur, which do not
 * bubble, or focusin and focusout, which do.  None can be canceled.
 */
int
bind_fire_focus_event(
	struct bind_window *window,
	struct dom_node *target,
	const char *type,
	int bubbles)
{
	struct bind_event *event;
	vm_value event_value;
	vm_value target_value;
	unsigned flags;
	int canceled;
	int status;

	/* Whether it bubbles. */
	flags = 0;
	if (bubbles)
		flags = BIND_EVENT_BUBBLES;

	/* The event and its target. */
	status = bind_event_prepare(window, target, BIND_FOCUS_EVENT, type, flags, &event_value, &target_value, &event);
	if (status != 0)
		return status;

	/* The dispatch; nothing can cancel it. */
	status = bind_dispatch(window, target_value, event_value, &canceled);
	if (status != 0)
		return status;

	/* Succeeded: the event is dispatched. */
	return 0;
}

/* Reports whether Shift was held (shiftKey). */
int
bind_event_shift_key(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The modifier's flag. */
	status = input_modifier(realm, this_value, BIND_MOD_SHIFT, result);
	if (status != 0)
		return status;

	/* Succeeded: the flag is reported. */
	return 0;
}

/* Reports whether Control was held (ctrlKey). */
int
bind_event_ctrl_key(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The modifier's flag. */
	status = input_modifier(realm, this_value, BIND_MOD_CTRL, result);
	if (status != 0)
		return status;

	/* Succeeded: the flag is reported. */
	return 0;
}

/* Reports whether Alt was held (altKey). */
int
bind_event_alt_key(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The modifier's flag. */
	status = input_modifier(realm, this_value, BIND_MOD_ALT, result);
	if (status != 0)
		return status;

	/* Succeeded: the flag is reported. */
	return 0;
}

/* Reports whether Meta was held (metaKey). */
int
bind_event_meta_key(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The modifier's flag. */
	status = input_modifier(realm, this_value, BIND_MOD_META, result);
	if (status != 0)
		return status;

	/* Succeeded: the flag is reported. */
	return 0;
}

/*
 * Takes the modifiers of an event's init (shiftKey, ctrlKey, altKey and
 * metaKey) into an event made by a constructor.
 */
int
bind_event_init_modifiers(
	struct vm_realm *realm,
	vm_value init,
	struct bind_event *event)
{
	static const char *const names[4] = { "shiftKey", "ctrlKey", "altKey", "metaKey" };
	static const unsigned flags[4] = { BIND_MOD_SHIFT, BIND_MOD_CTRL, BIND_MOD_ALT, BIND_MOD_META };
	vm_value value;
	int present;
	int held;
	int index;
	int status;

	/* Each modifier the init says was held. */
	for (index = 0; index < 4; index++) {
		status = bind_get_option(realm, init, names[index], &present, &value);
		if (status != 0)
			return status;

		/* A true value sets the modifier's flag. */
		held = vm_to_boolean(value);
		if (present && held)
			event->mouse.modifiers |= flags[index];
	}

	/* Succeeded: the modifiers are taken. */
	return 0;
}

/* Reports the key's value (key). */
static int
input_key_key(
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
	status = input_event_this(realm, this_value, &event);
	if (status != 0)
		return status;

	/* An event without a key reports the empty string. */
	if (event->key == NULL) {
		status = bind_string(realm, "", result);
		return status;
	}

	/* Succeeded: the key is reported. */
	*result = vm_value_cell(event->key);
	return 0;
}

/* Reports the key's physical code (code). */
static int
input_key_code(
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
	status = input_event_this(realm, this_value, &event);
	if (status != 0)
		return status;

	/* An event without a code reports the empty string. */
	if (event->code == NULL) {
		status = bind_string(realm, "", result);
		return status;
	}

	/* Succeeded: the code is reported. */
	*result = vm_value_cell(event->code);
	return 0;
}

/* Reports whether the key is held and repeating (repeat). */
static int
input_key_repeat(
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
	status = input_event_this(realm, this_value, &event);
	if (status != 0)
		return status;

	/* Succeeded: the flag is reported. */
	*result = vm_value_boolean(event->repeat);
	return 0;
}

/* Reports where the key is on the keyboard (location: every key is standard in this pass). */
static int
input_key_location(
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
	status = input_event_this(realm, this_value, &event);
	if (status != 0)
		return status;

	/* Succeeded: the standard location. */
	*result = vm_value_int32(0);
	return 0;
}

/* Reports whether the key is part of a composition (isComposing: there is no input method yet). */
static int
input_key_composing(
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
	status = input_event_this(realm, this_value, &event);
	if (status != 0)
		return status;

	/* Succeeded: never composing. */
	*result = VM_VALUE_FALSE;
	return 0;
}

/*
 * Reports the key's legacy number (keyCode, which): a letter's capital,
 * a digit's character, a named key's number, or 0.
 */
static int
input_key_number(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_event *event;
	int number;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The event. */
	status = input_event_this(realm, this_value, &event);
	if (status != 0)
		return status;

	/* An event without a code has no number. */
	number = 0;
	if (event->code != NULL)
		number = input_string_code(event->code);

	/* Succeeded: the number is reported. */
	*result = vm_value_int32(number);
	return 0;
}

/* Reports the wheel's horizontal distance in pixels (deltaX). */
static int
input_delta_x(
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
	status = input_event_this(realm, this_value, &event);
	if (status != 0)
		return status;

	/* Succeeded: the distance is reported. */
	*result = vm_value_number(event->mouse.delta_x);
	return 0;
}

/* Reports the wheel's vertical distance in pixels (deltaY; positive is down). */
static int
input_delta_y(
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
	status = input_event_this(realm, this_value, &event);
	if (status != 0)
		return status;

	/* Succeeded: the distance is reported. */
	*result = vm_value_number(event->mouse.delta_y);
	return 0;
}

/* Reports zero: the wheel's depth (deltaZ), and the unit of its distances (deltaMode: pixels). */
static int
input_delta_zero(
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
	status = input_event_this(realm, this_value, &event);
	if (status != 0)
		return status;

	/* Succeeded: zero. */
	*result = vm_value_int32(0);
	return 0;
}

/* Reports the other target of a focus change (relatedTarget: not kept in this pass). */
static int
input_related_target(
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
	status = input_event_this(realm, this_value, &event);
	if (status != 0)
		return status;

	/* Succeeded: no other target. */
	*result = VM_VALUE_NULL;
	return 0;
}

/* Makes a KeyboardEvent (new KeyboardEvent(type, init)) with the init's key, code, repeat and modifiers. */
static int
input_construct_keyboard(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_event *event;
	struct vm_cell *event_root;
	vm_value init;
	vm_value value;
	int present;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The event with its type and init. */
	status = bind_event_construct(realm, BIND_KEYBOARD_EVENT, args, count, result, &event);
	if (status != 0)
		return status;
	event_root = vm_value_as_cell(*result);
	status = vm_heap_add_root(realm->heap, &event_root);
	if (status != 0)
		return status;

	/* The key. */
	init = js_argument(args, count, 1);
	status = input_init_string(realm, init, "key", &event->key);
	if (status != 0)
		goto cleanup;

	/* The code. */
	status = input_init_string(realm, init, "code", &event->code);
	if (status != 0)
		goto cleanup;

	/* Whether it repeats. */
	status = bind_get_option(realm, init, "repeat", &present, &value);
	if (status != 0)
		goto cleanup;
	event->repeat = vm_to_boolean(value);

	/* The modifiers. */
	status = bind_event_init_modifiers(realm, init, event);
	if (status != 0)
		goto cleanup;

	/* The initialized event now belongs to the constructor's caller. */
cleanup:
	vm_heap_remove_root(realm->heap, &event_root);
	if (status != 0)
		return status;

	/* Succeeded: the event is made. */
	return 0;
}

/* Makes a WheelEvent (new WheelEvent(type, init)) with the init's pointer and distances. */
static int
input_construct_wheel(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_event *event;
	struct vm_cell *event_root;
	vm_value init;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The event with its type and init. */
	status = bind_event_construct(realm, BIND_WHEEL_EVENT, args, count, result, &event);
	if (status != 0)
		return status;
	event_root = vm_value_as_cell(*result);
	status = vm_heap_add_root(realm->heap, &event_root);
	if (status != 0)
		return status;

	/* The pointer's part of the init. */
	init = js_argument(args, count, 1);
	status = bind_event_init_mouse(realm, init, event);
	if (status != 0)
		goto cleanup;

	/* The horizontal distance. */
	status = input_init_number(realm, init, "deltaX", &event->mouse.delta_x);
	if (status != 0)
		goto cleanup;

	/* The vertical distance. */
	status = input_init_number(realm, init, "deltaY", &event->mouse.delta_y);
	if (status != 0)
		goto cleanup;

	/* The initialized event now belongs to the constructor's caller. */
cleanup:
	vm_heap_remove_root(realm->heap, &event_root);
	if (status != 0)
		return status;

	/* Succeeded: the event is made. */
	return 0;
}

/* Makes a FocusEvent (new FocusEvent(type, init)). */
static int
input_construct_focus(
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
	status = bind_event_construct(realm, BIND_FOCUS_EVENT, args, count, result, &event);
	if (status != 0)
		return status;

	/* Succeeded: the event is made. */
	return 0;
}

/* Finds the event state a getter's this value stands for, throwing a TypeError otherwise. */
static int
input_event_this(
	struct vm_realm *realm,
	vm_value this_value,
	struct bind_event **event)
{
	int status;

	/* The state, or the error of a getter on the wrong object. */
	*event = bind_event_of(this_value);
	if (*event == NULL) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Succeeded: the event is found. */
	return 0;
}

/* Reports whether an event says a modifier was held. */
static int
input_modifier(
	struct vm_realm *realm,
	vm_value this_value,
	unsigned modifier,
	vm_value *result)
{
	struct bind_event *event;
	int status;

	/* The event. */
	status = input_event_this(realm, this_value, &event);
	if (status != 0)
		return status;

	/* A held modifier is true. */
	*result = VM_VALUE_FALSE;
	if ((event->mouse.modifiers & modifier) != 0U)
		*result = VM_VALUE_TRUE;

	/* Succeeded: the flag is reported. */
	return 0;
}

/* Takes a string member of an event's init, when it is there (it stays as it was otherwise). */
static int
input_init_string(
	struct vm_realm *realm,
	vm_value init,
	const char *name,
	struct vm_string **string)
{
	vm_value value;
	int present;
	int status;

	/* The member. */
	status = bind_get_option(realm, init, name, &present, &value);
	if (status != 0)
		return status;
	if (!present)
		return 0;

	/* Its string. */
	status = bind_to_string(realm, value, string);
	if (status != 0)
		return status;

	/* Succeeded: the string is taken. */
	return 0;
}

/* Takes a number member of an event's init, when it is there (it stays as it was otherwise). */
static int
input_init_number(
	struct vm_realm *realm,
	vm_value init,
	const char *name,
	double *number)
{
	vm_value value;
	int present;
	int status;

	/* The member. */
	status = bind_get_option(realm, init, name, &present, &value);
	if (status != 0)
		return status;
	if (!present)
		return 0;

	/* Its number. */
	status = vm_to_number(realm, value, number);
	if (status != 0)
		return status;

	/* Succeeded: the number is taken. */
	return 0;
}

/*
 * Finds the legacy number of a key's code: KeyA to KeyZ are the capitals'
 * characters, Digit0 to Digit9 the digits', the named keys the table's,
 * and any other code 0.
 */
static int
input_string_code(
	const struct vm_string *code)
{
	const unsigned char *latin1;
	size_t index;
	int differs;
	int same;

	/* Codes of letters and digits are plain ASCII; a code of wider characters is none of them. */
	if ((code->flags & VM_STRING_WIDE) != 0U)
		return 0;
	latin1 = vm_string_latin1(code);

	/* KeyA to KeyZ: the capital letter. */
	if (code->length == 4U) {
		differs = memcmp(latin1, "Key", 3);
		if (differs == 0 &&
		    latin1[3] >= 'A' &&
		    latin1[3] <= 'Z')
			return (int)latin1[3];
	}

	/* Digit0 to Digit9: the digit. */
	if (code->length == 6U) {
		differs = memcmp(latin1, "Digit", 5);
		if (differs == 0 &&
		    latin1[5] >= '0' &&
		    latin1[5] <= '9')
			return (int)latin1[5];
	}

	/* The named keys, by the table. */
	for (index = 0; input_key_numbers[index].code != NULL; index++) {
		same = vm_string_equal_ascii(code, input_key_numbers[index].code);
		if (same)
			return input_key_numbers[index].number;
	}

	/* Any other key has none. */
	return 0;
}
