/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The DMC firmware of the display 12 and 13 code (see xd-dmc.h).
 */

#include "xd-dmc.h"
#include "xd-io.h"
#include "xd-platform.h"

#include "../intel/display-regs-xd.h"

#include <kern/sched.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Where the fields of a firmware file are, in bytes: the signature
 * header (its length in dwords, the firmware version, its own size), the
 * package header (its length in dwords, its version, the entry count,
 * its size before the entries) and one entry (its DMC, stepping,
 * substepping and program offset in dwords, its size), and each
 * program's header (shared part: the header length, the header version,
 * the program size in dwords; version 3: where the program goes, the
 * register count, addresses and values, its size; version 1: the
 * register count, addresses and values, its size).
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dmc.c (struct intel_css_header,
 * struct intel_package_header, struct intel_fw_info,
 * struct intel_dmc_header_base, struct intel_dmc_header_v1,
 * struct intel_dmc_header_v3).
 */
#define I915_XD_DMC_CSS_LENGTH		4U
#define I915_XD_DMC_CSS_VERSION		88U
#define I915_XD_DMC_CSS_SIZE		128U
#define I915_XD_DMC_PACKAGE_LENGTH	0U
#define I915_XD_DMC_PACKAGE_VERSION	1U
#define I915_XD_DMC_PACKAGE_ENTRIES	12U
#define I915_XD_DMC_PACKAGE_SIZE	16U
#define I915_XD_DMC_ENTRY_ID		1U
#define I915_XD_DMC_ENTRY_STEPPING	2U
#define I915_XD_DMC_ENTRY_SUBSTEPPING	3U
#define I915_XD_DMC_ENTRY_OFFSET	4U
#define I915_XD_DMC_ENTRY_SIZE		12U
#define I915_XD_DMC_HEADER_LENGTH_AT	4U
#define I915_XD_DMC_HEADER_VERSION_AT	5U
#define I915_XD_DMC_HEADER_DWORDS_AT	12U
#define I915_XD_DMC_HEADER_BASE_SIZE	20U
#define I915_XD_DMC_V3_START		20U
#define I915_XD_DMC_V3_COUNT		92U
#define I915_XD_DMC_V3_ADDRESS		96U
#define I915_XD_DMC_V3_VALUE		176U
#define I915_XD_DMC_V3_SIZE		256U
#define I915_XD_DMC_V1_COUNT		20U
#define I915_XD_DMC_V1_ADDRESS		24U
#define I915_XD_DMC_V1_VALUE		56U
#define I915_XD_DMC_V1_SIZE		128U

/*
 * The most entries a package of each version lists, the most registers
 * a version 1 header names, and the largest program of display 12 and of
 * display 13.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dmc.c (PACKAGE_MAX_FW_INFO_ENTRIES,
 * PACKAGE_V2_MAX_FW_INFO_ENTRIES, DMC_V1_MAX_MMIO_COUNT,
 * ICL_DMC_MAX_FW_SIZE, DISPLAY_VER13_DMC_MAX_FW_SIZE).
 */
#define I915_XD_DMC_V1_ENTRIES		20U
#define I915_XD_DMC_V2_ENTRIES		32U
#define I915_XD_DMC_V1_REGISTERS	8U
#define I915_XD_DMC_MAX_BYTES_12	0x6000U
#define I915_XD_DMC_MAX_BYTES_13	0x20000U

/* The stepping a package entry names for any stepping. */
#define I915_XD_DMC_ANY_STEP		'*'

