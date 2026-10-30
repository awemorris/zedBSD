/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws037-p008: the host test of the nvrtx SM75 shader compiler.
 *
 *   1. Known instructions: words the CUDA compiler writes in every SM70+
 *      kernel (the prologue's MOV R1, c[0x0][0x28], S2R, IMAD, EXIT, the
 *      closing BRA and NOP) are built with the encoder and compared bit for
 *      bit.  They check the field positions and the ALU forms.
 *   2. Fields: an instruction of each kind the compiler emits has its
 *      opcode and modifiers where NAK (Mesa, MIT) puts them.
 *   3. Execution: compute modules (SPIR-V made by glslc) are compiled and
 *      run on the small interpreter of the emitted instructions (sm75-machine.c), one
 *      invocation after another, against buffers the test fills; the
 *      results are compared with what C computes from the same inputs.
 *      The interpreter decodes the same field positions the encoder uses,
 *      so this part checks the lowering (instruction choice, registers,
 *      addresses, predicates, the serial dependences), not the encoding.
 *   4. Refusals: an operation stages A and B do not cover is refused with a reason.
 *   5. The first compute launch's parts (ws037-p008 stage C): the QMD 2.2
 *      fields at the positions of NVIDIA's clc3c0qmd.h, the launch's push
 *      buffer, the IR of the first program compiled and run, and the
 *      system-memory page entries of the launch's tables.
 *   6. The first 3D draw's parts (ws037-p007): the push buffer's methods
 *      decoded and read at the positions of NVIDIA's clc597.h, the texture
 *      header and sampler fields at the positions of clc597tex.h, and the
 *      IR of the draw's programs compiled and run.
 *   7. Shader models (ws242-p002): the parts that compile and run programs
 *      (3, 5's and 6's programs) are run for Turing (SM75), Ampere (SM86)
 *      and Ada (SM89), the interpreter checking each model's latencies and
 *      memory order field; a global access's memory order field is checked
 *      for each model, and a model the compiler does not target is refused.
 *
 *   nvrtx-sm75-host-test DIR    DIR holds the .spv of const, add, ids, float, integer, loop, division, shared, atomic,
 *                               contract and precise
 *
 * Nothing here is evidence about the GPU.
 */

#include "drivers/gpu/nvrtx/compiler/sm75/encode.h"
#include "drivers/gpu/nvrtx/compiler/sm75/sm75.h"
#include "drivers/gpu/nvrtx/nvrtx-compute.h"
#include "drivers/gpu/nvrtx/nvrtx-draw.h"
#include "drivers/gpu/nvrtx/nvrtx-gsp.h"
#include "plan/tools/nvrtx/sm75-machine.h"

#include <uapi/errno.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* The shader model the parts that compile programs compile for (main() runs them for each). */
static uint32_t test_sm = NVRTX_SM_TURING;

/* Checks counted and failed. */
static unsigned test_checks;
static unsigned test_failures;

static void check(int passed, const char *what);
static void check_known_words(void);
static void check_fields(void);
static void check_models(const char *directory);
static void check_graphics_fields(void);
static void check_const(const char *directory);
static void check_add(const char *directory);
static void check_ids(const char *directory);
static void check_float(const char *directory);
static void check_integer(const char *directory);
static void check_refusals(const char *directory);
static void check_loop(const char *directory);
static void check_division(const char *directory);
static void check_shared(const char *directory);
static void check_atomic(const char *directory);
static void check_quad(const char *directory);
static void check_contraction(const char *directory);
static uint32_t count_alu(const struct nvrtx_sm75_binary *binary, uint32_t opcode);
static struct nvrtx_sm75_binary *compile_graphics(const char *directory, const char *name, enum drv_gpu_shader_stage stage);
static void check_qmd(void);
static void check_launch_push(void);
static void check_first_program(void);
static void check_system_pages(void);
static void check_draw_push(void);
static void check_draw_descriptors(void);
static void check_draw_programs(void);
static struct nvrtx_sm75_binary *compile_draw_ir(const struct nvrtx_draw_ir *program, const char *name);
static int header_is(const uint32_t *sph, const uint32_t *want);
static int decode_push(const uint8_t *push, size_t length, uint32_t *state, uint8_t *seen);
static uint64_t field(const uint32_t *words, unsigned low, unsigned bits);
static int words_are(const struct nvrtx_sm75_word *word, uint64_t low, uint64_t high);
static struct nvrtx_sm75_binary *compile_file(const char *directory, const char *name);
static int near(float a, float b);

/* The kernel's allocator, on the host. */
void *kern_calloc(size_t count, size_t size);
void kern_free(void *pointer);

void *
kern_calloc(
	size_t count,
	size_t size)
{
	return calloc(count, size);
}

void
kern_free(
	void *pointer)
{
	free(pointer);
}

/*
 * Runs every part and reports the tally.
 */
int
main(
	int argc,
	char **argv)
{
	static const uint32_t models[] = { NVRTX_SM_TURING, NVRTX_SM_AMPERE, NVRTX_SM_ADA };
	uint32_t index;

	/* The directory of the modules. */
	if (argc != 2) {
		fprintf(stderr, "usage: %s DIR\n", argv[0]);
		return 2;
	}

	/* The parts that do not compile, once. */
	check_known_words();
	check_fields();
	check_graphics_fields();
	check_models(argv[1]);
	check_refusals(argv[1]);
	check_qmd();
	check_launch_push();
	check_system_pages();
	check_draw_push();
	check_draw_descriptors();

	/* The parts that compile and run programs, for each shader model. */
	for (index = 0U; index < sizeof(models) / sizeof(models[0]); index++) {
		test_sm = models[index];
		printf("== shader model %u\n", test_sm);
		check_const(argv[1]);
		check_add(argv[1]);
		check_ids(argv[1]);
		check_float(argv[1]);
		check_integer(argv[1]);
		check_loop(argv[1]);
		check_division(argv[1]);
		check_shared(argv[1]);
		check_atomic(argv[1]);
		check_quad(argv[1]);
		check_contraction(argv[1]);
		check_first_program();
		check_draw_programs();
	}

	/* The tally. */
	printf("nvrtx_sm75_host_test: %u checks, %u failures\n", test_checks, test_failures);
	if (test_failures != 0U)
		return 1;

	/* Succeeded: every check passed. */
	return 0;
}

/* Counts a check, and names a failed one. */
static void
check(
	int passed,
	const char *what)
{
	test_checks++;

	/* A failure is named. */
	if (!passed) {
		test_failures++;
		printf("FAIL: %s\n", what);
	}
}

/*
 * Builds the words the CUDA compiler writes in every SM70+ kernel and
 * compares them bit for bit.
 */
static void
check_known_words(void)
{
	struct nvrtx_sm75_word word;
	struct nvrtx_sm75_src a;
	struct nvrtx_sm75_src b;
	struct nvrtx_sm75_src c;

	/* MOV R1, c[0x0][0x28] (the stack pointer of the prologue), delay 2. */
	nvrtx_sm75_begin(&word);
	b = nvrtx_sm75_cb(0U, 0x28U);
	nvrtx_sm75_mov(&word, 1U, &b);
	nvrtx_sm75_control(&word, 2U, 0, NVRTX_SM75_BAR_NONE, NVRTX_SM75_BAR_NONE, 0U);
	check(words_are(&word, 0x00000a0000017a02ULL, 0x000fc40000000f00ULL), "MOV R1, c[0x0][0x28]");

	/* S2R R2, SR_TID.X, writing scoreboard 0, delay 1, yield. */
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_s2r(&word, 2U, NVRTX_SM75_SR_TID_X);
	nvrtx_sm75_control(&word, 1U, 1, 0U, NVRTX_SM75_BAR_NONE, 0U);
	check(words_are(&word, 0x0000000000027919ULL, 0x000e220000002100ULL), "S2R R2, SR_TID.X");

	/* IMAD R0, R3, c[0x0][0x0], R0, waiting for scoreboard 0, delay 5. */
	nvrtx_sm75_begin(&word);
	a = nvrtx_sm75_reg(3U);
	b = nvrtx_sm75_cb(0U, 0U);
	c = nvrtx_sm75_reg(0U);
	nvrtx_sm75_imad(&word, 0U, &a, &b, &c, 1);
	nvrtx_sm75_control(&word, 5U, 0, NVRTX_SM75_BAR_NONE, NVRTX_SM75_BAR_NONE, 1U);
	check(words_are(&word, 0x0000000003007a24ULL, 0x001fca00078e0200ULL), "IMAD R0, R3, c[0x0][0x0], R0");

	/* EXIT, delay 5, yield. */
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_exit(&word);
	nvrtx_sm75_control(&word, 5U, 1, NVRTX_SM75_BAR_NONE, NVRTX_SM75_BAR_NONE, 0U);
	check(words_are(&word, 0x000000000000794dULL, 0x000fea0003800000ULL), "EXIT");

	/* BRA to itself. */
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_bra(&word, -1);
	check(words_are(&word, 0xfffffff000007947ULL, 0x000fc0000383ffffULL), "BRA to itself");

	/* NOP. */
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_nop(&word);
	check(words_are(&word, 0x0000000000007918ULL, 0x000fc00000000000ULL), "NOP");
}

