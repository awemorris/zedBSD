/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The legacy PC-98 system-DMA contract.
 */

#ifndef ZEDBSD_HAL_I386_PC98_DMA_H
#define ZEDBSD_HAL_I386_PC98_DMA_H

#include <hal/hal.h>

/* DMA transfer direction. */
#define HAL_DMA_CPU_TO_DEVICE	(1)	/* CPU -> Device */
#define HAL_DMA_DEVICE_TO_CPU	(2)	/* Device -> CPU */

/* Allocate a cache-coherent memory block for DMA. */
int hal_dma_alloc(size_t size, struct hal_pmem *desc);

/* Free a cache-coherent memory block. */
void hal_dma_free(struct hal_pmem *desc);

/* Sync memory (writeback). */
void hal_dma_writeback(struct hal_pmem *desc, uintptr_t addr, size_t size);

/* Sync memory (invalidate). */
void hal_dma_invalidate(struct hal_pmem *desc, uintptr_t addr, size_t size);

/* Start a system DMA channel. (legacy and simple) */
int hal_dma_kick(int channel, void *paddr, uintptr_t dev_fifo_addr, size_t size, int direction);

/* Check if a DMA transfer is finished. (optional) */
bool hal_dma_check(int channel);

/* Wait for a DMA transfer to finish. (optional) */
bool hal_dma_wait(int channel);

/* Stop the DMA channel execution. (optional) */
void hal_dma_cancel(int channel);

#endif
