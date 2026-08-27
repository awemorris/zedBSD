/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/service/service-command.h"

#include "userland/base/service/rcconf.h"
#include "userland/base/service/service-config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SERVICE_PATH_CAPACITY 4096U
#define SERVICE_COMMAND_CAPACITY 256U
#define SERVICE_ARGUMENTS_CAPACITY 512U
#define SERVICE_TYPE_CAPACITY 16U
#define SERVICE_RESTART_CAPACITY 16U

enum service_operation {
	SERVICE_OPERATION_LIST,
	SERVICE_OPERATION_SHOW,
	SERVICE_OPERATION_START,
	SERVICE_OPERATION_STOP,
	SERVICE_OPERATION_RESTART,
	SERVICE_OPERATION_ENABLE,
	SERVICE_OPERATION_DISABLE,
	SERVICE_OPERATION_RELOAD,
};

struct service_command {
	enum service_operation operation;
	char name[ZSV1_NAME_CAPACITY];
};

struct service_metadata {
	char type[SERVICE_TYPE_CAPACITY];
	char command[SERVICE_COMMAND_CAPACITY];
	char arguments[SERVICE_ARGUMENTS_CAPACITY];
	char restart[SERVICE_RESTART_CAPACITY];
};

struct service_command_result {
	struct zsv1_response response;
	struct service_metadata metadata;
};

static int
default_zsv1_call(void *opaque, const char *path,
		  const struct zsv1_request *request,
		  struct zsv1_response *response)
{
	(void)opaque;
	return zsv1_client_call(path, request, response);
}

void
service_command_context_init(struct service_command_context *context)
{
	if (context == NULL)
		return;
	memset(context, 0, sizeof(*context));
	context->rcconf_path = RCCONF_PATH;
	context->service_directory = SERVICE_DEFINITION_DIRECTORY;
	context->init_socket = ZSV1_INIT_SOCKET;
	context->effective_uid = geteuid();
	context->output = stdout;
	context->error = stderr;
	context->zsv1_call = default_zsv1_call;
}

void
service_command_print_usage(FILE *stream)
{
	if (stream == NULL)
		return;
	(void)fprintf(stream, "usage: service list\n"
			      "       service show [name]\n"
			      "       service status name\n"
			      "       service start name\n"
			      "       service stop name\n"
			      "       service restart name\n"
			      "       service enable name\n"
			      "       service disable name\n"
			      "       service reload\n");
}

static int
parse_command(int argc, char *const argv[], struct service_command *command,
	      FILE *error)
{
	const char *verb;
	int needs_name = 0;

	if (argc < 1 || argv == NULL || command == NULL) {
		service_command_print_usage(error);
		return -1;
	}
	memset(command, 0, sizeof(*command));
	verb = argv[0];
	if (strcmp(verb, "list") == 0) {
		command->operation = SERVICE_OPERATION_LIST;
		if (argc != 1)
			goto arity;
	} else if (strcmp(verb, "show") == 0) {
		if (argc == 1)
			command->operation = SERVICE_OPERATION_LIST;
		else if (argc == 2) {
			command->operation = SERVICE_OPERATION_SHOW;
			needs_name = 1;
		} else {
			goto arity;
		}
	} else if (strcmp(verb, "status") == 0) {
		command->operation = SERVICE_OPERATION_SHOW;
		needs_name = 1;
		if (argc != 2)
			goto arity;
	} else if (strcmp(verb, "start") == 0) {
		command->operation = SERVICE_OPERATION_START;
		needs_name = 1;
		if (argc != 2)
			goto arity;
	} else if (strcmp(verb, "stop") == 0) {
		command->operation = SERVICE_OPERATION_STOP;
		needs_name = 1;
		if (argc != 2)
			goto arity;
	} else if (strcmp(verb, "restart") == 0) {
		command->operation = SERVICE_OPERATION_RESTART;
		needs_name = 1;
		if (argc != 2)
			goto arity;
	} else if (strcmp(verb, "enable") == 0) {
		command->operation = SERVICE_OPERATION_ENABLE;
		needs_name = 1;
		if (argc != 2)
			goto arity;
	} else if (strcmp(verb, "disable") == 0) {
		command->operation = SERVICE_OPERATION_DISABLE;
		needs_name = 1;
		if (argc != 2)
			goto arity;
	} else if (strcmp(verb, "reload") == 0) {
		command->operation = SERVICE_OPERATION_RELOAD;
		if (argc != 1)
			goto arity;
	} else {
		(void)fprintf(error, "service: unknown command: %s\n", verb);
		service_command_print_usage(error);
		return -1;
	}
	if (needs_name) {
		if (!zsv1_name_valid(argv[1])) {
			(void)fprintf(error,
				      "service: invalid service name: %s\n",
				      argv[1]);
			return -1;
		}
		strcpy(command->name, argv[1]);
	}
	return 0;

arity:
	(void)fprintf(error, "service: invalid arguments for %s\n", verb);
	service_command_print_usage(error);
	return -1;
}

