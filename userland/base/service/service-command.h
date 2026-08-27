/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_SERVICE_COMMAND_H
#define ZEDBSD_SERVICE_COMMAND_H

#include "userland/base/service/zsv1-client.h"

#include <stdio.h>
#include <sys/types.h>

#define SERVICE_DEFINITION_DIRECTORY "/etc/service.d"

typedef int (*service_zsv1_call_t)(void *, const char *,
				   const struct zsv1_request *,
				   struct zsv1_response *);

struct service_command_context {
	const char *rcconf_path;
	const char *service_directory;
	const char *init_socket;
	uid_t effective_uid;
	FILE *output;
	FILE *error;
	service_zsv1_call_t zsv1_call;
	void *zsv1_opaque;
};

void service_command_context_init(struct service_command_context *);
void service_command_print_usage(FILE *);
int service_command_dispatch(struct service_command_context *, int,
			     char *const[]);

#endif
