/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PowerVR Series6 cores this driver knows, and how a core says
 * which one it is.
 *
 * A Series6 core is named by four numbers: branch, version, number of
 * scalable units and configuration (B.V.N.C).  Each known core carries
 * the facts the rest of the driver decides by: its feature set, the
 * hardware errata the firmware and the host work around, its memory
 * geometry, its firmware processor and the firmware file it runs.  This
 * part only computes; it holds no lock and no memory, and it builds on
 * the host too.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_S6_CORE_TABLE_H
#define KERN_DRIVERS_GPU_POWERVR_S6_CORE_TABLE_H

#include <stddef.h>
#include <stdint.h>

/*
 * The features a Series6 core may have.
 *
 * Each bit stands for one capability the configuration of a core lists.
 * A capability that is absent from every known core has no bit.
 *
 * Values referenced:
 * Android kernel/x86_64 android-x86_64-fugu-3.10-nougat-mr2 999603b9ab
 * drivers/staging/imgtec/rogue/hwdefs/km/configs/rgxconfig_km_1.V.4.12.h
 * and rgxconfig_km_1.V.4.6.h (the feature lines of each configuration).
 */
#define POWERVR_S6_FEATURE_PERFBUS		0x00000001U
#define POWERVR_S6_FEATURE_TLA			0x00000002U
#define POWERVR_S6_FEATURE_GS_RTA		0x00000004U
#define POWERVR_S6_FEATURE_COMPUTE		0x00000008U
#define POWERVR_S6_FEATURE_COMPUTE_OVERLAP	0x00000010U
#define POWERVR_S6_FEATURE_META			0x00000020U

/*
 * The hardware errata and enhancements a Series6 core carries.
 *
 * An erratum bit means the firmware and the host both build their
 * records with the workaround, so the layout of some command records
 * depends on it.  An enhancement bit means the core has the improved
 * behaviour.
 *
 * Values referenced:
 * Android kernel/x86_64 android-x86_64-fugu-3.10-nougat-mr2 999603b9ab
 * drivers/staging/imgtec/rogue/hwdefs/km/cores/rgxcore_km_1.72.4.12.h
 * and rgxcore_km_1.76.4.6.h (the errata and enhancement lines).
 */
#define POWERVR_S6_ERRATUM_38344		0x00000001U
#define POWERVR_S6_ERRATUM_42480		0x00000002U
#define POWERVR_S6_ERRATUM_44455		0x00000004U
#define POWERVR_S6_ERRATUM_54441		0x00000008U
#define POWERVR_S6_ENHANCEMENT_36400		0x00010000U

/*
 * The firmware processor of every known Series6 core: a META MTP218
 * with no core memory, which reports this value in the version field of
 * its core identity register.
 *
 * Values referenced:
 * Android kernel/x86_64 android-x86_64-fugu-3.10-nougat-mr2 999603b9ab
 * drivers/staging/imgtec/rogue/rgx_meta.h (META_CR_CORE_ID 0x04831000,
 * META_CR_CORE_ID_VER_SHIFT 16, RGX_CR_META_CORE_ID_VALUE 0x19 for MTP218).
 */
#define POWERVR_S6_META_CORE_ID_REGISTER	0x04831000U
#define POWERVR_S6_META_CORE_ID_SHIFT		16U
#define POWERVR_S6_META_MTP218			0x19U

/*
 * The identity registers of a Series6 core.
 *
 * A core of this age has no packed identity: the 64-bit word at 0x0020
 * reads with a zero branch field (bits 63:48).  The branch and version
 * are then the major and minor fields of the revision register at
 * 0x0020, and the unit count and configuration are the two bytes of the
 * configuration field of the identity register at 0x0018.  Every
 * register is read as two 32-bit halves, low half first, as the vendor
 * driver does on this bus.
 *
 * Values referenced:
 * Android kernel/x86_64 android-x86_64-fugu-3.10-oreo-r6 8ff24327ec
 * drivers/staging/imgtec/rogue/hwdefs/km/rgx_cr_defs_km.h
 * (RGX_CR_CORE_ID__PBVNC 0x0020 with BRANCH_ID_SHIFT 48, VERSION_ID_SHIFT
 * 32, NUMBER_OF_SCALABLE_UNITS_SHIFT 16, CONFIG_ID_SHIFT 0, 16 bits each;
 * RGX_CR_CORE_ID 0x0018; RGX_CR_CORE_REVISION 0x0020 with MAJOR_SHIFT 16
 * and MINOR_SHIFT 8, 8 bits each) and hwdefs/km/rgxdefs_km.h
 * (RGX_CR_CORE_ID_CONFIG_N_SHIFT 8, RGX_CR_CORE_ID_CONFIG_C_SHIFT 0).
 */
#define POWERVR_S6_REG_CORE_ID			0x0018U
#define POWERVR_S6_REG_CORE_REVISION		0x0020U
#define POWERVR_S6_PACKED_BRANCH_SHIFT		48U
#define POWERVR_S6_PACKED_VERSION_SHIFT		32U
#define POWERVR_S6_PACKED_UNITS_SHIFT		16U
#define POWERVR_S6_PACKED_FIELD_MASK		0xffffU
#define POWERVR_S6_REVISION_MAJOR_SHIFT		16U
#define POWERVR_S6_REVISION_MINOR_SHIFT		8U
#define POWERVR_S6_CONFIG_UNITS_SHIFT		8U
#define POWERVR_S6_BYTE_MASK			0xffU

/* The four numbers that name a core. */
struct powervr_s6_bvnc {
	uint32_t branch;
	uint32_t version;
	uint32_t units;
	uint32_t config;
};

/*
 * One known Series6 core.
 *
 * An entry is constant and lives in the driver's table for the life of
 * the kernel; a device keeps a pointer to the entry its core matched.
 */
struct powervr_s6_core {
	const char *name;
	struct powervr_s6_bvnc bvnc;
	uint32_t clusters;
	uint32_t slc_bytes;
	uint32_t slc_line_bits;
	uint32_t physical_bits;
	uint32_t virtual_bits;
	uint32_t isp_pipes;
	uint32_t fbcdc_architecture;
	uint32_t meta_core_memory;
	uint32_t meta_core_id;
	uint32_t features;
	uint32_t errata;
	uint32_t core_clock_hz;
	const char *firmware_name;
	int split_register_access;
};

/* Finds the known core with these four numbers, or NULL. */
const struct powervr_s6_core *powervr_s6_core_find(const struct powervr_s6_bvnc *bvnc);

/* Reports how many cores the table knows. */
size_t powervr_s6_core_count(void);

/* Returns the known core at an index of the table, or NULL past its end. */
const struct powervr_s6_core *powervr_s6_core_at(size_t index);

/*
 * Decodes a core's identity from its registers.
 *
 * Packed is the 64-bit word at 0x0020, revision its low 32 bits read as
 * the revision register, and identity the 32-bit register at 0x0018.
 * Returns zero with the four numbers filled, or EINVAL when the words
 * name no core.
 */
int powervr_s6_identity_decode(uint64_t packed, uint32_t identity, struct powervr_s6_bvnc *bvnc);

/*
 * Packs a core name the way the firmware's compatibility record holds it.
 *
 * The branch, unit count and configuration go into one word, one byte
 * each from bit 16 down; the version goes into a four-byte string field
 * as decimal digits.  Returns zero, or EINVAL when a number does not fit.
 */
int powervr_s6_bvnc_pack(const struct powervr_s6_bvnc *bvnc, uint32_t *bnc, char version[4]);

#endif
