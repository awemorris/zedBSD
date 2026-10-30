/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The records the host shares with the PowerVR firmware, and the core's
 * registers the driver programs around it.
 *
 * The records are the firmware's binary interface: the host writes them
 * at the byte offsets the firmware reads, and reads the fields the
 * firmware fills.  They are named here by offset and size only; the
 * driver keeps no structure of its own for them.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_INTERFACE_H
#define KERN_DRIVERS_GPU_POWERVR_INTERFACE_H

/*
 * The circular command buffers and their control blocks.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_rogue_fwif_check.h
 * (struct rogue_fwif_ccb_ctl: write offset 0, read offset 128, wrap mask
 * 132, command size 136, 144 bytes; struct rogue_fwif_kccb_cmd and
 * struct rogue_fwif_fwccb_cmd: type 0, flags 4, 88 bytes); pvr_fw.h
 * (ROGUE_FWIF_KCCB_NUMCMDS_LOG2_DEFAULT 7, ROGUE_FWIF_FWCCB_NUMCMDS_LOG2
 * 5).
 */
#define POWERVR_CCB_WRITE		0U
#define POWERVR_CCB_READ		128U
#define POWERVR_CCB_WRAP_MASK		132U
#define POWERVR_CCB_COMMAND_BYTES	136U
#define POWERVR_CCB_CONTROL_BYTES	144U
#define POWERVR_COMMAND_TYPE		0U
#define POWERVR_COMMAND_FLAGS		4U
#define POWERVR_COMMAND_BYTES		88U
#define POWERVR_KERNEL_CCB_SLOTS	128U
#define POWERVR_FIRMWARE_CCB_SLOTS	32U

/*
 * Command types, and the answer the firmware leaves in a kernel command's
 * return slot.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_rogue_fwif.h
 * (ROGUE_CMD_MAGIC_DWORD 0x2ABC in the upper 16 bits,
 * ROGUE_FWIF_KCCB_CMD_HEALTH_CHECK 115, ROGUE_FWIF_KCCB_RTN_SLOT_CMD_EXECUTED
 * bit 0, ROGUE_FWIF_KCCB_RTN_SLOT_NO_RESPONSE 0).
 */
#define POWERVR_COMMAND_MAGIC		0x2abc0000U
#define POWERVR_COMMAND_MAGIC_MASK	0xffff0000U
#define POWERVR_COMMAND_HEALTH_CHECK	(POWERVR_COMMAND_MAGIC | 115U)
#define POWERVR_RETURN_EXECUTED		0x1U
#define POWERVR_RETURN_NONE		0x0U

/*
 * The value the host leaves in a cleared return slot while it looks for
 * where the firmware's answers go: a pattern no answer has (the executed
 * bit clear), with the slot's number in it.
 */
#define POWERVR_RETURN_MARKER(slot)	(0xdead0000U | ((uint32_t)(slot) << 4))

/*
 * The per-OS initialisation record.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_rogue_fwif_check.h
 * (struct rogue_fwif_osinit, 104 bytes; struct rogue_fwif_compchecks at
 * 40 with its hardware and firmware identity records of 16 bytes each,
 * their layout version first); pvr_rogue_fwif.h
 * (ROGUE_FWIF_COMPCHECKS_LAYOUT_VERSION 3).
 */
#define POWERVR_OS_INIT_KCCB_CONTROL	0U
#define POWERVR_OS_INIT_KCCB		4U
#define POWERVR_OS_INIT_KCCB_RETURNS	8U
#define POWERVR_OS_INIT_FWCCB_CONTROL	12U
#define POWERVR_OS_INIT_FWCCB		16U
#define POWERVR_OS_INIT_HWR_INFO	28U
#define POWERVR_OS_INIT_OS_DATA		36U
#define POWERVR_OS_INIT_HW_IDENTITY	40U
#define POWERVR_OS_INIT_FW_IDENTITY	56U
#define POWERVR_OS_INIT_BYTES		104U
#define POWERVR_IDENTITY_LAYOUT		3U

/*
 * The system initialisation record.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_rogue_fwif_check.h
 * (struct rogue_fwif_sysinit, 544 bytes: the fault page's physical
 * address 0, the PDS and USC execution bases 8 and 16, the core memory
 * data store's firmware address 296, the runtime configuration 316, the
 * trace control 320, the system data 324, the utilisation buffer 328,
 * the initial core clock 344, firmware started 352, the marker 356);
 * pvr_rogue_heap_config.h (ROGUE_PDSCODEDATA_HEAP_BASE 0xDA00000000,
 * ROGUE_USCCODE_HEAP_BASE 0xE000000000).
 */
