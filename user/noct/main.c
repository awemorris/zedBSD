/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "user/noct/zedbsd-api.h"

#include <noct/noct.h>
#include <noct/repl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static size_t noct_write(void *context, const char *bytes, size_t length)
{
	ssize_t count;
	(void)context;
	count = write(1, bytes, length);
	return count < 0 ? 0U : (size_t)count;
}

static void print_error(NoctEnv *env, const char *kind)
{
	const char *file = "<unknown>";
	const char *message = "Noct error";
	int line = 0;
	if (env != NULL) {
		(void)noct_get_error_file(env, &file);
		(void)noct_get_error_line(env, &line);
		(void)noct_get_error_message(env, &message);
	}
	fprintf(stderr, "%s: %s:%d: %s\n", kind, file, line, message);
}

static void return_error(NoctEnv *env)
{
	const char *message = "Noct error";
	if (env != NULL)
		(void)noct_get_error_message(env, &message);
	if (message != NULL)
		(void)write(3, message, strlen(message));
}

static int read_repl_line(char *line, size_t capacity, int continuation)
{
	size_t length = 0;
	const char *prompt = continuation ? "... " : "noct> ";
	(void)write(1, prompt, strlen(prompt));
	while (length + 2U < capacity) {
		unsigned char byte;
		if (read(0, &byte, 1) != 1)
			return -1;
		if (byte == 3U) {
			(void)write(1, "^C\n", 3);
			return 0;
		}
		if (byte == 4U)
			return 0;
		if (byte == '\r' || byte == '\n') {
			line[length++] = '\n';
			line[length] = '\0';
			(void)write(1, "\n", 1);
			return 1;
		}
		if (byte == 8U || byte == 0x7fU) {
			if (length != 0) {
				length--;
				(void)write(1, "\b \b", 3);
			}
			continue;
		}
		if (byte >= 0x20U && byte < 0x7fU) {
			line[length++] = (char)byte;
			(void)write(1, &byte, 1);
		}
	}
	return -1;
}

