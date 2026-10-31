/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The native shader compiler checked against the source program (WS141).
 *
 * Every Keiland and app shader in the tree is compiled for each native stage
 * and pipeline variant.  The words run on the timing model of one QPU lane
 * (qpu-machine.c, read with the independent MIT Mesa decoder), and each
 * output is compared, bit for bit, with an independent interpretation of the
 * shared frontend's scalar source program over deterministic and random
 * inputs.  The table of native program sizes is printed (WS141 p013).
 */
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "broadcom/common/v3d_device_info.h"
#include "broadcom/qpu/qpu_instr.h"
#include "drivers/gpu/bcm2711/compiler/shader.h"
#include "drivers/gpu/compiler/optimize.h"
#include "drivers/gpu/compiler/spirv.h"
#include <uapi/errno.h>
#include "plan/tools/v3d-qpu/qpu-machine.h"
#include "userland/desktop/wayland/shaders.h"
#include "userland/tests/mview/shaders.h"
#include "userland/tests/vkdemo/shaders.h"
#include "userland/tests/egltest/shaders.h"
#include "userland/desktop/files/shaders.h"
#include "userland/desktop/monitor/shaders.h"
#include "userland/desktop/libbrowser/paint/shaders.h"
#include "userland/desktop/notes/shaders.h"
#include "userland/desktop/libkeiland/ui/shaders.h"
#include "userland/desktop/libGL/shaders.h"
#include "userland/desktop/terminal/shaders.h"
#include "userland/desktop/imageview/shaders.h"

/* Random trials per native program, besides the deterministic Keiland ones. */
#define RANDOM_TRIALS 96U

/* One host-only bit conversion preserves exact scalar representations without aliasing violations. */
union scalar_bits {
	uint32_t bits;
	float number;
};

/* One evaluation's inputs and outputs, shared by the source interpreter and the native run. */
struct shader_inputs {
	/* Inputs by location times four plus component. */
	uint32_t input[64];
	uint32_t push[64];
	uint32_t output[68];
	uint32_t lookups;
	/* Varies the uniform block words between trials. */
	uint32_t block_seed;
	/* Set when the source program discarded the fragment. */
	uint32_t discarded;
};

/* One vertex and fragment module pair of the tree; a module the compiler refuses is named so. */
struct shader_pair {
	const char *name;
	const uint32_t *vertex;
	size_t vertex_bytes;
	const uint32_t *fragment;
	size_t fragment_bytes;
	/* Set when the native compiler is expected to refuse the fragment or the vertex module. */
	uint32_t fragment_refused;
	uint32_t vertex_refused;
};

/* Every module pair of the tree that the native compiler may meet. */
static const struct shader_pair shader_pairs[] = {
	{"kwl quad", kwl_quad_vert, sizeof(kwl_quad_vert), kwl_quad_frag, sizeof(kwl_quad_frag), 0, 0},
	{"kwl panel", kwl_panel_vert, sizeof(kwl_panel_vert), kwl_panel_frag, sizeof(kwl_panel_frag), 0, 0},
	{"kwl batch", kwl_batch_vert, sizeof(kwl_batch_vert), kwl_batch_frag, sizeof(kwl_batch_frag), 0, 0},
	{"mview", mview_vertex_shader, sizeof(mview_vertex_shader), mview_fragment_shader, sizeof(mview_fragment_shader), 0, 0},
	{"mview cutout", mview_vertex_shader, sizeof(mview_vertex_shader), mview_cutout_shader, sizeof(mview_cutout_shader), 0, 0},
	{"mview pixel", mview_pixel_vertex_shader, sizeof(mview_pixel_vertex_shader), mview_pixel_fragment_shader, sizeof(mview_pixel_fragment_shader), 1, 0},
	{"mview pixel cutout", mview_pixel_vertex_shader, sizeof(mview_pixel_vertex_shader), mview_pixel_cutout_shader, sizeof(mview_pixel_cutout_shader), 1, 0},
	{"vkdemo", vkdemo_vertex_shader, sizeof(vkdemo_vertex_shader), vkdemo_fragment_shader, sizeof(vkdemo_fragment_shader), 0, 1},
	{"egltest", egltest_scene_vert, sizeof(egltest_scene_vert), egltest_scene_frag, sizeof(egltest_scene_frag), 0, 0},
	{"files", files_canvas_vert, sizeof(files_canvas_vert), files_canvas_frag, sizeof(files_canvas_frag), 0, 0},
	{"monitor shape", monitor_draw_vert, sizeof(monitor_draw_vert), monitor_shape_frag, sizeof(monitor_shape_frag), 0, 0},
	{"monitor glyph", monitor_draw_vert, sizeof(monitor_draw_vert), monitor_glyph_frag, sizeof(monitor_glyph_frag), 0, 0},
	{"browser paint", paint_display_vert, sizeof(paint_display_vert), paint_display_frag, sizeof(paint_display_frag), 0, 0},
	{"notes fill", notes_draw_vert, sizeof(notes_draw_vert), notes_fill_frag, sizeof(notes_fill_frag), 0, 0},
	{"notes texture", notes_draw_vert, sizeof(notes_draw_vert), notes_texture_frag, sizeof(notes_texture_frag), 0, 0},
	{"keiland ui", keiui_canvas_vert, sizeof(keiui_canvas_vert), keiui_canvas_frag, sizeof(keiui_canvas_frag), 0, 0},
	{"libGL 1", fixed_vert_1, sizeof(fixed_vert_1), fixed_frag, sizeof(fixed_frag), 0, 0},
	{"libGL 2", fixed_vert_2, sizeof(fixed_vert_2), fixed_frag, sizeof(fixed_frag), 0, 0},
	{"libGL 4", fixed_vert_4, sizeof(fixed_vert_4), fixed_frag, sizeof(fixed_frag), 0, 0},
	{"libGL 8", fixed_vert_8, sizeof(fixed_vert_8), fixed_frag, sizeof(fixed_frag), 0, 0},
	{"libGL flat", fixed_flat_vert, sizeof(fixed_flat_vert), fixed_flat_frag, sizeof(fixed_flat_frag), 0, 0},
	{"terminal", terminal_cell_vert, sizeof(terminal_cell_vert), terminal_cell_frag, sizeof(terminal_cell_frag), 0, 0},
	{"imageview", imageview_canvas_vert, sizeof(imageview_canvas_vert), imageview_canvas_frag, sizeof(imageview_canvas_frag), 0, 0},
};

/* Allocation accounting observes actual parser/compiler cleanup without any production test control. */
static size_t allocations_live;

/* One host allocation attempt can be refused to exercise the ordinary unpublished-program unwind. */
static size_t allocation_attempt;
static size_t allocation_refusal;

/* The random generator's state, reseeded for each program and trial. */
static uint32_t random_state;

/*
 * The hand-built source program of the optimizer test (WS141 p013).
 *
 * optimizer_program() fills it from the start for each stage; it lives in
 * static storage because the source program description only points at it.
 */
static struct drv_gpu_shader_ir_inst optimizer_code[192];
static uint32_t optimizer_count;
static uint32_t optimizer_values;

/* The program and trial under test, named by a failure. */
static const char *current_program;
static uint32_t current_trial;

/* The trials whose fragment the source program discarded, counted for each program. */
static uint32_t discards;

static uint32_t float_bits(float number);
static float bits_float(uint32_t bits);
static uint32_t truth(int condition);
static uint32_t half_bits(float number);
static uint32_t next_random(void);
static uint32_t random_float(void);
static uint32_t sample_texel(uint32_t set, uint32_t binding, uint32_t s, uint32_t t, uint32_t component);
static uint32_t block_word(uint32_t seed, uint32_t set, uint32_t binding, uint32_t offset);
static uint32_t native_square_root(uint32_t bits);
static int32_t float_to_signed(float number);
static uint32_t float_to_unsigned(float number);
static void source_program(const struct drv_gpu_shader_ir *ir, struct shader_inputs *inputs);
static uint32_t machine_uniform(void *context, const struct bcm2711_shader_uniform *uniform);
static void machine_sample(void *context, uint32_t set, uint32_t binding, uint32_t s, uint32_t t, uint32_t *texel);
static void pipeline_key(const struct drv_gpu_shader_ir *ir, struct bcm2711_shader_key *key);
static void desktop_inputs(struct shader_inputs *inputs, uint32_t fragment, uint32_t trial);
static void random_inputs(struct shader_inputs *inputs, uint32_t seed);
static void verify_panel_mode(const struct drv_gpu_shader_ir *generic, const struct drv_gpu_shader_ir *specialized, uint32_t mode);
static void glass_constants(uint32_t *push);
static void compare_number(uint32_t native, uint32_t expected);
static void verify_program(const struct v3d_device_info *device, const char *name, const uint32_t *words, size_t word_count, const struct drv_gpu_shader_ir *ir, enum bcm2711_shader_stage stage, struct bcm2711_shader_key *key, uint32_t keiland);
static void run_trial(const struct v3d_device_info *device, const struct bcm2711_shader_binary *binary, const struct drv_gpu_shader_ir *ir, const struct bcm2711_shader_key *key, struct shader_inputs *inputs, uint32_t exact_viewport);
static void verify_window(const struct v3d_device_info *device, const struct bcm2711_shader_binary *binary);
static void report_size(const struct v3d_device_info *device, const char *name, const struct bcm2711_shader_binary *binary);
static void verify_pair(const struct v3d_device_info *device, const struct shader_pair *pair);
static void verify_allocation_refusal(const struct bcm2711_shader_key *key, const uint32_t *words, size_t word_count);
static void verify_graphics_profile(const struct bcm2711_shader_key *key);
static uint32_t optimizer_emit(enum drv_gpu_shader_ir_op operation, uint32_t first, uint32_t second, uint32_t third);
static uint32_t optimizer_constant(enum drv_gpu_shader_ir_op operation, uint32_t bits);
static uint32_t optimizer_load(uint32_t offset);
static void optimizer_store(uint32_t location, uint32_t component, uint32_t value);
static void optimizer_program(struct drv_gpu_shader_ir *ir, enum drv_gpu_shader_stage stage);
static uint32_t optimizer_count_operation(const struct drv_gpu_shader_ir *ir, enum drv_gpu_shader_ir_op operation, uint32_t constant);
static void optimizer_options(enum bcm2711_shader_stage stage, struct drv_gpu_shader_optimize_options *options);
static void verify_optimizer_blocks(void);
static uint32_t optimizer_push_word(void);
static void verify_optimizer(void);

/*
 * Supplies the ordinary kernel allocation API through the host allocator.
 */
void *
kern_calloc(
	size_t count,
	size_t bytes)
{
	void *pointer;

	/* One selected allocator failure enters the same cleanup path as a real kernel heap refusal. */
	allocation_attempt++;
	if (allocation_attempt == allocation_refusal)
		return NULL;

	/* The production compiler's allocations stay visible to host memory instrumentation. */
	pointer = calloc(count, bytes);
	if (pointer != NULL)
		allocations_live++;

	/* Exact ordinary allocation ownership is returned to production source. */
	return pointer;
}