#define POWERVR_SYSTEM_INIT_FAULT_PAGE	0U
#define POWERVR_SYSTEM_INIT_PDS_BASE	8U
#define POWERVR_SYSTEM_INIT_USC_BASE	16U
#define POWERVR_SYSTEM_INIT_RUNTIME	316U
#define POWERVR_SYSTEM_INIT_TRACE	320U
#define POWERVR_SYSTEM_INIT_SYSTEM_DATA	324U
#define POWERVR_SYSTEM_INIT_UTILISATION	328U
#define POWERVR_SYSTEM_INIT_CORE_CLOCK	344U
#define POWERVR_SYSTEM_INIT_STARTED	352U
#define POWERVR_SYSTEM_INIT_MARKER	356U
#define POWERVR_SYSTEM_INIT_BYTES	544U
#define POWERVR_PDS_HEAP_BASE		0xda00000000ULL
#define POWERVR_USC_HEAP_BASE		0xe000000000ULL

/*
 * The per-OS and system data records the firmware keeps up to date.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_rogue_fwif_check.h
 * (struct rogue_fwif_osdata, 584 bytes: kernel commands executed 568,
 * the power synchronisation object 572; struct rogue_fwif_sysdata, 3624
 * bytes: configuration flags 0); pvr_rogue_fwif.h
 * (ROGUE_FWIF_INICFG_DISABLE_DM_OVERLAP bit 19); pvr_fw.c
 * (ROGUE_FWIF_SLC_MIN_SIZE_FOR_DM_OVERLAP_KB 4).
 */
#define POWERVR_OS_DATA_KCCB_EXECUTED	568U
#define POWERVR_OS_DATA_POWER_SYNC	572U
#define POWERVR_OS_DATA_BYTES		584U
#define POWERVR_SYSTEM_DATA_FLAGS	0U
#define POWERVR_SYSTEM_DATA_BYTES	3624U
#define POWERVR_FLAG_NO_DM_OVERLAP	0x80000U
#define POWERVR_FLAG_POW_RASCALDUST	0x10U

/*
 * The context switch profile in the system flags, and the per-OS flags
 * that enable the context switch of each data master.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/gpu/drm/img/img-rogue/include/rogue/rgx_fwif_km.h
 * (RGXFWIF_INICFG_CTXSWITCH_PROFILE_SHIFT 16,
 * RGXFWIF_CTXSWITCH_PROFILE_MEDIUM_EN 2; RGXFWIF_INICFG_OS_CTXSWITCH_TDM_EN
 * bit 0, _GEOM_EN 1, _3D_EN 2, _CDM_EN 3; RGXFWIF_OSDATA:
 * ui32FwOsConfigFlags first); the vendor driver's debug dump on the
 * board reads system flags 0x00020010 and OS flags 0x0000000F.
 */
#define POWERVR_FLAG_CSW_PROFILE_MEDIUM	0x20000U
#define POWERVR_OS_DATA_CONFIG_FLAGS	0U
#define POWERVR_OS_FLAGS_CSW_ALL	0xfU

/* Whether the data masters may overlap (geometry with another context's 3D): 1, as mainline on a core whose cache is big enough. */
#ifndef POWERVR_DM_OVERLAP
#define POWERVR_DM_OVERLAP		1
#endif

/* Whether the firmware switches contexts (preempts a job for another context's): 0 by default, as mainline. */
#ifndef POWERVR_CONTEXT_SWITCH
#define POWERVR_CONTEXT_SWITCH		0
#endif
#define POWERVR_DM_OVERLAP_MIN_KB	4U

/*
 * The runtime configuration, the utilisation buffer, the recovery
 * information buffer and the connection control.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_rogue_fwif_check.h
 * (struct rogue_fwif_runtime_cfg, 72 bytes: the active power
 * management's latency in milliseconds 0, the persistent latency flag 8, the core clock 12, the default cluster count 16, the watchdog period
 * 28; struct rogue_fwif_gpu_util_fwcb, 10280 bytes, its last word at
 * 10248; struct rogue_fwif_hwrinfobuf, 2336 bytes; struct
 * rogue_fwif_connection_ctl, 16 bytes); pvr_rogue_fwif_common.h
 * (PVR_FWIF_GPU_UTIL_STATE_IDLE 0).
 */
