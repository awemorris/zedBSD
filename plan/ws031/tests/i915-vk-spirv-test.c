/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host fixture for the SPIR-V parser (p004). Parses the vkdemo vertex and
 * fragment shaders and checks the extracted interface and instruction stream.
 * VK_REPO names the repository root so the .spv files can be read.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned fixture_live;

void *
kern_calloc(size_t count, size_t size)
{
	void *pointer = calloc(count, size);
	if (pointer != NULL)
		fixture_live++;
	return pointer;
}

void
kern_free(void *pointer)
{
	if (pointer != NULL)
		fixture_live--;
	free(pointer);
}

#include "../../../src/drivers/gpu/compiler/spirv.c"

/* Loads a SPIR-V file into a word buffer the caller frees. */
static uint32_t *
load_spv(const char *name, size_t *words)
{
	char path[512];
	FILE *file;
	long size;
	uint32_t *code;

	snprintf(path, sizeof(path), "%s/userland/tests/vkdemo/shaders/%s", VK_REPO, name);
	file = fopen(path, "rb");
	assert(file != NULL);
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	assert(size > 0 && (size % 4) == 0);
	code = malloc((size_t)size);
	assert(code != NULL);
	assert(fread(code, 1, (size_t)size, file) == (size_t)size);
	fclose(file);
	*words = (size_t)size / 4U;
	return code;
}

/* Loads one of the compiler test's SPIR-V files (compiler-shaders/) into a word buffer the caller frees. */
static uint32_t *
load_compiler_spv(const char *name, size_t *words)
{
	char path[512];
	FILE *file;
	long size;
	uint32_t *code;

	snprintf(path, sizeof(path), "%s/src/drivers/gpu/i915/tests/render/compiler-shaders/%s", VK_REPO, name);
	file = fopen(path, "rb");
	assert(file != NULL);
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	assert(size > 0 && (size % 4) == 0);
	code = malloc((size_t)size);
	assert(code != NULL);
	assert(fread(code, 1, (size_t)size, file) == (size_t)size);
	fclose(file);
	*words = (size_t)size / 4U;
	return code;
}

/* Counts IR instructions of one opcode. */
static unsigned
count_op(const struct drv_gpu_shader_ir *ir, enum drv_gpu_shader_ir_op op)
{
	unsigned found;
	unsigned index;

	found = 0U;
	for (index = 0U; index < ir->instruction_count; index++) {
		if (ir->instructions[index].op == op)
			found++;
	}
	return found;
}

/*
 * The vkdemo vertex shader (glslc -O0, as shipped) keeps its intermediate values in
 * Function-storage variables and uses float constants, OpFNegate, component access and
 * 3/4-element composites.  It parses now; what the IR COMPUTES is checked by the lowering
 * fixture (i915-vk-lower-test.c), here only the interface and the shape of the stream.
 */
static void
test_vertex(void)
{
	uint32_t *code;
	size_t words;
	struct drv_gpu_shader_ir *ir;
	struct drv_gpu_compile_diagnostic diag;
	int error;

	code = load_spv("cuboid.vert.spv", &words);
	error = drv_gpu_shader_parse(code, words, DRV_GPU_STAGE_VERTEX, &ir, &diag);
	if (error != 0)
		printf("  vkdemo VS refused: opcode %u at word %u: %s\n", diag.opcode, diag.word_offset, diag.reason);
	assert(error == 0 && ir != NULL);
	assert(ir->stage == DRV_GPU_STAGE_VERTEX);
	assert(ir->input_count == 2U && ir->output_count == 1U && ir->uniform_count == 0U);
	assert(ir->inputs[0].location == 0U && ir->inputs[0].components == 3U);
	assert(ir->inputs[1].location == 1U && ir->inputs[1].components == 2U);
	assert(ir->outputs[0].location == 0U && ir->outputs[0].components == 2U);
	assert(ir->push_bytes == 4U);                                   /* one float: animation.seconds */
	assert(count_op(ir, DRV_GPU_IR_SIN) == 2U && count_op(ir, DRV_GPU_IR_COS) == 2U);
	assert(count_op(ir, DRV_GPU_IR_FNEG) == 1U);                    /* -sy; the -1.6 is a constant */
	assert(count_op(ir, DRV_GPU_IR_LOAD_PUSH) == 2U);               /* seconds is read twice */
	assert(count_op(ir, DRV_GPU_IR_STORE_OUTPUT) == 6U);            /* gl_Position 4 + texture_coordinate 2 */
	drv_gpu_shader_ir_free(ir);
	free(code);
}

