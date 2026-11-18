/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GLSL compiler's back end: the SPIR-V module builder (module.c),
 * the emitter of a checked shader (emit.c, emit-builtin.c) and what the
 * link (link.c) hands the emitter.
 *
 * The module is SPIR-V 1.0 in the shape i915's native compiler takes
 * (see plan/ws068/glsl-design.md section 5): every user function is
 * inlined into main, every variable is a Function variable of main that
 * is stored before it is read, control flow is structured selections and
 * loops only, and the default uniform block holds only the uniforms the
 * stage uses.
 */

#ifndef GLSL_EMIT_H
#define GLSL_EMIT_H

#include "internal.h"

/* SPIR-V's opcodes that the back end writes. */
#define SPV_OP_NAME			5U
#define SPV_OP_MEMBER_NAME		6U
#define SPV_OP_EXT_INST_IMPORT		11U
#define SPV_OP_EXT_INST			12U
#define SPV_OP_MEMORY_MODEL		14U
#define SPV_OP_ENTRY_POINT		15U
#define SPV_OP_EXECUTION_MODE		16U
#define SPV_OP_CAPABILITY		17U
#define SPV_OP_TYPE_VOID		19U
#define SPV_OP_TYPE_BOOL		20U
#define SPV_OP_TYPE_INT			21U
#define SPV_OP_TYPE_FLOAT		22U
#define SPV_OP_TYPE_VECTOR		23U
#define SPV_OP_TYPE_MATRIX		24U
#define SPV_OP_TYPE_IMAGE		25U
#define SPV_OP_TYPE_SAMPLED_IMAGE	27U
#define SPV_OP_TYPE_ARRAY		28U
#define SPV_OP_TYPE_RUNTIME_ARRAY	29U
#define SPV_OP_TYPE_STRUCT		30U
#define SPV_OP_TYPE_POINTER		32U
#define SPV_OP_TYPE_FUNCTION		33U
#define SPV_OP_CONSTANT_TRUE		41U
#define SPV_OP_CONSTANT_FALSE		42U
#define SPV_OP_CONSTANT			43U
#define SPV_OP_CONSTANT_COMPOSITE	44U
#define SPV_OP_FUNCTION			54U
#define SPV_OP_FUNCTION_END		56U
#define SPV_OP_VARIABLE			59U
#define SPV_OP_LOAD			61U
#define SPV_OP_STORE			62U
#define SPV_OP_ACCESS_CHAIN		65U
#define SPV_OP_DECORATE			71U
#define SPV_OP_MEMBER_DECORATE		72U
#define SPV_OP_VECTOR_EXTRACT_DYNAMIC	77U
#define SPV_OP_VECTOR_SHUFFLE		79U
#define SPV_OP_COMPOSITE_CONSTRUCT	80U
#define SPV_OP_COMPOSITE_EXTRACT	81U
#define SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD 87U
#define SPV_OP_IMAGE_SAMPLE_EXPLICIT_LOD 88U
#define SPV_OP_IMAGE_SAMPLE_DREF_IMPLICIT_LOD 89U
#define SPV_OP_IMAGE_SAMPLE_DREF_EXPLICIT_LOD 90U
#define SPV_OP_IMAGE_FETCH		95U
#define SPV_OP_IMAGE_GATHER		96U
#define SPV_OP_IMAGE			100U
#define SPV_OP_IMAGE_QUERY_SIZE_LOD	103U
#define SPV_OP_IMAGE_QUERY_SIZE		104U
#define SPV_OP_IMAGE_QUERY_LOD		105U
#define SPV_IMAGE_OPERAND_SAMPLE	0x40U
#define SPV_OP_CONVERT_F_TO_U		109U
#define SPV_OP_CONVERT_F_TO_S		110U
#define SPV_OP_CONVERT_S_TO_F		111U
#define SPV_OP_CONVERT_U_TO_F		112U
#define SPV_OP_F_CONVERT		115U
#define SPV_OP_BITCAST			124U
#define SPV_OP_S_NEGATE			126U
#define SPV_OP_F_NEGATE			127U
#define SPV_OP_I_ADD			128U
#define SPV_OP_F_ADD			129U
#define SPV_OP_I_SUB			130U
#define SPV_OP_F_SUB			131U
#define SPV_OP_I_MUL			132U
#define SPV_OP_F_MUL			133U
#define SPV_OP_U_DIV			134U
#define SPV_OP_S_DIV			135U
#define SPV_OP_F_DIV			136U
#define SPV_OP_U_MOD			137U
#define SPV_OP_S_REM			138U
#define SPV_OP_F_MOD			141U
#define SPV_OP_VECTOR_TIMES_SCALAR	142U
#define SPV_OP_MATRIX_TIMES_SCALAR	143U
#define SPV_OP_VECTOR_TIMES_MATRIX	144U
#define SPV_OP_MATRIX_TIMES_VECTOR	145U
#define SPV_OP_MATRIX_TIMES_MATRIX	146U
#define SPV_OP_OUTER_PRODUCT		147U
#define SPV_OP_DOT			148U
#define SPV_OP_I_ADD_CARRY		149U
#define SPV_OP_I_SUB_BORROW		150U
#define SPV_OP_U_MUL_EXTENDED		151U
#define SPV_OP_S_MUL_EXTENDED		152U
#define SPV_OP_LOGICAL_EQUAL		164U
#define SPV_OP_LOGICAL_NOT_EQUAL	165U
#define SPV_OP_LOGICAL_OR		166U
#define SPV_OP_LOGICAL_AND		167U
#define SPV_OP_LOGICAL_NOT		168U
#define SPV_OP_SELECT			169U
#define SPV_OP_I_EQUAL			170U
#define SPV_OP_I_NOT_EQUAL		171U
#define SPV_OP_U_GREATER_THAN		172U
#define SPV_OP_S_GREATER_THAN		173U
#define SPV_OP_U_GREATER_THAN_EQUAL	174U
#define SPV_OP_S_GREATER_THAN_EQUAL	175U
#define SPV_OP_U_LESS_THAN		176U
#define SPV_OP_S_LESS_THAN		177U
#define SPV_OP_U_LESS_THAN_EQUAL	178U
#define SPV_OP_S_LESS_THAN_EQUAL	179U
#define SPV_OP_F_ORD_EQUAL		180U
#define SPV_OP_F_UNORD_NOT_EQUAL	183U
#define SPV_OP_F_ORD_NOT_EQUAL		182U
#define SPV_OP_F_ORD_LESS_THAN		184U
#define SPV_OP_F_ORD_GREATER_THAN	186U
#define SPV_OP_F_ORD_LESS_THAN_EQUAL	188U
#define SPV_OP_F_ORD_GREATER_THAN_EQUAL	190U
#define SPV_OP_SHIFT_RIGHT_LOGICAL	194U
#define SPV_OP_SHIFT_RIGHT_ARITHMETIC	195U
#define SPV_OP_SHIFT_LEFT_LOGICAL	196U
#define SPV_OP_BITWISE_OR		197U
#define SPV_OP_BITWISE_XOR		198U
#define SPV_OP_BITWISE_AND		199U
#define SPV_OP_NOT			200U
#define SPV_OP_DPDY			208U
#define SPV_OP_LOOP_MERGE		246U
#define SPV_OP_SELECTION_MERGE		247U
#define SPV_OP_LABEL			248U
#define SPV_OP_BRANCH			249U
#define SPV_OP_BRANCH_CONDITIONAL	250U
#define SPV_OP_KILL			252U
#define SPV_OP_RETURN			253U
#define SPV_OP_UNREACHABLE		255U

