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
 *   i915-shader-check geometry VERTEX.spv FILE.spv ...
 *
 * A geometry shader reads its inputs from the VUE of the vertex shader
 * before it (ws075-p007a), so the geometry mode first compiles VERTEX.spv
 * and gives it to each geometry shader as its producer.
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

#include "../../../../src/drivers/gpu/compiler/spirv.c"
#include "../../../../src/drivers/gpu/i915/compiler/eu.c"
#include "../../../../src/drivers/gpu/i915/compiler/compile.c"

static uint32_t *check_load(const char *path, size_t *words);
static struct i915_shader_binary *check_compile(const char *path, enum drv_gpu_shader_stage stage, const struct i915_shader_binary *producer);

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

/*
 * Parses and compiles one file as the stage given, after `producer` (the
 * vertex kernel before a geometry shader, NULL otherwise).  Returns the
 * binary, or NULL with the refusal printed.
 */
static struct i915_shader_binary *
check_compile(
	const char *path,
	enum drv_gpu_shader_stage stage,
	const struct i915_shader_binary *producer)
{
	struct drv_gpu_compile_diagnostic diagnostic;
	struct drv_gpu_shader_ir *ir;
	struct i915_shader_binary *binary;
	const char *reason;
	uint32_t *code;
	size_t words;
	int error;

	/* Reads the module. */
	code = check_load(path, &words);
	if (code == NULL) {
		printf("%s: cannot read\n", path);
		return NULL;
	}

	/* Parses it into the compiler's IR, and says why when it is refused. */
	memset(&diagnostic, 0, sizeof(diagnostic));
	error = drv_gpu_shader_parse(code, words, stage, &ir, &diagnostic);
	free(code);
	if (error != 0) {
		reason = "?";
		if (diagnostic.reason != NULL)
			reason = diagnostic.reason;
		printf("%s: REFUSED by the parser: error %d (%s; opcode %u at word %u)\n", path, error, reason,
		       diagnostic.opcode, diagnostic.word_offset);
		return NULL;
	}

	/* A module of another stage than the one asked for is refused as the driver refuses it. */
	if (ir->stage != stage) {
		printf("%s: REFUSED: a module of stage %d given as stage %d\n", path, (int)ir->stage, (int)stage);
		drv_gpu_shader_ir_free(ir);
		return NULL;
	}

	/* Compiles the IR to EU code. */
	binary = NULL;
	error = drv_i915_shader_compile_stage(ir, producer, &binary);
	drv_gpu_shader_ir_free(ir);
	if (error != 0) {
		printf("%s: REFUSED by the compiler: error %d\n", path, error);
		return NULL;
	}

	/* Succeeded: the module is accepted. */
	return binary;
}

int
main(
	int argc,
	char **argv)
{
	struct i915_shader_binary *producer;
	struct i915_shader_binary *binary;
	enum drv_gpu_shader_stage stage;
	int first;
	int index;
	int status;

	/* The stage, then the files. */
	if (argc < 3) {
		fprintf(stderr, "usage: i915-shader-check vertex|fragment FILE.spv ... | geometry VERTEX.spv FILE.spv ...\n");
		return 2;
	}
	stage = DRV_GPU_STAGE_VERTEX;
	if (strcmp(argv[1], "fragment") == 0)
		stage = DRV_GPU_STAGE_FRAGMENT;
	if (strcmp(argv[1], "geometry") == 0)
		stage = DRV_GPU_STAGE_GEOMETRY;

	/* A geometry shader reads the VUE of the vertex shader named first. */
	producer = NULL;
	first = 2;
	if (stage == DRV_GPU_STAGE_GEOMETRY) {
		if (argc < 4) {
			fprintf(stderr, "usage: i915-shader-check geometry VERTEX.spv FILE.spv ...\n");
			return 2;
		}
		producer = check_compile(argv[2], DRV_GPU_STAGE_VERTEX, NULL);
		if (producer == NULL)
			return 1;
		first = 3;
	}

	/* Each file: parsed, then compiled. */
	status = 0;
	for (index = first; index < argc; index++) {
		binary = check_compile(argv[index], stage, producer);
		if (binary == NULL) {
			status = 1;
			continue;
		}
		printf("%s: accepted (%u bytes)\n", argv[index], binary->code_bytes);
		drv_i915_shader_binary_free(binary);
	}

	/* The producer was only needed while compiling. */
	drv_i915_shader_binary_free(producer);
	return status;
}
