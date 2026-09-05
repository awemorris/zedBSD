/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_NETWORKD_CONFIRMED_H
#define ZEDBSD_NETWORKD_CONFIRMED_H

#include "userland/base/net/protocol.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef int (*networkd_rollback_callback)(const char *, char *, size_t,
	void *);

struct networkd_confirmed {
	int descriptor;
	char path[NETWORKD_ROLLBACK_PATH_MAX + 1U];
	dev_t device;
	ino_t inode;
	uint64_t deadline;
	uint32_t token;
	uint32_t next_token;
	int active;
};

void networkd_confirmed_init(struct networkd_confirmed *);
void networkd_confirmed_reset(struct networkd_confirmed *);
int networkd_confirmed_arm(struct networkd_confirmed *, const char *, uid_t,
	unsigned, uint64_t, networkd_rollback_callback, void *, uint32_t *,
	char *, size_t);
int networkd_confirmed_disarm(struct networkd_confirmed *, uint32_t);
int networkd_confirmed_check(const struct networkd_confirmed *, uint32_t);
int networkd_confirmed_rollback(struct networkd_confirmed *,
	networkd_rollback_callback, void *, char *, size_t);
int networkd_confirmed_run_due(struct networkd_confirmed *, uint64_t,
	networkd_rollback_callback, void *, char *, size_t);
int networkd_confirmed_poll_timeout(const struct networkd_confirmed *,
	uint64_t);
int networkd_confirmed_active(const struct networkd_confirmed *);

#endif