static uint32_t i915_xd_dmc_le32(const uint8_t *bytes, size_t offset);
static void i915_xd_dmc_stepping(uint8_t step, char *stepping, char *substepping);
static int i915_xd_dmc_entry_fits(const uint8_t *entry, char stepping, char substepping);
static void i915_xd_dmc_choose(struct i915_xd_dmc *dmc, const uint8_t *entries, unsigned count, unsigned package_version, char stepping, char substepping);
static int i915_xd_dmc_in_range(const struct i915_xd_platform *platform, unsigned id, unsigned header_version, const uint32_t *address, uint32_t count);
static void i915_xd_dmc_program(struct i915_xd_dmc *dmc, const struct i915_xd_platform *platform, unsigned id, const uint8_t *data, size_t rem, struct i915_xd_dmc_store *store);
static uint32_t i915_xd_dmc_base(const struct i915_xd_platform *platform, unsigned id);
static uint32_t i915_xd_dmc_handler_word(void);
static uint32_t i915_xd_dmc_register_value(const struct i915_xd_dmc *dmc, const struct i915_xd_platform *platform, unsigned id, unsigned index);

/*
 * Prepares an empty parse: no program, no version, no load done.
 */
void
drv_i915_xd_dmc_init(
	struct i915_xd_dmc *dmc)
{
	unsigned id;
	unsigned index;

	/* Nothing found in a file yet. */
	dmc->version = 0U;
	dmc->file_refused = I915_XD_DMC_FILE_OK;
	dmc->file_found = 0U;
	dmc->package_version = 0U;
	dmc->entries = 0U;

	/* No DMC has a program. */
	for (id = 0U; id < I915_XD_DMC_COUNT; id++) {
		dmc->program[id].chosen = 0;
		dmc->program[id].offset = 0U;
		dmc->program[id].refused = I915_XD_DMC_TAKEN;
		dmc->program[id].header_version = 0U;
		dmc->program[id].start = 0U;
		dmc->program[id].dwords = 0U;
		dmc->program[id].payload = NULL;
		dmc->program[id].registers = 0U;
		for (index = 0U; index < I915_XD_DMC_MAX_REGISTERS; index++) {
			dmc->program[id].address[index] = 0U;
			dmc->program[id].value[index] = 0U;
		}
	}

	/* No load done. */
	dmc->program_writes = 0U;
	dmc->register_writes = 0U;
	dmc->handler_writes = 0U;
	dmc->program_sum = 0ULL;
	dmc->register_sum = 0ULL;
	dmc->loaded = 0;
}

/*
 * Parses a firmware file: the signature header (its length must be its
 * own), the package header (version 1 lists only the main DMC, version 2
 * every DMC; its length must cover its entries), the entry of each DMC
 * for the stepping (the first that fits: exact, any substepping of the
 * stepping, the substepping of any stepping, or any at all), and each
 * chosen program, copied into the owner's storage.  Returns 0 once the
 * main DMC's program is taken, EINVAL for a file refused as a whole
 * (file_refused says why), or ENOENT without a main program (each
 * program's refused says why).
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dmc.c (parse_dmc_fw,
 * parse_dmc_fw_css, parse_dmc_fw_package, dmc_set_fw_offset,
 * fw_info_matches_stepping).
 */
int
drv_i915_xd_dmc_parse(
	struct i915_xd_dmc *dmc,
	const struct i915_xd_platform *platform,
	uint8_t step,
	const uint8_t *data,
	size_t size,
	struct i915_xd_dmc_store *store)
{
	const uint8_t *package;
	uint32_t length;
	size_t package_size;
	size_t done;
	size_t at;
	unsigned most;
	unsigned count;
	unsigned id;
	char stepping;
	char substepping;
	int main_taken;

	/* Starts from an empty parse. */
	drv_i915_xd_dmc_init(dmc);

	/* Refuses a missing file. */
	if (data == NULL || size == 0U) {
		dmc->file_refused = I915_XD_DMC_FILE_NONE;
		return EINVAL;
	}

	/* The signature header must fit and say its own length. */
	if (size < I915_XD_DMC_CSS_SIZE) {
		dmc->file_refused = I915_XD_DMC_FILE_CSS_LENGTH;
		return EINVAL;
	}

	/* Refuses a header length other than the CSS header's. */
	length = i915_xd_dmc_le32(data, I915_XD_DMC_CSS_LENGTH) * 4U;
	if (length != I915_XD_DMC_CSS_SIZE) {
		dmc->file_refused = I915_XD_DMC_FILE_CSS_LENGTH;
		dmc->file_found = length;
		return EINVAL;
	}

