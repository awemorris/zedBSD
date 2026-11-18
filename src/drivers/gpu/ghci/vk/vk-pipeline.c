/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The pipelines of the Vulkan executor: shader modules, pipeline caches,
 * and graphics and compute pipelines.
 *
 * A pipeline is compiled at its create: the vertex program first, then
 * the fragment program with the vertex program's tables (the same
 * binding takes the same slot and entry), or the driver's own fragment
 * program for a pipeline without a fragment stage.  The code goes into
 * the device's executor heap (mapped executable) after the 128-byte
 * pipeline state record (section 8.2 of the specification), which is
 * baked then; the driver's table of set 3 (the scratch memory's
 * addresses) follows the code.  What the record does not keep (the
 * stencil masks and reference, the depth bias, the blend constants, the
 * blend of each attachment, the viewport and scissor) is kept for the
 * executor to set at each bind.
 *
 * A pipeline the hardware cannot run is refused at its create with
 * VK_ERROR_FEATURE_NOT_PRESENT and a line in the log: more texture or
 * sampler slots than sixteen over its stages,
 * a resource table past its size, attributes past sixty registers (the
 * compiler's refusals), more colour attachments than MAX_MRT, a blend
 * factor of the second colour without dual-source blending, a logic op
 * without FEATURE[11], attachments blended differently without
 * MRT_BLEND, shared memory past LDS_KB, a workgroup past the limits the
 * device reports, a sample count query or four samples without
 * multisampling, a vertex attribute format the device does not fetch,
 * and the features Vulkan 1.0 lets the device lack.  One whose scratch
 * memory or code does not fit the executor's heap is refused with
 * VK_ERROR_OUT_OF_DEVICE_MEMORY, and one the host has no memory for with
 * VK_ERROR_OUT_OF_HOST_MEMORY, so that a caller can tell what freeing
 * memory may help from what it cannot.
 */

#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "vk.h"
#include "../compiler/ghci-isa.h"

#include "drivers/gpu/i915/vk/vk-codec.inc"

/* The most pipelines one create makes, the most SPIR-V words of a module, and the most specialization entries. */
#define PIPELINE_BATCH_MAX		64U
#define PIPELINE_MODULE_WORDS_MAX	(1U << 20)
#define PIPELINE_SPECIALIZATIONS_MAX	256U

/* The stages a graphics pipeline runs: the vertex and the fragment. */
#define PIPELINE_STAGES			2U
#define PIPELINE_VERTEX			0U
#define PIPELINE_FRAGMENT		1U

/* The bind points as the record keeps them. */
#define PIPELINE_GRAPHICS		((uint32_t)VK_PIPELINE_BIND_POINT_GRAPHICS)
#define PIPELINE_COMPUTE		((uint32_t)VK_PIPELINE_BIND_POINT_COMPUTE)

/* The alignment of the pipeline state record, of each program's code and of the scratch memory. */
#define PIPELINE_PSO_ALIGN		128U
#define PIPELINE_CODE_ALIGN		64U

/* The hardware's topology of a triangle list, which a fan is drawn as. */
#define PIPELINE_TOPOLOGY_TRIANGLES	3U

/* The line width of the record (8-bit fixed point, 16 = 1.0) and of SET_DYNAMIC_STATE (an IEEE single). */
#define PIPELINE_LINE_WIDTH_ONE		16U
#define PIPELINE_FLOAT_ONE		0x3f800000U

/* All four samples of the record's sample mask. */
#define PIPELINE_SAMPLE_MASK_ALL	0xfU

/* The scratch memory's warps: RESIDENT_WARPS[15:0]. */
#define PIPELINE_WARPS_MASK		0xffffU

/* The four samples a multisampled pipeline has. */
#define PIPELINE_SAMPLES_MSAA		4U

/* The header of a pipeline cache's data (VkPipelineCacheHeaderVersionOne): 32 bytes, version 1. */
#define PIPELINE_CACHE_HEADER_BYTES	32U
#define PIPELINE_CACHE_VERSION		1U

/*
 * SPIR-V: the first word of the instructions and the word of the id
 * bound, an instruction's opcode and word count, and the opcodes,
 * decorations and execution modes a specialization rewrites.
 *
 * Values referenced: SPIR-V 1.0 specification (section 2.3, the physical
 * layout; section 3.32, OpNop 0, OpExecutionMode 16, OpTypeBool 20,
 * OpTypeInt 21, OpConstantTrue 41 to OpConstantComposite 44,
 * OpSpecConstantTrue 48 to OpSpecConstantOp 52, OpDecorate 71; section
 * 3.20, the SpecId decoration 1 and BuiltIn 11; section 3.21, the
 * WorkgroupSize built-in 25; section 3.6, the LocalSize execution mode
 * 17; SPIR-V 1.2, OpExecutionModeId 331 and LocalSizeId 38).
 */
#define SPIRV_HEADER_WORDS		5U
#define SPIRV_BOUND_WORD		3U
#define SPIRV_OPCODE_MASK		0xffffU
#define SPIRV_COUNT_SHIFT		16U
#define SPIRV_OP_NOP			0U
#define SPIRV_OP_EXECUTION_MODE		16U
#define SPIRV_OP_TYPE_BOOL		20U
#define SPIRV_OP_TYPE_INT		21U
#define SPIRV_OP_CONSTANT_TRUE		41U
#define SPIRV_OP_CONSTANT_FALSE		42U
#define SPIRV_OP_CONSTANT		43U
#define SPIRV_OP_CONSTANT_COMPOSITE	44U
#define SPIRV_OP_SPEC_TRUE		48U
#define SPIRV_OP_SPEC_FALSE		49U
#define SPIRV_OP_SPEC_CONSTANT		50U
#define SPIRV_OP_SPEC_COMPOSITE		51U
#define SPIRV_OP_SPEC_OP		52U
#define SPIRV_OP_DECORATE		71U
#define SPIRV_OP_EXECUTION_MODE_ID	331U
#define SPIRV_DECORATION_SPEC_ID	1U
#define SPIRV_DECORATION_BUILTIN	11U
#define SPIRV_BUILTIN_WORKGROUP_SIZE	25U
#define SPIRV_MODE_LOCAL_SIZE		17U
#define SPIRV_MODE_LOCAL_SIZE_ID	38U
#define SPIRV_NOP_WORD			(1U << SPIRV_COUNT_SHIFT)

/*
 * The operations of OpSpecConstantOp the driver folds on 32-bit integers
 * and Booleans.
 *
 * Values referenced: SPIR-V 1.0 specification (section 3.32: OpUConvert
 * 113, OpSConvert 114, OpSNegate 126, OpIAdd 128, OpISub 130, OpIMul
 * 132, OpUDiv 134, OpSDiv 135, OpUMod 137, OpSRem 138, OpSMod 139,
 * OpLogicalEqual 164 to OpSLessThanEqual 179, OpShiftRightLogical 194 to
 * OpNot 200).
 */
#define SPIRV_OP_U_CONVERT		113U
#define SPIRV_OP_S_CONVERT		114U
#define SPIRV_OP_S_NEGATE		126U
#define SPIRV_OP_I_ADD			128U
#define SPIRV_OP_I_SUB			130U
#define SPIRV_OP_I_MUL			132U
#define SPIRV_OP_U_DIV			134U
#define SPIRV_OP_S_DIV			135U
#define SPIRV_OP_U_MOD			137U
#define SPIRV_OP_S_REM			138U
#define SPIRV_OP_S_MOD			139U
#define SPIRV_OP_LOGICAL_EQUAL		164U
#define SPIRV_OP_LOGICAL_NOT_EQUAL	165U
#define SPIRV_OP_LOGICAL_OR		166U
#define SPIRV_OP_LOGICAL_AND		167U
#define SPIRV_OP_LOGICAL_NOT		168U
#define SPIRV_OP_SELECT			169U
#define SPIRV_OP_I_EQUAL		170U
#define SPIRV_OP_I_NOT_EQUAL		171U
#define SPIRV_OP_U_GREATER		172U
#define SPIRV_OP_S_GREATER		173U
#define SPIRV_OP_U_GREATER_EQUAL	174U
#define SPIRV_OP_S_GREATER_EQUAL	175U
#define SPIRV_OP_U_LESS			176U
#define SPIRV_OP_S_LESS			177U
#define SPIRV_OP_U_LESS_EQUAL		178U
#define SPIRV_OP_S_LESS_EQUAL		179U
#define SPIRV_OP_SHIFT_RIGHT_LOGICAL	194U
#define SPIRV_OP_SHIFT_RIGHT_ARITHMETIC	195U
#define SPIRV_OP_SHIFT_LEFT_LOGICAL	196U
#define SPIRV_OP_BITWISE_OR		197U
#define SPIRV_OP_BITWISE_XOR		198U
#define SPIRV_OP_BITWISE_AND		199U
#define SPIRV_OP_NOT			200U

/* The most operands of a folded operation (OpSelect's three), and the bits of a 32-bit integer. */
#define SPEC_OPERANDS_MAX		3U
#define SPEC_INT_BITS			32U
#define SPEC_INT_SIGN			0x80000000U

/* What an id of a module being specialized is: a 32-bit integer or Boolean type or constant, or a constant composite. */
#define SPEC_UNKNOWN			0U
#define SPEC_TYPE_INT			1U
#define SPEC_TYPE_BOOL			2U
#define SPEC_INT			3U
#define SPEC_BOOL			4U
#define SPEC_COMPOSITE			5U

/* What a pipeline's create is refused with when the device cannot run it (not for want of memory). */
#define PIPELINE_REFUSED		((uint32_t)VK_ERROR_FEATURE_NOT_PRESENT)

/* One stage of a pipeline's create: the stage, the module, and a copy of its specialization. */
struct stage_request {
	uint32_t stage;
	uint64_t module;
	uint32_t map_count;
	VkSpecializationMapEntry *map;
	uint32_t data_bytes;
	uint8_t *data;
};

/* One vertex attribute of a pipeline's create. */
struct attribute_request {
	uint32_t location;
	uint32_t binding;
	uint32_t format;
	uint32_t offset;
};

/*
 * One pipeline's create, decoded out of the command's scratch: its
 * stages, its fixed-function state and the objects it names.  refusal is
 * the reason a pipeline the hardware cannot run is refused (NULL for
 * none).
 */
struct pipeline_request {
	uint32_t bind_point;
	uint32_t stage_count;
	struct stage_request stages[PIPELINE_STAGES];
	const char *refusal;
	uint32_t binding_mask;
	uint32_t strides[GHCI_VERTEX_BINDINGS];
	uint32_t per_instance;
	uint32_t attribute_count;
	struct attribute_request attributes[GHCI_VERTEX_ATTRIBUTES];
	uint32_t topology;
	uint32_t primitive_restart;
	VkPipelineRasterizationStateCreateInfo raster;
	uint32_t multisample;
	VkPipelineMultisampleStateCreateInfo samples;
	uint32_t sample_mask;
	uint32_t depth_stencil;
	VkPipelineDepthStencilStateCreateInfo depth;
	uint32_t blend;
	VkPipelineColorBlendStateCreateInfo blend_state;
	VkPipelineColorBlendAttachmentState attachments[GHCI_VK_COLOR_ATTACHMENTS];
	uint32_t viewport_given;
	VkViewport viewport;
	uint32_t scissor_given;
	VkRect2D scissor;
	uint32_t dynamic;
	uint64_t layout;
	uint64_t render_pass;
	uint32_t subpass;
};

/* The programs of a pipeline while it is made. */
struct pipeline_programs {
	struct ghci_gsi1_program *program[PIPELINE_STAGES];
};

/*
 * One id of a module while it is specialized: what it is (SPEC_*) and
 * its value (a constant's, or the word where a composite starts).
 */
struct spec_value {
	uint32_t kind;
	uint32_t value;
};

/*
 * A module's words while they are specialized: the copy, the table of
 * its SpecId decorations (each id, then its SpecId), the ids below the
 * bound the table of values follows (NULL when nothing needs it), the id
 * the WorkgroupSize built-in decorates (0 for none), and the reason a
 * specialization is refused.
 */
struct spec_module {
	uint32_t *words;
	uint32_t word_count;
	uint32_t *ids;
	uint32_t *spec_ids;
	uint32_t spec_count;
	struct spec_value *values;
	uint32_t value_count;
	uint32_t workgroup;
	const char *reason;
};

static int create_module(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int create_cache(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int cache_data(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int merge_caches(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int create_pipelines(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, uint32_t bind_point);
static int decode_graphics(struct ghci_vk_session *session, struct i915_wire_reader *reader, struct pipeline_request *request);
static int decode_stage(struct ghci_vk_session *session, const VkPipelineShaderStageCreateInfo *info, struct pipeline_request *request);
static int decode_vertex_input(struct ghci_vk_session *session, const VkPipelineVertexInputStateCreateInfo *info, struct pipeline_request *request);
static int decode_blend(struct ghci_vk_session *session, const VkPipelineColorBlendStateCreateInfo *info, struct pipeline_request *request);
static int decode_compute(struct ghci_vk_session *session, struct i915_wire_reader *reader, struct pipeline_request *request);
static void request_free(struct pipeline_request *request);
static uint32_t pipeline_build(struct ghci_vk_session *session, struct ghci_vk_device *device, const struct pipeline_request *request, struct ghci_vk_pipeline **result);
static void pipeline_free(struct ghci_vk_session *session, struct ghci_vk_pipeline *pipeline);
static uint32_t graphics_check(struct ghci_vk_session *session, const struct pipeline_request *request, const struct ghci_vk_subpass *subpass);
static uint32_t pipeline_compile(struct ghci_vk_session *session, struct ghci_vk_device *device, const struct pipeline_request *request, const struct ghci_vk_pipeline_layout *layout, const struct ghci_vk_subpass *subpass, struct pipeline_programs *programs);
static uint32_t pipeline_compile_with(struct ghci_vk_session *session, struct ghci_vk_device *device, const struct pipeline_request *request, const struct ghci_vk_pipeline_layout *layout, const struct ghci_vk_subpass *subpass, struct ghci_gsi1_options *options, struct pipeline_programs *programs);
static uint32_t compile_stage(struct ghci_vk_session *session, const struct stage_request *stage, enum drv_gpu_shader_stage kind, const struct ghci_gsi1_options *options, struct ghci_gsi1_program **program);
static uint32_t programs_check(struct ghci_vk_session *session, const struct pipeline_request *request, const struct ghci_vk_pipeline_layout *layout, const struct pipeline_programs *programs);
static uint32_t pipeline_place(struct ghci_vk_session *session, struct ghci_vk_device *device, const struct pipeline_programs *programs, struct ghci_vk_pipeline *pipeline);
static void pipeline_pso(const struct pipeline_request *request, const struct pipeline_programs *programs, struct ghci_pso *pso);
static void pipeline_state(struct ghci_vk_session *session, const struct pipeline_request *request, struct ghci_vk_pipeline *pipeline);
static uint32_t blend_word(const VkPipelineColorBlendAttachmentState *attachment);
static uint32_t blend_word_used(const VkPipelineColorBlendAttachmentState *attachment);
static int subpass_of(struct ghci_vk_session *session, const struct pipeline_request *request, struct ghci_vk_subpass *subpass);
static int specialize(const struct ghci_vk_shader_module *module, const struct stage_request *stage, uint32_t **words, const char **reason);
static int specialize_scan(struct spec_module *spec);
static int specialize_constant(struct spec_module *spec, uint32_t at, uint32_t count, const struct stage_request *stage);
static int specialize_track(struct spec_module *spec, uint32_t at, uint32_t count);
static int specialize_fold(struct spec_module *spec, uint32_t at, uint32_t count);
static int fold_value(uint32_t operation, uint32_t operands, const uint32_t *kinds, const uint32_t *values, uint32_t *kind, uint32_t *value);
static int fold_signed(uint32_t operation, uint32_t left, uint32_t right, uint32_t *value);
static int specialize_workgroup(struct spec_module *spec);
static uint32_t pipeline_refuse(struct ghci_vk_session *session, const char *reason);
static int destroy_object(struct ghci_vk_session *session, enum ghci_vk_kind kind, struct i915_wire_reader *reader);
static struct ghci_vk_device *device_of(struct ghci_vk_session *session, uint64_t handle);
static uint32_t align_up(uint32_t value, uint32_t granule);
static void store64(uint8_t *out, uint64_t value);
static uint32_t load32(const uint8_t *in);

/*
 * Runs a shader module, pipeline cache or pipeline command; *handled
 * stays clear for another group's number.
 */
int
ghci_vk_pipeline_route(
	struct ghci_vk_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	/* Picks the command, or leaves it to another group. */
	*handled = 1;
	switch (opcode) {
	case GPU_OP_CREATE_SHADER_MODULE:
		error = create_module(session, requested, reader, reply);
		break;
	case GPU_OP_DESTROY_SHADER_MODULE:
		error = destroy_object(session, GHCI_VK_SHADER_MODULE, reader);
		break;
	case GPU_OP_CREATE_PIPELINE_CACHE:
		error = create_cache(session, requested, reader, reply);
		break;
	case GPU_OP_DESTROY_PIPELINE_CACHE:
		error = destroy_object(session, GHCI_VK_PIPELINE_CACHE, reader);
		break;
	case GPU_OP_GET_PIPELINE_CACHE_DATA:
		error = cache_data(session, requested, reader, reply);
		break;
	case GPU_OP_MERGE_PIPELINE_CACHES:
		error = merge_caches(session, requested, reader, reply);
		break;
	case GPU_OP_CREATE_GRAPHICS_PIPELINES:
		error = create_pipelines(session, requested, reader, reply, PIPELINE_GRAPHICS);
		break;
	case GPU_OP_CREATE_COMPUTE_PIPELINES:
		error = create_pipelines(session, requested, reader, reply, PIPELINE_COMPUTE);
		break;
	case GPU_OP_DESTROY_PIPELINE:
		error = destroy_object(session, GHCI_VK_PIPELINE, reader);
		break;
	default:
		*handled = 0;
		return 0;
	}

	/* A command that could not be decoded or run. */
	if (error != 0)
		return error;

	/* Succeeded: the command ran. */
	return 0;
}

/*
 * Lets a shader module or a pipeline go: the module's words; the
 * pipeline's runs of the executor heap (once the work placed until now
 * has passed) and its record.
 */
void
ghci_vk_pipeline_release(
	struct ghci_vk_session *session,
	struct ghci_vk_object *object)
{
	struct ghci_vk_pipeline *pipeline;

	/* What each kind holds. */
	(void)session;
	switch (object->kind) {
	case GHCI_VK_SHADER_MODULE:
		kern_free(object->as.module.words);
		object->as.module.words = NULL;
		break;
	case GHCI_VK_PIPELINE:
		pipeline = object->as.pipeline;
		if (pipeline == NULL)
			break;

		/* The record and code and the scratch memory once the work placed until now has passed, then the record of the kernel. */
		ghci_vk_heap_retire(object->device, pipeline->block, pipeline->offset, NULL);
		ghci_vk_heap_retire(object->device, pipeline->scratch_block, pipeline->scratch_offset, NULL);
		kern_free(pipeline);
		object->as.pipeline = NULL;
		break;
	default:
		break;
	}
}

/* vkCreateShaderModule: the create form with VkShaderModuleCreateInfo; the words are copied. */
static int
create_module(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkShaderModuleCreateInfo info;
	struct ghci_vk_device *device;
	struct ghci_vk_object *object;
	uint64_t handle;
	uint64_t present;
	uint64_t identity;
	uint64_t decoded;
	uint32_t *words;
	uint32_t count;
	int error;

	/* The device, the create info and the tail. */
	kern_memset(&info, 0, sizeof(info));
	handle = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present == 1U)
		i915_vkc_dec_VkShaderModuleCreateInfo(reader, &session->arena, &info);
	identity = ghci_vk_create_tail(reader);
	if (reader->error != 0 || present != 1U)
		return EINVAL;
	device = device_of(session, handle);
	if (device == NULL)
		return EINVAL;

	/* Whole words, the header at least, a bounded module, and all its words decoded. */
	if (info.pCode == NULL || (info.codeSize & 3U) != 0U || info.codeSize < SPIRV_HEADER_WORDS * 4U)
		return EINVAL;
	if (info.codeSize > PIPELINE_MODULE_WORDS_MAX * 4U)
		return EINVAL;
	count = (uint32_t)(info.codeSize / 4U);
	decoded = ghci_vk_decoded_count(session, info.pCode);
	if (decoded != count)
		return EINVAL;

	/* Its bytes, within the session's budget. */
	error = ghci_vk_budget_take(session, (uint64_t)count * 4U);
	if (error != 0) {
		ghci_vk_reply_created(reply, requested, (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY, identity);
		return 0;
	}

	/* A copy of the words, which outlive the command's scratch. */
	words = kern_malloc((size_t)count * 4U);
	if (words == NULL) {
		ghci_vk_budget_give(session, (uint64_t)count * 4U);
		return ENOMEM;
	}
	kern_memcpy(words, info.pCode, (size_t)count * 4U);

	/* The object. */
	error = ghci_vk_object_add(session, GHCI_VK_SHADER_MODULE, identity, device, &object);
	if (error != 0) {
		ghci_vk_budget_give(session, (uint64_t)count * 4U);
		kern_free(words);
		return error;
	}
	object->as.module.words = words;
	object->as.module.word_count = count;
	object->charged = (uint64_t)count * 4U;

	/* The result and the identity. */
	ghci_vk_reply_created(reply, requested, (uint32_t)VK_SUCCESS, identity);

	/* Succeeded: the module exists. */
	return 0;
}

/* vkCreatePipelineCache: the create form with VkPipelineCacheCreateInfo; a cache keeps nothing. */
static int
create_cache(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkPipelineCacheCreateInfo info;
	struct ghci_vk_device *device;
	struct ghci_vk_object *object;
	uint64_t handle;
	uint64_t present;
	uint64_t identity;
	int error;

	/* The device, the create info (its initial data is not used) and the tail. */
	kern_memset(&info, 0, sizeof(info));
	handle = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present == 1U)
		i915_vkc_dec_VkPipelineCacheCreateInfo(reader, &session->arena, &info);
	identity = ghci_vk_create_tail(reader);
	if (reader->error != 0 || present != 1U)
		return EINVAL;
	device = device_of(session, handle);
	if (device == NULL)
		return EINVAL;

	/* The object. */
	error = ghci_vk_object_add(session, GHCI_VK_PIPELINE_CACHE, identity, device, &object);
	if (error != 0)
		return error;

	/* The result and the identity. */
	ghci_vk_reply_created(reply, requested, (uint32_t)VK_SUCCESS, identity);

	/* Succeeded: the cache exists. */
	return 0;
}

/*
 * vkGetPipelineCacheData: [device][cache][u64 1][u64 capacity]
 * [u64 capacity] -> [result][u64 1][u64 size][u64 count][bytes].  The
 * data is the 32-byte header alone; a capacity of 0 asks for its size, a
 * capacity below it gets nothing and VK_INCOMPLETE.
 */
static int
cache_data(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	uint8_t header[PIPELINE_CACHE_HEADER_BYTES];
	struct ghci_vk_object *object;
	struct ghci_device *device;
	uint64_t cache;
	uint64_t present;
	uint64_t capacity;
	uint64_t count;
	uint32_t word;

	/* The device, the cache and the capacity. */
	(void)drv_i915_wire_read_u64(reader);
	cache = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	capacity = drv_i915_wire_read_u64(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || present != 1U || count != capacity)
		return EINVAL;
	object = ghci_vk_object_find(session, GHCI_VK_PIPELINE_CACHE, cache);
	if (object == NULL)
		return EINVAL;

	/*
	 * The header: its bytes, its version, the vendor and device IDs, and
	 * the pipelineCacheUUID the device reports.
	 *
	 * Values referenced: Vulkan 1.0 VkPipelineCacheHeaderVersionOne.
	 */
	device = session->device;
	word = PIPELINE_CACHE_HEADER_BYTES;
	kern_memcpy(&header[0], &word, 4U);
	word = PIPELINE_CACHE_VERSION;
	kern_memcpy(&header[4], &word, 4U);
	word = device->pci_vendor;
	kern_memcpy(&header[8], &word, 4U);
	word = device->pci_device;
	kern_memcpy(&header[12], &word, 4U);
	kern_memcpy(&header[16], GHCI_VK_CACHE_UUID, VK_UUID_SIZE);

	/* Nothing asked for. */
	if (requested == 0U)
		return 0;

	/* The size alone, nothing for a capacity too small, or the header. */
	if (capacity == 0U) {
		drv_i915_wire_reply_u32(reply, (uint32_t)VK_SUCCESS);
		drv_i915_wire_reply_u64(reply, 1U);
		drv_i915_wire_reply_u64(reply, PIPELINE_CACHE_HEADER_BYTES);
		drv_i915_wire_reply_u64(reply, 0U);
	} else if (capacity < PIPELINE_CACHE_HEADER_BYTES) {
		drv_i915_wire_reply_u32(reply, (uint32_t)VK_INCOMPLETE);
		drv_i915_wire_reply_u64(reply, 1U);
		drv_i915_wire_reply_u64(reply, 0U);
		drv_i915_wire_reply_u64(reply, 0U);
	} else {
		drv_i915_wire_reply_u32(reply, (uint32_t)VK_SUCCESS);
		drv_i915_wire_reply_u64(reply, 1U);
		drv_i915_wire_reply_u64(reply, PIPELINE_CACHE_HEADER_BYTES);
		drv_i915_wire_reply_u64(reply, PIPELINE_CACHE_HEADER_BYTES);
		drv_i915_wire_reply_bytes(reply, header, PIPELINE_CACHE_HEADER_BYTES);
	}

	/* Succeeded: the reply holds the data. */
	return 0;
}

/* vkMergePipelineCaches: [device][cache][u32 n][u64 n][caches] -> [result]; caches keep nothing to merge. */
static int
merge_caches(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	const uint8_t *sources;
	struct ghci_vk_object *object;
	uint64_t source;
	uint64_t cache;
	uint64_t count;
	uint64_t index;

	/* The device, the destination and the sources, all caches of the session. */
	(void)drv_i915_wire_read_u64(reader);
	cache = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = ghci_vk_read_count(reader);
	sources = drv_i915_wire_read_array(reader, (size_t)count, sizeof(uint64_t));
	if (reader->error != 0 || (count != 0U && sources == NULL))
		return EINVAL;
	object = ghci_vk_object_find(session, GHCI_VK_PIPELINE_CACHE, cache);
	if (object == NULL)
		return EINVAL;
	for (index = 0U; index < count; index++) {
		kern_memcpy(&source, sources + index * sizeof(source), sizeof(source));
		object = ghci_vk_object_find(session, GHCI_VK_PIPELINE_CACHE, source);
		if (object == NULL)
			return EINVAL;
	}

	/* The result. */
	ghci_vk_reply_result(reply, requested, (uint32_t)VK_SUCCESS);

	/* Succeeded: the caches are merged. */
	return 0;
}

/*
 * vkCreateGraphicsPipelines and vkCreateComputePipelines: [device][cache]
 * [u32 n][u64 n]{create info}[u64 0][u64 n][identities] -> [result]
 * [u64 n][identities, 0 for one not made].  Each pipeline is made on its
 * own; the result is the first failure's, and the pipelines made are
 * kept.  A record that does not decode, or a pipeline that cannot be
 * kept as an object of the session (an identity already used), fails the
 * command with none of its pipelines made.
 */
static int
create_pipelines(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	uint32_t bind_point)
{
	struct pipeline_request *requests[PIPELINE_BATCH_MAX];
	uint64_t identities[PIPELINE_BATCH_MAX];
	struct ghci_vk_pipeline *pipeline;
	uint64_t kept;
	struct ghci_vk_device *device;
	struct ghci_vk_object *object;
	uint64_t handle;
	uint64_t count;
	uint64_t listed;
	uint64_t index;
	uint32_t result;
	uint32_t made;
	int error;

	/* The device, the cache and the count. */
	handle = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count == 0U || count > PIPELINE_BATCH_MAX)
		return EINVAL;
	device = device_of(session, handle);
	if (device == NULL)
		return EINVAL;

	/* Each record, decoded out of a scratch emptied for it. */
	kern_memset(requests, 0, sizeof(requests));
	error = 0;
	for (index = 0U; index < count; index++) {
		requests[index] = kern_calloc(1U, sizeof(*requests[index]));
		if (requests[index] == NULL) {
			error = ENOMEM;
			break;
		}
		requests[index]->bind_point = bind_point;
		session->arena.used = 0U;
		if (bind_point == PIPELINE_GRAPHICS)
			error = decode_graphics(session, reader, requests[index]);
		else
			error = decode_compute(session, reader, requests[index]);
		if (error != 0)
			break;
	}

	/* The allocator and the identities, one per pipeline. */
	if (error == 0) {
		(void)drv_i915_wire_read_u64(reader);
		listed = drv_i915_wire_read_u64(reader);
		if (listed != count)
			reader->error = 1;
		for (index = 0U; index < count; index++)
			identities[index] = drv_i915_wire_read_u64(reader);
		if (reader->error != 0)
			error = EINVAL;
	}

	/* Each pipeline on its own; the first failure is the result. */
	result = (uint32_t)VK_SUCCESS;
	for (index = 0U; index < count && error == 0; index++) {
		pipeline = NULL;
		made = pipeline_build(session, device, requests[index], &pipeline);
		if (made == (uint32_t)VK_SUCCESS) {
			error = ghci_vk_object_add(session, GHCI_VK_PIPELINE, identities[index], device, &object);
			if (error != 0) {
				pipeline_free(session, pipeline);
				break;
			}
			object->as.pipeline = pipeline;
			object->charged = pipeline->charged;
		} else {
			identities[index] = 0U;
			if (result == (uint32_t)VK_SUCCESS)
				result = made;
		}
	}

	/* A pipeline that could not be kept takes the ones made before it with it. */
	for (kept = 0U; error != 0 && kept < index && kept < count; kept++) {
		if (identities[kept] == 0U)
			continue;
		object = ghci_vk_object_find(session, GHCI_VK_PIPELINE, identities[kept]);
		if (object != NULL)
			ghci_vk_object_destroy(session, object);
	}

	/* The decoded records go. */
	for (index = 0U; index < count; index++)
		request_free(requests[index]);
	if (error != 0)
		return error;

	/* The result, the count, and each identity (0 for one not made). */
	if (requested != 0U) {
		drv_i915_wire_reply_u32(reply, result);
		drv_i915_wire_reply_u64(reply, count);
		for (index = 0U; index < count; index++)
			drv_i915_wire_reply_u64(reply, identities[index]);
	}

	/* Succeeded: the pipelines exist, or the reply says which do not. */
	return 0;
}

/*
 * Decodes a graphics pipeline's record as libvulkan encodes it: [sType]
 * [u64 0][flags][u32 n][u64 n]{stage}, then each state behind its
 * presence word (vertex input, input assembly, tessellation, viewport,
 * rasterization, multisample, depth and stencil, colour blend, dynamic),
 * then [layout][render pass][u32 subpass][base][u32 base index].
 */
static int
decode_graphics(
	struct ghci_vk_session *session,
	struct i915_wire_reader *reader,
	struct pipeline_request *request)
{
	VkPipelineShaderStageCreateInfo stage;
	VkPipelineVertexInputStateCreateInfo vertex_input;
	VkPipelineInputAssemblyStateCreateInfo assembly;
	VkPipelineTessellationStateCreateInfo tessellation;
	VkPipelineViewportStateCreateInfo viewport;
	VkPipelineColorBlendStateCreateInfo blend;
	VkPipelineDynamicStateCreateInfo dynamic;
	uint64_t present;
	uint64_t decoded;
	uint64_t stages;
	uint64_t index;
	int error;

	/* The head and the stages. */
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	stages = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || stages == 0U || stages > PIPELINE_STAGES + 3U)
		return EINVAL;
	for (index = 0U; index < stages; index++) {
		kern_memset(&stage, 0, sizeof(stage));
		i915_vkc_dec_VkPipelineShaderStageCreateInfo(reader, &session->arena, &stage);
		if (reader->error != 0)
			return EINVAL;
		error = decode_stage(session, &stage, request);
		if (error != 0)
			return error;
	}

	/* The vertex input. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		kern_memset(&vertex_input, 0, sizeof(vertex_input));
		i915_vkc_dec_VkPipelineVertexInputStateCreateInfo(reader, &session->arena, &vertex_input);
		if (reader->error != 0)
			return EINVAL;
		error = decode_vertex_input(session, &vertex_input, request);
		if (error != 0)
			return error;
	}

	/* The input assembly: the topology (a list of triangles without one). */
	request->topology = PIPELINE_TOPOLOGY_TRIANGLES;
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		kern_memset(&assembly, 0, sizeof(assembly));
		i915_vkc_dec_VkPipelineInputAssemblyStateCreateInfo(reader, &session->arena, &assembly);
		request->topology = assembly.topology;
		request->primitive_restart = assembly.primitiveRestartEnable != VK_FALSE ? 1U : 0U;
	}

	/* The tessellation, which no pipeline of the device has. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		kern_memset(&tessellation, 0, sizeof(tessellation));
		i915_vkc_dec_VkPipelineTessellationStateCreateInfo(reader, &session->arena, &tessellation);
	}

	/* The viewport state: the first viewport and scissor when they are static. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		kern_memset(&viewport, 0, sizeof(viewport));
		i915_vkc_dec_VkPipelineViewportStateCreateInfo(reader, &session->arena, &viewport);
		if (reader->error != 0)
			return EINVAL;
		decoded = ghci_vk_decoded_count(session, viewport.pViewports);
		if (viewport.pViewports != NULL && decoded != viewport.viewportCount)
			return EINVAL;
		decoded = ghci_vk_decoded_count(session, viewport.pScissors);
		if (viewport.pScissors != NULL && decoded != viewport.scissorCount)
			return EINVAL;
		if (viewport.pViewports != NULL && viewport.viewportCount != 0U) {
			request->viewport = viewport.pViewports[0];
			request->viewport_given = 1U;
		}
		if (viewport.pScissors != NULL && viewport.scissorCount != 0U) {
			request->scissor = viewport.pScissors[0];
			request->scissor_given = 1U;
		}
	}

	/* The rasterization, always there. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		i915_vkc_dec_VkPipelineRasterizationStateCreateInfo(reader, &session->arena, &request->raster);

	/* The multisample state, and the first word of its sample mask. */
	request->sample_mask = PIPELINE_SAMPLE_MASK_ALL;
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		i915_vkc_dec_VkPipelineMultisampleStateCreateInfo(reader, &session->arena, &request->samples);
		request->multisample = 1U;
		if (reader->error == 0 && request->samples.pSampleMask != NULL)
			request->sample_mask = request->samples.pSampleMask[0] & PIPELINE_SAMPLE_MASK_ALL;
		request->samples.pSampleMask = NULL;
	}

	/* The depth and stencil state. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		i915_vkc_dec_VkPipelineDepthStencilStateCreateInfo(reader, &session->arena, &request->depth);
		request->depth_stencil = 1U;
	}

	/* The colour blend state and its attachments. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		kern_memset(&blend, 0, sizeof(blend));
		i915_vkc_dec_VkPipelineColorBlendStateCreateInfo(reader, &session->arena, &blend);
		if (reader->error != 0)
			return EINVAL;
		error = decode_blend(session, &blend, request);
		if (error != 0)
			return error;
	}

	/* The dynamic state: a bit per VkDynamicState of Vulkan 1.0. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		kern_memset(&dynamic, 0, sizeof(dynamic));
		i915_vkc_dec_VkPipelineDynamicStateCreateInfo(reader, &session->arena, &dynamic);
		decoded = ghci_vk_decoded_count(session, dynamic.pDynamicStates);
		if (reader->error != 0 || decoded != dynamic.dynamicStateCount)
			return EINVAL;
		for (index = 0U; index < dynamic.dynamicStateCount; index++) {
			if ((uint32_t)dynamic.pDynamicStates[index] < 32U)
				request->dynamic |= 1U << (uint32_t)dynamic.pDynamicStates[index];
		}
	}

	/* The layout, the render pass and subpass, the base pipeline. */
	request->layout = drv_i915_wire_read_u64(reader);
	request->render_pass = drv_i915_wire_read_u64(reader);
	request->subpass = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: the request holds the record. */
	return 0;
}

