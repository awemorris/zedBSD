/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A host interpreter of the PowerVR compiler's intermediate form, for
 * the compiler's host test: it runs a shader over up to 32 lanes with a
 * mask per lane, the way the core runs a task, so the compiled program's
 * simulated results can be checked against it.
 */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ir-run.h"

/* How deep ifs and loops may nest. */
#define RUN_DEPTH	32

/* One open if: the lanes active at it and those whose condition held. */
struct run_if {
	uint32_t saved;
	uint32_t taken;
};

/* One open loop: the lanes at its start, those that broke and those that continued. */
struct run_loop {
	uint32_t entry;
	uint32_t broken;
	uint32_t continued;
	unsigned start;
};

static float as_float(uint32_t word);
static uint32_t as_word(float value);
static uint32_t read_operand(const struct ir_run *run, const struct powervr_ir_operand *operand, unsigned lane);
static uint32_t float_op(unsigned opcode, uint32_t a, uint32_t b, uint32_t c);
static uint32_t saturated(uint32_t word);

/*
 * Runs a shader on the lanes of a mask; reports 0, or -1 for a shader
 * the interpreter cannot follow.
 */
int
ir_run(
	struct ir_run *run,
	const struct powervr_ir_shader *shader,
	uint32_t lanes)
{
	const struct powervr_ir_instruction *instruction;
	struct run_if ifs[RUN_DEPTH];
	struct run_loop loops[RUN_DEPTH];
	unsigned if_depth;
	unsigned loop_depth;
	unsigned at;
	unsigned lane;
	unsigned word;
	uint32_t active;
	uint32_t a;
	uint32_t b;
	uint32_t c;
	uint32_t d;
	uint32_t result[4];
	uint64_t address;
	unsigned steps;

	/* Starts with every lane given. */
	active = lanes;
	if_depth = 0;
	loop_depth = 0;
	steps = 0;
	for (at = 0; at < shader->instruction_count; at++) {
		instruction = &shader->instructions[at];
		if (++steps > 10000000U)
			return -1;

		/* Structure moves the mask. */
		switch (instruction->opcode) {
		case POWERVR_IR_IF:
			if (if_depth >= RUN_DEPTH)
				return -1;
			ifs[if_depth].saved = active;
			ifs[if_depth].taken = 0;
			for (lane = 0; lane < 32; lane++) {
				if ((active >> lane & 1U) != 0 && read_operand(run, &instruction->source[0], lane) != 0)
					ifs[if_depth].taken |= 1U << lane;
			}
			active = ifs[if_depth].taken;
			if_depth++;
			continue;
		case POWERVR_IR_ELSE:
			if (if_depth == 0)
				return -1;
			active = ifs[if_depth - 1].saved & ~ifs[if_depth - 1].taken;
			continue;
		case POWERVR_IR_END_IF:
			if (if_depth == 0)
				return -1;
			if_depth--;
			active = ifs[if_depth].saved;
			if (loop_depth != 0)
				active &= ~(loops[loop_depth - 1].broken | loops[loop_depth - 1].continued);
			continue;
		case POWERVR_IR_LOOP:
			if (loop_depth >= RUN_DEPTH)
				return -1;
			loops[loop_depth].entry = active;
			loops[loop_depth].broken = 0;
			loops[loop_depth].continued = 0;
			loops[loop_depth].start = at;
			loop_depth++;
			continue;
		case POWERVR_IR_BREAK:
			if (loop_depth == 0)
				return -1;
			loops[loop_depth - 1].broken |= active;
			active = 0;
			continue;
		case POWERVR_IR_CONTINUE:
			if (loop_depth == 0)
				return -1;
			loops[loop_depth - 1].continued |= active;
			active = 0;
			continue;
		case POWERVR_IR_LOOP_CONTINUE:
			if (loop_depth == 0)
				return -1;
			active |= loops[loop_depth - 1].continued;
			loops[loop_depth - 1].continued = 0;
			continue;
		case POWERVR_IR_END_LOOP:
			if (loop_depth == 0)
				return -1;
			active |= loops[loop_depth - 1].continued;
			loops[loop_depth - 1].continued = 0;
			if (active != 0) {
				at = loops[loop_depth - 1].start;
				continue;
			}
			loop_depth--;
			active = loops[loop_depth].broken;
			continue;
		default:
			break;
		}

		/* Every other instruction runs on each active lane. */
		for (lane = 0; lane < 32; lane++) {
			if ((active >> lane & 1U) == 0)
				continue;
			a = read_operand(run, &instruction->source[0], lane);
			b = read_operand(run, &instruction->source[1], lane);
			c = read_operand(run, &instruction->source[2], lane);
			d = read_operand(run, &instruction->source[3], lane);
			result[0] = 0;
			result[1] = 0;
			result[2] = 0;
			result[3] = 0;

			switch (instruction->opcode) {
			case POWERVR_IR_MOVE:
				result[0] = float_op(instruction->opcode, a, b, c);
				break;
			case POWERVR_IR_INTEGER_ADD:
				result[0] = a + b;
				break;
			case POWERVR_IR_INTEGER_MULTIPLY:
				result[0] = a * b;
				break;
			case POWERVR_IR_BIT_AND:
				result[0] = a & b;
				break;
			case POWERVR_IR_BIT_OR:
				result[0] = a | b;
				break;
			case POWERVR_IR_BIT_XOR:
				result[0] = a ^ b;
				break;
			case POWERVR_IR_SHIFT_LEFT:
				result[0] = a << (b & 31U);
				break;
			case POWERVR_IR_SHIFT_RIGHT:
				result[0] = a >> (b & 31U);
				break;
			case POWERVR_IR_SHIFT_RIGHT_SIGNED:
				result[0] = (uint32_t)((int32_t)a >> (b & 31U));
				break;
			case POWERVR_IR_UNSIGNED_MINIMUM:
				result[0] = a < b ? a : b;
				break;
			case POWERVR_IR_UNSIGNED_MAXIMUM:
				result[0] = a > b ? a : b;
				break;
			case POWERVR_IR_SIGNED_MINIMUM:
				result[0] = (int32_t)a < (int32_t)b ? a : b;
				break;
			case POWERVR_IR_SIGNED_MAXIMUM:
				result[0] = (int32_t)a > (int32_t)b ? a : b;
				break;
			case POWERVR_IR_INTEGER_EQUAL:
				result[0] = a == b ? 0xffffffffU : 0;
				break;
			case POWERVR_IR_INTEGER_NOT_EQUAL:
				result[0] = a != b ? 0xffffffffU : 0;
				break;
			case POWERVR_IR_UNSIGNED_LESS:
				result[0] = a < b ? 0xffffffffU : 0;
				break;
			case POWERVR_IR_UNSIGNED_GREATER_EQUAL:
				result[0] = a >= b ? 0xffffffffU : 0;
				break;
			case POWERVR_IR_SIGNED_LESS:
				result[0] = (int32_t)a < (int32_t)b ? 0xffffffffU : 0;
				break;
			case POWERVR_IR_SIGNED_GREATER_EQUAL:
				result[0] = (int32_t)a >= (int32_t)b ? 0xffffffffU : 0;
				break;
			case POWERVR_IR_SELECT:
				result[0] = a != 0 ? b : c;
				break;
			case POWERVR_IR_SHARED:
				result[0] = run->shared(run, instruction->immediate, instruction->immediate_2);
				break;
			case POWERVR_IR_INSTANCE:
				result[0] = lane;
				break;
			case POWERVR_IR_LOAD:
				address = ((uint64_t)b << 32 | a) + c;
				result[0] = run->load(run, address);
				break;
			case POWERVR_IR_STORE:
				address = ((uint64_t)b << 32 | a) + c;
				run->store(run, address, d);
				break;
			case POWERVR_IR_INPUT:
				result[0] = run->input(run, lane, instruction->immediate);
				break;
			case POWERVR_IR_ITERATE:
				result[0] = run->iterate(run, lane, instruction->immediate, instruction->flags);
				break;
			case POWERVR_IR_OUTPUT:
				if (instruction->immediate < IR_RUN_OUTPUTS)
					run->outputs[instruction->immediate][lane] = a;
				break;
			case POWERVR_IR_FEEDBACK:
				/* The lane's fragment is dropped when the discard is not zero. */
				run->dropped[lane] = a != 0;
				break;
			case POWERVR_IR_SAMPLE:
				run->sample(run, instruction->immediate, instruction->immediate_2, a, b, result);
				break;
			case POWERVR_IR_PIXEL_LOAD:
				if (instruction->immediate_2 != 0) {
					/* Half floats: two channels in each of two registers. */
					ir_run_unpack_half(run->pixels[instruction->immediate & 3U][lane], result);
					ir_run_unpack_half(run->pixels[(instruction->immediate + 1U) & 3U][lane], result + 2);
				} else {
					ir_run_unpack(run->pixels[instruction->immediate & 3U][lane], result);
				}
				break;
			case POWERVR_IR_PIXEL_STORE:
				result[0] = a;
				result[1] = b;
				result[2] = c;
				result[3] = d;
				if (instruction->immediate_2 != 0) {
					/* Half floats: two channels in each of two registers. */
					run->pixels[instruction->immediate & 3U][lane] = ir_run_pack_half(a, b);
					run->pixels[(instruction->immediate + 1U) & 3U][lane] = ir_run_pack_half(c, d);
				} else {
					run->pixels[instruction->immediate & 3U][lane] = ir_run_pack(result);
				}
				result[0] = 0;
				result[1] = 0;
				result[2] = 0;
				result[3] = 0;
				break;
			case POWERVR_IR_PIXEL_BLEND:
				/* The colour blended with the 8-bit one in the pixel output, packed back (q1548). */
				result[0] = a;
				result[1] = b;
				result[2] = c;
				result[3] = d;
				run->pixels[instruction->immediate & 3U][lane] = ir_run_blend(instruction->immediate_2, result, run->pixels[instruction->immediate & 3U][lane]);
				result[0] = 0;
				result[1] = 0;
				result[2] = 0;
				result[3] = 0;
				break;
			default:
				result[0] = float_op(instruction->opcode, a, b, c);
				if ((instruction->flags & POWERVR_IR_SATURATE) != 0)
					result[0] = saturated(result[0]);
				break;
			}

			/* Writes the result's words. */
			if (instruction->destination != 0) {
				for (word = 0; word < shader->values[instruction->destination].width && word < 4; word++)
					run->values[instruction->destination][word][lane] = result[word];
			}
		}
	}

	/* Succeeded: the lanes ran to the end. */
	if (if_depth != 0 || loop_depth != 0)
		return -1;
	return 0;
}

