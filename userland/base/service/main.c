/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD service userland command.
 */

#include "userland/base/service/service-command.h"
#include "userland/base/service/service-console.h"

#include <stdio.h>

/*
 * Runs the service command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	struct service_command_context context;

	service_command_context_init(&context);

	/* Validates the command-line arguments. */
	if (argc == 1) {
		/* Obtains the service console run result. */
		function_result = service_console_run(&context, stdin);

		/* Returns the computed result. */
		return function_result;
	}

	/* Obtains the service command dispatch result. */
	function_result = service_command_dispatch(&context, argc - 1, argv + 1);

	/* Returns the computed result. */
	return function_result;
}
