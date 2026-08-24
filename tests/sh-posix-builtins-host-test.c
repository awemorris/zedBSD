/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/sh/builtins.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>

static void
hash_test(void)
{
	assert(sh_hash_sync_path("/one:/two") == 0);
	assert(sh_hash_store("sample", "/one/sample") == 0);
	assert(sh_hash_lookup("sample") != NULL);
	assert(sh_hash_sync_path("/one:/two") == 0);
	assert(sh_hash_lookup("sample") != NULL);
	assert(sh_hash_sync_path("/three") == 0);
	assert(sh_hash_lookup("sample") == NULL);
	assert(sh_hash_store("sample", "/three/sample") == 0);
	sh_hash_clear();
	assert(sh_hash_lookup("sample") == NULL);
}

static void
ulimit_test(void)
{
	struct rlimit before;
	struct rlimit after;
	char value[64];
	char *set_arguments[] = {"ulimit", "-S", "-n", value};
	char *invalid_arguments[] = {"ulimit", "-n", "invalid"};
	int handled;

	assert(getrlimit(RLIMIT_NOFILE, &before) == 0);
	assert(before.rlim_cur > 3);
	(void)snprintf(value, sizeof(value), "%llu",
		       (unsigned long long)(before.rlim_cur - 1U));
	assert(sh_builtin_dispatch(4, set_arguments, &handled) == 1);
	assert(handled == 1);
	assert(getrlimit(RLIMIT_NOFILE, &after) == 0);
	assert(after.rlim_cur == before.rlim_cur - 1U);
	assert(after.rlim_max == before.rlim_max);
	assert(setrlimit(RLIMIT_NOFILE, &before) == 0);
	assert(sh_builtin_dispatch(3, invalid_arguments, &handled) == 0);
	assert(handled == 1);
}

int
main(void)
{
	hash_test();
	ulimit_test();
	puts("zedBSD POSIX shell builtin host tests: PASS");
	return 0;
}
