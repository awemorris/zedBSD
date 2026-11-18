/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Displays who is on the system (POSIX XCU who).
 *
 *	who [-mTu] [-abdHlprt] [file]
 *	who [-mu] -s [-bHlprt] [file]
 *	who -q [file]
 *	who am i
 *	who am I
 *
 * who reads the user accounting database (utmpx), or the file named, a
 * sequence of struct utmpx records.  By default it writes each logged-in
 * user: name, terminal line and login time ("%b %e %H:%M"), and the host
 * a user came from.  -b, -d, -l, -p, -r, -t and -u select the boot time,
 * dead processes, lines waiting for a login, processes init spawned, the
 * run level, clock changes and users with their idle time and process;
 * -a selects them all with -T.  -T writes the state of each terminal (+
 * writable by others, - not, ? unknown), -H headings, -m (or "am i") the
 * current terminal only, -q the names and their count, and -s the name,
 * line and time only.
 *
 * The columns follow the layout of other systems: name (8), state, line
 * (12), time, idle time (6), process ID (10), comment, exit status.  Users
 * whose process no longer exists are left out of the system database.
 * who exits with 0, or 1 when the database cannot be read.
 */

#include "userland/base/common/command.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utmpx.h>

/* The width of a time written by "%b %e %H:%M". */
#define WHO_TIME_WIDTH 12

/*
 * What to write.
 *
 * The need_ flags select kinds of record; the include_ flags add
 * columns.  short_output keeps the name, line, time and comment only.
 */
struct who_options {
	int need_boot;
	int need_dead;
	int need_login;
	int need_init;
	int need_run_level;
	int need_clock;
	int need_users;
	int include_state;
	int include_idle;
	int include_exit;
	int short_output;
	int heading;
	int mine;
	int quick;
};

/*
 * The records read, in order.
 */
struct who_records {
	struct utmpx *items;
	size_t count;
	size_t capacity;
};

static int read_options(int argc, char **argv, struct who_options *options);
static int load_database(struct who_records *records);
static int load_file(const char *path, struct who_records *records);
static int add_record(struct who_records *records, const struct utmpx *record);
static int is_user(const struct utmpx *record);
static void list_quick(const struct who_records *records);
static void list_entries(const struct who_options *options, const struct who_records *records);
static void print_record(const struct who_options *options, const struct utmpx *record, time_t boot_time);
static void print_user(const struct who_options *options, const struct utmpx *record, time_t boot_time);
static void print_other(const struct who_options *options, const struct utmpx *record, const char *user, const char *line, const char *comment, const char *exit_text);
static void print_line(const struct who_options *options, const char *user, char state, const char *line, const char *time_text, const char *idle, const char *pid, const char *comment, const char *exit_text);
static void format_time(const struct utmpx *record, char *text, size_t size);
static const char *idle_text(time_t when, time_t boot_time, char *text, size_t size);
static void copy_field(char *out, size_t out_size, const char *field, size_t field_size);
static void usage(void);

/*
 * Runs who.
 */
int
main(
	int argc,
	char **argv)
{
	struct who_options options;
	struct who_records records;
	int first;
	int status;

	/* Reads the options and the operands: a file, or "am i". */
	first = read_options(argc, argv, &options);
	memset(&records, 0, sizeof(records));
	if (argc - first == 2) {
		options.mine = 1;
		status = load_database(&records);
	} else if (argc - first == 1) {
		status = load_file(argv[first], &records);
	} else if (argc == first) {
		status = load_database(&records);
	} else {
		usage();
		status = 1;
	}

	/* A database not read ends who. */
	if (status != 0)
		return 1;

	/* Writes what was asked for. */
	if (options.quick)
		list_quick(&records);
	else
		list_entries(&options, &records);
	free(records.items);

	/* A failed write is a failure too. */
	status = fflush(stdout);
	if (status != 0) {
		command_error("who", "standard output");
		return 1;
	}

	/* Succeeded: the entries are written. */
	return 0;
}

/*
 * Reads the options and returns the index of the first operand.
 */
static int
read_options(
	int argc,
	char **argv,
	struct who_options *options)
{
	int option;
	int selected;

