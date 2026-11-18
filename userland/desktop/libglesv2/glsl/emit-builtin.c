/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GLSL compiler's built-in functions in SPIR-V: GLSL.std.450's
 * extended instructions, single SPIR-V instructions, and the ones built
 * of several (texture lookups, dot of scalars, any and all without OpAny
 * and OpAll, projective lookups as a division, faceforward as a select).
 */

#include "emit.h"

#include <string.h>

/* GLSL.std.450's refract, whose last operand stays a scalar. */
#define BUILTIN_REFRACT		72U

static struct emit_value builtin_ext(struct emit_state *state, struct glsl_node *node, struct emit_value *arguments, unsigned count);
static struct emit_value builtin_special(struct emit_state *state, struct glsl_node *node, struct emit_value *arguments, unsigned count);
static struct emit_value builtin_texture(struct emit_state *state, struct glsl_node *node, struct emit_value *arguments, unsigned count);
static struct emit_value builtin_texture_query(struct emit_state *state, struct glsl_node *node, struct emit_value *arguments);
static struct emit_value builtin_gather(struct emit_state *state, struct glsl_node *node, struct emit_value *arguments);
static struct emit_value builtin_query_lod(struct emit_state *state, struct glsl_node *node, struct emit_value *arguments);
static struct emit_value builtin_stream(struct emit_state *state, struct glsl_node *node);
static uint32_t builtin_coordinate(struct emit_state *state, struct emit_value coordinate, unsigned size, int project, uint32_t *reference, int shadow);
static unsigned builtin_dimension(const struct glsl_type *sampler);
static uint32_t builtin_image(struct emit_state *state, struct emit_value sampler);
static uint32_t builtin_rect_coordinate(struct emit_state *state, struct emit_value sampler, uint32_t coordinate);
static void builtin_image_query(struct emit_state *state);
static struct emit_value builtin_value(uint32_t id, const struct glsl_type *type);

/*
 * Emits a call of a built-in function on evaluated arguments.
 */
struct emit_value
glsl_emit_builtin(
	struct emit_state *state,
	struct glsl_node *node,
	struct emit_value *arguments,
	unsigned count)
{
	const struct glsl_builtin *builtin;
	struct emit_value result;
	uint32_t operands[8];
	unsigned index;

	/* The kinds of built-ins. */
	builtin = node->builtin;
	if (builtin->operation == GLSL_BI_EXT) {
		result = builtin_ext(state, node, arguments, count);
		return result;
	}

	/* The ones made of several instructions. */
	if (builtin->operation == GLSL_BI_SPECIAL) {
		result = builtin_special(state, node, arguments, count);
		return result;
	}

	/* One SPIR-V instruction on the arguments. */
	for (index = 0U; index < count; index++)
		operands[index] = arguments[index].id;
	result = builtin_value(glsl_emit_op(state, builtin->number, glsl_emit_type(state, node->type), operands, count), node->type);

	/* Succeeded: the result. */
	return result;
}

/* Emits a GLSL.std.450 instruction, a scalar argument spread to the result's vector (but refract's eta). */
static struct emit_value
builtin_ext(
	struct emit_state *state,
	struct glsl_node *node,
	struct emit_value *arguments,
	unsigned count)
{
	uint32_t operands[10];
	unsigned index;
	struct emit_value result;
	const char *generic;

	/* The set and the instruction number. */
	operands[0] = state->module->std450;
	operands[1] = node->builtin->number;

	/* Whether the result is generic (a scalar operand is then made a vector like it; unpacking is not). */
	generic = strchr("GIUH", node->builtin->signature[0]);

	/* The operands, a scalar one made a vector when the result is a generic vector. */
	for (index = 0U; index < count; index++) {
		operands[2U + index] = arguments[index].id;
		if (node->builtin->number == BUILTIN_REFRACT && index == 2U)
			continue;
		if (generic == NULL)
			continue;
		if (arguments[index].type->kind == GLSL_KIND_SCALAR && node->type->kind == GLSL_KIND_VECTOR)
			operands[2U + index] = glsl_emit_splat(state, arguments[index].id, glsl_type_with_base(node->type, arguments[index].type->base));
	}

	/* Succeeded: OpExtInst. */
	result = builtin_value(glsl_emit_op(state, SPV_OP_EXT_INST, glsl_emit_type(state, node->type), operands, 2U + count), node->type);
	return result;
}