/* Checks the opcode and the modifiers of each kind of instruction the compiler emits. */
static void
check_fields(void)
{
	struct nvrtx_sm75_word word;
	struct nvrtx_sm75_src a;
	struct nvrtx_sm75_src b;
	struct nvrtx_sm75_src zero;

	zero = nvrtx_sm75_reg(NVRTX_SM75_RZ);

	/* FADD R4, R5, -R6: form 1, b at 32 with neg at 63. */
	nvrtx_sm75_begin(&word);
	a = nvrtx_sm75_reg(5U);
	b = nvrtx_sm75_reg(6U);
	b.neg = 1;
	nvrtx_sm75_fadd(&word, 4U, &a, &b);
	check(nvrtx_sm75_get(&word, 0U, 9U) == 0x021U && nvrtx_sm75_get(&word, 9U, 3U) == 1U, "FADD opcode and form");
	check(nvrtx_sm75_get(&word, 16U, 8U) == 4U && nvrtx_sm75_get(&word, 24U, 8U) == 5U && nvrtx_sm75_get(&word, 32U, 8U) == 6U, "FADD registers");
	check(nvrtx_sm75_get(&word, 63U, 1U) == 1U && nvrtx_sm75_get(&word, 72U, 1U) == 0U, "FADD negation of b");

	/* FADD R4, R5, 1.0: the immediate is the third operand (form 2), RZ the second. */
	nvrtx_sm75_begin(&word);
	b = nvrtx_sm75_imm(0x3f800000U);
	nvrtx_sm75_fadd(&word, 4U, &a, &b);
	check(nvrtx_sm75_get(&word, 9U, 3U) == 2U && nvrtx_sm75_get(&word, 32U, 32U) == 0x3f800000U && nvrtx_sm75_get(&word, 64U, 8U) == 255U, "FADD immediate in form 2");

	/* ISETP.GE.AND P0, PT, R1, R2, PT, signed. */
	nvrtx_sm75_begin(&word);
	a = nvrtx_sm75_reg(1U);
	b = nvrtx_sm75_reg(2U);
	nvrtx_sm75_isetp(&word, 0U, NVRTX_SM75_ICMP_GE, 1, &a, &b);
	check(nvrtx_sm75_get(&word, 0U, 9U) == 0x00cU && nvrtx_sm75_get(&word, 76U, 3U) == 6U && nvrtx_sm75_get(&word, 73U, 1U) == 1U, "ISETP compare and sign");
	check(nvrtx_sm75_get(&word, 81U, 3U) == 0U && nvrtx_sm75_get(&word, 84U, 3U) == 7U && nvrtx_sm75_get(&word, 87U, 3U) == 7U && nvrtx_sm75_get(&word, 68U, 3U) == 7U, "ISETP predicates");

	/* LOP3.LUT R1, R2, 0x80000000, RZ, 0x3c. */
	nvrtx_sm75_begin(&word);
	a = nvrtx_sm75_reg(2U);
	b = nvrtx_sm75_imm(0x80000000U);
	nvrtx_sm75_lop3(&word, 1U, &a, &b, &zero, 0x3cU);
	check(nvrtx_sm75_get(&word, 0U, 9U) == 0x012U && nvrtx_sm75_get(&word, 9U, 3U) == 4U && nvrtx_sm75_get(&word, 72U, 8U) == 0x3cU, "LOP3 opcode, form and table");
	check(nvrtx_sm75_get(&word, 64U, 8U) == 255U && nvrtx_sm75_get(&word, 87U, 4U) == 0xfU, "LOP3 third operand and predicate input");

	/* LDC.64 R2, c[0x0][0x210]. */
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_ldc(&word, 2U, 0U, 0x210U, 1);
	check(nvrtx_sm75_get(&word, 0U, 12U) == 0xb82U && nvrtx_sm75_get(&word, 38U, 16U) == 0x210U && nvrtx_sm75_get(&word, 73U, 3U) == 5U, "LDC.64 opcode, offset and size");
	check(nvrtx_sm75_get(&word, 24U, 8U) == 255U && nvrtx_sm75_get(&word, 54U, 5U) == 0U, "LDC index and buffer");

	/* STG.E.STRONG.GPU [R4.64], R6. */
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_stg(&word, 4U, 6U);
	check(nvrtx_sm75_get(&word, 0U, 12U) == 0x386U && nvrtx_sm75_get(&word, 24U, 8U) == 4U && nvrtx_sm75_get(&word, 32U, 8U) == 6U, "STG opcode and registers");
	check(nvrtx_sm75_get(&word, 72U, 1U) == 1U && nvrtx_sm75_get(&word, 73U, 3U) == 4U && nvrtx_sm75_get(&word, 77U, 4U) == 0xaU && nvrtx_sm75_get(&word, 84U, 3U) == 1U, "STG access bits");

	/* IMAD.WIDE.U32 R4, R1, 0x1, R2. */
	nvrtx_sm75_begin(&word);
	a = nvrtx_sm75_reg(1U);
	b = nvrtx_sm75_imm(1U);
	zero = nvrtx_sm75_reg(2U);
	nvrtx_sm75_imad_wide(&word, 4U, &a, &b, &zero);
	check(nvrtx_sm75_get(&word, 0U, 9U) == 0x025U && nvrtx_sm75_get(&word, 9U, 3U) == 4U && nvrtx_sm75_get(&word, 64U, 8U) == 2U && nvrtx_sm75_get(&word, 73U, 1U) == 0U, "IMAD.WIDE.U32 form and pair");

	/* SHF.R.U32.HI R1, RZ, R2, R3. */
	nvrtx_sm75_begin(&word);
	b = nvrtx_sm75_reg(2U);
	nvrtx_sm75_shift(&word, 1U, 3U, &b, 1, 0);
	check(nvrtx_sm75_get(&word, 24U, 8U) == 255U && nvrtx_sm75_get(&word, 64U, 8U) == 3U && nvrtx_sm75_get(&word, 73U, 2U) == 3U, "SHF right operands and type");
	check(nvrtx_sm75_get(&word, 75U, 1U) == 1U && nvrtx_sm75_get(&word, 76U, 1U) == 1U && nvrtx_sm75_get(&word, 80U, 1U) == 1U, "SHF wrap, right, high");

	/* BSSY B1 to 5 instructions on, BSYNC B1, a BRA under P0, BAR.SYNC, MEMBAR.GPU, ATOMG.ADD, LDS. */
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_bssy(&word, 1U, 5);
	check(nvrtx_sm75_get(&word, 0U, 12U) == 0x945U && nvrtx_sm75_get(&word, 16U, 4U) == 1U && nvrtx_sm75_get(&word, 34U, 30U) == 20U && nvrtx_sm75_get(&word, 87U, 3U) == 7U, "BSSY barrier and offset");
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_bsync(&word, 1U);
	check(nvrtx_sm75_get(&word, 0U, 12U) == 0x941U && nvrtx_sm75_get(&word, 16U, 4U) == 1U, "BSYNC barrier");
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_bra(&word, -3);
	nvrtx_sm75_bra_if(&word, 0U, 0);
	check(nvrtx_sm75_get(&word, 87U, 3U) == 0U && nvrtx_sm75_get(&word, 90U, 1U) == 0U && nvrtx_sm75_get(&word, 12U, 3U) == 7U, "BRA on P0");
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_bar(&word);
	check(nvrtx_sm75_get(&word, 0U, 12U) == 0xb1dU, "BAR.SYNC");
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_membar(&word, NVRTX_SM75_MEMBAR_GPU);
	check(nvrtx_sm75_get(&word, 0U, 12U) == 0x992U && nvrtx_sm75_get(&word, 76U, 3U) == 2U, "MEMBAR.GPU");
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_atomic(&word, 0, 3U, 4U, 5U, NVRTX_SM75_ATOM_MAX, 1);
	check(nvrtx_sm75_get(&word, 0U, 12U) == 0x3a8U && nvrtx_sm75_get(&word, 87U, 4U) == 2U && nvrtx_sm75_get(&word, 73U, 3U) == 1U && nvrtx_sm75_get(&word, 72U, 1U) == 1U, "ATOMG.MAX.S32");
	check(nvrtx_sm75_get(&word, 16U, 8U) == 3U && nvrtx_sm75_get(&word, 24U, 8U) == 4U && nvrtx_sm75_get(&word, 32U, 8U) == 5U && nvrtx_sm75_get(&word, 81U, 3U) == 7U, "ATOMG registers");
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_lds(&word, 3U, 4U);
	check(nvrtx_sm75_get(&word, 0U, 12U) == 0x984U && nvrtx_sm75_get(&word, 73U, 3U) == 4U && nvrtx_sm75_get(&word, 24U, 8U) == 4U, "LDS");

	/* MUFU.RSQ R1, R2; F2I.TRUNC.NTZ R1, R2; I2F.U32 R1, R2. */
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_mufu(&word, 1U, NVRTX_SM75_MUFU_RSQ, 2U);
	check(nvrtx_sm75_get(&word, 0U, 9U) == 0x108U && nvrtx_sm75_get(&word, 32U, 8U) == 2U && nvrtx_sm75_get(&word, 74U, 6U) == 5U, "MUFU.RSQ");
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_f2i(&word, 1U, 2U, 1);
	check(nvrtx_sm75_get(&word, 0U, 9U) == 0x105U && nvrtx_sm75_get(&word, 72U, 1U) == 1U && nvrtx_sm75_get(&word, 78U, 2U) == 3U && nvrtx_sm75_get(&word, 75U, 2U) == 2U, "F2I signed truncating");
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_i2f(&word, 1U, 2U, 0);
	check(nvrtx_sm75_get(&word, 0U, 9U) == 0x106U && nvrtx_sm75_get(&word, 74U, 1U) == 0U && nvrtx_sm75_get(&word, 84U, 2U) == 2U, "I2F unsigned");
}

/*
 * The shader models: a global access's memory order field in each form
 * (NAK's set_mem_order: before SM80 the scope 2 at 77..78 and strong 2 at
 * 79..80, from SM80 on 7 at 77..80), a compiled program's model and its
 * global accesses, and the refusal of a model the compiler does not
 * target.
 */
static void
check_models(
	const char *directory)
{
	struct nvrtx_sm75_word word;
	struct nvrtx_sm75_binary *binary;
	struct nvrtx_sm75_diagnostic diagnostic;
	uint32_t index;
	uint32_t opcode;
	uint32_t globals;
	int right;
	int error;

	/* LDG, STG and ATOMG for Turing and for Ampere and Ada. */
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_ldg(&word, 3U, 4U);
	nvrtx_sm75_memory_order(&word, NVRTX_SM_TURING);
	check(nvrtx_sm75_get(&word, 77U, 2U) == 2U && nvrtx_sm75_get(&word, 79U, 2U) == 2U && nvrtx_sm75_get(&word, 84U, 3U) == 1U, "LDG.STRONG.GPU on SM75");
	nvrtx_sm75_memory_order(&word, NVRTX_SM_AMPERE);
	check(nvrtx_sm75_get(&word, 77U, 4U) == 7U && nvrtx_sm75_get(&word, 72U, 4U) == 0x9U && nvrtx_sm75_get(&word, 84U, 3U) == 1U, "LDG.STRONG.GPU on SM86");
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_stg(&word, 4U, 6U);
	nvrtx_sm75_memory_order(&word, NVRTX_SM_ADA);
	check(nvrtx_sm75_get(&word, 77U, 4U) == 7U && nvrtx_sm75_get(&word, 0U, 12U) == 0x386U, "STG.STRONG.GPU on SM89");
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_atomic(&word, 0, 3U, 4U, 5U, NVRTX_SM75_ATOM_ADD, 0);
	nvrtx_sm75_memory_order(&word, NVRTX_SM_AMPERE);
	check(nvrtx_sm75_get(&word, 77U, 4U) == 7U && nvrtx_sm75_get(&word, 87U, 4U) == 0U && nvrtx_sm75_get(&word, 73U, 3U) == 0U, "ATOMG.ADD.STRONG.GPU on SM86");

	/* A program for Ada says so, and every global access has the one field. */
	test_sm = NVRTX_SM_ADA;
	binary = compile_file(directory, "add");
	test_sm = NVRTX_SM_TURING;
	check(binary != NULL && binary->shader_model == NVRTX_SM_ADA, "add compiles for SM89");
	if (binary != NULL) {
		right = 1;
		globals = 0U;
		for (index = 0U; index < binary->code_words; index += 4U) {
			opcode = (uint32_t)field(&binary->code[index], 0U, 12U);
			if (opcode != NVRTX_SM75_OP_LDG && opcode != NVRTX_SM75_OP_STG)
				continue;
			globals++;
			if (field(&binary->code[index], 77U, 4U) != 7U)
				right = 0;
		}
		check(right && globals == 3U, "add's two loads and its store are strong at the GPU's scope in SM80's form");
		nvrtx_sm75_binary_free(binary);
	}

	/* Hopper and Blackwell (90, 120) and an unknown model are refused, with a reason. */
	binary = (struct nvrtx_sm75_binary *)1;
	memset(&diagnostic, 0, sizeof(diagnostic));
	error = nvrtx_sm75_compile_stage_sm(NULL, 0U, DRV_GPU_STAGE_COMPUTE, 90U, &binary, &diagnostic);
	check(error == EINVAL && binary == NULL, "a module without words is refused for SM90 too");
	test_sm = 120U;
	binary = compile_file(directory, "add");
	test_sm = NVRTX_SM_TURING;
	check(binary == NULL, "SM120 is refused");
	test_sm = 80U;
	binary = compile_file(directory, "add");
	test_sm = NVRTX_SM_TURING;
	check(binary == NULL, "SM80 (GA100, no RTX) is refused");
}

