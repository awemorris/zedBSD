/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The decoder of the CPUID leaves the processor driver reads: who made
 * the processor and which model it is, the feature bits that gate every
 * register the driver touches, the MONITOR/MWAIT states, the thermal and
 * power features with the hardware feedback geometry, the cache sharing,
 * the core kind of a hybrid part, and the place of a logical processor in
 * the package from the extended topology leaves.
 *
 * Every function here takes registers the driver read with CPUID on the
 * logical processor the answer is about; topology, core kind and the
 * feedback row are per logical processor and must be read on each one.
 */

#ifndef DRIVERS_PLATFORM_PCAT_CPU_X86_CPUX86_CPUID_H
#define DRIVERS_PLATFORM_PCAT_CPU_X86_CPUX86_CPUID_H

#include "cpux86.h"

/* Who made the processor, as leaf 00H names it. */
#define CPUX86_VENDOR_UNKNOWN		0U
#define CPUX86_VENDOR_INTEL		1U

/*
 * The vendor string of an Intel processor, four characters per register
 * with the first character in the low byte.
 *
 * Values referenced: Intel 64 and IA-32 Architectures Software
 * Developer's Manual Vol. 1 (253665-093, September 2026), Table 21-4 and
 * the paragraph after it ("Genu", "ineI", "ntel").
 */
#define CPUX86_INTEL_VENDOR_EBX		0x756e6547U
#define CPUX86_INTEL_VENDOR_EDX		0x49656e69U
#define CPUX86_INTEL_VENDOR_ECX		0x6c65746eU

/* The first leaf of the extended range; leaf 80000000H reports the last. */
#define CPUX86_EXTENDED_BASE		0x80000000U

/*
 * Who made the processor, which model it is, and how far each CPUID range
 * goes.
 *
 * Filled by cpux86_cpuid_identity() from leaves 00H and 01H and completed
 * by cpux86_cpuid_extended_range() from leaf 80000000H; one per machine is
 * enough, since every logical processor reports the same.
 */
struct cpux86_identity {
	uint32_t vendor;		/* CPUX86_VENDOR_* */
	uint32_t max_basic_leaf;	/* leaf 00H EAX */
	uint32_t max_extended_leaf;	/* leaf 80000000H EAX, 0 until known or when absent */
	uint32_t display_family;	/* the family the model tables name, e.g. 06H */
	uint32_t display_model;		/* the model the model tables name, e.g. 9AH */
	uint32_t stepping;		/* leaf 01H EAX[3:0] */
	uint32_t processor_type;	/* leaf 01H EAX[13:12] */
};

/*
 * Identifies the processor from leaves 00H and 01H.
 *
 * Reads the vendor string and MAX_LEAF from leaf 00H and the family,
 * model, stepping and type from leaf 01H, and combines the extended
 * family and model with the base ones by the display rule:
 * DisplayFamily = Family_ID, or Extended_Family_ID + Family_ID when
 * Family_ID is 0FH; DisplayModel = (Extended_Model_ID << 4) + Model_ID
 * when Family_ID is 06H or 0FH, otherwise Model_ID.
 * Input: leaf0 and leaf1, the registers of CPUID leaves 00H and 01H.  A
 * vendor string other than Intel's gives CPUX86_VENDOR_UNKNOWN.
 * Output: *identity, every field but max_extended_leaf, which is set to 0.
 * Precondition: none; leaf 01H is meaningful only when leaf 00H EAX >= 1.
 * Errors: EINVAL for a NULL pointer; ENOTSUP when leaf 00H EAX is 0 (leaf
 * 01H does not exist), *identity unchanged.
 * Specification: Intel SDM Vol. 1 rev. 093, Table 21-4 (leaf 00H),
 * Table 21-6 (leaf 01H EAX) and the DisplayFamily/DisplayModel rule after
 * Table 21-7.
 */
int
cpux86_cpuid_identity(
	const struct cpux86_cpuid_regs *leaf0,
	const struct cpux86_cpuid_regs *leaf1,
	struct cpux86_identity *identity);

