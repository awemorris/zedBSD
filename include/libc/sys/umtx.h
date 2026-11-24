/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * User-space mutex operations mapped onto the kernel usync primitive.
 */

#ifndef LIBC_SYS_UMTX_H
#define LIBC_SYS_UMTX_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <time.h>

#define UMTX_OP_WAIT			2
#define UMTX_OP_WAKE			3
#define UMTX_OP_WAIT_UINT		11
#define UMTX_OP_WAIT_UINT_PRIVATE	15
#define UMTX_OP_WAKE_PRIVATE		16

#define UMTX_ABSTIME			0x01U

struct _umtx_time {
	struct timespec _timeout;
	uint32_t _flags;
	uint32_t _clockid;
};

int _umtx_op(void *obj, int op, unsigned long val, void *uaddr, void *uaddr2);

#ifdef __cplusplus
}
#endif

#endif
