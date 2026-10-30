/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The decoder of the model register allow list, RAPL energy, C-state residency and the package C-state limit.
 */

#include "cpux86-power.h"

/*
 * The family every listed model belongs to, as DisplayFamily.
 *
 * Value referenced: Intel SDM Vol. 4 rev. 093, §2.17 (DisplayFamily_DisplayModel
 * 06_97H, 06_9AH, 06_B7H, 06_BAH and 06_BFH).
 */
#define POWER_LISTED_FAMILY		0x06U

/*
 * The registers every listed model has: all fifteen of the allow list,
 * since the five models take the same tables (2-20, 2-21, 2-25, 2-29,
 * 2-35 and 2-39, and 2-46 for the module C6 counter).  The DRAM energy
 * register is not among them.
 *
 * Values referenced: Intel SDM Vol. 4 rev. 093, §2.17 (the paragraph
 * before Table 2-39) and §2.17.5 (the paragraph before Table 2-46).
 */
#define POWER_LISTED_REGISTERS \
	(CPUX86_MODEL_PLATFORM_INFO | \
	 CPUX86_MODEL_PKG_CST_CONFIG | \
	 CPUX86_MODEL_RAPL_UNIT | \
	 CPUX86_MODEL_PKG_ENERGY | \
	 CPUX86_MODEL_PP0_ENERGY | \
	 CPUX86_MODEL_PP1_ENERGY | \
	 CPUX86_MODEL_PLATFORM_ENERGY | \
	 CPUX86_MODEL_PKG_C2 | \
	 CPUX86_MODEL_PKG_C3 | \
	 CPUX86_MODEL_PKG_C6 | \
	 CPUX86_MODEL_PKG_C7 | \
	 CPUX86_MODEL_CORE_C3 | \
	 CPUX86_MODEL_CORE_C6 | \
	 CPUX86_MODEL_CORE_C7 | \
	 CPUX86_MODEL_MODULE_C6)

/* The reserved package C-state limit codes start here. */
#define POWER_PKG_LIMIT_RESERVED	((uint32_t)CPUX86_PKG_LIMIT_C10 + 1U)

/* The number of microjoules in a joule. */
#define POWER_MICRO_PER_UNIT		1000000U

/* The number of nanoseconds in a second. */
#define POWER_NANO_PER_SECOND		1000000000U

/* The largest energy shift the units conversion accepts: 2^31 fits a 32-bit mask. */
#define POWER_ENERGY_SHIFT_MAX		31U

/* The largest value a 64-bit counter holds. */
#define POWER_U64_MAX			(~(uint64_t)0U)

/* The CFG Lock bit of MSR_PKG_CST_CONFIG_CONTROL, which freezes bits 15:0 until the next reset. */
#define POWER_PKG_CST_LOCK_BIT		15U

/*
 * One processor model of the allow list: its DisplayModel and the
 * registers it has.
 */
struct power_model {
	uint32_t display_model;		/* DisplayModel, with DisplayFamily POWER_LISTED_FAMILY */
	uint32_t registers;		/* CPUX86_MODEL_* bits */
};

/*
 * The allow list.
 *
 * Only these models are covered; reading a register of a model not here
 * could fault.  The table is constant and lives as long as the program.
 *
 * Values referenced: Intel SDM Vol. 4 rev. 093, §2.17 (the paragraph
 * before Table 2-39: 06_97H and 06_9AH, the 12th generation Intel Core
 * processors) and §2.17.5 (the 13th generation: 06_B7H, 06_BAH and
 * 06_BFH); the registers are those of Tables 2-20, 2-21, 2-25, 2-29, 2-35
 * and 2-39 and Tables 2-44 to 2-48.
 */
static const struct power_model power_models[] = {
	{ 0x97U, POWER_LISTED_REGISTERS },
	{ 0x9AU, POWER_LISTED_REGISTERS },
	{ 0xB7U, POWER_LISTED_REGISTERS },
	{ 0xBAU, POWER_LISTED_REGISTERS },
	{ 0xBFU, POWER_LISTED_REGISTERS }
};

