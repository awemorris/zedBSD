/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The SPIR-V reader's numbers and the functions its files share: the
 * module's opcodes and enumerants, the reader's id kinds and flags,
 * and the steps one file of the reader calls in another.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_SHADER_SPIRV_H
#define KERN_DRIVERS_GPU_POWERVR_SHADER_SPIRV_H

#include <stdint.h>

#include "drivers/gpu/powervr/compiler/shader.h"
#include "drivers/gpu/powervr/compiler/shader-ir.h"
#include "drivers/gpu/powervr/compiler/shader-private.h"

/*
 * The module's header and the opcodes, decorations, built-ins, storage
 * classes and execution models the reader knows.
 *
 * Values referenced:
 * The Khronos SPIR-V 1.6 specification, sections 2.3 (physical layout)
 * and 3 (binary form: opcodes, Decoration, BuiltIn, StorageClass,
 * ExecutionModel, ExecutionMode); the GLSL.std.450 extended
 * instruction set's numbers.
 */
#define SPIRV_MAGIC			0x07230203U
#define SPIRV_HEADER_WORDS		5U

#define OP_UNDEF			1U
#define OP_EXT_INST_IMPORT		11U
#define OP_EXT_INST			12U
#define OP_ENTRY_POINT			15U
#define OP_EXECUTION_MODE		16U
#define OP_TYPE_VOID			19U
#define OP_TYPE_BOOL			20U
#define OP_TYPE_INT			21U
#define OP_TYPE_FLOAT			22U
#define OP_TYPE_VECTOR			23U
#define OP_TYPE_MATRIX			24U
#define OP_TYPE_IMAGE			25U
#define OP_TYPE_SAMPLER			26U
#define OP_TYPE_SAMPLED_IMAGE		27U
#define OP_TYPE_ARRAY			28U
#define OP_TYPE_RUNTIME_ARRAY		29U
#define OP_TYPE_STRUCT			30U
#define OP_TYPE_POINTER			32U
#define OP_TYPE_FUNCTION		33U
#define OP_CONSTANT_TRUE		41U
#define OP_CONSTANT_FALSE		42U
#define OP_CONSTANT			43U
#define OP_CONSTANT_COMPOSITE		44U
#define OP_CONSTANT_NULL		46U
#define OP_SPEC_CONSTANT_TRUE		48U
#define OP_SPEC_CONSTANT_FALSE		49U
#define OP_SPEC_CONSTANT		50U
#define OP_SPEC_CONSTANT_COMPOSITE	51U
#define OP_FUNCTION			54U
#define OP_FUNCTION_PARAMETER		55U
#define OP_FUNCTION_END			56U
#define OP_FUNCTION_CALL		57U
#define OP_VARIABLE			59U
#define OP_LOAD				61U
#define OP_STORE			62U
#define OP_ACCESS_CHAIN			65U
#define OP_IN_BOUNDS_ACCESS_CHAIN	66U
#define OP_DECORATE			71U
#define OP_MEMBER_DECORATE		72U
#define OP_VECTOR_SHUFFLE		79U
#define OP_COMPOSITE_CONSTRUCT		80U
#define OP_COMPOSITE_EXTRACT		81U
#define OP_COMPOSITE_INSERT		82U
#define OP_COPY_OBJECT			83U
#define OP_SAMPLED_IMAGE		86U
#define OP_IMAGE_SAMPLE_IMPLICIT_LOD	87U
#define OP_CONVERT_F_TO_U		109U
#define OP_CONVERT_F_TO_S		110U
#define OP_CONVERT_S_TO_F		111U
#define OP_CONVERT_U_TO_F		112U
#define OP_BITCAST			124U
#define OP_S_NEGATE			126U
#define OP_F_NEGATE			127U
#define OP_I_ADD			128U
#define OP_F_ADD			129U
#define OP_I_SUB			130U
#define OP_F_SUB			131U
#define OP_I_MUL			132U
#define OP_F_MUL			133U
#define OP_F_DIV			136U
#define OP_VECTOR_TIMES_SCALAR		142U
#define OP_MATRIX_TIMES_SCALAR		143U
#define OP_MATRIX_TIMES_VECTOR		145U
#define OP_DOT				148U
#define OP_LOGICAL_EQUAL		164U
#define OP_LOGICAL_NOT_EQUAL		165U
#define OP_LOGICAL_OR			166U
#define OP_LOGICAL_AND			167U
#define OP_LOGICAL_NOT			168U
#define OP_SELECT			169U
#define OP_I_EQUAL			170U
#define OP_I_NOT_EQUAL			171U
#define OP_U_GREATER_THAN		172U
#define OP_S_GREATER_THAN		173U
#define OP_U_GREATER_THAN_EQUAL		174U
#define OP_S_GREATER_THAN_EQUAL		175U
#define OP_U_LESS_THAN			176U
#define OP_S_LESS_THAN			177U
#define OP_U_LESS_THAN_EQUAL		178U
#define OP_S_LESS_THAN_EQUAL		179U
#define OP_F_ORD_EQUAL			180U
#define OP_F_UNORD_GREATER_THAN_EQUAL	191U
#define OP_SHIFT_RIGHT_LOGICAL		194U
#define OP_SHIFT_RIGHT_ARITHMETIC	195U
#define OP_SHIFT_LEFT_LOGICAL		196U
#define OP_BITWISE_OR			197U
#define OP_BITWISE_XOR			198U
#define OP_BITWISE_AND			199U
#define OP_NOT				200U
#define OP_PHI				245U
#define OP_LOOP_MERGE			246U
#define OP_SELECTION_MERGE		247U
#define OP_LABEL			248U
#define OP_BRANCH			249U
#define OP_BRANCH_CONDITIONAL		250U
#define OP_KILL				252U
#define OP_RETURN			253U
#define OP_RETURN_VALUE			254U
#define OP_TERMINATE_INVOCATION		4416U

