/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host fixture for the LOWERING stage of the shader compiler (SPIR-V -> scalar IR).
 *
 * The IR is executed by a small interpreter and the outputs are compared with values
 * computed by an independent formula -- never with what the parser says it did.  The
 * inputs are chosen so that a wrong substitute cannot match by accident: distinct
 * powers of two per component, operands whose order matters (5 - 2 against 2 - 5),
 * a local that is overwritten between two loads, dot((1,2,3,4),(5,6,7,8)) = 70.
 *
 * "Lowered" here means exactly this stage.  EU generation is checked in the compile
 * fixture; nothing in this file is evidence about GPU execution.
 */

#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned fixture_live;

void *
kern_calloc(size_t count, size_t size)
{
	void *pointer = calloc(count, size);
	if (pointer != NULL)
		fixture_live++;
	return pointer;
}

void
kern_free(void *pointer)
{
	if (pointer != NULL)
		fixture_live--;
	free(pointer);
}

#include "../../../src/drivers/gpu/compiler/spirv.c"
#include "../../../src/drivers/gpu/i915/compiler/eu.c"
#include "../../../src/drivers/gpu/i915/compiler/compile.c"
#include "../../../src/drivers/gpu/i915/tests/fixtures/generality-shaders-gen.inc"

/* ------------------------------------------------------------------ the IR interpreter */

#define SLOTS 17U
#define SLOT_POSITION (SLOTS - 1U)

struct machine {
	float input[SLOTS][4];
	float frag_coord[4];            /* gl_FragCoord of the pixel (ws075-p004) */
	uint8_t push[128];
	uint8_t ubo[4][256];            /* the words of uniform blocks 0 .. 3 of the IR's uniform list */
	float output[SLOTS][4];
	unsigned written[SLOTS][4];     /* how many times each output component was stored */
	int killed;                     /* a KILL whose condition was true ran */
};

/* the "texture": every component depends on u, v and the binding in a different way */
static void
fake_texture(uint32_t set, uint32_t binding, float u, float v, float rgba[4])
{
	rgba[0] = u + 10.0f * (float)binding;
	rgba[1] = v + 100.0f * (float)set;
	rgba[2] = u + 2.0f * v;
	rgba[3] = u * v + 0.5f;
}

static float
bits_to_float(uint32_t bits)
{
	float value;

	memcpy(&value, &bits, sizeof(value));
	return value;
}

static uint32_t
float_to_bits(float value)
{
	uint32_t bits;

	memcpy(&bits, &value, sizeof(bits));
	return bits;
}

/* the IR value bits of a float, and back */
static float
fbits(uint32_t bits)
{
	return bits_to_float(bits);
}

/* The result of a comparison as the IR keeps a Boolean: all ones or zero. */
static uint32_t
boolean(int truth)
{
	return truth ? 0xFFFFFFFFU : 0U;
}

/*
 * Runs the IR for one invocation; asserts the SSA discipline (defined once -- by one instruction, which a loop
 * may run again, or by MOVEs of a loop variable -- and defined before read) and that a Boolean operand is all
 * ones or zero.  Values are kept as bits: a float, an integer, or a Boolean.  A loop runs its body again while
 * LOOP_END's Boolean is true (one invocation: one channel).
 */
/*
 * ws075-p023: runs the IR with the skippable regions the code generator proves safe (i915_compile_skips()) really
 * skipped where their predicate is false for the one channel, every value such a skipped region would have made
 * set to `poison` -- `skipping` 0 runs every region.
 */
static unsigned skip_regions_run, skip_regions_skipped;

static void
run_ir_mode(const struct drv_gpu_shader_ir *ir, struct machine *m, const uint32_t *skip_ok, int skipping, uint32_t poison)
{
	uint32_t *value = calloc(ir->value_count + 4U, sizeof(*value));
	uint32_t *def_at = calloc(ir->value_count + 4U, sizeof(*def_at));
	uint8_t *defined = calloc(ir->value_count + 4U, 1U);
	unsigned loop_top[16], loop_depth = 0U, passes = 0U;
	unsigned index, k;

	assert(value != NULL && defined != NULL && def_at != NULL);
	memset(m->output, 0, sizeof(m->output));
	memset(m->written, 0, sizeof(m->written));
	m->killed = 0;
	for (index = 0U; index < ir->instruction_count; index++) {
		const struct drv_gpu_shader_ir_inst *inst = &ir->instructions[index];
		unsigned sources = 0U, results = 1U, slot;
		uint32_t a, b, c;
		float fa, fb, rgba[4];

		/* a skippable region the code generator skips, entered with the predicate false: jumped over, its values poisoned */
		if (inst->op == DRV_GPU_IR_SKIP_BEGIN) {
			assert(inst->src[0] < ir->value_count && defined[inst->src[0]] != 0U);
			if (skipping && skip_ok[index] != 0U && value[inst->src[0]] == 0U) {
				unsigned end = index + 1U;

				/* every value made up to the region's end is poison */
				while (ir->instructions[end].op != DRV_GPU_IR_SKIP_END) {
					unsigned made = i915_compile_results(&ir->instructions[end]);

					/* its results */
					for (k = 0U; k < made; k++) {
						value[ir->instructions[end].dst + k] = poison;
						if (defined[ir->instructions[end].dst + k] == 0U)
							def_at[ir->instructions[end].dst + k] = end;
						defined[ir->instructions[end].dst + k] = 1U;
					}

					/* the next instruction of the region */
					end++;
				}

				/* resumes at the SKIP_END */
				skip_regions_skipped++;
				index = end;
			}

			continue;
		}

		/* a region's end does nothing */
		if (inst->op == DRV_GPU_IR_SKIP_END)
			continue;

		switch (inst->op) {
		case DRV_GPU_IR_STORE_OUTPUT: sources = 1U; results = 0U; break;
		case DRV_GPU_IR_KILL: sources = 1U; results = 0U; break;
		case DRV_GPU_IR_LOOP_END: sources = 1U; results = 0U; break;
		case DRV_GPU_IR_LOOP_BEGIN: results = 0U; break;
		case DRV_GPU_IR_FNEG: case DRV_GPU_IR_SIN: case DRV_GPU_IR_COS: case DRV_GPU_IR_RSQ:
		case DRV_GPU_IR_RCP: case DRV_GPU_IR_SQRT: case DRV_GPU_IR_EXP2: case DRV_GPU_IR_LOG2:
		case DRV_GPU_IR_FABS: case DRV_GPU_IR_FLOOR: case DRV_GPU_IR_FRACT: case DRV_GPU_IR_NOT:
		case DRV_GPU_IR_FTRUNC: case DRV_GPU_IR_INEG: case DRV_GPU_IR_INOT: case DRV_GPU_IR_I2F: case DRV_GPU_IR_U2F:
		case DRV_GPU_IR_F2I: case DRV_GPU_IR_F2U: case DRV_GPU_IR_MOVE: case DRV_GPU_IR_FROUND_EVEN:
		case DRV_GPU_IR_UNPACK_HALF:
			sources = 1U; break;
		case DRV_GPU_IR_FADD: case DRV_GPU_IR_FSUB: case DRV_GPU_IR_FMUL: case DRV_GPU_IR_FMIN: case DRV_GPU_IR_FMAX:
		case DRV_GPU_IR_FLT: case DRV_GPU_IR_FGE: case DRV_GPU_IR_FEQ: case DRV_GPU_IR_FNEU: case DRV_GPU_IR_AND: case DRV_GPU_IR_OR:
		case DRV_GPU_IR_IADD: case DRV_GPU_IR_ISUB: case DRV_GPU_IR_IMUL: case DRV_GPU_IR_UDIV: case DRV_GPU_IR_UMOD:
		case DRV_GPU_IR_IDIV: case DRV_GPU_IR_IREM:
		case DRV_GPU_IR_IAND: case DRV_GPU_IR_IOR: case DRV_GPU_IR_IXOR: case DRV_GPU_IR_SHL: case DRV_GPU_IR_SHR: case DRV_GPU_IR_ASR:
		case DRV_GPU_IR_ILT: case DRV_GPU_IR_IGE: case DRV_GPU_IR_ULT: case DRV_GPU_IR_UGE: case DRV_GPU_IR_IEQ: case DRV_GPU_IR_INE:
		case DRV_GPU_IR_PACK_HALF:
			sources = 2U; break;
		case DRV_GPU_IR_SELECT: sources = 3U; break;
		case DRV_GPU_IR_SAMPLE: sources = 2U; results = 4U; break;
		case DRV_GPU_IR_TEXTURE:
			/* a run of src[1] parameters from src[0], each defined before */
			assert(inst->src[1] >= 1U && inst->src[1] <= DRV_GPU_IR_TEXTURE_MAX_PARAMS);
			for (k = 0U; k < inst->src[1]; k++)
				assert(inst->src[0] + k < ir->value_count && defined[inst->src[0] + k] != 0U);
			results = 4U;
			break;
		case DRV_GPU_IR_CONST: case DRV_GPU_IR_BOOL: case DRV_GPU_IR_ICONST: case DRV_GPU_IR_LOAD_INPUT: case DRV_GPU_IR_LOAD_PUSH:
		case DRV_GPU_IR_LOAD_UBO:
			break;
		default: assert(!"IR operation the interpreter does not know"); break;
		}
		for (k = 0U; k < sources; k++)
			assert(inst->src[k] < ir->value_count && defined[inst->src[k]] != 0U);
		for (k = 0U; k < results; k++) {
			assert(inst->dst + k < ir->value_count);
			/* defined once; again only by the same instruction (a loop's next pass) or by a MOVE */
			assert(defined[inst->dst + k] == 0U || def_at[inst->dst + k] == index || inst->op == DRV_GPU_IR_MOVE);
			if (defined[inst->dst + k] == 0U)
				def_at[inst->dst + k] = index;
			defined[inst->dst + k] = 1U;
		}
		a = sources >= 1U ? value[inst->src[0]] : 0U;
		b = sources >= 2U ? value[inst->src[1]] : 0U;
		c = sources >= 3U ? value[inst->src[2]] : 0U;
		fa = fbits(a);
		fb = fbits(b);

		/* the logic operations, SELECT's condition, KILL and LOOP_END read Booleans only */
		if (inst->op == DRV_GPU_IR_AND || inst->op == DRV_GPU_IR_OR)
			assert((a == 0U || a == 0xFFFFFFFFU) && (b == 0U || b == 0xFFFFFFFFU));
		if (inst->op == DRV_GPU_IR_NOT || inst->op == DRV_GPU_IR_SELECT || inst->op == DRV_GPU_IR_KILL ||
		    inst->op == DRV_GPU_IR_LOOP_END)
			assert(a == 0U || a == 0xFFFFFFFFU);
		if (inst->op == DRV_GPU_IR_BOOL)
			assert(inst->immediate == 0U || inst->immediate == 0xFFFFFFFFU);

		switch (inst->op) {
		case DRV_GPU_IR_CONST: value[inst->dst] = inst->immediate; break;
		case DRV_GPU_IR_BOOL: value[inst->dst] = inst->immediate; break;
		case DRV_GPU_IR_ICONST: value[inst->dst] = inst->immediate; break;
		case DRV_GPU_IR_LOAD_INPUT:
			if (inst->location == DRV_GPU_SHADER_LOCATION_FRAG_COORD) {
				assert(inst->component < 4U);
				value[inst->dst] = float_to_bits(m->frag_coord[inst->component]);
				break;
			}
			assert(inst->location < SLOTS && inst->component < 4U);
			value[inst->dst] = float_to_bits(m->input[inst->location][inst->component]);
			break;
		case DRV_GPU_IR_PACK_HALF: {
			_Float16 low = (_Float16)fa, high = (_Float16)fb;
			uint16_t low_bits, high_bits;

			memcpy(&low_bits, &low, 2U);
			memcpy(&high_bits, &high, 2U);
			value[inst->dst] = (uint32_t)low_bits | ((uint32_t)high_bits << 16);
			break;
		}
		case DRV_GPU_IR_UNPACK_HALF: {
			uint16_t half_bits = (uint16_t)(a >> (16U * (inst->component & 1U)));
			_Float16 half;

			memcpy(&half, &half_bits, 2U);
			value[inst->dst] = float_to_bits((float)half);
			break;
		}
		case DRV_GPU_IR_LOAD_PUSH:
			assert(inst->immediate + 4U <= sizeof(m->push) && inst->immediate + 4U <= ir->push_bytes);
			memcpy(&value[inst->dst], m->push + inst->immediate, 4U);
			break;
		case DRV_GPU_IR_LOAD_UBO:
			assert(inst->location < ir->uniform_count && ir->uniforms[inst->location].kind == DRV_GPU_IR_UNIFORM_BLOCK);
			assert(inst->location < 4U && inst->immediate + 4U <= sizeof(m->ubo[0]));
			assert(inst->immediate >= ir->uniforms[inst->location].offset &&
			       inst->immediate + 4U <= ir->uniforms[inst->location].offset + ir->uniforms[inst->location].size);
			memcpy(&value[inst->dst], m->ubo[inst->location] + inst->immediate, 4U);
			break;
		case DRV_GPU_IR_STORE_OUTPUT:
			slot = inst->location == DRV_GPU_IR_LOCATION_POSITION ? SLOT_POSITION : inst->location;
			assert(slot < SLOTS && inst->component < 4U);
			assert(inst->location == DRV_GPU_IR_LOCATION_POSITION || inst->location < SLOT_POSITION);
			m->output[slot][inst->component] = fa;
			m->written[slot][inst->component]++;
			break;
		case DRV_GPU_IR_KILL: if (a != 0U) m->killed = 1; break;
		case DRV_GPU_IR_FADD: value[inst->dst] = float_to_bits(fa + fb); break;
		case DRV_GPU_IR_FSUB: value[inst->dst] = float_to_bits(fa - fb); break;
		case DRV_GPU_IR_FMUL: value[inst->dst] = float_to_bits(fa * fb); break;
		case DRV_GPU_IR_FNEG: value[inst->dst] = float_to_bits(-fa); break;
		case DRV_GPU_IR_SIN: value[inst->dst] = float_to_bits(sinf(fa)); break;
		case DRV_GPU_IR_COS: value[inst->dst] = float_to_bits(cosf(fa)); break;
		case DRV_GPU_IR_RSQ: value[inst->dst] = float_to_bits(1.0f / sqrtf(fa)); break;
		case DRV_GPU_IR_RCP: value[inst->dst] = float_to_bits(1.0f / fa); break;
		case DRV_GPU_IR_SQRT: value[inst->dst] = float_to_bits(sqrtf(fa)); break;
		case DRV_GPU_IR_EXP2: value[inst->dst] = float_to_bits(exp2f(fa)); break;
		case DRV_GPU_IR_LOG2: value[inst->dst] = float_to_bits(log2f(fa)); break;
		case DRV_GPU_IR_FABS: value[inst->dst] = float_to_bits(fabsf(fa)); break;
		case DRV_GPU_IR_FLOOR: value[inst->dst] = float_to_bits(floorf(fa)); break;
		case DRV_GPU_IR_FRACT: value[inst->dst] = float_to_bits(fa - floorf(fa)); break;
		case DRV_GPU_IR_FTRUNC: value[inst->dst] = float_to_bits(truncf(fa)); break;
		case DRV_GPU_IR_FMIN: value[inst->dst] = float_to_bits(fa < fb ? fa : fb); break;
		case DRV_GPU_IR_FMAX: value[inst->dst] = float_to_bits(fa >= fb ? fa : fb); break;
		case DRV_GPU_IR_FLT: value[inst->dst] = boolean(fa < fb); break;
		case DRV_GPU_IR_FGE: value[inst->dst] = boolean(fa >= fb); break;
		case DRV_GPU_IR_FEQ: value[inst->dst] = boolean(fa == fb); break;
		case DRV_GPU_IR_FNEU: value[inst->dst] = boolean(fa != fb); break;
		case DRV_GPU_IR_AND: value[inst->dst] = a & b; break;
		case DRV_GPU_IR_OR: value[inst->dst] = a | b; break;
		case DRV_GPU_IR_NOT: value[inst->dst] = ~a; break;
		case DRV_GPU_IR_SELECT: value[inst->dst] = a != 0U ? b : c; break;
		case DRV_GPU_IR_IADD: value[inst->dst] = a + b; break;
		case DRV_GPU_IR_ISUB: value[inst->dst] = a - b; break;
		case DRV_GPU_IR_IMUL: value[inst->dst] = a * b; break;
		case DRV_GPU_IR_INEG: value[inst->dst] = 0U - a; break;
		/*
		 * A zero divisor, and INT_MIN / -1, have no defined result (SPIR-V; ws031-p024): the interpreter gives 0
		 * where the hardware gives some value, and the tests compare only defined words.
		 */
		case DRV_GPU_IR_UDIV: value[inst->dst] = b != 0U ? a / b : 0U; break;
		case DRV_GPU_IR_UMOD: value[inst->dst] = b != 0U ? a % b : 0U; break;
		case DRV_GPU_IR_IDIV:
			value[inst->dst] = 0U;
			if (b != 0U && !(a == 0x80000000U && b == 0xFFFFFFFFU))
				value[inst->dst] = (uint32_t)((int32_t)a / (int32_t)b);
			break;
		case DRV_GPU_IR_IREM:
			value[inst->dst] = 0U;
			if (b != 0U && !(a == 0x80000000U && b == 0xFFFFFFFFU))
				value[inst->dst] = (uint32_t)((int32_t)a % (int32_t)b);
			break;
		case DRV_GPU_IR_FROUND_EVEN: value[inst->dst] = float_to_bits(nearbyintf(fa)); break;
		case DRV_GPU_IR_IAND: value[inst->dst] = a & b; break;
		case DRV_GPU_IR_IOR: value[inst->dst] = a | b; break;
		case DRV_GPU_IR_IXOR: value[inst->dst] = a ^ b; break;
		case DRV_GPU_IR_INOT: value[inst->dst] = ~a; break;
		case DRV_GPU_IR_SHL: value[inst->dst] = a << (b & 31U); break;
		case DRV_GPU_IR_SHR: value[inst->dst] = a >> (b & 31U); break;
		case DRV_GPU_IR_ASR: value[inst->dst] = (uint32_t)((int32_t)a >> (b & 31U)); break;
		case DRV_GPU_IR_I2F: value[inst->dst] = float_to_bits((float)(int32_t)a); break;
		case DRV_GPU_IR_U2F: value[inst->dst] = float_to_bits((float)a); break;
		case DRV_GPU_IR_F2I: value[inst->dst] = (uint32_t)(int32_t)fa; break;
		case DRV_GPU_IR_F2U: value[inst->dst] = (uint32_t)fa; break;
		case DRV_GPU_IR_ILT: value[inst->dst] = boolean((int32_t)a < (int32_t)b); break;
		case DRV_GPU_IR_IGE: value[inst->dst] = boolean((int32_t)a >= (int32_t)b); break;
		case DRV_GPU_IR_ULT: value[inst->dst] = boolean(a < b); break;
		case DRV_GPU_IR_UGE: value[inst->dst] = boolean(a >= b); break;
		case DRV_GPU_IR_IEQ: value[inst->dst] = boolean(a == b); break;
		case DRV_GPU_IR_INE: value[inst->dst] = boolean(a != b); break;
		case DRV_GPU_IR_MOVE: value[inst->dst] = a; break;
		case DRV_GPU_IR_LOOP_BEGIN:
			assert(loop_depth < 16U);
			loop_top[loop_depth++] = index;
			break;
		case DRV_GPU_IR_LOOP_END:
			assert(loop_depth != 0U);
			if (a != 0U) {
				assert(++passes < 100000U);
				index = loop_top[loop_depth - 1U];      /* the body again, after the LOOP_BEGIN */
			} else {
				loop_depth--;
			}
			break;
		case DRV_GPU_IR_SAMPLE:
			fake_texture(inst->location, inst->immediate, fa, fb, rgba);
			for (k = 0U; k < 4U; k++)
				value[inst->dst + k] = float_to_bits(rgba[k]);
			break;
		case DRV_GPU_IR_TEXTURE:
			/* the interpreter has no image: the reply is the message type and the parameters' first word */
			for (k = 0U; k < 4U; k++)
				value[inst->dst + k] = (inst->component & 0x1fU) + value[inst->src[0]];
			break;
		default: break;
		}
	}
	assert(loop_depth == 0U);
	free(value);
	free(def_at);
	free(defined);
}

