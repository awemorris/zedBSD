/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The register map of an Arm Mali Valhall GPU with a command stream
 * front end: the GPU control block, the microcontroller's control, the
 * three interrupt blocks, the address spaces and the doorbells.
 */

#ifndef KERN_DRIVERS_GPU_MALI_REGS_H
#define KERN_DRIVERS_GPU_MALI_REGS_H

/*
 * The GPU control block: identity, features and the present cores.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_gpu_regs.h
 * (GPU_ID, GPU_L2_FEATURES, GPU_CORE_FEATURES, GPU_TILER_FEATURES,
 * GPU_MEM_FEATURES, GPU_MMU_FEATURES, GPU_AS_PRESENT, GPU_CSF_ID,
 * GPU_REVID, GPU_SHADER_PRESENT, GPU_TILER_PRESENT, GPU_L2_PRESENT and
 * the GPU_ID field positions).
 */
#define MALI_REG_GPU_ID			0x000U
#define MALI_REG_L2_FEATURES		0x004U
#define MALI_REG_CORE_FEATURES		0x008U
#define MALI_REG_TILER_FEATURES		0x00cU
#define MALI_REG_MEM_FEATURES		0x010U
#define MALI_REG_MMU_FEATURES		0x014U
#define MALI_REG_AS_PRESENT		0x018U
#define MALI_REG_CSF_ID			0x01cU
#define MALI_REG_REVID			0x280U
#define MALI_REG_SHADER_PRESENT		0x100U
#define MALI_REG_TILER_PRESENT		0x110U
#define MALI_REG_L2_PRESENT		0x120U
#define MALI_ID_ARCH_MAJOR_SHIFT	28U
#define MALI_ID_ARCH_MINOR_SHIFT	24U
#define MALI_ID_ARCH_REV_SHIFT		20U
#define MALI_ID_PRODUCT_MAJOR_SHIFT	16U
#define MALI_ID_VERSION_MAJOR_SHIFT	12U
#define MALI_ID_VERSION_MINOR_SHIFT	4U
#define MALI_ID_NIBBLE_MASK		0xfU
#define MALI_ID_BYTE_MASK		0xffU
#define MALI_MMU_VA_BITS_MASK		0xffU
#define MALI_MMU_PA_BITS_SHIFT		8U

/*
 * The architecture and product the driver knows: Valhall's v10 with a
 * command stream front end, and the G310 among its products.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_hw.c (the arch_v10 entry
 * and the model names of product 2, 3, 4 and 7 of arch 10).
 */
#define MALI_ARCH_VALHALL_CSF		10U
#define MALI_PRODUCT_G710		2U
#define MALI_PRODUCT_G510		3U
#define MALI_PRODUCT_G310		4U
#define MALI_PRODUCT_G610		7U

/*
 * The four registers of each interrupt block, at the block's base: the
 * raw status, the clear, the mask and the masked status.  The GPU, the
 * job (firmware) and the MMU blocks each have one.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_device.h (INT_RAWSTAT,
 * INT_CLEAR, INT_MASK, INT_STAT); panthor_gpu_regs.h (GPU_INT_BASE);
 * panthor_fw_regs.h (JOB_INT_BASE); panthor_mmu_regs.h (MMU_INT_BASE).
 */
#define MALI_INT_RAWSTAT		0x0U
#define MALI_INT_CLEAR			0x4U
#define MALI_INT_MASK			0x8U
#define MALI_INT_STAT			0xcU
#define MALI_GPU_INT_BASE		0x0020U
#define MALI_JOB_INT_BASE		0x1000U
#define MALI_MMU_INT_BASE		0x2000U

/*
 * The GPU block's interrupt bits.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_gpu_regs.h (GPU_IRQ_FAULT,
 * GPU_IRQ_PROTM_FAULT, GPU_IRQ_RESET_COMPLETED, GPU_IRQ_POWER_CHANGED,
 * GPU_IRQ_POWER_CHANGED_ALL, GPU_IRQ_CLEAN_CACHES_COMPLETED,
 * GPU_IRQ_MCU_STATUS_CHANGED).
 */
#define MALI_GPU_IRQ_FAULT		0x00000001U
#define MALI_GPU_IRQ_PROTECTED_FAULT	0x00000002U
#define MALI_GPU_IRQ_RESET_DONE		0x00000100U
#define MALI_GPU_IRQ_POWER_CHANGED	0x00000200U
#define MALI_GPU_IRQ_POWER_CHANGED_ALL	0x00000400U
#define MALI_GPU_IRQ_CACHES_CLEAN	0x00020000U
#define MALI_GPU_IRQ_MCU_STATUS		0x00080000U

