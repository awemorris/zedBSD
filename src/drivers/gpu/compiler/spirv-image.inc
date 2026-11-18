/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The storage images, storage texel buffers and input attachments of the
 * SPIR-V parser, and OpImageQuerySamples, included by spirv.c.  Only a
 * parse with the GHCI features lowers them (DRV_GPU_SPIRV_FEATURE_*).
 *
 * A storage image (an image type with Sampled 2) and a storage texel buffer
 * (Dim Buffer, Sampled 2) become uniforms of their own kind with the VkFormat
 * their Image Format names; an input attachment (Dim SubpassData) a uniform
 * with its InputAttachmentIndex, its format being the render pass's.
 * OpImageRead and OpImageWrite become IMAGE_READ and IMAGE_WRITE on three
 * integer parameters: x, y and the layer (or a 3D image's z) of a storage
 * image, zero where the image has none; dx, dy and the sample of an input
 * attachment.  A write is predicated by its block, and a compute shader's
 * read too, as the storage buffers' accesses are.
 *
 * The file is part of spirv.c's translation unit: it uses that file's types,
 * constants and helpers, and its functions are declared with the others at
 * the top of spirv.c.
 */

/*
 * Reports the VkFormat of a storage image's SPIR-V Image Format: the
 * formats Vulkan 1.0 requires for storage images without
 * shaderStorageImageExtendedFormats; 0 for any other.
 *
 * Values referenced: SPIR-V specification 1.6, section 3.11 "Image Format"
 * (Rgba32f 1, Rgba16f 2, R32f 3, Rgba8 4, Rgba8Snorm 5, Rg32f 6, Rgba32i
 * 21, Rgba16i 22, Rgba8i 23, R32i 24, Rg32i 25, Rgba32ui 30, Rgba16ui 31,
 * Rgba8ui 32, R32ui 33, Rg32ui 35), and the Vulkan specification, VkFormat
 * (the values of the formats named).
 */
static uint32_t
drv_gpu_spirv_storage_format(
	uint32_t image_format)
{
	/* Each Image Format Vulkan requires for storage images. */
	switch (image_format) {
	case 1U:
		return 109U;
	case 2U:
		return 97U;
	case 3U:
		return 100U;
	case 4U:
		return 37U;
	case 5U:
		return 38U;
	case 6U:
		return 103U;
	case 21U:
		return 108U;
	case 22U:
		return 96U;
	case 23U:
		return 42U;
	case 24U:
		return 99U;
	case 25U:
		return 102U;
	case 30U:
		return 107U;
	case 31U:
		return 95U;
	case 32U:
		return 41U;
	case 33U:
		return 98U;
	case 35U:
		return 101U;
	default:
		return 0U;
	}
}

/*
 * Adds the uniform of a UniformConstant variable: a sampled image, or with
 * the GHCI features a storage image, a storage texel buffer or an input
 * attachment by its image type.  Refuses a storage image of a format the
 * parse does not lower and a multisampled storage image.
 */
static int
drv_gpu_spirv_add_constant_uniform(
	struct drv_gpu_spirv_parser *parser,
	uint32_t id,
	uint32_t opcode,
	uint32_t offset)
{
	struct drv_gpu_shader_ir_uniform *uniform;
	struct drv_gpu_spirv_id *variable;
	struct drv_gpu_spirv_id *type;
	uint32_t kind;
	uint32_t format;
	int image_array;
	int error;

	/* Resolves the variable's pointee. */
	variable = &parser->ids[id];
	type = drv_gpu_spirv_id(parser, variable->pointee);

	/* An array of storage images is one uniform per element when the backend binds them. */
	image_array = drv_gpu_spirv_is_image_array(parser, type);
	if (image_array) {
		error = drv_gpu_spirv_add_image_array(parser, id, type, opcode, offset);
		return error;
	}

	/*
	 * An array of combined image samplers is one uniform per element when
	 * the backend binds them; otherwise one uniform, as before, and a use
	 * of an element is refused.
	 */
	if (type != NULL && type->kind == ID_TYPE_ARRAY &&
	    (parser->features & DRV_GPU_SPIRV_FEATURE_SAMPLER_ARRAYS) != 0U) {
		error = drv_gpu_spirv_add_sampler_array(parser, id, type, opcode, offset);
		return error;
	}

	/* A sampled image unless the features say otherwise. */
	kind = DRV_GPU_IR_UNIFORM_SAMPLED_IMAGE;
	format = 0U;
	if (type != NULL && type->kind == ID_TYPE_IMAGE) {
		if (type->image_dim == DIM_SUBPASS_DATA &&
		    (parser->features & DRV_GPU_SPIRV_FEATURE_INPUT_ATTACHMENT) != 0U) {
			kind = DRV_GPU_IR_UNIFORM_INPUT_ATTACHMENT;
		} else if (type->image_sampled == IMAGE_SAMPLED_STORAGE &&
			   (parser->features & DRV_GPU_SPIRV_FEATURE_STORAGE_IMAGE) != 0U) {
			kind = DRV_GPU_IR_UNIFORM_STORAGE_IMAGE;
			if (type->image_dim == DIM_BUFFER)
				kind = DRV_GPU_IR_UNIFORM_TEXEL_BUFFER;
			format = drv_gpu_spirv_storage_format(type->image_format);
		}
	}

	/* A storage image needs a format the parse knows. */
	if ((kind == DRV_GPU_IR_UNIFORM_STORAGE_IMAGE || kind == DRV_GPU_IR_UNIFORM_TEXEL_BUFFER) &&
	    format == 0U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "storage image of a format that is not lowered");
		return error;
	}

	/* A storage image is not multisampled (shaderStorageImageMultisample is not offered). */
	if (kind == DRV_GPU_IR_UNIFORM_STORAGE_IMAGE && type->image_ms != 0U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "multisampled storage image");
		return error;
	}

	/* The uniform, with what its kind needs. */
	uniform = &parser->ir->uniforms[parser->ir->uniform_count];
	drv_gpu_spirv_add_uniform(parser, id, kind);
	uniform->format = format;
	if (kind == DRV_GPU_IR_UNIFORM_INPUT_ATTACHMENT) {
		if (variable->input_attachment_index == 0U) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "input attachment without its index");
			return error;
		}

		/* The attachment of the subpass's input list, and whether it has samples. */
		uniform->input_attachment_index = variable->input_attachment_index - 1U;
		uniform->multisampled = type->image_ms;
	}

	/* Succeeded: the uniform is listed. */
	return 0;
}

