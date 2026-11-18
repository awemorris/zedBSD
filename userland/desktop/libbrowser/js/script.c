/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the headless script shell (browser --js) and the test
 * runners give a script besides the language: a print function that
 * writes a line to standard output, and the text of an uncaught exception
 * for the report.  A page's console arrives with the DOM binding
 * (ws074-p030).
 */

#include "js/js.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int script_print(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);

/*
 * Defines print on a realm's global object: its arguments as strings,
 * separated by spaces, as one line on standard output.
 */
int
js_define_print(
	struct vm_realm *realm)
{
	struct vm_function *function;
	vm_value key;
	int error;

	/* The native function. */
	function = vm_function_create_native(realm, "print", 1, script_print);
	if (function == NULL)
		return ENOMEM;

	/* The global property: writable and configurable, not enumerable (like the built-ins). */
	key = vm_key_from_ascii(realm->heap, "print");
	if (key == VM_VALUE_EMPTY)
		return ENOMEM;
	error = vm_object_define(realm->heap, realm->global, key, vm_value_cell(function),
	    VM_PROPERTY_WRITABLE | VM_PROPERTY_CONFIGURABLE);
	if (error != 0)
		return error;

	/* Succeeded: print is defined. */
	return 0;
}

/*
 * Writes an exception's text as UTF-8: its string, or a placeholder when
 * even that throws.
 */
int
js_exception_text(
	struct vm_realm *realm,
	vm_value exception,
	struct wb_buffer *out)
{
	struct vm_string *string;
	int status;

	/* The exception's string (a second exception from the conversion is dropped). */
	status = vm_to_string(realm, exception, &string);
	if (status == VM_THROWN) {
		realm->exception = VM_VALUE_UNDEFINED;
		status = wb_buffer_append_string(out, "(an exception that cannot be converted to a string)");
		if (status != 0)
			return status;
		return 0;
	}

	/* Any other failure of the conversion. */
	if (status != 0)
		return status;

	/* Its characters. */
	status = vm_string_to_utf8(string, out);
	if (status != 0)
		return status;

	/* Succeeded: the text is written. */
	return 0;
}

/* Writes the arguments as one line on standard output (print). */
static int
script_print(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct wb_buffer line;
	struct vm_string *string;
	unsigned index;
	int status;

	UNUSED_PARAMETER(this_value);

	/* Each argument's string, a space between them. */
	*result = VM_VALUE_UNDEFINED;
	wb_buffer_init(&line);
	for (index = 0; index < count; index++) {
		status = vm_to_string(realm, args[index], &string);
		if (status == 0 && index > 0)
			status = wb_buffer_append_string(&line, " ");
		if (status == 0)
			status = vm_string_to_utf8(string, &line);
		if (status != 0) {
			wb_buffer_release(&line);
			return status;
		}
	}

	/* The line. */
	status = wb_buffer_append_string(&line, "\n");
	if (status != 0) {
		wb_buffer_release(&line);
		return status;
	}

	/* The line on standard output. */
	fwrite(wb_buffer_string(&line), 1, line.length, stdout);
	fflush(stdout);
	wb_buffer_release(&line);

	/* Succeeded: print returns undefined. */
	return 0;
}
