/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The selection of processes that pgrep and pkill share.
 *
 *	pgrep [-acfilnoqvx] [-d delim] [-g pgrp] [-G gid] [-P ppid] [-s sid]
 *	      [-u euid] [-U uid] [pattern ...]
 *	pkill [-signal] [-acfilnovx] [-g pgrp] [-G gid] [-P ppid] [-s sid]
 *	      [-u euid] [-U uid] [pattern ...]
 *
 * A process is selected when it meets every criterion given: one of the
 * patterns (extended regular expressions) matches its name, or with -f its
 * command line, and its value is in each list option.  -v turns the
 * selection round, and -n or -o then keeps the newest or the oldest
 * process only.  The caller itself is never selected, nor are its
 * ancestors unless -a is given, so a shell that runs pkill does not signal
 * itself.  Neither is a process that has ended, nor the kernel.
 *
 * A process's name is the last part of the path it was run by (argv[0]),
 * or the program's name at the front of the title it set, without the '-'
 * a login shell's name begins with: the system keeps no other name of the
 * program a process runs.  It keeps no real user or group either, nor the
 * time a process started: -U and -G compare the effective IDs, and -n and
 * -o the process IDs, which only grow until they wrap round.
 *
 * pgrep writes each selected process's ID (with -l its name or command
 * line too), pkill sends it the signal (TERM by default).  The exit status
 * is 0 when a process was selected, 1 when none was, 2 for a wrong
 * argument and 3 when the processes could not be read.
 */

#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <regex.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "match.h"

/* The most values one list option holds. */
#define PKILL_MAX_LIST		64

/* The longest list item read: a number, or a user's or group's name. */
#define PKILL_ITEM_MAX		64

/* The longest signal name read, with its SIG prefix. */
#define PKILL_SIGNAL_NAME_MAX	32

/*
 * The states of struct process_info in which a process has ended: exiting,
 * a zombie, or dead.  They are the kernel's own numbers, which ps also reads.
 */
#define PKILL_STATE_EXITING	3U
#define PKILL_STATE_ZOMBIE	4U
#define PKILL_STATE_DEAD	5U

/* The lists that take names besides numbers: none, user names, group names. */
#define PKILL_NAMES_NONE	0
#define PKILL_NAMES_USER	1
#define PKILL_NAMES_GROUP	2

/*
 * The values one list option names, and whether the option was given.  A
 * process meets the option when its value is one of them.
 */
struct pkill_list {
	long values[PKILL_MAX_LIST];
	size_t count;
	int given;
};

/*
 * One signal name.
 *
 * The table below is constant for the life of the program.
 */
struct pkill_signal_name {
	const char *name;
	int number;
};

/*
 * What the arguments ask for: the program, the signal pkill sends, how the
 * output is written, the patterns and the list options.
 */
struct pkill_request {
	enum pkill_program program;
	const char *name;
	int signal_number;
	int ancestors;
	int count_only;
	const char *delimiter;
	int full;
	int ignore_case;
	int long_output;
	int newest;
	int oldest;
	int quiet;
	int inverse;
	int exact;
	struct pkill_list groups;
	struct pkill_list real_groups;
	struct pkill_list parents;
	struct pkill_list sessions;
	struct pkill_list effective_users;
	struct pkill_list real_users;
	regex_t *patterns;
	size_t pattern_count;
};

/* The names of the signals pkill takes, as kill takes them. */
static const struct pkill_signal_name pkill_signal_names[] = {
	{ "HUP", SIGHUP },
	{ "INT", SIGINT },
	{ "QUIT", SIGQUIT },
	{ "ILL", SIGILL },
	{ "TRAP", SIGTRAP },
	{ "ABRT", SIGABRT },
	{ "BUS", SIGBUS },
	{ "FPE", SIGFPE },
	{ "KILL", SIGKILL },
	{ "USR1", SIGUSR1 },
	{ "SEGV", SIGSEGV },
	{ "USR2", SIGUSR2 },
	{ "PIPE", SIGPIPE },
	{ "ALRM", SIGALRM },
	{ "TERM", SIGTERM },
	{ "CHLD", SIGCHLD },
	{ "CONT", SIGCONT },
	{ "STOP", SIGSTOP },
	{ "TSTP", SIGTSTP },
	{ "TTIN", SIGTTIN },
	{ "TTOU", SIGTTOU },
	{ "URG", SIGURG },
	{ "XCPU", SIGXCPU },
	{ "XFSZ", SIGXFSZ },
	{ "VTALRM", SIGVTALRM },
	{ "PROF", SIGPROF },
	{ "WINCH", SIGWINCH },
	{ "SYS", SIGSYS },
	{ "IO", SIGIO },
	{ "POLL", SIGPOLL },
};