/* a minimal module: header, OpFunction, one body instruction, OpFunctionEnd */
static int
parse_body_instruction(const uint32_t *inst, unsigned inst_words, struct drv_gpu_compile_diagnostic *diag)
{
	uint32_t module[32];
	struct drv_gpu_shader_ir *ir;
	unsigned n = 0U, i;
	int error;

	module[n++] = 0x07230203U; module[n++] = 0x00010000U; module[n++] = 0U; module[n++] = 16U; module[n++] = 0U;
	module[n++] = (5U << 16) | 54U; module[n++] = 1U; module[n++] = 2U; module[n++] = 0U; module[n++] = 3U;   /* OpFunction */
	module[n++] = (2U << 16) | 248U; module[n++] = 4U;                                                        /* OpLabel */
	for (i = 0U; i < inst_words; i++)
		module[n++] = inst[i];
	module[n++] = (1U << 16) | 253U;                                                                          /* OpReturn */
	module[n++] = (1U << 16) | 56U;                                                                           /* OpFunctionEnd */
	error = drv_gpu_shader_parse(module, n, DRV_GPU_STAGE_VERTEX, &ir, diag);
	if (error == 0)
		drv_gpu_shader_ir_free(ir);
	else
		assert(ir == NULL);
	return error;
}

static void
test_body_classification(void)
{
	struct drv_gpu_compile_diagnostic diag;
	/* OpLine (debug): file id 5, line 1, column 1 -- no execution semantics */
	static const uint32_t op_line[4] = { (4U << 16) | 8U, 5U, 1U, 1U };
	/* OpFNegate %6 = -%7, where %7 is not a float value (nothing defines it): refused, not skipped */
	static const uint32_t op_fnegate[4] = { (4U << 16) | 127U, 1U, 6U, 7U };
	/* OpFDiv of %7, which is not a float value (nothing defines it): refused, not skipped */
	static const uint32_t op_fdiv[5] = { (5U << 16) | 136U, 1U, 6U, 7U, 8U };
	/* OpBranch back to the block it ends (%4): a loop, which is not lowered */
	static const uint32_t op_branch[2] = { (2U << 16) | 249U, 4U };
	/* OpFunctionCall */
	static const uint32_t op_call[4] = { (4U << 16) | 57U, 1U, 6U, 7U };
	/* an instruction whose length runs past the module */
	static const uint32_t op_overrun[1] = { (9U << 16) | 129U };
	/* OpExtInst with a GLSL.std.450 instruction that is not lowered (Tan = 15) */
	static const uint32_t op_tan[6] = { (6U << 16) | 12U, 1U, 6U, 9U, 15U, 7U };

	assert(parse_body_instruction(op_line, 4U, &diag) == 0);
	assert(parse_body_instruction(op_fnegate, 4U, &diag) == ENOTSUP && diag.opcode == 127U);
	assert(parse_body_instruction(op_fdiv, 5U, &diag) == ENOTSUP && diag.opcode == 136U);
	assert(parse_body_instruction(op_branch, 2U, &diag) == ENOTSUP && diag.opcode == 249U);
	assert(parse_body_instruction(op_call, 4U, &diag) == ENOTSUP && diag.opcode == 57U);
	assert(parse_body_instruction(op_tan, 6U, &diag) == ENOTSUP && diag.opcode == 12U);
	/* malformed stays EINVAL: a different thing from "valid but not lowered" */
	assert(parse_body_instruction(op_overrun, 1U, &diag) == EINVAL);
}

/* Parses a geometry fixture, which must parse; the caller frees the IR. */
static struct drv_gpu_shader_ir *
parse_geometry(const char *name)
{
	struct drv_gpu_compile_diagnostic diag;
	struct drv_gpu_shader_ir *ir;
	uint32_t *code;
	size_t words;
	int error;

	code = load_compiler_spv(name, &words);
	error = drv_gpu_shader_parse(code, words, DRV_GPU_STAGE_VERTEX, &ir, &diag);
	if (error != 0)
		printf("  %s refused: opcode %u at word %u: %s\n", name, diag.opcode, diag.word_offset, diag.reason);
	assert(error == 0 && ir != NULL);
	free(code);
	return ir;
}