/*
 * Runs the IR as the tests always did, then -- when the code generator skips some of its regions -- again with
 * them skipped, the skipped values poisoned with all-zero and with all-one bits: the outputs, the writes and the
 * discard must be the same bit for bit (ws075-p023).
 */
static void
run_ir(const struct drv_gpu_shader_ir *ir, struct machine *m)
{
	struct i915_compile_state state;
	struct machine again;
	uint32_t *maps;
	unsigned index, skipped = 0U, pass;
	int same;

	/* the plain run: every block runs under its predicate */
	run_ir_mode(ir, m, NULL, 0, 0U);

	/* the code generator's proof of which regions are skipped */
	memset(&state, 0, sizeof(state));
	state.ir = ir;
	maps = calloc(2U * ir->value_count + ir->instruction_count + 4U, sizeof(*maps));
	assert(maps != NULL);
	state.skip_taint = maps;
	state.skip_def = maps + ir->value_count;
	state.skip_ok = maps + 2U * ir->value_count;
	i915_compile_skips(&state);
	for (index = 0U; index < ir->instruction_count; index++)
		skipped += state.skip_ok[index] != 0U;
	skip_regions_run += skipped;
	if (skipped != 0U) {
		for (pass = 0U; pass < 2U; pass++) {
			/* poison 0, then all ones */
			again = *m;
			if (pass == 0U)
				run_ir_mode(ir, &again, state.skip_ok, 1, 0U);
			else
				run_ir_mode(ir, &again, state.skip_ok, 1, 0xFFFFFFFFU);
			same = memcmp(again.output, m->output, sizeof(m->output)) == 0;
			same &= memcmp(again.written, m->written, sizeof(m->written)) == 0;
			same &= again.killed == m->killed;
			if (!same) {
				printf("  a skipped region changed the result (poison %u)\n", pass);
				assert(!"a region the code generator skips is seen");
			}
		}
	}

	/* the proof's tables */
	free(maps);
}

/* ------------------------------------------------------------------ a tiny SPIR-V assembler */

static uint32_t mod[2048];
static unsigned mod_n;

static void
op(unsigned opcode, unsigned operands, ...)
{
	va_list list;
	unsigned k;

	assert(mod_n + 1U + operands <= sizeof(mod) / sizeof(mod[0]));
	mod[mod_n++] = ((operands + 1U) << 16) | opcode;
	va_start(list, operands);
	for (k = 0U; k < operands; k++)
		mod[mod_n++] = va_arg(list, uint32_t);
	va_end(list);
}

/* fixed ids of the common preamble */
enum {
	T_VOID = 1, T_FN, T_FLOAT, T_VEC2, T_VEC3, T_VEC4, T_PTR_FN_FLOAT, T_PTR_FN_VEC4, T_PTR_IN_VEC4,
	T_PTR_OUT_VEC4, T_INT, V_IN0, V_IN1, V_OUT0, C_I0, C_I1, C_I2, C_I3, C_F5, C_F2, C_F16, T_PTR_IN_FLOAT,
	T_PTR_OUT_FLOAT, F_MAIN, L_ENTRY, V_OUT1, T_PTR_FN_VEC3, V_DYN, T_BOOL, T_PTR_FN_BOOL,
	B = 40                          /* first body id */
};

#define U(x) ((uint32_t)(x))