/* Reads one operand on one lane, applying its float modifiers. */
static uint32_t
read_operand(
	const struct ir_run *run,
	const struct powervr_ir_operand *operand,
	unsigned lane)
{
	uint32_t word;

	/* The bits, then the modifiers. */
	if (operand->kind == POWERVR_IR_OPERAND_CONSTANT) {
		word = operand->constant;
	} else if (operand->kind == POWERVR_IR_OPERAND_VALUE) {
		word = run->values[operand->value][operand->component][lane];
	} else {
		return 0;
	}
	if ((operand->modifiers & POWERVR_IR_ABSOLUTE) != 0)
		word &= 0x7fffffffU;
	if ((operand->modifiers & POWERVR_IR_NEGATE) != 0)
		word ^= 0x80000000U;
	return word;
}

/* Works out one float operation. */
static uint32_t
float_op(
	unsigned opcode,
	uint32_t a,
	uint32_t b,
	uint32_t c)
{
	float x;
	float y;
	float z;

	/* The floats, then the operation. */
	x = as_float(a);
	y = as_float(b);
	z = as_float(c);
	switch (opcode) {
	case POWERVR_IR_MOVE:
		return a;
	case POWERVR_IR_FLOAT_ADD:
		return as_word(x + y);
	case POWERVR_IR_FLOAT_MULTIPLY:
		return as_word(x * y);
	case POWERVR_IR_FLOAT_MULTIPLY_ADD:
		return as_word(x * y + z);
	case POWERVR_IR_FLOAT_MINIMUM:
		return as_word(x < y ? x : y);
	case POWERVR_IR_FLOAT_MAXIMUM:
		return as_word(x > y ? x : y);
	case POWERVR_IR_FLOAT_FLOOR:
		return as_word(floorf(x));
	case POWERVR_IR_FLOAT_RECIPROCAL:
		return as_word(1.0f / x);
	case POWERVR_IR_FLOAT_RECIPROCAL_ROOT:
		return as_word(1.0f / sqrtf(x));
	case POWERVR_IR_FLOAT_LOG2:
		return as_word(log2f(x));
	case POWERVR_IR_FLOAT_EXP2:
		return as_word(exp2f(x));
	case POWERVR_IR_FLOAT_LESS:
		return x < y ? 0xffffffffU : 0;
	case POWERVR_IR_FLOAT_GREATER_EQUAL:
		return x >= y ? 0xffffffffU : 0;
	case POWERVR_IR_FLOAT_EQUAL:
		return x == y ? 0xffffffffU : 0;
	case POWERVR_IR_FLOAT_NOT_EQUAL:
		return x != y ? 0xffffffffU : 0;
	case POWERVR_IR_FLOAT_TO_SIGNED:
		if (!(x > -2147483648.0f))
			return 0x80000000U;
		if (x >= 2147483647.0f)
			return 0x7fffffffU;
		return (uint32_t)(int32_t)x;
	case POWERVR_IR_FLOAT_TO_UNSIGNED:
		if (!(x > 0.0f))
			return 0;
		if (x >= 4294967295.0f)
			return 0xffffffffU;
		return (uint32_t)x;
	case POWERVR_IR_SIGNED_TO_FLOAT:
		return as_word((float)(int32_t)a);
	case POWERVR_IR_UNSIGNED_TO_FLOAT:
		return as_word((float)a);
	default:
		return 0xdeadbeefU;
	}
}