/*
 * Supplies the shared parser's ordinary host allocation operation.
 */
void *
kern_malloc(
	size_t bytes)
{
	void *pointer;

	/* A selected heap refusal never allocates hidden test storage. */
	allocation_attempt++;
	if (allocation_attempt == allocation_refusal)
		return NULL;

	/* Only the allocation backend is replaced; no parser or compiler path is gated for testing. */
	pointer = malloc(bytes);
	if (pointer != NULL)
		allocations_live++;

	/* Exact ordinary allocation ownership is returned to the shared parser. */
	return pointer;
}

/*
 * Supplies the shared parser's growable host allocation operation.
 */
void *
kern_realloc(
	void *pointer,
	size_t bytes)
{
	void *grown;

	/* The shared parser never requests the ambiguous free-on-zero realloc convention. */
	assert(bytes != 0);
	allocation_attempt++;
	if (allocation_attempt == allocation_refusal)
		return NULL;

	/* Existing ownership and failure behavior are preserved by the host allocator. */
	grown = realloc(pointer, bytes);
	if (pointer == NULL && grown != NULL)
		allocations_live++;

	/* Failure preserves the old allocation exactly as the ordinary realloc contract requires. */
	return grown;
}

/*
 * Releases ordinary host allocation storage used by actual production source.
 */
void
kern_free(
	void *pointer)
{
	/* A null cleanup owns no storage and may be repeated along a partial allocation unwind. */
	if (pointer != NULL) {
		assert(allocations_live != 0);
		allocations_live--;
	}

	/* This releases host memory, never files or GPU storage. */
	free(pointer);
}

/*
 * Checks all actual shaders of the tree and native pipeline variants.
 */
int
main(
	void)
{
	struct drv_gpu_shader_ir *vertex;
	struct drv_gpu_shader_ir *fragment;
	struct drv_gpu_shader_ir *generic;
	struct drv_gpu_compile_diagnostic diagnostic;
	struct bcm2711_shader_key key;
	struct v3d_device_info device;
	char name[32];
	uint32_t pair;
	uint32_t mode;
	int error;

	/* The unmodified fixed decoder interprets actual 4.2 machine words independently of production field construction. */
	memset(&device, 0, sizeof(device));
	device.ver = 42;
	device.has_accumulators = true;

	/* One actual quad profile exercises independent graphics admission and its metadata lifetime. */
	error = drv_gpu_shader_parse(kwl_quad_frag, sizeof(kwl_quad_frag) / 4, DRV_GPU_STAGE_FRAGMENT, &fragment, &diagnostic);
	assert(error == 0);
	pipeline_key(fragment, &key);
	verify_graphics_profile(&key);
	verify_allocation_refusal(&key, kwl_quad_frag, sizeof(kwl_quad_frag) / 4);
	drv_gpu_shader_ir_free(fragment);

	/* The glass, the longest scheduled Keiland program, refuses every allocation cleanly too. */
	error = drv_gpu_shader_parse(kwl_panel_variants[0].words, kwl_panel_variants[0].bytes / 4, DRV_GPU_STAGE_FRAGMENT, &fragment, &diagnostic);
	assert(error == 0);
	pipeline_key(fragment, &key);
	verify_allocation_refusal(&key, kwl_panel_variants[0].words, kwl_panel_variants[0].bytes / 4);
	drv_gpu_shader_ir_free(fragment);

	/* Every module pair of the tree, each native stage and both target channel orders. */
	printf("%-24s %-6s %7s %5s %5s %6s %8s\n", "program", "stage", "threads", "words", "idle", "paired", "uniforms");
	for (pair = 0; pair < sizeof(shader_pairs) / sizeof(shader_pairs[0]); pair++)
		verify_pair(&device, &shader_pairs[pair]);

	/* Every specialized shader must agree with the generic shader at its actual mode. */
	error = drv_gpu_shader_parse(kwl_panel_frag, sizeof(kwl_panel_frag) / 4, DRV_GPU_STAGE_FRAGMENT, &generic, &diagnostic);
	assert(error == 0);

	/* Native varying interfaces may shrink when a mode needs only texture or position (mode 7 is the dark glass, ws211-p011). */
	for (mode = 0; mode < 8; mode++) {
		/* Parses each immutable variant and the matching shared vertex shader. */
		error = drv_gpu_shader_parse(kwl_panel_vert, sizeof(kwl_panel_vert) / 4, DRV_GPU_STAGE_VERTEX, &vertex, &diagnostic);
		assert(error == 0);
		error = drv_gpu_shader_parse(kwl_panel_variants[mode].words, kwl_panel_variants[mode].bytes / 4, DRV_GPU_STAGE_FRAGMENT, &fragment, &diagnostic);
		assert(error == 0);
		pipeline_key(fragment, &key);

		/* Independent scalar evaluation checks both original colors and unused-sample removal. */
		verify_panel_mode(generic, fragment, mode);
		snprintf(name, sizeof(name), "kwl panel %u", mode);
		verify_program(&device, name, kwl_panel_vert, sizeof(kwl_panel_vert) / 4, vertex, BCM2711_SHADER_COORDINATE, &key, 1);
		verify_program(&device, name, kwl_panel_vert, sizeof(kwl_panel_vert) / 4, vertex, BCM2711_SHADER_VERTEX, &key, 1);
		verify_program(&device, name, kwl_panel_variants[mode].words, kwl_panel_variants[mode].bytes / 4, fragment, BCM2711_SHADER_FRAGMENT, &key, 1);
		key.swap_red_blue = 1;
		verify_program(&device, name, kwl_panel_variants[mode].words, kwl_panel_variants[mode].bytes / 4, fragment, BCM2711_SHADER_FRAGMENT, &key, 1);

		/* Compiled and parsed owners retire independently for each mode. */
		drv_gpu_shader_ir_free(vertex);
		drv_gpu_shader_ir_free(fragment);
	}

	/* The comparison baseline lives until every specialized mode has been checked. */
	drv_gpu_shader_ir_free(generic);

	/* The optimizer's rules, including those no shader of the tree meets, keep every output bit. */
	verify_optimizer();

	/* All compiler and parser objects must retire after the final completed program owner releases them. */
	assert(allocations_live == 0);

	/* This is evidence on the host model of one lane, not a physical QPU execution result. */
	puts("WS141 native shader host test: PASS (every shader of the tree, timing model of one lane, independent decoder and scalar source)");
	return 0;
}

/* Converts a host reference number into exact IEEE scalar bits. */
static uint32_t
float_bits(
	float number)
{
	union scalar_bits scalar;

	/* A union avoids strict-aliasing assumptions in both reference interpreters. */
	scalar.number = number;

	/* The returned bits are not computed by production kernel floating point. */
	return scalar.bits;
}

/* Converts scalar bits into a host reference number. */
static float
bits_float(
	uint32_t bits)
{
	union scalar_bits scalar;

	/* The source and native interpreters share representations, but independently implement operations. */
	scalar.bits = bits;

	/* Host arithmetic is a reference for scalar meaning rather than an SFU accuracy specification. */
	return scalar.number;
}

/* Constructs the frontend's all-ones Boolean representation. */
static uint32_t
truth(
	int condition)
{
	/* The source representation is a mask, not the C integer one. */
	if (condition != 0)
		return 0xffffffffU;

	/* False selects no lane. */
	return 0;
}

/* Rounds one float to the nearest half float, as the native pack does. */
static uint32_t
half_bits(
	float number)
{
	_Float16 half;
	uint16_t bits;

	/* The host's conversion rounds to nearest even. */
	half = (_Float16)number;
	memcpy(&bits, &half, sizeof(bits));

	/* Reports the sixteen-bit pattern. */
	return bits;
}

/* Advances the test's random generator. */
static uint32_t
next_random(void)
{
	/* A xorshift generator is deterministic and independent of the host library. */
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;

	/* Reports the next value. */
	return random_state;
}

/* Draws one float, mostly ordinary values with some edges, never a NaN. */
static uint32_t
random_float(void)
{
	static const float edges[8] = {0.0f, -0.0f, 1.0f, 0.5f, 2.0f, -1.0f, 0.25f, 1024.0f};
	uint32_t choice;
	float number;

	/* Picks the kind of value, then the value. */
	choice = next_random() % 10U;
	number = (float)(next_random() & 0xffffffU) / 16777216.0f;
	if (choice < 6U)
		number = number * 4.0f - 2.0f;
	else if (choice < 8U)
		number = number * 400.0f - 200.0f;
	else
		number = edges[next_random() % 8U];

	/* Reports the value's bits. */
	return float_bits(number);
}

/* Produces deterministic coordinate-dependent mock texture data, independently of both compiler paths. */
static uint32_t
sample_texel(
	uint32_t set,
	uint32_t binding,
	uint32_t s,
	uint32_t t,
	uint32_t component)
{
	float number;

	/* Nonconstant channels expose swapped coordinates, wrong textures and incorrect result ordering. */
	number = 0.6f + 0.01f * (float)(set * 8U + binding);
	if (component == 0)
		number = bits_float(s);
	if (component == 1)
		number = bits_float(t);
	if (component == 2)
		number = 0.25f + 0.1f * bits_float(s) + 0.03f * (float)binding;

	/* No actual texture descriptor, filtering, derivative or memory behavior is simulated here. */
	return float_bits(number);
}

/* Produces one uniform block word from its identity and the trial's seed. */
static uint32_t
block_word(
	uint32_t seed,
	uint32_t set,
	uint32_t binding,
	uint32_t offset)
{
	uint32_t mixed;

	/* A small hash spreads the identity over plausible floats. */
	mixed = seed * 2654435761U ^ set * 40503U ^ binding * 9973U ^ offset * 131U;
	mixed ^= mixed >> 15;
	mixed *= 2246822519U;
	mixed ^= mixed >> 13;

	/* Reports a float between minus two and two. */
	return float_bits((float)(mixed & 0xffffU) / 16384.0f - 2.0f);
}

/* Computes the native square root exactly as the compiler lowers it: x times its reciprocal root, keeping zeros and infinity. */
static uint32_t
native_square_root(
	uint32_t bits)
{
	float number;

	/* Both zeros and positive infinity keep their bits. */
	if ((bits & 0x7fffffffU) == 0 || bits == 0x7f800000U)
		return bits;

	/* Any other value is multiplied by its reciprocal root. */
	number = bits_float(bits);
	return float_bits(number * (1.0f / sqrtf(number)));
}

/* Converts a float to a signed integer, saturating and taking NaN to zero, as the model does. */
static int32_t
float_to_signed(
	float number)
{
	/* Out-of-range values saturate rather than reaching undefined host conversion; NaN compares unequal to itself. */
	if (number != number)
		return 0;
	if (number >= 2147483648.0f)
		return 2147483647;
	if (number < -2147483648.0f)
		return (int32_t)0x80000000U;

	/* An in-range value truncates toward zero. */
	return (int32_t)number;
}