/* A fragment module: in0, in1 : vec4 (locations 0, 1), out0, out1 : vec4 (locations 0, 1). */
static void
begin_module(void)
{
	mod_n = 0U;
	mod[mod_n++] = 0x07230203U; mod[mod_n++] = 0x00010000U; mod[mod_n++] = 0U; mod[mod_n++] = 256U; mod[mod_n++] = 0U;
	op(17U, 1U, U(1));                                                      /* OpCapability Shader */
	op(14U, 2U, U(0), U(1));                                                /* OpMemoryModel Logical GLSL450 */
	op(15U, 7U, U(4), U(F_MAIN), U(0x6E69616D), U(0), U(V_IN0), U(V_IN1), U(V_OUT0));  /* OpEntryPoint Fragment "main" */
	op(16U, 2U, U(F_MAIN), U(7));                                           /* OpExecutionMode OriginUpperLeft */
	op(71U, 3U, U(V_IN0), U(30), U(0));                                     /* OpDecorate Location */
	op(71U, 3U, U(V_IN1), U(30), U(1));
	op(71U, 3U, U(V_OUT0), U(30), U(0));
	op(71U, 3U, U(V_OUT1), U(30), U(1));
	op(19U, 1U, U(T_VOID));
	op(33U, 2U, U(T_FN), U(T_VOID));
	op(22U, 2U, U(T_FLOAT), U(32));
	op(23U, 3U, U(T_VEC2), U(T_FLOAT), U(2));
	op(23U, 3U, U(T_VEC3), U(T_FLOAT), U(3));
	op(23U, 3U, U(T_VEC4), U(T_FLOAT), U(4));
	op(32U, 3U, U(T_PTR_FN_FLOAT), U(7), U(T_FLOAT));
	op(32U, 3U, U(T_PTR_FN_VEC4), U(7), U(T_VEC4));
	op(32U, 3U, U(T_PTR_FN_VEC3), U(7), U(T_VEC3));
	op(32U, 3U, U(T_PTR_IN_VEC4), U(1), U(T_VEC4));
	op(32U, 3U, U(T_PTR_IN_FLOAT), U(1), U(T_FLOAT));
	op(32U, 3U, U(T_PTR_OUT_VEC4), U(3), U(T_VEC4));
	op(32U, 3U, U(T_PTR_OUT_FLOAT), U(3), U(T_FLOAT));
	op(21U, 3U, U(T_INT), U(32), U(1));
	op(20U, 1U, U(T_BOOL));                                                 /* OpTypeBool */
	op(32U, 3U, U(T_PTR_FN_BOOL), U(7), U(T_BOOL));
	op(43U, 3U, U(T_INT), U(C_I0), U(0));
	op(43U, 3U, U(T_INT), U(C_I1), U(1));
	op(43U, 3U, U(T_INT), U(C_I2), U(2));
	op(43U, 3U, U(T_INT), U(C_I3), U(3));
	op(43U, 3U, U(T_FLOAT), U(C_F5), float_to_bits(5.0f));
	op(43U, 3U, U(T_FLOAT), U(C_F2), float_to_bits(2.0f));
	op(43U, 3U, U(T_FLOAT), U(C_F16), float_to_bits(16.0f));
	op(59U, 3U, U(T_PTR_IN_VEC4), U(V_IN0), U(1));
	op(59U, 3U, U(T_PTR_IN_VEC4), U(V_IN1), U(1));
	op(59U, 3U, U(T_PTR_OUT_VEC4), U(V_OUT0), U(3));
	op(59U, 3U, U(T_PTR_OUT_VEC4), U(V_OUT1), U(3));
	op(54U, 4U, U(T_VOID), U(F_MAIN), U(0), U(T_FN));
	op(248U, 1U, U(L_ENTRY));
}

static int
end_module(struct drv_gpu_shader_ir **ir, struct drv_gpu_compile_diagnostic *diag)
{
	op(253U, 0U);
	op(56U, 0U);
	return drv_gpu_shader_parse(mod, mod_n, DRV_GPU_STAGE_FRAGMENT, ir, diag);
}

static void
set_inputs(struct machine *m)
{
	static const float in0[4] = { 1.0f, 2.0f, 4.0f, 8.0f };
	static const float in1[4] = { 5.0f, 6.0f, 7.0f, 8.0f };

	memset(m, 0, sizeof(*m));
	memcpy(m->input[0], in0, sizeof(in0));
	memcpy(m->input[1], in1, sizeof(in1));
}

static void
expect_out(const struct machine *m, unsigned slot, float x, float y, float z, float w)
{
	const float want[4] = { x, y, z, w };
	unsigned k;

	for (k = 0U; k < 4U; k++) {
		if (m->written[slot][k] != 1U || m->output[slot][k] != want[k]) {
			printf("  output %u.%u = %g (stored %u times), want %g\n", slot, k, (double)m->output[slot][k],
				m->written[slot][k], (double)want[k]);
			assert(!"output mismatch");
		}
	}
}

/* ------------------------------------------------------------------ the unit tests */

/* Function-storage local: store 5, load, overwrite with 2, load again; the two loads differ. */
static void
test_local_store_load_overwrite(void)
{
	struct drv_gpu_shader_ir *ir;
	struct drv_gpu_compile_diagnostic diag;
	struct machine m;

	begin_module();
	op(59U, 3U, U(T_PTR_FN_FLOAT), U(B), U(7));                             /* %B = OpVariable Function */
	op(62U, 2U, U(B), U(C_F5));                                             /* store 5 */
	op(61U, 3U, U(T_FLOAT), U(B + 1), U(B));                                /* first  = load  -> 5 */
	op(62U, 2U, U(B), U(C_F2));                                             /* store 2 (overwrite) */
	op(61U, 3U, U(T_FLOAT), U(B + 2), U(B));                                /* second = load  -> 2 */
	op(131U, 4U, U(T_FLOAT), U(B + 3), U(B + 1), U(B + 2));                 /* 5 - 2 =  3 */
	op(131U, 4U, U(T_FLOAT), U(B + 4), U(B + 2), U(B + 1));                 /* 2 - 5 = -3 */
	op(80U, 6U, U(T_VEC4), U(B + 5), U(B + 1), U(B + 2), U(B + 3), U(B + 4));
	op(62U, 2U, U(V_OUT0), U(B + 5));
	assert(end_module(&ir, &diag) == 0);
	set_inputs(&m);
	run_ir(ir, &m);
	expect_out(&m, 0U, 5.0f, 2.0f, 3.0f, -3.0f);
	drv_gpu_shader_ir_free(ir);
	printf("  local: store 5 / load / store 2 / load -> (5, 2), 5-2 = 3, 2-5 = -3\n");
}

/* (1,2,4,8): every component extracted on its own, none dropped or duplicated; construct and shuffle. */
static void
test_vector_construct_extract_shuffle(void)
{
	struct drv_gpu_shader_ir *ir;
	struct drv_gpu_compile_diagnostic diag;
	struct machine m;

	begin_module();
	op(61U, 3U, U(T_VEC4), U(B), U(V_IN0));                                 /* v = (1,2,4,8) */
	op(81U, 4U, U(T_FLOAT), U(B + 1), U(B), U(0));
	op(81U, 4U, U(T_FLOAT), U(B + 2), U(B), U(1));
	op(81U, 4U, U(T_FLOAT), U(B + 3), U(B), U(2));
	op(81U, 4U, U(T_FLOAT), U(B + 4), U(B), U(3));
	op(80U, 6U, U(T_VEC4), U(B + 5), U(B + 4), U(B + 3), U(B + 2), U(B + 1)); /* reversed: (8,4,2,1) */
	op(62U, 2U, U(V_OUT0), U(B + 5));
	/* shuffle v with in1 = (5,6,7,8): components 1, 6, 3, 4 -> (2, 7, 8, 5) */
	op(61U, 3U, U(T_VEC4), U(B + 6), U(V_IN1));
	op(79U, 8U, U(T_VEC4), U(B + 7), U(B), U(B + 6), U(1), U(6), U(3), U(4));
	/* vec2 from the shuffle (components 2, 0 -> (8, 2)), then vec4(vec2, scalar, scalar) = (8, 2, 16, 4) */
	op(79U, 6U, U(T_VEC2), U(B + 8), U(B + 7), U(B + 7), U(2), U(0));
	op(80U, 5U, U(T_VEC4), U(B + 9), U(B + 8), U(C_F16), U(B + 3));
	op(129U, 4U, U(T_VEC4), U(B + 10), U(B + 7), U(B + 9));                 /* (2,7,8,5) + (8,2,16,4) = (10,9,24,9) */
	op(62U, 2U, U(V_OUT1), U(B + 10));
	assert(end_module(&ir, &diag) == 0);
	set_inputs(&m);
	run_ir(ir, &m);
	expect_out(&m, 0U, 8.0f, 4.0f, 2.0f, 1.0f);
	expect_out(&m, 1U, 10.0f, 9.0f, 24.0f, 9.0f);
	drv_gpu_shader_ir_free(ir);
	printf("  vector: (1,2,4,8) extracted, reversed, shuffled and rebuilt without losing a component\n");
}

/* Component access chains on a local, an input and an output. */
static void
test_component_access(void)
{
	struct drv_gpu_shader_ir *ir;
	struct drv_gpu_compile_diagnostic diag;
	struct machine m;

	begin_module();
	op(59U, 3U, U(T_PTR_FN_VEC4), U(B), U(7));                              /* local vec4 */
	op(61U, 3U, U(T_VEC4), U(B + 1), U(V_IN0));
	op(62U, 2U, U(B), U(B + 1));                                            /* local = (1,2,4,8) */
	op(65U, 4U, U(T_PTR_FN_FLOAT), U(B + 2), U(B), U(C_I2));                /* &local.z */
	op(62U, 2U, U(B + 2), U(C_F16));                                        /* local.z = 16 */
	op(61U, 3U, U(T_VEC4), U(B + 3), U(B));                                 /* (1,2,16,8) */
	op(62U, 2U, U(V_OUT0), U(B + 3));
	op(65U, 4U, U(T_PTR_FN_FLOAT), U(B + 4), U(B), U(C_I1));                /* &local.y */
	op(61U, 3U, U(T_FLOAT), U(B + 5), U(B + 4));                            /* 2 */
	op(65U, 4U, U(T_PTR_IN_FLOAT), U(B + 6), U(V_IN1), U(C_I3));            /* &in1.w */
	op(61U, 3U, U(T_FLOAT), U(B + 7), U(B + 6));                            /* 8 */
	op(65U, 4U, U(T_PTR_IN_FLOAT), U(B + 8), U(V_IN1), U(C_I0));            /* &in1.x */
	op(61U, 3U, U(T_FLOAT), U(B + 9), U(B + 8));                            /* 5 */
	op(133U, 4U, U(T_FLOAT), U(B + 10), U(B + 5), U(B + 7));                /* 2 * 8 = 16 */
	/* the output written one component at a time, out of order */
	op(65U, 4U, U(T_PTR_OUT_FLOAT), U(B + 11), U(V_OUT1), U(C_I3));
	op(62U, 2U, U(B + 11), U(B + 5));                                       /* out1.w = 2 */
	op(65U, 4U, U(T_PTR_OUT_FLOAT), U(B + 12), U(V_OUT1), U(C_I0));
	op(62U, 2U, U(B + 12), U(B + 7));                                       /* out1.x = 8 */
	op(65U, 4U, U(T_PTR_OUT_FLOAT), U(B + 13), U(V_OUT1), U(C_I1));
	op(62U, 2U, U(B + 13), U(B + 9));                                       /* out1.y = 5 */
	op(65U, 4U, U(T_PTR_OUT_FLOAT), U(B + 14), U(V_OUT1), U(C_I2));
	op(62U, 2U, U(B + 14), U(B + 10));                                      /* out1.z = 16 */
	assert(end_module(&ir, &diag) == 0);
	set_inputs(&m);
	run_ir(ir, &m);
	expect_out(&m, 0U, 1.0f, 2.0f, 16.0f, 8.0f);
	expect_out(&m, 1U, 8.0f, 5.0f, 16.0f, 2.0f);
	drv_gpu_shader_ir_free(ir);
	printf("  access chain: one component of a local overwritten, inputs / outputs addressed by component\n");
}

/* dot((1,2,3,4),(5,6,7,8)) = 70; negate; vector * scalar; float constants as operands. */
static void
test_dot_negate_scale(void)
{
	struct drv_gpu_shader_ir *ir;
	struct drv_gpu_compile_diagnostic diag;
	struct machine m;
	static const float a[4] = { 1.0f, 2.0f, 3.0f, 4.0f };

	begin_module();
	op(61U, 3U, U(T_VEC4), U(B), U(V_IN0));
	op(61U, 3U, U(T_VEC4), U(B + 1), U(V_IN1));
	op(148U, 4U, U(T_FLOAT), U(B + 2), U(B), U(B + 1));                     /* 70 */
	op(127U, 3U, U(T_FLOAT), U(B + 3), U(B + 2));                           /* -70 */
	op(131U, 4U, U(T_FLOAT), U(B + 4), U(B + 2), U(C_F5));                  /* 70 - 5 = 65 */
	op(131U, 4U, U(T_FLOAT), U(B + 5), U(C_F5), U(B + 2));                  /* 5 - 70 = -65 */
	op(80U, 6U, U(T_VEC4), U(B + 6), U(B + 2), U(B + 3), U(B + 4), U(B + 5));
	op(62U, 2U, U(V_OUT0), U(B + 6));
	op(142U, 4U, U(T_VEC4), U(B + 7), U(B), U(C_F2));                       /* (1,2,3,4) * 2 */
	op(127U, 3U, U(T_VEC4), U(B + 8), U(B + 7));                            /* -(2,4,6,8) */
	op(133U, 4U, U(T_VEC4), U(B + 9), U(B + 8), U(B + 1));                  /* * (5,6,7,8) = (-10,-24,-42,-64) */
	op(62U, 2U, U(V_OUT1), U(B + 9));
	assert(end_module(&ir, &diag) == 0);
	set_inputs(&m);
	memcpy(m.input[0], a, sizeof(a));
	run_ir(ir, &m);
	expect_out(&m, 0U, 70.0f, -70.0f, 65.0f, -65.0f);
	expect_out(&m, 1U, -10.0f, -24.0f, -42.0f, -64.0f);
	drv_gpu_shader_ir_free(ir);
	printf("  arithmetic: dot = 70, negate = -70, 70-5 = 65, 5-70 = -65, vector * scalar per component\n");
}