#define POWERVR_RUNTIME_APM_LATENCY	0U
#define POWERVR_RUNTIME_PERSISTENT	8U
#define POWERVR_RUNTIME_CORE_CLOCK	12U
#define POWERVR_RUNTIME_CLUSTERS	16U
#define POWERVR_RUNTIME_WATCHDOG	28U
#define POWERVR_RUNTIME_BYTES		72U
#define POWERVR_UTILISATION_BYTES	10280U
#define POWERVR_HWR_INFO_BYTES		2336U
#define POWERVR_CONNECTION_BYTES	16U

/*
 * The firmware's trace: its control block and the one thread's buffer.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_rogue_fwif_check.h
 * (struct rogue_fwif_tracebuf, 864 bytes: the log type 0, the first
 * thread's space at 8 whose buffer's firmware address is at 4 within it,
 * the size in words 856); pvr_rogue_fwif.h
 * (ROGUE_FW_TRACE_BUF_DEFAULT_SIZE_IN_DWORDS 12000, ROGUE_FW_THREAD_NUM 1,
 * ROGUE_FWIF_LOG_TYPE_NONE 0).
 */
#define POWERVR_TRACE_LOG_TYPE		0U
#define POWERVR_TRACE_BUFFER_ADDRESS	12U
#define POWERVR_TRACE_WORDS_FIELD	856U
#define POWERVR_TRACE_CONTROL_BYTES	864U
#define POWERVR_TRACE_WORDS		12000U
#define POWERVR_FAULT_PAGE_FILL		0xdeadbee0U

/*
 * The core's registers the firmware's start and stop program.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_rogue_cr_defs.h
 * (ROGUE_CR_SOFT_RESET 0x0100 with ..._MASKFULL, ..._PBE2_XE__MASKFULL,
 * ..._GARTEN_EN, ..._RASCAL_CORE_EN and ..._DUST_A_CORE_EN to
 * ..._DUST_H_CORE_EN; ROGUE_CR_SOFT_RESET2 0x0108 with its full mask;
 * ROGUE_CR_SYS_BUS_SECURE 0xA100; ROGUE_CR_SLC_CTRL_MISC 0x3800 with
 * ..._ENABLE_PSG_HAZARD_CHECK_EN, ..._ADDR_DECODE_MODE_PVR_HASH1 and
 * ..._BYPASS_BURST_COMBINER_EN; ROGUE_CR_AXI_ACE_LITE_CONFIGURATION
 * 0x38C0; ROGUE_CR_MTS_SCHEDULE 0x0B00).
 */
#define POWERVR_SOFT_RESET		0x0100U
#define POWERVR_SOFT_RESET_ALL		0x00e7fffffffffc3dULL
#define POWERVR_SOFT_RESET_ALL_PBE2	0xffeffffffffffc3dULL
#define POWERVR_SOFT_RESET_GARTEN	0x0000000100000000ULL
#define POWERVR_SOFT_RESET_RASCAL_DUSTS	0x000001f8e0000000ULL
#define POWERVR_SOFT_RESET2		0x0108U
#define POWERVR_SOFT_RESET2_ALL		0x00000000001fffffULL
#define POWERVR_SYSTEM_BUS_SECURE	0xa100U
#define POWERVR_SLC_CONTROL		0x3800U
#define POWERVR_SLC_HAZARD_CHECK	0x00000002U
#define POWERVR_SLC_HASH_DECODE		0x00200000U
#define POWERVR_SLC_BYPASS_COMBINER	0x00000001U
#define POWERVR_SLC_COMBINER_MIN_BITS	1024U
#define POWERVR_AXI_ACE_LITE		0x38c0U
#define POWERVR_MTS_SCHEDULE		0x0b00U

/*
 * The ACE-Lite domains and cache attributes of the core's bus requests:
 * outer shareable for non-snooping requests, inner shareable and
 * cacheable for coherent ones and for cache maintenance.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_rogue_cr_defs.h
 * (the ROGUE_CR_AXI_ACE_LITE_CONFIGURATION_*_SHIFT fields) and
 * pvr_fw_startstop.c (rogue_axi_ace_list_init: 3 for the non-snooping
 * domains, 2 for the rest).
 */
#define POWERVR_AXI_ACE_LITE_VALUE	((3ULL << 8) | (3ULL << 10) | (2ULL << 12) | (2ULL << 14) | \
					 (2ULL << 16) | (2ULL << 22) | (2ULL << 26) | (2ULL << 30))

