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

/* The highest leaf of the basic range; the extended range starts above it. */
#define CPUX86_CPUID_BASIC_LIMIT	(CPUX86_EXTENDED_BASE - 1U)

/* The core domain's sub-leaf index in an extended topology leaf. */
#define CPUX86_TOPOLOGY_CORE_INDEX	1U

/* The highest domain type leaf 0BH defines; leaf 1FH defines up to DIEGRP. */
#define CPUX86_TOPOLOGY_V1_LAST_TYPE	CPUX86_DOMAIN_CORE

/* The width of the shift count field of an extended topology sub-leaf, EAX[4:0]. */
#define CPUX86_TOPOLOGY_SHIFT_WIDTH	5U

static int cpux86_cpuid_topology_type_known(uint32_t leaf, uint32_t type);
static uint32_t cpux86_cpuid_topology_end(const struct cpux86_cpuid_regs *subleaves, uint32_t count);
static void cpux86_cpuid_topology_clear(struct cpux86_topology *topology);

/*
 * Identifies the processor from leaves 00H and 01H.
 *
 * The extended family and model are folded into the display values by the
 * rule the manual gives; a vendor other than Intel is reported as unknown
 * rather than refused, since the feature bits that follow are defined for
 * Intel only.
 */
int
cpux86_cpuid_identity(
	const struct cpux86_cpuid_regs *leaf0,
	const struct cpux86_cpuid_regs *leaf1,
	struct cpux86_identity *identity)
{
	uint32_t vendor;
	uint32_t family;
	uint32_t model;
	uint32_t extended_family;
	uint32_t extended_model;
	uint32_t display_family;
	uint32_t display_model;

	/* Refuses a missing argument. */
	if (leaf0 == NULL || leaf1 == NULL || identity == NULL)
		return EINVAL;

	/* Reports that leaf 01H does not exist when MAX_LEAF is 0. */
	if (leaf0->eax == 0U)
		return ENOTSUP;

	/*
	 * Names the vendor from the 12 characters of leaf 00H.  Only a
	 * string that matches in all three registers is Intel.
	 *
	 * Values referenced: Intel SDM Vol. 1 rev. 093, Table 21-4 (EBX, EDX
	 * and ECX hold "Genu", "ineI" and "ntel").
	 */
	vendor = CPUX86_VENDOR_UNKNOWN;
	if (leaf0->ebx == CPUX86_INTEL_VENDOR_EBX &&
	    leaf0->edx == CPUX86_INTEL_VENDOR_EDX &&
	    leaf0->ecx == CPUX86_INTEL_VENDOR_ECX)
		vendor = CPUX86_VENDOR_INTEL;

	/*
	 * Splits leaf 01H EAX into the raw family and model fields.
	 *
	 * Values referenced: Intel SDM Vol. 1 rev. 093, Table 21-6
	 * (MODEL_ID EAX[7:4], FAMILY_ID EAX[11:8], EXTENDED_MODEL_ID
	 * EAX[19:16], EXTENDED_FAMILY_ID EAX[27:20]).
	 */
	model = cpux86_field32(leaf1->eax, 4U, 4U);
	family = cpux86_field32(leaf1->eax, 8U, 4U);
	extended_model = cpux86_field32(leaf1->eax, 16U, 4U);
	extended_family = cpux86_field32(leaf1->eax, 20U, 8U);

	/*
	 * Folds the extended family in only for family 0FH, and the extended
	 * model in only for families 06H and 0FH; any other family leaves
	 * those fields unread.
	 *
	 * Values referenced: Intel SDM Vol. 1 rev. 093, the DisplayFamily
	 * and DisplayModel rules after Table 21-7.
	 */
	display_family = family;
	if (family == 0x0FU)
		display_family = extended_family + family;

	display_model = model;
	if (family == 0x06U) {
		display_model = (extended_model << 4) + model;
	} else if (family == 0x0FU) {
		display_model = (extended_model << 4) + model;
	}

