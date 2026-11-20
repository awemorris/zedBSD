/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The group operations of a subgroup, included by spirv.c.
 *
 * A subgroup here is one invocation (spirv-compute.inc), in every stage, so
 * every operation of its group is decided by the invocation alone, and is
 * lowered with constants, selections and the bit operations every backend
 * has (SPIR-V 1.6, section 3.52.21, Non-Uniform Instructions; Vulkan 1.3,
 * section 9.25, Subgroup Operations):
 *
 * - a vote (All, Any) is its predicate, AllEqual is true;
 * - a broadcast and a shuffle (Broadcast, BroadcastFirst, Shuffle,
 *   ShuffleXor, ShuffleUp, ShuffleDown) are the invocation's own value: the
 *   one invocation is the only one an id can name, and an id past the
 *   subgroup leaves the result undefined, so the same value is as good;
 * - a ballot is the predicate in bit 0 of the first word; InverseBallot and
 *   BallotBitExtract read bits of the operand; BallotBitCount counts bit 0
 *   (none before it for an exclusive scan); BallotFindLSB and BallotFindMSB
 *   are 0, the only bit of the subgroup (undefined when it is clear);
 * - the arithmetic's reduction, inclusive scan and clustered reduction are
 *   the invocation's value, and its exclusive scan is the operation's
 *   identity, none of the subgroup's invocations coming before.
 *
 * Only the subgroup scope is lowered; a group operation of another scope is
 * refused, and so are the quad operations, which need a subgroup of four.
 *
 * The file is part of spirv.c's translation unit: it uses that file's types,
 * constants and helpers, and its functions are declared with the others at
 * the top of spirv.c.
 */

/* The group operations of a scan (SPIR-V spec, section 3.28). */
#define GROUP_OPERATION_REDUCE 0U
#define GROUP_OPERATION_INCLUSIVE_SCAN 1U
#define GROUP_OPERATION_EXCLUSIVE_SCAN 2U
#define GROUP_OPERATION_CLUSTERED_REDUCE 3U

/* The group arithmetic of a subgroup (SPIR-V spec, section 3.52.21). */
#define OP_GROUP_NON_UNIFORM_FADD 350U
#define OP_GROUP_NON_UNIFORM_IMUL 351U
#define OP_GROUP_NON_UNIFORM_FMUL 352U
#define OP_GROUP_NON_UNIFORM_SMIN 353U
#define OP_GROUP_NON_UNIFORM_UMIN 354U
#define OP_GROUP_NON_UNIFORM_FMIN 355U
#define OP_GROUP_NON_UNIFORM_SMAX 356U
#define OP_GROUP_NON_UNIFORM_UMAX 357U
#define OP_GROUP_NON_UNIFORM_FMAX 358U
#define OP_GROUP_NON_UNIFORM_BITWISE_AND 359U
#define OP_GROUP_NON_UNIFORM_BITWISE_OR 360U
#define OP_GROUP_NON_UNIFORM_BITWISE_XOR 361U
#define OP_GROUP_NON_UNIFORM_LOGICAL_AND 362U
#define OP_GROUP_NON_UNIFORM_LOGICAL_OR 363U

/* The bits of a float's 1.0, +infinity and -infinity. */
#define GROUP_FLOAT_ONE 0x3F800000U
#define GROUP_FLOAT_INFINITY 0x7F800000U
#define GROUP_FLOAT_NEGATIVE_INFINITY 0xFF800000U

/* The words of a ballot, and the bits of each. */
#define GROUP_BALLOT_WORDS 4U
#define GROUP_BALLOT_WORD_SHIFT 5U
#define GROUP_BALLOT_BIT_MASK 31U

/*
 * Reports whether a built-in is one of a graphics stage's subgroup: its
 * size, the invocation's place in it and its ballots.  The number of the
 * subgroups and the subgroup's own are a compute shader's alone.
 */
static int
drv_gpu_spirv_subgroup_builtin(
	uint32_t builtin)
{
	/* The built-ins of a subgroup of one invocation in any stage. */
	switch (builtin) {
	case BUILTIN_SUBGROUP_SIZE:
	case BUILTIN_SUBGROUP_LOCAL_INVOCATION_ID:
	case BUILTIN_SUBGROUP_EQ_MASK:
	case BUILTIN_SUBGROUP_GE_MASK:
	case BUILTIN_SUBGROUP_GT_MASK:
	case BUILTIN_SUBGROUP_LE_MASK:
	case BUILTIN_SUBGROUP_LT_MASK:
		return 1;
	default:
		break;
	}

	/* Succeeded: any other built-in is not the subgroup's. */
	return 0;
}

