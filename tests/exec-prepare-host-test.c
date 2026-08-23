/* Exec preparation contract tests. SPDX-License-Identifier: Zlib */
#include "kern/cred.h"
#include "kern/exec.h"
#include "kern/mount.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <zedbsd/process.h>

void *kern_malloc(size_t size) { return malloc(size); }
void *kern_calloc(size_t count, size_t size) { return calloc(count, size); }
void kern_free(void *pointer) { free(pointer); }

static void
test_shebang_parser(void)
{
	struct exec_shebang parsed;
	static const char elf[] = "\177ELF";
	static const char plain[] = "#!/bin/sh\n";
	static const char optional[] = "#! \t/usr/bin/env  -S noct -x \r\n";
	static const char eof_line[] = "#!/bin/noct --safe";
	static const char relative[] = "#!bin/sh\n";
	static const char embedded_nul[] = { '#', '!', '/', 'b', 'i', 'n', 0,
	    'x', '\n' };
	unsigned char too_long[EXEC_SHEBANG_LINE_MAX + 1U];

	assert(exec_shebang_parse(elf, sizeof(elf) - 1U, 1, &parsed) == 0);
	assert(exec_shebang_parse(plain, sizeof(plain) - 1U, 1, &parsed) == 1);
	assert(strcmp(parsed.interpreter, "/bin/sh") == 0);
	assert(!parsed.has_optional_argument);
	assert(exec_shebang_parse(optional, sizeof(optional) - 1U, 1,
	    &parsed) == 1);
	assert(strcmp(parsed.interpreter, "/usr/bin/env") == 0);
	assert(parsed.has_optional_argument);
	assert(strcmp(parsed.optional_argument, "-S noct -x") == 0);
	assert(exec_shebang_parse(eof_line, sizeof(eof_line) - 1U, 1,
	    &parsed) == 1);
	assert(strcmp(parsed.optional_argument, "--safe") == 0);
	assert(exec_shebang_parse(relative, sizeof(relative) - 1U, 1,
	    &parsed) == -ENOEXEC);
	assert(exec_shebang_parse(embedded_nul, sizeof(embedded_nul), 1,
	    &parsed) == -ENOEXEC);
	memset(too_long, 'x', sizeof(too_long));
	too_long[0] = '#';
	too_long[1] = '!';
	assert(exec_shebang_parse(too_long, sizeof(too_long), 0, &parsed) ==
	    -E2BIG);
}

static void
test_script_argv(void)
{
	struct exec_shebang shebang;
	char original_zero[] = "caller-zero";
	char original_one[] = "one";
	char original_two[] = "two";
	char *original[] = { original_zero, original_one, original_two, NULL };
	char **rewritten = NULL;
	char **full;
	char *huge;
	char *oversized[3];
	size_t i;

	memset(&shebang, 0, sizeof(shebang));
	strcpy(shebang.interpreter, "/usr/bin/noct");
	strcpy(shebang.optional_argument, "--safe mode");
	shebang.has_optional_argument = 1;
	assert(exec_script_argv_build(original, &shebang, "/tmp/program.nap",
	    &rewritten) == 0);
	assert(strcmp(rewritten[0], "/usr/bin/noct") == 0);
	assert(strcmp(rewritten[1], "--safe mode") == 0);
	assert(strcmp(rewritten[2], "/tmp/program.nap") == 0);
	assert(strcmp(rewritten[3], "one") == 0);
	assert(strcmp(rewritten[4], "two") == 0);
	assert(rewritten[5] == NULL);
	/* The rewritten vector owns every string needed by later recursion. */
	strcpy(original_one, "ONE");
	strcpy(shebang.interpreter, "/x");
	assert(strcmp(rewritten[0], "/usr/bin/noct") == 0);
	assert(strcmp(rewritten[3], "one") == 0);
	exec_script_argv_free(rewritten);

	huge = malloc(ZEDBSD_ARG_MAX + 1U);
	assert(huge != NULL);
	memset(huge, 'a', ZEDBSD_ARG_MAX);
	huge[ZEDBSD_ARG_MAX] = '\0';
	oversized[0] = original_zero;
	oversized[1] = huge;
	oversized[2] = NULL;
	memset(&shebang, 0, sizeof(shebang));
	strcpy(shebang.interpreter, "/bin/sh");
	assert(exec_script_argv_build(oversized, &shebang, "/tmp/x",
	    &rewritten) == E2BIG);
	free(huge);
	full = malloc(ZEDBSD_EXEC_VECTOR_MAX * sizeof(*full));
	assert(full != NULL);
	for (i = 0; i < ZEDBSD_EXEC_VECTOR_MAX; i++)
		full[i] = original_zero;
	assert(exec_script_argv_build(full, &shebang, "/tmp/x", &rewritten) ==
	    E2BIG);
	free(full);
}

static void
test_exec_credentials(void)
{
	struct ucred credential;
	struct stat status;
	unsigned secure;

	memset(&credential, 0, sizeof(credential));
	memset(&status, 0, sizeof(status));
	credential.ruid = credential.euid = credential.suid = 100;
	credential.rgid = credential.egid = credential.sgid = 200;
	status.st_mode = S_ISUID | S_ISGID | 0755U;
	status.st_uid = 10;
	status.st_gid = 20;
	exec_credential_prepare(&credential, &status, 0, 0, &secure);
	assert(credential.ruid == 100 && credential.euid == 10 &&
	    credential.suid == 10);
	assert(credential.rgid == 200 && credential.egid == 20 &&
	    credential.sgid == 20);
	assert(secure);

	credential.euid = credential.suid = 100;
	credential.egid = credential.sgid = 200;
	exec_credential_prepare(&credential, &status, MOUNT_NOSUID, 0, &secure);
	assert(credential.euid == 100 && credential.suid == 100);
	assert(credential.egid == 200 && credential.sgid == 200);
	assert(!secure);

	credential.euid = credential.suid = 100;
	credential.egid = credential.sgid = 200;
	exec_credential_prepare(&credential, &status, 0, 1, &secure);
	assert(credential.euid == 100 && credential.suid == 100);
	assert(credential.egid == 200 && credential.sgid == 200);
	assert(!secure);

	/* Plain exec resets saved IDs even when no set-id bit is present. */
	status.st_mode = 0755U;
	credential.euid = 101;
	credential.suid = 99;
	exec_credential_prepare(&credential, &status, 0, 0, &secure);
	assert(credential.euid == 101 && credential.suid == 101 && secure);
}

int
main(void)
{
	test_shebang_parser();
	test_script_argv();
	test_exec_credentials();
	return 0;
}