	/* Publishes the identity; the extended range stays unknown until read. */
	identity->vendor = vendor;
	identity->max_basic_leaf = leaf0->eax;
	identity->max_extended_leaf = 0U;
	identity->display_family = display_family;
	identity->display_model = display_model;
	identity->stepping = cpux86_field32(leaf1->eax, 0U, 4U);
	identity->processor_type = cpux86_field32(leaf1->eax, 12U, 2U);

	/* Succeeded: the identity is filled. */
	return 0;
}

/*
 * Records how far the extended CPUID range goes.
 *
 * A value below 80000000H means the processor answered the extended query
 * with something that is not an extended maximum, so no extended leaf is
 * trusted.
 */
int
cpux86_cpuid_extended_range(
	const struct cpux86_cpuid_regs *leaf,
	struct cpux86_identity *identity)
{
	/* Refuses a missing argument. */
	if (leaf == NULL || identity == NULL)
		return EINVAL;

	/*
	 * Trusts the reported maximum only when it lies in the extended
	 * range.
	 *
	 * Values referenced: Intel SDM Vol. 1 rev. 093, Table 21-89
	 * (MAX_EXTENDED_LEAF is EAX[31:0]).
	 */
	if (leaf->eax >= CPUX86_EXTENDED_BASE) {
		identity->max_extended_leaf = leaf->eax;
	} else {
		identity->max_extended_leaf = 0U;
	}

	/* Succeeded: the extended range is recorded. */
	return 0;
}

/*
 * Reports whether a leaf may be read.
 *
 * Basic and extended leaves are judged against their own maximum; an
 * extended maximum of 0 means the extended range was never established.
 */
int
cpux86_cpuid_has_leaf(
	const struct cpux86_identity *identity,
	uint32_t leaf)
{
	/* Reports no leaf for a missing identity. */
	if (identity == NULL)
		return 0;

	/* A basic leaf is readable up to the basic maximum. */
	if (leaf <= CPUX86_CPUID_BASIC_LIMIT) {
		if (leaf <= identity->max_basic_leaf)
			return 1;

		return 0;
	}

	/* An extended leaf needs an established extended maximum. */
	if (identity->max_extended_leaf == 0U)
		return 0;

	/* An extended leaf is readable up to the extended maximum. */
	if (leaf <= identity->max_extended_leaf)
		return 1;

	/* The leaf lies beyond the extended maximum. */
	return 0;
}

/*
 * Decodes the feature bits of leaf 01H.
 *
 * Each flag is set from exactly one register bit; bits the header does not
 * name never reach the flags.
 */
int
cpux86_cpuid_features(
	const struct cpux86_cpuid_regs *leaf1,
	struct cpux86_features *features)
{
	uint32_t flags;
	uint32_t bit;

	/* Refuses a missing argument. */
	if (leaf1 == NULL || features == NULL)
		return EINVAL;

	/*
	 * Collects the feature flags from the bits of ECX the driver gates
	 * on.
	 *
	 * Values referenced: Intel SDM Vol. 1 rev. 093, Table 21-9 (MONITOR
	 * ECX[3], EIST ECX[7], X2APIC ECX[21], TSC_DEADLINE ECX[24], and
	 * ECX[31], which Intel returns as 0 and an emulating hypervisor
	 * sets).
	 */
	flags = 0U;
	bit = cpux86_field32(leaf1->ecx, 3U, 1U);
	if (bit != 0U)
		flags |= CPUX86_FEATURE_MONITOR;
	bit = cpux86_field32(leaf1->ecx, 7U, 1U);
	if (bit != 0U)
		flags |= CPUX86_FEATURE_EIST;
	bit = cpux86_field32(leaf1->ecx, 21U, 1U);
	if (bit != 0U)
		flags |= CPUX86_FEATURE_X2APIC;
	bit = cpux86_field32(leaf1->ecx, 24U, 1U);
	if (bit != 0U)
		flags |= CPUX86_FEATURE_TSC_DEADLINE;
	bit = cpux86_field32(leaf1->ecx, 31U, 1U);
	if (bit != 0U)
		flags |= CPUX86_FEATURE_HYPERVISOR;

