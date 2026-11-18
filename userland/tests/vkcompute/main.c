/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * vkcompute: the Vulkan compute tests.
 *
 * Run with no argument, it is the smallest compute test.  It creates an
 * instance and a device on the first physical device, dispatches the fill
 * shader (data[x] = 3x + 7, local size 32) over one host-visible storage
 * buffer of 4096 words in 128 workgroups, waits on a fence, maps the memory
 * and counts the words that hold the expected value.  It prints one line:
 *
 *   vkcompute: <device>: PASS, 4096/4096 words
 *   vkcompute: <device>: FAIL, <n>/4096 words (first bad <i>: <value>, expected <value>)
 *   vkcompute: <device>: FAIL, <call> <VkResult>
 *   vkcompute: no Vulkan device (<call> <VkResult>)
 *
 * Run with options, it is the oracle: a set of compute shaders, each of
 * which exercises one property of the compute path, and whose results are
 * compared word by word with what a reference computation in C expects.
 * The shaders come in three stages:
 *
 *   stage 1  storage buffers, workgroup and invocation IDs, push constants,
 *            integer and fp32 arithmetic, branches and loops
 *   stage 2  stage 1 and shared memory with barriers, filling 1, 4 and
 *            16 KiB so that a device reporting one of those sizes is held
 *            to it
 *   stage 3  stage 2 and 32-bit integer atomics on buffers and shared memory
 *
 * The options are:
 *
 *   -a        every stage
 *   -s N      stage N (1, 2 or 3); may be given more than once
 *   -t NAME   the shader NAME; may be given more than once
 *   -l        lists the shaders and exits
 *   -n        negative control: one expected word of each shader is made
 *             wrong on purpose, so that every shader that runs must FAIL
 *
 * Each shader prints one line, then each stage its totals and the run its
 * verdict:
 *
 *   vkcompute: s1-copy: PASS, 4160/4160 words
 *   vkcompute: s1-copy: FAIL, 4159/4160 words (buffer 1 word 17: 0x..., expected 0x...)
 *   vkcompute: s1-copy: FAIL, vkWaitForFences -4
 *   vkcompute: s1-copy: UNSUPPORTED, vkCreateComputePipelines -13
 *   vkcompute: s2-shared-16k: BEYOND LIMITS, maxComputeSharedMemorySize 1024 < 16384
 *   vkcompute: <device>: stage 2: 6 PASS, 0 FAIL, 0 UNSUPPORTED, 0 not run, 2 beyond limits
 *   vkcompute: <device>: oracle PASS, 36/36 within limits, 2 beyond limits
 *
 * The device is judged within the limits it reports.  A shader that needs
 * more than the device reports (a larger workgroup, more shared memory, more
 * buffers or sets or push constants) is BEYOND LIMITS: it is not run and
 * does not count against the device.  Within the limits:
 *
 *   UNSUPPORTED  the driver refused the shader or its layout when the
 *                pipeline was made, although the limits promise it;
 *   FAIL         the shader ran and left a wrong word, or a submit failed;
 *   not run      the device stopped answering before the shader's turn.
 *
 * All three count against the device, so that a limit reported larger than
 * what the device can really do shows up as a failure.  The verdict is PASS
 * and the exit status 0 only when every selected shader within the limits
 * passed.
 *
 * Run with -g first, it is the graphics checks instead (graphics.inc): draws
 * whose shaders use what OpenGL asks of a driver's shader compiler beyond
 * compute -- large uniform blocks, sampling with gather, the fragment
 * shader's depth and sample mask, a vertex shader's atomics -- each
 * compared pixel by pixel with C:
 *
 *   vkcompute -g [-n] [NAME]...
 *   vkcompute: g-gather: PASS, 4096/4096 words
 *   vkcompute: <device>: graphics PASS, 6/6
 */

#include <vulkan/vulkan.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shader.h"
#include "oracle-shaders.h"

/* The shader's workgroup size (local_size_x in fill.comp). */
#define VKCOMPUTE_LOCAL_SIZE	32U

/* The workgroups dispatched; 128 is the most one submit accepts on PowerVR. */
#define VKCOMPUTE_GROUPS	128U

/* The words the dispatch writes. */
#define VKCOMPUTE_WORDS		(VKCOMPUTE_LOCAL_SIZE * VKCOMPUTE_GROUPS)

/* The shader's multiplier and addend. */
#define VKCOMPUTE_MULTIPLIER	3U
#define VKCOMPUTE_ADDEND	7U

/* The fence wait's limit in nanoseconds (5 s, above the driver's 2 s). */
#define VKCOMPUTE_WAIT_NS	5000000000ULL

/* The most storage buffers, descriptor sets, dispatches and push-constant words an oracle shader uses. */
#define ORACLE_MAX_BUFFERS	4U
#define ORACLE_MAX_SETS		2U
#define ORACLE_MAX_PASSES	3U
#define ORACLE_MAX_PUSH_WORDS	4U

/* The value every buffer word holds before a shader runs; no shader writes it. */
#define ORACLE_SENTINEL		0xa5a5a5a5U

/* The words after each buffer's declared end that must still hold the sentinel afterwards. */
#define ORACLE_MARGIN_WORDS	64U

/* The most invocations an atomic shader returns old values to. */
#define ORACLE_MAX_INVOCATIONS	1024U

/* The highest stage. */
#define ORACLE_STAGES		3U

/* The most shaders one command line names with -t. */
#define ORACLE_MAX_NAMES	64U

/* The fp32 tolerance of division (Vulkan allows 2.5 ULP) and of square root (inherited from 1 / inversesqrt). */
#define ORACLE_DIVIDE_ULPS	3U
#define ORACLE_SQRT_ULPS	5U

/* The room for the line that names the limit a shader exceeds. */
#define ORACLE_REASON_BYTES	128U

/* The usage line printed for a bad option. */
#define ORACLE_USAGE		"usage: vkcompute [-a] [-s 1|2|3]... [-t NAME]... [-l] [-n] | -g [-n] [NAME]...\n"

/* Marks an argument a function must take but does not read. */
#define UNUSED_PARAMETER(name)	((void)(name))

/*
 * What one oracle shader leaves behind.
 *
 * UNSUPPORTED is kept apart from FAIL: a driver that refuses a shader has
 * said so, while one that runs it and writes a wrong word has not.
 */
enum oracle_outcome {
	ORACLE_PASS,
	ORACLE_FAIL,
	ORACLE_UNSUPPORTED
};

/* The options of the oracle's command line. */
enum oracle_option {
	ORACLE_OPTION_ALL,
	ORACLE_OPTION_STAGE,
	ORACLE_OPTION_TEST,
	ORACLE_OPTION_LIST,
	ORACLE_OPTION_NEGATIVE,
	ORACLE_OPTION_UNKNOWN
};

/*
 * The Vulkan objects of one run.
 *
 * Every handle starts as VK_NULL_HANDLE and is destroyed by
 * vkcompute_destroy() whether or not the run got as far as creating it.
 */
struct vkcompute {
	VkInstance instance;
	VkPhysicalDevice physical;
	VkDevice device;
	VkQueue queue;
	uint32_t family;
	VkBuffer buffer;
	VkDeviceMemory memory;
	VkShaderModule module;
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout pipeline_layout;
	VkPipeline pipeline;
	VkDescriptorPool pool;
	VkDescriptorSet set;
	VkCommandPool command_pool;
	VkCommandBuffer command;
	VkFence fence;
	char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
	const char *failed_call;
};

/*
 * One storage buffer of an oracle shader: where the shader finds it and how
 * many 32-bit words it declares.  The buffer is made ORACLE_MARGIN_WORDS
 * longer, so that a write past the end is seen.
 */
struct oracle_buffer {
	uint32_t set;
	uint32_t binding;
	uint32_t words;
};

/* The push constants of one dispatch. */
struct oracle_pass {
	uint32_t push[ORACLE_MAX_PUSH_WORDS];
};

/*
 * The comparison of one shader's buffers with the reference.
 *
 * Every word the reference decides goes through oracle_expect() or
 * oracle_expect_near(), which count it and keep the first mismatch.  With
 * corrupt set, the first expected word is made wrong on purpose (the
 * negative control), so a checker that compares nothing cannot pass.
 */
struct oracle_check {
	uint32_t compared;
	uint32_t bad;
	uint32_t first_buffer;
	uint32_t first_index;
	uint32_t first_got;
	uint32_t first_expected;
	int corrupt;
};

/*
 * One oracle shader: its SPIR-V, its dispatch, its buffers, and the C
 * functions that fill the inputs and compute and compare the expected
 * words.  The local size and the shared bytes repeat what the shader
 * declares, so that the run can compare them with the device's limits.
 */
struct oracle_test {
	const char *name;
	uint32_t stage;
	const uint32_t *code;
	size_t code_size;
	uint32_t local[3];
	uint32_t shared_bytes;
	uint32_t groups[3];
	uint32_t buffer_count;
	struct oracle_buffer buffers[ORACLE_MAX_BUFFERS];
	uint32_t push_words;
	uint32_t pass_count;
	struct oracle_pass passes[ORACLE_MAX_PASSES];
	void (*fill)(const struct oracle_test *test, uint32_t *const *words);
	void (*check)(struct oracle_check *check, const struct oracle_test *test, uint32_t *const *words);
};

/*
 * The objects of one oracle shader's run, destroyed by oracle_destroy()
 * however far the run got.  The buffers stay mapped from creation to
 * destruction (host-coherent memory needs no flush).
 */
struct oracle_objects {
	VkBuffer buffers[ORACLE_MAX_BUFFERS];
	VkDeviceMemory memories[ORACLE_MAX_BUFFERS];
	uint32_t *words[ORACLE_MAX_BUFFERS];
	VkShaderModule module;
	VkDescriptorSetLayout set_layouts[ORACLE_MAX_SETS];
	uint32_t set_count;
	VkPipelineLayout pipeline_layout;
	VkPipeline pipeline;
	VkDescriptorPool pool;
	VkDescriptorSet sets[ORACLE_MAX_SETS];
	VkCommandPool command_pool;
	VkCommandBuffer command;
	VkFence fence;
	const char *failed_call;
};

/*
 * The outcomes counted for one stage.  The first four are the shaders
 * within the device's reported limits, which the device is judged by;
 * beyond_limits counts the shaders that need more than it reports.
 */
struct oracle_tally {
	uint32_t passed;
	uint32_t failed;
	uint32_t unsupported;
	uint32_t not_run;
	uint32_t beyond_limits;
};

/* What the command line selects. */
struct oracle_selection {
	int stages[ORACLE_STAGES + 1];
	const char *names[ORACLE_MAX_NAMES];
	uint32_t name_count;
	int list;
	int corrupt;
};

/* The fill and check functions the shader table names (declared before it, as ANSI C needs). */
static void oracle_fill_none(const struct oracle_test *test, uint32_t *const *words);
static void oracle_fill_inputs(const struct oracle_test *test, uint32_t *const *words);
static void oracle_fill_div(const struct oracle_test *test, uint32_t *const *words);
static void oracle_fill_float_arith(const struct oracle_test *test, uint32_t *const *words);
static void oracle_fill_float_convert(const struct oracle_test *test, uint32_t *const *words);
static void oracle_fill_float_div(const struct oracle_test *test, uint32_t *const *words);
static void oracle_fill_chain(const struct oracle_test *test, uint32_t *const *words);
static void oracle_fill_zero(const struct oracle_test *test, uint32_t *const *words);
static void oracle_fill_buffer_minmax(const struct oracle_test *test, uint32_t *const *words);
static void oracle_fill_buffer_bits(const struct oracle_test *test, uint32_t *const *words);
static void oracle_check_copy(struct oracle_check *check, const struct oracle_test *test, uint32_t *const *words);
static void oracle_check_int_alu(struct oracle_check *check, const struct oracle_test *test, uint32_t *const *words);
static void oracle_check_int_div(struct oracle_check *check, const struct oracle_test *test, uint32_t *const *words);
static void oracle_check_int_cmp(struct oracle_check *check, const struct oracle_test *test, uint32_t *const *words);
static void oracle_check_float_arith(struct oracle_check *check, const struct oracle_test *test, uint32_t *const *words);
static void oracle_check_float_convert(struct oracle_check *check, const struct oracle_test *test, uint32_t *const *words);
static void oracle_check_float_div(struct oracle_check *check, const struct oracle_test *test, uint32_t *const *words);
static void oracle_check_ids(struct oracle_check *check, const struct oracle_test *test, uint32_t *const *words);
static void oracle_check_num_groups(struct oracle_check *check, const struct oracle_test *test, uint32_t *const *words);
static void oracle_check_push(struct oracle_check *check, const struct oracle_test *test, uint32_t *const *words);
static void oracle_check_buffers(struct oracle_check *check, const struct oracle_test *test, uint32_t *const *words);
static void oracle_check_sets(struct oracle_check *check, const struct oracle_test *test, uint32_t *const *words);
static void oracle_check_size(struct oracle_check *check, const struct oracle_test *test, uint32_t *const *words);
static void oracle_check_dispatches(struct oracle_check *check, const struct oracle_test *test, uint32_t *const *words);
static void oracle_check_chain(struct oracle_check *check, const struct oracle_test *test, uint32_t *const *words);
static void oracle_check_branch(struct oracle_check *check, const struct oracle_test *test, uint32_t *const *words);
static void oracle_check_loop(struct oracle_check *check, const struct oracle_test *test, uint32_t *const *words);
static void oracle_check_neighbor(struct oracle_check *check, const struct oracle_test *test, uint32_t *const *words);
static void oracle_check_reduce(struct oracle_check *check, const struct oracle_test *test, uint32_t *const *words);
static void oracle_check_scan(struct oracle_check *check, const struct oracle_test *test, uint32_t *const *words);
static void oracle_check_shared_rows(struct oracle_check *check, const struct oracle_test *test, uint32_t *const *words);
static void oracle_check_transpose(struct oracle_check *check, const struct oracle_test *test, uint32_t *const *words);
static void oracle_check_shared_float(struct oracle_check *check, const struct oracle_test *test, uint32_t *const *words);
static void oracle_check_buffer_add(struct oracle_check *check, const struct oracle_test *test, uint32_t *const *words);
static void oracle_check_buffer_minmax(struct oracle_check *check, const struct oracle_test *test, uint32_t *const *words);
static void oracle_check_buffer_bits(struct oracle_check *check, const struct oracle_test *test, uint32_t *const *words);
static void oracle_check_buffer_exchange(struct oracle_check *check, const struct oracle_test *test, uint32_t *const *words);
static void oracle_check_buffer_compswap(struct oracle_check *check, const struct oracle_test *test, uint32_t *const *words);
static void oracle_check_shared_add(struct oracle_check *check, const struct oracle_test *test, uint32_t *const *words);
static void oracle_check_shared_minmax(struct oracle_check *check, const struct oracle_test *test, uint32_t *const *words);
static void oracle_check_shared_xchg(struct oracle_check *check, const struct oracle_test *test, uint32_t *const *words);

/*
 * The oracle's shaders, in the order they run.
 *
 * Each entry repeats the local size and the shared bytes that its shader
 * (the .comp file of the same name under oracle/) declares, then gives its
 * dispatch, its buffers (set, binding, words) and the push constants of
 * each dispatch.  Within a stage the simpler shaders come first, so that
 * the first failure points at the most basic broken piece.  The table is
 * constant for the life of the program.
 */
