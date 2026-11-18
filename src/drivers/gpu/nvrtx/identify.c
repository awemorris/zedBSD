/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Chip identity of the nvrtx driver.
 *
 * The PCI device ID only names a product; the chip is named by the identity
 * register at the start of the register window.  This file turns the two
 * identity registers into a chip record, and points the chip at its family
 * record: what its generation needs from the GSP boot path (the firmware
 * files, the falcons' loading, the registers that moved, the heap) and the
 * class numbers and formats the later stages use.  It touches no hardware,
 * so the host test links it alone.
 */

#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/nvrtx/nvrtx-private.h"

/*
 * The architecture numbers of the identity register this driver knows.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/engine/device/base.c (nvkm_device_ctor).
 */
#define IDENTIFY_ARCH_TURING		0x16U
#define IDENTIFY_ARCH_AMPERE		0x17U
#define IDENTIFY_ARCH_HOPPER		0x18U
#define IDENTIFY_ARCH_ADA		0x19U
#define IDENTIFY_ARCH_BLACKWELL_DC	0x1aU
#define IDENTIFY_ARCH_BLACKWELL		0x1bU

/*
 * The GSP heap's part for its operating system and its least size (libos 2 on
 * Turing, libos 3 after).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r570/nvrm/gsp.h
 * (GSP_FW_HEAP_PARAM_OS_SIZE_LIBOS2,
 * GSP_FW_HEAP_PARAM_OS_SIZE_LIBOS3_BAREMETAL,
 * GSP_FW_HEAP_SIZE_OVERRIDE_LIBOS2_MIN_MB,
 * GSP_FW_HEAP_SIZE_OVERRIDE_LIBOS3_BAREMETAL_MIN_MB) and
 * nvkm/subdev/gsp/rm/r570/rm.c (r570_wpr_libos2, r570_wpr_libos3).
 */
#define IDENTIFY_MIB			0x100000ULL
#define IDENTIFY_HEAP_OS_TURING		0ULL
#define IDENTIFY_HEAP_MIN_TURING	(64ULL * IDENTIFY_MIB)
#define IDENTIFY_HEAP_OS_AMPERE		(22ULL * IDENTIFY_MIB)
#define IDENTIFY_HEAP_MIN_AMPERE	(170ULL * IDENTIFY_MIB)

/*
 * One known chip: its number in the identity register, its name, and the
 * family record of the GSP boot path, if it has one.
 */
struct identify_entry {
	uint32_t chipset;
	const char *name;
	const struct nvrtx_profile *profile;
};

/*
 * TU102, TU104 and TU106 (GeForce RTX 20): the firmware below tu102, the
 * falcon bootloader for FWSEC, and the graphics scrubber channel.  The
 * family the driver was brought up and seen working on (RTX 2070).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/tu102.c (the family's firmware
 * and boot path), and NVIDIA open-gpu-doc (the class numbers).
 */
static const struct nvrtx_profile identify_tu10x = {
	"TU10x",
	1,
	"tu102/gsp/gsp-570.144.bin",
	"tu102/gsp/bootloader-570.144.bin",
	"tu102/gsp/booter_load-570.144.bin",
	"tu102/gsp/booter_unload-570.144.bin",
	"tu102/acr/bl.bin",
	".fwsignature_tu10x",
	NVRTX_FALCON_PORTS,
	0x100ce0U,
	0,
	0x021c04U,
	IDENTIFY_HEAP_OS_TURING,
	IDENTIFY_HEAP_MIN_TURING,
	1,
	0xc46fU,
	0xc5b5U,
	0xc597U,
	0xc5c0U,
	0xc570U,
	0xc57dU,
	0xc57eU,
	0xc57bU,
	0xc57aU,
	0x00000010U,
	2U,
	2U,
	75U,
	32U,
	{ 32U, 64U, 0U, 0U, 0U, 0U },
	2U
};

/*
 * TU116 and TU117 (GeForce GTX 16): Turing as TU10x, with booters of their
 * own below tu116 and their own signature in the same GSP image, and no
 * scrubber channel.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/tu116.c (the family's firmware
 * and boot path), and NVIDIA open-gpu-doc (the class numbers).
 */
static const struct nvrtx_profile identify_tu11x = {
	"TU11x",
	0,
	"tu102/gsp/gsp-570.144.bin",
	"tu102/gsp/bootloader-570.144.bin",
	"tu116/gsp/booter_load-570.144.bin",
	"tu116/gsp/booter_unload-570.144.bin",
	"tu102/acr/bl.bin",
	".fwsignature_tu11x",
	NVRTX_FALCON_PORTS,
	0x100ce0U,
	0,
	0x021c04U,
	IDENTIFY_HEAP_OS_TURING,
	IDENTIFY_HEAP_MIN_TURING,
	0,
	0xc46fU,
	0xc5b5U,
	0xc597U,
	0xc5c0U,
	0xc570U,
	0xc57dU,
	0xc57eU,
	0xc57bU,
	0xc57aU,
	0x00000010U,
	2U,
	2U,
	75U,
	32U,
	{ 32U, 64U, 0U, 0U, 0U, 0U },
	2U
};

/*
 * GA102 to GA107 (GeForce RTX 30): the firmware below ga102, falcons that
 * load by DMA and check signatures in their boot ROM, the VRAM size in
 * MiB, the larger heap of the third operating-system version, Ampere's
 * classes and QMD 3.0.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/ga102.c (the family's firmware
 * and boot path), and NVIDIA open-gpu-doc (the class numbers and the QMD
 * version).
 */
static const struct nvrtx_profile identify_ga10x = {
	"GA10x",
	0,
	"ga102/gsp/gsp-570.144.bin",
	"ga102/gsp/bootloader-570.144.bin",
	"ga102/gsp/booter_load-570.144.bin",
	"ga102/gsp/booter_unload-570.144.bin",
	NULL,
	".fwsignature_ga10x",
	NVRTX_FALCON_BOOT_ROM,
	0x1183a4U,
	1,
	0x820c04U,
	IDENTIFY_HEAP_OS_AMPERE,
	IDENTIFY_HEAP_MIN_AMPERE,
	0,
	0xc56fU,
	0xc7b5U,
	0xc797U,
	0xc7c0U,
	0xc670U,
	0xc67dU,
	0xc67eU,
	0xc67bU,
	0xc67aU,
	0x00000000U,
	3U,
	0U,
	86U,
	48U,
	{ 0U, 8U, 16U, 32U, 64U, 100U },
	6U
};

/*
 * AD102 to AD107 (GeForce RTX 40): Ampere's boot path with the bootloader
 * and booters below ad102 and the GSP image shared with GA10x (its Ada
 * signature), Ada's 3D, compute and display-core classes.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/ad102.c (the family's firmware
 * and boot path), and NVIDIA open-gpu-doc (the class numbers and the QMD
 * version).
 */
static const struct nvrtx_profile identify_ad10x = {
	"AD10x",
	0,
	"ga102/gsp/gsp-570.144.bin",
	"ad102/gsp/bootloader-570.144.bin",
	"ad102/gsp/booter_load-570.144.bin",
	"ad102/gsp/booter_unload-570.144.bin",
	NULL,
	".fwsignature_ad10x",
	NVRTX_FALCON_BOOT_ROM,
	0x1183a4U,
	1,
	0x820c04U,
	IDENTIFY_HEAP_OS_AMPERE,
	IDENTIFY_HEAP_MIN_AMPERE,
	0,
	0xc56fU,
	0xc7b5U,
	0xc997U,
	0xc9c0U,
	0xc770U,
	0xc77dU,
	0xc67eU,
	0xc67bU,
	0xc67aU,
	0x00000000U,
	3U,
	0U,
	89U,
	48U,
	{ 0U, 8U, 16U, 32U, 64U, 100U },
	6U
};

/*
 * The chips of Turing and later, by identity number.
 *
 * Every Turing, Ampere and Ada chip of the GeForce RTX 20, 30 and 40 (and
 * GTX 16) has a family record.  GA100 and GH100 (data-centre parts without a
 * display) and Blackwell are recognized and named only, and the stages after
 * console leave them alone.
 */
static const struct identify_entry identify_table[] = {
	{ 0x162U, "TU102", &identify_tu10x },
	{ 0x164U, "TU104", &identify_tu10x },
	{ 0x166U, "TU106", &identify_tu10x },
	{ 0x167U, "TU117", &identify_tu11x },
	{ 0x168U, "TU116", &identify_tu11x },
	{ 0x170U, "GA100", NULL },
	{ 0x172U, "GA102", &identify_ga10x },
	{ 0x173U, "GA103", &identify_ga10x },
	{ 0x174U, "GA104", &identify_ga10x },
	{ 0x176U, "GA106", &identify_ga10x },
	{ 0x177U, "GA107", &identify_ga10x },
	{ 0x180U, "GH100", NULL },
	{ 0x192U, "AD102", &identify_ad10x },
	{ 0x193U, "AD103", &identify_ad10x },
	{ 0x194U, "AD104", &identify_ad10x },
	{ 0x196U, "AD106", &identify_ad10x },
	{ 0x197U, "AD107", &identify_ad10x },
	{ 0x1b2U, "GB202", NULL },
	{ 0x1b3U, "GB203", NULL },
	{ 0x1b5U, "GB205", NULL },
	{ 0x1b6U, "GB206", NULL },
	{ 0x1b7U, "GB207", NULL }
};

static enum nvrtx_family identify_family(uint32_t architecture);

/*
 * Decodes the chip identity from the two identity registers.
 *
 * Returns 0 for a Turing or later GPU the identity names, ENODEV for a GPU
 * the driver never drives (older generations, a virtual function, a
 * big-endian or unreadable identity), and ENOTSUP for a newer chip number
 * it does not know.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/engine/device/base.c (nvkm_device_ctor).
 */
int
nvrtx_identify(
	uint32_t ident0,
	uint32_t ident1,
	struct nvrtx_chip *chip)
{
	uint32_t architecture;
	size_t index;

	/* Starts from an unknown chip, so a refusal leaves nothing half filled. */
	chip->chipset = 0U;
	chip->revision = 0U;
	chip->family = NVRTX_FAMILY_UNKNOWN;
	chip->name = "unknown";
	chip->profile = NULL;

	/* A window that reads all ones has no GPU behind it, or the GPU fell off the bus. */
	if (ident0 == 0xffffffffU)
		return ENODEV;

	/*
	 * A GPU switched to big-endian would need a write to switch back, and
	 * identify never writes; anything else in the second register but the
	 * little-endian zero and the virtual-function bits is not trusted
	 * either.
	 */
	if (ident1 == NVRTX_IDENT_1_BIG_ENDIAN)
		return ENODEV;
	if ((ident1 & ~NVRTX_IDENT_1_VIRTUAL_MASK) != 0U)
		return ENODEV;

	/* Reads the chip number and the revision. */
	chip->chipset = (ident0 >> NVRTX_IDENT_CHIP_SHIFT) & NVRTX_IDENT_CHIP_MASK;
	chip->revision = ident0 & NVRTX_IDENT_REVISION_MASK;

	/* Refuses the generations before Turing, which this driver never covers. */
	architecture = (ident0 >> NVRTX_IDENT_ARCH_SHIFT) & NVRTX_IDENT_ARCH_MASK;
	chip->family = identify_family(architecture);
	if (chip->family == NVRTX_FAMILY_UNKNOWN) {
		/* A number above the newest known architecture is newer, not older. */
		if (architecture > IDENTIFY_ARCH_BLACKWELL)
			return ENOTSUP;

		return ENODEV;
	}

	/* A virtual function of a partitioned GPU is not a whole GPU to drive. */
	if ((ident1 & NVRTX_IDENT_1_VIRTUAL_MASK) != 0U)
		return ENODEV;

	/* Names the chip from the table of known chips. */
	for (index = 0U; index < sizeof(identify_table) / sizeof(identify_table[0]); index++) {
		if (identify_table[index].chipset == chip->chipset) {
			chip->name = identify_table[index].name;
			chip->profile = identify_table[index].profile;

			/* Succeeded: the chip is known. */
			return 0;
		}
	}

	/* A chip of a known generation that the table does not list yet. */
	return ENOTSUP;
}

/*
 * Names a GPU generation for the stage marks.
 */
const char *
nvrtx_family_name(
	enum nvrtx_family family)
{
	/* Names each generation the identity register can report. */
	switch (family) {
	case NVRTX_FAMILY_TURING:
		return "Turing";
	case NVRTX_FAMILY_AMPERE:
		return "Ampere";
	case NVRTX_FAMILY_HOPPER:
		return "Hopper";
	case NVRTX_FAMILY_ADA:
		return "Ada";
	case NVRTX_FAMILY_BLACKWELL:
		return "Blackwell";
	case NVRTX_FAMILY_UNKNOWN:
		break;
	}

	/* An unknown generation has no name. */
	return "unknown";
}

/*
 * Decides whether the attach boots the GSP on a GPU or leaves it to the firmware.
 *
 * nvrtx.gsp=0 leaves every GPU to the firmware, and a chip without a family
 * record has no GSP path.  Otherwise nvrtx.gsp=1 boots any chip with a
 * path; without it the GSP is booted on every family, seen working on real
 * hardware or not, but only where the firmware's picture is on the GPU (a
 * GPU the firmware showed nothing on has nothing the default boot would
 * keep).
 */
enum nvrtx_boot_choice
nvrtx_boot_choose(
	const struct nvrtx_profile *profile,
	int gop_on_gpu,
	bool gsp_wanted,
	bool gsp_forced)
{
	/* The boot keeps every GPU away from the GSP path. */
	if (!gsp_wanted)
		return NVRTX_BOOT_LEAVE_OFF;

	/* A recognized chip whose generation has no family record. */
	if (profile == NULL)
		return NVRTX_BOOT_LEAVE_NO_PATH;

	/* A forced boot starts any family, seen on hardware or not. */
	if (gsp_forced) {
		if (profile->verified == 0)
			return NVRTX_BOOT_START_UNSEEN;

		return NVRTX_BOOT_START;
	}

	/* By default only a GPU that shows the firmware's picture is booted. */
	if (gop_on_gpu == 0)
		return NVRTX_BOOT_LEAVE_NO_PICTURE;

	/* A family not yet seen on hardware is booted too, and says so. */
	if (profile->verified == 0)
		return NVRTX_BOOT_START_UNSEEN;

	/* Boots a seen family that shows the firmware's picture. */
	return NVRTX_BOOT_START;
}

/*
 * Finds the reset bit of one engine in the device information table.
 *
 * The table is 64 words.  An engine's description is a run of words: a
 * data word, an enumeration word (which may carry the reset bit), and an
 * engine-type word; bit 31 set means more words of the same engine follow.
 * Returns 0 with the bit number, or ENOENT when the engine is not listed
 * with a reset bit.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/top/gk104.c (gk104_top_parse).
 */
int
nvrtx_device_info_reset(
	const uint32_t *words,
	uint32_t engine_type,
	uint32_t *reset_bit)
{
	uint32_t index;
	uint32_t word;
	uint32_t type;
	int32_t reset;

	/* Walks the words, collecting one engine's fields until its last word. */
	type = 0xffffffffU;
	reset = -1;
	for (index = 0U; index < NVRTX_DEVICE_INFO_WORDS; index++) {
		word = words[index];

		/* An invalid word carries nothing and does not end an engine. */
		if ((word & NVRTX_DEVICE_INFO_KIND_MASK) == NVRTX_DEVICE_INFO_INVALID)
			continue;

		/* An enumeration word may carry the reset bit. */
		if ((word & NVRTX_DEVICE_INFO_KIND_MASK) == NVRTX_DEVICE_INFO_ENUM &&
		    (word & NVRTX_DEVICE_INFO_HAS_RESET) != 0U)
			reset = (int32_t)((word >> NVRTX_DEVICE_INFO_RESET_SHIFT) & NVRTX_DEVICE_INFO_RESET_MASK);

		/* An engine-type word names the engine. */
		if ((word & NVRTX_DEVICE_INFO_KIND_MASK) == NVRTX_DEVICE_INFO_TYPE)
			type = (word >> NVRTX_DEVICE_INFO_TYPE_SHIFT) & NVRTX_DEVICE_INFO_TYPE_MASK;

		/* More words of the same engine follow. */
		if ((word & NVRTX_DEVICE_INFO_CHAINED) != 0U)
			continue;

		/* The engine is complete: report it when it is the one asked for. */
		if (type == engine_type && reset >= 0) {
			*reset_bit = (uint32_t)reset;
			return 0;
		}

		/* Starts the next engine. */
		type = 0xffffffffU;
		reset = -1;
	}

	/* The engine is not listed with a reset bit. */
	return ENOENT;
}

/* Maps the architecture field of the identity register to a generation. */
static enum nvrtx_family
identify_family(
	uint32_t architecture)
{
	/* Each architecture number belongs to one generation. */
	switch (architecture) {
	case IDENTIFY_ARCH_TURING:
		return NVRTX_FAMILY_TURING;
	case IDENTIFY_ARCH_AMPERE:
		return NVRTX_FAMILY_AMPERE;
	case IDENTIFY_ARCH_HOPPER:
		return NVRTX_FAMILY_HOPPER;
	case IDENTIFY_ARCH_ADA:
		return NVRTX_FAMILY_ADA;
	case IDENTIFY_ARCH_BLACKWELL_DC:
		return NVRTX_FAMILY_BLACKWELL;
	case IDENTIFY_ARCH_BLACKWELL:
		return NVRTX_FAMILY_BLACKWELL;
	default:
		break;
	}

	/* Older generations and unknown numbers. */
	return NVRTX_FAMILY_UNKNOWN;
}
