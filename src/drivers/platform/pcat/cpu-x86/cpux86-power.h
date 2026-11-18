/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The decoder of the model-specific power registers the driver reads: the
 * list of those registers each known model has, the units and energy
 * counters of running average power limit (RAPL) reporting, the C-state
 * residency counters, and the package C-state limit register.
 *
 * A model-specific register that a model does not have faults when it is
 * read, so the driver reads one only when cpux86_model_registers() names
 * it for the processor at hand.
 */

#ifndef DRIVERS_PLATFORM_PCAT_CPU_X86_CPUX86_POWER_H
#define DRIVERS_PLATFORM_PCAT_CPU_X86_CPUX86_POWER_H

#include "cpux86.h"
#include "cpux86-cpuid.h"

/*
 * The model-specific registers the allow list can name, as bits of the
 * value cpux86_model_registers() reports.
 *
 * Values referenced: Intel SDM Vol. 4 rev. 093, the register addresses of
 * Table 2-20 (E2H, 606H, 611H, 60DH, 3F8H .. 3FEH), Table 2-21 (641H),
 * Table 2-29 (CEH), Table 2-35 (E2H), Table 2-39 (639H, 64DH) and
 * Table 2-46 (664H).
 */
#define CPUX86_MODEL_PLATFORM_INFO	0x0001U	/* CEH, cpux86-pstate.h */
#define CPUX86_MODEL_PKG_CST_CONFIG	0x0002U	/* E2H */
#define CPUX86_MODEL_RAPL_UNIT		0x0004U	/* 606H */
#define CPUX86_MODEL_PKG_ENERGY		0x0008U	/* 611H */
#define CPUX86_MODEL_PP0_ENERGY		0x0010U	/* 639H, the cores */
#define CPUX86_MODEL_PP1_ENERGY		0x0020U	/* 641H, a client uncore device */
#define CPUX86_MODEL_PLATFORM_ENERGY	0x0040U	/* 64DH */
#define CPUX86_MODEL_PKG_C2		0x0080U	/* 60DH */
#define CPUX86_MODEL_PKG_C3		0x0100U	/* 3F8H */
#define CPUX86_MODEL_PKG_C6		0x0200U	/* 3F9H */
#define CPUX86_MODEL_PKG_C7		0x0400U	/* 3FAH */
#define CPUX86_MODEL_CORE_C3		0x0800U	/* 3FCH */
#define CPUX86_MODEL_CORE_C6		0x1000U	/* 3FDH */
#define CPUX86_MODEL_CORE_C7		0x2000U	/* 3FEH */
#define CPUX86_MODEL_MODULE_C6		0x4000U	/* 664H */

/*
 * Reports which of the registers above a processor model has.
 *
 * The list covers the models whose register tables were read for it: the
 * 12th and 13th generation Intel Core processors with the performance
 * hybrid architecture (DisplayFamily_DisplayModel 06_97H, 06_9AH, 06_B7H,
 * 06_BAH and 06_BFH), which take their registers from Vol. 4 Tables 2-20,
 * 2-21, 2-25, 2-29, 2-35 and 2-39 (§2.17) and Tables 2-44 to 2-48
 * (§2.17.5).  Any other processor is not covered.  The DRAM energy
 * register (619H) of Table 2-29 is left out: the client RAPL hierarchy has
 * no DRAM domain (Vol. 3B §17.10.2).
 * Input: identity, from cpux86_cpuid_identity().
 * Output: *registers, the CPUX86_MODEL_* bits; 0 on ENOTSUP.
 * Errors: EINVAL for a NULL pointer; ENOTSUP for a vendor, family or
 * model the list does not cover.
 * Specification: Intel SDM Vol. 4 rev. 093, §2.17 (the paragraph before
 * Table 2-39), §2.17.5 (the paragraph before Table 2-46), the register
 * entries of the tables named above; Vol. 3B rev. 093, §17.10.2.
 */
int
cpux86_model_registers(
	const struct cpux86_identity *identity,
	uint32_t *registers);

/*
 * The RAPL registers.
 *
 * Values referenced: Intel SDM Vol. 4 rev. 093, Table 2-20 (606H, 611H),
 * Table 2-21 (641H), Table 2-39 (639H, 64DH).
 */