/*
 * The MIPS wrapper: its configuration, its four remap windows, the
 * Garten idle control, the debug probe and the interrupt to the host.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_rogue_cr_defs.h
 * (ROGUE_CR_MIPS_WRAPPER_CONFIG 0x0810 with ..._BOOT_ISA_MODE_MICROMIPS,
 * ROGUE_CR_MIPS_ADDR_REMAP1_CONFIG1 0x0818 to
 * ROGUE_CR_MIPS_ADDR_REMAP5_CONFIG2 0x0860 with ..._MODE_ENABLE_EN,
 * ROGUE_CR_MTS_GARTEN_WRAPPER_CONFIG 0x0B50 with ..._IDLE_CTRL_META 0,
 * ROGUE_CR_MIPS_DEBUG_CONFIG 0x08C8, ROGUE_CR_MIPS_WRAPPER_IRQ_STATUS
 * 0x08A8 and ROGUE_CR_MIPS_WRAPPER_IRQ_CLEAR 0x08B0 with their event bit);
 * pvr_rogue_mips.h (ROGUE_MIPSFW_REGISTERS_VIRTUAL_BASE 0xCF800000,
 * ROGUE_MIPSFW_WRAPPER_CONFIG_REGBANK_ADDR_ALIGN 16, the boot, data and
 * code remap input addresses 0x1FC00000, 0x1FC01000 and 0x1FC02000).
 */
#define POWERVR_MIPS_WRAPPER		0x0810U
#define POWERVR_MIPS_MICROMIPS_BOOT	0x00010000U
#define POWERVR_MIPS_REGISTER_WINDOW	0xcf800000U
#define POWERVR_MIPS_REGISTER_ALIGN	16U
#define POWERVR_MIPS_REMAP1_INPUT	0x0818U
#define POWERVR_MIPS_REMAP1_OUTPUT	0x0820U
#define POWERVR_MIPS_REMAP2_INPUT	0x0828U
#define POWERVR_MIPS_REMAP2_OUTPUT	0x0830U
#define POWERVR_MIPS_REMAP3_INPUT	0x0838U
#define POWERVR_MIPS_REMAP3_OUTPUT	0x0840U
#define POWERVR_MIPS_REMAP5_INPUT	0x0858U
#define POWERVR_MIPS_REMAP5_OUTPUT	0x0860U
#define POWERVR_MIPS_REMAP_ENABLE	0x1ULL
#define POWERVR_MIPS_BOOT_INPUT		0x1fc00000ULL
#define POWERVR_MIPS_DATA_INPUT		0x1fc01000ULL
#define POWERVR_MIPS_CODE_INPUT		0x1fc02000ULL
#define POWERVR_GARTEN_WRAPPER		0x0b50U
#define POWERVR_GARTEN_IDLE_BY_FIRMWARE	0x0ULL
#define POWERVR_MIPS_DEBUG		0x08c8U
#define POWERVR_MIPS_IRQ_STATUS		0x08a8U
#define POWERVR_MIPS_IRQ_CLEAR		0x08b0U
#define POWERVR_MIPS_IRQ_EVENT		0x1U

/*
 * The registers the stop waits on before it holds the core in reset.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_rogue_cr_defs.h
 * (ROGUE_CR_MTS_INTCTX_THREAD0_DM_ASSOC 0x0B40 and
 * ROGUE_CR_MTS_BGCTX_THREAD0_DM_ASSOC 0x0B30, ROGUE_CR_BIF_STATUS_MMU
 * 0x1358, ROGUE_CR_BIFPM_STATUS_MMU 0x1350, ROGUE_CR_BIF_READS_EXT_STATUS
 * 0x1320, ROGUE_CR_BIFPM_READS_EXT_STATUS 0x1338, ROGUE_CR_SLC_STATUS1
 * 0x3870, ROGUE_CR_MARS_IDLE 0x08F8 with ..._CPU_EN and
 * ..._MH_SYSARB0_EN, and each one's full mask).
 */
#define POWERVR_MTS_INTERRUPT_ASSOC	0x0b40U
#define POWERVR_MTS_BACKGROUND_ASSOC	0x0b30U
#define POWERVR_BIF_MMU_STATUS		0x1358U
#define POWERVR_BIF_MMU_STATUS_MASK	0xffU
#define POWERVR_BIFPM_MMU_STATUS	0x1350U
#define POWERVR_BIFPM_MMU_STATUS_MASK	0xffU
#define POWERVR_BIF_READS_STATUS	0x1320U
#define POWERVR_BIF_READS_STATUS_MASK	0x0fffffffU
#define POWERVR_BIFPM_READS_STATUS	0x1338U
#define POWERVR_BIFPM_READS_STATUS_MASK	0xffffU
#define POWERVR_SLC_STATUS		0x3870U
#define POWERVR_SLC_STATUS_MASK		0x800003ff03ffffffULL
#define POWERVR_MARS_IDLE		0x08f8U
#define POWERVR_MARS_IDLE_FIRMWARE	0x00000006U

