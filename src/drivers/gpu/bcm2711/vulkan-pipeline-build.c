/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Native pipeline construction validates the declared interfaces before retaining complete compiled programs and logical owners. */
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/vulkan-pipeline.h"
#include "drivers/gpu/i915/compiler/compiler.h"

static int build_pipeline(struct bcm2711_vulkan_session *session, struct bcm2711_vulkan_object *device, const VkGraphicsPipelineCreateInfo *info, struct bcm2711_vulkan_pipeline *pipeline);
static int select_modules(struct bcm2711_vulkan_session *session, struct bcm2711_vulkan_object *device, const VkGraphicsPipelineCreateInfo *info, struct bcm2711_vulkan_module **vertex, struct bcm2711_vulkan_module **fragment);
static int module_entry(const struct bcm2711_vulkan_module *module, uint32_t model);
static int fragment_key(const struct bcm2711_vulkan_module *module, struct bcm2711_shader_key *key);
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

	/* Program ownership is entirely CPU-side until native draw preparation copies the code into independently retained GPU storage. */
	(void)session;
	pipeline = payload;
	for (stage = 0; stage < 3; stage++)
		bcm2711_shader_binary_free(pipeline->programs[stage]);

	/* Every independent graph edge retires even if another edge reports a native storage uncertainty. */
	error = bcm2711_vulkan_object_release(pipeline->owner.parent);
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
	struct bcm2711_shader_key key;
	struct bcm2711_shader_diagnostic diagnostic;
	enum bcm2711_shader_stage stage;
	uint32_t index;
	int error;

	/* State validation copies exact finite fetch/raster semantics without acquiring an owner or retaining temporary pointers. */
	error = bcm2711_vulkan_pipeline_state(pipeline, info);
	if (error != 0)
		return error;
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

	/* Derive canonical fragment inputs from the read-only device-independent frontend before lowering either vertex variant. */
	error = fragment_key(fragment, &key);
	if (error != 0)
		return error;
	key.swap_red_blue = 0;
	if (pass->colour.format == VK_FORMAT_B8G8R8A8_UNORM)
		key.swap_red_blue = 1;
	key.premultiplied_blend = 0;
	if (pipeline->blend)
		key.premultiplied_blend = 1;

	/* All three native programs share an exact varying ABI and independently owned code/uniform arrays. */
	for (index = 0; index < 3; index++) {
		stage = (enum bcm2711_shader_stage)index;
		if (stage == BCM2711_SHADER_FRAGMENT)
			error = bcm2711_shader_compile(fragment->words, fragment->word_count, stage, &key, &pipeline->programs[index], &diagnostic);
		else
			error = bcm2711_shader_compile(vertex->words, vertex->word_count, stage, &key, &pipeline->programs[index], &diagnostic);
		if (error != 0)
			return error;
		error = program_interface(pipeline->programs[index], layout);
		if (error != 0)
			return error;
	}

	/* Vertex fetch declarations must supply every actual coordinate/render scalar in the compiler's canonical FIFO order. */
	error = vertex_interface(pipeline);
	if (error != 0)
		return error;

	/* Succeeded: every compiled program can be prepared from the admitted immutable pipeline interface. */
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
		if (stage->sType != VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO || stage->pNext != NULL || stage->flags != 0 || stage->pSpecializationInfo != NULL)
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
			if (module->words[offset + 1] != model || module->words[offset + 3] != 0x6e69616dU || module->words[offset + 4] != 0)
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
	struct i915_shader_ir *ir;
	struct i915_compile_diagnostic diagnostic;
	struct bcm2711_shader_component *varying;
	const struct i915_shader_ir_io *input;
	uint32_t location;
	uint32_t index;
	uint32_t component;
	int error;

	/* Only the existing read-only Zlib scalar frontend is reused; no Gen12 pipeline or instruction backend participates. */
	kern_memset(key, 0, sizeof(*key));
	ir = NULL;
	error = drv_i915_shader_parse(module->words, module->word_count, I915_STAGE_FRAGMENT, &ir, &diagnostic);
	if (error != 0)
		return error;

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
	drv_i915_shader_ir_free(ir);
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

	/* Coordinate and render vertex variants share Vulkan vertex-stage interface permission. */
	stage = VK_SHADER_STAGE_VERTEX_BIT;
	if (program->stage == BCM2711_SHADER_FRAGMENT)
		stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	for (index = 0; index < program->uniform_count; index++) {
		uniform = &program->uniforms[index];
		if (uniform->kind == BCM2711_SHADER_PUSH) {
			if ((uniform->offset & 3U) != 0 || uniform->offset >= 4U * BCM2711_VULKAN_PUSH_WORDS)
				return EINVAL;
			if ((layout->push[uniform->offset / 4U] & stage) == 0)
				return EINVAL;
			continue;
		}

		/* Constants and implicit viewport/depth words require no descriptor binding. */
		if (uniform->kind != BCM2711_SHADER_BLOCK && uniform->kind != BCM2711_SHADER_TEXTURE && uniform->kind != BCM2711_SHADER_SAMPLER)
			continue;
		if (uniform->set >= layout->count)
			return EINVAL;
		set = layout->sets[uniform->set]->payload;
		binding = NULL;
		for (candidate = 0; candidate < set->count; candidate++) {
			if (set->bindings[candidate].number == uniform->binding)
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
		if (binding->type != type)
			return EINVAL;
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

	/* Both vertex variants must consume the same canonical attributes for native state to use one fetch declaration. */
	coordinate = pipeline->programs[BCM2711_SHADER_COORDINATE];
	vertex = pipeline->programs[BCM2711_SHADER_VERTEX];
	if (coordinate->input_count != vertex->input_count)
		return EINVAL;
	for (index = 0; index < coordinate->input_count; index++) {
		input = &coordinate->inputs[index];
		if (input->location != vertex->inputs[index].location || input->component != vertex->inputs[index].component)
			return EINVAL;
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
	/* Native fetch records transfer one through four raw scalar float words per attribute. */
	if (format == VK_FORMAT_R32_SFLOAT)
		return 1;
	if (format == VK_FORMAT_R32G32_SFLOAT)
		return 2;
	if (format == VK_FORMAT_R32G32B32_SFLOAT)
		return 3;
	if (format != VK_FORMAT_R32G32B32A32_SFLOAT)
		return 0;

	/* Succeeded: this admitted attribute supplies four scalar components. */
	return 4;
}
