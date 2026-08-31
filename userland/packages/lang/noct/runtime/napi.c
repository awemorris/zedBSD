/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * zedBSD Noct native APIs
 */

#include "userland/packages/lang/noct/runtime/zedbsd-api.h"
#include "libc/heap.h"

#include <noct/noct.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SERIALIZE_LIMIT 4096U
#define SERIALIZE_DEPTH 4U
#define SERIALIZE_ITEMS 64U

struct imported_source {
	struct imported_source *next;
	char *path;
	char *source;
};

struct active_napi {
	const struct noct_services *services;
	noct_write_fn write;
	void *write_context;
	size_t arena_size;
	size_t source_max;
	struct imported_source *imports;
	struct environment *environment;
};

struct api_item {
	const char *global_name;
	const char *field_name;
	size_t parameter_count;
	const char *parameters[6];
	bool (*function)(NoctEnv *env);
};

struct serializer {
	NoctEnv *env;
	size_t bytes;
};

static struct active_napi active;

static bool register_intrinsics(NoctEnv *env, struct api_item *items, size_t item_count);
static bool register_module(NoctEnv *env, const char *module, struct api_item *items, size_t item_count);
static void write_bytes(const char *bytes, size_t length);
static void write_string(const char *string);
static bool return_int(NoctEnv *env, int value);
static bool return_string(NoctEnv *env, const char *value);
static void serialize_emit(struct serializer *output, const char *bytes, size_t length);
static void serialize_string(struct serializer *output, const char *string);
static bool serialize_array(struct serializer *output, NoctValue *value, unsigned depth);
static bool serialize_value(struct serializer *output, NoctValue *value, unsigned depth, bool quoted);
static bool serialize_dict(struct serializer *output, NoctValue *value, unsigned depth);
static bool cfunc_console_write(NoctEnv *env);
static bool cfunc_console_print(NoctEnv *env);
static bool cfunc_console_gets(NoctEnv *env);
static int services_ready(void);
static bool cfunc_screen_get_width(NoctEnv *env);
static bool cfunc_screen_get_height(NoctEnv *env);
static bool cfunc_screen_clear(NoctEnv *env);
static bool cfunc_screen_clear_row(NoctEnv *env);
static bool cfunc_screen_put(NoctEnv *env);
static bool cfunc_screen_set_cursor(NoctEnv *env);
static bool cfunc_screen_show_cursor(NoctEnv *env);
static bool cfunc_keyboard_poll(NoctEnv *env);
static bool cfunc_keyboard_read(NoctEnv *env);
static bool cfunc_keyboard_is_printable(NoctEnv *env);
static bool make_directory_entry(NoctEnv *env, NoctValue *dictionary, NoctValue *scratch, const struct noct_dirent *entry);
static bool cfunc_directory_list(NoctEnv *env);
static int ascii_equal_folded(const char *left, const char *right);
static bool cfunc_directory_stat(NoctEnv *env);
static bool cfunc_system_get_os_name(NoctEnv *env);
static bool cfunc_system_pcall(NoctEnv *env);
static bool cfunc_system_get_env(NoctEnv *env);
static bool cfunc_system_set_env(NoctEnv *env);
static bool cfunc_system_unset_env(NoctEnv *env);
static bool cfunc_system_list_env(NoctEnv *env);
static bool cfunc_system_memory_usage(NoctEnv *env);
static bool cfunc_system_import(NoctEnv *env);

/*
 * Implements the key normalize bios ax operation.
 */
int
key_normalize_bios_ax(
	uint16_t bios_ax)
{
	unsigned ascii;
	unsigned scan;

	ascii = bios_ax & 0xffU;
	scan = bios_ax >> 8;

	/* Handles the ascii condition. */
	if (ascii != 0)
		return (int)ascii;

	/*
 * Some genuine PC-98 BIOS revisions return Tab as scan 0x0f with a
	 * zero ASCII byte.  Noct and Remacs use the ordinary control character,
	 * so normalize both BIOS forms at the shared keyboard boundary. */
	if (scan == 0x0fU)
		return NOCT_BEUI_KEY_TAB;

	/* Returns the computed result. */
	return 0x100 | scan;
}

/*
 * Implements the noct napi register operation.
 */