/*
 * The kernel commands the render node sends beyond the health check, and
 * the cleanup's busy answer in a return slot.
 *
 * Values referenced:
 * Linux v7.3
 * drivers/gpu/drm/imagination/pvr_rogue_fwif.h
 * (ROGUE_FWIF_KCCB_CMD_KICK 101, ROGUE_FWIF_KCCB_CMD_MMUCACHE 102,
 * ROGUE_FWIF_KCCB_CMD_CLEANUP 106, each with the magic in the upper 16
 * bits; ROGUE_FWIF_KCCB_RTN_SLOT_CLEANUP_BUSY bit 1;
 * ROGUE_FWIF_CLEANUP_FWCOMMONCONTEXT 0); pvr_rogue_fwif_check.h
 * (the command's data at 8; struct rogue_fwif_kccb_cmd_kick_data: the
 * context 0, the write offset 4, the wrap mask 8, the cleanup count 12;
 * struct rogue_fwif_cleanup_request: the type 0, the context 4).
 */
#define POWERVR_COMMAND_DATA		8U
#define POWERVR_COMMAND_KICK		(POWERVR_COMMAND_MAGIC | 101U)
#define POWERVR_COMMAND_MMU_CACHE	(POWERVR_COMMAND_MAGIC | 102U)
#define POWERVR_COMMAND_CLEANUP		(POWERVR_COMMAND_MAGIC | 106U)
#define POWERVR_RETURN_CLEANUP_BUSY	0x2U
#define POWERVR_KICK_CONTEXT		0U
#define POWERVR_KICK_WRITE_OFFSET	4U
#define POWERVR_KICK_WRAP_MASK		8U
#define POWERVR_KICK_CLEANUPS		12U
#define POWERVR_CLEANUP_TYPE		0U
#define POWERVR_CLEANUP_CONTEXT		4U
#define POWERVR_CLEANUP_COMMON_CONTEXT	0U

/*
 * The MMU cache command: which of the core's MMU caches to drop, and the
 * word the firmware writes once it has.
 *
 * Values referenced:
 * Linux v7.3
 * drivers/gpu/drm/imagination/pvr_rogue_fwif.h
 * (struct rogue_fwif_mmucachedata: the flags 0, the sync word's firmware
 * address 4, its value 8; ROGUE_FWIF_MMUCACHEDATA_FLAGS_PT 1, ..._PD 2,
 * ..._PC 4, ..._TLB 0x18).
 */
#define POWERVR_MMU_CACHE_FLAGS		0U
#define POWERVR_MMU_CACHE_SYNC		4U
#define POWERVR_MMU_CACHE_VALUE		8U
#define POWERVR_MMU_CACHE_ALL		0x1fU

/*
 * The kernel command that writes back and drops the system level cache's
 * lines: whole cache, invalidating as well.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_rogue_fwif.h
 * (ROGUE_FWIF_KCCB_CMD_SLCFLUSHINVAL 105; struct
 * rogue_fwif_slcflushinvaldata: the context 0, invalidate 4, a DM
 * context 8, the range's address 16 and size 24, zero for all).
 */
#define POWERVR_COMMAND_SLC_FLUSH	(POWERVR_COMMAND_MAGIC | 105U)
#define POWERVR_SLC_FLUSH_INVALIDATE	4U

/*
 * Whether a cached buffer's pages wait for a flush of the system level
 * cache before they are given back (1).  With 0 they are given back at
 * once, as the mainline driver frees its buffers without flushing the
 * cache.
 */
#ifndef POWERVR_SLC_FLUSH_ON_FREE
#define POWERVR_SLC_FLUSH_ON_FREE	1
#endif

/*
 * Whether device-local Vulkan memory (only the GPU reaches it) is mapped
 * through the system level cache (1), instead of around it like memory
 * the processor maps; with POWERVR_SLC_FLUSH_ON_FREE its buffers' cached
 * lines are written back and dropped before their pages are given back
 * (into the pool, or freed).  The vertex data and textures of a draw are
 * read many times over; around the cache the geometry of a large model
 * takes about twice as long.
 */
#ifndef POWERVR_DEVICE_MEMORY_CACHED
#define POWERVR_DEVICE_MEMORY_CACHED	1
#endif