	/* The firmware's version. */
	dmc->version = i915_xd_dmc_le32(data, I915_XD_DMC_CSS_VERSION);

	/* The package header must fit; its version says how many entries it has room for. */
	package = data + I915_XD_DMC_CSS_SIZE;
	if (size - I915_XD_DMC_CSS_SIZE < I915_XD_DMC_PACKAGE_SIZE) {
		dmc->file_refused = I915_XD_DMC_FILE_PACKAGE_LENGTH;
		return EINVAL;
	}

	/* The package's version decides the size of its entries. */
	dmc->package_version = package[I915_XD_DMC_PACKAGE_VERSION];
	if (dmc->package_version == 1U) {
		most = I915_XD_DMC_V1_ENTRIES;
	} else if (dmc->package_version == 2U) {
		most = I915_XD_DMC_V2_ENTRIES;
	} else {
		dmc->file_refused = I915_XD_DMC_FILE_PACKAGE_VERSION;
		dmc->file_found = dmc->package_version;
		return EINVAL;
	}

	/* The entries must fit, and the header's length must cover them. */
	package_size = I915_XD_DMC_PACKAGE_SIZE + (size_t)most * I915_XD_DMC_ENTRY_SIZE;
	if (size - I915_XD_DMC_CSS_SIZE < package_size) {
		dmc->file_refused = I915_XD_DMC_FILE_PACKAGE_LENGTH;
		return EINVAL;
	}

	/* Refuses a package whose own length disagrees. */
	if ((size_t)package[I915_XD_DMC_PACKAGE_LENGTH] * 4U != package_size) {
		dmc->file_refused = I915_XD_DMC_FILE_PACKAGE_LENGTH;
		dmc->file_found = (uint32_t)package_size;
		return EINVAL;
	}

	/* Takes at most the entries the version has room for, and chooses each DMC's entry. */
	count = i915_xd_dmc_le32(package, I915_XD_DMC_PACKAGE_ENTRIES);
	if (count > most)
		count = most;
	dmc->entries = count;
	i915_xd_dmc_stepping(step, &stepping, &substepping);
	i915_xd_dmc_choose(dmc, package + I915_XD_DMC_PACKAGE_SIZE, count, dmc->package_version, stepping, substepping);

	/* Parses each chosen program; its offset counts dwords after the two headers. */
	done = I915_XD_DMC_CSS_SIZE + package_size;
	for (id = 0U; id < I915_XD_DMC_COUNT; id++) {
		if (!dmc->program[id].chosen)
			continue;

		/* An offset past the file is refused. */
		at = done + (size_t)dmc->program[id].offset * 4U;
		if (at > size) {
			dmc->program[id].refused = I915_XD_DMC_BEYOND;
			continue;
		}

		/* Takes the program of the DMC. */
		i915_xd_dmc_program(dmc, platform, id, data + at, size - at, store);
	}

	/* The main DMC's program is what a load needs. */
	main_taken = drv_i915_xd_dmc_has_main(dmc);
	if (!main_taken)
		return ENOENT;

	/* Succeeded: the main program is taken. */
	return 0;
}

/* Tells whether the main DMC's program was taken (1 or 0). */
int
drv_i915_xd_dmc_has_main(
	const struct i915_xd_dmc *dmc)
{
	int has;

	/* The main DMC is the first. */
	has = drv_i915_xd_dmc_has(dmc, I915_XD_DMC_MAIN);
	if (!has)
		return 0;

	/* Succeeded: the main program is there. */
	return 1;
}

/* Tells whether a DMC's program was taken (1 or 0). */
int
drv_i915_xd_dmc_has(
	const struct i915_xd_dmc *dmc,
	unsigned id)
{
	/* A DMC the package does not name has none. */
	if (id >= I915_XD_DMC_COUNT)
		return 0;

	/* Only a copied program counts. */
	if (dmc->program[id].payload == NULL)
		return 0;

	/* Succeeded: the program is there. */
	return 1;
}

