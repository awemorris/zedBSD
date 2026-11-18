/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Exports drm-kmod reservations with native FreeBSD ioctl encoding.
 * The record follows fixed drm_v6.6.25_13; no driver implementation is imported.
 */
#include "userland/desktop/libkeiland-backend/dmabuf/sync.h"
#include "../freebsd-compat/freebsd/dma-sync.h"
#include <stdint.h>
#include <sys/ioccom.h>
#include <sys/ioctl.h>

#define FREEBSD_DMA_EXPORT _IOWR('b', 2, struct kwl_freebsd_dma_sync)
#define FREEBSD_DMA_READ 1U

/* One native export request borrows the buffer and receives an owned sync descriptor. */
struct kwl_freebsd_dma_sync {
	uint32_t flags;
	int32_t fd;
};

/*
 * Exports the native writers that must finish before the compositor samples a buffer.
 */
int
kl_backend_dmabuf_export_read(
	int buffer_fd,
	int *sync_fd)
{
	struct kwl_freebsd_dma_sync request;
	int error;
	int native_error;

	/* The native ioctl owns its request until success publishes a new descriptor. */
	request.flags = FREEBSD_DMA_READ;
	request.fd = -1;
	error = ioctl(buffer_fd, FREEBSD_DMA_EXPORT, &request);
	if (error != 0) {
		/* Preserves the failure while recognizing only the native driver's unavailable transport. */
		native_error = errno;
		native_error = kl_freebsd_dma_error(buffer_fd, native_error);
		errno = native_error;
		return error;
	}

	/* Publishes only the driver's successful reservation payload. */
	*sync_fd = request.fd;

	/* Succeeded: common commit cleanup owns the exported native sync descriptor. */
	return 0;
}
