/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * AHCI register layout, command construction and IDENTIFY decoding.
 *
 * Everything here is arithmetic on memory the caller owns, so the host tests
 * build the same FIS, command header and PRDT that the driver hands the HBA.
 */

#ifndef KERN_DRIVERS_PCI_AHCI_PROTOCOL_H
#define KERN_DRIVERS_PCI_AHCI_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

/* PCI class 01.06.01: mass storage, SATA, AHCI 1.0 programming interface. */
#define DRV_AHCI_PCI_CLASS		0x010601U

/* The AHCI base address register (ABAR) is BAR5. */
#define DRV_AHCI_ABAR_INDEX		5U

/* Generic host control registers. */
#define DRV_AHCI_REG_CAP		0x00U
#define DRV_AHCI_REG_GHC		0x04U
#define DRV_AHCI_REG_IS			0x08U
#define DRV_AHCI_REG_PI			0x0cU
#define DRV_AHCI_REG_VS			0x10U
#define DRV_AHCI_REG_CCC_CTL		0x14U
#define DRV_AHCI_REG_CAP2		0x24U
#define DRV_AHCI_REG_BOHC		0x28U

/*
 * Intel RST: a vendor-specific capability register and the remapped-device
 * table that tells whether NVMe drives were hidden behind the ABAR.
 */
#define DRV_AHCI_REG_INTEL_VSCAP	0xa4U
#define DRV_AHCI_REG_INTEL_REMAP_CAP	0x800U
#define DRV_AHCI_REG_INTEL_REMAP_DCC	0x880U
#define DRV_AHCI_INTEL_REMAP_MAX	3U
#define DRV_AHCI_INTEL_REMAP_END	0xa00U

#define DRV_AHCI_CAP_NP_MASK		0x0000001fU
#define DRV_AHCI_CAP_NCS_SHIFT		8U
#define DRV_AHCI_CAP_NCS_MASK		0x00001f00U
#define DRV_AHCI_CAP_CCCS		0x00000080U
#define DRV_AHCI_CAP_FBSS		0x00010000U
#define DRV_AHCI_CAP_SSS		0x08000000U
#define DRV_AHCI_CAP_S64A		0x80000000U
#define DRV_AHCI_CAP_CPD		0x00080000U

#define DRV_AHCI_GHC_HR			0x00000001U
#define DRV_AHCI_GHC_IE			0x00000002U
#define DRV_AHCI_GHC_AE			0x80000000U

#define DRV_AHCI_CAP2_BOH		0x00000001U
#define DRV_AHCI_CAP2_SDS		0x00000008U

#define DRV_AHCI_CCC_CTL_EN		0x00000001U

#define DRV_AHCI_BOHC_BOS		0x00000001U
#define DRV_AHCI_BOHC_OOS		0x00000002U
#define DRV_AHCI_BOHC_BB		0x00000010U

/* Port registers start at 0x100 and take 0x80 bytes per port. */
#define DRV_AHCI_PORT_BASE		0x100U
#define DRV_AHCI_PORT_STRIDE		0x80U
#define DRV_AHCI_PORT_MAX		32U

#define DRV_AHCI_PX_CLB			0x00U
#define DRV_AHCI_PX_CLBU		0x04U
#define DRV_AHCI_PX_FB			0x08U
#define DRV_AHCI_PX_FBU			0x0cU
#define DRV_AHCI_PX_IS			0x10U
#define DRV_AHCI_PX_IE			0x14U
#define DRV_AHCI_PX_CMD			0x18U
#define DRV_AHCI_PX_TFD			0x20U
#define DRV_AHCI_PX_SIG			0x24U
#define DRV_AHCI_PX_SSTS		0x28U
#define DRV_AHCI_PX_SCTL		0x2cU
#define DRV_AHCI_PX_SERR		0x30U
#define DRV_AHCI_PX_SACT		0x34U
#define DRV_AHCI_PX_CI			0x38U
#define DRV_AHCI_PX_FBS			0x40U
#define DRV_AHCI_PX_DEVSLP		0x44U

#define DRV_AHCI_PX_FBS_EN		0x00000001U
#define DRV_AHCI_PX_DEVSLP_ADSE		0x00000001U