int
noct_napi_register(
	NoctEnv *env,
	const struct noct_options *options)
{
	static struct api_item console[] = {
	    {"Console.print", "print", 1, {"value"}, cfunc_console_print},
	    {"Console.write", "write", 1, {"text"}, cfunc_console_write},
	    {"Console.gets", "gets", 0, {NULL}, cfunc_console_gets},
	};
	static struct api_item intrinsics[] = {
	    {"print", NULL, 1, {"value"}, cfunc_console_print},
	    {"gets", NULL, 0, {NULL}, cfunc_console_gets},
	};
	static struct api_item screen[] = {
	    {"Screen.getWidth", "getWidth", 0, {NULL}, cfunc_screen_get_width},
	    {"Screen.getHeight",
	     "getHeight",
	     0,
	     {NULL},
	     cfunc_screen_get_height},
	    {"Screen.clear", "clear", 0, {NULL}, cfunc_screen_clear},
	    {"Screen.clearRow", "clearRow", 1, {"row"}, cfunc_screen_clear_row},
	    {"Screen.put",
	     "put",
	     4,
	     {"row", "column", "text", "attribute"},
	     cfunc_screen_put},
	    {"Screen.setCursor",
	     "setCursor",
	     2,
	     {"row", "column"},
	     cfunc_screen_set_cursor},
	    {"Screen.showCursor",
	     "showCursor",
	     1,
	     {"visible"},
	     cfunc_screen_show_cursor},
	};
	static struct api_item keyboard[] = {
	    {"Keyboard.poll", "poll", 0, {NULL}, cfunc_keyboard_poll},
	    {"Keyboard.read", "read", 0, {NULL}, cfunc_keyboard_read},
	    {"Keyboard.isPrintable",
	     "isPrintable",
	     1,
	     {"code"},
	     cfunc_keyboard_is_printable},
	};
	static struct api_item directory[] = {
	    {"Directory.list", "list", 1, {"path"}, cfunc_directory_list},
	    {"Directory.stat", "stat", 1, {"path"}, cfunc_directory_stat},
	};
	static struct api_item system[] = {
	    {"System.getOSName",
	     "getOSName",
	     0,
	     {NULL},
	     cfunc_system_get_os_name},
	    {"System.pcall", "pcall", 3, {"f", "a", "b"}, cfunc_system_pcall},
	    {"System.import", "import", 1, {"path"}, cfunc_system_import},
	    {"System.memoryUsage",
	     "memoryUsage",
	     0,
	     {NULL},
	     cfunc_system_memory_usage},
	    {"System.getEnv", "getEnv", 1, {"name"}, cfunc_system_get_env},
	    {"System.setEnv",
	     "setEnv",
	     2,
	     {"name", "value"},
	     cfunc_system_set_env},
	    {"System.unsetEnv",
	     "unsetEnv",
	     1,
	     {"name"},
	     cfunc_system_unset_env},
	    {"System.listEnv", "listEnv", 0, {NULL}, cfunc_system_list_env},
	};

	/* Handles the env availability. */
	if (env == NULL || options == NULL || options->write == NULL ||
	    active.write != NULL)

		/* Reports successful completion. */
		return 0;
	active.services = options->services;
	active.write = options->write;
	active.write_context = options->write_context;
	active.arena_size = options->arena_size;
	active.source_max = ZEDBSD_NOCT_SOURCE_MAX;
	active.imports = NULL;
	active.environment = options->environment;

	/*
 * BeUI registers its own module, key dictionary, and image registry
	 * upstream; the boot target only supplies the backend. */
	if (!noct_register_api_beui(env, options->services != NULL
					     ? options->services->beui
					     : NULL) ||
	    !register_intrinsics(env, intrinsics,
				 sizeof(intrinsics) / sizeof(intrinsics[0])) ||
	    !register_module(env, "Console", console,
			     sizeof(console) / sizeof(console[0])) ||
	    !register_module(env, "Screen", screen,
			     sizeof(screen) / sizeof(screen[0])) ||
	    !register_module(env, "Keyboard", keyboard,
			     sizeof(keyboard) / sizeof(keyboard[0])) ||
	    !register_module(env, "Directory", directory,
			     sizeof(directory) / sizeof(directory[0])) ||
	    !register_module(env, "System", system,
			     sizeof(system) / sizeof(system[0]))) {
		noct_napi_cleanup();

		/* Reports successful completion. */
		return 0;
	}

	/* Reports operation failure. */
	return 1;
}

/*
 * Implements the noct napi cleanup operation.
 */
void
noct_napi_cleanup(
	void)
{
	struct imported_source *next;
	struct imported_source *source;

	source = active.imports;

	/*
 * Restores text mode even when a script raises or omits BeUI.close(),
	 * and releases any image the script left loaded. */
	noct_beui_cleanup();

	/* Continue while the operation condition remains true. */
	while (source != NULL) {

		next = source->next;

		free(source);
		source = next;
	}
	memset(&active, 0, sizeof(active));
}

/* Intrinsic-like global conveniences are declared in one auditable table. */
static bool
register_intrinsics(
	NoctEnv *env,
	struct api_item *items,
	size_t item_count)
{
	struct api_item *item;
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < item_count; index++) {
				item = &items[index];

		/* Handles a failed noct register cfunc operation. */
		if (!noct_register_cfunc(
			env, item->global_name, item->parameter_count,
			item->parameters, item->function, NULL))

			/* Reports operation failure. */
			return false;
	}

	/* Reports successful completion. */
	return true;
}

/* Supports the register module operation. */
static bool
register_module(
	NoctEnv *env,
	const char *module,
	struct api_item *items,
	size_t item_count)
{
	struct api_item *item;

	NoctValue dictionary;
	NoctValue function;
	size_t index;
	bool ok = false;

	memset(&dictionary, 0, sizeof(dictionary));
	memset(&function, 0, sizeof(function));

	/* Handles a failed noct pin local operation. */
	if (!noct_pin_local(env, 2, &dictionary, &function))
		return false;

	/* Handles a failed noct make empty dict operation. */
	if (!noct_make_empty_dict(env, &dictionary) ||
	    !noct_set_global(env, module, &dictionary))
		goto out;

	/* Process each remaining element. */
	for (index = 0; index < item_count; index++) {
				item = &items[index];

		/* Handles a failed noct register cfunc operation. */
		if (!noct_register_cfunc(
			env, item->global_name, item->parameter_count,
			item->parameters, item->function, NULL) ||
		    !noct_get_global(env, item->global_name, &function) ||
		    !noct_set_dict_elem_cstr(env, &dictionary, item->field_name,
					     &function))
			goto out;
	}
	ok = true;
out:
	(void)noct_unpin_local(env, 2, &dictionary, &function);

	/* Returns the computed result. */
	return ok;
}

/* Supports the write bytes operation. */
static void
write_bytes(
	const char *bytes,
	size_t length)
{
	/* Handles the write availability. */
	if (active.write != NULL && length != 0)
		(void)active.write(active.write_context, bytes, length);
}

