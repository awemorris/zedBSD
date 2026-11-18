/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The math part of the SPIR-V parser, included by spirv.c.
 *
 * It lowers the GLSL.std.450 functions the rest of the parser does not --
 * the inverse and hyperbolic trigonometry, Modf and Frexp with their struct
 * forms, Fma, Ldexp, the normalized packings, FaceForward, Refract, the bit
 * searches and the NaN-aware minimum, maximum and clamp -- and the core
 * instructions OpIsNan, OpIsInf, the bit-field instructions, OpBitReverse,
 * OpBitCount and the carrying and widening integer arithmetic.  Each is
 * built from operations every backend already has: the IR gains no
 * operation, so a backend that lowers the IR lowers these as well.
 *
 * Precision follows the GLSL specifications: a function the GLSL ES 3.00
 * and GLSL 4.60 specifications define by an equation (the hyperbolic ones,
 * the normalized packings) is lowered as that equation, and its precision is
 * the equation's; the inverse trigonometry, whose precision those
 * specifications leave open, is held well inside the 4096 ULP the Vulkan
 * specification gives atan2 and its derived functions.
 *
 * The file is part of spirv.c's translation unit: it uses that file's types,
 * constants and helpers, and its functions are declared with the others at
 * the top of spirv.c.
 */

/*
 * The bits of a float's sign, of its magnitude, of an infinity's magnitude
 * and of its exponent field (IEEE 754 binary32: 1 sign bit, 8 exponent bits
 * biased by 127, 23 fraction bits).
 */
#define MATH_SIGN_BITS 0x80000000U
#define MATH_MAGNITUDE_BITS 0x7FFFFFFFU
#define MATH_INFINITY_BITS 0x7F800000U
#define MATH_EXPONENT_FIELD 0xFFU
#define MATH_FRACTION_SHIFT 23U
#define MATH_EXPONENT_BIAS 127U

/*
 * The sign and fraction bits of a float, and the exponent field of a float
 * in [0.5, 1): a significand keeps the first and takes the second.
 */
#define MATH_SIGN_AND_FRACTION_BITS 0x807FFFFFU
#define MATH_HALF_EXPONENT_BITS 0x3F000000U

/* The bits of the floats pi / 2, pi and -1. */
#define FLOAT_HALF_PI_BITS 0x3FC90FDBU
#define FLOAT_PI_BITS 0x40490FDBU
#define FLOAT_MINUS_ONE_BITS 0xBF800000U

/* The bits of 2 log2(e), by which tanh's 2x is taken to a power of two. */
#define FLOAT_TWO_LOG2_E_BITS 0x4038AA3BU

/*
 * The bits of 10 and -10: tanh(10) rounds to 1 in binary32, so tanh's
 * argument is held in [-10, 10] and its exponential never overflows.
 */
#define FLOAT_TEN_BITS 0x41200000U
#define FLOAT_MINUS_TEN_BITS 0xC1200000U

/*
 * The bits of 2^64, which takes a subnormal float to a normal one for
 * Frexp, and the exponent Frexp then gives back.
 */
#define FLOAT_TWO_TO_64_BITS 0x5F800000U
#define MATH_SUBNORMAL_SCALE 64U

/*
 * Ldexp's exponent is held in [-252, 254]: the sum of two exponents of
 * normal floats, each in [-126, 127], and wide enough that a farther one
 * changes no finite float's result but to zero or an infinity.
 */
#define MATH_LDEXP_LOW 0xFFFFFF04U
#define MATH_LDEXP_HIGH 254U

/*
 * The normalized packings: the largest code of an unsigned 16-bit and
 * 8-bit field, of a signed one, as floats, and the field masks.
 */
#define FLOAT_65535_BITS 0x477FFF00U
#define FLOAT_32767_BITS 0x46FFFE00U
#define FLOAT_255_BITS 0x437F0000U
#define FLOAT_127_BITS 0x42FE0000U
#define MATH_MASK_16 0xFFFFU
#define MATH_MASK_8 0xFFU

/*
 * The reciprocals the unpackings multiply by, as the first terms of the
 * geometric series 1 / (2^n - 1) = 2^-n (1 + 2^-n + 2^-2n + ...), which are
 * exact in binary32 and leave out less than 2^-32 of the quotient:
 * 2^-16 + 2^-32 for 65535 (65535 times it is 1 - 2^-32, which rounds to 1),
 * 2^-15 + 2^-30 for 32767, and 2^-7 + 2^-14 + 2^-21 + 2^-28 for 127.  An
 * 8-bit unsigned code is first multiplied by 257 (255 * 257 is 65535) and
 * divided as a 16-bit one.
 */
#define FLOAT_INVERSE_65535_BITS 0x37800080U
#define FLOAT_INVERSE_32767_BITS 0x38000100U
#define FLOAT_INVERSE_127_BITS 0x3C010204U
#define MATH_UNORM_8_TO_16 257U

/*
 * The coefficients of arctan(t) / t = 1 + a2 t^2 + a4 t^4 + ... + a16 t^16
 * on 0 <= t <= 1, whose error is at most 2e-8 there, as binary32 bits,
 * the constant term first.
 *
 * Values referenced: M. Abramowitz and I. A. Stegun, Handbook of
 * Mathematical Functions, 10th printing (1972), formula 4.4.49 (from C.
 * Hastings, Approximations for Digital Computers): a2 -0.3333314528,
 * a4 0.1999355085, a6 -0.1420889944, a8 0.1065626393, a10 -0.0752896400,
 * a12 0.0429096138, a14 -0.0161657367, a16 0.0028662257.
 */
#define MATH_ATAN_TERMS 9U
static const uint32_t drv_gpu_spirv_atan_coefficients[MATH_ATAN_TERMS] = {
	0x3F800000U,
	0xBEAAAA6CU,
	0x3E4CBBE5U,
	0xBE117FC7U,
	0x3DDA3D83U,
	0xBD9A3174U,
	0x3D2FC1FEU,
	0xBC846E02U,
	0x3B3BD74AU
};

/*
 * Lowers a GLSL.std.450 function of this part, or refuses one no part
 * lowers.
 *
 * The functions of one component at a time -- Asin, Acos, Atan, Atan2,
 * the hyperbolic ones, Fma, Ldexp, NMin, NMax, NClamp and the bit searches
 * -- are lowered here; the struct-valued and pointer-writing ones (Modf,
 * Frexp), the packings and the functions of whole vectors (FaceForward,
 * Refract) have lowerings of their own.
 */
static int
drv_gpu_spirv_lower_math(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct drv_gpu_spirv_id *record;
	uint32_t operand[3][4];
	uint32_t operand_count;
	uint32_t operands;
	uint32_t components;
	uint32_t function;
	uint32_t width;
	uint32_t index;
	int integers;
	int error;

	/* Sends the functions with lowerings of their own to them. */
	function = word[4];

	/* Picks the lowering by the function. */
	switch (function) {
	case GLSL_MODF:
	case GLSL_MODF_STRUCT:
	case GLSL_FREXP:
	case GLSL_FREXP_STRUCT:
		/* Splits the operand into two parts. */
		error = drv_gpu_spirv_lower_split(parser,
						  word,
						  count,
						  opcode,
						  offset);
		if (error != 0)
			return error;

		/* Succeeded: both parts are lowered. */
		return 0;

	case GLSL_PACK_SNORM_4X8:
	case GLSL_PACK_UNORM_4X8:
	case GLSL_PACK_SNORM_2X16:
	case GLSL_PACK_UNORM_2X16:
	case GLSL_UNPACK_SNORM_2X16:
	case GLSL_UNPACK_UNORM_2X16:
	case GLSL_UNPACK_SNORM_4X8:
	case GLSL_UNPACK_UNORM_4X8:
		/* Packs a vector into an integer, or unpacks one. */
		error = drv_gpu_spirv_lower_pack(parser,
						 word,
						 count,
						 opcode,
						 offset);
		if (error != 0)
			return error;

		/* Succeeded: the packing is lowered. */
		return 0;

	case GLSL_FACE_FORWARD:
	case GLSL_REFRACT:
		/* Works on whole vectors. */
		error = drv_gpu_spirv_lower_facing(parser,
						   word,
						   count,
						   opcode,
						   offset);
		if (error != 0)
			return error;

		/* Succeeded: the vector function is lowered. */
		return 0;

	case GLSL_ASIN:
	case GLSL_ACOS:
	case GLSL_ATAN:
	case GLSL_SINH:
	case GLSL_COSH:
	case GLSL_TANH:
	case GLSL_ASINH:
	case GLSL_ACOSH:
	case GLSL_ATANH:
		operands = 1U;
		integers = 0;
		break;

	case GLSL_ATAN2:
	case GLSL_NMIN:
	case GLSL_NMAX:
	case GLSL_LDEXP:
		operands = 2U;
		integers = 0;
		break;

	case GLSL_FMA:
	case GLSL_NCLAMP:
		operands = 3U;
		integers = 0;
		break;

	case GLSL_FIND_I_LSB:
	case GLSL_FIND_S_MSB:
	case GLSL_FIND_U_MSB:
		operands = 1U;
		integers = 1;
		break;

	default:
		/* Records the function no part of the parser lowers. */
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
	 * The result is a float scalar or vector, or for the bit searches a
	 * 32-bit integer one.
	 */
	width = 32U;
	if (integers != 0) {
		components = drv_gpu_spirv_int_components(parser, word[1]);
		width = drv_gpu_spirv_int_width(parser, word[1]);
	} else {
		components = drv_gpu_spirv_float_components(parser, word[1]);
	}

	/* Refuses a result of another kind or width. */
	if (components == 0U || width != 32U) {
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "extended instruction operand");
		return error;
	}

	/*
	 * Resolves each operand, which has the result's size (Ldexp's exponent
	 * is a 32-bit integer one); an operand may emit a constant.
	 */
	for (index = 0U; index < operands; index++) {
		operand_count = drv_gpu_spirv_operand(parser,
						      word[5U + index],
						      operand[index]);
		if (operand_count != components) {
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "extended instruction operand");
			return error;
		}

		/* An integer operand must be 32 bits wide. */
		if (integers != 0 ||
		    (function == GLSL_LDEXP && index == 1U)) {
			width = drv_gpu_spirv_operand_int_width(parser,
								word[5U + index]);
			if (width != 32U) {
				error = drv_gpu_spirv_refuse(
				    parser,
				    opcode,
				    offset,
				    "extended instruction operand of another "
				    "width");
				return error;
			}
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
		record->comp[index] = drv_gpu_spirv_math_component(parser,
								   function,
								   operand,
								   index);
	}

	/* Reports a failure an emitter latched. */
	if (parser->error != 0)
		return parser->error;

	/* Succeeded: the function is lowered. */
	return 0;
}

/*
 * Lowers one component of a per-component function of this part and
 * returns the IR value of the result.
 */
