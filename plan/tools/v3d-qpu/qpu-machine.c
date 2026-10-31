/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host model of one V3D 4.2 QPU lane running a native program (WS141 p013).
 *
 * See qpu-machine.h.  The timing and transaction rules are the hardware facts
 * recorded in WS141 p013's design section; the decoder is the unmodified MIT
 * Mesa one, independent of the production encoder.
 */
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "broadcom/qpu/qpu_instr.h"
#include "plan/tools/v3d-qpu/qpu-machine.h"

/* Register-file entries keep their numbers; accumulators and the flags follow them. */
#define MACHINE_ACCUMULATOR 64U
#define MACHINE_FLAGS 70U
#define MACHINE_RESOURCES 71U

/* No write is in flight. */
#define MACHINE_NONE 0xffffffffU

/* One lane's architectural state and the writes still on their way. */
struct machine {
	const struct v3d_device_info *device;
	const struct qpu_machine_setup *setup;
	struct qpu_machine_result *result;
	uint32_t value[MACHINE_RESOURCES];
	/* A resource made meaningless by a thread switch; reading it is a failure. */
	bool lost[MACHINE_RESOURCES];
	/* A pending write: the value, and the word at whose end it lands. */
	uint32_t pending_value[MACHINE_RESOURCES];
	uint32_t pending_land[MACHINE_RESOURCES];
	/* The lane flags A and B; the flags resource holds A, B follows it here. */
	uint32_t flag_b;
	uint32_t pending_flag_b;
	uint32_t uniform_read;
	uint32_t varying_read;
	uint32_t attribute_read;
	/* TMU: the T coordinate, configurations since the last lookup, the lookup's set and binding. */
	uint32_t texture_t;
	uint32_t configurations;
	uint32_t texture_set;
	uint32_t texture_binding;
	uint32_t texels[4];
	uint32_t texel_count;
	uint32_t texel_read;
	/* The lookup launched and not yet waited for by a switch. */
	bool lookup_waiting;
	/* Thread switches: the last one, whether the last-switch mark was seen, and the thread end. */
	uint32_t last_switch;
	uint32_t switch_effect;
	bool last_marked;
	uint32_t last_marked_switch;
	bool scoreboard;
	uint32_t thread_end;
	uint32_t last_special;
	uint32_t word;
};

static void fail(const struct machine *machine, const char *reason);
static void decode_word(struct machine *machine, uint32_t index, struct v3d_qpu_instr *instruction);
static void run_alus(struct machine *machine, const struct v3d_qpu_instr *instruction);
static void run_configuration(struct machine *machine, const struct v3d_qpu_instr *instruction);
static void run_signals(struct machine *machine, const struct v3d_qpu_instr *instruction);
static void run_switch(struct machine *machine, const struct v3d_qpu_instr *instruction);
static uint32_t float_bits(float number);
static float bits_float(uint32_t bits);
static uint32_t half_bits(float number);
static uint32_t truth(bool condition);
static void land(struct machine *machine);
static void switch_effect(struct machine *machine);
static uint32_t read_resource(struct machine *machine, uint32_t resource);
static uint32_t read_mux(struct machine *machine, const struct v3d_qpu_instr *instruction, enum v3d_qpu_mux mux);
static void write_resource(struct machine *machine, uint32_t resource, uint32_t bits, uint32_t delay);
static bool condition_holds(struct machine *machine, enum v3d_qpu_cond condition);
static uint32_t add_result(struct machine *machine, const struct v3d_qpu_instr *instruction, uint32_t left, uint32_t right, bool *flags_set, uint32_t *flag);
static uint32_t mul_result(struct machine *machine, const struct v3d_qpu_instr *instruction, uint32_t left, uint32_t right);
static void write_destination(struct machine *machine, uint32_t address, bool magic, uint32_t bits, uint32_t delay);
static void check_word_rules(struct machine *machine, const struct v3d_qpu_instr *instruction);
static uint32_t next_uniform(struct machine *machine);
static int32_t float_to_signed(float number);
static uint32_t float_to_unsigned(float number);

/*
 * Runs one lane of a native program.
 */
void
qpu_machine_run(
	const struct v3d_device_info *device,
	const struct qpu_machine_setup *setup,
	struct qpu_machine_result *result)
{
	const struct bcm2711_shader_binary *binary;
	struct machine machine;
	struct v3d_qpu_instr instruction;
	uint32_t index;
	uint32_t seed;

	/* The lane starts with the payload and garbage elsewhere, nothing in flight and nothing lost. */
	binary = setup->binary;
	memset(&machine, 0, sizeof(machine));
	memset(result, 0, sizeof(*result));
	machine.device = device;
	machine.setup = setup;
	machine.result = result;
	seed = setup->garbage_seed * 2654435761U + 12345U;
	for (index = 0; index < MACHINE_RESOURCES; index++) {
		seed = seed * 1103515245U + 12345U;
		machine.value[index] = seed;
		machine.pending_land[index] = MACHINE_NONE;
	}

