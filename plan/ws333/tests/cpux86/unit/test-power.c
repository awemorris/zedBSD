/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The checks of the power register decoders: the model allow list, the RAPL
 * units and energy counters, the C-state residency counters and the package
 * C-state limit register.  The expected values come from Intel SDM Vol. 4
 * rev. 093 (Tables 2-20, 2-35, 2-39, 2-46) and Vol. 3B rev. 093 (§17.10) and
 * from arithmetic worked out by hand.
 */

#include "check.h"
#include "regbuild.h"
#include "cpux86-power.h"

/* The largest value of 64 bits, spelled from two halves. */
#define MAX64		(((uint64_t)0xFFFFFFFFU << 32) | 0xFFFFFFFFU)

/* The state of the random input generator; the seed is fixed so a failure repeats. */
static uint64_t random_state = ((uint64_t)0x0FEDCBA9U << 32) | 0x87654321U;

/*
 * Gives the next 64-bit pseudo-random value (xorshift).
 */
static uint64_t
next_random(void)
{
	/* Mixes the state three times and keeps it. */
	random_state ^= random_state << 13;
	random_state ^= random_state >> 7;
	random_state ^= random_state << 17;

	/* Reports the new state. */
	return random_state;
}

/*
 * Fills an identity for the model check.
 */
static void
make_identity(
	struct cpux86_identity *identity,
	uint32_t vendor,
	uint32_t family,
	uint32_t model)
{
	/* Only the three fields the allow list reads matter. */
	identity->vendor = vendor;
	identity->max_basic_leaf = 0x20U;
	identity->max_extended_leaf = 0x80000008U;
	identity->display_family = family;
	identity->display_model = model;
	identity->stepping = 2U;
	identity->processor_type = 0U;
}

/*
 * Checks the allow list.
 */
static void
check_model_registers(void)
{
	struct cpux86_identity identity;
	uint32_t registers;
	uint32_t listed[5];
	uint32_t index;
	uint32_t model;
	uint32_t family;
	int error;

	/* The five listed models have all fifteen bits and no more. */
	listed[0] = 0x97U;
	listed[1] = 0x9AU;
	listed[2] = 0xB7U;
	listed[3] = 0xBAU;
	listed[4] = 0xBFU;
	for (index = 0U; index < 5U; index++) {
		make_identity(&identity, CPUX86_VENDOR_INTEL, 6U, listed[index]);
		registers = 0xDEADU;
		error = cpux86_model_registers(&identity, &registers);
		CHECK(error == 0);
		CHECK(registers == 0x7FFFU);
	}

	/* The bits are the fifteen distinct single bits 0x0001 .. 0x4000. */
	CHECK(CPUX86_MODEL_PLATFORM_INFO == 0x0001U);
	CHECK(CPUX86_MODEL_PKG_CST_CONFIG == 0x0002U);
	CHECK(CPUX86_MODEL_RAPL_UNIT == 0x0004U);
	CHECK(CPUX86_MODEL_PKG_ENERGY == 0x0008U);
	CHECK(CPUX86_MODEL_PP0_ENERGY == 0x0010U);
	CHECK(CPUX86_MODEL_PP1_ENERGY == 0x0020U);
	CHECK(CPUX86_MODEL_PLATFORM_ENERGY == 0x0040U);
	CHECK(CPUX86_MODEL_PKG_C2 == 0x0080U);
	CHECK(CPUX86_MODEL_PKG_C3 == 0x0100U);
	CHECK(CPUX86_MODEL_PKG_C6 == 0x0200U);
	CHECK(CPUX86_MODEL_PKG_C7 == 0x0400U);
	CHECK(CPUX86_MODEL_CORE_C3 == 0x0800U);
	CHECK(CPUX86_MODEL_CORE_C6 == 0x1000U);
	CHECK(CPUX86_MODEL_CORE_C7 == 0x2000U);
	CHECK(CPUX86_MODEL_MODULE_C6 == 0x4000U);

	/* The models next to the listed ones (0x99, 0x9B) are not covered. */
	make_identity(&identity, CPUX86_VENDOR_INTEL, 6U, 0x99U);
	registers = 0xDEADU;
	error = cpux86_model_registers(&identity, &registers);
	CHECK(error == ENOTSUP);
	CHECK(registers == 0U);
	make_identity(&identity, CPUX86_VENDOR_INTEL, 6U, 0x9BU);
	registers = 0xDEADU;
	error = cpux86_model_registers(&identity, &registers);
	CHECK(error == ENOTSUP);
	CHECK(registers == 0U);

	/* Family 0FH with a listed model number is not covered. */
	make_identity(&identity, CPUX86_VENDOR_INTEL, 0x0FU, 0x9AU);
	registers = 0xDEADU;
	error = cpux86_model_registers(&identity, &registers);
	CHECK(error == ENOTSUP);
	CHECK(registers == 0U);

	/* An unknown vendor with the listed family and model is not covered. */
	make_identity(&identity, CPUX86_VENDOR_UNKNOWN, 6U, 0x9AU);
	registers = 0xDEADU;
	error = cpux86_model_registers(&identity, &registers);
	CHECK(error == ENOTSUP);
	CHECK(registers == 0U);

	/* Exactly the five models are covered among all family 6 models of Intel. */
	for (model = 0U; model < 0x200U; model++) {
		make_identity(&identity, CPUX86_VENDOR_INTEL, 6U, model);
		registers = 0xDEADU;
		error = cpux86_model_registers(&identity, &registers);
		if (model == 0x97U || model == 0x9AU || model == 0xB7U || model == 0xBAU || model == 0xBFU) {
			CHECK(error == 0);
			CHECK(registers == 0x7FFFU);
		} else {
			CHECK(error == ENOTSUP);
			CHECK(registers == 0U);
		}
	}

	/* No other family is covered, whatever the model. */
	for (family = 0U; family < 0x110U; family++) {
		if (family == 6U)
			continue;
		make_identity(&identity, CPUX86_VENDOR_INTEL, family, 0x9AU);
		registers = 0xDEADU;
		error = cpux86_model_registers(&identity, &registers);
		CHECK(error == ENOTSUP);
		CHECK(registers == 0U);
	}

	/* NULL pointers are refused and leave the output alone. */
	make_identity(&identity, CPUX86_VENDOR_INTEL, 6U, 0x9AU);
	registers = 0xDEADU;
	error = cpux86_model_registers(NULL, &registers);
	CHECK(error == EINVAL);
	CHECK(registers == 0xDEADU);
	error = cpux86_model_registers(&identity, NULL);
	CHECK(error == EINVAL);
}

