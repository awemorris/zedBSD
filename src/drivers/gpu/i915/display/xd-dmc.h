/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The DMC firmware of the display 12 and 13 code (xd-dmc.c).
 *
 * The DMC is the display's own microcontroller: it saves and restores the
 * display around the DC states, and on display 13 each pipe has one more.
 * A firmware file is a package: a signature header, a list of entries
 * (which DMC and which display stepping each program is for), and the
 * programs, each with its own header naming where the program goes and
 * the registers to write after it.  The parse picks the entry of each DMC
 * for the stepping and copies the program into the owner's storage, so it
 * outlives the file; the load writes the programs and their registers
 * into the hardware with the DMC's event handlers off; the pipe DMC of a
 * pipe is switched on once the pipe runs and off before it stops.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_XD_DMC_H
#define DRIVERS_GPU_I915_DISPLAY_XD_DMC_H

#include <stddef.h>
#include <stdint.h>

#include "xd-io.h"
#include "xd-platform.h"

/* The DMCs: the main one, then the pipe DMCs of pipes A to D. */
#define I915_XD_DMC_MAIN		0U
#define I915_XD_DMC_PIPE(pipe)		(1U + (pipe))
#define I915_XD_DMC_COUNT		5U

/* The most registers a program's header may name. */
#define I915_XD_DMC_MAX_REGISTERS	20U

/*
 * Why a DMC's program was not taken: its entry's offset is past the
 * file, the file ends inside its header or program, its header version
 * or length is unknown, it names too many registers or one outside the
 * DMC's range, the program is larger than the platform takes, or the
 * owner's storage is full.
 */
#define I915_XD_DMC_TAKEN		0
#define I915_XD_DMC_BEYOND		1
#define I915_XD_DMC_TRUNCATED		2
#define I915_XD_DMC_HEADER_VERSION	3
#define I915_XD_DMC_HEADER_LENGTH	4
#define I915_XD_DMC_TOO_MANY		5
#define I915_XD_DMC_OUT_OF_RANGE	6
#define I915_XD_DMC_TOO_BIG		7
#define I915_XD_DMC_NO_ROOM		8

/*
 * Why a file was refused as a whole: none given, its signature header's
 * length, its package header's version or length.
 */
#define I915_XD_DMC_FILE_OK		0
#define I915_XD_DMC_FILE_NONE		1
#define I915_XD_DMC_FILE_CSS_LENGTH	2
#define I915_XD_DMC_FILE_PACKAGE_VERSION	3
#define I915_XD_DMC_FILE_PACKAGE_LENGTH	4

/*
 * One DMC's program as the parse found it: whether the package had an
 * entry for the stepping and where its header starts (dwords after the
 * package), why it was not taken, the header version, where the program
 * goes and how many dwords it has, the copy in the owner's storage (NULL
 * when not taken), and the registers its header writes after it.
 */
struct i915_xd_dmc_program {
	int chosen;
	uint32_t offset;
	int refused;
	unsigned header_version;
	uint32_t start;
	uint32_t dwords;
	const uint8_t *payload;
	uint32_t registers;
	uint32_t address[I915_XD_DMC_MAX_REGISTERS];
	uint32_t value[I915_XD_DMC_MAX_REGISTERS];
};

/*
 * The owner's storage the programs are copied into: the bytes, their
 * size and how many are used.
 */
struct i915_xd_dmc_store {
	uint8_t *bytes;
	size_t room;
	size_t used;
};

/*
 * A parsed DMC firmware and what its loads did: the firmware version
 * (major in the upper half), why the file was refused and the length or
 * version found, the package version and entry count, each DMC's
 * program, and the diagnostics of the last load (the program and
 * register writes, the event handler writes, two order-sensitive sums of
 * what was written, and whether the whole load ran).
 */
struct i915_xd_dmc {
	uint32_t version;
	int file_refused;
	uint32_t file_found;
	unsigned package_version;
	unsigned entries;
	struct i915_xd_dmc_program program[I915_XD_DMC_COUNT];
	unsigned program_writes;
	unsigned register_writes;
	unsigned handler_writes;
	uint64_t program_sum;
	uint64_t register_sum;
	int loaded;
};

void drv_i915_xd_dmc_init(struct i915_xd_dmc *dmc);
int drv_i915_xd_dmc_parse(struct i915_xd_dmc *dmc, const struct i915_xd_platform *platform, uint8_t step, const uint8_t *data, size_t size, struct i915_xd_dmc_store *store);
int drv_i915_xd_dmc_has_main(const struct i915_xd_dmc *dmc);
int drv_i915_xd_dmc_has(const struct i915_xd_dmc *dmc, unsigned id);
void drv_i915_xd_dmc_load(struct i915_xd_dmc *dmc, struct i915_xd_io *io, const struct i915_xd_platform *platform);
void drv_i915_xd_dmc_pipe(const struct i915_xd_dmc *dmc, struct i915_xd_io *io, unsigned pipe, int on);
void drv_i915_xd_dmc_pipe_switch(struct i915_xd_io *io, unsigned pipe, int on);

#endif /* DRIVERS_GPU_I915_DISPLAY_XD_DMC_H */
