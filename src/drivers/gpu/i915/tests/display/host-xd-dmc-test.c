/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the xd DMC firmware (display/xd-dmc.c) on the register
 * model (xd-fake-regs.c), with firmware packages made up here: the choice
 * of each DMC's entry for the stepping, the programs copied into the
 * owner's storage, the refusals of a file and of a program, the exact
 * writes of a load on Alder Lake-P and the handlers Tiger Lake turns off,
 * and the pipe DMC switch.
 */

#include "host-test.h"
#include "xd-fake-regs.h"

#include "../../display/xd-dmc.h"
#include "../../display/xd-io.h"
#include "../../display/xd-platform.h"
#include "../../intel/display-regs-xd.h"

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The GPUs of the Latitude 5330 (Alder Lake-P, D0) and of a Tiger Lake laptop, with their revisions. */
#define I915_XD_DMC_TEST_ADLP		0x46a6U
#define I915_XD_DMC_TEST_ADLP_REVID	0x0cU
#define I915_XD_DMC_TEST_TGL		0x9a49U
#define I915_XD_DMC_TEST_TGL_REVID	0x01U

/* The size of a made-up package and of the owner's storage. */
#define I915_XD_DMC_TEST_FILE		8192U
#define I915_XD_DMC_TEST_STORE		4096U

/* The sizes of the headers of a version 2 package with version 3 programs. */
#define I915_XD_DMC_TEST_CSS		128U
#define I915_XD_DMC_TEST_PACKAGE	(16U + 32U * 12U)
#define I915_XD_DMC_TEST_HEADER		256U

/* The most writes one check reads back. */
#define I915_XD_DMC_TEST_WRITES		128U

/*
 * One program of a made-up package: the DMC and stepping of its entry,
 * where it goes, how many dwords it has, and its registers.
 */
struct i915_xd_dmc_test_program {
	uint8_t id;
	char stepping;
	char substepping;
	uint32_t start;
	uint32_t dwords;
	uint32_t registers;
	uint32_t address[4];
	uint32_t value[4];
};

/* The window the checks share, reaching a fresh model each time. */
static struct i915_xd_io i915_xd_dmc_test_io;

/* A made-up package and the owner's storage. */
static uint8_t i915_xd_dmc_test_file[I915_XD_DMC_TEST_FILE];
static uint8_t i915_xd_dmc_test_store[I915_XD_DMC_TEST_STORE];

static const struct i915_xd_platform *i915_xd_dmc_test_setup(uint16_t device, uint8_t revid, uint8_t *step);
static void i915_xd_dmc_test_put32(uint8_t *bytes, size_t offset, uint32_t value);
static size_t i915_xd_dmc_test_package(const struct i915_xd_dmc_test_program *programs, unsigned count);
static int i915_xd_dmc_test_parse(struct i915_xd_dmc *dmc, const struct i915_xd_platform *platform, uint8_t step, size_t size);
static unsigned i915_xd_dmc_test_writes(uint32_t *offsets, uint32_t *values, unsigned max);
static int i915_xd_dmc_test_find(const uint32_t *offsets, const uint32_t *values, unsigned count, uint32_t offset, uint32_t *value);
static void i915_xd_dmc_test_choice(void);
static void i915_xd_dmc_test_refusals(void);
static void i915_xd_dmc_test_load_adlp(void);
static void i915_xd_dmc_test_load_tgl(void);
static void i915_xd_dmc_test_pipe(void);

/*
 * Runs every check of the xd DMC firmware and reports.
 */
int
main(void)
{
	int failures;

	/* Runs each group of checks. */
	i915_xd_dmc_test_choice();
	i915_xd_dmc_test_refusals();
	i915_xd_dmc_test_load_adlp();
	i915_xd_dmc_test_load_tgl();
	i915_xd_dmc_test_pipe();

	/* Reports the result. */
	failures = i915_host_report("host_xd_dmc_test");
	if (failures != 0)
		return 1;

	/* Succeeded: every check passed. */
	return 0;
}

