/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements shared userland service service command support.
 */

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

static int context_valid(const struct service_command_context *context);
static int parse_command(int argc, char *const argv[], struct service_command *command, FILE *error);
static int request_init(struct service_command_context *context, enum zsv1_command command, const char *name, struct zsv1_response *response, int policy_changed);
static int call_init(struct service_command_context *context, const struct zsv1_request *request, struct zsv1_response *response, int policy_changed);
static int response_bounds_valid(const struct zsv1_response *response);
static int response_has_error(const struct zsv1_response *response);
static int print_list(struct service_command_context *context, const struct zsv1_response *response);
static int service_record_valid(const struct zsv1_service_record *service);
static int print_table_header(FILE *output);
static int print_service(FILE *output, const struct zsv1_service_record *service);
static int load_metadata(const struct service_command_context *context, const char *name, struct service_metadata *metadata);
static int optional_assignment(const char *path, const char *key, char *output, size_t capacity);
static int print_detail(struct service_command_context *context, const char *name, struct service_command_result *result);
static int print_dependency(FILE *output, const char *label, const struct zsv1_dependency_record *dependencies, size_t count, enum zsv1_dependency_type type);
static int run_policy(struct service_command_context *context, const struct service_command *command);
static int response_is_ok(const struct zsv1_response *response, const char *token);
static int run_runtime(struct service_command_context *context, const struct service_command *command);
static const char *expected_ok(enum service_operation operation);
static enum zsv1_command runtime_command(enum service_operation operation);
static int default_zsv1_call(void *opaque, const char *path, const struct zsv1_request *request, struct zsv1_response *response);
static int service_record_compare(const void *left, const void *right);
static int dependency_record_compare(const void *left, const void *right);

/*
 * Implements the service command context init operation.
 */