	/*
	 * Publishes the flags with the two APIC numbers of EBX.
	 *
	 * Values referenced: Intel SDM Vol. 1 rev. 093, Table 21-8
	 * (APIC_ID_SPACE EBX[23:16], INITIAL_APIC_ID EBX[31:24]).
	 */
	features->flags = flags;
	features->apic_id_space = cpux86_field32(leaf1->ebx, 16U, 8U);
	features->initial_apic_id = cpux86_field32(leaf1->ebx, 24U, 8U);

	/* Succeeded: the features are filled. */
	return 0;
}

/*
 * Decodes leaf 05H.
 *
 * The sub-state counts are meaningful only when the extensions flag says
 * EDX enumerates them; without it they all read as 0.
 */
int
cpux86_cpuid_mwait(
	const struct cpux86_cpuid_regs *leaf5,
	struct cpux86_mwait *mwait)
{
	uint32_t flags;
	uint32_t bit;
	uint32_t index;

	/* Refuses a missing argument. */
	if (leaf5 == NULL || mwait == NULL)
		return EINVAL;

	/*
	 * Collects the MWAIT flags from ECX.
	 *
	 * Values referenced: Intel SDM Vol. 1 rev. 093, Table 21-15
	 * (MONITOR_MWAIT_EXTENSIONS ECX[0], INTERRUPT_AS_BREAK_EVENT ECX[1],
	 * MONITORLESS_MWAIT ECX[3]).
	 */
	flags = 0U;
	bit = cpux86_field32(leaf5->ecx, 0U, 1U);
	if (bit != 0U)
		flags |= CPUX86_MWAIT_EXTENSIONS;
	bit = cpux86_field32(leaf5->ecx, 1U, 1U);
	if (bit != 0U)
		flags |= CPUX86_MWAIT_INTERRUPT_BREAK;
	bit = cpux86_field32(leaf5->ecx, 3U, 1U);
	if (bit != 0U)
		flags |= CPUX86_MWAIT_MONITORLESS;

	/*
	 * Publishes the monitor line sizes and the flags.
	 *
	 * Values referenced: Intel SDM Vol. 1 rev. 093, Table 21-15
	 * (SMALLEST_MONITOR_LINE_SIZE EAX[15:0], LARGEST_MONITOR_LINE_SIZE
	 * EBX[15:0]).
	 */
	mwait->smallest_line = cpux86_field32(leaf5->eax, 0U, 16U);
	mwait->largest_line = cpux86_field32(leaf5->ebx, 0U, 16U);
	mwait->flags = flags;

	/*
	 * Reads the sub-state count of each processor-specific C-state: four
	 * bits of EDX per state, C0 in the lowest, but only when the
	 * extensions flag is set.
	 *
	 * Values referenced: Intel SDM Vol. 1 rev. 093, Table 21-15
	 * (C0_SUB_STATES EDX[3:0] through C7_SUB_STATES EDX[31:28]).
	 */
	for (index = 0U;
	     index < CPUX86_MWAIT_CSTATE_COUNT;
	     index++) {
		mwait->substates[index] = 0U;
		if ((flags & CPUX86_MWAIT_EXTENSIONS) != 0U)
			mwait->substates[index] = cpux86_field32(leaf5->edx, index * 4U, 4U);
	}

	/* Succeeded: the MWAIT features are filled. */
	return 0;
}

/*
 * Decodes leaf 06H.
 *
 * The feedback table size is reported as a page count even when the
 * feedback interface is absent; the caller reads the feature bit first.
 */
int
cpux86_cpuid_power(
	const struct cpux86_cpuid_regs *leaf6,
	struct cpux86_power *power)
{
	uint32_t table_pages;

	/* Refuses a missing argument. */
	if (leaf6 == NULL || power == NULL)
		return EINVAL;

	/*
	 * Keeps the defined bits of EAX; the reserved bits 3, 12, 21 and
	 * 31:24 never reach the features.
	 *
	 * Values referenced: Intel SDM Vol. 1 rev. 093, Table 21-16 (EAX).
	 */
	power->features = leaf6->eax & CPUX86_POWER_DEFINED;

