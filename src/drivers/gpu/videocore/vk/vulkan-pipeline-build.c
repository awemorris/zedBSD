/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Native pipeline construction validates the declared interfaces before retaining complete compiled programs and logical owners. */
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/videocore/vk/vulkan-pipeline.h"
#include "drivers/gpu/videocore/vk/vulkan-resource.h"
#include "drivers/gpu/compiler/spirv.h"

static int build_pipeline(struct bcm2711_vulkan_session *session, struct bcm2711_vulkan_object *device, const VkGraphicsPipelineCreateInfo *info, struct bcm2711_vulkan_pipeline *pipeline);
static int build_compute(struct bcm2711_vulkan_session *session, struct bcm2711_vulkan_object *device, const VkComputePipelineCreateInfo *info, struct bcm2711_vulkan_pipeline *pipeline);
static int select_modules(struct bcm2711_vulkan_session *session, struct bcm2711_vulkan_object *device, const VkGraphicsPipelineCreateInfo *info, struct bcm2711_vulkan_module **vertex, struct bcm2711_vulkan_module **fragment);
static int module_entry(const struct bcm2711_vulkan_module *module, uint32_t model);
static int fragment_key(const struct bcm2711_vulkan_module *module, struct bcm2711_shader_key *key);
static int target_key(struct bcm2711_vulkan_pipeline *pipeline, const struct bcm2711_vulkan_pass *pass, struct bcm2711_shader_key *key);
static int program_interface(const struct bcm2711_shader_binary *program, const struct bcm2711_vulkan_pipeline_layout *layout);
static int vertex_interface(const struct bcm2711_vulkan_pipeline *pipeline);
static uint32_t attribute_components(VkFormat format);

/*
 * Builds a fully owned immutable graphics pipeline from complete temporary native creation fields.
 */
int
bcm2711_vulkan_pipeline_build(
	struct bcm2711_vulkan_session *session,
	struct bcm2711_vulkan_object *device,
	const VkGraphicsPipelineCreateInfo *info,
	struct bcm2711_vulkan_pipeline **pipeline)
{
	struct bcm2711_vulkan_pipeline *created;
	int error;
	int retired;

	/* Failure never publishes a partial pipeline; ordinary allocation owns one zeroed construction graph. */
	*pipeline = NULL;
	created = kern_calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;
	error = build_pipeline(session, device, info, created);
	if (error != 0) {
		retired = bcm2711_vulkan_pipeline_release(session, created);
		if (retired != 0)
			return retired;
		return error;
	}

	/* Publish the complete compiled graph only after every interface and ownership edge is valid. */
	*pipeline = created;

	/* Succeeded: neither temporary records nor shader-module source storage are needed by this pipeline. */
	return 0;
}

/*
 * Builds a fully owned immutable compute pipeline from one complete temporary creation record.
 */
int
bcm2711_vulkan_pipeline_build_compute(
	struct bcm2711_vulkan_session *session,
	struct bcm2711_vulkan_object *device,
	const VkComputePipelineCreateInfo *info,
	struct bcm2711_vulkan_pipeline **pipeline)
{
	struct bcm2711_vulkan_pipeline *created;
	int error;
	int retired;

	/* Failure never publishes a partial pipeline. */
	*pipeline = NULL;
	created = kern_calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;
	created->compute = true;
	error = build_compute(session, device, info, created);
	if (error != 0) {
		retired = bcm2711_vulkan_pipeline_release(session, created);
		if (retired != 0)
			return retired;
		return error;
	}

	/* Publish the complete compiled program only after its interface and ownership edges are valid. */
	*pipeline = created;

	/* Succeeded: the caller publishes the compute pipeline. */
	return 0;
}

/*
 * Retires all compiled programs and immutable layout/pass/device dependencies after the final pipeline owner releases them.
 */