/* A compute shader's (ws101-p008): a run-time array's length, the barriers and the atomics. */
#define SPV_OP_ARRAY_LENGTH		68U
#define SPV_OP_CONTROL_BARRIER		224U
#define SPV_OP_MEMORY_BARRIER		225U
#define SPV_OP_ATOMIC_EXCHANGE		229U
#define SPV_OP_ATOMIC_COMPARE_EXCHANGE	230U
#define SPV_OP_ATOMIC_I_ADD		234U
#define SPV_OP_ATOMIC_S_MIN		236U
#define SPV_OP_ATOMIC_U_MIN		237U
#define SPV_OP_ATOMIC_S_MAX		238U
#define SPV_OP_ATOMIC_U_MAX		239U
#define SPV_OP_ATOMIC_AND		240U
#define SPV_OP_ATOMIC_OR		241U
#define SPV_OP_ATOMIC_XOR		242U

/* Storage images (GLSL ES 3.10's images): a texel's pointer, a read and a write, their storage class and decorations. */
#define SPV_OP_IMAGE_TEXEL_POINTER	60U
#define SPV_OP_IMAGE_READ		98U
#define SPV_OP_IMAGE_WRITE		99U
#define SPV_STORAGE_IMAGE		11U
#define SPV_DECORATION_NON_WRITABLE	24U
#define SPV_DECORATION_NON_READABLE	25U
#define SPV_SEMANTICS_IMAGE_MEMORY	0x800U