#define DRV_AHCI_PX_CMD_ST		0x00000001U
#define DRV_AHCI_PX_CMD_SUD		0x00000002U
#define DRV_AHCI_PX_CMD_POD		0x00000004U
#define DRV_AHCI_PX_CMD_FRE		0x00000010U
#define DRV_AHCI_PX_CMD_FR		0x00004000U
#define DRV_AHCI_PX_CMD_CR		0x00008000U
#define DRV_AHCI_PX_CMD_ALPE		0x04000000U
#define DRV_AHCI_PX_CMD_ASP		0x08000000U
#define DRV_AHCI_PX_CMD_ICC_MASK	0xf0000000U
#define DRV_AHCI_PX_CMD_ICC_ACTIVE	0x10000000U

/* Interrupt status bits: completions and the conditions that stop a port. */
#define DRV_AHCI_PX_IS_DHRS		0x00000001U
#define DRV_AHCI_PX_IS_PSS		0x00000002U
#define DRV_AHCI_PX_IS_DSS		0x00000004U
#define DRV_AHCI_PX_IS_SDBS		0x00000008U
#define DRV_AHCI_PX_IS_UFS		0x00000010U
#define DRV_AHCI_PX_IS_DPS		0x00000020U
#define DRV_AHCI_PX_IS_PCS		0x00000040U
#define DRV_AHCI_PX_IS_PRCS		0x00400000U
#define DRV_AHCI_PX_IS_IPMS		0x00800000U
#define DRV_AHCI_PX_IS_OFS		0x01000000U
#define DRV_AHCI_PX_IS_INFS		0x04000000U
#define DRV_AHCI_PX_IS_IFS		0x08000000U
#define DRV_AHCI_PX_IS_HBDS		0x10000000U
#define DRV_AHCI_PX_IS_HBFS		0x20000000U
#define DRV_AHCI_PX_IS_TFES		0x40000000U

/* The completions the driver waits for. */
#define DRV_AHCI_PX_IS_COMPLETION \
	(DRV_AHCI_PX_IS_DHRS | DRV_AHCI_PX_IS_PSS | DRV_AHCI_PX_IS_DSS | \
	 DRV_AHCI_PX_IS_SDBS)

/*
 * Conditions after which the link or the HBA must be brought back.  A PhyRdy
 * change seen while a command runs means the link dropped under it.
 */
#define DRV_AHCI_PX_IS_LINK_FAULT \
	(DRV_AHCI_PX_IS_HBFS | DRV_AHCI_PX_IS_HBDS | DRV_AHCI_PX_IS_IFS | \
	 DRV_AHCI_PX_IS_OFS | DRV_AHCI_PX_IS_PRCS)

/*
 * Every interrupt source the driver enables on a running port.  PCS and
 * PRCS are left out: they are held by PxSERR rather than PxIS, and without
 * hotplug handling they would only repeat.
 */
#define DRV_AHCI_PX_IE_DEFAULT \
	(DRV_AHCI_PX_IS_DHRS | DRV_AHCI_PX_IS_PSS | DRV_AHCI_PX_IS_DSS | \
	 DRV_AHCI_PX_IS_SDBS | DRV_AHCI_PX_IS_UFS | DRV_AHCI_PX_IS_DPS | \
	 DRV_AHCI_PX_IS_OFS | DRV_AHCI_PX_IS_INFS | DRV_AHCI_PX_IS_IFS | \
	 DRV_AHCI_PX_IS_HBDS | DRV_AHCI_PX_IS_HBFS | DRV_AHCI_PX_IS_TFES)

#define DRV_AHCI_PX_TFD_ERR		0x00000001U
#define DRV_AHCI_PX_TFD_DRQ		0x00000008U
#define DRV_AHCI_PX_TFD_BSY		0x00000080U

#define DRV_AHCI_PX_SSTS_DET_MASK	0x0000000fU
#define DRV_AHCI_PX_SSTS_DET_NONE	0x0U
#define DRV_AHCI_PX_SSTS_DET_PHY	0x3U

#define DRV_AHCI_PX_SSTS_DET_PRESENT	0x1U