/* The first target: a constant written at every invocation's index. */
static void
check_const(
	const char *directory)
{
	static struct test_run run;
	uint32_t out[96];
	uint32_t index;
	int same;

	memset(&run, 0, sizeof(run));
	run.binary = compile_file(directory, "const");
	check(run.binary != NULL, "const compiles");
	if (run.binary == NULL)
		return;

	/* Three groups of 32 over a buffer of 96 words. */
	memset(out, 0, sizeof(out));
	sm75_machine_bind(&run, 0U, 0U, out, sizeof(out));
	run.groups[0] = 3U;
	run.groups[1] = 1U;
	run.groups[2] = 1U;
	sm75_machine_dispatch(&run);

	/* Every word is 42. */
	same = 1;
	for (index = 0U; index < 96U; index++) {
		if (out[index] != 42U)
			same = 0;
	}
	check(!run.failed && same, "const writes 42 everywhere");
	check(run.binary->gpr_count >= NVRTX_SM75_GPR_MINIMUM && (run.binary->code_words % 32U) == 0U, "const register count and padding");
	check(run.binary->storage_count == 1U && run.binary->storage[0].binding == 0U, "const storage table");

	nvrtx_sm75_binary_free((struct nvrtx_sm75_binary *)run.binary);
}

/* c[i] = a[i] + b[i] for i < n, the rest untouched (ws101's add.comp). */
static void
check_add(
	const char *directory)
{
	static struct test_run run;
	uint32_t a[128];
	uint32_t b[128];
	uint32_t c[128];
	uint32_t n;
	uint32_t index;
	int same;

	memset(&run, 0, sizeof(run));
	run.binary = compile_file(directory, "add");
	check(run.binary != NULL, "add compiles");
	if (run.binary == NULL)
		return;

	/* The inputs, the output's sentinel, and n = 100 in the push constants. */
	for (index = 0U; index < 128U; index++) {
		a[index] = index * 7U + 3U;
		b[index] = 0xfffffff0U + index;
		c[index] = 0xdeadbeefU;
	}
	n = 100U;
	memcpy(&run.cb[0][NVRTX_SM75_CB_PUSH], &n, sizeof(n));
	sm75_machine_bind(&run, 0U, 0U, a, sizeof(a));
	sm75_machine_bind(&run, 0U, 1U, b, sizeof(b));
	sm75_machine_bind(&run, 0U, 2U, c, sizeof(c));
	run.groups[0] = 2U;
	run.groups[1] = 1U;
	run.groups[2] = 1U;
	sm75_machine_dispatch(&run);

	/* The sums below n, the sentinel above. */
	same = 1;
	for (index = 0U; index < 128U; index++) {
		if (index < n && c[index] != a[index] + b[index])
			same = 0;
		if (index >= n && c[index] != 0xdeadbeefU)
			same = 0;
	}
	check(!run.failed && same, "add: c = a + b below n, untouched above");
	check(run.binary->push_bytes == 4U, "add reads four push bytes");

	nvrtx_sm75_binary_free((struct nvrtx_sm75_binary *)run.binary);
}

/* Every invocation's built-ins (ws101's ids.comp), with the group counts in the system buffer. */
static void
check_ids(
	const char *directory)
{
	static struct test_run run;
	static uint32_t out[13U * 4U * 2U * 3U * 2U * 2U * 1U];
	uint32_t counts[3];
	uint32_t expect[13];
	uint32_t g[3];
	uint32_t l[3];
	uint32_t size[3];
	uint32_t linear;
	uint32_t word;
	int same;

	memset(&run, 0, sizeof(run));
	run.binary = compile_file(directory, "ids");
	check(run.binary != NULL, "ids compiles");
	if (run.binary == NULL)
		return;

	/* A grid of 2 x 2 x 1 groups of 4 x 2 x 3. */
	counts[0] = 2U;
	counts[1] = 2U;
	counts[2] = 1U;
	memset(out, 0xff, sizeof(out));
	sm75_machine_bind(&run, 0U, 0U, out, sizeof(out));
	sm75_machine_bind(&run, DRV_GPU_IR_SYSTEM_SET, 0U, counts, sizeof(counts));
	run.groups[0] = 2U;
	run.groups[1] = 2U;
	run.groups[2] = 1U;
	sm75_machine_dispatch(&run);

	/* Each invocation's thirteen words. */
	same = 1;
	size[0] = 8U;
	size[1] = 4U;
	size[2] = 3U;
	for (g[2] = 0U; g[2] < size[2]; g[2]++) {
		for (g[1] = 0U; g[1] < size[1]; g[1]++) {
			for (g[0] = 0U; g[0] < size[0]; g[0]++) {
				l[0] = g[0] % 4U;
				l[1] = g[1] % 2U;
				l[2] = g[2] % 3U;
				linear = g[0] + size[0] * (g[1] + size[1] * g[2]);
				expect[0] = l[0];
				expect[1] = l[1];
				expect[2] = l[2];
				expect[3] = l[0] + 4U * (l[1] + 2U * l[2]);
				expect[4] = g[0] / 4U;
				expect[5] = g[1] / 2U;
				expect[6] = g[2] / 3U;
				expect[7] = counts[0];
				expect[8] = counts[1];
				expect[9] = counts[2];
				expect[10] = g[0];
				expect[11] = g[1];
				expect[12] = g[2];
				for (word = 0U; word < 13U; word++) {
					if (out[linear * 13U + word] != expect[word])
						same = 0;
				}
			}
		}
	}
	check(!run.failed && same, "ids: every built-in of every invocation");

	nvrtx_sm75_binary_free((struct nvrtx_sm75_binary *)run.binary);
}

/* Float arithmetic, conversions, roundings, special functions and a selection. */
static void
check_float(
	const char *directory)
{
	static struct test_run run;
	float x[48];
	float o[48 * 3];
	float scale;
	float v;
	float r;
	uint32_t count;
	uint32_t index;
	int same;

	memset(&run, 0, sizeof(run));
	run.binary = compile_file(directory, "float");
	check(run.binary != NULL, "float compiles");
	if (run.binary == NULL)
		return;

	/* The inputs, the scale and the count. */
	for (index = 0U; index < 48U; index++)
		x[index] = (float)index * 0.37f - 4.0f;
	for (index = 0U; index < 48U * 3U; index++)
		o[index] = -12345.0f;
	scale = 0.75f;
	count = 40U;
	memcpy(&run.cb[0][NVRTX_SM75_CB_PUSH], &scale, sizeof(scale));
	memcpy(&run.cb[0][NVRTX_SM75_CB_PUSH + 4U], &count, sizeof(count));
	sm75_machine_bind(&run, 0U, 0U, (uint32_t *)x, sizeof(x));
	sm75_machine_bind(&run, 0U, 1U, (uint32_t *)o, sizeof(o));
	run.groups[0] = 3U;
	run.groups[1] = 1U;
	run.groups[2] = 1U;
	sm75_machine_dispatch(&run);

	/* The same arithmetic in C, within a small tolerance. */
	same = 1;
	for (index = 0U; index < 48U; index++) {
		if (index >= count) {
			if (o[index * 3U] != -12345.0f)
				same = 0;
			continue;
		}
		v = x[index] * scale;
		r = (v - floorf(v)) + floorf(v) * 2.0f - fabsf(v - 1.5f);
		r = r + sqrtf(fabsf(v)) - exp2f(v * 0.25f) + fminf(v, 2.0f) * fmaxf(v, -1.0f);
		if (!(v < 0.5f))
			r = -r;
		if (!near(o[index * 3U], r))
			same = 0;
		if (!near(o[index * 3U + 1U], (float)(int)(v * 3.0f) + (float)index))
			same = 0;
		if (!near(o[index * 3U + 2U], 1.0f / (fabsf(v) + 1.0f)))
			same = 0;
	}
	check(!run.failed && same, "float: arithmetic, roundings, special functions, selection");

	nvrtx_sm75_binary_free((struct nvrtx_sm75_binary *)run.binary);
}

/* Integer arithmetic, shifts, bitwise operations and comparisons. */
static void
check_integer(
	const char *directory)
{
	static struct test_run run;
	uint32_t o[64 * 4];
	uint32_t k;
	int32_t s;
	uint32_t i;
	uint32_t a;
	int32_t b;
	uint32_t c;
	uint32_t d;
	int same;

	memset(&run, 0, sizeof(run));
	run.binary = compile_file(directory, "integer");
	check(run.binary != NULL, "integer compiles");
	if (run.binary == NULL)
		return;

	/* The push constants. */
	k = 0x12345678U;
	s = -1000;
	memcpy(&run.cb[0][NVRTX_SM75_CB_PUSH], &k, sizeof(k));
	memcpy(&run.cb[0][NVRTX_SM75_CB_PUSH + 4U], &s, sizeof(s));
	memset(o, 0, sizeof(o));
	sm75_machine_bind(&run, 0U, 0U, o, sizeof(o));
	run.groups[0] = 4U;
	run.groups[1] = 1U;
	run.groups[2] = 1U;
	sm75_machine_dispatch(&run);

	/* The same in C. */
	same = 1;
	for (i = 0U; i < 64U; i++) {
		a = i * 2654435761U + k;
		b = (int32_t)a >> 3;
		c = ((a << 5) ^ (a >> 7)) | (a & 0xff00U);
		d = ~c;
		if (b < s)
			d = c;
		if (o[i * 4U] != a - c)
			same = 0;
		if (o[i * 4U + 1U] != (uint32_t)b + (uint32_t)(-s))
			same = 0;
		if (o[i * 4U + 2U] != d)
			same = 0;
		if (o[i * 4U + 3U] != (uint32_t)((a >= c) ? 1U : 0U) + (uint32_t)(((int32_t)a < b) ? 2U : 0U) + (uint32_t)((a == i) ? 4U : 0U))
			same = 0;
	}
	check(!run.failed && same, "integer: arithmetic, shifts, bitwise, comparisons");

	nvrtx_sm75_binary_free((struct nvrtx_sm75_binary *)run.binary);
}

