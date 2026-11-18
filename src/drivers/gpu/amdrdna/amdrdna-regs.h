/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The registers the amdrdna driver reads to find and describe an RDNA GPU.
 *
 * A register is named by the block it belongs to, the window of that block
 * it lives in, and its dword offset inside the window.  The GPU's own
 * table of IP blocks says where each window starts, so the absolute dword
 * index is that start plus the offset; the byte offset in the register BAR
 * is the index times four.  A few registers are read before the table is
 * known, and those carry their absolute index.
 */

#ifndef DRIVERS_GPU_AMDRDNA_AMDRDNA_REGS_H
#define DRIVERS_GPU_AMDRDNA_AMDRDNA_REGS_H

/*
 * The PCI identity: the vendor, the display class and the extended
 * capability that lets the VRAM window be resized.
 *
 * Values referenced: PCI Express Base Specification (Resizable BAR Extended
 * Capability: capability ID 0x0015, one 8-byte entry per resizable BAR
 * with its capability register at +4 and its control register at +8; the
 * control register holds the BAR index in bits 2:0, the number of entries
 * in bits 7:5 of the first one, and the size in bits 13:8 as log2 of the
 * size in MiB) and the PCI code and ID assignment (vendor 0x1002, display
 * class 0x03).
 */
#define AMDRDNA_PCI_VENDOR			0x1002U
#define AMDRDNA_PCI_CLASS_DISPLAY		0x030000U
#define AMDRDNA_PCI_CLASS_DISPLAY_MASK		0xff0000U
#define AMDRDNA_PCI_RESIZE_CAPABILITY		0x0015U
#define AMDRDNA_PCI_RESIZE_SIZES		0x4U
#define AMDRDNA_PCI_RESIZE_CONTROL		0x8U
#define AMDRDNA_PCI_RESIZE_CONTROL_INDEX	0x7U
#define AMDRDNA_PCI_RESIZE_CONTROL_COUNT_SHIFT	5U
#define AMDRDNA_PCI_RESIZE_CONTROL_COUNT_MASK	0x7U
#define AMDRDNA_PCI_RESIZE_CONTROL_SIZE_SHIFT	8U
#define AMDRDNA_PCI_RESIZE_CONTROL_SIZE_MASK	0x3fU
#define AMDRDNA_PCI_RESIZE_ENTRY_BYTES		8U

/*
 * The BARs of an RDNA function: the CPU's window on the VRAM, the doorbell
 * page and the register window.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_device.c (the register BAR of chips
 * from Bonaire on), amdgpu_doorbell_mgr.c (the doorbell BAR) and
 * gmc_v10_0.c (the VRAM aperture BAR).
 */
#define AMDRDNA_BAR_VRAM			0U
#define AMDRDNA_BAR_DOORBELL			2U
#define AMDRDNA_BAR_REGISTERS			5U

/*
 * The size of the register window this driver maps, and the part of the
 * VRAM window it maps for the CPU (the first 1 GiB of a larger window:
 * the kernel's device mappings share a 2 GiB space).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/nbio_v2_3.c (the register hole at the end of
 * the first 512 KiB of the register window).
 * derived: the register window spans 0x80000 bytes; the CPU window is
 * at most 0x40000000 bytes.
 */
#define AMDRDNA_REGISTER_WINDOW_BYTES		0x80000U
#define AMDRDNA_CPU_WINDOW_BYTES		0x40000000ULL

/*
 * The registers read before the block table is known: the indirect VRAM
 * window (address low, data, address high), the VRAM size in MiB and the
 * word the platform security processor sets once the board firmware is up.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c (mmMM_INDEX, mmMM_DATA,
 * mmMM_INDEX_HI, mmRCC_CONFIG_MEMSIZE, mmMP0_SMN_C2PMSG_33).
 */
#define AMDRDNA_NBIO_REG_VRAM_WINDOW_LOW	0x0000U
#define AMDRDNA_NBIO_REG_VRAM_WINDOW_DATA	0x0001U
#define AMDRDNA_NBIO_REG_VRAM_WINDOW_HIGH	0x0006U
#define AMDRDNA_NBIO_REG_VRAM_SIZE_MIB		0x0de3U
#define AMDRDNA_PSP_REG_EARLY_INIT_DONE		0x16061U

/*
 * The indirect VRAM window: the low address word carries the request bit
 * and the low 31 address bits, the high word the bits above them.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_device.c (amdgpu_device_mm_access).
 */
#define AMDRDNA_VRAM_WINDOW_REQUEST		0x80000000U
#define AMDRDNA_VRAM_WINDOW_LOW_MASK		0x7fffffffULL
#define AMDRDNA_VRAM_WINDOW_HIGH_SHIFT		31U

/*
 * The indirect register window for addresses outside the register BAR:
 * the address register and the data register (NBIO window 0).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/nbio/nbio_2_3_offset.h
 * (mmPCIE_INDEX2, mmPCIE_DATA2).
 */
#define AMDRDNA_NBIO_WINDOW_FAR			0U
#define AMDRDNA_NBIO_REG_FAR_ADDRESS		0x000eU
#define AMDRDNA_NBIO_REG_FAR_DATA		0x000fU

/*
 * The scratch words the video BIOS leaves for the driver (NBIO window 1):
 * the first one, the seventh, whose bit 9 says the board was posted, and
 * the sixth, whose bit 4 says a driver owns the display.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/nbio/nbio_2_3_offset.h
 * (mmBIOS_SCRATCH_0) and drivers/gpu/drm/amd/include/atombios.h
 * (ATOM_S7_ASIC_INIT_COMPLETE_MASK, ATOM_S6_ACC_MODE).
 */
#define AMDRDNA_NBIO_WINDOW_SCRATCH		1U
#define AMDRDNA_NBIO_REG_BIOS_SCRATCH_FIRST	0x0038U
#define AMDRDNA_BIOS_SCRATCH_ACCELERATED	6U
#define AMDRDNA_BIOS_SCRATCH_POST		7U
#define AMDRDNA_BIOS_SCRATCH_7_POSTED		0x00000200U
#define AMDRDNA_BIOS_SCRATCH_6_DRIVER_OWNS	0x00000010U

/*
 * The security processor's mailbox words (MP0 window 0): the board
 * firmware's ready word, the boot loader's command word and the word the
 * secure OS sets while it runs.  Bit 31 of the first two means done.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/mp/mp_11_0_offset.h
 * (mmMP0_SMN_C2PMSG_33, mmMP0_SMN_C2PMSG_35, mmMP0_SMN_C2PMSG_81) and
 * drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c (the bit-31 test of
 * C2PMSG_33).
 */
#define AMDRDNA_PSP_WINDOW_MAILBOX		0U
#define AMDRDNA_PSP_REG_PLATFORM_READY		0x0061U
#define AMDRDNA_PSP_REG_BOOT_COMMAND		0x0063U
#define AMDRDNA_PSP_REG_SECURE_OS_ALIVE		0x0091U
#define AMDRDNA_PSP_DONE			0x80000000U

/*
 * The system-management unit's firmware state word, reached through the
 * indirect window; bit 0 says its firmware runs.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/inc/smu_v11_0.h (MP1_Public,
 * smnMP1_FIRMWARE_FLAGS) and
 * drivers/gpu/drm/amd/include/asic_reg/mp/mp_11_0_sh_mask.h
 * (MP1_FIRMWARE_FLAGS__INTERRUPTS_ENABLED_MASK).
 * derived: 0x03b00000 | 0x03010024 = 0x03b10024.
 */
#define AMDRDNA_SMU_FAR_FIRMWARE_STATE		0x03b10024U
#define AMDRDNA_SMU_FIRMWARE_RUNNING		0x00000001U

/*
 * The graphics block's status word and the frame-buffer placement words
 * (GC window 0); the placement words count 16 MiB units.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_1_0_offset.h
 * (mmGRBM_STATUS, mmGCMC_VM_FB_LOCATION_BASE, mmGCMC_VM_FB_LOCATION_TOP,
 * mmGCMC_VM_FB_OFFSET) and gc_10_1_0_sh_mask.h
 * (GCMC_VM_FB_LOCATION_BASE__FB_BASE_MASK).
 */
#define AMDRDNA_GC_WINDOW_MAIN			0U
#define AMDRDNA_GC_REG_BLOCK_STATUS		0x0da4U
#define AMDRDNA_GC_REG_FRAME_BUFFER_BOTTOM	0x1720U
#define AMDRDNA_GC_REG_FRAME_BUFFER_TOP		0x1721U
#define AMDRDNA_GC_REG_FRAME_BUFFER_PHYSICAL	0x170bU
#define AMDRDNA_GC_FRAME_BUFFER_MASK		0x00ffffffU
#define AMDRDNA_GC_FRAME_BUFFER_SHIFT		24U

/*
 * The graphics block's other status words (GC window 0): the second
 * word (the command processors, the caches, the memory hub) and those of
 * shader engines 0 and 1, which say which units still work when a run
 * does not end.
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10.json
 * (GRBM_STATUS 0x8010, GRBM_STATUS2 0x8008, GRBM_STATUS_SE0 0x8014,
 * GRBM_STATUS_SE1 0x8018).
 * derived: each byte offset / 4 less the window's base 0x1260 (0x8010 / 4
 * - 0x1260 = 0x0da4, mmGRBM_STATUS).
 */
#define AMDRDNA_GC_REG_BLOCK_STATUS2		0x0da2U
#define AMDRDNA_GC_REG_ENGINE0_STATUS		0x0da5U
#define AMDRDNA_GC_REG_ENGINE1_STATUS		0x0da6U

/*
 * More status words of the graphics block (GC window 0): the third
 * status word, the command processor's busy and status words, and the
 * clipper's and the setup unit's (busy in bit 31).
 *
 * Values referenced: Linux v6.12
 * drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_1_0_offset.h
 * (mmGRBM_STATUS3 0x0da7, mmCP_BUSY_STAT 0x0f3f, mmCP_STAT 0x0f40,
 * mmPA_CL_CNTL_STATUS 0x1024, mmPA_SU_CNTL_STATUS 0x1034).
 */
#define AMDRDNA_GC_REG_BLOCK_STATUS3		0x0da7U
#define AMDRDNA_GC_REG_CP_BUSY			0x0f3fU
#define AMDRDNA_GC_REG_CP_STATUS		0x0f40U
#define AMDRDNA_GC_REG_CLIP_STATUS		0x1024U
#define AMDRDNA_GC_REG_SETUP_STATUS		0x1034U

