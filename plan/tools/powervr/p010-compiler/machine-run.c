/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A host simulator of the PowerVR compiler's machine operations, for
 * the compiler's host test.  It runs the operations the back end chose,
 * on registers, over 32 instances, so the register allocation, the
 * selection and the execution mask counter's steps can be checked
 * against the intermediate form's interpreter.
 *
 * The execution mask counter's steps follow a model inferred from how
 * the Mesa compiler uses them (pco_cf.c), not from a hardware manual:
 * an instance runs while its counter is zero; a start raises the
 * counter of an instance that is not taken (or already off) by the
 * adjust, an else swaps 0 and 1, an end lowers it by the adjust, a set
 * gives a running instance a value, and the loop test either keeps the
 * running instances looping (p0) or, when none runs, lowers every
 * counter by the adjust.
 */

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "ir-run.h"
#include "machine-run.h"

/* The operations the simulator knows, in the back end's order. */
enum {
	KIND_IMMEDIATE = 0,
	KIND_MOVE,
	KIND_MOVE_SPECIAL,
	KIND_FLOAT_ADD,
	KIND_FLOAT_MULTIPLY,
	KIND_FLOAT_MULTIPLY_ADD,
	KIND_SINGLE,
	KIND_MINMAX,
	KIND_COMPARE,
	KIND_SELECT,
	KIND_INTEGER,
	KIND_ADD64,
	KIND_LOGIC,
	KIND_SHIFT,
	KIND_PACK,
	KIND_UNPACK,
	KIND_LOAD,
	KIND_STORE,
	KIND_WAIT,
	KIND_ITERATE,
	KIND_SAMPLE,
	KIND_VERTEX_WRITE,
	KIND_VERTEX_END,
	KIND_END,
	KIND_TEST_ZERO,
	KIND_CONDITION,
	KIND_BRANCH,
	KIND_ITERATE_SAMPLE,
	KIND_SUM_OF_PRODUCTS,
	KIND_FEEDBACK
};

static float as_float(uint32_t word);
static uint32_t as_word(float value);
static uint32_t saturated(uint32_t word, unsigned saturate);
static uint32_t read_register(const struct machine_run *run, struct shader_machine_register reg, unsigned lane);
static uint32_t modified(uint32_t word, unsigned modifiers);
static void write_register(struct machine_run *run, struct shader_machine_register reg, unsigned lane, uint32_t word);
static int test(unsigned comparison, unsigned type, uint32_t a, uint32_t b);
static int request_at(const struct powervr_shader_program *program, unsigned shared, unsigned *word);

/*
 * Runs a compiled program's machine operations on the lanes of a mask;
 * reports 0, or -1 for an operation the simulator does not follow.
 */
