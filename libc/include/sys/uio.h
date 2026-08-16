/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_SYS_UIO_H
#define ZEDBSD_SYS_UIO_H

#include <stddef.h>
#include <sys/types.h>

#define IOV_MAX 16
struct iovec {
	void *iov_base;
	size_t iov_len;
};

ssize_t readv(int, const struct iovec *, int);
ssize_t writev(int, const struct iovec *, int);
ssize_t preadv(int, const struct iovec *, int, off_t);
ssize_t pwritev(int, const struct iovec *, int, off_t);

#endif