static int pkill_leading_signal(int argc, char **argv, struct pkill_request *request);
static int pkill_read_signal(const char *text, int *number);
static int pkill_parse_options(int argc, char **argv, struct pkill_request *request);
static int pkill_compile_patterns(int count, char **texts, struct pkill_request *request);
static void pkill_free_patterns(struct pkill_request *request);
static int pkill_parse_list(const char *argument, struct pkill_list *list, int names, const char *program);
static int pkill_list_value(const char *item, int names, long *value);
static int pkill_list_has(const struct pkill_list *list, long value);
static void pkill_list_own(struct pkill_list *list, long own);
static void pkill_exclude(const struct pkill_request *request, const struct pkill_process *processes, size_t count, unsigned char *excluded);
static long pkill_find(const struct pkill_process *processes, size_t count, long pid);
static int pkill_ended(const struct pkill_process *process);
static int pkill_matches(const struct pkill_request *request, const struct pkill_process *process);
static int pkill_lists_match(const struct pkill_request *request, const struct pkill_process *process);
static int pkill_pattern_matches(const struct pkill_request *request, const char *text);
static void pkill_name(const struct pkill_process *process, char *buffer, size_t size);
static const char *pkill_text(const struct pkill_request *request, const struct pkill_process *process, char *buffer, size_t size);
static void pkill_keep_one(const struct pkill_request *request, const struct pkill_process *processes, size_t count, unsigned char *selected);
static size_t pkill_write(const struct pkill_request *request, const struct pkill_process *processes, size_t count, const unsigned char *selected);
static size_t pkill_signal(const struct pkill_request *request, const struct pkill_process *processes, size_t count, const unsigned char *selected);
static void pkill_usage(const struct pkill_request *request);

/*
 * Runs pgrep or pkill.
 */
int
pkill_run(
	int argc,
	char **argv,
	enum pkill_program program)
{
	static struct pkill_request request;
	struct pkill_process *processes;
	unsigned char *excluded;
	unsigned char *selected;
	size_t count;
	size_t index;
	size_t done;
	int consumed;
	int matched;
	int error;

	/* Starts from the defaults: TERM, one process ID to a line. */
	memset(&request, 0, sizeof(request));
	request.program = program;
	request.signal_number = SIGTERM;
	request.delimiter = "\n";

	/* Names the program in its messages. */
	request.name = "pgrep";
	if (program == PKILL_PROGRAM_PKILL)
		request.name = "pkill";

	/* Takes pkill's signal, which comes before the options. */
	consumed = 0;
	if (program == PKILL_PROGRAM_PKILL) {
		consumed = pkill_leading_signal(argc, argv, &request);
		if (consumed < 0) {
			pkill_usage(&request);
			return PKILL_STATUS_USAGE;
		}
	}

	/* Reads the options; the patterns follow them. */
	error = pkill_parse_options(argc - consumed, argv + consumed, &request);
	if (error != 0) {
		pkill_usage(&request);
		return PKILL_STATUS_USAGE;
	}

	/* Compiles the patterns. */
	error = pkill_compile_patterns(argc - consumed - optind, argv + consumed + optind, &request);
	if (error != 0)
		return error;

	/* Refuses a run with nothing to select by. */
	if (request.pattern_count == 0 &&
	    !request.groups.given &&
	    !request.real_groups.given &&
	    !request.parents.given &&
	    !request.sessions.given &&
	    !request.effective_users.given &&
	    !request.real_users.given) {
		fprintf(stderr, "%s: no matching criteria specified\n", request.name);
		pkill_usage(&request);
		return PKILL_STATUS_USAGE;
	}

