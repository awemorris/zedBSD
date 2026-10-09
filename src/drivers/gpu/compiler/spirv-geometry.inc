/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The geometry part of the SPIR-V parser (ws075-p007a), included by spirv.c.
 *
 * A Geometry entry point is a geometry shader.  Its execution modes say how
 * many vertices one input primitive has (points 1, lines 2, lines with
 * adjacency 4, triangles 3, triangles with adjacency 6), which primitive it
 * emits (points, line strips or triangle strips) and how many vertices one
 * invocation emits at most; one invocation runs for each input primitive.
 *
 * Its inputs are per vertex: gl_in, an array of gl_PerVertex blocks whose
 * members are built-ins, and located arrays, one element to each input
 * vertex.  The first index of an access chain into one chooses the vertex,
 * as a constant or at run time; a load reads each scalar of the chosen
 * vertex bit for bit (LOAD_VERTEX_INPUT).  gl_PrimitiveIDIn is the
 * thread's own value (LOAD_SYSTEM).
 *
 * The outputs are written as in a vertex shader -- gl_Position, located
 * outputs, and the geometry shader's own gl_Layer and gl_PrimitiveID -- and
 * OpEmitVertex sends what they hold now as one vertex (EMIT_VERTEX);
 * OpEndPrimitive ends the strip (END_PRIMITIVE).  Both take the predicate
 * of the block they are in, since if-conversion runs every block for every
 * channel.
 *
 * Refused, with the reason named: more than one invocation
 * (gl_InvocationID), gl_ViewportIndex (the device has one viewport),
 * gl_PointSize written (shaderTessellationAndGeometryPointSize is not
 * offered), a gl_in member other than gl_Position and gl_PointSize, and an
 * input array whose length is not the input primitive's vertex count.
 *
 * The file is part of spirv.c's translation unit: it uses that file's types,
 * constants and helpers, and its functions are declared with the others at
 * the top of spirv.c.
 */

/*
 * The execution modes of a geometry shader (Khronos SPIR-V spec, section 3.6).
 */
#define EXECUTION_MODE_INVOCATIONS 0U
#define EXECUTION_MODE_INPUT_POINTS 19U
#define EXECUTION_MODE_INPUT_LINES 20U
#define EXECUTION_MODE_INPUT_LINES_ADJACENCY 21U
#define EXECUTION_MODE_TRIANGLES 22U
#define EXECUTION_MODE_INPUT_TRIANGLES_ADJACENCY 23U
#define EXECUTION_MODE_OUTPUT_VERTICES 26U
#define EXECUTION_MODE_OUTPUT_POINTS 27U
#define EXECUTION_MODE_OUTPUT_LINE_STRIP 28U
#define EXECUTION_MODE_OUTPUT_TRIANGLE_STRIP 29U

/*
 * Records one execution mode of a geometry shader: the input primitive, the
 * output primitive, the most vertices emitted, and the invocation count,
 * which must be one.  Any other mode is refused, since it would change what
 * the shader is.
 */
static int
drv_gpu_spirv_geometry_mode(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;

	/* No geometry mode names constants by id. */
	if (opcode != OP_EXECUTION_MODE) {
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "OpExecutionModeId in a geometry shader");
		return error;
	}

	/* Records what the mode says. */
	switch (word[2]) {
	case EXECUTION_MODE_INPUT_POINTS:
		/* A point is one input vertex. */
		parser->ir->vertices_in = 1U;
		break;

	case EXECUTION_MODE_INPUT_LINES:
		/* A line is two. */
		parser->ir->vertices_in = 2U;
		break;

	case EXECUTION_MODE_INPUT_LINES_ADJACENCY:
		/* A line with its neighbours is four. */
		parser->ir->vertices_in = 4U;
		break;

	case EXECUTION_MODE_TRIANGLES:
		/* A triangle is three. */
		parser->ir->vertices_in = 3U;
		break;

	case EXECUTION_MODE_INPUT_TRIANGLES_ADJACENCY:
		/* A triangle with its neighbours is six. */
		parser->ir->vertices_in = 6U;
		break;

	case EXECUTION_MODE_OUTPUT_POINTS:
		/* The vertices emitted are points. */
		parser->ir->output_topology = DRV_GPU_IR_OUTPUT_POINTS;
		break;

	case EXECUTION_MODE_OUTPUT_LINE_STRIP:
		/* The vertices emitted make line strips. */
		parser->ir->output_topology = DRV_GPU_IR_OUTPUT_LINE_STRIP;
		break;

	case EXECUTION_MODE_OUTPUT_TRIANGLE_STRIP:
		/* The vertices emitted make triangle strips. */
		parser->ir->output_topology = DRV_GPU_IR_OUTPUT_TRIANGLE_STRIP;
		break;

	case EXECUTION_MODE_OUTPUT_VERTICES:
		/*
		 * The most vertices one invocation emits, which must be some
		 * and no more than the device takes.
		 */
		if (count < 4U)
			return EINVAL;
		if (word[3] == 0U || word[3] > DRV_GPU_IR_MAX_OUTPUT_VERTICES) {
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "geometry OutputVertices of zero or more than 256 "
			    "(maxGeometryOutputVertices)");
			return error;
		}

		/* Records the geometry invocation's declared output vertex bound. */
		parser->ir->max_vertices = word[3];
		break;

	case EXECUTION_MODE_INVOCATIONS:
		/*
		 * One invocation runs for each input primitive; more are not
		 * lowered.
		 */
		if (count < 4U)
			return EINVAL;
		if (word[3] != 1U) {
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "geometry invocations other than one");
			return error;
		}

		break;

	default:
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "execution mode of a geometry shader that is not lowered");
		return error;
	}

	/* Succeeded: the mode is recorded. */
	return 0;
}

