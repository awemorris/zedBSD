/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD ps userland command.
 */

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <zedbsd/system.h>

#define PS_MAX_PROCESSES 256
#define PS_MAX_FIELDS 24
#define PS_MAX_SELECTIONS 64

enum field_kind {
	FIELD_PID,
	FIELD_PPID,
	FIELD_PGID,
	FIELD_SID,
	FIELD_UID,
	FIELD_USER,
	FIELD_GID,
	FIELD_GROUP,
	FIELD_STATE,
	FIELD_NICE,
	FIELD_PRIORITY,
	FIELD_TIME,
	FIELD_TTY,
	FIELD_SIZE,
	FIELD_COMMAND,
};

struct output_field {
	enum field_kind kind;
	const char *header;
};

struct selection {
	long values[PS_MAX_SELECTIONS];
	size_t count;
};

static int parse_fields(char *argument, struct output_field *fields, size_t *count);
static int field_definition(const char *name, struct output_field *field);
static void usage(void);
static int parse_selection(const char *text, struct selection *selection);
static int snapshot(int descriptor, struct process_info *processes, size_t *count);
static int selected(long value, const struct selection *selection);
static void print_value(const struct output_field *field, const struct process_info *process);
static const char *uid_name(uid_t uid, char buffer[32]);
static const char *gid_name(gid_t gid, char buffer[32]);
static char state_name(unsigned state);

/*
 * Runs the ps command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	const char *format;
	char copy[128];
	struct process_info *process;
	struct process_info processes[PS_MAX_PROCESSES];
	struct output_field fields[PS_MAX_FIELDS];
	struct selection pids = {{0}, 0}, groups = {{0}, 0}, users = {{0}, 0};
	size_t process_count, field_count, index, field_index;
	int descriptor, option, all, tty_only, full, long_form;

	field_count = 0;
	all = 0;
	tty_only = 0;
	full = 0;
	long_form = 0;

	/* Parse each command-line option. */
	while ((option = getopt(argc, argv, "Aadeflo:p:g:u:")) != -1) {
		/* Dispatch the selected command-line option. */
		switch (option) {
		case 'A':
		case 'e':
			all = 1;
			break;
		case 'a':
			all = 1;
			tty_only = 1;
			break;
		case 'd':
			all = 1;
			break;
		case 'f':
			full = 1;
			break;
		case 'l':
			long_form = 1;
			break;
		case 'o':
			/* Handles a failed parse fields operation. */
			if (parse_fields(optarg, fields, &field_count) != 0) {
				usage();

				/* Reports operation failure. */
				return 2;
			}
			break;
		case 'p':
			/* Handles a failed parse selection operation. */
			if (parse_selection(optarg, &pids) != 0)
				return 2;
			break;
		case 'g':
			/* Handles a failed parse selection operation. */
			if (parse_selection(optarg, &groups) != 0)
				return 2;
			break;
		case 'u':
			/* Handles a failed parse selection operation. */
			if (parse_selection(optarg, &users) != 0)
				return 2;
			break;
		default:
			usage();

			/* Reports operation failure. */
			return 2;
		}
	}

	/* Validates the command-line arguments. */
	if (optind != argc) {
		usage();

		/* Reports operation failure. */
		return 2;
	}

	/* Handles the field count condition. */
	if (field_count == 0) {
				format = long_form ? "state,uid,pid,ppid,pri,ni,vsz,tty,time,comm"
		    : full    ? "user,pid,ppid,tty,time,comm"
			      : "pid,tty,time,comm";

		strcpy(copy, format);
		(void)parse_fields(copy, fields, &field_count);
	}
	descriptor = open("/dev/system", O_RDONLY);

	/* Handles a failed snapshot operation. */
	if (descriptor < 0 ||
	    snapshot(descriptor, processes, &process_count) != 0) {
		fprintf(stderr, "ps: process snapshot: %s\n", strerror(errno));

		/* Checks the file descriptor. */
		if (descriptor >= 0)
			close(descriptor);

		/* Reports operation failure. */
		return 1;
	}
	close(descriptor);

	/* Process each remaining element. */
	for (field_index = 0; field_index < field_count; field_index++)
		printf("%s%s", field_index == 0 ? "" : " ",
		       fields[field_index].header);
	putchar('\n');

	/* Process each remaining element. */
	for (index = 0; index < process_count; index++) {
				process = &processes[index];

		/* Handles a failed selected operation. */
		if (!selected(process->pid, &pids) ||
		    !selected(process->process_group, &groups) ||
		    !selected(process->uid, &users))
			continue;

		/* Handles a failed geteuid operation. */
		if (!all && pids.count == 0 && groups.count == 0 &&
		    users.count == 0 &&
		    (process->uid != geteuid() ||
		     !process->has_controlling_terminal))
			continue;

		/* Handles the tty only condition. */
		if (tty_only && !process->has_controlling_terminal)
			continue;

		/* Process each remaining element. */
		for (field_index = 0; field_index < field_count;
		     field_index++) {
			/* Handles the field index condition. */
			if (field_index != 0)
				putchar(' ');
			print_value(&fields[field_index], process);
		}
		putchar('\n');
	}

	/* Computes the function result. */
	function_result = ferror(stdout) ? 1 : 0;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the parse fields operation. */
