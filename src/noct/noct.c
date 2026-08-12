/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * zedBSD Noct lifecycle
 * This will be removed after Noct is moved to userspace.
 */

#include "kern/noct.h"
#include "noct/memory.h"
#include "noct/napi.h"
#include "noct/target.h"
#include "libc/heap.h"
#include "libc/stdio-fs.h"

#include <noct/noct.h>
#include <noct/repl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

struct active_console {
	zedbsd_noct_write_fn write;
	void *context;
};

struct jit_observation {
	void *region;
	size_t size;
	size_t expected_size;
	int released;
};

static struct active_console active_console;
static int lifecycle_active;

static void
observe_heap(void *context, void *pointer, size_t size,
	     enum zedbsd_heap_event event)
{
	struct jit_observation *jit = context;

	if (event == ZEDBSD_HEAP_ALLOCATED &&
	    size == jit->expected_size && jit->region == NULL) {
		jit->region = pointer;
		jit->size = size;
	} else if (event == ZEDBSD_HEAP_FREED && pointer == jit->region) {
		jit->released = 1;
	}
}

static void
emit_bytes(const char *bytes, size_t length)
{
	if (active_console.write != NULL && length != 0)
		(void)active_console.write(active_console.context, bytes, length);
}

static void
emit_string(const char *string)
{
	if (string != NULL)
		emit_bytes(string, strlen(string));
}

static void
emit_noct_error(NoctEnv *env, const char *kind)
{
	const char *file = "<unknown>";
	const char *message = "Noct error";
	char line_buffer[24];
	int line = 0;

	if (env != NULL) {
		(void)noct_get_error_file(env, &file);
		(void)noct_get_error_line(env, &line);
		(void)noct_get_error_message(env, &message);
	}
	emit_string(kind);
	emit_string(": ");
	emit_string(file != NULL ? file : "<unknown>");
	(void)snprintf(line_buffer, sizeof(line_buffer), ":%d: ", line);
	emit_string(line_buffer);
	emit_string(message != NULL ? message : "Noct error");
	emit_string("\n");
}

static int
run_program_args(const char *program_name, void *program, uint32_t program_size,
		 int bytecode, int argc, char *const argv[],
		 const struct zedbsd_noct_options *options,
		 struct zedbsd_noct_result *result)
{
	NoctConfig config;
	NoctVM *vm = NULL;
	NoctEnv *env = NULL;
	NoctValue return_value;
	NoctValue main_value;
	NoctValue arguments;
	NoctValue argument_value;
	NoctFunc *main_function = NULL;
	struct jit_observation jit;
	struct zedbsd_heap noct_heap;
	struct zedbsd_heap *previous_heap = NULL;
	enum zedbsd_noct_status status = ZEDBSD_NOCT_INVALID_ARGUMENT;
	int vm_created = 0;
	int arguments_pinned = 0;
	size_t parameter_count = 0;
	int value_type = NOCT_VALUE_INT;
	int64_t script_status = 0;
	size_t peak = 0;
	size_t before_reset = 0;
	size_t after_reset = 0;
	size_t errors = 0;

	memset(&return_value, 0, sizeof(return_value));
	memset(&main_value, 0, sizeof(main_value));
	memset(&arguments, 0, sizeof(arguments));
	memset(&argument_value, 0, sizeof(argument_value));
	memset(&jit, 0, sizeof(jit));
	jit.expected_size = options != NULL && options->memory != NULL ?
		options->memory->jit_code_size : ZEDBSD_NOCT_JIT_CODE_MAX;
	if (result != NULL)
		memset(result, 0, sizeof(*result));
	if (program_name == NULL || program == NULL ||
	    (bytecode && program_size == 0) || argc < 0 ||
	    argc > NOCT_ARG_MAX || (argc != 0 && argv == NULL) ||
	    options == NULL ||
	    options->arena == NULL || options->arena_size < 4096U ||
	    options->write == NULL || options->jit_threshold < 0)
		goto finish_without_heap;
	if (lifecycle_active) {
		status = ZEDBSD_NOCT_BUSY;
		goto finish_without_heap;
	}

	lifecycle_active = 1;
	active_console.write = options->write;
	active_console.context = options->write_context;
	zedbsd_heap_init_instance(&noct_heap, options->arena, options->arena_size);
	previous_heap = zedbsd_heap_set_active(&noct_heap);
	zedbsd_stdio_set_filesystem(options->filesystem);
	zedbsd_stdio_set_environment(options->environment);
	zedbsd_heap_set_observer(observe_heap, &jit);
	if (options->fail_after != ZEDBSD_NOCT_NO_FAILURE)
		zedbsd_heap_set_failure_after(options->fail_after);