/* Supports the write string operation. */
static void
write_string(
	const char *string)
{
	/* Handles the string availability. */
	if (string != NULL)
		write_bytes(string, strlen(string));
}

/* Supports the return int operation. */
static bool
return_int(
	NoctEnv *env,
	int value)
{
	NoctValue result;
	bool ok;

	memset(&result, 0, sizeof(result));

	/* Handles a failed noct pin local operation. */
	if (!noct_pin_local(env, 1, &result))
		return false;
	ok = noct_set_return_make_int(env, &result, value);
	(void)noct_unpin_local(env, 1, &result);

	/* Returns the computed result. */
	return ok;
}

/* Supports the return string operation. */
static bool
return_string(
	NoctEnv *env,
	const char *value)
{
	NoctValue result;
	bool ok;

	memset(&result, 0, sizeof(result));

	/* Handles a failed noct pin local operation. */
	if (!noct_pin_local(env, 1, &result))
		return false;
	ok = noct_set_return_make_string(env, &result, value);
	(void)noct_unpin_local(env, 1, &result);

	/* Returns the computed result. */
	return ok;
}

/* Supports the serialize emit operation. */
static void
serialize_emit(
	struct serializer *output,
	const char *bytes,
	size_t length)
{
	size_t remaining;

	/* Handles the output condition. */
	if (output->bytes >= SERIALIZE_LIMIT)
		return;
	remaining = SERIALIZE_LIMIT - output->bytes;

	/* Checks the current data length. */
	if (length > remaining)
		length = remaining;
	write_bytes(bytes, length);
	output->bytes += length;
}

/* Supports the serialize string operation. */
static void
serialize_string(
	struct serializer *output,
	const char *string)
{
	serialize_emit(output, string, strlen(string));
}

/* Supports the serialize array operation. */
static bool
serialize_array(
	struct serializer *output,
	NoctValue *value,
	unsigned depth)
{
	NoctValue element;
	size_t count;
	size_t index;

	memset(&element, 0, sizeof(element));

	/* Handles a failed noct get array size operation. */
	if (!noct_get_array_size(output->env, value, &count))
		return false;
	serialize_string(output, "[");

	/* Handles the depth condition. */
	if (depth >= SERIALIZE_DEPTH) {
		serialize_string(output, "...]");

		/* Reports successful completion. */
		return true;
	}

	/* Process each remaining element. */
	for (index = 0; index < count && index < SERIALIZE_ITEMS; index++) {
		/* Checks the current index. */
		if (index != 0)
			serialize_string(output, ", ");

		/* Handles a failed noct get array elem operation. */
		if (!noct_get_array_elem(output->env, value, index, &element) ||
		    !serialize_value(output, &element, depth + 1U, true))

			/* Reports operation failure. */
			return false;
	}

	/* Checks the remaining item count. */
	if (count > SERIALIZE_ITEMS)
		serialize_string(output, ", ...");
	serialize_string(output, "]");

	/* Reports successful completion. */
	return true;
}

/* Supports the serialize value operation. */
static bool
serialize_value(
	struct serializer *output,
	NoctValue *value,
	unsigned depth,
	bool quoted)
{
	bool function_result;
	int number_local;
	int64_t number_local1;
	float number_local2;
	double number_local3;
	char buffer[64];
	const char *string;
	int type;

	/* Handles a failed noct get value type operation. */
	if (!noct_get_value_type(output->env, value, &type))
		return false;

	/* Dispatch the selected syntax or record type. */
	switch (type) {
	case NOCT_VALUE_INT:

	/* Handles a failed noct get int operation. */
	if (!noct_get_int(output->env, value, &number_local))
		return false;
	(void)snprintf(buffer, sizeof(buffer), "%d", number_local);
	serialize_string(output, buffer);

	/* Reports successful completion. */
	return true;
	case NOCT_VALUE_LONG:

	/* Handles a failed noct get long operation. */
	if (!noct_get_long(output->env, value, &number_local1))
		return false;
	(void)snprintf(buffer, sizeof(buffer), "%" PRId64, number_local1);
	serialize_string(output, buffer);

	/* Reports successful completion. */
	return true;
	case NOCT_VALUE_FLOAT:

	/* Handles a failed noct get float operation. */
	if (!noct_get_float(output->env, value, &number_local2))
		return false;
	(void)snprintf(buffer, sizeof(buffer), "%.7g", (double)number_local2);
	serialize_string(output, buffer);

	/* Reports successful completion. */
	return true;
	case NOCT_VALUE_DOUBLE:

	/* Handles a failed noct get double operation. */
	if (!noct_get_double(output->env, value, &number_local3))
		return false;
	(void)snprintf(buffer, sizeof(buffer), "%.15g", number_local3);
	serialize_string(output, buffer);

	/* Reports successful completion. */
	return true;
	case NOCT_VALUE_STRING:
		/* Handles a failed noct get string operation. */
		if (!noct_get_string(output->env, value, &string))
			return false;

		/* Handles the quoted condition. */
		if (quoted)
			serialize_string(output, "\"");
		serialize_string(output, string);

		/* Handles the quoted condition. */
		if (quoted)
			serialize_string(output, "\"");

		/* Reports successful completion. */
		return true;
	case NOCT_VALUE_ARRAY:
		/* Obtains the serialize array result. */
		function_result = serialize_array(output, value, depth);

		/* Returns the computed result. */
		return function_result;
	case NOCT_VALUE_DICT:
		/* Obtains the serialize dict result. */
		function_result = serialize_dict(output, value, depth);

		/* Returns the computed result. */
		return function_result;
	case NOCT_VALUE_FUNC:
		serialize_string(output, "<func>");

		/* Reports successful completion. */
		return true;
	case NOCT_VALUE_PACKED:
		serialize_string(output, "<packed>");

		/* Reports successful completion. */
		return true;
	default:
		serialize_string(output, "<unknown>");

		/* Reports successful completion. */
		return true;
	}
}