int
bcm2711_vulkan_pipeline_release(
	struct bcm2711_vulkan_session *session,
	void *payload)
{
	struct bcm2711_vulkan_pipeline *pipeline;
	uint32_t stage;
	int error;
	int retired;

	/*
	 * The programs, and their code in GPU storage: the pipeline is released
	 * only after every job of a command buffer naming it has retired, so no
	 * job reads the code any more.
	 */
	(void)session;
	pipeline = payload;
	error = 0;
	for (stage = 0; stage < 3; stage++) {
		bcm2711_shader_binary_free(pipeline->programs[stage]);
		if (pipeline->code[stage] != NULL) {
			retired = bcm2711_native_storage_release(pipeline->code[stage]->space, &pipeline->code[stage], true);
			if (retired != 0 && error == 0)
				error = retired;
		}
	}

	/* Every independent graph edge retires even if another edge reports a native storage uncertainty. */
	retired = bcm2711_vulkan_object_release(pipeline->owner.parent);
	if (retired != 0 && error == 0)
		error = retired;
	retired = bcm2711_vulkan_object_release(pipeline->pass);
	if (retired != 0 && error == 0)
		error = retired;
	retired = bcm2711_vulkan_object_release(pipeline->owner.device);
	if (retired != 0 && error == 0)
		error = retired;
	kern_free(pipeline);
	if (error != 0)
		return error;

	/* Succeeded: no program or logical pipeline dependency remains owned. */
	return 0;
}

/* Acquires exact parents, derives the shared varying ABI and validates all compiled draw inputs before success. */
static int
build_pipeline(
	struct bcm2711_vulkan_session *session,
	struct bcm2711_vulkan_object *device,
	const VkGraphicsPipelineCreateInfo *info,
	struct bcm2711_vulkan_pipeline *pipeline)
{
	struct bcm2711_vulkan_object *layout_object;
	struct bcm2711_vulkan_object *pass_object;
	struct bcm2711_vulkan_pipeline_layout *layout;
	struct bcm2711_vulkan_pass *pass;
	struct bcm2711_vulkan_module *vertex;
	struct bcm2711_vulkan_module *fragment;
	struct bcm2711_shader_key *key;
	struct bcm2711_shader_diagnostic diagnostic;
	enum bcm2711_shader_stage stage;
	uint32_t index;
	int error;

	/* State validation copies exact finite fetch/raster semantics without acquiring an owner or retaining temporary pointers. */
	error = bcm2711_vulkan_pipeline_state(pipeline, info);
	if (error != 0) {
		kern_logf("v3d: pipeline state refused (%d)\n", error);
		return error;
	}
	layout_object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_PIPELINE_LAYOUT, (uint64_t)(uintptr_t)info->layout);
	pass_object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_RENDER_PASS, (uint64_t)(uintptr_t)info->renderPass);
	if (layout_object == NULL || pass_object == NULL)
		return EINVAL;
	layout = layout_object->payload;
	pass = pass_object->payload;
	if (layout->owner.device != device || pass->owner.device != device)
		return EINVAL;
	error = select_modules(session, device, info, &vertex, &fragment);
	if (error != 0)
		return error;

	/* Each recorded field owns only a successfully acquired parent reference. */
	error = bcm2711_vulkan_object_retain(layout_object);
	if (error != 0)
		return error;
	pipeline->owner.parent = layout_object;
	error = bcm2711_vulkan_object_retain(pass_object);
	if (error != 0)
		return error;
	pipeline->pass = pass_object;
	error = bcm2711_vulkan_object_retain(device);
	if (error != 0)
		return error;
	pipeline->owner.device = device;

	/* Temporary interface storage remains CPU-owned and never competes with parser recursion for kernel stack space. */
	key = kern_calloc(1, sizeof(*key));
	if (key == NULL)
		return ENOMEM;

	/* Derive canonical fragment inputs from the read-only device-independent frontend before lowering either vertex variant. */
	error = fragment_key(fragment, key);
	if (error != 0) {
		kern_logf("v3d: pipeline fragment inputs refused (%d)\n", error);
		kern_free(key);
		return error;
	}

	/* The immutable key names the pass's render targets for every native variant; blending belongs to the tile buffer. */
	error = target_key(pipeline, pass, key);
	if (error != 0) {
		kern_logf("v3d: pipeline colour attachments refused (%d)\n", error);
		kern_free(key);
		return error;
	}

	/* A points pipeline passes on the point size and reads gl_PointCoord. */
	if (pipeline->topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST)
		key->points = 1;

	/* All three native programs share an exact varying ABI and independently owned code/uniform arrays. */
	for (index = 0; index < 3; index++) {
		stage = (enum bcm2711_shader_stage)index;
		diagnostic.instruction = 0;
		diagnostic.reason = NULL;
		if (stage == BCM2711_SHADER_FRAGMENT)
			error = bcm2711_shader_compile(fragment->words, fragment->word_count, stage, key, &pipeline->programs[index], &diagnostic);
		else
			error = bcm2711_shader_compile(vertex->words, vertex->word_count, stage, key, &pipeline->programs[index], &diagnostic);
		if (error != 0) {
			/* The stage and the compiler's reason, for the kernel's log. */
			kern_logf("v3d: pipeline stage %u not compiled (%d): %s at %u\n", index, error,
				  diagnostic.reason != NULL ? diagnostic.reason : "no reason", diagnostic.instruction);
			kern_free(key);
			return error;
		}

		/* Every completed variant must fit the actual retained layout before another program can be acknowledged. */
		error = program_interface(pipeline->programs[index], layout);
		if (error != 0) {
			kern_logf("v3d: pipeline stage %u does not fit its layout (%d)\n", index, error);
			kern_free(key);
			return error;
		}
	}

	/* Compiled programs contain independent interface values and retain no pointer to the temporary key. */
	kern_free(key);

	/* Vertex fetch declarations must supply every actual coordinate/render scalar in the compiler's canonical FIFO order. */
	error = vertex_interface(pipeline);
	if (error != 0) {
		kern_logf("v3d: pipeline vertex input does not match its programs (%d)\n", error);
		return error;
	}

	/* Succeeded: every compiled program can be prepared from the admitted immutable pipeline interface. */
	return 0;
}