static const struct oracle_test oracle_tests[] = {
	{
		"s1-copy", 1, vkcompute_oracle_s1_copy, sizeof(vkcompute_oracle_s1_copy),
		{ 64, 1, 1 }, 0, { 64, 1, 1 },
		2, { { 0, 0, 4096 }, { 0, 1, 4096 } },
		0, 1, { { { 0 } } },
		oracle_fill_inputs, oracle_check_copy
	},
	{
		"s1-int-alu", 1, vkcompute_oracle_s1_int_alu, sizeof(vkcompute_oracle_s1_int_alu),
		{ 64, 1, 1 }, 0, { 16, 1, 1 },
		3, { { 0, 0, 1024 }, { 0, 1, 1024 }, { 0, 2, 10240 } },
		0, 1, { { { 0 } } },
		oracle_fill_inputs, oracle_check_int_alu
	},
	{
		"s1-int-div", 1, vkcompute_oracle_s1_int_div, sizeof(vkcompute_oracle_s1_int_div),
		{ 64, 1, 1 }, 0, { 16, 1, 1 },
		3, { { 0, 0, 1024 }, { 0, 1, 1024 }, { 0, 2, 4096 } },
		0, 1, { { { 0 } } },
		oracle_fill_div, oracle_check_int_div
	},
	{
		"s1-int-cmp", 1, vkcompute_oracle_s1_int_cmp, sizeof(vkcompute_oracle_s1_int_cmp),
		{ 64, 1, 1 }, 0, { 16, 1, 1 },
		3, { { 0, 0, 1024 }, { 0, 1, 1024 }, { 0, 2, 8192 } },
		0, 1, { { { 0 } } },
		oracle_fill_inputs, oracle_check_int_cmp
	},
	{
		"s1-float-arith", 1, vkcompute_oracle_s1_float_arith, sizeof(vkcompute_oracle_s1_float_arith),
		{ 64, 1, 1 }, 0, { 16, 1, 1 },
		3, { { 0, 0, 1024 }, { 0, 1, 1024 }, { 0, 2, 10240 } },
		0, 1, { { { 0 } } },
		oracle_fill_float_arith, oracle_check_float_arith
	},
	{
		"s1-float-convert", 1, vkcompute_oracle_s1_float_convert, sizeof(vkcompute_oracle_s1_float_convert),
		{ 64, 1, 1 }, 0, { 16, 1, 1 },
		3, { { 0, 0, 1024 }, { 0, 1, 1024 }, { 0, 2, 6144 } },
		0, 1, { { { 0 } } },
		oracle_fill_float_convert, oracle_check_float_convert
	},
	{
		"s1-float-div", 1, vkcompute_oracle_s1_float_div, sizeof(vkcompute_oracle_s1_float_div),
		{ 64, 1, 1 }, 0, { 16, 1, 1 },
		3, { { 0, 0, 1024 }, { 0, 1, 1024 }, { 0, 2, 2048 } },
		0, 1, { { { 0 } } },
		oracle_fill_float_div, oracle_check_float_div
	},
	{
		"s1-ids-1d", 1, vkcompute_oracle_s1_ids_1d, sizeof(vkcompute_oracle_s1_ids_1d),
		{ 64, 1, 1 }, 0, { 4, 1, 1 },
		1, { { 0, 0, 2560 } },
		0, 1, { { { 0 } } },
		oracle_fill_none, oracle_check_ids
	},
	{
		"s1-ids-2d", 1, vkcompute_oracle_s1_ids_2d, sizeof(vkcompute_oracle_s1_ids_2d),
		{ 8, 4, 1 }, 0, { 3, 2, 1 },
		1, { { 0, 0, 1920 } },
		0, 1, { { { 0 } } },
		oracle_fill_none, oracle_check_ids
	},
	{
		"s1-ids-3d", 1, vkcompute_oracle_s1_ids_3d, sizeof(vkcompute_oracle_s1_ids_3d),
		{ 4, 2, 2 }, 0, { 2, 3, 2 },
		1, { { 0, 0, 1920 } },
		0, 1, { { { 0 } } },
		oracle_fill_none, oracle_check_ids
	},
	{
		"s1-num-groups", 1, vkcompute_oracle_s1_num_groups, sizeof(vkcompute_oracle_s1_num_groups),
		{ 2, 1, 1 }, 0, { 3, 2, 2 },
		1, { { 0, 0, 144 } },
		0, 1, { { { 0 } } },
		oracle_fill_none, oracle_check_num_groups
	},
	{
		"s1-push", 1, vkcompute_oracle_s1_push, sizeof(vkcompute_oracle_s1_push),
		{ 64, 1, 1 }, 0, { 64, 1, 1 },
		2, { { 0, 0, 4096 }, { 0, 1, 4096 } },
		4, 1, { { { 5, 11, 0x3f000000U, 0xa5a50000U } } },
		oracle_fill_none, oracle_check_push
	},
	{
		"s1-buffers", 1, vkcompute_oracle_s1_buffers, sizeof(vkcompute_oracle_s1_buffers),
		{ 64, 1, 1 }, 0, { 64, 1, 1 },
		4, { { 0, 0, 4096 }, { 0, 1, 4096 }, { 0, 2, 4096 }, { 0, 3, 4096 } },
		0, 1, { { { 0 } } },
		oracle_fill_inputs, oracle_check_buffers
	},
	{
		"s1-sets", 1, vkcompute_oracle_s1_sets, sizeof(vkcompute_oracle_s1_sets),
		{ 64, 1, 1 }, 0, { 64, 1, 1 },
		3, { { 0, 0, 4096 }, { 1, 0, 4096 }, { 1, 2, 4096 } },
		0, 1, { { { 0 } } },
		oracle_fill_inputs, oracle_check_sets
	},
	{
		"s1-size-1", 1, vkcompute_oracle_s1_size_1, sizeof(vkcompute_oracle_s1_size_1),
		{ 1, 1, 1 }, 0, { 128, 1, 1 },
		1, { { 0, 0, 128 } },
		0, 1, { { { 0 } } },
		oracle_fill_none, oracle_check_size
	},
	{
		"s1-size-32", 1, vkcompute_oracle_s1_size_32, sizeof(vkcompute_oracle_s1_size_32),
		{ 32, 1, 1 }, 0, { 64, 1, 1 },
		1, { { 0, 0, 2048 } },
		0, 1, { { { 0 } } },
		oracle_fill_none, oracle_check_size
	},
	{
		"s1-size-64", 1, vkcompute_oracle_s1_size_64, sizeof(vkcompute_oracle_s1_size_64),
		{ 64, 1, 1 }, 0, { 64, 1, 1 },
		1, { { 0, 0, 4096 } },
		0, 1, { { { 0 } } },
		oracle_fill_none, oracle_check_size
	},
	{
		"s1-size-128", 1, vkcompute_oracle_s1_size_128, sizeof(vkcompute_oracle_s1_size_128),
		{ 128, 1, 1 }, 0, { 32, 1, 1 },
		1, { { 0, 0, 4096 } },
		0, 1, { { { 0 } } },
		oracle_fill_none, oracle_check_size
	},
	{
		"s1-dispatches", 1, vkcompute_oracle_s1_dispatches, sizeof(vkcompute_oracle_s1_dispatches),
		{ 64, 1, 1 }, 0, { 16, 1, 1 },
		1, { { 0, 0, 4096 } },
		2, 3, { { { 0, 3 } }, { { 1024, 5 } }, { { 2048, 7 } } },
		oracle_fill_none, oracle_check_dispatches
	},
	{
		"s1-chain", 1, vkcompute_oracle_s1_chain, sizeof(vkcompute_oracle_s1_chain),
		{ 64, 1, 1 }, 0, { 64, 1, 1 },
		1, { { 0, 0, 8192 } },
		3, 3, { { { 0, 4096, 1 } }, { { 4096, 0, 2 } }, { { 0, 4096, 3 } } },
		oracle_fill_chain, oracle_check_chain
	},
	{
		"s1-branch", 1, vkcompute_oracle_s1_branch, sizeof(vkcompute_oracle_s1_branch),
		{ 64, 1, 1 }, 0, { 64, 1, 1 },
		3, { { 0, 0, 4096 }, { 0, 1, 4096 }, { 0, 2, 4096 } },
		0, 1, { { { 0 } } },
		oracle_fill_inputs, oracle_check_branch
	},
	{
		"s1-loop", 1, vkcompute_oracle_s1_loop, sizeof(vkcompute_oracle_s1_loop),
		{ 64, 1, 1 }, 0, { 64, 1, 1 },
		2, { { 0, 0, 4096 }, { 0, 1, 12288 } },
		0, 1, { { { 0 } } },
		oracle_fill_inputs, oracle_check_loop
	},
	{
		"s2-neighbor", 2, vkcompute_oracle_s2_neighbor, sizeof(vkcompute_oracle_s2_neighbor),
		{ 64, 1, 1 }, 256, { 64, 1, 1 },
		2, { { 0, 0, 4096 }, { 0, 1, 4096 } },
		0, 1, { { { 0 } } },
		oracle_fill_inputs, oracle_check_neighbor
	},
	{
		"s2-reduce", 2, vkcompute_oracle_s2_reduce, sizeof(vkcompute_oracle_s2_reduce),
		{ 128, 1, 1 }, 512, { 32, 1, 1 },
		2, { { 0, 0, 4096 }, { 0, 1, 32 } },
		0, 1, { { { 0 } } },
		oracle_fill_inputs, oracle_check_reduce
	},
	{
		"s2-scan", 2, vkcompute_oracle_s2_scan, sizeof(vkcompute_oracle_s2_scan),
		{ 64, 1, 1 }, 512, { 64, 1, 1 },
		2, { { 0, 0, 4096 }, { 0, 1, 4096 } },
		0, 1, { { { 0 } } },
		oracle_fill_inputs, oracle_check_scan
	},
	{
		"s2-shared-1k", 2, vkcompute_oracle_s2_shared_1k, sizeof(vkcompute_oracle_s2_shared_1k),
		{ 32, 1, 1 }, 1024, { 4, 1, 1 },
		2, { { 0, 0, 128 }, { 0, 1, 128 } },
		0, 1, { { { 0 } } },
		oracle_fill_inputs, oracle_check_shared_rows
	},
	{
		"s2-shared-4k", 2, vkcompute_oracle_s2_shared_4k, sizeof(vkcompute_oracle_s2_shared_4k),
		{ 64, 1, 1 }, 4096, { 4, 1, 1 },
		2, { { 0, 0, 256 }, { 0, 1, 256 } },
		0, 1, { { { 0 } } },
		oracle_fill_inputs, oracle_check_shared_rows
	},
	{
		"s2-shared-16k", 2, vkcompute_oracle_s2_shared_16k, sizeof(vkcompute_oracle_s2_shared_16k),
		{ 128, 1, 1 }, 16384, { 4, 1, 1 },
		2, { { 0, 0, 512 }, { 0, 1, 512 } },
		0, 1, { { { 0 } } },
		oracle_fill_inputs, oracle_check_shared_rows
	},
	{
		"s2-transpose", 2, vkcompute_oracle_s2_transpose, sizeof(vkcompute_oracle_s2_transpose),
		{ 16, 8, 1 }, 512, { 2, 2, 1 },
		2, { { 0, 0, 512 }, { 0, 1, 512 } },
		0, 1, { { { 0 } } },
		oracle_fill_inputs, oracle_check_transpose
	},
	{
		"s2-shared-float", 2, vkcompute_oracle_s2_shared_float, sizeof(vkcompute_oracle_s2_shared_float),
		{ 64, 1, 1 }, 256, { 64, 1, 1 },
		2, { { 0, 0, 4096 }, { 0, 1, 4096 } },
		0, 1, { { { 0 } } },
		oracle_fill_inputs, oracle_check_shared_float
	},
	{
		"s3-buffer-add", 3, vkcompute_oracle_s3_buffer_add, sizeof(vkcompute_oracle_s3_buffer_add),
		{ 64, 1, 1 }, 0, { 16, 1, 1 },
		2, { { 0, 0, 9 }, { 0, 1, 1024 } },
		0, 1, { { { 0 } } },
		oracle_fill_zero, oracle_check_buffer_add
	},
	{
		"s3-buffer-minmax", 3, vkcompute_oracle_s3_buffer_minmax, sizeof(vkcompute_oracle_s3_buffer_minmax),
		{ 64, 1, 1 }, 0, { 16, 1, 1 },
		3, { { 0, 0, 1024 }, { 0, 1, 8 }, { 0, 2, 8 } },
		0, 1, { { { 0 } } },
		oracle_fill_buffer_minmax, oracle_check_buffer_minmax
	},
	{
		"s3-buffer-bits", 3, vkcompute_oracle_s3_buffer_bits, sizeof(vkcompute_oracle_s3_buffer_bits),
		{ 64, 1, 1 }, 0, { 16, 1, 1 },
		2, { { 0, 0, 1024 }, { 0, 1, 12 } },
		0, 1, { { { 0 } } },
		oracle_fill_buffer_bits, oracle_check_buffer_bits
	},
	{
		"s3-buffer-exchange", 3, vkcompute_oracle_s3_buffer_exchange, sizeof(vkcompute_oracle_s3_buffer_exchange),
		{ 64, 1, 1 }, 0, { 16, 1, 1 },
		2, { { 0, 0, 16 }, { 0, 1, 1024 } },
		0, 1, { { { 0 } } },
		oracle_fill_zero, oracle_check_buffer_exchange
	},
	{
		"s3-buffer-compswap", 3, vkcompute_oracle_s3_buffer_compswap, sizeof(vkcompute_oracle_s3_buffer_compswap),
		{ 64, 1, 1 }, 0, { 16, 1, 1 },
		2, { { 0, 0, 16 }, { 0, 1, 1024 } },
		0, 1, { { { 0 } } },
		oracle_fill_zero, oracle_check_buffer_compswap
	},
	{
		"s3-shared-add", 3, vkcompute_oracle_s3_shared_add, sizeof(vkcompute_oracle_s3_shared_add),
		{ 128, 1, 1 }, 20, { 8, 1, 1 },
		2, { { 0, 0, 1024 }, { 0, 1, 40 } },
		0, 1, { { { 0 } } },
		oracle_fill_none, oracle_check_shared_add
	},
	{
		"s3-shared-minmax", 3, vkcompute_oracle_s3_shared_minmax, sizeof(vkcompute_oracle_s3_shared_minmax),
		{ 64, 1, 1 }, 16, { 16, 1, 1 },
		2, { { 0, 0, 1024 }, { 0, 1, 64 } },
		0, 1, { { { 0 } } },
		oracle_fill_inputs, oracle_check_shared_minmax
	},
	{
		"s3-shared-xchg", 3, vkcompute_oracle_s3_shared_xchg, sizeof(vkcompute_oracle_s3_shared_xchg),
		{ 64, 1, 1 }, 8, { 8, 1, 1 },
		2, { { 0, 0, 1024 }, { 0, 1, 16 } },
		0, 1, { { { 0 } } },
		oracle_fill_none, oracle_check_shared_xchg
	}
};

/* The number of oracle shaders. */
#define ORACLE_TEST_COUNT	(sizeof(oracle_tests) / sizeof(oracle_tests[0]))

static VkResult vkcompute_open_device(struct vkcompute *run);
static VkResult vkcompute_create_buffer(struct vkcompute *run);
static VkResult vkcompute_create_pipeline(struct vkcompute *run);
static VkResult vkcompute_bind_buffer(struct vkcompute *run);
static VkResult vkcompute_submit(struct vkcompute *run);
static int vkcompute_verify(struct vkcompute *run);
static void vkcompute_destroy(struct vkcompute *run);
static int vkcompute_fill_test(void);
static int oracle_main(int argc, char **argv);
#ifndef VKCOMPUTE_NO_GRAPHICS
static int gfx_main(int argc, char **argv);
#endif
static int oracle_parse(int argc, char **argv, struct oracle_selection *selection);
static enum oracle_option oracle_option_kind(const char *option);
static int oracle_test_named(const char *name);
static int oracle_selected(const struct oracle_test *test, const struct oracle_selection *selection);
static void oracle_list(void);
static int oracle_limit_refusal(const VkPhysicalDeviceLimits *limits, const struct oracle_test *test, char *reason);
static enum oracle_outcome oracle_run_test(struct vkcompute *run, const struct oracle_test *test, int corrupt, int *lost);
static VkResult oracle_create_buffers(struct vkcompute *run, const struct oracle_test *test, struct oracle_objects *objects);
static VkResult oracle_create_pipeline(struct vkcompute *run, const struct oracle_test *test, struct oracle_objects *objects);
static VkResult oracle_bind_buffers(struct vkcompute *run, const struct oracle_test *test, struct oracle_objects *objects);
static VkResult oracle_record(struct vkcompute *run, const struct oracle_test *test, struct oracle_objects *objects);
static VkResult oracle_submit(struct vkcompute *run, struct oracle_objects *objects);
static void oracle_compare(const struct oracle_test *test, struct oracle_objects *objects, struct oracle_check *check);
static void oracle_destroy(struct vkcompute *run, struct oracle_objects *objects);
static uint32_t oracle_memory_type(struct vkcompute *run, uint32_t type_bits);
static void oracle_expect(struct oracle_check *check, uint32_t buffer, uint32_t index, uint32_t got, uint32_t expected);
static void oracle_expect_near(struct oracle_check *check, uint32_t buffer, uint32_t index, uint32_t got, float expected, uint32_t ulps);
static void oracle_expect_untouched(struct oracle_check *check, uint32_t *const *words, uint32_t buffer, uint32_t from, uint32_t to);
static uint32_t oracle_hash(uint32_t value);
static uint32_t oracle_input(uint32_t buffer, uint32_t index);
static uint32_t oracle_float_bits(float value);
static float oracle_bits_float(uint32_t bits);
static int64_t oracle_float_order(uint32_t bits);
static double oracle_square_root(double value);
static uint32_t oracle_signed_shift(uint32_t value, uint32_t shift);
static uint32_t oracle_signed_divide(uint32_t dividend, uint32_t divisor);
static int32_t oracle_signed(uint32_t value);
static uint32_t oracle_div_divisor(uint32_t index);
static float oracle_float_operand(uint32_t buffer, uint32_t index);
static uint32_t oracle_convert_integer(uint32_t index);
static float oracle_convert_float(uint32_t index);
static float oracle_div_operand(uint32_t buffer, uint32_t index);

