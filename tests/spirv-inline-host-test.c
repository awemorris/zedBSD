/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host test of the SPIR-V parser's inlining of function calls
 * (src/drivers/gpu/compiler/spirv-inline.inc), through the Mali Valhall
 * compiler and its one-lane model.
 *
 *   pair A.spv B.spv     A has helper functions (glslc without -O), B is the same source with one function (glslc
 *                        -O).  A must be refused without the INLINE_CALLS feature, as before (its call is not
 *                        lowered, or it has more than one function); with it the rewritten module is written to
 *                        OUTDIR/A.inlined.spv (for spirv-val), A compiles, and its program agrees on the model with
 *                        the IR reference of A's rewritten module and of B.
 *   refuse A.spv TEXT    the inlining refuses A with a reason containing TEXT.
 *   same A.spv           a module of one function parses to the same IR with and without INLINE_CALLS.
 *
 * usage: spirv-inline-host-test OUTDIR TRIALS COMMAND...   One line per check, PASS or FAIL.
 * Nothing here is evidence about any GPU.
 */

#include "src/drivers/gpu/compiler/spirv.c"

#include "drivers/gpu/mali/compiler/shader.h"

#include "tests/mali-compiler-model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *kern_malloc(size_t size);
void *kern_calloc(size_t count, size_t size);
void kern_free(void *pointer);

static uint32_t *read_file(const char *path, size_t *words);
static uint32_t module_stage(const uint32_t *words, size_t count, enum drv_gpu_shader_stage *ir_stage);
static const char *base_name(const char *path);
static int test_pair(const char *outdir, const char *a_path, const char *b_path, unsigned trials);
static int test_refuse(const char *path, const char *text);
static int test_same(const char *path);

void *
kern_malloc(
	size_t size)
{
	return malloc(size);
}

void *
kern_calloc(
	size_t count,
	size_t size)
{
	return calloc(count, size);
}

void
kern_free(
	void *pointer)
{
	free(pointer);
}

int
main(
	int argc,
	char **argv)
{
	unsigned trials;
	int failed;
	int index;

	if (argc < 3) {
		fprintf(stderr, "usage: %s OUTDIR TRIALS COMMAND...\n", argv[0]);
		return 2;
	}
	trials = (unsigned)strtoul(argv[2], NULL, 10);
	failed = 0;
	index = 3;
	while (index < argc) {
		if (strcmp(argv[index], "pair") == 0 && index + 2 < argc) {
			failed |= test_pair(argv[1], argv[index + 1], argv[index + 2], trials);
			index += 3;
		} else if (strcmp(argv[index], "refuse") == 0 && index + 2 < argc) {
			failed |= test_refuse(argv[index + 1], argv[index + 2]);
			index += 3;
		} else if (strcmp(argv[index], "same") == 0 && index + 1 < argc) {
			failed |= test_same(argv[index + 1]);
			index += 2;
		} else {
			fprintf(stderr, "bad command %s\n", argv[index]);
			return 2;
		}
	}
	return failed;
}

