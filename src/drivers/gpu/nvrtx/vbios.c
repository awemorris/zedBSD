/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The VBIOS image and the FWSEC program in it.
 *
 * The GPU's ROM is a chain of images, each with a PCI data structure that
 * gives its size, type and whether it is the last.  The first image holds
 * the BIT table; the falcon programs are in a later image of type 0xE0, and
 * a pointer at or past the end of the first image is read in that image
 * instead.  The BIT table's 'p' entry points at a table of falcon programs;
 * entry type 0x85 is FWSEC, signed firmware that runs on the GSP's falcon to
 * set up the protected region (FRTS) or tear it down (SB).
 *
 * The driver copies the FWSEC program as it is and writes only the command
 * block in its data memory that tells it what to do: the command number and,
 * for FRTS, where the region goes.  Nothing here touches the hardware.
 */

#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/nvrtx/nvrtx-gsp.h"

/*
 * The signatures an image of the chain may start with.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/bios/image.c (nvbios_imagen).
 */
#define VBIOS_SIGNATURE_PC		0xaa55U
#define VBIOS_SIGNATURE_ALT		0xbb77U
#define VBIOS_SIGNATURE_NV		0x4e56U

/*
 * The PCI data structure: where its pointer is, its signatures and the fields
 * read.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/bios/pcir.c (nvbios_pcirTe,
 * nvbios_pcirTp).
 */
#define VBIOS_PCIR_POINTER_AT		0x18U
#define VBIOS_PCIR_PCIR			0x52494350U
#define VBIOS_PCIR_RGIS			0x53494752U
#define VBIOS_PCIR_NPDS			0x5344504eU
#define VBIOS_PCIR_LENGTH_AT		0x0aU
#define VBIOS_PCIR_SIZE_AT		0x10U
#define VBIOS_PCIR_TYPE_AT		0x14U
#define VBIOS_PCIR_INDICATOR_AT		0x15U
#define VBIOS_PCIR_LAST			0x80U
#define VBIOS_IMAGE_UNIT		512U

/*
 * NVIDIA's extension after the PCI data structure, which corrects the size and
 * the last flag.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/bios/npde.c (nvbios_npdeTe,
 * nvbios_npdeTp).
 */
#define VBIOS_NPDE_SIGNATURE		0x4544504eU
#define VBIOS_NPDE_SIZE_AT		0x08U
#define VBIOS_NPDE_INDICATOR_AT		0x0aU

/*
 * The image types: the PC image is checksummed; 0x70 has no extension; 0xE0
 * holds the falcon programs.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/bios/shadow.c (shadow_image),
 * nvkm/subdev/bios/image.c (nvbios_imagen) and nvkm/subdev/bios/base.c
 * (nvkm_bios_new).
 */
#define VBIOS_TYPE_PC			0x00U
#define VBIOS_TYPE_NO_EXTENSION		0x70U
#define VBIOS_TYPE_FALCON		0xe0U

/* The most images followed, so a broken chain cannot loop. */
#define VBIOS_IMAGE_MAX			16U

/*
 * The BIT table: its signature, the entry size and count fields, and the first
 * entry.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/nouveau/nvkm/subdev/bios/bit.c
 * (bit_entry) and nvkm/subdev/bios/pmu.c (nvbios_pmuTe).
 */
#define VBIOS_BIT_ENTRY_SIZE_AT		9U
#define VBIOS_BIT_ENTRY_COUNT_AT	10U
#define VBIOS_BIT_ENTRIES_AT		12U
#define VBIOS_BIT_FALCON_ID		'p'
#define VBIOS_BIT_FALCON_VERSION	2U

/*
 * The falcon program table's header fields and the FWSEC entry type.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/nouveau/nvkm/subdev/bios/pmu.c
 * (nvbios_pmuTe, nvbios_pmuEp) and nvkm/subdev/gsp/fwsec.c
 * (nvkm_gsp_fwsec_init).
 */
#define VBIOS_FALCON_HEADER_AT		1U
#define VBIOS_FALCON_ENTRY_SIZE_AT	2U
#define VBIOS_FALCON_COUNT_AT		3U
#define VBIOS_FALCON_FWSEC		0x85U
#define VBIOS_FALCON_DATA_AT		2U

/*
 * The FWSEC descriptor (version 2): the header word and the fields read.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/fwsec.c (union
 * nvfw_falcon_ucode_desc, nvkm_gsp_fwsec_init).
 */
#define VBIOS_DESC_VALID		0x1U
#define VBIOS_DESC_VERSION_SHIFT	8U
#define VBIOS_DESC_VERSION_MASK		0xffU
#define VBIOS_DESC_SIZE_SHIFT		16U
#define VBIOS_DESC_INTERFACE_AT		16U
#define VBIOS_DESC_IMEM_BASE_AT		20U
#define VBIOS_DESC_IMEM_SIZE_AT		24U
#define VBIOS_DESC_IMEM_SECURE_BASE_AT	32U
#define VBIOS_DESC_IMEM_SECURE_SIZE_AT	36U
#define VBIOS_DESC_DMEM_OFFSET_AT	40U
#define VBIOS_DESC_DMEM_BASE_AT		44U
#define VBIOS_DESC_DMEM_SIZE_AT		48U
#define VBIOS_DESC_V2_BYTES		52U