/*
 * The shader sequencer's indirect window on a wave of the array the
 * array select names (GC window 0): the index word (the wave in bits 4:0,
 * the register in 31:16) and the data word; and the wave registers read
 * through it: the status (valid in bit 16, in a barrier 12, export ready
 * 8, must export 27), the program counter's low and high words, the
 * instruction at it, the lane mask and the hardware identity.
 *
 * Values referenced: Linux v6.12
 * drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_1_0_offset.h
 * (mmSQ_IND_INDEX 0x1118, mmSQ_IND_DATA 0x1119, ixSQ_WAVE_STATUS 0x0102,
 * ixSQ_WAVE_PC_LO 0x0108, ixSQ_WAVE_PC_HI 0x0109, ixSQ_WAVE_INST_DW0
 * 0x010a, ixSQ_WAVE_HW_ID1 0x0117, ixSQ_WAVE_EXEC_LO 0x027e) and
 * gc_10_1_0_sh_mask.h (SQ_IND_INDEX__WAVE_ID__SHIFT 0,
 * SQ_IND_INDEX__INDEX__SHIFT 16, SQ_WAVE_STATUS__VALID__SHIFT 16).
 */
#define AMDRDNA_GC_REG_WAVE_INDEX		0x1118U
#define AMDRDNA_GC_REG_WAVE_DATA		0x1119U
#define AMDRDNA_GC_WAVE_REGISTER_SHIFT		16U
#define AMDRDNA_WAVE_STATUS			0x0102U
#define AMDRDNA_WAVE_PC_LOW			0x0108U
#define AMDRDNA_WAVE_PC_HIGH			0x0109U
#define AMDRDNA_WAVE_INSTRUCTION		0x010aU
#define AMDRDNA_WAVE_IDENTITY			0x0117U
#define AMDRDNA_WAVE_EXEC			0x027eU
#define AMDRDNA_WAVE_STATUS_VALID		(1U << 16)

/*
 * The flash ROM's address and data registers (SMUIO window 0); each read
 * of the data register returns the next dword of the ROM.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/smuio/smuio_11_0_0_offset.h
 * (mmROM_INDEX, mmROM_DATA).
 */
#define AMDRDNA_SMUIO_WINDOW_MAIN		0U
#define AMDRDNA_SMUIO_REG_ROM_ADDRESS		0x00e4U
#define AMDRDNA_SMUIO_REG_ROM_DATA		0x00e5U

/*
 * Where the GPU keeps its table of IP blocks and its boot database: both
 * near the top of VRAM.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.h (DISCOVERY_TMR_SIZE,
 * DISCOVERY_TMR_OFFSET) and drivers/gpu/drm/amd/amdgpu/amdgpu_psp.h
 * (PSP_RUNTIME_DB_OFFSET).
 */
#define AMDRDNA_BLOCK_TABLE_FROM_TOP		0x10000U
#define AMDRDNA_BLOCK_TABLE_BYTES		0x2800U
#define AMDRDNA_BOOT_DATABASE_FROM_TOP		0x100000U

/*
 * The host bridge's registers the memory stage uses (NBIO window 2): the
 * doorbell aperture switch (bit 0 lets the doorbell BAR through), the
 * interrupt control word, the dummy-read page of the interrupt controller
 * (its address shifted right by 8), the host data path's write flush, and
 * the doorbell range that routes the interrupt controller's doorbell.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/nbio/nbio_2_3_offset.h
 * (mmRCC_DEV0_EPF0_RCC_DOORBELL_APER_EN, mmINTERRUPT_CNTL,
 * mmINTERRUPT_CNTL2, mmBIF_BX_PF_HDP_MEM_COHERENCY_FLUSH_CNTL,
 * mmBIF_IH_DOORBELL_RANGE), nbio_2_3_sh_mask.h
 * (RCC_DEV0_EPF0_RCC_DOORBELL_APER_EN__BIF_DOORBELL_APER_EN) and
 * drivers/gpu/drm/amd/amdgpu/nbio_v2_3.c (nbio_v2_3_ih_control,
 * nbio_v2_3_get_rmmio_remap: the flush register when the remapped one is
 * not set up).
 */
#define AMDRDNA_NBIO_WINDOW_HOST		2U
#define AMDRDNA_NBIO_REG_DOORBELL_APERTURE	0x00c0U
#define AMDRDNA_NBIO_REG_INTERRUPT_CONTROL	0x00f1U
#define AMDRDNA_NBIO_REG_INTERRUPT_DUMMY	0x00f2U
#define AMDRDNA_NBIO_REG_HOST_PATH_FLUSH	0x00f7U
#define AMDRDNA_NBIO_REG_INTERRUPT_DOORBELLS	0x01d2U
#define AMDRDNA_DOORBELL_APERTURE_ON		0x00000001U
#define AMDRDNA_INTERRUPT_DUMMY_SHIFT		8U

/*
 * The host data path's registers (HDP window 0): the word that drops its
 * read cache (write 1, read back), and the control word whose bit 0 makes
 * a flush also drop the read cache.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/hdp/hdp_5_0_0_offset.h
 * (mmHDP_READ_CACHE_INVALIDATE, mmHDP_MISC_CNTL), hdp_5_0_0_sh_mask.h
 * (HDP_MISC_CNTL__FLUSH_INVALIDATE_CACHE) and
 * drivers/gpu/drm/amd/amdgpu/hdp_v5_0.c (hdp_v5_0_invalidate_hdp,
 * hdp_v5_0_init_registers).
 */
#define AMDRDNA_HDP_WINDOW_MAIN			0U
#define AMDRDNA_HDP_REG_READ_DROP		0x00d1U
#define AMDRDNA_HDP_REG_MISC			0x00d3U
#define AMDRDNA_HDP_MISC_FLUSH_DROPS		0x00000001U

/*
 * The interrupt controller's registers (OSSSYS window 0): the table that
 * names the process address space of each graphics-hub context (one word
 * per context), the ring's control word, its base (address bits 39:8) and
 * the base's high byte (bits 47:40), the read and write pointers, the
 * address of the write pointer's copy in memory (high 16 bits, low 32
 * bits) and the read-pointer doorbell word.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/oss/osssys_5_0_0_offset.h
 * (mmIH_VMID_0_LUT, mmIH_RB_CNTL, mmIH_RB_BASE, mmIH_RB_BASE_HI,
 * mmIH_RB_RPTR, mmIH_RB_WPTR, mmIH_RB_WPTR_ADDR_HI, mmIH_RB_WPTR_ADDR_LO,
 * mmIH_DOORBELL_RPTR) and drivers/gpu/drm/amd/amdgpu/navi10_ih.c
 * (navi10_ih_enable_ring: the base shifts and the 16-bit high address).
 */
#define AMDRDNA_IH_WINDOW_MAIN			0U
#define AMDRDNA_IH_REG_SPACE_TO_PROCESS		0x0000U
#define AMDRDNA_IH_REG_RING_CONTROL		0x0080U
#define AMDRDNA_IH_REG_RING_BASE		0x0081U
#define AMDRDNA_IH_REG_RING_BASE_HIGH		0x0082U
#define AMDRDNA_IH_REG_READ_POINTER		0x0083U
#define AMDRDNA_IH_REG_WRITE_POINTER		0x0084U
#define AMDRDNA_IH_REG_POINTER_COPY_HIGH	0x0085U
#define AMDRDNA_IH_REG_POINTER_COPY_LOW		0x0086U
#define AMDRDNA_IH_REG_DOORBELL			0x0087U
#define AMDRDNA_IH_RING_BASE_SHIFT		8U
#define AMDRDNA_IH_RING_BASE_HIGH_SHIFT		40U
#define AMDRDNA_IH_RING_BASE_HIGH_MASK		0x000000ffU
#define AMDRDNA_IH_POINTER_COPY_HIGH_MASK	0x0000ffffU

/*
 * The interrupt ring's size, and the doorbell of its read pointer: slot
 * 0x178 of the 64-bit doorbell slots, which is dword 0x2f0 of the
 * doorbell BAR.  The driver maps the first page of that BAR, which holds
 * every doorbell it uses.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_ih.h (IH_RING_SIZE),
 * drivers/gpu/drm/amd/amdgpu/amdgpu_doorbell.h
 * (AMDGPU_NAVI10_DOORBELL_IH) and drivers/gpu/drm/amd/amdgpu/navi10_ih.c
 * (navi10_ih_sw_init: the slot shifted left by one).
 * derived: 0x178 << 1 = 0x2f0.
 */
#define AMDRDNA_INTERRUPT_RING_BYTES		0x40000U
#define AMDRDNA_DOORBELL_INTERRUPTS		0x02f0U
#define AMDRDNA_DOORBELL_BYTES			0x1000U

/*
 * The memory hubs' settings the driver uses: the invalidation engine the
 * CPU drives (17), the bit an engine's lock word reads as 1 once taken,
 * the reach of the system-memory table of context 0 (512 MiB), the range
 * every invalidation engine covers (all of it), the values that leave the
 * bypass aperture shut (bottom above top) and the identity aperture shut
 * (first above last), the last page of a context's 48-bit space, and the
 * shifts of the aperture words (256 KiB units), the bypass words (16 MiB
 * units) and the page words (4 KiB units, high half from bit 44).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gmc_v10_0.c (gmc_v10_0_flush_gpu_tlb:
 * engine 17 and the semaphore; gmc_v10_0_mc_init: 512 MiB for GC 10.1),
 * drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_1_0_sh_mask.h
 * (GCVM_INVALIDATE_ENG0_SEM__SEMAPHORE),
 * drivers/gpu/drm/amd/amdgpu/gfxhub_v2_0.c
 * (gfxhub_v2_0_program_invalidation, gfxhub_v2_0_disable_identity_aperture,
 * gfxhub_v2_0_init_system_aperture_regs, gfxhub_v2_0_setup_vmid_config,
 * gfxhub_v2_0_init_gart_aperture_regs) and amdgpu_gmc.c
 * (amdgpu_gmc_set_agp_default).
 */
#define AMDRDNA_HUB_CPU_ENGINE			17U
#define AMDRDNA_HUB_LOCK_TAKEN			0x00000001U
#define AMDRDNA_GART_BYTES			0x20000000ULL
#define AMDRDNA_HUB_RANGE_ALL_LOW		0xffffffffU
#define AMDRDNA_HUB_RANGE_ALL_HIGH		0x0000001fU
#define AMDRDNA_HUB_BYPASS_SHUT_BOTTOM		0x00ffffffU
#define AMDRDNA_HUB_IDENTITY_SHUT_LOW		0xffffffffU
#define AMDRDNA_HUB_IDENTITY_SHUT_HIGH		0x0000000fU
#define AMDRDNA_HUB_LAST_PAGE_LOW		0xffffffffU
#define AMDRDNA_HUB_LAST_PAGE_HIGH		0x0000000fU
#define AMDRDNA_HUB_APERTURE_SHIFT		18U
#define AMDRDNA_HUB_BYPASS_SHIFT		24U
#define AMDRDNA_HUB_PAGE_SHIFT			12U
#define AMDRDNA_HUB_PAGE_HIGH_SHIFT		44U


