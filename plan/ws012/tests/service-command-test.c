/*
 * WS012 SVC-T004 production service-command fixture
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#define _POSIX_C_SOURCE 200809L

#include "userland/base/service/rcconf.h"
#include "userland/base/service/service-command.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define ARRAY_LENGTH(array) (sizeof(array) / sizeof((array)[0]))
#define CALL_MAX 8

static const char usage_text[] = "usage: service list\n"
				 "       service show [name]\n"
				 "       service status name\n"
				 "       service start name\n"
				 "       service stop name\n"
				 "       service restart name\n"
				 "       service enable name\n"
				 "       service disable name\n"
				 "       service reload\n";

struct fake_zsv1 {
	struct zsv1_request calls[CALL_MAX];
	size_t call_count;
	struct zsv1_response list_response;
	struct zsv1_response show_response;
	int custom_show;
	int transport_command;
	int transport_error;
	int typed_error_command;
	int typed_error_number;
	char typed_error_reason[ZSV1_TOKEN_CAPACITY];
	int wrong_token_command;
	char wrong_token[ZSV1_TOKEN_CAPACITY];
	int barrier_on_show;
	int barrier_ready;
	int barrier_go;
};

struct run_result {
	int status;
	char *output;
	char *error;
};

struct fixture {
	char directory[256];
	char rcconf[320];
	char service_directory[320];
	struct service_command_context context;
	struct fake_zsv1 backend;
};

static void
fail(const char *message)
{
	fprintf(stderr, "SVC-T004: %s (errno=%d)\n", message, errno);
	exit(1);
}

static void
require(int condition, const char *message)
{
	if (!condition)
		fail(message);
}

static char *
read_stream(FILE *stream)
{
	char *data;
	long length;

	require(fflush(stream) == 0 && fseek(stream, 0, SEEK_END) == 0,
		"flush captured stream");
	length = ftell(stream);
	require(length >= 0 && fseek(stream, 0, SEEK_SET) == 0,
		"measure captured stream");
	data = malloc((size_t)length + 1U);
	require(data != NULL, "allocate captured stream");
	require(fread(data, 1, (size_t)length, stream) == (size_t)length,
		"read captured stream");
	data[length] = '\0';
	return data;
}

static char *
read_path(const char *path)
{
	FILE *stream = fopen(path, "rb");
	char *data;

	require(stream != NULL, "open captured path");
	data = read_stream(stream);
	require(fclose(stream) == 0, "close captured path");
	return data;
}

static void
write_text(const char *path, const char *text)
{
	FILE *stream = fopen(path, "w");

	require(stream != NULL && fputs(text, stream) >= 0 &&
		    fflush(stream) == 0 && fsync(fileno(stream)) == 0 &&
		    fclose(stream) == 0,
		"write fixture text");
}

static void
write_definition(struct fixture *fixture, const char *name, const char *text)
{
	char path[512];

	require(snprintf(path, sizeof(path), "%s/%s",
			 fixture->service_directory, name) < (int)sizeof(path),
		"format definition path");
	write_text(path, text);
}

static void
write_initial_rcconf(struct fixture *fixture, int cron_enabled,
		     int ntpdate_enabled)
{
	struct rcconf_model model;
	FILE *stream;

	rcconf_model_init(&model);
	strcpy(model.hostname, "zedbsd");
	require(
	    rcconf_model_set_enabled(&model, "cron", cron_enabled) == 0 &&
		rcconf_model_set_enabled(&model, "ntpdate", ntpdate_enabled) ==
		    0 &&
		rcconf_model_set_enabled(&model, "syslogd", 1) == 0 &&
		rcconf_model_set_setting(&model, "ntpdate", "servers",
					 "0.pool.ntp.org 1.pool.ntp.org") == 0,
	    "construct initial rc.conf");
	stream = fopen(fixture->rcconf, "w");
	require(stream != NULL && rcconf_write(stream, &model) == 0 &&
		    fflush(stream) == 0 && fsync(fileno(stream)) == 0 &&
		    fclose(stream) == 0,
		"write initial rc.conf");
}

static void
backend_reset(struct fake_zsv1 *backend)
{
	memset(backend, 0, sizeof(*backend));
	backend->transport_command = -1;
	backend->typed_error_command = -1;
	backend->wrong_token_command = -1;
	backend->barrier_ready = -1;
	backend->barrier_go = -1;
	backend->list_response.ended = 1;
}

static void
set_service(struct zsv1_service_record *record, const char *name,
	    enum zsv1_service_state state, int enabled, pid_t pid)
{
	memset(record, 0, sizeof(*record));
	strcpy(record->name, name);
	record->state = state;
	record->enabled = enabled;
	record->pid = pid;
}

static void
set_ok(struct zsv1_response *response, const char *token)
{
	memset(response, 0, sizeof(*response));
	response->ended = 1;
	response->ok_present = 1;
	strcpy(response->ok_token, token);
}

static void
barrier_wait(struct fake_zsv1 *backend)
{
	char byte = 'R';
	ssize_t result;

	if (!backend->barrier_on_show)
		return;
	do {
		result = write(backend->barrier_ready, &byte, 1);
	} while (result < 0 && errno == EINTR);
	if (result != 1)
		_exit(90);
	do {
		result = read(backend->barrier_go, &byte, 1);
	} while (result < 0 && errno == EINTR);
	if (result != 1 || byte != 'G')
		_exit(91);
}

static int
fake_zsv1_call(void *opaque, const char *path,
	       const struct zsv1_request *request,
	       struct zsv1_response *response)
{
	struct fake_zsv1 *backend = opaque;
	const char *token;

	if (strcmp(path, "/tmp/fake-init.sock") != 0 ||
	    backend->call_count == CALL_MAX) {
		errno = EINVAL;
		return -1;
	}
	backend->calls[backend->call_count++] = *request;
	if ((int)request->command == backend->transport_command) {
		errno = backend->transport_error;
		return -1;
	}
	if ((int)request->command == backend->typed_error_command) {
		memset(response, 0, sizeof(*response));
		response->ended = 1;
		response->error_present = 1;
		response->error_number = backend->typed_error_number;
		strcpy(response->error_reason, backend->typed_error_reason);
		return 0;
	}
	if (request->command == ZSV1_COMMAND_LIST) {
		*response = backend->list_response;
		return 0;
	}
	if (request->command == ZSV1_COMMAND_SHOW) {
		barrier_wait(backend);
		if (backend->custom_show) {
			*response = backend->show_response;
			return 0;
		}
		memset(response, 0, sizeof(*response));
		response->ended = 1;
		response->service_count = 1;
		set_service(&response->services[0], request->service,
			    ZSV1_STATE_RUNNING, 0, 73);
		return 0;
	}
	switch (request->command) {
	case ZSV1_COMMAND_START:
		token = "started";
		break;
	case ZSV1_COMMAND_STOP:
		token = "stopped";
		break;
	case ZSV1_COMMAND_RESTART:
		token = "restarted";
		break;
	case ZSV1_COMMAND_RELOAD:
		token = "reloaded";
		break;
	default:
		errno = EINVAL;
		return -1;
	}
	if ((int)request->command == backend->wrong_token_command)
		token = backend->wrong_token;
	set_ok(response, token);
	return 0;
}

static void
fixture_init(struct fixture *fixture)
{
	char template[] = "/tmp/zedbsd-service-command-XXXXXX";

	memset(fixture, 0, sizeof(*fixture));
	require(mkdtemp(template) != NULL, "create fixture directory");
	strcpy(fixture->directory, template);
	require(snprintf(fixture->rcconf, sizeof(fixture->rcconf), "%s/rc.conf",
			 fixture->directory) < (int)sizeof(fixture->rcconf) &&
		    snprintf(fixture->service_directory,
			     sizeof(fixture->service_directory), "%s/service.d",
			     fixture->directory) <
			(int)sizeof(fixture->service_directory) &&
		    mkdir(fixture->service_directory, 0700) == 0,
		"create fixture paths");
	write_definition(fixture, "cron",
			 "type=daemon\ncommand=/sbin/cron\n"
			 "restart=on-failure\n");
	write_definition(fixture, "ntpdate",
			 "type=oneshot\ncommand=/sbin/ntpdate\n"
			 "arguments=-b\nrestart=no\n");
	write_definition(fixture, "networkd",
			 "type=daemon\ncommand=/sbin/networkd\n"
			 "arguments=--foreground --debug\n"
			 "restart=on-failure\n");
	write_definition(fixture, "badmeta",
			 "type=invalid\ncommand=/sbin/badmeta\nrestart=no\n");
	write_initial_rcconf(fixture, 0, 0);
	backend_reset(&fixture->backend);
	service_command_context_init(&fixture->context);
	fixture->context.rcconf_path = fixture->rcconf;
	fixture->context.service_directory = fixture->service_directory;
	fixture->context.init_socket = "/tmp/fake-init.sock";
	fixture->context.effective_uid = 0;
	fixture->context.zsv1_call = fake_zsv1_call;
	fixture->context.zsv1_opaque = &fixture->backend;
}

static struct run_result
run_command(struct fixture *fixture, int argc, char *const argv[])
{
	struct service_command_context context = fixture->context;
	struct run_result result;
	FILE *output = tmpfile(), *error = tmpfile();

	require(output != NULL && error != NULL, "create captured streams");
	context.output = output;
	context.error = error;
	result.status = service_command_dispatch(&context, argc, argv);
	result.output = read_stream(output);
	result.error = read_stream(error);
	require(fclose(output) == 0 && fclose(error) == 0,
		"close captured streams");
	return result;
}

static void
free_result(struct run_result *result)
{
	free(result->output);
	free(result->error);
	memset(result, 0, sizeof(*result));
}

static void
expect_result(struct run_result *result, int status, const char *output,
	      const char *error, const char *message)
{
	if (result->status != status || strcmp(result->output, output) != 0 ||
	    strcmp(result->error, error) != 0) {
		fprintf(stderr,
			"SVC-T004: %s\nstatus=%d expected=%d\n"
			"stdout=[%s]\nstderr=[%s]\n",
			message, result->status, status, result->output,
			result->error);
		exit(1);
	}
}

static void
test_grammar(struct fixture *fixture)
{
	struct grammar_case {
		int argc;
		char *argv[4];
		const char *error;
	} cases[] = {
	    {0, {NULL}, usage_text},
	    {1,
	     {"bogus", NULL},
	     "service: unknown command: bogus\n"
	     "usage: service list\n"
	     "       service show [name]\n"
	     "       service status name\n"
	     "       service start name\n"
	     "       service stop name\n"
	     "       service restart name\n"
	     "       service enable name\n"
	     "       service disable name\n"
	     "       service reload\n"},
	    {2,
	     {"list", "cron", NULL},
	     "service: invalid arguments for list\n"
	     "usage: service list\n"
	     "       service show [name]\n"
	     "       service status name\n"
	     "       service start name\n"
	     "       service stop name\n"
	     "       service restart name\n"
	     "       service enable name\n"
	     "       service disable name\n"
	     "       service reload\n"},
	    {3,
	     {"show", "cron", "extra", NULL},
	     "service: invalid arguments for show\n"
	     "usage: service list\n"
	     "       service show [name]\n"
	     "       service status name\n"
	     "       service start name\n"
	     "       service stop name\n"
	     "       service restart name\n"
	     "       service enable name\n"
	     "       service disable name\n"
	     "       service reload\n"},
	    {1,
	     {"status", NULL},
	     "service: invalid arguments for status\n"
	     "usage: service list\n"
	     "       service show [name]\n"
	     "       service status name\n"
	     "       service start name\n"
	     "       service stop name\n"
	     "       service restart name\n"
	     "       service enable name\n"
	     "       service disable name\n"
	     "       service reload\n"},
	    {1,
	     {"start", NULL},
	     "service: invalid arguments for start\n"
	     "usage: service list\n"
	     "       service show [name]\n"
	     "       service status name\n"
	     "       service start name\n"
	     "       service stop name\n"
	     "       service restart name\n"
	     "       service enable name\n"
	     "       service disable name\n"
	     "       service reload\n"},
	    {3,
	     {"stop", "cron", "extra", NULL},
	     "service: invalid arguments for stop\n"
	     "usage: service list\n"
	     "       service show [name]\n"
	     "       service status name\n"
	     "       service start name\n"
	     "       service stop name\n"
	     "       service restart name\n"
	     "       service enable name\n"
	     "       service disable name\n"
	     "       service reload\n"},
	    {1,
	     {"restart", NULL},
	     "service: invalid arguments for restart\n"
	     "usage: service list\n"
	     "       service show [name]\n"
	     "       service status name\n"
	     "       service start name\n"
	     "       service stop name\n"
	     "       service restart name\n"
	     "       service enable name\n"
	     "       service disable name\n"
	     "       service reload\n"},
	    {1,
	     {"enable", NULL},
	     "service: invalid arguments for enable\n"
	     "usage: service list\n"
	     "       service show [name]\n"
	     "       service status name\n"
	     "       service start name\n"
	     "       service stop name\n"
	     "       service restart name\n"
	     "       service enable name\n"
	     "       service disable name\n"
	     "       service reload\n"},
	    {3,
	     {"disable", "cron", "extra", NULL},
	     "service: invalid arguments for disable\n"
	     "usage: service list\n"
	     "       service show [name]\n"
	     "       service status name\n"
	     "       service start name\n"
	     "       service stop name\n"
	     "       service restart name\n"
	     "       service enable name\n"
	     "       service disable name\n"
	     "       service reload\n"},
	    {2,
	     {"reload", "cron", NULL},
	     "service: invalid arguments for reload\n"
	     "usage: service list\n"
	     "       service show [name]\n"
	     "       service status name\n"
	     "       service start name\n"
	     "       service stop name\n"
	     "       service restart name\n"
	     "       service enable name\n"
	     "       service disable name\n"
	     "       service reload\n"},
	    {2,
	     {"start", "bad/name", NULL},
	     "service: invalid service name: bad/name\n"},
	};
	size_t index;

	for (index = 0; index < ARRAY_LENGTH(cases); index++) {
		struct run_result result;

		backend_reset(&fixture->backend);
		result =
		    run_command(fixture, cases[index].argc, cases[index].argv);
		expect_result(&result, 2, "", cases[index].error,
			      "grammar rejection");
		require(fixture->backend.call_count == 0,
			"grammar rejection reached backend");
		free_result(&result);
	}
}

static void
test_nonroot(struct fixture *fixture)
{
	char *list[] = {"list", NULL};
	char *enable[] = {"enable", "cron", NULL};
	char *before, *after;
	struct run_result result;

	fixture->context.effective_uid = 1000;
	backend_reset(&fixture->backend);
	result = run_command(fixture, 1, list);
	expect_result(&result, 1, "", "service: effective UID 0 is required\n",
		      "non-root list");
	require(fixture->backend.call_count == 0,
		"non-root list reached backend");
	free_result(&result);

	before = read_path(fixture->rcconf);
	backend_reset(&fixture->backend);
	result = run_command(fixture, 2, enable);
	expect_result(&result, 1, "", "service: effective UID 0 is required\n",
		      "non-root policy mutation");
	after = read_path(fixture->rcconf);
	require(fixture->backend.call_count == 0 && strcmp(before, after) == 0,
		"non-root policy mutation changed state");
	free(after);
	free(before);
	free_result(&result);
	fixture->context.effective_uid = 0;
}

static void
test_list_and_show_alias(struct fixture *fixture)
{
	static const char expected[] = "NAME        STATUS    ENABLED   PID\n"
				       "alpha       stopped   no        -\n"
				       "bravo       starting  yes       41\n"
				       "charlie     running   yes       42\n"
				       "delta       completed no        -\n"
				       "echo        failed    yes       -\n"
				       "foxtrot     skipped   no        -\n";
	char *list[] = {"list", NULL};
	char *show[] = {"show", NULL};
	struct run_result result;
	struct zsv1_response saved;

	backend_reset(&fixture->backend);
	fixture->backend.list_response.service_count = 6;
	set_service(&fixture->backend.list_response.services[0], "foxtrot",
		    ZSV1_STATE_SKIPPED, 0, 0);
	set_service(&fixture->backend.list_response.services[1], "bravo",
		    ZSV1_STATE_STARTING, 1, 41);
	set_service(&fixture->backend.list_response.services[2], "delta",
		    ZSV1_STATE_COMPLETED, 0, 0);
	set_service(&fixture->backend.list_response.services[3], "charlie",
		    ZSV1_STATE_RUNNING, 1, 42);
	set_service(&fixture->backend.list_response.services[4], "alpha",
		    ZSV1_STATE_STOPPED, 0, 0);
	set_service(&fixture->backend.list_response.services[5], "echo",
		    ZSV1_STATE_FAILED, 1, 0);
	saved = fixture->backend.list_response;
	result = run_command(fixture, 1, list);
	expect_result(&result, 0, expected, "", "six-state sorted list");
	require(fixture->backend.call_count == 1 &&
		    fixture->backend.calls[0].command == ZSV1_COMMAND_LIST,
		"list request mapping");
	free_result(&result);

	backend_reset(&fixture->backend);
	fixture->backend.list_response = saved;
	result = run_command(fixture, 1, show);
	expect_result(&result, 0, expected, "", "argument-free show alias");
	require(fixture->backend.call_count == 1 &&
		    fixture->backend.calls[0].command == ZSV1_COMMAND_LIST,
		"show alias request mapping");
	free_result(&result);
}

static void
prepare_detail_response(struct fake_zsv1 *backend)
{
	struct zsv1_response *response = &backend->show_response;

	backend_reset(backend);
	backend->custom_show = 1;
	memset(response, 0, sizeof(*response));
	response->ended = 1;
	response->service_count = 1;
	set_service(&response->services[0], "networkd", ZSV1_STATE_RUNNING, 1,
		    184);
	response->dependency_count = 4;
	response->dependencies[0].type = ZSV1_DEPENDENCY_REQUIRES;
	strcpy(response->dependencies[0].name, "zeta");
	response->dependencies[1].type = ZSV1_DEPENDENCY_AFTER;
	strcpy(response->dependencies[1].name, "syslogd");
	response->dependencies[2].type = ZSV1_DEPENDENCY_REQUIRES;
	strcpy(response->dependencies[2].name, "alpha");
	response->dependencies[3].type = ZSV1_DEPENDENCY_AFTER;
	strcpy(response->dependencies[3].name, "net");
}

static void
test_detail(struct fixture *fixture)
{
	static const char expected[] =
	    "NAME        STATUS    ENABLED   PID\n"
	    "networkd    running   yes       184\n"
	    "\n"
	    "TYPE        daemon\n"
	    "COMMAND     /sbin/networkd --foreground --debug\n"
	    "RESTART     on-failure\n"
	    "AFTER       net,syslogd\n"
	    "REQUIRES    alpha,zeta\n";
	char *show[] = {"show", "networkd", NULL};
	char *status[] = {"status", "networkd", NULL};
	struct run_result result;

	prepare_detail_response(&fixture->backend);
	result = run_command(fixture, 2, show);
	expect_result(&result, 0, expected, "", "detailed show");
	require(fixture->backend.call_count == 1 &&
		    fixture->backend.calls[0].command == ZSV1_COMMAND_SHOW &&
		    strcmp(fixture->backend.calls[0].service, "networkd") == 0,
		"detailed show request");
	free_result(&result);

	prepare_detail_response(&fixture->backend);
	result = run_command(fixture, 2, status);
	expect_result(&result, 0, expected, "", "status alias detail");
	free_result(&result);
}

static void
test_runtime(struct fixture *fixture)
{
	struct runtime_case {
		char *verb;
		int argc;
		char *name;
		enum zsv1_command command;
		const char *output;
	} cases[] = {
	    {"start", 2, "cron", ZSV1_COMMAND_START, "OK started\n"},
	    {"stop", 2, "cron", ZSV1_COMMAND_STOP, "OK stopped\n"},
	    {"restart", 2, "cron", ZSV1_COMMAND_RESTART, "OK restarted\n"},
	    {"reload", 1, NULL, ZSV1_COMMAND_RELOAD, "OK reloaded\n"},
	};
	char *before = read_path(fixture->rcconf);
	size_t index;

	for (index = 0; index < ARRAY_LENGTH(cases); index++) {
		char *argv[] = {cases[index].verb, cases[index].name, NULL};
		char *after;
		struct run_result result;

		backend_reset(&fixture->backend);
		result = run_command(fixture, cases[index].argc, argv);
		expect_result(&result, 0, cases[index].output, "",
			      "runtime success");
		require(fixture->backend.call_count == 1 &&
			    fixture->backend.calls[0].command ==
				cases[index].command,
			"runtime command mapping");
		if (cases[index].name != NULL)
			require(strcmp(fixture->backend.calls[0].service,
				       cases[index].name) == 0,
				"runtime service mapping");
		after = read_path(fixture->rcconf);
		require(strcmp(before, after) == 0,
			"runtime operation changed rc.conf");
		free(after);
		free_result(&result);
	}
	free(before);
}

static void
test_runtime_failures(struct fixture *fixture)
{
	char *start[] = {"start", "cron", NULL};
	char *stop[] = {"stop", "cron", NULL};
	char expected[256];
	struct run_result result;

	backend_reset(&fixture->backend);
	fixture->backend.wrong_token_command = ZSV1_COMMAND_START;
	strcpy(fixture->backend.wrong_token, "stopped");
	result = run_command(fixture, 2, start);
	expect_result(&result, 1, "", "service: invalid ZSV1 response\n",
		      "wrong runtime OK token");
	free_result(&result);

	backend_reset(&fixture->backend);
	fixture->backend.typed_error_command = ZSV1_COMMAND_START;
	fixture->backend.typed_error_number = EIO;
	strcpy(fixture->backend.typed_error_reason, "start-failed");
	result = run_command(fixture, 2, start);
	expect_result(
	    &result, 1, "",
	    "service: init rejected request: start-failed (errno 5)\n",
	    "typed runtime error");
	free_result(&result);

	backend_reset(&fixture->backend);
	fixture->backend.transport_command = ZSV1_COMMAND_STOP;
	fixture->backend.transport_error = ECONNREFUSED;
	require(snprintf(expected, sizeof(expected),
			 "service: init request failed: %s\n",
			 strerror(ECONNREFUSED)) < (int)sizeof(expected),
		"format transport diagnostic");
	result = run_command(fixture, 2, stop);
	expect_result(&result, 1, "", expected, "runtime transport error");
	free_result(&result);
}

static void
require_policy(struct fixture *fixture, const char *name, int wanted)
{
	struct rcconf_model model;
	char servers[RCCONF_SETTING_VALUE_CAPACITY];
	int enabled;

	require(rcconf_load(fixture->rcconf, &model) == 0 &&
		    strcmp(model.hostname, "zedbsd") == 0 &&
		    rcconf_service_enabled(&model, name, &enabled) == 0 &&
		    enabled == wanted &&
		    rcconf_service_enabled(&model, "syslogd", &enabled) == 0 &&
		    enabled == 1 &&
		    rcconf_setting_get(&model, "ntpdate", "servers", servers,
				       sizeof(servers)) == 0 &&
		    strcmp(servers, "0.pool.ntp.org 1.pool.ntp.org") == 0,
		"persistent policy or unrelated fields");
}

static void
require_canonical(struct fixture *fixture)
{
	struct rcconf_model model;
	FILE *stream = tmpfile();
	char *expected, *actual;

	require(stream != NULL && rcconf_load(fixture->rcconf, &model) == 0 &&
		    rcconf_write(stream, &model) == 0,
		"serialize canonical comparison");
	expected = read_stream(stream);
	actual = read_path(fixture->rcconf);
	require(strcmp(expected, actual) == 0, "rc.conf is not canonical");
	free(actual);
	free(expected);
	require(fclose(stream) == 0, "close canonical comparison");
}

static void
require_policy_calls(struct fake_zsv1 *backend, const char *name)
{
	require(backend->call_count == 2 &&
		    backend->calls[0].command == ZSV1_COMMAND_SHOW &&
		    strcmp(backend->calls[0].service, name) == 0 &&
		    backend->calls[1].command == ZSV1_COMMAND_RELOAD &&
		    backend->calls[1].service[0] == '\0',
		"policy command did not use SHOW then RELOAD only");
}

static void
test_policy_success(struct fixture *fixture)
{
	char *enable[] = {"enable", "cron", NULL};
	char *disable[] = {"disable", "cron", NULL};
	struct run_result result;

	write_initial_rcconf(fixture, 0, 0);
	backend_reset(&fixture->backend);
	result = run_command(fixture, 2, enable);
	expect_result(&result, 0, "OK enabled cron\n", "", "enable success");
	require_policy_calls(&fixture->backend, "cron");
	require_policy(fixture, "cron", 1);
	require_canonical(fixture);
	free_result(&result);

	write_initial_rcconf(fixture, 1, 0);
	backend_reset(&fixture->backend);
	result = run_command(fixture, 2, disable);
	expect_result(&result, 0, "OK disabled cron\n", "", "disable success");
	require_policy_calls(&fixture->backend, "cron");
	require_policy(fixture, "cron", 0);
	require_canonical(fixture);
	free_result(&result);
}

static void
test_missing_and_invalid_definition(struct fixture *fixture)
{
	char *missing[] = {"enable", "missing", NULL};
	char *invalid[] = {"enable", "badmeta", NULL};
	char *before, *after, expected[512];
	struct run_result result;

	write_initial_rcconf(fixture, 0, 0);
	before = read_path(fixture->rcconf);
	backend_reset(&fixture->backend);
	require(
	    snprintf(expected, sizeof(expected),
		     "service: invalid or missing definition for missing: %s\n",
		     strerror(ENOENT)) < (int)sizeof(expected),
	    "format missing-definition diagnostic");
	result = run_command(fixture, 2, missing);
	expect_result(&result, 1, "", expected, "missing definition");
	after = read_path(fixture->rcconf);
	require(fixture->backend.call_count == 0 && strcmp(before, after) == 0,
		"missing definition changed policy or called init");
	free(after);
	free_result(&result);

	backend_reset(&fixture->backend);
	require(
	    snprintf(expected, sizeof(expected),
		     "service: invalid or missing definition for badmeta: %s\n",
		     strerror(EINVAL)) < (int)sizeof(expected),
	    "format invalid-definition diagnostic");
	result = run_command(fixture, 2, invalid);
	expect_result(&result, 1, "", expected, "invalid definition");
	after = read_path(fixture->rcconf);
	require(fixture->backend.call_count == 0 && strcmp(before, after) == 0,
		"invalid definition changed policy or called init");
	free(after);
	free(before);
	free_result(&result);
}

static void
test_policy_reload_failures(struct fixture *fixture)
{
	enum failure_kind { TRANSPORT, TYPED, WRONG_TOKEN };
	const enum failure_kind kinds[] = {TRANSPORT, TYPED, WRONG_TOKEN};
	char *enable[] = {"enable", "cron", NULL};
	size_t index;

	for (index = 0; index < ARRAY_LENGTH(kinds); index++) {
		char expected[512];
		struct run_result result;

		write_initial_rcconf(fixture, 0, 0);
		backend_reset(&fixture->backend);
		if (kinds[index] == TRANSPORT) {
			fixture->backend.transport_command =
			    ZSV1_COMMAND_RELOAD;
			fixture->backend.transport_error = ECONNRESET;
			require(
			    snprintf(expected, sizeof(expected),
				     "service: persistent policy changed; init "
				     "reload failed: %s; runtime policy may "
				     "remain stale\n",
				     strerror(ECONNRESET)) <
				(int)sizeof(expected),
			    "format stale transport diagnostic");
		} else if (kinds[index] == TYPED) {
			fixture->backend.typed_error_command =
			    ZSV1_COMMAND_RELOAD;
			fixture->backend.typed_error_number = EIO;
			strcpy(fixture->backend.typed_error_reason,
			       "reload-failed");
			strcpy(
			    expected,
			    "service: persistent policy changed; init reload "
			    "failed: reload-failed (errno 5); runtime policy "
			    "may remain stale\n");
		} else {
			fixture->backend.wrong_token_command =
			    ZSV1_COMMAND_RELOAD;
			strcpy(fixture->backend.wrong_token, "accepted");
			strcpy(
			    expected,
			    "service: persistent policy changed; init reload "
			    "returned an invalid ZSV1 response; runtime "
			    "policy may remain stale\n");
		}
		result = run_command(fixture, 2, enable);
		expect_result(&result, 1, "", expected,
			      "post-persistence reload failure");
		require_policy_calls(&fixture->backend, "cron");
		require_policy(fixture, "cron", 1);
		require_canonical(fixture);
		free_result(&result);
	}
}

static void
read_exact(int descriptor, void *buffer, size_t length)
{
	unsigned char *cursor = buffer;
	size_t offset = 0;

	while (offset < length) {
		ssize_t result =
		    read(descriptor, cursor + offset, length - offset);

		require(result > 0, "read writer barrier");
		offset += (size_t)result;
	}
}

static void
write_exact(int descriptor, const void *buffer, size_t length)
{
	const unsigned char *cursor = buffer;
	size_t offset = 0;

	while (offset < length) {
		ssize_t result =
		    write(descriptor, cursor + offset, length - offset);

		require(result > 0, "write writer barrier");
		offset += (size_t)result;
	}
}

static void
writer_child(struct fixture *fixture, const char *name, int ready, int go)
{
	char *argv[] = {"enable", (char *)name, NULL};
	struct run_result result;

	backend_reset(&fixture->backend);
	fixture->backend.barrier_on_show = 1;
	fixture->backend.barrier_ready = ready;
	fixture->backend.barrier_go = go;
	result = run_command(fixture, 2, argv);
	if (result.status != 0 || fixture->backend.call_count != 2)
		_exit(92);
	free_result(&result);
	_exit(0);
}

static void
test_two_writers(struct fixture *fixture)
{
	int ready[2], go[2], first_status, second_status;
	char bytes[2], release[2] = {'G', 'G'};
	pid_t first, second;

	write_initial_rcconf(fixture, 0, 0);
	require(pipe(ready) == 0 && pipe(go) == 0,
		"create two-writer barriers");
	first = fork();
	require(first >= 0, "fork first service writer");
	if (first == 0) {
		close(ready[0]);
		close(go[1]);
		writer_child(fixture, "cron", ready[1], go[0]);
	}
	second = fork();
	require(second >= 0, "fork second service writer");
	if (second == 0) {
		close(ready[0]);
		close(go[1]);
		writer_child(fixture, "ntpdate", ready[1], go[0]);
	}
	close(ready[1]);
	close(go[0]);
	read_exact(ready[0], bytes, sizeof(bytes));
	require(bytes[0] == 'R' && bytes[1] == 'R',
		"writers did not reach pre-update barrier");
	write_exact(go[1], release, sizeof(release));
	close(ready[0]);
	close(go[1]);
	require(waitpid(first, &first_status, 0) == first &&
		    WIFEXITED(first_status) && WEXITSTATUS(first_status) == 0 &&
		    waitpid(second, &second_status, 0) == second &&
		    WIFEXITED(second_status) && WEXITSTATUS(second_status) == 0,
		"concurrent service writer failed");
	require_policy(fixture, "cron", 1);
	require_policy(fixture, "ntpdate", 1);
	require_canonical(fixture);
}

static void
fixture_destroy(struct fixture *fixture)
{
	static const char *const definitions[] = {
	    "cron",
	    "ntpdate",
	    "networkd",
	    "badmeta",
	};
	char path[512], lock_path[512];
	size_t index;

	for (index = 0; index < ARRAY_LENGTH(definitions); index++) {
		require(snprintf(path, sizeof(path), "%s/%s",
				 fixture->service_directory,
				 definitions[index]) < (int)sizeof(path) &&
			    unlink(path) == 0,
			"remove definition fixture");
	}
	require(snprintf(lock_path, sizeof(lock_path), "%s.lock",
			 fixture->rcconf) < (int)sizeof(lock_path),
		"format lock cleanup path");
	require(unlink(fixture->rcconf) == 0 && unlink(lock_path) == 0 &&
		    rmdir(fixture->service_directory) == 0 &&
		    rmdir(fixture->directory) == 0,
		"remove service-command fixtures");
}

int
main(void)
{
	struct fixture fixture;

	fixture_init(&fixture);
	test_grammar(&fixture);
	test_nonroot(&fixture);
	test_list_and_show_alias(&fixture);
	test_detail(&fixture);
	test_runtime(&fixture);
	test_runtime_failures(&fixture);
	test_policy_success(&fixture);
	test_missing_and_invalid_definition(&fixture);
	test_policy_reload_failures(&fixture);
	test_two_writers(&fixture);
	fixture_destroy(&fixture);
	puts("WS012 service command: PASS");
	return 0;
}