/*
 * The FWSEC descriptor of version 3 (Ampere and later): the signature's
 * place in the data, the interface table, the code's and data's places and
 * sizes, the engine mask (16 bits), the ucode number and signature count
 * (a byte each) and the fuse versions the signatures are for (16 bits).
 * The signatures follow the fixed part, inside the descriptor's size.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/fwsec.c (union
 * nvfw_falcon_ucode_desc).
 */
#define VBIOS_DESC3_SIGNATURE_AT	8U
#define VBIOS_DESC3_INTERFACE_AT	12U
#define VBIOS_DESC3_IMEM_BASE_AT	16U
#define VBIOS_DESC3_IMEM_SIZE_AT	20U
#define VBIOS_DESC3_DMEM_BASE_AT	28U
#define VBIOS_DESC3_DMEM_SIZE_AT	32U
#define VBIOS_DESC3_ENGINE_AT		36U
#define VBIOS_DESC3_UCODE_AT		38U
#define VBIOS_DESC3_COUNT_AT		39U
#define VBIOS_DESC3_VERSIONS_AT		40U
#define VBIOS_DESC3_BYTES		44U

/*
 * The block a falcon's DMA copies, which the version 3 program's parts are
 * whole numbers of.
 *
 * Value referenced: Linux v6.19 drivers/gpu/drm/nouveau/nvkm/subdev/gsp/fwsec.c
 * (nvkm_gsp_fwsec_v3) and nvkm/falcon/ga102.c (ga102_flcn_fw_load).
 */
#define VBIOS_DMA_BLOCK			0x100U

/*
 * The engine bit of FWSEC on the GSP's falcon, and its fuse bank (one word per
 * ucode number from 1).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/ga102.c (ga102_gsp_fwsec_signature)
 * and nvkm/subdev/gsp/fwsec.c (union nvfw_falcon_ucode_desc).
 */
#define VBIOS_ENGINE_GSP		0x400U
#define VBIOS_FUSE_BANK_GSP		0x8241c0U
#define VBIOS_FUSE_VERSION_BITS		16U

/*
 * The application interface table in the program's data memory, and the entry
 * that maps the command block.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/fwsec.c (union nvfw_falcon_appif_hdr,
 * union nvfw_falcon_appif, NVFW_FALCON_APPIF_ID_DMEMMAPPER,
 * nvkm_gsp_fwsec_patch).
 */
#define VBIOS_APPIF_VERSION		1U
#define VBIOS_APPIF_HEADER_AT		1U
#define VBIOS_APPIF_ENTRY_SIZE_AT	2U
#define VBIOS_APPIF_COUNT_AT		3U
#define VBIOS_APPIF_DMEM_MAPPER		4U
#define VBIOS_APPIF_BASE_AT		4U

/*
 * The command block: where the input buffer is, and the command word.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/fwsec.c (union
 * nvfw_falcon_appif_dmemmapper).
 */
#define VBIOS_MAPPER_INPUT_AT		8U
#define VBIOS_MAPPER_COMMAND_AT		44U
#define VBIOS_MAPPER_BYTES		48U

/*
 * The input buffer: the VBIOS read request, then the FRTS region.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/fwsec.c (struct nvfw_fwsec_frts_cmd,
 * NVFW_FRTS_CMD_REGION_TYPE_FB, nvkm_gsp_fwsec_patch).
 */
#define VBIOS_INPUT_BYTES		44U
#define VBIOS_READ_REQUEST_BYTES	24U
#define VBIOS_READ_REQUEST_FLAGS	2U
#define VBIOS_REGION_AT			24U
#define VBIOS_REGION_BYTES		20U
#define VBIOS_REGION_VRAM		2U

static int vbios_get8(const struct nvrtx_vbios *vbios, uint32_t address, uint32_t *value);
static int vbios_get16(const struct nvrtx_vbios *vbios, uint32_t address, uint32_t *value);
static int vbios_get32(const struct nvrtx_vbios *vbios, uint32_t address, uint32_t *value);
static int vbios_translate(const struct nvrtx_vbios *vbios, uint32_t address, uint32_t length, uint32_t *offset);
static int vbios_raw16(const uint8_t *rom, size_t size, size_t at, uint32_t *value);
static int vbios_raw32(const uint8_t *rom, size_t size, size_t at, uint32_t *value);
static int vbios_image(const uint8_t *rom, size_t size, size_t base, size_t *image_size, uint32_t *type, int *last);
static int vbios_find_bit(const struct nvrtx_vbios *vbios, uint32_t *bit);
static int vbios_fwsec_v2(const struct nvrtx_vbios *vbios, uint32_t descriptor, uint32_t header, struct nvrtx_fwsec *fwsec);
static int vbios_fwsec_v3(const struct nvrtx_vbios *vbios, uint32_t descriptor, uint32_t header, struct nvrtx_fwsec *fwsec);