/* What stays refused -- each with the instruction named, none skipped. */
static void
test_refusals(void)
{
	struct drv_gpu_shader_ir *ir;
	struct drv_gpu_compile_diagnostic diag;

	/* a load of a local (component) that was never stored */
	begin_module();
	op(59U, 3U, U(T_PTR_FN_FLOAT), U(B), U(7));
	op(61U, 3U, U(T_FLOAT), U(B + 1), U(B));
	assert(end_module(&ir, &diag) == ENOTSUP && ir == NULL && diag.opcode == 61U);

	/* only ONE component stored, then the whole vector loaded */
	begin_module();
	op(59U, 3U, U(T_PTR_FN_VEC4), U(B), U(7));
	op(65U, 4U, U(T_PTR_FN_FLOAT), U(B + 1), U(B), U(C_I1));
	op(62U, 2U, U(B + 1), U(C_F5));
	op(61U, 3U, U(T_VEC4), U(B + 2), U(B));
	assert(end_module(&ir, &diag) == ENOTSUP && ir == NULL && diag.opcode == 61U);

	/* a dynamic access-chain index (a loaded value, not a constant) */
	begin_module();
	op(59U, 3U, U(T_PTR_FN_VEC4), U(B), U(7));
	op(61U, 3U, U(T_VEC4), U(B + 1), U(V_IN0));
	op(65U, 4U, U(T_PTR_FN_FLOAT), U(B + 2), U(B), U(B + 1));
	assert(end_module(&ir, &diag) == ENOTSUP && ir == NULL && diag.opcode == 65U);

	/* a local with an initializer */
	begin_module();
	op(59U, 4U, U(T_PTR_FN_FLOAT), U(B), U(7), U(C_F5));
	assert(end_module(&ir, &diag) == ENOTSUP && ir == NULL && diag.opcode == 59U);

	/* a store of a vec4 into a float */
	begin_module();
	op(59U, 3U, U(T_PTR_FN_FLOAT), U(B), U(7));
	op(61U, 3U, U(T_VEC4), U(B + 1), U(V_IN0));
	op(62U, 2U, U(B), U(B + 1));
	assert(end_module(&ir, &diag) == ENOTSUP && ir == NULL && diag.opcode == 62U);

	/* a store through an INPUT pointer */
	begin_module();
	op(61U, 3U, U(T_VEC4), U(B + 1), U(V_IN0));
	op(62U, 2U, U(V_IN1), U(B + 1));
	assert(end_module(&ir, &diag) == ENOTSUP && ir == NULL && diag.opcode == 62U);

	/* composite whose constituents do not fill the vector: malformed, not "unsupported" */
	begin_module();
	op(80U, 4U, U(T_VEC4), U(B), U(C_F5), U(C_F2));
	assert(end_module(&ir, &diag) == EINVAL && ir == NULL);

	/*
	 * still not lowered: a branch back to a block already lowered that is no loop's header, a return inside a
	 * loop (the entry block made a loop header whose merge block never comes).  OpSwitch is lowered (ws075-p009,
	 * test_switch)
	 */
	begin_module();
	op(249U, 1U, U(L_ENTRY));
	assert(end_module(&ir, &diag) == ENOTSUP && diag.opcode == 249U);
	begin_module();
	op(246U, 3U, U(B), U(B + 1), U(0));
	assert(end_module(&ir, &diag) == ENOTSUP && diag.opcode == 253U);
	/* OpKill outside a fragment shader: the entry point's execution model made Vertex */
	begin_module();
	mod[5U + 2U + 3U + 1U] = 0U;
	op(252U, 0U);
	op(56U, 0U);
	assert(drv_gpu_shader_parse(mod, mod_n, DRV_GPU_STAGE_VERTEX, &ir, &diag) == ENOTSUP && ir == NULL && diag.opcode == 252U);

	/* a decoration that would place data (Component = 31) is not ignored (Flat is kept for the draw, ws075-p004) */
	begin_module();
	/* the decoration goes before the function: lift the OpFunction / OpLabel tail and put it back */
	{
		unsigned function_at = mod_n - 7U;      /* OpFunction (5 words) + OpLabel (2 words) */
		uint32_t tail[7];

		memcpy(tail, mod + function_at, sizeof(tail));
		mod_n = function_at;
		op(71U, 3U, U(V_IN0), U(31), U(0));     /* OpDecorate %in0 Component 0 */
		memcpy(mod + mod_n, tail, sizeof(tail));
		mod_n += 7U;
	}
	assert(end_module(&ir, &diag) == ENOTSUP && ir == NULL && diag.opcode == 71U);
	printf("  refusals: unset local, partial local, dynamic index, initializer, size mismatch, input store, back edge, return in a loop, vertex OpKill, Component\n");
}

/* RelaxedPrecision (no effect on this lowering) is accepted by name. */
static void
test_harmless_decoration(void)
{
	struct drv_gpu_shader_ir *ir;
	struct drv_gpu_compile_diagnostic diag;
	unsigned function_at;
	uint32_t tail[7];

	begin_module();
	function_at = mod_n - 7U;
	memcpy(tail, mod + function_at, sizeof(tail));
	mod_n = function_at;
	op(71U, 2U, U(V_IN0), U(0));                    /* OpDecorate %in0 RelaxedPrecision */
	memcpy(mod + mod_n, tail, sizeof(tail));
	mod_n += 7U;
	assert(end_module(&ir, &diag) == 0);
	drv_gpu_shader_ir_free(ir);

	/*
	 * Flat and Centroid (ws075-p004) are accepted.  The decorations go after
	 * the variables here, so Flat does not reach the input record; that it
	 * does in a real module is checked by the draws on the hardware.
	 */
	begin_module();
	function_at = mod_n - 7U;
	memcpy(tail, mod + function_at, sizeof(tail));
	mod_n = function_at;
	op(71U, 2U, U(V_IN0), U(14));                   /* OpDecorate %in0 Flat */
	op(71U, 2U, U(V_IN0), U(16));                   /* OpDecorate %in0 Centroid */
	memcpy(mod + mod_n, tail, sizeof(tail));
	mod_n += 7U;
	assert(end_module(&ir, &diag) == 0);
	drv_gpu_shader_ir_free(ir);
}

/* ------------------------------------------------------------------ the fixed vkdemo shaders */

static uint32_t *
load_spv(const char *name, size_t *words)
{
	char path[512];
	FILE *file;
	long size;
	uint32_t *code;

	snprintf(path, sizeof(path), "%s/userland/tests/vkdemo/shaders/%s", VK_REPO, name);
	file = fopen(path, "rb");
	assert(file != NULL);
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	assert(size > 0 && (size % 4) == 0);
	code = malloc((size_t)size);
	assert(code != NULL);
	assert(fread(code, 1, (size_t)size, file) == (size_t)size);
	fclose(file);
	*words = (size_t)size / 4U;
	return code;
}

static int
close_to(float got, float want)
{
	float scale = fabsf(want) > 1.0f ? fabsf(want) : 1.0f;

	return fabsf(got - want) <= 2e-6f * scale;
}

/*
 * cuboid.vert as shipped (glslang -O0, not simplified), against the GLSL source evaluated
 * here in C.  Several vertices and times, so that a formula that is wrong in one term
 * cannot pass.
 */
static void
test_vkdemo_vertex_shader(void)
{
	static const float vertices[4][5] = {
		{ 0.5f, -1.25f, 2.0f, 0.25f, 0.75f },
		{ -1.0f, 1.0f, -1.0f, 0.0f, 1.0f },
		{ 1.0f, 0.5f, 0.75f, 1.0f, 0.0f },
		{ -0.3f, -0.7f, 1.9f, 0.125f, 0.625f },
	};
	static const float times[3] = { 0.0f, 1.7f, 12.34f };
	uint32_t *code;
	size_t words;
	struct drv_gpu_shader_ir *ir;
	struct drv_gpu_compile_diagnostic diag;
	struct machine m;
	unsigned v, t, k;
	int error;

	code = load_spv("cuboid.vert.spv", &words);
	error = drv_gpu_shader_parse(code, words, DRV_GPU_STAGE_VERTEX, &ir, &diag);
	if (error != 0)
		printf("  cuboid.vert.spv refused: opcode %u at word %u: %s\n", diag.opcode, diag.word_offset,
			diag.reason != NULL ? diag.reason : "-");
	assert(error == 0);
	assert(ir->stage == DRV_GPU_STAGE_VERTEX && ir->push_bytes == 4U);
	assert(ir->input_count == 2U && ir->output_count == 1U);

	for (v = 0U; v < 4U; v++) {
		for (t = 0U; t < 3U; t++) {
			const float *p = vertices[v];
			float seconds = times[t];
			float angle_x = 0.30f + 0.43f * seconds, angle_y = 0.40f + 0.70f * seconds;
			float sx = sinf(angle_x), cx = cosf(angle_x), sy = sinf(angle_y), cy = cosf(angle_y);
			float rx = p[0], ry = cx * p[1] - sx * p[2], rz = sx * p[1] + cx * p[2];
			float vx = cy * rx + sy * rz, vy = ry, vz = -sy * rx + cy * rz + 3.0f;
			float want[4];

			want[0] = 1.2f * vx;
			want[1] = -1.6f * vy;
			want[2] = (10.0f / 9.9f) * vz - (1.0f / 9.9f);
			want[3] = vz;

			memset(&m, 0, sizeof(m));
			memcpy(m.input[0], p, 3U * sizeof(float));
			m.input[0][3] = 1234.5f;                /* not an attribute component: must not be read */
			memcpy(m.input[1], p + 3, 2U * sizeof(float));
			m.input[1][2] = m.input[1][3] = -999.0f;
			memcpy(m.push, &seconds, 4U);
			run_ir(ir, &m);
			for (k = 0U; k < 4U; k++) {
				if (m.written[SLOT_POSITION][k] != 1U || !close_to(m.output[SLOT_POSITION][k], want[k])) {
					printf("  vertex %u time %g: gl_Position.%u = %.9g want %.9g\n", v, (double)seconds, k,
						(double)m.output[SLOT_POSITION][k], (double)want[k]);
					assert(!"gl_Position mismatch");
				}
			}
			assert(m.written[0][0] == 1U && m.written[0][1] == 1U && m.written[0][2] == 0U && m.written[0][3] == 0U);
			assert(m.output[0][0] == p[3] && m.output[0][1] == p[4]);
		}
	}
	printf("  cuboid.vert.spv (-O0, as shipped): %u IR instructions, %u values; gl_Position and texture_coordinate match the GLSL source for 4 vertices x 3 times\n",
		ir->instruction_count, ir->value_count);
	drv_gpu_shader_ir_free(ir);
	free(code);
}

static void
test_vkdemo_fragment_shader(void)
{
	uint32_t *code;
	size_t words;
	struct drv_gpu_shader_ir *ir;
	struct machine m;
	float want[4];

	code = load_spv("cuboid.frag.spv", &words);
	assert(drv_gpu_shader_parse(code, words, DRV_GPU_STAGE_FRAGMENT, &ir, NULL) == 0);
	assert(ir->uniform_count == 1U && ir->uniforms[0].set == 0U && ir->uniforms[0].binding == 0U);
	memset(&m, 0, sizeof(m));
	m.input[0][0] = 0.3f;
	m.input[0][1] = 0.6f;
	run_ir(ir, &m);
	fake_texture(0U, 0U, 0.3f, 0.6f, want);
	expect_out(&m, 0U, want[0], want[1], want[2], want[3]);
	printf("  cuboid.frag.spv: texture(checker, texture_coordinate) with u, v in the right order, 4 components out\n");
	drv_gpu_shader_ir_free(ir);
	free(code);
}

/* ------------------------------------------------------------------ p014 stage C: control flow, comparisons, GLSL.std.450 */

/* Loads a .spv file under the repository into a word buffer the caller frees. */
static uint32_t *
load_spv_at(const char *directory, const char *name, size_t *words)
{
	char path[512];
	FILE *file;
	long size;
	uint32_t *code;

	snprintf(path, sizeof(path), "%s/%s/%s", VK_REPO, directory, name);
	file = fopen(path, "rb");
	if (file == NULL)
		printf("  cannot open %s\n", path);
	assert(file != NULL);
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	assert(size > 0 && (size % 4) == 0);
	code = malloc((size_t)size);
	assert(code != NULL);
	assert(fread(code, 1, (size_t)size, file) == (size_t)size);
	fclose(file);
	*words = (size_t)size / 4U;
	return code;
}