	/*
	 * Reads the thermal threshold count and the two counter bits.
	 *
	 * Values referenced: Intel SDM Vol. 1 rev. 093, Table 21-16
	 * (DTS_NUM_INT_THRESHOLDS EBX[3:0], HW_FEEDBACK_CAP ECX[0],
	 * ENERGY_PERF_BIAS ECX[3]).
	 */
	power->thermal_thresholds = cpux86_field32(leaf6->ebx, 0U, 4U);
	power->counters = leaf6->ecx & (CPUX86_POWER_APERF_MPERF | CPUX86_POWER_ENERGY_BIAS);

	/*
	 * Reads the geometry of the hardware feedback table: the number of
	 * classes, the capability bits as reported, the size in 4 KiB pages
	 * (the field plus one) and this processor's row.
	 *
	 * Values referenced: Intel SDM Vol. 1 rev. 093, Table 21-16
	 * (HW_FEEDBACK_NUM_CLASSES ECX[15:8], HW_FEEDBACK_CAPS EDX[7:0],
	 * HW_FEEDBACK_TABLE_SIZE EDX[11:8], HW_FEEDBACK_TABLE_INDEX
	 * EDX[31:16]).
	 */
	table_pages = cpux86_field32(leaf6->edx, 8U, 4U) + 1U;
	power->feedback_classes = cpux86_field32(leaf6->ecx, 8U, 8U);
	power->feedback_capabilities = cpux86_field32(leaf6->edx, 0U, 8U);
	power->feedback_pages = table_pages;
	power->feedback_row = cpux86_field32(leaf6->edx, 16U, 16U);

	/* Succeeded: the power features are filled. */
	return 0;
}

/*
 * Reports whether the processor is a hybrid part.
 *
 * Only the hybrid bit of leaf 07H is read; the core kind of each logical
 * processor comes from leaf 1AH.
 */
int
cpux86_cpuid_hybrid(
	const struct cpux86_cpuid_regs *leaf7,
	uint32_t *hybrid)
{
	/* Refuses a missing argument. */
	if (leaf7 == NULL || hybrid == NULL)
		return EINVAL;

	/*
	 * Reads the hybrid bit.
	 *
	 * Values referenced: Intel SDM Vol. 1 rev. 093, Table 21-21 (HYBRID
	 * EDX[15]).
	 */
	*hybrid = cpux86_field32(leaf7->edx, 15U, 1U);

	/* Succeeded: the hybrid flag is filled. */
	return 0;
}

/*
 * Decodes leaf 1AH.
 *
 * A core type other than the two the header names is passed through,
 * since the manual gives the value no meaning beyond naming the
 * microarchitecture.
 */
int
cpux86_cpuid_core_kind(
	const struct cpux86_cpuid_regs *leaf1a,
	struct cpux86_core_kind *kind)
{
	/* Refuses a missing argument. */
	if (leaf1a == NULL || kind == NULL)
		return EINVAL;

	/* Reports that the leaf is not valid on this processor when EAX is 0. */
	if (leaf1a->eax == 0U)
		return ENOTSUP;

	/*
	 * Publishes the core type and the native model number.
	 *
	 * Values referenced: Intel SDM Vol. 1 rev. 093, Table 21-63
	 * (CORE_TYPE EAX[31:24], CORE_NATIVE_MODEL_ID EAX[23:0]).
	 */
	kind->core_type = cpux86_field32(leaf1a->eax, 24U, 8U);
	kind->native_model_id = cpux86_field32(leaf1a->eax, 0U, 24U);

	/* Succeeded: the core kind is filled. */
	return 0;
}

/*
 * Decodes one sub-leaf of leaf 04H.
 *
 * Every field is stored as the register holds it plus one; the size is the
 * product of the four counts and is refused when it would not fit 64 bits.
 */
int
cpux86_cpuid_cache(
	const struct cpux86_cpuid_regs *leaf4,
	struct cpux86_cache *cache)
{
	struct cpux86_cache decoded;
	uint32_t type;
	uint32_t flags;
	uint32_t bit;
	uint64_t geometry;

	/* Refuses a missing argument. */
	if (leaf4 == NULL || cache == NULL)
		return EINVAL;

