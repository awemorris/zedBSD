/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws068-p006: runs the i915 executor's SPIR-V parser and shader compiler
 * (src/drivers/gpu/i915/compiler) on the host over SPIR-V files, and says
 * for each whether it is accepted, or why it is refused (the parser's
 * reason and the refused opcode, or the compiler's error).
 *
 *   i915-shader-check vertex|fragment FILE.spv ...
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The kernel's allocator, on the host. */
void *
kern_calloc(size_t count, size_t size)
{
	return calloc(count, size);
}

void
kern_free(void *pointer)
{
	free(pointer);
}

#include "../../../../src/drivers/gpu/i915/compiler/spirv.c"
#include "../../../../src/drivers/gpu/i915/compiler/eu.c"
#include "../../../../src/drivers/gpu/i915/compiler/compile.c"

/* Reads a file of words. */
static uint32_t *
check_load(const char *path, size_t *words)
{
	FILE *file;
	long size;
	uint32_t *code;

	file = fopen(path, "rb");
	if (file == NULL)
		return NULL;
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	code = malloc((size_t)size);
	if (code == NULL || fread(code, 1U, (size_t)size, file) != (size_t)size) {
		fclose(file);
		free(code);
		return NULL;
	}
	fclose(file);
	*words = (size_t)size / 4U;
	return code;
}

int
main(
	int argc,
	char **argv)
{
	struct i915_compile_diagnostic diagnostic;
	struct i915_shader_ir *ir;
	struct i915_shader_binary *binary;
	enum i915_shader_stage stage;
	uint32_t *code;
	size_t words;
	int index;
	int error;
	int status;

	/* The stage, then the files. */
	if (argc < 3) {
		fprintf(stderr, "usage: i915-shader-check vertex|fragment FILE.spv ...\n");
		return 2;
	}
	stage = I915_STAGE_VERTEX;
	if (strcmp(argv[1], "fragment") == 0)
		stage = I915_STAGE_FRAGMENT;

	/* Each file: parsed, then compiled. */
	status = 0;
	for (index = 2; index < argc; index++) {
		code = check_load(argv[index], &words);
		if (code == NULL) {
			printf("%s: cannot read\n", argv[index]);
			status = 1;
			continue;
		}
		memset(&diagnostic, 0, sizeof(diagnostic));
		error = drv_i915_shader_parse(code, words, stage, &ir, &diagnostic);
		if (error != 0) {
			printf("%s: REFUSED by the parser: error %d (%s; opcode %u at word %u)\n", argv[index], error,
			       diagnostic.reason != NULL ? diagnostic.reason : "?", diagnostic.opcode, diagnostic.word_offset);
			free(code);
			status = 1;
			continue;
		}
		error = drv_i915_shader_compile(ir, &binary);
		drv_i915_shader_ir_free(ir);
		if (error != 0) {
			printf("%s: REFUSED by the compiler: error %d\n", argv[index], error);
			status = 1;
		} else {
			printf("%s: accepted (%u bytes)\n", argv[index], binary->code_bytes);
		}
		free(code);
	}
	return status;
}
