/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The script tools of the test suites (<browser/browser.h>, browser_script_tool;
 * moved out of /bin/browser's main.c when the engine became libbrowser,
 * ws074-p057): a JavaScript file run in a realm of its own with print (the
 * test262 runner's --js), its syntax tree (--dump=ast), or its compiled
 * code (--dump=code), written to standard output.  What goes wrong is
 * written to standard error in the forms the runners read.
 */

#include "js/js.h"
#include "vm/bytecode.h"
#include "base/os-error.h"

#include <browser/browser.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The most live bytes a script run by the tool may keep in its heap. */
#define TOOL_HEAP_LIMIT		((size_t)1024U * 1024U * 1024U)

static int tool_dump_ast(const char *path, unsigned how);
static int tool_run(const char *path, unsigned how, int dump_code, const void *stack_base);
static int tool_report(struct vm_realm *realm, const char *path, int status, const struct js_syntax_error *error);
static void tool_report_job(struct vm_realm *realm, vm_value exception, void *context);
static int tool_dump_code(struct vm_realm *realm, const struct wb_units *units, unsigned how, struct js_syntax_error *error);
static int tool_dump_unit(const struct vm_code *code, struct wb_buffer *out);
static int tool_read(const char *path, struct wb_units *units);

/*
 * Runs a JavaScript file, or writes its syntax tree or its code, as the
 * program's script modes do.
 *
 * Returns the program's exit status: 0, 1 for a script that failed or
 * could not be read, 2 for a misuse.
 */
int
browser_script_tool(
	enum browser_script_tool tool,
	const char *path,
	unsigned flags,
	const void *stack_base)
{
	unsigned how;
	int status;

	/* The parser's flags. */
	how = 0;
	if ((flags & BROWSER_SCRIPT_STRICT) != 0U)
		how |= JS_PARSE_STRICT;
	if ((flags & BROWSER_SCRIPT_MODULE) != 0U)
		how |= JS_PARSE_MODULE;

	/* The syntax tree. */
	if (tool == BROWSER_SCRIPT_DUMP_AST) {
		status = tool_dump_ast(path, how);
		return status;
	}

	/* A run, or the dump of the code. */
	if (tool == BROWSER_SCRIPT_DUMP_CODE)
		status = tool_run(path, how, 1, stack_base);
	else
		status = tool_run(path, how, 0, stack_base);

	/* The exit status of the run. */
	return status;
}

/* Parses a script and writes its syntax tree, or its syntax error (exit status 1). */
static int
tool_dump_ast(
	const char *path,
	unsigned how)
{
	struct wb_buffer out;
	struct wb_units units;
	struct js_program program;
	struct js_syntax_error error;
	int status;

	/* A dump needs a file. */
	if (path == NULL) {
		fprintf(stderr, "browser: a file to parse is needed\n");
		return 2;
	}

	/* The file, as UTF-16. */
	status = tool_read(path, &units);
	if (status != 0)
		return 1;

	/* The parse; a syntax error is the dump's answer. */
	status = js_parse(units.data, units.length, how, &program, &error);
	if (status == EINVAL) {
		printf("SyntaxError: %s:%u:%u: %s\n", path, error.line, error.column, error.message);
		wb_units_release(&units);
		return 1;
	}

	/* Any other failure of the parse. */
	if (status != 0) {
		fprintf(stderr, "browser: cannot parse %s: %s\n", path, wb_os_error_text(status));
		wb_units_release(&units);
		return 1;
	}

	/* The tree as text, written when it could be made. */
	wb_buffer_init(&out);
	status = js_dump(program.root, &out);
	if (status == 0)
		fwrite(wb_buffer_string(&out), 1, out.length, stdout);

	/* The text, the tree and the source are no longer needed. */
	wb_buffer_release(&out);
	js_program_release(&program);
	wb_units_release(&units);
	if (status != 0)
		return 1;

	/* Succeeded: the tree is written. */
	return 0;
}

/*
 * Runs a script in a realm of its own with print, or writes its code;
 * reports a syntax error, what is not supported, or an uncaught exception
 * on standard error with exit status 1.
 */
static int
tool_run(
	const char *path,
	unsigned how,
	int dump_code,
	const void *stack_base)
{
	struct wb_units units;
	struct js_syntax_error error;
	struct vm_heap *heap;
	struct vm_realm *realm;
	vm_value completion;
	int exit_status;
	int status;

	/* A run needs a file. */
	if (path == NULL) {
		fprintf(stderr, "browser: a script to run is needed\n");
		return 2;
	}

	/* The file, as UTF-16. */
	status = tool_read(path, &units);
	if (status != 0)
		return 1;

	/* The heap. */
	status = vm_heap_create(&heap, TOOL_HEAP_LIMIT);
	if (status != 0) {
		fprintf(stderr, "browser: cannot make a heap: %s\n", wb_os_error_text(status));
		wb_units_release(&units);
		return 1;
	}

	/* The stack the collector scans ends at the caller's frame. */
	vm_heap_set_stack_base(heap, stack_base);

	/* The realm. */
	status = vm_realm_create(heap, &realm);
	if (status != 0) {
		fprintf(stderr, "browser: cannot make a realm: %s\n", wb_os_error_text(status));
		vm_heap_destroy(heap);
		wb_units_release(&units);
		return 1;
	}

	/* The language's built-in objects, and print. */
	status = js_install_builtins(realm);
	if (status == 0)
		status = js_define_print(realm);
	if (status != 0) {
		fprintf(stderr, "browser: cannot make a realm: %s\n", wb_os_error_text(status));
		vm_realm_destroy(realm);
		vm_heap_destroy(heap);
		wb_units_release(&units);
		return 1;
	}

	/* The run, or the dump of the code. */
	if (dump_code)
		status = tool_dump_code(realm, &units, how, &error);
	else
		status = js_run_script(realm, units.data, units.length, how, &completion, &error);

	/* The microtasks the script queued (its promises' reactions and awaits, ws074-p086). */
	if (!dump_code && status == 0)
		status = vm_run_jobs(realm, tool_report_job, NULL);

	/* The source is no longer needed; why the script did not run to its end is said. */
	wb_units_release(&units);
	exit_status = tool_report(realm, path, status, &error);

	/* The realm and its heap are no longer needed. */
	vm_realm_destroy(realm);
	vm_heap_destroy(heap);
	if (exit_status != 0)
		return exit_status;

	/* Succeeded: the script ran to its end. */
	return 0;
}

/*
 * Says on standard error why a script did not run to its end: what the
 * engine does not support yet, a syntax error, an uncaught exception, or
 * another failure.  Returns the exit status (0 when it ran).
 */
static int
tool_report(
	struct vm_realm *realm,
	const char *path,
	int status,
	const struct js_syntax_error *error)
{
	struct wb_buffer text;
	int described;

	/* A script that ran to its end. */
	if (status == 0)
		return 0;

	/* A syntax the engine does not support yet, or a syntax error. */
	if (status == EINVAL) {
		if (error->unsupported)
			fprintf(stderr, "browser: %s:%u:%u: %s\n", path, error->line, error->column, error->message);
		else
			fprintf(stderr, "SyntaxError: %s:%u:%u: %s\n", path, error->line, error->column, error->message);
		return 1;
	}

	/* An uncaught exception, described when it can be. */
	if (status == VM_THROWN) {
		wb_buffer_init(&text);
		described = js_exception_text(realm, realm->exception, &text);
		if (described == 0)
			fprintf(stderr, "Uncaught %s\n", wb_buffer_string(&text));
		wb_buffer_release(&text);
		return 1;
	}

	/* Any other failure. */
	fprintf(stderr, "browser: cannot run %s: %s\n", path, wb_os_error_text(status));
	return 1;
}

/* Says on standard error what a microtask threw, or a promise rejected with while nothing handled it. */
static void
tool_report_job(
	struct vm_realm *realm,
	vm_value exception,
	void *context)
{
	struct wb_buffer text;
	int described;

	UNUSED_PARAMETER(context);

	/* The exception's text, as a browser's console writes an uncaught one. */
	wb_buffer_init(&text);
	described = js_exception_text(realm, exception, &text);
	if (described == 0 && realm->reporting_rejection)
		fprintf(stderr, "Uncaught (in promise) %s\n", wb_buffer_string(&text));
	else if (described == 0)
		fprintf(stderr, "Uncaught %s\n", wb_buffer_string(&text));
	wb_buffer_release(&text);
}

/* Compiles a script and writes its code units (the program's, then each function's inside it). */
static int
tool_dump_code(
	struct vm_realm *realm,
	const struct wb_units *units,
	unsigned how,
	struct js_syntax_error *error)
{
	struct js_program program;
	struct vm_function *function;
	struct wb_buffer out;
	int status;

	/* The tree. */
	status = js_parse(units->data, units->length, how, &program, error);
	if (status != 0)
		return status;

	/* The code, after which the tree is no longer needed. */
	status = js_compile(realm, &program, &function, error);
	js_program_release(&program);
	if (status != 0)
		return status;

	/* The units as text, written when they could be made. */
	wb_buffer_init(&out);
	status = tool_dump_unit(function->code, &out);
	if (status == 0)
		fwrite(wb_buffer_string(&out), 1, out.length, stdout);
	wb_buffer_release(&out);
	if (status != 0)
		return status;

	/* Succeeded: the code is written. */
	return 0;
}

/* Writes a code unit, then the code units among its constants. */
static int
tool_dump_unit(
	const struct vm_code *code,
	struct wb_buffer *out)
{
	struct vm_cell *cell;
	uint32_t index;
	int is_cell;
	int status;

	/* The unit's heading. */
	status = wb_buffer_append_string(out, "\n== ");
	if (status != 0)
		return status;

	/* Its name. */
	status = vm_string_to_utf8(code->name, out);
	if (status != 0)
		return status;

	/* The end of the heading. */
	status = wb_buffer_append_string(out, "\n");
	if (status != 0)
		return status;

	/* Its instructions. */
	status = vm_code_dump(code, out);
	if (status != 0)
		return status;

	/* The functions made inside it. */
	for (index = 0; index < code->constant_count; index++) {
		/* A constant that is not a code unit is passed by. */
		is_cell = vm_value_is_cell(code->constants[index]);
		if (!is_cell)
			continue;
		cell = vm_value_as_cell(code->constants[index]);
		if (cell->type != &vm_code_type)
			continue;

		/* The unit, and those inside it. */
		status = tool_dump_unit((const struct vm_code *)cell, out);
		if (status != 0)
			return status;
	}

	/* Succeeded: the units are written. */
	return 0;
}

/* Reads a script file as UTF-16; says why on standard error when it cannot. */
static int
tool_read(
	const char *path,
	struct wb_units *units)
{
	struct wb_buffer bytes;
	int status;

	/* The bytes. */
	wb_buffer_init(&bytes);
	wb_units_init(units);
	status = wb_file_read(path, &bytes);

	/* Their UTF-16. */
	if (status == 0)
		status = wb_utf8_to_units((const unsigned char *)bytes.data, bytes.length, units);
	wb_buffer_release(&bytes);
	if (status != 0) {
		fprintf(stderr, "browser: cannot read %s: %s\n", path, wb_os_error_text(status));
		wb_units_release(units);
		return status;
	}

	/* Succeeded: the script's characters. */
	return 0;
}