/*
 * Walks the chain of images of a ROM read into memory.
 *
 * Returns 0 with the size of the first image, the place of the falcon image
 * and the end of the last image, or EINVAL when the chain is broken or the
 * PC image's checksum is wrong.  The end may be past the bytes given: the
 * caller reads that much and walks again.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/bios/shadow.c (shadow_image) and
 * nvkm/subdev/bios/image.c (nvbios_imagen).
 */
int
nvrtx_vbios_walk(
	const uint8_t *rom,
	size_t size,
	struct nvrtx_vbios *vbios)
{
	size_t base;
	size_t image_size;
	uint32_t type;
	uint32_t sum;
	size_t at;
	unsigned count;
	int last;
	int error;

	/* Starts with no image known. */
	vbios->rom = rom;
	vbios->size = size;
	vbios->image0_size = 0U;
	vbios->falcon_image = 0U;
	vbios->end = 0U;

	/* Follows the images until the one marked last. */
	base = 0U;
	for (count = 0U; count < VBIOS_IMAGE_MAX; count++) {
		/* Reads this image's header. */
		error = vbios_image(rom, size, base, &image_size, &type, &last);
		if (error != 0)
			return error;

		/* The first image's size is where the falcon pointers start to be read elsewhere. */
		if (count == 0U)
			vbios->image0_size = image_size;

		/* The first falcon image is where those pointers lead. */
		if (type == VBIOS_TYPE_FALCON && vbios->falcon_image == 0U)
			vbios->falcon_image = base;

		/* A PC image read in full must sum to zero. */
		if (type == VBIOS_TYPE_PC && base + image_size <= size) {
			sum = 0U;
			for (at = base; at < base + image_size; at++)
				sum += rom[at];
			if ((sum & 0xffU) != 0U)
				return EINVAL;
		}

		/* The chain ends after the last image. */
		base += image_size;
		if (last != 0) {
			vbios->end = base;

			/* Succeeded: the whole chain is known. */
			return 0;
		}
	}

	/* A chain without a last image within the limit is broken. */
	return EINVAL;
}

/*
 * Finds the FWSEC program through the BIT table.
 *
 * Returns 0 with the descriptor's fields and the program's place in the
 * ROM, ENOENT when the ROM has no FWSEC entry, ENOTSUP for a descriptor
 * version other than 2 (Turing) and 3 (Ampere and later), or EINVAL for
 * broken tables.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/fwsec.c (nvkm_gsp_fwsec_init) and
 * nvkm/subdev/bios/pmu.c (nvbios_pmuEp).
 */
int
nvrtx_vbios_fwsec(
	const struct nvrtx_vbios *vbios,
	struct nvrtx_fwsec *fwsec)
{
	uint32_t bit;
	uint32_t entry_size;
	uint32_t entry_count;
	uint32_t entry;
	uint32_t value;
	uint32_t table;
	uint32_t table_header;
	uint32_t table_entry_size;
	uint32_t table_count;
	uint32_t descriptor;
	uint32_t header;
	uint32_t index;
	uint32_t found;
	int error;

	/* Finds the BIT table and its falcon entry. */
	error = vbios_find_bit(vbios, &bit);
	if (error != 0)
		return error;

	error = vbios_get8(vbios, bit + VBIOS_BIT_ENTRY_SIZE_AT, &entry_size);
	if (error != 0)
		return error;

	error = vbios_get8(vbios, bit + VBIOS_BIT_ENTRY_COUNT_AT, &entry_count);
	if (error != 0)
		return error;

	/* Walks the BIT entries for the falcon table pointer. */
	table = 0U;
	entry = bit + VBIOS_BIT_ENTRIES_AT;
	for (index = 0U; index < entry_count; index++) {
		/* Reads the entry's identifier. */
		error = vbios_get8(vbios, entry, &value);
		if (error != 0)
			return error;

		/* The falcon entry of version 2 holds the table's address in its first word. */
		if (value == VBIOS_BIT_FALCON_ID) {
			error = vbios_get8(vbios, entry + 1U, &value);
			if (error != 0)
				return error;
			if (value != VBIOS_BIT_FALCON_VERSION)
				return ENOTSUP;

			error = vbios_get16(vbios, entry + 4U, &value);
			if (error != 0)
				return error;

			error = vbios_get32(vbios, value, &table);
			if (error != 0)
				return error;

			break;
		}

		/* The next entry follows by the table's entry size. */
		entry += entry_size;
	}

	/* A ROM without the falcon table has no FWSEC. */
	if (table == 0U)
		return ENOENT;

