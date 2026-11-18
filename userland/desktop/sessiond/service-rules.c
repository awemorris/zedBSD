/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What a session may ask of the system's services through sessiond
 * (ws089-p025: the Sharing page's Remote Login).
 *
 * A request is "SERVICE NAME on|off|status": NAME one of the fixed table
 * (sshd alone for now), the word one of the three, separated by one space,
 * nothing else.  Only root or a member of wheel may ask (the rule sudo
 * keeps, Q1 2026-10-05); status too, as it says how the machine is reached.
 */

#include "service-rules.h"

#include <errno.h>
#include <string.h>

/* The services a session may name. */
static const char *const rules_services[] = { "sshd" };

/*
 * Decides a SERVICE request's arguments (the line after "SERVICE ") for a
 * user (in_wheel: whether the user is a member of wheel).  Returns 0 with
 * the request, EPERM for a user who may not, or EINVAL for arguments that
 * are not one of the table's services and one of the words.
 */
int
sessiond_service_decide(
	const char *arguments,
	uid_t uid,
	int in_wheel,
	struct sessiond_service_request *request)
{
	const char *space;
	const char *word;
	size_t length;
	size_t index;
	int differs;
	int known;

	/* The name, up to the one space. */
	memset(request, 0, sizeof(*request));
	space = strchr(arguments, ' ');
	if (space == NULL)
		return EINVAL;
	length = (size_t)(space - arguments);
	if (length == 0U || length >= sizeof(request->name))
		return EINVAL;

	/* One of the table's services. */
	known = 0;
	for (index = 0; index < sizeof(rules_services) / sizeof(rules_services[0]); index++) {
		differs = strncmp(arguments, rules_services[index], length);
		if (differs == 0 && rules_services[index][length] == '\0')
			known = 1;
	}

	/* Only one of them. */
	if (!known)
		return EINVAL;
	memcpy(request->name, arguments, length);
	request->name[length] = '\0';

	/* One of the words, and nothing after it. */
	word = space + 1;
	differs = strcmp(word, "status");
	if (differs == 0) {
		request->action = SESSIOND_SERVICE_STATUS;
	} else {
		differs = strcmp(word, "on");
		if (differs == 0) {
			request->action = SESSIOND_SERVICE_ON;
		} else {
			differs = strcmp(word, "off");
			if (differs != 0)
				return EINVAL;
			request->action = SESSIOND_SERVICE_OFF;
		}
	}

	/* Root, or a member of wheel. */
	if (uid != 0 && !in_wheel)
		return EPERM;

	/* Succeeded: the request may be carried out. */
	return 0;
}