	/* Takes the snapshot of the processes. */
	error = pkill_system_snapshot(&processes, &count);
	if (error != 0) {
		fprintf(stderr, "%s: process snapshot: %s\n", request.name, strerror(error));
		pkill_free_patterns(&request);
		return PKILL_STATUS_ERROR;
	}

	/* Allocates the marks of the excluded processes. */
	excluded = calloc(count + 1, 1);
	if (excluded == NULL) {
		fprintf(stderr, "%s: out of memory\n", request.name);
		free(processes);
		pkill_free_patterns(&request);
		return PKILL_STATUS_ERROR;
	}

	/* Allocates the marks of the selected processes. */
	selected = calloc(count + 1, 1);
	if (selected == NULL) {
		fprintf(stderr, "%s: out of memory\n", request.name);
		free(excluded);
		free(processes);
		pkill_free_patterns(&request);
		return PKILL_STATUS_ERROR;
	}

	/* Leaves out the caller, its ancestors, the kernel and the ended processes. */
	pkill_exclude(&request, processes, count, excluded);

	/* Selects each process the criteria choose, turned round by -v. */
	for (index = 0; index < count; index++) {
		if (excluded[index])
			continue;

		/* Tests the process against the criteria. */
		matched = pkill_matches(&request, &processes[index]);

		/* -v selects the processes that do not meet them. */
		if (request.inverse)
			matched = !matched;

		/* Records the choice. */
		selected[index] = (unsigned char)matched;
	}

	/* Keeps only the newest or the oldest one for -n or -o. */
	if (request.newest || request.oldest)
		pkill_keep_one(&request, processes, count, selected);

	/* Writes or signals the selected processes. */
	if (program == PKILL_PROGRAM_PKILL) {
		done = pkill_signal(&request, processes, count, selected);
	} else {
		done = pkill_write(&request, processes, count, selected);
	}

	/* Writes the count for -c. */
	if (request.count_only && !request.quiet)
		printf("%lu\n", (unsigned long)done);

	/* Releases the snapshot and the patterns. */
	free(selected);
	free(excluded);
	free(processes);
	pkill_free_patterns(&request);

	/* Reports a failed write of the output. */
	error = fflush(stdout);
	if (error != 0) {
		fprintf(stderr, "%s: write error\n", request.name);
		return PKILL_STATUS_ERROR;
	}

	/* Reports that nothing was selected. */
	if (done == 0)
		return PKILL_STATUS_NO_MATCH;

	/* Succeeded: at least one process was written or signalled. */
	return PKILL_STATUS_MATCH;
}

/*
 * Reads pkill's signal from its first argument: -name (with or without
 * SIG, in any case) or -number.  Returns 1 when the argument was the
 * signal, 0 when it is something else (an option or a pattern), or -1 for
 * a number that is not a signal.
 */
static int
pkill_leading_signal(
	int argc,
	char **argv,
	struct pkill_request *request)
{
	const char *text;
	int number;
	int error;

	/* Only a first argument that begins with '-' can be the signal. */
	if (argc < 2)
		return 0;
	if (argv[1][0] != '-')
		return 0;

	/* The signal follows the '-'. */
	text = argv[1] + 1;

	/* A number must be a signal. */
	if (text[0] >= '0' && text[0] <= '9') {
		error = pkill_read_signal(text, &number);
		if (error != 0) {
			fprintf(stderr, "%s: unknown signal: '%s'\n", request->name, text);
			return -1;
		}

		/* The number is the signal. */
		request->signal_number = number;
		return 1;
	}

	/* A name that is not a signal's is left for the options. */
	error = pkill_read_signal(text, &number);
	if (error != 0)
		return 0;

	/* Succeeded: the name is the signal. */
	request->signal_number = number;
	return 1;
}

/*
 * Reads a signal: a decimal number below NSIG, or a name in any case,
 * with or without SIG.  Returns 0 with the number, or -1.
 */