	/* The payload registers and cleared flags; no switch, thread end or special function yet. */
	machine.value[0] = setup->payload[0];
	machine.value[1] = setup->payload[1];
	machine.value[2] = setup->payload[2];
	machine.value[MACHINE_FLAGS] = 0;
	machine.last_switch = MACHINE_NONE;
	machine.switch_effect = MACHINE_NONE;
	machine.thread_end = MACHINE_NONE;
	machine.last_special = MACHINE_NONE;

	/* Runs every word; the program must end exactly with the thread end's second delay word. */
	for (index = 0; index < binary->code_count; index++) {
		machine.word = index;

		/* Writes that landed at the end of the previous word become visible; a switch takes effect between words. */
		land(&machine);
		if (machine.switch_effect == index)
			switch_effect(&machine);

		/* The word runs: its rules, a texture configuration, both ALUs, its loads and a switch. */
		decode_word(&machine, index, &instruction);
		check_word_rules(&machine, &instruction);
		run_configuration(&machine, &instruction);
		run_alus(&machine, &instruction);
		run_signals(&machine, &instruction);
		run_switch(&machine, &instruction);
	}

	/* The program ends with the thread end's second delay word, its transactions all complete. */
	machine.word = binary->code_count;
	land(&machine);
	if (machine.thread_end == MACHINE_NONE || machine.thread_end + 3 != binary->code_count)
		fail(&machine, "program does not end two words after its thread end");
	if (machine.uniform_read != binary->uniform_count)
		fail(&machine, "uniform stream not consumed exactly");
	if (binary->stage == BCM2711_SHADER_FRAGMENT && machine.varying_read != binary->input_count)
		fail(&machine, "varyings not consumed exactly");
	if (machine.configurations != 0 || machine.texel_read != machine.texel_count || machine.lookup_waiting)
		fail(&machine, "texture transaction left incomplete");
}

/*
 * Decodes and checks every word of a program once, for runs that share it.
 */
void
qpu_machine_decode(
	const struct v3d_device_info *device,
	const struct bcm2711_shader_binary *binary,
	struct v3d_qpu_instr *decoded)
{
	uint64_t repacked;
	uint32_t index;
	bool accepted;

	/* Each word decodes as an ALU instruction and rebuilds exactly. */
	for (index = 0; index < binary->code_count; index++) {
		accepted = v3d_qpu_instr_unpack(device, binary->code[index], &decoded[index]);
		if (!accepted || decoded[index].type != V3D_QPU_INSTR_TYPE_ALU) {
			fprintf(stderr, "qpu machine: word %u does not decode as an ALU instruction\n", index);
			abort();
		}

		/* The oracle rebuilds the same word. */
		accepted = v3d_qpu_instr_pack(device, &decoded[index], &repacked);
		if (!accepted || repacked != binary->code[index]) {
			fprintf(stderr, "qpu machine: word %u does not repack to itself\n", index);
			abort();
		}
	}
}

/* Reads one word, from the run's decoded copy or with the independent decoder. */
static void
decode_word(
	struct machine *machine,
	uint32_t index,
	struct v3d_qpu_instr *instruction)
{
	const struct bcm2711_shader_binary *binary;
	uint64_t repacked;
	bool accepted;

	/* A word decoded once already needs no second check. */
	binary = machine->setup->binary;
	if (machine->setup->decoded != NULL) {
		*instruction = machine->setup->decoded[index];
	} else {
		accepted = v3d_qpu_instr_unpack(machine->device, binary->code[index], instruction);
		if (!accepted || instruction->type != V3D_QPU_INSTR_TYPE_ALU)
			fail(machine, "word does not decode as an ALU instruction");
		accepted = v3d_qpu_instr_pack(machine->device, instruction, &repacked);
		if (!accepted || repacked != binary->code[index])
			fail(machine, "word does not repack to itself");
	}

	/* Flag updates and output packing are outside the compiler's vocabulary. */
	if (instruction->flags.auf != V3D_QPU_UF_NONE || instruction->flags.muf != V3D_QPU_UF_NONE)
		fail(machine, "flag update operations are not modeled");
	if (instruction->alu.add.output_pack != V3D_QPU_PACK_NONE || instruction->alu.mul.output_pack != V3D_QPU_PACK_NONE)
		fail(machine, "output packing is not modeled");
}