	/* Reads the falcon table's header. */
	error = vbios_get8(vbios, table + VBIOS_FALCON_HEADER_AT, &table_header);
	if (error != 0)
		return error;

	error = vbios_get8(vbios, table + VBIOS_FALCON_ENTRY_SIZE_AT, &table_entry_size);
	if (error != 0)
		return error;

	error = vbios_get8(vbios, table + VBIOS_FALCON_COUNT_AT, &table_count);
	if (error != 0)
		return error;

	/* Finds the FWSEC entry and its descriptor's address. */
	found = 0U;
	descriptor = 0U;
	for (index = 0U; index < table_count; index++) {
		/* Reads the entry's type. */
		entry = table + table_header + index * table_entry_size;
		error = vbios_get8(vbios, entry, &value);
		if (error != 0)
			return error;

		/* The FWSEC entry's data word is the descriptor's address. */
		if (value == VBIOS_FALCON_FWSEC) {
			error = vbios_get32(vbios, entry + VBIOS_FALCON_DATA_AT, &descriptor);
			if (error != 0)
				return error;

			found = 1U;
			break;
		}
	}

	/* A table without FWSEC is not a Turing-or-later VBIOS. */
	if (found == 0U)
		return ENOENT;

	/* The descriptor's header says whether it is valid, its version and its size. */
	error = vbios_get32(vbios, descriptor, &header);
	if (error != 0)
		return error;
	if ((header & VBIOS_DESC_VALID) == 0U)
		return EINVAL;

	/* Reads the descriptor of the version it has; the program follows the descriptor in the ROM. */
	fwsec->version = (header >> VBIOS_DESC_VERSION_SHIFT) & VBIOS_DESC_VERSION_MASK;
	if (fwsec->version == 2U) {
		error = vbios_fwsec_v2(vbios, descriptor, header, fwsec);
	} else if (fwsec->version == 3U) {
		error = vbios_fwsec_v3(vbios, descriptor, header, fwsec);
	} else {
		/* A version this driver does not know. */
		error = ENOTSUP;
	}

	/* Reports a descriptor that does not describe a program inside the ROM. */
	if (error != 0)
		return error;

	/* Succeeded: the FWSEC program is found. */
	return 0;
}

/*
 * Writes the command block of a copy of the FWSEC program.
 *
 * The program is copied as it is; in its data block the application
 * interface table leads to the command block, whose command word and input
 * buffer (a VBIOS read request and, for FRTS, the region in 4 KiB units) are
 * written.  Returns EINVAL when a table or block lies outside the data.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/fwsec.c (nvkm_gsp_fwsec_patch).
 */
int
nvrtx_fwsec_command(
	uint8_t *program,
	const struct nvrtx_fwsec *fwsec,
	uint32_t command,
	uint64_t frts,
	uint64_t frts_size)
{
	uint8_t *data;
	uint32_t table;
	uint32_t table_header;
	uint32_t entry_size;
	uint32_t count;
	uint32_t entry;
	uint32_t mapper;
	uint32_t input;
	uint32_t identifier;
	uint32_t index;

	/* The application interface table is at its offset in the data block. */
	data = program + fwsec->dmem_at;
	table = fwsec->interface_at;
	if (table + 4U > fwsec->dmem_size)
		return EINVAL;
	if (data[table] != VBIOS_APPIF_VERSION)
		return EINVAL;

	table_header = data[table + VBIOS_APPIF_HEADER_AT];
	entry_size = data[table + VBIOS_APPIF_ENTRY_SIZE_AT];
	count = data[table + VBIOS_APPIF_COUNT_AT];

	/* Finds the entry that maps the command block. */
	for (index = 0U; index < count; index++) {
		/* Each entry is an identifier and the block's place in the data. */
		entry = table + table_header + index * entry_size;
		if (entry + 8U > fwsec->dmem_size)
			return EINVAL;

		/* Another application's entry is passed over. */
		identifier = nvrtx_get32(data, entry);
		if (identifier != VBIOS_APPIF_DMEM_MAPPER)
			continue;

		/* The command block and its input buffer must lie inside the data. */
		mapper = nvrtx_get32(data, entry + VBIOS_APPIF_BASE_AT);
		if (mapper > fwsec->dmem_size || VBIOS_MAPPER_BYTES > fwsec->dmem_size - mapper)
			return EINVAL;

		input = nvrtx_get32(data, mapper + VBIOS_MAPPER_INPUT_AT);
		if (input > fwsec->dmem_size || VBIOS_INPUT_BYTES > fwsec->dmem_size - input)
			return EINVAL;

		/* Says which command to run. */
		nvrtx_put32(data, mapper + VBIOS_MAPPER_COMMAND_AT, command);

		/* Asks the program to read the VBIOS itself. */
		nvrtx_put32(data, input + 0U, 1U);
		nvrtx_put32(data, input + 4U, VBIOS_READ_REQUEST_BYTES);
		nvrtx_put64(data, input + 8U, 0U);
		nvrtx_put32(data, input + 16U, 0U);
		nvrtx_put32(data, input + 20U, VBIOS_READ_REQUEST_FLAGS);

		/* For FRTS, says where the region goes, in 4 KiB units of the VRAM. */
		if (command == NVRTX_FWSEC_FRTS) {
			nvrtx_put32(data, input + VBIOS_REGION_AT + 0U, 1U);
			nvrtx_put32(data, input + VBIOS_REGION_AT + 4U, VBIOS_REGION_BYTES);
			nvrtx_put32(data, input + VBIOS_REGION_AT + 8U, (uint32_t)(frts >> NVRTX_GSP_PAGE_SHIFT));
			nvrtx_put32(data, input + VBIOS_REGION_AT + 12U, (uint32_t)(frts_size >> NVRTX_GSP_PAGE_SHIFT));
			nvrtx_put32(data, input + VBIOS_REGION_AT + 16U, VBIOS_REGION_VRAM);
		}

		/* Succeeded: the program will run the command. */
		return 0;
	}

	/* A program without the command block cannot be told what to do. */
	return EINVAL;
}