/*
 * Runs the fill test with no argument, the oracle with options.
 */
int
main(
	int argc,
	char **argv)
{
	int status;

	/* Keeps the smallest test, and the line the scripts read from it, as it was. */
	if (argc < 2) {
		status = vkcompute_fill_test();
		return status;
	}

#ifndef VKCOMPUTE_NO_GRAPHICS
	/* Runs the graphics checks when the first option asks for them. */
	if (strcmp(argv[1], "-g") == 0) {
		status = gfx_main(argc, argv);
		return status;
	}
#endif

	/* Runs the oracle shaders the options select. */
	status = oracle_main(argc, argv);

	/* Reports the oracle's verdict as the exit status. */
	return status;
}

/* Runs the fill shader once and prints the result line; returns the exit status. */
static int
vkcompute_fill_test(
	void)
{
	struct vkcompute run;
	VkResult status;
	int failed;

	/* Starts with no objects, so the teardown can run from any point. */
	memset(&run, 0, sizeof(run));

	/* Opens the instance and the device; with none, nothing else can run. */
	status = vkcompute_open_device(&run);
	if (status != VK_SUCCESS && run.name[0] == '\0') {
		printf("vkcompute: no Vulkan device (%s %d)\n", run.failed_call, (int)status);
		vkcompute_destroy(&run);
		return 1;
	}

	/* Builds the buffer, the pipeline and the descriptor, then dispatches. */
	if (status == VK_SUCCESS)
		status = vkcompute_create_buffer(&run);
	if (status == VK_SUCCESS)
		status = vkcompute_create_pipeline(&run);
	if (status == VK_SUCCESS)
		status = vkcompute_bind_buffer(&run);
	if (status == VK_SUCCESS)
		status = vkcompute_submit(&run);

	/* Reports the call that failed before any word could be read back. */
	if (status != VK_SUCCESS) {
		printf("vkcompute: %s: FAIL, %s %d\n", run.name, run.failed_call, (int)status);
		vkcompute_destroy(&run);
		return 1;
	}

	/* Counts the words the shader wrote and prints the verdict. */
	failed = vkcompute_verify(&run);
	vkcompute_destroy(&run);

	/* Reports the verdict as the exit status. */
	if (failed != 0)
		return 1;

	/* Succeeded: every word holds 3x + 7. */
	return 0;
}

/* Creates the instance and the device on the first physical device's compute family. */
static VkResult
vkcompute_open_device(
	struct vkcompute *run)
{
	VkApplicationInfo application;
	VkInstanceCreateInfo instance_info;
	VkPhysicalDeviceProperties properties;
	VkQueueFamilyProperties families[8];
	VkDeviceQueueCreateInfo queue_info;
	VkDeviceCreateInfo device_info;
	VkPhysicalDeviceFeatures features;
	uint32_t count;
	uint32_t index;
	float priority;
	VkResult status;

	/* Creates a Vulkan 1.0 instance with no layer and no extension. */
	memset(&application, 0, sizeof(application));
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.pApplicationName = "vkcompute";
	application.apiVersion = VK_API_VERSION_1_0;
	memset(&instance_info, 0, sizeof(instance_info));
	instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance_info.pApplicationInfo = &application;
	run->failed_call = "vkCreateInstance";
	status = vkCreateInstance(&instance_info, NULL, &run->instance);
	if (status != VK_SUCCESS)
		return status;

	/* Takes the first physical device; VK_INCOMPLETE only means there are more. */
	count = 1;
	run->failed_call = "vkEnumeratePhysicalDevices";
	status = vkEnumeratePhysicalDevices(run->instance, &count, &run->physical);
	if (status != VK_SUCCESS && status != VK_INCOMPLETE)
		return status;
	if (count == 0)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Names the device in every later line. */
	vkGetPhysicalDeviceProperties(run->physical, &properties);
	memcpy(run->name, properties.deviceName, sizeof(run->name));
	run->name[sizeof(run->name) - 1] = '\0';

	/* Finds the first family with compute. */
	count = sizeof(families) / sizeof(families[0]);
	vkGetPhysicalDeviceQueueFamilyProperties(run->physical, &count, families);
	run->failed_call = "compute queue family";
	for (index = 0; index < count; index++) {
		if ((families[index].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0)
			break;
	}

	/* Refuses a device without compute. */
	if (index == count)
		return VK_ERROR_FEATURE_NOT_PRESENT;

	/* Creates the device with one queue of that family and every feature the device reports (the graphics checks use some). */
	run->family = index;
	vkGetPhysicalDeviceFeatures(run->physical, &features);
	priority = 1.0f;
	memset(&queue_info, 0, sizeof(queue_info));
	queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue_info.queueFamilyIndex = run->family;
	queue_info.queueCount = 1;
	queue_info.pQueuePriorities = &priority;
	memset(&device_info, 0, sizeof(device_info));
	device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	device_info.queueCreateInfoCount = 1;
	device_info.pQueueCreateInfos = &queue_info;
	device_info.pEnabledFeatures = &features;
	run->failed_call = "vkCreateDevice";
	status = vkCreateDevice(run->physical, &device_info, NULL, &run->device);
	if (status != VK_SUCCESS)
		return status;

	/* Takes the queue the dispatch is submitted to. */
	vkGetDeviceQueue(run->device, run->family, 0, &run->queue);

	/* Succeeded: the device and its queue are open. */
	return VK_SUCCESS;
}

/* Creates the storage buffer in host-visible, host-coherent memory and clears it. */
static VkResult
vkcompute_create_buffer(
	struct vkcompute *run)
{
	VkBufferCreateInfo buffer_info;
	VkMemoryRequirements requirements;
	VkPhysicalDeviceMemoryProperties memory;
	VkMemoryAllocateInfo allocate_info;
	VkMemoryPropertyFlags wanted;
	uint32_t type;
	void *words;
	VkResult status;

	/* Creates a buffer of VKCOMPUTE_WORDS 32-bit words. */
	memset(&buffer_info, 0, sizeof(buffer_info));
	buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer_info.size = VKCOMPUTE_WORDS * sizeof(uint32_t);
	buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	run->failed_call = "vkCreateBuffer";
	status = vkCreateBuffer(run->device, &buffer_info, NULL, &run->buffer);
	if (status != VK_SUCCESS)
		return status;

	/* Finds a type the buffer accepts that the CPU sees without flushes. */
	vkGetBufferMemoryRequirements(run->device, run->buffer, &requirements);
	vkGetPhysicalDeviceMemoryProperties(run->physical, &memory);
	wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	for (type = 0; type < memory.memoryTypeCount; type++) {
		if ((requirements.memoryTypeBits & (1U << type)) == 0)
			continue;
		if ((memory.memoryTypes[type].propertyFlags & wanted) == wanted)
			break;
	}

	/* Refuses a device with no such type. */
	run->failed_call = "host-coherent memory type";
	if (type == memory.memoryTypeCount)
		return VK_ERROR_FEATURE_NOT_PRESENT;

	/* Allocates the memory and binds the buffer to it. */
	memset(&allocate_info, 0, sizeof(allocate_info));
	allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate_info.allocationSize = requirements.size;
	allocate_info.memoryTypeIndex = type;
	run->failed_call = "vkAllocateMemory";
	status = vkAllocateMemory(run->device, &allocate_info, NULL, &run->memory);
	if (status != VK_SUCCESS)
		return status;
	run->failed_call = "vkBindBufferMemory";
	status = vkBindBufferMemory(run->device, run->buffer, run->memory, 0);
	if (status != VK_SUCCESS)
		return status;

	/* Fills the words with a value the shader never writes, so stale memory fails. */
	run->failed_call = "vkMapMemory";
	status = vkMapMemory(run->device, run->memory, 0, VK_WHOLE_SIZE, 0, &words);
	if (status != VK_SUCCESS)
		return status;
	memset(words, 0xa5, VKCOMPUTE_WORDS * sizeof(uint32_t));
	vkUnmapMemory(run->device, run->memory);

	/* Succeeded: the buffer is bound and holds the fill pattern. */
	return VK_SUCCESS;
}

/* Creates the shader module, the layouts and the compute pipeline. */
static VkResult
vkcompute_create_pipeline(
	struct vkcompute *run)
{
	VkShaderModuleCreateInfo module_info;
	VkDescriptorSetLayoutBinding binding;
	VkDescriptorSetLayoutCreateInfo set_info;
	VkPipelineLayoutCreateInfo layout_info;
	VkComputePipelineCreateInfo pipeline_info;
	VkResult status;

	/* Creates the module from the embedded SPIR-V. */
	memset(&module_info, 0, sizeof(module_info));
	module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	module_info.codeSize = sizeof(vkcompute_fill_shader);
	module_info.pCode = vkcompute_fill_shader;
	run->failed_call = "vkCreateShaderModule";
	status = vkCreateShaderModule(run->device, &module_info, NULL, &run->module);
	if (status != VK_SUCCESS)
		return status;

	/* Describes set 0 as one storage buffer at binding 0. */
	memset(&binding, 0, sizeof(binding));
	binding.binding = 0;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	binding.descriptorCount = 1;
	binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	memset(&set_info, 0, sizeof(set_info));
	set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set_info.bindingCount = 1;
	set_info.pBindings = &binding;
	run->failed_call = "vkCreateDescriptorSetLayout";
	status = vkCreateDescriptorSetLayout(run->device, &set_info, NULL, &run->set_layout);
	if (status != VK_SUCCESS)
		return status;

	/* Creates the pipeline layout with that set and no push constant. */
	memset(&layout_info, 0, sizeof(layout_info));
	layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = &run->set_layout;
	run->failed_call = "vkCreatePipelineLayout";
	status = vkCreatePipelineLayout(run->device, &layout_info, NULL, &run->pipeline_layout);
	if (status != VK_SUCCESS)
		return status;

	/* Creates the compute pipeline from the module's main. */
	memset(&pipeline_info, 0, sizeof(pipeline_info));
	pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	pipeline_info.stage.module = run->module;
	pipeline_info.stage.pName = "main";
	pipeline_info.layout = run->pipeline_layout;
	run->failed_call = "vkCreateComputePipelines";
	status = vkCreateComputePipelines(run->device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &run->pipeline);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the pipeline is ready to bind. */
	return VK_SUCCESS;
}

/* Allocates the descriptor set and points it at the buffer. */
static VkResult
vkcompute_bind_buffer(
	struct vkcompute *run)
{
	VkDescriptorPoolSize size;
	VkDescriptorPoolCreateInfo pool_info;
	VkDescriptorSetAllocateInfo set_info;
	VkDescriptorBufferInfo buffer_info;
	VkWriteDescriptorSet write;
	VkResult status;

	/* Creates a pool for the one set. */
	memset(&size, 0, sizeof(size));
	size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	size.descriptorCount = 1;
	memset(&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.maxSets = 1;
	pool_info.poolSizeCount = 1;
	pool_info.pPoolSizes = &size;
	run->failed_call = "vkCreateDescriptorPool";
	status = vkCreateDescriptorPool(run->device, &pool_info, NULL, &run->pool);
	if (status != VK_SUCCESS)
		return status;

	/* Allocates the set. */
	memset(&set_info, 0, sizeof(set_info));
	set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	set_info.descriptorPool = run->pool;
	set_info.descriptorSetCount = 1;
	set_info.pSetLayouts = &run->set_layout;
	run->failed_call = "vkAllocateDescriptorSets";
	status = vkAllocateDescriptorSets(run->device, &set_info, &run->set);
	if (status != VK_SUCCESS)
		return status;

	/* Writes the whole buffer into binding 0. */
	memset(&buffer_info, 0, sizeof(buffer_info));
	buffer_info.buffer = run->buffer;
	buffer_info.offset = 0;
	buffer_info.range = VK_WHOLE_SIZE;
	memset(&write, 0, sizeof(write));
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = run->set;
	write.dstBinding = 0;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	write.pBufferInfo = &buffer_info;
	vkUpdateDescriptorSets(run->device, 1, &write, 0, NULL);

	/* Succeeded: the set names the buffer. */
	return VK_SUCCESS;
}

/* Records the dispatch, submits it with a fence and waits for the fence. */
static VkResult
vkcompute_submit(
	struct vkcompute *run)
{
	VkCommandPoolCreateInfo pool_info;
	VkCommandBufferAllocateInfo command_info;
	VkCommandBufferBeginInfo begin;
	VkMemoryBarrier barrier;
	VkFenceCreateInfo fence_info;
	VkSubmitInfo submit;
	VkResult status;

	/* Creates the command pool and one primary command buffer. */
	memset(&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool_info.queueFamilyIndex = run->family;
	run->failed_call = "vkCreateCommandPool";
	status = vkCreateCommandPool(run->device, &pool_info, NULL, &run->command_pool);
	if (status != VK_SUCCESS)
		return status;
	memset(&command_info, 0, sizeof(command_info));
	command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	command_info.commandPool = run->command_pool;
	command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	command_info.commandBufferCount = 1;
	run->failed_call = "vkAllocateCommandBuffers";
	status = vkAllocateCommandBuffers(run->device, &command_info, &run->command);
	if (status != VK_SUCCESS)
		return status;

	/* Records bind, dispatch and the barrier that makes the writes visible to the host. */
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	run->failed_call = "vkBeginCommandBuffer";
	status = vkBeginCommandBuffer(run->command, &begin);
	if (status != VK_SUCCESS)
		return status;
	vkCmdBindPipeline(run->command, VK_PIPELINE_BIND_POINT_COMPUTE, run->pipeline);
	vkCmdBindDescriptorSets(run->command, VK_PIPELINE_BIND_POINT_COMPUTE, run->pipeline_layout, 0, 1, &run->set, 0, NULL);
	vkCmdDispatch(run->command, VKCOMPUTE_GROUPS, 1, 1);
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	vkCmdPipelineBarrier(
		run->command,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_HOST_BIT,
		0,
		1,
		&barrier,
		0,
		NULL,
		0,
		NULL);
	run->failed_call = "vkEndCommandBuffer";
	status = vkEndCommandBuffer(run->command);
	if (status != VK_SUCCESS)
		return status;

	/* Creates the fence the submit signals. */
	memset(&fence_info, 0, sizeof(fence_info));
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	run->failed_call = "vkCreateFence";
	status = vkCreateFence(run->device, &fence_info, NULL, &run->fence);
	if (status != VK_SUCCESS)
		return status;

	/* Submits the command buffer. */
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &run->command;
	run->failed_call = "vkQueueSubmit";
	status = vkQueueSubmit(run->queue, 1, &submit, run->fence);
	if (status != VK_SUCCESS)
		return status;

	/* Waits for the dispatch; VK_TIMEOUT is reported like any failure. */
	run->failed_call = "vkWaitForFences";
	status = vkWaitForFences(run->device, 1, &run->fence, VK_TRUE, VKCOMPUTE_WAIT_NS);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the dispatch has finished. */
	return VK_SUCCESS;
}

/* Counts the words that hold 3x + 7 and prints the verdict; returns 1 on failure. */
static int
vkcompute_verify(
	struct vkcompute *run)
{
	const uint32_t *words;
	void *mapped;
	uint32_t index;
	uint32_t expected;
	uint32_t good;
	uint32_t first_bad;
	VkResult status;

	/* Maps the memory the shader wrote. */
	status = vkMapMemory(run->device, run->memory, 0, VK_WHOLE_SIZE, 0, &mapped);
	if (status != VK_SUCCESS) {
		printf("vkcompute: %s: FAIL, vkMapMemory %d\n", run->name, (int)status);
		return 1;
	}

