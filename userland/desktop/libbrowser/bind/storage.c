/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Web Storage for scripts (ws074-p080): the Storage interface and the
 * window's sessionStorage and localStorage.
 *
 * Each window has one Storage object per area, made when a script first
 * asks for it; its methods ask the host, which keeps the items of the
 * page's origin (page/storage.c).  The engine has no exotic objects, so
 * the items are reached through the methods only: a property of the
 * object (localStorage.name) is an ordinary property, not an item.
 */

#include "bind/internal.h"

#include <errno.h>
#include <string.h>

static int storage_object(struct bind_window *window, int area, vm_value *result);
static int storage_area_of(struct vm_realm *realm, vm_value this_value, int *area);
static int storage_units(struct vm_realm *realm, vm_value value, struct wb_units *units);
static int storage_length(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int storage_key(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int storage_get_item(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int storage_set_item(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int storage_remove_item(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int storage_clear(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);

/*
 * The attributes of Storage.  The table is constant for the life of the
 * program.
 */
static const struct bind_attribute storage_attributes[] = {
	{ "length", storage_length, NULL },
	{ NULL, NULL, NULL }
};

/*
 * The operations of Storage.  The table is constant for the life of the
 * program.
 */
static const struct bind_operation storage_operations[] = {
	{ "key", 1, storage_key },
	{ "getItem", 1, storage_get_item },
	{ "setItem", 2, storage_set_item },
	{ "removeItem", 1, storage_remove_item },
	{ "clear", 0, storage_clear },
	{ NULL, 0, NULL }
};

/*
 * The Storage interface.
 */
const struct bind_interface bind_storage_interface = {
	"Storage", BIND_NO_PARENT, 0, NULL, storage_attributes, storage_operations, NULL
};

/*
 * Reports the window's localStorage (the same object each time).
 */
int
bind_local_storage(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	int status;

	UNUSED_PARAMETER(this_value);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The window's object for the area. */
	window = bind_window_of(realm);
	status = storage_object(window, BIND_STORAGE_LOCAL, result);
	if (status != 0)
		return status;

	/* Succeeded: the object. */
	return 0;
}

/*
 * Reports the window's sessionStorage (the same object each time).
 */
int
bind_session_storage(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	int status;

	UNUSED_PARAMETER(this_value);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The window's object for the area. */
	window = bind_window_of(realm);
	status = storage_object(window, BIND_STORAGE_SESSION, result);
	if (status != 0)
		return status;

	/* Succeeded: the object. */
	return 0;
}

/* Finds the window's Storage object of an area, made the first time. */
static int
storage_object(
	struct bind_window *window,
	int area,
	vm_value *result)
{
	struct vm_object **slot;
	struct vm_object *object;

	/* The window's place for the area's object. */
	slot = &window->session_storage;
	if (area == BIND_STORAGE_LOCAL)
		slot = &window->local_storage;

	/* One made before. */
	if (*slot != NULL) {
		*result = vm_value_cell(*slot);
		return 0;
	}

	/* A new one that knows its area; the window keeps it. */
	object = vm_object_create(window->realm->heap, window->prototypes[BIND_STORAGE]);
	if (object == NULL)
		return ENOMEM;
	object->kind = VM_KIND_PLATFORM;
	object->internal = vm_value_int32(area);
	*slot = object;

	/* Succeeded: the object. */
	*result = vm_value_cell(object);
	return 0;
}

/* Finds the area of the Storage object a method's this value is, throwing a TypeError otherwise. */
static int
storage_area_of(
	struct vm_realm *realm,
	vm_value this_value,
	int *area)
{
	struct bind_window *window;
	struct vm_object *object;
	int is_object;
	int status;

	/* An object. */
	window = bind_window_of(realm);
	is_object = vm_value_is_object(this_value);
	if (!is_object) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* One of the window's two. */
	object = (struct vm_object *)vm_value_as_cell(this_value);
	if (object != window->local_storage && object != window->session_storage) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Succeeded: its area. */
	*area = vm_value_as_int32(object->internal);
	return 0;
}

/* Reads a value as a string's UTF-16 units. */
static int
storage_units(
	struct vm_realm *realm,
	vm_value value,
	struct wb_units *units)
{
	struct vm_string *string;
	int status;

	/* The string. */
	status = bind_to_string(realm, value, &string);
	if (status != 0)
		return status;

	/* Succeeded: its units. */
	status = vm_string_append_units(string, units);
	if (status != 0)
		return status;
	return 0;
}

/* Reports how many items the area has (length). */
static int
storage_length(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	size_t items;
	int area;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The area. */
	window = bind_window_of(realm);
	status = storage_area_of(realm, this_value, &area);
	if (status != 0)
		return status;

	/* The host's count; a host without storage has none. */
	items = 0;
	if (window->host.storage != NULL) {
		status = window->host.storage->length(window->host.context, area, &items);
		if (status != 0)
			return status;
	}

	/* Succeeded: the count. */
	*result = vm_value_number((double)items);
	return 0;
}

/* Reports the key at a place of the area, or null past its end (key). */
static int
storage_key(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct wb_units key;
	uint32_t index;
	int found;
	int area;
	int status;

	/* The area and the place. */
	window = bind_window_of(realm);
	status = storage_area_of(realm, this_value, &area);
	if (status != 0)
		return status;
	status = vm_to_uint32(realm, js_argument(args, count, 0), &index);
	if (status != 0)
		return status;

	/* The host's key; a host without storage has none. */
	found = 0;
	wb_units_init(&key);
	if (window->host.storage != NULL)
		status = window->host.storage->key(window->host.context, area, index, &key, &found);
	if (status != 0) {
		wb_units_release(&key);
		return status;
	}

	/* No key there. */
	if (!found) {
		wb_units_release(&key);
		*result = VM_VALUE_NULL;
		return 0;
	}

	/* Succeeded: the key as a string. */
	status = bind_units(realm, key.data, key.length, result);
	wb_units_release(&key);
	if (status != 0)
		return status;
	return 0;
}

/* Reports the value of a key, or null when the area does not have it (getItem). */
static int
storage_get_item(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct wb_units key;
	struct wb_units value;
	int found;
	int area;
	int status;

	/* The area and the key. */
	window = bind_window_of(realm);
	status = storage_area_of(realm, this_value, &area);
	if (status != 0)
		return status;
	wb_units_init(&key);
	status = storage_units(realm, js_argument(args, count, 0), &key);
	if (status != 0) {
		wb_units_release(&key);
		return status;
	}

	/* The host's value; a host without storage has none. */
	found = 0;
	wb_units_init(&value);
	if (window->host.storage != NULL)
		status = window->host.storage->get(window->host.context, area, key.data, key.length, &value, &found);
	wb_units_release(&key);
	if (status != 0) {
		wb_units_release(&value);
		return status;
	}

	/* No such key. */
	if (!found) {
		wb_units_release(&value);
		*result = VM_VALUE_NULL;
		return 0;
	}

	/* Succeeded: the value as a string. */
	status = bind_units(realm, value.data, value.length, result);
	wb_units_release(&value);
	if (status != 0)
		return status;
	return 0;
}

/*
 * Sets a key's value (setItem); an item past the origin's quota throws a
 * QuotaExceededError.
 */
static int
storage_set_item(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct wb_units key;
	struct wb_units value;
	int area;
	int status;

	/* The area, the key and the value. */
	*result = VM_VALUE_UNDEFINED;
	window = bind_window_of(realm);
	status = storage_area_of(realm, this_value, &area);
	if (status != 0)
		return status;
	wb_units_init(&key);
	wb_units_init(&value);
	status = storage_units(realm, js_argument(args, count, 0), &key);
	if (status == 0)
		status = storage_units(realm, js_argument(args, count, 1), &value);
	if (status != 0) {
		wb_units_release(&key);
		wb_units_release(&value);
		return status;
	}

	/* The host keeps it; a host without storage drops it. */
	if (window->host.storage != NULL)
		status = window->host.storage->set(window->host.context, area, key.data, key.length, value.data, value.length);
	wb_units_release(&key);
	wb_units_release(&value);

	/* An item past the quota. */
	if (status == ENOSPC) {
		status = bind_throw_dom(realm, "QuotaExceededError", "Setting the value exceeded the quota.");
		return status;
	}

	/* Any other failure. */
	if (status != 0)
		return status;

	/* Succeeded: the item is set. */
	return 0;
}

/* Removes a key and its value (removeItem). */
static int
storage_remove_item(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct wb_units key;
	int area;
	int status;

	/* The area and the key. */
	*result = VM_VALUE_UNDEFINED;
	window = bind_window_of(realm);
	status = storage_area_of(realm, this_value, &area);
	if (status != 0)
		return status;
	wb_units_init(&key);
	status = storage_units(realm, js_argument(args, count, 0), &key);
	if (status != 0) {
		wb_units_release(&key);
		return status;
	}

	/* The host forgets it. */
	if (window->host.storage != NULL)
		status = window->host.storage->remove(window->host.context, area, key.data, key.length);
	wb_units_release(&key);
	if (status != 0)
		return status;

	/* Succeeded: the item is gone. */
	return 0;
}

/* Removes every item of the area (clear). */
static int
storage_clear(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	int area;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The area. */
	*result = VM_VALUE_UNDEFINED;
	window = bind_window_of(realm);
	status = storage_area_of(realm, this_value, &area);
	if (status != 0)
		return status;

	/* The host empties it. */
	if (window->host.storage != NULL) {
		status = window->host.storage->clear(window->host.context, area);
		if (status != 0)
			return status;
	}

	/* Succeeded: the area is empty. */
	return 0;
}
