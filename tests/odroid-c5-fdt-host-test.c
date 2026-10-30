/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Parses the ODROID-C5's device tree with the HAL's parser, built for the
 * ODROID-C5, on the host, and checks what the boot needs from it: the debug
 * UART and the GIC, the usable memory, the secure monitor's reservation
 * (and not the reservations whose status is off), the four PSCI CPUs, and
 * the boot script's framebuffer.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hal/hal.h>
#include "hal/arm64/fdt.h"

/* The largest blob the test reads. */
#define BLOB_LIMIT		(2U * 1024U * 1024U)

/* Where the boot finds the debug UART and the GIC. */
#define EXPECT_UART		0xfe07a000ULL
#define EXPECT_GIC_DIST		0xfff01000ULL
#define EXPECT_GIC_CPU		0xfff02000ULL

/* The debug UART's interrupt: shared line 169, after the 32 private ones. */
#define EXPECT_UART_IRQ		(169U + 32U)

/* The RAM the tree lets the system use: the first 2 GiB. */
#define EXPECT_USABLE		0x80000000ULL

/* The secure monitor's memory, and the dump area whose status is off. */
#define EXPECT_SECMON_BASE	0x05000000ULL
#define EXPECT_SECMON_SIZE	0x02400000ULL
#define DISABLED_DUMP_SIZE	0x01800000ULL

/* The boot script's framebuffer. */
#define EXPECT_FB_BASE		0x7f800000ULL
#define EXPECT_FB_WIDTH		1920U
#define EXPECT_FB_HEIGHT	1080U
#define EXPECT_FB_STRIDE	7680U

static int check(int condition, const char *what);
static int reserves(const struct arm64_fdt_info *info, unsigned long long base, unsigned long long size);

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

/*
 * Runs the test on the device tree blob named on the command line.
 */
int
main(
	int argc,
	char **argv)
{
	static unsigned char blob[BLOB_LIMIT];
	struct arm64_fdt_info info;
	FILE *file;
	size_t length;
	unsigned index;
	int failures;
	int error;

	/* Requires the blob's path. */
	if (argc != 2) {
		fprintf(stderr, "usage: odroid-c5-fdt-host-test odroid-c5.dtb\n");
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
		fprintf(stderr, "odroid-c5-fdt-host-test: FAIL: parse: %s\n", arm64_fdt_error(error));
		return 1;
	}

	/* The board and the peripherals the boot maps. */
	failures = 0;
	failures += check(info.compatible_board, "the tree is an ODROID-C5's");
	failures += check(info.uart_base == EXPECT_UART, "the debug UART is uart_B");
	failures += check(info.uart_irq == EXPECT_UART_IRQ, "the debug UART's interrupt");
	failures += check(info.gic_dist_base == EXPECT_GIC_DIST, "the GIC distributor is found");
	failures += check(info.gic_cpu_base == EXPECT_GIC_CPU, "the GIC CPU interface is found");
	failures += check(info.mailbox_base == 0, "no mailbox is named");

	/* The usable memory and the reservations. */
	failures += check(info.memory_count == 1, "one memory range is listed");
	failures += check(info.memory[0].base == 0, "RAM starts at zero");
	failures += check(info.memory[0].size == EXPECT_USABLE, "the usable memory wins over reg");
	failures += check(reserves(&info, EXPECT_SECMON_BASE, EXPECT_SECMON_SIZE),
			  "the secure monitor's memory is reserved");
	failures += check(!reserves(&info, 0, DISABLED_DUMP_SIZE), "a reservation whose status is off is left out");
	failures += check(info.no_map_count >= 1 &&
			  info.no_map[0].base == EXPECT_SECMON_BASE && info.no_map[0].size == EXPECT_SECMON_SIZE,
			  "the secure monitor's memory is kept out of the direct map (no-map)");

	/* The early framebuffer is drawn on through the boot's tables, which map the first 2 GiB. */
	failures += check(info.framebuffer_base + info.framebuffer_size <= 0x80000000ULL,
			  "the framebuffer lies in the boot's mapped RAM");

	/* The four cores: affinity in Aff1, brought up through PSCI, not a spin table. */
	failures += check(info.cpu_count == 4, "four CPUs are listed");
	for (index = 0; index < info.cpu_count && index < 4; index++) {
		failures += check(info.cpus[index].mpidr == (unsigned long long)index << 8,
				  "a CPU's affinity is its index in Aff1");
		failures += check(info.cpus[index].spin_table == 0, "a CPU is not held in a spin table");
		failures += check(info.cpus[index].psci != 0, "a CPU is started through PSCI");
	}

	/* The boot script's framebuffer: ARGB, blue in the low byte, alpha in the top. */
	failures += check(info.framebuffer_base == EXPECT_FB_BASE, "the framebuffer's base");
	failures += check(info.framebuffer_size >= (unsigned long long)EXPECT_FB_STRIDE * EXPECT_FB_HEIGHT,
			  "the framebuffer holds the screen");
	failures += check(info.framebuffer_width == EXPECT_FB_WIDTH, "the framebuffer's width");
	failures += check(info.framebuffer_height == EXPECT_FB_HEIGHT, "the framebuffer's height");
	failures += check(info.framebuffer_stride == EXPECT_FB_STRIDE, "the framebuffer's row length");
	failures += check(info.framebuffer_red_low == 0, "the framebuffer has blue in the low byte");
	failures += check(info.framebuffer_alpha == 1, "the framebuffer blends by its alpha");

	/* Reports a failed tree. */
	if (failures != 0)
		return 1;

	/* Reports what the boot will use. */
	printf("odroid-c5-fdt-host-test: PASS (%s: uart %llx irq %u, %u CPUs, %u reserved, fb %llx)\n",
	       argv[1],
	       (unsigned long long)info.uart_base,
	       info.uart_irq,
	       info.cpu_count,
	       info.reserved_count,
	       (unsigned long long)info.framebuffer_base);

	/* Succeeded. */
	return 0;
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

	fprintf(stderr, "odroid-c5-fdt-host-test: FAIL: %s\n", what);

	/* Failed: one more failure. */
	return 1;
}

/* Tells whether a reserved range covers the given one. */
static int
reserves(
	const struct arm64_fdt_info *info,
	unsigned long long base,
	unsigned long long size)
{
	unsigned index;

	/* Looks for a reservation that holds the whole range. */
	for (index = 0; index < info->reserved_count; index++) {
		if (info->reserved[index].base <= base &&
		    info->reserved[index].base + info->reserved[index].size >= base + size)
			return 1;
	}

	/* Succeeded: no reservation holds it. */
	return 0;
}