/*
 * Keeps one stage of a pipeline's create: the vertex, fragment or compute
 * stage with its module and a copy of its specialization (a stage the
 * device has not is refused).  Returns 0, EINVAL (a specialization whose
 * counts are not its arrays') or ENOMEM.
 */
static int
decode_stage(
	struct ghci_vk_session *session,
	const VkPipelineShaderStageCreateInfo *info,
	struct pipeline_request *request)
{
	const VkSpecializationInfo *special;
	struct stage_request *stage;
	uint64_t decoded;
	uint32_t slot;

	/* The vertex and fragment stages of a graphics pipeline, the compute stage of a compute one. */
	if (info->stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
		slot = PIPELINE_FRAGMENT;
	} else if (info->stage == VK_SHADER_STAGE_VERTEX_BIT || info->stage == VK_SHADER_STAGE_COMPUTE_BIT) {
		slot = PIPELINE_VERTEX;
	} else {
		request->refusal = "a stage other than vertex, fragment and compute";
		return 0;
	}
	stage = &request->stages[slot];
	if (stage->stage != 0U)
		return EINVAL;
	stage->stage = info->stage;
	stage->module = (uint64_t)(uintptr_t)info->module;
	request->stage_count++;

	/* The specialization, its entries and data the arrays decoded, copied out of the command's scratch. */
	special = info->pSpecializationInfo;
	if (special == NULL)
		return 0;
	decoded = ghci_vk_decoded_count(session, special->pMapEntries);
	if (decoded != special->mapEntryCount)
		return EINVAL;
	decoded = ghci_vk_decoded_count(session, special->pData);
	if (special->pData != NULL && decoded != special->dataSize)
		return EINVAL;
	if (special->mapEntryCount == 0U)
		return 0;
	if (special->mapEntryCount > PIPELINE_SPECIALIZATIONS_MAX)
		return EINVAL;
	stage->map = kern_calloc(special->mapEntryCount, sizeof(*stage->map));
	if (stage->map == NULL)
		return ENOMEM;
	kern_memcpy(stage->map, special->pMapEntries, special->mapEntryCount * sizeof(*stage->map));
	stage->map_count = special->mapEntryCount;
	if (special->dataSize != 0U && special->pData != NULL) {
		if (special->dataSize > PIPELINE_SPECIALIZATIONS_MAX * 8U)
			return EINVAL;
		stage->data = kern_malloc(special->dataSize);
		if (stage->data == NULL)
			return ENOMEM;
		kern_memcpy(stage->data, special->pData, special->dataSize);
		stage->data_bytes = (uint32_t)special->dataSize;
	}