/*
 * Loads the taken programs into the DMCs.
 *
 * Where the platform needs it the pipe DMCs' clock gating is turned off
 * for every pipe (pipes A and B keep it off); from display 12 on the
 * event handlers of every DMC with a program are turned off; the
 * programs are written, dword by dword, with preemption off; then each
 * program's registers, a pipe DMC's event handlers (and on some
 * platforms some of the main DMC's) written as off; the DC state debug
 * masks are set; and the clock gating of pipes C and D is given back.  Without a main program
 * nothing is written.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dmc.c (intel_dmc_load_program,
 * pipedmc_clock_gating_wa, adlp_pipedmc_clock_gating_wa,
 * disable_all_event_handlers, disable_event_handler, dmc_mmiodata,
 * disable_dmc_evt, gen9_set_dc_state_debugmask).
 */
void
drv_i915_xd_dmc_load(
	struct i915_xd_dmc *dmc,
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform)
{
	const struct i915_xd_dmc_program *program;
	uint32_t address;
	uint32_t value;
	unsigned pipe;
	unsigned id;
	unsigned handler;
	unsigned index;
	int has;

	/* Starts the diagnostics of this load. */
	dmc->program_writes = 0U;
	dmc->register_writes = 0U;
	dmc->handler_writes = 0U;
	dmc->program_sum = 0ULL;
	dmc->register_sum = 0ULL;
	dmc->loaded = 0;

	/* Nothing to load without the main program. */
	has = drv_i915_xd_dmc_has_main(dmc);
	if (!has)
		return;

	/* Turns the pipe DMCs' clock gating off while the programs go in. */
	if ((platform->wa & I915_XD_WA_PIPEDMC_UNGATE) != 0U) {
		for (pipe = 0U; pipe < 4U; pipe++)
			(void)drv_i915_xd_io_rmw(io, I915_XD_CLKGATE_DIS_PSL_EXT(pipe), 0U, I915_XD_CLKGATE_PSL_EXT_PIPEDMC);
	}

	/* Turns every event handler of each DMC with a program off (display 12 on; display 11 keeps them). */
	for (id = 0U; id < I915_XD_DMC_COUNT && platform->display_ver >= 12U; id++) {
		has = drv_i915_xd_dmc_has(dmc, id);
		if (!has)
			continue;

		/* Each handler's control to a never-firing event, its address to none. */
		for (handler = 0U; handler < I915_XD_DMC_HANDLERS; handler++) {
			address = i915_xd_dmc_base(platform, id) + I915_XD_DMC_EVT_CTL_OFFSET + 4U * handler;
			drv_i915_xd_io_write(io, address, i915_xd_dmc_handler_word());
			address = i915_xd_dmc_base(platform, id) + I915_XD_DMC_EVT_HTP_OFFSET + 4U * handler;
			drv_i915_xd_io_write(io, address, 0U);
			dmc->handler_writes += 2U;
		}
	}

	/* Writes the programs with preemption off, folding each write into an order-sensitive sum. */
	kern_preempt_disable();

	for (id = 0U; id < I915_XD_DMC_COUNT; id++) {
		program = &dmc->program[id];
		if (program->payload == NULL)
			continue;

		/* Writes each dword at its place in the program storage. */
		for (index = 0U; index < program->dwords; index++) {
			address = program->start + 4U * index;
			value = i915_xd_dmc_le32(program->payload, 4U * (size_t)index);
			drv_i915_xd_io_write(io, address, value);
			dmc->program_sum = dmc->program_sum * 1000003ULL + address + (uint64_t)value * 7ULL;
			dmc->program_writes++;
		}
	}

	kern_preempt_enable();

	/* Writes each program's registers, with the event handlers that must stay off written as off. */
	for (id = 0U; id < I915_XD_DMC_COUNT; id++) {
		program = &dmc->program[id];
		if (program->payload == NULL)
			continue;

		/* Writes each register of the header. */
		for (index = 0U; index < program->registers; index++) {
			address = program->address[index];
			value = i915_xd_dmc_register_value(dmc, platform, id, index);
			drv_i915_xd_io_write(io, address, value);
			dmc->register_sum = dmc->register_sum * 1000003ULL + address + (uint64_t)value * 7ULL;
			dmc->register_writes++;
		}
	}

	/* Sets the DC state debug masks, which are never cleared, and reads the word back to post the write. */
	(void)drv_i915_xd_io_rmw(io, I915_XD_DC_STATE_DEBUG, 0U, I915_XD_DC_STATE_DEBUG_MASK_CORES | I915_XD_DC_STATE_DEBUG_MASK_MEMORY_UP);
	(void)drv_i915_xd_io_read(io, I915_XD_DC_STATE_DEBUG);

	/* Gives the clock gating of pipes C and D back; pipes A and B keep it off. */
	if ((platform->wa & I915_XD_WA_PIPEDMC_UNGATE) != 0U) {
		for (pipe = 2U; pipe < 4U; pipe++)
			(void)drv_i915_xd_io_rmw(io, I915_XD_CLKGATE_DIS_PSL_EXT(pipe), I915_XD_CLKGATE_PSL_EXT_PIPEDMC, 0U);
	}

	/* The whole load ran. */
	dmc->loaded = 1;
}

