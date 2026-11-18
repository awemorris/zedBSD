/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The table of known PowerVR Series6 cores and the decoding of a core's
 * identity registers.
 */

#include "drivers/gpu/powervr/s6/core-table.h"
#include <uapi/errno.h>

/* The most decimal digits a version takes in a compatibility record. */
#define VERSION_DIGITS_MAX	3U

/*
 * The cores this driver knows.
 *
 * The G6430 of Moorefield (Atom Z3560 and Z3580) and the G6400 of
 * Merrifield (Atom Z34xx).  Both have four clusters, a META MTP218
 * firmware processor without core memory, 40-bit physical and virtual
 * addresses and the same feature lines but for the frame buffer
 * compression of the G6430.  The firmware file is the one the vendor
 * kernel of the same release loads by this name, without a core suffix.
 * The core clock is the one the vendor platform hands the firmware; the
 * table is constant and read without a lock.
 *
 * Values referenced:
 * Android kernel/x86_64 android-x86_64-fugu-3.10-nougat-mr2 999603b9ab
 * drivers/staging/imgtec/rogue/hwdefs/km/cores/rgxcore_km_1.72.4.12.h
 * (FIX_HW_BRN_44455, FIX_HW_BRN_54441, HW_ERN_36400),
 * hwdefs/km/cores/rgxcore_km_1.76.4.6.h (FIX_HW_BRN_38344, 42480, 44455,
 * 54441, HW_ERN_36400), hwdefs/km/configs/rgxconfig_km_1.V.4.12.h
 * (RGX_FEATURE_NUM_CLUSTERS 4, SLC_SIZE_IN_BYTES 256 KiB,
 * SLC_CACHE_LINE_SIZE_BITS 512, PHYS_BUS_WIDTH 40,
 * VIRTUAL_ADDRESS_SPACE_BITS 40, NUM_ISP_IPP_PIPES 3, META MTP218,
 * META_COREMEM_SIZE 0, FBCDC_ARCHITECTURE 1, PERFBUS, TLA, GS_RTA_SUPPORT,
 * COMPUTE, COMPUTE_OVERLAP), hwdefs/km/configs/rgxconfig_km_1.V.4.6.h
 * (the same with SLC_SIZE_IN_BYTES 128 KiB and no FBCDC line),
 * config_kernel.h (RGX_FW_FILENAME "rgx.fw.signed") and intel/sysinfo.h
 * (RGX_CORE_CLOCK_SPEED_DEFAULT 400000000).
 */
static const struct powervr_s6_core known_cores[] = {
	{
		"G6430",
		{ 1, 72, 4, 12 },
		4,
		256U * 1024U,
		512,
		40,
		40,
		3,
		1,
		0,
		POWERVR_S6_META_MTP218,
		POWERVR_S6_FEATURE_PERFBUS |
		    POWERVR_S6_FEATURE_TLA |
		    POWERVR_S6_FEATURE_GS_RTA |
		    POWERVR_S6_FEATURE_COMPUTE |
		    POWERVR_S6_FEATURE_COMPUTE_OVERLAP |
		    POWERVR_S6_FEATURE_META,
		POWERVR_S6_ERRATUM_44455 |
		    POWERVR_S6_ERRATUM_54441 |
		    POWERVR_S6_ENHANCEMENT_36400,
		400000000U,
		"rgx.fw.signed",
		1
	},
	{
		"G6400",
		{ 1, 76, 4, 6 },
		4,
		128U * 1024U,
		512,
		40,
		40,
		3,
		0,
		0,
		POWERVR_S6_META_MTP218,
		POWERVR_S6_FEATURE_PERFBUS |
		    POWERVR_S6_FEATURE_TLA |
		    POWERVR_S6_FEATURE_GS_RTA |
		    POWERVR_S6_FEATURE_COMPUTE |
		    POWERVR_S6_FEATURE_COMPUTE_OVERLAP |
		    POWERVR_S6_FEATURE_META,
		POWERVR_S6_ERRATUM_38344 |
		    POWERVR_S6_ERRATUM_42480 |
		    POWERVR_S6_ERRATUM_44455 |
		    POWERVR_S6_ERRATUM_54441 |
		    POWERVR_S6_ENHANCEMENT_36400,
		400000000U,
		"rgx.fw.signed",
		1
	}
};

static int same_bvnc(const struct powervr_s6_bvnc *left, const struct powervr_s6_bvnc *right);

/*
 * Finds the known core with these four numbers.
 *
 * NULL means the core is not one this driver can run.
 */
const struct powervr_s6_core *
powervr_s6_core_find(
	const struct powervr_s6_bvnc *bvnc)
{
	size_t index;
	int same;

	/* Walks the table for an entry with the same four numbers. */
	for (index = 0; index < sizeof(known_cores) / sizeof(known_cores[0]); index++) {
		same = same_bvnc(&known_cores[index].bvnc, bvnc);
		if (same)
			return &known_cores[index];
	}

	/* No known core has these numbers. */
	return NULL;
}

/*
 * Reports how many cores the table knows.
 */