/*
 * Checks the RAPL units and the energy status decoders.
 */
static void
check_rapl_units_and_status(void)
{
	struct cpux86_rapl_units units;
	struct cpux86_rapl_units before;
	uint64_t raw;
	uint32_t count;
	uint32_t bit;
	uint32_t power;
	uint32_t energy;
	uint32_t time;
	int error;

	/* A hand-built value: power 3, energy 14, time 10. */
	raw = 0U;
	regbuild_put64(&raw, 0U, 4U, 3U);
	regbuild_put64(&raw, 8U, 5U, 14U);
	regbuild_put64(&raw, 16U, 4U, 10U);
	CHECK(raw == 0xA0E03U);
	error = cpux86_rapl_units_decode(raw, &units);
	CHECK(error == 0);
	CHECK(units.power_shift == 3U);
	CHECK(units.energy_shift == 14U);
	CHECK(units.time_shift == 10U);

	/* The ends of each field alone: 0 and the largest value. */
	error = cpux86_rapl_units_decode(0U, &units);
	CHECK(error == 0);
	CHECK(units.power_shift == 0U);
	CHECK(units.energy_shift == 0U);
	CHECK(units.time_shift == 0U);
	error = cpux86_rapl_units_decode(0xFU, &units);
	CHECK(error == 0);
	CHECK(units.power_shift == 15U);
	CHECK(units.energy_shift == 0U);
	CHECK(units.time_shift == 0U);
	error = cpux86_rapl_units_decode(0x1F00U, &units);
	CHECK(error == 0);
	CHECK(units.power_shift == 0U);
	CHECK(units.energy_shift == 31U);
	CHECK(units.time_shift == 0U);
	error = cpux86_rapl_units_decode(0xF0000U, &units);
	CHECK(error == 0);
	CHECK(units.power_shift == 0U);
	CHECK(units.energy_shift == 0U);
	CHECK(units.time_shift == 15U);

	/* Every combination of the three fields round-trips. */
	for (power = 0U; power < 16U; power++) {
		for (energy = 0U; energy < 32U; energy++) {
			for (time = 0U; time < 16U; time++) {
				raw = 0U;
				regbuild_put64(&raw, 0U, 4U, power);
				regbuild_put64(&raw, 8U, 5U, energy);
				regbuild_put64(&raw, 16U, 4U, time);
				error = cpux86_rapl_units_decode(raw, &units);
				CHECK(error == 0);
				CHECK(units.power_shift == power);
				CHECK(units.energy_shift == energy);
				CHECK(units.time_shift == time);
			}
		}
	}

	/* Each reserved bit alone (7:4, 15:13, 63:20) is refused and the output is unchanged. */
	for (bit = 0U; bit < 64U; bit++) {
		before.power_shift = 1U;
		before.energy_shift = 2U;
		before.time_shift = 3U;
		units = before;
		raw = (uint64_t)1U << bit;
		error = cpux86_rapl_units_decode(raw, &units);
		if ((bit >= 4U && bit < 8U) || (bit >= 13U && bit < 16U) || bit >= 20U) {
			CHECK(error == EINVAL);
			CHECK(units.power_shift == 1U);
			CHECK(units.energy_shift == 2U);
			CHECK(units.time_shift == 3U);
		} else {
			CHECK(error == 0);
		}
	}

	/* A valid register with a reserved bit set is refused. */
	raw = 0xA0E03U | ((uint64_t)1U << 63);
	error = cpux86_rapl_units_decode(raw, &units);
	CHECK(error == EINVAL);

	/* A NULL output is refused. */
	error = cpux86_rapl_units_decode(0U, NULL);
	CHECK(error == EINVAL);

	/* The energy status is the low 32 bits. */
	error = cpux86_energy_status_decode(0U, &count);
	CHECK(error == 0);
	CHECK(count == 0U);
	error = cpux86_energy_status_decode(0xFFFFFFFFU, &count);
	CHECK(error == 0);
	CHECK(count == 0xFFFFFFFFU);
	error = cpux86_energy_status_decode(0x12345678U, &count);
	CHECK(error == 0);
	CHECK(count == 0x12345678U);

	/* Bit 32 and above are reserved, each alone and together. */
	count = 77U;
	for (bit = 32U; bit < 64U; bit++) {
		error = cpux86_energy_status_decode((uint64_t)1U << bit, &count);
		CHECK(error == EINVAL);
		CHECK(count == 77U);
	}
	error = cpux86_energy_status_decode(((uint64_t)1U << 32) | 5U, &count);
	CHECK(error == EINVAL);
	CHECK(count == 77U);

	/* A NULL output is refused. */
	error = cpux86_energy_status_decode(0U, NULL);
	CHECK(error == EINVAL);
}

