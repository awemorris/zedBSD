/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The ps program (XCU ps, ws001-p044).
 *
 * It takes a snapshot of the processes from /dev/system
 * (KERN_SYSTEM_GET_PROCESS, and each one's command line with
 * KERN_SYSTEM_GET_PROCESS_ARGUMENTS), selects the ones the options ask for
 * and writes one line for each in the columns of the format: the -o lists,
 * or the default, -f or -l format.  A column is as wide as its header and
 * its widest value; numbers are aligned to the right, text to the left,
 * and the last column is not padded.
 *
 * What the kernel does not give is written as "-": the share of the CPU
 * (pcpu) and the elapsed time (etime), for no start time is kept.  The
 * real user and group (ruser, rgroup, -U, -G) are the effective ones.  The
 * terminal is "tty" for a process with a controlling terminal and "?"
 * otherwise, so -t is not taken.
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
#include <uapi/process.h>
#include <uapi/system.h>

/* The most processes a snapshot holds. */
#define PS_MAX_PROCESSES	256

/* The most columns of a format, and the most values of one selection list. */
#define PS_MAX_COLUMNS		32
#define PS_MAX_SELECTIONS	64

/* The longest header kept (a header runs to the end of its -o argument). */
#define PS_HEADER_MAX		256

/* The selection lists that take names: none, user names, group names. */
#define PS_NAMES_NONE		0
#define PS_NAMES_USER		1
#define PS_NAMES_GROUP		2

/*
 * What a column shows.  The XCU names come first, then the ones this ps
 * keeps from before (uid, gid, sid, state, pri).
 */
enum ps_field_kind {
	PS_FIELD_RUSER,
	PS_FIELD_USER,
	PS_FIELD_RGROUP,
	PS_FIELD_GROUP,
	PS_FIELD_PID,
	PS_FIELD_PPID,
	PS_FIELD_PGID,
	PS_FIELD_PCPU,
	PS_FIELD_VSZ,
	PS_FIELD_NICE,
	PS_FIELD_ETIME,
	PS_FIELD_TIME,
	PS_FIELD_TTY,
	PS_FIELD_COMM,
	PS_FIELD_ARGS,
	PS_FIELD_UID,
	PS_FIELD_GID,
	PS_FIELD_SID,
	PS_FIELD_STATE,
	PS_FIELD_PRIORITY
};

/*
 * One name -o takes: the column it makes, its default header, and whether
 * its values are numbers (aligned to the right).
 */
struct ps_field_definition {
	const char *name;
	enum ps_field_kind kind;
	const char *header;
	int numeric;
};

/*
 * One column of a built-in format: the -o name and the header the format
 * gives it.
 */
struct ps_default_column {
	const char *name;
	const char *header;
};

/*
 * One column of the output: what it shows, its header (empty when the
 * format gave "name="), how its values align, and its width, measured over
 * the header and every value written in it.
 */
struct ps_column {
	enum ps_field_kind kind;
	char header[PS_HEADER_MAX];
	int numeric;
	size_t width;
};

/*
 * One process of the snapshot: what the kernel gives, and its command line
 * (empty when the kernel gives none; the command stands for it then).
 */
struct ps_process {
	struct process_info info;
	char arguments[KERN_SYSTEM_PROCESS_ARGUMENTS_MAX];
};

/*
 * The numbers one selection option lists (process IDs, process groups,
 * user or group IDs, names already turned into IDs), and whether the
 * option was given at all.
 */
struct ps_list {
	long values[PS_MAX_SELECTIONS];
	size_t count;
	int given;
};

/*
 * What the options ask for: the processes to select (each selection
 * option adds the processes it names; none at all selects the caller's
 * own on its terminal) and the columns.
 */
struct ps_request {
	int every;
	int terminals;
	int non_leaders;
	int full;
	int long_form;
	struct ps_list pids;
	struct ps_list groups;
	struct ps_list effective_users;
	struct ps_list real_users;
	struct ps_list real_groups;
	struct ps_column columns[PS_MAX_COLUMNS];
	size_t column_count;
};

/*
 * The names -o takes, with their XCU headers.
 *
 * The table is constant: it only maps a name to a column.
 */