/*
 * Describes the pass's render targets in the fragment key and gives the
 * pipeline's blending and channel masks to them.
 *
 * Fragment output n goes to colour slot n, whose attachment's blend state
 * is attachment n's: an unused slot is left out, a 32-bit float or integer
 * format takes its channels as 32-bit words, a BGRA one has red and blue
 * swapped.  Each used slot keeps the channels its attachment writes and
 * blends when its attachment does and its format blends; every other
 * target is masked.  A discarding pipeline without blend state draws no
 * fragment, so its key keeps the default.
 */
static int
target_key(
	struct bcm2711_vulkan_pipeline *pipeline,
	const struct bcm2711_vulkan_pass *pass,
	struct bcm2711_shader_key *key)
{
	struct bcm2711_vulkan_colour_target colour;
	uint32_t masks;
	uint32_t slot;
	int error;

	/*
	 * The attachments the blend state names are the subpass's colour slots,
	 * rasterized with the pass's samples; a pass without attachments takes
	 * the pipeline's.
	 */
	if (!pipeline->discard && pipeline->colour_count != pass->colour_count)
		return EINVAL;
	if (pipeline->samples != 0 && pass->samples != 0 && pipeline->samples != pass->samples)
		return EINVAL;
	if (pass->colour_count > BCM2711_SHADER_TARGETS)
		return ENOTSUP;

	/* Each slot: its kind of write and its mask bits. */
	masks = pipeline->colour_skip;
	pipeline->colour_skip = 0;
	pipeline->blend_targets = 0;
	key->target_count = pass->colour_count;
	for (slot = 0; slot < BCM2711_SHADER_TARGETS; slot++) {
		/* An unused slot, or none at all, is masked. */
		if (slot >= pass->colour_count || !pass->colour_used[slot] || slot >= pipeline->colour_count) {
			pipeline->colour_skip |= 0xfU << (slot * 4U);
			if (slot < pass->colour_count)
				key->target_unused |= 1U << slot;
			continue;
		}

		/* The slot's format decides how the program writes it and whether it blends. */
		error = bcm2711_vulkan_colour_target(pass->colours[slot].format, &colour);
		if (error != 0)
			return error;
		pipeline->colour_skip |= masks & (0xfU << (slot * 4U));
		if ((pipeline->blend & (1U << slot)) != 0 && colour.blend)
			pipeline->blend_targets |= 1U << slot;
		if (colour.wide) {
			key->target_wide |= 1U << slot;
			key->target_components[slot] = colour.components;
		}
		if (colour.swap)
			key->swap_red_blue |= 1U << slot;
	}

