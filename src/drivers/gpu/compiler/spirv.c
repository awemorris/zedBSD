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
 * a discard discards only where its predicate is true.  An OpSwitch with a
 * 32-bit selector is a set of edges the same way: each case's edge is taken
 * where the selector equals one of its literals, the default's where it
 * equals none (the construct spirv-opt's merge-return pass wraps a function
 * body in is an OpSwitch with the default target alone).
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

#include "spirv.h"
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
#define OP_TYPE_RUNTIME_ARRAY 29U
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
#define OP_COMPOSITE_INSERT 82U
#define OP_COPY_OBJECT 83U
#define OP_TRANSPOSE 84U
#define OP_IMAGE_SAMPLE_IMPLICIT_LOD 87U
#define OP_IMAGE_SAMPLE_EXPLICIT_LOD 88U
#define OP_IMAGE_SAMPLE_DREF_IMPLICIT_LOD 89U
#define OP_IMAGE_SAMPLE_DREF_EXPLICIT_LOD 90U
#define OP_IMAGE_SAMPLE_PROJ_IMPLICIT_LOD 91U
#define OP_IMAGE_SAMPLE_PROJ_EXPLICIT_LOD 92U
#define OP_IMAGE_SAMPLE_PROJ_DREF_IMPLICIT_LOD 93U
#define OP_IMAGE_SAMPLE_PROJ_DREF_EXPLICIT_LOD 94U
#define OP_IMAGE_FETCH 95U
#define OP_IMAGE 100U
#define OP_IMAGE_QUERY_SIZE_LOD 103U
#define OP_IMAGE_QUERY_SIZE 104U
#define OP_IMAGE_QUERY_LEVELS 106U
#define OP_CONVERT_F_TO_U 109U
#define OP_CONVERT_F_TO_S 110U
#define OP_CONVERT_S_TO_F 111U
#define OP_CONVERT_U_TO_F 112U
#define OP_BITCAST 124U
#define OP_U_CONVERT 113U
#define OP_S_CONVERT 114U
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
#define OP_DPDX 207U
#define OP_DPDY 208U
#define OP_FWIDTH 209U
#define OP_DPDX_FINE 210U
#define OP_DPDY_FINE 211U
#define OP_FWIDTH_FINE 212U
#define OP_DPDX_COARSE 213U
#define OP_DPDY_COARSE 214U
#define OP_FWIDTH_COARSE 215U
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

/* Opcodes of compute shaders (ws101-p002, spirv-compute.inc). */
#define OP_ARRAY_LENGTH 68U
#define OP_CONTROL_BARRIER 224U
#define OP_MEMORY_BARRIER 225U
#define OP_ATOMIC_LOAD 227U
#define OP_ATOMIC_STORE 228U
#define OP_ATOMIC_EXCHANGE 229U
#define OP_ATOMIC_COMPARE_EXCHANGE 230U
#define OP_ATOMIC_I_INCREMENT 232U
#define OP_ATOMIC_I_DECREMENT 233U
#define OP_ATOMIC_I_ADD 234U
#define OP_ATOMIC_I_SUB 235U
#define OP_ATOMIC_S_MIN 236U
#define OP_ATOMIC_U_MIN 237U
#define OP_ATOMIC_S_MAX 238U
#define OP_ATOMIC_U_MAX 239U
#define OP_ATOMIC_AND 240U
#define OP_ATOMIC_OR 241U
#define OP_ATOMIC_XOR 242U
#define OP_EXECUTION_MODE_ID 331U

/* Opcodes of geometry shaders (ws075-p007a, spirv-geometry.inc). */
#define OP_EMIT_VERTEX 218U
#define OP_END_PRIMITIVE 219U

/* Storage classes (SPIR-V spec, section 3.7). */
#define SC_UNIFORM_CONSTANT 0U
#define SC_INPUT 1U
#define SC_UNIFORM 2U
#define SC_OUTPUT 3U
#define SC_FUNCTION 7U
#define SC_PUSH_CONSTANT 9U
#define SC_STORAGE_BUFFER 12U
#define SC_WORKGROUP 4U

/* Decorations (SPIR-V spec, section 3.20). */
#define DEC_RELAXED_PRECISION 0U
#define DEC_BLOCK 2U
#define DEC_BUFFER_BLOCK 3U
#define DEC_ROW_MAJOR 4U
#define DEC_COL_MAJOR 5U
#define DEC_ARRAY_STRIDE 6U
#define DEC_MATRIX_STRIDE 7U
#define DEC_BUILTIN 11U
#define DEC_NO_PERSPECTIVE 13U
#define DEC_FLAT 14U
#define DEC_CENTROID 16U
#define DEC_LOCATION 30U
#define DEC_INDEX 32U
#define DEC_BINDING 33U
#define DEC_DESCRIPTOR_SET 34U
#define DEC_OFFSET 35U
#define DEC_NON_WRITABLE 24U
#define DEC_NON_READABLE 25U

/*
 * Decorations of a storage buffer's accesses that change nothing the lowering
 * does (ws101-p002).
 */
#define DEC_RESTRICT 19U
#define DEC_ALIASED 20U
#define DEC_VOLATILE 21U
#define DEC_COHERENT 23U

/* BuiltIn values (SPIR-V spec, section 3.21). */
#define BUILTIN_POSITION 0U
#define BUILTIN_POINT_SIZE 1U
#define BUILTIN_FRAG_COORD 15U
#define BUILTIN_POINT_COORD 16U
#define BUILTIN_FRONT_FACING 17U
#define BUILTIN_VERTEX_INDEX 42U
#define BUILTIN_INSTANCE_INDEX 43U

/* The built-in inputs of a compute shader (ws101-p002). */
#define BUILTIN_NUM_WORKGROUPS 24U
#define BUILTIN_WORKGROUP_ID 26U
#define BUILTIN_LOCAL_INVOCATION_ID 27U
#define BUILTIN_GLOBAL_INVOCATION_ID 28U
#define BUILTIN_LOCAL_INVOCATION_INDEX 29U

/* The built-ins of a geometry shader (ws075-p007a). */
#define BUILTIN_PRIMITIVE_ID 7U
#define BUILTIN_INVOCATION_ID 8U
#define BUILTIN_LAYER 9U
#define BUILTIN_VIEWPORT_INDEX 10U

/* Execution models (SPIR-V spec, section 3.3). */
#define EM_VERTEX 0U
#define EM_GEOMETRY 3U
#define EM_FRAGMENT 4U
#define EM_GL_COMPUTE 5U

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
#define GLSL_DETERMINANT 33U
#define GLSL_MATRIX_INVERSE 34U
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
#define GLSL_PACK_HALF_2X16 58U
#define GLSL_UNPACK_HALF_2X16 62U
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

/*
 * The deepest nesting of loops the parser follows, and the most loops of a
 * function.
 */
#define MAX_LOOP_DEPTH 8U
#define MAX_LOOPS 64U

/* The most phis a loop header may have. */
#define MAX_LOOP_PHIS 32U

/* The most members a structure type may have. */
#define MAX_MEMBERS 16U

/* The most scalars a value may have: a 4 x 4 matrix. */
#define MAX_COMPONENTS 16U

/*
 * The most elements an array of a block read through a dynamic index may have.
 */
#define MAX_DYNAMIC_ELEMENTS 16U

/* The most parts a dynamic index into a local may choose among. */
#define MAX_LOCAL_DYNAMIC 64U

/*
 * The most scalars a local variable holds, and the most interface slots (four
 * to a location) an output holds: what the parser keeps stored for them.
 */
#define MAX_VARIABLE_SLOTS 256U

/* The deepest nesting of arrays and structures a type may have. */
#define MAX_TYPE_DEPTH 8U

/*
 * The interface locations a shader's own inputs and outputs may use: below the
 * generated ones.
 */
#define MAX_USER_LOCATION DRV_GPU_SHADER_LOCATION_VERTEX_INDEX

/* The OpVectorShuffle component index that means "undefined". */
#define SHUFFLE_UNDEFINED 0xFFFFFFFFU

/*
 * The IR instructions the stream starts with room for, per SPIR-V
 * instruction of the body: a four-component dot product is four multiplies
 * and three adds.  A longer lowering (a normalize, a mix, a phi of many
 * edges, a matrix product) grows the stream.
 */
#define IR_PER_INSTRUCTION 8U

/*
 * The image operands of a sample (Khronos SPIR-V spec, Image Operands) that are
 * lowered.
 */
#define IMAGE_OPERAND_BIAS 0x1U
#define IMAGE_OPERAND_LOD 0x2U
#define IMAGE_OPERAND_GRAD 0x4U
#define IMAGE_OPERAND_CONST_OFFSET 0x8U
#define IMAGE_OPERAND_SAMPLE 0x40U

/* The Dim of OpTypeImage. */
#define DIM_1D 0U
#define DIM_2D 1U
#define DIM_3D 2U
#define DIM_CUBE 3U
#define DIM_BUFFER 5U

/* Pointer target kinds. */
#define PTR_NONE 0U
#define PTR_INPUT 1U
#define PTR_OUTPUT 2U
#define PTR_OUTPUT_BLOCK 3U /* gl_PerVertex: the members carry the builtins */
#define PTR_PUSH 4U
#define PTR_SAMPLER 5U
#define PTR_LOCAL 6U
#define PTR_UBO 7U
/* A storage buffer addressed at shader-computed byte offsets. */
#define PTR_SSBO 8U

/* A compute built-in supplied by the dispatch parameters. */
#define PTR_SYSTEM 9U

/* A workgroup variable stored in the group's shared memory. */
#define PTR_SHARED 10U

/* A geometry input whose array elements identify primitive vertices. */
#define PTR_VERTEX 11U

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
enum drv_gpu_spirv_id_kind {
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

	/*
	 * A vector or matrix constant: member_type[] names the constituent
	 * constants.
	 */
	ID_CONSTANT_COMPOSITE,

	/*
	 * A block label: comp[0] is the predicate of the block once it is
	 * reached.
	 */
	ID_LABEL,

	/* An OpVariable. */
	ID_VARIABLE,

	/* An OpAccessChain result. */
	ID_POINTER,

	/*
	 * A float, integer or Boolean scalar, vector or matrix value: comp[]
	 * names the IR scalars.
	 */
	ID_VALUE,

	/* A loaded combined image sampler. */
	ID_SAMPLED_IMAGE,

	/* Names the imported extended instruction set. */
	ID_EXT_SET
};

/*
 * What the parser knows about one SPIR-V id.
 *
 * The parser holds one per id below the module's bound for the length of one
 * parse; the record of a local variable also carries the scalars currently
 * stored in it.
 */
struct drv_gpu_spirv_id {
	/* An enum drv_gpu_spirv_id_kind. */
	uint8_t kind;

	/* Int or float bit width. */
	uint8_t width;

	/*
	 * Vector component count; matrix column count; struct member count;
	 * value component count.
	 */
	uint8_t count;

	uint8_t ptr_kind;
	uint8_t has_location;
	uint8_t has_binding;
	uint8_t has_set;
	uint8_t has_builtin;

	/*
	 * A variable decorated Flat: a fragment input the draw sets up as its
	 * provoking vertex's value.
	 */
	uint8_t flat;

	/*
	 * A variable decorated NoPerspective: a fragment input interpolated
	 * linearly in screen space.
	 */
	uint8_t noperspective;

	/*
	 * An image type: its Dim, whether it is Arrayed and whether it is
	 * multisampled (MS).
	 */
	uint8_t image_dim;
	uint8_t image_arrayed;
	uint8_t image_ms;

	/* A structure type decorated BufferBlock: a storage buffer's block. */
	uint8_t buffer_block;

	/*
	 * A fragment output's Index decoration: 1 is the second colour of a
	 * dual-source blend (ws031-p032).
	 */
	uint8_t index;

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

	/*
	 * Array types: the element count and the ArrayStride in bytes (0 when
	 * undecorated).
	 */
	uint32_t length;
	uint32_t stride;

	/* Uniform blocks: the index of the block in the IR's uniform list. */
	uint32_t uniform;

	/* Structure types: member types and offsets. */
	uint32_t member_type[MAX_MEMBERS];
	uint32_t member_offset[MAX_MEMBERS];

	/*
	 * The member's builtin plus one; zero when the member is not a builtin.
	 */
	uint32_t member_builtin[MAX_MEMBERS];

	/* A matrix member's MatrixStride, and whether it is RowMajor. */
	uint32_t member_matrix_stride[MAX_MEMBERS];
	uint8_t member_row_major[MAX_MEMBERS];

	uint8_t member_has_offset[MAX_MEMBERS];

	/*
	 * An interface block member decorated Flat, or NoPerspective (see
	 * `flat`).
	 */
	uint8_t member_flat[MAX_MEMBERS];
	uint8_t member_noperspective[MAX_MEMBERS];

	/*
	 * The scalars of a value (a label's predicate, a constant's first use).
	 */
	uint32_t comp[MAX_COMPONENTS];

	/*
	 * A local variable or an output: the run of the parser's slots that
	 * holds what is currently stored in it -- a local's scalars in order,
	 * an output's four slots to a location (see
	 * drv_gpu_spirv_variable_slot()).
	 */
	uint32_t first_slot;
	uint32_t slot_count;

	/*
	 * Pointers (variables and access chains): the OpVariable the pointer
	 * leads to.
	 */
	uint32_t var;

	/* The type id of what the pointer addresses now. */
	uint32_t pointee;

	/* The struct member selected, or -1. */
	int32_t member;

	/*
	 * A pointer into a local, an input or an output: the first scalar it
	 * addresses, or -1 for 0.
	 */
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

	/*
	 * A pointer into a geometry shader's per-vertex input (ws075-p007a):
	 * nonzero once the chain's first index chose the input vertex; the
	 * vertex's number when that index is a constant; and the IR integer of
	 * the number when it is chosen at run time (NO_VALUE for a constant).
	 */
	uint8_t vertex_chosen;
	uint32_t vertex;
	uint32_t vertex_index;
};

/*
 * One control-flow edge of the body, from a block's terminator to a block.
 *
 * `predicate` is the IR Boolean of the channels that take the edge, or
 * PREDICATE_ALWAYS.  The parser keeps every edge until the parse ends: the
 * target's predicate and its phis are built from them.
 */
struct drv_gpu_spirv_edge {
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
 * the loop (`loop` nonzero; see struct drv_gpu_spirv_loop).
 */
struct drv_gpu_spirv_construct {
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
struct drv_gpu_spirv_loop {
	/* The header, merge and continue target labels. */
	uint32_t header;
	uint32_t merge;
	uint32_t continue_target;

	/* The channels that entered the loop: the merge block's predicate. */
	uint32_t entry_predicate;

	/*
	 * The loop variable of the channels still in the loop: the header's
	 * predicate.
	 */
	uint32_t active;

	/* The construct stack depth of the loop's own construct entry. */
	uint32_t construct;

	/*
	 * The first IR value made inside the body (the value count at
	 * LOOP_BEGIN).
	 */
	uint32_t first_value;

	/*
	 * Nonzero once LOOP_BEGIN is emitted, and once the back edge closed the
	 * body.
	 */
	int begun;
	int closed;

	/*
	 * The header phis: the result id and the value id the back edge brings.
	 */
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
struct drv_gpu_spirv_carried {
	uint32_t variable;
	uint32_t component;
	uint32_t value;
};

/*
 * The IR value range of a loop that has ended: [first, end) were made
 * inside it.
 */
struct drv_gpu_spirv_range {
	uint32_t first;
	uint32_t end;
};

/*
 * The decode state threaded through both passes of one parse.
 *
 * It is heap-owned by drv_gpu_shader_parse() and owns the id table,
 * the edge list, the carried list and the construct and loop stacks until
 * the parse ends.
 */
struct drv_gpu_spirv_parser {
	const uint32_t *code;
	uint32_t words;
	uint32_t bound;
	struct drv_gpu_spirv_id *ids;
	struct drv_gpu_shader_ir *ir;

	/* Instruction slots of ir->instructions. */
	uint32_t capacity;

	uint32_t body_instructions;

	/*
	 * The edges the OpSwitch instructions of the body can add beyond two
	 * each, to size the edge list.
	 */
	uint32_t switch_edges;

	/*
	 * A failure latched by an emitter that has no return value to carry it.
	 */
	int error;

	struct drv_gpu_compile_diagnostic diag;

	/*
	 * The block being lowered: its label (0 before the first), the IR
	 * Boolean of the channels that run it (or PREDICATE_ALWAYS), and
	 * whether its terminator has been seen.
	 */
	uint32_t block;
	uint32_t predicate;
	int terminated;

	/*
	 * Nonzero while the block is the merge block of a loop, where no phi is
	 * lowered.
	 */
	int loop_merge_block;

	/*
	 * The block now lowered is a skippable region (ws075-p023): a
	 * SKIP_BEGIN on its predicate was emitted at its label and its SKIP_END
	 * comes before its terminator; `skip_first_value` is the first value
	 * made inside it.
	 */
	int skip_open;
	uint32_t skip_first_value;

	/*
	 * The merge block an OpSelectionMerge named for the terminator after
	 * it, or NO_VALUE.
	 */
	uint32_t pending_merge;

	/* Every edge so far, with room for two per body instruction. */
	struct drv_gpu_spirv_edge *edges;
	uint32_t edge_count;
	uint32_t edge_capacity;

	/*
	 * The selection and loop constructs the walk is inside, innermost last.
	 */
	struct drv_gpu_spirv_construct constructs[MAX_CONSTRUCT_DEPTH];
	uint32_t depth;

	/* The loops the walk is inside, innermost last. */
	struct drv_gpu_spirv_loop loops[MAX_LOOP_DEPTH];
	uint32_t loop_depth;

	/* The components the open loops carry, the outer loops' first. */
	struct drv_gpu_spirv_carried *carried;
	uint32_t carried_count;
	uint32_t carried_capacity;

	/* The value ranges of the loops that have ended. */
	struct drv_gpu_spirv_range closed[MAX_LOOPS];
	uint32_t closed_count;

	/*
	 * The shared constants the lowering introduces: 0.0, 1.0 and true, and
	 * the integers 16 and 0xFFFF of a 16-bit value's extension; NO_VALUE
	 * until first needed.
	 */
	uint32_t zero_value;
	uint32_t one_value;
	uint32_t true_value;
	uint32_t int_16_value;
	uint32_t int_ffff_value;

	/*
	 * What the local variables and the outputs currently hold, one IR value
	 * (or NO_VALUE, nothing stored) to a slot; each variable owns a run of
	 * it from its declaration to the end of the parse.
	 */
	uint32_t *slots;
	uint32_t slot_count;
	uint32_t slot_capacity;

	/*
	 * The entries the interface lists (ir->inputs, ir->outputs) have room
	 * for.
	 */
	uint32_t io_capacity;

	/*
	 * Compute: the constants a LocalSizeId execution mode names, resolved
	 * after the declarations when local_size_by_id is nonzero, and the
	 * index plus one of the system storage buffer in the uniform list, zero
	 * until gl_NumWorkGroups is read (spirv-compute.inc).
	 */
	uint32_t local_size_id[3];
	int local_size_by_id;
	uint32_t system_uniform;

	/*
	 * Compute (ws101-p006): nonzero once an OpReturn was lowered, after
	 * which a workgroup barrier is refused (a thread whose channels all
	 * returned would not wait at it the same number of times).
	 */
	int returned;
};

static int drv_gpu_spirv_pass_declarations(struct drv_gpu_spirv_parser *parser);
static int drv_gpu_spirv_declare(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_declare_entry_point(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_declare_decoration(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_declare_member_decoration(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_declare_type(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_declare_constant(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_declare_constant_bool(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode);
static int drv_gpu_spirv_declare_constant_composite(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_declare_variable(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_decoration_ignored(uint32_t decoration);
static int drv_gpu_spirv_add_io(struct drv_gpu_spirv_parser *parser, uint32_t id, uint32_t type_id, int is_input, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_add_io_run(struct drv_gpu_spirv_parser *parser, int is_input, uint32_t location, uint32_t count, uint32_t components, uint32_t flat, uint32_t noperspective);
static void drv_gpu_spirv_add_uniform(struct drv_gpu_spirv_parser *parser, uint32_t id, uint32_t kind);
static int drv_gpu_spirv_pass_body(struct drv_gpu_spirv_parser *parser);
static int drv_gpu_spirv_lower(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_variable(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_access_chain(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_chain_block(struct drv_gpu_spirv_parser *parser, struct drv_gpu_spirv_id *record, struct drv_gpu_spirv_id *pointee, struct drv_gpu_spirv_id *index_record, uint32_t index_id, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_chain_scalars(struct drv_gpu_spirv_parser *parser, struct drv_gpu_spirv_id *record, struct drv_gpu_spirv_id *pointee, struct drv_gpu_spirv_id *index_record, uint32_t index_id, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_chain_dynamic(struct drv_gpu_spirv_parser *parser, struct drv_gpu_spirv_id *record, struct drv_gpu_spirv_id *pointee, uint32_t index_id, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_load(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_load_local(struct drv_gpu_spirv_parser *parser, const uint32_t *word, const struct drv_gpu_spirv_id *pointer, const struct drv_gpu_spirv_id *variable, uint32_t components, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_load_output(struct drv_gpu_spirv_parser *parser, const uint32_t *word, const struct drv_gpu_spirv_id *pointer, const struct drv_gpu_spirv_id *variable, uint32_t components, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_load_input(struct drv_gpu_spirv_parser *parser, const uint32_t *word, const struct drv_gpu_spirv_id *pointer, const struct drv_gpu_spirv_id *variable, uint32_t components, uint32_t opcode, uint32_t offset);
static uint32_t drv_gpu_spirv_local_scalar(struct drv_gpu_spirv_parser *parser, const struct drv_gpu_spirv_id *variable, uint32_t index, int zero_if_unstored);
static int drv_gpu_spirv_input_flat(struct drv_gpu_spirv_parser *parser, uint32_t location);
static uint32_t drv_gpu_spirv_index_is(struct drv_gpu_spirv_parser *parser, uint32_t index, uint32_t number);
static int drv_gpu_spirv_lower_load_block(struct drv_gpu_spirv_parser *parser, const uint32_t *word, struct drv_gpu_spirv_id *pointer, struct drv_gpu_spirv_id *variable, uint32_t opcode, uint32_t offset);
static uint32_t drv_gpu_spirv_load_word(struct drv_gpu_spirv_parser *parser, const struct drv_gpu_spirv_id *pointer, const struct drv_gpu_spirv_id *variable, uint32_t byte);
static int drv_gpu_spirv_lower_store(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_storage(struct drv_gpu_spirv_parser *parser, const uint32_t *word, const struct drv_gpu_spirv_id *pointer, const struct drv_gpu_spirv_id *variable, const uint32_t *scalars, uint32_t components, uint32_t opcode, uint32_t offset);
static uint32_t drv_gpu_spirv_storage_offset(struct drv_gpu_spirv_parser *parser, const struct drv_gpu_spirv_id *pointer, uint32_t byte);
static int drv_gpu_spirv_is_buffer_block(struct drv_gpu_spirv_parser *parser, uint32_t type_id);
static int drv_gpu_spirv_lower_store_local(struct drv_gpu_spirv_parser *parser, const struct drv_gpu_spirv_id *pointer, const struct drv_gpu_spirv_id *variable, const uint32_t *scalars, uint32_t components);
static int drv_gpu_spirv_lower_store_output(struct drv_gpu_spirv_parser *parser, const struct drv_gpu_spirv_id *pointer, const struct drv_gpu_spirv_id *variable, const uint32_t *scalars, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_arithmetic(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_integer(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static uint32_t drv_gpu_spirv_lower_integer_component(struct drv_gpu_spirv_parser *parser, uint32_t opcode, uint32_t left, uint32_t right);
static uint32_t drv_gpu_spirv_absolute_integer(struct drv_gpu_spirv_parser *parser, uint32_t value, uint32_t negative);
static int drv_gpu_spirv_lower_integer_unary(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_convert(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_width_convert(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_bitcast(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_float_remainder(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_vector_times_scalar(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_matrix_product(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_transpose(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_outer_product(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_negate(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_dot(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static uint32_t drv_gpu_spirv_dot_value(struct drv_gpu_spirv_parser *parser, const uint32_t *left, const uint32_t *right, uint32_t components);
static int drv_gpu_spirv_lower_construct(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_extract(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_insert(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_copy(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_composite_part(struct drv_gpu_spirv_parser *parser, uint32_t type_id, const uint32_t *word, uint32_t count, uint32_t first_index, uint32_t *first, uint32_t *length, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_shuffle(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_extended(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static uint32_t drv_gpu_spirv_lower_extended_component(struct drv_gpu_spirv_parser *parser, uint32_t function, uint32_t operand[3][4], uint32_t component);
static uint32_t drv_gpu_spirv_smooth_step(struct drv_gpu_spirv_parser *parser, uint32_t edge0, uint32_t edge1, uint32_t x);
static int drv_gpu_spirv_lower_matrix_function(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static uint32_t drv_gpu_spirv_minor_determinant(struct drv_gpu_spirv_parser *parser, const uint32_t *matrix, uint32_t rows, const uint32_t *columns_kept, const uint32_t *rows_kept, uint32_t size);
static int drv_gpu_spirv_lower_half(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_geometric(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_divide(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_compare(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static uint32_t drv_gpu_spirv_lower_compare_component(struct drv_gpu_spirv_parser *parser, uint32_t opcode, uint32_t left, uint32_t right);
static int drv_gpu_spirv_lower_integer_compare(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_logical(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_select(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_sample(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_derivative(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_texel_offset(struct drv_gpu_spirv_parser *parser, uint32_t id, uint32_t *bits, uint32_t opcode, uint32_t offset);
static struct drv_gpu_spirv_id * drv_gpu_spirv_image_type(struct drv_gpu_spirv_parser *parser, struct drv_gpu_spirv_id *image);
static void drv_gpu_spirv_texture_param(uint32_t *params, uint32_t *count, uint32_t value);
static int drv_gpu_spirv_emit_texture(struct drv_gpu_spirv_parser *parser, const struct drv_gpu_spirv_id *image, const uint32_t *params, uint32_t param_count, uint32_t message, uint32_t texel_offset, uint32_t *first);
static int drv_gpu_spirv_lower_image(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_texture(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_fetch(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_query(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_label(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_find_loop_merge(struct drv_gpu_spirv_parser *parser, uint32_t offset, uint32_t *merge, uint32_t *continue_target);
static int drv_gpu_spirv_loop_open(struct drv_gpu_spirv_parser *parser, uint32_t header, uint32_t merge, uint32_t continue_target, uint32_t *predicate, uint32_t opcode, uint32_t offset);
static void drv_gpu_spirv_loop_begin(struct drv_gpu_spirv_parser *parser, struct drv_gpu_spirv_loop *loop);
static int drv_gpu_spirv_lower_loop_merge(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_loop_back_edge(struct drv_gpu_spirv_parser *parser, uint32_t predicate, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_loop_close(struct drv_gpu_spirv_parser *parser, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_carry(struct drv_gpu_spirv_parser *parser, uint32_t variable, uint32_t component, uint32_t value);
static int drv_gpu_spirv_lower_phi(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_header_phi(struct drv_gpu_spirv_parser *parser, struct drv_gpu_spirv_loop *loop, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_branch(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_branch_conditional(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_switch(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_kill(struct drv_gpu_spirv_parser *parser, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_return(struct drv_gpu_spirv_parser *parser, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_edge_add(struct drv_gpu_spirv_parser *parser, uint32_t target, uint32_t predicate, uint32_t opcode, uint32_t offset);
static uint32_t drv_gpu_spirv_edge_predicate(struct drv_gpu_spirv_parser *parser, uint32_t from, uint32_t to, int *found);
static uint32_t drv_gpu_spirv_predicate_and(struct drv_gpu_spirv_parser *parser, uint32_t predicate, uint32_t condition);
static uint32_t drv_gpu_spirv_predicated_value(struct drv_gpu_spirv_parser *parser, uint32_t value, uint32_t previous);
static uint32_t drv_gpu_spirv_shared_constant(struct drv_gpu_spirv_parser *parser, uint32_t *slot, enum drv_gpu_shader_ir_op op, uint32_t bits);
static uint32_t drv_gpu_spirv_float_constant(struct drv_gpu_spirv_parser *parser, uint32_t bits);
static uint32_t drv_gpu_spirv_integer_constant(struct drv_gpu_spirv_parser *parser, uint32_t bits);
static uint32_t drv_gpu_spirv_select_value(struct drv_gpu_spirv_parser *parser, uint32_t condition, uint32_t taken, uint32_t other);
static void drv_gpu_spirv_guard(struct drv_gpu_spirv_parser *parser, struct drv_gpu_shader_ir_inst *inst);
static void drv_gpu_spirv_skip_open(struct drv_gpu_spirv_parser *parser);
static void drv_gpu_spirv_skip_close(struct drv_gpu_spirv_parser *parser);
static uint32_t drv_gpu_spirv_move_value(struct drv_gpu_spirv_parser *parser, uint32_t destination, uint32_t source);
static int drv_gpu_spirv_refuse(struct drv_gpu_spirv_parser *parser, uint32_t opcode, uint32_t word_offset, const char *reason);
static struct drv_gpu_spirv_id * drv_gpu_spirv_id(struct drv_gpu_spirv_parser *parser, uint32_t id);
static uint32_t drv_gpu_spirv_kind_components(struct drv_gpu_spirv_parser *parser, uint32_t type_id, uint32_t scalar);
static uint32_t drv_gpu_spirv_float_components(struct drv_gpu_spirv_parser *parser, uint32_t type_id);
static uint32_t drv_gpu_spirv_int_components(struct drv_gpu_spirv_parser *parser, uint32_t type_id);
static uint32_t drv_gpu_spirv_bool_components(struct drv_gpu_spirv_parser *parser, uint32_t type_id);
static uint32_t drv_gpu_spirv_value_components(struct drv_gpu_spirv_parser *parser, uint32_t type_id);
static int drv_gpu_spirv_type_has_int16(struct drv_gpu_spirv_parser *parser, uint32_t type_id, uint32_t depth);
static uint32_t drv_gpu_spirv_int_width(struct drv_gpu_spirv_parser *parser, uint32_t type_id);
static uint32_t drv_gpu_spirv_operand_int_width(struct drv_gpu_spirv_parser *parser, uint32_t id);
static uint32_t drv_gpu_spirv_extend16(struct drv_gpu_spirv_parser *parser, uint32_t value, uint32_t width, int is_signed);
static uint32_t drv_gpu_spirv_matrix_components(struct drv_gpu_spirv_parser *parser, uint32_t type_id);
static uint32_t drv_gpu_spirv_float_components_wide(struct drv_gpu_spirv_parser *parser, uint32_t type_id);
static int drv_gpu_spirv_type_size(struct drv_gpu_spirv_parser *parser, uint32_t type_id, uint32_t depth, uint32_t *scalars, uint32_t *locations);
static uint32_t drv_gpu_spirv_aggregate_scalars(struct drv_gpu_spirv_parser *parser, uint32_t type_id);
static int drv_gpu_spirv_type_step(struct drv_gpu_spirv_parser *parser, uint32_t type_id, uint32_t index, uint32_t *next_type, uint32_t *scalar_offset, uint32_t *io_offset, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_io_map(struct drv_gpu_spirv_parser *parser, uint32_t type_id, uint32_t depth, uint32_t base, uint32_t *map, uint32_t *count, uint32_t capacity);
static int drv_gpu_spirv_variable_slots(struct drv_gpu_spirv_parser *parser, struct drv_gpu_spirv_id *variable, uint32_t count);
static uint32_t *
drv_gpu_spirv_variable_slot(struct drv_gpu_spirv_parser *parser,
			    const struct drv_gpu_spirv_id *variable,
			    uint32_t index);
static uint32_t drv_gpu_spirv_operand_type(struct drv_gpu_spirv_parser *parser, uint32_t id);
static uint32_t drv_gpu_spirv_operand_int_components(struct drv_gpu_spirv_parser *parser, uint32_t id);
static uint32_t drv_gpu_spirv_operand_float_components(struct drv_gpu_spirv_parser *parser, uint32_t id);
static void drv_gpu_spirv_operand_shape(struct drv_gpu_spirv_parser *parser, uint32_t id, uint32_t *columns, uint32_t *rows);
static uint32_t drv_gpu_spirv_emit_value(struct drv_gpu_spirv_parser *parser, enum drv_gpu_shader_ir_op op, uint32_t source0, uint32_t source1);
static struct drv_gpu_shader_ir_inst * drv_gpu_spirv_emit(struct drv_gpu_spirv_parser *parser, enum drv_gpu_shader_ir_op op, uint32_t dst, uint32_t source0, uint32_t source1);
static uint32_t drv_gpu_spirv_new_value(struct drv_gpu_spirv_parser *parser);
static uint32_t drv_gpu_spirv_operand(struct drv_gpu_spirv_parser *parser, uint32_t id, uint32_t comp[4]);
static uint32_t drv_gpu_spirv_operand_wide(struct drv_gpu_spirv_parser *parser, uint32_t id, uint32_t comp[MAX_COMPONENTS]);
static int drv_gpu_spirv_escaped(struct drv_gpu_spirv_parser *parser, uint32_t value);
static uint32_t drv_gpu_spirv_constant_operand(struct drv_gpu_spirv_parser *parser, struct drv_gpu_spirv_id *record, uint32_t comp[MAX_COMPONENTS]);
static struct drv_gpu_spirv_id * drv_gpu_spirv_result(struct drv_gpu_spirv_parser *parser, uint32_t id, uint32_t type_id, uint32_t count, int fresh);
static int drv_gpu_spirv_declare_execution_mode(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_compute_declared(struct drv_gpu_spirv_parser *parser);
static int drv_gpu_spirv_declare_system(struct drv_gpu_spirv_parser *parser, struct drv_gpu_spirv_id *record, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_chain_system(struct drv_gpu_spirv_parser *parser, struct drv_gpu_spirv_id *record, struct drv_gpu_spirv_id *pointee, struct drv_gpu_spirv_id *index_record, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_load_system(struct drv_gpu_spirv_parser *parser, const uint32_t *word, const struct drv_gpu_spirv_id *pointer, const struct drv_gpu_spirv_id *variable, uint32_t components, uint32_t opcode, uint32_t offset);
static uint32_t drv_gpu_spirv_system_component(struct drv_gpu_spirv_parser *parser, uint32_t builtin, uint32_t component);
static uint32_t drv_gpu_spirv_load_system(struct drv_gpu_spirv_parser *parser, uint32_t which);
static uint32_t drv_gpu_spirv_system_uniform(struct drv_gpu_spirv_parser *parser);
static int drv_gpu_spirv_lower_atomic(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_atomic_access(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_atomic_pointer(struct drv_gpu_spirv_parser *parser, uint32_t pointer_id, struct drv_gpu_spirv_id **pointer, struct drv_gpu_spirv_id **variable, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_array_length(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static const char *drv_gpu_spirv_compute_refusal(uint32_t opcode);
static int drv_gpu_spirv_declare_shared(struct drv_gpu_spirv_parser *parser, struct drv_gpu_spirv_id *record, uint32_t opcode, uint32_t offset);
static uint32_t drv_gpu_spirv_shared_bytes(struct drv_gpu_spirv_parser *parser, uint32_t type_id);
static int drv_gpu_spirv_chain_shared(struct drv_gpu_spirv_parser *parser, struct drv_gpu_spirv_id *record, struct drv_gpu_spirv_id *pointee, struct drv_gpu_spirv_id *index_record, uint32_t index_id, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_shared(struct drv_gpu_spirv_parser *parser, const uint32_t *word, const struct drv_gpu_spirv_id *pointer, const uint32_t *scalars, uint32_t components, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_semantics(struct drv_gpu_spirv_parser *parser, uint32_t semantics_id, uint32_t *fences, uint32_t opcode, uint32_t offset);
static void drv_gpu_spirv_fence(struct drv_gpu_spirv_parser *parser, uint32_t fences);
static int drv_gpu_spirv_lower_barrier(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_geometry_mode(struct drv_gpu_spirv_parser *parser, const uint32_t *word, uint32_t count, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_geometry_declared(struct drv_gpu_spirv_parser *parser);
static int drv_gpu_spirv_declare_geometry_input(struct drv_gpu_spirv_parser *parser, struct drv_gpu_spirv_id *record, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_chain_vertex(struct drv_gpu_spirv_parser *parser, struct drv_gpu_spirv_id *record, struct drv_gpu_spirv_id *pointee, struct drv_gpu_spirv_id *index_record, uint32_t index_id, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_load_vertex(struct drv_gpu_spirv_parser *parser, const uint32_t *word, const struct drv_gpu_spirv_id *pointer, const struct drv_gpu_spirv_id *variable, uint32_t components, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_gl_in_location(struct drv_gpu_spirv_parser *parser, const struct drv_gpu_spirv_id *pointer, const struct drv_gpu_spirv_id *variable, uint32_t slot, uint32_t *location, uint32_t *component, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_geometry_output(struct drv_gpu_spirv_parser *parser, uint32_t builtin, uint32_t *location, uint32_t opcode, uint32_t offset);
static int drv_gpu_spirv_lower_emit(struct drv_gpu_spirv_parser *parser, uint32_t opcode, uint32_t offset);

/*
 * Parses SPIR-V words into the scalar IR of one stage.
 *
 * Returns 0 and the IR in `*out`, EINVAL for a malformed module, ENOTSUP for
 * valid SPIR-V this parser does not lower, or ENOMEM.  On a failure `*out` is
 * NULL and, when `diagnostic` is not NULL, it names the refused instruction
 * (it is cleared on entry and stays clear for a failure that names none).
 */
int
drv_gpu_shader_parse(
	const uint32_t *words,
	size_t word_count,
	enum drv_gpu_shader_stage stage,
	struct drv_gpu_shader_ir **out,
	struct drv_gpu_compile_diagnostic *diagnostic)
{
	int error;
	struct drv_gpu_spirv_parser *parser;
	struct drv_gpu_shader_ir *ir;
	uint32_t slots;

	/* Every call starts without a previous module refusal. */
	if (diagnostic != NULL)
		kern_memset(diagnostic, 0, sizeof(*diagnostic));

	/* Requires a place to publish the wholly parsed shader. */
	if (out == NULL)
		return EINVAL;

	/* The caller receives nothing unless the whole module parses. */
	*out = NULL;

	/*
	 * The stage the caller expects must be one of the known ones (a module
	 * without an entry point keeps it).
	 */
	if ((uint32_t)stage >= DRV_GPU_STAGE_COUNT)
		return EINVAL;

	/*
	 * Refuses a missing stream or a length the 32-bit parser cannot
	 * represent.
	 */
	if (words == NULL || word_count > UINT32_MAX)
		return EINVAL;

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

	/*
	 * Keeps the decode state off the finite kernel stack until this parse
	 * ends.
	 */
	parser = kern_calloc(1U, sizeof(*parser));
	if (parser == NULL) {
		kern_free(ir);
		return ENOMEM;
	}

	/* Prepares the decode state and its id table. */
	parser->code = words;
	parser->words = (uint32_t)word_count;
	parser->bound = words[3];
	parser->ir = ir;
	parser->predicate = PREDICATE_ALWAYS;
	parser->pending_merge = NO_VALUE;
	parser->zero_value = NO_VALUE;
	parser->one_value = NO_VALUE;
	parser->true_value = NO_VALUE;
	parser->int_16_value = NO_VALUE;
	parser->int_ffff_value = NO_VALUE;
	parser->ids = kern_calloc(parser->bound, sizeof(*parser->ids));
	if (parser->ids == NULL) {
		kern_free(ir);
		kern_free(parser);
		return ENOMEM;
	}

	/*
	 * Interface lists are bounded by the id count and the locations a
	 * shader's own interface may take (an array or a block takes several).
	 */
	slots = parser->bound + MAX_USER_LOCATION;
	parser->io_capacity = slots;

	/* Allocates the input list. */
	ir->inputs = kern_calloc(slots, sizeof(*ir->inputs));
	if (ir->inputs == NULL) {
		drv_gpu_shader_ir_free(ir);
		kern_free(parser->ids);
		kern_free(parser);
		return ENOMEM;
	}

	/* Allocates the output list. */
	ir->outputs = kern_calloc(slots, sizeof(*ir->outputs));
	if (ir->outputs == NULL) {
		drv_gpu_shader_ir_free(ir);
		kern_free(parser->ids);
		kern_free(parser);
		return ENOMEM;
	}

	/* Allocates the uniform list. */
	ir->uniforms = kern_calloc(slots, sizeof(*ir->uniforms));
	if (ir->uniforms == NULL) {
		drv_gpu_shader_ir_free(ir);
		kern_free(parser->ids);
		kern_free(parser);
		return ENOMEM;
	}

	/*
	 * The first pass records types, constants, decorations and interface
	 * variables.
	 */
	error = drv_gpu_spirv_pass_declarations(parser);

	/*
	 * A compute shader's workgroup size is settled once every constant is
	 * declared.
	 */
	if (error == 0)
		error = drv_gpu_spirv_compute_declared(parser);

	/*
	 * A geometry shader must have said what it takes and emits
	 * (ws075-p007a).
	 */
	if (error == 0)
		error = drv_gpu_spirv_geometry_declared(parser);

	/*
	 * The stream starts with room for the common lowerings and grows for
	 * the longer ones; failing to grow is an error, never a silently
	 * shorter shader.
	 */
	if (error == 0) {
		parser->capacity =
		    parser->body_instructions * IR_PER_INSTRUCTION +
		    IR_PER_INSTRUCTION;
		ir->instructions =
		    kern_calloc(parser->capacity, sizeof(*ir->instructions));
		if (ir->instructions == NULL)
			error = ENOMEM;
	}

	/*
	 * A terminator adds at most two edges, so the edge list has room for
	 * two per body instruction, and for each case of an OpSwitch.
	 */
	if (error == 0) {
		parser->edge_capacity =
		    2U * parser->body_instructions + parser->switch_edges + 2U;
		parser->edges =
		    kern_calloc(parser->edge_capacity, sizeof(*parser->edges));
		if (parser->edges == NULL)
			error = ENOMEM;
	}

	/* The second pass lowers the entry function body. */
	if (error == 0)
		error = drv_gpu_spirv_pass_body(parser);

	/* A failure an emitter latched fails the parse as well. */
	if (error == 0 && parser->error != 0)
		error = parser->error;

	/*
	 * The edge and carried lists and the variables' slots are only needed
	 * while parsing.
	 */
	if (parser->edges != NULL)
		kern_free(parser->edges);
	if (parser->carried != NULL)
		kern_free(parser->carried);
	if (parser->slots != NULL)
		kern_free(parser->slots);

	/*
	 * A failed parse reports the refused instruction and releases
	 * everything.
	 */
	if (error != 0) {
		if (diagnostic != NULL)
			*diagnostic = parser->diag;
		drv_gpu_shader_ir_free(ir);
		kern_free(parser->ids);
		kern_free(parser);
		return error;
	}

	/* The id table and decode state are only needed while parsing. */
	kern_free(parser->ids);
	kern_free(parser);

	/* Succeeded: the caller owns the IR. */
	*out = ir;
	return 0;
}

/*
 * Releases a parsed shader IR and its lists.
 */
void
drv_gpu_shader_ir_free(
	struct drv_gpu_shader_ir *ir)
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

	/*
	 * Succeeded: the parsed shader and its lists have no remaining owner.
	 */
	return;
}

/* Records types, constants, decorations and interface variables. */
static int
drv_gpu_spirv_pass_declarations(
	struct drv_gpu_spirv_parser *parser)
{
	int error;
	const uint32_t *word;
	uint32_t offset;
	uint32_t count;
	uint32_t opcode;
	int in_function;

	/* Walks every instruction after the header. */
	in_function = 0;
	offset = SPIRV_HEADER_WORDS;
	while (offset < parser->words) {
		/* Decodes the word count and the opcode of the instruction. */
		word = parser->code + offset;
		count = word[0] >> 16;
		opcode = word[0] & 0xFFFFU;

		/*
		 * An instruction must have a length and end inside the module.
		 */
		if (count == 0U || offset + count > parser->words)
			return EINVAL;

		/*
		 * Function bodies are only counted here, to size the IR stream;
		 * the OpFunction and OpFunctionEnd instructions count as well.
		 */
		if (opcode == OP_FUNCTION)
			in_function = 1;
		if (in_function != 0) {
			parser->body_instructions++;

			/*
			 * An OpSwitch adds an edge per case and one for the
			 * default.
			 */
			if (opcode == OP_SWITCH && count > 3U)
				parser->switch_edges += (count - 3U) / 2U;
			if (opcode == OP_FUNCTION_END)
				in_function = 0;
			offset += count;
			continue;
		}

		/* Records what the module-level instruction declares. */
		error =
		    drv_gpu_spirv_declare(parser, word, count, opcode, offset);
		if (error != 0)
			return error;

		/* Advances to the next module-level declaration. */
		offset += count;
	}

	/* Succeeded: every module-level instruction was interpreted. */
	return 0;
}

/* Records one module-level instruction. */
static int
drv_gpu_spirv_declare(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *record;

	/* Dispatches on what the instruction declares. */
	switch (opcode) {
	case OP_ENTRY_POINT:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_declare_entry_point(parser,
							  word,
							  count,
							  opcode,
							  offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_DECORATE:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_declare_decoration(parser,
							 word,
							 count,
							 opcode,
							 offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_MEMBER_DECORATE:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_declare_member_decoration(parser,
								word,
								count,
								opcode,
								offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

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
	case OP_TYPE_RUNTIME_ARRAY:
	case OP_TYPE_STRUCT:
	case OP_TYPE_POINTER:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_declare_type(parser,
						   word,
						   count,
						   opcode,
						   offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_CONSTANT:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_declare_constant(parser,
						       word,
						       count,
						       opcode,
						       offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_CONSTANT_TRUE:
	case OP_CONSTANT_FALSE:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_declare_constant_bool(parser,
							    word,
							    count,
							    opcode);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_CONSTANT_COMPOSITE:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_declare_constant_composite(parser,
								 word,
								 count,
								 opcode,
								 offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_VARIABLE:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_declare_variable(parser,
						       word,
						       count,
						       opcode,
						       offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_EXT_INST_IMPORT:
		/* An extended instruction set: only its id is recorded. */
		record = NULL;
		if (count >= 2U)
			record = drv_gpu_spirv_id(parser, word[1]);
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
		/* Module-level instructions without execution semantics. */
		return 0;

	case OP_EXECUTION_MODE:
	case OP_EXECUTION_MODE_ID:
		/*
		 * A compute shader's workgroup size; any other literal mode has
		 * no effect.
		 */
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_declare_execution_mode(parser,
							     word,
							     count,
							     opcode,
							     offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	default:
		break;
	}

	/* Anything else could change the shader and is refused. */
	error = drv_gpu_spirv_refuse(
	    parser,
	    opcode,
	    offset,
	    "module-level instruction that is not interpreted");
	return error;
}

/* Takes the stage from the entry point's execution model. */
static int
drv_gpu_spirv_declare_entry_point(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;

	/* The instruction must name an execution model and an entry. */
	if (count < 3U)
		return EINVAL;

	/* Vertex, fragment, compute and geometry shaders are lowered. */
	if (word[1] == EM_VERTEX) {
		parser->ir->stage = DRV_GPU_STAGE_VERTEX;
	} else if (word[1] == EM_FRAGMENT) {
		parser->ir->stage = DRV_GPU_STAGE_FRAGMENT;
	} else if (word[1] == EM_GL_COMPUTE) {
		parser->ir->stage = DRV_GPU_STAGE_COMPUTE;
	} else if (word[1] == EM_GEOMETRY) {
		parser->ir->stage = DRV_GPU_STAGE_GEOMETRY;
	} else {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error =
		    drv_gpu_spirv_refuse(parser,
					 opcode,
					 offset,
					 "execution model other than Vertex / "
					 "Fragment / GLCompute / Geometry");
		return error;
	}

	/* Succeeded: the stage is known. */
	return 0;
}

/*
 * Interprets one decoration.
 *
 * Interpreted: Location, Binding, DescriptorSet, BuiltIn, Block,
 * ArrayStride, Flat (kept for the draw's constant interpolation) and
 * NoPerspective (a fragment input interpolated linearly; a vertex shader's
 * output only carries it).  Without effect on this lowering:
 * RelaxedPrecision (a permission to lose precision, never used here) and
 * Centroid (a draw has one sample per pixel, whose centre is the centroid).
 * Anything else could place or qualify data -- components, buffer blocks --
 * and is refused.
 */
static int
drv_gpu_spirv_declare_decoration(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *record;
	int ignored;

	/* Resolves the decorated id. */
	record = NULL;
	if (count >= 3U)
		record = drv_gpu_spirv_id(parser, word[1]);
	if (record == NULL)
		return EINVAL;

	/* Records the decoration, or refuses one that is not interpreted. */
	if (word[2] == DEC_LOCATION && count >= 4U) {
		record->has_location = 1U;
		record->location = word[3];
	} else if (word[2] == DEC_INDEX && count >= 4U && word[3] <= 1U) {
		record->index = (uint8_t)word[3];
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
	} else if (word[2] == DEC_FLAT) {
		record->flat = 1U;
	} else if (word[2] == DEC_NO_PERSPECTIVE) {
		record->noperspective = 1U;
	} else if (word[2] == DEC_BUFFER_BLOCK) {
		record->buffer_block = 1U;
	} else {
		/* A decoration without effect, or one that is refused. */
		ignored = drv_gpu_spirv_decoration_ignored(word[2]);
		if (ignored == 0) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "decoration that is not interpreted");
			return error;
		}
	}

	/* Succeeded: the decoration is recorded or has no effect. */
	return 0;
}

/*
 * Reports whether a decoration has no effect on this lowering: Block,
 * RelaxedPrecision and Centroid (one sample per pixel, whose centre is the
 * centroid), NonWritable and NonReadable.
 */
static int
drv_gpu_spirv_decoration_ignored(
	uint32_t decoration)
{
	/* Those without effect in every stage. */
	if (decoration == DEC_BLOCK ||
	    decoration == DEC_RELAXED_PRECISION ||
	    decoration == DEC_CENTROID)
		return 1;

	/*
	 * A storage buffer's access qualifiers change nothing the lowering
	 * does.
	 */
	if (decoration == DEC_NON_WRITABLE || decoration == DEC_NON_READABLE)
		return 1;

	/*
	 * Nor do its coherence and aliasing qualifiers: the code generator
	 * never reorders a memory access, and every access goes to memory
	 * uncached (ws101-p002).
	 */
	if (decoration == DEC_COHERENT ||
	    decoration == DEC_VOLATILE ||
	    decoration == DEC_RESTRICT ||
	    decoration == DEC_ALIASED)
		return 1;

	/* Anything else has an effect. */
	return 0;
}

/*
 * Interprets one structure member decoration.
 *
 * Interpreted: Offset, BuiltIn, MatrixStride, ColMajor and RowMajor, and
 * an interface block member's Flat and NoPerspective; RelaxedPrecision and
 * Centroid have no effect; anything else is refused.
 */
static int
drv_gpu_spirv_declare_member_decoration(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *record;
	uint32_t member;
	int ignored;

	/* Resolves the decorated structure type. */
	record = NULL;
	if (count >= 4U)
		record = drv_gpu_spirv_id(parser, word[1]);
	if (record == NULL)
		return EINVAL;

	/* Refuses a member beyond the member table. */
	member = word[2];
	if (member >= MAX_MEMBERS) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "structure with more members than supported");
		return error;
	}

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
	} else if (word[3] == DEC_FLAT) {
		record->member_flat[member] = 1U;
	} else if (word[3] == DEC_NO_PERSPECTIVE) {
		record->member_noperspective[member] = 1U;
	} else {
		/*
		 * A decoration without effect (RelaxedPrecision, Centroid, the
		 * access qualifiers), or one that is refused.
		 */
		ignored = drv_gpu_spirv_decoration_ignored(word[3]);
		if (ignored == 0) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "member decoration that is not interpreted");
			return error;
		}
	}

	/* Succeeded: the decoration is recorded or has no effect. */
	return 0;
}

/* Records one type declaration. */
static int
drv_gpu_spirv_declare_type(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *record;
	struct drv_gpu_spirv_id *length;
	uint32_t index;

	/* Resolves the declared type's id. */
	record = NULL;
	if (count >= 2U)
		record = drv_gpu_spirv_id(parser, word[1]);
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
		/*
		 * An image: the type of its texels, its Dim and whether it is
		 * Arrayed.
		 */
		if (count < 9U)
			return EINVAL;
		record->kind = ID_TYPE_IMAGE;
		record->type = word[2];
		record->image_dim = (uint8_t)word[3];
		record->image_arrayed = (uint8_t)word[5];
		record->image_ms = (uint8_t)word[6];
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

		/* The width decides which ones are lowered. */
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
		/*
		 * An array: the element type, and the length of its constant.
		 */
		if (count < 4U)
			return EINVAL;
		length = drv_gpu_spirv_id(parser, word[3]);
		if (length == NULL)
			return EINVAL;
		record->kind = ID_TYPE_ARRAY;
		record->type = word[2];
		record->length = 0U;
		if (length->kind == ID_CONSTANT)
			record->length = length->constant;
		break;

	case OP_TYPE_RUNTIME_ARRAY:
		/*
		 * An array of a storage buffer whose length the buffer decides:
		 * length 0.
		 */
		if (count < 3U)
			return EINVAL;
		record->kind = ID_TYPE_ARRAY;
		record->type = word[2];
		record->length = 0U;
		break;

	case OP_TYPE_STRUCT:
		/* A structure with at most MAX_MEMBERS members. */
		if (count - 2U > MAX_MEMBERS) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "structure with more members than supported");
			return error;
		}

		/* Publishes the structure type after validating all member identities. */
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
drv_gpu_spirv_declare_constant(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *record;

	/* Resolves the constant's result id. */
	record = NULL;
	if (count >= 4U)
		record = drv_gpu_spirv_id(parser, word[2]);
	if (record == NULL)
		return EINVAL;

	/*
	 * A constant of more than one word is wider than any value lowered
	 * here.
	 */
	if (count != 4U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "constant wider than 32 bits");
		return error;
	}

	/* Records the constant's type and bits. */
	record->kind = ID_CONSTANT;
	record->type = word[1];
	record->constant = word[3];

	/* Succeeded: the constant becomes IR the first time it is used. */
	return 0;
}

/*
 * Records OpConstantTrue or OpConstantFalse: a Boolean constant of all ones or
 * zero.
 */
static int
drv_gpu_spirv_declare_constant_bool(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode)
{
	struct drv_gpu_spirv_id *record;

	/* Resolves the constant's result id. */
	record = NULL;
	if (count >= 3U)
		record = drv_gpu_spirv_id(parser, word[2]);
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
 * Records a composite constant: a vector of scalar constants, a matrix of
 * vector constants, or an array or a structure of constants.  Its scalars
 * are its constituents' in order, as a value's are (see
 * drv_gpu_spirv_type_size()).
 */
static int
drv_gpu_spirv_declare_constant_composite(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *record;
	struct drv_gpu_spirv_id *constituent;
	uint32_t index;

	/* Resolves the constant's result id. */
	record = NULL;
	if (count >= 3U)
		record = drv_gpu_spirv_id(parser, word[2]);
	if (record == NULL)
		return EINVAL;

	/*
	 * A composite has one constituent at least, and no more than a
	 * structure has members.
	 */
	if (count < 4U || count - 3U > MAX_MEMBERS) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "constant composite of more constituents than supported");
		return error;
	}

	/* Every constituent must be a constant declared before. */
	for (index = 3U; index < count; index++) {
		constituent = drv_gpu_spirv_id(parser, word[index]);
		if (constituent == NULL)
			return EINVAL;
		if (constituent->kind != ID_CONSTANT &&
		    constituent->kind != ID_CONSTANT_COMPOSITE) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "constant composite of something that is not a "
			    "constant");
			return error;
		}
	}

	/*
	 * Records the type and the constituents; each becomes IR the first time
	 * it is used.
	 */
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
drv_gpu_spirv_declare_variable(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	uint32_t source_shape;
	int error;
	struct drv_gpu_spirv_id *record;
	struct drv_gpu_spirv_id *type;
	uint32_t storage;
	uint32_t scalars;
	uint32_t locations;
	int buffer_block;

	/* Resolves the variable and its pointer type. */
	record = NULL;
	if (count >= 4U)
		record = drv_gpu_spirv_id(parser, word[2]);
	if (record == NULL)
		return EINVAL;
	type = drv_gpu_spirv_id(parser, word[1]);
	if (type == NULL)
		return EINVAL;
	if (type->kind != ID_TYPE_POINTER)
		return EINVAL;

	/*
	 * An initializer would give the variable a value this lowering does not
	 * track.
	 */
	if (count > 4U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error =
		    drv_gpu_spirv_refuse(parser,
					 opcode,
					 offset,
					 "variable initializer is not lowered");
		return error;
	}

	/*
	 * Records the variable as a pointer to its pointee, nothing selected
	 * yet; a BufferBlock pointee is a storage buffer.
	 */
	storage = word[3];
	buffer_block = drv_gpu_spirv_is_buffer_block(parser, type->type);
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
	 * A vertex shader's gl_VertexIndex and gl_InstanceIndex are inputs at
	 * the locations the draw fills them at.
	 */
	if (storage == SC_INPUT &&
	    record->has_builtin != 0U &&
	    parser->ir->stage == DRV_GPU_STAGE_VERTEX) {
		if (record->builtin == BUILTIN_VERTEX_INDEX) {
			record->has_location = 1U;
			record->location = DRV_GPU_SHADER_LOCATION_VERTEX_INDEX;
		} else if (record->builtin == BUILTIN_INSTANCE_INDEX) {
			record->has_location = 1U;
			record->location =
			    DRV_GPU_SHADER_LOCATION_INSTANCE_INDEX;
		}
	}

	/*
	 * A fragment shader's gl_FrontFacing, gl_FragCoord and gl_PointCoord
	 * are inputs at the locations of the payload's facing bit and pixel
	 * position and of the point sprite's coordinate.
	 */
	if (storage == SC_INPUT &&
	    record->has_builtin != 0U &&
	    parser->ir->stage == DRV_GPU_STAGE_FRAGMENT) {
		if (record->builtin == BUILTIN_FRONT_FACING) {
			record->has_location = 1U;
			record->location = DRV_GPU_SHADER_LOCATION_FRONT_FACING;
		} else if (record->builtin == BUILTIN_FRAG_COORD) {
			record->has_location = 1U;
			record->location = DRV_GPU_SHADER_LOCATION_FRAG_COORD;
		} else if (record->builtin == BUILTIN_POINT_COORD) {
			record->has_location = 1U;
			record->location = DRV_GPU_SHADER_LOCATION_POINT_COORD;
		} else if (record->builtin == BUILTIN_PRIMITIVE_ID) {
			/*
			 * gl_PrimitiveID: the primitive's number, the same for
			 * all its pixels (ws075-p007a).
			 */
			record->has_location = 1U;
			record->location = DRV_GPU_SHADER_LOCATION_PRIMITIVE_ID;
			record->flat = 1U;
		}
	}

	/*
	 * A 16-bit integer is not lowered in an interface's memory (that needs
	 * the 16-bit storage extensions).
	 */
	if ((storage == SC_INPUT ||
	    storage == SC_OUTPUT ||
	    storage == SC_PUSH_CONSTANT ||
	    storage == SC_UNIFORM ||
	    storage == SC_STORAGE_BUFFER)) {
		/* Resolves the declared operand shape before comparing it. */
		source_shape =
		    drv_gpu_spirv_type_has_int16(parser, record->pointee, 0U);

		/* Requires the shape accepted by this lowering operation. */
		if (source_shape != 0) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "16-bit integer in an input, output, push constant "
			    "or buffer");
			return error;
		}
	}

	/*
	 * The storage class, and a location, decide what the variable is to the
	 * shader.
	 */
	if (storage == SC_INPUT &&
	    record->has_builtin != 0U &&
	    parser->ir->stage == DRV_GPU_STAGE_COMPUTE) {
		error = drv_gpu_spirv_declare_system(parser,
						     record,
						     opcode,
						     offset);
		if (error != 0)
			return error;
	} else if (storage == SC_INPUT &&
		   parser->ir->stage == DRV_GPU_STAGE_GEOMETRY) {
		error = drv_gpu_spirv_declare_geometry_input(parser,
							     record,
							     opcode,
							     offset);
		if (error != 0)
			return error;
	} else if (storage == SC_INPUT && record->has_location != 0U) {
		record->ptr_kind = PTR_INPUT;
		error = drv_gpu_spirv_add_io(parser,
					     word[2],
					     record->pointee,
					     1,
					     opcode,
					     offset);
		if (error != 0)
			return error;
	} else if (storage == SC_OUTPUT && record->has_location != 0U) {
		record->ptr_kind = PTR_OUTPUT;
		error = drv_gpu_spirv_add_io(parser,
					     word[2],
					     record->pointee,
					     0,
					     opcode,
					     offset);
		if (error != 0)
			return error;
	} else if (storage == SC_OUTPUT) {
		record->ptr_kind = PTR_OUTPUT_BLOCK;
	} else if (storage == SC_PUSH_CONSTANT) {
		record->ptr_kind = PTR_PUSH;
	} else if (storage == SC_UNIFORM_CONSTANT) {
		record->ptr_kind = PTR_SAMPLER;
		drv_gpu_spirv_add_uniform(parser,
					  word[2],
					  DRV_GPU_IR_UNIFORM_SAMPLED_IMAGE);
	} else if (storage == SC_STORAGE_BUFFER ||
		   (storage == SC_UNIFORM && buffer_block != 0)) {
		record->ptr_kind = PTR_SSBO;
		record->uniform = parser->ir->uniform_count;
		drv_gpu_spirv_add_uniform(parser,
					  word[2],
					  DRV_GPU_IR_UNIFORM_STORAGE);
	} else if (storage == SC_UNIFORM) {
		record->ptr_kind = PTR_UBO;
		record->uniform = parser->ir->uniform_count;
		drv_gpu_spirv_add_uniform(parser,
					  word[2],
					  DRV_GPU_IR_UNIFORM_BLOCK);
	} else if (storage == SC_WORKGROUP &&
		   parser->ir->stage == DRV_GPU_STAGE_COMPUTE) {
		error = drv_gpu_spirv_declare_shared(parser,
						     record,
						     opcode,
						     offset);
		if (error != 0)
			return error;
	} else {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "variable in a storage class that is not lowered");
		return error;
	}

	/* Only an output remembers what is stored in it. */
	if (record->ptr_kind != PTR_OUTPUT &&
	    record->ptr_kind != PTR_OUTPUT_BLOCK)
		return 0;

	/*
	 * An output remembers what was last stored to each of its interface
	 * slots, four to a location, for a store under a predicate to keep
	 * where the predicate is false; nothing is stored yet.
	 */
	error = drv_gpu_spirv_type_size(parser,
					record->pointee,
					0U,
					&scalars,
					&locations);
	if (error != 0) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error =
		    drv_gpu_spirv_refuse(parser,
					 opcode,
					 offset,
					 "output that is not made of scalars");
		return error;
	}

	/* Allocates the scalar slots carried by this interface variable. */
	error = drv_gpu_spirv_variable_slots(parser, record, 4U * locations);
	if (error == ENOMEM)
		return error;
	if (error != 0) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "output larger than supported");
		return error;
	}

	/* Succeeded: the variable is part of the interface. */
	return 0;
}

/*
 * Adds the interface locations of an input or an output variable: one entry
 * to each location the type `type_id` takes, from the variable's own
 * location on -- a scalar or a vector one, a matrix one to a column, an
 * array its elements', an interface block its members' in order -- each
 * Flat or NoPerspective as the variable or its block member is.  The type
 * is the variable's pointee, or, for a geometry shader's per-vertex input,
 * the element one input vertex has.  Refuses a variable whose locations run
 * into the ones the draw generates.
 */
static int
drv_gpu_spirv_add_io(
	struct drv_gpu_spirv_parser *parser,
	uint32_t id,
	uint32_t type_id,
	int is_input,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *variable;
	struct drv_gpu_spirv_id *type;
	uint32_t location;
	uint32_t scalars;
	uint32_t locations;
	uint32_t components;
	uint32_t member;

	/* Resolves the variable and the type whose locations it takes. */
	variable = &parser->ids[id];
	type = drv_gpu_spirv_id(parser, type_id);
	if (type == NULL)
		return EINVAL;

	/*
	 * A scalar or a vector counts its float components, one when it holds
	 * none.
	 */
	components = drv_gpu_spirv_float_components(parser, type_id);
	if (components == 0U)
		components = 1U;

	/* A location the draw generates is one entry of its own. */
	if (variable->location >= MAX_USER_LOCATION) {
		error = drv_gpu_spirv_add_io_run(parser,
						 is_input,
						 variable->location,
						 1U,
						 components,
						 variable->flat,
						 variable->noperspective);
		if (error != 0) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "more interface locations than supported");
			return error;
		}

		/* Succeeded: the declared variable has its storage and interface recorded. */
		return 0;
	}

	/*
	 * A variable that is not an interface block takes its locations with
	 * its own interpolation.
	 */
	location = variable->location;
	if (type->kind != ID_TYPE_STRUCT) {
		error = drv_gpu_spirv_type_size(parser,
						type_id,
						0U,
						&scalars,
						&locations);
		if (error != 0) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "interface variable that is not made of scalars");
			return error;
		}

		/* Refuses an interface array that extends past the supported user locations. */
		if (location + locations > MAX_USER_LOCATION) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "interface location past the ones supported");
			return error;
		}

		/*
		 * A matrix or an array counts four components to each of its
		 * locations.
		 */
		if (locations > 1U)
			components = 4U;
		error = drv_gpu_spirv_add_io_run(parser,
						 is_input,
						 location,
						 locations,
						 components,
						 variable->flat,
						 variable->noperspective);
		if (error != 0) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "more interface locations than supported");
			return error;
		}

		/* Succeeded: the interface array's scalar slots are mapped. */
		return 0;
	}

	/*
	 * An interface block takes its members' locations in order, each with
	 * its own interpolation.
	 */
	for (member = 0U; member < type->count; member++) {
		error = drv_gpu_spirv_type_size(parser,
						type->member_type[member],
						1U,
						&scalars,
						&locations);
		if (error != 0) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error =
			    drv_gpu_spirv_refuse(parser,
						 opcode,
						 offset,
						 "interface block member that "
						 "is not made of scalars");
			return error;
		}

		/* Refuses a structure member that extends past the supported user locations. */
		if (location + locations > MAX_USER_LOCATION) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "interface location past the ones supported");
			return error;
		}

		/* Appends the member's input or output locations in declaration order. */
		error = drv_gpu_spirv_add_io_run(
		    parser,
		    is_input,
		    location,
		    locations,
		    4U,
		    variable->flat | type->member_flat[member],
		    variable->noperspective |
			type->member_noperspective[member]);
		if (error != 0) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "more interface locations than supported");
			return error;
		}

		/* Places the next member after the locations this member occupies. */
		location += locations;
	}

	/* Succeeded: every location of the block is listed. */
	return 0;
}

/*
 * Appends `count` locations from `location` on to the input or the output
 * list, each of `components` components with the interpolation given.
 * Returns ENOSPC when the list has no room for them.
 */
static int
drv_gpu_spirv_add_io_run(
	struct drv_gpu_spirv_parser *parser,
	int is_input,
	uint32_t location,
	uint32_t count,
	uint32_t components,
	uint32_t flat,
	uint32_t noperspective)
{
	struct drv_gpu_shader_ir_io *list;
	uint32_t *used;
	uint32_t index;

	/* Picks the input or the output list. */
	if (is_input != 0) {
		list = parser->ir->inputs;
		used = &parser->ir->input_count;
	} else {
		list = parser->ir->outputs;
		used = &parser->ir->output_count;
	}

	/* A list without room for the run refuses it. */
	if (*used + count > parser->io_capacity)
		return ENOSPC;

	/* Records each location of the run. */
	for (index = 0U; index < count; index++) {
		list[*used].location = location + index;
		list[*used].components = components;
		list[*used].type = 0U;
		list[*used].flat = flat;
		list[*used].noperspective = noperspective;
		(*used)++;
	}

	/* Succeeded: the run is listed. */
	return 0;
}

/*
 * Adds a uniform slot for a variable id: a sampled image, or a uniform
 * block whose read range the loads widen (none read yet).
 */
static void
drv_gpu_spirv_add_uniform(
	struct drv_gpu_spirv_parser *parser,
	uint32_t id,
	uint32_t kind)
{
	struct drv_gpu_shader_ir_uniform *slot;

	/* Takes the next slot of the uniform list. */
	slot = &parser->ir->uniforms[parser->ir->uniform_count];
	parser->ir->uniform_count++;

	/* Records the descriptor set and binding of the resource. */
	slot->set = parser->ids[id].set;
	slot->binding = parser->ids[id].binding;
	slot->kind = kind;
	slot->offset = 0U;
	slot->size = 0U;

	/* Succeeded: the bound resource is recorded in the shader interface. */
	return;
}

/* Lowers the body of the entry function. */
static int
drv_gpu_spirv_pass_body(
	struct drv_gpu_spirv_parser *parser)
{
	int error;
	const uint32_t *word;
	uint32_t offset;
	uint32_t count;
	uint32_t opcode;
	uint32_t functions;
	int in_function;

	/*
	 * Walks every instruction after the header, lowering those inside the
	 * function.
	 */
	functions = 0U;
	in_function = 0;
	offset = SPIRV_HEADER_WORDS;
	while (offset < parser->words) {
		/* Decodes the word count and the opcode of the instruction. */
		word = parser->code + offset;
		count = word[0] >> 16;
		opcode = word[0] & 0xFFFFU;

		/*
		 * An instruction must have a length and end inside the module.
		 */
		if (count == 0U || offset + count > parser->words)
			return EINVAL;

		/*
		 * A function opens the body; only one is accepted, since calls
		 * are not lowered.
		 */
		if (opcode == OP_FUNCTION) {
			if (in_function != 0)
				return EINVAL;
			functions++;
			if (functions > 1U) {
				/*
				 * Records the unsupported instruction in the
				 * caller's diagnostic.
				 */
				error = drv_gpu_spirv_refuse(
				    parser,
				    opcode,
				    offset,
				    "more than one function (calls are not "
				    "lowered)");
				return error;
			}

			/* Marks the entry function body as the instruction stream to lower. */
			in_function = 1;
			offset += count;
			continue;
		}

		/* Module-level instructions were handled by the first pass. */
		if (in_function == 0) {
			offset += count;
			continue;
		}

		/*
		 * The end of the function closes the body, after a terminated
		 * block and every construct's merge.
		 */
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
		error =
		    drv_gpu_spirv_lower(parser, word, count, opcode, offset);
		if (error != 0)
			return error;

		/* Advances beyond the fully lowered function instruction. */
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
drv_gpu_spirv_lower(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_loop *loop;
	const char *refusal;

	/*
	 * Debug instructions carry no execution semantics, wherever they sit.
	 */
	if (opcode == OP_NOP ||
	    opcode == OP_LINE ||
	    opcode == OP_NO_LINE)
		return 0;

	/* A label opens the next block. */
	if (opcode == OP_LABEL) {
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_label(parser,
						  word,
						  count,
						  opcode,
						  offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;
	}

	/*
	 * Every other instruction belongs to an open block, before its
	 * terminator.
	 */
	if (parser->block == 0U || parser->terminated != 0)
		return EINVAL;

	/*
	 * A skippable region ends before the block's merge declaration or
	 * terminator, which lower edges and loop moves.
	 */
	if (opcode == OP_SELECTION_MERGE ||
	    opcode == OP_LOOP_MERGE ||
	    opcode == OP_BRANCH ||
	    opcode == OP_BRANCH_CONDITIONAL ||
	    opcode == OP_SWITCH ||
	    opcode == OP_RETURN ||
	    opcode == OP_KILL ||
	    opcode == OP_UNREACHABLE)
		drv_gpu_spirv_skip_close(parser);

	/*
	 * A loop's body starts at the first instruction of its header that is
	 * not a phi.
	 */
	if (parser->loop_depth != 0U && opcode != OP_PHI) {
		loop = &parser->loops[parser->loop_depth - 1U];
		if (loop->begun == 0 && loop->header == parser->block)
			drv_gpu_spirv_loop_begin(parser, loop);
	}

	/* A compute shader refuses what its dispatch cannot give it. */
	if (parser->ir->stage == DRV_GPU_STAGE_COMPUTE) {
		refusal = drv_gpu_spirv_compute_refusal(opcode);
		if (refusal != NULL) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(parser,
						     opcode,
						     offset,
						     refusal);
			return error;
		}
	}

	/* Dispatches on the instruction. */
	switch (opcode) {
	case OP_RETURN:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_return(parser, opcode, offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_UNREACHABLE:
		/* No channel gets here: the block simply ends. */
		parser->terminated = 1;
		return 0;

	case OP_BRANCH:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_branch(parser,
						   word,
						   count,
						   opcode,
						   offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_BRANCH_CONDITIONAL:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_branch_conditional(parser,
							       word,
							       count,
							       opcode,
							       offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_SELECTION_MERGE:
		/*
		 * The next terminator is a selection header's; its construct
		 * ends at this merge block.
		 */
		if (count < 3U)
			return EINVAL;
		parser->pending_merge = word[1];
		return 0;

	case OP_LOOP_MERGE:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_loop_merge(parser,
						       word,
						       count,
						       opcode,
						       offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_KILL:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_kill(parser, opcode, offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_PHI:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_phi(parser,
						word,
						count,
						opcode,
						offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_SWITCH:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_switch(parser,
						   word,
						   count,
						   opcode,
						   offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_FDIV:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_divide(parser,
						   word,
						   count,
						   opcode,
						   offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_FMOD:
	case OP_FREM:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_float_remainder(parser,
							    word,
							    count,
							    opcode,
							    offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

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
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_compare(parser,
						    word,
						    count,
						    opcode,
						    offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

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
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_integer_compare(parser,
							    word,
							    count,
							    opcode,
							    offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_LOGICAL_AND:
	case OP_LOGICAL_OR:
	case OP_LOGICAL_NOT:
	case OP_LOGICAL_EQUAL:
	case OP_LOGICAL_NOT_EQUAL:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_logical(parser,
						    word,
						    count,
						    opcode,
						    offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_SELECT:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_select(parser,
						   word,
						   count,
						   opcode,
						   offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_VARIABLE:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_variable(parser,
						     word,
						     count,
						     opcode,
						     offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_ACCESS_CHAIN:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_access_chain(parser,
							 word,
							 count,
							 opcode,
							 offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_LOAD:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_load(parser,
						 word,
						 count,
						 opcode,
						 offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_STORE:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_store(parser,
						  word,
						  count,
						  opcode,
						  offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

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
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_atomic(parser,
						   word,
						   count,
						   opcode,
						   offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_ATOMIC_LOAD:
	case OP_ATOMIC_STORE:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_atomic_access(parser,
							  word,
							  count,
							  opcode,
							  offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_ARRAY_LENGTH:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_array_length(parser,
							 word,
							 count,
							 opcode,
							 offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_CONTROL_BARRIER:
	case OP_MEMORY_BARRIER:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_barrier(parser,
						    word,
						    count,
						    opcode,
						    offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_EMIT_VERTEX:
	case OP_END_PRIMITIVE:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_emit(parser, opcode, offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_FADD:
	case OP_FSUB:
	case OP_FMUL:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_arithmetic(parser,
						       word,
						       count,
						       opcode,
						       offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

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
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_integer(parser,
						    word,
						    count,
						    opcode,
						    offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_SNEGATE:
	case OP_NOT:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_integer_unary(parser,
							  word,
							  count,
							  opcode,
							  offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_CONVERT_F_TO_U:
	case OP_CONVERT_F_TO_S:
	case OP_CONVERT_S_TO_F:
	case OP_CONVERT_U_TO_F:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_convert(parser,
						    word,
						    count,
						    opcode,
						    offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_S_CONVERT:
	case OP_U_CONVERT:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_width_convert(parser,
							  word,
							  count,
							  opcode,
							  offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_BITCAST:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_bitcast(parser,
						    word,
						    count,
						    opcode,
						    offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_VECTOR_TIMES_SCALAR:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_vector_times_scalar(parser,
								word,
								count,
								opcode,
								offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_MATRIX_TIMES_SCALAR:
	case OP_VECTOR_TIMES_MATRIX:
	case OP_MATRIX_TIMES_VECTOR:
	case OP_MATRIX_TIMES_MATRIX:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_matrix_product(parser,
							   word,
							   count,
							   opcode,
							   offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_TRANSPOSE:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_transpose(parser,
						      word,
						      count,
						      opcode,
						      offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_OUTER_PRODUCT:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_outer_product(parser,
							  word,
							  count,
							  opcode,
							  offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_FNEGATE:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_negate(parser,
						   word,
						   count,
						   opcode,
						   offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_DOT:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_dot(parser,
						word,
						count,
						opcode,
						offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_COMPOSITE_CONSTRUCT:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_construct(parser,
						      word,
						      count,
						      opcode,
						      offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_COMPOSITE_EXTRACT:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_extract(parser,
						    word,
						    count,
						    opcode,
						    offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_COMPOSITE_INSERT:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_insert(parser,
						   word,
						   count,
						   opcode,
						   offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_COPY_OBJECT:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_copy(parser,
						 word,
						 count,
						 opcode,
						 offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_VECTOR_SHUFFLE:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_shuffle(parser,
						    word,
						    count,
						    opcode,
						    offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_EXT_INST:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_extended(parser,
						     word,
						     count,
						     opcode,
						     offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_IMAGE_SAMPLE_IMPLICIT_LOD:
	case OP_IMAGE_SAMPLE_EXPLICIT_LOD:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_sample(parser,
						   word,
						   count,
						   opcode,
						   offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_IMAGE_SAMPLE_DREF_IMPLICIT_LOD:
	case OP_IMAGE_SAMPLE_DREF_EXPLICIT_LOD:
	case OP_IMAGE_SAMPLE_PROJ_IMPLICIT_LOD:
	case OP_IMAGE_SAMPLE_PROJ_EXPLICIT_LOD:
	case OP_IMAGE_SAMPLE_PROJ_DREF_IMPLICIT_LOD:
	case OP_IMAGE_SAMPLE_PROJ_DREF_EXPLICIT_LOD:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_texture(parser,
						    word,
						    count,
						    opcode,
						    offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_IMAGE_FETCH:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_fetch(parser,
						  word,
						  count,
						  opcode,
						  offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_IMAGE:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_image(parser,
						  word,
						  count,
						  opcode,
						  offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_IMAGE_QUERY_SIZE_LOD:
	case OP_IMAGE_QUERY_SIZE:
	case OP_IMAGE_QUERY_LEVELS:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_query(parser,
						  word,
						  count,
						  opcode,
						  offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	case OP_DPDX:
	case OP_DPDY:
	case OP_FWIDTH:
	case OP_DPDX_FINE:
	case OP_DPDY_FINE:
	case OP_FWIDTH_FINE:
	case OP_DPDX_COARSE:
	case OP_DPDY_COARSE:
	case OP_FWIDTH_COARSE:
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_derivative(parser,
						       word,
						       count,
						       opcode,
						       offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;

	default:
		break;
	}

	/*
	 * A skipped instruction would be a different shader, so it is refused.
	 */
	error = drv_gpu_spirv_refuse(
	    parser,
	    opcode,
	    offset,
	    "instruction with execution semantics is not lowered");
	return error;
}

/*
 * Lowers a local variable: not memory, a run of "currently stored" scalars
 * (none yet) -- a scalar, a vector, a matrix, or an array or a structure of
 * them, flattened (see drv_gpu_spirv_type_size()).
 */
static int
drv_gpu_spirv_lower_variable(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *record;
	struct drv_gpu_spirv_id *type;
	uint32_t components;

	/* Resolves the variable and its pointer type. */
	record = NULL;
	type = NULL;
	if (count >= 4U) {
		record = drv_gpu_spirv_id(parser, word[2]);
		type = drv_gpu_spirv_id(parser, word[1]);
	}

	/* The variable must be a fresh id with a pointer type. */
	if (record == NULL || type == NULL)
		return EINVAL;
	if (type->kind != ID_TYPE_POINTER || record->kind != ID_NONE)
		return EINVAL;

	/*
	 * A local with an initializer, or in another storage class, is refused.
	 */
	if (word[3] != SC_FUNCTION || count > 4U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error =
		    drv_gpu_spirv_refuse(parser,
					 opcode,
					 offset,
					 "local variable with an initializer "
					 "or a non-Function storage class");
		return error;
	}

	/* Only what is made of scalars can be tracked as scalars. */
	components = drv_gpu_spirv_aggregate_scalars(parser, type->type);
	if (components == 0U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "local variable that is not made of scalars");
		return error;
	}

	/* Records the local as a pointer to itself. */
	record->kind = ID_VARIABLE;
	record->storage = SC_FUNCTION;
	record->ptr_kind = PTR_LOCAL;
	record->type = word[1];
	record->var = word[2];
	record->pointee = type->type;
	record->member = -1;
	record->component = -1;
	record->dynamic_index = NO_VALUE;

	/* Gives it its scalars, none stored until a store. */
	error = drv_gpu_spirv_variable_slots(parser, record, components);
	if (error == ENOMEM)
		return error;
	if (error != 0) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "local variable larger than supported");
		return error;
	}

	/* Succeeded: the local is ready for stores. */
	return 0;
}

/*
 * Lowers OpAccessChain.
 *
 * Into push constants and uniform blocks: struct members, array elements
 * (one dynamic index at most), matrix columns and vector components, as a
 * byte offset.  Into locals, inputs and outputs: structure members, array
 * elements, matrix columns and vector components, as the first scalar or
 * interface slot addressed (one dynamic index into a local array at most).
 */
static int
drv_gpu_spirv_lower_access_chain(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *record;
	struct drv_gpu_spirv_id *base;
	struct drv_gpu_spirv_id *index_record;
	struct drv_gpu_spirv_id *pointee;
	uint32_t index;

	/* Resolves the result and the pointer the chain starts from. */
	record = NULL;
	base = NULL;
	if (count >= 4U) {
		record = drv_gpu_spirv_id(parser, word[2]);
		base = drv_gpu_spirv_id(parser, word[3]);
	}

	/* The result must be fresh and the base must be a pointer. */
	if (record == NULL ||
	    base == NULL ||
	    record->kind != ID_NONE)
		return EINVAL;
	if (base->kind != ID_VARIABLE && base->kind != ID_POINTER)
		return EINVAL;

	/* The chain starts as a copy of the base pointer. */
	*record = *base;
	record->kind = ID_POINTER;

	/* Each index selects one level of what the pointer addresses. */
	for (index = 4U; index < count; index++) {
		/* Resolves the index and what the pointer addresses so far. */
		index_record = drv_gpu_spirv_id(parser, word[index]);
		pointee = drv_gpu_spirv_id(parser, record->pointee);
		if (index_record == NULL || pointee == NULL)
			return EINVAL;

		/*
		 * Memory the draw delivers is addressed in bytes; a compute
		 * built-in by component; a geometry shader's per-vertex input
		 * first by its input vertex; anything else in scalars.
		 */
		if (record->ptr_kind == PTR_PUSH ||
		    record->ptr_kind == PTR_UBO ||
		    record->ptr_kind == PTR_SSBO) {
			error = drv_gpu_spirv_chain_block(parser,
							  record,
							  pointee,
							  index_record,
							  word[index],
							  opcode,
							  offset);
		} else if (record->ptr_kind == PTR_SYSTEM) {
			error = drv_gpu_spirv_chain_system(parser,
							   record,
							   pointee,
							   index_record,
							   opcode,
							   offset);
		} else if (record->ptr_kind == PTR_SHARED) {
			error = drv_gpu_spirv_chain_shared(parser,
							   record,
							   pointee,
							   index_record,
							   word[index],
							   opcode,
							   offset);
		} else if (record->ptr_kind == PTR_VERTEX &&
			   record->vertex_chosen == 0U) {
			error = drv_gpu_spirv_chain_vertex(parser,
							   record,
							   pointee,
							   index_record,
							   word[index],
							   opcode,
							   offset);
		} else {
			error = drv_gpu_spirv_chain_scalars(parser,
							    record,
							    pointee,
							    index_record,
							    word[index],
							    opcode,
							    offset);
		}

		/* Reports why the index could not be followed. */
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
drv_gpu_spirv_chain_block(
	struct drv_gpu_spirv_parser *parser,
	struct drv_gpu_spirv_id *record,
	struct drv_gpu_spirv_id *pointee,
	struct drv_gpu_spirv_id *index_record,
	uint32_t index_id,
	uint32_t opcode,
	uint32_t offset)
{
	uint32_t index_width;
	uint32_t constant_scalar;
	int error;
	struct drv_gpu_spirv_id *column;
	uint32_t scalars[MAX_COMPONENTS];
	uint32_t scalar_count;
	uint32_t selected;
	uint32_t before;
	uint32_t after;
	int constant;

	/*
	 * A constant index names its element now; any other is an IR integer.
	 */
	constant = 0;
	selected = 0U;
	if (index_record->kind == ID_CONSTANT) {
		constant = 1;
		selected = index_record->constant;
	}

	/*
	 * An element of an array may be selected at run time; nothing else may.
	 */
	if (constant == 0 && pointee->kind != ID_TYPE_ARRAY) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error =
		    drv_gpu_spirv_refuse(parser,
					 opcode,
					 offset,
					 "access chain with a dynamic index "
					 "into something that is not an array");
		return error;
	}

	/* Steps by what the pointer addresses. */
	switch (pointee->kind) {
	case ID_TYPE_STRUCT:
		/*
		 * A member: its Offset, and the layout its decorations give a
		 * matrix.
		 */
		if (selected >= pointee->count)
			return EINVAL;
		if (pointee->member_has_offset[selected] == 0U) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "block member without an Offset");
			return error;
		}

		/* Adds the selected member's decorated byte offset to the access chain. */
		record->byte_offset += pointee->member_offset[selected];
		record->member = (int32_t)selected;
		record->matrix_stride = pointee->member_matrix_stride[selected];
		record->row_major = pointee->member_row_major[selected];
		record->component_stride = 4U;
		record->pointee = pointee->member_type[selected];
		break;

	case ID_TYPE_ARRAY:
		/*
		 * An element: the array's stride, which the layout must give.
		 */
		if (pointee->stride == 0U) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "array without an ArrayStride in a block");
			return error;
		}

		/* Resolves a constant index before choosing between fixed and dynamic addressing. */
		if (constant != 0) {
			if (pointee->length != 0U &&
			    selected >= pointee->length)
				return EINVAL;
			record->byte_offset += selected * pointee->stride;
		} else {
			/*
			 * One dynamic index, over an array short enough to
			 * select from; a storage buffer's is an address, so any
			 * number of them add up to one byte offset.
			 */
			if (record->dynamic_index != NO_VALUE &&
			    record->ptr_kind != PTR_SSBO) {
				/*
				 * Records the unsupported instruction in the
				 * caller's diagnostic.
				 */
				error = drv_gpu_spirv_refuse(
				    parser,
				    opcode,
				    offset,
				    "access chain with more than one dynamic "
				    "index");
				return error;
			}

			/* Refuses an unsupported dynamic array while retaining storage-buffer addressing. */
			if (record->ptr_kind != PTR_SSBO &&
			    (pointee->length == 0U ||
			    pointee->length > MAX_DYNAMIC_ELEMENTS)) {
				/*
				 * Records the unsupported instruction in the
				 * caller's diagnostic.
				 */
				error = drv_gpu_spirv_refuse(
				    parser,
				    opcode,
				    offset,
				    "dynamic index into an array longer than "
				    "supported");
				return error;
			}

			/* Resolves the scalar components of the selected array element. */
			scalar_count =
			    drv_gpu_spirv_operand(parser, index_id, scalars);
			if (scalar_count != 1U) {
				/*
				 * Records the unsupported instruction in the
				 * caller's diagnostic.
				 */
				error = drv_gpu_spirv_refuse(
				    parser,
				    opcode,
				    offset,
				    "dynamic index that is not an integer "
				    "scalar");
				return error;
			}

			/*
			 * A 16-bit index made whole, signed as SPIR-V's indices
			 * are (ws031-p039).
			 */
			/*
			 * Resolves the scalar operand before constructing its
			 * enclosing operation.
			 */
			index_width =
			    drv_gpu_spirv_operand_int_width(parser, index_id);
			scalars[0] = drv_gpu_spirv_extend16(parser,
							    scalars[0],
							    index_width,
							    1);

			/*
			 * A storage buffer's second dynamic index folds the
			 * first into bytes, and itself after it (ws101-p002).
			 */
			if (record->dynamic_index != NO_VALUE) {

				/*
				 * Resolves the scalar operand before
				 * constructing its enclosing operation.
				 */
				constant_scalar =
				    drv_gpu_spirv_integer_constant(
					parser,
					record->dynamic_stride);
				before = drv_gpu_spirv_emit_value(
				    parser,
				    DRV_GPU_IR_IMUL,
				    record->dynamic_index,
				    constant_scalar);

				/*
				 * Resolves the scalar operand before
				 * constructing its enclosing operation.
				 */
				constant_scalar =
				    drv_gpu_spirv_integer_constant(
					parser,
					pointee->stride);
				after =
				    drv_gpu_spirv_emit_value(parser,
							     DRV_GPU_IR_IMUL,
							     scalars[0],
							     constant_scalar);
				record->dynamic_index =
				    drv_gpu_spirv_emit_value(parser,
							     DRV_GPU_IR_IADD,
							     before,
							     after);
				record->dynamic_stride = 1U;
				record->dynamic_length = 0U;
			} else {
				record->dynamic_index = scalars[0];
				record->dynamic_stride = pointee->stride;
				record->dynamic_length = pointee->length;
			}
		}

		/* What follows the element is addressed from it. */
		record->component_stride = 4U;
		record->pointee = pointee->type;
		break;

	case ID_TYPE_MATRIX:
		/*
		 * A column: MatrixStride apart when column-major, four bytes
		 * apart when row-major.
		 */
		column = drv_gpu_spirv_id(parser, pointee->type);
		if (column == NULL || selected >= pointee->count)
			return EINVAL;
		if (record->matrix_stride == 0U) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "matrix without a MatrixStride in a block");
			return error;
		}

		/* Applies the decorated matrix row or column stride. */
		if (record->row_major != 0U) {
			record->byte_offset += selected * 4U;
			record->component_stride = record->matrix_stride;
		} else {
			record->byte_offset += selected * record->matrix_stride;
			record->component_stride = 4U;
		}

		/* What follows the column is addressed from it. */
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
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "access chain into a scalar");
		return error;
	}

	/* Succeeded: the pointer addresses the selected part. */
	return 0;
}

/*
 * Steps a pointer into a local, an input or an output one index down.
 *
 * A local is addressed by its scalars in order, an input or an output by
 * its interface slots, four to a location (drv_gpu_spirv_io_map()), so
 * `component` is the first scalar or slot the pointer addresses.  A
 * structure member, an array element, a matrix column or a vector component
 * is selected by a constant index; a local's array element, matrix column
 * or vector component may also be selected at run time
 * (drv_gpu_spirv_chain_dynamic()), which the load or the store then resolves.
 */
static int
drv_gpu_spirv_chain_scalars(
	struct drv_gpu_spirv_parser *parser,
	struct drv_gpu_spirv_id *record,
	struct drv_gpu_spirv_id *pointee,
	struct drv_gpu_spirv_id *index_record,
	uint32_t index_id,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	uint32_t next_type;
	uint32_t scalar_offset;
	uint32_t io_offset;
	uint32_t first;

	/* The first scalar or slot addressed so far. */
	first = 0U;
	if (record->component >= 0)
		first = (uint32_t)record->component;

	/*
	 * An index known only at run time is resolved by the load or the store.
	 */
	if (index_record->kind != ID_CONSTANT) {
		error = drv_gpu_spirv_chain_dynamic(parser,
						    record,
						    pointee,
						    index_id,
						    opcode,
						    offset);
		if (error != 0)
			return error;
		return 0;
	}

	/*
	 * The first structure member selected is the one an output block's
	 * builtin is on.
	 */
	if (pointee->kind == ID_TYPE_STRUCT && record->member < 0)
		record->member = (int32_t)index_record->constant;

	/* Finds the part the constant index selects. */
	error = drv_gpu_spirv_type_step(parser,
					record->pointee,
					index_record->constant,
					&next_type,
					&scalar_offset,
					&io_offset,
					opcode,
					offset);
	if (error != 0)
		return error;

	/* A local counts scalars; an input or an output interface slots. */
	if (record->ptr_kind == PTR_LOCAL) {
		record->component = (int32_t)(first + scalar_offset);
	} else {
		record->component = (int32_t)(first + io_offset);
	}

	/* The pointer now addresses the part. */
	record->pointee = next_type;

	/* Succeeded: the pointer names the selected part. */
	return 0;
}

/*
 * Steps a pointer into a local one index known only at run time down: an
 * array's element, a matrix's column or a vector's component.  The pointer
 * keeps the index as the run of candidate parts it may pick: the IR integer
 * of the part, the scalars between two parts and how many parts there are.
 * A second such index in the chain folds both into one flat scalar offset
 * (the first index times its stride plus the second times its), its
 * candidates every scalar of the span the two may reach; more candidates
 * than MAX_LOCAL_DYNAMIC are refused.
 */
static int
drv_gpu_spirv_chain_dynamic(
	struct drv_gpu_spirv_parser *parser,
	struct drv_gpu_spirv_id *record,
	struct drv_gpu_spirv_id *pointee,
	uint32_t index_id,
	uint32_t opcode,
	uint32_t offset)
{
	uint32_t index_width;
	int error;
	struct drv_gpu_spirv_id *column;
	uint32_t scalars[MAX_COMPONENTS];
	uint32_t scalar_count;
	uint32_t element_scalars;
	uint32_t element_locations;
	uint32_t length;
	uint32_t stride;
	uint32_t next_type;
	uint32_t span;
	uint32_t factor;
	uint32_t before;
	uint32_t after;

	/* Only a local's parts are chosen at run time. */
	if (record->ptr_kind != PTR_LOCAL) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error =
		    drv_gpu_spirv_refuse(parser,
					 opcode,
					 offset,
					 "access chain with a dynamic index "
					 "into something that is not a local");
		return error;
	}

	/*
	 * The parts the index chooses among: elements, columns or components.
	 */
	if (pointee->kind == ID_TYPE_ARRAY) {
		error = drv_gpu_spirv_type_size(parser,
						pointee->type,
						1U,
						&element_scalars,
						&element_locations);
		if (error != 0) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "array of elements that are not made of scalars");
			return error;
		}

		/* Uses the declared vector length to bound a dynamic component index. */
		length = pointee->length;
		stride = element_scalars;
		next_type = pointee->type;
	} else if (pointee->kind == ID_TYPE_MATRIX) {
		column = drv_gpu_spirv_id(parser, pointee->type);
		if (column == NULL)
			return EINVAL;
		length = pointee->count;
		stride = column->count;
		next_type = pointee->type;
	} else if (pointee->kind == ID_TYPE_VECTOR) {
		length = pointee->count;
		stride = 1U;
		next_type = pointee->type;
	} else {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "dynamic index into a scalar or a structure");
		return error;
	}

	/* An aggregate without parts is malformed. */
	if (length == 0U)
		return EINVAL;

	/* The index is one integer. */
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
		    "dynamic index that is not an integer scalar");
		return error;
	}

	/*
	 * A 16-bit index made whole, signed as SPIR-V's indices are
	 * (ws031-p039).
	 */
	/*
	 * Resolves the scalar operand before constructing its enclosing
	 * operation.
	 */
	index_width = drv_gpu_spirv_operand_int_width(parser, index_id);
	scalars[0] = drv_gpu_spirv_extend16(parser, scalars[0], index_width, 1);

	/* The first index of the chain picks among its parts as they are. */
	if (record->dynamic_index == NO_VALUE) {
		if (length > MAX_LOCAL_DYNAMIC) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "dynamic index into more parts than supported");
			return error;
		}

		/* Carries the scalar index into the dynamic pointer description. */
		record->dynamic_index = scalars[0];
		record->dynamic_stride = stride;
		record->dynamic_length = length;
		record->pointee = next_type;
		return 0;
	}

	/*
	 * A second one folds both into one scalar offset, which picks among
	 * every scalar of the span they reach.
	 */
	span = (record->dynamic_length - 1U) * record->dynamic_stride +
	       (length - 1U) * stride + 1U;
	if (span > MAX_LOCAL_DYNAMIC) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "dynamic index into more parts than supported");
		return error;
	}

	/* Emits the stride multiplier for the combined dynamic index. */
	factor = drv_gpu_spirv_integer_constant(parser, record->dynamic_stride);
	before = drv_gpu_spirv_emit_value(parser,
					  DRV_GPU_IR_IMUL,
					  record->dynamic_index,
					  factor);
	factor = drv_gpu_spirv_integer_constant(parser, stride);
	after = drv_gpu_spirv_emit_value(parser,
					 DRV_GPU_IR_IMUL,
					 scalars[0],
					 factor);
	record->dynamic_index =
	    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_IADD, before, after);
	record->dynamic_stride = 1U;
	record->dynamic_length = span;
	record->pointee = next_type;

	/* Succeeded: the pointer names the part the indices choose. */
	return 0;
}

/*
 * Lowers OpLoad from a sampler, a local, an output, an input, push constants or
 * a uniform block.
 */
static int
drv_gpu_spirv_lower_load(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *base;
	struct drv_gpu_spirv_id *variable;
	struct drv_gpu_spirv_id *record;
	uint32_t components;
	uint32_t loaded;

	/* Resolves the pointer loaded through. */
	base = NULL;
	if (count >= 4U)
		base = drv_gpu_spirv_id(parser, word[3]);
	if (base == NULL)
		return EINVAL;
	if (base->kind != ID_VARIABLE && base->kind != ID_POINTER)
		return EINVAL;

	/* Resolves the variable the pointer leads to. */
	variable = drv_gpu_spirv_id(parser, base->var);
	if (variable == NULL)
		return EINVAL;

	/* A sampler load names the combined image sampler; it emits nothing. */
	if (base->ptr_kind == PTR_SAMPLER) {
		record = drv_gpu_spirv_id(parser, word[2]);
		if (record == NULL || record->kind != ID_NONE)
			return EINVAL;
		record->kind = ID_SAMPLED_IMAGE;
		record->binding = variable->binding;
		record->set = variable->set;
		record->type = base->pointee;
		return 0;
	}

	/*
	 * Anything else must load what is made of scalars, no more than a value
	 * holds, as much as the pointee holds.
	 */
	components = drv_gpu_spirv_aggregate_scalars(parser, base->pointee);
	if (components == 0U || components > MAX_COMPONENTS) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error =
		    drv_gpu_spirv_refuse(parser,
					 opcode,
					 offset,
					 "load of something that is not made "
					 "of scalars, or larger than a value");
		return error;
	}

	/* Resolves an aggregate load using its scalar storage size. */
	loaded = drv_gpu_spirv_aggregate_scalars(parser, word[1]);
	if (components != loaded) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error =
		    drv_gpu_spirv_refuse(parser,
					 opcode,
					 offset,
					 "load of something that is not made "
					 "of scalars, or larger than a value");
		return error;
	}

	/* Loads by what the pointer addresses. */
	if (base->ptr_kind == PTR_LOCAL) {
		error = drv_gpu_spirv_lower_load_local(parser,
						       word,
						       base,
						       variable,
						       components,
						       opcode,
						       offset);
	} else if (base->ptr_kind == PTR_OUTPUT ||
		   base->ptr_kind == PTR_OUTPUT_BLOCK) {
		error = drv_gpu_spirv_lower_load_output(parser,
							word,
							base,
							variable,
							components,
							opcode,
							offset);
	} else if (base->ptr_kind == PTR_INPUT) {
		error = drv_gpu_spirv_lower_load_input(parser,
						       word,
						       base,
						       variable,
						       components,
						       opcode,
						       offset);
	} else if (base->ptr_kind == PTR_PUSH || base->ptr_kind == PTR_UBO) {
		error = drv_gpu_spirv_lower_load_block(parser,
						       word,
						       base,
						       variable,
						       opcode,
						       offset);
	} else if (base->ptr_kind == PTR_SSBO) {
		error = drv_gpu_spirv_lower_storage(parser,
						    word,
						    base,
						    variable,
						    NULL,
						    components,
						    opcode,
						    offset);
	} else if (base->ptr_kind == PTR_SYSTEM) {
		error = drv_gpu_spirv_lower_load_system(parser,
							word,
							base,
							variable,
							components,
							opcode,
							offset);
	} else if (base->ptr_kind == PTR_SHARED) {
		error = drv_gpu_spirv_lower_shared(parser,
						   word,
						   base,
						   NULL,
						   components,
						   opcode,
						   offset);
	} else if (base->ptr_kind == PTR_VERTEX) {
		error = drv_gpu_spirv_lower_load_vertex(parser,
							word,
							base,
							variable,
							components,
							opcode,
							offset);
	} else {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "load through a pointer that is not an input, push "
		    "constant, uniform block, sampler or local");
		return error;
	}

	/* Reports why the load could not be lowered. */
	if (error != 0)
		return error;

	/* Succeeded: the loaded value names its scalars. */
	return 0;
}

/*
 * Lowers a load from a local: store-to-load forwarding, the scalars
 * currently stored in it.  Through a dynamic index every candidate part's
 * scalars are read and the ones the index names are selected channel by
 * channel, as a block load selects (drv_gpu_spirv_lower_load_block()); a
 * candidate never stored reads as zero bits, since no channel may read it.
 */
static int
drv_gpu_spirv_lower_load_local(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	const struct drv_gpu_spirv_id *pointer,
	const struct drv_gpu_spirv_id *variable,
	uint32_t components,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *record;
	uint32_t selects[MAX_LOCAL_DYNAMIC];
	uint32_t first;
	uint32_t elements;
	uint32_t element;
	uint32_t index;
	uint32_t value;
	uint32_t candidate;

	/* The first scalar the pointer addresses. */
	first = 0U;
	if (pointer->component >= 0)
		first = (uint32_t)pointer->component;

	/*
	 * Under a dynamic index, compares the index with each element number
	 * past the first once.
	 */
	elements = 1U;
	if (pointer->dynamic_index != NO_VALUE) {
		elements = pointer->dynamic_length;
		for (element = 1U; element < elements; element++) {
			selects[element] =
			    drv_gpu_spirv_index_is(parser,
						   pointer->dynamic_index,
						   element);
		}
	}

	/* The loaded value names scalars already made. */
	record = drv_gpu_spirv_result(parser, word[2], word[1], components, 0);
	if (record == NULL)
		return EINVAL;

	/*
	 * Reads each scalar: the first part's, then each later part's where the
	 * index names it.
	 */
	for (index = 0U; index < components; index++) {
		value = drv_gpu_spirv_local_scalar(parser,
						   variable,
						   first + index,
						   elements > 1U);
		if (value == NO_VALUE) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "load of a local or output component that was "
			    "never stored");
			return error;
		}

		/* Selects the addressed array element from the already loaded candidates. */
		for (element = 1U; element < elements; element++) {
			candidate = drv_gpu_spirv_local_scalar(
			    parser,
			    variable,
			    first + element * pointer->dynamic_stride + index,
			    1);
			if (candidate == NO_VALUE)
				return EINVAL;
			value = drv_gpu_spirv_select_value(parser,
							   selects[element],
							   candidate,
							   value);
		}

		/* The load names the chosen scalar. */
		record->comp[index] = value;
	}

	/* Succeeded: the load names the stored scalars. */
	return 0;
}

/*
 * Returns the value a local's slot holds; a slot past the local, or never
 * stored, is NO_VALUE -- or, when `zero_if_unstored` is set, a slot never
 * stored reads as zero bits (a dynamic index's candidate no channel reads).
 */
static uint32_t
drv_gpu_spirv_local_scalar(
	struct drv_gpu_spirv_parser *parser,
	const struct drv_gpu_spirv_id *variable,
	uint32_t index,
	int zero_if_unstored)
{
	uint32_t *slot;
	uint32_t zero;

	/* A slot past the local holds nothing. */
	slot = drv_gpu_spirv_variable_slot(parser, variable, index);
	if (slot == NULL)
		return NO_VALUE;

	/* A stored slot holds its value. */
	if (*slot != NO_VALUE)
		return *slot;

	/* A slot never stored holds nothing, or zero bits for a candidate. */
	if (zero_if_unstored == 0)
		return NO_VALUE;
	zero = drv_gpu_spirv_shared_constant(parser,
					     &parser->zero_value,
					     DRV_GPU_IR_CONST,
					     FLOAT_ZERO_BITS);

	/* Succeeded: the zero bits. */
	return zero;
}

/*
 * Lowers a load from an output: the value last written to each of its
 * interface slots (a shader may read back what it wrote).
 */
static int
drv_gpu_spirv_lower_load_output(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	const struct drv_gpu_spirv_id *pointer,
	const struct drv_gpu_spirv_id *variable,
	uint32_t components,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *record;
	uint32_t map[MAX_COMPONENTS];
	uint32_t map_count;
	uint32_t *slot;
	uint32_t first;
	uint32_t index;

	/* The first interface slot the pointer addresses. */
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
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "load of an output that is not made of scalars");
		return error;
	}

	/* The loaded value names scalars already made. */
	record = drv_gpu_spirv_result(parser, word[2], word[1], components, 0);
	if (record == NULL)
		return EINVAL;

	/* Each slot must have been written before it is read. */
	for (index = 0U; index < components; index++) {
		slot =
		    drv_gpu_spirv_variable_slot(parser, variable, map[index]);
		if (slot == NULL || *slot == NO_VALUE) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "load of a local or output component that was "
			    "never stored");
			return error;
		}

		/* Forwards the scalar stored in the selected local slot. */
		record->comp[index] = *slot;
	}

	/* Succeeded: the load names the written scalars. */
	return 0;
}

/*
 * Lowers a load from an input: one input load to each scalar, at the
 * location and component of its interface slot.  An input is floats (a
 * vertex attribute or an interpolated input), or integers or Booleans moved
 * bit for bit: a vertex attribute, a Flat fragment input (the setup's
 * constant, not interpolated) or gl_FrontFacing.  An aggregate is not
 * checked member by member: Vulkan has every integer fragment input Flat.
 */
static int
drv_gpu_spirv_lower_load_input(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	const struct drv_gpu_spirv_id *pointer,
	const struct drv_gpu_spirv_id *variable,
	uint32_t components,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *record;
	struct drv_gpu_shader_ir_inst *inst;
	uint32_t map[MAX_COMPONENTS];
	uint32_t map_count;
	uint32_t vector;
	uint32_t floats;
	uint32_t first;
	uint32_t index;
	int raw;
	int flat;

	/* The first interface slot the pointer addresses. */
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
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "load of an input that is not made of scalars");
		return error;
	}

	/*
	 * A vertex attribute, a Flat fragment input and gl_FrontFacing are
	 * moved bit for bit.
	 */
	raw = 0;
	if (parser->ir->stage == DRV_GPU_STAGE_VERTEX)
		raw = 1;
	if (variable->location == DRV_GPU_SHADER_LOCATION_FRONT_FACING)
		raw = 1;
	flat =
	    drv_gpu_spirv_input_flat(parser, variable->location + map[0] / 4U);
	if (flat != 0)
		raw = 1;

	/* A scalar or a vector interpolated must be floats. */
	vector = drv_gpu_spirv_value_components(parser, word[1]);
	floats = drv_gpu_spirv_float_components(parser, word[1]);
	if (vector != 0U &&
	    floats != components &&
	    raw == 0) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "load of an input that is not a float scalar or vector");
		return error;
	}

	/* Each scalar is a fresh value read from its location. */
	record = drv_gpu_spirv_result(parser, word[2], word[1], components, 1);
	if (record == NULL)
		return EINVAL;

	/* Emits one input load to each scalar. */
	for (index = 0U; index < components; index++) {
		inst = drv_gpu_spirv_emit(parser,
					  DRV_GPU_IR_LOAD_INPUT,
					  record->comp[index],
					  0U,
					  0U);
		if (inst != NULL) {
			inst->location = variable->location + map[index] / 4U;
			inst->component = map[index] % 4U;
		}
	}

	/* Succeeded: the load names the input's scalars. */
	return 0;
}

/* Reports whether the input of a location is Flat. */
static int
drv_gpu_spirv_input_flat(
	struct drv_gpu_spirv_parser *parser,
	uint32_t location)
{
	uint32_t index;

	/* Looks the location up among the inputs. */
	for (index = 0U; index < parser->ir->input_count; index++) {
		if (parser->ir->inputs[index].location != location)
			continue;
		if (parser->ir->inputs[index].flat != 0U)
			return 1;
	}

	/* Succeeded: the input is interpolated, or there is none. */
	return 0;
}

/*
 * Emits the Boolean of an integer value being equal to a constant number, and
 * returns it.
 */
static uint32_t
drv_gpu_spirv_index_is(
	struct drv_gpu_spirv_parser *parser,
	uint32_t index,
	uint32_t number)
{
	uint32_t constant;
	uint32_t equal;

	/* The number as an integer constant. */
	constant = drv_gpu_spirv_integer_constant(parser, number);

	/* Succeeded: the comparison. */
	equal =
	    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_IEQ, index, constant);
	return equal;
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
drv_gpu_spirv_lower_load_block(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	struct drv_gpu_spirv_id *pointer,
	struct drv_gpu_spirv_id *variable,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *record;
	struct drv_gpu_spirv_id *type;
	struct drv_gpu_spirv_id *column;
	struct drv_gpu_shader_ir_inst *inst;
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
	type = drv_gpu_spirv_id(parser, pointer->pointee);
	if (type == NULL)
		return EINVAL;
	columns = 1U;
	rows = 1U;
	if (type->kind == ID_TYPE_VECTOR) {
		rows = type->count;
	} else if (type->kind == ID_TYPE_MATRIX) {
		column = drv_gpu_spirv_id(parser, type->type);
		if (column == NULL)
			return EINVAL;
		columns = type->count;
		rows = column->count;
	}

	/*
	 * A structure or an array is loaded member by member, element by
	 * element, through its own chains.
	 */
	if (type->kind == ID_TYPE_STRUCT || type->kind == ID_TYPE_ARRAY) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "load of a structure or an array from a block");
		return error;
	}

	/* A matrix needs its stride to be read. */
	if (columns > 1U && pointer->matrix_stride == 0U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "matrix without a MatrixStride in a block");
		return error;
	}

	/* The loaded value is fresh scalars named below. */
	record =
	    drv_gpu_spirv_result(parser, word[2], word[1], columns * rows, 0);
	if (record == NULL)
		return EINVAL;

	/*
	 * Under a dynamic index, compares the index with each element number
	 * past the first once.
	 */
	elements = 1U;
	if (pointer->dynamic_index != NO_VALUE) {
		elements = pointer->dynamic_length;
		for (element = 1U; element < elements; element++) {
			number = drv_gpu_spirv_new_value(parser);
			inst = drv_gpu_spirv_emit(parser,
						  DRV_GPU_IR_ICONST,
						  number,
						  0U,
						  0U);
			if (inst != NULL)
				inst->immediate = element;
			selects[element] =
			    drv_gpu_spirv_emit_value(parser,
						     DRV_GPU_IR_IEQ,
						     pointer->dynamic_index,
						     number);
		}
	}

	/* Loads each scalar, column after column. */
	for (column_index = 0U; column_index < columns; column_index++) {
		for (row = 0U; row < rows; row++) {
			/*
			 * The scalar's byte in the value: by the matrix layout,
			 * or by the vector's stride.
			 */
			if (columns > 1U && pointer->row_major != 0U) {
				place = pointer->byte_offset +
					row * pointer->matrix_stride +
					column_index * 4U;
			} else if (columns > 1U) {
				place = pointer->byte_offset +
					column_index * pointer->matrix_stride +
					row * 4U;
			} else {
				place = pointer->byte_offset +
					row * pointer->component_stride;
			}

			/*
			 * The first element's word, then each later element's
			 * where the index names it.
			 */
			value = drv_gpu_spirv_load_word(parser,
							pointer,
							variable,
							place);
			for (element = 1U; element < elements; element++) {
				candidate = drv_gpu_spirv_load_word(
				    parser,
				    pointer,
				    variable,
				    place + element * pointer->dynamic_stride);
				merged = drv_gpu_spirv_new_value(parser);
				inst = drv_gpu_spirv_emit(parser,
							  DRV_GPU_IR_SELECT,
							  merged,
							  selects[element],
							  candidate);
				if (inst != NULL)
					inst->src[2] = value;
				value = merged;
			}

			/* The load names the chosen word. */
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
drv_gpu_spirv_load_word(
	struct drv_gpu_spirv_parser *parser,
	const struct drv_gpu_spirv_id *pointer,
	const struct drv_gpu_spirv_id *variable,
	uint32_t byte)
{
	struct drv_gpu_shader_ir_uniform *uniform;
	struct drv_gpu_shader_ir_inst *inst;
	uint32_t value;
	uint32_t end;

	/* A word starts on a four-byte boundary. */
	if ((byte & 3U) != 0U) {
		parser->error = EINVAL;
		return 0U;
	}

	/* A push constant widens the push bytes the shader reads. */
	value = drv_gpu_spirv_new_value(parser);
	if (pointer->ptr_kind == PTR_PUSH) {
		inst = drv_gpu_spirv_emit(parser,
					  DRV_GPU_IR_LOAD_PUSH,
					  value,
					  0U,
					  0U);
		if (inst != NULL)
			inst->immediate = byte;
		if (byte + 4U > parser->ir->push_bytes)
			parser->ir->push_bytes = byte + 4U;
		return value;
	}

	/* A uniform block word names its block. */
	inst = drv_gpu_spirv_emit(parser, DRV_GPU_IR_LOAD_UBO, value, 0U, 0U);
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

/*
 * Lowers a load from (`scalars` NULL) or a store to (`scalars` the stored
 * values) a storage buffer: one LOAD_STORAGE or STORE_STORAGE of each
 * scalar of a scalar or a vector, at the pointer's byte offset plus the
 * component's, plus the dynamic index times its stride.  A store under a
 * predicate carries it.
 */
static int
drv_gpu_spirv_lower_storage(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	const struct drv_gpu_spirv_id *pointer,
	const struct drv_gpu_spirv_id *variable,
	const uint32_t *scalars,
	uint32_t components,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *record;
	struct drv_gpu_spirv_id *type;
	struct drv_gpu_shader_ir_inst *inst;
	uint32_t index;
	uint32_t place;
	uint32_t value;

	/* Only a scalar or a vector is moved at once. */
	type = drv_gpu_spirv_id(parser, pointer->pointee);
	if (type == NULL)
		return EINVAL;
	if (type->kind != ID_TYPE_INT &&
	    type->kind != ID_TYPE_FLOAT &&
	    type->kind != ID_TYPE_VECTOR) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "load or store of something that is not a scalar or a "
		    "vector in a storage buffer");
		return error;
	}

	/* A load's result names fresh scalars. */
	record = NULL;
	if (scalars == NULL) {
		record = drv_gpu_spirv_result(parser,
					      word[2],
					      word[1],
					      components,
					      0);
		if (record == NULL)
			return EINVAL;
	}

	/* Each scalar at its byte. */
	for (index = 0U; index < components; index++) {
		place = drv_gpu_spirv_storage_offset(
		    parser,
		    pointer,
		    pointer->byte_offset + index * pointer->component_stride);
		if (scalars == NULL) {
			value = drv_gpu_spirv_new_value(parser);
			inst = drv_gpu_spirv_emit(parser,
						  DRV_GPU_IR_LOAD_STORAGE,
						  value,
						  place,
						  0U);
			if (inst != NULL)
				inst->location = variable->uniform;
			record->comp[index] = value;

			/*
			 * A compute shader's load reads only for the block's
			 * channels (ws101-p002): a channel outside it may hold
			 * an offset past the buffer, as after `if (i >= n)
			 * return;`.
			 */
			if (inst != NULL &&
			    parser->ir->stage == DRV_GPU_STAGE_COMPUTE &&
			    parser->predicate != PREDICATE_ALWAYS) {
				inst->src[1] = parser->predicate;
				inst->component = 1U;
			}

			/* The load is recorded; the next component. */
			continue;
		}

		/* A store, predicated when the block is. */
		inst = drv_gpu_spirv_emit(parser,
					  DRV_GPU_IR_STORE_STORAGE,
					  0U,
					  place,
					  scalars[index]);
		if (inst == NULL)
			continue;
		inst->location = variable->uniform;
		if (parser->predicate != PREDICATE_ALWAYS) {
			inst->src[2] = parser->predicate;
			inst->component = 1U;
		}
	}

	/* A value the parser could not make fails the lowering. */
	if (parser->error != 0)
		return parser->error;

	/* Succeeded: the words are loaded or stored. */
	return 0;
}

/*
 * Returns an IR integer of a storage buffer pointer's byte offset `byte`, plus
 * its dynamic index times the stride.
 */
static uint32_t
drv_gpu_spirv_storage_offset(
	struct drv_gpu_spirv_parser *parser,
	const struct drv_gpu_spirv_id *pointer,
	uint32_t byte)
{
	uint32_t constant_scalar;
	uint32_t emitted_scalar;
	uint32_t constant;
	uint32_t scaled;

	/* The constant part. */
	constant = drv_gpu_spirv_integer_constant(parser, byte);
	if (pointer->dynamic_index == NO_VALUE)
		return constant;

	/* Plus the element the index names. */
	/*
	 * Resolves the scalar operand before constructing its enclosing
	 * operation.
	 */
	constant_scalar =
	    drv_gpu_spirv_integer_constant(parser, pointer->dynamic_stride);
	scaled = drv_gpu_spirv_emit_value(parser,
					  DRV_GPU_IR_IMUL,
					  pointer->dynamic_index,
					  constant_scalar);

	/* Emits the scalar selected by this lowering operation. */
	emitted_scalar =
	    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_IADD, scaled, constant);

	/*
	 * Preserves the scalar number while the caller handles a latched
	 * refusal.
	 */
	if (parser->error != 0)
		return emitted_scalar;

	/* Succeeded: reports the generated scalar number. */
	return emitted_scalar;
}

/*
 * Reports 1 when a type is a structure decorated BufferBlock: an old-style
 * storage buffer in the Uniform class.
 */
static int
drv_gpu_spirv_is_buffer_block(
	struct drv_gpu_spirv_parser *parser,
	uint32_t type_id)
{
	struct drv_gpu_spirv_id *type;

	/* The pointee structure. */
	type = drv_gpu_spirv_id(parser, type_id);
	if (type == NULL || type->kind != ID_TYPE_STRUCT)
		return 0;

	/* Succeeded: decorated or not. */
	if (type->buffer_block != 0U)
		return 1;
	return 0;
}

/* Lowers OpStore to a local, an output or a storage buffer. */
static int
drv_gpu_spirv_lower_store(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *base;
	struct drv_gpu_spirv_id *variable;
	uint32_t scalars[MAX_COMPONENTS];
	uint32_t components;
	uint32_t stored;

	/* Resolves the pointer stored through. */
	base = NULL;
	if (count >= 3U)
		base = drv_gpu_spirv_id(parser, word[1]);
	if (base == NULL)
		return EINVAL;
	if (base->kind != ID_VARIABLE && base->kind != ID_POINTER)
		return EINVAL;

	/*
	 * Resolves the variable, the pointee's size and the stored scalars; the
	 * operand may emit a constant, and it does so before the variable is
	 * checked.
	 */
	variable = drv_gpu_spirv_id(parser, base->var);
	components = drv_gpu_spirv_aggregate_scalars(parser, base->pointee);
	stored = drv_gpu_spirv_operand_wide(parser, word[2], scalars);
	if (variable == NULL)
		return EINVAL;

	/*
	 * The stored value must be made of scalars, as many as the pointee
	 * holds.
	 */
	if (components == 0U || stored != components) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "store of something that is not made of scalars, or not of "
		    "the pointee's size");
		return error;
	}

	/* Stores by what the pointer addresses. */
	if (base->ptr_kind == PTR_LOCAL) {
		error = drv_gpu_spirv_lower_store_local(parser,
							base,
							variable,
							scalars,
							components);
	} else if (base->ptr_kind == PTR_OUTPUT ||
		   base->ptr_kind == PTR_OUTPUT_BLOCK) {
		error = drv_gpu_spirv_lower_store_output(parser,
							 base,
							 variable,
							 scalars,
							 components,
							 opcode,
							 offset);
	} else if (base->ptr_kind == PTR_SSBO) {
		error = drv_gpu_spirv_lower_storage(parser,
						    word,
						    base,
						    variable,
						    scalars,
						    components,
						    opcode,
						    offset);
	} else if (base->ptr_kind == PTR_SHARED) {
		error = drv_gpu_spirv_lower_shared(parser,
						   word,
						   base,
						   scalars,
						   components,
						   opcode,
						   offset);
	} else {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "store through a pointer that is not an output, a local or "
		    "a storage buffer");
		return error;
	}

	/* Reports why the store could not be lowered. */
	if (error != 0)
		return error;

	/* Succeeded: the store is lowered. */
	return 0;
}

/*
 * Lowers a store to a local, which only remembers which scalars it now
 * holds.  Under the block's predicate a scalar is the new one where the
 * predicate holds and the old one elsewhere (a scalar never stored has no
 * old one to keep).  Through a dynamic array index each element takes the
 * new scalars where the index names it and keeps its own elsewhere; a
 * scalar never stored then starts as zero bits, which no channel reads
 * before it stores it.
 */
static int
drv_gpu_spirv_lower_store_local(
	struct drv_gpu_spirv_parser *parser,
	const struct drv_gpu_spirv_id *pointer,
	const struct drv_gpu_spirv_id *variable,
	const uint32_t *scalars,
	uint32_t components)
{
	uint32_t *slot;
	uint32_t first;
	uint32_t element;
	uint32_t index;
	uint32_t hit;
	uint32_t old;
	uint32_t value;

	/* The first scalar the pointer addresses. */
	first = 0U;
	if (pointer->component >= 0)
		first = (uint32_t)pointer->component;

	/* Without a dynamic index the one run of scalars takes the new ones. */
	if (pointer->dynamic_index == NO_VALUE) {
		for (index = 0U; index < components; index++) {
			slot = drv_gpu_spirv_variable_slot(parser,
							   variable,
							   first + index);
			if (slot == NULL)
				return EINVAL;
			value = scalars[index];
			if (*slot != NO_VALUE) {
				value = drv_gpu_spirv_predicated_value(parser,
								       value,
								       *slot);
			}

			/* Publishes the latest scalar definition for this local component. */
			*slot = value;
		}

		/* Succeeded: the local holds the stored scalars. */
		return 0;
	}

	/*
	 * Through a dynamic index each element takes the new scalars on the
	 * channels whose index names it.
	 */
	for (element = 0U; element < pointer->dynamic_length; element++) {
		hit = drv_gpu_spirv_index_is(parser,
					     pointer->dynamic_index,
					     element);
		hit =
		    drv_gpu_spirv_predicate_and(parser, parser->predicate, hit);
		for (index = 0U; index < components; index++) {
			slot = drv_gpu_spirv_variable_slot(
			    parser,
			    variable,
			    first + element * pointer->dynamic_stride + index);
			if (slot == NULL)
				return EINVAL;
			old = *slot;
			if (old == NO_VALUE) {
				old = drv_gpu_spirv_shared_constant(
				    parser,
				    &parser->zero_value,
				    DRV_GPU_IR_CONST,
				    FLOAT_ZERO_BITS);
			}

			/* Merges the new local value with the value on inactive channels. */
			*slot = drv_gpu_spirv_select_value(parser,
							   hit,
							   scalars[index],
							   old);
		}
	}

	/* Succeeded: the local holds the stored scalars. */
	return 0;
}

/*
 * Lowers a store to an output: one output write to each scalar, at the
 * location and component of its interface slot -- a located output's own,
 * or the Position or the PointSize builtin of an output block or of a
 * builtin variable (a geometry shader's Position, Layer or PrimitiveId:
 * drv_gpu_spirv_geometry_output()).  Under the block's predicate a slot keeps
 * what it held where the predicate is false: the last value stored, or the
 * zero every output starts as.
 */
static int
drv_gpu_spirv_lower_store_output(
	struct drv_gpu_spirv_parser *parser,
	const struct drv_gpu_spirv_id *pointer,
	const struct drv_gpu_spirv_id *variable,
	const uint32_t *scalars,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *type;
	struct drv_gpu_shader_ir_inst *inst;
	uint32_t map[MAX_COMPONENTS];
	uint32_t map_count;
	uint32_t *slot;
	uint32_t first;
	uint32_t previous;
	uint32_t value;
	uint32_t builtin;
	uint32_t builtin_location;
	uint32_t index;
	int is_builtin;

	/* The first interface slot the pointer addresses. */
	first = 0U;
	if (pointer->component >= 0)
		first = (uint32_t)pointer->component;

	/*
	 * An output that is not located is written only through its Position or
	 * PointSize builtin.
	 */
	builtin = 0U;
	builtin_location = DRV_GPU_IR_LOCATION_POSITION;
	is_builtin = 0;
	if (pointer->ptr_kind == PTR_OUTPUT_BLOCK) {
		/*
		 * The builtin is on the variable, or on the member the chain
		 * selected.
		 */
		type = drv_gpu_spirv_id(parser, variable->pointee);
		if (variable->has_builtin != 0U) {
			builtin = variable->builtin;
			is_builtin = 1;
		} else if (pointer->member >= 0 && type != NULL &&
			   type->kind == ID_TYPE_STRUCT &&
			   type->member_builtin[pointer->member] != 0U) {
			builtin = type->member_builtin[pointer->member] - 1U;
			is_builtin = 1;
		}

		/* An unlocated plain output is refused. */
		if (is_builtin == 0) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "store to an output that is neither located nor a "
			    "built-in");
			return error;
		}

		/*
		 * A geometry shader has builtins of its own; elsewhere any but
		 * Position and PointSize is refused.
		 */
		if (parser->ir->stage == DRV_GPU_STAGE_GEOMETRY) {
			error = drv_gpu_spirv_geometry_output(parser,
							      builtin,
							      &builtin_location,
							      opcode,
							      offset);
			if (error != 0)
				return error;
		} else if (builtin == BUILTIN_POSITION) {
			builtin_location = DRV_GPU_IR_LOCATION_POSITION;
		} else if (builtin == BUILTIN_POINT_SIZE) {
			builtin_location = DRV_GPU_IR_LOCATION_POINT_SIZE;
		} else {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "store to an output that is neither located nor "
			    "the Position or PointSize builtin");
			return error;
		}
	}

	/* Finds the interface slot of each scalar stored. */
	map_count = 0U;
	error = drv_gpu_spirv_io_map(parser,
				     pointer->pointee,
				     0U,
				     first,
				     map,
				     &map_count,
				     MAX_COMPONENTS);
	if (error != 0 || map_count != count) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "store to an output of something "
					     "that is not made of scalars");
		return error;
	}

	/*
	 * A second colour (Index 1) is a fragment shader's, of Location 0, four
	 * components at most (Vulkan: attachment 0 alone).
	 */
	if (variable->index != 0U) {
		if (parser->ir->stage != DRV_GPU_STAGE_FRAGMENT ||
		    variable->location != 0U) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "Index 1 output other than a fragment shader's "
			    "Location 0");
			return error;
		}

		/* Merges every component of the local store under the block predicate. */
		for (index = 0U; index < count; index++) {
			if (map[index] >= 4U) {
				/*
				 * Records the unsupported instruction in the
				 * caller's diagnostic.
				 */
				error = drv_gpu_spirv_refuse(
				    parser,
				    opcode,
				    offset,
				    "Index 1 output of more than one location");
				return error;
			}
		}
	}

	/* Emits one output write to each scalar. */
	for (index = 0U; index < count; index++) {
		slot =
		    drv_gpu_spirv_variable_slot(parser, variable, map[index]);
		if (slot == NULL)
			return EINVAL;

		/*
		 * Under a predicate the slot keeps what it held where the
		 * predicate is false.
		 */
		value = scalars[index];
		if (parser->predicate != PREDICATE_ALWAYS) {
			previous = *slot;
			if (previous == NO_VALUE) {
				previous = drv_gpu_spirv_shared_constant(
				    parser,
				    &parser->zero_value,
				    DRV_GPU_IR_CONST,
				    FLOAT_ZERO_BITS);
			}

			/* Attaches the active-channel guard to this storage write. */
			value = drv_gpu_spirv_predicated_value(parser,
							       value,
							       previous);
		}

		/* The slot holds what the output now has. */
		*slot = value;

		/* Writes the scalar. */
		inst = drv_gpu_spirv_emit(parser,
					  DRV_GPU_IR_STORE_OUTPUT,
					  0U,
					  value,
					  0U);
		if (inst == NULL)
			continue;

		/*
		 * The builtins are written to their own locations, a scalar one
		 * at component 0; a located output to its slot's.
		 */
		if (is_builtin != 0 &&
		    builtin_location == DRV_GPU_IR_LOCATION_POSITION) {
			inst->location = DRV_GPU_IR_LOCATION_POSITION;
			inst->component = map[index] % 4U;
		} else if (is_builtin != 0) {
			inst->location = builtin_location;
			inst->component = 0U;
		} else if (variable->index != 0U) {
			/*
			 * The second colour (Index 1) of Location 0: a fragment
			 * shader's dual-source write.
			 */
			inst->location = DRV_GPU_IR_LOCATION_SECOND_COLOR;
			inst->component = map[index] % 4U;
		} else {
			inst->location = variable->location + map[index] / 4U;
			inst->component = map[index] % 4U;
		}
	}

	/* Succeeded: the output is written. */
	return 0;
}

/* Lowers per-component OpFAdd, OpFSub and OpFMul. */
static int
drv_gpu_spirv_lower_arithmetic(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *record;
	enum drv_gpu_shader_ir_op op;
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
	left_count = drv_gpu_spirv_operand(parser, word[3], left);
	right_count = drv_gpu_spirv_operand(parser, word[4], right);

	/*
	 * Both operands and the result must be float scalars or vectors of one
	 * size.
	 */
	if (left_count == 0U || left_count != right_count) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error =
		    drv_gpu_spirv_refuse(parser,
					 opcode,
					 offset,
					 "arithmetic on operands that are not "
					 "float scalars / vectors of one size");
		return error;
	}

	/* Resolves the floating result shape required by the arithmetic operation. */
	components = drv_gpu_spirv_float_components(parser, word[1]);
	if (left_count != components) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error =
		    drv_gpu_spirv_refuse(parser,
					 opcode,
					 offset,
					 "arithmetic on operands that are not "
					 "float scalars / vectors of one size");
		return error;
	}

	/* Declares the result as fresh scalars. */
	record = drv_gpu_spirv_result(parser, word[2], word[1], left_count, 1);
	if (record == NULL)
		return EINVAL;

	/* Chooses the IR operation of the SPIR-V one. */
	if (opcode == OP_FADD) {
		op = DRV_GPU_IR_FADD;
	} else if (opcode == OP_FSUB) {
		op = DRV_GPU_IR_FSUB;
	} else {
		op = DRV_GPU_IR_FMUL;
	}

	/* Emits one operation per component. */
	for (index = 0U; index < left_count; index++) {
		(void)drv_gpu_spirv_emit(parser,
					 op,
					 record->comp[index],
					 left[index],
					 right[index]);
	}

	/* Succeeded: the arithmetic is lowered. */
	return 0;
}

/*
 * Lowers the two-operand integer instructions per component: add,
 * subtract, multiply, the divisions and remainders, the shifts and the
 * bitwise operations.
 */
static int
drv_gpu_spirv_lower_integer(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *record;
	uint32_t left[4];
	uint32_t right[4];
	uint32_t left_count;
	uint32_t right_count;
	uint32_t components;
	uint32_t left_integers;
	uint32_t right_integers;
	uint32_t width;
	uint32_t left_width;
	uint32_t right_width;
	uint32_t index;
	int shift;

	/* The instruction must carry both operands. */
	if (count != 5U)
		return EINVAL;

	/* Resolves both operands; either may emit a constant. */
	left_count = drv_gpu_spirv_operand(parser, word[3], left);
	right_count = drv_gpu_spirv_operand(parser, word[4], right);

	/*
	 * The operands and the result must be integer scalars or vectors of one
	 * size.
	 */
	components = drv_gpu_spirv_int_components(parser, word[1]);
	left_integers = drv_gpu_spirv_operand_int_components(parser, word[3]);
	right_integers = drv_gpu_spirv_operand_int_components(parser, word[4]);
	if (components == 0U ||
	    left_count != components ||
	    right_count != components) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "integer operation on operands that are not integer "
		    "scalars / vectors of the result's size");
		return error;
	}

	/* Refuses integer operands whose component counts differ from the result. */
	if (left_integers != components || right_integers != components) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "integer operation on operands that are not integer "
		    "scalars / vectors of the result's size");
		return error;
	}

	/*
	 * The widths (ws031-p039): a shift's base is the result's and its count
	 * any; the other operations' operands are all of the result's.
	 */
	width = drv_gpu_spirv_int_width(parser, word[1]);
	left_width = drv_gpu_spirv_operand_int_width(parser, word[3]);
	right_width = drv_gpu_spirv_operand_int_width(parser, word[4]);
	shift = 0;
	if (opcode == OP_SHIFT_LEFT_LOGICAL ||
	    opcode == OP_SHIFT_RIGHT_LOGICAL ||
	    opcode == OP_SHIFT_RIGHT_ARITHMETIC)
		shift = 1;
	if (left_width != width ||
	    (!shift &&
	    right_width != width)) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "integer operation on operands of another width");
		return error;
	}

	/*
	 * A 16-bit operand made whole where its high half would change the
	 * result (a shift count is used & 31).
	 */
	for (index = 0U; index < components && width == 16U; index++) {
		if (opcode == OP_SDIV ||
		    opcode == OP_SREM ||
		    opcode == OP_SMOD) {
			left[index] = drv_gpu_spirv_extend16(parser,
							     left[index],
							     width,
							     1);
			right[index] = drv_gpu_spirv_extend16(parser,
							      right[index],
							      width,
							      1);
		} else if (opcode == OP_UDIV || opcode == OP_UMOD) {
			left[index] = drv_gpu_spirv_extend16(parser,
							     left[index],
							     width,
							     0);
			right[index] = drv_gpu_spirv_extend16(parser,
							      right[index],
							      width,
							      0);
		} else if (opcode == OP_SHIFT_RIGHT_ARITHMETIC) {
			left[index] = drv_gpu_spirv_extend16(parser,
							     left[index],
							     width,
							     1);
		} else if (opcode == OP_SHIFT_RIGHT_LOGICAL) {
			left[index] = drv_gpu_spirv_extend16(parser,
							     left[index],
							     width,
							     0);
		}
	}

	/*
	 * Declares the result; its scalars are named by the lowering of each
	 * component.
	 */
	record = drv_gpu_spirv_result(parser, word[2], word[1], components, 0);
	if (record == NULL)
		return EINVAL;

	/* Lowers each component on its own. */
	for (index = 0U; index < components; index++) {
		record->comp[index] =
		    drv_gpu_spirv_lower_integer_component(parser,
							  opcode,
							  left[index],
							  right[index]);
	}

	/* Succeeded: the integer operation is lowered. */
	return 0;
}

/*
 * Lowers one component of a two-operand integer instruction and returns
 * the IR value of the result.
 *
 * The divisions and remainders are the hardware's (see
 * the selected backend's integer lowering): OpSDiv is IDIV, OpSRem IREM, OpUDiv
 * UDIV and OpUMod UMOD.  OpSMod is the remainder moved to the divisor's sign as
 * Mesa lowers imod on Gen12.0 (brw_fs_nir.cpp): where the remainder is not zero
 * and the operands' signs differ (their exclusive or is negative), the divisor
 * is added to it.
 */
static uint32_t
drv_gpu_spirv_lower_integer_component(
	struct drv_gpu_spirv_parser *parser,
	uint32_t opcode,
	uint32_t left,
	uint32_t right)
{
	uint32_t emitted_scalar;
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
		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar = drv_gpu_spirv_emit_value(parser,
							  DRV_GPU_IR_IADD,
							  left,
							  right);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case OP_ISUB:
		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar = drv_gpu_spirv_emit_value(parser,
							  DRV_GPU_IR_ISUB,
							  left,
							  right);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case OP_IMUL:
		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar = drv_gpu_spirv_emit_value(parser,
							  DRV_GPU_IR_IMUL,
							  left,
							  right);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case OP_UDIV:
		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar = drv_gpu_spirv_emit_value(parser,
							  DRV_GPU_IR_UDIV,
							  left,
							  right);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case OP_UMOD:
		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar = drv_gpu_spirv_emit_value(parser,
							  DRV_GPU_IR_UMOD,
							  left,
							  right);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case OP_SDIV:
		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar = drv_gpu_spirv_emit_value(parser,
							  DRV_GPU_IR_IDIV,
							  left,
							  right);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case OP_SREM:
		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar = drv_gpu_spirv_emit_value(parser,
							  DRV_GPU_IR_IREM,
							  left,
							  right);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case OP_SHIFT_RIGHT_LOGICAL:
		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar = drv_gpu_spirv_emit_value(parser,
							  DRV_GPU_IR_SHR,
							  left,
							  right);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case OP_SHIFT_RIGHT_ARITHMETIC:
		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar = drv_gpu_spirv_emit_value(parser,
							  DRV_GPU_IR_ASR,
							  left,
							  right);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case OP_SHIFT_LEFT_LOGICAL:
		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar = drv_gpu_spirv_emit_value(parser,
							  DRV_GPU_IR_SHL,
							  left,
							  right);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case OP_BITWISE_OR:
		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar = drv_gpu_spirv_emit_value(parser,
							  DRV_GPU_IR_IOR,
							  left,
							  right);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case OP_BITWISE_XOR:
		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar = drv_gpu_spirv_emit_value(parser,
							  DRV_GPU_IR_IXOR,
							  left,
							  right);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case OP_BITWISE_AND:
		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar = drv_gpu_spirv_emit_value(parser,
							  DRV_GPU_IR_IAND,
							  left,
							  right);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	default:
		break;
	}

	/* OpSMod: the signed remainder, which has the dividend's sign. */
	remainder =
	    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_IREM, left, right);

	/* It moves where it is not zero and the operands' signs differ. */
	zero = drv_gpu_spirv_integer_constant(parser, 0U);
	nonzero =
	    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_INE, remainder, zero);
	signs = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_IXOR, left, right);
	different =
	    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_ILT, signs, zero);
	moves = drv_gpu_spirv_emit_value(parser,
					 DRV_GPU_IR_AND,
					 nonzero,
					 different);
	moved =
	    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_IADD, remainder, right);

	/* Succeeded: the modulus with the divisor's sign. */
	emitted_scalar =
	    drv_gpu_spirv_select_value(parser, moves, moved, remainder);

	/*
	 * Preserves the scalar number while the caller handles a latched
	 * refusal.
	 */
	if (parser->error != 0)
		return emitted_scalar;

	/* Succeeded: reports the generated scalar number. */
	return emitted_scalar;
}

/*
 * Returns the absolute value of an integer whose negativity is already known.
 */
static uint32_t
drv_gpu_spirv_absolute_integer(
	struct drv_gpu_spirv_parser *parser,
	uint32_t value,
	uint32_t negative)
{
	uint32_t emitted_scalar;
	uint32_t negated;

	/* The negation where the value is negative, the value elsewhere. */
	negated = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_INEG, value, 0U);

	/* Succeeded: the magnitude. */
	emitted_scalar =
	    drv_gpu_spirv_select_value(parser, negative, negated, value);

	/*
	 * Preserves the scalar number while the caller handles a latched
	 * refusal.
	 */
	if (parser->error != 0)
		return emitted_scalar;

	/* Succeeded: reports the generated scalar number. */
	return emitted_scalar;
}

/* Lowers OpSNegate and OpNot per component. */
static int
drv_gpu_spirv_lower_integer_unary(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	uint32_t source_shape;
	uint32_t target_shape;
	int error;
	struct drv_gpu_spirv_id *record;
	enum drv_gpu_shader_ir_op op;
	uint32_t operand[4];
	uint32_t operand_count;
	uint32_t components;
	uint32_t integers;
	uint32_t index;

	/* The instruction must carry its operand. */
	if (count != 4U)
		return EINVAL;

	/* Resolves the operand; it may emit a constant. */
	operand_count = drv_gpu_spirv_operand(parser, word[3], operand);

	/*
	 * The operand and the result must be integer scalars or vectors of one
	 * size.
	 */
	components = drv_gpu_spirv_int_components(parser, word[1]);
	integers = drv_gpu_spirv_operand_int_components(parser, word[3]);
	if (components == 0U ||
	    operand_count != components ||
	    integers != components) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "integer operation on an operand that is not an integer "
		    "scalar / vector of the result's size");
		return error;
	}

	/* Resolves the declared operand shape before comparing it. */
	source_shape = drv_gpu_spirv_operand_int_width(parser, word[3]);
	/* Resolves the declared operand shape before comparing it. */
	target_shape = drv_gpu_spirv_int_width(parser, word[1]);

	/* Requires the shape accepted by this lowering operation. */
	if (source_shape != target_shape) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "integer operation on an operand of another width");
		return error;
	}

	/* Declares the result as fresh scalars. */
	record = drv_gpu_spirv_result(parser, word[2], word[1], components, 1);
	if (record == NULL)
		return EINVAL;

	/*
	 * A negation or a complement (the low 16 bits of a 16-bit one are right
	 * whatever its high half).
	 */
	if (opcode == OP_SNEGATE) {
		op = DRV_GPU_IR_INEG;
	} else {
		op = DRV_GPU_IR_INOT;
	}

	/* Emits one operation per component. */
	for (index = 0U; index < components; index++) {
		(void)drv_gpu_spirv_emit(parser,
					 op,
					 record->comp[index],
					 operand[index],
					 0U);
	}

	/* Succeeded: the operation is lowered. */
	return 0;
}

/*
 * Lowers the conversions between floats and signed or unsigned integers per
 * component.
 */
static int
drv_gpu_spirv_lower_convert(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	uint32_t source_shape;
	int error;
	struct drv_gpu_spirv_id *record;
	enum drv_gpu_shader_ir_op op;
	uint32_t operand[4];
	uint32_t operand_count;
	uint32_t components;
	uint32_t width;
	uint32_t index;

	/* The instruction must carry its operand. */
	if (count != 4U)
		return EINVAL;

	/* Resolves the operand; it may emit a constant. */
	operand_count = drv_gpu_spirv_operand(parser, word[3], operand);

	/*
	 * A float result from an integer, or an integer result from a float, of
	 * one size.
	 */
	if (opcode == OP_CONVERT_S_TO_F || opcode == OP_CONVERT_U_TO_F) {
		components = drv_gpu_spirv_float_components(parser, word[1]);
		/* Resolves the declared operand shape before comparing it. */
		source_shape =
		    drv_gpu_spirv_operand_int_components(parser, word[3]);

		/* Requires the shape accepted by this lowering operation. */
		if (source_shape != operand_count) {
			components = 0U;
		}
	} else {
		components = drv_gpu_spirv_int_components(parser, word[1]);
		/* Resolves the declared operand shape before comparing it. */
		source_shape =
		    drv_gpu_spirv_operand_float_components(parser, word[3]);

		/* Requires the shape accepted by this lowering operation. */
		if (source_shape != operand_count) {
			components = 0U;
		}
	}

	/* The operand and the result must be integers or floats of one size. */
	if (components == 0U || operand_count != components) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "conversion between operands of other kinds or sizes");
		return error;
	}

	/* Declares the result as fresh scalars. */
	record = drv_gpu_spirv_result(parser, word[2], word[1], components, 1);
	if (record == NULL)
		return EINVAL;

	/* Chooses the conversion. */
	if (opcode == OP_CONVERT_F_TO_U) {
		op = DRV_GPU_IR_F2U;
	} else if (opcode == OP_CONVERT_F_TO_S) {
		op = DRV_GPU_IR_F2I;
	} else if (opcode == OP_CONVERT_S_TO_F) {
		op = DRV_GPU_IR_I2F;
	} else {
		op = DRV_GPU_IR_U2F;
	}

	/*
	 * A 16-bit integer converted to a float is made whole first, by the
	 * conversion's signedness (ws031-p039).
	 */
	width = 0U;
	if (opcode == OP_CONVERT_S_TO_F || opcode == OP_CONVERT_U_TO_F)
		width = drv_gpu_spirv_operand_int_width(parser, word[3]);
	for (index = 0U; index < components && width == 16U; index++) {
		operand[index] =
		    drv_gpu_spirv_extend16(parser,
					   operand[index],
					   width,
					   opcode == OP_CONVERT_S_TO_F);
	}

	/*
	 * Emits one conversion per component (to a 16-bit integer, the 32-bit
	 * one's low half: SPIR-V leaves a value out of range undefined).
	 */
	for (index = 0U; index < components; index++) {
		(void)drv_gpu_spirv_emit(parser,
					 op,
					 record->comp[index],
					 operand[index],
					 0U);
	}

	/* Succeeded: the conversion is lowered. */
	return 0;
}

/*
 * Lowers OpSConvert and OpUConvert between 16-bit and 32-bit integers
 * (ws031-p039): to 32 bits the 16-bit value made whole, sign extended or
 * zero extended; to 16 bits the 32-bit value as it is (its low half is the
 * result).  The widths must differ (the SPIR-V rule).
 */
static int
drv_gpu_spirv_lower_width_convert(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	uint32_t source_shape;
	int error;
	struct drv_gpu_spirv_id *record;
	uint32_t operand[4];
	uint32_t operand_count;
	uint32_t components;
	uint32_t width;
	uint32_t from;
	uint32_t index;

	/* The instruction must carry its operand. */
	if (count != 4U)
		return EINVAL;
	operand_count = drv_gpu_spirv_operand(parser, word[3], operand);

	/* Integers of one size, of two widths. */
	components = drv_gpu_spirv_int_components(parser, word[1]);
	if (components == 0U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error =
		    drv_gpu_spirv_refuse(parser,
					 opcode,
					 offset,
					 "width conversion between operands of "
					 "other kinds or sizes");
		return error;
	}

	/* Refuses the next incompatible part of this instruction. */
	if (operand_count != components) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error =
		    drv_gpu_spirv_refuse(parser,
					 opcode,
					 offset,
					 "width conversion between operands of "
					 "other kinds or sizes");
		return error;
	}

	/* Refuses the next incompatible part of this instruction. */
	/* Resolves the declared operand shape before comparing it. */
	source_shape = drv_gpu_spirv_operand_int_components(parser, word[3]);

	/* Requires the shape accepted by this lowering operation. */
	if (source_shape != components) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error =
		    drv_gpu_spirv_refuse(parser,
					 opcode,
					 offset,
					 "width conversion between operands of "
					 "other kinds or sizes");
		return error;
	}

	/* Resolves the source and destination integer widths independently. */
	width = drv_gpu_spirv_int_width(parser, word[1]);
	from = drv_gpu_spirv_operand_int_width(parser, word[3]);
	if (width == from)
		return EINVAL;

	/*
	 * Declares the result: named scalars, the operand's or its extension.
	 */
	record = drv_gpu_spirv_result(parser, word[2], word[1], components, 0);
	if (record == NULL)
		return EINVAL;

	/* Succeeded: each component, made whole when it widens. */
	for (index = 0U; index < components; index++) {
		record->comp[index] =
		    drv_gpu_spirv_extend16(parser,
					   operand[index],
					   from,
					   opcode == OP_S_CONVERT);
	}

	/* Succeeded: every component has the requested width conversion. */
	return 0;
}

/*
 * Lowers OpBitcast between 32-bit floats and integers of one size: no IR,
 * the result names the operand's scalars, whose bits it reads as they are.
 */
static int
drv_gpu_spirv_lower_bitcast(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	uint32_t declared_width;
	int error;
	struct drv_gpu_spirv_id *record;
	uint32_t operand[4];
	uint32_t operand_count;
	uint32_t components;
	uint32_t booleans;
	uint32_t width;
	uint32_t from;
	uint32_t sixteen;
	uint32_t low;
	uint32_t high;
	uint32_t index;

	/* The instruction must carry its operand. */
	if (count != 4U)
		return EINVAL;

	/* Resolves the operand; it may emit a constant. */
	operand_count = drv_gpu_spirv_operand(parser, word[3], operand);

	/*
	 * A float or integer scalar or vector of the operand's bits; a Boolean
	 * has none.  A 16-bit integer's vector casts to or from 32-bit scalars
	 * of the same bits, component 0 the low half (ws031-p039).
	 */
	components = drv_gpu_spirv_value_components(parser, word[1]);
	booleans = drv_gpu_spirv_bool_components(parser, word[1]);
	/*
	 * Reads the width before selecting the matching scalar representation.
	 */
	declared_width = drv_gpu_spirv_int_width(parser, word[1]);
	if (declared_width == 16U) {
		width = 16U;
	} else {
		width = 32U;
	}

	/*
	 * Reads the width before selecting the matching scalar representation.
	 */
	declared_width = drv_gpu_spirv_operand_int_width(parser, word[3]);
	if (declared_width == 16U) {
		from = 16U;
	} else {
		from = 32U;
	}

	/* Refuses a Boolean or component count incompatible with this bitcast. */
	if (components == 0U ||
	    booleans != 0U ||
	    operand_count == 0U ||
	    components * width != operand_count * from) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error =
		    drv_gpu_spirv_refuse(parser,
					 opcode,
					 offset,
					 "bitcast that is not between scalars "
					 "/ vectors of the same bits");
		return error;
	}

	/*
	 * Declares the result: the operand's scalars renamed, or packed and
	 * unpacked.
	 */
	record = drv_gpu_spirv_result(parser, word[2], word[1], components, 0);
	if (record == NULL)
		return EINVAL;
	for (index = 0U; index < components; index++) {
		if (width == from) {
			record->comp[index] = operand[index];
		} else if (width == 32U) {
			/*
			 * Two 16-bit halves into one: the low one alone, the
			 * high one shifted up.
			 */
			sixteen =
			    drv_gpu_spirv_shared_constant(parser,
							  &parser->int_16_value,
							  DRV_GPU_IR_ICONST,
							  16U);
			low = drv_gpu_spirv_extend16(parser,
						     operand[2U * index],
						     16U,
						     0);
			high =
			    drv_gpu_spirv_emit_value(parser,
						     DRV_GPU_IR_SHL,
						     operand[2U * index + 1U],
						     sixteen);
			record->comp[index] =
			    drv_gpu_spirv_emit_value(parser,
						     DRV_GPU_IR_IOR,
						     low,
						     high);
		} else if ((index & 1U) == 0U) {
			/*
			 * The low half: the value itself, its high half left
			 * undefined.
			 */
			record->comp[index] = operand[index / 2U];
		} else {
			/* The high half, shifted down. */
			sixteen =
			    drv_gpu_spirv_shared_constant(parser,
							  &parser->int_16_value,
							  DRV_GPU_IR_ICONST,
							  16U);
			record->comp[index] =
			    drv_gpu_spirv_emit_value(parser,
						     DRV_GPU_IR_SHR,
						     operand[index / 2U],
						     sixteen);
		}
	}

	/* Succeeded: the bits are renamed or moved. */
	return 0;
}

/*
 * Lowers OpFMod and OpFRem per component as Mesa's nir_lower_fmod does:
 * x - y * floor(x / y) and x - y * trunc(x / y), the division as the
 * multiplication by the reciprocal OpFDiv lowers to.
 */
static int
drv_gpu_spirv_lower_float_remainder(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *record;
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
	left_count = drv_gpu_spirv_operand(parser, word[3], left);
	right_count = drv_gpu_spirv_operand(parser, word[4], right);

	/*
	 * Both operands and the result must be float scalars or vectors of one
	 * size.
	 */
	components = drv_gpu_spirv_float_components(parser, word[1]);
	if (components == 0U ||
	    left_count != components ||
	    right_count != components) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error =
		    drv_gpu_spirv_refuse(parser,
					 opcode,
					 offset,
					 "remainder of operands that are not "
					 "float scalars / vectors of one size");
		return error;
	}

	/* Declares the result; its scalars are the differences. */
	record = drv_gpu_spirv_result(parser, word[2], word[1], components, 0);
	if (record == NULL)
		return EINVAL;

	/*
	 * x - y * round(x / y), rounding down for FMod and toward zero for
	 * FRem.
	 */
	for (index = 0U; index < components; index++) {
		reciprocal = drv_gpu_spirv_emit_value(parser,
						      DRV_GPU_IR_RCP,
						      right[index],
						      0U);
		quotient = drv_gpu_spirv_emit_value(parser,
						    DRV_GPU_IR_FMUL,
						    left[index],
						    reciprocal);
		if (opcode == OP_FMOD) {
			rounded = drv_gpu_spirv_emit_value(parser,
							   DRV_GPU_IR_FLOOR,
							   quotient,
							   0U);
		} else {
			rounded = drv_gpu_spirv_emit_value(parser,
							   DRV_GPU_IR_FTRUNC,
							   quotient,
							   0U);
		}

		/*
		 * The remainder is the dividend less the divisor times the
		 * rounded quotient.
		 */
		product = drv_gpu_spirv_emit_value(parser,
						   DRV_GPU_IR_FMUL,
						   right[index],
						   rounded);
		record->comp[index] = drv_gpu_spirv_emit_value(parser,
							       DRV_GPU_IR_FSUB,
							       left[index],
							       product);
	}

	/* Succeeded: the remainder is lowered. */
	return 0;
}

/* Lowers OpVectorTimesScalar to one multiply per component. */
static int
drv_gpu_spirv_lower_vector_times_scalar(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *record;
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
	vector_count = drv_gpu_spirv_operand(parser, word[3], vector);
	scalar_count = drv_gpu_spirv_operand(parser, word[4], scalar);

	/*
	 * The operands must be a float vector and a float scalar, the result
	 * the vector's size.
	 */
	if (vector_count < 2U || scalar_count != 1U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "OpVectorTimesScalar operands");
		return error;
	}

	/* Resolves the floating shape of the vector-times-scalar result. */
	components = drv_gpu_spirv_float_components(parser, word[1]);
	if (vector_count != components) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "OpVectorTimesScalar operands");
		return error;
	}

	/* Declares the result as fresh scalars. */
	record =
	    drv_gpu_spirv_result(parser, word[2], word[1], vector_count, 1);
	if (record == NULL)
		return EINVAL;

	/* Emits one multiply by the scalar per component. */
	for (index = 0U; index < vector_count; index++) {
		(void)drv_gpu_spirv_emit(parser,
					 DRV_GPU_IR_FMUL,
					 record->comp[index],
					 vector[index],
					 scalar[0]);
	}

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
drv_gpu_spirv_lower_matrix_product(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *record;
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
	left_count = drv_gpu_spirv_operand_wide(parser, word[3], left);
	right_count = drv_gpu_spirv_operand_wide(parser, word[4], right);
	drv_gpu_spirv_operand_shape(parser, word[3], &left_columns, &left_rows);
	drv_gpu_spirv_operand_shape(parser,
				    word[4],
				    &right_columns,
				    &right_rows);
	components = drv_gpu_spirv_float_components_wide(parser, word[1]);
	if (left_count == 0U ||
	    right_count == 0U ||
	    components == 0U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "matrix product of operands that are not float scalars, "
		    "vectors or matrices");
		return error;
	}

	/*
	 * Declares the result; its scalars are named by the products and the
	 * sums.
	 */
	record = drv_gpu_spirv_result(parser, word[2], word[1], components, 0);
	if (record == NULL)
		return EINVAL;

	/* A matrix times a scalar: every scalar of the matrix times it. */
	if (opcode == OP_MATRIX_TIMES_SCALAR) {
		if (right_count != 1U || components != left_count) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "OpMatrixTimesScalar operands");
			return error;
		}

		/* Emits the row-vector product one result column at a time. */
		for (column = 0U; column < left_count; column++) {
			record->comp[column] =
			    drv_gpu_spirv_emit_value(parser,
						     DRV_GPU_IR_FMUL,
						     left[column],
						     right[0]);
		}

		/* Succeeded: the row-vector product's result columns are defined. */
		return 0;
	}

	/*
	 * A vector times a matrix: the dot product of the vector with each
	 * column.
	 */
	if (opcode == OP_VECTOR_TIMES_MATRIX) {
		if (left_columns != 1U ||
		    left_rows != right_rows ||
		    components != right_columns) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "OpVectorTimesMatrix operands");
			return error;
		}

		/* Builds each matrix product column from its input scalar components. */
		for (column = 0U; column < right_columns; column++) {
			sum = NO_VALUE;
			for (inner = 0U; inner < right_rows; inner++) {
				product = drv_gpu_spirv_emit_value(
				    parser,
				    DRV_GPU_IR_FMUL,
				    left[inner],
				    right[column * right_rows + inner]);
				if (sum == NO_VALUE) {
					sum = product;
				} else {
					sum = drv_gpu_spirv_emit_value(
					    parser,
					    DRV_GPU_IR_FADD,
					    sum,
					    product);
				}
			}

			/* The column of the product. */
			record->comp[column] = sum;
		}

		/* Succeeded: the vector times the matrix is lowered. */
		return 0;
	}

	/*
	 * A matrix times a vector or a matrix: each column of the result is the
	 * left matrix times a right column.
	 */
	if (left_columns != right_rows ||
	    components != right_columns * left_rows) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "matrix product of operands whose shapes do not agree");
		return error;
	}

	/* Refuses a matrix-times-vector operation with a matrix right operand. */
	if (opcode == OP_MATRIX_TIMES_VECTOR && right_columns != 1U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "OpMatrixTimesVector operands");
		return error;
	}

	/* Emits the remaining product columns in their scalar layout order. */
	for (column = 0U; column < right_columns; column++) {
		for (row = 0U; row < left_rows; row++) {
			/*
			 * The row of the left matrix times the right column,
			 * the column products added in order.
			 */
			sum = NO_VALUE;
			for (inner = 0U; inner < left_columns; inner++) {
				product = drv_gpu_spirv_emit_value(
				    parser,
				    DRV_GPU_IR_FMUL,
				    left[inner * left_rows + row],
				    right[column * right_rows + inner]);
				if (sum == NO_VALUE) {
					sum = product;
				} else {
					sum = drv_gpu_spirv_emit_value(
					    parser,
					    DRV_GPU_IR_FADD,
					    sum,
					    product);
				}
			}

			/* The element of the product. */
			record->comp[column * left_rows + row] = sum;
		}
	}

	/* Succeeded: the product is lowered. */
	return 0;
}

/*
 * Lowers OpTranspose: no IR, the result names the matrix's scalars row after
 * row.
 */
static int
drv_gpu_spirv_lower_transpose(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	uint32_t source_shape;
	int error;
	struct drv_gpu_spirv_id *record;
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
	matrix_count = drv_gpu_spirv_operand_wide(parser, word[3], matrix);
	drv_gpu_spirv_operand_shape(parser, word[3], &columns, &rows);
	if (matrix_count == 0U || columns < 2U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "OpTranspose of something that is not a matrix");
		return error;
	}

	/* Resolves the declared operand shape before comparing it. */
	source_shape = drv_gpu_spirv_float_components_wide(parser, word[1]);

	/* Requires the shape accepted by this lowering operation. */
	if (source_shape != matrix_count) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "OpTranspose of something that is not a matrix");
		return error;
	}

	/*
	 * Declares the result; column r of the result is row r of the matrix.
	 */
	record =
	    drv_gpu_spirv_result(parser, word[2], word[1], matrix_count, 0);
	if (record == NULL)
		return EINVAL;
	for (column = 0U; column < columns; column++) {
		for (row = 0U; row < rows; row++) {
			record->comp[row * columns + column] =
			    matrix[column * rows + row];
		}
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
drv_gpu_spirv_lower_outer_product(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *record;
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
	left_count = drv_gpu_spirv_operand(parser, word[3], left);
	right_count = drv_gpu_spirv_operand(parser, word[4], right);
	if (left_count < 2U || right_count < 2U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "OpOuterProduct of operands that are not float vectors");
		return error;
	}

	/*
	 * The result has a column per component of the second vector, a row per
	 * component of the first.
	 */
	components = drv_gpu_spirv_float_components_wide(parser, word[1]);
	if (components != left_count * right_count) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "OpOuterProduct whose result is not the operands' shape");
		return error;
	}

	/*
	 * Declares the result; its scalars are the products, column after
	 * column.
	 */
	record = drv_gpu_spirv_result(parser, word[2], word[1], components, 0);
	if (record == NULL)
		return EINVAL;
	for (column = 0U; column < right_count; column++) {
		for (row = 0U; row < left_count; row++) {
			record->comp[column * left_count + row] =
			    drv_gpu_spirv_emit_value(parser,
						     DRV_GPU_IR_FMUL,
						     left[row],
						     right[column]);
		}
	}

	/* Succeeded: the outer product is lowered. */
	return 0;
}

/* Lowers OpFNegate to one negation per component. */
static int
drv_gpu_spirv_lower_negate(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *record;
	uint32_t operand[4];
	uint32_t operand_count;
	uint32_t components;
	uint32_t index;

	/* The instruction must carry its operand. */
	if (count < 4U)
		return EINVAL;

	/* Resolves the operand; it may emit a constant. */
	operand_count = drv_gpu_spirv_operand(parser, word[3], operand);

	/*
	 * The operand and the result must be float scalars or vectors of one
	 * size.
	 */
	if (operand_count == 0U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "OpFNegate operand");
		return error;
	}

	/* Resolves the scalar or vector shape of the negated result. */
	components = drv_gpu_spirv_float_components(parser, word[1]);
	if (operand_count != components) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "OpFNegate operand");
		return error;
	}

	/* Declares the result as fresh scalars. */
	record =
	    drv_gpu_spirv_result(parser, word[2], word[1], operand_count, 1);
	if (record == NULL)
		return EINVAL;

	/* Emits one negation per component. */
	for (index = 0U; index < operand_count; index++) {
		(void)drv_gpu_spirv_emit(parser,
					 DRV_GPU_IR_FNEG,
					 record->comp[index],
					 operand[index],
					 0U);
	}

	/* Succeeded: the negation is lowered. */
	return 0;
}

/* Lowers dot(a, b) = a0*b0 + a1*b1 + ...: multiplies, then a chain of adds. */
static int
drv_gpu_spirv_lower_dot(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *record;
	uint32_t left[4];
	uint32_t right[4];
	uint32_t left_count;
	uint32_t right_count;
	uint32_t components;

	/* The instruction must carry both operands. */
	if (count < 5U)
		return EINVAL;

	/* Resolves both operands; either may emit a constant. */
	left_count = drv_gpu_spirv_operand(parser, word[3], left);
	right_count = drv_gpu_spirv_operand(parser, word[4], right);

	/*
	 * The operands must be float vectors of one size, the result a float
	 * scalar.
	 */
	if (left_count < 2U || left_count != right_count) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "OpDot operands");
		return error;
	}

	/* Requires the scalar result shape defined for a dot product. */
	components = drv_gpu_spirv_float_components(parser, word[1]);
	if (components != 1U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "OpDot operands");
		return error;
	}

	/* Declares the result; its scalar is the last sum. */
	record = drv_gpu_spirv_result(parser, word[2], word[1], 1U, 0);
	if (record == NULL)
		return EINVAL;
	record->comp[0] =
	    drv_gpu_spirv_dot_value(parser, left, right, left_count);

	/* Succeeded: the dot product is lowered. */
	return 0;
}

/* Returns the dot product of two float vectors: the products added in order. */
static uint32_t
drv_gpu_spirv_dot_value(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *left,
	const uint32_t *right,
	uint32_t components)
{
	uint32_t product;
	uint32_t sum;
	uint32_t index;

	/*
	 * Multiplies each component pair and adds each product to the running
	 * sum.
	 */
	sum = NO_VALUE;
	for (index = 0U; index < components; index++) {
		product = drv_gpu_spirv_emit_value(parser,
						   DRV_GPU_IR_FMUL,
						   left[index],
						   right[index]);

		/*
		 * The first product starts the sum; each later one is added to
		 * it.
		 */
		if (sum == NO_VALUE) {
			sum = product;
		} else {
			sum = drv_gpu_spirv_emit_value(parser,
						       DRV_GPU_IR_FADD,
						       sum,
						       product);
		}
	}

	/* Succeeded: the final sum. */
	return sum;
}

/*
 * Lowers OpCompositeConstruct of a vector, a matrix, an array or a
 * structure: no IR, only naming.  The composite's scalars are its
 * constituents' in order (see drv_gpu_spirv_type_size()).
 */
static int
drv_gpu_spirv_lower_construct(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *record;
	uint32_t constituent[MAX_COMPONENTS];
	uint32_t constituent_count;
	uint32_t components;
	uint32_t filled;
	uint32_t component;
	uint32_t index;

	/* The instruction must name its type and result. */
	if (count < 3U)
		return EINVAL;

	/*
	 * Only what is made of scalars, no more than a value holds, is
	 * constructed.
	 */
	components = drv_gpu_spirv_aggregate_scalars(parser, word[1]);
	if (components == 0U || components > MAX_COMPONENTS) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "composite that is not made of "
					     "scalars, or larger than a value");
		return error;
	}

	/* Declares the result; its scalars are named by the constituents. */
	record = drv_gpu_spirv_result(parser, word[2], word[1], components, 0);
	if (record == NULL)
		return EINVAL;

	/* Names the scalars of each constituent in order. */
	filled = 0U;
	for (index = 3U; index < count; index++) {
		constituent_count = drv_gpu_spirv_operand_wide(parser,
							       word[index],
							       constituent);
		if (constituent_count == 0U) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error =
			    drv_gpu_spirv_refuse(parser,
						 opcode,
						 offset,
						 "composite constituent that "
						 "is not made of scalars");
			return error;
		}

		/* Refuses a constituent that would overfill the composite result. */
		if (filled + constituent_count > components)
			return EINVAL;

		/* Copies the constituent's scalars into place. */
		for (component = 0U; component < constituent_count; component++) {
			record->comp[filled + component] =
			    constituent[component];
		}

		/* Places the next constituent after the scalars already copied. */
		filled += constituent_count;
	}

	/* The constituents must fill the composite exactly. */
	if (filled != components)
		return EINVAL;

	/* Succeeded: the composite names its scalars. */
	return 0;
}

/*
 * Lowers OpCompositeExtract: the scalars of the part the indices select,
 * down through members, elements, columns and components -- no IR, only
 * naming.
 */
static int
drv_gpu_spirv_lower_extract(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *record;
	uint32_t composite[MAX_COMPONENTS];
	uint32_t composite_count;
	uint32_t composite_type;
	uint32_t components;
	uint32_t length;
	uint32_t first;
	uint32_t index;

	/* The instruction must carry the composite and at least one index. */
	if (count < 5U)
		return EINVAL;

	/* Resolves the composite; it may emit a constant. */
	composite_count =
	    drv_gpu_spirv_operand_wide(parser, word[3], composite);
	if (composite_count == 0U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "extract from something that is not made of scalars");
		return error;
	}

	/* Finds the scalars the indices select. */
	composite_type = drv_gpu_spirv_operand_type(parser, word[3]);
	error = drv_gpu_spirv_composite_part(parser,
					     composite_type,
					     word,
					     count,
					     4U,
					     &first,
					     &length,
					     opcode,
					     offset);
	if (error != 0)
		return error;
	if (first + length > composite_count)
		return EINVAL;

	/* The result must be what the indices select. */
	components = drv_gpu_spirv_aggregate_scalars(parser, word[1]);
	if (components != length) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "extract whose result is not what the indices select");
		return error;
	}

	/* Declares the result as the selected scalars. */
	record = drv_gpu_spirv_result(parser, word[2], word[1], length, 0);
	if (record == NULL)
		return EINVAL;
	for (index = 0U; index < length; index++)
		record->comp[index] = composite[first + index];

	/* Succeeded: the scalars are named. */
	return 0;
}

/*
 * Lowers OpCompositeInsert: the composite's scalars with the part the
 * indices select replaced by the object's -- no IR, only naming.
 */
static int
drv_gpu_spirv_lower_insert(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *record;
	uint32_t object[MAX_COMPONENTS];
	uint32_t composite[MAX_COMPONENTS];
	uint32_t object_count;
	uint32_t composite_count;
	uint32_t length;
	uint32_t first;
	uint32_t index;

	/*
	 * The instruction must carry the object, the composite and at least one
	 * index.
	 */
	if (count < 6U)
		return EINVAL;

	/* Resolves the object and the composite; either may emit a constant. */
	object_count = drv_gpu_spirv_operand_wide(parser, word[3], object);
	composite_count =
	    drv_gpu_spirv_operand_wide(parser, word[4], composite);
	if (object_count == 0U || composite_count == 0U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "insert of something that is not made of scalars");
		return error;
	}

	/*
	 * Finds the scalars the indices select; the object must fill them
	 * exactly.
	 */
	error = drv_gpu_spirv_composite_part(parser,
					     word[1],
					     word,
					     count,
					     5U,
					     &first,
					     &length,
					     opcode,
					     offset);
	if (error != 0)
		return error;
	if (first + length > composite_count || length != object_count) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error =
		    drv_gpu_spirv_refuse(parser,
					 opcode,
					 offset,
					 "insert of an object that is not the "
					 "size of the part it replaces");
		return error;
	}

	/*
	 * Declares the result as the composite's scalars, the part replaced by
	 * the object's.
	 */
	record =
	    drv_gpu_spirv_result(parser, word[2], word[1], composite_count, 0);
	if (record == NULL)
		return EINVAL;
	for (index = 0U; index < composite_count; index++)
		record->comp[index] = composite[index];
	for (index = 0U; index < length; index++)
		record->comp[first + index] = object[index];

	/* Succeeded: the new composite names its scalars. */
	return 0;
}

/*
 * Lowers OpCopyObject of a value: the same scalars under a new id -- no IR,
 * only naming.
 */
static int
drv_gpu_spirv_lower_copy(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *record;
	uint32_t scalars[MAX_COMPONENTS];
	uint32_t scalar_count;
	uint32_t components;
	uint32_t index;

	/* The instruction must carry the operand. */
	if (count != 4U)
		return EINVAL;

	/* Only a value made of scalars is copied. */
	scalar_count = drv_gpu_spirv_operand_wide(parser, word[3], scalars);
	components = drv_gpu_spirv_aggregate_scalars(parser, word[1]);
	if (scalar_count == 0U || scalar_count != components) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "copy of something that is not a value made of scalars");
		return error;
	}

	/* Declares the result as the operand's scalars. */
	record =
	    drv_gpu_spirv_result(parser, word[2], word[1], scalar_count, 0);
	if (record == NULL)
		return EINVAL;
	for (index = 0U; index < scalar_count; index++)
		record->comp[index] = scalars[index];

	/* Succeeded: the copy names the scalars. */
	return 0;
}

/*
 * Walks the literal indices word[first_index] .. word[count - 1] down a
 * composite type and reports the part they select: its first scalar and how
 * many scalars it has.
 */
static int
drv_gpu_spirv_composite_part(
	struct drv_gpu_spirv_parser *parser,
	uint32_t type_id,
	const uint32_t *word,
	uint32_t count,
	uint32_t first_index,
	uint32_t *first,
	uint32_t *length,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	uint32_t next_type;
	uint32_t scalar_offset;
	uint32_t io_offset;
	uint32_t index;

	/* Steps down one level to each index. */
	*first = 0U;
	for (index = first_index; index < count; index++) {
		error = drv_gpu_spirv_type_step(parser,
						type_id,
						word[index],
						&next_type,
						&scalar_offset,
						&io_offset,
						opcode,
						offset);
		if (error != 0)
			return error;
		*first += scalar_offset;
		type_id = next_type;
	}

	/* The part is as long as its type. */
	*length = drv_gpu_spirv_aggregate_scalars(parser, type_id);
	if (*length == 0U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "composite part that is not made of scalars");
		return error;
	}

	/* Succeeded: the part is found. */
	return 0;
}

/* Lowers OpVectorShuffle: no IR, only naming. */
static int
drv_gpu_spirv_lower_shuffle(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *record;
	uint32_t left[4];
	uint32_t right[4];
	uint32_t left_count;
	uint32_t right_count;
	uint32_t components;
	uint32_t selector;
	uint32_t index;

	/*
	 * The instruction must carry both vectors and at least one component.
	 */
	if (count < 6U)
		return EINVAL;

	/* Resolves both vectors; either may emit a constant. */
	left_count = drv_gpu_spirv_operand(parser, word[3], left);
	right_count = drv_gpu_spirv_operand(parser, word[4], right);

	/*
	 * Both operands must be vectors and the result as long as the selector
	 * list.
	 */
	if (left_count < 2U || right_count < 2U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "OpVectorShuffle operands");
		return error;
	}

	/* Resolves the vector shape required by the shuffle result. */
	components = drv_gpu_spirv_value_components(parser, word[1]);
	if (components != count - 5U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "OpVectorShuffle operands");
		return error;
	}

	/* Declares the result; its scalars are named by the selectors. */
	record = drv_gpu_spirv_result(parser, word[2], word[1], count - 5U, 0);
	if (record == NULL)
		return EINVAL;

	/* Names each selected scalar from the first or the second vector. */
	for (index = 5U; index < count; index++) {
		selector = word[index];
		if (selector == SHUFFLE_UNDEFINED) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "OpVectorShuffle with an undefined component");
			return error;
		}

		/* Refuses a shuffle selector outside the concatenated source vectors. */
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
drv_gpu_spirv_lower_extended(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	uint32_t source_shape;
	int error;
	struct drv_gpu_spirv_id *record;
	uint32_t operand[3][4];
	uint32_t operand_count[3];
	uint32_t operands;
	uint32_t components;
	uint32_t function;
	uint32_t index;
	uint32_t width;
	int is_signed;
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
	    function == GLSL_REFLECT) {
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_geometric(parser,
						      word,
						      count,
						      opcode,
						      offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;
	}

	/* So do those of whole matrices, and the half-float packing. */
	if (function == GLSL_DETERMINANT || function == GLSL_MATRIX_INVERSE) {
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_matrix_function(parser,
							    word,
							    count,
							    opcode,
							    offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;
	}

	/* Dispatches whole-vector half packing before ordinary extended functions. */
	if (function == GLSL_PACK_HALF_2X16 ||
	    function == GLSL_UNPACK_HALF_2X16) {
		/*
		 * Lowers this instruction while preserving the frontend refusal
		 * report.
		 */
		error = drv_gpu_spirv_lower_half(parser,
						 word,
						 count,
						 opcode,
						 offset);
		if (error != 0)
			return error;

		/*
		 * Succeeded: this instruction has a complete scalar lowering.
		 */
		return 0;
	}

	/*
	 * Counts the operands of each lowered function and notes the integer
	 * ones; any other is refused.
	 */
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
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "GLSL.std.450 instruction that is not lowered");
		return error;
	}

	/* The instruction must carry exactly the function's operands. */
	if (count != 5U + operands)
		return EINVAL;

	/*
	 * The result must be a float (or, for the integer functions, integer)
	 * scalar or vector.
	 */
	if (integers != 0) {
		components = drv_gpu_spirv_int_components(parser, word[1]);
	} else {
		components = drv_gpu_spirv_float_components(parser, word[1]);
	}

	/* A result of another kind is refused. */
	if (components == 0U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "extended instruction operand");
		return error;
	}

	/*
	 * Resolves each operand, which has the result's size; an operand may
	 * emit a constant.
	 */
	for (index = 0U; index < operands; index++) {
		operand_count[index] = drv_gpu_spirv_operand(parser,
							     word[5U + index],
							     operand[index]);
		if (operand_count[index] != components) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "extended instruction operand");
			return error;
		}
	}

	/*
	 * The integer functions' 16-bit operands made whole (ws031-p039): sign
	 * extended for SAbs, SSign, SMin, SMax and SClamp, zero extended for
	 * UMin, UMax and UClamp; each operand of the result's width.
	 */
	width = 0U;
	if (integers != 0)
		width = drv_gpu_spirv_int_width(parser, word[1]);
	for (index = 0U; index < operands && width != 0U; index++) {
		/* Resolves the declared operand shape before comparing it. */
		source_shape =
		    drv_gpu_spirv_operand_int_width(parser, word[5U + index]);

		/* Requires the shape accepted by this lowering operation. */
		if (source_shape != width) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "extended instruction operand of another width");
			return error;
		}
	}

	/* Classifies unsigned minimum, maximum and clamp separately from signed functions. */
	is_signed = 0;
	if (function != GLSL_UMIN &&
	    function != GLSL_UMAX &&
	    function != GLSL_UCLAMP)
		is_signed = 1;
	for (index = 0U; index < operands * components && width == 16U; index++) {
		operand[index / components][index % components] =
		    drv_gpu_spirv_extend16(
			parser,
			operand[index / components][index % components],
			width,
			is_signed);
	}

	/*
	 * Declares the result; its scalars are named by the lowering of each
	 * component.
	 */
	record = drv_gpu_spirv_result(parser, word[2], word[1], components, 0);
	if (record == NULL)
		return EINVAL;

	/* Lowers each component on its own. */
	for (index = 0U; index < components; index++) {
		record->comp[index] =
		    drv_gpu_spirv_lower_extended_component(parser,
							   function,
							   operand,
							   index);
	}

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
drv_gpu_spirv_lower_extended_component(
	struct drv_gpu_spirv_parser *parser,
	uint32_t function,
	uint32_t operand[3][4],
	uint32_t component)
{
	uint32_t emitted_scalar;
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
		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar =
		    drv_gpu_spirv_emit_value(parser,
					     DRV_GPU_IR_FROUND_EVEN,
					     x,
					     0U);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case GLSL_TRUNC:
		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar =
		    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FTRUNC, x, 0U);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case GLSL_FABS:
		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar =
		    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FABS, x, 0U);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case GLSL_FLOOR:
		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar =
		    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FLOOR, x, 0U);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case GLSL_FRACT:
		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar =
		    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FRACT, x, 0U);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case GLSL_SIN:
		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar =
		    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_SIN, x, 0U);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case GLSL_COS:
		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar =
		    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_COS, x, 0U);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case GLSL_EXP2:
		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar =
		    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_EXP2, x, 0U);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case GLSL_LOG2:
		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar =
		    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_LOG2, x, 0U);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case GLSL_SQRT:
		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar =
		    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_SQRT, x, 0U);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case GLSL_INVERSE_SQRT:
		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar =
		    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_RSQ, x, 0U);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case GLSL_FMIN:
		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar =
		    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FMIN, x, y);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case GLSL_FMAX:
		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar =
		    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FMAX, x, y);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case GLSL_CEIL:
		/* -floor(-x). */
		temporary =
		    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FNEG, x, 0U);
		temporary = drv_gpu_spirv_emit_value(parser,
						     DRV_GPU_IR_FLOOR,
						     temporary,
						     0U);

		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar = drv_gpu_spirv_emit_value(parser,
							  DRV_GPU_IR_FNEG,
							  temporary,
							  0U);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case GLSL_FSIGN:
		/*
		 * 1 above zero, -1 below it, x itself (a zero or a NaN)
		 * otherwise.
		 */
		zero = drv_gpu_spirv_shared_constant(parser,
						     &parser->zero_value,
						     DRV_GPU_IR_CONST,
						     FLOAT_ZERO_BITS);
		one = drv_gpu_spirv_shared_constant(parser,
						    &parser->one_value,
						    DRV_GPU_IR_CONST,
						    FLOAT_ONE_BITS);
		other =
		    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FNEG, one, 0U);
		temporary =
		    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FLT, x, zero);
		temporary =
		    drv_gpu_spirv_select_value(parser, temporary, other, x);
		other =
		    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FLT, zero, x);

		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar =
		    drv_gpu_spirv_select_value(parser, other, one, temporary);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case GLSL_RADIANS:
		/* x * pi / 180. */
		temporary =
		    drv_gpu_spirv_float_constant(parser, FLOAT_DEGREE_BITS);

		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar = drv_gpu_spirv_emit_value(parser,
							  DRV_GPU_IR_FMUL,
							  x,
							  temporary);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case GLSL_DEGREES:
		/* x * 180 / pi. */
		temporary =
		    drv_gpu_spirv_float_constant(parser, FLOAT_RADIAN_BITS);

		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar = drv_gpu_spirv_emit_value(parser,
							  DRV_GPU_IR_FMUL,
							  x,
							  temporary);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case GLSL_TAN:
		/* sin(x) / cos(x). */
		temporary =
		    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_COS, x, 0U);
		temporary = drv_gpu_spirv_emit_value(parser,
						     DRV_GPU_IR_RCP,
						     temporary,
						     0U);
		other = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_SIN, x, 0U);

		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar = drv_gpu_spirv_emit_value(parser,
							  DRV_GPU_IR_FMUL,
							  other,
							  temporary);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case GLSL_EXP:
		/* 2 ^ (x * log2(e)). */
		temporary =
		    drv_gpu_spirv_float_constant(parser, FLOAT_LOG2_E_BITS);
		temporary = drv_gpu_spirv_emit_value(parser,
						     DRV_GPU_IR_FMUL,
						     x,
						     temporary);

		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar = drv_gpu_spirv_emit_value(parser,
							  DRV_GPU_IR_EXP2,
							  temporary,
							  0U);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case GLSL_LOG:
		/* log2(x) * ln(2). */
		temporary =
		    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_LOG2, x, 0U);
		other = drv_gpu_spirv_float_constant(parser, FLOAT_LN_2_BITS);

		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar = drv_gpu_spirv_emit_value(parser,
							  DRV_GPU_IR_FMUL,
							  temporary,
							  other);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case GLSL_POW:
		/* 2 ^ (log2(x) * y). */
		temporary =
		    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_LOG2, x, 0U);
		temporary = drv_gpu_spirv_emit_value(parser,
						     DRV_GPU_IR_FMUL,
						     temporary,
						     y);

		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar = drv_gpu_spirv_emit_value(parser,
							  DRV_GPU_IR_EXP2,
							  temporary,
							  0U);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case GLSL_FCLAMP:
		/* The larger of x and lo, then the smaller of that and hi. */
		temporary =
		    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FMAX, x, y);

		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar = drv_gpu_spirv_emit_value(parser,
							  DRV_GPU_IR_FMIN,
							  temporary,
							  a);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case GLSL_STEP:
		/*
		 * 0 where x (the second operand) is below the edge (the first),
		 * 1 elsewhere.
		 */
		zero = drv_gpu_spirv_shared_constant(parser,
						     &parser->zero_value,
						     DRV_GPU_IR_CONST,
						     FLOAT_ZERO_BITS);
		one = drv_gpu_spirv_shared_constant(parser,
						    &parser->one_value,
						    DRV_GPU_IR_CONST,
						    FLOAT_ONE_BITS);
		temporary =
		    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FLT, y, x);

		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar =
		    drv_gpu_spirv_select_value(parser, temporary, zero, one);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case GLSL_SMOOTH_STEP:
		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar = drv_gpu_spirv_smooth_step(parser, x, y, a);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case GLSL_SABS:
		/* -x where x is negative. */
		zero = drv_gpu_spirv_integer_constant(parser, 0U);
		temporary =
		    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_ILT, x, zero);

		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar =
		    drv_gpu_spirv_absolute_integer(parser, x, temporary);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case GLSL_SSIGN:
		/* -1 below zero, 1 above it, 0 at it. */
		zero = drv_gpu_spirv_integer_constant(parser, 0U);
		one = drv_gpu_spirv_integer_constant(parser, 1U);
		other = drv_gpu_spirv_integer_constant(parser, 0xFFFFFFFFU);
		temporary =
		    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_ILT, x, zero);
		temporary =
		    drv_gpu_spirv_select_value(parser, temporary, other, zero);
		other =
		    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_ILT, zero, x);

		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar =
		    drv_gpu_spirv_select_value(parser, other, one, temporary);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case GLSL_SMIN:
		temporary =
		    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_ILT, y, x);

		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar =
		    drv_gpu_spirv_select_value(parser, temporary, y, x);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case GLSL_UMIN:
		temporary =
		    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_ULT, y, x);

		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar =
		    drv_gpu_spirv_select_value(parser, temporary, y, x);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case GLSL_SMAX:
		temporary =
		    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_ILT, x, y);

		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar =
		    drv_gpu_spirv_select_value(parser, temporary, y, x);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case GLSL_UMAX:
		temporary =
		    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_ULT, x, y);

		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar =
		    drv_gpu_spirv_select_value(parser, temporary, y, x);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case GLSL_SCLAMP:
		/* The larger of x and lo, then the smaller of that and hi. */
		temporary =
		    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_ILT, x, y);
		temporary = drv_gpu_spirv_select_value(parser, temporary, y, x);
		other = drv_gpu_spirv_emit_value(parser,
						 DRV_GPU_IR_ILT,
						 a,
						 temporary);

		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar =
		    drv_gpu_spirv_select_value(parser, other, a, temporary);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case GLSL_UCLAMP:
		/* The same with unsigned comparisons. */
		temporary =
		    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_ULT, x, y);
		temporary = drv_gpu_spirv_select_value(parser, temporary, y, x);
		other = drv_gpu_spirv_emit_value(parser,
						 DRV_GPU_IR_ULT,
						 a,
						 temporary);

		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar =
		    drv_gpu_spirv_select_value(parser, other, a, temporary);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	default:
		break;
	}

	/* FMix: x * (1 - a) + y * a. */
	one = drv_gpu_spirv_shared_constant(parser,
					    &parser->one_value,
					    DRV_GPU_IR_CONST,
					    FLOAT_ONE_BITS);
	temporary = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FSUB, one, a);
	temporary =
	    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FMUL, x, temporary);
	other = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FMUL, y, a);

	/* Succeeded: the sum of the two weighted operands. */
	emitted_scalar =
	    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FADD, temporary, other);

	/*
	 * Preserves the scalar number while the caller handles a latched
	 * refusal.
	 */
	if (parser->error != 0)
		return emitted_scalar;

	/* Succeeded: reports the generated scalar number. */
	return emitted_scalar;
}

/*
 * Returns one component of smoothstep(e0, e1, x): t * t * (3 - 2 t), t =
 * clamp((x - e0) / (e1 - e0), 0, 1).
 */
static uint32_t
drv_gpu_spirv_smooth_step(
	struct drv_gpu_spirv_parser *parser,
	uint32_t edge0,
	uint32_t edge1,
	uint32_t x)
{
	uint32_t emitted_scalar;
	uint32_t zero;
	uint32_t one;
	uint32_t two;
	uint32_t three;
	uint32_t span;
	uint32_t t;
	uint32_t term;

	/* The constants the polynomial needs. */
	zero = drv_gpu_spirv_shared_constant(parser,
					     &parser->zero_value,
					     DRV_GPU_IR_CONST,
					     FLOAT_ZERO_BITS);
	one = drv_gpu_spirv_shared_constant(parser,
					    &parser->one_value,
					    DRV_GPU_IR_CONST,
					    FLOAT_ONE_BITS);
	two = drv_gpu_spirv_float_constant(parser, FLOAT_TWO_BITS);
	three = drv_gpu_spirv_float_constant(parser, FLOAT_THREE_BITS);

	/* t: where x lies between the edges, clamped to [0, 1]. */
	span = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FSUB, edge1, edge0);
	span = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_RCP, span, 0U);
	t = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FSUB, x, edge0);
	t = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FMUL, t, span);
	t = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FMAX, t, zero);
	t = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FMIN, t, one);

	/* t * t * (3 - 2 t). */
	term = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FMUL, two, t);
	term = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FSUB, three, term);
	term = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FMUL, t, term);

	/* Succeeded: the smooth step. */
	emitted_scalar =
	    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FMUL, t, term);

	/*
	 * Preserves the scalar number while the caller handles a latched
	 * refusal.
	 */
	if (parser->error != 0)
		return emitted_scalar;

	/* Succeeded: reports the generated scalar number. */
	return emitted_scalar;
}

/*
 * Lowers the GLSL.std.450 functions of a whole square matrix: Determinant
 * by cofactor expansion along the first row, MatrixInverse as the
 * adjugate (each element's cofactor, transposed) times the reciprocal of
 * the determinant -- the definitions Mesa lowers them by
 * (vtn_glsl450.c, build_mat_det() and matrix_inverse()), the products
 * perhaps associated otherwise.
 */
static int
drv_gpu_spirv_lower_matrix_function(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *record;
	uint32_t matrix[MAX_COMPONENTS];
	uint32_t columns_left[4];
	uint32_t rows_left[4];
	uint32_t matrix_count;
	uint32_t columns;
	uint32_t rows;
	uint32_t components;
	uint32_t expected;
	uint32_t determinant;
	uint32_t reciprocal;
	uint32_t cofactor;
	uint32_t column;
	uint32_t row;
	uint32_t index;
	uint32_t kept;

	/* The instruction must carry the one matrix. */
	if (count != 6U)
		return EINVAL;

	/* The operand must be a square float matrix. */
	matrix_count = drv_gpu_spirv_operand_wide(parser, word[5], matrix);
	drv_gpu_spirv_operand_shape(parser, word[5], &columns, &rows);
	if (matrix_count == 0U ||
	    columns < 2U ||
	    columns != rows ||
	    matrix_count != columns * rows) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "matrix function of something that is not a square matrix");
		return error;
	}

	/*
	 * The determinant is one float; the inverse a matrix of the operand's
	 * shape.
	 */
	if (word[4] == GLSL_DETERMINANT) {
		components = drv_gpu_spirv_float_components(parser, word[1]);
		expected = 1U;
	} else {
		components = drv_gpu_spirv_matrix_components(parser, word[1]);
		expected = matrix_count;
	}

	/* A result of another shape is refused. */
	if (components != expected) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "matrix function whose result is "
					     "not of the operand's shape");
		return error;
	}

	/* The whole matrix's determinant. */
	for (index = 0U; index < columns; index++) {
		columns_left[index] = index;
		rows_left[index] = index;
	}

	/* The whole matrix's determinant. */
	determinant = drv_gpu_spirv_minor_determinant(parser,
						      matrix,
						      rows,
						      columns_left,
						      rows_left,
						      columns);

	/* Declares the result; its scalars are named below. */
	record = drv_gpu_spirv_result(parser, word[2], word[1], components, 0);
	if (record == NULL)
		return EINVAL;

	/* A determinant is the one value. */
	if (word[4] == GLSL_DETERMINANT) {
		record->comp[0] = determinant;
		return 0;
	}

	/*
	 * Element (row r, column c) of the inverse is the cofactor of the
	 * operand's element (row c, column r) over the determinant: the
	 * determinant of the operand without row c and column r, negated when
	 * r + c is odd.
	 */
	reciprocal =
	    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_RCP, determinant, 0U);
	for (column = 0U; column < columns; column++) {
		for (row = 0U; row < rows; row++) {
			/* The operand's columns but r and rows but c. */
			kept = 0U;
			for (index = 0U; index < columns; index++) {
				if (index == row)
					continue;
				columns_left[kept] = index;
				kept++;
			}

			/* The operand's rows but c. */
			kept = 0U;
			for (index = 0U; index < rows; index++) {
				if (index == column)
					continue;
				rows_left[kept] = index;
				kept++;
			}

			/* The cofactor, with its sign, over the determinant. */
			cofactor =
			    drv_gpu_spirv_minor_determinant(parser,
							    matrix,
							    rows,
							    columns_left,
							    rows_left,
							    columns - 1U);
			if (((row + column) & 1U) != 0U) {
				cofactor =
				    drv_gpu_spirv_emit_value(parser,
							     DRV_GPU_IR_FNEG,
							     cofactor,
							     0U);
			}

			/* Publishes the scaled matrix cofactor in its result row and column. */
			record->comp[column * rows + row] =
			    drv_gpu_spirv_emit_value(parser,
						     DRV_GPU_IR_FMUL,
						     cofactor,
						     reciprocal);
		}
	}

	/* Succeeded: the inverse is lowered. */
	return 0;
}

/*
 * Emits the determinant of the square part of a matrix (its scalars column
 * after column, `rows` to a column) that the listed columns and rows keep,
 * `size` of each: an element for one, ad - bc for two, and the expansion
 * along the first kept row for more.  Returns its value.
 */
static uint32_t
drv_gpu_spirv_minor_determinant(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *matrix,
	uint32_t rows,
	const uint32_t *columns_kept,
	const uint32_t *rows_kept,
	uint32_t size)
{
	uint32_t sub_columns[4];
	uint32_t term;
	uint32_t other;
	uint32_t sum;
	uint32_t index;
	uint32_t kept;
	uint32_t column;

	/* One element is its own determinant. */
	if (size == 1U)
		return matrix[columns_kept[0] * rows + rows_kept[0]];

	/* Two by two: a d - b c. */
	if (size == 2U) {
		term = drv_gpu_spirv_emit_value(
		    parser,
		    DRV_GPU_IR_FMUL,
		    matrix[columns_kept[0] * rows + rows_kept[0]],
		    matrix[columns_kept[1] * rows + rows_kept[1]]);
		other = drv_gpu_spirv_emit_value(
		    parser,
		    DRV_GPU_IR_FMUL,
		    matrix[columns_kept[1] * rows + rows_kept[0]],
		    matrix[columns_kept[0] * rows + rows_kept[1]]);
		sum = drv_gpu_spirv_emit_value(parser,
					       DRV_GPU_IR_FSUB,
					       term,
					       other);
		return sum;
	}

	/*
	 * Larger: each element of the first row times its minor, the signs
	 * alternating.
	 */
	sum = NO_VALUE;
	for (column = 0U; column < size; column++) {
		/*
		 * The kept columns but this one, and every kept row but the
		 * first.
		 */
		kept = 0U;
		for (index = 0U; index < size; index++) {
			if (index == column)
				continue;
			sub_columns[kept] = columns_kept[index];
			kept++;
		}

		/* The element's minor. */
		other = drv_gpu_spirv_minor_determinant(parser,
							matrix,
							rows,
							sub_columns,
							rows_kept + 1,
							size - 1U);
		term = drv_gpu_spirv_emit_value(
		    parser,
		    DRV_GPU_IR_FMUL,
		    matrix[columns_kept[column] * rows + rows_kept[0]],
		    other);

		/* Adds the even terms and subtracts the odd ones. */
		if (sum == NO_VALUE) {
			sum = term;
		} else if ((column & 1U) != 0U) {
			sum = drv_gpu_spirv_emit_value(parser,
						       DRV_GPU_IR_FSUB,
						       sum,
						       term);
		} else {
			sum = drv_gpu_spirv_emit_value(parser,
						       DRV_GPU_IR_FADD,
						       sum,
						       term);
		}
	}

	/* Succeeded: the expansion. */
	return sum;
}

/*
 * Lowers PackHalf2x16 and UnpackHalf2x16: the two floats of a vec2 as the
 * low and the high 16-bit halves of an unsigned integer, and back, which
 * the EU converts (Mesa: pack_half_2x16_split and unpack_half_2x16_split,
 * brw_lower_pack.cpp and brw_fs_nir.cpp).
 */
static int
drv_gpu_spirv_lower_half(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	uint32_t declared_width;
	int error;
	struct drv_gpu_spirv_id *record;
	struct drv_gpu_shader_ir_inst *inst;
	uint32_t operand[4];
	uint32_t operand_count;
	uint32_t components;
	uint32_t half;

	/* The instruction must carry the one operand. */
	if (count != 6U)
		return EINVAL;
	operand_count = drv_gpu_spirv_operand(parser, word[5], operand);

	/* Packing: a vec2 into one unsigned integer. */
	if (word[4] == GLSL_PACK_HALF_2X16) {
		/*
		 * Reads the width before selecting the matching scalar
		 * representation.
		 */
		declared_width = drv_gpu_spirv_int_width(parser, word[1]);
		if (declared_width == 32U) {
			components =
			    drv_gpu_spirv_int_components(parser, word[1]);
		} else {
			components = 0U;
		}

		/* Requires two half components when packing them into one scalar. */
		if (operand_count != 2U || components != 1U) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "PackHalf2x16 that is not of a vec2 into a uint");
			return error;
		}

		/* Creates the scalar result that owns the packed pair of half components. */
		record = drv_gpu_spirv_result(parser, word[2], word[1], 1U, 0);
		if (record == NULL)
			return EINVAL;
		record->comp[0] = drv_gpu_spirv_emit_value(parser,
							   DRV_GPU_IR_PACK_HALF,
							   operand[0],
							   operand[1]);
		return 0;
	}

	/* Unpacking: one unsigned integer into a vec2. */
	components = drv_gpu_spirv_float_components(parser, word[1]);
	if (operand_count != 1U || components != 2U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "UnpackHalf2x16 that is not of a uint into a vec2");
		return error;
	}

	/* Creates the two result scalars that receive the unpacked half components. */
	record = drv_gpu_spirv_result(parser, word[2], word[1], 2U, 1);
	if (record == NULL)
		return EINVAL;

	/* Converts the low half, then the high one. */
	for (half = 0U; half < 2U; half++) {
		inst = drv_gpu_spirv_emit(parser,
					  DRV_GPU_IR_UNPACK_HALF,
					  record->comp[half],
					  operand[0],
					  0U);
		if (inst != NULL)
			inst->component = half;
	}

	/* Succeeded: the halves are lowered. */
	return 0;
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
drv_gpu_spirv_lower_geometric(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	uint32_t source_shape;
	int error;
	struct drv_gpu_spirv_id *record;
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

	/*
	 * Resolves the first operand, the vector every one of these functions
	 * takes.
	 */
	function = word[4];
	first_count = drv_gpu_spirv_operand(parser, word[5], first);
	if (first_count == 0U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "extended instruction operand");
		return error;
	}

	/* Refuses the next incompatible part of this instruction. */
	/* Resolves the declared operand shape before comparing it. */
	source_shape = drv_gpu_spirv_operand_float_components(parser, word[5]);

	/* Requires the shape accepted by this lowering operation. */
	if (first_count != source_shape) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "extended instruction operand");
		return error;
	}

	/*
	 * Normalize and Length take one operand; Distance, Cross and Reflect a
	 * second of the first's size.
	 */
	second_count = 0U;
	if (function == GLSL_NORMALIZE || function == GLSL_LENGTH) {
		if (count != 6U)
			return EINVAL;
	} else {
		if (count != 7U)
			return EINVAL;
		second_count = drv_gpu_spirv_operand(parser, word[6], second);
		if (second_count != first_count) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "extended instruction operand");
			return error;
		}
	}

	/*
	 * A length or a distance is a float scalar; the others the operand's
	 * size.
	 */
	components = drv_gpu_spirv_float_components(parser, word[1]);
	if (function == GLSL_LENGTH || function == GLSL_DISTANCE) {
		if (components != 1U) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "extended instruction operand");
			return error;
		}
	} else if (components != first_count) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "extended instruction operand");
		return error;
	}

	/* A cross product is of three-component vectors. */
	if (function == GLSL_CROSS && first_count != 3U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "cross product of vectors that do "
					     "not have three components");
		return error;
	}

	/* Declares the result; its scalars are named below. */
	record = drv_gpu_spirv_result(parser, word[2], word[1], components, 0);
	if (record == NULL)
		return EINVAL;

	/* Lowers by the function. */
	switch (function) {
	case GLSL_LENGTH:
		/* sqrt(dot(v, v)). */
		square =
		    drv_gpu_spirv_dot_value(parser, first, first, first_count);
		record->comp[0] = drv_gpu_spirv_emit_value(parser,
							   DRV_GPU_IR_SQRT,
							   square,
							   0U);
		break;

	case GLSL_DISTANCE:
		/* The length of p0 - p1. */
		for (index = 0U; index < first_count; index++) {
			difference[index] =
			    drv_gpu_spirv_emit_value(parser,
						     DRV_GPU_IR_FSUB,
						     first[index],
						     second[index]);
		}

		/* Builds the squared vector length used by normalization. */
		square = drv_gpu_spirv_dot_value(parser,
						 difference,
						 difference,
						 first_count);
		record->comp[0] = drv_gpu_spirv_emit_value(parser,
							   DRV_GPU_IR_SQRT,
							   square,
							   0U);
		break;

	case GLSL_CROSS:
		/* (a.y b.z - a.z b.y, a.z b.x - a.x b.z, a.x b.y - a.y b.x). */
		for (index = 0U; index < 3U; index++) {
			square =
			    drv_gpu_spirv_emit_value(parser,
						     DRV_GPU_IR_FMUL,
						     first[(index + 1U) % 3U],
						     second[(index + 2U) % 3U]);
			scale =
			    drv_gpu_spirv_emit_value(parser,
						     DRV_GPU_IR_FMUL,
						     first[(index + 2U) % 3U],
						     second[(index + 1U) % 3U]);
			record->comp[index] =
			    drv_gpu_spirv_emit_value(parser,
						     DRV_GPU_IR_FSUB,
						     square,
						     scale);
		}

		break;

	case GLSL_NORMALIZE:
		/* v times the inverse of its length. */
		square =
		    drv_gpu_spirv_dot_value(parser, first, first, first_count);
		scale = drv_gpu_spirv_emit_value(parser,
						 DRV_GPU_IR_RSQ,
						 square,
						 0U);
		for (index = 0U; index < first_count; index++) {
			record->comp[index] =
			    drv_gpu_spirv_emit_value(parser,
						     DRV_GPU_IR_FMUL,
						     first[index],
						     scale);
		}

		break;

	default:
		/* Reflect: I - 2 dot(N, I) N. */
		two = drv_gpu_spirv_float_constant(parser, FLOAT_TWO_BITS);
		square =
		    drv_gpu_spirv_dot_value(parser, second, first, first_count);
		scale = drv_gpu_spirv_emit_value(parser,
						 DRV_GPU_IR_FMUL,
						 two,
						 square);
		for (index = 0U; index < first_count; index++) {
			square = drv_gpu_spirv_emit_value(parser,
							  DRV_GPU_IR_FMUL,
							  scale,
							  second[index]);
			record->comp[index] =
			    drv_gpu_spirv_emit_value(parser,
						     DRV_GPU_IR_FSUB,
						     first[index],
						     square);
		}

		break;
	}

	/* Succeeded: the function is lowered. */
	return 0;
}

/* Lowers OpFDiv per component as Mesa does (lower_fdiv): a * (1 / b). */
static int
drv_gpu_spirv_lower_divide(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *record;
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
	left_count = drv_gpu_spirv_operand(parser, word[3], left);
	right_count = drv_gpu_spirv_operand(parser, word[4], right);

	/*
	 * Both operands and the result must be float scalars or vectors of one
	 * size.
	 */
	components = drv_gpu_spirv_float_components(parser, word[1]);
	if (components == 0U ||
	    left_count != components ||
	    right_count != components) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error =
		    drv_gpu_spirv_refuse(parser,
					 opcode,
					 offset,
					 "division of operands that are not "
					 "float scalars / vectors of one size");
		return error;
	}

	/* Declares the result; its scalars are the products. */
	record = drv_gpu_spirv_result(parser, word[2], word[1], components, 0);
	if (record == NULL)
		return EINVAL;

	/* The dividend times the reciprocal of the divisor, per component. */
	for (index = 0U; index < components; index++) {
		reciprocal = drv_gpu_spirv_emit_value(parser,
						      DRV_GPU_IR_RCP,
						      right[index],
						      0U);
		record->comp[index] = drv_gpu_spirv_emit_value(parser,
							       DRV_GPU_IR_FMUL,
							       left[index],
							       reciprocal);
	}

	/* Succeeded: the division is lowered. */
	return 0;
}

/* Lowers a float comparison per component to a Boolean. */
static int
drv_gpu_spirv_lower_compare(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *record;
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
	left_count = drv_gpu_spirv_operand(parser, word[3], left);
	right_count = drv_gpu_spirv_operand(parser, word[4], right);

	/* The operands must be float vectors of the Boolean result's size. */
	components = drv_gpu_spirv_bool_components(parser, word[1]);
	if (components == 0U ||
	    left_count != components ||
	    right_count != components) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "comparison of operands that are not float scalars / "
		    "vectors of the result's size");
		return error;
	}

	/* Declares the Boolean result. */
	record = drv_gpu_spirv_result(parser, word[2], word[1], components, 0);
	if (record == NULL)
		return EINVAL;

	/* Compares each component pair. */
	for (index = 0U; index < components; index++) {
		record->comp[index] =
		    drv_gpu_spirv_lower_compare_component(parser,
							  opcode,
							  left[index],
							  right[index]);
	}

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
drv_gpu_spirv_lower_compare_component(
	struct drv_gpu_spirv_parser *parser,
	uint32_t opcode,
	uint32_t left,
	uint32_t right)
{
	uint32_t emitted_scalar;
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
		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar = drv_gpu_spirv_emit_value(parser,
							  DRV_GPU_IR_FEQ,
							  left,
							  right);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case OP_FUNORD_NOT_EQUAL:
		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar = drv_gpu_spirv_emit_value(parser,
							  DRV_GPU_IR_FNEU,
							  left,
							  right);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case OP_FORD_LESS_THAN:
		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar = drv_gpu_spirv_emit_value(parser,
							  DRV_GPU_IR_FLT,
							  left,
							  right);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case OP_FORD_GREATER_THAN:
		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar = drv_gpu_spirv_emit_value(parser,
							  DRV_GPU_IR_FLT,
							  right,
							  left);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case OP_FORD_LESS_THAN_EQUAL:
		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar = drv_gpu_spirv_emit_value(parser,
							  DRV_GPU_IR_FGE,
							  right,
							  left);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case OP_FORD_GREATER_THAN_EQUAL:
		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar = drv_gpu_spirv_emit_value(parser,
							  DRV_GPU_IR_FGE,
							  left,
							  right);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case OP_FUNORD_LESS_THAN:
		/* Not left >= right. */
		test = drv_gpu_spirv_emit_value(parser,
						DRV_GPU_IR_FGE,
						left,
						right);

		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar =
		    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_NOT, test, 0U);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case OP_FUNORD_GREATER_THAN:
		/* Not right >= left. */
		test = drv_gpu_spirv_emit_value(parser,
						DRV_GPU_IR_FGE,
						right,
						left);

		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar =
		    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_NOT, test, 0U);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case OP_FUNORD_LESS_THAN_EQUAL:
		/* Not right < left. */
		test = drv_gpu_spirv_emit_value(parser,
						DRV_GPU_IR_FLT,
						right,
						left);

		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar =
		    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_NOT, test, 0U);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case OP_FUNORD_GREATER_THAN_EQUAL:
		/* Not left < right. */
		test = drv_gpu_spirv_emit_value(parser,
						DRV_GPU_IR_FLT,
						left,
						right);

		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar =
		    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_NOT, test, 0U);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	case OP_FUNORD_EQUAL:
		/*
		 * left == right, or either is NaN (a NaN is not equal to
		 * itself).
		 */
		equal = drv_gpu_spirv_emit_value(parser,
						 DRV_GPU_IR_FEQ,
						 left,
						 right);
		left_nan = drv_gpu_spirv_emit_value(parser,
						    DRV_GPU_IR_FNEU,
						    left,
						    left);
		right_nan = drv_gpu_spirv_emit_value(parser,
						     DRV_GPU_IR_FNEU,
						     right,
						     right);
		either_nan = drv_gpu_spirv_emit_value(parser,
						      DRV_GPU_IR_OR,
						      left_nan,
						      right_nan);

		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar = drv_gpu_spirv_emit_value(parser,
							  DRV_GPU_IR_OR,
							  equal,
							  either_nan);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;

	default:
		break;
	}

	/*
	 * FOrdNotEqual: left != right, and both are numbers (a number is equal
	 * to itself).
	 */
	different =
	    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FNEU, left, right);
	left_number =
	    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FEQ, left, left);
	right_number =
	    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FEQ, right, right);
	both_numbers = drv_gpu_spirv_emit_value(parser,
						DRV_GPU_IR_AND,
						left_number,
						right_number);

	/* Succeeded: different and ordered. */
	emitted_scalar = drv_gpu_spirv_emit_value(parser,
						  DRV_GPU_IR_AND,
						  different,
						  both_numbers);

	/*
	 * Preserves the scalar number while the caller handles a latched
	 * refusal.
	 */
	if (parser->error != 0)
		return emitted_scalar;

	/* Succeeded: reports the generated scalar number. */
	return emitted_scalar;
}

/*
 * Lowers an integer comparison per component to a Boolean: equality, and
 * the signed or unsigned orderings, > and <= with the operands swapped
 * (Mesa's spirv_to_nir, vtn_alu.c).
 */
static int
drv_gpu_spirv_lower_integer_compare(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	uint32_t source_shape;
	int error;
	struct drv_gpu_spirv_id *record;
	enum drv_gpu_shader_ir_op op;
	uint32_t left[4];
	uint32_t right[4];
	uint32_t left_count;
	uint32_t right_count;
	uint32_t components;
	uint32_t index;
	uint32_t width;
	int is_signed;
	int swapped;

	/* The instruction must carry both operands. */
	if (count != 5U)
		return EINVAL;

	/* Resolves both operands; either may emit a constant. */
	left_count = drv_gpu_spirv_operand(parser, word[3], left);
	right_count = drv_gpu_spirv_operand(parser, word[4], right);

	/* The operands must be integer vectors of the Boolean result's size. */
	components = drv_gpu_spirv_bool_components(parser, word[1]);
	if (components == 0U ||
	    left_count != components ||
	    right_count != components) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "comparison of operands that are not integer scalars / "
		    "vectors of the result's size");
		return error;
	}

	/* Resolves the declared operand shape before comparing it. */
	source_shape = drv_gpu_spirv_operand_int_components(parser, word[3]);

	/* Requires the shape accepted by this lowering operation. */
	if (source_shape != components) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "comparison of operands that are not integer scalars / "
		    "vectors of the result's size");
		return error;
	}

	/* Refuses the next incompatible part of this instruction. */
	/* Resolves the declared operand shape before comparing it. */
	source_shape = drv_gpu_spirv_operand_int_components(parser, word[4]);

	/* Requires the shape accepted by this lowering operation. */
	if (source_shape != components) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "comparison of operands that are not integer scalars / "
		    "vectors of the result's size");
		return error;
	}

	/* Resolves the common integer width before comparing the two operands. */
	width = drv_gpu_spirv_operand_int_width(parser, word[3]);
	/* Resolves the declared operand shape before comparing it. */
	source_shape = drv_gpu_spirv_operand_int_width(parser, word[4]);

	/* Requires the shape accepted by this lowering operation. */
	if (source_shape != width) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "comparison of operands of different widths");
		return error;
	}

	/* Picks the test and whether the operands are swapped. */
	swapped = 0;
	switch (opcode) {
	case OP_IEQUAL:
		op = DRV_GPU_IR_IEQ;
		break;

	case OP_INOT_EQUAL:
		op = DRV_GPU_IR_INE;
		break;

	case OP_ULESS_THAN:
		op = DRV_GPU_IR_ULT;
		break;

	case OP_SLESS_THAN:
		op = DRV_GPU_IR_ILT;
		break;

	case OP_UGREATER_THAN:
		op = DRV_GPU_IR_ULT;
		swapped = 1;
		break;

	case OP_SGREATER_THAN:
		op = DRV_GPU_IR_ILT;
		swapped = 1;
		break;

	case OP_UGREATER_THAN_EQUAL:
		op = DRV_GPU_IR_UGE;
		break;

	case OP_SGREATER_THAN_EQUAL:
		op = DRV_GPU_IR_IGE;
		break;

	case OP_ULESS_THAN_EQUAL:
		op = DRV_GPU_IR_UGE;
		swapped = 1;
		break;

	default:
		/* OpSLessThanEqual. */
		op = DRV_GPU_IR_IGE;
		swapped = 1;
		break;
	}

	/*
	 * 16-bit operands made whole: signed for the signed orderings, unsigned
	 * for the others and equality (ws031-p039).
	 */
	is_signed = 0;
	if (op == DRV_GPU_IR_ILT || op == DRV_GPU_IR_IGE)
		is_signed = 1;
	for (index = 0U; index < components && width == 16U; index++) {
		left[index] = drv_gpu_spirv_extend16(parser,
						     left[index],
						     width,
						     is_signed);
		right[index] = drv_gpu_spirv_extend16(parser,
						      right[index],
						      width,
						      is_signed);
	}

	/* Declares the Boolean result. */
	record = drv_gpu_spirv_result(parser, word[2], word[1], components, 0);
	if (record == NULL)
		return EINVAL;

	/* Compares each component pair. */
	for (index = 0U; index < components; index++) {
		if (swapped != 0) {
			record->comp[index] =
			    drv_gpu_spirv_emit_value(parser,
						     op,
						     right[index],
						     left[index]);
		} else {
			record->comp[index] =
			    drv_gpu_spirv_emit_value(parser,
						     op,
						     left[index],
						     right[index]);
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
drv_gpu_spirv_lower_logical(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *record;
	enum drv_gpu_shader_ir_op op;
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
	left_count = drv_gpu_spirv_operand(parser, word[3], left);
	right_count = left_count;
	kern_memcpy(right, left, sizeof(right));
	if (opcode != OP_LOGICAL_NOT)
		right_count = drv_gpu_spirv_operand(parser, word[4], right);

	/* The operands and the result must be Booleans of one size. */
	components = drv_gpu_spirv_bool_components(parser, word[1]);
	if (components == 0U ||
	    left_count != components ||
	    right_count != components) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "logical operation on operands that are not Booleans of "
		    "the result's size");
		return error;
	}

	/* Chooses the IR operation of the SPIR-V one. */
	if (opcode == OP_LOGICAL_AND) {
		op = DRV_GPU_IR_AND;
	} else if (opcode == OP_LOGICAL_OR) {
		op = DRV_GPU_IR_OR;
	} else if (opcode == OP_LOGICAL_EQUAL) {
		op = DRV_GPU_IR_IEQ;
	} else if (opcode == OP_LOGICAL_NOT_EQUAL) {
		op = DRV_GPU_IR_INE;
	} else {
		op = DRV_GPU_IR_NOT;
	}

	/* Declares the result; its scalars are named by the operations. */
	record = drv_gpu_spirv_result(parser, word[2], word[1], components, 0);
	if (record == NULL)
		return EINVAL;

	/* Emits one operation per component. */
	for (index = 0U; index < components; index++) {
		record->comp[index] = drv_gpu_spirv_emit_value(parser,
							       op,
							       left[index],
							       right[index]);
	}

	/* Succeeded: the logical operation is lowered. */
	return 0;
}

/*
 * Lowers OpSelect per component: a scalar condition applies to every
 * component, a vector one component by component.
 */
static int
drv_gpu_spirv_lower_select(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *record;
	struct drv_gpu_shader_ir_inst *inst;
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
	condition_count = drv_gpu_spirv_operand(parser, word[3], condition);
	taken_count = drv_gpu_spirv_operand(parser, word[4], taken);
	other_count = drv_gpu_spirv_operand(parser, word[5], other);

	/*
	 * The values have the result's size; the condition is one Boolean or
	 * one to a component.
	 */
	components = drv_gpu_spirv_value_components(parser, word[1]);
	if (components == 0U ||
	    taken_count != components ||
	    other_count != components) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "OpSelect of values that are not scalars / vectors of the "
		    "result's size");
		return error;
	}

	/* Requires a scalar condition or one condition component per selected value. */
	if (condition_count != 1U && condition_count != components) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error =
		    drv_gpu_spirv_refuse(parser,
					 opcode,
					 offset,
					 "OpSelect condition of another size");
		return error;
	}

	/* Declares the result as fresh scalars. */
	record = drv_gpu_spirv_result(parser, word[2], word[1], components, 1);
	if (record == NULL)
		return EINVAL;

	/* Emits one selection per component. */
	for (index = 0U; index < components; index++) {
		/* A scalar condition chooses for every component. */
		chosen = condition[0];
		if (condition_count != 1U)
			chosen = condition[index];

		/*
		 * The selection reads three sources; the third is set after the
		 * emit.
		 */
		inst = drv_gpu_spirv_emit(parser,
					  DRV_GPU_IR_SELECT,
					  record->comp[index],
					  chosen,
					  taken[index]);
		if (inst != NULL)
			inst->src[2] = other[index];
	}

	/* Succeeded: the selection is lowered. */
	return 0;
}

/*
 * Lowers a sample of a 2D combined image sampler of floats at a two-float
 * coordinate, four float results dst .. dst + 3 (any other sample is
 * drv_gpu_spirv_lower_texture()'s): OpImageSampleImplicitLod (texture(),
 * with a Bias or not) and OpImageSampleExplicitLod with a Lod
 * (textureLod()), either with a ConstOffset (textureOffset()) of -8 .. 7
 * texels.  Any other image operand is refused.
 */
static int
drv_gpu_spirv_lower_sample(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *sampler;
	struct drv_gpu_spirv_id *type;
	struct drv_gpu_spirv_id *record;
	struct drv_gpu_shader_ir_inst *inst;
	enum drv_gpu_shader_ir_op op;
	uint32_t coordinate[4];
	uint32_t scalars[4];
	uint32_t coordinate_count;
	uint32_t scalar_count;
	uint32_t components;
	uint32_t operands;
	uint32_t next;
	uint32_t level;
	uint32_t texel_offset;

	/* The instruction must carry the sampler and the coordinate. */
	if (count < 5U)
		return EINVAL;

	/*
	 * Any image but a 2D one of floats, and gradients, take the general
	 * message.
	 */
	sampler = drv_gpu_spirv_id(parser, word[3]);
	type = drv_gpu_spirv_image_type(parser, sampler);
	components = drv_gpu_spirv_float_components(parser, word[1]);
	if (type == NULL ||
	    type->image_dim != DIM_2D ||
	    type->image_arrayed != 0U ||
	    components != 4U ||
	    (count > 5U &&
	    (word[5] & IMAGE_OPERAND_GRAD) != 0U)) {
		error = drv_gpu_spirv_lower_texture(parser,
						    word,
						    count,
						    opcode,
						    offset);
		return error;
	}

	/* Resolves the coordinate; it may emit a constant. */
	coordinate_count = drv_gpu_spirv_operand(parser, word[4], coordinate);

	/*
	 * Only a loaded combined sampler at a two-float coordinate, giving a
	 * four-float result, is lowered.
	 */
	if (sampler == NULL ||
	    sampler->kind != ID_SAMPLED_IMAGE ||
	    coordinate_count != 2U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "sample that is not of a sampler2D at a vec2");
		return error;
	}

	/* Resolves the floating result shape before building the sampler message. */
	components = drv_gpu_spirv_float_components(parser, word[1]);
	if (components != 4U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "sample that is not of a sampler2D at a vec2");
		return error;
	}

	/* The image operands, and their values in the order of their bits. */
	operands = 0U;
	if (count > 5U)
		operands = word[5];
	if ((operands & ~(IMAGE_OPERAND_BIAS | IMAGE_OPERAND_LOD |
			  IMAGE_OPERAND_CONST_OFFSET)) != 0U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "sample with image operands other "
					     "than Bias, Lod and ConstOffset");
		return error;
	}

	/* Starts the optional image operands after the depth-reference scalar. */
	next = 6U;

	/* A bias moves the level of detail texture() chooses. */
	op = DRV_GPU_IR_SAMPLE;
	level = 0U;
	if ((operands & IMAGE_OPERAND_BIAS) != 0U) {
		if (opcode != OP_IMAGE_SAMPLE_IMPLICIT_LOD || next >= count)
			return EINVAL;
		scalar_count =
		    drv_gpu_spirv_operand(parser, word[next], scalars);
		if (scalar_count != 1U) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "sample with a bias that is not a float");
			return error;
		}

		/* Selects a biased sample message after resolving its bias operand. */
		op = DRV_GPU_IR_SAMPLE_BIAS;
		level = scalars[0];
		next++;
	}

	/* A level of detail replaces the one the derivatives would choose. */
	if ((operands & IMAGE_OPERAND_LOD) != 0U) {
		if (opcode != OP_IMAGE_SAMPLE_EXPLICIT_LOD || next >= count)
			return EINVAL;
		scalar_count =
		    drv_gpu_spirv_operand(parser, word[next], scalars);
		if (scalar_count != 1U) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error =
			    drv_gpu_spirv_refuse(parser,
						 opcode,
						 offset,
						 "sample with a level of "
						 "detail that is not a float");
			return error;
		}

		/* Selects an explicit-level sample message after resolving its level operand. */
		op = DRV_GPU_IR_SAMPLE_LOD;
		level = scalars[0];
		next++;
	}

	/*
	 * An explicit sample without a level of detail takes gradients, which
	 * are not lowered.
	 */
	if (opcode == OP_IMAGE_SAMPLE_EXPLICIT_LOD &&
	    op != DRV_GPU_IR_SAMPLE_LOD) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "sample with gradients");
		return error;
	}

	/* A constant offset moves the texel grid. */
	texel_offset = 0U;
	if ((operands & IMAGE_OPERAND_CONST_OFFSET) != 0U) {
		if (next >= count)
			return EINVAL;
		error = drv_gpu_spirv_texel_offset(parser,
						   word[next],
						   &texel_offset,
						   opcode,
						   offset);
		if (error != 0)
			return error;
		next++;
	}

	/* Every operand must have been read. */
	if (count > 5U && next != count)
		return EINVAL;

	/* Declares the result as four fresh scalars. */
	record = drv_gpu_spirv_result(parser, word[2], word[1], 4U, 1);
	if (record == NULL)
		return EINVAL;

	/*
	 * The reply is four consecutive values, so the scalars must be
	 * consecutive.
	 */
	if (record->comp[1] != record->comp[0] + 1U ||
	    record->comp[3] != record->comp[0] + 3U)
		return EINVAL;

	/*
	 * Emits the sample: set in `location`, binding in `immediate`, the
	 * offset in `component`, a level in src[2].
	 */
	inst = drv_gpu_spirv_emit(parser,
				  op,
				  record->comp[0],
				  coordinate[0],
				  coordinate[1]);
	if (inst != NULL) {
		inst->immediate = sampler->binding;
		inst->location = sampler->set;
		inst->component = texel_offset;
		if (op != DRV_GPU_IR_SAMPLE)
			inst->src[2] = level;
		drv_gpu_spirv_guard(parser, inst);
	}

	/* Succeeded: the sample is lowered. */
	return 0;
}

/*
 * Packs a constant texel offset (an integer scalar or a vector of two or
 * three components, each -8 .. 7) the way the sampler message header takes
 * it (Mesa's brw_texture_offset(), brw_fs_nir.cpp): u in bits 11:8, v in
 * bits 7:4, r in bits 3:0.
 */
static int
drv_gpu_spirv_texel_offset(
	struct drv_gpu_spirv_parser *parser,
	uint32_t id,
	uint32_t *bits,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *record;
	struct drv_gpu_spirv_id *component;
	uint32_t members[3];
	uint32_t member_count;
	int32_t value;
	uint32_t index;

	/*
	 * The offset must be a constant integer scalar, or a vector of two or
	 * three.
	 */
	record = drv_gpu_spirv_id(parser, id);
	if (record == NULL) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error =
		    drv_gpu_spirv_refuse(parser,
					 opcode,
					 offset,
					 "texel offset that is not a constant");
		return error;
	}

	/* Resolves the texel offset from a declared integer constant. */
	if (record->kind == ID_CONSTANT) {
		members[0] = id;
		member_count = 1U;
	} else if (record->kind == ID_CONSTANT_COMPOSITE &&
		   record->count >= 2U && record->count <= 3U) {
		member_count = record->count;
		for (index = 0U; index < member_count; index++)
			members[index] = record->member_type[index];
	} else {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error =
		    drv_gpu_spirv_refuse(parser,
					 opcode,
					 offset,
					 "texel offset that is not a constant");
		return error;
	}

	/* Packs each component, u first. */
	*bits = 0U;
	for (index = 0U; index < member_count; index++) {
		component = drv_gpu_spirv_id(parser, members[index]);
		if (component == NULL || component->kind != ID_CONSTANT) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "texel offset that is not a constant");
			return error;
		}

		/* The hardware takes -8 .. 7. */
		value = (int32_t)component->constant;
		if (value < -8 || value > 7) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "texel offset outside -8 .. 7");
			return error;
		}

		/* Packs this signed texel offset into its message nibble. */
		*bits |= ((uint32_t)value & 0xFU) << (8U - 4U * index);
	}

	/* Succeeded: the offset bits. */
	return 0;
}

/*
 * Finds the image type of a loaded combined image sampler or of the image
 * OpImage takes from it; NULL for anything else.
 */
static struct drv_gpu_spirv_id *
drv_gpu_spirv_image_type(
	struct drv_gpu_spirv_parser *parser,
	struct drv_gpu_spirv_id *image)
{
	struct drv_gpu_spirv_id *type;

	/* Only a loaded sampler or its image has an image type. */
	if (image == NULL || image->kind != ID_SAMPLED_IMAGE)
		return NULL;

	/* A sampled image type names its image type. */
	type = drv_gpu_spirv_id(parser, image->type);
	if (type != NULL && type->kind == ID_TYPE_SAMPLED_IMAGE)
		type = drv_gpu_spirv_id(parser, type->type);

	/* Succeeded: the image type, or none. */
	if (type == NULL || type->kind != ID_TYPE_IMAGE)
		return NULL;
	return type;
}

/*
 * Appends one parameter to a sampler message's list; a list longer than a
 * message takes still counts, so the emit refuses it.
 */
static void
drv_gpu_spirv_texture_param(
	uint32_t *params,
	uint32_t *count,
	uint32_t value)
{
	/* Drops what would overflow; the count still grows. */
	if (*count < DRV_GPU_IR_TEXTURE_MAX_PARAMS)
		params[*count] = value;
	(*count)++;

	/* Succeeded: the next texture operand is part of the message. */
	return;
}

/*
 * Emits a TEXTURE instruction: copies the parameters into a run of fresh
 * consecutive values and defines four fresh consecutive results, the
 * first returned in *first.
 */
static int
drv_gpu_spirv_emit_texture(
	struct drv_gpu_spirv_parser *parser,
	const struct drv_gpu_spirv_id *image,
	const uint32_t *params,
	uint32_t param_count,
	uint32_t message,
	uint32_t texel_offset,
	uint32_t *first)
{
	struct drv_gpu_shader_ir_inst *inst;
	uint32_t run;
	uint32_t value;
	uint32_t index;

	/* Refuses more parameters than a message takes. */
	if (param_count == 0U || param_count > DRV_GPU_IR_TEXTURE_MAX_PARAMS)
		return EINVAL;

	/* Copies the parameters into consecutive values. */
	run = parser->ir->value_count;
	for (index = 0U; index < param_count; index++) {
		value = drv_gpu_spirv_new_value(parser);
		(void)drv_gpu_spirv_emit(parser,
					 DRV_GPU_IR_MOVE,
					 value,
					 params[index],
					 0U);
	}

	/* Four consecutive results. */
	*first = parser->ir->value_count;
	for (index = 0U; index < 4U; index++)
		(void)drv_gpu_spirv_new_value(parser);

	/*
	 * Emits the message: set in `location`, binding in `immediate`, the
	 * type and the offset in `component`.
	 */
	inst = drv_gpu_spirv_emit(parser,
				  DRV_GPU_IR_TEXTURE,
				  *first,
				  run,
				  param_count);
	if (inst != NULL) {
		inst->immediate = image->binding;
		inst->location = image->set;
		inst->component = message | (texel_offset << 8);
		drv_gpu_spirv_guard(parser, inst);
	}

	/* Succeeded unless the emit failed, which the parser latched. */
	return parser->error;
}

/*
 * Lowers OpImage: the image of a loaded combined image sampler names the same
 * binding.
 */
static int
drv_gpu_spirv_lower_image(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *sampled;
	struct drv_gpu_spirv_id *record;

	/* The instruction must carry the sampled image. */
	if (count != 4U)
		return EINVAL;

	/* Only the image of a loaded combined sampler is lowered. */
	sampled = drv_gpu_spirv_id(parser, word[3]);
	if (sampled == NULL || sampled->kind != ID_SAMPLED_IMAGE) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "image of something that is not a loaded sampler");
		return error;
	}

	/*
	 * The result names the same binding with the image's type; it emits
	 * nothing.
	 */
	record = drv_gpu_spirv_id(parser, word[2]);
	if (record == NULL || record->kind != ID_NONE)
		return EINVAL;
	record->kind = ID_SAMPLED_IMAGE;
	record->binding = sampled->binding;
	record->set = sampled->set;
	record->type = word[1];

	/* Succeeded: the image is named. */
	return 0;
}

/*
 * Lowers the samples drv_gpu_spirv_lower_sample() does not: of a 1D, 3D or
 * cube image or an array (the coordinate's component after the image's
 * dimensions is the layer), with a depth reference (OpImageSampleDref*),
 * projective (OpImageSampleProj*: the coordinate and the reference divided
 * by the component after the image's dimensions), with gradients (Grad),
 * or of an integer image; as Mesa builds the message on Gen12.0
 * (lower_sampler_logical_send(), brw_lower_logical_sends.cpp): the
 * reference, then the bias or the level, then the coordinate, each
 * component followed by its two gradients for sample_d, the layer last.
 * A 1D image is a 2D surface one texel high to the executor, so its
 * coordinate gets v = 0 and its layer moves to r.  An implicit level
 * outside a fragment shader is level 0.
 */
static int
drv_gpu_spirv_lower_texture(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	uint32_t declared_width;
	int error;
	struct drv_gpu_spirv_id *image;
	struct drv_gpu_spirv_id *type;
	struct drv_gpu_spirv_id *record;
	uint32_t coordinate[4];
	uint32_t position[4];
	uint32_t dx[4];
	uint32_t dy[4];
	uint32_t scalars[4];
	uint32_t params[DRV_GPU_IR_TEXTURE_MAX_PARAMS];
	uint32_t param_count;
	uint32_t coordinate_count;
	uint32_t scalar_count;
	uint32_t gradient_count;
	uint32_t dimensions;
	uint32_t position_count;
	uint32_t components;
	uint32_t operands;
	uint32_t next;
	uint32_t reference;
	uint32_t level;
	uint32_t divisor;
	uint32_t zero;
	uint32_t message;
	uint32_t texel_offset;
	uint32_t first;
	uint32_t index;
	int compare;
	int projective;
	int explicit_level;
	int has_bias;
	int has_level;
	int has_gradient;

	/* The instruction must carry the image and the coordinate. */
	if (count < 5U)
		return EINVAL;

	/*
	 * Resolves the image and its type; a 1D, 2D, 3D or cube image is
	 * lowered.
	 */
	image = drv_gpu_spirv_id(parser, word[3]);
	type = drv_gpu_spirv_image_type(parser, image);
	if (type == NULL) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "sample of something that is not a loaded sampler");
		return error;
	}

	/* Refuses image dimensions outside the frontend's supported sampler profile. */
	if (type->image_dim > DIM_CUBE) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "sample of an image that is not 1D, 2D, 3D or cube");
		return error;
	}

	/* Derives the coordinate dimensions from the declared image type. */
	dimensions = type->image_dim + 1U;
	if (type->image_dim == DIM_CUBE)
		dimensions = 3U;

	/*
	 * The kind of sample: a compare, a projective one, one at an explicit
	 * level.
	 */
	compare = 0;
	if (opcode == OP_IMAGE_SAMPLE_DREF_IMPLICIT_LOD ||
	    opcode == OP_IMAGE_SAMPLE_DREF_EXPLICIT_LOD ||
	    opcode == OP_IMAGE_SAMPLE_PROJ_DREF_IMPLICIT_LOD ||
	    opcode == OP_IMAGE_SAMPLE_PROJ_DREF_EXPLICIT_LOD)
		compare = 1;
	projective = 0;
	if (opcode >= OP_IMAGE_SAMPLE_PROJ_IMPLICIT_LOD &&
	    opcode <= OP_IMAGE_SAMPLE_PROJ_DREF_EXPLICIT_LOD)
		projective = 1;
	explicit_level = 0;
	if (opcode == OP_IMAGE_SAMPLE_EXPLICIT_LOD ||
	    opcode == OP_IMAGE_SAMPLE_DREF_EXPLICIT_LOD ||
	    opcode == OP_IMAGE_SAMPLE_PROJ_EXPLICIT_LOD ||
	    opcode == OP_IMAGE_SAMPLE_PROJ_DREF_EXPLICIT_LOD)
		explicit_level = 1;

	/*
	 * The coordinate: the image's dimensions, the layer of an array, the
	 * divisor of a projective sample.
	 */
	coordinate_count = drv_gpu_spirv_operand(parser, word[4], coordinate);
	position_count = dimensions + type->image_arrayed;
	if (projective != 0 && type->image_arrayed != 0U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "projective sample of an array");
		return error;
	}

	/* Requires every spatial and projective coordinate used by the sample. */
	if (coordinate_count < position_count + (uint32_t)projective) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "sample at a coordinate shorter than the image's");
		return error;
	}

	/* The depth reference of a compare. */
	next = 5U;
	reference = NO_VALUE;
	if (compare != 0) {
		scalar_count = 0U;
		if (next < count) {
			scalar_count =
			    drv_gpu_spirv_operand(parser, word[next], scalars);
		}

		/* Requires a scalar depth reference for a comparison sample. */
		if (scalar_count != 1U)
			return EINVAL;
		reference = scalars[0];
		next++;
	}

	/* The image operands. */
	operands = 0U;
	if (next < count) {
		operands = word[next];
		next++;
	}

	/* Refuses an operand that is not lowered. */
	if ((operands & ~(IMAGE_OPERAND_BIAS | IMAGE_OPERAND_LOD |
			  IMAGE_OPERAND_GRAD | IMAGE_OPERAND_CONST_OFFSET)) !=
	    0U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "sample with image operands other than Bias, Lod, Grad and "
		    "ConstOffset");
		return error;
	}

	/*
	 * Their values in the order of their bits: a bias of an implicit level.
	 */
	has_bias = 0;
	has_level = 0;
	has_gradient = 0;
	level = NO_VALUE;
	if ((operands & IMAGE_OPERAND_BIAS) != 0U) {
		scalar_count = 0U;
		if (explicit_level == 0 && next < count) {
			scalar_count =
			    drv_gpu_spirv_operand(parser, word[next], scalars);
		}

		/* Requires a scalar bias operand for the selected sample operation. */
		if (scalar_count != 1U)
			return EINVAL;
		has_bias = 1;
		level = scalars[0];
		next++;
	}

	/* An explicit level. */
	if ((operands & IMAGE_OPERAND_LOD) != 0U) {
		scalar_count = 0U;
		if (explicit_level != 0 && next < count) {
			scalar_count =
			    drv_gpu_spirv_operand(parser, word[next], scalars);
		}

		/* Requires a scalar level operand for the selected sample operation. */
		if (scalar_count != 1U)
			return EINVAL;
		has_level = 1;
		level = scalars[0];
		next++;
	}

	/* Gradients, one per dimension of the image. */
	if ((operands & IMAGE_OPERAND_GRAD) != 0U) {
		if (explicit_level == 0 || next + 1U >= count)
			return EINVAL;
		scalar_count = drv_gpu_spirv_operand(parser, word[next], dx);
		gradient_count =
		    drv_gpu_spirv_operand(parser, word[next + 1U], dy);
		if (scalar_count != dimensions ||
		    gradient_count != dimensions) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "sample with gradients that are not the image's "
			    "dimensions");
			return error;
		}

		/* Records that the sample carries explicit coordinate derivatives. */
		has_gradient = 1;
		next += 2U;
	}

	/* A constant offset moves the texel grid. */
	texel_offset = 0U;
	if ((operands & IMAGE_OPERAND_CONST_OFFSET) != 0U) {
		if (next >= count)
			return EINVAL;
		error = drv_gpu_spirv_texel_offset(parser,
						   word[next],
						   &texel_offset,
						   opcode,
						   offset);
		if (error != 0)
			return error;
		next++;
	}

	/*
	 * Every operand must have been read, and an explicit sample has a level
	 * or gradients.
	 */
	if (next != count)
		return EINVAL;
	if (explicit_level != 0 &&
	    has_level == 0 &&
	    has_gradient == 0)
		return EINVAL;

	/* The result: four floats or integers, or one float for a compare. */
	/*
	 * Reads the width before selecting the matching scalar representation.
	 */
	declared_width = drv_gpu_spirv_int_width(parser, word[1]);
	if (declared_width == 16U) {
		components = 0U;
	} else {
		components = drv_gpu_spirv_value_components(parser, word[1]);
	}

	/* Requires the result shape associated with comparison or ordinary sampling. */
	if ((compare == 0 &&
	    components != 4U) ||
	    (compare != 0 &&
	    components != 1U)) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "sample whose result is not a "
					     "vec4 (or a float of a compare)");
		return error;
	}

	/*
	 * A projective sample divides the coordinate and the reference by the
	 * component after the image's.
	 */
	for (index = 0U; index < position_count; index++)
		position[index] = coordinate[index];
	if (projective != 0) {
		divisor = drv_gpu_spirv_emit_value(parser,
						   DRV_GPU_IR_RCP,
						   coordinate[dimensions],
						   0U);
		for (index = 0U; index < dimensions; index++) {
			position[index] =
			    drv_gpu_spirv_emit_value(parser,
						     DRV_GPU_IR_FMUL,
						     coordinate[index],
						     divisor);
		}

		/* Prepends the comparison reference to the sampler operands. */
		if (reference != NO_VALUE) {
			reference = drv_gpu_spirv_emit_value(parser,
							     DRV_GPU_IR_FMUL,
							     reference,
							     divisor);
		}
	}

	/* An implicit level outside a fragment shader is level 0. */
	if (explicit_level == 0 &&
	    parser->ir->stage != DRV_GPU_STAGE_FRAGMENT) {
		has_bias = 0;
		has_level = 1;
		level = drv_gpu_spirv_float_constant(parser, FLOAT_ZERO_BITS);
	}

	/* The message the kind of sample takes. */
	if (has_gradient != 0) {
		message = DRV_GPU_IR_TEXTURE_SAMPLE_DERIVS;
		if (compare != 0)
			message = DRV_GPU_IR_TEXTURE_SAMPLE_DERIV_COMPARE;
	} else if (has_level != 0) {
		message = DRV_GPU_IR_TEXTURE_SAMPLE_LOD;
		if (compare != 0)
			message = DRV_GPU_IR_TEXTURE_SAMPLE_LOD_COMPARE;
	} else if (has_bias != 0) {
		message = DRV_GPU_IR_TEXTURE_SAMPLE_BIAS;
		if (compare != 0)
			message = DRV_GPU_IR_TEXTURE_SAMPLE_BIAS_COMPARE;
	} else {
		message = DRV_GPU_IR_TEXTURE_SAMPLE;
		if (compare != 0)
			message = DRV_GPU_IR_TEXTURE_SAMPLE_COMPARE;
	}

	/* The reference, then the bias or the level. */
	param_count = 0U;
	if (reference != NO_VALUE)
		drv_gpu_spirv_texture_param(params, &param_count, reference);
	if (has_bias != 0 || has_level != 0)
		drv_gpu_spirv_texture_param(params, &param_count, level);

	/*
	 * The coordinate, each component with its gradients; a 1D image's v is
	 * 0 (and so are its gradients).
	 */
	zero = NO_VALUE;
	if (type->image_dim == DIM_1D)
		zero = drv_gpu_spirv_float_constant(parser, FLOAT_ZERO_BITS);
	for (index = 0U; index < dimensions; index++) {
		drv_gpu_spirv_texture_param(params,
					    &param_count,
					    position[index]);
		if (has_gradient != 0) {
			drv_gpu_spirv_texture_param(params,
						    &param_count,
						    dx[index]);
			drv_gpu_spirv_texture_param(params,
						    &param_count,
						    dy[index]);
		}
	}

	/* A 1D image's v, and its gradients, after u. */
	if (type->image_dim == DIM_1D) {
		drv_gpu_spirv_texture_param(params, &param_count, zero);
		if (has_gradient != 0) {
			drv_gpu_spirv_texture_param(params, &param_count, zero);
			drv_gpu_spirv_texture_param(params, &param_count, zero);
		}
	}

	/*
	 * The layer of an array: r after a 1D or 2D coordinate, ai after a cube
	 * one.
	 */
	if (type->image_arrayed != 0U) {
		drv_gpu_spirv_texture_param(params,
					    &param_count,
					    position[dimensions]);
	}

	/* Emits the message. */
	error = drv_gpu_spirv_emit_texture(parser,
					   image,
					   params,
					   param_count,
					   message,
					   texel_offset,
					   &first);
	if (error != 0)
		return error;

	/* The result names the reply's first components. */
	record = drv_gpu_spirv_result(parser, word[2], word[1], components, 0);
	if (record == NULL)
		return EINVAL;
	for (index = 0U; index < components; index++)
		record->comp[index] = first + index;

	/* Succeeded: the sample is lowered. */
	return 0;
}

/*
 * Lowers OpImageFetch (texelFetch()): the ld message at an integer
 * coordinate and level, u v lod r on Gen9+ (Mesa's
 * lower_sampler_logical_send()); a constant offset is added to the
 * coordinate, as Mesa lowers it for ld (nir_lower_tex's lower_txf_offset).
 * A 1D image gets v = 0, its layer r; a texel buffer (a buffer surface)
 * is read the same way at level 0, as Mesa's ld of a buffer is.  A sample
 * of a multisampled 2D image is read as layer r of a 2D array, as the
 * executor describes such an image to a shader that samples it (its
 * samples are slices QPitch apart, MSFMT_MSS without a control surface);
 * the ld2dms_w message is not used.
 */
static int
drv_gpu_spirv_lower_fetch(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	uint32_t declared_width;
	uint32_t constant_scalar;
	int error;
	struct drv_gpu_spirv_id *image;
	struct drv_gpu_spirv_id *type;
	struct drv_gpu_spirv_id *record;
	uint32_t coordinate[4];
	uint32_t scalars[4];
	uint32_t params[DRV_GPU_IR_TEXTURE_MAX_PARAMS];
	uint32_t param_count;
	uint32_t coordinate_count;
	uint32_t position_count;
	uint32_t scalar_count;
	uint32_t operands;
	uint32_t next;
	uint32_t level;
	uint32_t texel_offset;
	uint32_t shift;
	uint32_t first;
	uint32_t index;
	uint32_t sample;
	int32_t moved;
	int buffer;

	/* The instruction must carry the image and the coordinate. */
	if (count < 5U)
		return EINVAL;

	/*
	 * Resolves the image; a 1D, 2D or 3D image, or a texel buffer, is
	 * fetched from.
	 */
	image = drv_gpu_spirv_id(parser, word[3]);
	type = drv_gpu_spirv_image_type(parser, image);
	if (type == NULL) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "fetch from something that is not an image");
		return error;
	}

	/*
	 * A texel buffer is fetched as a 1D image (its one coordinate, v = 0,
	 * level 0); it has no layers.
	 */
	buffer = 0;
	if (type->image_dim == DIM_BUFFER) {
		if (type->image_arrayed != 0U) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "fetch from an arrayed texel buffer");
			return error;
		}

		/* Selects the buffer-fetch interpretation of a texel-buffer image. */
		buffer = 1;
	} else if (type->image_dim > DIM_3D) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "fetch from something that is not a 1D, 2D or 3D image or "
		    "a texel buffer");
		return error;
	}

	/*
	 * A multisampled image is a 2D one, not an array (its samples are the
	 * executor's layers).
	 */
	if (type->image_ms != 0U &&
	    (type->image_dim != DIM_2D ||
	    type->image_arrayed != 0U)) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "fetch from a multisampled image "
					     "that is not 2D, or an array");
		return error;
	}

	/*
	 * The integer coordinate: the image's dimensions and the layer of an
	 * array (one for a texel buffer).
	 */
	coordinate_count = drv_gpu_spirv_operand(parser, word[4], coordinate);
	position_count = type->image_dim + 1U + type->image_arrayed;
	if (buffer != 0)
		position_count = 1U;
	if (coordinate_count != position_count) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "fetch at a coordinate that is not the image's");
		return error;
	}

	/* The image operands. */
	operands = 0U;
	next = 5U;
	if (next < count) {
		operands = word[next];
		next++;
	}

	/* Refuses an operand that is not lowered. */
	if ((operands & ~(IMAGE_OPERAND_LOD | IMAGE_OPERAND_CONST_OFFSET |
			  IMAGE_OPERAND_SAMPLE)) != 0U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error =
		    drv_gpu_spirv_refuse(parser,
					 opcode,
					 offset,
					 "fetch with image operands other than "
					 "Lod, ConstOffset and Sample");
		return error;
	}

	/*
	 * A multisampled image is fetched from one sample, which only it has.
	 */
	if (type->image_ms != 0U && (operands & IMAGE_OPERAND_SAMPLE) == 0U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "fetch from a multisampled image without a sample");
		return error;
	}

	/* Refuses a sample-number operand on a non-multisampled image. */
	if (type->image_ms == 0U && (operands & IMAGE_OPERAND_SAMPLE) != 0U)
		return EINVAL;

	/* The level. */
	level = NO_VALUE;
	if ((operands & IMAGE_OPERAND_LOD) != 0U) {
		scalar_count = 0U;
		if (next < count) {
			scalar_count =
			    drv_gpu_spirv_operand(parser, word[next], scalars);
		}

		/* Requires a scalar mip-level operand for the fetch message. */
		if (scalar_count != 1U)
			return EINVAL;
		level = scalars[0];
		next++;
	}

	/* The offset. */
	texel_offset = 0U;
	if ((operands & IMAGE_OPERAND_CONST_OFFSET) != 0U) {
		if (next >= count)
			return EINVAL;
		error = drv_gpu_spirv_texel_offset(parser,
						   word[next],
						   &texel_offset,
						   opcode,
						   offset);
		if (error != 0)
			return error;
		next++;
	}

	/*
	 * The sample (the operands come in bit order: Lod, ConstOffset, then
	 * Sample).
	 */
	sample = NO_VALUE;
	if ((operands & IMAGE_OPERAND_SAMPLE) != 0U) {
		scalar_count = 0U;
		if (next < count) {
			scalar_count =
			    drv_gpu_spirv_operand(parser, word[next], scalars);
		}

		/* Requires a scalar sample-number operand for the multisample fetch message. */
		if (scalar_count != 1U)
			return EINVAL;
		sample = scalars[0];
		next++;
	}

	/* Every operand must have been read. */
	if (next != count)
		return EINVAL;

	/* The result is four floats or integers. */
	/*
	 * Reads the width before selecting the matching scalar representation.
	 */
	declared_width = drv_gpu_spirv_int_width(parser, word[1]);
	if (declared_width == 16U) {
		scalar_count = 0U;
	} else {
		scalar_count = drv_gpu_spirv_value_components(parser, word[1]);
	}

	/* Requires the four-component result shape of this texel fetch. */
	if (scalar_count != 4U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error =
		    drv_gpu_spirv_refuse(parser,
					 opcode,
					 offset,
					 "fetch whose result is not a vec4");
		return error;
	}

	/* A texel buffer has no level and no offset. */
	if (buffer != 0 &&
	    (level != NO_VALUE ||
	    texel_offset != 0U)) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "fetch from a texel buffer with a level or an offset");
		return error;
	}

	/* Adds the offset to each dimension of the coordinate. */
	for (index = 0U; index <= type->image_dim && texel_offset != 0U;
	     index++) {
		moved = (int32_t)(((texel_offset >> (8U - 4U * index)) & 0xFU)
				  << 28) >>
			28;
		if (moved == 0)
			continue;
		shift = drv_gpu_spirv_integer_constant(parser, (uint32_t)moved);
		coordinate[index] = drv_gpu_spirv_emit_value(parser,
							     DRV_GPU_IR_IADD,
							     coordinate[index],
							     shift);
	}

	/*
	 * u, then v (0 for a 1D image), the level (0 when absent) and r (a
	 * layer, or a 3D image's depth).
	 */
	if (level == NO_VALUE)
		level = drv_gpu_spirv_integer_constant(parser, 0U);
	param_count = 0U;
	drv_gpu_spirv_texture_param(params, &param_count, coordinate[0]);
	if (type->image_dim == DIM_1D || buffer != 0) {

		/*
		 * Resolves the scalar operand before constructing its enclosing
		 * operation.
		 */
		constant_scalar = drv_gpu_spirv_integer_constant(parser, 0U);
		drv_gpu_spirv_texture_param(params,
					    &param_count,
					    constant_scalar);
		drv_gpu_spirv_texture_param(params, &param_count, level);
		if (type->image_arrayed != 0U) {
			drv_gpu_spirv_texture_param(params,
						    &param_count,
						    coordinate[1]);
		}
	} else {
		drv_gpu_spirv_texture_param(params,
					    &param_count,
					    coordinate[1]);
		drv_gpu_spirv_texture_param(params, &param_count, level);
		if (position_count > 2U) {
			drv_gpu_spirv_texture_param(params,
						    &param_count,
						    coordinate[2]);
		}
	}

	/*
	 * A sample of a multisampled image is its layer r: the executor reads
	 * its samples as an array (state.c).
	 */
	if (sample != NO_VALUE)
		drv_gpu_spirv_texture_param(params, &param_count, sample);

	/* Emits the message. */
	error = drv_gpu_spirv_emit_texture(parser,
					   image,
					   params,
					   param_count,
					   DRV_GPU_IR_TEXTURE_LD,
					   0U,
					   &first);
	if (error != 0)
		return error;

	/* The result names the reply. */
	record = drv_gpu_spirv_result(parser, word[2], word[1], 4U, 0);
	if (record == NULL)
		return EINVAL;
	for (index = 0U; index < 4U; index++)
		record->comp[index] = first + index;

	/* Succeeded: the fetch is lowered. */
	return 0;
}

/*
 * Lowers OpImageQuerySizeLod, OpImageQuerySize (textureSize()) and
 * OpImageQueryLevels (textureQueryLevels()) to the resinfo message, whose
 * reply is the width, the height, the depth (a 3D image's, an array's
 * layers) and the level count at the level asked for (Mesa's TXS); a 1D
 * array's layers are its depth, as the executor makes it a 2D surface.
 */
static int
drv_gpu_spirv_lower_query(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	uint32_t declared_width;
	int error;
	struct drv_gpu_spirv_id *image;
	struct drv_gpu_spirv_id *type;
	struct drv_gpu_spirv_id *record;
	uint32_t scalars[4];
	uint32_t params[DRV_GPU_IR_TEXTURE_MAX_PARAMS];
	uint32_t param_count;
	uint32_t scalar_count;
	uint32_t components;
	uint32_t expected;
	uint32_t level;
	uint32_t first;
	uint32_t index;

	/*
	 * The instruction must carry the image, and the level for
	 * OpImageQuerySizeLod.
	 */
	if (opcode == OP_IMAGE_QUERY_SIZE_LOD && count != 5U)
		return EINVAL;
	if (opcode != OP_IMAGE_QUERY_SIZE_LOD && count != 4U)
		return EINVAL;

	/* Resolves the image. */
	image = drv_gpu_spirv_id(parser, word[3]);
	type = drv_gpu_spirv_image_type(parser, image);
	if (type == NULL || type->image_dim > DIM_CUBE) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "query of something that is not a "
					     "1D, 2D, 3D or cube image");
		return error;
	}

	/* The level asked for; level 0 without one. */
	if (opcode == OP_IMAGE_QUERY_SIZE_LOD) {
		scalar_count = drv_gpu_spirv_operand(parser, word[4], scalars);
		if (scalar_count != 1U)
			return EINVAL;
		level = scalars[0];
	} else {
		level = drv_gpu_spirv_integer_constant(parser, 0U);
	}

	/*
	 * The result: the level count, or the image's dimensions and its
	 * layers.
	 */
	/*
	 * Reads the width before selecting the matching scalar representation.
	 */
	declared_width = drv_gpu_spirv_int_width(parser, word[1]);
	if (declared_width == 32U) {
		components = drv_gpu_spirv_int_components(parser, word[1]);
	} else {
		components = 0U;
	}

	/* Starts the image-query result width from its non-array dimensions. */
	expected = 1U;
	if (opcode != OP_IMAGE_QUERY_LEVELS) {
		expected = type->image_dim + 1U;
		if (type->image_dim == DIM_CUBE)
			expected = 2U;
		expected += type->image_arrayed;
	}

	/* Refuses a result of another size. */
	if (components != expected) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "query whose result is not the image's size");
		return error;
	}

	/* Emits the message. */
	param_count = 0U;
	drv_gpu_spirv_texture_param(params, &param_count, level);
	error = drv_gpu_spirv_emit_texture(parser,
					   image,
					   params,
					   param_count,
					   DRV_GPU_IR_TEXTURE_RESINFO,
					   0U,
					   &first);
	if (error != 0)
		return error;

	/* The result names the reply. */
	record = drv_gpu_spirv_result(parser, word[2], word[1], components, 0);
	if (record == NULL)
		return EINVAL;

	/*
	 * The level count, or the size with a 1D array's layers from the depth.
	 */
	if (opcode == OP_IMAGE_QUERY_LEVELS) {
		record->comp[0] = first + 3U;
	} else {
		for (index = 0U; index < components; index++)
			record->comp[index] = first + index;
		if (type->image_dim == DIM_1D && type->image_arrayed != 0U)
			record->comp[1] = first + 2U;
	}

	/* Succeeded: the query is lowered. */
	return 0;
}

/*
 * Lowers the derivatives per component: OpDPdx, OpDPdy and their Coarse
 * forms to the coarse differences, OpDPdxFine and OpDPdyFine to the fine
 * ones, and OpFwidth (and its Coarse and Fine forms) to |dx| + |dy| of the
 * same kind, as Mesa lowers it (vtn_alu.c: fabs(ddx) + fabs(ddy)); an
 * unqualified derivative may be coarse (the Vulkan specification), and anv
 * asks for coarse ones.
 */
static int
drv_gpu_spirv_lower_derivative(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *record;
	uint32_t operand[4];
	uint32_t operand_count;
	uint32_t components;
	uint32_t index;
	uint32_t across;
	uint32_t down;

	/* The instruction must carry its operand. */
	if (count != 4U)
		return EINVAL;

	/* Only a fragment shader has neighbouring pixels. */
	if (parser->ir->stage != DRV_GPU_STAGE_FRAGMENT) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "derivative outside a fragment shader");
		return error;
	}

	/*
	 * The operand and the result are float scalars or vectors of one size.
	 */
	operand_count = drv_gpu_spirv_operand(parser, word[3], operand);
	components = drv_gpu_spirv_float_components(parser, word[1]);
	if (components == 0U || operand_count != components) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "derivative of something that is "
					     "not a float scalar or vector");
		return error;
	}

	/* Declares the result; its scalars are named below. */
	record = drv_gpu_spirv_result(parser, word[2], word[1], components, 0);
	if (record == NULL)
		return EINVAL;

	/* Lowers each component on its own. */
	for (index = 0U; index < components; index++) {
		/* A derivative along one axis is one difference. */
		if (opcode == OP_DPDX || opcode == OP_DPDX_COARSE) {
			record->comp[index] =
			    drv_gpu_spirv_emit_value(parser,
						     DRV_GPU_IR_DDX,
						     operand[index],
						     0U);
			continue;
		}

		/* The fine x derivative takes each row's own difference. */
		if (opcode == OP_DPDX_FINE) {
			record->comp[index] =
			    drv_gpu_spirv_emit_value(parser,
						     DRV_GPU_IR_DDX_FINE,
						     operand[index],
						     0U);
			continue;
		}

		/*
		 * The coarse y derivative takes the left column's difference.
		 */
		if (opcode == OP_DPDY || opcode == OP_DPDY_COARSE) {
			record->comp[index] =
			    drv_gpu_spirv_emit_value(parser,
						     DRV_GPU_IR_DDY,
						     operand[index],
						     0U);
			continue;
		}

		/* The fine y derivative takes each column's own difference. */
		if (opcode == OP_DPDY_FINE) {
			record->comp[index] =
			    drv_gpu_spirv_emit_value(parser,
						     DRV_GPU_IR_DDY_FINE,
						     operand[index],
						     0U);
			continue;
		}

		/*
		 * The width is the sum of the two differences' magnitudes, fine
		 * ones for OpFwidthFine.
		 */
		if (opcode == OP_FWIDTH_FINE) {
			across = drv_gpu_spirv_emit_value(parser,
							  DRV_GPU_IR_DDX_FINE,
							  operand[index],
							  0U);
			down = drv_gpu_spirv_emit_value(parser,
							DRV_GPU_IR_DDY_FINE,
							operand[index],
							0U);
		} else {
			across = drv_gpu_spirv_emit_value(parser,
							  DRV_GPU_IR_DDX,
							  operand[index],
							  0U);
			down = drv_gpu_spirv_emit_value(parser,
							DRV_GPU_IR_DDY,
							operand[index],
							0U);
		}

		/* The magnitudes' sum. */
		across = drv_gpu_spirv_emit_value(parser,
						  DRV_GPU_IR_FABS,
						  across,
						  0U);
		down =
		    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FABS, down, 0U);
		record->comp[index] = drv_gpu_spirv_emit_value(parser,
							       DRV_GPU_IR_FADD,
							       across,
							       down);
	}

	/* Succeeded: the derivative is lowered. */
	return 0;
}

/*
 * Opens a block: finds the predicate of the channels that run it.
 *
 * The entry block runs for every channel.  The merge block of a selection
 * runs for the channels that ran its header, unless a block inside leaked
 * (see struct drv_gpu_spirv_construct); the merge block of a loop runs for the
 * channels that entered the loop; any other block, and a merge block after
 * a leak, runs for the channels of its incoming edges.  A block no edge
 * reaches runs for no channel.  A loop header then runs for the channels
 * still in its loop (see drv_gpu_spirv_loop_open()).
 */
static int
drv_gpu_spirv_lower_label(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *record;
	struct drv_gpu_spirv_construct *construct;
	struct drv_gpu_spirv_loop *loop;
	struct drv_gpu_shader_ir_inst *inst;
	uint32_t predicate;
	uint32_t merge;
	uint32_t continue_target;
	uint32_t index;
	int reached;
	int always;
	int loop_merge;
	int header;

	/* Resolves the label, which must be fresh. */
	record = NULL;
	if (count >= 2U)
		record = drv_gpu_spirv_id(parser, word[1]);
	if (record == NULL || record->kind != ID_NONE)
		return EINVAL;

	/* The block before must have ended with its terminator. */
	if (parser->block != 0U && parser->terminated == 0)
		return EINVAL;

	/*
	 * A merge block of an outer construct before the inner one's is not the
	 * structured order this walk follows.
	 */
	for (index = 0U; index + 1U < parser->depth; index++) {
		if (parser->constructs[index].merge == word[1]) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "merge block before the merge block of a construct "
			    "inside it");
			return error;
		}
	}

	/* After a loop's back edge only its merge block may follow. */
	loop = NULL;
	if (parser->loop_depth != 0U)
		loop = &parser->loops[parser->loop_depth - 1U];
	if (loop != NULL &&
	    loop->closed != 0 &&
	    loop->merge != word[1]) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "block after a loop's back edge "
					     "that is not its merge block");
		return error;
	}

	/* Finds the predicate of the block. */
	loop_merge = 0;
	construct = NULL;
	if (parser->depth != 0U &&
	    parser->constructs[parser->depth - 1U].merge == word[1])
		construct = &parser->constructs[parser->depth - 1U];
	if (parser->block == 0U) {
		/* The entry block. */
		predicate = PREDICATE_ALWAYS;
	} else if (loop != NULL && loop->merge == word[1]) {
		/*
		 * A loop's merge block: every channel that entered the loop is
		 * back.
		 */
		if (loop->closed == 0) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "loop merge block before the loop's back edge");
			return error;
		}

		/* Restores the predicate of the channels that entered the loop. */
		predicate = loop->entry_predicate;
		error = drv_gpu_spirv_loop_close(parser, opcode, offset);
		if (error != 0)
			return error;
		loop_merge = 1;
		construct = NULL;
	} else if (construct != NULL && construct->leaky == 0) {
		/* Every channel that ran the header is back. */
		predicate = construct->predicate;
	} else {
		/*
		 * The channels of the incoming edges: the or of their
		 * predicates.
		 */
		predicate = NO_VALUE;
		reached = 0;
		always = 0;
		for (index = 0U; index < parser->edge_count; index++) {
			if (parser->edges[index].to != word[1])
				continue;
			reached = 1;
			if (parser->edges[index].predicate ==
			    PREDICATE_ALWAYS) {
				always = 1;
			} else if (predicate == NO_VALUE) {
				predicate = parser->edges[index].predicate;
			} else {
				predicate = drv_gpu_spirv_emit_value(
				    parser,
				    DRV_GPU_IR_OR,
				    predicate,
				    parser->edges[index].predicate);
			}
		}

		/*
		 * An edge every channel takes, or no edge at all: no channel, a
		 * false Boolean.
		 */
		if (always != 0) {
			predicate = PREDICATE_ALWAYS;
		} else if (reached == 0) {
			predicate = drv_gpu_spirv_new_value(parser);
			inst = drv_gpu_spirv_emit(parser,
						  DRV_GPU_IR_BOOL,
						  predicate,
						  0U,
						  0U);
			if (inst != NULL)
				inst->immediate = 0U;
		}
	}

	/* The merge block of a selection closes its construct. */
	if (construct != NULL)
		parser->depth--;

	/*
	 * A block whose terminator follows an OpLoopMerge is a loop header: its
	 * loop opens here.
	 */
	header = drv_gpu_spirv_find_loop_merge(parser,
					       offset + count,
					       &merge,
					       &continue_target);
	if (header != 0) {
		error = drv_gpu_spirv_loop_open(parser,
						word[1],
						merge,
						continue_target,
						&predicate,
						opcode,
						offset);
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

	/*
	 * A block not every channel runs is a skippable region, unless it heads
	 * a loop (its loop begins inside it).
	 */
	if (predicate != PREDICATE_ALWAYS && header == 0)
		drv_gpu_spirv_skip_open(parser);

	/* Succeeded: the block is open. */
	return 0;
}

/*
 * Looks ahead from the first instruction of a block to its terminator for
 * an OpLoopMerge, which makes the block a loop header; reports its merge
 * block and continue target.
 */
static int
drv_gpu_spirv_find_loop_merge(
	struct drv_gpu_spirv_parser *parser,
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

		/*
		 * A malformed instruction ends the look; the body walk reports
		 * it.
		 */
		if (count == 0U || offset + count > parser->words)
			return 0;

		/*
		 * The OpLoopMerge names the loop's merge block and continue
		 * target.
		 */
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

		/* Advances beyond the inspected loop instruction. */
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
 * header's phis (drv_gpu_spirv_loop_begin()).
 */
static int
drv_gpu_spirv_loop_open(
	struct drv_gpu_spirv_parser *parser,
	uint32_t header,
	uint32_t merge,
	uint32_t continue_target,
	uint32_t *predicate,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_loop *loop;
	struct drv_gpu_spirv_construct *construct;
	struct drv_gpu_spirv_id *variable;
	uint32_t *slot;
	uint32_t entry;
	uint32_t id;
	uint32_t component;
	uint32_t carried;

	/* Loops nested deeper than the parser follows are refused. */
	if (parser->loop_depth >= MAX_LOOP_DEPTH) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "loops nested too deep");
		return error;
	}

	/* Refuses another selection construct when the fixed construct stack is full. */
	if (parser->depth >= MAX_CONSTRUCT_DEPTH) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "constructs nested too deep");
		return error;
	}

	/* Describes the loop. */
	loop = &parser->loops[parser->loop_depth];
	kern_memset(loop, 0, sizeof(*loop));
	loop->header = header;
	loop->merge = merge;
	loop->continue_target = continue_target;
	loop->entry_predicate = *predicate;
	loop->carried_first = parser->carried_count;

	/*
	 * The channels in the loop start as the channels that enter it, all of
	 * them when every channel does.
	 */
	entry = *predicate;
	if (entry == PREDICATE_ALWAYS) {
		entry = drv_gpu_spirv_shared_constant(parser,
						      &parser->true_value,
						      DRV_GPU_IR_BOOL,
						      BOOL_TRUE_BITS);
	}

	/* Creates the loop-active scalar before any body reader can use it. */
	loop->active = drv_gpu_spirv_move_value(parser, NO_VALUE, entry);

	/*
	 * Every component a local or an output holds becomes a loop variable.
	 */
	for (id = 0U; id < parser->bound; id++) {
		variable = &parser->ids[id];
		if (variable->kind != ID_VARIABLE)
			continue;
		if (variable->ptr_kind != PTR_LOCAL &&
		    variable->ptr_kind != PTR_OUTPUT &&
		    variable->ptr_kind != PTR_OUTPUT_BLOCK)
			continue;

		/*
		 * Moves each held slot into its loop variable and remembers it.
		 */
		for (component = 0U; component < variable->slot_count;
		     component++) {
			slot = drv_gpu_spirv_variable_slot(parser,
							   variable,
							   component);
			if (slot == NULL || *slot == NO_VALUE)
				continue;
			carried =
			    drv_gpu_spirv_move_value(parser, NO_VALUE, *slot);
			*slot = carried;
			error =
			    drv_gpu_spirv_carry(parser, id, component, carried);
			if (error != 0)
				return error;
		}
	}

	/*
	 * Opens the loop's construct: a break leaves the constructs inside it.
	 */
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

/*
 * Starts a loop's body: LOOP_BEGIN, after which every IR value is made inside
 * the loop.
 */
static void
drv_gpu_spirv_loop_begin(
	struct drv_gpu_spirv_parser *parser,
	struct drv_gpu_spirv_loop *loop)
{
	/* Emits the mark the loop's end jumps back to. */
	(void)drv_gpu_spirv_emit(parser, DRV_GPU_IR_LOOP_BEGIN, 0U, 0U, 0U);

	/* The values made from here on belong to the loop. */
	loop->first_value = parser->ir->value_count;
	loop->begun = 1;

	/* Succeeded: the loop-carried definitions precede their readers. */
	return;
}

/*
 * Lowers OpLoopMerge: the header's loop opened at its label; the operands must
 * agree with it.
 */
static int
drv_gpu_spirv_lower_loop_merge(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_loop *loop;

	/* The instruction names the merge block and the continue target. */
	if (count < 4U)
		return EINVAL;

	/* It belongs to the header of the innermost loop. */
	if (parser->loop_depth == 0U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error =
		    drv_gpu_spirv_refuse(parser,
					 opcode,
					 offset,
					 "OpLoopMerge outside a loop header");
		return error;
	}

	/* Finds the innermost loop whose carried values this block updates. */
	loop = &parser->loops[parser->loop_depth - 1U];
	if (loop->header != parser->block || loop->begun == 0) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error =
		    drv_gpu_spirv_refuse(parser,
					 opcode,
					 offset,
					 "OpLoopMerge outside a loop header");
		return error;
	}

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
drv_gpu_spirv_loop_back_edge(
	struct drv_gpu_spirv_parser *parser,
	uint32_t predicate,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_loop *loop;
	struct drv_gpu_spirv_id *record;
	struct drv_gpu_spirv_id *variable;
	struct drv_gpu_spirv_carried *carried;
	uint32_t *slot;
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
	if (loop->closed != 0) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error =
		    drv_gpu_spirv_refuse(parser,
					 opcode,
					 offset,
					 "loop with more than one back edge");
		return error;
	}

	/*
	 * The channels that go round again; every channel when the edge has no
	 * condition.
	 */
	condition = predicate;
	if (condition == PREDICATE_ALWAYS) {
		condition = drv_gpu_spirv_shared_constant(parser,
							  &parser->true_value,
							  DRV_GPU_IR_BOOL,
							  BOOL_TRUE_BITS);
	}

	/*
	 * Makes each header phi's new value: the back edge's value where the
	 * channels go round again.
	 */
	for (phi = 0U; phi < loop->phi_count; phi++) {
		record = drv_gpu_spirv_id(parser, loop->phi_result[phi]);
		back_count =
		    drv_gpu_spirv_operand(parser, loop->phi_back[phi], back);
		if (record == NULL || back_count != record->count) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error =
			    drv_gpu_spirv_refuse(parser,
						 opcode,
						 offset,
						 "OpPhi value of another size "
						 "on a loop's back edge");
			return error;
		}

		/* Moves each back-edge scalar into its loop-carried definition. */
		for (component = 0U; component < back_count; component++) {
			updated[phi][component] =
			    drv_gpu_spirv_select_value(parser,
						       condition,
						       back[component],
						       record->comp[component]);
		}
	}

	/* Makes a copy of each carried slot that changed in the pass. */
	for (index = loop->carried_first; index < parser->carried_count;
	     index++) {
		carried = &parser->carried[index];
		variable = &parser->ids[carried->variable];
		slot = drv_gpu_spirv_variable_slot(parser,
						   variable,
						   carried->component);
		if (slot == NULL)
			return EINVAL;
		current = *slot;
		if (current == carried->value)
			continue;
		*slot = drv_gpu_spirv_move_value(parser, NO_VALUE, current);
	}

	/* Moves the phis' new values into their loop variables. */
	for (phi = 0U; phi < loop->phi_count; phi++) {
		record = drv_gpu_spirv_id(parser, loop->phi_result[phi]);
		for (component = 0U; component < record->count; component++) {
			(void)drv_gpu_spirv_move_value(parser,
						       record->comp[component],
						       updated[phi][component]);
		}
	}

	/*
	 * Moves the carried slots' copies into their loop variables, which the
	 * variables hold again.
	 */
	for (index = loop->carried_first; index < parser->carried_count;
	     index++) {
		carried = &parser->carried[index];
		variable = &parser->ids[carried->variable];
		slot = drv_gpu_spirv_variable_slot(parser,
						   variable,
						   carried->component);
		if (slot == NULL)
			return EINVAL;
		current = *slot;
		if (current == carried->value)
			continue;
		(void)drv_gpu_spirv_move_value(parser, carried->value, current);
		*slot = carried->value;
	}

	/* The channels in the loop are the ones that go round again. */
	if (condition != loop->active)
		(void)drv_gpu_spirv_move_value(parser, loop->active, condition);

	/* Ends the pass: the body runs again while a channel is in the loop. */
	(void)drv_gpu_spirv_emit(parser,
				 DRV_GPU_IR_LOOP_END,
				 0U,
				 loop->active,
				 0U);
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
drv_gpu_spirv_loop_close(
	struct drv_gpu_spirv_parser *parser,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_loop *loop;
	struct drv_gpu_spirv_id *record;
	uint32_t first;
	uint32_t end;
	uint32_t id;

	/* The loop's construct must be the innermost one left. */
	loop = &parser->loops[parser->loop_depth - 1U];
	if (parser->depth != loop->construct + 1U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "construct inside a loop still "
					     "open at the loop's merge block");
		return error;
	}

	/* The value range stays for the reads after the loop. */
	if (parser->closed_count >= MAX_LOOPS) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "more loops than supported");
		return error;
	}

	/* Marks the scalar range that cannot escape a completed loop. */
	first = loop->first_value;
	end = parser->ir->value_count;
	parser->closed[parser->closed_count].first = first;
	parser->closed[parser->closed_count].end = end;
	parser->closed_count++;

	/*
	 * A constant first used in the loop is used again as a fresh constant.
	 */
	for (id = 0U; id < parser->bound; id++) {
		record = &parser->ids[id];
		if (record->kind != ID_CONSTANT || record->count == 0U)
			continue;
		if (record->comp[0] >= first && record->comp[0] < end)
			record->count = 0U;
	}

	/* So is a constant the lowering introduced in it. */
	if (parser->zero_value != NO_VALUE &&
	    parser->zero_value >= first &&
	    parser->zero_value < end)
		parser->zero_value = NO_VALUE;
	if (parser->one_value != NO_VALUE &&
	    parser->one_value >= first &&
	    parser->one_value < end)
		parser->one_value = NO_VALUE;
	if (parser->true_value != NO_VALUE &&
	    parser->true_value >= first &&
	    parser->true_value < end)
		parser->true_value = NO_VALUE;
	if (parser->int_16_value != NO_VALUE &&
	    parser->int_16_value >= first &&
	    parser->int_16_value < end)
		parser->int_16_value = NO_VALUE;
	if (parser->int_ffff_value != NO_VALUE &&
	    parser->int_ffff_value >= first &&
	    parser->int_ffff_value < end)
		parser->int_ffff_value = NO_VALUE;

	/* Drops the loop's construct, its carried components and the loop. */
	parser->depth--;
	parser->carried_count = loop->carried_first;
	parser->loop_depth--;

	/* Succeeded: the walk is after the loop. */
	return 0;
}

/* Remembers a component a loop carries, growing the list when it is full. */
static int
drv_gpu_spirv_carry(
	struct drv_gpu_spirv_parser *parser,
	uint32_t variable,
	uint32_t component,
	uint32_t value)
{
	struct drv_gpu_spirv_carried *grown;
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
			kern_memcpy(grown,
				    parser->carried,
				    parser->carried_count * sizeof(*grown));
			kern_free(parser->carried);
		}

		/* Publishes the larger list. */
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
 * drv_gpu_spirv_lower_header_phi()); a phi of a loop's merge block is refused,
 * since its edges were taken in different passes of the loop.
 */
static int
drv_gpu_spirv_lower_phi(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *record;
	struct drv_gpu_spirv_loop *loop;
	struct drv_gpu_shader_ir_inst *inst;
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
	if (parser->loop_merge_block != 0) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "OpPhi in a loop's merge block");
		return error;
	}

	/* A loop header's phi carries a value from one pass to the next. */
	if (parser->loop_depth != 0U) {
		loop = &parser->loops[parser->loop_depth - 1U];
		if (loop->header == parser->block && loop->begun == 0) {
			/*
			 * Lowers this instruction while preserving the frontend
			 * refusal report.
			 */
			error = drv_gpu_spirv_lower_header_phi(parser,
							       loop,
							       word,
							       count,
							       opcode,
							       offset);
			if (error != 0)
				return error;

			/*
			 * Succeeded: this instruction has a complete scalar
			 * lowering.
			 */
			return 0;
		}
	}

	/* The result must be a scalar or a vector. */
	components = drv_gpu_spirv_value_components(parser, word[1]);
	if (components == 0U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "OpPhi of something that is not a scalar / vector");
		return error;
	}

	/* Declares the result; its scalars are named by the selections. */
	record = drv_gpu_spirv_result(parser, word[2], word[1], components, 0);
	if (record == NULL)
		return EINVAL;

	/*
	 * Merges the incoming values in the order the instruction lists them.
	 */
	first = 1;
	for (index = 3U; index + 1U < count; index += 2U) {
		/*
		 * An edge from a block that never branched here (a block no
		 * channel runs) adds nothing.
		 */
		predicate = drv_gpu_spirv_edge_predicate(parser,
							 word[index + 1U],
							 parser->block,
							 &found);
		if (found == 0)
			continue;

		/*
		 * Resolves the value the edge brings; it may emit a constant.
		 */
		incoming_count =
		    drv_gpu_spirv_operand(parser, word[index], incoming);
		if (incoming_count != components) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error =
			    drv_gpu_spirv_refuse(parser,
						 opcode,
						 offset,
						 "OpPhi value of another size");
			return error;
		}

		/*
		 * The first value is taken as it is; a later one where its
		 * edge's predicate holds.
		 */
		for (component = 0U; component < components; component++) {
			if (first != 0 || predicate == PREDICATE_ALWAYS) {
				record->comp[component] = incoming[component];
				continue;
			}

			/*
			 * Selects the edge's value on its channels, the value
			 * so far elsewhere.
			 */
			merged = drv_gpu_spirv_new_value(parser);
			inst = drv_gpu_spirv_emit(parser,
						  DRV_GPU_IR_SELECT,
						  merged,
						  predicate,
						  incoming[component]);
			if (inst != NULL)
				inst->src[2] = record->comp[component];
			record->comp[component] = merged;
		}

		/*
		 * Every later edge's value is taken where its predicate holds.
		 */
		first = 0;
	}

	/* A phi of a block no edge reaches keeps the first value listed. */
	if (first != 0) {
		incoming_count =
		    drv_gpu_spirv_operand(parser, word[3], incoming);
		if (incoming_count != components) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error =
			    drv_gpu_spirv_refuse(parser,
						 opcode,
						 offset,
						 "OpPhi value of another size");
			return error;
		}

		/* Carries each local component into its loop-header definition. */
		for (component = 0U; component < components; component++)
			record->comp[component] = incoming[component];
	}

	/* Succeeded: the phi names its scalars. */
	return 0;
}

/*
 * Lowers the phi of a loop header: a loop variable per component, moved
 * into from the value of the edge that enters the loop now, and from the
 * value of the back edge at the loop's end (drv_gpu_spirv_loop_back_edge()).
 * The edge into the loop comes from a block already lowered; the back edge
 * from a block of the loop, not lowered yet.
 */
static int
drv_gpu_spirv_lower_header_phi(
	struct drv_gpu_spirv_parser *parser,
	struct drv_gpu_spirv_loop *loop,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *record;
	struct drv_gpu_spirv_id *parent;
	uint32_t incoming[4];
	uint32_t incoming_count;
	uint32_t components;
	uint32_t back;
	uint32_t index;
	uint32_t component;
	int entered;

	/*
	 * The result must be a scalar or a vector, and the header must have
	 * room for another phi.
	 */
	components = drv_gpu_spirv_value_components(parser, word[1]);
	if (components == 0U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "OpPhi of something that is not a scalar / vector");
		return error;
	}

	/* Refuses another header phi when the loop's phi table is full. */
	if (loop->phi_count >= MAX_LOOP_PHIS) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "loop header with more phis than supported");
		return error;
	}

	/*
	 * Declares the result; its scalars are the loop variables made below.
	 */
	record = drv_gpu_spirv_result(parser, word[2], word[1], components, 0);
	if (record == NULL)
		return EINVAL;

	/*
	 * Sorts the edges: the one into the loop gives the value now, the back
	 * edge its id.
	 */
	entered = 0;
	back = NO_VALUE;
	for (index = 3U; index + 1U < count; index += 2U) {
		parent = drv_gpu_spirv_id(parser, word[index + 1U]);
		if (parent == NULL)
			return EINVAL;

		/*
		 * A parent not lowered yet is inside the loop: the back edge.
		 */
		if (parent->kind != ID_LABEL) {
			if (back != NO_VALUE) {
				/*
				 * Records the unsupported instruction in the
				 * caller's diagnostic.
				 */
				error = drv_gpu_spirv_refuse(
				    parser,
				    opcode,
				    offset,
				    "loop header phi with more than one back "
				    "edge");
				return error;
			}

			/* Resolves the back-edge value for the loop-header phi. */
			back = word[index];
			continue;
		}

		/* The edge into the loop; one of them only. */
		if (entered != 0) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error =
			    drv_gpu_spirv_refuse(parser,
						 opcode,
						 offset,
						 "loop header phi with more "
						 "than one edge into the loop");
			return error;
		}

		/* Resolves the entry-edge scalars before recording their carried definitions. */
		incoming_count =
		    drv_gpu_spirv_operand(parser, word[index], incoming);
		if (incoming_count != components) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error =
			    drv_gpu_spirv_refuse(parser,
						 opcode,
						 offset,
						 "OpPhi value of another size");
			return error;
		}

		/* Records that this phi has an entry edge from outside the loop. */
		entered = 1;
	}

	/* A header phi needs both edges. */
	if (entered == 0 || back == NO_VALUE) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "loop header phi without an edge "
					     "into the loop and a back edge");
		return error;
	}

	/*
	 * Each component becomes a loop variable holding the value that enters
	 * the loop.
	 */
	for (component = 0U; component < components; component++) {
		record->comp[component] =
		    drv_gpu_spirv_move_value(parser,
					     NO_VALUE,
					     incoming[component]);
	}

	/* Remembers the phi for the back edge. */
	loop->phi_result[loop->phi_count] = word[2];
	loop->phi_back[loop->phi_count] = back;
	loop->phi_count++;

	/* Succeeded: the phi is a set of loop variables. */
	return 0;
}

/*
 * Lowers OpBranch: every channel of the block takes the edge; to the loop
 * header it is the back edge.
 */
static int
drv_gpu_spirv_lower_branch(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;

	/* The instruction names its target. */
	if (count != 2U)
		return EINVAL;

	/*
	 * A header needs a conditional branch here: a selection of one way is
	 * not structured.
	 */
	if (parser->pending_merge != NO_VALUE) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error =
		    drv_gpu_spirv_refuse(parser,
					 opcode,
					 offset,
					 "OpSelectionMerge before an OpBranch");
		return error;
	}

	/* A branch to the innermost loop's header ends a pass of the loop. */
	if (parser->loop_depth != 0U &&
	    word[1] == parser->loops[parser->loop_depth - 1U].header) {
		error = drv_gpu_spirv_loop_back_edge(parser,
						     parser->predicate,
						     opcode,
						     offset);
		if (error != 0)
			return error;
		parser->terminated = 1;
		return 0;
	}

	/* Records the edge and ends the block. */
	error = drv_gpu_spirv_edge_add(parser,
				       word[1],
				       parser->predicate,
				       opcode,
				       offset);
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
drv_gpu_spirv_lower_branch_conditional(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_construct *construct;
	uint32_t condition[4];
	uint32_t condition_count;
	uint32_t negated;
	uint32_t taken;
	uint32_t other;
	uint32_t header;

	/*
	 * The instruction names the condition and both targets; branch weights
	 * may follow.
	 */
	if (count != 4U && count != 6U)
		return EINVAL;

	/* The condition is one Boolean. */
	condition_count = drv_gpu_spirv_operand(parser, word[1], condition);
	if (condition_count != 1U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "branch condition that is not a Boolean scalar");
		return error;
	}

	/* Notes the innermost loop's header, which a branch may go back to. */
	header = NO_VALUE;
	if (parser->loop_depth != 0U)
		header = parser->loops[parser->loop_depth - 1U].header;

	/*
	 * A header opens its construct before its edges are checked against it.
	 */
	if (parser->pending_merge != NO_VALUE) {
		if (word[2] == header || word[3] == header) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "selection header that branches back to its loop's "
			    "header");
			return error;
		}

		/* Refuses a selection whose construct stack exceeds the fixed depth limit. */
		if (parser->depth >= MAX_CONSTRUCT_DEPTH) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "selection constructs nested too deep");
			return error;
		}

		/* Publishes this selection's merge block in the construct stack. */
		construct = &parser->constructs[parser->depth];
		construct->merge = parser->pending_merge;
		construct->predicate = parser->predicate;
		construct->leaky = 0;
		construct->loop = 0;
		parser->depth++;
		parser->pending_merge = NO_VALUE;
	}

	/*
	 * Both targets the same: every channel takes the one edge, and the
	 * block ends.
	 */
	if (word[2] == word[3]) {
		if (word[2] == header) {
			error = drv_gpu_spirv_loop_back_edge(parser,
							     parser->predicate,
							     opcode,
							     offset);
		} else {
			error = drv_gpu_spirv_edge_add(parser,
						       word[2],
						       parser->predicate,
						       opcode,
						       offset);
		}

		/* Reports why the edge could not be recorded. */
		if (error != 0)
			return error;
		parser->terminated = 1;

		/* Succeeded: the one edge is recorded. */
		return 0;
	}

	/*
	 * The channels of the block where the condition holds, and where it
	 * does not.
	 */
	taken = drv_gpu_spirv_predicate_and(parser,
					    parser->predicate,
					    condition[0]);
	negated =
	    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_NOT, condition[0], 0U);
	other = drv_gpu_spirv_predicate_and(parser, parser->predicate, negated);

	/*
	 * The edge that leaves the loop is recorded before the back edge ends
	 * the pass.
	 */
	if (word[2] == header) {
		error = drv_gpu_spirv_edge_add(parser,
					       word[3],
					       other,
					       opcode,
					       offset);
		if (error != 0)
			return error;
		error =
		    drv_gpu_spirv_loop_back_edge(parser, taken, opcode, offset);
	} else if (word[3] == header) {
		error = drv_gpu_spirv_edge_add(parser,
					       word[2],
					       taken,
					       opcode,
					       offset);
		if (error != 0)
			return error;
		error =
		    drv_gpu_spirv_loop_back_edge(parser, other, opcode, offset);
	} else {
		error = drv_gpu_spirv_edge_add(parser,
					       word[2],
					       taken,
					       opcode,
					       offset);
		if (error != 0)
			return error;
		error = drv_gpu_spirv_edge_add(parser,
					       word[3],
					       other,
					       opcode,
					       offset);
	}

	/* Reports why an edge could not be recorded. */
	if (error != 0)
		return error;
	parser->terminated = 1;

	/* Succeeded: both edges are recorded. */
	return 0;
}

/*
 * Lowers OpSwitch: the edge of each case for the block's channels where the
 * selector equals one of the case's literals, and the default's edge for
 * the others.  Several literals of one target make one edge, the or of
 * their conditions, so a phi in the target finds one edge from the block.
 * After an OpSelectionMerge the block is a header, and its construct opens
 * here.  Only a 32-bit integer selector is taken, and no target may be the
 * innermost loop's header.
 */
static int
drv_gpu_spirv_lower_switch(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	uint32_t source_shape;
	int error;
	struct drv_gpu_spirv_construct *construct;
	struct drv_gpu_spirv_id *selector_record;
	uint32_t selector[4];
	uint32_t selector_count;
	uint32_t matched;
	uint32_t equal;
	uint32_t literal;
	uint32_t target;
	uint32_t condition;
	uint32_t others;
	uint32_t predicate;
	uint32_t header;
	uint32_t index;
	uint32_t later;
	int seen;

	/*
	 * The instruction names the selector, the default and pairs of a
	 * literal and a target.
	 */
	if (count < 3U || ((count - 3U) % 2U) != 0U)
		return EINVAL;

	/* The selector is one 32-bit integer, so each literal is one word. */
	selector_record = drv_gpu_spirv_id(parser, word[1]);
	if (selector_record == NULL)
		return EINVAL;
	/* Resolves the declared operand shape before comparing it. */
	source_shape = drv_gpu_spirv_kind_components(parser,
						     selector_record->type,
						     SCALAR_INT);

	/* Requires the shape accepted by this lowering operation. */
	if (source_shape != 1U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "OpSwitch selector that is not a 32-bit integer scalar");
		return error;
	}

	/* Refuses the next incompatible part of this instruction. */
	/* Resolves the declared operand shape before comparing it. */
	source_shape = drv_gpu_spirv_int_width(parser, selector_record->type);

	/* Requires the shape accepted by this lowering operation. */
	if (source_shape != 32U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "OpSwitch selector that is not a 32-bit integer scalar");
		return error;
	}

	/* Resolves the switch selector after validating its integer scalar type. */
	selector_count = drv_gpu_spirv_operand(parser, word[1], selector);
	if (selector_count != 1U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "OpSwitch selector that is not a 32-bit integer scalar");
		return error;
	}

	/*
	 * No target may be the innermost loop's header: a back edge from a
	 * switch is not lowered.
	 */
	header = NO_VALUE;
	if (parser->loop_depth != 0U)
		header = parser->loops[parser->loop_depth - 1U].header;
	for (index = 2U; index < count; index += 2U) {
		if (word[index] == header) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "OpSwitch that branches back to its loop's header");
			return error;
		}
	}

	/* A header opens its construct before its edges are recorded. */
	if (parser->pending_merge != NO_VALUE) {
		if (parser->depth >= MAX_CONSTRUCT_DEPTH) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "selection constructs nested too deep");
			return error;
		}

		/* Publishes the switch construct before recording its target edges. */
		construct = &parser->constructs[parser->depth];
		construct->merge = parser->pending_merge;
		construct->predicate = parser->predicate;
		construct->leaky = 0;
		construct->loop = 0;
		parser->depth++;
		parser->pending_merge = NO_VALUE;
	}

	/*
	 * Records the edge of each case target once, for every literal that
	 * names it.
	 */
	matched = NO_VALUE;
	for (index = 3U; index < count; index += 2U) {
		target = word[index + 1U];

		/* A target an earlier pair named has its edge already. */
		seen = 0;
		for (later = 3U; later < index; later += 2U) {
			if (word[later + 1U] == target)
				seen = 1;
		}

		/* Skips a target whose edge was already represented by an earlier case. */
		if (seen)
			continue;

		/* The channels whose selector is any literal of this target. */
		condition = NO_VALUE;
		for (later = index; later < count; later += 2U) {
			if (word[later + 1U] != target)
				continue;
			literal =
			    drv_gpu_spirv_integer_constant(parser, word[later]);
			equal = drv_gpu_spirv_emit_value(parser,
							 DRV_GPU_IR_IEQ,
							 selector[0],
							 literal);
			if (condition == NO_VALUE) {
				condition = equal;
			} else {
				condition =
				    drv_gpu_spirv_emit_value(parser,
							     DRV_GPU_IR_OR,
							     condition,
							     equal);
			}
		}

		/*
		 * The target's channels so far, for the default's complement.
		 */
		if (matched == NO_VALUE) {
			matched = condition;
		} else {
			matched = drv_gpu_spirv_emit_value(parser,
							   DRV_GPU_IR_OR,
							   matched,
							   condition);
		}

		/*
		 * The default taking this target as well adds its channels to
		 * the edge below.
		 */
		if (target == word[2])
			continue;

		/* Records the case's edge. */
		predicate = drv_gpu_spirv_predicate_and(parser,
							parser->predicate,
							condition);
		error = drv_gpu_spirv_edge_add(parser,
					       target,
					       predicate,
					       opcode,
					       offset);
		if (error != 0)
			return error;
	}

	/*
	 * The default's channels: those no literal matched, and those of a case
	 * that shares its target.
	 */
	if (matched == NO_VALUE) {
		predicate = parser->predicate;
	} else {
		others = drv_gpu_spirv_emit_value(parser,
						  DRV_GPU_IR_NOT,
						  matched,
						  0U);
		for (index = 3U; index < count; index += 2U) {
			if (word[index + 1U] != word[2])
				continue;
			literal =
			    drv_gpu_spirv_integer_constant(parser, word[index]);
			equal = drv_gpu_spirv_emit_value(parser,
							 DRV_GPU_IR_IEQ,
							 selector[0],
							 literal);
			others = drv_gpu_spirv_emit_value(parser,
							  DRV_GPU_IR_OR,
							  others,
							  equal);
		}

		/* Combines the block predicate with the unmatched switch-case predicate. */
		predicate = drv_gpu_spirv_predicate_and(parser,
							parser->predicate,
							others);
	}

	/* Records the default's edge and ends the block. */
	error =
	    drv_gpu_spirv_edge_add(parser, word[2], predicate, opcode, offset);
	if (error != 0)
		return error;
	parser->terminated = 1;

	/* Succeeded: every edge of the switch is recorded. */
	return 0;
}

/*
 * Lowers OpKill: the block's channels are discarded.
 *
 * The discard does not end them (see the IR KILL channel semantics), so the
 * constructs around do not leak.
 */
static int
drv_gpu_spirv_lower_kill(
	struct drv_gpu_spirv_parser *parser,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	uint32_t condition;

	/* Only a fragment shader discards. */
	if (parser->ir->stage != DRV_GPU_STAGE_FRAGMENT) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error =
		    drv_gpu_spirv_refuse(parser,
					 opcode,
					 offset,
					 "OpKill outside a fragment shader");
		return error;
	}

	/* Discards where the block runs: everywhere when it always does. */
	condition = parser->predicate;
	if (condition == PREDICATE_ALWAYS) {
		condition = drv_gpu_spirv_shared_constant(parser,
							  &parser->true_value,
							  DRV_GPU_IR_BOOL,
							  BOOL_TRUE_BITS);
	}

	/* Emits a discard only for the channels selected by this block. */
	(void)drv_gpu_spirv_emit(parser, DRV_GPU_IR_KILL, 0U, condition, 0U);

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
drv_gpu_spirv_lower_return(
	struct drv_gpu_spirv_parser *parser,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	uint32_t index;

	/*
	 * A channel returning from a loop would still reach the loop's merge
	 * block.
	 */
	if (parser->loop_depth != 0U) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "OpReturn inside a loop");
		return error;
	}

	/* Every open construct loses the returning channels. */
	for (index = 0U; index < parser->depth; index++)
		parser->constructs[index].leaky = 1;

	/* From here on a workgroup barrier is refused (ws101-p006). */
	parser->returned = 1;

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
drv_gpu_spirv_edge_add(
	struct drv_gpu_spirv_parser *parser,
	uint32_t target,
	uint32_t predicate,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *label;
	struct drv_gpu_spirv_loop *loop;
	uint32_t level;
	uint32_t inner;

	/* The edge list has room for two edges per body instruction. */
	if (parser->edge_count >= parser->edge_capacity)
		return EINVAL;

	/* The target must be an id of the module. */
	label = drv_gpu_spirv_id(parser, target);
	if (label == NULL)
		return EINVAL;

	/*
	 * A branch to a block already lowered that is not the loop's header is
	 * not structured.
	 */
	if (label->kind == ID_LABEL) {
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "branch back to an earlier block "
					     "that is not the loop header");
		return error;
	}

	/* Finds the innermost construct the target is the merge of, if any. */
	for (level = parser->depth; level > 0U; level--) {
		if (parser->constructs[level - 1U].merge != target)
			continue;

		/*
		 * The constructs inside that one are left before their merge
		 * blocks.
		 */
		for (inner = level; inner < parser->depth; inner++)
			parser->constructs[inner].leaky = 1;
		break;
	}

	/*
	 * A continue leaves the constructs inside its loop before their merge
	 * blocks.
	 */
	if (parser->loop_depth != 0U) {
		loop = &parser->loops[parser->loop_depth - 1U];
		if (loop->continue_target == target) {
			for (inner = loop->construct + 1U;
			     inner < parser->depth;
			     inner++)
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

/*
 * Returns the predicate of the edge from one block to another; `found` says
 * whether there is one.
 */
static uint32_t
drv_gpu_spirv_edge_predicate(
	struct drv_gpu_spirv_parser *parser,
	uint32_t from,
	uint32_t to,
	int *found)
{
	uint32_t index;

	/* Looks the edge up. */
	for (index = 0U; index < parser->edge_count; index++) {
		if (parser->edges[index].from == from &&
		    parser->edges[index].to == to) {
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
drv_gpu_spirv_predicate_and(
	struct drv_gpu_spirv_parser *parser,
	uint32_t predicate,
	uint32_t condition)
{
	uint32_t both;

	/* Under no predicate the condition alone decides. */
	if (predicate == PREDICATE_ALWAYS)
		return condition;

	/* Both must hold. */
	both = drv_gpu_spirv_emit_value(parser,
					DRV_GPU_IR_AND,
					predicate,
					condition);

	/* Succeeded: the channels where both hold. */
	return both;
}

/*
 * Returns a value that is `value` where the block's predicate holds and
 * `previous` elsewhere.
 */
static uint32_t
drv_gpu_spirv_predicated_value(
	struct drv_gpu_spirv_parser *parser,
	uint32_t value,
	uint32_t previous)
{
	uint32_t merged;

	/* A block every channel runs replaces the value outright. */
	if (parser->predicate == PREDICATE_ALWAYS)
		return value;

	/* Selects channel by channel. */
	merged = drv_gpu_spirv_select_value(parser,
					    parser->predicate,
					    value,
					    previous);

	/* Succeeded: the merged value. */
	return merged;
}

/*
 * Returns the IR value of a constant the lowering itself introduces (0.0,
 * 1.0 or true), emitting it the first time; `slot` remembers it.  A constant
 * first emitted inside a loop is emitted again after the loop (see
 * drv_gpu_spirv_loop_close()), so every later reader comes after a definition.
 */
static uint32_t
drv_gpu_spirv_shared_constant(
	struct drv_gpu_spirv_parser *parser,
	uint32_t *slot,
	enum drv_gpu_shader_ir_op op,
	uint32_t bits)
{
	struct drv_gpu_shader_ir_inst *inst;

	/* A constant already emitted is shared. */
	if (*slot != NO_VALUE)
		return *slot;

	/* Emits the constant. */
	*slot = drv_gpu_spirv_new_value(parser);
	inst = drv_gpu_spirv_emit(parser, op, *slot, 0U, 0U);
	if (inst != NULL)
		inst->immediate = bits;

	/* Succeeded: the constant's value. */
	return *slot;
}

/*
 * Guards a texture instruction with the predicate of the block it is in: the
 * channels outside the predicate meet its result only in a selection by the
 * predicate (a store, a phi, a later block's edge), so the message is needed
 * for the predicate's channels alone.  A block every channel runs needs no
 * guard.
 */
static void
drv_gpu_spirv_guard(
	struct drv_gpu_spirv_parser *parser,
	struct drv_gpu_shader_ir_inst *inst)
{
	/* Every channel runs the block: the message is needed for all. */
	if (parser->predicate == PREDICATE_ALWAYS)
		return;

	/* The block's predicate, plus one (zero means no guard). */
	inst->guard = parser->predicate + 1U;

	/* Succeeded: the instruction carries its active-channel predicate. */
	return;
}

/*
 * Opens a skippable region at the start of a block: a SKIP_BEGIN on the
 * block's predicate.  Whether the region is really skipped is decided by the
 * backend, which checks the values that leave the skippable region.
 */
static void
drv_gpu_spirv_skip_open(
	struct drv_gpu_spirv_parser *parser)
{
	/* The region starts with the next value. */
	parser->skip_first_value = parser->ir->value_count;
	parser->skip_open = 1;

	/* Marks its start with the block's predicate. */
	(void)drv_gpu_spirv_emit(parser,
				 DRV_GPU_IR_SKIP_BEGIN,
				 0U,
				 parser->predicate,
				 0U);

	/* Succeeded: the skippable region starts under its block predicate. */
	return;
}

/*
 * Closes the open skippable region, if any, with a SKIP_END.  A constant
 * first materialized inside it is materialized again when read after it, as
 * after a loop: when the region is skipped its value is never made.
 */
static void
drv_gpu_spirv_skip_close(
	struct drv_gpu_spirv_parser *parser)
{
	struct drv_gpu_spirv_id *record;
	uint32_t first;
	uint32_t end;
	uint32_t id;

	/* No region is open. */
	if (!parser->skip_open)
		return;

	/* Marks its end. */
	(void)drv_gpu_spirv_emit(parser, DRV_GPU_IR_SKIP_END, 0U, 0U, 0U);
	parser->skip_open = 0;

	/*
	 * A constant first used in the region is used again as a fresh
	 * constant.
	 */
	first = parser->skip_first_value;
	end = parser->ir->value_count;
	for (id = 0U; id < parser->bound; id++) {
		record = &parser->ids[id];
		if (record->kind != ID_CONSTANT || record->count == 0U)
			continue;
		if (record->comp[0] >= first && record->comp[0] < end)
			record->count = 0U;
	}

	/* So is a constant the lowering introduced in it. */
	if (parser->zero_value != NO_VALUE &&
	    parser->zero_value >= first &&
	    parser->zero_value < end)
		parser->zero_value = NO_VALUE;
	if (parser->one_value != NO_VALUE &&
	    parser->one_value >= first &&
	    parser->one_value < end)
		parser->one_value = NO_VALUE;
	if (parser->true_value != NO_VALUE &&
	    parser->true_value >= first &&
	    parser->true_value < end)
		parser->true_value = NO_VALUE;
	if (parser->int_16_value != NO_VALUE &&
	    parser->int_16_value >= first &&
	    parser->int_16_value < end)
		parser->int_16_value = NO_VALUE;
	if (parser->int_ffff_value != NO_VALUE &&
	    parser->int_ffff_value >= first &&
	    parser->int_ffff_value < end)
		parser->int_ffff_value = NO_VALUE;

	/* Succeeded: the skippable region is terminated. */
	return;
}

/* Emits a float constant of the given bits and returns its value. */
static uint32_t
drv_gpu_spirv_float_constant(
	struct drv_gpu_spirv_parser *parser,
	uint32_t bits)
{
	struct drv_gpu_shader_ir_inst *inst;
	uint32_t value;

	/* Emits the constant into a fresh value. */
	value = drv_gpu_spirv_new_value(parser);
	inst = drv_gpu_spirv_emit(parser, DRV_GPU_IR_CONST, value, 0U, 0U);
	if (inst != NULL)
		inst->immediate = bits;

	/* Succeeded: the constant's value. */
	return value;
}

/* Emits a 32-bit integer constant and returns its value. */
static uint32_t
drv_gpu_spirv_integer_constant(
	struct drv_gpu_spirv_parser *parser,
	uint32_t bits)
{
	struct drv_gpu_shader_ir_inst *inst;
	uint32_t value;

	/* Emits the constant into a fresh value. */
	value = drv_gpu_spirv_new_value(parser);
	inst = drv_gpu_spirv_emit(parser, DRV_GPU_IR_ICONST, value, 0U, 0U);
	if (inst != NULL)
		inst->immediate = bits;

	/* Succeeded: the constant's value. */
	return value;
}

/*
 * Emits a selection of `taken` where the Boolean `condition` holds and `other`
 * elsewhere; returns it.
 */
static uint32_t
drv_gpu_spirv_select_value(
	struct drv_gpu_spirv_parser *parser,
	uint32_t condition,
	uint32_t taken,
	uint32_t other)
{
	struct drv_gpu_shader_ir_inst *inst;
	uint32_t merged;

	/*
	 * The selection reads three sources; the third is set after the emit.
	 */
	merged = drv_gpu_spirv_new_value(parser);
	inst = drv_gpu_spirv_emit(parser,
				  DRV_GPU_IR_SELECT,
				  merged,
				  condition,
				  taken);
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
drv_gpu_spirv_move_value(
	struct drv_gpu_spirv_parser *parser,
	uint32_t destination,
	uint32_t source)
{
	/* A move into a new loop variable numbers it first. */
	if (destination == NO_VALUE)
		destination = drv_gpu_spirv_new_value(parser);

	/* Moves the bits. */
	(void)drv_gpu_spirv_emit(parser,
				 DRV_GPU_IR_MOVE,
				 destination,
				 source,
				 0U);

	/* Succeeded: the value moved into. */
	return destination;
}

/* Records why the module is refused; the instruction is NOT skipped. */
static int
drv_gpu_spirv_refuse(
	struct drv_gpu_spirv_parser *parser,
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
static struct drv_gpu_spirv_id *
drv_gpu_spirv_id(
	struct drv_gpu_spirv_parser *parser,
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
drv_gpu_spirv_kind_components(
	struct drv_gpu_spirv_parser *parser,
	uint32_t type_id,
	uint32_t scalar)
{
	struct drv_gpu_spirv_id *type;
	uint32_t count;

	/* Resolves the type; a vector counts its elements, a scalar one. */
	type = drv_gpu_spirv_id(parser, type_id);
	if (type == NULL)
		return 0U;
	count = 1U;
	if (type->kind == ID_TYPE_VECTOR) {
		if (type->count < 2U || type->count > 4U)
			return 0U;
		count = type->count;
		type = drv_gpu_spirv_id(parser, type->type);
		if (type == NULL)
			return 0U;
	}

	/*
	 * The scalar must be of the kind asked for: a float of 32 bits, an
	 * integer of 32 or 16 bits (ws031-p039: a 16-bit one is carried in a
	 * 32-bit value, its high half undefined; drv_gpu_spirv_extend16() makes
	 * it whole where that matters, and a 16-bit integer never reaches
	 * memory other than a local or a shared one,
	 * drv_gpu_spirv_declare_variable()).
	 */
	if (scalar == SCALAR_FLOAT &&
	    type->kind == ID_TYPE_FLOAT &&
	    type->width == 32U)
		return count;
	if (scalar == SCALAR_INT &&
	    type->kind == ID_TYPE_INT &&
	    (type->width == 32U ||
	    type->width == 16U))
		return count;
	if (scalar == SCALAR_BOOL && type->kind == ID_TYPE_BOOL)
		return count;

	/* Succeeded: another kind of type counts nothing. */
	return 0U;
}

/*
 * Returns the component count of a float scalar or float vector type; 0 for
 * anything else.
 */
static uint32_t
drv_gpu_spirv_float_components(
	struct drv_gpu_spirv_parser *parser,
	uint32_t type_id)
{
	uint32_t components;

	/* Counts the floats. */
	components =
	    drv_gpu_spirv_kind_components(parser, type_id, SCALAR_FLOAT);

	/* Succeeded: the float components, or none. */
	return components;
}

/*
 * Returns the component count of an integer scalar or integer vector type; 0
 * for anything else.
 */
static uint32_t
drv_gpu_spirv_int_components(
	struct drv_gpu_spirv_parser *parser,
	uint32_t type_id)
{
	uint32_t components;

	/* Counts the integers. */
	components = drv_gpu_spirv_kind_components(parser, type_id, SCALAR_INT);

	/* Succeeded: the integer components, or none. */
	return components;
}

/*
 * Returns the component count of a Boolean scalar or Boolean vector type; 0 for
 * anything else.
 */
static uint32_t
drv_gpu_spirv_bool_components(
	struct drv_gpu_spirv_parser *parser,
	uint32_t type_id)
{
	uint32_t components;

	/* Counts the Booleans. */
	components =
	    drv_gpu_spirv_kind_components(parser, type_id, SCALAR_BOOL);

	/* Succeeded: the Boolean components, or none. */
	return components;
}

/*
 * Returns the component count of a float, integer or Boolean scalar or vector
 * type; 0 for anything else.
 */
static uint32_t
drv_gpu_spirv_value_components(
	struct drv_gpu_spirv_parser *parser,
	uint32_t type_id)
{
	uint32_t components;

	/* A float type counts its floats. */
	components = drv_gpu_spirv_float_components(parser, type_id);
	if (components != 0U)
		return components;

	/* An integer type counts its integers. */
	components = drv_gpu_spirv_int_components(parser, type_id);
	if (components != 0U)
		return components;

	/*
	 * Succeeded: a Boolean type counts its Booleans, anything else nothing.
	 */
	components = drv_gpu_spirv_bool_components(parser, type_id);
	return components;
}

/*
 * Tells whether a type holds a 16-bit integer anywhere: a scalar, a
 * vector's, a matrix's, an array's, a pointer's pointee or a structure's
 * member (ws031-p039).  A type nested past MAX_TYPE_DEPTH is taken to.
 */
static int
drv_gpu_spirv_type_has_int16(
	struct drv_gpu_spirv_parser *parser,
	uint32_t type_id,
	uint32_t depth)
{
	int contains_int16;
	struct drv_gpu_spirv_id *type;
	uint32_t index;
	int found;

	/* Bounded, and a type of the module. */
	if (depth > MAX_TYPE_DEPTH)
		return 1;
	type = drv_gpu_spirv_id(parser, type_id);
	if (type == NULL)
		return 0;

	/* The kinds that hold others, and the integer itself. */
	switch (type->kind) {
	case ID_TYPE_INT:
		/*
		 * A scalar integer contains a narrow component only at this
		 * width.
		 */
		if (type->width == 16U)
			return 1;

		/* Succeeded: this integer has no 16-bit component. */
		return 0;
	case ID_TYPE_VECTOR:
	case ID_TYPE_MATRIX:
	case ID_TYPE_ARRAY:
	case ID_TYPE_POINTER:
		/*
		 * Classifies the nested type using the same bounded type walk.
		 */
		contains_int16 = drv_gpu_spirv_type_has_int16(parser,
							      type->type,
							      depth + 1U);

		/*
		 * Succeeded: reports whether the nested type contains a 16-bit
		 * integer.
		 */
		return contains_int16;
	case ID_TYPE_STRUCT:
		for (index = 0U; index < type->count && index < MAX_MEMBERS;
		     index++) {
			found = drv_gpu_spirv_type_has_int16(
			    parser,
			    type->member_type[index],
			    depth + 1U);
			if (found != 0)
				return 1;
		}

		/* Succeeded: this structure contains no narrow integer component. */
		return 0;
	default:
		/* Succeeded: no integer in it. */
		return 0;
	}
}

/*
 * Gives the width of an integer scalar or vector type (16 or 32); 0 for any
 * other type.
 */
static uint32_t
drv_gpu_spirv_int_width(
	struct drv_gpu_spirv_parser *parser,
	uint32_t type_id)
{
	struct drv_gpu_spirv_id *type;

	/* A vector's scalar. */
	type = drv_gpu_spirv_id(parser, type_id);
	if (type != NULL && type->kind == ID_TYPE_VECTOR)
		type = drv_gpu_spirv_id(parser, type->type);

	/* Succeeded: the integer's width. */
	if (type == NULL || type->kind != ID_TYPE_INT)
		return 0U;
	return type->width;
}

/*
 * Gives the integer width of an operand id's type (16 or 32); 0 for another
 * kind or an unknown id.
 */
static uint32_t
drv_gpu_spirv_operand_int_width(
	struct drv_gpu_spirv_parser *parser,
	uint32_t id)
{
	uint32_t emitted_scalar;
	struct drv_gpu_spirv_id *record;

	/* The id's type. */
	record = drv_gpu_spirv_id(parser, id);
	if (record == NULL)
		return 0U;

	/* Succeeded: its width. */
	emitted_scalar = drv_gpu_spirv_int_width(parser, record->type);

	/*
	 * Preserves the scalar number while the caller handles a latched
	 * refusal.
	 */
	if (parser->error != 0)
		return emitted_scalar;

	/* Succeeded: reports the generated scalar number. */
	return emitted_scalar;
}

/*
 * Makes a 16-bit integer's value whole (ws031-p039): its low half sign
 * extended (SHL 16, ASR 16) or zero extended (IAND 0xFFFF), where its high
 * half would change a result.  A 32-bit value is returned as it is.
 */
static uint32_t
drv_gpu_spirv_extend16(
	struct drv_gpu_spirv_parser *parser,
	uint32_t value,
	uint32_t width,
	int is_signed)
{
	uint32_t emitted_scalar;
	uint32_t sixteen;
	uint32_t mask;
	uint32_t shifted;

	/* Only a 16-bit value. */
	if (width != 16U)
		return value;

	/* Zero extended: the low half alone. */
	if (!is_signed) {
		mask = drv_gpu_spirv_shared_constant(parser,
						     &parser->int_ffff_value,
						     DRV_GPU_IR_ICONST,
						     0xFFFFU);

		/* Emits the scalar selected by this lowering operation. */
		emitted_scalar = drv_gpu_spirv_emit_value(parser,
							  DRV_GPU_IR_IAND,
							  value,
							  mask);

		/*
		 * Preserves the scalar number while the caller handles a
		 * latched refusal.
		 */
		if (parser->error != 0)
			return emitted_scalar;

		/* Succeeded: reports the generated scalar number. */
		return emitted_scalar;
	}

	/* Succeeded: sign extended, the low half up and back with its sign. */
	sixteen = drv_gpu_spirv_shared_constant(parser,
						&parser->int_16_value,
						DRV_GPU_IR_ICONST,
						16U);
	shifted =
	    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_SHL, value, sixteen);

	/* Emits the scalar selected by this lowering operation. */
	emitted_scalar =
	    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_ASR, shifted, sixteen);

	/*
	 * Preserves the scalar number while the caller handles a latched
	 * refusal.
	 */
	if (parser->error != 0)
		return emitted_scalar;

	/* Succeeded: reports the generated scalar number. */
	return emitted_scalar;
}

/*
 * Returns the scalar count of a float matrix type, columns times rows; 0 for
 * anything else.
 */
static uint32_t
drv_gpu_spirv_matrix_components(
	struct drv_gpu_spirv_parser *parser,
	uint32_t type_id)
{
	struct drv_gpu_spirv_id *type;
	uint32_t rows;

	/* Resolves the type, which must be a matrix. */
	type = drv_gpu_spirv_id(parser, type_id);
	if (type == NULL || type->kind != ID_TYPE_MATRIX)
		return 0U;

	/* Its columns must be float vectors. */
	rows = drv_gpu_spirv_float_components(parser, type->type);
	if (rows < 2U)
		return 0U;

	/* Succeeded: every scalar of every column. */
	return type->count * rows;
}

/*
 * Returns the scalar count of a float scalar, vector or matrix type; 0 for
 * anything else.
 */
static uint32_t
drv_gpu_spirv_float_components_wide(
	struct drv_gpu_spirv_parser *parser,
	uint32_t type_id)
{
	uint32_t components;

	/* A matrix counts its scalars. */
	components = drv_gpu_spirv_matrix_components(parser, type_id);
	if (components != 0U)
		return components;

	/* Succeeded: a float scalar or vector counts its components. */
	components = drv_gpu_spirv_float_components(parser, type_id);
	return components;
}

/*
 * Measures a type made of 32-bit scalars: how many scalars it holds, its
 * aggregates flattened member after member and element after element, and
 * how many interface locations it takes (a scalar or a vector one, a matrix
 * one per column).  Returns ENOTSUP for any other type, one nested deeper
 * than MAX_TYPE_DEPTH or one larger than the parser follows.
 */
static int
drv_gpu_spirv_type_size(
	struct drv_gpu_spirv_parser *parser,
	uint32_t type_id,
	uint32_t depth,
	uint32_t *scalars,
	uint32_t *locations)
{
	int error;
	struct drv_gpu_spirv_id *type;
	uint32_t member_scalars;
	uint32_t member_locations;
	uint32_t components;
	uint32_t member;

	/* A type nested too deep, or not declared, is not measured. */
	if (depth > MAX_TYPE_DEPTH)
		return ENOTSUP;
	type = drv_gpu_spirv_id(parser, type_id);
	if (type == NULL)
		return ENOTSUP;

	/* A scalar or a vector is one location of its components. */
	components = drv_gpu_spirv_value_components(parser, type_id);
	if (components != 0U) {
		*scalars = components;
		*locations = 1U;
		return 0;
	}

	/* A matrix is a location to a column. */
	components = drv_gpu_spirv_matrix_components(parser, type_id);
	if (components != 0U) {
		*scalars = components;
		*locations = type->count;
		return 0;
	}

	/* An array is its elements, one after another. */
	if (type->kind == ID_TYPE_ARRAY) {
		if (type->length == 0U || type->length > MAX_VARIABLE_SLOTS)
			return ENOTSUP;
		error = drv_gpu_spirv_type_size(parser,
						type->type,
						depth + 1U,
						&member_scalars,
						&member_locations);
		if (error != 0)
			return error;
		*scalars = type->length * member_scalars;
		*locations = type->length * member_locations;
		if (*scalars > MAX_VARIABLE_SLOTS)
			return ENOTSUP;
		return 0;
	}

	/* Anything else but a structure is not made of scalars. */
	if (type->kind != ID_TYPE_STRUCT)
		return ENOTSUP;

	/* A structure is its members in order. */
	*scalars = 0U;
	*locations = 0U;
	for (member = 0U; member < type->count; member++) {
		error = drv_gpu_spirv_type_size(parser,
						type->member_type[member],
						depth + 1U,
						&member_scalars,
						&member_locations);
		if (error != 0)
			return error;
		*scalars += member_scalars;
		*locations += member_locations;
	}

	/* A structure larger than a variable holds is not followed. */
	if (*scalars > MAX_VARIABLE_SLOTS)
		return ENOTSUP;

	/* Succeeded: the structure's scalars and locations. */
	return 0;
}

/*
 * Returns how many scalars a scalar, vector, matrix, array or structure
 * type holds (see drv_gpu_spirv_type_size()); 0 for any other type.
 */
static uint32_t
drv_gpu_spirv_aggregate_scalars(
	struct drv_gpu_spirv_parser *parser,
	uint32_t type_id)
{
	int error;
	uint32_t scalars;
	uint32_t locations;

	/* Measures the type. */
	error =
	    drv_gpu_spirv_type_size(parser, type_id, 0U, &scalars, &locations);
	if (error != 0)
		return 0U;

	/* Succeeded: the type's scalars. */
	return scalars;
}

/*
 * Selects one part of an aggregate type by a constant index: a structure's
 * member, an array's element, a matrix's column or a vector's component.
 * Reports the part's type, the first of its scalars among the aggregate's
 * (see drv_gpu_spirv_type_size()) and the first of its interface slots, four
 * to a location (see drv_gpu_spirv_io_map()).  Returns EINVAL for an index
 * past the aggregate, ENOTSUP (with the refusal named) for a scalar or a
 * type that is not made of scalars.
 */
static int
drv_gpu_spirv_type_step(
	struct drv_gpu_spirv_parser *parser,
	uint32_t type_id,
	uint32_t index,
	uint32_t *next_type,
	uint32_t *scalar_offset,
	uint32_t *io_offset,
	uint32_t opcode,
	uint32_t offset)
{
	int error;
	struct drv_gpu_spirv_id *type;
	struct drv_gpu_spirv_id *column;
	uint32_t scalars;
	uint32_t locations;
	uint32_t member;

	/* Resolves the aggregate. */
	type = drv_gpu_spirv_id(parser, type_id);
	if (type == NULL)
		return EINVAL;

	/* Steps by what the aggregate is. */
	switch (type->kind) {
	case ID_TYPE_STRUCT:
		/* A member: after every member before it. */
		if (index >= type->count)
			return EINVAL;
		*scalar_offset = 0U;
		*io_offset = 0U;
		for (member = 0U; member < index; member++) {
			error =
			    drv_gpu_spirv_type_size(parser,
						    type->member_type[member],
						    1U,
						    &scalars,
						    &locations);
			if (error != 0) {
				/*
				 * Records the unsupported instruction in the
				 * caller's diagnostic.
				 */
				error = drv_gpu_spirv_refuse(
				    parser,
				    opcode,
				    offset,
				    "structure with a member that is not made "
				    "of scalars");
				return error;
			}

			/* Places the selected member after the preceding members' scalar storage. */
			*scalar_offset += scalars;
			*io_offset += 4U * locations;
		}

		/* The member's type. */
		*next_type = type->member_type[index];
		break;

	case ID_TYPE_ARRAY:
		/* An element: after every element before it. */
		if (type->length == 0U || index >= type->length)
			return EINVAL;
		error = drv_gpu_spirv_type_size(parser,
						type->type,
						1U,
						&scalars,
						&locations);
		if (error != 0) {
			/*
			 * Records the unsupported instruction in the caller's
			 * diagnostic.
			 */
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "array of elements that are not made of scalars");
			return error;
		}

		/* Locates the selected array element in its contiguous scalar storage. */
		*scalar_offset = index * scalars;
		*io_offset = 4U * index * locations;
		*next_type = type->type;
		break;

	case ID_TYPE_MATRIX:
		/*
		 * A column: after every column before it, each a location of
		 * its own.
		 */
		column = drv_gpu_spirv_id(parser, type->type);
		if (column == NULL || index >= type->count)
			return EINVAL;
		*scalar_offset = index * column->count;
		*io_offset = 4U * index;
		*next_type = type->type;
		break;

	case ID_TYPE_VECTOR:
		/* A component of the vector's one location. */
		if (index >= type->count)
			return EINVAL;
		*scalar_offset = index;
		*io_offset = index;
		*next_type = type->type;
		break;

	default:
		/*
		 * Records the unsupported instruction in the caller's
		 * diagnostic.
		 */
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "index into a scalar");
		return error;
	}

	/* Succeeded: the part is found. */
	return 0;
}

/*
 * Lists the interface slot of each scalar of a type, four slots to a
 * location, from slot `base` on, in the order drv_gpu_spirv_type_size()
 * flattens the scalars: a vector's components in its one location, a
 * matrix's columns a location each, an array's elements and a structure's
 * members each from the location after the one before.  Returns ENOTSUP for
 * a type that is not made of scalars or has more than `capacity`.
 */
static int
drv_gpu_spirv_io_map(
	struct drv_gpu_spirv_parser *parser,
	uint32_t type_id,
	uint32_t depth,
	uint32_t base,
	uint32_t *map,
	uint32_t *count,
	uint32_t capacity)
{
	int error;
	struct drv_gpu_spirv_id *type;
	uint32_t scalars;
	uint32_t locations;
	uint32_t components;
	uint32_t rows;
	uint32_t index;
	uint32_t row;

	/* A type nested too deep, or not declared, has no slots. */
	if (depth > MAX_TYPE_DEPTH)
		return ENOTSUP;
	type = drv_gpu_spirv_id(parser, type_id);
	if (type == NULL)
		return ENOTSUP;

	/* A scalar or a vector takes the components of its location. */
	components = drv_gpu_spirv_value_components(parser, type_id);
	if (components != 0U) {
		if (*count + components > capacity)
			return ENOTSUP;
		for (index = 0U; index < components; index++) {
			map[*count] = base + index;
			(*count)++;
		}

		/* Succeeded: the vector's slots are listed. */
		return 0;
	}

	/* A matrix takes a location to a column. */
	components = drv_gpu_spirv_matrix_components(parser, type_id);
	if (components != 0U) {
		if (*count + components > capacity)
			return ENOTSUP;
		rows = components / type->count;
		for (index = 0U; index < type->count; index++) {
			for (row = 0U; row < rows; row++) {
				map[*count] = base + 4U * index + row;
				(*count)++;
			}
		}

		/* Succeeded: the matrix's slots are listed. */
		return 0;
	}

	/* An array takes the run of locations of each element in turn. */
	if (type->kind == ID_TYPE_ARRAY) {
		error = drv_gpu_spirv_type_size(parser,
						type->type,
						depth + 1U,
						&scalars,
						&locations);
		if (error != 0 || type->length == 0U)
			return ENOTSUP;
		for (index = 0U; index < type->length; index++) {
			error =
			    drv_gpu_spirv_io_map(parser,
						 type->type,
						 depth + 1U,
						 base + 4U * index * locations,
						 map,
						 count,
						 capacity);
			if (error != 0)
				return error;
		}

		/* Succeeded: the array's slots are listed. */
		return 0;
	}

	/* Anything else but a structure has no slots. */
	if (type->kind != ID_TYPE_STRUCT)
		return ENOTSUP;

	/* A structure takes the run of locations of each member in turn. */
	for (index = 0U; index < type->count; index++) {
		error = drv_gpu_spirv_io_map(parser,
					     type->member_type[index],
					     depth + 1U,
					     base,
					     map,
					     count,
					     capacity);
		if (error != 0)
			return error;
		error = drv_gpu_spirv_type_size(parser,
						type->member_type[index],
						depth + 1U,
						&scalars,
						&locations);
		if (error != 0)
			return error;
		base += 4U * locations;
	}

	/* Succeeded: every scalar has its slot. */
	return 0;
}

/*
 * Gives a local variable or an output `count` slots of the parser's store,
 * each holding nothing yet; the store grows as needed.  Returns ENOMEM, or
 * ENOTSUP for more slots than a variable may have.
 */
static int
drv_gpu_spirv_variable_slots(
	struct drv_gpu_spirv_parser *parser,
	struct drv_gpu_spirv_id *variable,
	uint32_t count)
{
	uint32_t *grown;
	uint32_t capacity;
	uint32_t index;

	/* A variable larger than the parser follows is refused. */
	if (count > MAX_VARIABLE_SLOTS)
		return ENOTSUP;

	/*
	 * A full store grows: to 256 slots at first, doubling after, until the
	 * run fits.
	 */
	if (parser->slot_count + count > parser->slot_capacity) {
		capacity = parser->slot_capacity;
		if (capacity == 0U)
			capacity = MAX_VARIABLE_SLOTS;
		while (capacity < parser->slot_count + count)
			capacity *= 2U;

		/* Allocates the larger store and moves the slots into it. */
		grown = kern_calloc(capacity, sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;
		if (parser->slots != NULL) {
			kern_memcpy(grown,
				    parser->slots,
				    parser->slot_count * sizeof(*grown));
			kern_free(parser->slots);
		}

		/* Publishes the larger store. */
		parser->slots = grown;
		parser->slot_capacity = capacity;
	}

	/* Takes the run; nothing is stored in it yet. */
	variable->first_slot = parser->slot_count;
	variable->slot_count = count;
	for (index = 0U; index < count; index++)
		parser->slots[parser->slot_count + index] = NO_VALUE;
	parser->slot_count += count;

	/* Succeeded: the variable has its slots. */
	return 0;
}

/*
 * Returns where slot `index` of a variable's run is kept, or NULL for an
 * index past the run.  The address is good until the next variable is
 * given slots.
 */
static uint32_t *
drv_gpu_spirv_variable_slot(struct drv_gpu_spirv_parser *parser,
			    const struct drv_gpu_spirv_id *variable,
			    uint32_t index)
{
	/* An index past the run has no slot. */
	if (index >= variable->slot_count)
		return NULL;

	/* Succeeded: the slot in the store. */
	return &parser->slots[variable->first_slot + index];
}

/* Returns the type id of a value or constant operand; 0 for any other id. */
static uint32_t
drv_gpu_spirv_operand_type(
	struct drv_gpu_spirv_parser *parser,
	uint32_t id)
{
	struct drv_gpu_spirv_id *record;

	/* Resolves the operand. */
	record = drv_gpu_spirv_id(parser, id);
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

/*
 * Returns the integer component count of an operand's type; 0 when it is not an
 * integer scalar or vector.
 */
static uint32_t
drv_gpu_spirv_operand_int_components(
	struct drv_gpu_spirv_parser *parser,
	uint32_t id)
{
	uint32_t type;
	uint32_t components;

	/* Counts the integers of the operand's type. */
	type = drv_gpu_spirv_operand_type(parser, id);
	components = drv_gpu_spirv_int_components(parser, type);

	/* Succeeded: the integer components, or none. */
	return components;
}

/*
 * Returns the float component count of an operand's type; 0 when it is not a
 * float scalar or vector.
 */
static uint32_t
drv_gpu_spirv_operand_float_components(
	struct drv_gpu_spirv_parser *parser,
	uint32_t id)
{
	uint32_t type;
	uint32_t components;

	/* Counts the floats of the operand's type. */
	type = drv_gpu_spirv_operand_type(parser, id);
	components = drv_gpu_spirv_float_components(parser, type);

	/* Succeeded: the float components, or none. */
	return components;
}

/*
 * Reports the shape of an operand: the columns and rows of a matrix, one
 * column of a vector's components, one of one for a scalar; zero columns
 * for anything else.
 */
static void
drv_gpu_spirv_operand_shape(
	struct drv_gpu_spirv_parser *parser,
	uint32_t id,
	uint32_t *columns,
	uint32_t *rows)
{
	struct drv_gpu_spirv_id *type;
	uint32_t type_id;
	uint32_t components;

	/* Starts with no shape. */
	*columns = 0U;
	*rows = 0U;

	/* Resolves the operand's type. */
	type_id = drv_gpu_spirv_operand_type(parser, id);
	type = drv_gpu_spirv_id(parser, type_id);
	if (type == NULL)
		return;

	/* A matrix has its columns of its column type's rows. */
	if (type->kind == ID_TYPE_MATRIX) {
		*columns = type->count;
		*rows = drv_gpu_spirv_float_components(parser, type->type);
		return;
	}

	/* A scalar or a vector is one column. */
	components = drv_gpu_spirv_value_components(parser, type_id);
	if (components == 0U)
		return;
	*columns = 1U;
	*rows = components;

	/* Succeeded: the operand dimensions are available to its caller. */
	return;
}

/* Appends one IR instruction defining a fresh value and returns that value. */
static uint32_t
drv_gpu_spirv_emit_value(
	struct drv_gpu_spirv_parser *parser,
	enum drv_gpu_shader_ir_op op,
	uint32_t source0,
	uint32_t source1)
{
	uint32_t value;

	/*
	 * Numbers the value, then defines it; a failure is latched by the emit.
	 */
	value = drv_gpu_spirv_new_value(parser);
	(void)drv_gpu_spirv_emit(parser, op, value, source0, source1);

	/* Succeeded: the value the instruction defines. */
	return value;
}

/*
 * Appends one IR instruction and returns it, or NULL (with the error latched).
 */
static struct drv_gpu_shader_ir_inst *
drv_gpu_spirv_emit(
	struct drv_gpu_spirv_parser *parser,
	enum drv_gpu_shader_ir_op op,
	uint32_t dst,
	uint32_t source0,
	uint32_t source1)
{
	struct drv_gpu_shader_ir_inst *inst;
	struct drv_gpu_shader_ir_inst *grown;
	uint32_t capacity;

	/*
	 * A full stream doubles; failing to grow is never a silently shorter
	 * shader.
	 */
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
		kern_memcpy(grown,
			    parser->ir->instructions,
			    parser->ir->instruction_count * sizeof(*grown));
		kern_free(parser->ir->instructions);
		parser->ir->instructions = grown;
		parser->capacity = capacity;
	}

	/* Takes the next slot. */
	inst = &parser->ir->instructions[parser->ir->instruction_count];
	parser->ir->instruction_count++;

	/*
	 * Fills the instruction; the caller sets the operation-specific fields.
	 */
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
drv_gpu_spirv_new_value(
	struct drv_gpu_spirv_parser *parser)
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
 * drv_gpu_spirv_operand_wide().  Returns 0 for a matrix.
 */
static uint32_t
drv_gpu_spirv_operand(
	struct drv_gpu_spirv_parser *parser,
	uint32_t id,
	uint32_t comp[4])
{
	uint32_t wide[MAX_COMPONENTS];
	uint32_t components;
	uint32_t index;

	/* Resolves the operand's scalars. */
	components = drv_gpu_spirv_operand_wide(parser, id, wide);

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
drv_gpu_spirv_operand_wide(
	struct drv_gpu_spirv_parser *parser,
	uint32_t id,
	uint32_t comp[MAX_COMPONENTS])
{
	struct drv_gpu_spirv_id *record;
	struct drv_gpu_spirv_id *constituent;
	uint32_t constituent_comp[MAX_COMPONENTS];
	uint32_t components;
	uint32_t filled;
	uint32_t index;
	uint32_t component;
	int escaped;

	/* Resolves the operand. */
	record = drv_gpu_spirv_id(parser, id);
	if (record == NULL)
		return 0U;

	/* A scalar constant is one scalar, materialized on its first use. */
	if (record->kind == ID_CONSTANT) {
		components =
		    drv_gpu_spirv_constant_operand(parser, record, comp);
		return components;
	}

	/*
	 * A composite constant is its constituents, each materialized on its
	 * first use.
	 */
	if (record->kind == ID_CONSTANT_COMPOSITE) {
		filled = 0U;
		for (index = 0U; index < record->count; index++) {
			constituent =
			    drv_gpu_spirv_id(parser,
					     record->member_type[index]);
			if (constituent == NULL)
				return 0U;
			components = drv_gpu_spirv_operand_wide(
			    parser,
			    record->member_type[index],
			    constituent_comp);
			if (components == 0U ||
			    filled + components > MAX_COMPONENTS)
				return 0U;
			for (component = 0U; component < components;
			     component++)
				comp[filled + component] =
				    constituent_comp[component];
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
		escaped = drv_gpu_spirv_escaped(parser, record->comp[index]);
		if (escaped != 0)
			return 0U;
		comp[index] = record->comp[index];
	}

	/* Succeeded: the operand has this many components. */
	return record->count;
}

/* Reports whether an IR value was made inside a loop that has ended. */
static int
drv_gpu_spirv_escaped(
	struct drv_gpu_spirv_parser *parser,
	uint32_t value)
{
	uint32_t index;

	/* Looks for a closed loop whose value range holds it. */
	for (index = 0U; index < parser->closed_count; index++) {
		if (value >= parser->closed[index].first &&
		    value < parser->closed[index].end)
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
drv_gpu_spirv_constant_operand(
	struct drv_gpu_spirv_parser *parser,
	struct drv_gpu_spirv_id *record,
	uint32_t comp[MAX_COMPONENTS])
{
	struct drv_gpu_shader_ir_inst *inst;
	enum drv_gpu_shader_ir_op op;
	uint32_t floats;
	uint32_t integers;
	uint32_t booleans;

	/*
	 * A float constant is a CONST, an integer one an ICONST, a Boolean one
	 * a BOOL.
	 */
	floats = drv_gpu_spirv_float_components(parser, record->type);
	integers = drv_gpu_spirv_int_components(parser, record->type);
	booleans = drv_gpu_spirv_bool_components(parser, record->type);
	if (floats == 1U) {
		op = DRV_GPU_IR_CONST;
	} else if (integers == 1U) {
		op = DRV_GPU_IR_ICONST;
	} else if (booleans == 1U) {
		op = DRV_GPU_IR_BOOL;
	} else {
		return 0U;
	}

	/* The first use emits the constant; later uses share its value. */
	if (record->comp[0] == NO_VALUE || record->count == 0U) {
		record->comp[0] = drv_gpu_spirv_new_value(parser);
		record->count = 1U;
		inst = drv_gpu_spirv_emit(parser, op, record->comp[0], 0U, 0U);
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
static struct drv_gpu_spirv_id *
drv_gpu_spirv_result(
	struct drv_gpu_spirv_parser *parser,
	uint32_t id,
	uint32_t type_id,
	uint32_t count,
	int fresh)
{
	struct drv_gpu_spirv_id *record;
	uint32_t index;

	/*
	 * SSA: a result id is defined once, with no more scalars than a value
	 * holds.
	 */
	record = drv_gpu_spirv_id(parser, id);
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
			record->comp[index] = drv_gpu_spirv_new_value(parser);
		} else {
			record->comp[index] = NO_VALUE;
		}
	}

	/* Succeeded: the result is declared. */
	return record;
}

/* The compute part of the parser (ws101-p002). */
#include "spirv-compute.inc"

/* The geometry part of the parser (ws075-p007a). */
#include "spirv-geometry.inc"