static const struct ps_field_definition ps_fields[] = {
	{ "ruser", PS_FIELD_RUSER, "RUSER", 0 },
	{ "user", PS_FIELD_USER, "USER", 0 },
	{ "rgroup", PS_FIELD_RGROUP, "RGROUP", 0 },
	{ "group", PS_FIELD_GROUP, "GROUP", 0 },
	{ "pid", PS_FIELD_PID, "PID", 1 },
	{ "ppid", PS_FIELD_PPID, "PPID", 1 },
	{ "pgid", PS_FIELD_PGID, "PGID", 1 },
	{ "pcpu", PS_FIELD_PCPU, "%CPU", 1 },
	{ "vsz", PS_FIELD_VSZ, "VSZ", 1 },
	{ "nice", PS_FIELD_NICE, "NI", 1 },
	{ "etime", PS_FIELD_ETIME, "ELAPSED", 1 },
	{ "time", PS_FIELD_TIME, "TIME", 1 },
	{ "tty", PS_FIELD_TTY, "TT", 0 },
	{ "comm", PS_FIELD_COMM, "COMMAND", 0 },
	{ "args", PS_FIELD_ARGS, "COMMAND", 0 },
	{ "uid", PS_FIELD_UID, "UID", 1 },
	{ "gid", PS_FIELD_GID, "GID", 1 },
	{ "sid", PS_FIELD_SID, "SID", 1 },
	{ "state", PS_FIELD_STATE, "S", 0 },
	{ "stat", PS_FIELD_STATE, "S", 0 },
	{ "pri", PS_FIELD_PRIORITY, "PRI", 1 },
	{ "ni", PS_FIELD_NICE, "NI", 1 },
	{ "command", PS_FIELD_ARGS, "COMMAND", 0 }
};

/*
 * The built-in formats: the default one (PID TTY TIME CMD), -f's, -l's and
 * both's.  -f writes the command line, the others the command.  The XSI
 * columns the kernel has nothing for (F, C, STIME, ADDR, WCHAN) are left
 * out.  Each list ends with a NULL name.
 */
static const struct ps_default_column ps_format_default[] = {
	{ "pid", "PID" }, { "tty", "TTY" }, { "time", "TIME" }, { "comm", "CMD" }, { NULL, NULL }
};
static const struct ps_default_column ps_format_full[] = {
	{ "user", "UID" }, { "pid", "PID" }, { "ppid", "PPID" }, { "tty", "TTY" }, { "time", "TIME" }, { "args", "CMD" },
	{ NULL, NULL }
};
static const struct ps_default_column ps_format_long[] = {
	{ "state", "S" }, { "uid", "UID" }, { "pid", "PID" }, { "ppid", "PPID" }, { "pri", "PRI" }, { "nice", "NI" },
	{ "vsz", "SZ" }, { "tty", "TTY" }, { "time", "TIME" }, { "comm", "CMD" }, { NULL, NULL }
};
static const struct ps_default_column ps_format_long_full[] = {
	{ "state", "S" }, { "user", "UID" }, { "pid", "PID" }, { "ppid", "PPID" }, { "pri", "PRI" }, { "nice", "NI" },
	{ "vsz", "SZ" }, { "tty", "TTY" }, { "time", "TIME" }, { "args", "CMD" }, { NULL, NULL }
};

/*
 * The snapshot of the processes, filled once by ps_snapshot().
 *
 * It is file-scope because it is large (each process carries its command
 * line); main() fills and reads it, nothing else does.
 */
static struct ps_process ps_processes[PS_MAX_PROCESSES];

static int ps_parse_options(int argc, char **argv, struct ps_request *request);
static int ps_parse_format(const char *argument, struct ps_request *request);
static int ps_add_column(struct ps_request *request, const char *name, size_t name_length, const char *header);
static int ps_parse_list(const char *argument, struct ps_list *list, int names);
static int ps_list_value(const char *item, int names, long *value);
static int ps_list_has(const struct ps_list *list, long value);
static int ps_snapshot(size_t *count);
static void ps_snapshot_arguments(int descriptor, struct ps_process *process);
static int ps_selected(const struct ps_request *request, const struct ps_process *process, long caller_session);
static void ps_value(const struct ps_column *column, const struct ps_process *process, char *buffer, size_t size);
static void ps_time(unsigned long long seconds, char *buffer, size_t size);
static void ps_print_cell(const struct ps_column *column, const char *text, int last);
static void ps_usage(void);
static char ps_state_name(unsigned state);