static int run_repl(NoctEnv *env)
{
	NoctReplSession *session = noct_repl_create(env, 32U * 1024U);
	char line[1024];
	int continuation = 0;
	if (session == NULL)
		return 16;
	for (;;) {
		enum NoctReplResult result;
		int input = read_repl_line(line, sizeof(line), continuation);
		if (input == 0) {
			(void)noct_repl_submit(session, NULL);
			break;
		}
		if (input < 0) {
			noct_repl_destroy(session);
			return 17;
		}
		result = noct_repl_submit(session, line);
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
	return 0;
}

static char *read_source(const char *path, size_t *size_out)
{
	struct stat status;
	FILE *file;
	char *source;
	if (stat(path, &status) != 0 || status.st_size < 0 ||
	    (uint32_t)status.st_size > 4U * 1024U * 1024U)
		return NULL;
	file = fopen(path, "rb");
	if (file == NULL) return NULL;
	source = malloc((size_t)status.st_size + 1U);
	if (source == NULL) { (void)fclose(file); return NULL; }
	if (fread(source, 1, (size_t)status.st_size, file) != (size_t)status.st_size) {
		free(source); source = NULL;
	} else {
		source[status.st_size] = '\0';
	}
	(void)fclose(file);
	if (source != NULL && size_out != NULL)
		*size_out = (size_t)status.st_size;
	return source;
}

static int bytecode_path(const char *path)
{
	size_t length = strlen(path);
	return length >= 4U && path[length - 4U] == '.' &&
		(path[length - 3U] == 'N' || path[length - 3U] == 'n') &&
		(path[length - 2U] == 'A' || path[length - 2U] == 'a') &&
		(path[length - 1U] == 'P' || path[length - 1U] == 'p');
}

static void import_environment(struct zedbsd_environment *environment, char **envp)
{
	unsigned i;
	zedbsd_env_init(environment);
	for (i = 0; envp != NULL && envp[i] != NULL; i++) {
		char *equals = strchr(envp[i], '=');
		char name[32]; size_t length;
		if (equals == NULL) continue;
		length = (size_t)(equals - envp[i]);
		if (length == 0 || length >= sizeof(name)) continue;
		memcpy(name, envp[i], length); name[length] = '\0';
		(void)zedbsd_env_set(environment, name, equals + 1);
	}
}

int main(int argc, char **argv, char **envp)
{
	NoctConfig config;
	NoctVM *vm = NULL;
	NoctEnv *env = NULL;
	NoctValue main_value, return_value, arguments, argument_value;
	NoctFunc *function = NULL;
	struct zedbsd_noct_options options;
	struct zedbsd_environment environment;
	const char *path = NULL;
	char *source = NULL;
	size_t source_size = 0;
	size_t parameter_count = 0;
	int pinned = 0, status = 1, type;

	if (argc >= 2 && strcmp(argv[1], "--repl") != 0) {
		path = argv[1];
		source = read_source(path, &source_size);
		if (source == NULL) {
			fprintf(stderr, "noct: cannot read %s\n", path);
			return 2;
		}
	}
	memset(&main_value, 0, sizeof(main_value));
	memset(&return_value, 0, sizeof(return_value));
	memset(&arguments, 0, sizeof(arguments));
	memset(&argument_value, 0, sizeof(argument_value));
	memset(&options, 0, sizeof(options));
	import_environment(&environment, envp);
	options.arena = source;
	options.arena_size = 8U * 1024U * 1024U;
	options.fail_after = (size_t)-1;
	options.jit_enable = 1;
	options.jit_threshold = 32;
	options.write = noct_write;
	options.services = zedbsd_user_noct_services();
	options.environment = &environment;

	noct_set_default_config(&config);
	config.jit_enable = true;
	config.jit_threshold = 32;
	config.jit_code_size = 1024U * 1024U;
	config.gc_nursery_size = 256U * 1024U;
	config.gc_graduate_size = 256U * 1024U;
	config.gc_tenure_size = 2U * 1024U * 1024U;
	if (!noct_create_vm(&vm, &env, &config)) {
		fprintf(stderr, "NOCT.ELF: unable to create VM\n");
		status = 10;
		goto out;
	}
	if (!zedbsd_noct_napi_register(env, &options) ||
	    !zedbsd_noct_target_register(env, options.services)) {
		status = 11; print_error(env, "Noct target API error"); goto out;
	}
	if (path == NULL) {
		status = run_repl(env);
		goto out;
	}
	if (!(bytecode_path(path) ?
	      noct_register_bytecode(env, (uint8_t *)source,
				     (uint32_t)source_size) :
	      noct_register_source(env, path, source))) {
		status = 12; print_error(env, "Noct source error");
		return_error(env); goto out;
	}
	if (!noct_get_global(env, "main", &main_value) ||
	    !noct_get_func(env, &main_value, &function) ||
	    !noct_get_func_param_count(env, function, &parameter_count) ||
	    parameter_count > 1U) {
		status = 13; print_error(env, "Noct main error"); goto out;
	}
	if (parameter_count == 1U) {
		int i;
		if (!noct_pin_local(env, 2, &arguments, &argument_value) ||
		    !noct_make_empty_array(env, &arguments))
			goto out;
		pinned = 1;
		for (i = 2; i < argc; i++)
			if (!noct_set_array_elem_make_string(env, &arguments,
				(size_t)(i - 2), &argument_value, argv[i]))
				goto out;
	}
	if (!noct_enter_vm(env, "main", parameter_count, parameter_count ? &arguments : NULL,
		&return_value)) {
		status = 14; print_error(env, "Noct runtime error"); goto out;
	}
	{
		const char *action = zedbsd_env_get(&environment, "BOOT_ACTION");
		const char *result_fd = zedbsd_env_get(&environment,
						      "ZEDBSD_RESULT_FD");
		if (action != NULL && result_fd != NULL &&
		    result_fd[0] == '3' && result_fd[1] == '\0') {
			size_t length = strlen(action);
			if (length == 0 || write(3, action, length) != (ssize_t)length) {
				fprintf(stderr, "NOCT.ELF: unable to return BOOT_ACTION\n");
				status = 15;
				goto out;
			}
		}
	}
	status = 0;
	if (noct_get_value_type(env, &return_value, &type) && type == NOCT_VALUE_INT)
		(void)noct_get_int(env, &return_value, &status);
out:
	if (pinned) (void)noct_unpin_local(env, 2, &arguments, &argument_value);
	zedbsd_noct_target_cleanup();
	zedbsd_noct_napi_cleanup();
	noct_beui_cleanup();
	if (vm != NULL) (void)noct_destroy_vm(vm);
	free(source);
	return status;
}
