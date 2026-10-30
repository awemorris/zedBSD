/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Parses the Raspberry Pi 4 device tree with the HAL's parser on the host
 * and checks what the boot needs from it: the peripherals, the memory and
 * the four spin-table CPUs (ws208-p003).
 *
 * It also checks the drivers' test for QEMU's raspi4b (T1-554) on the
 * board's tree as it is and on the tree QEMU makes of it, which has the
 * nodes of the devices QEMU does not emulate overwritten with NOP tokens.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hal/hal.h>
#include "hal/arm64/fdt.h"
#include <drivers/generic/fdt.h>
#include "drivers/platform/rpi4/rpi4-board.h"
#include <uapi/errno.h>

/* The flattened device tree's tokens, as big-endian words. */
#define TOKEN_BEGIN_NODE	1U
#define TOKEN_END_NODE		2U
#define TOKEN_PROPERTY		3U
#define TOKEN_NOP		4U

/* The largest blob the test reads. */
#define BLOB_LIMIT		(2U * 1024U * 1024U)

/*
 * The four kinds of node QEMU's raspi4b removes from the tree it passes,
 * in the order raspi4_modify_dtb() removes them.
 */
static const char *const qemu_removed[4] = {
	"brcm,bcm2711-pcie",
	"brcm,bcm2711-rng200",
	"brcm,bcm2711-thermal",
	"brcm,bcm2711-genet-v5",
};

/*
 * The tree a scene edits, a copy of the board's.
 *
 * Each scene fills it again from the board's blob, so no scene sees what
 * another one removed.
 */
static unsigned char scene_blob[BLOB_LIMIT];

static int check(int condition, const char *what);
static unsigned long read_word(const unsigned char *blob, unsigned long offset);
static int nop_node(unsigned char *blob, size_t length, unsigned long offset);
static int remove_nodes(unsigned char *blob, size_t length, const char *compatible);
static int check_board(const unsigned char *blob, size_t length);

/* The parser's one HAL dependency, done the host's way. */
void *
hal_memset(
	void *pointer,
	int value,
	size_t size)
{
	/* Fills the bytes. */
	memset(pointer, value, size);

	/* Succeeded: the filled memory. */
	return pointer;
}

/* Reports a failed check and counts it. */
static int
check(
	int condition,
	const char *what)
{
	/* A check that holds says nothing. */
	if (condition)
		return 0;

	fprintf(stderr, "rpi4-fdt-host-test: FAIL: %s\n", what);

	/* Failed: one more failure. */
	return 1;
}

/*
 * Runs the test on the device tree blob named on the command line.
 */
int
main(
	int argc,
	char **argv)
{
	static unsigned char blob[BLOB_LIMIT];
	static const unsigned long long release[4] = {0xd8, 0xe0, 0xe8, 0xf0};
	struct arm64_fdt_info info;
	FILE *file;
	size_t length;
	unsigned index;
	int failures;
	int error;

	/* Requires the blob's path. */
	if (argc != 2) {
		fprintf(stderr, "usage: rpi4-fdt-host-test bcm2711-rpi-4-b.dtb\n");
		return 2;
	}

	/* Reads the blob. */
	file = fopen(argv[1], "rb");
	if (file == NULL) {
		perror(argv[1]);
		return 2;
	}
	length = fread(blob, 1, sizeof(blob), file);
	fclose(file);

	/* Parses it as the HAL does at boot. */
	error = arm64_fdt_parse(blob, length, &info);
	if (error != 0) {
		fprintf(stderr, "rpi4-fdt-host-test: FAIL: parse: %s\n", arm64_fdt_error(error));
		return 1;
	}

	/* The peripherals the boot maps. */
	failures = 0;
	failures += check(info.compatible_board, "the tree is a Raspberry Pi 4's");
	failures += check(info.uart_base != 0, "the PL011 is found");
	failures += check(info.mailbox_base != 0, "the mailbox is found");
	failures += check(info.gic_dist_base != 0, "the GIC distributor is found");
	failures += check(info.gic_cpu_base != 0, "the GIC CPU interface is found");
	failures += check(info.sdhci_base != 0, "the eMMC2 controller is found");

	/* The four cores, each held in the firmware's spin table. */
	failures += check(info.cpu_count == 4, "four CPUs are listed");
	for (index = 0; index < info.cpu_count && index < 4; index++) {
		failures += check(info.cpus[index].mpidr == index, "a CPU's affinity is its index");
		failures += check(info.cpus[index].spin_table != 0, "a CPU uses the spin table");
		failures += check(info.cpus[index].release_addr == release[index], "a CPU's release address");
	}

	/* The drivers' test for QEMU's raspi4b, on this tree and QEMU's. */
	failures += check_board(blob, length);

	/* Reports the result. */
	if (failures != 0)
		return 1;
	printf("rpi4-fdt-host-test: PASS (%u CPUs, gic %llx/%llx)\n",
	       info.cpu_count,
	       (unsigned long long)info.gic_dist_base,
	       (unsigned long long)info.gic_cpu_base);

	/* Succeeded. */
	return 0;
}

/*
 * Reads one big-endian word of the blob.
 */
static unsigned long
read_word(
	const unsigned char *blob,
	unsigned long offset)
{
	unsigned long word;

	/* Assembles the word from its most significant byte down. */
	word = (unsigned long)blob[offset] << 24;
	word |= (unsigned long)blob[offset + 1U] << 16;
	word |= (unsigned long)blob[offset + 2U] << 8;
	word |= (unsigned long)blob[offset + 3U];

	/* Succeeded: the word. */
	return word;
}