static int
context_valid(const struct service_command_context *context)
{
	return context != NULL && context->rcconf_path != NULL &&
	       context->service_directory != NULL &&
	       context->init_socket != NULL && context->output != NULL &&
	       context->error != NULL && context->zsv1_call != NULL;
}

static int
response_has_error(const struct zsv1_response *response)
{
	return response->error_present && !response->ok_present &&
	       response->service_count == 0 && response->dependency_count == 0;
}

static int
response_bounds_valid(const struct zsv1_response *response)
{
	return response->service_count <= ZSV1_SERVICE_MAX &&
	       response->dependency_count <= ZSV1_DEPENDENCY_MAX &&
	       (!response->ok_present ||
		memchr(response->ok_token, '\0', sizeof(response->ok_token)) !=
		    NULL) &&
	       (!response->error_present ||
		memchr(response->error_reason, '\0',
		       sizeof(response->error_reason)) != NULL);
}

static int
call_init(struct service_command_context *context,
	  const struct zsv1_request *request, struct zsv1_response *response,
	  int policy_changed)
{
	if (context->zsv1_call(context->zsv1_opaque, context->init_socket,
			       request, response) != 0) {
		if (policy_changed)
			(void)fprintf(
			    context->error,
			    "service: persistent policy changed; init "
			    "reload failed: %s; runtime policy may "
			    "remain stale\n",
			    strerror(errno));
		else
			(void)fprintf(context->error,
				      "service: init request failed: %s\n",
				      strerror(errno));
		return -1;
	}
	if (!response->ended || !response_bounds_valid(response)) {
		if (policy_changed)
			(void)fprintf(
			    context->error,
			    "service: persistent policy changed; init "
			    "reload returned an invalid ZSV1 response; "
			    "runtime policy may remain stale\n");
		else
			(void)fprintf(context->error,
				      "service: invalid ZSV1 response\n");
		return -1;
	}
	if (response->error_present) {
		if (!response_has_error(response)) {
			if (policy_changed)
				(void)fprintf(
				    context->error,
				    "service: persistent policy changed; init "
				    "reload returned an invalid ZSV1 response; "
				    "runtime policy may remain stale\n");
			else
				(void)fprintf(
				    context->error,
				    "service: invalid ZSV1 response\n");
			return -1;
		}
		if (policy_changed)
			(void)fprintf(
			    context->error,
			    "service: persistent policy changed; init "
			    "reload failed: %s (errno %d); runtime "
			    "policy may remain stale\n",
			    response->error_reason, response->error_number);
		else
			(void)fprintf(context->error,
				      "service: init rejected request: %s "
				      "(errno %d)\n",
				      response->error_reason,
				      response->error_number);
		return -1;
	}
	return 0;
}