	/*
	 * Reads the cache type, the first field of the sub-leaf.
	 *
	 * Values referenced: Intel SDM Vol. 1 rev. 093, Table 21-14
	 * (CACHE_TYPE EAX[4:0]: 0 null, 1 data, 2 instruction, 3 unified,
	 * 4 to 31 reserved).
	 */
	type = cpux86_field32(leaf4->eax, 0U, 5U);

	/* Reports that no more caches are described. */
	if (type == CPUX86_CACHE_NULL)
		return ENOTSUP;

	/* Refuses a reserved cache type. */
	if (type > CPUX86_CACHE_UNIFIED)
		return EINVAL;

	/*
	 * Collects the flags from EAX and EDX.
	 *
	 * Values referenced: Intel SDM Vol. 1 rev. 093, Table 21-14
	 * (SELF_INITIALIZING_CACHE EAX[8], FULLY_ASSOC EAX[9],
	 * NOT_LWR_CACHE_FLUSH EDX[0], INCLUSIVE_CACHE EDX[1],
	 * COMPLEX_CACHE_INDEXING EDX[2]).
	 */
	flags = 0U;
	bit = cpux86_field32(leaf4->eax, 8U, 1U);
	if (bit != 0U)
		flags |= CPUX86_CACHE_SELF_INIT;
	bit = cpux86_field32(leaf4->eax, 9U, 1U);
	if (bit != 0U)
		flags |= CPUX86_CACHE_FULLY_ASSOC;
	bit = cpux86_field32(leaf4->edx, 0U, 1U);
	if (bit != 0U)
		flags |= CPUX86_CACHE_NO_LOWER_FLUSH;
	bit = cpux86_field32(leaf4->edx, 1U, 1U);
	if (bit != 0U)
		flags |= CPUX86_CACHE_INCLUSIVE;
	bit = cpux86_field32(leaf4->edx, 2U, 1U);
	if (bit != 0U)
		flags |= CPUX86_CACHE_COMPLEX_INDEX;

	/*
	 * Decodes the kind, level and sharing of the cache.  The processor
	 * reserves the next power of two of APIC IDs for the sharing logical
	 * processors, so the shift is the width of that reservation.
	 *
	 * Values referenced: Intel SDM Vol. 1 rev. 093, Table 21-14
	 * (CACHE_LEVEL EAX[7:5], MAX_LP_ADDRESSABLE_IDS EAX[25:14], plus
	 * one).
	 */
	decoded.type = type;
	decoded.level = cpux86_field32(leaf4->eax, 5U, 3U);
	decoded.flags = flags;
	decoded.sharing_ids = cpux86_field32(leaf4->eax, 14U, 12U) + 1U;
	decoded.share_shift = cpux86_ceil_log2(decoded.sharing_ids);

	/*
	 * Decodes the geometry; every field is stored plus one.
	 *
	 * Values referenced: Intel SDM Vol. 1 rev. 093, Table 21-14
	 * (LINE_SIZE EBX[11:0], PHYS_LINE_PARTITIONS EBX[21:12], NUM_WAYS
	 * EBX[31:22], NUM_SETS ECX[31:0], each plus one).
	 */
	decoded.line_size = cpux86_field32(leaf4->ebx, 0U, 12U) + 1U;
	decoded.partitions = cpux86_field32(leaf4->ebx, 12U, 10U) + 1U;
	decoded.ways = cpux86_field32(leaf4->ebx, 22U, 10U) + 1U;
	decoded.sets = (uint64_t)leaf4->ecx + 1U;

	/*
	 * Multiplies the three small counts first: at most 1024 * 1024 * 4096,
	 * which is 2^32 and fits.
	 */
	geometry = (uint64_t)decoded.ways;
	geometry = geometry * (uint64_t)decoded.partitions;
	geometry = geometry * (uint64_t)decoded.line_size;

	/*
	 * Refuses a size that needs 65 bits.  Both factors are at most 2^32,
	 * so their product overflows only when both are exactly 2^32.
	 */
	if (geometry > (uint64_t)0xFFFFFFFFU &&
	    decoded.sets > (uint64_t)0xFFFFFFFFU)
		return ERANGE;