/*
 * Whether a job's programs (the data sequencer's and the shading
 * cluster's windows) and its general memory (draw slots, descriptor sets)
 * go through the system level cache.  The processor writes them before
 * the kick and the core only reads them; no flush command is sent, so a
 * place reused for other contents may still read stale lines (a trial).
 */
#ifndef POWERVR_PROGRAM_SLC_CACHED
#define POWERVR_PROGRAM_SLC_CACHED	0
#endif

/*
 * The firmware's notices the host acts on.
 *
 * Values referenced:
 * Linux v7.3
 * drivers/gpu/drm/imagination/pvr_rogue_fwif.h
 * (ROGUE_FWIF_FWCCB_CMD_CONTEXT_RESET_NOTIFICATION 105,
 * ROGUE_FWIF_FWCCB_CMD_REQUEST_GPU_RESTART 109,
 * ROGUE_FWIF_FWCCB_CMD_CONTEXT_FW_PF_NOTIFICATION 112, a page fault of
 * the firmware's own, each with the magic); pvr_rogue_fwif_check.h (struct
 * rogue_fwif_fwccb_cmd_context_reset_data: the context's identity 0).
 */
#define POWERVR_NOTICE_CONTEXT_RESET	(POWERVR_COMMAND_MAGIC | 105U)
#define POWERVR_NOTICE_RESTART		(POWERVR_COMMAND_MAGIC | 109U)
#define POWERVR_NOTICE_FIRMWARE_FAULT	(POWERVR_COMMAND_MAGIC | 112U)
#define POWERVR_NOTICE_CONTEXT_ID	0U

/*
 * One memory context as the firmware sees it.
 *
 * Values referenced:
 * Linux v7.3
 * drivers/gpu/drm/imagination/pvr_rogue_fwif_check.h
 * (struct rogue_fwif_fwmemcontext, 32 bytes: the catalogue's physical
 * address 0, the catalogue register set 8); pvr_rogue_fwif.h
 * (ROGUE_FW_BIF_INVALID_PCSET 0xFFFFFFFF).
 */
#define POWERVR_MEMORY_CONTEXT_ROOT	0U
#define POWERVR_MEMORY_CONTEXT_SET	8U
#define POWERVR_MEMORY_CONTEXT_BYTES	32U
#define POWERVR_NO_REGISTER_SET		0xffffffffU

/*
 * One compute context as the firmware sees it: the common context of its
 * data master first, then the static state, then counters and flags.
 *
 * Values referenced:
 * Linux v7.3
 * drivers/gpu/drm/imagination/pvr_rogue_fwif_check.h
 * (struct rogue_fwif_fwcommoncontext, 152 bytes: the command buffer's
 * control 0 and base 4, the state 24, the priority 32, the data master
 * 60, the deadline 104, the memory context 136, the host's identity 140;
 * struct rogue_fwif_static_computecontext_state, 56 bytes; struct
 * rogue_fwif_compute_ctx_state, 4 bytes); pvr_rogue_fwif.h (struct
 * rogue_fwif_fwcomputecontext: the common context, the static state,
 * then six 32-bit words, aligned to 8); pvr_rogue_fwif_common.h
 * (PVR_FWIF_DM_CDM 4); pvr_queue.c (MAX_DEADLINE_MS 30000,
 * CTX_COMPUTE_CCCB_SIZE_LOG2 15).
 */
#define POWERVR_CONTEXT_CCB_CONTROL	0U
#define POWERVR_CONTEXT_CCB		4U
#define POWERVR_CONTEXT_STATE		24U
#define POWERVR_CONTEXT_PRIORITY	32U
#define POWERVR_CONTEXT_DATA_MASTER	60U
#define POWERVR_CONTEXT_DEADLINE	104U
#define POWERVR_CONTEXT_MEMORY		136U
#define POWERVR_CONTEXT_IDENTITY	140U
#define POWERVR_COMPUTE_CONTEXT_BYTES	232U
#define POWERVR_COMPUTE_STATE_BYTES	4U
#define POWERVR_DATA_MASTER_COMPUTE	4U
#define POWERVR_DEADLINE_MS		30000U
#define POWERVR_CLIENT_CCB_BYTES	0x8000U