/* Emits the built-ins made of several instructions. */
static struct emit_value
builtin_special(
	struct emit_state *state,
	struct glsl_node *node,
	struct emit_value *arguments,
	unsigned count)
{
	const struct glsl_type *type;
	const struct glsl_type *bool_type;
	struct emit_value result;
	struct emit_value negated;
	uint32_t operands[4];
	uint32_t columns[4];
	uint32_t id;
	unsigned column;
	struct emit_value left_column;
	struct emit_value right_column;

	/* The built-in. */
	type = node->type;
	bool_type = glsl_type_scalar(GLSL_BASE_BOOL);
	switch (node->builtin->number) {
	case GLSL_SPECIAL_TEXTURE:
	case GLSL_SPECIAL_TEXTURE_BIAS:
	case GLSL_SPECIAL_TEXTURE_PROJ:
	case GLSL_SPECIAL_TEXTURE_PROJ_BIAS:
	case GLSL_SPECIAL_TEXTURE_LOD:
	case GLSL_SPECIAL_TEXTURE_PROJ_LOD:
	case GLSL_SPECIAL_SHADOW:
	case GLSL_SPECIAL_SHADOW_PROJ:
	case GLSL_SPECIAL_TEXTURE_GRAD:
	case GLSL_SPECIAL_TEXTURE_OFFSET:
	case GLSL_SPECIAL_TEXTURE_OFFSET_BIAS:
	case GLSL_SPECIAL_TEXTURE_LOD_OFFSET:
		result = builtin_texture(state, node, arguments, count);
		return result;
	case GLSL_SPECIAL_TEXTURE_GATHER:
	case GLSL_SPECIAL_TEXTURE_GATHER_OFFSET:
	case GLSL_SPECIAL_TEXTURE_GATHER_OFFSETS:
		result = builtin_gather(state, node, arguments);
		return result;
	case GLSL_SPECIAL_TEXTURE_SIZE:
	case GLSL_SPECIAL_TEXEL_FETCH:
		result = builtin_texture_query(state, node, arguments);
		return result;
	case GLSL_SPECIAL_TEXTURE_QUERY_LOD:
		result = builtin_query_lod(state, node, arguments);
		return result;
	case GLSL_SPECIAL_EMIT_VERTEX:
		/* A geometry shader emits a vertex of its outputs' values on stream 0 (captured first): no operands, no value. */
		glsl_emit_capture_vertex(state, 0U);
		glsl_words_add(state->module, &state->module->body, SPV_OP_EMIT_VERTEX, NULL, 0U);
		result = builtin_value(0U, type);
		return result;
	case GLSL_SPECIAL_END_PRIMITIVE:
		/* A geometry shader ends the strip it emits on stream 0. */
		glsl_emit_capture_end(state, 0U);
		glsl_words_add(state->module, &state->module->body, SPV_OP_END_PRIMITIVE, NULL, 0U);
		result = builtin_value(0U, type);
		return result;
	case GLSL_SPECIAL_EMIT_STREAM_VERTEX:
	case GLSL_SPECIAL_END_STREAM_PRIMITIVE:
		result = builtin_stream(state, node);
		return result;
	case GLSL_SPECIAL_MOD:
		/* x - y * floor(x / y) is OpFMod (a scalar y spread first). */
		operands[0] = arguments[0].id;
		operands[1] = glsl_emit_splat(state, arguments[1].id, type);
		if (arguments[1].type->kind != GLSL_KIND_SCALAR)
			operands[1] = arguments[1].id;
		result = builtin_value(glsl_emit_op(state, SPV_OP_F_MOD, glsl_emit_type(state, type), operands, 2U), type);
		return result;
	case GLSL_SPECIAL_DOT:
		/* The dot product of scalars is their product. */
		operands[0] = arguments[0].id;
		operands[1] = arguments[1].id;
		if (arguments[0].type->kind == GLSL_KIND_SCALAR) {
			id = glsl_emit_op(state, SPV_OP_F_MUL, glsl_emit_type(state, type), operands, 2U);
		} else {
			id = glsl_emit_op(state, SPV_OP_DOT, glsl_emit_type(state, type), operands, 2U);
		}

		/* Succeeded: the dot product. */
		result = builtin_value(id, type);
		return result;
	case GLSL_SPECIAL_ANY:
	case GLSL_SPECIAL_ALL:
		id = glsl_emit_all(state, arguments[0].id, arguments[0].type->components, node->builtin->number == GLSL_SPECIAL_ANY);
		result = builtin_value(id, type);
		return result;
	case GLSL_SPECIAL_NOT:
		result = builtin_value(glsl_emit_op(state, SPV_OP_LOGICAL_NOT, glsl_emit_type(state, type), &arguments[0].id, 1U), type);
		return result;
	case GLSL_SPECIAL_MIX_BOOL:
		/* mix(x, y, a) with bools: y where a is true. */
		operands[0] = arguments[2].id;
		operands[1] = arguments[1].id;
		operands[2] = arguments[0].id;
		result = builtin_value(glsl_emit_op(state, SPV_OP_SELECT, glsl_emit_type(state, type), operands, 3U), type);
		return result;
	case GLSL_SPECIAL_MATRIX_COMP_MULT:
		/* The columns multiplied component by component. */
		for (column = 0U; column < type->columns; column++) {
			left_column = builtin_value(glsl_emit_extract(state, arguments[0], column), glsl_type_column(type));
			right_column = builtin_value(glsl_emit_extract(state, arguments[1], column), glsl_type_column(type));
			operands[0] = left_column.id;
			operands[1] = right_column.id;
			columns[column] = glsl_emit_op(state, SPV_OP_F_MUL, glsl_emit_type(state, glsl_type_column(type)), operands, 2U);
		}

		/* Succeeded: the matrix. */
		result = builtin_value(glsl_emit_op(state, SPV_OP_COMPOSITE_CONSTRUCT, glsl_emit_type(state, type), columns, type->columns), type);
		return result;
	case GLSL_SPECIAL_OUTER_PRODUCT:
		operands[0] = arguments[0].id;
		operands[1] = arguments[1].id;
		result = builtin_value(glsl_emit_op(state, SPV_OP_OUTER_PRODUCT, glsl_emit_type(state, type), operands, 2U), type);
		return result;
	case GLSL_SPECIAL_NOISE:
		/* Noise is 0 (the specification allows it). */
		result = builtin_value(glsl_emit_splat(state, glsl_emit_float(state, 0.0f), type), type);
		return result;
	default:
		break;
	}

	/* faceforward(N, I, Nref): N when dot(Nref, I) < 0, else -N (a select; i915 lacks FaceForward). */
	operands[0] = arguments[2].id;
	operands[1] = arguments[1].id;
	if (arguments[0].type->kind == GLSL_KIND_SCALAR) {
		id = glsl_emit_op(state, SPV_OP_F_MUL, glsl_emit_type(state, glsl_type_scalar(type->base)), operands, 2U);
	} else {
		id = glsl_emit_op(state, SPV_OP_DOT, glsl_emit_type(state, glsl_type_scalar(type->base)), operands, 2U);
	}

	/* Whether the reference faces away. */
	operands[0] = id;
	operands[1] = glsl_emit_floating(state, type->base, 0.0);
	id = glsl_emit_op(state, SPV_OP_F_ORD_LESS_THAN, glsl_emit_type(state, bool_type), operands, 2U);
	negated = builtin_value(glsl_emit_op(state, SPV_OP_F_NEGATE, glsl_emit_type(state, type), &arguments[0].id, 1U), type);
	operands[0] = glsl_emit_splat(state, id, glsl_type_with_base(type, GLSL_BASE_BOOL));
	operands[1] = arguments[0].id;
	operands[2] = negated.id;
	result = builtin_value(glsl_emit_op(state, SPV_OP_SELECT, glsl_emit_type(state, type), operands, 3U), type);

	/* Succeeded: the faced-forward vector. */
	return result;
}