/*
 * The security processor's mailbox words the bring-up uses beyond the
 * ones above (MP0 window 0): the boot loader's argument (an address in
 * 1 MiB units), the secure OS's control word for its ring, the ring's
 * write pointer (in dwords), the ring's address (low, high) and its size
 * in bytes.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/mp/mp_11_0_offset.h
 * (mmMP0_SMN_C2PMSG_36, mmMP0_SMN_C2PMSG_64, mmMP0_SMN_C2PMSG_67,
 * mmMP0_SMN_C2PMSG_69, mmMP0_SMN_C2PMSG_70, mmMP0_SMN_C2PMSG_71).
 */
#define AMDRDNA_PSP_REG_BOOT_ARGUMENT		0x0064U
#define AMDRDNA_PSP_REG_RING_CONTROL		0x0080U
#define AMDRDNA_PSP_REG_RING_WRITE		0x0083U
#define AMDRDNA_PSP_REG_RING_LOW		0x0085U
#define AMDRDNA_PSP_REG_RING_HIGH		0x0086U
#define AMDRDNA_PSP_REG_RING_BYTES		0x0087U

/*
 * The security processor's handshake values: the mask and value of a
 * mailbox word that reads "ready, no error" (bit 31 and a zero status in
 * bits 15:0), the boot loader's commands (load the key database, the
 * system driver, the secure OS, and run the long memory training), and
 * the secure OS's ring commands (create the kernel-mode ring, type 2 in
 * bits 23:16, and destroy every ring).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_psp.h (PSP_BL__LOAD_KEY_DATABASE,
 * PSP_BL__LOAD_SYSDRV, PSP_BL__LOAD_SOSDRV, PSP_BL__DRAM_LONG_TRAIN,
 * MBOX_TOS_READY_FLAG, MBOX_TOS_READY_MASK, enum psp_ring_type) and
 * drivers/gpu/drm/amd/amdgpu/psp_gfx_if.h (GFX_CMD_STATUS_MASK,
 * GFX_CMD_RESPONSE_MASK, GFX_CTRL_CMD_ID_INIT_GPCOM_RING,
 * GFX_CTRL_CMD_ID_DESTROY_RINGS), and psp_v11_0.c
 * (psp_v11_0_wait_for_bootloader: the 0x8000FFFF mask).
 */
#define AMDRDNA_PSP_READY_MASK			0x8000ffffU
#define AMDRDNA_PSP_READY			0x80000000U
#define AMDRDNA_PSP_BOOT_KEY_DATABASE		0x00080000U
#define AMDRDNA_PSP_BOOT_SYSTEM_DRIVER		0x00010000U
#define AMDRDNA_PSP_BOOT_SECURE_OS		0x00020000U
#define AMDRDNA_PSP_BOOT_LONG_TRAINING		0x00100000U
#define AMDRDNA_PSP_RING_CREATE			0x00020000U
#define AMDRDNA_PSP_RING_DESTROY		0x00030000U
#define AMDRDNA_PSP_ADDRESS_SHIFT		20U

/*
 * The system-management unit's mailbox (MP1 window 0): the message word,
 * its argument and its response, and the words that let the unit raise a
 * software interrupt (the interrupt's ID in bits 7:0, valid in bit 8) and
 * mask or acknowledge it (mask in bit 0, acknowledge in bit 8).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/mp/mp_11_0_offset.h
 * (mmMP1_SMN_C2PMSG_66, mmMP1_SMN_C2PMSG_82, mmMP1_SMN_C2PMSG_90,
 * mmMP1_SMN_IH_SW_INT, mmMP1_SMN_IH_SW_INT_CTRL) and mp_11_0_sh_mask.h
 * (MP1_SMN_IH_SW_INT__ID_MASK, MP1_SMN_IH_SW_INT__VALID_MASK,
 * MP1_SMN_IH_SW_INT_CTRL__INT_MASK_MASK,
 * MP1_SMN_IH_SW_INT_CTRL__INT_ACK_MASK).
 */
#define AMDRDNA_SMU_WINDOW_MAILBOX		0U
#define AMDRDNA_SMU_REG_MESSAGE			0x0282U
#define AMDRDNA_SMU_REG_ARGUMENT		0x0292U
#define AMDRDNA_SMU_REG_RESPONSE		0x029aU
#define AMDRDNA_SMU_REG_SOFT_INTERRUPT		0x02c2U
#define AMDRDNA_SMU_REG_SOFT_INTERRUPT_CONTROL	0x02c3U
#define AMDRDNA_SMU_SOFT_INTERRUPT_ID_MASK	0x000000ffU
#define AMDRDNA_SMU_SOFT_INTERRUPT_VALID	0x00000100U
#define AMDRDNA_SMU_SOFT_INTERRUPT_MASKED	0x00000001U
#define AMDRDNA_SMU_SOFT_INTERRUPT_ACKNOWLEDGE	0x00000100U

/*
 * The thermal block's registers (THM window 0): the interrupt enable and
 * control words, the temperature status, the fan controller's three
 * control words, the fan's tachometer period and the thermal status that
 * reports the duty the fan is driven at.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/thm/thm_11_0_2_offset.h
 * (mmTHM_THERMAL_INT_ENA, mmTHM_THERMAL_INT_CTRL,
 * mmCG_MULT_THERMAL_STATUS, mmCG_FDO_CTRL0, mmCG_FDO_CTRL1,
 * mmCG_FDO_CTRL2, mmCG_TACH_STATUS, mmCG_THERMAL_STATUS).
 */
#define AMDRDNA_THM_WINDOW_MAIN			0U
#define AMDRDNA_THM_REG_ALERT_ENABLE		0x000aU
#define AMDRDNA_THM_REG_ALERT_CONTROL		0x000bU
#define AMDRDNA_THM_REG_TEMPERATURE		0x005fU
#define AMDRDNA_THM_REG_FAN_DUTY		0x0067U
#define AMDRDNA_THM_REG_FAN_LIMITS		0x0068U
#define AMDRDNA_THM_REG_FAN_MODE		0x0069U
#define AMDRDNA_THM_REG_FAN_TACHOMETER		0x006bU
#define AMDRDNA_THM_REG_FAN_STATUS		0x006cU

/*
 * The graphics block's second window, where the shader-array select, the
 * scratch word, the run-list controller and the cache-channel fuses live.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/navi10_ip_offset.h (GC_BASE, segment 1).
 */
#define AMDRDNA_GC_WINDOW_SECOND		1U

/*
 * Where the graphics block's second window starts on Navi10 (a dword
 * index).  The commands address registers by absolute index, and the
 * clear state's context registers are counted from it.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/navi10_ip_offset.h (GC_BASE: segment 1 at
 * 0x0000A000).
 */
#define AMDRDNA_GC_SECOND_WINDOW_START		0x0000a000U

/*
 * The graphics block's control and shape registers (GC window 0): the bus
 * control word (its read timeout in bits 7:0), the queue select (pipe
 * 1:0, engine 3:2, context 7:4, queue 10:8), the address configuration,
 * the render back ends fused off and switched off (bits 23:16 each), the
 * shader array's workgroup processors fused off and switched off (bits
 * 31:16 each), the shader memory mode and apertures of the selected
 * context, the global data share's base, size, wave sync and ordered
 * append words of context 0 (the base and size of context n at 2n, the
 * other two at n), and the two words that keep invalidations away from
 * absent workgroup processors.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_1_0_offset.h (mmGRBM_CNTL,
 * mmGRBM_GFX_CNTL, mmGB_ADDR_CONFIG, mmCC_RB_BACKEND_DISABLE,
 * mmGC_USER_RB_BACKEND_DISABLE, mmCC_GC_SHADER_ARRAY_CONFIG,
 * mmGC_USER_SHADER_ARRAY_CONFIG, mmSH_MEM_CONFIG, mmSH_MEM_BASES,
 * mmGDS_VMID0_BASE, mmGDS_VMID0_SIZE, mmGDS_GWS_VMID0, mmGDS_OA_VMID0,
 * mmUTCL1_UTCL0_INVREQ_DISABLE, mmGCRD_SA_TARGETS_DISABLE) and
 * gc_10_1_0_sh_mask.h (GRBM_CNTL__READ_TIMEOUT_MASK, the GRBM_GFX_CNTL
 * fields, CC_RB_BACKEND_DISABLE__BACKEND_DISABLE_MASK,
 * CC_GC_SHADER_ARRAY_CONFIG__INACTIVE_WGPS_MASK).
 */
#define AMDRDNA_GC_REG_BUS_CONTROL		0x0da0U
#define AMDRDNA_GC_REG_QUEUE_SELECT		0x0dc2U
#define AMDRDNA_GC_REG_ADDRESS_CONFIG		0x13deU
#define AMDRDNA_GC_REG_BACKENDS_FUSED		0x13ddU
#define AMDRDNA_GC_REG_BACKENDS_OFF		0x147fU
#define AMDRDNA_GC_REG_GROUPS_FUSED		0x100fU
#define AMDRDNA_GC_REG_GROUPS_OFF		0x1010U
#define AMDRDNA_GC_REG_SHADER_MEMORY_MODE	0x10adU
#define AMDRDNA_GC_REG_SHADER_APERTURES		0x10aaU
#define AMDRDNA_GC_REG_SHARE_BASE		0x20a0U
#define AMDRDNA_GC_REG_SHARE_SIZE		0x20a1U
#define AMDRDNA_GC_REG_WAVE_SYNC		0x20c0U
#define AMDRDNA_GC_REG_ORDERED_APPEND		0x20d0U
#define AMDRDNA_GC_REG_TRANSLATION_SKIP		0x158aU
#define AMDRDNA_GC_REG_CACHE_TARGETS_SKIP	0x158bU
#define AMDRDNA_GC_BUS_TIMEOUT_MASK		0x000000ffU
#define AMDRDNA_GC_SELECT_PIPE_SHIFT		0U
#define AMDRDNA_GC_SELECT_ENGINE_SHIFT		2U
#define AMDRDNA_GC_SELECT_CONTEXT_SHIFT		4U
#define AMDRDNA_GC_SELECT_QUEUE_SHIFT		8U

