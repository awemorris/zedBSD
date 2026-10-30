/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws291-p022: compiles one SPIR-V shader with the i915 compiler on the host
 * (graphics version 12) and writes the EU code, for a disassembler to read.
 *
 *     shader-dump vert|frag SPV OUT.bin [PRODUCER.spv]
 *
 * A fragment shader is compiled against its vertex shader (PRODUCER), whose
 * varyings it reads.  Prints the code size, the registers and the layout.
 */

#include "../../../../src/drivers/gpu/compiler/spirv.h"
#include "../../../../src/drivers/gpu/i915/compiler/compiler.h"

#include <stdio.h>
#include <stdlib.h>

/* The kernel's allocator, on the host's. */
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

static uint32_t *read_words(const char *path, size_t *count);
static int compile_one(const char *path, enum drv_gpu_shader_stage stage, const struct i915_shader_binary *producer, struct i915_shader_binary **out);

int
main(
	int argc,
	char **argv)
{
	struct i915_shader_binary *producer;
	struct i915_shader_binary *binary;
	enum drv_gpu_shader_stage stage;
	FILE *file;
	int error;

	if (argc != 4 && argc != 5) {
		fprintf(stderr, "usage: shader-dump vert|frag SPV OUT.bin [PRODUCER.spv]\n");
		return 2;
	}
	stage = DRV_GPU_STAGE_VERTEX;
	if (argv[1][0] == 'f')
		stage = DRV_GPU_STAGE_FRAGMENT;

	producer = NULL;
	if (argc == 5) {
		error = compile_one(argv[4], DRV_GPU_STAGE_VERTEX, NULL, &producer);
		if (error != 0)
			return 1;
	}
	error = compile_one(argv[2], stage, producer, &binary);
	if (error != 0)
		return 1;

	file = fopen(argv[3], "wb");
	if (file == NULL)
		return 1;
	fwrite(binary->code, 1, binary->code_bytes, file);
	fclose(file);
	printf("code %u bytes (%u instructions), grf %u, simd %u, push regs %u, inputs %u, varyings %u, scratch %u\n",
	       binary->code_bytes, binary->code_bytes / 16U, binary->grf_used, binary->simd, binary->push_regs,
	       binary->input_count, binary->varying_count, binary->scratch_bytes);
	return 0;
}

static int
compile_one(
	const char *path,
	enum drv_gpu_shader_stage stage,
	const struct i915_shader_binary *producer,
	struct i915_shader_binary **out)
{
	struct drv_gpu_compile_diagnostic diagnostic;
	struct drv_gpu_shader_ir *ir;
	uint32_t *words;
	size_t count;
	int error;

	words = read_words(path, &count);
	if (words == NULL)
		return 1;
	error = drv_gpu_shader_parse(words, count, stage, &ir, &diagnostic);
	if (error != 0) {
		fprintf(stderr, "%s: parse error %d\n", path, error);
		return 1;
	}
	error = drv_i915_shader_compile_gen(ir, producer, 12U, out);
	if (error != 0) {
		fprintf(stderr, "%s: compile error %d\n", path, error);
		return 1;
	}
	return 0;
}

static uint32_t *
read_words(
	const char *path,
	size_t *count)
{
	uint32_t *words;
	FILE *file;
	long size;

	file = fopen(path, "rb");
	if (file == NULL)
		return NULL;
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	words = malloc((size_t)size);
	if (words == NULL || fread(words, 1, (size_t)size, file) != (size_t)size) {
		fclose(file);
		return NULL;
	}
	fclose(file);
	*count = (size_t)size / 4U;
	return words;
}
