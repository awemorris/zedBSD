/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host check of the executor's own programs
 * (src/drivers/gpu/mali/vk/vk-meta-spirv.c): the SPIR-V the driver
 * carries compiles with the Mali Valhall compiler, and each program has
 * the shape vk-meta.c writes descriptors for -- the blit a compute
 * program of eight by eight threads reading 52 bytes of push constants
 * and two storage buffers (set 0, bindings 0 and 1) whose lengths it
 * does not read; the clear's vertex program the position program alone
 * from the vertex index; the clear's fragment program one colour at the
 * location the module is moved to, for each of the eight; the aspect
 * copy a compute program of eight by eight threads reading 52 bytes of
 * push constants and two storage buffers like the blit's.  The
 * conversion of a depth to a 16- and 24-bit normalized value is checked
 * against values worked by hand.  The modules (the colour one at
 * location 0 and 3) are written to the directory named by the argument,
 * when one is given, for a decode with the compiler's host test.
 *
 * cc -std=gnu11 -DKERN_USER_ABI_LP64 -Iinclude -Isrc -I. -idirafter libc/include -Wall -Wextra -Werror \
 *    tests/mali-vk-meta-host-test.c src/drivers/gpu/mali/vk/vk-meta-spirv.c \
 *    src/drivers/gpu/mali/compiler/va-compile.c src/drivers/gpu/mali/compiler/va-encode.c \
 *    src/drivers/gpu/mali/compiler/va-schedule.c src/drivers/gpu/compiler/spirv.c \
 *    src/drivers/gpu/compiler/optimize.c -lm -o <out> && <out> [directory]
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "drivers/gpu/mali/compiler/shader.h"
#include "drivers/gpu/mali/vk/vk-meta-spirv.h"

/* The colour locations a clear writes. */
#define LOCATIONS	8U

/* How many checks failed and ran. */
static int failures;
static int checks;

/* The kernel's allocator, as the compiler calls it, on the host's. */
void *kern_malloc(size_t size);
void *kern_calloc(size_t count, size_t size);
void kern_free(void *pointer);

static void expect(const char *name, uint64_t got, uint64_t want);
static struct mali_shader_program *compile(const char *name, const uint32_t *words, size_t count, uint32_t stage);
static void check_blit(void);
static void check_aspect(void);
static void check_unorm(void);
static void check_clear_vertex(void);
static void check_clear_color(uint32_t location);
static void write_module(const char *directory, const char *name, const uint32_t *words, size_t count);

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

/* Compiles every program, checks its shape and reports the count. */
int
main(
	int argc,
	char **argv)
{
	uint32_t moved[512];
	uint32_t location;
	int error;

	/* The modules, for the compiler's own host test to decode, when asked. */
	if (argc > 1) {
		write_module(argv[1], "meta-blit.spv", mali_meta_blit_spirv, mali_meta_blit_words);
		write_module(argv[1], "meta-clear-vertex.spv", mali_meta_clear_vertex_spirv, mali_meta_clear_vertex_words);
		write_module(argv[1], "meta-clear-color-0.spv", mali_meta_clear_color_spirv, mali_meta_clear_color_words);
		write_module(argv[1], "meta-aspect.spv", mali_meta_aspect_spirv, mali_meta_aspect_words);
		error = mali_meta_clear_color_at(moved, sizeof(moved) / sizeof(moved[0]), 3);
		expect("colour moved for the file", (uint64_t)error, 0);
		if (error == 0)
			write_module(argv[1], "meta-clear-color-3.spv", moved, mali_meta_clear_color_words);
	}

	/* The location change refuses a short buffer. */
	error = mali_meta_clear_color_at(moved, 4, 0);
	expect("short buffer refused", (uint64_t)error, (uint64_t)-1);

	/* Each program. */
	check_blit();
	check_aspect();
	check_clear_vertex();
	for (location = 0; location < LOCATIONS; location++)
		check_clear_color(location);
	check_unorm();

	/* The report. */
	if (failures != 0) {
		printf("%d of %d checks failed\n", failures, checks);
		return 1;
	}

	printf("all checks passed (%d)\n", checks);
	return 0;
}

