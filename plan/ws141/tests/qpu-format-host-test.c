/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Compare independent production words with the unchanged pinned MIT Mesa packer and decoder. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/qpu.h"
#include "broadcom/common/v3d_device_info.h"
#include "broadcom/qpu/qpu_instr.h"

/* Semantic correspondence does not duplicate the upstream field layout or its encoding implementation. */
struct expected_operation {
	enum bcm2711_qpu_operation native;
	enum v3d_qpu_add_op add;
	enum v3d_qpu_mul_op multiply;
};

/* The operation vocabulary remains observable through an independent decoder from the fixed source. */
static const struct expected_operation expected_operations[] = {
    {BCM2711_QPU_IDLE, V3D_QPU_A_NOP, V3D_QPU_M_NOP},
    {BCM2711_QPU_MOVE, V3D_QPU_A_NOP, V3D_QPU_M_MOV},
    {BCM2711_QPU_FLOAT_ADD, V3D_QPU_A_FADD, V3D_QPU_M_NOP},
    {BCM2711_QPU_FLOAT_SUBTRACT, V3D_QPU_A_FSUB, V3D_QPU_M_NOP},
    {BCM2711_QPU_FLOAT_MULTIPLY, V3D_QPU_A_NOP, V3D_QPU_M_FMUL},
    {BCM2711_QPU_FLOAT_MINIMUM, V3D_QPU_A_FMIN, V3D_QPU_M_NOP},
    {BCM2711_QPU_FLOAT_MAXIMUM, V3D_QPU_A_FMAX, V3D_QPU_M_NOP},
    {BCM2711_QPU_FLOAT_COMPARE, V3D_QPU_A_FCMP, V3D_QPU_M_NOP},
    {BCM2711_QPU_INTEGER_ADD, V3D_QPU_A_ADD, V3D_QPU_M_NOP},
    {BCM2711_QPU_INTEGER_SUBTRACT, V3D_QPU_A_SUB, V3D_QPU_M_NOP},
    {BCM2711_QPU_INTEGER_MINIMUM, V3D_QPU_A_MIN, V3D_QPU_M_NOP},
    {BCM2711_QPU_INTEGER_MAXIMUM, V3D_QPU_A_MAX, V3D_QPU_M_NOP},
    {BCM2711_QPU_UNSIGNED_MINIMUM, V3D_QPU_A_UMIN, V3D_QPU_M_NOP},
    {BCM2711_QPU_UNSIGNED_MAXIMUM, V3D_QPU_A_UMAX, V3D_QPU_M_NOP},
    {BCM2711_QPU_SHIFT_LEFT, V3D_QPU_A_SHL, V3D_QPU_M_NOP},
    {BCM2711_QPU_SHIFT_RIGHT, V3D_QPU_A_SHR, V3D_QPU_M_NOP},
    {BCM2711_QPU_SHIFT_SIGNED, V3D_QPU_A_ASR, V3D_QPU_M_NOP},
    {BCM2711_QPU_BITS_AND, V3D_QPU_A_AND, V3D_QPU_M_NOP},
    {BCM2711_QPU_BITS_OR, V3D_QPU_A_OR, V3D_QPU_M_NOP},
    {BCM2711_QPU_BITS_XOR, V3D_QPU_A_XOR, V3D_QPU_M_NOP},
    {BCM2711_QPU_BITS_NOT, V3D_QPU_A_NOT, V3D_QPU_M_NOP},
    {BCM2711_QPU_INTEGER_NEGATE, V3D_QPU_A_NEG, V3D_QPU_M_NOP},
    {BCM2711_QPU_FLOAT_RECIPROCAL, V3D_QPU_A_RECIP, V3D_QPU_M_NOP},
    {BCM2711_QPU_FLOAT_ROUND, V3D_QPU_A_FROUND, V3D_QPU_M_NOP},
    {BCM2711_QPU_FLOAT_TRUNCATE, V3D_QPU_A_FTRUNC, V3D_QPU_M_NOP},
    {BCM2711_QPU_FLOAT_FLOOR, V3D_QPU_A_FFLOOR, V3D_QPU_M_NOP},
    {BCM2711_QPU_FLOAT_CEILING, V3D_QPU_A_FCEIL, V3D_QPU_M_NOP},
    {BCM2711_QPU_FLOAT_TO_SIGNED, V3D_QPU_A_FTOIZ, V3D_QPU_M_NOP},
    {BCM2711_QPU_FLOAT_TO_UNSIGNED, V3D_QPU_A_FTOUZ, V3D_QPU_M_NOP},
    {BCM2711_QPU_SIGNED_TO_FLOAT, V3D_QPU_A_ITOF, V3D_QPU_M_NOP},
    {BCM2711_QPU_UNSIGNED_TO_FLOAT, V3D_QPU_A_UTOF, V3D_QPU_M_NOP},
    {BCM2711_QPU_DERIVATIVE_X, V3D_QPU_A_FDX, V3D_QPU_M_NOP},
    {BCM2711_QPU_DERIVATIVE_Y, V3D_QPU_A_FDY, V3D_QPU_M_NOP},
    {BCM2711_QPU_FLOAT_INVERSE_ROOT, V3D_QPU_A_RSQRT, V3D_QPU_M_NOP},
    {BCM2711_QPU_FLOAT_EXPONENT, V3D_QPU_A_EXP, V3D_QPU_M_NOP},
    {BCM2711_QPU_FLOAT_LOGARITHM, V3D_QPU_A_LOG, V3D_QPU_M_NOP},
    {BCM2711_QPU_FLOAT_SINE, V3D_QPU_A_SIN, V3D_QPU_M_NOP},
    {BCM2711_QPU_VPM_LOAD, V3D_QPU_A_LDVPMV_IN, V3D_QPU_M_NOP},
    {BCM2711_QPU_VPM_STORE, V3D_QPU_A_STVPMV, V3D_QPU_M_NOP},
    {BCM2711_QPU_TEXTURE_WAIT, V3D_QPU_A_TMUWT, V3D_QPU_M_NOP},
    {BCM2711_QPU_VPM_WAIT, V3D_QPU_A_VPMWT, V3D_QPU_M_NOP},
    {BCM2711_QPU_PIXEL_X, V3D_QPU_A_FXCD, V3D_QPU_M_NOP},
    {BCM2711_QPU_PIXEL_Y, V3D_QPU_A_FYCD, V3D_QPU_M_NOP},
    {BCM2711_QPU_INSTANCE_INDEX, V3D_QPU_A_IID, V3D_QPU_M_NOP},
    {BCM2711_QPU_ELEMENT_INDEX, V3D_QPU_A_EIDX, V3D_QPU_M_NOP}};