/*
 * Switches a pipe's DMC on (after the pipe runs) or off (before it
 * stops), when the package had a program for it.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dmc.c (intel_dmc_enable_pipe,
 * intel_dmc_disable_pipe).
 */
void
drv_i915_xd_dmc_pipe(
	const struct i915_xd_dmc *dmc,
	struct i915_xd_io *io,
	unsigned pipe,
	int on)
{
	int has;

	/* A pipe without a program has nothing to switch. */
	has = drv_i915_xd_dmc_has(dmc, I915_XD_DMC_PIPE(pipe));
	if (!has)
		return;

	/* Sets or clears the enable of the pipe's DMC. */
	drv_i915_xd_dmc_pipe_switch(io, pipe, on);
}

/*
 * Sets or clears the enable of a pipe's DMC whose program is loaded (the
 * caller knows it is).
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dmc.c (intel_dmc_enable_pipe,
 * intel_dmc_disable_pipe).
 */
void
drv_i915_xd_dmc_pipe_switch(
	struct i915_xd_io *io,
	unsigned pipe,
	int on)
{
	/* The enable bit, alone. */
	if (on) {
		(void)drv_i915_xd_io_rmw(io, I915_XD_PIPEDMC_CONTROL(pipe), 0U, I915_XD_PIPEDMC_ENABLE);
	} else {
		(void)drv_i915_xd_io_rmw(io, I915_XD_PIPEDMC_CONTROL(pipe), I915_XD_PIPEDMC_ENABLE, 0U);
	}
}

/* Reads a little-endian dword of the file. */
static uint32_t
i915_xd_dmc_le32(
	const uint8_t *bytes,
	size_t offset)
{
	uint32_t value;

	/* The lowest byte first. */
	value = (uint32_t)bytes[offset];
	value |= (uint32_t)bytes[offset + 1U] << 8;
	value |= (uint32_t)bytes[offset + 2U] << 16;
	value |= (uint32_t)bytes[offset + 3U] << 24;

	/* Succeeded: the dword. */
	return value;
}

/*
 * Spells a display stepping as the package names it: the letter and the
 * digit, or two wildcards for a stepping past the platform's table.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/intel_step.c
 * (intel_step_name: STEP_FUTURE).
 */
static void
i915_xd_dmc_stepping(
	uint8_t step,
	char *stepping,
	char *substepping)
{
	/* A future stepping is named by wildcards. */
	if (step == I915_XD_STEP_FUTURE) {
		*stepping = I915_XD_DMC_ANY_STEP;
		*substepping = I915_XD_DMC_ANY_STEP;
		return;
	}

	/* The letter from the upper half, the digit from the lower. */
	*stepping = (char)('A' + (step >> 4));
	*substepping = (char)('0' + (step & 0x0fU));
}

/*
 * Tells whether a package entry is for the stepping: exactly, any
 * substepping of the stepping, the substepping of any stepping, or any
 * stepping at all (1 or 0).
 */