int
machine_run_program(
	struct machine_run *run,
	const struct shader_backend *backend,
	uint32_t lanes)
{
	const struct shader_machine_operation *operation;
	uint32_t enabled;
	uint32_t taking;
	uint32_t a;
	uint32_t b;
	uint32_t c;
	uint32_t result;
	uint32_t words[4];
	uint64_t address;
	unsigned pc;
	unsigned lane;
	unsigned steps;
	unsigned index;
	unsigned word;
	unsigned holds;
	int any;

	/* Every lane starts running. */
	enabled = lanes;
	pc = 0;
	steps = 0;
	while (pc < backend->operation_count) {
		operation = &backend->operations[pc];
		if (++steps > 4000000U)
			return -1;

		/* The lanes the operation runs on. */
		if (operation->condition == 0) {
			taking = enabled;
		} else if (operation->condition == 1) {
			taking = 0;
			for (lane = 0; lane < 32; lane++) {
				if ((enabled >> lane & 1U) != 0 && run->predicate[lane] != 0)
					taking |= 1U << lane;
			}
		} else if (operation->condition == 2) {
			taking = lanes;
		} else {
			return -1;
		}

		/* Control first. */
		if (operation->kind == KIND_END || operation->kind == KIND_VERTEX_END)
			break;

		/* The alpha feedback passes the lanes it runs on; an enabled lane it skips is dropped. */
		if (operation->kind == KIND_FEEDBACK) {
			for (lane = 0; lane < 32; lane++) {
				if ((enabled >> lane & 1U) != 0)
					run->dropped[lane] = (taking >> lane & 1U) == 0;
			}
			pc++;
			continue;
		}
		if (operation->kind == KIND_BRANCH) {
			any = 0;
			if (operation->variant == 1) {
				/* Taken when no lane runs. */
				if (enabled == 0)
					any = 1;
			} else if (taking != 0) {
				any = 1;
			}
			pc = any ? operation->target : pc + 1U;
			continue;
		}
		if (operation->kind == KIND_CONDITION && operation->variant == 3) {
			/* The loop test over the whole task. */
			any = 0;
			for (lane = 0; lane < 32; lane++) {
				if ((lanes >> lane & 1U) != 0 && run->temps[0][lane] == 0)
					any = 1;
			}
			for (lane = 0; lane < 32; lane++) {
				if ((lanes >> lane & 1U) == 0)
					continue;
				if (any != 0) {
					run->predicate[lane] = run->temps[0][lane] == 0;
				} else {
					run->predicate[lane] = 0;
					if (run->temps[0][lane] >= operation->immediate_2)
						run->temps[0][lane] -= operation->immediate_2;
				}
			}
			enabled = 0;
			for (lane = 0; lane < 32; lane++) {
				if ((lanes >> lane & 1U) != 0 && run->temps[0][lane] == 0)
					enabled |= 1U << lane;
			}
			pc++;
			continue;
		}

		/* Every other operation lane by lane. */
		for (lane = 0; lane < 32; lane++) {
			if ((taking >> lane & 1U) == 0)
				continue;
			a = modified(read_register(run, operation->source[0], lane), operation->modifiers[0]);
			b = modified(read_register(run, operation->source[1], lane), operation->modifiers[1]);
			c = modified(read_register(run, operation->source[2], lane), operation->modifiers[2]);

			switch (operation->kind) {
			case KIND_IMMEDIATE:
				write_register(run, operation->destination[0], lane, operation->immediate);
				break;
			case KIND_MOVE:
			case KIND_MOVE_SPECIAL:
				write_register(run, operation->destination[0], lane, a);
				break;
			case KIND_FLOAT_ADD:
				if (operation->variant != 0)
					a = as_word(floorf(as_float(a)));
				write_register(run, operation->destination[0], lane, saturated(as_word(as_float(a) + as_float(b)), operation->saturate));
				break;
			case KIND_FLOAT_MULTIPLY:
				write_register(run, operation->destination[0], lane, saturated(as_word(as_float(a) * as_float(b)), operation->saturate));
				break;
			case KIND_FLOAT_MULTIPLY_ADD:
				write_register(run, operation->destination[0], lane, saturated(as_word(as_float(a) * as_float(b) + as_float(c)), operation->saturate));
				break;
			case KIND_SINGLE:
				switch (operation->variant) {
				case 0:
					result = as_word(1.0f / as_float(a));
					break;
				case 1:
					result = as_word(1.0f / sqrtf(as_float(a)));
					break;
				case 2:
					result = as_word(log2f(as_float(a)));
					break;
				case 3:
					result = as_word(exp2f(as_float(a)));
					break;
				default:
					return -1;
				}
				write_register(run, operation->destination[0], lane, result);
				break;
			case KIND_MINMAX:
				result = test(operation->variant, operation->immediate, a, b) ? a : b;
				write_register(run, operation->destination[0], lane, result);
				break;
			case KIND_COMPARE:
				result = test(operation->variant, operation->immediate, a, b) ? 0xffffffffU : 0;
				write_register(run, operation->destination[0], lane, result);
				break;
			case KIND_SELECT:
				write_register(run, operation->destination[0], lane, a != 0 ? b : c);
				break;
			case KIND_INTEGER:
				write_register(run, operation->destination[0], lane, a * b + c);
				break;
			case KIND_ADD64:
				address = ((uint64_t)b << 32 | a) + c;
				write_register(run, operation->destination[0], lane, (uint32_t)address);
				write_register(run, operation->destination[1], lane, (uint32_t)(address >> 32));
				break;
			case KIND_LOGIC:
				if (operation->variant == 0) {
					result = a | b;
				} else if (operation->variant == 1) {
					result = a & b;
				} else if (operation->variant == 2) {
					result = a ^ b;
				} else {
					return -1;
				}
				write_register(run, operation->destination[0], lane, result);
				break;
			case KIND_SHIFT:
				if (operation->variant == 0) {
					result = a << (b & 31U);
				} else if (operation->variant == 1) {
					result = a >> (b & 31U);
				} else if (operation->variant == 4) {
					result = (uint32_t)((int32_t)a >> (b & 31U));
				} else {
					return -1;
				}
				write_register(run, operation->destination[0], lane, result);
				break;
			case KIND_PACK:
				if (operation->variant == 14 && operation->immediate == 2) {
					/* Two registers from the source on, as half floats. */
					write_register(run, operation->destination[0], lane,
						       ir_run_pack_half(run->temps[operation->source[0].index][lane], run->temps[operation->source[0].index + 1U][lane]));
					break;
				}
				if (operation->variant == 0 && operation->immediate == 4) {
					/* The four registers from the source on, packed. */
					for (word = 0; word < 4; word++)
						words[word] = run->temps[operation->source[0].index + word][lane];
					write_register(run, operation->destination[0], lane, ir_run_pack(words));
					break;
				}
				if (operation->variant == 7) {
					if (!(as_float(a) > -2147483648.0f)) {
						result = 0x80000000U;
					} else if (as_float(a) >= 2147483647.0f) {
						result = 0x7fffffffU;
					} else {
						result = (uint32_t)(int32_t)as_float(a);
					}
				} else {
					if (!(as_float(a) > 0.0f)) {
						result = 0;
					} else if (as_float(a) >= 4294967295.0f) {
						result = 0xffffffffU;
					} else {
						result = (uint32_t)as_float(a);
					}
				}
				write_register(run, operation->destination[0], lane, result);
				break;
			case KIND_UNPACK:
				if (operation->variant == 14 && operation->immediate == 2) {
					/* Into two registers from the destination on, from half floats. */
					ir_run_unpack_half(a, words);
					run->temps[operation->destination[0].index][lane] = words[0];
					run->temps[operation->destination[0].index + 1U][lane] = words[1];
					break;
				}
				if (operation->variant == 0 && operation->immediate == 4) {
					/* Into the four registers from the destination on. */
					ir_run_unpack(a, words);
					for (word = 0; word < 4; word++)
						run->temps[operation->destination[0].index + word][lane] = words[word];
					break;
				}
				if (operation->variant == 7) {
					result = as_word((float)(int32_t)a);
				} else {
					result = as_word((float)a);
				}
				write_register(run, operation->destination[0], lane, result);
				break;
			case KIND_LOAD:
			case KIND_STORE:
				index = operation->kind == KIND_LOAD ? 0 : 1;
				address = run->temps[operation->source[index].index][lane];
				address |= (uint64_t)run->temps[operation->source[index].index + 1U][lane] << 32;
				if (address < run->memory_base || address >= run->memory_base + run->memory_words * 4ULL || (address & 3U) != 0) {
					if (operation->kind == KIND_LOAD)
						write_register(run, operation->destination[0], lane, 0xbadbad00U);
					break;
				}
				if (operation->kind == KIND_LOAD) {
					write_register(run, operation->destination[0], lane, run->memory[(address - run->memory_base) / 4U]);
				} else {
					/* The data from the register right after the address pair, as the hardware takes it (vf2-dmesg95). */
					run->memory[(address - run->memory_base) / 4U] = run->temps[operation->source[index].index + 2U][lane];
				}
				break;
			case KIND_WAIT:
				break;
			case KIND_ITERATE:
				/* Variant + 1 words from consecutive coefficient sets into consecutive registers (q1539). */
				for (word = 0; word <= operation->variant; word++) {
					struct shader_machine_register next = operation->destination[0];

					next.index = (uint16_t)(next.index + word);
					write_register(run, next, lane, run->iterate_word(lane, operation->immediate + word * 4U, 0));
				}
				break;
			case KIND_SUM_OF_PRODUCTS:
				/*
				 * The four floats packed into the pixel output, or blended
				 * with the colour there first (q1548; the forms of enum
				 * powervr_usc_sum in their order, over to multiply).
				 */
				if (operation->variant == POWERVR_USC_SUM_OPAQUE_HALF || operation->variant == POWERVR_USC_SUM_OVER_HALF) {
					/* The half forms: red and green, then blue and alpha, as half-float pairs in two registers. */
					ir_run_unpack_half(read_register(run, operation->source[0], lane), words);
					ir_run_unpack_half(read_register(run, operation->source[1], lane), words + 2);
				} else {
					for (word = 0; word < 4; word++)
						words[word] = read_register(run, operation->source[word], lane);
				}
				if (operation->variant == POWERVR_USC_SUM_OPAQUE || operation->variant == POWERVR_USC_SUM_OPAQUE_HALF) {
					write_register(run, operation->destination[0], lane, ir_run_pack(words));
				} else if (operation->variant == POWERVR_USC_SUM_OVER_HALF) {
					result = read_register(run, operation->destination[0], lane);
					write_register(run, operation->destination[0], lane, ir_run_blend(POWERVR_USC_SUM_OVER - 1U, words, result));
				} else if (operation->variant < POWERVR_USC_SUM_OPAQUE_HALF) {
					result = read_register(run, operation->destination[0], lane);
					write_register(run, operation->destination[0], lane, ir_run_blend(operation->variant - 1U, words, result));
				} else {
					return -1;
				}
				break;
			case KIND_ITERATE_SAMPLE:
				{
					unsigned image_word;
					unsigned sampler_word;
					int image;
					int sampler;

					/* The two coordinates iterated, then sampled (q1546). */
					image = request_at(run->program, operation->source[0].index, &image_word);
					sampler = request_at(run->program, operation->source[2].index, &sampler_word);
					if (image < 0 || sampler < 0)
						return -1;
					run->sample_words((unsigned)image, (unsigned)sampler, run->iterate_word(lane, operation->immediate, 0),
							  run->iterate_word(lane, operation->immediate + 4U, 0), words);
					if (operation->half) {
						/* Half floats, two to a register. */
						run->temps[operation->destination[0].index][lane] = ir_run_pack_half(words[0], words[1]);
						run->temps[operation->destination[0].index + 1U][lane] = ir_run_pack_half(words[2], words[3]);
						break;
					}
					for (word = 0; word < 4; word++)
						run->temps[operation->destination[0].index + word][lane] = words[word];
				}
				break;
			case KIND_SAMPLE:
				{
					unsigned image;
					unsigned sampler;
					unsigned image_word;
					unsigned sampler_word;

					if (request_at(run->program, operation->source[0].index, &image_word) < 0)
						return -1;
					if (request_at(run->program, operation->source[2].index, &sampler_word) < 0)
						return -1;
					image = (unsigned)request_at(run->program, operation->source[0].index, &image_word);
					sampler = (unsigned)request_at(run->program, operation->source[2].index, &sampler_word);
					run->sample_words(image, sampler, run->temps[operation->source[1].index][lane], run->temps[operation->source[1].index + 1U][lane], words);
					for (word = 0; word < 4; word++)
						run->temps[operation->destination[0].index + word][lane] = words[word];
				}
				break;
			case KIND_VERTEX_WRITE:
				if (operation->immediate < MACHINE_RUN_OUTPUTS)
					run->outputs[operation->immediate][lane] = a;
				break;
			case KIND_TEST_ZERO:
				run->predicate[lane] = a == 0;
				break;
			case KIND_CONDITION:
				holds = 1;
				if (operation->immediate == 1)
					holds = run->predicate[lane] != 0;
				if (operation->immediate == 3)
					holds = run->predicate[lane] == 0;
				result = run->temps[0][lane];
				if (operation->variant == 0) {
					/* A start, from the source (the counter, or zero at the beginning). */
					if (a == 0) {
						result = holds ? 0 : operation->immediate_2;
					} else {
						result = a + operation->immediate_2;
					}
				} else if (operation->variant == 1) {
					/* An else swaps the taken and the not taken. */
					if (result == 0) {
						result = 1;
					} else if (result == 1) {
						result = 0;
					}
				} else if (operation->variant == 2) {
					/* A set stops a running lane by the value. */
					if (result == 0 && holds)
						result = b;
				} else if (operation->variant == 4) {
					/* An end lowers a stopped lane's counter. */
					if (result >= operation->immediate_2)
						result -= operation->immediate_2;
				} else {
					return -1;
				}
				run->temps[0][lane] = result;
				break;
			default:
				return -1;
			}
		}

		/* A counter step changes which lanes run. */
		if (operation->kind == KIND_CONDITION) {
			enabled = 0;
			for (lane = 0; lane < 32; lane++) {
				if ((lanes >> lane & 1U) != 0 && run->temps[0][lane] == 0)
					enabled |= 1U << lane;
			}
		}

		/* An operation with the end flag ends the program after it runs (q1539). */
		if (operation->end != 0)
			break;
		pc++;
	}

	/* Collects a fragment shader's colours from their temporaries. */
	if (run->program->stage == POWERVR_SHADER_FRAGMENT) {
		for (index = 0; index < run->program->output_count; index++) {
			if (run->program->outputs[index].format != 0)
				continue;
			for (word = 0; word < 4; word++) {
				for (lane = 0; lane < 32; lane++) {
					if (run->program->outputs[index].location * 4U + word < MACHINE_RUN_OUTPUTS)
						run->outputs[run->program->outputs[index].location * 4U + word][lane] = run->temps[run->program->outputs[index].reg + word][lane];
				}
			}
		}
	}

	/* Succeeded: the program ran to its end. */
	if (pc >= backend->operation_count)
		return -1;
	return 0;
}