static int
parse_fields(
	char *argument,
	struct output_field *fields,
	size_t *count)
{
	char *equals;
	char *cursor;
	char *item;

	/* Continue while the operation condition remains true. */
	cursor = argument;
	while ((item = strsep(&cursor, ", ")) != NULL) {
		/* Handles the item condition. */
		if (*item == '\0')
			continue;

		/* Checks the remaining item count. */
		if (*count == PS_MAX_FIELDS)
			return -1;
		equals = strchr(item, '=');

		/* Handles the equals availability. */
		if (equals != NULL)
			*equals++ = '\0';
		/* Handles a failed field definition operation. */
		if (field_definition(item, &fields[*count]) != 0)
			return -1;

		/* Handles the equals availability. */
		if (equals != NULL)
			fields[*count].header = equals;
		(*count)++;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the field definition operation. */
static int
field_definition(
	const char *name,
	struct output_field *field)
{
	static const struct {
		const char *name;
		enum field_kind kind;
		const char *header;
	} definitions[] = {
	    {"pid", FIELD_PID, "PID"},
	    {"ppid", FIELD_PPID, "PPID"},
	    {"pgid", FIELD_PGID, "PGID"},
	    {"sid", FIELD_SID, "SID"},
	    {"uid", FIELD_UID, "UID"},
	    {"user", FIELD_USER, "USER"},
	    {"gid", FIELD_GID, "GID"},
	    {"group", FIELD_GROUP, "GROUP"},
	    {"stat", FIELD_STATE, "S"},
	    {"state", FIELD_STATE, "S"},
	    {"ni", FIELD_NICE, "NI"},
	    {"nice", FIELD_NICE, "NI"},
	    {"pri", FIELD_PRIORITY, "PRI"},
	    {"time", FIELD_TIME, "TIME"},
	    {"tty", FIELD_TTY, "TTY"},
	    {"vsz", FIELD_SIZE, "VSZ"},
	    {"comm", FIELD_COMMAND, "COMMAND"},
	    {"args", FIELD_COMMAND, "COMMAND"},
	    {"command", FIELD_COMMAND, "COMMAND"},
	};
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < sizeof(definitions) / sizeof(definitions[0]);
	     index++)

		/* Selects the matching value. */
		if (!strcmp(name, definitions[index].name)) {
			field->kind = definitions[index].kind;
			field->header = definitions[index].header;

			/* Reports successful completion. */
			return 0;
		}

	/* Reports operation failure. */
	return -1;
}

/* Supports the usage operation. */
static void
usage(
	void)
{
	fprintf(stderr, "usage: ps [-Aadefl] [-o format] [-p pidlist] "
			"[-g pgrouplist] [-u uidlist]\n");
}

/* Supports the parse selection operation. */
static int
parse_selection(
	const char *text,
	struct selection *selection)
{
	char *end;
	long value;
	char *copy;
	char *cursor;
	char *item;

	copy = strdup(text);
	cursor = copy;

	/* Handles the copy availability. */
	if (copy == NULL)
		return -1;

	/* Continue while the operation condition remains true. */
	while ((item = strsep(&cursor, ",")) != NULL) {
		/* Handles the item condition. */
		if (*item == '\0' || selection->count == PS_MAX_SELECTIONS) {
			free(copy);

			/* Reports operation failure. */
			return -1;
		}
		errno = 0;
		value = strtol(item, &end, 10);

		/* Handles the reported system error. */
		if (errno != 0 || *end != '\0') {
			free(copy);

			/* Reports operation failure. */
			return -1;
		}
		selection->values[selection->count++] = value;
	}
	free(copy);

	/* Reports successful completion. */
	return 0;
}

/* Supports the snapshot operation. */
static int
snapshot(
	int descriptor,
	struct process_info *processes,
	size_t *count)
{
	struct process_info *process;
	int32_t cursor;

	/* Process each remaining element. */
	cursor = -1;
	*count = 0;
	while (*count < PS_MAX_PROCESSES) {
				process = &processes[*count];
		memset(process, 0, sizeof(*process));
		process->pid = cursor;

		/* Handles a failed ioctl operation. */
		if (ioctl(descriptor, ZEDBSD_SYSTEM_GET_PROCESS, process) !=
		    0) {
			/* Handles the reported system error. */
			if (errno == ENOENT)
				return 0;

			/* Reports operation failure. */
			return -1;
		}

		/* Handles the process condition. */
		if (process->version != ZEDBSD_SYSTEM_PROCESS_INFO_VERSION ||
		    process->struct_size != sizeof(*process)) {
			errno = EINVAL;

			/* Reports operation failure. */
			return -1;
		}
		process->command[sizeof(process->command) - 1] = '\0';
		cursor = process->pid;
		(*count)++;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the selected operation. */
static int
selected(
	long value,
	const struct selection *selection)
{
	size_t index;

	/* Handles the selection condition. */
	if (selection->count == 0)
		return 1;

	/* Process each remaining element. */
	for (index = 0; index < selection->count; index++)

		/* Handles the selection condition. */
		if (selection->values[index] == value)
			return 1;

	/* Reports successful completion. */
	return 0;
}

/* Supports the print value operation. */
static void
print_value(
	const struct output_field *field,
	const struct process_info *process)
{
	char buffer[32];
	unsigned long long seconds;

	/* Dispatch the selected syntax or record type. */
	switch (field->kind) {
	case FIELD_PID:
		printf("%d", process->pid);
		break;
	case FIELD_PPID:
		printf("%d", process->ppid);
		break;
	case FIELD_PGID:
		printf("%d", process->process_group);
		break;
	case FIELD_SID:
		printf("%d", process->session);
		break;
	case FIELD_UID:
		printf("%u", process->uid);
		break;
	case FIELD_USER:
		printf("%s", uid_name(process->uid, buffer));
		break;
	case FIELD_GID:
		printf("%u", process->gid);
		break;
	case FIELD_GROUP:
		printf("%s", gid_name(process->gid, buffer));
		break;
	case FIELD_STATE:
		printf("%c", state_name(process->state));
		break;
	case FIELD_NICE:
		printf("%d", process->nice_value);
		break;
	case FIELD_PRIORITY:
		printf("%d", 20 + process->nice_value);
		break;
	case FIELD_TIME:
		seconds = process->cpu_ticks / 100;
		printf("%02llu:%02llu:%02llu", seconds / 3600,
		       seconds / 60 % 60, seconds % 60);
		break;
	case FIELD_TTY:
		printf("%s", process->has_controlling_terminal ? "tty" : "?");
		break;
	case FIELD_SIZE:
		printf("%llu",
		       (unsigned long long)(process->virtual_bytes / 1024));
		break;
	case FIELD_COMMAND:
		printf("%s", process->command[0] ? process->command : "kernel");
		break;
	}
}

/* Supports the uid name operation. */
static const char *
uid_name(
	uid_t uid,
	char buffer[32])
{
	struct passwd *entry;

	entry = getpwuid(uid);

	/* Handles the entry availability. */
	if (entry != NULL)
		return entry->pw_name;
	(void)snprintf(buffer, 32, "%u", (unsigned)uid);

	/* Returns the computed result. */
	return buffer;
}

/* Supports the gid name operation. */
static const char *
gid_name(
	gid_t gid,
	char buffer[32])
{
	struct group *entry;

	entry = getgrgid(gid);

	/* Handles the entry availability. */
	if (entry != NULL)
		return entry->gr_name;
	(void)snprintf(buffer, 32, "%u", (unsigned)gid);

	/* Returns the computed result. */
	return buffer;
}

/* Supports the state name operation. */
static char
state_name(
	unsigned state)
{
	char function_result;
	static const char names[] = "?RTXZX";

	/* Computes the function result. */
	function_result = state < sizeof(names) - 1 ? names[state] : '?';

	/* Returns the computed result. */
	return function_result;
}