	/*
	 * Computes the cache size in bytes.
	 *
	 * Values referenced: Intel SDM Vol. 1 rev. 093, the cache size
	 * formula after Table 21-14 ((ways + 1) * (partitions + 1) *
	 * (line size + 1) * (sets + 1)).
	 */
	decoded.size = geometry * decoded.sets;

	/* Publishes the decoded cache. */
	*cache = decoded;

	/* Succeeded: the cache is filled. */
	return 0;
}

/*
 * Reports which instance of a cache a logical processor uses.
 *
 * Processors that share a cache agree on every x2APIC ID bit above the
 * ones the sharing group reserves, so dropping those bits names the cache
 * instance.
 */
int
cpux86_cache_group(
	uint32_t x2apic_id,
	const struct cpux86_cache *cache,
	uint32_t *group)
{
	/* Refuses a missing argument. */
	if (cache == NULL || group == NULL)
		return EINVAL;

	/* Refuses a shift that would reach the register width. */
	if (cache->share_shift > 31U)
		return EINVAL;

	/* Drops the bits that tell the sharing logical processors apart. */
	*group = x2apic_id >> cache->share_shift;

	/* Succeeded: the cache group is filled. */
	return 0;
}

/*
 * Decodes one sub-leaf of leaf 0BH or 1FH.
 *
 * The fields are reported as they are, including a reserved domain type
 * and the INVALID type that ends the enumeration.
 */
int
cpux86_cpuid_topology_level(
	const struct cpux86_cpuid_regs *subleaf,
	struct cpux86_topology_level *level)
{
	/* Refuses a missing argument. */
	if (subleaf == NULL || level == NULL)
		return EINVAL;

	/*
	 * Publishes the fields of the sub-leaf.
	 *
	 * Values referenced: Intel SDM Vol. 1 rev. 093, Table 21-31 and
	 * Table 21-70 (SHIFT_COUNT EAX[4:0], NEXT_LEVEL_NUM_LP EBX[15:0],
	 * LEVEL_NUM ECX[7:0], DOMAIN_TYPE ECX[15:8], X2APIC_ID EDX[31:0]).
	 */
	level->level = cpux86_field32(subleaf->ecx, 0U, 8U);
	level->type = cpux86_field32(subleaf->ecx, 8U, 8U);
	level->shift = cpux86_field32(subleaf->eax, 0U, CPUX86_TOPOLOGY_SHIFT_WIDTH);
	level->logical_count = cpux86_field32(subleaf->ebx, 0U, 16U);
	level->x2apic_id = subleaf->edx;

	/* Succeeded: the sub-leaf is decoded. */
	return 0;
}

/*
 * Places one logical processor in its package from the sub-leaves of
 * leaf 0BH or 1FH.
 *
 * Each known domain owns the x2APIC ID bits between the previous
 * sub-leaf's shift and its own; a domain type the decoder does not know
 * widens the last known domain below it, so a new domain between two known
 * ones never moves a bit field the caller relies on.
 */
int
cpux86_cpuid_topology(
	uint32_t leaf,
	const struct cpux86_cpuid_regs *subleaves,
	uint32_t count,
	struct cpux86_topology *topology)
{
	struct cpux86_topology placed;
	struct cpux86_topology_level level;
	uint32_t valid;
	uint32_t index;
	uint32_t type;
	uint32_t previous_shift;
	uint32_t highest_known;
	uint32_t last_known;
	uint32_t x2apic_id;
	uint32_t domain;
	uint32_t width;
	int error;
	int known;

	/* Refuses a missing argument. */
	if (subleaves == NULL || topology == NULL)
		return EINVAL;

	/* Refuses a leaf that is not an extended topology leaf. */
	if (leaf != CPUX86_LEAF_TOPOLOGY && leaf != CPUX86_LEAF_TOPOLOGY_V2)
		return EINVAL;

	/* Refuses a sub-leaf count outside what the decoder reads. */
	if (count == 0U || count > CPUX86_TOPOLOGY_SUBLEAF_MAX)
		return EINVAL;

	/* Decodes sub-leaf 0, which says whether the leaf exists at all. */
	error = cpux86_cpuid_topology_level(&subleaves[0], &level);
	if (error != 0)
		return error;

