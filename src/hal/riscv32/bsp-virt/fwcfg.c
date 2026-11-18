/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The emulator's firmware configuration interface on the virt board.
 *
 * The interface is a selector register, a data register and a DMA address
 * register.  The board's HAL reads the file directory through the data
 * register and writes a file through DMA, which is the only way a file can
 * be written.  The device tree names the interface; without it the board
 * has no display and nothing here is used.
 */

#include <hal/hal.h>
#include "../defs.h"
#include "../space.h"
#include "fwcfg.h"

/*
 * The registers, the fixed selectors and the DMA control bits.
 *
 * Values referenced: QEMU v10.0.0 docs/specs/fw_cfg.rst (the memory-mapped
 * register offsets: data at 0, selector at 8, DMA address at 16; the
 * signature and feature selectors 0x0000 and 0x0001 and the feature's DMA
 * bit; the file directory at selector 0x0019 and its 64-byte entries with a
 * 56-byte name; the DMA control bits error, select and write).
 */
#define FWCFG_DATA		0x00U
#define FWCFG_SELECTOR		0x08U
#define FWCFG_DMA_HIGH		0x10U
#define FWCFG_DMA_LOW		0x14U
#define FWCFG_KEY_SIGNATURE	0x0000U
#define FWCFG_KEY_FEATURES	0x0001U
#define FWCFG_KEY_FILE_DIR	0x0019U
#define FWCFG_FEATURE_DMA	0x2U
#define FWCFG_FILE_NAME_SIZE	56U
#define FWCFG_DMA_ERROR		0x01U
#define FWCFG_DMA_SELECT	0x08U
#define FWCFG_DMA_WRITE		0x10U

/* How many times the completion of one DMA request is looked for. */
#define FWCFG_DMA_POLLS		10000000U

/*
 * One DMA request, in the interface's byte order (big-endian), as the
 * device reads it from memory.
 *
 * Values referenced: QEMU v10.0.0 docs/specs/fw_cfg.rst (FWCfgDmaAccess:
 * a 32-bit control, a 32-bit length and a 64-bit address).
 */
struct fwcfg_dma_request {
	uint32_t control;
	uint32_t length;
	uint64_t address;
};

/*
 * The interface's registers through the device window, or NULL before
 * virt_fwcfg_init() found the interface.  Set once on the boot hart.
 */
static volatile uint8_t *fwcfg_registers;

/*
 * The DMA request the board writes, kept in the kernel image so that its
 * physical address is known.  Used only by the boot hart before the
 * kernel starts, so it needs no lock.
 */
static struct fwcfg_dma_request fwcfg_request __attribute__((aligned(16)));

static void select_item(uint16_t selector);
static uint8_t read_byte(void);
static uint32_t read_be32(void);
static uint16_t read_be16(void);
static int name_equal(const char *name, const char *wanted);

/*
 * Finds the interface and checks that it can take DMA writes.
 *
 * Returns HAL_OK, or HAL_ERR_UNSUPPORTED when there is no interface or it
 * cannot write files.
 */
int
virt_fwcfg_init(
	uint64_t base_phys,
	uint64_t size)
{
	uint8_t signature[4];
	uint32_t features;
	unsigned index;

	/* A board without the interface has nothing to set up. */
	if (base_phys == 0 || size < FWCFG_DMA_LOW + 4U)
		return HAL_ERR_UNSUPPORTED;
	fwcfg_registers = (volatile uint8_t *)(RISCV32_DEVICE_BASE + base_phys);

	/* Reads the signature, which is the four bytes "QEMU". */
	select_item(FWCFG_KEY_SIGNATURE);
	for (index = 0; index < 4U; index++)
		signature[index] = read_byte();
	if (signature[0] != 'Q' || signature[1] != 'E' || signature[2] != 'M' || signature[3] != 'U') {
		fwcfg_registers = NULL;
		return HAL_ERR_UNSUPPORTED;
	}

	/* Reads the features, little-endian; a file can be written only through DMA. */
	select_item(FWCFG_KEY_FEATURES);
	features = read_byte();
	features |= (uint32_t)read_byte() << 8;
	features |= (uint32_t)read_byte() << 16;
	features |= (uint32_t)read_byte() << 24;
	if ((features & FWCFG_FEATURE_DMA) == 0) {
		fwcfg_registers = NULL;
		return HAL_ERR_UNSUPPORTED;
	}

	/* Succeeded: files can be found and written. */
	return HAL_OK;
}

/*
 * Finds a file by its name in the interface's directory.
 *
 * Returns HAL_OK with the file's selector and size, HAL_ERR_STATE before
 * virt_fwcfg_init() succeeded, or HAL_ERR_INVALID for a name that is not
 * there.
 */