static void check_word(const struct v3d_device_info *device, struct bcm2711_qpu_instruction *instruction, enum v3d_qpu_add_op add, enum v3d_qpu_mul_op multiply);
static void check_signals(const struct v3d_device_info *device);
static void check_constants(const struct v3d_device_info *device);

/*
 * Verifies semantic operation identity, byte-for-byte roundtrip and meaningful refused combinations.
 */
int
main(
	void)
{
	struct v3d_device_info device;
	struct bcm2711_qpu_instruction instruction;
	uint64_t word;
	uint32_t index;
	int error;

	/* The oracle is the fixed actual 4.2 encoder, independent of production register and packet macros. */
	memset(&device, 0, sizeof(device));
	device.ver = 42;
	device.has_accumulators = true;
	memset(&instruction, 0, sizeof(instruction));
	instruction.destination.number = 17;
	instruction.left.number = 7;
	instruction.right.number = 31;

	/* Every supported semantic operation must decode as that operation and repack to the same complete word. */
	for (index = 0; index < sizeof(expected_operations) / sizeof(expected_operations[0]); index++) {
		instruction.operation = expected_operations[index].native;
		check_word(&device, &instruction, expected_operations[index].add, expected_operations[index].multiply);
	}

	/* A literal native idle word anchors the otherwise independent semantic decoder comparison. */
	instruction.operation = BCM2711_QPU_IDLE;
	error = bcm2711_qpu_encode(&instruction, &word);
	assert(error == 0 && word == UINT64_C(0x3c003186bb800000));

	/* All sixty-four physical source/destination selectors retain their actual identities without truncation. */
	instruction.operation = BCM2711_QPU_MOVE;
	for (index = 0; index < 64; index++) {
		instruction.left.number = index;
		instruction.destination.number = 63 - index;
		check_word(&device, &instruction, V3D_QPU_A_NOP, V3D_QPU_M_MOV);
	}

	/* Signals and small constants carry independent, hardware-defined register and immediate interpretation. */
	check_signals(&device);
	check_constants(&device);

	/* Invalid physical registers and unrepresentable opcodes must preserve the caller's original word. */
	word = UINT64_C(0x1122334455667788);
	instruction.left.number = 64;
	error = bcm2711_qpu_encode(&instruction, &word);
	assert(error == EINVAL && word == UINT64_C(0x1122334455667788));
	instruction.left.number = 0;
	instruction.operation = (enum bcm2711_qpu_operation)UINT32_MAX;
	error = bcm2711_qpu_encode(&instruction, &word);
	assert(error == ENOTSUP && word == UINT64_C(0x1122334455667788));

	/* Succeeded: actual pinned MIT encoder/decoder agrees with independent production words. */
	puts("qpu-format-host-test PASS");
	return 0;
}