/* Decorations. */
#define SPV_DECORATION_BLOCK		2U
#define SPV_DECORATION_BUFFER_BLOCK	3U
#define SPV_DECORATION_ROW_MAJOR	4U
#define SPV_DECORATION_COL_MAJOR	5U
#define SPV_DECORATION_ARRAY_STRIDE	6U
#define SPV_DECORATION_MATRIX_STRIDE	7U
#define SPV_DECORATION_BUILT_IN		11U
#define SPV_DECORATION_NO_PERSPECTIVE	13U
#define SPV_DECORATION_FLAT		14U
#define SPV_DECORATION_CENTROID		16U
#define SPV_DECORATION_INVARIANT	18U
#define SPV_DECORATION_LOCATION		30U
#define SPV_DECORATION_INDEX		32U
#define SPV_DECORATION_BINDING		33U
#define SPV_DECORATION_DESCRIPTOR_SET	34U
#define SPV_DECORATION_OFFSET		35U
#define SPV_DECORATION_RESTRICT		19U
#define SPV_DECORATION_VOLATILE		21U
#define SPV_DECORATION_COHERENT		23U
#define SPV_DECORATION_NON_WRITABLE	24U
#define SPV_DECORATION_NON_READABLE	25U

/* Built-ins. */
#define SPV_BUILT_IN_POSITION		0U
#define SPV_BUILT_IN_POINT_SIZE		1U
#define SPV_BUILT_IN_PRIMITIVE_ID	7U
#define SPV_BUILT_IN_LAYER		9U
#define SPV_BUILT_IN_FRAG_COORD		15U
#define SPV_BUILT_IN_POINT_COORD	16U
#define SPV_BUILT_IN_FRONT_FACING	17U
#define SPV_BUILT_IN_FRAG_DEPTH		22U
#define SPV_BUILT_IN_VERTEX_INDEX	42U
#define SPV_BUILT_IN_INSTANCE_INDEX	43U
#define SPV_BUILT_IN_NUM_WORKGROUPS	24U
#define SPV_BUILT_IN_WORKGROUP_ID	26U
#define SPV_BUILT_IN_LOCAL_INVOCATION_ID 27U
#define SPV_BUILT_IN_GLOBAL_INVOCATION_ID 28U
#define SPV_BUILT_IN_LOCAL_INVOCATION_INDEX 29U

/* Storage classes. */
#define SPV_STORAGE_UNIFORM_CONSTANT	0U
#define SPV_STORAGE_INPUT		1U
#define SPV_STORAGE_UNIFORM		2U
#define SPV_STORAGE_OUTPUT		3U
#define SPV_STORAGE_FUNCTION		7U
#define SPV_STORAGE_WORKGROUP		4U

/* Scopes and memory semantics of the barriers and atomics (ws101-p008). */
#define SPV_SCOPE_DEVICE		1U
#define SPV_SCOPE_WORKGROUP		2U
#define SPV_SCOPE_INVOCATION		4U
#define SPV_SEMANTICS_ACQUIRE_RELEASE	0x8U
#define SPV_SEMANTICS_UNIFORM_MEMORY	0x40U
#define SPV_SEMANTICS_WORKGROUP_MEMORY	0x100U

/* Capabilities, execution models and modes, image operands. */
#define SPV_CAPABILITY_SHADER		1U
#define SPV_CAPABILITY_SAMPLED_1D	43U
#define SPV_CAPABILITY_IMAGE_QUERY	50U
#define SPV_CAPABILITY_SAMPLED_BUFFER	46U
#define SPV_CAPABILITY_SAMPLED_CUBE_ARRAY 45U
#define SPV_CAPABILITY_FLOAT64		10U
#define SPV_MODEL_VERTEX		0U
#define SPV_MODEL_GEOMETRY		3U
#define SPV_MODEL_FRAGMENT		4U
#define SPV_MODEL_GL_COMPUTE		5U
#define SPV_MODE_LOCAL_SIZE		17U
#define SPV_MODE_INVOCATIONS		0U
#define SPV_MODE_ORIGIN_UPPER_LEFT	7U
#define SPV_MODE_DEPTH_REPLACING	12U
#define SPV_MODE_INPUT_POINTS		19U
#define SPV_MODE_OUTPUT_VERTICES	26U
#define SPV_MODE_OUTPUT_POINTS		27U
#define SPV_MODE_OUTPUT_LINE_STRIP	28U
#define SPV_MODE_OUTPUT_TRIANGLE_STRIP	29U
#define SPV_CAPABILITY_GEOMETRY		2U