/*
 * Runs the ps command.
 */
int
main(
	int argc,
	char **argv)
{
	static struct ps_request request;
	struct ps_process *process;
	char buffer[KERN_SYSTEM_PROCESS_ARGUMENTS_MAX];
	size_t count;
	size_t index;
	size_t column;
	size_t length;
	long caller_session;
	int selected;
	int headers;
	int last;
	int error;

	/* Reads the options and the format. */
	error = ps_parse_options(argc, argv, &request);
	if (error != 0) {
		ps_usage();
		return 1;
	}

	/* Takes the snapshot of the processes. */
	error = ps_snapshot(&count);
	if (error != 0) {
		fprintf(stderr, "ps: process snapshot: %s\n", strerror(error));
		return 1;
	}

	/* The caller's session stands for its terminal when no selection is given. */
	caller_session = (long)getsid(0);

	/* Starts each column as wide as its header; the headers are written unless every one is empty. */
	headers = 0;
	for (column = 0; column < request.column_count; column++) {
		request.columns[column].width = strlen(request.columns[column].header);
		if (request.columns[column].width != 0)
			headers = 1;
	}

	/* Widens each column to the widest value of the selected processes. */
	for (index = 0; index < count; index++) {
		process = &ps_processes[index];
		selected = ps_selected(&request, process, caller_session);
		if (!selected)
			continue;
		for (column = 0; column < request.column_count; column++) {
			ps_value(&request.columns[column], process, buffer, sizeof(buffer));
			length = strlen(buffer);
			if (length > request.columns[column].width)
				request.columns[column].width = length;
		}
	}

	/* Writes the headers. */
	if (headers) {
		for (column = 0; column < request.column_count; column++) {
			last = column + 1 == request.column_count;
			if (column != 0)
				putchar(' ');
			ps_print_cell(&request.columns[column], request.columns[column].header, last);
		}

		/* Ends the header line. */
		putchar('\n');
	}

	/* Writes one line for each selected process. */
	for (index = 0; index < count; index++) {
		process = &ps_processes[index];
		selected = ps_selected(&request, process, caller_session);
		if (!selected)
			continue;
		for (column = 0; column < request.column_count; column++) {
			last = column + 1 == request.column_count;
			if (column != 0)
				putchar(' ');
			ps_value(&request.columns[column], process, buffer, sizeof(buffer));
			ps_print_cell(&request.columns[column], buffer, last);
		}

		/* Ends the process's line. */
		putchar('\n');
	}

	/* Reports a failed write of the output. */
	error = ferror(stdout);
	if (error != 0) {
		fprintf(stderr, "ps: write error\n");
		return 1;
	}

	/* Succeeded: every selected process is written. */
	return 0;
}

/*
 * Reads the options into a request: the selections and the format (the
 * -o lists in order, or the default, -f or -l one).  Returns 0, or -1 for
 * an option, operand, list or field ps does not take.
 */
static int
ps_parse_options(
	int argc,
	char **argv,
	struct ps_request *request)
{
	const struct ps_default_column *format;
	int option;
	int error;

	/* Reads each option; the first one that cannot be taken stops the command. */
	for (;;) {
		option = getopt(argc, argv, "aAdefg:G:ln:o:p:u:U:");
		if (option == -1)
			break;

		/* Applies the option. */
		error = 0;
		switch (option) {
		case 'A':
		case 'e':
			request->every = 1;
			break;
		case 'a':
			request->terminals = 1;
			break;
		case 'd':
			request->non_leaders = 1;
			break;
		case 'f':
			request->full = 1;
			break;
		case 'l':
			request->long_form = 1;
			break;
		case 'n':
			/* The name list is the system's own; there is no other one to read. */
			break;
		case 'o':
			error = ps_parse_format(optarg, request);
			break;
		case 'p':
			error = ps_parse_list(optarg, &request->pids, PS_NAMES_NONE);
			break;
		case 'g':
			error = ps_parse_list(optarg, &request->groups, PS_NAMES_NONE);
			break;
		case 'u':
			error = ps_parse_list(optarg, &request->effective_users, PS_NAMES_USER);
			break;
		case 'U':
			error = ps_parse_list(optarg, &request->real_users, PS_NAMES_USER);
			break;
		case 'G':
			error = ps_parse_list(optarg, &request->real_groups, PS_NAMES_GROUP);
			break;
		default:
			error = -1;
			break;
		}

		/* Stops at an option that could not be taken. */
		if (error != 0)
			return -1;
	}