	/* Succeeded: the key and the masks follow the pass's slots. */
	return 0;
}

/*
 * Acquires the layout and device of a compute pipeline, compiles its one
 * GLCompute module and checks the program against the layout.
 */
static int
build_compute(
	struct bcm2711_vulkan_session *session,
	struct bcm2711_vulkan_object *device,
	const VkComputePipelineCreateInfo *info,
	struct bcm2711_vulkan_pipeline *pipeline)
{
	struct bcm2711_vulkan_object *layout_object;
	struct bcm2711_vulkan_object *module_object;
	struct bcm2711_vulkan_pipeline_layout *layout;
	struct bcm2711_vulkan_module *module;
	struct bcm2711_shader_key *key;
	struct bcm2711_shader_diagnostic diagnostic;
	const struct bcm2711_shader_binary *program;
	enum bcm2711_shader_uniform_kind kind;
	uint32_t index;
	int error;

	/* No derivative or chained pipeline, and one compute stage. */
	if (info->flags != 0 || info->basePipelineHandle != VK_NULL_HANDLE)
		return ENOTSUP;
	if (info->stage.stage != VK_SHADER_STAGE_COMPUTE_BIT)
		return EINVAL;

	/* The same-device layout and module; the module's single entry is main of the GLCompute model (5). */
	layout_object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_PIPELINE_LAYOUT, (uint64_t)(uintptr_t)info->layout);
	module_object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_SHADER_MODULE, (uint64_t)(uintptr_t)info->stage.module);
	if (layout_object == NULL || module_object == NULL)
		return EINVAL;
	layout = layout_object->payload;
	module = module_object->payload;
	if (layout->owner.device != device || module->owner.device != device)
		return EINVAL;
	error = module_entry(module, 5);
	if (error != 0)
		return error;

	/* Each recorded field owns only a successfully acquired parent reference. */
	error = bcm2711_vulkan_object_retain(layout_object);
	if (error != 0)
		return error;
	pipeline->owner.parent = layout_object;
	error = bcm2711_vulkan_object_retain(device);
	if (error != 0)
		return error;
	pipeline->owner.device = device;

	/* A compute program has no varyings; its zeroed key lives on the heap like a graphics key. */
	key = kern_calloc(1, sizeof(*key));
	if (key == NULL)
		return ENOMEM;
	kern_memset(&diagnostic, 0, sizeof(diagnostic));
	error = bcm2711_shader_compile(module->words, module->word_count, BCM2711_SHADER_COMPUTE, key, &pipeline->programs[0], &diagnostic);
	kern_free(key);
	if (error != 0)
		return error;

	/* Every uniform the program reads has a binding of its type visible to the compute stage. */
	error = program_interface(pipeline->programs[0], layout);
	if (error != 0)
		return error;

	/* A dispatch supplies storage buffers and images, uniform blocks, push words and its own words; sampling has no compute path. */
	program = pipeline->programs[0];
	for (index = 0; index < program->uniform_count; index++) {
		kind = program->uniforms[index].kind;
		if (kind == BCM2711_SHADER_TEXTURE ||
		    kind == BCM2711_SHADER_SAMPLER ||
		    kind == BCM2711_SHADER_TEXTURE_SIZE ||
		    kind == BCM2711_SHADER_VIEWPORT_X ||
		    kind == BCM2711_SHADER_VIEWPORT_Y ||
		    kind == BCM2711_SHADER_VIEWPORT_Z ||
		    kind == BCM2711_SHADER_DEPTH_OFFSET)
			return ENOTSUP;
	}

	/* Succeeded: the program can be dispatched with the layout's descriptors. */
	return 0;
}

