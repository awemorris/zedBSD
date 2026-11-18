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

/* The sources one instruction may have (a texture lookup's data words take six). */
#define POWERVR_IR_SOURCES		6U

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

	/* The upper word of the 64-bit product of two unsigned words. */
	POWERVR_IR_UNSIGNED_MULTIPLY_HIGH,

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

	/*
	 * The difference of source 0 across the fragment's 2x2 block,
	 * horizontally or vertically (immediate: POWERVR_IR_DERIVATIVE_*);
	 * fragment programs only, and never the shared program's work.
	 */
	POWERVR_IR_FLOAT_DERIVATIVE,

	/*
	 * Packs the floats of sources 0 up (immediate_2 of them: 1 to 4 for
	 * the 8-bit formats, 1 or 2 for the 16-bit ones) into a one-word
	 * value by the core's pack format immediate (POWERVR_IR_PACK_*), the
	 * first channel in the lowest bits and the bits past the last
	 * channel undefined; and unpacks source 0 by that format into a
	 * value of immediate_2 floats.
	 */
	POWERVR_IR_PACK_CHANNELS,
	POWERVR_IR_UNPACK_CHANNELS,

	/* One word of a shared register request (immediate: request, word). */
	POWERVR_IR_SHARED,

	/* The instance's number within its task (a compute invocation's x). */
	POWERVR_IR_INSTANCE,

	/*
	 * A special register's word (immediate: the register): a fragment's
	 * position or facing; POWERVR_IR_SPECIAL_NONE for a word the core
	 * does not give, which the back end refuses when anything reads it.
	 */
	POWERVR_IR_SPECIAL,

	/* A memory word at the address pair of sources 0 and 1 plus the byte offset of source 2. */
	POWERVR_IR_LOAD,

	/* Stores source 3 as that memory word. */
	POWERVR_IR_STORE,

	/*
	 * Combines source 3 into that memory word at once and answers the
	 * word as it was (immediate: the core's atomic code,
	 * POWERVR_IR_ATOMIC_*).
	 */
	POWERVR_IR_ATOMIC,

	/* Locks or releases one of the core's mutexes (immediate: the mutex, immediate_2: POWERVR_IR_MUTEX_*). */
	POWERVR_IR_MUTEX,

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
	 * Looks a texture up as immediate_2 says (POWERVR_IR_TEXTURE_*):
	 * its data words are sources 0 up, in the core's order (the
	 * coordinates, then the level of detail, the address and the texel
	 * offsets the form names), and the four words it answers the
	 * destination's.  Immediate is the image state request, with the
	 * sampler state request in bits 8 to 15 (four words in shared
	 * registers each).
	 */
	POWERVR_IR_TEXTURE,

	/*
	 * Reads the colour already in pixel output register immediate into a
	 * four-word value of floats, unpacked from 8-bit unsigned normalised
	 * channels (red in the lowest byte); with immediate_2
	 * POWERVR_IR_PIXEL_HALF, from half floats in registers immediate and
	 * immediate + 1 (two channels a register, the lower first); with
	 * POWERVR_IR_PIXEL_WORD, the register's word as it is into a one-word
	 * value; with POWERVR_IR_PIXEL_TILE, a tile buffer's word the same way.
	 */
	POWERVR_IR_PIXEL_LOAD,

	/*
	 * Packs the four floats of sources 0 to 3 that way into pixel output
	 * register immediate (and the next for half floats); with
	 * POWERVR_IR_PIXEL_WORD, writes the word of source 0 as it is (a
	 * colour the reader packed itself); with POWERVR_IR_PIXEL_TILE, into a
	 * tile buffer's word, for the pixel's covered samples.
	 */
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

/*
 * The atomic operations of POWERVR_IR_ATOMIC, as the core codes them
 * (the add with the old word answered, and so on); a compare and
 * exchange has no code and is a loop under a mutex.
 *
 * Values referenced:
 * Mesa 26.2.6 src/imagination/pco/pco_isa.py (F_ATOMIC_OP).
 */