	/* Nothing selected yet. */
	memset(options, 0, sizeof(*options));
	selected = 0;
	for (;;) {
		option = getopt(argc, argv, "abdHlmpqrstTu");
		if (option == -1)
			break;

		/* Each selection clears the default of users only. */
		switch (option) {
		case 'a':
			options->need_boot = 1;
			options->need_dead = 1;
			options->need_login = 1;
			options->need_init = 1;
			options->need_run_level = 1;
			options->need_clock = 1;
			options->need_users = 1;
			options->include_state = 1;
			options->include_idle = 1;
			options->include_exit = 1;
			selected = 1;
			break;
		case 'b':
			options->need_boot = 1;
			selected = 1;
			break;
		case 'd':
			options->need_dead = 1;
			options->include_idle = 1;
			options->include_exit = 1;
			selected = 1;
			break;
		case 'H':
			options->heading = 1;
			break;
		case 'l':
			options->need_login = 1;
			options->include_idle = 1;
			selected = 1;
			break;
		case 'm':
			options->mine = 1;
			break;
		case 'p':
			options->need_init = 1;
			selected = 1;
			break;
		case 'q':
			options->quick = 1;
			break;
		case 'r':
			options->need_run_level = 1;
			options->include_idle = 1;
			selected = 1;
			break;
		case 's':
			options->short_output = 1;
			break;
		case 't':
			options->need_clock = 1;
			selected = 1;
			break;
		case 'T':
			options->include_state = 1;
			break;
		case 'u':
			options->need_users = 1;
			options->include_idle = 1;
			selected = 1;
			break;
		default:
			usage();
			break;
		}
	}

	/* Without a selection: the users, in the short form. */
	if (!selected) {
		options->need_users = 1;
		options->short_output = 1;
	}

	/* The dead processes' exit needs the long form. */
	if (options->include_exit)
		options->short_output = 0;
	return optind;
}

/*
 * Reads the system's database, leaving out users whose process is gone.
 */
static int
load_database(
	struct who_records *records)
{
	struct utmpx *record;
	int status;
	int alive;

	/* Reads each record. */
	setutxent();
	for (;;) {
		record = getutxent();
		if (record == NULL)
			break;

		/* A user whose process is gone is no longer on. */
		if (record->ut_type == USER_PROCESS && record->ut_pid > 0) {
			alive = kill(record->ut_pid, 0);
			if (alive != 0 && errno == ESRCH)
				continue;
		}

		/* Keeps the record. */
		status = add_record(records, record);
		if (status != 0) {
			endutxent();
			fprintf(stderr, "who: out of memory\n");
			return -1;
		}
	}

	/* Done with the database. */
	endutxent();
	return 0;
}

/*
 * Reads a file of struct utmpx records; a partial record at the end is
 * left out.
 */
static int
load_file(
	const char *path,
	struct who_records *records)
{
	struct utmpx record;
	size_t have;
	ssize_t count;
	int descriptor;
	int status;

	/* Opens the file. */
	descriptor = open(path, O_RDONLY);
	if (descriptor < 0) {
		command_error("who", path);
		return -1;
	}

	/* Reads whole records. */
	have = 0;
	for (;;) {
		count = read(descriptor, (char *)&record + have, sizeof(record) - have);
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0) {
			command_error("who", path);
			close(descriptor);
			return -1;
		}

		/* The end of the file. */
		if (count == 0)
			break;

		/* A whole record is kept. */
		have += (size_t)count;
		if (have < sizeof(record))
			continue;
		have = 0;
		status = add_record(records, &record);
		if (status != 0) {
			fprintf(stderr, "who: out of memory\n");
			close(descriptor);
			return -1;
		}
	}

	/* Succeeded: the records. */
	close(descriptor);
	return 0;
}

/* Adds a copy of a record. */
static int
add_record(
	struct who_records *records,
	const struct utmpx *record)
{
	struct utmpx *grown;
	size_t capacity;

	/* Grows the array when it is full. */
	if (records->count == records->capacity) {
		capacity = records->capacity * 2;
		if (capacity < 16)
			capacity = 16;
		grown = realloc(records->items, capacity * sizeof(*grown));
		if (grown == NULL)
			return -1;
		records->items = grown;
		records->capacity = capacity;
	}

	/* Copies it. */
	records->items[records->count] = *record;
	records->count++;
	return 0;
}

/* Tells whether a record is a logged-in user. */
static int
is_user(
	const struct utmpx *record)
{
	/* A user process with a name. */
	if (record->ut_type != USER_PROCESS)
		return 0;
	if (record->ut_user[0] == '\0')
		return 0;
	return 1;
}

/* Writes the names of the users and their count (-q). */
static void
list_quick(
	const struct who_records *records)
{
	char name[sizeof(records->items[0].ut_user) + 1];
	const char *separator;
	size_t index;
	size_t users;
	int user;

	/* The names on one line. */
	separator = "";
	users = 0;
	for (index = 0; index < records->count; index++) {
		user = is_user(&records->items[index]);
		if (!user)
			continue;
		copy_field(name, sizeof(name), records->items[index].ut_user, sizeof(records->items[index].ut_user));
		printf("%s%s", separator, name);
		separator = " ";
		users++;
	}

	/* The count. */
	printf("\n# users=%lu\n", (unsigned long)users);
}

