/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The machine encoding of the PowerVR Rogue unified shading cluster's
 * instructions.
 *
 * The cluster executes instruction groups.  A group is a header, the
 * encoded operations of up to four pipeline phases, the operand words
 * that route registers into the phases and the results out of them, and
 * padding.  A group's size varies with its content, so the encoder picks
 * the smallest operand form that can name every register the group uses
 * and fills in the length and decode-assist fields of the header.
 *
 * The encoder knows nothing of a shader's meaning: a compiler decides
 * which operation runs in which phase and how the internal feed-through
 * selectors connect them.  The same code runs on the build host.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_USC_ENCODE_H
#define KERN_DRIVERS_GPU_POWERVR_USC_ENCODE_H

#include <stddef.h>
#include <stdint.h>

/*
 * The largest encoded size of one phase's operation, in bytes: the
 * sum of products of the main unit, at fourteen, is the longest.
 */
#define POWERVR_USC_OPERATION_MAX	16U

/* The largest encoded size of one instruction group, in bytes. */
#define POWERVR_USC_GROUP_MAX		32U

/*
 * The alignment of a whole program in the instruction cache, in bytes.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/include/hwdef/rogue_hw_defs.h
 * (ROGUE_ICACHE_ALIGN 8).
 */
#define POWERVR_USC_PROGRAM_ALIGN	8U

/*
 * The kinds of arithmetic unit a group drives.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
 * (F_ALUTYPE: main 0b00, bitwise 0b10, control 0b11).
 */
enum powervr_usc_unit {
	POWERVR_USC_UNIT_MAIN = 0,
	POWERVR_USC_UNIT_BITWISE = 2,
	POWERVR_USC_UNIT_CONTROL = 3
};

/*
 * The phases of a main or bitwise group, in pipeline order.
 *
 * The main unit's second phase slot holds one of the pack, test or
 * conditional-move operations; the back end does memory and interface
 * work.  A bitwise group uses the first three slots.
 */
enum powervr_usc_phase {
	POWERVR_USC_PHASE_0 = 0,
	POWERVR_USC_PHASE_1 = 1,
	POWERVR_USC_PHASE_2 = 2,
	POWERVR_USC_PHASE_BACKEND = 3,
	POWERVR_USC_PHASE_COUNT = 4
};

/*
 * The register banks an operand word can name.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
 * (F_REGBANK: special 0, temp 1, vtxin 2, coeff 3, shared 4,
 * coeff_alt 5, idx0 6, idx1 7).
 */
enum powervr_usc_bank {
	POWERVR_USC_BANK_SPECIAL = 0,
	POWERVR_USC_BANK_TEMPORARY = 1,
	POWERVR_USC_BANK_VERTEX_INPUT = 2,
	POWERVR_USC_BANK_COEFFICIENT = 3,
	POWERVR_USC_BANK_SHARED = 4,
	POWERVR_USC_BANK_COEFFICIENT_ALTERNATE = 5,
	POWERVR_USC_BANK_INDEXED_0 = 6,
	POWERVR_USC_BANK_INDEXED_1 = 7
};

/*
 * The execution conditions of a group.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
 * (F_CC: e1_zx 0b00, e1_z1 0b01, ex_zx 0b10, e1_z0 0b11).
 */
enum powervr_usc_condition {
	POWERVR_USC_RUN_ENABLED = 0,
	POWERVR_USC_RUN_IF_PREDICATE = 1,
	POWERVR_USC_RUN_IGNORING_ENABLE = 2,
	POWERVR_USC_RUN_UNLESS_PREDICATE = 3
};

/*
 * The control unit's operations, named in a control group's header.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
 * (F_CTRLOP: b 0b0000, lapc 0b0001, savl 0b0010, cnd 0b0011,
 * wop 0b0100, wdf 0b0101, mutex 0b0110, nop 0b0111, sbo 0b1011).
 */
