/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * SPIR-V parser producing a SCALAR IR (see ir.h).
 *
 * The opcode and enumerant numbers are from the public Khronos SPIR-V
 * specification.  Every IR value is one 32-bit scalar -- a float, an integer
 * or a Boolean; a SPIR-V vector is a group of up to four IR values, one per
 * component, and a matrix a group of up to sixteen, column after column.
 * That is what lets the operations that only rearrange components --
 * OpCompositeConstruct, OpCompositeExtract, OpVectorShuffle, OpTranspose,
 * OpBitcast, component access chains -- be lowered exactly: they emit no IR
 * at all, they only name which scalar is which.
 *
 * Structured selection (OpSelectionMerge, OpBranchConditional, OpBranch,
 * OpPhi, OpKill, OpReturn) is IF-CONVERTED: the blocks are lowered in their
 * order, each under a predicate -- the IR Boolean of the channels that run
 * it, built from the conditions of the branches that lead to it -- and every
 * block runs for every channel.  That is exact because nothing a block does
 * is visible outside it except through a store, a phi or a discard, and each
 * of those takes the predicate: a store keeps the old value where the
 * predicate is false (SELECT), a phi selects by the predicates of its edges,
 * a discard discards only where its predicate is true.  OpSwitch is refused.
 *
 * A structured loop (OpLoopMerge) keeps the same model inside one pass of
 * its body, and the IR runs the body again (LOOP_BEGIN .. LOOP_END) while
 * any channel is still in the loop.  What one pass leaves to the next lives
 * in loop variables, IR values the parser moves into before the loop and
 * again at the back edge (MOVE): the channels still in the loop (`active`,
 * the predicate of the header), each header phi, and each component of a
 * local variable or an output that held a value when the loop began.  A
 * break is an edge to the merge block and a continue an edge to the continue
 * target, like any edge; the back edge's predicate is the channels that go
 * round again, and the merge block runs for the channels that entered the
 * loop.  A value made inside a loop is not read after it (such a module is
 * refused), except through a loop variable.
 *
 * Function-storage variables are not memory.  A load reads the value of the
 * latest store to that component, merged with the older one under the
 * predicate of the storing block: the parser keeps, per local variable and
 * component, the IR value currently stored there (store-to-load forwarding).
 * A load of a component that was never stored is refused.  An output read
 * back by the shader gives the value last written to it the same way.
 *
 * Push constants and uniform blocks are memory the draw delivers with the
 * push data: a load reads words at byte offsets the access chain works out
 * from the Offset, ArrayStride, MatrixStride and ColMajor / RowMajor
 * decorations.  One array index of such a chain may be dynamic: every
 * element of the array is loaded and the one the index names is selected
 * channel by channel.
 *
 * Inside a function body an instruction with execution semantics that is not
 * lowered FAILS the parse (ENOTSUP, with a diagnostic) -- it is never skipped,
 * because a skipped instruction is a different shader.  The same holds for
 * decorations: the ones that place or identify data are interpreted, the ones
 * listed as having no effect on this lowering are ignored by name, and any
 * other decoration is refused.  EINVAL is kept for malformed modules.
 */

#include "compiler.h"
#include <kern/kcrt.h>

#include <kern/kmem.h>

#include <uapi/errno.h>

/* SPIR-V module header (Khronos SPIR-V spec, section 2.3). */
#define SPIRV_MAGIC 0x07230203U

/* The words of the module header; the first instruction follows them. */
#define SPIRV_HEADER_WORDS 5U

/* The largest id bound the parser accepts. */
#define SPIRV_MAX_BOUND 65536U

/* Opcodes (Khronos SPIR-V spec, section 3.37). */
#define OP_NOP 0U
#define OP_SOURCE_CONTINUED 2U
#define OP_SOURCE 3U
#define OP_SOURCE_EXTENSION 4U
#define OP_NAME 5U
#define OP_MEMBER_NAME 6U
#define OP_STRING 7U
#define OP_LINE 8U
#define OP_EXTENSION 10U
#define OP_EXT_INST_IMPORT 11U
#define OP_EXT_INST 12U
#define OP_MEMORY_MODEL 14U
#define OP_ENTRY_POINT 15U
#define OP_EXECUTION_MODE 16U
#define OP_CAPABILITY 17U
#define OP_TYPE_VOID 19U
#define OP_TYPE_BOOL 20U
#define OP_TYPE_INT 21U
#define OP_TYPE_FLOAT 22U
#define OP_TYPE_VECTOR 23U
#define OP_TYPE_MATRIX 24U
#define OP_TYPE_IMAGE 25U
#define OP_TYPE_SAMPLER 26U
#define OP_TYPE_SAMPLED_IMAGE 27U
#define OP_TYPE_ARRAY 28U
#define OP_TYPE_STRUCT 30U
#define OP_TYPE_POINTER 32U
#define OP_TYPE_FUNCTION 33U
#define OP_CONSTANT_TRUE 41U
#define OP_CONSTANT_FALSE 42U
#define OP_CONSTANT 43U
#define OP_CONSTANT_COMPOSITE 44U
#define OP_FUNCTION 54U
#define OP_FUNCTION_END 56U
#define OP_VARIABLE 59U
#define OP_LOAD 61U
#define OP_STORE 62U
#define OP_ACCESS_CHAIN 65U
#define OP_DECORATE 71U
#define OP_MEMBER_DECORATE 72U
#define OP_VECTOR_SHUFFLE 79U
#define OP_COMPOSITE_CONSTRUCT 80U
#define OP_COMPOSITE_EXTRACT 81U
#define OP_TRANSPOSE 84U
#define OP_IMAGE_SAMPLE_IMPLICIT_LOD 87U
#define OP_CONVERT_F_TO_U 109U
#define OP_CONVERT_F_TO_S 110U
#define OP_CONVERT_S_TO_F 111U
#define OP_CONVERT_U_TO_F 112U
#define OP_BITCAST 124U
#define OP_SNEGATE 126U
#define OP_FNEGATE 127U
#define OP_IADD 128U
#define OP_FADD 129U
#define OP_ISUB 130U
#define OP_FSUB 131U
#define OP_IMUL 132U
#define OP_FMUL 133U
#define OP_UDIV 134U
#define OP_SDIV 135U
#define OP_FDIV 136U
#define OP_UMOD 137U
#define OP_SREM 138U
#define OP_SMOD 139U
#define OP_FREM 140U
#define OP_FMOD 141U
#define OP_VECTOR_TIMES_SCALAR 142U
#define OP_MATRIX_TIMES_SCALAR 143U
#define OP_VECTOR_TIMES_MATRIX 144U
#define OP_MATRIX_TIMES_VECTOR 145U
#define OP_MATRIX_TIMES_MATRIX 146U
#define OP_OUTER_PRODUCT 147U
#define OP_DOT 148U
#define OP_LOGICAL_EQUAL 164U
#define OP_LOGICAL_NOT_EQUAL 165U
#define OP_LOGICAL_OR 166U
#define OP_LOGICAL_AND 167U
#define OP_LOGICAL_NOT 168U
#define OP_SELECT 169U
#define OP_IEQUAL 170U
#define OP_INOT_EQUAL 171U
#define OP_UGREATER_THAN 172U
#define OP_SGREATER_THAN 173U
#define OP_UGREATER_THAN_EQUAL 174U
#define OP_SGREATER_THAN_EQUAL 175U
#define OP_ULESS_THAN 176U
#define OP_SLESS_THAN 177U
#define OP_ULESS_THAN_EQUAL 178U
#define OP_SLESS_THAN_EQUAL 179U
#define OP_FORD_EQUAL 180U
#define OP_FUNORD_EQUAL 181U
#define OP_FORD_NOT_EQUAL 182U
#define OP_FUNORD_NOT_EQUAL 183U
#define OP_FORD_LESS_THAN 184U
#define OP_FUNORD_LESS_THAN 185U
#define OP_FORD_GREATER_THAN 186U
#define OP_FUNORD_GREATER_THAN 187U
#define OP_FORD_LESS_THAN_EQUAL 188U
#define OP_FUNORD_LESS_THAN_EQUAL 189U
#define OP_FORD_GREATER_THAN_EQUAL 190U
#define OP_FUNORD_GREATER_THAN_EQUAL 191U
#define OP_SHIFT_RIGHT_LOGICAL 194U
#define OP_SHIFT_RIGHT_ARITHMETIC 195U
#define OP_SHIFT_LEFT_LOGICAL 196U
#define OP_BITWISE_OR 197U
#define OP_BITWISE_XOR 198U
#define OP_BITWISE_AND 199U
#define OP_NOT 200U
#define OP_PHI 245U
#define OP_LOOP_MERGE 246U
#define OP_SELECTION_MERGE 247U
#define OP_LABEL 248U
#define OP_BRANCH 249U
#define OP_BRANCH_CONDITIONAL 250U
#define OP_SWITCH 251U
#define OP_KILL 252U
#define OP_RETURN 253U
#define OP_UNREACHABLE 255U
#define OP_NO_LINE 317U
#define OP_MODULE_PROCESSED 330U

/* Storage classes (SPIR-V spec, section 3.7). */
#define SC_UNIFORM_CONSTANT 0U
#define SC_INPUT 1U
#define SC_UNIFORM 2U
#define SC_OUTPUT 3U
#define SC_FUNCTION 7U
#define SC_PUSH_CONSTANT 9U

/* Decorations (SPIR-V spec, section 3.20). */
#define DEC_RELAXED_PRECISION 0U
#define DEC_BLOCK 2U
#define DEC_ROW_MAJOR 4U
#define DEC_COL_MAJOR 5U
#define DEC_ARRAY_STRIDE 6U
#define DEC_MATRIX_STRIDE 7U
#define DEC_BUILTIN 11U
#define DEC_LOCATION 30U
#define DEC_BINDING 33U
#define DEC_DESCRIPTOR_SET 34U
#define DEC_OFFSET 35U

/* BuiltIn values (SPIR-V spec, section 3.21). */
#define BUILTIN_POSITION 0U

/* Execution models (SPIR-V spec, section 3.3). */
#define EM_VERTEX 0U
#define EM_FRAGMENT 4U

/* GLSL.std.450 extended instruction numbers. */
#define GLSL_ROUND 1U
#define GLSL_ROUND_EVEN 2U
#define GLSL_TRUNC 3U
#define GLSL_FABS 4U
#define GLSL_SABS 5U
#define GLSL_FSIGN 6U
#define GLSL_SSIGN 7U
#define GLSL_FLOOR 8U
#define GLSL_CEIL 9U
#define GLSL_FRACT 10U
#define GLSL_RADIANS 11U
#define GLSL_DEGREES 12U
#define GLSL_SIN 13U
#define GLSL_COS 14U
#define GLSL_TAN 15U
#define GLSL_POW 26U
#define GLSL_EXP 27U
#define GLSL_LOG 28U
#define GLSL_EXP2 29U
#define GLSL_LOG2 30U
#define GLSL_SQRT 31U
#define GLSL_INVERSE_SQRT 32U
#define GLSL_FMIN 37U
#define GLSL_UMIN 38U
#define GLSL_SMIN 39U
#define GLSL_FMAX 40U
#define GLSL_UMAX 41U
#define GLSL_SMAX 42U
#define GLSL_FCLAMP 43U
#define GLSL_UCLAMP 44U
#define GLSL_SCLAMP 45U
#define GLSL_FMIX 46U
#define GLSL_STEP 48U
#define GLSL_SMOOTH_STEP 49U
#define GLSL_LENGTH 66U
#define GLSL_DISTANCE 67U
#define GLSL_CROSS 68U
#define GLSL_NORMALIZE 69U
#define GLSL_REFLECT 71U

/* An IR value slot that names no value. */
#define NO_VALUE 0xFFFFFFFFU

/*
 * The predicate of a block every channel that entered the function runs:
 * the entry block and whatever every channel reaches again.
 */
#define PREDICATE_ALWAYS 0xFFFFFFFEU

/* The bits of a true Boolean, and of the floats 0, 0.5, 1, 2 and 3. */
#define BOOL_TRUE_BITS 0xFFFFFFFFU
#define FLOAT_ZERO_BITS 0x00000000U
#define FLOAT_HALF_BITS 0x3F000000U
#define FLOAT_ONE_BITS 0x3F800000U
#define FLOAT_TWO_BITS 0x40000000U
#define FLOAT_THREE_BITS 0x40400000U

/* The bits of log2(e), 1 / log2(e), pi / 180 and 180 / pi as floats. */
#define FLOAT_LOG2_E_BITS 0x3FB8AA3BU
#define FLOAT_LN_2_BITS 0x3F317218U
#define FLOAT_DEGREE_BITS 0x3C8EFA35U
#define FLOAT_RADIAN_BITS 0x42652EE1U

/* The deepest nesting of selection and loop constructs the parser follows. */
#define MAX_CONSTRUCT_DEPTH 32U

/* The deepest nesting of loops the parser follows, and the most loops of a function. */
#define MAX_LOOP_DEPTH 8U
#define MAX_LOOPS 64U

/* The most phis a loop header may have. */
#define MAX_LOOP_PHIS 32U

/* The most members a structure type may have. */
#define MAX_MEMBERS 16U

/* The most scalars a value may have: a 4 x 4 matrix. */
#define MAX_COMPONENTS 16U

/* The most elements an array read through a dynamic index may have. */
#define MAX_DYNAMIC_ELEMENTS 16U

/* The OpVectorShuffle component index that means "undefined". */
#define SHUFFLE_UNDEFINED 0xFFFFFFFFU

/*
 * The IR instructions the stream starts with room for, per SPIR-V
 * instruction of the body: a four-component dot product is four multiplies
 * and three adds.  A longer lowering (a normalize, a mix, a phi of many
 * edges, a matrix product) grows the stream.
 */
#define IR_PER_INSTRUCTION 8U

/* Pointer target kinds. */
#define PTR_NONE 0U
#define PTR_INPUT 1U
#define PTR_OUTPUT 2U
#define PTR_OUTPUT_BLOCK 3U	/* gl_PerVertex: the members carry the builtins */
#define PTR_PUSH 4U
#define PTR_SAMPLER 5U
#define PTR_LOCAL 6U
#define PTR_UBO 7U

/* What the scalars of a value are, as a type says. */
#define SCALAR_NONE 0U
#define SCALAR_FLOAT 1U
#define SCALAR_INT 2U
#define SCALAR_BOOL 3U

/*
 * What a SPIR-V id is.
 *
 * Every id starts as ID_NONE; the declaration or instruction that defines it
 * sets the kind once.
 */
enum i915_spirv_id_kind {
	ID_NONE = 0,
	ID_TYPE_VOID,
	ID_TYPE_BOOL,
	ID_TYPE_INT,
	ID_TYPE_FLOAT,
	ID_TYPE_VECTOR,
	ID_TYPE_MATRIX,
	ID_TYPE_IMAGE,
	ID_TYPE_SAMPLER,
	ID_TYPE_SAMPLED_IMAGE,
	ID_TYPE_ARRAY,
	ID_TYPE_STRUCT,
	ID_TYPE_POINTER,
	ID_TYPE_FUNCTION,

	/* A scalar int, float or Boolean constant. */
	ID_CONSTANT,

	/* A vector or matrix constant: member_type[] names the constituent constants. */
	ID_CONSTANT_COMPOSITE,

	/* A block label: comp[0] is the predicate of the block once it is reached. */
	ID_LABEL,

	/* An OpVariable. */
	ID_VARIABLE,

	/* An OpAccessChain result. */
	ID_POINTER,

	/* A float, integer or Boolean scalar, vector or matrix value: comp[] names the IR scalars. */
	ID_VALUE,

	/* A loaded combined image sampler. */
	ID_SAMPLED_IMAGE,

	ID_EXT_SET
};

/*
 * What the parser knows about one SPIR-V id.
 *
 * The parser holds one per id below the module's bound for the length of one
 * parse; the record of a local variable also carries the scalars currently
 * stored in it.
 */
struct i915_spirv_id {
	/* An enum i915_spirv_id_kind. */
	uint8_t kind;

	/* Int or float bit width. */
	uint8_t width;

	/* Vector component count; matrix column count; struct member count; value component count. */
	uint8_t count;

	uint8_t ptr_kind;
	uint8_t has_location;
	uint8_t has_binding;
	uint8_t has_set;
	uint8_t has_builtin;

	/* Pointer type or variable storage class. */
	uint16_t storage;

	/* Element, pointee, column or value type id. */
	uint32_t type;

	uint32_t location;
	uint32_t binding;
	uint32_t set;
	uint32_t builtin;

	/* Constant bits. */
	uint32_t constant;

	/* Array types: the element count and the ArrayStride in bytes (0 when undecorated). */
	uint32_t length;
	uint32_t stride;

	/* Uniform blocks: the index of the block in the IR's uniform list. */
	uint32_t uniform;

	/* Structure types: member types and offsets. */
	uint32_t member_type[MAX_MEMBERS];
	uint32_t member_offset[MAX_MEMBERS];

	/* The member's builtin plus one; zero when the member is not a builtin. */
	uint32_t member_builtin[MAX_MEMBERS];

	/* A matrix member's MatrixStride, and whether it is RowMajor. */
	uint32_t member_matrix_stride[MAX_MEMBERS];
	uint8_t member_row_major[MAX_MEMBERS];

	uint8_t member_has_offset[MAX_MEMBERS];

	/* Values; locals keep their CURRENT stored scalars here too. */
	uint32_t comp[MAX_COMPONENTS];

	/* Pointers (variables and access chains): the OpVariable the pointer leads to. */
	uint32_t var;

	/* The type id of what the pointer addresses now. */
	uint32_t pointee;

	/* The struct member selected, or -1. */
	int32_t member;

	/* A pointer into a local, an input or an output: the first scalar it addresses, or -1 for 0. */
	int32_t component;

	/*
	 * A pointer into push constants or a uniform block: the byte offset of
	 * what it addresses, the bytes between the components of an addressed
	 * vector, and the layout of an addressed matrix.
	 */
	uint32_t byte_offset;
	uint32_t component_stride;
	uint32_t matrix_stride;
	uint8_t row_major;

	/*
	 * The one dynamic array index such a pointer may carry: the IR integer
	 * of the index (NO_VALUE for none), the array's stride and element
	 * count.
	 */
	uint32_t dynamic_index;
	uint32_t dynamic_stride;
	uint32_t dynamic_length;
};

/*
 * One control-flow edge of the body, from a block's terminator to a block.
 *
 * `predicate` is the IR Boolean of the channels that take the edge, or
 * PREDICATE_ALWAYS.  The parser keeps every edge until the parse ends: the
 * target's predicate and its phis are built from them.
 */
struct i915_spirv_edge {
	uint32_t from;
	uint32_t to;
	uint32_t predicate;
};

/*
 * One selection or loop construct the walk is inside: from its header to
 * its merge block.
 *
 * Every channel that runs the header of a selection reaches the merge
 * block unless a block inside returns or branches out past the merge;
 * `leaky` records that, and then the merge block's predicate is built from
 * its edges instead of being the header's.  A discard does not leak: a
 * discarded pixel keeps running and only its render-target write is
 * dropped.  A loop's merge block always runs for the channels that entered
 * the loop (`loop` nonzero; see struct i915_spirv_loop).
 */
struct i915_spirv_construct {
	uint32_t merge;
	uint32_t predicate;
	int leaky;
	int loop;
};

/*
 * One structured loop the walk is inside, from its header to its merge
 * block.
 *
 * It lives in the parser's loop stack while the walk is inside the loop;
 * the value range it covers stays in the closed list after it (a value made
 * in that range is not read after the loop).
 */
struct i915_spirv_loop {
	/* The header, merge and continue target labels. */
	uint32_t header;
	uint32_t merge;
	uint32_t continue_target;

	/* The channels that entered the loop: the merge block's predicate. */
	uint32_t entry_predicate;

	/* The loop variable of the channels still in the loop: the header's predicate. */
	uint32_t active;

	/* The construct stack depth of the loop's own construct entry. */
	uint32_t construct;

	/* The first IR value made inside the body (the value count at LOOP_BEGIN). */
	uint32_t first_value;

	/* Nonzero once LOOP_BEGIN is emitted, and once the back edge closed the body. */
	int begun;
	int closed;

	/* The header phis: the result id and the value id the back edge brings. */
	uint32_t phi_count;
	uint32_t phi_result[MAX_LOOP_PHIS];
	uint32_t phi_back[MAX_LOOP_PHIS];

	/* Where this loop's entries start in the parser's carried list. */
	uint32_t carried_first;
};

/*
 * One component of a local variable or an output that a loop carries: the
 * variable, the component and the loop variable that holds it.
 */
struct i915_spirv_carried {
	uint32_t variable;
	uint32_t component;
	uint32_t value;
};

/*
 * The IR value range of a loop that has ended: [first, end) were made
 * inside it.
 */
struct i915_spirv_range {
	uint32_t first;
	uint32_t end;
};

/*
 * The decode state threaded through both passes of one parse.
 *
 * It lives on the stack of drv_i915_shader_parse() and owns the id table,
 * the edge list, the carried list and the construct and loop stacks until
 * the parse ends.
 */
struct i915_spirv_parser {
	const uint32_t *code;
	uint32_t words;
	uint32_t bound;
	struct i915_spirv_id *ids;
	struct i915_shader_ir *ir;

	/* Instruction slots of ir->instructions. */
	uint32_t capacity;

	uint32_t body_instructions;

	/* A failure latched by an emitter that has no return value to carry it. */
	int error;

	struct i915_compile_diagnostic diag;

	/*
	 * The block being lowered: its label (0 before the first), the IR
	 * Boolean of the channels that run it (or PREDICATE_ALWAYS), and whether
	 * its terminator has been seen.
	 */
	uint32_t block;
	uint32_t predicate;
	int terminated;

	/* Nonzero while the block is the merge block of a loop, where no phi is lowered. */
	int loop_merge_block;

	/* The merge block an OpSelectionMerge named for the terminator after it, or NO_VALUE. */
	uint32_t pending_merge;

	/* Every edge so far, with room for two per body instruction. */
	struct i915_spirv_edge *edges;
	uint32_t edge_count;
	uint32_t edge_capacity;

	/* The selection and loop constructs the walk is inside, innermost last. */
	struct i915_spirv_construct constructs[MAX_CONSTRUCT_DEPTH];
	uint32_t depth;

	/* The loops the walk is inside, innermost last. */
	struct i915_spirv_loop loops[MAX_LOOP_DEPTH];
	uint32_t loop_depth;

	/* The components the open loops carry, the outer loops' first. */
	struct i915_spirv_carried *carried;
	uint32_t carried_count;
	uint32_t carried_capacity;

	/* The value ranges of the loops that have ended. */
	struct i915_spirv_range closed[MAX_LOOPS];
	uint32_t closed_count;

	/* The shared constants the lowering introduces: 0.0, 1.0 and true; NO_VALUE until first needed. */
	uint32_t zero_value;
	uint32_t one_value;
	uint32_t true_value;
};