#define DRV_AHCI_PX_SCTL_DET_MASK	0x0000000fU
#define DRV_AHCI_PX_SCTL_DET_INIT	0x00000001U
#define DRV_AHCI_PX_SCTL_SPD_MASK	0x000000f0U
#define DRV_AHCI_PX_SCTL_IPM_MASK	0x00000f00U
#define DRV_AHCI_PX_SCTL_IPM_NONE	0x00000300U

/*
 * Device classes by the upper half of PxSIG (LBA mid and high of the first
 * D2H Register FIS); the lower half is the sector count and LBA low, which
 * not every device sets to one.
 */
#define DRV_AHCI_SIG_CLASS_ATA		0x0000U
#define DRV_AHCI_SIG_CLASS_ATAPI	0xeb14U
#define DRV_AHCI_SIG_CLASS_PM		0x9669U
#define DRV_AHCI_SIG_CLASS_SEMB		0xc33cU

/* ATA commands used by the basic driver. */
#define DRV_AHCI_ATA_READ_DMA_EXT	0x25U
#define DRV_AHCI_ATA_WRITE_DMA_EXT	0x35U
#define DRV_AHCI_ATA_FLUSH_CACHE	0xe7U
#define DRV_AHCI_ATA_FLUSH_CACHE_EXT	0xeaU
#define DRV_AHCI_ATA_IDENTIFY		0xecU

#define DRV_AHCI_ATA_DEVICE_LBA		0x40U

#define DRV_AHCI_FIS_TYPE_H2D		0x27U
#define DRV_AHCI_FIS_H2D_COMMAND	0x80U
#define DRV_AHCI_FIS_H2D_DWORDS		5U

/* Memory the HBA reads and writes, with the alignment AHCI requires. */
#define DRV_AHCI_COMMAND_HEADER_SIZE	32U
#define DRV_AHCI_COMMAND_LIST_SIZE	1024U
#define DRV_AHCI_COMMAND_LIST_ALIGN	1024U
#define DRV_AHCI_RECEIVED_FIS_SIZE	256U
#define DRV_AHCI_RECEIVED_FIS_ALIGN	256U
#define DRV_AHCI_TABLE_ALIGN		128U
#define DRV_AHCI_TABLE_PRDT_OFFSET	0x80U
#define DRV_AHCI_TABLE_PRD_MAX		16U
#define DRV_AHCI_TABLE_SIZE \
	(DRV_AHCI_TABLE_PRDT_OFFSET + DRV_AHCI_TABLE_PRD_MAX * 16U)
#define DRV_AHCI_SLOT_MAX		32U

/* One PRD entry moves at most 4 MiB, and always an even byte count. */
#define DRV_AHCI_PRD_MAX_BYTES		(4U * 1024U * 1024U)
#define DRV_AHCI_PRD_INTERRUPT		0x80000000U

#define DRV_AHCI_HEADER_WRITE		0x00000040U
#define DRV_AHCI_HEADER_PRDTL_SHIFT	16U

#define DRV_AHCI_IDENTIFY_SIZE		512U
#define DRV_AHCI_SECTOR_SIZE		512U
#define DRV_AHCI_SECTOR_SIZE_MAX	65536U

/* Why an IDENTIFY DEVICE answer cannot be used by this driver. */
#define DRV_AHCI_IDENTIFY_NO_LBA48	0x00000001U
#define DRV_AHCI_IDENTIFY_NO_CAPACITY	0x00000002U
#define DRV_AHCI_IDENTIFY_SECTOR_SIZE	0x00000004U
#define DRV_AHCI_IDENTIFY_INVALID	0x00000008U

/* How a finished or failed command left the port. */
#define DRV_AHCI_RESULT_RUNNING		0U
#define DRV_AHCI_RESULT_DONE		1U
#define DRV_AHCI_RESULT_DEVICE_ERROR	2U
#define DRV_AHCI_RESULT_LINK_ERROR	3U

/*
 * The life of the one command a port runs.  Only an ACTIVE command can be
 * completed by the hardware status; once the waiter gives up it marks the
 * command ABORTED before it stops the engine, because stopping the engine
 * clears PxCI and must not read as a completion.
 */