/* Converts a float to an unsigned integer, saturating and taking NaN to zero, as the model does. */
static uint32_t
float_to_unsigned(
	float number)
{
	/* Out-of-range values saturate rather than reaching undefined host conversion; NaN compares unequal to itself. */
	if (number != number || number <= 0.0f)
		return 0;
	if (number >= 4294967296.0f)
		return 0xffffffffU;

	/* An in-range value truncates toward zero. */
	return (uint32_t)number;
}

/*
 * The minimum and maximum of two floats with -0 below +0, whichever order
 * they come in: SPIR-V's FMin and FMax may return either zero for two
 * zeros, and the native compiler may exchange the operands, so the reference
 * and the lane model both take the one order-independent answer.
 */
static float
ordered_minimum(
	float a,
	float b)
{
	/* Two zeros give -0 when either is -0. */
	if (a == 0.0f && b == 0.0f)
		return signbit(a) || signbit(b) ? -0.0f : 0.0f;
	return fminf(a, b);
}

static float
ordered_maximum(
	float a,
	float b)
{
	/* Two zeros give +0 when either is +0. */
	if (a == 0.0f && b == 0.0f)
		return signbit(a) && signbit(b) ? -0.0f : 0.0f;
	return fmaxf(a, b);
}

/* Independently interprets the shared parser's source scalar semantics before native register allocation. */
static void
source_program(
	const struct drv_gpu_shader_ir *ir,
	struct shader_inputs *inputs)
{
	const struct drv_gpu_shader_ir_inst *instruction;
	const struct drv_gpu_shader_ir_uniform *block;
	uint32_t *values;
	uint32_t index;
	uint32_t component;
	uint32_t left;
	uint32_t right;
	uint32_t bits;
	float a;
	float b;

	/* The source interpreter owns a distinct scalar array and never observes native register assignment. */
	values = calloc(ir->value_count, sizeof(*values));
	assert(values != NULL);

	/* Source operations define exact scalar values even when hardware code uses several native instructions. */
	for (index = 0; index < ir->instruction_count; index++) {
		instruction = &ir->instructions[index];
		left = values[instruction->src[0]];
		right = values[instruction->src[1]];
		a = bits_float(left);
		b = bits_float(right);
		bits = 0;

		/* Pure if-conversion markers preserve all arithmetic; final source selection decides visible output. */
		switch (instruction->op) {
		case DRV_GPU_IR_NOP:
		case DRV_GPU_IR_SKIP_BEGIN:
		case DRV_GPU_IR_SKIP_END:
			continue;
		case DRV_GPU_IR_CONST:
		case DRV_GPU_IR_BOOL:
		case DRV_GPU_IR_ICONST:
			bits = instruction->immediate;
			break;
		case DRV_GPU_IR_LOAD_INPUT:
			assert(instruction->location < 16 && instruction->component < 4);
			bits = inputs->input[instruction->location * 4 + instruction->component];
			break;
		case DRV_GPU_IR_LOAD_PUSH:
			assert(instruction->immediate / 4 < 64);
			bits = inputs->push[instruction->immediate / 4];
			break;
		case DRV_GPU_IR_LOAD_UBO:
			block = &ir->uniforms[instruction->location];
			bits = block_word(inputs->block_seed, block->set, block->binding, instruction->immediate);
			break;
		case DRV_GPU_IR_STORE_OUTPUT:
			component = instruction->location * 4 + instruction->component;
			if (instruction->location == DRV_GPU_IR_LOCATION_POSITION)
				component = 64 + instruction->component;
			assert(component < 68);
			inputs->output[component] = left;
			continue;
		case DRV_GPU_IR_SAMPLE:
			for (component = 0; component < 4; component++)
				values[instruction->dst + component] = sample_texel(instruction->location, instruction->immediate, left, right, component);
			inputs->lookups++;
			continue;
		case DRV_GPU_IR_FADD:
			bits = float_bits(a + b);
			break;
		case DRV_GPU_IR_FSUB:
			bits = float_bits(a - b);
			break;
		case DRV_GPU_IR_FMUL:
			bits = float_bits(a * b);
			break;
		case DRV_GPU_IR_FNEG:
			bits = left ^ 0x80000000U;
			break;
		case DRV_GPU_IR_FABS:
			bits = left & 0x7fffffffU;
			break;
		case DRV_GPU_IR_RCP:
			bits = float_bits(1.0f / a);
			break;
		case DRV_GPU_IR_RSQ:
			bits = float_bits(1.0f / sqrtf(a));
			break;
		case DRV_GPU_IR_SQRT:
			bits = native_square_root(left);
			break;
		case DRV_GPU_IR_EXP2:
			bits = float_bits(exp2f(a));
			break;
		case DRV_GPU_IR_LOG2:
			bits = float_bits(log2f(a));
			break;
		case DRV_GPU_IR_FLOOR:
			bits = float_bits(floorf(a));
			break;
		case DRV_GPU_IR_FRACT:
			bits = float_bits(a - floorf(a));
			break;
		case DRV_GPU_IR_FTRUNC:
			bits = float_bits(truncf(a));
			break;
		case DRV_GPU_IR_FROUND_EVEN:
			bits = float_bits(rintf(a));
			break;
		case DRV_GPU_IR_FMIN:
			bits = float_bits(ordered_minimum(a, b));
			break;
		case DRV_GPU_IR_FMAX:
			bits = float_bits(ordered_maximum(a, b));
			break;
		case DRV_GPU_IR_FLT:
			bits = truth(a < b);
			break;
		case DRV_GPU_IR_FGE:
			bits = truth(a >= b);
			break;
		case DRV_GPU_IR_FEQ:
			bits = truth(a == b);
			break;
		case DRV_GPU_IR_FNEU:
			bits = truth(!(a == b));
			break;
		case DRV_GPU_IR_AND:
		case DRV_GPU_IR_IAND:
			bits = left & right;
			break;
		case DRV_GPU_IR_OR:
		case DRV_GPU_IR_IOR:
			bits = left | right;
			break;
		case DRV_GPU_IR_IXOR:
			bits = left ^ right;
			break;
		case DRV_GPU_IR_NOT:
		case DRV_GPU_IR_INOT:
			bits = ~left;
			break;
		case DRV_GPU_IR_IADD:
			bits = left + right;
			break;
		case DRV_GPU_IR_ISUB:
			bits = left - right;
			break;
		case DRV_GPU_IR_INEG:
			bits = 0U - left;
			break;
		case DRV_GPU_IR_SHL:
			bits = left << (right & 31U);
			break;
		case DRV_GPU_IR_SHR:
			bits = left >> (right & 31U);
			break;
		case DRV_GPU_IR_ASR:
			bits = (uint32_t)((int32_t)left >> (right & 31U));
			break;
		case DRV_GPU_IR_I2F:
			bits = float_bits((float)(int32_t)left);
			break;
		case DRV_GPU_IR_U2F:
			bits = float_bits((float)left);
			break;
		case DRV_GPU_IR_F2I:
			bits = (uint32_t)float_to_signed(a);
			break;
		case DRV_GPU_IR_F2U:
			bits = float_to_unsigned(a);
			break;
		case DRV_GPU_IR_MOVE:
			bits = left;
			break;
		case DRV_GPU_IR_SELECT:
			bits = values[instruction->src[2]];
			if (left != 0)
				bits = right;
			break;
		case DRV_GPU_IR_KILL:
			/* A discard of a fragment with a true Boolean. */
			if (left != 0)
				inputs->discarded = 1;
			continue;
		default:
			fprintf(stderr, "source interpreter: unmodeled operation %u\n", (unsigned)instruction->op);
			assert(0);
		}

		/* Source destinations are independent of all native storage and temporary registers. */
		values[instruction->dst] = bits;
	}

	/* The reference owns only host scalar memory. */
	free(values);
}

/* Supplies the compiled uniform interface through ordinary checked test bindings. */
static uint32_t
machine_uniform(
	void *context,
	const struct bcm2711_shader_uniform *uniform)
{
	struct shader_inputs *inputs;

	/* Binding identity is interpreted independently of native opcode and register construction. */
	inputs = context;
	switch (uniform->kind) {
	case BCM2711_SHADER_CONSTANT:
		return uniform->bits;
	case BCM2711_SHADER_PUSH:
		assert(uniform->offset / 4 < 64);
		return inputs->push[uniform->offset / 4];
	case BCM2711_SHADER_BLOCK:
		return block_word(inputs->block_seed, uniform->set, uniform->binding, uniform->offset);
	case BCM2711_SHADER_TEXTURE:
		assert(uniform->bits == 15 && uniform->set < 256 && uniform->binding < 256);
		return 0x10000000U | (uniform->set << 8) | uniform->binding;
	case BCM2711_SHADER_SAMPLER:
		assert(uniform->bits == 1 && uniform->set < 256 && uniform->binding < 256);
		return 0x20000000U | (uniform->set << 8) | uniform->binding;
	case BCM2711_SHADER_VIEWPORT_X:
		return float_bits(640.0f * 256.0f);
	case BCM2711_SHADER_VIEWPORT_Y:
		return float_bits(400.0f * 256.0f);
	case BCM2711_SHADER_VIEWPORT_Z:
		return float_bits(1.0f);
	case BCM2711_SHADER_DEPTH_OFFSET:
		return 0;
	default:
		break;
	}

	/* Every descriptor kind the compiler emits is modeled. */
	assert(0);
	return 0;
}

/* Supplies one texture lookup through the same mock data as the source interpreter. */
static void
machine_sample(
	void *context,
	uint32_t set,
	uint32_t binding,
	uint32_t s,
	uint32_t t,
	uint32_t *texel)
{
	uint32_t component;

	/* The lookup's four components. */
	(void)context;
	for (component = 0; component < 4; component++)
		texel[component] = sample_texel(set, binding, s, t, component);
}

/* Derives the canonical fragment scalar interface independently of frontend declaration order. */
static void
pipeline_key(
	const struct drv_gpu_shader_ir *ir,
	struct bcm2711_shader_key *key)
{
	struct bcm2711_shader_component *varying;
	uint32_t location;
	uint32_t index;
	uint32_t component;

	/* Fragment declarations may be reversed in actual panel SPIR-V. */
	memset(key, 0, sizeof(*key));
	for (location = 0; location < 16; location++) {
		for (index = 0; index < ir->input_count; index++) {
			if (ir->inputs[index].location != location)
				continue;

			/* Each varying scalar preserves the actual interpolation qualifiers. */
			for (component = 0; component < ir->inputs[index].components; component++) {
				assert(key->varying_count < BCM2711_SHADER_INTERFACE_WORDS);
				varying = &key->varyings[key->varying_count++];
				varying->location = location;
				varying->component = component;
				varying->flat = ir->inputs[index].flat;
				varying->noperspective = ir->inputs[index].noperspective;
			}
		}
	}
}