/* Writes the records selected, with a heading under -H. */
static void
list_entries(
	const struct who_options *options,
	const struct who_records *records)
{
	const struct utmpx *record;
	char line[sizeof(records->items[0].ut_line) + 1];
	const char *terminal;
	time_t boot_time;
	size_t index;
	int compare;

	/* The heading. */
	if (options->heading)
		print_line(options, "NAME", ' ', "LINE", "TIME", "IDLE", "PID", "COMMENT", "EXIT");

	/* -m keeps the current terminal only; without one there is nothing. */
	terminal = NULL;
	if (options->mine) {
		terminal = ttyname(STDIN_FILENO);
		if (terminal == NULL)
			return;
		compare = strncmp(terminal, "/dev/", 5);
		if (compare == 0)
			terminal += 5;
	}

	/* Each record; idle times are measured from the last boot before it. */
	boot_time = 0;
	for (index = 0; index < records->count; index++) {
		record = &records->items[index];
		if (record->ut_type == BOOT_TIME)
			boot_time = record->ut_tv.tv_sec;

		/* Another terminal's record is left out under -m. */
		if (terminal != NULL) {
			copy_field(line, sizeof(line), record->ut_line, sizeof(record->ut_line));
			compare = strcmp(line, terminal);
			if (compare != 0)
				continue;
		}

		/* Writes it if its kind is selected. */
		print_record(options, record, boot_time);
	}
}

/* Writes a record if its kind is selected. */
static void
print_record(
	const struct who_options *options,
	const struct utmpx *record,
	time_t boot_time)
{
	char line[sizeof(record->ut_line) + 1];
	int user;
#ifdef RUN_LVL
	char level[32];
	char comment[16];
	int last;
#endif

	/* A user. */
	user = is_user(record);
	if (options->need_users && user) {
		print_user(options, record, boot_time);
		return;
	}

#ifdef RUN_LVL
	/* A run level where the system records one: the level and the one before. */
	if (options->need_run_level && record->ut_type == RUN_LVL) {
		snprintf(level, sizeof(level), "run-level %c", record->ut_pid % 256);
		last = record->ut_pid / 256;
		if (last == 'N')
			last = 'S';
		comment[0] = '\0';
		if (last >= ' ' && last < 127)
			snprintf(comment, sizeof(comment), "last=%c", last);
		print_other(options, record, "", level, comment, NULL);
		return;
	}
#endif

	/* The boot and clock changes. */
	if (options->need_boot && record->ut_type == BOOT_TIME) {
		print_other(options, record, "", "system boot", "", NULL);
		return;
	}

	/* A clock change. */
	if (options->need_clock && record->ut_type == NEW_TIME) {
		print_other(options, record, "", "clock change", "", NULL);
		return;
	}

	/* Processes init spawned, lines waiting for a login and processes gone. */
	copy_field(line, sizeof(line), record->ut_line, sizeof(record->ut_line));
	if (options->need_init && record->ut_type == INIT_PROCESS)
		print_other(options, record, "", line, NULL, NULL);
	else if (options->need_login && record->ut_type == LOGIN_PROCESS)
		print_other(options, record, "LOGIN", line, NULL, NULL);
	else if (options->need_dead && record->ut_type == DEAD_PROCESS)
		print_other(options, record, "", line, NULL, "");
}

/* Writes a logged-in user. */
static void
print_user(
	const struct who_options *options,
	const struct utmpx *record,
	time_t boot_time)
{
	struct stat status;
	char user[sizeof(record->ut_user) + 1];
	char line[sizeof(record->ut_line) + 1];
	char host[sizeof(record->ut_host) + 1];
	char path[sizeof(record->ut_line) + 8];
	char time_text[64];
	char idle[16];
	char pid[24];
	char comment[sizeof(record->ut_host) + 3];
	const char *idle_shown;
	char state;
	int result;

	/* The fields of the record. */
	copy_field(user, sizeof(user), record->ut_user, sizeof(record->ut_user));
	copy_field(line, sizeof(line), record->ut_line, sizeof(record->ut_line));
	copy_field(host, sizeof(host), record->ut_host, sizeof(record->ut_host));
	format_time(record, time_text, sizeof(time_text));
	snprintf(pid, sizeof(pid), "%ld", (long)record->ut_pid);
	comment[0] = '\0';
	if (host[0] != '\0')
		snprintf(comment, sizeof(comment), "(%s)", host);

	/* The terminal: writable by others or not, and when it was last used. */
	if (line[0] == '/')
		snprintf(path, sizeof(path), "%s", line);
	else
		snprintf(path, sizeof(path), "/dev/%s", line);
	result = stat(path, &status);
	state = '?';
	idle_shown = "  ?";
	if (result == 0) {
		state = '-';
		if (status.st_mode & S_IWGRP)
			state = '+';
		idle_shown = idle_text(status.st_atime, boot_time, idle, sizeof(idle));
	}

	/* Writes the line. */
	print_line(options, user, state, line, time_text, idle_shown, pid, comment, "");
}

