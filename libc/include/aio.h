/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_AIO_H
#define ZEDBSD_AIO_H

#include <signal.h>
#include <stddef.h>
#include <sys/types.h>
#include <time.h>

#define AIO_CANCELED    0
#define AIO_NOTCANCELED 1
#define AIO_ALLDONE     2

#define LIO_READ   0
#define LIO_WRITE  1
#define LIO_NOP    2
#define LIO_WAIT   0
#define LIO_NOWAIT 1

struct aiocb {
	int aio_fildes;
	off_t aio_offset;
	volatile void *aio_buf;
	size_t aio_nbytes;
	int aio_reqprio;
	struct sigevent aio_sigevent;
	int aio_lio_opcode;
	/* Private completion state.  Applications must not inspect these. */
	ssize_t __aio_result;
	int __aio_error;
	unsigned __aio_submitted;
	unsigned __aio_returned;
};

int aio_cancel(int, struct aiocb *);
int aio_error(const struct aiocb *);
int aio_fsync(int, struct aiocb *);
int aio_read(struct aiocb *);
ssize_t aio_return(struct aiocb *);
int aio_suspend(const struct aiocb *const [], int, const struct timespec *);
int aio_write(struct aiocb *);
int lio_listio(int, struct aiocb *restrict const [restrict], int,
	struct sigevent *restrict);

#endif
