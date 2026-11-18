/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host check of the pre-frame draw's program
 * (src/drivers/gpu/mali/vk/vk-preload-spirv.h): the SPIR-V the driver
 * carries compiles with the Mali Valhall compiler, and the program has
 * the shape the pre-frame draw writes descriptors for -- a fragment
 * program of one texture at entry 0 whose size it does not read, no
 * buffers and no FAU words, writing colour 0 from the pixel's position.
 * The program's words are written to the file named by the argument,
 * when one is given, for a decode with the compiler's host test.
 *
 * cc -std=gnu11 -DKERN_USER_ABI_LP64 -Iinclude -Isrc -I. -idirafter libc/include -Wall -Wextra -Werror \
 *    tests/mali-preload-host-test.c src/drivers/gpu/mali/compiler/va-compile.c \
 *    src/drivers/gpu/mali/compiler/va-encode.c src/drivers/gpu/mali/compiler/va-schedule.c \
 *    src/drivers/gpu/compiler/spirv.c src/drivers/gpu/compiler/optimize.c -lm -o <out> && <out> [spv]
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "drivers/gpu/mali/compiler/shader.h"
#include "drivers/gpu/mali/vk/vk-preload-spirv.h"

/* How many checks failed and ran. */
static int failures;
static int checks;

/* The kernel's allocator, as the compiler calls it, on the host's. */
void *kern_malloc(size_t size);
void *kern_calloc(size_t count, size_t size);
void kern_free(void *pointer);

static void expect(const char *name, uint64_t got, uint64_t want);
static int write_spirv(const char *path);

/* Allocates through the host's allocator. */
void *
kern_malloc(
	size_t size)
{
	/* The host's memory. */
	return malloc(size);
}

/* Allocates zeroed memory through the host's allocator. */
void *
kern_calloc(
	size_t count,
	size_t size)
{
	/* The host's zeroed memory. */
	return calloc(count, size);
}

/* Frees through the host's allocator. */
void
kern_free(
	void *pointer)
{
	/* Back to the host. */
	free(pointer);
}

/* Compiles the program, checks its shape and reports the count. */
int
main(
	int argc,
	char **argv)
{
	struct mali_shader_program *program;
	struct mali_shader_diagnostic diagnostic = { 0 };
	int error;

	/* The words, for the compiler's own host test to decode, when asked. */
	if (argc > 1) {
		error = write_spirv(argv[1]);
		if (error != 0)
			return 1;
	}

	/* The fragment program. */
	program = NULL;
	error = mali_shader_compile(mali_preload_spirv, sizeof(mali_preload_spirv) / sizeof(mali_preload_spirv[0]), MALI_SHADER_FRAGMENT, NULL, &program, &diagnostic);
	expect("compiled", (uint64_t)error, 0);
	if (error != 0 || program == NULL) {
		printf("FAIL compile (%d) at %u: %s\n", error, diagnostic.instruction, diagnostic.reason != NULL ? diagnostic.reason : "no reason");
		return 1;
	}

	/* The shape the pre-frame draw's descriptors are written for. */
	expect("fragment stage", program->stage, MALI_SHADER_FRAGMENT);
	expect("one texture", program->texture_count, 1);
	expect("texture at entry 0", program->textures[0].index, 0);
	expect("texture at set 0", program->textures[0].set, 0);
	expect("texture at binding 0", program->textures[0].binding, 0);
	expect("texture size not read", program->textures[0].size_fau_word, MALI_SHADER_NO_FAU_WORD);
	expect("no buffers", program->buffer_count, 0);
	expect("no FAU words", program->fau_words, 0);
	expect("colour 0 written", program->color_mask, 1);
	expect("no discard", program->discards, 0);
	expect("no varyings read", program->input_mask, 0);
	expect("pixel position read", program->reads_frag_coord, 1);
	expect("code present", program->length != 0U, 1);
	expect("registers fit the descriptor", program->work_registers <= 64U, 1);

	/* What the descriptor gets. */
	printf("program: %zu bytes, %u registers, preload %04x\n", program->length, program->work_registers, program->preload);
	mali_shader_program_free(program);

	/* The report. */
	if (failures != 0) {
		printf("%d of %d checks failed\n", failures, checks);
		return 1;
	}

	printf("all checks passed (%d)\n", checks);
	return 0;
}

/* Compares one value and records a failure. */
static void
expect(
	const char *name,
	uint64_t got,
	uint64_t want)
{
	/* One check more; a difference is said and counted. */
	checks++;
	if (got != want) {
		printf("FAIL %s: got %llx want %llx\n", name, (unsigned long long)got, (unsigned long long)want);
		failures++;
	}
}

/* Writes the SPIR-V words to a file, little-endian as the module's own byte order. */
static int
write_spirv(
	const char *path)
{
	FILE *file;
	size_t written;

	/* The file. */
	file = fopen(path, "wb");
	if (file == NULL) {
		printf("FAIL cannot write %s\n", path);
		return 1;
	}

	/* Every word. */
	written = fwrite(mali_preload_spirv, sizeof(mali_preload_spirv[0]), sizeof(mali_preload_spirv) / sizeof(mali_preload_spirv[0]), file);
	fclose(file);
	if (written != sizeof(mali_preload_spirv) / sizeof(mali_preload_spirv[0])) {
		printf("FAIL short write of %s\n", path);
		return 1;
	}

	/* Succeeded: the file holds the module. */
	return 0;
}