/* An operation the compiler does not cover (a derivative) is refused with a reason. */
static void
check_refusals(
	const char *directory)
{
	static struct drv_gpu_shader_ir_inst list[2];
	struct drv_gpu_shader_ir ir;
	struct nvrtx_sm75_binary *binary;
	struct nvrtx_sm75_diagnostic diagnostic;
	int error;

	(void)directory;
	memset(&ir, 0, sizeof(ir));
	memset(list, 0, sizeof(list));
	list[0].op = DRV_GPU_IR_ICONST;
	list[0].dst = 0U;
	list[1].op = DRV_GPU_IR_DDX;
	list[1].dst = 1U;
	list[1].src[0] = 0U;
	ir.stage = DRV_GPU_STAGE_COMPUTE;
	ir.instructions = list;
	ir.instruction_count = 2U;
	ir.value_count = 5U;
	ir.local_size[0] = 1U;
	ir.local_size[1] = 1U;
	ir.local_size[2] = 1U;

	/* Refused, with a reason, and no program. */
	binary = (struct nvrtx_sm75_binary *)1;
	memset(&diagnostic, 0, sizeof(diagnostic));
	error = nvrtx_sm75_compile_ir(&ir, &binary, &diagnostic);
	check(error == ENOTSUP && binary == NULL && diagnostic.reason != NULL && diagnostic.instruction == 1U, "a derivative is refused with a reason");
	if (diagnostic.reason != NULL)
		printf("  derivative refused: %s (instruction %u)\n", diagnostic.reason, diagnostic.instruction);
}

/*
 * The graphics instructions' fields where NAK's sm70_encode.rs (MIT) puts
 * them: ALD and AST (OpALd, OpASt), IPA (OpIpa), TEX and TLD with the
 * handle in a constant buffer (OpTex, OpTld), the texel offset bit, KILL.
 */
static void
check_graphics_fields(void)
{
	struct nvrtx_sm75_word word;

	/* ALD R5, a[0x84]: vertex and offset RZ, one component. */
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_ald(&word, 5U, 0x84U);
	check(nvrtx_sm75_get(&word, 0U, 12U) == 0x321U && nvrtx_sm75_get(&word, 16U, 8U) == 5U && nvrtx_sm75_get(&word, 40U, 10U) == 0x84U, "ALD opcode, destination, address");
	check(nvrtx_sm75_get(&word, 24U, 8U) == 255U && nvrtx_sm75_get(&word, 32U, 8U) == 255U && nvrtx_sm75_get(&word, 74U, 2U) == 0U && nvrtx_sm75_get(&word, 76U, 4U) == 0U, "ALD offset and vertex RZ, one component, not patch, physical or output");

	/* AST a[0x70], R6: data at 32, vertex at 64, offset at 24. */
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_ast(&word, 0x70U, 6U);
	check(nvrtx_sm75_get(&word, 0U, 12U) == 0x322U && nvrtx_sm75_get(&word, 32U, 8U) == 6U && nvrtx_sm75_get(&word, 64U, 8U) == 255U && nvrtx_sm75_get(&word, 24U, 8U) == 255U && nvrtx_sm75_get(&word, 40U, 10U) == 0x70U, "AST data, vertex, offset, address");

	/* IPA.CONSTANT R7, a[0x90]: the address in words, the frequency, no predicate. */
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_ipa(&word, 7U, 0x90U, NVRTX_SM75_IPA_CONSTANT);
	check(nvrtx_sm75_get(&word, 0U, 12U) == 0x326U && nvrtx_sm75_get(&word, 16U, 8U) == 7U && nvrtx_sm75_get(&word, 64U, 8U) == 0x24U, "IPA opcode, destination, address in words");
	check(nvrtx_sm75_get(&word, 76U, 2U) == 0U && nvrtx_sm75_get(&word, 78U, 2U) == 1U && nvrtx_sm75_get(&word, 32U, 8U) == 255U && nvrtx_sm75_get(&word, 81U, 3U) == 7U, "IPA default location, constant, offset RZ, PT");

	/* TEX.2D.LL R8, R10, R2, R4, c[0x0][0x404]: the handle's word offset and buffer, both pairs, the sources, the mode. */
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_tex(&word, 8U, 10U, 2U, 4U, NVRTX_SM75_TEX_LOD_EXPLICIT, 0U, 0x404U);
	check(nvrtx_sm75_get(&word, 0U, 12U) == 0xb60U && nvrtx_sm75_get(&word, 40U, 14U) == 0x101U && nvrtx_sm75_get(&word, 54U, 5U) == 0U, "TEX opcode and constant buffer handle");
	check(nvrtx_sm75_get(&word, 16U, 8U) == 8U && nvrtx_sm75_get(&word, 64U, 8U) == 10U && nvrtx_sm75_get(&word, 24U, 8U) == 2U && nvrtx_sm75_get(&word, 32U, 8U) == 4U, "TEX destinations and sources");
	check(nvrtx_sm75_get(&word, 61U, 3U) == 1U && nvrtx_sm75_get(&word, 72U, 4U) == 0xfU && nvrtx_sm75_get(&word, 87U, 3U) == 3U && nvrtx_sm75_get(&word, 81U, 3U) == 7U && nvrtx_sm75_get(&word, 84U, 3U) == 1U, "TEX 2D, four channels, explicit level, no fault, normal eviction");
	check(nvrtx_sm75_get(&word, 76U, 3U) == 0U && nvrtx_sm75_get(&word, 59U, 1U) == 0U, "TEX without offset, derivatives or compare, not bindless");

	/* The same as a fetch with an offset: TLD, AOFFI at 76. */
	nvrtx_sm75_tex_fetch(&word);
	nvrtx_sm75_tex_offset(&word);
	check(nvrtx_sm75_get(&word, 0U, 12U) == 0xb66U && nvrtx_sm75_get(&word, 76U, 1U) == 1U && nvrtx_sm75_get(&word, 78U, 1U) == 0U && nvrtx_sm75_get(&word, 87U, 3U) == 3U, "TLD opcode, offset, not multisampled, explicit level");

	/* KILL under a guard: the predicate source PT. */
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_kill(&word);
	check(nvrtx_sm75_get(&word, 0U, 12U) == 0x95bU && nvrtx_sm75_get(&word, 87U, 3U) == 7U && nvrtx_sm75_get(&word, 90U, 1U) == 0U, "KILL predicate source PT");
}

/* Loops whose trip counts differ between invocations, nested. */
static void
check_loop(
	const char *directory)
{
	static struct test_run run;
	uint32_t o[64];
	uint32_t g;
	uint32_t i;
	uint32_t j;
	uint32_t n;
	uint32_t sum;
	int same;

	memset(&run, 0, sizeof(run));
	run.binary = compile_file(directory, "loop");
	check(run.binary != NULL, "loop compiles");
	if (run.binary == NULL)
		return;

	memset(o, 0, sizeof(o));
	sm75_machine_bind(&run, 0U, 0U, o, sizeof(o));
	run.groups[0] = 2U;
	run.groups[1] = 1U;
	run.groups[2] = 1U;
	sm75_machine_dispatch(&run);

	same = 1;
	for (g = 0U; g < 64U; g++) {
		n = g % 7U;
		sum = 0U;
		for (i = 0U; i < n; i++) {
			sum += i * g + 1U;
			for (j = 0U; j < (i & 1U) + 1U; j++)
				sum ^= j << i;
		}
		if (o[g] != sum)
			same = 0;
	}
	check(!run.failed && same, "loop: nested loops of different trip counts");

	nvrtx_sm75_binary_free((struct nvrtx_sm75_binary *)run.binary);
}

/* Unsigned and signed division and remainder over awkward operands. */
static void
check_division(
	const char *directory)
{
	static const uint32_t fixed[] = {
		100U, 7U, 0xffffffffU, 1U, 0xffffffffU, 0xffffffffU, 5U, 0xfffffffbU,
		0x80000001U, 3U, 0xfffffff9U, 2U, 7U, 0xfffffffeU, 0xfffffff9U, 0xfffffffeU,
		123456789U, 10U, 1U, 0x80000000U, 0x7fffffffU, 0x10001U, 0U, 3U
	};
	static struct test_run run;
	uint32_t v[64];
	uint32_t o[128];
	uint32_t seed;
	uint32_t g;
	uint32_t a;
	uint32_t b;
	int32_t modulo;
	int same;

	memset(&run, 0, sizeof(run));
	run.binary = compile_file(directory, "division");
	check(run.binary != NULL, "division compiles");
	if (run.binary == NULL)
		return;

	/* The fixed pairs, then pseudo-random ones (no zero divisor, no INT_MIN / -1). */
	seed = 12345U;
	for (g = 0U; g < 32U; g++) {
		if (g * 2U < sizeof(fixed) / 4U) {
			v[g * 2U] = fixed[g * 2U];
			v[g * 2U + 1U] = fixed[g * 2U + 1U];
			continue;
		}
		seed = seed * 1103515245U + 12345U;
		v[g * 2U] = seed ^ (seed >> 7);
		seed = seed * 1103515245U + 12345U;
		v[g * 2U + 1U] = (seed >> (seed & 31U)) | 1U;
	}
	memset(o, 0, sizeof(o));
	sm75_machine_bind(&run, 0U, 0U, v, sizeof(v));
	sm75_machine_bind(&run, 0U, 1U, o, sizeof(o));
	run.groups[0] = 1U;
	run.groups[1] = 1U;
	run.groups[2] = 1U;
	sm75_machine_dispatch(&run);

	same = 1;
	for (g = 0U; g < 32U; g++) {
		a = v[g * 2U];
		b = v[g * 2U + 1U];
		if (o[g * 4U] != a / b || o[g * 4U + 1U] != a % b) {
			printf("  %u / %u: %u %u\n", a, b, o[g * 4U], o[g * 4U + 1U]);
			same = 0;
		}
		/* GLSL's % on ints is OpSMod: the remainder takes the divisor's sign. */
		modulo = (int32_t)a % (int32_t)b;
		if (modulo != 0 && ((modulo < 0) != ((int32_t)b < 0)))
			modulo += (int32_t)b;
		if (o[g * 4U + 2U] != (uint32_t)((int32_t)a / (int32_t)b) || o[g * 4U + 3U] != (uint32_t)modulo) {
			printf("  %d / %d: %d %d\n", (int32_t)a, (int32_t)b, (int32_t)o[g * 4U + 2U], (int32_t)o[g * 4U + 3U]);
			same = 0;
		}
	}
	check(!run.failed && same, "division: unsigned and signed quotients and remainders");

	nvrtx_sm75_binary_free((struct nvrtx_sm75_binary *)run.binary);
}

/* Shared memory across barriers, and a shared atomic. */
static void
check_shared(
	const char *directory)
{
	static struct test_run run;
	uint32_t o[256];
	uint32_t g;
	uint32_t l;
	uint32_t base;
	int same;

	memset(&run, 0, sizeof(run));
	run.binary = compile_file(directory, "shared");
	check(run.binary != NULL, "shared compiles");
	if (run.binary == NULL)
		return;
	check(run.binary->barrier_count == 1U && run.binary->shared_bytes >= 65U * 4U, "shared: a barrier and the shared bytes");

	memset(o, 0, sizeof(o));
	sm75_machine_bind(&run, 0U, 0U, o, sizeof(o));
	run.groups[0] = 2U;
	run.groups[1] = 1U;
	run.groups[2] = 1U;
	sm75_machine_dispatch(&run);

	same = 1;
	for (g = 0U; g < 128U; g++) {
		l = g % 64U;
		base = g - l;
		if (o[g * 2U] != ((base + (l + 1U) % 64U) * 3U + 1U) + ((base + 63U - l) * 3U + 1U))
			same = 0;
		if (o[g * 2U + 1U] != 2016U)
			same = 0;
	}
	check(!run.failed && same, "shared: words across barriers and a shared atomic sum");

	nvrtx_sm75_binary_free((struct nvrtx_sm75_binary *)run.binary);
}