enum powervr_usc_control {
	POWERVR_USC_CONTROL_BRANCH = 0,
	POWERVR_USC_CONTROL_LINK_TO_COUNTER = 1,
	POWERVR_USC_CONTROL_SAVE_LINK = 2,
	POWERVR_USC_CONTROL_CONDITIONAL = 3,
	POWERVR_USC_CONTROL_WAIT_OUTPUT = 4,
	POWERVR_USC_CONTROL_WAIT_DATA = 5,
	POWERVR_USC_CONTROL_MUTEX = 6,
	POWERVR_USC_CONTROL_NOTHING = 7,
	POWERVR_USC_CONTROL_SHARED_BASE = 11
};

/*
 * The main unit's operation classes, in the top bits of an operation.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
 * (F_MAIN_OP: fadd 0b000, fadd_lp 0b001, fmul 0b010, fmul_lp 0b011,
 * sngl 0b100, int8_16 0b101, fmad_movc 0b110, int32_64_tst 0b111).
 */
enum powervr_usc_float_pair {
	POWERVR_USC_FLOAT_ADD = 0,
	POWERVR_USC_FLOAT_ADD_LOW_PRECISION = 1,
	POWERVR_USC_FLOAT_MULTIPLY = 2,
	POWERVR_USC_FLOAT_MULTIPLY_LOW_PRECISION = 3
};

/*
 * The single-source operations of the main unit.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
 * (F_SNGL_OP: rcp 0b0000, rsq 0b0001, log 0b0010, exp 0b0011,
 * logcn 0b0101, byp 0b0111, dsx 0b1000, dsy 0b1001, dsxf 0b1010,
 * dsyf 0b1011, sinc 0b1110, arctanc 0b1111).
 */
enum powervr_usc_single {
	POWERVR_USC_SINGLE_RECIPROCAL = 0,
	POWERVR_USC_SINGLE_RECIPROCAL_ROOT = 1,
	POWERVR_USC_SINGLE_LOG2 = 2,
	POWERVR_USC_SINGLE_EXP2 = 3,
	POWERVR_USC_SINGLE_LOG2_CLAMPED = 5,
	POWERVR_USC_SINGLE_PASS = 7,
	POWERVR_USC_SINGLE_DERIVATIVE_X = 8,
	POWERVR_USC_SINGLE_DERIVATIVE_Y = 9,
	POWERVR_USC_SINGLE_DERIVATIVE_X_FINE = 10,
	POWERVR_USC_SINGLE_DERIVATIVE_Y_FINE = 11,
	POWERVR_USC_SINGLE_SINE_CORRECTION = 14,
	POWERVR_USC_SINGLE_ARCTANGENT_CORRECTION = 15
};

/*
 * The integer add and multiply-add forms of the main unit.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
 * (F_INT32_64_OP: add6432 0b00, add64 0b01, madd32 0b10, madd64 0b11).
 */
enum powervr_usc_integer {
	POWERVR_USC_INTEGER_ADD_64_FROM_32 = 0,
	POWERVR_USC_INTEGER_ADD_64 = 1,
	POWERVR_USC_INTEGER_MULTIPLY_ADD_32 = 2,
	POWERVR_USC_INTEGER_MULTIPLY_ADD_64 = 3
};

/*
 * The data path operations of the back end that this encoder builds.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
 * (F_DMA_OP: idf 0b000, ld 0b001, st 0b010, atomic 0b101).
 */
enum powervr_usc_memory {
	POWERVR_USC_MEMORY_FENCE = 0,
	POWERVR_USC_MEMORY_LOAD = 1,
	POWERVR_USC_MEMORY_STORE = 2,
	POWERVR_USC_MEMORY_ATOMIC = 5
};