/* Tessellation: the capability, the two execution models, their modes, the built-ins and the Patch decoration. */
#define SPV_CAPABILITY_TESSELLATION	3U
#define SPV_MODEL_TESS_CONTROL		1U
#define SPV_MODEL_TESS_EVALUATION	2U
#define SPV_MODE_SPACING_EQUAL		1U
#define SPV_MODE_SPACING_FRACTIONAL_EVEN 2U
#define SPV_MODE_SPACING_FRACTIONAL_ODD	3U
#define SPV_MODE_VERTEX_ORDER_CW	4U
#define SPV_MODE_VERTEX_ORDER_CCW	5U
#define SPV_MODE_POINT_MODE		10U
#define SPV_MODE_TRIANGLES		22U
#define SPV_MODE_QUADS			24U
#define SPV_MODE_ISOLINES		25U
#define SPV_BUILT_IN_INVOCATION_ID	8U
#define SPV_BUILT_IN_TESS_LEVEL_OUTER	11U
#define SPV_BUILT_IN_TESS_LEVEL_INNER	12U
#define SPV_BUILT_IN_TESS_COORD		13U
#define SPV_BUILT_IN_PATCH_VERTICES	14U
#define SPV_DECORATION_PATCH		15U

/* Per-sample shading: the capability and the built-ins. */
#define SPV_CAPABILITY_SAMPLE_RATE_SHADING 35U
#define SPV_BUILT_IN_SAMPLE_ID		18U
#define SPV_BUILT_IN_SAMPLE_POSITION	19U
#define SPV_BUILT_IN_SAMPLE_MASK	20U
#define SPV_OP_EMIT_VERTEX		218U
#define SPV_OP_END_PRIMITIVE		219U
#define SPV_IMAGE_OPERAND_BIAS		1U
#define SPV_IMAGE_OPERAND_LOD		2U
#define SPV_IMAGE_OPERAND_GRAD		4U
#define SPV_IMAGE_OPERAND_CONST_OFFSET	8U

/*
 * GL_ARB_gpu_shader5: a gather's computed offset and four constant
 * offsets (ImageGatherExtended), the depth comparison's gather, the
 * interpolation of an input elsewhere (InterpolationFunction, and
 * GLSL.std.450's instructions), and an operation not to contract.
 */
#define SPV_IMAGE_OPERAND_OFFSET	0x10U
#define SPV_IMAGE_OPERAND_CONST_OFFSETS	0x20U
#define SPV_OP_IMAGE_DREF_GATHER	97U
#define SPV_CAPABILITY_IMAGE_GATHER_EXTENDED 25U
#define SPV_CAPABILITY_INTERPOLATION_FUNCTION 52U
#define SPV_DECORATION_NO_CONTRACTION	42U
#define EMIT_INTERPOLATE_AT_CENTROID	76U
#define EMIT_INTERPOLATE_AT_SAMPLE	77U
#define EMIT_INTERPOLATE_AT_OFFSET	78U

/* The most words one instruction the back end writes has. */
#define GLSL_MAX_OPERANDS		64U

/*
 * A growing sequence of SPIR-V words (one section of a module).
 */
struct glsl_words {
	uint32_t *data;
	size_t count;
	size_t capacity;
};

/*
 * A type or constant instruction the module already has, found again by
 * its words (so each is declared once).
 */
struct glsl_module_entry {
	uint32_t hash;
	size_t offset;
	uint32_t length;
	uint32_t id;
};

/*
 * A SPIR-V module being built: its sections in the order SPIR-V wants
 * them, the ids handed out, and the table of types and constants.
 *
 * Every section grows in the arena of the link; running out of memory
 * jumps to the link's failure point.
 */
struct glsl_module {
	struct glsl_arena *arena;

	/* The next id to hand out. */
	uint32_t next_id;

	/* The sections. */
	struct glsl_words capabilities;
	struct glsl_words imports;
	struct glsl_words entry_points;
	struct glsl_words modes;
	struct glsl_words names;
	struct glsl_words decorations;
	struct glsl_words globals;
	struct glsl_words variables;
	struct glsl_words body;