/* Creates concrete finite Keiland draw inputs that exercise each panel shape branch and several fragment locations. */
static void
desktop_inputs(
	struct shader_inputs *inputs,
	uint32_t fragment,
	uint32_t trial)
{
	float number;

	/* The quad's push layout covers rectangle, texture, box, color, shape and screen uniforms. */
	memset(inputs, 0, sizeof(*inputs));
	inputs->push[0] = float_bits(-0.7f);
	inputs->push[1] = float_bits(-0.5f);
	inputs->push[2] = float_bits(0.4f);
	inputs->push[3] = float_bits(0.6f);
	inputs->push[4] = float_bits(0.1f);
	inputs->push[5] = float_bits(0.2f);
	inputs->push[6] = float_bits(0.8f);
	inputs->push[7] = float_bits(0.9f);
	inputs->push[8] = float_bits(100.0f);
	inputs->push[9] = float_bits(80.0f);
	inputs->push[10] = float_bits(200.0f);
	inputs->push[11] = float_bits(120.0f);
	inputs->push[12] = float_bits(0.4f);
	inputs->push[13] = float_bits(0.6f);
	inputs->push[14] = float_bits(0.8f);
	inputs->push[15] = float_bits(0.75f);
	inputs->push[16] = float_bits(12.0f);
	number = (float)(trial % 8) - 1.0f;
	inputs->push[17] = float_bits(number);
	inputs->push[18] = float_bits(2.0f);
	inputs->push[19] = float_bits(0.0f);
	inputs->push[20] = float_bits(1280.0f);
	inputs->push[21] = float_bits(800.0f);
	inputs->push[22] = float_bits(0.3f);
	inputs->push[23] = float_bits(0.85f);

	/* Texture and pixel varyings vary independently across native/source comparisons. */
	inputs->input[0] = float_bits(0.25f);
	inputs->input[1] = float_bits(0.75f);
	if (fragment != 0) {
		inputs->input[4] = float_bits(96.0f + 17.0f * (float)(trial / 8));
		inputs->input[5] = float_bits(84.0f + 11.0f * (float)(trial / 8));
	}

	/* Uniform block words vary with the trial. */
	inputs->block_seed = trial;
}

/* Fills every input, push word and block seed with random values. */
static void
random_inputs(
	struct shader_inputs *inputs,
	uint32_t seed)
{
	uint32_t index;

	/* Each trial has its own deterministic stream. */
	memset(inputs, 0, sizeof(*inputs));
	random_state = seed * 747796405U + 2891336453U;
	if (random_state == 0)
		random_state = 1;
	for (index = 0; index < 64; index++)
		inputs->input[index] = random_float();
	for (index = 0; index < 64; index++)
		inputs->push[index] = random_float();
	inputs->block_seed = next_random();
}

/* Compares scalar floating outputs with bounded tolerance for the vertex header's distinct evaluation paths. */
static void
compare_number(
	uint32_t native,
	uint32_t expected)
{
	float a;
	float b;
	float distance;
	float limit;

	/* Exact equality accepts integers and preserves special-value bits without floating conversion. */
	if (native == expected)
		return;

	/* NaNs of both sides agree; a NaN compares unequal to itself. */
	a = bits_float(native);
	b = bits_float(expected);
	if (a != a && b != b)
		return;
	distance = fabsf(a - b);
	limit = 0.00002f * (1.0f + fabsf(b));
	if (!(distance <= limit)) {
		fprintf(stderr, "native scalar %08x %.9g differs from source %08x %.9g\n", native, a, expected, b);
		assert(0);
	}
}

/* Compares each specialized body against the original shader at the same visible mode. */
static void
verify_panel_mode(
	const struct drv_gpu_shader_ir *generic,
	const struct drv_gpu_shader_ir *specialized,
	uint32_t mode)
{
	struct shader_inputs original;
	struct shader_inputs variant;
	uint32_t trial;
	uint32_t component;

	/* Glass includes both light and dark appearances; other modes have one exact selector. */
	for (trial = 0; trial < 32; trial++) {
		/*
		 * All source inputs agree, including nontrivial position, texture
		 * and opacity.  Light glass is mode 0, dark glass (mode -1) its own
		 * variant 7, white or not; both take the values glass.c works out
		 * once for the shape (ws211-p011).
		 */
		desktop_inputs(&original, 1, trial);
		original.push[17] = float_bits((float)mode);
		if (mode == 7)
			original.push[17] = float_bits(-1.0f);
		if (mode == 0 || mode == 7) {
			if ((trial & 2) != 0) {
				original.push[12] = float_bits(1.0f);
				original.push[13] = float_bits(0.95f);
				original.push[14] = float_bits(0.92f);
			}

			/* Flatness varies across trials, then the per-shape values follow. */
			original.push[18] = float_bits((float)(trial % 3) * 0.5f);
			glass_constants(original.push);
		}

		/* These evaluations share no native instruction or register representation. */
		variant = original;
		source_program(generic, &original);
		source_program(specialized, &variant);
		for (component = 0; component < 4; component++)
			compare_number(variant.output[component], original.output[component]);

		/* Texture-free shapes sample nothing, text/images/glass once, and blur five times. */
		if (mode == 1 || mode == 3 || mode == 4)
			assert(variant.lookups == 0);
		else if (mode == 6)
			assert(variant.lookups == 5);
		else
			assert(variant.lookups == 1);
	}

	/* Succeeded: all independent ownership or scalar comparisons passed. */
	return;
}

/* Fills a glass shape's per-shape values as glass.c's glass_shape_glass_constants does (ws211-p011). */
static void
glass_constants(
	uint32_t *push)
{
	float least;
	float second;
	float third;
	float appearance;
	float flat;
	float height;

	/* The same per-shape values as the compositor works out once for each shape. */
	least = bits_float(push[12]);
	second = bits_float(push[13]);
	third = bits_float(push[14]);
	if (second < least)
		least = second;
	if (third < least)
		least = third;
	push[4] = float_bits(0.0f);
	if (least >= 0.9f)
		push[4] = float_bits(1.0f);
	appearance = bits_float(push[17]);
	push[5] = float_bits(0.0f);
	if (appearance <= -0.5f)
		push[5] = float_bits(1.0f);
	flat = bits_float(push[18]);
	if (flat < 0.0f)
		flat = 0.0f;
	if (flat > 1.0f)
		flat = 1.0f;
	push[6] = float_bits(0.05f * (1.0f - flat));
	height = bits_float(push[11]);
	if (height < 1.0f)
		height = 1.0f;
	push[7] = float_bits(1.0f / height);
}

/* Compiles one module pair's stages and checks each that the compiler accepts. */
static void
verify_pair(
	const struct v3d_device_info *device,
	const struct shader_pair *pair)
{
	struct drv_gpu_shader_ir *vertex;
	struct drv_gpu_shader_ir *fragment;
	struct drv_gpu_compile_diagnostic diagnostic;
	struct bcm2711_shader_binary *binary;
	struct bcm2711_shader_diagnostic refusal;
	struct bcm2711_shader_key key;
	const char *reason;
	uint32_t keiland;
	int error;

	/* The fragment module's inputs decide the pipeline key of both stages. */
	keiland = strncmp(pair->name, "kwl", 3) == 0;
	error = drv_gpu_shader_parse(pair->fragment, pair->fragment_bytes / 4, DRV_GPU_STAGE_FRAGMENT, &fragment, &diagnostic);
	assert(error == 0);
	pipeline_key(fragment, &key);

	/* A module the native compiler refuses is expected to be refused, and named. */
	if (pair->fragment_refused != 0) {
		error = bcm2711_shader_compile(pair->fragment, pair->fragment_bytes / 4, BCM2711_SHADER_FRAGMENT, &key, &binary, &refusal);
		assert(error == ENOTSUP && binary == NULL);
		reason = "?";
		if (refusal.reason != NULL)
			reason = refusal.reason;
		printf("%-24s %-6s refused: %s\n", pair->name, "frag", reason);
	} else {
		verify_program(device, pair->name, pair->fragment, pair->fragment_bytes / 4, fragment, BCM2711_SHADER_FRAGMENT, &key, keiland);
		key.swap_red_blue = 1;
		verify_program(device, pair->name, pair->fragment, pair->fragment_bytes / 4, fragment, BCM2711_SHADER_FRAGMENT, &key, keiland);
		key.swap_red_blue = 0;
	}

	/* Both vertex variants of the paired vertex module. */
	error = drv_gpu_shader_parse(pair->vertex, pair->vertex_bytes / 4, DRV_GPU_STAGE_VERTEX, &vertex, &diagnostic);
	assert(error == 0);
	if (pair->vertex_refused != 0) {
		error = bcm2711_shader_compile(pair->vertex, pair->vertex_bytes / 4, BCM2711_SHADER_VERTEX, &key, &binary, &refusal);
		assert(error == ENOTSUP && binary == NULL);
		reason = "?";
		if (refusal.reason != NULL)
			reason = refusal.reason;
		printf("%-24s %-6s refused: %s\n", pair->name, "vert", reason);
	} else {
		verify_program(device, pair->name, pair->vertex, pair->vertex_bytes / 4, vertex, BCM2711_SHADER_COORDINATE, &key, keiland);
		verify_program(device, pair->name, pair->vertex, pair->vertex_bytes / 4, vertex, BCM2711_SHADER_VERTEX, &key, keiland);
	}

	/* Parser ownership retires after the module's variants are checked. */
	drv_gpu_shader_ir_free(vertex);
	drv_gpu_shader_ir_free(fragment);
}

