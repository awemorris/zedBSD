/*
 * zedBSD Noct lifecycle
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_NOCT_H
#define ZEDBSD_NOCT_H

#include <stddef.h>
#include <stdint.h>

struct bootfs;
struct environment;
struct noct_memory_profile;

#define ZEDBSD_NOCT_NO_FAILURE ((size_t)-1)
#define ZEDBSD_NOCT_REPL_LINE_MAX 256U
#define ZEDBSD_NOCT_REPL_SOURCE_MAX (32U * 1024U)
#ifndef ZEDBSD_NOCT_JIT_CODE_MAX
#define ZEDBSD_NOCT_JIT_CODE_MAX (192U * 1024U)
#endif

enum noct_status {
	ZEDBSD_NOCT_OK = 0,
	ZEDBSD_NOCT_INVALID_ARGUMENT,
	ZEDBSD_NOCT_BUSY,
	ZEDBSD_NOCT_VM_ERROR,
	ZEDBSD_NOCT_API_ERROR,
	ZEDBSD_NOCT_SOURCE_ERROR,
	ZEDBSD_NOCT_SIGNATURE_ERROR,
	ZEDBSD_NOCT_RUNTIME_ERROR,
	ZEDBSD_NOCT_INPUT_ERROR,
	ZEDBSD_NOCT_CLEANUP_ERROR,
};

typedef size_t (*noct_write_fn)(void *context, const char *bytes,
				       size_t length);
typedef void (*noct_jit_code_fn)(void *context, const void *code,
					size_t length);

enum noct_repl_input_result {
	ZEDBSD_NOCT_REPL_INPUT_ERROR = -1,
	ZEDBSD_NOCT_REPL_INPUT_EXIT = 0,
	ZEDBSD_NOCT_REPL_INPUT_LINE = 1,
};

typedef enum noct_repl_input_result
(*noct_repl_read_fn)(void *context, int continuation, char *line,
			    size_t capacity);

struct noct_options {
	void *arena;
	size_t arena_size;
	size_t fail_after;
	int jit_enable;
	int jit_threshold;
	noct_write_fn write;
	void *write_context;
	noct_jit_code_fn observe_jit_code;
	void *jit_context;
	const struct noct_services *services;
	struct bootfs *filesystem;
	struct environment *environment;
	const struct noct_memory_profile *memory;
};

struct noct_result {
	enum noct_status status;
	size_t heap_peak;
	size_t bytes_before_reset;
	size_t current_after_reset;
	size_t heap_errors;
	size_t jit_code_size;
	int jit_region_released;
	int64_t script_status;
};

int noct_run_args(const char *source_name, const char *source,
			 int argc, char *const argv[],
			 const struct noct_options *options,
			 struct noct_result *result);
int noct_run_bytecode_args(const char *program_name, uint8_t *bytecode,
				  uint32_t bytecode_size, int argc,
				  char *const argv[],
				  const struct noct_options *options,
				  struct noct_result *result);
int noct_run(const char *source_name, const char *source,
		    const struct noct_options *options,
		    struct noct_result *result);
int noct_repl(const struct noct_options *options,
		     noct_repl_read_fn read_line, void *read_context,
		     struct noct_result *result);
const char *noct_status_string(enum noct_status status);

#endif