/*
 * Checks the 32-bit counter carried into 64 bits.
 */
static void
check_energy_counter(void)
{
	struct cpux86_energy_counter counter;
	struct cpux86_energy_counter before;
	int error;

	/* The first reading only records itself. */
	counter.total = 0U;
	counter.last = 0U;
	counter.started = 0U;
	error = cpux86_energy_counter_update(&counter, 1000U);
	CHECK(error == 0);
	CHECK(counter.total == 0U);
	CHECK(counter.last == 1000U);
	CHECK(counter.started == 1U);

	/* A larger reading adds the difference. */
	error = cpux86_energy_counter_update(&counter, 1500U);
	CHECK(error == 0);
	CHECK(counter.total == 500U);
	CHECK(counter.last == 1500U);

	/* The same reading adds nothing. */
	error = cpux86_energy_counter_update(&counter, 1500U);
	CHECK(error == 0);
	CHECK(counter.total == 500U);
	CHECK(counter.last == 1500U);

	/* A wrap: from 0xFFFFFFF0 to 0x10 is an advance of 0x20. */
	counter.total = 0U;
	counter.last = 0xFFFFFFF0U;
	counter.started = 1U;
	error = cpux86_energy_counter_update(&counter, 0x10U);
	CHECK(error == 0);
	CHECK(counter.total == 0x20U);
	CHECK(counter.last == 0x10U);

	/* The largest single advance, one less than a whole turn. */
	counter.total = 0U;
	counter.last = 5U;
	counter.started = 1U;
	error = cpux86_energy_counter_update(&counter, 4U);
	CHECK(error == 0);
	CHECK(counter.total == 0xFFFFFFFFU);
	CHECK(counter.last == 4U);

	/* A total that crosses 2^32 keeps growing in 64 bits. */
	counter.total = 0xFFFFFFFFU;
	counter.last = 0xFFFFFFFFU;
	counter.started = 1U;
	error = cpux86_energy_counter_update(&counter, 9U);
	CHECK(error == 0);
	CHECK(counter.total == (uint64_t)0xFFFFFFFFU + 10U);

	/* A total that would pass 2^64 - 1 is refused and nothing changes. */
	counter.total = MAX64 - 5U;
	counter.last = 100U;
	counter.started = 1U;
	before = counter;
	error = cpux86_energy_counter_update(&counter, 110U);
	CHECK(error == ERANGE);
	CHECK(counter.total == before.total);
	CHECK(counter.last == before.last);
	CHECK(counter.started == before.started);

	/* A total that exactly reaches 2^64 - 1 is accepted. */
	counter.total = MAX64 - 10U;
	counter.last = 100U;
	counter.started = 1U;
	error = cpux86_energy_counter_update(&counter, 110U);
	CHECK(error == 0);
	CHECK(counter.total == MAX64);
	CHECK(counter.last == 110U);

	/* A started value above 1 is refused and nothing changes. */
	counter.total = 3U;
	counter.last = 4U;
	counter.started = 2U;
	before = counter;
	error = cpux86_energy_counter_update(&counter, 9U);
	CHECK(error == EINVAL);
	CHECK(counter.total == before.total);
	CHECK(counter.last == before.last);
	CHECK(counter.started == before.started);
	counter.started = 0xFFFFFFFFU;
	error = cpux86_energy_counter_update(&counter, 9U);
	CHECK(error == EINVAL);
	CHECK(counter.started == 0xFFFFFFFFU);

	/* A NULL counter is refused. */
	error = cpux86_energy_counter_update(NULL, 9U);
	CHECK(error == EINVAL);
}

/*
 * Checks the energy-to-microjoule conversion.
 */