/* Checks a module of helper functions against the same source with one function. */
static int
test_pair(
	const char *outdir,
	const char *a_path,
	const char *b_path,
	unsigned trials)
{
	struct drv_gpu_compile_diagnostic diagnostic;
	struct mali_shader_diagnostic mali_diagnostic;
	struct drv_gpu_shader_ir *ir;
	struct mali_shader_program *program;
	enum drv_gpu_shader_stage ir_stage;
	uint32_t *a;
	uint32_t *b;
	uint32_t *inlined;
	size_t a_count;
	size_t b_count;
	size_t inlined_count;
	uint32_t stage;
	char path[1024];
	const char *name;
	FILE *file;
	int error;
	int failed;

	name = base_name(a_path);
	a = read_file(a_path, &a_count);
	b = read_file(b_path, &b_count);
	if (a == NULL || b == NULL) {
		printf("FAIL %s: cannot read\n", name);
		return 1;
	}
	stage = module_stage(a, a_count, &ir_stage);
	failed = 0;

	/*
	 * Without the feature the module of several functions is refused as before: at the entry function's first call
	 * (not lowered) when the entry comes first, else at the second function.
	 */
	ir = NULL;
	error = drv_gpu_shader_parse_features(a, a_count, ir_stage, MALI_SHADER_SPIRV_FEATURES & ~DRV_GPU_SPIRV_FEATURE_INLINE_CALLS,
					      &ir, &diagnostic);
	if (error != ENOTSUP || diagnostic.reason == NULL ||
	    (strstr(diagnostic.reason, "more than one function") == NULL && strstr(diagnostic.reason, "not lowered") == NULL)) {
		printf("FAIL %s: without INLINE_CALLS the parse gives %d (%s)\n", name, error,
		       diagnostic.reason != NULL ? diagnostic.reason : "");
		failed = 1;
	}
	drv_gpu_shader_ir_free(ir);

	/* The rewritten module, for spirv-val. */
	inlined = NULL;
	error = drv_gpu_spirv_inline(a, a_count, &inlined, &inlined_count, &diagnostic);
	if (error != 0 || inlined == NULL) {
		printf("FAIL %s: the inlining gives %d (%s)\n", name, error, diagnostic.reason != NULL ? diagnostic.reason : "");
		free(a);
		free(b);
		return 1;
	}
	snprintf(path, sizeof(path), "%s/%s.inlined.spv", outdir, name);
	file = fopen(path, "wb");
	if (file == NULL || fwrite(inlined, sizeof(*inlined), inlined_count, file) != inlined_count) {
		printf("FAIL %s: cannot write %s\n", name, path);
		failed = 1;
	}
	if (file != NULL)
		fclose(file);

	/* Compiles with the feature: the parse inlines. */
	ir = NULL;
	error = drv_gpu_shader_parse_features(a, a_count, ir_stage, MALI_SHADER_SPIRV_FEATURES | DRV_GPU_SPIRV_FEATURE_INLINE_CALLS,
					      &ir, &diagnostic);
	program = NULL;
	if (error == 0)
		error = mali_shader_compile_ir(ir, NULL, &program, &mali_diagnostic);
	drv_gpu_shader_ir_free(ir);
	if (error != 0) {
		printf("FAIL %s: compile error %d (%s)\n", name, error, diagnostic.reason != NULL ? diagnostic.reason : "");
		free(inlined);
		free(a);
		free(b);
		return 1;
	}

	/* The program against the rewritten module's IR, then against the one-function module's. */
	error = mali_model_check(inlined, inlined_count, stage, program, trials, name);
	if (error != 0) {
		printf("FAIL %s: the model disagrees with the rewritten module's IR\n", name);
		failed = 1;
	}
	error = mali_model_check(b, b_count, stage, program, trials, name);
	if (error != 0) {
		printf("FAIL %s: the model disagrees with %s's IR\n", name, base_name(b_path));
		failed = 1;
	}
	mali_shader_program_free(program);
	free(inlined);
	free(a);
	free(b);
	if (failed == 0)
		printf("PASS pair %s %s\n", name, base_name(b_path));
	return failed;
}

/* Checks that the inlining refuses a module, with a reason. */
static int
test_refuse(
	const char *path,
	const char *text)
{
	struct drv_gpu_compile_diagnostic diagnostic;
	struct drv_gpu_shader_ir *ir;
	enum drv_gpu_shader_stage ir_stage;
	uint32_t *words;
	size_t count;
	int error;

	words = read_file(path, &count);
	if (words == NULL) {
		printf("FAIL %s: cannot read\n", base_name(path));
		return 1;
	}
	(void)module_stage(words, count, &ir_stage);
	ir = NULL;
	memset(&diagnostic, 0, sizeof(diagnostic));
	error = drv_gpu_shader_parse_features(words, count, ir_stage, MALI_SHADER_SPIRV_FEATURES | DRV_GPU_SPIRV_FEATURE_INLINE_CALLS,
					      &ir, &diagnostic);
	drv_gpu_shader_ir_free(ir);
	free(words);
	if (error != ENOTSUP || diagnostic.reason == NULL || strstr(diagnostic.reason, text) == NULL ||
	    diagnostic.opcode != OP_FUNCTION_CALL) {
		printf("FAIL refuse %s: %d (%s)\n", base_name(path), error, diagnostic.reason != NULL ? diagnostic.reason : "");
		return 1;
	}
	printf("PASS refuse %s: %s\n", base_name(path), diagnostic.reason);
	return 0;
}