static uint32_t
drv_gpu_spirv_math_component(
	struct drv_gpu_spirv_parser *parser,
	uint32_t function,
	uint32_t operand[3][4],
	uint32_t component)
{
	uint32_t value;
	uint32_t x;
	uint32_t y;
	uint32_t z;
	uint32_t square_root;
	uint32_t one;
	uint32_t below;
	uint32_t above;
	uint32_t product;

	/* Names the component of each operand. */
	x = operand[0][component];
	y = operand[1][component];
	z = operand[2][component];

	/* Lowers by the function. */
	switch (function) {
	case GLSL_ATAN:
		/* The angle in (-pi / 2, pi / 2) whose tangent is x. */
		value = drv_gpu_spirv_math_atan(parser, x);
		break;

	case GLSL_ATAN2:
		/* The angle in [-pi, pi] of the point (x, y) = (second, first). */
		value = drv_gpu_spirv_math_atan2(parser, x, y);
		break;

	case GLSL_ASIN:
	case GLSL_ACOS:
		/*
		 * asin(x) = atan2(x, sqrt(1 - x^2)) and acos(x) = atan2(sqrt(1 -
		 * x^2), x), the forms the Vulkan specification derives their
		 * precision from; 1 - x^2 is taken as (1 - x)(1 + x), whose
		 * factors are exact near |x| = 1 where the square root is
		 * steepest.
		 */
		one = drv_gpu_spirv_shared_constant(parser,
						    &parser->one_value,
						    DRV_GPU_IR_CONST,
						    FLOAT_ONE_BITS);
		below = drv_gpu_spirv_emit_value(parser,
						 DRV_GPU_IR_FSUB,
						 one,
						 x);
		above = drv_gpu_spirv_emit_value(parser,
						 DRV_GPU_IR_FADD,
						 one,
						 x);
		product = drv_gpu_spirv_emit_value(parser,
						   DRV_GPU_IR_FMUL,
						   below,
						   above);
		square_root = drv_gpu_spirv_emit_value(parser,
						       DRV_GPU_IR_SQRT,
						       product,
						       0U);

		/* Takes the angle whose sine or cosine is x. */
		if (function == GLSL_ASIN) {
			value = drv_gpu_spirv_math_atan2(parser,
							 x,
							 square_root);
		} else {
			value = drv_gpu_spirv_math_atan2(parser,
							 square_root,
							 x);
		}
		break;

	case GLSL_SINH:
	case GLSL_COSH:
	case GLSL_TANH:
		/* The hyperbolic functions, from the exponential. */
		value = drv_gpu_spirv_math_hyperbolic(parser, function, x);
		break;

	case GLSL_ASINH:
	case GLSL_ACOSH:
	case GLSL_ATANH:
		/* Their inverses, from the logarithm. */
		value = drv_gpu_spirv_math_inverse_hyperbolic(parser,
							      function,
							      x);
		break;

	case GLSL_FMA:
		/*
		 * a * b + c.  GLSL asks only that every fma consumed by a precise
		 * variable be computed the same way, not that it round once; the
		 * product and the sum are kept apart when the result is precise
		 * (NoContraction), and a code generator may fuse them otherwise.
		 */
		product = drv_gpu_spirv_emit_value(parser,
						   DRV_GPU_IR_FMUL,
						   x,
						   y);
		value = drv_gpu_spirv_emit_value(parser,
						 DRV_GPU_IR_FADD,
						 product,
						 z);
		break;

	case GLSL_LDEXP:
		/* x times 2 to the integer y. */
		value = drv_gpu_spirv_math_ldexp(parser, x, y);
		break;

	case GLSL_NMIN:
	case GLSL_NMAX:
		/* The smaller or the larger, a NaN operand giving the other. */
		value = drv_gpu_spirv_math_nan_min_max(parser, function, x, y);
		break;

	case GLSL_NCLAMP:
		/* NMin(NMax(x, minVal), maxVal). */
		value = drv_gpu_spirv_math_nan_min_max(parser, GLSL_NMAX, x, y);
		value = drv_gpu_spirv_math_nan_min_max(parser,
						       GLSL_NMIN,
						       value,
						       z);
		break;

	case GLSL_FIND_I_LSB:
		/* The number of the lowest set bit, or -1 for none. */
		value = drv_gpu_spirv_math_find_lsb(parser, x);
		break;

	case GLSL_FIND_U_MSB:
		/* The number of the highest set bit, or -1 for none. */
		value = drv_gpu_spirv_math_find_msb(parser, x);
		break;

	default:
		/*
		 * FindSMsb: the highest bit that differs from the sign bit, the
		 * highest set bit of x for a positive x and of its complement
		 * for a negative one (x ^ (x >> 31), the shift arithmetic).
		 */
		one = drv_gpu_spirv_math_integer_op(parser,
						    DRV_GPU_IR_ASR,
						    x,
						    31U);
		value = drv_gpu_spirv_emit_value(parser,
						 DRV_GPU_IR_IXOR,
						 x,
						 one);
		value = drv_gpu_spirv_math_find_msb(parser, value);
		break;
	}

	/* Succeeded: the component's value. */
	return value;
}

/*
 * Emits `op` of `value` and the 32-bit integer constant `bits`, and returns
 * its result.
 */
static uint32_t
drv_gpu_spirv_math_integer_op(
	struct drv_gpu_spirv_parser *parser,
	enum drv_gpu_shader_ir_op op,
	uint32_t value,
	uint32_t bits)
{
	uint32_t constant;
	uint32_t result;

	/* Names the constant, then applies the operation. */
	constant = drv_gpu_spirv_integer_constant(parser, bits);
	result = drv_gpu_spirv_emit_value(parser, op, value, constant);

	/* Succeeded: the operation's value. */
	return result;
}

/*
 * Emits `op` of `value` and the float constant of `bits`, and returns its
 * result.
 */
static uint32_t
drv_gpu_spirv_math_float_op(
	struct drv_gpu_spirv_parser *parser,
	enum drv_gpu_shader_ir_op op,
	uint32_t value,
	uint32_t bits)
{
	uint32_t constant;
	uint32_t result;

	/* Names the constant, then applies the operation. */
	constant = drv_gpu_spirv_float_constant(parser, bits);
	result = drv_gpu_spirv_emit_value(parser, op, value, constant);

	/* Succeeded: the operation's value. */
	return result;
}

/*
 * Returns the float of `magnitude`, a value whose sign bit is clear, with
 * the sign bit of `sign_source` (copysign): the sign bit is moved as an
 * integer bit, so a negative zero keeps its sign.
 */
static uint32_t
drv_gpu_spirv_math_with_sign(
	struct drv_gpu_spirv_parser *parser,
	uint32_t magnitude,
	uint32_t sign_source)
{
	uint32_t sign;
	uint32_t signed_value;

	/* Takes the sign bit and sets it on the magnitude. */
	sign = drv_gpu_spirv_math_integer_op(parser,
					     DRV_GPU_IR_IAND,
					     sign_source,
					     MATH_SIGN_BITS);
	signed_value = drv_gpu_spirv_emit_value(parser,
						DRV_GPU_IR_IOR,
						magnitude,
						sign);

	/* Succeeded: the signed value. */
	return signed_value;
}

/*
 * Returns the Boolean of a float that is a NaN: its magnitude bits above an
 * infinity's (an exponent of all ones and a fraction that is not zero).  The
 * test is on the bits, so it holds however a code generator compares NaNs.
 */
static uint32_t
drv_gpu_spirv_math_is_nan(
	struct drv_gpu_spirv_parser *parser,
	uint32_t x)
{
	uint32_t magnitude;
	uint32_t infinity;
	uint32_t is_nan;

	/* Compares the magnitude with an infinity's, unsigned. */
	magnitude = drv_gpu_spirv_math_integer_op(parser,
						  DRV_GPU_IR_IAND,
						  x,
						  MATH_MAGNITUDE_BITS);
	infinity = drv_gpu_spirv_integer_constant(parser, MATH_INFINITY_BITS);
	is_nan = drv_gpu_spirv_emit_value(parser,
					  DRV_GPU_IR_ULT,
					  infinity,
					  magnitude);

	/* Succeeded: true for a NaN. */
	return is_nan;
}

/*
 * Returns the Boolean of a float that is an infinity of either sign: its
 * magnitude bits an infinity's.
 */
static uint32_t
drv_gpu_spirv_math_is_infinity(
	struct drv_gpu_spirv_parser *parser,
	uint32_t x)
{
	uint32_t magnitude;
	uint32_t is_infinity;

	/* Compares the magnitude with an infinity's. */
	magnitude = drv_gpu_spirv_math_integer_op(parser,
						  DRV_GPU_IR_IAND,
						  x,
						  MATH_MAGNITUDE_BITS);
	is_infinity = drv_gpu_spirv_math_integer_op(parser,
						    DRV_GPU_IR_IEQ,
						    magnitude,
						    MATH_INFINITY_BITS);

	/* Succeeded: true for an infinity. */
	return is_infinity;
}

/*
 * Evaluates the polynomial of `count` coefficients (binary32 bits, the
 * constant term first) at `x` by Horner's rule and returns its value.
 */
static uint32_t
drv_gpu_spirv_math_polynomial(
	struct drv_gpu_spirv_parser *parser,
	uint32_t x,
	const uint32_t *coefficients,
	uint32_t count)
{
	uint32_t sum;
	uint32_t product;
	uint32_t coefficient;
	uint32_t index;

	/* Starts from the highest coefficient. */
	sum = drv_gpu_spirv_float_constant(parser, coefficients[count - 1U]);

	/* Multiplies by x and adds the next lower coefficient, down to the constant. */
	for (index = count - 1U; index > 0U; index--) {
		product = drv_gpu_spirv_emit_value(parser,
						   DRV_GPU_IR_FMUL,
						   sum,
						   x);
		coefficient =
		    drv_gpu_spirv_float_constant(parser,
						 coefficients[index - 1U]);
		sum = drv_gpu_spirv_emit_value(parser,
					       DRV_GPU_IR_FADD,
					       product,
					       coefficient);
	}

	/* Succeeded: the polynomial's value. */
	return sum;
}

/*
 * Returns arctan(t) for 0 <= t <= 1: t times the polynomial in t^2 of
 * drv_gpu_spirv_atan_coefficients, whose error is at most 2e-8 there and
 * which keeps a small t's relative precision (the product starts with t).
 */
static uint32_t
drv_gpu_spirv_math_atan_unit(
	struct drv_gpu_spirv_parser *parser,
	uint32_t t)
{
	uint32_t square;
	uint32_t series;
	uint32_t angle;

	/* Evaluates the series in t^2 and scales it by t. */
	square = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FMUL, t, t);
	series = drv_gpu_spirv_math_polynomial(parser,
					       square,
					       drv_gpu_spirv_atan_coefficients,
					       MATH_ATAN_TERMS);
	angle = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FMUL, series, t);

	/* Succeeded: the angle in [0, pi / 4]. */
	return angle;
}

/*
 * Returns atan(x).  Past |x| = 1 the identity atan(|x|) = pi / 2 -
 * atan(1 / |x|) brings the argument into [0, 1]; the sign of x is put on the
 * angle last (atan is odd).  An infinity gives pi / 2 of its sign and a NaN a
 * NaN.
 */