static void
check_energy_microjoules(void)
{
	uint64_t microjoules;
	uint64_t units;
	uint64_t limit;
	uint32_t shift;
	int error;

	/* Shift 0: one unit is one joule, so 10^6 microjoules. */
	error = cpux86_energy_microjoules(0U, 0U, &microjoules);
	CHECK(error == 0);
	CHECK(microjoules == 0U);
	error = cpux86_energy_microjoules(1U, 0U, &microjoules);
	CHECK(error == 0);
	CHECK(microjoules == 1000000U);
	error = cpux86_energy_microjoules(12345U, 0U, &microjoules);
	CHECK(error == 0);
	CHECK(microjoules == 12345000000U);

	/* Shift 14: one unit is 1000000/16384 = 61.03515625 microjoules, rounded down. */
	error = cpux86_energy_microjoules(1U, 14U, &microjoules);
	CHECK(error == 0);
	CHECK(microjoules == 61U);
	error = cpux86_energy_microjoules(16384U, 14U, &microjoules);
	CHECK(error == 0);
	CHECK(microjoules == 1000000U);
	error = cpux86_energy_microjoules(16385U, 14U, &microjoules);
	CHECK(error == 0);
	CHECK(microjoules == 1000061U);
	error = cpux86_energy_microjoules(100U, 14U, &microjoules);
	CHECK(error == 0);
	CHECK(microjoules == 6103U);

	/* Shift 16: 1000000/65536 = 15.2587890625 microjoules per unit. */
	error = cpux86_energy_microjoules(1U, 16U, &microjoules);
	CHECK(error == 0);
	CHECK(microjoules == 15U);
	error = cpux86_energy_microjoules(65536U, 16U, &microjoules);
	CHECK(error == 0);
	CHECK(microjoules == 1000000U);
	error = cpux86_energy_microjoules(3U, 16U, &microjoules);
	CHECK(error == 0);
	CHECK(microjoules == 45U);

	/* Shift 31: 1000000/2147483648 is below one, so one unit rounds to 0. */
	error = cpux86_energy_microjoules(1U, 31U, &microjoules);
	CHECK(error == 0);
	CHECK(microjoules == 0U);
	error = cpux86_energy_microjoules((uint64_t)1U << 31, 31U, &microjoules);
	CHECK(error == 0);
	CHECK(microjoules == 1000000U);
	error = cpux86_energy_microjoules(2147483U, 31U, &microjoules);
	CHECK(error == 0);
	CHECK(microjoules == 999U);

	/* Shift 32 and more are refused and leave the output alone. */
	microjoules = 55U;
	error = cpux86_energy_microjoules(1U, 32U, &microjoules);
	CHECK(error == EINVAL);
	CHECK(microjoules == 55U);
	error = cpux86_energy_microjoules(1U, 0xFFFFFFFFU, &microjoules);
	CHECK(error == EINVAL);
	CHECK(microjoules == 55U);

	/* A NULL output is refused. */
	error = cpux86_energy_microjoules(1U, 14U, NULL);
	CHECK(error == EINVAL);

	/*
	 * The largest whole-joule count whose microjoules fit 64 bits is
	 * floor((2^64 - 1) / 10^6) = 18446744073709 = 0x10C6F7A0B5ED, whose
	 * product is 18446744073709000000, leaving 551615 below 2^64.
	 */
	limit = ((uint64_t)0x10C6U << 32) | 0xF7A0B5EDU;
	CHECK(limit == 18446744073709U);

	/* Shift 0: that count converts and one more is refused. */
	microjoules = 55U;
	error = cpux86_energy_microjoules(limit, 0U, &microjoules);
	CHECK(error == 0);
	CHECK(microjoules == limit * 1000000U);
	microjoules = 55U;
	error = cpux86_energy_microjoules(limit + 1U, 0U, &microjoules);
	CHECK(error == ERANGE);
	CHECK(microjoules == 55U);
	error = cpux86_energy_microjoules(MAX64, 0U, &microjoules);
	CHECK(error == ERANGE);
	CHECK(microjoules == 55U);

	/*
	 * Shift 14: units = limit * 2^14 + r has the whole part limit and the
	 * part below a joule r * 10^6 / 2^14.  With r = 9037 that is 551574.46,
	 * so the sum 18446744073709551574 still fits; r = 9038 gives 551635.5,
	 * and the sum passes 2^64 - 1, so it is refused.
	 */
	units = (limit << 14) + 9037U;
	error = cpux86_energy_microjoules(units, 14U, &microjoules);
	CHECK(error == 0);
	CHECK(microjoules == limit * 1000000U + 551574U);
	microjoules = 55U;
	error = cpux86_energy_microjoules(units + 1U, 14U, &microjoules);
	CHECK(error == ERANGE);
	CHECK(microjoules == 55U);

	/*
	 * Shift 31: the whole part of the largest units is 2^33 - 1 =
	 * 8589934591 and the remainder 2^31 - 1 gives 999999 microjoules below
	 * a joule, so the sum is 8589934591999999 and fits.
	 */
	error = cpux86_energy_microjoules(MAX64, 31U, &microjoules);
	CHECK(error == 0);
	CHECK(microjoules == 8589934591U * 1000000U + 999999U);

	/*
	 * Shift 20: the whole part of the largest units is 2^44 - 1 =
	 * 17592186044415 (0xFFFFFFFFFFF) and the remainder 2^20 - 1 gives
	 * 999999 microjoules below a joule.
	 */
	error = cpux86_energy_microjoules(MAX64, 20U, &microjoules);
	CHECK(error == 0);
	CHECK(microjoules == (((uint64_t)0xFFFU << 32) | 0xFFFFFFFFU) * 1000000U + 999999U);

	/*
	 * The largest input is refused for the shifts 0..19 (the whole part
	 * reaches 2^45 - 1 or more, which is above 18446744073709) and converted
	 * for the shifts 20..31.
	 */
	for (shift = 0U; shift < 32U; shift++) {
		microjoules = 55U;
		error = cpux86_energy_microjoules(MAX64, shift, &microjoules);
		if (shift < 20U) {
			CHECK(error == ERANGE);
			CHECK(microjoules == 55U);
		} else {
			CHECK(error == 0);
			CHECK(microjoules >= (MAX64 >> shift) * 1000000U);
		}
	}
}

/*
 * Checks the residency table.
 */