void
service_command_context_init(
	struct service_command_context *context)
{
	/* Handles the context availability. */
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

/*
 * Implements the service command print usage operation.
 */
void
service_command_print_usage(
	FILE *stream)
{
	/* Handles the stream availability. */
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

/*
 * Implements the service command dispatch operation.
 */
int
service_command_dispatch(
	struct service_command_context *context,
	int argc,
	char *const argv[])
{
	int function_result;
	struct service_command command;
	struct service_command_result result;

	/* Handles a failed context valid operation. */
	if (!context_valid(context)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return 1;
	}

	/* Validates the command-line arguments. */
	if (parse_command(argc, argv, &command, context->error) != 0)
		return 2;

	/* Handles the context condition. */
	if (context->effective_uid != 0) {
		(void)fprintf(context->error,
			      "service: effective UID 0 is required\n");

		/* Reports operation failure. */
		return 1;
	}

	/* Handles the command condition. */
	if (command.operation == SERVICE_OPERATION_LIST) {
		/* Handles a failed request init operation. */
		if (request_init(context, ZSV1_COMMAND_LIST, NULL,
				 &result.response, 0) != 0)

			/* Reports operation failure. */
			return 1;

		/* Computes the function result. */
		function_result = print_list(context, &result.response) == 0 ? 0 : 1;

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the command condition. */
	if (command.operation == SERVICE_OPERATION_SHOW) {
		/* Handles a failed request init operation. */
		if (request_init(context, ZSV1_COMMAND_SHOW, command.name,
				 &result.response, 0) != 0)

			/* Reports operation failure. */
			return 1;

		/* Handles a failed load metadata operation. */
		if (load_metadata(context, command.name, &result.metadata) !=
		    0) {
			(void)fprintf(context->error,
				      "service: invalid or missing definition "
				      "for %s: %s\n",
				      command.name, strerror(errno));

			/* Reports operation failure. */
			return 1;
		}

		/* Computes the function result. */
		function_result = print_detail(context, command.name, &result) == 0 ? 0
									 : 1;

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the command condition. */
	if (command.operation == SERVICE_OPERATION_ENABLE ||
	    command.operation == SERVICE_OPERATION_DISABLE) {
		/* Obtains the run policy result. */
		function_result = run_policy(context, &command);

		/* Returns the computed result. */
		return function_result;
	}

	/* Obtains the run runtime result. */
	function_result = run_runtime(context, &command);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the context valid operation. */
static int
context_valid(
	const struct service_command_context *context)
{
	/* Returns the computed result. */
	return context != NULL && context->rcconf_path != NULL &&
	       context->service_directory != NULL &&
	       context->init_socket != NULL && context->output != NULL &&
	       context->error != NULL && context->zsv1_call != NULL;
}

/* Supports the parse command operation. */
static int
parse_command(
	int argc,
	char *const argv[],
	struct service_command *command,
	FILE *error)
{
	const char *verb;
	int needs_name;

	needs_name = 0;

	/* Validates the command-line arguments. */
	if (argc < 1 || argv == NULL || command == NULL) {
		service_command_print_usage(error);

		/* Reports operation failure. */
		return -1;
	}
	memset(command, 0, sizeof(*command));
	verb = argv[0];

	/* Selects the matching value. */
	if (strcmp(verb, "list") == 0) {
		command->operation = SERVICE_OPERATION_LIST;

		/* Validates the command-line arguments. */
		if (argc != 1)
			goto arity;
	} else if (strcmp(verb, "show") == 0) {
		/* Validates the command-line arguments. */
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

		/* Validates the command-line arguments. */
		if (argc != 2)
			goto arity;
	} else if (strcmp(verb, "start") == 0) {
		command->operation = SERVICE_OPERATION_START;
		needs_name = 1;

		/* Validates the command-line arguments. */
		if (argc != 2)
			goto arity;
	} else if (strcmp(verb, "stop") == 0) {
		command->operation = SERVICE_OPERATION_STOP;
		needs_name = 1;

		/* Validates the command-line arguments. */
		if (argc != 2)
			goto arity;
	} else if (strcmp(verb, "restart") == 0) {
		command->operation = SERVICE_OPERATION_RESTART;
		needs_name = 1;

		/* Validates the command-line arguments. */
		if (argc != 2)
			goto arity;
	} else if (strcmp(verb, "enable") == 0) {
		command->operation = SERVICE_OPERATION_ENABLE;
		needs_name = 1;

		/* Validates the command-line arguments. */
		if (argc != 2)
			goto arity;
	} else if (strcmp(verb, "disable") == 0) {
		command->operation = SERVICE_OPERATION_DISABLE;
		needs_name = 1;

		/* Validates the command-line arguments. */
		if (argc != 2)
			goto arity;
	} else if (strcmp(verb, "reload") == 0) {
		command->operation = SERVICE_OPERATION_RELOAD;

		/* Validates the command-line arguments. */
		if (argc != 1)
			goto arity;
	} else {
		(void)fprintf(error, "service: unknown command: %s\n", verb);
		service_command_print_usage(error);

		/* Reports operation failure. */
		return -1;
	}

	/* Handles the name condition. */
	if (needs_name) {
		/* Validates the command-line arguments. */
		if (!zsv1_name_valid(argv[1])) {
			(void)fprintf(error,
				      "service: invalid service name: %s\n",
				      argv[1]);

			/* Reports operation failure. */
			return -1;
		}
		strcpy(command->name, argv[1]);
	}

	/* Reports successful completion. */
	return 0;

arity:
	(void)fprintf(error, "service: invalid arguments for %s\n", verb);
	service_command_print_usage(error);

	/* Reports operation failure. */
	return -1;
}

/* Supports the request init operation. */
static int
request_init(
	struct service_command_context *context,
	enum zsv1_command command,
	const char *name,
	struct zsv1_response *response,
	int policy_changed)
{
	int function_result;
	struct zsv1_request request;

	memset(&request, 0, sizeof(request));
	request.command = command;

	/* Handles the name availability. */
	if (name != NULL)
		strcpy(request.service, name);
	memset(response, 0, sizeof(*response));

	/* Obtains the call init result. */
	function_result = call_init(context, &request, response, policy_changed);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the call init operation. */
static int
call_init(
	struct service_command_context *context,
	const struct zsv1_request *request,
	struct zsv1_response *response,
	int policy_changed)
{
	/* Handles a failed zsv1 call operation. */
	if (context->zsv1_call(context->zsv1_opaque, context->init_socket,
			       request, response) != 0) {
		/* Handles the policy changed condition. */
		if (policy_changed) {
			(void)fprintf(
			    context->error,
			    "service: persistent policy changed; init "
			    "reload failed: %s; runtime policy may "
			    "remain stale\n",
			    strerror(errno));
		} else {
			(void)fprintf(context->error,
				      "service: init request failed: %s\n",
				      strerror(errno));
		}

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed response bounds valid operation. */
	if (!response->ended || !response_bounds_valid(response)) {
		/* Handles the policy changed condition. */
		if (policy_changed) {
			(void)fprintf(
			    context->error,
			    "service: persistent policy changed; init "
			    "reload returned an invalid ZSV1 response; "
			    "runtime policy may remain stale\n");
		} else {
			(void)fprintf(context->error,
				      "service: invalid ZSV1 response\n");
		}

		/* Reports operation failure. */
		return -1;
	}

	/* Handles an operation failure. */
	if (response->error_present) {
		/* Handles an operation failure. */
		if (!response_has_error(response)) {
			/* Handles the policy changed condition. */
			if (policy_changed) {
				(void)fprintf(
				    context->error,
				    "service: persistent policy changed; init "
				    "reload returned an invalid ZSV1 response; "
				    "runtime policy may remain stale\n");
			} else {
				(void)fprintf(
				    context->error,
				    "service: invalid ZSV1 response\n");
			}

			/* Reports operation failure. */
			return -1;
		}

		/* Handles the policy changed condition. */
		if (policy_changed) {
			(void)fprintf(
			    context->error,
			    "service: persistent policy changed; init "
			    "reload failed: %s (errno %d); runtime "
			    "policy may remain stale\n",
			    response->error_reason, response->error_number);
		} else {
			(void)fprintf(context->error,
				      "service: init rejected request: %s "
				      "(errno %d)\n",
				      response->error_reason,
				      response->error_number);
		}

		/* Reports operation failure. */
		return -1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the response bounds valid operation. */
static int
response_bounds_valid(
	const struct zsv1_response *response)
{
	int function_result;

	/* Computes the function result. */
	function_result = response->service_count <= ZSV1_SERVICE_MAX &&
	       response->dependency_count <= ZSV1_DEPENDENCY_MAX &&
	       (!response->ok_present ||
		memchr(response->ok_token, '\0', sizeof(response->ok_token)) !=
		    NULL) &&
	       (!response->error_present ||
		memchr(response->error_reason, '\0',
		       sizeof(response->error_reason)) != NULL);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the response has error operation. */
static int
response_has_error(
	const struct zsv1_response *response)
{
	/* Returns the computed result. */
	return response->error_present && !response->ok_present &&
	       response->service_count == 0 && response->dependency_count == 0;
}

/* Supports the print list operation. */
static int
print_list(
	struct service_command_context *context,
	const struct zsv1_response *response)
{
	struct zsv1_service_record services[ZSV1_SERVICE_MAX];
	size_t index;

	/* Handles the response condition. */
	if (response->ok_present || response->dependency_count != 0)
		goto invalid;
	memcpy(services, response->services,
	       response->service_count * sizeof(services[0]));

	/* Process each remaining element. */
	for (index = 0; index < response->service_count; index++) {
		/* Handles a failed service record valid operation. */
		if (!service_record_valid(&services[index]))
			goto invalid;
	}
	qsort(services, response->service_count, sizeof(services[0]),
	      service_record_compare);

	/* Handles a failed print table header operation. */
	if (print_table_header(context->output) != 0)
		return -1;

	/* Process each remaining element. */
	for (index = 0; index < response->service_count; index++) {
		/* Handles a failed print service operation. */
		if (print_service(context->output, &services[index]) != 0)
			return -1;
	}

	/* Reports successful completion. */
	return 0;

invalid:
	(void)fprintf(context->error, "service: invalid ZSV1 LIST response\n");

	/* Reports operation failure. */
	return -1;
}

/* Supports the service record valid operation. */
static int
service_record_valid(
	const struct zsv1_service_record *service)
{
	int function_result;

	/* Computes the function result. */
	function_result = service != NULL &&
	       memchr(service->name, '\0', sizeof(service->name)) != NULL &&
	       zsv1_name_valid(service->name) &&
	       zsv1_state_name(service->state) != NULL &&
	       (service->enabled == 0 || service->enabled == 1) &&
	       service->pid >= 0;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the print table header operation. */
static int
print_table_header(
	FILE *output)
{
	int function_result;

	/* Computes the function result. */
	function_result = fprintf(output, "%-11s %-9s %-9s %s\n", "NAME", "STATUS",
		       "ENABLED", "PID") < 0
		   ? -1
		   : 0;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the print service operation. */
static int
print_service(
	FILE *output,
	const struct zsv1_service_record *service)
{
	int function_result;
	const char *state;

	state = zsv1_state_name(service->state);

	/* Handles the state availability. */
	if (state == NULL)
		return -1;

	/* Handles the service condition. */
	if (service->pid > 0) {
		/* Computes the function result. */
		function_result = fprintf(output, "%-11s %-9s %-9s %ld\n", service->name,
			       state, service->enabled ? "yes" : "no",
			       (long)service->pid) < 0
			   ? -1
			   : 0;

		/* Returns the computed result. */
		return function_result;
	}

	/* Computes the function result. */
	function_result = fprintf(output, "%-11s %-9s %-9s -\n", service->name, state,
		       service->enabled ? "yes" : "no") < 0
		   ? -1
		   : 0;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the load metadata operation. */
static int
load_metadata(
	const struct service_command_context *context,
	const char *name,
	struct service_metadata *metadata)
{
	char path[SERVICE_PATH_CAPACITY], value[SERVICE_TYPE_CAPACITY];
	int result;

	/* Handles a failed snprintf operation. */
	if (snprintf(path, sizeof(path), "%s/%s", context->service_directory,
		     name) >= (int)sizeof(path)) {
		errno = ENAMETOOLONG;

		/* Reports operation failure. */
		return -1;
	}
	memset(metadata, 0, sizeof(*metadata));

	/* Handles a failed assignment get operation. */
	if (assignment_get(path, "command", metadata->command,
			   sizeof(metadata->command)) != 0)

		/* Reports operation failure. */
		return -1;

	/* Handles the metadata condition. */
	if (metadata->command[0] != '/') {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed optional assignment operation. */
	if (optional_assignment(path, "arguments", metadata->arguments,
				sizeof(metadata->arguments)) < 0)

		/* Reports operation failure. */
		return -1;
	strcpy(metadata->type, "daemon");
	result = optional_assignment(path, "type", value, sizeof(value));

	/* Checks the operation result. */
	if (result < 0)
		return -1;

	/* Checks the operation result. */
	if (result > 0) {
		/* Selects the matching value. */
		if (strcmp(value, "daemon") != 0 &&
		    strcmp(value, "oneshot") != 0 &&
		    strcmp(value, "respawn") != 0) {
			errno = EINVAL;

			/* Reports operation failure. */
			return -1;
		}
		strcpy(metadata->type, value);
	}
	strcpy(metadata->restart, "no");
	result = optional_assignment(path, "restart", value, sizeof(value));

	/* Checks the operation result. */
	if (result < 0)
		return -1;

	/* Checks the operation result. */
	if (result > 0) {
		/* Handles an operation failure. */
		if (strcmp(value, "no") != 0 && strcmp(value, "always") != 0 &&
		    strcmp(value, "on-failure") != 0) {
			errno = EINVAL;

			/* Reports operation failure. */
			return -1;
		}
		strcpy(metadata->restart, value);
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the optional assignment operation. */
static int
optional_assignment(
	const char *path,
	const char *key,
	char *output,
	size_t capacity)
{
	/* Handles a failed assignment get operation. */
	if (assignment_get(path, key, output, capacity) == 0)
		return 1;

	/* Handles the reported system error. */
	if (errno == ENOENT) {
		output[0] = '\0';

		/* Reports successful completion. */
		return 0;
	}

	/* Reports operation failure. */
	return -1;
}

/* Supports the print detail operation. */
static int
print_detail(
	struct service_command_context *context,
	const char *name,
	struct service_command_result *result)
{
	struct zsv1_response *response;
	struct zsv1_dependency_record dependencies[ZSV1_DEPENDENCY_MAX];
	const struct zsv1_service_record *service;
	size_t index;

	response = &result->response;

	/* Handles a failed service record valid operation. */
	if (response->ok_present || response->service_count != 1 ||
	    !service_record_valid(&response->services[0]) ||
	    strcmp(response->services[0].name, name) != 0)
		goto invalid;
	service = &response->services[0];
	memcpy(dependencies, response->dependencies,
	       response->dependency_count * sizeof(dependencies[0]));

	/* Process each remaining element. */
	for (index = 0; index < response->dependency_count; index++) {
		/* Handles a failed memchr operation. */
		if (memchr(dependencies[index].name, '\0',
			   sizeof(dependencies[index].name)) == NULL ||
		    (dependencies[index].type != ZSV1_DEPENDENCY_AFTER &&
		     dependencies[index].type != ZSV1_DEPENDENCY_REQUIRES) ||
		    !zsv1_name_valid(dependencies[index].name))
			goto invalid;
	}
	qsort(dependencies, response->dependency_count, sizeof(dependencies[0]),
	      dependency_record_compare);

	/* Handles a failed print table header operation. */
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

		/* Reports operation failure. */
		return -1;

	/* Reports successful completion. */
	return 0;

invalid:
	(void)fprintf(context->error, "service: invalid ZSV1 SHOW response\n");

	/* Reports operation failure. */
	return -1;
}

/* Supports the print dependency operation. */
static int
print_dependency(
	FILE *output,
	const char *label,
	const struct zsv1_dependency_record *dependencies,
	size_t count,
	enum zsv1_dependency_type type)
{
	int function_result;
	size_t index;
	int first;

	first = 1;

	/* Handles a failed fprintf operation. */
	if (fprintf(output, "%-12s", label) < 0)
		return -1;

	/* Process each remaining element. */
	for (index = 0; index < count; index++) {
		/* Handles the dependencies condition. */
		if (dependencies[index].type != type)
			continue;

		/* Handles a failed fprintf operation. */
		if (fprintf(output, "%s%s", first ? "" : ",",
			    dependencies[index].name) < 0)

			/* Reports operation failure. */
			return -1;
		first = 0;
	}

	/* Computes the function result. */
	function_result = fprintf(output, "%s\n", first ? "-" : "") < 0 ? -1 : 0;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the run policy operation. */
static int
run_policy(
	struct service_command_context *context,
	const struct service_command *command)
{
	int function_result;
	struct service_command_result result;
	struct zsv1_response reload_response;
	int enabled = command->operation == SERVICE_OPERATION_ENABLE;

	/* Handles a failed load metadata operation. */
	if (load_metadata(context, command->name, &result.metadata) != 0) {
		(void)fprintf(
		    context->error,
		    "service: invalid or missing definition for %s: %s\n",
		    command->name, strerror(errno));

		/* Reports operation failure. */
		return 1;
	}

	/* Handles a failed request init operation. */
	if (request_init(context, ZSV1_COMMAND_SHOW, command->name,
			 &result.response, 0) != 0)

		/* Reports operation failure. */
		return 1;

	/* Handles a failed service record valid operation. */
	if (result.response.ok_present || result.response.service_count != 1 ||
	    !service_record_valid(&result.response.services[0]) ||
	    strcmp(result.response.services[0].name, command->name) != 0) {
		(void)fprintf(context->error,
			      "service: invalid ZSV1 SHOW response\n");

		/* Reports operation failure. */
		return 1;
	}

	/* Handles a failed rcconf set enabled operation. */
	if (rcconf_set_enabled(context->rcconf_path, command->name, enabled) !=
	    0) {
		(void)fprintf(context->error, "service: cannot update %s: %s\n",
			      context->rcconf_path, strerror(errno));

		/* Reports operation failure. */
		return 1;
	}

	/* Handles a failed request init operation. */
	if (request_init(context, ZSV1_COMMAND_RELOAD, NULL, &reload_response,
			 1) != 0)

		/* Reports operation failure. */
		return 1;

	/* Handles a failed response is ok operation. */
	if (!response_is_ok(&reload_response, "reloaded")) {
		(void)fprintf(
		    context->error,
		    "service: persistent policy changed; init reload "
		    "returned an invalid ZSV1 response; runtime policy "
		    "may remain stale\n");

		/* Reports operation failure. */
		return 1;
	}

	/* Computes the function result. */
	function_result = fprintf(context->output, "OK %s %s\n",
		       enabled ? "enabled" : "disabled", command->name) < 0
		   ? 1
		   : 0;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the response is ok operation. */
static int
response_is_ok(
	const struct zsv1_response *response,
	const char *token)
{
	int function_result;

	/* Computes the function result. */
	function_result = response->ended && !response->error_present &&
	       response->ok_present && strcmp(response->ok_token, token) == 0 &&
	       response->service_count == 0 && response->dependency_count == 0;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the run runtime operation. */
static int
run_runtime(
	struct service_command_context *context,
	const struct service_command *command)
{
	int function_result;
	struct zsv1_response response;
	const char *token;
	const char *name = command->name[0] != '\0' ? command->name : NULL;

	token = expected_ok(command->operation);

	/* Handles a failed request init operation. */
	if (request_init(context, runtime_command(command->operation), name,
			 &response, 0) != 0)

		/* Reports operation failure. */
		return 1;

	/* Handles a failed response is ok operation. */
	if (token == NULL || !response_is_ok(&response, token)) {
		(void)fprintf(context->error,
			      "service: invalid ZSV1 response\n");

		/* Reports operation failure. */
		return 1;
	}

	/* Handles the command condition. */
	if (command->operation == SERVICE_OPERATION_RELOAD) {
		/* Computes the function result. */
		function_result = fprintf(context->output, "OK reloaded\n") < 0 ? 1 : 0;

		/* Returns the computed result. */
		return function_result;
	}

	/* Computes the function result. */
	function_result = fprintf(context->output, "OK %s\n", token) < 0 ? 1 : 0;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the expected ok operation. */
static const char *
expected_ok(
	enum service_operation operation)
{
	/* Dispatch the selected operation case. */
	switch (operation) {
	case SERVICE_OPERATION_START:
		/* Returns the computed result. */
		return "started";
	case SERVICE_OPERATION_STOP:
		/* Returns the computed result. */
		return "stopped";
	case SERVICE_OPERATION_RESTART:
		/* Returns the computed result. */
		return "restarted";
	case SERVICE_OPERATION_RELOAD:
		/* Returns the computed result. */
		return "reloaded";
	default:
		/* Reports that no result is available. */
		return NULL;
	}
}

/* Supports the runtime command operation. */
static enum zsv1_command
runtime_command(
	enum service_operation operation)
{
	/* Dispatch the selected operation case. */
	switch (operation) {
	case SERVICE_OPERATION_START:
		/* Returns the computed result. */
		return ZSV1_COMMAND_START;
	case SERVICE_OPERATION_STOP:
		/* Returns the computed result. */
		return ZSV1_COMMAND_STOP;
	case SERVICE_OPERATION_RESTART:
		/* Returns the computed result. */
		return ZSV1_COMMAND_RESTART;
	default:
		/* Returns the computed result. */
		return ZSV1_COMMAND_RELOAD;
	}
}

/* Supports the default zsv1 call operation. */
static int
default_zsv1_call(
	void *opaque,
	const char *path,
	const struct zsv1_request *request,
	struct zsv1_response *response)
{
	int function_result;

	(void)opaque;

	/* Obtains the zsv1 client call result. */
	function_result = zsv1_client_call(path, request, response);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the service record compare operation. */
static int
service_record_compare(
	const void *left,
	const void *right)
{
	int function_result;
	const struct zsv1_service_record *a, *b;

	a = left;
	b = right;

	/* Obtains the strcmp result. */
	function_result = strcmp(a->name, b->name);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the dependency record compare operation. */
static int
dependency_record_compare(
	const void *left,
	const void *right)
{
	int function_result;
	const struct zsv1_dependency_record *a, *b;

	a = left;
	b = right;

	/* Handles the a condition. */
	if (a->type != b->type)
		return a->type < b->type ? -1 : 1;

	/* Obtains the strcmp result. */
	function_result = strcmp(a->name, b->name);

	/* Returns the computed result. */
	return function_result;
}
