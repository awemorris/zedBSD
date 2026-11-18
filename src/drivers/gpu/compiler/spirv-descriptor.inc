/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The arrays of descriptors and OpImageQueryLod of the SPIR-V parser,
 * included by spirv.c.
 *
 * An array of combined image samplers, of uniform blocks, of storage
 * buffers or of storage images (a GL `uniform sampler2D t[4]`,
 * `uniform B { ... } b[4]`, `buffer S { ... } s[4]` or
 * `uniform image2D i[4]`) is, with the SAMPLER_ARRAYS, BUFFER_ARRAYS or
 * IMAGE_ARRAYS feature, one uniform per element, each naming its element
 * (struct drv_gpu_shader_ir_uniform, `element`).  The first index of an
 * access chain into the array chooses the element.  A constant index names
 * the element's uniform at once.  An index known only at run time is kept on
 * the pointer, and on the combined image sampler loaded through it, until an
 * instruction uses it: that instruction is then lowered once per element,
 * each time with the element as a constant and with the block's predicate
 * narrowed to the channels whose index names the element (so a load, a store
 * or an atomic of a storage buffer, and a texture message's guard, cover
 * those channels alone), and its result is selected from the elements' by
 * the index.  A backend sees constant elements alone and needs no indexing
 * of its own; the cost is one access per element, so an array may have at
 * most DRV_GPU_SPIRV_MAX_DESCRIPTOR_ARRAY elements.  The selection is right
 * whatever the index is, dynamically uniform or not.
 *
 * OpImageQueryLod becomes, with the QUERY_LOD feature, a TEXTURE of the LOD
 * message on the coordinate, whose reply's first two values are the
 * result.
 *
 * The file is part of spirv.c's translation unit: it uses that file's types,
 * constants and helpers, and its functions are declared with the others at
 * the top of spirv.c.
 */

/*
 * Adds the uniform of a uniform block or a storage buffer variable: one, or
 * one per element of an array of them when the backend binds the elements
 * (DRV_GPU_SPIRV_FEATURE_BUFFER_ARRAYS); the variable's `uniform` is the
 * first.
 */
static int
drv_gpu_spirv_add_buffer_uniform(
	struct drv_gpu_spirv_parser *parser,
	uint32_t id,
	uint32_t kind,
	uint32_t opcode,
	uint32_t offset)
{
	struct drv_gpu_spirv_id *variable;
	struct drv_gpu_spirv_id *type;
	struct drv_gpu_spirv_id *block;
	int error;

	/* Resolves the variable and what it points to. */
	variable = &parser->ids[id];
	type = drv_gpu_spirv_id(parser, variable->pointee);
	variable->uniform = parser->ir->uniform_count;

	/*
	 * A block, or an array the backend does not bind (a use of an element
	 * is refused), is one uniform.
	 */
	if (type == NULL ||
	    type->kind != ID_TYPE_ARRAY ||
	    (parser->features & DRV_GPU_SPIRV_FEATURE_BUFFER_ARRAYS) == 0U) {
		if (parser->ir->uniform_count >= parser->uniform_capacity)
			return ENOSPC;
		drv_gpu_spirv_add_uniform(parser, id, kind);
		return 0;
	}

	/* The elements are blocks. */
	block = drv_gpu_spirv_id(parser, type->type);
	if (block == NULL || block->kind != ID_TYPE_STRUCT) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "array of buffers whose element is not a block");
		return error;
	}

	/* One uniform per element. */
	error = drv_gpu_spirv_add_descriptor_array(parser,
						   id,
						   type,
						   kind,
						   opcode,
						   offset);
	if (error != 0)
		return error;

	/* Succeeded: the elements are listed. */
	return 0;
}

/*
 * Adds the uniforms of an array of combined image samplers, one per element
 * (DRV_GPU_SPIRV_FEATURE_SAMPLER_ARRAYS); the variable's `uniform` is the
 * first.  An array of anything else in the UniformConstant class is
 * refused.
 */
