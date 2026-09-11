/* WS012 SVC-T003 production ZSV1 grammar/decoder fixture.
 * SPDX-License-Identifier: Zlib */
#include "userland/base/service/zsv1-protocol.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
fail(const char *message)
{
	fprintf(stderr, "zsv1-protocol-test: %s (errno=%d)\n", message, errno);
	exit(1);
}

static void
request_round_trip(enum zsv1_command command, const char *service,
		   const char *expected)
{
	struct zsv1_request input, output;
	char wire[ZSV1_REQUEST_MAX + 1U];
	size_t length;

	memset(&input, 0, sizeof(input));
	input.command = command;
	if (service != NULL)
		strcpy(input.service, service);
	if (zsv1_request_format(&input, wire, sizeof(wire), &length) != 0 ||
	    length != strlen(expected) || memcmp(wire, expected, length) != 0 ||
	    zsv1_request_parse(wire, length, &output) != 0 ||
	    output.command != command ||
	    strcmp(output.service, service != NULL ? service : "") != 0)
		fail("request round trip");
}

static void
request_must_fail(const void *wire, size_t length)
{
	struct zsv1_request request;

	memset(&request, 0xa5, sizeof(request));
	if (zsv1_request_parse(wire, length, &request) == 0)
		fail("invalid request accepted");
}

static void
request_must_fail_with(const void *wire, size_t length, int expected_error)
{
	struct zsv1_request request;

	errno = 0;
	if (zsv1_request_parse(wire, length, &request) == 0 ||
	    errno != expected_error)
		fail("invalid request error classification");
}

static void
record_round_trip(const char *wire, enum zsv1_record_type type)
{
	struct zsv1_record record;
	char formatted[ZSV1_RESPONSE_LINE_MAX + 1U];
	size_t length;

	if (zsv1_record_parse(wire, strlen(wire), &record) != 0 ||
	    record.type != type ||
	    zsv1_record_format(&record, formatted, sizeof(formatted),
			       &length) != 0 ||
	    length != strlen(wire) || memcmp(formatted, wire, length) != 0)
		fail("record round trip");
}

static void
record_must_fail(const char *wire)
{
	struct zsv1_record record;

	if (zsv1_record_parse(wire, strlen(wire), &record) == 0)
		fail("invalid record accepted");
}

static void
decoder_must_fail(const char *wire, int finish_only)
{
	struct zsv1_decoder decoder;

	zsv1_decoder_init(&decoder);
	if (zsv1_decoder_feed(&decoder, wire, strlen(wire)) != 0) {
		if (finish_only)
			fail("decoder failed before EOF check");
		return;
	}
	if (finish_only && zsv1_decoder_finish(&decoder) != 0)
		return;
	fail("invalid response accepted");
}

static void
test_requests(void)
{
	static const unsigned char nul_request[] = {
	    'Z', 'S', 'V', '1', ' ', 'L', 'I', 'S', 'T', '\0', '\n'};
	char long_name[ZSV1_NAME_CAPACITY + 1U];
	char overlong[ZSV1_REQUEST_MAX + 1U];

	request_round_trip(ZSV1_COMMAND_LIST, NULL, "ZSV1 LIST\n");
	request_round_trip(ZSV1_COMMAND_SHOW, "sshd", "ZSV1 SHOW sshd\n");
	request_round_trip(ZSV1_COMMAND_START, "sshd", "ZSV1 START sshd\n");
	request_round_trip(ZSV1_COMMAND_STOP, "sshd", "ZSV1 STOP sshd\n");
	request_round_trip(ZSV1_COMMAND_RESTART, "sshd", "ZSV1 RESTART sshd\n");
	request_round_trip(ZSV1_COMMAND_RELOAD, NULL, "ZSV1 RELOAD\n");
	request_round_trip(ZSV1_COMMAND_HALT, NULL, "ZSV1 HALT\n");
	request_round_trip(ZSV1_COMMAND_POWEROFF, NULL, "ZSV1 POWEROFF\n");
	request_round_trip(ZSV1_COMMAND_REBOOT, NULL, "ZSV1 REBOOT\n");

	request_must_fail("ZSV1 LIST", strlen("ZSV1 LIST"));
	request_must_fail(nul_request, sizeof(nul_request));
	request_must_fail("ZSV1\tLIST\n", strlen("ZSV1\tLIST\n"));
	request_must_fail_with("LIST\n", strlen("LIST\n"), EPROTONOSUPPORT);
	request_must_fail_with("halt\n", strlen("halt\n"), EPROTONOSUPPORT);
	request_must_fail_with("ZSV2 LIST\n", strlen("ZSV2 LIST\n"),
			       EPROTONOSUPPORT);
	request_must_fail("ZSV1 LIST extra\n", strlen("ZSV1 LIST extra\n"));
	request_must_fail("ZSV1 SHOW\n", strlen("ZSV1 SHOW\n"));
	request_must_fail("ZSV1 SHOW sshd extra\n",
			  strlen("ZSV1 SHOW sshd extra\n"));
	request_must_fail("ZSV1  LIST\n", strlen("ZSV1  LIST\n"));
	request_must_fail("ZSV1 LIST\nZSV1 HALT\n",
			  strlen("ZSV1 LIST\nZSV1 HALT\n"));
	memset(long_name, 'a', ZSV1_NAME_CAPACITY);
	long_name[ZSV1_NAME_CAPACITY] = '\0';
	{
		char request[160];

		snprintf(request, sizeof(request), "ZSV1 SHOW %s\n", long_name);
		request_must_fail(request, strlen(request));
	}
	memset(overlong, 'A', sizeof(overlong));
	overlong[sizeof(overlong) - 1] = '\n';
	request_must_fail(overlong, sizeof(overlong));
}

