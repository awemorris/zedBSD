/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GSP boot path of the nvrtx driver: the parts that touch no hardware.
 *
 * The GSP firmware and the driver talk through memory the driver lays out:
 * the firmware images and their container headers, the placement of the
 * firmware in the top of the VRAM, the three-level table that describes the
 * firmware image to the GSP, and the two shared-memory queues the remote
 * procedure calls travel in.  Every layout here is written as byte offsets
 * into a buffer and filled one field at a time, so the host tests can check
 * each offset against the documented sizes without the hardware.
 */

#ifndef DRIVERS_GPU_NVRTX_GSP_H
#define DRIVERS_GPU_NVRTX_GSP_H

#include <stddef.h>
#include <stdint.h>

/*
 * The page the GSP understands, and the shift that converts to it.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/include/nvkm/subdev/gsp.h (GSP_PAGE_SHIFT,
 * GSP_PAGE_SIZE).
 */
#define NVRTX_GSP_PAGE_SIZE		0x1000U
#define NVRTX_GSP_PAGE_SHIFT		12U

/*
 * The firmware container header at the start of the booter and bootloader
 * files.
 *
 * Six little-endian words: magic, version, file size, offset of the inner
 * header, offset and size of the payload.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/nouveau/include/nvfw/fw.h
 * (struct nvfw_bin_hdr) and nvkm/falcon/fw.c (nvkm_falcon_fw_ctor_hs, the
 * magic).
 */
#define NVRTX_BIN_MAGIC			0x10deU
#define NVRTX_BIN_VERSION		1U
#define NVRTX_BIN_HEADER_BYTES		24U
#define NVRTX_BIN_MAGIC_AT		0U
#define NVRTX_BIN_VERSION_AT		4U
#define NVRTX_BIN_SIZE_AT		8U
#define NVRTX_BIN_INNER_AT		12U
#define NVRTX_BIN_PAYLOAD_AT		16U
#define NVRTX_BIN_PAYLOAD_SIZE_AT	20U

/*
 * The high-security header of a booter image (version 2), nine words.
 *
 * The signature words, the places inside the file that hold the patch
 * location, the patch signature index and the signature count, and the
 * offset of the load header.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/nouveau/include/nvfw/hs.h
 * (struct nvfw_hs_header_v2).
 */
#define NVRTX_HS_HEADER_BYTES		36U
#define NVRTX_HS_SIG_PROD_AT		0U
#define NVRTX_HS_SIG_PROD_SIZE_AT	4U
#define NVRTX_HS_PATCH_LOC_AT		8U
#define NVRTX_HS_PATCH_SIG_AT		12U
#define NVRTX_HS_META_AT		16U
#define NVRTX_HS_META_SIZE_AT		20U
#define NVRTX_HS_SIG_COUNT_AT		24U
#define NVRTX_HS_LOAD_AT		28U

/*
 * The load header of a booter image (version 2): the operating-system code
 * and data ranges, the application count and the first application's range.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/nouveau/include/nvfw/hs.h
 * (struct nvfw_hs_load_header_v2).
 */
#define NVRTX_HS_LOAD_BYTES		36U
#define NVRTX_HS_LOAD_OS_CODE_AT	0U
#define NVRTX_HS_LOAD_OS_CODE_SIZE_AT	4U
#define NVRTX_HS_LOAD_OS_DATA_AT	8U
#define NVRTX_HS_LOAD_OS_DATA_SIZE_AT	12U
#define NVRTX_HS_LOAD_APP_COUNT_AT	16U
#define NVRTX_HS_LOAD_APP0_AT		20U
#define NVRTX_HS_LOAD_APP0_SIZE_AT	24U

/*
 * The descriptor inside the GSP bootloader file: the application version
 * written to the GSP before it starts, and the manifest, monitor data and
 * monitor code offsets the placement record names.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/nvidia/arch/nvalloc/common/inc/rmRiscvUcode.h (RM_RISCV_UCODE_DESC).
 */
#define NVRTX_RV_DESC_BYTES		56U
#define NVRTX_RV_DESC_APP_VERSION_AT	28U
#define NVRTX_RV_DESC_MANIFEST_AT	32U
#define NVRTX_RV_DESC_MONITOR_DATA_AT	40U
#define NVRTX_RV_DESC_MONITOR_CODE_AT	48U

/*
 * The section the driver takes the image from in the GSP firmware's ELF
 * file; the signature's section is the chip family's (its profile).
 *
 * Value referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/gsp.c (r535_gsp_oneinit).
 */
#define NVRTX_GSP_IMAGE_SECTION		".fwimage"

/* The ELF file header's size. */
#define NVRTX_ELF_HEADER_BYTES		64U

/*
 * The placement record of the GSP firmware in the top of the VRAM, 256 bytes.
 *
 * The booter reads it to put the firmware image, the bootloader and the
 * heap into the write-protected region.  Every field is a 64-bit word except
 * the few in the tail the driver leaves zero.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/nvidia/arch/nvalloc/common/inc/gsp/gsp_fw_wpr_meta.h (GspFwWprMeta,
 * GSP_FW_WPR_META_MAGIC, GSP_FW_WPR_META_REVISION).
 */