	/*
	 * Reports a leaf the processor does not implement: the first
	 * sub-leaf has no domain, or no logical processors.  This is decided
	 * before the shape of the list is judged.
	 *
	 * Values referenced: Intel SDM Vol. 1 rev. 093, Table 21-31 and
	 * Table 21-70 (the leaf is valid if sub-leaf 0 EBX[15:0] is not 0;
	 * an invalid leaf reports domain type INVALID).
	 */
	if (level.type == CPUX86_DOMAIN_INVALID)
		return ENOTSUP;
	if (level.logical_count == 0U)
		return ENOTSUP;

	/* Finds the first INVALID sub-leaf, which ends the enumeration. */
	valid = cpux86_cpuid_topology_end(subleaves, count);

	/* Refuses a list that never reaches the INVALID terminator. */
	if (valid >= count)
		return EINVAL;

	/*
	 * Refuses a list with fewer than two valid sub-leaves: the logical
	 * processor and core domains are always enumerated.
	 */
	if (valid < 2U)
		return EINVAL;

	/* Starts from a topology with no domain placed. */
	cpux86_cpuid_topology_clear(&placed);

	/*
	 * Walks the valid sub-leaves from the logical processor upward,
	 * giving each known domain the bits from the previous shift to its
	 * own and widening the last known domain for each unknown type.
	 *
	 * Values referenced: Intel SDM Vol. 3A rev. 093, section 11.9.4
	 * step 3 (an unknown domain is an extension of the last known
	 * domain below it).
	 */
	x2apic_id = level.x2apic_id;
	previous_shift = 0U;
	highest_known = 0U;
	last_known = 0U;
	for (index = 0U;
	     index < valid;
	     index++) {
		/* Decodes this sub-leaf. */
		error = cpux86_cpuid_topology_level(&subleaves[index], &level);
		if (error != 0)
			return error;

		/* Refuses a sub-leaf that does not carry its own index. */
		if (level.level != index)
			return EINVAL;

		/* Refuses an x2APIC ID that changes between sub-leaves. */
		if (level.x2apic_id != x2apic_id)
			return EINVAL;

		/* Refuses a shift that goes back down. */
		if (level.shift < previous_shift)
			return EINVAL;

		/* Refuses a first sub-leaf that is not the logical processor. */
		type = level.type;
		if (index == 0U && type != CPUX86_DOMAIN_THREAD)
			return EINVAL;

		/* Refuses a second sub-leaf that is not the core. */
		if (index == CPUX86_TOPOLOGY_CORE_INDEX && type != CPUX86_DOMAIN_CORE)
			return EINVAL;

		/*
		 * Places a known domain: it must rise above every known
		 * domain already placed, and owns the bits up to its shift.
		 */
		known = cpux86_cpuid_topology_type_known(leaf, type);
		if (known != 0) {
			if (type <= highest_known)
				return EINVAL;

			placed.present |= (uint32_t)1U << type;
			placed.low[type] = previous_shift;
			placed.high[type] = level.shift;
			highest_known = type;
			last_known = type;
		} else {
			/*
			 * Widens the last known domain up to this unknown
			 * domain's shift; there is always one, because the
			 * first two sub-leaves are known.
			 */
			placed.high[last_known] = level.shift;
		}

		previous_shift = level.shift;
	}

	/* Decodes the terminator, whose index must still be its own. */
	error = cpux86_cpuid_topology_level(&subleaves[valid], &level);
	if (error != 0)
		return error;

	/* Refuses a terminator that does not carry its own index. */
	if (level.level != valid)
		return EINVAL;

	/*
	 * Cuts each placed domain's instance number out of the x2APIC ID,
	 * and the package number from everything above the last shift.
	 */
	for (domain = 1U;
	     domain < CPUX86_DOMAIN_COUNT;
	     domain++) {
		if ((placed.present & ((uint32_t)1U << domain)) == 0U)
			continue;

		width = placed.high[domain] - placed.low[domain];
		placed.id[domain] = cpux86_field32(x2apic_id, placed.low[domain], width);
	}

