/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#define _POSIX_C_SOURCE 200809L
#include "userland/base/sh/glob.h"
#include "userland/base/sh/lexer.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void
create_file(const char *path)
{
	int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	assert(descriptor >= 0);
	assert(close(descriptor) == 0);
}

static void
expect(const char *pattern, size_t count, const char *const expected[])
{
	struct sh_token_list tokens;
	struct sh_field_list fields;
	struct sh_expand_context context = { 0 };
	const char *error;
	size_t index;
	assert(sh_lex(pattern, &tokens, &error));
	assert(sh_expand_fields(&tokens.tokens[0], &context, &fields, &error));
	assert(sh_glob_fields(&fields, &error));
	assert(fields.count == count);
	for (index = 0; index < count; index++)
		assert(strcmp(fields.fields[index], expected[index]) == 0);
	sh_fields_free(&fields);
	sh_tokens_free(&tokens);
}

int
main(void)
{
	char directory[] = "/tmp/zedbsd-sh-glob.XXXXXX";
	static const char *const text[] = { "a.txt", "b.txt" };
	static const char *const nested[] = { "sub/file.c" };
	static const char *const literal[] = { "*.txt" };
	static const char *const unmatched[] = { "*.none" };
	assert(mkdtemp(directory) != NULL);
	assert(chdir(directory) == 0);
	create_file("b.txt");
	create_file("a.txt");
	create_file(".hidden.txt");
	assert(mkdir("sub", 0700) == 0);
	create_file("sub/file.c");
	expect("*.txt", 2, text);
	expect("sub/*.c", 1, nested);
	expect("\"*.txt\"", 1, literal);
	expect("*.none", 1, unmatched);
	assert(unlink("sub/file.c") == 0);
	assert(rmdir("sub") == 0);
	assert(unlink(".hidden.txt") == 0);
	assert(unlink("a.txt") == 0);
	assert(unlink("b.txt") == 0);
	assert(chdir("/") == 0);
	assert(rmdir(directory) == 0);
	puts("zedBSD shell glob host test: PASS");
	return 0;
}