/*
 * Checks, once the declarations are read, that a geometry shader said what
 * primitive it takes, what primitive it emits and how many vertices at
 * most.  A module of another stage passes unchanged.
 */
static int
drv_gpu_spirv_geometry_declared(
	struct drv_gpu_spirv_parser *parser)
{
	int error;

	/* Only a geometry shader has these modes. */
	if (parser->ir->stage != DRV_GPU_STAGE_GEOMETRY)
		return 0;

	/* The input primitive decides the thread's payload. */
	if (parser->ir->vertices_in == 0U) {
		error = drv_gpu_spirv_refuse(
		    parser,
		    OP_ENTRY_POINT,
		    0U,
		    "geometry shader without an input primitive");
		return error;
	}

	/*
	 * The output primitive decides how the emitted vertices are assembled.
	 */
	if (parser->ir->output_topology == 0U) {
		error = drv_gpu_spirv_refuse(
		    parser,
		    OP_ENTRY_POINT,
		    0U,
		    "geometry shader without an output primitive");
		return error;
	}

	/* The most vertices emitted sizes the output. */
	if (parser->ir->max_vertices == 0U) {
		error = drv_gpu_spirv_refuse(
		    parser,
		    OP_ENTRY_POINT,
		    0U,
		    "geometry shader without OutputVertices");
		return error;
	}

	/* Succeeded: the shader's primitives are known. */
	return 0;
}

/*
 * Records an input variable of a geometry shader: gl_PrimitiveIDIn, the
 * thread's value; gl_in, whose members are built-ins; or a located array,
 * whose element's locations one input vertex takes.  Every array has one
 * element to each vertex of the input primitive.
 */