#define DRV_AHCI_COMMAND_IDLE		0U
#define DRV_AHCI_COMMAND_ACTIVE		1U
#define DRV_AHCI_COMMAND_DONE		2U
#define DRV_AHCI_COMMAND_DEVICE_ERROR	3U
#define DRV_AHCI_COMMAND_LINK_ERROR	4U
#define DRV_AHCI_COMMAND_ABORTED	5U

/*
 * The Register Host-to-Device FIS that carries one ATA command.
 *
 * The byte layout is fixed by SATA; the driver copies one of these into the
 * first 20 bytes of a command table.
 */
struct drv_ahci_fis_h2d {
	uint8_t type;
	uint8_t flags;
	uint8_t command;
	uint8_t features_low;
	uint8_t lba0;
	uint8_t lba1;
	uint8_t lba2;
	uint8_t device;
	uint8_t lba3;
	uint8_t lba4;
	uint8_t lba5;
	uint8_t features_high;
	uint8_t count_low;
	uint8_t count_high;
	uint8_t icc;
	uint8_t control;
	uint8_t reserved[4];
} __attribute__((packed));

/*
 * One entry of a port's command list.
 *
 * The HBA reads the table address and the PRD count from it when the slot is
 * issued and writes back the byte count it moved.
 */
struct drv_ahci_command_header {
	uint32_t flags;
	uint32_t transferred;
	uint32_t table_low;
	uint32_t table_high;
	uint32_t reserved[4];
} __attribute__((packed));

/*
 * One physical region of a command's data.
 *
 * The byte count is stored minus one, and the top bit asks for an interrupt
 * when the region is done.
 */
struct drv_ahci_prd {
	uint32_t address_low;
	uint32_t address_high;
	uint32_t reserved;
	uint32_t count;
} __attribute__((packed));

/*
 * What the driver keeps from a disk's IDENTIFY DEVICE answer.
 *
 * Only the facts that decide how the disk is addressed and flushed are kept;
 * the text fields are for the attach log.
 */
struct drv_ahci_identity {
	uint64_t sector_count;
	uint32_t sector_size;
	unsigned flush;
	unsigned flush_ext;
	unsigned write_cache;
	uint16_t cylinders;
	uint16_t heads;
	uint16_t sectors_per_track;
};

/*
 * Reports how many command slots the driver uses on this HBA.
 */
static __inline unsigned
drv_ahci_slot_count(
	uint32_t capability)
{
	unsigned slots;

	/* CAP.NCS holds the number of slots minus one. */
	slots = ((capability & DRV_AHCI_CAP_NCS_MASK) >> DRV_AHCI_CAP_NCS_SHIFT) + 1U;

	/* Reports the slot count, which the field width already bounds by 32. */
	return slots;
}

/*
 * Reports the offset of one port's registers inside the ABAR.
 */
static __inline size_t
drv_ahci_port_offset(
	unsigned port)
{
	/* Reports the start of the port's 0x80-byte register block. */
	return DRV_AHCI_PORT_BASE + (size_t)port * DRV_AHCI_PORT_STRIDE;
}

/*
 * Builds the Register Host-to-Device FIS for one ATA command.
 *
 * READ and WRITE DMA EXT carry a 48-bit LBA and a count of 1 to 65536
 * sectors, where 65536 is encoded as zero.  FLUSH CACHE EXT and IDENTIFY
 * DEVICE carry neither.  Returns 1 when the FIS was built.
 */
