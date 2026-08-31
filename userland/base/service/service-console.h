/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the zedBSD userland service console interface.
 */

#ifndef ZEDBSD_SERVICE_CONSOLE_H
#define ZEDBSD_SERVICE_CONSOLE_H

#include "userland/base/service/service-command.h"

#include <stdio.h>

#define SERVICE_CONSOLE_LINE_CAPACITY 512U
#define SERVICE_CONSOLE_ARGUMENT_MAX 16U

int service_console_print_help(FILE *);
int service_console_run(struct service_command_context *, FILE *);

#endif