/* Starts a fresh model and returns the description of a device, with its stepping. */
static const struct i915_xd_platform *
i915_xd_dmc_test_setup(
	uint16_t device,
	uint8_t revid,
	uint8_t *step)
{
	struct i915_xd_io_hooks hooks;
	struct i915_xd_choice choice;
	int error;

	/* A fresh model and a window that reaches it. */
	i915_xd_fake_reset();
	i915_xd_fake_hooks(&hooks);
	drv_i915_xd_io_init(&i915_xd_dmc_test_io, NULL, NULL);
	drv_i915_xd_io_set_hooks(&i915_xd_dmc_test_io, &hooks);

	/* The description and the stepping. */
	error = drv_i915_xd_platform_choose(device, revid, 0, 0U, &choice);
	if (error != 0)
		return NULL;
	*step = choice.step;

	/* Succeeded: the description. */
	return choice.platform;
}

/* Stores a little-endian dword. */
static void
i915_xd_dmc_test_put32(
	uint8_t *bytes,
	size_t offset,
	uint32_t value)
{
	/* The lowest byte first. */
	bytes[offset] = (uint8_t)value;
	bytes[offset + 1U] = (uint8_t)(value >> 8);
	bytes[offset + 2U] = (uint8_t)(value >> 16);
	bytes[offset + 3U] = (uint8_t)(value >> 24);
}

/*
 * Makes a version 2 package of version 3 programs: the signature header
 * (version 2.20), the entries in the order given, and each program (its
 * dword n is 0x1000 * (entry + 1) + n).  Returns the file's size.
 */
static size_t
i915_xd_dmc_test_package(
	const struct i915_xd_dmc_test_program *programs,
	unsigned count)
{
	uint8_t *file;
	uint8_t *entry;
	uint8_t *header;
	size_t at;
	unsigned index;
	unsigned word;
	unsigned reg;

	/* An empty file and its signature header. */
	file = i915_xd_dmc_test_file;
	memset(file, 0, I915_XD_DMC_TEST_FILE);
	i915_xd_dmc_test_put32(file, 4U, I915_XD_DMC_TEST_CSS / 4U);
	i915_xd_dmc_test_put32(file, 88U, 0x00020014U);

	/* The package header. */
	file[I915_XD_DMC_TEST_CSS] = (uint8_t)(I915_XD_DMC_TEST_PACKAGE / 4U);
	file[I915_XD_DMC_TEST_CSS + 1U] = 2U;
	i915_xd_dmc_test_put32(file, I915_XD_DMC_TEST_CSS + 12U, count);

	/* Each entry and its program after the headers. */
	at = 0U;
	for (index = 0U; index < count; index++) {
		/* The entry: its DMC, stepping and offset in dwords. */
		entry = file + I915_XD_DMC_TEST_CSS + 16U + 12U * index;
		entry[1] = programs[index].id;
		entry[2] = (uint8_t)programs[index].stepping;
		entry[3] = (uint8_t)programs[index].substepping;
		i915_xd_dmc_test_put32(entry, 4U, (uint32_t)(at / 4U));

		/* The program's header. */
		header = file + I915_XD_DMC_TEST_CSS + I915_XD_DMC_TEST_PACKAGE + at;
		i915_xd_dmc_test_put32(header, 0U, 0x40403e3eU);
		header[4] = (uint8_t)(I915_XD_DMC_TEST_HEADER / 4U);
		header[5] = 3U;
		i915_xd_dmc_test_put32(header, 12U, programs[index].dwords);
		i915_xd_dmc_test_put32(header, 20U, programs[index].start);
		i915_xd_dmc_test_put32(header, 92U, programs[index].registers);
		for (reg = 0U; reg < programs[index].registers; reg++) {
			i915_xd_dmc_test_put32(header, 96U + 4U * reg, programs[index].address[reg]);
			i915_xd_dmc_test_put32(header, 176U + 4U * reg, programs[index].value[reg]);
		}

		/* The program's dwords. */
		for (word = 0U; word < programs[index].dwords; word++)
			i915_xd_dmc_test_put32(header, I915_XD_DMC_TEST_HEADER + 4U * word, 0x1000U * (index + 1U) + word);
		at += I915_XD_DMC_TEST_HEADER + 4U * programs[index].dwords;
	}

	/* Succeeded: the file's size. */
	return I915_XD_DMC_TEST_CSS + I915_XD_DMC_TEST_PACKAGE + at;
}

/* Parses the made-up package into a fresh storage. */
static int
i915_xd_dmc_test_parse(
	struct i915_xd_dmc *dmc,
	const struct i915_xd_platform *platform,
	uint8_t step,
	size_t size)
{
	struct i915_xd_dmc_store store;
	int error;

	/* A storage with room for the made-up programs. */
	store.bytes = i915_xd_dmc_test_store;
	store.room = sizeof(i915_xd_dmc_test_store);
	store.used = 0U;

	/* Parses the made-up file into the storage. */
	error = drv_i915_xd_dmc_parse(dmc, platform, step, i915_xd_dmc_test_file, size, &store);

	/* Succeeded: reports what the parse said. */
	return error;
}

