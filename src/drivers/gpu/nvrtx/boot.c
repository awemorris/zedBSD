/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Running the signed programs of the GSP boot.
 *
 * FWSEC runs on the GSP's falcon.  It is too large for the falcon's
 * memories to be written by the driver, so the small falcon bootloader is
 * written at the top of the instruction memory with a descriptor in the data
 * memory, and it copies FWSEC from system memory by DMA.  The booters run
 * on the secure processor (SEC2) and are written whole: the non-secure code,
 * the secure code, and the data, with the production signature patched in.
 *
 * From Ampere on (a profile's NVRTX_FALCON_BOOT_ROM) both are given to the
 * falcon differently: the falcon copies the code and the data by its own
 * DMA from a buffer in system memory, the signature the fuses ask for is
 * patched in by the driver, and the falcon's boot ROM is told where it is
 * before the start.
 *
 * The protected region FWSEC sets up (FRTS) is torn down by FWSEC's SB
 * command and then by booter_unload, once each, after the GSP firmware has
 * unloaded; a region that stays is reported and left for a power cycle.
 */

#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/nvrtx/nvrtx-gpu.h"

/*
 * The DMA context the falcon bootloader reads FWSEC through.
 *
 * Value referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/include/nvkm/engine/falcon.h
 * (FALCON_DMAIDX_PHYS_SYS_NCOH) and nvkm/subdev/gsp/tu102.c
 * (tu102_gsp_fwsec_load_bld).
 */
#define BOOT_DMA_CONTEXT_SYSTEM		4U

/*
 * The descriptor the falcon bootloader reads from data memory 0, 84 bytes:
 * the DMA context, the code's bus address, the non-secure and secure code
 * ranges in the program, the entry point, and the data's bus address and size.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/nouveau/include/nvfw/flcn.h
 * (struct flcn_bl_dmem_desc_v2).
 */
#define BOOT_BL_DESC_BYTES		84U
#define BOOT_BL_CONTEXT_AT		32U
#define BOOT_BL_CODE_BUS_AT		36U
#define BOOT_BL_CODE_AT			44U
#define BOOT_BL_CODE_SIZE_AT		48U
#define BOOT_BL_SECURE_AT		52U
#define BOOT_BL_SECURE_SIZE_AT		56U
#define BOOT_BL_ENTRY_AT		60U
#define BOOT_BL_DATA_BUS_AT		64U
#define BOOT_BL_DATA_SIZE_AT		72U

/*
 * The secure processor's debug-fuse register: bit 20 set on a debug part, which
 * takes the second signature.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/engine/sec2/tu102.c (tu102_sec2_flcn, the debug
 * register) and nvkm/falcon/gm200.c (gm200_flcn_fw_signature).
 */
#define BOOT_SEC2_DEBUG			0x408U
#define BOOT_SEC2_DEBUG_FUSED		(1U << 20)

/*
 * The scratch registers FWSEC reports in: FRTS in the high half of 0x1438, SB
 * in the low half of 0x1454.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/fwsec.c (nvkm_gsp_fwsec_frts,
 * nvkm_gsp_fwsec_sb).
 */
#define BOOT_REG_FRTS_RESULT		0x001438U
#define BOOT_REG_SB_RESULT		0x001454U

/*
 * The protected region's bounds (VRAM address bits 31:4 in 4 KiB units).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/fwsec.c (nvkm_gsp_fwsec_frts).
 */
#define BOOT_REG_REGION_LOW		0x1fa824U
#define BOOT_REG_REGION_HIGH		0x1fa828U

/*
 * The mailbox values booter_unload takes for a plain unload.
 *
 * Value referenced: Linux v6.19 drivers/gpu/drm/nouveau/nvkm/subdev/gsp/tu102.c
 * (tu102_gsp_fini).
 */
#define BOOT_UNLOAD_ARGUMENT		0xffU

static int boot_fwsec_rom(struct nvrtx_gpu *gpu, const struct nvrtx_buffer *program);
static int boot_booter_rom(struct nvrtx_gpu *gpu, const struct nvrtx_booter *booter, uint32_t mailbox0, uint32_t mailbox1);
static int boot_halt_result(const char *what, int error, uint32_t mailbox0, uint32_t mailbox1);

/*
 * Runs one FWSEC program (FRTS or SB) on the GSP's falcon.
 *
 * Returns 0 when the falcon halted with mailbox 0 at zero, EIO when it
 * halted with an error there, or ETIMEDOUT when it did not halt.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/tu102.c (tu102_gsp_fwsec_load_bld)
 * and nvkm/subdev/gsp/fwsec.c (nvkm_gsp_fwsec_boot).
 */
int
nvrtx_fwsec_run(
	struct nvrtx_gpu *gpu,
	const struct nvrtx_buffer *program)
{
	const struct nvrtx_falcon *falcon;
	const struct nvrtx_fwsec *fwsec;
	uint8_t descriptor[BOOT_BL_DESC_BYTES];
	uint32_t limit;
	uint32_t mailbox0;
	uint32_t mailbox1;
	size_t at;
	int error;

	falcon = &gpu->gsp_falcon;
	fwsec = &gpu->fwsec;

	/* An Ampere or later falcon loads FWSEC itself, checked by its boot ROM. */
	if (falcon->style == NVRTX_FALCON_BOOT_ROM) {
		error = boot_fwsec_rom(gpu, program);
		if (error != 0)
			return error;

		/* Succeeded: FWSEC ran its command. */
		return 0;
	}

	/* Resets the falcon and lets it load from physical memory. */
	error = nvrtx_falcon_reset(gpu, falcon);
	if (error != 0)
		return error;

	nvrtx_falcon_load_physical(gpu, falcon);

	/* Writes the falcon bootloader at the top of the instruction memory. */
	limit = nvrtx_falcon_code_limit(gpu, falcon);
	if (limit < gpu->falcon_bl.code_size)
		return EINVAL;

	error = nvrtx_falcon_imem(gpu,
	    falcon,
	    gpu->falcon_bl.code,
	    gpu->falcon_bl.code_size,
	    limit - gpu->falcon_bl.code_size,
	    gpu->falcon_bl.start_tag,
	    0);
	if (error != 0)
		return error;

	/* Builds the descriptor that tells the bootloader where FWSEC is. */
	for (at = 0U; at < sizeof(descriptor); at++)
		descriptor[at] = 0U;
	nvrtx_put32(descriptor, BOOT_BL_CONTEXT_AT, BOOT_DMA_CONTEXT_SYSTEM);
	nvrtx_put64(descriptor, BOOT_BL_CODE_BUS_AT, program->dma.device_address);
	nvrtx_put32(descriptor, BOOT_BL_CODE_AT, fwsec->imem_base);
	nvrtx_put32(descriptor, BOOT_BL_CODE_SIZE_AT, fwsec->imem_size - fwsec->imem_secure_size);
	nvrtx_put32(descriptor, BOOT_BL_SECURE_AT, fwsec->imem_secure_base);
	nvrtx_put32(descriptor, BOOT_BL_SECURE_SIZE_AT, fwsec->imem_secure_size);
	nvrtx_put32(descriptor, BOOT_BL_ENTRY_AT, 0U);
	nvrtx_put64(descriptor, BOOT_BL_DATA_BUS_AT, program->dma.device_address + fwsec->dmem_at);
	nvrtx_put32(descriptor, BOOT_BL_DATA_SIZE_AT, fwsec->dmem_size);

	/* Points the bootloader's DMA context at system memory and writes the descriptor. */
	nvrtx_falcon_dma_system(gpu, falcon, BOOT_DMA_CONTEXT_SYSTEM);
	error = nvrtx_falcon_dmem(gpu, falcon, descriptor, sizeof(descriptor), 0U);
	if (error != 0)
		return error;

	/* Starts the bootloader at its tag's address and waits for FWSEC to halt. */
	mailbox0 = 0U;
	mailbox1 = 0U;
	error = nvrtx_falcon_boot(gpu, falcon, gpu->falcon_bl.start_tag << 8, &mailbox0, &mailbox1);
	if (error != 0) {
		nvrtx_stage_mark("fwsec: no halt (mailbox %08x %08x)", (unsigned)mailbox0, (unsigned)mailbox1);
		return error;
	}

	/* A nonzero mailbox is FWSEC's error. */
	if (mailbox0 != 0U) {
		nvrtx_stage_mark("fwsec: mailbox %08x %08x", (unsigned)mailbox0, (unsigned)mailbox1);
		return EIO;
	}

	/* Succeeded: FWSEC ran its command. */
	return 0;
}

/*
 * Runs a booter on the secure processor with two mailbox arguments.
 *
 * Returns 0 when the processor halted with mailbox 0 at zero, EIO when it
 * halted with an error there, or ETIMEDOUT when it did not halt.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/tu102.c (tu102_gsp_booter_ctor,
 * tu102_gsp_booter_load) and nvkm/falcon/fw.c (nvkm_falcon_fw_ctor_hs).
 */
int
nvrtx_booter_run(
	struct nvrtx_gpu *gpu,
	const struct nvrtx_booter *booter,
	uint32_t mailbox0,
	uint32_t mailbox1)
{
	const struct nvrtx_falcon *falcon;
	uint8_t *payload;
	uint32_t debug;
	unsigned index;
	size_t signature_at;
	int error;

	falcon = &gpu->sec2_falcon;

	/* An Ampere or later secure processor loads the booter itself, checked by its boot ROM. */
	if (falcon->style == NVRTX_FALCON_BOOT_ROM) {
		error = boot_booter_rom(gpu, booter, mailbox0, mailbox1);
		if (error != 0)
			return error;

		/* Succeeded: the booter ran. */
		return 0;
	}

	/* Resets the processor. */
	error = nvrtx_falcon_reset(gpu, falcon);
	if (error != 0)
		return error;

	/* A debug-fused part takes the second signature. */
	debug = nvrtx_rd32(gpu, falcon->base + BOOT_SEC2_DEBUG);
	index = 0U;
	if ((debug & BOOT_SEC2_DEBUG_FUSED) != 0U)
		index = 1U;

	error = nvrtx_booter_signature(booter, index, &signature_at);
	if (error != 0)
		return error;

	/* Copies the payload and patches the signature into it. */
	payload = kern_malloc(booter->payload_size);
	if (payload == NULL)
		return ENOMEM;

	kern_memcpy(payload, booter->file + booter->payload_at, booter->payload_size);
	kern_memcpy(payload + booter->patch_location, booter->file + signature_at, booter->signature_size);

	/* Writes the non-secure code, the secure code after it, and the data. */
	nvrtx_falcon_load_physical(gpu, falcon);
	error = nvrtx_falcon_imem(gpu, falcon, payload, booter->os_code_size, booter->os_code_at, booter->os_code_at >> 8, 0);
	if (error == 0) {
		error = nvrtx_falcon_imem(gpu,
		    falcon,
		    payload + booter->os_code_size,
		    booter->app_size,
		    booter->app_at,
		    booter->app_at >> 8,
		    1);
	}

	if (error == 0)
		error = nvrtx_falcon_dmem(gpu, falcon, payload + booter->os_data_at, booter->os_data_size, 0U);
	kern_free(payload);
	if (error != 0)
		return error;

	/* Starts the booter and waits for it to halt. */
	error = nvrtx_falcon_boot(gpu, falcon, booter->os_code_at, &mailbox0, &mailbox1);
	if (error != 0) {
		nvrtx_stage_mark("booter: no halt (mailbox %08x %08x)", (unsigned)mailbox0, (unsigned)mailbox1);
		return error;
	}

	/* A nonzero mailbox is the booter's error. */
	if (mailbox0 != 0U) {
		nvrtx_stage_mark("booter: mailbox %08x %08x", (unsigned)mailbox0, (unsigned)mailbox1);
		return EIO;
	}

	/* Succeeded: the booter ran. */
	return 0;
}

/*
 * Tears the protected region down: FWSEC-SB, then booter_unload, once each.
 *
 * Returns 0 when the region is gone (or was never there), EIO when it
 * stays, or ETIMEDOUT when a program did not halt (its memory must then be
 * kept until the power is cut).
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/tu102.c (tu102_gsp_fini) and
 * nvkm/subdev/gsp/fwsec.c (nvkm_gsp_fwsec_sb).
 */
int
nvrtx_region_tear_down(
	struct nvrtx_gpu *gpu)
{
	uint32_t high;
	uint32_t result;
	int error;

	/* Nothing to tear down when the region is not there. */
	high = nvrtx_rd32(gpu, BOOT_REG_REGION_HIGH);
	if (high == 0U)
		return 0;

	/* Runs FWSEC's SB command and reads its result. */
	nvrtx_stage_mark("unload begin: FWSEC-SB");
	error = nvrtx_fwsec_run(gpu, &gpu->memory.fwsec_sb);
	if (error == ETIMEDOUT)
		return error;

	result = nvrtx_rd32(gpu, BOOT_REG_SB_RESULT) & 0xffffU;
	if (error != 0 || result != 0U)
		nvrtx_stage_mark("unload FWSEC-SB error %d result %04x", error, (unsigned)result);

	/* Runs booter_unload with the plain-unload arguments. */
	nvrtx_stage_mark("unload booter_unload");
	error = nvrtx_booter_run(gpu, &gpu->unload, BOOT_UNLOAD_ARGUMENT, BOOT_UNLOAD_ARGUMENT);
	if (error == ETIMEDOUT)
		return error;

	/* The region must be gone now. */
	high = nvrtx_rd32(gpu, BOOT_REG_REGION_HIGH);
	if (high != 0U) {
		nvrtx_stage_mark("unload fail: region still up to %08x; power cycle needed", (unsigned)high);
		return EIO;
	}

	/* Succeeded: the region is torn down. */
	nvrtx_stage_mark("unload ok: region torn down");
	return 0;
}

/*
 * Reads the protected region's bounds.
 */
void
nvrtx_region_bounds(
	struct nvrtx_gpu *gpu,
	uint32_t *low,
	uint32_t *high)
{
	/* Reads the two bound registers. */
	*low = nvrtx_rd32(gpu, BOOT_REG_REGION_LOW);
	*high = nvrtx_rd32(gpu, BOOT_REG_REGION_HIGH);
}

/*
 * Reads FWSEC-FRTS's result from its scratch register: zero is success.
 */
uint32_t
nvrtx_frts_result(
	struct nvrtx_gpu *gpu)
{
	uint32_t value;

	/* The result is the register's high half. */
	value = nvrtx_rd32(gpu, BOOT_REG_FRTS_RESULT);

	/* Succeeded: the result code. */
	return value >> 16;
}

/*
 * Runs one FWSEC program (FRTS or SB) on an Ampere or later GSP falcon.
 *
 * The program's copy already carries its command and the signature the
 * fuses ask for (layout).  The falcon is reset, copies the code (all secure)
 * and the data by DMA, and its boot ROM checks the signature and starts it.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/falcon/ga102.c (ga102_flcn_fw_load,
 * ga102_flcn_fw_boot).
 */
static int
boot_fwsec_rom(
	struct nvrtx_gpu *gpu,
	const struct nvrtx_buffer *program)
{
	const struct nvrtx_falcon *falcon;
	const struct nvrtx_fwsec *fwsec;
	uint32_t mailbox0;
	uint32_t mailbox1;
	int error;

	falcon = &gpu->gsp_falcon;
	fwsec = &gpu->fwsec;

	/* Resets the falcon. */
	error = nvrtx_falcon_reset(gpu, falcon);
	if (error != 0)
		return error;

	/* Copies the code, then the data in whole blocks. */
	error = nvrtx_falcon_dma_load(gpu, falcon, program->dma.device_address, 0U, fwsec->imem_size, fwsec->imem_base, 1, 1);
	if (error != 0)
		return error;

	error = nvrtx_falcon_dma_load(gpu,
	    falcon,
	    program->dma.device_address,
	    fwsec->dmem_at,
	    fwsec->dmem_load_size,
	    fwsec->dmem_base,
	    0,
	    0);
	if (error != 0)
		return error;

	/* Tells the boot ROM where the signature is and which ucode it checks, then starts at 0. */
	nvrtx_falcon_boot_rom(gpu, falcon, fwsec->signature_at, fwsec->engine_id, fwsec->ucode_id);
	mailbox0 = 0U;
	mailbox1 = 0U;
	error = nvrtx_falcon_boot(gpu, falcon, 0U, &mailbox0, &mailbox1);

	/* The halt and mailbox 0 decide the outcome. */
	error = boot_halt_result("fwsec", error, mailbox0, mailbox1);
	if (error != 0)
		return error;

	/* Succeeded: FWSEC ran its command. */
	return 0;
}

/*
 * Runs a booter on an Ampere or later secure processor.
 *
 * The payload is copied into DMA memory with the signature the fuses ask
 * for patched in; the processor is reset, copies the application (all
 * secure) and the data by DMA, and its boot ROM checks the signature and
 * starts the application.  The copy is kept when the processor does not
 * halt, as it may still be reading it.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/ga102.c (ga102_gsp_booter_ctor)
 * and nvkm/falcon/ga102.c (ga102_flcn_fw_load, ga102_flcn_fw_boot).
 */
static int
boot_booter_rom(
	struct nvrtx_gpu *gpu,
	const struct nvrtx_booter *booter,
	uint32_t mailbox0,
	uint32_t mailbox1)
{
	const struct nvrtx_falcon *falcon;
	struct nvrtx_buffer payload;
	uint32_t fuse_register;
	uint32_t fuse;
	unsigned index;
	size_t signature_at;
	uint8_t *bytes;
	int error;

	falcon = &gpu->sec2_falcon;

	/* A booter without its meta data cannot be described to the boot ROM. */
	if (booter->has_meta == 0)
		return EINVAL;

	/* The boot ROM finds the signature in the data, so it must lie there. */
	if (booter->patch_location < booter->os_data_at)
		return EINVAL;

	/* Reads the fuse the booter's signature is chosen by (none: the last one). */
	fuse = 0U;
	fuse_register = nvrtx_booter_fuse_register(booter->engine_id, booter->ucode_id);
	if (fuse_register != 0U)
		fuse = nvrtx_rd32(gpu, fuse_register);

	error = nvrtx_booter_fuse_index(booter, fuse, &index);
	if (error != 0) {
		nvrtx_stage_mark("booter: no signature for fuse %08x (version %u)", (unsigned)fuse, (unsigned)booter->fuse_version);
		return error;
	}

	error = nvrtx_booter_signature(booter, index, &signature_at);
	if (error != 0)
		return error;

	/* Copies the payload into DMA memory and patches the signature into it. */
	payload.allocated = 0;
	error = nvrtx_buffer_alloc(gpu, booter->payload_size, &payload);
	if (error != 0)
		return error;

	bytes = (uint8_t *)payload.dma.address;
	kern_memcpy(bytes, booter->file + booter->payload_at, booter->payload_size);
	kern_memcpy(bytes + booter->patch_location, booter->file + signature_at, booter->signature_size);

	/* Resets the processor. */
	error = nvrtx_falcon_reset(gpu, falcon);
	if (error != 0) {
		nvrtx_buffer_free(gpu, &payload);
		return error;
	}

	/* Copies the application into the code at 0, then the data at 0. */
	error = nvrtx_falcon_dma_load(gpu, falcon, payload.dma.device_address, booter->app_at, booter->app_size, 0U, 1, 1);
	if (error == 0) {
		error = nvrtx_falcon_dma_load(gpu,
		    falcon,
		    payload.dma.device_address,
		    booter->os_data_at,
		    booter->os_data_size,
		    0U,
		    0,
		    0);
	}

	if (error != 0) {
		nvrtx_buffer_free(gpu, &payload);
		return error;
	}

	/* Tells the boot ROM where the signature is in the data and which ucode it checks, then starts the application. */
	nvrtx_falcon_boot_rom(gpu, falcon, booter->patch_location - booter->os_data_at, booter->engine_id, booter->ucode_id);
	error = nvrtx_falcon_boot(gpu, falcon, booter->app_at, &mailbox0, &mailbox1);

	/* A processor that did not halt may still read the copy, which is then kept. */
	if (error != ETIMEDOUT)
		nvrtx_buffer_free(gpu, &payload);

	/* The halt and mailbox 0 decide the outcome. */
	error = boot_halt_result("booter", error, mailbox0, mailbox1);
	if (error != 0)
		return error;

	/* Succeeded: the booter ran. */
	return 0;
}

/*
 * Turns a falcon's halt into the result: the boot's error when it did not
 * halt, EIO for a nonzero mailbox 0, both shown with the mailboxes.
 */
static int
boot_halt_result(
	const char *what,
	int error,
	uint32_t mailbox0,
	uint32_t mailbox1)
{
	/* A falcon that did not halt. */
	if (error != 0) {
		nvrtx_stage_mark("%s: no halt (mailbox %08x %08x)", what, (unsigned)mailbox0, (unsigned)mailbox1);
		return error;
	}

	/* A nonzero mailbox is the program's error. */
	if (mailbox0 != 0U) {
		nvrtx_stage_mark("%s: mailbox %08x %08x", what, (unsigned)mailbox0, (unsigned)mailbox1);
		return EIO;
	}

	/* Succeeded: the program ran to its halt with no error. */
	return 0;
}