#define NVRTX_WPRM_BYTES		256U
#define NVRTX_WPRM_MAGIC		0xdc3aae21371a60b3ULL
#define NVRTX_WPRM_REVISION		1U
#define NVRTX_WPRM_MAGIC_AT		0U
#define NVRTX_WPRM_REVISION_AT		8U
#define NVRTX_WPRM_IMAGE_TABLE_AT	16U
#define NVRTX_WPRM_IMAGE_SIZE_AT	24U
#define NVRTX_WPRM_BOOT_ADDRESS_AT	32U
#define NVRTX_WPRM_BOOT_SIZE_AT		40U
#define NVRTX_WPRM_BOOT_CODE_AT		48U
#define NVRTX_WPRM_BOOT_DATA_AT		56U
#define NVRTX_WPRM_BOOT_MANIFEST_AT	64U
#define NVRTX_WPRM_SIGNATURE_AT		72U
#define NVRTX_WPRM_SIGNATURE_SIZE_AT	80U
#define NVRTX_WPRM_RESERVED_START_AT	88U
#define NVRTX_WPRM_OUTER_HEAP_AT	96U
#define NVRTX_WPRM_OUTER_HEAP_SIZE_AT	104U
#define NVRTX_WPRM_REGION_START_AT	112U
#define NVRTX_WPRM_HEAP_AT		120U
#define NVRTX_WPRM_HEAP_SIZE_AT		128U
#define NVRTX_WPRM_IMAGE_AT		136U
#define NVRTX_WPRM_BOOT_AT		144U
#define NVRTX_WPRM_FRTS_AT		152U
#define NVRTX_WPRM_FRTS_SIZE_AT		160U
#define NVRTX_WPRM_REGION_END_AT	168U
#define NVRTX_WPRM_VRAM_SIZE_AT		176U
#define NVRTX_WPRM_VGA_AT		184U
#define NVRTX_WPRM_VGA_SIZE_AT		192U
#define NVRTX_WPRM_BOOT_COUNT_AT	200U

/*
 * One memory region the GSP's operating system is told about at start, 32
 * bytes.
 *
 * An eight-character name packed into a word, the bus address, the size,
 * the kind (1 = one contiguous range) and the location (1 = system memory).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/nvrm/gsp.h
 * (LibosMemoryRegionInitArgument, LIBOS_MEMORY_REGION_CONTIGUOUS,
 * LIBOS_MEMORY_REGION_LOC_SYSMEM) and nvkm/subdev/gsp/rm/r535/gsp.c
 * (r535_gsp_libos_init).
 */
#define NVRTX_LIBOS_REGION_BYTES	32U
#define NVRTX_LIBOS_REGION_COUNT	4U
#define NVRTX_LIBOS_NAME_AT		0U
#define NVRTX_LIBOS_ADDRESS_AT		8U
#define NVRTX_LIBOS_SIZE_AT		16U
#define NVRTX_LIBOS_KIND_AT		24U
#define NVRTX_LIBOS_LOCATION_AT		25U
#define NVRTX_LIBOS_KIND_CONTIGUOUS	1U
#define NVRTX_LIBOS_LOCATION_SYSTEM	1U

/*
 * The three log buffers the GSP writes, each 64 KiB, and the argument page.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/gsp.c (r535_gsp_libos_init,
 * r535_gsp_rmargs_init).
 */
#define NVRTX_GSP_LOG_BYTES		0x10000U
#define NVRTX_GSP_ARGS_BYTES		0x1000U

/*
 * The resource manager's start arguments (the 570.144 form), 72 bytes.
 *
 * Where the queues are, the suspend-resume state (all zero on a cold start),
 * the GPU instance, and the stack-in-DMEM flag this firmware version needs.
 *
 * Value referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/nvidia/inc/kernel/gpu/gsp/gsp_init_args.h (GSP_ARGUMENTS_CACHED).
 */
#define NVRTX_RMARGS_BYTES		72U

/*
 * The region the start arguments are named as to the GSP's operating system:
 * one whole page, as both other drivers give it.  The operating system maps
 * its regions by pages and stops at a region of the arguments' own 72 bytes
 * (its init log ends after mapping that region).
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/nvidia/inc/kernel/gpu/gsp/gsp_init_args.h (GSP_ARGUMENTS_CACHED,
 * MESSAGE_QUEUE_INIT_ARGUMENTS, GSP_SR_INIT_ARGUMENTS) and Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/gsp.c (r535_gsp_rmargs_init).
 */
#define NVRTX_RMARGS_REGION_BYTES	0x1000U
#define NVRTX_RMARGS_QUEUE_PAGES_AT	0U
#define NVRTX_RMARGS_QUEUE_PTE_COUNT_AT	8U
#define NVRTX_RMARGS_COMMAND_AT		16U
#define NVRTX_RMARGS_STATUS_AT		24U
#define NVRTX_RMARGS_OLD_LEVEL_AT	32U
#define NVRTX_RMARGS_SR_FLAGS_AT	36U
#define NVRTX_RMARGS_IN_TRANSITION_AT	40U
#define NVRTX_RMARGS_INSTANCE_AT	44U
#define NVRTX_RMARGS_DMEM_STACK_AT	48U
#define NVRTX_RMARGS_PROFILER_AT	56U
#define NVRTX_RMARGS_PROFILER_SIZE_AT	64U

/*
 * The shared queue memory: a page of page addresses, then the command queue
 * (driver to GSP), then the status queue (GSP to driver).  Each queue is a
 * header page followed by 63 slots of one page.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/gsp.c (r535_gsp_shared_init).
 */
#define NVRTX_QUEUE_BYTES		0x40000U
#define NVRTX_QUEUE_SLOT_BYTES		0x1000U
#define NVRTX_QUEUE_SLOT_COUNT		63U
#define NVRTX_QUEUE_COMMAND_AT		0x1000U
#define NVRTX_QUEUE_STATUS_AT		0x41000U
#define NVRTX_QUEUE_SHARED_BYTES	0x81000U

/*
 * The header page of one queue: the writer's eight words, then the reader's
 * word.  Each side writes only its own words; the driver's read pointer of the
 * status queue lives in the command queue's reader word, and the GSP's read
 * pointer of the command queue in the status queue's reader word.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/nvrm/gsp.h (msgqTxHeader,
 * msgqRxHeader).
 */
#define NVRTX_QHDR_VERSION_AT		0U
#define NVRTX_QHDR_SIZE_AT		4U
#define NVRTX_QHDR_SLOT_SIZE_AT		8U
#define NVRTX_QHDR_SLOT_COUNT_AT	12U
#define NVRTX_QHDR_WRITE_AT		16U
#define NVRTX_QHDR_FLAGS_AT		20U
#define NVRTX_QHDR_READER_AT		24U
#define NVRTX_QHDR_SLOTS_AT		28U
#define NVRTX_QHDR_READ_AT		32U