	/* Refuses an operand: ps takes none. */
	if (optind != argc)
		return -1;

	/* A format given by -o is complete. */
	if (request->column_count != 0)
		return 0;

	/* Chooses the built-in format of -f, -l, both or neither. */
	format = ps_format_default;
	if (request->long_form && request->full) {
		format = ps_format_long_full;
	} else if (request->long_form) {
		format = ps_format_long;
	} else if (request->full) {
		format = ps_format_full;
	}

	/* Makes its columns. */
	for (; format->name != NULL; format++) {
		error = ps_add_column(request, format->name, strlen(format->name), format->header);
		if (error != 0)
			return -1;
	}

	/* Succeeded: the request is complete. */
	return 0;
}

/*
 * Reads one -o argument: names separated by commas or blanks, each giving
 * a column with its own header; "name=header" gives the column that header,
 * which runs to the end of the argument (commas and blanks included), so it
 * is the argument's last column.  Returns 0, or -1 for an unknown name or
 * too many columns.
 */
static int
ps_parse_format(
	const char *argument,
	struct ps_request *request)
{
	const char *cursor;
	const char *start;
	size_t length;
	int error;

	/* Takes each name in turn. */
	cursor = argument;
	while (*cursor != '\0') {
		/* Skips the separators before it. */
		if (*cursor == ',' || *cursor == ' ' || *cursor == '\t') {
			cursor++;
			continue;
		}

		/* Measures the name up to a separator or its '='. */
		start = cursor;
		while (*cursor != '\0' && *cursor != ',' && *cursor != ' ' && *cursor != '\t' && *cursor != '=')
			cursor++;
		length = (size_t)(cursor - start);

		/* A name with '=': its header is the rest of the argument, and the argument ends. */
		if (*cursor == '=') {
			error = ps_add_column(request, start, length, cursor + 1);
			if (error != 0)
				return -1;
			break;
		}

		/* A name alone takes its default header. */
		error = ps_add_column(request, start, length, NULL);
		if (error != 0)
			return -1;
	}

	/* Succeeded: the argument's columns are added. */
	return 0;
}

/*
 * Adds the column a name (of name_length bytes) gives, with a header, or
 * with its default header when header is NULL.  Returns 0, or -1 for an
 * unknown name, a header too long to keep or too many columns.
 */
static int
ps_add_column(
	struct ps_request *request,
	const char *name,
	size_t name_length,
	const char *header)
{
	const struct ps_field_definition *definition;
	struct ps_column *column;
	size_t index;
	size_t count;
	size_t length;
	int differs;

	/* Refuses a column past the most a format holds. */
	if (request->column_count == PS_MAX_COLUMNS) {
		fprintf(stderr, "ps: too many columns\n");
		return -1;
	}

	/* Finds the name among the fields. */
	definition = NULL;
	count = sizeof(ps_fields) / sizeof(ps_fields[0]);
	for (index = 0; index < count; index++) {
		length = strlen(ps_fields[index].name);
		if (length != name_length)
			continue;
		differs = strncmp(ps_fields[index].name, name, name_length);
		if (differs == 0) {
			definition = &ps_fields[index];
			break;
		}
	}

	/* Refuses a name ps does not know. */
	if (definition == NULL) {
		fprintf(stderr, "ps: unknown format field '%.*s'\n", (int)name_length, name);
		return -1;
	}

	/* Takes the header given, or the field's own. */
	if (header == NULL)
		header = definition->header;

	/* Refuses a header too long to keep. */
	length = strlen(header);
	if (length >= PS_HEADER_MAX) {
		fprintf(stderr, "ps: header too long\n");
		return -1;
	}

	/* Adds the column. */
	column = &request->columns[request->column_count];
	column->kind = definition->kind;
	column->numeric = definition->numeric;
	column->width = 0;
	strcpy(column->header, header);
	request->column_count++;

	/* Succeeded: the column is the format's last. */
	return 0;
}

/*
 * Reads one selection list (numbers separated by commas or blanks, or for
 * a user or group list names too) into a list, after what an earlier
 * option of the same letter gave.  Returns 0, or -1 for an empty list, an
 * item that is not a number nor a known name, or too many items.
 */