	/* Succeeded: the stage is kept. */
	return 0;
}

/*
 * Keeps a pipeline's vertex bindings (their strides and rates) and
 * attributes.  Returns 0, or EINVAL for counts that are not the arrays'.
 */
static int
decode_vertex_input(
	struct ghci_vk_session *session,
	const VkPipelineVertexInputStateCreateInfo *info,
	struct pipeline_request *request)
{
	const VkVertexInputBindingDescription *binding;
	const VkVertexInputAttributeDescription *attribute;
	uint64_t decoded;
	uint32_t index;

	/* The arrays as decoded. */
	decoded = ghci_vk_decoded_count(session, info->pVertexBindingDescriptions);
	if (decoded != info->vertexBindingDescriptionCount)
		return EINVAL;
	decoded = ghci_vk_decoded_count(session, info->pVertexAttributeDescriptions);
	if (decoded != info->vertexAttributeDescriptionCount)
		return EINVAL;

	/* The bindings: a binding past the hardware's sixteen is refused. */
	for (index = 0U; index < info->vertexBindingDescriptionCount; index++) {
		binding = &info->pVertexBindingDescriptions[index];
		if (binding->binding >= GHCI_VERTEX_BINDINGS) {
			request->refusal = "a vertex binding past 15";
			continue;
		}
		request->binding_mask |= 1U << binding->binding;
		request->strides[binding->binding] = binding->stride;
		if (binding->inputRate == VK_VERTEX_INPUT_RATE_INSTANCE)
			request->per_instance |= 1U << binding->binding;
	}

