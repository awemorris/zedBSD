/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef KERN_UAPI_THREAD_H
#define KERN_UAPI_THREAD_H

#define KERN_THREAD_SELF_TID		0U
#define KERN_THREAD_SELF_GET_TLS	1U
#define KERN_THREAD_SELF_SET_TLS	2U
#define KERN_THREAD_CANCEL_REQUEST	0U
#define KERN_THREAD_CANCEL_TEST		1U
#define KERN_THREAD_CANCEL_CLEAR	2U

/*
 * Internal thread_join syscall option used by the pthread cancellation
 * wrapper.  Raw non-cancelable join users leave the option clear.
 */
#define KERN_THREAD_JOIN_CANCELABLE	0x0001U

#endif