/* Selects exactly one same-device vertex and fragment module with the supported single main entry point and no specialization. */
static int
select_modules(
	struct bcm2711_vulkan_session *session,
	struct bcm2711_vulkan_object *device,
	const VkGraphicsPipelineCreateInfo *info,
	struct bcm2711_vulkan_module **vertex,
	struct bcm2711_vulkan_module **fragment)
{
	const VkPipelineShaderStageCreateInfo *stage;
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_module *module;
	uint32_t model;
	uint32_t index;
	int error;
	int name;

	/* Duplicate or unsupported stages cannot overwrite the module chosen by an earlier declaration. */
	*vertex = NULL;
	*fragment = NULL;
	if (info->stageCount != 2 || info->pStages == NULL)
		return ENOTSUP;
	for (index = 0; index < info->stageCount; index++) {
		stage = &info->pStages[index];
		if (stage->sType != VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO ||
		    stage->pNext != NULL ||
		    stage->flags != 0 ||
		    stage->pSpecializationInfo != NULL)
			return ENOTSUP;
		if (stage->pName == NULL)
			return EINVAL;
		name = kern_strcmp(stage->pName, "main");
		if (name != 0)
			return ENOTSUP;
		object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_SHADER_MODULE, (uint64_t)(uintptr_t)stage->module);
		if (object == NULL)
			return EINVAL;
		module = object->payload;
		if (module->owner.device != device)
			return EINVAL;
		model = 0;
		if (stage->stage == VK_SHADER_STAGE_VERTEX_BIT) {
			if (*vertex != NULL)
				return EINVAL;
			*vertex = module;
		} else if (stage->stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
			if (*fragment != NULL)
				return EINVAL;
			*fragment = module;
			model = 4;
		} else {
			return ENOTSUP;
		}

		/* The frontend identifies execution models but does not select arbitrary named entries; make that finite contract explicit. */
		error = module_entry(module, model);
		if (error != 0)
			return error;
	}

	/* Both native stage roles are necessary for the admitted rasterizing graphics pipeline. */
	if (*vertex == NULL || *fragment == NULL)
		return EINVAL;

	/* Succeeded: these borrowed source modules remain stable under the controller mutex during compilation. */
	return 0;
}

/* Validates one matching main execution model so the device-independent parser cannot select another entry implicitly. */
static int
module_entry(
	const struct bcm2711_vulkan_module *module,
	uint32_t model)
{
	uint32_t offset;
	uint32_t length;
	uint32_t opcode;
	uint32_t entries;

	/* Walk complete SPIR-V instructions; only the exact main string and one entry model are admitted. */
	entries = 0;
	offset = 5;
	while (offset < module->word_count) {
		length = module->words[offset] >> 16;
		opcode = module->words[offset] & 0xffffU;
		if (length == 0 || length > module->word_count - offset)
			return EINVAL;
		if (opcode == 15U) {
			if (entries != 0 || length < 5)
				return ENOTSUP;
			if (module->words[offset + 1] != model ||
			    module->words[offset + 3] != 0x6e69616dU ||
			    module->words[offset + 4] != 0)
				return ENOTSUP;
			entries++;
		}

		/* Complete declared instruction lengths define the next boundary without a speculative word read. */
		offset += length;
	}

	/* Missing entry point is never treated as a usable default stage. */
	if (entries != 1)
		return EINVAL;

	/* Succeeded: the only entry point exactly matches the selected pipeline stage. */
	return 0;
}

/* Derives stable scalar varying order from actual fragment declarations, independent of their source order. */
static int
fragment_key(
	const struct bcm2711_vulkan_module *module,
	struct bcm2711_shader_key *key)
{
	struct drv_gpu_shader_ir *ir;
	struct drv_gpu_compile_diagnostic diagnostic;
	struct bcm2711_shader_component *varying;
	const struct drv_gpu_shader_ir_io *input;
	uint32_t location;
	uint32_t index;
	uint32_t component;
	int error;

	/* The preliminary varying pass obeys the same stage and finite constant graph as all native compilation entry points. */
	error = bcm2711_shader_preflight(module->words, module->word_count, BCM2711_SHADER_FRAGMENT, NULL);
	if (error != 0)
		return error;