/*
 * Records how far the extended CPUID range goes.
 *
 * Input: leaf, the registers of CPUID leaf 80000000H; identity, filled by
 * cpux86_cpuid_identity().
 * Output: identity->max_extended_leaf is leaf->eax when that is at least
 * 80000000H, otherwise 0 (no extended leaf can be trusted).
 * Errors: EINVAL for a NULL pointer.
 * Specification: Intel SDM Vol. 1 rev. 093, Table 21-89.
 */
int
cpux86_cpuid_extended_range(
	const struct cpux86_cpuid_regs *leaf,
	struct cpux86_identity *identity);

/*
 * Reports whether a leaf may be read.
 *
 * Input: identity, filled as above; leaf, a basic leaf (below 80000000H)
 * or an extended one.
 * Output: 1 when the leaf is within the range its MAX_LEAF reports (a
 * basic leaf <= max_basic_leaf; an extended leaf >= 80000000H and <=
 * max_extended_leaf, with max_extended_leaf not 0), otherwise 0; 0 for a
 * NULL identity.
 * Errors: none.
 * Specification: Intel SDM Vol. 1 rev. 093, §21.1.3 and the "valid if
 * MAX_LEAF >= ..." line of each leaf.
 */
int
cpux86_cpuid_has_leaf(
	const struct cpux86_identity *identity,
	uint32_t leaf);

/*
 * The feature bits of leaf 01H the driver gates on, as bits of
 * struct cpux86_features.flags.  Each is set exactly when the named
 * register bit is 1.
 *
 * Values referenced: Intel SDM Vol. 1 rev. 093, Table 21-9 (leaf 01H
 * ECX).  ECX[31] is "Not Used ... allocated for use by software
 * emulation": Intel processors return 0, and a hypervisor that emulates
 * CPUID sets it.
 */
#define CPUX86_FEATURE_MONITOR		0x1U	/* ECX[3], MONITOR/MWAIT */
#define CPUX86_FEATURE_EIST		0x2U	/* ECX[7], Enhanced Intel SpeedStep */
#define CPUX86_FEATURE_X2APIC		0x4U	/* ECX[21] */
#define CPUX86_FEATURE_TSC_DEADLINE	0x8U	/* ECX[24], LAPIC timer TSC-deadline mode */
#define CPUX86_FEATURE_HYPERVISOR	0x10U	/* ECX[31], set by an emulating hypervisor */

/*
 * The feature bits and APIC numbers one logical processor reports in leaf
 * 01H.
 */
struct cpux86_features {
	uint32_t flags;			/* CPUX86_FEATURE_* */
	uint32_t apic_id_space;		/* EBX[23:16], addressable logical processor IDs */
	uint32_t initial_apic_id;	/* EBX[31:24], the 8-bit initial APIC ID */
};

/*
 * Decodes the feature bits of leaf 01H.
 *
 * Input: leaf1, the registers of CPUID leaf 01H.
 * Output: *features; bits of ECX not named above are ignored, EDX is not
 * read.
 * Errors: EINVAL for a NULL pointer.
 * Specification: Intel SDM Vol. 1 rev. 093, Table 21-8 (EBX) and
 * Table 21-9 (ECX).
 */
int
cpux86_cpuid_features(
	const struct cpux86_cpuid_regs *leaf1,
	struct cpux86_features *features);

/*
 * The flags of leaf 05H ECX, as bits of struct cpux86_mwait.flags.
 *
 * Values referenced: Intel SDM Vol. 1 rev. 093, Table 21-15.
 */
#define CPUX86_MWAIT_EXTENSIONS		0x1U	/* ECX[0], EDX enumerates the sub-states */
#define CPUX86_MWAIT_INTERRUPT_BREAK	0x2U	/* ECX[1], interrupts break MWAIT when masked */
#define CPUX86_MWAIT_MONITORLESS	0x4U	/* ECX[3], MWAIT without MONITOR */

