/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host test of ws269 p010 (the compiler): each kind of machine operation
 * is built into an instruction group, and the group's fields (unit,
 * write enables, which source slot holds which register, the source and
 * internal selectors) and its phases' bytes are compared with ones built
 * by hand from Mesa 26.2.5's tables: the group layout from pco_map.py's
 * group_map, the bits from pco_isa.py's pieces.  The operand words and
 * the header are p008's encoder's, tested there.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "drivers/gpu/powervr/compiler/shader-private.h"
#include "drivers/gpu/powervr/compiler/usc-encode.h"

static unsigned passed;
static unsigned failed;

/* A machine operation with no register named. */
static struct shader_machine_operation
blank(
	unsigned kind)
{
	struct shader_machine_operation operation;
	unsigned index;

	memset(&operation, 0, sizeof(operation));
	operation.kind = (uint8_t)kind;
	for (index = 0; index < 2; index++)
		operation.destination[index].bank = 0xff;
	for (index = 0; index < 4; index++)
		operation.source[index].bank = 0xff;
	return operation;
}

static struct shader_machine_register
reg(
	unsigned bank,
	unsigned index)
{
	struct shader_machine_register value;

	value.bank = (uint8_t)bank;
	value.index = (uint16_t)index;
	return value;
}

/* Compares one phase's bytes. */
static int
phase_is(
	const struct powervr_usc_group *group,
	unsigned phase,
	const uint8_t *bytes,
	unsigned length)
{
	if (group->operation[phase].length != length)
		return 0;
	return memcmp(group->operation[phase].bytes, bytes, length) == 0;
}

/* Compares a source or destination slot. */
static int
slot_is(
	const struct powervr_usc_register *operand,
	unsigned bank,
	unsigned index)
{
	return operand->used == 1 && operand->bank == bank && operand->index == index;
}

static void
check(
	int ok,
	const char *name)
{
	if (ok) {
		passed++;
	} else {
		failed++;
		printf("FAIL %s\n", name);
	}
}

/* Prints a group's phases when a check fails, to see what was built. */
static void
dump(
	const struct powervr_usc_group *group)
{
	unsigned phase;
	unsigned index;

	for (phase = 0; phase < 4; phase++) {
		printf("  phase %u:", phase);
		for (index = 0; index < group->operation[phase].length; index++)
			printf(" %02x", group->operation[phase].bytes[index]);
		printf("\n");
	}
	printf("  internal %u %u %u %u %u %u mux %u/%u\n", group->internal[0], group->internal[1], group->internal[2], group->internal[3], group->internal[4], group->internal[5], group->mux_used, group->mux);
}