	noct_set_default_config(&config);
	if (options->memory != NULL) {
		config.jit_code_size = options->memory->jit_code_size;
		config.gc_nursery_size = options->memory->gc_nursery_size;
		config.gc_graduate_size = options->memory->gc_graduate_size;
		config.gc_tenure_size = options->memory->gc_tenure_size;
	}
	config.jit_enable = options->jit_enable != 0;
	config.optimize_level = ZEDBSD_NOCT_OPTIMIZE_LEVEL;
	if (!noct_create_vm(&vm, &env, &config)) {
		status = ZEDBSD_NOCT_VM_ERROR;
		emit_string("Noct: unable to create VM\n");
		goto cleanup;
	}
	vm_created = 1;
	if (!zedbsd_noct_napi_register(env, options)) {
		status = ZEDBSD_NOCT_API_ERROR;
		emit_noct_error(env, "Noct API error");
		goto cleanup;
	}
	if (!zedbsd_noct_target_register(env, options->services)) {
		status = ZEDBSD_NOCT_API_ERROR;
		emit_noct_error(env, "Noct target API error");
		goto cleanup;
	}
	if (!(bytecode ? noct_register_bytecode(env, program, program_size) :
	      noct_register_source(env, program_name, program))) {
		status = ZEDBSD_NOCT_SOURCE_ERROR;
		emit_noct_error(env, bytecode ? "Noct bytecode error" :
					      "Noct source error");
		goto cleanup;
	}
	if (!noct_get_global(env, "main", &main_value) ||
	    !noct_get_func(env, &main_value, &main_function) ||
	    !noct_get_func_param_count(env, main_function, &parameter_count)) {
		status = ZEDBSD_NOCT_SIGNATURE_ERROR;
		emit_noct_error(env, "Noct main error");
		goto cleanup;
	}
	if (parameter_count > 1U) {
		status = ZEDBSD_NOCT_SIGNATURE_ERROR;
		emit_string("Noct main error: main must accept zero or one argument\n");
		goto cleanup;
	}
	if (parameter_count == 1U) {
		int index;

		if (!noct_pin_local(env, 2, &arguments, &argument_value)) {
			status = ZEDBSD_NOCT_API_ERROR;
			emit_noct_error(env, "Noct API error");
			goto cleanup;
		}
		arguments_pinned = 1;
		if (!noct_make_empty_array(env, &arguments)) {
			status = ZEDBSD_NOCT_API_ERROR;
			emit_noct_error(env, "Noct API error");
			goto cleanup;
		}
		for (index = 0; index < argc; index++) {
			if (argv[index] == NULL ||
			    !noct_set_array_elem_make_string(env, &arguments,
							  (size_t)index,
							  &argument_value,
							  argv[index])) {
				status = ZEDBSD_NOCT_API_ERROR;
				emit_noct_error(env, "Noct API error");
				goto cleanup;
			}
		}
	}
	if (!noct_enter_vm(env, "main", parameter_count == 0U ? 0U : 1U,
			   parameter_count == 0U ? NULL : &arguments,
			   &return_value)) {
		status = ZEDBSD_NOCT_RUNTIME_ERROR;
		emit_noct_error(env, "Noct runtime error");
		goto cleanup;
	}
	if (noct_get_value_type(env, &return_value, &value_type)) {
		if (value_type == NOCT_VALUE_INT) {
			int value;

			if (noct_get_int(env, &return_value, &value))
				script_status = value;
		} else if (value_type == NOCT_VALUE_LONG) {
			(void)noct_get_long(env, &return_value, &script_status);
		}
	}
	status = ZEDBSD_NOCT_OK;

cleanup:
	if (arguments_pinned)
		(void)noct_unpin_local(env, 2, &arguments, &argument_value);
	if (jit.region != NULL && options->observe_jit_code != NULL)
		options->observe_jit_code(options->jit_context, jit.region,
					  jit.size);
	if (vm_created && !noct_destroy_vm(vm) && status == ZEDBSD_NOCT_OK)
		status = ZEDBSD_NOCT_CLEANUP_ERROR;
	if (zedbsd_stdio_close_all() != 0 && status == ZEDBSD_NOCT_OK)
		status = ZEDBSD_NOCT_CLEANUP_ERROR;
	zedbsd_stdio_set_filesystem(NULL);
	zedbsd_stdio_set_environment(NULL);
	zedbsd_noct_target_cleanup();
	zedbsd_noct_napi_cleanup();
	peak = zedbsd_heap_peak();
	before_reset = zedbsd_heap_current();
	errors = zedbsd_heap_error_count();
	zedbsd_heap_set_observer(NULL, NULL);
	zedbsd_heap_reset();
	after_reset = zedbsd_heap_current();
	(void)zedbsd_heap_set_active(previous_heap);
	if ((after_reset != 0 || errors != 0) && status == ZEDBSD_NOCT_OK)
		status = ZEDBSD_NOCT_CLEANUP_ERROR;
	active_console.write = NULL;
	active_console.context = NULL;
	lifecycle_active = 0;

finish_without_heap:
	if (result != NULL) {
		result->status = status;
		result->heap_peak = peak;
		result->bytes_before_reset = before_reset;
		result->current_after_reset = after_reset;
		result->heap_errors = errors;
		result->jit_code_size = jit.size;
		result->jit_region_released = jit.released;
		result->script_status = script_status;
	}
	return status == ZEDBSD_NOCT_OK;
}

