/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/net/protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define FAKE_TOKEN 305419896U

static void
fail(const char *message)
{
	perror(message);
	exit(1);
}

static int
append(char *output, size_t capacity, size_t *used, const void *text,
	size_t length)
{
	if (length >= capacity - *used)
		return -1;
	memcpy(output + *used, text, length);
	*used += length;
	output[*used] = '\0';
	return 0;
}

static int
validate_rollback_path(const struct networkd_field *field)
{
	const char *expected;
	struct stat status;
	char path[NETWORKD_ROLLBACK_PATH_MAX + 1U];
	char program[NETWORKD_ROLLBACK_PROGRAM_MAX + 1U];
	ssize_t count;
	int descriptor;

	if (field->length == 0U || field->length > NETWORKD_ROLLBACK_PATH_MAX ||
	    memchr(field->value, '\0', field->length) != NULL)
		return -1;
	memcpy(path, field->value, field->length);
	path[field->length] = '\0';
	if (strncmp(path, "/tmp/net.rollback.", 18U) != 0 ||
	    stat(path, &status) != 0 || !S_ISREG(status.st_mode) ||
	    (status.st_mode & 07777U) != 0600U)
		return -1;
	descriptor = open(path, O_RDONLY);
	if (descriptor < 0)
		return -1;
	count = read(descriptor, program, sizeof(program) - 1U);
	(void)close(descriptor);
	if (count <= 0)
		return -1;
	program[count] = '\0';
	expected = getenv("NETWORKD_FAKE_ROLLBACK");
	if (expected != NULL && program[count - 1] == '\n')
		program[count - 1] = '\0';
	if (expected != NULL && strcmp(program, expected) != 0) {
		fprintf(stderr, "rollback program: got <%s>, expected <%s>\n",
		    program, expected);
		return -1;
	}
	return 0;
}

static int
configuration_contains(const char *environment)
{
	const char *needle;
	const char *path;
	char bytes[8192];
	ssize_t count;
	int descriptor;

	needle = getenv(environment);
	if (needle == NULL)
		return 0;
	path = getenv("NETWORKD_FAKE_NETCONF");
	if (path == NULL)
		return -1;
	descriptor = open(path, O_RDONLY);
	if (descriptor < 0)
		return -1;
	count = read(descriptor, bytes, sizeof(bytes) - 1U);
	(void)close(descriptor);
	if (count < 0)
		return -1;
	bytes[count] = '\0';
	return strstr(bytes, needle) != NULL ? 0 : -1;
}

static int
normalize(const struct networkd_protocol_header *header,
	const unsigned char *payload, char *output, size_t capacity)
{
	struct networkd_field_reader reader;
	struct networkd_field field;
	char number[32];
	size_t used;
	uint32_t timeout;
	int count;
	int result;

	used = 0U;
	if (header->opcode == NETWORKD_OP_SHOW)
		result = append(output, capacity, &used, "V1 SHOW ", 8U);
	else if (header->opcode == NETWORKD_OP_UP)
		result = append(output, capacity, &used, "V1 UP ", 6U);
	else if (header->opcode == NETWORKD_OP_DOWN)
		result = append(output, capacity, &used, "V1 DOWN ", 8U);
	else if (header->opcode == NETWORKD_OP_DHCP)
		result = append(output, capacity, &used, "V1 DHCP ", 8U);
	else if (header->opcode == NETWORKD_OP_STATIC)
		result = append(output, capacity, &used, "V1 STATIC ", 10U);
	else if (header->opcode == NETWORKD_OP_DEFAULT_ROUTE)
		result = append(output, capacity, &used, "V1 DEFAULTROUTE ", 16U);
	else if (header->opcode == NETWORKD_OP_DNS)
		result = append(output, capacity, &used, "V1 DNS ", 7U);
	else if (header->opcode == NETWORKD_OP_DEFAULT_ROUTE_CLEAR)
		result = append(output, capacity, &used,
		    "V1 DEFAULTROUTE_CLEAR ", 22U);
	else if (header->opcode == NETWORKD_OP_DNS_CLEAR)
		result = append(output, capacity, &used, "V1 DNS_CLEAR ", 13U);
	else if (header->opcode == NETWORKD_OP_CONFIRMED_ARM)
		result = append(output, capacity, &used, "V1 ARM ", 7U);
	else if (header->opcode == NETWORKD_OP_CONFIRMED_DISARM)
		result = append(output, capacity, &used, "V1 DISARM ", 10U);
	else if (header->opcode == NETWORKD_OP_CONFIRMED_ROLLBACK)
		result = append(output, capacity, &used, "V1 ROLLBACK ", 12U);
	else if (header->opcode == NETWORKD_OP_CONFIRMED_CHECK)
		result = append(output, capacity, &used, "V1 CHECK ", 9U);
	else
		return -1;
	if (result != 0)
		return -1;

	networkd_field_reader_init(&reader, payload, header->payload_length);
	while ((result = networkd_field_read(&reader, &field)) == 0) {
		if (field.type == NETWORKD_FIELD_INTERFACE ||
		    field.type == NETWORKD_FIELD_GATEWAY ||
		    field.type == NETWORKD_FIELD_DNS) {
			if (append(output, capacity, &used, field.value,
			    field.length) != 0 ||
			    append(output, capacity, &used, " ", 1U) != 0)
				return -1;
		} else if (field.type == NETWORKD_FIELD_ADDRESS) {
			if (append(output, capacity, &used, "ipv4 ", 5U) != 0 ||
			    append(output, capacity, &used, field.value,
			    field.length) != 0 ||
			    append(output, capacity, &used, " ", 1U) != 0)
				return -1;
		} else if (field.type == NETWORKD_FIELD_NETMASK) {
			if (append(output, capacity, &used, "netmask ", 8U) != 0 ||
			    append(output, capacity, &used, field.value,
			    field.length) != 0 ||
			    append(output, capacity, &used, " ", 1U) != 0)
				return -1;
		} else if (field.type == NETWORKD_FIELD_TIMEOUT) {
			if (networkd_field_read_u32(&field, &timeout) != 0)
				return -1;
			count = snprintf(number, sizeof(number), "%lu ",
			    (unsigned long)timeout);
			if (count < 0 || (size_t)count >= sizeof(number) ||
			    append(output, capacity, &used, number,
			    (size_t)count) != 0)
				return -1;
		} else if (field.type == NETWORKD_FIELD_TOKEN) {
			if (networkd_field_read_u32(&field, &timeout) != 0)
				return -1;
			count = snprintf(number, sizeof(number), "token %lu ",
			    (unsigned long)timeout);
			if (count < 0 || (size_t)count >= sizeof(number) ||
			    append(output, capacity, &used, number,
			    (size_t)count) != 0)
				return -1;
		} else if (field.type == NETWORKD_FIELD_PATH &&
		    header->opcode == NETWORKD_OP_CONFIRMED_ARM) {
			if (validate_rollback_path(&field) != 0)
				return -1;
		} else {
			return -1;
		}
	}
	if (result < 0 || used == 0U)
		return -1;
	output[used - 1U] = '\n';
	return 0;
}

