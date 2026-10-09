/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws075-p021: compiles one SPIR-V module with the i915 executor's compiler on
 * the host and writes the kernel's EU code to a file, for Mesa's disassembler
 * (plan/ws075/tests/guard/run.sh), and checks its scoreboard (scoreboard-check.h, ws075-p022): the kernel
 * must be sound, and the same kernel with every sync.nop taken out must not be (so the check can fail) when
 * it has an out-of-order instruction whose result is used.
 *
 *   shader-dump vertex|fragment FILE.spv OUT.bin
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The kernel's allocator, on the host. */
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

#include "../../../../src/drivers/gpu/compiler/spirv.c"
#include "../../../../src/drivers/gpu/i915/compiler/eu.c"
#include "../../../../src/drivers/gpu/i915/compiler/compile.c"
#include "scoreboard-check.h"

int
main(
	int argc,
	char **argv)
{
	struct drv_gpu_compile_diagnostic diagnostic;
	struct drv_gpu_shader_ir *ir;
	struct i915_shader_binary *binary;
	enum drv_gpu_shader_stage stage;
	uint32_t *code;
	FILE *file;
	long size;
	int error;

	/* The stage, the module and the output. */
	if (argc != 4) {
		fprintf(stderr, "usage: shader-dump vertex|fragment FILE.spv OUT.bin\n");
		return 2;
	}
	stage = DRV_GPU_STAGE_VERTEX;
	if (strcmp(argv[1], "fragment") == 0)
		stage = DRV_GPU_STAGE_FRAGMENT;

	/* Reads the module. */
	file = fopen(argv[2], "rb");
	if (file == NULL)
		return 1;
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	code = malloc((size_t)size);
	if (code == NULL || fread(code, 1U, (size_t)size, file) != (size_t)size)
		return 1;
	fclose(file);

	/* Parses and compiles it. */
	memset(&diagnostic, 0, sizeof(diagnostic));
	error = drv_gpu_shader_parse(code, (size_t)size / 4U, stage, &ir, &diagnostic);
	if (error != 0) {
		fprintf(stderr, "%s: refused by the parser: %d (%s)\n", argv[2], error, diagnostic.reason != NULL ? diagnostic.reason : "?");
		return 1;
	}
	error = drv_i915_shader_compile(ir, &binary);
	if (error != 0) {
		fprintf(stderr, "%s: refused by the compiler: %d\n", argv[2], error);
		return 1;
	}

	/* The scoreboard: sound as made, and faulty without its waits (when it has any). */
	{
		uint32_t *stripped;
		unsigned count, index, kept;
		int fault;

		count = binary->code_bytes / 16U;
		fault = sbc_check(binary->code, count);
		if (fault >= 0) {
			fprintf(stderr, "%s: scoreboard fault at instruction %d\n", argv[2], fault);
			return 1;
		}
		stripped = calloc(count, 16U);
		kept = 0U;
		for (index = 0U; index < count; index++) {
			if (sbc_field(binary->code + index * 4U, 6U, 0U) == 1U)
				continue;
			memcpy(stripped + kept * 4U, binary->code + index * 4U, 16U);
			kept++;
		}
		fault = sbc_check(stripped, kept);
		printf("%s: scoreboard sound; %u sync.nop; without them %s\n", argv[3], count - kept,
		       fault >= 0 ? "a fault is found" : "no fault");
		free(stripped);
	}

	/* Writes the kernel's code. */
	file = fopen(argv[3], "wb");
	if (file == NULL)
		return 1;
	fwrite(binary->code, 1U, binary->code_bytes, file);
	fclose(file);
	printf("%s: %u bytes\n", argv[3], binary->code_bytes);
	return 0;
}