static int
i915_xd_dmc_entry_fits(
	const uint8_t *entry,
	char stepping,
	char substepping)
{
	char entry_stepping;
	char entry_substepping;

	/* The entry's stepping and substepping. */
	entry_stepping = (char)entry[I915_XD_DMC_ENTRY_STEPPING];
	entry_substepping = (char)entry[I915_XD_DMC_ENTRY_SUBSTEPPING];

	/* Any substepping of the stepping. */
	if (entry_substepping == I915_XD_DMC_ANY_STEP && entry_stepping == stepping)
		return 1;

	/* Exactly the stepping and the substepping. */
	if (entry_stepping == stepping && entry_substepping == substepping)
		return 1;

	/* A wildcard stepping of the display with the entry's substepping. */
	if (stepping == I915_XD_DMC_ANY_STEP && entry_substepping == substepping)
		return 1;

	/* Any stepping at all. */
	if (entry_stepping == I915_XD_DMC_ANY_STEP && entry_substepping == I915_XD_DMC_ANY_STEP)
		return 1;

	/* The entry is for another stepping. */
	return 0;
}

/*
 * Chooses each DMC's entry: the first one that fits wins (the package
 * lists the more specific first).  A version 1 package's entries are all
 * for the main DMC.
 */
static void
i915_xd_dmc_choose(
	struct i915_xd_dmc *dmc,
	const uint8_t *entries,
	unsigned count,
	unsigned package_version,
	char stepping,
	char substepping)
{
	const uint8_t *entry;
	unsigned index;
	unsigned id;
	int fits;

	/* Walks the entries in order. */
	for (index = 0U; index < count; index++) {
		/* The DMC the entry is for. */
		entry = entries + (size_t)index * I915_XD_DMC_ENTRY_SIZE;
		id = I915_XD_DMC_MAIN;
		if (package_version > 1U)
			id = entry[I915_XD_DMC_ENTRY_ID];

		/* An unknown DMC, or one already given an entry, is passed over. */
		if (id >= I915_XD_DMC_COUNT)
			continue;
		if (dmc->program[id].chosen)
			continue;

		/* Takes the entry when it fits the stepping. */
		fits = i915_xd_dmc_entry_fits(entry, stepping, substepping);
		if (fits) {
			dmc->program[id].chosen = 1;
			dmc->program[id].offset = i915_xd_dmc_le32(entry, I915_XD_DMC_ENTRY_OFFSET);
		}
	}
}

/*
 * Tells whether every register a program's header names is in the range
 * its DMC may write (1 or 0): the whole DMC block for a version 1
 * header, the main DMC's block, the pipe DMCs' block on display 13, or
 * the pipe's own block on display 12.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dmc.c (dmc_mmio_addr_sanity_check).
 */
static int
i915_xd_dmc_in_range(
	const struct i915_xd_platform *platform,
	unsigned id,
	unsigned header_version,
	const uint32_t *address,
	uint32_t count)
{
	uint32_t low;
	uint32_t high;
	uint32_t index;

	/* Picks the range of the header version, the DMC and the display. */
	if (header_version == 1U) {
		low = I915_XD_DMC_V1_PROGRAM_BASE;
		high = I915_XD_DMC_MAIN_MMIO_END;
	} else if (id == I915_XD_DMC_MAIN) {
		low = I915_XD_DMC_MAIN_MMIO_START;
		high = I915_XD_DMC_MAIN_MMIO_END;
	} else if (platform->display_ver >= 13U) {
		low = I915_XD_DMC_PIPE_MMIO_START_ADLP;
		high = I915_XD_DMC_PIPE_MMIO_END_ADLP;
	} else {
		low = I915_XD_DMC_PIPE_A_MMIO_START_TGL + (id - 1U) * (I915_XD_DMC_PIPE_B_MMIO_START_TGL - I915_XD_DMC_PIPE_A_MMIO_START_TGL);
		high = I915_XD_DMC_PIPE_A_MMIO_END_TGL + (id - 1U) * (I915_XD_DMC_PIPE_B_MMIO_END_TGL - I915_XD_DMC_PIPE_A_MMIO_END_TGL);
	}

	/* Every register must be in the range. */
	for (index = 0U; index < count; index++) {
		if (address[index] < low)
			return 0;
		if (address[index] > high)
			return 0;
	}

	/* Succeeded: every register is in range. */
	return 1;
}