#define CPUX86_MSR_RAPL_POWER_UNIT	0x606U
#define CPUX86_MSR_PKG_ENERGY_STATUS	0x611U
#define CPUX86_MSR_PP0_ENERGY_STATUS	0x639U
#define CPUX86_MSR_PP1_ENERGY_STATUS	0x641U
#define CPUX86_MSR_PLATFORM_ENERGY	0x64DU

/*
 * The units of all RAPL domains, each a power of one half: a power field
 * counts 1/2^power_shift W, an energy counter 1/2^energy_shift J, a time
 * field 1/2^time_shift s.
 */
struct cpux86_rapl_units {
	uint32_t power_shift;		/* bits 3:0 */
	uint32_t energy_shift;		/* bits 12:8 */
	uint32_t time_shift;		/* bits 19:16 */
};

/*
 * Decodes MSR_RAPL_POWER_UNIT.
 *
 * Input: raw, the register as read.
 * Output: *units.
 * Errors: EINVAL for a NULL pointer or a set reserved bit (7:4, 15:13,
 * 63:20); *units unchanged.
 * Specification: Intel SDM Vol. 3B rev. 093, §17.10.1, Figure 17-35.
 */
int
cpux86_rapl_units_decode(
	uint64_t raw,
	struct cpux86_rapl_units *units);

/*
 * Decodes one energy status register (package, PP0, PP1 or platform).
 *
 * Input: raw, the register as read.
 * Output: *count, bits 31:0, the energy consumed since the counter was
 * last cleared, in units of 1/2^energy_shift J.  The counter wraps at
 * 2^32.  The platform counter reads 0 when the platform does not support
 * it.
 * Errors: EINVAL for a NULL pointer or a set reserved bit (63:32).
 * Specification: Intel SDM Vol. 3B rev. 093, §17.10.3 (Figure 17-37) and
 * §17.10.4; Vol. 4 rev. 093, Table 2-39 (MSR_PLATFORM_ENERGY_COUNTER).
 */
int
cpux86_energy_status_decode(
	uint64_t raw,
	uint32_t *count);

/*
 * A 32-bit energy counter carried into 64 bits.
 *
 * The owner updates it with every reading; a reading taken after the
 * hardware counter advanced by 2^32 units or more since the previous one
 * loses the whole turns, which the counter cannot see.  The hardware
 * counter of the package can turn in about 60 seconds under high power.
 * Starts zeroed; the first update only records the reading.
 */
struct cpux86_energy_counter {
	uint64_t total;			/* units accumulated since the first reading */
	uint32_t last;			/* the previous reading */
	uint32_t started;		/* 0 until the first reading, then 1 */
};

/*
 * Adds a reading to an energy counter.
 *
 * Input: counter, a counter that is zeroed or was updated before; reading,
 * the 32-bit count of cpux86_energy_status_decode().
 * Output: on the first reading, started becomes 1 and last the reading,
 * total unchanged.  After that, total grows by (reading - last) modulo
 * 2^32, which is the advance including one wrap, and last becomes the
 * reading.
 * Errors, counter unchanged: EINVAL for a NULL pointer or started above 1;
 * ERANGE when total would pass 2^64 - 1.
 * Specification: Intel SDM Vol. 3B rev. 093, §17.10.3 (the 32-bit total
 * and its wraparound).
 */
int
cpux86_energy_counter_update(
	struct cpux86_energy_counter *counter,
	uint32_t reading);

/*
 * Converts energy units to microjoules.
 *
 * Input: units, a count of 1/2^energy_shift J; energy_shift, 0..31.
 * Output: *microjoules, units * 10^6 / 2^energy_shift rounded down,
 * computed without an intermediate wider than 64 bits.
 * Errors: EINVAL for a NULL pointer or energy_shift above 31; ERANGE when
 * the result does not fit 64 bits; *microjoules unchanged.
 * Specification: Intel SDM Vol. 3B rev. 093, §17.10.1 (Energy Status
 * Units).
 */
int
cpux86_energy_microjoules(
	uint64_t units,
	uint32_t energy_shift,
	uint64_t *microjoules);

/*
 * The C-state residency counters, by the identifier the driver asks
 * cpux86_residency_info() about.
 */