/* Runs both ALUs of one word: sources read at the start, predicates from the old flags, results landing later. */
static void
run_alus(
	struct machine *machine,
	const struct v3d_qpu_instr *instruction)
{
	uint32_t add_left;
	uint32_t add_right;
	uint32_t mul_left;
	uint32_t mul_right;
	uint32_t add_bits;
	uint32_t mul_bits;
	uint32_t flag;
	uint32_t delay;
	int add_sources;
	int mul_sources;
	bool add_write;
	bool mul_write;
	bool holds;
	bool special;
	bool flags_set;

	/* Both ALUs read their sources when the word starts. */
	add_left = 0;
	add_right = 0;
	mul_left = 0;
	mul_right = 0;
	add_sources = 0;
	mul_sources = 0;
	if (instruction->alu.add.op != V3D_QPU_A_NOP)
		add_sources = v3d_qpu_add_op_num_src(instruction->alu.add.op);
	if (instruction->alu.mul.op != V3D_QPU_M_NOP)
		mul_sources = v3d_qpu_mul_op_num_src(instruction->alu.mul.op);
	if (add_sources > 0)
		add_left = read_mux(machine, instruction, instruction->alu.add.a.mux);
	if (add_sources > 1)
		add_right = read_mux(machine, instruction, instruction->alu.add.b.mux);
	if (mul_sources > 0)
		mul_left = read_mux(machine, instruction, instruction->alu.mul.a.mux);
	if (mul_sources > 1)
		mul_right = read_mux(machine, instruction, instruction->alu.mul.b.mux);

	/* Predicates read the flags as they were when the word started. */
	add_write = false;
	mul_write = false;
	if (instruction->alu.add.op != V3D_QPU_A_NOP)
		add_write = v3d_qpu_add_op_has_dst(instruction->alu.add.op);
	if (instruction->alu.mul.op != V3D_QPU_M_NOP)
		mul_write = v3d_qpu_mul_op_has_dst(instruction->alu.mul.op);
	if (instruction->flags.ac != V3D_QPU_COND_NONE) {
		holds = condition_holds(machine, instruction->flags.ac);
		if (!holds)
			add_write = false;
	}

	/* The multiply ALU's predicate likewise. */
	if (instruction->flags.mc != V3D_QPU_COND_NONE) {
		holds = condition_holds(machine, instruction->flags.mc);
		if (!holds)
			mul_write = false;
	}

	/* Each ALU computes; a special function's result lands two words later. */
	flags_set = false;
	flag = 0;
	add_bits = 0;
	mul_bits = 0;
	if (instruction->alu.add.op != V3D_QPU_A_NOP)
		add_bits = add_result(machine, instruction, add_left, add_right, &flags_set, &flag);
	if (instruction->alu.mul.op != V3D_QPU_M_NOP)
		mul_bits = mul_result(machine, instruction, mul_left, mul_right);
	special = v3d_qpu_instr_is_sfu(instruction);
	delay = 0;
	if (special)
		delay = 2;
	if (add_write)
		write_destination(machine, instruction->alu.add.waddr, instruction->alu.add.magic_write, add_bits, delay);

	/* A SETMSF of zero discards the lane; the tile writes it masks must come in later words. */
	if (add_write && instruction->alu.add.op == V3D_QPU_A_SETMSF) {
		if (machine->result->tile_writes != 0 || v3d_qpu_uses_tlb(instruction))
			fail(machine, "multisample flags set at or after a tile write");
		if (machine->setup->binary->stage != BCM2711_SHADER_FRAGMENT)
			fail(machine, "multisample flags set outside a fragment program");
		machine->result->discarded = add_bits == 0 ? 1U : 0U;
	}
	if (mul_write)
		write_destination(machine, instruction->alu.mul.waddr, instruction->alu.mul.magic_write, mul_bits, 0);

	/* A multiply ALU flag push follows its own result. */
	if (instruction->flags.mpf != V3D_QPU_PF_NONE) {
		if (instruction->flags.mpf != V3D_QPU_PF_PUSHZ)
			fail(machine, "multiply flag push other than zero is not modeled");
		flags_set = true;
		flag = truth(mul_bits == 0);
	}

	/* A push moves flag A to B and sets the new A when the word ends. */
	if (flags_set) {
		machine->lost[MACHINE_FLAGS] = false;
		machine->pending_flag_b = machine->value[MACHINE_FLAGS];
		write_resource(machine, MACHINE_FLAGS, flag, 0);
	}
}

/* Runs one word's texture configuration, before its ALUs, so it applies to a lookup the same word launches. */
static void
run_configuration(
	struct machine *machine,
	const struct v3d_qpu_instr *instruction)
{
	uint32_t word;

	/* The texture configuration's two uniforms name the texture and its sampler. */
	if (instruction->sig.wrtmuc) {
		word = next_uniform(machine);
		if (machine->configurations == 0) {
			if ((word >> 28) != 1U)
				fail(machine, "first texture configuration is not a texture");
			machine->texture_set = (word >> 8) & 0xffU;
			machine->texture_binding = word & 0xffU;
		} else if (machine->configurations == 1) {
			if ((word >> 28) != 2U ||
			    ((word >> 8) & 0xffU) != machine->texture_set ||
			    (word & 0xffU) != machine->texture_binding)
				fail(machine, "second texture configuration does not match its texture");
		} else {
			fail(machine, "too many texture configurations");
		}

		/* One more configuration belongs to the next lookup. */
		machine->configurations++;
	}


}

