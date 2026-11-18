/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Transfers DMA-BUF synchronization payloads through the selected native ABI.
 */

#ifndef KEILAND_DMA_SYNC_H
#define KEILAND_DMA_SYNC_H

#define COMPAT_DMA_SYNC_READ 1U
#define COMPAT_DMA_SYNC_WRITE 2U

/* Export success gives the caller a descriptor; failure preserves its output and errno. */
int compat_dma_sync_export(int buffer_fd, unsigned access, int *sync_fd);

/* Import retains the caller's descriptor on both success and failure. */
int compat_dma_sync_import(int buffer_fd, unsigned access, int sync_fd);

#endif
