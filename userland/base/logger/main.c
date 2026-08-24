/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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

static int
lookup(const struct priority_name *table, size_t count, const char *name,
       int *value)
{
	size_t index;
	for (index = 0; index < count; index++)
		if (strcmp(table[index].name, name) == 0) {
			*value = table[index].value;
			return 0;
		}
	return -1;
}

static int
parse_priority(const char *text, int *priority)
{
	char copy[64], *dot;
	int facility = LOG_USER, level;
	char *end;
	long numeric;

	if (strlen(text) >= sizeof(copy))
		return -1;
	strcpy(copy, text);
	dot = strchr(copy, '.');
	if (dot != NULL) {
		*dot++ = '\0';
		if (lookup(facilities,
			   sizeof(facilities) / sizeof(facilities[0]), copy,
			   &facility) != 0)
			return -1;
	} else
		dot = copy;
	if (lookup(levels, sizeof(levels) / sizeof(levels[0]), dot, &level) ==
	    0) {
		*priority = facility | level;
		return 0;
	}
	errno = 0;
	numeric = strtol(text, &end, 10);
	if (errno != 0 || *end != '\0' || numeric < 0 || numeric > 191)
		return -1;
	*priority = (int)numeric;
	return 0;
}

static void
log_stream(FILE *stream, int priority)
{
	char line[1024];
	while (fgets(line, sizeof(line), stream) != NULL) {
		size_t length = strlen(line);
		if (length != 0 && line[length - 1] == '\n')
			line[length - 1] = '\0';
		syslog(priority, "%s", line);
	}
}

int
main(int argc, char **argv)
{
	const char *tag = NULL, *file = NULL;
	int option, priority = LOG_USER | LOG_NOTICE, flags = 0, index;
	FILE *stream;
	char message[1024];
	size_t used = 0;

	while ((option = getopt(argc, argv, "f:ip:st:")) != -1) {
		switch (option) {
		case 'f':
			file = optarg;
			break;
		case 'i':
			flags |= LOG_PID;
			break;
		case 'p':
			if (parse_priority(optarg, &priority) != 0) {
				fprintf(stderr,
					"logger: invalid priority: %s\n",
					optarg);
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
			return 2;
		}
	}
	if (file != NULL && optind != argc) {
		fprintf(stderr,
			"logger: -f cannot be combined with a message\n");
		return 2;
	}
	openlog(tag != NULL ? tag : "logger", flags, priority & ~LOG_PRIMASK);
	if (file != NULL) {
		stream = fopen(file, "r");
		if (stream == NULL) {
			fprintf(stderr, "logger: %s: %s\n", file,
				strerror(errno));
			return 1;
		}
		log_stream(stream, priority);
		if (ferror(stream) || fclose(stream) != 0)
			return 1;
	} else if (optind == argc) {
		log_stream(stdin, priority);
		if (ferror(stdin))
			return 1;
	} else {
		message[0] = '\0';
		for (index = optind; index < argc; index++) {
			size_t length = strlen(argv[index]);
			if (used + length + (used != 0) + 1 > sizeof(message)) {
				fprintf(stderr,
					"logger: message is too long\n");
				return 1;
			}
			if (used != 0)
				message[used++] = ' ';
			memcpy(message + used, argv[index], length + 1);
			used += length;
		}
		syslog(priority, "%s", message);
	}
	closelog();
	return 0;
}