/* Runs one word's uniform, varying and texture loads, which write when the word ends. */
static void
run_signals(
	struct machine *machine,
	const struct v3d_qpu_instr *instruction)
{
	const struct qpu_machine_setup *setup;
	uint32_t word;

	/* The setup supplies the varyings. */
	setup = machine->setup;

	/* A uniform load to a register. */
	if (instruction->sig.ldunifrf) {
		if (instruction->sig_magic)
			fail(machine, "uniform load to a magic address is not modeled");
		word = next_uniform(machine);
		write_resource(machine, instruction->sig_addr, word, 0);
	}

	/*
	 * A varying's interpolated term carries the whole value and its
	 * constant term, in accumulator five a word later, is minus zero, so the
	 * sum keeps every value exactly, signed zeros included; a flat varying
	 * is all constant term.
	 */
	if (instruction->sig.ldvary) {
		if (instruction->sig_magic || setup->varyings == NULL || setup->key == NULL)
			fail(machine, "varying load is not modeled here");
		if (machine->varying_read >= setup->binary->input_count)
			fail(machine, "more varying loads than inputs");
		word = setup->varyings[machine->varying_read];
		if (setup->key->varyings[machine->varying_read].flat != 0) {
			write_resource(machine, instruction->sig_addr, 0x7fc00000U, 0);
			write_resource(machine, MACHINE_ACCUMULATOR + 5, word, 1);
		} else if (setup->key->varyings[machine->varying_read].noperspective != 0) {
			write_resource(machine, instruction->sig_addr, word, 0);
			write_resource(machine, MACHINE_ACCUMULATOR + 5, 0x80000000U, 1);
		} else {
			word = float_bits(bits_float(word) / bits_float(setup->payload[0]));
			write_resource(machine, instruction->sig_addr, word, 0);
			write_resource(machine, MACHINE_ACCUMULATOR + 5, 0x80000000U, 1);
		}

		/* The FIFO advances. */
		machine->varying_read++;
	}


	/* A texture result, available only after a switch waited for its lookup. */
	if (instruction->sig.ldtmu) {
		if (instruction->sig_magic)
			fail(machine, "texture load to a magic address is not modeled");
		if (machine->lookup_waiting)
			fail(machine, "texture result read before a thread switch waited for it");
		if (machine->texel_read >= machine->texel_count)
			fail(machine, "texture result read beyond the lookup");
		write_resource(machine, instruction->sig_addr, machine->texels[machine->texel_read], 0);
		machine->texel_read++;
	}
}

/* Notes one word's switch: it takes effect after its two delay words, and two consecutive switches mark the last one. */
static void
run_switch(
	struct machine *machine,
	const struct v3d_qpu_instr *instruction)
{
	uint32_t index;

	/* A word without a switch changes nothing here. */
	if (!instruction->sig.thrsw)
		return;

	/* A switch right after an unmarked one marks it as the last; it is not a switch of its own. */
	index = machine->word;
	if (machine->last_switch != MACHINE_NONE && machine->last_switch + 1 == index && !machine->last_marked) {
		machine->last_marked = true;
		machine->last_marked_switch = machine->last_switch;
		return;
	}

	/* Any other switch is outside the previous one's delay words; after the last one, it ends the thread. */
	if (machine->last_switch != MACHINE_NONE && index < machine->last_switch + 3)
		fail(machine, "thread switch inside another switch's delay words");
	if (machine->last_marked || machine->setup->binary->starts_final != 0) {
		if (machine->thread_end != MACHINE_NONE)
			fail(machine, "second thread end");
		machine->thread_end = index;
	}

	/* The switch takes effect after its two delay words. */
	machine->last_switch = index;
	machine->switch_effect = index + 3;
}

/* Reports a broken rule with the word it happened at and stops. */
static void
fail(
	const struct machine *machine,
	const char *reason)
{
	/* The word index names where the program broke the rule. */
	fprintf(stderr, "qpu machine: word %u: %s\n", machine->word, reason);
	if (machine->word < machine->setup->binary->code_count)
		fprintf(stderr, "qpu machine: word bits %016llx\n", (unsigned long long)machine->setup->binary->code[machine->word]);
	abort();
}

/* Converts a float to its bits. */
static uint32_t
float_bits(
	float number)
{
	uint32_t bits;

	/* A copy avoids aliasing assumptions. */
	memcpy(&bits, &number, sizeof(bits));
	return bits;
}

/* Converts bits to a float. */
static float
bits_float(
	uint32_t bits)
{
	float number;

	/* A copy avoids aliasing assumptions. */
	memcpy(&number, &bits, sizeof(number));
	return number;
}

/* Rounds a float to the nearest half float, as the pack does. */
static uint32_t
half_bits(
	float number)
{
	_Float16 half;
	uint16_t bits;

	/* The host's conversion rounds to nearest even. */
	half = (_Float16)number;
	memcpy(&bits, &half, sizeof(bits));
	return bits;
}

/* Builds an all-ones or zero lane mask. */
static uint32_t
truth(
	bool condition)
{
	/* True is all ones, as the flags and Boolean values hold it. */
	if (condition)
		return 0xffffffffU;
	return 0;
}

/* Makes the writes that landed at the end of the previous word visible. */
static void
land(
	struct machine *machine)
{
	uint32_t resource;

	/* A write landing at the end of word w is visible from word w + 1. */
	for (resource = 0; resource < MACHINE_RESOURCES; resource++) {
		if (machine->pending_land[resource] == MACHINE_NONE)
			continue;
		if (machine->pending_land[resource] >= machine->word)
			continue;
		machine->value[resource] = machine->pending_value[resource];
		machine->lost[resource] = false;
		machine->pending_land[resource] = MACHINE_NONE;
		if (resource == MACHINE_FLAGS)
			machine->flag_b = machine->pending_flag_b;
	}
}

