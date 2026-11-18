/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Fatal error reporting.
 *
 * A driver states a condition it cannot continue from. The kernel
 * records the site and stops the machine; it never returns.
 */

#ifndef KERN_PANIC_H
#define KERN_PANIC_H

__attribute__((noreturn))
void kern_fatal(const char *file, int line, const char *message);

/* States a condition the caller cannot continue from. */
#define KERN_FATAL(message) kern_fatal(__FILE__, __LINE__, (message))

#endif