static int
drv_gpu_spirv_declare_geometry_input(
	struct drv_gpu_spirv_parser *parser,
	struct drv_gpu_spirv_id *record,
	uint32_t opcode,
	uint32_t offset)
{
	struct drv_gpu_spirv_id *array;
	struct drv_gpu_spirv_id *block;
	uint32_t member;
	int error;

	/*
	 * A built-in variable: gl_PrimitiveIDIn is the thread's value; the
	 * others are refused.
	 */
	if (record->has_builtin != 0U) {
		if (record->builtin == BUILTIN_PRIMITIVE_ID) {
			record->ptr_kind = PTR_SYSTEM;

			/*
			 * Succeeded: a load reads the input primitive's number
			 * from the payload.
			 */
			return 0;
		}

		/*
		 * gl_InvocationID only means something with more than one
		 * invocation.
		 */
		if (record->builtin == BUILTIN_INVOCATION_ID) {
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "gl_InvocationID (geometry invocations other than "
			    "one are not lowered)");
			return error;
		}

		/* Any other built-in input is not lowered. */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "geometry built-in input that is not lowered");
		return error;
	}

	/* The input primitive must be known before its vertices' arrays. */
	if (parser->ir->vertices_in == 0U) {
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "geometry input declared before an input primitive");
		return error;
	}

	/* Resolves the array of the input vertices. */
	array = drv_gpu_spirv_id(parser, record->pointee);
	if (array == NULL)
		return EINVAL;

	/*
	 * Every other input is an array with one element to each input vertex.
	 */
	if (array->kind != ID_TYPE_ARRAY) {
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "geometry input that is not an "
					     "array of the input vertices");
		return error;
	}

	/* Requires the input array length to match the geometry primitive's vertex count. */
	if (array->length != parser->ir->vertices_in) {
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "geometry input array of a length other than the input "
		    "primitive's vertex count");
		return error;
	}

	/* A load chooses the vertex first; nothing is chosen yet. */
	record->ptr_kind = PTR_VERTEX;
	record->vertex_chosen = 0U;
	record->vertex = 0U;
	record->vertex_index = NO_VALUE;

	/* A located array lists the locations one vertex's element takes. */
	if (record->has_location != 0U) {
		error = drv_gpu_spirv_add_io(parser,
					     record->var,
					     array->type,
					     1,
					     opcode,
					     offset);
		if (error != 0)
			return error;

		/* Succeeded: the located input is part of the interface. */
		return 0;
	}

	/*
	 * Without a location the array must be gl_in: blocks whose members are
	 * all built-ins.
	 */
	block = drv_gpu_spirv_id(parser, array->type);
	if (block == NULL)
		return EINVAL;
	if (block->kind != ID_TYPE_STRUCT) {
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "geometry input that is neither located nor gl_in");
		return error;
	}

	/*
	 * A member that is not a built-in would have no location to be read at.
	 */
	for (member = 0U; member < block->count; member++) {
		if (block->member_builtin[member] == 0U) {
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "gl_in member that is not a built-in");
			return error;
		}
	}

	/* Succeeded: gl_in's built-ins are read through access chains. */
	return 0;
}

/*
 * Steps a pointer into a geometry shader's per-vertex input its first index
 * down: the input vertex, by a constant or by an integer known at run time.
 * What follows the vertex is stepped as any other input
 * (drv_gpu_spirv_chain_scalars()).
 */
static int
drv_gpu_spirv_chain_vertex(
	struct drv_gpu_spirv_parser *parser,
	struct drv_gpu_spirv_id *record,
	struct drv_gpu_spirv_id *pointee,
	struct drv_gpu_spirv_id *index_record,
	uint32_t index_id,
	uint32_t opcode,
	uint32_t offset)
{
	uint32_t scalars[MAX_COMPONENTS];
	uint32_t scalar_count;
	uint32_t width;
	int error;

	/*
	 * The declaration made every per-vertex input an array of the input
	 * vertices.
	 */
	if (pointee->kind != ID_TYPE_ARRAY)
		return EINVAL;

	/*
	 * A constant index names the vertex now; one past the primitive's
	 * vertices is refused.
	 */
	if (index_record->kind == ID_CONSTANT) {
		if (index_record->constant >= parser->ir->vertices_in) {
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "constant input vertex past the input primitive's "
			    "vertices");
			return error;
		}

		/* Publishes the statically selected input vertex in the geometry pointer. */
		record->vertex = index_record->constant;
		record->vertex_index = NO_VALUE;
	} else {
		/* An index known at run time is one integer. */
		scalar_count = drv_gpu_spirv_operand(parser, index_id, scalars);
		if (scalar_count != 1U) {
			error =
			    drv_gpu_spirv_refuse(parser,
						 opcode,
						 offset,
						 "dynamic input vertex that is "
						 "not an integer scalar");
			return error;
		}

		/* A 16-bit index made whole, signed as SPIR-V's indices are. */
		width = drv_gpu_spirv_operand_int_width(parser, index_id);
		record->vertex = 0U;
		record->vertex_index =
		    drv_gpu_spirv_extend16(parser, scalars[0], width, 1);
	}

	/*
	 * The vertex is chosen: the pointer addresses its element, and every
	 * later index steps inside the element.
	 */
	record->vertex_chosen = 1U;
	record->pointee = pointee->type;

	/* Succeeded: the pointer names the input vertex's element. */
	return 0;
}

/*
 * Lowers a load from a geometry shader's per-vertex input: one
 * LOAD_VERTEX_INPUT to each scalar, of the chosen vertex, at the location
 * and component of its interface slot -- a located input's own, or the
 * position or the point size of the VUE for gl_in's built-ins.  The value
 * is read bit for bit: nothing is interpolated before a geometry shader.
 */