	placed.x2apic_id = x2apic_id;
	placed.package_shift = previous_shift;
	placed.package = x2apic_id >> previous_shift;

	/* Publishes the placement. */
	*topology = placed;

	/* Succeeded: the topology is filled. */
	return 0;
}

/*
 * Decodes leaves 80000007H and 80000008H.
 *
 * A leaf the caller could not read is passed as NULL and leaves its field
 * 0, so a processor with a short extended range still decodes.
 */
int
cpux86_cpuid_extended(
	const struct cpux86_cpuid_regs *leaf7,
	const struct cpux86_cpuid_regs *leaf8,
	struct cpux86_extended *extended)
{
	uint32_t invariant_tsc;
	uint32_t address_bits;

	/* Refuses a missing output. */
	if (extended == NULL)
		return EINVAL;

	/*
	 * Reads the invariant TSC bit when leaf 80000007H was read.
	 *
	 * Values referenced: Intel SDM Vol. 1 rev. 093, Table 21-97
	 * (TSC_INVARIANT EDX[8]).
	 */
	invariant_tsc = 0U;
	if (leaf7 != NULL)
		invariant_tsc = cpux86_field32(leaf7->edx, 8U, 1U);

	/*
	 * Reads the physical address width when leaf 80000008H was read.
	 *
	 * Values referenced: Intel SDM Vol. 1 rev. 093, Table 21-98
	 * (PHYS_ADDR_SIZE EAX[7:0]).
	 */
	address_bits = 0U;
	if (leaf8 != NULL)
		address_bits = cpux86_field32(leaf8->eax, 0U, 8U);

	/* Publishes the two fields. */
	extended->invariant_tsc = invariant_tsc;
	extended->physical_address_bits = address_bits;

	/* Succeeded: the extended features are filled. */
	return 0;
}

/*
 * Reports whether the decoder knows a domain type in a topology leaf.
 *
 * Leaf 0BH defines the logical processor and core domains only; leaf 1FH
 * also defines module, tile, die and die group.  Everything else is
 * reserved and is treated as an extension of the domain below it.
 */
static int
cpux86_cpuid_topology_type_known(
	uint32_t leaf,
	uint32_t type)
{
	/* The invalid type ends the list and is not a domain. */
	if (type == CPUX86_DOMAIN_INVALID)
		return 0;

	/* Leaf 0BH knows only the types up to the core. */
	if (leaf == CPUX86_LEAF_TOPOLOGY) {
		if (type <= CPUX86_TOPOLOGY_V1_LAST_TYPE)
			return 1;

		return 0;
	}

	/* Leaf 1FH knows every type up to the die group. */
	if (type <= CPUX86_DOMAIN_DIEGRP)
		return 1;

	/* A reserved type. */
	return 0;
}

/*
 * Finds where a list of topology sub-leaves ends.
 *
 * The result is the index of the first sub-leaf of type INVALID, or count
 * when there is none; entries after the first INVALID one are not read.
 */
static uint32_t
cpux86_cpuid_topology_end(
	const struct cpux86_cpuid_regs *subleaves,
	uint32_t count)
{
	uint32_t index;
	uint32_t type;

	/* Looks for the terminating sub-leaf. */
	for (index = 0U;
	     index < count;
	     index++) {
		type = cpux86_field32(subleaves[index].ecx, 8U, 8U);
		if (type == CPUX86_DOMAIN_INVALID)
			return index;
	}

	/* No terminator within the list. */
	return count;
}

/*
 * Clears a topology to the state with no domain placed.
 */
static void
cpux86_cpuid_topology_clear(
	struct cpux86_topology *topology)
{
	uint32_t domain;

	/* Zeroes the scalar fields. */
	topology->x2apic_id = 0U;
	topology->present = 0U;
	topology->package = 0U;
	topology->package_shift = 0U;

	/* Zeroes the per-domain arrays. */
	for (domain = 0U;
	     domain < CPUX86_DOMAIN_COUNT;
	     domain++) {
		topology->id[domain] = 0U;
		topology->low[domain] = 0U;
		topology->high[domain] = 0U;
	}
}