/*
 * Emits a texture lookup: the coordinate (divided by its last component
 * for a projective lookup), the depth reference of a shadow lookup, a
 * bias or a level, and the sample instruction (an explicit level 0 in a
 * vertex shader, which has no implicit level).
 */
static struct emit_value
builtin_texture(
	struct emit_state *state,
	struct glsl_node *node,
	struct emit_value *arguments,
	unsigned count)
{
	const struct glsl_type *sampler;
	const struct glsl_type *texel;
	struct emit_value result;
	uint32_t operands[10];
	uint32_t coordinate;
	uint32_t reference;
	uint32_t opcode;
	uint32_t parts[4];
	unsigned special;
	unsigned operand_count;
	unsigned mask;
	unsigned dimension;
	uint32_t bias;
	uint32_t lod;
	uint32_t gradient_x;
	uint32_t gradient_y;
	uint32_t offset;
	int project;
	int shadow;
	int explicit_lod;

	/* What kind of lookup. */
	(void)count;
	special = node->builtin->number;
	sampler = arguments[0].type;
	shadow = (int)sampler->shadow;
	project = 0;
	if (special == GLSL_SPECIAL_TEXTURE_PROJ || special == GLSL_SPECIAL_TEXTURE_PROJ_BIAS ||
	    special == GLSL_SPECIAL_TEXTURE_PROJ_LOD || special == GLSL_SPECIAL_SHADOW_PROJ)
		project = 1;

	/* The operands after the coordinate, by the kind of lookup. */
	bias = 0U;
	lod = 0U;
	gradient_x = 0U;
	gradient_y = 0U;
	offset = 0U;
	switch (special) {
	case GLSL_SPECIAL_TEXTURE_BIAS:
	case GLSL_SPECIAL_TEXTURE_PROJ_BIAS:
		bias = arguments[2].id;
		break;
	case GLSL_SPECIAL_TEXTURE_LOD:
	case GLSL_SPECIAL_TEXTURE_PROJ_LOD:
		lod = arguments[2].id;
		break;
	case GLSL_SPECIAL_TEXTURE_GRAD:
		gradient_x = arguments[2].id;
		gradient_y = arguments[3].id;
		break;
	case GLSL_SPECIAL_TEXTURE_OFFSET:
		offset = arguments[2].id;
		break;
	case GLSL_SPECIAL_TEXTURE_OFFSET_BIAS:
		offset = arguments[2].id;
		bias = arguments[3].id;
		break;
	case GLSL_SPECIAL_TEXTURE_LOD_OFFSET:
		lod = arguments[2].id;
		offset = arguments[3].id;
		break;
	default:
		break;
	}

	/*
	 * The coordinate (and the depth reference); a rectangle's, in texels,
	 * made 2D's.  A cube map array's four components leave the reference
	 * an argument of its own.
	 */
	reference = 0U;
	dimension = builtin_dimension(sampler);
	if (shadow && dimension == 4U) {
		coordinate = builtin_coordinate(state, arguments[1], dimension, 0, &reference, 0);
		reference = arguments[2].id;
	} else {
		coordinate = builtin_coordinate(state, arguments[1], dimension, project, &reference, shadow);
	}
	if (sampler->sampler == GLSL_SAMPLER_RECT)
		coordinate = builtin_rect_coordinate(state, arguments[0], coordinate);

	/* The instruction: an explicit level for level and gradient lookups, and in vertex shaders (a level 0). */
	explicit_lod = 0;
	if (lod != 0U || gradient_x != 0U || state->shader->stage != GLSL_STAGE_FRAGMENT)
		explicit_lod = 1;
	if (explicit_lod && lod == 0U && gradient_x == 0U)
		lod = glsl_emit_float(state, 0.0f);
	if (shadow) {
		opcode = SPV_OP_IMAGE_SAMPLE_DREF_IMPLICIT_LOD;
		if (explicit_lod)
			opcode = SPV_OP_IMAGE_SAMPLE_DREF_EXPLICIT_LOD;
	} else {
		opcode = SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD;
		if (explicit_lod)
			opcode = SPV_OP_IMAGE_SAMPLE_EXPLICIT_LOD;
	}

	/* The operands: the sampled image, the coordinate, the reference, then the bias or the level. */
	operands[0] = arguments[0].id;
	operands[1] = coordinate;
	operand_count = 2U;
	if (shadow) {
		operands[2] = reference;
		operand_count = 3U;
	}

	/* The image operands in the order of their bits: a bias, a level, gradients, a constant offset. */
	mask = 0U;
	if (bias != 0U)
		mask |= SPV_IMAGE_OPERAND_BIAS;
	if (lod != 0U)
		mask |= SPV_IMAGE_OPERAND_LOD;
	if (gradient_x != 0U)
		mask |= SPV_IMAGE_OPERAND_GRAD;
	if (offset != 0U)
		mask |= SPV_IMAGE_OPERAND_CONST_OFFSET;
	if (mask != 0U) {
		operands[operand_count] = mask;
		operand_count++;
	}

	/* The bias. */
	if (bias != 0U) {
		operands[operand_count] = bias;
		operand_count++;
	}

	/* The level. */
	if (lod != 0U) {
		operands[operand_count] = lod;
		operand_count++;
	}

	/* The gradients. */
	if (gradient_x != 0U) {
		operands[operand_count] = gradient_x;
		operands[operand_count + 1U] = gradient_y;
		operand_count += 2U;
	}

	/* The offset. */
	if (offset != 0U) {
		operands[operand_count] = offset;
		operand_count++;
	}

	/* A colour lookup gives its texel vector. */
	texel = glsl_type_vector(sampler->base, 4U);
	if (!shadow) {
		result = builtin_value(glsl_emit_op(state, opcode, glsl_emit_type(state, texel), operands, operand_count), texel);
		return result;
	}

	/* A shadow lookup gives a float; shadow1D and shadow2D make it (d, d, d, 1). */
	result = builtin_value(glsl_emit_op(state, opcode, glsl_emit_type(state, glsl_type_scalar(GLSL_BASE_FLOAT)), operands, operand_count),
			       glsl_type_scalar(GLSL_BASE_FLOAT));
	if (node->type->kind == GLSL_KIND_SCALAR)
		return result;
	parts[0] = result.id;
	parts[1] = result.id;
	parts[2] = result.id;
	parts[3] = glsl_emit_float(state, 1.0f);
	result = builtin_value(glsl_emit_op(state, SPV_OP_COMPOSITE_CONSTRUCT, glsl_emit_type(state, node->type), parts, 4U), node->type);

	/* Succeeded: the vector. */
	return result;
}