/* Compiles one stage, checks its interface and runs deterministic and random trials against the source. */
static void
verify_program(
	const struct v3d_device_info *device,
	const char *name,
	const uint32_t *words,
	size_t word_count,
	const struct drv_gpu_shader_ir *ir,
	enum bcm2711_shader_stage stage,
	struct bcm2711_shader_key *key,
	uint32_t keiland)
{
	struct bcm2711_shader_binary *binary;
	struct bcm2711_shader_diagnostic diagnostic;
	struct shader_inputs inputs;
	uint32_t discarding;
	uint32_t trial;
	int error;

	/* Actual production compiler source supplies each independent native variant. */
	error = bcm2711_shader_compile(words, word_count, stage, key, &binary, &diagnostic);
	if (error != 0) {
		fprintf(stderr, "%s: compile stage %u error %d at %u: %s\n", name, stage, error, diagnostic.instruction, diagnostic.reason);
		assert(0);
	}

	/* Thread windows: four threads see rf0 to rf31, two all sixty-four; fragment programs are scheduled. */
	if (binary->threads == 4) {
		assert(stage == BCM2711_SHADER_FRAGMENT && binary->registers_used == 32);
	} else {
		assert(binary->threads == 2 && binary->registers_used == 64);
	}

	/*
	 * Every program of the tree is lowered from the optimizer's rewritten
	 * source, never from the fallback, except a discarding one: the shared
	 * optimizer leaves a program with a discard unchanged.
	 */
	discarding = 0;
	for (trial = 0; trial < ir->instruction_count; trial++) {
		if (ir->instructions[trial].op == DRV_GPU_IR_KILL)
			discarding = 1;
	}
	assert(binary->optimized == 1 || discarding != 0);

	/* Fragment programs are scheduled; vertex programs keep their source order. */
	if (stage == BCM2711_SHADER_FRAGMENT)
		assert(binary->scheduled == 1);
	else
		assert(binary->scheduled == 0);
	verify_window(device, binary);
	if (key->swap_red_blue == 0)
		report_size(device, name, binary);
	current_program = name;

	/* The Keiland modules' realistic inputs, then random ones. */
	if (keiland != 0) {
		for (trial = 0; trial < 32; trial++) {
			desktop_inputs(&inputs, stage == BCM2711_SHADER_FRAGMENT, trial);
			current_trial = trial;
			run_trial(device, binary, ir, key, &inputs, 1);
		}
	}

	/* Random inputs reach every source path. */
	discards = 0;
	for (trial = 0; trial < RANDOM_TRIALS; trial++) {
		random_inputs(&inputs, trial * 7919U + (uint32_t)word_count * 31U + stage);
		current_trial = 1000 + trial;
		run_trial(device, binary, ir, key, &inputs, 0);
	}

	/* A discarding program must meet both kept and discarded fragments. */
	if (discards != 0 && key->swap_red_blue == 0)
		printf("%-24s %-6s discarded %u of %u random trials\n", name, "frag", discards, RANDOM_TRIALS);
	assert(discards < RANDOM_TRIALS);

	/* The caller exercises ordinary program ownership after every completed native variant. */
	bcm2711_shader_binary_free(binary);
}

/* Runs one trial on the source interpreter and the lane model and compares every output. */
static void
run_trial(
	const struct v3d_device_info *device,
	const struct bcm2711_shader_binary *binary,
	const struct drv_gpu_shader_ir *ir,
	const struct bcm2711_shader_key *key,
	struct shader_inputs *inputs,
	uint32_t exact_viewport)
{
	struct qpu_machine_setup setup;
	struct qpu_machine_result result;
	struct shader_inputs source;
	uint32_t varyings[BCM2711_SHADER_INTERFACE_WORDS];
	uint32_t attributes[BCM2711_SHADER_INTERFACE_WORDS];
	uint32_t index;
	uint32_t component;
	uint32_t native;
	uint32_t expected;
	float number;

	/* The source interpreter works on its own copy of the inputs. */
	source = *inputs;
	source_program(ir, &source);

	/* The native FIFO carries the key's varyings, the VPM the program's attributes, in canonical order. */
	for (index = 0; index < binary->input_count; index++) {
		component = binary->inputs[index].location * 4 + binary->inputs[index].component;
		varyings[index] = inputs->input[component];
		attributes[index] = inputs->input[component];
	}

	/* The lane runs the program with the same inputs. */
	memset(&setup, 0, sizeof(setup));
	setup.binary = binary;
	setup.varyings = varyings;
	setup.key = key;
	setup.attributes = attributes;
	setup.payload[0] = float_bits(2.0f);
	setup.payload[1] = float_bits(2.0f);
	setup.payload[2] = float_bits(0.5f);
	setup.garbage_seed = inputs->block_seed ^ binary->code_count;
	setup.context = inputs;
	setup.uniform = machine_uniform;
	setup.sample = machine_sample;
	qpu_machine_run(device, &setup, &result);
	/* The compiler may share a repeated lookup and drop one whose texel nothing reads, never add one. */
	if (result.lookups != source.lookups && current_trial == 0)
		printf("%-24s stage %u: %u of the source's %u lookups\n", current_program, binary->stage, result.lookups, source.lookups);
	assert(result.lookups <= source.lookups);

	/* Fragment colour: each physical channel carries its source colour, rounded to a half float, exactly. */
	if (binary->stage == BCM2711_SHADER_FRAGMENT) {
		assert(result.tile_writes == 2);

		/* A discarded fragment's tile writes are masked, so only the discard itself is compared. */
		if (result.discarded != source.discarded) {
			fprintf(stderr, "%s stage %u trial %u: native discard %u differs from source discard %u\n", current_program, binary->stage, current_trial, result.discarded, source.discarded);
			assert(0);
		}
		if (source.discarded != 0) {
			discards++;
			return;
		}
		for (index = 0; index < 4; index++) {
			component = index;
			if (key->swap_red_blue != 0 && (index == 0 || index == 2))
				component = 2 - index;
			native = (result.tile[index / 2] >> (16 * (index % 2))) & 0xffffU;
			expected = half_bits(bits_float(source.output[component]));
			if (native != expected) {
				fprintf(stderr, "%s stage %u trial %u: native half %04x differs from source %04x (%.9g) in channel %u\n", current_program, binary->stage, current_trial, native, expected, bits_float(source.output[component]), index);
				assert(0);
			}
		}

		/* Fragment variants have no vertex VPM header to compare. */
		return;
	}

	/* Coordinate/bin exports all clip scalars, while render exports depth/W followed by canonical varyings. */
	component = 0;
	if (binary->stage == BCM2711_SHADER_COORDINATE) {
		for (index = 0; index < 4; index++)
			compare_number(result.vpm[index], source.output[64 + index]);
		component = 4;
	} else {
		for (index = 0; index < key->varying_count; index++) {
			expected = source.output[key->varyings[index].location * 4 + key->varyings[index].component];
			compare_number(result.vpm[4 + index], expected);
		}
	}

	/* Real Keiland vertices have clip W one; native XY still must follow 4.2 floor-to-fixed conversion. */
	if (exact_viewport != 0) {
		if (binary->stage == BCM2711_SHADER_VERTEX) {
			compare_number(result.vpm[2], source.output[66]);
			compare_number(result.vpm[3], float_bits(1.0f));
		}

		/* The viewport XY follow the floor-to-fixed conversion. */
		number = floorf(bits_float(source.output[64]) * 640.0f * 256.0f);
		assert(result.vpm[component] == (uint32_t)(int32_t)number);
		number = floorf(bits_float(source.output[65]) * 400.0f * 256.0f);
		assert(result.vpm[component + 1] == (uint32_t)(int32_t)number);
	}
}

/* Checks a four-thread program reads and writes only rf0 to rf31. */
static void
verify_window(
	const struct v3d_device_info *device,
	const struct bcm2711_shader_binary *binary)
{
	struct v3d_qpu_instr instruction;
	uint32_t index;
	bool accepted;
	bool port_a;
	bool port_b;
	bool signal_register;

	/* A two-thread program sees all sixty-four registers. */
	if (binary->threads != 4)
		return;

	/* Every register field of every word stays inside the four-thread window. */
	for (index = 0; index < binary->code_count; index++) {
		accepted = v3d_qpu_instr_unpack(device, binary->code[index], &instruction);
		assert(accepted);
		port_a = v3d_qpu_uses_mux(&instruction, V3D_QPU_MUX_A);
		port_b = v3d_qpu_uses_mux(&instruction, V3D_QPU_MUX_B);
		signal_register = v3d_qpu_sig_writes_address(device, &instruction.sig);
		if (port_a)
			assert(instruction.raddr_a < 32);
		if (port_b && !instruction.sig.small_imm_b)
			assert(instruction.raddr_b < 32);
		if (instruction.alu.add.op != V3D_QPU_A_NOP && !instruction.alu.add.magic_write)
			assert(instruction.alu.add.waddr < 32);
		if (instruction.alu.mul.op != V3D_QPU_M_NOP && !instruction.alu.mul.magic_write)
			assert(instruction.alu.mul.waddr < 32);
		if (signal_register && !instruction.sig_magic)
			assert(instruction.sig_addr < 32);
	}
}

/* Prints one native program's size: words, words with nothing to do, words pairing both ALUs and uniforms. */
static void
report_size(
	const struct v3d_device_info *device,
	const char *name,
	const struct bcm2711_shader_binary *binary)
{
	static const char *stages[3] = {"coord", "vertex", "frag"};
	struct v3d_qpu_instr instruction;
	uint32_t index;
	uint32_t idle;
	uint32_t paired;
	bool accepted;
	bool add;
	bool mul;
	bool signal;

	/* Counts the words that issue nothing and those that issue both ALUs. */
	idle = 0;
	paired = 0;
	for (index = 0; index < binary->code_count; index++) {
		accepted = v3d_qpu_instr_unpack(device, binary->code[index], &instruction);
		assert(accepted);
		add = instruction.alu.add.op != V3D_QPU_A_NOP;
		mul = instruction.alu.mul.op != V3D_QPU_M_NOP;
		signal = instruction.sig.thrsw || instruction.sig.ldunif || instruction.sig.ldunifrf ||
			instruction.sig.ldtmu || instruction.sig.ldvary || instruction.sig.wrtmuc ||
			instruction.sig.ldtlb || instruction.sig.ldtlbu || instruction.sig.ldvpm;
		if (!add && !mul && !signal)
			idle++;
		if (add && mul)
			paired++;
	}

	/* One table row per program. */
	printf("%-24s %-6s %7u %5u %5u %6u %8u\n", name, stages[binary->stage], binary->threads, binary->code_count, idle, paired, binary->uniform_count);
}