/*
 * Names the fuse register that holds FWSEC's revocation version (version 3).
 *
 * Returns 0 when the descriptor is not version 3 or its engine is not the
 * GSP's falcon, the only one FWSEC is signed for.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/ga102.c (ga102_gsp_fwsec_signature).
 */
uint32_t
nvrtx_fwsec_fuse_register(
	const struct nvrtx_fwsec *fwsec)
{
	/* Only a version 3 program on the GSP's falcon is checked against a fuse. */
	if (fwsec->version != 3U)
		return 0U;
	if ((fwsec->engine_id & VBIOS_ENGINE_GSP) == 0U)
		return 0U;
	if (fwsec->ucode_id == 0U || fwsec->ucode_id > 32U)
		return 0U;

	/* Succeeded: the ucode's word of the GSP's bank. */
	return VBIOS_FUSE_BANK_GSP + (fwsec->ucode_id - 1U) * 4U;
}

/*
 * Picks which of FWSEC's signatures the boot ROM checks (version 3).
 *
 * The fuse's highest set bit is the version the GPU has revoked up to; the
 * signatures are stored for the fuse versions whose bits are set in the
 * descriptor, lowest first, and the one for that version is taken.  Returns
 * EINVAL when the ROM holds no signature for the GPU's version.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/ga102.c (ga102_gsp_fwsec_signature).
 */
int
nvrtx_fwsec_fuse_index(
	const struct nvrtx_fwsec *fwsec,
	uint32_t fuse,
	unsigned *index)
{
	uint32_t position;
	uint32_t below;
	uint32_t count;
	uint32_t bit;

	/* Finds the bit just above the fuse's highest set bit: the version to sign for. */
	position = 0U;
	while (fuse != 0U) {
		position++;
		fuse >>= 1;
	}

	/* The descriptor must hold a signature for that version. */
	if (position >= VBIOS_FUSE_VERSION_BITS)
		return EINVAL;
	if ((fwsec->signature_versions & (1U << position)) == 0U)
		return EINVAL;

	/* The signature's index counts the versions stored before it. */
	below = fwsec->signature_versions & ((1U << position) - 1U);
	count = 0U;
	for (bit = 0U; bit < VBIOS_FUSE_VERSION_BITS; bit++) {
		if (((below >> bit) & 1U) != 0U)
			count++;
	}

	/* The index must name a signature the ROM holds. */
	if (count >= fwsec->signature_count)
		return EINVAL;

	/* Succeeded: the signature's index. */
	*index = count;
	return 0;
}

/*
 * Writes one of FWSEC's signatures into a copy of the program (version 3).
 *
 * The boot ROM checks the signature at its place in the data block before
 * it starts the program.  Returns EINVAL for another version, an index the
 * ROM does not hold, or a place outside the data block.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/fwsec.c (nvkm_gsp_fwsec_v3).
 */
int
nvrtx_fwsec_sign(
	uint8_t *program,
	const struct nvrtx_fwsec *fwsec,
	const struct nvrtx_vbios *vbios,
	unsigned index)
{
	const uint8_t *signature;
	uint8_t *place;
	uint32_t at;

	/* Only version 3 carries its signatures in the ROM. */
	if (fwsec->version != 3U)
		return EINVAL;
	if (index >= fwsec->signature_count)
		return EINVAL;

	/* The signature's place must lie inside the data block the DMA copies. */
	if (fwsec->signature_at > fwsec->dmem_load_size ||
	    NVRTX_FWSEC_SIGNATURE_BYTES > fwsec->dmem_load_size - fwsec->signature_at)
		return EINVAL;

	/* Copies the chosen signature over its place. */
	signature = vbios->rom + fwsec->signatures_at + (size_t)index * NVRTX_FWSEC_SIGNATURE_BYTES;
	place = program + fwsec->dmem_at + fwsec->signature_at;
	for (at = 0U; at < NVRTX_FWSEC_SIGNATURE_BYTES; at++)
		place[at] = signature[at];

	/* Succeeded: the program carries the signature its GPU checks. */
	return 0;
}

