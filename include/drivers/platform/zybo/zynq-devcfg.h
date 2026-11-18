/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Zynq-7000's device configuration interface (devcfg) and its PCAP:
 * the driver of /dev/fpga0, which writes a bitstream into the programmable
 * logic and reaches the block the logic holds.
 *
 * The checks the driver makes before it touches the hardware are kept
 * apart from it (zynq-devcfg-check.c) so that a host test runs them.
 */

#ifndef KERN_DRIVERS_PLATFORM_ZYBO_ZYNQ_DEVCFG_H
#define KERN_DRIVERS_PLATFORM_ZYBO_ZYNQ_DEVCFG_H

#include <stddef.h>
#include <stdint.h>

/*
 * The bits of devcfg CTRL the driver may set or clear: PCFG_PROG_B (30),
 * PCAP_PR (27), PCAP_MODE (26) and PCAP_RATE_EN (25).  The register also
 * holds bits that reset the PS or cut the JTAG chain, the board's only way
 * in, so nothing else in it is ever written.
 *
 * Values referenced: Linux v6.12 drivers/fpga/zynq-fpga.c,
 * CTRL_PCFG_PROG_B_MASK, CTRL_PCAP_PR_MASK, CTRL_PCAP_MODE_MASK and
 * CTRL_PCAP_RATE_EN_MASK.
 */
#define DRV_ZYNQ_DEVCFG_CTRL_PCFG_PROG_B	0x40000000U
#define DRV_ZYNQ_DEVCFG_CTRL_PCAP_PR		0x08000000U
#define DRV_ZYNQ_DEVCFG_CTRL_PCAP_MODE		0x04000000U
#define DRV_ZYNQ_DEVCFG_CTRL_PCAP_RATE_EN	0x02000000U
#define DRV_ZYNQ_DEVCFG_CTRL_ALLOWED		0x4e000000U

/*
 * The one value devcfg UNLOCK is ever written with; the interface takes
 * another as a reason to close itself until the next reset.
 *
 * Values referenced: Linux v6.12 drivers/fpga/zynq-fpga.c, UNLOCK_MASK.
 */
#define DRV_ZYNQ_DEVCFG_UNLOCK_KEY		0x757bdf0dU

/*
 * How far into a bitstream its sync word is looked for: past the pad and
 * the bus width detection, which take a few dozen bytes.
 */
#define DRV_ZYNQ_DEVCFG_SYNC_SEARCH		4096U

/*
 * Starts the driver of /dev/fpga0: maps the configuration interface,
 * installs the logic's interrupt and publishes the device.  The logic
 * itself is left as it is.
 */
int drv_zynq_devcfg_attach(void);

/*
 * Reports whether a request to set and clear bits of CTRL stays inside
 * DRV_ZYNQ_DEVCFG_CTRL_ALLOWED and asks for something: 1 when it does.
 */
int drv_zynq_devcfg_ctrl_request_valid(uint32_t set, uint32_t clear);

/*
 * Reports whether a bitstream in the configuration DMA's order carries the
 * sync word in its first DRV_ZYNQ_DEVCFG_SYNC_SEARCH bytes, at a word
 * boundary: 1 when it does.
 */
int drv_zynq_devcfg_has_sync(const uint8_t *data, size_t size);

/*
 * Reports whether the DMA can take a buffer: its address 8-byte aligned,
 * below 4 GiB, its length a nonzero multiple of four: 1 when it can.
 */
int drv_zynq_devcfg_dma_valid(uint64_t physical, size_t length);

#endif
