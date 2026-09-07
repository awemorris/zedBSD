/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_IO_STATS_H
#define ZEDBSD_KERN_IO_STATS_H

#include <zedbsd/io-stats.h>

void io_stats_record(enum io_stat_event event, uint64_t bytes);
void io_stats_snapshot(struct io_stats *snapshot);

#endif
