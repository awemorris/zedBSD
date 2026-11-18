/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Mali compiler's instruction list and the encoding of the parts of
 * a Valhall word every instruction shares: sources, destinations and
 * staging registers.
 */

#include "drivers/gpu/mali/compiler/va-private.h"

#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include <stddef.h>
#include <stdint.h>

/* The instructions the list grows by when it is full. */
#define VA_CODE_GROWTH		256U

/*
 * The constant table every instruction may read without a register.
 *
 * Values referenced: Mesa 26.2.6
 * src/panfrost/compiler/bifrost/valhall/ISA.xml (lut "Immediates").
 */
static const uint32_t va_immediates[32] = {
	0x00000000U, 0xFFFFFFFFU, 0x7FFFFFFFU, 0xFAFCFDFEU,
	0x01000000U, 0x80002000U, 0x70605030U, 0xC0B0A090U,
	0x03020100U, 0x07060504U, 0x0B0A0908U, 0x0F0E0D0CU,
	0x13121110U, 0x17161514U, 0x1B1A1918U, 0x1F1E1D1CU,
	0x3F800000U, 0x3DCCCCCDU, 0x3EA2F983U, 0x3F317218U,
	0x40490FDBU, 0x00000000U, 0x477FFF00U, 0x5C005BF8U,
	0x2E660000U, 0x34000000U, 0x38000000U, 0x3C000000U,
	0x40000000U, 0x44000000U, 0x48000000U, 0x42480000U
};

/* The 64-bit FAU slots of one page, and the words of one page. */
#define VA_PAGE_SLOTS		32U
#define VA_PAGE_WORDS		64U

/*
 * Appends one instruction to the list.
 *
 * A list that once ran out of memory takes nothing more, so the lowering
 * can append freely and check failed once.
 */
void
va_code_append(
	struct va_code *code,
	const struct va_inst *inst)
{
	struct va_inst *grown;
	uint32_t room;

	/* A list that already failed stays failed. */
	if (code->failed != 0)
		return;

	/* Grows the list when it is full. */
	if (code->count == code->room) {
		room = code->room + VA_CODE_GROWTH;
		grown = kern_malloc((size_t)room * sizeof(*grown));
		if (grown == NULL) {
			code->failed = ENOMEM;
			return;
		}

		/* Moves what the list holds to the larger storage. */
		if (code->count != 0U)
			kern_memcpy(grown, code->list, (size_t)code->count * sizeof(*grown));
		kern_free(code->list);
		code->list = grown;
		code->room = room;
	}

	/* Stores the instruction. */
	code->list[code->count] = *inst;
	code->count++;
}

/*
 * Releases the list's storage.
 */
void
va_code_release(
	struct va_code *code)
{
	/* Frees the instructions and leaves the list empty. */
	kern_free(code->list);
	code->list = NULL;
	code->count = 0U;
	code->room = 0U;
}

/*
 * Encodes a register source.
 *
 * The last-use bit (bit 6) is a hint the compiler leaves clear.
 */
uint8_t
va_source_register(
	uint32_t reg)
{
	/* Reports the register's number. */
	return (uint8_t)(reg & 0x3FU);
}

/*
 * Finds the entry of the constant table that holds a value.
 */
int
va_immediate_entry(
	uint32_t value)
{
	uint32_t entry;

	/* Looks through the table. */
	for (entry = 0U; entry < 32U; entry++) {
		if (va_immediates[entry] == value)
			return (int)entry;
	}

	/* The value is not in the table. */
	return -1;
}

/*
 * Reports the value of an entry of the constant table.
 */
uint32_t
va_immediate_value(
	uint32_t entry)
{
	/* Reports the entry's value. */
	return va_immediates[entry & 0x1FU];
}

/*
 * Encodes an entry of the constant table as a source.
 */
uint8_t
va_source_immediate(
	uint32_t entry)
{
	/* Reports the table's mode with the entry. */
	return (uint8_t)(VA_SOURCE_IMMEDIATE | (entry & 0x1FU));
}

/*
 * Encodes a FAU uniform word as a source: its 64-bit slot within its
 * page and which word of the slot.
 */
uint8_t
va_source_uniform(
	uint32_t word)
{
	uint32_t slot;

	/* Finds the word's slot within its page. */
	slot = (word % VA_PAGE_WORDS) / 2U;

	/* Reports the uniform mode, the slot and the word. */
	return (uint8_t)(VA_SOURCE_UNIFORM | (slot << 1) | (word & 1U));
}

/*
 * Reports the FAU page of a uniform word.
 */
uint32_t
va_uniform_page(
	uint32_t word)
{
	/* Each page holds 32 slots of two words. */
	return word / VA_PAGE_WORDS;
}

/*
 * Encodes one word of a special FAU value as a source.
 */
uint8_t
va_source_special(
	uint32_t number,
	uint32_t word)
{
	/* Reports the special mode, the value's number and the word. */
	return (uint8_t)(VA_SOURCE_SPECIAL | ((number & 0xFU) << 1) | (word & 1U));
}

/*
 * Moves a field's value into place in a word.
 */
uint64_t
va_field(
	uint64_t value,
	uint32_t shift)
{
	/* Reports the shifted value. */
	return value << shift;
}

/*
 * Encodes the destination of an arithmetic instruction: the register
 * and the write mask of both of its halves.
 */
uint64_t
va_dest(
	uint32_t reg)
{
	uint64_t byte;

	/* Combines the register with the full write mask. */
	byte = (uint64_t)((reg & 0x3FU) | VA_DEST_FULL);

	/* Reports the destination byte in place. */
	return byte << VA_SHIFT_DEST;
}

/*
 * Encodes a message's staging registers: the first register, how many,
 * and whether the message reads or writes them.
 */
uint64_t
va_staging(
	uint32_t reg,
	uint32_t count,
	uint32_t control)
{
	uint64_t bits;

	/* Places the register count. */
	bits = (uint64_t)(count & 7U) << VA_SHIFT_STAGING_COUNT;

	/* Places the first register and the read or write control. */
	bits |= (uint64_t)(reg & 0x3FU) << VA_SHIFT_DEST;
	bits |= (uint64_t)(control & 3U) << VA_SHIFT_STAGING_CONTROL;

	/* Reports the staging fields. */
	return bits;
}

/*
 * Reports the mask of count registers from reg.
 */
uint64_t
va_register_mask(
	uint32_t reg,
	uint32_t count)
{
	uint64_t mask;
	uint32_t index;

	/* Sets one bit per register that exists. */
	mask = 0U;
	for (index = 0U; index < count; index++) {
		if (reg + index < VA_REGISTERS)
			mask |= (uint64_t)1U << (reg + index);
	}

	/* Reports the registers. */
	return mask;
}