/* The number of models in the allow list. */
#define POWER_MODEL_COUNT		(sizeof(power_models) / sizeof(power_models[0]))

/*
 * The residency counters, in the order of the CPUX86_RESIDENCY_*
 * identifiers.
 *
 * A counter's register address, the allow list bit that permits it, what
 * it covers, the processor's number n of the state Cn, and its rate.
 * The table is constant and lives as long as the program.
 *
 * Values referenced: Intel SDM Vol. 4 rev. 093, Table 2-20 (PKG C2 60DH,
 * PKG C3 3F8H, PKG C6 3F9H, PKG C7 3FAH: Scope Package; CORE C3 3FCH,
 * CORE C6 3FDH, CORE C7 3FEH: Scope Core; all "Count at the same
 * frequency as the TSC") and Table 2-46 (MC6 664H: Scope Module, "Counts
 * at 1 Mhz frequency").
 */
static const struct cpux86_residency_info power_residency[CPUX86_RESIDENCY_COUNT] = {
	{ 0x60DU, CPUX86_MODEL_PKG_C2, CPUX86_SCOPE_PACKAGE, 2U, CPUX86_RATE_TSC },
	{ 0x3F8U, CPUX86_MODEL_PKG_C3, CPUX86_SCOPE_PACKAGE, 3U, CPUX86_RATE_TSC },
	{ 0x3F9U, CPUX86_MODEL_PKG_C6, CPUX86_SCOPE_PACKAGE, 6U, CPUX86_RATE_TSC },
	{ 0x3FAU, CPUX86_MODEL_PKG_C7, CPUX86_SCOPE_PACKAGE, 7U, CPUX86_RATE_TSC },
	{ 0x3FCU, CPUX86_MODEL_CORE_C3, CPUX86_SCOPE_CORE, 3U, CPUX86_RATE_TSC },
	{ 0x3FDU, CPUX86_MODEL_CORE_C6, CPUX86_SCOPE_CORE, 6U, CPUX86_RATE_TSC },
	{ 0x3FEU, CPUX86_MODEL_CORE_C7, CPUX86_SCOPE_CORE, 7U, CPUX86_RATE_TSC },
	{ 0x664U, CPUX86_MODEL_MODULE_C6, CPUX86_SCOPE_MODULE, 6U, CPUX86_RATE_1MHZ }
};

/*
 * Reports which of the registers above a processor model has.
 *
 * The identity is matched against the allow list by vendor, family and
 * model; a processor that is not listed gets no register at all.
 */
int
cpux86_model_registers(
	const struct cpux86_identity *identity,
	uint32_t *registers)
{
	size_t index;

	/* Refuses a missing identity or a missing output. */
	if (identity == NULL || registers == NULL)
		return EINVAL;

	/* A vendor other than Intel has no registers the list knows. */
	if (identity->vendor != CPUX86_VENDOR_INTEL) {
		*registers = 0U;
		return ENOTSUP;
	}

	/* A family other than 06H has no registers the list knows. */
	if (identity->display_family != POWER_LISTED_FAMILY) {
		*registers = 0U;
		return ENOTSUP;
	}

	/* Looks for the model in the allow list. */
	for (index = 0U;
	     index < POWER_MODEL_COUNT;
	     index++) {
		/* A model that matches gives its registers. */
		if (power_models[index].display_model == identity->display_model) {
			*registers = power_models[index].registers;

			/* Succeeded: the listed registers of this model. */
			return 0;
		}
	}

	/* The model is not in the list, so no register may be read. */
	*registers = 0U;

	/* Reports that the model is not covered. */
	return ENOTSUP;
}

/*
 * Decodes MSR_RAPL_POWER_UNIT.
 *
 * The three unit fields are read at the bits of Figure 17-35; any bit
 * outside them must be zero.
 */