/*
 * The contraction of a product and a sum: contract.comp's x * y + z is one
 * FFMA and no FMUL; precise.comp's same arithmetic, decorated NoContraction,
 * never becomes an FFMA: the frontend marks the instructions of a precise
 * result and the compiler keeps them apart.
 */
static void
check_contraction(
	const char *directory)
{
	struct nvrtx_sm75_binary *binary;

	binary = compile_file(directory, "contract");
	check(binary != NULL, "contract.comp compiles");
	if (binary != NULL) {
		check(count_alu(binary, NVRTX_SM75_OP_FFMA) == 1U, "contract.comp: the product and the sum are one FFMA");
		check(count_alu(binary, NVRTX_SM75_OP_FMUL) == 0U, "contract.comp: no FMUL is left");
		nvrtx_sm75_binary_free(binary);
	}

	binary = compile_file(directory, "precise");
	check(binary != NULL, "precise.comp compiles");
	if (binary != NULL) {
		check(count_alu(binary, NVRTX_SM75_OP_FFMA) == 0U, "precise.comp: NoContraction keeps the product out of an FFMA");
		check(count_alu(binary, NVRTX_SM75_OP_FMUL) == 1U, "precise.comp: the product is an FMUL");
		check(count_alu(binary, NVRTX_SM75_OP_FADD) == 1U, "precise.comp: the sum is an FADD");
		nvrtx_sm75_binary_free(binary);
	}
}

/* Counts the instructions of a program with an ALU opcode (its 9 bits, any form). */
static uint32_t
count_alu(
	const struct nvrtx_sm75_binary *binary,
	uint32_t opcode)
{
	uint32_t index;
	uint32_t count;
	uint32_t form;

	/* Each instruction of an ALU form (1 to 5) with the opcode. */
	count = 0U;
	for (index = 0U; index < binary->code_words / 4U; index++) {
		form = (binary->code[index * 4U] >> 9) & 7U;
		if (form < 1U || form > 5U)
			continue;
		if ((binary->code[index * 4U] & 0x1ffU) == opcode)
			count++;
	}

	/* Succeeded: the count. */
	return count;
}

/* Storage buffer atomics (ws101's atomic.comp). */
static void
check_atomic(
	const char *directory)
{
	static struct test_run run;
	uint32_t v[128];
	uint32_t h[28];
	uint32_t old[128];
	uint32_t want[28];
	uint8_t seen[128];
	uint32_t n;
	uint32_t i;
	uint32_t x;
	int same;

	memset(&run, 0, sizeof(run));
	run.binary = compile_file(directory, "atomic");
	check(run.binary != NULL, "atomic compiles");
	if (run.binary == NULL)
		return;

	/* The values, the bins' start, n = 100. */
	for (i = 0U; i < 128U; i++)
		v[i] = i * 37U + 5U;
	memset(h, 0, sizeof(h));
	h[17] = 0x7fffffffU;
	h[18] = 0x80000000U;
	h[19] = 0xffffffffU;
	h[20] = 0U;
	h[21] = 0xffffffffU;
	memcpy(want, h, sizeof(want));
	memset(old, 0xff, sizeof(old));
	n = 100U;
	memcpy(&run.cb[0][NVRTX_SM75_CB_PUSH], &n, sizeof(n));
	sm75_machine_bind(&run, 0U, 0U, v, sizeof(v));
	sm75_machine_bind(&run, 0U, 1U, h, sizeof(h));
	sm75_machine_bind(&run, 0U, 2U, old, sizeof(old));
	run.groups[0] = 2U;
	run.groups[1] = 1U;
	run.groups[2] = 1U;
	sm75_machine_dispatch(&run);

	/* The same in C. */
	for (i = 0U; i < n; i++) {
		x = v[i];
		want[x % 16U]++;
		want[16] += x;
		if ((int32_t)(x - 1000U) < (int32_t)want[17])
			want[17] = x - 1000U;
		if ((int32_t)(x - 1000U) > (int32_t)want[18])
			want[18] = x - 1000U;
		if (x < want[19])
			want[19] = x;
		if (x > want[20])
			want[20] = x;
		want[21] &= x | 0xffff0000U;
		want[22] |= x;
		want[23] ^= x;
		want[24] = 7U;
		if (want[25] == 0U)
			want[25] = 1U;
		want[26]++;
	}
	same = memcmp(h, want, sizeof(h)) == 0;
	memset(seen, 0, sizeof(seen));
	for (i = 0U; i < n; i++) {
		if (old[i] >= n || seen[old[i]])
			same = 0;
		else
			seen[old[i]] = 1U;
	}
	if (old[n] != 0xffffffffU)
		same = 0;
	check(!run.failed && same, "atomic: every storage atomic, predicated, returning the old value");

	nvrtx_sm75_binary_free((struct nvrtx_sm75_binary *)run.binary);
}

/*
 * The compositor's quad (userland/desktop/wayland/shaders/quad.vert and
 * quad.frag): the vertex program's position and texture coordinate from
 * the push constants and the corner, and the fragment program's sample of
 * the interpolated coordinate into colour 0; and their headers.
 */
static void
check_quad(
	const char *directory)
{
	static struct test_run run;
	static struct test_thread thread;
	struct nvrtx_sm75_binary *binary;
	const uint32_t group[3] = { 0U, 0U, 0U };
	float push[8];
	float corner[2];
	float want;
	uint32_t handle;
	uint32_t index;
	int same;

	/* The vertex program. */
	binary = compile_graphics(directory, "quad-vert", DRV_GPU_STAGE_VERTEX);
	check(binary != NULL, "quad.vert compiles");
	if (binary == NULL)
		return;

	/* Its header: VTG version 4, a vertex program, the corner read, the position and coordinate written. */
	check(field(binary->sph, 0U, 5U) == 1U && field(binary->sph, 5U, 5U) == 4U && field(binary->sph, 10U, 4U) == 1U && field(binary->sph, 17U, 4U) == 1U, "quad.vert header type, version, stage");
	check(field(binary->sph, 192U, 2U) == 3U && field(binary->sph, 194U, 126U) == 0U, "quad.vert reads the corner's two words");
	check(field(binary->sph, 400U + 28U, 4U) == 0xfU && field(binary->sph, 432U, 2U) == 3U, "quad.vert writes the position and the coordinate");
	check(field(binary->sph, 140U, 8U) == 28U && field(binary->sph, 152U, 8U) == 33U, "quad.vert store request");

	/* Runs it for the corner (0.25, 0.75). */
	memset(&run, 0, sizeof(run));
	run.binary = binary;
	push[0] = -1.0f;
	push[1] = -0.5f;
	push[2] = 1.0f;
	push[3] = 0.5f;
	push[4] = 0.0f;
	push[5] = 0.1f;
	push[6] = 1.0f;
	push[7] = 0.9f;
	memcpy(&run.cb[0][NVRTX_SM75_CB_PUSH], push, sizeof(push));
	corner[0] = 0.25f;
	corner[1] = 0.75f;
	run.attr_in[0x80U / 4U] = sm75_machine_bits(corner[0]);
	run.attr_in[0x84U / 4U] = sm75_machine_bits(corner[1]);
	memset(&thread, 0, sizeof(thread));
	thread.p[7] = 1;
	sm75_machine_invoke(&run, &thread, group);

	same = !run.failed && thread.state == TEST_DONE;
	for (index = 0U; index < 2U; index++) {
		want = push[index] + (push[2U + index] - push[index]) * corner[index];
		if (!near(sm75_machine_float(run.attr_out[0x70U / 4U + index]), want))
			same = 0;
		want = push[4U + index] + (push[6U + index] - push[4U + index]) * corner[index];
		if (!near(sm75_machine_float(run.attr_out[0x80U / 4U + index]), want))
			same = 0;
	}
	if (sm75_machine_float(run.attr_out[0x78U / 4U]) != 0.0f || sm75_machine_float(run.attr_out[0x7cU / 4U]) != 1.0f)
		same = 0;
	check(same, "quad.vert: position and texture coordinate");
	nvrtx_sm75_binary_free(binary);

	/* The fragment program. */
	binary = compile_graphics(directory, "quad-frag", DRV_GPU_STAGE_FRAGMENT);
	check(binary != NULL, "quad.frag compiles");
	if (binary == NULL)
		return;

	/* Its header: PS version 4, a pixel program, multiple targets, the coordinate in perspective, w, colour 0. */
	check(field(binary->sph, 0U, 5U) == 2U && field(binary->sph, 5U, 5U) == 4U && field(binary->sph, 10U, 4U) == 5U && field(binary->sph, 14U, 1U) == 1U, "quad.frag header type, version, stage, targets");
	check(field(binary->sph, 192U, 4U) == 0xaU && field(binary->sph, 160U + 31U, 1U) == 1U, "quad.frag interpolates the coordinate in perspective and reads w");
	check(field(binary->sph, 576U, 32U) == 0xfU && field(binary->sph, 15U, 1U) == 0U, "quad.frag writes colour 0 and does not discard");
	check(binary->texture_count == 1U && binary->textures[0].binding == 0U, "quad.frag one sampled image");

	/* Runs it for the coordinate (0.3, 0.6) and the handle 77. */
	memset(&run, 0, sizeof(run));
	run.binary = binary;
	handle = 77U;
	memcpy(&run.cb[0][NVRTX_SM75_CB_TEXTURES], &handle, sizeof(handle));
	run.attr_in[0x80U / 4U] = sm75_machine_bits(0.3f);
	run.attr_in[0x84U / 4U] = sm75_machine_bits(0.6f);
	run.attr_in[0x7cU / 4U] = sm75_machine_bits(2.0f);
	memset(&thread, 0, sizeof(thread));
	thread.p[7] = 1;
	sm75_machine_invoke(&run, &thread, group);

	same = !run.failed && thread.state == TEST_DONE && !run.killed;
	if (!near(sm75_machine_float(run.exit_regs[0]), 0.3f) || !near(sm75_machine_float(run.exit_regs[1]), 0.6f))
		same = 0;
	if (sm75_machine_float(run.exit_regs[2]) != 77.0f || sm75_machine_float(run.exit_regs[3]) != 1.0f)
		same = 0;
	check(same, "quad.frag: the sample of the interpolated coordinate in R0..R3");
	nvrtx_sm75_binary_free(binary);
}

/* Reads DIR/NAME.spv and compiles a stage of it; NULL (with the reason printed) when refused. */
static struct nvrtx_sm75_binary *
compile_graphics(
	const char *directory,
	const char *name,
	enum drv_gpu_shader_stage stage)
{
	struct nvrtx_sm75_binary *binary;
	struct nvrtx_sm75_diagnostic diagnostic;
	char path[512];
	uint32_t *words;
	long bytes;
	FILE *file;
	int error;