/*
 * Finds the uniform of the storage image, texel buffer or input attachment
 * an image (a loaded one, or a pointer to one) names: an element of an
 * array of storage images named by a constant has its own, any other the
 * one of its set and binding.  An element chosen at run time has none
 * here: drv_gpu_spirv_lower_elements() lowers its access once for each
 * element, each named by a constant.  UINT32_MAX for none.
 */
static uint32_t
drv_gpu_spirv_resource_image_uniform(
	struct drv_gpu_spirv_parser *parser,
	const struct drv_gpu_spirv_id *image)
{
	/* An image that is no element of an array: by its set and binding. */
	if (image->descriptor_count == 0U)
		return drv_gpu_spirv_image_uniform(parser, image->set, image->binding);

	/* An element named by a constant: its own storage image uniform. */
	if (image->descriptor_chosen == 0U || image->descriptor_dynamic != 0U)
		return UINT32_MAX;
	if (image->uniform >= parser->ir->uniform_count)
		return UINT32_MAX;
	if (parser->ir->uniforms[image->uniform].kind != DRV_GPU_IR_UNIFORM_STORAGE_IMAGE)
		return UINT32_MAX;

	/* Succeeded: the element's uniform. */
	return image->uniform;
}

/*
 * Finds the uniform of a storage image, texel buffer or input attachment
 * by its set and binding; UINT32_MAX for none.
 */
static uint32_t
drv_gpu_spirv_image_uniform(
	struct drv_gpu_spirv_parser *parser,
	uint32_t set,
	uint32_t binding)
{
	const struct drv_gpu_shader_ir_uniform *uniform;
	uint32_t index;

	/* Each uniform of the image kinds. */
	for (index = 0U; index < parser->ir->uniform_count; index++) {
		uniform = &parser->ir->uniforms[index];
		if (uniform->set != set)
			continue;
		if (uniform->binding != binding)
			continue;
		if (uniform->kind == DRV_GPU_IR_UNIFORM_STORAGE_IMAGE)
			return index;
		if (uniform->kind == DRV_GPU_IR_UNIFORM_TEXEL_BUFFER)
			return index;
		if (uniform->kind == DRV_GPU_IR_UNIFORM_INPUT_ATTACHMENT)
			return index;
	}

	/* No image uniform has the binding. */
	return UINT32_MAX;
}

/*
 * Copies values into a run of fresh consecutive values (MOVEs) and returns
 * the first.
 */