static void
test_records(void)
{
	record_round_trip("ZSV1 SERVICE sshd running 1 231\n",
			  ZSV1_RECORD_SERVICE);
	record_round_trip("ZSV1 SERVICE cron stopped 0 0\n",
			  ZSV1_RECORD_SERVICE);
	record_round_trip("ZSV1 AFTER networkd\n", ZSV1_RECORD_AFTER);
	record_round_trip("ZSV1 REQUIRES networkd\n", ZSV1_RECORD_REQUIRES);
	record_round_trip("ZSV1 OK stopped\n", ZSV1_RECORD_OK);
	record_round_trip("ZSV1 ERROR 2 unknown-service\n", ZSV1_RECORD_ERROR);
	record_round_trip("ZSV1 END\n", ZSV1_RECORD_END);

	record_must_fail("ZSV2 END\n");
	record_must_fail("ZSV1 UNKNOWN thing\n");
	record_must_fail("ZSV1 SERVICE sshd unknown 1 1\n");
	record_must_fail("ZSV1 SERVICE sshd running yes 1\n");
	record_must_fail("ZSV1 SERVICE sshd running 1 -1\n");
	record_must_fail("ZSV1 SERVICE sshd running 1 999999999999999\n");
	record_must_fail("ZSV1 ERROR 0 failed\n");
	record_must_fail("ZSV1 ERROR 5 Bad-Reason\n");
	record_must_fail("ZSV1 ERROR 5 bad--reason\n");
	record_must_fail("ZSV1 END extra\n");
	record_must_fail("ZSV1 END");
}

static void
test_decoder(void)
{
	static const char response[] = "ZSV1 SERVICE sshd running 1 231\n"
				       "ZSV1 AFTER networkd\n"
				       "ZSV1 REQUIRES networkd\n"
				       "ZSV1 OK shown\n"
				       "ZSV1 END\n";
	struct zsv1_decoder decoder;
	const struct zsv1_response *decoded;
	size_t index;

	zsv1_decoder_init(&decoder);
	for (index = 0; index < sizeof(response) - 1; index++) {
		if (zsv1_decoder_feed(&decoder, response + index, 1) != 0)
			fail("fragmented response");
	}
	if (zsv1_decoder_finish(&decoder) != 0 ||
	    (decoded = zsv1_decoder_response(&decoder)) == NULL ||
	    decoded->service_count != 1 || decoded->dependency_count != 2 ||
	    !decoded->ok_present || decoded->error_present ||
	    strcmp(decoded->services[0].name, "sshd") != 0 ||
	    strcmp(decoded->ok_token, "shown") != 0)
		fail("decoded response content");

	decoder_must_fail("ZSV1 OK ready\n", 1);
	decoder_must_fail("ZSV1 SERVICE a running 1 1\n"
			  "ZSV1 SERVICE a stopped 0 0\nZSV1 END\n",
			  0);
	decoder_must_fail("ZSV1 AFTER a\nZSV1 AFTER a\nZSV1 END\n", 0);
	decoder_must_fail("ZSV1 OK ready\nZSV1 OK ready\nZSV1 END\n", 0);
	decoder_must_fail("ZSV1 ERROR 5 failed\nZSV1 SERVICE a running 1 1\n"
			  "ZSV1 END\n",
			  0);
	decoder_must_fail("ZSV1 END\nZSV1 OK late\n", 0);
	decoder_must_fail("ZSV1 BOGUS\nZSV1 END\n", 0);

	zsv1_decoder_init(&decoder);
	for (index = 0; index < ZSV1_SERVICE_MAX; index++) {
		char line[128];

		snprintf(line, sizeof(line),
			 "ZSV1 SERVICE svc%02zu stopped 0 0\n", index);
		if (zsv1_decoder_feed(&decoder, line, strlen(line)) != 0)
			fail("service count boundary");
	}
	if (zsv1_decoder_feed(&decoder, "ZSV1 SERVICE overflow stopped 0 0\n",
			      strlen("ZSV1 SERVICE overflow stopped 0 0\n")) ==
	    0)
		fail("service count overflow accepted");

	zsv1_decoder_init(&decoder);
	{
		char line[ZSV1_RESPONSE_LINE_MAX + 1U];

		memset(line, 'a', sizeof(line));
		line[sizeof(line) - 1] = '\n';
		if (zsv1_decoder_feed(&decoder, line, sizeof(line)) == 0)
			fail("response line overflow accepted");
	}

	zsv1_decoder_init(&decoder);
	if (zsv1_decoder_feed(&decoder, "ZSV1 ERROR 5 failed\nZSV1 END\n",
			      strlen("ZSV1 ERROR 5 failed\nZSV1 END\n")) != 0 ||
	    zsv1_decoder_finish(&decoder) != 0 ||
	    !decoder.response.error_present ||
	    decoder.response.error_number != 5)
		fail("error response");
}

int
main(void)
{
	test_requests();
	test_records();
	test_decoder();
	puts("zsv1 protocol test: PASS");
	return 0;
}