	/* Only the existing read-only Zlib scalar frontend is reused; no Gen12 pipeline or instruction backend participates. */
	kern_memset(key, 0, sizeof(*key));
	ir = NULL;
	diagnostic.reason = NULL;
	diagnostic.word_offset = 0;
	error = drv_gpu_shader_parse_features(module->words, module->word_count, DRV_GPU_STAGE_FRAGMENT, BCM2711_SHADER_SPIRV_FEATURES, &ir, &diagnostic);
	if (error != 0) {
		kern_logf("v3d: fragment program not parsed (%d): %s at %u\n", error,
			  diagnostic.reason != NULL ? diagnostic.reason : "no reason", diagnostic.word_offset);
		return error;
	}

	/* Native compiler validation supplies duplicate/qualifier checks after this finite canonical key is formed. */
	error = 0;
	for (location = 0; location < 16 && error == 0; location++) {
		for (index = 0; index < ir->input_count && error == 0; index++) {
			input = &ir->inputs[index];
			if (input->location != location)
				continue;
			if (input->components == 0 || input->components > 4) {
				error = ENOTSUP;
				break;
			}

			/* Each declared component has one exact interpolation identity shared by all three emitted programs. */
			for (component = 0; component < input->components; component++) {
				if (key->varying_count == BCM2711_SHADER_INTERFACE_WORDS) {
					error = E2BIG;
					break;
				}

				/* The key stores immutable scalar metadata rather than pointers into source IR. */
				varying = &key->varyings[key->varying_count];
				varying->location = location;
				varying->component = component;
				varying->flat = input->flat;
				varying->noperspective = input->noperspective;
				key->varying_count++;
			}
		}
	}

	/* Temporary source IR retires on every finite-interface outcome. */
	drv_gpu_shader_ir_free(ir);
	if (error != 0)
		return error;

	/* Succeeded: canonical varying metadata is independently owned by the key. */
	return 0;
}

/* Checks every emitted draw-time uniform against actual set binding types, stage visibility and exact push-word permissions. */
static int
program_interface(
	const struct bcm2711_shader_binary *program,
	const struct bcm2711_vulkan_pipeline_layout *layout)
{
	const struct bcm2711_shader_uniform *uniform;
	const struct bcm2711_vulkan_set_layout *set;
	const struct bcm2711_vulkan_binding_layout *binding;
	VkShaderStageFlags stage;
	VkDescriptorType type;
	uint32_t index;
	uint32_t candidate;
	int error;

	/* Coordinate and render vertex variants share Vulkan vertex-stage interface permission. */
	stage = VK_SHADER_STAGE_VERTEX_BIT;
	if (program->stage == BCM2711_SHADER_FRAGMENT)
		stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	else if (program->stage == BCM2711_SHADER_COMPUTE)
		stage = VK_SHADER_STAGE_COMPUTE_BIT;
	for (index = 0; index < program->uniform_count; index++) {
		uniform = &program->uniforms[index];
		if (uniform->kind == BCM2711_SHADER_PUSH) {
			if ((uniform->offset & 3U) != 0 || uniform->offset >= 4U * BCM2711_VULKAN_PUSH_WORDS)
				return EINVAL;
			if ((layout->push[uniform->offset / 4U] & stage) == 0)
				return EINVAL;
			continue;
		}

		/* Constants, implicit viewport/depth words and a dispatch's own words require no descriptor binding. */
		if (uniform->kind != BCM2711_SHADER_BLOCK &&
		    uniform->kind != BCM2711_SHADER_TEXTURE &&
		    uniform->kind != BCM2711_SHADER_SAMPLER &&
		    uniform->kind != BCM2711_SHADER_TEXTURE_SIZE &&
		    uniform->kind != BCM2711_SHADER_STORAGE_BASE &&
		    uniform->kind != BCM2711_SHADER_STORAGE_LIMIT &&
		    uniform->kind != BCM2711_SHADER_STORAGE_SIZE &&
		    uniform->kind != BCM2711_SHADER_IMAGE_BASE &&
		    uniform->kind != BCM2711_SHADER_IMAGE_LIMIT &&
		    uniform->kind != BCM2711_SHADER_IMAGE_PITCH &&
		    uniform->kind != BCM2711_SHADER_IMAGE_LAYER &&
		    uniform->kind != BCM2711_SHADER_IMAGE_SIZE)
			continue;
		if (uniform->set >= layout->count)
			return EINVAL;
		set = layout->sets[uniform->set]->payload;
		binding = NULL;
		for (candidate = 0; candidate < set->count; candidate++) {
			if (set->bindings[candidate].number == uniform->binding && set->bindings[candidate].element == uniform->element)
				binding = &set->bindings[candidate];
		}

		/* A missing or stage-invisible declaration cannot be supplied by a later mutable descriptor update. */
		if (binding == NULL)
			return EINVAL;
		if ((binding->stages & stage) == 0)
			return EINVAL;
		type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		if (uniform->kind == BCM2711_SHADER_BLOCK)
			type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		else if (uniform->kind >= BCM2711_SHADER_IMAGE_BASE && uniform->kind <= BCM2711_SHADER_IMAGE_SIZE)
			type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		else if (uniform->kind != BCM2711_SHADER_TEXTURE && uniform->kind != BCM2711_SHADER_SAMPLER &&
			 uniform->kind != BCM2711_SHADER_TEXTURE_SIZE)
			type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

		/* The declaration supplies the kind the shader reads (a uniform block from a uniform buffer, dynamic or not). */
		error = bcm2711_vulkan_layout_type_matches(binding->type, type);
		if (error != 0)
			return error;
		if (uniform->kind == BCM2711_SHADER_BLOCK && (uniform->offset & 3U) != 0)
			return EINVAL;
	}

	/* Succeeded: all actual uniform transactions have a compatible immutable interface. */
	return 0;
}