/*
 * The frame of one message in a queue: a 48-byte element header, then the
 * call header, then the payload.  A message spans 1 to 16 slots.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/nvidia/inc/kernel/gpu/gsp/message_queue_priv.h (GSP_MSG_QUEUE_ELEMENT,
 * GSP_MSG_QUEUE_ELEMENT_SIZE_MAX).
 */
#define NVRTX_ELEMENT_HEADER_BYTES	48U
#define NVRTX_ELEMENT_CHECKSUM_AT	32U
#define NVRTX_ELEMENT_SEQUENCE_AT	36U
#define NVRTX_ELEMENT_SLOTS_AT		40U
#define NVRTX_ELEMENT_MAX_SLOTS		16U

/*
 * The call header, 32 bytes: version, signature "VRPC" in memory, the length
 * of header and payload, the function, the two result words (all ones when
 * sent), the caller's sequence, and a spare word.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/nvrm/gsp.h
 * (rpc_message_header_v03_00) and nvkm/subdev/gsp/rm/r535/rpc.c
 * (r535_gsp_rpc_get).
 */
#define NVRTX_CALL_HEADER_BYTES		32U
#define NVRTX_CALL_VERSION		0x03000000U
#define NVRTX_CALL_SIGNATURE		0x43505256U
#define NVRTX_CALL_VERSION_AT		0U
#define NVRTX_CALL_SIGNATURE_AT		4U
#define NVRTX_CALL_LENGTH_AT		8U
#define NVRTX_CALL_FUNCTION_AT		12U
#define NVRTX_CALL_RESULT_AT		16U
#define NVRTX_CALL_RESULT_PRIVATE_AT	20U
#define NVRTX_CALL_SEQUENCE_AT		24U
#define NVRTX_CALL_SPARE_AT		28U
#define NVRTX_CALL_RESULT_PENDING	0xffffffffU

/* The largest payload one message carries: sixteen slots less both headers. */
#define NVRTX_STAGING_BYTES		(NVRTX_ELEMENT_MAX_SLOTS * NVRTX_QUEUE_SLOT_BYTES)
#define NVRTX_CALL_PAYLOAD_MAX \
	(NVRTX_ELEMENT_MAX_SLOTS * NVRTX_QUEUE_SLOT_BYTES - NVRTX_ELEMENT_HEADER_BYTES - NVRTX_CALL_HEADER_BYTES)

/*
 * Memory the GSP reads, described one page at a time.
 *
 * The kernel's DMA buffers are at most 64 KiB, so a large region is a run of
 * chunks; each chunk is page aligned and a whole number of pages, so one page
 * never straddles two chunks.  The CPU address and the bus address of page i
 * are found through its chunk.  The owner keeps the chunks alive for as long
 * as the GSP may read them.
 */
struct nvrtx_pages {
	size_t chunk_count;
	size_t chunk_bytes;
	size_t page_count;
	uint8_t **chunk_cpu;
	uint64_t *chunk_bus;
};

/* A firmware file and the parts of it the boot uses, by offset into its bytes. */
struct nvrtx_booter {
	const uint8_t *file;
	size_t file_size;
	/* The payload copied to the falcon: offset into the file and length. */
	size_t payload_at;
	size_t payload_size;
	/* Where in the payload the chosen signature is written before the boot. */
	uint32_t patch_location;
	uint32_t patch_signature;
	uint32_t signature_count;
	size_t signatures_at;
	size_t signature_size;
	uint32_t os_code_at;
	uint32_t os_code_size;
	uint32_t os_data_at;
	uint32_t os_data_size;
	uint32_t app_at;
	uint32_t app_size;
	/*
	 * What the boot ROM of an Ampere or later falcon is told with the
	 * booter (the booter's meta data, three words): the fuse version its
	 * signatures were made for, the engine it runs on, and its ucode
	 * number.  has_meta is zero when the file carries none.
	 */
	int has_meta;
	uint32_t fuse_version;
	uint32_t engine_id;
	uint32_t ucode_id;
};

/* The GSP's RISC-V bootloader file and what the placement record names of it. */
struct nvrtx_bootloader {
	size_t payload_at;
	size_t payload_size;
	uint32_t app_version;
	uint32_t manifest_at;
	uint32_t monitor_data_at;
	uint32_t monitor_code_at;
};

/* The section table of an ELF file, as its file header describes it. */
struct nvrtx_elf {
	size_t file_size;
	uint64_t table_at;
	size_t table_bytes;
	uint32_t entry_bytes;
	uint32_t count;
	uint32_t names_index;
};

/*
 * The falcon bootloader's descriptor: the start tag, then the code's offset and
 * size in the payload.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/nouveau/include/nvfw/fw.h
 * (struct nvfw_bl_desc).
 */
#define NVRTX_FALCON_BL_DESC_BYTES	24U
#define NVRTX_FALCON_BL_START_TAG_AT	0U
#define NVRTX_FALCON_BL_CODE_AT		8U
#define NVRTX_FALCON_BL_CODE_SIZE_AT	12U

/* The falcon bootloader's code, by pointer into its file. */
struct nvrtx_falcon_bl {
	const uint8_t *code;
	uint32_t code_size;
	uint32_t start_tag;
};

/*
 * A point where the GSP boot is left with a program that may still run:
 * FWSEC-FRTS did not halt, booter_load did not halt, the firmware never
 * said it was up, the firmware did not unload, or a program of the
 * region's teardown did not halt.
 */
enum nvrtx_gsp_hold {
	NVRTX_GSP_HOLD_FWSEC = 0,
	NVRTX_GSP_HOLD_BOOTER,
	NVRTX_GSP_HOLD_INIT,
	NVRTX_GSP_HOLD_UNLOAD,
	NVRTX_GSP_HOLD_TEAR_DOWN
};

/* The falcons nvrtx_gsp_hold_falcons() names: the GSP's and the secure processor's. */
#define NVRTX_GSP_HOLD_GSP_FALCON	0x1U
#define NVRTX_GSP_HOLD_SEC2_FALCON	0x2U