	snprintf(path, sizeof(path), "%s/%s.spv", directory, name);
	file = fopen(path, "rb");
	if (file == NULL) {
		printf("  %s: cannot open\n", path);
		return NULL;
	}
	fseek(file, 0, SEEK_END);
	bytes = ftell(file);
	fseek(file, 0, SEEK_SET);
	words = malloc((size_t)bytes);
	if (words == NULL || fread(words, 1U, (size_t)bytes, file) != (size_t)bytes) {
		fclose(file);
		free(words);
		return NULL;
	}
	fclose(file);

	binary = NULL;
	memset(&diagnostic, 0, sizeof(diagnostic));
	error = nvrtx_sm75_compile_stage_sm(words, (size_t)bytes / 4U, stage, test_sm, &binary, &diagnostic);
	free(words);
	if (error != 0) {
		printf("  %s: refused %d: %s (instruction %u)\n", name, error, (diagnostic.reason != NULL) ? diagnostic.reason : "?", diagnostic.instruction);
		return NULL;
	}
	printf("  %s: %u instructions, %u registers\n", name, binary->code_words / 4U, binary->gpr_count);
	return binary;
}

/* Checks the QMD 2.2 fields against the positions of clc3c0qmd.h, written out here on their own. */
static void
check_qmd(void)
{
	struct nvrtx_qmd_info info;
	uint32_t qmd[NVRTX_QMD_WORDS];
	int error;

	memset(&info, 0, sizeof(info));
	info.program = 0x28006000ULL;
	info.groups[0] = 3U;
	info.groups[1] = 2U;
	info.groups[2] = 1U;
	info.local_size[0] = 32U;
	info.local_size[1] = 1U;
	info.local_size[2] = 1U;
	info.gpr_count = 8U;
	info.cbuf_address[0] = 0x128008000ULL;
	info.cbuf_size[0] = 0x300U;
	error = nvrtx_qmd_build(qmd, &info);
	check(error == 0, "QMD builds");

	/* Version 2.2, global caching, no call limit. */
	check(field(qmd, 576U, 4U) == 2U && field(qmd, 580U, 4U) == 2U, "QMD version 2.2");
	check(field(qmd, 134U, 1U) == 1U && field(qmd, 378U, 1U) == 1U && field(qmd, 382U, 1U) == 0U, "QMD caching, call limit, sampler index");

	/* The grid, the workgroup, the registers. */
	check(field(qmd, 384U, 32U) == 3U && field(qmd, 416U, 16U) == 2U && field(qmd, 448U, 16U) == 1U, "QMD grid");
	check(field(qmd, 592U, 16U) == 32U && field(qmd, 608U, 16U) == 1U && field(qmd, 624U, 16U) == 1U, "QMD workgroup");
	check(field(qmd, 648U, 9U) == 8U && field(qmd, 955U, 5U) == 0U, "QMD registers and barriers");

	/* No shared memory: 32 KiB carve-outs for one and for many, 64 KiB at most (codes 9, 9, 17). */
	check(field(qmd, 544U, 18U) == 0U && field(qmd, 562U, 7U) == 9U && field(qmd, 657U, 7U) == 9U && field(qmd, 569U, 7U) == 17U, "QMD shared memory carve-outs");

	/* Constant buffer 0 and the program. */
	check(field(qmd, 640U, 1U) == 1U && field(qmd, 641U, 7U) == 0U, "QMD constant buffer 0 valid alone");
	check(field(qmd, 1024U, 32U) == 0x28008000U && field(qmd, 1056U, 17U) == 1U && field(qmd, 1075U, 13U) == 0x30U, "QMD constant buffer 0 address and size");
	check(field(qmd, 1536U, 32U) == 0x28006000U && field(qmd, 1568U, 17U) == 0U, "QMD program address");

	/* A misaligned program is refused. */
	info.program = 0x28006010ULL;
	error = nvrtx_qmd_build(qmd, &info);
	check(error == EINVAL, "QMD refuses a misaligned program");
}

/* Checks the launch's push buffer word by word (incrementing headers on subchannel 1). */
static void
check_launch_push(void)
{
	static const uint32_t expect[] = {
		0x20012000U, 0x0000c5c0U,
		0x200220a8U, 0x00000000U, 0xfe000000U,
		0x200221ecU, 0x00000000U, 0xff000000U,
		0x20012087U, 0x00001011U,
		0x200120adU, 0x00280070U,
		0x200120afU, 0x00000003U
	};
	uint8_t push[256];
	uint32_t word;
	size_t length;
	size_t index;
	int same;

	length = nvrtx_push_compute_launch(push, 0x28007000ULL);
	check(length == sizeof(expect), "launch push length");

	same = 1;
	for (index = 0U; index < sizeof(expect) / 4U && index * 4U < length; index++) {
		memcpy(&word, push + index * 4U, 4U);
		if (word != expect[index]) {
			printf("  push word %u is 0x%08x, want 0x%08x\n", (unsigned)index, word, expect[index]);
			same = 0;
		}
	}
	check(same, "launch push words");
}

/* Compiles the first program from its IR and runs it: 32 words of 42. */
static void
check_first_program(void)
{
	static struct test_run run;
	static struct nvrtx_compute_first first;
	struct nvrtx_sm75_binary *binary;
	struct nvrtx_sm75_diagnostic diagnostic;
	uint32_t out[40];
	uint32_t index;
	int same;
	int error;

	nvrtx_compute_first_ir(&first);
	binary = NULL;
	error = nvrtx_sm75_compile_ir_sm(&first.ir, test_sm, &binary, &diagnostic);
	check(error == 0 && binary != NULL, "the first program compiles from its IR");
	if (binary == NULL)
		return;
	printf("  first: %u instructions, %u registers\n", binary->code_words / 4U, binary->gpr_count);

	/* One group of 32 over 40 words. */
	memset(&run, 0, sizeof(run));
	run.binary = binary;
	for (index = 0U; index < 40U; index++)
		out[index] = 0xdeadbeefU;
	sm75_machine_bind(&run, 0U, 0U, out, 32U * 4U);
	run.groups[0] = 1U;
	run.groups[1] = 1U;
	run.groups[2] = 1U;
	sm75_machine_dispatch(&run);

	same = 1;
	for (index = 0U; index < 32U; index++) {
		if (out[index] != NVRTX_COMPUTE_FIRST_VALUE)
			same = 0;
	}
	if (out[32] != 0xdeadbeefU)
		same = 0;
	check(!run.failed && same, "the first program writes 42 at each of 32 words");

	nvrtx_sm75_binary_free(binary);
}

/* Checks the system-memory page entries of a graphics range's tables. */
static void
check_system_pages(void)
{
	static uint8_t image[0x4000U + 66U * 0x1000U];
	uint64_t pages[2];
	uint64_t entry;
	size_t length;
	int error;

	length = nvrtx_mmu_gr_tables_bytes(0x8200000ULL);
	check(length == 0x4000U + 65U * 0x1000U, "tables of 130 MiB take 65 page tables");
	error = nvrtx_mmu_gr_tables(image, 0x1f0000000ULL, 0x20000000ULL, 0x8200000ULL);
	check(error == 0, "tables of the compute range build");

	/* Two pages at 0x28000000: page tables 64, entries 0 and 1. */
	pages[0] = 0x123456000ULL;
	pages[1] = 0x00789a000ULL;
	error = nvrtx_mmu_gr_map_system(image, 0x20000000ULL, 0x8200000ULL, 0x28000000ULL, pages, 2U);
	check(error == 0, "system pages map");
	memcpy(&entry, image + 0x4000U + 64U * 0x1000U, 8U);
	check(entry == ((0x123456000ULL >> 4) | 0x1U | 0x4U | 0x8U), "system page entry: valid, coherent system, volatile, not privileged");
	memcpy(&entry, image + 0x4000U + 64U * 0x1000U + 8U, 8U);
	check(entry == ((0x00789a000ULL >> 4) | 0xdU), "second system page entry");

	/* Pages past the range are refused. */
	error = nvrtx_mmu_gr_map_system(image, 0x20000000ULL, 0x8200000ULL, 0x281ff000ULL, pages, 2U);
	check(error == EINVAL, "system pages past the range are refused");

	/* The draw's VRAM target at 0x28100000: page table 64, entry 256, valid VRAM, neither privileged nor read-only. */
	error = nvrtx_mmu_gr_map_vram(image, 0x20000000ULL, 0x8200000ULL, 0x28100000ULL, 0x1f1f00000ULL, 0x40000ULL);
	check(error == 0, "VRAM work pages map");
	memcpy(&entry, image + 0x4000U + 64U * 0x1000U + 256U * 8U, 8U);
	check(entry == ((0x1f1f00000ULL >> 4) | 0x1U), "VRAM work page entry");
	memcpy(&entry, image + 0x4000U + 64U * 0x1000U + 319U * 8U, 8U);
	check(entry == (((0x1f1f00000ULL + 63U * 0x1000U) >> 4) | 0x1U), "last VRAM work page entry");
}

/*
 * Builds one draw's push buffer as draw.c does and reads the methods back
 * by their numbers in clc597.h, written out here on their own: the class,
 * colour target 0, the viewport, the pipeline, the vertex stream, constant
 * buffer 0, the clear and the draw.
 */