static __inline int
drv_ahci_fis_h2d_build(
	struct drv_ahci_fis_h2d *fis,
	uint8_t command,
	uint64_t lba,
	uint32_t count)
{
	unsigned data_command;

	/* Refuses a missing destination. */
	if (fis == NULL)
		return 0;

	/* Sorts the command into one that moves sectors and one that does not. */
	data_command = 0;
	if (command == DRV_AHCI_ATA_READ_DMA_EXT) {
		data_command = 1;
	} else if (command == DRV_AHCI_ATA_WRITE_DMA_EXT) {
		data_command = 1;
	} else if (command == DRV_AHCI_ATA_FLUSH_CACHE_EXT) {
		data_command = 0;
	} else if (command == DRV_AHCI_ATA_FLUSH_CACHE) {
		data_command = 0;
	} else if (command == DRV_AHCI_ATA_IDENTIFY) {
		data_command = 0;
	} else {
		/* The basic driver issues no other command. */
		return 0;
	}

	/* A sector command moves 1 to 65536 sectors inside the 48-bit space. */
	if (data_command) {
		if (count == 0U)
			return 0;
		if (count > 65536U)
			return 0;
		if (lba > (UINT64_C(1) << 48) - count)
			return 0;
	}

	/* A command without sectors names no address and no count. */
	if (!data_command) {
		if (lba != 0U)
			return 0;
		if (count != 0U)
			return 0;
	}

	/* Starts from a FIS with every field clear. */
	fis->type = 0;
	fis->flags = 0;
	fis->command = 0;
	fis->features_low = 0;
	fis->lba0 = 0;
	fis->lba1 = 0;
	fis->lba2 = 0;
	fis->device = 0;
	fis->lba3 = 0;
	fis->lba4 = 0;
	fis->lba5 = 0;
	fis->features_high = 0;
	fis->count_low = 0;
	fis->count_high = 0;
	fis->icc = 0;
	fis->control = 0;
	fis->reserved[0] = 0;
	fis->reserved[1] = 0;
	fis->reserved[2] = 0;
	fis->reserved[3] = 0;

	/* Marks the FIS as a command update of the device's registers. */
	fis->type = DRV_AHCI_FIS_TYPE_H2D;
	fis->flags = DRV_AHCI_FIS_H2D_COMMAND;
	fis->command = command;

	/* Places the 48-bit LBA and the sector count, low byte first. */
	if (data_command) {
		fis->device = DRV_AHCI_ATA_DEVICE_LBA;
		fis->lba0 = (uint8_t)(lba & 0xffU);
		fis->lba1 = (uint8_t)((lba >> 8) & 0xffU);
		fis->lba2 = (uint8_t)((lba >> 16) & 0xffU);
		fis->lba3 = (uint8_t)((lba >> 24) & 0xffU);
		fis->lba4 = (uint8_t)((lba >> 32) & 0xffU);
		fis->lba5 = (uint8_t)((lba >> 40) & 0xffU);
		fis->count_low = (uint8_t)(count & 0xffU);
		fis->count_high = (uint8_t)((count >> 8) & 0xffU);
	}

	/* Succeeded: the FIS is ready to copy into a command table. */
	return 1;
}

/*
 * Builds the command list entry that points the HBA at one command table.
 *
 * Returns 1 when the entry was built.
 */
static __inline int
drv_ahci_command_header_build(
	struct drv_ahci_command_header *header,
	unsigned fis_dwords,
	int write,
	unsigned prd_count,
	uint64_t table_address)
{
	uint32_t flags;

	/* Refuses a missing destination. */
	if (header == NULL)
		return 0;

	/* The command FIS length is 2 to 16 doublewords. */
	if (fis_dwords < 2U || fis_dwords > 16U)
		return 0;

	/* The table holds at most the PRD entries the driver allocated. */
	if (prd_count > DRV_AHCI_TABLE_PRD_MAX)
		return 0;

	/* A command table must start on a 128-byte boundary. */
	if ((table_address & (DRV_AHCI_TABLE_ALIGN - 1U)) != 0U)
		return 0;

	/* Encodes the FIS length, the direction and the PRD count. */
	flags = (uint32_t)fis_dwords;
	if (write)
		flags |= DRV_AHCI_HEADER_WRITE;
	flags |= (uint32_t)prd_count << DRV_AHCI_HEADER_PRDTL_SHIFT;

	/* Fills the entry; the HBA counts transferred bytes up from zero. */
	header->flags = flags;
	header->transferred = 0;
	header->table_low = (uint32_t)(table_address & 0xffffffffU);
	header->table_high = (uint32_t)(table_address >> 32);
	header->reserved[0] = 0;
	header->reserved[1] = 0;
	header->reserved[2] = 0;
	header->reserved[3] = 0;

	/* Succeeded: the slot's entry names its table. */
	return 1;
}

/*
 * Describes one physically contiguous data region as PRD entries.
 *
 * The region is cut into pieces of at most 4 MiB; the last entry asks for an
 * interrupt.  Returns 1 and the number of entries used, or 0 when the region
 * cannot be described in the entries available.
 */