/*
 * Checks that a group operation's execution scope is the subgroup, named by
 * a constant; refuses any other.
 */
static int
drv_gpu_spirv_group_scope(
	struct drv_gpu_spirv_parser *parser,
	uint32_t scope_id,
	uint32_t opcode,
	uint32_t offset)
{
	struct drv_gpu_spirv_id *scope;
	int error;

	/* Resolves the execution scope. */
	scope = drv_gpu_spirv_id(parser, scope_id);
	if (scope == NULL)
		return EINVAL;

	/* Only the subgroup scope, named by a constant, is lowered. */
	if (scope->kind != ID_CONSTANT || scope->constant != SCOPE_SUBGROUP) {
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "group operation of a scope other "
					     "than the subgroup");
		return error;
	}

	/* Succeeded: the operation is the subgroup's. */
	return 0;
}

/*
 * Declares the result word[2], of the type word[1], as the scalars of the
 * value `value_id`: the operation of a subgroup of one invocation that
 * gives back the invocation's own value.
 */
static int
drv_gpu_spirv_group_same(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t value_id,
	uint32_t opcode,
	uint32_t offset)
{
	struct drv_gpu_spirv_id *record;
	uint32_t scalars[MAX_COMPONENTS];
	uint32_t scalar_count;
	uint32_t components;
	uint32_t index;
	int error;

	/* Only a value made of the result type's scalars is given back. */
	scalar_count = drv_gpu_spirv_operand_wide(parser, value_id, scalars);
	components = drv_gpu_spirv_aggregate_scalars(parser, word[1]);
	if (scalar_count == 0U || scalar_count != components) {
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "group operation on something that "
					     "is not a value made of scalars");
		return error;
	}

	/* Names the operand's scalars under the result. */
	record =
	    drv_gpu_spirv_result(parser, word[2], word[1], scalar_count, 0);
	if (record == NULL)
		return EINVAL;
	for (index = 0U; index < scalar_count; index++)
		record->comp[index] = scalars[index];

	/* Succeeded: the result is the invocation's value. */
	return 0;
}

/*
 * Declares the result word[2], of the scalar or vector type word[1], as a
 * constant whose every component has the bits `bits`, materialized on its
 * first use.
 */
static int
drv_gpu_spirv_group_constant(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t bits)
{
	struct drv_gpu_spirv_id *record;

	/* The result is a constant of its type. */
	record = drv_gpu_spirv_id(parser, word[2]);
	if (record == NULL || record->kind != ID_NONE)
		return EINVAL;
	record->kind = ID_CONSTANT;
	record->type = word[1];
	record->constant = bits;
	record->count = 0U;

	/* Succeeded: the constant is known. */
	return 0;
}

/*
 * Lowers a vote, a broadcast, a ballot or a shuffle of a subgroup of one
 * invocation (see the top of this file).
 */
