/* WS012 SVC-T003 shutdown argv/ZSV1 mapping fixture. SPDX-License-Identifier:
 * Zlib */
#include "userland/base/service/zsv1-client.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int call_mode, call_count;
static enum zsv1_command last_command;

int
zsv1_client_call(const char *path, const struct zsv1_request *request,
		 struct zsv1_response *response)
{
	if (strcmp(path, ZSV1_INIT_SOCKET) != 0)
		return -1;
	call_count++;
	last_command = request->command;
	memset(response, 0, sizeof(*response));
	response->ended = 1;
	if (call_mode == 1) {
		response->error_present = 1;
		response->error_number = EIO;
		strcpy(response->error_reason, "action-failed");
		return 0;
	}
	if (call_mode == 2) {
		errno = EIO;
		return -1;
	}
	if (call_mode == 3)
		return 0;
	response->ok_present = 1;
	strcpy(response->ok_token, call_mode == 4 ? "accepted" : "scheduled");
	return 0;
}

#define main shutdown_control_main
#include "userland/base/shutdown-control/main.c"
#undef main

static void
fail(const char *message)
{
	fprintf(stderr, "zsv1-shutdown-argv-test: %s\n", message);
	exit(1);
}

static void
expect_command(int argc, char **argv, enum zsv1_command command)
{
	call_mode = 0;
	call_count = 0;
	if (shutdown_control_main(argc, argv) != 0 || call_count != 1 ||
	    last_command != command)
		fail("command mapping");
}

static void
expect_usage(int argc, char **argv)
{
	call_mode = 0;
	call_count = 0;
	if (shutdown_control_main(argc, argv) != 2 || call_count != 0)
		fail("usage status");
}

int
main(void)
{
	char *halt[] = {"/sbin/halt", NULL};
	char *poweroff[] = {"/sbin/poweroff", NULL};
	char *reboot[] = {"/sbin/reboot", NULL};
	char *shutdown[] = {"/sbin/shutdown", NULL};
	char *shutdown_h[] = {"/sbin/shutdown", "-h", NULL};
	char *shutdown_r[] = {"/sbin/shutdown", "-r", NULL};
	char *shutdown_bad[] = {"/sbin/shutdown", "-x", NULL};
	char *halt_bad[] = {"/sbin/halt", "extra", NULL};
	char *unknown[] = {"/sbin/not-an-action", NULL};

	expect_command(1, halt, ZSV1_COMMAND_HALT);
	expect_command(1, poweroff, ZSV1_COMMAND_POWEROFF);
	expect_command(1, reboot, ZSV1_COMMAND_REBOOT);
	expect_command(1, shutdown, ZSV1_COMMAND_POWEROFF);
	expect_command(2, shutdown_h, ZSV1_COMMAND_POWEROFF);
	expect_command(2, shutdown_r, ZSV1_COMMAND_REBOOT);
	expect_usage(2, shutdown_bad);
	expect_usage(2, halt_bad);
	expect_usage(1, unknown);

	call_mode = 1;
	if (shutdown_control_main(1, halt) != 1)
		fail("server error status");
	call_mode = 2;
	if (shutdown_control_main(1, halt) != 1)
		fail("protocol/transport status");
	call_mode = 3;
	if (shutdown_control_main(1, halt) != 1)
		fail("missing OK status");
	call_mode = 4;
	if (shutdown_control_main(1, halt) != 1)
		fail("non-scheduled OK status");

	puts("zsv1 shutdown argv test: PASS");
	return 0;
}