/* One section of an ELF file, by offset into the file. */
struct nvrtx_section {
	size_t at;
	size_t size;
};

/*
 * The placement of the GSP firmware in the top of the VRAM.
 *
 * Computed from the VRAM size, the VGA workspace and the sizes of the
 * images; every address is a VRAM offset.  From the top down: the VGA
 * workspace, the FRTS region the VBIOS firmware fills, the bootloader, the
 * firmware image, the GSP heap, the placement record's region start, and
 * the 1 MiB heap outside the protected region.
 */
struct nvrtx_wpr_layout {
	uint64_t vram_size;
	uint64_t vga;
	uint64_t vga_size;
	uint64_t frts;
	uint64_t frts_size;
	uint64_t boot;
	uint64_t boot_size;
	uint64_t image;
	uint64_t image_size;
	uint64_t heap;
	uint64_t heap_size;
	uint64_t region;
	uint64_t region_size;
	uint64_t outer_heap;
	uint64_t outer_heap_size;
};

/*
 * The bus addresses and sizes the placement record points the booter at.
 */
struct nvrtx_wpr_sources {
	uint64_t image_table;
	uint64_t boot_address;
	uint64_t boot_size;
	uint64_t signature;
	uint64_t signature_size;
	const struct nvrtx_bootloader *bootloader;
};

/*
 * One direction of the shared queue memory as the driver sees it.
 *
 * The pages are those of the whole shared region; the queue starts at a
 * byte offset into them.  The writer's header is in this queue, the reader's
 * pointer of the same queue in the other queue's header page.
 */
struct nvrtx_queue {
	const struct nvrtx_pages *pages;
	size_t at;
	size_t peer_at;
};

/*
 * The driver's side of the two queues.
 *
 * It sends in the command queue and receives from the status queue.  The
 * element sequence counts every element the driver sends.
 */
struct nvrtx_channel {
	struct nvrtx_queue command;
	struct nvrtx_queue status;
	uint32_t element_sequence;
	/* One whole message (sixteen slots) while it is built and checksummed. */
	uint8_t *staging;
};

/*
 * The size of one FWSEC signature of a version 3 descriptor.
 *
 * Value referenced: Linux v6.19 drivers/gpu/drm/nouveau/nvkm/subdev/gsp/fwsec.c
 * (nvkm_gsp_fwsec_v3).
 */
#define NVRTX_FWSEC_SIGNATURE_BYTES	384U

/*
 * The two FWSEC commands: set up the protected region, and tear it down.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/fwsec.c
 * (NVFW_FALCON_APPIF_DMEMMAPPER_CMD_FRTS, NVFW_FALCON_APPIF_DMEMMAPPER_CMD_SB).
 */
#define NVRTX_FWSEC_FRTS		0x15U
#define NVRTX_FWSEC_SB			0x19U

/*
 * A ROM read into memory and the shape of its chain of images.
 *
 * A pointer at or past the end of the first image is read in the falcon
 * image instead.  The end is where the last image ends.
 */
struct nvrtx_vbios {
	const uint8_t *rom;
	size_t size;
	size_t image0_size;
	size_t falcon_image;
	size_t end;
};

/*
 * The FWSEC program as its descriptor (version 2) describes it.
 *
 * The program is the code (non-secure then secure) followed by the data;
 * program_at is its offset in the ROM bytes.  The interface table is at
 * interface_at in the data block, which starts at dmem_at in the program.
 */
struct nvrtx_fwsec {
	uint32_t version;
	/*
	 * Version 3 (Ampere and later) only: the signature's place in the data
	 * block, the engine and ucode the boot ROM is told, how many signatures
	 * the ROM holds and the fuse versions they were made for, and where they
	 * are in the ROM bytes (each NVRTX_FWSEC_SIGNATURE_BYTES long).  The data
	 * block is copied by DMA in whole 256-byte blocks: dmem_load_size.
	 */
	uint32_t signature_at;
	uint32_t engine_id;
	uint32_t ucode_id;
	uint32_t signature_count;
	uint32_t signature_versions;
	size_t signatures_at;
	uint32_t dmem_load_size;
	uint32_t interface_at;
	uint32_t imem_base;
	uint32_t imem_size;
	uint32_t imem_secure_base;
	uint32_t imem_secure_size;
	uint32_t dmem_at;
	uint32_t dmem_base;
	uint32_t dmem_size;
	size_t program_at;
	uint32_t program_size;
};

/*
 * The call numbers of the 570.144 resource manager the boot uses.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r570/nvrm/rpcfn.h (FREE,
 * UNLOADING_GUEST_DRIVER, GET_GSP_STATIC_INFO, CONTINUATION_RECORD,
 * GSP_SET_SYSTEM_INFO, SET_REGISTRY, GSP_RM_CONTROL, GSP_RM_ALLOC).
 */
#define NVRTX_CALL_FREE			10U
#define NVRTX_CALL_UNLOAD		47U
#define NVRTX_CALL_STATIC_INFO		65U
#define NVRTX_CALL_CONTINUATION		71U
#define NVRTX_CALL_SYSTEM_INFO		72U
#define NVRTX_CALL_REGISTRY		73U
#define NVRTX_CALL_CONTROL		76U
#define NVRTX_CALL_ALLOCATE		103U

/*
 * The events the GSP sends on its own (numbers from 0x1000).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r570/nvrm/msgfn.h (FIRST_EVENT,
 * GSP_INIT_DONE, GSP_RUN_CPU_SEQUENCER, OS_ERROR_LOG, UCODE_LIBOS_PRINT,
 * PERF_BRIDGELESS_INFO_UPDATE, GSP_POST_NOCAT_RECORD).
 */
#define NVRTX_EVENT_FIRST		0x1000U
#define NVRTX_EVENT_INIT_DONE		0x1001U
#define NVRTX_EVENT_SEQUENCER		0x1002U
#define NVRTX_EVENT_ERROR_LOG		0x1006U
#define NVRTX_EVENT_LIBOS_PRINT		0x100cU
#define NVRTX_EVENT_BRIDGELESS		0x100fU
#define NVRTX_EVENT_POST_NOCAT		0x1020U