/* Compiles one module, saying a refusal; NULL then. */
static struct mali_shader_program *
compile(
	const char *name,
	const uint32_t *words,
	size_t count,
	uint32_t stage)
{
	struct mali_shader_program *program;
	struct mali_shader_diagnostic diagnostic = { 0 };
	int error;

	/* The program. */
	program = NULL;
	error = mali_shader_compile(words, count, stage, NULL, &program, &diagnostic);
	expect(name, (uint64_t)error, 0);
	if (error != 0 || program == NULL) {
		printf("FAIL compile %s (%d) at %u: %s\n", name, error, diagnostic.instruction, diagnostic.reason != NULL ? diagnostic.reason : "no reason");
		return NULL;
	}

	/* Succeeded: the program, described. */
	printf("%s: %zu bytes, %u registers, preload %04x, %u FAU words\n", name, program->length, program->work_registers, program->preload, program->fau_words);
	return program;
}

/* Checks the blit's shape. */
static void
check_blit(void)
{
	struct mali_shader_program *program;
	uint32_t index;
	uint32_t bindings;

	/* The compute program. */
	program = compile("blit", mali_meta_blit_spirv, mali_meta_blit_words, MALI_SHADER_COMPUTE);
	if (program == NULL)
		return;

	/* Its workgroup, its push constants, nothing else. */
	expect("blit compute", program->stage, MALI_SHADER_COMPUTE);
	expect("blit group x", program->local_size[0], MALI_META_BLIT_GROUP);
	expect("blit group y", program->local_size[1], MALI_META_BLIT_GROUP);
	expect("blit group z", program->local_size[2], 1);
	expect("blit push", program->push_bytes, MALI_META_BLIT_PUSH_BYTES);
	expect("blit no textures", program->texture_count, 0);
	expect("blit no images", program->image_count, 0);
	expect("blit no local storage", program->tls_bytes, 0);
	expect("blit no shared memory", program->shared_bytes, 0);
	expect("blit FAU fits", program->fau_words <= 64U, 1);
	expect("blit registers fit", program->work_registers <= 64U, 1);

	/* Its two storage buffers at set 0, bindings 0 and 1, inside the FAU words, lengths not read. */
	expect("blit two buffers", program->buffer_count, 2);
	bindings = 0;
	for (index = 0; index < program->buffer_count && index < 2U; index++) {
		expect("blit buffer set", program->buffers[index].set, 0);
		expect("blit buffer storage", program->buffers[index].kind, MALI_SHADER_BUFFER_STORAGE);
		expect("blit buffer in FAU", program->buffers[index].fau_word + 2U <= program->fau_words, 1);
		expect("blit buffer after push", program->buffers[index].fau_word >= MALI_META_BLIT_PUSH_BYTES / 4U, 1);
		expect("blit length not read", program->buffers[index].size_fau_word, MALI_SHADER_NO_FAU_WORD);
		if (program->buffers[index].binding < 2U)
			bindings |= 1U << program->buffers[index].binding;
	}

	expect("blit bindings 0 and 1", bindings, 3);
	mali_shader_program_free(program);
}