/* Reads a byte of the ROM through the falcon-image translation. */
static int
vbios_get8(
	const struct nvrtx_vbios *vbios,
	uint32_t address,
	uint32_t *value)
{
	uint32_t offset;
	int error;

	/* Finds the byte's place. */
	error = vbios_translate(vbios, address, 1U, &offset);
	if (error != 0)
		return error;

	/* Succeeded: the byte. */
	*value = vbios->rom[offset];
	return 0;
}

/* Reads a little-endian 16-bit word of the ROM through the translation. */
static int
vbios_get16(
	const struct nvrtx_vbios *vbios,
	uint32_t address,
	uint32_t *value)
{
	uint32_t offset;
	int error;

	/* Finds the word's place. */
	error = vbios_translate(vbios, address, 2U, &offset);
	if (error != 0)
		return error;

	/* Succeeded: the word. */
	*value = (uint32_t)vbios->rom[offset] | ((uint32_t)vbios->rom[offset + 1U] << 8);
	return 0;
}

/* Reads a little-endian 32-bit word of the ROM through the translation. */
static int
vbios_get32(
	const struct nvrtx_vbios *vbios,
	uint32_t address,
	uint32_t *value)
{
	uint32_t offset;
	int error;

	/* Finds the word's place. */
	error = vbios_translate(vbios, address, 4U, &offset);
	if (error != 0)
		return error;

	/* Succeeded: the word. */
	*value = nvrtx_get32(vbios->rom, offset);
	return 0;
}

/*
 * Turns a ROM address into an offset of the bytes read.
 *
 * An address at or past the end of the first image is read in the falcon
 * image instead.  Returns EINVAL when the range is outside the bytes.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/bios/base.c (nvbios_addr).
 */
static int
vbios_translate(
	const struct nvrtx_vbios *vbios,
	uint32_t address,
	uint32_t length,
	uint32_t *offset)
{
	uint64_t at;

	/* Moves a pointer past the first image into the falcon image. */
	at = address;
	if (at >= vbios->image0_size && vbios->falcon_image != 0U)
		at = at - vbios->image0_size + vbios->falcon_image;

	/* The range must lie inside the bytes read. */
	if (at > vbios->size || length > vbios->size - at)
		return EINVAL;

	/* Succeeded: the offset. */
	*offset = (uint32_t)at;
	return 0;
}

/* Reads a 16-bit word at a raw offset, refusing one outside the bytes. */
static int
vbios_raw16(
	const uint8_t *rom,
	size_t size,
	size_t at,
	uint32_t *value)
{
	/* The word must lie inside the bytes. */
	if (at > size || 2U > size - at)
		return EINVAL;

	/* Succeeded: the word. */
	*value = (uint32_t)rom[at] | ((uint32_t)rom[at + 1U] << 8);
	return 0;
}

/* Reads a 32-bit word at a raw offset, refusing one outside the bytes. */
static int
vbios_raw32(
	const uint8_t *rom,
	size_t size,
	size_t at,
	uint32_t *value)
{
	/* The word must lie inside the bytes. */
	if (at > size || 4U > size - at)
		return EINVAL;

	/* Succeeded: the word. */
	*value = nvrtx_get32(rom, at);
	return 0;
}

