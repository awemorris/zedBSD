/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host fixture for shader modules and graphics pipelines
 * (render/pipeline.c, pipeline-prepare.c) and the shader state a draw emits
 * for a pipeline (render/state.c).
 *
 * vkCreateShaderModule and vkCreateGraphicsPipelines are driven through the
 * wire with the vkdemo shaders, so the pipeline's kernels come from the
 * executor's own compiler; the vertex and pixel shader state is then
 * emitted from those kernels.  A second pipeline, made from the executor
 * test's shaders, declares its viewport and scissor dynamic and reads push
 * constants in both stages, as the model viewer's pipelines do.
 */

#include "i915-vk-render-stubs.inc"

#include "../../../src/drivers/gpu/i915/compiler/compiler.h"
#include "../../../src/drivers/gpu/i915/render/batch.h"
#include "../../../src/drivers/gpu/i915/render/heap.h"
#include "../../../src/drivers/gpu/i915/render/state.h"

#include "../../../src/drivers/gpu/i915/intel/genxml.h"

#include "../../../src/drivers/gpu/i915/tests/fixtures/generality-shaders-gen.inc"

/* The wire opcodes the fixture sends, as libvulkan numbers them. */
#define FIXTURE_CREATE_SHADER_MODULE		59U
#define FIXTURE_DESTROY_SHADER_MODULE		60U
#define FIXTURE_CREATE_GRAPHICS_PIPELINES	65U
#define FIXTURE_DESTROY_PIPELINE		67U

/* The SPIR-V opcodes the fixture rewrites: a float multiply of scalars becomes an outer product of them (refused). */
#define FIXTURE_SPIRV_FMUL	133U
#define FIXTURE_SPIRV_OUTER_PRODUCT	147U

/* The wire identities the fixture gives its objects. */
#define FIXTURE_DEVICE		0xd0ULL
#define FIXTURE_PROBE		0xc00ULL
#define FIXTURE_VS		0xd00ULL
#define FIXTURE_FS		0xd01ULL
#define FIXTURE_BAD_VS		0xd10ULL
#define FIXTURE_PIPELINE	0xe00ULL
#define FIXTURE_BAD_PIPELINE	0xe10ULL
#define FIXTURE_PLACE_VS	0xd20ULL
#define FIXTURE_PUSH_FS		0xd21ULL
#define FIXTURE_DYNAMIC_PIPELINE	0xe20ULL

/* The shaders of the executor test, compiled next to their GLSL. */
#define FIXTURE_EXECUTOR_SHADERS	"src/drivers/gpu/i915/tests/render/shaders"

/* The stream every command is built in. */
static struct stub_wire fixture_wire;

static uint32_t *fixture_load_spirv(const char *directory, const char *name, size_t *words);
static void fixture_shader_module(const uint32_t *code, size_t words, uint64_t identity);
static void fixture_stage(uint32_t stage, uint64_t module);
static void fixture_pipeline(uint64_t vertex, uint64_t fragment, uint64_t identity);
static void fixture_dynamic_pipeline(uint64_t vertex, uint64_t fragment, uint64_t identity);
static void fixture_destroy(uint32_t opcode, uint64_t identity);
static int fixture_find_command(const uint32_t *batch, unsigned used, uint32_t opcode);
static void test_shader_module(void);
static void test_graphics_pipeline(void);
static void test_dynamic_push_pipeline(void);
static void fixture_generality_pipeline(struct i915_gfx_pipeline *pipeline, struct i915_gfx_shader *vertex, struct i915_gfx_shader *fragment, const uint32_t *vertex_words, size_t vertex_bytes, const uint32_t *fragment_words, size_t fragment_bytes);
static void test_varying_routing(void);

/*
 * Runs the pipeline checks.
 */
int
main(void)
{
	/* Checks shader modules, then pipelines and the state they emit. */
	test_shader_module();
	test_graphics_pipeline();
	test_dynamic_push_pipeline();
	test_varying_routing();

	/* Succeeded: every check held. */
	printf("i915 vk pipe host test PASS\n");
	return 0;
}

/* Loads one SPIR-V shader of a directory of the tree into words the caller frees. */
static uint32_t *
fixture_load_spirv(
	const char *directory,
	const char *name,
	size_t *words)
{
	char path[512];
	FILE *file;
	uint32_t *code;
	size_t read;
	long size;
	int status;

	/* Opens the shader. */
	snprintf(path, sizeof(path), "%s/%s/%s", VK_REPO, directory, name);
	file = fopen(path, "rb");
	assert(file != NULL);

	/* Measures it: a module is whole words. */
	status = fseek(file, 0, SEEK_END);
	assert(status == 0);
	size = ftell(file);
	assert(size > 0);
	assert((size % 4) == 0);
	status = fseek(file, 0, SEEK_SET);
	assert(status == 0);

	/* Reads the words. */
	code = malloc((size_t)size);
	assert(code != NULL);
	read = fread(code, 1U, (size_t)size, file);
	assert(read == (size_t)size);
	fclose(file);

	/* Succeeded: the caller owns the words. */
	*words = (size_t)size / 4U;
	return code;
}