/* Checks the aspect copy's shape: the blit's, with its own push constants. */
static void
check_aspect(void)
{
	struct mali_shader_program *program;
	uint32_t index;
	uint32_t bindings;

	/* The compute program. */
	program = compile("aspect", mali_meta_aspect_spirv, mali_meta_aspect_words, MALI_SHADER_COMPUTE);
	if (program == NULL)
		return;

	/* Its workgroup, its push constants, nothing else. */
	expect("aspect compute", program->stage, MALI_SHADER_COMPUTE);
	expect("aspect group x", program->local_size[0], MALI_META_ASPECT_GROUP);
	expect("aspect group y", program->local_size[1], MALI_META_ASPECT_GROUP);
	expect("aspect group z", program->local_size[2], 1);
	expect("aspect push", program->push_bytes, MALI_META_ASPECT_PUSH_BYTES);
	expect("aspect no textures", program->texture_count, 0);
	expect("aspect no images", program->image_count, 0);
	expect("aspect no local storage", program->tls_bytes, 0);
	expect("aspect no shared memory", program->shared_bytes, 0);
	expect("aspect FAU fits", program->fau_words <= 64U, 1);
	expect("aspect registers fit", program->work_registers <= 64U, 1);

	/* Its two storage buffers at set 0, bindings 0 and 1, inside the FAU words, lengths not read. */
	expect("aspect two buffers", program->buffer_count, 2);
	bindings = 0;
	for (index = 0; index < program->buffer_count && index < 2U; index++) {
		expect("aspect buffer set", program->buffers[index].set, 0);
		expect("aspect buffer storage", program->buffers[index].kind, MALI_SHADER_BUFFER_STORAGE);
		expect("aspect buffer in FAU", program->buffers[index].fau_word + 2U <= program->fau_words, 1);
		expect("aspect buffer after push", program->buffers[index].fau_word >= MALI_META_ASPECT_PUSH_BYTES / 4U, 1);
		expect("aspect length not read", program->buffers[index].size_fau_word, MALI_SHADER_NO_FAU_WORD);
		if (program->buffers[index].binding < 2U)
			bindings |= 1U << program->buffers[index].binding;
	}

	expect("aspect bindings 0 and 1", bindings, 3);
	mali_shader_program_free(program);
}

/*
 * Checks the depth conversion against values worked by hand: the ends,
 * a half (0.5 * 65535 = 32767.5 rounds up), a quarter, three quarters
 * (12582911.25 rounds down), the largest float below one, the smallest
 * normal float, and the values clamped (negative, above one, NaN,
 * infinity).
 */
static void
check_unorm(void)
{
	/* Sixteen bits. */
	expect("unorm16 0.0", mali_meta_unorm(0x00000000U, 0xffffU), 0);
	expect("unorm16 1.0", mali_meta_unorm(0x3f800000U, 0xffffU), 0xffff);
	expect("unorm16 0.5", mali_meta_unorm(0x3f000000U, 0xffffU), 0x8000);
	expect("unorm16 0.25", mali_meta_unorm(0x3e800000U, 0xffffU), 0x4000);
	expect("unorm16 1/65535", mali_meta_unorm(0x37800080U, 0xffffU), 1);
	expect("unorm16 0.4/65535", mali_meta_unorm(0x36cccccdU, 0xffffU), 0);
	expect("unorm16 0.6/65535", mali_meta_unorm(0x3719999aU, 0xffffU), 1);

	/* Twenty-four bits. */
	expect("unorm24 0.0", mali_meta_unorm(0x00000000U, 0xffffffU), 0);
	expect("unorm24 1.0", mali_meta_unorm(0x3f800000U, 0xffffffU), 0xffffff);
	expect("unorm24 0.5", mali_meta_unorm(0x3f000000U, 0xffffffU), 0x800000);
	expect("unorm24 0.75", mali_meta_unorm(0x3f400000U, 0xffffffU), 0xbfffff);
	expect("unorm24 just below 1", mali_meta_unorm(0x3f7fffffU, 0xffffffU), 0xfffffe);
	expect("unorm24 smallest normal", mali_meta_unorm(0x00800000U, 0xffffffU), 0);

	/* Clamped and refused values. */
	expect("unorm24 -0.5", mali_meta_unorm(0xbf000000U, 0xffffffU), 0);
	expect("unorm24 -0.0", mali_meta_unorm(0x80000000U, 0xffffffU), 0);
	expect("unorm24 2.0", mali_meta_unorm(0x40000000U, 0xffffffU), 0xffffff);
	expect("unorm24 infinity", mali_meta_unorm(0x7f800000U, 0xffffffU), 0xffffff);
	expect("unorm24 NaN", mali_meta_unorm(0x7fc00000U, 0xffffffU), 0);
	expect("unorm16 denormal", mali_meta_unorm(0x00000001U, 0xffffU), 0);
}

