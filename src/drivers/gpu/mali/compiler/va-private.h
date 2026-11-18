/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Mali compiler's own interfaces: the list of Valhall instructions
 * the lowering writes (va-compile.c), the source and field encoding
 * (va-encode.c), and the pass that gives the list its slots, waits and
 * branch offsets and writes it as words (va-schedule.c).
 */

#ifndef KERN_DRIVERS_GPU_MALI_COMPILER_VA_PRIVATE_H
#define KERN_DRIVERS_GPU_MALI_COMPILER_VA_PRIVATE_H

#include "drivers/gpu/mali/compiler/shader.h"
#include "drivers/gpu/mali/compiler/va-isa.h"

#include <stddef.h>
#include <stdint.h>

/*
 * What the scheduler must know of an instruction beyond its word: that it
 * is a message (it takes a dependency slot and its results arrive later),
 * that it reads or writes memory a later access must not overtake, that
 * it loads a varying (which serializes with the other varying loads),
 * that it is an ATEST or a ZS_EMIT (both on slot 0 after the wait0126), a
 * BLEND or a BARRIER (each with its own waits), that it is a branch, and
 * that a branch jumps to it.
 */
#define VA_INST_MESSAGE		0x01U
#define VA_INST_MEMORY		0x02U
#define VA_INST_VARYING		0x04U
#define VA_INST_ATEST		0x08U
#define VA_INST_BLEND		0x10U
#define VA_INST_BARRIER		0x20U
#define VA_INST_BRANCH		0x40U
#define VA_INST_TARGET		0x80U

/* No branch target. */
#define VA_NO_TARGET		(-1)

/*
 * One instruction of the list: its word without the slot and the flow
 * control, the registers it reads and writes (bit r for register r), the
 * index in the list of the instruction a branch jumps to, the flow
 * control the lowering asks for (reconvergence of a branch), and its
 * VA_INST_* properties.
 */
struct va_inst {
	uint64_t bits;
	uint64_t reads;
	uint64_t writes;
	int32_t target;
	uint8_t flow;
	uint8_t flags;
};

/*
 * A growing list of instructions.
 *
 * The lowering appends to it; failed records that an append ran out of
 * memory, so the lowering checks once at the end.
 */
struct va_code {
	struct va_inst *list;
	uint32_t count;
	uint32_t room;
	int failed;
};

/* Appends one instruction to the list (a failure is kept in the list). */
void va_code_append(struct va_code *code, const struct va_inst *inst);

/* Releases the list's storage. */
void va_code_release(struct va_code *code);

/* The source byte of register reg. */
uint8_t va_source_register(uint32_t reg);

/* The entry of the constant table holding value, or -1. */
int va_immediate_entry(uint32_t value);

/* The value of entry entry of the constant table. */
uint32_t va_immediate_value(uint32_t entry);

/* The source byte of entry entry of the constant table. */
uint8_t va_source_immediate(uint32_t entry);

/* The source byte of FAU uniform word word (its page goes in the word's page field). */
uint8_t va_source_uniform(uint32_t word);

/* The FAU page of uniform word word. */
uint32_t va_uniform_page(uint32_t word);

/* The source byte of special value number's word word. */
uint8_t va_source_special(uint32_t number, uint32_t word);

/* A field's value moved into place in the word. */
uint64_t va_field(uint64_t value, uint32_t shift);

/* The destination field of an arithmetic instruction writing register reg. */
uint64_t va_dest(uint32_t reg);

/*
 * The staging fields of a message: count registers from reg, read or
 * written as control says.
 */
uint64_t va_staging(uint32_t reg, uint32_t count, uint32_t control);

/* The mask of count registers from reg. */
uint64_t va_register_mask(uint32_t reg, uint32_t count);

/*
 * Gives the list its dependency slots, its waits and its end, and writes
 * it as little-endian words to out (room bytes), followed by the zero
 * padding of the prefetch to a MALI_SHADER_CODE_ALIGNMENT boundary:
 * the bytes written, or 0 when they do not fit or memory runs out.
 */
size_t va_schedule_write(const struct va_code *code, uint8_t *out, size_t room);

/* The bytes va_schedule_write() may write for the list, at most. */
size_t va_schedule_bound(const struct va_code *code);

#endif /* KERN_DRIVERS_GPU_MALI_COMPILER_VA_PRIVATE_H */