/*
 * The GPU command register and its two commands the driver issues: the
 * soft reset, and the cache flush whose payload names what to do to the
 * L2, the load-store and the other caches (clean 1, invalidate 2).
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_gpu_regs.h (GPU_CMD,
 * GPU_CMD_DEF, GPU_SOFT_RESET, GPU_FLUSH_CACHES, CACHE_CLEAN,
 * CACHE_INV, GPU_STATUS, GPU_FAULT_STATUS, GPU_FAULT_ADDR).
 */
#define MALI_REG_COMMAND		0x030U
#define MALI_REG_STATUS			0x034U
#define MALI_REG_FAULT_STATUS		0x03cU
#define MALI_REG_FAULT_ADDRESS		0x040U
#define MALI_COMMAND_SOFT_RESET		0x00000101U
#define MALI_COMMAND_FLUSH_CACHES	0x4U
#define MALI_FLUSH_L2_SHIFT		8U
#define MALI_FLUSH_LSC_SHIFT		12U
#define MALI_FLUSH_OTHER_SHIFT		16U
#define MALI_CACHE_CLEAN		1U
#define MALI_CACHE_INVALIDATE		2U

/*
 * The power manager's key and its override: a write of the key lets the
 * next write of an override register take effect.
 *
 * Values referenced:
 * hardkernel/kernel_common_drivers odroids7d-6.12.y 34daba9
 * drivers/gpu/arm/midgard/hw_access/mali_kbase_hw_access_regmap_legacy.h
 * (PWR_KEY, PWR_OVERRIDE1); drivers/gpu/arm/midgard/platform/devicetree/
 * mali_kbase_runtime_pm.c (mali_hw_init: the key's value).
 */
#define MALI_REG_POWER_KEY		0x050U
#define MALI_REG_POWER_OVERRIDE1	0x058U
#define MALI_POWER_KEY			0x2968a819U

/*
 * The power registers of the three kinds of core: which are ready, the
 * requests to power them on and off, and which are changing.  Each is a
 * 64-bit mask with one bit per core.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_gpu_regs.h (SHADER_READY,
 * TILER_READY, L2_READY, SHADER_PWRON, TILER_PWRON, L2_PWRON,
 * SHADER_PWROFF, TILER_PWROFF, L2_PWROFF, SHADER_PWRTRANS,
 * TILER_PWRTRANS, L2_PWRTRANS).
 */
#define MALI_REG_SHADER_READY		0x140U
#define MALI_REG_TILER_READY		0x150U
#define MALI_REG_L2_READY		0x160U
#define MALI_REG_SHADER_POWER_ON	0x180U
#define MALI_REG_TILER_POWER_ON		0x190U
#define MALI_REG_L2_POWER_ON		0x1a0U
#define MALI_REG_SHADER_POWER_OFF	0x1c0U
#define MALI_REG_TILER_POWER_OFF	0x1d0U
#define MALI_REG_L2_POWER_OFF		0x1e0U
#define MALI_REG_SHADER_TRANSITION	0x200U
#define MALI_REG_TILER_TRANSITION	0x210U
#define MALI_REG_L2_TRANSITION		0x220U

/*
 * How the GPU keeps its caches coherent with the processor's: the
 * protocols it offers and the one chosen.  "None" is the only one the
 * S7D's GPU is wired for.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_gpu_regs.h
 * (GPU_COHERENCY_FEATURES, GPU_COHERENCY_PROTOCOL, GPU_COHERENCY_NONE).
 */
#define MALI_REG_COHERENCY_FEATURES	0x300U
#define MALI_REG_COHERENCY_PROTOCOL	0x304U
#define MALI_COHERENCY_NONE		31U

/*
 * The microcontroller that runs the command stream firmware: its
 * control (disabled, enabled, or automatic restart) and its status.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_fw_regs.h
 * (MCU_CONTROL_BASE, MCU_CONTROL, MCU_STATUS and their values).
 */
#define MALI_REG_MCU_CONTROL		0x700U
#define MALI_REG_MCU_STATUS		0x704U
#define MALI_MCU_DISABLE		0U
#define MALI_MCU_ENABLE			1U
#define MALI_MCU_AUTO			2U
#define MALI_MCU_STATUS_DISABLED	0U
#define MALI_MCU_STATUS_ENABLED		1U
#define MALI_MCU_STATUS_HALT		2U
#define MALI_MCU_STATUS_FATAL		3U

