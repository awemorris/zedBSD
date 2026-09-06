/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
/* Checks real wifi output with the daemon's production stream consumer. */

#include "userland/base/networkd/wifi-child.c"

int fixture_validate_wifi_stream(const void *, size_t, int);

int
fixture_validate_wifi_stream(const void *output, size_t length, int exit_status)
{
	struct networkd_wifi_child_state state;
	struct networkd_wifi_child_result result;
	size_t index;

	initialize_state(&state);
	networkd_wifi_child_result_clear(&result);
	if (length > sizeof(result.output))
		return EOVERFLOW;
	memcpy(result.output, output, length);
	result.output_length = length;
	for (index = 0U; index < length; index++) {
		if (result.output[index] == '\n')
			result.output_records++;
	}
	state.child_reaped = 1;
	state.child_status = exit_status << 8;
	return finish_child(&state, &result, 0);
}
