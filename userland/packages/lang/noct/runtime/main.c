/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD runtime userland command.
 */

#include "userland/packages/lang/noct/runtime/zedbsd-api.h"
#include "userland/packages/lang/noct/runtime/memory.h"
#include "libc/heap.h"

#include <noct/noct.h>
#include <noct/repl.h>
#include <zedbsd/system.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static char *read_source(const char *path, size_t *size_out);
static void import_environment(struct environment *environment, char **envp);
static int select_memory_profile(struct user_noct_memory_profile *profile);
static void print_error(NoctEnv *env, const char *kind);
static int run_repl(NoctEnv *env);
static int read_repl_line(char *line, size_t capacity, int continuation);
static int bytecode_path(const char *path);
static void return_error(NoctEnv *env);
static size_t noct_write(void *context, const char *bytes, size_t length);

/*
 * Runs the runtime command.
 */
int
main(
	int argc,
	char **argv,
	char **envp)
{
	int i;

	NoctConfig config;
	NoctVM *vm = NULL;
	NoctEnv *env = NULL;
	NoctValue main_value, return_value, arguments, argument_value;
	NoctFunc *function = NULL;
	struct noct_options options;
	struct environment environment;
	struct user_noct_memory_profile memory;
	struct heap_allocator noct_heap;
	struct heap_allocator *bootstrap_heap = NULL;
	const char *path = NULL;
	char *source = NULL;
	void *arena = MAP_FAILED;
	size_t source_size = 0;
	size_t parameter_count = 0;
	int heap_active = 0, pinned = 0, status = 1, type;

	/* Handles the selected command-line operation. */
	if (argc >= 2 && strcmp(argv[1], "--repl") != 0) {
		path = argv[1];
		source = read_source(path, &source_size);

		/* Handles the source availability. */
		if (source == NULL) {
			fprintf(stderr, "noct: cannot read %s\n", path);

			/* Reports operation failure. */
			return 2;
		}
	}
	memset(&main_value, 0, sizeof(main_value));
	memset(&return_value, 0, sizeof(return_value));
	memset(&arguments, 0, sizeof(arguments));
	memset(&argument_value, 0, sizeof(argument_value));
	memset(&options, 0, sizeof(options));
	memset(&memory, 0, sizeof(memory));
	import_environment(&environment, envp);

	/* Handles a failed select memory profile operation. */
	if (!select_memory_profile(&memory)) {
		fprintf(stderr,
			"noct: cannot determine virtual memory capacity\n");
		status = 3;
		goto out;
	}
	arena = mmap(NULL, memory.arena_size, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	/* Handles an operation failure. */
	if (arena == MAP_FAILED) {
		fprintf(stderr, "noct: cannot reserve %u MiB VM heap\n",
			memory.profile_mib);
		status = 4;
		goto out;
	}
	heap_allocator_init(&noct_heap, arena, memory.arena_size);
	bootstrap_heap = heap_active_set(&noct_heap);
	heap_active = 1;
	options.arena = arena;
	options.arena_size = memory.arena_size;
	options.fail_after = (size_t)-1;
	options.jit_enable = 1;
	options.jit_threshold = 32;
	options.write = noct_write;
	options.services = user_noct_services();
	options.environment = &environment;

	noct_set_default_config(&config);
	config.jit_enable = true;
	config.jit_code_size = memory.jit_code_size;
	config.gc_nursery_size = memory.gc_nursery_size;
	config.gc_graduate_size = memory.gc_graduate_size;
	config.gc_tenure_size = memory.gc_tenure_size;

	/* Handles a failed noct create vm operation. */
	if (!noct_create_vm(&vm, &env, &config)) {
		fprintf(stderr, "NOCT.ELF: unable to create VM\n");
		status = 10;
		goto out;
	}

	/* Handles a failed noct napi register operation. */
	if (!noct_napi_register(env, &options) ||
	    !noct_target_register(env, options.services)) {
		status = 11;
		print_error(env, "Noct target API error");
		goto out;
	}

	/* Handles the path availability. */
	if (path == NULL) {
		status = run_repl(env);
		goto out;
	}

	/* Handles a failed bytecode path operation. */
	if (!(bytecode_path(path)
		  ? noct_register_bytecode(env, (uint8_t *)source,
					   (uint32_t)source_size)
		  : noct_register_source(env, path, source))) {
		status = 12;
		print_error(env, "Noct source error");
		return_error(env);
		goto out;
	}

	/* Handles a failed noct get global operation. */
	if (!noct_get_global(env, "main", &main_value) ||
	    !noct_get_func(env, &main_value, &function) ||
	    !noct_get_func_param_count(env, function, &parameter_count) ||
	    parameter_count > 1U) {
		status = 13;
		print_error(env, "Noct main error");
		goto out;
	}

	/* Handles the parameter count condition. */
	if (parameter_count == 1U) {
		/* Handles a failed noct pin local operation. */
		if (!noct_pin_local(env, 2, &arguments, &argument_value) ||
		    !noct_make_empty_array(env, &arguments))
			goto out;

		/* Process each remaining command-line operand. */
		pinned = 1;
		for (i = 2; i < argc; i++) {
			/* Validates the command-line arguments. */
			if (!noct_set_array_elem_make_string(
				env, &arguments, (size_t)(i - 2),
				&argument_value, argv[i]))
				goto out;
		}
	}

	/* Handles a failed noct enter vm operation. */
	if (!noct_enter_vm(env, "main", parameter_count,
			   parameter_count ? &arguments : NULL,
			   &return_value)) {
		status = 14;
		print_error(env, "Noct runtime error");
		goto out;
	}
	status = 0;

	/* Handles a failed noct get value type operation. */
	if (noct_get_value_type(env, &return_value, &type) &&
	    type == NOCT_VALUE_INT)
		(void)noct_get_int(env, &return_value, &status);
out:

	/* Handles the pinned condition. */
	if (pinned)
		(void)noct_unpin_local(env, 2, &arguments, &argument_value);
	noct_target_cleanup();
	noct_napi_cleanup();
	noct_beui_cleanup();

	/* Handles the vm availability. */
	if (vm != NULL)
		(void)noct_destroy_vm(vm);

	/* Handles the heap active condition. */
	if (heap_active) {
		(void)heap_active_set(bootstrap_heap);
		heap_active = 0;
	}

	/* Handles an operation failure. */
	if (arena != MAP_FAILED)
		(void)munmap(arena, memory.arena_size);
	free(source);

	/* Returns the computed result. */
	return status;
}

/* Supports the read source operation. */
static char *
read_source(
	const char *path,
	size_t *size_out)
{
	struct stat status;
	FILE *file;
	char *source;

	/* Handles a failed stat operation. */
	if (stat(path, &status) != 0 || status.st_size < 0 ||
	    (uint32_t)status.st_size > 4U * 1024U * 1024U)

		/* Reports that no result is available. */
		return NULL;
	file = fopen(path, "rb");

	/* Handles the file availability. */
	if (file == NULL)
		return NULL;
	source = malloc((size_t)status.st_size + 1U);

	/* Handles the source availability. */
	if (source == NULL) {
		(void)fclose(file);

		/* Reports that no result is available. */
		return NULL;
	}

	/* Handles a failed fread operation. */
	if (fread(source, 1, (size_t)status.st_size, file) !=
	    (size_t)status.st_size) {
		free(source);
		source = NULL;
	} else {
		source[status.st_size] = '\0';
	}
	(void)fclose(file);

	/* Handles the source availability. */
	if (source != NULL && size_out != NULL)
		*size_out = (size_t)status.st_size;
	/* Returns the computed result. */
	return source;
}

/* Supports the import environment operation. */
static void
import_environment(
	struct environment *environment,
	char **envp)
{
	char *equals;
	char name[32];
	size_t length;
	unsigned i;

	env_init(environment);

	/* Process each element required by the operation. */
	for (i = 0; envp != NULL && envp[i] != NULL; i++) {
		equals = strchr(envp[i], '=');

		/* Handles the equals availability. */
		if (equals == NULL)
			continue;
		length = (size_t)(equals - envp[i]);

		/* Checks the current data length. */
		if (length == 0 || length >= sizeof(name))
			continue;
		memcpy(name, envp[i], length);
		name[length] = '\0';
		(void)env_set(environment, name, equals + 1);
	}
}

/* Supports the select memory profile operation. */
static int
select_memory_profile(
	struct user_noct_memory_profile *profile)
{
	int function_result;
	struct vm_statistics stats;
	int fd;

	memset(&stats, 0, sizeof(stats));
	fd = open("/dev/system", O_RDONLY);

	/* Checks the file descriptor. */
	if (fd < 0)
		return 0;

	/* Handles a failed ioctl operation. */
	if (ioctl(fd, ZEDBSD_SYSTEM_GET_VMSTAT, &stats) != 0) {
		(void)close(fd);

		/* Reports successful completion. */
		return 0;
	}
	(void)close(fd);

	/* Obtains the user noct select memory result. */
	function_result = user_noct_select_memory(stats.physical_total +
					   stats.swap_total *
					       ZEDBSD_SYSTEM_SWAP_PAGE_SIZE,
				       stats.vm_commit_available, profile);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the print error operation. */
static void
print_error(
	NoctEnv *env,
	const char *kind)
{
	const char *file;
	const char *message;
	int line;

	file = "<unknown>";
	message = "Noct error";
	line = 0;

	/* Handles the env availability. */
	if (env != NULL) {
		(void)noct_get_error_file(env, &file);
		(void)noct_get_error_line(env, &line);
		(void)noct_get_error_message(env, &message);
	}
	fprintf(stderr, "%s: %s:%d: %s\n", kind, file, line, message);
}

/* Supports the run repl operation. */
static int
run_repl(
	NoctEnv *env)
{
	enum NoctReplResult result;
	int input;

	NoctReplSession *session = noct_repl_create(env, 32U * 1024U);
	char line[1024];
	int continuation = 0;

	/* Handles the session availability. */
	if (session == NULL)
		return 16;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		input = read_repl_line(line, sizeof(line), continuation);

		/* Validates the current input. */
		if (input == 0) {
			(void)noct_repl_submit(session, NULL);
			break;
		}

		/* Validates the current input. */
		if (input < 0) {
			noct_repl_destroy(session);

			/* Returns the computed result. */
			return 17;
		}
		result = noct_repl_submit(session, line);

		/* Handles an operation failure. */
		if (result == NOCT_REPL_ERROR) {
			print_error(env, "Noct REPL error");
			continuation = 0;
		} else if (result == NOCT_REPL_NEED_MORE) {
			continuation = 1;
		} else {
			continuation = 0;
		}
	}
	noct_repl_destroy(session);

	/* Reports successful completion. */
	return 0;
}

/* Supports the read repl line operation. */
static int
read_repl_line(
	char *line,
	size_t capacity,
	int continuation)
{
	unsigned char byte;
	size_t length;
	const char *prompt;

	length = 0;
	prompt = continuation ? "... " : "noct> ";
	(void)write(1, prompt, strlen(prompt));

	/* Process each remaining element. */
	while (length + 2U < capacity) {
		/* Handles a failed read operation. */
		if (read(0, &byte, 1) != 1)
			return -1;

		/* Classifies the current byte. */
		if (byte == 3U) {
			(void)write(1, "^C\n", 3);

			/* Reports successful completion. */
			return 0;
		}

		/* Classifies the current byte. */
		if (byte == 4U)
			return 0;

		/* Classifies the current byte. */
		if (byte == '\r' || byte == '\n') {
			line[length++] = '\n';
			line[length] = '\0';
			(void)write(1, "\n", 1);

			/* Reports operation failure. */
			return 1;
		}

		/* Classifies the current byte. */
		if (byte == 8U || byte == 0x7fU) {
			/* Checks the current data length. */
			if (length != 0) {
				length--;
				(void)write(1, "\b \b", 3);
			}
			continue;
		}

		/* Classifies the current byte. */
		if (byte >= 0x20U && byte < 0x7fU) {
			line[length++] = (char)byte;
			(void)write(1, &byte, 1);
		}
	}

	/* Reports operation failure. */
	return -1;
}

/* Supports the bytecode path operation. */
static int
bytecode_path(
	const char *path)
{
	size_t length;

	length = strlen(path);

	/* Returns the computed result. */
	return length >= 4U && path[length - 4U] == '.' &&
	       (path[length - 3U] == 'N' || path[length - 3U] == 'n') &&
	       (path[length - 2U] == 'A' || path[length - 2U] == 'a') &&
	       (path[length - 1U] == 'P' || path[length - 1U] == 'p');
}

/* Supports the return error operation. */
static void
return_error(
	NoctEnv *env)
{
	const char *message;

	message = "Noct error";

	/* Handles the env availability. */
	if (env != NULL)
		(void)noct_get_error_message(env, &message);

	/* Handles the message availability. */
	if (message != NULL)
		(void)write(3, message, strlen(message));
}

/* Supports the noct write operation. */
static size_t
noct_write(
	void *context,
	const char *bytes,
	size_t length)
{
	ssize_t count;

	(void)context;
	count = write(1, bytes, length);

	/* Returns the computed result. */
	return count < 0 ? 0U : (size_t)count;
}