/*
 * The command processor's registers (GC window 0): its busy word, the
 * halt words of the compute engines (engine 1 in bit 30, engine 2 in bit
 * 28) and of the graphics engine (constant 24, prefetch 26, micro 28),
 * the hardware context count, the device ID word, the interrupt control
 * of the graphics ring and of compute engine 1's pipe 0 (pipes 1 to 3
 * follow), the doorbell ranges of the graphics queues and of the compute
 * queues (a doorbell's dword index from bit 2, the field 27:2), the
 * graphics ring's doorbell enable (bit 30), the write-pointer poll
 * control (enable in bit 31) and the queue status (doorbells on in bit 1).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_1_0_offset.h (mmCP_STAT,
 * mmCP_MEC_CNTL, mmCP_ME_CNTL, mmCP_MAX_CONTEXT, mmCP_DEVICE_ID,
 * mmCP_INT_CNTL_RING0, mmCP_ME1_PIPE0_INT_CNTL, mmCP_RB_DOORBELL_RANGE_LOWER,
 * mmCP_RB_DOORBELL_RANGE_UPPER, mmCP_MEC_DOORBELL_RANGE_LOWER,
 * mmCP_MEC_DOORBELL_RANGE_UPPER, mmCP_PQ_WPTR_POLL_CNTL, mmCP_PQ_STATUS)
 * and gc_10_1_0_sh_mask.h (CP_MEC_CNTL__MEC_ME1_HALT_MASK,
 * CP_MEC_CNTL__MEC_ME2_HALT_MASK, CP_ME_CNTL__CE_HALT_MASK,
 * CP_ME_CNTL__PFP_HALT_MASK, CP_ME_CNTL__ME_HALT_MASK,
 * CP_RB_DOORBELL_RANGE_LOWER__DOORBELL_RANGE_LOWER_MASK,
 * CP_RB_DOORBELL_CONTROL__DOORBELL_EN_MASK, CP_PQ_WPTR_POLL_CNTL__EN_MASK,
 * CP_PQ_STATUS__DOORBELL_ENABLE_MASK).
 */
#define AMDRDNA_GC_REG_COMMAND_BUSY		0x0f40U
#define AMDRDNA_GC_REG_COMPUTE_HALT		0x0e2dU
#define AMDRDNA_GC_REG_GRAPHICS_HALT		0x0f56U
#define AMDRDNA_GC_REG_CONTEXT_LIMIT		0x1e4eU
#define AMDRDNA_GC_REG_COMMAND_DEVICE		0x1debU
#define AMDRDNA_GC_REG_GRAPHICS_INTERRUPTS	0x1e0aU
#define AMDRDNA_GC_REG_COMPUTE_INTERRUPTS	0x1e25U
#define AMDRDNA_GC_REG_GRAPHICS_DOORBELLS_LOW	0x1dfaU
#define AMDRDNA_GC_REG_GRAPHICS_DOORBELLS_HIGH	0x1dfbU
#define AMDRDNA_GC_REG_COMPUTE_DOORBELLS_LOW	0x1dfcU
#define AMDRDNA_GC_REG_COMPUTE_DOORBELLS_HIGH	0x1dfdU
#define AMDRDNA_GC_REG_POINTER_POLL		0x1e23U
#define AMDRDNA_GC_REG_QUEUE_STATUS		0x1e58U

/*
 * The register a queue's work is reset through: a request bit for each
 * context (15:0) and, for the graphics pipe 0, a bit for each of its
 * queues (23:16); it reads 0 once the reset is done.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_1_0_offset.h
 * (mmCP_VMID_RESET 0x1e53) and gc_10_1_0_sh_mask.h
 * (CP_VMID_RESET__RESET_REQUEST_MASK 0xffff,
 * CP_VMID_RESET__PIPE0_QUEUES__SHIFT 0x10).
 */
#define AMDRDNA_GC_REG_CONTEXT_RESET		0x1e53U
#define AMDRDNA_GC_CONTEXT_RESET_QUEUES_SHIFT	16U

/*
 * The shader sequencer's command register (GC window 0) and the command
 * that kills every wave of one context: the kill (3), sent to every wave
 * (1 in bits 6:4), only to those of the context named (bit 7), which is
 * in bits 31:28.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_1_0_offset.h
 * (mmSQ_CMD 0x111b), gc_10_1_0_sh_mask.h (SQ_CMD__CMD__SHIFT 0,
 * SQ_CMD__MODE__SHIFT 4, SQ_CMD__CHECK_VMID__SHIFT 7,
 * SQ_CMD__VM_ID__SHIFT 28) and navi10_enum.h (SQ_IND_CMD_CMD_KILL 3,
 * SQ_IND_CMD_MODE_BROADCAST 1).
 */
#define AMDRDNA_GC_REG_WAVE_COMMAND		0x111bU
#define AMDRDNA_GC_WAVE_KILL			0x3U
#define AMDRDNA_GC_WAVE_EVERY			(1U << 4)
#define AMDRDNA_GC_WAVE_OF_CONTEXT		(1U << 7)
#define AMDRDNA_GC_WAVE_CONTEXT_SHIFT		28U
#define AMDRDNA_GC_COMPUTE_HALT_FIRST		0x40000000U
#define AMDRDNA_GC_COMPUTE_HALT_SECOND		0x10000000U
#define AMDRDNA_GC_GRAPHICS_HALT_CONSTANT	0x01000000U
#define AMDRDNA_GC_GRAPHICS_HALT_PREFETCH	0x04000000U
#define AMDRDNA_GC_GRAPHICS_HALT_MICRO		0x10000000U
#define AMDRDNA_GC_POINTER_POLL_ON		0x80000000U
#define AMDRDNA_GC_DOORBELL_RANGE_MASK		0x0ffffffcU
#define AMDRDNA_GC_DOORBELL_ON			0x40000000U
#define AMDRDNA_GC_QUEUE_DOORBELLS_ON		0x00000002U

/*
 * The interrupt control bits of the graphics ring and of a compute pipe:
 * the four idle and busy events (busy compute 18, busy context 19, empty
 * context 20, idle 21), the privileged instruction (22, graphics only),
 * the privileged register (23), the bad opcode (24) and the end-of-pipe
 * timestamp (26).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_1_0_sh_mask.h
 * (CP_INT_CNTL_RING0__CMP_BUSY_INT_ENABLE_MASK,
 * CP_INT_CNTL_RING0__CNTX_BUSY_INT_ENABLE_MASK,
 * CP_INT_CNTL_RING0__CNTX_EMPTY_INT_ENABLE_MASK,
 * CP_INT_CNTL_RING0__GFX_IDLE_INT_ENABLE_MASK,
 * CP_INT_CNTL_RING0__PRIV_INSTR_INT_ENABLE_MASK,
 * CP_INT_CNTL_RING0__PRIV_REG_INT_ENABLE_MASK,
 * CP_INT_CNTL_RING0__OPCODE_ERROR_INT_ENABLE_MASK,
 * CP_INT_CNTL_RING0__TIME_STAMP_INT_ENABLE_MASK,
 * CP_ME1_PIPE0_INT_CNTL__TIME_STAMP_INT_ENABLE_MASK).
 */
#define AMDRDNA_GC_INTERRUPT_IDLE_EVENTS	0x003c0000U
#define AMDRDNA_GC_INTERRUPT_PRIVILEGED_CODE	0x00400000U
#define AMDRDNA_GC_INTERRUPT_PRIVILEGED_WORD	0x00800000U
#define AMDRDNA_GC_INTERRUPT_BAD_OPCODE		0x01000000U
#define AMDRDNA_GC_INTERRUPT_END_OF_PIPE	0x04000000U

/*
 * The registers of the hardware queue the queue select names (GC window
 * 0): the queue image's address (low, high), active, context, persistent
 * state, the ring's base (low, high, in 256-byte units), the read
 * pointer, the read pointer's report address (low, high), the write
 * pointer's poll address (low, high), the doorbell control, the ring
 * control, the indirect-buffer control, the dequeue request, the image
 * control, the end-of-pipe buffer's base (low, high) and control, and
 * the write pointer (low, high).  The compute queue image holds these
 * words in the same order from its word 128 on.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_1_0_offset.h
 * (mmCP_MQD_BASE_ADDR, mmCP_MQD_BASE_ADDR_HI, mmCP_HQD_ACTIVE,
 * mmCP_HQD_VMID, mmCP_HQD_PERSISTENT_STATE, mmCP_HQD_PQ_BASE,
 * mmCP_HQD_PQ_BASE_HI, mmCP_HQD_PQ_RPTR, mmCP_HQD_PQ_RPTR_REPORT_ADDR,
 * mmCP_HQD_PQ_RPTR_REPORT_ADDR_HI, mmCP_HQD_PQ_WPTR_POLL_ADDR,
 * mmCP_HQD_PQ_WPTR_POLL_ADDR_HI, mmCP_HQD_PQ_DOORBELL_CONTROL,
 * mmCP_HQD_PQ_CONTROL, mmCP_HQD_IB_CONTROL, mmCP_HQD_DEQUEUE_REQUEST,
 * mmCP_MQD_CONTROL, mmCP_HQD_EOP_BASE_ADDR, mmCP_HQD_EOP_BASE_ADDR_HI,
 * mmCP_HQD_EOP_CONTROL, mmCP_HQD_PQ_WPTR_LO, mmCP_HQD_PQ_WPTR_HI).
 */
#define AMDRDNA_GC_REG_QUEUE_IMAGE_LOW		0x1fa9U
#define AMDRDNA_GC_REG_QUEUE_IMAGE_HIGH		0x1faaU
#define AMDRDNA_GC_REG_QUEUE_ACTIVE		0x1fabU
#define AMDRDNA_GC_REG_QUEUE_CONTEXT		0x1facU
#define AMDRDNA_GC_REG_QUEUE_PERSISTENT		0x1fadU
#define AMDRDNA_GC_REG_QUEUE_RING_LOW		0x1fb1U
#define AMDRDNA_GC_REG_QUEUE_RING_HIGH		0x1fb2U
#define AMDRDNA_GC_REG_QUEUE_READ		0x1fb3U
#define AMDRDNA_GC_REG_QUEUE_READ_REPORT_LOW	0x1fb4U
#define AMDRDNA_GC_REG_QUEUE_READ_REPORT_HIGH	0x1fb5U
#define AMDRDNA_GC_REG_QUEUE_WRITE_POLL_LOW	0x1fb6U
#define AMDRDNA_GC_REG_QUEUE_WRITE_POLL_HIGH	0x1fb7U
#define AMDRDNA_GC_REG_QUEUE_DOORBELL		0x1fb8U
#define AMDRDNA_GC_REG_QUEUE_RING_CONTROL	0x1fbaU
#define AMDRDNA_GC_REG_QUEUE_BUFFER_CONTROL	0x1fbeU
#define AMDRDNA_GC_REG_QUEUE_DEQUEUE		0x1fc1U
#define AMDRDNA_GC_REG_QUEUE_IMAGE_CONTROL	0x1fcbU
#define AMDRDNA_GC_REG_QUEUE_END_LOW		0x1fceU
#define AMDRDNA_GC_REG_QUEUE_END_HIGH		0x1fcfU
#define AMDRDNA_GC_REG_QUEUE_END_CONTROL	0x1fd0U
#define AMDRDNA_GC_REG_QUEUE_WRITE_LOW		0x1fdfU
#define AMDRDNA_GC_REG_QUEUE_WRITE_HIGH		0x1fe0U