/*
 * The forms of the main unit's sum of products this encoder builds: one
 * operation writing a pixel's four 8-bit unsigned normalised channels
 * into its destination from 32-bit float sources, each channel a
 * multiply-add of the sources it names.
 *
 * Opaque writes red, green, blue and alpha from sources 0 to 3.  Over
 * and subtract take the destination's channels in source 0, the alpha
 * in source 1 and red, green and blue in sources 2 to 4: each channel
 * is the colour's plus (or minus) the destination's times one minus the
 * alpha (Vulkan's one and one minus source alpha, add or subtract).
 * Add and multiply take the destination in source 0 and red to alpha in
 * sources 1 to 4: the destination plus the colour, and the destination
 * times the colour.  The half forms read the colour as two registers of
 * half-float pairs, red and green in the low and high halves of one,
 * blue and alpha of the other (as an iterate and sample returning half
 * floats writes them): opaque half takes red and green in source 0 and
 * blue and alpha in source 1; over half the destination in source 0,
 * blue and alpha in source 1 and red and green in source 2.
 *
 * The encodings are derived from observation of the operation; each
 * form is the one byte pattern seen for it, with the registers in the
 * group's sources.
 */
enum powervr_usc_sum {
	POWERVR_USC_SUM_OPAQUE = 0,
	POWERVR_USC_SUM_OVER,
	POWERVR_USC_SUM_SUBTRACT,
	POWERVR_USC_SUM_ADD,
	POWERVR_USC_SUM_MULTIPLY,
	POWERVR_USC_SUM_OPAQUE_HALF,
	POWERVR_USC_SUM_OVER_HALF,
	POWERVR_USC_SUM_COUNT
};

/*
 * One register an operand word routes into or out of a group.
 *
 * An unused operand has used == 0.  An indexed bank's index field holds
 * the index bank and offset exactly as the hardware reads them.
 */
struct powervr_usc_register {
	uint8_t used;
	uint8_t bank;
	uint16_t index;
};

/*
 * One phase's encoded operation, ready to be placed in a group.
 *
 * A phase the group does not use has length 0 in a main or bitwise
 * group.  A control group always uses its single operation, whose
 * length may be 0 for operations that have no fields.
 */
struct powervr_usc_operation {
	uint8_t length;
	uint8_t bytes[POWERVR_USC_OPERATION_MAX];
};

/*
 * One instruction group as the compiler hands it to the encoder.
 *
 * The encoder fills in nothing here; it chooses the operand forms and
 * the header's length fields as it writes the bytes out.  Sources 0 to 2
 * are the lower operand set, 3 to 5 the upper one; the source selector
 * mux picks which of them feeds the first phase's internal input.
 */
struct powervr_usc_group {
	uint8_t unit;
	uint8_t condition;
	uint8_t end;
	uint8_t atomic;
	uint8_t repeat;
	uint8_t overlap_check;
	uint8_t write_0;
	uint8_t write_1;
	uint8_t control;
	uint8_t control_misc;
	struct powervr_usc_operation operation[POWERVR_USC_PHASE_COUNT];
	struct powervr_usc_register source[6];
	uint8_t mux_used;
	uint8_t mux;
	uint8_t internal[6];
	struct powervr_usc_register destination[2];

	/*
	 * A main group whose operation takes no internal selectors (the sum
	 * of products): the group carries no selector byte, and its mux and
	 * selectors must be zero.
	 */
	uint8_t internal_absent;
};

int powervr_usc_group_size(const struct powervr_usc_group *group, size_t *size);
int powervr_usc_program_encode(const struct powervr_usc_group *groups, size_t count, uint8_t *output, size_t room, size_t *length);
int powervr_usc_program_build(size_t count, int (*fetch)(void *context, size_t index, struct powervr_usc_group *group), void *context, uint8_t *output, size_t room, size_t *length);