/* Applies a thread switch: accumulators and flags are lost, a waited lookup is ready, the last switch takes the scoreboard. */
static void
switch_effect(
	struct machine *machine)
{
	uint32_t resource;

	/* A write still in flight would land in the other thread. */
	for (resource = 0; resource < MACHINE_RESOURCES; resource++) {
		if (machine->pending_land[resource] != MACHINE_NONE)
			fail(machine, "write in flight across a thread switch");
	}

	/* A switch that is neither the last one nor the thread end waits for an outstanding lookup. */
	if (!machine->lookup_waiting && !(machine->last_marked && machine->last_switch == machine->last_marked_switch) && machine->thread_end == MACHINE_NONE)
		fail(machine, "thread switch with no outstanding lookup");

	/* Another thread runs here; it leaves the accumulators and flags meaningless. */
	for (resource = MACHINE_ACCUMULATOR; resource < MACHINE_RESOURCES; resource++)
		machine->lost[resource] = true;
	machine->lookup_waiting = false;

	/* The marked last switch holds the tile scoreboard from here on. */
	if (machine->last_marked && machine->last_switch == machine->last_marked_switch)
		machine->scoreboard = true;
}

/* Reads one register, accumulator or the flags, refusing a value in flight or lost. */
static uint32_t
read_resource(
	struct machine *machine,
	uint32_t resource)
{
	/* A write still on its way means the read would see an older value than the program meant. */
	if (machine->pending_land[resource] != MACHINE_NONE)
		fail(machine, "read of a resource whose write is in flight");
	if (machine->lost[resource])
		fail(machine, "read of an accumulator or the flags across a thread switch");

	/* Reports the visible value. */
	return machine->value[resource];
}

/* Reads one source mux of the decoded word. */
static uint32_t
read_mux(
	struct machine *machine,
	const struct v3d_qpu_instr *instruction,
	enum v3d_qpu_mux mux)
{
	uint32_t bits;
	bool accepted;

	/* Accumulators need no read port. */
	if (mux < V3D_QPU_MUX_A) {
		if (mux == V3D_QPU_MUX_R4)
			fail(machine, "accumulator four is not used by the compiler");
		return read_resource(machine, MACHINE_ACCUMULATOR + (uint32_t)mux);
	}

	/* Port A always names a register; port B a register or the small constant. */
	if (mux == V3D_QPU_MUX_A)
		return read_resource(machine, instruction->raddr_a);
	if (!instruction->sig.small_imm_b)
		return read_resource(machine, instruction->raddr_b);
	accepted = v3d_qpu_small_imm_unpack(machine->device, instruction->raddr_b, &bits);
	if (!accepted)
		fail(machine, "small constant does not decode");

	/* Reports the decoded constant's bits. */
	return bits;
}

/* Starts one write that lands at the end of the word plus a delay. */
static void
write_resource(
	struct machine *machine,
	uint32_t resource,
	uint32_t bits,
	uint32_t delay)
{
	/* Two writes of one resource in flight would race. */
	if (machine->pending_land[resource] != MACHINE_NONE)
		fail(machine, "write while another write of the resource is in flight");
	machine->pending_value[resource] = bits;
	machine->pending_land[resource] = machine->word + delay;
}

/* Evaluates one predicate against the flags as they were when the word started. */
static bool
condition_holds(
	struct machine *machine,
	enum v3d_qpu_cond condition)
{
	uint32_t flag_a;

	/* Reading A checks it is visible; B moves with A. */
	flag_a = read_resource(machine, MACHINE_FLAGS);
	switch (condition) {
	case V3D_QPU_COND_IFA:
		return flag_a != 0;
	case V3D_QPU_COND_IFNA:
		return flag_a == 0;
	case V3D_QPU_COND_IFB:
		return machine->flag_b != 0;
	case V3D_QPU_COND_IFNB:
		return machine->flag_b == 0;
	default:
		break;
	}

	/* An unpredicated write always happens. */
	return true;
}

/*
 * The minimum and maximum of two floats with -0 below +0, whichever order
 * they come in: SPIR-V's FMin and FMax may return either zero for two
 * zeros, and the native compiler may exchange the operands, so the reference
 * and the lane model both take the one order-independent answer.
 */
static float
ordered_minimum(
	float a,
	float b)
{
	/* Two zeros give -0 when either is -0. */
	if (a == 0.0f && b == 0.0f)
		return signbit(a) || signbit(b) ? -0.0f : 0.0f;
	return fminf(a, b);
}

static float
ordered_maximum(
	float a,
	float b)
{
	/* Two zeros give +0 when either is +0. */
	if (a == 0.0f && b == 0.0f)
		return signbit(a) && signbit(b) ? -0.0f : 0.0f;
	return fmaxf(a, b);
}

