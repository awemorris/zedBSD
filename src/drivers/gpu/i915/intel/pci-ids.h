/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PCI device ids of the Skylake to Comet Lake, Ice Lake, Tiger Lake,
 * Rocket Lake, Alder Lake and Raptor Lake graphics devices.  Each list applies its
 * argument, a macro, to every id.
 */

#ifndef DRIVERS_GPU_I915_INTEL_PCI_IDS_H
#define DRIVERS_GPU_I915_INTEL_PCI_IDS_H

/*
 * Values referenced: Linux v6.19 include/drm/intel/pciids.h
 * (INTEL_TGL_GT1_IDS).
 */
#define I915_PCI_TGL_GT1_IDS(MACRO__)	MACRO__(0x9A60), \
	MACRO__(0x9A68), \
	MACRO__(0x9A70)
/*
 * Values referenced: Linux v6.19 include/drm/intel/pciids.h
 * (INTEL_TGL_GT2_IDS).
 */
#define I915_PCI_TGL_GT2_IDS(MACRO__)	MACRO__(0x9A40), \
	MACRO__(0x9A49), \
	MACRO__(0x9A59), \
	MACRO__(0x9A78), \
	MACRO__(0x9AC0), \
	MACRO__(0x9AC9), \
	MACRO__(0x9AD9), \
	MACRO__(0x9AF8)
/* Values referenced: Linux v6.19 include/drm/intel/pciids.h (INTEL_TGL_IDS). */
#define I915_PCI_TGL_IDS(MACRO__)	I915_PCI_TGL_GT1_IDS(MACRO__), \
	I915_PCI_TGL_GT2_IDS(MACRO__)
/*
 * Values referenced: Linux v6.8.12 include/drm/i915_pciids.h
 * (INTEL_ICL_PORT_F_IDS, INTEL_ICL_11_IDS).
 */
#define I915_PCI_ICL_IDS(MACRO__)	MACRO__(0x8A50), \
	MACRO__(0x8A52), \
	MACRO__(0x8A53), \
	MACRO__(0x8A54), \
	MACRO__(0x8A56), \
	MACRO__(0x8A57), \
	MACRO__(0x8A58), \
	MACRO__(0x8A59), \
	MACRO__(0x8A5A), \
	MACRO__(0x8A5B), \
	MACRO__(0x8A5C), \
	MACRO__(0x8A70), \
	MACRO__(0x8A71), \
	MACRO__(0x8A51), \
	MACRO__(0x8A5D)
/*
 * Values referenced: Linux v6.8.12 include/drm/i915_pciids.h
 * (INTEL_RKL_IDS).
 */
#define I915_PCI_RKL_IDS(MACRO__)	MACRO__(0x4C80), \
	MACRO__(0x4C8A), \
	MACRO__(0x4C8B), \
	MACRO__(0x4C8C), \
	MACRO__(0x4C90), \
	MACRO__(0x4C9A)
/*
 * Values referenced: Linux v6.8.12 include/drm/i915_pciids.h
 * (INTEL_ADLS_IDS).
 */
#define I915_PCI_ADLS_IDS(MACRO__)	MACRO__(0x4680), \
	MACRO__(0x4682), \
	MACRO__(0x4688), \
	MACRO__(0x468A), \
	MACRO__(0x468B), \
	MACRO__(0x4690), \
	MACRO__(0x4692), \
	MACRO__(0x4693)
/*
 * Values referenced: Linux v6.8.12 include/drm/i915_pciids.h
 * (INTEL_RPLS_IDS).
 */
#define I915_PCI_RPLS_IDS(MACRO__)	MACRO__(0xA780), \
	MACRO__(0xA781), \
	MACRO__(0xA782), \
	MACRO__(0xA783), \
	MACRO__(0xA788), \
	MACRO__(0xA789), \
	MACRO__(0xA78A), \
	MACRO__(0xA78B)
/*
 * Values referenced: Linux v6.19 include/drm/intel/pciids.h (INTEL_ADLP_IDS).
 */
#define I915_PCI_ADLP_IDS(MACRO__)	MACRO__(0x46A0), \
	MACRO__(0x46A1), \
	MACRO__(0x46A2), \
	MACRO__(0x46A3), \
	MACRO__(0x46A6), \
	MACRO__(0x46A8), \
	MACRO__(0x46AA), \
	MACRO__(0x462A), \
	MACRO__(0x4626), \
	MACRO__(0x4628), \
	MACRO__(0x46B0), \
	MACRO__(0x46B1), \
	MACRO__(0x46B2), \
	MACRO__(0x46B3), \
	MACRO__(0x46C0), \
	MACRO__(0x46C1), \
	MACRO__(0x46C2), \
	MACRO__(0x46C3)
/*
 * Values referenced: Linux v6.19 include/drm/intel/pciids.h (INTEL_ADLN_IDS).
 */
#define I915_PCI_ADLN_IDS(MACRO__)	MACRO__(0x46D0), \
	MACRO__(0x46D1), \
	MACRO__(0x46D2), \
	MACRO__(0x46D3), \
	MACRO__(0x46D4)
/*
 * Values referenced: Linux v6.19 include/drm/intel/pciids.h (INTEL_RPLU_IDS).
 */
#define I915_PCI_RPLU_IDS(MACRO__)	MACRO__(0xA721), \
	MACRO__(0xA7A1), \
	MACRO__(0xA7A9), \
	MACRO__(0xA7AC), \
	MACRO__(0xA7AD)