/*
 * A context's command buffer: its control block, the header in front of
 * every command, the commands the render node writes, and the sync word
 * a fence waits for or an update writes.
 *
 * Values referenced:
 * Linux v7.3
 * drivers/gpu/drm/imagination/pvr_rogue_fwif_shared_check.h
 * (struct rogue_fwif_cccb_ctl, 32 bytes: the write offset 0, the read
 * offset 4, the dependency offset 8, the wrap mask 12; struct
 * rogue_fwif_ufo, 8 bytes: the address 0, the value 4);
 * pvr_rogue_fwif_check.h (struct rogue_fwif_ccb_cmd_header, 40 bytes:
 * the type 0, the size 4); pvr_rogue_fwif.h
 * (ROGUE_FWIF_CCB_CMD_TYPE_NULL 210 and ROGUE_CCB_TYPE_TASK bit 15,
 * ROGUE_FWIF_CCB_CMD_TYPE_FENCE 212, ..._UPDATE 213, ..._PADDING 221,
 * each with the magic; command sizes aligned to 8).
 */
#define POWERVR_CLIENT_CONTROL_BYTES	32U
#define POWERVR_CLIENT_WRITE		0U
#define POWERVR_CLIENT_READ		4U
#define POWERVR_CLIENT_DEPENDENCY	8U
#define POWERVR_CLIENT_WRAP_MASK	12U
#define POWERVR_HEADER_TYPE		0U
#define POWERVR_HEADER_SIZE		4U
#define POWERVR_HEADER_BYTES		40U
#define POWERVR_CLIENT_NULL		(POWERVR_COMMAND_MAGIC | 0x8000U | 210U)
#define POWERVR_CLIENT_FENCE		(POWERVR_COMMAND_MAGIC | 212U)
#define POWERVR_CLIENT_UPDATE		(POWERVR_COMMAND_MAGIC | 213U)
#define POWERVR_CLIENT_PADDING		(POWERVR_COMMAND_MAGIC | 221U)
#define POWERVR_SYNC_BYTES		8U
#define POWERVR_SYNC_ADDRESS		0U
#define POWERVR_SYNC_VALUE		4U

/*
 * A compute command in a context's command buffer: the command's type,
 * and where the control stream and the context's resume buffer go in it.
 *
 * Values referenced:
 * Linux v7.3
 * drivers/gpu/drm/imagination/pvr_rogue_fwif.h
 * (ROGUE_FWIF_CCB_CMD_TYPE_CDM 205 with ROGUE_CCB_TYPE_TASK and the
 * magic); pvr_rogue_fwif_client_check.h (struct rogue_fwif_cmd_compute,
 * 96 bytes: the registers at 8, the flags 80, the temporary regions 84,
 * the stream start 88, the execute count 92; struct
 * rogue_fwif_compute_regs, 72 bytes: the control stream base 32 and the
 * context state base 40 in it).
 */
#define POWERVR_CLIENT_COMPUTE		(POWERVR_COMMAND_MAGIC | 0x8000U | 205U)
#define POWERVR_COMPUTE_BYTES		96U
#define POWERVR_COMPUTE_STREAM		40U
#define POWERVR_COMPUTE_RESUME		48U

/*
 * The device address window a render context's buffers are mapped in.
 *
 * Values referenced:
 * Linux v7.3
 * drivers/gpu/drm/imagination/pvr_rogue_heap_config.h
 * (ROGUE_GENERAL_HEAP_BASE 0x8000000000, ROGUE_GENERAL_HEAP_SIZE
 * 128 GiB).
 */
#define POWERVR_GENERAL_HEAP_BASE	0x8000000000ULL
#define POWERVR_GENERAL_HEAP_BYTES	0x2000000000ULL

/*
 * The free list requests and the statistics the firmware sends, the
 * kernel commands that answer the requests, and where their fields lie
 * after a command's type and flags.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_rogue_fwif.h
 * (ROGUE_FWIF_FWCCB_CMD_FREELIST_GROW 103,
 * ROGUE_FWIF_FWCCB_CMD_FREELISTS_RECONSTRUCTION 104,
 * ROGUE_FWIF_FWCCB_CMD_UPDATE_STATS 107,
 * ROGUE_FWIF_KCCB_CMD_FREELIST_GROW_UPDATE 110,
 * ROGUE_FWIF_KCCB_CMD_FREELISTS_RECONSTRUCTION_UPDATE 112, each with the
 * magic; struct rogue_fwif_fwccb_cmd_freelist_gs_data: the list's
 * identity 0; struct rogue_fwif_fwccb_cmd_freelists_reconstruction_data:
 * the count 0, the recovery count 4, the identities 8; struct
 * rogue_fwif_fwccb_cmd_update_stats_data: the counter 0, the process 4,
 * the adjustment 8; ROGUE_FWIF_FWCCB_CMD_UPDATE_NUM_PARTIAL_RENDERS 1,
 * ..._NUM_OUT_OF_MEMORY 2); pvr_rogue_fwif_check.h (struct
 * rogue_fwif_freelist_gs_data: the list's firmware address 0, the pages
 * added 4, the pages in all 8, the pages held ready 12, 16 bytes; struct
 * rogue_fwif_freelists_reconstruction_data: the count 0, the identities
 * 4, 76 bytes).
 */