/*
 * Parses one DMC's program header and takes its program: the header's
 * version and length, its registers (count and range), where the
 * program goes, and the program itself (it must fit in the file, not
 * exceed the platform's largest, and fit in the owner's storage).  A
 * program not taken has refused set.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dmc.c (parse_dmc_fw_header).
 */
static void
i915_xd_dmc_program(
	struct i915_xd_dmc *dmc,
	const struct i915_xd_platform *platform,
	unsigned id,
	const uint8_t *data,
	size_t rem,
	struct i915_xd_dmc_store *store)
{
	struct i915_xd_dmc_program *program;
	size_t header_size;
	size_t header_length;
	size_t address_at;
	size_t value_at;
	size_t bytes;
	size_t most_bytes;
	uint32_t most_registers;
	uint32_t count;
	uint32_t index;
	uint32_t start;
	unsigned version;
	int in_range;

	/* The shared part of the header must fit. */
	program = &dmc->program[id];
	if (rem < I915_XD_DMC_HEADER_BASE_SIZE) {
		program->refused = I915_XD_DMC_TRUNCATED;
		return;
	}

	/* Reads the layout of the version: version 3 counts its length in dwords, version 1 in bytes. */
	version = data[I915_XD_DMC_HEADER_VERSION_AT];
	if (version == 3U) {
		header_size = I915_XD_DMC_V3_SIZE;
		header_length = (size_t)data[I915_XD_DMC_HEADER_LENGTH_AT] * 4U;
		most_registers = I915_XD_DMC_MAX_REGISTERS;
		address_at = I915_XD_DMC_V3_ADDRESS;
		value_at = I915_XD_DMC_V3_VALUE;
	} else if (version == 1U) {
		header_size = I915_XD_DMC_V1_SIZE;
		header_length = data[I915_XD_DMC_HEADER_LENGTH_AT];
		most_registers = I915_XD_DMC_V1_REGISTERS;
		address_at = I915_XD_DMC_V1_ADDRESS;
		value_at = I915_XD_DMC_V1_VALUE;
	} else {
		program->refused = I915_XD_DMC_HEADER_VERSION;
		return;
	}

	/* The whole header must fit and say its own length. */
	if (rem < header_size) {
		program->refused = I915_XD_DMC_TRUNCATED;
		return;
	}

	/* Refuses a header whose length is not its version's. */
	if (header_length != header_size) {
		program->refused = I915_XD_DMC_HEADER_LENGTH;
		return;
	}

	/* The register count of the version, where the program goes. */
	start = I915_XD_DMC_V1_PROGRAM_BASE;
	if (version == 3U) {
		count = i915_xd_dmc_le32(data, I915_XD_DMC_V3_COUNT);
		start = i915_xd_dmc_le32(data, I915_XD_DMC_V3_START);
	} else {
		count = i915_xd_dmc_le32(data, I915_XD_DMC_V1_COUNT);
	}

	/* Refuses more registers than the header holds. */
	if (count > most_registers) {
		program->refused = I915_XD_DMC_TOO_MANY;
		return;
	}

	/* Reads the registers; every one must be in the DMC's range. */
	for (index = 0U; index < count; index++) {
		program->address[index] = i915_xd_dmc_le32(data, address_at + 4U * (size_t)index);
		program->value[index] = i915_xd_dmc_le32(data, value_at + 4U * (size_t)index);
	}

	/* Refuses registers outside the DMC's range. */
	in_range = i915_xd_dmc_in_range(platform, id, version, program->address, count);
	if (!in_range) {
		program->refused = I915_XD_DMC_OUT_OF_RANGE;
		return;
	}