/* Checks that every EMIT_VERTEX and END_PRIMITIVE is predicated (inside a loop) or not (straight code). */
static void
check_emits(const struct drv_gpu_shader_ir *ir, uint32_t predicated)
{
	unsigned index;
	const struct drv_gpu_shader_ir_inst *inst;

	for (index = 0U; index < ir->instruction_count; index++) {
		inst = &ir->instructions[index];
		if (inst->op != DRV_GPU_IR_EMIT_VERTEX && inst->op != DRV_GPU_IR_END_PRIMITIVE)
			continue;
		assert(inst->component == predicated);
		if (predicated != 0U)
			assert(inst->src[0] < ir->value_count);
	}
}

/*
 * glxtest's points shader: one input vertex, a triangle strip of four, gl_in[0].gl_Position read at a constant vertex,
 * four emits and one end in straight code.
 */
static void
test_geometry_points(void)
{
	struct drv_gpu_shader_ir *ir;
	const struct drv_gpu_shader_ir_inst *inst;
	unsigned index;
	unsigned loads;

	ir = parse_geometry("points.geom.spv");
	assert(ir->stage == DRV_GPU_STAGE_GEOMETRY);
	assert(ir->vertices_in == 1U);
	assert(ir->output_topology == DRV_GPU_IR_OUTPUT_TRIANGLE_STRIP);
	assert(ir->max_vertices == 4U);
	assert(ir->uses_end_primitive == 1U);
	assert(ir->uses_primitive_id == 0U && ir->writes_layer == 0U);
	assert(ir->input_count == 0U);
	assert(ir->output_count == 1U && ir->outputs[0].location == 0U);
	assert(count_op(ir, DRV_GPU_IR_EMIT_VERTEX) == 4U);
	assert(count_op(ir, DRV_GPU_IR_END_PRIMITIVE) == 1U);
	check_emits(ir, 0U);

	/* The centre is the four components of vertex 0's position. */
	loads = 0U;
	for (index = 0U; index < ir->instruction_count; index++) {
		inst = &ir->instructions[index];
		if (inst->op != DRV_GPU_IR_LOAD_VERTEX_INPUT)
			continue;
		assert(inst->location == DRV_GPU_IR_LOCATION_POSITION);
		assert(inst->immediate == 0U);
		assert(inst->component == loads);
		loads++;
	}
	assert(loads == 4U);
	drv_gpu_shader_ir_free(ir);
}

/* glxtest's lines-with-adjacency shader: four input vertices, gl_in[3].gl_Position.x read, a store under a selection. */
static void
test_geometry_adjacency(void)
{
	struct drv_gpu_shader_ir *ir;
	const struct drv_gpu_shader_ir_inst *inst;
	unsigned index;
	unsigned loads;

	ir = parse_geometry("adjacency.geom.spv");
	assert(ir->vertices_in == 4U);
	assert(ir->output_topology == DRV_GPU_IR_OUTPUT_TRIANGLE_STRIP);
	assert(ir->max_vertices == 4U);
	assert(count_op(ir, DRV_GPU_IR_EMIT_VERTEX) == 4U);
	assert(count_op(ir, DRV_GPU_IR_END_PRIMITIVE) == 1U);
	assert(count_op(ir, DRV_GPU_IR_SELECT) >= 1U);
	check_emits(ir, 0U);

	/* Only the fourth vertex's x is read. */
	loads = 0U;
	for (index = 0U; index < ir->instruction_count; index++) {
		inst = &ir->instructions[index];
		if (inst->op != DRV_GPU_IR_LOAD_VERTEX_INPUT)
			continue;
		assert(inst->location == DRV_GPU_IR_LOCATION_POSITION);
		assert(inst->immediate == 3U && inst->component == 0U);
		loads++;
	}
	assert(loads == 1U);
	drv_gpu_shader_ir_free(ir);
}

/*
 * glxtest's layered shader: three input vertices, two loops, gl_in[i] chosen at run time, gl_Layer written, the
 * emit and the end predicated by the loops' channels.
 */