int
virt_fwcfg_find(
	const char *name,
	uint16_t *selector,
	uint32_t *size)
{
	char entry_name[FWCFG_FILE_NAME_SIZE];
	uint32_t count;
	uint32_t entry;
	uint32_t entry_size;
	uint16_t entry_selector;
	unsigned index;
	int equal;

	/* Nothing can be found without the interface. */
	if (fwcfg_registers == NULL)
		return HAL_ERR_STATE;

	/* The directory starts with the number of files. */
	select_item(FWCFG_KEY_FILE_DIR);
	count = read_be32();

	/* Reads each entry: size, selector, a reserved word and the name. */
	for (entry = 0; entry < count; entry++) {
		entry_size = read_be32();
		entry_selector = read_be16();
		(void)read_be16();
		for (index = 0; index < FWCFG_FILE_NAME_SIZE; index++)
			entry_name[index] = (char)read_byte();
		entry_name[FWCFG_FILE_NAME_SIZE - 1U] = '\0';

		/* Stops at the entry with the name. */
		equal = name_equal(entry_name, name);
		if (equal) {
			*selector = entry_selector;
			*size = entry_size;
			return HAL_OK;
		}
	}

	/* The directory has no such file. */
	return HAL_ERR_INVALID;
}

/*
 * Writes a file through DMA and waits until the interface took it.
 *
 * The data must be in RAM the kernel image or the direct map holds, since
 * the device reads it by its physical address.  Returns HAL_OK,
 * HAL_ERR_STATE without the interface, HAL_ERR_IO when the device reports
 * an error, or HAL_ERR_TIMEOUT when it never answers.
 */
int
virt_fwcfg_write(
	uint16_t selector,
	const void *data,
	uint32_t length)
{
	uint64_t request_phys;
	uint64_t data_phys;
	uint32_t control;
	uint32_t polls;

	/* Nothing can be written without the interface. */
	if (fwcfg_registers == NULL)
		return HAL_ERR_STATE;

	/* Describes the transfer: select the file, write the bytes from the data. */
	data_phys = riscv32_virt_to_phys(data);
	fwcfg_request.control = __builtin_bswap32(((uint32_t)selector << 16) | FWCFG_DMA_SELECT | FWCFG_DMA_WRITE);
	fwcfg_request.length = __builtin_bswap32(length);
	fwcfg_request.address = __builtin_bswap64(data_phys);

	/*
	 * Hands the request's address to the device, the high half first:
	 * writing the low half starts the transfer.
	 *
	 * Procedure referenced: QEMU v10.0.0 docs/specs/fw_cfg.rst (the DMA
	 * interface: the request's physical address written big-endian to the
	 * DMA address register, the transfer starting on the write of its low
	 * half, and the control field cleared by the device when it is done).
	 */
	request_phys = riscv32_virt_to_phys(&fwcfg_request);
	hal_mb();
	hal_mmio_write32(fwcfg_registers + FWCFG_DMA_HIGH, __builtin_bswap32((uint32_t)(request_phys >> 32)));
	hal_mmio_write32(fwcfg_registers + FWCFG_DMA_LOW, __builtin_bswap32((uint32_t)request_phys));

	/* Waits for the device to clear the control field, or to report an error. */
	for (polls = 0; polls < FWCFG_DMA_POLLS; polls++) {
		control = __builtin_bswap32(__atomic_load_n(&fwcfg_request.control, __ATOMIC_ACQUIRE));

		/* Reports a transfer the device refused. */
		if ((control & FWCFG_DMA_ERROR) != 0)
			return HAL_ERR_IO;

		/* Succeeded: the device took the whole file. */
		if (control == 0)
			return HAL_OK;
		hal_atomic_relax();
	}

	/* The device never finished. */
	return HAL_ERR_TIMEOUT;
}

/* Selects an item, which also rewinds its data to the start. */
static void
select_item(
	uint16_t selector)
{
	/* The selector register takes the item big-endian. */
	hal_mmio_write16(fwcfg_registers + FWCFG_SELECTOR, __builtin_bswap16(selector));
}

/* Reads the selected item's next byte. */
static uint8_t
read_byte(
	void)
{
	uint8_t value;

	/* Each read of the data register moves on by the bytes read. */
	value = hal_mmio_read8(fwcfg_registers + FWCFG_DATA);

	/* Succeeded: the byte. */
	return value;
}

/* Reads the selected item's next big-endian 32-bit word. */
static uint32_t
read_be32(
	void)
{
	uint32_t value;

	/* Assembles the word from its most significant byte down. */
	value = (uint32_t)read_byte() << 24;
	value |= (uint32_t)read_byte() << 16;
	value |= (uint32_t)read_byte() << 8;
	value |= (uint32_t)read_byte();

	/* Succeeded: the word in host order. */
	return value;
}

/* Reads the selected item's next big-endian 16-bit word. */
static uint16_t
read_be16(
	void)
{
	uint16_t value;

	/* Assembles the word from its most significant byte down. */
	value = (uint16_t)((uint16_t)read_byte() << 8);
	value |= (uint16_t)read_byte();

	/* Succeeded: the word in host order. */
	return value;
}

/* Reports whether a directory name is the one wanted. */
static int
name_equal(
	const char *name,
	const char *wanted)
{
	unsigned index;

	/* Compares up to the wanted name's terminator. */
	for (index = 0; wanted[index] != '\0'; index++) {
		if (index >= FWCFG_FILE_NAME_SIZE - 1U)
			return 0;
		if (name[index] != wanted[index])
			return 0;
	}

	/* Equal only when the directory name ends there too. */
	if (name[index] != '\0')
		return 0;
	return 1;
}