static int
drv_gpu_spirv_add_sampler_array(
	struct drv_gpu_spirv_parser *parser,
	uint32_t id,
	const struct drv_gpu_spirv_id *type,
	uint32_t opcode,
	uint32_t offset)
{
	struct drv_gpu_spirv_id *element;
	int error;

	/* Only combined image samplers are lowered in an array. */
	element = drv_gpu_spirv_id(parser, type->type);
	if (element == NULL || element->kind != ID_TYPE_SAMPLED_IMAGE) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "array of images or samplers that are not combined image "
		    "samplers");
		return error;
	}

	/* One sampled image uniform per element. */
	parser->ids[id].uniform = parser->ir->uniform_count;
	error = drv_gpu_spirv_add_descriptor_array(
	    parser,
	    id,
	    type,
	    DRV_GPU_IR_UNIFORM_SAMPLED_IMAGE,
	    opcode,
	    offset);
	if (error != 0)
		return error;

	/* Succeeded: the elements are listed. */
	return 0;
}

/*
 * Tells whether a UniformConstant variable's pointee is an array of
 * storage images the backend binds element by element
 * (DRV_GPU_SPIRV_FEATURE_IMAGE_ARRAYS with STORAGE_IMAGE): of 1D, 2D, 3D
 * or cube images, not of texel buffers or input attachments.
 */
static int
drv_gpu_spirv_is_image_array(
	struct drv_gpu_spirv_parser *parser,
	const struct drv_gpu_spirv_id *type)
{
	const struct drv_gpu_spirv_id *element;

	/* The backend binds the elements of arrays of storage images. */
	if ((parser->features & DRV_GPU_SPIRV_FEATURE_IMAGE_ARRAYS) == 0U ||
	    (parser->features & DRV_GPU_SPIRV_FEATURE_STORAGE_IMAGE) == 0U)
		return 0;

	/* An array of images. */
	if (type == NULL || type->kind != ID_TYPE_ARRAY)
		return 0;
	element = drv_gpu_spirv_id(parser, type->type);
	if (element == NULL || element->kind != ID_TYPE_IMAGE)
		return 0;

	/* Of storage images. */
	if (element->image_sampled != IMAGE_SAMPLED_STORAGE)
		return 0;
	if (element->image_dim == DIM_BUFFER || element->image_dim == DIM_SUBPASS_DATA)
		return 0;

	/* Succeeded: an array of storage images. */
	return 1;
}

/*
 * Adds the uniforms of an array of storage images, one per element
 * (DRV_GPU_SPIRV_FEATURE_IMAGE_ARRAYS), each with the VkFormat of the
 * elements' Image Format; the variable's `uniform` is the first.  An array
 * of a format the parse does not lower, or of multisampled images, is
 * refused, as one such image is.
 */
static int
drv_gpu_spirv_add_image_array(
	struct drv_gpu_spirv_parser *parser,
	uint32_t id,
	const struct drv_gpu_spirv_id *type,
	uint32_t opcode,
	uint32_t offset)
{
	struct drv_gpu_spirv_id *element;
	uint32_t format;
	uint32_t first;
	uint32_t index;
	int error;

	/* The elements' format, one the parse lowers. */
	element = drv_gpu_spirv_id(parser, type->type);
	if (element == NULL)
		return EINVAL;
	format = drv_gpu_spirv_storage_format(element->image_format);
	if (format == 0U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "array of storage images of a format that is not lowered");
		return error;
	}

	/* A storage image is not multisampled (shaderStorageImageMultisample is not offered). */
	if (element->image_ms != 0U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "array of multisampled storage images");
		return error;
	}

	/* One storage image uniform per element. */
	first = parser->ir->uniform_count;
	parser->ids[id].uniform = first;
	error = drv_gpu_spirv_add_descriptor_array(
	    parser,
	    id,
	    type,
	    DRV_GPU_IR_UNIFORM_STORAGE_IMAGE,
	    opcode,
	    offset);
	if (error != 0)
		return error;

	/* Each element's format. */
	for (index = first; index < parser->ir->uniform_count; index++)
		parser->ir->uniforms[index].format = format;

	/* Succeeded: the elements are listed. */
	return 0;
}