static int
drv_gpu_spirv_lower_group(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	int error;

	/* The result type, the result, the scope and the operand. */
	if (count < 5U)
		return EINVAL;

	/* Only the subgroup scope is lowered. */
	error = drv_gpu_spirv_group_scope(parser, word[3], opcode, offset);
	if (error != 0)
		return error;

	/* Decides the result by the operation. */
	switch (opcode) {
	case OP_GROUP_NON_UNIFORM_ALL:
	case OP_GROUP_NON_UNIFORM_ANY:
	case OP_GROUP_NON_UNIFORM_BROADCAST_FIRST:
		/* The predicate, or the value, of the only invocation. */
		if (count != 5U)
			return EINVAL;
		error = drv_gpu_spirv_group_same(parser,
						 word,
						 word[4],
						 opcode,
						 offset);
		break;

	case OP_GROUP_NON_UNIFORM_ALL_EQUAL:
		/* One invocation's value equals itself. */
		if (count != 5U)
			return EINVAL;
		error = drv_gpu_spirv_group_constant(parser,
						     word,
						     BOOL_TRUE_BITS);
		break;

	case OP_GROUP_NON_UNIFORM_BROADCAST:
	case OP_GROUP_NON_UNIFORM_SHUFFLE:
	case OP_GROUP_NON_UNIFORM_SHUFFLE_XOR:
	case OP_GROUP_NON_UNIFORM_SHUFFLE_UP:
	case OP_GROUP_NON_UNIFORM_SHUFFLE_DOWN:
		/*
		 * The value of the invocation the id, mask or delta names: the
		 * only one there is (any other is undefined).
		 */
		if (count != 6U)
			return EINVAL;
		error = drv_gpu_spirv_group_same(parser,
						 word,
						 word[4],
						 opcode,
						 offset);
		break;

	case OP_GROUP_NON_UNIFORM_BALLOT_FIND_LSB:
	case OP_GROUP_NON_UNIFORM_BALLOT_FIND_MSB:
		/* The subgroup's only bit, bit 0. */
		if (count != 5U)
			return EINVAL;
		error = drv_gpu_spirv_group_constant(parser, word, 0U);
		break;

	case OP_GROUP_NON_UNIFORM_BALLOT:
		/* The predicate in the invocation's bit. */
		error = drv_gpu_spirv_lower_ballot(parser,
						   word,
						   count,
						   opcode,
						   offset);
		break;

	case OP_GROUP_NON_UNIFORM_INVERSE_BALLOT:
	case OP_GROUP_NON_UNIFORM_BALLOT_BIT_COUNT:
		/* The invocation's bit read from a ballot. */
		error = drv_gpu_spirv_lower_ballot_read(parser,
							word,
							count,
							opcode,
							offset);
		break;

	case OP_GROUP_NON_UNIFORM_BALLOT_BIT_EXTRACT:
		/* One bit of a ballot, at an index known at run time. */
		error = drv_gpu_spirv_lower_ballot_bit(parser,
						       word,
						       count,
						       opcode,
						       offset);
		break;

	case OP_GROUP_NON_UNIFORM_QUAD_BROADCAST:
	case OP_GROUP_NON_UNIFORM_QUAD_SWAP:
		/* A quad is four invocations, more than a subgroup has. */
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "quad operation (a subgroup is one "
					     "invocation)");
		break;

	default:
		/* The dispatch sends no other operation here. */
		error = EINVAL;
		break;
	}

	/* Reports why the operation could not be lowered. */
	if (error != 0)
		return error;

	/* Succeeded: the result is the operation of the one invocation. */
	return 0;
}

/*
 * Lowers OpGroupNonUniformBallot of a subgroup of one invocation: the
 * predicate in bit 0 of the first word, the other words clear.
 */
static int
drv_gpu_spirv_lower_ballot(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct drv_gpu_spirv_id *record;
	uint32_t predicate[4];
	uint32_t predicate_type;
	uint32_t components;
	uint32_t booleans;
	uint32_t words;
	uint32_t one;
	uint32_t zero;
	uint32_t index;
	int error;

	/* The result type, the result, the scope and the predicate. */
	if (count != 5U)
		return EINVAL;

	/* Measures the predicate and the ballot. */
	components = drv_gpu_spirv_operand(parser, word[4], predicate);
	predicate_type = drv_gpu_spirv_operand_type(parser, word[4]);
	booleans = drv_gpu_spirv_bool_components(parser, predicate_type);
	words = drv_gpu_spirv_int_components(parser, word[1]);

	/* Refuses anything but one Boolean into four words. */
	if (components != 1U ||
	    booleans != 1U ||
	    words != GROUP_BALLOT_WORDS) {
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "ballot of something other than a "
					     "Boolean into four words");
		return error;
	}

	/* The invocation's bit is 1 where the predicate holds. */
	one = drv_gpu_spirv_integer_constant(parser, 1U);
	zero = drv_gpu_spirv_integer_constant(parser, 0U);
	record = drv_gpu_spirv_result(parser,
				      word[2],
				      word[1],
				      GROUP_BALLOT_WORDS,
				      0);
	if (record == NULL)
		return EINVAL;
	record->comp[0] = drv_gpu_spirv_select_value(parser,
						     predicate[0],
						     one,
						     zero);

	/* No other invocation: the other words are clear. */
	for (index = 1U; index < GROUP_BALLOT_WORDS; index++)
		record->comp[index] = zero;

	/* Reports an emit that failed. */
	if (parser->error != 0)
		return parser->error;

	/* Succeeded: the ballot holds the invocation's bit. */
	return 0;
}