	/* The attributes: more than the record's sixteen are refused. */
	for (index = 0U; index < info->vertexAttributeDescriptionCount; index++) {
		attribute = &info->pVertexAttributeDescriptions[index];
		if (request->attribute_count >= GHCI_VERTEX_ATTRIBUTES) {
			request->refusal = "more than 16 vertex attributes";
			break;
		}
		request->attributes[request->attribute_count].location = attribute->location;
		request->attributes[request->attribute_count].binding = attribute->binding;
		request->attributes[request->attribute_count].format = attribute->format;
		request->attributes[request->attribute_count].offset = attribute->offset;
		request->attribute_count++;
	}

	/* Succeeded: the vertex input is kept. */
	return 0;
}

/*
 * Keeps a pipeline's colour blend state and its attachments (more than
 * eight are refused).  Returns 0, or EINVAL for a count that is not the
 * array's.
 */
static int
decode_blend(
	struct ghci_vk_session *session,
	const VkPipelineColorBlendStateCreateInfo *info,
	struct pipeline_request *request)
{
	uint64_t decoded;
	uint32_t index;

	/* The array as decoded. */
	decoded = ghci_vk_decoded_count(session, info->pAttachments);
	if (decoded != info->attachmentCount)
		return EINVAL;

	/* The state, without the scratch's array. */
	request->blend = 1U;
	request->blend_state = *info;
	request->blend_state.pAttachments = NULL;
	if (info->attachmentCount > GHCI_VK_COLOR_ATTACHMENTS) {
		request->refusal = "more than 8 colour attachments";
		request->blend_state.attachmentCount = GHCI_VK_COLOR_ATTACHMENTS;
	}

	/* The attachments. */
	for (index = 0U; index < request->blend_state.attachmentCount; index++)
		request->attachments[index] = info->pAttachments[index];

	/* Succeeded: the blend state is kept. */
	return 0;
}

/* Decodes a compute pipeline's record (VkComputePipelineCreateInfo). */
static int
decode_compute(
	struct ghci_vk_session *session,
	struct i915_wire_reader *reader,
	struct pipeline_request *request)
{
	VkComputePipelineCreateInfo info;
	int error;

	/* The record. */
	kern_memset(&info, 0, sizeof(info));
	i915_vkc_dec_VkComputePipelineCreateInfo(reader, &session->arena, &info);
	if (reader->error != 0)
		return EINVAL;

	/* The compute stage alone. */
	if (info.stage.stage != VK_SHADER_STAGE_COMPUTE_BIT)
		return EINVAL;
	error = decode_stage(session, &info.stage, request);
	if (error != 0)
		return error;
	request->layout = (uint64_t)(uintptr_t)info.layout;

	/* Succeeded: the request holds the record. */
	return 0;
}

/* Frees a decoded record and its specializations. */
static void
request_free(
	struct pipeline_request *request)
{
	uint32_t index;

	/* Nothing decoded. */
	if (request == NULL)
		return;

	/* Each stage's copies, then the record. */
	for (index = 0U; index < PIPELINE_STAGES; index++) {
		kern_free(request->stages[index].map);
		kern_free(request->stages[index].data);
	}
	kern_free(request);
}

/*
 * Makes one pipeline from its decoded record: the checks of the state,
 * the programs, their place in the executor heap with the baked pipeline
 * state record, and the state the record does not keep.  Returns the
 * VkResult; *result is the pipeline when it is VK_SUCCESS.
 */
static uint32_t
pipeline_build(
	struct ghci_vk_session *session,
	struct ghci_vk_device *device,
	const struct pipeline_request *request,
	struct ghci_vk_pipeline **result)
{
	const struct ghci_vk_pipeline_layout *layout;
	struct ghci_vk_pipeline *pipeline;
	struct pipeline_programs programs;
	struct ghci_vk_subpass subpass;
	struct ghci_vk_object *object;
	struct ghci_pso pso;
	uint8_t *view;
	uint32_t made;
	uint32_t index;
	int error;

	/* A refusal found while decoding. */
	if (request->refusal != NULL)
		return pipeline_refuse(session, request->refusal);

	/* The layout, of the session. */
	object = ghci_vk_object_find(session, GHCI_VK_PIPELINE_LAYOUT, request->layout);
	if (object == NULL)
		return pipeline_refuse(session, "a pipeline layout the session does not have");
	layout = object->as.pipeline_layout;

	/* The subpass, and the fixed-function state a graphics pipeline may have. */
	kern_memset(&subpass, 0, sizeof(subpass));
	if (request->bind_point == PIPELINE_GRAPHICS) {
		if (subpass_of(session, request, &subpass) != 0)
			return pipeline_refuse(session, "a render pass the session does not have, or a subpass it does not have");
		made = graphics_check(session, request, &subpass);
		if (made != (uint32_t)VK_SUCCESS)
			return made;
	}

	/* The programs, and what they need of the device. */
	kern_memset(&programs, 0, sizeof(programs));
	made = pipeline_compile(session, device, request, layout, &subpass, &programs);
	if (made == (uint32_t)VK_SUCCESS)
		made = programs_check(session, request, layout, &programs);