#define POWERVR_IR_ATOMIC_ADD		0x0U
#define POWERVR_IR_ATOMIC_EXCHANGE	0x2U
#define POWERVR_IR_ATOMIC_UNSIGNED_MIN	0x4U
#define POWERVR_IR_ATOMIC_SIGNED_MIN	0x5U
#define POWERVR_IR_ATOMIC_UNSIGNED_MAX	0x6U
#define POWERVR_IR_ATOMIC_SIGNED_MAX	0x7U
#define POWERVR_IR_ATOMIC_AND		0x8U
#define POWERVR_IR_ATOMIC_OR		0x9U
#define POWERVR_IR_ATOMIC_XOR		0xaU

/*
 * The core's mutexes a program takes (one for the emulated atomics, one
 * for the barriers between the tasks of a workgroup) and what
 * POWERVR_IR_MUTEX does with one: release it, release it and sleep
 * until another task wakes the sleepers, release it and wake them, or
 * lock it.
 *
 * Values referenced:
 * Mesa 26.2.6 src/imagination/pco/pco_ops.py (MUTEX_ID atomic_emu and
 * barrier) and pco_isa.py (F_LR).
 */
#define POWERVR_IR_MUTEX_ATOMIC		0U
#define POWERVR_IR_MUTEX_BARRIER	1U
#define POWERVR_IR_MUTEX_RELEASE	0U
#define POWERVR_IR_MUTEX_RELEASE_SLEEP	1U
#define POWERVR_IR_MUTEX_RELEASE_WAKEUP	2U
#define POWERVR_IR_MUTEX_LOCK		3U

/*
 * The derivatives of POWERVR_IR_FLOAT_DERIVATIVE: horizontal and
 * vertical, from the 2x2 block's first row or column (coarse) or the
 * fragment's own (fine).
 *
 * Values referenced:
 * Mesa 26.2.6 src/imagination/pco/pco_isa.py (F_SNGL_OP dsx 0b1000,
 * dsy 0b1001, dsxf 0b1010, dsyf 0b1011): the derivative's code less the
 * first one's.
 */
#define POWERVR_IR_DERIVATIVE_X		0U
#define POWERVR_IR_DERIVATIVE_Y		1U
#define POWERVR_IR_DERIVATIVE_X_FINE	2U
#define POWERVR_IR_DERIVATIVE_Y_FINE	3U

/*
 * The forms of POWERVR_IR_PIXEL_LOAD and _STORE in their immediate_2.
 * The tile form reads or writes a word as it is, like the word form,
 * in a tile buffer's words of the pixel rather than in a pixel output
 * register: its immediate names the shared register request of the
 * buffer's address (POWERVR_SHADER_REQUEST_TILE_BUFFER) and the word
 * (POWERVR_IR_PIXEL_TILE_PLACE), with POWERVR_IR_PIXEL_TILE_SAMPLES on a
 * store of a multisampled program (written for the pixel's covered
 * samples rather than its instance).
 */
#define POWERVR_IR_PIXEL_UNORM8		0U
#define POWERVR_IR_PIXEL_HALF		1U
#define POWERVR_IR_PIXEL_WORD		2U
#define POWERVR_IR_PIXEL_TILE		3U

#define POWERVR_IR_PIXEL_TILE_SAMPLES		0x80U
#define POWERVR_IR_PIXEL_TILE_PLACE(request, word)	(((uint32_t)(request) << 8) | (uint32_t)(word))
#define POWERVR_IR_PIXEL_TILE_REQUEST(immediate)	((uint32_t)(immediate) >> 8)
#define POWERVR_IR_PIXEL_TILE_WORD(immediate)		((uint32_t)(immediate) & 0x7fU)

/*
 * The pack formats of POWERVR_IR_PACK_CHANNELS and _UNPACK_CHANNELS:
 * four 8-bit and two 16-bit channels, unsigned or signed normalised
 * (scaled to the channels' range, rounded and clamped), and two half
 * floats.
 *
 * Values referenced:
 * Mesa 26.2.6 src/imagination/pco/pco_isa.py (F_PCK_FORMAT u8888
 * 0b00000, s8888 0b00001, u1616 0b00011, s1616 0b00100, f16f16
 * 0b01110) and pco_trans_nir.c (the pack and unpack of the 2x16 and
 * 4x8 forms: the normalised ones scaled).
 */