/* Checks the clear's vertex program. */
static void
check_clear_vertex(void)
{
	struct mali_shader_program *program;

	/* The vertex program. */
	program = compile("clear vertex", mali_meta_clear_vertex_spirv, mali_meta_clear_vertex_words, MALI_SHADER_VERTEX);
	if (program == NULL)
		return;

	/* The position program alone, from the vertex index and the rectangle, with the viewport words. */
	expect("clear vertex stage", program->stage, MALI_SHADER_VERTEX);
	expect("clear vertex push", program->push_bytes, MALI_META_CLEAR_PUSH_BYTES);
	expect("clear vertex no buffers", program->buffer_count, 0);
	expect("clear vertex no textures", program->texture_count, 0);
	expect("clear vertex no attributes", program->attribute_mask, 0);
	expect("clear vertex position only", program->varying_offset, 0);
	expect("clear vertex no point size", program->writes_point_size, 0);
	expect("clear vertex reads its index", program->reads_vertex_index, 1);
	expect("clear vertex viewport in FAU", program->viewport_fau_word + 6U <= program->fau_words, 1);
	expect("clear vertex viewport after push", program->viewport_fau_word >= MALI_META_CLEAR_PUSH_BYTES / 4U, 1);
	expect("clear vertex no local storage", program->tls_bytes, 0);
	mali_shader_program_free(program);
}

/* Checks the clear's fragment program moved to one location. */
static void
check_clear_color(
	uint32_t location)
{
	struct mali_shader_program *program;
	uint32_t moved[512];
	char name[32];
	int error;

	/* The module with its colour at the location. */
	error = mali_meta_clear_color_at(moved, sizeof(moved) / sizeof(moved[0]), location);
	expect("colour moved", (uint64_t)error, 0);
	if (error != 0)
		return;

	/* The fragment program. */
	snprintf(name, sizeof(name), "clear colour %u", location);
	program = compile(name, moved, mali_meta_clear_color_words, MALI_SHADER_FRAGMENT);
	if (program == NULL)
		return;

	/* One colour at the location from the push constants, nothing read, no discard. */
	expect("clear colour stage", program->stage, MALI_SHADER_FRAGMENT);
	expect("clear colour location", program->color_mask, 1U << location);
	expect("clear colour push", program->push_bytes, MALI_META_CLEAR_PUSH_BYTES);
	expect("clear colour FAU", program->fau_words >= MALI_META_CLEAR_PUSH_BYTES / 4U, 1);
	expect("clear colour no buffers", program->buffer_count, 0);
	expect("clear colour no textures", program->texture_count, 0);
	expect("clear colour no inputs", program->input_mask, 0);
	expect("clear colour no discard", program->discards, 0);
	expect("clear colour no local storage", program->tls_bytes, 0);
	mali_shader_program_free(program);
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

/* Writes a module's words to a file of the directory, little-endian as the module's own byte order. */
static void
write_module(
	const char *directory,
	const char *name,
	const uint32_t *words,
	size_t count)
{
	char path[512];
	FILE *file;
	size_t written;

	/* The file. */
	snprintf(path, sizeof(path), "%s/%s", directory, name);
	file = fopen(path, "wb");
	if (file == NULL) {
		expect("module file opened", 0, 1);
		return;
	}

	/* Every word. */
	written = fwrite(words, sizeof(words[0]), count, file);
	fclose(file);
	expect("module file written", written, count);
}