int
zedbsd_noct_run_args(const char *source_name, const char *source,
		    int argc, char *const argv[],
		    const struct zedbsd_noct_options *options,
		    struct zedbsd_noct_result *result)
{
	return run_program_args(source_name, (void *)source, 0, 0, argc, argv,
				options, result);
}

int
zedbsd_noct_run_bytecode_args(const char *program_name, uint8_t *bytecode,
			      uint32_t bytecode_size, int argc,
			      char *const argv[],
			      const struct zedbsd_noct_options *options,
			      struct zedbsd_noct_result *result)
{
	return run_program_args(program_name, bytecode, bytecode_size, 1, argc,
				argv, options, result);
}

int
zedbsd_noct_run(const char *source_name, const char *source,
		const struct zedbsd_noct_options *options,
		struct zedbsd_noct_result *result)
{
	return zedbsd_noct_run_args(source_name, source, 0, NULL, options,
				    result);
}

int
zedbsd_noct_repl(const struct zedbsd_noct_options *options,
		 zedbsd_noct_repl_read_fn read_line, void *read_context,
		 struct zedbsd_noct_result *result)
{
	NoctConfig config;
	NoctVM *vm = NULL;
	NoctEnv *env = NULL;
	NoctReplSession *session = NULL;
	struct jit_observation jit;
	struct zedbsd_heap noct_heap;
	struct zedbsd_heap *previous_heap = NULL;
	enum zedbsd_noct_status status = ZEDBSD_NOCT_INVALID_ARGUMENT;
	char line[ZEDBSD_NOCT_REPL_LINE_MAX];
	int vm_created = 0;
	int continuation = 0;
	size_t peak = 0;
	size_t before_reset = 0;
	size_t after_reset = 0;
	size_t errors = 0;

	memset(&jit, 0, sizeof(jit));
	jit.expected_size = options != NULL && options->memory != NULL ?
		options->memory->jit_code_size : ZEDBSD_NOCT_JIT_CODE_MAX;
	if (result != NULL)
		memset(result, 0, sizeof(*result));
	if (options == NULL || options->arena == NULL ||
	    options->arena_size < 4096U || options->write == NULL ||
	    options->jit_threshold < 0 || read_line == NULL)
		goto finish_without_heap;
	if (lifecycle_active) {
		status = ZEDBSD_NOCT_BUSY;
		goto finish_without_heap;
	}

	lifecycle_active = 1;
	active_console.write = options->write;
	active_console.context = options->write_context;
	zedbsd_heap_init_instance(&noct_heap, options->arena, options->arena_size);
	previous_heap = zedbsd_heap_set_active(&noct_heap);
	zedbsd_stdio_set_filesystem(options->filesystem);
	zedbsd_stdio_set_environment(options->environment);
	zedbsd_heap_set_observer(observe_heap, &jit);
	if (options->fail_after != ZEDBSD_NOCT_NO_FAILURE)
		zedbsd_heap_set_failure_after(options->fail_after);

	noct_set_default_config(&config);
	if (options->memory != NULL) {
		config.jit_code_size = options->memory->jit_code_size;
		config.gc_nursery_size = options->memory->gc_nursery_size;
		config.gc_graduate_size = options->memory->gc_graduate_size;
		config.gc_tenure_size = options->memory->gc_tenure_size;
	}
	config.jit_enable = options->jit_enable != 0;
	if (!noct_create_vm(&vm, &env, &config)) {
		status = ZEDBSD_NOCT_VM_ERROR;
		emit_string("Noct: unable to create VM\n");
		goto cleanup;
	}
	vm_created = 1;
	if (!zedbsd_noct_napi_register(env, options)) {
		status = ZEDBSD_NOCT_API_ERROR;
		emit_noct_error(env, "Noct API error");
		goto cleanup;
	}
	if (!zedbsd_noct_target_register(env, options->services)) {
		status = ZEDBSD_NOCT_API_ERROR;
		emit_noct_error(env, "Noct target API error");
		goto cleanup;
	}
	session = noct_repl_create(env, options->memory != NULL ?
		options->memory->repl_source_max : ZEDBSD_NOCT_REPL_SOURCE_MAX);
	if (session == NULL) {
		status = ZEDBSD_NOCT_VM_ERROR;
		emit_noct_error(env, "Noct REPL error");
		goto cleanup;
	}

