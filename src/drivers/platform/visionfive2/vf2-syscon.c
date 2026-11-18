/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The JH7110's three system control blocks.
 *
 * The blocks are found by their compatible strings and mapped once at boot.
 * Reads are single register loads and take no lock; every change is a
 * read-modify-write under syscon_lock, because the SD, PCIe and Ethernet
 * drivers each own a few bits of registers they share.
 */

#include "drivers/platform/visionfive2/vf2-syscon.h"
#include <kern/device-io.h>
#include <kern/lock.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

/* The most cells a phandle-and-arguments property is read with. */
#define SYSCON_PROPERTY_CELLS	8U

/*
 * One block's place in the device tree and in the kernel's address space.
 *
 * Filled by drv_vf2_syscon_init() at boot and never changed afterwards.
 * Registers is NULL while the block is absent.
 */
struct syscon_block {
	uint32_t node;
	volatile uint8_t *registers;
	uint64_t size;
};

/*
 * The compatible string of each block, in drv_vf2_syscon order.
 *
 * Value referenced:
 * Linux v6.12 arch/riscv/boot/dts/starfive/jh7110.dtsi
 * (sys_syscon, stg_syscon, aon_syscon).
 */
static const char *const syscon_compatibles[DRV_VF2_SYSCON_COUNT] = {
	"starfive,jh7110-sys-syscon",
	"starfive,jh7110-stg-syscon",
	"starfive,jh7110-aon-syscon"
};

/*
 * The mapped blocks, in drv_vf2_syscon order.
 *
 * Written only by drv_vf2_syscon_init(), which the board runs once on the
 * boot CPU before any driver that uses the blocks attaches; read without
 * a lock afterwards.
 */
static struct syscon_block syscon_blocks[DRV_VF2_SYSCON_COUNT];

/*
 * Serializes every read-modify-write of a block register.
 *
 * A leaf lock: nothing else is taken while it is held.  Taken with
 * interrupts held off, so an update may also run from an interrupt.
 */
static struct spinlock syscon_lock = {
	{ 0 }, LOCK_RANK_DEVICE, "vf2 syscon", 0, 0
};

static int map_block(const struct drv_fdt *fdt, enum drv_vf2_syscon syscon);
static int register_address(enum drv_vf2_syscon syscon, uint32_t offset, volatile uint32_t **address);
static uint32_t read_cell(const uint8_t *value, uint32_t index);

/*
 * Finds the three blocks in the device tree and maps them.
 */
int
drv_vf2_syscon_init(
	const struct drv_fdt *fdt)
{
	unsigned syscon;
	int error;

	/* Refuses a missing tree. */
	if (fdt == NULL)
		return EINVAL;

	/* Maps every block the tree describes; a missing one stays absent. */
	for (syscon = 0; syscon < DRV_VF2_SYSCON_COUNT; syscon++) {
		error = map_block(fdt, (enum drv_vf2_syscon)syscon);
		if (error != 0 && error != ENOENT)
			return error;
	}

	/* Refuses a tree without the block that holds the PLL settings. */
	if (syscon_blocks[DRV_VF2_SYSCON_SYS].registers == NULL)
		return ENODEV;

	/* Succeeded: the blocks the tree describes are mapped. */
	return 0;
}

/*
 * Reads one register of a block.
 */
int
drv_vf2_syscon_read(
	enum drv_vf2_syscon syscon,
	uint32_t offset,
	uint32_t *value)
{
	volatile uint32_t *address;
	int error;

	/* Refuses a missing result. */
	if (value == NULL)
		return EINVAL;

	/* Finds the register; an absent block or a bad offset is refused. */
	error = register_address(syscon, offset, &address);
	if (error != 0)
		return error;

	/* Succeeded: the register's contents. */
	*value = kern_mmio_read32(address);
	return 0;
}

/*
 * Changes the bits of one register that the mask selects.
 */
int
drv_vf2_syscon_update(
	enum drv_vf2_syscon syscon,
	uint32_t offset,
	uint32_t mask,
	uint32_t value)
{
	volatile uint32_t *address;
	unsigned long irq;
	uint32_t old_contents;
	uint32_t new_contents;
	int error;

	/* Finds the register; an absent block or a bad offset is refused. */
	error = register_address(syscon, offset, &address);
	if (error != 0)
		return error;

	/* Replaces the selected bits, writing only when they change. */
	irq = spin_lock_irqsave(&syscon_lock);

	old_contents = kern_mmio_read32(address);
	new_contents = (old_contents & ~mask) | (value & mask);
	if (new_contents != old_contents)
		kern_mmio_write32(address, new_contents);

	spin_unlock_irqrestore(&syscon_lock, irq);

	/* Succeeded: the selected bits hold the value. */
	return 0;
}