#define DECORATION_ARRAY_STRIDE		6U
#define DECORATION_MATRIX_STRIDE	7U
#define DECORATION_BUILT_IN		11U
#define DECORATION_NO_PERSPECTIVE	13U
#define DECORATION_FLAT			14U
#define DECORATION_LOCATION		30U
#define DECORATION_COMPONENT		31U
#define DECORATION_BINDING		33U
#define DECORATION_DESCRIPTOR_SET	34U
#define DECORATION_OFFSET		35U
#define DECORATION_NO_CONTRACTION	42U

#define BUILT_IN_POSITION		0U
#define BUILT_IN_NUM_WORKGROUPS		24U
#define BUILT_IN_WORKGROUP_ID		26U
#define BUILT_IN_LOCAL_INVOCATION_ID	27U
#define BUILT_IN_GLOBAL_INVOCATION_ID	28U
#define BUILT_IN_LOCAL_INVOCATION_INDEX	29U
#define BUILT_IN_VERTEX_INDEX		42U
#define BUILT_IN_INSTANCE_INDEX		43U

#define STORAGE_UNIFORM_CONSTANT	0U
#define STORAGE_INPUT			1U
#define STORAGE_UNIFORM			2U
#define STORAGE_OUTPUT			3U
#define STORAGE_PRIVATE			6U
#define STORAGE_FUNCTION		7U
#define STORAGE_PUSH_CONSTANT		9U
#define STORAGE_STORAGE_BUFFER		12U

#define MODEL_VERTEX			0U
#define MODEL_FRAGMENT			4U
#define MODEL_GL_COMPUTE		5U
#define MODE_LOCAL_SIZE			17U