int
main(void)
{
	struct shader_machine_operation operation;
	struct powervr_usc_group group;
	size_t size;
	int ok;

	/*
	 * A move with negate: main, phase 0 the extended single-source pass
	 * (main_op sngl 0b100, ext0 1, sngl_op byp 0b0111 = 0x97; s0neg 1:1
	 * = 0x02), s0 the source, w0 the destination from ft0.
	 */
	{
		static const uint8_t pass[] = { 0x97, 0x02 };

		operation = blank(MACHINE_MOVE);
		operation.destination[0] = reg(1, 8);
		operation.source[0] = reg(1, 9);
		operation.modifiers[0] = POWERVR_IR_NEGATE;
		ok = shader_backend_group(&operation, 0, &group) == 0;
		ok = ok && group.unit == POWERVR_USC_UNIT_MAIN && group.write_0 == 1 && group.write_1 == 0;
		ok = ok && slot_is(&group.source[0], 1, 9) && slot_is(&group.destination[0], 1, 8);
		ok = ok && group.internal[4] == 0 && group.mux_used == 0;
		ok = ok && phase_is(&group, 0, pass, 2);
		check(ok, "move with negate");
	}

	/*
	 * The instance number: phase 2's extended move (main_op fmad_movc
	 * 0b110, ext0 1, movw1 ft0 0, movw0 fte 3 = 0xd3; maskw0 all 0xf
	 * at 1:5..2, aw 1:1, p2end 1:0 = 0x3f), s1 the special register,
	 * the source selector s1 (0b100), is4 fte.
	 */
	{
		static const uint8_t move[] = { 0xd3, 0x3f };

		operation = blank(MACHINE_MOVE_SPECIAL);
		operation.destination[0] = reg(1, 10);
		operation.source[0] = reg(0, 51);
		ok = shader_backend_group(&operation, 0, &group) == 0;
		ok = ok && slot_is(&group.source[1], 0, 51) && !group.source[0].used && slot_is(&group.destination[0], 1, 10);
		ok = ok && group.mux_used == 1 && group.mux == 4 && group.internal[4] == 3 && group.write_0 == 1;
		ok = ok && phase_is(&group, 2, move, 2) && group.operation[0].length == 0;
		check(ok, "instance number through movs1");
		if (!ok)
			dump(&group);
	}

	/*
	 * A float maximum: phases 0 and 1 the short passes (0x87); phase 2
	 * the extended move (movw0 ft0: 0xd0, maskw0 all: 0x3c) then the
	 * extended test (main_op int32_64_tst 0b111, ext0 1, tst_op_2_0
	 * g 0b101 at 0:3..1 = 0xfa; type f32 0, p2end 1 at 1:4 = 0x10);
	 * s0 a, s3 b, is1 ft0, is2 ft1, is4 ft1.
	 */
	{
		static const uint8_t pass[] = { 0x87 };
		static const uint8_t phase2[] = { 0xd0, 0x3c, 0xfa, 0x10 };

		operation = blank(MACHINE_MINMAX);
		operation.destination[0] = reg(1, 8);
		operation.source[0] = reg(1, 9);
		operation.source[1] = reg(1, 10);
		operation.variant = 5;
		operation.immediate = 0;
		ok = shader_backend_group(&operation, 0, &group) == 0;
		ok = ok && slot_is(&group.source[0], 1, 9) && slot_is(&group.source[3], 1, 10);
		ok = ok && group.internal[1] == 0 && group.internal[2] == 0 && group.internal[4] == 1;
		ok = ok && phase_is(&group, 0, pass, 1) && phase_is(&group, 1, pass, 1) && phase_is(&group, 2, phase2, 4);
		check(ok, "float maximum");
		if (!ok)
			dump(&group);
	}

	/*
	 * A float less-than to a Boolean: phase 2 the move of fte (0xd3,
	 * 0x3c), the extended test (op l 0b1000: tst_op_2_0 0, op_3 1 at
	 * 1:0: 0xf0, 0x01), the zero pack (main_op sngl 0b100, ext0 1,
	 * sngl_op pck 0b1100 = 0x9c; format zero 0b11110 = 0x1e); s1 the
	 * all-ones constant 143 through the selector, is4 ft2.
	 */
	{
		static const uint8_t phase2[] = { 0xd3, 0x3c, 0xf0, 0x01, 0x9c, 0x1e };
		static const uint8_t equal[] = { 0xd3, 0x3c, 0xe8, 0x9c, 0x1e };

		operation = blank(MACHINE_COMPARE);
		operation.destination[0] = reg(1, 8);
		operation.source[0] = reg(1, 9);
		operation.source[1] = reg(1, 10);
		operation.variant = 8;
		ok = shader_backend_group(&operation, 0, &group) == 0;
		ok = ok && slot_is(&group.source[0], 1, 9) && slot_is(&group.source[1], 0, 143) && slot_is(&group.source[3], 1, 10);
		ok = ok && group.mux_used == 1 && group.mux == 4 && group.internal[4] == 2 && group.write_0 == 1;
		ok = ok && phase_is(&group, 2, phase2, 6);
		check(ok, "float less to a Boolean");
		if (!ok)
			dump(&group);

		/* Equal is the short test (tst_op_2_0 e 0b100: 0xe8). */
		operation.variant = 4;
		ok = shader_backend_group(&operation, 0, &group) == 0 && phase_is(&group, 2, equal, 5);
		check(ok, "float equal takes the short test");
		if (!ok)
			dump(&group);
	}

	/*
	 * A select: the move of ft1 (0xd1, 0x3c), the extended zero test
	 * of u32 with p2end (0xf0; type u32 0b101 at 1:7..5 and p2end:
	 * 0xb0); s0 a, s1 the condition through the selector and is1 fte,
	 * s3 b, is4 ft0.
	 */
	{
		static const uint8_t phase2[] = { 0xd1, 0x3c, 0xf0, 0xb0 };

		operation = blank(MACHINE_SELECT);
		operation.destination[0] = reg(1, 8);
		operation.source[0] = reg(1, 11);
		operation.source[1] = reg(1, 9);
		operation.source[2] = reg(1, 10);
		ok = shader_backend_group(&operation, 0, &group) == 0;
		ok = ok && slot_is(&group.source[0], 1, 9) && slot_is(&group.source[1], 1, 11) && slot_is(&group.source[3], 1, 10);
		ok = ok && group.mux == 4 && group.internal[1] == 1 && group.internal[4] == 0;
		ok = ok && phase_is(&group, 2, phase2, 4);
		check(ok, "select");
		if (!ok)
			dump(&group);
	}

	/* The if's zero test writes p0 (pwen 0:0: 0xf1) and no register. */
	{
		static const uint8_t phase2[] = { 0xd3, 0x3c, 0xf1, 0xa0, 0x9c, 0x1e };

		operation = blank(MACHINE_TEST_ZERO);
		operation.source[0] = reg(1, 9);
		ok = shader_backend_group(&operation, 0, &group) == 0;
		ok = ok && group.write_0 == 0 && !group.destination[0].used && phase_is(&group, 2, phase2, 6);
		check(ok, "zero test to p0");
		if (!ok)
			dump(&group);
	}

	/*
	 * A conditional start, p0 false, adjust 1: the control group's cnd
	 * (adjust 1 at 0:6..5, pcnd p0_false 0b11 at 0:4..3, cndinst st 0:
	 * 0x38), run ignoring the enable, the counter in s0 and w0, zero in s3.
	 */
	{
		static const uint8_t start[] = { 0x38 };

		operation = blank(MACHINE_CONDITION);
		operation.variant = 0;
		operation.condition = POWERVR_USC_RUN_IGNORING_ENABLE;
		operation.immediate = 3;
		operation.immediate_2 = 1;
		operation.destination[0] = reg(1, 0);
		operation.source[0] = reg(1, 0);
		ok = shader_backend_group(&operation, 0, &group) == 0;
		ok = ok && group.unit == POWERVR_USC_UNIT_CONTROL && group.control == 3 && group.write_0 == 1;
		ok = ok && group.condition == 2 && slot_is(&group.source[0], 1, 0) && slot_is(&group.source[3], 0, 0) && slot_is(&group.destination[0], 1, 0);
		ok = ok && phase_is(&group, 0, start, 1);
		check(ok, "conditional start");
		if (!ok)
			dump(&group);
	}

	/*
	 * A branch when no lane runs, 12 bytes on: bpred allp 0b01 at
	 * 0:3..2 (0x04), the offset halved at 1:7..1 (6 << 1 = 0x0c).
	 */
	{
		static const uint8_t branch[] = { 0x04, 0x0c, 0x00, 0x00, 0x00 };

		operation = blank(MACHINE_BRANCH);
		operation.variant = 1;
		ok = shader_backend_group(&operation, 12, &group) == 0;
		ok = ok && group.control == 0 && phase_is(&group, 0, branch, 5);
		check(ok, "branch when no lane runs");
	}

	/*
	 * A perspective iteration of one word: dest 9, coefficient 8
	 * (quartered 2 at 1:7..2) with p iter_mul 1 (0x09), w at 0, mode
	 * pixel, count 1 at 3:7..4 (0x10), data return counter 0.
	 */
	{
		static const uint8_t iterate[] = { 0x09, 0x09, 0x00, 0x10, 0x00 };

		operation = blank(MACHINE_ITERATE);
		operation.destination[0] = reg(1, 9);
		operation.immediate = 8;
		operation.immediate_2 = 0;
		ok = shader_backend_group(&operation, 0, &group) == 0;
		ok = ok && group.unit == POWERVR_USC_UNIT_CONTROL && group.control == 12 && phase_is(&group, 0, iterate, 5);
		ok = ok && powervr_usc_group_size(&group, &size) == 0;
		check(ok, "iteration");
		if (!ok)
			dump(&group);
	}

	/*
	 * An iterate and sample (q1546, I_ITRSMP without ext5): dest 10,
	 * coefficient 4 (quartered 1 at 1:7..2) with p iter_mul (0x05),
	 * texoff 0 and drc 0 and mode pixel (0x00), smpoff 4 (quartered 1
	 * at 3:7..2) with dmn 2d 0b10 (0x06), woff 0 (0x00), chan 4 sent as
	 * 3 at 5:3..2 and fcnorm 1 (0x0d); control op itrsmp 0b1000.
	 */
	{
		static const uint8_t iterate_sample[] = { 0x0a, 0x05, 0x00, 0x06, 0x00, 0x0d };
		uint8_t iterate_waits[6] = { 0x0a, 0x05, 0x00, 0x06, 0x00, 0x0d };

		operation = blank(MACHINE_ITERATE_SAMPLE);
		operation.destination[0] = reg(1, 10);
		operation.source[0] = reg(4, 0);
		operation.source[2] = reg(4, 4);
		operation.immediate = 4;
		operation.immediate_2 = 0;
		ok = shader_backend_group(&operation, 0, &group) == 0;
		ok = ok && group.unit == POWERVR_USC_UNIT_CONTROL && group.control == 8 && phase_is(&group, 0, iterate_sample, 6);
		ok = ok && powervr_usc_group_size(&group, &size) == 0;
		check(ok, "iterate and sample");
		if (!ok)
			dump(&group);

		/* Waiting for its own data (q1548): sched_ctrl wdf 0b10 at 5:5..4 (0x2d). */
		iterate_waits[5] = 0x2d;
		operation.variant = 2;
		ok = shader_backend_group(&operation, 0, &group) == 0;
		ok = ok && phase_is(&group, 0, iterate_waits, 6);
		check(ok, "iterate and sample waiting for its data");
		if (!ok)
			dump(&group);
	}

	/*
	 * A sample: the brief form (dma 0b111, fcnorm 1, drc 0, smp 0b100:
	 * 0xf4; dmn 2d 0b10 at 1:6..5, chan 4 sent as 3 at 1:3..2: 0x4c);
	 * image state s0, coordinates s1, sampler state s2, results s4.
	 */
	{
		static const uint8_t sample[] = { 0xf4, 0x4c };

		operation = blank(MACHINE_SAMPLE);
		operation.destination[0] = reg(1, 12);
		operation.source[0] = reg(4, 0);
		operation.source[1] = reg(1, 4);
		operation.source[2] = reg(4, 4);
		ok = shader_backend_group(&operation, 0, &group) == 0;
		ok = ok && slot_is(&group.source[0], 4, 0) && slot_is(&group.source[1], 1, 4) && slot_is(&group.source[2], 4, 4) && slot_is(&group.source[4], 1, 12);
		ok = ok && group.write_0 == 0 && phase_is(&group, 3, sample, 2);
		check(ok, "sample");
		if (!ok)
			dump(&group);
	}

	/*
	 * A vertex output write to word 5: uvsw 0b000, dsel w0, imm 1 at
	 * 0:3, write 0b000 (0x08); the address 5; s0 through the selector
	 * (s0) and is4 fte.
	 */
	{
		static const uint8_t write[] = { 0x08, 0x05 };
		static const uint8_t end[] = { 0x05 };

		operation = blank(MACHINE_VERTEX_WRITE);
		operation.source[0] = reg(1, 9);
		operation.immediate = 5;
		ok = shader_backend_group(&operation, 0, &group) == 0;
		ok = ok && slot_is(&group.source[0], 1, 9) && group.mux_used == 1 && group.mux == 0 && group.internal[4] == 3;
		ok = ok && phase_is(&group, 3, write, 2);
		check(ok, "vertex output write");

		/* The ending: emit and end the task (0b101), with the end flag. */
		operation = blank(MACHINE_VERTEX_END);
		ok = shader_backend_group(&operation, 0, &group) == 0 && group.end == 1 && phase_is(&group, 3, end, 1);
		check(ok, "vertex end");
	}

	/*
	 * A load: ld with an immediate burst (dma 0b111, immbl 1, drc 0,
	 * ld 0b001: 0xf1; srcseladd s0, burst 1 at 1:4..2: 0x04; 0x00);
	 * the address pair in s0, the result's register in s3.
	 */
	{
		static const uint8_t load[] = { 0xf1, 0x04, 0x00 };

		operation = blank(MACHINE_LOAD);
		operation.destination[0] = reg(1, 9);
		operation.source[0] = reg(1, 6);
		ok = shader_backend_group(&operation, 0, &group) == 0;
		ok = ok && slot_is(&group.source[0], 1, 6) && slot_is(&group.source[3], 1, 9) && group.write_0 == 0;
		ok = ok && phase_is(&group, 3, load, 3);
		check(ok, "load");
		if (!ok)
			dump(&group);
	}

	/*
	 * A 64-bit address add: the short int32_64 (main_op 0b111, ext0 0,
	 * s 0, s2neg 0, add6432 0b00: 0xe0); both writes, is4 ft0, is5 fte.
	 */
	{
		static const uint8_t add[] = { 0xe0 };

		operation = blank(MACHINE_ADD64);
		operation.destination[0] = reg(1, 6);
		operation.destination[1] = reg(1, 7);
		operation.source[0] = reg(1, 9);
		operation.source[1] = reg(1, 10);
		operation.source[2] = reg(1, 11);
		ok = shader_backend_group(&operation, 0, &group) == 0;
		ok = ok && group.write_0 == 1 && group.write_1 == 1 && group.internal[4] == 0 && group.internal[5] == 3;
		ok = ok && phase_is(&group, 0, add, 1);
		check(ok, "address add");
		if (!ok)
			dump(&group);
	}

	/*
	 * A bitwise and: phase 0 passes s2 (count_src s2, count_op byp
	 * 0b10 as cnt_byp 0:1 and cnt 0:5: 0x02), phase 1 the logical and
	 * (ph1 0:6, logical_op and 1: 0x41); s2 a, s3 b.  A left shift:
	 * the same phase 0, phase 2 lsl (0x00); s1 the value, s4 the count.
	 */
	{
		static const uint8_t pass[] = { 0x02 };
		static const uint8_t logic[] = { 0x41 };
		static const uint8_t shift[] = { 0x00 };

		operation = blank(MACHINE_LOGIC);
		operation.destination[0] = reg(1, 8);
		operation.source[0] = reg(1, 9);
		operation.source[1] = reg(1, 10);
		operation.variant = 1;
		ok = shader_backend_group(&operation, 0, &group) == 0;
		ok = ok && group.unit == POWERVR_USC_UNIT_BITWISE && slot_is(&group.source[2], 1, 9) && slot_is(&group.source[3], 1, 10);
		ok = ok && phase_is(&group, 0, pass, 1) && phase_is(&group, 1, logic, 1);
		check(ok, "bitwise and");
		if (!ok)
			dump(&group);

		operation = blank(MACHINE_SHIFT);
		operation.destination[0] = reg(1, 8);
		operation.source[0] = reg(1, 9);
		operation.source[1] = reg(1, 10);
		ok = shader_backend_group(&operation, 0, &group) == 0;
		ok = ok && slot_is(&group.source[1], 1, 9) && slot_is(&group.source[4], 1, 10);
		ok = ok && phase_is(&group, 0, pass, 1) && phase_is(&group, 2, shift, 1);
		check(ok, "left shift");
		if (!ok)
			dump(&group);
	}

	/*
	 * A colour's pack: phase 2's pck (main_op sngl 0b100, ext0 1,
	 * sngl_op pck 0b1100 = 0x9c; scale 1:5, format u8888 0 = 0x20),
	 * repeated over the four registers from s2, which reaches it
	 * through the source selector (s2, 0b101) and is3 fte; is4 ft2.
	 * Its unpack: upck (0x9c; scale_rtz 1:5, element 0, format 0 =
	 * 0x20) in phase 0, repeated over four destinations.  The pixel
	 * output's move sets the overlap check.
	 */
	{
		static const uint8_t pack[] = { 0x9c, 0x20 };
		static const uint8_t pass[] = { 0x87 };

		operation = blank(MACHINE_PACK);
		operation.destination[0] = reg(1, 1);
		operation.source[0] = reg(1, 4);
		operation.variant = 0;
		operation.immediate = 4;
		operation.immediate_2 = 1;
		ok = shader_backend_group(&operation, 0, &group) == 0;
		ok = ok && group.repeat == 4 && slot_is(&group.source[2], 1, 4) && group.mux_used == 1 && group.mux == 5;
		ok = ok && group.internal[3] == 3 && group.internal[4] == 2 && phase_is(&group, 2, pack, 2);
		check(ok, "colour pack");
		if (!ok)
			dump(&group);

		operation = blank(MACHINE_UNPACK);
		operation.destination[0] = reg(1, 12);
		operation.source[0] = reg(1, 1);
		operation.variant = 0;
		operation.immediate = 4;
		operation.immediate_2 = 1;
		ok = shader_backend_group(&operation, 0, &group) == 0;
		ok = ok && group.repeat == 4 && slot_is(&group.source[0], 1, 1) && phase_is(&group, 0, pack, 2);
		check(ok, "colour unpack");
		if (!ok)
			dump(&group);

		/*
		 * A half-float colour's pack and unpack: the same groups with
		 * format f16f16 0b01110 and no scale (0x0e), repeated over two
		 * registers (Mesa 26.2.5 pco_trans_nir.c: nir_op_pack_half_2x16
		 * and unpack_half_2x16, rpt 2, PCO_PCK_FMT_F16F16).
		 */
		{
			static const uint8_t half[] = { 0x9c, 0x0e };

			operation = blank(MACHINE_PACK);
			operation.destination[0] = reg(1, 1);
			operation.source[0] = reg(1, 4);
			operation.variant = 14;
			operation.immediate = 2;
			ok = shader_backend_group(&operation, 0, &group) == 0;
			ok = ok && group.repeat == 2 && slot_is(&group.source[2], 1, 4) && phase_is(&group, 2, half, 2);
			check(ok, "half colour pack");
			if (!ok)
				dump(&group);

			operation = blank(MACHINE_UNPACK);
			operation.destination[0] = reg(1, 12);
			operation.source[0] = reg(1, 1);
			operation.variant = 14;
			operation.immediate = 2;
			ok = shader_backend_group(&operation, 0, &group) == 0;
			ok = ok && group.repeat == 2 && slot_is(&group.source[0], 1, 1) && phase_is(&group, 0, half, 2);
			check(ok, "half colour unpack");
			if (!ok)
				dump(&group);
		}

		operation = blank(MACHINE_MOVE);
		operation.destination[0] = reg(0, 32);
		operation.source[0] = reg(1, 1);
		operation.variant = 1;
		ok = shader_backend_group(&operation, 0, &group) == 0;
		ok = ok && group.overlap_check == 1 && slot_is(&group.destination[0], 0, 32) && phase_is(&group, 0, pass, 1);
		check(ok, "pixel output move with the overlap check");
	}

	/*
	 * The sum of products (q1548): the whole group of the opaque, over
	 * and add forms with the program's alignment, byte for byte against
	 * the groups the forms were derived from by observation (registers
	 * r0 to r2, the internal register 0 and the pixel output).
	 */
	{
		static const struct {
			unsigned form;
			struct { unsigned bank; unsigned index; } sources[4];
			uint8_t bytes[32];
			unsigned length;
			const char *name;
		} sums[] = {
			{ POWERVR_USC_SUM_OPAQUE, { { 1, 0 }, { 1, 1 }, { 1, 2 }, { 0, 36 } },
			  { 0x3c, 0x8a, 0x80, 0x94, 0x99, 0x99, 0x0f, 0x06, 0x4f, 0x32, 0x42, 0x00, 0x00, 0x0b, 0x99, 0x99,
			    0xf0, 0xc0, 0x61, 0x00, 0x42, 0x24, 0x20, 0xff }, 24, "opaque sum of products group" },
			{ POWERVR_USC_SUM_OVER, { { 1, 0 }, { 1, 1 }, { 1, 2 }, { 0, 36 } },
			  { 0x3c, 0x8a, 0x80, 0x94, 0x22, 0x22, 0x0f, 0xfe, 0x7f, 0xa8, 0xb9, 0xd0, 0x00, 0x0f, 0x73, 0x24,
			    0x00, 0xa0, 0x44, 0x01, 0x40, 0xc1, 0xa2, 0x20 }, 24, "over sum of products group" },
			{ POWERVR_USC_SUM_ADD, { { 1, 0 }, { 1, 1 }, { 1, 2 }, { 0, 36 } },
			  { 0x30, 0x8a, 0x80, 0x94, 0x99, 0x99, 0x0f, 0xfe, 0x4f, 0xa8, 0xb9, 0x20, 0x00, 0x0f, 0x42, 0x73,
			    0x00, 0xa0, 0x60, 0x00, 0x41, 0xc2, 0xc4, 0x01, 0x20, 0xff, 0xf3, 0xff, 0xff, 0xff, 0xff, 0xff }, 32, "add sum of products group" }
		};
		uint8_t code[64];
		size_t length;
		unsigned sum;
		unsigned word;

		for (sum = 0; sum < sizeof(sums) / sizeof(sums[0]); sum++) {
			operation = blank(MACHINE_SUM_OF_PRODUCTS);
			operation.variant = (uint8_t)sums[sum].form;
			operation.overlap = 1;
			operation.end = 1;
			operation.destination[0] = reg(0, 32);
			for (word = 0; word < 4; word++)
				operation.source[word] = reg(sums[sum].sources[word].bank, sums[sum].sources[word].index);
			ok = shader_backend_group(&operation, 0, &group) == 0;
			ok = ok && powervr_usc_program_encode(&group, 1, code, sizeof(code), &length) == 0;
			ok = ok && length == sums[sum].length && memcmp(code, sums[sum].bytes, length) == 0;
			check(ok, sums[sum].name);
			if (!ok) {
				for (word = 0; word < length; word++)
					printf(" %02x", code[word]);
				printf("\n");
			}
		}
	}

	/*
	 * The alpha feedback of a discarding fragment shader (Mesa's alphaf):
	 * main, the back end's vistest (backend_op 0b100 at 0:7..5, pwen 0,
	 * vistest_op atst 1 at 0:1, ifb 0 = 0x82), sources zero, zero and 7
	 * of the special bank, run where p0 holds (e1_z1, 1), under the
	 * overlap check; then the alpha test with its own feedback and p0
	 * written (alphatst: pwen 0:2, ifb 0:0 = 0x87).
	 */
	{
		static const uint8_t alphaf[] = { 0x82 };
		static const uint8_t alphatst[] = { 0x87 };
		struct powervr_usc_operation part;

		operation = blank(MACHINE_FEEDBACK);
		operation.condition = POWERVR_USC_RUN_IF_PREDICATE;
		ok = shader_backend_group(&operation, 0, &group) == 0;
		ok = ok && group.unit == POWERVR_USC_UNIT_MAIN && group.condition == 1 && group.overlap_check == 1;
		ok = ok && group.write_0 == 0 && group.write_1 == 0;
		ok = ok && slot_is(&group.source[0], 0, 0) && slot_is(&group.source[1], 0, 0) && slot_is(&group.source[2], 0, 7);
		ok = ok && phase_is(&group, POWERVR_USC_PHASE_BACKEND, alphaf, 1);
		check(ok, "alpha feedback where p0 holds");
		ok = powervr_usc_visibility_test(1, 1, 1, &part) == 0 && part.length == 1 && memcmp(part.bytes, alphatst, 1) == 0;
		check(ok, "alpha test with its feedback, writing p0");
	}

	printf("powervr compiler group host test: %u passed, %u failed\n", passed, failed);
	return failed == 0 ? 0 : 1;
}
