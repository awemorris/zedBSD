/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Exports Linux reservation fences without exposing kernel records to shared rendering. */
#include "userland/desktop/libkeiland-backend/dmabuf/sync.h"
#include <linux/dma-buf.h>
#include <string.h>
#include <sys/ioctl.h>

/*
 * Exports the writers that must finish before the compositor samples the buffer.
 */
int
kl_backend_dmabuf_export_read(
	int buffer_fd,
	int *sync_fd)
{
	struct dma_buf_export_sync_file request;
	int error;

	/* Borrows the buffer and requests a separately owned reservation payload. */
	memset(&request, 0, sizeof(request));
	request.flags = DMA_BUF_SYNC_READ;
	request.fd = -1;
	error = ioctl(buffer_fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &request);
	if (error != 0)
		return error;

	/* Publishes only the descriptor actually exported by the native kernel. */
	*sync_fd = request.fd;

	/* Succeeded: common commit cleanup may now own the exported payload. */
	return 0;
}
