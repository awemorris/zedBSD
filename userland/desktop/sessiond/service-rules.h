/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What a session may ask of the system's services through sessiond
 * (service-rules.c, ws089-p025): the fixed table of the services it may
 * name, the words it may ask, and who may.  Nothing here reads a file or
 * speaks to init, so the host tests run it alone.
 */

#ifndef SESSIOND_SERVICE_RULES_H
#define SESSIOND_SERVICE_RULES_H

#include <sys/types.h>

/* What a SERVICE request asks: its state, to turn the service on, or off. */
#define SESSIOND_SERVICE_STATUS	0U
#define SESSIOND_SERVICE_ON	1U
#define SESSIOND_SERVICE_OFF	2U

/* The longest service name in the table, with its end. */
#define SESSIOND_SERVICE_NAME	16U

/*
 * A SERVICE request as decided: the service's name (one of the table's)
 * and what is asked.
 */
struct sessiond_service_request {
	char name[SESSIOND_SERVICE_NAME];
	unsigned action;
};

int sessiond_service_decide(const char *arguments, uid_t uid, int in_wheel, struct sessiond_service_request *request);

#endif