#define GLSL_ROUND			1U
#define GLSL_F_ABS			4U
#define GLSL_F_SIGN			6U
#define GLSL_FLOOR			8U
#define GLSL_CEIL			9U
#define GLSL_FRACT			10U
#define GLSL_SIN			13U
#define GLSL_COS			14U
#define GLSL_POW			26U
#define GLSL_EXP2			29U
#define GLSL_LOG2			30U
#define GLSL_SQRT			31U
#define GLSL_INVERSE_SQRT		32U
#define GLSL_F_MIN			37U
#define GLSL_U_MIN			38U
#define GLSL_S_MIN			39U
#define GLSL_F_MAX			40U
#define GLSL_U_MAX			41U
#define GLSL_S_MAX			42U
#define GLSL_F_CLAMP			43U
#define GLSL_U_CLAMP			44U
#define GLSL_S_CLAMP			45U
#define GLSL_F_MIX			46U
#define GLSL_STEP			48U
#define GLSL_SMOOTH_STEP		49U
#define GLSL_FMA			50U
#define GLSL_LENGTH			66U
#define GLSL_DISTANCE			67U
#define GLSL_NORMALIZE			69U

/*
 * The Vulkan formats the key may name, the blend factors and operations.
 *
 * Values referenced:
 * The Khronos Vulkan 1.3 specification (VkFormat R8G8B8A8_UNORM 37,
 * B8G8R8A8_UNORM 44, R16G16B16A16_SFLOAT 97, R32_SFLOAT 100, R32G32_SFLOAT 103,
 * R32G32B32_SFLOAT 106, R32G32B32A32_SFLOAT 109; VkBlendFactor ZERO 0
 * to SRC_ALPHA_SATURATE 14; VkBlendOp ADD 0, SUBTRACT 1,
 * REVERSE_SUBTRACT 2, MIN 3, MAX 4).
 */
#define FORMAT_R8G8B8A8_UNORM		37U
#define FORMAT_B8G8R8A8_UNORM		44U
#define FORMAT_R16G16B16A16_SFLOAT	97U
#define FORMAT_R32_SFLOAT		100U
#define FORMAT_R32G32_SFLOAT		103U
#define FORMAT_R32G32B32_SFLOAT		106U
#define FORMAT_R32G32B32A32_SFLOAT	109U
#define FACTOR_ZERO			0U
#define FACTOR_ONE			1U
#define FACTOR_SOURCE_COLOR		2U
#define FACTOR_ONE_MINUS_SOURCE_COLOR	3U
#define FACTOR_DESTINATION_COLOR	4U
#define FACTOR_ONE_MINUS_DESTINATION_COLOR 5U
#define FACTOR_SOURCE_ALPHA		6U
#define FACTOR_ONE_MINUS_SOURCE_ALPHA	7U
#define FACTOR_DESTINATION_ALPHA	8U
#define FACTOR_ONE_MINUS_DESTINATION_ALPHA 9U
#define FACTOR_CONSTANT_COLOR		10U
#define FACTOR_ONE_MINUS_CONSTANT_COLOR	11U
#define FACTOR_CONSTANT_ALPHA		12U
#define FACTOR_ONE_MINUS_CONSTANT_ALPHA	13U
#define FACTOR_SOURCE_ALPHA_SATURATE	14U
#define OPERATION_ADD			0U
#define OPERATION_SUBTRACT		1U
#define OPERATION_REVERSE_SUBTRACT	2U
#define OPERATION_MINIMUM		3U
#define OPERATION_MAXIMUM		4U

/* The pixel output registers a fragment shader may fill. */
#define PIXEL_OUTPUTS_MAX		4U

/* What an id stands for. */
#define ID_NONE				0U
#define ID_TYPE				1U
#define ID_CONSTANT			2U
#define ID_VALUE			3U
#define ID_POINTER			4U
#define ID_FUNCTION			5U
#define ID_LABEL			6U
#define ID_EXTENSION			7U
#define ID_SAMPLED_IMAGE		8U

/* The shapes of a type. */
#define TYPE_VOID			0U
#define TYPE_BOOL			1U
#define TYPE_INT			2U
#define TYPE_FLOAT			3U
#define TYPE_VECTOR			4U
#define TYPE_MATRIX			5U
#define TYPE_ARRAY			6U
#define TYPE_RUNTIME_ARRAY		7U
#define TYPE_STRUCT			8U
#define TYPE_POINTER			9U
#define TYPE_FUNCTION			10U
#define TYPE_IMAGE			11U
#define TYPE_SAMPLED_IMAGE		12U
#define TYPE_SAMPLER			13U