/*
 * Overwrites a node and its subnodes with NOP tokens, as QEMU's
 * fdt_nop_node() does.
 */
static int
nop_node(
	unsigned char *blob,
	size_t length,
	unsigned long offset)
{
	unsigned long position;
	unsigned long token;
	unsigned long size;
	unsigned long depth;
	unsigned long word;

	/* Walks the node's tokens until the end-node token that closes it. */
	position = offset;
	depth = 0;
	for (;;) {
		/* Refuses a node that runs past the blob. */
		if (position + 4U > length)
			return -1;

		/* Steps over one token and whatever it carries. */
		token = read_word(blob, position);
		position += 4U;
		if (token == TOKEN_BEGIN_NODE) {
			/* A node's name, NUL-terminated and padded to a word. */
			while (position < length && blob[position] != 0)
				position++;
			position = (position + 4U) & ~3UL;
			depth++;
		} else if (token == TOKEN_PROPERTY) {
			/* A property's length and name offset, then its padded value. */
			if (position + 8U > length)
				return -1;
			size = read_word(blob, position);
			position += 8U + ((size + 3U) & ~3UL);
		} else if (token == TOKEN_END_NODE) {
			/* The end of a node: the walk stops at the outermost one. */
			depth--;
			if (depth == 0)
				break;
		} else if (token != TOKEN_NOP) {
			/* Any other token cannot appear inside a node. */
			return -1;
		}
	}

	/* Writes a NOP token over every word of the node. */
	for (word = offset; word < position; word += 4U) {
		blob[word] = 0;
		blob[word + 1U] = 0;
		blob[word + 2U] = 0;
		blob[word + 3U] = (unsigned char)TOKEN_NOP;
	}

	/* Succeeded: the node is gone from the tree. */
	return 0;
}

/*
 * Removes every node of a kind from the tree, as raspi4_modify_dtb() does.
 */
static int
remove_nodes(
	unsigned char *blob,
	size_t length,
	const char *compatible)
{
	struct drv_fdt fdt;
	uint32_t node;
	int error;

	/* Opens the tree, which an earlier removal leaves the same size. */
	error = drv_fdt_open(&fdt, blob, length);
	if (error != 0)
		return -1;

	/* Removes the first remaining node of the kind until none is left. */
	for (;;) {
		/* Finds the next node; the walk reaching the end is done. */
		error = drv_fdt_find_compatible(&fdt, compatible, DRV_FDT_NO_NODE, &node);
		if (error == ENOENT)
			break;
		if (error != 0)
			return -1;

		/* Overwrites it with NOP tokens. */
		error = nop_node(blob, length, node);
		if (error != 0)
			return -1;
	}

	/* Succeeded: no node of the kind is left. */
	return 0;
}

/*
 * Checks the drivers' test for QEMU's raspi4b on the board's tree, on the
 * tree QEMU makes of it, and on a tree that has lost the PCIe node alone.
 */
static int
check_board(
	const unsigned char *blob,
	size_t length)
{
	struct drv_fdt fdt;
	unsigned index;
	uint32_t node;
	bool emulated;
	int failures;
	int error;

	/* The board's tree as the firmware's file holds it is a board's. */
	failures = 0;
	error = drv_fdt_open(&fdt, blob, length);
	failures += check(error == 0, "the drivers open the board's tree");
	if (error != 0)
		return failures;
	emulated = drv_rpi4_board_emulated(&fdt);
	failures += check(!emulated, "the board's tree is not taken for QEMU's");

	/* QEMU's tree: the four kinds of node it does not emulate are removed. */
	memcpy(scene_blob, blob, length);
	for (index = 0; index < 4U; index++) {
		error = remove_nodes(scene_blob, length, qemu_removed[index]);
		failures += check(error == 0, "a node QEMU removes is overwritten");
	}

	/* The drivers see QEMU's tree as QEMU's, still with its GPIO and PWM. */
	error = drv_fdt_open(&fdt, scene_blob, length);
	failures += check(error == 0, "the drivers open QEMU's tree");
	if (error != 0)
		return failures;
	error = drv_fdt_find_compatible(&fdt, "brcm,bcm2711-pcie", DRV_FDT_NO_NODE, &node);
	failures += check(error == ENOENT, "QEMU's tree has no PCIe controller");
	error = drv_fdt_find_compatible(&fdt, "brcm,bcm2711-gpio", DRV_FDT_NO_NODE, &node);
	failures += check(error == 0, "QEMU's tree keeps the GPIO block");
	emulated = drv_rpi4_board_emulated(&fdt);
	failures += check(emulated, "QEMU's tree is taken for QEMU's");

	/* A tree that has lost only the PCIe node is not enough for QEMU's. */
	memcpy(scene_blob, blob, length);
	error = remove_nodes(scene_blob, length, "brcm,bcm2711-pcie");
	failures += check(error == 0, "the PCIe node alone is overwritten");
	error = drv_fdt_open(&fdt, scene_blob, length);
	failures += check(error == 0, "the drivers open the tree without PCIe");
	if (error != 0)
		return failures;
	emulated = drv_rpi4_board_emulated(&fdt);
	failures += check(!emulated, "a tree without PCIe alone is not taken for QEMU's");

	/* Succeeded: the number of failed checks. */
	return failures;
}
