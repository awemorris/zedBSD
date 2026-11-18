/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Parses a Raspberry Pi 5 device tree with the HAL's parser, built for the
 * Pi 5, on the host, and checks what the boot needs from it: the
 * peripherals above 4 GiB, the memory, the secure firmware's reservation
 * and the four PSCI CPUs.  It runs on both of the board's trees, the C1
 * stepping's and the D0 stepping's.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hal/hal.h>
#include "hal/arm64/fdt.h"

/* The largest blob the test reads. */
#define BLOB_LIMIT		(2U * 1024U * 1024U)

/* Where the boot finds the debug UART, the mailbox, the GIC and the SD controller. */
#define EXPECT_UART		0x107d001000ULL
#define EXPECT_MAILBOX		0x107c013880ULL
#define EXPECT_GIC_DIST		0x107fff9000ULL
#define EXPECT_GIC_CPU		0x107fffa000ULL
#define EXPECT_SDHCI		0x1000fff000ULL

/* The SD controller's interrupt: shared line 273, after the 32 private ones. */
#define EXPECT_SDHCI_IRQ	(273U + 32U)

/* The secure firmware's memory at the start of RAM. */
#define EXPECT_ATF_SIZE		0x80000ULL

static int check(int condition, const char *what);
static int reserves_atf(const struct arm64_fdt_info *info);

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
		fprintf(stderr, "usage: rpi5-fdt-host-test bcm2712-rpi-5-b.dtb\n");
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
		fprintf(stderr, "rpi5-fdt-host-test: FAIL: parse: %s\n", arm64_fdt_error(error));
		return 1;
	}

	/* The board and the peripherals the boot maps. */
	failures = 0;
	failures += check(info.compatible_board, "the tree is a Raspberry Pi 5's");
	failures += check(info.uart_base == EXPECT_UART, "the debug UART is uart10");
	failures += check(info.uart_irq != 0, "the debug UART has an interrupt");
	failures += check(info.mailbox_base == EXPECT_MAILBOX, "the mailbox is found");
	failures += check(info.gic_dist_base == EXPECT_GIC_DIST, "the GIC distributor is found");
	failures += check(info.gic_cpu_base == EXPECT_GIC_CPU, "the GIC CPU interface is found");
	failures += check(info.sdhci_base == EXPECT_SDHCI, "the SD controller is sdio1");
	failures += check(info.sdhci_irq == EXPECT_SDHCI_IRQ, "the SD controller's interrupt");

	/* The memory node and the secure firmware's reservation. */
	failures += check(info.memory_count >= 1, "a memory range is listed");
	failures += check(info.memory[0].base == 0, "RAM starts at zero");
	failures += check(reserves_atf(&info), "the secure firmware's memory is reserved");

	/* The four cores: affinity in Aff1, brought up through PSCI, not a spin table. */
	failures += check(info.cpu_count == 4, "four CPUs are listed");
	for (index = 0; index < info.cpu_count && index < 4; index++) {
		failures += check(info.cpus[index].mpidr == (unsigned long long)index << 8,
				  "a CPU's affinity is its index in Aff1");
		failures += check(info.cpus[index].spin_table == 0, "a CPU is not held in a spin table");
		failures += check(info.cpus[index].psci != 0, "a CPU is started through PSCI");
	}

	/* Reports a failed tree. */
	if (failures != 0)
		return 1;

	/* Reports what the boot will use. */
	printf("rpi5-fdt-host-test: PASS (%s: uart %llx irq %u, sd %llx, %u CPUs, %u reserved)\n",
	       argv[1],
	       (unsigned long long)info.uart_base,
	       info.uart_irq,
	       (unsigned long long)info.sdhci_base,
	       info.cpu_count,
	       info.reserved_count);

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

	fprintf(stderr, "rpi5-fdt-host-test: FAIL: %s\n", what);

	/* Failed: one more failure. */
	return 1;
}

/* Tells whether the secure firmware's first 512 KiB is among the reserved ranges. */
static int
reserves_atf(
	const struct arm64_fdt_info *info)
{
	unsigned index;

	/* Looks for the range at zero that covers the firmware. */
	for (index = 0; index < info->reserved_count; index++) {
		/* A range elsewhere is another reservation. */
		if (info->reserved[index].base != 0)
			continue;

		/* The range at zero must cover the firmware. */
		if (info->reserved[index].size >= EXPECT_ATF_SIZE)
			return 1;
	}

	/* Failed: no range covers it. */
	return 0;
}
