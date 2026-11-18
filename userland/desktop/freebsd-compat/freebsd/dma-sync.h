/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Classifies drm-kmod's live zeroaccess files without changing their flags or ownership. */
#ifndef KEILAND_FREEBSD_DMA_SYNC_H
#define KEILAND_FREEBSD_DMA_SYNC_H

#include <errno.h>
#include <fcntl.h>

/* Recognizes unavailable native ioctl transport while preserving genuine descriptor errors. */
static __inline int
kl_freebsd_dma_error(
	int descriptor,
	int native_error)
{
	int descriptor_flags;
	int access_flags;
	int query_error;

	/* Other failures cannot be explained by the driver's zeroaccess file construction. */
	if (native_error != EBADF)
		return native_error;

	/* A closed descriptor must retain its original ownership diagnostic. */
	descriptor_flags = fcntl(descriptor, F_GETFD);
	if (descriptor_flags < 0)
		return native_error;

	/* Zero internal access flags produce successful minus-one F_GETFL with no native errno. */
	errno = 0;
	access_flags = fcntl(descriptor, F_GETFL);
	query_error = errno;
	if (query_error != 0)
		return native_error;

	/* Readable, writable and other native files do not have this exact driver fingerprint. */
	if (access_flags != -1)
		return native_error;

	/* Succeeded: existing CPU completion handles this live file's unavailable ioctl transport. */
	return ENOTTY;
}

#endif