/* Reads one register on one lane. */
static uint32_t
read_register(
	const struct machine_run *run,
	struct shader_machine_register reg,
	unsigned lane)
{
	unsigned word;
	int request;

	/* By bank. */
	switch (reg.bank) {
	case 0:
		if (reg.index == 0)
			return 0;
		if (reg.index == 1)
			return 1;
		if (reg.index == 64)
			return 0x3f800000U;
		if (reg.index == 143)
			return 0xffffffffU;
		if (reg.index == 51)
			return lane;
		if (reg.index >= 32 && reg.index < 36)
			return run->pixels[reg.index - 32U][lane];
		if (reg.index < 32)
			return reg.index;
		return 0xdead0000U;
	case 1:
		if (reg.index >= SHADER_MACHINE_TEMPORARIES)
			return 0xdead0001U;
		return run->temps[reg.index][lane];
	case 2:
		return run->input_word(lane, reg.index);
	case 3:
		/* A compute program's coefficients (its workgroup id); otherwise only flat varyings' C coefficients are read directly. */
		if (run->coefficient_word != NULL)
			return run->coefficient_word(reg.index);
		return run->iterate_word(lane, reg.index - 2U, 1);
	case 4:
		request = request_at(run->program, reg.index, &word);
		if (request < 0)
			return 0xdead0004U;
		/* A literal the data sequencer writes (q1539: the back end's constants of float operations). */
		if (run->program->requests[request].kind == POWERVR_SHADER_REQUEST_LITERAL)
			return run->program->requests[request].literal;
		/* A word the shared program wrote (q1546). */
		if (run->program->requests[request].kind == POWERVR_SHADER_REQUEST_COMPUTED) {
			if (reg.index >= MACHINE_RUN_SHAREDS || run->shared_written[reg.index] == 0)
				return 0xdead0005U;
			return run->shareds[reg.index];
		}
		return run->shared_word(run->program, (uint32_t)request, word);
	default:
		return 0;
	}
}