static __inline int
drv_ahci_prdt_build(
	struct drv_ahci_prd *entries,
	unsigned capacity,
	uint64_t address,
	size_t length,
	unsigned *count)
{
	size_t remaining;
	size_t piece;
	unsigned used;

	/* Refuses missing destinations. */
	if (entries == NULL)
		return 0;
	if (count == NULL)
		return 0;
	*count = 0;

	/* A region must be present, and AHCI moves whole 16-bit words. */
	if (length == 0U)
		return 0;
	if ((length & 1U) != 0U)
		return 0;
	if ((address & 1U) != 0U)
		return 0;

	/* Cuts the region into entries of at most 4 MiB each. */
	remaining = length;
	used = 0;
	while (remaining != 0U) {
		/* Refuses a region that needs more entries than the table holds. */
		if (used >= capacity)
			return 0;

		/* Takes the next piece, no larger than one entry can move. */
		piece = remaining;
		if (piece > DRV_AHCI_PRD_MAX_BYTES)
			piece = DRV_AHCI_PRD_MAX_BYTES;

		/* Records the piece; the byte count is stored minus one. */
		entries[used].address_low = (uint32_t)(address & 0xffffffffU);
		entries[used].address_high = (uint32_t)(address >> 32);
		entries[used].reserved = 0;
		entries[used].count = (uint32_t)(piece - 1U);

		/* Moves past the piece just described. */
		address += piece;
		remaining -= piece;
		used++;
	}

	/* Asks for an interrupt when the last region is done. */
	entries[used - 1U].count |= DRV_AHCI_PRD_INTERRUPT;

	/* Succeeded: reports how many entries describe the region. */
	*count = used;
	return 1;
}

/*
 * Decodes the facts the driver needs from an IDENTIFY DEVICE answer.
 *
 * Returns zero when the disk can be used, or the DRV_AHCI_IDENTIFY_* reasons
 * it cannot.
 */
static __inline uint32_t
drv_ahci_identify_parse(
	const uint16_t *words,
	struct drv_ahci_identity *identity)
{
	uint32_t reasons;
	uint32_t logical_words;
	uint32_t size;

	/* Refuses missing input or output. */
	if (words == NULL)
		return DRV_AHCI_IDENTIFY_INVALID;
	if (identity == NULL)
		return DRV_AHCI_IDENTIFY_INVALID;

	/* Starts with no reason to refuse the disk. */
	reasons = 0;

	/* Word 83 bit 10: the 48-bit address feature set, needed for DMA EXT. */
	if ((words[83] & 0x0400U) == 0U)
		reasons |= DRV_AHCI_IDENTIFY_NO_LBA48;

	/* Words 100 to 103: the number of user-addressable logical sectors. */
	identity->sector_count = (uint64_t)words[100];
	identity->sector_count |= (uint64_t)words[101] << 16;
	identity->sector_count |= (uint64_t)words[102] << 32;
	identity->sector_count |= (uint64_t)words[103] << 48;
	if (identity->sector_count == 0U)
		reasons |= DRV_AHCI_IDENTIFY_NO_CAPACITY;

	/*
	 * Word 106 is valid when bit 14 is one and bit 15 zero.  Its bit 12 says
	 * that words 117 and 118 give the logical sector size in 16-bit words;
	 * otherwise a logical sector is 512 bytes.
	 */
	size = DRV_AHCI_SECTOR_SIZE;
	if ((words[106] & 0xc000U) == 0x4000U) {
		if ((words[106] & 0x1000U) != 0U) {
			logical_words = (uint32_t)words[117];
			logical_words |= (uint32_t)words[118] << 16;
			size = logical_words * 2U;
		}
	}

	/* Accepts a power-of-two size from 512 bytes to 64 KiB. */
	if (size < DRV_AHCI_SECTOR_SIZE) {
		reasons |= DRV_AHCI_IDENTIFY_SECTOR_SIZE;
	} else if (size > DRV_AHCI_SECTOR_SIZE_MAX) {
		reasons |= DRV_AHCI_IDENTIFY_SECTOR_SIZE;
	} else if ((size & (size - 1U)) != 0U) {
		reasons |= DRV_AHCI_IDENTIFY_SECTOR_SIZE;
	}

