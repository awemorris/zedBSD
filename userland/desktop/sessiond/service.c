/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A session's SERVICE request (ws089-p025, the Sharing page's Remote
 * Login): "SERVICE sshd on|off|status" on the session's socket, decided by
 * service-rules.c (the fixed table, root or wheel), carried out as the
 * service command does (userland/base/service): on and off change the
 * service's enabled in rc.conf (kept across starts), have init read it
 * again, and start or stop it now; every answer is the state:
 *
 *   SERVICE available=0|1 enabled=0|1 running=0|1 port=N
 *
 * or DENIED (not root nor wheel) or ERROR (anything else).  Every request
 * is logged to the authentication log.
 */

#include "sessiond.h"
#include "service-rules.h"
#include "../../base/common/account.h"
#include "../../base/service/rcconf.h"
#include "../../base/service/zsv1-client.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

/* Where sshd's configuration is, the port it listens on without one, and the longest line read of it. */
#define SERVICE_SSHD_CONFIG	"/etc/ssh/sshd_config"
#define SERVICE_SSH_PORT	22U
#define SERVICE_LINE_MAX	256U

/*
 * A service's state for the answer: whether init knows it, whether rc.conf
 * enables it, whether it runs, and its port.
 */
struct service_state {
	unsigned available;
	unsigned enabled;
	unsigned running;
	unsigned port;
};

static int service_init(enum zsv1_command command, const char *name, struct zsv1_response *response);
static void service_state(const char *name, struct service_state *state);
static unsigned service_port(void);

/*
 * Answers a session's SERVICE request (arguments: the line after
 * "SERVICE ") on its socket.
 */
void
sessiond_service(
	const struct sessiond_account *account,
	int control,
	const char *arguments)
{
	struct sessiond_service_request request;
	struct service_state state;
	struct zsv1_response response;
	char answer[128];
	const char *name;
	int in_wheel;
	int decided;
	int error;
	int length;

	/* Decided: the table's service, a word, and the user's right. */
	name = account->passwd.pw_name;
	in_wheel = account_in_wheel(name, account->passwd.pw_gid);
	decided = sessiond_service_decide(arguments, account->passwd.pw_uid, in_wheel, &request);
	if (decided == EPERM) {
		syslog(LOG_WARNING, "service request \"%.40s\" by %s refused (not in wheel)", arguments, name);
		(void)write(control, "DENIED\n", 7U);
		return;
	}

	/* Another service or word is not one a session may ask. */
	if (decided != 0) {
		syslog(LOG_WARNING, "service request \"%.40s\" by %s refused (not allowed)", arguments, name);
		(void)write(control, "ERROR\n", 6U);
		return;
	}

	/* On or off: known to init, kept in rc.conf, read again, started or stopped. */
	error = 0;
	if (request.action != SESSIOND_SERVICE_STATUS) {
		error = service_init(ZSV1_COMMAND_SHOW, request.name, &response);
		if (error == 0) {
			error = rcconf_set_enabled(RCCONF_PATH, request.name, request.action == SESSIOND_SERVICE_ON);
			if (error != 0)
				error = errno;
		}

		/* init reads rc.conf again, then starts or stops it. */
		if (error == 0)
			error = service_init(ZSV1_COMMAND_RELOAD, NULL, &response);
		if (error == 0 && request.action == SESSIOND_SERVICE_ON)
			error = service_init(ZSV1_COMMAND_START, request.name, &response);
		if (error == 0 && request.action == SESSIOND_SERVICE_OFF)
			error = service_init(ZSV1_COMMAND_STOP, request.name, &response);
		syslog(LOG_NOTICE, "service %s %s by %s: errno=%d", request.name, arguments + strlen(request.name) + 1U, name, error);
	}

	/* A failure says so. */
	if (error != 0) {
		(void)write(control, "ERROR\n", 6U);
		return;
	}

	/* The state as it is now. */
	service_state(request.name, &state);
	length = snprintf(answer, sizeof(answer), "SERVICE available=%u enabled=%u running=%u port=%u\n", state.available, state.enabled, state.running, state.port);
	if (length > 0 && (size_t)length < sizeof(answer))
		(void)write(control, answer, (size_t)length);
}

/* Asks init one thing of a service (or of all, without a name); returns 0 or an errno value. */
static int
service_init(
	enum zsv1_command command,
	const char *name,
	struct zsv1_response *response)
{
	struct zsv1_request request;
	int result;

	/* The request. */
	memset(&request, 0, sizeof(request));
	memset(response, 0, sizeof(*response));
	request.command = command;
	if (name != NULL)
		(void)snprintf(request.service, sizeof(request.service), "%s", name);

	/* Asked, and answered whole without an error. */
	result = zsv1_client_call(ZSV1_INIT_SOCKET, &request, response);
	if (result != 0 && errno != 0)
		return errno;
	if (result != 0)
		return EIO;
	if (!response->ended)
		return EIO;
	if (response->error_present) {
		if (response->error_number != 0)
			return response->error_number;
		return EIO;
	}

	/* Succeeded: init did it. */
	return 0;
}

/* Reads a service's state: known to init, enabled in rc.conf, running, and its port. */
static void
service_state(
	const char *name,
	struct service_state *state)
{
	struct rcconf_model *model;
	struct zsv1_response response;
	int enabled;
	int error;

	/* What init says of it (a service init does not know is not available). */
	memset(state, 0, sizeof(*state));
	state->port = service_port();
	error = service_init(ZSV1_COMMAND_SHOW, name, &response);
	if (error == 0 && response.service_count == 1U) {
		state->available = 1U;
		if (response.services[0].state == ZSV1_STATE_RUNNING || response.services[0].state == ZSV1_STATE_STARTING)
			state->running = 1U;
	}

	/* What rc.conf says (its model is large: on the heap). */
	model = malloc(sizeof(*model));
	if (model == NULL)
		return;
	rcconf_model_init(model);
	error = rcconf_load(RCCONF_PATH, model);
	enabled = 0;
	if (error == 0)
		error = rcconf_service_enabled(model, name, &enabled);
	if (error == 0 && enabled)
		state->enabled = 1U;
	free(model);
}

/* Reads sshd's port from its configuration (the first Port line), 22 without one. */
static unsigned
service_port(void)
{
	char line[SERVICE_LINE_MAX];
	unsigned long port;
	char *got;
	char *end;
	FILE *file;
	int differs;

	/* The configuration, when there is one. */
	file = fopen(SERVICE_SSHD_CONFIG, "r");
	if (file == NULL)
		return SERVICE_SSH_PORT;

	/* The first "Port N" line. */
	port = SERVICE_SSH_PORT;
	for (;;) {
		got = fgets(line, sizeof(line), file);
		if (got == NULL)
			break;
		differs = strncmp(line, "Port ", 5U);
		if (differs != 0)
			continue;
		port = strtoul(line + 5, &end, 10);
		if (port == 0UL || port > 65535UL)
			port = SERVICE_SSH_PORT;
		break;
	}

	/* The file closed. */
	(void)fclose(file);
	return (unsigned)port;
}