/*
 * The graphics queue's registers (GC window 0) its image is filled from:
 * the image control, the context, the queue priority, the time quantum,
 * the ring control, the read pointer and the doorbell control.  The last
 * one also serves the graphics ring the driver runs without the
 * firmware's scheduler.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_1_0_offset.h
 * (mmCP_GFX_MQD_CONTROL, mmCP_GFX_HQD_VMID, mmCP_GFX_HQD_QUEUE_PRIORITY,
 * mmCP_GFX_HQD_QUANTUM, mmCP_GFX_HQD_CNTL, mmCP_GFX_HQD_RPTR,
 * mmCP_RB_DOORBELL_CONTROL).
 */
#define AMDRDNA_GC_REG_GRAPHICS_IMAGE_CONTROL	0x1e9aU
#define AMDRDNA_GC_REG_GRAPHICS_CONTEXT		0x1e81U
#define AMDRDNA_GC_REG_GRAPHICS_PRIORITY	0x1e84U
#define AMDRDNA_GC_REG_GRAPHICS_QUANTUM		0x1e85U
#define AMDRDNA_GC_REG_GRAPHICS_RING_CONTROL	0x1e8fU
#define AMDRDNA_GC_REG_GRAPHICS_READ		0x1e88U
#define AMDRDNA_GC_REG_GRAPHICS_DOORBELL	0x1e8dU

/*
 * The graphics ring the driver runs itself when the firmware's scheduler
 * is not used (GC window 0): the write-pointer delay, the ring's context,
 * the ring control, the write pointer (low, high), the read pointer's
 * report address (low, high), the write pointer's poll address (low,
 * high), the ring's base (low, high) and the active word.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_1_0_offset.h
 * (mmCP_RB_WPTR_DELAY, mmCP_RB_VMID, mmCP_RB0_CNTL, mmCP_RB0_WPTR,
 * mmCP_RB0_WPTR_HI, mmCP_RB0_RPTR_ADDR, mmCP_RB0_RPTR_ADDR_HI,
 * mmCP_RB_WPTR_POLL_ADDR_LO, mmCP_RB_WPTR_POLL_ADDR_HI, mmCP_RB0_BASE,
 * mmCP_RB0_BASE_HI, mmCP_RB_ACTIVE).
 */
#define AMDRDNA_GC_REG_RING_WRITE_DELAY		0x0f61U
#define AMDRDNA_GC_REG_RING_CONTEXT		0x1df1U
#define AMDRDNA_GC_REG_RING_CONTROL		0x1de1U
#define AMDRDNA_GC_REG_RING_WRITE_LOW		0x1df4U
#define AMDRDNA_GC_REG_RING_WRITE_HIGH		0x1df5U
#define AMDRDNA_GC_REG_RING_READ_REPORT_LOW	0x1de3U
#define AMDRDNA_GC_REG_RING_READ_REPORT_HIGH	0x1de4U
#define AMDRDNA_GC_REG_RING_WRITE_POLL_LOW	0x1e8bU
#define AMDRDNA_GC_REG_RING_WRITE_POLL_HIGH	0x1e8cU
#define AMDRDNA_GC_REG_RING_BASE_LOW		0x1de0U
#define AMDRDNA_GC_REG_RING_BASE_HIGH		0x1e51U
#define AMDRDNA_GC_REG_RING_ACTIVE		0x1f40U

/*
 * The registers of the second window: the shader-array select (instance
 * 7:0, array 15:8, engine 23:16, and the broadcast bits 29, 30 and 31),
 * the scratch word the ring tests write (also the user-configuration
 * register 0x40), the level-2 cache channels fused off and switched off
 * (channels 0 to 7 in bits 15:8, 8 and up in bits 31:16), the run-list
 * controller's clear-state buffer address (low, high) and length, its
 * save/restore machine control (enable 0, auto increment 1), the word
 * that names the scheduler's own queue, and the boot-load status (done in
 * bit 31).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_1_0_offset.h
 * (mmGRBM_GFX_INDEX, mmSCRATCH_REG0, mmCGTS_TCC_DISABLE,
 * mmCGTS_USER_TCC_DISABLE, mmRLC_CSIB_ADDR_LO, mmRLC_CSIB_ADDR_HI,
 * mmRLC_CSIB_LENGTH, mmRLC_SRM_CNTL, mmRLC_CP_SCHEDULERS,
 * mmRLC_RLCS_BOOTLOAD_STATUS) and gc_10_1_0_sh_mask.h (the GRBM_GFX_INDEX
 * fields, CGTS_TCC_DISABLE__HI_TCC_DISABLE_MASK,
 * CGTS_TCC_DISABLE__TCC_DISABLE_MASK, RLC_SRM_CNTL__SRM_ENABLE_MASK,
 * RLC_SRM_CNTL__AUTO_INCR_ADDR_MASK,
 * RLC_RLCS_BOOTLOAD_STATUS__BOOTLOAD_COMPLETE_MASK).
 */
#define AMDRDNA_GC_REG_ARRAY_SELECT		0x2200U
#define AMDRDNA_GC_REG_SCRATCH			0x2040U
#define AMDRDNA_GC_REG_CHANNELS_FUSED		0x500aU
#define AMDRDNA_GC_REG_CHANNELS_OFF		0x500bU
#define AMDRDNA_GC_REG_CLEAR_STATE_LOW		0x4ca2U
#define AMDRDNA_GC_REG_CLEAR_STATE_HIGH		0x4ca3U
#define AMDRDNA_GC_REG_CLEAR_STATE_LENGTH	0x4ca4U
#define AMDRDNA_GC_REG_SAVE_RESTORE		0x4c80U
#define AMDRDNA_GC_REG_SCHEDULER_QUEUE_SELECT	0x4caaU
#define AMDRDNA_GC_REG_RUN_LIST_BOOT		0x4e8dU
#define AMDRDNA_GC_SELECT_INSTANCE_SHIFT	0U
#define AMDRDNA_GC_SELECT_ARRAY_SHIFT		8U
#define AMDRDNA_GC_SELECT_ENGINE_INDEX_SHIFT	16U
#define AMDRDNA_GC_SELECT_ALL_ARRAYS		0x20000000U
#define AMDRDNA_GC_SELECT_ALL_INSTANCES		0x40000000U
#define AMDRDNA_GC_SELECT_ALL_ENGINES		0x80000000U
#define AMDRDNA_GC_SCRATCH_USER_OFFSET		0x0040U
#define AMDRDNA_GC_SAVE_RESTORE_ON		0x00000003U
#define AMDRDNA_GC_RUN_LIST_BOOT_DONE		0x80000000U

/*
 * The first copy engine's registers, which live in the graphics block's
 * first window: its control (trap 0, address translation 1, mid-command
 * preemption 5, automatic context switch 18), its chicken bits, the
 * semaphore wait-fail timer, the engine's halt word (bit 0), the address
 * translation's control (redo delay 5:1, response mode 11:9) and page
 * policy (read policy 13:12, write policy 15:14), and its graphics
 * queue's ring control (enable 0, size in dwords as log2 in 5:1, read
 * pointer write-back 12), ring base (low, high), read pointer (low,
 * high), write pointer (low, high), write-pointer poll control (poll
 * enable 2), read pointer report address (high, low), indirect-buffer
 * control (enable 0), doorbell (enable 28), doorbell offset (27:2),
 * write-pointer poll address (high, low) and minor pointer update.  The
 * second engine's registers are 0x600 further on.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_1_0_offset.h
 * (mmSDMA0_CNTL, mmSDMA0_CHICKEN_BITS, mmSDMA0_SEM_WAIT_FAIL_TIMER_CNTL,
 * mmSDMA0_F32_CNTL, mmSDMA0_UTCL1_CNTL, mmSDMA0_UTCL1_PAGE,
 * mmSDMA0_GFX_RB_CNTL, mmSDMA0_GFX_RB_BASE, mmSDMA0_GFX_RB_BASE_HI,
 * mmSDMA0_GFX_RB_RPTR, mmSDMA0_GFX_RB_RPTR_HI, mmSDMA0_GFX_RB_WPTR,
 * mmSDMA0_GFX_RB_WPTR_HI, mmSDMA0_GFX_RB_WPTR_POLL_CNTL,
 * mmSDMA0_GFX_RB_RPTR_ADDR_HI, mmSDMA0_GFX_RB_RPTR_ADDR_LO,
 * mmSDMA0_GFX_IB_CNTL, mmSDMA0_GFX_DOORBELL, mmSDMA0_GFX_DOORBELL_OFFSET,
 * mmSDMA0_GFX_RB_WPTR_POLL_ADDR_HI, mmSDMA0_GFX_RB_WPTR_POLL_ADDR_LO,
 * mmSDMA0_GFX_MINOR_PTR_UPDATE), gc_10_1_0_sh_mask.h (the SDMA0_CNTL,
 * SDMA0_F32_CNTL, SDMA0_UTCL1_CNTL, SDMA0_GFX_RB_CNTL,
 * SDMA0_GFX_RB_WPTR_POLL_CNTL, SDMA0_GFX_IB_CNTL, SDMA0_GFX_DOORBELL and
 * SDMA0_GFX_DOORBELL_OFFSET fields) and
 * drivers/gpu/drm/amd/amdgpu/sdma_v5_0.c (SDMA1_REG_OFFSET).
 */
