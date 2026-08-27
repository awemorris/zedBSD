/*
 * WS012 SVC-T005 production service-console fixture
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#define _POSIX_C_SOURCE 200809L

#include "userland/base/service/rcconf.h"
#include "userland/base/service/service-console.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CALL_MAX 64

static const char banner[] = "zedBSD Service Console\n"
			     "Type '?' for help.\n";
static const char prompt[] = "service> ";
static const char help_text[] =
    "Commands:\n"
    "  show [NAME]    show all services or one service\n"
    "  list           show all services\n"
    "  status NAME    show one service\n"
    "  start NAME     start a service for this boot\n"
    "  stop NAME      stop a service for this boot\n"
    "  restart NAME   restart a service for this boot\n"
    "  enable NAME    enable a service persistently\n"
    "  disable NAME   disable a service persistently\n"
    "  reload         reload persistent policy\n"
    "  help, ?        show this help\n"
    "  exit, quit     leave the console\n";
static const char usage_text[] = "usage: service list\n"
				 "       service show [name]\n"
				 "       service status name\n"
				 "       service start name\n"
				 "       service stop name\n"
				 "       service restart name\n"
				 "       service enable name\n"
				 "       service disable name\n"
				 "       service reload\n";
static const char list_text[] = "NAME        STATUS    ENABLED   PID\n"
				"networkd    running   yes       184\n";
static const char detail_text[] = "NAME        STATUS    ENABLED   PID\n"
				  "networkd    running   yes       184\n"
				  "\n"
				  "TYPE        daemon\n"
				  "COMMAND     /sbin/networkd --foreground\n"
				  "RESTART     on-failure\n"
				  "AFTER       syslogd\n"
				  "REQUIRES    -\n";

struct fake_zsv1 {
	struct zsv1_request calls[CALL_MAX];
	size_t call_count;
	int fail_command;
	int fail_count;
	int fail_error;
	int alternating_lists;
	unsigned list_count;
};

struct fixture {
	char directory[256];
	char rcconf[320];
	char service_directory[320];
	struct service_command_context context;
	struct fake_zsv1 backend;
};

struct run_result {
	int status;
	char *output;
	char *error;
};

static void
fail(const char *message)
{
	fprintf(stderr, "SVC-T005: %s (errno=%d)\n", message, errno);
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

static void
write_definition(struct fixture *fixture, const char *name, const char *text)
{
	char path[512];
	FILE *stream;

	require(snprintf(path, sizeof(path), "%s/%s",
			 fixture->service_directory, name) < (int)sizeof(path),
		"format definition path");
	stream = fopen(path, "w");
	require(stream != NULL && fputs(text, stream) >= 0 &&
		    fflush(stream) == 0 && fclose(stream) == 0,
		"write service definition");
}

static void
write_initial_rcconf(struct fixture *fixture)
{
	struct rcconf_model model;
	FILE *stream;

	rcconf_model_init(&model);
	strcpy(model.hostname, "zedbsd");
	require(rcconf_model_set_enabled(&model, "cron", 0) == 0 &&
		    rcconf_model_set_enabled(&model, "networkd", 1) == 0 &&
		    rcconf_model_set_enabled(&model, "ntpdate", 0) == 0 &&
		    rcconf_model_set_setting(&model, "ntpdate", "servers",
					     "0.pool.ntp.org") == 0,
		"construct rc.conf");
	stream = fopen(fixture->rcconf, "w");
	require(stream != NULL && rcconf_write(stream, &model) == 0 &&
		    fflush(stream) == 0 && fclose(stream) == 0,
		"write rc.conf");
}

static void
backend_reset(struct fake_zsv1 *backend)
{
	memset(backend, 0, sizeof(*backend));
	backend->fail_command = -1;
}

static void
set_service(struct zsv1_response *response, const char *name,
	    enum zsv1_service_state state, int enabled, pid_t pid)
{
	response->service_count = 1;
	strcpy(response->services[0].name, name);
	response->services[0].state = state;
	response->services[0].enabled = enabled;
	response->services[0].pid = pid;
}

static void
set_ok(struct zsv1_response *response, const char *token)
{
	response->ok_present = 1;
	strcpy(response->ok_token, token);
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
	if ((int)request->command == backend->fail_command &&
	    backend->fail_count > 0) {
		backend->fail_count--;
		errno = backend->fail_error;
		return -1;
	}
	memset(response, 0, sizeof(*response));
	response->ended = 1;
	if (request->command == ZSV1_COMMAND_LIST) {
		if (backend->alternating_lists) {
			const char *name =
			    backend->list_count++ == 0 ? "alpha" : "beta";

			set_service(response, name, ZSV1_STATE_STOPPED, 0, 0);
		} else {
			set_service(response, "networkd", ZSV1_STATE_RUNNING, 1,
				    184);
		}
		return 0;
	}
	if (request->command == ZSV1_COMMAND_SHOW) {
		if (strcmp(request->service, "networkd") == 0) {
			set_service(response, "networkd", ZSV1_STATE_RUNNING, 1,
				    184);
			response->dependency_count = 1;
			response->dependencies[0].type = ZSV1_DEPENDENCY_AFTER;
			strcpy(response->dependencies[0].name, "syslogd");
		} else {
			set_service(response, request->service,
				    ZSV1_STATE_STOPPED, 0, 0);
		}
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
	set_ok(response, token);
	return 0;
}

static void
fixture_init(struct fixture *fixture)
{
	char template[] = "/tmp/zedbsd-service-console-XXXXXX";

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
	write_definition(fixture, "networkd",
			 "type=daemon\ncommand=/sbin/networkd\n"
			 "arguments=--foreground\nrestart=on-failure\n");
	write_definition(fixture, "cron",
			 "type=daemon\ncommand=/sbin/cron\n"
			 "restart=on-failure\n");
	write_initial_rcconf(fixture);
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
run_console(struct fixture *fixture, const void *input_data,
	    size_t input_length)
{
	struct service_command_context context = fixture->context;
	struct run_result result;
	FILE *input = tmpfile(), *output = tmpfile(), *error = tmpfile();

	require(input != NULL && output != NULL && error != NULL,
		"create console streams");
	require(fwrite(input_data, 1, input_length, input) == input_length &&
		    fflush(input) == 0 && fseek(input, 0, SEEK_SET) == 0,
		"prepare console input");
	context.output = output;
	context.error = error;
	result.status = service_console_run(&context, input);
	result.output = read_stream(output);
	result.error = read_stream(error);
	require(fclose(input) == 0 && fclose(output) == 0 && fclose(error) == 0,
		"close console streams");
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
			"SVC-T005: %s\nstatus=%d expected=%d\n"
			"stdout=[%s]\nstderr=[%s]\n",
			message, result->status, status, result->output,
			result->error);
		exit(1);
	}
}

static void
format_expected(char *output, size_t capacity, const char *format,
		const char *a, const char *b, const char *c, const char *d)
{
	require(snprintf(output, capacity, format, a, b, c, d) < (int)capacity,
		"format expected console output");
}

static void
test_nonroot_and_eof(struct fixture *fixture)
{
	struct run_result result;
	char expected[256];

	fixture->context.effective_uid = 1000;
	backend_reset(&fixture->backend);
	result = run_console(fixture, "list\n", 5);
	expect_result(&result, 1, "", "service: effective UID 0 is required\n",
		      "non-root console");
	require(fixture->backend.call_count == 0,
		"non-root console reached backend");
	free_result(&result);
	fixture->context.effective_uid = 0;

	backend_reset(&fixture->backend);
	format_expected(expected, sizeof(expected), "%s%s", banner, prompt, "",
			"");
	result = run_console(fixture, "", 0);
	expect_result(&result, 0, expected, "", "empty EOF");
	require(fixture->backend.call_count == 0, "empty EOF reached backend");
	free_result(&result);
}

static void
test_blank_help_and_exit(struct fixture *fixture)
{
	struct run_result result;
	char expected[8192];
	static const char blanks[] = "\n \t  \nexit\n";
	static const char helps[] = "?\nhelp\nexit\n";

	backend_reset(&fixture->backend);
	require(snprintf(expected, sizeof(expected), "%s%s%s%s", banner, prompt,
			 prompt, prompt) < (int)sizeof(expected),
		"format blank output");
	result = run_console(fixture, blanks, sizeof(blanks) - 1U);
	expect_result(&result, 0, expected, "", "blank and space lines");
	free_result(&result);

	backend_reset(&fixture->backend);
	require(snprintf(expected, sizeof(expected), "%s%s%s%s%s%s%s", banner,
			 prompt, help_text, prompt, help_text, prompt,
			 "") < (int)sizeof(expected),
		"format help output");
	result = run_console(fixture, helps, sizeof(helps) - 1U);
	expect_result(&result, 0, expected, "", "help aliases");
	require(strstr(result.output, "save") == NULL &&
		    strstr(result.output, "commit") == NULL,
		"help advertises candidate commands");
	free_result(&result);

	backend_reset(&fixture->backend);
	require(snprintf(expected, sizeof(expected), "%s%s", banner, prompt) <
		    (int)sizeof(expected),
		"format exit output");
	result = run_console(fixture, "exit\n", 5);
	expect_result(&result, 0, expected, "", "exit command");
	free_result(&result);
	result = run_console(fixture, "quit\n", 5);
	expect_result(&result, 0, expected, "", "quit command");
	free_result(&result);
}

static void
test_local_command_recovery(struct fixture *fixture)
{
	static const char input[] =
	    "exit now\nquit now\nhelp now\n? now\nlist\nexit\n";
	static const char error[] = "service: exit takes no arguments\n"
				    "service: quit takes no arguments\n"
				    "service: help takes no arguments\n"
				    "service: ? takes no arguments\n";
	struct run_result result;
	char expected[2048];

	backend_reset(&fixture->backend);
	require(snprintf(expected, sizeof(expected), "%s%s%s%s%s%s%s%s", banner,
			 prompt, prompt, prompt, prompt, prompt, list_text,
			 prompt) < (int)sizeof(expected),
		"format local recovery output");
	result = run_console(fixture, input, sizeof(input) - 1U);
	expect_result(&result, 0, expected, error, "local command recovery");
	require(fixture->backend.call_count == 1 &&
		    fixture->backend.calls[0].command == ZSV1_COMMAND_LIST,
		"local errors reached dispatcher backend");
	free_result(&result);
}

static void
test_dispatch_recovery(struct fixture *fixture)
{
	static const char input[] =
	    "bogus\nstart\nstart bad/name\n"
	    "a b c d e f g h i j k l m n o p q\nlist\nexit\n";
	struct run_result result;
	char expected_output[2048], expected_error[8192];

	backend_reset(&fixture->backend);
	require(snprintf(expected_output, sizeof(expected_output),
			 "%s%s%s%s%s%s%s%s", banner, prompt, prompt, prompt,
			 prompt, prompt, list_text,
			 prompt) < (int)sizeof(expected_output),
		"format dispatch recovery output");
	require(snprintf(expected_error, sizeof(expected_error),
			 "service: unknown command: bogus\n%s"
			 "service: invalid arguments for start\n%s"
			 "service: invalid service name: bad/name\n"
			 "service: too many command fields\n",
			 usage_text, usage_text) < (int)sizeof(expected_error),
		"format dispatch recovery error");
	result = run_console(fixture, input, sizeof(input) - 1U);
	expect_result(&result, 0, expected_output, expected_error,
		      "unsupported and malformed recovery");
	require(fixture->backend.call_count == 1 &&
		    fixture->backend.calls[0].command == ZSV1_COMMAND_LIST,
		"malformed commands reached backend");
	free_result(&result);
}

static void
test_line_boundaries(struct fixture *fixture)
{
	struct run_result result;
	char expected[2048];
	unsigned char control_input[] = {'b', 'a', 'd', 1,   'x',  '\n',
					 'l', 'i', 's', 't', '\n', 'e',
					 'x', 'i', 't', '\n'};
	char *long_input;
	size_t long_length = 700U + strlen("\nlist\nexit\n");

	require(snprintf(expected, sizeof(expected), "%s%s%s%s%s", banner,
			 prompt, prompt, list_text,
			 prompt) < (int)sizeof(expected),
		"format boundary recovery output");
	long_input = malloc(long_length);
	require(long_input != NULL, "allocate overlong input");
	memset(long_input, 'x', 700U);
	memcpy(long_input + 700U, "\nlist\nexit\n", strlen("\nlist\nexit\n"));
	backend_reset(&fixture->backend);
	result = run_console(fixture, long_input, long_length);
	expect_result(&result, 0, expected, "service: input line too long\n",
		      "overlong line recovery");
	require(fixture->backend.call_count == 1 &&
		    fixture->backend.calls[0].command == ZSV1_COMMAND_LIST,
		"overlong line was not fully consumed");
	free_result(&result);
	free(long_input);

	backend_reset(&fixture->backend);
	result = run_console(fixture, control_input, sizeof(control_input));
	expect_result(&result, 0, expected,
		      "service: invalid input character\n",
		      "control-character recovery");
	require(fixture->backend.call_count == 1,
		"invalid control line reached backend");
	free_result(&result);
}

static void
test_no_shell_quoting(struct fixture *fixture)
{
	static const char input[] =
	    "start 'cron'\nstart \"cron\"\nstart cron\nexit\n";
	static const char error[] = "service: invalid service name: 'cron'\n"
				    "service: invalid service name: \"cron\"\n";
	struct run_result result;
	char expected[2048];

	backend_reset(&fixture->backend);
	require(snprintf(expected, sizeof(expected), "%s%s%s%s%s%s", banner,
			 prompt, prompt, prompt, "OK started\n",
			 prompt) < (int)sizeof(expected),
		"format no-quoting output");
	result = run_console(fixture, input, sizeof(input) - 1U);
	expect_result(&result, 0, expected, error, "no shell quoting");
	require(fixture->backend.call_count == 1 &&
		    fixture->backend.calls[0].command == ZSV1_COMMAND_START &&
		    strcmp(fixture->backend.calls[0].service, "cron") == 0,
		"quoted names reached backend");
	free_result(&result);
}

static void
require_final_policy(struct fixture *fixture)
{
	struct rcconf_model model;
	char servers[RCCONF_SETTING_VALUE_CAPACITY];
	int enabled;

	require(rcconf_load(fixture->rcconf, &model) == 0 &&
		    strcmp(model.hostname, "zedbsd") == 0 &&
		    rcconf_service_enabled(&model, "cron", &enabled) == 0 &&
		    enabled == 0 &&
		    rcconf_service_enabled(&model, "networkd", &enabled) == 0 &&
		    enabled == 1 &&
		    rcconf_setting_get(&model, "ntpdate", "servers", servers,
				       sizeof(servers)) == 0 &&
		    strcmp(servers, "0.pool.ntp.org") == 0,
		"console policy persistence");
}

static void
test_all_public_commands(struct fixture *fixture)
{
	static const char input[] =
	    "list\nshow\nshow networkd\nstatus networkd\n"
	    "start cron\nstop cron\nrestart cron\n"
	    "enable cron\ndisable cron\nreload\nexit\n";
	struct run_result result;
	char expected[16384];
	static const enum zsv1_command commands[] = {
	    ZSV1_COMMAND_LIST,	  ZSV1_COMMAND_LIST,   ZSV1_COMMAND_SHOW,
	    ZSV1_COMMAND_SHOW,	  ZSV1_COMMAND_START,  ZSV1_COMMAND_STOP,
	    ZSV1_COMMAND_RESTART, ZSV1_COMMAND_SHOW,   ZSV1_COMMAND_RELOAD,
	    ZSV1_COMMAND_SHOW,	  ZSV1_COMMAND_RELOAD, ZSV1_COMMAND_RELOAD,
	};
	size_t index;

	write_initial_rcconf(fixture);
	backend_reset(&fixture->backend);
	require(snprintf(expected, sizeof(expected),
			 "%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s",
			 banner, prompt, list_text, prompt, list_text, prompt,
			 detail_text, prompt, detail_text, prompt,
			 "OK started\n", prompt, "OK stopped\n", prompt,
			 "OK restarted\n", prompt, "OK enabled cron\n", prompt,
			 "OK disabled cron\n", prompt, "OK reloaded\n", prompt,
			 "", "", "") < (int)sizeof(expected),
		"format all-command output");
	result = run_console(fixture, input, sizeof(input) - 1U);
	expect_result(&result, 0, expected, "", "all public commands");
	require(fixture->backend.call_count ==
		    sizeof(commands) / sizeof(commands[0]),
		"all-command backend count");
	for (index = 0; index < sizeof(commands) / sizeof(commands[0]); index++)
		require(fixture->backend.calls[index].command ==
			    commands[index],
			"all-command backend order");
	require_final_policy(fixture);
	free_result(&result);
}

static void
test_backend_failure_and_fresh_calls(struct fixture *fixture)
{
	static const char input[] = "start cron\nlist\nexit\n";
	static const char repeated[] = "list\nlist\nexit\n";
	struct run_result result;
	char expected[4096], error[512];

	backend_reset(&fixture->backend);
	fixture->backend.fail_command = ZSV1_COMMAND_START;
	fixture->backend.fail_count = 1;
	fixture->backend.fail_error = ECONNREFUSED;
	require(snprintf(expected, sizeof(expected), "%s%s%s%s%s", banner,
			 prompt, prompt, list_text,
			 prompt) < (int)sizeof(expected) &&
		    snprintf(error, sizeof(error),
			     "service: init request failed: %s\n",
			     strerror(ECONNREFUSED)) < (int)sizeof(error),
		"format backend failure output");
	result = run_console(fixture, input, sizeof(input) - 1U);
	expect_result(&result, 0, expected, error,
		      "backend failure session recovery");
	require(fixture->backend.call_count == 2 &&
		    fixture->backend.calls[0].command == ZSV1_COMMAND_START &&
		    fixture->backend.calls[1].command == ZSV1_COMMAND_LIST,
		"backend failure stopped session");
	free_result(&result);

	backend_reset(&fixture->backend);
	fixture->backend.alternating_lists = 1;
	require(snprintf(expected, sizeof(expected),
			 "%s%sNAME        STATUS    ENABLED   PID\n"
			 "alpha       stopped   no        -\n"
			 "%sNAME        STATUS    ENABLED   PID\n"
			 "beta        stopped   no        -\n%s",
			 banner, prompt, prompt,
			 prompt) < (int)sizeof(expected),
		"format fresh-call output");
	result = run_console(fixture, repeated, sizeof(repeated) - 1U);
	expect_result(&result, 0, expected, "", "fresh backend per command");
	require(fixture->backend.call_count == 2,
		"repeated commands reused a response");
	free_result(&result);
}

static void
fixture_destroy(struct fixture *fixture)
{
	static const char *const definitions[] = {"networkd", "cron"};
	char path[512], lock_path[512];
	size_t index;

	for (index = 0; index < sizeof(definitions) / sizeof(definitions[0]);
	     index++) {
		require(snprintf(path, sizeof(path), "%s/%s",
				 fixture->service_directory,
				 definitions[index]) < (int)sizeof(path) &&
			    unlink(path) == 0,
			"remove definition");
	}
	require(snprintf(lock_path, sizeof(lock_path), "%s.lock",
			 fixture->rcconf) < (int)sizeof(lock_path) &&
		    unlink(fixture->rcconf) == 0 && unlink(lock_path) == 0 &&
		    rmdir(fixture->service_directory) == 0 &&
		    rmdir(fixture->directory) == 0,
		"remove console fixture");
}

int
main(void)
{
	struct fixture fixture;

	/*
	 * SVC-T004 already drives this same dispatcher through a synchronized
	 * forked two-writer policy race.  The policy sequence below proves that
	 * the console reaches that dispatcher rather than a second writer path.
	 */
	fixture_init(&fixture);
	test_nonroot_and_eof(&fixture);
	test_blank_help_and_exit(&fixture);
	test_local_command_recovery(&fixture);
	test_dispatch_recovery(&fixture);
	test_line_boundaries(&fixture);
	test_no_shell_quoting(&fixture);
	test_all_public_commands(&fixture);
	test_backend_failure_and_fresh_calls(&fixture);
	fixture_destroy(&fixture);
	puts("WS012 service console: PASS");
	return 0;
}