/* Copies the writes the window recorded, oldest first; reports how many. */
static unsigned
i915_xd_dmc_test_writes(
	uint32_t *offsets,
	uint32_t *values,
	unsigned max)
{
	struct i915_xd_io_record records[I915_XD_IO_RECORDS];
	unsigned count;
	unsigned index;
	unsigned kept;

	/* Takes the record and keeps the writes. */
	count = drv_i915_xd_io_records(&i915_xd_dmc_test_io, records, I915_XD_IO_RECORDS);
	kept = 0U;
	for (index = 0U; index < count; index++) {
		/* Only writes are kept. */
		if (records[index].kind != I915_XD_IO_RECORD_WRITE)
			continue;
		if (kept >= max)
			break;

		/* Keeps the write. */
		offsets[kept] = records[index].offset;
		values[kept] = records[index].value;
		kept++;
	}

	/* Succeeded: the writes copied. */
	return kept;
}

/* Finds the last write to a register; reports 1 with its value, or 0. */
static int
i915_xd_dmc_test_find(
	const uint32_t *offsets,
	const uint32_t *values,
	unsigned count,
	uint32_t offset,
	uint32_t *value)
{
	int found;
	unsigned index;

	/* Walks every write, keeping the last to the register. */
	found = 0;
	for (index = 0U; index < count; index++) {
		if (offsets[index] != offset)
			continue;

		/* The register's last value. */
		*value = values[index];
		found = 1;
	}

	/* Succeeded: whether the register was written. */
	return found;
}

/* Checks the choice of each DMC's entry and the copies of the programs. */
static void
i915_xd_dmc_test_choice(void)
{
	static struct i915_xd_dmc dmc;
	struct i915_xd_dmc_test_program programs[4];
	const struct i915_xd_platform *platform;
	size_t size;
	uint8_t step;
	int error;

	/* A wildcard main program, pipe A for C0 then for D0 then for any, pipe B for D of any substepping. */
	memset(programs, 0, sizeof(programs));
	programs[0].id = 0U;
	programs[0].stepping = '*';
	programs[0].substepping = '*';
	programs[0].start = 0x80000U;
	programs[0].dwords = 8U;
	programs[1].id = 1U;
	programs[1].stepping = 'C';
	programs[1].substepping = '0';
	programs[1].start = 0x90000U;
	programs[1].dwords = 4U;
	programs[2].id = 1U;
	programs[2].stepping = 'D';
	programs[2].substepping = '0';
	programs[2].start = 0x90000U;
	programs[2].dwords = 4U;
	programs[3].id = 2U;
	programs[3].stepping = 'D';
	programs[3].substepping = '*';
	programs[3].start = 0x98000U;
	programs[3].dwords = 2U;

	/* The 5330 is D0. */
	platform = i915_xd_dmc_test_setup(I915_XD_DMC_TEST_ADLP, I915_XD_DMC_TEST_ADLP_REVID, &step);
	if (platform == NULL)
		return;
	size = i915_xd_dmc_test_package(programs, 4U);
	error = i915_xd_dmc_test_parse(&dmc, platform, step, size);
	i915_host_check(step == 0x30U, "dmc: the 5330 is stepping D0");
	i915_host_check(error == 0 && dmc.version == 0x00020014U && dmc.package_version == 2U && dmc.entries == 4U,
			"dmc: the package parses with its version and entries");
	i915_host_check(dmc.program[0].payload != NULL && dmc.program[0].dwords == 8U && dmc.program[0].start == 0x80000U,
			"dmc: the wildcard main program is taken");
	i915_host_check(dmc.program[1].payload != NULL && dmc.program[1].payload[0] == 0x00U && dmc.program[1].payload[1] == 0x30U,
			"dmc: pipe A takes the D0 entry, not the C0 one");
	i915_host_check(dmc.program[2].payload != NULL && dmc.program[2].dwords == 2U,
			"dmc: pipe B takes the entry for any substepping of D");
	i915_host_check(dmc.program[3].chosen == 0 && drv_i915_xd_dmc_has(&dmc, 3U) == 0,
			"dmc: pipe C has no program");
	i915_host_check(dmc.program[0].payload == i915_xd_dmc_test_store && dmc.program[1].payload == i915_xd_dmc_test_store + 32U,
			"dmc: the programs are copied into the owner's storage in order");
}