/*
 * Emits textureGather, textureGatherOffset and textureGatherOffsets: one
 * component (0 unless the last argument names it) of each of the four
 * texels a bilinear lookup at the coordinate reads, as OpImageGather, or
 * the depth comparisons of the four with a reference, as
 * OpImageDrefGather; a rectangle's coordinate made 2D's.  A constant
 * offset is ConstOffset, a computed one Offset and the four offsets
 * ConstOffsets (both GL_ARB_gpu_shader5's, ImageGatherExtended).
 */
static struct emit_value
builtin_gather(
	struct emit_state *state,
	struct glsl_node *node,
	struct emit_value *arguments)
{
	const struct glsl_type *sampler;
	const struct glsl_type *texel;
	struct glsl_node *argument;
	struct emit_value result;
	uint32_t operands[6];
	uint32_t coordinate;
	uint32_t reference;
	uint32_t component;
	uint32_t opcode;
	uint32_t offset;
	uint32_t offset_mask;
	unsigned operand_count;
	unsigned index;
	char code;

	/* The coordinate (a rectangle's in texels, made 2D's). */
	sampler = arguments[0].type;
	reference = 0U;
	coordinate = builtin_coordinate(state, arguments[1], builtin_dimension(sampler), 0, &reference, 0);
	if (sampler->sampler == GLSL_SAMPLER_RECT)
		coordinate = builtin_rect_coordinate(state, arguments[0], coordinate);