/*
 * Adds one uniform of kind `kind` per element of the array of descriptors
 * `type` that variable `id` is, in the order of the elements, and records
 * the element count on the variable.  An array without a constant length (a
 * run-time array, which needs descriptor indexing) and one longer than
 * DRV_GPU_SPIRV_MAX_DESCRIPTOR_ARRAY are refused.
 */
static int
drv_gpu_spirv_add_descriptor_array(
	struct drv_gpu_spirv_parser *parser,
	uint32_t id,
	const struct drv_gpu_spirv_id *type,
	uint32_t kind,
	uint32_t opcode,
	uint32_t offset)
{
	struct drv_gpu_shader_ir_uniform *slot;
	uint32_t element;
	int error;

	/* The array has a length the parse can unroll. */
	if (type->length == 0U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "array of descriptors without a constant length");
		return error;
	}
	if (type->length > DRV_GPU_SPIRV_MAX_DESCRIPTOR_ARRAY) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "array of more descriptors than supported");
		return error;
	}

	/* The uniform list has room for every element. */
	if (parser->ir->uniform_count + type->length > parser->uniform_capacity) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "more descriptors in arrays than supported");
		return error;
	}

	/* Lists each element as a uniform of its own. */
	for (element = 0U; element < type->length; element++) {
		slot = &parser->ir->uniforms[parser->ir->uniform_count];
		drv_gpu_spirv_add_uniform(parser, id, kind);
		slot->element = element;
	}

	/* The variable is an array of that many descriptors. */
	parser->ids[id].descriptor_count = type->length;

	/* Succeeded: the elements are listed. */
	return 0;
}

/*
 * Steps a pointer into an array of descriptors by the chain's first index:
 * a constant index names its element (and the element's uniform), an index
 * known only at run time is kept as an IR integer for the instruction that
 * uses the pointer (drv_gpu_spirv_lower_elements()).
 */
static int
drv_gpu_spirv_chain_descriptor(
	struct drv_gpu_spirv_parser *parser,
	struct drv_gpu_spirv_id *record,
	const struct drv_gpu_spirv_id *pointee,
	const struct drv_gpu_spirv_id *index_record,
	uint32_t index_id,
	uint32_t opcode,
	uint32_t offset)
{
	uint32_t scalars[MAX_COMPONENTS];
	uint32_t scalar_count;
	uint32_t index_width;
	int error;

	/* The pointer addresses the array. */
	if (pointee->kind != ID_TYPE_ARRAY)
		return EINVAL;

	/* What follows is addressed in the element, from its start. */
	record->descriptor_chosen = 1U;
	record->pointee = pointee->type;

	/* A constant index names the element now. */
	if (index_record->kind == ID_CONSTANT) {
		if (index_record->constant >= record->descriptor_count)
			return EINVAL;
		record->descriptor_element = index_record->constant;
		record->uniform += index_record->constant;
		return 0;
	}

	/* Any other index is one integer. */
	scalar_count = drv_gpu_spirv_operand(parser, index_id, scalars);
	if (scalar_count != 1U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "index of an array of descriptors that is not an integer "
		    "scalar");
		return error;
	}

	/* A 16-bit index made whole, signed as SPIR-V's indices are. */
	index_width = drv_gpu_spirv_operand_int_width(parser, index_id);
	scalars[0] = drv_gpu_spirv_extend16(parser, scalars[0], index_width, 1);

	/* The element is chosen where the pointer is used. */
	record->descriptor_dynamic = 1U;
	record->descriptor_index = scalars[0];

	/* Succeeded: the pointer names the element. */
	return 0;
}

/*
 * Copies the element of an array of descriptors a pointer or a combined
 * image sampler names to what is made from it (a loaded sampler, its
 * OpImage).
 */