/* Checks the refusals of a file and of a program. */
static void
i915_xd_dmc_test_refusals(void)
{
	static struct i915_xd_dmc dmc;
	struct i915_xd_dmc_test_program programs[2];
	struct i915_xd_dmc_store store;
	const struct i915_xd_platform *platform;
	size_t size;
	uint8_t step;
	int error;

	/* A main program and a pipe A program. */
	memset(programs, 0, sizeof(programs));
	programs[0].id = 0U;
	programs[0].stepping = '*';
	programs[0].substepping = '*';
	programs[0].start = 0x80000U;
	programs[0].dwords = 4U;
	programs[0].registers = 1U;
	programs[0].address[0] = 0x8f034U;
	programs[0].value[0] = 0xc0033200U;
	programs[1].id = 1U;
	programs[1].stepping = '*';
	programs[1].substepping = '*';
	programs[1].start = 0x90000U;
	programs[1].dwords = 4U;
	platform = i915_xd_dmc_test_setup(I915_XD_DMC_TEST_ADLP, I915_XD_DMC_TEST_ADLP_REVID, &step);
	if (platform == NULL)
		return;

	/* No file. */
	error = drv_i915_xd_dmc_parse(&dmc, platform, step, NULL, 0U, &store);
	i915_host_check(error == EINVAL && dmc.file_refused == I915_XD_DMC_FILE_NONE, "dmc: no file is refused");

	/* A signature header of the wrong length. */
	size = i915_xd_dmc_test_package(programs, 2U);
	i915_xd_dmc_test_put32(i915_xd_dmc_test_file, 4U, 31U);
	error = i915_xd_dmc_test_parse(&dmc, platform, step, size);
	i915_host_check(error == EINVAL && dmc.file_refused == I915_XD_DMC_FILE_CSS_LENGTH && dmc.file_found == 124U,
			"dmc: a signature header of the wrong length is refused");

	/* An unknown package version. */
	size = i915_xd_dmc_test_package(programs, 2U);
	i915_xd_dmc_test_file[I915_XD_DMC_TEST_CSS + 1U] = 3U;
	error = i915_xd_dmc_test_parse(&dmc, platform, step, size);
	i915_host_check(error == EINVAL && dmc.file_refused == I915_XD_DMC_FILE_PACKAGE_VERSION, "dmc: an unknown package version is refused");

	/* A package header whose length does not cover its entries. */
	size = i915_xd_dmc_test_package(programs, 2U);
	i915_xd_dmc_test_file[I915_XD_DMC_TEST_CSS] = 99U;
	error = i915_xd_dmc_test_parse(&dmc, platform, step, size);
	i915_host_check(error == EINVAL && dmc.file_refused == I915_XD_DMC_FILE_PACKAGE_LENGTH, "dmc: a short package header is refused");

	/* A main program register outside the main DMC's block: no main program. */
	programs[0].address[0] = 0x5f034U;
	size = i915_xd_dmc_test_package(programs, 2U);
	error = i915_xd_dmc_test_parse(&dmc, platform, step, size);
	i915_host_check(error == ENOENT && dmc.program[0].refused == I915_XD_DMC_OUT_OF_RANGE && dmc.program[1].payload != NULL,
			"dmc: a main register outside its block leaves no main program");
	programs[0].address[0] = 0x8f034U;

	/* A pipe program register in the pipe DMCs' block of display 13 is taken. */
	programs[1].registers = 1U;
	programs[1].address[0] = 0x5f034U;
	size = i915_xd_dmc_test_package(programs, 2U);
	error = i915_xd_dmc_test_parse(&dmc, platform, step, size);
	i915_host_check(error == 0 && dmc.program[1].payload != NULL, "dmc: a pipe register in the pipe DMCs' block is taken");
	programs[1].registers = 0U;

	/* A file that ends inside a program. */
	size = i915_xd_dmc_test_package(programs, 2U);
	error = i915_xd_dmc_test_parse(&dmc, platform, step, size - 4U);
	i915_host_check(error == 0 && dmc.program[1].refused == I915_XD_DMC_TRUNCATED, "dmc: a program cut short is refused");

	/* An entry whose offset is past the file. */
	size = i915_xd_dmc_test_package(programs, 2U);
	i915_xd_dmc_test_put32(i915_xd_dmc_test_file, I915_XD_DMC_TEST_CSS + 16U + 12U + 4U, 0x10000U);
	error = i915_xd_dmc_test_parse(&dmc, platform, step, size);
	i915_host_check(error == 0 && dmc.program[1].refused == I915_XD_DMC_BEYOND, "dmc: an entry past the file is refused");

	/* An unknown program header version. */
	size = i915_xd_dmc_test_package(programs, 2U);
	i915_xd_dmc_test_file[I915_XD_DMC_TEST_CSS + I915_XD_DMC_TEST_PACKAGE + 5U] = 2U;
	error = i915_xd_dmc_test_parse(&dmc, platform, step, size);
	i915_host_check(error == ENOENT && dmc.program[0].refused == I915_XD_DMC_HEADER_VERSION, "dmc: an unknown header version is refused");

	/* Too many registers. */
	size = i915_xd_dmc_test_package(programs, 2U);
	i915_xd_dmc_test_put32(i915_xd_dmc_test_file, I915_XD_DMC_TEST_CSS + I915_XD_DMC_TEST_PACKAGE + 92U, 21U);
	error = i915_xd_dmc_test_parse(&dmc, platform, step, size);
	i915_host_check(error == ENOENT && dmc.program[0].refused == I915_XD_DMC_TOO_MANY, "dmc: more than 20 registers are refused");

	/* A storage without room. */
	size = i915_xd_dmc_test_package(programs, 2U);
	store.bytes = i915_xd_dmc_test_store;
	store.room = 20U;
	store.used = 0U;
	error = drv_i915_xd_dmc_parse(&dmc, platform, step, i915_xd_dmc_test_file, size, &store);
	i915_host_check(error == 0 && dmc.program[0].payload != NULL && dmc.program[1].refused == I915_XD_DMC_NO_ROOM && store.used == 16U,
			"dmc: a program without room in the storage is refused");
}

