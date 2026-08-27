/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/service/service-command.h"

int
main(int argc, char **argv)
{
	struct service_command_context context;

	service_command_context_init(&context);
	return service_command_dispatch(&context, argc - 1, argv + 1);
}