static void
drv_gpu_spirv_descriptor_copy(
	struct drv_gpu_spirv_id *record,
	const struct drv_gpu_spirv_id *source)
{
	/* The array's size, and the element constant or chosen at run time. */
	record->descriptor_count = source->descriptor_count;
	record->descriptor_chosen = source->descriptor_chosen;
	record->descriptor_dynamic = source->descriptor_dynamic;
	record->descriptor_element = source->descriptor_element;
	record->descriptor_index = source->descriptor_index;
	record->uniform = source->uniform;

	/* Succeeded: the record names the same element. */
	return;
}

/*
 * Reports the word of an instruction that names the pointer or the image it
 * accesses, when that may be an element of an array of descriptors chosen at
 * run time; 0 for an instruction that only passes the element on (an access
 * chain, OpImage, a load of a combined image sampler) or accesses none.
 */
static uint32_t
drv_gpu_spirv_descriptor_operand(
	uint32_t opcode,
	uint32_t count)
{
	/* By the operand each instruction takes its pointer or image in. */
	switch (opcode) {
	case OP_STORE:
	case OP_ATOMIC_STORE:
		/* The pointer stored through. */
		if (count < 2U)
			return 0U;
		return 1U;

	case OP_IMAGE_WRITE:
		/* The storage image written. */
		if (count < 4U)
			return 0U;
		return 1U;

	case OP_LOAD:
	case OP_ATOMIC_LOAD:
	case OP_ATOMIC_EXCHANGE:
	case OP_ATOMIC_COMPARE_EXCHANGE:
	case OP_ATOMIC_I_INCREMENT:
	case OP_ATOMIC_I_DECREMENT:
	case OP_ATOMIC_I_ADD:
	case OP_ATOMIC_I_SUB:
	case OP_ATOMIC_S_MIN:
	case OP_ATOMIC_U_MIN:
	case OP_ATOMIC_S_MAX:
	case OP_ATOMIC_U_MAX:
	case OP_ATOMIC_AND:
	case OP_ATOMIC_OR:
	case OP_ATOMIC_XOR:
	case OP_ARRAY_LENGTH:
	case OP_IMAGE_SAMPLE_IMPLICIT_LOD:
	case OP_IMAGE_SAMPLE_EXPLICIT_LOD:
	case OP_IMAGE_SAMPLE_DREF_IMPLICIT_LOD:
	case OP_IMAGE_SAMPLE_DREF_EXPLICIT_LOD:
	case OP_IMAGE_SAMPLE_PROJ_IMPLICIT_LOD:
	case OP_IMAGE_SAMPLE_PROJ_EXPLICIT_LOD:
	case OP_IMAGE_SAMPLE_PROJ_DREF_IMPLICIT_LOD:
	case OP_IMAGE_SAMPLE_PROJ_DREF_EXPLICIT_LOD:
	case OP_IMAGE_FETCH:
	case OP_IMAGE_GATHER:
	case OP_IMAGE_DREF_GATHER:
	case OP_IMAGE_QUERY_SIZE_LOD:
	case OP_IMAGE_QUERY_SIZE:
	case OP_IMAGE_QUERY_LOD:
	case OP_IMAGE_QUERY_LEVELS:
	case OP_IMAGE_QUERY_SAMPLES:
	case OP_IMAGE_READ:
		/* The pointer or the image after the result. */
		if (count < 4U)
			return 0U;
		return 3U;

	default:
		/* No access through a descriptor. */
		return 0U;
	}
}

/*
 * Lowers one instruction of the function body (drv_gpu_spirv_lower()); an
 * instruction whose pointer or image is an element of an array of
 * descriptors chosen at run time is lowered once per element, each time for
 * the channels whose index names the element, and its result selected from
 * the elements' by the index.
 */