static int
request_init(struct service_command_context *context, enum zsv1_command command,
	     const char *name, struct zsv1_response *response,
	     int policy_changed)
{
	struct zsv1_request request;

	memset(&request, 0, sizeof(request));
	request.command = command;
	if (name != NULL)
		strcpy(request.service, name);
	memset(response, 0, sizeof(*response));
	return call_init(context, &request, response, policy_changed);
}

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
print_table_header(FILE *output)
{
	return fprintf(output, "%-11s %-9s %-9s %s\n", "NAME", "STATUS",
		       "ENABLED", "PID") < 0
		   ? -1
		   : 0;
}

static int
service_record_valid(const struct zsv1_service_record *service)
{
	return service != NULL &&
	       memchr(service->name, '\0', sizeof(service->name)) != NULL &&
	       zsv1_name_valid(service->name) &&
	       zsv1_state_name(service->state) != NULL &&
	       (service->enabled == 0 || service->enabled == 1) &&
	       service->pid >= 0;
}

static int
print_service(FILE *output, const struct zsv1_service_record *service)
{
	const char *state = zsv1_state_name(service->state);

	if (state == NULL)
		return -1;
	if (service->pid > 0)
		return fprintf(output, "%-11s %-9s %-9s %ld\n", service->name,
			       state, service->enabled ? "yes" : "no",
			       (long)service->pid) < 0
			   ? -1
			   : 0;
	return fprintf(output, "%-11s %-9s %-9s -\n", service->name, state,
		       service->enabled ? "yes" : "no") < 0
		   ? -1
		   : 0;
}

static int
print_list(struct service_command_context *context,
	   const struct zsv1_response *response)
{
	struct zsv1_service_record services[ZSV1_SERVICE_MAX];
	size_t index;

	if (response->ok_present || response->dependency_count != 0)
		goto invalid;
	memcpy(services, response->services,
	       response->service_count * sizeof(services[0]));
	for (index = 0; index < response->service_count; index++) {
		if (!service_record_valid(&services[index]))
			goto invalid;
	}
	qsort(services, response->service_count, sizeof(services[0]),
	      service_record_compare);
	if (print_table_header(context->output) != 0)
		return -1;
	for (index = 0; index < response->service_count; index++) {
		if (print_service(context->output, &services[index]) != 0)
			return -1;
	}
	return 0;

invalid:
	(void)fprintf(context->error, "service: invalid ZSV1 LIST response\n");
	return -1;
}

static int
optional_assignment(const char *path, const char *key, char *output,
		    size_t capacity)
{
	if (assignment_get(path, key, output, capacity) == 0)
		return 1;
	if (errno == ENOENT) {
		output[0] = '\0';
		return 0;
	}
	return -1;
}

static int
load_metadata(const struct service_command_context *context, const char *name,
	      struct service_metadata *metadata)
{
	char path[SERVICE_PATH_CAPACITY], value[SERVICE_TYPE_CAPACITY];
	int result;