/*
 * The system information's length, and the unload call's.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r570/nvrm/gsp.h (GspSystemInfo)
 * and nvkm/subdev/gsp/rm/r535/nvrm/gsp.h (rpc_unloading_guest_driver_v1F_07).
 */
#define NVRTX_SYSINFO_BYTES		928U
#define NVRTX_UNLOAD_ARGS_BYTES		8U

/*
 * The CPU sequencer's opcodes.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/nvidia/arch/nvalloc/common/inc/rmgspseq.h (GSP_SEQ_BUF_OPCODE).
 */
#define NVRTX_SEQ_WRITE			0U
#define NVRTX_SEQ_MODIFY		1U
#define NVRTX_SEQ_POLL			2U
#define NVRTX_SEQ_DELAY			3U
#define NVRTX_SEQ_STORE			4U
#define NVRTX_SEQ_CORE_RESET		5U
#define NVRTX_SEQ_CORE_START		6U
#define NVRTX_SEQ_CORE_WAIT_HALT	7U
#define NVRTX_SEQ_CORE_RESUME		8U

/*
 * The static information the firmware reports, and the fields the driver reads
 * of it.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r570/nvrm/gsp.h
 * (GspStaticConfigInfo: fb_length, gpuShortNameString, bar1PdeBase,
 * bar2PdeBase, hInternalClient, bIsEfiInit).
 */
#define NVRTX_STATIC_INFO_BYTES		1656U
#define NVRTX_STATIC_FB_LENGTH_AT	1224U
#define NVRTX_STATIC_SHORT_NAME_AT	1324U
#define NVRTX_STATIC_NAME_BYTES		64U
#define NVRTX_STATIC_BAR1_ROOT_AT	1536U
#define NVRTX_STATIC_BAR2_ROOT_AT	1544U
#define NVRTX_STATIC_INTERNAL_CLIENT_AT	1600U
#define NVRTX_STATIC_UEFI_AT		1614U

/*
 * The allocation call's header (seven words and four reserved bytes, 32
 * bytes; parameters put 12 bytes late leave the space's flags unread) and
 * where the resource manager reports its status.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/nvrm/alloc.h
 * (rpc_gsp_rm_alloc_v03_00).
 */
#define NVRTX_ALLOC_HEADER_BYTES	32U
#define NVRTX_ALLOC_STATUS_AT		16U

/*
 * The three classes of the first objects, and their parameter lengths.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/class/cl0000.h (NV01_ROOT,
 * NV0000_ALLOC_PARAMETERS), class/cl0080.h (NV01_DEVICE_0,
 * NV0080_ALLOC_PARAMETERS) and class/cl2080.h (NV20_SUBDEVICE_0,
 * NV2080_ALLOC_PARAMETERS).
 */
#define NVRTX_CLASS_ROOT		0x0000U
#define NVRTX_CLASS_DEVICE		0x0080U
#define NVRTX_CLASS_SUBDEVICE		0x2080U
#define NVRTX_ROOT_PARAMS_BYTES		120U
#define NVRTX_ROOT_PROCESS_AT		4U
#define NVRTX_DEVICE_PARAMS_BYTES	56U
#define NVRTX_DEVICE_SHARE_AT		4U
#define NVRTX_SUBDEVICE_PARAMS_BYTES	4U

/*
 * The FB region table in the static information.
 *
 * Value referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r570/nvrm/gsp.h
 * (GspStaticConfigInfo, fbRegionInfoParams).
 */
#define NVRTX_STATIC_REGIONS_AT		344U

/* The driver's page tables: five pages covering one 2 MiB range. */
#define NVRTX_MMU_TABLES_BYTES		0x5000U
#define NVRTX_MMU_RANGE_BYTES		0x200000ULL

/*
 * A channel's instance block, its RAMFC (the block's first part) and its USERD.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/engine/fifo/gf100.c (gf100_chan_inst),
 * nvkm/engine/fifo/gv100.c (gv100_chan_userd) and
 * nvkm/subdev/gsp/rm/r535/fifo.c (r535_chan_alloc).
 */
#define NVRTX_INSTANCE_BYTES		0x1000U
#define NVRTX_RAMFC_BYTES		0x200U
#define NVRTX_USERD_BYTES		0x200U

/*
 * The control call's header, and the parameters of the calls address-space to
 * channel-stop make.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/nvrm/ctrl.h
 * (rpc_gsp_rm_control_v03_00); NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/ctrl/ctrl0080/ctrl0080dma.h
 * (NV0080_CTRL_DMA_SET_PAGE_DIRECTORY_PARAMS,
 * NV0080_CTRL_DMA_UNSET_PAGE_DIRECTORY_PARAMS), ctrl/ctrl2080/ctrl2080fifo.h
 * (NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE_PARAMS), ctrl/ctrl2080/ctrl2080ce.h
 * (NV2080_CTRL_CE_GET_FAULT_METHOD_BUFFER_SIZE_PARAMS), alloc/alloc_channel.h
 * (NV_CHANNEL_ALLOC_PARAMS), ctrl/ctrla06f/ctrla06fgpfifo.h
 * (NVA06F_CTRL_BIND_PARAMS, NVA06F_CTRL_GPFIFO_SCHEDULE_PARAMS),
 * class/clb0b5sw.h (NVB0B5_ALLOCATION_PARAMETERS) and nvos.h
 * (NV_VASPACE_ALLOCATION_PARAMETERS, NVOS00_PARAMETERS).
 */
