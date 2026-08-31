/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD logger userland command.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

struct priority_name {
	const char *name;
	int value;
};

static const struct priority_name levels[] = {
    {"emerg", LOG_EMERG}, {"alert", LOG_ALERT},	    {"crit", LOG_CRIT},
    {"err", LOG_ERR},	  {"warning", LOG_WARNING}, {"notice", LOG_NOTICE},
    {"info", LOG_INFO},	  {"debug", LOG_DEBUG},
};
static const struct priority_name facilities[] = {
    {"user", LOG_USER},	    {"daemon", LOG_DAEMON}, {"auth", LOG_AUTH},
    {"syslog", LOG_SYSLOG}, {"cron", LOG_CRON},	    {"local0", LOG_LOCAL0},
    {"local1", LOG_LOCAL1}, {"local2", LOG_LOCAL2}, {"local3", LOG_LOCAL3},
    {"local4", LOG_LOCAL4}, {"local5", LOG_LOCAL5}, {"local6", LOG_LOCAL6},
    {"local7", LOG_LOCAL7},
};

static int parse_priority(const char *text, int *priority);
static int lookup(const struct priority_name *table, size_t count, const char *name, int *value);
static void log_stream(FILE *stream, int priority);

/*
 * Runs the logger command.
 */
int
main(
	int argc,
	char **argv)
{
	size_t length;
	const char *tag, *file;
	int option, priority, flags, index;
	FILE *stream;
	char message[1024];
	size_t used;

	tag = NULL;
	file = NULL;
	priority = LOG_USER | LOG_NOTICE;
	flags = 0;
	used = 0;

	/* Parse each command-line option. */
	while ((option = getopt(argc, argv, "f:ip:st:")) != -1) {
		/* Dispatch the selected command-line option. */
		switch (option) {
		case 'f':
			file = optarg;
			break;
		case 'i':
			flags |= LOG_PID;
			break;
		case 'p':
			/* Handles a failed parse priority operation. */
			if (parse_priority(optarg, &priority) != 0) {
				fprintf(stderr,
					"logger: invalid priority: %s\n",
					optarg);

				/* Reports operation failure. */
				return 2;
			}
			break;
		case 's':
			flags |= LOG_PERROR;
			break;
		case 't':
			tag = optarg;
			break;
		default:
			fprintf(stderr, "usage: logger [-is] [-f file] [-p "
					"priority] [-t tag] [message ...]\n");

			/* Reports operation failure. */
			return 2;
		}
	}

	/* Validates the command-line arguments. */
	if (file != NULL && optind != argc) {
		fprintf(stderr,
			"logger: -f cannot be combined with a message\n");

		/* Reports operation failure. */
		return 2;
	}
	openlog(tag != NULL ? tag : "logger", flags, priority & ~LOG_PRIMASK);

	/* Handles the file availability. */
	if (file != NULL) {
		stream = fopen(file, "r");

		/* Handles the stream availability. */
		if (stream == NULL) {
			fprintf(stderr, "logger: %s: %s\n", file,
				strerror(errno));

			/* Reports operation failure. */
			return 1;
		}
		log_stream(stream, priority);

		/* Handles an operation failure. */
		if (ferror(stream) || fclose(stream) != 0)
			return 1;
	} else if (optind == argc) {
		log_stream(stdin, priority);

		/* Handles an operation failure. */
		if (ferror(stdin))
			return 1;
	} else {
		/* Process each remaining command-line operand. */
		message[0] = '\0';
		for (index = optind; index < argc; index++) {
						length = strlen(argv[index]);

			/* Checks the current capacity usage. */
			if (used + length + (used != 0) + 1 > sizeof(message)) {
				fprintf(stderr,
					"logger: message is too long\n");

				/* Reports operation failure. */
				return 1;
			}

			/* Checks the current capacity usage. */
			if (used != 0)
				message[used++] = ' ';
			memcpy(message + used, argv[index], length + 1);
			used += length;
		}
		syslog(priority, "%s", message);
	}
	closelog();

	/* Reports successful completion. */
	return 0;
}

/* Supports the parse priority operation. */
static int
parse_priority(
	const char *text,
	int *priority)
{
	char copy[64], *dot;
	int facility, level;
	char *end;
	long numeric;

	facility = LOG_USER;

	/* Handles a failed strlen operation. */
	if (strlen(text) >= sizeof(copy))
		return -1;
	strcpy(copy, text);
	dot = strchr(copy, '.');

	/* Handles the dot availability. */
	if (dot != NULL) {
		*dot++ = '\0';
		/* Handles a failed lookup operation. */
		if (lookup(facilities,
			   sizeof(facilities) / sizeof(facilities[0]), copy,
			   &facility) != 0)

			/* Reports operation failure. */
			return -1;
	} else
		dot = copy;

	/* Handles a failed lookup operation. */
	if (lookup(levels, sizeof(levels) / sizeof(levels[0]), dot, &level) ==
	    0) {
		*priority = facility | level;
		/* Reports successful completion. */
		return 0;
	}
	errno = 0;
	numeric = strtol(text, &end, 10);

	/* Handles the reported system error. */
	if (errno != 0 || *end != '\0' || numeric < 0 || numeric > 191)
		return -1;
	*priority = (int)numeric;
	/* Reports successful completion. */
	return 0;
}

/* Supports the lookup operation. */
static int
lookup(
	const struct priority_name *table,
	size_t count,
	const char *name,
	int *value)
{
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < count; index++)

		/* Selects the matching value. */
		if (strcmp(table[index].name, name) == 0) {
			*value = table[index].value;
			/* Reports successful completion. */
			return 0;
		}

	/* Reports operation failure. */
	return -1;
}

/* Supports the log stream operation. */
static void
log_stream(
	FILE *stream,
	int priority)
{
	size_t length;
	char line[1024];

	/* Process input until it is exhausted. */
	while (fgets(line, sizeof(line), stream) != NULL) {

		length = strlen(line);

		/* Checks the current data length. */
		if (length != 0 && line[length - 1] == '\n')
			line[length - 1] = '\0';
		syslog(priority, "%s", line);
	}
}