	/* The types and constants declared, by their words. */
	struct glsl_module_entry *entries;
	unsigned entry_count;
	unsigned entry_capacity;

	/* The GLSL.std.450 import, and the capabilities declared. */
	uint32_t std450;
	unsigned sampled_1d;
	unsigned image_query;
	unsigned sampled_buffer;
	unsigned sampled_cube_array;
	unsigned geometry;
	unsigned tessellation;
	unsigned sample_rate_shading;
	unsigned float64;
	unsigned image_gather_extended;
	unsigned interpolation_function;
};

/*
 * A value an expression computed: its id and its GLSL type.
 */
struct emit_value {
	uint32_t id;
	const struct glsl_type *type;
};

/*
 * Where an lvalue (or a variable read through a path) is: a variable, an
 * access chain into it, and a swizzle of the vector at the end.
 */
struct emit_path {
	/*
	 * The variable, its storage class, and the layout its memory has:
	 * EMIT_LAYOUT_STD140 (a uniform block, the default one or a named
	 * one), EMIT_LAYOUT_STD430 (a shader storage block, ws101-p008), 0
	 * (a variable of the local types).
	 */
	uint32_t base;
	unsigned storage;
	int block;

	/* The access chain's index ids, and the GLSL type they reach. */
	uint32_t indices[16];
	unsigned index_count;
	const struct glsl_type *type;

	/* A swizzle of that vector (count 0: none), and the vector's type. */
	unsigned swizzle[4];
	unsigned swizzle_count;
	const struct glsl_type *vector;
};

/* The layouts of emit_path.block. */
#define EMIT_LAYOUT_STD140	1
#define EMIT_LAYOUT_STD430	2

/*
 * A construct break and continue can leave: a loop, a switch (a loop
 * that runs once), or an inlined function with early returns (also a
 * loop that runs once), and the inlined function it belongs to.
 */
struct emit_target {
	uint32_t merge;
	uint32_t continue_label;
	unsigned kind;
	struct emit_frame *frame;
};

/* The kinds of break targets. */
#define EMIT_TARGET_LOOP	1U
#define EMIT_TARGET_SWITCH	2U
#define EMIT_TARGET_FRAME	3U

/*
 * A function being inlined: the variable its return value goes to, and
 * for a function with early returns the flag that says it returned.
 */
struct emit_frame {
	struct glsl_function *function;
	uint32_t result;
	uint32_t returned;
	unsigned target;
	struct emit_frame *outer;
};

/*
 * One uniform of a program as the link laid it out: the name, the type,
 * its offset in the default uniform block or its sampler binding.
 */
struct glsl_link_uniform {
	const char *name;
	const struct glsl_type *type;
	uint32_t offset;
	uint32_t binding;
	unsigned sampler;
};

/*
 * One output a vertex shader captures (transform feedback): its variable,
 * and its first word in a vertex's record.
 */
struct glsl_link_capture {
	struct glsl_symbol *symbol;
	unsigned offset;
};

/*
 * The emitter's state for one stage.
 */
struct emit_state {
	/* The shader, the module, and the link's uniforms. */
	struct glsl_shader *shader;
	struct glsl_module *module;
	struct glsl_link_uniform *uniforms;
	unsigned uniform_count;

	/* The GLSL types given ids so far: plain ones, the std140 laid-out ones and the std430 ones (ws101-p008). */
	const struct glsl_type **type_keys;
	uint32_t *type_ids;
	uint32_t *layout_ids;
	uint32_t *layout430_ids;
	unsigned type_count;
	unsigned type_capacity;

	/* The block, its type, and each uniform's member in it (-1 when the stage does not use it). */
	uint32_t block;
	int *members;

	/* The interface variables of the entry point. */
	uint32_t interface[64];
	unsigned interface_count;

	/* The current block: whether it ended with a branch, return or kill. */
	int terminated;

	/* The break targets and the inlined functions. */
	struct emit_target targets[64];
	unsigned target_count;
	struct emit_frame *frame;

	/* Whether the shader writes gl_FragDepth. */
	unsigned depth_written;

	/*
	 * The outputs a vertex shader captures (none: 0), the words of a
	 * vertex's record, the storage buffer they go into, and the vertex's
	 * and the instance's numbers they are placed by.
	 */
	const struct glsl_link_capture *captures;
	unsigned capture_count;
	unsigned capture_stride;
	uint32_t capture_buffer;
	struct glsl_symbol *vertex_id;
	struct glsl_symbol *instance_id;

