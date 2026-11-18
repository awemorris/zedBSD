/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The runner of the video BIOS's command routines (bytecode.c).
 *
 * A routine is bytecode in the image: it reads and writes the GPU's
 * registers, its parameters, a workspace of its own, a scratch area in
 * memory and the image's data tables, compares, branches, waits and calls
 * other routines.  The runner reaches the hardware only through the
 * caller's access functions, so the same runner sets the display up on the
 * board and records what it would write in a dry run on the host.
 */

#ifndef DRIVERS_GPU_AMDRDNA_AMDRDNA_BYTECODE_H
#define DRIVERS_GPU_AMDRDNA_AMDRDNA_BYTECODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The places of the image's small register programs (the indirect
 * register access a routine can select instead of the plain one), by
 * their 8-bit number.
 */
#define AMDRDNA_BYTECODE_IO_PROGRAMS	256U

/*
 * How a run reaches the hardware: a register's read and write by its
 * dword index (index 0 is the indirect window's address, which a routine
 * writes in dwords and the hardware takes in bytes), and a wait.  owner is
 * passed back to each.
 */
struct amdrdna_bytecode_access {
	void *owner;
	uint32_t (*read)(void *owner, uint32_t index);
	void (*write)(void *owner, uint32_t index, uint32_t value);
	void (*wait)(void *owner, uint32_t microseconds);
};

/*
 * A runner over one image.
 *
 * image and length are the image, which stays the caller's; command_list
 * and data_list are where its lists of routines and of data tables lie;
 * io_programs holds where each register program starts (0 for none).
 * scratch is the caller's scratch area of scratch_bytes bytes (NULL for
 * none).  max_steps bounds the instructions of one run.
 *
 * The state a run leaves for the routines it calls: the data table and
 * the register block the operands are relative to, the scratch window, the
 * product and quotient words, the shift and its masks, the register access
 * kind (plain, PCI or system, or a register program) and its attribute
 * word, and the flags of the last comparison.
 *
 * The counts of the last run: its instructions, its register reads and
 * writes, the waits in microseconds, the deepest call, and the
 * instructions the runner does not have (which do nothing); failure names
 * why a run stopped and failed_at where (an image offset).
 */
struct amdrdna_bytecode {
	const uint8_t *image;
	uint32_t length;
	uint16_t command_list;
	uint16_t data_list;
	uint16_t io_programs[AMDRDNA_BYTECODE_IO_PROGRAMS];
	uint32_t *scratch;
	uint32_t scratch_bytes;
	struct amdrdna_bytecode_access access;
	uint64_t max_steps;

	uint16_t data_block;
	uint16_t register_block;
	uint32_t scratch_window;
	uint32_t product_low;
	uint32_t product_high;
	uint8_t shift;
	uint16_t io_attributes;
	uint32_t io_mode;
	bool equal;
	bool above;

	uint64_t steps;
	uint32_t reads;
	uint32_t writes;
	uint64_t waited_us;
	unsigned depth;
	unsigned deepest;
	uint32_t missing;
	const char *failure;
	uint32_t failed_at;
};

int
amdrdna_bytecode_open(
	struct amdrdna_bytecode *runner,
	const uint8_t *image,
	uint32_t length,
	const struct amdrdna_bytecode_access *access,
	uint32_t *scratch,
	uint32_t scratch_bytes);

int
amdrdna_bytecode_run(
	struct amdrdna_bytecode *runner,
	unsigned routine,
	uint32_t *parameters,
	uint32_t parameter_words);

bool
amdrdna_bytecode_has(
	const struct amdrdna_bytecode *runner,
	unsigned routine);

#endif
