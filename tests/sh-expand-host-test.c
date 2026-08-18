/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#define _POSIX_C_SOURCE 200809L
#include "userland/sh/expand.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *
host_lookup(void *context, const char *name)
{
	(void)context;
	return getenv(name);
}

static int
host_assign(void *context, const char *name, const char *value)
{
	(void)context;
	return setenv(name, value, 1);
}

static int
host_command(void *context, const char *source, char **result)
{
	const char *value = "command output";
	(void)context;
	assert(strcmp(source, "host command") == 0);
	*result = malloc(strlen(value) + 1U);
	assert(*result != NULL);
	strcpy(*result, value);
	return 1;
}

static void
expect(const char *source, const struct sh_expand_context *context,
    const char *expected)
{
	struct sh_token_list list;
	const char *error;
	char *value;
	assert(sh_lex(source, &list, &error));
	assert(list.count == 2);
	assert(sh_expand_word(&list.tokens[0], context, &value, &error));
	assert(strcmp(value, expected) == 0);
	free(value);
	sh_tokens_free(&list);
}

static void
expect_fields(const char *source, const struct sh_expand_context *context,
    size_t count, const char *const expected[])
{
	struct sh_token_list tokens;
	struct sh_field_list fields;
	const char *error;
	size_t index;
	assert(sh_lex(source, &tokens, &error));
	assert(tokens.count == 2);
	assert(sh_expand_fields(&tokens.tokens[0], context, &fields, &error));
	assert(fields.count == count);
	for (index = 0; index < count; index++)
		assert(strcmp(fields.fields[index], expected[index]) == 0);
	sh_fields_free(&fields);
	sh_tokens_free(&tokens);
}

int
main(void)
{
	struct sh_expand_context context = {
		.status = 7, .shell_pid = 123, .last_job = 456,
		.lookup = host_lookup, .assign = host_assign,
		.command_substitute = host_command
	};
	struct sh_token_list list;
	const char *error;
	char *value;
	static const char *const split[] = { "value", "with", "spaces" };
	static const char *const joined[] = { "value with spaces" };
	static const char *const empty[] = { "" };
	static const char *const at_fields[] = { "first value", "second" };
	char *positionals[] = { "first value", "second" };
	context.shell_name = "test-script";
	context.positional_count = 2;
	context.positional = positionals;

	assert(setenv("SH_EXPAND_TEST", "value with spaces", 1) == 0);
	assert(setenv("HOME", "/home/tester", 1) == 0);
	assert(setenv("COUNT", "7", 1) == 0);
	expect("~/file", &context, "/home/tester/file");
	expect("'~'/file", &context, "~/file");
	expect("$(host command)", &context, "command output");
	expect("$((1 + 2 * 3))", &context, "7");
	expect("$((COUNT > 3 ? COUNT : 3))", &context, "7");
	expect("'$SH_EXPAND_TEST'", &context, "$SH_EXPAND_TEST");
	expect("\"$SH_EXPAND_TEST\"", &context, "value with spaces");
	expect("pre${SH_EXPAND_TEST}post", &context,
	    "prevalue with spacespost");
	expect("\\$SH_EXPAND_TEST", &context, "$SH_EXPAND_TEST");
	expect("$?", &context, "7");
	expect("$$", &context, "123");
	expect("$!", &context, "456");
	expect("$0", &context, "test-script");
	expect("$#", &context, "2");
	expect("$1", &context, "first value");
	expect("$9", &context, "");
	expect("\"$*\"", &context, "first value second");
	expect_fields("\"$@\"", &context, 2, at_fields);
	expect("${SH_EXPAND_UNDEFINED}", &context, "");
	expect_fields("$SH_EXPAND_TEST", &context, 3, split);
	expect_fields("\"$SH_EXPAND_TEST\"", &context, 1, joined);
	expect_fields("$SH_EXPAND_UNDEFINED", &context, 0, NULL);
	expect_fields("\"$SH_EXPAND_UNDEFINED\"", &context, 1, empty);
	expect_fields("\"\"", &context, 1, empty);
	(void)unsetenv("SH_EXPAND_MISSING");
	expect("${SH_EXPAND_MISSING:-fallback}", &context, "fallback");
	expect("${SH_EXPAND_TEST:+alternate}", &context, "alternate");
	expect("${SH_EXPAND_MISSING+alternate}", &context, "");
	expect("${SH_EXPAND_MISSING:-${SH_EXPAND_TEST}}", &context,
	    "value with spaces");
	expect("${SH_EXPAND_MISSING:=assigned}", &context, "assigned");
	assert(strcmp(getenv("SH_EXPAND_MISSING"), "assigned") == 0);

	(void)unsetenv("SH_EXPAND_MISSING");
	assert(sh_lex("${SH_EXPAND_MISSING:?required}", &list, &error));
	assert(!sh_expand_word(&list.tokens[0], &context, &value, &error));
	assert(strcmp(error, "parameter expansion requested an error") == 0);
	sh_tokens_free(&list);
	puts("zedBSD shell expansion host test: PASS");
	return 0;
}