static int i915_spirv_pass_declarations(struct i915_spirv_parser *parser);
static int i915_spirv_declare(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int i915_spirv_declare_entry_point(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int i915_spirv_declare_decoration(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int i915_spirv_declare_member_decoration(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int i915_spirv_declare_type(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int i915_spirv_declare_constant(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int i915_spirv_declare_constant_bool(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode);
static int i915_spirv_declare_constant_composite(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int i915_spirv_declare_variable(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static void i915_spirv_add_io(struct i915_spirv_parser *parser, uint32_t id, int is_input);
static void i915_spirv_add_uniform(struct i915_spirv_parser *parser, uint32_t id, uint32_t kind);
static int i915_spirv_pass_body(struct i915_spirv_parser *parser);
static int i915_spirv_lower(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int i915_spirv_lower_variable(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int i915_spirv_lower_access_chain(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int i915_spirv_chain_block(struct i915_spirv_parser *parser, struct i915_spirv_id *record, struct i915_spirv_id *pointee, struct i915_spirv_id *index_record, uint32_t index_id, uint32_t opcode, uint32_t offset);
static int i915_spirv_chain_scalars(struct i915_spirv_parser *parser, struct i915_spirv_id *record, struct i915_spirv_id *pointee, struct i915_spirv_id *index_record, uint32_t opcode, uint32_t offset);
static int i915_spirv_lower_load(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int i915_spirv_lower_load_block(struct i915_spirv_parser *parser, const uint32_t *word, struct i915_spirv_id *pointer, struct i915_spirv_id *variable, uint32_t opcode, uint32_t offset);
static uint32_t i915_spirv_load_word(struct i915_spirv_parser *parser, const struct i915_spirv_id *pointer, const struct i915_spirv_id *variable, uint32_t byte);
static int i915_spirv_lower_store(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int i915_spirv_lower_store_output(struct i915_spirv_parser *parser, struct i915_spirv_id *pointer, struct i915_spirv_id *variable, const uint32_t *scalars, uint32_t count, uint32_t first, uint32_t opcode, uint32_t offset);
static int i915_spirv_lower_arithmetic(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int i915_spirv_lower_integer(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static uint32_t i915_spirv_lower_integer_component(struct i915_spirv_parser *parser, uint32_t opcode, uint32_t left, uint32_t right);
static uint32_t i915_spirv_absolute_integer(struct i915_spirv_parser *parser, uint32_t value, uint32_t negative);
static int i915_spirv_lower_integer_unary(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int i915_spirv_lower_convert(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int i915_spirv_lower_bitcast(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int i915_spirv_lower_float_remainder(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int i915_spirv_lower_vector_times_scalar(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int i915_spirv_lower_matrix_product(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int i915_spirv_lower_transpose(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int i915_spirv_lower_outer_product(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int i915_spirv_lower_negate(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int i915_spirv_lower_dot(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static uint32_t i915_spirv_dot_value(struct i915_spirv_parser *parser, const uint32_t *left, const uint32_t *right, uint32_t components);
static int i915_spirv_lower_construct(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int i915_spirv_lower_extract(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int i915_spirv_lower_shuffle(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int i915_spirv_lower_extended(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static uint32_t i915_spirv_lower_extended_component(struct i915_spirv_parser *parser, uint32_t function, uint32_t operand[3][4], uint32_t component);
static uint32_t i915_spirv_smooth_step(struct i915_spirv_parser *parser, uint32_t edge0, uint32_t edge1, uint32_t x);
static int i915_spirv_lower_geometric(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int i915_spirv_lower_divide(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int i915_spirv_lower_compare(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static uint32_t i915_spirv_lower_compare_component(struct i915_spirv_parser *parser, uint32_t opcode, uint32_t left, uint32_t right);
static int i915_spirv_lower_integer_compare(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int i915_spirv_lower_logical(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int i915_spirv_lower_select(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int i915_spirv_lower_sample(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int i915_spirv_lower_label(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int i915_spirv_find_loop_merge(struct i915_spirv_parser *parser, uint32_t offset, uint32_t *merge, uint32_t *continue_target);
static int i915_spirv_loop_open(struct i915_spirv_parser *parser, uint32_t header, uint32_t merge, uint32_t continue_target, uint32_t *predicate, uint32_t opcode, uint32_t offset);
static void i915_spirv_loop_begin(struct i915_spirv_parser *parser, struct i915_spirv_loop *loop);
static int i915_spirv_lower_loop_merge(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int i915_spirv_loop_back_edge(struct i915_spirv_parser *parser, uint32_t predicate, uint32_t opcode, uint32_t offset);
static int i915_spirv_loop_close(struct i915_spirv_parser *parser, uint32_t opcode, uint32_t offset);
static int i915_spirv_carry(struct i915_spirv_parser *parser, uint32_t variable, uint32_t component, uint32_t value);
static int i915_spirv_lower_phi(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int i915_spirv_lower_header_phi(struct i915_spirv_parser *parser, struct i915_spirv_loop *loop, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int i915_spirv_lower_branch(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int i915_spirv_lower_branch_conditional(struct i915_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int i915_spirv_lower_kill(struct i915_spirv_parser *parser, uint32_t opcode, uint32_t offset);
static int i915_spirv_lower_return(struct i915_spirv_parser *parser, uint32_t opcode, uint32_t offset);
static int i915_spirv_edge_add(struct i915_spirv_parser *parser, uint32_t target, uint32_t predicate, uint32_t opcode, uint32_t offset);
static uint32_t i915_spirv_edge_predicate(struct i915_spirv_parser *parser, uint32_t from, uint32_t to, int *found);
static uint32_t i915_spirv_predicate_and(struct i915_spirv_parser *parser, uint32_t predicate, uint32_t condition);
static uint32_t i915_spirv_predicated_value(struct i915_spirv_parser *parser, uint32_t value, uint32_t previous);
static uint32_t i915_spirv_shared_constant(struct i915_spirv_parser *parser, uint32_t *slot, enum i915_shader_ir_op op, uint32_t bits);
static uint32_t i915_spirv_float_constant(struct i915_spirv_parser *parser, uint32_t bits);
static uint32_t i915_spirv_integer_constant(struct i915_spirv_parser *parser, uint32_t bits);
static uint32_t i915_spirv_select_value(struct i915_spirv_parser *parser, uint32_t condition, uint32_t taken, uint32_t other);
static uint32_t i915_spirv_move_value(struct i915_spirv_parser *parser, uint32_t destination, uint32_t source);
static int i915_spirv_refuse(struct i915_spirv_parser *parser, uint32_t opcode, uint32_t word_offset, const char *reason);
static struct i915_spirv_id *i915_spirv_id(struct i915_spirv_parser *parser, uint32_t id);
static uint32_t i915_spirv_kind_components(struct i915_spirv_parser *parser, uint32_t type_id, uint32_t scalar);
static uint32_t i915_spirv_float_components(struct i915_spirv_parser *parser, uint32_t type_id);
static uint32_t i915_spirv_int_components(struct i915_spirv_parser *parser, uint32_t type_id);
static uint32_t i915_spirv_bool_components(struct i915_spirv_parser *parser, uint32_t type_id);
static uint32_t i915_spirv_value_components(struct i915_spirv_parser *parser, uint32_t type_id);
static uint32_t i915_spirv_matrix_components(struct i915_spirv_parser *parser, uint32_t type_id);
static uint32_t i915_spirv_float_components_wide(struct i915_spirv_parser *parser, uint32_t type_id);
static uint32_t i915_spirv_value_components_wide(struct i915_spirv_parser *parser, uint32_t type_id);
static uint32_t i915_spirv_operand_type(struct i915_spirv_parser *parser, uint32_t id);
static uint32_t i915_spirv_operand_int_components(struct i915_spirv_parser *parser, uint32_t id);
static uint32_t i915_spirv_operand_float_components(struct i915_spirv_parser *parser, uint32_t id);
static void i915_spirv_operand_shape(struct i915_spirv_parser *parser, uint32_t id, uint32_t *columns, uint32_t *rows);
static uint32_t i915_spirv_emit_value(struct i915_spirv_parser *parser, enum i915_shader_ir_op op, uint32_t source0, uint32_t source1);
static struct i915_shader_ir_inst *i915_spirv_emit(struct i915_spirv_parser *parser, enum i915_shader_ir_op op, uint32_t dst, uint32_t source0, uint32_t source1);
static uint32_t i915_spirv_new_value(struct i915_spirv_parser *parser);
static uint32_t i915_spirv_operand(struct i915_spirv_parser *parser, uint32_t id, uint32_t comp[4]);
static uint32_t i915_spirv_operand_wide(struct i915_spirv_parser *parser, uint32_t id, uint32_t comp[MAX_COMPONENTS]);
static int i915_spirv_escaped(struct i915_spirv_parser *parser, uint32_t value);
static uint32_t i915_spirv_constant_operand(struct i915_spirv_parser *parser, struct i915_spirv_id *record, uint32_t comp[MAX_COMPONENTS]);
static struct i915_spirv_id *i915_spirv_result(struct i915_spirv_parser *parser, uint32_t id, uint32_t type_id, uint32_t count, int fresh);

/*
 * Parses SPIR-V words into the scalar IR of one stage.
 *
 * Returns 0 and the IR in `*out`, EINVAL for a malformed module, ENOTSUP for
 * valid SPIR-V this parser does not lower, or ENOMEM.  On a failure `*out` is
 * NULL and, when `diagnostic` is not NULL, it names the refused instruction
 * (it is cleared on entry and stays clear for a failure that names none).
 */
int
drv_i915_shader_parse(
	const uint32_t *words,
	size_t word_count,
	enum i915_shader_stage stage,
	struct i915_shader_ir **out,
	struct i915_compile_diagnostic *diagnostic)
{
	struct i915_spirv_parser parser;
	struct i915_shader_ir *ir;
	uint32_t slots;
	int error;

	/* The caller receives nothing unless the whole module parses. */
	*out = NULL;
	if (diagnostic != NULL)
		kern_memset(diagnostic, 0, sizeof(*diagnostic));

	/* A module must have a header and a matching magic. */
	if (word_count < SPIRV_HEADER_WORDS)
		return EINVAL;
	if (words[0] != SPIRV_MAGIC)
		return EINVAL;

	/* The id bound sizes the id table, so it must be present and modest. */
	if (words[3] == 0U)
		return EINVAL;
	if (words[3] > SPIRV_MAX_BOUND)
		return EINVAL;

	/* Allocates the IR the parse fills. */
	ir = kern_calloc(1U, sizeof(*ir));
	if (ir == NULL)
		return ENOMEM;
	ir->stage = stage;

	/* Prepares the decode state and its id table. */
	kern_memset(&parser, 0, sizeof(parser));
	parser.code = words;
	parser.words = (uint32_t)word_count;
	parser.bound = words[3];
	parser.ir = ir;
	parser.predicate = PREDICATE_ALWAYS;
	parser.pending_merge = NO_VALUE;
	parser.zero_value = NO_VALUE;
	parser.one_value = NO_VALUE;
	parser.true_value = NO_VALUE;
	parser.ids = kern_calloc(parser.bound, sizeof(*parser.ids));
	if (parser.ids == NULL) {
		kern_free(ir);
		return ENOMEM;
	}

	/* Interface lists are bounded by the id count. */
	slots = parser.bound;

	/* Allocates the input list. */
	ir->inputs = kern_calloc(slots, sizeof(*ir->inputs));
	if (ir->inputs == NULL) {
		drv_i915_shader_ir_free(ir);
		kern_free(parser.ids);
		return ENOMEM;
	}

	/* Allocates the output list. */
	ir->outputs = kern_calloc(slots, sizeof(*ir->outputs));
	if (ir->outputs == NULL) {
		drv_i915_shader_ir_free(ir);
		kern_free(parser.ids);
		return ENOMEM;
	}

	/* Allocates the uniform list. */
	ir->uniforms = kern_calloc(slots, sizeof(*ir->uniforms));
	if (ir->uniforms == NULL) {
		drv_i915_shader_ir_free(ir);
		kern_free(parser.ids);
		return ENOMEM;
	}

	/* The first pass records types, constants, decorations and interface variables. */
	error = i915_spirv_pass_declarations(&parser);

	/*
	 * The stream starts with room for the common lowerings and grows for the
	 * longer ones; failing to grow is an error, never a silently shorter
	 * shader.
	 */
	if (error == 0) {
		parser.capacity = parser.body_instructions * IR_PER_INSTRUCTION + IR_PER_INSTRUCTION;
		ir->instructions = kern_calloc(parser.capacity, sizeof(*ir->instructions));
		if (ir->instructions == NULL)
			error = ENOMEM;
	}

	/* A terminator adds at most two edges, so the edge list has room for two per body instruction. */
	if (error == 0) {
		parser.edge_capacity = 2U * parser.body_instructions + 2U;
		parser.edges = kern_calloc(parser.edge_capacity, sizeof(*parser.edges));
		if (parser.edges == NULL)
			error = ENOMEM;
	}

	/* The second pass lowers the entry function body. */
	if (error == 0)
		error = i915_spirv_pass_body(&parser);

	/* A failure an emitter latched fails the parse as well. */
	if (error == 0 && parser.error != 0)
		error = parser.error;

	/* The edge and carried lists are only needed while parsing. */
	if (parser.edges != NULL)
		kern_free(parser.edges);
	if (parser.carried != NULL)
		kern_free(parser.carried);

	/* A failed parse reports the refused instruction and releases everything. */
	if (error != 0) {
		if (diagnostic != NULL)
			*diagnostic = parser.diag;
		drv_i915_shader_ir_free(ir);
		kern_free(parser.ids);
		return error;
	}

	/* The id table is only needed while parsing. */
	kern_free(parser.ids);

	/* Succeeded: the caller owns the IR. */
	*out = ir;
	return 0;
}

/*
 * Releases a parsed shader IR and its lists.
 */
void
drv_i915_shader_ir_free(
	struct i915_shader_ir *ir)
{
	/* Nothing was parsed. */
	if (ir == NULL)
		return;

	/* Releases each list the parser allocated. */
	if (ir->instructions != NULL)
		kern_free(ir->instructions);
	if (ir->inputs != NULL)
		kern_free(ir->inputs);
	if (ir->outputs != NULL)
		kern_free(ir->outputs);
	if (ir->uniforms != NULL)
		kern_free(ir->uniforms);

	/* Releases the IR itself. */
	kern_free(ir);
}

/* Records types, constants, decorations and interface variables. */
static int
i915_spirv_pass_declarations(
	struct i915_spirv_parser *parser)
{
	const uint32_t *word;
	uint32_t offset;
	uint32_t count;
	uint32_t opcode;
	int in_function;
	int error;

	/* Walks every instruction after the header. */
	in_function = 0;
	offset = SPIRV_HEADER_WORDS;
	while (offset < parser->words) {
		/* Decodes the word count and the opcode of the instruction. */
		word = parser->code + offset;
		count = word[0] >> 16;
		opcode = word[0] & 0xFFFFU;

		/* An instruction must have a length and end inside the module. */
		if (count == 0U || offset + count > parser->words)
			return EINVAL;

		/*
		 * Function bodies are only counted here, to size the IR stream; the
		 * OpFunction and OpFunctionEnd instructions count as well.
		 */
		if (opcode == OP_FUNCTION)
			in_function = 1;
		if (in_function != 0) {
			parser->body_instructions++;
			if (opcode == OP_FUNCTION_END)
				in_function = 0;
			offset += count;
			continue;
		}

		/* Records what the module-level instruction declares. */
		error = i915_spirv_declare(parser, word, count, opcode, offset);
		if (error != 0)
			return error;

		offset += count;
	}

	/* Succeeded: every module-level instruction was interpreted. */
	return 0;
}

/* Records one module-level instruction. */
static int
i915_spirv_declare(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *record;

	/* Dispatches on what the instruction declares. */
	switch (opcode) {
	case OP_ENTRY_POINT:
		return i915_spirv_declare_entry_point(parser, word, count, opcode, offset);

	case OP_DECORATE:
		return i915_spirv_declare_decoration(parser, word, count, opcode, offset);

	case OP_MEMBER_DECORATE:
		return i915_spirv_declare_member_decoration(parser, word, count, opcode, offset);

	case OP_TYPE_VOID:
	case OP_TYPE_BOOL:
	case OP_TYPE_SAMPLER:
	case OP_TYPE_FUNCTION:
	case OP_TYPE_IMAGE:
	case OP_TYPE_INT:
	case OP_TYPE_FLOAT:
	case OP_TYPE_VECTOR:
	case OP_TYPE_MATRIX:
	case OP_TYPE_SAMPLED_IMAGE:
	case OP_TYPE_ARRAY:
	case OP_TYPE_STRUCT:
	case OP_TYPE_POINTER:
		return i915_spirv_declare_type(parser, word, count, opcode, offset);

	case OP_CONSTANT:
		return i915_spirv_declare_constant(parser, word, count, opcode, offset);

	case OP_CONSTANT_TRUE:
	case OP_CONSTANT_FALSE:
		return i915_spirv_declare_constant_bool(parser, word, count, opcode);

	case OP_CONSTANT_COMPOSITE:
		return i915_spirv_declare_constant_composite(parser, word, count, opcode, offset);

	case OP_VARIABLE:
		return i915_spirv_declare_variable(parser, word, count, opcode, offset);

	case OP_EXT_INST_IMPORT:
		/* An extended instruction set: only its id is recorded. */
		record = NULL;
		if (count >= 2U)
			record = i915_spirv_id(parser, word[1]);
		if (record == NULL)
			return EINVAL;
		record->kind = ID_EXT_SET;
		return 0;

	case OP_NOP:
	case OP_SOURCE_CONTINUED:
	case OP_SOURCE:
	case OP_SOURCE_EXTENSION:
	case OP_NAME:
	case OP_MEMBER_NAME:
	case OP_STRING:
	case OP_LINE:
	case OP_NO_LINE:
	case OP_MODULE_PROCESSED:
	case OP_CAPABILITY:
	case OP_EXTENSION:
	case OP_MEMORY_MODEL:
	case OP_EXECUTION_MODE:
		/* Module-level instructions without execution semantics. */
		return 0;

	default:
		break;
	}

	/* Anything else could change the shader and is refused. */
	return i915_spirv_refuse(parser, opcode, offset, "module-level instruction that is not interpreted");
}

/* Takes the stage from the entry point's execution model. */
static int
i915_spirv_declare_entry_point(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	/* The instruction must name an execution model and an entry. */
	if (count < 3U)
		return EINVAL;

	/* Only vertex and fragment shaders are lowered. */
	if (word[1] == EM_VERTEX) {
		parser->ir->stage = I915_STAGE_VERTEX;
	} else if (word[1] == EM_FRAGMENT) {
		parser->ir->stage = I915_STAGE_FRAGMENT;
	} else {
		return i915_spirv_refuse(parser, opcode, offset, "execution model other than Vertex / Fragment");
	}

	/* Succeeded: the stage is known. */
	return 0;
}

/*
 * Interprets one decoration.
 *
 * Interpreted: Location, Binding, DescriptorSet, BuiltIn, Block and
 * ArrayStride.  Without effect on this lowering: RelaxedPrecision (a
 * permission to lose precision, never used here).  Anything else could
 * place or qualify data -- components, interpolation, buffer blocks -- and
 * is refused.
 */
static int
i915_spirv_declare_decoration(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *record;

	/* Resolves the decorated id. */
	record = NULL;
	if (count >= 3U)
		record = i915_spirv_id(parser, word[1]);
	if (record == NULL)
		return EINVAL;

	/* Records the decoration, or refuses one that is not interpreted. */
	if (word[2] == DEC_LOCATION && count >= 4U) {
		record->has_location = 1U;
		record->location = word[3];
	} else if (word[2] == DEC_BINDING && count >= 4U) {
		record->has_binding = 1U;
		record->binding = word[3];
	} else if (word[2] == DEC_DESCRIPTOR_SET && count >= 4U) {
		record->has_set = 1U;
		record->set = word[3];
	} else if (word[2] == DEC_BUILTIN && count >= 4U) {
		record->has_builtin = 1U;
		record->builtin = word[3];
	} else if (word[2] == DEC_ARRAY_STRIDE && count >= 4U) {
		record->stride = word[3];
	} else if (word[2] != DEC_BLOCK && word[2] != DEC_RELAXED_PRECISION) {
		return i915_spirv_refuse(parser, opcode, offset, "decoration that is not interpreted");
	}

	/* Succeeded: the decoration is recorded or has no effect. */
	return 0;
}

/*
 * Interprets one structure member decoration.
 *
 * Interpreted: Offset, BuiltIn, MatrixStride, ColMajor and RowMajor;
 * RelaxedPrecision has no effect; anything else is refused.
 */
static int
i915_spirv_declare_member_decoration(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *record;
	uint32_t member;

	/* Resolves the decorated structure type. */
	record = NULL;
	if (count >= 4U)
		record = i915_spirv_id(parser, word[1]);
	if (record == NULL)
		return EINVAL;

	/* Refuses a member beyond the member table. */
	member = word[2];
	if (member >= MAX_MEMBERS)
		return i915_spirv_refuse(parser, opcode, offset, "structure with more members than supported");

	/* Records the decoration, or refuses one that is not interpreted. */
	if (word[3] == DEC_OFFSET && count >= 5U) {
		record->member_offset[member] = word[4];
		record->member_has_offset[member] = 1U;
	} else if (word[3] == DEC_BUILTIN && count >= 5U) {
		/* Stored plus one, so zero keeps meaning "not a builtin". */
		record->member_builtin[member] = word[4] + 1U;
	} else if (word[3] == DEC_MATRIX_STRIDE && count >= 5U) {
		record->member_matrix_stride[member] = word[4];
	} else if (word[3] == DEC_ROW_MAJOR) {
		record->member_row_major[member] = 1U;
	} else if (word[3] == DEC_COL_MAJOR) {
		record->member_row_major[member] = 0U;
	} else if (word[3] != DEC_RELAXED_PRECISION) {
		return i915_spirv_refuse(parser, opcode, offset, "member decoration that is not interpreted");
	}

	/* Succeeded: the decoration is recorded or has no effect. */
	return 0;
}

/* Records one type declaration. */
static int
i915_spirv_declare_type(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *record;
	struct i915_spirv_id *length;
	uint32_t index;

	/* Resolves the declared type's id. */
	record = NULL;
	if (count >= 2U)
		record = i915_spirv_id(parser, word[1]);
	if (record == NULL)
		return EINVAL;

	/* Records the type by its kind. */
	switch (opcode) {
	case OP_TYPE_VOID:
		record->kind = ID_TYPE_VOID;
		break;

	case OP_TYPE_BOOL:
		record->kind = ID_TYPE_BOOL;
		break;

	case OP_TYPE_SAMPLER:
		record->kind = ID_TYPE_SAMPLER;
		break;

	case OP_TYPE_IMAGE:
		record->kind = ID_TYPE_IMAGE;
		break;

	case OP_TYPE_FUNCTION:
		record->kind = ID_TYPE_FUNCTION;
		break;

	case OP_TYPE_INT:
	case OP_TYPE_FLOAT:
		/* Scalar types: the width decides which ones are lowered. */
		if (count < 3U)
			return EINVAL;
		if (opcode == OP_TYPE_INT) {
			record->kind = ID_TYPE_INT;
		} else {
			record->kind = ID_TYPE_FLOAT;
		}
		record->width = (uint8_t)word[2];
		break;

	case OP_TYPE_VECTOR:
		/* A vector of two to four components. */
		if (count < 4U)
			return EINVAL;
		if (word[3] < 2U || word[3] > 4U)
			return EINVAL;
		record->kind = ID_TYPE_VECTOR;
		record->type = word[2];
		record->count = (uint8_t)word[3];
		break;

	case OP_TYPE_MATRIX:
		/* A matrix of two to four columns of a vector type. */
		if (count < 4U)
			return EINVAL;
		if (word[3] < 2U || word[3] > 4U)
			return EINVAL;
		record->kind = ID_TYPE_MATRIX;
		record->type = word[2];
		record->count = (uint8_t)word[3];
		break;

	case OP_TYPE_SAMPLED_IMAGE:
		/* A combined image sampler of an image type. */
		if (count < 3U)
			return EINVAL;
		record->kind = ID_TYPE_SAMPLED_IMAGE;
		record->type = word[2];
		break;

	case OP_TYPE_ARRAY:
		/* An array: the element type, and the length of its constant. */
		if (count < 4U)
			return EINVAL;
		length = i915_spirv_id(parser, word[3]);
		if (length == NULL)
			return EINVAL;
		record->kind = ID_TYPE_ARRAY;
		record->type = word[2];
		record->length = 0U;
		if (length->kind == ID_CONSTANT)
			record->length = length->constant;
		break;

	case OP_TYPE_STRUCT:
		/* A structure with at most MAX_MEMBERS members. */
		if (count - 2U > MAX_MEMBERS)
			return i915_spirv_refuse(parser, opcode, offset, "structure with more members than supported");
		record->kind = ID_TYPE_STRUCT;
		record->count = (uint8_t)(count - 2U);

		/* Records the type of each member. */
		for (index = 0U; index < count - 2U; index++)
			record->member_type[index] = word[2U + index];
		break;

	case OP_TYPE_POINTER:
		/* A pointer: its storage class and pointee type. */
		if (count < 4U)
			return EINVAL;
		record->kind = ID_TYPE_POINTER;
		record->storage = (uint16_t)word[2];
		record->type = word[3];
		break;

	default:
		/* Only the opcodes above are routed here. */
		return EINVAL;
	}

	/* Succeeded: the type is recorded. */
	return 0;
}

/* Records a scalar constant: 32-bit int or float. */
static int
i915_spirv_declare_constant(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *record;

	/* Resolves the constant's result id. */
	record = NULL;
	if (count >= 4U)
		record = i915_spirv_id(parser, word[2]);
	if (record == NULL)
		return EINVAL;

	/* A constant of more than one word is wider than any value lowered here. */
	if (count != 4U)
		return i915_spirv_refuse(parser, opcode, offset, "constant wider than 32 bits");

	/* Records the constant's type and bits. */
	record->kind = ID_CONSTANT;
	record->type = word[1];
	record->constant = word[3];

	/* Succeeded: the constant becomes IR the first time it is used. */
	return 0;
}

/* Records OpConstantTrue or OpConstantFalse: a Boolean constant of all ones or zero. */
static int
i915_spirv_declare_constant_bool(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode)
{
	struct i915_spirv_id *record;

	/* Resolves the constant's result id. */
	record = NULL;
	if (count >= 3U)
		record = i915_spirv_id(parser, word[2]);
	if (record == NULL)
		return EINVAL;

	/* Records the constant's type and the bits of its value. */
	record->kind = ID_CONSTANT;
	record->type = word[1];
	if (opcode == OP_CONSTANT_TRUE) {
		record->constant = BOOL_TRUE_BITS;
	} else {
		record->constant = 0U;
	}

	/* Succeeded: the constant becomes IR the first time it is used. */
	return 0;
}

/*
 * Records a vector constant made of scalar constants, or a matrix constant
 * made of vector constants; any other composite is refused.
 */
static int
i915_spirv_declare_constant_composite(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *record;
	struct i915_spirv_id *constituent;
	uint32_t index;

	/* Resolves the constant's result id. */
	record = NULL;
	if (count >= 3U)
		record = i915_spirv_id(parser, word[2]);
	if (record == NULL)
		return EINVAL;

	/* A vector or a matrix has two to four constituents; a structure or an array constant is not lowered. */
	if (count - 3U < 2U || count - 3U > 4U)
		return i915_spirv_refuse(parser, opcode, offset, "constant composite that is not a vector or a matrix");

	/* Every constituent must be a constant declared before: a scalar, or a vector of a matrix. */
	for (index = 3U; index < count; index++) {
		constituent = i915_spirv_id(parser, word[index]);
		if (constituent == NULL)
			return EINVAL;
		if (constituent->kind != ID_CONSTANT && constituent->kind != ID_CONSTANT_COMPOSITE)
			return i915_spirv_refuse(parser, opcode, offset, "constant composite that is not a vector or a matrix");
	}

	/* Records the type and the constituents; each becomes IR the first time it is used. */
	record->kind = ID_CONSTANT_COMPOSITE;
	record->type = word[1];
	record->count = (uint8_t)(count - 3U);
	for (index = 3U; index < count; index++)
		record->member_type[index - 3U] = word[index];

	/* Succeeded: the composite constant is recorded. */
	return 0;
}

/* Records a module-level (interface) variable. */
static int
i915_spirv_declare_variable(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *record;
	struct i915_spirv_id *type;
	uint32_t storage;
	uint32_t index;

	/* Resolves the variable and its pointer type. */
	record = NULL;
	if (count >= 4U)
		record = i915_spirv_id(parser, word[2]);
	if (record == NULL)
		return EINVAL;
	type = i915_spirv_id(parser, word[1]);
	if (type == NULL)
		return EINVAL;
	if (type->kind != ID_TYPE_POINTER)
		return EINVAL;

	/* An initializer would give the variable a value this lowering does not track. */
	if (count > 4U)
		return i915_spirv_refuse(parser, opcode, offset, "variable initializer is not lowered");

	/* Records the variable as a pointer to its pointee, nothing selected yet. */
	storage = word[3];
	record->kind = ID_VARIABLE;
	record->storage = (uint16_t)storage;
	record->type = word[1];
	record->var = word[2];
	record->pointee = type->type;
	record->member = -1;
	record->component = -1;
	record->byte_offset = 0U;
	record->component_stride = 4U;
	record->dynamic_index = NO_VALUE;

	/*
	 * An output remembers what was last stored to each component, for a
	 * store under a predicate to keep where the predicate is false; nothing
	 * is stored yet.
	 */
	for (index = 0U; index < MAX_COMPONENTS; index++)
		record->comp[index] = NO_VALUE;

	/* The storage class, and a location, decide what the variable is to the shader. */
	if (storage == SC_INPUT && record->has_location != 0U) {
		record->ptr_kind = PTR_INPUT;
		i915_spirv_add_io(parser, word[2], 1);
	} else if (storage == SC_OUTPUT && record->has_location != 0U) {
		record->ptr_kind = PTR_OUTPUT;
		i915_spirv_add_io(parser, word[2], 0);
	} else if (storage == SC_OUTPUT) {
		record->ptr_kind = PTR_OUTPUT_BLOCK;
	} else if (storage == SC_PUSH_CONSTANT) {
		record->ptr_kind = PTR_PUSH;
	} else if (storage == SC_UNIFORM_CONSTANT) {
		record->ptr_kind = PTR_SAMPLER;
		i915_spirv_add_uniform(parser, word[2], I915_IR_UNIFORM_SAMPLED_IMAGE);
	} else if (storage == SC_UNIFORM) {
		record->ptr_kind = PTR_UBO;
		record->uniform = parser->ir->uniform_count;
		i915_spirv_add_uniform(parser, word[2], I915_IR_UNIFORM_BLOCK);
	} else {
		return i915_spirv_refuse(parser, opcode, offset, "variable in a storage class that is not lowered");
	}

	/* Succeeded: the variable is part of the interface. */
	return 0;
}

/* Adds an input or output interface slot for a variable id. */
static void
i915_spirv_add_io(
	struct i915_spirv_parser *parser,
	uint32_t id,
	int is_input)
{
	struct i915_shader_ir_io *slot;
	uint32_t components;

	/* Counts the float components of what the variable holds. */
	components = i915_spirv_float_components(parser, parser->ids[id].pointee);

	/* Takes the next slot of the input or the output list. */
	if (is_input != 0) {
		slot = &parser->ir->inputs[parser->ir->input_count];
		parser->ir->input_count++;
	} else {
		slot = &parser->ir->outputs[parser->ir->output_count];
		parser->ir->output_count++;
	}

	/* Records the location; a variable that is not a float vector counts as one component. */
	slot->location = parser->ids[id].location;
	if (components != 0U) {
		slot->components = components;
	} else {
		slot->components = 1U;
	}
	slot->type = 0U;
}

/*
 * Adds a uniform slot for a variable id: a sampled image, or a uniform
 * block whose read range the loads widen (none read yet).
 */
static void
i915_spirv_add_uniform(
	struct i915_spirv_parser *parser,
	uint32_t id,
	uint32_t kind)
{
	struct i915_shader_ir_uniform *slot;

	/* Takes the next slot of the uniform list. */
	slot = &parser->ir->uniforms[parser->ir->uniform_count];
	parser->ir->uniform_count++;

	/* Records the descriptor set and binding of the resource. */
	slot->set = parser->ids[id].set;
	slot->binding = parser->ids[id].binding;
	slot->kind = kind;
	slot->offset = 0U;
	slot->size = 0U;
}

/* Lowers the body of the entry function. */
static int
i915_spirv_pass_body(
	struct i915_spirv_parser *parser)
{
	const uint32_t *word;
	uint32_t offset;
	uint32_t count;
	uint32_t opcode;
	uint32_t functions;
	int in_function;
	int error;

	/* Walks every instruction after the header, lowering those inside the function. */
	functions = 0U;
	in_function = 0;
	offset = SPIRV_HEADER_WORDS;
	while (offset < parser->words) {
		/* Decodes the word count and the opcode of the instruction. */
		word = parser->code + offset;
		count = word[0] >> 16;
		opcode = word[0] & 0xFFFFU;

		/* An instruction must have a length and end inside the module. */
		if (count == 0U || offset + count > parser->words)
			return EINVAL;

		/* A function opens the body; only one is accepted, since calls are not lowered. */
		if (opcode == OP_FUNCTION) {
			if (in_function != 0)
				return EINVAL;
			functions++;
			if (functions > 1U)
				return i915_spirv_refuse(parser, opcode, offset, "more than one function (calls are not lowered)");
			in_function = 1;
			offset += count;
			continue;
		}

		/* Module-level instructions were handled by the first pass. */
		if (in_function == 0) {
			offset += count;
			continue;
		}

		/* The end of the function closes the body, after a terminated block and every construct's merge. */
		if (opcode == OP_FUNCTION_END) {
			if (parser->block == 0U ||
			    parser->terminated == 0 ||
			    parser->depth != 0U ||
			    parser->loop_depth != 0U)
				return EINVAL;
			in_function = 0;
			offset += count;
			continue;
		}

		/* Lowers one instruction of the body. */
		error = i915_spirv_lower(parser, word, count, opcode, offset);
		if (error != 0)
			return error;

		offset += count;
	}

	/* A module must hold one complete function. */
	if (in_function != 0 || functions == 0U)
		return EINVAL;

	/* Succeeded: the whole body is lowered. */
	return 0;
}

/* Lowers one instruction of the function body. */
static int
i915_spirv_lower(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_loop *loop;

	/* Debug instructions carry no execution semantics, wherever they sit. */
	if (opcode == OP_NOP || opcode == OP_LINE || opcode == OP_NO_LINE)
		return 0;

	/* A label opens the next block. */
	if (opcode == OP_LABEL)
		return i915_spirv_lower_label(parser, word, count, opcode, offset);

	/* Every other instruction belongs to an open block, before its terminator. */
	if (parser->block == 0U || parser->terminated != 0)
		return EINVAL;

	/* A loop's body starts at the first instruction of its header that is not a phi. */
	if (parser->loop_depth != 0U && opcode != OP_PHI) {
		loop = &parser->loops[parser->loop_depth - 1U];
		if (loop->begun == 0 && loop->header == parser->block)
			i915_spirv_loop_begin(parser, loop);
	}

	/* Dispatches on the instruction. */
	switch (opcode) {
	case OP_RETURN:
		return i915_spirv_lower_return(parser, opcode, offset);

	case OP_UNREACHABLE:
		/* No channel gets here: the block simply ends. */
		parser->terminated = 1;
		return 0;

	case OP_BRANCH:
		return i915_spirv_lower_branch(parser, word, count, opcode, offset);

	case OP_BRANCH_CONDITIONAL:
		return i915_spirv_lower_branch_conditional(parser, word, count, opcode, offset);

	case OP_SELECTION_MERGE:
		/* The next terminator is a selection header's; its construct ends at this merge block. */
		if (count < 3U)
			return EINVAL;
		parser->pending_merge = word[1];
		return 0;

	case OP_LOOP_MERGE:
		return i915_spirv_lower_loop_merge(parser, word, count, opcode, offset);

	case OP_KILL:
		return i915_spirv_lower_kill(parser, opcode, offset);

	case OP_PHI:
		return i915_spirv_lower_phi(parser, word, count, opcode, offset);

	case OP_SWITCH:
		return i915_spirv_refuse(parser, opcode, offset, "OpSwitch is not lowered");

	case OP_FDIV:
		return i915_spirv_lower_divide(parser, word, count, opcode, offset);

	case OP_FMOD:
	case OP_FREM:
		return i915_spirv_lower_float_remainder(parser, word, count, opcode, offset);

	case OP_FORD_EQUAL:
	case OP_FUNORD_EQUAL:
	case OP_FORD_NOT_EQUAL:
	case OP_FUNORD_NOT_EQUAL:
	case OP_FORD_LESS_THAN:
	case OP_FUNORD_LESS_THAN:
	case OP_FORD_GREATER_THAN:
	case OP_FUNORD_GREATER_THAN:
	case OP_FORD_LESS_THAN_EQUAL:
	case OP_FUNORD_LESS_THAN_EQUAL:
	case OP_FORD_GREATER_THAN_EQUAL:
	case OP_FUNORD_GREATER_THAN_EQUAL:
		return i915_spirv_lower_compare(parser, word, count, opcode, offset);

	case OP_IEQUAL:
	case OP_INOT_EQUAL:
	case OP_UGREATER_THAN:
	case OP_SGREATER_THAN:
	case OP_UGREATER_THAN_EQUAL:
	case OP_SGREATER_THAN_EQUAL:
	case OP_ULESS_THAN:
	case OP_SLESS_THAN:
	case OP_ULESS_THAN_EQUAL:
	case OP_SLESS_THAN_EQUAL:
		return i915_spirv_lower_integer_compare(parser, word, count, opcode, offset);

	case OP_LOGICAL_AND:
	case OP_LOGICAL_OR:
	case OP_LOGICAL_NOT:
	case OP_LOGICAL_EQUAL:
	case OP_LOGICAL_NOT_EQUAL:
		return i915_spirv_lower_logical(parser, word, count, opcode, offset);

	case OP_SELECT:
		return i915_spirv_lower_select(parser, word, count, opcode, offset);

	case OP_VARIABLE:
		return i915_spirv_lower_variable(parser, word, count, opcode, offset);

	case OP_ACCESS_CHAIN:
		return i915_spirv_lower_access_chain(parser, word, count, opcode, offset);

	case OP_LOAD:
		return i915_spirv_lower_load(parser, word, count, opcode, offset);

	case OP_STORE:
		return i915_spirv_lower_store(parser, word, count, opcode, offset);

	case OP_FADD:
	case OP_FSUB:
	case OP_FMUL:
		return i915_spirv_lower_arithmetic(parser, word, count, opcode, offset);

	case OP_IADD:
	case OP_ISUB:
	case OP_IMUL:
	case OP_UDIV:
	case OP_SDIV:
	case OP_UMOD:
	case OP_SREM:
	case OP_SMOD:
	case OP_SHIFT_RIGHT_LOGICAL:
	case OP_SHIFT_RIGHT_ARITHMETIC:
	case OP_SHIFT_LEFT_LOGICAL:
	case OP_BITWISE_OR:
	case OP_BITWISE_XOR:
	case OP_BITWISE_AND:
		return i915_spirv_lower_integer(parser, word, count, opcode, offset);

	case OP_SNEGATE:
	case OP_NOT:
		return i915_spirv_lower_integer_unary(parser, word, count, opcode, offset);

	case OP_CONVERT_F_TO_U:
	case OP_CONVERT_F_TO_S:
	case OP_CONVERT_S_TO_F:
	case OP_CONVERT_U_TO_F:
		return i915_spirv_lower_convert(parser, word, count, opcode, offset);

	case OP_BITCAST:
		return i915_spirv_lower_bitcast(parser, word, count, opcode, offset);

	case OP_VECTOR_TIMES_SCALAR:
		return i915_spirv_lower_vector_times_scalar(parser, word, count, opcode, offset);

	case OP_MATRIX_TIMES_SCALAR:
	case OP_VECTOR_TIMES_MATRIX:
	case OP_MATRIX_TIMES_VECTOR:
	case OP_MATRIX_TIMES_MATRIX:
		return i915_spirv_lower_matrix_product(parser, word, count, opcode, offset);

	case OP_TRANSPOSE:
		return i915_spirv_lower_transpose(parser, word, count, opcode, offset);

	case OP_OUTER_PRODUCT:
		return i915_spirv_lower_outer_product(parser, word, count, opcode, offset);

	case OP_FNEGATE:
		return i915_spirv_lower_negate(parser, word, count, opcode, offset);

	case OP_DOT:
		return i915_spirv_lower_dot(parser, word, count, opcode, offset);

	case OP_COMPOSITE_CONSTRUCT:
		return i915_spirv_lower_construct(parser, word, count, opcode, offset);

	case OP_COMPOSITE_EXTRACT:
		return i915_spirv_lower_extract(parser, word, count, opcode, offset);

	case OP_VECTOR_SHUFFLE:
		return i915_spirv_lower_shuffle(parser, word, count, opcode, offset);

	case OP_EXT_INST:
		return i915_spirv_lower_extended(parser, word, count, opcode, offset);

	case OP_IMAGE_SAMPLE_IMPLICIT_LOD:
		return i915_spirv_lower_sample(parser, word, count, opcode, offset);

	default:
		break;
	}

	/* A skipped instruction would be a different shader, so it is refused. */
	return i915_spirv_refuse(parser, opcode, offset, "instruction with execution semantics is not lowered");
}

/*
 * Lowers a local variable: not memory, a set of "currently stored" scalars
 * (none yet).  Only float, integer or Boolean scalars and vectors, and float
 * matrices; a struct or array local is refused.
 */
static int
i915_spirv_lower_variable(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *record;
	struct i915_spirv_id *type;
	uint32_t components;
	uint32_t index;

	/* Resolves the variable and its pointer type. */
	record = NULL;
	type = NULL;
	if (count >= 4U) {
		record = i915_spirv_id(parser, word[2]);
		type = i915_spirv_id(parser, word[1]);
	}

	/* The variable must be a fresh id with a pointer type. */
	if (record == NULL || type == NULL)
		return EINVAL;
	if (type->kind != ID_TYPE_POINTER || record->kind != ID_NONE)
		return EINVAL;

	/* A local with an initializer, or in another storage class, is refused. */
	if (word[3] != SC_FUNCTION || count > 4U)
		return i915_spirv_refuse(parser, opcode, offset, "local variable with an initializer or a non-Function storage class");

	/* Only scalars, vectors and matrices can be tracked as scalars. */
	components = i915_spirv_value_components_wide(parser, type->type);
	if (components == 0U)
		return i915_spirv_refuse(parser, opcode, offset, "local variable that is not a scalar, a vector or a matrix");

	/* Records the local as a pointer to itself with nothing stored yet. */
	record->kind = ID_VARIABLE;
	record->storage = SC_FUNCTION;
	record->ptr_kind = PTR_LOCAL;
	record->type = word[1];
	record->var = word[2];
	record->pointee = type->type;
	record->member = -1;
	record->component = -1;
	record->count = (uint8_t)components;
	record->dynamic_index = NO_VALUE;

	/* No component holds a value until it is stored. */
	for (index = 0U; index < MAX_COMPONENTS; index++)
		record->comp[index] = NO_VALUE;

	/* Succeeded: the local is ready for stores. */
	return 0;
}

/*
 * Lowers OpAccessChain.
 *
 * Into push constants and uniform blocks: struct members, array elements
 * (one dynamic index at most), matrix columns and vector components, as a
 * byte offset.  Into locals, inputs and outputs: an output block's member,
 * a matrix column and a vector component, with constant indices only, as
 * the first scalar addressed.
 */
static int
i915_spirv_lower_access_chain(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *record;
	struct i915_spirv_id *base;
	struct i915_spirv_id *index_record;
	struct i915_spirv_id *pointee;
	uint32_t index;
	int error;

	/* Resolves the result and the pointer the chain starts from. */
	record = NULL;
	base = NULL;
	if (count >= 4U) {
		record = i915_spirv_id(parser, word[2]);
		base = i915_spirv_id(parser, word[3]);
	}

	/* The result must be fresh and the base must be a pointer. */
	if (record == NULL || base == NULL || record->kind != ID_NONE)
		return EINVAL;
	if (base->kind != ID_VARIABLE && base->kind != ID_POINTER)
		return EINVAL;

	/* The chain starts as a copy of the base pointer. */
	*record = *base;
	record->kind = ID_POINTER;

	/* Each index selects one level of what the pointer addresses. */
	for (index = 4U; index < count; index++) {
		/* Resolves the index and what the pointer addresses so far. */
		index_record = i915_spirv_id(parser, word[index]);
		pointee = i915_spirv_id(parser, record->pointee);
		if (index_record == NULL || pointee == NULL)
			return EINVAL;

		/* Memory the draw delivers is addressed in bytes; anything else in scalars. */
		if (record->ptr_kind == PTR_PUSH || record->ptr_kind == PTR_UBO) {
			error = i915_spirv_chain_block(parser, record, pointee, index_record, word[index], opcode, offset);
		} else {
			error = i915_spirv_chain_scalars(parser, record, pointee, index_record, opcode, offset);
		}
		if (error != 0)
			return error;
	}

	/* Succeeded: the pointer names what the indices select. */
	return 0;
}

/*
 * Steps a pointer into push constants or a uniform block one index down:
 * a member by its Offset, an element by the array's ArrayStride (a dynamic
 * index is kept for the load), a column by the matrix's MatrixStride and
 * layout, a component by the vector's component stride.
 */
static int
i915_spirv_chain_block(
	struct i915_spirv_parser *parser,
	struct i915_spirv_id *record,
	struct i915_spirv_id *pointee,
	struct i915_spirv_id *index_record,
	uint32_t index_id,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *column;
	uint32_t scalars[MAX_COMPONENTS];
	uint32_t scalar_count;
	uint32_t selected;
	int constant;

	/* A constant index names its element now; any other is an IR integer. */
	constant = 0;
	selected = 0U;
	if (index_record->kind == ID_CONSTANT) {
		constant = 1;
		selected = index_record->constant;
	}

	/* An element of an array may be selected at run time; nothing else may. */
	if (constant == 0 && pointee->kind != ID_TYPE_ARRAY)
		return i915_spirv_refuse(parser, opcode, offset, "access chain with a dynamic index into something that is not an array");

	/* Steps by what the pointer addresses. */
	switch (pointee->kind) {
	case ID_TYPE_STRUCT:
		/* A member: its Offset, and the layout its decorations give a matrix. */
		if (selected >= pointee->count)
			return EINVAL;
		if (pointee->member_has_offset[selected] == 0U)
			return i915_spirv_refuse(parser, opcode, offset, "block member without an Offset");
		record->byte_offset += pointee->member_offset[selected];
		record->member = (int32_t)selected;
		record->matrix_stride = pointee->member_matrix_stride[selected];
		record->row_major = pointee->member_row_major[selected];
		record->component_stride = 4U;
		record->pointee = pointee->member_type[selected];
		break;

	case ID_TYPE_ARRAY:
		/* An element: the array's stride, which the layout must give. */
		if (pointee->stride == 0U)
			return i915_spirv_refuse(parser, opcode, offset, "array without an ArrayStride in a block");
		if (constant != 0) {
			if (pointee->length != 0U && selected >= pointee->length)
				return EINVAL;
			record->byte_offset += selected * pointee->stride;
		} else {
			/* One dynamic index, over an array short enough to select from. */
			if (record->dynamic_index != NO_VALUE)
				return i915_spirv_refuse(parser, opcode, offset, "access chain with more than one dynamic index");
			if (pointee->length == 0U || pointee->length > MAX_DYNAMIC_ELEMENTS)
				return i915_spirv_refuse(parser, opcode, offset, "dynamic index into an array longer than supported");
			scalar_count = i915_spirv_operand(parser, index_id, scalars);
			if (scalar_count != 1U)
				return i915_spirv_refuse(parser, opcode, offset, "dynamic index that is not an integer scalar");
			record->dynamic_index = scalars[0];
			record->dynamic_stride = pointee->stride;
			record->dynamic_length = pointee->length;
		}
		record->component_stride = 4U;
		record->pointee = pointee->type;
		break;

	case ID_TYPE_MATRIX:
		/* A column: MatrixStride apart when column-major, four bytes apart when row-major. */
		column = i915_spirv_id(parser, pointee->type);
		if (column == NULL || selected >= pointee->count)
			return EINVAL;
		if (record->matrix_stride == 0U)
			return i915_spirv_refuse(parser, opcode, offset, "matrix without a MatrixStride in a block");
		if (record->row_major != 0U) {
			record->byte_offset += selected * 4U;
			record->component_stride = record->matrix_stride;
		} else {
			record->byte_offset += selected * record->matrix_stride;
			record->component_stride = 4U;
		}
		record->pointee = pointee->type;
		break;

	case ID_TYPE_VECTOR:
		/* A component: its stride apart. */
		if (selected >= pointee->count)
			return EINVAL;
		record->byte_offset += selected * record->component_stride;
		record->pointee = pointee->type;
		break;

	default:
		return i915_spirv_refuse(parser, opcode, offset, "access chain into a scalar");
	}

	/* Succeeded: the pointer addresses the selected part. */
	return 0;
}

/*
 * Steps a pointer into a local, an input or an output one index down: an
 * output block's member, a matrix column (a local's), a vector component.
 */
static int
i915_spirv_chain_scalars(
	struct i915_spirv_parser *parser,
	struct i915_spirv_id *record,
	struct i915_spirv_id *pointee,
	struct i915_spirv_id *index_record,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *column;
	uint32_t first;
	uint32_t selected;

	/* Only a constant index names one scalar at parse time. */
	if (index_record->kind != ID_CONSTANT)
		return i915_spirv_refuse(parser, opcode, offset, "access chain with a dynamic index");
	selected = index_record->constant;

	/* The first scalar addressed so far. */
	first = 0U;
	if (record->component >= 0)
		first = (uint32_t)record->component;

	/* Selects a structure member first (an output block), then a column, then a component. */
	if (pointee->kind == ID_TYPE_STRUCT && record->member < 0 && record->component < 0) {
		if (selected >= pointee->count)
			return EINVAL;
		record->member = (int32_t)selected;
		record->pointee = pointee->member_type[selected];
	} else if (pointee->kind == ID_TYPE_MATRIX) {
		column = i915_spirv_id(parser, pointee->type);
		if (column == NULL || selected >= pointee->count)
			return EINVAL;
		record->component = (int32_t)(first + selected * column->count);
		record->pointee = pointee->type;
	} else if (pointee->kind == ID_TYPE_VECTOR) {
		if (selected >= pointee->count)
			return EINVAL;
		record->component = (int32_t)(first + selected);
		record->pointee = pointee->type;
	} else {
		return i915_spirv_refuse(parser, opcode, offset, "access chain into an array or nested aggregate");
	}

	/* Succeeded: the pointer names one member, column or component. */
	return 0;
}

/* Lowers OpLoad from a sampler, a local, an output, an input, push constants or a uniform block. */
static int
i915_spirv_lower_load(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *base;
	struct i915_spirv_id *variable;
	struct i915_spirv_id *record;
	struct i915_shader_ir_inst *inst;
	uint32_t components;
	uint32_t loaded;
	uint32_t floats;
	uint32_t first;
	uint32_t index;
	int error;

	/* Resolves the pointer loaded through. */
	base = NULL;
	if (count >= 4U)
		base = i915_spirv_id(parser, word[3]);
	if (base == NULL)
		return EINVAL;
	if (base->kind != ID_VARIABLE && base->kind != ID_POINTER)
		return EINVAL;

	/* Resolves the variable the pointer leads to. */
	variable = i915_spirv_id(parser, base->var);
	if (variable == NULL)
		return EINVAL;

	/* A sampler load names the combined image sampler; it emits nothing. */
	if (base->ptr_kind == PTR_SAMPLER) {
		record = i915_spirv_id(parser, word[2]);
		if (record == NULL || record->kind != ID_NONE)
			return EINVAL;
		record->kind = ID_SAMPLED_IMAGE;
		record->binding = variable->binding;
		record->set = variable->set;
		return 0;
	}

	/* Anything else must load a scalar, a vector or a matrix of the pointee's size. */
	components = i915_spirv_value_components_wide(parser, base->pointee);
	if (components == 0U)
		return i915_spirv_refuse(parser, opcode, offset, "load of something that is not a scalar, a vector or a matrix");
	loaded = i915_spirv_value_components_wide(parser, word[1]);
	if (components != loaded)
		return i915_spirv_refuse(parser, opcode, offset, "load of something that is not a scalar, a vector or a matrix");

	/* A component access chain loads from its component onward. */
	first = 0U;
	if (base->component >= 0)
		first = (uint32_t)base->component;

	/* Loads by what the pointer addresses. */
	if (base->ptr_kind == PTR_LOCAL ||
	    base->ptr_kind == PTR_OUTPUT ||
	    base->ptr_kind == PTR_OUTPUT_BLOCK) {
		/*
		 * Store-to-load forwarding: the scalars currently stored in the local,
		 * or last written to the output (a shader may read back what it
		 * wrote).
		 */
		record = i915_spirv_result(parser, word[2], word[1], components, 0);
		if (record == NULL)
			return EINVAL;

		/* Each component must have been stored before it is read. */
		for (index = 0U; index < components; index++) {
			if (first + index >= MAX_COMPONENTS || variable->comp[first + index] == NO_VALUE)
				return i915_spirv_refuse(parser, opcode, offset, "load of a local or output component that was never stored");
			record->comp[index] = variable->comp[first + index];
		}
	} else if (base->ptr_kind == PTR_INPUT) {
		/* An input is floats: a vertex attribute or an interpolated input. */
		floats = i915_spirv_float_components(parser, word[1]);
		if (floats != components)
			return i915_spirv_refuse(parser, opcode, offset, "load of an input that is not a float scalar or vector");

		/* Each component is a fresh value read from the input location. */
		record = i915_spirv_result(parser, word[2], word[1], components, 1);
		if (record == NULL)
			return EINVAL;

		/* Emits one input load per component. */
		for (index = 0U; index < components; index++) {
			inst = i915_spirv_emit(parser, I915_IR_LOAD_INPUT, record->comp[index], 0U, 0U);
			if (inst != NULL) {
				inst->location = variable->location;
				inst->component = first + index;
			}
		}
	} else if (base->ptr_kind == PTR_PUSH || base->ptr_kind == PTR_UBO) {
		/* Push constants and uniform blocks are read word by word at the chain's offsets. */
		error = i915_spirv_lower_load_block(parser, word, base, variable, opcode, offset);
		if (error != 0)
			return error;
	} else {
		return i915_spirv_refuse(parser, opcode, offset, "load through a pointer that is not an input, push constant, uniform block, sampler or local");
	}

	/* Succeeded: the loaded value names its scalars. */
	return 0;
}

/*
 * Lowers a load from push constants or a uniform block: one word per
 * scalar, at the pointer's byte offset plus the scalar's place in the
 * vector or the matrix (MatrixStride and the layout of the member).  Under a
 * dynamic array index every element is loaded and the one the index names
 * is selected: the index compared with each element number, the element
 * taken where it is equal.
 */
static int
i915_spirv_lower_load_block(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	struct i915_spirv_id *pointer,
	struct i915_spirv_id *variable,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *record;
	struct i915_spirv_id *type;
	struct i915_spirv_id *column;
	struct i915_shader_ir_inst *inst;
	uint32_t selects[MAX_DYNAMIC_ELEMENTS];
	uint32_t columns;
	uint32_t rows;
	uint32_t elements;
	uint32_t column_index;
	uint32_t row;
	uint32_t element;
	uint32_t place;
	uint32_t number;
	uint32_t value;
	uint32_t candidate;
	uint32_t merged;

	/* Finds the shape of what is loaded: columns of rows. */
	type = i915_spirv_id(parser, pointer->pointee);
	if (type == NULL)
		return EINVAL;
	columns = 1U;
	rows = 1U;
	if (type->kind == ID_TYPE_VECTOR) {
		rows = type->count;
	} else if (type->kind == ID_TYPE_MATRIX) {
		column = i915_spirv_id(parser, type->type);
		if (column == NULL)
			return EINVAL;
		columns = type->count;
		rows = column->count;
	}

	/* A matrix needs its stride to be read. */
	if (columns > 1U && pointer->matrix_stride == 0U)
		return i915_spirv_refuse(parser, opcode, offset, "matrix without a MatrixStride in a block");

	/* The loaded value is fresh scalars named below. */
	record = i915_spirv_result(parser, word[2], word[1], columns * rows, 0);
	if (record == NULL)
		return EINVAL;

	/* Under a dynamic index, compares the index with each element number past the first once. */
	elements = 1U;
	if (pointer->dynamic_index != NO_VALUE) {
		elements = pointer->dynamic_length;
		for (element = 1U; element < elements; element++) {
			number = i915_spirv_new_value(parser);
			inst = i915_spirv_emit(parser, I915_IR_ICONST, number, 0U, 0U);
			if (inst != NULL)
				inst->immediate = element;
			selects[element] = i915_spirv_emit_value(parser, I915_IR_IEQ, pointer->dynamic_index, number);
		}
	}

	/* Loads each scalar, column after column. */
	for (column_index = 0U; column_index < columns; column_index++) {
		for (row = 0U; row < rows; row++) {
			/* The scalar's byte in the value: by the matrix layout, or by the vector's stride. */
			if (columns > 1U && pointer->row_major != 0U) {
				place = pointer->byte_offset + row * pointer->matrix_stride + column_index * 4U;
			} else if (columns > 1U) {
				place = pointer->byte_offset + column_index * pointer->matrix_stride + row * 4U;
			} else {
				place = pointer->byte_offset + row * pointer->component_stride;
			}

			/* The first element's word, then each later element's where the index names it. */
			value = i915_spirv_load_word(parser, pointer, variable, place);
			for (element = 1U; element < elements; element++) {
				candidate = i915_spirv_load_word(parser, pointer, variable, place + element * pointer->dynamic_stride);
				merged = i915_spirv_new_value(parser);
				inst = i915_spirv_emit(parser, I915_IR_SELECT, merged, selects[element], candidate);
				if (inst != NULL)
					inst->src[2] = value;
				value = merged;
			}
			record->comp[column_index * rows + row] = value;
		}
	}

	/* A word the parser could not place fails the load. */
	if (parser->error != 0)
		return parser->error;

	/* Succeeded: the load names its scalars. */
	return 0;
}

/*
 * Emits the load of one word of push constants or of a uniform block and
 * returns its value, widening what the shader reads of it.  A word that is
 * not four-byte aligned latches EINVAL.
 */
static uint32_t
i915_spirv_load_word(
	struct i915_spirv_parser *parser,
	const struct i915_spirv_id *pointer,
	const struct i915_spirv_id *variable,
	uint32_t byte)
{
	struct i915_shader_ir_uniform *uniform;
	struct i915_shader_ir_inst *inst;
	uint32_t value;
	uint32_t end;

	/* A word starts on a four-byte boundary. */
	if ((byte & 3U) != 0U) {
		parser->error = EINVAL;
		return 0U;
	}

	/* A push constant widens the push bytes the shader reads. */
	value = i915_spirv_new_value(parser);
	if (pointer->ptr_kind == PTR_PUSH) {
		inst = i915_spirv_emit(parser, I915_IR_LOAD_PUSH, value, 0U, 0U);
		if (inst != NULL)
			inst->immediate = byte;
		if (byte + 4U > parser->ir->push_bytes)
			parser->ir->push_bytes = byte + 4U;
		return value;
	}

	/* A uniform block word names its block. */
	inst = i915_spirv_emit(parser, I915_IR_LOAD_UBO, value, 0U, 0U);
	if (inst != NULL) {
		inst->immediate = byte;
		inst->location = variable->uniform;
	}

	/* Widens the range of the block the shader reads to the word. */
	uniform = &parser->ir->uniforms[variable->uniform];
	if (uniform->size == 0U) {
		uniform->offset = byte;
		uniform->size = 4U;
	} else {
		end = uniform->offset + uniform->size;
		if (byte < uniform->offset)
			uniform->offset = byte;
		if (byte + 4U > end)
			end = byte + 4U;
		uniform->size = end - uniform->offset;
	}

	/* Succeeded: the word's value. */
	return value;
}

/* Lowers OpStore to a local or an output. */
static int
i915_spirv_lower_store(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *base;
	struct i915_spirv_id *variable;
	uint32_t scalars[MAX_COMPONENTS];
	uint32_t components;
	uint32_t stored;
	uint32_t first;
	uint32_t index;
	int error;

	/* Resolves the pointer stored through. */
	base = NULL;
	if (count >= 3U)
		base = i915_spirv_id(parser, word[1]);
	if (base == NULL)
		return EINVAL;
	if (base->kind != ID_VARIABLE && base->kind != ID_POINTER)
		return EINVAL;

	/*
	 * Resolves the variable, the pointee's size and the stored scalars; the
	 * operand may emit a constant, and it does so before the variable is
	 * checked.
	 */
	variable = i915_spirv_id(parser, base->var);
	components = i915_spirv_value_components_wide(parser, base->pointee);
	stored = i915_spirv_operand_wide(parser, word[2], scalars);
	if (variable == NULL)
		return EINVAL;

	/* The stored value must be a scalar, a vector or a matrix of the pointee's size. */
	if (components == 0U || stored != components)
		return i915_spirv_refuse(parser, opcode, offset, "store of something that is not a scalar, a vector or a matrix of the pointee's size");

	/* A component access chain stores from its component onward. */
	first = 0U;
	if (base->component >= 0)
		first = (uint32_t)base->component;
	if (first + components > MAX_COMPONENTS)
		return EINVAL;

	/* Stores by what the pointer addresses. */
	if (base->ptr_kind == PTR_LOCAL) {
		/*
		 * A local only remembers which scalars it now holds; under a
		 * predicate, the new scalar where it is true and the old one where
		 * it is false (a component never stored has no old value to keep).
		 */
		for (index = 0U; index < components; index++) {
			if (variable->comp[first + index] == NO_VALUE) {
				variable->comp[first + index] = scalars[index];
			} else {
				variable->comp[first + index] = i915_spirv_predicated_value(parser, scalars[index], variable->comp[first + index]);
			}
		}
	} else if (base->ptr_kind == PTR_OUTPUT || base->ptr_kind == PTR_OUTPUT_BLOCK) {
		/* An output store becomes one output write per component. */
		if (components > 4U)
			return i915_spirv_refuse(parser, opcode, offset, "store of a matrix to an output");
		error = i915_spirv_lower_store_output(parser, base, variable, scalars, components, first, opcode, offset);
		if (error != 0)
			return error;
	} else {
		return i915_spirv_refuse(parser, opcode, offset, "store through a pointer that is not an output or a local");
	}

	/* Succeeded: the store is lowered. */
	return 0;
}

/* Lowers a store to a located output or to the Position builtin. */
static int
i915_spirv_lower_store_output(
	struct i915_spirv_parser *parser,
	struct i915_spirv_id *pointer,
	struct i915_spirv_id *variable,
	const uint32_t *scalars,
	uint32_t count,
	uint32_t first,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *type;
	struct i915_shader_ir_inst *inst;
	uint32_t previous;
	uint32_t value;
	uint32_t builtin;
	uint32_t index;
	int is_builtin;

	/* An output block is written only through its Position builtin. */
	builtin = 0U;
	is_builtin = 0;
	if (pointer->ptr_kind == PTR_OUTPUT_BLOCK) {
		/* The builtin is on the variable, or on the member the chain selected. */
		type = i915_spirv_id(parser, variable->pointee);
		if (variable->has_builtin != 0U) {
			builtin = variable->builtin;
			is_builtin = 1;
		} else if (pointer->member >= 0 &&
		    type != NULL &&
		    type->kind == ID_TYPE_STRUCT &&
		    type->member_builtin[pointer->member] != 0U) {
			builtin = type->member_builtin[pointer->member] - 1U;
			is_builtin = 1;
		}

		/* Any other builtin, or an unlocated plain output, is refused. */
		if (is_builtin == 0 || builtin != BUILTIN_POSITION)
			return i915_spirv_refuse(parser, opcode, offset, "store to an output that is neither located nor the Position builtin");
	}

	/* Emits one output write per component. */
	for (index = 0U; index < count; index++) {
		/*
		 * Under a predicate the component keeps what it held where the
		 * predicate is false: the last value stored, or the zero every output
		 * starts as.
		 */
		value = scalars[index];
		if (parser->predicate != PREDICATE_ALWAYS) {
			previous = variable->comp[first + index];
			if (previous == NO_VALUE)
				previous = i915_spirv_shared_constant(parser, &parser->zero_value, I915_IR_CONST, FLOAT_ZERO_BITS);
			value = i915_spirv_predicated_value(parser, value, previous);
		}
		variable->comp[first + index] = value;

		/* Writes the component. */
		inst = i915_spirv_emit(parser, I915_IR_STORE_OUTPUT, 0U, value, 0U);
		if (inst == NULL)
			continue;

		/* The Position builtin is written to its own location. */
		if (is_builtin != 0) {
			inst->location = I915_IR_LOCATION_POSITION;
		} else {
			inst->location = variable->location;
		}
		inst->component = first + index;
	}

	/* Succeeded: the output is written. */
	return 0;
}

/* Lowers per-component OpFAdd, OpFSub and OpFMul. */
static int
i915_spirv_lower_arithmetic(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *record;
	enum i915_shader_ir_op op;
	uint32_t left[4];
	uint32_t right[4];
	uint32_t left_count;
	uint32_t right_count;
	uint32_t components;
	uint32_t index;

	/* The instruction must carry both operands. */
	if (count < 5U)
		return EINVAL;

	/* Resolves both operands; either may emit a constant. */
	left_count = i915_spirv_operand(parser, word[3], left);
	right_count = i915_spirv_operand(parser, word[4], right);

	/* Both operands and the result must be float scalars or vectors of one size. */
	if (left_count == 0U || left_count != right_count)
		return i915_spirv_refuse(parser, opcode, offset, "arithmetic on operands that are not float scalars / vectors of one size");
	components = i915_spirv_float_components(parser, word[1]);
	if (left_count != components)
		return i915_spirv_refuse(parser, opcode, offset, "arithmetic on operands that are not float scalars / vectors of one size");

	/* Declares the result as fresh scalars. */
	record = i915_spirv_result(parser, word[2], word[1], left_count, 1);
	if (record == NULL)
		return EINVAL;

	/* Chooses the IR operation of the SPIR-V one. */
	if (opcode == OP_FADD) {
		op = I915_IR_FADD;
	} else if (opcode == OP_FSUB) {
		op = I915_IR_FSUB;
	} else {
		op = I915_IR_FMUL;
	}

	/* Emits one operation per component. */
	for (index = 0U; index < left_count; index++)
		(void)i915_spirv_emit(parser, op, record->comp[index], left[index], right[index]);

	/* Succeeded: the arithmetic is lowered. */
	return 0;
}

/*
 * Lowers the two-operand integer instructions per component: add,
 * subtract, multiply, the divisions and remainders, the shifts and the
 * bitwise operations.
 */
static int
i915_spirv_lower_integer(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *record;
	uint32_t left[4];
	uint32_t right[4];
	uint32_t left_count;
	uint32_t right_count;
	uint32_t components;
	uint32_t left_integers;
	uint32_t right_integers;
	uint32_t index;

	/* The instruction must carry both operands. */
	if (count != 5U)
		return EINVAL;

	/* Resolves both operands; either may emit a constant. */
	left_count = i915_spirv_operand(parser, word[3], left);
	right_count = i915_spirv_operand(parser, word[4], right);

	/* The operands and the result must be integer scalars or vectors of one size. */
	components = i915_spirv_int_components(parser, word[1]);
	left_integers = i915_spirv_operand_int_components(parser, word[3]);
	right_integers = i915_spirv_operand_int_components(parser, word[4]);
	if (components == 0U || left_count != components || right_count != components)
		return i915_spirv_refuse(parser, opcode, offset, "integer operation on operands that are not integer scalars / vectors of the result's size");
	if (left_integers != components || right_integers != components)
		return i915_spirv_refuse(parser, opcode, offset, "integer operation on operands that are not integer scalars / vectors of the result's size");

	/* Declares the result; its scalars are named by the lowering of each component. */
	record = i915_spirv_result(parser, word[2], word[1], components, 0);
	if (record == NULL)
		return EINVAL;

	/* Lowers each component on its own. */
	for (index = 0U; index < components; index++)
		record->comp[index] = i915_spirv_lower_integer_component(parser, opcode, left[index], right[index]);

	/* Succeeded: the integer operation is lowered. */
	return 0;
}

/*
 * Lowers one component of a two-operand integer instruction and returns
 * the IR value of the result.
 *
 * The divisions and remainders are the hardware's (see
 * i915_compile_divide()): OpSDiv is IDIV, OpSRem IREM, OpUDiv UDIV and OpUMod
 * UMOD.  OpSMod is the remainder moved to the divisor's sign as Mesa lowers
 * imod on Gen12.0 (brw_fs_nir.cpp): where the remainder is not zero and the
 * operands' signs differ (their exclusive or is negative), the divisor is
 * added to it.
 */
static uint32_t
i915_spirv_lower_integer_component(
	struct i915_spirv_parser *parser,
	uint32_t opcode,
	uint32_t left,
	uint32_t right)
{
	uint32_t zero;
	uint32_t remainder;
	uint32_t nonzero;
	uint32_t signs;
	uint32_t different;
	uint32_t moves;
	uint32_t moved;

	/* The operations the IR has as they are. */
	switch (opcode) {
	case OP_IADD:
		return i915_spirv_emit_value(parser, I915_IR_IADD, left, right);

	case OP_ISUB:
		return i915_spirv_emit_value(parser, I915_IR_ISUB, left, right);

	case OP_IMUL:
		return i915_spirv_emit_value(parser, I915_IR_IMUL, left, right);

	case OP_UDIV:
		return i915_spirv_emit_value(parser, I915_IR_UDIV, left, right);

	case OP_UMOD:
		return i915_spirv_emit_value(parser, I915_IR_UMOD, left, right);

	case OP_SDIV:
		return i915_spirv_emit_value(parser, I915_IR_IDIV, left, right);

	case OP_SREM:
		return i915_spirv_emit_value(parser, I915_IR_IREM, left, right);

	case OP_SHIFT_RIGHT_LOGICAL:
		return i915_spirv_emit_value(parser, I915_IR_SHR, left, right);

	case OP_SHIFT_RIGHT_ARITHMETIC:
		return i915_spirv_emit_value(parser, I915_IR_ASR, left, right);

	case OP_SHIFT_LEFT_LOGICAL:
		return i915_spirv_emit_value(parser, I915_IR_SHL, left, right);

	case OP_BITWISE_OR:
		return i915_spirv_emit_value(parser, I915_IR_IOR, left, right);

	case OP_BITWISE_XOR:
		return i915_spirv_emit_value(parser, I915_IR_IXOR, left, right);

	case OP_BITWISE_AND:
		return i915_spirv_emit_value(parser, I915_IR_IAND, left, right);

	default:
		break;
	}

	/* OpSMod: the signed remainder, which has the dividend's sign. */
	remainder = i915_spirv_emit_value(parser, I915_IR_IREM, left, right);

	/* It moves where it is not zero and the operands' signs differ. */
	zero = i915_spirv_integer_constant(parser, 0U);
	nonzero = i915_spirv_emit_value(parser, I915_IR_INE, remainder, zero);
	signs = i915_spirv_emit_value(parser, I915_IR_IXOR, left, right);
	different = i915_spirv_emit_value(parser, I915_IR_ILT, signs, zero);
	moves = i915_spirv_emit_value(parser, I915_IR_AND, nonzero, different);
	moved = i915_spirv_emit_value(parser, I915_IR_IADD, remainder, right);

	/* Succeeded: the modulus with the divisor's sign. */
	return i915_spirv_select_value(parser, moves, moved, remainder);
}

/* Returns the absolute value of an integer whose negativity is already known. */
static uint32_t
i915_spirv_absolute_integer(
	struct i915_spirv_parser *parser,
	uint32_t value,
	uint32_t negative)
{
	uint32_t negated;

	/* The negation where the value is negative, the value elsewhere. */
	negated = i915_spirv_emit_value(parser, I915_IR_INEG, value, 0U);

	/* Succeeded: the magnitude. */
	return i915_spirv_select_value(parser, negative, negated, value);
}

/* Lowers OpSNegate and OpNot per component. */
static int
i915_spirv_lower_integer_unary(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *record;
	enum i915_shader_ir_op op;
	uint32_t operand[4];
	uint32_t operand_count;
	uint32_t components;
	uint32_t integers;
	uint32_t index;

	/* The instruction must carry its operand. */
	if (count != 4U)
		return EINVAL;

	/* Resolves the operand; it may emit a constant. */
	operand_count = i915_spirv_operand(parser, word[3], operand);

	/* The operand and the result must be integer scalars or vectors of one size. */
	components = i915_spirv_int_components(parser, word[1]);
	integers = i915_spirv_operand_int_components(parser, word[3]);
	if (components == 0U || operand_count != components || integers != components)
		return i915_spirv_refuse(parser, opcode, offset, "integer operation on an operand that is not an integer scalar / vector of the result's size");

	/* Declares the result as fresh scalars. */
	record = i915_spirv_result(parser, word[2], word[1], components, 1);
	if (record == NULL)
		return EINVAL;

	/* A negation or a complement. */
	if (opcode == OP_SNEGATE) {
		op = I915_IR_INEG;
	} else {
		op = I915_IR_INOT;
	}

	/* Emits one operation per component. */
	for (index = 0U; index < components; index++)
		(void)i915_spirv_emit(parser, op, record->comp[index], operand[index], 0U);

	/* Succeeded: the operation is lowered. */
	return 0;
}

/* Lowers the conversions between floats and signed or unsigned integers per component. */
static int
i915_spirv_lower_convert(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *record;
	enum i915_shader_ir_op op;
	uint32_t operand[4];
	uint32_t operand_count;
	uint32_t components;
	uint32_t index;

	/* The instruction must carry its operand. */
	if (count != 4U)
		return EINVAL;

	/* Resolves the operand; it may emit a constant. */
	operand_count = i915_spirv_operand(parser, word[3], operand);

	/* A float result from an integer, or an integer result from a float, of one size. */
	if (opcode == OP_CONVERT_S_TO_F || opcode == OP_CONVERT_U_TO_F) {
		components = i915_spirv_float_components(parser, word[1]);
		if (i915_spirv_operand_int_components(parser, word[3]) != operand_count)
			components = 0U;
	} else {
		components = i915_spirv_int_components(parser, word[1]);
		if (i915_spirv_operand_float_components(parser, word[3]) != operand_count)
			components = 0U;
	}
	if (components == 0U || operand_count != components)
		return i915_spirv_refuse(parser, opcode, offset, "conversion between operands of other kinds or sizes");

	/* Declares the result as fresh scalars. */
	record = i915_spirv_result(parser, word[2], word[1], components, 1);
	if (record == NULL)
		return EINVAL;

	/* Chooses the conversion. */
	if (opcode == OP_CONVERT_F_TO_U) {
		op = I915_IR_F2U;
	} else if (opcode == OP_CONVERT_F_TO_S) {
		op = I915_IR_F2I;
	} else if (opcode == OP_CONVERT_S_TO_F) {
		op = I915_IR_I2F;
	} else {
		op = I915_IR_U2F;
	}

	/* Emits one conversion per component. */
	for (index = 0U; index < components; index++)
		(void)i915_spirv_emit(parser, op, record->comp[index], operand[index], 0U);

	/* Succeeded: the conversion is lowered. */
	return 0;
}

/*
 * Lowers OpBitcast between 32-bit floats and integers of one size: no IR,
 * the result names the operand's scalars, whose bits it reads as they are.
 */
static int
i915_spirv_lower_bitcast(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *record;
	uint32_t operand[4];
	uint32_t operand_count;
	uint32_t components;
	uint32_t booleans;
	uint32_t index;

	/* The instruction must carry its operand. */
	if (count != 4U)
		return EINVAL;

	/* Resolves the operand; it may emit a constant. */
	operand_count = i915_spirv_operand(parser, word[3], operand);

	/* A float or integer scalar or vector of the operand's size; a Boolean has no bits to cast. */
	components = i915_spirv_value_components(parser, word[1]);
	booleans = i915_spirv_bool_components(parser, word[1]);
	if (components == 0U || booleans != 0U || operand_count != components)
		return i915_spirv_refuse(parser, opcode, offset, "bitcast that is not between 32-bit scalars / vectors of one size");

	/* Declares the result as the operand's scalars. */
	record = i915_spirv_result(parser, word[2], word[1], components, 0);
	if (record == NULL)
		return EINVAL;
	for (index = 0U; index < components; index++)
		record->comp[index] = operand[index];

	/* Succeeded: the bits are renamed. */
	return 0;
}

/*
 * Lowers OpFMod and OpFRem per component as Mesa's nir_lower_fmod does:
 * x - y * floor(x / y) and x - y * trunc(x / y), the division as the
 * multiplication by the reciprocal OpFDiv lowers to.
 */
static int
i915_spirv_lower_float_remainder(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *record;
	uint32_t left[4];
	uint32_t right[4];
	uint32_t left_count;
	uint32_t right_count;
	uint32_t components;
	uint32_t reciprocal;
	uint32_t quotient;
	uint32_t rounded;
	uint32_t product;
	uint32_t index;

	/* The instruction must carry both operands. */
	if (count != 5U)
		return EINVAL;

	/* Resolves both operands; either may emit a constant. */
	left_count = i915_spirv_operand(parser, word[3], left);
	right_count = i915_spirv_operand(parser, word[4], right);

	/* Both operands and the result must be float scalars or vectors of one size. */
	components = i915_spirv_float_components(parser, word[1]);
	if (components == 0U || left_count != components || right_count != components)
		return i915_spirv_refuse(parser, opcode, offset, "remainder of operands that are not float scalars / vectors of one size");

	/* Declares the result; its scalars are the differences. */
	record = i915_spirv_result(parser, word[2], word[1], components, 0);
	if (record == NULL)
		return EINVAL;

	/* x - y * round(x / y), rounding down for FMod and toward zero for FRem. */
	for (index = 0U; index < components; index++) {
		reciprocal = i915_spirv_emit_value(parser, I915_IR_RCP, right[index], 0U);
		quotient = i915_spirv_emit_value(parser, I915_IR_FMUL, left[index], reciprocal);
		if (opcode == OP_FMOD) {
			rounded = i915_spirv_emit_value(parser, I915_IR_FLOOR, quotient, 0U);
		} else {
			rounded = i915_spirv_emit_value(parser, I915_IR_FTRUNC, quotient, 0U);
		}
		product = i915_spirv_emit_value(parser, I915_IR_FMUL, right[index], rounded);
		record->comp[index] = i915_spirv_emit_value(parser, I915_IR_FSUB, left[index], product);
	}

	/* Succeeded: the remainder is lowered. */
	return 0;
}

/* Lowers OpVectorTimesScalar to one multiply per component. */
static int
i915_spirv_lower_vector_times_scalar(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *record;
	uint32_t vector[4];
	uint32_t scalar[4];
	uint32_t vector_count;
	uint32_t scalar_count;
	uint32_t components;
	uint32_t index;

	/* The instruction must carry both operands. */
	if (count < 5U)
		return EINVAL;

	/* Resolves the vector and the scalar; either may emit a constant. */
	vector_count = i915_spirv_operand(parser, word[3], vector);
	scalar_count = i915_spirv_operand(parser, word[4], scalar);

	/* The operands must be a float vector and a float scalar, the result the vector's size. */
	if (vector_count < 2U || scalar_count != 1U)
		return i915_spirv_refuse(parser, opcode, offset, "OpVectorTimesScalar operands");
	components = i915_spirv_float_components(parser, word[1]);
	if (vector_count != components)
		return i915_spirv_refuse(parser, opcode, offset, "OpVectorTimesScalar operands");

	/* Declares the result as fresh scalars. */
	record = i915_spirv_result(parser, word[2], word[1], vector_count, 1);
	if (record == NULL)
		return EINVAL;

	/* Emits one multiply by the scalar per component. */
	for (index = 0U; index < vector_count; index++)
		(void)i915_spirv_emit(parser, I915_IR_FMUL, record->comp[index], vector[index], scalar[0]);

	/* Succeeded: the product is lowered. */
	return 0;
}

/*
 * Lowers the matrix products: matrix times scalar, vector times matrix,
 * matrix times vector and matrix times matrix, column-major scalars, each
 * sum accumulated in the order of the SPIR-V definition (spirv_to_nir's
 * matrix_multiply adds the column products one after the other).
 */
static int
i915_spirv_lower_matrix_product(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *record;
	uint32_t left[MAX_COMPONENTS];
	uint32_t right[MAX_COMPONENTS];
	uint32_t left_count;
	uint32_t right_count;
	uint32_t left_columns;
	uint32_t left_rows;
	uint32_t right_columns;
	uint32_t right_rows;
	uint32_t components;
	uint32_t column;
	uint32_t row;
	uint32_t inner;
	uint32_t product;
	uint32_t sum;

	/* The instruction must carry both operands. */
	if (count != 5U)
		return EINVAL;

	/* Resolves both operands and their shapes; a vector is one column. */
	left_count = i915_spirv_operand_wide(parser, word[3], left);
	right_count = i915_spirv_operand_wide(parser, word[4], right);
	i915_spirv_operand_shape(parser, word[3], &left_columns, &left_rows);
	i915_spirv_operand_shape(parser, word[4], &right_columns, &right_rows);
	components = i915_spirv_float_components_wide(parser, word[1]);
	if (left_count == 0U || right_count == 0U || components == 0U)
		return i915_spirv_refuse(parser, opcode, offset, "matrix product of operands that are not float scalars, vectors or matrices");

	/* Declares the result; its scalars are named by the products and the sums. */
	record = i915_spirv_result(parser, word[2], word[1], components, 0);
	if (record == NULL)
		return EINVAL;

	/* A matrix times a scalar: every scalar of the matrix times it. */
	if (opcode == OP_MATRIX_TIMES_SCALAR) {
		if (right_count != 1U || components != left_count)
			return i915_spirv_refuse(parser, opcode, offset, "OpMatrixTimesScalar operands");
		for (column = 0U; column < left_count; column++)
			record->comp[column] = i915_spirv_emit_value(parser, I915_IR_FMUL, left[column], right[0]);
		return 0;
	}

	/* A vector times a matrix: the dot product of the vector with each column. */
	if (opcode == OP_VECTOR_TIMES_MATRIX) {
		if (left_columns != 1U || left_rows != right_rows || components != right_columns)
			return i915_spirv_refuse(parser, opcode, offset, "OpVectorTimesMatrix operands");
		for (column = 0U; column < right_columns; column++) {
			sum = NO_VALUE;
			for (inner = 0U; inner < right_rows; inner++) {
				product = i915_spirv_emit_value(parser, I915_IR_FMUL, left[inner], right[column * right_rows + inner]);
				if (sum == NO_VALUE) {
					sum = product;
				} else {
					sum = i915_spirv_emit_value(parser, I915_IR_FADD, sum, product);
				}
			}
			record->comp[column] = sum;
		}
		return 0;
	}

	/* A matrix times a vector or a matrix: each column of the result is the left matrix times a right column. */
	if (left_columns != right_rows || components != right_columns * left_rows)
		return i915_spirv_refuse(parser, opcode, offset, "matrix product of operands whose shapes do not agree");
	if (opcode == OP_MATRIX_TIMES_VECTOR && right_columns != 1U)
		return i915_spirv_refuse(parser, opcode, offset, "OpMatrixTimesVector operands");
	for (column = 0U; column < right_columns; column++) {
		for (row = 0U; row < left_rows; row++) {
			/* The row of the left matrix times the right column, the column products added in order. */
			sum = NO_VALUE;
			for (inner = 0U; inner < left_columns; inner++) {
				product = i915_spirv_emit_value(parser, I915_IR_FMUL, left[inner * left_rows + row], right[column * right_rows + inner]);
				if (sum == NO_VALUE) {
					sum = product;
				} else {
					sum = i915_spirv_emit_value(parser, I915_IR_FADD, sum, product);
				}
			}
			record->comp[column * left_rows + row] = sum;
		}
	}

	/* Succeeded: the product is lowered. */
	return 0;
}

/* Lowers OpTranspose: no IR, the result names the matrix's scalars row after row. */
static int
i915_spirv_lower_transpose(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *record;
	uint32_t matrix[MAX_COMPONENTS];
	uint32_t matrix_count;
	uint32_t columns;
	uint32_t rows;
	uint32_t column;
	uint32_t row;

	/* The instruction must carry the matrix. */
	if (count != 4U)
		return EINVAL;

	/* Resolves the matrix and its shape. */
	matrix_count = i915_spirv_operand_wide(parser, word[3], matrix);
	i915_spirv_operand_shape(parser, word[3], &columns, &rows);
	if (matrix_count == 0U || columns < 2U)
		return i915_spirv_refuse(parser, opcode, offset, "OpTranspose of something that is not a matrix");
	if (i915_spirv_float_components_wide(parser, word[1]) != matrix_count)
		return i915_spirv_refuse(parser, opcode, offset, "OpTranspose of something that is not a matrix");

	/* Declares the result; column r of the result is row r of the matrix. */
	record = i915_spirv_result(parser, word[2], word[1], matrix_count, 0);
	if (record == NULL)
		return EINVAL;
	for (column = 0U; column < columns; column++) {
		for (row = 0U; row < rows; row++)
			record->comp[row * columns + column] = matrix[column * rows + row];
	}

	/* Succeeded: the transpose names its scalars. */
	return 0;
}

/*
 * Lowers OpOuterProduct: column c of the result is the first vector times
 * component c of the second, one multiply per scalar (spirv_to_nir builds
 * the same columns).
 */
static int
i915_spirv_lower_outer_product(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *record;
	uint32_t left[4];
	uint32_t right[4];
	uint32_t left_count;
	uint32_t right_count;
	uint32_t components;
	uint32_t column;
	uint32_t row;

	/* The instruction must carry both vectors. */
	if (count != 5U)
		return EINVAL;

	/* Resolves both vectors; either may emit a constant. */
	left_count = i915_spirv_operand(parser, word[3], left);
	right_count = i915_spirv_operand(parser, word[4], right);
	if (left_count < 2U || right_count < 2U)
		return i915_spirv_refuse(parser, opcode, offset, "OpOuterProduct of operands that are not float vectors");

	/* The result has a column per component of the second vector, a row per component of the first. */
	components = i915_spirv_float_components_wide(parser, word[1]);
	if (components != left_count * right_count)
		return i915_spirv_refuse(parser, opcode, offset, "OpOuterProduct whose result is not the operands' shape");

	/* Declares the result; its scalars are the products, column after column. */
	record = i915_spirv_result(parser, word[2], word[1], components, 0);
	if (record == NULL)
		return EINVAL;
	for (column = 0U; column < right_count; column++) {
		for (row = 0U; row < left_count; row++)
			record->comp[column * left_count + row] = i915_spirv_emit_value(parser, I915_IR_FMUL, left[row], right[column]);
	}

	/* Succeeded: the outer product is lowered. */
	return 0;
}

/* Lowers OpFNegate to one negation per component. */
static int
i915_spirv_lower_negate(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *record;
	uint32_t operand[4];
	uint32_t operand_count;
	uint32_t components;
	uint32_t index;

	/* The instruction must carry its operand. */
	if (count < 4U)
		return EINVAL;

	/* Resolves the operand; it may emit a constant. */
	operand_count = i915_spirv_operand(parser, word[3], operand);

	/* The operand and the result must be float scalars or vectors of one size. */
	if (operand_count == 0U)
		return i915_spirv_refuse(parser, opcode, offset, "OpFNegate operand");
	components = i915_spirv_float_components(parser, word[1]);
	if (operand_count != components)
		return i915_spirv_refuse(parser, opcode, offset, "OpFNegate operand");

	/* Declares the result as fresh scalars. */
	record = i915_spirv_result(parser, word[2], word[1], operand_count, 1);
	if (record == NULL)
		return EINVAL;

	/* Emits one negation per component. */
	for (index = 0U; index < operand_count; index++)
		(void)i915_spirv_emit(parser, I915_IR_FNEG, record->comp[index], operand[index], 0U);

	/* Succeeded: the negation is lowered. */
	return 0;
}

/* Lowers dot(a, b) = a0*b0 + a1*b1 + ...: multiplies, then a chain of adds. */
static int
i915_spirv_lower_dot(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *record;
	uint32_t left[4];
	uint32_t right[4];
	uint32_t left_count;
	uint32_t right_count;
	uint32_t components;

	/* The instruction must carry both operands. */
	if (count < 5U)
		return EINVAL;

	/* Resolves both operands; either may emit a constant. */
	left_count = i915_spirv_operand(parser, word[3], left);
	right_count = i915_spirv_operand(parser, word[4], right);

	/* The operands must be float vectors of one size, the result a float scalar. */
	if (left_count < 2U || left_count != right_count)
		return i915_spirv_refuse(parser, opcode, offset, "OpDot operands");
	components = i915_spirv_float_components(parser, word[1]);
	if (components != 1U)
		return i915_spirv_refuse(parser, opcode, offset, "OpDot operands");

	/* Declares the result; its scalar is the last sum. */
	record = i915_spirv_result(parser, word[2], word[1], 1U, 0);
	if (record == NULL)
		return EINVAL;
	record->comp[0] = i915_spirv_dot_value(parser, left, right, left_count);

	/* Succeeded: the dot product is lowered. */
	return 0;
}

/* Returns the dot product of two float vectors: the products added in order. */
static uint32_t
i915_spirv_dot_value(
	struct i915_spirv_parser *parser,
	const uint32_t *left,
	const uint32_t *right,
	uint32_t components)
{
	uint32_t product;
	uint32_t sum;
	uint32_t index;

	/* Multiplies each component pair and adds each product to the running sum. */
	sum = NO_VALUE;
	for (index = 0U; index < components; index++) {
		product = i915_spirv_emit_value(parser, I915_IR_FMUL, left[index], right[index]);

		/* The first product starts the sum; each later one is added to it. */
		if (sum == NO_VALUE) {
			sum = product;
		} else {
			sum = i915_spirv_emit_value(parser, I915_IR_FADD, sum, product);
		}
	}

	/* Succeeded: the final sum. */
	return sum;
}

/*
 * Lowers OpCompositeConstruct of a vector (of floats, integers or
 * Booleans) or of a matrix (of columns): no IR, only naming.
 */
static int
i915_spirv_lower_construct(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *record;
	uint32_t constituent[MAX_COMPONENTS];
	uint32_t constituent_count;
	uint32_t components;
	uint32_t filled;
	uint32_t component;
	uint32_t index;

	/* The instruction must name its type and result. */
	if (count < 3U)
		return EINVAL;

	/* Only a vector or a matrix is constructed. */
	components = i915_spirv_value_components_wide(parser, word[1]);
	if (components < 2U)
		return i915_spirv_refuse(parser, opcode, offset, "composite that is not a vector or a matrix");

	/* Declares the result; its scalars are named by the constituents. */
	record = i915_spirv_result(parser, word[2], word[1], components, 0);
	if (record == NULL)
		return EINVAL;

	/* Names the scalars of each constituent in order. */
	filled = 0U;
	for (index = 3U; index < count; index++) {
		constituent_count = i915_spirv_operand_wide(parser, word[index], constituent);
		if (constituent_count == 0U)
			return i915_spirv_refuse(parser, opcode, offset, "composite constituent that is not a scalar or a vector");
		if (filled + constituent_count > components)
			return EINVAL;

		/* Copies the constituent's scalars into place. */
		for (component = 0U; component < constituent_count; component++)
			record->comp[filled + component] = constituent[component];
		filled += constituent_count;
	}

	/* The constituents must fill the composite exactly. */
	if (filled != components)
		return EINVAL;

	/* Succeeded: the composite names its scalars. */
	return 0;
}

/*
 * Lowers OpCompositeExtract of a component from a vector, or of a column
 * or a component from a matrix: no IR, only naming.
 */
static int
i915_spirv_lower_extract(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *record;
	uint32_t composite[MAX_COMPONENTS];
	uint32_t composite_count;
	uint32_t components;
	uint32_t columns;
	uint32_t rows;
	uint32_t first;
	uint32_t length;
	uint32_t index;

	/* The instruction must carry the composite and one or two indices. */
	if (count < 5U)
		return EINVAL;
	if (count > 6U)
		return i915_spirv_refuse(parser, opcode, offset, "extract from a nested aggregate");

	/* Resolves the composite and its shape; it may emit a constant. */
	composite_count = i915_spirv_operand_wide(parser, word[3], composite);
	i915_spirv_operand_shape(parser, word[3], &columns, &rows);
	if (composite_count < 2U)
		return i915_spirv_refuse(parser, opcode, offset, "extract from something that is not a vector or a matrix");

	/* Finds the scalars the indices select: a component of a vector, a column or a component of a matrix. */
	if (columns == 1U) {
		if (count != 5U || word[4] >= rows)
			return EINVAL;
		first = word[4];
		length = 1U;
	} else if (count == 5U) {
		if (word[4] >= columns)
			return EINVAL;
		first = word[4] * rows;
		length = rows;
	} else {
		if (word[4] >= columns || word[5] >= rows)
			return EINVAL;
		first = word[4] * rows + word[5];
		length = 1U;
	}

	/* The result must be what the indices select. */
	components = i915_spirv_value_components(parser, word[1]);
	if (components != length)
		return i915_spirv_refuse(parser, opcode, offset, "extract whose result is not what the indices select");

	/* Declares the result as the selected scalars. */
	record = i915_spirv_result(parser, word[2], word[1], length, 0);
	if (record == NULL)
		return EINVAL;
	for (index = 0U; index < length; index++)
		record->comp[index] = composite[first + index];

	/* Succeeded: the scalars are named. */
	return 0;
}

/* Lowers OpVectorShuffle: no IR, only naming. */
static int
i915_spirv_lower_shuffle(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *record;
	uint32_t left[4];
	uint32_t right[4];
	uint32_t left_count;
	uint32_t right_count;
	uint32_t components;
	uint32_t selector;
	uint32_t index;

	/* The instruction must carry both vectors and at least one component. */
	if (count < 6U)
		return EINVAL;

	/* Resolves both vectors; either may emit a constant. */
	left_count = i915_spirv_operand(parser, word[3], left);
	right_count = i915_spirv_operand(parser, word[4], right);

	/* Both operands must be vectors and the result as long as the selector list. */
	if (left_count < 2U || right_count < 2U)
		return i915_spirv_refuse(parser, opcode, offset, "OpVectorShuffle operands");
	components = i915_spirv_value_components(parser, word[1]);
	if (components != count - 5U)
		return i915_spirv_refuse(parser, opcode, offset, "OpVectorShuffle operands");

	/* Declares the result; its scalars are named by the selectors. */
	record = i915_spirv_result(parser, word[2], word[1], count - 5U, 0);
	if (record == NULL)
		return EINVAL;

	/* Names each selected scalar from the first or the second vector. */
	for (index = 5U; index < count; index++) {
		selector = word[index];
		if (selector == SHUFFLE_UNDEFINED)
			return i915_spirv_refuse(parser, opcode, offset, "OpVectorShuffle with an undefined component");
		if (selector >= left_count + right_count)
			return EINVAL;

		/* Selectors past the first vector index into the second. */
		if (selector < left_count) {
			record->comp[index - 5U] = left[selector];
		} else {
			record->comp[index - 5U] = right[selector - left_count];
		}
	}

	/* Succeeded: the shuffled vector names its scalars. */
	return 0;
}

/*
 * Lowers a GLSL.std.450 instruction.
 *
 * Per component: Round, RoundEven, Trunc, FAbs, SAbs, FSign, SSign, Floor,
 * Ceil, Fract, Radians,
 * Degrees, Sin, Cos, Tan, Pow, Exp, Log, Exp2, Log2, Sqrt, InverseSqrt,
 * FMin, UMin, SMin, FMax, UMax, SMax, FClamp, UClamp, SClamp, FMix, Step
 * and SmoothStep, each the way Mesa lowers it for Gen12 where Mesa has a
 * lowering (see the component lowering).  On whole vectors: Length,
 * Distance, Cross, Normalize and Reflect.  Any other instruction of the set
 * is refused.
 */
static int
i915_spirv_lower_extended(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *record;
	uint32_t operand[3][4];
	uint32_t operand_count[3];
	uint32_t operands;
	uint32_t components;
	uint32_t function;
	uint32_t index;
	int integers;

	/* The instruction must carry the set, the number and one operand. */
	if (count < 6U)
		return EINVAL;

	/* The functions of whole vectors have a lowering of their own. */
	function = word[4];
	if (function == GLSL_NORMALIZE ||
	    function == GLSL_LENGTH ||
	    function == GLSL_DISTANCE ||
	    function == GLSL_CROSS ||
	    function == GLSL_REFLECT)
		return i915_spirv_lower_geometric(parser, word, count, opcode, offset);

	/* Counts the operands of each lowered function and notes the integer ones; any other is refused. */
	integers = 0;
	switch (function) {
	case GLSL_ROUND:
	case GLSL_ROUND_EVEN:
	case GLSL_TRUNC:
	case GLSL_FABS:
	case GLSL_FSIGN:
	case GLSL_FLOOR:
	case GLSL_CEIL:
	case GLSL_FRACT:
	case GLSL_RADIANS:
	case GLSL_DEGREES:
	case GLSL_SIN:
	case GLSL_COS:
	case GLSL_TAN:
	case GLSL_EXP:
	case GLSL_LOG:
	case GLSL_EXP2:
	case GLSL_LOG2:
	case GLSL_SQRT:
	case GLSL_INVERSE_SQRT:
		operands = 1U;
		break;

	case GLSL_SABS:
	case GLSL_SSIGN:
		operands = 1U;
		integers = 1;
		break;

	case GLSL_POW:
	case GLSL_FMIN:
	case GLSL_FMAX:
	case GLSL_STEP:
		operands = 2U;
		break;

	case GLSL_UMIN:
	case GLSL_SMIN:
	case GLSL_UMAX:
	case GLSL_SMAX:
		operands = 2U;
		integers = 1;
		break;

	case GLSL_FCLAMP:
	case GLSL_FMIX:
	case GLSL_SMOOTH_STEP:
		operands = 3U;
		break;

	case GLSL_UCLAMP:
	case GLSL_SCLAMP:
		operands = 3U;
		integers = 1;
		break;

	default:
		return i915_spirv_refuse(parser, opcode, offset, "GLSL.std.450 instruction that is not lowered");
	}

	/* The instruction must carry exactly the function's operands. */
	if (count != 5U + operands)
		return EINVAL;

	/* The result must be a float (or, for the integer functions, integer) scalar or vector. */
	if (integers != 0) {
		components = i915_spirv_int_components(parser, word[1]);
	} else {
		components = i915_spirv_float_components(parser, word[1]);
	}
	if (components == 0U)
		return i915_spirv_refuse(parser, opcode, offset, "extended instruction operand");

	/* Resolves each operand, which has the result's size; an operand may emit a constant. */
	for (index = 0U; index < operands; index++) {
		operand_count[index] = i915_spirv_operand(parser, word[5U + index], operand[index]);
		if (operand_count[index] != components)
			return i915_spirv_refuse(parser, opcode, offset, "extended instruction operand");
	}

	/* Declares the result; its scalars are named by the lowering of each component. */
	record = i915_spirv_result(parser, word[2], word[1], components, 0);
	if (record == NULL)
		return EINVAL;

	/* Lowers each component on its own. */
	for (index = 0U; index < components; index++)
		record->comp[index] = i915_spirv_lower_extended_component(parser, function, operand, index);

	/* Succeeded: the function is lowered. */
	return 0;
}

/*
 * Lowers one component of a per-component GLSL.std.450 function and
 * returns the IR value of the result.
 *
 * Round and RoundEven both round a tie to the even integer (RNDE; Mesa
 * lowers Round to fround_even, which SPIR-V's choice of direction allows).
 * Pow is exp2(log2(x) * y) and FClamp is min(max(x, lo), hi), as Mesa
 * lowers them on Gen12 (nir lower_fpow, nir_fclamp); Exp and Log go through
 * exp2 and log2 with log2(e), Tan is sin / cos, Ceil is -floor(-x), FSign
 * keeps a zero or a NaN as it is (nir fsign), Step is x < edge ? 0 : 1 and
 * SmoothStep t * t * (3 - 2 t) with t = clamp((x - e0) / (e1 - e0), 0, 1)
 * (the GLSL specification).  FMix is x * (1 - a) + y * a, the definition of
 * the GLSL specification; Mesa's nir_lower_flrp may pick another
 * association.  The integer minimum, maximum, clamp and absolute value
 * compare and select, and SSign is 1, 0 or -1 by two comparisons.
 */
static uint32_t
i915_spirv_lower_extended_component(
	struct i915_spirv_parser *parser,
	uint32_t function,
	uint32_t operand[3][4],
	uint32_t component)
{
	uint32_t x;
	uint32_t y;
	uint32_t a;
	uint32_t temporary;
	uint32_t other;
	uint32_t one;
	uint32_t zero;

	/* Names the component of each operand. */
	x = operand[0][component];
	y = operand[1][component];
	a = operand[2][component];

	/* Lowers by the function. */
	switch (function) {
	case GLSL_ROUND:
	case GLSL_ROUND_EVEN:
		return i915_spirv_emit_value(parser, I915_IR_FROUND_EVEN, x, 0U);

	case GLSL_TRUNC:
		return i915_spirv_emit_value(parser, I915_IR_FTRUNC, x, 0U);

	case GLSL_FABS:
		return i915_spirv_emit_value(parser, I915_IR_FABS, x, 0U);

	case GLSL_FLOOR:
		return i915_spirv_emit_value(parser, I915_IR_FLOOR, x, 0U);

	case GLSL_FRACT:
		return i915_spirv_emit_value(parser, I915_IR_FRACT, x, 0U);

	case GLSL_SIN:
		return i915_spirv_emit_value(parser, I915_IR_SIN, x, 0U);

	case GLSL_COS:
		return i915_spirv_emit_value(parser, I915_IR_COS, x, 0U);

	case GLSL_EXP2:
		return i915_spirv_emit_value(parser, I915_IR_EXP2, x, 0U);

	case GLSL_LOG2:
		return i915_spirv_emit_value(parser, I915_IR_LOG2, x, 0U);

	case GLSL_SQRT:
		return i915_spirv_emit_value(parser, I915_IR_SQRT, x, 0U);

	case GLSL_INVERSE_SQRT:
		return i915_spirv_emit_value(parser, I915_IR_RSQ, x, 0U);

	case GLSL_FMIN:
		return i915_spirv_emit_value(parser, I915_IR_FMIN, x, y);

	case GLSL_FMAX:
		return i915_spirv_emit_value(parser, I915_IR_FMAX, x, y);

	case GLSL_CEIL:
		/* -floor(-x). */
		temporary = i915_spirv_emit_value(parser, I915_IR_FNEG, x, 0U);
		temporary = i915_spirv_emit_value(parser, I915_IR_FLOOR, temporary, 0U);
		return i915_spirv_emit_value(parser, I915_IR_FNEG, temporary, 0U);

	case GLSL_FSIGN:
		/* 1 above zero, -1 below it, x itself (a zero or a NaN) otherwise. */
		zero = i915_spirv_shared_constant(parser, &parser->zero_value, I915_IR_CONST, FLOAT_ZERO_BITS);
		one = i915_spirv_shared_constant(parser, &parser->one_value, I915_IR_CONST, FLOAT_ONE_BITS);
		other = i915_spirv_emit_value(parser, I915_IR_FNEG, one, 0U);
		temporary = i915_spirv_emit_value(parser, I915_IR_FLT, x, zero);
		temporary = i915_spirv_select_value(parser, temporary, other, x);
		other = i915_spirv_emit_value(parser, I915_IR_FLT, zero, x);
		return i915_spirv_select_value(parser, other, one, temporary);

	case GLSL_RADIANS:
		/* x * pi / 180. */
		temporary = i915_spirv_float_constant(parser, FLOAT_DEGREE_BITS);
		return i915_spirv_emit_value(parser, I915_IR_FMUL, x, temporary);

	case GLSL_DEGREES:
		/* x * 180 / pi. */
		temporary = i915_spirv_float_constant(parser, FLOAT_RADIAN_BITS);
		return i915_spirv_emit_value(parser, I915_IR_FMUL, x, temporary);

	case GLSL_TAN:
		/* sin(x) / cos(x). */
		temporary = i915_spirv_emit_value(parser, I915_IR_COS, x, 0U);
		temporary = i915_spirv_emit_value(parser, I915_IR_RCP, temporary, 0U);
		other = i915_spirv_emit_value(parser, I915_IR_SIN, x, 0U);
		return i915_spirv_emit_value(parser, I915_IR_FMUL, other, temporary);

	case GLSL_EXP:
		/* 2 ^ (x * log2(e)). */
		temporary = i915_spirv_float_constant(parser, FLOAT_LOG2_E_BITS);
		temporary = i915_spirv_emit_value(parser, I915_IR_FMUL, x, temporary);
		return i915_spirv_emit_value(parser, I915_IR_EXP2, temporary, 0U);

	case GLSL_LOG:
		/* log2(x) * ln(2). */
		temporary = i915_spirv_emit_value(parser, I915_IR_LOG2, x, 0U);
		other = i915_spirv_float_constant(parser, FLOAT_LN_2_BITS);
		return i915_spirv_emit_value(parser, I915_IR_FMUL, temporary, other);

	case GLSL_POW:
		/* 2 ^ (log2(x) * y). */
		temporary = i915_spirv_emit_value(parser, I915_IR_LOG2, x, 0U);
		temporary = i915_spirv_emit_value(parser, I915_IR_FMUL, temporary, y);
		return i915_spirv_emit_value(parser, I915_IR_EXP2, temporary, 0U);

	case GLSL_FCLAMP:
		/* The larger of x and lo, then the smaller of that and hi. */
		temporary = i915_spirv_emit_value(parser, I915_IR_FMAX, x, y);
		return i915_spirv_emit_value(parser, I915_IR_FMIN, temporary, a);

	case GLSL_STEP:
		/* 0 where x (the second operand) is below the edge (the first), 1 elsewhere. */
		zero = i915_spirv_shared_constant(parser, &parser->zero_value, I915_IR_CONST, FLOAT_ZERO_BITS);
		one = i915_spirv_shared_constant(parser, &parser->one_value, I915_IR_CONST, FLOAT_ONE_BITS);
		temporary = i915_spirv_emit_value(parser, I915_IR_FLT, y, x);
		return i915_spirv_select_value(parser, temporary, zero, one);

	case GLSL_SMOOTH_STEP:
		return i915_spirv_smooth_step(parser, x, y, a);

	case GLSL_SABS:
		/* -x where x is negative. */
		zero = i915_spirv_integer_constant(parser, 0U);
		temporary = i915_spirv_emit_value(parser, I915_IR_ILT, x, zero);
		return i915_spirv_absolute_integer(parser, x, temporary);

	case GLSL_SSIGN:
		/* -1 below zero, 1 above it, 0 at it. */
		zero = i915_spirv_integer_constant(parser, 0U);
		one = i915_spirv_integer_constant(parser, 1U);
		other = i915_spirv_integer_constant(parser, 0xFFFFFFFFU);
		temporary = i915_spirv_emit_value(parser, I915_IR_ILT, x, zero);
		temporary = i915_spirv_select_value(parser, temporary, other, zero);
		other = i915_spirv_emit_value(parser, I915_IR_ILT, zero, x);
		return i915_spirv_select_value(parser, other, one, temporary);

	case GLSL_SMIN:
		temporary = i915_spirv_emit_value(parser, I915_IR_ILT, y, x);
		return i915_spirv_select_value(parser, temporary, y, x);

	case GLSL_UMIN:
		temporary = i915_spirv_emit_value(parser, I915_IR_ULT, y, x);
		return i915_spirv_select_value(parser, temporary, y, x);

	case GLSL_SMAX:
		temporary = i915_spirv_emit_value(parser, I915_IR_ILT, x, y);
		return i915_spirv_select_value(parser, temporary, y, x);

	case GLSL_UMAX:
		temporary = i915_spirv_emit_value(parser, I915_IR_ULT, x, y);
		return i915_spirv_select_value(parser, temporary, y, x);

	case GLSL_SCLAMP:
		/* The larger of x and lo, then the smaller of that and hi. */
		temporary = i915_spirv_emit_value(parser, I915_IR_ILT, x, y);
		temporary = i915_spirv_select_value(parser, temporary, y, x);
		other = i915_spirv_emit_value(parser, I915_IR_ILT, a, temporary);
		return i915_spirv_select_value(parser, other, a, temporary);

	case GLSL_UCLAMP:
		/* The same with unsigned comparisons. */
		temporary = i915_spirv_emit_value(parser, I915_IR_ULT, x, y);
		temporary = i915_spirv_select_value(parser, temporary, y, x);
		other = i915_spirv_emit_value(parser, I915_IR_ULT, a, temporary);
		return i915_spirv_select_value(parser, other, a, temporary);

	default:
		break;
	}

	/* FMix: x * (1 - a) + y * a. */
	one = i915_spirv_shared_constant(parser, &parser->one_value, I915_IR_CONST, FLOAT_ONE_BITS);
	temporary = i915_spirv_emit_value(parser, I915_IR_FSUB, one, a);
	temporary = i915_spirv_emit_value(parser, I915_IR_FMUL, x, temporary);
	other = i915_spirv_emit_value(parser, I915_IR_FMUL, y, a);

	/* Succeeded: the sum of the two weighted operands. */
	return i915_spirv_emit_value(parser, I915_IR_FADD, temporary, other);
}

/* Returns one component of smoothstep(e0, e1, x): t * t * (3 - 2 t), t = clamp((x - e0) / (e1 - e0), 0, 1). */
static uint32_t
i915_spirv_smooth_step(
	struct i915_spirv_parser *parser,
	uint32_t edge0,
	uint32_t edge1,
	uint32_t x)
{
	uint32_t zero;
	uint32_t one;
	uint32_t two;
	uint32_t three;
	uint32_t span;
	uint32_t t;
	uint32_t term;

	/* The constants the polynomial needs. */
	zero = i915_spirv_shared_constant(parser, &parser->zero_value, I915_IR_CONST, FLOAT_ZERO_BITS);
	one = i915_spirv_shared_constant(parser, &parser->one_value, I915_IR_CONST, FLOAT_ONE_BITS);
	two = i915_spirv_float_constant(parser, FLOAT_TWO_BITS);
	three = i915_spirv_float_constant(parser, FLOAT_THREE_BITS);

	/* t: where x lies between the edges, clamped to [0, 1]. */
	span = i915_spirv_emit_value(parser, I915_IR_FSUB, edge1, edge0);
	span = i915_spirv_emit_value(parser, I915_IR_RCP, span, 0U);
	t = i915_spirv_emit_value(parser, I915_IR_FSUB, x, edge0);
	t = i915_spirv_emit_value(parser, I915_IR_FMUL, t, span);
	t = i915_spirv_emit_value(parser, I915_IR_FMAX, t, zero);
	t = i915_spirv_emit_value(parser, I915_IR_FMIN, t, one);

	/* t * t * (3 - 2 t). */
	term = i915_spirv_emit_value(parser, I915_IR_FMUL, two, t);
	term = i915_spirv_emit_value(parser, I915_IR_FSUB, three, term);
	term = i915_spirv_emit_value(parser, I915_IR_FMUL, t, term);

	/* Succeeded: the smooth step. */
	return i915_spirv_emit_value(parser, I915_IR_FMUL, t, term);
}

/*
 * Lowers the GLSL.std.450 functions of whole vectors: Length is
 * sqrt(dot(v, v)), Distance the length of p0 - p1, Cross the three
 * differences of products, Normalize v * inversesqrt(dot(v, v)) (Mesa
 * lowers normalize to v / length(v), nir_fast_normalize, and folds the
 * reciprocal of the square root into frsq, which is this sequence) and
 * Reflect I - 2 dot(N, I) N (the GLSL specification).
 */
static int
i915_spirv_lower_geometric(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *record;
	uint32_t first[4];
	uint32_t second[4];
	uint32_t difference[4];
	uint32_t first_count;
	uint32_t second_count;
	uint32_t components;
	uint32_t function;
	uint32_t square;
	uint32_t scale;
	uint32_t two;
	uint32_t index;

	/* Resolves the first operand, the vector every one of these functions takes. */
	function = word[4];
	first_count = i915_spirv_operand(parser, word[5], first);
	if (first_count == 0U || first_count != i915_spirv_operand_float_components(parser, word[5]))
		return i915_spirv_refuse(parser, opcode, offset, "extended instruction operand");

	/* Normalize and Length take one operand; Distance, Cross and Reflect a second of the first's size. */
	second_count = 0U;
	if (function == GLSL_NORMALIZE || function == GLSL_LENGTH) {
		if (count != 6U)
			return EINVAL;
	} else {
		if (count != 7U)
			return EINVAL;
		second_count = i915_spirv_operand(parser, word[6], second);
		if (second_count != first_count)
			return i915_spirv_refuse(parser, opcode, offset, "extended instruction operand");
	}

	/* A length or a distance is a float scalar; the others the operand's size. */
	components = i915_spirv_float_components(parser, word[1]);
	if (function == GLSL_LENGTH || function == GLSL_DISTANCE) {
		if (components != 1U)
			return i915_spirv_refuse(parser, opcode, offset, "extended instruction operand");
	} else if (components != first_count) {
		return i915_spirv_refuse(parser, opcode, offset, "extended instruction operand");
	}

	/* A cross product is of three-component vectors. */
	if (function == GLSL_CROSS && first_count != 3U)
		return i915_spirv_refuse(parser, opcode, offset, "cross product of vectors that do not have three components");

	/* Declares the result; its scalars are named below. */
	record = i915_spirv_result(parser, word[2], word[1], components, 0);
	if (record == NULL)
		return EINVAL;

	/* Lowers by the function. */
	switch (function) {
	case GLSL_LENGTH:
		/* sqrt(dot(v, v)). */
		square = i915_spirv_dot_value(parser, first, first, first_count);
		record->comp[0] = i915_spirv_emit_value(parser, I915_IR_SQRT, square, 0U);
		break;

	case GLSL_DISTANCE:
		/* The length of p0 - p1. */
		for (index = 0U; index < first_count; index++)
			difference[index] = i915_spirv_emit_value(parser, I915_IR_FSUB, first[index], second[index]);
		square = i915_spirv_dot_value(parser, difference, difference, first_count);
		record->comp[0] = i915_spirv_emit_value(parser, I915_IR_SQRT, square, 0U);
		break;

	case GLSL_CROSS:
		/* (a.y b.z - a.z b.y, a.z b.x - a.x b.z, a.x b.y - a.y b.x). */
		for (index = 0U; index < 3U; index++) {
			square = i915_spirv_emit_value(parser, I915_IR_FMUL, first[(index + 1U) % 3U], second[(index + 2U) % 3U]);
			scale = i915_spirv_emit_value(parser, I915_IR_FMUL, first[(index + 2U) % 3U], second[(index + 1U) % 3U]);
			record->comp[index] = i915_spirv_emit_value(parser, I915_IR_FSUB, square, scale);
		}
		break;

	case GLSL_NORMALIZE:
		/* v times the inverse of its length. */
		square = i915_spirv_dot_value(parser, first, first, first_count);
		scale = i915_spirv_emit_value(parser, I915_IR_RSQ, square, 0U);
		for (index = 0U; index < first_count; index++)
			record->comp[index] = i915_spirv_emit_value(parser, I915_IR_FMUL, first[index], scale);
		break;

	default:
		/* Reflect: I - 2 dot(N, I) N. */
		two = i915_spirv_float_constant(parser, FLOAT_TWO_BITS);
		square = i915_spirv_dot_value(parser, second, first, first_count);
		scale = i915_spirv_emit_value(parser, I915_IR_FMUL, two, square);
		for (index = 0U; index < first_count; index++) {
			square = i915_spirv_emit_value(parser, I915_IR_FMUL, scale, second[index]);
			record->comp[index] = i915_spirv_emit_value(parser, I915_IR_FSUB, first[index], square);
		}
		break;
	}

	/* Succeeded: the function is lowered. */
	return 0;
}

/* Lowers OpFDiv per component as Mesa does (lower_fdiv): a * (1 / b). */
static int
i915_spirv_lower_divide(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *record;
	uint32_t left[4];
	uint32_t right[4];
	uint32_t left_count;
	uint32_t right_count;
	uint32_t components;
	uint32_t reciprocal;
	uint32_t index;

	/* The instruction must carry both operands. */
	if (count < 5U)
		return EINVAL;

	/* Resolves both operands; either may emit a constant. */
	left_count = i915_spirv_operand(parser, word[3], left);
	right_count = i915_spirv_operand(parser, word[4], right);

	/* Both operands and the result must be float scalars or vectors of one size. */
	components = i915_spirv_float_components(parser, word[1]);
	if (components == 0U || left_count != components || right_count != components)
		return i915_spirv_refuse(parser, opcode, offset, "division of operands that are not float scalars / vectors of one size");

	/* Declares the result; its scalars are the products. */
	record = i915_spirv_result(parser, word[2], word[1], components, 0);
	if (record == NULL)
		return EINVAL;

	/* The dividend times the reciprocal of the divisor, per component. */
	for (index = 0U; index < components; index++) {
		reciprocal = i915_spirv_emit_value(parser, I915_IR_RCP, right[index], 0U);
		record->comp[index] = i915_spirv_emit_value(parser, I915_IR_FMUL, left[index], reciprocal);
	}

	/* Succeeded: the division is lowered. */
	return 0;
}

/* Lowers a float comparison per component to a Boolean. */
static int
i915_spirv_lower_compare(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *record;
	uint32_t left[4];
	uint32_t right[4];
	uint32_t left_count;
	uint32_t right_count;
	uint32_t components;
	uint32_t index;

	/* The instruction must carry both operands. */
	if (count != 5U)
		return EINVAL;

	/* Resolves both operands; either may emit a constant. */
	left_count = i915_spirv_operand(parser, word[3], left);
	right_count = i915_spirv_operand(parser, word[4], right);

	/* The operands must be float vectors of the Boolean result's size. */
	components = i915_spirv_bool_components(parser, word[1]);
	if (components == 0U || left_count != components || right_count != components)
		return i915_spirv_refuse(parser, opcode, offset, "comparison of operands that are not float scalars / vectors of the result's size");

	/* Declares the Boolean result. */
	record = i915_spirv_result(parser, word[2], word[1], components, 0);
	if (record == NULL)
		return EINVAL;

	/* Compares each component pair. */
	for (index = 0U; index < components; index++)
		record->comp[index] = i915_spirv_lower_compare_component(parser, opcode, left[index], right[index]);

	/* Succeeded: the comparison is lowered. */
	return 0;
}

/*
 * Lowers one component of a float comparison, as Mesa's spirv_to_nir does
 * (vtn_alu.c): four tests (<, >=, ==, and the unordered !=), the operands
 * swapped for > and <=, the unordered <, >, <=, >= as the negation of the
 * opposite ordered test, FUnordEqual as == or either operand NaN, and
 * FOrdNotEqual as the unordered != with both operands numbers.
 */
static uint32_t
i915_spirv_lower_compare_component(
	struct i915_spirv_parser *parser,
	uint32_t opcode,
	uint32_t left,
	uint32_t right)
{
	uint32_t test;
	uint32_t left_nan;
	uint32_t right_nan;
	uint32_t either_nan;
	uint32_t equal;
	uint32_t left_number;
	uint32_t right_number;
	uint32_t both_numbers;
	uint32_t different;

	/* Lowers by the comparison. */
	switch (opcode) {
	case OP_FORD_EQUAL:
		return i915_spirv_emit_value(parser, I915_IR_FEQ, left, right);

	case OP_FUNORD_NOT_EQUAL:
		return i915_spirv_emit_value(parser, I915_IR_FNEU, left, right);

	case OP_FORD_LESS_THAN:
		return i915_spirv_emit_value(parser, I915_IR_FLT, left, right);

	case OP_FORD_GREATER_THAN:
		return i915_spirv_emit_value(parser, I915_IR_FLT, right, left);

	case OP_FORD_LESS_THAN_EQUAL:
		return i915_spirv_emit_value(parser, I915_IR_FGE, right, left);

	case OP_FORD_GREATER_THAN_EQUAL:
		return i915_spirv_emit_value(parser, I915_IR_FGE, left, right);

	case OP_FUNORD_LESS_THAN:
		/* Not left >= right. */
		test = i915_spirv_emit_value(parser, I915_IR_FGE, left, right);
		return i915_spirv_emit_value(parser, I915_IR_NOT, test, 0U);

	case OP_FUNORD_GREATER_THAN:
		/* Not right >= left. */
		test = i915_spirv_emit_value(parser, I915_IR_FGE, right, left);
		return i915_spirv_emit_value(parser, I915_IR_NOT, test, 0U);

	case OP_FUNORD_LESS_THAN_EQUAL:
		/* Not right < left. */
		test = i915_spirv_emit_value(parser, I915_IR_FLT, right, left);
		return i915_spirv_emit_value(parser, I915_IR_NOT, test, 0U);

	case OP_FUNORD_GREATER_THAN_EQUAL:
		/* Not left < right. */
		test = i915_spirv_emit_value(parser, I915_IR_FLT, left, right);
		return i915_spirv_emit_value(parser, I915_IR_NOT, test, 0U);

	case OP_FUNORD_EQUAL:
		/* left == right, or either is NaN (a NaN is not equal to itself). */
		equal = i915_spirv_emit_value(parser, I915_IR_FEQ, left, right);
		left_nan = i915_spirv_emit_value(parser, I915_IR_FNEU, left, left);
		right_nan = i915_spirv_emit_value(parser, I915_IR_FNEU, right, right);
		either_nan = i915_spirv_emit_value(parser, I915_IR_OR, left_nan, right_nan);
		return i915_spirv_emit_value(parser, I915_IR_OR, equal, either_nan);

	default:
		break;
	}

	/* FOrdNotEqual: left != right, and both are numbers (a number is equal to itself). */
	different = i915_spirv_emit_value(parser, I915_IR_FNEU, left, right);
	left_number = i915_spirv_emit_value(parser, I915_IR_FEQ, left, left);
	right_number = i915_spirv_emit_value(parser, I915_IR_FEQ, right, right);
	both_numbers = i915_spirv_emit_value(parser, I915_IR_AND, left_number, right_number);

	/* Succeeded: different and ordered. */
	return i915_spirv_emit_value(parser, I915_IR_AND, different, both_numbers);
}

/*
 * Lowers an integer comparison per component to a Boolean: equality, and
 * the signed or unsigned orderings, > and <= with the operands swapped
 * (Mesa's spirv_to_nir, vtn_alu.c).
 */
static int
i915_spirv_lower_integer_compare(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *record;
	enum i915_shader_ir_op op;
	uint32_t left[4];
	uint32_t right[4];
	uint32_t left_count;
	uint32_t right_count;
	uint32_t components;
	uint32_t index;
	int swapped;

	/* The instruction must carry both operands. */
	if (count != 5U)
		return EINVAL;

	/* Resolves both operands; either may emit a constant. */
	left_count = i915_spirv_operand(parser, word[3], left);
	right_count = i915_spirv_operand(parser, word[4], right);

	/* The operands must be integer vectors of the Boolean result's size. */
	components = i915_spirv_bool_components(parser, word[1]);
	if (components == 0U || left_count != components || right_count != components)
		return i915_spirv_refuse(parser, opcode, offset, "comparison of operands that are not integer scalars / vectors of the result's size");
	if (i915_spirv_operand_int_components(parser, word[3]) != components)
		return i915_spirv_refuse(parser, opcode, offset, "comparison of operands that are not integer scalars / vectors of the result's size");

	/* Picks the test and whether the operands are swapped. */
	swapped = 0;
	switch (opcode) {
	case OP_IEQUAL:
		op = I915_IR_IEQ;
		break;

	case OP_INOT_EQUAL:
		op = I915_IR_INE;
		break;

	case OP_ULESS_THAN:
		op = I915_IR_ULT;
		break;

	case OP_SLESS_THAN:
		op = I915_IR_ILT;
		break;

	case OP_UGREATER_THAN:
		op = I915_IR_ULT;
		swapped = 1;
		break;

	case OP_SGREATER_THAN:
		op = I915_IR_ILT;
		swapped = 1;
		break;

	case OP_UGREATER_THAN_EQUAL:
		op = I915_IR_UGE;
		break;

	case OP_SGREATER_THAN_EQUAL:
		op = I915_IR_IGE;
		break;

	case OP_ULESS_THAN_EQUAL:
		op = I915_IR_UGE;
		swapped = 1;
		break;

	default:
		/* OpSLessThanEqual. */
		op = I915_IR_IGE;
		swapped = 1;
		break;
	}

	/* Declares the Boolean result. */
	record = i915_spirv_result(parser, word[2], word[1], components, 0);
	if (record == NULL)
		return EINVAL;

	/* Compares each component pair. */
	for (index = 0U; index < components; index++) {
		if (swapped != 0) {
			record->comp[index] = i915_spirv_emit_value(parser, op, right[index], left[index]);
		} else {
			record->comp[index] = i915_spirv_emit_value(parser, op, left[index], right[index]);
		}
	}

	/* Succeeded: the comparison is lowered. */
	return 0;
}

/*
 * Lowers OpLogicalAnd, OpLogicalOr, OpLogicalNot, OpLogicalEqual and
 * OpLogicalNotEqual per component; the last two compare the all-ones /
 * zero words as integers.
 */
static int
i915_spirv_lower_logical(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *record;
	enum i915_shader_ir_op op;
	uint32_t left[4];
	uint32_t right[4];
	uint32_t left_count;
	uint32_t right_count;
	uint32_t components;
	uint32_t index;

	/* A not carries one operand, the others two. */
	if (opcode == OP_LOGICAL_NOT && count != 4U)
		return EINVAL;
	if (opcode != OP_LOGICAL_NOT && count != 5U)
		return EINVAL;

	/* Resolves the operands; the second of a not is the first again. */
	left_count = i915_spirv_operand(parser, word[3], left);
	right_count = left_count;
	kern_memcpy(right, left, sizeof(right));
	if (opcode != OP_LOGICAL_NOT)
		right_count = i915_spirv_operand(parser, word[4], right);

	/* The operands and the result must be Booleans of one size. */
	components = i915_spirv_bool_components(parser, word[1]);
	if (components == 0U || left_count != components || right_count != components)
		return i915_spirv_refuse(parser, opcode, offset, "logical operation on operands that are not Booleans of the result's size");

	/* Chooses the IR operation of the SPIR-V one. */
	if (opcode == OP_LOGICAL_AND) {
		op = I915_IR_AND;
	} else if (opcode == OP_LOGICAL_OR) {
		op = I915_IR_OR;
	} else if (opcode == OP_LOGICAL_EQUAL) {
		op = I915_IR_IEQ;
	} else if (opcode == OP_LOGICAL_NOT_EQUAL) {
		op = I915_IR_INE;
	} else {
		op = I915_IR_NOT;
	}

	/* Declares the result; its scalars are named by the operations. */
	record = i915_spirv_result(parser, word[2], word[1], components, 0);
	if (record == NULL)
		return EINVAL;

	/* Emits one operation per component. */
	for (index = 0U; index < components; index++)
		record->comp[index] = i915_spirv_emit_value(parser, op, left[index], right[index]);

	/* Succeeded: the logical operation is lowered. */
	return 0;
}

/*
 * Lowers OpSelect per component: a scalar condition applies to every
 * component, a vector one component by component.
 */
static int
i915_spirv_lower_select(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *record;
	struct i915_shader_ir_inst *inst;
	uint32_t condition[4];
	uint32_t taken[4];
	uint32_t other[4];
	uint32_t condition_count;
	uint32_t taken_count;
	uint32_t other_count;
	uint32_t components;
	uint32_t chosen;
	uint32_t index;

	/* The instruction must carry the condition and both values. */
	if (count != 6U)
		return EINVAL;

	/* Resolves the condition and the values; any may emit a constant. */
	condition_count = i915_spirv_operand(parser, word[3], condition);
	taken_count = i915_spirv_operand(parser, word[4], taken);
	other_count = i915_spirv_operand(parser, word[5], other);

	/* The values have the result's size; the condition is one Boolean or one to a component. */
	components = i915_spirv_value_components(parser, word[1]);
	if (components == 0U || taken_count != components || other_count != components)
		return i915_spirv_refuse(parser, opcode, offset, "OpSelect of values that are not scalars / vectors of the result's size");
	if (condition_count != 1U && condition_count != components)
		return i915_spirv_refuse(parser, opcode, offset, "OpSelect condition of another size");

	/* Declares the result as fresh scalars. */
	record = i915_spirv_result(parser, word[2], word[1], components, 1);
	if (record == NULL)
		return EINVAL;

	/* Emits one selection per component. */
	for (index = 0U; index < components; index++) {
		/* A scalar condition chooses for every component. */
		chosen = condition[0];
		if (condition_count != 1U)
			chosen = condition[index];

		/* The selection reads three sources; the third is set after the emit. */
		inst = i915_spirv_emit(parser, I915_IR_SELECT, record->comp[index], chosen, taken[index]);
		if (inst != NULL)
			inst->src[2] = other[index];
	}

	/* Succeeded: the selection is lowered. */
	return 0;
}

/* Lowers texture(sampler2D, vec2): one instruction, four result scalars dst .. dst + 3. */
static int
i915_spirv_lower_sample(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *sampler;
	struct i915_spirv_id *record;
	struct i915_shader_ir_inst *inst;
	uint32_t coordinate[4];
	uint32_t coordinate_count;
	uint32_t components;

	/* The instruction must carry the sampler and the coordinate. */
	if (count < 5U)
		return EINVAL;

	/* Resolves the sampler and the coordinate; the coordinate may emit a constant. */
	sampler = i915_spirv_id(parser, word[3]);
	coordinate_count = i915_spirv_operand(parser, word[4], coordinate);

	/*
	 * Only a loaded combined sampler at a two-float coordinate, with no image
	 * operands, giving a four-float result, is lowered.
	 */
	if (sampler == NULL ||
	    sampler->kind != ID_SAMPLED_IMAGE ||
	    coordinate_count != 2U ||
	    count != 5U)
		return i915_spirv_refuse(parser, opcode, offset, "sample that is not texture(sampler2D, vec2) without operands");
	components = i915_spirv_float_components(parser, word[1]);
	if (components != 4U)
		return i915_spirv_refuse(parser, opcode, offset, "sample that is not texture(sampler2D, vec2) without operands");

	/* Declares the result as four fresh scalars. */
	record = i915_spirv_result(parser, word[2], word[1], 4U, 1);
	if (record == NULL)
		return EINVAL;

	/* The reply is four consecutive values, so the scalars must be consecutive. */
	if (record->comp[1] != record->comp[0] + 1U || record->comp[3] != record->comp[0] + 3U)
		return EINVAL;

	/* Emits the sample: set in `location`, binding in `immediate`. */
	inst = i915_spirv_emit(parser, I915_IR_SAMPLE, record->comp[0], coordinate[0], coordinate[1]);
	if (inst != NULL) {
		inst->immediate = sampler->binding;
		inst->location = sampler->set;
	}

	/* Succeeded: the sample is lowered. */
	return 0;
}

/*
 * Opens a block: finds the predicate of the channels that run it.
 *
 * The entry block runs for every channel.  The merge block of a selection
 * runs for the channels that ran its header, unless a block inside leaked
 * (see struct i915_spirv_construct); the merge block of a loop runs for the
 * channels that entered the loop; any other block, and a merge block after
 * a leak, runs for the channels of its incoming edges.  A block no edge
 * reaches runs for no channel.  A loop header then runs for the channels
 * still in its loop (see i915_spirv_loop_open()).
 */
static int
i915_spirv_lower_label(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *record;
	struct i915_spirv_construct *construct;
	struct i915_spirv_loop *loop;
	struct i915_shader_ir_inst *inst;
	uint32_t predicate;
	uint32_t merge;
	uint32_t continue_target;
	uint32_t index;
	int reached;
	int always;
	int loop_merge;
	int header;
	int error;

	/* Resolves the label, which must be fresh. */
	record = NULL;
	if (count >= 2U)
		record = i915_spirv_id(parser, word[1]);
	if (record == NULL || record->kind != ID_NONE)
		return EINVAL;

	/* The block before must have ended with its terminator. */
	if (parser->block != 0U && parser->terminated == 0)
		return EINVAL;

	/* A merge block of an outer construct before the inner one's is not the structured order this walk follows. */
	for (index = 0U; index + 1U < parser->depth; index++) {
		if (parser->constructs[index].merge == word[1])
			return i915_spirv_refuse(parser, opcode, offset, "merge block before the merge block of a construct inside it");
	}

	/* After a loop's back edge only its merge block may follow. */
	loop = NULL;
	if (parser->loop_depth != 0U)
		loop = &parser->loops[parser->loop_depth - 1U];
	if (loop != NULL && loop->closed != 0 && loop->merge != word[1])
		return i915_spirv_refuse(parser, opcode, offset, "block after a loop's back edge that is not its merge block");

	/* Finds the predicate of the block. */
	loop_merge = 0;
	construct = NULL;
	if (parser->depth != 0U && parser->constructs[parser->depth - 1U].merge == word[1])
		construct = &parser->constructs[parser->depth - 1U];
	if (parser->block == 0U) {
		/* The entry block. */
		predicate = PREDICATE_ALWAYS;
	} else if (loop != NULL && loop->merge == word[1]) {
		/* A loop's merge block: every channel that entered the loop is back. */
		if (loop->closed == 0)
			return i915_spirv_refuse(parser, opcode, offset, "loop merge block before the loop's back edge");
		predicate = loop->entry_predicate;
		error = i915_spirv_loop_close(parser, opcode, offset);
		if (error != 0)
			return error;
		loop_merge = 1;
		construct = NULL;
	} else if (construct != NULL && construct->leaky == 0) {
		/* Every channel that ran the header is back. */
		predicate = construct->predicate;
	} else {
		/* The channels of the incoming edges: the or of their predicates. */
		predicate = NO_VALUE;
		reached = 0;
		always = 0;
		for (index = 0U; index < parser->edge_count; index++) {
			if (parser->edges[index].to != word[1])
				continue;
			reached = 1;
			if (parser->edges[index].predicate == PREDICATE_ALWAYS) {
				always = 1;
			} else if (predicate == NO_VALUE) {
				predicate = parser->edges[index].predicate;
			} else {
				predicate = i915_spirv_emit_value(parser, I915_IR_OR, predicate, parser->edges[index].predicate);
			}
		}

		/* An edge every channel takes, or no edge at all: no channel, a false Boolean. */
		if (always != 0) {
			predicate = PREDICATE_ALWAYS;
		} else if (reached == 0) {
			predicate = i915_spirv_new_value(parser);
			inst = i915_spirv_emit(parser, I915_IR_BOOL, predicate, 0U, 0U);
			if (inst != NULL)
				inst->immediate = 0U;
		}
	}

	/* The merge block of a selection closes its construct. */
	if (construct != NULL)
		parser->depth--;

	/* A block whose terminator follows an OpLoopMerge is a loop header: its loop opens here. */
	header = i915_spirv_find_loop_merge(parser, offset + count, &merge, &continue_target);
	if (header != 0) {
		error = i915_spirv_loop_open(parser, word[1], merge, continue_target, &predicate, opcode, offset);
		if (error != 0)
			return error;
	}

	/* Opens the block. */
	record->kind = ID_LABEL;
	record->comp[0] = predicate;
	parser->block = word[1];
	parser->predicate = predicate;
	parser->terminated = 0;
	parser->pending_merge = NO_VALUE;
	parser->loop_merge_block = loop_merge;

	/* Succeeded: the block is open. */
	return 0;
}

/*
 * Looks ahead from the first instruction of a block to its terminator for
 * an OpLoopMerge, which makes the block a loop header; reports its merge
 * block and continue target.
 */
static int
i915_spirv_find_loop_merge(
	struct i915_spirv_parser *parser,
	uint32_t offset,
	uint32_t *merge,
	uint32_t *continue_target)
{
	const uint32_t *word;
	uint32_t count;
	uint32_t opcode;

	/* Walks the block's instructions up to its terminator. */
	while (offset < parser->words) {
		word = parser->code + offset;
		count = word[0] >> 16;
		opcode = word[0] & 0xFFFFU;

		/* A malformed instruction ends the look; the body walk reports it. */
		if (count == 0U || offset + count > parser->words)
			return 0;

		/* The OpLoopMerge names the loop's merge block and continue target. */
		if (opcode == OP_LOOP_MERGE && count >= 3U) {
			*merge = word[1];
			*continue_target = word[2];
			return 1;
		}

		/* A terminator or a new label ends the block. */
		if (opcode == OP_BRANCH ||
		    opcode == OP_BRANCH_CONDITIONAL ||
		    opcode == OP_SWITCH ||
		    opcode == OP_RETURN ||
		    opcode == OP_KILL ||
		    opcode == OP_UNREACHABLE ||
		    opcode == OP_LABEL ||
		    opcode == OP_FUNCTION_END)
			return 0;

		offset += count;
	}

	/* Succeeded: the block is not a loop header. */
	return 0;
}

/*
 * Opens a loop at its header: the loop variable of the channels in the
 * loop starts as the channels that enter it, every component of a local or
 * an output that holds a value becomes a loop variable, and the loop's
 * construct opens.  The header then runs for the channels in the loop
 * (`*predicate` becomes that loop variable).  LOOP_BEGIN follows the
 * header's phis (i915_spirv_loop_begin()).
 */
static int
i915_spirv_loop_open(
	struct i915_spirv_parser *parser,
	uint32_t header,
	uint32_t merge,
	uint32_t continue_target,
	uint32_t *predicate,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_loop *loop;
	struct i915_spirv_construct *construct;
	struct i915_spirv_id *variable;
	uint32_t entry;
	uint32_t id;
	uint32_t component;
	uint32_t carried;
	int error;

	/* Loops nested deeper than the parser follows are refused. */
	if (parser->loop_depth >= MAX_LOOP_DEPTH)
		return i915_spirv_refuse(parser, opcode, offset, "loops nested too deep");
	if (parser->depth >= MAX_CONSTRUCT_DEPTH)
		return i915_spirv_refuse(parser, opcode, offset, "constructs nested too deep");

	/* Describes the loop. */
	loop = &parser->loops[parser->loop_depth];
	kern_memset(loop, 0, sizeof(*loop));
	loop->header = header;
	loop->merge = merge;
	loop->continue_target = continue_target;
	loop->entry_predicate = *predicate;
	loop->carried_first = parser->carried_count;

	/* The channels in the loop start as the channels that enter it, all of them when every channel does. */
	entry = *predicate;
	if (entry == PREDICATE_ALWAYS)
		entry = i915_spirv_shared_constant(parser, &parser->true_value, I915_IR_BOOL, BOOL_TRUE_BITS);
	loop->active = i915_spirv_move_value(parser, NO_VALUE, entry);

	/* Every component a local or an output holds becomes a loop variable. */
	for (id = 0U; id < parser->bound; id++) {
		variable = &parser->ids[id];
		if (variable->kind != ID_VARIABLE)
			continue;
		if (variable->ptr_kind != PTR_LOCAL &&
		    variable->ptr_kind != PTR_OUTPUT &&
		    variable->ptr_kind != PTR_OUTPUT_BLOCK)
			continue;

		/* Moves each held component into its loop variable and remembers it. */
		for (component = 0U; component < MAX_COMPONENTS; component++) {
			if (variable->comp[component] == NO_VALUE)
				continue;
			carried = i915_spirv_move_value(parser, NO_VALUE, variable->comp[component]);
			variable->comp[component] = carried;
			error = i915_spirv_carry(parser, id, component, carried);
			if (error != 0)
				return error;
		}
	}

	/* Opens the loop's construct: a break leaves the constructs inside it. */
	construct = &parser->constructs[parser->depth];
	construct->merge = merge;
	construct->predicate = loop->active;
	construct->leaky = 0;
	construct->loop = 1;
	loop->construct = parser->depth;
	parser->depth++;
	parser->loop_depth++;

	/* Succeeded: the header runs for the channels in the loop. */
	*predicate = loop->active;
	return 0;
}

/* Starts a loop's body: LOOP_BEGIN, after which every IR value is made inside the loop. */
static void
i915_spirv_loop_begin(
	struct i915_spirv_parser *parser,
	struct i915_spirv_loop *loop)
{
	/* Emits the mark the loop's end jumps back to. */
	(void)i915_spirv_emit(parser, I915_IR_LOOP_BEGIN, 0U, 0U, 0U);

	/* The values made from here on belong to the loop. */
	loop->first_value = parser->ir->value_count;
	loop->begun = 1;
}

/* Lowers OpLoopMerge: the header's loop opened at its label; the operands must agree with it. */
static int
i915_spirv_lower_loop_merge(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_loop *loop;

	/* The instruction names the merge block and the continue target. */
	if (count < 4U)
		return EINVAL;

	/* It belongs to the header of the innermost loop. */
	if (parser->loop_depth == 0U)
		return i915_spirv_refuse(parser, opcode, offset, "OpLoopMerge outside a loop header");
	loop = &parser->loops[parser->loop_depth - 1U];
	if (loop->header != parser->block || loop->begun == 0)
		return i915_spirv_refuse(parser, opcode, offset, "OpLoopMerge outside a loop header");

	/* The look-ahead found the same operands. */
	if (word[1] != loop->merge || word[2] != loop->continue_target)
		return EINVAL;

	/* Succeeded: the loop is as its label opened it. */
	return 0;
}

/*
 * Ends a pass of a loop at its back edge: every loop variable takes what
 * the pass leaves it where the channels go round again -- a header phi
 * the value its back edge brings, a local or an output its current value
 * -- and the channels in the loop become the back edge's; LOOP_END then
 * runs the body again for them.  The new values are all made before any
 * loop variable is moved into, since one may be read for another.
 */
static int
i915_spirv_loop_back_edge(
	struct i915_spirv_parser *parser,
	uint32_t predicate,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_loop *loop;
	struct i915_spirv_id *record;
	struct i915_spirv_id *variable;
	struct i915_spirv_carried *carried;
	uint32_t back[4];
	uint32_t updated[MAX_LOOP_PHIS][4];
	uint32_t back_count;
	uint32_t condition;
	uint32_t phi;
	uint32_t component;
	uint32_t index;
	uint32_t current;

	/* A loop has one back edge. */
	loop = &parser->loops[parser->loop_depth - 1U];
	if (loop->closed != 0)
		return i915_spirv_refuse(parser, opcode, offset, "loop with more than one back edge");

	/* The channels that go round again; every channel when the edge has no condition. */
	condition = predicate;
	if (condition == PREDICATE_ALWAYS)
		condition = i915_spirv_shared_constant(parser, &parser->true_value, I915_IR_BOOL, BOOL_TRUE_BITS);

	/* Makes each header phi's new value: the back edge's value where the channels go round again. */
	for (phi = 0U; phi < loop->phi_count; phi++) {
		record = i915_spirv_id(parser, loop->phi_result[phi]);
		back_count = i915_spirv_operand(parser, loop->phi_back[phi], back);
		if (record == NULL || back_count != record->count)
			return i915_spirv_refuse(parser, opcode, offset, "OpPhi value of another size on a loop's back edge");
		for (component = 0U; component < back_count; component++)
			updated[phi][component] = i915_spirv_select_value(parser, condition, back[component], record->comp[component]);
	}

	/* Makes a copy of each carried component that changed in the pass. */
	for (index = loop->carried_first; index < parser->carried_count; index++) {
		carried = &parser->carried[index];
		variable = &parser->ids[carried->variable];
		current = variable->comp[carried->component];
		if (current == carried->value)
			continue;
		variable->comp[carried->component] = i915_spirv_move_value(parser, NO_VALUE, current);
	}

	/* Moves the phis' new values into their loop variables. */
	for (phi = 0U; phi < loop->phi_count; phi++) {
		record = i915_spirv_id(parser, loop->phi_result[phi]);
		for (component = 0U; component < record->count; component++)
			(void)i915_spirv_move_value(parser, record->comp[component], updated[phi][component]);
	}

	/* Moves the carried components' copies into their loop variables, which the variables hold again. */
	for (index = loop->carried_first; index < parser->carried_count; index++) {
		carried = &parser->carried[index];
		variable = &parser->ids[carried->variable];
		current = variable->comp[carried->component];
		if (current == carried->value)
			continue;
		(void)i915_spirv_move_value(parser, carried->value, current);
		variable->comp[carried->component] = carried->value;
	}

	/* The channels in the loop are the ones that go round again. */
	if (condition != loop->active)
		(void)i915_spirv_move_value(parser, loop->active, condition);

	/* Ends the pass: the body runs again while a channel is in the loop. */
	(void)i915_spirv_emit(parser, I915_IR_LOOP_END, 0U, loop->active, 0U);
	loop->closed = 1;

	/* Succeeded: the loop's body is complete. */
	return 0;
}

/*
 * Closes the innermost loop at its merge block: its construct and its
 * carried components are dropped, its value range is kept (a value made in
 * it is not read after it), and the constants it materialized are
 * materialized again when read after it.
 */
static int
i915_spirv_loop_close(
	struct i915_spirv_parser *parser,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_loop *loop;
	struct i915_spirv_id *record;
	uint32_t first;
	uint32_t end;
	uint32_t id;

	/* The loop's construct must be the innermost one left. */
	loop = &parser->loops[parser->loop_depth - 1U];
	if (parser->depth != loop->construct + 1U)
		return i915_spirv_refuse(parser, opcode, offset, "construct inside a loop still open at the loop's merge block");

	/* The value range stays for the reads after the loop. */
	if (parser->closed_count >= MAX_LOOPS)
		return i915_spirv_refuse(parser, opcode, offset, "more loops than supported");
	first = loop->first_value;
	end = parser->ir->value_count;
	parser->closed[parser->closed_count].first = first;
	parser->closed[parser->closed_count].end = end;
	parser->closed_count++;

	/* A constant first used in the loop is used again as a fresh constant. */
	for (id = 0U; id < parser->bound; id++) {
		record = &parser->ids[id];
		if (record->kind != ID_CONSTANT || record->count == 0U)
			continue;
		if (record->comp[0] >= first && record->comp[0] < end)
			record->count = 0U;
	}

	/* So is a constant the lowering introduced in it. */
	if (parser->zero_value != NO_VALUE && parser->zero_value >= first && parser->zero_value < end)
		parser->zero_value = NO_VALUE;
	if (parser->one_value != NO_VALUE && parser->one_value >= first && parser->one_value < end)
		parser->one_value = NO_VALUE;
	if (parser->true_value != NO_VALUE && parser->true_value >= first && parser->true_value < end)
		parser->true_value = NO_VALUE;

	/* Drops the loop's construct, its carried components and the loop. */
	parser->depth--;
	parser->carried_count = loop->carried_first;
	parser->loop_depth--;

	/* Succeeded: the walk is after the loop. */
	return 0;
}

/* Remembers a component a loop carries, growing the list when it is full. */
static int
i915_spirv_carry(
	struct i915_spirv_parser *parser,
	uint32_t variable,
	uint32_t component,
	uint32_t value)
{
	struct i915_spirv_carried *grown;
	uint32_t capacity;

	/* A full list grows: to 64 entries at first, doubling after. */
	if (parser->carried_count >= parser->carried_capacity) {
		capacity = 64U;
		if (parser->carried_capacity != 0U)
			capacity = parser->carried_capacity * 2U;

		/* Allocates the larger list and moves the entries into it. */
		grown = kern_calloc(capacity, sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;
		if (parser->carried != NULL) {
			kern_memcpy(grown, parser->carried, parser->carried_count * sizeof(*grown));
			kern_free(parser->carried);
		}
		parser->carried = grown;
		parser->carried_capacity = capacity;
	}

	/* Records the component. */
	parser->carried[parser->carried_count].variable = variable;
	parser->carried[parser->carried_count].component = component;
	parser->carried[parser->carried_count].value = value;
	parser->carried_count++;

	/* Succeeded: the loop carries the component. */
	return 0;
}

/*
 * Lowers OpPhi: the value of each incoming edge where that edge's predicate
 * holds.
 *
 * The value of the first edge is taken, and each later edge's value
 * replaces it on that edge's channels; the edges of a block reach disjoint
 * channels, so every channel ends with the value of the edge it came by.
 * A loop header's phi is a loop variable instead (see
 * i915_spirv_lower_header_phi()); a phi of a loop's merge block is refused,
 * since its edges were taken in different passes of the loop.
 */
static int
i915_spirv_lower_phi(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *record;
	struct i915_spirv_loop *loop;
	struct i915_shader_ir_inst *inst;
	uint32_t incoming[4];
	uint32_t incoming_count;
	uint32_t components;
	uint32_t predicate;
	uint32_t merged;
	uint32_t component;
	uint32_t index;
	int first;
	int found;

	/* The instruction carries value / parent pairs. */
	if (count < 5U || ((count - 3U) % 2U) != 0U)
		return EINVAL;

	/* A phi of a loop's merge block reads edges of different passes. */
	if (parser->loop_merge_block != 0)
		return i915_spirv_refuse(parser, opcode, offset, "OpPhi in a loop's merge block");

	/* A loop header's phi carries a value from one pass to the next. */
	if (parser->loop_depth != 0U) {
		loop = &parser->loops[parser->loop_depth - 1U];
		if (loop->header == parser->block && loop->begun == 0)
			return i915_spirv_lower_header_phi(parser, loop, word, count, opcode, offset);
	}

	/* The result must be a scalar or a vector. */
	components = i915_spirv_value_components(parser, word[1]);
	if (components == 0U)
		return i915_spirv_refuse(parser, opcode, offset, "OpPhi of something that is not a scalar / vector");

	/* Declares the result; its scalars are named by the selections. */
	record = i915_spirv_result(parser, word[2], word[1], components, 0);
	if (record == NULL)
		return EINVAL;

	/* Merges the incoming values in the order the instruction lists them. */
	first = 1;
	for (index = 3U; index + 1U < count; index += 2U) {
		/* An edge from a block that never branched here (a block no channel runs) adds nothing. */
		predicate = i915_spirv_edge_predicate(parser, word[index + 1U], parser->block, &found);
		if (found == 0)
			continue;

		/* Resolves the value the edge brings; it may emit a constant. */
		incoming_count = i915_spirv_operand(parser, word[index], incoming);
		if (incoming_count != components)
			return i915_spirv_refuse(parser, opcode, offset, "OpPhi value of another size");

		/* The first value is taken as it is; a later one where its edge's predicate holds. */
		for (component = 0U; component < components; component++) {
			if (first != 0 || predicate == PREDICATE_ALWAYS) {
				record->comp[component] = incoming[component];
				continue;
			}

			/* Selects the edge's value on its channels, the value so far elsewhere. */
			merged = i915_spirv_new_value(parser);
			inst = i915_spirv_emit(parser, I915_IR_SELECT, merged, predicate, incoming[component]);
			if (inst != NULL)
				inst->src[2] = record->comp[component];
			record->comp[component] = merged;
		}
		first = 0;
	}

	/* A phi of a block no edge reaches keeps the first value listed. */
	if (first != 0) {
		incoming_count = i915_spirv_operand(parser, word[3], incoming);
		if (incoming_count != components)
			return i915_spirv_refuse(parser, opcode, offset, "OpPhi value of another size");
		for (component = 0U; component < components; component++)
			record->comp[component] = incoming[component];
	}

	/* Succeeded: the phi names its scalars. */
	return 0;
}

/*
 * Lowers the phi of a loop header: a loop variable per component, moved
 * into from the value of the edge that enters the loop now, and from the
 * value of the back edge at the loop's end (i915_spirv_loop_back_edge()).
 * The edge into the loop comes from a block already lowered; the back edge
 * from a block of the loop, not lowered yet.
 */
static int
i915_spirv_lower_header_phi(
	struct i915_spirv_parser *parser,
	struct i915_spirv_loop *loop,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *record;
	struct i915_spirv_id *parent;
	uint32_t incoming[4];
	uint32_t incoming_count;
	uint32_t components;
	uint32_t back;
	uint32_t index;
	uint32_t component;
	int entered;

	/* The result must be a scalar or a vector, and the header must have room for another phi. */
	components = i915_spirv_value_components(parser, word[1]);
	if (components == 0U)
		return i915_spirv_refuse(parser, opcode, offset, "OpPhi of something that is not a scalar / vector");
	if (loop->phi_count >= MAX_LOOP_PHIS)
		return i915_spirv_refuse(parser, opcode, offset, "loop header with more phis than supported");

	/* Declares the result; its scalars are the loop variables made below. */
	record = i915_spirv_result(parser, word[2], word[1], components, 0);
	if (record == NULL)
		return EINVAL;

	/* Sorts the edges: the one into the loop gives the value now, the back edge its id. */
	entered = 0;
	back = NO_VALUE;
	for (index = 3U; index + 1U < count; index += 2U) {
		parent = i915_spirv_id(parser, word[index + 1U]);
		if (parent == NULL)
			return EINVAL;

		/* A parent not lowered yet is inside the loop: the back edge. */
		if (parent->kind != ID_LABEL) {
			if (back != NO_VALUE)
				return i915_spirv_refuse(parser, opcode, offset, "loop header phi with more than one back edge");
			back = word[index];
			continue;
		}

		/* The edge into the loop; one of them only. */
		if (entered != 0)
			return i915_spirv_refuse(parser, opcode, offset, "loop header phi with more than one edge into the loop");
		incoming_count = i915_spirv_operand(parser, word[index], incoming);
		if (incoming_count != components)
			return i915_spirv_refuse(parser, opcode, offset, "OpPhi value of another size");
		entered = 1;
	}

	/* A header phi needs both edges. */
	if (entered == 0 || back == NO_VALUE)
		return i915_spirv_refuse(parser, opcode, offset, "loop header phi without an edge into the loop and a back edge");

	/* Each component becomes a loop variable holding the value that enters the loop. */
	for (component = 0U; component < components; component++)
		record->comp[component] = i915_spirv_move_value(parser, NO_VALUE, incoming[component]);

	/* Remembers the phi for the back edge. */
	loop->phi_result[loop->phi_count] = word[2];
	loop->phi_back[loop->phi_count] = back;
	loop->phi_count++;

	/* Succeeded: the phi is a set of loop variables. */
	return 0;
}

/* Lowers OpBranch: every channel of the block takes the edge; to the loop header it is the back edge. */
static int
i915_spirv_lower_branch(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;

	/* The instruction names its target. */
	if (count != 2U)
		return EINVAL;

	/* A header needs a conditional branch here: a selection of one way is not structured. */
	if (parser->pending_merge != NO_VALUE)
		return i915_spirv_refuse(parser, opcode, offset, "OpSelectionMerge before an OpBranch");

	/* A branch to the innermost loop's header ends a pass of the loop. */
	if (parser->loop_depth != 0U && word[1] == parser->loops[parser->loop_depth - 1U].header) {
		error = i915_spirv_loop_back_edge(parser, parser->predicate, opcode, offset);
		if (error != 0)
			return error;
		parser->terminated = 1;
		return 0;
	}

	/* Records the edge and ends the block. */
	error = i915_spirv_edge_add(parser, word[1], parser->predicate, opcode, offset);
	if (error != 0)
		return error;
	parser->terminated = 1;

	/* Succeeded: the edge is recorded. */
	return 0;
}

/*
 * Lowers OpBranchConditional: the true edge for the block's channels where
 * the condition holds, the false edge for the others.  After an
 * OpSelectionMerge the block is a header, and its construct opens here.  An
 * edge to the innermost loop's header is the loop's back edge.
 */
static int
i915_spirv_lower_branch_conditional(
	struct i915_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_construct *construct;
	uint32_t condition[4];
	uint32_t condition_count;
	uint32_t negated;
	uint32_t taken;
	uint32_t other;
	uint32_t header;
	int error;

	/* The instruction names the condition and both targets; branch weights may follow. */
	if (count != 4U && count != 6U)
		return EINVAL;

	/* The condition is one Boolean. */
	condition_count = i915_spirv_operand(parser, word[1], condition);
	if (condition_count != 1U)
		return i915_spirv_refuse(parser, opcode, offset, "branch condition that is not a Boolean scalar");

	/* Notes the innermost loop's header, which a branch may go back to. */
	header = NO_VALUE;
	if (parser->loop_depth != 0U)
		header = parser->loops[parser->loop_depth - 1U].header;

	/* A header opens its construct before its edges are checked against it. */
	if (parser->pending_merge != NO_VALUE) {
		if (word[2] == header || word[3] == header)
			return i915_spirv_refuse(parser, opcode, offset, "selection header that branches back to its loop's header");
		if (parser->depth >= MAX_CONSTRUCT_DEPTH)
			return i915_spirv_refuse(parser, opcode, offset, "selection constructs nested too deep");
		construct = &parser->constructs[parser->depth];
		construct->merge = parser->pending_merge;
		construct->predicate = parser->predicate;
		construct->leaky = 0;
		construct->loop = 0;
		parser->depth++;
		parser->pending_merge = NO_VALUE;
	}

	/* Both targets the same: every channel takes the one edge, and the block ends. */
	if (word[2] == word[3]) {
		if (word[2] == header) {
			error = i915_spirv_loop_back_edge(parser, parser->predicate, opcode, offset);
		} else {
			error = i915_spirv_edge_add(parser, word[2], parser->predicate, opcode, offset);
		}
		if (error != 0)
			return error;
		parser->terminated = 1;

		/* Succeeded: the one edge is recorded. */
		return 0;
	}

	/* The channels of the block where the condition holds, and where it does not. */
	taken = i915_spirv_predicate_and(parser, parser->predicate, condition[0]);
	negated = i915_spirv_emit_value(parser, I915_IR_NOT, condition[0], 0U);
	other = i915_spirv_predicate_and(parser, parser->predicate, negated);

	/* The edge that leaves the loop is recorded before the back edge ends the pass. */
	if (word[2] == header) {
		error = i915_spirv_edge_add(parser, word[3], other, opcode, offset);
		if (error != 0)
			return error;
		error = i915_spirv_loop_back_edge(parser, taken, opcode, offset);
	} else if (word[3] == header) {
		error = i915_spirv_edge_add(parser, word[2], taken, opcode, offset);
		if (error != 0)
			return error;
		error = i915_spirv_loop_back_edge(parser, other, opcode, offset);
	} else {
		error = i915_spirv_edge_add(parser, word[2], taken, opcode, offset);
		if (error != 0)
			return error;
		error = i915_spirv_edge_add(parser, word[3], other, opcode, offset);
	}
	if (error != 0)
		return error;
	parser->terminated = 1;

	/* Succeeded: both edges are recorded. */
	return 0;
}

/*
 * Lowers OpKill: the block's channels are discarded.
 *
 * The discard does not end them (see i915_compile_kill()), so the constructs
 * around do not leak.
 */
static int
i915_spirv_lower_kill(
	struct i915_spirv_parser *parser,
	uint32_t opcode,
	uint32_t offset)
{
	uint32_t condition;

	/* Only a fragment shader discards. */
	if (parser->ir->stage != I915_STAGE_FRAGMENT)
		return i915_spirv_refuse(parser, opcode, offset, "OpKill outside a fragment shader");

	/* Discards where the block runs: everywhere when it always does. */
	condition = parser->predicate;
	if (condition == PREDICATE_ALWAYS)
		condition = i915_spirv_shared_constant(parser, &parser->true_value, I915_IR_BOOL, BOOL_TRUE_BITS);
	(void)i915_spirv_emit(parser, I915_IR_KILL, 0U, condition, 0U);

	/* The block ends here. */
	parser->terminated = 1;

	/* Succeeded: the discard is lowered. */
	return 0;
}

/*
 * Lowers OpReturn: the block's channels are done.  Inside a construct they
 * leave every open construct without reaching its merge block.  A return
 * inside a loop is refused: the loop's merge block runs for every channel
 * that entered it.
 */
static int
i915_spirv_lower_return(
	struct i915_spirv_parser *parser,
	uint32_t opcode,
	uint32_t offset)
{
	uint32_t index;

	/* A channel returning from a loop would still reach the loop's merge block. */
	if (parser->loop_depth != 0U)
		return i915_spirv_refuse(parser, opcode, offset, "OpReturn inside a loop");

	/* Every open construct loses the returning channels. */
	for (index = 0U; index < parser->depth; index++)
		parser->constructs[index].leaky = 1;

	/* The block ends here. */
	parser->terminated = 1;

	/* Succeeded: the block returns. */
	return 0;
}

/*
 * Records an edge from the current block; an edge to the merge block of an
 * outer construct leaves the constructs inside it without reaching their
 * merge blocks, and they leak, as does an edge to the innermost loop's
 * continue target for the constructs inside the loop.  `opcode` and
 * `offset` name the branch for a refusal.
 */
static int
i915_spirv_edge_add(
	struct i915_spirv_parser *parser,
	uint32_t target,
	uint32_t predicate,
	uint32_t opcode,
	uint32_t offset)
{
	struct i915_spirv_id *label;
	struct i915_spirv_loop *loop;
	uint32_t level;
	uint32_t inner;

	/* The edge list has room for two edges per body instruction. */
	if (parser->edge_count >= parser->edge_capacity)
		return EINVAL;

	/* The target must be an id of the module. */
	label = i915_spirv_id(parser, target);
	if (label == NULL)
		return EINVAL;

	/* A branch to a block already lowered that is not the loop's header is not structured. */
	if (label->kind == ID_LABEL)
		return i915_spirv_refuse(parser, opcode, offset, "branch back to an earlier block that is not the loop header");

	/* Finds the innermost construct the target is the merge of, if any. */
	for (level = parser->depth; level > 0U; level--) {
		if (parser->constructs[level - 1U].merge != target)
			continue;

		/* The constructs inside that one are left before their merge blocks. */
		for (inner = level; inner < parser->depth; inner++)
			parser->constructs[inner].leaky = 1;
		break;
	}

	/* A continue leaves the constructs inside its loop before their merge blocks. */
	if (parser->loop_depth != 0U) {
		loop = &parser->loops[parser->loop_depth - 1U];
		if (loop->continue_target == target) {
			for (inner = loop->construct + 1U; inner < parser->depth; inner++)
				parser->constructs[inner].leaky = 1;
		}
	}

	/* Records the edge. */
	parser->edges[parser->edge_count].from = parser->block;
	parser->edges[parser->edge_count].to = target;
	parser->edges[parser->edge_count].predicate = predicate;
	parser->edge_count++;

	/* Succeeded: the edge is recorded. */
	return 0;
}

/* Returns the predicate of the edge from one block to another; `found` says whether there is one. */
static uint32_t
i915_spirv_edge_predicate(
	struct i915_spirv_parser *parser,
	uint32_t from,
	uint32_t to,
	int *found)
{
	uint32_t index;

	/* Looks the edge up. */
	for (index = 0U; index < parser->edge_count; index++) {
		if (parser->edges[index].from == from && parser->edges[index].to == to) {
			/* Succeeded: the channels that take the edge. */
			*found = 1;
			return parser->edges[index].predicate;
		}
	}

	/* No such edge. */
	*found = 0;
	return NO_VALUE;
}

/* Returns the channels of a predicate where a condition also holds. */
static uint32_t
i915_spirv_predicate_and(
	struct i915_spirv_parser *parser,
	uint32_t predicate,
	uint32_t condition)
{
	uint32_t both;

	/* Under no predicate the condition alone decides. */
	if (predicate == PREDICATE_ALWAYS)
		return condition;

	/* Both must hold. */
	both = i915_spirv_emit_value(parser, I915_IR_AND, predicate, condition);

	/* Succeeded: the channels where both hold. */
	return both;
}

/* Returns a value that is `value` where the block's predicate holds and `previous` elsewhere. */
static uint32_t
i915_spirv_predicated_value(
	struct i915_spirv_parser *parser,
	uint32_t value,
	uint32_t previous)
{
	uint32_t merged;

	/* A block every channel runs replaces the value outright. */
	if (parser->predicate == PREDICATE_ALWAYS)
		return value;

	/* Selects channel by channel. */
	merged = i915_spirv_select_value(parser, parser->predicate, value, previous);

	/* Succeeded: the merged value. */
	return merged;
}

/*
 * Returns the IR value of a constant the lowering itself introduces (0.0,
 * 1.0 or true), emitting it the first time; `slot` remembers it.  A constant
 * first emitted inside a loop is emitted again after the loop (see
 * i915_spirv_loop_close()), so every later reader comes after a definition.
 */
static uint32_t
i915_spirv_shared_constant(
	struct i915_spirv_parser *parser,
	uint32_t *slot,
	enum i915_shader_ir_op op,
	uint32_t bits)
{
	struct i915_shader_ir_inst *inst;

	/* A constant already emitted is shared. */
	if (*slot != NO_VALUE)
		return *slot;

	/* Emits the constant. */
	*slot = i915_spirv_new_value(parser);
	inst = i915_spirv_emit(parser, op, *slot, 0U, 0U);
	if (inst != NULL)
		inst->immediate = bits;

	/* Succeeded: the constant's value. */
	return *slot;
}

/* Emits a float constant of the given bits and returns its value. */
static uint32_t
i915_spirv_float_constant(
	struct i915_spirv_parser *parser,
	uint32_t bits)
{
	struct i915_shader_ir_inst *inst;
	uint32_t value;

	/* Emits the constant into a fresh value. */
	value = i915_spirv_new_value(parser);
	inst = i915_spirv_emit(parser, I915_IR_CONST, value, 0U, 0U);
	if (inst != NULL)
		inst->immediate = bits;

	/* Succeeded: the constant's value. */
	return value;
}

/* Emits a 32-bit integer constant and returns its value. */
static uint32_t
i915_spirv_integer_constant(
	struct i915_spirv_parser *parser,
	uint32_t bits)
{
	struct i915_shader_ir_inst *inst;
	uint32_t value;

	/* Emits the constant into a fresh value. */
	value = i915_spirv_new_value(parser);
	inst = i915_spirv_emit(parser, I915_IR_ICONST, value, 0U, 0U);
	if (inst != NULL)
		inst->immediate = bits;

	/* Succeeded: the constant's value. */
	return value;
}

/* Emits a selection of `taken` where the Boolean `condition` holds and `other` elsewhere; returns it. */
static uint32_t
i915_spirv_select_value(
	struct i915_spirv_parser *parser,
	uint32_t condition,
	uint32_t taken,
	uint32_t other)
{
	struct i915_shader_ir_inst *inst;
	uint32_t merged;

	/* The selection reads three sources; the third is set after the emit. */
	merged = i915_spirv_new_value(parser);
	inst = i915_spirv_emit(parser, I915_IR_SELECT, merged, condition, taken);
	if (inst != NULL)
		inst->src[2] = other;

	/* Succeeded: the selected value. */
	return merged;
}

/*
 * Emits a MOVE of `source` into `destination` -- a fresh value when it is
 * NO_VALUE -- and returns the destination.
 */
static uint32_t
i915_spirv_move_value(
	struct i915_spirv_parser *parser,
	uint32_t destination,
	uint32_t source)
{
	/* A move into a new loop variable numbers it first. */
	if (destination == NO_VALUE)
		destination = i915_spirv_new_value(parser);

	/* Moves the bits. */
	(void)i915_spirv_emit(parser, I915_IR_MOVE, destination, source, 0U);

	/* Succeeded: the value moved into. */
	return destination;
}

/* Records why the module is refused; the instruction is NOT skipped. */
static int
i915_spirv_refuse(
	struct i915_spirv_parser *parser,
	uint32_t opcode,
	uint32_t word_offset,
	const char *reason)
{
	/* Names the refused instruction for the caller's diagnostic. */
	parser->diag.opcode = opcode;
	parser->diag.word_offset = word_offset;
	parser->diag.reason = reason;

	/* Reports valid SPIR-V this parser does not lower. */
	return ENOTSUP;
}

/* Returns the id record, or NULL for an id outside the module's bound. */
static struct i915_spirv_id *
i915_spirv_id(
	struct i915_spirv_parser *parser,
	uint32_t id)
{
	/* An id at or past the bound does not exist. */
	if (id >= parser->bound)
		return NULL;

	/* Succeeded: the id has a record. */
	return &parser->ids[id];
}

/*
 * Returns the component count of a scalar or vector type whose scalars are
 * of `scalar` kind (32-bit floats, 32-bit integers or Booleans); 0 for
 * anything else.
 */
static uint32_t
i915_spirv_kind_components(
	struct i915_spirv_parser *parser,
	uint32_t type_id,
	uint32_t scalar)
{
	struct i915_spirv_id *type;
	uint32_t count;

	/* Resolves the type; a vector counts its elements, a scalar one. */
	type = i915_spirv_id(parser, type_id);
	if (type == NULL)
		return 0U;
	count = 1U;
	if (type->kind == ID_TYPE_VECTOR) {
		if (type->count < 2U || type->count > 4U)
			return 0U;
		count = type->count;
		type = i915_spirv_id(parser, type->type);
		if (type == NULL)
			return 0U;
	}

	/* The scalar must be of the kind asked for, and 32 bits wide unless a Boolean. */
	if (scalar == SCALAR_FLOAT && type->kind == ID_TYPE_FLOAT && type->width == 32U)
		return count;
	if (scalar == SCALAR_INT && type->kind == ID_TYPE_INT && type->width == 32U)
		return count;
	if (scalar == SCALAR_BOOL && type->kind == ID_TYPE_BOOL)
		return count;

	/* Succeeded: another kind of type counts nothing. */
	return 0U;
}

/* Returns the component count of a float scalar or float vector type; 0 for anything else. */
static uint32_t
i915_spirv_float_components(
	struct i915_spirv_parser *parser,
	uint32_t type_id)
{
	uint32_t components;

	/* Counts the floats. */
	components = i915_spirv_kind_components(parser, type_id, SCALAR_FLOAT);

	/* Succeeded: the float components, or none. */
	return components;
}

/* Returns the component count of an integer scalar or integer vector type; 0 for anything else. */
static uint32_t
i915_spirv_int_components(
	struct i915_spirv_parser *parser,
	uint32_t type_id)
{
	uint32_t components;

	/* Counts the integers. */
	components = i915_spirv_kind_components(parser, type_id, SCALAR_INT);

	/* Succeeded: the integer components, or none. */
	return components;
}

/* Returns the component count of a Boolean scalar or Boolean vector type; 0 for anything else. */
static uint32_t
i915_spirv_bool_components(
	struct i915_spirv_parser *parser,
	uint32_t type_id)
{
	uint32_t components;

	/* Counts the Booleans. */
	components = i915_spirv_kind_components(parser, type_id, SCALAR_BOOL);

	/* Succeeded: the Boolean components, or none. */
	return components;
}

/* Returns the component count of a float, integer or Boolean scalar or vector type; 0 for anything else. */
static uint32_t
i915_spirv_value_components(
	struct i915_spirv_parser *parser,
	uint32_t type_id)
{
	uint32_t components;

	/* A float type counts its floats. */
	components = i915_spirv_float_components(parser, type_id);
	if (components != 0U)
		return components;

	/* An integer type counts its integers. */
	components = i915_spirv_int_components(parser, type_id);
	if (components != 0U)
		return components;

	/* Succeeded: a Boolean type counts its Booleans, anything else nothing. */
	components = i915_spirv_bool_components(parser, type_id);
	return components;
}

/* Returns the scalar count of a float matrix type, columns times rows; 0 for anything else. */
static uint32_t
i915_spirv_matrix_components(
	struct i915_spirv_parser *parser,
	uint32_t type_id)
{
	struct i915_spirv_id *type;
	uint32_t rows;

	/* Resolves the type, which must be a matrix. */
	type = i915_spirv_id(parser, type_id);
	if (type == NULL || type->kind != ID_TYPE_MATRIX)
		return 0U;

	/* Its columns must be float vectors. */
	rows = i915_spirv_float_components(parser, type->type);
	if (rows < 2U)
		return 0U;

	/* Succeeded: every scalar of every column. */
	return type->count * rows;
}

/* Returns the scalar count of a float scalar, vector or matrix type; 0 for anything else. */
static uint32_t
i915_spirv_float_components_wide(
	struct i915_spirv_parser *parser,
	uint32_t type_id)
{
	uint32_t components;

	/* A matrix counts its scalars. */
	components = i915_spirv_matrix_components(parser, type_id);
	if (components != 0U)
		return components;

	/* Succeeded: a float scalar or vector counts its components. */
	components = i915_spirv_float_components(parser, type_id);
	return components;
}

/* Returns the scalar count of a scalar, vector or float matrix type; 0 for anything else. */
static uint32_t
i915_spirv_value_components_wide(
	struct i915_spirv_parser *parser,
	uint32_t type_id)
{
	uint32_t components;

	/* A matrix counts its scalars. */
	components = i915_spirv_matrix_components(parser, type_id);
	if (components != 0U)
		return components;

	/* Succeeded: a scalar or a vector counts its components. */
	components = i915_spirv_value_components(parser, type_id);
	return components;
}

/* Returns the type id of a value or constant operand; 0 for any other id. */
static uint32_t
i915_spirv_operand_type(
	struct i915_spirv_parser *parser,
	uint32_t id)
{
	struct i915_spirv_id *record;

	/* Resolves the operand. */
	record = i915_spirv_id(parser, id);
	if (record == NULL)
		return 0U;

	/* Only a value or a constant carries a type here. */
	if (record->kind != ID_VALUE &&
	    record->kind != ID_CONSTANT &&
	    record->kind != ID_CONSTANT_COMPOSITE)
		return 0U;

	/* Succeeded: the operand's type. */
	return record->type;
}

/* Returns the integer component count of an operand's type; 0 when it is not an integer scalar or vector. */
static uint32_t
i915_spirv_operand_int_components(
	struct i915_spirv_parser *parser,
	uint32_t id)
{
	uint32_t type;
	uint32_t components;

	/* Counts the integers of the operand's type. */
	type = i915_spirv_operand_type(parser, id);
	components = i915_spirv_int_components(parser, type);

	/* Succeeded: the integer components, or none. */
	return components;
}

/* Returns the float component count of an operand's type; 0 when it is not a float scalar or vector. */
static uint32_t
i915_spirv_operand_float_components(
	struct i915_spirv_parser *parser,
	uint32_t id)
{
	uint32_t type;
	uint32_t components;

	/* Counts the floats of the operand's type. */
	type = i915_spirv_operand_type(parser, id);
	components = i915_spirv_float_components(parser, type);

	/* Succeeded: the float components, or none. */
	return components;
}

/*
 * Reports the shape of an operand: the columns and rows of a matrix, one
 * column of a vector's components, one of one for a scalar; zero columns
 * for anything else.
 */
static void
i915_spirv_operand_shape(
	struct i915_spirv_parser *parser,
	uint32_t id,
	uint32_t *columns,
	uint32_t *rows)
{
	struct i915_spirv_id *type;
	uint32_t type_id;
	uint32_t components;

	/* Starts with no shape. */
	*columns = 0U;
	*rows = 0U;

	/* Resolves the operand's type. */
	type_id = i915_spirv_operand_type(parser, id);
	type = i915_spirv_id(parser, type_id);
	if (type == NULL)
		return;

	/* A matrix has its columns of its column type's rows. */
	if (type->kind == ID_TYPE_MATRIX) {
		*columns = type->count;
		*rows = i915_spirv_float_components(parser, type->type);
		return;
	}

	/* A scalar or a vector is one column. */
	components = i915_spirv_value_components(parser, type_id);
	if (components == 0U)
		return;
	*columns = 1U;
	*rows = components;
}

/* Appends one IR instruction defining a fresh value and returns that value. */
static uint32_t
i915_spirv_emit_value(
	struct i915_spirv_parser *parser,
	enum i915_shader_ir_op op,
	uint32_t source0,
	uint32_t source1)
{
	uint32_t value;

	/* Numbers the value, then defines it; a failure is latched by the emit. */
	value = i915_spirv_new_value(parser);
	(void)i915_spirv_emit(parser, op, value, source0, source1);

	/* Succeeded: the value the instruction defines. */
	return value;
}

/* Appends one IR instruction and returns it, or NULL (with the error latched). */
static struct i915_shader_ir_inst *
i915_spirv_emit(
	struct i915_spirv_parser *parser,
	enum i915_shader_ir_op op,
	uint32_t dst,
	uint32_t source0,
	uint32_t source1)
{
	struct i915_shader_ir_inst *inst;
	struct i915_shader_ir_inst *grown;
	uint32_t capacity;

	/* A full stream doubles; failing to grow is never a silently shorter shader. */
	if (parser->ir->instruction_count >= parser->capacity) {
		/* Refuses a stream that cannot double any more. */
		if (parser->capacity > 0x7FFFFFFFU / 2U) {
			parser->error = EINVAL;
			return NULL;
		}

		/* Allocates the larger stream. */
		capacity = parser->capacity * 2U;
		grown = kern_calloc(capacity, sizeof(*grown));
		if (grown == NULL) {
			parser->error = ENOMEM;
			return NULL;
		}

		/* Moves the instructions so far into it and publishes it. */
		kern_memcpy(grown, parser->ir->instructions, parser->ir->instruction_count * sizeof(*grown));
		kern_free(parser->ir->instructions);
		parser->ir->instructions = grown;
		parser->capacity = capacity;
	}

	/* Takes the next slot. */
	inst = &parser->ir->instructions[parser->ir->instruction_count];
	parser->ir->instruction_count++;

	/* Fills the instruction; the caller sets the operation-specific fields. */
	kern_memset(inst, 0, sizeof(*inst));
	inst->op = op;
	inst->dst = dst;
	inst->src[0] = source0;
	inst->src[1] = source1;

	/* Succeeded: the instruction is part of the stream. */
	return inst;
}

/* Returns a fresh IR scalar value. */
static uint32_t
i915_spirv_new_value(
	struct i915_spirv_parser *parser)
{
	uint32_t value;

	/* Values are numbered in order of definition. */
	value = parser->ir->value_count;
	parser->ir->value_count++;

	/* Succeeded: the value is defined by the caller's next instruction. */
	return value;
}

/*
 * Returns the IR scalars of a scalar or vector operand, at most four: see
 * i915_spirv_operand_wide().  Returns 0 for a matrix.
 */
static uint32_t
i915_spirv_operand(
	struct i915_spirv_parser *parser,
	uint32_t id,
	uint32_t comp[4])
{
	uint32_t wide[MAX_COMPONENTS];
	uint32_t components;
	uint32_t index;

	/* Resolves the operand's scalars. */
	components = i915_spirv_operand_wide(parser, id, wide);

	/* More than four is a matrix, which this operand is not. */
	if (components > 4U)
		return 0U;

	/* Copies the scalars. */
	for (index = 0U; index < components; index++)
		comp[index] = wide[index];

	/* Succeeded: the operand has this many components. */
	return components;
}

/*
 * Returns the IR scalars of a float, integer or Boolean operand: a value id,
 * a scalar constant (which becomes a CONST, ICONST or BOOL instruction the
 * first time it is used), a vector constant or a matrix constant.  Returns
 * the component count, 0 if the id is none of these, or names a value made
 * inside a loop that has ended (which only a loop variable carries out).
 */
static uint32_t
i915_spirv_operand_wide(
	struct i915_spirv_parser *parser,
	uint32_t id,
	uint32_t comp[MAX_COMPONENTS])
{
	struct i915_spirv_id *record;
	struct i915_spirv_id *constituent;
	uint32_t constituent_comp[MAX_COMPONENTS];
	uint32_t components;
	uint32_t filled;
	uint32_t index;
	uint32_t component;
	int escaped;

	/* Resolves the operand. */
	record = i915_spirv_id(parser, id);
	if (record == NULL)
		return 0U;

	/* A scalar constant is one scalar, materialized on its first use. */
	if (record->kind == ID_CONSTANT) {
		components = i915_spirv_constant_operand(parser, record, comp);
		return components;
	}

	/* A composite constant is its constituents, each materialized on its first use. */
	if (record->kind == ID_CONSTANT_COMPOSITE) {
		filled = 0U;
		for (index = 0U; index < record->count; index++) {
			constituent = i915_spirv_id(parser, record->member_type[index]);
			if (constituent == NULL)
				return 0U;
			components = i915_spirv_operand_wide(parser, record->member_type[index], constituent_comp);
			if (components == 0U || filled + components > MAX_COMPONENTS)
				return 0U;
			for (component = 0U; component < components; component++)
				comp[filled + component] = constituent_comp[component];
			filled += components;
		}

		/* Succeeded: the composite constant has this many scalars. */
		return filled;
	}

	/* Anything else must be a value. */
	if (record->kind != ID_VALUE)
		return 0U;

	/* Refuses a value made inside a loop that has ended. */
	for (index = 0U; index < record->count; index++) {
		escaped = i915_spirv_escaped(parser, record->comp[index]);
		if (escaped != 0)
			return 0U;
		comp[index] = record->comp[index];
	}

	/* Succeeded: the operand has this many components. */
	return record->count;
}

/* Reports whether an IR value was made inside a loop that has ended. */
static int
i915_spirv_escaped(
	struct i915_spirv_parser *parser,
	uint32_t value)
{
	uint32_t index;

	/* Looks for a closed loop whose value range holds it. */
	for (index = 0U; index < parser->closed_count; index++) {
		if (value >= parser->closed[index].first && value < parser->closed[index].end)
			return 1;
	}

	/* Succeeded: the value may be read here. */
	return 0;
}

/*
 * Returns the IR scalar of a float, integer or Boolean scalar constant,
 * emitting it the first time it is used; later uses share its value.
 * Returns 1, or 0 for any other constant.
 */
static uint32_t
i915_spirv_constant_operand(
	struct i915_spirv_parser *parser,
	struct i915_spirv_id *record,
	uint32_t comp[MAX_COMPONENTS])
{
	struct i915_shader_ir_inst *inst;
	enum i915_shader_ir_op op;
	uint32_t floats;
	uint32_t integers;
	uint32_t booleans;

	/* A float constant is a CONST, an integer one an ICONST, a Boolean one a BOOL. */
	floats = i915_spirv_float_components(parser, record->type);
	integers = i915_spirv_int_components(parser, record->type);
	booleans = i915_spirv_bool_components(parser, record->type);
	if (floats == 1U) {
		op = I915_IR_CONST;
	} else if (integers == 1U) {
		op = I915_IR_ICONST;
	} else if (booleans == 1U) {
		op = I915_IR_BOOL;
	} else {
		return 0U;
	}

	/* The first use emits the constant; later uses share its value. */
	if (record->comp[0] == NO_VALUE || record->count == 0U) {
		record->comp[0] = i915_spirv_new_value(parser);
		record->count = 1U;
		inst = i915_spirv_emit(parser, op, record->comp[0], 0U, 0U);
		if (inst != NULL)
			inst->immediate = record->constant;
	}

	/* Succeeded: the constant's one scalar. */
	comp[0] = record->comp[0];
	return 1U;
}

/*
 * Declares result id `id` as a value of `count` scalars (at most sixteen):
 * fresh values when `fresh` is nonzero, otherwise left for the caller to
 * name.
 */
static struct i915_spirv_id *
i915_spirv_result(
	struct i915_spirv_parser *parser,
	uint32_t id,
	uint32_t type_id,
	uint32_t count,
	int fresh)
{
	struct i915_spirv_id *record;
	uint32_t index;

	/* SSA: a result id is defined once, with no more scalars than a value holds. */
	record = i915_spirv_id(parser, id);
	if (record == NULL || record->kind != ID_NONE)
		return NULL;
	if (count > MAX_COMPONENTS)
		return NULL;

	/* Records the value's type and size. */
	record->kind = ID_VALUE;
	record->type = type_id;
	record->count = (uint8_t)count;

	/* Numbers each component in order, or leaves it unnamed. */
	for (index = 0U; index < MAX_COMPONENTS; index++) {
		if (index < count && fresh != 0) {
			record->comp[index] = i915_spirv_new_value(parser);
		} else {
			record->comp[index] = NO_VALUE;
		}
	}

	/* Succeeded: the result is declared. */
	return record;
}