static void
check_residency_info(void)
{
	struct cpux86_residency_info info;
	struct cpux86_residency_info before;
	uint32_t counter;
	int error;

	/* PKG C2: 60DH, package, C2, TSC rate. */
	error = cpux86_residency_info(CPUX86_RESIDENCY_PKG_C2, &info);
	CHECK(error == 0);
	CHECK(info.msr == 0x60DU);
	CHECK(info.model_bit == CPUX86_MODEL_PKG_C2);
	CHECK(info.scope == CPUX86_SCOPE_PACKAGE);
	CHECK(info.state == 2U);
	CHECK(info.rate == CPUX86_RATE_TSC);

	/* PKG C3: 3F8H. */
	error = cpux86_residency_info(CPUX86_RESIDENCY_PKG_C3, &info);
	CHECK(error == 0);
	CHECK(info.msr == 0x3F8U);
	CHECK(info.model_bit == CPUX86_MODEL_PKG_C3);
	CHECK(info.scope == CPUX86_SCOPE_PACKAGE);
	CHECK(info.state == 3U);
	CHECK(info.rate == CPUX86_RATE_TSC);

	/* PKG C6: 3F9H. */
	error = cpux86_residency_info(CPUX86_RESIDENCY_PKG_C6, &info);
	CHECK(error == 0);
	CHECK(info.msr == 0x3F9U);
	CHECK(info.model_bit == CPUX86_MODEL_PKG_C6);
	CHECK(info.scope == CPUX86_SCOPE_PACKAGE);
	CHECK(info.state == 6U);
	CHECK(info.rate == CPUX86_RATE_TSC);

	/* PKG C7: 3FAH. */
	error = cpux86_residency_info(CPUX86_RESIDENCY_PKG_C7, &info);
	CHECK(error == 0);
	CHECK(info.msr == 0x3FAU);
	CHECK(info.model_bit == CPUX86_MODEL_PKG_C7);
	CHECK(info.scope == CPUX86_SCOPE_PACKAGE);
	CHECK(info.state == 7U);
	CHECK(info.rate == CPUX86_RATE_TSC);

	/* CORE C3: 3FCH, core scope. */
	error = cpux86_residency_info(CPUX86_RESIDENCY_CORE_C3, &info);
	CHECK(error == 0);
	CHECK(info.msr == 0x3FCU);
	CHECK(info.model_bit == CPUX86_MODEL_CORE_C3);
	CHECK(info.scope == CPUX86_SCOPE_CORE);
	CHECK(info.state == 3U);
	CHECK(info.rate == CPUX86_RATE_TSC);

	/* CORE C6: 3FDH. */
	error = cpux86_residency_info(CPUX86_RESIDENCY_CORE_C6, &info);
	CHECK(error == 0);
	CHECK(info.msr == 0x3FDU);
	CHECK(info.model_bit == CPUX86_MODEL_CORE_C6);
	CHECK(info.scope == CPUX86_SCOPE_CORE);
	CHECK(info.state == 6U);
	CHECK(info.rate == CPUX86_RATE_TSC);

	/* CORE C7: 3FEH. */
	error = cpux86_residency_info(CPUX86_RESIDENCY_CORE_C7, &info);
	CHECK(error == 0);
	CHECK(info.msr == 0x3FEU);
	CHECK(info.model_bit == CPUX86_MODEL_CORE_C7);
	CHECK(info.scope == CPUX86_SCOPE_CORE);
	CHECK(info.state == 7U);
	CHECK(info.rate == CPUX86_RATE_TSC);

	/* Module C6: 664H, module scope, 1 MHz. */
	error = cpux86_residency_info(CPUX86_RESIDENCY_MODULE_C6, &info);
	CHECK(error == 0);
	CHECK(info.msr == 0x664U);
	CHECK(info.model_bit == CPUX86_MODEL_MODULE_C6);
	CHECK(info.scope == CPUX86_SCOPE_MODULE);
	CHECK(info.state == 6U);
	CHECK(info.rate == CPUX86_RATE_1MHZ);

	/* The identifiers are 0..7 and the count is 8. */
	CHECK(CPUX86_RESIDENCY_COUNT == 8U);
	CHECK(CPUX86_RESIDENCY_PKG_C2 == 0U);
	CHECK(CPUX86_RESIDENCY_MODULE_C6 == 7U);

	/* An identifier at or above the count is refused and leaves the output alone. */
	before.msr = 1U;
	before.model_bit = 2U;
	before.scope = 3U;
	before.state = 4U;
	before.rate = 5U;
	for (counter = 8U; counter < 20U; counter++) {
		info = before;
		error = cpux86_residency_info(counter, &info);
		CHECK(error == EINVAL);
		CHECK(info.msr == 1U);
		CHECK(info.model_bit == 2U);
		CHECK(info.scope == 3U);
		CHECK(info.state == 4U);
		CHECK(info.rate == 5U);
	}
	info = before;
	error = cpux86_residency_info(0xFFFFFFFFU, &info);
	CHECK(error == EINVAL);
	CHECK(info.msr == 1U);

	/* A NULL output is refused. */
	error = cpux86_residency_info(0U, NULL);
	CHECK(error == EINVAL);

	/* Every counter's allow list bit is one single bit and every counter is distinct. */
	for (counter = 0U; counter < 8U; counter++) {
		error = cpux86_residency_info(counter, &info);
		CHECK(error == 0);
		CHECK(info.model_bit != 0U);
		CHECK((info.model_bit & (info.model_bit - 1U)) == 0U);
	}
}

/*
 * Checks the residency delta and the nanosecond conversion.
 */