/* The bits of a word as a float. */
static float
as_float(
	uint32_t word)
{
	float value;

	memcpy(&value, &word, sizeof(value));
	return value;
}

/* The bits of a float as a word. */
static uint32_t
as_word(
	float value)
{
	uint32_t word;

	memcpy(&word, &value, sizeof(word));
	return word;
}

/* Packs four floats to 8-bit unsigned normalised channels, red lowest, rounding to nearest. */
uint32_t
ir_run_pack(
	const uint32_t words[4])
{
	uint32_t packed;
	unsigned channel;
	float value;

	packed = 0;
	for (channel = 0; channel < 4; channel++) {
		value = as_float(words[channel]);
		if (!(value > 0.0f))
			value = 0.0f;
		if (value > 1.0f)
			value = 1.0f;
		packed |= (uint32_t)(value * 255.0f + 0.5f) << (channel * 8U);
	}
	return packed;
}

/*
 * Blends a colour of four floats, clamped, with an 8-bit unsigned
 * normalised one as POWERVR_IR_PIXEL_BLEND of the kind does, each channel
 * worked out in single precision the way the blend's arithmetic is, and
 * packs it.
 */
uint32_t
ir_run_blend(
	uint32_t kind,
	const uint32_t source[4],
	uint32_t packed)
{
	uint32_t destination[4];
	uint32_t blended[4];
	unsigned channel;
	float s;
	float d;
	float a;

	/* The destination's channels, then each channel's equation of the clamped colour. */
	ir_run_unpack(packed, destination);
	a = as_float(saturated(source[3]));
	for (channel = 0; channel < 4; channel++) {
		s = as_float(saturated(source[channel]));
		d = as_float(destination[channel]);
		if (kind == POWERVR_IR_BLEND_OVER)
			blended[channel] = as_word(d * (1.0f - a) + s);
		else if (kind == POWERVR_IR_BLEND_SUBTRACT)
			blended[channel] = as_word(-d * (1.0f - a) + s);
		else if (kind == POWERVR_IR_BLEND_ADD)
			blended[channel] = as_word(s + d);
		else
			blended[channel] = as_word(s * d);
	}
	return ir_run_pack(blended);
}