/* Checks deep declaration graphs and stage changes through the public compiler, retaining the actual Keiland source body. */
static void
verify_graphics_profile(
	const struct bcm2711_shader_key *key)
{
	struct bcm2711_shader_binary *binary;
	struct bcm2711_shader_diagnostic diagnostic;
	uint32_t *words;
	size_t source_count;
	size_t offset;
	size_t prefix;
	size_t entry;
	size_t variable;
	size_t position;
	size_t baseline;
	uint32_t opcode;
	uint32_t count;
	uint32_t scalar_type;
	uint32_t scalar;
	uint32_t previous_type;
	uint32_t previous_constant;
	uint32_t type;
	uint32_t identity;
	uint32_t index;
	int error;

	/* Locate actual declarations without relying on generated shader offsets or IDs. */
	source_count = sizeof(kwl_quad_vert) / 4;
	prefix = 0;
	entry = 0;
	variable = 0;
	scalar_type = 0;
	scalar = 0;
	offset = 5;
	while (offset < source_count) {
		count = kwl_quad_vert[offset] >> 16;
		opcode = kwl_quad_vert[offset] & 0xffffU;
		assert(count != 0 && count <= source_count - offset);

		/* A real float constant supplies the scalar leaf of the additional aggregate graph. */
		if (opcode == 22U)
			scalar_type = kwl_quad_vert[offset + 1];
		if (opcode == 43U && kwl_quad_vert[offset + 1] == scalar_type)
			scalar = kwl_quad_vert[offset + 2];
		if (opcode == 15U)
			entry = offset;
		if (opcode == 59U)
			variable = offset;
		if (opcode == 54U) {
			prefix = offset;
			break;
		}

		/* Advance only over complete immutable source instructions while locating the module/function boundary. */
		offset += count;
	}

	/* The fixture adds ordinary nested structure constants, leaving the source's actual executable shader unchanged. */
	assert(prefix != 0 && entry != 0 && variable != 0);
	assert(scalar_type != 0 && scalar != 0);
	words = malloc((source_count + 2048U * 7U) * sizeof(*words));
	assert(words != NULL);
	memcpy(words, kwl_quad_vert, prefix * sizeof(*words));
	previous_type = scalar_type;
	previous_constant = scalar;
	for (index = 0; index < 2048U; index++) {
		/* Each new structure has one member of the preceding type and one constant of that exact type. */
		position = prefix + index * 7U;
		type = kwl_quad_vert[3] + index * 2U;
		identity = type + 1U;
		words[position] = (3U << 16) | 30U;
		words[position + 1] = type;
		words[position + 2] = previous_type;
		words[position + 3] = (4U << 16) | 44U;
		words[position + 4] = type;
		words[position + 5] = identity;
		words[position + 6] = previous_constant;
		previous_type = type;
		previous_constant = identity;
	}

	/* Deep aggregate expansion is refused before the shared parser allocates an IR or recurses through any constant. */
	memcpy(words + prefix + 2048U * 7U, kwl_quad_vert + prefix, (source_count - prefix) * sizeof(*words));
	words[3] = kwl_quad_vert[3] + 4096U;
	baseline = allocations_live;
	binary = (void *)1;
	error = bcm2711_shader_compile(words, source_count + 2048U * 7U, BCM2711_SHADER_VERTEX, key, &binary, &diagnostic);
	assert(error == ENOTSUP && binary == NULL && allocations_live == baseline);
	assert(diagnostic.instruction == prefix + 8U * 7U + 3U);

	/* A same-ID redefinition that could mutate an earlier constant edge is refused independently of nesting depth. */
	words[prefix + 5] = scalar;
	error = bcm2711_shader_compile(words, source_count + 2048U * 7U, BCM2711_SHADER_VERTEX, key, &binary, &diagnostic);
	assert(error == EINVAL && binary == NULL && allocations_live == baseline);
	assert(diagnostic.instruction == prefix + 3U);

	/* Eight ordinary aggregate edges followed by the scalar leaf remain admitted and compile the unchanged actual shader. */
	words[prefix + 5] = kwl_quad_vert[3] + 1U;
	words[3] = kwl_quad_vert[3] + 16U;
	memcpy(words + prefix + 8U * 7U, kwl_quad_vert + prefix, (source_count - prefix) * sizeof(*words));
	error = bcm2711_shader_compile(words, source_count + 8U * 7U, BCM2711_SHADER_VERTEX, key, &binary, &diagnostic);
	assert(error == 0 && binary != NULL);
	bcm2711_shader_binary_free(binary);
	assert(allocations_live == baseline);

	/* A compute entry cannot overwrite the requested graphics stage before a Workgroup variable is decoded. */
	memcpy(words, kwl_quad_vert, sizeof(kwl_quad_vert));
	words[entry + 1] = 5U;
	error = bcm2711_shader_compile(words, source_count, BCM2711_SHADER_VERTEX, key, &binary, &diagnostic);
	assert(error == ENOTSUP && binary == NULL && allocations_live == baseline);
	assert(diagnostic.instruction == entry);

	/* Graphics entry points cannot route a malformed Workgroup variable into compute-only recursive layout. */
	memcpy(words, kwl_quad_vert, sizeof(kwl_quad_vert));
	words[variable + 3] = 4U;
	error = bcm2711_shader_compile(words, source_count, BCM2711_SHADER_VERTEX, key, &binary, &diagnostic);
	assert(error == ENOTSUP && binary == NULL && allocations_live == baseline);
	assert(diagnostic.instruction == variable);

	/* Preflight metadata OOM owns no IR, native program, or diagnostic pointer into freed storage. */
	allocation_attempt = 0;
	allocation_refusal = 1;
	error = bcm2711_shader_compile(kwl_quad_vert, source_count, BCM2711_SHADER_VERTEX, key, &binary, &diagnostic);
	assert(error == ENOMEM && binary == NULL && allocations_live == baseline);
	allocation_refusal = 0;
	free(words);

	/* These are actual compiler refusal and lifetime results; no physical GPU scheduling is modeled. */
	puts("WS141 native graphics stage/deep constant graph/redefinition/preflight OOM: PASS");
}

/* Verifies each native program allocation refusal leaves no leaked frontend or partial native ownership. */
static void
verify_allocation_refusal(
	const struct bcm2711_shader_key *key,
	const uint32_t *words,
	size_t word_count)
{
	struct bcm2711_shader_binary *binary;
	struct bcm2711_shader_diagnostic diagnostic;
	size_t baseline;
	size_t attempts;
	size_t refusal;
	int error;

	/* A successful real compile locates the final native-owner, liveness, code and uniform allocations. */
	baseline = allocations_live;
	allocation_attempt = 0;
	allocation_refusal = 0;
	error = bcm2711_shader_compile(words, word_count, BCM2711_SHADER_FRAGMENT, key, &binary, &diagnostic);
	assert(error == 0);
	attempts = allocation_attempt;
	bcm2711_shader_binary_free(binary);
	assert(allocations_live == baseline && attempts >= 5);

	/* Every frontend and native allocation fails independently without retaining partial ownership. */
	for (refusal = 1; refusal <= attempts; refusal++) {
		allocation_attempt = 0;
		allocation_refusal = refusal;
		binary = (void *)1;
		error = bcm2711_shader_compile(words, word_count, BCM2711_SHADER_FRAGMENT, key, &binary, &diagnostic);
		assert(error != 0 && binary == NULL);
		assert(allocations_live == baseline);
	}

	/* All following host comparisons use the ordinary allocation path. */
	allocation_refusal = 0;
}

/* Appends one operation reading up to three values to the optimizer test program and reports its result. */
static uint32_t
optimizer_emit(
	enum drv_gpu_shader_ir_op operation,
	uint32_t first,
	uint32_t second,
	uint32_t third)
{
	struct drv_gpu_shader_ir_inst *instruction;
	uint32_t results;

	/* A sample defines four values, every other operation one. */
	assert(optimizer_count < sizeof(optimizer_code) / sizeof(optimizer_code[0]));
	results = 1;
	if (operation == DRV_GPU_IR_SAMPLE)
		results = 4;

	/* Fills the operation with fresh results. */
	instruction = &optimizer_code[optimizer_count];
	memset(instruction, 0, sizeof(*instruction));
	instruction->op = operation;
	instruction->dst = optimizer_values;
	instruction->src[0] = first;
	instruction->src[1] = second;
	instruction->src[2] = third;
	optimizer_count++;
	optimizer_values += results;

	/* Reports the first result. */
	return instruction->dst;
}

/* Appends one constant to the optimizer test program. */
static uint32_t
optimizer_constant(
	enum drv_gpu_shader_ir_op operation,
	uint32_t bits)
{
	uint32_t value;

	/* The constant's bits are its immediate. */
	value = optimizer_emit(operation, 0, 0, 0);
	optimizer_code[optimizer_count - 1].immediate = bits;

	/* Reports the constant's value. */
	return value;
}

/* Appends one push-constant load to the optimizer test program. */
static uint32_t
optimizer_load(
	uint32_t offset)
{
	uint32_t value;

	/* The word's byte offset is the immediate. */
	value = optimizer_emit(DRV_GPU_IR_LOAD_PUSH, 0, 0, 0);
	optimizer_code[optimizer_count - 1].immediate = offset;

	/* Reports the loaded value. */
	return value;
}

/* Appends one output store to the optimizer test program. */
static void
optimizer_store(
	uint32_t location,
	uint32_t component,
	uint32_t value)
{
	/* A store reads its value and defines nothing. */
	optimizer_emit(DRV_GPU_IR_STORE_OUTPUT, value, 0, 0);
	optimizer_values--;
	optimizer_code[optimizer_count - 1].dst = 0;
	optimizer_code[optimizer_count - 1].location = location;
	optimizer_code[optimizer_count - 1].component = component;
}

/*
 * Builds the optimizer test program: each output holds one rule's case.
 *
 * The cases a rule must rewrite stand next to the near misses it must leave
 * alone, so a rule applied too widely changes an output or a count.
 */
static void
optimizer_program(
	struct drv_gpu_shader_ir *ir,
	enum drv_gpu_shader_stage stage)
{
	uint32_t x;
	uint32_t y;
	uint32_t z;
	uint32_t w;
	uint32_t condition;
	uint32_t integer;
	uint32_t shift;
	uint32_t one;
	uint32_t half;
	uint32_t positive_zero;
	uint32_t negative_zero;
	uint32_t nan;
	uint32_t ones;
	uint32_t yes;
	uint32_t no;
	uint32_t three;
	uint32_t minus_seven;
	uint32_t thirty_two;
	uint32_t inexact;
	uint32_t smallest;
	uint32_t texel;
	uint32_t repeated;
	uint32_t value;

	/* Starts an empty program. */
	optimizer_count = 0;
	optimizer_values = 0;

	/* Loads the varying operands and makes the constants the rules name. */
	x = optimizer_load(0);
	y = optimizer_load(4);
	z = optimizer_load(8);
	w = optimizer_load(12);
	condition = optimizer_load(16);
	integer = optimizer_load(20);
	shift = optimizer_load(24);
	one = optimizer_constant(DRV_GPU_IR_CONST, 0x3f800000U);
	half = optimizer_constant(DRV_GPU_IR_CONST, 0x3f000000U);
	positive_zero = optimizer_constant(DRV_GPU_IR_CONST, 0);
	negative_zero = optimizer_constant(DRV_GPU_IR_CONST, 0x80000000U);
	nan = optimizer_constant(DRV_GPU_IR_CONST, 0x7fc00000U);
	ones = optimizer_constant(DRV_GPU_IR_ICONST, 0xffffffffU);
	yes = optimizer_constant(DRV_GPU_IR_BOOL, 0xffffffffU);
	no = optimizer_constant(DRV_GPU_IR_BOOL, 0);
	three = optimizer_constant(DRV_GPU_IR_ICONST, 3);
	minus_seven = optimizer_constant(DRV_GPU_IR_ICONST, 0xfffffff9U);
	thirty_two = optimizer_constant(DRV_GPU_IR_ICONST, 32);
	inexact = optimizer_constant(DRV_GPU_IR_ICONST, 16777219);
	smallest = optimizer_constant(DRV_GPU_IR_ICONST, 0x80000000U);