static int
pkill_read_signal(
	const char *text,
	int *number)
{
	char upper[PKILL_SIGNAL_NAME_MAX];
	const char *name;
	char *end;
	long value;
	size_t length;
	size_t index;
	size_t count;
	int compare;

	/* A number is taken as it is. */
	if (text[0] >= '0' && text[0] <= '9') {
		errno = 0;
		value = strtol(text, &end, 10);
		if (*end != '\0' || errno != 0 || value < 0 || value >= NSIG)
			return -1;
		*number = (int)value;
		return 0;
	}

	/* A name is compared in upper case. */
	length = strlen(text);
	if (length == 0 || length >= sizeof(upper))
		return -1;
	for (index = 0; index <= length; index++) {
		upper[index] = text[index];
		if (upper[index] >= 'a' && upper[index] <= 'z')
			upper[index] = (char)(upper[index] - 'a' + 'A');
	}

	/* The SIG prefix may be there or not. */
	name = upper;
	compare = strncmp(name, "SIG", 3);
	if (compare == 0 && name[3] != '\0')
		name += 3;

	/* Looks the name up. */
	count = sizeof(pkill_signal_names) / sizeof(pkill_signal_names[0]);
	for (index = 0; index < count; index++) {
		compare = strcmp(name, pkill_signal_names[index].name);
		if (compare == 0) {
			*number = pkill_signal_names[index].number;
			return 0;
		}
	}

	/* Not a signal's name. */
	return -1;
}

/*
 * Reads the options into a request.  optind is left at the first pattern.
 * Returns 0, or -1 for an option that cannot be taken.
 */
static int
pkill_parse_options(
	int argc,
	char **argv,
	struct pkill_request *request)
{
	const char *options;
	int option;
	int error;

	/* pgrep alone writes with a delimiter or quietly. */
	options = "acfg:G:ilnoP:s:u:U:vx";
	if (request->program == PKILL_PROGRAM_PGREP)
		options = "acd:fg:G:ilnoP:qs:u:U:vx";

	/* Reads each option; the first one that cannot be taken stops the command. */
	optind = 1;
	for (;;) {
		option = getopt(argc, argv, options);
		if (option == -1)
			break;

		/* Applies the option. */
		error = 0;
		switch (option) {
		case 'a':
			request->ancestors = 1;
			break;
		case 'c':
			request->count_only = 1;
			break;
		case 'd':
			request->delimiter = optarg;
			break;
		case 'f':
			request->full = 1;
			break;
		case 'g':
			error = pkill_parse_list(optarg, &request->groups, PKILL_NAMES_NONE, request->name);
			break;
		case 'G':
			error = pkill_parse_list(optarg, &request->real_groups, PKILL_NAMES_GROUP, request->name);
			break;
		case 'i':
			request->ignore_case = 1;
			break;
		case 'l':
			request->long_output = 1;
			break;
		case 'n':
			request->newest = 1;
			break;
		case 'o':
			request->oldest = 1;
			break;
		case 'P':
			error = pkill_parse_list(optarg, &request->parents, PKILL_NAMES_NONE, request->name);
			break;
		case 'q':
			request->quiet = 1;
			break;
		case 's':
			error = pkill_parse_list(optarg, &request->sessions, PKILL_NAMES_NONE, request->name);
			break;
		case 'u':
			error = pkill_parse_list(optarg, &request->effective_users, PKILL_NAMES_USER, request->name);
			break;
		case 'U':
			error = pkill_parse_list(optarg, &request->real_users, PKILL_NAMES_USER, request->name);
			break;
		case 'v':
			request->inverse = 1;
			break;
		case 'x':
			request->exact = 1;
			break;
		default:
			error = -1;
			break;
		}

		/* Stops at an option that could not be taken. */
		if (error != 0)
			return -1;
	}

	/* Refuses -n with -o: only one process can be kept. */
	if (request->newest && request->oldest) {
		fprintf(stderr, "%s: -n and -o are mutually exclusive\n", request->name);
		return -1;
	}

	/* -g 0 and -s 0 stand for the caller's own process group and session. */
	pkill_list_own(&request->groups, (long)getpgrp());
	pkill_list_own(&request->sessions, (long)getsid(0));