/* Appends vkCreateShaderModule for `words` words of SPIR-V. */
static void
fixture_shader_module(
	const uint32_t *code,
	size_t words,
	uint64_t identity)
{
	size_t index;

	/* The header, the device and the create info's presence marker. */
	stub_put32(&fixture_wire, FIXTURE_CREATE_SHADER_MODULE);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);

	/* VkShaderModuleCreateInfo: sType 16, no chain, flags, the size in bytes and the word count. */
	stub_put32(&fixture_wire, 16U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, (uint64_t)words * 4U);
	stub_put64(&fixture_wire, (uint64_t)words);

	/* The words themselves. */
	for (index = 0U; index < words; index++)
		stub_put32(&fixture_wire, code[index]);

	/* The tail: no allocator, the identity behind its presence marker. */
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, identity);
}

/* Appends one VkPipelineShaderStageCreateInfo naming `module` for `stage`, entry "main". */
static void
fixture_stage(
	uint32_t stage,
	uint64_t module)
{
	/* sType 18, no chain, flags, the stage and the module. */
	stub_put32(&fixture_wire, 18U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, stage);
	stub_put64(&fixture_wire, module);

	/* The entry point: five bytes with the terminator, padded to two words. */
	stub_put64(&fixture_wire, 5U);
	stub_put32(&fixture_wire, 0x6e69616dU);
	stub_put32(&fixture_wire, 0U);

	/* No specialization. */
	stub_put64(&fixture_wire, 0U);
}

/* Appends vkCreateGraphicsPipelines of one triangle-list pipeline with two stages. */
static void
fixture_pipeline(
	uint64_t vertex,
	uint64_t fragment,
	uint64_t identity)
{
	unsigned index;

	/* The header, the device, no cache and one create info. */
	stub_put32(&fixture_wire, FIXTURE_CREATE_GRAPHICS_PIPELINES);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 1U);

	/* VkGraphicsPipelineCreateInfo: sType 28, no chain, flags, two stages. */
	stub_put32(&fixture_wire, 28U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 2U);
	stub_put64(&fixture_wire, 2U);
	fixture_stage(VK_SHADER_STAGE_VERTEX_BIT, vertex);
	fixture_stage(VK_SHADER_STAGE_FRAGMENT_BIT, fragment);

	/* No vertex input; input assembly: sType 20, no chain, flags, triangle list, no restart. */
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 20U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	stub_put32(&fixture_wire, 0U);

	/* No tessellation and no viewport; the rasterization record, all zero, is always present. */
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	for (index = 0U; index < 14U; index++)
		stub_put32(&fixture_wire, 0U);

	/* No multisample, depth-stencil, colour blend or dynamic state. */
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);

	/* No layout or render pass, subpass 0, no base pipeline. */
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);

	/* No allocator, then one identity. */
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, identity);
}

/*
 * Appends vkCreateGraphicsPipelines of one triangle-list pipeline with two
 * stages, two vec4 attributes, one viewport and one scissor, both dynamic,
 * attachment 0 blending source alpha over the destination with the alpha
 * added and blue masked, blend constants (0.25, 0.5, 0.75, 1) and the blend
 * constants dynamic as well.
 */