/*
 * The job block's interrupt bits: the global interface's, and one per
 * command stream group slot.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_fw_regs.h
 * (JOB_INT_GLOBAL_IF, JOB_INT_CSG_IF).
 */
#define MALI_JOB_IRQ_GLOBAL		0x80000000U
#define MALI_JOB_IRQ_GROUPS		0x7fffffffU

/*
 * The doorbells: one page each, from 0x80000 on; the global interface
 * rings number 0.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_fw_regs.h (CSF_DOORBELL,
 * CSF_GLB_DOORBELL_ID).
 */
#define MALI_DOORBELL_BASE		0x80000U
#define MALI_DOORBELL_STRIDE		0x10000U
#define MALI_DOORBELL_GLOBAL		0U

/*
 * The address space registers, 0x40 bytes per space from 0x2400.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_mmu_regs.h (MMU_AS_BASE,
 * MMU_AS_SHIFT, AS_TRANSTAB, AS_MEMATTR, AS_LOCKADDR, AS_COMMAND,
 * AS_FAULTSTATUS, AS_FAULTADDRESS, AS_STATUS, AS_TRANSCFG and the
 * AS_COMMAND values).
 */
#define MALI_AS_BASE			0x2400U
#define MALI_AS_STRIDE			0x40U
#define MALI_AS_TRANSLATION_TABLE	0x00U
#define MALI_AS_MEMORY_ATTRIBUTES	0x08U
#define MALI_AS_LOCK_ADDRESS		0x10U
#define MALI_AS_COMMAND			0x18U
#define MALI_AS_FAULT_STATUS		0x1cU
#define MALI_AS_FAULT_ADDRESS		0x20U
#define MALI_AS_STATUS			0x28U
#define MALI_AS_TRANSLATION_CONFIG	0x30U
#define MALI_AS_STATUS_ACTIVE		1U
#define MALI_AS_COMMAND_UPDATE		1U
#define MALI_AS_COMMAND_LOCK		2U
#define MALI_AS_COMMAND_UNLOCK		3U
#define MALI_AS_COMMAND_FLUSH_PT	4U
#define MALI_AS_COMMAND_FLUSH_MEM	5U

/*
 * The translation configuration: AArch64 tables of 4 KiB pages, the
 * input address width as 55 minus the virtual bits, how the table walk
 * caches, and the mode of a space that translates nothing.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_mmu_regs.h
 * (AS_TRANSCFG_ADRMODE_UNMAPPED, AS_TRANSCFG_ADRMODE_AARCH64_4K,
 * AS_TRANSCFG_INA_BITS, AS_TRANSCFG_PTW_MEMATTR_NC,
 * AS_TRANSCFG_PTW_MEMATTR_WB, AS_TRANSCFG_PTW_RA); panthor_mmu.c
 * (panthor_vm_active: INA_BITS(55 - va_bits)).
 */
#define MALI_TRANSCFG_UNMAPPED		1ULL
#define MALI_TRANSCFG_AARCH64_4K	6ULL
#define MALI_TRANSCFG_INPUT_SHIFT	6U
#define MALI_TRANSCFG_INPUT_BASE	55U
#define MALI_TRANSCFG_WALK_UNCACHED	(1ULL << 24)
#define MALI_TRANSCFG_WALK_WRITEBACK	(2ULL << 24)
#define MALI_TRANSCFG_WALK_ALLOCATE	(1ULL << 30)

/*
 * The two memory attributes the driver puts in an address space's eight
 * attribute slots: non-cacheable (slot 0, everything the driver maps,
 * since the GPU does not snoop the processor), and write-back (slot 1,
 * kept for later use).
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_mmu_regs.h
 * (AS_MEMATTR_AARCH64_INNER_OUTER_NC, AS_MEMATTR_AARCH64_INNER_OUTER_WB,
 * AS_MEMATTR_AARCH64_INNER_ALLOC_EXPL, AS_MEMATTR_AARCH64_SH_MIDGARD_INNER);
 * panthor_mmu.c (mair_to_memattr); drivers/iommu/io-pgtable-arm.c
 * (ARM_LPAE_MAIR_ATTR_IDX_NC 0, ARM_LPAE_MAIR_ATTR_IDX_CACHE 1).
 */
#define MALI_MEMATTR_UNCACHED		0x4cULL
#define MALI_MEMATTR_WRITEBACK		0x8fULL
#define MALI_MEMATTR_SLOT_UNCACHED	0U
#define MALI_MEMATTR_SLOT_WRITEBACK	1U

#endif
