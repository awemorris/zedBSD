/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_UAPI_USYNC_H
#define ZEDBSD_UAPI_USYNC_H
#define ZEDBSD_USYNC_WAIT 0U
#define ZEDBSD_USYNC_WAKE 1U
#define ZEDBSD_USYNC_PRIVATE 0x0001U
/* Make a WAIT observe the calling thread's sticky cancellation request. */
#define ZEDBSD_USYNC_CANCELABLE 0x0002U
#endif