static void
fixture_dynamic_pipeline(
	uint64_t vertex,
	uint64_t fragment,
	uint64_t identity)
{
	unsigned index;

	/* The header, the device, no cache and one create info. */
	stub_put32(&fixture_wire, FIXTURE_CREATE_GRAPHICS_PIPELINES);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 1U);

	/* VkGraphicsPipelineCreateInfo: sType 28, no chain, flags, two stages. */
	stub_put32(&fixture_wire, 28U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 2U);
	stub_put64(&fixture_wire, 2U);
	fixture_stage(VK_SHADER_STAGE_VERTEX_BIT, vertex);
	fixture_stage(VK_SHADER_STAGE_FRAGMENT_BIT, fragment);

	/* Vertex input: sType 19, no chain, flags, binding 0 of stride 32 per vertex. */
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 19U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 32U);
	stub_put32(&fixture_wire, VK_VERTEX_INPUT_RATE_VERTEX);

	/* Two vec4 attributes of binding 0: location 0 at 0 and location 1 at 16. */
	stub_put32(&fixture_wire, 2U);
	stub_put64(&fixture_wire, 2U);
	for (index = 0U; index < 2U; index++) {
		stub_put32(&fixture_wire, index);
		stub_put32(&fixture_wire, 0U);
		stub_put32(&fixture_wire, VK_FORMAT_R32G32B32A32_SFLOAT);
		stub_put32(&fixture_wire, index * 16U);
	}

	/* Input assembly: sType 20, no chain, flags, triangle list, no restart. */
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 20U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	stub_put32(&fixture_wire, 0U);

	/* No tessellation; viewport state: sType 22, one viewport and one scissor, neither array sent. */
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 22U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 0U);

	/* The rasterization record, all zero, is always present. */
	stub_put64(&fixture_wire, 1U);
	for (index = 0U; index < 14U; index++)
		stub_put32(&fixture_wire, 0U);

	/* No multisample or depth-stencil state. */
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);

	/* Colour blend state: sType 26, no chain, flags, no logic operation, one attachment. */
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 26U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 1U);

	/* Attachment 0: blending on, SRC_ALPHA / ONE_MINUS_SRC_ALPHA / ADD, ONE / ONE / ADD, R G A written. */
	stub_put32(&fixture_wire, VK_TRUE);
	stub_put32(&fixture_wire, VK_BLEND_FACTOR_SRC_ALPHA);
	stub_put32(&fixture_wire, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
	stub_put32(&fixture_wire, VK_BLEND_OP_ADD);
	stub_put32(&fixture_wire, VK_BLEND_FACTOR_ONE);
	stub_put32(&fixture_wire, VK_BLEND_FACTOR_ONE);
	stub_put32(&fixture_wire, VK_BLEND_OP_ADD);
	stub_put32(&fixture_wire, VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_A_BIT);

	/* The four blend constants. */
	stub_put64(&fixture_wire, 4U);
	stub_put32(&fixture_wire, 0x3e800000U);
	stub_put32(&fixture_wire, 0x3f000000U);
	stub_put32(&fixture_wire, 0x3f400000U);
	stub_put32(&fixture_wire, 0x3f800000U);

	/* Dynamic state: sType 27, no chain, flags, the viewport, the scissor and the blend constants. */
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 27U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 3U);
	stub_put64(&fixture_wire, 3U);
	stub_put32(&fixture_wire, VK_DYNAMIC_STATE_VIEWPORT);
	stub_put32(&fixture_wire, VK_DYNAMIC_STATE_SCISSOR);
	stub_put32(&fixture_wire, VK_DYNAMIC_STATE_BLEND_CONSTANTS);

	/* No layout or render pass, subpass 0, no base pipeline. */
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);

	/* No allocator, then one identity. */
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, identity);
}

/* Appends a generic destroy: [opcode][reply][device][identity][pAllocator]. */
static void
fixture_destroy(
	uint32_t opcode,
	uint64_t identity)
{
	/* The command has no reply body; the reply is its echoed opcode. */
	stub_put32(&fixture_wire, opcode);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, identity);
	stub_put64(&fixture_wire, 0U);
}

/* Finds the first dword of a batch whose command opcode is `opcode`; -1 when none is. */
static int
fixture_find_command(
	const uint32_t *batch,
	unsigned used,
	uint32_t opcode)
{
	unsigned index;

	/* Looks at every dword's high half. */
	for (index = 0U; index < used; index++) {
		if ((batch[index] >> 16) == opcode)
			return (int)index;
	}

	/* No dword carries the opcode. */
	return -1;
}

/*
 * A module keeps a copy of its words from the command's arena; code that is
 * not whole words fails the command.
 */
static void
test_shader_module(void)
{
	static const uint32_t probe[3] = {0x07230203U, 0x00010000U, 0x0badf00dU};
	struct i915_gfx_shader *shader;
	size_t reply_bytes;
	unsigned index;
	int error;

	/* Opens the fixture session. */
	stub_session_open(NULL);

	/* Three words round-trip: [59][VK_SUCCESS][present][identity]. */
	stub_wire_begin(&fixture_wire);
	fixture_shader_module(probe, 3U, FIXTURE_PROBE);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 24U);
	assert(stub_get32(stub_reply, 0U) == FIXTURE_CREATE_SHADER_MODULE);
	assert(stub_get64(stub_reply, 16U) == FIXTURE_PROBE);
	shader = drv_i915_object_lookup(stub_session, I915_VK_OBJ_SHADER_MODULE, FIXTURE_PROBE);
	assert(shader != NULL);
	assert(shader->word_count == 3U);
	for (index = 0U; index < 3U; index++)
		assert(shader->words[index] == probe[index]);

	/* vkDestroyShaderModule forgets and frees the module. */
	stub_wire_begin(&fixture_wire);
	fixture_destroy(FIXTURE_DESTROY_SHADER_MODULE, FIXTURE_PROBE);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 4U);
	shader = drv_i915_object_lookup(stub_session, I915_VK_OBJ_SHADER_MODULE, FIXTURE_PROBE);
	assert(shader == NULL);

	/* A code size that is not whole words fails the stream. */
	stub_wire_begin(&fixture_wire);
	fixture_shader_module(probe, 3U, FIXTURE_PROBE);
	fixture_wire.bytes[STUB_SELECTOR_BYTES + 8U + 8U + 8U + 4U + 8U + 4U] = 11U;
	error = stub_execute(&fixture_wire, &reply_bytes);
	assert(error == EINVAL);
	shader = drv_i915_object_lookup(stub_session, I915_VK_OBJ_SHADER_MODULE, FIXTURE_PROBE);
	assert(shader == NULL);

	/* Closes the session; nothing stays allocated. */
	stub_session_close();
	assert(stub_live == 0U);
}