/* Computes one add ALU operation and, when it pushes flags, the new flag A. */
static uint32_t
add_result(
	struct machine *machine,
	const struct v3d_qpu_instr *instruction,
	uint32_t left,
	uint32_t right,
	bool *flags_set,
	uint32_t *flag)
{
	float a;
	float b;
	uint32_t bits;

	/* Inputs are whole 32-bit values; no unpacking is modeled. */
	if (instruction->alu.add.a.unpack != V3D_QPU_UNPACK_NONE || instruction->alu.add.b.unpack != V3D_QPU_UNPACK_NONE)
		fail(machine, "add input unpacking is not modeled");
	a = bits_float(left);
	b = bits_float(right);
	bits = 0;

	/* The operation identities come from the independent decoder. */
	switch (instruction->alu.add.op) {
	case V3D_QPU_A_FADD:
	case V3D_QPU_A_FADDNF:
		bits = float_bits(a + b);
		break;
	case V3D_QPU_A_FSUB:
		bits = float_bits(a - b);
		break;
	case V3D_QPU_A_FMIN:
		bits = float_bits(ordered_minimum(a, b));
		break;
	case V3D_QPU_A_FMAX:
		bits = float_bits(ordered_maximum(a, b));
		break;
	case V3D_QPU_A_ADD:
		bits = left + right;
		break;
	case V3D_QPU_A_SUB:
		bits = left - right;
		break;
	case V3D_QPU_A_MIN:
		bits = right;
		if ((int32_t)left < (int32_t)right)
			bits = left;
		break;
	case V3D_QPU_A_MAX:
		bits = right;
		if ((int32_t)left > (int32_t)right)
			bits = left;
		break;
	case V3D_QPU_A_UMIN:
		bits = right;
		if (left < right)
			bits = left;
		break;
	case V3D_QPU_A_UMAX:
		bits = right;
		if (left > right)
			bits = left;
		break;
	case V3D_QPU_A_SHL:
		bits = left << (right & 31U);
		break;
	case V3D_QPU_A_SHR:
		bits = left >> (right & 31U);
		break;
	case V3D_QPU_A_ASR:
		bits = (uint32_t)((int32_t)left >> (right & 31U));
		break;
	case V3D_QPU_A_AND:
		bits = left & right;
		break;
	case V3D_QPU_A_OR:
		bits = left | right;
		break;
	case V3D_QPU_A_XOR:
		bits = left ^ right;
		break;
	case V3D_QPU_A_NOT:
		bits = ~left;
		break;
	case V3D_QPU_A_NEG:
		bits = 0U - left;
		break;
	case V3D_QPU_A_RECIP:
		bits = float_bits(1.0f / a);
		break;
	case V3D_QPU_A_RSQRT:
		bits = float_bits(1.0f / sqrtf(a));
		break;
	case V3D_QPU_A_EXP:
		bits = float_bits(exp2f(a));
		break;
	case V3D_QPU_A_LOG:
		bits = float_bits(log2f(a));
		break;
	case V3D_QPU_A_FFLOOR:
		bits = float_bits(floorf(a));
		break;
	case V3D_QPU_A_FCEIL:
		bits = float_bits(ceilf(a));
		break;
	case V3D_QPU_A_FTRUNC:
		bits = float_bits(truncf(a));
		break;
	case V3D_QPU_A_FROUND:
		bits = float_bits(rintf(a));
		break;
	case V3D_QPU_A_FTOIZ:
		bits = (uint32_t)float_to_signed(a);
		break;
	case V3D_QPU_A_FTOUZ:
		bits = float_to_unsigned(a);
		break;
	case V3D_QPU_A_ITOF:
		bits = float_bits((float)(int32_t)left);
		break;
	case V3D_QPU_A_UTOF:
		bits = float_bits((float)left);
		break;
	case V3D_QPU_A_VFPACK:
		bits = half_bits(a) | (half_bits(b) << 16);
		break;
	case V3D_QPU_A_FCMP:
		/* A float comparison pushes its own ordered flag. */
		if (instruction->flags.apf == V3D_QPU_PF_PUSHZ)
			*flag = truth(a == b);
		else if (instruction->flags.apf == V3D_QPU_PF_PUSHN)
			*flag = truth(a < b);
		else if (instruction->flags.apf == V3D_QPU_PF_PUSHC)
			*flag = truth(a <= b);
		else
			fail(machine, "comparison without a flag push");
		*flags_set = true;
		return 0;
	case V3D_QPU_A_LDVPMV_IN:
		if (machine->setup->attributes == NULL || left >= 32)
			fail(machine, "vertex attribute read is not modeled here");
		machine->attribute_read++;
		bits = machine->setup->attributes[left];
		break;
	case V3D_QPU_A_STVPMV:
		if (left >= 68)
			fail(machine, "vertex output index out of range");
		machine->result->vpm[left] = right;
		break;
	case V3D_QPU_A_VPMWT:
		break;
	case V3D_QPU_A_SETMSF:
		/* The new multisample flags; run_alus applies them when the predicate holds. */
		bits = left;
		break;
	default:
		fprintf(stderr, "unmodeled add operation %s\n", v3d_qpu_add_op_name(instruction->alu.add.op));
		fail(machine, "unmodeled add operation");
	}

	/* An integer zero push takes the computed bits. */
	if (instruction->flags.apf != V3D_QPU_PF_NONE) {
		if (instruction->flags.apf != V3D_QPU_PF_PUSHZ)
			fail(machine, "add flag push other than zero is not modeled");
		*flags_set = true;
		*flag = truth(bits == 0);
	}

	/* Reports the add ALU's result. */
	return bits;
}