static void
check_draw_push(void)
{
	static uint8_t push[0x4000];
	static uint32_t state[0x10000 / 4U];
	static uint8_t seen[0x10000 / 4U];
	struct nvrtx_draw_target target;
	const uint32_t colour[4] = { 0U, 0U, 0x3f800000U, 0x3f800000U };
	uint32_t first;
	size_t at;
	int error;

	/* The whole first submission of the draw. */
	target.address = 0x28100000ULL;
	target.width = 256U;
	target.height = 256U;
	target.pitch = 1024U;
	at = nvrtx_push_draw_init(push, 0U, 0x28011000ULL);
	at = nvrtx_push_draw_constants(push, at, 0x28018000ULL, 0x1000U);
	at = nvrtx_push_draw_target(push, at, &target);
	at = nvrtx_push_draw_invalidate(push, at);
	at = nvrtx_push_draw_pipeline(push, at, 0x28014000ULL, 12U, 0x28015000ULL, 8U);
	at = nvrtx_push_draw_vertices(push, at, 0x28019000ULL, 48U, 8U);
	at = nvrtx_push_draw_textures(push, at, 0x2801b000ULL, 1U, 0x2801c000ULL, 1U);
	at = nvrtx_push_draw_clear(push, at, colour, 256U, 256U);
	at = nvrtx_push_draw_triangles(push, at, 0U, 6U);
	printf("  draw push: %u bytes\n", (unsigned)at);
	check(at < 0x2000U, "draw push fits two pages");

	/* Every header is incrementing on subchannel 0. */
	memcpy(&first, push, 4U);
	check(first == 0x20010000U, "draw push starts with SET_OBJECT on subchannel 0");
	error = decode_push(push, at, state, seen);
	check(error == 0, "draw push decodes");

	/* The class, and the queue state NVK starts with. */
	check(state[0x0000U / 4U] == 0xc597U, "SET_OBJECT TURING_A");
	check(state[0x1558U / 4U] == 1U && state[0x16a8U / 4U] == 0x30003U && state[0x1794U / 4U] == 0x20002U, "render enable, SPH and AAM versions");
	check(state[0x1610U / 4U] == 0xeU && state[0x164cU / 4U] == 0x1000U && state[0x077cU / 4U] == 0xff000000U, "attribute default, vertex id, local window");
	check(state[0x0f84U / 4U] == 0U && state[0x0f88U / 4U] == 0x28011000U, "vertex substitute at the zero page");
	check(seen[0x0e00U / 4U] && seen[(0x0e00U + 15U * 16U) / 4U] && state[(0x0e00U + 15U * 16U) / 4U] == 0U, "every scissor off");
	check(seen[(0x1c00U + 31U * 16U) / 4U] && state[(0x1c00U + 31U * 16U) / 4U] == 0U, "every other vertex stream off");

	/* Colour target 0 (0x800..0x820) pitch-linear A8B8G8R8, the others null. */
	check(state[0x0800U / 4U] == 0U && state[0x0804U / 4U] == 0x28100000U, "colour target 0 address");
	check(state[0x0808U / 4U] == 1024U && state[0x080cU / 4U] == 256U, "colour target 0 pitch and height");
	check(state[0x0810U / 4U] == 0xd5U && state[0x0814U / 4U] == 0x1000U && state[0x0818U / 4U] == 1U, "colour target 0 format, pitch layout, one layer");
	check(state[0x19e0U / 4U] == 0U && state[0x19e4U / 4U] == 1U, "colour target 0 not compressed");
	check(state[(0x0800U + 7U * 64U + 0x10U) / 4U] == 0U && state[(0x0800U + 7U * 64U + 8U) / 4U] == 64U, "colour target 7 null");
	check(state[0x121cU / 4U] == 1U && state[0x1538U / 4U] == 0U && state[0x15d0U / 4U] == 0U, "one target, no depth target, one sample");
	check(state[0x0ff4U / 4U] == (256U << 16) && state[0x0ff8U / 4U] == (256U << 16), "surface clip");

	/* The viewport: scale and offset 128, depth 0 to 1. */
	check(state[0x0a00U / 4U] == 0x43000000U && state[0x0a04U / 4U] == 0x43000000U && state[0x0a08U / 4U] == 0x3f800000U, "viewport scale");
	check(state[0x0a0cU / 4U] == 0x43000000U && state[0x0a10U / 4U] == 0x43000000U && state[0x0a14U / 4U] == 0U, "viewport offset");
	check(state[0x0c00U / 4U] == (256U << 16) && state[0x0c0cU / 4U] == 0x3f800000U && state[0x0a18U / 4U] == 0x6420U, "viewport clip and swizzle");
	check(state[0x193cU / 4U] == 0x181dU && state[0x0dacU / 4U] == 0x1b02U && state[0x1918U / 4U] == 0U, "clip control, fill, no cull");
	check(state[0x1a00U / 4U] == 0x1111U && state[0x1360U / 4U] == 0U && state[0x0fbcU / 4U] == 0xffffU && state[0x12ccU / 4U] == 0U, "writes, no blend, sample mask, no depth test");

	/* The pipeline: vertex at index 1, pixel at 5, the rest off. */
	check(!seen[0x2000U / 4U], "stage 0 left alone, as NVK leaves it");
	check(state[0x2080U / 4U] == 0x20U && state[0x20c0U / 4U] == 0x30U && state[0x2100U / 4U] == 0x40U, "stages 2, 3, 4 off");
	check(state[0x2040U / 4U] == 0x11U && state[0x2054U / 4U] == 0U && state[0x2058U / 4U] == 0x28014000U, "vertex program");
	check(state[0x204cU / 4U] == 12U && state[0x2050U / 4U] == 0U, "vertex registers and group 0");
	check(state[0x2140U / 4U] == 0x51U && state[0x2158U / 4U] == 0x28015000U && state[0x214cU / 4U] == 8U && state[0x2150U / 4U] == 4U, "pixel program, registers, group 4");
	check(seen[0x0f1cU / 4U] && state[0x0f1cU / 4U] == 0U, "no post-depth coverage (SET_POST_Z_PS_IMASK)");
	check(seen[0x2a00U / 4U] && seen[(0x2a00U + 15U * 16U) / 4U] && state[0x2a00U / 4U] == 0U && state[(0x2a00U + 15U * 16U) / 4U] == 0U, "every variable pixel rate shading control off");
	check(state[0x0754U / 4U] == 1U, "hybrid anti-alias control: one pass, centroid per fragment");
	check(!seen[0x021cU / 4U] && state[0x0da4U / 4U] == 0x1011U, "shader caches invalidated without a wait for idle");

	/* Constant buffer 0 selected and bound to the five groups. */
	check(state[0x2380U / 4U] == 0x1000U && state[0x2384U / 4U] == 0U && state[0x2388U / 4U] == 0x28018000U, "constant buffer 0 selected");
	check(state[0x2410U / 4U] == 1U && state[(0x2410U + 4U * 32U) / 4U] == 1U, "constant buffer 0 bound to groups 0 and 4");

	/* Vertex stream 0 and attribute 0. */
	check(state[0x1160U / 4U] == 0x38800000U && state[0x1164U / 4U] == 0x38200040U, "attribute 0 two floats, attribute 1 inactive");
	check(state[0x1c00U / 4U] == 0x1008U && state[0x1c04U / 4U] == 0U && state[0x1c08U / 4U] == 0x28019000U, "vertex stream 0");
	check(state[0x0600U / 4U] == 0U && state[0x0604U / 4U] == 48U, "vertex stream 0 size");

	/* The texture pools. */
	check(state[0x1578U / 4U] == 0x2801b000U && state[0x157cU / 4U] == 0U && state[0x1560U / 4U] == 0x2801c000U, "texture header and sampler pools");

	/* The clear and the draw. */
	check(state[0x10f8U / 4U] == 0x10U && state[0x0d88U / 4U] == 0x3f800000U && state[0x0d6cU / 4U] == (256U << 16), "clear control, colour, rectangle");
	check(state[0x19d0U / 4U] == 0x3cU && state[0x1944U / 4U] == 0U, "clear of RGBA, render enable back");
	check(state[0x0220U / 4U] == 1U && state[0x1618U / 4U] == 0x80000004U, "one instance, BEGIN triangles");
	check(state[0x0d74U / 4U] == 0U && state[0x0d78U / 4U] == 6U && seen[0x1614U / 4U], "six vertices, END");
}

/* Checks the texture header and the sampler against the positions of clc597tex.h. */
static void
check_draw_descriptors(void)
{
	struct nvrtx_draw_texture texture;
	uint32_t header[NVRTX_DRAW_TIC_WORDS];
	uint32_t sampler[NVRTX_DRAW_TSC_WORDS];

	/* A 4x4 texture at a high address. */
	texture.address = 0x1234567a00ULL;
	texture.width = 4U;
	texture.height = 4U;
	texture.pitch = 256U;
	nvrtx_draw_texture_header(header, &texture);

	/* The format and its swizzle. */
	check(field(header, 0U, 7U) == 8U && field(header, 7U, 3U) == 2U && field(header, 16U, 3U) == 2U, "TIC A8B8G8R8 UNORM");
	check(field(header, 19U, 3U) == 2U && field(header, 22U, 3U) == 3U && field(header, 25U, 3U) == 4U && field(header, 28U, 3U) == 5U, "TIC sources R G B A");

	/* The address, the header version, the pitch, the size. */
	check(field(header, 37U, 27U) == (0x34567a00U >> 5) && field(header, 64U, 17U) == 0x12U, "TIC address");
	check(field(header, 85U, 3U) == 2U && field(header, 96U, 16U) == 256U / 32U, "TIC pitch version and pitch");
	check(field(header, 128U, 17U) == 3U && field(header, 160U, 16U) == 3U && field(header, 176U, 14U) == 0U, "TIC size");
	check(field(header, 151U, 4U) == 7U && field(header, 191U, 1U) == 1U && field(header, 124U, 4U) == 0U, "TIC 2D no mipmap, normalized, one level");
	check(field(header, 112U, 3U) == 7U && field(header, 155U, 2U) == 1U && field(header, 157U, 3U) == 7U, "TIC quality, promotion, border");

	/* The sampler. */
	nvrtx_draw_sampler(sampler);
	check(field(sampler, 0U, 9U) == 0x92U && field(sampler, 13U, 1U) == 1U && field(sampler, 14U, 6U) == 9U, "TSC clamp, sRGB border, font filter");
	check(field(sampler, 32U, 3U) == 1U && field(sampler, 36U, 2U) == 1U && field(sampler, 38U, 2U) == 2U && field(sampler, 40U, 2U) == 2U, "TSC point filters, cube seams");
	check(field(sampler, 64U, 24U) == 0U, "TSC levels of detail 0 to 0");
}

/*
 * Compiles the draw's programs from their IR and runs them: the vertex
 * program places a corner of the quad, the one-colour fragment program
 * writes the push constants' colour, the textured one samples at the
 * interpolated coordinate.  Each header is compared word for word with
 * the one NAK's encode_header (sph.rs, from_nir.rs) gives the same
 * program: word 0 the type, version 4, stage, SASS version 1 (and a
 * fragment program's multiple targets); word 4 a vertex program's store
 * request over its stores (0x1c, the position's x, to 0x1f or 0x21);
 * word 5 bit 31 the position's w, always in a fragment program's input
 * map; word 6 the generic input map; word 13 the vertex output map;
 * word 18 colour 0's four channels.
 */
