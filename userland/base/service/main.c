/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/service/service-command.h"
#include "userland/base/service/service-console.h"

#include <stdio.h>

int
main(int argc, char **argv)
{
	struct service_command_context context;

	service_command_context_init(&context);
	if (argc == 1)
		return service_console_run(&context, stdin);
	return service_command_dispatch(&context, argc - 1, argv + 1);
}
