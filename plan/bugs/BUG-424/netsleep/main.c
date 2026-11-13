/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * netsleep: asks networkd for SLEEP_PREPARE (the Wi-Fi radios off and kept
 * off) or SLEEP_END, as sessiond's sleep does around the kernel's S0 idle,
 * so that a test can ask the kernel itself (sleepctl s0idle) in between.
 *
 *   netsleep prepare | end
 *
 * It prints "netsleep: OPCODE status=S error=E" and exits 0 when networkd
 * answered OK.
 */

#include "userland/base/net/protocol.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* How long networkd may take to answer, in seconds. */
#define NETSLEEP_WAIT_SECONDS	(NETWORKD_SLEEP_PREPARE_SECONDS + 5U)

/*
 * Sends one request and prints the answer.
 */
int
main(
	int argc,
	char **argv)
{
	struct networkd_protocol_header request;
	struct networkd_protocol_header response;
	struct networkd_field_reader reader;
	struct networkd_field field;
	struct sockaddr_un address;
	unsigned char payload[NETWORKD_RESPONSE_MAX];
	uint32_t status;
	uint32_t error;
	uint32_t opcode;
	int descriptor;
	int result;

	/* The request asked for. */
	if (argc != 2) {
		fprintf(stderr, "usage: netsleep prepare | end\n");
		return 2;
	}
	if (strcmp(argv[1], "prepare") == 0) {
		opcode = NETWORKD_OP_SLEEP_PREPARE;
	} else if (strcmp(argv[1], "end") == 0) {
		opcode = NETWORKD_OP_SLEEP_END;
	} else {
		fprintf(stderr, "usage: netsleep prepare | end\n");
		return 2;
	}

	/* networkd's endpoint. */
	descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
	if (descriptor < 0) {
		fprintf(stderr, "netsleep: socket: %s\n", strerror(errno));
		return 1;
	}
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	snprintf(address.sun_path, sizeof(address.sun_path), "%s", NETWORKD_SOCKET);
	result = connect(descriptor, (struct sockaddr *)&address, sizeof(address));
	if (result != 0) {
		fprintf(stderr, "netsleep: %s: %s\n", NETWORKD_SOCKET, strerror(errno));
		return 1;
	}

	/* One frame, then the request's direction closed. */
	request.request_id = 1U;
	request.opcode = opcode;
	request.payload_length = 0U;
	result = networkd_protocol_write_frame(descriptor, &request, NULL);
	if (result == 0)
		result = shutdown(descriptor, SHUT_WR);
	if (result != 0) {
		fprintf(stderr, "netsleep: write: %s\n", strerror(errno));
		return 1;
	}

	/* The answer. */
	result = networkd_protocol_read_frame_timed(descriptor, &response, payload, sizeof(payload), NETWORKD_RESPONSE_MAX, NETSLEEP_WAIT_SECONDS);
	if (result != 0) {
		fprintf(stderr, "netsleep: read: %s\n", strerror(errno));
		return 1;
	}
	(void)close(descriptor);

	/* Its status and error. */
	status = NETWORKD_RESULT_ERROR;
	error = EIO;
	networkd_field_reader_init(&reader, payload, response.payload_length);
	for (;;) {
		result = networkd_field_read(&reader, &field);
		if (result != 0)
			break;
		if (field.type == NETWORKD_FIELD_STATUS)
			(void)networkd_field_read_u32(&field, &status);
		else if (field.type == NETWORKD_FIELD_ERROR)
			(void)networkd_field_read_u32(&field, &error);
	}
	printf("netsleep: %s status=%u error=%u\n", argv[1], (unsigned)status, (unsigned)error);

	/* Succeeded only when networkd says so. */
	if (status != NETWORKD_RESULT_OK || error != 0U)
		return 1;
	return 0;
}