#define NVRTX_CONTROL_HEADER_BYTES	24U
#define NVRTX_CONTROL_STATUS_AT		12U
#define NVRTX_SPACE_PARAMS_BYTES	48U
#define NVRTX_PDIR_PARAMS_BYTES		32U
#define NVRTX_UNPDIR_PARAMS_BYTES	8U
#define NVRTX_DEVINFO_PARAMS_BYTES	3212U
#define NVRTX_METHOD_BUFFER_PARAMS_BYTES 4U
#define NVRTX_CHANNEL_PARAMS_BYTES	368U
#define NVRTX_BIND_PARAMS_BYTES		4U
#define NVRTX_SCHEDULE_PARAMS_BYTES	2U
#define NVRTX_COPY_PARAMS_BYTES		8U
#define NVRTX_FREE_PARAMS_BYTES		16U

/* The most bytes nvrtx_push_copy_clear() writes: thirteen words. */
#define NVRTX_PUSH_COPY_CLEAR_BYTES	52U

/*
 * The memory kinds of a page entry: pitch (every run's), and S8Z24, which
 * the 3D engine's depth target in S8Z24 needs its pages to be (else a Z
 * kind violation).
 *
 * Values referenced: Mesa 26.2.6
 * src/nouveau/headers/nvidia/hwref/turing/tu102/dev_mmu.h
 * (NV_MMU_PTE_KIND_PITCH, NV_MMU_PTE_KIND_S8Z24) and src/nouveau/nil/image.rs
 * (tu102_choose_pte_kind: Z24_UNORM_S8_UINT uncompressed is S8Z24).
 */
#define NVRTX_MMU_KIND_PITCH		0x00U
#define NVRTX_MMU_KIND_S8Z24		0x03U

/*
 * The control commands of address-space to channel-stop.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/ctrl/ctrl0080/ctrl0080dma.h
 * (NV0080_CTRL_CMD_DMA_SET_PAGE_DIRECTORY,
 * NV0080_CTRL_CMD_DMA_UNSET_PAGE_DIRECTORY), ctrl/ctrl2080/ctrl2080fifo.h
 * (NV2080_CTRL_CMD_FIFO_GET_DEVICE_INFO_TABLE), ctrl/ctrl2080/ctrl2080ce.h
 * (NV2080_CTRL_CMD_CE_GET_FAULT_METHOD_BUFFER_SIZE) and
 * ctrl/ctrla06f/ctrla06fgpfifo.h (NVA06F_CTRL_CMD_BIND,
 * NVA06F_CTRL_CMD_GPFIFO_SCHEDULE).
 */
#define NVRTX_CTL_SET_PAGE_DIRECTORY	0x00801813U
#define NVRTX_CTL_UNSET_PAGE_DIRECTORY	0x00801814U
#define NVRTX_CTL_DEVICE_INFO		0x20801112U
#define NVRTX_CTL_METHOD_BUFFER_SIZE	0x20802a08U
#define NVRTX_CTL_BIND			0xa06f0104U
#define NVRTX_CTL_SCHEDULE		0xa06f0103U

/*
 * A channel's stop (off its runlist, preempted at once when asked) and its
 * reset (the channel and its engine).
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/ctrl/ctrla06f/ctrla06fgpfifo.h
 * (NVA06F_CTRL_CMD_STOP_CHANNEL) and
 * src/common/sdk/nvidia/inc/ctrl/ctrl906f.h (NV906F_CTRL_CMD_RESET_CHANNEL).
 */
#define NVRTX_CTL_STOP_CHANNEL		0xa06f0112U
#define NVRTX_CTL_RESET_CHANNEL		0x906f0102U

/*
 * The classes of address-space and copy-channel on Turing (a generation's own
 * are its profile's).
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/class/cl90f1.h (FERMI_VASPACE_A), class/clc46f.h
 * (TURING_CHANNEL_GPFIFO_A) and class/clc5b5.h (TURING_DMA_COPY_A).
 */
#define NVRTX_CLASS_SPACE		0x90f1U
#define NVRTX_CLASS_CHANNEL		0xc46fU
#define NVRTX_CLASS_COPY		0xc5b5U

/*
 * The event a channel error raises.
 *
 * Value referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r570/nvrm/msgfn.h (RC_TRIGGERED).
 */
#define NVRTX_EVENT_CHANNEL_ERROR	0x1004U

/*
 * The graphics context (golden-context): the controls and classes, the
 * parameter lengths, and the context buffers the resource manager names.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080internal.h
 * (NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_CONTEXT_BUFFERS_INFO,
 * NV2080_CTRL_CMD_INTERNAL_KGR_INIT_BUG4208224_WAR and their parameters),
 * ctrl/ctrl2080/ctrl2080gpu.h (NV2080_CTRL_CMD_GPU_PROMOTE_CTX,
 * NV2080_CTRL_GPU_PROMOTE_CTX_PARAMS), ctrl/ctrl90f1.h
 * (NV90F1_CTRL_CMD_VASPACE_COPY_SERVER_RESERVED_PDES and its parameters),
 * class/clc597.h (TURING_A) and class/cl2080_notification.h
 * (NV2080_ENGINE_TYPE_GRAPHICS); Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/gr.h (R515_GR_MAX_CTXBUFS).
 */
#define NVRTX_CTL_GR_BUFFERS_INFO	0x20800a32U
#define NVRTX_CTL_PROMOTE_CONTEXT	0x2080012bU
#define NVRTX_CTL_COPY_SERVER_PDES	0x90f10106U
#define NVRTX_CTL_SCRUBBER		0x20800a46U
#define NVRTX_CLASS_THREED		0xc597U
#define NVRTX_GR_BUFFERS_INFO_BYTES	1664U
#define NVRTX_PROMOTE_PARAMS_BYTES	560U
#define NVRTX_COPY_PDES_PARAMS_BYTES	184U
#define NVRTX_SCRUBBER_PARAMS_BYTES	1U
#define NVRTX_GR_BUFFER_MAX		9U
#define NVRTX_ENGINE_GRAPHICS		1U

/*
 * The server's reserved range in a space the resource manager shares: 512 MiB
 * at 4 GiB.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/nvrm/vmm.h
 * (SPLIT_VAS_SERVER_RM_MANAGED_VA_START, SPLIT_VAS_SERVER_RM_MANAGED_VA_SIZE).
 */