/* Reads one image's header: its size, type and whether it is the last. */
static int
vbios_image(
	const uint8_t *rom,
	size_t size,
	size_t base,
	size_t *image_size,
	uint32_t *type,
	int *last)
{
	uint32_t signature;
	uint32_t pointer;
	uint32_t length;
	uint32_t value;
	size_t pcir;
	size_t npde;
	int error;

	/* The image must start with one of the known signatures. */
	error = vbios_raw16(rom, size, base, &signature);
	if (error != 0)
		return error;
	if (signature != VBIOS_SIGNATURE_PC &&
	    signature != VBIOS_SIGNATURE_ALT &&
	    signature != VBIOS_SIGNATURE_NV)
		return EINVAL;

	/* Finds the PCI data structure and checks its signature. */
	error = vbios_raw16(rom, size, base + VBIOS_PCIR_POINTER_AT, &pointer);
	if (error != 0)
		return error;

	pcir = base + pointer;
	error = vbios_raw32(rom, size, pcir, &value);
	if (error != 0)
		return error;
	if (value != VBIOS_PCIR_PCIR &&
	    value != VBIOS_PCIR_RGIS &&
	    value != VBIOS_PCIR_NPDS)
		return EINVAL;

	/* Reads the size, the type and the last flag. */
	error = vbios_raw16(rom, size, pcir + VBIOS_PCIR_SIZE_AT, &value);
	if (error != 0)
		return error;

	*image_size = (size_t)value * VBIOS_IMAGE_UNIT;
	if (pcir + VBIOS_PCIR_INDICATOR_AT >= size)
		return EINVAL;
	*type = rom[pcir + VBIOS_PCIR_TYPE_AT];
	*last = 0;
	if ((rom[pcir + VBIOS_PCIR_INDICATOR_AT] & VBIOS_PCIR_LAST) != 0U)
		*last = 1;

	/* An image of type 0x70 is always the last and has no extension. */
	if (*type == VBIOS_TYPE_NO_EXTENSION) {
		*last = 1;
		return 0;
	}

	/* The extension, when present, follows the structure on 16 bytes and corrects the size and flag. */
	error = vbios_raw16(rom, size, pcir + VBIOS_PCIR_LENGTH_AT, &length);
	if (error != 0)
		return error;

	npde = (pcir + length + 0xfU) & ~(size_t)0xfU;
	error = vbios_raw32(rom, size, npde, &value);
	if (error == 0 && value == VBIOS_NPDE_SIGNATURE) {
		error = vbios_raw16(rom, size, npde + VBIOS_NPDE_SIZE_AT, &value);
		if (error != 0)
			return error;
		if (npde + VBIOS_NPDE_INDICATOR_AT >= size)
			return EINVAL;

		*image_size = (size_t)value * VBIOS_IMAGE_UNIT;
		*last = 0;
		if ((rom[npde + VBIOS_NPDE_INDICATOR_AT] & VBIOS_PCIR_LAST) != 0U)
			*last = 1;
	}

	/* An image of no size would loop forever. */
	if (*image_size == 0U)
		return EINVAL;

	/* Succeeded: the image's header is read. */
	return 0;
}

/*
 * Finds the BIT table's signature in the first image.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/bios/base.c (nvkm_bios_new) and
 * nvkm/subdev/bios/bit.c (bit_entry).
 */
static int
vbios_find_bit(
	const struct nvrtx_vbios *vbios,
	uint32_t *bit)
{
	size_t at;
	size_t limit;

	/* Searches the first image for the five signature bytes. */
	limit = vbios->image0_size;
	if (limit > vbios->size)
		limit = vbios->size;
	for (at = 0U; at + 5U <= limit; at++) {
		if (vbios->rom[at] == 0xffU &&
		    vbios->rom[at + 1U] == 0xb8U &&
		    vbios->rom[at + 2U] == 'B' &&
		    vbios->rom[at + 3U] == 'I' &&
		    vbios->rom[at + 4U] == 'T') {
			*bit = (uint32_t)at;
			return 0;
		}
	}

	/* A ROM without the BIT table is not one this driver reads. */
	return ENOENT;
}

/*
 * Reads a version 2 (Turing) FWSEC descriptor and finds its program.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/fwsec.c (nvkm_gsp_fwsec_v2).
 */
static int
vbios_fwsec_v2(
	const struct nvrtx_vbios *vbios,
	uint32_t descriptor,
	uint32_t header,
	struct nvrtx_fwsec *fwsec)
{
	uint32_t value;
	int error;

	/* Reads the descriptor. */
	error = vbios_translate(vbios, descriptor, VBIOS_DESC_V2_BYTES, &value);
	if (error != 0)
		return error;

	fwsec->interface_at = nvrtx_get32(vbios->rom, value + VBIOS_DESC_INTERFACE_AT);
	fwsec->imem_base = nvrtx_get32(vbios->rom, value + VBIOS_DESC_IMEM_BASE_AT);
	fwsec->imem_size = nvrtx_get32(vbios->rom, value + VBIOS_DESC_IMEM_SIZE_AT);
	fwsec->imem_secure_base = nvrtx_get32(vbios->rom, value + VBIOS_DESC_IMEM_SECURE_BASE_AT);
	fwsec->imem_secure_size = nvrtx_get32(vbios->rom, value + VBIOS_DESC_IMEM_SECURE_SIZE_AT);
	fwsec->dmem_at = nvrtx_get32(vbios->rom, value + VBIOS_DESC_DMEM_OFFSET_AT);
	fwsec->dmem_base = nvrtx_get32(vbios->rom, value + VBIOS_DESC_DMEM_BASE_AT);
	fwsec->dmem_size = nvrtx_get32(vbios->rom, value + VBIOS_DESC_DMEM_SIZE_AT);
	fwsec->dmem_load_size = fwsec->dmem_size;

	/* Version 2 has no signatures for a boot ROM: the falcon bootloader loads it. */
	fwsec->signature_at = 0U;
	fwsec->engine_id = 0U;
	fwsec->ucode_id = 0U;
	fwsec->signature_count = 0U;
	fwsec->signature_versions = 0U;
	fwsec->signatures_at = 0U;