static uint32_t
drv_gpu_spirv_consecutive(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *values,
	uint32_t count)
{
	uint32_t first;
	uint32_t value;
	uint32_t index;

	/* The run starts at the next value. */
	first = parser->ir->value_count;

	/* Copies each value into the next one of the run. */
	for (index = 0U; index < count; index++) {
		value = drv_gpu_spirv_new_value(parser);
		(void)drv_gpu_spirv_emit(parser,
					 DRV_GPU_IR_MOVE,
					 value,
					 values[index],
					 0U);
	}

	/* Succeeded: the run's first value. */
	return first;
}

/*
 * Reads the three integer parameters of an image access: x, y and the
 * layer (or z) of a storage image from its coordinate, or dx, dy and the
 * sample of an input attachment from its offset and its Sample operand.
 * `operands` is the index of the image operands' mask word (count when
 * there is none).
 */
static int
drv_gpu_spirv_image_params(
	struct drv_gpu_spirv_parser *parser,
	const struct drv_gpu_spirv_id *type,
	uint32_t coordinate_id,
	const uint32_t *word,
	uint32_t count,
	uint32_t operands,
	uint32_t *params,
	uint32_t opcode,
	uint32_t offset)
{
	uint32_t coordinate[MAX_COMPONENTS];
	uint32_t scalars[MAX_COMPONENTS];
	uint32_t coordinate_count;
	uint32_t expected;
	uint32_t mask;
	uint32_t zero;
	uint32_t index;
	int error;

	/* The coordinate the image's Dim and Arrayed give: one to three integers. */
	coordinate_count = drv_gpu_spirv_operand(parser, coordinate_id, coordinate);
	expected = 2U;
	if (type->image_dim == DIM_1D || type->image_dim == DIM_BUFFER) {
		expected = 1U;
	} else if (type->image_dim == DIM_3D || type->image_dim == DIM_CUBE) {
		expected = 3U;
	}
	if (type->image_arrayed != 0U && type->image_dim != DIM_CUBE)
		expected++;
	if (coordinate_count != expected || expected > 3U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "image access at a coordinate that is not the image's");
		return error;
	}

	/* x, then y (zero for a 1D image or a buffer), then the layer or z (zero for none). */
	zero = drv_gpu_spirv_integer_constant(parser, 0U);
	params[0] = coordinate[0];
	params[1] = zero;
	params[2] = zero;
	if (type->image_dim == DIM_1D && coordinate_count == 2U) {
		params[2] = coordinate[1];
	} else if (coordinate_count >= 2U && type->image_dim != DIM_1D) {
		params[1] = coordinate[1];
		if (coordinate_count == 3U)
			params[2] = coordinate[2];
	}

	/* The image operands: an input attachment's Sample, nothing else. */
	mask = 0U;
	if (operands < count)
		mask = word[operands];
	if ((mask & ~IMAGE_OPERAND_SAMPLE) != 0U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "image access with image operands other than Sample");
		return error;
	}

	/* A sample is an input attachment's third parameter (a storage image has no samples). */
	if ((mask & IMAGE_OPERAND_SAMPLE) != 0U) {
		if (type->image_dim != DIM_SUBPASS_DATA || type->image_ms == 0U)
			return EINVAL;
		if (operands + 1U >= count)
			return EINVAL;
		index = drv_gpu_spirv_operand(parser, word[operands + 1U], scalars);
		if (index != 1U)
			return EINVAL;
		params[2] = scalars[0];
	}

	/* Succeeded: the three parameters. */
	return 0;
}

/*
 * Lowers OpImageRead of a storage image, a storage texel buffer or an input
 * attachment (subpassLoad).
 */