/* Computes one multiply ALU operation. */
static uint32_t
mul_result(
	struct machine *machine,
	const struct v3d_qpu_instr *instruction,
	uint32_t left,
	uint32_t right)
{
	_Float16 value;
	uint16_t half_bits16;
	uint32_t half;

	/* Multiplication, bit moves and half unpacking are the multiply operations used. */
	switch (instruction->alu.mul.op) {
	case V3D_QPU_M_FMUL:
		if (instruction->alu.mul.a.unpack != V3D_QPU_UNPACK_NONE || instruction->alu.mul.b.unpack != V3D_QPU_UNPACK_NONE)
			fail(machine, "multiply input unpacking is not modeled");
		return float_bits(bits_float(left) * bits_float(right));
	case V3D_QPU_M_MOV:
		return left;
	case V3D_QPU_M_FMOV:
		/* Widens one half float, exactly. */
		half = left & 0xffffU;
		if (instruction->alu.mul.a.unpack == V3D_QPU_UNPACK_H)
			half = left >> 16;
		else if (instruction->alu.mul.a.unpack != V3D_QPU_UNPACK_L)
			fail(machine, "float move without a half unpack is not modeled");
		half_bits16 = (uint16_t)half;
		memcpy(&value, &half_bits16, sizeof(value));
		return float_bits((float)value);
	default:
		break;
	}

	/* Any other multiply operation is outside the compiler's vocabulary. */
	fprintf(stderr, "unmodeled multiply operation %s\n", v3d_qpu_mul_op_name(instruction->alu.mul.op));
	fail(machine, "unmodeled multiply operation");
	return 0;
}

/* Writes one ALU result to a register, an accumulator or a peripheral. */
static void
write_destination(
	struct machine *machine,
	uint32_t address,
	bool magic,
	uint32_t bits,
	uint32_t delay)
{
	uint32_t component;

	/* A register write lands when the word ends, a special function's two words later. */
	if (!magic) {
		write_resource(machine, address, bits, delay);
		return;
	}

	/* Peripheral writes: the null address, accumulators, the TMU and the tile buffer. */
	switch (address) {
	case V3D_QPU_WADDR_NOP:
		return;
	case V3D_QPU_WADDR_R0:
	case V3D_QPU_WADDR_R1:
	case V3D_QPU_WADDR_R2:
	case V3D_QPU_WADDR_R3:
		if (delay != 0)
			fail(machine, "special function result to an accumulator is not modeled");
		write_resource(machine, MACHINE_ACCUMULATOR + address, bits, 0);
		return;
	case V3D_QPU_WADDR_TMUT:
		machine->texture_t = bits;
		return;
	case V3D_QPU_WADDR_TMUS:
		/* The S write launches the lookup with the two configurations before it. */
		if (machine->configurations != 2 || machine->texel_read != machine->texel_count)
			fail(machine, "texture lookup launched without its configuration or with results unread");
		machine->setup->sample(machine->setup->context, machine->texture_set, machine->texture_binding, bits, machine->texture_t, machine->texels);
		machine->texel_count = 4;
		machine->texel_read = 0;
		machine->configurations = 0;
		machine->lookup_waiting = true;
		machine->result->lookups++;
		return;
	case V3D_QPU_WADDR_TLB:
		/* The scoreboard must be held; the default format takes two packed words. */
		if (!machine->scoreboard)
			fail(machine, "tile write before the last switch holds the scoreboard");
		if (machine->result->tile_writes >= 2)
			fail(machine, "more than two tile writes");
		machine->result->tile[machine->result->tile_writes] = bits;
		machine->result->tile_writes++;
		return;
	default:
		break;
	}

	/* Any other peripheral is outside the compiler's vocabulary. */
	component = address;
	fprintf(stderr, "unmodeled peripheral address %u\n", component);
	fail(machine, "unmodeled peripheral write");
}

