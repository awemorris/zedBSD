/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The DesignWare AXI DMA controller.
 *
 * A controller has a few channels, each walking a chain of descriptors in
 * memory.  The calls here hand out channels, copy memory to memory and
 * run a cyclic transfer from a ring in memory to one register of a
 * device, paced by the device's handshake line, reporting each finished
 * period from the controller's interrupt.
 */

#ifndef KERN_DRIVERS_DMA_DW_AXI_DMAC_H
#define KERN_DRIVERS_DMA_DW_AXI_DMAC_H

#include <stddef.h>
#include <stdint.h>

#include <drivers/generic/dma.h>
#include <drivers/generic/fdt.h>

struct drv_dw_axi_dmac;
struct drv_dw_axi_dmac_channel;

/* The most channels one controller may have. */
#define DRV_DW_AXI_DMAC_CHANNEL_MAX	8U

/* The most periods one cyclic transfer may have. */
#define DRV_DW_AXI_DMAC_PERIOD_MAX	32U

/*
 * Called from the controller's interrupt each time a cyclic transfer has
 * finished one period.  It must not sleep.
 */
typedef void (*drv_dw_axi_dmac_period_t)(void *argument);

/*
 * One cyclic transfer from a ring in memory to a device register.
 *
 * The ring is periods back to back; each period is one block, so it must
 * fit the channel's block size.  Device width is the register's width in
 * bytes (1, 2 or 4); the ring's address and period must be multiples of
 * four bytes.  Handshake is the device's request line, the cell of its
 * dmas property.
 */
struct drv_dw_axi_dmac_cyclic {
	uint64_t ring_address;
	uint32_t period_bytes;
	uint32_t period_count;
	uint64_t device_address;
	uint32_t device_width;
	uint32_t handshake;
	drv_dw_axi_dmac_period_t period_done;
	void *argument;
};

/*
 * Attaches the controller a device tree node describes.
 *
 * The board's glue has already started the controller's clocks and
 * released its resets.  Board default is what DMA coherency the board
 * assumes when no dma-coherent or dma-noncoherent property says
 * otherwise.  The controller stays attached for good.
 */
int drv_dw_axi_dmac_attach(const struct drv_fdt *fdt, uint32_t node, int board_default_coherent, struct drv_dw_axi_dmac **controller);

/*
 * Reports the DMA owner of the controller, whose buffers have addresses
 * the controller can use.
 */
struct drv_dma_device *drv_dw_axi_dmac_dma_device(struct drv_dw_axi_dmac *controller);

/*
 * Takes a free channel for one user.
 *
 * EBUSY means every channel is taken.
 */
int drv_dw_axi_dmac_channel_get(struct drv_dw_axi_dmac *controller, struct drv_dw_axi_dmac_channel **channel);

/*
 * Gives a channel back.  The channel must be idle.
 */
void drv_dw_axi_dmac_channel_put(struct drv_dw_axi_dmac_channel *channel);

/*
 * Copies memory to memory and waits for the copy to end.
 *
 * Addresses are the controller's (from its DMA owner).  Runs in thread
 * context.  ETIMEDOUT means the copy did not end within a second; EIO
 * that the controller reported an error.
 */
int drv_dw_axi_dmac_copy(struct drv_dw_axi_dmac_channel *channel, uint64_t destination, uint64_t source, size_t size);

/*
 * Builds the descriptors of a cyclic transfer without starting it.
 */
int drv_dw_axi_dmac_cyclic_prepare(struct drv_dw_axi_dmac_channel *channel, const struct drv_dw_axi_dmac_cyclic *cyclic);

/*
 * Starts a prepared cyclic transfer from its first period.
 */
int drv_dw_axi_dmac_cyclic_start(struct drv_dw_axi_dmac_channel *channel);

/*
 * Stops a cyclic transfer and waits until the channel is idle.
 *
 * After it returns the period callback is no longer called.
 */
void drv_dw_axi_dmac_cyclic_stop(struct drv_dw_axi_dmac_channel *channel);

/*
 * Reports the byte offset in the ring of the period the channel works
 * on.
 */
uint32_t drv_dw_axi_dmac_cyclic_position(struct drv_dw_axi_dmac_channel *channel);

#endif