/*
 * The C-states leaf 05H EDX counts sub-states of: C0 .. C7.
 *
 * Value referenced: Intel SDM Vol. 1 rev. 093, Table 21-15 (EDX[31:0]).
 */
#define CPUX86_MWAIT_CSTATE_COUNT	8U

/*
 * What MONITOR and MWAIT offer: the monitor line sizes and, for each
 * processor-specific C-state Cn, how many sub-states MWAIT can request.
 *
 * substates[n] is the 4-bit count of EDX[4n+3:4n]; the C-state numbers
 * are the processor's own, not ACPI's.
 */
struct cpux86_mwait {
	uint32_t smallest_line;		/* EAX[15:0], bytes */
	uint32_t largest_line;		/* EBX[15:0], bytes */
	uint32_t flags;			/* CPUX86_MWAIT_* */
	uint32_t substates[CPUX86_MWAIT_CSTATE_COUNT];
};

/*
 * Decodes leaf 05H.
 *
 * Input: leaf5, the registers of CPUID leaf 05H.
 * Output: *mwait.  When ECX[0] (the extensions flag) is 0, EDX does not
 * enumerate sub-states and every substates[] entry is 0.
 * Precondition: max_basic_leaf >= 05H (the caller checks).
 * Errors: EINVAL for a NULL pointer.
 * Specification: Intel SDM Vol. 1 rev. 093, Table 21-15.
 */
int
cpux86_cpuid_mwait(
	const struct cpux86_cpuid_regs *leaf5,
	struct cpux86_mwait *mwait);

/*
 * The thermal and power feature bits of leaf 06H EAX, as bits of
 * struct cpux86_power.features.  Each constant is the register bit itself
 * (bit n of EAX is 1 << n), so features is EAX with its reserved bits
 * (3, 12, 21 and 31:24) cleared.
 *
 * Values referenced: Intel SDM Vol. 1 rev. 093, Table 21-16 (leaf 06H
 * EAX).
 */
#define CPUX86_POWER_DIGITAL_TEMP	0x00000001U	/* EAX[0] */
#define CPUX86_POWER_TURBO		0x00000002U	/* EAX[1], opportunistic performance enabled */
#define CPUX86_POWER_ARAT		0x00000004U	/* EAX[2], APIC timer always running */
#define CPUX86_POWER_LIMIT_NOTIFY	0x00000010U	/* EAX[4] */
#define CPUX86_POWER_EXT_CLOCK_MOD	0x00000020U	/* EAX[5] */
#define CPUX86_POWER_PKG_THERM		0x00000040U	/* EAX[6], package thermal management */
#define CPUX86_POWER_HWP		0x00000080U	/* EAX[7], HWP base registers */
#define CPUX86_POWER_HWP_INTERRUPT	0x00000100U	/* EAX[8], IA32_HWP_INTERRUPT */
#define CPUX86_POWER_HWP_WINDOW		0x00000200U	/* EAX[9], IA32_HWP_REQUEST[41:32] */
#define CPUX86_POWER_HWP_EPP		0x00000400U	/* EAX[10], IA32_HWP_REQUEST[31:24] */
#define CPUX86_POWER_HWP_PKG		0x00000800U	/* EAX[11], IA32_HWP_REQUEST_PKG */
#define CPUX86_POWER_HDC		0x00002000U	/* EAX[13] */
#define CPUX86_POWER_TURBO_MAX		0x00004000U	/* EAX[14], Turbo Boost Max 3.0 */
#define CPUX86_POWER_HWP_HIGHEST_CHANGE	0x00008000U	/* EAX[15] */
#define CPUX86_POWER_HWP_PECI		0x00010000U	/* EAX[16], PECI override */
#define CPUX86_POWER_HWP_FLEXIBLE	0x00020000U	/* EAX[17], the request's valid bits */
#define CPUX86_POWER_HWP_FAST		0x00040000U	/* EAX[18], fast access to the request */
#define CPUX86_POWER_HW_FEEDBACK	0x00080000U	/* EAX[19], hardware feedback interface */
#define CPUX86_POWER_HWP_IGNORE_IDLE	0x00100000U	/* EAX[20] */
#define CPUX86_POWER_HWP_CTL		0x00400000U	/* EAX[22], IA32_HWP_CTL */
#define CPUX86_POWER_THREAD_DIRECTOR	0x00800000U	/* EAX[23] */

