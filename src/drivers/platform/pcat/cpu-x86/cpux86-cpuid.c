/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The decoder of the CPUID leaves the x86 processor driver reads.
 */

#include "cpux86-cpuid.h"

/*
 * Identifies the processor from leaves 00H and 01H.
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
 */
int
cpux86_cpuid_identity(
	const struct cpux86_cpuid_regs *leaf0,
	const struct cpux86_cpuid_regs *leaf1,
	struct cpux86_identity *identity)
{
	/* The skeleton reads none of its arguments. */
	(void)leaf0;
	(void)leaf1;
	(void)identity;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}

/*
 * Records how far the extended CPUID range goes.
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
 */
int
cpux86_cpuid_extended_range(
	const struct cpux86_cpuid_regs *leaf,
	struct cpux86_identity *identity)
{
	/* The skeleton reads none of its arguments. */
	(void)leaf;
	(void)identity;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}

/*
 * Reports whether a leaf may be read.
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
 */
int
cpux86_cpuid_has_leaf(
	const struct cpux86_identity *identity,
	uint32_t leaf)
{
	/* The skeleton reads none of its arguments. */
	(void)identity;
	(void)leaf;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}

/*
 * Decodes the feature bits of leaf 01H.
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
 */
int
cpux86_cpuid_features(
	const struct cpux86_cpuid_regs *leaf1,
	struct cpux86_features *features)
{
	/* The skeleton reads none of its arguments. */
	(void)leaf1;
	(void)features;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}

/*
 * Decodes leaf 05H.
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
 */
int
cpux86_cpuid_mwait(
	const struct cpux86_cpuid_regs *leaf5,
	struct cpux86_mwait *mwait)
{
	/* The skeleton reads none of its arguments. */
	(void)leaf5;
	(void)mwait;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}

/*
 * Decodes leaf 06H.
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
 */
int
cpux86_cpuid_power(
	const struct cpux86_cpuid_regs *leaf6,
	struct cpux86_power *power)
{
	/* The skeleton reads none of its arguments. */
	(void)leaf6;
	(void)power;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}

/*
 * Reports whether the processor is a hybrid part.
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
 */
int
cpux86_cpuid_hybrid(
	const struct cpux86_cpuid_regs *leaf7,
	uint32_t *hybrid)
{
	/* The skeleton reads none of its arguments. */
	(void)leaf7;
	(void)hybrid;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}

/*
 * Decodes leaf 1AH.
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
 */
int
cpux86_cpuid_core_kind(
	const struct cpux86_cpuid_regs *leaf1a,
	struct cpux86_core_kind *kind)
{
	/* The skeleton reads none of its arguments. */
	(void)leaf1a;
	(void)kind;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}

/*
 * Decodes one sub-leaf of leaf 04H.
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
 */
int
cpux86_cpuid_cache(
	const struct cpux86_cpuid_regs *leaf4,
	struct cpux86_cache *cache)
{
	/* The skeleton reads none of its arguments. */
	(void)leaf4;
	(void)cache;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}

/*
 * Reports which instance of a cache a logical processor uses.
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
 */
int
cpux86_cache_group(
	uint32_t x2apic_id,
	const struct cpux86_cache *cache,
	uint32_t *group)
{
	/* The skeleton reads none of its arguments. */
	(void)x2apic_id;
	(void)cache;
	(void)group;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}

/*
 * Decodes one sub-leaf of leaf 0BH or 1FH.
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
 */
int
cpux86_cpuid_topology_level(
	const struct cpux86_cpuid_regs *subleaf,
	struct cpux86_topology_level *level)
{
	/* The skeleton reads none of its arguments. */
	(void)subleaf;
	(void)level;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}

/*
 * Places one logical processor in its package from the sub-leaves of
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
 */
int
cpux86_cpuid_topology(
	uint32_t leaf,
	const struct cpux86_cpuid_regs *subleaves,
	uint32_t count,
	struct cpux86_topology *topology)
{
	/* The skeleton reads none of its arguments. */
	(void)leaf;
	(void)subleaves;
	(void)count;
	(void)topology;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}

/*
 * Decodes leaves 80000007H and 80000008H.
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
 */
int
cpux86_cpuid_extended(
	const struct cpux86_cpuid_regs *leaf7,
	const struct cpux86_cpuid_regs *leaf8,
	struct cpux86_extended *extended)
{
	/* The skeleton reads none of its arguments. */
	(void)leaf7;
	(void)leaf8;
	(void)extended;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}