static int
drv_gpu_spirv_lower_image_read(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct drv_gpu_shader_ir_inst *inst;
	struct drv_gpu_spirv_id *image;
	struct drv_gpu_spirv_id *type;
	struct drv_gpu_spirv_id *record;
	uint32_t params[3];
	uint32_t components;
	uint32_t uniform;
	uint32_t first_param;
	uint32_t first;
	uint32_t index;
	int error;

	/* The result type, the result, the image and the coordinate. */
	if (count < 5U)
		return EINVAL;

	/* Resolves the image and its uniform. */
	image = drv_gpu_spirv_id(parser, word[3]);
	type = drv_gpu_spirv_image_type(parser, image);
	uniform = UINT32_MAX;
	if (type != NULL)
		uniform = drv_gpu_spirv_resource_image_uniform(parser, image);
	if (uniform == UINT32_MAX) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "image read of something that is not a storage image or an "
		    "input attachment");
		return error;
	}

	/* An input attachment is read by a fragment shader only. */
	if (type->image_dim == DIM_SUBPASS_DATA && parser->ir->stage != DRV_GPU_STAGE_FRAGMENT)
		return EINVAL;

	/* The three parameters. */
	error = drv_gpu_spirv_image_params(parser,
					   type,
					   word[4],
					   word,
					   count,
					   5U,
					   params,
					   opcode,
					   offset);
	if (error != 0)
		return error;

	/* The result is up to four floats or integers. */
	components = drv_gpu_spirv_value_components(parser, word[1]);
	if (components == 0U || components > 4U)
		return EINVAL;

	/* The parameters in consecutive values, then four consecutive results. */
	first_param = drv_gpu_spirv_consecutive(parser, params, 3U);
	first = parser->ir->value_count;
	for (index = 0U; index < 4U; index++)
		(void)drv_gpu_spirv_new_value(parser);

	/* Emits the read: the uniform in `location`, predicated in a compute shader's block. */
	inst = drv_gpu_spirv_emit(parser,
				  DRV_GPU_IR_IMAGE_READ,
				  first,
				  first_param,
				  3U);
	if (inst != NULL) {
		inst->location = uniform;
		inst->image_shape = drv_gpu_spirv_image_shape(type);
		if (parser->ir->stage == DRV_GPU_STAGE_COMPUTE &&
		    parser->predicate != PREDICATE_ALWAYS) {
			inst->src[2] = parser->predicate;
			inst->component = 1U;
		}
	}

	/* The result names the read's first components. */
	record = drv_gpu_spirv_result(parser, word[2], word[1], components, 0);
	if (record == NULL)
		return EINVAL;
	for (index = 0U; index < components; index++)
		record->comp[index] = first + index;

	/* Succeeded unless the emit failed, which the parser latched. */
	return parser->error;
}

/*
 * Lowers OpImageWrite of a storage image or a storage texel buffer: the
 * texel's components, the missing ones zero, written under the block's
 * predicate.
 */
static int
drv_gpu_spirv_lower_image_write(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct drv_gpu_shader_ir_inst *inst;
	struct drv_gpu_spirv_id *image;
	struct drv_gpu_spirv_id *type;
	uint32_t texel[MAX_COMPONENTS];
	uint32_t values[4];
	uint32_t params[3];
	uint32_t components;
	uint32_t uniform;
	uint32_t first_param;
	uint32_t first_value;
	uint32_t index;
	int error;

	/* The image, the coordinate and the texel. */
	if (count < 4U)
		return EINVAL;

	/* Resolves the image and its uniform. */
	image = drv_gpu_spirv_id(parser, word[1]);
	type = drv_gpu_spirv_image_type(parser, image);
	uniform = UINT32_MAX;
	if (type != NULL)
		uniform = drv_gpu_spirv_resource_image_uniform(parser, image);
	if (uniform == UINT32_MAX || type->image_dim == DIM_SUBPASS_DATA) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "image write of something that is not a storage image");
		return error;
	}

	/* The three parameters. */
	error = drv_gpu_spirv_image_params(parser,
					   type,
					   word[2],
					   word,
					   count,
					   4U,
					   params,
					   opcode,
					   offset);
	if (error != 0)
		return error;

	/* The texel, padded with zeros to four components. */
	components = drv_gpu_spirv_operand(parser, word[3], texel);
	if (components == 0U || components > 4U)
		return EINVAL;
	for (index = 0U; index < 4U; index++) {
		if (index < components) {
			values[index] = texel[index];
		} else {
			values[index] = drv_gpu_spirv_integer_constant(parser, 0U);
		}
	}

	/* The parameters and the texel in consecutive values. */
	first_param = drv_gpu_spirv_consecutive(parser, params, 3U);
	first_value = drv_gpu_spirv_consecutive(parser, values, 4U);

	/* Emits the write: the uniform in `location`, predicated by the block. */
	inst = drv_gpu_spirv_emit(parser,
				  DRV_GPU_IR_IMAGE_WRITE,
				  0U,
				  first_param,
				  3U);
	if (inst != NULL) {
		inst->src[2] = first_value;
		inst->location = uniform;
		inst->image_shape = drv_gpu_spirv_image_shape(type);
		if (parser->predicate != PREDICATE_ALWAYS) {
			inst->src[3] = parser->predicate;
			inst->component = 1U;
		}
	}

	/* Succeeded unless the emit failed, which the parser latched. */
	return parser->error;
}

/*
 * Lowers OpImageQuerySamples (textureSamples()): the samples message on a
 * multisampled sampled image, whose reply's first value is the count.
 */