static int
ps_parse_list(
	const char *argument,
	struct ps_list *list,
	int names)
{
	char item[64];
	const char *cursor;
	const char *start;
	size_t length;
	long value;
	int taken;
	int error;

	/* Takes each item in turn. */
	taken = 0;
	cursor = argument;
	while (*cursor != '\0') {
		/* Skips the separators before it. */
		if (*cursor == ',' || *cursor == ' ' || *cursor == '\t') {
			cursor++;
			continue;
		}

		/* Measures the item up to a separator. */
		start = cursor;
		while (*cursor != '\0' && *cursor != ',' && *cursor != ' ' && *cursor != '\t')
			cursor++;
		length = (size_t)(cursor - start);

		/* Refuses an item too long to be an ID or a name, or one past the most a list holds. */
		if (length >= sizeof(item) || list->count == PS_MAX_SELECTIONS) {
			fprintf(stderr, "ps: list item too long, or too many items\n");
			return -1;
		}

		/* Reads the item as a number or a name. */
		memcpy(item, start, length);
		item[length] = '\0';
		error = ps_list_value(item, names, &value);
		if (error != 0) {
			fprintf(stderr, "ps: invalid list item '%s'\n", item);
			return -1;
		}

		/* Keeps it. */
		list->values[list->count] = value;
		list->count++;
		taken++;
	}

	/* Refuses a list with no item. */
	if (taken == 0) {
		fprintf(stderr, "ps: empty list\n");
		return -1;
	}

	/* Succeeded: the option selects the listed processes. */
	list->given = 1;
	return 0;
}

/*
 * Reads one list item: a decimal number, or for a user or group list the
 * name of a user or group.  Returns 0 with its value, or -1.
 */
static int
ps_list_value(
	const char *item,
	int names,
	long *value)
{
	struct passwd *user;
	struct group *group;
	char *end;
	long number;

	/* A decimal number. */
	errno = 0;
	number = strtol(item, &end, 10);
	if (errno == 0 && end != item && *end == '\0') {
		*value = number;
		return 0;
	}

	/* A user's name, for a user list. */
	if (names == PS_NAMES_USER) {
		user = getpwnam(item);
		if (user == NULL)
			return -1;
		*value = (long)user->pw_uid;
		return 0;
	}

	/* A group's name, for a group list. */
	if (names == PS_NAMES_GROUP) {
		group = getgrnam(item);
		if (group == NULL)
			return -1;
		*value = (long)group->gr_gid;
		return 0;
	}

	/* Refuses anything else. */
	return -1;
}

/* Reports whether a given list has a value. */
static int
ps_list_has(
	const struct ps_list *list,
	long value)
{
	size_t index;

	/* Looks at each value of the list. */
	for (index = 0; index < list->count; index++) {
		if (list->values[index] == value)
			return 1;
	}

	/* Succeeded: the value is not listed. */
	return 0;
}

/*
 * Takes the snapshot of the processes into ps_processes: each process in
 * the order of its ID, with its command line.  Returns 0 with the count, or
 * the error that kept the snapshot from being taken.
 */
static int
ps_snapshot(
	size_t *count)
{
	struct ps_process *process;
	int32_t cursor;
	int descriptor;
	int status;
	int error;

	/* Opens the system's device. */
	descriptor = open("/dev/system", O_RDONLY);
	if (descriptor < 0)
		return errno;

	/* Asks for each process after the one before, until there is none. */
	error = 0;
	cursor = -1;
	*count = 0;
	while (*count < PS_MAX_PROCESSES) {
		process = &ps_processes[*count];
		memset(process, 0, sizeof(*process));
		process->info.pid = cursor;
		status = ioctl(descriptor, KERN_SYSTEM_GET_PROCESS, &process->info);
		if (status != 0) {
			if (errno != ENOENT)
				error = errno;
			break;
		}

		/* Refuses an answer of another version or size. */
		if (process->info.version != KERN_SYSTEM_PROCESS_INFO_VERSION ||
		    process->info.struct_size != sizeof(process->info)) {
			error = EINVAL;
			break;
		}

		/* Keeps the process with its command line. */
		process->info.command[sizeof(process->info.command) - 1] = '\0';
		ps_snapshot_arguments(descriptor, process);
		cursor = process->info.pid;
		(*count)++;
	}

	/* Closes the device. */
	(void)close(descriptor);

	/* Reports why the snapshot could not be taken. */
	if (error != 0)
		return error;

	/* Succeeded: the snapshot holds every process (up to the most it holds). */
	return 0;
}

