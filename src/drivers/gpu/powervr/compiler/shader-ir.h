/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PowerVR shader compiler's intermediate form.
 *
 * The SPIR-V reader turns a shader into a linear list of scalar
 * instructions on numbered values.  Control flow stays structured: an
 * if, else, end-if, loop, end-loop, break and continue are instructions
 * of their own, in the order the shader nests them, so the back end can
 * lower each one to the core's execution mask counter.
 *
 * A value is a register-sized slot, written by any number of
 * instructions (the reader turns phis and local variables into plain
 * moves).  Most values are one 32-bit word; the four words a texture
 * sample returns are one value four words wide, kept in adjacent
 * registers.  The same code runs on the build host.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_SHADER_IR_H
#define KERN_DRIVERS_GPU_POWERVR_SHADER_IR_H

#include <stdint.h>

/* The most instructions and values one shader may have. */
#define POWERVR_IR_INSTRUCTIONS		4096U
#define POWERVR_IR_VALUES		4096U

/*
 * The operations of the intermediate form.
 *
 * Booleans are words: all ones for true, zero for false.  A float
 * comparison answers in that form; a select and an if test a word
 * against zero.
 */
enum powervr_ir_opcode {
	/* Structure. */
	POWERVR_IR_IF = 0,
	POWERVR_IR_ELSE,
	POWERVR_IR_END_IF,
	POWERVR_IR_LOOP,
	POWERVR_IR_LOOP_CONTINUE,
	POWERVR_IR_END_LOOP,
	POWERVR_IR_BREAK,
	POWERVR_IR_CONTINUE,

	/* Copies, with the float negate and absolute modifiers of the source. */
	POWERVR_IR_MOVE,

	/* Float arithmetic. */
	POWERVR_IR_FLOAT_ADD,
	POWERVR_IR_FLOAT_MULTIPLY,
	POWERVR_IR_FLOAT_MULTIPLY_ADD,
	POWERVR_IR_FLOAT_MINIMUM,
	POWERVR_IR_FLOAT_MAXIMUM,
	POWERVR_IR_FLOAT_FLOOR,
	POWERVR_IR_FLOAT_RECIPROCAL,
	POWERVR_IR_FLOAT_RECIPROCAL_ROOT,
	POWERVR_IR_FLOAT_LOG2,
	POWERVR_IR_FLOAT_EXP2,

	/* Float comparisons, answering a Boolean word. */
	POWERVR_IR_FLOAT_LESS,
	POWERVR_IR_FLOAT_GREATER_EQUAL,
	POWERVR_IR_FLOAT_EQUAL,
	POWERVR_IR_FLOAT_NOT_EQUAL,

	/* Integer arithmetic and bit operations. */
	POWERVR_IR_INTEGER_ADD,
	POWERVR_IR_INTEGER_MULTIPLY,
	POWERVR_IR_BIT_AND,
	POWERVR_IR_BIT_OR,
	POWERVR_IR_BIT_XOR,
	POWERVR_IR_SHIFT_LEFT,
	POWERVR_IR_SHIFT_RIGHT,
	POWERVR_IR_SHIFT_RIGHT_SIGNED,
	POWERVR_IR_UNSIGNED_MINIMUM,
	POWERVR_IR_UNSIGNED_MAXIMUM,
	POWERVR_IR_SIGNED_MINIMUM,
	POWERVR_IR_SIGNED_MAXIMUM,

	/* Integer comparisons, answering a Boolean word. */
	POWERVR_IR_INTEGER_EQUAL,
	POWERVR_IR_INTEGER_NOT_EQUAL,
	POWERVR_IR_UNSIGNED_LESS,
	POWERVR_IR_UNSIGNED_GREATER_EQUAL,
	POWERVR_IR_SIGNED_LESS,
	POWERVR_IR_SIGNED_GREATER_EQUAL,

	/* The second source where the first is not zero, else the third. */
	POWERVR_IR_SELECT,

	/* Conversions between float and 32-bit integers, rounding toward zero. */
	POWERVR_IR_FLOAT_TO_SIGNED,
	POWERVR_IR_FLOAT_TO_UNSIGNED,
	POWERVR_IR_SIGNED_TO_FLOAT,
	POWERVR_IR_UNSIGNED_TO_FLOAT,

	/* One word of a shared register request (immediate: request, word). */
	POWERVR_IR_SHARED,

	/* The instance's number within its task (a compute invocation's x). */
	POWERVR_IR_INSTANCE,

	/* A memory word at the address pair of sources 0 and 1 plus the byte offset of source 2. */
	POWERVR_IR_LOAD,

	/* Stores source 3 as that memory word. */
	POWERVR_IR_STORE,

	/* A vertex input word (immediate: its vertex input register). */
	POWERVR_IR_INPUT,