	/* The record, and the programs' place in the heap. */
	pipeline = NULL;
	if (made == (uint32_t)VK_SUCCESS) {
		pipeline = kern_calloc(1U, sizeof(*pipeline));
		if (pipeline == NULL)
			made = (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	if (made == (uint32_t)VK_SUCCESS)
		made = pipeline_place(session, device, &programs, pipeline);

	/* The pipeline state record, baked into the heap and kept. */
	if (made == (uint32_t)VK_SUCCESS) {
		pipeline_pso(request, &programs, &pso);
		pso.vs_code = pipeline->code_va[PIPELINE_VERTEX];
		pso.fs_code = pipeline->code_va[PIPELINE_FRAGMENT];
		if (request->bind_point == PIPELINE_COMPUTE) {
			pso.vs_code = 0U;
			pso.fs_code = 0U;
			pso.cs_code = pipeline->code_va[PIPELINE_VERTEX];
		}
		error = ghci_pso_encode(pipeline->pso, &pso);
		if (error != 0)
			made = pipeline_refuse(session, "a pipeline state the record does not hold");
	}
	if (made == (uint32_t)VK_SUCCESS) {
		view = pipeline->block->allocation->view + pipeline->offset;
		kern_memcpy(view, pipeline->pso, GHCI_PSO_BYTES);

		/* The tables its programs share, and what the executor sets at each bind. */
		pipeline->bind_point = request->bind_point;
		pipeline->bindings = programs.program[PIPELINE_VERTEX]->bindings;
		if (programs.program[PIPELINE_FRAGMENT] != NULL && request->stages[PIPELINE_FRAGMENT].stage != 0U)
			pipeline->bindings = programs.program[PIPELINE_FRAGMENT]->bindings;
		pipeline->push_bytes = layout->push_bytes;
		pipeline->dynamic_count = layout->dynamic_count;
		kern_memcpy(pipeline->dynamic_base, layout->dynamic_base, sizeof(pipeline->dynamic_base));
		kern_memcpy(pipeline->dynamic_sets, layout->dynamic_sets, sizeof(pipeline->dynamic_sets));
		pipeline->layout = request->layout;
		pipeline->render_pass = request->render_pass;
		pipeline->subpass = request->subpass;
		pipeline->samples = subpass.samples;
		pipeline_state(session, request, pipeline);
	}

	/* The programs' records go; a pipeline not made gives its runs back. */
	for (index = 0U; index < PIPELINE_STAGES; index++)
		ghci_gsi1_program_free(programs.program[index]);
	if (made != (uint32_t)VK_SUCCESS) {
		if (pipeline != NULL)
			pipeline_free(session, pipeline);
		return made;
	}

	/* Succeeded: the pipeline. */
	*result = pipeline;
	return (uint32_t)VK_SUCCESS;
}

/* Frees a pipeline that is no object: its runs of the heap, its budget and its record. */
static void
pipeline_free(
	struct ghci_vk_session *session,
	struct ghci_vk_pipeline *pipeline)
{
	/* The runs, what they took of the budget, then the record. */
	ghci_vk_heap_give(pipeline->block, pipeline->offset);
	ghci_vk_heap_give(pipeline->scratch_block, pipeline->scratch_offset);
	ghci_vk_budget_give(session, pipeline->charged);
	kern_free(pipeline);
}

/*
 * Checks a graphics pipeline's fixed-function state against what the
 * device has.  Returns the VkResult.
 */
static uint32_t
graphics_check(
	struct ghci_vk_session *session,
	const struct pipeline_request *request,
	const struct ghci_vk_subpass *subpass)
{
	const struct ghci_caps *caps;
	const struct ghci_vk_formats *formats;
	const struct ghci_vk *vk;
	const VkPipelineColorBlendAttachmentState *attachment;
	uint32_t factors[4];
	uint32_t index;
	uint32_t factor;
	uint32_t format;
	int has;

	/* A vertex stage, and a topology the hardware draws (a fan as triangles). */
	caps = &session->device->caps;
	if (request->stages[PIPELINE_VERTEX].stage != VK_SHADER_STAGE_VERTEX_BIT)
		return pipeline_refuse(session, "a graphics pipeline without a vertex stage");
	if (request->topology > (uint32_t)VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN)
		return pipeline_refuse(session, "an adjacency or patch topology");
	if ((uint32_t)request->raster.polygonMode > (uint32_t)VK_POLYGON_MODE_POINT)
		return pipeline_refuse(session, "a polygon mode the hardware has not");

	/* One sample, or four with multisampling (two have no standard positions). */
	has = ghci_caps_has(caps, GHCI_HAS_MSAA);
	if (subpass->samples != 1U && (subpass->samples != PIPELINE_SAMPLES_MSAA || has == 0))
		return pipeline_refuse(session, "a sample count the device does not draw");

	/* No sample shading, alpha to one, depth bounds (features the device lacks). */
	if (request->multisample != 0U && (request->samples.sampleShadingEnable != VK_FALSE || request->samples.alphaToOneEnable != VK_FALSE))
		return pipeline_refuse(session, "sample shading or alpha to one");
	if (request->depth_stencil != 0U && request->depth.depthBoundsTestEnable != VK_FALSE)
		return pipeline_refuse(session, "a depth bounds test");

	/* At most MAX_MRT colour attachments. */
	if (subpass->color_count > caps->max_mrt || request->blend_state.attachmentCount > caps->max_mrt)
		return pipeline_refuse(session, "more colour attachments than MAX_MRT");

	/* A logic op needs FEATURE[11]. */
	has = ghci_caps_has(caps, GHCI_HAS_LOGIC_OP);
	if (request->blend != 0U && request->blend_state.logicOpEnable != VK_FALSE && has == 0)
		return pipeline_refuse(session, "a logic op without LOGIC_OP");

	/* Each attachment: the second colour's factors need dual-source blending. */
	has = ghci_caps_has(caps, GHCI_HAS_DUAL_SOURCE);
	for (index = 0U; index < request->blend_state.attachmentCount; index++) {
		attachment = &request->attachments[index];
		if (attachment->blendEnable == VK_FALSE)
			continue;
		factors[0] = attachment->srcColorBlendFactor;
		factors[1] = attachment->dstColorBlendFactor;
		factors[2] = attachment->srcAlphaBlendFactor;
		factors[3] = attachment->dstAlphaBlendFactor;
		for (factor = 0U; factor < 4U; factor++) {
			if (factors[factor] >= (uint32_t)VK_BLEND_FACTOR_SRC1_COLOR && has == 0)
				return pipeline_refuse(session, "a blend factor of the second colour without DUAL_SOURCE");
		}
	}

	/* Attachments blended differently need MRT_BLEND (the factors and operators of one that does not blend do not count). */
	has = ghci_caps_has(caps, GHCI_HAS_MRT_BLEND);
	for (index = 1U; index < request->blend_state.attachmentCount && has == 0; index++) {
		if (blend_word_used(&request->attachments[index]) != blend_word_used(&request->attachments[0]))
			return pipeline_refuse(session, "attachments blended differently without MRT_BLEND");
	}

	/* Each attribute's format is one the device fetches from a vertex buffer. */
	vk = session->device->vk;
	formats = &vk->formats;
	for (index = 0U; index < request->attribute_count; index++) {
		format = request->attributes[index].format;
		if (format >= GHCI_VK_FORMATS || (formats->buffer[format] & (uint32_t)VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) == 0U)
			return pipeline_refuse(session, "a vertex attribute format without VERTEX_BUFFER");
	}

	/* Succeeded: the hardware draws the state. */
	return (uint32_t)VK_SUCCESS;
}

/*
 * Compiles a pipeline's programs: the vertex (or compute) program with
 * the pipeline's attributes in location order, then the fragment program
 * with the vertex program's tables (or the driver's own for a pipeline
 * without a fragment stage).  Returns the VkResult.
 */
static uint32_t
pipeline_compile(
	struct ghci_vk_session *session,
	struct ghci_vk_device *device,
	const struct pipeline_request *request,
	const struct ghci_vk_pipeline_layout *layout,
	const struct ghci_vk_subpass *subpass,
	struct pipeline_programs *programs)
{
	struct ghci_gsi1_options *options;
	uint32_t made;

	/* The options, too large for the stack. */
	options = kern_malloc(sizeof(*options));
	if (options == NULL)
		return (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY;

	/* The programs. */
	made = pipeline_compile_with(session, device, request, layout, subpass, options, programs);
	kern_free(options);
	return made;
}

/* Compiles a pipeline's programs as pipeline_compile() says, with room for the options. */
static uint32_t
pipeline_compile_with(
	struct ghci_vk_session *session,
	struct ghci_vk_device *device,
	const struct pipeline_request *request,
	const struct ghci_vk_pipeline_layout *layout,
	const struct ghci_vk_subpass *subpass,
	struct ghci_gsi1_options *options,
	struct pipeline_programs *programs)
{
	enum drv_gpu_shader_stage kind;
	uint32_t components;
	uint32_t traits;
	uint32_t location;
	uint32_t index;
	uint32_t made;
	int error;

	/* What the device gives every compile, and what the pipeline adds. */
	ghci_isa_options(&session->device->caps, options);
	options->robust_buffer_access = device->robust != 0 ? 1U : 0U;
	options->dynamic_count = layout->dynamic_count;
	kern_memcpy(options->dynamic, layout->dynamic, sizeof(options->dynamic));

	/* The attribute of each location: its format's components and whether it is an integer one. */
	kind = DRV_GPU_STAGE_COMPUTE;
	if (request->bind_point == PIPELINE_GRAPHICS) {
		kind = DRV_GPU_STAGE_VERTEX;
		options->has_attribute_map = 1U;
		for (index = 0U; index < request->attribute_count; index++) {
			location = request->attributes[index].location;
			components = ghci_vk_format_traits(request->attributes[index].format, &traits);
			if (location >= GHCI_GSI1_LOCATIONS || components == 0U)
				return pipeline_refuse(session, "a vertex attribute the hardware does not fetch");
			if (request->attributes[index].binding >= GHCI_VERTEX_BINDINGS || (request->binding_mask & (1U << request->attributes[index].binding)) == 0U)
				return pipeline_refuse(session, "a vertex attribute of no binding");
			options->attribute_components[location] = components;
			if ((traits & GHCI_VK_FORMAT_INTEGER) != 0U)
				options->attribute_integer |= 1U << location;
		}
	}

	/* The vertex or compute program. */
	made = compile_stage(session, &request->stages[PIPELINE_VERTEX], kind, options, &programs->program[PIPELINE_VERTEX]);
	if (made != (uint32_t)VK_SUCCESS || request->bind_point == PIPELINE_COMPUTE)
		return made;

	/* The fragment program's options: the samples, the primitive, the input attachments, the vertex program's tables. */
	options->has_attribute_map = 0U;
	kern_memset(options->attribute_components, 0, sizeof(options->attribute_components));
	options->attribute_integer = 0U;
	options->samples = subpass->samples;
	options->primitive = GHCI_GSI1_PRIMITIVE_TRIANGLE;
	if (request->topology == (uint32_t)VK_PRIMITIVE_TOPOLOGY_POINT_LIST || request->raster.polygonMode == VK_POLYGON_MODE_POINT)
		options->primitive = GHCI_GSI1_PRIMITIVE_POINT;
	else if (request->topology <= (uint32_t)VK_PRIMITIVE_TOPOLOGY_LINE_STRIP || request->raster.polygonMode == VK_POLYGON_MODE_LINE)
		options->primitive = GHCI_GSI1_PRIMITIVE_LINE;
	kern_memcpy(options->input_attachment_format, subpass->input_format, sizeof(options->input_attachment_format));
	kern_memcpy(options->input_attachment_samples, subpass->input_samples, sizeof(options->input_attachment_samples));
	options->bindings = programs->program[PIPELINE_VERTEX]->bindings;

	/* The fragment program, or the driver's own. */
	if (request->stages[PIPELINE_FRAGMENT].stage != 0U)
		return compile_stage(session, &request->stages[PIPELINE_FRAGMENT], DRV_GPU_STAGE_FRAGMENT, options, &programs->program[PIPELINE_FRAGMENT]);
	error = ghci_gsi1_empty_fragment(&programs->program[PIPELINE_FRAGMENT]);
	if (error != 0)
		return (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Succeeded: both programs. */
	return (uint32_t)VK_SUCCESS;
}

/*
 * Compiles one stage's module, specialized, with the options.  Returns
 * the VkResult; a module the compiler refuses is logged with its reason.
 */
static uint32_t
compile_stage(
	struct ghci_vk_session *session,
	const struct stage_request *stage,
	enum drv_gpu_shader_stage kind,
	const struct ghci_gsi1_options *options,
	struct ghci_gsi1_program **program)
{
	const struct ghci_isa_backend *backend;
	struct ghci_gsi1_diagnostic diagnostic;
	struct ghci_vk_object *object;
	const char *reason;
	uint32_t *words;
	int error;

	/* The module, of the session, and the device's compiler. */
	object = ghci_vk_object_find(session, GHCI_VK_SHADER_MODULE, stage->module);
	if (object == NULL)
		return pipeline_refuse(session, "a shader module the session does not have");
	backend = ghci_isa_select(&session->device->caps);
	if (backend == NULL)
		return pipeline_refuse(session, "a device without a shader unit");

	/* The words with the specialization's constants, folded operations and workgroup size in place. */
	reason = NULL;
	error = specialize(&object->as.module, stage, &words, &reason);
	if (error == ENOMEM)
		return (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY;
	if (error != 0 && reason != NULL)
		return pipeline_refuse(session, reason);
	if (error != 0)
		return pipeline_refuse(session, "a specialization the driver does not apply");

	/* The compile. */
	kern_memset(&diagnostic, 0, sizeof(diagnostic));
	error = backend->compile(words, object->as.module.word_count, kind, options, program, &diagnostic);
	kern_free(words);
	if (error == ENOMEM)
		return (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY;
	if (error != 0) {
		*program = NULL;
		if (diagnostic.reason != NULL)
			return pipeline_refuse(session, diagnostic.reason);
		return pipeline_refuse(session, "a program the compiler refuses");
	}

	/* Succeeded: the program. */
	return (uint32_t)VK_SUCCESS;
}

/*
 * Checks what a pipeline's programs need of the device: shared memory
 * within LDS_KB, a workgroup within the maxComputeWorkGroupSize and
 * maxComputeWorkGroupInvocations the device reports, no sample count
 * query without multisampling, and one dynamic offset per dynamic buffer
 * array (the compiler reads element 0's).  Returns the VkResult.
 */
static uint32_t
programs_check(
	struct ghci_vk_session *session,
	const struct pipeline_request *request,
	const struct ghci_vk_pipeline_layout *layout,
	const struct pipeline_programs *programs)
{
	const struct ghci_gsi1_program *program;
	const struct ghci_gsi1_resource *entry;
	VkPhysicalDeviceLimits *limits;
	uint64_t invocations;
	uint32_t index;
	uint32_t dynamic;
	uint32_t made;
	int has;

	/* A compute program's shared memory and workgroup, within the limits the device reports. */
	program = programs->program[PIPELINE_VERTEX];
	if (request->bind_point == PIPELINE_COMPUTE) {
		if (program->lds_bytes > session->device->caps.lds_bytes)
			return pipeline_refuse(session, "shared memory past LDS_KB");
		limits = kern_malloc(sizeof(*limits));
		if (limits == NULL)
			return (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY;
		ghci_vk_caps_limits(&session->device->caps, limits);
		invocations = (uint64_t)program->local_size[0] * program->local_size[1] * program->local_size[2];
		made = (uint32_t)VK_SUCCESS;
		if (program->local_size[0] > limits->maxComputeWorkGroupSize[0] || program->local_size[1] > limits->maxComputeWorkGroupSize[1] ||
		    program->local_size[2] > limits->maxComputeWorkGroupSize[2])
			made = pipeline_refuse(session, "a workgroup past maxComputeWorkGroupSize");
		else if (invocations > limits->maxComputeWorkGroupInvocations)
			made = pipeline_refuse(session, "a workgroup past maxComputeWorkGroupInvocations");
		kern_free(limits);
		if (made != (uint32_t)VK_SUCCESS)
			return made;
	}

	/* A sample count query needs multisampling. */
	has = ghci_caps_has(&session->device->caps, GHCI_HAS_MSAA);
	for (index = 0U; index < PIPELINE_STAGES; index++) {
		program = programs->program[index];
		if (program != NULL && (program->flags & GHCI_GSI1_FLAG_SAMPLE_QUERY) != 0U && has == 0)
			return pipeline_refuse(session, "a sample count query (texsz.s) without MSAA");
	}

	/* An element past the first of a dynamic buffer array has no offset of its own in the program. */
	program = programs->program[PIPELINE_FRAGMENT];
	if (program == NULL || request->stages[PIPELINE_FRAGMENT].stage == 0U)
		program = programs->program[PIPELINE_VERTEX];
	for (index = 0U; index < program->bindings.resource_count; index++) {
		entry = &program->bindings.resource[index];
		if (entry->element == 0U)
			continue;
		for (dynamic = 0U; dynamic < layout->dynamic_count; dynamic++) {
			if (layout->dynamic[dynamic].set == entry->set && layout->dynamic[dynamic].binding == entry->binding)
				return pipeline_refuse(session, "an element past the first of a dynamic buffer array");
		}
	}

	/* Succeeded: the device runs the programs. */
	return (uint32_t)VK_SUCCESS;
}

/*
 * Places a pipeline in the device's executor heap: the 128-byte record,
 * the code of each program (64-byte aligned), and, when a program spills,
 * the driver's table of set 3 with each stage's scratch memory
 * (RESIDENT_WARPS[15:0] warps of its bytes per warp).  Returns the
 * VkResult.
 */
static uint32_t
pipeline_place(
	struct ghci_vk_session *session,
	struct ghci_vk_device *device,
	const struct pipeline_programs *programs,
	struct ghci_vk_pipeline *pipeline)
{
	const struct ghci_gsi1_program *program;
	uint32_t code_at[PIPELINE_STAGES];
	uint32_t scratch_at[PIPELINE_STAGES];
	uint64_t scratch_bytes;
	uint64_t stage_bytes;
	uint64_t charged;
	uint32_t table_at;
	uint32_t bytes;
	uint32_t warps;
	uint32_t index;
	uint64_t base;
	uint8_t *view;
	int error;

	/* The record, then each program's code, then the driver's table. */
	bytes = GHCI_PSO_BYTES;
	scratch_bytes = 0U;
	warps = session->device->caps.resident_warps & PIPELINE_WARPS_MASK;
	for (index = 0U; index < PIPELINE_STAGES; index++) {
		code_at[index] = 0U;
		scratch_at[index] = 0U;
		program = programs->program[index];
		if (program == NULL)
			continue;
		bytes = align_up(bytes, PIPELINE_CODE_ALIGN);
		code_at[index] = bytes;
		bytes += program->word_count * 4U;
		if (program->scratch_bytes != 0U) {
			if (warps == 0U)
				return pipeline_refuse(session, "a program that spills on a device without RESIDENT_WARPS");
			scratch_at[index] = (uint32_t)scratch_bytes;
			stage_bytes = ((uint64_t)program->scratch_bytes * warps + PIPELINE_CODE_ALIGN - 1U) & ~(uint64_t)(PIPELINE_CODE_ALIGN - 1U);
			scratch_bytes += stage_bytes;
			if (scratch_bytes > GHCI_VK_HEAP_BLOCK_MAX) {
				(void)pipeline_refuse(session, "scratch memory past a block of the executor heap");
				return (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY;
			}
		}
	}
	bytes = align_up(bytes, PIPELINE_CODE_ALIGN);
	table_at = bytes;
	if (scratch_bytes != 0U)
		bytes += GSI1_ABI_DRIVER_TABLE_BYTES;

	/* The record and its runs, within the session's budget. */
	charged = sizeof(*pipeline) + (uint64_t)bytes + scratch_bytes;
	error = ghci_vk_budget_take(session, charged);
	if (error != 0)
		return (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY;
	pipeline->charged = charged;

	/* The run of the heap. */
	error = ghci_vk_heap_take(session, device, GHCI_VK_HEAP_CODE, bytes, PIPELINE_PSO_ALIGN, &pipeline->block, &pipeline->offset);
	if (error != 0)
		return ghci_vk_result(error);
	view = pipeline->block->allocation->view + pipeline->offset;
	base = pipeline->block->mapping->va + pipeline->offset;
	kern_memset(view, 0, bytes);
	pipeline->pso_va = base;

	/* Each program's code. */
	for (index = 0U; index < PIPELINE_STAGES; index++) {
		program = programs->program[index];
		if (program == NULL)
			continue;
		kern_memcpy(view + code_at[index], program->words, program->word_count * 4U);
		pipeline->code_va[index] = base + code_at[index];
	}

	/* The scratch memory and the table of its addresses, by stage (a compute program's at 0x10). */
	if (scratch_bytes != 0U) {
		error = ghci_vk_heap_take(session, device, GHCI_VK_HEAP_DATA, (uint32_t)scratch_bytes, PIPELINE_CODE_ALIGN, &pipeline->scratch_block, &pipeline->scratch_offset);
		if (error != 0)
			return ghci_vk_result(error);
		pipeline->table_va = base + table_at;
		for (index = 0U; index < PIPELINE_STAGES; index++) {
			program = programs->program[index];
			if (program == NULL || program->scratch_bytes == 0U)
				continue;
			if (program->stage == DRV_GPU_STAGE_COMPUTE)
				store64(view + table_at + GSI1_ABI_DRIVER_SCRATCH_COMPUTE, pipeline->scratch_block->mapping->va + pipeline->scratch_offset + scratch_at[index]);
			else if (program->stage == DRV_GPU_STAGE_FRAGMENT)
				store64(view + table_at + GSI1_ABI_DRIVER_SCRATCH_FRAGMENT, pipeline->scratch_block->mapping->va + pipeline->scratch_offset + scratch_at[index]);
			else
				store64(view + table_at + GSI1_ABI_DRIVER_SCRATCH_VERTEX, pipeline->scratch_block->mapping->va + pipeline->scratch_offset + scratch_at[index]);
		}
	}

	/* Succeeded: the pipeline has its place. */
	return (uint32_t)VK_SUCCESS;
}

/*
 * Fills a pipeline state from the record and the programs (the code
 * addresses are the caller's).  The Vulkan numbers of the topology, the
 * polygon mode, the cull mode, the compare and stencil operators, the
 * blend factors and operators and the logic op are the hardware's
 * (section 8.2 of the specification, "State Encodings").
 */
static void
pipeline_pso(
	const struct pipeline_request *request,
	const struct pipeline_programs *programs,
	struct ghci_pso *pso)
{
	const struct ghci_gsi1_program *vertex;
	const struct ghci_gsi1_program *fragment;
	const VkStencilOpState *faces[2];
	uint32_t *targets[2];
	uint32_t order[GHCI_VERTEX_ATTRIBUTES];
	uint32_t index;
	uint32_t place;
	uint32_t moved;
	uint32_t face;

	/* A compute pipeline: its program's registers, workgroup and shared memory. */
	kern_memset(pso, 0, sizeof(*pso));
	vertex = programs->program[PIPELINE_VERTEX];
	if (request->bind_point == PIPELINE_COMPUTE) {
		pso->regs_cs = vertex->used_regs;
		pso->local_size[0] = vertex->local_size[0];
		pso->local_size[1] = vertex->local_size[1];
		pso->local_size[2] = vertex->local_size[2];
		pso->lds_bytes = vertex->lds_bytes;
		return;
	}
	fragment = programs->program[PIPELINE_FRAGMENT];

	/* dw8: the topology (a fan drawn as a triangle list), restart, depth clamp, the logic op, the samples. */
	pso->topology = request->topology;
	if (request->topology == (uint32_t)VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN)
		pso->topology = PIPELINE_TOPOLOGY_TRIANGLES;
	pso->primitive_restart = request->primitive_restart;
	pso->depth_clamp = request->raster.depthClampEnable != VK_FALSE ? 1U : 0U;
	if (request->blend != 0U && request->blend_state.logicOpEnable != VK_FALSE) {
		pso->logic_op_enable = 1U;
		pso->logic_op = request->blend_state.logicOp;
	}
	if (request->multisample != 0U && request->samples.alphaToCoverageEnable != VK_FALSE)
		pso->alpha_to_coverage = 1U;
	pso->sample_mask = request->sample_mask;

	/* dw9: culling, the front face, the polygon mode, depth bias, the line width (1.0 when dynamic). */
	pso->cull_mode = request->raster.cullMode;
	pso->front_cw = request->raster.frontFace == VK_FRONT_FACE_CLOCKWISE ? 1U : 0U;
	pso->polygon_mode = request->raster.polygonMode;
	pso->depth_bias_enable = request->raster.depthBiasEnable != VK_FALSE ? 1U : 0U;
	pso->line_width_fx4 = PIPELINE_LINE_WIDTH_ONE;

	/* dw10: the depth test and each face's stencil (fail, pass, depth fail, compare), separate when they differ. */
	if (request->depth_stencil != 0U) {
		pso->depth_test = request->depth.depthTestEnable != VK_FALSE ? 1U : 0U;
		pso->depth_write = request->depth.depthWriteEnable != VK_FALSE ? 1U : 0U;
		pso->depth_op = request->depth.depthCompareOp;
		pso->stencil_test = request->depth.stencilTestEnable != VK_FALSE ? 1U : 0U;
		faces[0] = &request->depth.front;
		faces[1] = &request->depth.back;
		targets[0] = pso->front_stencil;
		targets[1] = pso->back_stencil;
		for (face = 0U; face < 2U; face++) {
			targets[face][0] = faces[face]->failOp;
			targets[face][1] = faces[face]->passOp;
			targets[face][2] = faces[face]->depthFailOp;
			targets[face][3] = faces[face]->compareOp;
		}
		for (index = 0U; index < 4U; index++) {
			if (pso->front_stencil[index] != pso->back_stencil[index])
				pso->separate_back = 1U;
		}
	}

	/* dw11: attachment 0's blend (each attachment's is set dynamically with MRT_BLEND). */
	if (request->blend != 0U && request->blend_state.attachmentCount != 0U) {
		pso->blend_enable = request->attachments[0].blendEnable != VK_FALSE ? 1U : 0U;
		pso->src_color = request->attachments[0].srcColorBlendFactor;
		pso->dst_color = request->attachments[0].dstColorBlendFactor;
		pso->color_op = request->attachments[0].colorBlendOp;
		pso->src_alpha = request->attachments[0].srcAlphaBlendFactor;
		pso->dst_alpha = request->attachments[0].dstAlphaBlendFactor;
		pso->alpha_op = request->attachments[0].alphaBlendOp;
		pso->write_mask = request->attachments[0].colorWriteMask;
	}

	/* dw12: the default strides of bindings 0 to 3 that fit a byte (the executor binds each stride). */
	for (index = 0U; index < 4U; index++) {
		if (request->strides[index] <= 0xffU)
			pso->strides[index] = request->strides[index];
	}

	/* dw13 to dw15: the registers, the varyings, the fragment program's flags. */
	pso->regs_vs = vertex->used_regs;
	pso->regs_fs = fragment->used_regs;
	pso->varyings = vertex->varying_count;
	pso->fs_centroid = (fragment->flags & GHCI_GSI1_FLAG_CENTROID) != 0U ? 1U : 0U;
	pso->fs_depth_export = (fragment->flags & GHCI_GSI1_FLAG_DEPTH_EXPORT) != 0U ? 1U : 0U;
	pso->fs_discard = (fragment->flags & GHCI_GSI1_FLAG_DISCARD) != 0U ? 1U : 0U;

	/* The attributes in increasing location order, slot after slot (an insertion). */
	for (index = 0U; index < request->attribute_count; index++) {
		place = index;
		moved = index;
		while (place > 0U && request->attributes[order[place - 1U]].location > request->attributes[moved].location) {
			order[place] = order[place - 1U];
			place--;
		}
		order[place] = moved;
	}
	for (index = 0U; index < request->attribute_count; index++) {
		pso->attributes[index].format = request->attributes[order[index]].format;
		pso->attributes[index].binding = request->attributes[order[index]].binding;
		pso->attributes[index].per_instance = (request->per_instance >> request->attributes[order[index]].binding) & 1U;
		pso->attributes[index].offset = request->attributes[order[index]].offset;
	}
	pso->attribute_count = request->attribute_count;
}

/*
 * Keeps what the pipeline state record does not: the dynamic states, the
 * static viewport and scissor, line width, depth bias, blend constants
 * and stencil masks and references, each attachment's blend word on a
 * device with independent blending, the vertex bindings' strides, the
 * fan and the primitive.
 */
static void
pipeline_state(
	struct ghci_vk_session *session,
	const struct pipeline_request *request,
	struct ghci_vk_pipeline *pipeline)
{
	struct ghci_vk_static_state *state;
	const VkStencilOpState *faces[2];
	uint32_t index;
	int has;

	/* The vertex bindings, the fan and the primitive. */
	state = &pipeline->state;
	pipeline->vertex_bindings = request->binding_mask;
	kern_memcpy(pipeline->strides, request->strides, sizeof(pipeline->strides));
	pipeline->fan = request->topology == (uint32_t)VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN ? 1U : 0U;
	pipeline->primitive = request->topology;
	if (request->bind_point == PIPELINE_COMPUTE)
		return;

	/* The dynamic states, the viewport and the scissor. */
	state->dynamic = request->dynamic;
	if (request->viewport_given != 0U) {
		state->viewport[0] = load32((const uint8_t *)&request->viewport.x);
		state->viewport[1] = load32((const uint8_t *)&request->viewport.y);
		state->viewport[2] = load32((const uint8_t *)&request->viewport.width);
		state->viewport[3] = load32((const uint8_t *)&request->viewport.height);
		state->viewport[4] = load32((const uint8_t *)&request->viewport.minDepth);
		state->viewport[5] = load32((const uint8_t *)&request->viewport.maxDepth);
	}
	if (request->scissor_given != 0U) {
		state->scissor_offset[0] = request->scissor.offset.x;
		state->scissor_offset[1] = request->scissor.offset.y;
		state->scissor_extent[0] = request->scissor.extent.width;
		state->scissor_extent[1] = request->scissor.extent.height;
	}

	/* The line width, the depth bias (constant, slope, clamp) and the blend constants. */
	state->line_width = PIPELINE_FLOAT_ONE;
	state->depth_bias[0] = load32((const uint8_t *)&request->raster.depthBiasConstantFactor);
	state->depth_bias[1] = load32((const uint8_t *)&request->raster.depthBiasSlopeFactor);
	state->depth_bias[2] = load32((const uint8_t *)&request->raster.depthBiasClamp);
	for (index = 0U; index < 4U; index++)
		state->blend_constants[index] = load32((const uint8_t *)&request->blend_state.blendConstants[index]);

	/*
	 * Each face's stencil compare mask, write mask and reference in one
	 * word.
	 *
	 * Values referenced: amaterrace-gpu d9252e4 docs/ghci.md (section
	 * 7.3, CMD_SET_DYNAMIC_STATE 0x03: [7:0] compare mask, [15:8] write
	 * mask, [23:16] reference).
	 */
	faces[0] = &request->depth.front;
	faces[1] = &request->depth.back;
	for (index = 0U; index < 2U; index++) {
		state->stencil[index] = faces[index]->compareMask & 0xffU;
		state->stencil[index] |= (faces[index]->writeMask & 0xffU) << 8;
		state->stencil[index] |= (faces[index]->reference & 0xffU) << 16;
	}

	/* Each attachment's blend word, which only a device with independent blending takes. */
	has = ghci_caps_has(&session->device->caps, GHCI_HAS_MRT_BLEND);
	if (has != 0) {
		state->blend_count = request->blend_state.attachmentCount;
		for (index = 0U; index < state->blend_count; index++)
			state->blend_attachment[index] = blend_word(&request->attachments[index]);
	}

	/* A pipeline that discards its primitives draws with an empty scissor. */
	state->rasterizer_discard = request->raster.rasterizerDiscardEnable != VK_FALSE ? 1U : 0U;
}

/*
 * Reports the blend word of an attachment (the record's dw11, which
 * SET_DYNAMIC_STATE 0x07 to 0x0E takes), packed by the record's encoder
 * so that the bits have one place.
 */
static uint32_t
blend_word(
	const VkPipelineColorBlendAttachmentState *attachment)
{
	uint8_t bytes[GHCI_PSO_BYTES];
	struct ghci_pso pso;
	int error;

	/* A record with the attachment's blend alone. */
	kern_memset(&pso, 0, sizeof(pso));
	pso.blend_enable = attachment->blendEnable != VK_FALSE ? 1U : 0U;
	pso.src_color = attachment->srcColorBlendFactor;
	pso.dst_color = attachment->dstColorBlendFactor;
	pso.color_op = attachment->colorBlendOp;
	pso.src_alpha = attachment->srcAlphaBlendFactor;
	pso.dst_alpha = attachment->dstAlphaBlendFactor;
	pso.alpha_op = attachment->alphaBlendOp;
	pso.write_mask = attachment->colorWriteMask;

	/* Its dw11, or 0 for factors the record does not hold. */
	error = ghci_pso_encode(bytes, &pso);
	if (error != 0)
		return 0U;

	/* Succeeded: the word. */
	return load32(&bytes[GHCI_PSO_BLEND]);
}

/*
 * Reports what of an attachment's blend word the hardware uses: the
 * whole word when it blends, the enable bit and the write mask alone
 * when it does not.
 */
static uint32_t
blend_word_used(
	const VkPipelineColorBlendAttachmentState *attachment)
{
	VkPipelineColorBlendAttachmentState plain;

	/* A blending attachment's whole word. */
	if (attachment->blendEnable != VK_FALSE)
		return blend_word(attachment);

	/* One that does not blend: its write mask alone. */
	kern_memset(&plain, 0, sizeof(plain));
	plain.colorWriteMask = attachment->colorWriteMask;

	/* Succeeded: the word. */
	return blend_word(&plain);
}

/*
 * Reports what a pipeline knows of its subpass: the render pass's record.
 * Returns 0, or ENOENT for a render pass the session does not have (or
 * the null handle) and a subpass the pass does not have.
 */
static int
subpass_of(
	struct ghci_vk_session *session,
	const struct pipeline_request *request,
	struct ghci_vk_subpass *subpass)
{
	struct ghci_vk_object *object;

	/* The render pass's subpass. */
	object = ghci_vk_object_find(session, GHCI_VK_RENDER_PASS, request->render_pass);
	if (object == NULL || object->as.render_pass == NULL || request->subpass >= object->as.render_pass->subpass_count)
		return ENOENT;
	*subpass = object->as.render_pass->subpasses[request->subpass];

	/* Succeeded: the subpass is known. */
	return 0;
}

/*
 * Makes a copy of a module's words with a stage's specialization in
 * place: each OpSpecConstantTrue, OpSpecConstantFalse and OpSpecConstant
 * becomes the plain constant of its SpecId's value (its default without
 * one), OpSpecConstantComposite becomes OpConstantComposite, each
 * OpSpecConstantOp of 32-bit integers and Booleans is folded into the
 * plain constant of its result (in the order of the module, so that one
 * may use another's), the SpecId decorations become OpNop, and the
 * constant the WorkgroupSize built-in decorates goes into the LocalSize
 * (or LocalSizeId) execution mode, which it overrides.  Returns 0,
 * EINVAL with *reason for what the driver does not apply, or ENOMEM; the
 * caller frees the copy.
 */
static int
specialize(
	const struct ghci_vk_shader_module *module,
	const struct stage_request *stage,
	uint32_t **words,
	const char **reason)
{
	struct spec_module spec;
	uint32_t at;
	uint32_t count;
	uint32_t limit;
	int folds;
	int error;

	/* The copy, and the table of the SpecId decorations. */
	kern_memset(&spec, 0, sizeof(spec));
	spec.word_count = module->word_count;
	spec.words = kern_malloc((size_t)module->word_count * 4U);
	if (spec.words == NULL)
		return ENOMEM;
	spec.ids = kern_calloc(2U * PIPELINE_SPECIALIZATIONS_MAX, sizeof(*spec.ids));
	if (spec.ids == NULL) {
		kern_free(spec.words);
		return ENOMEM;
	}
	spec.spec_ids = spec.ids + PIPELINE_SPECIALIZATIONS_MAX;
	kern_memcpy(spec.words, module->words, (size_t)module->word_count * 4U);

	/* The SpecId decorations (each turned into OpNop), the operations to fold and the WorkgroupSize. */
	folds = specialize_scan(&spec);
	error = 0;
	if (folds < 0)
		error = EINVAL;

	/* The values of the ids below the bound, when an operation or the workgroup size needs them. */
	if (error == 0 && (folds > 0 || spec.workgroup != 0U)) {
		limit = spec.words[SPIRV_BOUND_WORD];
		if (limit > spec.word_count)
			limit = spec.word_count;
		if (limit == 0U)
			limit = 1U;
		spec.value_count = limit;
		spec.values = kern_calloc(limit, sizeof(*spec.values));
		if (spec.values == NULL)
			error = ENOMEM;
	}

	/* The constants, then the folds, in the order of the module. */
	for (at = SPIRV_HEADER_WORDS; error == 0 && at < spec.word_count; at += count) {
		count = spec.words[at] >> SPIRV_COUNT_SHIFT;
		error = specialize_constant(&spec, at, count, stage);
		if (error == 0 && spec.values != NULL)
			error = specialize_track(&spec, at, count);
	}

	/* The workgroup size the built-in gives. */
	if (error == 0 && spec.workgroup != 0U)
		error = specialize_workgroup(&spec);

	/* The tables go; a copy not specialized goes too. */
	kern_free(spec.values);
	kern_free(spec.ids);
	if (error != 0) {
		kern_free(spec.words);
		*reason = spec.reason;
		return error;
	}

	/* Succeeded: the specialized words. */
	*words = spec.words;
	return 0;
}

/*
 * Reads a module's copy once: its instructions' word counts, each SpecId
 * decoration (kept in the table and turned into OpNop), whether it has an
 * OpSpecConstantOp, and the id the WorkgroupSize built-in decorates.
 * Returns 1 when there is an operation to fold, 0 when there is none, or
 * -1 for a module the driver does not read (the reason set).
 */
static int
specialize_scan(
	struct spec_module *spec)
{
	uint32_t *words;
	uint32_t at;
	uint32_t count;
	uint32_t opcode;
	int folds;

	/* Each instruction, of a whole word count within the module. */
	words = spec->words;
	folds = 0;
	for (at = SPIRV_HEADER_WORDS; at < spec->word_count; at += count) {
		opcode = words[at] & SPIRV_OPCODE_MASK;
		count = words[at] >> SPIRV_COUNT_SHIFT;
		if (count == 0U || count > spec->word_count - at) {
			spec->reason = "a module whose instructions do not fit its words";
			return -1;
		}

		/* An operation to fold. */
		if (opcode == SPIRV_OP_SPEC_OP) {
			folds = 1;
			continue;
		}

		/* The constant the WorkgroupSize built-in decorates. */
		if (opcode == SPIRV_OP_DECORATE && count == 4U && words[at + 2U] == SPIRV_DECORATION_BUILTIN &&
		    words[at + 3U] == SPIRV_BUILTIN_WORKGROUP_SIZE) {
			spec->workgroup = words[at + 1U];
			continue;
		}

		/* A SpecId decoration, kept and turned into OpNop. */
		if (opcode != SPIRV_OP_DECORATE || count != 4U || words[at + 2U] != SPIRV_DECORATION_SPEC_ID)
			continue;
		if (spec->spec_count >= PIPELINE_SPECIALIZATIONS_MAX) {
			spec->reason = "more than 256 specialization constants";
			return -1;
		}
		spec->ids[spec->spec_count] = words[at + 1U];
		spec->spec_ids[spec->spec_count] = words[at + 3U];
		spec->spec_count++;
		words[at] = SPIRV_NOP_WORD;
		words[at + 1U] = SPIRV_NOP_WORD;
		words[at + 2U] = SPIRV_NOP_WORD;
		words[at + 3U] = SPIRV_NOP_WORD;
	}

	/* Succeeded: the module is read. */
	return folds;
}

/*
 * Rewrites one instruction of a specialized copy when it is a
 * specialization constant (an OpSpecConstantOp is left to
 * specialize_fold()).  Returns 0, or EINVAL for a value that does not fit
 * its constant.
 */
static int
specialize_constant(
	struct spec_module *spec,
	uint32_t at,
	uint32_t count,
	const struct stage_request *stage)
{
	const VkSpecializationMapEntry *entry;
	uint32_t *words;
	uint32_t opcode;
	uint32_t value;
	uint32_t index;
	uint32_t spec_id;
	int found;

	/* The instruction's opcode; a composite only changes its opcode. */
	words = spec->words;
	opcode = words[at] & SPIRV_OPCODE_MASK;
	if (opcode == SPIRV_OP_SPEC_COMPOSITE) {
		words[at] = (words[at] & ~SPIRV_OPCODE_MASK) | SPIRV_OP_CONSTANT_COMPOSITE;
		return 0;
	}
	if (opcode != SPIRV_OP_SPEC_TRUE && opcode != SPIRV_OP_SPEC_FALSE && opcode != SPIRV_OP_SPEC_CONSTANT)
		return 0;
	if (count < 3U) {
		spec->reason = "a specialization constant without its id";
		return EINVAL;
	}

	/* The constant's SpecId, and the map's entry for it. */
	entry = NULL;
	found = 0;
	spec_id = 0U;
	for (index = 0U; index < spec->spec_count; index++) {
		if (spec->ids[index] == words[at + 2U]) {
			spec_id = spec->spec_ids[index];
			found = 1;
			break;
		}
	}
	for (index = 0U; found != 0 && index < stage->map_count; index++) {
		if (stage->map[index].constantID == spec_id) {
			entry = &stage->map[index];
			break;
		}
	}
	if (entry != NULL && ((uint64_t)entry->offset + entry->size > stage->data_bytes || stage->data == NULL)) {
		spec->reason = "a specialization map entry past its data";
		return EINVAL;
	}

	/* A Boolean: the entry's value (a VkBool32), or its default. */
	if (opcode == SPIRV_OP_SPEC_TRUE || opcode == SPIRV_OP_SPEC_FALSE) {
		value = opcode == SPIRV_OP_SPEC_TRUE ? 1U : 0U;
		if (entry != NULL) {
			value = 0U;
			kern_memcpy(&value, stage->data + entry->offset, entry->size < 4U ? entry->size : 4U);
		}
		words[at] = (words[at] & ~SPIRV_OPCODE_MASK) | (value != 0U ? SPIRV_OP_CONSTANT_TRUE : SPIRV_OP_CONSTANT_FALSE);
		return 0;
	}

	/* A scalar: the entry's bytes over the literal words. */
	if (entry != NULL) {
		if (entry->size > (uint64_t)(count - 3U) * 4U) {
			spec->reason = "a specialization value larger than its constant";
			return EINVAL;
		}
		kern_memcpy(&words[at + 3U], stage->data + entry->offset, entry->size);
	}
	words[at] = (words[at] & ~SPIRV_OPCODE_MASK) | SPIRV_OP_CONSTANT;

	/* Succeeded: the constant is plain. */
	return 0;
}

/*
 * Keeps what one instruction of a specialized copy makes, for the folds
 * and the workgroup size: a 32-bit integer or Boolean type, a plain
 * constant of one, a constant composite; and folds an OpSpecConstantOp.
 * Returns 0, or EINVAL for an operation the driver does not fold.
 */
static int
specialize_track(
	struct spec_module *spec,
	uint32_t at,
	uint32_t count)
{
	struct spec_value *values;
	uint32_t *words;
	uint32_t opcode;
	uint32_t limit;
	int error;

	/* The instruction, and the ids the table follows. */
	words = spec->words;
	values = spec->values;
	limit = spec->value_count;
	opcode = words[at] & SPIRV_OPCODE_MASK;

	/* What each kind of instruction makes (an id past the table is not followed). */
	error = 0;
	switch (opcode) {
	case SPIRV_OP_TYPE_INT:
		if (count == 4U && words[at + 1U] < limit && words[at + 2U] == SPEC_INT_BITS)
			values[words[at + 1U]].kind = SPEC_TYPE_INT;
		break;
	case SPIRV_OP_TYPE_BOOL:
		if (count == 2U && words[at + 1U] < limit)
			values[words[at + 1U]].kind = SPEC_TYPE_BOOL;
		break;
	case SPIRV_OP_CONSTANT:
		if (count == 4U && words[at + 1U] < limit && words[at + 2U] < limit && values[words[at + 1U]].kind == SPEC_TYPE_INT) {
			values[words[at + 2U]].kind = SPEC_INT;
			values[words[at + 2U]].value = words[at + 3U];
		}
		break;
	case SPIRV_OP_CONSTANT_TRUE:
	case SPIRV_OP_CONSTANT_FALSE:
		if (count == 3U && words[at + 2U] < limit) {
			values[words[at + 2U]].kind = SPEC_BOOL;
			values[words[at + 2U]].value = opcode == SPIRV_OP_CONSTANT_TRUE ? 1U : 0U;
		}
		break;
	case SPIRV_OP_CONSTANT_COMPOSITE:
		if (count >= 3U && words[at + 2U] < limit) {
			values[words[at + 2U]].kind = SPEC_COMPOSITE;
			values[words[at + 2U]].value = at;
		}
		break;
	case SPIRV_OP_SPEC_OP:
		error = specialize_fold(spec, at, count);
		if (error != 0)
			return error;
		break;
	default:
		break;
	}

	/* Succeeded: the instruction is kept. */
	return 0;
}

/*
 * Folds one OpSpecConstantOp of a specialized copy into the plain
 * constant of its result (OpConstant, OpConstantTrue or OpConstantFalse,
 * the words after it OpNop).  Its operands are 32-bit integer or Boolean
 * constants made before it.  Returns 0, or EINVAL (the reason set).
 */
static int
specialize_fold(
	struct spec_module *spec,
	uint32_t at,
	uint32_t count)
{
	uint32_t kinds[SPEC_OPERANDS_MAX];
	uint32_t operands[SPEC_OPERANDS_MAX];
	struct spec_value *values;
	uint32_t *words;
	uint32_t operation;
	uint32_t operand_count;
	uint32_t type;
	uint32_t result;
	uint32_t kind;
	uint32_t value;
	uint32_t index;
	uint32_t id;
	int error;

	/* The result's type and id, the operation, and its operands' values. */
	words = spec->words;
	values = spec->values;
	if (count < 5U || count - 4U > SPEC_OPERANDS_MAX) {
		spec->reason = "an OpSpecConstantOp the driver does not fold";
		return EINVAL;
	}
	type = words[at + 1U];
	result = words[at + 2U];
	operation = words[at + 3U];
	operand_count = count - 4U;
	if (type >= spec->value_count || result >= spec->value_count) {
		spec->reason = "an OpSpecConstantOp of an id past the module's bound";
		return EINVAL;
	}
	for (index = 0U; index < operand_count; index++) {
		id = words[at + 4U + index];
		if (id >= spec->value_count || (values[id].kind != SPEC_INT && values[id].kind != SPEC_BOOL)) {
			spec->reason = "an OpSpecConstantOp of something other than 32-bit integer and Boolean constants";
			return EINVAL;
		}
		kinds[index] = values[id].kind;
		operands[index] = values[id].value;
	}

	/* The result, of the type the instruction names. */
	error = fold_value(operation, operand_count, kinds, operands, &kind, &value);
	if (error != 0) {
		spec->reason = "an OpSpecConstantOp the driver does not fold";
		return EINVAL;
	}
	if ((kind == SPEC_INT && values[type].kind != SPEC_TYPE_INT) || (kind == SPEC_BOOL && values[type].kind != SPEC_TYPE_BOOL)) {
		spec->reason = "an OpSpecConstantOp whose result is not of its type";
		return EINVAL;
	}

	/* The plain constant, then OpNop to the instruction's end. */
	if (kind == SPEC_INT) {
		words[at] = (4U << SPIRV_COUNT_SHIFT) | SPIRV_OP_CONSTANT;
		words[at + 3U] = value;
		index = 4U;
	} else {
		words[at] = (3U << SPIRV_COUNT_SHIFT) | (value != 0U ? SPIRV_OP_CONSTANT_TRUE : SPIRV_OP_CONSTANT_FALSE);
		index = 3U;
	}
	while (index < count) {
		words[at + index] = SPIRV_NOP_WORD;
		index++;
	}
	values[result].kind = kind;
	values[result].value = value;

	/* Succeeded: the operation is a constant. */
	return 0;
}

/*
 * Reports the value of a folded operation on 32-bit integers (wrapping)
 * and Booleans, and its kind (SPEC_INT or SPEC_BOOL).  Returns 0, or
 * EINVAL for an operation the driver does not fold, operands of another
 * kind, a division by zero, a signed division's overflow, or a shift of
 * 32 bits or more.
 */
static int
fold_value(
	uint32_t operation,
	uint32_t operands,
	const uint32_t *kinds,
	const uint32_t *values,
	uint32_t *kind,
	uint32_t *value)
{
	uint32_t left;
	uint32_t right;
	int error;

	/* OpSelect: a Boolean, then two operands of one kind. */
	if (operation == SPIRV_OP_SELECT) {
		if (operands != 3U || kinds[0] != SPEC_BOOL || kinds[1] != kinds[2])
			return EINVAL;
		*kind = kinds[1];
		*value = values[0] != 0U ? values[1] : values[2];
		return 0;
	}

	/* One operand: a negation, a complement, a conversion of 32 bits to 32 bits, a logical not. */
	left = values[0];
	if (operands == 1U) {
		*kind = kinds[0];
		if (kinds[0] == SPEC_BOOL && operation == SPIRV_OP_LOGICAL_NOT) {
			*value = left != 0U ? 0U : 1U;
			return 0;
		}
		if (kinds[0] != SPEC_INT)
			return EINVAL;
		switch (operation) {
		case SPIRV_OP_S_NEGATE:
			*value = 0U - left;
			return 0;
		case SPIRV_OP_NOT:
			*value = ~left;
			return 0;
		case SPIRV_OP_U_CONVERT:
		case SPIRV_OP_S_CONVERT:
			*value = left;
			return 0;
		default:
			return EINVAL;
		}
	}

	/* Two operands of one kind. */
	if (operands != 2U || kinds[0] != kinds[1])
		return EINVAL;
	right = values[1];

	/* Two Booleans (each 0 or 1). */
	if (kinds[0] == SPEC_BOOL) {
		*kind = SPEC_BOOL;
		switch (operation) {
		case SPIRV_OP_LOGICAL_EQUAL:
			*value = left == right ? 1U : 0U;
			return 0;
		case SPIRV_OP_LOGICAL_NOT_EQUAL:
			*value = left != right ? 1U : 0U;
			return 0;
		case SPIRV_OP_LOGICAL_OR:
			*value = left | right;
			return 0;
		case SPIRV_OP_LOGICAL_AND:
			*value = left & right;
			return 0;
		default:
			return EINVAL;
		}
	}

	/* Two integers compared. */
	*kind = SPEC_BOOL;
	switch (operation) {
	case SPIRV_OP_I_EQUAL:
		*value = left == right ? 1U : 0U;
		return 0;
	case SPIRV_OP_I_NOT_EQUAL:
		*value = left != right ? 1U : 0U;
		return 0;
	case SPIRV_OP_U_GREATER:
		*value = left > right ? 1U : 0U;
		return 0;
	case SPIRV_OP_U_GREATER_EQUAL:
		*value = left >= right ? 1U : 0U;
		return 0;
	case SPIRV_OP_U_LESS:
		*value = left < right ? 1U : 0U;
		return 0;
	case SPIRV_OP_U_LESS_EQUAL:
		*value = left <= right ? 1U : 0U;
		return 0;
	default:
		break;
	}

	/* Two integers, wrapping at 32 bits. */
	*kind = SPEC_INT;
	switch (operation) {
	case SPIRV_OP_I_ADD:
		*value = left + right;
		return 0;
	case SPIRV_OP_I_SUB:
		*value = left - right;
		return 0;
	case SPIRV_OP_I_MUL:
		*value = left * right;
		return 0;
	case SPIRV_OP_U_DIV:
	case SPIRV_OP_U_MOD:
		if (right == 0U)
			return EINVAL;
		if (operation == SPIRV_OP_U_DIV)
			*value = left / right;
		else
			*value = left % right;
		return 0;
	case SPIRV_OP_SHIFT_LEFT_LOGICAL:
	case SPIRV_OP_SHIFT_RIGHT_LOGICAL:
	case SPIRV_OP_SHIFT_RIGHT_ARITHMETIC:
		if (right >= SPEC_INT_BITS)
			return EINVAL;
		if (operation == SPIRV_OP_SHIFT_LEFT_LOGICAL)
			*value = left << right;
		else if (operation == SPIRV_OP_SHIFT_RIGHT_LOGICAL || (left & SPEC_INT_SIGN) == 0U)
			*value = left >> right;
		else
			*value = ~(~left >> right);
		return 0;
	case SPIRV_OP_BITWISE_OR:
		*value = left | right;
		return 0;
	case SPIRV_OP_BITWISE_XOR:
		*value = left ^ right;
		return 0;
	case SPIRV_OP_BITWISE_AND:
		*value = left & right;
		return 0;
	default:
		break;
	}

	/* The signed ones (comparisons give a Boolean). */
	error = fold_signed(operation, left, right, value);
	if (error != 0)
		return error;
	if (operation == SPIRV_OP_S_GREATER || operation == SPIRV_OP_S_GREATER_EQUAL || operation == SPIRV_OP_S_LESS ||
	    operation == SPIRV_OP_S_LESS_EQUAL)
		*kind = SPEC_BOOL;

	/* Succeeded: the value. */
	return 0;
}

/*
 * Reports the value of a signed operation on two 32-bit integers: a
 * comparison (1 or 0), a division rounded toward zero, the remainder with
 * the dividend's sign (OpSRem) or the divisor's (OpSMod).  Returns 0, or
 * EINVAL for another operation, a division by zero, or the most negative
 * value divided by -1.
 */
static int
fold_signed(
	uint32_t operation,
	uint32_t left,
	uint32_t right,
	uint32_t *value)
{
	int32_t a;
	int32_t b;
	int32_t remainder;

	/* The two's-complement values. */
	a = (int32_t)left;
	b = (int32_t)right;

	/* The comparisons. */
	switch (operation) {
	case SPIRV_OP_S_GREATER:
		*value = a > b ? 1U : 0U;
		return 0;
	case SPIRV_OP_S_GREATER_EQUAL:
		*value = a >= b ? 1U : 0U;
		return 0;
	case SPIRV_OP_S_LESS:
		*value = a < b ? 1U : 0U;
		return 0;
	case SPIRV_OP_S_LESS_EQUAL:
		*value = a <= b ? 1U : 0U;
		return 0;
	default:
		break;
	}

	/* A division: a divisor that is not zero, no overflow. */
	if (operation != SPIRV_OP_S_DIV && operation != SPIRV_OP_S_REM && operation != SPIRV_OP_S_MOD)
		return EINVAL;
	if (b == 0 || (left == SPEC_INT_SIGN && right == UINT32_MAX))
		return EINVAL;
	if (operation == SPIRV_OP_S_DIV) {
		*value = (uint32_t)(a / b);
		return 0;
	}

	/* The remainders. */
	remainder = a % b;
	if (operation == SPIRV_OP_S_MOD && remainder != 0 && (remainder < 0) != (b < 0))
		remainder += b;

	/* Succeeded: the remainder. */
	*value = (uint32_t)remainder;
	return 0;
}

/*
 * Puts the workgroup size the WorkgroupSize built-in's constant composite
 * gives into the module's LocalSize execution mode (its three values) or
 * LocalSizeId one (its three ids).  Returns 0, or EINVAL (the reason
 * set).
 */
static int
specialize_workgroup(
	struct spec_module *spec)
{
	uint32_t sizes[3];
	uint32_t ids[3];
	uint32_t *words;
	uint32_t composite;
	uint32_t at;
	uint32_t count;
	uint32_t opcode;
	uint32_t axis;
	uint32_t id;
	int placed;

	/* The composite of three 32-bit integer constants. */
	words = spec->words;
	if (spec->workgroup >= spec->value_count || spec->values[spec->workgroup].kind != SPEC_COMPOSITE) {
		spec->reason = "a WorkgroupSize that is not a constant composite";
		return EINVAL;
	}
	composite = spec->values[spec->workgroup].value;
	if ((words[composite] >> SPIRV_COUNT_SHIFT) != 6U) {
		spec->reason = "a WorkgroupSize that is not a constant composite";
		return EINVAL;
	}
	for (axis = 0U; axis < 3U; axis++) {
		id = words[composite + 3U + axis];
		if (id >= spec->value_count || spec->values[id].kind != SPEC_INT) {
			spec->reason = "a WorkgroupSize of something other than 32-bit integer constants";
			return EINVAL;
		}
		ids[axis] = id;
		sizes[axis] = spec->values[id].value;
	}

	/* The execution modes that give the size. */
	placed = 0;
	for (at = SPIRV_HEADER_WORDS; at < spec->word_count; at += count) {
		opcode = words[at] & SPIRV_OPCODE_MASK;
		count = words[at] >> SPIRV_COUNT_SHIFT;
		if (count != 6U)
			continue;
		if (opcode == SPIRV_OP_EXECUTION_MODE && words[at + 2U] == SPIRV_MODE_LOCAL_SIZE) {
			for (axis = 0U; axis < 3U; axis++)
				words[at + 3U + axis] = sizes[axis];
			placed = 1;
		} else if (opcode == SPIRV_OP_EXECUTION_MODE_ID && words[at + 2U] == SPIRV_MODE_LOCAL_SIZE_ID) {
			for (axis = 0U; axis < 3U; axis++)
				words[at + 3U + axis] = ids[axis];
			placed = 1;
		}
	}
	if (placed == 0) {
		spec->reason = "a WorkgroupSize without a LocalSize to put it in";
		return EINVAL;
	}

	/* Succeeded: the module's workgroup is the built-in's. */
	return 0;
}

/*
 * Logs why a pipeline is refused (the first ones of the device) and
 * reports the VkResult it is refused with.
 */
static uint32_t
pipeline_refuse(
	struct ghci_vk_session *session,
	const char *reason)
{
	struct ghci_vk *vk;

	/* The first refusals of the device are shown. */
	vk = session->device->vk;
	if (vk->refusals < PIPELINE_BATCH_MAX) {
		vk->refusals++;
		kern_logf("ghci: %s: vk: pipeline refused: %s\n", session->device->name, reason);
	}

	/* Succeeded: the result. */
	return PIPELINE_REFUSED;
}

/* The destroy of a shader module, a pipeline cache or a pipeline: no reply body. */
static int
destroy_object(
	struct ghci_vk_session *session,
	enum ghci_vk_kind kind,
	struct i915_wire_reader *reader)
{
	struct ghci_vk_object *object;
	uint64_t identity;
	uint64_t device;

	/* The destroy form. */
	identity = ghci_vk_destroy_form(reader, &device);
	if (reader->error != 0)
		return EINVAL;

	/* The object, when the session has it (the null handle destroys nothing). */
	object = ghci_vk_object_find(session, kind, identity);
	if (object != NULL)
		ghci_vk_object_destroy(session, object);

	/* Succeeded: the object is gone. */
	return 0;
}

/* Finds a device of the session by its identity; NULL when it has none. */
static struct ghci_vk_device *
device_of(
	struct ghci_vk_session *session,
	uint64_t handle)
{
	struct ghci_vk_object *object;

	/* The device's object, and its record. */
	object = ghci_vk_object_find(session, GHCI_VK_DEVICE, handle);
	if (object == NULL)
		return NULL;

	/* Succeeded: the record. */
	return object->device;
}

/* Reports a value rounded up to a granule (a power of two). */
static uint32_t
align_up(
	uint32_t value,
	uint32_t granule)
{
	/* The next multiple. */
	return (value + granule - 1U) & ~(granule - 1U);
}

/* Stores a 64-bit value little-endian. */
static void
store64(
	uint8_t *out,
	uint64_t value)
{
	uint32_t index;

	/* Byte by byte, low first. */
	for (index = 0U; index < 8U; index++)
		out[index] = (uint8_t)(value >> (8U * index));
}

/* Loads a 32-bit value little-endian (the bits of a float field too). */
static uint32_t
load32(
	const uint8_t *in)
{
	uint32_t value;

	/* The bytes as they are. */
	kern_memcpy(&value, in, sizeof(value));
	return value;
}