static uint32_t
drv_gpu_spirv_math_atan(
	struct drv_gpu_spirv_parser *parser,
	uint32_t x)
{
	uint32_t magnitude;
	uint32_t one;
	uint32_t steep;
	uint32_t reciprocal;
	uint32_t t;
	uint32_t angle;
	uint32_t complement;
	uint32_t half_pi;
	uint32_t signed_angle;

	/* Takes |x|, or 1 / |x| when it is above 1. */
	magnitude = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FABS, x, 0U);
	one = drv_gpu_spirv_shared_constant(parser,
					    &parser->one_value,
					    DRV_GPU_IR_CONST,
					    FLOAT_ONE_BITS);
	steep = drv_gpu_spirv_emit_value(parser,
					 DRV_GPU_IR_FLT,
					 one,
					 magnitude);
	reciprocal = drv_gpu_spirv_emit_value(parser,
					      DRV_GPU_IR_RCP,
					      magnitude,
					      0U);
	t = drv_gpu_spirv_select_value(parser, steep, reciprocal, magnitude);

	/* The angle of t, taken from pi / 2 for a steep x. */
	angle = drv_gpu_spirv_math_atan_unit(parser, t);
	half_pi = drv_gpu_spirv_float_constant(parser, FLOAT_HALF_PI_BITS);
	complement = drv_gpu_spirv_emit_value(parser,
					      DRV_GPU_IR_FSUB,
					      half_pi,
					      angle);
	angle = drv_gpu_spirv_select_value(parser, steep, complement, angle);

	/* Gives the angle the sign of x. */
	signed_angle = drv_gpu_spirv_math_with_sign(parser, angle, x);

	/* Succeeded: the arc tangent. */
	return signed_angle;
}

/*
 * Returns atan2(y, x), the angle in [-pi, pi] of the point (x, y).
 *
 * The smaller of |x| and |y| divided by the larger is in [0, 1]; its arc
 * tangent is the angle from the nearer axis, taken from pi / 2 when |y| is
 * the larger, from pi when x is negative (its sign bit set, so x = -0 gives
 * pi as well), and given the sign of y last.  Both zero is undefined (GLSL).
 */
static uint32_t
drv_gpu_spirv_math_atan2(
	struct drv_gpu_spirv_parser *parser,
	uint32_t y,
	uint32_t x)
{
	uint32_t across;
	uint32_t up;
	uint32_t larger;
	uint32_t smaller;
	uint32_t reciprocal;
	uint32_t t;
	uint32_t angle;
	uint32_t steep;
	uint32_t complement;
	uint32_t half_pi;
	uint32_t pi;
	uint32_t zero;
	uint32_t backward;
	uint32_t signed_angle;

	/* The ratio of the smaller magnitude to the larger. */
	across = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FABS, x, 0U);
	up = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FABS, y, 0U);
	larger = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FMAX, across, up);
	smaller = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FMIN, across, up);
	reciprocal = drv_gpu_spirv_emit_value(parser,
					      DRV_GPU_IR_RCP,
					      larger,
					      0U);
	t = drv_gpu_spirv_emit_value(parser,
				     DRV_GPU_IR_FMUL,
				     smaller,
				     reciprocal);

	/* The angle from the x axis in the first quadrant. */
	angle = drv_gpu_spirv_math_atan_unit(parser, t);
	steep = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FLT, across, up);
	half_pi = drv_gpu_spirv_float_constant(parser, FLOAT_HALF_PI_BITS);
	complement = drv_gpu_spirv_emit_value(parser,
					      DRV_GPU_IR_FSUB,
					      half_pi,
					      angle);
	angle = drv_gpu_spirv_select_value(parser, steep, complement, angle);

	/* Mirrors it into the second quadrant where x is negative. */
	zero = drv_gpu_spirv_integer_constant(parser, 0U);
	backward = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_ILT, x, zero);
	pi = drv_gpu_spirv_float_constant(parser, FLOAT_PI_BITS);
	complement = drv_gpu_spirv_emit_value(parser,
					      DRV_GPU_IR_FSUB,
					      pi,
					      angle);
	angle = drv_gpu_spirv_select_value(parser, backward, complement, angle);

	/* Gives the angle the sign of y. */
	signed_angle = drv_gpu_spirv_math_with_sign(parser, angle, y);

	/* Succeeded: the angle of the point. */
	return signed_angle;
}

/*
 * Returns e^x as 2^(x log2(e)), the way the parser lowers Exp, and in
 * `negative` e^-x from the same product.
 */
static uint32_t
drv_gpu_spirv_math_exp_pair(
	struct drv_gpu_spirv_parser *parser,
	uint32_t x,
	uint32_t *negative)
{
	uint32_t power;
	uint32_t opposite;
	uint32_t exponential;

	/* Takes x to the base 2, then both powers. */
	power = drv_gpu_spirv_math_float_op(parser,
					    DRV_GPU_IR_FMUL,
					    x,
					    FLOAT_LOG2_E_BITS);
	exponential = drv_gpu_spirv_emit_value(parser,
					       DRV_GPU_IR_EXP2,
					       power,
					       0U);
	opposite = drv_gpu_spirv_emit_value(parser,
					    DRV_GPU_IR_FNEG,
					    power,
					    0U);
	*negative = drv_gpu_spirv_emit_value(parser,
					     DRV_GPU_IR_EXP2,
					     opposite,
					     0U);

	/* Succeeded: e^x, and e^-x through `negative`. */
	return exponential;
}

/*
 * Returns ln(x) as log2(x) ln(2), the way the parser lowers Log.
 */
static uint32_t
drv_gpu_spirv_math_log(
	struct drv_gpu_spirv_parser *parser,
	uint32_t x)
{
	uint32_t binary;
	uint32_t natural;

	/* Takes the base-2 logarithm and rescales it. */
	binary = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_LOG2, x, 0U);
	natural = drv_gpu_spirv_math_float_op(parser,
					      DRV_GPU_IR_FMUL,
					      binary,
					      FLOAT_LN_2_BITS);

	/* Succeeded: the natural logarithm. */
	return natural;
}

/*
 * Returns sinh(x), cosh(x) or tanh(x) by the equations the GLSL
 * specifications define them with: (e^x - e^-x) / 2, (e^x + e^-x) / 2 and
 * sinh(x) / cosh(x) = (e^2x - 1) / (e^2x + 1).  For tanh x is first held in
 * [-10, 10], past which the result rounds to +-1, so e^2x never overflows
 * into infinity / infinity.
 */
static uint32_t
drv_gpu_spirv_math_hyperbolic(
	struct drv_gpu_spirv_parser *parser,
	uint32_t function,
	uint32_t x)
{
	uint32_t exponential;
	uint32_t negative;
	uint32_t half;
	uint32_t sum;
	uint32_t held;
	uint32_t power;
	uint32_t one;
	uint32_t numerator;
	uint32_t denominator;
	uint32_t reciprocal;
	uint32_t value;

	/* tanh: (e^2x - 1) / (e^2x + 1) on the held argument. */
	if (function == GLSL_TANH) {
		held = drv_gpu_spirv_math_float_op(parser,
						   DRV_GPU_IR_FMAX,
						   x,
						   FLOAT_MINUS_TEN_BITS);
		held = drv_gpu_spirv_math_float_op(parser,
						   DRV_GPU_IR_FMIN,
						   held,
						   FLOAT_TEN_BITS);
		power = drv_gpu_spirv_math_float_op(parser,
						    DRV_GPU_IR_FMUL,
						    held,
						    FLOAT_TWO_LOG2_E_BITS);
		power = drv_gpu_spirv_emit_value(parser,
						 DRV_GPU_IR_EXP2,
						 power,
						 0U);
		one = drv_gpu_spirv_shared_constant(parser,
						    &parser->one_value,
						    DRV_GPU_IR_CONST,
						    FLOAT_ONE_BITS);
		numerator = drv_gpu_spirv_emit_value(parser,
						     DRV_GPU_IR_FSUB,
						     power,
						     one);
		denominator = drv_gpu_spirv_emit_value(parser,
						       DRV_GPU_IR_FADD,
						       power,
						       one);
		reciprocal = drv_gpu_spirv_emit_value(parser,
						      DRV_GPU_IR_RCP,
						      denominator,
						      0U);
		value = drv_gpu_spirv_emit_value(parser,
						 DRV_GPU_IR_FMUL,
						 numerator,
						 reciprocal);

		/* Succeeded: the hyperbolic tangent. */
		return value;
	}

	/* sinh and cosh: half the difference or the sum of e^x and e^-x. */
	exponential = drv_gpu_spirv_math_exp_pair(parser, x, &negative);
	if (function == GLSL_SINH) {
		sum = drv_gpu_spirv_emit_value(parser,
					       DRV_GPU_IR_FSUB,
					       exponential,
					       negative);
	} else {
		sum = drv_gpu_spirv_emit_value(parser,
					       DRV_GPU_IR_FADD,
					       exponential,
					       negative);
	}

	/* Halves it. */
	half = drv_gpu_spirv_float_constant(parser, FLOAT_HALF_BITS);
	value = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FMUL, sum, half);

	/* Succeeded: the hyperbolic sine or cosine. */
	return value;
}

/*
 * Returns asinh(x), acosh(x) or atanh(x) by the equations the GLSL
 * specifications define them with: ln(x + sqrt(x^2 + 1)), ln(x + sqrt(x^2 -
 * 1)) and ln((1 + x) / (1 - x)) / 2, x^2 - 1 taken as (x - 1)(x + 1).  asinh is odd, so it is taken of |x|
 * and given the sign of x, which keeps a negative x clear of the
 * cancellation in x + sqrt(x^2 + 1).
 */