/* Compares a complete production word to semantic identities and exact roundtrip in the upstream oracle. */
static void
check_word(
	const struct v3d_device_info *device,
	struct bcm2711_qpu_instruction *instruction,
	enum v3d_qpu_add_op add,
	enum v3d_qpu_mul_op multiply)
{
	struct v3d_qpu_instr decoded;
	uint64_t word;
	uint64_t repacked;
	bool accepted;
	int error;

	/* No production instruction is accepted unless the unchanged actual decoder recognizes its semantic meaning. */
	error = bcm2711_qpu_encode(instruction, &word);
	assert(error == 0);
	memset(&decoded, 0, sizeof(decoded));
	accepted = v3d_qpu_instr_unpack(device, word, &decoded);
	assert(accepted && decoded.type == V3D_QPU_INSTR_TYPE_ALU);
	/* The fixed decoder retains a legacy pre-4.0 name for the exact IID opcode; packing its 4.2 name verifies the word. */
	if (instruction->operation == BCM2711_QPU_INSTANCE_INDEX && decoded.alu.add.op == V3D_QPU_A_VDWWT)
		decoded.alu.add.op = V3D_QPU_A_IID;

	/* A disagreement names the semantic operation before stopping, so native format errors are actionable. */
	if (decoded.alu.add.op != add || decoded.alu.mul.op != multiply) {
		fprintf(stderr, "semantic operation %u: decoded add=%u/mul=%u expected add=%u/mul=%u word=%016llx\n",
			(unsigned)instruction->operation,
			(unsigned)decoded.alu.add.op,
			(unsigned)decoded.alu.mul.op,
			(unsigned)add,
			(unsigned)multiply,
			(unsigned long long)word);
		assert(0);
	}

	/* The unchanged encoder must reconstruct exactly the independent production word. */
	accepted = v3d_qpu_instr_pack(device, &decoded, &repacked);
	assert(accepted && word == repacked);

	/* Bit-preserving moves retain exact physical source and destination identities, including peripheral writes. */
	if (instruction->operation == BCM2711_QPU_MOVE && instruction->left.kind == BCM2711_QPU_REGISTER) {
		assert(decoded.alu.mul.a.mux == V3D_QPU_MUX_A);
		assert(decoded.raddr_a == instruction->left.number);
		assert(decoded.alu.mul.waddr == instruction->destination.number);
		assert(decoded.alu.mul.magic_write == instruction->destination.peripheral);
	}
}