#define NVRTX_SERVER_VA			0x100000000ULL
#define NVRTX_SERVER_VA_BYTES		0x20000000ULL

/*
 * One graphics context buffer, as the resource manager describes it.
 *
 * The promote call names it by its buffer number.  A global buffer is made
 * once and shared by every graphics channel; an initialized one is filled
 * by the resource manager, which needs its physical address; a read-only one
 * is mapped without write access.  The page is the mapping's page size
 * (log 2) and the alignment the address's (log 2).
 */
struct nvrtx_gr_buffer {
	uint32_t buffer_id;
	uint64_t size;
	uint32_t page_shift;
	uint32_t align_shift;
	int global;
	int initialize;
	int read_only;
};

/* One entry of a promote call: where a buffer is and what the resource manager does with it. */
struct nvrtx_promote_entry {
	uint64_t physical;
	uint64_t virtual_address;
	uint64_t size;
	uint32_t buffer_id;
	int initialize;
	int nonmapped;
};

/* The copy engine chosen for the first channel. */
struct nvrtx_engine_pick {
	uint32_t engine;
	uint32_t runlist;
	uint32_t graphics_runlist;
	/* A copy engine on the graphics runlist (0xffffffff: none), which a graphics channel's copy object runs on. */
	uint32_t graphics_copy;
	/* Whether the first video decoder is in the table, its runlist and its engine descriptor. */
	uint32_t nvdec_found;
	uint32_t nvdec_runlist;
	uint32_t nvdec_descriptor;
	/* Whether the first video encoder is in the table, its runlist and its engine descriptor. */
	uint32_t nvenc_found;
	uint32_t nvenc_runlist;
	uint32_t nvenc_descriptor;
};

/* Where a channel's pieces are, for its allocation. */
struct nvrtx_channel_setup {
	uint64_t ring_va;
	uint32_t ring_entries;
	uint32_t chid;
	uint32_t space;
	uint32_t engine;
	/* Nonzero for a privileged (admin) channel, as the golden context's is. */
	uint32_t privileged;
	uint64_t instance;
	uint64_t userd;
	uint64_t methods;
	uint64_t methods_size;
};

/* One resource-manager object: its client, parent, own handle and class. */
struct nvrtx_object {
	uint32_t client;
	uint32_t parent;
	uint32_t handle;
	uint32_t class_number;
};

/* What the system information says about the GPU. */
struct nvrtx_sysinfo_values {
	uint64_t registers;
	uint64_t vram;
	uint64_t instance;
	uint64_t bus_device_function;
	uint64_t user_address_limit;
	uint64_t console_size;
	uint16_t vendor;
	uint16_t device;
	uint16_t subvendor;
	uint16_t subdevice;
	uint32_t revision;
	int primary;
};

/*
 * One decoded command of the CPU sequencer.
 *
 * The arguments by opcode: write (address, value), modify (address, mask,
 * value), poll (address, mask, value, timeout in us, error), delay (us),
 * store (address, slot).
 */
struct nvrtx_seq_command {
	uint32_t opcode;
	uint32_t argument[5];
};

uint32_t nvrtx_get32(const uint8_t *bytes, size_t at);
uint64_t nvrtx_get64(const uint8_t *bytes, size_t at);
void nvrtx_put8(uint8_t *bytes, size_t at, uint8_t value);
void nvrtx_put32(uint8_t *bytes, size_t at, uint32_t value);
void nvrtx_put64(uint8_t *bytes, size_t at, uint64_t value);
uint32_t nvrtx_fold_checksum(const uint8_t *bytes, size_t length);

uint8_t *nvrtx_pages_cpu(const struct nvrtx_pages *pages, size_t page);
uint64_t nvrtx_pages_bus(const struct nvrtx_pages *pages, size_t page);

int nvrtx_booter_parse(const uint8_t *file, size_t size, struct nvrtx_booter *booter);
int nvrtx_booter_signature(const struct nvrtx_booter *booter, unsigned index, size_t *at);
uint32_t nvrtx_booter_fuse_register(uint32_t engine_id, uint32_t ucode_id);
int nvrtx_booter_fuse_index(const struct nvrtx_booter *booter, uint32_t fuse, unsigned *index);
int nvrtx_falcon_bl_parse(const uint8_t *file, size_t size, struct nvrtx_falcon_bl *bl);
int nvrtx_bootloader_parse(const uint8_t *file, size_t size, struct nvrtx_bootloader *bootloader);
int nvrtx_elf_header(const uint8_t *header, size_t file_size, struct nvrtx_elf *elf);
int nvrtx_elf_entry(const struct nvrtx_elf *elf, const uint8_t *table, uint32_t index, struct nvrtx_section *section);
int nvrtx_elf_find(const struct nvrtx_elf *elf, const uint8_t *table, const uint8_t *names, size_t names_size, const char *name, struct nvrtx_section *section);
int nvrtx_elf_section(const uint8_t *file, size_t size, const char *name, struct nvrtx_section *section);

uint64_t nvrtx_vram_size(uint32_t value);
uint64_t nvrtx_vram_size_mib(uint32_t value);
uint64_t nvrtx_vga_workspace(uint64_t vram_size, uint32_t value);
int nvrtx_wpr_layout(uint64_t vram_size, uint64_t vga, uint64_t boot_size, uint64_t image_size, uint64_t heap_os, uint64_t heap_min, struct nvrtx_wpr_layout *layout);
int nvrtx_wpr_scrubbed(const struct nvrtx_wpr_layout *layout);
void nvrtx_wpr_record(uint8_t *record, const struct nvrtx_wpr_layout *layout, const struct nvrtx_wpr_sources *sources);
void nvrtx_libos_region(uint8_t *page, unsigned index, const char *name, uint64_t address, uint64_t size);
void nvrtx_log_buffer(uint8_t *buffer, uint64_t bus, size_t size);
void nvrtx_rmargs(uint8_t *args, uint64_t queue_pages, uint32_t pte_count);

uint32_t nvrtx_gsp_hold_falcons(enum nvrtx_gsp_hold point);