static uint32_t
drv_gpu_spirv_math_inverse_hyperbolic(
	struct drv_gpu_spirv_parser *parser,
	uint32_t function,
	uint32_t x)
{
	uint32_t argument;
	uint32_t square;
	uint32_t below;
	uint32_t one;
	uint32_t root;
	uint32_t numerator;
	uint32_t denominator;
	uint32_t reciprocal;
	uint32_t half;
	uint32_t value;

	/* atanh: half the logarithm of (1 + x) / (1 - x). */
	one = drv_gpu_spirv_shared_constant(parser,
					    &parser->one_value,
					    DRV_GPU_IR_CONST,
					    FLOAT_ONE_BITS);
	if (function == GLSL_ATANH) {
		numerator = drv_gpu_spirv_emit_value(parser,
						     DRV_GPU_IR_FADD,
						     one,
						     x);
		denominator = drv_gpu_spirv_emit_value(parser,
						       DRV_GPU_IR_FSUB,
						       one,
						       x);
		reciprocal = drv_gpu_spirv_emit_value(parser,
						      DRV_GPU_IR_RCP,
						      denominator,
						      0U);
		argument = drv_gpu_spirv_emit_value(parser,
						    DRV_GPU_IR_FMUL,
						    numerator,
						    reciprocal);
		value = drv_gpu_spirv_math_log(parser, argument);
		half = drv_gpu_spirv_float_constant(parser, FLOAT_HALF_BITS);
		value = drv_gpu_spirv_emit_value(parser,
						 DRV_GPU_IR_FMUL,
						 value,
						 half);

		/* Succeeded: the inverse hyperbolic tangent. */
		return value;
	}

	/* asinh works on |x|; acosh on x itself. */
	argument = x;
	if (function == GLSL_ASINH) {
		argument = drv_gpu_spirv_emit_value(parser,
						    DRV_GPU_IR_FABS,
						    x,
						    0U);
	}

	/*
	 * The logarithm of the argument plus sqrt(argument^2 + 1), or of
	 * sqrt(argument^2 - 1) taken as (argument - 1)(argument + 1), whose
	 * factors keep their precision near 1 where argument^2 - 1 cancels.
	 */
	if (function == GLSL_ASINH) {
		square = drv_gpu_spirv_emit_value(parser,
						  DRV_GPU_IR_FMUL,
						  argument,
						  argument);
		root = drv_gpu_spirv_emit_value(parser,
						DRV_GPU_IR_FADD,
						square,
						one);
	} else {
		below = drv_gpu_spirv_emit_value(parser,
						 DRV_GPU_IR_FSUB,
						 argument,
						 one);
		square = drv_gpu_spirv_emit_value(parser,
						  DRV_GPU_IR_FADD,
						  argument,
						  one);
		root = drv_gpu_spirv_emit_value(parser,
						DRV_GPU_IR_FMUL,
						below,
						square);
	}
	root = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_SQRT, root, 0U);
	value = drv_gpu_spirv_emit_value(parser,
					 DRV_GPU_IR_FADD,
					 argument,
					 root);
	value = drv_gpu_spirv_math_log(parser, value);

	/* acosh is done; asinh takes the sign of x. */
	if (function == GLSL_ACOSH)
		return value;
	value = drv_gpu_spirv_math_with_sign(parser, value, x);

	/* Succeeded: the inverse hyperbolic sine. */
	return value;
}

/*
 * Returns x times 2 to the integer `exponent`.
 *
 * The exponent is held in [-252, 254] and split into two halves, each in
 * [-126, 127], whose powers of two are built as floats from their exponent
 * fields; x is multiplied by one and then the other.  Both halves have the
 * exponent's sign, so the first product lies between x and the result and
 * overflows or underflows only where the result does.
 */
static uint32_t
drv_gpu_spirv_math_ldexp(
	struct drv_gpu_spirv_parser *parser,
	uint32_t x,
	uint32_t exponent)
{
	uint32_t held;
	uint32_t first;
	uint32_t second;
	uint32_t power;
	uint32_t value;

	/* Holds the exponent in the range two normal powers reach. */
	held = drv_gpu_spirv_math_signed_clamp(parser,
					       exponent,
					       MATH_LDEXP_LOW,
					       MATH_LDEXP_HIGH);

	/* Splits it: floor(n / 2) and the rest. */
	first = drv_gpu_spirv_math_integer_op(parser,
					      DRV_GPU_IR_ASR,
					      held,
					      1U);
	second = drv_gpu_spirv_emit_value(parser,
					  DRV_GPU_IR_ISUB,
					  held,
					  first);

	/* Multiplies by 2^first, built from its biased exponent field. */
	power = drv_gpu_spirv_math_integer_op(parser,
					      DRV_GPU_IR_IADD,
					      first,
					      MATH_EXPONENT_BIAS);
	power = drv_gpu_spirv_math_integer_op(parser,
					      DRV_GPU_IR_SHL,
					      power,
					      MATH_FRACTION_SHIFT);
	value = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FMUL, x, power);

	/* Then by 2^second. */
	power = drv_gpu_spirv_math_integer_op(parser,
					      DRV_GPU_IR_IADD,
					      second,
					      MATH_EXPONENT_BIAS);
	power = drv_gpu_spirv_math_integer_op(parser,
					      DRV_GPU_IR_SHL,
					      power,
					      MATH_FRACTION_SHIFT);
	value = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FMUL, value, power);

	/* Succeeded: x scaled by the power of two. */
	return value;
}

/*
 * Returns the signed integer `value` held in [low, high] (the bounds given
 * as their 32-bit patterns).
 */
static uint32_t
drv_gpu_spirv_math_signed_clamp(
	struct drv_gpu_spirv_parser *parser,
	uint32_t value,
	uint32_t low,
	uint32_t high)
{
	uint32_t bound;
	uint32_t below;
	uint32_t above;
	uint32_t held;

	/* Raises a value below the low bound to it. */
	bound = drv_gpu_spirv_integer_constant(parser, low);
	below = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_ILT, value, bound);
	held = drv_gpu_spirv_select_value(parser, below, bound, value);

	/* Lowers one above the high bound to it. */
	bound = drv_gpu_spirv_integer_constant(parser, high);
	above = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_ILT, bound, held);
	held = drv_gpu_spirv_select_value(parser, above, bound, held);

	/* Succeeded: the held value. */
	return held;
}

/*
 * Returns NMin(x, y) or NMax(x, y): the smaller or the larger, where a NaN
 * operand gives the other operand (and two NaNs a NaN).
 */
static uint32_t
drv_gpu_spirv_math_nan_min_max(
	struct drv_gpu_spirv_parser *parser,
	uint32_t function,
	uint32_t x,
	uint32_t y)
{
	uint32_t x_nan;
	uint32_t y_nan;
	uint32_t ordered;
	uint32_t value;

	/* The plain minimum or maximum. */
	if (function == GLSL_NMIN) {
		ordered = drv_gpu_spirv_emit_value(parser,
						   DRV_GPU_IR_FMIN,
						   x,
						   y);
	} else {
		ordered = drv_gpu_spirv_emit_value(parser,
						   DRV_GPU_IR_FMAX,
						   x,
						   y);
	}

	/* x where y is a NaN, y where x is one. */
	y_nan = drv_gpu_spirv_math_is_nan(parser, y);
	value = drv_gpu_spirv_select_value(parser, y_nan, x, ordered);
	x_nan = drv_gpu_spirv_math_is_nan(parser, x);
	value = drv_gpu_spirv_select_value(parser, x_nan, y, value);

	/* Succeeded: the NaN-aware minimum or maximum. */
	return value;
}

/*
 * Returns the number of the lowest set bit of x, or -1 when x is zero.
 *
 * x & -x keeps only the lowest set bit (two's complement); that power of two
 * converts to a float exactly, and the float's exponent field less the bias
 * is its bit number.
 */
static uint32_t
drv_gpu_spirv_math_find_lsb(
	struct drv_gpu_spirv_parser *parser,
	uint32_t x)
{
	uint32_t negated;
	uint32_t lowest;
	uint32_t number;
	uint32_t none;
	uint32_t is_zero;
	uint32_t value;

	/* Isolates the lowest set bit. */
	negated = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_INEG, x, 0U);
	lowest = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_IAND, x, negated);

	/* Its number, from the exponent of its float. */
	number = drv_gpu_spirv_math_power_number(parser, lowest);

	/* Gives -1 for a zero x. */
	none = drv_gpu_spirv_integer_constant(parser, 0xFFFFFFFFU);
	is_zero = drv_gpu_spirv_math_integer_op(parser,
						DRV_GPU_IR_IEQ,
						x,
						0U);
	value = drv_gpu_spirv_select_value(parser, is_zero, none, number);

	/* Succeeded: the bit number. */
	return value;
}

/*
 * Returns the number of the highest set bit of x (unsigned), or -1 when x is
 * zero.
 *
 * Or-ing x with itself shifted right by 1, 2, 4, 8 and 16 sets every bit
 * below the highest set one; that run less itself shifted right by one
 * keeps only the highest bit, a power of two whose float's exponent is its
 * number.  (Converting x itself would round up to the next power of two
 * when its top 25 bits are all set.)
 *
 * Procedure referenced: H. S. Warren, Jr., Hacker's Delight, 2nd ed.
 * (2013), section 3-1 (propagating the leftmost one-bit rightward).
 */
static uint32_t
drv_gpu_spirv_math_find_msb(
	struct drv_gpu_spirv_parser *parser,
	uint32_t x)
{
	uint32_t run;
	uint32_t shifted;
	uint32_t highest;
	uint32_t number;
	uint32_t none;
	uint32_t is_zero;
	uint32_t value;
	uint32_t distance;

	/* Sets every bit below the highest set one. */
	run = x;
	for (distance = 1U; distance <= 16U; distance *= 2U) {
		shifted = drv_gpu_spirv_math_integer_op(parser,
							DRV_GPU_IR_SHR,
							run,
							distance);
		run = drv_gpu_spirv_emit_value(parser,
					       DRV_GPU_IR_IOR,
					       run,
					       shifted);
	}

	/* Keeps the highest bit alone. */
	shifted = drv_gpu_spirv_math_integer_op(parser,
						DRV_GPU_IR_SHR,
						run,
						1U);
	highest = drv_gpu_spirv_emit_value(parser,
					   DRV_GPU_IR_IXOR,
					   run,
					   shifted);

	/* Its number, from the exponent of its float. */
	number = drv_gpu_spirv_math_power_number(parser, highest);

	/* Gives -1 for a zero x. */
	none = drv_gpu_spirv_integer_constant(parser, 0xFFFFFFFFU);
	is_zero = drv_gpu_spirv_math_integer_op(parser,
						DRV_GPU_IR_IEQ,
						x,
						0U);
	value = drv_gpu_spirv_select_value(parser, is_zero, none, number);

	/* Succeeded: the bit number. */
	return value;
}

/*
 * Returns n for the unsigned power of two 2^n: the exponent field of its
 * float (exact, being a power of two) less the bias.
 */
static uint32_t
drv_gpu_spirv_math_power_number(
	struct drv_gpu_spirv_parser *parser,
	uint32_t power)
{
	uint32_t converted;
	uint32_t field;
	uint32_t number;

	/* Converts the power and reads its exponent field. */
	converted = drv_gpu_spirv_emit_value(parser,
					     DRV_GPU_IR_U2F,
					     power,
					     0U);
	field = drv_gpu_spirv_math_integer_op(parser,
					      DRV_GPU_IR_SHR,
					      converted,
					      MATH_FRACTION_SHIFT);
	number = drv_gpu_spirv_math_integer_op(parser,
					       DRV_GPU_IR_ISUB,
					       field,
					       MATH_EXPONENT_BIAS);

	/* Succeeded: the bit number. */
	return number;
}

/*
 * Splits the float x into a significand in [0.5, 1) of x's sign and an
 * integer exponent with x = significand * 2^exponent (Frexp); a zero gives
 * itself and 0.  A subnormal x is first multiplied by 2^64 to make it
 * normal; a code generator that flushes subnormals gives a zero there, as
 * it would for x itself.  An infinity or a NaN gives something undefined
 * (GLSL).
 */