	/* Records the size, usable or not, for the caller's log. */
	identity->sector_size = size;

	/* Word 83 bit 12: FLUSH CACHE is supported. */
	identity->flush = 0;
	if ((words[83] & 0x1000U) != 0U)
		identity->flush = 1;

	/* Word 83 bit 13: FLUSH CACHE EXT is supported. */
	identity->flush_ext = 0;
	if ((words[83] & 0x2000U) != 0U)
		identity->flush_ext = 1;

	/* Word 85 bit 5: the volatile write cache is enabled. */
	identity->write_cache = 0;
	if ((words[85] & 0x0020U) != 0U)
		identity->write_cache = 1;

	/* Words 1, 3 and 6: the obsolete default geometry, zero on new disks. */
	identity->cylinders = words[1];
	identity->heads = words[3];
	identity->sectors_per_track = words[6];

	/* Reports what keeps the disk out of service, or zero. */
	return reasons;
}

/*
 * Copies an IDENTIFY string field into a terminated C string.
 *
 * ATA strings hold two characters per word with the first in the high byte,
 * padded with spaces; the padding is dropped.
 */
static __inline void
drv_ahci_identify_text(
	const uint16_t *words,
	unsigned first_word,
	unsigned word_count,
	char *text,
	size_t size)
{
	size_t length;
	unsigned index;

	/* Refuses a destination with no room for the terminator. */
	if (text == NULL)
		return;
	if (size == 0U)
		return;
	text[0] = '\0';
	if (words == NULL)
		return;

	/* Unpacks each word, high byte first, while the destination has room. */
	length = 0;
	for (index = 0; index < word_count; index++) {
		if (length + 2U >= size)
			break;
		text[length] = (char)(words[first_word + index] >> 8);
		text[length + 1U] = (char)(words[first_word + index] & 0xffU);
		length += 2U;
	}

	/* Drops the trailing space padding. */
	while (length > 0U && text[length - 1U] == ' ')
		length--;
	text[length] = '\0';
}

/*
 * Classifies a port's interrupt status and task file for one issued slot.
 *
 * A device error (TFES or the ERR bit) ends the command; a link or HBA fault
 * means the port must be reset; a cleared command-issue bit with neither
 * means the command finished.
 */
static __inline unsigned
drv_ahci_command_result(
	uint32_t interrupt_status,
	uint32_t task_file,
	uint32_t command_issue,
	uint32_t slot_mask)
{
	/* An HBA, interface or link fault leaves the port needing a reset. */
	if ((interrupt_status & DRV_AHCI_PX_IS_LINK_FAULT) != 0U)
		return DRV_AHCI_RESULT_LINK_ERROR;

	/* The device reported an error status for the command. */
	if ((interrupt_status & DRV_AHCI_PX_IS_TFES) != 0U)
		return DRV_AHCI_RESULT_DEVICE_ERROR;

	/* The slot is still owned by the HBA. */
	if ((command_issue & slot_mask) != 0U)
		return DRV_AHCI_RESULT_RUNNING;

	/* The slot came back, but the final status still carries ERR. */
	if ((task_file & DRV_AHCI_PX_TFD_ERR) != 0U)
		return DRV_AHCI_RESULT_DEVICE_ERROR;

	/* Succeeded: the command finished without an error. */
	return DRV_AHCI_RESULT_DONE;
}

/*
 * Reports how many blocks of a request the next command moves.
 *
 * One command moves at most what fits in the bounce buffer, and at most
 * 65536 sectors.  Returns zero when not even one block fits.
 */
static __inline uint32_t
drv_ahci_chunk_blocks(
	uint32_t remaining,
	uint32_t block_size,
	size_t bounce_size)
{
	size_t fit;

	/* A block size of zero describes no disk. */
	if (block_size == 0U)
		return 0;

	/* Counts the whole blocks that fit in the bounce buffer. */
	fit = bounce_size / block_size;
	if (fit > 65536U)
		fit = 65536U;

	/* Reports the smaller of the remainder and what one command moves. */
	if ((size_t)remaining < fit)
		return remaining;
	return (uint32_t)fit;
}