/* Supports the serialize dict operation. */
static bool
serialize_dict(
	struct serializer *output,
	NoctValue *value,
	unsigned depth)
{
	NoctValue key;
	NoctValue element;
	size_t count;
	size_t index;
	const char *name;

	memset(&key, 0, sizeof(key));
	memset(&element, 0, sizeof(element));

	/* Handles a failed noct get dict size operation. */
	if (!noct_get_dict_size(output->env, value, &count))
		return false;
	serialize_string(output, "{");

	/* Handles the depth condition. */
	if (depth >= SERIALIZE_DEPTH) {
		serialize_string(output, "...}");

		/* Reports successful completion. */
		return true;
	}

	/* Process each remaining element. */
	for (index = 0; index < count && index < SERIALIZE_ITEMS; index++) {
		/* Checks the current index. */
		if (index != 0)
			serialize_string(output, ", ");

		/* Handles a failed noct get dict by index operation. */
		if (!noct_get_dict_by_index(output->env, value, index, &key,
					    &element) ||
		    !noct_get_string(output->env, &key, &name))

			/* Reports operation failure. */
			return false;
		serialize_string(output, name);
		serialize_string(output, ": ");

		/* Handles a failed serialize value operation. */
		if (!serialize_value(output, &element, depth + 1U, true))
			return false;
	}

	/* Checks the remaining item count. */
	if (count > SERIALIZE_ITEMS)
		serialize_string(output, ", ...");
	serialize_string(output, "}");

	/* Reports successful completion. */
	return true;
}

/* Supports the cfunc console write operation. */
static bool
cfunc_console_write(
	NoctEnv *env)
{
	NoctValue value;
	const char *text;
	bool ok = false;

	memset(&value, 0, sizeof(value));

	/* Handles a failed noct pin local operation. */
	if (!noct_pin_local(env, 1, &value))
		return false;

	/* Handles the noct get arg condition. */
	if (noct_get_arg(env, 0, &value) &&
	    noct_get_string(env, &value, &text)) {
		write_string(text);
		ok = true;
	}
	(void)noct_unpin_local(env, 1, &value);

	/* Returns the computed result. */
	return ok;
}

/* Supports the cfunc console print operation. */
static bool
cfunc_console_print(
	NoctEnv *env)
{
	NoctValue value;
	struct serializer output;
	bool ok = false;

	memset(&value, 0, sizeof(value));
	output.env = env;
	output.bytes = 0;

	/* Handles a failed noct pin local operation. */
	if (!noct_pin_local(env, 1, &value))
		return false;

	/* Handles the noct get arg condition. */
	if (noct_get_arg(env, 0, &value) &&
	    serialize_value(&output, &value, 0, false)) {
		write_string("\n");
		ok = true;
	}
	(void)noct_unpin_local(env, 1, &value);

	/* Returns the computed result. */
	return ok;
}

/* Bounded, ASCII line input.  Unlike C gets(), this can never overflow. */
static bool
cfunc_console_gets(
	NoctEnv *env)
{
	bool function_result;
	int key;
	char line[256];
	size_t length;

	length = 0;

	/* Handles a failed services ready operation. */
	if (!services_ready() || active.services->keyboard_read == NULL) {
		noct_error(env, "gets is unavailable.");

		/* Reports operation failure. */
		return false;
	}

	/* Handles the screen show cursor availability. */
	if (active.services->screen_show_cursor != NULL)
		(void)active.services->screen_show_cursor(
		    active.services->context, 1);

	/* Continue until the operation reaches a terminal state. */
	for (;;) {

		key = active.services->keyboard_read(active.services->context);

		/* Handles the selected key. */
		if (key < 0) {
			noct_error(env, "gets failed.");

			/* Reports operation failure. */
			return false;
		}

		/* Handles the selected key. */
		if (key > 0xff)
			continue;

		/* Handles the selected key. */
		if (key == '\r' || key == '\n') {
			write_string("\n");
			line[length] = '\0';

			/* Obtains the return string result. */
			function_result = return_string(env, line);

			/* Returns the computed result. */
			return function_result;
		}

		/* Handles the selected key. */
		if (key == 0x03) {
			write_string("^C\n");

			/* Obtains the return string result. */
			function_result = return_string(env, "");

			/* Returns the computed result. */
			return function_result;
		}

		/* Handles the selected key. */
		if (key == '\b' || key == 0x7f) {
			/* Checks the current data length. */
			if (length != 0) {
				length--;
				write_string("\b");
			}
			continue;
		}

		/* Handles the selected key. */
		if (key >= 32 && key < 127 && length + 1U < sizeof(line)) {
			line[length++] = (char)key;
			write_bytes((const char *)&line[length - 1U], 1U);
		}
	}
}

/* Supports the services ready operation. */
static int
services_ready(
	void)
{
	/* Returns the computed result. */
	return active.services != NULL;
}