/*
 * Lowers OpGroupNonUniformInverseBallot (whether the invocation's bit, bit
 * 0 of the first word, is set) and OpGroupNonUniformBallotBitCount (the
 * bit for a reduction and an inclusive scan, 0 for an exclusive scan) of a
 * subgroup of one invocation.
 */
static int
drv_gpu_spirv_lower_ballot_read(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct drv_gpu_spirv_id *record;
	uint32_t ballot[4];
	uint32_t ballot_id;
	uint32_t components;
	uint32_t integers;
	uint32_t one;
	uint32_t zero;
	uint32_t bit;
	int error;

	/*
	 * An inverse ballot reads the ballot after the scope, a bit count
	 * after the group operation.
	 */
	if (opcode == OP_GROUP_NON_UNIFORM_INVERSE_BALLOT) {
		if (count != 5U)
			return EINVAL;
		ballot_id = word[4];
	} else {
		if (count != 6U)
			return EINVAL;
		ballot_id = word[5];
	}

	/* A bit count of an exclusive scan counts no invocation before the only one. */
	if (opcode == OP_GROUP_NON_UNIFORM_BALLOT_BIT_COUNT &&
	    word[4] == GROUP_OPERATION_EXCLUSIVE_SCAN) {
		error = drv_gpu_spirv_group_constant(parser, word, 0U);
		if (error != 0)
			return error;

		/* Succeeded: no bit is counted. */
		return 0;
	}

	/* Refuses a bit count of a clustered or partitioned operation. */
	if (opcode == OP_GROUP_NON_UNIFORM_BALLOT_BIT_COUNT &&
	    word[4] != GROUP_OPERATION_REDUCE &&
	    word[4] != GROUP_OPERATION_INCLUSIVE_SCAN) {
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "ballot bit count of a group "
					     "operation other than a reduction "
					     "or a scan");
		return error;
	}

	/* Measures the ballot: four 32-bit words. */
	components = drv_gpu_spirv_operand(parser, ballot_id, ballot);
	integers = drv_gpu_spirv_operand_int_components(parser, ballot_id);
	if (components != GROUP_BALLOT_WORDS ||
	    integers != GROUP_BALLOT_WORDS) {
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "ballot read that is not four "
					     "integer words");
		return error;
	}

	/* The invocation's bit, bit 0 of the first word. */
	one = drv_gpu_spirv_integer_constant(parser, 1U);
	bit = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_IAND, ballot[0], one);
	record = drv_gpu_spirv_result(parser, word[2], word[1], 1U, 0);
	if (record == NULL)
		return EINVAL;

	if (opcode == OP_GROUP_NON_UNIFORM_BALLOT_BIT_COUNT) {
		/* A count of the subgroup's bits is the one bit. */
		record->comp[0] = bit;
	} else {
		/* An inverse ballot is whether the bit is set. */
		zero = drv_gpu_spirv_integer_constant(parser, 0U);
		record->comp[0] = drv_gpu_spirv_emit_value(parser,
							   DRV_GPU_IR_INE,
							   bit,
							   zero);
	}

	/* Reports an emit that failed. */
	if (parser->error != 0)
		return parser->error;

	/* Succeeded: the result reads the invocation's bit. */
	return 0;
}

/*
 * Lowers OpGroupNonUniformBallotBitExtract: whether the bit of the ballot
 * at the index is set, the word the index names chosen among the four and
 * the bit shifted down from it.
 */