static int
drv_gpu_spirv_lower_load_vertex(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	const struct drv_gpu_spirv_id *pointer,
	const struct drv_gpu_spirv_id *variable,
	uint32_t components,
	uint32_t opcode,
	uint32_t offset)
{
	struct drv_gpu_spirv_id *record;
	struct drv_gpu_shader_ir_inst *inst;
	uint32_t map[MAX_COMPONENTS];
	uint32_t map_count;
	uint32_t location;
	uint32_t component;
	uint32_t vertex_source;
	uint32_t vertex_immediate;
	uint32_t first;
	uint32_t index;
	int error;

	/* The whole array of the input vertices is not one value. */
	if (pointer->vertex_chosen == 0U) {
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "load of a whole per-vertex input array");
		return error;
	}

	/*
	 * The first interface slot the pointer addresses inside the vertex's
	 * element.
	 */
	first = 0U;
	if (pointer->component >= 0)
		first = (uint32_t)pointer->component;

	/* Finds the slot of each scalar loaded. */
	map_count = 0U;
	error = drv_gpu_spirv_io_map(parser,
				     pointer->pointee,
				     0U,
				     first,
				     map,
				     &map_count,
				     MAX_COMPONENTS);
	if (error != 0 || map_count != components) {
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "load of a geometry input that is not made of scalars");
		return error;
	}

	/* The vertex: a constant number, or the integer chosen at run time. */
	vertex_source = 0U;
	vertex_immediate = pointer->vertex;
	if (pointer->vertex_index != NO_VALUE) {
		vertex_source = pointer->vertex_index;
		vertex_immediate = DRV_GPU_IR_VERTEX_DYNAMIC;
	}

	/* Each scalar is a fresh value read from the vertex. */
	record = drv_gpu_spirv_result(parser, word[2], word[1], components, 1);
	if (record == NULL)
		return EINVAL;

	/* Emits one per-vertex load to each scalar. */
	for (index = 0U; index < components; index++) {
		/*
		 * gl_in's built-ins have places of their own in the VUE; a
		 * located input's slot names its location.
		 */
		if (variable->has_location == 0U) {
			error = drv_gpu_spirv_gl_in_location(parser,
							     pointer,
							     variable,
							     map[index],
							     &location,
							     &component,
							     opcode,
							     offset);
			if (error != 0)
				return error;
		} else {
			location = variable->location + map[index] / 4U;
			component = map[index] % 4U;
		}

		/*
		 * Reads the scalar of the vertex; a failed emit has latched the
		 * error.
		 */
		inst = drv_gpu_spirv_emit(parser,
					  DRV_GPU_IR_LOAD_VERTEX_INPUT,
					  record->comp[index],
					  vertex_source,
					  0U);
		if (inst == NULL)
			continue;
		inst->location = location;
		inst->component = component;
		inst->immediate = vertex_immediate;
	}

	/* Succeeded: the load names the vertex's scalars. */
	return 0;
}

/*
 * Finds where a scalar of gl_in is read: gl_Position's components are the
 * VUE's position, gl_PointSize its point size.  `slot` is the scalar's
 * interface slot in the block (four to a member location).  Any other
 * member is refused.
 */
static int
drv_gpu_spirv_gl_in_location(
	struct drv_gpu_spirv_parser *parser,
	const struct drv_gpu_spirv_id *pointer,
	const struct drv_gpu_spirv_id *variable,
	uint32_t slot,
	uint32_t *location,
	uint32_t *component,
	uint32_t opcode,
	uint32_t offset)
{
	struct drv_gpu_spirv_id *array;
	struct drv_gpu_spirv_id *block;
	uint32_t builtin;
	uint32_t member_type;
	uint32_t member_scalar;
	uint32_t member_slot;
	int error;

	/* A whole vertex of gl_in is not one built-in. */
	if (pointer->member < 0) {
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "load of a whole vertex of gl_in");
		return error;
	}

	/* Resolves the block one vertex of gl_in is. */
	array = drv_gpu_spirv_id(parser, variable->pointee);
	if (array == NULL)
		return EINVAL;
	block = drv_gpu_spirv_id(parser, array->type);
	if (block == NULL)
		return EINVAL;
	if ((uint32_t)pointer->member >= block->count)
		return EINVAL;

	/*
	 * The member's built-in, and the first slot the member takes in the
	 * block.
	 */
	builtin = block->member_builtin[pointer->member] - 1U;
	error = drv_gpu_spirv_type_step(parser,
					array->type,
					(uint32_t)pointer->member,
					&member_type,
					&member_scalar,
					&member_slot,
					opcode,
					offset);
	if (error != 0)
		return error;

	/*
	 * gl_Position is the VUE's position, a component to each scalar;
	 * gl_PointSize its point size.
	 */
	if (builtin == BUILTIN_POSITION) {
		*location = DRV_GPU_IR_LOCATION_POSITION;
		*component = slot - member_slot;
	} else if (builtin == BUILTIN_POINT_SIZE) {
		*location = DRV_GPU_IR_LOCATION_POINT_SIZE;
		*component = 0U;
	} else {
		error =
		    drv_gpu_spirv_refuse(parser,
					 opcode,
					 offset,
					 "read of a gl_in member other than "
					 "gl_Position and gl_PointSize");
		return error;
	}

	/* Succeeded: the scalar's place in the VUE. */
	return 0;
}

