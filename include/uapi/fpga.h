/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The programmable logic of a Zynq-7000: /dev/fpga0.
 *
 * A program writes a bitstream to the device in the order the PS's
 * configuration DMA takes it (a .bin: the words of a .bit with their bytes
 * reversed, so the sync word 0xAA995566 reads as a little-endian word) and
 * asks FPGA_PROGRAM to write it into the logic.  Once the logic is
 * configured and enabled (the level shifters open and its resets
 * released), the registers of the block it holds are read and written one
 * 32-bit word at a time in the window the device names, and the block's
 * interrupt is waited for with FPGA_IRQ_WAIT.  Only root opens the device.
 */

#ifndef KERN_UAPI_FPGA_H
#define KERN_UAPI_FPGA_H

#include <stdint.h>
#include <uapi/ioctl.h>

#define KERN_FPGA_IOC_GROUP		'F'

/* The largest bitstream the device keeps (the 7Z010's whole one is about 2.08 MB). */
#define FPGA_BITSTREAM_MAX		(4U * 1024U * 1024U)

/* What fpga_status.flags says of the logic. */
#define FPGA_STATUS_DONE		0x00000001U	/* the logic is configured */
#define FPGA_STATUS_INIT		0x00000002U	/* the configuration logic is ready */
#define FPGA_STATUS_ENABLED		0x00000004U	/* level shifters open, resets released, window usable */
#define FPGA_STATUS_PROGRAMMING		0x00000008U	/* a bitstream is being written */
#define FPGA_STATUS_EMULATED		0x00000010U	/* the emulator's model: the waits on the logic were skipped */

/* What fpga_status.error says of the last FPGA_PROGRAM: 0, or an errno. */

/*
 * The state of the logic and of the device: the flags above, the errno of
 * the last FPGA_PROGRAM, the configuration interface's raw status
 * register, and the window the registers of the block are reached in.
 */
struct fpga_status {
	uint32_t flags;
	uint32_t error;
	uint32_t devcfg_status;
	uint32_t window_base;
	uint32_t window_size;
};

/* One register of the block: its offset in the window (4-byte aligned) and its value. */
struct fpga_reg {
	uint32_t offset;
	uint32_t value;
};

/*
 * A wait for the block's interrupt: how long to wait, in milliseconds,
 * and, on return, how many times the interrupt came since the last wait
 * returned.
 */
struct fpga_irq_wait {
	uint32_t timeout_ms;
	uint32_t count;
};

/*
 * PROGRAM writes the bitstream written to this open of the device into
 * the logic and enables it, after any program in progress (EINVAL
 * without a sync word, EBUSY while the configuration DMA is busy,
 * ETIMEDOUT when the logic does not answer, EIO on a configuration
 * error).  STATUS reports the state.  ENABLE opens the
 * logic's level shifters and releases its resets, once it is configured
 * (EIO otherwise); DISABLE closes the window and holds the logic in
 * reset.  REG_READ and REG_WRITE reach one register (EIO unless enabled,
 * EINVAL outside the window).  IRQ_WAIT waits for the interrupt (EIO
 * unless enabled, EBUSY while another waits, ETIMEDOUT, EINTR).
 */
#define FPGA_PROGRAM		_IO(KERN_FPGA_IOC_GROUP, 0)
#define FPGA_STATUS		_IOR(KERN_FPGA_IOC_GROUP, 1, struct fpga_status)
#define FPGA_ENABLE		_IO(KERN_FPGA_IOC_GROUP, 2)
#define FPGA_DISABLE		_IO(KERN_FPGA_IOC_GROUP, 3)
#define FPGA_REG_READ		_IOWR(KERN_FPGA_IOC_GROUP, 4, struct fpga_reg)
#define FPGA_REG_WRITE		_IOW(KERN_FPGA_IOC_GROUP, 5, struct fpga_reg)
#define FPGA_IRQ_WAIT		_IOWR(KERN_FPGA_IOC_GROUP, 6, struct fpga_irq_wait)

#endif