static int
drv_gpu_spirv_lower_ballot_bit(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct drv_gpu_spirv_id *record;
	uint32_t ballot[4];
	uint32_t index_value[4];
	uint32_t components;
	uint32_t integers;
	uint32_t index_components;
	uint32_t index_integers;
	uint32_t which;
	uint32_t shift;
	uint32_t chosen;
	uint32_t number;
	uint32_t matched;
	uint32_t bit;
	uint32_t one;
	uint32_t zero;
	uint32_t index;
	int error;

	/* The result type, the result, the scope, the ballot and the index. */
	if (count != 6U)
		return EINVAL;

	/* Measures the ballot and the index. */
	components = drv_gpu_spirv_operand(parser, word[4], ballot);
	integers = drv_gpu_spirv_operand_int_components(parser, word[4]);
	index_components = drv_gpu_spirv_operand(parser, word[5], index_value);
	index_integers = drv_gpu_spirv_operand_int_components(parser, word[5]);

	/* Refuses anything but four integer words and one integer index. */
	if (components != GROUP_BALLOT_WORDS ||
	    integers != GROUP_BALLOT_WORDS ||
	    index_components != 1U ||
	    index_integers != 1U) {
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "ballot bit extract of something "
					     "other than four words and an "
					     "integer index");
		return error;
	}

	/* The word the index names and the bit's place in it. */
	shift = drv_gpu_spirv_integer_constant(parser, GROUP_BALLOT_WORD_SHIFT);
	which = drv_gpu_spirv_emit_value(parser,
					 DRV_GPU_IR_SHR,
					 index_value[0],
					 shift);
	number = drv_gpu_spirv_integer_constant(parser, GROUP_BALLOT_BIT_MASK);
	shift = drv_gpu_spirv_emit_value(parser,
					 DRV_GPU_IR_IAND,
					 index_value[0],
					 number);

	/* Chooses the word: the first unless the index names a later one. */
	chosen = ballot[0];
	for (index = 1U; index < GROUP_BALLOT_WORDS; index++) {
		number = drv_gpu_spirv_integer_constant(parser, index);
		matched = drv_gpu_spirv_emit_value(parser,
						   DRV_GPU_IR_IEQ,
						   which,
						   number);
		chosen = drv_gpu_spirv_select_value(parser,
						    matched,
						    ballot[index],
						    chosen);
	}

	/* The bit, shifted down to bit 0. */
	bit = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_SHR, chosen, shift);
	one = drv_gpu_spirv_integer_constant(parser, 1U);
	bit = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_IAND, bit, one);

	/* The result is whether the bit is set. */
	zero = drv_gpu_spirv_integer_constant(parser, 0U);
	record = drv_gpu_spirv_result(parser, word[2], word[1], 1U, 0);
	if (record == NULL)
		return EINVAL;
	record->comp[0] =
	    drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_INE, bit, zero);

	/* Reports an emit that failed. */
	if (parser->error != 0)
		return parser->error;

	/* Succeeded: the result is the ballot's bit at the index. */
	return 0;
}

/*
 * Lowers the group arithmetic of a subgroup of one invocation: a reduction,
 * an inclusive scan and a clustered reduction are the invocation's value,
 * an exclusive scan the operation's identity.
 */
static int
drv_gpu_spirv_lower_group_arithmetic(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	uint32_t bits;
	uint32_t expected;
	int error;

	/*
	 * The result type, the result, the scope, the group operation and the
	 * value.
	 */
	if (count < 6U)
		return EINVAL;

	/* Only the subgroup scope is lowered. */
	error = drv_gpu_spirv_group_scope(parser, word[3], opcode, offset);
	if (error != 0)
		return error;

	/* A clustered reduction also names the cluster's size. */
	expected = 6U;
	if (word[4] == GROUP_OPERATION_CLUSTERED_REDUCE)
		expected = 7U;
	if (count != expected)
		return EINVAL;

	/* Decides the result by the group operation. */
	switch (word[4]) {
	case GROUP_OPERATION_REDUCE:
	case GROUP_OPERATION_INCLUSIVE_SCAN:
	case GROUP_OPERATION_CLUSTERED_REDUCE:
		/*
		 * The only invocation's value (a cluster past the subgroup is
		 * undefined, so the same value is as good).
		 */
		error = drv_gpu_spirv_group_same(parser,
						 word,
						 word[5],
						 opcode,
						 offset);
		break;

	case GROUP_OPERATION_EXCLUSIVE_SCAN:
		/* No invocation before the only one: the identity. */
		error = drv_gpu_spirv_group_identity(parser,
						     opcode,
						     word[1],
						     &bits);
		if (error != 0) {
			error = drv_gpu_spirv_refuse(parser,
						     opcode,
						     offset,
						     "exclusive scan of a type the "
						     "operation has no identity "
						     "for here");
			break;
		}
		error = drv_gpu_spirv_group_constant(parser, word, bits);
		break;

	default:
		/* A partitioned operation is not lowered. */
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "group arithmetic of an operation "
					     "other than a reduction, a scan or "
					     "a clustered reduction");
		break;
	}

	/* Reports why the arithmetic could not be lowered. */
	if (error != 0)
		return error;

	/* Succeeded: the result is the arithmetic of the one invocation. */
	return 0;
}