#define AMDRDNA_COPY_REG_CONTROL		0x001cU
#define AMDRDNA_COPY_REG_CHICKEN		0x001dU
#define AMDRDNA_COPY_REG_WAIT_TIMER		0x0021U
#define AMDRDNA_COPY_REG_HALT			0x002aU
#define AMDRDNA_COPY_REG_TRANSLATION		0x003cU
#define AMDRDNA_COPY_REG_PAGE_POLICY		0x0048U
#define AMDRDNA_COPY_REG_RING_CONTROL		0x0080U
#define AMDRDNA_COPY_REG_RING_BASE_LOW		0x0081U
#define AMDRDNA_COPY_REG_RING_BASE_HIGH		0x0082U
#define AMDRDNA_COPY_REG_READ_LOW		0x0083U
#define AMDRDNA_COPY_REG_READ_HIGH		0x0084U
#define AMDRDNA_COPY_REG_WRITE_LOW		0x0085U
#define AMDRDNA_COPY_REG_WRITE_HIGH		0x0086U
#define AMDRDNA_COPY_REG_WRITE_POLL		0x0087U
#define AMDRDNA_COPY_REG_READ_REPORT_HIGH	0x0088U
#define AMDRDNA_COPY_REG_READ_REPORT_LOW	0x0089U
#define AMDRDNA_COPY_REG_BUFFER_CONTROL		0x008aU
#define AMDRDNA_COPY_REG_DOORBELL		0x0092U
#define AMDRDNA_COPY_REG_DOORBELL_OFFSET	0x00abU
#define AMDRDNA_COPY_REG_WRITE_POLL_HIGH	0x00b2U
#define AMDRDNA_COPY_REG_WRITE_POLL_LOW		0x00b3U
#define AMDRDNA_COPY_REG_MINOR_UPDATE		0x00b5U
#define AMDRDNA_COPY_SECOND_ENGINE		0x0600U
#define AMDRDNA_COPY_CONTROL_TRAP		0x00000001U
#define AMDRDNA_COPY_CONTROL_TRANSLATE		0x00000002U
#define AMDRDNA_COPY_CONTROL_PREEMPT		0x00000020U
#define AMDRDNA_COPY_CONTROL_SWITCH		0x00040000U
#define AMDRDNA_COPY_HALTED			0x00000001U
#define AMDRDNA_COPY_TRANSLATION_DELAY_MASK	0x0000003eU
#define AMDRDNA_COPY_TRANSLATION_DELAY_SHIFT	1U
#define AMDRDNA_COPY_TRANSLATION_MODE_MASK	0x00000e00U
#define AMDRDNA_COPY_TRANSLATION_MODE_SHIFT	9U
#define AMDRDNA_COPY_RING_ON			0x00000001U
#define AMDRDNA_COPY_RING_SIZE_MASK		0x0000003eU
#define AMDRDNA_COPY_RING_SIZE_SHIFT		1U
#define AMDRDNA_COPY_RING_REPORT		0x00001000U
#define AMDRDNA_COPY_WRITE_POLL_ON		0x00000004U
#define AMDRDNA_COPY_BUFFERS_ON			0x00000001U
#define AMDRDNA_COPY_DOORBELL_ON		0x10000000U
#define AMDRDNA_COPY_DOORBELL_OFFSET_MASK	0x0ffffffcU
#define AMDRDNA_COPY_DOORBELL_OFFSET_SHIFT	2U

/*
 * The copy engines' page policy: the bits the driver keeps (all but the
 * read and write policies), and the policies it sets (read: no allocate,
 * 2 at bit 12; write: bypass, 3 at bit 14).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/sdma_v5_0.c (sdma_v5_0_gfx_resume_instance:
 * the 0xFF0FFF mask) and sdma_common.h (CACHE_READ_POLICY_L2__NOA,
 * CACHE_WRITE_POLICY_L2__BYPASS).
 * derived: (2 << 12) | (3 << 14) = 0xe000.
 */
#define AMDRDNA_COPY_PAGE_POLICY_KEEP		0x00ff0fffU
#define AMDRDNA_COPY_PAGE_POLICY_SET		0x0000e000U

/*
 * The host bridge's doorbell range of the first copy engine (NBIO window
 * 2): the first doorbell in bits 11:2 and the count in bits 20:16.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/nbio/nbio_2_3_offset.h
 * (mmBIF_SDMA0_DOORBELL_RANGE) and nbio_2_3_sh_mask.h
 * (BIF_SDMA0_DOORBELL_RANGE__OFFSET_MASK, BIF_SDMA0_DOORBELL_RANGE__SIZE_MASK).
 */
#define AMDRDNA_NBIO_REG_COPY_DOORBELLS		0x01d0U
#define AMDRDNA_NBIO_COPY_DOORBELLS_OFFSET_MASK	0x00000ffcU
#define AMDRDNA_NBIO_COPY_DOORBELLS_SIZE_MASK	0x001f0000U
#define AMDRDNA_NBIO_COPY_DOORBELLS_SIZE_SHIFT	16U

/*
 * The doorbells of the engines, as dword indices into the doorbell page
 * (the 64-bit slot number shifted left by one): the scheduler's own queue
 * (slot 0), the first compute queue (slot 3, the others follow), the
 * graphics queue (slot 0x8b) and the first copy engine (slot 0x100); the
 * last slot of the compute queues' range (0x8a); and the copy engine's
 * doorbell count in the host bridge's range (20).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_doorbell.h
 * (AMDGPU_NAVI10_DOORBELL_KIQ, AMDGPU_NAVI10_DOORBELL_MEC_RING0,
 * AMDGPU_NAVI10_DOORBELL_USERQUEUE_END, AMDGPU_NAVI10_DOORBELL_GFX_RING0,
 * AMDGPU_NAVI10_DOORBELL_sDMA_ENGINE0) and sdma_v5_0.c
 * (sdma_v5_0_gfx_resume_instance: the range of 20).
 * derived: 0x003 << 1 = 0x006, 0x8b << 1 = 0x116, 0x100 << 1 = 0x200,
 * 0x8a << 1 = 0x114.
 */
#define AMDRDNA_DOORBELL_SCHEDULER		0x0000U
#define AMDRDNA_DOORBELL_COMPUTE_FIRST		0x0006U
#define AMDRDNA_DOORBELL_COMPUTE_LAST		0x0114U
#define AMDRDNA_DOORBELL_GRAPHICS		0x0116U
#define AMDRDNA_DOORBELL_COPY			0x0200U
#define AMDRDNA_DOORBELL_COPY_COUNT		20U

/*
 * The compute dispatch's shader registers (GC window 0): the start of the
 * dispatch (x, y, z) followed by the workgroup size (x, y, z), the
 * program's address (low, in 256-byte units, then high), its two control
 * words (the counts, then the inputs), the resource limits followed by
 * the compute units of shader engines 0 and 1, the scratch ring size and
 * the compute units of shader engines 2 and 3, the third control word,
 * and the first user data word.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_1_0_offset.h
 * (mmCOMPUTE_START_X, mmCOMPUTE_NUM_THREAD_X, mmCOMPUTE_PGM_LO,
 * mmCOMPUTE_PGM_HI, mmCOMPUTE_PGM_RSRC1, mmCOMPUTE_PGM_RSRC2,
 * mmCOMPUTE_RESOURCE_LIMITS, mmCOMPUTE_STATIC_THREAD_MGMT_SE0,
 * mmCOMPUTE_STATIC_THREAD_MGMT_SE1, mmCOMPUTE_TMPRING_SIZE,
 * mmCOMPUTE_STATIC_THREAD_MGMT_SE2, mmCOMPUTE_STATIC_THREAD_MGMT_SE3,
 * mmCOMPUTE_PGM_RSRC3, mmCOMPUTE_USER_DATA_0).
 */
#define AMDRDNA_GC_REG_DISPATCH_START		0x1ba4U
#define AMDRDNA_GC_REG_DISPATCH_PROGRAM		0x1bacU
#define AMDRDNA_GC_REG_DISPATCH_COUNTS		0x1bb2U
#define AMDRDNA_GC_REG_DISPATCH_LIMITS		0x1bb5U
#define AMDRDNA_GC_REG_DISPATCH_MORE		0x1bc8U
#define AMDRDNA_GC_REG_DISPATCH_USER		0x1be0U

/*
 * The video engine's registers (VCN window 1): the tiles' power switches
 * and their state, the power status word, the ninth scratch register, the
 * copy of the graphics block's address layout, the memory interface's
 * second control word, the clock gates and their control, the interrupt
 * enable, the memory interface's control, status and byte swap, the
 * picture cache's control and its three multiplexers, the offsets and
 * sizes of the engine processor's three cache windows and of its
 * uncached window, the processor's control, the soft resets, the decode
 * ring's address context, read pointer, write pointer and control, the
 * engine's status, the fine clock gates and their control, the cache
 * windows' addresses (window 1, window 2), the low-latency encode ring's
 * base, size, read and write pointers, the general encode ring's, the
 * first cache window's address, the uncached window's address, and the
 * decode ring's base.
 *
 * Values referenced: Linux 7.1
 * drivers/gpu/drm/amd/include/asic_reg/vcn/vcn_2_0_0_offset.h
 * (mmUVD_PGFSM_CONFIG, mmUVD_PGFSM_STATUS, mmUVD_POWER_STATUS,
 * mmUVD_SCRATCH9, mmUVD_GFX10_ADDR_CONFIG, mmUVD_LMI_CTRL2,
 * mmUVD_CGC_GATE, mmUVD_CGC_CTRL, mmUVD_MASTINT_EN, mmUVD_LMI_CTRL,
 * mmUVD_LMI_STATUS, mmUVD_LMI_SWAP_CNTL, mmUVD_MPC_CNTL,
 * mmUVD_MPC_SET_MUXA0, mmUVD_MPC_SET_MUXB0, mmUVD_MPC_SET_MUX,
 * mmUVD_VCPU_CACHE_OFFSET0 to mmUVD_VCPU_CACHE_SIZE2,
 * mmUVD_VCPU_NONCACHE_OFFSET0, mmUVD_VCPU_NONCACHE_SIZE0,
 * mmUVD_VCPU_CNTL, mmUVD_SOFT_RESET, mmUVD_LMI_RBC_RB_VMID,
 * mmUVD_RBC_RB_RPTR, mmUVD_RBC_RB_WPTR, mmUVD_RBC_RB_CNTL, mmUVD_STATUS,
 * mmUVD_SUVD_CGC_GATE, mmUVD_SUVD_CGC_CTRL,
 * mmUVD_LMI_VCPU_CACHE1_64BIT_BAR_LOW/HIGH,
 * mmUVD_LMI_VCPU_CACHE2_64BIT_BAR_LOW/HIGH, mmUVD_RB_BASE_LO2 to
 * mmUVD_RB_WPTR2, mmUVD_RB_BASE_LO to mmUVD_RB_WPTR,
 * mmUVD_LMI_VCPU_CACHE_64BIT_BAR_HIGH/LOW,
 * mmUVD_LMI_VCPU_NC0_64BIT_BAR_HIGH/LOW,
 * mmUVD_LMI_RBC_RB_64BIT_BAR_HIGH/LOW; every one with BASE_IDX 1).
 */