/*
 * Takes a process's command line (BUG-274): the kernel's line, or nothing
 * when it gives none (a kernel without it, or a process gone or of another
 * identity), when the command stands for it.
 */
static void
ps_snapshot_arguments(
	int descriptor,
	struct ps_process *process)
{
	struct system_process_arguments line;
	int status;

	/* Asks the kernel for the line. */
	memset(&line, 0, sizeof(line));
	line.pid = process->info.pid;
	status = ioctl(descriptor, KERN_SYSTEM_GET_PROCESS_ARGUMENTS, &line);
	if (status != 0)
		return;

	/* Takes only an answer of this version and size. */
	if (line.version != KERN_SYSTEM_PROCESS_ARGUMENTS_VERSION ||
	    line.struct_size != sizeof(line))
		return;

	/* Keeps the line, ended within its field. */
	line.arguments[sizeof(line.arguments) - 1] = '\0';
	memcpy(process->arguments, line.arguments, sizeof(process->arguments));
}

/*
 * Reports whether a process is to be written.  The selection options add
 * up: a process is written when any of them selects it.  With none, the
 * caller's own processes on its terminal are written: those of its
 * effective user in its session (the kernel does not say which terminal a
 * process has, and a terminal's processes share its session).
 */
static int
ps_selected(
	const struct ps_request *request,
	const struct ps_process *process,
	long caller_session)
{
	const struct process_info *info;
	uid_t caller;
	int leader;
	int listed;

	/* The process, and whether it leads its session. */
	info = &process->info;
	leader = info->pid == info->session;

	/* Every process (-A, -e). */
	if (request->every)
		return 1;

	/* Every process with a terminal but the session leaders (-a). */
	if (request->terminals && info->has_controlling_terminal && !leader)
		return 1;

	/* Every process but the session leaders (-d). */
	if (request->non_leaders && !leader)
		return 1;

	/* The processes the lists name (-p, -g, -u, -U, -G). */
	listed = ps_list_has(&request->pids, info->pid);
	if (listed)
		return 1;
	listed = ps_list_has(&request->groups, info->process_group);
	if (listed)
		return 1;
	listed = ps_list_has(&request->effective_users, (long)info->uid);
	if (listed)
		return 1;
	listed = ps_list_has(&request->real_users, (long)info->uid);
	if (listed)
		return 1;
	listed = ps_list_has(&request->real_groups, (long)info->gid);
	if (listed)
		return 1;

	/* With a selection given, nothing else is written. */
	if (request->terminals ||
	    request->non_leaders ||
	    request->pids.given ||
	    request->groups.given ||
	    request->effective_users.given ||
	    request->real_users.given ||
	    request->real_groups.given)
		return 0;

	/* Without one: the caller's own processes in its session. */
	caller = geteuid();
	if ((uid_t)info->uid != caller)
		return 0;
	if (info->session != caller_session)
		return 0;

	/* Succeeded: the process is the caller's on its terminal. */
	return 1;
}