	/* Counts the good words and remembers the first bad one. */
	words = mapped;
	good = 0;
	first_bad = VKCOMPUTE_WORDS;
	for (index = 0; index < VKCOMPUTE_WORDS; index++) {
		expected = index * VKCOMPUTE_MULTIPLIER + VKCOMPUTE_ADDEND;
		if (words[index] == expected) {
			good++;
		} else if (first_bad == VKCOMPUTE_WORDS) {
			first_bad = index;
		}
	}

	/* Prints the verdict line. */
	if (good == VKCOMPUTE_WORDS) {
		printf("vkcompute: %s: PASS, %u/%u words\n", run->name, good, VKCOMPUTE_WORDS);
	} else {
		expected = first_bad * VKCOMPUTE_MULTIPLIER + VKCOMPUTE_ADDEND;
		printf("vkcompute: %s: FAIL, %u/%u words (first bad %u: 0x%08x, expected 0x%08x)\n",
		       run->name,
		       good,
		       VKCOMPUTE_WORDS,
		       first_bad,
		       words[first_bad],
		       expected);
	}
	vkUnmapMemory(run->device, run->memory);

	/* Reports a mismatch as a failure. */
	if (good != VKCOMPUTE_WORDS)
		return 1;

	/* Succeeded: every word matched. */
	return 0;
}

/* Destroys every object the run created, newest first. */
static void
vkcompute_destroy(
	struct vkcompute *run)
{
	/* Leaves the instance alone when no device was created. */
	if (run->device != VK_NULL_HANDLE) {
		vkDeviceWaitIdle(run->device);
		vkDestroyFence(run->device, run->fence, NULL);
		vkDestroyCommandPool(run->device, run->command_pool, NULL);
		vkDestroyDescriptorPool(run->device, run->pool, NULL);
		vkDestroyPipeline(run->device, run->pipeline, NULL);
		vkDestroyPipelineLayout(run->device, run->pipeline_layout, NULL);
		vkDestroyDescriptorSetLayout(run->device, run->set_layout, NULL);
		vkDestroyShaderModule(run->device, run->module, NULL);
		vkDestroyBuffer(run->device, run->buffer, NULL);
		vkFreeMemory(run->device, run->memory, NULL);
		vkDestroyDevice(run->device, NULL);
	}

	/* Destroys the instance last. */
	if (run->instance != VK_NULL_HANDLE)
		vkDestroyInstance(run->instance, NULL);
}

/* Runs the oracle shaders the options select and prints the totals; returns the exit status. */
static int
oracle_main(
	int argc,
	char **argv)
{
	struct vkcompute run;
	struct oracle_selection selection;
	struct oracle_tally tallies[ORACLE_STAGES + 1];
	VkPhysicalDeviceProperties properties;
	char refusal[ORACLE_REASON_BYTES];
	const struct oracle_test *test;
	enum oracle_outcome outcome;
	struct oracle_tally *tally;
	uint32_t index;
	uint32_t selected;
	uint32_t within;
	uint32_t passed;
	uint32_t stage;
	uint32_t counted;
	VkResult status;
	int chosen;
	int beyond;
	int lost;
	int error;

	/* Reads the options; a bad one has already been reported. */
	error = oracle_parse(argc, argv, &selection);
	if (error != 0)
		return 2;

	/* Lists the shaders without touching the device. */
	if (selection.list != 0) {
		oracle_list();
		return 0;
	}

	/* Starts with no objects, so the teardown can run from any point. */
	memset(&run, 0, sizeof(run));
	memset(tallies, 0, sizeof(tallies));

	/* Opens the instance and the device; with none, no shader can run. */
	status = vkcompute_open_device(&run);
	if (status != VK_SUCCESS) {
		printf("vkcompute: no Vulkan device (%s %d)\n", run.failed_call, (int)status);
		vkcompute_destroy(&run);
		return 1;
	}

	/* Takes the limits every shader is compared with before it runs. */
	vkGetPhysicalDeviceProperties(run.physical, &properties);

	/* Says that every failure to come is meant, under the negative control. */
	if (selection.corrupt != 0)
		printf("vkcompute: %s: negative control: every shader within the limits must FAIL\n", run.name);

	/* Runs each selected shader once, in the table's order. */
	lost = 0;
	selected = 0;
	within = 0;
	passed = 0;
	for (index = 0; index < ORACLE_TEST_COUNT; index++) {
		test = &oracle_tests[index];
		tally = &tallies[test->stage];

		/* Skips a shader the options leave out. */
		chosen = oracle_selected(test, &selection);
		if (chosen == 0)
			continue;
		selected++;

		/* Leaves out a shader that needs more than the device reports; it does not count against the device. */
		beyond = oracle_limit_refusal(&properties.limits, test, refusal);
		if (beyond != 0) {
			printf("vkcompute: %s: BEYOND LIMITS, %s\n", test->name, refusal);
			tally->beyond_limits++;
			continue;
		}
		within++;

		/* Counts what a lost or hung device leaves without running. */
		if (lost != 0) {
			printf("vkcompute: %s: FAIL, not run (the device stopped answering)\n", test->name);
			tally->not_run++;
			continue;
		}

		/* Runs the shader, which prints its own line. */
		outcome = oracle_run_test(&run, test, selection.corrupt, &lost);

		/* Counts the outcome in its stage. */
		switch (outcome) {
		case ORACLE_PASS:
			tally->passed++;
			passed++;
			break;
		case ORACLE_UNSUPPORTED:
			tally->unsupported++;
			break;
		default:
			tally->failed++;
			break;
		}
	}

	/* Prints the totals of each stage that ran anything. */
	for (stage = 1; stage <= ORACLE_STAGES; stage++) {
		tally = &tallies[stage];
		counted = tally->passed + tally->failed + tally->unsupported + tally->not_run + tally->beyond_limits;
		if (counted == 0)
			continue;
		printf("vkcompute: %s: stage %u: %u PASS, %u FAIL, %u UNSUPPORTED, %u not run, %u beyond limits\n",
		       run.name,
		       stage,
		       tally->passed,
		       tally->failed,
		       tally->unsupported,
		       tally->not_run,
		       tally->beyond_limits);
	}

	/* Prints the verdict: PASS needs every shader within the limits to pass, and at least one to be within them. */
	if (within != 0 && passed == within) {
		printf("vkcompute: %s: oracle PASS, %u/%u within limits, %u beyond limits\n",
		       run.name,
		       passed,
		       within,
		       selected - within);
	} else {
		printf("vkcompute: %s: oracle FAIL, %u/%u within limits, %u beyond limits\n",
		       run.name,
		       passed,
		       within,
		       selected - within);
	}

	/* Leaves a hung device to the process exit: waiting for it to go idle could block for ever. */
	if (lost == 0)
		vkcompute_destroy(&run);

	/* Reports a run with anything but PASS within the limits as a failure. */
	if (within == 0)
		return 1;
	if (passed != within)
		return 1;

	/* Succeeded: every selected shader within the device's limits passed. */
	return 0;
}

/* Reads the oracle's options into a selection; returns nonzero after printing what was wrong. */
static int
oracle_parse(
	int argc,
	char **argv,
	struct oracle_selection *selection)
{
	enum oracle_option kind;
	unsigned long number;
	char *end;
	uint32_t stage;
	int argument;
	int known;

	/* Starts with nothing selected. */
	memset(selection, 0, sizeof(*selection));

	/* Reads each option and the value that follows it. */
	for (argument = 1; argument < argc; argument++) {
		kind = oracle_option_kind(argv[argument]);

		/* Refuses an option whose value is missing. */
		if (kind == ORACLE_OPTION_STAGE || kind == ORACLE_OPTION_TEST) {
			if (argument + 1 == argc) {
				printf(ORACLE_USAGE);
				return 1;
			}
			argument++;
		}

		/* Applies the option. */
		switch (kind) {
		case ORACLE_OPTION_ALL:
			for (stage = 1; stage <= ORACLE_STAGES; stage++)
				selection->stages[stage] = 1;
			break;
		case ORACLE_OPTION_STAGE:
			number = strtoul(argv[argument], &end, 10);
			if (*end != '\0' || number < 1 || number > ORACLE_STAGES) {
				printf("vkcompute: no stage %s (1 to %u)\n", argv[argument], ORACLE_STAGES);
				return 1;
			}
			selection->stages[number] = 1;
			break;
		case ORACLE_OPTION_TEST:
			known = oracle_test_named(argv[argument]);
			if (known == 0 || selection->name_count == ORACLE_MAX_NAMES) {
				printf("vkcompute: no shader %s (-l lists them)\n", argv[argument]);
				return 1;
			}
			selection->names[selection->name_count] = argv[argument];
			selection->name_count++;
			break;
		case ORACLE_OPTION_LIST:
			selection->list = 1;
			break;
		case ORACLE_OPTION_NEGATIVE:
			selection->corrupt = 1;
			break;
		default:
			printf(ORACLE_USAGE);
			return 1;
		}
	}

	/* Refuses a run that selects nothing. */
	if (selection->list == 0 &&
	    selection->name_count == 0 &&
	    selection->stages[1] + selection->stages[2] + selection->stages[3] == 0) {
		printf(ORACLE_USAGE);
		return 1;
	}

	/* Succeeded: the selection is complete. */
	return 0;
}

/* Names the kind of one command-line word. */
static enum oracle_option
oracle_option_kind(
	const char *option)
{
	int different;

	/* -a selects every stage. */
	different = strcmp(option, "-a");
	if (different == 0)
		return ORACLE_OPTION_ALL;

	/* -s selects one stage. */
	different = strcmp(option, "-s");
	if (different == 0)
		return ORACLE_OPTION_STAGE;

	/* -t selects one shader. */
	different = strcmp(option, "-t");
	if (different == 0)
		return ORACLE_OPTION_TEST;

	/* -l lists the shaders. */
	different = strcmp(option, "-l");
	if (different == 0)
		return ORACLE_OPTION_LIST;

	/* -n turns on the negative control. */
	different = strcmp(option, "-n");
	if (different == 0)
		return ORACLE_OPTION_NEGATIVE;

	/* Anything else is not an option. */
	return ORACLE_OPTION_UNKNOWN;
}

/* Tells whether a shader of that name is in the table. */
static int
oracle_test_named(
	const char *name)
{
	uint32_t index;
	int different;

	/* Looks for the name among the shaders. */
	for (index = 0; index < ORACLE_TEST_COUNT; index++) {
		different = strcmp(oracle_tests[index].name, name);
		if (different == 0)
			return 1;
	}

	/* No shader has that name. */
	return 0;
}

/* Tells whether the options select a shader, by its stage or by its name. */
static int
oracle_selected(
	const struct oracle_test *test,
	const struct oracle_selection *selection)
{
	uint32_t index;
	int different;

	/* A shader of a selected stage runs. */
	if (selection->stages[test->stage] != 0)
		return 1;

	/* So does a shader named on the command line. */
	for (index = 0; index < selection->name_count; index++) {
		different = strcmp(selection->names[index], test->name);
		if (different == 0)
			return 1;
	}

	/* Everything else is left out. */
	return 0;
}

/* Prints each shader's name, stage, workgroup and dispatch. */
static void
oracle_list(
	void)
{
	const struct oracle_test *test;
	uint32_t index;

	/* Prints one line per shader. */
	for (index = 0; index < ORACLE_TEST_COUNT; index++) {
		test = &oracle_tests[index];
		printf("%-20s stage %u, local %ux%ux%u, shared %u bytes, groups %ux%ux%u, %u buffers, %u dispatches\n",
		       test->name,
		       test->stage,
		       test->local[0],
		       test->local[1],
		       test->local[2],
		       test->shared_bytes,
		       test->groups[0],
		       test->groups[1],
		       test->groups[2],
		       test->buffer_count,
		       test->pass_count);
	}
}

/*
 * Tells whether a shader needs more than the device's limits report.
 *
 * Writes the first limit it exceeds into reason (at least
 * ORACLE_REASON_BYTES long), as the limit's name, the reported value and
 * what the shader needs, and returns 1; returns 0 when the shader fits.
 */
static int
oracle_limit_refusal(
	const VkPhysicalDeviceLimits *limits,
	const struct oracle_test *test,
	char *reason)
{
	static const char axis_names[3] = { 'x', 'y', 'z' };
	uint32_t axis;
	uint32_t invocations;
	uint32_t push_bytes;
	uint32_t sets;
	uint32_t index;

	/* Refuses a workgroup wider than the device's along any axis, or a dispatch longer. */
	for (axis = 0; axis < 3; axis++) {
		if (test->local[axis] > limits->maxComputeWorkGroupSize[axis]) {
			sprintf(reason,
				"maxComputeWorkGroupSize.%c %u < %u",
				axis_names[axis],
				limits->maxComputeWorkGroupSize[axis],
				test->local[axis]);
			return 1;
		}
		if (test->groups[axis] > limits->maxComputeWorkGroupCount[axis]) {
			sprintf(reason,
				"maxComputeWorkGroupCount.%c %u < %u",
				axis_names[axis],
				limits->maxComputeWorkGroupCount[axis],
				test->groups[axis]);
			return 1;
		}
	}

	/* Refuses a workgroup with more invocations than the device runs together. */
	invocations = test->local[0] * test->local[1] * test->local[2];
	if (invocations > limits->maxComputeWorkGroupInvocations) {
		sprintf(reason, "maxComputeWorkGroupInvocations %u < %u", limits->maxComputeWorkGroupInvocations, invocations);
		return 1;
	}

	/* Refuses more shared memory than the device reports. */
	if (test->shared_bytes > limits->maxComputeSharedMemorySize) {
		sprintf(reason, "maxComputeSharedMemorySize %u < %u", limits->maxComputeSharedMemorySize, test->shared_bytes);
		return 1;
	}

	/* Refuses more storage buffers than one stage may see. */
	if (test->buffer_count > limits->maxPerStageDescriptorStorageBuffers) {
		sprintf(reason,
			"maxPerStageDescriptorStorageBuffers %u < %u",
			limits->maxPerStageDescriptorStorageBuffers,
			test->buffer_count);
		return 1;
	}

	/* Refuses more push-constant bytes than the device takes. */
	push_bytes = test->push_words * (uint32_t)sizeof(uint32_t);
	if (push_bytes > limits->maxPushConstantsSize) {
		sprintf(reason, "maxPushConstantsSize %u < %u", limits->maxPushConstantsSize, push_bytes);
		return 1;
	}

	/* Counts the descriptor sets: one past the highest set any buffer names. */
	sets = 0;
	for (index = 0; index < test->buffer_count; index++) {
		if (test->buffers[index].set + 1 > sets)
			sets = test->buffers[index].set + 1;
	}

	/* Refuses more descriptor sets than the device binds at once. */
	if (sets > limits->maxBoundDescriptorSets) {
		sprintf(reason, "maxBoundDescriptorSets %u < %u", limits->maxBoundDescriptorSets, sets);
		return 1;
	}

	/* The shader fits the device. */
	return 0;
}

/* Runs one oracle shader, prints its line and returns its outcome; sets *lost when the device stops answering. */
static enum oracle_outcome
oracle_run_test(
	struct vkcompute *run,
	const struct oracle_test *test,
	int corrupt,
	int *lost)
{
	struct oracle_objects objects;
	struct oracle_check check;
	VkResult status;

	/* Starts with no objects, so the teardown can run from any point. */
	memset(&objects, 0, sizeof(objects));

	/* Creates the buffers and fills them; a failure here is the driver's, not the shader's. */
	status = oracle_create_buffers(run, test, &objects);
	if (status != VK_SUCCESS) {
		printf("vkcompute: %s: FAIL, %s %d\n", test->name, objects.failed_call, (int)status);
		oracle_destroy(run, &objects);
		return ORACLE_FAIL;
	}

	/* Creates the pipeline; a driver that refuses it does not support the shader. */
	status = oracle_create_pipeline(run, test, &objects);
	if (status != VK_SUCCESS) {
		printf("vkcompute: %s: UNSUPPORTED, %s %d\n", test->name, objects.failed_call, (int)status);
		oracle_destroy(run, &objects);
		return ORACLE_UNSUPPORTED;
	}

	/* Points the descriptor sets at the buffers and records the dispatches. */
	status = oracle_bind_buffers(run, test, &objects);
	if (status == VK_SUCCESS)
		status = oracle_record(run, test, &objects);
	if (status != VK_SUCCESS) {
		printf("vkcompute: %s: FAIL, %s %d\n", test->name, objects.failed_call, (int)status);
		oracle_destroy(run, &objects);
		return ORACLE_FAIL;
	}

	/* Submits and waits; a lost or hung device stops the run with its objects left in place. */
	status = oracle_submit(run, &objects);
	if (status == VK_TIMEOUT || status == VK_ERROR_DEVICE_LOST) {
		printf("vkcompute: %s: FAIL, %s %d\n", test->name, objects.failed_call, (int)status);
		*lost = 1;
		return ORACLE_FAIL;
	}

	/* Reports any other failed submit. */
	if (status != VK_SUCCESS) {
		printf("vkcompute: %s: FAIL, %s %d\n", test->name, objects.failed_call, (int)status);
		oracle_destroy(run, &objects);
		return ORACLE_FAIL;
	}

	/* Compares every decided word with the reference. */
	memset(&check, 0, sizeof(check));
	check.corrupt = corrupt;
	oracle_compare(test, &objects, &check);
	oracle_destroy(run, &objects);

	/* Reports the first mismatch of a shader that left wrong words. */
	if (check.bad != 0) {
		printf("vkcompute: %s: FAIL, %u/%u words (buffer %u word %u: 0x%08x, expected 0x%08x)\n",
		       test->name,
		       check.compared - check.bad,
		       check.compared,
		       check.first_buffer,
		       check.first_index,
		       check.first_got,
		       check.first_expected);
		return ORACLE_FAIL;
	}

	/* Succeeded: every decided word matched. */
	printf("vkcompute: %s: PASS, %u/%u words\n", test->name, check.compared, check.compared);
	return ORACLE_PASS;
}

/* Creates the shader's buffers in host-coherent memory, maps them, and fills the sentinel and the inputs. */
static VkResult
oracle_create_buffers(
	struct vkcompute *run,
	const struct oracle_test *test,
	struct oracle_objects *objects)
{
	VkBufferCreateInfo buffer_info;
	VkMemoryRequirements requirements;
	VkMemoryAllocateInfo allocate_info;
	uint32_t index;
	uint32_t word;
	uint32_t total_words;
	uint32_t type;
	void *mapped;
	VkResult status;