size_t
powervr_s6_core_count(void)
{
	/* The number of entries of the constant table. */
	return sizeof(known_cores) / sizeof(known_cores[0]);
}

/*
 * Returns the known core at an index of the table.
 */
const struct powervr_s6_core *
powervr_s6_core_at(
	size_t index)
{
	/* An index past the table names no core. */
	if (index >= sizeof(known_cores) / sizeof(known_cores[0]))
		return NULL;

	/* The entry at the index. */
	return &known_cores[index];
}

/*
 * Decodes a core's identity from its registers.
 *
 * A packed word with a non-zero branch field is a later core's packed
 * identity and is decoded field by field; otherwise the revision and the
 * identity register give the four numbers the Series6 way.
 *
 * Procedure referenced:
 * Android kernel/x86_64 android-x86_64-fugu-3.10-oreo-r6 8ff24327ec
 * drivers/staging/imgtec/rogue/rgxinit.c (the reading of the core's
 * numbers from its registers before the device is powered down).
 */
int
powervr_s6_identity_decode(
	uint64_t packed,
	uint32_t identity,
	struct powervr_s6_bvnc *bvnc)
{
	uint32_t revision;
	uint32_t branch;

	/* Takes the branch field of the packed word. */
	branch = (uint32_t)(packed >> POWERVR_S6_PACKED_BRANCH_SHIFT) & POWERVR_S6_PACKED_FIELD_MASK;

	/* A later core names itself in the packed word. */
	if (branch != 0) {
		bvnc->branch = branch;
		bvnc->version = (uint32_t)(packed >> POWERVR_S6_PACKED_VERSION_SHIFT) & POWERVR_S6_PACKED_FIELD_MASK;
		bvnc->units = (uint32_t)(packed >> POWERVR_S6_PACKED_UNITS_SHIFT) & POWERVR_S6_PACKED_FIELD_MASK;
		bvnc->config = (uint32_t)packed & POWERVR_S6_PACKED_FIELD_MASK;
		return 0;
	}

	/*
	 * A Series6 core: the major and minor revision are the branch and
	 * version, the two bytes of the configuration field the unit count
	 * and the configuration.
	 */
	revision = (uint32_t)packed;
	bvnc->branch = (revision >> POWERVR_S6_REVISION_MAJOR_SHIFT) & POWERVR_S6_BYTE_MASK;
	bvnc->version = (revision >> POWERVR_S6_REVISION_MINOR_SHIFT) & POWERVR_S6_BYTE_MASK;
	bvnc->units = (identity >> POWERVR_S6_CONFIG_UNITS_SHIFT) & POWERVR_S6_BYTE_MASK;
	bvnc->config = identity & POWERVR_S6_BYTE_MASK;

	/* A zero branch in both forms is a register that reads as nothing. */
	if (bvnc->branch == 0)
		return EINVAL;

	/* Succeeded: the four numbers are the core's. */
	return 0;
}

/*
 * Packs a core name the way the firmware's compatibility record holds it.
 *
 * Procedure referenced:
 * Android kernel/x86_64 android-x86_64-fugu-3.10-nougat-mr2 999603b9ab
 * drivers/staging/imgtec/rogue/rgx_compat_bvnc.c (rgx_bvnc_packed: B, N
 * and C one byte each at bits 23:16, 15:8 and 7:0, the version as its
 * decimal characters from the first byte of the string field).
 */
int
powervr_s6_bvnc_pack(
	const struct powervr_s6_bvnc *bvnc,
	uint32_t *bnc,
	char version[4])
{
	char digits[VERSION_DIGITS_MAX];
	uint32_t value;
	unsigned count;
	unsigned index;

	/* Refuses a number that does not fit its byte. */
	if (bvnc->branch > POWERVR_S6_BYTE_MASK ||
	    bvnc->units > POWERVR_S6_BYTE_MASK ||
	    bvnc->config > POWERVR_S6_BYTE_MASK)
		return EINVAL;

	/* Refuses a version with more digits than the string field holds. */
	if (bvnc->version > 999U)
		return EINVAL;

	/* Packs the three bytes of the word. */
	*bnc = (bvnc->branch << 16) | (bvnc->units << 8) | bvnc->config;

	/* Writes the version's digits from the least significant up. */
	value = bvnc->version;
	count = 0;
	do {
		digits[count] = (char)('0' + value % 10U);
		count++;
		value /= 10U;
	} while (value != 0);

	/* Clears the string field, so the digits end with a zero byte. */
	for (index = 0; index < 4; index++)
		version[index] = 0;

	/* Stores the digits most significant first. */
	for (index = 0; index < count; index++)
		version[index] = digits[count - 1 - index];

	/* Succeeded: the record's word and string are filled. */
	return 0;
}

/* Tells whether two core names are the same. */
static int
same_bvnc(
	const struct powervr_s6_bvnc *left,
	const struct powervr_s6_bvnc *right)
{
	/* Each of the four numbers has to agree. */
	if (left->branch != right->branch)
		return 0;
	if (left->version != right->version)
		return 0;
	if (left->units != right->units)
		return 0;
	if (left->config != right->config)
		return 0;

	/* All four agree. */
	return 1;
}
