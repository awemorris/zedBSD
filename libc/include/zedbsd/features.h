/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_FEATURES_H
#define ZEDBSD_FEATURES_H

/*
 * Application feature selections are inputs and are never overwritten here.
 * Internal visibility names let public headers distinguish the Issue 8
 * namespace from the default zedBSD extension namespace.
 */
#if defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 202405L
#define __ZEDBSD_POSIX_2024_VISIBLE	1
#elif defined(_XOPEN_SOURCE) && _XOPEN_SOURCE >= 800
#define __ZEDBSD_POSIX_2024_VISIBLE	1
#else
#define __ZEDBSD_POSIX_2024_VISIBLE	0
#endif

#if !defined(_POSIX_C_SOURCE) && !defined(_XOPEN_SOURCE)
#define __ZEDBSD_LEGACY_VISIBLE	1
#elif defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE < 202405L
#define __ZEDBSD_LEGACY_VISIBLE	1
#elif defined(_XOPEN_SOURCE) && _XOPEN_SOURCE < 800
#define __ZEDBSD_LEGACY_VISIBLE	1
#else
#define __ZEDBSD_LEGACY_VISIBLE	0
#endif

/* POSIX.1-2024 system interfaces; POSIX.2 utilities remain Issue 7. */
#define _POSIX_VERSION 202405L
#define _POSIX2_VERSION 200809L
#define _XOPEN_VERSION 700
#define _XOPEN_UNIX 1
#define _POSIX_JOB_CONTROL 1
#define _POSIX_THREADS 200809L
#define _POSIX_THREAD_ATTR_STACKSIZE 200809L
#define _POSIX_THREAD_PROCESS_SHARED 200809L
#define _POSIX_REALTIME_SIGNALS 200809L
#define _POSIX_SHARED_MEMORY_OBJECTS 200809L
#define _POSIX_SEMAPHORES 200809L
#define _POSIX_MESSAGE_PASSING 200809L
#define _POSIX_DEVICE_CONTROL 202405L
#define _POSIX_THREAD_SAFE_FUNCTIONS (-1)
#define _POSIX_THREAD_PRIO_INHERIT (-1)
#define _POSIX_THREAD_PRIO_PROTECT (-1)
#define _POSIX_ASYNCHRONOUS_IO (-1)
#define _POSIX_PRIORITIZED_IO (-1)
#define _POSIX_TIMERS 200809L
#define _POSIX_MONOTONIC_CLOCK 200809L
#define _POSIX_TYPED_MEMORY_OBJECTS (-1)

#endif