	for (;;) {
		enum zedbsd_noct_repl_input_result input_result;
		enum NoctReplResult repl_result;

		line[0] = '\0';
		input_result = read_line(read_context, continuation, line,
					 sizeof(line));
		line[sizeof(line) - 1U] = '\0';
		if (input_result == ZEDBSD_NOCT_REPL_INPUT_EXIT) {
			(void)noct_repl_submit(session, NULL);
			status = ZEDBSD_NOCT_OK;
			break;
		}
		if (input_result != ZEDBSD_NOCT_REPL_INPUT_LINE) {
			status = ZEDBSD_NOCT_INPUT_ERROR;
			break;
		}

		repl_result = noct_repl_submit(session, line);
		switch (repl_result) {
		case NOCT_REPL_READY:
		case NOCT_REPL_EXECUTED:
			continuation = 0;
			break;
		case NOCT_REPL_NEED_MORE:
			continuation = 1;
			break;
		case NOCT_REPL_ERROR:
			emit_noct_error(env, "Noct REPL error");
			continuation = 0;
			break;
		case NOCT_REPL_EXIT:
			status = ZEDBSD_NOCT_OK;
			goto cleanup;
		default:
			status = ZEDBSD_NOCT_VM_ERROR;
			goto cleanup;
		}
	}

cleanup:
	noct_repl_destroy(session);
	if (jit.region != NULL && options->observe_jit_code != NULL)
		options->observe_jit_code(options->jit_context, jit.region,
					  jit.size);
	if (vm_created && !noct_destroy_vm(vm) && status == ZEDBSD_NOCT_OK)
		status = ZEDBSD_NOCT_CLEANUP_ERROR;
	if (zedbsd_stdio_close_all() != 0 && status == ZEDBSD_NOCT_OK)
		status = ZEDBSD_NOCT_CLEANUP_ERROR;
	zedbsd_stdio_set_filesystem(NULL);
	zedbsd_stdio_set_environment(NULL);
	zedbsd_noct_target_cleanup();
	zedbsd_noct_napi_cleanup();
	peak = zedbsd_heap_peak();
	before_reset = zedbsd_heap_current();
	errors = zedbsd_heap_error_count();
	zedbsd_heap_set_observer(NULL, NULL);
	zedbsd_heap_reset();
	after_reset = zedbsd_heap_current();
	(void)zedbsd_heap_set_active(previous_heap);
	if ((after_reset != 0 || errors != 0) && status == ZEDBSD_NOCT_OK)
		status = ZEDBSD_NOCT_CLEANUP_ERROR;
	active_console.write = NULL;
	active_console.context = NULL;
	lifecycle_active = 0;

finish_without_heap:
	if (result != NULL) {
		result->status = status;
		result->heap_peak = peak;
		result->bytes_before_reset = before_reset;
		result->current_after_reset = after_reset;
		result->heap_errors = errors;
		result->jit_code_size = jit.size;
		result->jit_region_released = jit.released;
		result->script_status = 0;
	}
	return status == ZEDBSD_NOCT_OK;
}

const char *
zedbsd_noct_status_string(enum zedbsd_noct_status status)
{
	switch (status) {
	case ZEDBSD_NOCT_OK:
		return "ok";
	case ZEDBSD_NOCT_INVALID_ARGUMENT:
		return "invalid argument";
	case ZEDBSD_NOCT_BUSY:
		return "lifecycle busy";
	case ZEDBSD_NOCT_VM_ERROR:
		return "VM creation failed";
	case ZEDBSD_NOCT_API_ERROR:
		return "API registration failed";
	case ZEDBSD_NOCT_SOURCE_ERROR:
		return "source error";
	case ZEDBSD_NOCT_SIGNATURE_ERROR:
		return "invalid main signature";
	case ZEDBSD_NOCT_RUNTIME_ERROR:
		return "runtime error";
	case ZEDBSD_NOCT_INPUT_ERROR:
		return "input error";
	case ZEDBSD_NOCT_CLEANUP_ERROR:
		return "cleanup error";
	default:
		return "unknown";
	}
}