/* Checks that a module of one function parses to the same IR with and without the feature. */
static int
test_same(
	const char *path)
{
	struct drv_gpu_compile_diagnostic diagnostic;
	struct drv_gpu_shader_ir *plain;
	struct drv_gpu_shader_ir *with;
	enum drv_gpu_shader_stage ir_stage;
	uint32_t *words;
	size_t count;
	int error_plain;
	int error_with;
	int same;

	words = read_file(path, &count);
	if (words == NULL) {
		printf("FAIL %s: cannot read\n", base_name(path));
		return 1;
	}
	(void)module_stage(words, count, &ir_stage);
	plain = NULL;
	with = NULL;
	error_plain = drv_gpu_shader_parse_features(words, count, ir_stage, MALI_SHADER_SPIRV_FEATURES & ~DRV_GPU_SPIRV_FEATURE_INLINE_CALLS,
						    &plain, &diagnostic);
	error_with = drv_gpu_shader_parse_features(words, count, ir_stage, MALI_SHADER_SPIRV_FEATURES | DRV_GPU_SPIRV_FEATURE_INLINE_CALLS,
						   &with, &diagnostic);
	same = error_plain == error_with;
	if (same && plain != NULL && with != NULL) {
		same = plain->instruction_count == with->instruction_count &&
		       memcmp(plain->instructions, with->instructions, plain->instruction_count * sizeof(*plain->instructions)) == 0;
	}
	drv_gpu_shader_ir_free(plain);
	drv_gpu_shader_ir_free(with);
	free(words);
	if (!same) {
		printf("FAIL same %s: the IR differs with INLINE_CALLS\n", base_name(path));
		return 1;
	}
	printf("PASS same %s\n", base_name(path));
	return 0;
}

/* Returns the Mali stage of a module's first entry point (and the IR's). */
static uint32_t
module_stage(
	const uint32_t *words,
	size_t count,
	enum drv_gpu_shader_stage *ir_stage)
{
	size_t at;

	*ir_stage = DRV_GPU_STAGE_COMPUTE;
	for (at = 5U; at < count && (words[at] >> 16) != 0U; at += words[at] >> 16) {
		if ((words[at] & 0xFFFFU) == 15U && at + 1U < count) {
			if (words[at + 1U] == 0U) {
				*ir_stage = DRV_GPU_STAGE_VERTEX;
				return MALI_SHADER_VERTEX;
			}
			if (words[at + 1U] == 4U) {
				*ir_stage = DRV_GPU_STAGE_FRAGMENT;
				return MALI_SHADER_FRAGMENT;
			}
			break;
		}
	}
	return MALI_SHADER_COMPUTE;
}

/* Returns the file name of a path. */
static const char *
base_name(
	const char *path)
{
	const char *slash;

	slash = strrchr(path, '/');
	return slash == NULL ? path : slash + 1;
}

/* Reads a SPIR-V file. */
static uint32_t *
read_file(
	const char *path,
	size_t *words)
{
	FILE *file;
	uint32_t *buffer;
	long size;

	file = fopen(path, "rb");
	if (file == NULL)
		return NULL;
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	if (size <= 0 || (size % 4) != 0) {
		fclose(file);
		return NULL;
	}
	buffer = malloc((size_t)size);
	if (buffer != NULL && fread(buffer, 1, (size_t)size, file) != (size_t)size) {
		free(buffer);
		buffer = NULL;
	}
	fclose(file);
	*words = (size_t)size / 4U;
	return buffer;
}