	/* Floating point identities read by arithmetic go; read by an output, or with the wrong zero, stay. */
	value = optimizer_emit(DRV_GPU_IR_FMUL, x, one, 0);
	optimizer_store(0, 0, optimizer_emit(DRV_GPU_IR_FADD, value, y, 0));
	optimizer_store(0, 1, optimizer_emit(DRV_GPU_IR_FMUL, one, z, 0));
	value = optimizer_emit(DRV_GPU_IR_FADD, y, negative_zero, 0);
	optimizer_store(0, 2, optimizer_emit(DRV_GPU_IR_FADD, value, w, 0));
	value = optimizer_emit(DRV_GPU_IR_FADD, positive_zero, y, 0);
	optimizer_store(0, 3, optimizer_emit(DRV_GPU_IR_FADD, value, w, 0));
	value = optimizer_emit(DRV_GPU_IR_FSUB, w, positive_zero, 0);
	optimizer_store(1, 0, optimizer_emit(DRV_GPU_IR_FSUB, value, x, 0));
	value = optimizer_emit(DRV_GPU_IR_FSUB, w, negative_zero, 0);
	optimizer_store(1, 1, optimizer_emit(DRV_GPU_IR_FADD, value, x, 0));

	/* Only commutative operations share results with their operands exchanged; a product's NaN operands keep their order. */
	optimizer_store(1, 2, optimizer_emit(DRV_GPU_IR_FSUB, x, y, 0));
	optimizer_store(1, 3, optimizer_emit(DRV_GPU_IR_FSUB, y, x, 0));
	optimizer_store(2, 0, optimizer_emit(DRV_GPU_IR_FMIN, x, y, 0));
	optimizer_store(2, 1, optimizer_emit(DRV_GPU_IR_FMIN, y, x, 0));
	optimizer_store(2, 2, optimizer_emit(DRV_GPU_IR_FMUL, x, y, 0));
	optimizer_store(2, 3, optimizer_emit(DRV_GPU_IR_FMUL, y, x, 0));

	/* Sign-bit operations. */
	value = optimizer_emit(DRV_GPU_IR_FNEG, x, 0, 0);
	optimizer_store(3, 0, optimizer_emit(DRV_GPU_IR_FABS, value, 0, 0));
	value = optimizer_emit(DRV_GPU_IR_FNEG, y, 0, 0);
	optimizer_store(3, 1, optimizer_emit(DRV_GPU_IR_FNEG, value, 0, 0));
	value = optimizer_emit(DRV_GPU_IR_FABS, z, 0, 0);
	optimizer_store(3, 2, optimizer_emit(DRV_GPU_IR_FABS, value, 0, 0));
	optimizer_store(3, 3, optimizer_emit(DRV_GPU_IR_FNEG, nan, 0, 0));

	/* Selections. */
	optimizer_store(4, 0, optimizer_emit(DRV_GPU_IR_SELECT, yes, x, y));
	optimizer_store(4, 1, optimizer_emit(DRV_GPU_IR_SELECT, no, x, y));
	optimizer_store(4, 2, optimizer_emit(DRV_GPU_IR_SELECT, condition, w, w));
	optimizer_store(4, 3, optimizer_emit(DRV_GPU_IR_SELECT, condition, x, y));

	/* Bitwise and Boolean logic. */
	value = optimizer_emit(DRV_GPU_IR_INOT, integer, 0, 0);
	optimizer_store(5, 0, optimizer_emit(DRV_GPU_IR_INOT, value, 0, 0));
	optimizer_store(5, 1, optimizer_emit(DRV_GPU_IR_IAND, integer, ones, 0));
	optimizer_store(5, 2, optimizer_emit(DRV_GPU_IR_AND, no, condition, 0));
	optimizer_store(5, 3, optimizer_emit(DRV_GPU_IR_IOR, integer, positive_zero, 0));
	optimizer_store(6, 0, optimizer_emit(DRV_GPU_IR_OR, condition, yes, 0));
	optimizer_store(6, 1, optimizer_emit(DRV_GPU_IR_IXOR, positive_zero, integer, 0));
	optimizer_store(6, 2, optimizer_emit(DRV_GPU_IR_IXOR, integer, integer, 0));
	optimizer_store(6, 3, optimizer_emit(DRV_GPU_IR_IAND, condition, condition, 0));
	optimizer_store(7, 0, optimizer_emit(DRV_GPU_IR_NOT, yes, 0, 0));
	optimizer_store(7, 1, optimizer_emit(DRV_GPU_IR_IXOR, three, minus_seven, 0));

	/* Integer arithmetic and shifts. */
	optimizer_store(7, 2, optimizer_emit(DRV_GPU_IR_IADD, integer, positive_zero, 0));
	optimizer_store(7, 3, optimizer_emit(DRV_GPU_IR_ISUB, integer, positive_zero, 0));
	optimizer_store(8, 0, optimizer_emit(DRV_GPU_IR_ISUB, integer, integer, 0));
	optimizer_store(8, 1, optimizer_emit(DRV_GPU_IR_IADD, three, minus_seven, 0));
	optimizer_store(8, 2, optimizer_emit(DRV_GPU_IR_ISUB, three, minus_seven, 0));
	optimizer_store(8, 3, optimizer_emit(DRV_GPU_IR_INEG, minus_seven, 0, 0));
	optimizer_store(9, 0, optimizer_emit(DRV_GPU_IR_SHL, integer, thirty_two, 0));
	optimizer_store(9, 1, optimizer_emit(DRV_GPU_IR_SHR, minus_seven, three, 0));
	optimizer_store(9, 2, optimizer_emit(DRV_GPU_IR_ASR, minus_seven, three, 0));
	optimizer_store(9, 3, optimizer_emit(DRV_GPU_IR_SHL, minus_seven, three, 0));
	optimizer_store(10, 0, optimizer_emit(DRV_GPU_IR_ASR, integer, shift, 0));

	/* Integer conversions fold only when exact. */
	optimizer_store(10, 1, optimizer_emit(DRV_GPU_IR_I2F, inexact, 0, 0));
	optimizer_store(10, 2, optimizer_emit(DRV_GPU_IR_I2F, minus_seven, 0, 0));
	optimizer_store(10, 3, optimizer_emit(DRV_GPU_IR_U2F, three, 0, 0));
	optimizer_store(11, 0, optimizer_emit(DRV_GPU_IR_I2F, smallest, 0, 0));
	optimizer_store(11, 1, optimizer_emit(DRV_GPU_IR_U2F, smallest, 0, 0));

	/* Minimum, maximum and comparisons of constants; a NaN and two zeros are not folded. */
	optimizer_store(11, 2, optimizer_emit(DRV_GPU_IR_FMIN, one, half, 0));
	optimizer_store(11, 3, optimizer_emit(DRV_GPU_IR_FMAX, negative_zero, positive_zero, 0));
	optimizer_store(12, 0, optimizer_emit(DRV_GPU_IR_FLT, half, one, 0));
	optimizer_store(12, 1, optimizer_emit(DRV_GPU_IR_FGE, negative_zero, one, 0));
	optimizer_store(12, 2, optimizer_emit(DRV_GPU_IR_FEQ, positive_zero, negative_zero, 0));
	optimizer_store(12, 3, optimizer_emit(DRV_GPU_IR_FLT, nan, one, 0));
	optimizer_store(14, 0, optimizer_emit(DRV_GPU_IR_FGE, one, one, 0));
	optimizer_store(14, 1, optimizer_emit(DRV_GPU_IR_ISUB, integer, shift, 0));
	optimizer_store(14, 2, optimizer_emit(DRV_GPU_IR_ISUB, shift, integer, 0));
	optimizer_store(14, 3, optimizer_emit(DRV_GPU_IR_SHR, minus_seven, optimizer_constant(DRV_GPU_IR_ICONST, 35), 0));

	/* A maximum of a value with itself quiets a signalling NaN, so it stays. */
	optimizer_store(13, 0, optimizer_emit(DRV_GPU_IR_FMAX, z, z, 0));

	/* A repeated load and lookup are shared; an unread lookup and an unread chain go. */
	repeated = optimizer_load(0);
	optimizer_store(13, 1, optimizer_emit(DRV_GPU_IR_FADD, repeated, x, 0));
	texel = optimizer_emit(DRV_GPU_IR_SAMPLE, x, y, 0);
	value = optimizer_emit(DRV_GPU_IR_SAMPLE, x, y, 0);
	optimizer_store(13, 2, optimizer_emit(DRV_GPU_IR_FADD, texel + 1U, value + 2U, 0));
	optimizer_emit(DRV_GPU_IR_SAMPLE, y, x, 0);
	value = optimizer_emit(DRV_GPU_IR_FMUL, z, w, 0);
	optimizer_emit(DRV_GPU_IR_FADD, value, x, 0);

	/* A store replaced by a later one is not needed. */
	optimizer_store(13, 3, x);
	optimizer_store(13, 3, optimizer_emit(DRV_GPU_IR_FMUL, x, half, 0));

	/* A vertex program also stores its position, the first with an identity a vertex program keeps. */
	if (stage == DRV_GPU_STAGE_VERTEX) {
		value = optimizer_emit(DRV_GPU_IR_FMUL, x, one, 0);
		optimizer_store(DRV_GPU_IR_LOCATION_POSITION, 0, optimizer_emit(DRV_GPU_IR_FADD, value, y, 0));
		optimizer_store(DRV_GPU_IR_LOCATION_POSITION, 1, y);
		optimizer_store(DRV_GPU_IR_LOCATION_POSITION, 2, z);
		optimizer_store(DRV_GPU_IR_LOCATION_POSITION, 3, one);
	}

	/* Describes the program. */
	memset(ir, 0, sizeof(*ir));
	ir->stage = stage;
	ir->instructions = optimizer_code;
	ir->instruction_count = optimizer_count;
	ir->value_count = optimizer_values;
	ir->push_bytes = 28;
}

/* Counts the operations of one kind, reading the constant bits given when they are not 0xffffffff. */
static uint32_t
optimizer_count_operation(
	const struct drv_gpu_shader_ir *ir,
	enum drv_gpu_shader_ir_op operation,
	uint32_t constant)
{
	const struct drv_gpu_shader_ir_inst *instruction;
	const struct drv_gpu_shader_ir_inst *definition;
	uint32_t index;
	uint32_t source;
	uint32_t count;
	uint32_t found;

	/* Visits every operation of the kind. */
	count = 0;
	for (index = 0; index < ir->instruction_count; index++) {
		instruction = &ir->instructions[index];
		if (instruction->op != operation)
			continue;
		if (constant == 0xffffffffU) {
			count++;
			continue;
		}

		/* Looks for an operand defined as the constant. */
		found = 0;
		for (source = 0; source < 2; source++) {
			for (definition = ir->instructions; definition < instruction; definition++) {
				if (definition->dst == instruction->src[source] &&
				    (definition->op == DRV_GPU_IR_CONST || definition->op == DRV_GPU_IR_ICONST || definition->op == DRV_GPU_IR_BOOL) &&
				    definition->immediate == constant)
					found = 1;
			}
		}

		/* Counts the operation when an operand is the constant. */
		count += found;
	}

	/* Reports the count. */
	return count;
}