/* Checks the per-word unit, switch-window, special-function and thread end rules. */
static void
check_word_rules(
	struct machine *machine,
	const struct v3d_qpu_instr *instruction)
{
	uint32_t units;
	uint32_t uniforms;
	bool special;
	bool in_switch_window;
	bool in_end_window;
	bool add_register;
	bool mul_register;
	bool tmu_write;
	bool coordinate_write;
	bool tile;
	bool vpm;
	bool signal_register;
	bool port_a;
	bool port_b;

	/* One of the TMU, special function, tile, VPM and synchronization units per word; a texture configuration may join a TMU coordinate write. */
	special = v3d_qpu_instr_is_sfu(instruction);
	tmu_write = v3d_qpu_writes_tmu(machine->device, instruction);
	coordinate_write = v3d_qpu_writes_tmu_not_tmuc(machine->device, instruction);
	tile = v3d_qpu_uses_tlb(instruction);
	vpm = v3d_qpu_uses_vpm(instruction);
	units = 0;
	if (tmu_write)
		units++;
	if (instruction->sig.ldtmu)
		units++;
	if (instruction->sig.wrtmuc && !coordinate_write)
		units++;
	if (special)
		units++;
	if (tile)
		units++;
	if (vpm)
		units++;
	if (units > 1)
		fail(machine, "more than one peripheral unit in one word");

	/* One uniform per word. */
	uniforms = 0;
	if (instruction->sig.ldunifrf || instruction->sig.ldunif)
		uniforms++;
	if (instruction->sig.wrtmuc)
		uniforms++;
	if (uniforms > 1)
		fail(machine, "more than one uniform in one word");
	if (instruction->sig.ldunif || instruction->sig.ldunifa || instruction->sig.ldunifarf)
		fail(machine, "uniform loads other than to a register are not modeled");

	/* A special function starts no sooner than three words after the previous one. */
	if (special) {
		if (machine->last_special != MACHINE_NONE && machine->word < machine->last_special + 3)
			fail(machine, "special function too soon after another");
		machine->last_special = machine->word;
	}

	/* A switch word and its delay words start no special function, varying load or texture result read; a launch may sit there. */
	in_switch_window = instruction->sig.thrsw;
	if (machine->last_switch != MACHINE_NONE && machine->word <= machine->last_switch + 2)
		in_switch_window = true;
	if (in_switch_window) {
		if (special)
			fail(machine, "special function in a thread switch's delay words");
		if (instruction->sig.ldvary)
			fail(machine, "varying load in a thread switch's delay words");
		if (instruction->sig.ldtmu)
			fail(machine, "texture load in a thread switch's delay words");
	}

	/* The thread end's word and delay words write no register and read no payload. */
	in_end_window = machine->thread_end != MACHINE_NONE;
	if (instruction->sig.thrsw && (machine->last_marked || machine->setup->binary->starts_final != 0))
		in_end_window = true;
	if (in_end_window) {
		add_register = false;
		mul_register = false;
		if (instruction->alu.add.op != V3D_QPU_A_NOP && !instruction->alu.add.magic_write)
			add_register = v3d_qpu_add_op_has_dst(instruction->alu.add.op);
		if (instruction->alu.mul.op != V3D_QPU_M_NOP && !instruction->alu.mul.magic_write)
			mul_register = v3d_qpu_mul_op_has_dst(instruction->alu.mul.op);
		if (add_register || mul_register)
			fail(machine, "register write in the thread end's words");
		signal_register = v3d_qpu_sig_writes_address(machine->device, &instruction->sig);
		if (signal_register && !instruction->sig_magic)
			fail(machine, "signal register write in the thread end's words");
		if (instruction->sig.ldvary || special)
			fail(machine, "varying load or special function in the thread end's words");
		port_a = v3d_qpu_uses_mux(instruction, V3D_QPU_MUX_A);
		port_b = v3d_qpu_uses_mux(instruction, V3D_QPU_MUX_B);
		if (port_a && instruction->raddr_a < 3)
			fail(machine, "payload register read in the thread end's words");
		if (port_b && !instruction->sig.small_imm_b && instruction->raddr_b < 3)
			fail(machine, "payload register read in the thread end's words");
		if (uniforms != 0 && !instruction->sig.thrsw)
			fail(machine, "uniform read in the thread end's delay words");
	}
}

/* Reads the next word of the uniform stream through the setup's binding callback. */
static uint32_t
next_uniform(
	struct machine *machine)
{
	const struct bcm2711_shader_binary *binary;

	/* Every native consumption takes exactly one compiled descriptor. */
	binary = machine->setup->binary;
	if (machine->uniform_read >= binary->uniform_count)
		fail(machine, "uniform stream read beyond its descriptors");
	machine->uniform_read++;

	/* The caller turns the descriptor into the word the runtime would supply. */
	return machine->setup->uniform(machine->setup->context, &binary->uniforms[machine->uniform_read - 1]);
}

/* Converts a float to a signed integer, saturating and taking NaN to zero. */
static int32_t
float_to_signed(
	float number)
{
	/* Out-of-range values saturate rather than reaching undefined host conversion; NaN compares unequal to itself. */
	if (number != number)
		return 0;
	if (number >= 2147483648.0f)
		return 2147483647;
	if (number < -2147483648.0f)
		return (int32_t)0x80000000U;

	/* An in-range value truncates toward zero. */
	return (int32_t)number;
}

/* Converts a float to an unsigned integer, saturating and taking NaN to zero. */
static uint32_t
float_to_unsigned(
	float number)
{
	/* Out-of-range values saturate rather than reaching undefined host conversion; NaN compares unequal to itself. */
	if (number != number || number <= 0.0f)
		return 0;
	if (number >= 4294967296.0f)
		return 0xffffffffU;

	/* An in-range value truncates toward zero. */
	return (uint32_t)number;
}