	/* Succeeded: the options are read. */
	return 0;
}

/*
 * Compiles each pattern as an extended regular expression.  Returns 0, or
 * the exit status of a pattern that does not compile or of memory that
 * cannot be had.
 */
static int
pkill_compile_patterns(
	int count,
	char **texts,
	struct pkill_request *request)
{
	char message[256];
	int flags;
	int index;
	int error;

	/* No pattern: the list options alone select. */
	if (count <= 0)
		return 0;

	/* Allocates the compiled patterns. */
	request->patterns = calloc((size_t)count, sizeof(*request->patterns));
	if (request->patterns == NULL) {
		fprintf(stderr, "%s: out of memory\n", request->name);
		return PKILL_STATUS_ERROR;
	}

	/* -x needs where the match ends; otherwise only whether there is one. */
	flags = REG_EXTENDED;
	if (!request->exact)
		flags |= REG_NOSUB;

	/* -i compares without regard to case. */
	if (request->ignore_case)
		flags |= REG_ICASE;

	/* Compiles each pattern. */
	for (index = 0; index < count; index++) {
		error = regcomp(&request->patterns[index], texts[index], flags);
		if (error != 0) {
			(void)regerror(error, &request->patterns[index], message, sizeof(message));
			fprintf(stderr, "%s: cannot compile regular expression '%s': %s\n", request->name, texts[index], message);
			pkill_free_patterns(request);
			return PKILL_STATUS_USAGE;
		}

		/* The pattern counts once it is compiled, so it is freed with the others. */
		request->pattern_count++;
	}

	/* Succeeded: every pattern is compiled. */
	return 0;
}

/* Frees the compiled patterns. */
static void
pkill_free_patterns(
	struct pkill_request *request)
{
	size_t index;

	/* Frees each compiled pattern, then their array. */
	for (index = 0; index < request->pattern_count; index++)
		regfree(&request->patterns[index]);
	free(request->patterns);
	request->patterns = NULL;
	request->pattern_count = 0;
}

/*
 * Reads one list option's argument (items separated by commas or blanks;
 * for a user or group list names too) into a list, after what an earlier
 * option of the same letter gave.  Returns 0, or -1 for an empty list, an
 * item that is not a number nor a known name, or too many items.
 */
static int
pkill_parse_list(
	const char *argument,
	struct pkill_list *list,
	int names,
	const char *program)
{
	char item[PKILL_ITEM_MAX];
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
		if (length >= sizeof(item) || list->count == PKILL_MAX_LIST) {
			fprintf(stderr, "%s: list item too long, or too many items\n", program);
			return -1;
		}

		/* Reads the item as a number or a name. */
		memcpy(item, start, length);
		item[length] = '\0';
		error = pkill_list_value(item, names, &value);
		if (error != 0) {
			fprintf(stderr, "%s: invalid list item '%s'\n", program, item);
			return -1;
		}

		/* Keeps it. */
		list->values[list->count] = value;
		list->count++;
		taken++;
	}

	/* Refuses a list with no item. */
	if (taken == 0) {
		fprintf(stderr, "%s: empty list\n", program);
		return -1;
	}

	/* Succeeded: the option selects the listed values. */
	list->given = 1;
	return 0;
}

/*
 * Reads one list item: a decimal number, or for a user or group list the
 * name of a user or group.  Returns 0 with its value, or -1.
 */
static int
pkill_list_value(
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
	if (names == PKILL_NAMES_USER) {
		user = getpwnam(item);
		if (user == NULL)
			return -1;
		*value = (long)user->pw_uid;
		return 0;
	}

	/* A group's name, for a group list. */
	if (names == PKILL_NAMES_GROUP) {
		group = getgrnam(item);
		if (group == NULL)
			return -1;
		*value = (long)group->gr_gid;
		return 0;
	}

	/* Refuses anything else. */
	return -1;
}