/*
 * A pipeline made from the vkdemo shaders is compiled by the executor and
 * its kernels drive the shader state; a pipeline whose vertex shader the
 * compiler cannot lower is refused as a whole.
 */
static void
test_graphics_pipeline(void)
{
	struct i915_gfx_pipeline *pipeline;
	struct i915_gfx_kernels kernels;
	struct i915_gfx_batch batch;
	uint32_t commands[256];
	uint32_t *vertex;
	uint32_t *fragment;
	uint32_t *broken;
	unsigned long mark;
	size_t vertex_words;
	size_t fragment_words;
	size_t reply_bytes;
	size_t at;
	int found;

	/* Loads the vkdemo shaders and opens the fixture session. */
	vertex = fixture_load_spirv("userland/base/vkdemo/shaders", "cuboid.vert.spv", &vertex_words);
	fragment = fixture_load_spirv("userland/base/vkdemo/shaders", "cuboid.frag.spv", &fragment_words);
	stub_session_open(NULL);

	/*
	 * Makes a valid vertex shader with an instruction the compiler does not
	 * lower: the shipped shader with its first OpFMul turned into OpOuterProduct
	 * on the same operands.
	 */
	broken = malloc(vertex_words * sizeof(*broken));
	assert(broken != NULL);
	memcpy(broken, vertex, vertex_words * sizeof(*broken));
	for (at = 5U;
	     at < vertex_words && (broken[at] & 0xffffU) != FIXTURE_SPIRV_FMUL;
	     at += broken[at] >> 16)
		assert((broken[at] >> 16) != 0U);
	assert(at < vertex_words);
	broken[at] = (broken[at] & 0xffff0000U) | FIXTURE_SPIRV_OUTER_PRODUCT;

	/* Creates the three modules. */
	stub_wire_begin(&fixture_wire);
	fixture_shader_module(vertex, vertex_words, FIXTURE_VS);
	fixture_shader_module(fragment, fragment_words, FIXTURE_FS);
	fixture_shader_module(broken, vertex_words, FIXTURE_BAD_VS);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 3U * 24U);
	free(broken);

	/*
	 * The pipeline with the unlowered vertex shader fails as a whole: a
	 * defined VkResult and a null identity, and no pipeline published.
	 */
	mark = stub_allocation_mark();
	stub_wire_begin(&fixture_wire);
	fixture_pipeline(FIXTURE_BAD_VS, FIXTURE_FS, FIXTURE_BAD_PIPELINE);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 24U);
	assert(stub_get32(stub_reply, 0U) == FIXTURE_CREATE_GRAPHICS_PIPELINES);
	assert(stub_get32(stub_reply, 4U) == (uint32_t)VK_ERROR_FEATURE_NOT_PRESENT);
	assert(stub_get64(stub_reply, 8U) == 1U);
	assert(stub_get64(stub_reply, 16U) == 0U);
	pipeline = drv_i915_object_lookup(stub_session, I915_VK_OBJ_PIPELINE, FIXTURE_BAD_PIPELINE);
	assert(pipeline == NULL);

	/*
	 * XXX: the refused pipeline's record is neither published nor freed
	 * (render/pipeline.c, "happy path only"): exactly that one block is
	 * left behind, and its kernels were released.  The fixture reclaims it
	 * so the rest of the run starts clean.
	 */
	assert(stub_live_since(mark) == 1U);
	assert(stub_release_since(mark) == 1U);

	/* The pipeline made from the shipped shaders: [65][VK_SUCCESS][count 1][identity]. */
	stub_wire_begin(&fixture_wire);
	fixture_pipeline(FIXTURE_VS, FIXTURE_FS, FIXTURE_PIPELINE);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 24U);
	assert(stub_get32(stub_reply, 4U) == VK_SUCCESS);
	assert(stub_get64(stub_reply, 8U) == 1U);
	assert(stub_get64(stub_reply, 16U) == FIXTURE_PIPELINE);

	/* The pipeline holds both modules, its topology and two compiled kernels. */
	pipeline = drv_i915_object_lookup(stub_session, I915_VK_OBJ_PIPELINE, FIXTURE_PIPELINE);
	assert(pipeline != NULL);
	assert(pipeline->vertex == drv_i915_object_lookup(stub_session, I915_VK_OBJ_SHADER_MODULE, FIXTURE_VS));
	assert(pipeline->fragment == drv_i915_object_lookup(stub_session, I915_VK_OBJ_SHADER_MODULE, FIXTURE_FS));
	assert(pipeline->topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	assert(pipeline->kernels_ready != 0);
	assert(pipeline->vs_binary != NULL);
	assert(pipeline->fs_binary != NULL);
	assert(pipeline->vs_binary->code_bytes != 0U);
	assert(pipeline->fs_binary->code_bytes != 0U);

	/* The draw takes the kernels' code and interface from the pipeline. */
	drv_i915_gfx_pipeline_kernels(pipeline, &kernels);
	assert(kernels.vs_code == pipeline->vs_binary->code);
	assert(kernels.ps_code == pipeline->fs_binary->code);
	assert(kernels.varyings == pipeline->vs_binary->varying_count);

	/* Emits the vertex and pixel shader state of those kernels into a batch. */
	memset(commands, 0, sizeof(commands));
	batch.cmds = commands;
	batch.count = 0U;
	batch.capacity = 256U;
	batch.overflow = 0;
	drv_i915_gfx_emit_vertex_shader(&batch, &kernels);
	drv_i915_gfx_emit_pixel_shader(&batch, &kernels);
	assert(batch.overflow == 0);

	/* 3DSTATE_VS starts the vertex kernel at its heap offset. */
	found = fixture_find_command(commands, batch.count, GEN12_CMD_3DSTATE_VS);
	assert(found >= 0);
	assert(commands[found + 1] == I915_GFX_VS_KERNEL);
	assert((commands[found + 6] >> 20) == kernels.vs_grf_start);

	/* 3DSTATE_PS is emitted for the pixel kernel. */
	found = fixture_find_command(commands, batch.count, GEN12_CMD_3DSTATE_PS);
	assert(found >= 0);

	/* vkDestroyPipeline releases the pipeline and its kernels; the modules go next. */
	stub_wire_begin(&fixture_wire);
	fixture_destroy(FIXTURE_DESTROY_PIPELINE, FIXTURE_PIPELINE);
	fixture_destroy(FIXTURE_DESTROY_SHADER_MODULE, FIXTURE_VS);
	fixture_destroy(FIXTURE_DESTROY_SHADER_MODULE, FIXTURE_FS);
	fixture_destroy(FIXTURE_DESTROY_SHADER_MODULE, FIXTURE_BAD_VS);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 4U * 4U);
	pipeline = drv_i915_object_lookup(stub_session, I915_VK_OBJ_PIPELINE, FIXTURE_PIPELINE);
	assert(pipeline == NULL);

	/* Closes the session; nothing stays allocated. */
	stub_session_close();
	assert(stub_live == 0U);
	free(vertex);
	free(fragment);
}