/* Checks exact vertex FIFO words against admitted float attribute declarations before a draw can create native fetch records. */
static int
vertex_interface(
	const struct bcm2711_vulkan_pipeline *pipeline)
{
	const struct bcm2711_shader_binary *coordinate;
	const struct bcm2711_shader_binary *vertex;
	const struct bcm2711_shader_component *input;
	const VkVertexInputAttributeDescription *attribute;
	uint32_t index;
	uint32_t candidate;
	uint32_t components;

	/*
	 * The coordinate variant reads a subset of the vertex variant's
	 * canonical scalars (the leading components of its locations), so one
	 * fetch declaration serves both.
	 */
	coordinate = pipeline->programs[BCM2711_SHADER_COORDINATE];
	vertex = pipeline->programs[BCM2711_SHADER_VERTEX];
	if (coordinate->input_count > vertex->input_count)
		return EINVAL;
	for (index = 0; index < coordinate->input_count; index++) {
		input = &coordinate->inputs[index];
		for (candidate = 0; candidate < vertex->input_count; candidate++) {
			if (vertex->inputs[candidate].location == input->location && vertex->inputs[candidate].component == input->component)
				break;
		}
		if (candidate == vertex->input_count)
			return EINVAL;
	}
	for (index = 0; index < vertex->input_count; index++) {
		input = &vertex->inputs[index];
		attribute = NULL;
		for (candidate = 0; candidate < pipeline->attribute_count; candidate++) {
			if (pipeline->attributes[candidate].location == input->location)
				attribute = &pipeline->attributes[candidate];
		}

		/* Every consumed scalar requires a declared float source; native FIFO packing fills missing format components with zero/one. */
		if (attribute == NULL)
			return ENOTSUP;
		components = attribute_components(attribute->format);
		if (components == 0 || input->component >= 4)
			return ENOTSUP;
	}

	/* Succeeded: later checked draw preparation can form the exact declared fetch intervals. */
	return 0;
}

/* Classifies the admitted contiguous 32-bit float vertex formats without performing kernel floating-point arithmetic. */
static uint32_t
attribute_components(
	VkFormat format)
{
	/* Native fetch records transfer one through four raw scalar words per attribute. */
	return bcm2711_vulkan_attribute_components(format);
}