static void
drv_gpu_spirv_math_frexp(
	struct drv_gpu_spirv_parser *parser,
	uint32_t x,
	uint32_t *significand,
	uint32_t *exponent)
{
	uint32_t field;
	uint32_t small;
	uint32_t scaled;
	uint32_t normal;
	uint32_t bias;
	uint32_t subnormal_bias;
	uint32_t is_zero;
	uint32_t zero;
	uint32_t fraction;
	uint32_t value;

	/* Makes a subnormal x normal (its exponent field is zero). */
	field = drv_gpu_spirv_math_integer_op(parser,
					      DRV_GPU_IR_SHR,
					      x,
					      MATH_FRACTION_SHIFT);
	field = drv_gpu_spirv_math_integer_op(parser,
					      DRV_GPU_IR_IAND,
					      field,
					      MATH_EXPONENT_FIELD);
	small = drv_gpu_spirv_math_integer_op(parser,
					      DRV_GPU_IR_IEQ,
					      field,
					      0U);
	scaled = drv_gpu_spirv_math_float_op(parser,
					     DRV_GPU_IR_FMUL,
					     x,
					     FLOAT_TWO_TO_64_BITS);
	normal = drv_gpu_spirv_select_value(parser, small, scaled, x);

	/*
	 * The exponent: the normal value's field less 126 (a significand in
	 * [0.5, 1) is one power below the field's), and less 64 more for a
	 * scaled one.
	 */
	field = drv_gpu_spirv_math_integer_op(parser,
					      DRV_GPU_IR_SHR,
					      normal,
					      MATH_FRACTION_SHIFT);
	field = drv_gpu_spirv_math_integer_op(parser,
					      DRV_GPU_IR_IAND,
					      field,
					      MATH_EXPONENT_FIELD);
	bias = drv_gpu_spirv_integer_constant(parser,
					      MATH_EXPONENT_BIAS - 1U);
	subnormal_bias =
	    drv_gpu_spirv_integer_constant(parser,
					   MATH_EXPONENT_BIAS - 1U +
					   MATH_SUBNORMAL_SCALE);
	bias = drv_gpu_spirv_select_value(parser, small, subnormal_bias, bias);
	value = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_ISUB, field, bias);

	/* The significand: the sign and fraction under the field of 0.5. */
	fraction = drv_gpu_spirv_math_integer_op(parser,
						 DRV_GPU_IR_IAND,
						 normal,
						 MATH_SIGN_AND_FRACTION_BITS);
	fraction = drv_gpu_spirv_math_integer_op(parser,
						 DRV_GPU_IR_IOR,
						 fraction,
						 MATH_HALF_EXPONENT_BITS);

	/* A zero (still zero after the scaling) gives itself and 0. */
	is_zero = drv_gpu_spirv_math_integer_op(parser,
						DRV_GPU_IR_IEQ,
						field,
						0U);
	zero = drv_gpu_spirv_integer_constant(parser, 0U);
	*significand = drv_gpu_spirv_select_value(parser,
						  is_zero,
						  normal,
						  fraction);
	*exponent = drv_gpu_spirv_select_value(parser, is_zero, zero, value);

	/* Succeeded: both parts are named. */
	return;
}

/*
 * Lowers Modf, ModfStruct, Frexp and FrexpStruct: x split into a fractional
 * and a whole part (both of x's sign; the whole part trunc(x)), or into a
 * significand and an exponent.  The struct forms' result is the two parts as
 * its two members; the others' result is the first part, and the second is
 * stored through the pointer operand, which must address a local.
 */
static int
drv_gpu_spirv_lower_split(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct drv_gpu_spirv_id *record;
	struct drv_gpu_spirv_id *pointer;
	struct drv_gpu_spirv_id *variable;
	uint32_t operand[4];
	uint32_t first[4];
	uint32_t second[4];
	uint32_t operand_count;
	uint32_t components;
	uint32_t pointee;
	uint32_t function;
	uint32_t index;
	int structured;
	int error;

	/* The struct forms take the operand alone, the others a pointer too. */
	function = word[4];
	structured = 0;
	if (function == GLSL_MODF_STRUCT || function == GLSL_FREXP_STRUCT)
		structured = 1;
	if (structured != 0 && count != 6U)
		return EINVAL;
	if (structured == 0 && count != 7U)
		return EINVAL;

	/* Resolves the operand, a float scalar or vector. */
	operand_count = drv_gpu_spirv_operand(parser, word[5], operand);
	components = drv_gpu_spirv_operand_float_components(parser, word[5]);
	if (operand_count == 0U || components != operand_count) {
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "extended instruction operand");
		return error;
	}

	/* Splits each component. */
	for (index = 0U; index < components; index++) {
		if (function == GLSL_MODF || function == GLSL_MODF_STRUCT) {
			second[index] =
			    drv_gpu_spirv_emit_value(parser,
						     DRV_GPU_IR_FTRUNC,
						     operand[index],
						     0U);
			first[index] =
			    drv_gpu_spirv_emit_value(parser,
						     DRV_GPU_IR_FSUB,
						     operand[index],
						     second[index]);
		} else {
			drv_gpu_spirv_math_frexp(parser,
						 operand[index],
						 &first[index],
						 &second[index]);
		}
	}

	/* A struct result names both parts, the first part's members first. */
	if (structured != 0) {
		/* The result must be the two parts' scalars. */
		pointee = drv_gpu_spirv_aggregate_scalars(parser, word[1]);
		if (pointee != 2U * components) {
			error = drv_gpu_spirv_refuse(
			    parser,
			    opcode,
			    offset,
			    "extended instruction result of another shape");
			return error;
		}

		/* Declares the result as the parts' scalars. */
		record = drv_gpu_spirv_result(parser,
					      word[2],
					      word[1],
					      2U * components,
					      0);
		if (record == NULL)
			return EINVAL;
		for (index = 0U; index < components; index++) {
			record->comp[index] = first[index];
			record->comp[components + index] = second[index];
		}

		/* Reports a failure an emitter latched. */
		if (parser->error != 0)
			return parser->error;

		/* Succeeded: both parts are the result's. */
		return 0;
	}

	/* The pointer form stores the second part into a local. */
	pointer = drv_gpu_spirv_id(parser, word[6]);
	if (pointer == NULL)
		return EINVAL;
	variable = NULL;
	if (pointer->kind == ID_VARIABLE || pointer->kind == ID_POINTER)
		variable = drv_gpu_spirv_id(parser, pointer->var);
	pointee = 0U;
	if (variable != NULL)
		pointee = drv_gpu_spirv_aggregate_scalars(parser,
							  pointer->pointee);

	/* Refuses a pointer that is not a local of the part's size. */
	if (variable == NULL ||
	    pointer->ptr_kind != PTR_LOCAL ||
	    pointee != components) {
		error = drv_gpu_spirv_refuse(
		    parser,
		    opcode,
		    offset,
		    "Modf or Frexp storing through something that is not a "
		    "local of the operand's size");
		return error;
	}

	/* Stores the second part. */
	error = drv_gpu_spirv_lower_store_local(parser,
						pointer,
						variable,
						second,
						components);
	if (error != 0)
		return error;

	/* Declares the result as the first part. */
	record = drv_gpu_spirv_result(parser, word[2], word[1], components, 0);
	if (record == NULL)
		return EINVAL;
	for (index = 0U; index < components; index++)
		record->comp[index] = first[index];

	/* Reports a failure an emitter latched. */
	if (parser->error != 0)
		return parser->error;

	/* Succeeded: the first part is the result, the second is stored. */
	return 0;
}

/*
 * Lowers the normalized packings, by the equations of the GLSL
 * specifications: packUnorm is round(clamp(c, 0, 1) * (2^n - 1)) and
 * packSnorm round(clamp(c, -1, 1) * (2^(n-1) - 1)) into n-bit fields, the
 * first component lowest; unpackUnorm is f / (2^n - 1) and unpackSnorm
 * clamp(f / (2^(n-1) - 1), -1, 1) of each field.  The rounding of a half
 * is to the even code (GLSL leaves its direction to the implementation).
 */
static int
drv_gpu_spirv_lower_pack(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct drv_gpu_spirv_id *record;
	uint32_t operand[4];
	uint32_t operand_count;
	uint32_t function;
	uint32_t fields;
	uint32_t result_count;
	uint32_t width;
	uint32_t index;
	int packing;
	int error;

	/* The instruction must carry the one operand. */
	if (count != 6U)
		return EINVAL;

	/* The direction, and how many fields one integer holds. */
	function = word[4];
	packing = 0;
	if (function == GLSL_PACK_SNORM_4X8 ||
	    function == GLSL_PACK_UNORM_4X8 ||
	    function == GLSL_PACK_SNORM_2X16 ||
	    function == GLSL_PACK_UNORM_2X16)
		packing = 1;
	fields = 2U;
	if (function == GLSL_PACK_SNORM_4X8 ||
	    function == GLSL_PACK_UNORM_4X8 ||
	    function == GLSL_UNPACK_SNORM_4X8 ||
	    function == GLSL_UNPACK_UNORM_4X8)
		fields = 4U;

	/*
	 * Packing takes a float vector of the fields and makes one 32-bit
	 * integer; unpacking the reverse.
	 */
	operand_count = drv_gpu_spirv_operand(parser, word[5], operand);
	if (packing != 0) {
		result_count = drv_gpu_spirv_int_components(parser, word[1]);
		width = drv_gpu_spirv_int_width(parser, word[1]);
		if (width != 32U)
			result_count = 0U;
		if (operand_count != fields || result_count != 1U)
			result_count = 0U;
	} else {
		result_count = drv_gpu_spirv_float_components(parser, word[1]);
		if (operand_count != 1U || result_count != fields)
			result_count = 0U;
	}

	/* Refuses another shape. */
	if (result_count == 0U) {
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "normalized packing of another "
					     "shape");
		return error;
	}

	/* Declares the result; its scalars are named below. */
	record = drv_gpu_spirv_result(parser,
				      word[2],
				      word[1],
				      result_count,
				      0);
	if (record == NULL)
		return EINVAL;

	/* Packs the fields into one integer, or unpacks each field. */
	if (packing != 0) {
		record->comp[0] = drv_gpu_spirv_math_pack_norm(parser,
							       function,
							       operand);
	} else {
		for (index = 0U; index < fields; index++) {
			record->comp[index] =
			    drv_gpu_spirv_math_unpack_norm(parser,
							   function,
							   operand[0],
							   index);
		}
	}

	/* Reports a failure an emitter latched. */
	if (parser->error != 0)
		return parser->error;

	/* Succeeded: the packing is lowered. */
	return 0;
}

/*
 * Returns the integer packing the normalized fields of `operand` for a
 * packing function (two 16-bit or four 8-bit fields).
 */