#define POWERVR_IR_PACK_U8888		0x00U
#define POWERVR_IR_PACK_S8888		0x01U
#define POWERVR_IR_PACK_U1616		0x03U
#define POWERVR_IR_PACK_S1616		0x04U
#define POWERVR_IR_PACK_F16F16		0x0eU

/*
 * The form of POWERVR_IR_TEXTURE in its immediate_2: the image's
 * dimensions (1 to 3, a cube's 3), how the level of detail is chosen
 * (the core's own from the coordinates' change, that plus a data word's
 * bias, a data word's level, or gradients in the data words), whether
 * the coordinates are in texels rather than normalised, whether two data
 * words give the texels' address in place of the image state's, whether
 * the result is integers (not converted to floats), whether a data word
 * packs constant texel offsets, whether that word (the last) carries a
 * multisampled image's sample number in its bits 16 to 18, and how many
 * data words there are.
 *
 * Values referenced:
 * Mesa 26.2.6 src/imagination/pco/pco_isa.py (F_DMN 1d 0b01, 2d 0b10,
 * 3d 0b11; F_LODM normal 0b00, bias 0b01, replace 0b10, gradients 0b11).
 */
#define POWERVR_IR_TEXTURE_DIMENSIONS(form)	((form) & 0x3U)
#define POWERVR_IR_TEXTURE_LOD(form)		(((form) >> 2) & 0x3U)
#define POWERVR_IR_TEXTURE_LOD_NORMAL		0x0U
#define POWERVR_IR_TEXTURE_LOD_BIAS		0x1U
#define POWERVR_IR_TEXTURE_LOD_REPLACE		0x2U
#define POWERVR_IR_TEXTURE_LOD_GRADIENTS	0x3U
#define POWERVR_IR_TEXTURE_TEXELS		0x10U
#define POWERVR_IR_TEXTURE_ADDRESS		0x20U
#define POWERVR_IR_TEXTURE_INTEGER		0x40U
#define POWERVR_IR_TEXTURE_OFFSETS		0x80U
#define POWERVR_IR_TEXTURE_SAMPLE_NUMBER	0x800U
#define POWERVR_IR_TEXTURE_WORDS(form)		(((form) >> 8) & 0x7U)
#define POWERVR_IR_TEXTURE_FORM(dimensions, lod, flags, words)	((dimensions) | ((lod) << 2) | (flags) | ((words) << 8))

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

/* A load that goes around the caches to memory (a word other tasks write). */
#define POWERVR_IR_BYPASS	0x4U

/* A fragment input iterated linearly in screen space, without perspective. */
#define POWERVR_IR_LINEAR	0x8U

/*
 * The special registers a fragment reads: its pixel's x and y (single
 * sampled) and which way its primitive faces; and none.
 *
 * Values referenced:
 * Mesa 26.2.6 src/imagination/pco/pco_isa.py (the special registers
 * x_p 97, y_p 100 and face_orient 44).
 */
#define POWERVR_IR_SPECIAL_X		97U
#define POWERVR_IR_SPECIAL_Y		100U
#define POWERVR_IR_SPECIAL_FACING	44U

/*
 * The special registers of a fragment run at the sample rate (four
 * samples): its sample's x and y, and its sample's number.
 *
 * Values referenced: Mesa 26.2.6 src/imagination/pco/pco_isa.py (x_s 98,
 * y_s 101, samp_num 53) and pco_trans_nir.c (trans_load_input: the
 * position's x and y from x_s and y_s unless single sampled;
 * nir_intrinsic_load_sample_id from samp_num).
 */
#define POWERVR_IR_SPECIAL_SAMPLE_X	98U
#define POWERVR_IR_SPECIAL_SAMPLE_Y	101U
#define POWERVR_IR_SPECIAL_SAMPLE	53U
#define POWERVR_IR_SPECIAL_NONE		0xffffU

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
	struct powervr_ir_operand source[POWERVR_IR_SOURCES];
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
