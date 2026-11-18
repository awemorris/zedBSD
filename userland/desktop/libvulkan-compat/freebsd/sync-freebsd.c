/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Uses drm-kmod's sync-file records with native FreeBSD ioctl encoding.
 * The ABI follows drm_v6.6.25_13 linuxkpi/bsd/include/uapi/linux/dma-buf.h.
 * Ports do not install that kernel header; no driver implementation is imported.
 */

#include "../dma-sync.h"
#include "../../freebsd-compat/freebsd/dma-sync.h"
#include <stdint.h>
#include <sys/ioccom.h>
#include <sys/ioctl.h>

#define FREEBSD_DMA_EXPORT _IOWR('b', 2, struct compat_freebsd_dma_sync)
#define FREEBSD_DMA_IMPORT _IOW('b', 3, struct compat_freebsd_dma_sync)

/* One native ioctl request; export writes its fd and import only borrows it. */
struct compat_freebsd_dma_sync {
	uint32_t flags;
	int32_t fd;
};

/*
 * Exports the native reservation fences needed before the caller uses a buffer.
 */
int
compat_dma_sync_export(
	int buffer_fd,
	unsigned access,
	int *sync_fd)
{
	struct compat_freebsd_dma_sync request;
	int error;
	int native_error;

	/* Requires the native driver to publish a new descriptor before transferring ownership. */
	request.flags = access;
	request.fd = -1;
	error = ioctl(buffer_fd, FREEBSD_DMA_EXPORT, &request);
	if (error != 0) {
		/* Preserves the failure while recognizing only the native driver's unavailable transport. */
		native_error = errno;
		native_error = kl_freebsd_dma_error(buffer_fd, native_error);
		errno = native_error;
		return error;
	}

	/* Publishes only the output of a successful native export. */
	*sync_fd = request.fd;

	/* Succeeded: the caller owns the driver's exported payload. */
	return 0;
}

/*
 * Attaches a native completion payload while retaining the caller's fd ownership.
 */
int
compat_dma_sync_import(
	int buffer_fd,
	unsigned access,
	int sync_fd)
{
	struct compat_freebsd_dma_sync request;
	int error;
	int native_error;
	int descriptor_flags;

	/* Lets the native driver retain a reference without closing the supplied descriptor. */
	request.flags = access;
	request.fd = sync_fd;
	error = ioctl(buffer_fd, FREEBSD_DMA_IMPORT, &request);
	if (error != 0) {
		/* Preserves the failure while recognizing only the native driver's unavailable transport. */
		native_error = errno;

		/* An invalid borrowed completion fd must retain the original kernel refusal. */
		if (sync_fd >= 0) {
			/* A live borrowed completion is required before recognizing buffer capability. */
			descriptor_flags = fcntl(sync_fd, F_GETFD);
			if (descriptor_flags >= 0)
				native_error = kl_freebsd_dma_error(buffer_fd, native_error);
		}

		/* Reports unsupported transport without publishing a fictitious completion payload. */
		errno = native_error;
		return error;
	}

	/* Succeeded: the reservation owns its payload and the caller still owns its fd. */
	return 0;
}