/*
 * Writes a record other than a user: its name and line as given, the
 * comment (NULL: its id), and the exit text (NULL: none).
 */
static void
print_other(
	const struct who_options *options,
	const struct utmpx *record,
	const char *user,
	const char *line,
	const char *comment,
	const char *exit_text)
{
	char time_text[64];
	char identifier[sizeof(record->ut_id) + 1];
	char id_comment[sizeof(record->ut_id) + 8];
	char pid[24];

	/* The time, and the process of all but the boot, clock and run level. */
	format_time(record, time_text, sizeof(time_text));
	pid[0] = '\0';
	if (comment == NULL) {
		snprintf(pid, sizeof(pid), "%ld", (long)record->ut_pid);
		copy_field(identifier, sizeof(identifier), record->ut_id, sizeof(record->ut_id));
		snprintf(id_comment, sizeof(id_comment), "id=%s", identifier);
		comment = id_comment;
	}

	/* No exit text unless given. */
	if (exit_text == NULL)
		exit_text = "";
	print_line(options, user, ' ', line, time_text, "", pid, comment, exit_text);
}

/*
 * Writes one line in the columns selected, without trailing blanks.
 */
static void
print_line(
	const struct who_options *options,
	const char *user,
	char state,
	const char *line,
	const char *time_text,
	const char *idle,
	const char *pid,
	const char *comment,
	const char *exit_text)
{
	char text[512];
	size_t length;
	int used;

	/* Name, state, line and time. */
	used = snprintf(text, sizeof(text), "%-8s", user);
	if (options->include_state)
		used += snprintf(text + used, sizeof(text) - (size_t)used, " %c", state);
	used += snprintf(text + used, sizeof(text) - (size_t)used, " %-12s %-*s", line, WHO_TIME_WIDTH, time_text);

	/* Idle time and process, unless the short form. */
	if (options->include_idle && !options->short_output)
		used += snprintf(text + used, sizeof(text) - (size_t)used, " %-6s", idle);
	if (!options->short_output)
		used += snprintf(text + used, sizeof(text) - (size_t)used, " %10s", pid);

	/* The comment and the exit status. */
	used += snprintf(text + used, sizeof(text) - (size_t)used, " %-8s", comment);
	if (options->include_exit)
		snprintf(text + used, sizeof(text) - (size_t)used, " %-12s", exit_text);

	/* Without the blanks at the end. */
	length = strlen(text);
	while (length > 0 && text[length - 1] == ' ')
		length--;
	text[length] = '\0';
	puts(text);
}

/* Formats the time of a record as "%b %e %H:%M". */
static void
format_time(
	const struct utmpx *record,
	char *text,
	size_t size)
{
	struct tm *broken;
	time_t seconds;
	size_t written;

	/* The local time, or the number of seconds when it has none. */
	seconds = record->ut_tv.tv_sec;
	broken = localtime(&seconds);
	written = 0;
	if (broken != NULL)
		written = strftime(text, size, "%b %e %H:%M", broken);
	if (written == 0)
		snprintf(text, size, "%lld", (long long)seconds);
}

/*
 * Describes how long a terminal has been idle: "  .  " under a minute,
 * hours and minutes under a day, else " old ".
 */
static const char *
idle_text(
	time_t when,
	time_t boot_time,
	char *text,
	size_t size)
{
	time_t now;
	long idle;

	/* Since the terminal was last used, if that was after the boot. */
	now = time(NULL);
	if (when <= boot_time || when > now)
		return " old ";
	idle = (long)(now - when);
	if (idle >= 24L * 60 * 60)
		return " old ";
	if (idle < 60)
		return "  .  ";

	/* Hours and minutes. */
	snprintf(text, size, "%02ld:%02ld", idle / 3600, idle % 3600 / 60);
	return text;
}

/* Copies a field that need not end in a null character. */
static void
copy_field(
	char *out,
	size_t out_size,
	const char *field,
	size_t field_size)
{
	size_t length;

	/* Up to the null character or the end of the field. */
	length = 0;
	while (length < field_size && length + 1 < out_size && field[length] != '\0')
		length++;
	memcpy(out, field, length);
	out[length] = '\0';
}

/* Writes the usage message and exits. */
static void
usage(void)
{
	/* Names the POSIX forms. */
	fprintf(stderr,
		"usage: who [-mTu] [-abdHlprt] [file]\n"
		"       who [-mu] -s [-bHlprt] [file]\n"
		"       who -q [file]\n"
		"       who am i\n");
	exit(1);
}