/* The bits of leaf 06H EAX that the table above defines. */
#define CPUX86_POWER_DEFINED		0x00DFEFF7U

/*
 * The bits of leaf 06H ECX, as bits of struct cpux86_power.counters.
 *
 * Values referenced: Intel SDM Vol. 1 rev. 093, Table 21-16 (ECX).
 */
#define CPUX86_POWER_APERF_MPERF	0x1U	/* ECX[0], IA32_MPERF and IA32_APERF */
#define CPUX86_POWER_ENERGY_BIAS	0x8U	/* ECX[3], IA32_ENERGY_PERF_BIAS */

/*
 * The capabilities of leaf 06H EDX[7:0], as bits of feedback_capabilities.
 *
 * Values referenced: Intel SDM Vol. 1 rev. 093, Table 21-16 (EDX[7:0]).
 */
#define CPUX86_FEEDBACK_PERFORMANCE	0x1U	/* bit 0 */
#define CPUX86_FEEDBACK_EFFICIENCY	0x2U	/* bit 1 */

/*
 * The thermal and power features one logical processor reports, with the
 * geometry of the hardware feedback table and this processor's row in it.
 */
struct cpux86_power {
	uint32_t features;		/* CPUX86_POWER_*, EAX without reserved bits */
	uint32_t thermal_thresholds;	/* EBX[3:0] */
	uint32_t counters;		/* CPUX86_POWER_APERF_MPERF | _ENERGY_BIAS, ECX bits */
	uint32_t feedback_classes;	/* ECX[15:8], Thread Director classes */
	uint32_t feedback_capabilities;	/* EDX[7:0] as the processor reports it, reserved bits included */
	uint32_t feedback_pages;	/* EDX[11:8] + 1, the table's size in 4 KiB pages */
	uint32_t feedback_row;		/* EDX[31:16], this logical processor's row */
};

/*
 * Decodes leaf 06H.
 *
 * Input: leaf6, the registers of CPUID leaf 06H, read on the logical
 * processor whose feedback row is wanted.
 * Output: *power.  feedback_pages is EDX[11:8] + 1 (1..16) whatever
 * EAX[19] says; it means something only when CPUX86_POWER_HW_FEEDBACK is
 * set.
 * Precondition: max_basic_leaf >= 06H (the caller checks).
 * Errors: EINVAL for a NULL pointer.
 * Specification: Intel SDM Vol. 1 rev. 093, Table 21-16.
 */
int
cpux86_cpuid_power(
	const struct cpux86_cpuid_regs *leaf6,
	struct cpux86_power *power);

/*
 * Reports whether the processor is a hybrid part.
 *
 * Input: leaf7, the registers of CPUID leaf 07H sub-leaf 0.
 * Output: *hybrid is 1 when EDX[15] (HYBRID) is 1, otherwise 0.
 * Precondition: max_basic_leaf >= 07H (the caller checks).
 * Errors: EINVAL for a NULL pointer.
 * Specification: Intel SDM Vol. 1 rev. 093, Table 21-21.
 */
int
cpux86_cpuid_hybrid(
	const struct cpux86_cpuid_regs *leaf7,
	uint32_t *hybrid);

/*
 * The core types of leaf 1AH EAX[31:24].  The value only names the
 * microarchitecture; it carries no order and no other attribute.
 *
 * Values referenced: Intel SDM Vol. 1 rev. 093, Table 21-63.
 */
#define CPUX86_CORE_TYPE_ATOM		0x20U
#define CPUX86_CORE_TYPE_CORE		0x40U