/* Writes a process's value of a column into a buffer. */
static void
ps_value(
	const struct ps_column *column,
	const struct ps_process *process,
	char *buffer,
	size_t size)
{
	const struct process_info *info;
	struct passwd *user;
	struct group *group;

	/* Writes the column's value. */
	info = &process->info;
	switch (column->kind) {
	case PS_FIELD_RUSER:
	case PS_FIELD_USER:
		/* The user's name, or the number of a user without one. */
		user = getpwuid((uid_t)info->uid);
		if (user != NULL) {
			(void)snprintf(buffer, size, "%s", user->pw_name);
		} else {
			(void)snprintf(buffer, size, "%u", (unsigned)info->uid);
		}

		break;
	case PS_FIELD_RGROUP:
	case PS_FIELD_GROUP:
		/* The group's name, or the number of a group without one. */
		group = getgrgid((gid_t)info->gid);
		if (group != NULL) {
			(void)snprintf(buffer, size, "%s", group->gr_name);
		} else {
			(void)snprintf(buffer, size, "%u", (unsigned)info->gid);
		}

		break;
	case PS_FIELD_PID:
		(void)snprintf(buffer, size, "%d", (int)info->pid);
		break;
	case PS_FIELD_PPID:
		(void)snprintf(buffer, size, "%d", (int)info->ppid);
		break;
	case PS_FIELD_PGID:
		(void)snprintf(buffer, size, "%d", (int)info->process_group);
		break;
	case PS_FIELD_SID:
		(void)snprintf(buffer, size, "%d", (int)info->session);
		break;
	case PS_FIELD_UID:
		(void)snprintf(buffer, size, "%u", (unsigned)info->uid);
		break;
	case PS_FIELD_GID:
		(void)snprintf(buffer, size, "%u", (unsigned)info->gid);
		break;
	case PS_FIELD_PCPU:
	case PS_FIELD_ETIME:
		/* The kernel keeps no start time to measure them against. */
		(void)snprintf(buffer, size, "-");
		break;
	case PS_FIELD_VSZ:
		(void)snprintf(buffer, size, "%llu", (unsigned long long)(info->virtual_bytes / 1024U));
		break;
	case PS_FIELD_NICE:
		(void)snprintf(buffer, size, "%d", (int)info->nice_value);
		break;
	case PS_FIELD_PRIORITY:
		(void)snprintf(buffer, size, "%d", 20 + (int)info->nice_value);
		break;
	case PS_FIELD_TIME:
		ps_time(info->cpu_ticks / KERN_PROCESS_TIMES_HZ, buffer, size);
		break;
	case PS_FIELD_TTY:
		/* The kernel says only whether the process has a terminal. */
		if (info->has_controlling_terminal) {
			(void)snprintf(buffer, size, "tty");
		} else {
			(void)snprintf(buffer, size, "?");
		}

		break;
	case PS_FIELD_STATE:
		(void)snprintf(buffer, size, "%c", ps_state_name(info->state));
		break;
	case PS_FIELD_COMM:
		/* The command, argv[0] (the kernel's own processes have none). */
		if (info->command[0] != '\0') {
			(void)snprintf(buffer, size, "%s", info->command);
		} else {
			(void)snprintf(buffer, size, "kernel");
		}

		break;
	case PS_FIELD_ARGS:
		/* The command line, or the command when there is none. */
		if (process->arguments[0] != '\0') {
			(void)snprintf(buffer, size, "%s", process->arguments);
		} else if (info->command[0] != '\0') {
			(void)snprintf(buffer, size, "%s", info->command);
		} else {
			(void)snprintf(buffer, size, "kernel");
		}

		break;
	}
}

/* Writes a time in seconds as XCU's [dd-]hh:mm:ss. */
static void
ps_time(
	unsigned long long seconds,
	char *buffer,
	size_t size)
{
	unsigned long long days;

	/* Days only when there is at least one. */
	days = seconds / 86400U;
	if (days != 0) {
		(void)snprintf(buffer, size, "%llu-%02llu:%02llu:%02llu", days, seconds / 3600U % 24U, seconds / 60U % 60U, seconds % 60U);
	} else {
		(void)snprintf(buffer, size, "%02llu:%02llu:%02llu", seconds / 3600U, seconds / 60U % 60U, seconds % 60U);
	}
}

/*
 * Writes one cell in its column's width: a number to the right, text to
 * the left; the last column's text is not padded.
 */
static void
ps_print_cell(
	const struct ps_column *column,
	const char *text,
	int last)
{
	int width;

	/* The column's width as printf takes it. */
	width = (int)column->width;

	/* Aligns a number to the right, pads text to the left but in the last column. */
	if (column->numeric) {
		printf("%*s", width, text);
	} else if (last) {
		printf("%s", text);
	} else {
		printf("%-*s", width, text);
	}
}

/* Writes the usage. */
static void
ps_usage(
	void)
{
	/* The options ps takes. */
	fprintf(stderr, "usage: ps [-aAdefl] [-g grouplist] [-G grouplist] [-n namelist] [-o format]... "
			"[-p proclist] [-u userlist] [-U userlist]\n");
}

/* Gives the letter of a process's state (R running, T stopped, Z zombie). */
static char
ps_state_name(
	unsigned state)
{
	static const char names[] = "?RTXZX";

	/* A state the table does not have. */
	if (state >= sizeof(names) - 1)
		return '?';

	/* Succeeded: the state's letter. */
	return names[state];
}