	/*
	 * A geometry shader that captures or emits on a stream other than 0
	 * (glsl.h; capture_geometry nonzero): its input primitive's and
	 * invocation's numbers (NULL: 0), main's variables of the invocation's
	 * number, the next vertex's place, the streams whose next vertex
	 * starts a strip (bit i stream i) and each stream's vertices in its
	 * strip, and the vertices of a primitive it outputs.
	 */
	int capture_geometry;
	struct glsl_symbol *primitive_id;
	struct glsl_symbol *invocation_id;
	uint32_t capture_token;
	uint32_t capture_sequence;
	uint32_t capture_starts;
	uint32_t capture_strip[4];
	unsigned capture_primitive_vertices;
};

/* module.c: building a module. */
void glsl_module_init(struct glsl_module *module, struct glsl_arena *arena);
uint32_t glsl_module_id(struct glsl_module *module);
void glsl_words_add(struct glsl_module *module, struct glsl_words *words, uint32_t opcode, const uint32_t *operands, unsigned count);
void glsl_words_string(struct glsl_module *module, struct glsl_words *words, uint32_t opcode, const uint32_t *before, unsigned before_count, const char *text);
uint32_t glsl_module_declare(struct glsl_module *module, uint32_t opcode, const uint32_t *operands, unsigned count, int unique);
uint32_t glsl_module_constant(struct glsl_module *module, uint32_t type, uint32_t value);
uint32_t glsl_module_constant64(struct glsl_module *module, uint32_t type, uint64_t value);
uint32_t glsl_module_composite(struct glsl_module *module, uint32_t type, const uint32_t *parts, unsigned count);
uint32_t glsl_module_bool(struct glsl_module *module, uint32_t type, int value);
void glsl_module_decorate(struct glsl_module *module, uint32_t target, uint32_t decoration, const uint32_t *operands, unsigned count);
void glsl_module_member_decorate(struct glsl_module *module, uint32_t target, uint32_t member, uint32_t decoration, uint32_t operand);
void glsl_module_name(struct glsl_module *module, uint32_t target, const char *name);
void glsl_module_member_name(struct glsl_module *module, uint32_t target, uint32_t member, const char *name);
uint32_t *glsl_module_finish(struct glsl_module *module, size_t *words);

/* emit.c: the emitter. */
uint32_t *glsl_emit(struct glsl_shader *shader, struct glsl_arena *arena, struct glsl_link_uniform *uniforms, unsigned uniform_count, const struct glsl_link_capture *captures, unsigned capture_count, unsigned capture_stride, size_t *words);
uint32_t glsl_emit_type(struct emit_state *state, const struct glsl_type *type);
uint32_t glsl_emit_pointer(struct emit_state *state, unsigned storage, uint32_t type);
uint32_t glsl_emit_image_type(struct emit_state *state, const struct glsl_type *type);
void glsl_emit_capability(struct emit_state *state, uint32_t capability, unsigned *declared);
uint32_t glsl_emit_int(struct emit_state *state, int32_t value);
uint32_t glsl_emit_float(struct emit_state *state, float value);
uint32_t glsl_emit_floating(struct emit_state *state, unsigned base, double value);
uint32_t glsl_emit_splat(struct emit_state *state, uint32_t scalar, const struct glsl_type *type);
uint32_t glsl_emit_op(struct emit_state *state, uint32_t opcode, uint32_t result_type, const uint32_t *operands, unsigned count);
uint32_t glsl_emit_extract(struct emit_state *state, struct emit_value value, unsigned index);
struct emit_value glsl_emit_expression(struct emit_state *state, struct glsl_node *node);
struct emit_value glsl_emit_convert(struct emit_state *state, struct emit_value value, const struct glsl_type *target);
uint32_t glsl_emit_all(struct emit_state *state, uint32_t vector, unsigned components, int any);
int glsl_capture_counted(const struct glsl_shader *shader, unsigned capture_count);
void glsl_emit_capture_vertex(struct emit_state *state, unsigned stream);
void glsl_emit_capture_end(struct emit_state *state, unsigned stream);

/* emit-builtin.c: built-in functions. */
struct emit_value glsl_emit_builtin(struct emit_state *state, struct glsl_node *node, struct emit_value *arguments, unsigned count);

#endif
