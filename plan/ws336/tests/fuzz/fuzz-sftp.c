/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* libFuzzer target: SFTP packets taken from a stream and read as requests. */

#include "ssh-sftp.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct ssh_buf input;
	struct ssh_buf packet;
	struct ssh_sftp_request request;

	ssh_buf_init(&input, SSH_SFTP_PACKET_MAX * 2U, 0);
	ssh_buf_init(&packet, SSH_SFTP_PACKET_MAX, 0);
	ssh_buf_put_bytes(&input, data, size);
	for (;;) {
		ssh_buf_reset(&packet);
		if (ssh_sftp_next_packet(&input, &packet) != 0)
			break;
		(void)ssh_sftp_parse_request(packet.data, packet.size, &request);
	}

	(void)ssh_sftp_parse_request(data, size, &request);
	ssh_buf_free(&input);
	ssh_buf_free(&packet);
	return 0;
}
