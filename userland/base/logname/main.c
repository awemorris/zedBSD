/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD logname userland command.
 */

#include "userland/base/common/command.h"
#include <stdio.h>
#include <unistd.h>

/*
 * Runs the logname command.
 */
int
main(
	void)
{
	char b[128];

	/* Handles the getlogin r condition. */
	if (getlogin_r(b, sizeof(b))) {
		command_error("logname", NULL);

		/* Reports operation failure. */
		return 1;
	}
	puts(b);

	/* Reports successful completion. */
	return 0;
}