/* Applies the float modifiers: absolute, then negate. */
static uint32_t
modified(
	uint32_t word,
	unsigned modifiers)
{
	if ((modifiers & POWERVR_IR_ABSOLUTE) != 0)
		word &= 0x7fffffffU;
	if ((modifiers & POWERVR_IR_NEGATE) != 0)
		word ^= 0x80000000U;
	return word;
}

/* Writes one temporary on one lane. */
static void
write_register(
	struct machine_run *run,
	struct shader_machine_register reg,
	unsigned lane,
	uint32_t word)
{
	if (reg.bank == 0 && reg.index >= 32 && reg.index < 36) {
		run->pixels[reg.index - 32U][lane] = word;
		return;
	}
	if (reg.bank == 4 && reg.index < MACHINE_RUN_SHAREDS) {
		run->shareds[reg.index] = word;
		run->shared_written[reg.index] = 1;
		return;
	}
	if (reg.bank != 1 || reg.index >= SHADER_MACHINE_TEMPORARIES)
		return;
	run->temps[reg.index][lane] = word;
}

/* Tests two words as the test operation does. */
static int
test(
	unsigned comparison,
	unsigned type,
	uint32_t a,
	uint32_t b)
{
	float x;
	float y;

	/* Floats, unsigned or signed. */
	if (type == 0) {
		x = as_float(a);
		y = as_float(b);
		switch (comparison) {
		case 4:
			return x == y;
		case 5:
			return x > y;
		case 6:
			return x >= y;
		case 7:
			return x != y;
		case 8:
			return x < y;
		default:
			return 0;
		}
	}
	if (type == 6) {
		a ^= 0x80000000U;
		b ^= 0x80000000U;
	}
	switch (comparison) {
	case 4:
		return a == b;
	case 5:
		return a > b;
	case 6:
		return a >= b;
	case 7:
		return a != b;
	case 8:
		return a < b;
	default:
		return 0;
	}
}

/* Finds the request a shared register belongs to, and its word in it. */
static int
request_at(
	const struct powervr_shader_program *program,
	unsigned shared,
	unsigned *word)
{
	unsigned index;

	for (index = 0; index < program->request_count; index++) {
		if (shared >= program->requests[index].shared && shared < program->requests[index].shared + program->requests[index].words) {
			*word = shared - program->requests[index].shared;
			return (int)index;
		}
	}
	return -1;
}

static float
as_float(
	uint32_t word)
{
	float value;

	memcpy(&value, &word, sizeof(value));
	return value;
}

static uint32_t
as_word(
	float value)
{
	uint32_t word;

	memcpy(&word, &value, sizeof(word));
	return word;
}

/* Clamps a float word to [0, 1] (NaN to 0) when the operation saturates (q1539). */
static uint32_t
saturated(
	uint32_t word,
	unsigned saturate)
{
	float value;

	/* As it is without the saturate. */
	if (saturate == 0)
		return word;

	/* The maximum with zero (NaN to 0), then the minimum with one. */
	value = as_float(word);
	value = value > 0.0f ? value : 0.0f;
	value = value < 1.0f ? value : 1.0f;
	return as_word(value);
}