int powervr_usc_float_pair(enum powervr_usc_float_pair kind, unsigned saturate, unsigned negate_0, unsigned absolute_0, unsigned absolute_1, unsigned floor_0, struct powervr_usc_operation *operation);
int powervr_usc_float_multiply_add(unsigned saturate, unsigned negate_0, unsigned absolute_0, unsigned negate_1, unsigned absolute_1, unsigned negate_2, unsigned absolute_2, unsigned floor_2, unsigned low_precision, struct powervr_usc_operation *operation);
int powervr_usc_single(enum powervr_usc_single kind, unsigned negate, unsigned absolute, struct powervr_usc_operation *operation);
int powervr_usc_pack(unsigned format, unsigned programmable, unsigned round_to_zero, unsigned scale, struct powervr_usc_operation *operation);
int powervr_usc_unpack(unsigned format, unsigned element, unsigned scale_round, struct powervr_usc_operation *operation);
int powervr_usc_sum_of_products(enum powervr_usc_sum form, struct powervr_usc_operation *operation);
int powervr_usc_integer(enum powervr_usc_integer kind, unsigned is_signed, unsigned carry_in, unsigned negate_0, unsigned absolute_0, unsigned negate_1, unsigned absolute_1, unsigned negate_2, unsigned absolute_2, struct powervr_usc_operation *operation);
int powervr_usc_test(unsigned comparison, unsigned type, unsigned element, unsigned predicate_write, unsigned phase_2_end, unsigned extended, struct powervr_usc_operation *operation);
int powervr_usc_move(unsigned select_1, unsigned select_0, unsigned mask_0, unsigned all_write, unsigned phase_2_end, unsigned extended, struct powervr_usc_operation *operation);
int powervr_usc_load(unsigned address_source, unsigned burst, unsigned burst_source, unsigned cache_mode, unsigned data_return, struct powervr_usc_operation *operation);
int powervr_usc_store(unsigned address_source, unsigned burst, unsigned burst_source, unsigned data_source, unsigned data_size, unsigned cache_mode, unsigned data_return, struct powervr_usc_operation *operation);
int powervr_usc_fence(unsigned address_source, unsigned data_return, struct powervr_usc_operation *operation);
int powervr_usc_atomic(unsigned kind, unsigned source, unsigned destination, unsigned data_return, struct powervr_usc_operation *operation);
int powervr_usc_sample(unsigned normalised, unsigned data_return, unsigned dimensions, unsigned channels, unsigned lod_mode, struct powervr_usc_operation *operation);
int powervr_usc_emit_pixel(unsigned free_partition, struct powervr_usc_operation *operation);
int powervr_usc_visibility_test(unsigned predicate_write, unsigned alpha_test, unsigned feedback, struct powervr_usc_operation *operation);
int powervr_usc_iterate(unsigned destination, unsigned coefficient, unsigned perspective, unsigned w_coefficient, unsigned mode, unsigned count, unsigned data_return, struct powervr_usc_operation *operation);
int powervr_usc_iterate_sample(unsigned destination, unsigned coefficient, unsigned perspective, unsigned w_coefficient, unsigned image, unsigned sampler, unsigned mode, unsigned dimensions, unsigned channels, unsigned normalised, unsigned data_return, unsigned schedule, unsigned half, struct powervr_usc_operation *operation);
int powervr_usc_vertex_output(unsigned kind, unsigned write_select, unsigned source, unsigned immediate, unsigned address, struct powervr_usc_operation *operation);
int powervr_usc_bit_count_shift(unsigned count_source, unsigned count_kind, unsigned mask_from_source, unsigned shift_kind, struct powervr_usc_operation *operation);
int powervr_usc_bit_immediate(uint32_t value, unsigned wide, unsigned count_source, unsigned count_kind, unsigned shift_kind, struct powervr_usc_operation *operation);
int powervr_usc_bit_logic(unsigned kind, unsigned mask_a, unsigned mask_b, struct powervr_usc_operation *operation);
int powervr_usc_bit_shift(unsigned kind, unsigned test_source, unsigned test_nonzero, unsigned predicate_write, struct powervr_usc_operation *operation);
int powervr_usc_branch(int32_t offset, unsigned absolute, unsigned link, unsigned prediction, struct powervr_usc_operation *operation);
int powervr_usc_conditional(unsigned kind, unsigned predicate, unsigned adjust, struct powervr_usc_operation *operation);
int powervr_usc_mutex(unsigned identifier, unsigned action, struct powervr_usc_operation *operation);
int powervr_usc_nothing(struct powervr_usc_operation *operation);
int powervr_usc_empty(struct powervr_usc_operation *operation);
int powervr_usc_shared_base(unsigned offset, unsigned shared_target, struct powervr_usc_operation *operation);

#endif /* KERN_DRIVERS_GPU_POWERVR_USC_ENCODE_H */