#define AMDRDNA_VCN_WINDOW_MAIN			1U
#define AMDRDNA_VCN_REG_TILE_POWER		0x0000U
#define AMDRDNA_VCN_REG_TILE_STATE		0x0001U
#define AMDRDNA_VCN_REG_POWER_STATUS		0x0004U
#define AMDRDNA_VCN_REG_SCRATCH9		0x001dU
#define AMDRDNA_VCN_REG_ADDRESS_CONFIG		0x004aU
#define AMDRDNA_VCN_REG_MEMORY_CONTROL2		0x01fdU
#define AMDRDNA_VCN_REG_GATE			0x01eaU
#define AMDRDNA_VCN_REG_GATE_CONTROL		0x01ecU
#define AMDRDNA_VCN_REG_INTERRUPT_ENABLE	0x0200U
#define AMDRDNA_VCN_REG_MEMORY_CONTROL		0x0226U
#define AMDRDNA_VCN_REG_MEMORY_STATUS		0x0227U
#define AMDRDNA_VCN_REG_BYTE_SWAP		0x022dU
#define AMDRDNA_VCN_REG_PICTURE_CONTROL		0x0237U
#define AMDRDNA_VCN_REG_PICTURE_MUX_A		0x0239U
#define AMDRDNA_VCN_REG_PICTURE_MUX_B		0x023bU
#define AMDRDNA_VCN_REG_PICTURE_MUX		0x023dU
#define AMDRDNA_VCN_REG_CACHE0_OFFSET		0x0242U
#define AMDRDNA_VCN_REG_CACHE0_SIZE		0x0243U
#define AMDRDNA_VCN_REG_CACHE1_OFFSET		0x0244U
#define AMDRDNA_VCN_REG_CACHE1_SIZE		0x0245U
#define AMDRDNA_VCN_REG_CACHE2_OFFSET		0x0246U
#define AMDRDNA_VCN_REG_CACHE2_SIZE		0x0247U
#define AMDRDNA_VCN_REG_UNCACHED_OFFSET		0x0254U
#define AMDRDNA_VCN_REG_UNCACHED_SIZE		0x0255U
#define AMDRDNA_VCN_REG_PROCESSOR_CONTROL	0x0258U
#define AMDRDNA_VCN_REG_SOFT_RESET		0x0260U
#define AMDRDNA_VCN_REG_DECODE_CONTEXT		0x0263U
#define AMDRDNA_VCN_REG_DECODE_READ		0x0264U
#define AMDRDNA_VCN_REG_DECODE_WRITE		0x0265U
#define AMDRDNA_VCN_REG_DECODE_CONTROL		0x0269U
#define AMDRDNA_VCN_REG_STATUS			0x026fU
#define AMDRDNA_VCN_REG_FINE_GATE		0x05a4U
#define AMDRDNA_VCN_REG_FINE_GATE_CONTROL	0x05a6U
#define AMDRDNA_VCN_REG_CACHE1_LOW		0x05acU
#define AMDRDNA_VCN_REG_CACHE1_HIGH		0x05adU
#define AMDRDNA_VCN_REG_CACHE2_LOW		0x05b0U
#define AMDRDNA_VCN_REG_CACHE2_HIGH		0x05b1U
#define AMDRDNA_VCN_REG_ENCODE_LOW_BASE_LOW	0x05e1U
#define AMDRDNA_VCN_REG_ENCODE_LOW_BASE_HIGH	0x05e2U
#define AMDRDNA_VCN_REG_ENCODE_LOW_SIZE		0x05e3U
#define AMDRDNA_VCN_REG_ENCODE_LOW_READ		0x05e4U
#define AMDRDNA_VCN_REG_ENCODE_LOW_WRITE	0x05e5U
#define AMDRDNA_VCN_REG_ENCODE_BASE_LOW		0x05e6U
#define AMDRDNA_VCN_REG_ENCODE_BASE_HIGH	0x05e7U
#define AMDRDNA_VCN_REG_ENCODE_SIZE		0x05e8U
#define AMDRDNA_VCN_REG_ENCODE_READ		0x05e9U
#define AMDRDNA_VCN_REG_ENCODE_WRITE		0x05eaU
#define AMDRDNA_VCN_REG_CACHE0_HIGH		0x061eU
#define AMDRDNA_VCN_REG_CACHE0_LOW		0x061fU
#define AMDRDNA_VCN_REG_UNCACHED_HIGH		0x0622U
#define AMDRDNA_VCN_REG_UNCACHED_LOW		0x0623U
#define AMDRDNA_VCN_REG_DECODE_BASE_HIGH	0x0628U
#define AMDRDNA_VCN_REG_DECODE_BASE_LOW		0x0629U

/*
 * The registers a failed start reports besides those above: the clock
 * gates' state, the fine gates' state, the context the processor's cache
 * windows read through, and the memory interface's address-space control.
 *
 * Values referenced: Linux 7.1
 * drivers/gpu/drm/amd/include/asic_reg/vcn/vcn_2_0_0_offset.h
 * (mmUVD_CGC_STATUS, mmUVD_SUVD_CGC_STATUS, mmUVD_LMI_VCPU_CACHE_VMID,
 * mmUVD_LMI_VM_CTRL; every one with BASE_IDX 1).
 */
#define AMDRDNA_VCN_REG_GATE_STATUS		0x01ebU
#define AMDRDNA_VCN_REG_FINE_GATE_STATUS	0x05a5U
#define AMDRDNA_VCN_REG_CACHE_CONTEXT		0x01fcU
#define AMDRDNA_VCN_REG_MEMORY_CONTEXT		0x0228U

/*
 * The registers of the dynamic power gating (the start the chip's
 * security processor allows when it loaded the firmware: it guards the
 * processor's reset and the first two cache windows from the host): the
 * pause request and its acknowledgement, the scratch word that carries
 * the decode ring's write pointer to the gating, the decode ring's
 * write-pointer delay and its read-pointer write-back address, the
 * register check of the indirect buffers and the register mask.
 *
 * Values referenced: Linux 7.1
 * drivers/gpu/drm/amd/include/asic_reg/vcn/vcn_2_0_0_offset.h
 * (mmUVD_DPG_PAUSE, mmUVD_SCRATCH2, mmUVD_RBC_RB_WPTR_CNTL,
 * mmUVD_RBC_RB_RPTR_ADDR; every one with BASE_IDX 1) and
 * drivers/gpu/drm/amd/amdgpu/amdgpu_vcn.h (mmUVD_RBC_XX_IB_REG_CHECK,
 * mmUVD_REG_XX_MASK, BASE_IDX 1).
 */
#define AMDRDNA_VCN_REG_PAUSE			0x0014U
#define AMDRDNA_VCN_REG_SCRATCH2		0x0016U
#define AMDRDNA_VCN_REG_DECODE_WRITE_DELAY	0x0266U
#define AMDRDNA_VCN_REG_DECODE_READ_ADDRESS	0x026aU
#define AMDRDNA_VCN_REG_IB_CHECK		0x026bU
#define AMDRDNA_VCN_REG_REGISTER_MASK		0x026cU

/*
 * The dynamic power gating's bits: the power status word's gating mode
 * (bit 2), its gating enable (bit 8) and its stall of the gating's power
 * up (bit 31); the power state of the engine's main tiles powered (2);
 * the pause request (bit 2) and its acknowledgement (bit 3); the mark of
 * a new decode write pointer in the scratch word (bit 31) and the
 * pointer under it; and the values the gating's table writes: the gate
 * control (dynamic mode, gate delay 1, clock-off delay 4), the fine gates
 * (1) and their control (0), the processor's control (probe timeout 0xff,
 * the write threshold bypass, the clock), the memory interface's settings
 * (write-clean timer 8 and its enable, request mode, CRC reset, urgent
 * requests masked, data coherency, the processor's data coherency, bit
 * 20), its second control (31 write requests of the offload), the
 * register mask (0x10) and the indirect buffers' register check (3).
 *
 * Values referenced: Linux 7.1
 * drivers/gpu/drm/amd/include/asic_reg/vcn/vcn_2_0_0_sh_mask.h
 * (UVD_POWER_STATUS__UVD_PG_MODE_MASK, __UVD_PG_EN_MASK,
 * __STALL_DPG_POWER_UP_MASK, UVD_DPG_PAUSE__NJ_PAUSE_DPG_REQ_MASK,
 * __NJ_PAUSE_DPG_ACK_MASK, UVD_VCPU_CNTL__PRB_TIMEOUT_VAL__SHIFT,
 * __MIF_WR_LOW_THRESHOLD_BP_MASK, __CLK_EN_MASK,
 * UVD_LMI_CTRL__WRITE_CLEAN_TIMER__SHIFT, __REQ_MODE_MASK,
 * __CRC_RESET_MASK, UVD_LMI_CTRL2__RE_OFLD_MIF_WR_REQ_NUM__SHIFT),
 * drivers/gpu/drm/amd/amdgpu/amdgpu_vcn.h
 * (UVD_PGFSM_CONFIG__UVDM_UVDU_PWR_ON) and vcn_v2_0.c
 * (vcn_v2_0_start_dpg_mode, vcn_v2_0_clock_gating_dpg_mode,
 * vcn_v2_0_pause_dpg_mode, vcn_v2_0_dec_ring_set_wptr: the values).
 * derived: 0x1 | 1 << 2 | 4 << 6 = 0x105; 0xff << 20 | 0x20000 | 0x200 =
 * 0x0ff20200; 0x100 | 0x200 | 0x4000 | 0x1000 | 0x2000 | 0x200000 | 8 |
 * 0x100000 = 0x00307308; 0x1f << 17 = 0x003e0000.
 */