/* The core kind one logical processor of a hybrid part reports. */
struct cpux86_core_kind {
	uint32_t core_type;		/* EAX[31:24], CPUX86_CORE_TYPE_* or another value */
	uint32_t native_model_id;	/* EAX[23:0] */
};

/*
 * Decodes leaf 1AH.
 *
 * Input: leaf1a, the registers of CPUID leaf 1AH sub-leaf 0, read on the
 * logical processor the answer is about.
 * Output: *kind.  A core type other than the two named is passed through
 * as it is; it is not an error.
 * Precondition: max_basic_leaf >= 1AH (the caller checks).
 * Errors: EINVAL for a NULL pointer; ENOTSUP when EAX is 0 (the leaf is
 * not valid on this processor), *kind unchanged.
 * Specification: Intel SDM Vol. 1 rev. 093, Table 21-63 and the validity
 * rule above it.
 */
int
cpux86_cpuid_core_kind(
	const struct cpux86_cpuid_regs *leaf1a,
	struct cpux86_core_kind *kind);

/*
 * The cache types of leaf 04H EAX[4:0].
 *
 * Values referenced: Intel SDM Vol. 1 rev. 093, Table 21-14.
 */
#define CPUX86_CACHE_NULL		0U
#define CPUX86_CACHE_DATA		1U
#define CPUX86_CACHE_INSTRUCTION	2U
#define CPUX86_CACHE_UNIFIED		3U

/*
 * The flags of leaf 04H, as bits of struct cpux86_cache.flags.
 *
 * Values referenced: Intel SDM Vol. 1 rev. 093, Table 21-14.
 */
#define CPUX86_CACHE_SELF_INIT		0x1U	/* EAX[8] */
#define CPUX86_CACHE_FULLY_ASSOC	0x2U	/* EAX[9] */
#define CPUX86_CACHE_NO_LOWER_FLUSH	0x4U	/* EDX[0] */
#define CPUX86_CACHE_INCLUSIVE		0x8U	/* EDX[1] */
#define CPUX86_CACHE_COMPLEX_INDEX	0x10U	/* EDX[2] */

/*
 * One cache level that leaf 04H describes for one logical processor: its
 * kind and level, its geometry, and how many logical processors share it.
 *
 * sharing_ids is 1 + EAX[25:14]; the processor reserves the next power of
 * two of APIC IDs for them, so two logical processors share this cache
 * when their x2APIC IDs agree above the low share_shift bits.
 */
struct cpux86_cache {
	uint32_t type;			/* CPUX86_CACHE_DATA, _INSTRUCTION or _UNIFIED */
	uint32_t level;			/* EAX[7:5], 1 for the first level */
	uint32_t flags;			/* CPUX86_CACHE_* */
	uint32_t sharing_ids;		/* 1 + EAX[25:14], 1..4096 */
	uint32_t share_shift;		/* cpux86_ceil_log2(sharing_ids), 0..12 */
	uint32_t line_size;		/* 1 + EBX[11:0], bytes */
	uint32_t partitions;		/* 1 + EBX[21:12] */
	uint32_t ways;			/* 1 + EBX[31:22] */
	uint64_t sets;			/* 1 + ECX, up to 2^32 */
	uint64_t size;			/* ways * partitions * line_size * sets, bytes */
};

/*
 * Decodes one sub-leaf of leaf 04H.
 *
 * Input: leaf4, the registers of CPUID leaf 04H for one sub-leaf index.
 * Sub-leaves are read from index 0 until one reports type NULL.
 * Output: *cache.  The size is computed in 64 bits; a product that would
 * not fit gives ERANGE.
 * Precondition: max_basic_leaf >= 04H (the caller checks).
 * Errors: EINVAL for a NULL pointer or a reserved cache type (4..31);
 * ENOTSUP for type NULL (no more caches); ERANGE for a size beyond
 * 64 bits.  *cache is unchanged on every error.
 * Specification: Intel SDM Vol. 1 rev. 093, Table 21-14 and the cache
 * size formula after it.
 */