/*
 * Reports the device class in a port's signature register.
 */
static __inline unsigned
drv_ahci_signature_class(
	uint32_t signature)
{
	unsigned device_class;

	/* The class is in LBA mid and LBA high, the upper 16 bits. */
	device_class = (unsigned)(signature >> 16);

	/* Reports the class for comparison with DRV_AHCI_SIG_CLASS_*. */
	return device_class;
}

/*
 * Advances a port's command state by what the hardware shows.
 *
 * Only an ACTIVE command moves: a command already finished, failed or
 * aborted keeps its state whatever PxCI says afterwards.
 */
static __inline unsigned
drv_ahci_command_advance(
	unsigned state,
	uint32_t interrupt_status,
	uint32_t task_file,
	uint32_t command_issue,
	uint32_t slot_mask)
{
	unsigned result;

	/* A command that is not running is not touched by later status. */
	if (state != DRV_AHCI_COMMAND_ACTIVE)
		return state;

	/* Classifies the status the port shows for the running slot. */
	result = drv_ahci_command_result(
		interrupt_status,
		task_file,
		command_issue,
		slot_mask);

	/* Maps the classification to the next state. */
	if (result == DRV_AHCI_RESULT_DONE)
		return DRV_AHCI_COMMAND_DONE;
	if (result == DRV_AHCI_RESULT_DEVICE_ERROR)
		return DRV_AHCI_COMMAND_DEVICE_ERROR;
	if (result == DRV_AHCI_RESULT_LINK_ERROR)
		return DRV_AHCI_COMMAND_LINK_ERROR;

	/* Still running. */
	return DRV_AHCI_COMMAND_ACTIVE;
}

/*
 * Gives up on a command whose time ran out.
 *
 * The caller has looked at the hardware one last time under the lock, so a
 * command that finished in that look keeps its outcome.
 */
static __inline unsigned
drv_ahci_command_expire(
	unsigned state)
{
	/* Only a command still running is aborted. */
	if (state == DRV_AHCI_COMMAND_ACTIVE)
		return DRV_AHCI_COMMAND_ABORTED;

	/* A command that reached an outcome keeps it. */
	return state;
}

/*
 * Says whether a finished command is evidence of a missing interrupt.
 *
 * Only a completion found after a sleep that the poll interval ended,
 * with no wakeup from the interrupt handler, counts.  A command found
 * finished at the waiter's first look, before it ever slept, finished
 * faster than its interrupt could be taken (the interrupt then finds the
 * status already cleared); that says nothing about the interrupt path.
 * An idle-thread waiter never sleeps and is not counted.
 */
static __inline int
drv_ahci_completion_by_polling(
	unsigned state,
	int idle,
	int slept,
	int woken)
{
	/* Only a command that finished without an error is counted. */
	if (state != DRV_AHCI_COMMAND_DONE)
		return 0;

	/* A waiter that yields instead of sleeping never waits for an interrupt. */
	if (idle)
		return 0;

	/* A completion seen before any sleep did not wait for an interrupt. */
	if (!slept)
		return 0;

	/* The interrupt handler woke the waiter: the interrupt worked. */
	if (woken)
		return 0;

	/* The poll interval, not an interrupt, ended the last sleep. */
	return 1;
}

/*
 * Reports whether a disk's volatile write cache may hold unflushed data
 * after one command.
 *
 * Any write, even one that failed, may have left data in the cache.  A
 * flush that succeeded empties it of every earlier write, since a port runs
 * one command at a time.  Other commands leave it as it was.
 */
static __inline int
drv_ahci_cache_dirty_after(
	int dirty,
	uint8_t command,
	int succeeded)
{
	/* A write may have reached the cache, whatever its outcome. */
	if (command == DRV_AHCI_ATA_WRITE_DMA_EXT)
		return 1;

	/* A failed flush or a non-flush command changes nothing. */
	if (!succeeded)
		return dirty;
	if (command != DRV_AHCI_ATA_FLUSH_CACHE_EXT &&
	    command != DRV_AHCI_ATA_FLUSH_CACHE)
		return dirty;

	/* A finished flush leaves nothing behind it. */
	return 0;
}

#endif