#define AMDRDNA_VCN_POWER_GATING_MODE		0x00000004U
#define AMDRDNA_VCN_POWER_GATING_ON		0x00000100U
#define AMDRDNA_VCN_POWER_STALL			0x80000000U
#define AMDRDNA_VCN_POWER_STATE_ON		0x00000002U
#define AMDRDNA_VCN_PAUSE_REQUEST		0x00000004U
#define AMDRDNA_VCN_PAUSE_ACKNOWLEDGED		0x00000008U
#define AMDRDNA_VCN_SCRATCH2_NEW_WRITE		0x80000000U
#define AMDRDNA_VCN_SCRATCH2_WRITE_MASK		0x7fffffffU
#define AMDRDNA_VCN_DPG_GATE_CONTROL		0x00000105U
#define AMDRDNA_VCN_DPG_FINE_GATE		0x00000001U
#define AMDRDNA_VCN_DPG_PROCESSOR		0x0ff20200U
#define AMDRDNA_VCN_DPG_MEMORY			0x00307308U
#define AMDRDNA_VCN_DPG_MEMORY2			0x003e0000U
#define AMDRDNA_VCN_DPG_REGISTER_MASK		0x00000010U
#define AMDRDNA_VCN_DPG_IB_CHECK		0x00000003U

/*
 * The tiles' power: every tile's two-bit switch on (1) or off (2), the
 * state word reading all on (0), and the mask of the ten tiles' fields;
 * the power status word's state field (off: 1) and its two fields the
 * start clears (the state and the hardware's own power gating).
 *
 * Values referenced: Linux 7.1
 * drivers/gpu/drm/amd/include/asic_reg/vcn/vcn_2_0_0_sh_mask.h
 * (UVD_PGFSM_CONFIG__UVDM_PWR_CONFIG__SHIFT to
 * UVD_PGFSM_CONFIG__UVDE_PWR_CONFIG__SHIFT,
 * UVD_POWER_STATUS__UVD_POWER_STATUS_MASK,
 * UVD_POWER_STATUS__UVD_PG_EN_MASK), drivers/gpu/drm/amd/amdgpu/amdgpu_vcn.h
 * (UVD_POWER_STATUS__UVD_POWER_STATUS_TILES_OFF) and vcn_v2_0.c
 * (vcn_v2_0_disable_static_power_gating: every tile on and the state
 * waited for under 0xFFFFF, the status word's 0x103 cleared;
 * vcn_v2_0_enable_static_power_gating: every tile off).
 * derived: the ten fields at bits 0, 2, ..., 18 give 0x55555 for on and
 * 0xaaaaa for off.
 */
#define AMDRDNA_VCN_TILES_ON			0x00055555U
#define AMDRDNA_VCN_TILES_OFF			0x000aaaaaU
#define AMDRDNA_VCN_TILES_STATE_ON		0x00000000U
#define AMDRDNA_VCN_TILES_MASK			0x000fffffU
#define AMDRDNA_VCN_POWER_STATE_MASK		0x00000003U
#define AMDRDNA_VCN_POWER_STATE_OFF		0x00000001U
#define AMDRDNA_VCN_POWER_START_CLEAR		0x00000103U

/*
 * The engine's status word: busy (bit 2, which the driver raises while
 * it starts the engine), the processor's report that it runs (bit 1),
 * and the idle state (0x2 under the low three bits).
 *
 * Values referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/amdgpu_vcn.h
 * (UVD_STATUS__UVD_BUSY, UVD_STATUS__IDLE) and vcn_v2_0.c
 * (vcn_v2_0_start: status & 2; vcn_v2_0_stop: UVD_STATUS__IDLE under 0x7).
 */
#define AMDRDNA_VCN_STATUS_BUSY			0x00000004U
#define AMDRDNA_VCN_STATUS_RUNNING		0x00000002U
#define AMDRDNA_VCN_STATUS_IDLE			0x00000002U
#define AMDRDNA_VCN_STATUS_IDLE_MASK		0x00000007U

/*
 * The clock gates: the gate control's dynamic mode (bit 0, the chip
 * supports medium-grain gating), its gate delay (1 at bit 2) and its
 * clock-off delay (4 at bit 6), the per-unit modes of the gate control
 * (bits 30:11), the per-unit gates (bits 19:0); the fine gates the start
 * sets and the fine gates' modes it clears.
 *
 * Values referenced: Linux 7.1
 * drivers/gpu/drm/amd/include/asic_reg/vcn/vcn_2_0_0_sh_mask.h
 * (UVD_CGC_CTRL__DYN_CLOCK_MODE_MASK, __CLK_GATE_DLY_TIMER__SHIFT,
 * __CLK_OFF_DELAY__SHIFT, __UDEC_RE_MODE_MASK to __SCPU_MODE_MASK,
 * UVD_CGC_GATE__SYS_MASK to __SCPU_MASK, UVD_SUVD_CGC_GATE__SRE_MASK to
 * __IME_HEVC_MASK, UVD_SUVD_CGC_CTRL__SRE_MODE_MASK to __SITE_MODE_MASK),
 * drivers/gpu/drm/amd/amdgpu/nv.c (nv_common_early_init: Navi10's
 * AMD_CG_SUPPORT_VCN_MGCG) and vcn_v2_0.c (vcn_v2_0_disable_clock_gating,
 * vcn_v2_0_enable_clock_gating: the masks they set and clear).
 * derived: the fine gates are bits 15:0 and 24:17 (not the IME gate at
 * bit 16).
 */
#define AMDRDNA_VCN_GATE_DYNAMIC		0x00000001U
#define AMDRDNA_VCN_GATE_DELAYS			0x00000104U
#define AMDRDNA_VCN_GATE_MODES			0x7ffff800U
#define AMDRDNA_VCN_GATE_UNITS			0x000fffffU
#define AMDRDNA_VCN_FINE_GATES			0x01feffffU
#define AMDRDNA_VCN_FINE_GATE_MODES		0x000003ffU

/*
 * The processor's clock enable, the processor's interrupt enable, the
 * memory interface's settings at the start (the write-clean timer, the
 * urgent requests masked, data coherency for the engine and the
 * processor), the picture cache's replacement mode (field bits 5:3, 2)
 * and its multiplexers' defaults.
 *
 * Values referenced: Linux 7.1
 * drivers/gpu/drm/amd/include/asic_reg/vcn/vcn_2_0_0_sh_mask.h
 * (UVD_VCPU_CNTL__CLK_EN_MASK, UVD_MASTINT_EN__VCPU_EN_MASK,
 * UVD_LMI_CTRL__WRITE_CLEAN_TIMER_EN_MASK, __MASK_MC_URGENT_MASK,
 * __DATA_COHERENCY_EN_MASK, __VCPU_DATA_COHERENCY_EN_MASK,
 * UVD_MPC_CNTL__REPLACEMENT_MODE_MASK, UVD_MPC_SET_MUXA0__VARA_1__SHIFT
 * to __VARA_4__SHIFT, UVD_MPC_SET_MUXB0__VARB_1__SHIFT to
 * __VARB_4__SHIFT, UVD_MPC_SET_MUX__SET_0__SHIFT to __SET_2__SHIFT) and
 * drivers/gpu/drm/amd/amdgpu/vcn_v2_0.c (vcn_v2_0_start: the values).
 * derived: 1 << 6 | 2 << 12 | 3 << 18 | 4 << 24 = 0x040c2040 for each
 * of the first two multiplexers, 1 << 3 | 2 << 6 = 0x88 for the third.
 */
#define AMDRDNA_VCN_PROCESSOR_CLOCK		0x00000200U
#define AMDRDNA_VCN_INTERRUPT_PROCESSOR		0x00000002U
#define AMDRDNA_VCN_MEMORY_SETTINGS		0x00203100U
#define AMDRDNA_VCN_PICTURE_REPLACE_MASK	0x00000038U
#define AMDRDNA_VCN_PICTURE_REPLACE		0x00000010U
#define AMDRDNA_VCN_PICTURE_MUX_A		0x040c2040U
#define AMDRDNA_VCN_PICTURE_MUX_B		0x040c2040U
#define AMDRDNA_VCN_PICTURE_MUX			0x00000088U

/*
 * The soft resets of the memory interface (bit 2), the processor (bit 3)
 * and the memory interface's memory-controller side (bit 13); the memory
 * interface's stall of that side; and the memory interface's status:
 * the four clean bits the stop waits for first, then the two the stall
 * waits for.
 *
 * Values referenced: Linux 7.1
 * drivers/gpu/drm/amd/include/asic_reg/vcn/vcn_2_0_0_sh_mask.h
 * (UVD_SOFT_RESET__LMI_SOFT_RESET_MASK, __VCPU_SOFT_RESET_MASK,
 * __LMI_UMC_SOFT_RESET_MASK, UVD_LMI_CTRL2__STALL_ARB_UMC_MASK,
 * UVD_LMI_STATUS__READ_CLEAN_MASK, __WRITE_CLEAN_MASK,
 * __WRITE_CLEAN_RAW_MASK, __VCPU_LMI_WRITE_CLEAN_MASK,
 * __UMC_WRITE_CLEAN_RAW_MASK, __UMC_READ_CLEAN_RAW_MASK) and
 * drivers/gpu/drm/amd/amdgpu/vcn_v2_0.c (vcn_v2_0_stop).
 */
#define AMDRDNA_VCN_RESET_MEMORY		0x00000004U
#define AMDRDNA_VCN_RESET_PROCESSOR		0x00000008U
#define AMDRDNA_VCN_RESET_MEMORY_CONTROLLER	0x00002000U
#define AMDRDNA_VCN_MEMORY_STALL		0x00000100U
#define AMDRDNA_VCN_MEMORY_CLEAN		0x0000000fU
#define AMDRDNA_VCN_MEMORY_CONTROLLER_CLEAN	0x00000240U

/*
 * The host bridge's doorbell range of the video engine (NBIO window 2),
 * the engine's doorbells as dword indices (the decode ring at slot 0x188,
 * the general encode ring two dwords on, the low-latency one three), and
 * the count of doorbells in the range.
 *
 * Values referenced: Linux 7.1
 * drivers/gpu/drm/amd/include/asic_reg/nbio/nbio_2_3_offset.h
 * (mmBIF_MMSCH0_DOORBELL_RANGE), drivers/gpu/drm/amd/amdgpu/amdgpu_doorbell.h
 * (AMDGPU_NAVI10_DOORBELL64_VCN0_1), vcn_v2_0.c (vcn_v2_0_sw_init: the
 * doorbell indices) and nbio_v2_3.c (nbio_v2_3_vcn_doorbell_range: 8).
 * derived: 0x188 << 1 = 0x310, 0x310 + 2 = 0x312, 0x310 + 3 = 0x313.
 */
#define AMDRDNA_NBIO_REG_VIDEO_DOORBELLS	0x01d3U
#define AMDRDNA_DOORBELL_VIDEO_DECODE		0x0310U
#define AMDRDNA_DOORBELL_VIDEO_ENCODE		0x0312U
#define AMDRDNA_DOORBELL_VIDEO_ENCODE_LOW	0x0313U
#define AMDRDNA_DOORBELL_VIDEO_COUNT		8U

#endif