/*
 * Values referenced: Linux v6.19 include/drm/intel/pciids.h (INTEL_RPLP_IDS).
 */
#define I915_PCI_RPLP_IDS(MACRO__)	MACRO__(0xA720), \
	MACRO__(0xA7A0), \
	MACRO__(0xA7A8), \
	MACRO__(0xA7AA), \
	MACRO__(0xA7AB)

/*
 * The Gen9 graphics devices: Skylake, Kaby Lake (with Amber Lake-Y on the
 * Kaby Lake GT), Coffee Lake (with Whiskey Lake and Amber Lake-Y on the
 * Coffee Lake GT) and Comet Lake.  The Gen9 LP parts (Broxton, Apollo Lake,
 * Gemini Lake) are not listed.
 *
 * Comet Lake is a list of its own because its workarounds differ.  The
 * GT3 and GT4 parts have a second video decode engine the driver does not
 * use.
 */

/*
 * Values referenced: Linux v6.8.12 include/drm/i915_pciids.h (INTEL_SKL_IDS).
 */
#define I915_PCI_SKL_IDS(MACRO__)	MACRO__(0x1906), \
	MACRO__(0x1913), \
	MACRO__(0x190E), \
	MACRO__(0x1915), \
	MACRO__(0x1902), \
	MACRO__(0x190A), \
	MACRO__(0x190B), \
	MACRO__(0x1917), \
	MACRO__(0x1916), \
	MACRO__(0x1921), \
	MACRO__(0x191E), \
	MACRO__(0x1912), \
	MACRO__(0x191A), \
	MACRO__(0x191B), \
	MACRO__(0x191D), \
	MACRO__(0x1923), \
	MACRO__(0x1926), \
	MACRO__(0x1927), \
	MACRO__(0x192A), \
	MACRO__(0x192B), \
	MACRO__(0x192D), \
	MACRO__(0x1932), \
	MACRO__(0x193A), \
	MACRO__(0x193B), \
	MACRO__(0x193D)

/*
 * include/drm/i915_pciids.h:394-447, 475-480
 *
 * Values referenced: Linux v6.8.12 include/drm/i915_pciids.h (INTEL_KBL_IDS).
 */
#define I915_PCI_KBL_IDS(MACRO__)	MACRO__(0x5906), \
	MACRO__(0x5913), \
	MACRO__(0x590E), \
	MACRO__(0x5915), \
	MACRO__(0x5902), \
	MACRO__(0x5908), \
	MACRO__(0x590A), \
	MACRO__(0x590B), \
	MACRO__(0x5916), \
	MACRO__(0x5921), \
	MACRO__(0x591E), \
	MACRO__(0x5912), \
	MACRO__(0x5917), \
	MACRO__(0x591A), \
	MACRO__(0x591B), \
	MACRO__(0x591D), \
	MACRO__(0x5926), \
	MACRO__(0x5923), \
	MACRO__(0x5927), \
	MACRO__(0x593B), \
	MACRO__(0x591C), \
	MACRO__(0x87C0)

/*
 * include/drm/i915_pciids.h:451-453, 482-525: Coffee Lake, Whiskey Lake and
 * Amber Lake-Y
 *
 * Values referenced: Linux v6.8.12 include/drm/i915_pciids.h (INTEL_CFL_IDS).
 */
#define I915_PCI_CFL_IDS(MACRO__)	MACRO__(0x3E90), \
	MACRO__(0x3E93), \
	MACRO__(0x3E99), \
	MACRO__(0x3E91), \
	MACRO__(0x3E92), \
	MACRO__(0x3E96), \
	MACRO__(0x3E98), \
	MACRO__(0x3E9A), \
	MACRO__(0x3E9C), \
	MACRO__(0x3E94), \
	MACRO__(0x3E9B), \
	MACRO__(0x3EA9), \
	MACRO__(0x3EA5), \
	MACRO__(0x3EA6), \
	MACRO__(0x3EA7), \
	MACRO__(0x3EA8), \
	MACRO__(0x3EA1), \
	MACRO__(0x3EA4), \
	MACRO__(0x3EA0), \
	MACRO__(0x3EA3), \
	MACRO__(0x3EA2), \
	MACRO__(0x87CA)

/*
 * Comet Lake.
 *
 * Values referenced: Linux v6.8.12 include/drm/i915_pciids.h
 * (INTEL_CML_GT1_IDS, INTEL_CML_U_GT1_IDS, INTEL_CML_GT2_IDS,
 * INTEL_CML_U_GT2_IDS).
 */
#define I915_PCI_CML_IDS(MACRO__)	MACRO__(0x9BA2), \
	MACRO__(0x9BA4), \
	MACRO__(0x9BA5), \
	MACRO__(0x9BA8), \
	MACRO__(0x9B21), \
	MACRO__(0x9BAA), \
	MACRO__(0x9BAC), \
	MACRO__(0x9BC2), \
	MACRO__(0x9BC4), \
	MACRO__(0x9BC5), \
	MACRO__(0x9BC6), \
	MACRO__(0x9BC8), \
	MACRO__(0x9BE6), \
	MACRO__(0x9BF6), \
	MACRO__(0x9B41), \
	MACRO__(0x9BCA), \
	MACRO__(0x9BCC)

#endif /* DRIVERS_GPU_I915_INTEL_PCI_IDS_H */
