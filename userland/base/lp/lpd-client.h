/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_USERLAND_LP_LPD_CLIENT_H
#define ZEDBSD_USERLAND_LP_LPD_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#define LPD_HOST_MAX 255
#define LPD_QUEUE_MAX 63
#define LPD_SERVICE_MAX 5

struct lpd_destination {
	char host[LPD_HOST_MAX + 1];
	char queue[LPD_QUEUE_MAX + 1];
	char service[LPD_SERVICE_MAX + 1];
};

int lpd_parse_destination(const char *text, struct lpd_destination *result);
int lpd_submit(const struct lpd_destination *destination, int input,
	uint64_t size, const char *host, const char *user, const char *title,
	const char *source_name, unsigned copies, int mail, unsigned sequence);

#endif