/*
 * A pipeline whose viewport, scissor and blend constants are dynamic keeps
 * them for the command buffer and keeps attachment 0's blend, and one whose
 * fragment shader reads the push constant
 * colour at byte 112 is accepted: its pixel stage is given the push
 * constants in front of its setup data, from the same block the vertex
 * stage reads.
 */
static void
test_dynamic_push_pipeline(void)
{
	struct i915_gfx_pipeline *pipeline;
	struct i915_gfx_kernels kernels;
	struct i915_gfx_batch batch;
	uint32_t commands[256];
	uint32_t *vertex;
	uint32_t *fragment;
	size_t vertex_words;
	size_t fragment_words;
	size_t reply_bytes;
	int found;

	/* Loads the executor test's shaders and opens the fixture session. */
	vertex = fixture_load_spirv(FIXTURE_EXECUTOR_SHADERS, "place.vert.spv", &vertex_words);
	fragment = fixture_load_spirv(FIXTURE_EXECUTOR_SHADERS, "push.frag.spv", &fragment_words);
	stub_session_open(NULL);

	/* Creates the two modules and the pipeline: [65][VK_SUCCESS][count 1][identity]. */
	stub_wire_begin(&fixture_wire);
	fixture_shader_module(vertex, vertex_words, FIXTURE_PLACE_VS);
	fixture_shader_module(fragment, fragment_words, FIXTURE_PUSH_FS);
	fixture_dynamic_pipeline(FIXTURE_PLACE_VS, FIXTURE_PUSH_FS, FIXTURE_DYNAMIC_PIPELINE);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 2U * 24U + 24U);
	assert(stub_get32(stub_reply, 48U + 4U) == VK_SUCCESS);
	assert(stub_get64(stub_reply, 48U + 16U) == FIXTURE_DYNAMIC_PIPELINE);

	/* The pipeline takes its viewport and scissor from the command buffer. */
	pipeline = drv_i915_object_lookup(stub_session, I915_VK_OBJ_PIPELINE, FIXTURE_DYNAMIC_PIPELINE);
	assert(pipeline != NULL);
	assert(pipeline->dynamic_viewport != 0);
	assert(pipeline->dynamic_scissor != 0);

	/* It keeps attachment 0's blend, the components it does not write, and dynamic blend constants. */
	assert(pipeline->blend_enable == 1U);
	assert(pipeline->blend_src_color == VK_BLEND_FACTOR_SRC_ALPHA);
	assert(pipeline->blend_dst_color == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
	assert(pipeline->blend_color_op == VK_BLEND_OP_ADD);
	assert(pipeline->blend_src_alpha == VK_BLEND_FACTOR_ONE);
	assert(pipeline->blend_dst_alpha == VK_BLEND_FACTOR_ONE);
	assert(pipeline->blend_alpha_op == VK_BLEND_OP_ADD);
	assert(pipeline->color_write_disable == VK_COLOR_COMPONENT_B_BIT);
	assert(pipeline->blend_constants[0] == 0x3e800000U);
	assert(pipeline->blend_constants[3] == 0x3f800000U);
	assert(pipeline->dynamic_blend_constants != 0);
	assert(pipeline->binding_count == 1U);
	assert(pipeline->bindings[0].stride == 32U);
	assert(pipeline->attribute_count == 2U);
	assert(pipeline->attributes[1].offset == 16U);

	/*
	 * The vertex stage reads the offset at the start of the block, one
	 * register; the fragment stage reads the colour at byte 112, so it
	 * takes the block's first four registers.
	 */
	assert(pipeline->kernels_ready != 0);
	assert(pipeline->vs_binary->push_regs == 1U);
	assert(pipeline->fs_binary->push_regs == 4U);
	drv_i915_gfx_pipeline_kernels(pipeline, &kernels);
	assert(kernels.vs_push_regs == 1U);
	assert(kernels.ps_push_regs == 4U);

	/* Emits the push constants of both stages and the pixel shader state. */
	memset(commands, 0, sizeof(commands));
	batch.cmds = commands;
	batch.count = 0U;
	batch.capacity = 256U;
	batch.overflow = 0;
	drv_i915_gfx_emit_constants(&batch, 0x123450000ULL, kernels.vs_push_regs, 0x123450400ULL, kernels.ps_push_regs, 0x6U);
	drv_i915_gfx_emit_pixel_shader(&batch, &kernels);
	assert(batch.overflow == 0);

	/* 3DSTATE_CONSTANT_VS reads one register from buffer 3 at the block. */
	found = fixture_find_command(commands, batch.count, GEN12_CMD_3DSTATE_CONSTANT_VS);
	assert(found >= 0);
	assert(commands[found + 2] == (1U << 16));
	assert(commands[found + 9] == 0x23450000U);
	assert(commands[found + 10] == 0x1U);

	/* 3DSTATE_CONSTANT_PS reads four registers from buffer 3 at the pixel stage's own push data. */
	found = fixture_find_command(commands, batch.count, GEN12_CMD_3DSTATE_CONSTANT_PS);
	assert(found >= 0);
	assert(commands[found + 1] == 0U);
	assert(commands[found + 2] == (4U << 16));
	assert(commands[found + 9] == 0x23450400U);
	assert(commands[found + 10] == 0x1U);

	/* 3DSTATE_PS enables the push constants, in front of the setup data at the payload start. */
	found = fixture_find_command(commands, batch.count, GEN12_CMD_3DSTATE_PS);
	assert(found >= 0);
	assert((commands[found + 6] & GEN12_3DSTATE_PS_PUSH_CONSTANT_ENABLE) != 0U);
	assert((commands[found + 7] >> 16) == kernels.ps_grf_start);

	/*
	 * Closes the session with the pipeline (and its kernels) and the
	 * modules alive: the close frees them and nothing stays allocated.
	 */
	stub_session_close();
	assert(stub_live == 0U);
	free(vertex);
	free(fragment);
}