	/* Records the registers and the program's start. */
	program->registers = count;
	program->start = start;
	program->header_version = version;

	/* The program follows the header: it must fit in the file and not exceed the platform's largest. */
	bytes = (size_t)i915_xd_dmc_le32(data, I915_XD_DMC_HEADER_DWORDS_AT) * 4U;
	if (rem - header_length < bytes) {
		program->refused = I915_XD_DMC_TRUNCATED;
		return;
	}

	/* The largest program the display version takes. */
	most_bytes = I915_XD_DMC_MAX_BYTES_12;
	if (platform->display_ver >= 13U)
		most_bytes = I915_XD_DMC_MAX_BYTES_13;
	if (bytes > most_bytes) {
		program->refused = I915_XD_DMC_TOO_BIG;
		return;
	}

	/* Copies the program into the owner's storage. */
	if (bytes > store->room - store->used) {
		program->refused = I915_XD_DMC_NO_ROOM;
		return;
	}

	/* Copies the program into the display's storage. */
	for (index = 0U; index < bytes; index++)
		store->bytes[store->used + index] = data[header_length + index];
	program->payload = &store->bytes[store->used];
	program->dwords = (uint32_t)(bytes / 4U);
	store->used += bytes;
}

/* Reports the base of a DMC's event registers: the main DMC's, or a pipe DMC's in the platform's block. */
static uint32_t
i915_xd_dmc_base(
	const struct i915_xd_platform *platform,
	unsigned id)
{
	/* The main DMC has its own block. */
	if (id == I915_XD_DMC_MAIN)
		return I915_XD_DMC_MAIN_BASE;

	/* Succeeded: the pipe DMC's block. */
	return platform->pipedmc_base + I915_XD_DMC_PIPE_STRIDE * (id - 1U);
}

/* Reports the control word of an event handler that never fires: the FALSE event on a rising edge. */
static uint32_t
i915_xd_dmc_handler_word(void)
{
	uint32_t word;

	/* The edge type and the event. */
	word = I915_XD_DMC_EVT_TYPE_EDGE_0_1 << 16;
	word |= I915_XD_DMC_EVT_ID_FALSE << 8;

	/* Succeeded: the word. */
	return word;
}

/*
 * Reports the value a program's register is written with: a pipe DMC's
 * event handler control is written as off, and so is the main DMC's
 * handler for the millisecond clock (Tiger Lake) or for pipe A's vertical
 * blank (Tiger Lake, Alder Lake-S); every other register takes the
 * header's value.
 */
static uint32_t
i915_xd_dmc_register_value(
	const struct i915_xd_dmc *dmc,
	const struct i915_xd_platform *platform,
	unsigned id,
	unsigned index)
{
	uint32_t address;
	uint32_t value;
	uint32_t first;
	uint32_t event;

	/* A register that is not an event handler control keeps its value. */
	address = dmc->program[id].address[index];
	value = dmc->program[id].value[index];
	first = i915_xd_dmc_base(platform, id) + I915_XD_DMC_EVT_CTL_OFFSET;
	if (address < first || address >= first + 4U * I915_XD_DMC_HANDLERS)
		return value;

	/* Every pipe DMC handler stays off. */
	if (id != I915_XD_DMC_MAIN)
		return i915_xd_dmc_handler_word();

	/* The main DMC's millisecond clock handler stays off where the platform asks. */
	event = (value & I915_XD_DMC_EVT_ID) >> 8;
	if ((platform->wa & I915_XD_WA_DMC_CLK_MSEC_VBLANK) != 0U && event == I915_XD_DMC_EVT_ID_CLK_MSEC)
		return i915_xd_dmc_handler_word();

	/* So does its pipe A vertical blank handler. */
	if ((platform->wa & (I915_XD_WA_DMC_CLK_MSEC_VBLANK | I915_XD_WA_DMC_VBLANK_A)) != 0U && event == I915_XD_DMC_EVT_ID_VBLANK_A)
		return i915_xd_dmc_handler_word();

	/* Succeeded: the header's value. */
	return value;
}