#define COMPILER_SHADERS "src/drivers/gpu/i915/tests/render/compiler-shaders"
#define MVIEW_SHADERS "userland/tests/mview/shaders"
#define FEATURE_SHADERS "src/drivers/gpu/i915/tests/render/feature-shaders"

/* Parses a shader file, printing a refusal before failing. */
static struct drv_gpu_shader_ir *
parse_file(const char *directory, const char *name, enum drv_gpu_shader_stage stage)
{
	struct drv_gpu_shader_ir *ir;
	struct drv_gpu_compile_diagnostic diag;
	uint32_t *code;
	size_t words;
	int error;

	code = load_spv_at(directory, name, &words);
	error = drv_gpu_shader_parse(code, words, stage, &ir, &diag);
	if (error != 0)
		printf("  %s refused (%d): opcode %u at word %u: %s\n", name, error, diag.opcode, diag.word_offset,
			diag.reason != NULL ? diag.reason : "-");
	assert(error == 0);
	free(code);
	return ir;
}

/* Runs a fragment IR whose one input, location 0, is v. */
static void
run_fragment(const struct drv_gpu_shader_ir *ir, struct machine *m, const float v[4])
{
	memset(m, 0, sizeof(*m));
	memcpy(m->input[0], v, 4U * sizeof(float));
	run_ir(ir, m);
}

/* Compares output 0 with four floats bit for bit (NaN never expected). */
static void
expect_color(const struct machine *m, const char *what, const float v[4], const float want[4])
{
	unsigned k;

	for (k = 0U; k < 4U; k++) {
		if (float_to_bits(m->output[0][k]) != float_to_bits(want[k])) {
			printf("  %s at (%g, %g, %g, %g): colour.%u = %.9g want %.9g\n", what, (double)v[0], (double)v[1],
				(double)v[2], (double)v[3], k, (double)m->output[0][k], (double)want[k]);
			assert(!"output mismatch");
		}
	}
}

/*
 * The inputs every GLSL.std.450 check runs over: x of both signs, integer and not, y > 0 for the
 * functions defined there only, z over the range of exp2, w over mix weights and exponents.
 */
static const float math_inputs[][4] = {
	{ 0.0f, 1.0f, 0.0f, 0.0f }, { 1.0f, 2.0f, 1.0f, 1.0f }, { -1.0f, 0.5f, -1.0f, 0.5f },
	{ 2.75f, 3.0f, 4.5f, -2.0f }, { -2.75f, 0.1f, -6.25f, 1.5f }, { 7.5f, 100.0f, 10.0f, 0.25f },
	{ -0.3f, 0.01f, -0.3f, 3.0f }, { 0.75f, 1.5f, 2.0f, -0.5f }, { -8.0f, 42.0f, -9.5f, 0.75f },
	{ 5.0f, 5.0f, 5.0f, 2.5f }, { 0.25f, 4096.0f, 0.001f, -1.25f },
};

/* abs, floor, fract, sqrt, exp2, log2, pow, inversesqrt, min, max, clamp, mix, division, normalize. */
static void
test_glsl_std450_lowering(void)
{
	struct drv_gpu_shader_ir *unary, *exponent, *minmax, *divide;
	struct machine m;
	unsigned i;

	unary = parse_file(COMPILER_SHADERS, "unary.frag.spv", DRV_GPU_STAGE_FRAGMENT);
	exponent = parse_file(COMPILER_SHADERS, "exponent.frag.spv", DRV_GPU_STAGE_FRAGMENT);
	minmax = parse_file(COMPILER_SHADERS, "minmax.frag.spv", DRV_GPU_STAGE_FRAGMENT);
	divide = parse_file(COMPILER_SHADERS, "divide.frag.spv", DRV_GPU_STAGE_FRAGMENT);
	for (i = 0U; i < sizeof(math_inputs) / sizeof(math_inputs[0]); i++) {
		const float *v = math_inputs[i];
		float want[4], larger, length2, scale;

		run_fragment(unary, &m, v);
		want[0] = fabsf(v[0]);
		want[1] = floorf(v[0]);
		want[2] = v[0] - floorf(v[0]);
		want[3] = sqrtf(v[1]);
		expect_color(&m, "unary.frag", v, want);

		/* pow is exp2(log2(x) * y): the operands in that order */
		run_fragment(exponent, &m, v);
		want[0] = exp2f(v[2]);
		want[1] = log2f(v[1]);
		want[2] = exp2f(log2f(v[1]) * v[3]);
		want[3] = 1.0f / sqrtf(v[1]);
		expect_color(&m, "exponent.frag", v, want);

		/* clamp is min(max(x, lo), hi); mix is x * (1 - a) + y * a */
		run_fragment(minmax, &m, v);
		want[0] = v[0] < v[2] ? v[0] : v[2];
		want[1] = v[0] >= v[2] ? v[0] : v[2];
		larger = v[0] >= -0.5f ? v[0] : -0.5f;
		want[2] = larger < 0.75f ? larger : 0.75f;
		want[3] = v[0] * (1.0f - v[3]) + v[2] * v[3];
		expect_color(&m, "minmax.frag", v, want);

		/* a / b is a * (1 / b); normalize is v * inversesqrt(dot(v, v)) */
		run_fragment(divide, &m, v);
		length2 = v[0] * v[0] + v[1] * v[1];
		length2 = length2 + v[2] * v[2];
		scale = 1.0f / sqrtf(length2);
		want[0] = v[0] * (1.0f / v[1]);
		want[1] = 1.0f * (1.0f / v[2]);
		want[2] = v[0] * scale;
		want[3] = v[2] * scale;
		if (v[2] != 0.0f)
			expect_color(&m, "divide.frag", v, want);
	}
	drv_gpu_shader_ir_free(unary);
	drv_gpu_shader_ir_free(exponent);
	drv_gpu_shader_ir_free(minmax);
	drv_gpu_shader_ir_free(divide);
	printf("  GLSL.std.450: abs floor fract sqrt exp2 log2 pow inversesqrt min max clamp mix normalize, and a / b, over %u inputs\n",
		(unsigned)(sizeof(math_inputs) / sizeof(math_inputs[0])));
}

/* compare.frag: <, >, <=, >=, ==, !=, &&, || (phis), ?: (a branch and a local), !, mix(bool). */
static void
test_comparisons_and_selection(void)
{
	static const float inputs[][4] = {
		{ 1.0f, 2.0f, 5.0f, 3.0f }, { 2.0f, 1.0f, 3.0f, 5.0f }, { 3.0f, 3.0f, -1.0f, -1.0f },
		{ -4.0f, 7.0f, -9.0f, 0.5f }, { 0.0f, -0.0f, 2.0f, 1.0f }, { 0.0f, 0.0f, 0.0f, 0.0f },
	};
	struct drv_gpu_shader_ir *ir;
	struct machine m;
	unsigned i;
	float nan_value;

	ir = parse_file(COMPILER_SHADERS, "compare.frag.spv", DRV_GPU_STAGE_FRAGMENT);
	nan_value = bits_to_float(0x7FC00000U);
	for (i = 0U; i < sizeof(inputs) / sizeof(inputs[0]) + 2U; i++) {
		float v[4], want[4];

		if (i < sizeof(inputs) / sizeof(inputs[0])) {
			memcpy(v, inputs[i], sizeof(v));
		} else {
			/* a NaN on either side: every ordered test false, != true */
			v[0] = i == 6U ? nan_value : 1.0f;
			v[1] = i == 6U ? 1.0f : nan_value;
			v[2] = 2.0f;
			v[3] = 1.0f;
		}
		run_fragment(ir, &m, v);
		want[0] = (float)(v[0] < v[1]) + 2.0f * (float)(v[0] > v[1]) + 4.0f * (float)(v[0] <= v[1]) +
			8.0f * (float)(v[0] >= v[1]);
		want[1] = (float)(v[0] == v[1]) + 2.0f * (float)(v[0] != v[1]) +
			4.0f * (float)(v[0] < v[1] && v[2] > v[3]) + 8.0f * (float)(v[0] < v[1] || v[2] > v[3]);
		want[2] = v[0] < v[2] ? v[1] : v[3];
		want[3] = (float)(!(v[0] < v[1])) + 2.0f * (v[2] < v[3] ? v[3] : v[2]);
		expect_color(&m, "compare.frag", v, want);
	}
	drv_gpu_shader_ir_free(ir);
	printf("  comparisons: < > <= >= == != && || ?: ! mix(bool), NaN on either side, -0 == 0\n");
}

/* Hand-assembled: all twelve float comparisons of SPIR-V, ordered and unordered, against NaN. */
static void
test_unordered_comparisons(void)
{
	static const float pairs[][2] = {
		{ 1.0f, 2.0f }, { 2.0f, 1.0f }, { 3.0f, 3.0f }, { 0.0f, 0.0f },
	};
	struct drv_gpu_shader_ir *ir[2];
	struct drv_gpu_compile_diagnostic diag;
	struct machine m;
	unsigned half, i, k;
	float nan_value;

	/* two modules of six comparisons each (180 .. 185, 186 .. 191), each result selecting 16 or 2 */
	for (half = 0U; half < 2U; half++) {
		begin_module();
		op(61U, 3U, U(T_VEC4), U(B), U(V_IN0));
		op(81U, 4U, U(T_FLOAT), U(B + 1), U(B), U(0));
		op(81U, 4U, U(T_FLOAT), U(B + 2), U(B), U(1));
		for (k = 0U; k < 6U; k++) {
			op(180U + 6U * half + k, 4U, U(T_BOOL), U(B + 10 + k), U(B + 1), U(B + 2));
			op(169U, 5U, U(T_FLOAT), U(B + 20 + k), U(B + 10 + k), U(C_F16), U(C_F2));
		}
		op(80U, 6U, U(T_VEC4), U(B + 30), U(B + 20), U(B + 21), U(B + 22), U(B + 23));
		op(62U, 2U, U(V_OUT0), U(B + 30));
		op(80U, 6U, U(T_VEC4), U(B + 31), U(B + 24), U(B + 25), U(C_F5), U(C_F5));
		op(62U, 2U, U(V_OUT1), U(B + 31));
		assert(end_module(&ir[half], &diag) == 0);
	}

	nan_value = bits_to_float(0x7FC00000U);
	for (i = 0U; i < sizeof(pairs) / sizeof(pairs[0]) + 3U; i++) {
		float a, b;
		int truth[12];

		a = i < 4U ? pairs[i][0] : (i == 5U ? 1.0f : nan_value);
		b = i < 4U ? pairs[i][1] : (i == 4U ? 1.0f : nan_value);
		truth[0] = a == b;                      /* FOrdEqual */
		truth[1] = !(a < b || a > b);           /* FUnordEqual: equal, or unordered */
		truth[2] = a < b || a > b;              /* FOrdNotEqual: ordered and different */
		truth[3] = a != b;                      /* FUnordNotEqual */
		truth[4] = a < b;                       /* FOrdLessThan */
		truth[5] = !(a >= b);                   /* FUnordLessThan */
		truth[6] = a > b;                       /* FOrdGreaterThan */
		truth[7] = !(a <= b);                   /* FUnordGreaterThan */
		truth[8] = a <= b;                      /* FOrdLessThanEqual */
		truth[9] = !(a > b);                    /* FUnordLessThanEqual */
		truth[10] = a >= b;                     /* FOrdGreaterThanEqual */
		truth[11] = !(a < b);                   /* FUnordGreaterThanEqual */
		for (half = 0U; half < 2U; half++) {
			float v[4];

			v[0] = a;
			v[1] = b;
			v[2] = 0.0f;
			v[3] = 0.0f;
			memset(&m, 0, sizeof(m));
			memcpy(m.input[0], v, sizeof(v));
			run_ir(ir[half], &m);
			for (k = 0U; k < 6U; k++) {
				float got = k < 4U ? m.output[0][k] : m.output[1][k - 4U];
				float want = truth[6U * half + k] ? 16.0f : 2.0f;

				if (got != want) {
					printf("  comparison opcode %u of (%g, %g) = %g, want %g\n", 180U + 6U * half + k,
						(double)a, (double)b, (double)got, (double)want);
					assert(!"comparison mismatch");
				}
			}
		}
	}
	drv_gpu_shader_ir_free(ir[0]);
	drv_gpu_shader_ir_free(ir[1]);
	printf("  comparisons: the twelve FOrd* / FUnord* opcodes, ordered and unordered, with NaN on either or both sides\n");
}