/*
 * Picks where a geometry shader's built-in output is written: gl_Position
 * as in a vertex shader, gl_Layer to the VUE header's render target array
 * index, gl_PrimitiveID as a varying at its own location.  gl_PointSize,
 * gl_ViewportIndex and any other built-in are refused.
 */
static int
drv_gpu_spirv_geometry_output(
	struct drv_gpu_spirv_parser *parser,
	uint32_t builtin,
	uint32_t *location,
	uint32_t opcode,
	uint32_t offset)
{
	int error;

	/* Picks the place of the built-in. */
	switch (builtin) {
	case BUILTIN_POSITION:
		/* The vertex's position. */
		*location = DRV_GPU_IR_LOCATION_POSITION;
		break;

	case BUILTIN_LAYER:
		/*
		 * The layer the primitive is drawn into; the draw takes it from
		 * the VUE header.
		 */
		*location = DRV_GPU_IR_LOCATION_LAYER;
		parser->ir->writes_layer = 1U;
		break;

	case BUILTIN_PRIMITIVE_ID:
		/* The number the fragment shader reads as gl_PrimitiveID. */
		*location = DRV_GPU_SHADER_LOCATION_PRIMITIVE_ID;
		break;

	case BUILTIN_POINT_SIZE:
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "gl_PointSize written by a geometry shader "
		    "(shaderTessellationAndGeometryPointSize is not offered)");
		return error;

	case BUILTIN_VIEWPORT_INDEX:
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "gl_ViewportIndex (the device has one viewport)");
		return error;

	default:
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "geometry built-in output that is not lowered");
		return error;
	}

	/* Succeeded: the built-in's place is known. */
	return 0;
}

/*
 * Lowers OpEmitVertex and OpEndPrimitive: EMIT_VERTEX sends what the
 * outputs hold now as one vertex, END_PRIMITIVE ends the strip.  Both run
 * for the channels of the block they are in; a strip of points has nothing
 * to end, so OpEndPrimitive of a shader emitting points emits nothing.
 */
static int
drv_gpu_spirv_lower_emit(
	struct drv_gpu_spirv_parser *parser,
	uint32_t opcode,
	uint32_t offset)
{
	struct drv_gpu_shader_ir_inst *inst;
	enum drv_gpu_shader_ir_op op;
	int error;

	/* Only a geometry shader emits vertices. */
	if (parser->ir->stage != DRV_GPU_STAGE_GEOMETRY) {
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "OpEmitVertex or OpEndPrimitive outside a geometry shader");
		return error;
	}

	/* Every point is a primitive of its own: ending one changes nothing. */
	if (opcode == OP_END_PRIMITIVE &&
	    parser->ir->output_topology == DRV_GPU_IR_OUTPUT_POINTS)
		return 0;

	/*
	 * Picks the operation; a strip that is ended makes the output carry cut
	 * bits.
	 */
	if (opcode == OP_EMIT_VERTEX) {
		op = DRV_GPU_IR_EMIT_VERTEX;
	} else {
		op = DRV_GPU_IR_END_PRIMITIVE;
		parser->ir->uses_end_primitive = 1U;
	}

	/* Emits it; a failed emit has latched the error. */
	inst = drv_gpu_spirv_emit(parser, op, 0U, 0U, 0U);
	if (inst == NULL)
		return parser->error;

	/* Under a predicate only the block's channels emit or end. */
	if (parser->predicate != PREDICATE_ALWAYS) {
		inst->src[0] = parser->predicate;
		inst->component = 1U;
	}

	/* Succeeded: the vertex is emitted or the strip ended. */
	return 0;
}