int
cpux86_cpuid_cache(
	const struct cpux86_cpuid_regs *leaf4,
	struct cpux86_cache *cache);

/*
 * Reports which instance of a cache a logical processor uses.
 *
 * Input: x2apic_id, the logical processor's x2APIC ID; cache, decoded by
 * cpux86_cpuid_cache() on that processor.
 * Output: *group is x2apic_id with its low cache->share_shift bits
 * dropped; two logical processors share the cache exactly when their
 * groups are equal.
 * Errors: EINVAL for a NULL pointer or a share_shift above 31.
 * Specification: Intel SDM Vol. 1 rev. 093, Table 21-14, EAX[25:14].
 */
int
cpux86_cache_group(
	uint32_t x2apic_id,
	const struct cpux86_cache *cache,
	uint32_t *group);

/*
 * The domain types of the extended topology leaves (ECX[15:8]).  The
 * hierarchy is fixed from THREAD (the logical processor) up to DIEGRP;
 * the package is above them all and is not enumerated.  Leaf 0BH defines
 * only THREAD and CORE; other values are reserved.
 *
 * Values referenced: Intel SDM Vol. 1 rev. 093, Table 21-71 (leaf 1FH)
 * and Table 21-32 (leaf 0BH).
 */
#define CPUX86_DOMAIN_INVALID		0U
#define CPUX86_DOMAIN_THREAD		1U
#define CPUX86_DOMAIN_CORE		2U
#define CPUX86_DOMAIN_MODULE		3U
#define CPUX86_DOMAIN_TILE		4U
#define CPUX86_DOMAIN_DIE		5U
#define CPUX86_DOMAIN_DIEGRP		6U

/* The size of arrays indexed by a domain type, index 0 unused. */
#define CPUX86_DOMAIN_COUNT		7U

/*
 * The most sub-leaves of an extended topology leaf the decoder reads,
 * the terminating INVALID one included: six known domain types and room
 * for one unknown one and the terminator.
 */
#define CPUX86_TOPOLOGY_SUBLEAF_MAX	8U

/*
 * The extended topology leaves.
 *
 * Values referenced: Intel SDM Vol. 1 rev. 093, Table 21-31 (0BH) and
 * Table 21-70 (1FH).
 */
#define CPUX86_LEAF_TOPOLOGY		0x0BU
#define CPUX86_LEAF_TOPOLOGY_V2		0x1FU

/*
 * One sub-leaf of an extended topology leaf, decoded as it is.
 */
struct cpux86_topology_level {
	uint32_t level;			/* ECX[7:0], the sub-leaf index */
	uint32_t type;			/* ECX[15:8], CPUX86_DOMAIN_* or a reserved value */
	uint32_t shift;			/* EAX[4:0], x2APIC ID bits below the next higher domain */
	uint32_t logical_count;		/* EBX[15:0], for display only */
	uint32_t x2apic_id;		/* EDX */
};

/*
 * Decodes one sub-leaf of leaf 0BH or 1FH.
 *
 * Input: subleaf, the registers of one sub-leaf.
 * Output: *level, the fields as they are; a type of 0 (INVALID) means the
 * enumeration has ended and is not an error here.
 * Errors: EINVAL for a NULL pointer.
 * Specification: Intel SDM Vol. 1 rev. 093, Table 21-31 (leaf 0BH) and
 * Table 21-70 (leaf 1FH).
 */
int
cpux86_cpuid_topology_level(
	const struct cpux86_cpuid_regs *subleaf,
	struct cpux86_topology_level *level);

/*
 * Where one logical processor sits in its package.
 *
 * present has bit t set for each domain type t that the leaf enumerated
 * and the decoder knows (THREAD and CORE always).  For such a domain,
 * id[t] is the instance number of the domain within the next higher
 * enumerated domain: the x2APIC ID bits from low[t] up to, but not
 * including, high[t].  For an absent domain id[t], low[t] and high[t] are
 * 0.  package is the x2APIC ID with the low package_shift bits dropped.
 */