static void
test_geometry_layers(void)
{
	struct drv_gpu_shader_ir *ir;
	const struct drv_gpu_shader_ir_inst *inst;
	unsigned index;
	unsigned loads;
	unsigned layers;

	ir = parse_geometry("layers.geom.spv");
	assert(ir->vertices_in == 3U);
	assert(ir->max_vertices == 6U);
	assert(ir->writes_layer == 1U);
	assert(ir->uses_end_primitive == 1U);
	assert(count_op(ir, DRV_GPU_IR_LOOP_BEGIN) == 2U);
	assert(count_op(ir, DRV_GPU_IR_EMIT_VERTEX) == 1U);
	assert(count_op(ir, DRV_GPU_IR_END_PRIMITIVE) == 1U);
	check_emits(ir, 1U);

	/* The position comes from the vertex i names; the layer is written to the VUE header's place. */
	loads = 0U;
	layers = 0U;
	for (index = 0U; index < ir->instruction_count; index++) {
		inst = &ir->instructions[index];
		if (inst->op == DRV_GPU_IR_STORE_OUTPUT && inst->location == DRV_GPU_IR_LOCATION_LAYER) {
			assert(inst->component == 0U);
			layers++;
		}
		if (inst->op != DRV_GPU_IR_LOAD_VERTEX_INPUT)
			continue;
		assert(inst->location == DRV_GPU_IR_LOCATION_POSITION);
		assert(inst->immediate == DRV_GPU_IR_VERTEX_DYNAMIC);
		assert(inst->src[0] < ir->value_count);
		loads++;
	}
	assert(loads == 4U);
	assert(layers == 1U);
	drv_gpu_shader_ir_free(ir);
}

/*
 * WS068's varyings: located per-vertex inputs and an array of blocks, read at a vertex chosen at run time,
 * gl_PrimitiveIDIn read and gl_PrimitiveID written as the varying at its own location.
 */
static void
test_geometry_varyings(void)
{
	struct drv_gpu_shader_ir *ir;
	const struct drv_gpu_shader_ir_inst *inst;
	unsigned index;
	unsigned seen[3];
	unsigned primitive_id_loads;
	unsigned primitive_id_stores;

	ir = parse_geometry("varyings.geom.spv");
	assert(ir->vertices_in == 3U);
	assert(ir->uses_primitive_id == 1U);
	assert(ir->writes_layer == 1U);

	/* One input vertex's element: v_colour at 0, the block's uv at 1 and id at 2. */
	assert(ir->input_count == 3U);
	assert(ir->inputs[0].location == 0U && ir->inputs[0].components == 3U);
	assert(ir->inputs[1].location == 1U);
	assert(ir->inputs[2].location == 2U);

	memset(seen, 0, sizeof(seen));
	primitive_id_loads = 0U;
	primitive_id_stores = 0U;
	for (index = 0U; index < ir->instruction_count; index++) {
		inst = &ir->instructions[index];
		if (inst->op == DRV_GPU_IR_LOAD_SYSTEM) {
			assert(inst->component == DRV_GPU_IR_SYSTEM_PRIMITIVE_ID);
			primitive_id_loads++;
		}
		if (inst->op == DRV_GPU_IR_STORE_OUTPUT && inst->location == DRV_GPU_SHADER_LOCATION_PRIMITIVE_ID)
			primitive_id_stores++;
		if (inst->op != DRV_GPU_IR_LOAD_VERTEX_INPUT)
			continue;
		assert(inst->immediate == DRV_GPU_IR_VERTEX_DYNAMIC);
		if (inst->location == DRV_GPU_IR_LOCATION_POSITION)
			continue;
		assert(inst->location <= 2U);
		seen[inst->location]++;
	}
	assert(seen[0] == 3U && seen[1] == 2U && seen[2] == 1U);
	assert(primitive_id_loads >= 1U);
	assert(primitive_id_stores == 1U);
	drv_gpu_shader_ir_free(ir);
}

/* Parses a fixture the parser must refuse, and checks the reason names what is refused. */
static void
check_geometry_refused(const char *name, const char *reason)
{
	struct drv_gpu_compile_diagnostic diag;
	struct drv_gpu_shader_ir *ir;
	uint32_t *code;
	size_t words;
	int error;

	code = load_compiler_spv(name, &words);
	error = drv_gpu_shader_parse(code, words, DRV_GPU_STAGE_VERTEX, &ir, &diag);
	assert(error == ENOTSUP);
	assert(ir == NULL);
	assert(diag.reason != NULL);
	if (strstr(diag.reason, reason) == NULL)
		printf("  %s refused for another reason: %s\n", name, diag.reason);
	assert(strstr(diag.reason, reason) != NULL);
	free(code);
}

/* What a geometry shader may not do is refused with its reason, never parsed into something else. */
static void
test_geometry_refused(void)
{
	struct drv_gpu_compile_diagnostic diag;
	/* OpEmitVertex in a vertex shader */
	static const uint32_t op_emit[1] = { (1U << 16) | 218U };

	check_geometry_refused("refuse-invocations.geom.spv", "geometry invocations other than one");
	check_geometry_refused("refuse-invocation-id.geom.spv", "gl_InvocationID");
	check_geometry_refused("refuse-viewport.geom.spv", "gl_ViewportIndex");
	check_geometry_refused("refuse-clip-distance.geom.spv", "gl_in member other than");
	check_geometry_refused("refuse-length.spv", "length other than the input primitive's vertex count");
	assert(parse_body_instruction(op_emit, 1U, &diag) == ENOTSUP && diag.opcode == 218U);
}