	/* The arguments after the coordinate by their codes: the reference, the component, the offset or offsets. */
	component = glsl_emit_int(state, 0);
	offset = 0U;
	offset_mask = 0U;
	index = 0U;
	for (argument = node->child[1]; argument != NULL; argument = argument->next) {
		code = node->builtin->signature[index + 1U];
		if (index < 2U) {
			index++;
			continue;
		}

		/* What the argument is. */
		switch (code) {
		case 'f':
			reference = arguments[index].id;
			break;
		case 'i':
			component = arguments[index].id;
			break;
		case 'j':
			/* One offset: constant, or (GL_ARB_gpu_shader5) computed. */
			offset = arguments[index].id;
			offset_mask = SPV_IMAGE_OPERAND_CONST_OFFSET;
			if (argument->constant == NULL) {
				offset_mask = SPV_IMAGE_OPERAND_OFFSET;
				glsl_emit_capability(state, SPV_CAPABILITY_IMAGE_GATHER_EXTENDED, &state->module->image_gather_extended);
			}
			break;
		case 'L':
			/* The four constant offsets. */
			offset = arguments[index].id;
			offset_mask = SPV_IMAGE_OPERAND_CONST_OFFSETS;
			glsl_emit_capability(state, SPV_CAPABILITY_IMAGE_GATHER_EXTENDED, &state->module->image_gather_extended);
			break;
		default:
			break;
		}
		index++;
	}