	/* Creates, backs and maps each buffer with its margin. */
	for (index = 0; index < test->buffer_count; index++) {
		total_words = test->buffers[index].words + ORACLE_MARGIN_WORDS;

		/* Creates the buffer. */
		memset(&buffer_info, 0, sizeof(buffer_info));
		buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		buffer_info.size = total_words * sizeof(uint32_t);
		buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		objects->failed_call = "vkCreateBuffer";
		status = vkCreateBuffer(run->device, &buffer_info, NULL, &objects->buffers[index]);
		if (status != VK_SUCCESS)
			return status;

		/* Finds a memory type the CPU sees without flushes. */
		vkGetBufferMemoryRequirements(run->device, objects->buffers[index], &requirements);
		type = oracle_memory_type(run, requirements.memoryTypeBits);
		objects->failed_call = "host-coherent memory type";
		if (type == VK_MAX_MEMORY_TYPES)
			return VK_ERROR_FEATURE_NOT_PRESENT;

		/* Allocates the memory and binds the buffer to it. */
		memset(&allocate_info, 0, sizeof(allocate_info));
		allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocate_info.allocationSize = requirements.size;
		allocate_info.memoryTypeIndex = type;
		objects->failed_call = "vkAllocateMemory";
		status = vkAllocateMemory(run->device, &allocate_info, NULL, &objects->memories[index]);
		if (status != VK_SUCCESS)
			return status;
		objects->failed_call = "vkBindBufferMemory";
		status = vkBindBufferMemory(run->device, objects->buffers[index], objects->memories[index], 0);
		if (status != VK_SUCCESS)
			return status;

		/* Maps the memory for the whole run of the shader. */
		objects->failed_call = "vkMapMemory";
		status = vkMapMemory(run->device, objects->memories[index], 0, VK_WHOLE_SIZE, 0, &mapped);
		if (status != VK_SUCCESS)
			return status;
		objects->words[index] = mapped;

		/* Fills every word with the sentinel no shader writes. */
		for (word = 0; word < total_words; word++)
			objects->words[index][word] = ORACLE_SENTINEL;
	}

	/* Writes the inputs the reference computes from. */
	test->fill(test, objects->words);

	/* Succeeded: the buffers hold their inputs and the sentinel. */
	return VK_SUCCESS;
}

/* Creates the shader module, a layout per descriptor set, the pipeline layout and the pipeline. */
static VkResult
oracle_create_pipeline(
	struct vkcompute *run,
	const struct oracle_test *test,
	struct oracle_objects *objects)
{
	VkShaderModuleCreateInfo module_info;
	VkDescriptorSetLayoutBinding bindings[ORACLE_MAX_BUFFERS];
	VkDescriptorSetLayoutCreateInfo set_info;
	VkPushConstantRange push_range;
	VkPipelineLayoutCreateInfo layout_info;
	VkComputePipelineCreateInfo pipeline_info;
	uint32_t set;
	uint32_t index;
	uint32_t binding_count;
	VkResult status;

	/* Creates the module from the embedded SPIR-V. */
	memset(&module_info, 0, sizeof(module_info));
	module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	module_info.codeSize = test->code_size;
	module_info.pCode = test->code;
	objects->failed_call = "vkCreateShaderModule";
	status = vkCreateShaderModule(run->device, &module_info, NULL, &objects->module);
	if (status != VK_SUCCESS)
		return status;

	/* Counts the descriptor sets: one past the highest set any buffer names. */
	objects->set_count = 1;
	for (index = 0; index < test->buffer_count; index++) {
		if (test->buffers[index].set + 1 > objects->set_count)
			objects->set_count = test->buffers[index].set + 1;
	}

	/* Creates each set's layout from the buffers that name it. */
	for (set = 0; set < objects->set_count; set++) {
		binding_count = 0;
		for (index = 0; index < test->buffer_count; index++) {
			if (test->buffers[index].set != set)
				continue;
			memset(&bindings[binding_count], 0, sizeof(bindings[binding_count]));
			bindings[binding_count].binding = test->buffers[index].binding;
			bindings[binding_count].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			bindings[binding_count].descriptorCount = 1;
			bindings[binding_count].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
			binding_count++;
		}

		/* Creates the layout of this set. */
		memset(&set_info, 0, sizeof(set_info));
		set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		set_info.bindingCount = binding_count;
		set_info.pBindings = bindings;
		objects->failed_call = "vkCreateDescriptorSetLayout";
		status = vkCreateDescriptorSetLayout(run->device, &set_info, NULL, &objects->set_layouts[set]);
		if (status != VK_SUCCESS)
			return status;
	}

	/* Describes the pipeline layout: the sets, and the push constants when the shader reads any. */
	memset(&push_range, 0, sizeof(push_range));
	push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	push_range.offset = 0;
	push_range.size = test->push_words * sizeof(uint32_t);
	memset(&layout_info, 0, sizeof(layout_info));
	layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.setLayoutCount = objects->set_count;
	layout_info.pSetLayouts = objects->set_layouts;
	if (test->push_words != 0) {
		layout_info.pushConstantRangeCount = 1;
		layout_info.pPushConstantRanges = &push_range;
	}

	/* Creates the pipeline layout. */
	objects->failed_call = "vkCreatePipelineLayout";
	status = vkCreatePipelineLayout(run->device, &layout_info, NULL, &objects->pipeline_layout);
	if (status != VK_SUCCESS)
		return status;

	/* Creates the compute pipeline from the module's main. */
	memset(&pipeline_info, 0, sizeof(pipeline_info));
	pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	pipeline_info.stage.module = objects->module;
	pipeline_info.stage.pName = "main";
	pipeline_info.layout = objects->pipeline_layout;
	objects->failed_call = "vkCreateComputePipelines";
	status = vkCreateComputePipelines(run->device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &objects->pipeline);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the pipeline is ready to bind. */
	return VK_SUCCESS;
}

/* Allocates the descriptor sets and points each binding at its buffer. */
static VkResult
oracle_bind_buffers(
	struct vkcompute *run,
	const struct oracle_test *test,
	struct oracle_objects *objects)
{
	VkDescriptorPoolSize size;
	VkDescriptorPoolCreateInfo pool_info;
	VkDescriptorSetAllocateInfo set_info;
	VkDescriptorBufferInfo buffer_infos[ORACLE_MAX_BUFFERS];
	VkWriteDescriptorSet writes[ORACLE_MAX_BUFFERS];
	uint32_t index;
	VkResult status;

	/* Creates a pool for the sets and their storage buffers. */
	memset(&size, 0, sizeof(size));
	size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	size.descriptorCount = test->buffer_count;
	memset(&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.maxSets = objects->set_count;
	pool_info.poolSizeCount = 1;
	pool_info.pPoolSizes = &size;
	objects->failed_call = "vkCreateDescriptorPool";
	status = vkCreateDescriptorPool(run->device, &pool_info, NULL, &objects->pool);
	if (status != VK_SUCCESS)
		return status;

	/* Allocates one set per layout. */
	memset(&set_info, 0, sizeof(set_info));
	set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	set_info.descriptorPool = objects->pool;
	set_info.descriptorSetCount = objects->set_count;
	set_info.pSetLayouts = objects->set_layouts;
	objects->failed_call = "vkAllocateDescriptorSets";
	status = vkAllocateDescriptorSets(run->device, &set_info, objects->sets);
	if (status != VK_SUCCESS)
		return status;

	/* Writes each whole buffer, margin included, into its set and binding. */
	for (index = 0; index < test->buffer_count; index++) {
		memset(&buffer_infos[index], 0, sizeof(buffer_infos[index]));
		buffer_infos[index].buffer = objects->buffers[index];
		buffer_infos[index].offset = 0;
		buffer_infos[index].range = VK_WHOLE_SIZE;
		memset(&writes[index], 0, sizeof(writes[index]));
		writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[index].dstSet = objects->sets[test->buffers[index].set];
		writes[index].dstBinding = test->buffers[index].binding;
		writes[index].descriptorCount = 1;
		writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[index].pBufferInfo = &buffer_infos[index];
	}
	vkUpdateDescriptorSets(run->device, test->buffer_count, writes, 0, NULL);

	/* Succeeded: every binding names its buffer. */
	return VK_SUCCESS;
}

/* Records each dispatch with its push constants, a barrier between dispatches and one for the host at the end. */
static VkResult
oracle_record(
	struct vkcompute *run,
	const struct oracle_test *test,
	struct oracle_objects *objects)
{
	VkCommandPoolCreateInfo pool_info;
	VkCommandBufferAllocateInfo command_info;
	VkCommandBufferBeginInfo begin;
	VkMemoryBarrier barrier;
	uint32_t pass;
	VkResult status;

	/* Creates the command pool and one primary command buffer. */
	memset(&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool_info.queueFamilyIndex = run->family;
	objects->failed_call = "vkCreateCommandPool";
	status = vkCreateCommandPool(run->device, &pool_info, NULL, &objects->command_pool);
	if (status != VK_SUCCESS)
		return status;
	memset(&command_info, 0, sizeof(command_info));
	command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	command_info.commandPool = objects->command_pool;
	command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	command_info.commandBufferCount = 1;
	objects->failed_call = "vkAllocateCommandBuffers";
	status = vkAllocateCommandBuffers(run->device, &command_info, &objects->command);
	if (status != VK_SUCCESS)
		return status;

	/* Begins the command buffer and binds the pipeline and its sets. */
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	objects->failed_call = "vkBeginCommandBuffer";
	status = vkBeginCommandBuffer(objects->command, &begin);
	if (status != VK_SUCCESS)
		return status;
	vkCmdBindPipeline(objects->command, VK_PIPELINE_BIND_POINT_COMPUTE, objects->pipeline);
	vkCmdBindDescriptorSets(
		objects->command,
		VK_PIPELINE_BIND_POINT_COMPUTE,
		objects->pipeline_layout,
		0,
		objects->set_count,
		objects->sets,
		0,
		NULL);

	/* Records each dispatch with its push constants, the barrier before it letting it read what the last one wrote. */
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	for (pass = 0; pass < test->pass_count; pass++) {
		/* Orders this dispatch after the one before it. */
		if (pass != 0) {
			vkCmdPipelineBarrier(
				objects->command,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				0,
				1,
				&barrier,
				0,
				NULL,
				0,
				NULL);
		}

		/* Gives the dispatch its own push constants. */
		if (test->push_words != 0) {
			vkCmdPushConstants(
				objects->command,
				objects->pipeline_layout,
				VK_SHADER_STAGE_COMPUTE_BIT,
				0,
				test->push_words * sizeof(uint32_t),
				test->passes[pass].push);
		}

		/* Dispatches the workgroups. */
		vkCmdDispatch(objects->command, test->groups[0], test->groups[1], test->groups[2]);
	}

	/* Makes the last dispatch's writes visible to the host and ends the command buffer. */
	barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	vkCmdPipelineBarrier(
		objects->command,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_HOST_BIT,
		0,
		1,
		&barrier,
		0,
		NULL,
		0,
		NULL);
	objects->failed_call = "vkEndCommandBuffer";
	status = vkEndCommandBuffer(objects->command);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the command buffer is ready to submit. */
	return VK_SUCCESS;
}

/* Submits the recorded command buffer with a fence and waits for the fence. */
static VkResult
oracle_submit(
	struct vkcompute *run,
	struct oracle_objects *objects)
{
	VkFenceCreateInfo fence_info;
	VkSubmitInfo submit;
	VkResult status;

	/* Creates the fence the submit signals. */
	memset(&fence_info, 0, sizeof(fence_info));
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	objects->failed_call = "vkCreateFence";
	status = vkCreateFence(run->device, &fence_info, NULL, &objects->fence);
	if (status != VK_SUCCESS)
		return status;

	/* Submits the command buffer. */
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &objects->command;
	objects->failed_call = "vkQueueSubmit";
	status = vkQueueSubmit(run->queue, 1, &submit, objects->fence);
	if (status != VK_SUCCESS)
		return status;

	/* Waits for the dispatches; VK_TIMEOUT means the device hung. */
	objects->failed_call = "vkWaitForFences";
	status = vkWaitForFences(run->device, 1, &objects->fence, VK_TRUE, VKCOMPUTE_WAIT_NS);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: every dispatch has finished. */
	return VK_SUCCESS;
}

/* Runs the shader's checker, then checks that every buffer's margin still holds the sentinel. */
static void
oracle_compare(
	const struct oracle_test *test,
	struct oracle_objects *objects,
	struct oracle_check *check)
{
	uint32_t index;
	uint32_t end;

	/* Compares the words the reference decides. */
	test->check(check, test, objects->words);

	/* Finds writes past the end of any buffer. */
	for (index = 0; index < test->buffer_count; index++) {
		end = test->buffers[index].words;
		oracle_expect_untouched(check, objects->words, index, end, end + ORACLE_MARGIN_WORDS);
	}
}

/* Destroys every object of one shader's run, newest first. */
static void
oracle_destroy(
	struct vkcompute *run,
	struct oracle_objects *objects)
{
	uint32_t index;

	/* Waits for the device, so that nothing is destroyed while it is in use. */
	vkDeviceWaitIdle(run->device);

	/* Destroys the command objects, the pipeline and its layouts. */
	vkDestroyFence(run->device, objects->fence, NULL);
	vkDestroyCommandPool(run->device, objects->command_pool, NULL);
	vkDestroyDescriptorPool(run->device, objects->pool, NULL);
	vkDestroyPipeline(run->device, objects->pipeline, NULL);
	vkDestroyPipelineLayout(run->device, objects->pipeline_layout, NULL);
	for (index = 0; index < ORACLE_MAX_SETS; index++)
		vkDestroyDescriptorSetLayout(run->device, objects->set_layouts[index], NULL);
	vkDestroyShaderModule(run->device, objects->module, NULL);