	/* A fragment input word (immediate: its coefficient set; flat or iterated). */
	POWERVR_IR_ITERATE,

	/* Gives source 0 to output word immediate. */
	POWERVR_IR_OUTPUT,

	/*
	 * Samples a two-dimensional texture at the coordinates of sources 0
	 * and 1 into a four-word value; immediate is the image state
	 * request, immediate_2 the sampler state request.
	 */
	POWERVR_IR_SAMPLE,

	/*
	 * Reads the colour already in pixel output register immediate into a
	 * four-word value of floats, unpacked from 8-bit unsigned normalised
	 * channels (red in the lowest byte); with immediate_2 1, from half
	 * floats in registers immediate and immediate + 1 (two channels a
	 * register, the lower first).
	 */
	POWERVR_IR_PIXEL_LOAD,

	/* Packs the four floats of sources 0 to 3 that way into pixel output register immediate (and the next for half floats). */
	POWERVR_IR_PIXEL_STORE,

	/*
	 * The shared program's parts: a one-word value of the four
	 * floats of sources 0 to 3 packed as 8-bit unsigned normalised
	 * channels (red in the lowest byte); source 0 given to shared
	 * register immediate; and, in the program that reads them, shared
	 * register immediate_2 (a colour the shared program packed) moved
	 * into pixel output register immediate under the overlap check.
	 */
	POWERVR_IR_PACK,
	POWERVR_IR_SHARED_STORE,
	POWERVR_IR_PIXEL_SHARED,

	/*
	 * Blends the four floats of sources 0 to 3, each clamped to [0, 1]
	 * (NaN to 0), with the 8-bit unsigned normalised colour in pixel
	 * output register immediate and packs the result back into it;
	 * immediate_2 is the blend (POWERVR_IR_BLEND_*).
	 */
	POWERVR_IR_PIXEL_BLEND,

	/*
	 * Tells the depth and visibility unit whether the fragment stays:
	 * source 0 a Boolean word, not zero when the shader discarded it.
	 * Once, before the colour outputs, in a fragment shader that
	 * discards (whose draws pass punch-through).
	 */
	POWERVR_IR_FEEDBACK,

	POWERVR_IR_OPCODE_COUNT
};

/*
 * The blends of POWERVR_IR_PIXEL_BLEND, s the source colour and d the
 * destination's, the same equation on every channel: s + d * (1 - s.a),
 * s - d * (1 - s.a), s + d, and s * d.
 */
#define POWERVR_IR_BLEND_OVER		0U
#define POWERVR_IR_BLEND_SUBTRACT	1U
#define POWERVR_IR_BLEND_ADD		2U
#define POWERVR_IR_BLEND_MULTIPLY	3U

/* What an operand names: a value's word, or a constant word. */
enum powervr_ir_operand_kind {
	POWERVR_IR_OPERAND_NONE = 0,
	POWERVR_IR_OPERAND_VALUE,
	POWERVR_IR_OPERAND_CONSTANT
};

/* The float modifiers an operand applies to what it reads, absolute first. */
#define POWERVR_IR_NEGATE	0x1U
#define POWERVR_IR_ABSOLUTE	0x2U

/* Interpolation of a fragment input. */
#define POWERVR_IR_FLAT		0x1U

/* A float add, multiply or multiply-add whose result is clamped to [0, 1] (NaN to 0), as the core's saturate does. */
#define POWERVR_IR_SATURATE	0x2U

/*
 * One source of an instruction.
 *
 * A value operand reads word component of value; a constant operand is
 * its bits.  The modifiers act on the word as a float.
 */
struct powervr_ir_operand {
	uint8_t kind;
	uint8_t modifiers;
	uint8_t component;
	uint16_t value;
	uint32_t constant;
};

/*
 * One instruction.
 *
 * Destination is the value written (unused by structure, stores and
 * outputs); the meaning of the sources and immediates follows the
 * opcode.
 */
struct powervr_ir_instruction {
	uint8_t opcode;
	uint8_t flags;
	uint16_t destination;
	struct powervr_ir_operand source[4];
	uint32_t immediate;
	uint32_t immediate_2;
};

/* One value: how many adjacent words it spans. */
struct powervr_ir_value {
	uint8_t width;
};

/*
 * A shader in the intermediate form.
 *
 * Value 0 is never used, so a zero destination means none.  Exact is set
 * when the module asks that some result not be contracted (SPIR-V
 * NoContraction), so no multiply and add are fused anywhere in it.
 */
struct powervr_ir_shader {
	struct powervr_ir_instruction instructions[POWERVR_IR_INSTRUCTIONS];
	struct powervr_ir_value values[POWERVR_IR_VALUES];
	unsigned instruction_count;
	unsigned value_count;
	unsigned exact;
};

#endif /* KERN_DRIVERS_GPU_POWERVR_SHADER_IR_H */