/*
 * Reads a property that names a block by phandle, followed by arguments.
 */
int
drv_vf2_syscon_lookup(
	const struct drv_fdt *fdt,
	uint32_t node,
	const char *name,
	enum drv_vf2_syscon *syscon,
	uint32_t *arguments,
	unsigned capacity,
	unsigned *count)
{
	const uint8_t *value;
	uint32_t length;
	uint32_t cells;
	uint32_t target;
	unsigned candidate;
	unsigned index;
	int error;

	/* Refuses a missing blob, name or result. */
	if (fdt == NULL || name == NULL || syscon == NULL || count == NULL)
		return EINVAL;
	if (capacity != 0 && arguments == NULL)
		return EINVAL;

	/* Reads the property, which holds at least the phandle. */
	error = drv_fdt_property(fdt, node, name, &value, &length);
	if (error != 0)
		return error;

	/* Refuses a value that is not whole cells, or more cells than are read. */
	if (length < 4U || (length & 3U) != 0)
		return EINVAL;
	cells = length / 4U;
	if (cells > SYSCON_PROPERTY_CELLS)
		return E2BIG;

	/* Finds the node the phandle names. */
	error = drv_fdt_find_phandle(fdt, read_cell(value, 0), &target);
	if (error != 0)
		return error;

	/* Matches the node with one of the mapped blocks. */
	*syscon = DRV_VF2_SYSCON_COUNT;
	for (candidate = 0; candidate < DRV_VF2_SYSCON_COUNT; candidate++) {
		/* A block counts only once it is mapped. */
		if (syscon_blocks[candidate].registers == NULL)
			continue;
		if (syscon_blocks[candidate].node == target)
			*syscon = (enum drv_vf2_syscon)candidate;
	}

	/* Refuses a phandle that names no known block. */
	if (*syscon == DRV_VF2_SYSCON_COUNT)
		return ENODEV;

	/* Copies the arguments that follow the phandle. */
	*count = cells - 1U;
	for (index = 0; index + 1U < cells && index < capacity; index++)
		arguments[index] = read_cell(value, index + 1U);

	/* Succeeded: the block and its arguments. */
	return 0;
}

/* Finds and maps one block, recording where it is. */
static int
map_block(
	const struct drv_fdt *fdt,
	enum drv_vf2_syscon syscon)
{
	struct syscon_block *block;
	uint64_t address;
	uint64_t size;
	uint32_t node;
	void *mapped;
	int error;

	/* Finds the block's node; a tree without it leaves the block absent. */
	block = &syscon_blocks[syscon];
	error = drv_fdt_find_compatible(fdt, syscon_compatibles[syscon], DRV_FDT_NO_NODE, &node);
	if (error != 0)
		return ENOENT;

	/* Reads where the registers are. */
	error = drv_fdt_reg(fdt, node, 0, &address, &size);
	if (error != 0)
		return error;

	/* Refuses a block smaller than a register or larger than any of the three. */
	if (size < 4U || size > 0x10000U)
		return EINVAL;

	/* Maps the registers as device memory. */
	error = kern_device_map(address, (size_t)size, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0)
		return error;

	/* Succeeded: records the block for the other calls. */
	block->node = node;
	block->size = size;
	block->registers = mapped;
	return 0;
}

/* Finds the kernel address of one register of a mapped block. */
static int
register_address(
	enum drv_vf2_syscon syscon,
	uint32_t offset,
	volatile uint32_t **address)
{
	struct syscon_block *block;

	/* Refuses a block number outside the three. */
	if ((unsigned)syscon >= DRV_VF2_SYSCON_COUNT)
		return EINVAL;

	/* Refuses a block the tree did not describe. */
	block = &syscon_blocks[syscon];
	if (block->registers == NULL)
		return ENODEV;

	/* Refuses an offset off a register or past the block. */
	if ((offset & 3U) != 0)
		return EINVAL;
	if ((uint64_t)offset + 4U > block->size)
		return EINVAL;

	/* Succeeded: the register inside the mapping. */
	*address = (volatile uint32_t *)(block->registers + offset);
	return 0;
}

/* Reads one big-endian cell of a property value. */
static uint32_t
read_cell(
	const uint8_t *value,
	uint32_t index)
{
	uint64_t number;

	/* Decodes the single cell at the index. */
	number = drv_fdt_cells_value(value, index, 1U);

	/* Reports the cell's number. */
	return (uint32_t)number;
}