/* A fragment shader's gl_PrimitiveID is a Flat input at its own location. */
static void
test_fragment_primitive_id(void)
{
	struct drv_gpu_shader_ir *ir;
	uint32_t *code;
	size_t words;
	int error;

	code = load_compiler_spv("primitive-id.frag.spv", &words);
	error = drv_gpu_shader_parse(code, words, DRV_GPU_STAGE_FRAGMENT, &ir, NULL);
	assert(error == 0);
	assert(ir->input_count == 1U);
	assert(ir->inputs[0].location == DRV_GPU_SHADER_LOCATION_PRIMITIVE_ID);
	assert(ir->inputs[0].flat == 1U);
	assert(count_op(ir, DRV_GPU_IR_LOAD_INPUT) == 1U);
	drv_gpu_shader_ir_free(ir);
	free(code);
}

static void
test_fragment(void)
{
	uint32_t *code;
	size_t words;
	struct drv_gpu_shader_ir *ir;
	int error;

	code = load_spv("cuboid.frag.spv", &words);
	error = drv_gpu_shader_parse(code, words, DRV_GPU_STAGE_FRAGMENT, &ir, NULL);
	assert(error == 0);

	/* The fragment shader samples one texture at an interpolated coordinate. */
	assert(ir->stage == DRV_GPU_STAGE_FRAGMENT);
	assert(ir->input_count == 1U);
	assert(ir->inputs[0].location == 0U && ir->inputs[0].components == 2U);
	assert(ir->output_count == 1U);
	assert(ir->outputs[0].location == 0U && ir->outputs[0].components == 4U);
	assert(ir->uniform_count == 1U);
	assert(ir->uniforms[0].set == 0U && ir->uniforms[0].binding == 0U);
	assert(count_op(ir, DRV_GPU_IR_SAMPLE) >= 1U);

	drv_gpu_shader_ir_free(ir);
	free(code);
}

static void
test_rejects_garbage(void)
{
	uint32_t bad[8];
	uint32_t *code;
	size_t words;
	struct drv_gpu_shader_ir *ir;
	struct drv_gpu_compile_diagnostic diagnostic;
	int error;

	/* A missing stream, output slot or unrepresentable count cannot publish parser state. */
	memset(&diagnostic, 0x5a, sizeof(diagnostic));
	error = drv_gpu_shader_parse(NULL, 8U, DRV_GPU_STAGE_VERTEX, &ir, &diagnostic);
	assert(error == EINVAL && ir == NULL);
	assert(diagnostic.opcode == 0U && diagnostic.reason == NULL);
	error = drv_gpu_shader_parse(bad, 8U, DRV_GPU_STAGE_VERTEX, NULL, &diagnostic);
	assert(error == EINVAL && diagnostic.reason == NULL);
	error = drv_gpu_shader_parse(bad, (size_t)UINT32_MAX + 1U, DRV_GPU_STAGE_VERTEX, &ir, &diagnostic);
	assert(error == EINVAL && ir == NULL);
	error = drv_gpu_shader_parse(bad, 8U, (enum drv_gpu_shader_stage)-1, &ir, &diagnostic);
	assert(error == EINVAL && ir == NULL);

	/* A wrong magic is rejected without allocating an IR. */
	memset(bad, 0, sizeof(bad));
	bad[0] = 0x12345678U;
	error = drv_gpu_shader_parse(bad, 8U, DRV_GPU_STAGE_VERTEX, &ir, NULL);
	assert(error != 0);
	assert(ir == NULL);

	/* A valid module expected as a stage past DRV_GPU_STAGE_COUNT is refused as inconsistent (ws075-p007a a4). */
	code = load_spv("cuboid.frag.spv", &words);
	error = drv_gpu_shader_parse(code, words, DRV_GPU_STAGE_COUNT, &ir, NULL);
	assert(error == EINVAL);
	assert(ir == NULL);
	free(code);
}

int
main(void)
{
	test_vertex();
	test_body_classification();
	test_fragment();
	test_rejects_garbage();
	test_geometry_points();
	test_geometry_adjacency();
	test_geometry_layers();
	test_geometry_varyings();
	test_geometry_refused();
	test_fragment_primitive_id();
	assert(fixture_live == 0U);
	printf("i915 vk spirv host test PASS\n");
	return 0;
}
