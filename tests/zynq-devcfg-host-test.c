/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host check of the devcfg driver's checks
 * (src/drivers/platform/zybo/zynq-devcfg-check.c) and of the layout of
 * include/uapi/fpga.h: CTRL requests outside the configuration's bits are
 * refused (the bits that reset the PS or cut JTAG), the sync word is found
 * only at a word boundary in the DMA's order near the start, the DMA's
 * address and length rules, and the sizes and offsets of the ioctl
 * structures and their request numbers as the zedBSD encoding builds them.
 *
 * cc -std=c11 -Wall -Wextra -Werror -Iinclude -Isrc \
 *    tests/zynq-devcfg-host-test.c \
 *    src/drivers/platform/zybo/zynq-devcfg-check.c -o <out> && <out>
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <drivers/platform/zybo/zynq-devcfg.h>

/* The zedBSD request encoding, not the host's: the structures are the target's. */
#define KERN_UAPI_NATIVE 1
#include <uapi/fpga.h>

/* The number of failed checks. */
static int failures;

static void check(int condition, const char *name);

/*
 * Runs the checks and reports PASS or the failures.
 */
int
main(
	void)
{
	uint8_t data[64];

	/* CTRL: the configuration's bits alone, one way at a time. */
	check(drv_zynq_devcfg_ctrl_request_valid(DRV_ZYNQ_DEVCFG_CTRL_PCFG_PROG_B, 0) == 1, "ctrl PROG_B set");
	check(drv_zynq_devcfg_ctrl_request_valid(0, DRV_ZYNQ_DEVCFG_CTRL_PCAP_PR) == 1, "ctrl PCAP_PR clear");
	check(drv_zynq_devcfg_ctrl_request_valid(DRV_ZYNQ_DEVCFG_CTRL_PCAP_PR | DRV_ZYNQ_DEVCFG_CTRL_PCAP_MODE, 0) == 1, "ctrl PR|MODE");
	check(drv_zynq_devcfg_ctrl_request_valid(0x80000000U, 0) == 0, "ctrl bit 31 refused");
	check(drv_zynq_devcfg_ctrl_request_valid(0, 0x00800000U) == 0, "ctrl bit 23 refused");
	check(drv_zynq_devcfg_ctrl_request_valid(0x00000001U, 0) == 0, "ctrl bit 0 refused");
	check(drv_zynq_devcfg_ctrl_request_valid(DRV_ZYNQ_DEVCFG_CTRL_PCAP_PR | 0x10U, 0) == 0, "ctrl allowed plus other refused");
	check(drv_zynq_devcfg_ctrl_request_valid(0, 0) == 0, "ctrl empty refused");
	check(drv_zynq_devcfg_ctrl_request_valid(DRV_ZYNQ_DEVCFG_CTRL_PCAP_PR, DRV_ZYNQ_DEVCFG_CTRL_PCAP_PR) == 0, "ctrl set and clear refused");
	check(DRV_ZYNQ_DEVCFG_UNLOCK_KEY == 0x757bdf0dU, "unlock key");

	/* The sync word: in the DMA's order at a word boundary, near the start. */
	memset(data, 0xff, sizeof(data));
	data[32] = 0x66;
	data[33] = 0x55;
	data[34] = 0x99;
	data[35] = 0xaa;
	check(drv_zynq_devcfg_has_sync(data, sizeof(data)) == 1, "sync at word 8");
	check(drv_zynq_devcfg_has_sync(data, 32) == 0, "sync past the data");
	memset(data, 0xff, sizeof(data));
	data[33] = 0x66;
	data[34] = 0x55;
	data[35] = 0x99;
	data[36] = 0xaa;
	check(drv_zynq_devcfg_has_sync(data, sizeof(data)) == 0, "sync off a word boundary");
	memset(data, 0xff, sizeof(data));
	data[32] = 0xaa;
	data[33] = 0x99;
	data[34] = 0x55;
	data[35] = 0x66;
	check(drv_zynq_devcfg_has_sync(data, sizeof(data)) == 0, "sync in .bit order refused");
	check(drv_zynq_devcfg_has_sync(NULL, 16) == 0, "sync without data");

	/* The DMA: 8-byte aligned, whole words, below 4 GiB. */
	check(drv_zynq_devcfg_dma_valid(0x01000000ULL, 2083740U) == 1, "dma plio");
	check(drv_zynq_devcfg_dma_valid(0x01000004ULL, 16U) == 0, "dma 4-aligned refused");
	check(drv_zynq_devcfg_dma_valid(0x01000000ULL, 18U) == 0, "dma half word refused");
	check(drv_zynq_devcfg_dma_valid(0x01000000ULL, 0U) == 0, "dma empty refused");
	check(drv_zynq_devcfg_dma_valid(0x100000000ULL, 16U) == 0, "dma above 4 GiB refused");
	check(drv_zynq_devcfg_dma_valid(0xfffffff8ULL, 16U) == 0, "dma crossing 4 GiB refused");

	/* The UAPI: the structures' layout and the request numbers. */
	check(sizeof(struct fpga_status) == 20U, "fpga_status size");
	check(offsetof(struct fpga_status, window_size) == 16U, "fpga_status window_size");
	check(sizeof(struct fpga_reg) == 8U, "fpga_reg size");
	check(offsetof(struct fpga_reg, value) == 4U, "fpga_reg value");
	check(sizeof(struct fpga_irq_wait) == 8U, "fpga_irq_wait size");
	check(FPGA_PROGRAM == 0x00004600UL, "FPGA_PROGRAM");
	check(FPGA_STATUS == 0x40144601UL, "FPGA_STATUS");
	check(FPGA_REG_READ == 0xc0084604UL, "FPGA_REG_READ");
	check(FPGA_REG_WRITE == 0x80084605UL, "FPGA_REG_WRITE");
	check(FPGA_IRQ_WAIT == 0xc0084606UL, "FPGA_IRQ_WAIT");

	/* Reports the result. */
	if (failures != 0) {
		printf("FAIL devcfg-host-test (%d)\n", failures);
		return 1;
	}

	/* Succeeded: every check held. */
	printf("PASS devcfg-host-test\n");
	return 0;
}

/* Counts and names a failed check. */
static void
check(
	int condition,
	const char *name)
{
	/* A check that holds says nothing. */
	if (condition)
		return;

	/* Names the check that failed. */
	printf("not ok %s\n", name);
	failures++;
}