/* Checks the writes of a load on Alder Lake-P. */
static void
i915_xd_dmc_test_load_adlp(void)
{
	static struct i915_xd_dmc dmc;
	struct i915_xd_dmc_test_program programs[2];
	uint32_t offsets[I915_XD_DMC_TEST_WRITES];
	uint32_t values[I915_XD_DMC_TEST_WRITES];
	const struct i915_xd_platform *platform;
	unsigned count;
	uint32_t value;
	size_t size;
	uint8_t step;
	int found;

	/* A main program with a handler and a register, a pipe A program with a handler. */
	memset(programs, 0, sizeof(programs));
	programs[0].id = 0U;
	programs[0].stepping = '*';
	programs[0].substepping = '*';
	programs[0].start = 0x80000U;
	programs[0].dwords = 2U;
	programs[0].registers = 2U;
	programs[0].address[0] = 0x8f034U;
	programs[0].value[0] = 0xc0033200U;
	programs[0].address[1] = 0x8f100U;
	programs[0].value[1] = 0x12345678U;
	programs[1].id = 1U;
	programs[1].stepping = '*';
	programs[1].substepping = '*';
	programs[1].start = 0x90000U;
	programs[1].dwords = 1U;
	programs[1].registers = 1U;
	programs[1].address[0] = 0x5f038U;
	programs[1].value[0] = 0xc003df00U;
	platform = i915_xd_dmc_test_setup(I915_XD_DMC_TEST_ADLP, I915_XD_DMC_TEST_ADLP_REVID, &step);
	if (platform == NULL)
		return;
	size = i915_xd_dmc_test_package(programs, 2U);
	(void)i915_xd_dmc_test_parse(&dmc, platform, step, size);

	/* The load. */
	i915_xd_fake_set(I915_XD_CLKGATE_DIS_PSL_EXT(2U), 0x4U);
	drv_i915_xd_dmc_load(&dmc, &i915_xd_dmc_test_io, platform);
	count = i915_xd_dmc_test_writes(offsets, values, I915_XD_DMC_TEST_WRITES);

	/* The pipe DMCs' clock gating goes off for every pipe first, keeping the other bits. */
	i915_host_check(count == 4U + 32U + 3U + 3U + 1U + 2U &&
			offsets[0] == I915_XD_CLKGATE_DIS_PSL_EXT(0U) && values[0] == I915_XD_CLKGATE_PSL_EXT_PIPEDMC &&
			offsets[2] == I915_XD_CLKGATE_DIS_PSL_EXT(2U) && values[2] == (0x4U | I915_XD_CLKGATE_PSL_EXT_PIPEDMC) &&
			offsets[3] == I915_XD_CLKGATE_DIS_PSL_EXT(3U),
			"dmc: the load starts with the pipe DMC clock gating off on the four pipes");

	/* Every handler of the main DMC and of pipe A's goes off, the control then the address. */
	i915_host_check(offsets[4] == 0x8f034U && values[4] == 0x00030100U && offsets[5] == 0x8f004U && values[5] == 0U &&
			offsets[19] == 0x8f020U && offsets[20] == 0x5f034U && values[20] == 0x00030100U && offsets[35] == 0x5f020U,
			"dmc: the main and pipe A handlers go off before the programs");

	/* The programs, dword by dword at their places. */
	i915_host_check(offsets[36] == 0x80000U && values[36] == 0x1000U && offsets[37] == 0x80004U && values[37] == 0x1001U &&
			offsets[38] == 0x90000U && values[38] == 0x2000U && dmc.program_writes == 3U,
			"dmc: the programs are written at their places");

	/* The registers: the main handler as the header says, pipe A's handler off. */
	i915_host_check(offsets[39] == 0x8f034U && values[39] == 0xc0033200U && offsets[40] == 0x8f100U && values[40] == 0x12345678U &&
			offsets[41] == 0x5f038U && values[41] == 0x00030100U && dmc.register_writes == 3U,
			"dmc: the main registers are written as given and a pipe handler as off");

	/* The DC state debug masks, then the clock gating of pipes C and D back. */
	i915_host_check(offsets[42] == I915_XD_DC_STATE_DEBUG && values[42] == 0x3U &&
			offsets[43] == I915_XD_CLKGATE_DIS_PSL_EXT(2U) && values[43] == 0x4U &&
			offsets[44] == I915_XD_CLKGATE_DIS_PSL_EXT(3U) && values[44] == 0U && dmc.loaded == 1,
			"dmc: the debug masks are set and pipes C and D get their clock gating back");

	/* Pipes A and B keep the gating off. */
	found = i915_xd_dmc_test_find(offsets, values, count, I915_XD_CLKGATE_DIS_PSL_EXT(1U), &value);
	i915_host_check(found == 1 && value == I915_XD_CLKGATE_PSL_EXT_PIPEDMC, "dmc: pipe B keeps its gating off");

	/* Without a main program nothing is written. */
	platform = i915_xd_dmc_test_setup(I915_XD_DMC_TEST_ADLP, I915_XD_DMC_TEST_ADLP_REVID, &step);
	drv_i915_xd_dmc_init(&dmc);
	drv_i915_xd_dmc_load(&dmc, &i915_xd_dmc_test_io, platform);
	count = i915_xd_dmc_test_writes(offsets, values, I915_XD_DMC_TEST_WRITES);
	i915_host_check(count == 0U && dmc.loaded == 0, "dmc: nothing is loaded without a main program");
}