#define CPUX86_RESIDENCY_PKG_C2		0U
#define CPUX86_RESIDENCY_PKG_C3		1U
#define CPUX86_RESIDENCY_PKG_C6		2U
#define CPUX86_RESIDENCY_PKG_C7		3U
#define CPUX86_RESIDENCY_CORE_C3	4U
#define CPUX86_RESIDENCY_CORE_C6	5U
#define CPUX86_RESIDENCY_CORE_C7	6U
#define CPUX86_RESIDENCY_MODULE_C6	7U
#define CPUX86_RESIDENCY_COUNT		8U

/*
 * What a residency counter covers.
 *
 * Values referenced: Intel SDM Vol. 4 rev. 093, the Scope column of
 * Table 2-20 and Table 2-46.
 */
#define CPUX86_SCOPE_PACKAGE		1U
#define CPUX86_SCOPE_MODULE		2U
#define CPUX86_SCOPE_CORE		3U

/*
 * What a residency counter counts at.
 *
 * Values referenced: Intel SDM Vol. 4 rev. 093, Table 2-20 ("Count at the
 * same frequency as the TSC") and Table 2-46 ("Counts at 1 Mhz
 * frequency").
 */
#define CPUX86_RATE_TSC			1U	/* the time-stamp counter's frequency */
#define CPUX86_RATE_1MHZ		2U	/* one count per microsecond */

/*
 * Where one residency counter is and how to read it.
 *
 * Each counter is 64 bits wide and counts the time its package, module or
 * core spent in the processor-specific C-state named by state since the
 * last reset.
 */
struct cpux86_residency_info {
	uint32_t msr;			/* the register address */
	uint32_t model_bit;		/* the CPUX86_MODEL_* bit that allows it */
	uint32_t scope;			/* CPUX86_SCOPE_* */
	uint32_t state;			/* n of Cn, the processor's own numbering */
	uint32_t rate;			/* CPUX86_RATE_* */
};

/*
 * Reports where a residency counter is and how it counts.
 *
 * Input: counter, a CPUX86_RESIDENCY_* identifier.
 * Output: *info.
 * Errors: EINVAL for a NULL pointer or an identifier at or above
 * CPUX86_RESIDENCY_COUNT.
 * Specification: Intel SDM Vol. 4 rev. 093, Table 2-20 (60DH, 3F8H,
 * 3F9H, 3FAH, 3FCH, 3FDH, 3FEH: counted at the TSC's frequency) and
 * Table 2-46 (664H, MSR_MC6_RESIDENCY_COUNTER: module scope, 1 MHz).
 */
int
cpux86_residency_info(
	uint32_t counter,
	struct cpux86_residency_info *info);

/*
 * Reports how far a residency counter advanced between two readings.
 *
 * Input: before and after, two readings of one counter on the same
 * package, module or core.
 * Output: *delta, after - before.
 * Errors: EINVAL for a NULL pointer; ERANGE when after is below before
 * (the counter was reset between the readings, so the advance is not
 * known); *delta unchanged.
 * Specification: Intel SDM Vol. 4 rev. 093, Table 2-20 ("Value since last
 * reset").
 */
int
cpux86_residency_delta(
	uint64_t before,
	uint64_t after,
	uint64_t *delta);

/* The highest counting rate the conversion below accepts, 10 GHz. */
#define CPUX86_RATE_MAX_HZ		((uint64_t)10000000U * 1000U)

/*
 * Converts residency counts to nanoseconds.
 *
 * Input: counts, an advance from cpux86_residency_delta(); rate_hz, the
 * counter's rate in Hz (the TSC's frequency, or 1000000 for
 * CPUX86_RATE_1MHZ), 1..CPUX86_RATE_MAX_HZ.
 * Output: *nanoseconds, counts * 10^9 / rate_hz rounded down, computed
 * without an intermediate wider than 64 bits.
 * Errors: EINVAL for a NULL pointer or a rate outside the range; ERANGE
 * when the result does not fit 64 bits; *nanoseconds unchanged.
 * Specification: arithmetic on the counters of Table 2-20 and Table 2-46.
 */
int
cpux86_residency_nanoseconds(
	uint64_t counts,
	uint64_t rate_hz,
	uint64_t *nanoseconds);

/*
 * The package C-state register.
 *
 * Values referenced: Intel SDM Vol. 4 rev. 093, Table 2-35
 * (MSR_PKG_CST_CONFIG_CONTROL E2H, the 12th generation models take it
 * from that table through §2.17).
 */