/* The GLSL of branch.frag evaluated in C at one pixel centre (x, y). */
static void
branch_reference(float x, float y, float want[4])
{
	float c[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	float t = 1.0f, s;
	unsigned k;

	if (x < 21.0f) {
		if (y < 11.0f) {
			c[0] = 1.0f;
		} else {
			c[1] = 1.0f;
			t = t * 2.0f;
		}
	} else if (y < 33.0f && x > 41.0f) {
		c[2] = 1.0f;
		t = 3.0f;
	} else {
		for (k = 0U; k < 4U; k++)
			c[k] = 0.25f;
	}
	s = (x < y + 0.5f) ? t : -t;
	for (k = 0U; k < 3U; k++)
		want[k] = c[k];
	want[3] = c[3] + s;
}

/* The GLSL of discard.frag evaluated in C: returns 1 when the pixel is discarded. */
static int
discard_reference(float x, float y, float want[4])
{
	float cell = floorf(x * 0.125f) + floorf(y * 0.125f);

	if ((cell * 0.5f - floorf(cell * 0.5f)) > 0.25f)
		return 1;
	want[0] = 0.5f;
	want[1] = 0.25f;
	want[2] = 1.0f;
	want[3] = 1.0f;
	if (y < 32.0f) {
		if ((x * 0.5f - floorf(x * 0.5f)) < 0.5f && x > 16.0f)
			return 1;
		want[0] = 1.0f;
	}
	return 0;
}

/* branch.frag and discard.frag at every pixel centre of a 64 x 64 target. */
static void
test_branches_and_discard(void)
{
	struct drv_gpu_shader_ir *branch, *discard;
	struct machine m;
	unsigned x, y, kills;

	branch = parse_file(COMPILER_SHADERS, "branch.frag.spv", DRV_GPU_STAGE_FRAGMENT);
	discard = parse_file(COMPILER_SHADERS, "discard.frag.spv", DRV_GPU_STAGE_FRAGMENT);
	kills = 0U;
	for (y = 0U; y < 64U; y++) {
		for (x = 0U; x < 64U; x++) {
			float v[4];
			float want[4];
			int killed;

			v[0] = (float)x + 0.5f;
			v[1] = (float)y + 0.5f;
			v[2] = 0.0f;
			v[3] = 0.0f;
			run_fragment(branch, &m, v);
			branch_reference(v[0], v[1], want);
			expect_color(&m, "branch.frag", v, want);
			assert(m.killed == 0);

			run_fragment(discard, &m, v);
			killed = discard_reference(v[0], v[1], want);
			if (m.killed != killed) {
				printf("  discard.frag at (%u, %u): killed %d, want %d\n", x, y, m.killed, killed);
				assert(!"discard mismatch");
			}
			if (killed == 0)
				expect_color(&m, "discard.frag", v, want);
			kills += (unsigned)killed;
		}
	}
	assert(kills > 1024U && kills < 3072U);
	drv_gpu_shader_ir_free(branch);
	drv_gpu_shader_ir_free(discard);
	printf("  control flow: nested if / else, && via phi, ?: via a local, discard in and out of branches, at 4096 pixels (%u discarded)\n",
		kills);
}

/* Hand-assembled: a return inside a branch keeps its channels out of what follows the merge. */
static void
test_return_in_branch(void)
{
	struct drv_gpu_shader_ir *ir;
	struct drv_gpu_compile_diagnostic diag;
	struct machine m;
	unsigned i;

	/* if (in0.x < 5) { out0 = (16,16,16,16); return; } out0 = (2,2,2,2); */
	begin_module();
	op(61U, 3U, U(T_VEC4), U(B), U(V_IN0));
	op(81U, 4U, U(T_FLOAT), U(B + 1), U(B), U(0));
	op(184U, 4U, U(T_BOOL), U(B + 2), U(B + 1), U(C_F5));
	op(247U, 2U, U(B + 4), U(0));                                           /* OpSelectionMerge %merge */
	op(250U, 3U, U(B + 2), U(B + 3), U(B + 4));                             /* OpBranchConditional */
	op(248U, 1U, U(B + 3));
	op(80U, 6U, U(T_VEC4), U(B + 5), U(C_F16), U(C_F16), U(C_F16), U(C_F16));
	op(62U, 2U, U(V_OUT0), U(B + 5));
	op(253U, 0U);                                                           /* OpReturn */
	op(248U, 1U, U(B + 4));
	op(80U, 6U, U(T_VEC4), U(B + 6), U(C_F2), U(C_F2), U(C_F2), U(C_F2));
	op(62U, 2U, U(V_OUT0), U(B + 6));
	assert(end_module(&ir, &diag) == 0);
	for (i = 0U; i < 2U; i++) {
		float v[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		float want = i == 0U ? 16.0f : 2.0f;

		v[0] = i == 0U ? 1.0f : 9.0f;
		run_fragment(ir, &m, v);
		assert(m.output[0][0] == want && m.output[0][3] == want);
	}
	drv_gpu_shader_ir_free(ir);
	printf("  control flow: a return in a branch leaves the channels that took it out of the stores after the merge\n");
}

/* The vertex shaders: cells.vert passes its value on, vsmath.vert lights it as mview does. */
static void
test_stage_vertex_shaders(void)
{
	struct drv_gpu_shader_ir *cells, *vsmath;
	struct machine m;
	unsigned i, k;

	cells = parse_file(COMPILER_SHADERS, "cells.vert.spv", DRV_GPU_STAGE_VERTEX);
	vsmath = parse_file(COMPILER_SHADERS, "vsmath.vert.spv", DRV_GPU_STAGE_VERTEX);
	for (i = 0U; i < sizeof(math_inputs) / sizeof(math_inputs[0]); i++) {
		const float *v = math_inputs[i];
		float n[3], length2, scale, lambert, want[4];

		memset(&m, 0, sizeof(m));
		m.input[0][0] = 0.25f;
		m.input[0][1] = -0.5f;
		m.input[0][2] = 0.0f;
		m.input[0][3] = 1.0f;
		memcpy(m.input[1], v, 4U * sizeof(float));
		run_ir(cells, &m);
		for (k = 0U; k < 4U; k++)
			assert(m.output[0][k] == v[k] && m.output[SLOT_POSITION][k] == m.input[0][k]);

		run_ir(vsmath, &m);
		length2 = v[0] * v[0] + v[1] * v[1];
		length2 = length2 + v[2] * v[2];
		scale = 1.0f / sqrtf(length2);
		for (k = 0U; k < 3U; k++)
			n[k] = v[k] * scale;
		lambert = n[0] * 0.267261f + n[1] * 0.534522f;
		lambert = lambert + n[2] * 0.801784f;
		lambert = lambert >= 0.0f ? lambert : 0.0f;
		want[0] = n[0];
		want[1] = n[1];
		want[2] = lambert;
		want[3] = v[3] >= 0.0f ? v[3] : 0.0f;
		want[3] = want[3] < 1.0f ? want[3] : 1.0f;
		for (k = 0U; k < 4U; k++)
			assert(float_to_bits(m.output[0][k]) == float_to_bits(want[k]));
	}
	drv_gpu_shader_ir_free(cells);
	drv_gpu_shader_ir_free(vsmath);
	printf("  vertex: normalize, max(dot, 0) with a constant vec3 (OpConstantComposite), clamp\n");
}

/*
 * The feature test's shaders: ubo.vert transforms by a column-major mat4 of a uniform block (MatrixStride 16) and
 * adds its offset; ubo.frag reads a vec4, the second element of a vec4 array (ArrayStride 16) and the third column
 * of a mat4 of another block; tex3.frag samples three images of two sets.
 */
static void
test_feature_shaders(void)
{
	static const float transform[20] = {
		0.25f, 0.5f, -1.0f, 2.0f, 3.0f, 0.5f, 0.25f, -0.5f, 0.125f, -2.0f, 1.0f, 0.75f,
		0.5f, -0.5f, 0.0f, 1.0f, 0.25f, 0.125f, 0.0f, 0.5f,
	};
	static const float material[28] = {
		0.5f, 0.25f, 1.0f, 1.0f, 9.0f, 9.0f, 9.0f, 9.0f, 0.5f, 1.0f, 0.5f, 3.0f,
		7.0f, 7.0f, 7.0f, 7.0f, 6.0f, 6.0f, 6.0f, 6.0f, 0.25f, 0.5f, 0.75f, -1.0f, 5.0f, 5.0f, 5.0f, 5.0f,
	};
	static const float position[4] = { -0.5f, 0.75f, 0.25f, 1.0f };
	struct drv_gpu_shader_ir *vert, *frag, *tex;
	struct machine m;
	float want;
	unsigned k, c;

	vert = parse_file(FEATURE_SHADERS, "ubo.vert.spv", DRV_GPU_STAGE_VERTEX);
	frag = parse_file(FEATURE_SHADERS, "ubo.frag.spv", DRV_GPU_STAGE_FRAGMENT);
	tex = parse_file(FEATURE_SHADERS, "tex3.frag.spv", DRV_GPU_STAGE_FRAGMENT);

	/* One uniform block each, at the binding the GLSL names, read over the bytes the shader uses. */
	assert(vert->uniform_count == 1U && vert->uniforms[0].kind == DRV_GPU_IR_UNIFORM_BLOCK);
	assert(vert->uniforms[0].set == 0U && vert->uniforms[0].binding == 0U);
	assert(vert->uniforms[0].offset == 0U && vert->uniforms[0].size == 80U);
	assert(frag->uniform_count == 1U && frag->uniforms[0].binding == 1U);
	assert(frag->uniforms[0].offset == 0U && frag->uniforms[0].size == 96U);

	/* ubo.vert: gl_Position = transform * position + offset, the transform read as columns */
	memset(&m, 0, sizeof(m));
	memcpy(m.ubo[0], transform, sizeof(transform));
	memcpy(m.input[0], position, sizeof(position));
	m.input[1][0] = 0.5f;
	run_ir(vert, &m);
	for (k = 0U; k < 4U; k++) {
		want = 0.0f;
		for (c = 0U; c < 4U; c++)
			want = want + transform[c * 4U + k] * position[c];
		want = want + transform[16U + k];
		assert(fabsf(m.output[SLOT_POSITION][k] - want) <= 1e-6f);
	}
	assert(m.output[0][0] == 0.5f);

	/* ubo.frag: color * scale[1] + extra[2] */
	memset(&m, 0, sizeof(m));
	memcpy(m.ubo[0], material, sizeof(material));
	run_ir(frag, &m);
	for (k = 0U; k < 4U; k++) {
		want = material[k] * material[8U + k] + material[20U + k];
		assert(fabsf(m.output[0][k] - want) <= 1e-6f);
	}

	/* tex3.frag: three sampled images in the order named, (set 0, 0), (set 0, 2), (set 1, 1); r, g, b of each */
	assert(tex->uniform_count == 3U);
	assert(tex->uniforms[0].set == 0U && tex->uniforms[0].binding == 0U);
	assert(tex->uniforms[1].set == 0U && tex->uniforms[1].binding == 2U);
	assert(tex->uniforms[2].set == 1U && tex->uniforms[2].binding == 1U);
	memset(&m, 0, sizeof(m));
	m.input[0][0] = 0.25f;
	m.input[0][1] = 0.5f;
	run_ir(tex, &m);
	assert(m.output[0][0] == 0.25f);
	assert(m.output[0][1] == 0.5f);
	assert(m.output[0][2] == 0.25f + 2.0f * 0.5f);
	assert(m.output[0][3] == 1.0f);
	drv_gpu_shader_ir_free(vert);
	drv_gpu_shader_ir_free(frag);
	drv_gpu_shader_ir_free(tex);
	printf("  feature: mat4 uniform read as columns times a vec4 plus an offset; vec4, array element and mat4 column of a block; 3 samplers of 2 sets\n");
}

/* mview's three shaders as shipped: mview.vert against its GLSL, cutout.frag's discard at alpha 0.5. */
static void
test_mview_shaders(void)
{
	static const float columns[8][4] = {
		{ 0.5f, 0.0f, 0.0f, 0.0f }, { 0.0f, -0.75f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.25f, 1.0f },
		{ 0.125f, -0.25f, 0.5f, 2.0f }, { 0.0f, 1.0f, 0.0f, 0.0f }, { -1.0f, 0.0f, 0.0f, 0.0f },
		{ 0.0f, 0.0f, 1.0f, 0.0f }, { 1.0f, 1.0f, 1.0f, 0.5f },
	};
	static const float vertex[8] = { 0.5f, -1.0f, 2.0f, 0.3f, -0.6f, 0.8f, 0.25f, 0.75f };
	struct drv_gpu_shader_ir *vert, *frag, *cutout;
	struct machine m;
	float turned[3], length2, scale, lambert, shade, clip[4], want[4], rgba[4];
	unsigned k, i;

	vert = parse_file(MVIEW_SHADERS, "mview.vert.spv", DRV_GPU_STAGE_VERTEX);
	frag = parse_file(MVIEW_SHADERS, "mview.frag.spv", DRV_GPU_STAGE_FRAGMENT);
	cutout = parse_file(MVIEW_SHADERS, "cutout.frag.spv", DRV_GPU_STAGE_FRAGMENT);
	assert(vert->push_bytes == 112U && vert->input_count == 3U && vert->output_count == 2U);
	assert(frag->push_bytes == 128U && cutout->push_bytes == 128U);

	/* mview.vert: the rotated normal lit from a fixed direction, the position through the clip columns */
	memset(&m, 0, sizeof(m));
	memcpy(m.push, columns, sizeof(columns));
	m.input[0][0] = vertex[0];
	m.input[0][1] = vertex[1];
	m.input[0][2] = vertex[2];
	m.input[1][0] = vertex[3];
	m.input[1][1] = vertex[4];
	m.input[1][2] = vertex[5];
	m.input[2][0] = vertex[6];
	m.input[2][1] = vertex[7];
	run_ir(vert, &m);
	for (k = 0U; k < 3U; k++) {
		turned[k] = vertex[3] * columns[4][k] + vertex[4] * columns[5][k];
		turned[k] = turned[k] + vertex[5] * columns[6][k];
	}
	length2 = turned[0] * turned[0] + turned[1] * turned[1];
	length2 = length2 + turned[2] * turned[2];
	scale = 1.0f / sqrtf(length2);
	lambert = turned[0] * scale * 0.267261f + turned[1] * scale * 0.534522f;
	lambert = lambert + turned[2] * scale * 0.801784f;
	lambert = lambert >= 0.0f ? lambert : 0.0f;
	shade = 0.35f + 0.65f * lambert;
	for (k = 0U; k < 4U; k++) {
		clip[k] = columns[0][k] * vertex[0] + columns[1][k] * vertex[1];
		clip[k] = clip[k] + columns[2][k] * vertex[2];
		clip[k] = clip[k] + columns[3][k];
		assert(fabsf(m.output[SLOT_POSITION][k] - clip[k]) <= 1e-6f);
	}
	assert(fabsf(m.output[1][0] - shade) <= 1e-6f);
	assert(m.output[0][0] == vertex[6] && m.output[0][1] == vertex[7]);

	/* mview.frag and cutout.frag: texel times the colour pushed at byte 112, lit; alpha below 0.5 discards */
	for (i = 0U; i < 4U; i++) {
		float u = 0.2f + 0.2f * (float)i, v = 0.9f - 0.2f * (float)i, alpha = 0.3f + 0.2f * (float)i;
		float color[4];

		color[0] = 0.5f;
		color[1] = 0.75f;
		color[2] = 1.0f;
		color[3] = alpha;
		memset(&m, 0, sizeof(m));
		memcpy(m.push + 112, color, sizeof(color));
		m.input[0][0] = u;
		m.input[0][1] = v;
		m.input[1][0] = 0.8f;
		fake_texture(0U, 0U, u, v, rgba);
		for (k = 0U; k < 3U; k++)
			want[k] = (rgba[k] * color[k]) * 0.8f;
		want[3] = rgba[3] * color[3];
		run_ir(frag, &m);
		for (k = 0U; k < 4U; k++)
			assert(m.output[0][k] == want[k]);
		assert(m.killed == 0);

		run_ir(cutout, &m);
		assert(m.killed == (want[3] < 0.5f));
		if (m.killed == 0) {
			for (k = 0U; k < 3U; k++)
				assert(m.output[0][k] == want[k]);
			assert(m.output[0][3] == 1.0f);
		}
	}
	drv_gpu_shader_ir_free(vert);
	drv_gpu_shader_ir_free(frag);
	drv_gpu_shader_ir_free(cutout);
	printf("  mview: mview.vert (normalize, max, OpConstantComposite) against its GLSL; mview.frag; cutout.frag discards below alpha 0.5\n");
}

/* ------------------------------------------------------------------ the generality test (p014 E2) */

/* Parses one of the generality test's embedded modules. */
static struct drv_gpu_shader_ir *
parse_words(const char *name, const uint32_t *words, size_t bytes, enum drv_gpu_shader_stage stage)
{
	struct drv_gpu_shader_ir *ir;
	struct drv_gpu_compile_diagnostic diag;
	int error;

	error = drv_gpu_shader_parse(words, bytes / 4U, stage, &ir, &diag);
	if (error != 0)
		printf("  %s refused (%d): opcode %u at word %u: %s\n", name, error, diag.opcode, diag.word_offset,
			diag.reason != NULL ? diag.reason : "-");
	assert(error == 0);
	return ir;
}

/* The word an RGBA8 target stores for the colour a fragment shader wrote to location 0. */
static uint32_t
target_word(const struct machine *m)
{
	uint32_t word = 0U;
	unsigned k;

	for (k = 0U; k < 4U; k++) {
		float value = m->output[0][k];
		uint32_t byte;

		assert(m->written[0][k] >= 1U);
		value = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
		byte = (uint32_t)floorf(value * 255.0f + 0.5f);
		word |= byte << (8U * k);
	}
	return word;
}

/* The bits of the float that is twice / 2 (the varying steps' sums are whole halves below 2^24). */
static uint32_t
half_bits(uint32_t twice)
{
	return float_to_bits((float)twice * 0.5f);
}

/*
 * The generality test's fragment shaders at every pixel of 64 x 64, the pixel coordinate (x + 0.5, y + 0.5) at
 * location 0, against the words regenerate.py computed from the SPIR-V definitions: matrices from a uniform block
 * (column- and row-major, mat3) and push constants, a local matrix, transpose, outer product; the integer
 * operations, divisions and conversions; mod / round / trunc / ceil / sign / step / smoothstep; loops with
 * per-pixel trip counts, continue, break, nesting and do-while.
 */
static void
test_generality_fragment_shaders(void)
{
	static const struct {
		const char *name;
		const uint32_t *words;
		size_t bytes;
		const uint32_t *expected;
	} steps[8] = {
		{ "matrix.frag", i915_vke2_matrix_frag, sizeof(i915_vke2_matrix_frag), i915_vke2_matrix_expected },
		{ "int.frag", i915_vke2_int_frag, sizeof(i915_vke2_int_frag), i915_vke2_int_expected },
		{ "float.frag", i915_vke2_float_frag, sizeof(i915_vke2_float_frag), i915_vke2_float_expected },
		{ "loop.frag", i915_vke2_loop_frag, sizeof(i915_vke2_loop_frag), i915_vke2_loop_expected },
		{ "spill.frag", i915_vke2_spill_frag, sizeof(i915_vke2_spill_frag), i915_vke2_spill_expected },
		/* ws031-p024: the integer boundaries, and the undefined results (the models' words: 0, low-5-bit shifts) */
		{ "edge.frag", i915_vke2_edge_frag, sizeof(i915_vke2_edge_frag), i915_vke2_edge_expected },
		{ "undef.frag", i915_vke2_undef_frag, sizeof(i915_vke2_undef_frag), i915_vke2_undef_expected },
		/* ws031-p039: 16-bit integers whose high half the compiler leaves undefined, made whole where it matters */
		{ "int16.frag", i915_vke2_int16_frag, sizeof(i915_vke2_int16_frag), i915_vke2_int16_expected },
	};
	struct drv_gpu_shader_ir *ir;
	struct machine m;
	unsigned step, x, y, loops, k;

	for (step = 0U; step < 8U; step++) {
		ir = parse_words(steps[step].name, steps[step].words, steps[step].bytes, DRV_GPU_STAGE_FRAGMENT);
		loops = 0U;
		for (k = 0U; k < ir->instruction_count; k++)
			if (ir->instructions[k].op == DRV_GPU_IR_LOOP_BEGIN)
				loops++;
		if (step == 3U)
			assert(loops == 5U);            /* for, for { while }, do-while, while (true) */
		for (y = 0U; y < 64U; y++) {
			for (x = 0U; x < 64U; x++) {
				uint32_t got, want;

				memset(&m, 0, sizeof(m));
				m.input[0][0] = (float)x + 0.5f;
				m.input[0][1] = (float)y + 0.5f;
				memcpy(m.ubo[0], i915_vke2_matrices, sizeof(i915_vke2_matrices));
				memcpy(m.push, i915_vke2_push, sizeof(i915_vke2_push));
				run_ir(ir, &m);
				got = target_word(&m);
				want = steps[step].expected[y * 64U + x];
				if (got != want) {
					printf("  %s at (%u, %u): 0x%08x, want 0x%08x\n", steps[step].name, x, y, got, want);
					assert(!"generality fragment shader differs from regenerate.py");
				}
			}
		}
		drv_gpu_shader_ir_free(ir);
	}
	printf("  generality: matrix / int / float / loop / spill / edge / undef / int16 fragment shaders match regenerate.py at 8 x 4096 pixels (5 loops in loop.frag)\n");
}

/*
 * The generality test's fragment shaders of ws075-p004 at every pixel of 64 x 64, against regenerate.py: local
 * arrays and structures through dynamic indices (agg), determinants, inverses and half floats (matfn), gl_FragCoord
 * (coord: the pixel centre, depth 0, w 1) and a flat input array copied and indexed (vformat: the twelve words its
 * vertex shader makes of the attributes, here the expected words themselves).
 */
static void
test_p004_fragment_shaders(void)
{
	static const struct {
		const char *name;
		const uint32_t *words;
		size_t bytes;
		const uint32_t *expected;
	} steps[4] = {
		{ "agg.frag", i915_vke2_agg_frag, sizeof(i915_vke2_agg_frag), i915_vke2_agg_expected },
		{ "matfn.frag", i915_vke2_matfn_frag, sizeof(i915_vke2_matfn_frag), i915_vke2_matfn_expected },
		{ "coord.frag", i915_vke2_coord_frag, sizeof(i915_vke2_coord_frag), NULL },
		{ "vformat.frag", i915_vke2_vformat_frag, sizeof(i915_vke2_vformat_frag), NULL },
	};
	struct drv_gpu_shader_ir *ir;
	struct machine m;
	unsigned step, x, y, k;

	for (step = 0U; step < 4U; step++) {
		ir = parse_words(steps[step].name, steps[step].words, steps[step].bytes, DRV_GPU_STAGE_FRAGMENT);
		for (y = 0U; y < 64U; y++) {
			for (x = 0U; x < 64U; x++) {
				uint32_t got, want;

				memset(&m, 0, sizeof(m));
				m.input[0][0] = (float)x + 0.5f;
				m.input[0][1] = (float)y + 0.5f;
				m.frag_coord[0] = (float)x + 0.5f;
				m.frag_coord[1] = (float)y + 0.5f;
				m.frag_coord[2] = 0.0f;
				m.frag_coord[3] = 1.0f;
				if (step == 3U) {
					/* the flat array: word k of the twelve at location k / 4, component k % 4 */
					for (k = 0U; k < 12U; k++)
						m.input[k / 4U][k % 4U] = bits_to_float(i915_vke2_vformat_words[k]);
				}
				run_ir(ir, &m);
				got = target_word(&m);
				/* coord: x, y and five check bits; vformat: word x % 12 (the kernel test's rules) */
				if (step == 2U)
					want = x | (y << 8) | (0x1fU << 16);
				else if (step == 3U)
					want = i915_vke2_vformat_words[x % 12U];
				else
					want = steps[step].expected[y * 64U + x];
				/* a zero of either sign is the same result (the kernel test compares matfn as floats) */
				if (step == 1U && (got & 0x7fffffffU) == 0U && (want & 0x7fffffffU) == 0U)
					continue;
				if (got != want) {
					printf("  %s at (%u, %u): 0x%08x, want 0x%08x\n", steps[step].name, x, y, got, want);
					assert(!"ws075-p004 fragment shader differs from regenerate.py");
				}
			}
		}
		drv_gpu_shader_ir_free(ir);
	}
	printf("  generality (ws075-p004): agg / matfn / coord / vformat fragment shaders match regenerate.py at 4 x 4096 pixels\n");
}

/*
 * The generality test's vertex shaders and the varying / attribute fragment shaders: vary16.vert writes sixteen
 * varyings, vary16.frag reads them all and subset.frag five of them; vin16.vert reads sixteen attributes; the
 * matrix step's placement chain; vio16.vert (sixteen attributes and sixteen varyings) parses.
 */
static void
test_generality_interfaces(void)
{
	struct drv_gpu_shader_ir *vary_vert, *vary_frag, *subset, *vin_vert, *vin_frag, *matrix_vert, *spill;
	struct machine m;
	unsigned k, c, x, y;

	vary_vert = parse_words("vary16.vert", i915_vke2_vary16_vert, sizeof(i915_vke2_vary16_vert), DRV_GPU_STAGE_VERTEX);
	vary_frag = parse_words("vary16.frag", i915_vke2_vary16_frag, sizeof(i915_vke2_vary16_frag), DRV_GPU_STAGE_FRAGMENT);
	subset = parse_words("subset.frag", i915_vke2_subset_frag, sizeof(i915_vke2_subset_frag), DRV_GPU_STAGE_FRAGMENT);
	vin_vert = parse_words("vin16.vert", i915_vke2_vin16_vert, sizeof(i915_vke2_vin16_vert), DRV_GPU_STAGE_VERTEX);
	vin_frag = parse_words("vin16.frag", i915_vke2_vin16_frag, sizeof(i915_vke2_vin16_frag), DRV_GPU_STAGE_FRAGMENT);
	matrix_vert = parse_words("matrix.vert", i915_vke2_matrix_vert, sizeof(i915_vke2_matrix_vert), DRV_GPU_STAGE_VERTEX);
	spill = parse_words("spill.frag", i915_vke2_spill_frag, sizeof(i915_vke2_spill_frag), DRV_GPU_STAGE_FRAGMENT);

	/* vary16.vert: location 0 the coordinate, location k the seed times k + 1 plus (k, -k, k / 2, 16 - k) */
	memset(&m, 0, sizeof(m));
	m.input[0][0] = -1.0f;
	m.input[0][3] = 1.0f;
	m.input[1][0] = 12.0f;
	m.input[1][1] = 34.0f;
	for (c = 0U; c < 4U; c++)
		m.input[2][c] = bits_to_float(i915_vke2_seed[c]);
	run_ir(vary_vert, &m);
	assert(m.output[0][0] == 12.0f && m.output[0][1] == 34.0f);
	assert(m.output[SLOT_POSITION][0] == -1.0f && m.output[SLOT_POSITION][3] == 1.0f);
	for (k = 1U; k < 16U; k++) {
		const float offset[4] = { (float)k, -(float)k, 0.5f * (float)k, 16.0f - (float)k };

		for (c = 0U; c < 4U; c++)
			assert(m.output[k][c] == bits_to_float(i915_vke2_seed[c]) * (float)(k + 1U) + offset[c]);
	}

	/* vary16.frag and subset.frag, fed what vary16.vert wrote, at a few pixels */
	for (y = 0U; y < 64U; y += 21U) {
		for (x = 0U; x < 64U; x += 13U) {
			struct machine in;

			memset(&in, 0, sizeof(in));
			memcpy(in.input, m.output, sizeof(in.input));
			in.input[0][0] = (float)x + 0.5f;
			in.input[0][1] = (float)y + 0.5f;
			run_ir(vary_frag, &in);
			assert(target_word(&in) == half_bits(2U * (x * 1000U + y * 100000U) + I915_VKE2_VARY16_TWICE));
			run_ir(subset, &in);
			assert(target_word(&in) == half_bits(2U * (x * 1000U + y * 100000U) + I915_VKE2_SUBSET_TWICE));
		}
	}

	/* subset.frag reads locations 0, 1, 6, 11 and 15 only */
	assert(subset->input_count == 5U);
	assert(subset->inputs[0].location + subset->inputs[1].location + subset->inputs[2].location +
	       subset->inputs[3].location + subset->inputs[4].location == 0U + 1U + 6U + 11U + 15U);

	/* vin16.vert: sixteen attributes; the data 2 .. 15 weighted by their locations at location 3 */
	memset(&m, 0, sizeof(m));
	m.input[0][3] = 1.0f;
	m.input[1][0] = 7.0f;
	for (k = 2U; k < 16U; k++)
		for (c = 0U; c < 4U; c++)
			m.input[k][c] = bits_to_float(i915_vke2_vin_data[(k - 2U) * 4U + c]);
	run_ir(vin_vert, &m);
	assert(m.output[0][0] == 7.0f);
	{
		struct machine in;

		memset(&in, 0, sizeof(in));
		memcpy(in.input[3], m.output[3], sizeof(in.input[3]));
		in.input[0][0] = 5.5f;
		in.input[0][1] = 9.5f;
		run_ir(vin_frag, &in);
		assert(target_word(&in) == half_bits(2U * (5U * 1000U + 9U * 100000U) + I915_VKE2_VIN16_TWICE));
	}

	/* matrix.vert: transpose(turn) * scale, a row-major and a column-major matrix of one block, places each corner */
	for (k = 0U; k < 4U; k++) {
		static const float corners[4][2] = { { -1.0f, -1.0f }, { 1.0f, -1.0f }, { 1.0f, 1.0f }, { -1.0f, 1.0f } };

		memset(&m, 0, sizeof(m));
		memcpy(m.ubo[0], i915_vke2_placement, sizeof(i915_vke2_placement));
		m.input[0][0] = bits_to_float(i915_vke2_matrix_corners[2U * k]);
		m.input[0][1] = bits_to_float(i915_vke2_matrix_corners[2U * k + 1U]);
		m.input[0][3] = 1.0f;
		run_ir(matrix_vert, &m);
		assert(m.output[SLOT_POSITION][0] == corners[k][0] && m.output[SLOT_POSITION][1] == corners[k][1]);
		assert(m.output[SLOT_POSITION][3] == 1.0f);
	}

	drv_gpu_shader_ir_free(vary_vert);
	drv_gpu_shader_ir_free(vary_frag);
	drv_gpu_shader_ir_free(subset);
	drv_gpu_shader_ir_free(vin_vert);
	drv_gpu_shader_ir_free(vin_frag);
	drv_gpu_shader_ir_free(matrix_vert);
	drv_gpu_shader_ir_free(spill);
	printf("  generality: 16 varyings out and in, 5 of 16 read, 16 attributes, row-/column-major placement chain\n");
}

/*
 * OpSRem, OpSMod, OpFRem and OpFMod on negative operands (GLSL has no SRem or FRem): the remainder takes the
 * dividend's sign, the modulus the divisor's.
 */
static void
test_remainders(void)
{
	struct drv_gpu_shader_ir *ir;
	struct drv_gpu_compile_diagnostic diag;
	struct machine m;
	unsigned function_at;
	uint32_t tail[7];

	begin_module();
	function_at = mod_n - 7U;
	memcpy(tail, mod + function_at, sizeof(tail));
	mod_n = function_at;
	op(43U, 3U, U(T_INT), U(B), U(0xFFFFFFF9U));                    /* -7 */
	op(43U, 3U, U(T_INT), U(B + 1), U(0xFFFFFFFDU));                /* -3 */
	op(43U, 3U, U(T_INT), U(B + 2), U(7));
	op(43U, 3U, U(T_FLOAT), U(B + 3), float_to_bits(-7.5f));
	memcpy(mod + mod_n, tail, sizeof(tail));
	mod_n += 7U;
	op(138U, 4U, U(T_INT), U(B + 10), U(B), U(C_I3));               /* SRem(-7, 3) = -1 */
	op(138U, 4U, U(T_INT), U(B + 11), U(B + 2), U(B + 1));          /* SRem(7, -3) = 1 */
	op(139U, 4U, U(T_INT), U(B + 12), U(B), U(C_I3));               /* SMod(-7, 3) = 2 */
	op(139U, 4U, U(T_INT), U(B + 13), U(B + 2), U(B + 1));          /* SMod(7, -3) = -2 */
	op(111U, 3U, U(T_FLOAT), U(B + 14), U(B + 10));
	op(111U, 3U, U(T_FLOAT), U(B + 15), U(B + 11));
	op(111U, 3U, U(T_FLOAT), U(B + 16), U(B + 12));
	op(111U, 3U, U(T_FLOAT), U(B + 17), U(B + 13));
	op(80U, 6U, U(T_VEC4), U(B + 18), U(B + 14), U(B + 15), U(B + 16), U(B + 17));
	op(62U, 2U, U(V_OUT0), U(B + 18));
	op(140U, 4U, U(T_FLOAT), U(B + 19), U(B + 3), U(C_F2));         /* FRem(-7.5, 2) = -1.5 */
	op(141U, 4U, U(T_FLOAT), U(B + 20), U(B + 3), U(C_F2));         /* FMod(-7.5, 2) = 0.5 */
	op(80U, 6U, U(T_VEC4), U(B + 21), U(B + 19), U(B + 20), U(B + 19), U(B + 20));
	op(62U, 2U, U(V_OUT1), U(B + 21));
	assert(end_module(&ir, &diag) == 0);
	set_inputs(&m);
	run_ir(ir, &m);
	expect_out(&m, 0U, -1.0f, 1.0f, 2.0f, -2.0f);
	expect_out(&m, 1U, -1.5f, 0.5f, -1.5f, 0.5f);
	drv_gpu_shader_ir_free(ir);
	printf("  remainders: SRem(-7,3) = -1, SRem(7,-3) = 1, SMod(-7,3) = 2, SMod(7,-3) = -2, FRem(-7.5,2) = -1.5, FMod = 0.5\n");
}

/* Loads a SPIR-V file of plan/ws075/tests/switch/. */
static uint32_t *
load_switch_spv(const char *name, size_t *words)
{
	char path[512];
	FILE *file;
	long size;
	uint32_t *code;

	snprintf(path, sizeof(path), "%s/plan/ws075/tests/switch/%s", VK_REPO, name);
	file = fopen(path, "rb");
	assert(file != NULL);
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	assert(size > 0 && (size % 4) == 0);
	code = malloc((size_t)size);
	assert(code != NULL);
	assert(fread(code, 1, (size_t)size, file) == (size_t)size);
	fclose(file);
	*words = (size_t)size / 4U;
	return code;
}

/*
 * OpSwitch (ws075-p009): switch.frag as glslc makes it (one OpSwitch of six literals: two of one target, a
 * fall-through, a case on the default's target) and with -O (the merge-return pass adds a default-only OpSwitch
 * around the body for the early return), at every pixel of 64 x 64 against the C formula of the GLSL.
 */
static void
test_switch(void)
{
	static const char *const names[2] = { "switch.frag.spv", "switch-O.frag.spv" };
	struct drv_gpu_shader_ir *ir;
	struct machine m;
	uint32_t *code;
	size_t words;
	unsigned variant, x, y;

	for (variant = 0U; variant < 2U; variant++) {
		code = load_switch_spv(names[variant], &words);
		ir = parse_words(names[variant], code, words * 4U, DRV_GPU_STAGE_FRAGMENT);
		for (y = 0U; y < 64U; y++) {
			for (x = 0U; x < 64U; x++) {
				float place_x = (float)x + 0.5f;
				float place_y = (float)y + 0.5f;
				int k = (int)place_x % 7;
				float r = 0.0f;
				float want[4];
				unsigned c;

				/* The GLSL's switch in C. */
				switch (k) {
				case 0:
					r = 0.125f;
					break;
				case 1:
				case 2:
					r = 0.25f;
					break;
				case 3:
					r = 0.5f;
					/* falls through */
				case 4:
					r = r + 0.0625f;
					break;
				default:
					r = 0.875f;
					break;
				}
				want[0] = r;
				want[1] = place_y > 32.0f ? 0.5f : 0.25f;
				want[2] = place_y > 32.0f ? 0.0f : (float)k * 0.125f;
				want[3] = 1.0f;

				memset(&m, 0, sizeof(m));
				m.input[0][0] = place_x;
				m.input[0][1] = place_y;
				run_ir(ir, &m);
				for (c = 0U; c < 4U; c++) {
					assert(m.written[0][c] >= 1U);
					if (!close_to(m.output[0][c], want[c])) {
						printf("  %s at (%u, %u) component %u: %g, want %g\n", names[variant], x, y, c,
							(double)m.output[0][c], (double)want[c]);
						assert(!"switch.frag differs from its C formula");
					}
				}
			}
		}
		drv_gpu_shader_ir_free(ir);
		free(code);
	}
	printf("  switch: switch.frag (6 literals, shared target, fall-through, default) and its -O form (merge-return) match at 2 x 4096 pixels\n");
}

int
main(void)
{
	test_local_store_load_overwrite();
	test_vector_construct_extract_shuffle();
	test_component_access();
	test_dot_negate_scale();
	test_refusals();
	test_harmless_decoration();
	test_vkdemo_vertex_shader();
	test_vkdemo_fragment_shader();
	test_glsl_std450_lowering();
	test_comparisons_and_selection();
	test_unordered_comparisons();
	test_branches_and_discard();
	test_return_in_branch();
	test_stage_vertex_shaders();
	test_mview_shaders();
	test_feature_shaders();
	test_generality_fragment_shaders();
	test_p004_fragment_shaders();
	test_generality_interfaces();
	test_remainders();
	test_switch();
	assert(fixture_live == 0U);
	printf("  skippable regions (ws075-p023): %u proven across the runs, %u skipped in the poisoned runs, results unchanged\n",
		skip_regions_run, skip_regions_skipped);
	printf("i915 vk lower host test PASS\n");
	return 0;
}
