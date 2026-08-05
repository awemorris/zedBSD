/*
 * Boots Noct lifecycle
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#ifndef BOOTS_NOCT_H
#define BOOTS_NOCT_H

#include <stddef.h>
#include <stdint.h>

struct boots_filesystem;
struct boots_environment;
struct boots_noct_memory_profile;

#define BOOTS_NOCT_NO_FAILURE ((size_t)-1)
#define BOOTS_NOCT_REPL_LINE_MAX 256U
#define BOOTS_NOCT_REPL_SOURCE_MAX (32U * 1024U)
#ifndef BOOTS_NOCT_JIT_CODE_MAX
#define BOOTS_NOCT_JIT_CODE_MAX (192U * 1024U)
#endif

enum boots_noct_status {
	BOOTS_NOCT_OK = 0,
	BOOTS_NOCT_INVALID_ARGUMENT,
	BOOTS_NOCT_BUSY,
	BOOTS_NOCT_VM_ERROR,
	BOOTS_NOCT_API_ERROR,
	BOOTS_NOCT_SOURCE_ERROR,
	BOOTS_NOCT_SIGNATURE_ERROR,
	BOOTS_NOCT_RUNTIME_ERROR,
	BOOTS_NOCT_INPUT_ERROR,
	BOOTS_NOCT_CLEANUP_ERROR,
};

typedef size_t (*boots_noct_write_fn)(void *context, const char *bytes,
				       size_t length);
typedef void (*boots_noct_jit_code_fn)(void *context, const void *code,
					size_t length);

enum boots_noct_repl_input_result {
	BOOTS_NOCT_REPL_INPUT_ERROR = -1,
	BOOTS_NOCT_REPL_INPUT_EXIT = 0,
	BOOTS_NOCT_REPL_INPUT_LINE = 1,
};

typedef enum boots_noct_repl_input_result
(*boots_noct_repl_read_fn)(void *context, int continuation, char *line,
			    size_t capacity);

struct boots_noct_options {
	void *arena;
	size_t arena_size;
	size_t fail_after;
	int jit_enable;
	int jit_threshold;
	boots_noct_write_fn write;
	void *write_context;
	boots_noct_jit_code_fn observe_jit_code;
	void *jit_context;
	const struct boots_noct_services *services;
	struct boots_filesystem *filesystem;
	struct boots_environment *environment;
	const struct boots_noct_memory_profile *memory;
};

struct boots_noct_result {
	enum boots_noct_status status;
	size_t heap_peak;
	size_t bytes_before_reset;
	size_t current_after_reset;
	size_t heap_errors;
	size_t jit_code_size;
	int jit_region_released;
	int64_t script_status;
};

int boots_noct_run_args(const char *source_name, const char *source,
			 int argc, char *const argv[],
			 const struct boots_noct_options *options,
			 struct boots_noct_result *result);
int boots_noct_run_bytecode_args(const char *program_name, uint8_t *bytecode,
				  uint32_t bytecode_size, int argc,
				  char *const argv[],
				  const struct boots_noct_options *options,
				  struct boots_noct_result *result);
int boots_noct_run(const char *source_name, const char *source,
		    const struct boots_noct_options *options,
		    struct boots_noct_result *result);
int boots_noct_repl(const struct boots_noct_options *options,
		     boots_noct_repl_read_fn read_line, void *read_context,
		     struct boots_noct_result *result);
const char *boots_noct_status_string(enum boots_noct_status status);

#endif