	/* The program is the code then the data, right after the descriptor. */
	fwsec->program_size = fwsec->imem_size + fwsec->dmem_size;
	error = vbios_translate(vbios, descriptor + (header >> VBIOS_DESC_SIZE_SHIFT), fwsec->program_size, &value);
	if (error != 0)
		return error;

	fwsec->program_at = value;

	/* The secure code lies inside the code, and the data block inside the program. */
	if (fwsec->imem_secure_size > fwsec->imem_size)
		return EINVAL;
	if (fwsec->dmem_at > fwsec->program_size || fwsec->dmem_size > fwsec->program_size - fwsec->dmem_at)
		return EINVAL;

	/* Succeeded: the version 2 program is found. */
	return 0;
}

/*
 * Reads a version 3 (Ampere and later) FWSEC descriptor and finds its
 * program and its signatures.
 *
 * The program is the code then the data; the falcon copies both by DMA in
 * 256-byte blocks, so the code is a whole number of blocks and the data is
 * copied to its next block boundary.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/fwsec.c (nvkm_gsp_fwsec_v3).
 */
static int
vbios_fwsec_v3(
	const struct nvrtx_vbios *vbios,
	uint32_t descriptor,
	uint32_t header,
	struct nvrtx_fwsec *fwsec)
{
	uint32_t value;
	uint32_t place;
	int error;

	/* Reads the fixed part of the descriptor. */
	error = vbios_translate(vbios, descriptor, VBIOS_DESC3_BYTES, &value);
	if (error != 0)
		return error;

	fwsec->signature_at = nvrtx_get32(vbios->rom, value + VBIOS_DESC3_SIGNATURE_AT);
	fwsec->interface_at = nvrtx_get32(vbios->rom, value + VBIOS_DESC3_INTERFACE_AT);
	fwsec->imem_base = nvrtx_get32(vbios->rom, value + VBIOS_DESC3_IMEM_BASE_AT);
	fwsec->imem_size = nvrtx_get32(vbios->rom, value + VBIOS_DESC3_IMEM_SIZE_AT);
	fwsec->dmem_base = nvrtx_get32(vbios->rom, value + VBIOS_DESC3_DMEM_BASE_AT);
	fwsec->dmem_size = nvrtx_get32(vbios->rom, value + VBIOS_DESC3_DMEM_SIZE_AT);
	fwsec->engine_id = vbios->rom[value + VBIOS_DESC3_ENGINE_AT] | ((uint32_t)vbios->rom[value + VBIOS_DESC3_ENGINE_AT + 1U] << 8);
	fwsec->ucode_id = vbios->rom[value + VBIOS_DESC3_UCODE_AT];
	fwsec->signature_count = vbios->rom[value + VBIOS_DESC3_COUNT_AT];
	fwsec->signature_versions = vbios->rom[value + VBIOS_DESC3_VERSIONS_AT] | ((uint32_t)vbios->rom[value + VBIOS_DESC3_VERSIONS_AT + 1U] << 8);

	/* The whole code is secure; the data block starts right after it. */
	fwsec->imem_secure_base = fwsec->imem_base;
	fwsec->imem_secure_size = fwsec->imem_size;
	fwsec->dmem_at = fwsec->imem_size;
	fwsec->dmem_load_size = (fwsec->dmem_size + VBIOS_DMA_BLOCK - 1U) & ~(VBIOS_DMA_BLOCK - 1U);

	/* A program with no signature cannot pass the boot ROM. */
	if (fwsec->signature_count == 0U)
		return EINVAL;

	/* The signatures follow the fixed part, one after the other. */
	error = vbios_translate(vbios,
	    descriptor + VBIOS_DESC3_BYTES,
	    fwsec->signature_count * NVRTX_FWSEC_SIGNATURE_BYTES,
	    &place);
	if (error != 0)
		return error;

	fwsec->signatures_at = place;

	/* The program is the code then the data, right after the descriptor. */
	fwsec->program_size = fwsec->imem_size + fwsec->dmem_size;
	error = vbios_translate(vbios, descriptor + (header >> VBIOS_DESC_SIZE_SHIFT), fwsec->program_size, &value);
	if (error != 0)
		return error;

	fwsec->program_at = value;

	/* The code must be whole DMA blocks, so the data's copy starts on a block. */
	if ((fwsec->imem_size % VBIOS_DMA_BLOCK) != 0U || fwsec->imem_size == 0U)
		return EINVAL;

	/* The interface table and the signature's place lie inside the data block. */
	if (fwsec->interface_at > fwsec->dmem_size)
		return EINVAL;
	if (fwsec->signature_at > fwsec->dmem_load_size ||
	    NVRTX_FWSEC_SIGNATURE_BYTES > fwsec->dmem_load_size - fwsec->signature_at)
		return EINVAL;

	/* Succeeded: the version 3 program and its signatures are found. */
	return 0;
}