#define POWERVR_NOTICE_FREE_LIST_GROW	(POWERVR_COMMAND_MAGIC | 103U)
#define POWERVR_NOTICE_FREE_LIST_REBUILD	(POWERVR_COMMAND_MAGIC | 104U)
#define POWERVR_NOTICE_STATISTICS	(POWERVR_COMMAND_MAGIC | 107U)
#define POWERVR_COMMAND_GROW_ANSWER	(POWERVR_COMMAND_MAGIC | 110U)
#define POWERVR_COMMAND_REBUILD_ANSWER	(POWERVR_COMMAND_MAGIC | 112U)
#define POWERVR_GROW_REQUEST_LIST	0U
#define POWERVR_REBUILD_REQUEST_COUNT	0U
#define POWERVR_REBUILD_REQUEST_LISTS	8U
#define POWERVR_STATISTICS_COUNTER	0U
#define POWERVR_STATISTICS_ADJUSTMENT	8U
#define POWERVR_STATISTICS_PARTIAL	1U
#define POWERVR_STATISTICS_OUT_OF_MEMORY	2U
#define POWERVR_GROW_ANSWER_LIST	0U
#define POWERVR_GROW_ANSWER_ADDED	4U
#define POWERVR_GROW_ANSWER_PAGES	8U
#define POWERVR_GROW_ANSWER_READY	12U
#define POWERVR_REBUILD_ANSWER_COUNT	0U
#define POWERVR_REBUILD_ANSWER_LISTS	4U

/*
 * The firmware objects a cleanup command names beyond a context: one
 * half of a render target data set, and a free list.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_rogue_fwif.h
 * (enum rogue_fwif_cleanup_type: ROGUE_FWIF_CLEANUP_HWRTDATA 1,
 * ROGUE_FWIF_CLEANUP_FREELIST 2).
 */
#define POWERVR_CLEANUP_TARGET_DATA	1U
#define POWERVR_CLEANUP_FREE_LIST	2U

/*
 * The device address windows of the data sequencer's and the shading
 * cluster's programs and of the transfer queue's fragment streams, and
 * the part at the start of the first two that the firmware's own static
 * programs take (one page each).
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_rogue_heap_config.h
 * (ROGUE_PDSCODEDATA_HEAP_BASE 0xDA00000000 and its size 4 GiB,
 * ROGUE_USCCODE_HEAP_BASE 0xE000000000 and its size 4 GiB,
 * ROGUE_TRANSFER_FRAG_HEAP_BASE 0xE400000000 and its size 16 GiB);
 * pvr_vm.c (static_data_areas: the VDM sync program at 0 and the end of
 * tile program at 128 of the first, the VDM sync program at 0 of the
 * second, each heap's reserved part rounded to a page).
 */
#define POWERVR_PDS_HEAP_BYTES		0x100000000ULL
#define POWERVR_USC_HEAP_BYTES		0x100000000ULL
#define POWERVR_TRANSFER_HEAP_BASE	0xe400000000ULL
#define POWERVR_TRANSFER_HEAP_BYTES	0x400000000ULL

/*
 * The region header window, where a render target's region headers go,
 * as the vendor and Mesa drivers keep them in a heap of their own.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_rogue_heap_config.h
 * (ROGUE_RGNHDR_HEAP_BASE 0xDBF0000000, ROGUE_RGNHDR_HEAP_SIZE 256 MiB);
 * Mesa 25.0.0 src/imagination/vulkan/pvr_job_render.c
 * (pvr_rt_rgn_headers_data_init: the region headers in rgn_hdr_heap);
 * Mesa 25.0.7 src/imagination/csbgen/rogue_cr.xml (TE_PSGREGION_ADDR:
 * the base's bits 6 to 33 only).
 */
#define POWERVR_REGION_HEADER_HEAP_BASE	0xdbf0000000ULL
#define POWERVR_REGION_HEADER_HEAP_BYTES	0x10000000ULL
#define POWERVR_STATIC_AREA_BYTES	0x1000U

#endif