int
cpux86_rapl_units_decode(
	uint64_t raw,
	struct cpux86_rapl_units *units)
{
	struct cpux86_rapl_units decoded;
	uint64_t field;

	/* Refuses a missing output. */
	if (units == NULL)
		return EINVAL;

	/* Refuses the reserved bits 7:4 (the gap above the power units). */
	field = cpux86_field64(raw, 4U, 4U);
	if (field != 0U)
		return EINVAL;

	/* Refuses the reserved bits 15:13 (the gap above the energy units). */
	field = cpux86_field64(raw, 13U, 3U);
	if (field != 0U)
		return EINVAL;

	/* Refuses the reserved bits 63:20 (everything above the time units). */
	field = cpux86_field64(raw, 20U, 44U);
	if (field != 0U)
		return EINVAL;

	/* Reads the power unit: a power field counts 1/2^shift W. */
	field = cpux86_field64(raw, 0U, 4U);
	decoded.power_shift = (uint32_t)field;

	/* Reads the energy unit: an energy counter counts 1/2^shift J. */
	field = cpux86_field64(raw, 8U, 5U);
	decoded.energy_shift = (uint32_t)field;

	/* Reads the time unit: a time field counts 1/2^shift s. */
	field = cpux86_field64(raw, 16U, 4U);
	decoded.time_shift = (uint32_t)field;

	/* Publishes the units now that the whole register was accepted. */
	*units = decoded;

	/* Succeeded: *units holds the three shifts. */
	return 0;
}

/*
 * Decodes one energy status register (package, PP0, PP1 or platform).
 *
 * Only the low 32 bits carry the counter.
 */
int
cpux86_energy_status_decode(
	uint64_t raw,
	uint32_t *count)
{
	uint64_t field;

	/* Refuses a missing output. */
	if (count == NULL)
		return EINVAL;

	/* Refuses the reserved bits 63:32. */
	field = cpux86_field64(raw, 32U, 32U);
	if (field != 0U)
		return EINVAL;

	/* Reads the energy consumed, in units of 1/2^energy_shift J. */
	field = cpux86_field64(raw, 0U, 32U);
	*count = (uint32_t)field;

	/* Succeeded: *count holds the 32-bit reading. */
	return 0;
}

/*
 * Adds a reading to an energy counter.
 *
 * The advance since the previous reading is taken modulo 2^32, which
 * counts one wrap of the hardware counter and cannot see more.
 */
int
cpux86_energy_counter_update(
	struct cpux86_energy_counter *counter,
	uint32_t reading)
{
	uint32_t advance;

	/* Refuses a missing counter. */
	if (counter == NULL)
		return EINVAL;

	/* Refuses a started value that is neither 0 nor 1. */
	if (counter->started > 1U)
		return EINVAL;

	/* The first reading only records where the hardware counter is. */
	if (counter->started == 0U) {
		counter->last = reading;
		counter->started = 1U;

		/* Succeeded: the counter now has a baseline. */
		return 0;
	}

	/* Works out how far the hardware counter advanced, one wrap included. */
	advance = reading - counter->last;

	/* Refuses an advance that would take the total past 2^64 - 1. */
	if (counter->total > POWER_U64_MAX - (uint64_t)advance)
		return ERANGE;

	/* Accumulates the advance and remembers the reading for the next one. */
	counter->total += (uint64_t)advance;
	counter->last = reading;

	/* Succeeded: the total includes this reading. */
	return 0;
}

/*
 * Converts energy units to microjoules.
 *
 * The units are split into whole joules' worth (units >> shift) and the
 * remainder below one joule, so neither product passes 64 bits: the
 * remainder is below 2^31 and is multiplied by 10^6 < 2^20.
 */