static uint32_t
drv_gpu_spirv_math_pack_norm(
	struct drv_gpu_spirv_parser *parser,
	uint32_t function,
	const uint32_t *operand)
{
	uint32_t low_bits;
	uint32_t high_bits;
	uint32_t scale_bits;
	uint32_t mask;
	uint32_t width;
	uint32_t fields;
	uint32_t held;
	uint32_t code;
	uint32_t packed;
	uint32_t index;
	int is_signed;

	/* The clamp, the scale and the field of the function. */
	is_signed = 0;
	low_bits = FLOAT_ZERO_BITS;
	high_bits = FLOAT_ONE_BITS;
	if (function == GLSL_PACK_SNORM_4X8 ||
	    function == GLSL_PACK_SNORM_2X16) {
		is_signed = 1;
		low_bits = FLOAT_MINUS_ONE_BITS;
	}
	if (function == GLSL_PACK_UNORM_2X16) {
		scale_bits = FLOAT_65535_BITS;
	} else if (function == GLSL_PACK_SNORM_2X16) {
		scale_bits = FLOAT_32767_BITS;
	} else if (function == GLSL_PACK_UNORM_4X8) {
		scale_bits = FLOAT_255_BITS;
	} else {
		scale_bits = FLOAT_127_BITS;
	}
	fields = 4U;
	width = 8U;
	mask = MATH_MASK_8;
	if (function == GLSL_PACK_UNORM_2X16 ||
	    function == GLSL_PACK_SNORM_2X16) {
		fields = 2U;
		width = 16U;
		mask = MATH_MASK_16;
	}

	/* Converts each component to its code and places it in its field. */
	packed = NO_VALUE;
	for (index = 0U; index < fields; index++) {
		held = drv_gpu_spirv_math_float_op(parser,
						   DRV_GPU_IR_FMAX,
						   operand[index],
						   low_bits);
		held = drv_gpu_spirv_math_float_op(parser,
						   DRV_GPU_IR_FMIN,
						   held,
						   high_bits);
		held = drv_gpu_spirv_math_float_op(parser,
						   DRV_GPU_IR_FMUL,
						   held,
						   scale_bits);
		held = drv_gpu_spirv_emit_value(parser,
						DRV_GPU_IR_FROUND_EVEN,
						held,
						0U);
		if (is_signed != 0) {
			code = drv_gpu_spirv_emit_value(parser,
							DRV_GPU_IR_F2I,
							held,
							0U);
		} else {
			code = drv_gpu_spirv_emit_value(parser,
							DRV_GPU_IR_F2U,
							held,
							0U);
		}

		/* Masks the code to its field (a signed one is two's complement). */
		code = drv_gpu_spirv_math_integer_op(parser,
						     DRV_GPU_IR_IAND,
						     code,
						     mask);
		if (index > 0U) {
			code = drv_gpu_spirv_math_integer_op(parser,
							     DRV_GPU_IR_SHL,
							     code,
							     index * width);
			packed = drv_gpu_spirv_emit_value(parser,
							  DRV_GPU_IR_IOR,
							  packed,
							  code);
		} else {
			packed = code;
		}
	}

	/* Succeeded: the packed integer. */
	return packed;
}

/*
 * Returns the float of field `index` of the packed integer for an unpacking
 * function.
 *
 * A code is divided by 2^n - 1 as a product with the first terms of the
 * series 2^-n (1 + 2^-n + ...) (see FLOAT_INVERSE_65535_BITS), which is
 * within half an ULP and a 2^-32 part of the quotient and gives exactly 1 for
 * the largest code; a signed result is held in [-1, 1], so -2^(n-1) gives -1.
 */
static uint32_t
drv_gpu_spirv_math_unpack_norm(
	struct drv_gpu_spirv_parser *parser,
	uint32_t function,
	uint32_t packed,
	uint32_t index)
{
	uint32_t width;
	uint32_t code;
	uint32_t value;
	uint32_t number;

	/* The width of the function's fields. */
	width = 8U;
	if (function == GLSL_UNPACK_UNORM_2X16 ||
	    function == GLSL_UNPACK_SNORM_2X16)
		width = 16U;

	/*
	 * Moves the field to the top of the word, so a right shift brings it
	 * down sign-extended (signed) or zero-extended (unsigned).
	 */
	code = drv_gpu_spirv_math_integer_op(parser,
					     DRV_GPU_IR_SHL,
					     packed,
					     32U - width - index * width);

	/* An unsigned code: an 8-bit one is scaled to the 16-bit range first. */
	if (function == GLSL_UNPACK_UNORM_2X16 ||
	    function == GLSL_UNPACK_UNORM_4X8) {
		code = drv_gpu_spirv_math_integer_op(parser,
						     DRV_GPU_IR_SHR,
						     code,
						     32U - width);
		if (width == 8U) {
			code = drv_gpu_spirv_math_integer_op(
			    parser,
			    DRV_GPU_IR_IMUL,
			    code,
			    MATH_UNORM_8_TO_16);
		}
		number = drv_gpu_spirv_emit_value(parser,
						  DRV_GPU_IR_U2F,
						  code,
						  0U);
		value = drv_gpu_spirv_math_float_op(parser,
						    DRV_GPU_IR_FMUL,
						    number,
						    FLOAT_INVERSE_65535_BITS);

		/* Succeeded: the unsigned normalized value. */
		return value;
	}

	/* A signed code: divided, then held at -1. */
	code = drv_gpu_spirv_math_integer_op(parser,
					     DRV_GPU_IR_ASR,
					     code,
					     32U - width);
	number = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_I2F, code, 0U);
	if (width == 16U) {
		value = drv_gpu_spirv_math_float_op(parser,
						    DRV_GPU_IR_FMUL,
						    number,
						    FLOAT_INVERSE_32767_BITS);
	} else {
		value = drv_gpu_spirv_math_float_op(parser,
						    DRV_GPU_IR_FMUL,
						    number,
						    FLOAT_INVERSE_127_BITS);
	}
	value = drv_gpu_spirv_math_float_op(parser,
					    DRV_GPU_IR_FMAX,
					    value,
					    FLOAT_MINUS_ONE_BITS);

	/* Succeeded: the signed normalized value. */
	return value;
}

/*
 * Lowers FaceForward and Refract (the GLSL specification):
 * faceforward(N, I, Nref) is N where dot(Nref, I) < 0 and -N elsewhere;
 * refract(I, N, eta) is 0 where k = 1 - eta^2 (1 - dot(N, I)^2) is below
 * zero and eta I - (eta dot(N, I) + sqrt(k)) N elsewhere, eta a scalar.
 */
static int
drv_gpu_spirv_lower_facing(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct drv_gpu_spirv_id *record;
	uint32_t vector[3][4];
	uint32_t vector_count[3];
	uint32_t components;
	uint32_t vectors;
	uint32_t index;
	uint32_t dot;
	uint32_t zero;
	uint32_t one;
	uint32_t below;
	uint32_t negated;
	uint32_t eta;
	uint32_t term;
	uint32_t k;
	uint32_t root;
	uint32_t scale;
	uint32_t value;
	int error;

	/* The instruction must carry three operands. */
	if (count != 8U)
		return EINVAL;

	/*
	 * The result and the vector operands are float scalars or vectors of
	 * one size; Refract's third operand is a float scalar.
	 */
	components = drv_gpu_spirv_float_components(parser, word[1]);
	vectors = 3U;
	if (word[4] == GLSL_REFRACT)
		vectors = 2U;

	/* Resolves the three operands; each may emit a constant. */
	for (index = 0U; index < 3U; index++) {
		vector_count[index] = drv_gpu_spirv_operand(parser,
							    word[5U + index],
							    vector[index]);
	}

	/* Each vector operand must have the result's size. */
	for (index = 0U; index < vectors; index++) {
		if (vector_count[index] != components)
			components = 0U;
	}

	/* Refract's eta must be one float. */
	if (vectors == 2U && vector_count[2] != 1U)
		components = 0U;

	/* Refuses another shape. */
	if (components == 0U) {
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "extended instruction operand");
		return error;
	}

	/* Declares the result; its scalars are named below. */
	record = drv_gpu_spirv_result(parser, word[2], word[1], components, 0);
	if (record == NULL)
		return EINVAL;

	/* FaceForward: N or -N by the sign of dot(Nref, I). */
	zero = drv_gpu_spirv_shared_constant(parser,
					     &parser->zero_value,
					     DRV_GPU_IR_CONST,
					     FLOAT_ZERO_BITS);
	if (word[4] == GLSL_FACE_FORWARD) {
		dot = drv_gpu_spirv_dot_value(parser,
					      vector[2],
					      vector[1],
					      components);
		below = drv_gpu_spirv_emit_value(parser,
						 DRV_GPU_IR_FLT,
						 dot,
						 zero);
		for (index = 0U; index < components; index++) {
			negated = drv_gpu_spirv_emit_value(parser,
							   DRV_GPU_IR_FNEG,
							   vector[0][index],
							   0U);
			record->comp[index] =
			    drv_gpu_spirv_select_value(parser,
						       below,
						       vector[0][index],
						       negated);
		}

		/* Reports a failure an emitter latched. */
		if (parser->error != 0)
			return parser->error;

		/* Succeeded: the vector faces forward. */
		return 0;
	}

	/* Refract: k = 1 - eta^2 (1 - dot(N, I)^2). */
	eta = vector[2][0];
	one = drv_gpu_spirv_shared_constant(parser,
					    &parser->one_value,
					    DRV_GPU_IR_CONST,
					    FLOAT_ONE_BITS);
	dot = drv_gpu_spirv_dot_value(parser, vector[1], vector[0], components);
	term = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FMUL, dot, dot);
	term = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FSUB, one, term);
	k = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FMUL, eta, eta);
	k = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FMUL, k, term);
	k = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FSUB, one, k);

	/* The scale of N: eta dot(N, I) + sqrt(k). */
	root = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_SQRT, k, 0U);
	scale = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FMUL, eta, dot);
	scale = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FADD, scale, root);

	/* Each component, zero where the ray is reflected whole (k < 0). */
	below = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_FLT, k, zero);
	for (index = 0U; index < components; index++) {
		value = drv_gpu_spirv_emit_value(parser,
						 DRV_GPU_IR_FMUL,
						 eta,
						 vector[0][index]);
		term = drv_gpu_spirv_emit_value(parser,
						DRV_GPU_IR_FMUL,
						scale,
						vector[1][index]);
		value = drv_gpu_spirv_emit_value(parser,
						 DRV_GPU_IR_FSUB,
						 value,
						 term);
		record->comp[index] = drv_gpu_spirv_select_value(parser,
								 below,
								 zero,
								 value);
	}

	/* Reports a failure an emitter latched. */
	if (parser->error != 0)
		return parser->error;

	/* Succeeded: the refracted vector. */
	return 0;
}

/*
 * Lowers OpIsNan and OpIsInf per component: the Boolean of a float that is a
 * NaN, or an infinity, read from its bits.
 */
static int
drv_gpu_spirv_lower_float_class(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct drv_gpu_spirv_id *record;
	uint32_t operand[4];
	uint32_t operand_count;
	uint32_t components;
	uint32_t floats;
	uint32_t index;
	int error;

	/* The instruction must carry its operand. */
	if (count != 4U)
		return EINVAL;

	/* The result is a Boolean of the float operand's size. */
	operand_count = drv_gpu_spirv_operand(parser, word[3], operand);
	components = drv_gpu_spirv_bool_components(parser, word[1]);
	floats = drv_gpu_spirv_operand_float_components(parser, word[3]);
	if (components == 0U ||
	    operand_count != components ||
	    floats != components) {
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "float class test of something that "
					     "is not a float of the result's size");
		return error;
	}

	/* Declares the result; its scalars are named below. */
	record = drv_gpu_spirv_result(parser, word[2], word[1], components, 0);
	if (record == NULL)
		return EINVAL;

	/* Tests each component. */
	for (index = 0U; index < components; index++) {
		if (opcode == OP_IS_NAN) {
			record->comp[index] =
			    drv_gpu_spirv_math_is_nan(parser, operand[index]);
		} else {
			record->comp[index] =
			    drv_gpu_spirv_math_is_infinity(parser,
							   operand[index]);
		}
	}

	/* Reports a failure an emitter latched. */
	if (parser->error != 0)
		return parser->error;

	/* Succeeded: the class tests are lowered. */
	return 0;
}

