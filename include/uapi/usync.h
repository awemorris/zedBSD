/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef KERN_UAPI_USYNC_H
#define KERN_UAPI_USYNC_H

#define KERN_USYNC_WAIT			0U
#define KERN_USYNC_WAKE			1U
#define KERN_USYNC_PRIVATE		0x0001U

/*
 * Make a WAIT observe the calling thread's sticky cancellation request.
 */
#define KERN_USYNC_CANCELABLE		0x0002U

/*
 * Interpret the timeout as an absolute deadline.  CLOCK_MONOTONIC is used
 * unless KERN_USYNC_CLOCK_REALTIME is also present.
 */
#define KERN_USYNC_ABSTIME		0x0004U
#define KERN_USYNC_CLOCK_REALTIME	0x0008U

#endif