/* Checks the handlers Tiger Lake turns off, its pipe DMC block, and no clock gating step. */
static void
i915_xd_dmc_test_load_tgl(void)
{
	static struct i915_xd_dmc dmc;
	struct i915_xd_dmc_test_program programs[2];
	uint32_t offsets[I915_XD_DMC_TEST_WRITES];
	uint32_t values[I915_XD_DMC_TEST_WRITES];
	const struct i915_xd_platform *platform;
	unsigned count;
	uint32_t value;
	size_t size;
	uint8_t step;
	int found;
	int error;

	/* A main program with the millisecond clock, pipe A vblank and another handler, and a pipe A program. */
	memset(programs, 0, sizeof(programs));
	programs[0].id = 0U;
	programs[0].stepping = '*';
	programs[0].substepping = '*';
	programs[0].start = 0x80000U;
	programs[0].dwords = 1U;
	programs[0].registers = 3U;
	programs[0].address[0] = 0x8f03cU;
	programs[0].value[0] = 0xc0033200U;
	programs[0].address[1] = 0x8f040U;
	programs[0].value[1] = 0xc003bf00U;
	programs[0].address[2] = 0x8f044U;
	programs[0].value[2] = 0xc0034500U;
	programs[1].id = 1U;
	programs[1].stepping = '*';
	programs[1].substepping = '*';
	programs[1].start = 0x90000U;
	programs[1].dwords = 1U;
	programs[1].registers = 1U;
	programs[1].address[0] = 0x92034U;
	programs[1].value[0] = 0xc003df00U;
	platform = i915_xd_dmc_test_setup(I915_XD_DMC_TEST_TGL, I915_XD_DMC_TEST_TGL_REVID, &step);
	if (platform == NULL)
		return;
	size = i915_xd_dmc_test_package(programs, 2U);
	error = i915_xd_dmc_test_parse(&dmc, platform, step, size);
	i915_host_check(error == 0 && dmc.program[1].payload != NULL, "dmc: a pipe A register in Tiger Lake's pipe A block is taken");

	/* The load. */
	drv_i915_xd_dmc_load(&dmc, &i915_xd_dmc_test_io, platform);
	count = i915_xd_dmc_test_writes(offsets, values, I915_XD_DMC_TEST_WRITES);

	/* No clock gating step; the handlers first, pipe A's in its own block. */
	i915_host_check(count == 32U + 2U + 4U + 1U && offsets[0] == 0x8f034U && offsets[16] == 0x92034U && offsets[31] == 0x92020U,
			"dmc: Tiger Lake has no clock gating step and its pipe A handlers are at 0x92000");

	/* The main millisecond clock and pipe A vblank handlers go off, the other stays. */
	found = i915_xd_dmc_test_find(offsets + 32, values + 32, count - 32U, 0x8f03cU, &value);
	i915_host_check(found == 1 && value == 0x00030100U, "dmc: Tiger Lake turns the main pipe A vblank handler off");
	found = i915_xd_dmc_test_find(offsets + 32, values + 32, count - 32U, 0x8f040U, &value);
	i915_host_check(found == 1 && value == 0x00030100U, "dmc: Tiger Lake turns the main millisecond clock handler off");
	found = i915_xd_dmc_test_find(offsets + 32, values + 32, count - 32U, 0x8f044U, &value);
	i915_host_check(found == 1 && value == 0xc0034500U, "dmc: another main handler is written as given");
	found = i915_xd_dmc_test_find(offsets + 32, values + 32, count - 32U, 0x92034U, &value);
	i915_host_check(found == 1 && value == 0x00030100U, "dmc: Tiger Lake's pipe A handler is written as off");
}

