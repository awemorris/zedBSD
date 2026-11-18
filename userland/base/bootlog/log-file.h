/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the log files bootlog keeps and the reading of the kernel log.
 */

#ifndef KERN_USERLAND_BOOTLOG_LOG_FILE_H
#define KERN_USERLAND_BOOTLOG_LOG_FILE_H

#include <stddef.h>
#include <stdint.h>

/* The largest boot number a log file name carries (boot-999999.txt). */
#define BOOTLOG_NUMBER_MAX 999999UL

/* Room for a log file name with its terminator. */
#define BOOTLOG_NAME_SIZE 32U

/*
 * The longest trigger word less one: a scan starts this many bytes before
 * the text it has not seen, so a word split between two scans is found.
 */
#define BOOTLOG_TRIGGER_OVERLAP 15U

int bootlog_name_number(const char *name, unsigned long *number);
int bootlog_name_format(unsigned long number, char *name, size_t size);
int bootlog_file_create(const char *directory, unsigned keep, unsigned long *number);
int bootlog_file_append(int descriptor, const char *header, const char *text, size_t length);
size_t bootlog_unseen_offset(uint64_t seen_total, uint64_t dropped, size_t length, uint64_t *lost);
const char *bootlog_trigger_find(const char *text, size_t length);

#endif