struct cpux86_topology {
	uint32_t x2apic_id;
	uint32_t present;			/* bit t for domain type t */
	uint32_t id[CPUX86_DOMAIN_COUNT];
	uint32_t low[CPUX86_DOMAIN_COUNT];
	uint32_t high[CPUX86_DOMAIN_COUNT];
	uint32_t package;
	uint32_t package_shift;			/* the last valid sub-leaf's shift */
};

/*
 * Places one logical processor in its package from the sub-leaves of
 * leaf 0BH or 1FH.
 *
 * The sub-leaves describe domains from the logical processor upward; a
 * domain's field in the x2APIC ID runs from the previous sub-leaf's shift
 * (0 for sub-leaf 0) to its own shift, and the package is everything above
 * the last valid sub-leaf's shift.  A domain type the decoder does not
 * know (a reserved value, or a type above CORE in leaf 0BH) is taken as an
 * extension of the last known domain below it: that domain's field grows
 * to the unknown sub-leaf's shift.
 * Input: leaf, CPUX86_LEAF_TOPOLOGY or CPUX86_LEAF_TOPOLOGY_V2;
 * subleaves, the registers of sub-leaves 0, 1, ... as read on one logical
 * processor; count, how many were read (1..CPUX86_TOPOLOGY_SUBLEAF_MAX).
 * The caller reads until a sub-leaf of type INVALID and includes that
 * terminator in count; the first INVALID entry ends the list and entries
 * after it are not read.  A list without an INVALID entry is incomplete
 * (the package's shift would be unknown) and is refused.
 * Output: *topology.
 * Precondition: the leaf is within max_basic_leaf (the caller checks).
 * Errors, *topology unchanged:
 * - EINVAL: a NULL pointer, count 0 or above the maximum, a leaf other
 *   than 0BH and 1FH; no INVALID entry within count; fewer than two
 *   valid sub-leaves (the logical processor and core domains are always
 *   enumerated); a sub-leaf whose ECX[7:0] is not its index; a shift
 *   smaller than the previous sub-leaf's; an x2APIC ID (EDX) that differs
 *   between valid sub-leaves; sub-leaf 0 not of type THREAD or sub-leaf 1
 *   not of type CORE; a known type that repeats or comes below a known
 *   type already seen.
 * - ENOTSUP: sub-leaf 0 is of type INVALID, or its EBX[15:0] is 0 (the
 *   leaf is not valid on this processor); checked before the EINVAL
 *   cases about the list.
 * Specification: Intel SDM Vol. 1 rev. 093, Table 21-31, Table 21-32,
 * Table 21-70, Table 21-71 and the text after it; Vol. 3A §11.9.4 step 3
 * (unknown domains extend the last known domain).
 */
int
cpux86_cpuid_topology(
	uint32_t leaf,
	const struct cpux86_cpuid_regs *subleaves,
	uint32_t count,
	struct cpux86_topology *topology);

/*
 * The processor features of the extended leaves the driver needs.
 */
struct cpux86_extended {
	uint32_t invariant_tsc;		/* leaf 80000007H EDX[8], 0 or 1 */
	uint32_t physical_address_bits;	/* leaf 80000008H EAX[7:0], MAXPHYADDR */
};

/*
 * Decodes leaves 80000007H and 80000008H.
 *
 * Input: leaf7, the registers of leaf 80000007H, or NULL when the leaf is
 * beyond max_extended_leaf; leaf8, the same for leaf 80000008H.
 * Output: *extended; a field whose leaf was NULL is 0.
 * Errors: EINVAL for a NULL extended.
 * Specification: Intel SDM Vol. 1 rev. 093, Table 21-97 and Table 21-98.
 */
int
cpux86_cpuid_extended(
	const struct cpux86_cpuid_regs *leaf7,
	const struct cpux86_cpuid_regs *leaf8,
	struct cpux86_extended *extended);

#endif