	/* Unmaps each buffer's memory, then destroys the buffer and frees the memory. */
	for (index = 0; index < ORACLE_MAX_BUFFERS; index++) {
		if (objects->words[index] != NULL)
			vkUnmapMemory(run->device, objects->memories[index]);
		vkDestroyBuffer(run->device, objects->buffers[index], NULL);
		vkFreeMemory(run->device, objects->memories[index], NULL);
	}
}

/* Finds a memory type among type_bits that is host-visible and host-coherent; VK_MAX_MEMORY_TYPES if none. */
static uint32_t
oracle_memory_type(
	struct vkcompute *run,
	uint32_t type_bits)
{
	VkPhysicalDeviceMemoryProperties memory;
	VkMemoryPropertyFlags wanted;
	uint32_t type;

	/* Takes the first type that the buffer accepts and that the CPU sees without flushes. */
	vkGetPhysicalDeviceMemoryProperties(run->physical, &memory);
	wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	for (type = 0; type < memory.memoryTypeCount; type++) {
		if ((type_bits & (1U << type)) == 0)
			continue;
		if ((memory.memoryTypes[type].propertyFlags & wanted) == wanted)
			return type;
	}

	/* No type fits. */
	return VK_MAX_MEMORY_TYPES;
}

/* Compares one word with its expected value, counting it and keeping the first mismatch. */
static void
oracle_expect(
	struct oracle_check *check,
	uint32_t buffer,
	uint32_t index,
	uint32_t got,
	uint32_t expected)
{
	/* The negative control makes the first expected word of the shader wrong. */
	if (check->corrupt != 0 && check->compared == 0)
		expected = expected ^ 1U;
	check->compared++;

	/* A matching word needs nothing more. */
	if (got == expected)
		return;

	/* Keeps the first mismatch for the result line. */
	if (check->bad == 0) {
		check->first_buffer = buffer;
		check->first_index = index;
		check->first_got = got;
		check->first_expected = expected;
	}
	check->bad++;
}

/* Compares one fp32 word with an expected value within ulps units in the last place. */
static void
oracle_expect_near(
	struct oracle_check *check,
	uint32_t buffer,
	uint32_t index,
	uint32_t got,
	float expected,
	uint32_t ulps)
{
	uint32_t expected_bits;
	int64_t distance;

	/* The negative control moves the first expected value far outside the tolerance. */
	expected_bits = oracle_float_bits(expected);
	if (check->corrupt != 0 && check->compared == 0)
		expected_bits = expected_bits ^ 0x00400000U;

	/* Measures the distance in representable values between the two. */
	distance = oracle_float_order(got) - oracle_float_order(expected_bits);
	if (distance < 0)
		distance = -distance;

	/* Counts a word close enough as the expected one. */
	if (distance <= (int64_t)ulps) {
		oracle_expect(check, buffer, index, got, got);
		return;
	}

	/* Counts anything farther as a mismatch. */
	oracle_expect(check, buffer, index, got, expected_bits);
}

/* Expects the words from 'from' up to 'to' of one buffer to still hold the sentinel. */
static void
oracle_expect_untouched(
	struct oracle_check *check,
	uint32_t *const *words,
	uint32_t buffer,
	uint32_t from,
	uint32_t to)
{
	uint32_t index;

	/* Compares each word no shader may have written. */
	for (index = from; index < to; index++)
		oracle_expect(check, buffer, index, words[buffer][index], ORACLE_SENTINEL);
}

/* Mixes the bits of a word (the MurmurHash3 finalizer), for inputs that look random. */
static uint32_t
oracle_hash(
	uint32_t value)
{
	/* Spreads every input bit over the whole word. */
	value = value ^ (value >> 16);
	value = value * 0x85ebca6bU;
	value = value ^ (value >> 13);
	value = value * 0xc2b2ae35U;
	value = value ^ (value >> 16);

	/* The mixed word. */
	return value;
}

/* The input word at index of a buffer: the same on every run, so that the reference can compute it again. */
static uint32_t
oracle_input(
	uint32_t buffer,
	uint32_t index)
{
	uint32_t word;

	/* Hashes the index with a seed per buffer. */
	word = oracle_hash(index * 0x9e3779b1U + (buffer + 1) * 0x632be5abU);

	/* The input word. */
	return word;
}

/* The bits of an fp32 value. */
static uint32_t
oracle_float_bits(
	float value)
{
	uint32_t bits;

	/* Copies the representation without converting it. */
	memcpy(&bits, &value, sizeof(bits));

	/* The bits. */
	return bits;
}

/* The fp32 value of some bits. */
static float
oracle_bits_float(
	uint32_t bits)
{
	float value;

	/* Copies the representation without converting it. */
	memcpy(&value, &bits, sizeof(value));

	/* The value. */
	return value;
}

/* The place of an fp32 value in the order of all values, so that neighbouring values differ by one. */
static int64_t
oracle_float_order(
	uint32_t bits)
{
	/* Negative values count down from zero. */
	if ((bits & 0x80000000U) != 0)
		return -(int64_t)(bits & 0x7fffffffU);

	/* Positive values count up from zero. */
	return (int64_t)bits;
}

/* The square root of a value in double precision (Newton's method, so that no libm is needed). */
static double
oracle_square_root(
	double value)
{
	double root;
	int step;

	/* Leaves zero as it is. */
	if (value <= 0.0)
		return 0.0;

	/* Refines a guess until it settles; 64 steps reach double precision from any start up to 1e9. */
	root = value / 2.0 + 1.0;
	for (step = 0; step < 64; step++)
		root = (root + value / root) / 2.0;

	/* The root. */
	return root;
}

/* An arithmetic right shift of a two's complement word, written out so that C's rules for negative values do not matter. */
static uint32_t
oracle_signed_shift(
	uint32_t value,
	uint32_t shift)
{
	uint32_t shifted;

	/* Shifts in zeros, then sets the bits a negative value shifts in. */
	shifted = value >> shift;
	if ((value & 0x80000000U) != 0 && shift != 0)
		shifted = shifted | ~(0xffffffffU >> shift);

	/* The shifted word. */
	return shifted;
}

/* A signed division rounded toward zero, as SPIR-V's OpSDiv does, on two's complement words. */
static uint32_t
oracle_signed_divide(
	uint32_t dividend,
	uint32_t divisor)
{
	uint32_t dividend_magnitude;
	uint32_t divisor_magnitude;
	uint32_t quotient;
	int negative;

	/* Takes the magnitudes and remembers whether the signs differ. */
	dividend_magnitude = dividend;
	divisor_magnitude = divisor;
	negative = 0;
	if ((dividend & 0x80000000U) != 0) {
		dividend_magnitude = 0U - dividend;
		negative = !negative;
	}
	if ((divisor & 0x80000000U) != 0) {
		divisor_magnitude = 0U - divisor;
		negative = !negative;
	}

	/* Divides the magnitudes and gives the quotient the sign of the operands. */
	quotient = dividend_magnitude / divisor_magnitude;
	if (negative != 0)
		quotient = 0U - quotient;

	/* The quotient as a word. */
	return quotient;
}

/* A word read as a two's complement integer. */
static int32_t
oracle_signed(
	uint32_t value)
{
	/* Values with the top bit set are the negative ones. */
	if ((value & 0x80000000U) != 0)
		return -(int32_t)(0x7fffffffU - (value & 0x7fffffffU)) - 1;

	/* The others are the same number. */
	return (int32_t)value;
}

/* The divisor of s1-int-div at index: never 0 or -1, between -50000 and 50000. */
static uint32_t
oracle_div_divisor(
	uint32_t index)
{
	int32_t divisor;

	/* Spreads the hash over the range, then moves away from the two divisors SPIR-V leaves undefined. */
	divisor = (int32_t)(oracle_input(1, index) % 100001U) - 50000;
	if (divisor == 0 || divisor == -1)
		divisor = 7;

	/* The divisor as a word. */
	return (uint32_t)divisor;
}

/* An fp32 operand of s1-float-arith: a nonzero multiple of 1/8 between -32 and 32, so that every result is exact. */
static float
oracle_float_operand(
	uint32_t buffer,
	uint32_t index)
{
	int32_t eighths;

	/* Takes -256 to 256 eighths, with 3 eighths in place of zero. */
	eighths = (int32_t)(oracle_input(buffer, index) % 513U) - 256;
	if (eighths == 0)
		eighths = 3;

	/* The operand. */
	return (float)eighths * 0.125f;
}

/* The integer operand of s1-float-convert at index: within plus or minus 2^24, where fp32 is exact. */
static uint32_t
oracle_convert_integer(
	uint32_t index)
{
	uint32_t word;

	/* Keeps 25 bits and extends their sign. */
	word = oracle_input(0, index) & 0x01ffffffU;
	if ((word & 0x01000000U) != 0)
		word = word | 0xfe000000U;

	/* The operand. */
	return word;
}

/* The fp32 operand of s1-float-convert at index: a multiple of 1/4 between -25000 and 25000. */
static float
oracle_convert_float(
	uint32_t index)
{
	int32_t quarters;

	/* Takes -100000 to 100000 quarters. */
	quarters = (int32_t)(oracle_input(1, index) % 200001U) - 100000;

	/* The operand. */
	return (float)quarters * 0.25f;
}

/* An fp32 operand of s1-float-div: the dividend in 1/16 steps up to 625, the divisor up to 100 and at least 0.5 from zero. */
static float
oracle_div_operand(
	uint32_t buffer,
	uint32_t index)
{
	int32_t sixteenths;

	/* Takes -10000 to 10000 sixteenths for the dividend, -1600 to 1600 kept out of -8 to 8 for the divisor. */
	if (buffer == 0) {
		sixteenths = (int32_t)(oracle_input(0, index) % 20001U) - 10000;
		if (sixteenths == 0)
			sixteenths = 5;
	} else {
		sixteenths = (int32_t)(oracle_input(1, index) % 3201U) - 1600;
		if (sixteenths > -8 && sixteenths < 8)
			sixteenths = 9;
	}

	/* The operand. */
	return (float)sixteenths * 0.0625f;
}

/* Leaves every buffer at the sentinel: the shader writes everything it reads. */
static void
oracle_fill_none(
	const struct oracle_test *test,
	uint32_t *const *words)
{
	UNUSED_PARAMETER(test);
	UNUSED_PARAMETER(words);
}

/* Fills every buffer but the last with its hashed inputs: shaders of this kind read those and write the last. */
static void
oracle_fill_inputs(
	const struct oracle_test *test,
	uint32_t *const *words)
{
	uint32_t buffer;
	uint32_t index;

	/* Writes each input word. */
	for (buffer = 0; buffer + 1 < test->buffer_count; buffer++) {
		for (index = 0; index < test->buffers[buffer].words; index++)
			words[buffer][index] = oracle_input(buffer, index);
	}
}

/* Fills the dividends with hashed words and the divisors away from 0 and -1. */
static void
oracle_fill_div(
	const struct oracle_test *test,
	uint32_t *const *words)
{
	uint32_t index;

	/* Writes each dividend and divisor. */
	for (index = 0; index < test->buffers[0].words; index++) {
		words[0][index] = oracle_input(0, index);
		words[1][index] = oracle_div_divisor(index);
	}
}

/* Fills both operand buffers with fp32 operands whose results are exact. */
static void
oracle_fill_float_arith(
	const struct oracle_test *test,
	uint32_t *const *words)
{
	uint32_t index;

	/* Writes each operand pair. */
	for (index = 0; index < test->buffers[0].words; index++) {
		words[0][index] = oracle_float_bits(oracle_float_operand(0, index));
		words[1][index] = oracle_float_bits(oracle_float_operand(1, index));
	}
}

/* Fills the integer and fp32 operands of the conversions. */
static void
oracle_fill_float_convert(
	const struct oracle_test *test,
	uint32_t *const *words)
{
	uint32_t index;

	/* Writes each integer and fp32 operand. */
	for (index = 0; index < test->buffers[0].words; index++) {
		words[0][index] = oracle_convert_integer(index);
		words[1][index] = oracle_float_bits(oracle_convert_float(index));
	}
}

/* Fills the dividends and the divisors of the fp32 division. */
static void
oracle_fill_float_div(
	const struct oracle_test *test,
	uint32_t *const *words)
{
	uint32_t index;

	/* Writes each dividend and divisor. */
	for (index = 0; index < test->buffers[0].words; index++) {
		words[0][index] = oracle_float_bits(oracle_div_operand(0, index));
		words[1][index] = oracle_float_bits(oracle_div_operand(1, index));
	}
}

/* Fills the first half of the chain's buffer, which the first dispatch reads. */
static void
oracle_fill_chain(
	const struct oracle_test *test,
	uint32_t *const *words)
{
	uint32_t index;

	/* Writes the first half; the second keeps the sentinel until the first dispatch writes it. */
	for (index = 0; index < test->buffers[0].words / 2; index++)
		words[0][index] = oracle_input(0, index);
}

/* Clears the first buffer, which holds the counters or the slots the atomics start from. */
static void
oracle_fill_zero(
	const struct oracle_test *test,
	uint32_t *const *words)
{
	uint32_t index;

	/* Writes zero over the declared words. */
	for (index = 0; index < test->buffers[0].words; index++)
		words[0][index] = 0;
}

/* Fills the inputs of s3-buffer-minmax and starts each minimum high and each maximum low. */
static void
oracle_fill_buffer_minmax(
	const struct oracle_test *test,
	uint32_t *const *words)
{
	uint32_t index;

	/* Writes the values the atomics compare. */
	for (index = 0; index < test->buffers[0].words; index++)
		words[0][index] = oracle_input(0, index);

	/* Starts the unsigned and signed minima at the top and the maxima at the bottom. */
	for (index = 0; index < 4; index++) {
		words[1][index] = 0xffffffffU;
		words[1][4 + index] = 0;
		words[2][index] = 0x7fffffffU;
		words[2][4 + index] = 0x80000000U;
	}
}

/* Fills the inputs of s3-buffer-bits and the starting words of the and, or and xor slots. */
static void
oracle_fill_buffer_bits(
	const struct oracle_test *test,
	uint32_t *const *words)
{
	uint32_t index;

	/* Writes the values the atomics combine. */
	for (index = 0; index < test->buffers[0].words; index++)
		words[0][index] = oracle_input(0, index);

	/* Starts and at all ones, or at zero and xor at a pattern. */
	for (index = 0; index < 4; index++) {
		words[1][index] = 0xffffffffU;
		words[1][4 + index] = 0;
		words[1][8 + index] = 0x12345678U;
	}
}

/* Checks s1-copy: every destination word is its source word. */
static void
oracle_check_copy(
	struct oracle_check *check,
	const struct oracle_test *test,
	uint32_t *const *words)
{
	uint32_t index;

	/* Compares each copied word with the input it was copied from. */
	for (index = 0; index < test->buffers[1].words; index++)
		oracle_expect(check, 1, index, words[1][index], oracle_input(0, index));
}

/* Checks s1-int-alu: the ten integer results of each operand pair. */
static void
oracle_check_int_alu(
	struct oracle_check *check,
	const struct oracle_test *test,
	uint32_t *const *words)
{
	uint32_t expected[10];
	uint32_t index;
	uint32_t result;
	uint32_t x;
	uint32_t y;
	uint32_t shift;

	/* Computes each pair's results as the shader does, and compares them. */
	for (index = 0; index < test->buffers[0].words; index++) {
		x = oracle_input(0, index);
		y = oracle_input(1, index);
		shift = y & 31U;
		expected[0] = x + y;
		expected[1] = x - y;
		expected[2] = x * y;
		expected[3] = x & y;
		expected[4] = x | y;
		expected[5] = x ^ y;
		expected[6] = x << shift;
		expected[7] = x >> shift;
		expected[8] = oracle_signed_shift(x, shift);
		expected[9] = ~x + (0U - y);
		for (result = 0; result < 10; result++)
			oracle_expect(check, 2, index * 10 + result, words[2][index * 10 + result], expected[result]);
	}
}

/* Checks s1-int-div: unsigned and signed quotients and remainders. */
static void
oracle_check_int_div(
	struct oracle_check *check,
	const struct oracle_test *test,
	uint32_t *const *words)
{
	uint32_t expected[4];
	uint32_t index;
	uint32_t result;
	uint32_t x;
	uint32_t y;

	/* Computes each pair's quotients and remainders, and compares them. */
	for (index = 0; index < test->buffers[0].words; index++) {
		x = oracle_input(0, index);
		y = oracle_div_divisor(index);
		expected[0] = x / y;
		expected[1] = x % y;
		expected[2] = oracle_signed_divide(x, y);
		expected[3] = (x & 0x7fffffffU) % (y & 0x7fffffffU);
		for (result = 0; result < 4; result++)
			oracle_expect(check, 2, index * 4 + result, words[2][index * 4 + result], expected[result]);
	}
}

/* Checks s1-int-cmp: comparisons, the selection, minima, maxima and the clamp. */
static void
oracle_check_int_cmp(
	struct oracle_check *check,
	const struct oracle_test *test,
	uint32_t *const *words)
{
	uint32_t expected[8];
	uint32_t index;
	uint32_t result;
	uint32_t x;
	uint32_t y;
	int32_t signed_x;
	int32_t signed_y;
	int32_t clamped;

	/* Computes each pair's results as the shader does, and compares them. */
	for (index = 0; index < test->buffers[0].words; index++) {
		x = oracle_input(0, index);
		y = oracle_input(1, index);
		signed_x = oracle_signed(x);
		signed_y = oracle_signed(y);

		/* The unsigned comparison, then the signed one, then the selection on equality. */
		expected[0] = 0;
		if (x < y)
			expected[0] = 1;
		expected[1] = 0;
		if (signed_x < signed_y)
			expected[1] = 1;
		expected[2] = 9;
		if (x == y)
			expected[2] = 7;

		/* The unsigned minimum and maximum. */
		expected[3] = y;
		expected[4] = x;
		if (x < y) {
			expected[3] = x;
			expected[4] = y;
		}

		/* The signed minimum and maximum. */
		expected[5] = y;
		expected[6] = x;
		if (signed_x < signed_y) {
			expected[5] = x;
			expected[6] = y;
		}

		/* The clamp to -1000 to 1000. */
		clamped = signed_x;
		if (clamped < -1000)
			clamped = -1000;
		if (clamped > 1000)
			clamped = 1000;
		expected[7] = (uint32_t)clamped;

		/* Compares the eight results. */
		for (result = 0; result < 8; result++)
			oracle_expect(check, 2, index * 8 + result, words[2][index * 8 + result], expected[result]);
	}
}

/* Checks s1-float-arith: ten exact fp32 results per operand pair, bit for bit. */
static void
oracle_check_float_arith(
	struct oracle_check *check,
	const struct oracle_test *test,
	uint32_t *const *words)
{
	float expected[10];
	uint32_t index;
	uint32_t result;
	float x;
	float y;
	float floor_x;

	/* Computes each pair's results, which the operands keep exact, and compares them. */
	for (index = 0; index < test->buffers[0].words; index++) {
		x = oracle_float_operand(0, index);
		y = oracle_float_operand(1, index);

		/* Rounds x down: the conversion truncates, so a negative fraction steps one lower. */
		floor_x = (float)(int32_t)x;
		if (floor_x > x)
			floor_x = floor_x - 1.0f;

		/* The arithmetic, the negation and the absolute value. */
		expected[0] = x + y;
		expected[1] = x - y;
		expected[2] = x * y;
		expected[3] = -x;
		expected[4] = x;
		if (x < 0.0f)
			expected[4] = -x;

		/* The minimum and maximum as GLSL defines them (y unless x is the smaller or larger). */
		expected[5] = y;
		expected[6] = x;
		if (x < y) {
			expected[5] = x;
			expected[6] = y;
		}

		/* The floor, the comparison and the multiply-add. */
		expected[7] = floor_x;
		expected[8] = 0.0f;
		if (x > y)
			expected[8] = 1.0f;
		expected[9] = x * y + x;

		/* Compares the ten results bit for bit. */
		for (result = 0; result < 10; result++) {
			oracle_expect(
				check,
				2,
				index * 10 + result,
				words[2][index * 10 + result],
				oracle_float_bits(expected[result]));
		}
	}
}

/* Checks s1-float-convert: conversions both ways and bit casts. */
static void
oracle_check_float_convert(
	struct oracle_check *check,
	const struct oracle_test *test,
	uint32_t *const *words)
{
	uint32_t expected[6];
	uint32_t index;
	uint32_t result;
	uint32_t x;
	float y;
	float magnitude;

	/* Computes each pair's conversions, all exact for these operands, and compares them. */
	for (index = 0; index < test->buffers[0].words; index++) {
		x = oracle_convert_integer(index);
		y = oracle_convert_float(index);
		magnitude = y;
		if (magnitude < 0.0f)
			magnitude = -magnitude;
		expected[0] = oracle_float_bits((float)oracle_signed(x));
		expected[1] = oracle_float_bits((float)(x >> 8));
		expected[2] = (uint32_t)(int32_t)y;
		expected[3] = (uint32_t)magnitude;
		expected[4] = oracle_float_bits(y);
		expected[5] = oracle_float_bits(oracle_bits_float((x & 0x00ffffffU) | 0x3f000000U) * 2.0f);
		for (result = 0; result < 6; result++)
			oracle_expect(check, 2, index * 6 + result, words[2][index * 6 + result], expected[result]);
	}
}

/* Checks s1-float-div: quotients within 3 ULP and square roots within 5 ULP of the correctly rounded value. */
static void
oracle_check_float_div(
	struct oracle_check *check,
	const struct oracle_test *test,
	uint32_t *const *words)
{
	uint32_t index;
	float x;
	float y;
	double magnitude;
	float quotient;
	float root;

	/* Computes each pair's quotient and root in double precision, rounds them to fp32 and compares. */
	for (index = 0; index < test->buffers[0].words; index++) {
		x = oracle_div_operand(0, index);
		y = oracle_div_operand(1, index);
		magnitude = (double)x;
		if (magnitude < 0.0)
			magnitude = -magnitude;
		quotient = (float)((double)x / (double)y);
		root = (float)oracle_square_root(magnitude);
		oracle_expect_near(check, 2, index * 2, words[2][index * 2], quotient, ORACLE_DIVIDE_ULPS);
		oracle_expect_near(check, 2, index * 2 + 1, words[2][index * 2 + 1], root, ORACLE_SQRT_ULPS);
	}
}

/* Checks s1-ids-1d, -2d and -3d: each slot holds the IDs and the local index of the invocation it belongs to. */
static void
oracle_check_ids(
	struct oracle_check *check,
	const struct oracle_test *test,
	uint32_t *const *words)
{
	uint32_t expected[10];
	uint32_t width;
	uint32_t height;
	uint32_t depth;
	uint32_t slot;
	uint32_t result;
	uint32_t x;
	uint32_t y;
	uint32_t z;

	/* Walks the global grid in the order the shader lays out its slots. */
	width = test->groups[0] * test->local[0];
	height = test->groups[1] * test->local[1];
	depth = test->groups[2] * test->local[2];
	for (slot = 0; slot < width * height * depth; slot++) {
		x = slot % width;
		y = (slot / width) % height;
		z = slot / (width * height);

		/* The global, local and workgroup IDs and the local index this invocation must have seen. */
		expected[0] = x;
		expected[1] = y;
		expected[2] = z;
		expected[3] = x % test->local[0];
		expected[4] = y % test->local[1];
		expected[5] = z % test->local[2];
		expected[6] = x / test->local[0];
		expected[7] = y / test->local[1];
		expected[8] = z / test->local[2];
		expected[9] = (expected[5] * test->local[1] + expected[4]) * test->local[0] + expected[3];

		/* Compares the ten words of the slot. */
		for (result = 0; result < 10; result++)
			oracle_expect(check, 0, slot * 10 + result, words[0][slot * 10 + result], expected[result]);
	}
}

/* Checks s1-num-groups: every invocation saw the dispatch's workgroup counts and the workgroup size. */
static void
oracle_check_num_groups(
	struct oracle_check *check,
	const struct oracle_test *test,
	uint32_t *const *words)
{
	uint32_t slots;
	uint32_t slot;
	uint32_t axis;

	/* Compares the six words of each invocation's slot. */
	slots = test->groups[0] * test->local[0] * test->groups[1] * test->local[1] * test->groups[2] * test->local[2];
	for (slot = 0; slot < slots; slot++) {
		for (axis = 0; axis < 3; axis++) {
			oracle_expect(check, 0, slot * 6 + axis, words[0][slot * 6 + axis], test->groups[axis]);
			oracle_expect(check, 0, slot * 6 + 3 + axis, words[0][slot * 6 + 3 + axis], test->local[axis]);
		}
	}
}

/* Checks s1-push: the integer and the fp32 results of the four push constants. */
static void
oracle_check_push(
	struct oracle_check *check,
	const struct oracle_test *test,
	uint32_t *const *words)
{
	const uint32_t *push;
	uint32_t index;
	uint32_t expected;
	float scale;

	/* Computes what each invocation made of the push constants. */
	push = test->passes[0].push;
	scale = oracle_bits_float(push[2]);
	for (index = 0; index < test->buffers[0].words; index++) {
		expected = (index * push[0] + push[1]) ^ push[3];
		oracle_expect(check, 0, index, words[0][index], expected);
		oracle_expect(check, 1, index, words[1][index], oracle_float_bits((float)index * scale));
	}
}

/* Checks s1-buffers: o = a + b * c over the three input buffers. */
static void
oracle_check_buffers(
	struct oracle_check *check,
	const struct oracle_test *test,
	uint32_t *const *words)
{
	uint32_t index;
	uint32_t expected;

	/* Computes each word from the three inputs. */
	for (index = 0; index < test->buffers[3].words; index++) {
		expected = oracle_input(0, index) + oracle_input(1, index) * oracle_input(2, index);
		oracle_expect(check, 3, index, words[3][index], expected);
	}
}

/* Checks s1-sets: o = 2a + b, with a in set 0 and b and o in set 1. */
static void
oracle_check_sets(
	struct oracle_check *check,
	const struct oracle_test *test,
	uint32_t *const *words)
{
	uint32_t index;
	uint32_t expected;

	/* Computes each word from the two inputs. */
	for (index = 0; index < test->buffers[2].words; index++) {
		expected = oracle_input(0, index) * 2U + oracle_input(1, index);
		oracle_expect(check, 2, index, words[2][index], expected);
	}
}

/* Checks s1-size-1 to s1-size-128: each word names its global index, its local index and its workgroup. */
static void
oracle_check_size(
	struct oracle_check *check,
	const struct oracle_test *test,
	uint32_t *const *words)
{
	uint32_t index;
	uint32_t expected;
	uint32_t local;
	uint32_t group;

	/* Computes each invocation's word from the workgroup size. */
	for (index = 0; index < test->buffers[0].words; index++) {
		local = index % test->local[0];
		group = index / test->local[0];
		expected = (index * 0x9e3779b1U) ^ (local << 20) ^ (group << 8);
		oracle_expect(check, 0, index, words[0][index], expected);
	}
}

/* Checks s1-dispatches: each dispatch's range holds its words, and what follows the last range is untouched. */
static void
oracle_check_dispatches(
	struct oracle_check *check,
	const struct oracle_test *test,
	uint32_t *const *words)
{
	uint32_t invocations;
	uint32_t pass;
	uint32_t index;
	uint32_t base;
	uint32_t multiplier;
	uint32_t end;

	/* Computes each dispatch's range from its push constants. */
	invocations = test->groups[0] * test->local[0];
	end = 0;
	for (pass = 0; pass < test->pass_count; pass++) {
		base = test->passes[pass].push[0];
		multiplier = test->passes[pass].push[1];
		for (index = 0; index < invocations; index++)
			oracle_expect(check, 0, base + index, words[0][base + index], index * multiplier + base);
		end = base + invocations;
	}

	/* The words after the last range were never written. */
	oracle_expect_untouched(check, words, 0, end, test->buffers[0].words);
}

/* Checks s1-chain: simulates the three dependent dispatches and compares the buffer they leave. */
static void
oracle_check_chain(
	struct oracle_check *check,
	const struct oracle_test *test,
	uint32_t *const *words)
{
	uint32_t data[8192];
	uint32_t pass;
	uint32_t index;
	uint32_t source;
	uint32_t destination;
	uint32_t next;
	uint32_t invocations;

	/* Starts from the inputs in the first half and the sentinel in the second. */
	invocations = test->groups[0] * test->local[0];
	for (index = 0; index < test->buffers[0].words; index++)
		data[index] = ORACLE_SENTINEL;
	for (index = 0; index < invocations; index++)
		data[index] = oracle_input(0, index);

	/* Runs each dispatch in order, each reading only what the one before left. */
	for (pass = 0; pass < test->pass_count; pass++) {
		source = test->passes[pass].push[0];
		destination = test->passes[pass].push[1];
		for (index = 0; index < invocations; index++) {
			next = (index + 1) & 4095U;
			data[destination + index] = data[source + index] * 2U + data[source + next] + test->passes[pass].push[2];
		}
	}

	/* Compares the whole buffer. */
	for (index = 0; index < test->buffers[0].words; index++)
		oracle_expect(check, 0, index, words[0][index], data[index]);
}

/* Checks s1-branch: the nested if and else result, and the store only every eighth invocation makes. */
static void
oracle_check_branch(
	struct oracle_check *check,
	const struct oracle_test *test,
	uint32_t *const *words)
{
	uint32_t index;
	uint32_t value;
	uint32_t expected;

	/* Computes each invocation's branch result and its conditional store. */
	for (index = 0; index < test->buffers[0].words; index++) {
		value = oracle_input(0, index);

		/* An odd value takes the first arm and an even one the second. */
		if ((value & 1U) != 0) {
			expected = value * 3U + 1U;
		} else {
			expected = value >> 1;
		}

		/* A multiple of 3 adds 100, and a multiple of 9 then flips the low byte. */
		if (value % 3U == 0) {
			expected = expected + 100U;
			if (value % 9U == 0)
				expected = expected ^ 255U;
		}
		oracle_expect(check, 1, index, words[1][index], expected);

		/* Only the invocations at 3 modulo 8 store; the others leave the sentinel. */
		expected = ORACLE_SENTINEL;
		if ((index & 7U) == 3U)
			expected = value;
		oracle_expect(check, 2, index, words[2][index], expected);
	}
}

/* Checks s1-loop: the counted sum, the bounded Collatz steps and the lowest set bit. */
static void
oracle_check_loop(
	struct oracle_check *check,
	const struct oracle_test *test,
	uint32_t *const *words)
{
	uint32_t index;
	uint32_t value;
	uint32_t count;
	uint32_t step;
	uint32_t sum;
	uint32_t walk;
	uint32_t steps;
	uint32_t bit;

	/* Runs each invocation's three loops in C. */
	for (index = 0; index < test->buffers[0].words; index++) {
		value = oracle_input(0, index);

		/* Sums over a trip count that differs per invocation. */
		count = value % 17U;
		sum = 0;
		for (step = 0; step < count; step++)
			sum = sum + step * value + 1U;
		oracle_expect(check, 1, index * 3, words[1][index * 3], sum);

		/* Counts the Collatz steps, stopped after 64. */
		walk = (value & 1023U) + 1U;
		steps = 0;
		while (walk != 1U && steps < 64U) {
			if ((walk & 1U) != 0) {
				walk = walk * 3U + 1U;
			} else {
				walk = walk >> 1;
			}
			steps++;
		}
		oracle_expect(check, 1, index * 3 + 1, words[1][index * 3 + 1], steps);

		/* Finds the lowest set bit, or 32 for none. */
		bit = 32;
		for (step = 0; step < 32; step++) {
			if (((value >> step) & 1U) != 0) {
				bit = step;
				break;
			}
		}
		oracle_expect(check, 1, index * 3 + 2, words[1][index * 3 + 2], bit);
	}
}

/* Checks s2-neighbor: each word combines two other invocations' inputs of the same workgroup. */
static void
oracle_check_neighbor(
	struct oracle_check *check,
	const struct oracle_test *test,
	uint32_t *const *words)
{
	uint32_t index;
	uint32_t base;
	uint32_t local;
	uint32_t expected;

	/* Computes each word from the inputs the shader exchanged through shared memory. */
	for (index = 0; index < test->buffers[1].words; index++) {
		local = index % 64U;
		base = index - local;
		expected = oracle_input(0, base + ((local + 1U) & 63U)) * 3U + oracle_input(0, base + 63U - local);
		oracle_expect(check, 1, index, words[1][index], expected);
	}
}

/* Checks s2-reduce: one sum per workgroup of 128 inputs. */
static void
oracle_check_reduce(
	struct oracle_check *check,
	const struct oracle_test *test,
	uint32_t *const *words)
{
	uint32_t group;
	uint32_t index;
	uint32_t sum;

	/* Sums each workgroup's inputs, wrapping around as the shader does. */
	for (group = 0; group < test->groups[0]; group++) {
		sum = 0;
		for (index = 0; index < test->local[0]; index++)
			sum = sum + oracle_input(0, group * test->local[0] + index);
		oracle_expect(check, 1, group, words[1][group], sum);
	}
}

/* Checks s2-scan: the inclusive prefix sum within each workgroup of 64. */
static void
oracle_check_scan(
	struct oracle_check *check,
	const struct oracle_test *test,
	uint32_t *const *words)
{
	uint32_t index;
	uint32_t sum;

	/* Restarts the sum at each workgroup and compares every partial sum. */
	sum = 0;
	for (index = 0; index < test->buffers[1].words; index++) {
		if (index % test->local[0] == 0)
			sum = 0;
		sum = sum + oracle_input(0, index);
		oracle_expect(check, 1, index, words[1][index], sum);
	}
}

/*
 * Checks s2-shared-1k, -4k and -16k: simulates the shared array each
 * workgroup fills in rows (each invocation its own row of words) and reads
 * back in columns (each invocation one column, from every other row).
 */
static void
oracle_check_shared_rows(
	struct oracle_check *check,
	const struct oracle_test *test,
	uint32_t *const *words)
{
	uint32_t shared_words[4096];
	uint32_t invocations;
	uint32_t row_words;
	uint32_t group;
	uint32_t local;
	uint32_t step;
	uint32_t sum;
	uint32_t global;

	/* Splits the shader's shared words into one row per invocation. */
	invocations = test->local[0];
	row_words = test->shared_bytes / (uint32_t)sizeof(uint32_t) / invocations;

	/* Fills each workgroup's array, then sums each column as the shader does. */
	for (group = 0; group < test->groups[0]; group++) {
		for (local = 0; local < invocations; local++) {
			for (step = 0; step < row_words; step++)
				shared_words[local * row_words + step] = oracle_input(0, group * invocations + local) + step;
		}
		for (local = 0; local < invocations; local++) {
			sum = 0;
			for (step = 0; step < row_words; step++)
				sum = sum + shared_words[step * invocations + local] * (step + 1U);
			global = group * invocations + local;
			oracle_expect(check, 1, global, words[1][global], sum);
		}
	}
}

/* Checks s2-transpose: each 16 x 8 tile comes out transposed into 8 x 16. */
static void
oracle_check_transpose(
	struct oracle_check *check,
	const struct oracle_test *test,
	uint32_t *const *words)
{
	uint32_t tile;
	uint32_t index;
	uint32_t column;
	uint32_t row;
	uint32_t source_x;
	uint32_t source_y;
	uint32_t expected;

	/* Finds, for each output word, the input word the tile's transpose puts there. */
	for (tile = 0; tile < test->groups[0] * test->groups[1]; tile++) {
		for (index = 0; index < 128; index++) {
			column = index / 8U;
			row = index % 8U;
			source_x = (tile % 2U) * 16U + column;
			source_y = (tile / 2U) * 8U + row;
			expected = oracle_input(0, source_y * 32U + source_x);
			oracle_expect(check, 1, tile * 128 + index, words[1][tile * 128 + index], expected);
		}
	}
}

/* Checks s2-shared-float: fp32 values from two other invocations, combined exactly. */
static void
oracle_check_shared_float(
	struct oracle_check *check,
	const struct oracle_test *test,
	uint32_t *const *words)
{
	uint32_t index;
	uint32_t base;
	uint32_t local;
	float first;
	float second;

	/* Computes each word from the two values the shader read from shared memory. */
	for (index = 0; index < test->buffers[1].words; index++) {
		local = index % 64U;
		base = index - local;
		first = (float)(oracle_input(0, base + (local ^ 1U)) & 1023U) * 0.5f;
		second = (float)(oracle_input(0, base + (local ^ 32U)) & 1023U) * 0.5f;
		oracle_expect(check, 1, index, words[1][index], oracle_float_bits(first + second * 0.25f));
	}
}

/* Checks s3-buffer-add: the counter's total, the eight sums, and every old value returned exactly once. */
static void
oracle_check_buffer_add(
	struct oracle_check *check,
	const struct oracle_test *test,
	uint32_t *const *words)
{
	uint32_t seen[ORACLE_MAX_INVOCATIONS];
	uint32_t sums[8];
	uint32_t invocations;
	uint32_t index;
	uint32_t old;

	/* Computes the sums and counts how often each old value came back; one out of range is wrong. */
	invocations = test->groups[0] * test->local[0];
	memset(seen, 0, sizeof(seen));
	memset(sums, 0, sizeof(sums));
	for (index = 0; index < invocations; index++) {
		sums[index & 7U] = sums[index & 7U] + index;
		old = words[1][index];
		if (old < invocations) {
			seen[old]++;
		} else {
			oracle_expect(check, 1, index, old, invocations - 1);
		}
	}

	/* Compares the counter and the sums. */
	oracle_expect(check, 0, 0, words[0][0], invocations);
	for (index = 0; index < 8; index++)
		oracle_expect(check, 0, 1 + index, words[0][1 + index], sums[index]);

	/* Each old value from 0 to N - 1 must have come back once. */
	for (index = 0; index < invocations; index++)
		oracle_expect(check, 1, index, seen[index], 1);
}

/* Checks s3-buffer-minmax: the unsigned and signed minimum and maximum of each quarter of the inputs. */
static void
oracle_check_buffer_minmax(
	struct oracle_check *check,
	const struct oracle_test *test,
	uint32_t *const *words)
{
	uint32_t unsigned_min[4];
	uint32_t unsigned_max[4];
	int32_t signed_min[4];
	int32_t signed_max[4];
	uint32_t index;
	uint32_t slot;
	uint32_t value;
	int32_t signed_value;

	/* Starts each slot where the fill started it. */
	for (slot = 0; slot < 4; slot++) {
		unsigned_min[slot] = 0xffffffffU;
		unsigned_max[slot] = 0;
		signed_min[slot] = 0x7fffffff;
		signed_max[slot] = -0x7fffffff - 1;
	}

	/* Folds each input into its slot. */
	for (index = 0; index < test->buffers[0].words; index++) {
		value = oracle_input(0, index);
		signed_value = oracle_signed(value);
		slot = index & 3U;
		if (value < unsigned_min[slot])
			unsigned_min[slot] = value;
		if (value > unsigned_max[slot])
			unsigned_max[slot] = value;
		if (signed_value < signed_min[slot])
			signed_min[slot] = signed_value;
		if (signed_value > signed_max[slot])
			signed_max[slot] = signed_value;
	}

	/* Compares the sixteen results. */
	for (slot = 0; slot < 4; slot++) {
		oracle_expect(check, 1, slot, words[1][slot], unsigned_min[slot]);
		oracle_expect(check, 1, 4 + slot, words[1][4 + slot], unsigned_max[slot]);
		oracle_expect(check, 2, slot, words[2][slot], (uint32_t)signed_min[slot]);
		oracle_expect(check, 2, 4 + slot, words[2][4 + slot], (uint32_t)signed_max[slot]);
	}
}

/* Checks s3-buffer-bits: the and, or and xor of each quarter of the inputs. */
static void
oracle_check_buffer_bits(
	struct oracle_check *check,
	const struct oracle_test *test,
	uint32_t *const *words)
{
	uint32_t ands[4];
	uint32_t ors[4];
	uint32_t xors[4];
	uint32_t index;
	uint32_t slot;
	uint32_t value;
	uint32_t bit;

	/* Starts each slot where the fill started it. */
	for (slot = 0; slot < 4; slot++) {
		ands[slot] = 0xffffffffU;
		ors[slot] = 0;
		xors[slot] = 0x12345678U;
	}

	/* Folds each input into its slot as the shader does. */
	for (index = 0; index < test->buffers[0].words; index++) {
		value = oracle_input(0, index);
		slot = index & 3U;
		bit = 1U << ((index >> 2) & 31U);
		ands[slot] = ands[slot] & (value | bit);
		ors[slot] = ors[slot] | (value & bit);
		xors[slot] = xors[slot] ^ value;
	}

	/* Compares the twelve results. */
	for (slot = 0; slot < 4; slot++) {
		oracle_expect(check, 1, slot, words[1][slot], ands[slot]);
		oracle_expect(check, 1, 4 + slot, words[1][4 + slot], ors[slot]);
		oracle_expect(check, 1, 8 + slot, words[1][8 + slot], xors[slot]);
	}
}

/* Checks s3-buffer-exchange: per slot, the old values and the final value are the start value and every stored value, once each. */
static void
oracle_check_buffer_exchange(
	struct oracle_check *check,
	const struct oracle_test *test,
	uint32_t *const *words)
{
	uint32_t seen[ORACLE_MAX_INVOCATIONS + 1];
	uint32_t zeros[16];
	uint32_t invocations;
	uint32_t index;
	uint32_t value;
	uint32_t slot;

	/* Counts every value that came back from a slot, and every value a slot holds at the end. */
	invocations = test->groups[0] * test->local[0];
	memset(seen, 0, sizeof(seen));
	memset(zeros, 0, sizeof(zeros));
	for (index = 0; index < invocations + 16; index++) {
		/* The old values first, then the slots' final values. */
		if (index < invocations) {
			slot = index & 15U;
			value = words[1][index];
		} else {
			slot = index - invocations;
			value = words[0][slot];
		}

		/* The start value 0 counts per slot; a stored value only counts out of its own slot. */
		if (value == 0) {
			zeros[slot]++;
		} else if (value <= invocations && ((value - 1U) & 15U) == slot) {
			seen[value]++;
		} else {
			oracle_expect(check, 1, index, value, slot + 1U);
		}
	}

	/* Compares the counts: each slot's 0 once, each stored value once. */
	for (slot = 0; slot < 16; slot++)
		oracle_expect(check, 0, slot, zeros[slot], 1);
	for (index = 0; index < invocations; index++)
		oracle_expect(check, 1, index, seen[index + 1], 1);
}

/* Checks s3-buffer-compswap: per slot, one invocation found it empty and every other one found the winner's value. */
static void
oracle_check_buffer_compswap(
	struct oracle_check *check,
	const struct oracle_test *test,
	uint32_t *const *words)
{
	uint32_t winners[16];
	uint32_t invocations;
	uint32_t index;
	uint32_t slot;
	uint32_t final_value;

	/* Counts the invocations of each slot that found it empty. */
	invocations = test->groups[0] * test->local[0];
	memset(winners, 0, sizeof(winners));
	for (index = 0; index < invocations; index++) {
		if (words[1][index] == 0)
			winners[index & 15U]++;
	}

	/* Each slot had exactly one winner. */
	for (slot = 0; slot < 16; slot++)
		oracle_expect(check, 0, slot, winners[slot], 1);

	/* Every loser found the value its slot keeps. */
	for (index = 0; index < invocations; index++) {
		slot = index & 15U;
		if (words[1][index] != 0)
			oracle_expect(check, 1, index, words[1][index], words[0][slot]);
	}

	/* The value each slot keeps names an invocation of that slot, and that invocation was the winner. */
	for (slot = 0; slot < 16; slot++) {
		final_value = words[0][slot];
		if (final_value == 0 || final_value > invocations || ((final_value - 1U) & 15U) != slot) {
			oracle_expect(check, 0, slot, final_value, slot + 1U);
		} else {
			oracle_expect(check, 1, final_value - 1U, words[1][final_value - 1U], 0);
		}
	}
}

/* Checks s3-shared-add: each workgroup's counter returned 0 to 127 once, and its four sums and its total. */
static void
oracle_check_shared_add(
	struct oracle_check *check,
	const struct oracle_test *test,
	uint32_t *const *words)
{
	uint32_t seen[128];
	uint32_t sums[4];
	uint32_t group;
	uint32_t local;
	uint32_t old;
	uint32_t slot;

	/* Checks each workgroup on its own. */
	for (group = 0; group < test->groups[0]; group++) {
		memset(seen, 0, sizeof(seen));
		memset(sums, 0, sizeof(sums));

		/* Computes the sums and counts the old values; one out of range is wrong. */
		for (local = 0; local < 128; local++) {
			sums[local & 3U] = sums[local & 3U] + local;
			old = words[0][group * 128 + local];
			if (old < 128) {
				seen[old]++;
			} else {
				oracle_expect(check, 0, group * 128 + local, old, 127);
			}
		}

		/* Each old value once, then the four sums and the total. */
		for (local = 0; local < 128; local++)
			oracle_expect(check, 0, group * 128 + local, seen[local], 1);
		for (slot = 0; slot < 4; slot++)
			oracle_expect(check, 1, group * 5 + slot, words[1][group * 5 + slot], sums[slot]);
		oracle_expect(check, 1, group * 5 + 4, words[1][group * 5 + 4], 128);
	}
}

/* Checks s3-shared-minmax: each workgroup's unsigned and signed minimum and maximum. */
static void
oracle_check_shared_minmax(
	struct oracle_check *check,
	const struct oracle_test *test,
	uint32_t *const *words)
{
	uint32_t group;
	uint32_t local;
	uint32_t value;
	uint32_t unsigned_min;
	uint32_t unsigned_max;
	int32_t signed_value;
	int32_t signed_min;
	int32_t signed_max;

	/* Folds each workgroup's inputs as its atomics did. */
	for (group = 0; group < test->groups[0]; group++) {
		unsigned_min = 0xffffffffU;
		unsigned_max = 0;
		signed_min = 0x7fffffff;
		signed_max = -0x7fffffff - 1;
		for (local = 0; local < 64; local++) {
			value = oracle_input(0, group * 64 + local);
			signed_value = oracle_signed(value);
			if (value < unsigned_min)
				unsigned_min = value;
			if (value > unsigned_max)
				unsigned_max = value;
			if (signed_value < signed_min)
				signed_min = signed_value;
			if (signed_value > signed_max)
				signed_max = signed_value;
		}

		/* Compares the workgroup's four results. */
		oracle_expect(check, 1, group * 4, words[1][group * 4], unsigned_min);
		oracle_expect(check, 1, group * 4 + 1, words[1][group * 4 + 1], unsigned_max);
		oracle_expect(check, 1, group * 4 + 2, words[1][group * 4 + 2], (uint32_t)signed_min);
		oracle_expect(check, 1, group * 4 + 3, words[1][group * 4 + 3], (uint32_t)signed_max);
	}
}

/* Checks s3-shared-xchg: per workgroup, the chain of exchanges and the single compare-and-swap winner. */
static void
oracle_check_shared_xchg(
	struct oracle_check *check,
	const struct oracle_test *test,
	uint32_t *const *words)
{
	uint32_t seen[65];
	uint32_t group;
	uint32_t local;
	uint32_t global;
	uint32_t value;
	uint32_t swapped;
	uint32_t final_swap;
	uint32_t winners;

	/* Checks each workgroup on its own. */
	for (group = 0; group < test->groups[0]; group++) {
		memset(seen, 0, sizeof(seen));
		winners = 0;
		final_swap = words[1][group * 2 + 1];

		/* Counts the exchanged values (the 64 returned and the one left) and the swap's winners. */
		for (local = 0; local <= 64; local++) {
			global = group * 64 + local;

			/* The value the exchange returned to this invocation, or the one left at the end. */
			if (local < 64) {
				value = words[0][global * 2];
			} else {
				value = words[1][group * 2];
			}

			/* Counts a value in range; one out of range is wrong. */
			if (value <= 64) {
				seen[value]++;
			} else {
				oracle_expect(check, 0, global * 2, value, 64);
			}

			/* The compare-and-swap: 0 for the winner, the winner's value for everyone else. */
			if (local < 64) {
				swapped = words[0][global * 2 + 1];
				if (swapped == 0) {
					winners++;
				} else {
					oracle_expect(check, 0, global * 2 + 1, swapped, final_swap);
				}
			}
		}

		/* The exchanges gave back 0 and every stored value once each; the swap had one winner. */
		for (value = 0; value <= 64; value++)
			oracle_expect(check, 0, group * 128 + value, seen[value], 1);
		oracle_expect(check, 1, group * 2 + 1, winners, 1);

		/* The value the swap left names an invocation of the workgroup, and that one was the winner. */
		if (final_swap == 0 || final_swap > 64) {
			oracle_expect(check, 1, group * 2 + 1, final_swap, 1);
		} else {
			global = group * 64 + final_swap - 1U;
			oracle_expect(check, 0, global * 2 + 1, words[0][global * 2 + 1], 0);
		}
	}
}

/* The graphics checks (-g); a host that answers only the compute calls defines VKCOMPUTE_NO_GRAPHICS. */
#ifndef VKCOMPUTE_NO_GRAPHICS
#include "graphics.inc"
#endif