/*
 * Returns the mask of the low `count` bits for a count in [0, 32]: (1 <<
 * count) - 1, and all ones for 32, which a shift taken modulo 32 cannot
 * make.
 */
static uint32_t
drv_gpu_spirv_math_low_mask(
	struct drv_gpu_spirv_parser *parser,
	uint32_t count)
{
	uint32_t one;
	uint32_t power;
	uint32_t mask;
	uint32_t whole;
	uint32_t all;

	/* (1 << count) - 1. */
	one = drv_gpu_spirv_integer_constant(parser, 1U);
	power = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_SHL, one, count);
	mask = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_ISUB, power, one);

	/* All ones for a count of 32 or more. */
	whole = drv_gpu_spirv_math_integer_op(parser,
					      DRV_GPU_IR_UGE,
					      count,
					      32U);
	all = drv_gpu_spirv_integer_constant(parser, 0xFFFFFFFFU);
	mask = drv_gpu_spirv_select_value(parser, whole, all, mask);

	/* Succeeded: the mask. */
	return mask;
}

/*
 * Lowers OpBitFieldInsert, OpBitFieldSExtract and OpBitFieldUExtract per
 * component, the Offset and Count scalars shared by every component.
 *
 * Insert replaces the Count bits of Base from bit Offset on with the low
 * bits of Insert; UExtract is Base shifted right by Offset under a mask of
 * Count bits; SExtract shifts the field to the top of the word and back down
 * arithmetically, so its top bit is spread, and gives 0 for a Count of 0.  An
 * Offset plus Count past 32 is undefined (SPIR-V).
 */
static int
drv_gpu_spirv_lower_bit_field(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct drv_gpu_spirv_id *record;
	uint32_t base[4];
	uint32_t insert[4];
	uint32_t place[4];
	uint32_t length[4];
	uint32_t base_count;
	uint32_t insert_count;
	uint32_t scalar_count;
	uint32_t width;
	uint32_t components;
	uint32_t first;
	uint32_t index;
	uint32_t mask;
	uint32_t field;
	uint32_t kept;
	uint32_t up;
	uint32_t down;
	uint32_t thirty_two;
	uint32_t empty;
	uint32_t zero;
	uint32_t value;
	int error;

	/* Insert carries Base, Insert, Offset and Count; the extracts no Insert. */
	first = 4U;
	if (opcode == OP_BIT_FIELD_INSERT)
		first = 5U;
	if (count != first + 2U)
		return EINVAL;

	/* Base (and Insert) are 32-bit integers of the result's size. */
	components = drv_gpu_spirv_int_components(parser, word[1]);
	width = drv_gpu_spirv_int_width(parser, word[1]);
	if (width != 32U)
		components = 0U;
	base_count = drv_gpu_spirv_operand(parser, word[3], base);
	insert_count = components;
	if (opcode == OP_BIT_FIELD_INSERT)
		insert_count = drv_gpu_spirv_operand(parser, word[4], insert);

	/* Offset and Count are integer scalars. */
	scalar_count = drv_gpu_spirv_operand(parser, word[first], place);
	if (scalar_count != 1U)
		components = 0U;
	scalar_count = drv_gpu_spirv_operand(parser, word[first + 1U], length);
	if (scalar_count != 1U)
		components = 0U;

	/* Refuses another shape. */
	if (components == 0U ||
	    base_count != components ||
	    insert_count != components) {
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "bit-field instruction of operands "
					     "that are not 32-bit integers of "
					     "the result's size");
		return error;
	}

	/* Declares the result; its scalars are named below. */
	record = drv_gpu_spirv_result(parser, word[2], word[1], components, 0);
	if (record == NULL)
		return EINVAL;

	/* The mask of Count bits, shared by every component. */
	mask = drv_gpu_spirv_math_low_mask(parser, length[0]);

	/* Lowers each component by the instruction. */
	for (index = 0U; index < components; index++) {
		if (opcode == OP_BIT_FIELD_INSERT) {
			/* (Base & ~(mask << Offset)) | ((Insert << Offset) & (mask << Offset)). */
			field = drv_gpu_spirv_emit_value(parser,
							 DRV_GPU_IR_SHL,
							 mask,
							 place[0]);
			kept = drv_gpu_spirv_emit_value(parser,
							DRV_GPU_IR_INOT,
							field,
							0U);
			kept = drv_gpu_spirv_emit_value(parser,
							DRV_GPU_IR_IAND,
							base[index],
							kept);
			value = drv_gpu_spirv_emit_value(parser,
							 DRV_GPU_IR_SHL,
							 insert[index],
							 place[0]);
			value = drv_gpu_spirv_emit_value(parser,
							 DRV_GPU_IR_IAND,
							 value,
							 field);
			value = drv_gpu_spirv_emit_value(parser,
							 DRV_GPU_IR_IOR,
							 kept,
							 value);
		} else if (opcode == OP_BIT_FIELD_U_EXTRACT) {
			/* (Base >> Offset) & mask. */
			value = drv_gpu_spirv_emit_value(parser,
							 DRV_GPU_IR_SHR,
							 base[index],
							 place[0]);
			value = drv_gpu_spirv_emit_value(parser,
							 DRV_GPU_IR_IAND,
							 value,
							 mask);
		} else {
			/*
			 * (Base << (32 - Offset - Count)) >> (32 - Count),
			 * arithmetically; 0 for a Count of 0.
			 */
			thirty_two = drv_gpu_spirv_integer_constant(parser,
								    32U);
			up = drv_gpu_spirv_emit_value(parser,
						      DRV_GPU_IR_ISUB,
						      thirty_two,
						      place[0]);
			up = drv_gpu_spirv_emit_value(parser,
						      DRV_GPU_IR_ISUB,
						      up,
						      length[0]);
			down = drv_gpu_spirv_emit_value(parser,
							DRV_GPU_IR_ISUB,
							thirty_two,
							length[0]);
			value = drv_gpu_spirv_emit_value(parser,
							 DRV_GPU_IR_SHL,
							 base[index],
							 up);
			value = drv_gpu_spirv_emit_value(parser,
							 DRV_GPU_IR_ASR,
							 value,
							 down);
			empty = drv_gpu_spirv_math_integer_op(parser,
							      DRV_GPU_IR_IEQ,
							      length[0],
							      0U);
			zero = drv_gpu_spirv_integer_constant(parser, 0U);
			value = drv_gpu_spirv_select_value(parser,
							   empty,
							   zero,
							   value);
		}

		/* Names the component. */
		record->comp[index] = value;
	}

	/* Reports a failure an emitter latched. */
	if (parser->error != 0)
		return parser->error;

	/* Succeeded: the bit-field instruction is lowered. */
	return 0;
}

/*
 * Returns x with its bits in reverse order: the halves, quarters, bytes,
 * nibbles, pairs and single bits swapped in turn, each swap a pair of masks
 * and shifts.
 *
 * Procedure referenced: H. S. Warren, Jr., Hacker's Delight, 2nd ed.
 * (2013), section 7-1 (reversing bits).
 */
static uint32_t
drv_gpu_spirv_math_bit_reverse(
	struct drv_gpu_spirv_parser *parser,
	uint32_t x)
{
	static const uint32_t masks[5] = {
		0x55555555U,
		0x33333333U,
		0x0F0F0F0FU,
		0x00FF00FFU,
		0x0000FFFFU
	};
	uint32_t value;
	uint32_t low;
	uint32_t high;
	uint32_t step;
	uint32_t distance;

	/* Swaps neighbouring groups of 1, 2, 4, 8 and 16 bits. */
	value = x;
	distance = 1U;
	for (step = 0U; step < 5U; step++) {
		low = drv_gpu_spirv_math_integer_op(parser,
						    DRV_GPU_IR_IAND,
						    value,
						    masks[step]);
		low = drv_gpu_spirv_math_integer_op(parser,
						    DRV_GPU_IR_SHL,
						    low,
						    distance);
		high = drv_gpu_spirv_math_integer_op(parser,
						     DRV_GPU_IR_SHR,
						     value,
						     distance);
		high = drv_gpu_spirv_math_integer_op(parser,
						     DRV_GPU_IR_IAND,
						     high,
						     masks[step]);
		value = drv_gpu_spirv_emit_value(parser,
						 DRV_GPU_IR_IOR,
						 low,
						 high);
		distance *= 2U;
	}

	/* Succeeded: the reversed bits. */
	return value;
}

/*
 * Returns the number of set bits of x: counts of 2, 4 and 8 bits summed in
 * place, then the four byte counts added by one multiplication into the top
 * byte.
 *
 * Procedure referenced: H. S. Warren, Jr., Hacker's Delight, 2nd ed.
 * (2013), section 5-1 (counting 1-bits).
 */
static uint32_t
drv_gpu_spirv_math_bit_count(
	struct drv_gpu_spirv_parser *parser,
	uint32_t x)
{
	uint32_t value;
	uint32_t shifted;
	uint32_t low;

	/* Each pair of bits holds its count: x - ((x >> 1) & 0x55555555). */
	shifted = drv_gpu_spirv_math_integer_op(parser,
						DRV_GPU_IR_SHR,
						x,
						1U);
	shifted = drv_gpu_spirv_math_integer_op(parser,
						DRV_GPU_IR_IAND,
						shifted,
						0x55555555U);
	value = drv_gpu_spirv_emit_value(parser,
					 DRV_GPU_IR_ISUB,
					 x,
					 shifted);

	/* Each nibble: the sum of its two pairs. */
	low = drv_gpu_spirv_math_integer_op(parser,
					    DRV_GPU_IR_IAND,
					    value,
					    0x33333333U);
	shifted = drv_gpu_spirv_math_integer_op(parser,
						DRV_GPU_IR_SHR,
						value,
						2U);
	shifted = drv_gpu_spirv_math_integer_op(parser,
						DRV_GPU_IR_IAND,
						shifted,
						0x33333333U);
	value = drv_gpu_spirv_emit_value(parser,
					 DRV_GPU_IR_IADD,
					 low,
					 shifted);

	/* Each byte: the sum of its two nibbles. */
	shifted = drv_gpu_spirv_math_integer_op(parser,
						DRV_GPU_IR_SHR,
						value,
						4U);
	value = drv_gpu_spirv_emit_value(parser,
					 DRV_GPU_IR_IADD,
					 value,
					 shifted);
	value = drv_gpu_spirv_math_integer_op(parser,
					      DRV_GPU_IR_IAND,
					      value,
					      0x0F0F0F0FU);

	/* The four byte counts summed into the top byte. */
	value = drv_gpu_spirv_math_integer_op(parser,
					      DRV_GPU_IR_IMUL,
					      value,
					      0x01010101U);
	value = drv_gpu_spirv_math_integer_op(parser,
					      DRV_GPU_IR_SHR,
					      value,
					      24U);

	/* Succeeded: the count. */
	return value;
}