static void
reply_ok(int client, const struct networkd_protocol_header *request)
{
	struct networkd_protocol_header response;
	struct networkd_field_writer writer;
	unsigned char payload[32];

	networkd_field_writer_init(&writer, payload, sizeof(payload));
	if (networkd_field_write_u32(&writer, NETWORKD_FIELD_STATUS,
	    NETWORKD_RESULT_OK) != 0 ||
	    networkd_field_write_u32(&writer, NETWORKD_FIELD_ERROR, 0U) != 0 ||
	    (request->opcode == NETWORKD_OP_CONFIRMED_ARM &&
	    networkd_field_write_u32(&writer, NETWORKD_FIELD_TOKEN,
	    FAKE_TOKEN) != 0))
		fail("reply payload");
	response.request_id = request->request_id;
	response.opcode = request->opcode;
	response.payload_length = writer.used;
	if (networkd_protocol_write_frame(client, &response, payload) != 0)
		fail("reply");
}

static void
reply_error(int client, const struct networkd_protocol_header *request)
{
	struct networkd_protocol_header response;
	struct networkd_field_writer writer;
	unsigned char payload[32];

	networkd_field_writer_init(&writer, payload, sizeof(payload));
	if (networkd_field_write_u32(&writer, NETWORKD_FIELD_STATUS,
	    NETWORKD_RESULT_ERROR) != 0 ||
	    networkd_field_write_u32(&writer, NETWORKD_FIELD_ERROR, EIO) != 0)
		fail("error reply payload");
	response.request_id = request->request_id;
	response.opcode = request->opcode;
	response.payload_length = writer.used;
	if (networkd_protocol_write_frame(client, &response, payload) != 0)
		fail("error reply");
}

int
main(int argc, char **argv)
{
	struct sockaddr_un address;
	int listener, index;

	if (argc < 3) {
		fprintf(stderr, "usage: fake SOCKET REQUEST...\n");
		return 2;
	}
	listener = socket(AF_UNIX, SOCK_STREAM, 0);
	if (listener < 0)
		fail("socket");
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	if (strlen(argv[1]) >= sizeof(address.sun_path)) {
		errno = ENAMETOOLONG;
		fail("socket path");
	}
	strcpy(address.sun_path, argv[1]);
	(void)unlink(argv[1]);
	if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
	    listen(listener, 4) != 0)
		fail("listen");
	for (index = 2; index < argc; index++) {
		struct networkd_protocol_header header;
		unsigned char payload[NETWORKD_REQUEST_MAX];
		char request[512];
		int client = accept(listener, NULL, NULL);
		if (client < 0)
			fail("accept");
		if (networkd_protocol_read_frame(client, &header, payload,
		    sizeof(payload), NETWORKD_REQUEST_MAX) != 0 ||
		    normalize(&header, payload, request, sizeof(request)) != 0)
			fail("request");
		if ((header.opcode == NETWORKD_OP_CONFIRMED_ARM &&
		    configuration_contains("NETWORKD_FAKE_ARM_CONFIG_CONTAINS") != 0) ||
		    (header.opcode == NETWORKD_OP_CONFIRMED_DISARM &&
		    configuration_contains(
		    "NETWORKD_FAKE_DISARM_CONFIG_CONTAINS") != 0) ||
		    (header.opcode != NETWORKD_OP_CONFIRMED_DISARM &&
		    configuration_contains(
		    "NETWORKD_FAKE_REQUEST_CONFIG_CONTAINS") != 0)) {
			fprintf(stderr, "configuration ordering check failed\n");
			return 1;
		}
		if (strcmp(request,
		    strncmp(argv[index], "LOSE ", 5U) == 0 ||
		    strncmp(argv[index], "FAIL ", 5U) == 0 ?
		    argv[index] + 5 : argv[index]) != 0) {
			fprintf(stderr, "request %d: got <%s>, expected <%s>\n",
				index - 1, request, argv[index]);
			return 1;
		}
		if (strncmp(argv[index], "FAIL ", 5U) == 0)
			reply_error(client, &header);
		else if (strncmp(argv[index], "LOSE ", 5U) != 0)
			reply_ok(client, &header);
		(void)close(client);
	}
	(void)close(listener);
	(void)unlink(argv[1]);
	return 0;
}