/*
 * Reports the bits of the identity of a group arithmetic operation for the
 * scalar or vector type `type_id`: of a 32-bit float, of a 32-bit or 16-bit
 * integer (a 16-bit one in the low half of its word) or of a Boolean.
 * Returns ENOTSUP for a type the operation does not take here.
 */
static int
drv_gpu_spirv_group_identity(
	struct drv_gpu_spirv_parser *parser,
	uint32_t opcode,
	uint32_t type_id,
	uint32_t *bits)
{
	struct drv_gpu_spirv_id *type;
	uint32_t components;
	uint32_t ones;
	uint32_t sign;

	/* The Boolean operations: true for and, false for or and exclusive or. */
	if (opcode == OP_GROUP_NON_UNIFORM_LOGICAL_AND ||
	    opcode == OP_GROUP_NON_UNIFORM_LOGICAL_OR ||
	    opcode == OP_GROUP_NON_UNIFORM_LOGICAL_XOR) {
		components = drv_gpu_spirv_bool_components(parser, type_id);
		if (components == 0U)
			return ENOTSUP;
		*bits = 0U;
		if (opcode == OP_GROUP_NON_UNIFORM_LOGICAL_AND)
			*bits = BOOL_TRUE_BITS;

		/* Succeeded: the Boolean identity. */
		return 0;
	}

	/* The float operations: 0, 1, +infinity, -infinity. */
	if (opcode == OP_GROUP_NON_UNIFORM_FADD ||
	    opcode == OP_GROUP_NON_UNIFORM_FMUL ||
	    opcode == OP_GROUP_NON_UNIFORM_FMIN ||
	    opcode == OP_GROUP_NON_UNIFORM_FMAX) {
		components = drv_gpu_spirv_float_components(parser, type_id);
		if (components == 0U)
			return ENOTSUP;
		if (opcode == OP_GROUP_NON_UNIFORM_FADD) {
			*bits = 0U;
		} else if (opcode == OP_GROUP_NON_UNIFORM_FMUL) {
			*bits = GROUP_FLOAT_ONE;
		} else if (opcode == OP_GROUP_NON_UNIFORM_FMIN) {
			*bits = GROUP_FLOAT_INFINITY;
		} else {
			*bits = GROUP_FLOAT_NEGATIVE_INFINITY;
		}

		/* Succeeded: the float identity. */
		return 0;
	}

	/* The integer operations take a 32-bit or 16-bit integer. */
	components = drv_gpu_spirv_int_components(parser, type_id);
	if (components == 0U)
		return ENOTSUP;

	/* The integer's scalar type, for its width. */
	type = drv_gpu_spirv_id(parser, type_id);
	if (type != NULL && type->kind == ID_TYPE_VECTOR)
		type = drv_gpu_spirv_id(parser, type->type);
	if (type == NULL)
		return ENOTSUP;

	/* All the width's bits, and its sign bit. */
	ones = 0xFFFFFFFFU;
	if (type->width < 32U)
		ones = (1U << type->width) - 1U;
	sign = 1U << (type->width - 1U);

	/* The identity of the integer operation. */
	switch (opcode) {
	case OP_GROUP_NON_UNIFORM_IADD:
	case OP_GROUP_NON_UNIFORM_UMAX:
	case OP_GROUP_NON_UNIFORM_BITWISE_OR:
	case OP_GROUP_NON_UNIFORM_BITWISE_XOR:
		*bits = 0U;
		break;
	case OP_GROUP_NON_UNIFORM_IMUL:
		*bits = 1U;
		break;
	case OP_GROUP_NON_UNIFORM_SMIN:
		*bits = sign - 1U;
		break;
	case OP_GROUP_NON_UNIFORM_SMAX:
		*bits = sign;
		break;
	case OP_GROUP_NON_UNIFORM_UMIN:
	case OP_GROUP_NON_UNIFORM_BITWISE_AND:
		*bits = ones;
		break;
	default:
		return ENOTSUP;
	}

	/* Succeeded: the integer identity. */
	return 0;
}