/*
 * Lowers OpBitReverse and OpBitCount per component of a 32-bit integer
 * operand.
 */
static int
drv_gpu_spirv_lower_bit_unary(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct drv_gpu_spirv_id *record;
	uint32_t operand[4];
	uint32_t operand_count;
	uint32_t components;
	uint32_t integers;
	uint32_t width;
	uint32_t index;
	int error;

	/* The instruction must carry its operand. */
	if (count != 4U)
		return EINVAL;

	/* The operand and the result are integers of one size, 32 bits wide. */
	operand_count = drv_gpu_spirv_operand(parser, word[3], operand);
	components = drv_gpu_spirv_int_components(parser, word[1]);
	integers = drv_gpu_spirv_operand_int_components(parser, word[3]);
	width = drv_gpu_spirv_int_width(parser, word[1]);
	if (width != 32U)
		components = 0U;
	width = drv_gpu_spirv_operand_int_width(parser, word[3]);
	if (width != 32U)
		components = 0U;
	if (components == 0U ||
	    operand_count != components ||
	    integers != components) {
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "bit instruction of an operand that "
					     "is not a 32-bit integer of the "
					     "result's size");
		return error;
	}

	/* Declares the result; its scalars are named below. */
	record = drv_gpu_spirv_result(parser, word[2], word[1], components, 0);
	if (record == NULL)
		return EINVAL;

	/* Reverses or counts each component. */
	for (index = 0U; index < components; index++) {
		if (opcode == OP_BIT_REVERSE) {
			record->comp[index] =
			    drv_gpu_spirv_math_bit_reverse(parser,
							   operand[index]);
		} else {
			record->comp[index] =
			    drv_gpu_spirv_math_bit_count(parser,
							 operand[index]);
		}
	}

	/* Reports a failure an emitter latched. */
	if (parser->error != 0)
		return parser->error;

	/* Succeeded: the bit instruction is lowered. */
	return 0;
}

/*
 * Returns the high 32 bits of the 64-bit product of a and b, unsigned or
 * signed.
 *
 * The unsigned product is taken in 16-bit halves (schoolbook
 * multiplication): with a = ah 2^16 + al and b likewise, each of al bl, al
 * bh, ah bl and ah bh fits 32 bits, and the middle column's carry is
 * gathered before it is added to the high one.  The signed high half is the
 * unsigned one less b where a is negative and less a where b is negative.
 *
 * Procedure referenced: D. E. Knuth, The Art of Computer Programming,
 * vol. 2, 3rd ed. (1997), section 4.3.1 (Algorithm M); H. S. Warren, Jr.,
 * Hacker's Delight, 2nd ed. (2013), sections 8-2 and 8-3 (the high-order
 * half of a product, and the signed one from the unsigned one).
 */
static uint32_t
drv_gpu_spirv_math_multiply_high(
	struct drv_gpu_spirv_parser *parser,
	uint32_t a,
	uint32_t b,
	int is_signed)
{
	uint32_t a_low;
	uint32_t a_high;
	uint32_t b_low;
	uint32_t b_high;
	uint32_t low_low;
	uint32_t low_high;
	uint32_t high_low;
	uint32_t high_high;
	uint32_t middle;
	uint32_t part;
	uint32_t high;
	uint32_t zero;
	uint32_t negative;
	uint32_t correction;

	/* Splits both operands into 16-bit halves. */
	a_low = drv_gpu_spirv_math_integer_op(parser,
					      DRV_GPU_IR_IAND,
					      a,
					      MATH_MASK_16);
	a_high = drv_gpu_spirv_math_integer_op(parser,
					       DRV_GPU_IR_SHR,
					       a,
					       16U);
	b_low = drv_gpu_spirv_math_integer_op(parser,
					      DRV_GPU_IR_IAND,
					      b,
					      MATH_MASK_16);
	b_high = drv_gpu_spirv_math_integer_op(parser,
					       DRV_GPU_IR_SHR,
					       b,
					       16U);

	/* The four partial products. */
	low_low = drv_gpu_spirv_emit_value(parser,
					   DRV_GPU_IR_IMUL,
					   a_low,
					   b_low);
	low_high = drv_gpu_spirv_emit_value(parser,
					    DRV_GPU_IR_IMUL,
					    a_low,
					    b_high);
	high_low = drv_gpu_spirv_emit_value(parser,
					    DRV_GPU_IR_IMUL,
					    a_high,
					    b_low);
	high_high = drv_gpu_spirv_emit_value(parser,
					     DRV_GPU_IR_IMUL,
					     a_high,
					     b_high);

	/* The middle column: below 3 * 2^16, so it cannot overflow. */
	middle = drv_gpu_spirv_math_integer_op(parser,
					       DRV_GPU_IR_SHR,
					       low_low,
					       16U);
	part = drv_gpu_spirv_math_integer_op(parser,
					     DRV_GPU_IR_IAND,
					     low_high,
					     MATH_MASK_16);
	middle = drv_gpu_spirv_emit_value(parser,
					  DRV_GPU_IR_IADD,
					  middle,
					  part);
	part = drv_gpu_spirv_math_integer_op(parser,
					     DRV_GPU_IR_IAND,
					     high_low,
					     MATH_MASK_16);
	middle = drv_gpu_spirv_emit_value(parser,
					  DRV_GPU_IR_IADD,
					  middle,
					  part);

	/* The high column with the middle's carry and the high halves. */
	high = high_high;
	part = drv_gpu_spirv_math_integer_op(parser,
					     DRV_GPU_IR_SHR,
					     low_high,
					     16U);
	high = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_IADD, high, part);
	part = drv_gpu_spirv_math_integer_op(parser,
					     DRV_GPU_IR_SHR,
					     high_low,
					     16U);
	high = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_IADD, high, part);
	part = drv_gpu_spirv_math_integer_op(parser,
					     DRV_GPU_IR_SHR,
					     middle,
					     16U);
	high = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_IADD, high, part);

	/* An unsigned product is done. */
	if (is_signed == 0)
		return high;

	/* Less b where a is negative, and less a where b is negative. */
	zero = drv_gpu_spirv_integer_constant(parser, 0U);
	negative = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_ILT, a, zero);
	correction = drv_gpu_spirv_select_value(parser, negative, b, zero);
	high = drv_gpu_spirv_emit_value(parser,
					DRV_GPU_IR_ISUB,
					high,
					correction);
	negative = drv_gpu_spirv_emit_value(parser, DRV_GPU_IR_ILT, b, zero);
	correction = drv_gpu_spirv_select_value(parser, negative, a, zero);
	high = drv_gpu_spirv_emit_value(parser,
					DRV_GPU_IR_ISUB,
					high,
					correction);

	/* Succeeded: the signed high half. */
	return high;
}

/*
 * Lowers OpIAddCarry, OpISubBorrow, OpUMulExtended and OpSMulExtended per
 * component of two 32-bit integer operands; the result is a struct of two
 * members of the operands' type: the sum and the carry (1 where the sum
 * wrapped), the difference and the borrow (1 where b was larger), or the low
 * and the high half of the 64-bit product.
 */
static int
drv_gpu_spirv_lower_extended_integer(
	struct drv_gpu_spirv_parser *parser,
	const uint32_t *word,
	uint32_t count,
	uint32_t opcode,
	uint32_t offset)
{
	struct drv_gpu_spirv_id *record;
	uint32_t left[4];
	uint32_t right[4];
	uint32_t left_count;
	uint32_t right_count;
	uint32_t components;
	uint32_t width;
	uint32_t scalars;
	uint32_t index;
	uint32_t one;
	uint32_t zero;
	uint32_t low;
	uint32_t wrapped;
	uint32_t second;
	int is_signed;
	int error;

	/* The instruction must carry both operands. */
	if (count != 5U)
		return EINVAL;

	/* Both are 32-bit integers of one size; the result is two of them. */
	left_count = drv_gpu_spirv_operand(parser, word[3], left);
	right_count = drv_gpu_spirv_operand(parser, word[4], right);
	components = drv_gpu_spirv_operand_int_components(parser, word[3]);
	width = drv_gpu_spirv_operand_int_width(parser, word[3]);
	if (width != 32U)
		components = 0U;
	width = drv_gpu_spirv_operand_int_width(parser, word[4]);
	if (width != 32U)
		components = 0U;
	scalars = drv_gpu_spirv_aggregate_scalars(parser, word[1]);
	if (scalars != 2U * components)
		components = 0U;
	if (components == 0U ||
	    left_count != components ||
	    right_count != components) {
		error = drv_gpu_spirv_refuse(parser,
					     opcode,
					     offset,
					     "extended integer arithmetic of "
					     "operands that are not 32-bit "
					     "integers of one size");
		return error;
	}

	/* Declares the result as the two members' scalars. */
	record = drv_gpu_spirv_result(parser,
				      word[2],
				      word[1],
				      2U * components,
				      0);
	if (record == NULL)
		return EINVAL;

	/* Lowers each component's two members. */
	one = drv_gpu_spirv_integer_constant(parser, 1U);
	zero = drv_gpu_spirv_integer_constant(parser, 0U);
	for (index = 0U; index < components; index++) {
		if (opcode == OP_IADD_CARRY) {
			/* The sum, and a carry where it is below an operand. */
			low = drv_gpu_spirv_emit_value(parser,
						       DRV_GPU_IR_IADD,
						       left[index],
						       right[index]);
			wrapped = drv_gpu_spirv_emit_value(parser,
							   DRV_GPU_IR_ULT,
							   low,
							   left[index]);
			second = drv_gpu_spirv_select_value(parser,
							    wrapped,
							    one,
							    zero);
		} else if (opcode == OP_ISUB_BORROW) {
			/* The difference, and a borrow where b exceeds a. */
			low = drv_gpu_spirv_emit_value(parser,
						       DRV_GPU_IR_ISUB,
						       left[index],
						       right[index]);
			wrapped = drv_gpu_spirv_emit_value(parser,
							   DRV_GPU_IR_ULT,
							   left[index],
							   right[index]);
			second = drv_gpu_spirv_select_value(parser,
							    wrapped,
							    one,
							    zero);
		} else {
			/* The low half, then the signed or unsigned high half. */
			is_signed = 0;
			if (opcode == OP_SMUL_EXTENDED)
				is_signed = 1;
			low = drv_gpu_spirv_emit_value(parser,
						       DRV_GPU_IR_IMUL,
						       left[index],
						       right[index]);
			second = drv_gpu_spirv_math_multiply_high(parser,
								  left[index],
								  right[index],
								  is_signed);
		}

		/* Names the first member's and the second member's scalar. */
		record->comp[index] = low;
		record->comp[components + index] = second;
	}

	/* Reports a failure an emitter latched. */
	if (parser->error != 0)
		return parser->error;

	/* Succeeded: the extended arithmetic is lowered. */
	return 0;
}