static int
drv_gpu_spirv_lower_elements(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct drv_gpu_spirv_id *resource;
	struct drv_gpu_spirv_id *result;
	struct drv_gpu_spirv_id saved;
	uint32_t merged[MAX_COMPONENTS];
	uint32_t merged_count;
	uint32_t merged_type;
	uint32_t operand;
	uint32_t predicate;
	uint32_t number;
	uint32_t chosen;
	uint32_t element;
	uint32_t component;
	int has_result;
	int error;

	/* Finds the pointer or the image the instruction accesses. */
	operand = drv_gpu_spirv_descriptor_operand(opcode, count);
	resource = NULL;
	if (operand != 0U)
		resource = drv_gpu_spirv_id(parser, word[operand]);

	/*
	 * An access of no element chosen at run time, and a load of a combined
	 * image sampler (which passes the element on), lower once.
	 */
	if (resource == NULL ||
	    resource->descriptor_dynamic == 0U ||
	    (opcode == OP_LOAD && resource->ptr_kind == PTR_SAMPLER)) {
		error = drv_gpu_spirv_lower(parser, word, count, opcode, offset);
		if (error != 0)
			return error;

		/* Succeeded: the instruction is lowered. */
		return 0;
	}

	/* Every other instruction of the list has a result, but a store and an image write. */
	has_result = 1;
	if (opcode == OP_STORE || opcode == OP_ATOMIC_STORE || opcode == OP_IMAGE_WRITE)
		has_result = 0;
	result = NULL;
	if (has_result != 0) {
		result = drv_gpu_spirv_id(parser, word[2]);
		if (result == NULL || result->kind != ID_NONE)
			return EINVAL;
	}

	/* Lowers the instruction once per element, keeping the record to restore. */
	saved = *resource;
	predicate = parser->predicate;
	merged_count = 0U;
	merged_type = 0U;
	for (element = 0U; element < saved.descriptor_count; element++) {
		/* The channels whose index names the element. */
		number = drv_gpu_spirv_integer_constant(parser, element);
		chosen = drv_gpu_spirv_emit_value(parser,
						  DRV_GPU_IR_IEQ,
						  saved.descriptor_index,
						  number);
		if (predicate == PREDICATE_ALWAYS) {
			parser->predicate = chosen;
		} else {
			parser->predicate = drv_gpu_spirv_emit_value(parser,
								     DRV_GPU_IR_AND,
								     predicate,
								     chosen);
		}

		/* The element as a constant one. */
		resource->descriptor_dynamic = 0U;
		resource->descriptor_element = element;
		resource->uniform = saved.uniform + element;

		/* Lowers the instruction for those channels. */
		parser->descriptor_select = 1;
		error = drv_gpu_spirv_lower(parser, word, count, opcode, offset);
		parser->descriptor_select = 0;

		/* The record and the block's predicate as they were. */
		*resource = saved;
		parser->predicate = predicate;
		if (error != 0)
			return error;
		if (parser->error != 0)
			return parser->error;

		/* A store leaves no value to select. */
		if (has_result == 0)
			continue;

		/* The element's result is a value. */
		if (result->kind != ID_VALUE || result->count > MAX_COMPONENTS)
			return EINVAL;

		/*
		 * The first element's scalars, then each later element's where
		 * the index names it.
		 */
		if (element == 0U) {
			merged_count = result->count;
			merged_type = result->type;
			for (component = 0U; component < merged_count; component++)
				merged[component] = result->comp[component];
		} else {
			if (result->count != merged_count)
				return EINVAL;
			for (component = 0U; component < merged_count; component++) {
				merged[component] = drv_gpu_spirv_select_value(
				    parser,
				    chosen,
				    result->comp[component],
				    merged[component]);
			}
		}

		/* The result is declared again by the next element's lowering. */
		result->kind = ID_NONE;
	}

	/* The result names the selected scalars. */
	if (has_result != 0) {
		result->kind = ID_VALUE;
		result->type = merged_type;
		result->count = merged_count;
		for (component = 0U; component < MAX_COMPONENTS; component++) {
			result->comp[component] = NO_VALUE;
			if (component < merged_count)
				result->comp[component] = merged[component];
		}
	}

	/* A selection the parser could not emit fails the lowering. */
	if (parser->error != 0)
		return parser->error;

	/* Succeeded: the instruction is lowered for every element. */
	return 0;
}