/* Unpacks 8-bit unsigned normalised channels to floats. */
void
ir_run_unpack(
	uint32_t packed,
	uint32_t words[4])
{
	unsigned channel;

	for (channel = 0; channel < 4; channel++)
		words[channel] = as_word((float)((packed >> (channel * 8U)) & 0xffU) / 255.0f);
}

/* Converts a float's bits to a half float's (round to nearest even; overflow to infinity; NaN kept a NaN). */
static uint32_t
half_from_float(
	uint32_t bits)
{
	uint32_t sign;
	uint32_t mantissa;
	int32_t exponent;
	uint32_t half;
	uint32_t shift;
	uint32_t rest;

	sign = (bits >> 16) & 0x8000U;
	exponent = (int32_t)((bits >> 23) & 0xffU);
	mantissa = bits & 0x7fffffU;
	if (exponent == 0xff)
		return sign | 0x7c00U | (mantissa != 0 ? 0x200U : 0U);
	exponent -= 127 - 15;
	if (exponent >= 31)
		return sign | 0x7c00U;
	if (exponent <= 0) {
		if (exponent < -10)
			return sign;
		mantissa |= 0x800000U;
		shift = (uint32_t)(14 - exponent);
		half = mantissa >> shift;
		rest = mantissa & ((1U << shift) - 1U);
		if (rest > (1U << (shift - 1U)) || (rest == (1U << (shift - 1U)) && (half & 1U) != 0))
			half++;
		return sign | half;
	}
	half = ((uint32_t)exponent << 10) | (mantissa >> 13);
	rest = mantissa & 0x1fffU;
	if (rest > 0x1000U || (rest == 0x1000U && (half & 1U) != 0))
		half++;
	return sign | half;
}

