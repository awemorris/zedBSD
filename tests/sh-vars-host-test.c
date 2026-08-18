/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#define _POSIX_C_SOURCE 200809L
#include "userland/sh/vars.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main(void)
{
	struct sh_var_snapshot snapshot;
	(void)unsetenv("SH_VAR_LOCAL");
	assert(sh_var_set("SH_VAR_LOCAL", "one", -1) == 0);
	assert(strcmp(sh_var_get("SH_VAR_LOCAL"), "one") == 0);
	assert(getenv("SH_VAR_LOCAL") == NULL);
	assert(sh_var_export("SH_VAR_LOCAL") == 0);
	assert(strcmp(getenv("SH_VAR_LOCAL"), "one") == 0);
	assert(sh_var_set("SH_VAR_LOCAL", "two", -1) == 0);
	assert(strcmp(getenv("SH_VAR_LOCAL"), "two") == 0);
	assert(sh_var_readonly("SH_VAR_LOCAL") == 0);
	errno = 0;
	assert(sh_var_set("SH_VAR_LOCAL", "three", -1) == -1);
	assert(errno == EPERM);
	assert(sh_var_unset("SH_VAR_LOCAL") == -1);
	assert(errno == EPERM);
	(void)unsetenv("SH_VAR_TEMP");
	assert(sh_var_set("SH_VAR_TEMP", "before", 0) == 0);
	assert(sh_var_snapshot("SH_VAR_TEMP", &snapshot) == 0);
	assert(sh_var_set("SH_VAR_TEMP", "during", 1) == 0);
	assert(strcmp(sh_var_get("SH_VAR_TEMP"), "during") == 0);
	assert(sh_var_restore(&snapshot) == 0);
	assert(strcmp(sh_var_get("SH_VAR_TEMP"), "before") == 0);
	assert(getenv("SH_VAR_TEMP") == NULL);
	assert(!sh_var_name("9INVALID"));
	assert(sh_var_name("valid_9"));
	puts("zedBSD shell variable host test: PASS");
	return 0;
}