	/* The sampled image, the coordinate, then the reference of a comparison or the component. */
	operands[0] = arguments[0].id;
	operands[1] = coordinate;
	opcode = SPV_OP_IMAGE_GATHER;
	operands[2] = component;
	if (sampler->shadow) {
		opcode = SPV_OP_IMAGE_DREF_GATHER;
		operands[2] = reference;
	}
	operand_count = 3U;

	/* The offset or offsets. */
	if (offset_mask != 0U) {
		operands[3] = offset_mask;
		operands[4] = offset;
		operand_count = 5U;
	}

	/* Succeeded: the four texels' components (or comparisons), as the sampler's texel vector. */
	texel = glsl_type_vector(sampler->base, 4U);
	result = builtin_value(glsl_emit_op(state, opcode, glsl_emit_type(state, texel), operands, operand_count), texel);
	return result;
}

/*
 * Emits EmitStreamVertex and EndStreamPrimitive: the vertex or the end of
 * the strip for the stream's capture (glsl_emit_capture_vertex and _end,
 * which count its primitives and write its captured outputs when the
 * shader captures); on stream 0 also the vertex or the end of the strip
 * rasterization takes (OpEmitVertex, OpEndPrimitive), as Vulkan without
 * transform feedback rasterizes stream 0 only.
 */
static struct emit_value
builtin_stream(
	struct emit_state *state,
	struct glsl_node *node)
{
	struct emit_value result;
	unsigned opcode;
	unsigned stream;

	/* The stream's capture (the checker made the stream a constant of 0 to 3). */
	result = builtin_value(0U, node->type);
	stream = (unsigned)node->child[1]->constant->values[0].i;
	if (node->builtin->number == GLSL_SPECIAL_END_STREAM_PRIMITIVE) {
		glsl_emit_capture_end(state, stream);
	} else {
		glsl_emit_capture_vertex(state, stream);
	}

	/* Nothing rasterized for a stream other than 0. */
	if (stream != 0U)
		return result;

	/* Stream 0's vertex, or the end of its strip. */
	opcode = SPV_OP_EMIT_VERTEX;
	if (node->builtin->number == GLSL_SPECIAL_END_STREAM_PRIMITIVE)
		opcode = SPV_OP_END_PRIMITIVE;
	glsl_words_add(state->module, &state->module->body, opcode, NULL, 0U);

	/* Succeeded: no value. */
	return result;
}

/*
 * Emits textureQueryLod: the level of detail a lookup of the coordinate
 * would take (OpImageQueryLod of the sampled image, whose coordinate has
 * no array layer and no reference, as GLSL's has not).
 */
static struct emit_value
builtin_query_lod(
	struct emit_state *state,
	struct glsl_node *node,
	struct emit_value *arguments)
{
	struct emit_value result;
	uint32_t operands[2];

	/* The sampled image and the coordinate. */
	builtin_image_query(state);
	operands[0] = arguments[0].id;
	operands[1] = arguments[1].id;

	/* Succeeded: the level accessed and the level computed. */
	result = builtin_value(glsl_emit_op(state, SPV_OP_IMAGE_QUERY_LOD, glsl_emit_type(state, node->type), operands, 2U), node->type);
	return result;
}

/* Emits textureSize and texelFetch, which read the image the sampler holds. */
static struct emit_value
builtin_texture_query(
	struct emit_state *state,
	struct glsl_node *node,
	struct emit_value *arguments)
{
	const struct glsl_type *sampler;
	struct emit_value result;
	uint32_t operands[4];
	uint32_t image;
	uint32_t level;