int
cpux86_energy_microjoules(
	uint64_t units,
	uint32_t energy_shift,
	uint64_t *microjoules)
{
	uint64_t whole;
	uint64_t remainder;
	uint64_t whole_micro;
	uint64_t remainder_micro;

	/* Refuses a missing output or a shift the 31-bit mask cannot take. */
	if (microjoules == NULL || energy_shift > POWER_ENERGY_SHIFT_MAX)
		return EINVAL;

	/* Splits the units into whole joules and the part below one joule. */
	whole = units >> energy_shift;
	remainder = units & (((uint64_t)1U << energy_shift) - 1U);

	/* Refuses a whole-joule count whose microjoules pass 64 bits. */
	if (whole > POWER_U64_MAX / POWER_MICRO_PER_UNIT)
		return ERANGE;

	/* Converts the whole joules; the product fits by the check above. */
	whole_micro = whole * POWER_MICRO_PER_UNIT;

	/* Converts the part below one joule, rounding down. */
	remainder_micro = (remainder * POWER_MICRO_PER_UNIT) >> energy_shift;

	/* Refuses a sum that passes 64 bits. */
	if (whole_micro > POWER_U64_MAX - remainder_micro)
		return ERANGE;

	/* Publishes the converted energy. */
	*microjoules = whole_micro + remainder_micro;

	/* Succeeded: *microjoules holds the energy. */
	return 0;
}

/*
 * Reports where a residency counter is and how it counts.
 *
 * The answer is a row of the constant table.
 */
int
cpux86_residency_info(
	uint32_t counter,
	struct cpux86_residency_info *info)
{
	/* Refuses a missing output or an identifier the table does not have. */
	if (info == NULL || counter >= CPUX86_RESIDENCY_COUNT)
		return EINVAL;

	/* Copies the row of this counter. */
	*info = power_residency[counter];

	/* Succeeded: *info describes the counter. */
	return 0;
}

/*
 * Reports how far a residency counter advanced between two readings.
 *
 * A reading below the earlier one means the counter was reset, so the
 * advance is not known.
 */
int
cpux86_residency_delta(
	uint64_t before,
	uint64_t after,
	uint64_t *delta)
{
	/* Refuses a missing output. */
	if (delta == NULL)
		return EINVAL;

	/* Refuses a counter that went backwards: it was reset in between. */
	if (after < before)
		return ERANGE;

	/* Takes the advance. */
	*delta = after - before;

	/* Succeeded: *delta holds the advance in counts. */
	return 0;
}

/*
 * Converts residency counts to nanoseconds.
 *
 * The counts are split into whole seconds' worth (counts / rate) and the
 * remainder, so the remainder times 10^9 stays below rate * 10^9, which
 * is at most 10^19 and fits 64 bits.
 */
int
cpux86_residency_nanoseconds(
	uint64_t counts,
	uint64_t rate_hz,
	uint64_t *nanoseconds)
{
	uint64_t seconds;
	uint64_t remainder;
	uint64_t seconds_nano;
	uint64_t remainder_nano;

	/* Refuses a missing output or a rate outside 1 .. 10 GHz. */
	if (nanoseconds == NULL || rate_hz == 0U || rate_hz > CPUX86_RATE_MAX_HZ)
		return EINVAL;

	/* Splits the counts into whole seconds and a remainder below one second. */
	seconds = counts / rate_hz;
	remainder = counts % rate_hz;

	/* Refuses a whole-second count whose nanoseconds pass 64 bits. */
	if (seconds > POWER_U64_MAX / POWER_NANO_PER_SECOND)
		return ERANGE;

	/* Converts the whole seconds; the product fits by the check above. */
	seconds_nano = seconds * POWER_NANO_PER_SECOND;

	/* Converts the remainder, rounding down; the product is below 10^19. */
	remainder_nano = (remainder * POWER_NANO_PER_SECOND) / rate_hz;

	/* Refuses a sum that passes 64 bits. */
	if (seconds_nano > POWER_U64_MAX - remainder_nano)
		return ERANGE;

	/* Publishes the converted time. */
	*nanoseconds = seconds_nano + remainder_nano;

	/* Succeeded: *nanoseconds holds the time. */
	return 0;
}