static void
check_draw_programs(void)
{
	static const uint32_t vertex_header[NVRTX_SM75_SPH_WORDS] = {
		[0] = 0x00020481U, [4] = 0x1f01c000U, [6] = 0x00000003U, [13] = 0x0000f000U
	};
	static const uint32_t textured_vertex_header[NVRTX_SM75_SPH_WORDS] = {
		[0] = 0x00020481U, [4] = 0x2101c000U, [6] = 0x00000003U, [13] = 0x0003f000U
	};
	static const uint32_t fragment_header[NVRTX_SM75_SPH_WORDS] = {
		[0] = 0x00025482U, [5] = 0x80000000U, [18] = 0x0000000fU
	};
	static const uint32_t textured_fragment_header[NVRTX_SM75_SPH_WORDS] = {
		[0] = 0x00025482U, [5] = 0x80000000U, [6] = 0x0000000aU, [18] = 0x0000000fU
	};
	static struct test_run run;
	static struct test_thread thread;
	static struct nvrtx_draw_ir program;
	struct nvrtx_sm75_binary *binary;
	const uint32_t group[3] = { 0U, 0U, 0U };
	float push[12] = { -0.5f, -0.5f, 0.5f, 0.5f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 1.0f };
	uint32_t handle;
	uint32_t index;
	int same;

	/* The one-colour vertex program at the corner (1, 0): (0.5, -0.5, 0, 1). */
	nvrtx_draw_vertex_ir(&program, 0);
	binary = compile_draw_ir(&program, "draw vertex");
	check(binary != NULL, "the draw's vertex program compiles");
	if (binary == NULL)
		return;
	check(binary->graphics != 0U && field(binary->sph, 10U, 4U) == 1U && field(binary->sph, 400U + 28U, 4U) == 0xfU, "draw vertex header: a vertex program writing the position");
	check(field(binary->sph, 432U, 2U) == 0U, "draw vertex writes no generic attribute");
	check(header_is(binary->sph, vertex_header), "draw vertex header: NAK's words in full");
	memset(&run, 0, sizeof(run));
	run.binary = binary;
	memcpy(&run.cb[0][NVRTX_SM75_CB_PUSH], push, sizeof(push));
	run.attr_in[0x80U / 4U] = sm75_machine_bits(1.0f);
	run.attr_in[0x84U / 4U] = sm75_machine_bits(0.0f);
	memset(&thread, 0, sizeof(thread));
	thread.p[7] = 1;
	sm75_machine_invoke(&run, &thread, group);
	same = !run.failed && thread.state == TEST_DONE;
	if (sm75_machine_float(run.attr_out[0x70U / 4U]) != 0.5f || sm75_machine_float(run.attr_out[0x74U / 4U]) != -0.5f)
		same = 0;
	if (sm75_machine_float(run.attr_out[0x78U / 4U]) != 0.0f || sm75_machine_float(run.attr_out[0x7cU / 4U]) != 1.0f)
		same = 0;
	check(same, "draw vertex: the corner's position");
	nvrtx_sm75_binary_free(binary);

	/* The textured vertex program at the corner (0, 1): the coordinate (0, 1). */
	nvrtx_draw_vertex_ir(&program, 1);
	binary = compile_draw_ir(&program, "draw textured vertex");
	check(binary != NULL, "the draw's textured vertex program compiles");
	if (binary == NULL)
		return;
	check(field(binary->sph, 432U, 2U) == 3U, "draw textured vertex writes the coordinate");
	check(header_is(binary->sph, textured_vertex_header), "draw textured vertex header: NAK's words in full");
	memset(&run, 0, sizeof(run));
	run.binary = binary;
	memcpy(&run.cb[0][NVRTX_SM75_CB_PUSH], push, sizeof(push));
	run.attr_in[0x80U / 4U] = sm75_machine_bits(0.0f);
	run.attr_in[0x84U / 4U] = sm75_machine_bits(1.0f);
	memset(&thread, 0, sizeof(thread));
	thread.p[7] = 1;
	sm75_machine_invoke(&run, &thread, group);
	same = !run.failed && thread.state == TEST_DONE;
	if (sm75_machine_float(run.attr_out[0x70U / 4U]) != -0.5f || sm75_machine_float(run.attr_out[0x74U / 4U]) != 0.5f)
		same = 0;
	if (sm75_machine_float(run.attr_out[0x80U / 4U]) != 0.0f || sm75_machine_float(run.attr_out[0x84U / 4U]) != 1.0f)
		same = 0;
	check(same, "draw textured vertex: position and coordinate");
	nvrtx_sm75_binary_free(binary);

	/* The one-colour fragment program: R0..R3 = (1, 1, 0, 1). */
	nvrtx_draw_fragment_ir(&program, 0);
	binary = compile_draw_ir(&program, "draw fragment");
	check(binary != NULL, "the draw's fragment program compiles");
	if (binary == NULL)
		return;
	check(field(binary->sph, 10U, 4U) == 5U && field(binary->sph, 576U, 32U) == 0xfU && field(binary->sph, 192U, 64U) == 0U, "draw fragment header: colour 0, no input");
	check(field(binary->sph, 160U + 31U, 1U) == 1U, "draw fragment header maps the position's w though it does not read it");
	check(header_is(binary->sph, fragment_header), "draw fragment header: NAK's words in full");
	memset(&run, 0, sizeof(run));
	run.binary = binary;
	memcpy(&run.cb[0][NVRTX_SM75_CB_PUSH], push, sizeof(push));
	memset(&thread, 0, sizeof(thread));
	thread.p[7] = 1;
	sm75_machine_invoke(&run, &thread, group);
	same = !run.failed && thread.state == TEST_DONE;
	for (index = 0U; index < 4U; index++) {
		if (sm75_machine_float(run.exit_regs[index]) != push[8U + index])
			same = 0;
	}
	check(same, "draw fragment: the push constants' colour in R0..R3");
	nvrtx_sm75_binary_free(binary);

	/* The textured fragment program: the sample of handle 5 at (0.25, 0.75). */
	nvrtx_draw_fragment_ir(&program, 1);
	binary = compile_draw_ir(&program, "draw textured fragment");
	check(binary != NULL, "the draw's textured fragment program compiles");
	if (binary == NULL)
		return;
	check(binary->texture_count == 1U && field(binary->sph, 192U, 4U) == 0xaU, "draw textured fragment: one image, the coordinate in perspective");
	check(header_is(binary->sph, textured_fragment_header), "draw textured fragment header: NAK's words in full");
	memset(&run, 0, sizeof(run));
	run.binary = binary;
	handle = 5U;
	memcpy(&run.cb[0][NVRTX_SM75_CB_TEXTURES], &handle, sizeof(handle));
	run.attr_in[0x80U / 4U] = sm75_machine_bits(0.25f);
	run.attr_in[0x84U / 4U] = sm75_machine_bits(0.75f);
	run.attr_in[0x7cU / 4U] = sm75_machine_bits(2.0f);
	memset(&thread, 0, sizeof(thread));
	thread.p[7] = 1;
	sm75_machine_invoke(&run, &thread, group);
	same = !run.failed && thread.state == TEST_DONE;
	if (!near(sm75_machine_float(run.exit_regs[0]), 0.25f) || !near(sm75_machine_float(run.exit_regs[1]), 0.75f) || sm75_machine_float(run.exit_regs[2]) != 5.0f)
		same = 0;
	check(same, "draw textured fragment: the sample in R0..R3");
	nvrtx_sm75_binary_free(binary);
}

/* Compiles one of the draw's programs from its IR; NULL (with the reason printed) when refused. */
static struct nvrtx_sm75_binary *
compile_draw_ir(
	const struct nvrtx_draw_ir *program,
	const char *name)
{
	struct nvrtx_sm75_binary *binary;
	struct nvrtx_sm75_diagnostic diagnostic;
	int error;

	binary = NULL;
	memset(&diagnostic, 0, sizeof(diagnostic));
	error = nvrtx_sm75_compile_ir_sm(&program->ir, test_sm, &binary, &diagnostic);
	if (error != 0) {
		printf("  %s: refused %d: %s (instruction %u)\n", name, error, (diagnostic.reason != NULL) ? diagnostic.reason : "?", diagnostic.instruction);
		return NULL;
	}
	printf("  %s: %u instructions, %u registers\n", name, binary->code_words / 4U, binary->gpr_count);
	return binary;
}

/* Reports nonzero when a program's header holds the wanted words, each word printed where it differs. */
static int
header_is(
	const uint32_t *sph,
	const uint32_t *want)
{
	uint32_t index;
	int same;

	/* Every word of the header. */
	same = 1;
	for (index = 0U; index < NVRTX_SM75_SPH_WORDS; index++) {
		if (sph[index] != want[index]) {
			printf("  header word %u is %08x, want %08x\n", index, sph[index], want[index]);
			same = 0;
		}
	}

	/* Succeeded in comparing them. */
	return same;
}

/*
 * Decodes a push buffer of incrementing methods on subchannel 0 into the
 * last value each method received; reports nonzero for another header or
 * a method past the class's range.
 */
static int
decode_push(
	const uint8_t *push,
	size_t length,
	uint32_t *state,
	uint8_t *seen)
{
	uint32_t header;
	uint32_t method;
	uint32_t count;
	uint32_t value;
	size_t at;

	memset(seen, 0, 0x10000U / 4U);
	for (at = 0U; at < length;) {
		memcpy(&header, push + at, 4U);
		at += 4U;
		if ((header >> 29) != 1U || ((header >> 13) & 7U) != 0U)
			return 1;
		count = (header >> 16) & 0x1fffU;
		method = (header & 0x1fffU) << 2;
		for (; count > 0U; count--) {
			if (at + 4U > length || method >= 0x10000U)
				return 1;
			memcpy(&value, push + at, 4U);
			state[method / 4U] = value;
			seen[method / 4U] = 1U;
			method += 4U;
			at += 4U;
		}
	}
	return 0;
}

/* Reads a field of up to 64 bits from words at a bit position. */
static uint64_t
field(
	const uint32_t *words,
	unsigned low,
	unsigned bits)
{
	uint64_t value;
	unsigned bit;

	value = 0U;
	for (bit = 0U; bit < bits; bit++) {
		if (((words[(low + bit) / 32U] >> ((low + bit) % 32U)) & 1U) != 0U)
			value |= (uint64_t)1U << bit;
	}
	return value;
}

/* Reports nonzero when an instruction is the two 64-bit halves given. */
static int
words_are(
	const struct nvrtx_sm75_word *word,
	uint64_t low,
	uint64_t high)
{
	uint64_t got_low;
	uint64_t got_high;

	got_low = ((uint64_t)word->w[1] << 32) | word->w[0];
	got_high = ((uint64_t)word->w[3] << 32) | word->w[2];

	/* A mismatch is shown. */
	if (got_low != low || got_high != high) {
		printf("  got 0x%016llx 0x%016llx, want 0x%016llx 0x%016llx\n",
		    (unsigned long long)got_low,
		    (unsigned long long)got_high,
		    (unsigned long long)low,
		    (unsigned long long)high);
		return 0;
	}

	/* Succeeded: the same bits. */
	return 1;
}

/* Reads DIR/NAME.spv and compiles it; NULL (with the reason printed) when refused. */
static struct nvrtx_sm75_binary *
compile_file(
	const char *directory,
	const char *name)
{
	struct nvrtx_sm75_binary *binary;
	struct nvrtx_sm75_diagnostic diagnostic;
	char path[512];
	uint32_t *words;
	long bytes;
	FILE *file;
	int error;

	/* The module. */
	snprintf(path, sizeof(path), "%s/%s.spv", directory, name);
	file = fopen(path, "rb");
	if (file == NULL) {
		printf("  %s: cannot open\n", path);
		return NULL;
	}
	fseek(file, 0, SEEK_END);
	bytes = ftell(file);
	fseek(file, 0, SEEK_SET);
	words = malloc((size_t)bytes);
	if (words == NULL || fread(words, 1U, (size_t)bytes, file) != (size_t)bytes) {
		fclose(file);
		free(words);
		return NULL;
	}
	fclose(file);

	/* The compile. */
	binary = NULL;
	error = nvrtx_sm75_compile_stage_sm(words, (size_t)bytes / 4U, DRV_GPU_STAGE_COMPUTE, test_sm, &binary, &diagnostic);
	free(words);
	if (error != 0) {
		printf("  %s: refused %d: %s (instruction %u)\n", name, error, (diagnostic.reason != NULL) ? diagnostic.reason : "?", diagnostic.instruction);
		return NULL;
	}

	/* Succeeded: the program. */
	printf("  %s: %u instructions, %u registers\n", name, binary->code_words / 4U, binary->gpr_count);
	return binary;
}


/* Reports nonzero when two floats agree to a part in 10^5 (or 10^-5 absolute). */
static int
near(
	float a,
	float b)
{
	float difference;
	float size;

	difference = fabsf(a - b);
	size = fabsf(b);
	if (size < 1.0f)
		size = 1.0f;
	return difference <= size * 1e-5f;
}