static void
check_residency_arithmetic(void)
{
	uint64_t delta;
	uint64_t nanoseconds;
	uint64_t counts;
	int error;

	/* An increase gives the difference. */
	error = cpux86_residency_delta(100U, 350U, &delta);
	CHECK(error == 0);
	CHECK(delta == 250U);

	/* The same reading gives 0. */
	error = cpux86_residency_delta(MAX64, MAX64, &delta);
	CHECK(error == 0);
	CHECK(delta == 0U);

	/* The widest advance is accepted. */
	error = cpux86_residency_delta(0U, MAX64, &delta);
	CHECK(error == 0);
	CHECK(delta == MAX64);

	/* A decrease is refused and leaves the output alone. */
	delta = 9U;
	error = cpux86_residency_delta(350U, 100U, &delta);
	CHECK(error == ERANGE);
	CHECK(delta == 9U);
	error = cpux86_residency_delta(MAX64, 0U, &delta);
	CHECK(error == ERANGE);
	CHECK(delta == 9U);

	/* A NULL output is refused. */
	error = cpux86_residency_delta(1U, 2U, NULL);
	CHECK(error == EINVAL);

	/* At 1 MHz one count is one microsecond: 1000 ns. */
	error = cpux86_residency_nanoseconds(1U, 1000000U, &nanoseconds);
	CHECK(error == 0);
	CHECK(nanoseconds == 1000U);
	error = cpux86_residency_nanoseconds(0U, 1000000U, &nanoseconds);
	CHECK(error == 0);
	CHECK(nanoseconds == 0U);

	/* At a 2.5 GHz time-stamp counter, 2.5 * 10^9 counts is one second. */
	error = cpux86_residency_nanoseconds(2500000000U, 2500000000U, &nanoseconds);
	CHECK(error == 0);
	CHECK(nanoseconds == 1000000000U);

	/* At 2.5 GHz, one count is 0.4 ns, which rounds down to 0, and 3 counts are 1.2 ns, 1. */
	error = cpux86_residency_nanoseconds(1U, 2500000000U, &nanoseconds);
	CHECK(error == 0);
	CHECK(nanoseconds == 0U);
	error = cpux86_residency_nanoseconds(3U, 2500000000U, &nanoseconds);
	CHECK(error == 0);
	CHECK(nanoseconds == 1U);

	/* A rate of 0 is refused. */
	nanoseconds = 8U;
	error = cpux86_residency_nanoseconds(1U, 0U, &nanoseconds);
	CHECK(error == EINVAL);
	CHECK(nanoseconds == 8U);

	/* A rate above 10^10 is refused, and 10^10 itself is accepted. */
	error = cpux86_residency_nanoseconds(1U, 10000000001U, &nanoseconds);
	CHECK(error == EINVAL);
	CHECK(nanoseconds == 8U);
	error = cpux86_residency_nanoseconds(1U, MAX64, &nanoseconds);
	CHECK(error == EINVAL);
	CHECK(nanoseconds == 8U);
	error = cpux86_residency_nanoseconds(10000000000U, 10000000000U, &nanoseconds);
	CHECK(error == 0);
	CHECK(nanoseconds == 1000000000U);

	/* At 10 GHz, one count is 0.1 ns and 15 counts are 1.5 ns, 1. */
	error = cpux86_residency_nanoseconds(15U, 10000000000U, &nanoseconds);
	CHECK(error == 0);
	CHECK(nanoseconds == 1U);

	/* A NULL output is refused. */
	error = cpux86_residency_nanoseconds(1U, 1000000U, NULL);
	CHECK(error == EINVAL);

	/*
	 * At 1 MHz the largest counts that fit are floor((2^64 - 1) / 1000) =
	 * 18446744073709551; its nanoseconds are 18446744073709551000, and one
	 * more count is 18446744073709552000, above 2^64 - 1.
	 */
	counts = ((uint64_t)0x00418937U << 32) | 0x4BC6A7EFU;
	CHECK(counts == 18446744073709551U);
	nanoseconds = 8U;
	error = cpux86_residency_nanoseconds(counts, 1000000U, &nanoseconds);
	CHECK(error == 0);
	CHECK(nanoseconds == counts * 1000U);
	nanoseconds = 8U;
	error = cpux86_residency_nanoseconds(counts + 1U, 1000000U, &nanoseconds);
	CHECK(error == ERANGE);
	CHECK(nanoseconds == 8U);
}
/*
 * Checks the package C-state register decoder and the limit encoder.
 */