/*
 * Decodes MSR_PKG_CST_CONFIG_CONTROL.
 *
 * The limit is bits 3:0 and the flags are the single bits of Table 2-35;
 * the reserved bits are not looked at.
 */
int
cpux86_pkg_cstate_decode(
	uint64_t raw,
	struct cpux86_pkg_cstate *config)
{
	struct cpux86_pkg_cstate decoded;
	uint64_t field;

	/* Refuses a missing output. */
	if (config == NULL)
		return EINVAL;

	/* Reads the package C-state limit and refuses the reserved codes. */
	field = cpux86_field64(raw, 0U, 4U);
	if (field >= POWER_PKG_LIMIT_RESERVED)
		return EINVAL;

	/* Keeps the limit now that it is known to be a defined code. */
	decoded.limit = (uint32_t)field;

	/* Starts the flags empty so each bit adds its own flag. */
	decoded.flags = 0U;

	/* Records that I/O MWAIT redirection is enabled. */
	field = cpux86_field64(raw, 10U, 1U);
	if (field != 0U)
		decoded.flags |= CPUX86_PKG_CST_IO_MWAIT;

	/* Records that the limit is locked against writes until the next reset. */
	field = cpux86_field64(raw, POWER_PKG_CST_LOCK_BIT, 1U);
	if (field != 0U)
		decoded.flags |= CPUX86_PKG_CST_LOCKED;

	/* Records that C6 and C7 requests may be demoted to C3. */
	field = cpux86_field64(raw, 25U, 1U);
	if (field != 0U)
		decoded.flags |= CPUX86_PKG_CST_C3_DEMOTION;

	/* Records that C3 and deeper requests may be demoted to C1. */
	field = cpux86_field64(raw, 26U, 1U);
	if (field != 0U)
		decoded.flags |= CPUX86_PKG_CST_C1_DEMOTION;

	/* Records that C3 undemotion is enabled. */
	field = cpux86_field64(raw, 27U, 1U);
	if (field != 0U)
		decoded.flags |= CPUX86_PKG_CST_C3_UNDEMOTION;

	/* Records that C1 undemotion is enabled. */
	field = cpux86_field64(raw, 28U, 1U);
	if (field != 0U)
		decoded.flags |= CPUX86_PKG_CST_C1_UNDEMOTION;

	/* Records that package C-state auto-demotion is enabled. */
	field = cpux86_field64(raw, 29U, 1U);
	if (field != 0U)
		decoded.flags |= CPUX86_PKG_CST_PKG_DEMOTION;

	/* Records that package C-state undemotion is enabled. */
	field = cpux86_field64(raw, 30U, 1U);
	if (field != 0U)
		decoded.flags |= CPUX86_PKG_CST_PKG_UNDEMOTION;

	/* Publishes the decoded register now that the limit was accepted. */
	*config = decoded;

	/* Succeeded: *config holds the limit and the flags. */
	return 0;
}

/*
 * Builds the MSR_PKG_CST_CONFIG_CONTROL value with a new limit.
 *
 * A locked register cannot change its low 16 bits, so a write would be
 * ignored; it is refused here instead.
 */
int
cpux86_pkg_cstate_limit_encode(
	uint64_t current,
	uint32_t limit,
	uint64_t *raw)
{
	uint64_t locked;

	/* Refuses a missing output or a limit code that is not defined. */
	if (raw == NULL || limit >= POWER_PKG_LIMIT_RESERVED)
		return EINVAL;

	/* Refuses to change a register whose CFG Lock bit is set. */
	locked = cpux86_field64(current, POWER_PKG_CST_LOCK_BIT, 1U);
	if (locked != 0U)
		return EACCES;

	/* Replaces the limit and keeps every other bit as it was read. */
	*raw = cpux86_insert64(current, 0U, 4U, (uint64_t)limit);

	/* Succeeded: *raw is the value to write. */
	return 0;
}