	/* The image, and the level asked for (a rectangle's one level is 0; a buffer has none). */
	sampler = arguments[0].type;
	image = builtin_image(state, arguments[0]);
	level = 0U;
	if (sampler->sampler == GLSL_SAMPLER_RECT) {
		level = glsl_emit_int(state, 0);
	} else if (sampler->sampler == GLSL_SAMPLER_BUFFER || sampler->sampler == GLSL_SAMPLER_MS) {
		/* A buffer and a multisample image have no levels. */
		level = 0U;
	} else if (node->builtin->number == GLSL_SPECIAL_TEXTURE_SIZE) {
		/* textureSize's level is its second argument. */
		level = arguments[1].id;
	} else {
		/* texelFetch's is its third. */
		level = arguments[2].id;
	}

	/* textureSize: a buffer's texels, or the level's size. */
	if (node->builtin->number == GLSL_SPECIAL_TEXTURE_SIZE) {
		builtin_image_query(state);
		operands[0] = image;
		if (level == 0U) {
			result = builtin_value(glsl_emit_op(state, SPV_OP_IMAGE_QUERY_SIZE, glsl_emit_type(state, node->type), operands, 1U),
					       node->type);
			return result;
		}

		/* The size of the level. */
		operands[1] = level;
		result = builtin_value(glsl_emit_op(state, SPV_OP_IMAGE_QUERY_SIZE_LOD, glsl_emit_type(state, node->type), operands, 2U), node->type);
		return result;
	}

	/* texelFetch of a multisample image: the sample of the texel. */
	operands[0] = image;
	operands[1] = arguments[1].id;
	if (sampler->sampler == GLSL_SAMPLER_MS) {
		operands[2] = SPV_IMAGE_OPERAND_SAMPLE;
		operands[3] = arguments[2].id;
		result = builtin_value(glsl_emit_op(state, SPV_OP_IMAGE_FETCH, glsl_emit_type(state, node->type), operands, 4U), node->type);
		return result;
	}

	/* texelFetch of a buffer: the texel at the index. */
	if (level == 0U) {
		result = builtin_value(glsl_emit_op(state, SPV_OP_IMAGE_FETCH, glsl_emit_type(state, node->type), operands, 2U), node->type);
		return result;
	}

	/* texelFetch: the texel at the coordinate of the level. */
	operands[2] = SPV_IMAGE_OPERAND_LOD;
	operands[3] = level;
	result = builtin_value(glsl_emit_op(state, SPV_OP_IMAGE_FETCH, glsl_emit_type(state, node->type), operands, 4U), node->type);

	/* Succeeded: the texel. */
	return result;
}

/*
 * Returns the coordinate a lookup of a dimension uses: the first
 * components of the argument, divided by its last one when projective.
 * A shadow lookup's reference is the component after them (the third of
 * a 2D shadow coordinate, always the third for 1D), divided the same way.
 */
static uint32_t
builtin_coordinate(
	struct emit_state *state,
	struct emit_value coordinate,
	unsigned size,
	int project,
	uint32_t *reference,
	int shadow)
{
	const struct glsl_type *float_type;
	const struct glsl_type *result_type;
	struct emit_value divisor;
	uint32_t parts[4];
	uint32_t operands[2];
	uint32_t id;
	unsigned components;
	unsigned index;
	unsigned reference_index;

	/* The argument's components. */
	float_type = glsl_type_scalar(GLSL_BASE_FLOAT);
	components = coordinate.type->components;
	if (coordinate.type->kind == GLSL_KIND_SCALAR)
		components = 1U;

	/* The reference of a shadow lookup: the component after the coordinate (the third, at least). */
	reference_index = size;
	if (reference_index < 2U)
		reference_index = 2U;
	if (shadow)
		*reference = glsl_emit_extract(state, coordinate, reference_index);

	/* A plain lookup whose argument is the coordinate. */
	if (!project && components == size)
		return coordinate.id;

	/* The first components. */
	for (index = 0U; index < size; index++) {
		if (components == 1U) {
			parts[index] = coordinate.id;
		} else {
			parts[index] = glsl_emit_extract(state, coordinate, index);
		}
	}

	/* A projective lookup divides them (and the reference) by the last component. */
	if (project) {
		divisor = builtin_value(glsl_emit_extract(state, coordinate, components - 1U), float_type);
		for (index = 0U; index < size; index++) {
			operands[0] = parts[index];
			operands[1] = divisor.id;
			parts[index] = glsl_emit_op(state, SPV_OP_F_DIV, glsl_emit_type(state, float_type), operands, 2U);
		}

		/* The reference divided too. */
		if (shadow) {
			operands[0] = *reference;
			operands[1] = divisor.id;
			*reference = glsl_emit_op(state, SPV_OP_F_DIV, glsl_emit_type(state, float_type), operands, 2U);
		}
	}

	/* One component is a scalar coordinate. */
	if (size == 1U)
		return parts[0];

	/* Succeeded: the vector of them. */
	result_type = glsl_type_vector(GLSL_BASE_FLOAT, size);
	id = glsl_emit_op(state, SPV_OP_COMPOSITE_CONSTRUCT, glsl_emit_type(state, result_type), parts, size);
	return id;
}

