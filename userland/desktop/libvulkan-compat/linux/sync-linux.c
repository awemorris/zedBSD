/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Uses Linux's DMA-BUF sync-file ABI without exposing its records to shared WSI.
 */

#include "../dma-sync.h"
#include <linux/dma-buf.h>
#include <string.h>
#include <sys/ioctl.h>

/*
 * Exports the reservation fences needed by the caller's next buffer access.
 */
int
compat_dma_sync_export(
	int buffer_fd,
	unsigned access,
	int *sync_fd)
{
	struct dma_buf_export_sync_file request;
	int error;

	/* Requests a new sync descriptor without changing the caller's output on failure. */
	memset(&request, 0, sizeof(request));
	request.flags = access;
	request.fd = -1;
	error = ioctl(buffer_fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &request);
	if (error != 0)
		return error;

	/* Transfers the successfully exported descriptor to the caller. */
	*sync_fd = request.fd;

	/* Succeeded: the caller owns the exported reservation payload. */
	return 0;
}

/*
 * Attaches the caller's completion payload without consuming its descriptor.
 */
int
compat_dma_sync_import(
	int buffer_fd,
	unsigned access,
	int sync_fd)
{
	struct dma_buf_import_sync_file request;
	int error;

	/* Lets the kernel retain its own reference to the supplied sync payload. */
	memset(&request, 0, sizeof(request));
	request.flags = access;
	request.fd = sync_fd;
	error = ioctl(buffer_fd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &request);
	if (error != 0)
		return error;

	/* Succeeded: the buffer retains the payload and the caller still owns its fd. */
	return 0;
}