/* Draws one push word for the optimizer test: edges, a quiet NaN, a denormal, ordinary floats and integers. */
static uint32_t
optimizer_push_word(void)
{
	static const uint32_t edges[12] = {
		0x00000000U, 0x80000000U, 0x3f800000U, 0xbf800000U, 0x3f000000U, 0x40000000U,
		0x7fc00001U, 0x7f800000U, 0xff800000U, 0x00000003U, 0xffffffffU, 0x0000001fU
	};
	uint32_t choice;

	/* Chooses an edge half the time. */
	choice = next_random() % 4U;
	if (choice < 2U)
		return edges[next_random() % 12U];

	/* Otherwise an ordinary float or arbitrary bits. */
	if (choice == 2U)
		return random_float();

	/* Reports arbitrary bits. */
	return next_random();
}

/*
 * Checks the optimizer's rewritten programs against the programs as written.
 *
 * Both are run on the source interpreter with the same random inputs and
 * every stored output must agree bit for bit; counts of what remains check
 * the rules the interpreter cannot see (a floating point identity kept for
 * an output reader, operands of a minimum kept in order) and that a vertex
 * program is only cleared of repeated and unread work.
 */
static void
verify_optimizer(void)
{
	static const enum bcm2711_shader_stage stages[3] = {
		BCM2711_SHADER_FRAGMENT, BCM2711_SHADER_COORDINATE, BCM2711_SHADER_VERTEX
	};
	struct drv_gpu_shader_ir ir;
	struct drv_gpu_shader_ir optimized;
	struct drv_gpu_shader_optimize_options options;
	struct shader_inputs original;
	struct shader_inputs rewritten;
	uint32_t stage;
	uint32_t trial;
	uint32_t slot;
	uint32_t word;
	int error;

	/* Each native stage's options, as the bcm2711 compiler gives them. */
	for (stage = 0; stage < 3; stage++) {
		/* Builds and optimizes the program for the stage. */
		if (stages[stage] == BCM2711_SHADER_FRAGMENT) {
			optimizer_program(&ir, DRV_GPU_STAGE_FRAGMENT);
		} else {
			optimizer_program(&ir, DRV_GPU_STAGE_VERTEX);
		}

		/* Optimizes it as the bcm2711 compiler would. */
		optimizer_options(stages[stage], &options);
		error = drv_gpu_shader_optimize(&ir, &options, &optimized);
		assert(error == 0 && optimized.instructions != NULL);
		assert(optimized.instruction_count < ir.instruction_count);

		/* Runs both programs on the same inputs. */
		random_state = 0x2545f491U + stage;
		for (trial = 0; trial < 4096U; trial++) {
			memset(&original, 0, sizeof(original));
			for (word = 0; word < 7; word++)
				original.push[word] = optimizer_push_word();
			original.push[6] = next_random() % 48U;
			rewritten = original;
			source_program(&ir, &original);
			source_program(&optimized, &rewritten);
			assert(rewritten.lookups <= original.lookups);

			/* A fragment program keeps every output; a vertex program its position and the key's varying. */
			for (slot = 0; slot < 68; slot++) {
				if (stages[stage] == BCM2711_SHADER_COORDINATE && slot < 64)
					continue;
				if (stages[stage] == BCM2711_SHADER_VERTEX && slot < 64 && slot != 13 * 4 + 2)
					continue;
				if (rewritten.output[slot] != original.output[slot]) {
					fprintf(stderr, "optimizer stage %u trial %u: output %u is %08x, written %08x\n", stages[stage], trial, slot, rewritten.output[slot], original.output[slot]);
					assert(0);
				}
			}
		}

		/* Repeated loads and lookups are shared and unread ones dropped in every stage. */
		if (stages[stage] == BCM2711_SHADER_FRAGMENT) {
			assert(optimizer_count_operation(&optimized, DRV_GPU_IR_SAMPLE, 0xffffffffU) == 1);
			assert(optimizer_count_operation(&optimized, DRV_GPU_IR_LOAD_PUSH, 0xffffffffU) == 7);
		}

		/* A fragment program keeps the identities its readers could tell, and both orders of a minimum. */
		if (stages[stage] == BCM2711_SHADER_FRAGMENT) {
			assert(optimizer_count_operation(&optimized, DRV_GPU_IR_FMUL, 0x3f800000U) == 1);
			assert(optimizer_count_operation(&optimized, DRV_GPU_IR_FADD, 0) == 1);
			assert(optimizer_count_operation(&optimized, DRV_GPU_IR_FADD, 0x80000000U) == 0);
			assert(optimizer_count_operation(&optimized, DRV_GPU_IR_FSUB, 0x80000000U) == 1);
			assert(optimizer_count_operation(&optimized, DRV_GPU_IR_FSUB, 0) == 0);
			assert(optimizer_count_operation(&optimized, DRV_GPU_IR_FMIN, 0xffffffffU) == 2);
			assert(optimizer_count_operation(&optimized, DRV_GPU_IR_FMAX, 0xffffffffU) == 2);
			assert(optimizer_count_operation(&optimized, DRV_GPU_IR_FMUL, 0xffffffffU) == 4);
			assert(optimizer_count_operation(&optimized, DRV_GPU_IR_I2F, 0xffffffffU) == 1);
			assert(optimizer_count_operation(&optimized, DRV_GPU_IR_FLT, 0xffffffffU) == 1);
		}

		/* A vertex program is not simplified: its identities stay, and only its position and passed varying are computed. */
		if (stages[stage] != BCM2711_SHADER_FRAGMENT) {
			assert(optimizer_count_operation(&optimized, DRV_GPU_IR_FMUL, 0x3f800000U) == 1);
			assert(optimizer_count_operation(&optimized, DRV_GPU_IR_STORE_OUTPUT, 0xffffffffU) == 4 + (stages[stage] == BCM2711_SHADER_VERTEX));
		}

		/* The rewritten instructions are the caller's to release. */
		kern_free(optimized.instructions);
	}

	/* Skippable blocks and texture guards. */
	verify_optimizer_blocks();

	/* Reports the rules checked. */
	puts("WS141 shader optimizer: PASS (sharing, dead code, folding and identities, against the program as written)");
}

/*
 * Gives the options the bcm2711 compiler gives for one native stage.
 *
 * A fragment program keeps every output and is simplified; a coordinate
 * program needs its position, a vertex program also varying 13.2 (the test
 * key's one varying); neither is simplified.
 */
static void
optimizer_options(
	enum bcm2711_shader_stage stage,
	struct drv_gpu_shader_optimize_options *options)
{
	uint32_t index;

	/* The native programs run every block. */
	memset(options, 0, sizeof(*options));
	options->runs_every_block = 1;

	/* A fragment program needs every slot and is simplified. */
	if (stage == BCM2711_SHADER_FRAGMENT) {
		memset(options->needed, 1, sizeof(options->needed));
		options->simplify = 1;
		return;
	}

	/* A vertex program needs its position and, for the render stage, the varying. */
	for (index = 0; index < 4; index++)
		options->needed[DRV_GPU_IR_OPTIMIZE_POSITION + index] = 1;
	if (stage == BCM2711_SHADER_VERTEX)
		options->needed[13 * 4 + 2] = 1;
}

/*
 * Checks how the optimization treats skippable blocks and texture guards.
 *
 * A backend that may skip a block (the i915 one) gets such a program back
 * unchanged; one that runs every block gets it without markers or guards.
 */
static void
verify_optimizer_blocks(void)
{
	struct drv_gpu_shader_ir ir;
	struct drv_gpu_shader_ir optimized;
	struct drv_gpu_shader_optimize_options options;
	uint32_t x;
	uint32_t y;
	uint32_t predicate;
	uint32_t texel;
	uint32_t pass;
	int error;

	/* Runs the two programs, with a block and with a guard, for both kinds of backend. */
	for (pass = 0; pass < 4; pass++) {
		/* A repeated product inside and after a block, or a guarded repeated lookup. */
		optimizer_count = 0;
		optimizer_values = 0;
		x = optimizer_load(0);
		y = optimizer_load(4);
		predicate = optimizer_emit(DRV_GPU_IR_FLT, x, y, 0);
		if (pass % 2U == 0) {
			optimizer_emit(DRV_GPU_IR_SKIP_BEGIN, predicate, 0, 0);
			optimizer_values--;
			optimizer_code[optimizer_count - 1].dst = 0;
			optimizer_store(0, 0, optimizer_emit(DRV_GPU_IR_FMUL, x, y, 0));
			optimizer_emit(DRV_GPU_IR_SKIP_END, 0, 0, 0);
			optimizer_values--;
			optimizer_code[optimizer_count - 1].dst = 0;
			optimizer_store(0, 1, optimizer_emit(DRV_GPU_IR_FMUL, x, y, 0));
		} else {
			texel = optimizer_emit(DRV_GPU_IR_SAMPLE, x, y, 0);
			optimizer_code[optimizer_count - 1].guard = predicate + 1U;
			optimizer_store(0, 0, texel);
			texel = optimizer_emit(DRV_GPU_IR_SAMPLE, x, y, 0);
			optimizer_store(0, 1, texel);
		}

		/* Describes the program. */
		memset(&ir, 0, sizeof(ir));
		ir.stage = DRV_GPU_STAGE_FRAGMENT;
		ir.instructions = optimizer_code;
		ir.instruction_count = optimizer_count;
		ir.value_count = optimizer_values;
		ir.push_bytes = 8;

		/* Optimizes for a backend that may skip blocks, then for one that runs them all. */
		memset(&options, 0, sizeof(options));
		memset(options.needed, 1, sizeof(options.needed));
		options.simplify = 1;
		options.runs_every_block = pass / 2U;
		error = drv_gpu_shader_optimize(&ir, &options, &optimized);
		assert(error == 0);
		if (options.runs_every_block == 0) {
			assert(optimized.instructions == NULL);
			continue;
		}

		/* The markers, the guard, the predicate and the repeated work are gone. */
		assert(optimized.instructions != NULL);
		assert(optimizer_count_operation(&optimized, DRV_GPU_IR_SKIP_BEGIN, 0xffffffffU) == 0);
		assert(optimizer_count_operation(&optimized, DRV_GPU_IR_SKIP_END, 0xffffffffU) == 0);
		assert(optimizer_count_operation(&optimized, DRV_GPU_IR_FLT, 0xffffffffU) == 0);
		assert(optimizer_count_operation(&optimized, DRV_GPU_IR_FMUL, 0xffffffffU) + optimizer_count_operation(&optimized, DRV_GPU_IR_SAMPLE, 0xffffffffU) == 1);
		assert(optimized.instructions[2].guard == 0);
		kern_free(optimized.instructions);
	}
}