size_t nvrtx_radix3_pages(size_t image_pages);
int nvrtx_radix3_fill(const struct nvrtx_pages *image, const struct nvrtx_pages *table);

void nvrtx_queue_shared_init(const struct nvrtx_pages *shared);
void nvrtx_channel_init(struct nvrtx_channel *channel, const struct nvrtx_pages *shared, uint8_t *staging);
int nvrtx_channel_send(struct nvrtx_channel *channel, uint32_t function, uint32_t sequence, const uint8_t *payload, size_t length);
int nvrtx_channel_peek(const struct nvrtx_channel *channel, uint32_t *function, uint32_t *sequence, size_t *length, uint32_t *result);
int nvrtx_channel_receive(struct nvrtx_channel *channel, uint8_t *payload, size_t capacity, size_t *length);

void nvrtx_sysinfo(uint8_t *record, const struct nvrtx_sysinfo_values *values);
int nvrtx_registry(uint8_t *registry, size_t capacity, size_t *length);
void nvrtx_unload_args(uint8_t *args);
size_t nvrtx_alloc_call(uint8_t *call, size_t capacity, const struct nvrtx_object *object, const uint8_t *parameters, size_t parameters_length);
int nvrtx_sequencer_next(const uint8_t *payload, size_t length, uint32_t *cursor, struct nvrtx_seq_command *command);
int nvrtx_sequencer_save(uint8_t *payload, size_t length, uint32_t slot, uint32_t value);

int nvrtx_mmu_tables(uint8_t *image, uint64_t block, uint64_t va, const uint64_t *pages, size_t count);
void nvrtx_mmu_instance(uint8_t *instance, uint64_t root, uint64_t limit);
int nvrtx_mmu_vram_block(const uint8_t *static_info, uint64_t size, uint64_t *block, uint32_t *region);
int nvrtx_mmu_vram_region(const uint8_t *static_info, uint64_t *base, uint64_t *top, uint32_t *region);
uint64_t nvrtx_mmu_vram_pte(uint64_t physical);

size_t nvrtx_control_call(uint8_t *call, size_t capacity, uint32_t client, uint32_t object, uint32_t command, const uint8_t *parameters, size_t length);
void nvrtx_space_params(uint8_t *params);
void nvrtx_pdir_params(uint8_t *params, uint64_t root, uint32_t space);
int nvrtx_devinfo_pick(const uint8_t *params, size_t length, struct nvrtx_engine_pick *pick);
void nvrtx_channel_params(uint8_t *params, const struct nvrtx_channel_setup *setup);
size_t nvrtx_push_fence(uint8_t *push, uint64_t semaphore, uint32_t value);
size_t nvrtx_push_fence_timed(uint8_t *push, uint64_t semaphore, uint32_t value);
size_t nvrtx_push_stamp(uint8_t *push, uint64_t semaphore, uint32_t value);
size_t nvrtx_push_copy_fill(uint8_t *push, uint32_t subchannel, uint32_t copy_class, uint64_t destination, uint32_t words, uint32_t value);
size_t nvrtx_push_copy_clear(uint8_t *push, uint32_t subchannel, uint64_t destination, uint32_t line_words, uint32_t lines, uint32_t value);
void nvrtx_ring_entry(uint8_t *entry, uint64_t va, uint32_t words);
void nvrtx_free_params(uint8_t *params, uint32_t client, uint32_t object);
void nvrtx_space_params_shared(uint8_t *params);
void nvrtx_copy_pdes_params(uint8_t *params, uint64_t root, uint64_t directory2, uint64_t directory1);
size_t nvrtx_gr_buffers(const uint8_t *info, size_t length, struct nvrtx_gr_buffer *buffers, size_t capacity);
void nvrtx_promote_params(uint8_t *params, uint32_t client, uint32_t channel, const struct nvrtx_promote_entry *entries, size_t count);

size_t nvrtx_mmu_gr_tables_bytes(uint64_t va_bytes);
int nvrtx_mmu_gr_tables(uint8_t *image, uint64_t block, uint64_t va_base, uint64_t va_bytes);
int nvrtx_mmu_gr_map(uint8_t *image, uint64_t va_base, uint64_t va_bytes, uint64_t va, uint64_t physical, uint64_t size, int read_only);
int nvrtx_mmu_gr_map_system(uint8_t *image, uint64_t va_base, uint64_t va_bytes, uint64_t va, const uint64_t *pages, size_t count);
int nvrtx_mmu_gr_map_vram(uint8_t *image, uint64_t va_base, uint64_t va_bytes, uint64_t va, uint64_t physical, uint64_t size);
size_t nvrtx_mmu_gr_pte_offset(uint64_t va_base, uint64_t va_bytes, uint64_t va);
void nvrtx_mmu_gr_entries_vram(uint8_t *entries, uint64_t physical, uint64_t pages);
void nvrtx_mmu_gr_entries_vram_kind(uint8_t *entries, uint64_t physical, uint64_t pages, uint32_t kind);
void nvrtx_mmu_gr_entries_privileged(uint8_t *entries, uint64_t physical, uint64_t pages, int read_only);
void nvrtx_mmu_gr_entries_system(uint8_t *entries, const uint64_t *pages, size_t count);

int nvrtx_vbios_walk(const uint8_t *rom, size_t size, struct nvrtx_vbios *vbios);
int nvrtx_vbios_fwsec(const struct nvrtx_vbios *vbios, struct nvrtx_fwsec *fwsec);
int nvrtx_fwsec_command(uint8_t *program, const struct nvrtx_fwsec *fwsec, uint32_t command, uint64_t frts, uint64_t frts_size);
uint32_t nvrtx_fwsec_fuse_register(const struct nvrtx_fwsec *fwsec);
int nvrtx_fwsec_fuse_index(const struct nvrtx_fwsec *fwsec, uint32_t fuse, unsigned *index);
int nvrtx_fwsec_sign(uint8_t *program, const struct nvrtx_fwsec *fwsec, const struct nvrtx_vbios *vbios, unsigned index);

#endif