	if (snprintf(path, sizeof(path), "%s/%s", context->service_directory,
		     name) >= (int)sizeof(path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	memset(metadata, 0, sizeof(*metadata));
	if (assignment_get(path, "command", metadata->command,
			   sizeof(metadata->command)) != 0)
		return -1;
	if (metadata->command[0] != '/') {
		errno = EINVAL;
		return -1;
	}
	if (optional_assignment(path, "arguments", metadata->arguments,
				sizeof(metadata->arguments)) < 0)
		return -1;
	strcpy(metadata->type, "daemon");
	result = optional_assignment(path, "type", value, sizeof(value));
	if (result < 0)
		return -1;
	if (result > 0) {
		if (strcmp(value, "daemon") != 0 &&
		    strcmp(value, "oneshot") != 0 &&
		    strcmp(value, "respawn") != 0) {
			errno = EINVAL;
			return -1;
		}
		strcpy(metadata->type, value);
	}
	strcpy(metadata->restart, "no");
	result = optional_assignment(path, "restart", value, sizeof(value));
	if (result < 0)
		return -1;
	if (result > 0) {
		if (strcmp(value, "no") != 0 && strcmp(value, "always") != 0 &&
		    strcmp(value, "on-failure") != 0) {
			errno = EINVAL;
			return -1;
		}
		strcpy(metadata->restart, value);
	}
	return 0;
}

static int
print_dependency(FILE *output, const char *label,
		 const struct zsv1_dependency_record *dependencies,
		 size_t count, enum zsv1_dependency_type type)
{
	size_t index;
	int first = 1;

	if (fprintf(output, "%-12s", label) < 0)
		return -1;
	for (index = 0; index < count; index++) {
		if (dependencies[index].type != type)
			continue;
		if (fprintf(output, "%s%s", first ? "" : ",",
			    dependencies[index].name) < 0)
			return -1;
		first = 0;
	}
	return fprintf(output, "%s\n", first ? "-" : "") < 0 ? -1 : 0;
}

static int
print_detail(struct service_command_context *context, const char *name,
	     struct service_command_result *result)
{
	struct zsv1_response *response = &result->response;
	struct zsv1_dependency_record dependencies[ZSV1_DEPENDENCY_MAX];
	const struct zsv1_service_record *service;
	size_t index;

	if (response->ok_present || response->service_count != 1 ||
	    !service_record_valid(&response->services[0]) ||
	    strcmp(response->services[0].name, name) != 0)
		goto invalid;
	service = &response->services[0];
	memcpy(dependencies, response->dependencies,
	       response->dependency_count * sizeof(dependencies[0]));
	for (index = 0; index < response->dependency_count; index++) {
		if (memchr(dependencies[index].name, '\0',
			   sizeof(dependencies[index].name)) == NULL ||
		    (dependencies[index].type != ZSV1_DEPENDENCY_AFTER &&
		     dependencies[index].type != ZSV1_DEPENDENCY_REQUIRES) ||
		    !zsv1_name_valid(dependencies[index].name))
			goto invalid;
	}
	qsort(dependencies, response->dependency_count, sizeof(dependencies[0]),
	      dependency_record_compare);
	if (print_table_header(context->output) != 0 ||
	    print_service(context->output, service) != 0 ||
	    fprintf(context->output, "\n%-12s%s\n%-12s%s%s%s\n%-12s%s\n",
		    "TYPE", result->metadata.type, "COMMAND",
		    result->metadata.command,
		    result->metadata.arguments[0] != '\0' ? " " : "",
		    result->metadata.arguments, "RESTART",
		    result->metadata.restart) < 0 ||
	    print_dependency(context->output, "AFTER", dependencies,
			     response->dependency_count,
			     ZSV1_DEPENDENCY_AFTER) != 0 ||
	    print_dependency(context->output, "REQUIRES", dependencies,
			     response->dependency_count,
			     ZSV1_DEPENDENCY_REQUIRES) != 0)
		return -1;
	return 0;

invalid:
	(void)fprintf(context->error, "service: invalid ZSV1 SHOW response\n");
	return -1;
}

static const char *
expected_ok(enum service_operation operation)
{
	switch (operation) {
	case SERVICE_OPERATION_START:
		return "started";
	case SERVICE_OPERATION_STOP:
		return "stopped";
	case SERVICE_OPERATION_RESTART:
		return "restarted";
	case SERVICE_OPERATION_RELOAD:
		return "reloaded";
	default:
		return NULL;
	}
}

static enum zsv1_command
runtime_command(enum service_operation operation)
{
	switch (operation) {
	case SERVICE_OPERATION_START:
		return ZSV1_COMMAND_START;
	case SERVICE_OPERATION_STOP:
		return ZSV1_COMMAND_STOP;
	case SERVICE_OPERATION_RESTART:
		return ZSV1_COMMAND_RESTART;
	default:
		return ZSV1_COMMAND_RELOAD;
	}
}

static int
response_is_ok(const struct zsv1_response *response, const char *token)
{
	return response->ended && !response->error_present &&
	       response->ok_present && strcmp(response->ok_token, token) == 0 &&
	       response->service_count == 0 && response->dependency_count == 0;
}

static int
run_runtime(struct service_command_context *context,
	    const struct service_command *command)
{
	struct zsv1_response response;
	const char *token = expected_ok(command->operation);
	const char *name = command->name[0] != '\0' ? command->name : NULL;

	if (request_init(context, runtime_command(command->operation), name,
			 &response, 0) != 0)
		return 1;
	if (token == NULL || !response_is_ok(&response, token)) {
		(void)fprintf(context->error,
			      "service: invalid ZSV1 response\n");
		return 1;
	}
	if (command->operation == SERVICE_OPERATION_RELOAD)
		return fprintf(context->output, "OK reloaded\n") < 0 ? 1 : 0;
	return fprintf(context->output, "OK %s\n", token) < 0 ? 1 : 0;
}

static int
run_policy(struct service_command_context *context,
	   const struct service_command *command)
{
	struct service_command_result result;
	struct zsv1_response reload_response;
	int enabled = command->operation == SERVICE_OPERATION_ENABLE;

	if (load_metadata(context, command->name, &result.metadata) != 0) {
		(void)fprintf(
		    context->error,
		    "service: invalid or missing definition for %s: %s\n",
		    command->name, strerror(errno));
		return 1;
	}
	if (request_init(context, ZSV1_COMMAND_SHOW, command->name,
			 &result.response, 0) != 0)
		return 1;
	if (result.response.ok_present || result.response.service_count != 1 ||
	    !service_record_valid(&result.response.services[0]) ||
	    strcmp(result.response.services[0].name, command->name) != 0) {
		(void)fprintf(context->error,
			      "service: invalid ZSV1 SHOW response\n");
		return 1;
	}
	if (rcconf_set_enabled(context->rcconf_path, command->name, enabled) !=
	    0) {
		(void)fprintf(context->error, "service: cannot update %s: %s\n",
			      context->rcconf_path, strerror(errno));
		return 1;
	}
	if (request_init(context, ZSV1_COMMAND_RELOAD, NULL, &reload_response,
			 1) != 0)
		return 1;
	if (!response_is_ok(&reload_response, "reloaded")) {
		(void)fprintf(
		    context->error,
		    "service: persistent policy changed; init reload "
		    "returned an invalid ZSV1 response; runtime policy "
		    "may remain stale\n");
		return 1;
	}
	return fprintf(context->output, "OK %s %s\n",
		       enabled ? "enabled" : "disabled", command->name) < 0
		   ? 1
		   : 0;
}

int
service_command_dispatch(struct service_command_context *context, int argc,
			 char *const argv[])
{
	struct service_command command;
	struct service_command_result result;

	if (!context_valid(context)) {
		errno = EINVAL;
		return 1;
	}
	if (parse_command(argc, argv, &command, context->error) != 0)
		return 2;
	if (context->effective_uid != 0) {
		(void)fprintf(context->error,
			      "service: effective UID 0 is required\n");
		return 1;
	}
	if (command.operation == SERVICE_OPERATION_LIST) {
		if (request_init(context, ZSV1_COMMAND_LIST, NULL,
				 &result.response, 0) != 0)
			return 1;
		return print_list(context, &result.response) == 0 ? 0 : 1;
	}
	if (command.operation == SERVICE_OPERATION_SHOW) {
		if (request_init(context, ZSV1_COMMAND_SHOW, command.name,
				 &result.response, 0) != 0)
			return 1;
		if (load_metadata(context, command.name, &result.metadata) !=
		    0) {
			(void)fprintf(context->error,
				      "service: invalid or missing definition "
				      "for %s: %s\n",
				      command.name, strerror(errno));
			return 1;
		}
		return print_detail(context, command.name, &result) == 0 ? 0
									 : 1;
	}
	if (command.operation == SERVICE_OPERATION_ENABLE ||
	    command.operation == SERVICE_OPERATION_DISABLE)
		return run_policy(context, &command);
	return run_runtime(context, &command);
}
