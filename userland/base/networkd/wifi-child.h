/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the bounded networkd Wi-Fi child runner.
 */

#ifndef ZEDBSD_NETWORKD_WIFI_CHILD_H
#define ZEDBSD_NETWORKD_WIFI_CHILD_H

#include "userland/base/net/protocol.h"

#include <stddef.h>

/*
 * Reserves the two required ZNV2 u32 fields and one output field header.
 * A complete child stream therefore always fits in one networkd response,
 * even before its private WIFI1 prefixes and terminal record are removed.
 */
#define NETWORKD_WIFI_CHILD_ZNV2_OVERHEAD	20U
#define NETWORKD_WIFI_CHILD_OUTPUT_MAX		\
	(NETWORKD_RESPONSE_MAX - NETWORKD_WIFI_CHILD_ZNV2_OVERHEAD)
#define NETWORKD_WIFI_CHILD_RECORD_MAX		64U
#define NETWORKD_WIFI_CHILD_DIAGNOSTIC_MAX	512U
#define NETWORKD_WIFI_CHILD_TIMEOUT_MAX		90U

struct networkd_wifi_child_result {
	unsigned char output[NETWORKD_WIFI_CHILD_OUTPUT_MAX];
	size_t output_length;
	char diagnostic[NETWORKD_WIFI_CHILD_DIAGNOSTIC_MAX + 1U];
	size_t diagnostic_length;
	unsigned output_records;
	int terminal_error;
	int child_exit_status;
	int child_term_signal;
};

struct networkd_wifi_list_result {
	uint32_t scan_state;
	int scan_terminal;
	int scan_complete;
	int ssid_found;
	int ssid_supported;
};

int networkd_wifi_child_run(const char *, const char *, const void *, size_t,
	const void *, size_t, unsigned, struct networkd_wifi_child_result *);
void networkd_wifi_child_result_clear(struct networkd_wifi_child_result *);
int networkd_wifi_child_parse_list(const struct networkd_wifi_child_result *,
	const void *, size_t, struct networkd_wifi_list_result *);

#endif