#define CPUX86_MSR_PKG_CST_CONFIG	0x0E2U

/*
 * The package C-state limit codes of MSR_PKG_CST_CONFIG_CONTROL[3:0]:
 * the deepest package state the package may enter.  Codes 9..15 are
 * reserved.  The names are the processor's own C-states, not ACPI's or
 * MWAIT's.
 *
 * Values referenced: Intel SDM Vol. 4 rev. 093, Table 2-35, bits 3:0.
 */
#define CPUX86_PKG_LIMIT_C1		0U	/* C0/C1, no package C-state */
#define CPUX86_PKG_LIMIT_C2		1U
#define CPUX86_PKG_LIMIT_C3		2U
#define CPUX86_PKG_LIMIT_C6		3U
#define CPUX86_PKG_LIMIT_C7		4U
#define CPUX86_PKG_LIMIT_C7S		5U
#define CPUX86_PKG_LIMIT_C8		6U
#define CPUX86_PKG_LIMIT_C9		7U
#define CPUX86_PKG_LIMIT_C10		8U

/*
 * The flags of MSR_PKG_CST_CONFIG_CONTROL, as bits of
 * struct cpux86_pkg_cstate.flags.
 *
 * Values referenced: Intel SDM Vol. 4 rev. 093, Table 2-35 (bits 10, 15
 * and 25 .. 30).
 */
#define CPUX86_PKG_CST_IO_MWAIT		0x01U	/* bit 10, I/O MWAIT redirection */
#define CPUX86_PKG_CST_LOCKED		0x02U	/* bit 15, CFG Lock: bits 15:0 can no longer be written */
#define CPUX86_PKG_CST_C3_DEMOTION	0x04U	/* bit 25 */
#define CPUX86_PKG_CST_C1_DEMOTION	0x08U	/* bit 26 */
#define CPUX86_PKG_CST_C3_UNDEMOTION	0x10U	/* bit 27 */
#define CPUX86_PKG_CST_C1_UNDEMOTION	0x20U	/* bit 28 */
#define CPUX86_PKG_CST_PKG_DEMOTION	0x40U	/* bit 29 */
#define CPUX86_PKG_CST_PKG_UNDEMOTION	0x80U	/* bit 30 */

/*
 * What MSR_PKG_CST_CONFIG_CONTROL says: the package C-state limit and the
 * flags around it.  The register's scope on these models is the core:
 * each core has its own copy.
 */
struct cpux86_pkg_cstate {
	uint32_t limit;			/* CPUX86_PKG_LIMIT_* */
	uint32_t flags;			/* CPUX86_PKG_CST_* */
};

/*
 * Decodes MSR_PKG_CST_CONFIG_CONTROL.
 *
 * Input: raw, the register as read on a model whose allow list names it.
 * Output: *config.  The reserved bits (9:4, 14:11, 24:16, 63:31) are not
 * checked.
 * Errors: EINVAL for a NULL pointer or a reserved limit code (9..15);
 * *config unchanged.
 * Specification: Intel SDM Vol. 4 rev. 093, Table 2-35.
 */
int
cpux86_pkg_cstate_decode(
	uint64_t raw,
	struct cpux86_pkg_cstate *config);

/*
 * Builds the MSR_PKG_CST_CONFIG_CONTROL value with a new limit.
 *
 * The register is changed by reading it, replacing bits 3:0 and writing
 * it back; when the CFG Lock bit (15) is set, bits 15:0 cannot change.
 * Input: current, the register as read on the core to write; limit, a
 * CPUX86_PKG_LIMIT_* code.
 * Output: *raw, current with bits 3:0 replaced by limit and every other
 * bit as it was.
 * Errors, *raw unchanged: EINVAL for a NULL pointer or a limit above
 * CPUX86_PKG_LIMIT_C10; EACCES when bit 15 of current is set.
 * Specification: Intel SDM Vol. 4 rev. 093, Table 2-35 (bits 3:0, bit 15
 * "CFG Lock (R/WO)") and Table 2-20 (the same register: CFG Lock "locks
 * bits 15:0 of this register until next reset").
 */
int
cpux86_pkg_cstate_limit_encode(
	uint64_t current,
	uint32_t limit,
	uint64_t *raw);

#endif