/* Returns how many components a sampler's coordinate has. */
static unsigned
builtin_dimension(
	const struct glsl_type *sampler)
{
	/* 1D and buffer one, 2D and rectangle two (and a layer for an array), 3D and cube three (a cube map array four). */
	if (sampler->sampler == GLSL_SAMPLER_1D || sampler->sampler == GLSL_SAMPLER_BUFFER)
		return 1U;
	if (sampler->sampler == GLSL_SAMPLER_RECT)
		return 2U;
	if (sampler->sampler == GLSL_SAMPLER_2D && sampler->arrayed)
		return 3U;
	if (sampler->sampler == GLSL_SAMPLER_2D)
		return 2U;
	if (sampler->sampler == GLSL_SAMPLER_CUBE && sampler->arrayed)
		return 4U;

	/* 3D and cube maps. */
	return 3U;
}

/* Returns the image a sampled image holds (OpImage). */
static uint32_t
builtin_image(
	struct emit_state *state,
	struct emit_value sampler)
{
	uint32_t image_type;
	uint32_t id;

	/* A buffer sampler is its image already. */
	if (sampler.type->sampler == GLSL_SAMPLER_BUFFER)
		return sampler.id;

	/* OpImage of the sampler's image type. */
	image_type = glsl_emit_image_type(state, sampler.type);
	id = glsl_emit_op(state, SPV_OP_IMAGE, image_type, &sampler.id, 1U);

	/* Succeeded: the image. */
	return id;
}

/* Makes a value from an id and a type. */
static struct emit_value
builtin_value(
	uint32_t id,
	const struct glsl_type *type)
{
	struct emit_value value;

	/* The pair. */
	value.id = id;
	value.type = type;

	/* Succeeded: the value. */
	return value;
}

/* Divides a rectangle lookup's coordinate (in texels) by its image's size, which makes it a 2D lookup's. */
static uint32_t
builtin_rect_coordinate(
	struct emit_state *state,
	struct emit_value sampler,
	uint32_t coordinate)
{
	const struct glsl_type *size_type;
	const struct glsl_type *float_type;
	uint32_t operands[2];
	uint32_t image;
	uint32_t size;
	uint32_t scale;
	uint32_t id;

	/* The image's size (level 0) as floats. */
	size_type = glsl_type_vector(GLSL_BASE_INT, 2U);
	float_type = glsl_type_vector(GLSL_BASE_FLOAT, 2U);
	builtin_image_query(state);
	image = builtin_image(state, sampler);
	operands[0] = image;
	operands[1] = glsl_emit_int(state, 0);
	size = glsl_emit_op(state, SPV_OP_IMAGE_QUERY_SIZE_LOD, glsl_emit_type(state, size_type), operands, 2U);
	scale = glsl_emit_op(state, SPV_OP_CONVERT_S_TO_F, glsl_emit_type(state, float_type), &size, 1U);

	/* The coordinate over it. */
	operands[0] = coordinate;
	operands[1] = scale;
	id = glsl_emit_op(state, SPV_OP_F_DIV, glsl_emit_type(state, float_type), operands, 2U);

	/* Succeeded: the normalized coordinate. */
	return id;
}

/* Declares the ImageQuery capability once (textureSize and rectangle lookups ask images their sizes, textureQueryLod their levels). */
static void
builtin_image_query(
	struct emit_state *state)
{
	uint32_t capability;

	/* Once. */
	if (state->module->image_query)
		return;
	state->module->image_query = 1U;

	/* OpCapability ImageQuery. */
	capability = SPV_CAPABILITY_IMAGE_QUERY;
	glsl_words_add(state->module, &state->module->capabilities, SPV_OP_CAPABILITY, &capability, 1U);
}