static void
check_pkg_cstate(void)
{
	struct cpux86_pkg_cstate config;
	struct cpux86_pkg_cstate before;
	uint64_t raw;
	uint64_t result;
	uint32_t limit;
	uint32_t bit;
	uint32_t expected;
	int error;

	/* Every defined limit code 0..8 decodes to itself. */
	for (limit = 0U; limit < 9U; limit++) {
		error = cpux86_pkg_cstate_decode(limit, &config);
		CHECK(error == 0);
		CHECK(config.limit == limit);
		CHECK(config.flags == 0U);
	}

	/* The names are the codes of Table 2-35. */
	CHECK(CPUX86_PKG_LIMIT_C1 == 0U);
	CHECK(CPUX86_PKG_LIMIT_C2 == 1U);
	CHECK(CPUX86_PKG_LIMIT_C3 == 2U);
	CHECK(CPUX86_PKG_LIMIT_C6 == 3U);
	CHECK(CPUX86_PKG_LIMIT_C7 == 4U);
	CHECK(CPUX86_PKG_LIMIT_C7S == 5U);
	CHECK(CPUX86_PKG_LIMIT_C8 == 6U);
	CHECK(CPUX86_PKG_LIMIT_C9 == 7U);
	CHECK(CPUX86_PKG_LIMIT_C10 == 8U);

	/* The reserved codes 9..15 are refused and leave the output alone. */
	for (limit = 9U; limit < 16U; limit++) {
		before.limit = 99U;
		before.flags = 98U;
		config = before;
		error = cpux86_pkg_cstate_decode(limit, &config);
		CHECK(error == EINVAL);
		CHECK(config.limit == 99U);
		CHECK(config.flags == 98U);
	}

	/* Each flag bit alone: 10, 15 and 25..30. */
	error = cpux86_pkg_cstate_decode((uint64_t)1U << 10, &config);
	CHECK(error == 0);
	CHECK(config.flags == CPUX86_PKG_CST_IO_MWAIT);
	error = cpux86_pkg_cstate_decode((uint64_t)1U << 15, &config);
	CHECK(error == 0);
	CHECK(config.flags == CPUX86_PKG_CST_LOCKED);
	error = cpux86_pkg_cstate_decode((uint64_t)1U << 25, &config);
	CHECK(error == 0);
	CHECK(config.flags == CPUX86_PKG_CST_C3_DEMOTION);
	error = cpux86_pkg_cstate_decode((uint64_t)1U << 26, &config);
	CHECK(error == 0);
	CHECK(config.flags == CPUX86_PKG_CST_C1_DEMOTION);
	error = cpux86_pkg_cstate_decode((uint64_t)1U << 27, &config);
	CHECK(error == 0);
	CHECK(config.flags == CPUX86_PKG_CST_C3_UNDEMOTION);
	error = cpux86_pkg_cstate_decode((uint64_t)1U << 28, &config);
	CHECK(error == 0);
	CHECK(config.flags == CPUX86_PKG_CST_C1_UNDEMOTION);
	error = cpux86_pkg_cstate_decode((uint64_t)1U << 29, &config);
	CHECK(error == 0);
	CHECK(config.flags == CPUX86_PKG_CST_PKG_DEMOTION);
	error = cpux86_pkg_cstate_decode((uint64_t)1U << 30, &config);
	CHECK(error == 0);
	CHECK(config.flags == CPUX86_PKG_CST_PKG_UNDEMOTION);

	/* The flag values are eight distinct single bits 0x01 .. 0x80. */
	CHECK(CPUX86_PKG_CST_IO_MWAIT == 0x01U);
	CHECK(CPUX86_PKG_CST_LOCKED == 0x02U);
	CHECK(CPUX86_PKG_CST_C3_DEMOTION == 0x04U);
	CHECK(CPUX86_PKG_CST_C1_DEMOTION == 0x08U);
	CHECK(CPUX86_PKG_CST_C3_UNDEMOTION == 0x10U);
	CHECK(CPUX86_PKG_CST_C1_UNDEMOTION == 0x20U);
	CHECK(CPUX86_PKG_CST_PKG_DEMOTION == 0x40U);
	CHECK(CPUX86_PKG_CST_PKG_UNDEMOTION == 0x80U);

	/* All eight flags together with limit C8 (6). */
	raw = 6U | (1U << 10) | (1U << 15) | (1U << 25) | (1U << 26) | (1U << 27) | (1U << 28) | (1U << 29) | (1U << 30);
	error = cpux86_pkg_cstate_decode(raw, &config);
	CHECK(error == 0);
	CHECK(config.limit == 6U);
	CHECK(config.flags == 0xFFU);

	/* The reserved bits are not looked at: every one of them set decodes cleanly. */
	raw = 3U;
	for (bit = 4U; bit < 64U; bit++) {
		if (bit == 10U || bit == 15U || (bit >= 25U && bit <= 30U))
			continue;
		raw |= (uint64_t)1U << bit;
	}
	error = cpux86_pkg_cstate_decode(raw, &config);
	CHECK(error == 0);
	CHECK(config.limit == 3U);
	CHECK(config.flags == 0U);

	/* A NULL output is refused. */
	error = cpux86_pkg_cstate_decode(0U, NULL);
	CHECK(error == EINVAL);

	/* The encoder replaces bits 3:0 and keeps every other bit of the value read. */
	raw = ((uint64_t)0xA5A5A5A5U << 32) | 0x12345670U;
	for (limit = 0U; limit < 9U; limit++) {
		result = 0U;
		error = cpux86_pkg_cstate_limit_encode(raw, limit, &result);
		CHECK(error == 0);
		CHECK((result & ~(uint64_t)0xFU) == (raw & ~(uint64_t)0xFU));
		CHECK((result & 0xFU) == limit);
	}

	/* The old limit bits do not leak into the new one. */
	raw = ((uint64_t)0x00000001U << 32) | 0x0000000FU;
	error = cpux86_pkg_cstate_limit_encode(raw, 2U, &result);
	CHECK(error == 0);
	CHECK(result == (((uint64_t)1U << 32) | 2U));

	/* A reserved limit code is refused and leaves the output alone. */
	result = 0x77U;
	for (limit = 9U; limit < 17U; limit++) {
		error = cpux86_pkg_cstate_limit_encode(0U, limit, &result);
		CHECK(error == EINVAL);
		CHECK(result == 0x77U);
	}
	error = cpux86_pkg_cstate_limit_encode(0U, 0xFFFFFFFFU, &result);
	CHECK(error == EINVAL);
	CHECK(result == 0x77U);

	/* A locked register (bit 15) is refused with EACCES and the output is unchanged. */
	raw = (uint64_t)1U << 15;
	for (limit = 0U; limit < 9U; limit++) {
		result = 0x77U;
		error = cpux86_pkg_cstate_limit_encode(raw | 3U, limit, &result);
		CHECK(error == EACCES);
		CHECK(result == 0x77U);
	}

	/* A NULL output is refused. */
	error = cpux86_pkg_cstate_limit_encode(0U, 1U, NULL);
	CHECK(error == EINVAL);

	/* Decoding the encoded value gives back the limit and keeps the flags. */
	raw = (1U << 10) | (1U << 25);
	for (limit = 0U; limit < 9U; limit++) {
		error = cpux86_pkg_cstate_limit_encode(raw, limit, &result);
		CHECK(error == 0);
		error = cpux86_pkg_cstate_decode(result, &config);
		CHECK(error == 0);
		CHECK(config.limit == limit);
		expected = CPUX86_PKG_CST_IO_MWAIT | CPUX86_PKG_CST_C3_DEMOTION;
		CHECK(config.flags == expected);
	}
}