/* Supports the cfunc screen get width operation. */
static bool
cfunc_screen_get_width(
	NoctEnv *env)
{
	bool function_result;

	/* Obtains the return int result. */
	function_result = return_int(env, 80);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the cfunc screen get height operation. */
static bool
cfunc_screen_get_height(
	NoctEnv *env)
{
	bool function_result;

	/* Obtains the return int result. */
	function_result = return_int(env, 25);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the cfunc screen clear operation. */
static bool
cfunc_screen_clear(
	NoctEnv *env)
{
	bool function_result;

	/* Handles a failed services ready operation. */
	if (!services_ready() || active.services->screen_clear == NULL ||
	    !active.services->screen_clear(active.services->context)) {
		noct_error(env, "Screen.clear is unavailable.");

		/* Reports operation failure. */
		return false;
	}

	/* Obtains the return int result. */
	function_result = return_int(env, 0);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the cfunc screen clear row operation. */
static bool
cfunc_screen_clear_row(
	NoctEnv *env)
{
	NoctValue argument;
	int row;
	bool ok = false;

	memset(&argument, 0, sizeof(argument));

	/* Handles a failed noct pin local operation. */
	if (!noct_pin_local(env, 1, &argument))
		return false;

	/* Handles a failed noct get arg check int operation. */
	if (!noct_get_arg_check_int(env, 0, &argument, &row) || row < 0 ||
	    row >= 25 || !services_ready() ||
	    active.services->screen_clear_row == NULL ||
	    !active.services->screen_clear_row(active.services->context,
					       (unsigned)row))
		noct_error(env, "Screen.clearRow received an invalid row.");
	else
		ok = return_int(env, 0);
	(void)noct_unpin_local(env, 1, &argument);

	/* Returns the computed result. */
	return ok;
}

/* Supports the cfunc screen put operation. */
static bool
cfunc_screen_put(
	NoctEnv *env)
{
	NoctValue argument;
	int row;
	int column;
	int attribute;
	int cells;
	const char *text;
	bool ok = false;

	memset(&argument, 0, sizeof(argument));

	/* Handles a failed noct pin local operation. */
	if (!noct_pin_local(env, 1, &argument))
		return false;

	/* Handles a failed noct get arg check int operation. */
	if (!noct_get_arg_check_int(env, 0, &argument, &row) ||
	    !noct_get_arg_check_int(env, 1, &argument, &column) ||
	    !noct_get_arg_check_string(env, 2, &argument, &text) ||
	    !noct_get_arg_check_int(env, 3, &argument, &attribute) || row < 0 ||
	    row >= 25 || column < 0 || column >= 80 || attribute < 0 ||
	    attribute > 255 || !services_ready() ||
	    active.services->screen_put == NULL) {
		noct_error(env, "Screen.put received an invalid argument.");
		goto out;
	}
	cells = active.services->screen_put(active.services->context,
					    (unsigned)row, (unsigned)column,
					    text, (uint8_t)attribute);

	/* Handles the cells condition. */
	if (cells < 0) {
		noct_error(env, "Screen.put failed.");
		goto out;
	}
	ok = return_int(env, cells);
out:
	(void)noct_unpin_local(env, 1, &argument);

	/* Returns the computed result. */
	return ok;
}

/* Supports the cfunc screen set cursor operation. */
static bool
cfunc_screen_set_cursor(
	NoctEnv *env)
{
	NoctValue argument;
	int row;
	int column;
	bool ok = false;

	memset(&argument, 0, sizeof(argument));

	/* Handles a failed noct pin local operation. */
	if (!noct_pin_local(env, 1, &argument))
		return false;

	/* Handles a failed noct get arg check int operation. */
	if (!noct_get_arg_check_int(env, 0, &argument, &row) ||
	    !noct_get_arg_check_int(env, 1, &argument, &column) || row < 0 ||
	    row >= 25 || column < 0 || column >= 80 || !services_ready() ||
	    active.services->screen_set_cursor == NULL ||
	    !active.services->screen_set_cursor(
		active.services->context, (unsigned)row, (unsigned)column))
		noct_error(env,
			   "Screen.setCursor received an invalid position.");
	else
		ok = return_int(env, 0);
	(void)noct_unpin_local(env, 1, &argument);

	/* Returns the computed result. */
	return ok;
}

/* Supports the cfunc screen show cursor operation. */
static bool
cfunc_screen_show_cursor(
	NoctEnv *env)
{
	NoctValue argument;
	int visible;
	bool ok = false;

	memset(&argument, 0, sizeof(argument));

	/* Handles a failed noct pin local operation. */
	if (!noct_pin_local(env, 1, &argument))
		return false;

	/* Handles a failed noct get arg check int operation. */
	if (!noct_get_arg_check_int(env, 0, &argument, &visible) ||
	    !services_ready() || active.services->screen_show_cursor == NULL ||
	    !active.services->screen_show_cursor(active.services->context,
						 visible != 0))
		noct_error(env, "Screen.showCursor failed.");
	else
		ok = return_int(env, 0);
	(void)noct_unpin_local(env, 1, &argument);

	/* Returns the computed result. */
	return ok;
}

/* Supports the cfunc keyboard poll operation. */
static bool
cfunc_keyboard_poll(
	NoctEnv *env)
{
	bool function_result;
	int key;

	/* Handles a failed services ready operation. */
	if (!services_ready() || active.services->keyboard_poll == NULL) {
		noct_error(env, "Keyboard.poll is unavailable.");

		/* Reports operation failure. */
		return false;
	}
	key = active.services->keyboard_poll(active.services->context);

	/* Handles the selected key. */
	if (key >= 0)
		key &= 0x1ff;

	/* Obtains the return int result. */
	function_result = return_int(env, key);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the cfunc keyboard read operation. */
static bool
cfunc_keyboard_read(
	NoctEnv *env)
{
	bool function_result;
	int key;

	/* Handles a failed services ready operation. */
	if (!services_ready() || active.services->keyboard_read == NULL) {
		noct_error(env, "Keyboard.read is unavailable.");

		/* Reports operation failure. */
		return false;
	}
	key = active.services->keyboard_read(active.services->context);

	/* Handles the selected key. */
	if (key >= 0)
		key &= 0x1ff;

	/* Obtains the return int result. */
	function_result = return_int(env, key);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the cfunc keyboard is printable operation. */
static bool
cfunc_keyboard_is_printable(
	NoctEnv *env)
{
	NoctValue argument;
	int key;
	bool ok = false;

	memset(&argument, 0, sizeof(argument));

	/* Handles a failed noct pin local operation. */
	if (!noct_pin_local(env, 1, &argument))
		return false;

	/* Handles a failed noct get arg check int operation. */
	if (!noct_get_arg_check_int(env, 0, &argument, &key))
		goto out;
	ok = return_int(env, (key >= 0x20 && key <= 0x7e) ||
				 (key >= 0xa1 && key <= 0xdf));
out:
	(void)noct_unpin_local(env, 1, &argument);

	/* Returns the computed result. */
	return ok;
}

/* Supports the make directory entry operation. */
static bool
make_directory_entry(
	NoctEnv *env,
	NoctValue *dictionary,
	NoctValue *scratch,
	const struct noct_dirent *entry)
{
	bool function_result;

	/* Computes the function result. */
	function_result = noct_make_empty_dict(env, dictionary) &&
	       noct_set_dict_elem_make_string(env, dictionary, "name", scratch,
					      entry->name) &&
	       noct_set_dict_elem_make_long(env, dictionary, "size", scratch,
					    (int64_t)entry->size) &&
	       noct_set_dict_elem_make_int(env, dictionary, "attributes",
					   scratch, entry->attributes) &&
	       noct_set_dict_elem_make_int(env, dictionary, "directory",
					   scratch,
					   (entry->attributes & 0x10U) != 0);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the cfunc directory list operation. */
static bool
cfunc_directory_list(
	NoctEnv *env)
{
	NoctValue argument;
	NoctValue array;
	NoctValue dictionary;
	NoctValue scratch;
	struct noct_dirent entry;
	const char *path;
	unsigned index;
	int status;
	bool ok = false;

	memset(&argument, 0, sizeof(argument));
	memset(&array, 0, sizeof(array));
	memset(&dictionary, 0, sizeof(dictionary));
	memset(&scratch, 0, sizeof(scratch));

	/* Handles a failed noct pin local operation. */
	if (!noct_pin_local(env, 4, &argument, &array, &dictionary, &scratch))
		return false;

	/* Handles a failed noct get arg check string operation. */
	if (!noct_get_arg_check_string(env, 0, &argument, &path) ||
	    !services_ready() || active.services->directory_read == NULL ||
	    !noct_make_empty_array(env, &array))
		goto error;

	/* Process each remaining element. */
	for (index = 0; index < ZEDBSD_NOCT_DIRECTORY_MAX; index++) {
		status = active.services->directory_read(
		    active.services->context, path, index, &entry);

		/* Checks the operation status. */
		if (status < 0)
			goto error;

		/* Checks the operation status. */
		if (status == 0)
			break;

		/* Handles a failed make directory entry operation. */
		if (!make_directory_entry(env, &dictionary, &scratch, &entry) ||
		    !noct_set_array_elem(env, &array, index, &dictionary))
			goto error;
	}

	/* Handles a failed directory read operation. */
	if (index == ZEDBSD_NOCT_DIRECTORY_MAX &&
	    active.services->directory_read(active.services->context, path,
					    index, &entry) != 0) {
		noct_error(env, "Directory contains too many entries.");
		goto out;
	}

	/* Handles a failed noct set return operation. */
	if (!noct_set_return(env, &array))
		goto error;
	ok = true;
	goto out;
error:
	noct_error(env, "Directory.list failed.");
out:
	(void)noct_unpin_local(env, 4, &argument, &array, &dictionary,
			       &scratch);

	/* Returns the computed result. */
	return ok;
}

/* Supports the ascii equal folded operation. */
static int
ascii_equal_folded(
	const char *left,
	const char *right)
{
	unsigned char a;
	unsigned char b;

	/* Continue while the operation condition remains true. */
	while (*left != '\0' && *right != '\0') {

		a = (unsigned char)*left++;
		b = (unsigned char)*right++;

		/* Handles the a condition. */
		if (a >= 'a' && a <= 'z')
			a -= 'a' - 'A';

		/* Handles the b condition. */
		if (b >= 'a' && b <= 'z')
			b -= 'a' - 'A';

		/* Handles the a condition. */
		if (a != b)
			return 0;
	}

	/* Returns the computed result. */
	return *left == *right;
}

/* Supports the cfunc directory stat operation. */
static bool
cfunc_directory_stat(
	NoctEnv *env)
{
	NoctValue argument;
	NoctValue dictionary;
	NoctValue scratch;
	struct noct_dirent entry;
	const char *path;
	const char *name;
	unsigned index;
	int status;
	bool ok = false;

	memset(&argument, 0, sizeof(argument));
	memset(&dictionary, 0, sizeof(dictionary));
	memset(&scratch, 0, sizeof(scratch));

	/* Handles a failed noct pin local operation. */
	if (!noct_pin_local(env, 3, &argument, &dictionary, &scratch))
		return false;

	/* Handles a failed noct get arg check string operation. */
	if (!noct_get_arg_check_string(env, 0, &argument, &path) ||
	    !services_ready() || active.services->directory_read == NULL)
		goto error;

	/* Selects the matching value. */
	if (strcmp(path, "") == 0 || strcmp(path, "/") == 0) {
		memset(&entry, 0, sizeof(entry));
		entry.name[0] = '/';
		entry.name[1] = '\0';
		entry.attributes = 0x10;
	} else {
		name = path[0] == '/' ? path + 1 : path;

		/* Handles a failed strchr operation. */
		if (*name == '\0' || strchr(name, '/') != NULL)
			goto error;

		/* Process each remaining element. */
		for (index = 0; index < ZEDBSD_NOCT_DIRECTORY_MAX; index++) {
			status = active.services->directory_read(
			    active.services->context, "/", index, &entry);

			/* Checks the operation status. */
			if (status <= 0)
				goto error;

			/* Handles the ascii equal folded condition. */
			if (ascii_equal_folded(entry.name, name))
				break;
		}

		/* Checks the current index. */
		if (index == ZEDBSD_NOCT_DIRECTORY_MAX)
			goto error;
	}

	/* Handles a failed make directory entry operation. */
	if (!make_directory_entry(env, &dictionary, &scratch, &entry) ||
	    !noct_set_return(env, &dictionary))
		goto error;
	ok = true;
	goto out;
error:
	noct_error(env, "Directory.stat failed.");
out:
	(void)noct_unpin_local(env, 3, &argument, &dictionary, &scratch);

	/* Returns the computed result. */
	return ok;
}

/* Supports the cfunc system get os name operation. */
static bool
cfunc_system_get_os_name(
	NoctEnv *env)
{
	bool function_result;

	/* Obtains the return string result. */
	function_result = return_string(env, "zedBSD");

	/* Returns the computed result. */
	return function_result;
}

/* Keep the protected-call API available in the freestanding zedBSD System module.  The hosted Noct runtime registers this from api-system.c, which zedBSD intentionally does not link because that module depends on the host operating system.  Remacs uses pcall to recover from command errors. */
static bool
cfunc_system_pcall(
	NoctEnv *env)
{
	NoctValue function_value;
	NoctValue argument_a;
	NoctValue argument_b;
	NoctValue result;
	NoctValue return_value;
	NoctValue scratch;
	NoctValue arguments[2];
	NoctFunc *function;
	const char *message;
	bool call_ok;
	bool ok = false;

	memset(&function_value, 0, sizeof(function_value));
	memset(&argument_a, 0, sizeof(argument_a));
	memset(&argument_b, 0, sizeof(argument_b));
	memset(&result, 0, sizeof(result));
	memset(&return_value, 0, sizeof(return_value));
	memset(&scratch, 0, sizeof(scratch));

	/* Handles a failed noct pin local operation. */
	if (!noct_pin_local(env, 6, &function_value, &argument_a, &argument_b,
			    &result, &return_value, &scratch))

		/* Reports operation failure. */
		return false;

	/* Handles a failed noct get arg check func operation. */
	if (!noct_get_arg_check_func(env, 0, &function_value, &function) ||
	    !noct_get_arg(env, 1, &argument_a) ||
	    !noct_get_arg(env, 2, &argument_b))
		goto out;

	arguments[0] = argument_a;
	arguments[1] = argument_b;
	call_ok = noct_call(env, function, 2, arguments, &return_value);

	/* Handles a failed noct make empty dict operation. */
	if (!noct_make_empty_dict(env, &result) ||
	    !noct_set_dict_elem_make_int(env, &result, "ok", &scratch,
					 call_ok ? 1 : 0))
		goto out;

	/* Handles the call ok condition. */
	if (call_ok) {
		/* Handles a failed noct set dict elem cstr operation. */
		if (!noct_set_dict_elem_cstr(env, &result, "value",
					     &return_value))
			goto out;
	} else {
		noct_get_error_message(env, &message);

		/* Handles a failed noct set dict elem make string operation. */
		if (!noct_set_dict_elem_make_string(
			env, &result, "message", &scratch,
			message != NULL ? message : "?"))
			goto out;
	}
	ok = noct_set_return(env, &result);
out:
	(void)noct_unpin_local(env, 6, &function_value, &argument_a,
			       &argument_b, &result, &return_value, &scratch);

	/* Returns the computed result. */
	return ok;
}

/* Supports the cfunc system get env operation. */
static bool
cfunc_system_get_env(
	NoctEnv *env)
{
	NoctValue argument;
	const char *name;
	const char *value;
	bool ok = false;

	memset(&argument, 0, sizeof(argument));

	/* Handles a failed noct pin local operation. */
	if (!noct_pin_local(env, 1, &argument))
		return false;

	/* Handles a failed noct get arg check string operation. */
	if (!noct_get_arg_check_string(env, 0, &argument, &name) ||
	    active.environment == NULL || !env_name_valid(name))
		goto error;
	value = env_get(active.environment, name);
	ok = return_string(env, value != NULL ? value : "");
	goto out;
error:
	noct_error(env, "System.getEnv received an invalid name.");
out:
	(void)noct_unpin_local(env, 1, &argument);

	/* Returns the computed result. */
	return ok;
}

/* Supports the cfunc system set env operation. */
static bool
cfunc_system_set_env(
	NoctEnv *env)
{
	NoctValue name_value;
	NoctValue string_value;
	const char *name;
	const char *value;
	bool ok = false;

	memset(&name_value, 0, sizeof(name_value));
	memset(&string_value, 0, sizeof(string_value));

	/* Handles a failed noct pin local operation. */
	if (!noct_pin_local(env, 2, &name_value, &string_value))
		return false;

	/* Handles a failed noct get arg check string operation. */
	if (!noct_get_arg_check_string(env, 0, &name_value, &name) ||
	    !noct_get_arg_check_string(env, 1, &string_value, &value) ||
	    active.environment == NULL ||
	    !env_set(active.environment, name, value))
		goto error;
	ok = return_int(env, 0);
	goto out;
error:
	noct_error(env,
		   "System.setEnv rejected the name, value, or full store.");
out:
	(void)noct_unpin_local(env, 2, &name_value, &string_value);

	/* Returns the computed result. */
	return ok;
}

/* Supports the cfunc system unset env operation. */
static bool
cfunc_system_unset_env(
	NoctEnv *env)
{
	NoctValue argument;
	const char *name;
	bool ok = false;

	memset(&argument, 0, sizeof(argument));

	/* Handles a failed noct pin local operation. */
	if (!noct_pin_local(env, 1, &argument))
		return false;

	/* Handles a failed noct get arg check string operation. */
	if (!noct_get_arg_check_string(env, 0, &argument, &name) ||
	    active.environment == NULL || !env_name_valid(name))
		goto error;
	(void)env_unset(active.environment, name);
	ok = return_int(env, 0);
	goto out;
error:
	noct_error(env, "System.unsetEnv received an invalid name.");
out:
	(void)noct_unpin_local(env, 1, &argument);

	/* Returns the computed result. */
	return ok;
}

/* Supports the cfunc system list env operation. */
static bool
cfunc_system_list_env(
	NoctEnv *env)
{
	const char *name;
	const char *value;

	NoctValue dictionary;
	NoctValue scratch;
	size_t index;
	bool ok = false;

	memset(&dictionary, 0, sizeof(dictionary));
	memset(&scratch, 0, sizeof(scratch));

	/* Handles a failed noct pin local operation. */
	if (active.environment == NULL ||
	    !noct_pin_local(env, 2, &dictionary, &scratch))

		/* Reports operation failure. */
		return false;

	/* Handles a failed noct make empty dict operation. */
	if (!noct_make_empty_dict(env, &dictionary))
		goto out;

	/* Process each remaining element. */
	for (index = 0; index < env_count(active.environment); index++) {
		/* Handles a failed env at operation. */
		if (!env_at(active.environment, index, &name, &value) ||
		    !noct_set_dict_elem_make_string(env, &dictionary, name,
						    &scratch, value))
			goto out;
	}
	ok = noct_set_return(env, &dictionary);
out:
	(void)noct_unpin_local(env, 2, &dictionary, &scratch);

	/* Returns the computed result. */
	return ok;
}

/* Supports the cfunc system memory usage operation. */
static bool
cfunc_system_memory_usage(
	NoctEnv *env)
{
	NoctValue dictionary;
	NoctValue scratch;
	bool ok = false;

	memset(&dictionary, 0, sizeof(dictionary));
	memset(&scratch, 0, sizeof(scratch));

	/* Handles a failed noct pin local operation. */
	if (!noct_pin_local(env, 2, &dictionary, &scratch))
		return false;

	/* Handles the noct make empty dict condition. */
	if (noct_make_empty_dict(env, &dictionary) &&
	    noct_set_dict_elem_make_long(env, &dictionary, "current", &scratch,
					 (int64_t)heap_active_current()) &&
	    noct_set_dict_elem_make_long(env, &dictionary, "peak", &scratch,
					 (int64_t)heap_active_peak()) &&
	    noct_set_dict_elem_make_long(env, &dictionary, "arenaSize",
					 &scratch,
					 (int64_t)active.arena_size) &&
	    noct_set_return(env, &dictionary))
		ok = true;
	(void)noct_unpin_local(env, 2, &dictionary, &scratch);

	/* Returns the computed result. */
	return ok;
}

/* Supports the cfunc system import operation. */
static bool
cfunc_system_import(
	NoctEnv *env)
{
	NoctValue argument;
	struct imported_source *source = NULL;
	const char *path;
	size_t path_length;
	size_t allocation;
	uint32_t size;
	bool ok = false;

	memset(&argument, 0, sizeof(argument));

	/* Handles a failed noct pin local operation. */
	if (!noct_pin_local(env, 1, &argument))
		return false;

	/* Handles a failed noct get arg check string operation. */
	if (!noct_get_arg_check_string(env, 0, &argument, &path) ||
	    !services_ready() || active.services->file_size == NULL ||
	    active.services->file_read == NULL ||
	    !active.services->file_size(active.services->context, path,
					&size) ||
	    size > active.source_max)
		goto error;
	path_length = strlen(path);

	/* Handles the path length condition. */
	if (path_length >= ZEDBSD_NOCT_PATH_MAX ||
	    path_length > SIZE_MAX - sizeof(*source) - (size_t)size - 2U)
		goto error;
	allocation = sizeof(*source) + path_length + 1U + (size_t)size + 1U;
	source = malloc(allocation);

	/* Handles the source availability. */
	if (source == NULL)
		goto error;
	source->path = (char *)(source + 1);
	source->source = source->path + path_length + 1U;
	memcpy(source->path, path, path_length + 1U);

	/* Handles a failed file read operation. */
	if ((size != 0 &&
	     !active.services->file_read(active.services->context, path, 0,
					 source->source, size)) ||
	    (source->source[size] = '\0',
	     !noct_register_source(env, source->path, source->source)))
		goto error;
	source->next = active.imports;
	active.imports = source;
	source = NULL;
	ok = return_int(env, 0);
	goto out;
error:
	free(source);
	noct_error(env, "System.import failed.");
out:
	(void)noct_unpin_local(env, 1, &argument);

	/* Returns the computed result. */
	return ok;
}