/* Verifies signal destination/flag aliasing and predicates through the unchanged independent decoder. */
static void
check_signals(
	const struct v3d_device_info *device)
{
	struct bcm2711_qpu_instruction instruction;
	struct v3d_qpu_instr decoded;
	uint64_t word;
	uint32_t signal;
	uint32_t predicate;
	bool accepted;
	int error;

	/* Every supported native signal must survive the upstream encoder's exact roundtrip. */
	memset(&instruction, 0, sizeof(instruction));
	instruction.destination.peripheral = 1;
	instruction.destination.number = 6;
	instruction.signal_destination.number = 29;
	for (signal = 0; signal <= BCM2711_QPU_SIGNAL_UNIFORM_ADDRESS_REGISTER; signal++) {
		instruction.signal = (enum bcm2711_qpu_signal)signal;
		check_word(device, &instruction, V3D_QPU_A_NOP, V3D_QPU_M_NOP);
	}

	/* Addressed texture loads really target the complete physical signal destination rather than lane flags. */
	instruction.signal = BCM2711_QPU_SIGNAL_TEXTURE;
	error = bcm2711_qpu_encode(&instruction, &word);
	assert(error == 0);
	accepted = v3d_qpu_instr_unpack(device, word, &decoded);
	assert(accepted && decoded.sig.ldtmu && !decoded.sig_magic && decoded.sig_addr == 29);

	/* Signal and condition sharing is refused without losing the original output word. */
	instruction.operation = BCM2711_QPU_MOVE;
	instruction.predicate = 1;
	word = UINT64_C(0x1122334455667788);
	error = bcm2711_qpu_encode(&instruction, &word);
	assert(error == ENOTSUP && word == UINT64_C(0x1122334455667788));
	instruction.signal = BCM2711_QPU_SIGNAL_NONE;
	for (predicate = 1; predicate <= 4; predicate++) {
		instruction.predicate = predicate;
		check_word(device, &instruction, V3D_QPU_A_NOP, V3D_QPU_M_MOV);
		error = bcm2711_qpu_encode(&instruction, &word);
		assert(error == 0);
		accepted = v3d_qpu_instr_unpack(device, word, &decoded);
		assert(accepted && decoded.flags.mc == (enum v3d_qpu_cond)predicate);
	}

	/* The add ALU's zero/negative/carry pushes remain distinct from multiply ALU pushes. */
	instruction.predicate = 0;
	instruction.operation = BCM2711_QPU_INTEGER_SUBTRACT;
	for (predicate = 1; predicate <= 3; predicate++) {
		instruction.push_flags = predicate;
		check_word(device, &instruction, V3D_QPU_A_SUB, V3D_QPU_M_NOP);
		error = bcm2711_qpu_encode(&instruction, &word);
		assert(error == 0);
		accepted = v3d_qpu_instr_unpack(device, word, &decoded);
		assert(accepted && decoded.flags.apf == (enum v3d_qpu_pf)predicate);
	}
}

/* Compares the native immediate vocabulary with all forty-eight actual upstream values. */
static void
check_constants(
	const struct v3d_device_info *device)
{
	struct bcm2711_qpu_instruction instruction;
	uint64_t word;
	uint32_t bits;
	uint32_t index;
	uint32_t encoded;
	bool accepted;
	int error;

	/* Each native small constant is converted from its actual bits, never from an upstream index assumption. */
	memset(&instruction, 0, sizeof(instruction));
	instruction.operation = BCM2711_QPU_INTEGER_ADD;
	instruction.left.kind = BCM2711_QPU_CONSTANT;
	instruction.right.kind = BCM2711_QPU_REGISTER;
	instruction.right.number = 27;
	for (index = 0; index < 48; index++) {
		accepted = v3d_qpu_small_imm_unpack(device, index, &bits);
		assert(accepted);
		error = bcm2711_qpu_small_constant(bits, &encoded);
		assert(error == 0 && encoded == index);
		instruction.left.number = bits;
		check_word(device, &instruction, V3D_QPU_A_ADD, V3D_QPU_M_NOP);
	}

	/* Nonrepresentable constants are refused exactly, without approximating or truncating their bits. */
	encoded = 99;
	error = bcm2711_qpu_small_constant(0x3f800001U, &encoded);
	assert(error == ENOTSUP && encoded == 99);
	instruction.left.number = 1;
	instruction.right.kind = BCM2711_QPU_CONSTANT;
	instruction.right.number = 2;
	word = UINT64_C(0x1122334455667788);
	error = bcm2711_qpu_encode(&instruction, &word);
	assert(error == ENOTSUP && word == UINT64_C(0x1122334455667788));
}
