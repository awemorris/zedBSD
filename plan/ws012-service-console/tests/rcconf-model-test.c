/*
 * WS012 SVC-T001 strict rc.conf model and assignment-parser separation
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#define _POSIX_C_SOURCE 200809L

#include "userland/base/service/rcconf.h"
#include "userland/base/service/service-config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char configuration_path[] = "/tmp/zedbsd-rcconf-model-XXXXXX";

static void
require(int condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "SVC-T001: %s (errno=%d)\n", message, errno);
		exit(1);
	}
}

static void
write_bytes(const void *data, size_t length)
{
	FILE *stream = fopen(configuration_path, "wb");

	require(stream != NULL, "open input fixture");
	require(fwrite(data, 1, length, stream) == length,
		"write input fixture");
	require(fclose(stream) == 0, "close input fixture");
}

static void
write_text(const char *text)
{
	write_bytes(text, strlen(text));
}

static int
load_text(const char *text, struct rcconf_model *model)
{
	write_text(text);
	return rcconf_load(configuration_path, model);
}

static void
require_invalid(const char *name, const char *text)
{
	struct rcconf_model before, result;

	memset(&before, 0xa5, sizeof(before));
	result = before;
	errno = 0;
	write_text(text);
	if (rcconf_load(configuration_path, &result) == 0) {
		fprintf(stderr, "SVC-T001: %s unexpectedly accepted\n", name);
		exit(1);
	}
	require(memcmp(&before, &result, sizeof(result)) == 0,
		"failed parse exposed a partial model");
}

static char *
serialize(const struct rcconf_model *model)
{
	FILE *stream = tmpfile();
	char *result;
	long length;

	require(stream != NULL, "create canonical stream");
	require(rcconf_write(stream, model) == 0 && fflush(stream) == 0,
		"write canonical model");
	require(fseek(stream, 0, SEEK_END) == 0, "seek canonical stream end");
	length = ftell(stream);
	require(length >= 0 && fseek(stream, 0, SEEK_SET) == 0,
		"measure canonical stream");
	result = malloc((size_t)length + 1U);
	require(result != NULL, "allocate canonical text");
	require(fread(result, 1, (size_t)length, stream) == (size_t)length,
		"read canonical text");
	result[length] = '\0';
	require(fclose(stream) == 0, "close canonical stream");
	return result;
}

static void
test_accepted_and_canonical(void)
{
	static const char input[] =
	    "# Mapping order is intentionally non-canonical.\n"
	    "services:\n"
	    "  zeta-2:\n"
	    "    enabled: true\n"
	    "  ntpdate:\n"
	    "    settings:\n"
	    "      servers: 'pool \"one\"'\n"
	    "    enabled: false\n"
	    "  alpha_1:\n"
	    "    enabled: true\n"
	    "\n"
	    "hostname: zedbsd.local/domain:one\n"
	    "version: 1";
	static const char expected[] = "version: 1\n"
				       "hostname: zedbsd.local/domain:one\n"
				       "\n"
				       "services:\n"
				       "  alpha_1:\n"
				       "    enabled: true\n"
				       "  ntpdate:\n"
				       "    enabled: false\n"
				       "    settings:\n"
				       "      servers: 'pool \"one\"'\n"
				       "  zeta-2:\n"
				       "    enabled: true\n";
	struct rcconf_model first, second;
	char setting[64];
	char *canonical, *again;
	int enabled = -1;

	require(load_text(input, &first) == 0, "accept strict mapping input");
	require(first.version == 1 &&
		    strcmp(first.hostname, "zedbsd.local/domain:one") == 0 &&
		    first.service_count == 3,
		"accepted model contents");
	require(rcconf_service_enabled(&first, "alpha_1", &enabled) == 0 &&
		    enabled == 1,
		"enabled lookup");
	require(rcconf_setting_get(&first, "ntpdate", "servers", setting,
				   sizeof(setting)) == 0 &&
		    strcmp(setting, "pool \"one\"") == 0,
		"setting lookup");
	canonical = serialize(&first);
	require(strcmp(canonical, expected) == 0,
		"canonical ordering or quoting differs");
	require(load_text(canonical, &second) == 0,
		"canonical output reparses");
	again = serialize(&second);
	require(strcmp(canonical, again) == 0,
		"canonical parse/write is not byte-stable");
	free(again);
	free(canonical);
}

static void
test_rejected_grammar(void)
{
	static const struct {
		const char *name;
		const char *text;
	} cases[] = {
	    {"tab", "version: 1\nhostname: zedbsd\nservices:\n\tcron:\n"},
	    {"odd indentation",
	     "version: 1\nhostname: zedbsd\nservices:\n cron:\n"},
	    {"excess nesting",
	     "version: 1\nhostname: zedbsd\nservices:\n  ntpdate:\n    "
	     "enabled: true\n    settings:\n        servers: pool\n"},
	    {"sequence", "version: 1\nhostname: zedbsd\nservices:\n  - cron\n"},
	    {"flow mapping", "version: 1\nhostname: zedbsd\nservices: {}\n"},
	    {"flow sequence", "version: 1\nhostname: zedbsd\nservices: []\n"},
	    {"anchor", "version: 1\nhostname: &host zedbsd\nservices:\n"},
	    {"alias", "version: 1\nhostname: *host\nservices:\n"},
	    {"tag", "version: 1\nhostname: !str zedbsd\nservices:\n"},
	    {"document marker",
	     "---\nversion: 1\nhostname: zedbsd\nservices:\n"},
	    {"literal scalar", "version: 1\nhostname: |\nservices:\n"},
	    {"folded scalar", "version: 1\nhostname: >\nservices:\n"},
	    {"inline comment",
	     "version: 1 # no\nhostname: zedbsd\nservices:\n"},
	    {"duplicate version",
	     "version: 1\nversion: 1\nhostname: zedbsd\nservices:\n"},
	    {"duplicate hostname",
	     "version: 1\nhostname: one\nhostname: two\nservices:\n"},
	    {"duplicate services",
	     "version: 1\nhostname: zedbsd\nservices:\nservices:\n"},
	    {"unknown top level",
	     "version: 1\nhostname: zedbsd\nmystery: value\nservices:\n"},
	    {"bad version", "version: 2\nhostname: zedbsd\nservices:\n"},
	    {"quoted version", "version: \"1\"\nhostname: zedbsd\nservices:\n"},
	    {"boolean hostname", "version: 1\nhostname: true\nservices:\n"},
	    {"missing version", "hostname: zedbsd\nservices:\n"},
	    {"missing hostname", "version: 1\nservices:\n"},
	    {"missing services", "version: 1\nhostname: zedbsd\n"},
	    {"duplicate service",
	     "version: 1\nhostname: zedbsd\nservices:\n  cron:\n    enabled: "
	     "true\n  cron:\n    enabled: false\n"},
	    {"missing enabled",
	     "version: 1\nhostname: zedbsd\nservices:\n  cron:\n"},
	    {"nonboolean enabled", "version: 1\nhostname: zedbsd\nservices:\n  "
				   "cron:\n    enabled: yes\n"},
	    {"quoted enabled", "version: 1\nhostname: zedbsd\nservices:\n  "
			       "cron:\n    enabled: \"true\"\n"},
	    {"duplicate enabled",
	     "version: 1\nhostname: zedbsd\nservices:\n  cron:\n    enabled: "
	     "true\n    enabled: false\n"},
	    {"unknown service key",
	     "version: 1\nhostname: zedbsd\nservices:\n  cron:\n    enabled: "
	     "true\n    restart: true\n"},
	    {"unknown setting",
	     "version: 1\nhostname: zedbsd\nservices:\n  ntpdate:\n    "
	     "enabled: true\n    settings:\n      mystery: value\n"},
	    {"setting on wrong service",
	     "version: 1\nhostname: zedbsd\nservices:\n  cron:\n    enabled: "
	     "true\n    settings:\n      servers: pool\n"},
	    {"duplicate setting",
	     "version: 1\nhostname: zedbsd\nservices:\n  ntpdate:\n    "
	     "enabled: true\n    settings:\n      servers: one\n      servers: "
	     "two\n"},
	    {"unterminated quote",
	     "version: 1\nhostname: \"zedbsd\nservices:\n"},
	    {"embedded delimiter quote",
	     "version: 1\nhostname: 'zed'bsd'\nservices:\n"},
	};
	size_t index;

	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++)
		require_invalid(cases[index].name, cases[index].text);
}

static void
test_bounds_and_hidden_data(void)
{
	struct rcconf_model model;
	char *text, *cursor;
	size_t index, capacity;
	char maximum_name[RCCONF_SERVICE_NAME_CAPACITY];
	char too_long_name[RCCONF_SERVICE_NAME_CAPACITY + 1U];
	char maximum_value[RCCONF_SETTING_VALUE_CAPACITY];
	char too_long_value[RCCONF_SETTING_VALUE_CAPACITY + 1U];
	static const char hidden_nul[] =
	    "version: 1\0ignored: true\nhostname: zedbsd\nservices:\n";

	memset(maximum_name, 'a', sizeof(maximum_name) - 1U);
	maximum_name[sizeof(maximum_name) - 1U] = '\0';
	memset(too_long_name, 'b', sizeof(too_long_name) - 1U);
	too_long_name[sizeof(too_long_name) - 1U] = '\0';
	memset(maximum_value, 'c', sizeof(maximum_value) - 1U);
	maximum_value[sizeof(maximum_value) - 1U] = '\0';
	memset(too_long_value, 'd', sizeof(too_long_value) - 1U);
	too_long_value[sizeof(too_long_value) - 1U] = '\0';

	capacity = 128U + RCCONF_SERVICE_MAX * 100U;
	text = malloc(capacity);
	require(text != NULL, "allocate service-count fixture");
	cursor = text;
	cursor += sprintf(cursor, "version: 1\nhostname: zedbsd\nservices:\n");
	for (index = 0; index < RCCONF_SERVICE_MAX; index++)
		cursor +=
		    sprintf(cursor, "  svc%02zu:\n    enabled: true\n", index);
	require(load_text(text, &model) == 0 &&
		    model.service_count == RCCONF_SERVICE_MAX,
		"maximum service count rejected");
	cursor += sprintf(cursor, "  overflow:\n    enabled: true\n");
	require_invalid("service count overflow", text);
	free(text);

	capacity = RCCONF_SETTING_VALUE_CAPACITY + 256U;
	text = malloc(capacity);
	require(text != NULL, "allocate scalar-bound fixture");
	require(snprintf(text, capacity,
			 "version: 1\nhostname: zedbsd\nservices:\n  %s:\n"
			 "    enabled: true\n  ntpdate:\n    enabled: false\n"
			 "    settings:\n      servers: %s\n",
			 maximum_name, maximum_value) > 0,
		"format maximum scalar fixture");
	require(load_text(text, &model) == 0,
		"maximum service name/value rejected");
	require(snprintf(text, capacity,
			 "version: 1\nhostname: zedbsd\nservices:\n  %s:\n"
			 "    enabled: true\n",
			 too_long_name) > 0,
		"format long service fixture");
	require_invalid("service name overflow", text);
	require(
	    snprintf(text, capacity,
		     "version: 1\nhostname: zedbsd\nservices:\n  ntpdate:\n"
		     "    enabled: false\n    settings:\n      servers: %s\n",
		     too_long_value) > 0,
	    "format long setting fixture");
	require_invalid("setting value overflow", text);
	free(text);

	memset(&model, 0, sizeof(model));
	model.version = 1;
	strcpy(model.hostname, "zedbsd");
	model.service_count = RCCONF_SERVICE_MAX + 1U;
	require(rcconf_model_validate(&model) != 0,
		"model service-count overflow accepted");
	model.service_count = 1;
	strcpy(model.services[0].name, "cron");
	model.services[0].enabled = 2;
	require(rcconf_model_validate(&model) != 0,
		"nonboolean model state accepted");

	write_bytes(hidden_nul, sizeof(hidden_nul) - 1U);
	memset(&model, 0x5a, sizeof(model));
	require(rcconf_load(configuration_path, &model) != 0,
		"embedded NUL hid trailing input");
}

static void
test_model_api_and_assignment_separation(void)
{
	struct rcconf_model model;
	char value[64];
	int enabled;

	rcconf_model_init(&model);
	strcpy(model.hostname, "zedbsd");
	require(rcconf_model_set_enabled(&model, "cron", 1) == 0 &&
		    rcconf_model_set_enabled(&model, "ntpdate", 0) == 0 &&
		    rcconf_model_set_setting(&model, "ntpdate", "servers",
					     "one two") == 0 &&
		    rcconf_model_validate(&model) == 0,
		"model mutation API");
	require(rcconf_service_enabled(&model, "cron", &enabled) == 0 &&
		    enabled == 1,
		"model enabled readback");
	require(rcconf_setting_get(&model, "ntpdate", "servers", value,
				   sizeof(value)) == 0 &&
		    strcmp(value, "one two") == 0,
		"model setting readback");
	errno = 0;
	require(rcconf_model_set_setting(&model, "cron", "servers", "bad") !=
			0 &&
		    errno == EINVAL,
		"unknown service setting accepted");
	errno = 0;
	require(rcconf_service_enabled(&model, "missing", &enabled) != 0 &&
		    errno == ENOENT,
		"missing service lookup result");

	write_text("command=/sbin/example\narguments=\"one two\"\n");
	require(assignment_get(configuration_path, "command", value,
			       sizeof(value)) == 0 &&
		    strcmp(value, "/sbin/example") == 0,
		"service-definition assignment parser changed");
	require(rcconf_load(configuration_path, &model) != 0,
		"YAML parser accepted service-definition assignments");
	write_text("version: 1\nhostname: zedbsd\nservices:\n");
	require(assignment_get(configuration_path, "hostname", value,
			       sizeof(value)) != 0,
		"assignment parser accepted YAML rc.conf");
}

int
main(void)
{
	int descriptor = mkstemp(configuration_path);

	require(descriptor >= 0 && close(descriptor) == 0,
		"create model fixture path");
	test_accepted_and_canonical();
	test_rejected_grammar();
	test_bounds_and_hidden_data();
	test_model_api_and_assignment_separation();
	require(unlink(configuration_path) == 0, "remove model fixture");
	puts("WS012 rc.conf strict model: PASS");
	return 0;
}