/* Checks the pipe DMC switch. */
static void
i915_xd_dmc_test_pipe(void)
{
	static struct i915_xd_dmc dmc;
	struct i915_xd_dmc_test_program programs[2];
	uint32_t offsets[8];
	uint32_t values[8];
	const struct i915_xd_platform *platform;
	unsigned count;
	size_t size;
	uint8_t step;

	/* A main program and a pipe A program. */
	memset(programs, 0, sizeof(programs));
	programs[0].id = 0U;
	programs[0].stepping = '*';
	programs[0].substepping = '*';
	programs[0].start = 0x80000U;
	programs[0].dwords = 1U;
	programs[1].id = 1U;
	programs[1].stepping = '*';
	programs[1].substepping = '*';
	programs[1].start = 0x90000U;
	programs[1].dwords = 1U;
	platform = i915_xd_dmc_test_setup(I915_XD_DMC_TEST_ADLP, I915_XD_DMC_TEST_ADLP_REVID, &step);
	if (platform == NULL)
		return;
	size = i915_xd_dmc_test_package(programs, 2U);
	(void)i915_xd_dmc_test_parse(&dmc, platform, step, size);

	/* Pipe A's DMC goes on and off; pipe B has none and is not written. */
	i915_xd_fake_set(I915_XD_PIPEDMC_CONTROL(0U), 0x10U);
	drv_i915_xd_dmc_pipe(&dmc, &i915_xd_dmc_test_io, 0U, 1);
	drv_i915_xd_dmc_pipe(&dmc, &i915_xd_dmc_test_io, 1U, 1);
	drv_i915_xd_dmc_pipe(&dmc, &i915_xd_dmc_test_io, 0U, 0);
	count = i915_xd_dmc_test_writes(offsets, values, 8U);
	i915_host_check(count == 2U && offsets[0] == I915_XD_PIPEDMC_CONTROL(0U) && values[0] == 0x11U &&
			offsets[1] == I915_XD_PIPEDMC_CONTROL(0U) && values[1] == 0x10U,
			"dmc: pipe A's DMC is switched on and off, pipe B without a program is not");
}
