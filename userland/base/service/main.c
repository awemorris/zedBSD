/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/service/service-config.h"
#include "userland/base/service/rcconf.h"
#include "userland/base/service/zsv1-client.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int
service_record_compare(const void *left, const void *right)
{
	const struct zsv1_service_record *a = left, *b = right;

	return strcmp(a->name, b->name);
}

static int
dependency_record_compare(const void *left, const void *right)
{
	const struct zsv1_dependency_record *a = left, *b = right;

	if (a->type != b->type)
		return a->type < b->type ? -1 : 1;
	return strcmp(a->name, b->name);
}

static int
print_service_record(const struct zsv1_service_record *service, int detailed)
{
	const char *state = zsv1_state_name(service->state);

	if (state == NULL)
		return -1;
	if (detailed)
		return printf("%s\t%s\t%s\tpid=%ld\n", service->name,
			      service->enabled ? "enabled" : "disabled", state,
			      (long)service->pid) < 0
			   ? -1
			   : 0;
	return printf("%s\t%s\t%s\n", service->name,
		      service->enabled ? "enabled" : "disabled", state) < 0
		   ? -1
		   : 0;
}

static const char *
expected_ok_token(enum zsv1_command command)
{
	switch (command) {
	case ZSV1_COMMAND_START:
		return "started";
	case ZSV1_COMMAND_STOP:
		return "stopped";
	case ZSV1_COMMAND_RESTART:
		return "restarted";
	case ZSV1_COMMAND_RELOAD:
		return "reloaded";
	default:
		return NULL;
	}
}

static int
render_response(enum zsv1_command command, const char *name,
		const struct zsv1_response *response)
{
	struct zsv1_service_record services[ZSV1_SERVICE_MAX];
	struct zsv1_dependency_record dependencies[ZSV1_DEPENDENCY_MAX];
	size_t index;

	if (response->error_present) {
		fprintf(stderr, "service: %s: errno %d (%s)\n",
			response->error_reason, response->error_number,
			strerror(response->error_number));
		return 1;
	}

	if (command == ZSV1_COMMAND_LIST) {
		if (response->ok_present || response->dependency_count != 0)
			goto invalid;
		memcpy(services, response->services,
		       response->service_count * sizeof(services[0]));
		qsort(services, response->service_count, sizeof(services[0]),
		      service_record_compare);
		for (index = 0; index < response->service_count; index++) {
			if (print_service_record(&services[index], 0) != 0)
				return 1;
		}
		return 0;
	}

	if (command == ZSV1_COMMAND_SHOW) {
		if (response->ok_present || response->service_count != 1 ||
		    name == NULL ||
		    strcmp(response->services[0].name, name) != 0)
			goto invalid;
		if (print_service_record(&response->services[0], 1) != 0)
			return 1;
		memcpy(dependencies, response->dependencies,
		       response->dependency_count * sizeof(dependencies[0]));
		qsort(dependencies, response->dependency_count,
		      sizeof(dependencies[0]), dependency_record_compare);
		for (index = 0; index < response->dependency_count; index++) {
			if (printf("%s\t%s\n",
				   dependencies[index].type ==
					   ZSV1_DEPENDENCY_AFTER
				       ? "after"
				       : "requires",
				   dependencies[index].name) < 0)
				return 1;
		}
		return 0;
	}

	if (!response->ok_present || expected_ok_token(command) == NULL ||
	    strcmp(response->ok_token, expected_ok_token(command)) != 0 ||
	    response->service_count != 0 || response->dependency_count != 0)
		goto invalid;
	return printf("OK %s\n", response->ok_token) < 0 ? 1 : 0;

invalid:
	fprintf(stderr, "service: invalid ZSV1 response\n");
	return 1;
}

static int
send_request(enum zsv1_command command, const char *name)
{
	struct zsv1_request request;
	struct zsv1_response response;

	memset(&request, 0, sizeof(request));
	request.command = command;
	if (name != NULL)
		strcpy(request.service, name);
	if (zsv1_client_call(ZSV1_INIT_SOCKET, &request, &response) != 0) {
		fprintf(stderr, "service: ZSV1 request failed: %s\n",
			strerror(errno));
		return 1;
	}
	return render_response(command, name, &response);
}

int
main(int argc, char **argv)
{
	const char *command, *name = NULL;

	if (argc < 2 || argc > 3) {
		fprintf(stderr, "usage: service "
				"{list|reload|status|start|stop|restart|enable|"
				"disable} [name]\n");
		return 2;
	}
	command = argv[1];
	if (argc == 3)
		name = argv[2];
	if ((strcmp(command, "list") == 0 || strcmp(command, "reload") == 0) &&
	    name != NULL) {
		fprintf(stderr, "service: %s takes no service name\n", command);
		return 2;
	}
	if (name != NULL && !zsv1_name_valid(name)) {
		fprintf(stderr, "service: invalid service name: %s\n", name);
		return 2;
	}
	if (strcmp(command, "enable") == 0 || strcmp(command, "disable") == 0) {
		if (name == NULL || geteuid() != 0) {
			fprintf(
			    stderr,
			    "service: %s requires root and a service name\n",
			    command);
			return name == NULL ? 2 : 1;
		}
		if (rcconf_set_enabled(RCCONF_PATH, name,
				       strcmp(command, "enable") == 0) != 0) {
			fprintf(stderr, "service: cannot update %s: %s\n",
				RCCONF_PATH, strerror(errno));
			return 1;
		}
		return send_request(ZSV1_COMMAND_RELOAD, NULL);
	}
	if (strcmp(command, "list") != 0 && strcmp(command, "reload") != 0 &&
	    strcmp(command, "status") != 0 && strcmp(command, "start") != 0 &&
	    strcmp(command, "stop") != 0 && strcmp(command, "restart") != 0) {
		fprintf(stderr, "service: unknown command: %s\n", command);
		return 2;
	}
	if (strcmp(command, "list") != 0 && strcmp(command, "reload") != 0 &&
	    name == NULL) {
		fprintf(stderr, "service: %s requires a service name\n",
			command);
		return 2;
	}
	if (strcmp(command, "list") == 0)
		return send_request(ZSV1_COMMAND_LIST, NULL);
	if (strcmp(command, "reload") == 0)
		return send_request(ZSV1_COMMAND_RELOAD, NULL);
	if (strcmp(command, "status") == 0)
		return send_request(ZSV1_COMMAND_SHOW, name);
	if (strcmp(command, "start") == 0)
		return send_request(ZSV1_COMMAND_START, name);
	if (strcmp(command, "stop") == 0)
		return send_request(ZSV1_COMMAND_STOP, name);
	return send_request(ZSV1_COMMAND_RESTART, name);
}