/* The id flags the decorations and declarations set. */
#define FLAG_LOCATION			0x01U
#define FLAG_FLAT			0x02U
#define FLAG_BUILT_IN			0x04U
#define FLAG_STRIDE			0x08U
#define FLAG_ENTERED			0x10U
#define FLAG_INTERFACE			0x20U
#define FLAG_HAS_INDEX			0x40U
#define FLAG_LOOP			0x80U

/* The largest composite one result may be, in words. */
#define COMPOSITE_MAX			16U

/* The float words the reader writes as constants. */
#define FLOAT_ZERO			0x00000000U
#define FLOAT_ONE			0x3f800000U
#define FLOAT_HALF			0x3f000000U
#define FLOAT_TWO			0x40000000U
#define FLOAT_THREE			0x40400000U
#define FLOAT_SIGN			0x80000000U
#define TRUE_WORD			0xffffffffU

int shader_spirv_reader_constant(struct shader_spirv_reader *reader, const uint32_t *words, uint32_t length);
int shader_spirv_reader_layout_interface(struct shader_spirv_reader *reader);
int shader_spirv_reader_entry_prologue(struct shader_spirv_reader *reader);
int shader_spirv_reader_entry_epilogue(struct shader_spirv_reader *reader);
int shader_spirv_reader_function(struct shader_spirv_reader *reader, uint32_t function, uint32_t *result);
int shader_spirv_reader_instruction(struct shader_spirv_reader *reader, const uint32_t *words, uint32_t length);
uint32_t shader_spirv_scalar_count(struct shader_spirv_reader *reader, uint32_t type);
uint32_t shader_spirv_byte_size(struct shader_spirv_reader *reader, uint32_t type);
int shader_spirv_find_block(struct shader_spirv_reader *reader, uint32_t label, uint32_t *at);
uint32_t shader_spirv_new_value(struct shader_spirv_reader *reader, uint32_t width);
void shader_spirv_emit(struct shader_spirv_reader *reader, uint32_t opcode, uint32_t destination, const struct powervr_ir_operand *a, const struct powervr_ir_operand *b, const struct powervr_ir_operand *c, uint32_t immediate, uint32_t immediate_2);
struct powervr_ir_operand shader_spirv_emit_value(struct shader_spirv_reader *reader, uint32_t opcode, const struct powervr_ir_operand *a, const struct powervr_ir_operand *b, const struct powervr_ir_operand *c);
struct powervr_ir_operand shader_spirv_constant_operand(uint32_t bits);
struct powervr_ir_operand shader_spirv_value_operand(uint32_t value, uint32_t component);
struct powervr_ir_operand shader_spirv_plain(struct shader_spirv_reader *reader, struct powervr_ir_operand operand);
struct powervr_ir_operand shader_spirv_float_negate(struct powervr_ir_operand operand);
struct powervr_ir_operand shader_spirv_float_absolute(struct powervr_ir_operand operand);
struct powervr_ir_operand shader_spirv_shared_word(struct shader_spirv_reader *reader, uint32_t request, uint32_t word);
int shader_spirv_request_add(struct shader_spirv_reader *reader, uint32_t kind, uint32_t part, uint32_t set, uint32_t binding, uint32_t words, uint32_t *index);
int shader_spirv_result_begin(struct shader_spirv_reader *reader, uint32_t result, uint32_t type, uint32_t count, struct powervr_ir_operand **operands);
int shader_spirv_id_operands(struct shader_spirv_reader *reader, uint32_t id, struct powervr_ir_operand **operands, uint32_t *count);
int shader_spirv_operand_word(struct shader_spirv_reader *reader, uint32_t id, uint32_t component, struct powervr_ir_operand *operand);
int shader_spirv_composite_offset(struct shader_spirv_reader *reader, uint32_t type, const uint32_t *indices, uint32_t count, uint32_t *offset, uint32_t *member_type);

#endif /* KERN_DRIVERS_GPU_POWERVR_SHADER_SPIRV_H */