/* Reports whether a list has a value. */
static int
pkill_list_has(
	const struct pkill_list *list,
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

/* Replaces each 0 of a list with the caller's own value. */
static void
pkill_list_own(
	struct pkill_list *list,
	long own)
{
	size_t index;

	/* Looks at each value of the list. */
	for (index = 0; index < list->count; index++) {
		if (list->values[index] == 0)
			list->values[index] = own;
	}
}

/*
 * Marks the processes that are never selected: the caller, its ancestors
 * unless -a is given, the kernel (no process ID above 0, which kill would
 * read as the caller's own process group) and the processes that have
 * ended.
 */
static void
pkill_exclude(
	const struct pkill_request *request,
	const struct pkill_process *processes,
	size_t count,
	unsigned char *excluded)
{
	size_t index;
	size_t steps;
	long position;
	long pid;
	int ended;

	/* Marks the kernel and the ended processes. */
	for (index = 0; index < count; index++) {
		/* The kernel's records, which kill could not address alone. */
		if (processes[index].info.pid <= 0) {
			excluded[index] = 1;
			continue;
		}

		/* A process that has ended can take no signal. */
		ended = pkill_ended(&processes[index]);
		if (ended)
			excluded[index] = 1;
	}

	/* Marks the caller itself. */
	position = pkill_find(processes, count, (long)getpid());
	if (position >= 0)
		excluded[position] = 1;

	/* With -a, the ancestors may be selected. */
	if (request->ancestors)
		return;

	/* Follows the parents up from the caller's, at most once round the snapshot. */
	pid = (long)getppid();
	for (steps = 0; steps < count; steps++) {
		/* The kernel is the parent of the first processes. */
		if (pid <= 0)
			break;

		/* An ancestor that is not in the snapshot ends the line. */
		position = pkill_find(processes, count, pid);
		if (position < 0)
			break;

		/* Leaves the ancestor out and goes on to its parent. */
		excluded[position] = 1;
		pid = (long)processes[position].info.ppid;
	}
}

/* Gives the position of a process in the snapshot, or -1. */
static long
pkill_find(
	const struct pkill_process *processes,
	size_t count,
	long pid)
{
	size_t index;

	/* Looks at each process of the snapshot. */
	for (index = 0; index < count; index++) {
		if ((long)processes[index].info.pid == pid)
			return (long)index;
	}

	/* The process is not in the snapshot. */
	return -1;
}

/* Reports whether a process has ended (exiting, a zombie, or dead). */
static int
pkill_ended(
	const struct pkill_process *process)
{
	unsigned state;

	/* The process's state. */
	state = process->info.state;

	/* Each of the states of an ended process. */
	if (state == PKILL_STATE_EXITING)
		return 1;
	if (state == PKILL_STATE_ZOMBIE)
		return 1;
	if (state == PKILL_STATE_DEAD)
		return 1;

	/* Succeeded: the process still runs. */
	return 0;
}

/*
 * Reports whether a process meets every criterion: one of the patterns,
 * and each list option given.
 */
static int
pkill_matches(
	const struct pkill_request *request,
	const struct pkill_process *process)
{
	char buffer[KERN_SYSTEM_PROCESS_ARGUMENTS_MAX];
	const char *text;
	int listed;
	int matched;

	/* Each list option given must name the process's value. */
	listed = pkill_lists_match(request, process);
	if (!listed)
		return 0;

	/* Without a pattern, the list options decide. */
	if (request->pattern_count == 0)
		return 1;

	/* A process with no name or line known (the kernel's, another user's) matches no pattern. */
	text = pkill_text(request, process, buffer, sizeof(buffer));
	if (text[0] == '\0')
		return 0;

	/* One of the patterns must match the name or the line. */
	matched = pkill_pattern_matches(request, text);
	if (!matched)
		return 0;

	/* Succeeded: the process meets every criterion. */
	return 1;
}

/* Reports whether a process's values are in each list option given. */
static int
pkill_lists_match(
	const struct pkill_request *request,
	const struct pkill_process *process)
{
	const struct process_info *info;
	int listed;

	/* The values the system gives. */
	info = &process->info;

	/* The process group (-g). */
	if (request->groups.given) {
		listed = pkill_list_has(&request->groups, (long)info->process_group);
		if (!listed)
			return 0;
	}

	/* The group (-G), the effective one: the system keeps no other. */
	if (request->real_groups.given) {
		listed = pkill_list_has(&request->real_groups, (long)info->gid);
		if (!listed)
			return 0;
	}

	/* The parent (-P). */
	if (request->parents.given) {
		listed = pkill_list_has(&request->parents, (long)info->ppid);
		if (!listed)
			return 0;
	}

	/* The session (-s). */
	if (request->sessions.given) {
		listed = pkill_list_has(&request->sessions, (long)info->session);
		if (!listed)
			return 0;
	}

	/* The effective user (-u). */
	if (request->effective_users.given) {
		listed = pkill_list_has(&request->effective_users, (long)info->uid);
		if (!listed)
			return 0;
	}

	/* The user (-U), the effective one: the system keeps no other. */
	if (request->real_users.given) {
		listed = pkill_list_has(&request->real_users, (long)info->uid);
		if (!listed)
			return 0;
	}

	/* Succeeded: every list option given names the process. */
	return 1;
}

/*
 * Reports whether one of the patterns matches a text; with -x the match
 * must cover the whole text.
 */
static int
pkill_pattern_matches(
	const struct pkill_request *request,
	const char *text)
{
	regmatch_t match;
	size_t length;
	size_t index;
	int status;

	/* The length a whole match covers. */
	length = strlen(text);

	/* Tries each pattern. */
	for (index = 0; index < request->pattern_count; index++) {
		status = regexec(&request->patterns[index], text, 1, &match, 0);
		if (status != 0)
			continue;

		/* Without -x, a match anywhere selects. */
		if (!request->exact)
			return 1;

		/* With -x, only a match from the first byte to the last. */
		if (match.rm_so == 0 && (size_t)match.rm_eo == length)
			return 1;
	}

	/* No pattern matches. */
	return 0;
}

/*
 * Writes a process's name: its command (argv[0], or the title it set,
 * which begins with the program's name and ": ") up to the title's ": ",
 * after the last '/', without the '-' a login shell's name begins with.
 * The name is empty when the system gives no command.
 */
static void
pkill_name(
	const struct pkill_process *process,
	char *buffer,
	size_t size)
{
	const char *command;
	const char *title;
	const char *slash;
	const char *start;
	size_t length;

	/* The command, as far as a title's ": ". */
	command = process->info.command;
	length = strlen(command);
	title = strstr(command, ": ");
	if (title != NULL)
		length = (size_t)(title - command);

	/* The part after the last '/' within it. */
	start = command;
	for (slash = command; slash < command + length; slash++) {
		if (*slash == '/')
			start = slash + 1;
	}

	/* Shortens the length to that part. */
	length -= (size_t)(start - command);

	/* A login shell's name begins with '-', which is not part of the program's. */
	if (length > 1 && start[0] == '-') {
		start++;
		length--;
	}

	/* Cuts a name that would not fit the buffer. */
	if (length >= size)
		length = size - 1;

	/* Writes the name. */
	memcpy(buffer, start, length);
	buffer[length] = '\0';
}

/*
 * Gives the text a process's patterns are matched against and -l writes:
 * with -f its command line (its command when the system gives no line),
 * otherwise its name.
 */
static const char *
pkill_text(
	const struct pkill_request *request,
	const struct pkill_process *process,
	char *buffer,
	size_t size)
{
	/* The name, without -f. */
	if (!request->full) {
		pkill_name(process, buffer, size);
		return buffer;
	}

	/* The command line, when the system gives one. */
	if (process->arguments[0] != '\0')
		return process->arguments;

	/* The command (empty when the system gives none). */
	return process->info.command;
}

/*
 * Keeps only one of the selected processes for -n or -o: the one with the
 * highest or the lowest process ID, since the system keeps no time a
 * process started and its process IDs only grow until they wrap round.
 */
static void
pkill_keep_one(
	const struct pkill_request *request,
	const struct pkill_process *processes,
	size_t count,
	unsigned char *selected)
{
	size_t index;
	size_t kept;
	int found;
	int better;

	/* Finds the newest or the oldest selected process. */
	found = 0;
	kept = 0;
	for (index = 0; index < count; index++) {
		if (!selected[index])
			continue;

		/* The first one found is kept until a better one comes. */
		better = 0;
		if (!found) {
			better = 1;
		} else if (request->newest && processes[index].info.pid > processes[kept].info.pid) {
			better = 1;
		} else if (request->oldest && processes[index].info.pid < processes[kept].info.pid) {
			better = 1;
		}

		/* Keeps the better one. */
		if (better) {
			kept = index;
			found = 1;
		}
	}

	/* Unselects every process but the one kept. */
	for (index = 0; index < count; index++) {
		if (index != kept)
			selected[index] = 0;
	}
}

/*
 * Writes the selected processes for pgrep: each process ID (with -l its
 * name or, with -f, its command line), separated by the delimiter, and a
 * newline after the last.  Returns how many were selected.
 */
static size_t
pkill_write(
	const struct pkill_request *request,
	const struct pkill_process *processes,
	size_t count,
	const unsigned char *selected)
{
	char buffer[KERN_SYSTEM_PROCESS_ARGUMENTS_MAX];
	const char *text;
	size_t index;
	size_t written;
	int listing;

	/* The processes are listed unless -q or -c asks for none. */
	listing = 1;
	if (request->quiet || request->count_only)
		listing = 0;

	/* Writes each selected process. */
	written = 0;
	for (index = 0; index < count; index++) {
		if (!selected[index])
			continue;

		/* Writes the delimiter before every process but the first. */
		if (listing && written != 0)
			fputs(request->delimiter, stdout);

		/* Writes the process ID. */
		if (listing)
			printf("%d", (int)processes[index].info.pid);

		/* Writes the name or the command line after it for -l. */
		if (listing && request->long_output) {
			text = pkill_text(request, &processes[index], buffer, sizeof(buffer));
			if (text[0] != '\0')
				printf(" %s", text);
		}

		/* Counts the process. */
		written++;
	}

	/* Ends the list with a newline. */
	if (listing && written != 0)
		putchar('\n');

	/* Succeeded: the count of the selected processes. */
	return written;
}

/*
 * Sends pkill's signal to the selected processes, with -l writing the kill
 * command of each.  A process that is gone by then is not counted; one
 * that refuses the signal is reported and still counted, as it was
 * selected.  Returns how many were counted.
 */
static size_t
pkill_signal(
	const struct pkill_request *request,
	const struct pkill_process *processes,
	size_t count,
	const unsigned char *selected)
{
	size_t index;
	size_t signalled;
	pid_t pid;
	int error;

	/* Signals each selected process. */
	signalled = 0;
	for (index = 0; index < count; index++) {
		if (!selected[index])
			continue;

		/* Writes the kill command for -l. */
		pid = (pid_t)processes[index].info.pid;
		if (request->long_output)
			printf("kill -%d %d\n", request->signal_number, (int)pid);

		/* Sends the signal; a process that has gone since the snapshot is passed over. */
		error = pkill_system_signal(pid, request->signal_number);
		if (error == ESRCH)
			continue;

		/* Reports a process that refused the signal. */
		if (error != 0)
			fprintf(stderr, "%s: signalling pid %d: %s\n", request->name, (int)pid, strerror(error));

		/* Counts the process. */
		signalled++;
	}

	/* Succeeded: the count of the signalled processes. */
	return signalled;
}

/* Writes the usage. */
static void
pkill_usage(
	const struct pkill_request *request)
{
	/* The options pkill takes. */
	if (request->program == PKILL_PROGRAM_PKILL) {
		fprintf(stderr, "usage: pkill [-signal] [-acfilnovx] [-g pgrp] [-G gid] [-P ppid] [-s sid] "
				"[-u euid] [-U uid] [pattern ...]\n");
		return;
	}

	/* The options pgrep takes. */
	fprintf(stderr, "usage: pgrep [-acfilnoqvx] [-d delim] [-g pgrp] [-G gid] [-P ppid] [-s sid] "
			"[-u euid] [-U uid] [pattern ...]\n");
}