/*
 * Lowers OpImageQueryLod: a TEXTURE of the LOD message on the coordinate of
 * a sampled 1D, 2D, 3D or cube image (an array's layer is not part of it),
 * in a fragment shader, whose reply's first two values are the result: the
 * level accessed and the level computed.  Refused without the QUERY_LOD
 * feature.
 *
 * Procedure referenced: Mesa 25.0.7 src/intel/compiler/brw_fs_nir.cpp
 * (fs_nir_emit_texture, nir_texop_lod: SHADER_OPCODE_LOD_LOGICAL on the
 * coordinate, the reply taken as it is) and brw_lower_logical_sends.cpp
 * (GFX5_SAMPLER_MESSAGE_LOD, 9).
 */
static int
drv_gpu_spirv_lower_query_lod(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct drv_gpu_spirv_id *image;
	struct drv_gpu_spirv_id *type;
	struct drv_gpu_spirv_id *record;
	uint32_t coordinate[4];
	uint32_t params[DRV_GPU_IR_TEXTURE_MAX_PARAMS];
	uint32_t param_count;
	uint32_t coordinate_count;
	uint32_t expected;
	uint32_t components;
	uint32_t shape;
	uint32_t first;
	uint32_t index;
	int error;

	/* Only a backend with the query lowers it. */
	if ((parser->features & DRV_GPU_SPIRV_FEATURE_QUERY_LOD) == 0U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "OpImageQueryLod, which this GPU's compiler does not "
		    "lower");
		return error;
	}

	/* The instruction carries the image and the coordinate. */
	if (count != 5U)
		return EINVAL;

	/* The level the derivatives give is a fragment shader's. */
	if (parser->ir->stage != DRV_GPU_STAGE_FRAGMENT) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "OpImageQueryLod outside a fragment shader");
		return error;
	}

	/* Resolves the sampled image. */
	image = drv_gpu_spirv_id(parser, word[3]);
	type = drv_gpu_spirv_image_type(parser, image);
	if (type == NULL || type->image_dim > DIM_CUBE || type->image_ms != 0U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "OpImageQueryLod of something that is not a sampled 1D, "
		    "2D, 3D or cube image");
		return error;
	}

	/* The result is two floats. */
	components = drv_gpu_spirv_float_components(parser, word[1]);
	if (components != 2U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "OpImageQueryLod whose result is not a vec2");
		return error;
	}

	/* The coordinate has the image's dimensions, a cube's direction three. */
	expected = type->image_dim + 1U;
	if (type->image_dim == DIM_CUBE)
		expected = 3U;
	coordinate_count = drv_gpu_spirv_operand(parser, word[4], coordinate);
	if (coordinate_count != expected) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "OpImageQueryLod whose coordinate is not the image's");
		return error;
	}

	/* The message takes the coordinate; the rest reads as zero. */
	param_count = 0U;
	for (index = 0U; index < coordinate_count; index++)
		drv_gpu_spirv_texture_param(params, &param_count, coordinate[index]);
	shape = drv_gpu_spirv_image_shape(type);
	error = drv_gpu_spirv_emit_texture(parser,
					   image,
					   shape,
					   params,
					   param_count,
					   DRV_GPU_IR_TEXTURE_LOD,
					   0U,
					   &first);
	if (error != 0)
		return error;

	/* The result names the reply's first two values. */
	record = drv_gpu_spirv_result(parser, word[2], word[1], 2U, 0);
	if (record == NULL)
		return EINVAL;
	record->comp[0] = first;
	record->comp[1] = first + 1U;

	/* Succeeded: the query is lowered. */
	return 0;
}