/* Describes a pipeline over two of the generality test's modules, as the draw path sees it before preparing. */
static void
fixture_generality_pipeline(
	struct i915_gfx_pipeline *pipeline,
	struct i915_gfx_shader *vertex,
	struct i915_gfx_shader *fragment,
	const uint32_t *vertex_words,
	size_t vertex_bytes,
	const uint32_t *fragment_words,
	size_t fragment_bytes)
{
	/* The modules borrow the generated words, which the compiler only reads. */
	memset(vertex, 0, sizeof(*vertex));
	memset(fragment, 0, sizeof(*fragment));
	vertex->words = (uint32_t *)(uintptr_t)vertex_words;
	vertex->word_count = (uint32_t)(vertex_bytes / 4U);
	fragment->words = (uint32_t *)(uintptr_t)fragment_words;
	fragment->word_count = (uint32_t)(fragment_bytes / 4U);

	/* The pipeline names both stages; nothing else matters to the kernels. */
	memset(pipeline, 0, sizeof(*pipeline));
	pipeline->vertex = vertex;
	pipeline->fragment = fragment;
}

/*
 * A fragment shader may read only some of the vertex shader's varyings, in
 * any order: vary16.vert writes locations 0 .. 15, subset.frag reads 0, 1,
 * 6, 11 and 15, so 3DSTATE_SBE reads all sixteen slots (eight pairs) and
 * sends five attributes, and 3DSTATE_SBE_SWIZ routes attribute n from the
 * slot of the n-th location read.  A fragment shader reading a location
 * the vertex shader does not write (quad.vert writes location 0 only) is
 * refused.
 */