/* Converts a half float's bits to a float's. */
static uint32_t
float_from_half(
	uint32_t half)
{
	uint32_t sign;
	uint32_t exponent;
	uint32_t mantissa;

	sign = (half & 0x8000U) << 16;
	exponent = (half >> 10) & 0x1fU;
	mantissa = half & 0x3ffU;
	if (exponent == 0x1f)
		return sign | 0x7f800000U | (mantissa << 13);
	if (exponent == 0) {
		if (mantissa == 0)
			return sign;
		exponent = 1;
		while ((mantissa & 0x400U) == 0) {
			mantissa <<= 1;
			exponent--;
		}
		mantissa &= 0x3ffU;
		return sign | ((exponent + 127U - 15U) << 23) | (mantissa << 13);
	}
	return sign | ((exponent + 127U - 15U) << 23) | (mantissa << 13);
}

/* Packs two floats as half floats, the first in the low half. */
uint32_t
ir_run_pack_half(
	uint32_t low,
	uint32_t high)
{
	return half_from_float(low) | (half_from_float(high) << 16);
}

/* Unpacks two half floats to floats, the low half first. */
void
ir_run_unpack_half(
	uint32_t packed,
	uint32_t words[2])
{
	words[0] = float_from_half(packed & 0xffffU);
	words[1] = float_from_half(packed >> 16);
}

/* Clamps a float word to [0, 1], NaN to 0, as a saturated operation does (q1539). */
static uint32_t
saturated(
	uint32_t word)
{
	float value;

	/* The maximum with zero (NaN to 0), then the minimum with one. */
	value = as_float(word);
	value = value > 0.0f ? value : 0.0f;
	value = value < 1.0f ? value : 1.0f;
	return as_word(value);
}