static int
drv_gpu_spirv_lower_query_samples(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct drv_gpu_spirv_id *image;
	struct drv_gpu_spirv_id *type;
	struct drv_gpu_spirv_id *record;
	uint32_t params[1];
	uint32_t shape;
	uint32_t first;
	int error;

	/* The result type, the result and the image. */
	if (count != 4U)
		return EINVAL;

	/* Resolves the image: a multisampled 2D one. */
	image = drv_gpu_spirv_id(parser, word[3]);
	type = drv_gpu_spirv_image_type(parser, image);
	if (type == NULL || type->image_ms == 0U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "sample query of an image that is not multisampled");
		return error;
	}

	/* The message: its one parameter zero. */
	params[0] = drv_gpu_spirv_integer_constant(parser, 0U);
	shape = drv_gpu_spirv_image_shape(type);
	error = drv_gpu_spirv_emit_texture(parser,
					   image,
					   shape,
					   params,
					   1U,
					   DRV_GPU_IR_TEXTURE_SAMPLES,
					   0U,
					   &first);
	if (error != 0)
		return error;

	/* The result is the reply's first value. */
	record = drv_gpu_spirv_result(parser, word[2], word[1], 1U, 0);
	if (record == NULL)
		return EINVAL;
	record->comp[0] = first;

	/* Succeeded: the query is lowered. */
	return 0;
}

/*
 * Lowers OpImageTexelPointer of a storage image or texel buffer of a
 * 32-bit integer format: the result names the texel (x, y and the layer in
 * three consecutive values) for the atomic that follows.
 */
static int
drv_gpu_spirv_lower_texel_pointer(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct drv_gpu_spirv_id *variable;
	struct drv_gpu_spirv_id *record;
	struct drv_gpu_spirv_id *pointer_type;
	struct drv_gpu_spirv_id *type;
	uint32_t params[3];
	uint32_t uniform;
	uint32_t format;
	int dynamic;
	int error;

	/* The result type, the result, the image's variable, the coordinate and the sample. */
	if (count != 6U)
		return EINVAL;

	/* The image's variable, or an element of an array of them, and its type. */
	variable = drv_gpu_spirv_id(parser, word[3]);
	if (variable == NULL ||
	    (variable->kind != ID_VARIABLE && variable->kind != ID_POINTER) ||
	    variable->ptr_kind != PTR_SAMPLER)
		return EINVAL;
	type = drv_gpu_spirv_id(parser, variable->pointee);
	if (type == NULL || type->kind != ID_TYPE_IMAGE)
		return EINVAL;

	/*
	 * The image's uniform: a storage image or texel buffer of R32_UINT or
	 * R32_SINT.  An element chosen at run time names the first element's
	 * for now, and the atomic through the pointer is lowered once for each
	 * element (drv_gpu_spirv_lower_elements()); the elements share the
	 * format.
	 */
	dynamic = 0;
	if (variable->descriptor_count != 0U && variable->descriptor_chosen != 0U && variable->descriptor_dynamic != 0U)
		dynamic = 1;
	if (dynamic) {
		uniform = UINT32_MAX;
		if (variable->uniform < parser->ir->uniform_count &&
		    parser->ir->uniforms[variable->uniform].kind == DRV_GPU_IR_UNIFORM_STORAGE_IMAGE)
			uniform = variable->uniform;
	} else {
		uniform = drv_gpu_spirv_resource_image_uniform(parser, variable);
	}
	format = 0U;
	if (uniform != UINT32_MAX)
		format = parser->ir->uniforms[uniform].format;
	if (format != 98U && format != 99U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "image atomic on an image that is not R32_UINT or R32_SINT");
		return error;
	}

	/* The three coordinates (a storage image has no samples: the sample is ignored). */
	error = drv_gpu_spirv_image_params(parser,
					   type,
					   word[4],
					   word,
					   count,
					   count,
					   params,
					   opcode,
					   offset);
	if (error != 0)
		return error;

	/* The result names the texel. */
	record = drv_gpu_spirv_id(parser, word[2]);
	pointer_type = drv_gpu_spirv_id(parser, word[1]);
	if (record == NULL || record->kind != ID_NONE || pointer_type == NULL)
		return EINVAL;
	record->kind = ID_POINTER;
	record->ptr_kind = PTR_TEXEL;
	record->type = word[1];
	record->pointee = pointer_type->type;
	record->var = variable->var;
	if (dynamic)
		drv_gpu_spirv_descriptor_copy(record, variable);
	record->uniform = uniform;
	record->comp[0] = drv_gpu_spirv_consecutive(parser, params, 3U);

	/* Succeeded unless the emit failed, which the parser latched. */
	return parser->error;
}