static void
test_varying_routing(void)
{
	static const uint32_t slots[5] = { 0U, 1U, 6U, 11U, 15U };
	struct i915_gfx_pipeline pipeline;
	struct i915_gfx_shader vertex;
	struct i915_gfx_shader fragment;
	struct i915_gfx_kernels kernels;
	struct i915_gfx_batch batch;
	uint32_t commands[256];
	uint32_t low;
	uint32_t high;
	unsigned index;
	int found;
	int error;

	/* Prepares vary16.vert with subset.frag. */
	fixture_generality_pipeline(&pipeline,
				    &vertex,
				    &fragment,
				    i915_vke2_vary16_vert,
				    sizeof(i915_vke2_vary16_vert),
				    i915_vke2_subset_frag,
				    sizeof(i915_vke2_subset_frag));
	error = drv_i915_gfx_pipeline_prepare(NULL, &pipeline);
	assert(error == 0);
	assert(pipeline.vs_binary->varying_count == 16U);
	assert(pipeline.fs_binary->input_count == 5U);

	/* The kernels carry the route: input n from the slot of its location. */
	drv_i915_gfx_pipeline_kernels(&pipeline, &kernels);
	assert(kernels.varyings == 16U);
	assert(kernels.ps_inputs_mapped != 0U);
	assert(kernels.ps_input_count == 5U);
	for (index = 0U; index < 5U; index++)
		assert(kernels.ps_input_slots[index] == slots[index]);

	/* Emits the pixel shader state. */
	memset(commands, 0, sizeof(commands));
	batch.cmds = commands;
	batch.count = 0U;
	batch.capacity = 256U;
	batch.overflow = 0;
	drv_i915_gfx_emit_pixel_shader(&batch, &kernels);
	assert(batch.overflow == 0);

	/* 3DSTATE_SBE: five attributes, eight pairs of slots read from slot 2 on. */
	found = fixture_find_command(commands, batch.count, GEN12_CMD_3DSTATE_SBE);
	assert(found >= 0);
	assert(((commands[found + 1] >> 22) & 0x3fU) == 5U);
	assert(((commands[found + 1] >> 11) & 0x1fU) == 8U);
	assert(((commands[found + 1] >> 5) & 0x3fU) == 1U);

	/* 3DSTATE_SBE_SWIZ: the five sources, two to a dword, then slot 0 for the inputs not read. */
	found = fixture_find_command(commands, batch.count, GEN12_CMD_3DSTATE_SBE_SWIZ);
	assert(found >= 0);
	for (index = 0U; index < 16U; index += 2U) {
		low = commands[found + 1 + index / 2U] & 0xffffU;
		high = commands[found + 1 + index / 2U] >> 16;
		assert(low == (index < 5U ? slots[index] : 0U));
		assert(high == (index + 1U < 5U ? slots[index + 1U] : 0U));
	}

	/* 3DSTATE_PS_EXTRA: the kernel reads attributes. */
	found = fixture_find_command(commands, batch.count, GEN12_CMD_3DSTATE_PS_EXTRA);
	assert(found >= 0);
	assert((commands[found + 1] & (1U << 8)) != 0U);
	drv_i915_gfx_pipeline_release(&pipeline);

	/* quad.vert writes location 0 only: subset.frag's other four inputs have no source. */
	fixture_generality_pipeline(&pipeline,
				    &vertex,
				    &fragment,
				    i915_vke2_quad_vert,
				    sizeof(i915_vke2_quad_vert),
				    i915_vke2_subset_frag,
				    sizeof(i915_vke2_subset_frag));
	error = drv_i915_gfx_pipeline_prepare(NULL, &pipeline);
	assert(error == ENOTSUP);
	assert(pipeline.kernels_ready == 0 && pipeline.vs_binary == NULL && pipeline.fs_binary == NULL);

	/* vin16.vert writes locations 0 and 3; vin16.frag reads them from slots 0 and 1. */
	fixture_generality_pipeline(&pipeline,
				    &vertex,
				    &fragment,
				    i915_vke2_vin16_vert,
				    sizeof(i915_vke2_vin16_vert),
				    i915_vke2_vin16_frag,
				    sizeof(i915_vke2_vin16_frag));
	error = drv_i915_gfx_pipeline_prepare(NULL, &pipeline);
	assert(error == 0);
	drv_i915_gfx_pipeline_kernels(&pipeline, &kernels);
	assert(kernels.vs_input_count == 16U && kernels.varyings == 2U);
	assert(kernels.ps_input_count == 2U && kernels.ps_input_slots[0] == 0U && kernels.ps_input_slots[1] == 1U);

	/* Kernels that spill nothing leave dwords 4-5 of 3DSTATE_VS and PS (the scratch space) zero. */
	assert(kernels.vs_scratch_bytes == 0U && kernels.ps_scratch_bytes == 0U);
	memset(commands, 0, sizeof(commands));
	batch.count = 0U;
	batch.overflow = 0;
	drv_i915_gfx_emit_vertex_shader(&batch, &kernels);
	drv_i915_gfx_emit_pixel_shader(&batch, &kernels);
	found = fixture_find_command(commands, batch.count, GEN12_CMD_3DSTATE_VS);
	assert(found >= 0 && commands[found + 4] == 0U && commands[found + 5] == 0U);
	found = fixture_find_command(commands, batch.count, GEN12_CMD_3DSTATE_PS);
	assert(found >= 0 && commands[found + 4] == 0U && commands[found + 5] == 0U);
	drv_i915_gfx_pipeline_release(&pipeline);

	/*
	 * vio16.vert (16 attributes, 16 varyings) with spill.frag: both kernels spill, 2 KiB a thread each; their
	 * parts of the scratch buffer land in dwords 4-5 as the Scratch Space Base Pointer (bits 63:10, an offset
	 * from the general state base) and the Per-Thread Scratch Space (1 KiB << 1); STATE_BASE_ADDRESS puts
	 * the general state base at the buffer.
	 */
	fixture_generality_pipeline(&pipeline,
				    &vertex,
				    &fragment,
				    i915_vke2_vio16_vert,
				    sizeof(i915_vke2_vio16_vert),
				    i915_vke2_spill_frag,
				    sizeof(i915_vke2_spill_frag));
	error = drv_i915_gfx_pipeline_prepare(NULL, &pipeline);
	assert(error == 0);
	drv_i915_gfx_pipeline_kernels(&pipeline, &kernels);
	assert(kernels.vs_input_count == 16U && kernels.varyings == 16U);
	assert(kernels.vs_scratch_bytes == 2048U && kernels.ps_scratch_bytes == 2048U);
	kernels.scratch_base = 0x123450000ULL;
	kernels.vs_scratch_offset = 0x1000ULL;
	kernels.ps_scratch_offset = 0x00678400ULL;
	memset(commands, 0, sizeof(commands));
	batch.count = 0U;
	batch.overflow = 0;
	drv_i915_gfx_emit_vertex_shader(&batch, &kernels);
	drv_i915_gfx_emit_pixel_shader(&batch, &kernels);
	assert(batch.overflow == 0);
	found = fixture_find_command(commands, batch.count, GEN12_CMD_3DSTATE_VS);
	assert(found >= 0 && commands[found + 4] == (0x1000U | 1U) && commands[found + 5] == 0U);
	found = fixture_find_command(commands, batch.count, GEN12_CMD_3DSTATE_PS);
	assert(found >= 0 && commands[found + 4] == (0x00678400U | 1U) && commands[found + 5] == 0U);
	memset(commands, 0, sizeof(commands));
	batch.count = 0U;
	batch.overflow = 0;
	drv_i915_gfx_emit_context_setup(&batch, 0x200000ULL, 0x300000ULL, kernels.scratch_base, 3U << 1);
	found = fixture_find_command(commands, batch.count, GEN12_CMD_STATE_BASE_ADDRESS);
	assert(found >= 0 && commands[found + 1] == (1U | ((3U << 1) << 4) | 0x23450000U) && commands[found + 2] == 1U);
	drv_i915_gfx_pipeline_release(&pipeline);
	printf("  varyings: 5 of 16 routed by location through SBE_SWIZ; an unwritten location refused; 16 attributes; "
	       "16 attributes + 16 varyings and spill.frag spill 2 KiB a thread into the scratch fields of 3DSTATE_VS / PS\n");
}
