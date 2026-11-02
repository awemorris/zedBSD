/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* libFuzzer target: the monitor's frames taken from a stream. */

#include "ssh-privsep.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct ssh_buf input;
	struct ssh_buf body;
	enum ssh_mon_type type;

	ssh_buf_init(&input, 4U + SSH_MON_STATE_MAX, 1);
	ssh_buf_init(&body, SSH_MON_STATE_MAX, 1);
	ssh_buf_put_bytes(&input, data, size);
	while (ssh_mon_next_frame(&input, &type, &body) == 0)
		;
	ssh_buf_free(&input);
	ssh_buf_free(&body);
	return 0;
}
