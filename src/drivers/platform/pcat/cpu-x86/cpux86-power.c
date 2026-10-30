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
 * Reports which of the registers above a processor model has.
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
 */
int
cpux86_model_registers(
	const struct cpux86_identity *identity,
	uint32_t *registers)
{
	/* The skeleton reads none of its arguments. */
	(void)identity;
	(void)registers;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}

/*
 * Decodes MSR_RAPL_POWER_UNIT.
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
 */
int
cpux86_rapl_units_decode(
	uint64_t raw,
	struct cpux86_rapl_units *units)
{
	/* The skeleton reads none of its arguments. */
	(void)raw;
	(void)units;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}

/*
 * Decodes one energy status register (package, PP0, PP1 or platform).
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
 */
int
cpux86_energy_status_decode(
	uint64_t raw,
	uint32_t *count)
{
	/* The skeleton reads none of its arguments. */
	(void)raw;
	(void)count;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}

/*
 * Adds a reading to an energy counter.
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
 */
int
cpux86_energy_counter_update(
	struct cpux86_energy_counter *counter,
	uint32_t reading)
{
	/* The skeleton reads none of its arguments. */
	(void)counter;
	(void)reading;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}

/*
 * Converts energy units to microjoules.
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
 */
int
cpux86_energy_microjoules(
	uint64_t units,
	uint32_t energy_shift,
	uint64_t *microjoules)
{
	/* The skeleton reads none of its arguments. */
	(void)units;
	(void)energy_shift;
	(void)microjoules;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}

/*
 * Reports where a residency counter is and how it counts.
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
 */
int
cpux86_residency_info(
	uint32_t counter,
	struct cpux86_residency_info *info)
{
	/* The skeleton reads none of its arguments. */
	(void)counter;
	(void)info;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}

/*
 * Reports how far a residency counter advanced between two readings.
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
 */
int
cpux86_residency_delta(
	uint64_t before,
	uint64_t after,
	uint64_t *delta)
{
	/* The skeleton reads none of its arguments. */
	(void)before;
	(void)after;
	(void)delta;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}

/*
 * Converts residency counts to nanoseconds.
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
 */
int
cpux86_residency_nanoseconds(
	uint64_t counts,
	uint64_t rate_hz,
	uint64_t *nanoseconds)
{
	/* The skeleton reads none of its arguments. */
	(void)counts;
	(void)rate_hz;
	(void)nanoseconds;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}

/*
 * Decodes MSR_PKG_CST_CONFIG_CONTROL.
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
 */
int
cpux86_pkg_cstate_decode(
	uint64_t raw,
	struct cpux86_pkg_cstate *config)
{
	/* The skeleton reads none of its arguments. */
	(void)raw;
	(void)config;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}

/*
 * Builds the MSR_PKG_CST_CONFIG_CONTROL value with a new limit.
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
 */
int
cpux86_pkg_cstate_limit_encode(
	uint64_t current,
	uint32_t limit,
	uint64_t *raw)
{
	/* The skeleton reads none of its arguments. */
	(void)current;
	(void)limit;
	(void)raw;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}