/*
 * Runs every function on random values and checks each answer is one the
 * headers allow.
 */
static void
check_random(void)
{
	struct cpux86_identity identity;
	struct cpux86_rapl_units units;
	struct cpux86_residency_info info;
	struct cpux86_pkg_cstate config;
	struct cpux86_energy_counter counter;
	uint64_t raw;
	uint64_t other;
	uint64_t out;
	uint32_t registers;
	uint32_t count;
	uint32_t round;
	int error;

	/* Starts the counter from nothing. */
	counter.total = 0U;
	counter.last = 0U;
	counter.started = 0U;

	/* Feeds many fixed-seed values to every function. */
	for (round = 0U; round < 20000U; round++) {
		raw = next_random();
		other = next_random();

		/* The identity is any mix of vendor, family and model. */
		identity.vendor = (uint32_t)(raw & 3U);
		identity.max_basic_leaf = (uint32_t)(raw >> 8);
		identity.max_extended_leaf = (uint32_t)(raw >> 16);
		identity.display_family = (uint32_t)(raw >> 24) & 0x1FFU;
		identity.display_model = (uint32_t)(other >> 8) & 0x3FFU;
		identity.stepping = (uint32_t)(raw >> 40);
		identity.processor_type = (uint32_t)(raw >> 48);
		registers = 0xDEADU;
		error = cpux86_model_registers(&identity, &registers);
		CHECK(error == 0 || error == ENOTSUP);
		if (error == 0)
			CHECK(registers == 0x7FFFU);
		else
			CHECK(registers == 0U);

		/* The units decode to shifts within their fields, or are refused. */
		error = cpux86_rapl_units_decode(raw, &units);
		CHECK(error == 0 || error == EINVAL);
		if (error == 0) {
			CHECK(units.power_shift < 16U);
			CHECK(units.energy_shift < 32U);
			CHECK(units.time_shift < 16U);
		}

		/* The energy status keeps the low half or is refused. */
		error = cpux86_energy_status_decode(raw, &count);
		CHECK(error == 0 || error == EINVAL);
		if (error == 0)
			CHECK(count == (uint32_t)(raw & 0xFFFFFFFFU));

		/* The counter only moves forward or is refused. */
		error = cpux86_energy_counter_update(&counter, (uint32_t)other);
		CHECK(error == 0 || error == ERANGE);

		/* The microjoule conversion either converts or reports why not. */
		error = cpux86_energy_microjoules(raw, (uint32_t)other & 0x3FU, &out);
		CHECK(error == 0 || error == EINVAL || error == ERANGE);

		/* The residency table answers for 16 identifiers, 8 of them valid. */
		error = cpux86_residency_info((uint32_t)raw & 0xFU, &info);
		CHECK(error == 0 || error == EINVAL);

		/* The delta is the difference or a refusal of a decrease. */
		error = cpux86_residency_delta(raw, other, &out);
		if (other < raw)
			CHECK(error == ERANGE);
		else
			CHECK(error == 0 && out == other - raw);

		/* The nanosecond conversion answers within its three errors. */
		error = cpux86_residency_nanoseconds(raw, (other >> 20), &out);
		CHECK(error == 0 || error == EINVAL || error == ERANGE);
		error = cpux86_residency_nanoseconds(raw >> 20, (other >> 30), &out);
		CHECK(error == 0 || error == EINVAL || error == ERANGE);

		/* The package C-state decode and encode answer within their errors. */
		error = cpux86_pkg_cstate_decode(raw, &config);
		CHECK(error == 0 || error == EINVAL);
		error = cpux86_pkg_cstate_limit_encode(raw, (uint32_t)other & 0x1FU, &out);
		CHECK(error == 0 || error == EINVAL || error == EACCES);
	}
}

/*
 * Runs the power register checks.
 */
int
main(void)
{
	/* The allow list. */
	check_model_registers();

	/* The RAPL units and energy status. */
	check_rapl_units_and_status();

	/* The 32-bit energy counter. */
	check_energy_counter();

	/* The microjoule conversion. */
	check_energy_microjoules();

	/* The residency table. */
	check_residency_info();

	/* The residency delta and nanosecond conversion. */
	check_residency_arithmetic();

	/* The package C-state register. */
	check_pkg_cstate();

	/* Random inputs. */
	check_random();

	/* Reports the result. */
	return check_finish("power");
}
